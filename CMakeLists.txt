cmake_minimum_required(VERSION 3.20)
project(PrototypeSmoothMouse LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project must be built for Windows x86.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Prototype 1 is 32-bit. Configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    minhook_src
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(minhook_src)

add_library(PrototypeSmoothMouse SHARED
    src/main.cpp
)

target_include_directories(PrototypeSmoothMouse PRIVATE
    "${minhook_src_SOURCE_DIR}/include"
)

target_compile_definitions(PrototypeSmoothMouse PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    DIRECTINPUT_VERSION=0x0800
)

target_link_libraries(PrototypeSmoothMouse PRIVATE
    minhook
    dxguid
)

if(MSVC)
    target_compile_options(PrototypeSmoothMouse PRIVATE /W4 /EHsc /permissive-)
endif()

set_target_properties(PrototypeSmoothMouse PROPERTIES
    PREFIX ""
    OUTPUT_NAME "aaa_PrototypeSmoothMouse"
    SUFFIX ".asi"
)
