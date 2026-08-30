Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class TW {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
}
"@

# WM_APP_TRAY = 0x0402, WM_RBUTTONUP = 0x0205
$hwnd = [TW]::FindWindowW("FlowtaryLauncher", $null)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "FAIL: no flowtary window"; exit 1 }

[TW]::SetCursorPos(600, 400) | Out-Null
Start-Sleep -Milliseconds 200
[TW]::PostMessageW($hwnd, 0x0402, [IntPtr]::Zero, [IntPtr]0x0205) | Out-Null
Start-Sleep -Milliseconds 900

$bmp = New-Object System.Drawing.Bitmap(280, 200)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(600, 400, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save("C:\Users\ted\Desktop\flowtary\build\traymenu.png")

# 采样平均亮度
$sum = 0.0; $n = 0
for ($y = 0; $y -lt 200; $y += 4) {
    for ($x = 0; $x -lt 280; $x += 4) {
        $c = $bmp.GetPixel($x, $y)
        $sum += ($c.R + $c.G + $c.B) / 3.0
        $n++
    }
}
$mean = [math]::Round($sum / $n, 1)
Write-Output ("tray-menu mean brightness = " + $mean + "  (<120 => dark)")
$bmp.Dispose()

# 收尾：点远处关闭菜单
[TW]::SetCursorPos(20, 950) | Out-Null
[TW]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
[TW]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 300
[TW]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero)
[TW]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)