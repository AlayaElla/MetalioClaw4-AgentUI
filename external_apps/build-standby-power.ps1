param(
    [string]$BuildDirectory = "$PSScriptRoot/examples/standby_power/build",
    [string]$Output = "$PSScriptRoot/dist/standby-power-1.0.0.eapp"
)
$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { throw "Run ESP-IDF export.ps1 first." }
$example = Join-Path $PSScriptRoot "examples/standby_power"
$idf = Join-Path $env:IDF_PATH "tools/idf.py"
if (-not (Test-Path (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    python $idf -C $example -B $BuildDirectory set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "Standby Power target configuration failed." }
}
python $idf -C $example -B $BuildDirectory build
if ($LASTEXITCODE -ne 0) { throw "Standby Power build failed." }
python (Join-Path $PSScriptRoot "tools/package_app.py") `
    --manifest (Join-Path $example "manifest.json") `
    --elf (Join-Path $BuildDirectory "metalio_standby_power.app.elf") --output $Output
if ($LASTEXITCODE -ne 0) { throw "Standby Power packaging failed." }
