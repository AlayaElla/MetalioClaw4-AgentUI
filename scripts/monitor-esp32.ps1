param(
    [string]$Port = "",
    [ValidateRange(9600, 2000000)]
    [int]$Baud = 115200,
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf"
)

# Compatibility entry point.
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "hardware/monitor-esp32.ps1") @PSBoundParameters
