# Read kernel pool-tag counters without elevation, ETW sessions, or a driver.
param(
    [Parameter(Mandatory)][string]$OutputPath,
    [ValidateRange(5,600)][int]$Seconds = 30,
    [ValidateRange(1,10)][int]$IntervalSeconds = 1
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'logs/indexer')) + [IO.Path]::DirectorySeparatorChar
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (-not $OutputPath.StartsWith($allowedRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'OutputPath must stay inside logs/indexer.' }
if (Test-Path -LiteralPath $OutputPath) { throw 'OutputPath already exists.' }
$null = New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force

$source = @'
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;

public static class FlowtaryPoolTagSnapshot
{
    [StructLayout(LayoutKind.Sequential)]
    private struct PoolTagEntry
    {
        public uint Tag;
        public uint PagedAllocs;
        public uint PagedFrees;
        public UIntPtr PagedUsed;
        public uint NonPagedAllocs;
        public uint NonPagedFrees;
        public UIntPtr NonPagedUsed;
    }

    private sealed class Row
    {
        public string tag { get; set; }
        public ulong paged_used_bytes { get; set; }
        public ulong nonpaged_used_bytes { get; set; }
        public uint paged_allocs { get; set; }
        public uint paged_frees { get; set; }
        public uint nonpaged_allocs { get; set; }
        public uint nonpaged_frees { get; set; }
    }

    private sealed class Snapshot
    {
        public int schema { get; set; } = 1;
        public string timestamp_utc { get; set; }
        public int tag_count { get; set; }
        public ulong tagged_paged_bytes { get; set; }
        public ulong tagged_nonpaged_bytes { get; set; }
        public Row[] selected_tags { get; set; }
    }

    [DllImport("ntdll.dll")]
    private static extern uint NtQuerySystemInformation(int informationClass, IntPtr information,
        uint informationLength, out uint returnLength);

    public static string Capture()
    {
        const int SystemPoolTagInformation = 22;
        const uint StatusInfoLengthMismatch = 0xC0000004;
        uint size = 65536, returned = 0, status = 0;
        IntPtr buffer = IntPtr.Zero;
        try
        {
            for (int attempt = 0; attempt < 12; attempt++)
            {
                buffer = Marshal.AllocHGlobal(checked((int)size));
                status = NtQuerySystemInformation(SystemPoolTagInformation, buffer, size, out returned);
                if (status == 0) break;
                Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero;
                if (status != StatusInfoLengthMismatch)
                    throw new InvalidOperationException("NtQuerySystemInformation failed: 0x" + status.ToString("X8"));
                size = Math.Max(checked(size * 2), checked(returned + 4096));
                if (size > 256 * 1024 * 1024) throw new InvalidOperationException("Pool-tag response exceeded 256 MiB.");
            }
            if (status != 0) throw new InvalidOperationException("Pool-tag response remained too large: 0x" + status.ToString("X8"));

            int count = Marshal.ReadInt32(buffer);
            int stride = Marshal.SizeOf(typeof(PoolTagEntry));
            int firstEntry = 8; // ULONG count followed by naturally aligned 64-bit entries.
            if (count < 0 || (long)firstEntry + (long)count * stride > returned)
                throw new InvalidOperationException("Kernel pool-tag response has an invalid count or length.");

            ulong pagedTotal = 0, nonpagedTotal = 0;
            var entries = new List<PoolTagEntry>(count);
            for (int i = 0; i < count; i++)
            {
                var entry = (PoolTagEntry)Marshal.PtrToStructure(IntPtr.Add(buffer, firstEntry + i * stride), typeof(PoolTagEntry));
                entries.Add(entry);
                pagedTotal += entry.PagedUsed.ToUInt64();
                nonpagedTotal += entry.NonPagedUsed.ToUInt64();
            }

            var chosen = entries
                .Where(e => TagText(e.Tag).StartsWith("WC", StringComparison.OrdinalIgnoreCase) ||
                            TagText(e.Tag).StartsWith("FM", StringComparison.OrdinalIgnoreCase))
                .Concat(entries.OrderByDescending(e => e.NonPagedUsed.ToUInt64()).Take(20))
                .GroupBy(e => e.Tag).Select(g => g.First())
                .OrderByDescending(e => e.NonPagedUsed.ToUInt64())
                .Select(e => new Row {
                    tag = TagText(e.Tag), paged_used_bytes = e.PagedUsed.ToUInt64(),
                    nonpaged_used_bytes = e.NonPagedUsed.ToUInt64(), paged_allocs = e.PagedAllocs,
                    paged_frees = e.PagedFrees, nonpaged_allocs = e.NonPagedAllocs,
                    nonpaged_frees = e.NonPagedFrees
                }).ToArray();

            return JsonSerializer.Serialize(new Snapshot {
                timestamp_utc = DateTime.UtcNow.ToString("o"), tag_count = count,
                tagged_paged_bytes = pagedTotal, tagged_nonpaged_bytes = nonpagedTotal,
                selected_tags = chosen
            });
        }
        finally { if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer); }
    }

    private static string TagText(uint tag) => new string(new[] {
        (char)(tag & 0xff), (char)((tag >> 8) & 0xff),
        (char)((tag >> 16) & 0xff), (char)((tag >> 24) & 0xff)
    });
}
'@
Add-Type -TypeDefinition $source -ErrorAction Stop

$writer = [IO.StreamWriter]::new($OutputPath,$false,[Text.UTF8Encoding]::new($false))
$watch = [Diagnostics.Stopwatch]::StartNew()
$samples = 0
try {
    do {
        $writer.WriteLine([FlowtaryPoolTagSnapshot]::Capture())
        $writer.Flush()
        $samples++
        if ($watch.Elapsed.TotalSeconds -lt $Seconds) { Start-Sleep -Seconds $IntervalSeconds }
    } while ($watch.Elapsed.TotalSeconds -lt $Seconds)
} finally { $writer.Dispose() }
Write-Output ([ordered]@{schema=1;mode='read-only-SystemPoolTagInformation';output=$OutputPath;samples=$samples;
    seconds=$watch.Elapsed.TotalSeconds;requires_elevation=$false;etl_session_started=$false;driver_loaded=$false} | ConvertTo-Json -Depth 4)
