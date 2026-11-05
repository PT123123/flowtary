[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Snapshot,
    [Parameter(Mandatory)][string]$Output,
    [ValidateRange(100,10000)][int]$Queries=1000
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
$binaryPath=Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$snapshotPath=(Get-Item -LiteralPath $Snapshot).FullName
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=$binaryPath
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.RedirectStandardInput=$true
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
$start.ArgumentList.Add('serve-snapshot')
$start.ArgumentList.Add('--snapshot')
$start.ArgumentList.Add($snapshotPath)
$process=[Diagnostics.Process]::new()
$process.StartInfo=$start
$started=$false
$startup=[Diagnostics.Stopwatch]::StartNew()
try {
    $null=$process.Start()
    $started=$true
    $ready=$process.StandardError.ReadLineAsync()
    if(-not $ready.Wait(5000)){ throw 'Snapshot host did not become ready within 5 seconds' }
    if(-not $ready.Result -or -not $ready.Result.StartsWith('snapshot-ready:')){ throw "Snapshot load failed: $($ready.Result)" }
    $startup.Stop()
    # Drain remaining diagnostics so the child cannot block on a full stderr pipe.
    $diagnostics=$process.StandardError.ReadToEndAsync()
    $times=[Collections.Generic.List[double]]::new()
    for($i=0;$i -lt ($Queries+20);$i++){
        $request=@{ schema=1;id=$i;query=@{ words=@('flowtary');workshop_only=$true;limit=10;budget_ms=25 } } | ConvertTo-Json -Compress -Depth 5
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $process.StandardInput.WriteLine($request)
        $process.StandardInput.Flush()
        $reply=$process.StandardOutput.ReadLineAsync()
        if(-not $reply.Wait(3000)){ throw "Query $i exceeded the 3-second transport timeout" }
        $timer.Stop()
        if(-not $reply.Result){ throw "Snapshot host ended during query $i" }
        $result=$reply.Result | ConvertFrom-Json
        if($result.id -ne $i -or $result.error -or $result.result.truncated -or $result.result.hits.Count -ne 10){ throw "Unexpected query response: $($reply.Result)" }
        if($i -ge 20){ $times.Add($timer.Elapsed.TotalMilliseconds) }
    }
    $process.StandardInput.Close()
    if(-not $process.WaitForExit(3000)){ throw 'Snapshot host did not exit after EOF' }
    if($process.ExitCode -ne 0){ throw "Snapshot host failed: $($diagnostics.Result)" }
    $sorted=@($times | Sort-Object)
    $report=[ordered]@{ schema=1;mode='offline-snapshot-pipe-roundtrip';queries=$Queries;warmup_queries=20;
        snapshot=$snapshotPath;snapshot_bytes=(Get-Item -LiteralPath $snapshotPath).Length;
        binary_sha256=(Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash;
        startup_until_ready_ms=$startup.Elapsed.TotalMilliseconds;
        p50_ms=$sorted[[int][Math]::Floor(($Queries-1)*0.5)];p95_ms=$sorted[[int][Math]::Floor(($Queries-1)*0.95)];max_ms=$sorted[-1];
        timing_scope='From stdin write through complete stdout line receipt; excludes JSON decoding and UI painting; one persistent child, no live journal.' }
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
    $report | ConvertTo-Json -Depth 5
} finally {
    if($started -and -not $process.HasExited){ $process.Kill(); $null=$process.WaitForExit(3000) }
    $process.Dispose()
}
