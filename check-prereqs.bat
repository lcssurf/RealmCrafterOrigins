@echo off
setlocal

set "INSTALL_ARG=%~1"
if /I "%INSTALL_ARG%"=="-install" set "CHECK_INSTALL=-Install"
if /I "%INSTALL_ARG%"=="-i" set "CHECK_INSTALL=-Install"
if /I "%INSTALL_ARG%"=="install" set "CHECK_INSTALL=-Install"

echo [RCO] Verificando pre-requisitos...
if defined CHECK_INSTALL (
    echo [RCO] Modo: instalacao de dependencias ausentes (se possivel)
) else (
    echo [RCO] Modo: apenas verificacao (sem instalar)
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-prereqs.ps1" %CHECK_INSTALL%
if %ERRORLEVEL% neq 0 (
    echo [RCO] Verificacao falhou.
) else (
    echo [RCO] Verificacao concluida.
)

pause
exit /b %ERRORLEVEL%
