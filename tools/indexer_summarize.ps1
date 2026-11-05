[CmdletBinding()]
param(
    [Parameter(Mandatory, ValueFromRemainingArguments)]
    [string[]]$ReportPath
)

$ErrorActionPreference = 'Stop'

function Get-WeightedMean($Phases, [string]$ValueProperty, [string]$CoverageProperty) {
    $weighted = 0.0
    $covered = 0.0
    foreach ($phase in $Phases) {
        $coverage = [double]$phase.$CoverageProperty
        $value = $phase.$ValueProperty
        if ($coverage -gt 0 -and $null -ne $value) {
            $weighted += [double]$value * $coverage
            $covered += $coverage
        }
    }
    if ($covered -le 0) { return $null }
    return ($weighted / $covered)
}

function Get-Median([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $middle = [int][math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-TailRatePerMinute($Samples, [string]$Property, [double]$Scale = 1.0) {
    if ($Samples.Count -lt 8 -or ([double]$Samples[-1].elapsed_s - [double]$Samples[0].elapsed_s) -lt 60.0) { return $null }
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

$results = foreach ($path in $ReportPath) {
    $fullPath = [IO.Path]::GetFullPath($path)
    $report = Get-Content -LiteralPath $fullPath -Raw | ConvertFrom-Json
    $samplesPath = Join-Path (Split-Path -Parent $fullPath) 'samples.jsonl'
    if (-not (Test-Path -LiteralPath $samplesPath)) {
        $samplesPath = Join-Path (Split-Path -Parent (Split-Path -Parent $fullPath)) 'samples.jsonl'
    }
    $samples = @()
    if (Test-Path -LiteralPath $samplesPath) {
        $samples = @(Get-Content -LiteralPath $samplesPath | ForEach-Object { $_ | ConvertFrom-Json })
    }
    $maximumPagesInput = $null
    $pagingGuardSamples = 0
    $windowSeconds = $null
    if ($samples.Count -gt 0) {
        $pageRates = @($samples | Where-Object { $null -ne $_.pages_input_per_s } |
            ForEach-Object { [double]$_.pages_input_per_s })
        if ($pageRates.Count -gt 0) { $maximumPagesInput = ($pageRates | Measure-Object -Maximum).Maximum }
        $pagingGuardSamples = @($samples | Where-Object {
            @($_.guard_violations | Where-Object { $_ -like '*paging input*' }).Count -gt 0
        }).Count
        if ($samples.Count -gt 1) {
            $windowSeconds = [double]$samples[-1].elapsed_s - [double]$samples[0].elapsed_s
        }
    }

    $baseline = $report.first_sample
    $lastSample = if ($samples.Count -gt 0) { $samples[-1] } else { $null }
    $pressure = @($samples | Where-Object { $null -ne $_.memory_pressure_ratio } |
        ForEach-Object { [double]$_.memory_pressure_ratio })
    $pressureMin = $null
    $pressureMax = $null
    if ($pressure.Count -gt 0) {
        $pressureMin = ($pressure | Measure-Object -Minimum).Minimum
        $pressureMax = ($pressure | Measure-Object -Maximum).Maximum
    }

    [pscustomobject]@{
        report = $fullPath
        mode = $report.mode
        status = $report.status
        entries = $report.result.records
        commit_peak_growth_MiB = [math]::Round($report.whole_run.commit_peak_growth_bytes / 1MB, 2)
        commit_end_change_MiB = if ($null -ne $lastSample) { [math]::Round(([double]$lastSample.commit_bytes - [double]$baseline.commit_bytes) / 1MB, 2) } else { $null }
        commit_end_ratio_percent = if ($null -ne $lastSample) { [math]::Round([double]$lastSample.commit_ratio * 100, 2) } else { $null }
        minimum_available_GiB = [math]::Round($report.whole_run.available_minimum_bytes / 1GB, 3)
        available_end_GiB = if ($null -ne $lastSample) { [math]::Round([double]$lastSample.physical_available_bytes / 1GB, 3) } else { $null }
        memory_pressure_baseline_percent = [math]::Round([double]$baseline.memory_pressure_ratio * 100, 2)
        memory_pressure_min_percent = if ($null -ne $pressureMin) { [math]::Round($pressureMin * 100, 2) } else { $null }
        memory_pressure_max_percent = if ($null -ne $pressureMax) { [math]::Round($pressureMax * 100, 2) } else { $null }
        memory_pressure_end_percent = if ($null -ne $lastSample) { [math]::Round([double]$lastSample.memory_pressure_ratio * 100, 2) } else { $null }
        system_handles_baseline = $baseline.system_handles
        system_handles_peak_growth = $report.whole_run.system_handles_peak_growth
        system_handles_end_change = if ($null -ne $lastSample) { [long]$lastSample.system_handles - [long]$baseline.system_handles } else { $null }
        paged_pool_baseline_GiB = [math]::Round($baseline.pool_paged_bytes / 1GB, 3)
        paged_pool_peak_growth_MiB = [math]::Round($report.whole_run.pool_paged_peak_growth_bytes / 1MB, 2)
        paged_pool_end_change_MiB = if ($null -ne $lastSample) { [math]::Round(([double]$lastSample.pool_paged_bytes - [double]$baseline.pool_paged_bytes) / 1MB, 2) } else { $null }
        nonpaged_pool_baseline_GiB = [math]::Round($baseline.pool_nonpaged_bytes / 1GB, 3)
        nonpaged_pool_peak_growth_MiB = [math]::Round($report.whole_run.pool_nonpaged_peak_growth_bytes / 1MB, 2)
        nonpaged_pool_end_change_MiB = if ($null -ne $lastSample) { [math]::Round(([double]$lastSample.pool_nonpaged_bytes - [double]$baseline.pool_nonpaged_bytes) / 1MB, 2) } else { $null }
        machine_cpu_weighted_percent = [math]::Round((Get-WeightedMean $report.phases 'mean_cpu_percent' 'cpu_coverage_seconds'), 2)
        disk_busy_weighted_percent = [math]::Round((Get-WeightedMean $report.phases 'mean_disk_busy_percent' 'disk_coverage_seconds'), 2)
        disk_io_weighted_MiB_per_s = [math]::Round(((Get-WeightedMean $report.phases 'mean_disk_bytes_per_s' 'disk_coverage_seconds') / 1MB), 2)
        maximum_pages_input_per_s = if ($null -ne $maximumPagesInput) { [math]::Round($maximumPagesInput, 2) } else { $null }
        paging_guard_violation_samples = $pagingGuardSamples
        sample_window_seconds = if ($null -ne $windowSeconds) { [math]::Round($windowSeconds, 2) } else { $null }
        commit_tail_change_MiB_per_min = if ($windowSeconds -ge 60) { [math]::Round((Get-TailRatePerMinute $samples 'commit_bytes' 1MB), 2) } else { $null }
        system_handles_tail_change_per_min = if ($windowSeconds -ge 60) { [math]::Round((Get-TailRatePerMinute $samples 'system_handles'), 2) } else { $null }
        paged_pool_tail_change_MiB_per_min = if ($windowSeconds -ge 60) { [math]::Round((Get-TailRatePerMinute $samples 'pool_paged_bytes' 1MB), 2) } else { $null }
        nonpaged_pool_tail_change_MiB_per_min = if ($windowSeconds -ge 60) { [math]::Round((Get-TailRatePerMinute $samples 'pool_nonpaged_bytes' 1MB), 2) } else { $null }
        process_private_peak_MiB = [math]::Round($report.whole_run.process_private_peak_bytes / 1MB, 2)
        native_volume_opens = $report.native_volume_opens
        directory_enumerations = $report.directory_enumerations
        native_host_violations = @($report.native_host_violations)
        counter_error_samples = [int](($report.phases | Measure-Object -Property counter_error_samples -Sum).Sum)
        cancelled_by_guard_or_caller = $report.cancelled_by_guard_or_caller
    }
}

$results | ConvertTo-Json -Depth 6
