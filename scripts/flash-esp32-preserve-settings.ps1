param(
    [string]$Port = "COM6",
    [ValidateRange(115200, 2000000)]
    [int]$Baud = 921600,
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf",
    [switch]$PromptForPort,
    [switch]$DryRun
)

# Compatibility entry point.
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "hardware/flash-esp32-preserve-settings.ps1") @PSBoundParameters
