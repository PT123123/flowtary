#requires -Version 7.0
# Read-only, bounded FindFirstFileNameW resource probe for Flowtary's owned
# C:\workshop\*.exe scope. This never opens a volume device or modifies files.
[CmdletBinding()]
param(
    [string]$Root = 'C:\workshop',
    [string]$OutputDirectory,
    [ValidateRange(1, 128)][int]$MaximumFiles = 128,
    [ValidateRange(1, 146)][int]$Passes = 146
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$logsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer')) + [IO.Path]::DirectorySeparatorChar
$rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\')
if (-not $rootPath.Equals('C:\workshop', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'This diagnostic is confined to C:\workshop'
}
$rootItem = Get-Item -LiteralPath $rootPath -Force
if (-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'C:\workshop must be a real directory, not a reparse point'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('logs/indexer/workshop-link-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $OutputDirectory.StartsWith($logsRoot, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $OutputDirectory)) {
    throw 'OutputDirectory must be a new directory inside logs/indexer'
}
$null = New-Item -ItemType Directory -Path $OutputDirectory
$metricsExe = Join-Path $repoRoot 'indexer/target/release/flowtary-indexer.exe'
if (-not (Test-Path -LiteralPath $metricsExe)) { throw 'Build indexer/target/release/flowtary-indexer.exe first' }

if (-not ('FlowtaryWorkshopLinkProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class FlowtaryWorkshopLinkProbe
{
    private const int ERROR_HANDLE_EOF = 38;

    [StructLayout(LayoutKind.Sequential)]
    private struct PerformanceInformation
    {
        public uint cb;
        public UIntPtr CommitTotal, CommitLimit, CommitPeak;
        public UIntPtr PhysicalTotal, PhysicalAvailable, SystemCache;
        public UIntPtr KernelTotal, KernelPaged, KernelNonpaged, PageSize;
        public uint HandleCount, ProcessCount, ThreadCount;
    }

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Auto)]
    private struct MemoryStatusEx
    {
        public uint dwLength, dwMemoryLoad;
        public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
        public ulong ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
    }

    private sealed class SearchHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public SearchHandle() : base(true) { }
        protected override bool ReleaseHandle() { return FindClose(handle); }
    }

    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern SearchHandle FindFirstFileNameW(string fileName, uint flags,
        ref uint stringLength, StringBuilder linkName);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern bool FindNextFileNameW(SearchHandle handle,
        ref uint stringLength, StringBuilder linkName);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool FindClose(IntPtr handle);
    [DllImport("psapi.dll", SetLastError=true)]
    private static extern bool GetPerformanceInfo(out PerformanceInformation info, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx info);

    public static ulong[] GetMachineState()
    {
        PerformanceInformation p;
        uint size = (uint)Marshal.SizeOf(typeof(PerformanceInformation));
        if (!GetPerformanceInfo(out p, size)) throw new Win32Exception(Marshal.GetLastWin32Error(), "GetPerformanceInfo");
        MemoryStatusEx m = new MemoryStatusEx();
        m.dwLength = (uint)Marshal.SizeOf(typeof(MemoryStatusEx));
        if (!GlobalMemoryStatusEx(ref m)) throw new Win32Exception(Marshal.GetLastWin32Error(), "GlobalMemoryStatusEx");
        ulong page = p.PageSize.ToUInt64();
        return new ulong[] {
            p.CommitTotal.ToUInt64() * page, p.CommitLimit.ToUInt64() * page,
            m.ullAvailPhys, m.ullTotalPhys, p.HandleCount,
            p.KernelPaged.ToUInt64() * page, p.KernelNonpaged.ToUInt64() * page
        };
    }

    public static int EnumerateLinks(string path)
    {
        const int capacity = 32768;
        uint length = capacity;
        var name = new StringBuilder(capacity);
        using (SearchHandle handle = FindFirstFileNameW(path, 0, ref length, name))
        {
            if (handle == null || handle.IsInvalid)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "FindFirstFileNameW");
            int count = 1;
            for (;;)
            {
                length = capacity;
                name.Clear();
                if (FindNextFileNameW(handle, ref length, name))
                {
                    if (++count > 128) throw new InvalidOperationException("link count exceeded 128");
                    continue;
                }
                int error = Marshal.GetLastWin32Error();
                if (error == ERROR_HANDLE_EOF) return count;
                throw new Win32Exception(error, "FindNextFileNameW");
            }
        }
    }
}
'@
}

function Get-MachineState {
    $v = [FlowtaryWorkshopLinkProbe]::GetMachineState()
    return [ordered]@{
        commit_bytes=[ulong]$v[0]; commit_limit_bytes=[ulong]$v[1]
        physical_available_bytes=[ulong]$v[2]; physical_total_bytes=[ulong]$v[3]
        total_process_handles=[ulong]$v[4]; pool_paged_bytes=[ulong]$v[5]; pool_nonpaged_bytes=[ulong]$v[6]
        memory_pressure_ratio=1.0-([double]$v[2]/[double]$v[3])
    }
}

function Start-Metrics([int]$Seconds, [string]$Path) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $metricsExe
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($arg in @('metrics', '--seconds', "$Seconds", '--output', $Path)) { $info.ArgumentList.Add($arg) }
    $proc = [Diagnostics.Process]::new()
    $proc.StartInfo = $info
    $null = $proc.Start()
    return @{ process=$proc; stdout=$proc.StandardOutput.ReadToEndAsync(); stderr=$proc.StandardError.ReadToEndAsync() }
}

function WeightedMean($Samples, [string]$Property) {
    $sum = 0.0; $covered = 0.0
    for ($i = 1; $i -lt $Samples.Count; $i++) {
        $dt = [double]($Samples[$i].unix_ms - $Samples[$i - 1].unix_ms) / 1000.0
        $value = $Samples[$i].$Property
        if ($dt -gt 0 -and $null -ne $value) { $sum += [double]$value * $dt; $covered += $dt }
    }
    if ($covered -le 0) { return $null }
    return $sum / $covered
}

function Summarize-Phase($Samples, [long]$StartMs, [long]$EndMs) {
    $window = @($Samples | Where-Object { $_.unix_ms -ge $StartMs -and $_.unix_ms -le $EndMs })
    if ($window.Count -lt 2) { return @{ samples=$window.Count; counter_error_samples=$null } }
    $base = $window[0]
    $maximum = { param($name) ($window | Measure-Object -Property $name -Maximum).Maximum }
    $minimum = { param($name) ($window | Measure-Object -Property $name -Minimum).Minimum }
    return [ordered]@{
        samples = $window.Count
        counter_error_samples = @($window | Where-Object { @($_.counter_errors.PSObject.Properties).Count -gt 0 }).Count
        commit_peak_growth_bytes = [double](& $maximum 'commit_bytes') - $base.commit_bytes
        maximum_pressure_ratio = & $maximum 'memory_pressure_ratio'
        minimum_available_bytes = & $minimum 'physical_available_bytes'
        system_handles_peak_growth = [double](& $maximum 'system_handles') - $base.system_handles
        paged_pool_peak_growth_bytes = [double](& $maximum 'pool_paged_bytes') - $base.pool_paged_bytes
        nonpaged_pool_peak_growth_bytes = [double](& $maximum 'pool_nonpaged_bytes') - $base.pool_nonpaged_bytes
        cpu_weighted_mean_percent = WeightedMean $window 'cpu_percent'
        disk_busy_weighted_mean_percent = WeightedMean $window 'disk_busy_percent'
        disk_io_weighted_mean_bytes_per_s = (WeightedMean $window 'disk_read_bytes_per_s') + (WeightedMean $window 'disk_write_bytes_per_s')
        maximum_pages_input_per_s = & $maximum 'pages_input_per_s'
    }
}

$metricsJob = $null
try {
    $preflightPath = Join-Path $OutputDirectory 'preflight.jsonl'
    $preflightJob = Start-Metrics 5 $preflightPath
    if (-not $preflightJob.process.WaitForExit(15000)) { $preflightJob.process.Kill(); throw 'Preflight sampler timed out' }
    if ($preflightJob.process.ExitCode -ne 0) { throw "Preflight sampler failed: $($preflightJob.stderr.Result)" }
    $preflight = $preflightJob.stdout.Result | ConvertFrom-Json
    $preflightPhase = @($preflight.phases | Where-Object phase -eq 'machine_observation' | Select-Object -Last 1)[0]
    $reasons = [Collections.Generic.List[string]]::new()
    if (-not $preflightPhase -or $preflightPhase.samples -lt 5) { $reasons.Add('preflight did not cover five seconds and five samples') }
    elseif ($preflightPhase.counter_error_samples -ne 0) { $reasons.Add('required performance counters unavailable') }
    if ($preflightPhase) {
        if ($preflightPhase.maximum_commit_ratio -gt 0.85) { $reasons.Add('commit ratio above 85%') }
        if ($preflightPhase.maximum_pressure_ratio -gt 0.90) { $reasons.Add('memory pressure above 90%') }
        if ($preflightPhase.minimum_available_bytes -lt 1.5GB) { $reasons.Add('available memory below 1.5 GiB') }
        if ($preflightPhase.max_pages_input_per_s -gt 10000) { $reasons.Add('paging input above 10,000 pages/second') }
        if ($preflightPhase.mean_cpu_percent -gt 85) { $reasons.Add('mean CPU above 85%') }
        if ($preflightPhase.mean_disk_busy_percent -gt 90) { $reasons.Add('mean disk busy above 90%') }
    }
    if ($reasons.Count) { throw "Host preflight rejected the path probe: $($reasons -join '; ')" }

    # 45-second whole-machine observation covers warm baseline, directory
    # discovery, API loop, and cooldown. The API loop is capped at 8,192 calls.
    $metricsPath = Join-Path $OutputDirectory 'machine.jsonl'
    $metricsJob = Start-Metrics 45 $metricsPath
    Start-Sleep -Seconds 5
    $baselineEnd = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

    # Directory discovery is separately time/count/depth bounded and remains
    # strictly under C:\workshop; reparse points are never traversed.
    $files = [Collections.Generic.List[string]]::new()
    $stack = [Collections.Generic.Stack[object]]::new()
    $stack.Push(@{ path=$rootPath; depth=0 })
    $dirsVisited = 0; $enumerationErrors = 0
    $discoveryMachineBefore = Get-MachineState
    $discoveryStart = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $discoveryDeadline = [DateTime]::UtcNow.AddSeconds(12)
    while ($stack.Count -gt 0 -and $files.Count -lt $MaximumFiles -and $dirsVisited -lt 4096 -and [DateTime]::UtcNow -lt $discoveryDeadline) {
        $node = $stack.Pop(); $dirsVisited++
        try {
            foreach ($file in [IO.Directory]::EnumerateFiles($node.path, '*.exe', [IO.SearchOption]::TopDirectoryOnly)) {
                if ($files.Count -ge $MaximumFiles) { break }
                $files.Add($file)
            }
        } catch { $enumerationErrors++ }
        if ($node.depth -ge 12 -or $files.Count -ge $MaximumFiles) { continue }
        try {
            foreach ($dir in [IO.Directory]::EnumerateDirectories($node.path, '*', [IO.SearchOption]::TopDirectoryOnly)) {
                if ($stack.Count + $dirsVisited -ge 4096) { break }
                try {
                    if (([IO.File]::GetAttributes($dir) -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                        $stack.Push(@{ path=$dir; depth=($node.depth + 1) })
                    }
                } catch { $enumerationErrors++ }
            }
        } catch { $enumerationErrors++ }
    }
    $discoveryEnd = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $discoveryMachineAfter = Get-MachineState
    if ($files.Count -eq 0) { throw 'No executable files were found inside the bounded C:\workshop walk' }

    $apiStart = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $callCount = 0; $linkCount = 0; $apiErrors = 0
    $processBefore = Get-Process -Id $PID
    $machineBefore = Get-MachineState
    $machinePeak = $machineBefore
    $systemHandleBaselineSample = (Get-Content -LiteralPath $metricsPath -Tail 1) | ConvertFrom-Json
    $systemHandleBaseline = [long]$systemHandleBaselineSample.system_handles
    $systemHandlePeak = $systemHandleBaseline
    $guardPolls = 0
    for ($pass = 0; $pass -lt $Passes; $pass++) {
        foreach ($path in $files) {
            try { $linkCount += [FlowtaryWorkshopLinkProbe]::EnumerateLinks($path) }
            catch { $apiErrors++; if ($apiErrors -gt 16) { throw 'More than 16 bounded path API errors; stopping the probe' } }
            $callCount++
            if ($callCount -ge 8192) { break }
        }
        if ($callCount -ge 8192) { break }
        if (($pass % 8) -eq 7) {
            $machineNow = Get-MachineState
            $guardPolls++
            if ($machineNow.total_process_handles -gt $machinePeak.total_process_handles) { $machinePeak.total_process_handles=$machineNow.total_process_handles }
            if ($machineNow.commit_bytes -gt $machinePeak.commit_bytes) { $machinePeak.commit_bytes=$machineNow.commit_bytes }
            if ($machineNow.pool_paged_bytes -gt $machinePeak.pool_paged_bytes) { $machinePeak.pool_paged_bytes=$machineNow.pool_paged_bytes }
            if ($machineNow.pool_nonpaged_bytes -gt $machinePeak.pool_nonpaged_bytes) { $machinePeak.pool_nonpaged_bytes=$machineNow.pool_nonpaged_bytes }
            if ($machineNow.memory_pressure_ratio -gt $machinePeak.memory_pressure_ratio) { $machinePeak.memory_pressure_ratio=$machineNow.memory_pressure_ratio }
            if ($machineNow.physical_available_bytes -lt $machinePeak.physical_available_bytes) { $machinePeak.physical_available_bytes=$machineNow.physical_available_bytes }
            if ($machineNow.total_process_handles - $machineBefore.total_process_handles -gt 512) { throw 'Total process-handle growth exceeded 512; stopping the bounded probe' }
            if ($machineNow.commit_bytes - $machineBefore.commit_bytes -gt 1GB) { throw 'System commit growth exceeded 1 GiB; stopping the bounded probe' }
            if ($machineNow.pool_paged_bytes - $machineBefore.pool_paged_bytes -gt 256MB) { throw 'Paged-pool growth exceeded 256 MiB; stopping the bounded probe' }
            if ($machineNow.pool_nonpaged_bytes - $machineBefore.pool_nonpaged_bytes -gt 128MB) { throw 'Nonpaged-pool growth exceeded 128 MiB; stopping the bounded probe' }
            if ($machineNow.memory_pressure_ratio -gt 0.90 -or $machineNow.physical_available_bytes -lt 1.5GB) { throw 'Memory headroom guard stopped the bounded probe' }
            $latestLine = Get-Content -LiteralPath $metricsPath -Tail 1 -ErrorAction SilentlyContinue
            if ($latestLine) {
                $latest = $latestLine | ConvertFrom-Json
                if ($latest.system_handles -gt $systemHandlePeak) { $systemHandlePeak=[long]$latest.system_handles }
                if ($latest.system_handles - $systemHandleBaseline -gt 512) { throw 'System-process handle growth exceeded 512; stopping the bounded probe' }
                if ($latest.cpu_percent -gt 85 -or $latest.disk_busy_percent -gt 90 -or $latest.pages_input_per_s -gt 10000) {
                    throw 'CPU, disk, or paging guard stopped the bounded probe'
                }
            }
        }
    }
    $apiEnd = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $machineAfter = Get-MachineState
    $systemHandleAfterSample = (Get-Content -LiteralPath $metricsPath -Tail 1) | ConvertFrom-Json
    $processAfter = Get-Process -Id $PID
    $cooldownStart = $apiEnd
    Start-Sleep -Seconds 10
    $cooldownEnd = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    if (-not $metricsJob.process.WaitForExit(55000)) { $metricsJob.process.Kill(); throw 'Whole-machine sampler timed out' }
    if ($metricsJob.process.ExitCode -ne 0) { throw "Whole-machine sampler failed: $($metricsJob.stderr.Result)" }
    $machine = $metricsJob.stdout.Result | ConvertFrom-Json
    $samples = @(Get-Content -LiteralPath $metricsPath | ForEach-Object { $_ | ConvertFrom-Json })
    $report = [ordered]@{
        schema = 1
        mode = 'read-only-bounded-c-workshop-executable-link-path-probe'
        root = $rootPath
        discovery = [ordered]@{ files=$files.Count; directories_visited=$dirsVisited; errors=$enumerationErrors; elapsed_ms=($discoveryEnd-$discoveryStart) }
        discovery_machine_boundary = [ordered]@{
            before=$discoveryMachineBefore; after=$discoveryMachineAfter
            total_process_handles_delta=([long]$discoveryMachineAfter.total_process_handles-[long]$discoveryMachineBefore.total_process_handles)
            commit_delta_bytes=([long]$discoveryMachineAfter.commit_bytes-[long]$discoveryMachineBefore.commit_bytes)
            paged_pool_delta_bytes=([long]$discoveryMachineAfter.pool_paged_bytes-[long]$discoveryMachineBefore.pool_paged_bytes)
            nonpaged_pool_delta_bytes=([long]$discoveryMachineAfter.pool_nonpaged_bytes-[long]$discoveryMachineBefore.pool_nonpaged_bytes)
        }
        api = [ordered]@{ files_per_pass=$files.Count; passes=[math]::Ceiling($callCount/[double]$files.Count); calls=$callCount; returned_link_names=$linkCount; errors=$apiErrors; elapsed_ms=($apiEnd-$apiStart); calls_per_second=if($apiEnd -gt $apiStart){$callCount*1000.0/($apiEnd-$apiStart)}else{$null} }
        system_snapshot_at_api_boundary = [ordered]@{
            before=$machineBefore; peak_while_running=$machinePeak; after=$machineAfter; guard_polls=$guardPolls
            total_process_handles_delta=([long]$machineAfter.total_process_handles-[long]$machineBefore.total_process_handles)
            system_process_handles_baseline=$systemHandleBaseline
            system_process_handles_peak=$systemHandlePeak
            system_process_handles_after=[long]$systemHandleAfterSample.system_handles
            system_process_handles_peak_growth=($systemHandlePeak-$systemHandleBaseline)
            commit_delta_bytes=([long]$machineAfter.commit_bytes-[long]$machineBefore.commit_bytes)
            paged_pool_delta_bytes=([long]$machineAfter.pool_paged_bytes-[long]$machineBefore.pool_paged_bytes)
            nonpaged_pool_delta_bytes=([long]$machineAfter.pool_nonpaged_bytes-[long]$machineBefore.pool_nonpaged_bytes)
        }
        powershell_process = [ordered]@{ handles_before=$processBefore.HandleCount; handles_after=$processAfter.HandleCount; handle_delta=($processAfter.HandleCount-$processBefore.HandleCount); private_bytes_before=$processBefore.PrivateMemorySize64; private_bytes_after=$processAfter.PrivateMemorySize64 }
        preflight = [ordered]@{ maximum_commit_ratio=$preflightPhase.maximum_commit_ratio; maximum_pressure_ratio=$preflightPhase.maximum_pressure_ratio; minimum_available_bytes=$preflightPhase.minimum_available_bytes; maximum_pages_input_per_s=$preflightPhase.max_pages_input_per_s; warnings=@($machine.native_host_violations) }
        machine_phase_summary = $machine.phases
        phases = [ordered]@{
            warm_baseline = Summarize-Phase $samples $samples[0].unix_ms $baselineEnd
            discovery = Summarize-Phase $samples $discoveryStart $discoveryEnd
            path_api = Summarize-Phase $samples $apiStart $apiEnd
            cooldown = Summarize-Phase $samples $cooldownStart $cooldownEnd
        }
        counter_error_samples = @($samples | Where-Object { @($_.counter_errors.PSObject.Properties).Count -gt 0 }).Count
        limits = @('Only C:\workshop paths were opened through FindFirstFileNameW; no raw volume handle or file contents were read.', 'At most 128 executable paths, 4,096 directories, depth 12, and 8,192 API calls.', 'Machine-wide deltas include unrelated host activity and cannot alone be attributed to this probe.')
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'report.json') -Encoding utf8
    $report | ConvertTo-Json -Depth 8
} finally {
    if ($metricsJob -and -not $metricsJob.process.HasExited) { $metricsJob.process.Kill(); $null=$metricsJob.process.WaitForExit(3000) }
    if ($metricsJob) { $metricsJob.process.Dispose() }
}
