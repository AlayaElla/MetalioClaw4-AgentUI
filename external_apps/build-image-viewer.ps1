param(
    [string]$BuildDirectory = "$PSScriptRoot/examples/image_viewer/build",
    [string]$Output = "$PSScriptRoot/dist/image-viewer-1.0.0.eapp"
)

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) {
    throw "IDF_PATH is not set. Run ESP-IDF export.ps1 first."
}

$example = Join-Path $PSScriptRoot "examples/image_viewer"
$idf = Join-Path $env:IDF_PATH "tools/idf.py"
$elf = Join-Path $BuildDirectory "metalio_image_viewer.app.elf"
$manifest = Join-Path $example "manifest.json"
$assetDirectory = Join-Path $example "assets"
$packer = Join-Path $PSScriptRoot "tools/package_app.py"

if (-not (Test-Path (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    python $idf -C $example -B $BuildDirectory set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "Image Viewer target configuration failed." }
}
python $idf -C $example -B $BuildDirectory build
if ($LASTEXITCODE -ne 0) { throw "Image Viewer build failed." }
$assets = @(Get-ChildItem -LiteralPath $assetDirectory -Filter "*.png" -File |
    Sort-Object Name)
if ($assets.Count -ne 5) {
    throw "Image Viewer package requires exactly 5 PNG assets; found $($assets.Count)."
}
if (-not ($assets.Name -contains "demo.png")) {
    throw "Image Viewer package icon assets/demo.png is missing."
}

$packageArgs = @($packer, "--manifest", $manifest, "--elf", $elf)
foreach ($asset in $assets) {
    $packageArgs += @("--asset", $asset.FullName)
}
$packageArgs += @("--output", $Output)
python @packageArgs
if ($LASTEXITCODE -ne 0) { throw "Image Viewer packaging failed." }
