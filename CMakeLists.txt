cmake_minimum_required(VERSION 3.21)

project(ReplayHUD VERSION 1.3.0 LANGUAGES CXX)

# A per-configure identifier keeps runtime DLLs versioned. Games may keep an
# older hook mapped after ReplayHUD exits, so new builds must never overwrite it.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef REPLAYHUD_BUILD_ID)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ReplayHUDBuildConfig.h"
    "#pragma once\n#define REPLAYHUD_BUILD_ID L\"${REPLAYHUD_BUILD_ID}\"\n")

set(REPLAYHUD_X86_HOOK "" CACHE FILEPATH "Path to the x86 ReplayHUDHook.dll used by the x64 app")
set(REPLAYHUD_X86_INJECTOR "" CACHE FILEPATH "Path to the x86 ReplayHUDInjector32.exe used by the x64 app")

# Regenerate the build identifier whenever an embedded component changes.
set(REPLAYHUD_CONFIGURE_DEPENDENCIES
    "${CMAKE_CURRENT_SOURCE_DIR}/ReplayHUDHook.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ReplayHUDOverlayBridge.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ReplayHUDInjector32.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ReplayHUDD3D8Bootstrap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/resource.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateAppResources.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateD3D8Resources.cmake"
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND REPLAYHUD_CONFIGURE_DEPENDENCIES
        "${REPLAYHUD_X86_HOOK}"
        "${REPLAYHUD_X86_INJECTOR}"
    )
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${REPLAYHUD_CONFIGURE_DEPENDENCIES}
)

if(NOT WIN32)
    message(FATAL_ERROR "ReplayHUD is a native Windows application and must be built on Windows.")
endif()

enable_language(RC)

include(FetchContent)

# Universal Dear ImGui Hook supplies the tested D3D9/10/11/12, Vulkan and
# OpenGL render backends. Keep the dependency pinned; do not follow master.
option(REPLAYHUD_BUILD_BACKEND_TESTS "Build the upstream graphics backend test hosts" ON)
set(UIH_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(UIH_BUILD_TESTS ${REPLAYHUD_BUILD_BACKEND_TESTS} CACHE BOOL "" FORCE)
set(UIH_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_Declare(universal_imgui_hook
    GIT_REPOSITORY https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git
    GIT_TAG 2364c2f24e06af2e03e9576d3d88f7c92b7ccc43
)
FetchContent_MakeAvailable(universal_imgui_hook)

# Direct3D 8 has no renderer backend in the universal hook. Build the
# established D3D8-to-D3D9 compatibility proxy and bootstrap ReplayHUDHook
# from that proxy. The actual overlay then uses the already-tested D3D9 path.
set(D3D8TO9_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(d3d8to9
    GIT_REPOSITORY https://github.com/crosire/d3d8to9.git
    GIT_TAG 255338f698c8270b537f0a91a13f795f4f988250
)
FetchContent_MakeAvailable(d3d8to9)
target_sources(d3d8to9 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ReplayHUDD3D8Bootstrap.cpp")
if(MSVC)
    set_property(TARGET d3d8to9 PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(d3d8to9 PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W3>"
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
    )
endif()
set_target_properties(d3d8to9 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/d3d8-proxy"
)

# Embed the icon using an absolute path so RC resolves it consistently in
# out-of-source and GitHub Actions builds.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/ReplayHUD.ico" REPLAYHUD_ICON_FILE)
string(REPLACE "\\" "\\\\" REPLAYHUD_ICON_FILE "${REPLAYHUD_ICON_FILE}")
configure_file(ReplayHUD.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ReplayHUD.rc" @ONLY)

add_executable(ReplayHUD WIN32
    ReplayHUD.cpp
    ReplayHUDOverlayBridge.h
    "${CMAKE_CURRENT_BINARY_DIR}/ReplayHUD.rc"
    resource.h
    assets/ReplayHUD.ico
)

target_include_directories(ReplayHUD PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_features(ReplayHUD PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET ReplayHUD PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(ReplayHUD PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W4>"
        "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
    )
endif()

target_link_libraries(ReplayHUD PRIVATE
    user32
    gdi32
    shell32
    advapi32
    winhttp
    bcrypt
    crypt32
    winmm
    comctl32
    shcore
)

# This target is built by the separate Win32 configuration. The x64 main
# application launches it to inject the x86 hook into 32-bit games.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(ReplayHUDInjector32 WIN32 ReplayHUDInjector32.cpp)
    target_compile_features(ReplayHUDInjector32 PRIVATE cxx_std_17)
    target_link_libraries(ReplayHUDInjector32 PRIVATE shell32)
    if(MSVC)
        set_property(TARGET ReplayHUDInjector32 PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        target_compile_options(ReplayHUDInjector32 PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
endif()

add_library(ReplayHUDHook SHARED
    ReplayHUDHook.cpp
    ReplayHUDOverlayBridge.h
)

target_include_directories(ReplayHUDHook PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ReplayHUDHook PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET ReplayHUDHook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(ReplayHUDHook PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

target_include_directories(ReplayHUDHook PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ReplayHUDHook PRIVATE uih::uih user32)
set_target_properties(ReplayHUDHook PROPERTIES OUTPUT_NAME "ReplayHUDHook")

# D3D8 proxy is a standalone per-game DLL. Embed the architecture-matching
# hook so users do not need to copy a second file beside the game executable.
set(REPLAYHUD_D3D8_RESOURCES_RC "${CMAKE_CURRENT_BINARY_DIR}/ReplayHUDD3D8Resources.rc")
add_custom_command(
    OUTPUT "${REPLAYHUD_D3D8_RESOURCES_RC}"
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT=${REPLAYHUD_D3D8_RESOURCES_RC}"
        "-DHOOK=$<TARGET_FILE:ReplayHUDHook>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateD3D8Resources.cmake"
    DEPENDS ReplayHUDHook
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateD3D8Resources.cmake"
    VERBATIM
)
target_sources(d3d8to9 PRIVATE "${REPLAYHUD_D3D8_RESOURCES_RC}")
target_include_directories(d3d8to9 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
add_dependencies(d3d8to9 ReplayHUDHook)

# The 64-bit launcher bundles both hook architectures and the 32-bit helper.
# Build x86 components first, then pass their paths to the x64 configuration.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    foreach(required REPLAYHUD_X86_HOOK REPLAYHUD_X86_INJECTOR)
        if(NOT EXISTS "${${required}}")
            message(FATAL_ERROR
                "${required} must point to a built x86 component. Build the Win32 configuration first.")
        endif()
    endforeach()

    set(REPLAYHUD_APP_RESOURCES_RC "${CMAKE_CURRENT_BINARY_DIR}/ReplayHUDAppResources.rc")
    add_custom_command(
        OUTPUT "${REPLAYHUD_APP_RESOURCES_RC}"
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT=${REPLAYHUD_APP_RESOURCES_RC}"
            "-DX64_HOOK=$<TARGET_FILE:ReplayHUDHook>"
            "-DX86_HOOK=${REPLAYHUD_X86_HOOK}"
            "-DX86_INJECTOR=${REPLAYHUD_X86_INJECTOR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateAppResources.cmake"
        DEPENDS ReplayHUDHook
            "${REPLAYHUD_X86_HOOK}"
            "${REPLAYHUD_X86_INJECTOR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateAppResources.cmake"
        VERBATIM
    )
    target_sources(ReplayHUD PRIVATE "${REPLAYHUD_APP_RESOURCES_RC}")
    add_dependencies(ReplayHUD ReplayHUDHook)
endif()

set_target_properties(ReplayHUD PROPERTIES OUTPUT_NAME "ReplayHUD")
