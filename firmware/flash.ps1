<#
.SYNOPSIS
    Builds and flashes the ESP32-C3 firmware with automatic port detection and DEVICE_NUMBER support.
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

$rootScript = Join-Path $PSScriptRoot "..\flash.ps1"
if (Test-Path $rootScript) {
    & $rootScript @PSBoundParameters
} else {
    Write-Error "Root flash.ps1 not found."
}
