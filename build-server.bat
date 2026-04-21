@echo off
cd /d "%~dp0server"
echo [RCO] Building server...
if not exist "..\dist\server" mkdir "..\dist\server"
"C:\Program Files\Go\bin\go.exe" build -o "..\dist\server\server.exe" ./cmd/server
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- server.exe ready in dist\server\
pause
