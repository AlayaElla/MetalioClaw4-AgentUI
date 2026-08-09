param(
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDirectory "esp32-common.ps1")
$RepositoryDirectory = (Resolve-Path (Join-Path $ScriptDirectory "..")).Path
$ProjectDirectory = $RepositoryDirectory
$BuildDirectory = Join-Path $ProjectDirectory "build"
$OutputDirectory = Join-Path $RepositoryDirectory "build\esp32"

Push-Location $ProjectDirectory
try {
    $EspIdfPython = Import-AgentEspIdfEnvironment -IdfPath $IdfPath

    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "ESP32 build failed." }

    $PartitionTablePath = Join-Path $ProjectDirectory "partitions\v1\32m.csv"
    $PartitionTable = Get-Content -Raw -LiteralPath $PartitionTablePath
    if ($PartitionTable -match '(?im)^\s*(otadata|ota_0|ota_1)\s*,') {
        throw "OTA partition found in Agent partition table: $PartitionTablePath"
    }
    if ($PartitionTable -notmatch '(?im)^\s*factory\s*,\s*app\s*,\s*factory\s*,\s*0x200000\s*,\s*14M\s*,') {
        throw "Expected 14 MB factory app at 0x200000 was not found in $PartitionTablePath"
    }
    $FlashArgumentsPath = Join-Path $BuildDirectory "flash_args"
    $FlashArguments = Get-Content -Raw -LiteralPath $FlashArgumentsPath
    if ($FlashArguments -notmatch '(?im)^0x200000\s+agent\.bin\s*$') {
        throw "Built app is not mapped to 0x200000 in $FlashArgumentsPath"
    }

    Push-Location $BuildDirectory
    try {
        & $EspIdfPython -m esptool --chip esp32p4 merge-bin `
            -o Agent-ESP32P4-full.bin -f raw '@flash_args'
        if ($LASTEXITCODE -ne 0) { throw "ESP32 merge-bin failed." }
    }
    finally {
        Pop-Location
    }

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    Remove-Item -LiteralPath (Join-Path $OutputDirectory "Agent-ESP32P4-resources.bin") `
        -Force -ErrorAction SilentlyContinue
    $FullBinary = Join-Path $ProjectDirectory "build\Agent-ESP32P4-full.bin"
    $AppBinary = Join-Path $ProjectDirectory "build\agent.bin"
    $PackageDirectory = $OutputDirectory
    $ZipPath = Join-Path $OutputDirectory "Agent-ESP32P4-firmware.zip"

    if ((Get-Item -LiteralPath $AppBinary).Length -gt 14MB) {
        throw "Agent app exceeds the 14 MB factory partition: $AppBinary"
    }
    New-Item -ItemType Directory -Force -Path $PackageDirectory | Out-Null
    Copy-Item -LiteralPath $FullBinary -Destination (Join-Path $PackageDirectory "Agent-ESP32P4-full.bin") -Force
    Copy-Item -LiteralPath $AppBinary -Destination (Join-Path $PackageDirectory "Agent-ESP32P4-app.bin") -Force
    Compress-Archive -LiteralPath `
        (Join-Path $PackageDirectory "Agent-ESP32P4-full.bin"), `
        (Join-Path $PackageDirectory "Agent-ESP32P4-app.bin") `
        -DestinationPath $ZipPath -Force

    Write-Host "ESP32 package created: $ZipPath"
    Get-Item $ZipPath, `
        (Join-Path $PackageDirectory "Agent-ESP32P4-full.bin"), `
        (Join-Path $PackageDirectory "Agent-ESP32P4-app.bin") |
        Select-Object FullName, Length
}
finally {
    Pop-Location
}
