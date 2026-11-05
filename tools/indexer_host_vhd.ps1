#requires -Version 7.0
#requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory,[switch]$VerifyInitialResume,[switch]$CaptureFileRecordPayload,[switch]$DirectoryLinkWalk,
    [switch]$CapturePoolTrace,[ValidateRange(8,30)][int]$WatchSeconds=8,[ValidateRange(0,10000)][int]$ScaleFixtureCount=0)
$ErrorActionPreference='Stop'
$repoRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$allowedRoot=[IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer'))+[IO.Path]::DirectorySeparatorChar
$RunDirectory=[IO.Path]::GetFullPath($RunDirectory)
if(-not $RunDirectory.StartsWith($allowedRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Test run must stay inside this workspace logs/indexer directory'}
if($CaptureFileRecordPayload -and -not $VerifyInitialResume){throw '-CaptureFileRecordPayload requires -VerifyInitialResume and remains confined to its new FLOWTARY_TEST VHD'}
if($DirectoryLinkWalk -and -not $VerifyInitialResume){throw '-DirectoryLinkWalk requires -VerifyInitialResume and remains confined to its new FLOWTARY_TEST VHD'}
if(Test-Path -LiteralPath $RunDirectory){throw 'Use a new run directory; existing test disks are never overwritten'}
$vhd=[IO.Path]::GetFullPath((Join-Path $RunDirectory 'flowtary-test.vhd'))
if(-not $vhd.StartsWith($RunDirectory+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or $vhd -match '["\r\n]'){throw 'Unsafe VHD path'}
$binary=Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
$indexerRoot=Join-Path $repoRoot 'indexer'
$cargo=Get-Command cargo -ErrorAction SilentlyContinue
if(-not $cargo){throw 'Rust cargo is required to build the exact source revision before a host VHD run'}
Push-Location $indexerRoot
try {
    & $cargo.Source build --release --locked
    if($LASTEXITCODE -ne 0){throw "Release indexer build failed with exit code $LASTEXITCODE"}
} finally { Pop-Location }
if(-not (Test-Path -LiteralPath $binary)){throw 'Release indexer binary is missing after the source build'}
$binaryHash=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
$null=New-Item -ItemType Directory -Path $RunDirectory
$children=[Collections.Generic.List[Diagnostics.Process]]::new()
$attached=$false
function State([string]$Phase,[object]$Detail=$null){
    @{schema=1;phase=$Phase;detail=$Detail;updated_utc=(Get-Date).ToUniversalTime().ToString('o');pid=$PID} |
        ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $RunDirectory 'state.json') -Encoding utf8
}
function StartTool([string]$File,[string[]]$Arguments){
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$File;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($arg in $Arguments){$info.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info;$null=$process.Start();$children.Add($process)
    return @{process=$process;stdout=$process.StandardOutput.ReadToEndAsync();stderr=$process.StandardError.ReadToEndAsync()}
}
function FinishTool($Job,[string]$Name,[int]$Timeout=45000){
    if(-not $Job.process.WaitForExit($Timeout)){$Job.process.Kill();throw "$Name exceeded its bounded test window"}
    $Job.stdout.Result | Set-Content -LiteralPath (Join-Path $RunDirectory "$Name.json") -Encoding utf8
    $Job.stderr.Result | Set-Content -LiteralPath (Join-Path $RunDirectory "$Name-stderr.log") -Encoding utf8
    if($Job.process.ExitCode -ne 0){throw "$Name failed ($($Job.process.ExitCode)): $($Job.stderr.Result)"}
    return $Job.stdout.Result
}
$xperfPath=$null;$poolTraceStarted=$false
$poolLoggerName="FlowtaryVhdPool-$PID-$([Guid]::NewGuid().ToString('N'))"
$poolKernelTrace=Join-Path $RunDirectory 'pool-kernel.etl'
$poolMergedTrace=Join-Path $RunDirectory 'pool-trace.etl'
$poolSummary=Join-Path $RunDirectory 'pool-summary.txt'
$poolTraceStartedUtc=$null
function Start-PoolTrace {
    if(-not $CapturePoolTrace){return}
    $tracelogPattern=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/*/x64/tracelog.exe'
    $tracelogCommand=Get-ChildItem -Path $tracelogPattern -File -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if(-not $tracelogCommand){throw 'Pool-tag capture requested but x64 WDK tracelog.exe is unavailable.'}
    $script:xperfPath=(Get-Command xperf.exe -ErrorAction SilentlyContinue).Source
    if(-not $script:xperfPath){throw 'Pool-tag analysis requested but WPT xperf.exe is unavailable.'}
    $script:tracelogPath=$tracelogCommand.FullName
    # Microsoft documents tracelog -systemlogger for a distinct named
    # SystemTraceProvider session. It coexists with the already-running NT
    # Kernel Logger and this script stops only its random unique session.
    & $script:tracelogPath -start $script:poolLoggerName -f $script:poolKernelTrace -eflag 'PROC_THREAD+LOADER+POOL' `
        -b 64 -min 8 -max 64 -cir 128 -stackwalk 'PoolAllocation+PoolFree' -systemlogger 2>&1 |
        Set-Content -LiteralPath (Join-Path $RunDirectory 'pool-trace-start.log') -Encoding utf8
    if($LASTEXITCODE -ne 0){
        throw "tracelog could not start the bounded named SystemTraceProvider pool trace; see $(Join-Path $RunDirectory 'pool-trace-start.log')."
    }
    $script:poolTraceStarted=$true
    $script:poolTraceStartedUtc=[DateTimeOffset]::UtcNow
    State 'pool-trace-started' @{logger_name=$poolLoggerName;tool=$script:tracelogPath;system_logger=$true;profile='PROC_THREAD+LOADER+POOL';stackwalk='PoolAllocation+PoolFree';buffer_size_kib=64;minimum_buffers=8;maximum_buffers=64;maximum_file_mib=128}
}
function Stop-PoolTrace {
    if(-not $script:poolTraceStarted){return}
    & $script:tracelogPath -stop $script:poolLoggerName 2>&1 |
        Set-Content -LiteralPath (Join-Path $RunDirectory 'pool-trace-stop.log') -Encoding utf8
    if($LASTEXITCODE -ne 0){throw 'tracelog failed to stop the bounded named pool trace.'}
    if(Test-Path -LiteralPath $script:poolKernelTrace){Move-Item -LiteralPath $script:poolKernelTrace -Destination $script:poolMergedTrace -Force}
    if(-not (Test-Path -LiteralPath $script:poolMergedTrace)){throw 'tracelog did not produce the expected bounded ETL file.'}
    $script:poolTraceStoppedUtc=[DateTimeOffset]::UtcNow
    $script:poolTraceStarted=$false
    [ordered]@{schema=1;logger_name=$script:poolLoggerName;started_utc=$script:poolTraceStartedUtc.ToString('o');stopped_utc=$script:poolTraceStoppedUtc.ToString('o');
        capture_scope='native initial collection/checkpoint resume through USN watch and final replay; excludes VHD creation, formatting, and fixture population';
        profile='PROC_THREAD+LOADER+POOL';stackwalk='PoolAllocation+PoolFree';buffer_size_kib=64;minimum_buffers=8;maximum_buffers=64;maximum_file_mib=128;
        kernel_trace=$script:poolKernelTrace;merged_trace=$script:poolMergedTrace;summary=$script:poolSummary} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $RunDirectory 'pool-trace-window.json') -Encoding utf8
    & $script:xperfPath -i $script:poolMergedTrace -o $script:poolSummary -a pool -pooltags -paged -nonpaged -top 100 2>&1 |
        Set-Content -LiteralPath (Join-Path $RunDirectory 'pool-analysis.log') -Encoding utf8
    if($LASTEXITCODE -ne 0){throw 'xperf could not summarize pool tags from the trace.'}
    State 'pool-trace-analyzed' @{logger_name=$poolLoggerName;trace=$poolMergedTrace;summary=$poolSummary}
}
function Get-WeightedSampleRate($Rows,[string]$Property){
    $integral=0.0;$coverage=0.0
    for($i=1;$i -lt $Rows.Count;$i++){
        $dt=[double]$Rows[$i].elapsed_s-[double]$Rows[$i-1].elapsed_s
        $value=$Rows[$i].$Property
        if($dt -gt 0 -and $null -ne $value -and [double]::IsFinite([double]$value)){
            $integral += [double]$value*$dt;$coverage += $dt
        }
    }
    return @{value=if($coverage -gt 0){$integral/$coverage}else{$null};coverage_seconds=$coverage}
}
function Get-HostMetricsPreflight([string]$Stem,[string]$Description){
    $samplesPath=Join-Path $RunDirectory ($Stem+'.jsonl')
    $null=FinishTool (StartTool $binary @('metrics','--seconds','5','--interval-ms','1000','--output',$samplesPath)) ($Stem+'-report')
    $report=Get-Content -LiteralPath (Join-Path $RunDirectory ($Stem+'-report.json')) -Raw|ConvertFrom-Json
    $phase=@($report.phases|Where-Object phase -eq 'machine_observation'|Select-Object -Last 1)[0]
    $sample=$report.last
    $errors=[Collections.Generic.List[string]]::new()
    if(-not $phase -or $phase.samples -lt 5){$errors.Add("$Description did not cover five seconds and five samples")}
    elseif($phase.counter_error_samples -ne 0){$errors.Add("$Description metrics contain counter errors")}
    if(-not $sample -or $null -eq $sample.system_handles -or $null -eq $sample.pool_paged_bytes -or
       $null -eq $sample.pool_nonpaged_bytes -or $null -eq $sample.disk_busy_percent -or
       $null -eq $sample.cpu_percent -or $null -eq $sample.physical_available_bytes -or
       $null -eq $sample.commit_limit_bytes -or $null -eq $sample.memory_pressure_ratio -or
       -not $phase -or $null -eq $phase.max_pages_input_per_s -or
       $null -eq $phase.mean_cpu_percent -or $null -eq $phase.mean_disk_busy_percent){
        $errors.Add("one or more required $Description machine metrics are missing")
    }
    if($phase){
        if($phase.maximum_commit_ratio -gt 0.85){$errors.Add("$Description commit ratio is above 85%")}
        if($phase.maximum_pressure_ratio -gt 0.90){$errors.Add("$Description memory pressure exceeded 90%")}
        if($phase.minimum_available_bytes -lt 1.5GB){$errors.Add("$Description available memory is below 1.5 GiB")}
        if($phase.max_pages_input_per_s -gt 10000){$errors.Add("$Description paging input exceeded 10,000 pages/sec")}
        if($phase.mean_cpu_percent -gt 85){$errors.Add("$Description mean machine CPU exceeded 85%")}
        if($phase.mean_disk_busy_percent -gt 90){$errors.Add("$Description mean disk busy exceeded 90%")}
    }
    return @{report=$report;phase=$phase;sample=$sample;errors=@($errors)}
}
function Get-HostMetricsPreflightWithRetry([string]$Stem,[string]$Description){
    for($attempt=0;$attempt -le 1;$attempt++){
        $currentStem=if($attempt -eq 0){$Stem}else{"$Stem-retry-$attempt"}
        $result=Get-HostMetricsPreflight $currentStem $Description
        if($result.errors.Count -eq 0){return $result}
        $onlyPageInput=$result.errors.Count -eq 1 -and $result.errors[0] -like '*paging input exceeded 10,000 pages/sec*'
        if($attempt -eq 0 -and $onlyPageInput){
            State "host-$Description-page-input-cooldown-retry" @{reasons=$result.errors;sample=$result.sample;phase=$result.phase;cooldown_seconds=5}
            Start-Sleep -Seconds 5
            continue
        }
        if($Description -eq 'baseline'){
            State 'host-baseline-rejected-before-vhd' @{reasons=$result.errors;sample=$result.sample;phase=$result.phase}
            throw "Host baseline failed the VHD preflight; no test VHD was created: $($result.errors -join '; ')"
        }
        State 'post-fixture-preflight-rejected-before-volume-open' @{reasons=$result.errors;sample=$result.sample;phase=$result.phase}
        throw "Post-fixture resource preflight failed; no indexer volume probe started: $($result.errors -join '; ')"
    }
}
function Write-HostResourceSummary {
    $operations=[Collections.Generic.List[object]]::new()
    $preflightObservations=[Collections.Generic.List[object]]::new()
    foreach($reportFile in Get-ChildItem -LiteralPath $RunDirectory -File -Filter '*-report.json'){
        try{$work=Get-Content -LiteralPath $reportFile.FullName -Raw|ConvertFrom-Json}catch{continue}
        if($work.mode -eq 'metrics-only-no-volume-access' -and $work.phases){
            $sampleStem=$reportFile.BaseName -replace '-report$',''
            $samplesPath=Join-Path $RunDirectory ($sampleStem+'.jsonl')
            if(-not (Test-Path -LiteralPath $samplesPath)){continue}
            $rows=@(Get-Content -LiteralPath $samplesPath|Where-Object{$_}|ForEach-Object{$_|ConvertFrom-Json})
            if($rows.Count -eq 0){continue}
            $base=@($rows|Where-Object phase -eq 'machine_observation'|Select-Object -First 1)[0]
            if(-not $base){$base=$rows[0]}
            $last=$rows[-1]
            $commitPeak=($rows|Measure-Object commit_bytes -Maximum).Maximum
            $handles=@($rows|Where-Object{$null -ne $_.system_handles}|ForEach-Object{[long]$_.system_handles})
            $pagedPeak=($rows|Measure-Object pool_paged_bytes -Maximum).Maximum
            $nonpagedPeak=($rows|Measure-Object pool_nonpaged_bytes -Maximum).Maximum
            $cpu=Get-WeightedSampleRate $rows 'cpu_percent'
            $diskBusy=Get-WeightedSampleRate $rows 'disk_busy_percent'
            $diskRead=Get-WeightedSampleRate $rows 'disk_read_bytes_per_s'
            $diskWrite=Get-WeightedSampleRate $rows 'disk_write_bytes_per_s'
            $diskCoverage=[Math]::Min([double]$diskRead.coverage_seconds,[double]$diskWrite.coverage_seconds)
            $diskIo=$null
            if($diskCoverage -gt 0){$diskIo=(([double]$diskRead.value*[double]$diskRead.coverage_seconds)+([double]$diskWrite.value*[double]$diskWrite.coverage_seconds))/$diskCoverage}
            $pages=@($rows|Where-Object{$null -ne $_.pages_input_per_s}|ForEach-Object{[double]$_.pages_input_per_s})
            $errors=0;foreach($row in $rows){if($row.counter_errors -and $row.counter_errors.PSObject.Properties.Count -gt 0){$errors++}}
            $preflightObservations.Add([ordered]@{
                operation=$reportFile.BaseName;report=$reportFile.FullName;samples=$samplesPath;sample_count=$rows.Count;
                machine_commit_baseline_bytes=$base.commit_bytes;machine_commit_peak_growth_bytes=([long]$commitPeak-[long]$base.commit_bytes);
                memory_pressure_min=($rows|Measure-Object memory_pressure_ratio -Minimum).Minimum;
                memory_pressure_max=($rows|Measure-Object memory_pressure_ratio -Maximum).Maximum;
                available_memory_min_bytes=($rows|Measure-Object physical_available_bytes -Minimum).Minimum;
                system_handles_baseline=$base.system_handles;
                system_handles_peak_growth=if($handles.Count){($handles|Measure-Object -Maximum).Maximum-[long]$base.system_handles}else{$null};
                paged_pool_baseline_bytes=$base.pool_paged_bytes;paged_pool_peak_growth_bytes=([long]$pagedPeak-[long]$base.pool_paged_bytes);
                nonpaged_pool_baseline_bytes=$base.pool_nonpaged_bytes;nonpaged_pool_peak_growth_bytes=([long]$nonpagedPeak-[long]$base.pool_nonpaged_bytes);
                machine_cpu_weighted_percent=$cpu.value;disk_busy_weighted_percent=$diskBusy.value;
                disk_read_write_weighted_bytes_per_s=$diskIo;disk_io_coverage_seconds=$diskCoverage;
                pages_input_peak_per_s=if($pages.Count){($pages|Measure-Object -Maximum).Maximum}else{$null};
                counter_error_samples=$errors;phase_summaries=$work.phases
            })
            continue
        }
        if($work.mode -notin @('explicitly-authorized-host-vhd-test','authorized-bounded-host-vhd-probe') -or -not $work.phases){continue}
        $sampleStem=$reportFile.BaseName -replace '-report$',''
        $samplesPath=Join-Path $RunDirectory ($sampleStem+'.jsonl')
        if(-not (Test-Path -LiteralPath $samplesPath)){continue}
        $rows=@(Get-Content -LiteralPath $samplesPath|Where-Object{$_}|ForEach-Object{$_|ConvertFrom-Json})
        if($rows.Count -eq 0){continue}
        $base=$rows[0];$last=$rows[-1]
        $commitPeak=($rows|Measure-Object commit_bytes -Maximum).Maximum
        $availableMin=($rows|Measure-Object physical_available_bytes -Minimum).Minimum
        $pressureMin=($rows|Measure-Object memory_pressure_ratio -Minimum).Minimum
        $pressureMax=($rows|Measure-Object memory_pressure_ratio -Maximum).Maximum
        $handles=@($rows|Where-Object{$null -ne $_.system_handles}|ForEach-Object{[long]$_.system_handles})
        $pagedPeak=($rows|Measure-Object pool_paged_bytes -Maximum).Maximum
        $nonpagedPeak=($rows|Measure-Object pool_nonpaged_bytes -Maximum).Maximum
        $cpu=Get-WeightedSampleRate $rows 'cpu_percent'
        $processCpu=Get-WeightedSampleRate $rows 'process_cpu_percent'
        $diskBusy=Get-WeightedSampleRate $rows 'disk_busy_percent'
        $diskRead=Get-WeightedSampleRate $rows 'disk_read_bytes_per_s'
        $diskWrite=Get-WeightedSampleRate $rows 'disk_write_bytes_per_s'
        $pages=@($rows|Where-Object{$null -ne $_.pages_input_per_s}|ForEach-Object{[double]$_.pages_input_per_s})
        $errors=0;$guardViolations=0
        foreach($row in $rows){
            if($row.counter_errors -and $row.counter_errors.PSObject.Properties.Count -gt 0){$errors++}
            $guardViolations+=@($row.guard_violations).Count
        }
        $diskCoverage=[Math]::Min([double]$diskRead.coverage_seconds,[double]$diskWrite.coverage_seconds)
        $diskIo=$null
        if($diskCoverage -gt 0){
            $diskIo=(([double]$diskRead.value*[double]$diskRead.coverage_seconds)+([double]$diskWrite.value*[double]$diskWrite.coverage_seconds))/$diskCoverage
        }
        $operations.Add([ordered]@{
            operation=$reportFile.BaseName;report=$reportFile.FullName;samples=$samplesPath;sample_count=$rows.Count;
            start_unix_ms=$base.unix_ms;end_unix_ms=$last.unix_ms;window_seconds=[Math]::Round(([double]$last.elapsed_s-[double]$base.elapsed_s),3);
            machine_commit_baseline_bytes=$base.commit_bytes;machine_commit_peak_growth_bytes=([long]$commitPeak-[long]$base.commit_bytes);
            machine_commit_end_change_bytes=([long]$last.commit_bytes-[long]$base.commit_bytes);
            memory_pressure_min=[double]$pressureMin;memory_pressure_max=[double]$pressureMax;available_memory_min_bytes=[long]$availableMin;
            system_handles_baseline=$base.system_handles;system_handles_peak_growth=if($handles.Count){($handles|Measure-Object -Maximum).Maximum-[long]$base.system_handles}else{$null};
            system_handles_end_change=if($null -ne $base.system_handles -and $null -ne $last.system_handles){[long]$last.system_handles-[long]$base.system_handles}else{$null};
            paged_pool_baseline_bytes=$base.pool_paged_bytes;paged_pool_peak_growth_bytes=([long]$pagedPeak-[long]$base.pool_paged_bytes);
            paged_pool_end_change_bytes=([long]$last.pool_paged_bytes-[long]$base.pool_paged_bytes);
            nonpaged_pool_baseline_bytes=$base.pool_nonpaged_bytes;nonpaged_pool_peak_growth_bytes=([long]$nonpagedPeak-[long]$base.pool_nonpaged_bytes);
            nonpaged_pool_end_change_bytes=([long]$last.pool_nonpaged_bytes-[long]$base.pool_nonpaged_bytes);
            machine_cpu_weighted_percent=$cpu.value;machine_cpu_coverage_seconds=$cpu.coverage_seconds;
            process_cpu_weighted_percent=$processCpu.value;process_cpu_coverage_seconds=$processCpu.coverage_seconds;
            process_private_peak_bytes=(($rows|Measure-Object process_private_bytes -Maximum).Maximum);
            process_handles_peak=(($rows|Measure-Object process_handles -Maximum).Maximum);
            disk_busy_weighted_percent=$diskBusy.value;disk_busy_coverage_seconds=$diskBusy.coverage_seconds;
            disk_read_write_weighted_bytes_per_s=$diskIo;disk_io_coverage_seconds=$diskCoverage;
            pages_input_peak_per_s=if($pages.Count){($pages|Measure-Object -Maximum).Maximum}else{$null};
            counter_error_samples=$errors;guard_violation_count=$guardViolations;
            cancelled_by_guard_or_caller=$work.cancelled_by_guard_or_caller;native_io_stats=$work.native_io_stats
        })
    }
    $operationsArray=@($operations)
    $summary=[ordered]@{schema=1;mode='host-vhd-provider-resource-summary';scope='per-operation whole-machine measurements, relative to that operation first sample; values include host background activity';
        operation_count=$operationsArray.Count;operations=$operationsArray;machine_preflight_observation_count=$preflightObservations.Count;
        machine_preflight_observations=@($preflightObservations);
        pool_trace=if($CapturePoolTrace -and (Test-Path -LiteralPath (Join-Path $RunDirectory 'pool-trace-window.json'))){Get-Content -LiteralPath (Join-Path $RunDirectory 'pool-trace-window.json') -Raw|ConvertFrom-Json}else{$null};
        generated_utc=[DateTimeOffset]::UtcNow.ToString('o')}
    $summary|ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $RunDirectory 'resource-summary.json') -Encoding utf8
}
try{
    State 'baseline-before-fixture'
    $baselineCheck=Get-HostMetricsPreflightWithRetry 'before-fixture' 'baseline'
    $baselineReport=$baselineCheck.report;$machinePhase=$baselineCheck.phase;$sample=$baselineCheck.sample
    $baselineWarnings=@()
    if($sample.system_handles -gt 100000){$baselineWarnings+=('abnormal System handle baseline: {0}' -f $sample.system_handles)}
    if($sample.pool_paged_bytes -gt 4GB){$baselineWarnings+=('abnormal paged-pool baseline: {0}' -f $sample.pool_paged_bytes)}
    if($sample.pool_nonpaged_bytes -gt 2GB){$baselineWarnings+=('abnormal nonpaged-pool baseline: {0}' -f $sample.pool_nonpaged_bytes)}
    State 'host-baseline-accepted' @{sample=$sample;phase=$machinePhase;warnings=$baselineWarnings}
    State 'create-new-128MiB-vhd'
    @("create vdisk file=`"$vhd`" maximum=128 type=expandable","select vdisk file=`"$vhd`"",'attach vdisk','exit') |
        Set-Content -LiteralPath (Join-Path $RunDirectory 'create-vhd.txt') -Encoding ascii
    $diskpart=StartTool "$env:SystemRoot/System32/diskpart.exe" @('/s',"$RunDirectory/create-vhd.txt")
    $null=FinishTool $diskpart 'diskpart-create' 30000
    $image=Get-DiskImage -ImagePath $vhd
    if(-not $image.Attached){throw 'New test VHD did not attach'}
    $attached=$true
    $disks=@($image | Get-Disk)
    if($disks.Count -ne 1){throw 'VHD did not resolve to exactly one disk'}
    $disk=$disks[0]
    if($image.ImagePath -ne $vhd -or $image.DevicePath -ne "\\.\PhysicalDrive$($disk.Number)" -or
       $disk.PartitionStyle -ne 'RAW' -or $disk.Size -ne 128MB){throw 'Refusing to initialize a disk that is not this new 128 MiB VHD'}
    $disk | Select-Object Number,Size,PartitionStyle,BusType,FriendlyName,Path,Location |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunDirectory 'test-disk-before-format.json') -Encoding utf8
    $used=@(Get-CimInstance Win32_LogicalDisk | ForEach-Object {$_.DeviceID.TrimEnd(':')})
    $drive=@('T','U','V','W','X','Y','Z','S','R' | Where-Object {$_ -notin $used}) | Select-Object -First 1
    if(-not $drive){throw 'No free test drive letter'}
    State 'format-only-verified-new-vhd' @{disk_number=$disk.Number;drive=$drive;backing=$vhd}
    $null=Initialize-Disk -Number $disk.Number -PartitionStyle GPT -PassThru
    $partition=New-Partition -DiskNumber $disk.Number -UseMaximumSize -DriveLetter $drive
    $volume=$partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel FLOWTARY_TEST -Confirm:$false
    if($volume.FileSystem -ne 'NTFS' -or $volume.FileSystemLabel -ne 'FLOWTARY_TEST'){throw 'Test formatting identity mismatch'}
    $driveRoot="${drive}:\"
    # This journal is created only on the freshly formatted test VHD. The indexer
    # itself never creates/resizes a journal or changes a production volume.
    & "$env:SystemRoot/System32/fsutil.exe" usn createjournal m=1048576 a=262144 "${drive}:" > "$RunDirectory/create-test-journal.log"
    if($LASTEXITCODE -ne 0){throw 'Could not prepare test-volume journal'}
    foreach($relative in @('data','workshop','workshop/Flowtary-1.1.2','workshop/Flowtary-1.1.10','workshop/hardlink-bench')){
        $null=New-Item -ItemType Directory -Path (Join-Path $driveRoot $relative)
    }
    for($i=0;$i -lt 128;$i++){[IO.File]::WriteAllText((Join-Path $driveRoot ('data/file-{0:000}.txt' -f $i)),'fixture')}
    for($i=0;$i -lt 128;$i++){[IO.File]::WriteAllText((Join-Path $driveRoot ('workshop/hardlink-bench/bench-{0:000}.exe' -f $i)),'fixture')}
    if($ScaleFixtureCount -gt 0){
        $scaleRoot=Join-Path $driveRoot 'scale-fixture'
        $null=New-Item -ItemType Directory -Path $scaleRoot
        $shardCount=[Math]::Min(100,[Math]::Max(1,[Math]::Ceiling($ScaleFixtureCount/100)))
        for($shard=0;$shard -lt $shardCount;$shard++){$null=New-Item -ItemType Directory -Path (Join-Path $scaleRoot ('d{0:000}' -f $shard))}
        for($i=0;$i -lt $ScaleFixtureCount;$i++){
            $shard=[Math]::Floor($i/100)
            [IO.File]::WriteAllText((Join-Path $scaleRoot ('d{0:000}/fixture-{1:00000}.dat' -f $shard,$i)),'fixture')
        }
    }
    $firstExe=Join-Path $driveRoot 'workshop/Flowtary-1.1.2/flowtary.exe'
    $secondExe=Join-Path $driveRoot 'workshop/Flowtary-1.1.10/flowtary.exe'
    [IO.File]::WriteAllText($firstExe,'fixture');[IO.File]::WriteAllText($secondExe,'fixture')
    $null=New-Item -ItemType HardLink -Path (Join-Path $driveRoot 'workshop/alias-flowtary.exe') -Target $firstExe
    [IO.File]::WriteAllText((Join-Path $driveRoot 'data/微信测试.txt'),'fixture')
    $logical=Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='${drive}:'"
    $serial=[Convert]::ToUInt32($logical.VolumeSerialNumber,16)
    $volume=Get-Volume -DriveLetter $drive
    $manifest=@{schema=2;machine_name=$env:COMPUTERNAME;rollback_snapshot='';volume_guid=[string]$volume.Path;drive=$drive;
        volume_label='FLOWTARY_TEST';authorized_host_test=$true;backing_vhd=$vhd;volume_serial=$serial;force_enum_usn=[bool]$VerifyInitialResume;
        directory_link_walk=[bool]$DirectoryLinkWalk}
    $manifest | ConvertTo-Json | Set-Content -LiteralPath "$RunDirectory/host-vhd-manifest.json" -Encoding utf8NoBOM
    State 'post-fixture-six-metric-preflight'
    $postCheck=Get-HostMetricsPreflightWithRetry 'post-fixture-preflight' 'post-fixture'
    $postFixture=$postCheck.report;$postPhase=$postCheck.phase;$postSample=$postCheck.sample
    State 'post-fixture-preflight-accepted' @{sample=$postSample;phase=$postPhase}
    $argsBase=@('collect','--host-test-vhd','--manifest',"$RunDirectory/host-vhd-manifest.json",'--snapshot',"$RunDirectory/test.fti")
    if($VerifyInitialResume){
        State 'bounded-file-id-mft-stream-probe'
        if($CaptureFileRecordPayload){
            $rawArgs=@('probe-volume','--host-test-vhd','--manifest',"$RunDirectory/host-vhd-manifest.json",'--mechanism','enum-usn','--pages','1','--probe-frns','27','--capture-file-record-payload-on-error','--metrics',"$RunDirectory/raw-file-record-probe.jsonl")
            $rawJob=StartTool $binary $rawArgs
            if(-not $rawJob.process.WaitForExit(45000)){$rawJob.process.Kill();throw 'Raw FILE record probe exceeded its bounded test window'}
            $rawJob.stdout.Result | Set-Content -LiteralPath "$RunDirectory/raw-file-record-probe-report.json" -Encoding utf8
            $rawJob.stderr.Result | Set-Content -LiteralPath "$RunDirectory/raw-file-record-probe-report-stderr.log" -Encoding utf8
            $rawReport=Get-Content -LiteralPath "$RunDirectory/raw-file-record-probe-report.json" -Raw | ConvertFrom-Json
            $rawError=[string]$rawReport.result.Err
            if($rawJob.process.ExitCode -eq 0 -or -not $rawError.Contains('bounded_test_vhd_record_payload_hex=')){throw "Raw FILE record diagnostic did not capture the expected parse failure: $rawError"}
            $rawHex=[regex]::Match($rawError,'bounded_test_vhd_record_payload_hex=([0-9a-f]+)').Groups[1].Value
            if($rawHex.Length -lt 24 -or ($rawHex.Length % 2) -ne 0){throw 'Raw FILE record diagnostic returned malformed/truncated hex'}
            State 'raw-file-record-payload-captured' @{error=$rawError.Substring(0,$rawError.IndexOf('; bounded_test_vhd_record_payload_hex='));payload_hex_chars=$rawHex.Length;io=$rawReport.native_io_stats}
            $rawArgs2=@('probe-volume','--host-test-vhd','--manifest',"$RunDirectory/host-vhd-manifest.json",'--mechanism','enum-usn','--pages','1','--probe-frns','27','--capture-file-record-payload-on-error','--metrics',"$RunDirectory/raw-file-record-repeat.jsonl")
            $rawJob2=StartTool $binary $rawArgs2
            if(-not $rawJob2.process.WaitForExit(45000)){$rawJob2.process.Kill();throw 'Repeated raw FILE record probe exceeded its bounded test window'}
            $rawJob2.stdout.Result | Set-Content -LiteralPath "$RunDirectory/raw-file-record-repeat-report.json" -Encoding utf8
            $rawJob2.stderr.Result | Set-Content -LiteralPath "$RunDirectory/raw-file-record-repeat-report-stderr.log" -Encoding utf8
            $rawReport2=Get-Content -LiteralPath "$RunDirectory/raw-file-record-repeat-report.json" -Raw | ConvertFrom-Json
            $rawError2=[string]$rawReport2.result.Err
            if($rawJob2.process.ExitCode -eq 0 -or -not $rawError2.Contains('bounded_test_vhd_record_payload_hex=')){throw "Repeated raw FILE record diagnostic failed: $rawError2"}
            $rawHex2=[regex]::Match($rawError2,'bounded_test_vhd_record_payload_hex=([0-9a-f]+)').Groups[1].Value
            if($rawHex2.Length -lt 24 -or ($rawHex2.Length % 2) -ne 0){throw 'Repeated raw FILE record diagnostic returned malformed/truncated hex'}
            State 'raw-file-record-payload-repeat-compared' @{payload_bytes=$rawHex.Length/2;identical=($rawHex -ceq $rawHex2);first_native_io=$rawReport.native_io_stats;second_native_io=$rawReport2.native_io_stats}
        }
        $probePaths=[Collections.Generic.List[string]]::new()
        $probePaths.Add('workshop/alias-flowtary.exe')
        for($i=0;$i -lt 128;$i++){$probePaths.Add(('workshop/hardlink-bench/bench-{0:000}.exe' -f $i))}
        $directoryArgs=@('probe-volume','--host-test-vhd','--manifest',"$RunDirectory/host-vhd-manifest.json",'--mechanism','enum-usn','--pages','4')
        foreach($relative in @('workshop','workshop/Flowtary-1.1.2','workshop/Flowtary-1.1.10','workshop/hardlink-bench')){
            $directoryArgs+=@('--probe-directory-batch',$relative)
        }
        $directoryArgs+=@('--probe-directory-tree','--tree-max-entries','100000','--tree-max-directories','4096','--tree-max-depth','16','--tree-max-seconds','30')
        $directoryArgs+=@('--metrics',"$RunDirectory/directory-batch-probe.jsonl")
        $directoryReport=FinishTool (StartTool $binary $directoryArgs) 'directory-batch-probe-report' | ConvertFrom-Json
        $directoryProbes=@($directoryReport.result.Ok.directory_batch_probe)
        if($directoryProbes.Count -ne 4){throw 'Batched directory probe did not report all four fixture directories'}
        $treeProbe=$directoryReport.result.Ok.directory_tree_probe
        if(-not $treeProbe -or -not $treeProbe.complete){throw 'Bounded directory-tree probe did not complete within its explicit limits'}
        if($treeProbe.files -lt (256+$ScaleFixtureCount)){throw 'Directory-tree probe omitted expected fixture files'}
        $workshopDirectory=@($directoryProbes|Where-Object relative_directory -eq 'workshop')[0]
        $versionDirectory=@($directoryProbes|Where-Object relative_directory -eq 'workshop/Flowtary-1.1.2')[0]
        $versionEntry=@($workshopDirectory.sample_entries|Where-Object name -eq 'Flowtary-1.1.2')[0]
        $aliasEntry=@($workshopDirectory.sample_entries|Where-Object name -eq 'alias-flowtary.exe')[0]
        $originalEntry=@($versionDirectory.sample_entries|Where-Object name -eq 'flowtary.exe')[0]
        if(-not $versionEntry -or -not $aliasEntry -or -not $originalEntry){throw 'Batched directory probe omitted a known fixture name'}
        if([string]$versionEntry.file_id -ne [string]$versionDirectory.directory_file_id){throw 'Directory entry FileId did not match the opened directory FileId'}
        if([string]$aliasEntry.file_id -ne [string]$originalEntry.file_id){throw 'Batched directory FileIds did not merge the known cross-directory hardlink'}
        $enumSamples=@($directoryReport.result.Ok.enum_file_id_samples)
        $workshopFrn=@($enumSamples|Where-Object name -eq 'workshop')[0]
        $versionFrn=@($enumSamples|Where-Object name -eq 'Flowtary-1.1.2')[0]
        # USN enumeration may emit only one FILE_NAME link for a multi-link
        # file. Match the alias name that the USN page actually reports, then
        # prove the other directory entry resolves to the same FileId through
        # the batched directory API above.
        $aliasFrn=@($enumSamples|Where-Object { $_.name -eq 'alias-flowtary.exe' -and [string]$_.parent_frn -eq [string]$workshopDirectory.directory_file_id })[0]
        if(-not $workshopFrn -or -not $versionFrn -or -not $aliasFrn){throw 'USN page omitted a known directory/FileId comparison fixture'}
        if([string]$workshopFrn.frn -ne [string]$workshopDirectory.directory_file_id -or
            [string]$versionFrn.frn -ne [string]$versionDirectory.directory_file_id -or
            [string]$aliasFrn.frn -ne [string]$aliasEntry.file_id -or
            [string]$originalEntry.file_id -ne [string]$aliasFrn.frn){throw 'Directory FileId records did not match USN FRNs'}
        $usnHasOriginalHardlinkName=@($enumSamples|Where-Object { $_.name -eq 'flowtary.exe' -and [string]$_.frn -eq [string]$aliasEntry.file_id }).Count -gt 0
        State 'batched-directory-fileids-verified' @{directories=$directoryProbes.Count;entries=($directoryProbes|Measure-Object entries -Sum).Sum;
            api_requests=($directoryProbes|Measure-Object api_requests -Sum).Sum;elapsed_us=($directoryProbes|Measure-Object elapsed_us -Sum).Sum;
            hardlink_file_id=$aliasEntry.file_id;usn_frn_file_id_mapping_verified=$true;usn_emitted_alias_name=$aliasFrn.name;
            usn_omitted_one_hardlink_name=(-not $usnHasOriginalHardlinkName)}

        $probeArgs=@('probe-volume','--host-test-vhd','--manifest',"$RunDirectory/host-vhd-manifest.json",'--mechanism','enum-usn','--pages','1','--probe-enum-duplicate-names')
        foreach($relative in $probePaths){$probeArgs+=@('--probe-hardlink-path',$relative)}
        $probeArgs+=@('--metrics',"$RunDirectory/hardlink-path-probe.jsonl")
        $probeError=$null
        try{$null=FinishTool (StartTool $binary $probeArgs) 'hardlink-path-probe-report'}catch{$probeError=$_.Exception.Message}
        if(-not (Test-Path -LiteralPath "$RunDirectory/hardlink-path-probe-report.json")){throw "Hardlink path probe produced no report: $probeError"}
        $probeReport=Get-Content -LiteralPath "$RunDirectory/hardlink-path-probe-report.json" -Raw | ConvertFrom-Json
        State 'bounded-hardlink-path-probe-complete' @{error=$probeError;result=$probeReport.result;io=$probeReport.native_io_stats}
        if($probeError){throw "Bounded hardlink path probe failed: $probeError"}
        $hardlinkProbes=@($probeReport.result.Ok.hardlink_name_probe)
        if($hardlinkProbes.Count -ne 129 -or $hardlinkProbes[0].link_count -ne 2){throw 'FindFirstFileNameW did not return both known hardlink names'}
        foreach($probe in $hardlinkProbes | Select-Object -Skip 1){if($probe.link_count -ne 1){throw 'A singleton fixture returned an unexpected hardlink count'}}
        $mftArgs=@('probe-volume','--host-test-vhd','--manifest',"$RunDirectory/host-vhd-manifest.json",'--mechanism','enum-usn','--pages','1','--probe-mft-by-file-id','--metrics',"$RunDirectory/mft-file-id-probe.jsonl")
        $mftProbeError=$null
        try{$null=FinishTool (StartTool $binary $mftArgs) 'mft-file-id-probe-report'}catch{$mftProbeError=$_.Exception.Message}
        State 'optional-mft-file-id-probe-complete' @{error=$mftProbeError;report=(Join-Path $RunDirectory 'mft-file-id-probe-report.json')}
    }
    State 'native-initial-collection' $manifest
    Start-PoolTrace
    if($VerifyInitialResume){
        $expectedStopObserved=$false
        try{
            $null=FinishTool (StartTool $binary ($argsBase+@('--metrics',"$RunDirectory/initial-interrupted.jsonl",'--checkpoint-seconds','1','--test-stop-after-initial-checkpoint'))) 'initial-interrupted-report'
        }catch{$expectedStopObserved=$true}
        if(-not $expectedStopObserved){throw 'Fault-injection run completed without stopping after its checkpoint'}
        $interrupted=Get-Content -LiteralPath "$RunDirectory/initial-interrupted-report.json" -Raw | ConvertFrom-Json
        if(-not $interrupted.result.Err -or $interrupted.result.Err -notlike '*intentional host-VHD stop after durable initial checkpoint*'){
            throw 'Initial run failed before reaching the intentional durable-checkpoint stop'
        }
        if($DirectoryLinkWalk){
            $walkStats=$interrupted.native_io_stats
            if(-not $walkStats -or $walkStats.directory_batch_requests -lt 1 -or
                $walkStats.directory_handles_opened -lt 1 -or $walkStats.directory_entries_seen -lt 1){
                throw 'Directory-provider interruption happened before a durable directory batch was recorded'
            }
            if($walkStats.directory_handles_active -ne 0 -or $walkStats.directory_handles_peak -ne 1){
                throw 'Directory-provider interruption did not close its single active directory handle'
            }
            if($walkStats.hardlink_name_requests -ne 0){throw 'Directory provider unexpectedly used per-file hard-link name calls'}
        }
        if(-not (Test-Path -LiteralPath "$RunDirectory/test.partial.fti") -or (Test-Path -LiteralPath "$RunDirectory/test.fti")){
            throw 'Interrupted build did not leave only its private partial checkpoint'
        }
        State 'resume-initial-after-durable-checkpoint'
        $initial=FinishTool (StartTool $binary ($argsBase+@('--metrics',"$RunDirectory/initial-resume.jsonl",'--resume-initial','--checkpoint-seconds','1'))) 'initial-resume-report'
    }else{
        $initial=FinishTool (StartTool $binary ($argsBase+@('--metrics',"$RunDirectory/initial.jsonl"))) 'initial-report'
    }
    $initialReport=$initial | ConvertFrom-Json
    if($initialReport.result.Err -or $initialReport.native_volume_open_attempts -ne 1){throw 'Initial collection did not complete with one verified volume handle'}
    if($VerifyInitialResume -and -not $initialReport.result.Ok.resumed_initial){throw 'Initial generation was not resumed from its partial checkpoint'}
    if($DirectoryLinkWalk -and ($initialReport.native_io_stats.directory_batch_requests -lt 1 -or
        $initialReport.native_io_stats.directory_entries_seen -lt 1 -or $initialReport.native_io_stats.hardlink_name_requests -ne 0 -or
        $initialReport.native_io_stats.directory_handles_active -ne 0 -or $initialReport.native_io_stats.directory_handles_peak -ne 1)){
        throw 'Resumed directory-batched reconciliation did not complete through the expected provider path'
    }
    $initialQuery=FinishTool (StartTool $binary @('query','--snapshot',"$RunDirectory/test.fti",'--json','{"words":["flowtary"],"extension":"exe","limit":10}')) 'initial-query'
    if(($initialQuery | ConvertFrom-Json).hits.Count -ne 3){throw 'Initial hardlink/name query did not return all three executable names'}
    State 'native-resume-and-live-mutations'
    $resume=StartTool $binary ($argsBase+@('--metrics',"$RunDirectory/resume.jsonl",'--resume','--watch-seconds',[string]$WatchSeconds))
    Start-Sleep -Seconds 6
    [IO.File]::Move((Join-Path $driveRoot 'data/file-000.txt'),(Join-Path $driveRoot 'data/renamed-native.txt'))
    [IO.File]::Delete((Join-Path $driveRoot 'data/file-001.txt'))
    [IO.File]::WriteAllText((Join-Path $driveRoot 'data/created-native.txt'),'fixture')
    $null=New-Item -ItemType HardLink -Path (Join-Path $driveRoot 'workshop/added-flowtary.exe') -Target $secondExe
    $null=FinishTool $resume 'resume-report' ([Math]::Max(45000,($WatchSeconds+40)*1000))
    Stop-PoolTrace
    $changed=FinishTool (StartTool $binary @('query','--snapshot',"$RunDirectory/test.fti",'--json','{"words":["native"],"limit":10}')) 'changed-query'
    if(($changed | ConvertFrom-Json).hits.Count -ne 2){throw 'USN resume did not publish the renamed and created fixtures'}
    $links=FinishTool (StartTool $binary @('query','--snapshot',"$RunDirectory/test.fti",'--json','{"words":["flowtary"],"extension":"exe","limit":10}')) 'changed-links-query'
    if(($links | ConvertFrom-Json).hits.Count -ne 4){throw 'Live hardlink reconciliation did not publish all four names'}
    State 'cooldown-after-indexer-exit'
    $null=FinishTool (StartTool $binary @('metrics','--seconds','5','--output',"$RunDirectory/after-indexer.jsonl")) 'after-indexer-report'
    Write-HostResourceSummary
    State 'completed' @{scope='128 MiB host VHD only; no physical-volume enumeration';backing=$vhd;binary_sha256=$binaryHash;
        scale_fixture_count=$ScaleFixtureCount;directory_tree_probe=if($VerifyInitialResume){$treeProbe}else{$null};
        pool_trace=if($CapturePoolTrace){@{logger_name=$poolLoggerName;trace=$poolMergedTrace;summary=$poolSummary;window='pool-trace-window.json';profile='PROC_THREAD+LOADER+POOL';stackwalk='PoolAllocation+PoolFree';buffer_size_kib=64;minimum_buffers=8;maximum_buffers=64;maximum_file_mib=128}}else{$null}}
}catch{
    # Close and persist a started ETW session before generating the failure
    # summary, so partial runs still include the trace window and its evidence.
    if($poolTraceStarted){try{Stop-PoolTrace}catch{$_ | Out-String | Set-Content -LiteralPath (Join-Path $RunDirectory 'pool-trace-cleanup-error.log') -Encoding utf8}}
    try{Write-HostResourceSummary}catch{$_ | Out-String | Set-Content -LiteralPath (Join-Path $RunDirectory 'resource-summary-error.log') -Encoding utf8}
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $RunDirectory 'failure.log') -Encoding utf8
    State 'failed' $_.Exception.Message
    exit 1
}finally{
    if($poolTraceStarted){try{Stop-PoolTrace}catch{$_ | Out-String | Set-Content -LiteralPath (Join-Path $RunDirectory 'pool-trace-cleanup-error.log') -Encoding utf8}}
    foreach($process in $children){if(-not $process.HasExited){$process.Kill();$null=$process.WaitForExit(3000)};$process.Dispose()}
    if($attached){try{Dismount-DiskImage -ImagePath $vhd -ErrorAction Stop}catch{$_ | Out-String | Set-Content -LiteralPath (Join-Path $RunDirectory 'detach-warning.log') -Encoding utf8}}
}
