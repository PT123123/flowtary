[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Snapshot,
    [string]$OutputDirectory,
    [ValidateRange(1, 3580)][int]$SustainedSeconds = 4,
    [ValidateRange(0, 1000)][int]$QueriesPerSecond = 0,
    [ValidateLength(1, 128)][string]$Query = 'flowtary',
    [switch]$PathQuery,
    [switch]$ExerciseSnapshotReload
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = [IO.Path]::GetFullPath($repoRoot)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('logs/indexer/cpp-ipc-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$logsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer')) + [IO.Path]::DirectorySeparatorChar
if (-not $OutputDirectory.StartsWith($logsRoot, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $OutputDirectory)) {
    throw 'OutputDirectory must be a new directory inside logs/indexer'
}
$null = New-Item -ItemType Directory -Path $OutputDirectory
$snapshotPath = (Get-Item -LiteralPath $Snapshot).FullName
$clientSnapshotPath = $snapshotPath
if ($ExerciseSnapshotReload) {
    if ($QueriesPerSecond -lt 1 -or $SustainedSeconds -lt 15) {
        throw '-ExerciseSnapshotReload requires rate-limited mode and at least 15 seconds'
    }
    $clientSnapshotPath = Join-Path $OutputDirectory 'reload-test-snapshot.fti'
    Copy-Item -LiteralPath $snapshotPath -Destination $clientSnapshotPath
}
$clientPath = Join-Path $repoRoot 'build/indexer_client_test.exe'
$indexerPath = Join-Path $repoRoot 'build/flowtary-indexer.exe'
if (-not (Test-Path -LiteralPath $clientPath) -or -not (Test-Path -LiteralPath $indexerPath)) {
    throw 'Build indexer_client_test and stage flowtary-indexer.exe into build before running this benchmark'
}
$metricsPath = Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$processes = [Collections.Generic.List[Diagnostics.Process]]::new()
function Start-RedirectedProcess([string]$File, [string[]]$Arguments, [string]$SnapshotOverride = '', [string]$ChildPidFile = '', [string]$QueryOverride = '', [bool]$PathQueryOverride = $false) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $File
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    if ($SnapshotOverride) { $start.Environment['FLOWTARY_INDEXER_TEST_SNAPSHOT'] = $SnapshotOverride }
    if ($ChildPidFile) { $start.Environment['FLOWTARY_TEST_CHILD_PID_FILE'] = $ChildPidFile }
    if ($QueryOverride) { $start.Environment['FLOWTARY_TEST_QUERY'] = $QueryOverride }
    if ($PathQueryOverride) { $start.Environment['FLOWTARY_TEST_PATH_QUERY'] = '1' }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $null = $process.Start()
    $processes.Add($process)
    return $process
}
function Get-Median([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object); $middle = [int][math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}
function Get-TailChangePerMinute($Samples, [string]$Property, [double]$Scale = 1.0, [double]$MinimumSampleSeconds = 60.0) {
    if ($Samples.Count -lt 8 -or ([double]$Samples[-1].elapsed_s - [double]$Samples[0].elapsed_s) -lt $MinimumSampleSeconds) { return $null }
    $quarter = [math]::Max(2, [int][math]::Floor($Samples.Count / 4))
    $first = @($Samples | Select-Object -First $quarter | Where-Object { $null -ne $_.$Property })
    $last = @($Samples | Select-Object -Skip ($Samples.Count - $quarter) | Where-Object { $null -ne $_.$Property })
    if ($first.Count -lt 2 -or $last.Count -lt 2) { return $null }
    $firstValues = [double[]]@($first | ForEach-Object { [double]$_.$Property })
    $lastValues = [double[]]@($last | ForEach-Object { [double]$_.$Property })
    $firstTimes = [double[]]@($first | ForEach-Object { [double]$_.elapsed_s })
    $lastTimes = [double[]]@($last | ForEach-Object { [double]$_.elapsed_s })
    $elapsed = (Get-Median $lastTimes) - (Get-Median $firstTimes)
    if ($elapsed -le 0) { return $null }
    return (((Get-Median $lastValues) - (Get-Median $firstValues)) / $Scale) * 60.0 / $elapsed
}
function Get-TargetProcessSummary([string]$SamplesPath, [long]$StartMs, [long]$EndMs) {
    $all = @(Get-Content -LiteralPath $SamplesPath | ForEach-Object { $_ | ConvertFrom-Json })
    $target = @($all | Where-Object { $_.unix_ms -ge $StartMs -and $_.unix_ms -le $EndMs })
    if ($target.Count -lt 2) { throw "Target process sampler did not cover the query interval: $SamplesPath" }
    $cpuTotal = 0.0; $cpuCovered = 0.0
    for ($i = 1; $i -lt $target.Count; $i++) {
        $dt = [double]($target[$i].unix_ms - $target[$i - 1].unix_ms) / 1000.0
        $cpu = $target[$i].process_cpu_percent
        if ($dt -gt 0 -and $null -ne $cpu) { $cpuTotal += [double]$cpu * $dt; $cpuCovered += $dt }
    }
    return [ordered]@{
        pid = $target[0].monitored_pid
        samples = $target.Count
        private_baseline_bytes = $target[0].process_private_bytes
        private_peak_bytes = ($target | Measure-Object process_private_bytes -Maximum).Maximum
        private_peak_growth_bytes = ([double](($target | Measure-Object process_private_bytes -Maximum).Maximum) - $target[0].process_private_bytes)
        private_end_bytes = $target[-1].process_private_bytes
        working_set_baseline_bytes = $target[0].process_working_set_bytes
        working_set_peak_bytes = ($target | Measure-Object process_working_set_bytes -Maximum).Maximum
        working_set_peak_growth_bytes = ([double](($target | Measure-Object process_working_set_bytes -Maximum).Maximum) - $target[0].process_working_set_bytes)
        handles_baseline = $target[0].process_handles
        handles_peak = ($target | Measure-Object process_handles -Maximum).Maximum
        handles_peak_growth = ([double](($target | Measure-Object process_handles -Maximum).Maximum) - $target[0].process_handles)
        handles_end = $target[-1].process_handles
        cpu_weighted_percent_machine_capacity = if ($cpuCovered -gt 0) { $cpuTotal / $cpuCovered } else { $null }
        cpu_coverage_seconds = $cpuCovered
        counter_error_samples = @($target | Where-Object { @($_.counter_errors.PSObject.Properties).Count -gt 0 }).Count
    }
}
try {
    # Refuse to add synthetic query load when the host is already under pressure.
    # The scan harness uses the same global headroom/page-input thresholds.
    $preflightPath = Join-Path $OutputDirectory 'preflight-samples.jsonl'
    $preflightJob = Start-RedirectedProcess $metricsPath @('metrics', '--seconds', '5', '--output', $preflightPath)
    $preflightOut = $preflightJob.StandardOutput.ReadToEndAsync()
    $preflightErr = $preflightJob.StandardError.ReadToEndAsync()
    if (-not $preflightJob.WaitForExit(15000)) { $preflightJob.Kill(); throw 'Host preflight sampler exceeded its bounded five-second window' }
    if ($preflightJob.ExitCode -ne 0) { throw "Host preflight sampler failed: $($preflightErr.Result)" }
    $preflightReport = $preflightOut.Result | ConvertFrom-Json
    $preflightPhase = @($preflightReport.phases | Where-Object phase -eq 'machine_observation' | Select-Object -Last 1)[0]
    $preflightErrors = [Collections.Generic.List[string]]::new()
    if (-not $preflightPhase -or $preflightPhase.samples -lt 5) { $preflightErrors.Add('preflight did not cover five seconds and five samples') }
    elseif ($preflightPhase.counter_error_samples -ne 0) { $preflightErrors.Add('one or more required counters were unavailable') }
    $preflightSample = $preflightReport.last
    if ($null -eq $preflightSample.system_handles -or $null -eq $preflightSample.pool_paged_bytes -or
        $null -eq $preflightSample.pool_nonpaged_bytes -or $null -eq $preflightSample.cpu_percent -or
        $null -eq $preflightSample.disk_busy_percent -or $null -eq $preflightSample.physical_available_bytes -or
        $null -eq $preflightSample.commit_limit_bytes -or $null -eq $preflightSample.memory_pressure_ratio -or
        $null -eq $preflightPhase.max_pages_input_per_s) {
        $preflightErrors.Add('one or more required system metrics are missing')
    }
    if ($preflightPhase) {
        if ($preflightPhase.maximum_commit_ratio -gt 0.85) { $preflightErrors.Add('commit ratio is above 85%') }
        if ($preflightPhase.maximum_pressure_ratio -gt 0.90) { $preflightErrors.Add('memory pressure exceeded 90%') }
        if ($preflightPhase.minimum_available_bytes -lt 1.5GB) { $preflightErrors.Add('available physical memory is below 1.5 GiB') }
        if ($preflightPhase.max_pages_input_per_s -gt 10000) { $preflightErrors.Add('paging input exceeded 10,000 pages/sec') }
        if ($preflightPhase.mean_cpu_percent -gt 85) { $preflightErrors.Add('mean machine CPU exceeded 85%') }
        if ($preflightPhase.mean_disk_busy_percent -gt 90) { $preflightErrors.Add('mean disk busy exceeded 90%') }
    }
    $preflightWarnings = @()
    if ($preflightSample.system_handles -gt 100000) { $preflightWarnings += ('abnormal System handle baseline: {0}' -f $preflightSample.system_handles) }
    if ($preflightSample.pool_paged_bytes -gt 4GB) { $preflightWarnings += ('abnormal paged-pool baseline: {0}' -f $preflightSample.pool_paged_bytes) }
    if ($preflightSample.pool_nonpaged_bytes -gt 2GB) { $preflightWarnings += ('abnormal nonpaged-pool baseline: {0}' -f $preflightSample.pool_nonpaged_bytes) }
    $preflightSummary = [ordered]@{ sample = $preflightSample; phase = $preflightPhase; warnings = $preflightWarnings; rejected = [bool]$preflightErrors.Count; reasons = @($preflightErrors) }
    $preflightSummary | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'preflight-report.json') -Encoding utf8
    if ($preflightErrors.Count) { throw "Host baseline failed the IPC benchmark preflight; workload was not started: $($preflightErrors -join '; ')" }

    # Start sampling first. Rate-limited mode models repeat searches over a sustained
    # interval; the default remains the original 100,000-query IPC burst.
    $observationSeconds = if ($QueriesPerSecond -gt 0) { [math]::Min(3600, $SustainedSeconds + 14) } else { 18 }
    $sampler = Start-RedirectedProcess $metricsPath @('metrics', '--seconds', "$observationSeconds", '--output', (Join-Path $OutputDirectory 'samples.jsonl'))
    $samplerOut = $sampler.StandardOutput.ReadToEndAsync()
    $samplerErr = $sampler.StandardError.ReadToEndAsync()
    Start-Sleep -Seconds 1
    $clientArguments = @('--integration-stress')
    if ($QueriesPerSecond -gt 0) {
        $clientArguments += @("$SustainedSeconds", "$QueriesPerSecond")
    }
    $childPidPath = Join-Path $OutputDirectory 'query-child.pid'
    if ($Query -match '[^\x20-\x7E]') { throw 'The integration query must contain printable ASCII only' }
    $client = Start-RedirectedProcess $clientPath $clientArguments $clientSnapshotPath $childPidPath $Query ([bool]$PathQuery)
    $clientOut = $client.StandardOutput.ReadToEndAsync()
    $clientErr = $client.StandardError.ReadToEndAsync()
    $clientMeasureSeconds = if ($QueriesPerSecond -gt 0) { $SustainedSeconds + 6 } else { 10 }
    $clientSampler = Start-RedirectedProcess $metricsPath @('metrics', '--seconds', "$clientMeasureSeconds", '--pid', "$($client.Id)", '--output', (Join-Path $OutputDirectory 'client-samples.jsonl'))
    $clientSamplerOut = $clientSampler.StandardOutput.ReadToEndAsync()
    $clientSamplerErr = $clientSampler.StandardError.ReadToEndAsync()
    $pidDeadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not (Test-Path -LiteralPath $childPidPath) -and [DateTime]::UtcNow -lt $pidDeadline -and -not $client.HasExited) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path -LiteralPath $childPidPath)) {
        if (-not $client.HasExited) { $null = $client.WaitForExit(1000) }
        $clientOut.Result | Set-Content -LiteralPath (Join-Path $OutputDirectory 'client-stdout.log') -Encoding utf8
        $clientErr.Result | Set-Content -LiteralPath (Join-Path $OutputDirectory 'client-stderr.log') -Encoding utf8
        throw "C++ harness did not publish the Rust query child PID (exit=$($client.ExitCode)); see client-stdout.log and client-stderr.log"
    }
    $childPid = [uint32](Get-Content -LiteralPath $childPidPath -Raw).Trim()
    $childMeasureSeconds = if ($QueriesPerSecond -gt 0) { $SustainedSeconds + 3 } else { 7 }
    $childSampler = Start-RedirectedProcess $metricsPath @('metrics', '--seconds', "$childMeasureSeconds", '--pid', "$childPid", '--output', (Join-Path $OutputDirectory 'child-samples.jsonl'))
    $childSamplerOut = $childSampler.StandardOutput.ReadToEndAsync()
    $childSamplerErr = $childSampler.StandardError.ReadToEndAsync()
    $snapshotReload = $null
    if ($ExerciseSnapshotReload) {
        Start-Sleep -Milliseconds 1500
        if ($client.HasExited) { throw 'C++ client exited before the snapshot generation replacement' }
        $replacementPath = Join-Path $OutputDirectory 'reload-test-snapshot.replacement'
        Copy-Item -LiteralPath $snapshotPath -Destination $replacementPath
        [IO.File]::Move($replacementPath, $clientSnapshotPath, $true)
        $snapshotReload = [ordered]@{initial_child_pid=$childPid;replacement_path=$clientSnapshotPath;
            replacement_sha256=(Get-FileHash -LiteralPath $clientSnapshotPath -Algorithm SHA256).Hash;
            replaced_utc=(Get-Date).ToUniversalTime().ToString('o')}
        $snapshotReload | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'snapshot-reload.json') -Encoding utf8
    }
    if (-not $client.WaitForExit(($SustainedSeconds + 30) * 1000)) { throw 'C++ query workload exceeded its bounded wait' }
    if ($client.ExitCode -ne 0) { throw "C++ query workload failed: $($clientErr.Result)" }
    if (-not $sampler.WaitForExit(($observationSeconds + 10) * 1000)) { throw 'Machine-wide resource sampler exceeded its bounded wait' }
    if ($sampler.ExitCode -ne 0) { throw "Machine-wide sampler failed: $($samplerErr.Result)" }
    if (-not $clientSampler.WaitForExit(($clientMeasureSeconds + 10) * 1000) -or $clientSampler.ExitCode -ne 0) { throw "C++ process sampler failed: $($clientSamplerErr.Result)" }
    if ($childSampler -and (-not $childSampler.WaitForExit(($childMeasureSeconds + 10) * 1000) -or $childSampler.ExitCode -ne 0)) { throw "Rust child process sampler failed: $($childSamplerErr.Result)" }

    $queryMatch = [regex]::Match($clientOut.Result, 'startup_us=(\d+),startup_client_cpu_us=(\d+),startup_child_cpu_us=(\d+),first_query_us=(\d+),queries=(\d+),warm_elapsed_us=(\d+),warm_start_unix_ms=(\d+),warm_end_unix_ms=(\d+),warm_client_cpu_us=(\d+),warm_child_cpu_us=(\d+),p50_us=(\d+),p95_us=(\d+),max_us=(\d+)')
    if (-not $queryMatch.Success) { throw "Could not parse query latency output: $($clientOut.Result)" }
    $metricsReport = $samplerOut.Result | ConvertFrom-Json
    $samples = @(Get-Content -LiteralPath (Join-Path $OutputDirectory 'samples.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    $window = @($samples | Where-Object { $_.phase -eq 'machine_observation' })
    if ($window.Count -lt 3) { throw 'Fewer than three machine-wide workload samples were captured' }
    $baseline = $window[0]
    $getPeak = { param($Property) ($window | Measure-Object -Property $Property -Maximum).Maximum }
    $getMin = { param($Property) ($window | Measure-Object -Property $Property -Minimum).Minimum }
    $getWeightedMean = {
        param($ValueProperty, $CoverageProperty)
        $sum = 0.0; $covered = 0.0
        for ($i = 1; $i -lt $window.Count; $i++) {
            $coverage = [double]($window[$i].elapsed_s - $window[$i - 1].elapsed_s)
            $value = $window[$i].$ValueProperty
            if ($coverage -gt 0 -and $null -ne $value) { $sum += [double]$value * $coverage; $covered += $coverage }
        }
        if ($covered -le 0) { return $null }
        return ($sum / $covered)
    }
    $counterErrorSamples = @($window | Where-Object { @($_.counter_errors.PSObject.Properties).Count -gt 0 }).Count
    $warmStartUnixMs = [long]$queryMatch.Groups[7].Value
    $warmEndUnixMs = [long]$queryMatch.Groups[8].Value
    $warmSamples = @($window | Where-Object { $_.unix_ms -ge $warmStartUnixMs -and $_.unix_ms -le $warmEndUnixMs })
    $preWarmSamples = @($window | Where-Object { $_.unix_ms -le $warmStartUnixMs })
    if ($warmSamples.Count -lt 2 -or $preWarmSamples.Count -lt 1) { throw 'Machine samples do not cover the sustained query burst' }
    $clientProcess = Get-TargetProcessSummary (Join-Path $OutputDirectory 'client-samples.jsonl') $warmStartUnixMs $warmEndUnixMs
    if ($ExerciseSnapshotReload) {
        $reloadMatch = [regex]::Match($clientOut.Result, 'child_pid_before_warm=(\d+),child_pid_after_warm=(\d+)')
        if (-not $reloadMatch.Success) { throw 'C++ harness omitted child process identities for reload verification' }
        $pidBeforeReload = [uint32]$reloadMatch.Groups[1].Value
        $pidAfterReload = [uint32]$reloadMatch.Groups[2].Value
        if ($pidBeforeReload -ne $childPid -or $pidAfterReload -ne $pidBeforeReload) {
            throw "Background snapshot reload replaced the query host unexpectedly (initial=$childPid, before=$pidBeforeReload, after=$pidAfterReload)"
        }
        $reloadReports = @(Get-ChildItem -LiteralPath (Join-Path $OutputDirectory 'Logs') -Filter "snapshot-load-$childPid-*.json" -File -ErrorAction SilentlyContinue |
            ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json } |
            Where-Object { $_.kind -eq 'snapshot-generation-reload' -and $_.outcome -eq 'loaded' })
        $publicationReports = @(Get-ChildItem -LiteralPath (Join-Path $OutputDirectory 'Logs') -Filter "snapshot-published-$childPid-*.json" -File -ErrorAction SilentlyContinue |
            ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json } |
            Where-Object { $_.phase -eq 'published' })
        if (-not $reloadReports.Count -or -not $publicationReports.Count) { throw 'Background reloader did not publish the replaced snapshot generation' }
        $snapshotReload.initial_child_pid = $pidBeforeReload
        $snapshotReload.reloaded_child_pid = $pidAfterReload
        $snapshotReload.generation_reload_verified = $true
        $snapshotReload.reload_report = $reloadReports[-1]
        $snapshotReload.publication_report = $publicationReports[-1]
        $snapshotReload | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'snapshot-reload.json') -Encoding utf8
    }
    $childProcess = Get-TargetProcessSummary (Join-Path $OutputDirectory 'child-samples.jsonl') $warmStartUnixMs $warmEndUnixMs
    $warmBaseline = $preWarmSamples[-1]
    $warmGetPeak = { param($Property) ($warmSamples | Measure-Object -Property $Property -Maximum).Maximum }
    $warmGetMin = { param($Property) ($warmSamples | Measure-Object -Property $Property -Minimum).Minimum }
    $warmGetWeightedMean = {
        param($ValueProperty)
        $sum = 0.0; $coveredMs = 0.0
        for ($i = 1; $i -lt $warmSamples.Count; $i++) {
            $coverageMs = [double]($warmSamples[$i].unix_ms - $warmSamples[$i - 1].unix_ms)
            $value = $warmSamples[$i].$ValueProperty
            if ($coverageMs -gt 0 -and $null -ne $value) { $sum += [double]$value * $coverageMs; $coveredMs += $coverageMs }
        }
        if ($coveredMs -le 0) { return $null }
        return ($sum / $coveredMs)
    }
    $warmCounterErrors = @($warmSamples | Where-Object { @($_.counter_errors.PSObject.Properties).Count -gt 0 }).Count
    $warmWindowSeconds = ($warmEndUnixMs - $warmStartUnixMs) / 1000.0
    if ($QueriesPerSecond -gt 0 -and $warmWindowSeconds -lt $SustainedSeconds) { throw 'Rate-limited workload ended before its requested observation interval' }
    $tailCommit = Get-TailChangePerMinute $warmSamples 'commit_bytes' 1MB 50.0
    $tailHandles = Get-TailChangePerMinute $warmSamples 'system_handles' 1.0 50.0
    $tailPaged = Get-TailChangePerMinute $warmSamples 'pool_paged_bytes' 1MB 50.0
    $tailNonpaged = Get-TailChangePerMinute $warmSamples 'pool_nonpaged_bytes' 1MB 50.0
    $warmCommitPeakChange = ([double](& $warmGetPeak 'commit_bytes')) - $warmBaseline.commit_bytes
    $warmHandlesPeakChange = ([double](& $warmGetPeak 'system_handles')) - $warmBaseline.system_handles
    $warmPagedPeakChange = ([double](& $warmGetPeak 'pool_paged_bytes')) - $warmBaseline.pool_paged_bytes
    $warmNonpagedPeakChange = ([double](& $warmGetPeak 'pool_nonpaged_bytes')) - $warmBaseline.pool_nonpaged_bytes
    $commitPeakChange = ([double](& $getPeak 'commit_bytes')) - $baseline.commit_bytes
    $handlesPeakChange = ([double](& $getPeak 'system_handles')) - $baseline.system_handles
    $pagedPeakChange = ([double](& $getPeak 'pool_paged_bytes')) - $baseline.pool_paged_bytes
    $nonpagedPeakChange = ([double](& $getPeak 'pool_nonpaged_bytes')) - $baseline.pool_nonpaged_bytes
    $warmSystem = [ordered]@{
        sample_count = $warmSamples.Count
        commit_baseline_bytes = $warmBaseline.commit_bytes
        commit_peak_change_bytes = $warmCommitPeakChange
        commit_peak_growth_bytes = [math]::Max(0.0, $warmCommitPeakChange)
        minimum_available_bytes = & $warmGetMin 'physical_available_bytes'
        memory_pressure_min = & $warmGetMin 'memory_pressure_ratio'
        memory_pressure_max = & $warmGetPeak 'memory_pressure_ratio'
        system_handles_baseline = $warmBaseline.system_handles
        system_handles_peak_change = $warmHandlesPeakChange
        system_handles_peak_growth = [math]::Max(0.0, $warmHandlesPeakChange)
        paged_pool_baseline_bytes = $warmBaseline.pool_paged_bytes
        paged_pool_peak_change_bytes = $warmPagedPeakChange
        paged_pool_peak_growth_bytes = [math]::Max(0.0, $warmPagedPeakChange)
        nonpaged_pool_baseline_bytes = $warmBaseline.pool_nonpaged_bytes
        nonpaged_pool_peak_change_bytes = $warmNonpagedPeakChange
        nonpaged_pool_peak_growth_bytes = [math]::Max(0.0, $warmNonpagedPeakChange)
        machine_cpu_weighted_percent = & $warmGetWeightedMean 'cpu_percent'
        disk_busy_weighted_percent = & $warmGetWeightedMean 'disk_busy_percent'
        disk_io_weighted_bytes_per_s = (& $warmGetWeightedMean 'disk_read_bytes_per_s') + (& $warmGetWeightedMean 'disk_write_bytes_per_s')
        pages_input_peak_per_s = ($warmSamples | Measure-Object pages_input_per_s -Maximum).Maximum
        counter_error_samples = $warmCounterErrors
    }
    $system = [ordered]@{
        commit_baseline_bytes = $baseline.commit_bytes
        commit_peak_change_bytes = $commitPeakChange
        commit_peak_growth_bytes = [math]::Max(0.0, $commitPeakChange)
        minimum_available_bytes = & $getMin 'physical_available_bytes'
        memory_pressure_min = & $getMin 'memory_pressure_ratio'
        memory_pressure_max = & $getPeak 'memory_pressure_ratio'
        system_handles_baseline = $baseline.system_handles
        system_handles_peak_change = $handlesPeakChange
        system_handles_peak_growth = [math]::Max(0.0, $handlesPeakChange)
        paged_pool_baseline_bytes = $baseline.pool_paged_bytes
        paged_pool_peak_change_bytes = $pagedPeakChange
        paged_pool_peak_growth_bytes = [math]::Max(0.0, $pagedPeakChange)
        nonpaged_pool_baseline_bytes = $baseline.pool_nonpaged_bytes
        nonpaged_pool_peak_change_bytes = $nonpagedPeakChange
        nonpaged_pool_peak_growth_bytes = [math]::Max(0.0, $nonpagedPeakChange)
        machine_cpu_weighted_percent = & $getWeightedMean 'cpu_percent' 'duration_seconds'
        disk_busy_weighted_percent = & $getWeightedMean 'disk_busy_percent' 'duration_seconds'
        disk_io_weighted_bytes_per_s = (& $getWeightedMean 'disk_read_bytes_per_s' 'duration_seconds') + (& $getWeightedMean 'disk_write_bytes_per_s' 'duration_seconds')
        pages_input_peak_per_s = ($window | Measure-Object pages_input_per_s -Maximum).Maximum
        counter_error_samples = $counterErrorSamples
    }
    $report = [ordered]@{
        schema = 1
        mode = 'offline-snapshot-cpp-client-and-persistent-rust-child'
        query_mode = if ($PathQuery) { 'path-only' } else { 'name' }
        query = $Query
        snapshot = $snapshotPath
        snapshot_sha256 = (Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256).Hash
        client_snapshot = $clientSnapshotPath
        snapshot_reload = $snapshotReload
        client_sha256 = (Get-FileHash -LiteralPath $clientPath -Algorithm SHA256).Hash
        indexer_sha256 = (Get-FileHash -LiteralPath $indexerPath -Algorithm SHA256).Hash
        snapshot_prewarm_us = [int]$queryMatch.Groups[1].Value
        snapshot_prewarm_client_cpu_us = [long]$queryMatch.Groups[2].Value
        snapshot_prewarm_child_cpu_us = [long]$queryMatch.Groups[3].Value
        first_query_after_prewarm_us = [int]$queryMatch.Groups[4].Value
        query_count = [int]$queryMatch.Groups[5].Value
        warm_query_window_us = [long]$queryMatch.Groups[6].Value
        requested_sustained_seconds = if ($QueriesPerSecond -gt 0) { $SustainedSeconds } else { $null }
        requested_queries_per_second = if ($QueriesPerSecond -gt 0) { $QueriesPerSecond } else { $null }
        warm_resource_window_seconds = [math]::Round($warmWindowSeconds, 3)
        warm_query_start_unix_ms = $warmStartUnixMs
        warm_query_end_unix_ms = $warmEndUnixMs
        warm_client_cpu_us = [long]$queryMatch.Groups[9].Value
        warm_child_cpu_us = [long]$queryMatch.Groups[10].Value
        warm_client_cpu_percent_estimate_one_logical_processor = [math]::Round(([double]$queryMatch.Groups[9].Value / [double]$queryMatch.Groups[6].Value) * 100, 2)
        warm_child_cpu_percent_estimate_one_logical_processor = [math]::Round(([double]$queryMatch.Groups[10].Value / [double]$queryMatch.Groups[6].Value) * 100, 2)
        warm_query_latency_us = [ordered]@{ p50 = [int]$queryMatch.Groups[11].Value; p95 = [int]$queryMatch.Groups[12].Value; max = [int]$queryMatch.Groups[13].Value }
        resource_window_seconds = [math]::Round($window[-1].elapsed_s - $window[0].elapsed_s, 3)
        system = $system
        query_burst_system = $warmSystem
        query_processes = [ordered]@{ cpp_client = $clientProcess; rust_query_child = $childProcess }
        host_preflight = $preflightSummary
        query_burst_tail_change_per_minute = [ordered]@{
            commit_MiB = if ($null -ne $tailCommit) { [math]::Round($tailCommit, 2) } else { $null }
            system_handles = if ($null -ne $tailHandles) { [math]::Round($tailHandles, 2) } else { $null }
            paged_pool_MiB = if ($null -ne $tailPaged) { [math]::Round($tailPaged, 2) } else { $null }
            nonpaged_pool_MiB = if ($null -ne $tailNonpaged) { [math]::Round($tailNonpaged, 2) } else { $null }
        }
        native_host_violations = $metricsReport.native_host_violations
        sampler_phase_summary = $metricsReport.phases
        limits = @(
            'Fixture-only snapshot query; no volume open, index build, journal polling, or UI painting.',
            'Whole-machine deltas include unrelated host work; query process and Rust child are both included in machine totals.',
            'Resource deltas are relative to the first workload sample; raw samples are retained beside this report.'
        )
    }
    $json = $report | ConvertTo-Json -Depth 8
    $json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'integration-summary.json') -Encoding utf8
    $json
} finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) { $process.Kill(); $null = $process.WaitForExit(3000) }
        $process.Dispose()
    }
}
