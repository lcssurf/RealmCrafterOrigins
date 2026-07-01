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

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL="
set "VS_VERSION="
set "VS_HAS_CPP_TOOLS="
set "CMAKE_GENERATOR_NAME="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS_INSTALL=%%i"
    if defined VS_INSTALL (
        set "VS_HAS_CPP_TOOLS=1"
        for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2^>nul`) do set "VS_VERSION=%%i"
    )
)

if not defined VS_INSTALL if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do set "VS_INSTALL=%%i"
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationVersion 2^>nul`) do set "VS_VERSION=%%i"
)

if not defined VS_INSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools" (
    set "VS_INSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
    set "VS_VERSION=17"
)
if not defined VS_INSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community" (
    set "VS_INSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    set "VS_VERSION=17"
)
if not defined VS_INSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" (
    set "VS_INSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    set "VS_VERSION=17"
)
if not defined VS_INSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" (
    set "VS_INSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
    set "VS_VERSION=17"
)
if not defined VS_INSTALL if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools" (
    set "VS_INSTALL=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
    set "VS_VERSION=16"
)
if not defined VS_INSTALL if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community" (
    set "VS_INSTALL=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
    set "VS_VERSION=16"
)
if defined VS_INSTALL if exist "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_HAS_CPP_TOOLS=1"
)

if "%VS_VERSION:~0,3%"=="17." set "CMAKE_GENERATOR_NAME=Visual Studio 17 2022"
if "%VS_VERSION:~0,3%"=="16." set "CMAKE_GENERATOR_NAME=Visual Studio 16 2019"
if "%VS_VERSION%"=="17" set "CMAKE_GENERATOR_NAME=Visual Studio 17 2022"
if "%VS_VERSION%"=="16" set "CMAKE_GENERATOR_NAME=Visual Studio 16 2019"

if not defined CMAKE_GENERATOR_NAME (
    echo [RCO] Visual Studio 2022/2019 was not found.
    if not exist "%VSWHERE%" echo [RCO] vswhere was not found at: %VSWHERE%
    echo [RCO] Checked common Visual Studio install folders and found no usable install.
    echo [RCO] Run check-prereqs.bat, confirm the Visual Studio Build Tools installer finishes, then open a new terminal.
    pause
    exit /b 1
)

if not defined VS_HAS_CPP_TOOLS (
    echo [RCO] Visual Studio was found, but C++ build tools were not confirmed.
    echo [RCO] If CMake fails, run check-prereqs.bat and install the C++ workload.
)

echo [RCO] Using Visual Studio: %VS_INSTALL%
echo [RCO] Using CMake generator: %CMAKE_GENERATOR_NAME%
if exist "build\CMakeCache.txt" (
    findstr /I /C:"CMAKE_GENERATOR:INTERNAL=%CMAKE_GENERATOR_NAME%" "build\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo [RCO] Existing CMake cache uses another generator. Recreating client build folder...
        rmdir /s /q build
    ) else (
        findstr /I /C:"CMAKE_GENERATOR_PLATFORM:INTERNAL=x64" "build\CMakeCache.txt" >nul 2>&1
        if errorlevel 1 (
            echo [RCO] Existing CMake cache uses another platform. Recreating client build folder...
            rmdir /s /q build
        )
    )
)

echo [RCO] Configuring client (cmake)...
cmake -S . -B build -G "%CMAKE_GENERATOR_NAME%" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" -DCMAKE_BUILD_TYPE=Release
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
