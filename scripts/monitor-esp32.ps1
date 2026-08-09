param(
    [string]$Port = "COM6",
    [ValidateRange(9600, 2000000)]
    [int]$Baud = 115200,
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDirectory "esp32-common.ps1")
$ProjectDirectory = (Resolve-Path (Join-Path $ScriptDirectory "..")).Path
$ElfPath = Join-Path $ProjectDirectory "build\agent.elf"
if (-not (Test-Path -LiteralPath $ElfPath -PathType Leaf)) {
    throw "Firmware ELF not found: $ElfPath. Run package-esp32.cmd first."
}
$SerialPort = Resolve-AgentSerialPort -RequestedPort $Port

Push-Location $ProjectDirectory
try {
    $null = Import-AgentEspIdfEnvironment -IdfPath $IdfPath

    Write-Host "Monitoring Agent on $SerialPort at $Baud baud..."
    Write-Host "Reset: Ctrl+T, Ctrl+R    Exit: Ctrl+]"
    & idf.py -p $SerialPort -b $Baud monitor
    if ($LASTEXITCODE -ne 0) { throw "ESP32 monitor exited with code $LASTEXITCODE." }
}
finally {
    Pop-Location
}
