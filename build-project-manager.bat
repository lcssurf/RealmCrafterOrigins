@echo off
cd /d "%~dp0tools\project_manager"
echo [RCO] Configuring Project Manager (cmake)...
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [RCO] CMake configure FAILED.
    pause
    exit /b 1
)
echo [RCO] Building Project Manager...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- rco_project_manager.exe ready in dist\tools\
pause
