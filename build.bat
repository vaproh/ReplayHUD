@echo off
setlocal
cd /d "%~dp0"

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

rem Build the 32-bit hook and injector first. The x64 app embeds these outputs.
cmake -S . -B build-x86 -A Win32 -DREPLAYHUD_BUILD_BACKEND_TESTS=ON
if errorlevel 1 exit /b 1

cmake --build build-x86 --config Release --target ReplayHUDHook d3d8to9 ReplayHUDInjector32 --parallel
if errorlevel 1 exit /b 1

rem Configure x64 after x86 so the executable can embed every runtime component.
cmake -S . -B build -A x64 ^
  -DREPLAYHUD_BUILD_BACKEND_TESTS=ON ^
  -DREPLAYHUD_X86_HOOK="%CD%\build-x86\Release\ReplayHUDHook.dll" ^
  -DREPLAYHUD_X86_INJECTOR="%CD%\build-x86\Release\ReplayHUDInjector32.exe"
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

rem Prepare the same three user-facing outputs used by GitHub Releases.
if exist dist rmdir /s /q dist
mkdir dist
if errorlevel 1 exit /b 1
mkdir dist\x86
if errorlevel 1 exit /b 1

copy /Y build\Release\ReplayHUD.exe dist\ReplayHUD.exe >nul
if errorlevel 1 exit /b 1
copy /Y build\d3d8-proxy\Release\d3d8.dll dist\d3d8.dll >nul
if errorlevel 1 exit /b 1
copy /Y build-x86\d3d8-proxy\Release\d3d8.dll dist\x86\d3d8.dll >nul
if errorlevel 1 exit /b 1
copy /Y README.md dist\README.md >nul
if errorlevel 1 exit /b 1
copy /Y LICENSE.txt dist\LICENSE.txt >nul
if errorlevel 1 exit /b 1
copy /Y build\_deps\universal_imgui_hook-src\LICENSE dist\LICENSE-Universal-Dear-ImGui-Hook.txt >nul
if errorlevel 1 exit /b 1
copy /Y build\_deps\d3d8to9-src\LICENSE.md dist\LICENSE-d3d8to9.txt >nul
if errorlevel 1 exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path 'dist\*' -DestinationPath 'ReplayHUD-Windows.zip' -Force"
if errorlevel 1 exit /b 1

echo.
echo Build and package complete:
echo   dist\ReplayHUD.exe       Bundled application
echo   dist\d3d8.dll            x64 D3D8 proxy
echo   dist\x86\d3d8.dll       x86 D3D8 proxy
echo   ReplayHUD-Windows.zip     Full package
