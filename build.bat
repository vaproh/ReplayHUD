@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Install CMake and Visual Studio 2022 Build Tools
  echo with the "Desktop development with C++" workload.
  exit /b 1
)

where cl >nul 2>nul
if errorlevel 1 (
  echo MSVC compiler was not found.
  echo Run this file from "x64 Native Tools Command Prompt for VS 2022".
  exit /b 1
)

cmake -S . -B build -A x64
if errorlevel 1 exit /b 1

cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo Build complete: build\Release\ReplayHUD.exe
echo.
