@echo off
setlocal
title Agent - Package Firmware

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0package-esp32.ps1" %*
set "SCRIPT_EXIT_CODE=%ERRORLEVEL%"

echo.
if "%SCRIPT_EXIT_CODE%"=="0" (
    echo ESP32 packaging completed successfully.
) else (
    echo ESP32 packaging failed with exit code %SCRIPT_EXIT_CODE%.
)
echo Press any key to close this window.
pause >nul
exit /b %SCRIPT_EXIT_CODE%
