cmake_minimum_required(VERSION 3.20)

project(ReplayHUD VERSION 1.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ReplayHUD is a native Windows application and must be built on Windows.")
endif()

enable_language(RC)

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

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

target_include_directories(ReplayHUD PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ReplayHUD PRIVATE cxx_std_17)

if(MSVC)
    set_property(TARGET ReplayHUD PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(ReplayHUD PRIVATE /W4 /permissive- /utf-8 /EHsc)
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

target_link_libraries(ReplayHUDHook PRIVATE
    user32
    d3d11
    d2d1
    dwrite
    dxgi
    minhook
)

set_target_properties(ReplayHUDHook PROPERTIES OUTPUT_NAME "ReplayHUDHook")

set_target_properties(ReplayHUD PROPERTIES OUTPUT_NAME "ReplayHUD")
