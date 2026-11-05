cmake_minimum_required(VERSION 3.20)

project(ReplayHUD VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ReplayHUD is a native Windows application and must be built on Windows.")
endif()

add_executable(ReplayHUD WIN32 ReplayHUD.cpp)
target_compile_features(ReplayHUD PRIVATE cxx_std_17)
target_compile_definitions(ReplayHUD PRIVATE
    UNICODE _UNICODE _WIN32_WINNT=0x0602 WIN32_LEAN_AND_MEAN NOMINMAX
)

if(MSVC)
    set_property(TARGET ReplayHUD PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(ReplayHUD PRIVATE /W4 /O2 /EHsc)
endif()

target_link_libraries(ReplayHUD PRIVATE
    user32 gdi32 shell32 winhttp bcrypt crypt32 winmm
)
