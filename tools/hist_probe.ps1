# Probe the search-history ledger in the real app: type queries into the launcher
# window with posted messages, then read back the lines the run appended to
# %APPDATA%\Flowtary\search_history.log. No screenshots, no synthetic clicks.
# Usage: powershell -NoProfile -File tools\hist_probe.ps1 [-Exe <path>]
param([string]$Exe = "")

$ErrorActionPreference = "Stop"
if (-not $Exe) { $Exe = Join-Path (Split-Path $PSScriptRoot -Parent) "build\flowtary.exe" }
if (-not (Test-Path $Exe)) { Write-Output "FAIL: exe not found: $Exe"; exit 1 }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class FT {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@

$WM_CHAR = 0x0102
$WM_KEYDOWN = 0x0100
$WM_KEYUP = 0x0101
$VK_BACK = 0x08
$VK_RETURN = 0x0D

$ledger = Join-Path $env:APPDATA "Flowtary\search_history.log"
function LedgerCount { if (Test-Path $ledger) { (Get-Content -LiteralPath $ledger -Encoding UTF8 | Measure-Object -Line).Lines } else { 0 } }

$token = "ftprobe" + (Get-Random -Maximum 99999)
$probeFile = Join-Path ([Environment]::GetFolderPath("Desktop")) ($token + "-note.txt")
Set-Content -LiteralPath $probeFile -Value "flowtary history probe" -Encoding ASCII

$before = LedgerCount
$p = Start-Process -FilePath $Exe -PassThru
Write-Output ("started pid=" + $p.Id + " token=" + $token)
Start-Sleep -Seconds 12

$h = [FT]::FindWindowW("FlowtaryLauncher", [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { Write-Output "FAIL: launcher window not found"; Stop-Process -Id $p.Id; Remove-Item -LiteralPath $probeFile -Force; exit 1 }

function TypeText([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        [FT]::PostMessageW($h, $WM_CHAR, [IntPtr][int]$ch, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 200
}
function ClearText {
    for ($i = 0; $i -lt 60; $i++) {
        [FT]::PostMessageW($h, $WM_KEYDOWN, [IntPtr]$VK_BACK, [IntPtr]::Zero) | Out-Null
        [FT]::PostMessageW($h, $WM_KEYUP, [IntPtr]$VK_BACK, [IntPtr]::Zero) | Out-Null
    }
    Start-Sleep -Milliseconds 300
}
function PressEnter {
    [FT]::PostMessageW($h, $WM_KEYDOWN, [IntPtr]$VK_RETURN, [IntPtr]::Zero) | Out-Null
    [FT]::PostMessageW($h, $WM_KEYUP, [IntPtr]$VK_RETURN, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400
}

# probe 0: program search answers every keystroke -> ledger must keep ONE settled line, not one per char
TypeText "notepad"
Start-Sleep -Seconds 6
ClearText

# probe 1: a file that exists -> expect rows>0, first= set, then an O line from Enter
TypeText ("f " + $token)
Start-Sleep -Seconds 6
$notepadBefore = (Get-Process -Name notepad -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
PressEnter
Start-Sleep -Seconds 3
$notepadAfter = (Get-Process -Name notepad -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$opened = @($notepadAfter | Where-Object { $notepadBefore -notcontains $_ })
Write-Output ("opened-notepad-pids=" + ($opened -join ","))
foreach ($pid2 in $opened) { Stop-Process -Id $pid2 -ErrorAction SilentlyContinue }

# probe 2: a token that cannot match -> expect the rows=0 "scene" line
ClearText
TypeText ("d " + $token + "-nomatch-qq")
Start-Sleep -Seconds 17

Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Remove-Item -LiteralPath $probeFile -Force -ErrorAction SilentlyContinue

Write-Output ("ledger lines before=" + $before + " after=" + (LedgerCount))
if (Test-Path $ledger) {
    Write-Output "--- new lines ---"
    Get-Content -LiteralPath $ledger -Encoding UTF8 | Select-Object -Skip $before
}
