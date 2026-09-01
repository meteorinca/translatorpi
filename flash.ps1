<#
.SYNOPSIS
    Builds and flashes the ESP32-C3 firmware with automatic port detection and DEVICE_NUMBER support.

.DESCRIPTION
    Ensures ESP-IDF environment (v5.5.4) is loaded via $env:IDF_PATH = "C:\esp\v5\v5.5.4\esp-idf"
    and the Espressif PowerShell profile, builds the firmware with optional -DDEVICE_NUMBER,
    and flashes it to the connected ESP32-C3 device (autodetecting the COM port).

.PARAMETER Device
    The device number (e.g. 1, 2, 3) to compile into firmware. 
    Aliases: DDEVICE, DEVICE_NUMBER, DeviceNumber, D.

.PARAMETER Monitor
    Switch to automatically open the serial monitor after flashing.

.PARAMETER Clean
    Switch to perform a full clean before building.

.EXAMPLE
    .\flash.ps1 3
    .\flash.ps1 -Device 2
    .\flash.ps1 -DDEVICE_NUMBER 1
    .\flash.ps1 -Device 3 -Monitor
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $false)]
    [Alias("DDEVICE", "DEVICE_NUMBER", "DeviceNumber", "D")]
    [string]$Device = $null,

    [Parameter(Mandatory = $false)]
    [switch]$Monitor,

    [Parameter(Mandatory = $false)]
    [switch]$Clean,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"

# 1. Ensure IDF_PATH and environment
$idfPath = "C:\esp\v5\v5.5.4\esp-idf"
$env:IDF_PATH = $idfPath

if (-not (Get-Command Invoke-idfpy -ErrorAction SilentlyContinue) -and -not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $profilePath = "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1"
    if (Test-Path $profilePath) {
        Write-Host "[ESP-IDF] Loading environment from $profilePath..." -ForegroundColor Cyan
        . $profilePath
    } elseif (Test-Path "$idfPath\export.ps1") {
        Write-Host "[ESP-IDF] Loading environment from $idfPath\export.ps1..." -ForegroundColor Cyan
        . "$idfPath\export.ps1"
    } else {
        Write-Error "Could not find ESP-IDF environment at $profilePath or $idfPath."
        exit 1
    }
}

# 2. Locate firmware directory
$firmwareDir = Join-Path $PSScriptRoot "firmware"
if (-not (Test-Path (Join-Path $firmwareDir "CMakeLists.txt"))) {
    if (Test-Path (Join-Path $PSScriptRoot "CMakeLists.txt")) {
        $firmwareDir = $PSScriptRoot
    } else {
        Write-Error "Cannot locate firmware directory with CMakeLists.txt."
        exit 1
    }
}

Push-Location $firmwareDir
try {
    # 3. Assemble arguments
    $cmdArgs = @()

    if ($Device) {
        # Extract numeric value if user passed "DDEVICE_NUMBER=3" or "-DDEVICE=3" or just "3"
        $devNum = $Device -replace '^[^\d]*', ''
        if (-not $devNum) { $devNum = $Device }
        Write-Host "[ESP-IDF] Target Device Number: #$devNum" -ForegroundColor Yellow
        $cmdArgs += "-DDEVICE_NUMBER=$devNum"
    }

    if ($Clean) {
        Write-Host "[ESP-IDF] Cleaning project..." -ForegroundColor Yellow
        if (Get-Command Invoke-idfpy -ErrorAction SilentlyContinue) {
            Invoke-idfpy fullclean
        } else {
            idf.py fullclean
        }
    }

    $subcommands = @("build", "flash")
    if ($Monitor) {
        $subcommands += "monitor"
    }

    Write-Host "[ESP-IDF] Running: idf.py $($cmdArgs -join ' ') $($subcommands -join ' ')..." -ForegroundColor Green

    if (Get-Command Invoke-idfpy -ErrorAction SilentlyContinue) {
        Invoke-idfpy @cmdArgs @subcommands @ExtraArgs
    } else {
        idf.py @cmdArgs @subcommands @ExtraArgs
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Error "idf.py exited with error code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    Write-Host "`n[ESP-IDF] Build and Flash completed successfully!" -ForegroundColor Green
} finally {
    Pop-Location
}
