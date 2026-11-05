[CmdletBinding()]
param(
    [ValidateRange(10000,6000000)][int[]]$Entries = @(100000,300000),
    [ValidateSet('shallow','nested-hardlinks')][string]$Shape = 'shallow',
    [string]$OutputDirectory,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('logs/indexer/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output directory already exists: $OutputDirectory" }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$manifestPath = Join-Path $repoRoot 'indexer/Cargo.toml'
$binaryPath = Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
Push-Location $repoRoot
try {
    if (-not $SkipBuild) {
        & cargo test --manifest-path $manifestPath --locked --jobs 1 *> (Join-Path $OutputDirectory 'tests.log')
        if ($LASTEXITCODE -ne 0) { throw 'Indexer tests failed; see tests.log' }
        & cargo build --release --manifest-path $manifestPath --locked --jobs 1 *> (Join-Path $OutputDirectory 'build.log')
        if ($LASTEXITCODE -ne 0) { throw 'Indexer build failed; see build.log' }
    }
    if (-not (Test-Path -LiteralPath $binaryPath)) { throw "Binary missing: $binaryPath" }
    $binaryHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    [ordered]@{ schema=1; started=(Get-Date).ToUniversalTime().ToString('o'); mode='offline-only'; entries=$Entries; shape=$Shape; binary_sha256=$binaryHash } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'run.json') -Encoding utf8
    & $binaryPath metrics --seconds 10 --output (Join-Path $OutputDirectory 'baseline.jsonl') > (Join-Path $OutputDirectory 'baseline-report.json')
    if ($LASTEXITCODE -ne 0) { throw 'Baseline metrics failed' }
    foreach ($entryCount in $Entries) {
        $casePath = Join-Path $OutputDirectory "offline-$entryCount"
        & $binaryPath bench-offline --entries $entryCount --shape $Shape --output-dir $casePath > (Join-Path $OutputDirectory "offline-$entryCount-console.json")
        if ($LASTEXITCODE -ne 0) { throw "Offline benchmark failed/aborted: $casePath" }
        $report = Get-Content -LiteralPath (Join-Path $casePath 'report.json') -Raw | ConvertFrom-Json
        if ($report.status -ne 'completed' -or $report.native_volume_opens -ne 0 -or $report.directory_enumerations -ne 0) {
            throw "Unexpected benchmark status/access audit: $casePath"
        }
        if (-not $report.resource_guard_clean) {
            Write-Warning "Functional benchmark completed with $($report.resource_guard_violation_samples) resource-guard violation sample(s): $casePath"
        }
        Write-Host "$entryCount entries: general p95 $($report.result.general.p95_us) us; workshop p95 $($report.result.workshop.p95_us) us"
    }
    Write-Host "Raw six-dimension samples and reports: $OutputDirectory"
} finally { Pop-Location }
