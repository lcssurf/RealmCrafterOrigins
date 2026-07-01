@echo off
setlocal

set "INSTALL_ARG=%~1"
set "CHECK_INSTALL=-Install"

if /I "%INSTALL_ARG%"=="-check-only" set "CHECK_INSTALL="
if /I "%INSTALL_ARG%"=="-check"     set "CHECK_INSTALL="
if /I "%INSTALL_ARG%"=="-no-install" set "CHECK_INSTALL="

if defined CHECK_INSTALL (
    echo [RCO] Verificando pre-requisitos e instalando o que faltar...
) else (
    echo [RCO] Verificando apenas pre-requisitos (sem instalar)
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-prereqs.ps1" %CHECK_INSTALL%
if %ERRORLEVEL% neq 0 (
    echo [RCO] Verificacao falhou.
) else (
    echo [RCO] Verificacao concluida.
)

pause
exit /b %ERRORLEVEL%
