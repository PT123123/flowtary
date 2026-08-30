# ASCII-only on purpose: Windows PowerShell 5.1 misreads BOM-less UTF-8 scripts.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class SW {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public static List<IntPtr> Found = new List<IntPtr>();
    public static string WantClass = "";
    public static bool Cb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (lp != IntPtr.Zero && pid != (uint)lp) return true;
        StringBuilder sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
        if (sb.ToString() == WantClass) Found.Add(h);
        return true;
    }
    public static IntPtr FindByClass(string cls, uint pid) {
        Found.Clear(); WantClass = cls; EnumWindows(Cb, (IntPtr)pid);
        return Found.Count > 0 ? Found[0] : IntPtr.Zero;
    }
    public static List<IntPtr> All = new List<IntPtr>();
    public static bool AllCb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == (uint)lp) All.Add(h);
        return true;
    }
    public static List<IntPtr> Children = new List<IntPtr>();
    public static bool ChildCb(IntPtr h, IntPtr lp) { Children.Add(h); return true; }
    public static string Text(IntPtr h) {
        StringBuilder sb = new StringBuilder(1024);
        GetWindowTextW(h, sb, 1024);
        return sb.ToString();
    }
    public static string Cls(IntPtr h) {
        StringBuilder sb = new StringBuilder(256);
        GetClassNameW(h, sb, 256);
        return sb.ToString();
    }
}
"@

function DumpWindows($pid2, $tag) {
    [SW]::All.Clear()
    [SW]::EnumWindows([SW+EnumProc]{ param($a, $b) [SW]::AllCb($a, $b) }, [IntPtr][UInt32]$pid2) | Out-Null
    Write-Output ("-- windows of pid " + $pid2 + " (" + $tag + "): count=" + [SW]::All.Count)
    foreach ($w in [SW]::All) {
        Write-Output ("   [" + [SW]::Cls($w) + "] vis=" + [SW]::IsWindowVisible($w) + " title=[" + [SW]::Text($w) + "]")
    }
}

# 0) clean slate: kill any leftover instance
taskkill /F /IM flowtary.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 800

# 1) start fresh instance
Start-Process -FilePath "C:\Users\ted\Desktop\flowtary\build\flowtary.exe"
Start-Sleep -Seconds 6
$proc = Get-Process flowtary -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output "FAIL: flowtary.exe did not start"; exit 1 }
$pid2 = [UInt32]$proc.Id
Write-Output ("flowtary pid = " + $proc.Id + " responding = " + $proc.Responding)

$hwnd = [SW]::FindByClass("FlowtaryLauncher", $pid2)
if ($hwnd -eq [IntPtr]::Zero) { DumpWindows $pid2 "no-launcher"; Write-Output "FAIL: no launcher window"; taskkill /F /IM flowtary.exe 2>$null | Out-Null; exit 1 }
Write-Output ("launcher hwnd = " + $hwnd)

# 2) open settings via posted WM_COMMAND (IDM_SETTINGS=2001)
$ok = [SW]::PostMessageW($hwnd, 0x0111, [IntPtr]2001, [IntPtr]::Zero)
Write-Output ("post WM_COMMAND 2001 ok = " + $ok)
Start-Sleep -Milliseconds 2000

$proc = Get-Process -Id $pid2 -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "FAIL: flowtary process died after WM_COMMAND (crash?)"; exit 2 }

$sw = [SW]::FindByClass("FlowtarySettings", $pid2)
if ($sw -eq [IntPtr]::Zero) {
    DumpWindows $pid2 "settings-missing"
    Write-Output "FAIL: settings window not found"
    taskkill /F /IM flowtary.exe 2>$null | Out-Null
    exit 3
}
Write-Output ("settings hwnd = " + $sw)

# 3) enumerate children; find weight tab button BY ID (3025) to avoid encoding issues
[SW]::Children.Clear()
[SW]::EnumChildWindows($sw, [SW+EnumProc]{ param($a, $b) [SW]::ChildCb($a, $b) }, [IntPtr]::Zero) | Out-Null
Write-Output ("settings child controls = " + [SW]::Children.Count)
$tabBtn = [IntPtr]::Zero
foreach ($c in [SW]::Children) { if ([SW]::GetDlgCtrlID($c) -eq 3025) { $tabBtn = $c; break } }
if ($tabBtn -eq [IntPtr]::Zero) {
    foreach ($c in [SW]::Children) { Write-Output ("   child id=" + [SW]::GetDlgCtrlID($c) + " cls=" + [SW]::Cls($c)) }
    Write-Output "FAIL: tab button id=3025 not found"
    taskkill /F /IM flowtary.exe 2>$null | Out-Null
    exit 4
}
Write-Output ("tab button id=3025 found, class=" + [SW]::Cls($tabBtn) + " text-len=" + [SW]::Text($tabBtn).Length)

# 4) switch to weight tab (WM_COMMAND/BN_CLICKED, IDC_TAB_WEIGHT=3025)
[SW]::PostMessageW($sw, 0x0111, [IntPtr]3025, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 800

# 5) verify weight-tab controls exist and are visible
[SW]::Children.Clear()
[SW]::EnumChildWindows($sw, [SW+EnumProc]{ param($a, $b) [SW]::ChildCb($a, $b) }, [IntPtr]::Zero) | Out-Null
$byId = @{}
foreach ($c in [SW]::Children) { $byId[[SW]::GetDlgCtrlID($c)] = $c }
$checks = @(
    @(3026, "chk-enable"),
    @(3027, "lbl-flush"),
    @(3028, "cmb-flush"),
    @(3029, "lbl-maxent"),
    @(3030, "cmb-maxent"),
    @(3031, "btn-wipe"),
    @(3032, "lbl-count"),
    @(3033, "lbl-hint")
)
$allOk = $true
foreach ($pair in $checks) {
    $k = $pair[0]; $name = $pair[1]
    $c = $byId[$k]
    if (-not $c) { Write-Output ("MISSING: " + $name + " id=" + $k); $allOk = $false; continue }
    $vis = [SW]::IsWindowVisible($c)
    $t = [SW]::Text($c)
    Write-Output ($name + " id=" + $k + " visible=" + $vis + " text-len=" + $t.Length + " text-hex-first8=" + ([System.BitConverter]::ToString([System.Text.Encoding]::Unicode.GetBytes($t.Substring(0, [Math]::Min(8, $t.Length))))))
    if (-not $vis) { $allOk = $false }
}
$rulesEd = $byId[3007]
if ($rulesEd) {
    $rv = [SW]::IsWindowVisible($rulesEd)
    Write-Output ("contrast: web-rules editor visible=" + $rv + " (expect False)")
    if ($rv) { $allOk = $false }
}

# 6) screenshot of the weight tab
[SW]::SetWindowPos($sw, [IntPtr](-1), 0, 0, 0, 0, 0x43) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object RECT
[SW]::GetWindowRect($sw, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("C:\Users\ted\Desktop\flowtary\build\weighttab.png")
Write-Output "screenshot saved"

# 7) close settings and process
[SW]::PostMessageW($sw, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
taskkill /F /IM flowtary.exe 2>$null | Out-Null

if (-not $allOk) { Write-Output "FAIL: some weight controls missing or hidden"; exit 5 }
Write-Output "DONE"
