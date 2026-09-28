#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <dinput.h>
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

DirectInput8CreateFn g_originalDirectInput8Create = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
GetDeviceStateFn g_originalGetDeviceState = nullptr;
GetDeviceDataFn g_originalGetDeviceData = nullptr;
SetDataFormatFn g_originalSetDataFormat = nullptr;
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

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_windowStart{};
LARGE_INTEGER g_lastStateCall{};
LARGE_INTEGER g_lastMotionCall{};
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
    const LONG rawX = *reinterpret_cast<LONG*>(bytes + g_customXOffset);
    const LONG rawY = *reinterpret_cast<LONG*>(bytes + g_customYOffset);
    RecordInput(rawX, rawY);
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

    LogLine("PrototypeSmoothMouse v0.7 diagnostic starting (x86).");

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        LogLine("ERROR: MH_Initialize failed: " + std::to_string(static_cast<int>(initStatus)));
        return 0;
    }

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
