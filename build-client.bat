@echo off
setlocal
cd /d "%~dp0client"
set "VCPKG_ROOT=%VCPKG_ROOT%"
if not defined VCPKG_ROOT (
    if exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=C:\vcpkg"
    ) else if exist "D:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=D:\vcpkg"
    ) else if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    ) else if exist "C:\src\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=C:\src\vcpkg"
    )
)
set "VCPKG_TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"

if not exist "%VCPKG_TOOLCHAIN%" (
    echo [RCO] CMake toolchain not found:
    echo [RCO] %VCPKG_TOOLCHAIN%
    echo [RCO] Defina VCPKG_ROOT para a pasta do vcpkg ou instale em C:\vcpkg.
    echo [RCO] Exemplo: set VCPKG_ROOT=D:\vcpkg
    pause
    exit /b 1
)

echo [RCO] Using vcpkg toolchain: %VCPKG_TOOLCHAIN%
echo [RCO] Configuring client (cmake)...
cmake -B build -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo [RCO] CMake configure FAILED.
    pause
    exit /b 1
)
echo [RCO] Building client...
cmake --build build --config Release
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- rco_client.exe ready in dist\client\
pause
