@echo off
setlocal

cd /d "%~dp0"

where npm >nul 2>nul
if errorlevel 1 (
    echo npm is not installed or is not available in PATH.
    echo Install Node.js, then reopen this launcher.
    pause
    exit /b 1
)

cd /d "%~dp0app"
if errorlevel 1 (
    echo Could not find the app folder next to this launcher.
    pause
    exit /b 1
)

echo Starting De-Sentry...
echo Keep this window open while using the app.
echo.
npm run tauri:dev
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
    echo.
    echo De-Sentry exited with code %EXIT_CODE%.
)
pause
exit /b %EXIT_CODE%
