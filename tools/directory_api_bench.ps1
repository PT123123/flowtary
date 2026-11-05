[CmdletBinding()]
param(
    [ValidateRange(100,10000)][int]$Entries = 10000,
    [ValidateRange(1,100)][int]$Directories = 100,
    [string]$OutputDirectory,
    [ValidateRange(0,20)][int]$RepeatSeconds = 0,
    [ValidateRange(0,10)][int]$ActiveSeconds = 0,
    [ValidateRange(0,10)][int]$PauseSeconds = 0,
    [ValidateRange(0,10)][int]$ResumeSeconds = 0,
    [switch]$CapturePoolTrace
)
$ErrorActionPreference = 'Stop'
$phaseScheduleRequested = $ActiveSeconds -gt 0 -or $PauseSeconds -gt 0 -or $ResumeSeconds -gt 0
if ($phaseScheduleRequested -and ($RepeatSeconds -gt 0 -or $ActiveSeconds -eq 0 -or $PauseSeconds -eq 0 -or $ResumeSeconds -eq 0 -or
    ($ActiveSeconds + $PauseSeconds + $ResumeSeconds) -gt 20)) {
    throw 'Use either -RepeatSeconds or a complete active/pause/resume schedule with a total duration no greater than 20 seconds.'
}
$scheduledSeconds = if ($phaseScheduleRequested) { $ActiveSeconds + $PauseSeconds + $ResumeSeconds } else { $RepeatSeconds }
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$logsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer')) + [IO.Path]::DirectorySeparatorChar
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot ('logs/indexer/directory-api-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $OutputDirectory.StartsWith($logsRoot,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $OutputDirectory)) {
    throw 'OutputDirectory must be a new directory inside logs/indexer.'
}
$null = New-Item -ItemType Directory -Path $OutputDirectory
$fixtureRoot = Join-Path $OutputDirectory 'fixture'
$markerPath = Join-Path $fixtureRoot '.flowtary-dirapi-fixture'
$metricsExe = Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$probeExe = Join-Path $repoRoot 'build/directory-api-probe.exe'
if (-not (Test-Path -LiteralPath $metricsExe)) { throw 'Release metrics sampler is missing.' }
if (-not (Test-Path -LiteralPath $probeExe)) { throw 'Build the directory-api-probe target first.' }

function Start-Redirected([string]$File,[string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new(); $info.FileName = $File; $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info; $null = $process.Start()
    return $process
}

$target = $null; $sampler = $null; $startEvent = $null; $stopEvent = $null
$xperfPath = $null; $poolTraceStarted = $false
$poolLoggerName = "FlowtaryDirApiPool-$([Guid]::NewGuid().ToString('N'))"
$poolKernelTrace = Join-Path $OutputDirectory 'pool-kernel.etl'
$poolMergedTrace = Join-Path $OutputDirectory 'pool-trace.etl'
$poolSummary = Join-Path $OutputDirectory 'pool-summary.txt'
try {
    $null = New-Item -ItemType Directory -Path $fixtureRoot
    [IO.File]::WriteAllText($markerPath,'FLOWTARY-DIRAPI-FIXTURE-V1',[Text.Encoding]::ASCII)
    for ($d = 0; $d -lt $Directories; $d++) { $null = New-Item -ItemType Directory -Path (Join-Path $fixtureRoot ('d{0:D3}' -f $d)) }
    for ($i = 0; $i -lt $Entries; $i++) {
        $directory = Join-Path $fixtureRoot ('d{0:D3}' -f ($i % $Directories))
        [IO.File]::WriteAllBytes((Join-Path $directory ('item-{0:D6}.exe' -f $i)),[byte[]]::new(0))
    }
    [ordered]@{ schema=1; entries=$Entries; directories=$Directories; root=$fixtureRoot; setup='explicit empty .exe files; no file contents read by probe' } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'fixture.json') -Encoding utf8

    $preflight = Start-Redirected $metricsExe @('metrics','--seconds','5','--output',(Join-Path $OutputDirectory 'preflight.jsonl'))
    $preflightOut = $preflight.StandardOutput.ReadToEndAsync(); $preflightErr = $preflight.StandardError.ReadToEndAsync()
    if (-not $preflight.WaitForExit(15000)) { $preflight.Kill(); throw 'Metrics preflight exceeded 15 seconds.' }
    if ($preflight.ExitCode -ne 0) { throw "Metrics preflight failed: $($preflightErr.Result)" }
    $preflightReport = $preflightOut.Result | ConvertFrom-Json
    $phase = @($preflightReport.phases | Where-Object phase -eq machine_observation | Select-Object -Last 1)[0]
    $reasons = [Collections.Generic.List[string]]::new()
    if (-not $phase -or $phase.samples -lt 5 -or $phase.counter_error_samples -ne 0) { $reasons.Add('six-metric preflight coverage/counters invalid') }
    if ($phase) {
        if ($phase.maximum_commit_ratio -gt .85) { $reasons.Add('commit ratio exceeded 85%') }
        if ($phase.maximum_pressure_ratio -gt .90) { $reasons.Add('memory pressure exceeded 90%') }
        if ($phase.minimum_available_bytes -lt 1.5GB) { $reasons.Add('available memory fell below 1.5 GiB') }
        if ($phase.max_pages_input_per_s -gt 10000) { $reasons.Add('paging input exceeded 10,000 pages/sec') }
        if ($phase.mean_cpu_percent -gt 85) { $reasons.Add('CPU exceeded 85%') }
        if ($phase.mean_disk_busy_percent -gt 90) { $reasons.Add('disk busy exceeded 90%') }
    }
    $preflightReport | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'preflight-report.json') -Encoding utf8
    if ($reasons.Count) { throw "Host preflight rejected workload: $($reasons -join '; ')" }

    if ($CapturePoolTrace) {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Pool-tag ETW capture requires an already elevated PowerShell session. This script will not request UAC.'
        }
        $xperfCommand = Get-Command xperf.exe -ErrorAction SilentlyContinue
        if (-not $xperfCommand) { throw 'WPT xperf.exe is required for pool tag capture.' }
        $xperfPath = $xperfCommand.Source
        # A unique named session avoids replacing the Windows-owned NT Kernel Logger.
        & $xperfPath -start $poolLoggerName -on 'PROC_THREAD+LOADER+POOL' -BufferSize 64 -MinBuffers 8 -MaxBuffers 64 -MaxFile 128 -FileMode Circular `
            -f $poolKernelTrace -StackWalk 'PoolAlloc+PoolFree' 2>&1 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'pool-trace-start.log') -Encoding utf8
        if ($LASTEXITCODE -ne 0) {
            & $xperfPath -stop $poolLoggerName (Join-Path $OutputDirectory 'pool-start-failed.etl') 2>&1 |
                Set-Content -LiteralPath (Join-Path $OutputDirectory 'pool-trace-start-cleanup.log') -Encoding utf8
            throw "xperf could not start the bounded named kernel pool trace '$poolLoggerName'; no UAC was requested."
        }
        $poolTraceStarted = $true
    }

    $token = [Guid]::NewGuid().ToString('N')
    $startName = "Local\Flowtary-DirApi-Start-$token"; $stopName = "Local\Flowtary-DirApi-Stop-$token"
    $startEvent = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$startName)
    $stopEvent = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$stopName)
    $args = @('--root',$fixtureRoot,'--output',(Join-Path $OutputDirectory 'probe.json'),'--start-event',$startName,'--stop-event',$stopName)
    if ($phaseScheduleRequested) {
        $args += @('--active-ms',[string]($ActiveSeconds * 1000),'--pause-ms',[string]($PauseSeconds * 1000),'--resume-ms',[string]($ResumeSeconds * 1000))
    } elseif ($RepeatSeconds -gt 0) { $args += @('--duration-ms',[string]($RepeatSeconds * 1000)) }
    $target = Start-Redirected $probeExe $args
    $targetOut = $target.StandardOutput.ReadToEndAsync(); $targetErr = $target.StandardError.ReadToEndAsync()
    $sampleSeconds = 8 + $scheduledSeconds
    $sampler = Start-Redirected $metricsExe @('metrics','--seconds',[string]$sampleSeconds,'--pid',"$($target.Id)",'--output',(Join-Path $OutputDirectory 'samples.jsonl'))
    $samplerOut = $sampler.StandardOutput.ReadToEndAsync(); $samplerErr = $sampler.StandardError.ReadToEndAsync()
    Start-Sleep -Seconds 3
    $null = $startEvent.Set()
    if (-not $sampler.WaitForExit(($sampleSeconds + 10) * 1000)) { $sampler.Kill(); throw 'Six-metric workload window exceeded its deadline.' }
    if ($sampler.ExitCode -ne 0) { throw "Six-metric sampler failed: $($samplerErr.Result)" }
    $metrics = $samplerOut.Result | ConvertFrom-Json
    $metrics | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'metrics-report.json') -Encoding utf8
    $null = $stopEvent.Set()
    if (-not $target.WaitForExit(5000)) { throw 'Directory API probe did not exit after sampler release.' }
    if ($target.ExitCode -ne 0) { throw "Directory API probe failed ($($target.ExitCode)): $($targetErr.Result)" }
    if ($poolTraceStarted) {
        & $xperfPath -stop $poolLoggerName $poolMergedTrace 2>&1 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'pool-trace-stop.log') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw 'xperf failed to stop/merge the bounded kernel pool trace.' }
        $poolTraceStarted = $false
        & $xperfPath -i $poolMergedTrace -o $poolSummary -a pool -pooltags -paged -nonpaged -top 100 2>&1 |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'pool-analysis.log') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw 'xperf could not summarize pool tags from the trace.' }
    }
    $probe = Get-Content -LiteralPath (Join-Path $OutputDirectory 'probe.json') -Raw | ConvertFrom-Json
    $sampleRows = @(Get-Content -LiteralPath (Join-Path $OutputDirectory 'samples.jsonl') |
        Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
    $workloadStart = [long]$probe.workload_start_unix_ms
    $workloadEnd = [long]$probe.workload_end_unix_ms
    $baselineCandidates = @($sampleRows | Where-Object { [long]$_.unix_ms -le $workloadStart })
    $scanSamples = @($sampleRows | Where-Object { [long]$_.unix_ms -gt $workloadStart -and [long]$_.unix_ms -le $workloadEnd })
    $phaseAligned = @()
    function Get-PhaseObservation([string]$Name,[long]$Start,[long]$End) {
        $before = @($sampleRows | Where-Object { [long]$_.unix_ms -le $Start })
        if ($before.Count -eq 0 -or $End -le $Start) { return $null }
        $reference = $before[-1]
        $inside = @($sampleRows | Where-Object { [long]$_.unix_ms -gt $Start -and [long]$_.unix_ms -le $End })
        if ($inside.Count -eq 0) { return $null }
        $counterErrors = @($inside | Where-Object { $_.counter_errors -and $_.counter_errors.PSObject.Properties.Count -gt 0 }).Count
        return [ordered]@{
            phase=$Name; sample_interval_ms=1000; boundary_tolerance_ms=1000; sample_count=$inside.Count; counter_error_samples=$counterErrors;
            baseline_unix_ms=[long]$reference.unix_ms; phase_start_unix_ms=$Start; phase_end_unix_ms=$End;
            commit_peak_delta_bytes=([long](($inside | Measure-Object commit_bytes -Maximum).Maximum) - [long]$reference.commit_bytes);
            commit_end_delta_bytes=([long]$inside[-1].commit_bytes - [long]$reference.commit_bytes);
            memory_pressure_peak_ratio=(($inside | Measure-Object memory_pressure_ratio -Maximum).Maximum);
            physical_available_min_bytes=(($inside | Measure-Object physical_available_bytes -Minimum).Minimum);
            system_handle_peak_delta=([long](($inside | Measure-Object system_handles -Maximum).Maximum) - [long]$reference.system_handles);
            system_handle_end_delta=([long]$inside[-1].system_handles - [long]$reference.system_handles);
            paged_pool_peak_delta_bytes=([long](($inside | Measure-Object pool_paged_bytes -Maximum).Maximum) - [long]$reference.pool_paged_bytes);
            paged_pool_end_delta_bytes=([long]$inside[-1].pool_paged_bytes - [long]$reference.pool_paged_bytes);
            nonpaged_pool_peak_delta_bytes=([long](($inside | Measure-Object pool_nonpaged_bytes -Maximum).Maximum) - [long]$reference.pool_nonpaged_bytes);
            nonpaged_pool_end_delta_bytes=([long]$inside[-1].pool_nonpaged_bytes - [long]$reference.pool_nonpaged_bytes);
            cpu_mean_percent=(($inside | Measure-Object cpu_percent -Average).Average);
            disk_busy_mean_percent=(($inside | Measure-Object disk_busy_percent -Average).Average);
            disk_read_write_mean_bytes_per_s=(($inside | ForEach-Object { [double]$_.disk_read_bytes_per_s + [double]$_.disk_write_bytes_per_s } | Measure-Object -Average).Average);
            pages_input_peak_per_s=(($inside | Measure-Object pages_input_per_s -Maximum).Maximum)
        }
    }
    if ($phaseScheduleRequested) {
        $phaseAligned = @(
            (Get-PhaseObservation 'scan-initial' $workloadStart ([long]$probe.phase1_end_unix_ms)),
            (Get-PhaseObservation 'paused' ([long]$probe.pause_start_unix_ms) ([long]$probe.pause_end_unix_ms)),
            (Get-PhaseObservation 'scan-resumed' ([long]$probe.phase2_start_unix_ms) $workloadEnd)
        ) | Where-Object { $_ }
        $scanStageRows = @($phaseAligned | Where-Object { $_.phase -like 'scan-*' })
        if ($scanStageRows.Count -ne 2 -or @($scanStageRows | Where-Object { $_.sample_count -lt 2 -or $_.counter_error_samples -ne 0 }).Count -ne 0) {
            throw 'The active/pause/resume schedule did not produce two complete, counter-clean scan windows.'
        }
    }
    if ($baselineCandidates.Count -gt 0) {
        $scanBaseline = $baselineCandidates[-1]
        $alignedSamples = @($sampleRows | Where-Object {
            [long]$_.unix_ms -ge [long]$scanBaseline.unix_ms -and [long]$_.unix_ms -le ($workloadEnd + 1000)
        })
        $counterErrors = @($alignedSamples | Where-Object { $_.counter_errors -and $_.counter_errors.PSObject.Properties.Count -gt 0 }).Count
        $scanAligned = [ordered]@{
            sample_interval_ms=1000; boundary_tolerance_ms=1000; scan_sample_count=$scanSamples.Count;
            total_aligned_sample_count=$alignedSamples.Count; counter_error_samples=$counterErrors;
            baseline_unix_ms=[long]$scanBaseline.unix_ms; workload_start_unix_ms=$workloadStart; workload_end_unix_ms=$workloadEnd;
            commit_peak_delta_bytes=([long](($alignedSamples | Measure-Object commit_bytes -Maximum).Maximum) - [long]$scanBaseline.commit_bytes);
            commit_end_delta_bytes=([long]$alignedSamples[-1].commit_bytes - [long]$scanBaseline.commit_bytes);
            memory_pressure_peak_ratio=(($alignedSamples | Measure-Object memory_pressure_ratio -Maximum).Maximum);
            physical_available_min_bytes=(($alignedSamples | Measure-Object physical_available_bytes -Minimum).Minimum);
            system_handle_peak_delta=([long](($alignedSamples | Measure-Object system_handles -Maximum).Maximum) - [long]$scanBaseline.system_handles);
            system_handle_end_delta=([long]$alignedSamples[-1].system_handles - [long]$scanBaseline.system_handles);
            paged_pool_peak_delta_bytes=([long](($alignedSamples | Measure-Object pool_paged_bytes -Maximum).Maximum) - [long]$scanBaseline.pool_paged_bytes);
            paged_pool_end_delta_bytes=([long]$alignedSamples[-1].pool_paged_bytes - [long]$scanBaseline.pool_paged_bytes);
            nonpaged_pool_peak_delta_bytes=([long](($alignedSamples | Measure-Object pool_nonpaged_bytes -Maximum).Maximum) - [long]$scanBaseline.pool_nonpaged_bytes);
            nonpaged_pool_end_delta_bytes=([long]$alignedSamples[-1].pool_nonpaged_bytes - [long]$scanBaseline.pool_nonpaged_bytes);
            cpu_mean_percent=(($scanSamples | Measure-Object cpu_percent -Average).Average);
            disk_busy_mean_percent=(($scanSamples | Measure-Object disk_busy_percent -Average).Average);
            disk_read_write_mean_bytes_per_s=(($scanSamples | ForEach-Object { [double]$_.disk_read_bytes_per_s + [double]$_.disk_write_bytes_per_s } | Measure-Object -Average).Average);
            pages_input_peak_per_s=(($scanSamples | Measure-Object pages_input_per_s -Maximum).Maximum)
        }
        if ($RepeatSeconds -gt 0 -and $scanSamples.Count -lt [Math]::Max(2,$RepeatSeconds - 2)) {
            throw "Only $($scanSamples.Count) one-second machine samples overlap the $RepeatSeconds-second scan."
        }
        if ($counterErrors -ne 0) { throw 'One or more aligned scan-window samples had counter errors.' }
    } else {
        $scanAligned = $null
    }
    $expectedPasses = [int]$probe.passes
    if (-not $probe.complete -or $expectedPasses -lt 1 -or $probe.entries -ne (($Entries + $Directories) * $expectedPasses) -or
        $probe.files -ne ($Entries * $expectedPasses) -or $probe.directories -ne ($Directories * $expectedPasses) -or
        $probe.exe_files -ne ($Entries * $expectedPasses) -or $probe.file_id_mismatches -ne 0 -or
        $probe.directory_handles.active_end -ne 0 -or $probe.directory_handles.peak_active -ne 1) {
        throw 'Directory API completeness, ID identity, or handle-balance assertion failed.'
    }
    $counterErrorCount = @($metrics.last.counter_errors.PSObject.Properties).Count
    if ($metrics.last.monitored_pid -ne $probe.pid -or $probe.pid -ne $target.Id -or $counterErrorCount -ne 0) { throw 'Whole-machine sampling was incomplete.' }
    [ordered]@{
        schema=1; mode='bounded-directory-api-six-dimension-benchmark';
        scope='explicit marked test tree; no volume device, no ordinary-file opens, no file-content reads';
        requested_repeat_seconds=$RepeatSeconds; completed_passes=$expectedPasses;
        phase_schedule=if($phaseScheduleRequested){@{active_seconds=$ActiveSeconds;pause_seconds=$PauseSeconds;resume_seconds=$ResumeSeconds}}else{$null};
        probe=$probe; workload_envelope_observation=$scanAligned;
        scan_aligned_observation=if($phaseScheduleRequested){$null}else{$scanAligned};
        phase_aligned_observations=@($phaseAligned); whole_machine_phases=$metrics.phases; counter_errors=$metrics.last.counter_errors;
        pool_trace=if($CapturePoolTrace){@{logger_name=$poolLoggerName;profile='PROC_THREAD+LOADER+POOL';stackwalk='PoolAlloc+PoolFree';buffer_size_kib=64;maximum_buffers=64;maximum_file_mib=128;trace=$poolMergedTrace;summary=$poolSummary}}else{$null};
        native_host_violations=$metrics.native_host_violations;
        probe_sha256=(Get-FileHash -LiteralPath $probeExe -Algorithm SHA256).Hash;
        metrics_sha256=(Get-FileHash -LiteralPath $metricsExe -Algorithm SHA256).Hash
    } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'report.json') -Encoding utf8
    Get-Content -LiteralPath (Join-Path $OutputDirectory 'report.json') -Raw
} finally {
    if ($poolTraceStarted -and $xperfPath) {
        & $xperfPath -stop $poolLoggerName $poolMergedTrace 2>&1 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'pool-trace-stop.log') -Encoding utf8
        $poolTraceStarted = $false
    }
    if ($startEvent) { $null = $startEvent.Set() }
    if ($stopEvent) { $null = $stopEvent.Set() }
    foreach ($process in @($target,$sampler)) {
        if ($process) { if (-not $process.HasExited) { $process.Kill(); $null = $process.WaitForExit(3000) }; $process.Dispose() }
    }
    if ($startEvent) { $startEvent.Dispose() }
    if ($stopEvent) { $stopEvent.Dispose() }
    $resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory) + [IO.Path]::DirectorySeparatorChar
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    if ($resolvedFixture.StartsWith($resolvedOutput,[StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath (Join-Path $resolvedFixture '.flowtary-dirapi-fixture'))) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
