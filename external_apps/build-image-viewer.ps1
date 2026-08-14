param(
    [string]$BuildDirectory = "$PSScriptRoot/examples/image_viewer/build",
    [string]$Output = "$PSScriptRoot/dist/image-viewer-0.1.0.eapp"
)

$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) {
    throw "IDF_PATH is not set. Run ESP-IDF export.ps1 first."
}

$example = Join-Path $PSScriptRoot "examples/image_viewer"
$idf = Join-Path $env:IDF_PATH "tools/idf.py"
$elf = Join-Path $BuildDirectory "metalio_image_viewer.app.elf"
$manifest = Join-Path $example "manifest.json"
$asset = Join-Path $PSScriptRoot "../main/display/agent_ui/apps/pet_demo/assets/q_drop_preview.png"
$packer = Join-Path $PSScriptRoot "tools/package_app.py"

if (-not (Test-Path (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    python $idf -C $example -B $BuildDirectory set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "Image Viewer target configuration failed." }
}
python $idf -C $example -B $BuildDirectory build
if ($LASTEXITCODE -ne 0) { throw "Image Viewer build failed." }
python $packer --manifest $manifest --elf $elf --asset $asset --output $Output
if ($LASTEXITCODE -ne 0) { throw "Image Viewer packaging failed." }
