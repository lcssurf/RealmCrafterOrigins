@echo off
cd /d "%~dp0tools\gue"
echo [RCO] Configuring GUE (cmake)...
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [RCO] CMake configure FAILED.
    pause
    exit /b 1
)
echo [RCO] Building GUE...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- rco_gue.exe ready in dist\tools\
pause
