<#
.SYNOPSIS
    Builds the ESP32-C3 firmware and copies the output .bin to the root folder.

.DESCRIPTION
    Loads the ESP-IDF environment (v5.5.4), runs 'idf.py build' inside the firmware directory,
    and copies the generated binary (translator_c3.bin) to the main project folder.

.PARAMETER Device
    Optional device number to pass (e.g., -Device 1).

.PARAMETER Clean
    Performs a full clean before building.

.EXAMPLE
    .\buildc3.ps1
    .\buildc3.ps1 -Clean
    .\buildc3.ps1 -Device 2
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $false)]
    [Alias("DDEVICE", "DEVICE_NUMBER", "DeviceNumber", "D")]
    [string]$Device = $null,

    [Parameter(Mandatory = $false)]
    [switch]$Clean,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"
$origLocation = Get-Location

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
    Write-Error "Cannot locate firmware directory ($firmwareDir)."
    exit 1
}

try {
    Set-Location $firmwareDir

    # Clean if requested
    if ($Clean) {
        Write-Host "[ESP-IDF] Performing full clean..." -ForegroundColor Yellow
        idf.py fullclean
    }

    # Build command arguments
    $cmdArgs = @()
    if ($Device) {
        Write-Host "[ESP-IDF] Target DEVICE_NUMBER: $Device" -ForegroundColor Cyan
        $cmdArgs += "-DDEVICE_NUMBER=$Device"
    }

    Write-Host "[ESP-IDF] Building ESP32-C3 firmware..." -ForegroundColor Green
    if ($cmdArgs.Count -gt 0) {
        idf.py @cmdArgs build @ExtraArgs
    } else {
        idf.py build @ExtraArgs
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Error "idf.py build failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    # 3. Locate and copy .bin file to root project directory
    $binSource = Join-Path $firmwareDir "build\translator_c3.bin"
    if (Test-Path $binSource) {
        $destPath = Join-Path $PSScriptRoot "translator_c3.bin"
        Copy-Item -Path $binSource -Destination $destPath -Force
        $binSize = (Get-Item $destPath).Length
        Write-Host "=======================================================" -ForegroundColor Cyan
        Write-Host " [SUCCESS] Binary created and copied to root folder:" -ForegroundColor Green
        Write-Host "   Source : $binSource" -ForegroundColor Gray
        Write-Host "   Target : $destPath ($([math]::Round($binSize/1KB, 1)) KB)" -ForegroundColor Cyan
        Write-Host "=======================================================" -ForegroundColor Cyan
    } else {
        Write-Warning "Could not find built binary at $binSource"
    }
}
finally {
    Set-Location $origLocation
}
