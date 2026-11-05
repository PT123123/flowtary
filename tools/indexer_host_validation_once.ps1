# Run the validation matrix serially in one elevated PowerShell process and one UAC prompt.
param(
    [string]$RunDirectory,
    [switch]$VerifyInitialResume,
    [switch]$CaptureFileRecordPayload,
    [switch]$DirectoryLinkWalk,
    [string]$Scenarios,
    [ValidateRange(0,10000)][int]$ScaleFixtureCount = 0,
    [ValidateRange(8,30)][int]$WatchSeconds = 8,
    [switch]$ElevatedChild,
    [switch]$PlanOnly,
    [switch]$CapturePoolTrace
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $RunDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $RunDirectory = Join-Path $repoRoot "logs/indexer/host-validation-$stamp"
}
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer')) + [IO.Path]::DirectorySeparatorChar
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
if (-not $RunDirectory.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunDirectory must stay inside logs/indexer.'
}
if (Test-Path -LiteralPath $RunDirectory) { throw 'Choose a new run directory; existing VHD runs are never reused.' }

$harness = Join-Path $PSScriptRoot 'indexer_host_vhd.ps1'
$pwshCommand = Get-Command pwsh.exe -ErrorAction SilentlyContinue
if (-not $pwshCommand) { throw 'PowerShell 7 is required for the host validation.' }
$pwsh = $pwshCommand.Source
$scenariosWereSpecified = $PSBoundParameters.ContainsKey('Scenarios')
if (-not $scenariosWereSpecified) {
    if ($DirectoryLinkWalk) { $scenarioList = @('directory-link-walk') }
    elseif ($VerifyInitialResume -or $CaptureFileRecordPayload) { $scenarioList = @('usn-fallback-resume') }
    else { $scenarioList = @('file-layout','usn-fallback-resume','directory-link-walk') }
} elseif ($VerifyInitialResume -or $DirectoryLinkWalk) {
    throw 'Use either -Scenarios or the legacy single-scenario switches, not both.'
} else {
    $scenarioList = @($Scenarios -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
$allowedScenarios = @('file-layout','usn-fallback-resume','directory-link-walk')
$unknownScenarios = @($scenarioList | Where-Object { $_ -notin $allowedScenarios })
if ($unknownScenarios.Count) { throw "Unknown validation scenario(s): $($unknownScenarios -join ', ')" }
if ($scenarioList.Count -eq 0 -or @($scenarioList | Select-Object -Unique).Count -ne $scenarioList.Count) {
    throw 'Choose one or more unique validation scenarios.'
}
if ($CaptureFileRecordPayload -and $scenarioList -notcontains 'usn-fallback-resume') {
    throw '-CaptureFileRecordPayload requires the usn-fallback-resume scenario.'
}
if ($PlanOnly) {
    [ordered]@{run_directory=$RunDirectory;scenarios=@($scenarioList);scale_fixture_count=$ScaleFixtureCount;
        watch_seconds=$WatchSeconds;capture_pool_trace=[bool]$CapturePoolTrace;uac_requests=0;volume_access=$false} | ConvertTo-Json -Depth 5
    return
}
$logPrefix = Join-Path (Split-Path -Parent $RunDirectory) (Split-Path -Leaf $RunDirectory)
$launchLog = "$logPrefix-launch.json"
$tokenLog = "$logPrefix-token.json"
$childLog = "$logPrefix-child.json"
$resultLog = "$logPrefix-result.json"
if ($ElevatedChild) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    @{ phase='elevated-child-token'; identity=$identity.Name;
        is_administrator=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator);
        run_directory=$RunDirectory; updated_utc=(Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $tokenLog -Encoding utf8
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Elevated child did not receive an administrator token.'
    }
    $null = New-Item -ItemType Directory -Path $RunDirectory -Force
    $batchResults = [Collections.Generic.List[object]]::new()
    try {
        foreach ($scenario in $scenarioList) {
            $scenarioDirectory = Join-Path $RunDirectory $scenario
            $harnessArgs = [Collections.Generic.List[string]]::new()
            $harnessArgs.Add('-NoProfile'); $harnessArgs.Add('-ExecutionPolicy'); $harnessArgs.Add('Bypass')
            $harnessArgs.Add('-File'); $harnessArgs.Add($harness)
            $harnessArgs.Add('-RunDirectory'); $harnessArgs.Add($scenarioDirectory)
            $harnessArgs.Add('-WatchSeconds'); $harnessArgs.Add([string]$WatchSeconds)
            if ($CapturePoolTrace) { $harnessArgs.Add('-CapturePoolTrace') }
            $scenarioScaleCount = if ($scenario -eq 'directory-link-walk') { $ScaleFixtureCount } else { 0 }
            $harnessArgs.Add('-ScaleFixtureCount'); $harnessArgs.Add([string]$scenarioScaleCount)
            if ($scenario -ne 'file-layout') { $harnessArgs.Add('-VerifyInitialResume') }
            if ($scenario -eq 'directory-link-walk') { $harnessArgs.Add('-DirectoryLinkWalk') }
            if ($CaptureFileRecordPayload -and $scenario -eq 'usn-fallback-resume') { $harnessArgs.Add('-CaptureFileRecordPayload') }

            $stdoutPath = Join-Path $RunDirectory "$scenario-harness-stdout.log"
            $stderrPath = Join-Path $RunDirectory "$scenario-harness-stderr.log"
            @{phase='scenario-starting';scenario=$scenario;run_directory=$scenarioDirectory;
                updated_utc=(Get-Date).ToUniversalTime().ToString('o')} |
                ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunDirectory 'batch-state.json') -Encoding utf8
            $startInfo = [Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $pwsh
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            foreach ($argument in $harnessArgs) { $startInfo.ArgumentList.Add($argument) }
            $harnessProcess = [Diagnostics.Process]::new()
            $harnessProcess.StartInfo = $startInfo
            $null = $harnessProcess.Start()
            $stdoutTask = $harnessProcess.StandardOutput.ReadToEndAsync()
            $stderrTask = $harnessProcess.StandardError.ReadToEndAsync()
            $harnessProcess.WaitForExit()
            $stdoutTask.Result | Set-Content -LiteralPath $stdoutPath -Encoding utf8
            $stderrTask.Result | Set-Content -LiteralPath $stderrPath -Encoding utf8
            $scenarioExitCode = $harnessProcess.ExitCode
            $harnessProcess.Dispose()
            $batchResults.Add(@{scenario=$scenario;run_directory=$scenarioDirectory;exit_code=$scenarioExitCode;
                stdout=$stdoutPath;stderr=$stderrPath})
            @{phase='scenario-finished';scenario=$scenario;exit_code=$scenarioExitCode;
                updated_utc=(Get-Date).ToUniversalTime().ToString('o')} |
                ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunDirectory 'batch-state.json') -Encoding utf8
            if ($scenarioExitCode -ne 0) { throw "Scenario '$scenario' failed with exit code $scenarioExitCode; remaining scenarios were stopped." }
        }
        @{phase='batch-completed';scenarios=@($scenarioList);results=@($batchResults);
            updated_utc=(Get-Date).ToUniversalTime().ToString('o')} |
            ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RunDirectory 'batch-result.json') -Encoding utf8
        exit 0
    } catch {
        @{ phase='elevated-child-exception'; error=$_.ToString(); updated_utc=(Get-Date).ToUniversalTime().ToString('o') } |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $childLog -Encoding utf8
        @{phase='batch-failed';scenarios=@($scenarioList);completed=@($batchResults);error=$_.ToString();
            updated_utc=(Get-Date).ToUniversalTime().ToString('o')} |
            ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RunDirectory 'batch-result.json') -Encoding utf8
        throw
    }
}

$preflightDirectory = "$RunDirectory-preflight"
if (Test-Path -LiteralPath $preflightDirectory) { throw "Preflight directory already exists: $preflightDirectory" }
$null = New-Item -ItemType Directory -Path $preflightDirectory
$binary = Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$preflightReportPath = Join-Path $preflightDirectory 'report.json'
$preflightSamplesPath = Join-Path $preflightDirectory 'samples.jsonl'
$preflightResultPath = Join-Path (Split-Path -Parent $RunDirectory) ((Split-Path -Leaf $RunDirectory) + '-preflight-result.json')
try {
    if (-not (Test-Path -LiteralPath $binary)) { throw 'Release indexer binary is missing; no UAC was requested.' }
    & $binary metrics --seconds 5 --interval-ms 1000 --output $preflightSamplesPath > $preflightReportPath
    if ($LASTEXITCODE -ne 0) { throw "Metrics preflight failed with exit code $LASTEXITCODE; no UAC was requested." }
    $preflight = Get-Content -LiteralPath $preflightReportPath -Raw | ConvertFrom-Json
    $phase = @($preflight.phases | Where-Object phase -eq 'machine_observation' | Select-Object -Last 1)[0]
    $sample = $preflight.last
    $preflightSamples = @(Get-Content -LiteralPath $preflightSamplesPath | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
    $baselineSample = @($preflightSamples | Where-Object phase -eq 'baseline' | Select-Object -First 1)[0]
    $reasons = [Collections.Generic.List[string]]::new()
    if (-not $phase -or $phase.samples -lt 5) { $reasons.Add('whole-machine baseline did not cover five seconds and five samples') }
    elseif ($phase.counter_error_samples -ne 0) { $reasons.Add('one or more required machine counters were unavailable') }
    if (-not $sample -or $null -eq $sample.system_handles -or $null -eq $sample.pool_paged_bytes -or
        $null -eq $sample.pool_nonpaged_bytes -or $null -eq $sample.disk_busy_percent -or
        $null -eq $sample.cpu_percent -or $null -eq $sample.physical_available_bytes -or
        $null -eq $sample.commit_limit_bytes -or $null -eq $sample.memory_pressure_ratio -or
        -not $phase -or $null -eq $phase.max_pages_input_per_s) {
        $reasons.Add('one or more required whole-machine metrics are missing')
    }
    if (-not $baselineSample -or $null -eq $baselineSample.system_handles -or
        $null -eq $baselineSample.pool_paged_bytes -or $null -eq $baselineSample.pool_nonpaged_bytes) {
        $reasons.Add('System handle or kernel-pool baseline sample is missing')
    }
    if ($phase) {
        if ($phase.maximum_commit_ratio -gt 0.85) { $reasons.Add('commit ratio is above 85%') }
        if ($phase.maximum_pressure_ratio -gt 0.90) { $reasons.Add('memory pressure exceeded 90% during preflight') }
        if ($phase.minimum_available_bytes -lt 1.5GB) { $reasons.Add('available physical memory is below 1.5 GiB') }
        if ($phase.max_pages_input_per_s -gt 10000) { $reasons.Add('paging input exceeded 10,000 pages/sec during preflight') }
        if ($phase.mean_cpu_percent -gt 85) { $reasons.Add('mean machine CPU exceeded 85% during preflight') }
        if ($phase.mean_disk_busy_percent -gt 90) { $reasons.Add('mean disk busy exceeded 90% during preflight') }
        if ($baselineSample) {
            if ($null -ne $phase.maximum_commit_bytes -and $null -ne $baselineSample.commit_bytes -and
                $phase.maximum_commit_bytes - $baselineSample.commit_bytes -gt 1GB) {
                $reasons.Add('machine commit growth exceeded 1 GiB during preflight')
            }
            if ($null -ne $phase.maximum_system_handles -and $null -ne $baselineSample.system_handles -and
                $phase.maximum_system_handles - $baselineSample.system_handles -gt 512) {
                $reasons.Add('System handle growth exceeded 512 during preflight')
            }
            if ($null -ne $phase.maximum_paged_pool_bytes -and $null -ne $baselineSample.pool_paged_bytes -and
                $phase.maximum_paged_pool_bytes - $baselineSample.pool_paged_bytes -gt 256MB) {
                $reasons.Add('paged-pool growth exceeded 256 MiB during preflight')
            }
            if ($null -ne $phase.maximum_nonpaged_pool_bytes -and $null -ne $baselineSample.pool_nonpaged_bytes -and
                $phase.maximum_nonpaged_pool_bytes - $baselineSample.pool_nonpaged_bytes -gt 128MB) {
                $reasons.Add('nonpaged-pool growth exceeded 128 MiB during preflight')
            }
        }
    }
    $preflightOutcome = [ordered]@{ phase=if ($reasons.Count) {'rejected-before-uac'} else {'accepted-before-uac'};
        reasons=@($reasons); report=$preflightReportPath; samples=$preflightSamplesPath;
        updated_utc=(Get-Date).ToUniversalTime().ToString('o') }
    $preflightOutcome | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $preflightResultPath -Encoding utf8
    if ($reasons.Count) { throw "Host preflight failed; no UAC was requested: $($reasons -join '; '). Review $preflightReportPath." }
} catch {
    if (-not (Test-Path -LiteralPath $preflightResultPath)) {
        @{phase='preflight-error-before-uac';error=$_.ToString();report=$preflightReportPath;
            updated_utc=(Get-Date).ToUniversalTime().ToString('o')} |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $preflightResultPath -Encoding utf8
    }
    throw
}
$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath),
    '-ElevatedChild', '-RunDirectory', ('"{0}"' -f $RunDirectory), '-WatchSeconds', [string]$WatchSeconds,
    '-Scenarios', ($scenarioList -join ','))
if ($ScaleFixtureCount -gt 0) { $argList += @('-ScaleFixtureCount', [string]$ScaleFixtureCount) }
if ($CaptureFileRecordPayload) { $argList += '-CaptureFileRecordPayload' }
if ($CapturePoolTrace) { $argList += '-CapturePoolTrace' }

@{ phase='launching-elevated-child'; executable=$pwsh; arguments=$argList; run_directory=$RunDirectory;
    preflight_result=$preflightResultPath;
    elevation_mode=if ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {'inherited-administrator-token'} else {'single-runas-request'};
    updated_utc=(Get-Date).ToUniversalTime().ToString('o') } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $launchLog -Encoding utf8
Write-Host "Starting one elevated validation run. Results: $RunDirectory"
try {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $pwsh
        $startInfo.WorkingDirectory = $repoRoot
        $startInfo.UseShellExecute = $false
        foreach ($argument in $argList) { $startInfo.ArgumentList.Add($argument) }
        $process = [Diagnostics.Process]::Start($startInfo)
    } else {
        $process = Start-Process -FilePath $pwsh -Verb RunAs -PassThru -ArgumentList $argList
    }
    if (-not $process) { throw 'The validation child did not start.' }
    $childPid=$process.Id
    @{ phase='elevated-child-started'; child_pid=$childPid; run_directory=$RunDirectory;
        updated_utc=(Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultLog -Encoding utf8
    while (-not $process.HasExited) {
        Start-Sleep -Milliseconds 500
        $process.Refresh()
    }
    $exitCode=$process.ExitCode
    $process.Dispose()
} catch {
    @{ phase='elevated-start-failed'; error=$_.ToString(); run_directory=$RunDirectory;
        updated_utc=(Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultLog -Encoding utf8
    throw
}
@{ phase='elevated-child-exited'; exit_code=$exitCode; child_pid=$childPid; run_directory=$RunDirectory;
    child_log=$childLog; updated_utc=(Get-Date).ToUniversalTime().ToString('o') } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultLog -Encoding utf8
if ($exitCode -ne 0) { throw "Elevated validation failed with exit code $exitCode. Review $launchLog, $childLog, $resultLog, and $RunDirectory state files." }
Write-Host "Validation completed. Review $RunDirectory/state.json and machine metrics."
