Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DG {
    public delegate bool ECB(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(ECB cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Combo(byte[] vks) {
        foreach (byte b in vks) keybd_event(b, 0, 0, UIntPtr.Zero);
        for (int i = vks.Length - 1; i >= 0; i--) keybd_event(vks[i], 0, 2, UIntPtr.Zero);
    }
}
'@

$p = Get-Process flowtary -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "no flowtary process"; exit 1 }
Write-Output ("pid=" + $p.Id + " title=[" + $p.MainWindowTitle + "] ws=" + [math]::Round($p.WorkingSet64/1MB,2) + " priv=" + [math]::Round($p.PrivateMemorySize64/1MB,2))

$rows = @()
$cb = [DG+ECB] { param($h, $l)
    $wp = 0; [void][DG]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $p.Id) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][DG]::GetClassName($h, $sb, 256)
        $script:rows += ("hwnd=" + $h + " class=[" + $sb.ToString() + "] vis=" + [DG]::IsWindowVisible($h))
    }
    return $true
}
[void][DG]::EnumWindows($cb, [IntPtr]::Zero)
$rows | ForEach-Object { Write-Output $_ }

$target = [IntPtr][long]$rows[0].Split(' ')[0].Split('=')[1]
function TryCombo($name, $vks) {
    [DG]::Combo([byte[]]$vks)
    Start-Sleep -Milliseconds 700
    $fg = [DG]::GetForegroundWindow()
    $sb = New-Object System.Text.StringBuilder 256
    [void][DG]::GetClassName($fg, $sb, 256)
    Write-Output ($name + " -> vis=" + [DG]::IsWindowVisible($target) + " fgclass=[" + $sb.ToString() + "]")
}
TryCombo "alt+space" ([byte[]]@(0x12,0x20))
TryCombo "alt+q"     ([byte[]]@(0x12,0x51))
TryCombo "alt+q"     ([byte[]]@(0x12,0x51))
TryCombo "esc"       ([byte[]]@(0x1B))

# 绕过真实键盘输入（前台是全屏游戏时注入会被吞）：
# 直接投递 WM_HOTKEY(wParam=1) 验证 显示/隐藏 逻辑链路
[void][DG]::PostMessage($target, 0x0312, [IntPtr]1, [IntPtr]0)
Start-Sleep -Milliseconds 600
Write-Output ("wm_hotkey-show -> vis=" + [DG]::IsWindowVisible($target))
[void][DG]::PostMessage($target, 0x0312, [IntPtr]1, [IntPtr]0)
Start-Sleep -Milliseconds 600
Write-Output ("wm_hotkey-toggle -> vis=" + [DG]::IsWindowVisible($target))

$p2 = Get-Process -Id $p.Id
Write-Output ("final ws=" + [math]::Round($p2.WorkingSet64/1MB,2) + " priv=" + [math]::Round($p2.PrivateMemorySize64/1MB,2))
