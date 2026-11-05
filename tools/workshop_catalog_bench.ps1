# Six-dimension resource benchmark for Flowtary's production C:\workshop rule.
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repoRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$logsRoot=[IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer'))+[IO.Path]::DirectorySeparatorChar
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repoRoot ('logs/indexer/workshop-catalog-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $OutputDirectory.StartsWith($logsRoot,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $OutputDirectory)){
    throw 'OutputDirectory must be a new directory inside logs/indexer'
}
$null=New-Item -ItemType Directory -Path $OutputDirectory
$metricsExe=Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$probeExe=Join-Path $repoRoot 'build/workshop-catalog-probe.exe'
if(-not (Test-Path -LiteralPath $metricsExe)){throw 'Release indexer metrics sampler is missing'}
if(-not (Test-Path -LiteralPath $probeExe)){throw 'Build the workshop-catalog-probe target first'}

function Start-Redirected([string]$File,[string[]]$Arguments){
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$File;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info;$null=$process.Start()
    return $process
}

# Reject a poor resource baseline before starting even this bounded path workload.
$preflightSamples=Join-Path $OutputDirectory 'preflight-samples.jsonl'
$preflight=Start-Redirected $metricsExe @('metrics','--seconds','5','--output',$preflightSamples)
$preflightOut=$preflight.StandardOutput.ReadToEndAsync();$preflightErr=$preflight.StandardError.ReadToEndAsync()
if(-not $preflight.WaitForExit(15000)){$preflight.Kill();throw 'Five-second resource preflight exceeded its bound'}
if($preflight.ExitCode -ne 0){throw "Resource preflight failed: $($preflightErr.Result)"}
$preflightReport=$preflightOut.Result|ConvertFrom-Json
$preflightPhase=@($preflightReport.phases|Where-Object phase -eq machine_observation|Select-Object -Last 1)[0]
$reasons=[Collections.Generic.List[string]]::new()
if(-not $preflightPhase -or $preflightPhase.samples -lt 5){$reasons.Add('preflight did not cover five seconds')}
elseif($preflightPhase.counter_error_samples -ne 0){$reasons.Add('required counters are missing')}
if($preflightPhase){
    if($preflightPhase.maximum_commit_ratio -gt .85){$reasons.Add('commit ratio exceeded 85%')}
    if($preflightPhase.maximum_pressure_ratio -gt .90){$reasons.Add('memory pressure exceeded 90%')}
    if($preflightPhase.minimum_available_bytes -lt 1.5GB){$reasons.Add('available memory fell below 1.5 GiB')}
    if($preflightPhase.max_pages_input_per_s -gt 10000){$reasons.Add('paging input exceeded 10,000 pages/second')}
    if($preflightPhase.mean_cpu_percent -gt 85){$reasons.Add('mean CPU exceeded 85%')}
    if($preflightPhase.mean_disk_busy_percent -gt 90){$reasons.Add('mean disk busy exceeded 90%')}
}
$preflightReport|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'preflight-report.json') -Encoding utf8
if($reasons.Count){throw "Host preflight rejected the bounded workshop benchmark: $($reasons -join '; ')"}

$token=[Guid]::NewGuid().ToString('N')
$startName="Local\Flowtary-WorkshopProbe-Start-$token";$stopName="Local\Flowtary-WorkshopProbe-Stop-$token"
$startEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$startName)
$stopEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$stopName)
$target=$null;$sampler=$null
try{
    $target=Start-Redirected $probeExe @('--output',"$OutputDirectory/probe.json",'--start-event',$startName,'--stop-event',$stopName)
    $targetOut=$target.StandardOutput.ReadToEndAsync();$targetErr=$target.StandardError.ReadToEndAsync()
    $sampler=Start-Redirected $metricsExe @('metrics','--seconds','18','--pid',"$($target.Id)",'--output',"$OutputDirectory/samples.jsonl")
    $samplerOut=$sampler.StandardOutput.ReadToEndAsync();$samplerErr=$sampler.StandardError.ReadToEndAsync()
    Start-Sleep -Seconds 3
    $null=$startEvent.Set()
    if(-not $sampler.WaitForExit(25000)){$sampler.Kill();throw 'Whole-machine sample window exceeded its bound'}
    if($sampler.ExitCode -ne 0){throw "Six-dimension sampler failed: $($samplerErr.Result)"}
    $samplerOut.Result|Set-Content -LiteralPath (Join-Path $OutputDirectory 'metrics-report.json') -Encoding utf8
    $null=$stopEvent.Set()
    if(-not $target.WaitForExit(5000)){throw 'Workshop probe did not exit after sampler release'}
    $targetOut.Result|Set-Content -LiteralPath (Join-Path $OutputDirectory 'probe-stdout.log') -Encoding utf8
    $targetErr.Result|Set-Content -LiteralPath (Join-Path $OutputDirectory 'probe-stderr.log') -Encoding utf8
    if($target.ExitCode -ne 0){throw "Workshop probe failed ($($target.ExitCode))"}
    $probe=Get-Content -LiteralPath (Join-Path $OutputDirectory 'probe.json') -Raw|ConvertFrom-Json
    $metrics=$samplerOut.Result|ConvertFrom-Json
    if($metrics.last.monitored_pid -ne $target.Id){throw 'Sampler did not monitor the workshop probe process'}
    [ordered]@{
        schema=1;mode='production-workshop-catalog-six-dimension-benchmark';
        scope='C:\workshop descendants and .exe files only; no raw volume handle or file-content reads';
        probe=$probe;whole_machine_phases=$metrics.phases;counter_errors=$metrics.last.counter_errors;
        native_host_violations=$metrics.native_host_violations;
        probe_sha256=(Get-FileHash -LiteralPath $probeExe -Algorithm SHA256).Hash;
        metrics_sha256=(Get-FileHash -LiteralPath $metricsExe -Algorithm SHA256).Hash
    }|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $OutputDirectory 'report.json') -Encoding utf8
    Get-Content -LiteralPath (Join-Path $OutputDirectory 'report.json') -Raw
}finally{
    $null=$startEvent.Set();$null=$stopEvent.Set()
    foreach($process in @($target,$sampler)){
        if($process){if(-not $process.HasExited){$process.Kill();$null=$process.WaitForExit(3000)};$process.Dispose()}
    }
    $startEvent.Dispose();$stopEvent.Dispose()
}
