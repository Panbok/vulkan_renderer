param(
    [string]$EvidenceRoot = ".scratch\evidence\v3-v4-windows",
    [int]$Repetitions = 3
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if ($Repetitions -lt 3) {
    throw "V3/V4 validation requires at least three fresh process repetitions"
}

function Invoke-Checked([string]$Label, [scriptblock]$Command) {
    Write-Host "== $Label =="
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Captured(
    [string]$Label,
    [string]$Executable,
    [string[]]$Arguments,
    [string]$LogPath
) {
    Write-Host "== $Label =="
    $lines = @(& $Executable @Arguments 2>&1 | Tee-Object -FilePath $LogPath)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code $exitCode`n$($lines -join [Environment]::NewLine)"
    }
    return $lines
}

Invoke-Checked "Windows CPU suite" { & cmd.exe /d /c build_test.bat }
Invoke-Checked "Windows bindless Vulkan V3/V4 Debug build" {
    & cmd.exe /d /c build_bindless_vulkan_v3.bat Debug
}
Invoke-Checked "Windows bindless Vulkan V3/V4 Release build" {
    & cmd.exe /d /c build_bindless_vulkan_v3.bat Release
}

function Resolve-V3Executable([string]$BuildType) {
    $buildRoot = "build_bindless_vulkan_v3_$BuildType"
    $candidate = Join-Path $repoRoot `
        "$buildRoot\tools\vkr_bindless_vulkan_v3.exe"
    if (-not (Test-Path $candidate)) {
        $candidate = Join-Path $repoRoot `
            "$buildRoot\tools\$BuildType\vkr_bindless_vulkan_v3.exe"
    }
    if (-not (Test-Path $candidate)) {
        throw "Bindless Vulkan V3/V4 $BuildType executable not found"
    }
    return $candidate
}

$debugExecutable = Resolve-V3Executable "Debug"
$releaseExecutable = Resolve-V3Executable "Release"

$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmss.fffZ")
$evidencePath = Join-Path (Join-Path $repoRoot $EvidenceRoot) $stamp
New-Item -ItemType Directory -Force $evidencePath | Out-Null

$releaseLog = Join-Path $evidencePath "offscreen-release.log"
$releaseLines = Invoke-Captured -Label "Release offscreen publication" `
    -Executable $releaseExecutable -Arguments @() -LogPath $releaseLog
$releaseText = $releaseLines -join [Environment]::NewLine
if ($releaseText -notmatch 'V3 WALKING OFFSCREEN PASS' -or
    $releaseText -notmatch 'V4 PUBLICATION PASS .*exact-draws=6 shared=1 replacement=1' -or
    $releaseText -notmatch 'sampler-shared=1 material-republish=1 upload-waits=0' -or
    $releaseText -match 'VUID-|\[ERROR\]:|\[FATAL\]:') {
    throw "Release offscreen run lacks required V3/V4 proof"
}

$publicationLines = @()
for ($run = 1; $run -le $Repetitions; ++$run) {
    $logPath = Join-Path $evidencePath ("offscreen-validation-{0}.log" -f $run)
    $lines = Invoke-Captured -Label "Offscreen validation repetition $run" `
        -Executable $debugExecutable -Arguments @("--validation") `
        -LogPath $logPath
    $text = $lines -join [Environment]::NewLine
    $publication = $lines | Where-Object { $_ -match '^V4 PUBLICATION PASS ' } |
        Select-Object -Last 1
    if (-not $publication -or
        $publication -notmatch 'exact-draws=6 shared=1 replacement=1' -or
        $publication -notmatch 'sampler-shared=1 material-republish=1 upload-waits=0' -or
        $publication -notmatch 'pending=0' -or
        $text -notmatch 'V3 WAITS command-slots=0' -or
        $text -notmatch 'V3 VALIDATION setup-notices=0 warnings=0 errors=0 gpu-assisted=disabled') {
        throw "Offscreen repetition $run lacks required V3/V4 proof"
    }
    if ($text -match 'VUID-|\[ERROR\]:|\[FATAL\]:') {
        throw "Offscreen repetition $run contains a validation or runtime diagnostic"
    }
    $publicationLines += $publication
}

if (($publicationLines | Select-Object -Unique).Count -ne 1) {
    throw "Repeated create/submit/destroy runs produced different publication totals"
}

$windowLog = Join-Path $evidencePath "windowed-validation.log"
$windowLines = Invoke-Captured -Label "Windowed validation" `
    -Executable $debugExecutable -Arguments @("--validation", "--windowed") `
    -LogPath $windowLog
$windowText = $windowLines -join [Environment]::NewLine
if ($windowText -notmatch 'V3 WALKING WINDOWED PASS' -or
    $windowText -notmatch 'V3 WSI PASS .*retired=1 collected=1 live=0' -or
    $windowText -notmatch 'V3 WAITS command-slots=0' -or
    $windowText -notmatch 'V3 VALIDATION setup-notices=0 warnings=0 errors=0 gpu-assisted=disabled' -or
    $windowText -match 'VUID-|\[ERROR\]:|\[FATAL\]:') {
    throw "Windowed validation lacks required V3/V4 WSI proof"
}

$gpuLog = Join-Path $evidencePath "gpu-assisted.log"
$gpuLines = Invoke-Captured -Label "GPU-assisted validation" `
    -Executable $debugExecutable -Arguments @("--gpu-assisted") `
    -LogPath $gpuLog
$gpuText = $gpuLines -join [Environment]::NewLine
if ($gpuText -notmatch 'V4 PUBLICATION PASS .*upload-waits=0' -or
    $gpuText -notmatch 'V3 VALIDATION setup-notices=3 warnings=0 errors=0 gpu-assisted=enabled' -or
    $gpuText -match 'VUID-|\[ERROR\]:|\[FATAL\]:') {
    throw "GPU-assisted validation lacks required V4 proof"
}

$summary = [ordered]@{
    status = "pass"
    platform = "Windows"
    cpu_suite = "pass"
    debug_build = "pass"
    release_build = "pass"
    release_offscreen = "pass"
    offscreen_validation_repetitions = $Repetitions
    publication_totals = $publicationLines[0]
    windowed_validation = "pass"
    gpu_assisted_validation = "pass"
    git_sha = (& git rev-parse HEAD).Trim()
    evidence_path = $evidencePath.Substring($repoRoot.Length + 1)
}
$summaryPath = Join-Path $evidencePath "v3-v4-windows-summary.json"
$summary | ConvertTo-Json | Set-Content -Encoding UTF8 $summaryPath
Write-Host ($summary | ConvertTo-Json -Compress)
