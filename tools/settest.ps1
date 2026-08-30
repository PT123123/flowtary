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
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SendMessageW(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
    [DllImport("user32.dll")] public static extern int SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
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
}
"@

# 找 flowtary 主进程
$proc = Get-Process flowtary -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output "FAIL: flowtary not running"; exit 1 }
$pid2 = [UInt32]$proc.Id

$hwnd = [SW]::FindByClass("FlowtaryLauncher", $pid2)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "FAIL: no launcher window"; exit 1 }

# 1) 直接投递 WM_COMMAND(IDM_SETTINGS=2001) 打开设置（托盘菜单路径需要前台权限，不适合自动化）
[SW]::PostMessageW($hwnd, 0x0111, [IntPtr]2001, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1000

# 3) 找设置窗口，截图采样亮度
$sw = [SW]::FindByClass("FlowtarySettings", $pid2)
if ($sw -eq [IntPtr]::Zero) { Write-Output "FAIL: settings window not found"; exit 2 }
# 置顶后再截图，避免被其他窗口遮挡导致亮度采样失真
[SW]::SetWindowPos($sw, [IntPtr](-1), 0, 0, 0, 0, 0x43) | Out-Null   # HWND_TOPMOST, SWP_NOMOVE|NOSIZE|SHOWWINDOW
Start-Sleep -Milliseconds 500
$r = New-Object RECT
[SW]::GetWindowRect($sw, [ref]$r) | Out-Null
Write-Output ("settings rect: " + $r.Left + "," + $r.Top + " " + ($r.Right-$r.Left) + "x" + ($r.Bottom-$r.Top))

$w = [Math]::Min(700, $r.Right - $r.Left); $h = [Math]::Min(700, $r.Bottom - $r.Top)
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("C:\Users\ted\Desktop\flowtary\build\settings.png")
$sum = 0.0; $n = 0
for ($y = 0; $y -lt $h; $y += 4) {
    for ($x = 0; $x -lt $w; $x += 4) {
        $c = $bmp.GetPixel($x, $y)
        $sum += ($c.R + $c.G + $c.B) / 3.0; $n++
    }
}
Write-Output ("settings mean brightness = " + [math]::Round($sum / $n, 1) + "  (<100 => dark)")

# 4) 读取规则编辑器内容（前 2 行）：枚举子窗口找 EDIT
Add-Type -TypeDefinition @"
using System; using System.Text;
using System.Runtime.InteropServices;
public class CW {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetWindowTextLengthW(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    public static IntPtr EditH = IntPtr.Zero;
    public static bool Cb(IntPtr h, IntPtr lp) {
        StringBuilder sb = new StringBuilder(64); GetClassNameW(h, sb, 64);
        if (sb.ToString() == "Edit") { EditH = h; return false; }
        return true;
    }
}
"@
[CW]::EnumChildWindows($sw, [CW+EnumProc]{ param($a, $b) [CW]::Cb($a, $b) }, [IntPtr]::Zero) | Out-Null
if ([CW]::EditH -ne [IntPtr]::Zero) {
    # 注意：GetWindowTextLengthW 跨进程只返回标题长度（EDIT 为 0），必须用固定大缓冲区
    $sb = New-Object System.Text.StringBuilder 65536
    [SW]::SendMessageW([CW]::EditH, 0x000D, [IntPtr]65536, $sb) | Out-Null   # WM_GETTEXT
    $lines = $sb.ToString() -split "`r?`n" | Where-Object { $_.Trim() -ne "" }
    Write-Output ("rules lines = " + $lines.Count)
    if ($lines.Count -gt 0) { Write-Output ("first = " + $lines[0]) }
    if ($lines.Count -gt 1) { Write-Output ("second = " + $lines[1]) }
} else {
    Write-Output "FAIL: rules edit not found"
}

# 5) 关闭设置窗口
[SW]::PostMessageW($sw, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
Start-Sleep -Milliseconds 300
[SW]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [SW]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
Write-Output "DONE"