param(
    [switch]$BuildFirmware,
    [string]$IdfPath = "D:\esp\v6.0.2\esp-idf",
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Sha256 {
    param([string]$Path)
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    $Stream = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Path).Path)
    try {
        return ([System.BitConverter]::ToString($Hasher.ComputeHash($Stream))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $Stream.Dispose()
        $Hasher.Dispose()
    }
}

$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $CaptureStamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
    $OutputDirectory = Join-Path $RepositoryRoot ".tmp\esp32-system-performance-20261006\capture-$CaptureStamp"
}
if (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepositoryRoot $OutputDirectory
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

. (Join-Path $PSScriptRoot "esp32-common.ps1")
Push-Location $RepositoryRoot
try {
    $PythonPath = Import-AgentEspIdfEnvironment -IdfPath $IdfPath
    $BuildLog = Join-Path $OutputDirectory "build.log"
    $SizeLog = Join-Path $OutputDirectory "idf-size.log"
    if ($BuildFirmware) {
        "Starting direct ESP-IDF firmware build (no packaging or flashing)." | Set-Content -LiteralPath $BuildLog
        $PreviousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & rtk proxy idf.py.exe build 2>&1 | Tee-Object -FilePath $BuildLog -Append
            $BuildExitCode = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $PreviousErrorActionPreference }
        if ($BuildExitCode -ne 0) { throw "idf.py build failed with exit code $BuildExitCode" }
    }

    $ArtifactPaths = @("build\agent.bin", "build\agent.elf", "build\agent.map")
    $Artifacts = @(
        foreach ($ArtifactPath in $ArtifactPaths) {
            if (Test-Path -LiteralPath $ArtifactPath -PathType Leaf) {
                $ArtifactFile = Get-Item -LiteralPath $ArtifactPath
                [pscustomobject]@{
                    path = $ArtifactPath.Replace("\", "/")
                    size_bytes = $ArtifactFile.Length
                    sha256 = Get-Sha256 -Path $ArtifactPath
                }
            }
        }
    )

    $ElfSections = @{}
    $SizeTool = Get-Command "riscv32-esp-elf-size" -ErrorAction SilentlyContinue
    if ($SizeTool -and (Test-Path -LiteralPath "build\agent.elf" -PathType Leaf)) {
        $PreviousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $SizeOutput = @(& rtk proxy $SizeTool.Source -A "build\agent.elf" 2>&1)
            $SizeExitCode = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $PreviousErrorActionPreference }
        if ($SizeExitCode -ne 0) { throw "ELF section size command failed with exit code $SizeExitCode" }
        $SizeOutput | Set-Content -LiteralPath $SizeLog
        foreach ($Line in $SizeOutput) {
            if ($Line -match '^\s*(\.[^\s]+)\s+(\d+)\s+(?:0x[0-9A-Fa-f]+|\d+)\s*$') {
                $ElfSections[$Matches[1]] = [int64]$Matches[2]
            }
        }
    }

    $PartitionBytes = $null
    $PartitionTable = "partitions/v1/32m.csv"
    if (Test-Path -LiteralPath $PartitionTable -PathType Leaf) {
        foreach ($Line in Get-Content -LiteralPath $PartitionTable) {
            $TrimmedLine = $Line.Trim()
            if (-not $TrimmedLine -or $TrimmedLine.StartsWith("#")) { continue }
            $Columns = @($TrimmedLine.Split(',') | ForEach-Object { $_.Trim() })
            if ($Columns.Count -lt 5 -or $Columns[1] -ne "app") { continue }
            if ($Columns[0] -ne "factory") { continue }
            if ($Columns[4] -match '^(?<count>\d+)(?<unit>[KMG]?)$') {
                $Multiplier = switch ($Matches.unit) {
                    "K" { 1KB }
                    "M" { 1MB }
                    "G" { 1GB }
                    default { 1 }
                }
                $PartitionBytes = [int64]$Matches.count * $Multiplier
                break
            }
        }
    }
    $BinaryBytes = $null
    foreach ($Artifact in $Artifacts) {
        if ($Artifact.path -eq "build/agent.bin") { $BinaryBytes = [int64]$Artifact.size_bytes; break }
    }

    $ConfigSha = Get-Sha256 -Path "sdkconfig"
    $SourcePaths = @(
        "sdkconfig", "dependencies.lock", "main/display/lv_adapter_display.cc",
        "main/display/display_render_telemetry.cc", "main/display/display_cache_sync_plan.h",
        "main/display/agent_ui/components/render_snapshot_buffer.cc",
        "main/display/agent_ui/apps/codex/codex_ai_provider.cc",
        "patches/esp-lvgl-adapter-v0.6.3-system-performance.patch",
        "patches/esp-lvgl-adapter-v0.6.3-system-performance.manifest.json"
    )
    $SourceHashes = @(
        foreach ($SourcePath in $SourcePaths) {
            if (Test-Path -LiteralPath $SourcePath -PathType Leaf) {
                [pscustomobject]@{
                    path = $SourcePath.Replace("\", "/")
                    sha256 = Get-Sha256 -Path $SourcePath
                }
            }
        }
    )
    $CMakeCachePath = "build/CMakeCache.txt"
    $DirectRender = $null
    if (Test-Path -LiteralPath $CMakeCachePath -PathType Leaf) {
        $DirectLine = Select-String -LiteralPath $CMakeCachePath -Pattern '^AGENT_UI_EXPERIMENTAL_DIRECT_RENDER:BOOL=' | Select-Object -First 1
        if ($DirectLine) { $DirectRender = $DirectLine.Line.Split('=', 2)[1] }
    }
    $SpiramSettings = @(
        Select-String -LiteralPath "sdkconfig" -Pattern '^CONFIG_SPIRAM_(USE_MALLOC|MALLOC_ALWAYSINTERNAL|MALLOC_RESERVE_INTERNAL|ALLOW_STACK_EXTERNAL_MEMORY)=' |
            ForEach-Object { $_.Line }
    )
    $PreviousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $HeadHash = (& rtk proxy git rev-parse HEAD 2>$null).Trim()
        $HeadExitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $PreviousErrorActionPreference }
    if ($HeadExitCode -ne 0) { throw "git rev-parse failed with exit code $HeadExitCode" }

    $SectionMetrics = [ordered]@{
        iram0_text_bytes = [int64]$ElfSections[".iram0.text"]
        dram0_data_bytes = [int64]$ElfSections[".dram0.data"]
        dram0_bss_bytes = [int64]$ElfSections[".dram0.bss"]
        dram1_data_bytes = [int64]$ElfSections[".dram1.data"]
        dram1_bss_bytes = [int64]$ElfSections[".dram1.bss"]
        flash_text_bytes = [int64]$ElfSections[".flash.text"]
        flash_rodata_bytes = [int64]$ElfSections[".flash.rodata"]
    }
    $Summary = [ordered]@{
        captured_utc = [DateTime]::UtcNow.ToString("o")
        head = $HeadHash
        build_performed = [bool]$BuildFirmware
        sdkconfig_sha256 = $ConfigSha
        direct_render = $DirectRender
        spiram_settings = $SpiramSettings
        artifacts = $Artifacts
        elf_sections = $SectionMetrics
        elf_section_rows = $ElfSections
        app_partition_bytes = $PartitionBytes
        app_binary_bytes = $BinaryBytes
        app_partition_remaining_bytes = if ($null -ne $PartitionBytes -and $null -ne $BinaryBytes) { $PartitionBytes - $BinaryBytes } else { $null }
        source_config_sha256 = $SourceHashes
        runtime_stack_high_water = $null
        runtime_note = "Stack high-water and heap-cap free/min/largest values require sampled device logs; ELF/map sizes do not prove free heap."
    }

    $Summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory "system-performance.json")
    if (Test-Path -LiteralPath $CMakeCachePath -PathType Leaf) {
        Copy-Item -LiteralPath $CMakeCachePath -Destination (Join-Path $OutputDirectory "CMakeCache.txt") -Force
    }
    if (Test-Path -LiteralPath $PartitionTable -PathType Leaf) {
        Copy-Item -LiteralPath $PartitionTable -Destination (Join-Path $OutputDirectory "partition-table.csv") -Force
    }
    Write-Output (Join-Path $OutputDirectory "system-performance.json")
}
finally {
    Pop-Location
}
