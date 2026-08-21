param(
    [string]$BuildDirectory = "$PSScriptRoot/examples/radio/build",
    [string]$Output = "$PSScriptRoot/dist/radio-1.0.0.eapp"
)

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) {
    throw "IDF_PATH is not set. Run ESP-IDF export.ps1 first."
}

$example = Join-Path $PSScriptRoot "examples/radio"
$idf = Join-Path $env:IDF_PATH "tools/idf.py"
$elf = Join-Path $BuildDirectory "metalio_radio.app.elf"
$manifest = Join-Path $example "manifest.json"
$stations = Join-Path $example "assets/stations.json"
$packer = Join-Path $PSScriptRoot "tools/package_app.py"

if (-not (Test-Path (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    python $idf -C $example -B $BuildDirectory set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "Radio App target configuration failed." }
}
python $idf -C $example -B $BuildDirectory build
if ($LASTEXITCODE -ne 0) { throw "Radio App build failed." }
python $packer --manifest $manifest --elf $elf --asset $stations --output $Output
if ($LASTEXITCODE -ne 0) { throw "Radio App packaging failed." }
