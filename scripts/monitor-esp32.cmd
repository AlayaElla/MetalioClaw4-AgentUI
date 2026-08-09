@echo off
setlocal
title Agent - Serial Monitor

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor-esp32.ps1" %*
set "SCRIPT_EXIT_CODE=%ERRORLEVEL%"

echo.
if not "%SCRIPT_EXIT_CODE%"=="0" (
    echo ESP32 monitor exited with code %SCRIPT_EXIT_CODE%.
)
echo Press any key to close this window.
pause >nul
exit /b %SCRIPT_EXIT_CODE%
