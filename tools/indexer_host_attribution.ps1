# Capture whole-machine indexer metrics and per-process commit/handle deltas together.
param(
    [Parameter(Mandatory)][string]$RunDirectory,
    [ValidateRange(5,300)][int]$Seconds = 20,
    [string[]]$ProcessNames = @()
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer')) + [IO.Path]::DirectorySeparatorChar
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
if (-not $RunDirectory.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunDirectory must stay inside logs/indexer.'
}
if (Test-Path -LiteralPath $RunDirectory) { throw 'RunDirectory already exists.' }
$null = New-Item -ItemType Directory -Path $RunDirectory
$binary = Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build the release indexer before sampling.' }
$binaryCopy = Join-Path $RunDirectory 'indexer-tested.exe'
Copy-Item -LiteralPath $binary -Destination $binaryCopy
$machinePath = Join-Path $RunDirectory 'machine.jsonl'
$processPath = Join-Path $RunDirectory 'processes.jsonl'
$reportPath = Join-Path $RunDirectory 'machine-report.json'
$processInfo = [Diagnostics.ProcessStartInfo]::new()
$processInfo.FileName = $binaryCopy
$processInfo.UseShellExecute = $false
$processInfo.CreateNoWindow = $true
$processInfo.RedirectStandardOutput = $true
$processInfo.RedirectStandardError = $true
foreach ($arg in @('metrics','--seconds',[string]$Seconds,'--interval-ms','1000','--output',$machinePath)) {
    $processInfo.ArgumentList.Add($arg)
}
$sampler = [Diagnostics.Process]::new()
$sampler.StartInfo = $processInfo
$writer = [IO.StreamWriter]::new($processPath,$false,[Text.UTF8Encoding]::new($false))
$rows = [Collections.Generic.List[object]]::new()
try {
    $null = $sampler.Start()
    $stdout = $sampler.StandardOutput.ReadToEndAsync()
    $stderr = $sampler.StandardError.ReadToEndAsync()
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while (-not $sampler.HasExited -and $deadline.Elapsed.TotalSeconds -lt ($Seconds + 15)) {
        $started = [Diagnostics.Stopwatch]::StartNew()
        $items = [Collections.Generic.List[object]]::new()
        foreach ($proc in @(Get-Process -ErrorAction SilentlyContinue)) {
            try {
                if ($ProcessNames.Count -gt 0 -and $proc.ProcessName -notin $ProcessNames) { continue }
                $row = [ordered]@{
                    pid = $proc.Id
                    name = $proc.ProcessName
                    private_bytes = $proc.PrivateMemorySize64
                    working_set_bytes = $proc.WorkingSet64
                    handles = $proc.HandleCount
                }
                $items.Add($row)
                $rows.Add([pscustomobject]@{sample=$rows.Count; timestamp_utc=[DateTime]::UtcNow.ToString('o'); pid=$row.pid;
                    name=$row.name; private_bytes=$row.private_bytes; working_set_bytes=$row.working_set_bytes; handles=$row.handles})
            } catch { }
            finally { $proc.Dispose() }
        }
        @{ timestamp_utc=[DateTime]::UtcNow.ToString('o'); process_count=$items.Count; processes=@($items) } |
            ConvertTo-Json -Depth 4 -Compress | ForEach-Object { $writer.WriteLine($_) }
        $writer.Flush()
        $wait = [math]::Max(0,1000-[int]$started.ElapsedMilliseconds)
        if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
    }
    if (-not $sampler.HasExited) { $sampler.Kill(); $null=$sampler.WaitForExit(3000); throw 'Machine sampler exceeded its deadline.' }
    $stdout.Result | Set-Content -LiteralPath $reportPath -Encoding utf8
    $stderr.Result | Set-Content -LiteralPath (Join-Path $RunDirectory 'stderr.log') -Encoding utf8
    if ($sampler.ExitCode -ne 0) { throw "Machine sampler failed ($($sampler.ExitCode))." }
} finally {
    $writer.Dispose()
    if (-not $sampler.HasExited) { $sampler.Kill(); $null=$sampler.WaitForExit(3000) }
    $sampler.Dispose()
}

$samples = @(Get-Content -LiteralPath $processPath | ForEach-Object { $_ | ConvertFrom-Json })
$byPid = @{}
foreach ($sample in $samples) {
    foreach ($proc in $sample.processes) {
        if (-not $byPid.ContainsKey([int]$proc.pid)) { $byPid[[int]$proc.pid] = [Collections.Generic.List[object]]::new() }
        $byPid[[int]$proc.pid].Add([pscustomobject]@{name=$proc.name; private_bytes=[long]$proc.private_bytes;
            working_set_bytes=[long]$proc.working_set_bytes; handles=[int]$proc.handles})
    }
}
$changes = foreach ($pidKey in $byPid.Keys) {
    $values = $byPid[$pidKey]
    if ($values.Count -lt 2) { continue }
    [pscustomobject]@{pid=[int]$pidKey;name=$values[-1].name;samples=$values.Count;
        private_delta_bytes=($values[-1].private_bytes-$values[0].private_bytes);
        private_peak_bytes=($values|Measure-Object private_bytes -Maximum).Maximum;
        working_set_delta_bytes=($values[-1].working_set_bytes-$values[0].working_set_bytes);
        handle_delta=($values[-1].handles-$values[0].handles)}
}
$changes = @($changes)
$topGrowth = @($changes | Sort-Object private_delta_bytes -Descending | Select-Object -First 20)
$machine = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
[ordered]@{
    schema=1; mode='metrics-plus-process-attribution'; window_seconds=$Seconds
    machine_report='machine-report.json'; machine_samples='machine.jsonl'; process_samples='processes.jsonl'
    process_samples_count=$samples.Count
    process_private_bytes_sum_first=($samples[0].processes|Measure-Object private_bytes -Sum).Sum
    process_private_bytes_sum_last=($samples[-1].processes|Measure-Object private_bytes -Sum).Sum
    top_process_deltas=$topGrowth
    machine_last=$machine.last
    native_host_violations=$machine.native_host_violations
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $RunDirectory 'attribution.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $RunDirectory 'attribution.json')
