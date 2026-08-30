# dropdowntest.ps1 — 验证设置窗口「唤醒位置」自绘下拉按钮 + 黑暗弹出菜单
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
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public static List<IntPtr> Found = new List<IntPtr>();
    public static string WantClass = "";
    public static bool Cb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (lp != IntPtr.Zero && pid != (uint)lp) return true;
        StringBuilder sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
        if (sb.ToString() == WantClass) Found.Add(h);
        return true;
    }
    public static IntPtr FindByClass(string cls, uint pid, int idx) {
        Found.Clear(); WantClass = cls; EnumWindows(Cb, (IntPtr)pid);
        return idx < Found.Count ? Found[idx] : IntPtr.Zero;
    }
}
"@

$proc = Get-Process flowtary -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output "FAIL: flowtary not running"; exit 1 }
$pid2 = [UInt32]$proc.Id

# 打开设置窗口
$hwnd = [SW]::FindByClass("FlowtaryLauncher", $pid2, 0)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "FAIL: launcher not found"; exit 1 }
[SW]::PostMessageW($hwnd, 0x0111, [IntPtr]2001, [IntPtr]::Zero) | Out-Null   # WM_COMMAND IDM_SETTINGS
Start-Sleep -Milliseconds 1000
$sw = [SW]::FindByClass("FlowtarySettings", $pid2, 0)
if ($sw -eq [IntPtr]::Zero) { Write-Output "FAIL: settings not found"; exit 2 }
[SW]::SetWindowPos($sw, [IntPtr](-1), 0, 0, 0, 0, 0x43) | Out-Null   # TOPMOST
[SW]::SetForegroundWindow($sw) | Out-Null
Start-Sleep -Milliseconds 500

# 找组合按钮子控件（ID=3003）
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Runtime.InteropServices;
public class EC {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    public static IntPtr Found = IntPtr.Zero;
    public static bool Cb(IntPtr h, IntPtr lp) {
        int id = GetDlgCtrlID(h);
        StringBuilder sb = new StringBuilder(32); GetClassNameW(h, sb, 32);
        if (id == (int)lp && sb.ToString() == "Button") { Found = h; return false; }
        return true;
    }
}
"@
[EC]::Found = [IntPtr]::Zero
[EC]::EnumChildWindows($sw, [EC+EnumProc]{ param($a,$b) [EC]::Cb($a,$b) }, [IntPtr]3003) | Out-Null
if ([EC]::Found -eq [IntPtr]::Zero) { Write-Output "FAIL: wake button not found"; exit 3 }
Write-Output ("wake button hwnd=0x" + [EC]::Found.ToString("x"))

# 模拟点击（BM_CLICK -> BN_CLICKED -> TrackPopupMenu）
[SW]::PostMessageW([EC]::Found, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK
Start-Sleep -Milliseconds 800

# 找弹出菜单窗口（类名 #32768，属于本进程）
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Runtime.InteropServices;
public class MW {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static IntPtr Menu = IntPtr.Zero;
    public static uint Pid = 0;
    public static bool Cb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid != Pid || !IsWindowVisible(h)) return true;
        StringBuilder sb = new StringBuilder(16); GetClassNameW(h, sb, 16);
        if (sb.ToString() == "#32768") { Menu = h; return false; }
        return true;
    }
}
"@
[MW]::Pid = $pid2
[MW]::Menu = [IntPtr]::Zero
[MW]::EnumWindows([MW+EnumProc]{ param($a,$b) [MW]::Cb($a,$b) }, [IntPtr]::Zero) | Out-Null
if ([MW]::Menu -eq [IntPtr]::Zero) { Write-Output "FAIL: dropdown menu window not found"; exit 4 }
Write-Output ("dropdown menu hwnd=0x" + [MW]::Menu.ToString("x"))

# 截图采样菜单亮度
$r = New-Object RECT
[SW]::GetWindowRect([MW]::Menu, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
Write-Output ("menu rect: " + $r.Left + "," + $r.Top + " " + $w + "x" + $h)
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("C:\Users\ted\Desktop\flowtary\build\dropdown_menu.png")
$sum = 0.0; $n = 0
for ($y = 0; $y -lt $h; $y += 2) {
    for ($x = 0; $x -lt $w; $x += 2) {
        $c = $bmp.GetPixel($x, $y)
        $sum += ($c.R + $c.G + $c.B) / 3.0; $n++
    }
}
Write-Output ("dropdown menu mean brightness = " + [math]::Round($sum / $n, 1) + "  (<100 => dark)")

# 按 Esc 关闭菜单，关闭设置窗口
[SW]::PostMessageW([MW]::Menu, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
[SW]::PostMessageW($sw, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Write-Output "DONE"