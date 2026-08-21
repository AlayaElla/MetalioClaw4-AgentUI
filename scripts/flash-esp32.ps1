param(
    [string]$Port = "COM6",
    [ValidateRange(115200, 2000000)]
    [int]$Baud = 921600,
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf",
    [switch]$SkipBuild,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDirectory "esp32-common.ps1")
$RepositoryDirectory = (Resolve-Path (Join-Path $ScriptDirectory "..")).Path
$PackageScriptPath = Join-Path $ScriptDirectory "package-esp32.ps1"
$BuildDirectory = Join-Path $RepositoryDirectory "build"
$FlashArgumentsPath = Join-Path $BuildDirectory "flash_args"

if (-not $DryRun -and -not $SkipBuild) {
    Write-Host "Building and packaging the current AgentUI source before flashing..."
    & $PackageScriptPath -IdfPath $IdfPath
    Write-Host "Current AgentUI build and package completed successfully."
}
elseif ($SkipBuild) {
    Write-Warning "SkipBuild was requested; existing build artifacts will be flashed without a freshness check."
}
else {
    Write-Host "Dry run: build and packaging are skipped."
}

if (-not (Test-Path -LiteralPath $FlashArgumentsPath -PathType Leaf)) {
    throw "ESP-IDF flash arguments not found: $FlashArgumentsPath. Run without -SkipBuild to build the current AgentUI source."
}

$FlashImagePaths = @(
    foreach ($Line in Get-Content -LiteralPath $FlashArgumentsPath) {
        if ($Line -match '^\s*0x[0-9a-fA-F]+\s+(?<Path>\S+)\s*$') {
            [System.IO.Path]::GetFullPath((Join-Path $BuildDirectory $Matches.Path))
        }
    }
)
if ($FlashImagePaths.Count -eq 0) {
    throw "No flash images were listed in $FlashArgumentsPath."
}
foreach ($FlashImagePath in $FlashImagePaths) {
    if (-not (Test-Path -LiteralPath $FlashImagePath -PathType Leaf)) {
        throw "Flash image not found: $FlashImagePath. Run without -SkipBuild to rebuild the current AgentUI source."
    }
}
$AppImagePath = @(
    $FlashImagePaths | Where-Object {
        [System.IO.Path]::GetFileName($_) -ieq "agent.bin"
    }
) | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($AppImagePath)) {
    throw "Agent application image was not listed in $FlashArgumentsPath."
}
$AppImage = Get-Item -LiteralPath $AppImagePath
$FlashBytes = ($FlashImagePaths | Get-Item | Measure-Object -Property Length -Sum).Sum
$FlashMiB = $FlashBytes / 1MB

Write-Host "Agent application selected for flashing:"
Write-Host "  Image:    $($AppImage.FullName)"
Write-Host ("  Modified: {0:yyyy-MM-dd HH:mm:ss}" -f $AppImage.LastWriteTime)
Write-Host ("  Size:     {0:N0} bytes" -f $AppImage.Length)

$SerialPort = Resolve-AgentSerialPort -RequestedPort $Port
if ($DryRun) {
    Write-Host ("Dry run: would erase the full chip, then write and verify {0:N2} MiB from {1}." -f `
        $FlashMiB, $FlashArgumentsPath)
    Write-Host "  Port: $SerialPort"
    Write-Host "  Baud: $Baud"
    return
}

$EspIdfPython = Import-AgentEspIdfEnvironment -IdfPath $IdfPath
Write-Host ("Erasing the full chip, then flashing and verifying {0:N2} MiB at $Baud baud..." -f $FlashMiB)
Push-Location $BuildDirectory
try {
    & $EspIdfPython -m esptool `
        --chip esp32p4 `
        -p $SerialPort `
        -b $Baud `
        --before default_reset `
        --after hard_reset `
        write_flash `
        --erase-all `
        '@flash_args'
    if ($LASTEXITCODE -ne 0) { throw "ESP32 flashing failed." }
}
finally {
    Pop-Location
}

Write-Host "ESP32 flashing completed successfully."
