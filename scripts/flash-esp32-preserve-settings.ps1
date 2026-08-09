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
$ProjectDirectory = $RepositoryDirectory
$PartitionTablePath = Join-Path $ProjectDirectory "partitions\v1\32m.csv"
$FirmwarePath = Join-Path $RepositoryDirectory "build\esp32\Agent-ESP32P4-app.bin"
$FirmwarePath = [System.IO.Path]::GetFullPath($FirmwarePath)

$FactoryOffset = 0x200000
$FactorySize = 14MB
$PartitionTable = Get-Content -Raw -LiteralPath $PartitionTablePath
if ($PartitionTable -notmatch '(?im)^\s*factory\s*,\s*app\s*,\s*factory\s*,\s*0x200000\s*,\s*14M\s*,') {
    throw "Expected 14 MB factory app at 0x200000 was not found in $PartitionTablePath"
}
if (-not (Test-Path -LiteralPath $FirmwarePath)) {
    throw "App firmware file not found: $FirmwarePath. Run scripts\package-esp32.ps1 first."
}
if ((Get-Item -LiteralPath $FirmwarePath).Length -gt $FactorySize) {
    throw "App firmware exceeds the 14 MB factory partition: $FirmwarePath"
}

$SerialPort = Resolve-AgentSerialPort -RequestedPort $Port
if ($DryRun) {
    Write-Host "Dry run: would flash only the app image."
    Write-Host "  Image:  $FirmwarePath"
    Write-Host ("  Offset: 0x{0:X}" -f $FactoryOffset)
    Write-Host "  Port:   $SerialPort"
    Write-Host "  Baud:   $Baud"
    Write-Host "NVS and other data partitions would not be erased or written."
    return
}

$EspIdfPython = Import-AgentEspIdfEnvironment -IdfPath $IdfPath
Write-Host "Flashing app image only; preserving NVS and device settings..."
Write-Host ("Writing $FirmwarePath to $SerialPort at 0x{0:X} ($Baud baud)..." -f $FactoryOffset)
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
    $FactoryOffset $FirmwarePath
if ($LASTEXITCODE -ne 0) { throw "ESP32 app flashing failed." }

Write-Host "ESP32 app flashing completed successfully. NVS and device settings were preserved."
