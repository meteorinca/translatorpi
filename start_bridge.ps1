# PowerShell Startup Script for ESP32-C3 Translator Host Services
$origLocation = Get-Location
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "  Starting ESP32-C3 Translator Host Services (PowerShell)" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

# Check for venv python
$pythonExe = if (Test-Path "$ScriptDir\venv\Scripts\python.exe") {
    "$ScriptDir\venv\Scripts\python.exe"
} else {
    "python"
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

$mlProc = $null
try {
    Write-Host "[1/2] Starting Python ML Microservice in background on port $env:ML_SERVICE_PORT..." -ForegroundColor Green
    $mlProc = Start-Process -FilePath $pythonExe -ArgumentList "$ScriptDir\backend\pyworkers\ml_service.py" -WorkingDirectory $ScriptDir -NoNewWindow -PassThru

    Start-Sleep -Seconds 3

    Write-Host "[2/2] Starting Go WebSocket Bridge on port $env:BRIDGE_PORT..." -ForegroundColor Green
    Push-Location "$ScriptDir\backend\bridge"
    try {
        go run .
    } finally {
        Pop-Location
    }
} finally {
    if ($mlProc -and -not $mlProc.HasExited) {
        Write-Host "`nStopping Python ML Microservice (PID $($mlProc.Id))..." -ForegroundColor Yellow
        Stop-Process -Id $mlProc.Id -Force -ErrorAction SilentlyContinue
    }
    Set-Location $origLocation
}

