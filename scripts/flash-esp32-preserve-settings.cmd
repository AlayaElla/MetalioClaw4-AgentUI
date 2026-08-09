@echo off
setlocal
title Agent - Flash Firmware (Preserve Settings)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash-esp32-preserve-settings.ps1" %*
set "SCRIPT_EXIT_CODE=%ERRORLEVEL%"

echo.
if "%SCRIPT_EXIT_CODE%"=="0" (
    echo ESP32 app flashing completed successfully. Device settings were preserved.
) else (
    echo ESP32 app flashing failed with exit code %SCRIPT_EXIT_CODE%.
)
echo Press any key to close this window.
pause >nul
exit /b %SCRIPT_EXIT_CODE%
