# altkey.ps1 �?验证 Alt+字母快捷键徽章与触发、主题渲�?
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class AK {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
    public static List<IntPtr> Found = new List<IntPtr>();
    public static string Want = "";
    public static uint Pid = 0;
    public static bool Cb(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (Pid != 0 && pid != Pid) return true;
        StringBuilder sb = new StringBuilder(128); GetClassNameW(h, sb, 128);
        if (sb.ToString() == Want) Found.Add(h);
        return true;
    }
    public static IntPtr Find(string cls, uint pid) {
        Found.Clear(); Want = cls; Pid = pid; EnumWindows(Cb, IntPtr.Zero);
        return Found.Count > 0 ? Found[0] : IntPtr.Zero;
    }
    public static void Key(byte vk, bool down) {
        keybd_event(vk, 0, down ? 0u : 2u, UIntPtr.Zero);
        System.Threading.Thread.Sleep(24);
    }
    public static void Combo(byte a, byte b) {
        Key(a, true); Key(b, true); Key(b, false); Key(a, false);
    }
    public static void Type(string s) {
        foreach (char ch in s) {
            byte vk = ch == ' ' ? (byte)0x20 : (byte)char.ToUpperInvariant(ch);
            Key(vk, true); Key(vk, false);
        }
    }
}
"@

$proc = Get-Process flowtary -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output "FAIL: flowtary not running"; exit 1 }
$pid2 = [uint32]$proc.Id
$launcher = [AK]::Find("FlowtaryLauncher", $pid2)
if ($launcher -eq [IntPtr]::Zero) { Write-Output "FAIL: launcher not found"; exit 2 }

# 1) 真实键盘 Alt+Q 唤出（本机热�?Alt+Space 被占用→回退 Alt+Q�?
[AK]::SetForegroundWindow($launcher) | Out-Null
[System.Threading.Thread]::Sleep(200)
[AK]::Combo(0x12, 0x51)   # Alt+Q
[System.Threading.Thread]::Sleep(600)
Write-Output ("after hotkey: visible=" + [AK]::IsWindowVisible($launcher))

# 2) 输入 "d hosts"
[AK]::Type("d hosts")
[System.Threading.Thread]::Sleep(1200)          # �?Everything 回复
Write-Output ("after typing: visible=" + [AK]::IsWindowVisible($launcher))

# 3) 截图主窗看看徽章
$r = New-Object RECT
[AK]::GetWindowRect($launcher, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
Write-Output ("launcher rect: " + $r.Left + "," + $r.Top + " " + $w + "x" + $h)
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("C:\Users\ted\Desktop\flowtary\build\launcher_badges.png")

# 徽章检测：每行右侧 32px 条带内找亮色�?150）像�?
$rowH = 30 * $bmp.Height / (56 + 10*30)   # 近似行高
$rowsWithBadge = 0
for ($y = 0; $y -lt $bmp.Height; $y++) {
    $has = $false
    for ($x = $bmp.Width - 36; $x -lt $bmp.Width - 4; $x++) {
        $c = $bmp.GetPixel($x, $y)
        if (($c.R + $c.G + $c.B) -gt 450) { $has = $true; break }
    }
    if ($has) { $rowsWithBadge++ }
}
Write-Output ("bright-pixel-rows-right-edge = " + $rowsWithBadge + "  (>=4 => badges drawn)")

# 4) 模拟 Alt+A 触发第一�?
[AK]::SetForegroundWindow($launcher) | Out-Null
[System.Threading.Thread]::Sleep(120)
[AK]::Combo(0x12, 0x41)   # Alt+A
[System.Threading.Thread]::Sleep(800)
Write-Output ("after alt+a: visible=" + [AK]::IsWindowVisible($launcher) + "  (False = executed)")

# 清理：Esc 兜底
if ([AK]::IsWindowVisible($launcher)) {
    [AK]::Key(0x1B, $true); [AK]::Key(0x1B, $false)
}
# 重启主程序清�?everything 弹窗副作用并保持干净
Write-Output "DONE"