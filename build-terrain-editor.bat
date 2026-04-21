@echo off
cd /d "%~dp0tools\terrain-editor"
echo [RCO] Configuring terrain editor (cmake)...
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [RCO] CMake configure FAILED.
    pause
    exit /b 1
)
echo [RCO] Building terrain editor...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- rco_terrain.exe ready in dist\tools\
echo.
echo [RCO] Put materials in: dist\client\data\terrain\materials\^<name^>\
echo [RCO] Area files saved to: dist\client\data\areas\^<name^>\
pause
