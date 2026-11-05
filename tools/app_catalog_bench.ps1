[CmdletBinding()]
param([ValidateRange(100,4096)][int]$Entries=1000,[string]$OutputDirectory,[switch]$Discover)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repoRoot ('logs/indexer/app-query-'+(Get-Date -Format yyyyMMdd-HHmmss))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw "Output exists: $OutputDirectory"}
$null=New-Item -ItemType Directory -Path $OutputDirectory
$probePath=Join-Path $repoRoot 'build/app-catalog-probe.exe'
$metricsPath=Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$token=[Guid]::NewGuid().ToString('N')
$startName="Local\Flowtary-AppProbe-Start-$token";$stopName="Local\Flowtary-AppProbe-Stop-$token"
$startEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$startName)
$stopEvent=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$stopName)
$processes=[Collections.Generic.List[Diagnostics.Process]]::new()
function Launch([string]$File,[string[]]$Arguments){
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$File;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($value in $Arguments){$info.ArgumentList.Add($value)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info;$null=$process.Start();$processes.Add($process)
    return $process
}
try{
    $probeArgs=@('--entries',"$Entries",'--output',"$OutputDirectory/probe.json",'--start-event',$startName,'--stop-event',$stopName)
    if($Discover){$probeArgs+='--discover-real-catalog'}
    $target=Launch $probePath $probeArgs
    $targetOut=$target.StandardOutput.ReadToEndAsync();$targetError=$target.StandardError.ReadToEndAsync()
    $sampler=Launch $metricsPath @('metrics','--pid',"$($target.Id)",'--seconds','12','--output',"$OutputDirectory/samples.jsonl")
    $samplerOut=$sampler.StandardOutput.ReadToEndAsync();$samplerError=$sampler.StandardError.ReadToEndAsync()
    Start-Sleep -Seconds 3
    $null=$startEvent.Set()
    if(-not $sampler.WaitForExit(20000)){throw 'External-process sampler exceeded its time budget'}
    if($sampler.ExitCode -ne 0){throw "Sampler failed: $($samplerError.Result)"}
    $samplerOut.Result | Set-Content -LiteralPath "$OutputDirectory/metrics-report.json" -Encoding utf8
    $null=$stopEvent.Set()
    if(-not $target.WaitForExit(5000)){throw 'Application query probe did not exit'}
    if($target.ExitCode -ne 0){throw "Application query probe failed ($($target.ExitCode)): $($targetError.Result)"}
    $probe=Get-Content -LiteralPath "$OutputDirectory/probe.json" -Raw | ConvertFrom-Json
    $metrics=$samplerOut.Result | ConvertFrom-Json
    if($metrics.last.monitored_pid -ne $target.Id){throw 'Sampler did not monitor the requested process'}
    [ordered]@{schema=1;source=$(if($Discover){'production C++ app catalog discovery (Start Menu, App Paths, AppsFolder)'}else{'production C++ SearchPrograms with synthetic launch entries'});probe=$probe;
        monitored_pid=$target.Id;whole_window=$metrics.phases;
        probe_sha256=(Get-FileHash -LiteralPath $probePath -Algorithm SHA256).Hash;
        sampler_sha256=(Get-FileHash -LiteralPath $metricsPath -Algorithm SHA256).Hash;
        scope=$(if($Discover){'Performs bounded known-source app discovery; no installed-tree walk, raw volume read, icon extraction or UI painting. Whole-machine counters contain background work.'}else{'No actual installed-app discovery, shortcut/icon I/O, raw volume reads or UI painting. Whole-machine counters contain background work.'})} |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath "$OutputDirectory/report.json" -Encoding utf8
    $probe | ConvertTo-Json -Depth 20
    Write-Host "Six-dimension samples and target-process report: $OutputDirectory"
}finally{
    $null=$startEvent.Set();$null=$stopEvent.Set()
    foreach($process in $processes){if(-not $process.HasExited){$process.Kill();$null=$process.WaitForExit(3000)};$process.Dispose()}
    $startEvent.Dispose();$stopEvent.Dispose()
}
