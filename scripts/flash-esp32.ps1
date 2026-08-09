param(
    [string]$Port = "COM6",
    [ValidateRange(115200, 2000000)]
    [int]$Baud = 921600,
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf",
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDirectory "esp32-common.ps1")
$RepositoryDirectory = (Resolve-Path (Join-Path $ScriptDirectory "..")).Path
$FirmwarePath = Join-Path $RepositoryDirectory "build\esp32\Agent-ESP32P4-full.bin"
$FirmwarePath = [System.IO.Path]::GetFullPath($FirmwarePath)
if (-not (Test-Path -LiteralPath $FirmwarePath)) {
    throw "Firmware file not found: $FirmwarePath"
}

$SerialPort = Resolve-AgentSerialPort -RequestedPort $Port
if ($DryRun) {
    Write-Host "Dry run: would flash $FirmwarePath to $SerialPort at $Baud baud."
    return
}

$EspIdfPython = Import-AgentEspIdfEnvironment -IdfPath $IdfPath
Write-Host "Flashing $FirmwarePath to $SerialPort at $Baud baud..."
& $EspIdfPython -m esptool `
    --chip esp32p4 `
    -p $SerialPort `
    -b $Baud `
    --before default_reset `
    --after hard_reset `
    write_flash `
    --flash_mode dio `
    --flash_freq 40m `
    --flash_size 32MB `
    0x0 $FirmwarePath
if ($LASTEXITCODE -ne 0) { throw "ESP32 flashing failed." }

Write-Host "ESP32 flashing completed successfully."
