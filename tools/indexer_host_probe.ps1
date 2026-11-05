#requires -Version 7.0
#requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory,[ValidateRange(1,4)][int]$Pages=1,
    [ValidateSet('file-layout','enum-usn')][string]$Mechanism='file-layout',[switch]$ProbeFileIdLayout,[switch]$ProbeMftRecord,
    [switch]$ProbeMftStreamZero,[switch]$ProbeMftByFileId,[ValidateRange(0,128)][int]$ProbeMftBatch=0,[UInt64]$StartFrn=0,
    [UInt64[]]$ProbeFrns=@())
$ErrorActionPreference='Stop'
$repoRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$allowed=[IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer'))+[IO.Path]::DirectorySeparatorChar
$RunDirectory=[IO.Path]::GetFullPath($RunDirectory)
if(-not $RunDirectory.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $RunDirectory)){throw 'Use a new workspace logs/indexer run directory'}
$null=New-Item -ItemType Directory -Path $RunDirectory
$binary=Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
Copy-Item -LiteralPath $binary -Destination "$RunDirectory/indexer-tested.exe"
$binary=Join-Path $RunDirectory 'indexer-tested.exe'
$process=$null
function State([string]$Phase,[object]$Detail=$null){@{schema=1;phase=$Phase;detail=$Detail;updated_utc=(Get-Date).ToUniversalTime().ToString('o');pid=$PID}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$RunDirectory/state.json" -Encoding utf8}
try{
    State 'preparing-bounded-C-probe'
    & "$env:SystemRoot/System32/fltmc.exe" filters > "$RunDirectory/filters.txt"
    & "$env:SystemRoot/System32/fltmc.exe" instances -v C: > "$RunDirectory/C-filter-instances.txt"
    $volume=Get-Volume -DriveLetter C
    $logical=Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'"
    if($volume.FileSystem -ne 'NTFS'){throw 'The explicitly selected C volume is not NTFS'}
    @{schema=3;machine_name=$env:COMPUTERNAME;rollback_snapshot='';volume_guid=[string]$volume.Path;drive='C';
        volume_label=[string]$volume.FileSystemLabel;authorized_host_test=$true;backing_vhd=$null;
        volume_serial=[Convert]::ToUInt32($logical.VolumeSerialNumber,16)} |
        ConvertTo-Json | Set-Content -LiteralPath "$RunDirectory/manifest.json" -Encoding utf8NoBOM
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$binary;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($arg in @('probe-volume','--host-volume-probe','--manifest',"$RunDirectory/manifest.json",'--pages',"$Pages",'--mechanism',$Mechanism,'--metrics',"$RunDirectory/samples.jsonl")){$info.ArgumentList.Add($arg)}
    if($ProbeFileIdLayout){$info.ArgumentList.Add('--probe-file-id-layout')}
    if($ProbeMftRecord){$info.ArgumentList.Add('--probe-mft-record')}
    if($ProbeMftStreamZero){$info.ArgumentList.Add('--probe-mft-stream-zero')}
    if($ProbeMftByFileId){$info.ArgumentList.Add('--probe-mft-by-file-id')}
    if($ProbeMftBatch -gt 0){
        if($Mechanism -ne 'enum-usn'){throw 'MFT fragment batch requires -Mechanism enum-usn'}
        $info.ArgumentList.Add('--probe-mft-batch');$info.ArgumentList.Add([string]$ProbeMftBatch)
    }
    if($ProbeFrns.Count -gt 4){throw 'At most four explicit MFT references may be probed'}
    if($ProbeFrns.Count -gt 0){$info.ArgumentList.Add('--probe-frns');$info.ArgumentList.Add(($ProbeFrns -join ','))}
    if($StartFrn -gt 0){$info.ArgumentList.Add('--start-frn');$info.ArgumentList.Add([string]$StartFrn)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info;$null=$process.Start()
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    State 'bounded-read-running' @{child_pid=$process.Id;maximum_pages=$Pages;buffer_bytes=262144;volume='C:'}
    if(-not $process.WaitForExit(30000)){$process.Kill();throw 'Bounded C probe exceeded its process deadline'}
    $stdout.Result|Set-Content -LiteralPath "$RunDirectory/report.json" -Encoding utf8
    $stderr.Result|Set-Content -LiteralPath "$RunDirectory/stderr.log" -Encoding utf8
    if($process.ExitCode -ne 0){throw "Probe failed or was cancelled: $($stderr.Result)"}
    $report=$stdout.Result|ConvertFrom-Json
    if($report.cancelled_by_guard_or_caller -or $report.native_volume_open_attempts -ne 1 -or $report.result.Ok.complete_index_built){throw 'Probe access/guard audit failed'}
    State 'completed' @{scope='explicit C volume, at most four name-metadata pages, no complete index';binary_sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash;result=$report.result}
}catch{$_|Out-String|Set-Content -LiteralPath "$RunDirectory/failure.log" -Encoding utf8;State 'failed' $_.Exception.Message;exit 1}
finally{if($process){if(-not $process.HasExited){$process.Kill();$null=$process.WaitForExit(3000)};$process.Dispose()}}
