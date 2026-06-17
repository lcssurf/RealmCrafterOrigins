@echo off
cd /d "%~dp0tools\launcher"
echo [RCO] Configuring Launcher (cmake)...
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [RCO] CMake configure FAILED.
    pause
    exit /b 1
)
echo [RCO] Building Launcher...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- rco_launcher.exe ready in dist\tools\
pause
