Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class U32 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Combo(byte[] vks) {
        foreach (byte b in vks) keybd_event(b, 0, 0, UIntPtr.Zero);
        System.Array.Reverse(vks);
        foreach (byte b in vks) keybd_event(b, 0, 2, UIntPtr.Zero);
        System.Array.Reverse(vks);
    }
    public static void AltSpace()  { Combo(new byte[] { 0x12, 0x20 }); }
    public static void AltQ()      { Combo(new byte[] { 0x12, 0x51 }); }
    public static void CtrlAltSp() { Combo(new byte[] { 0x11, 0x12, 0x20 }); }
    public static void Esc()       { Combo(new byte[] { 0x1B }); }
}
'@

$h = [U32]::FindWindowW("FlowtaryLauncher", [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { Write-Output "RESULT: FAIL window-not-found"; exit 1 }
Write-Output ("before: visible=" + [U32]::IsWindowVisible($h))

Start-Sleep -Milliseconds 300
[U32]::AltSpace();  Start-Sleep -Milliseconds 600
if (-not [U32]::IsWindowVisible($h)) {
    [U32]::AltQ();  Start-Sleep -Milliseconds 600
    if (-not [U32]::IsWindowVisible($h)) {
        [U32]::CtrlAltSp();  Start-Sleep -Milliseconds 600
    }
}
$vis1 = [U32]::IsWindowVisible($h)
Write-Output ("after-show-attempt: visible=" + $vis1)

[U32]::Esc()
Start-Sleep -Milliseconds 600
$vis2 = [U32]::IsWindowVisible($h)
Write-Output ("after-esc: visible=" + $vis2)

$p = Get-Process -Name flowtary -ErrorAction SilentlyContinue
if ($p) {
    Write-Output ("memory: workingset_mb=" + [math]::Round($p.WorkingSet64/1MB, 2) + " private_mb=" + [math]::Round($p.PrivateMemorySize64/1MB, 2))
}

if ($vis1 -and -not $vis2) { Write-Output "RESULT: PASS show-then-hide" } else { Write-Output "RESULT: FAIL vis1=$vis1 vis2=$vis2" }
