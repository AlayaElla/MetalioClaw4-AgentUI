param(
    [string]$BuildDirectory = "$PSScriptRoot/examples/voice_recorder/build",
    [string]$Output = "$PSScriptRoot/dist/voice-recorder-1.0.0.eapp"
)
$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { throw "Run ESP-IDF export.ps1 first." }
$example = Join-Path $PSScriptRoot "examples/voice_recorder"
$idf = Join-Path $env:IDF_PATH "tools/idf.py"
if (-not (Test-Path (Join-Path $BuildDirectory "CMakeCache.txt"))) {
    python $idf -C $example -B $BuildDirectory set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "Voice Recorder target configuration failed." }
}
python $idf -C $example -B $BuildDirectory build
if ($LASTEXITCODE -ne 0) { throw "Voice Recorder build failed." }
python (Join-Path $PSScriptRoot "tools/package_app.py") `
    --manifest (Join-Path $example "manifest.json") `
    --elf (Join-Path $BuildDirectory "metalio_voice_recorder.app.elf") --output $Output
if ($LASTEXITCODE -ne 0) { throw "Voice Recorder packaging failed." }
