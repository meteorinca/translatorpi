@echo off
setlocal enabledelayedexpansion

echo =======================================================
echo   Starting ESP32-C3 Translator Host Services (Windows)
echo =======================================================

cd /d "%~dp0"

REM Activate virtualenv if available
if exist venv\Scripts\activate.bat (
    call venv\Scripts\activate.bat
) else if exist ..\venv\Scripts\activate.bat (
    call ..\venv\Scripts\activate.bat
)

set ML_SERVICE_PORT=9379
set BRIDGE_PORT=8765

echo [1/2] Launching Python ML Microservice on port %ML_SERVICE_PORT%...
start "Translator ML Microservice" cmd /k "python backend\pyworkers\ml_service.py"

REM Give ML service a few seconds to start
timeout /t 3 /nobreak >nul

echo [2/2] Launching Go Bridge on port %BRIDGE_PORT%...
cd backend\bridge
go run .

pause
