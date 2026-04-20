@echo off
cd /d "%~dp0server"
echo [RCO] Building server...
"C:\Program Files\Go\bin\go.exe" build ./cmd/server
if %ERRORLEVEL% neq 0 (
    echo [RCO] Build FAILED.
    pause
    exit /b 1
)
echo [RCO] Build OK -- server.exe ready in server\
pause
