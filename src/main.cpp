#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <dinput.h>
#include <d3d9.h>
#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace {

using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
using GetDeviceStateFn = HRESULT (STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT (STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using SetDataFormatFn = HRESULT (STDMETHODCALLTYPE*)(void*, LPCDIDATAFORMAT);

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using D3DCreateDeviceFn = HRESULT (STDMETHODCALLTYPE*)(
    void*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**
);
using D3DPresentFn = HRESULT (STDMETHODCALLTYPE*)(
    void*, const RECT*, const RECT*, HWND, const RGNDATA*
);
using D3DEndSceneFn = HRESULT (STDMETHODCALLTYPE*)(void*);

DirectInput8CreateFn g_originalDirectInput8Create = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
GetDeviceStateFn g_originalGetDeviceState = nullptr;
GetDeviceDataFn g_originalGetDeviceData = nullptr;
SetDataFormatFn g_originalSetDataFormat = nullptr;

D3DCreateDeviceFn g_originalD3DCreateDevice = nullptr;
D3DPresentFn g_originalD3DPresent = nullptr;
D3DEndSceneFn g_originalD3DEndScene = nullptr;
void* g_d3dDevice = nullptr;
thread_local bool g_insideBlurPass = false;

void* g_mouseDevice = nullptr;
volatile LONG g_loggedFirstStateCall = 0;
volatile LONG g_loggedFirstDataCall = 0;
volatile LONG g_loggedStateCalls = 0;
volatile LONG g_loggedFormats = 0;

DWORD g_customDataSize = 0;
DWORD g_customXOffset = 0xFFFFFFFFu;
DWORD g_customYOffset = 0xFFFFFFFFu;

std::mutex g_hookMutex;
std::mutex g_logMutex;
std::ofstream g_log;
std::string g_iniPath;
std::string g_logPath;

bool g_enabled = true;
bool g_logInput = true;
float g_smoothing = 0.0f;
float g_sensitivityMultiplier = 1.0f;
LONG g_spikeClamp = 0;
bool g_stallProtection = true;
double g_stallThresholdMs = 12.0;
double g_stallAlphaScale = 0.50;
bool g_timeBasedSmoothing = true;
double g_smoothingReferenceMs = 4.0;

bool g_motionBlurEnabled = false;
double g_motionBlurStrength = 0.22;
double g_motionBlurHoldMs = 70.0;
double g_motionBlurHistoryMs = 16.0;
double g_motionBlurTrailScale = 1.10;
double g_motionBlurStallBoost = 0.12;
int g_motionBlurSamples = 6;

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_windowStart{};
LARGE_INTEGER g_lastStateCall{};
LARGE_INTEGER g_lastMotionCall{};
LARGE_INTEGER g_lastCustomPoll{};
LARGE_INTEGER g_lastMouseMotion{};
std::uint64_t g_stateCalls = 0;
std::uint64_t g_motionCalls = 0;
double g_stateIntervalSumMs = 0.0;
double g_motionIntervalSumMs = 0.0;
double g_maxStateGapMs = 0.0;
double g_maxMotionGapMs = 0.0;
std::uint64_t g_absX = 0;
std::uint64_t g_absY = 0;
double g_accumX = 0.0;
double g_accumY = 0.0;
volatile LONG g_stallEvents = 0;

LARGE_INTEGER g_frameWindowStart{};
LARGE_INTEGER g_lastPresent{};
std::uint64_t g_frameCalls = 0;
double g_frameIntervalSumMs = 0.0;
double g_maxFrameGapMs = 0.0;
std::uint64_t g_frameOver12 = 0;
std::uint64_t g_frameOver20 = 0;
std::uint64_t g_frameOver33 = 0;
volatile LONG g_frameStallLogs = 0;
double g_lastFrameGapMs = 0.0;

IDirect3DDevice9* g_blurDevice = nullptr;
IDirect3DTexture9* g_blurPrevTexture = nullptr;
IDirect3DTexture9* g_blurCurrTexture = nullptr;
IDirect3DStateBlock9* g_blurStateBlock = nullptr;
UINT g_blurWidth = 0;
UINT g_blurHeight = 0;
D3DFORMAT g_blurFormat = D3DFMT_UNKNOWN;
bool g_blurPrevValid = false;
bool g_motionWasActive = false;
LARGE_INTEGER g_lastBlurCapture{};
volatile LONG g_blurFailureLogged = 0;
volatile LONG g_blurSuccessLogged = 0;
double g_blurMotionX = 0.0;
double g_blurMotionY = 0.0;

std::string ExeDirectory() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    const auto p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : s.substr(0, p);
}

void LogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log.is_open()) {
        g_log << line << "\n";
        g_log.flush();
    }
}

float ReadFloat(const char* section, const char* key, float fallback) {
    char buf[64]{};
    char def[64]{};
    std::snprintf(def, sizeof(def), "%.6f", fallback);
    GetPrivateProfileStringA(section, key, def, buf, static_cast<DWORD>(sizeof(buf)), g_iniPath.c_str());
    char* end = nullptr;
    const float v = std::strtof(buf, &end);
    return end == buf ? fallback : v;
}

void LoadConfig() {
    g_enabled = GetPrivateProfileIntA("General", "Enabled", 1, g_iniPath.c_str()) != 0;
    g_logInput = GetPrivateProfileIntA("Debug", "LogInput", 1, g_iniPath.c_str()) != 0;
    g_smoothing = std::clamp(ReadFloat("Mouse", "Smoothing", 0.0f), 0.0f, 0.95f);
    g_sensitivityMultiplier = std::clamp(ReadFloat("Mouse", "SensitivityMultiplier", 1.0f), 0.05f, 20.0f);
    const UINT spikeClamp = GetPrivateProfileIntA("Mouse", "SpikeClamp", 0, g_iniPath.c_str());
    g_spikeClamp = static_cast<LONG>(spikeClamp);
    g_stallProtection = GetPrivateProfileIntA("Mouse", "StallProtection", 1, g_iniPath.c_str()) != 0;
    g_stallThresholdMs = std::clamp(static_cast<double>(ReadFloat("Mouse", "StallThresholdMs", 12.0f)), 6.0, 50.0);
    g_stallAlphaScale = std::clamp(static_cast<double>(ReadFloat("Mouse", "StallAlphaScale", 0.50f)), 0.20, 1.0);
    g_timeBasedSmoothing = GetPrivateProfileIntA("Mouse", "TimeBasedSmoothing", 1, g_iniPath.c_str()) != 0;
    g_smoothingReferenceMs = std::clamp(static_cast<double>(ReadFloat("Mouse", "SmoothingReferenceMs", 4.0f)), 1.0, 16.0);

    g_motionBlurEnabled = GetPrivateProfileIntA("Visual", "MotionBlur", 1, g_iniPath.c_str()) != 0;
    g_motionBlurStrength = std::clamp(static_cast<double>(ReadFloat("Visual", "MotionBlurStrength", 0.22f)), 0.0, 0.35);
    g_motionBlurHoldMs = std::clamp(static_cast<double>(ReadFloat("Visual", "MotionBlurHoldMs", 70.0f)), 10.0, 250.0);
    g_motionBlurHistoryMs = std::clamp(static_cast<double>(ReadFloat("Visual", "MotionBlurHistoryMs", 16.0f)), 2.0, 30.0);
    g_motionBlurTrailScale = std::clamp(static_cast<double>(ReadFloat("Visual", "MotionBlurTrailScale", 1.10f)), 0.0, 3.0);
    g_motionBlurStallBoost = std::clamp(static_cast<double>(ReadFloat("Visual", "MotionBlurStallBoost", 0.12f)), 0.0, 0.25);
    g_motionBlurSamples = std::clamp(static_cast<int>(GetPrivateProfileIntA("Visual", "MotionBlurSamples", 6, g_iniPath.c_str())), 1, 8);
}

double MsBetween(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    return (static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0) / static_cast<double>(g_qpcFreq.QuadPart);
}

void ResetStats(const LARGE_INTEGER& now) {
    g_windowStart = now;
    g_lastStateCall = {};
    g_lastMotionCall = {};
    g_stateCalls = 0;
    g_motionCalls = 0;
    g_stateIntervalSumMs = 0.0;
    g_motionIntervalSumMs = 0.0;
    g_maxStateGapMs = 0.0;
    g_maxMotionGapMs = 0.0;
    g_absX = 0;
    g_absY = 0;
}

void RecordInput(LONG dx, LONG dy) {
    if (!g_logInput) return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_windowStart.QuadPart == 0) ResetStats(now);

    if (g_lastStateCall.QuadPart != 0) {
        const double gap = MsBetween(g_lastStateCall, now);
        g_stateIntervalSumMs += gap;
        g_maxStateGapMs = std::max(g_maxStateGapMs, gap);
    }
    g_lastStateCall = now;
    ++g_stateCalls;

    if (dx != 0 || dy != 0) {
        if (g_lastMotionCall.QuadPart != 0) {
            const double gap = MsBetween(g_lastMotionCall, now);
            g_motionIntervalSumMs += gap;
            g_maxMotionGapMs = std::max(g_maxMotionGapMs, gap);
        }
        g_lastMotionCall = now;
        ++g_motionCalls;
        g_absX += static_cast<std::uint64_t>(std::llabs(static_cast<long long>(dx)));
        g_absY += static_cast<std::uint64_t>(std::llabs(static_cast<long long>(dy)));
    }

    const double windowMs = MsBetween(g_windowStart, now);
    if (windowMs >= 1000.0) {
        const double seconds = windowMs / 1000.0;
        const double stateRate = g_stateCalls / seconds;
        const double motionRate = g_motionCalls / seconds;
        const double avgStateGap = g_stateCalls > 1 ? g_stateIntervalSumMs / static_cast<double>(g_stateCalls - 1) : 0.0;
        const double avgMotionGap = g_motionCalls > 1 ? g_motionIntervalSumMs / static_cast<double>(g_motionCalls - 1) : 0.0;

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "INPUT  state=" << stateRate << "/s"
           << " motion=" << motionRate << "/s"
           << " avgStateGap=" << avgStateGap << "ms"
           << " maxStateGap=" << g_maxStateGapMs << "ms"
           << " avgMotionGap=" << avgMotionGap << "ms"
           << " maxMotionGap=" << g_maxMotionGapMs << "ms"
           << " absX=" << g_absX
           << " absY=" << g_absY;
        LogLine(ss.str());
        ResetStats(now);
    }
}

void ResetFrameStats(const LARGE_INTEGER& now) {
    g_frameWindowStart = now;
    g_lastPresent = {};
    g_frameCalls = 0;
    g_frameIntervalSumMs = 0.0;
    g_maxFrameGapMs = 0.0;
    g_frameOver12 = 0;
    g_frameOver20 = 0;
    g_frameOver33 = 0;
}

void RecordPresent() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_frameWindowStart.QuadPart == 0) ResetFrameStats(now);

    double gapMs = 0.0;
    if (g_lastPresent.QuadPart != 0) {
        gapMs = MsBetween(g_lastPresent, now);
        g_lastFrameGapMs = gapMs;
        g_frameIntervalSumMs += gapMs;
        g_maxFrameGapMs = std::max(g_maxFrameGapMs, gapMs);
        if (gapMs >= 12.0) ++g_frameOver12;
        if (gapMs >= 20.0) ++g_frameOver20;
        if (gapMs >= 33.0) ++g_frameOver33;

        if (gapMs >= 12.0) {
            const LONG n = InterlockedIncrement(&g_frameStallLogs);
            if (n <= 80) {
                std::ostringstream stall;
                stall << std::fixed << std::setprecision(2)
                      << "FRAME_STALL #" << n
                      << " gap=" << gapMs << "ms";
                LogLine(stall.str());
            }
        }
    }
    g_lastPresent = now;
    ++g_frameCalls;

    const double windowMs = MsBetween(g_frameWindowStart, now);
    if (windowMs >= 1000.0) {
        const double seconds = windowMs / 1000.0;
        const double fps = g_frameCalls / seconds;
        const double avgFrame = g_frameCalls > 1
            ? g_frameIntervalSumMs / static_cast<double>(g_frameCalls - 1)
            : 0.0;

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "FRAME  fps=" << fps
           << " avg=" << avgFrame << "ms"
           << " max=" << g_maxFrameGapMs << "ms"
           << " over12=" << g_frameOver12
           << " over20=" << g_frameOver20
           << " over33=" << g_frameOver33;
        LogLine(ss.str());
        ResetFrameStats(now);
    }
}

void ProcessMouseState(DWORD cbData, LPVOID data) {
    if (!g_enabled || data == nullptr) return;
    if (cbData != sizeof(DIMOUSESTATE) && cbData != sizeof(DIMOUSESTATE2)) return;

    auto* values = static_cast<LONG*>(data);
    LONG rawX = values[0];
    LONG rawY = values[1];
    RecordInput(rawX, rawY);

    double x = static_cast<double>(rawX) * g_sensitivityMultiplier;
    double y = static_cast<double>(rawY) * g_sensitivityMultiplier;

    if (g_spikeClamp > 0) {
        x = std::clamp(x, -static_cast<double>(g_spikeClamp), static_cast<double>(g_spikeClamp));
        y = std::clamp(y, -static_cast<double>(g_spikeClamp), static_cast<double>(g_spikeClamp));
    }

    if (g_smoothing > 0.0f) {
        const double alpha = 1.0 - static_cast<double>(g_smoothing);
        g_accumX += x;
        g_accumY += y;

        LONG outX = static_cast<LONG>(std::llround(g_accumX * alpha));
        LONG outY = static_cast<LONG>(std::llround(g_accumY * alpha));

        if (outX == 0 && std::abs(g_accumX) >= 1.0) outX = g_accumX > 0.0 ? 1 : -1;
        if (outY == 0 && std::abs(g_accumY) >= 1.0) outY = g_accumY > 0.0 ? 1 : -1;

        g_accumX -= outX;
        g_accumY -= outY;
        values[0] = outX;
        values[1] = outY;
    } else {
        values[0] = static_cast<LONG>(std::llround(x));
        values[1] = static_cast<LONG>(std::llround(y));
    }
}

void ProcessCustomMouseState(DWORD cbData, LPVOID data) {
    if (!g_enabled || !data) return;
    if (g_customDataSize == 0 || cbData != g_customDataSize) return;
    if (g_customXOffset == 0xFFFFFFFFu || g_customYOffset == 0xFFFFFFFFu) return;
    if (g_customXOffset + sizeof(LONG) > cbData || g_customYOffset + sizeof(LONG) > cbData) return;

    auto* bytes = static_cast<unsigned char*>(data);
    auto* xPtr = reinterpret_cast<LONG*>(bytes + g_customXOffset);
    auto* yPtr = reinterpret_cast<LONG*>(bytes + g_customYOffset);

    const LONG rawX = *xPtr;
    const LONG rawY = *yPtr;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double pollGapMs = 0.0;
    if (g_lastCustomPoll.QuadPart != 0) {
        pollGapMs = MsBetween(g_lastCustomPoll, now);
    }
    g_lastCustomPoll = now;

    RecordInput(rawX, rawY);

    if (rawX != 0 || rawY != 0) {
        g_lastMouseMotion = now;

        // Keep a short, low-noise direction estimate for cinematic turn blur.
        // This does not feed back into gameplay input.
        g_blurMotionX = (g_blurMotionX * 0.60) + (static_cast<double>(rawX) * 0.40);
        g_blurMotionY = (g_blurMotionY * 0.60) + (static_cast<double>(rawY) * 0.40);
    }

    double x = static_cast<double>(rawX) * g_sensitivityMultiplier;
    double y = static_cast<double>(rawY) * g_sensitivityMultiplier;

    if (g_spikeClamp > 0) {
        x = std::clamp(x, -static_cast<double>(g_spikeClamp), static_cast<double>(g_spikeClamp));
        y = std::clamp(y, -static_cast<double>(g_spikeClamp), static_cast<double>(g_spikeClamp));
    }

    if (g_smoothing > 0.0f) {
        // The game polls DirectInput more often than many mice deliver new deltas.
        // Keep total movement, but spread each delta over subsequent game polls.
        // This fills otherwise-empty input polls at high frame rates with only a
        // small amount of latency instead of inventing or dropping movement.
        double alpha = 1.0 - static_cast<double>(g_smoothing);

        if (g_timeBasedSmoothing) {
            // Treat Smoothing as the fraction of residual motion that remains
            // after one reference interval. This keeps the same feel when the
            // game polls at 200, 300, 450+ Hz instead of making smoothing depend
            // on the number of GetDeviceState calls.
            const double dtMs = std::clamp(
                pollGapMs > 0.0 ? pollGapMs : g_smoothingReferenceMs,
                0.25,
                50.0
            );
            const double residual = std::clamp(
                static_cast<double>(g_smoothing),
                0.001,
                0.95
            );
            alpha = 1.0 - std::pow(residual, dtMs / g_smoothingReferenceMs);
            alpha = std::clamp(alpha, 0.05, 1.0);
        }

        const bool stalled = g_stallProtection &&
                             pollGapMs >= g_stallThresholdMs &&
                             (rawX != 0 || rawY != 0);

        if (stalled) {
            // With time-based smoothing a long frame should catch up, not add
            // another delayed tail. Keep the historical scale only as a tiny
            // safety floor for old configs.
            alpha = std::max(alpha, 1.0 - (0.20 * g_stallAlphaScale));

            const LONG stallNo = InterlockedIncrement(&g_stallEvents);
            if (stallNo <= 30) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2)
                   << "STALL_PROTECT #" << stallNo
                   << " gap=" << pollGapMs << "ms"
                   << " rawX=" << rawX
                   << " rawY=" << rawY
                   << " releaseAlpha=" << alpha;
                LogLine(ss.str());
            }
        }

        g_accumX += x;
        g_accumY += y;

        LONG outX = static_cast<LONG>(std::llround(g_accumX * alpha));
        LONG outY = static_cast<LONG>(std::llround(g_accumY * alpha));

        if (outX == 0 && std::abs(g_accumX) >= 1.0) outX = g_accumX > 0.0 ? 1 : -1;
        if (outY == 0 && std::abs(g_accumY) >= 1.0) outY = g_accumY > 0.0 ? 1 : -1;

        g_accumX -= outX;
        g_accumY -= outY;
        *xPtr = outX;
        *yPtr = outY;
    } else {
        *xPtr = static_cast<LONG>(std::llround(x));
        *yPtr = static_cast<LONG>(std::llround(y));
    }
}

HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(void* self, DWORD cbData, LPVOID data) {
    if (!g_originalGetDeviceState) return DIERR_NOTINITIALIZED;

    const LONG callNo = InterlockedIncrement(&g_loggedStateCalls);
    if (callNo <= 12) {
        std::ostringstream ss;
        ss << "GetDeviceState call #" << callNo
           << ": self=" << self
           << " createdMouse=" << g_mouseDevice
           << " cbData=" << cbData;
        LogLine(ss.str());
    }

    const HRESULT hr = g_originalGetDeviceState(self, cbData, data);
    if (SUCCEEDED(hr)) {
        ProcessMouseState(cbData, data);
        ProcessCustomMouseState(cbData, data);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_GetDeviceData(void* self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA data, LPDWORD inOutCount, DWORD flags) {
    if (!g_originalGetDeviceData) return DIERR_NOTINITIALIZED;

    if (InterlockedCompareExchange(&g_loggedFirstDataCall, 1, 0) == 0) {
        std::ostringstream ss;
        ss << "FIRST GetDeviceData call: self=" << self
           << " mouse=" << g_mouseDevice
           << " cbObjectData=" << cbObjectData
           << " count=" << (inOutCount ? *inOutCount : 0);
        LogLine(ss.str());
    }

    const HRESULT hr = g_originalGetDeviceData(self, cbObjectData, data, inOutCount, flags);

    if (self != g_mouseDevice) return hr;

    if (SUCCEEDED(hr) && data && inOutCount && *inOutCount > 0 && cbObjectData >= sizeof(DIDEVICEOBJECTDATA)) {
        LONG dx = 0;
        LONG dy = 0;

        for (DWORD i = 0; i < *inOutCount; ++i) {
            const auto& e = data[i];
            if (e.dwOfs == DIMOFS_X) {
                dx += static_cast<LONG>(e.dwData);
            } else if (e.dwOfs == DIMOFS_Y) {
                dy += static_cast<LONG>(e.dwData);
            }
        }

        RecordInput(dx, dy);
    } else if (SUCCEEDED(hr) && inOutCount) {
        RecordInput(0, 0);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SetDataFormat(void* self, LPCDIDATAFORMAT format) {
    if (!g_originalSetDataFormat) return DIERR_NOTINITIALIZED;

    const HRESULT hr = g_originalSetDataFormat(self, format);

    if (format) {
        const LONG n = InterlockedIncrement(&g_loggedFormats);
        DWORD xOffset = 0xFFFFFFFFu;
        DWORD yOffset = 0xFFFFFFFFu;
        DWORD firstAxisOffset = 0xFFFFFFFFu;
        DWORD secondAxisOffset = 0xFFFFFFFFu;
        DWORD axisCount = 0;

        if (format->rgodf && format->dwNumObjs > 0) {
            for (DWORD i = 0; i < format->dwNumObjs; ++i) {
                const DIOBJECTDATAFORMAT& obj = format->rgodf[i];

                if (obj.pguid) {
                    if (IsEqualGUID(*obj.pguid, GUID_XAxis)) xOffset = obj.dwOfs;
                    if (IsEqualGUID(*obj.pguid, GUID_YAxis)) yOffset = obj.dwOfs;
                }

                if ((obj.dwType & DIDFT_AXIS) != 0) {
                    if (axisCount == 0) firstAxisOffset = obj.dwOfs;
                    if (axisCount == 1) secondAxisOffset = obj.dwOfs;
                    ++axisCount;
                }

                if (self == g_mouseDevice && i < 16) {
                    std::ostringstream objLog;
                    objLog << "Mouse format obj[" << i << "]"
                           << ": ofs=" << obj.dwOfs
                           << " type=0x" << std::hex << obj.dwType << std::dec
                           << " flags=0x" << std::hex << obj.dwFlags << std::dec
                           << " hasGuid=" << (obj.pguid ? 1 : 0);
                    LogLine(objLog.str());
                }
            }
        }

        if (xOffset == 0xFFFFFFFFu && firstAxisOffset != 0xFFFFFFFFu) {
            xOffset = firstAxisOffset;
        }
        if (yOffset == 0xFFFFFFFFu && secondAxisOffset != 0xFFFFFFFFu) {
            yOffset = secondAxisOffset;
        }

        std::ostringstream ss;
        ss << "SetDataFormat #" << n
           << ": self=" << self
           << " dataSize=" << format->dwDataSize
           << " numObjs=" << format->dwNumObjs
           << " flags=" << format->dwFlags
           << " axisCount=" << axisCount
           << " xOfs=" << xOffset
           << " yOfs=" << yOffset
           << " hr=" << static_cast<long>(hr);
        LogLine(ss.str());

        if (self == g_mouseDevice && SUCCEEDED(hr) &&
            xOffset != 0xFFFFFFFFu && yOffset != 0xFFFFFFFFu) {
            g_customDataSize = format->dwDataSize;
            g_customXOffset = xOffset;
            g_customYOffset = yOffset;

            std::ostringstream captured;
            captured << "Captured REAL mouse data format: size=" << g_customDataSize
                     << " xOfs=" << g_customXOffset
                     << " yOfs=" << g_customYOffset;
            LogLine(captured.str());
        }
    }

    return hr;
}

void ReleaseBlurResources() {
    if (g_blurStateBlock) {
        g_blurStateBlock->Release();
        g_blurStateBlock = nullptr;
    }
    if (g_blurPrevTexture) {
        g_blurPrevTexture->Release();
        g_blurPrevTexture = nullptr;
    }
    if (g_blurCurrTexture) {
        g_blurCurrTexture->Release();
        g_blurCurrTexture = nullptr;
    }
    g_blurDevice = nullptr;
    g_blurWidth = 0;
    g_blurHeight = 0;
    g_blurFormat = D3DFMT_UNKNOWN;
    g_blurPrevValid = false;
    g_motionWasActive = false;
    g_lastBlurCapture = {};
    g_blurMotionX = 0.0;
    g_blurMotionY = 0.0;
}

bool EnsureBlurResources(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer) {
    if (!device || !backBuffer) return false;

    D3DSURFACE_DESC desc{};
    if (FAILED(backBuffer->GetDesc(&desc))) return false;

    if (g_blurDevice == device &&
        g_blurPrevTexture &&
        g_blurCurrTexture &&
        g_blurWidth == desc.Width &&
        g_blurHeight == desc.Height &&
        g_blurFormat == desc.Format) {
        return true;
    }

    ReleaseBlurResources();

    if (FAILED(device->CreateTexture(
            desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
            desc.Format, D3DPOOL_DEFAULT, &g_blurPrevTexture, nullptr))) {
        return false;
    }

    if (FAILED(device->CreateTexture(
            desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
            desc.Format, D3DPOOL_DEFAULT, &g_blurCurrTexture, nullptr))) {
        ReleaseBlurResources();
        return false;
    }

    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_blurStateBlock))) {
        ReleaseBlurResources();
        return false;
    }

    g_blurDevice = device;
    g_blurWidth = desc.Width;
    g_blurHeight = desc.Height;
    g_blurFormat = desc.Format;
    g_blurPrevValid = false;

    std::ostringstream ss;
    ss << "Motion blur resources ready: "
       << g_blurWidth << "x" << g_blurHeight
       << " format=" << static_cast<int>(g_blurFormat);
    LogLine(ss.str());
    return true;
}

bool CopyBackBufferToTexture(
    IDirect3DDevice9* device,
    IDirect3DSurface9* backBuffer,
    IDirect3DTexture9* texture
) {
    if (!device || !backBuffer || !texture) return false;

    IDirect3DSurface9* dst = nullptr;
    if (FAILED(texture->GetSurfaceLevel(0, &dst)) || !dst) return false;

    const HRESULT hr = device->StretchRect(
        backBuffer, nullptr, dst, nullptr, D3DTEXF_NONE
    );
    dst->Release();
    return SUCCEEDED(hr);
}

void ApplyTurnMotionBlur(IDirect3DDevice9* device) {
    if (!g_motionBlurEnabled || g_motionBlurStrength <= 0.0) {
        g_motionWasActive = false;
        g_blurPrevValid = false;
        g_lastBlurCapture = {};
        g_blurMotionX = 0.0;
        g_blurMotionY = 0.0;
        return;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    bool motionActive = false;
    if (g_lastMouseMotion.QuadPart != 0) {
        motionActive = MsBetween(g_lastMouseMotion, now) <= g_motionBlurHoldMs;
    }

    if (!motionActive) {
        g_motionWasActive = false;
        g_blurPrevValid = false;
        g_lastBlurCapture = {};
        g_blurMotionX *= 0.35;
        g_blurMotionY *= 0.35;
        return;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetRenderTarget(0, &backBuffer)) || !backBuffer) {
        return;
    }

    if (!EnsureBlurResources(device, backBuffer)) {
        if (InterlockedCompareExchange(&g_blurFailureLogged, 1, 0) == 0) {
            LogLine("Motion blur disabled for this run: resource creation failed.");
        }
        backBuffer->Release();
        return;
    }

    bool refreshHistory = !g_blurPrevValid;
    if (g_lastBlurCapture.QuadPart == 0) {
        refreshHistory = true;
    } else if (MsBetween(g_lastBlurCapture, now) >= g_motionBlurHistoryMs) {
        refreshHistory = true;
    }

    if (!g_blurPrevValid) {
        if (!CopyBackBufferToTexture(device, backBuffer, g_blurPrevTexture)) {
            if (InterlockedCompareExchange(&g_blurFailureLogged, 1, 0) == 0) {
                LogLine("Motion blur disabled for this run: initial StretchRect failed.");
            }
            backBuffer->Release();
            return;
        }
        g_blurPrevValid = true;
        g_motionWasActive = true;
        g_lastBlurCapture = now;
        backBuffer->Release();
        return;
    }

    bool capturedCurrent = false;
    if (refreshHistory) {
        capturedCurrent = CopyBackBufferToTexture(device, backBuffer, g_blurCurrTexture);
        if (!capturedCurrent && InterlockedCompareExchange(&g_blurFailureLogged, 1, 0) == 0) {
            LogLine("Motion blur history update failed: StretchRect failed.");
        }
    }

    if (g_motionWasActive && SUCCEEDED(g_blurStateBlock->Capture())) {
        struct BlurVertex {
            float x, y, z, rhw;
            float u, v;
        };

        const float w = static_cast<float>(g_blurWidth);
        const float h = static_cast<float>(g_blurHeight);

        double strength = g_motionBlurStrength;
        double trailScale = g_motionBlurTrailScale;

        // A long frame is exactly when the micro-stutter is most visible.
        // Briefly increase the visual trail on that frame only.
        if (g_lastFrameGapMs >= 10.0) {
            strength = std::min(0.35, strength + g_motionBlurStallBoost);
            trailScale *= 1.35;
        }

        const double maxTrailPx = 36.0;
        double trailX = std::clamp(-g_blurMotionX * trailScale, -maxTrailPx, maxTrailPx);
        double trailY = std::clamp(-g_blurMotionY * trailScale, -maxTrailPx, maxTrailPx);

        // Small movement should still create a visible cinematic smear.
        if (std::abs(trailX) < 1.25 && std::abs(g_blurMotionX) >= 0.5) {
            trailX = g_blurMotionX > 0.0 ? -1.25 : 1.25;
        }
        if (std::abs(trailY) < 1.25 && std::abs(g_blurMotionY) >= 0.5) {
            trailY = g_blurMotionY > 0.0 ? -1.25 : 1.25;
        }

        const int samples = std::clamp(g_motionBlurSamples, 1, 8);
        const double perTap = 1.0 - std::pow(1.0 - strength, 1.0 / static_cast<double>(samples));
        const DWORD alphaByte = static_cast<DWORD>(
            std::clamp(perTap, 0.0, 0.35) * 255.0
        );
        const DWORD textureFactor = (alphaByte << 24) | 0x00FFFFFFu;

        const HRESULT beginHr = device->BeginScene();
        const bool beganScene = SUCCEEDED(beginHr);

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device->SetTexture(0, g_blurPrevTexture);

        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_TEXTUREFACTOR, textureFactor);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);

        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        HRESULT drawHr = D3D_OK;
        for (int i = 1; i <= samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(samples);
            const float ox = static_cast<float>(trailX) * t;
            const float oy = static_cast<float>(trailY) * t;

            const BlurVertex quad[4] = {
                {-0.5f + ox,     -0.5f + oy,      0.0f, 1.0f, 0.0f, 0.0f},
                {w - 0.5f + ox,  -0.5f + oy,      0.0f, 1.0f, 1.0f, 0.0f},
                {-0.5f + ox,      h - 0.5f + oy,  0.0f, 1.0f, 0.0f, 1.0f},
                {w - 0.5f + ox,   h - 0.5f + oy,  0.0f, 1.0f, 1.0f, 1.0f}
            };

            const HRESULT hr = device->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP,
                2,
                quad,
                sizeof(BlurVertex)
            );
            if (FAILED(hr)) {
                drawHr = hr;
                break;
            }
        }

        if (beganScene) {
            // Never call the virtual EndScene here: it is hooked and would
            // recurse back into Hook_D3DEndScene until the game crashes.
            if (g_originalD3DEndScene) {
                g_originalD3DEndScene(device);
            }
        }

        g_blurStateBlock->Apply();

        if (SUCCEEDED(drawHr)) {
            if (InterlockedCompareExchange(&g_blurSuccessLogged, 1, 0) == 0) {
                std::ostringstream ok;
                ok << std::fixed << std::setprecision(2)
                   << "MOTION_BLUR_ACTIVE strength=" << g_motionBlurStrength
                   << " historyMs=" << g_motionBlurHistoryMs
                   << " trailScale=" << g_motionBlurTrailScale
                   << " samples=" << g_motionBlurSamples
                   << " holdMs=" << g_motionBlurHoldMs;
                LogLine(ok.str());
            }
        } else if (InterlockedCompareExchange(&g_blurFailureLogged, 1, 0) == 0) {
            std::ostringstream fail;
            fail << "Motion blur draw failed: BeginSceneHr="
                 << static_cast<long>(beginHr)
                 << " DrawHr=" << static_cast<long>(drawHr);
            LogLine(fail.str());
        }
    }

    if (capturedCurrent) {
        std::swap(g_blurPrevTexture, g_blurCurrTexture);
        g_lastBlurCapture = now;
    }

    // Keep the visual direction smooth, independent from gameplay smoothing.
    g_blurMotionX *= 0.88;
    g_blurMotionY *= 0.88;

    g_motionWasActive = true;
    backBuffer->Release();
}

HRESULT STDMETHODCALLTYPE Hook_D3DPresent(
    void* self,
    const RECT* sourceRect,
    const RECT* destRect,
    HWND destWindow,
    const RGNDATA* dirtyRegion
) {
    RecordPresent();
    return g_originalD3DPresent(self, sourceRect, destRect, destWindow, dirtyRegion);
}

HRESULT STDMETHODCALLTYPE Hook_D3DEndScene(void* self) {
    if (!g_originalD3DEndScene) {
        return D3DERR_INVALIDCALL;
    }

    // If our own blur pass ever reaches EndScene through the vtable, bypass
    // the hook body and call the original method directly.
    if (g_insideBlurPass) {
        return g_originalD3DEndScene(self);
    }

    const HRESULT hr = g_originalD3DEndScene(self);

    if (SUCCEEDED(hr) && g_motionBlurEnabled) {
        g_insideBlurPass = true;
        ApplyTurnMotionBlur(static_cast<IDirect3DDevice9*>(self));
        g_insideBlurPass = false;
    }

    return hr;
}

bool HookD3DDevice(void* device) {
    if (!device) return false;

    auto** vtable = *reinterpret_cast<void***>(device);
    if (!vtable) return false;

    g_d3dDevice = device;
    void* presentTarget = vtable[17];
    void* endSceneTarget = vtable[42];

    std::ostringstream ss;
    ss << "D3D9 real device: Present=" << presentTarget
       << " EndScene=" << endSceneTarget
       << " self=" << device;
    LogLine(ss.str());

    const MH_STATUS createPresent = MH_CreateHook(
        presentTarget,
        reinterpret_cast<void*>(&Hook_D3DPresent),
        reinterpret_cast<void**>(&g_originalD3DPresent)
    );
    if (createPresent != MH_OK && createPresent != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(D3D9 Present) failed: " + std::to_string(static_cast<int>(createPresent)));
        return false;
    }

    const MH_STATUS enablePresent = MH_EnableHook(presentTarget);
    if (enablePresent != MH_OK && enablePresent != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(D3D9 Present) failed: " + std::to_string(static_cast<int>(enablePresent)));
        return false;
    }

    const MH_STATUS createEndScene = MH_CreateHook(
        endSceneTarget,
        reinterpret_cast<void*>(&Hook_D3DEndScene),
        reinterpret_cast<void**>(&g_originalD3DEndScene)
    );
    if (createEndScene != MH_OK && createEndScene != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(D3D9 EndScene) failed: " + std::to_string(static_cast<int>(createEndScene)));
        return false;
    }

    const MH_STATUS enableEndScene = MH_EnableHook(endSceneTarget);
    if (enableEndScene != MH_OK && enableEndScene != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(D3D9 EndScene) failed: " + std::to_string(static_cast<int>(enableEndScene)));
        return false;
    }

    LogLine("Hooked D3D9 Present + EndScene. Blur now renders after the game's EndScene.");
    return true;
}

HRESULT STDMETHODCALLTYPE Hook_D3DCreateDevice(
    void* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* params,
    IDirect3DDevice9** outDevice
) {
    const HRESULT hr = g_originalD3DCreateDevice(
        self, adapter, deviceType, focusWindow, behaviorFlags, params, outDevice
    );

    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        LogLine("D3D9 game device created.");
        HookD3DDevice(*outDevice);
    }

    return hr;
}

bool BootstrapD3D9() {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) {
        LogLine("D3D9 diagnostics unavailable: d3d9.dll not loaded.");
        return false;
    }

    auto create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!create9) {
        LogLine("D3D9 diagnostics unavailable: Direct3DCreate9 export not found.");
        return false;
    }

    IDirect3D9* probe = create9(D3D_SDK_VERSION);
    if (!probe) {
        LogLine("D3D9 diagnostics unavailable: Direct3DCreate9 returned null.");
        return false;
    }

    HWND hwnd = CreateWindowExA(
        0, "STATIC", "PrototypeSmoothMouse_D3D9Probe",
        WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr
    );

    if (!hwnd) {
        LogLine("D3D9 diagnostics unavailable: temporary window creation failed.");
        probe->Release();
        return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* tempDevice = nullptr;
    HRESULT deviceHr = probe->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &pp,
        &tempDevice
    );

    if (FAILED(deviceHr) || !tempDevice) {
        deviceHr = probe->CreateDevice(
            D3DADAPTER_DEFAULT,
            D3DDEVTYPE_REF,
            hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &pp,
            &tempDevice
        );
    }

    bool presentHooked = false;
    if (SUCCEEDED(deviceHr) && tempDevice) {
        presentHooked = HookD3DDevice(tempDevice);
        if (presentHooked) {
            LogLine("Bootstrap Present hook installed from a temporary D3D9 device.");
        }
        tempDevice->Release();
    } else {
        LogLine("D3D9 diagnostics unavailable: temporary device creation failed: " +
                std::to_string(static_cast<long>(deviceHr)));
    }

    // Also keep CreateDevice hooked in case the game or renderer creates another
    // device later. The Present hook above is the important path for an already
    // existing game device.
    auto** vtable = *reinterpret_cast<void***>(probe);
    if (vtable) {
        void* createDeviceTarget = vtable[16];
        const MH_STATUS createHook = MH_CreateHook(
            createDeviceTarget,
            reinterpret_cast<void*>(&Hook_D3DCreateDevice),
            reinterpret_cast<void**>(&g_originalD3DCreateDevice)
        );

        if (createHook == MH_OK || createHook == MH_ERROR_ALREADY_CREATED) {
            const MH_STATUS enableHook = MH_EnableHook(createDeviceTarget);
            if (enableHook == MH_OK || enableHook == MH_ERROR_ENABLED) {
                LogLine("Hooked IDirect3D9::CreateDevice for future D3D9 devices.");
            } else {
                LogLine("ERROR: MH_EnableHook(D3D9 CreateDevice) failed: " +
                        std::to_string(static_cast<int>(enableHook)));
            }
        } else {
            LogLine("ERROR: MH_CreateHook(D3D9 CreateDevice) failed: " +
                    std::to_string(static_cast<int>(createHook)));
        }
    }

    probe->Release();
    DestroyWindow(hwnd);
    return presentHooked;
}

bool HookMouseDevice(void* device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);

    auto** vtable = *reinterpret_cast<void***>(device);
    if (!vtable) return false;

    g_mouseDevice = device;

    void* stateTarget = vtable[9];
    void* dataTarget = vtable[10];
    void* formatTarget = vtable[11];

    std::ostringstream before;
    before << "Actual mouse vtable: GetDeviceState=" << stateTarget
           << " GetDeviceData=" << dataTarget
           << " SetDataFormat=" << formatTarget
           << " self=" << device;
    LogLine(before.str());

    const MH_STATUS createState = MH_CreateHook(
        stateTarget,
        reinterpret_cast<void*>(&Hook_GetDeviceState),
        reinterpret_cast<void**>(&g_originalGetDeviceState)
    );
    if (createState != MH_OK && createState != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(real GetDeviceState) failed: " + std::to_string(static_cast<int>(createState)));
        return false;
    }

    const MH_STATUS createData = MH_CreateHook(
        dataTarget,
        reinterpret_cast<void*>(&Hook_GetDeviceData),
        reinterpret_cast<void**>(&g_originalGetDeviceData)
    );
    if (createData != MH_OK && createData != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(real GetDeviceData) failed: " + std::to_string(static_cast<int>(createData)));
        return false;
    }

    const MH_STATUS enableState = MH_EnableHook(stateTarget);
    if (enableState != MH_OK && enableState != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(real GetDeviceState) failed: " + std::to_string(static_cast<int>(enableState)));
        return false;
    }

    const MH_STATUS enableData = MH_EnableHook(dataTarget);
    if (enableData != MH_OK && enableData != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(real GetDeviceData) failed: " + std::to_string(static_cast<int>(enableData)));
        return false;
    }

    const MH_STATUS createFormat = MH_CreateHook(
        formatTarget,
        reinterpret_cast<void*>(&Hook_SetDataFormat),
        reinterpret_cast<void**>(&g_originalSetDataFormat)
    );
    if (createFormat != MH_OK && createFormat != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(real SetDataFormat) failed: " + std::to_string(static_cast<int>(createFormat)));
        return false;
    }

    const MH_STATUS enableFormat = MH_EnableHook(formatTarget);
    if (enableFormat != MH_OK && enableFormat != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(real SetDataFormat) failed: " + std::to_string(static_cast<int>(enableFormat)));
        return false;
    }

    LogLine("MinHook installed on real mouse GetDeviceState + GetDeviceData + SetDataFormat targets.");
    return true;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(void* self, REFGUID guid, void** outDevice, LPUNKNOWN outer) {
    const HRESULT hr = g_originalCreateDevice(self, guid, outDevice, outer);
    if (SUCCEEDED(hr) && outDevice && *outDevice && IsEqualGUID(guid, GUID_SysMouse)) {
        LogLine("DirectInput mouse device created.");
        HookMouseDevice(*outDevice);
    }
    return hr;
}

bool HookDirectInputObject(void* di) {
    if (!di) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_originalCreateDevice) return true;

    auto** vtable = *reinterpret_cast<void***>(di);
    if (!vtable) return false;
    void* target = vtable[3];

    const MH_STATUS createStatus = MH_CreateHook(target, reinterpret_cast<void*>(&Hook_CreateDevice), reinterpret_cast<void**>(&g_originalCreateDevice));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(CreateDevice) failed: " + std::to_string(static_cast<int>(createStatus)));
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(CreateDevice) failed: " + std::to_string(static_cast<int>(enableStatus)));
        return false;
    }

    LogLine("Hooked IDirectInput8::CreateDevice.");
    return true;
}

HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    const HRESULT hr = g_originalDirectInput8Create(hinst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        LogLine("DirectInput8Create intercepted.");
        HookDirectInputObject(*out);
    }
    return hr;
}

bool BootstrapExistingDirectInput(DirectInput8CreateFn createExport) {
    IDirectInput8A* probeDI = nullptr;

    const HRESULT diHr = createExport(
        GetModuleHandleA(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8A,
        reinterpret_cast<void**>(&probeDI),
        nullptr
    );

    if (FAILED(diHr) || !probeDI) {
        LogLine("ERROR: diagnostic DirectInput8 object creation failed: " + std::to_string(static_cast<long>(diHr)));
        return false;
    }

    const bool diHooked = HookDirectInputObject(probeDI);
    probeDI->Release();

    if (diHooked) {
        LogLine("Bootstrap CreateDevice hook installed. Waiting for the game's real mouse device.");
        return true;
    }

    LogLine("ERROR: bootstrap CreateDevice hook installation failed.");
    return false;
}

DWORD WINAPI InitThread(LPVOID) {
    const std::string dir = ExeDirectory();
    g_iniPath = dir + "\\PrototypeSmoothMouse.ini";
    g_logPath = dir + "\\PrototypeSmoothMouse.log";
    LoadConfig();

    QueryPerformanceFrequency(&g_qpcFreq);
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.open(g_logPath, std::ios::out | std::ios::trunc);
    }

    LogLine("PrototypeSmoothMouse v0.16 EndScene recursion fix starting (x86).");

    {
        std::ostringstream cfg;
        cfg << std::fixed << std::setprecision(2)
            << "CONFIG smoothing=" << g_smoothing
            << " sensitivity=" << g_sensitivityMultiplier
            << " stallProtection=" << (g_stallProtection ? 1 : 0)
            << " stallThresholdMs=" << g_stallThresholdMs
            << " stallAlphaScale=" << g_stallAlphaScale
            << " timeBased=" << (g_timeBasedSmoothing ? 1 : 0)
            << " smoothingReferenceMs=" << g_smoothingReferenceMs
            << " motionBlur=" << (g_motionBlurEnabled ? 1 : 0)
            << " blurStrength=" << g_motionBlurStrength
            << " blurHoldMs=" << g_motionBlurHoldMs
            << " blurHistoryMs=" << g_motionBlurHistoryMs
            << " blurTrailScale=" << g_motionBlurTrailScale
            << " blurSamples=" << g_motionBlurSamples
            << " blurStallBoost=" << g_motionBlurStallBoost;
        LogLine(cfg.str());
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        LogLine("ERROR: MH_Initialize failed: " + std::to_string(static_cast<int>(initStatus)));
        return 0;
    }

    BootstrapD3D9();

    HMODULE dinput = nullptr;
    for (int i = 0; i < 600 && !dinput; ++i) {
        dinput = GetModuleHandleA("dinput8.dll");
        if (!dinput) Sleep(10);
    }
    if (!dinput) {
        LogLine("ERROR: dinput8.dll was not loaded within 6 seconds.");
        return 0;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(dinput, "DirectInput8Create"));
    if (!target) {
        LogLine("ERROR: DirectInput8Create export not found.");
        return 0;
    }

    auto createExport = reinterpret_cast<DirectInput8CreateFn>(target);

    // The game may have already created DirectInput before this ASI starts.
    // Create a temporary DirectInput mouse device only to discover the shared
    // COM method addresses, then hook those method implementations directly.
    BootstrapExistingDirectInput(createExport);

    const MH_STATUS createStatus = MH_CreateHook(target, reinterpret_cast<void*>(&Hook_DirectInput8Create), reinterpret_cast<void**>(&g_originalDirectInput8Create));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        LogLine("ERROR: MH_CreateHook(DirectInput8Create) failed: " + std::to_string(static_cast<int>(createStatus)));
        return 0;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        LogLine("ERROR: MH_EnableHook(DirectInput8Create) failed: " + std::to_string(static_cast<int>(enableStatus)));
        return 0;
    }

    LogLine("Hooked dinput8!DirectInput8Create. Waiting for real mouse polling.");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
