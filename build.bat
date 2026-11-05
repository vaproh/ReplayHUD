@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake 3.21 or newer was not found. Install CMake first.
    exit /b 1
)

where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC compiler was not found.
    echo Open this file from "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

cmake -S . -B build -A x64 -DREPLAYHUD_BUILD_BACKEND_TESTS=ON
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

cmake -S . -B build-x86 -A Win32 -DREPLAYHUD_BUILD_BACKEND_TESTS=ON
if errorlevel 1 exit /b 1

cmake --build build-x86 --config Release --target ReplayHUDHook d3d8to9 ReplayHUDInjector32 uih_host --parallel
if errorlevel 1 exit /b 1

if not exist build\Release\x86 mkdir build\Release\x86
copy /Y build\d3d8-proxy\Release\d3d8.dll build\Release\d3d8.dll >nul
if errorlevel 1 exit /b 1
copy /Y build-x86\Release\ReplayHUDHook.dll build\Release\x86\ReplayHUDHook.dll >nul
if errorlevel 1 exit /b 1
copy /Y build-x86\d3d8-proxy\Release\d3d8.dll build\Release\x86\d3d8.dll >nul
if errorlevel 1 exit /b 1
copy /Y build-x86\Release\ReplayHUDInjector32.exe build\Release\ReplayHUDInjector32.exe >nul
if errorlevel 1 exit /b 1

echo.
echo Build complete:
echo   build\Release\ReplayHUD.exe
echo   build\Release\ReplayHUDHook.dll (x64)
echo   build\Release\d3d8.dll (x64 D3D8 compatibility proxy)
echo   build\Release\ReplayHUDInjector32.exe
echo   build\Release\x86\ReplayHUDHook.dll
echo   build\Release\x86\d3d8.dll (x86 D3D8 compatibility proxy)
