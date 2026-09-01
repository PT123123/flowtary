# release_copy.ps1 — 把 build\ 的发布产物拷贝到目标目录（默认 dist\）
# 文件名中的版本号取自 src\version.h 的 FT_VER_DOT。
# 由 Makefile 的 release 目标调用（原 release.bat Step 4 的等价实现）。
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File release_copy.ps1 [-Root <项目根>] [-Dist <输出目录>]
param(
    [string]$Root,
    [string]$Dist
)

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }   # tools\.. = 项目根
if (-not $Dist) { $Dist = Join-Path $Root 'dist' }

$src  = Join-Path $Root 'build'
$ver  = '0.0.0.0'

$line = Select-String -Path (Join-Path $Root 'src\version.h') -Pattern '#define FT_VER_DOT' | Select-Object -First 1
if ($line -and $line.Line -match '"([^"]+)"') { $ver = $Matches[1] }

New-Item -ItemType Directory -Force -Path $Dist | Out-Null

Copy-Item (Join-Path $src 'flowtary.exe')       (Join-Path $Dist "flowtary-$ver.exe") -Force
Copy-Item (Join-Path $src 'filedlg_hook64.dll') (Join-Path $Dist 'filedlg_hook64.dll') -Force
Copy-Item (Join-Path $src 'filedlg_hook32.dll') (Join-Path $Dist 'filedlg_hook32.dll') -Force
Copy-Item (Join-Path $src 'filedlg_agent32.exe')(Join-Path $Dist 'filedlg_agent32.exe') -Force

Write-Host ("[ok] {0}\flowtary-{1}.exe" -f $Dist, $ver)
