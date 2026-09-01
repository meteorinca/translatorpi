# PowerShell Startup Script for ESP32-C3 Translator Host Services
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Starting ESP32-C3 Translator Host Services (PowerShell)" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

# Check for venv
if (Test-Path "$ScriptDir\venv\Scripts\Activate.ps1") {
    & "$ScriptDir\venv\Scripts\Activate.ps1"
}

$env:ML_SERVICE_PORT = "9379"
$env:BRIDGE_PORT = "8765"

# Clean up any previous processes holding port 9379 or 8765
try {
    $busy = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 9379, 8765 }
    foreach ($conn in $busy) {
        if ($conn.OwningProcess -and $conn.OwningProcess -ne $PID) {
            Write-Host "Closing previous process ($($conn.OwningProcess)) on port $($conn.LocalPort)..." -ForegroundColor Yellow
            Stop-Process -Id $conn.OwningProcess -Force -ErrorAction SilentlyContinue
        }
    }
} catch {}

Write-Host "[1/2] Starting Python ML Microservice on port $env:ML_SERVICE_PORT..." -ForegroundColor Green
Start-Process powershell -ArgumentList "-NoExit", "-Command", "Set-Location '$ScriptDir'; python backend/pyworkers/ml_service.py"

Start-Sleep -Seconds 3

Write-Host "[2/2] Starting Go WebSocket Bridge on port $env:BRIDGE_PORT..." -ForegroundColor Green
Set-Location "$ScriptDir\backend\bridge"
go run .
