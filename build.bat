@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake 3.20 or newer was not found. Install CMake first.
    exit /b 1
)

where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC compiler was not found.
    echo Open this file from "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

cmake -S . -B build -A x64
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo Build complete: build\Release\ReplayHUD.exe
