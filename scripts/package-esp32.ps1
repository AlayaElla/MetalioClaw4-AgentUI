param(
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf"
)

# Compatibility entry point.
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "hardware/package-esp32.ps1") @PSBoundParameters
