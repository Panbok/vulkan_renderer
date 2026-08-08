param(
    [string]$EvidenceRoot = ".scratch\evidence\v1-v2-windows"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Invoke-Checked([string]$Label, [scriptblock]$Command) {
    Write-Host "== $Label =="
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

Invoke-Checked "Windows CPU suite" { & cmd.exe /d /c build_test.bat }
Invoke-Checked "Windows Debug renderer and harness build" {
    & cmd.exe /d /c build.bat Debug
}

$env:VKR_HARNESS_RENDERER_BACKEND = "vulkan"
Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
$harness = Join-Path $repoRoot "build_debug\tools\vkr_harness.exe"
if (-not (Test-Path $harness)) {
    throw "Debug harness not found at $harness"
}

Write-Host "== Windows legacy-Vulkan startup/resize/shutdown validation =="
$harnessLines = @(& $harness profile `
    --case tools/cases/smoke/sponza_windowed_resize.case.json `
    --profile tools/profiles/local-windowed.json 2>&1 | Tee-Object -Variable output)
$harnessExit = $LASTEXITCODE
if ($harnessExit -ne 0) {
    throw "Harness failed with exit code $harnessExit`n$($harnessLines -join [Environment]::NewLine)"
}

$resultLine = $harnessLines | Where-Object { $_ -match '^\{' } |
    Select-Object -Last 1
if (-not $resultLine) {
    throw "Harness did not emit its final JSON result"
}
$result = $resultLine | ConvertFrom-Json
if ($result.status -ne "pass" -or $result.exit_code -ne 0) {
    throw "Harness aggregate did not pass: $resultLine"
}

$reportPath = Join-Path $repoRoot ($result.report -replace '/', '\')
$report = Get-Content -Raw $reportPath | ConvertFrom-Json
if ($report.status -ne "pass" -or $report.execution.completed_repetitions -lt 2) {
    throw "Aggregate report is incomplete: $reportPath"
}
if ($report.provenance.os -notmatch '^Windows') {
    throw "Report is not native Windows evidence: $($report.provenance.os)"
}
if (-not $report.effective_config.resize_round_trip) {
    throw "Report does not identify the resize-round-trip workload"
}

$runRoot = Split-Path -Parent $reportPath
foreach ($run in $report.runs) {
    if ($run.status -ne "pass") {
        throw "Child run $($run.index) did not pass"
    }
    $childRoot = Join-Path $runRoot ("runs\" + $run.index)
    $stdoutPath = Join-Path $childRoot "stdout.log"
    $stderrPath = Join-Path $childRoot "stderr.log"
    $stdout = Get-Content -Raw $stdoutPath
    $stderr = Get-Content -Raw $stderrPath
    if ($stdout -notmatch 'Validation layers supported' -or
        $stdout -notmatch 'Debug messenger created' -or
        $stdout -notmatch 'VKR_HARNESS_RESIZE_ROUND_TRIP_PASS' -or
        ([regex]::Matches($stdout, 'Recreating swapchain')).Count -lt 2 -or
        $stdout -notmatch 'Destroying debug messenger') {
        throw "Child run $($run.index) lacks startup/resize/shutdown proof"
    }
    if (($stdout + $stderr) -match 'VUID-|\[WARN\]:.*validation layer:|\[ERROR\]:|\[FATAL\]:') {
        throw "Child run $($run.index) contains validation or runtime diagnostics"
    }
}

$evidenceBase = Join-Path $repoRoot $EvidenceRoot
$evidencePath = Join-Path $evidenceBase $report.run_id
New-Item -ItemType Directory -Force $evidencePath | Out-Null
Copy-Item -Recurse -Force (Join-Path $runRoot '*') $evidencePath
$summary = [ordered]@{
    status = "pass"
    platform = "Windows"
    cpu_suite = "pass"
    debug_build = "pass"
    legacy_vulkan_validation = "pass"
    run_id = $report.run_id
    git_sha = $report.provenance.git_sha
    source_report = $result.report
    source_report_sha256 = $result.sha256
}
$summary | ConvertTo-Json | Set-Content -Encoding UTF8 `
    (Join-Path $evidencePath "v1-v2-windows-summary.json")
Write-Host ($summary | ConvertTo-Json -Compress)
