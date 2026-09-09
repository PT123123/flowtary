# deploy_workshop.ps1 - bump patch digit, build, deploy to C:\workshop\<Product>-<ver>
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\deploy_workshop.ps1 [-Root <project root>]
#
# Behavior:
#   * Reads src\version.h, increments the 3rd segment (FT_VER_PATCH) by 1,
#     and keeps FT_VER_COMMA / FT_VER_DOT in sync (minor is left untouched),
#   * Writes back as UTF-8 without BOM (rc.exe / findstr compatible),
#   * Runs `make build`,
#   * Copies build artifacts into C:\workshop\<FT_PRODUCT_NAME>-<new version>\.
#
# NOTE: keep this file ASCII-only on purpose. On a Chinese-locale system,
# Windows PowerShell 5.1 reads BOM-less UTF-8 as the ANSI codepage (GBK) and
# any non-ASCII byte can be misparsed, breaking the script parse.
param(
    [string]$Root
)

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }   # .. from tools/ = project root

$verH = Join-Path $Root 'src\version.h'
if (-not (Test-Path $verH)) { Write-Error "version.h not found: $verH"; exit 1 }

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$c = [IO.File]::ReadAllText($verH, [Text.Encoding]::UTF8)

$prod = 'project'
if ($c -match '(?m)^\s*#define\s+FT_PRODUCT_NAME\s+"([^"]+)"') { $prod = $Matches[1] }

if ($c -notmatch '(?m)^\s*#define\s+FT_VER_PATCH\s+(\d+)') {
    Write-Error 'FT_VER_PATCH not found in version.h'; exit 1
}
$p = [int]$Matches[1] + 1

$c = [regex]::Replace($c, '(?m)(#define\s+FT_VER_PATCH\s+)\d+', ('${1}' + $p))
$c = [regex]::Replace($c, '(?m)(#define\s+FT_VER_COMMA\s+\d+,\d+,)\d+', ('${1}' + $p))
$c = [regex]::Replace($c, '(?m)(#define\s+FT_VER_DOT\s+"\d+\.\d+\.)\d+', ('${1}' + $p))
[IO.File]::WriteAllText($verH, $c, $utf8NoBom)
Write-Output ("[deploy] bumped patch digit to {0}" -f $p)

# ---- build ----
Push-Location $Root
try {
    cmd /c "make build"
    if ($LASTEXITCODE -ne 0) { Write-Error 'build failed'; exit 1 }
} finally { Pop-Location }

# ---- read final version, copy into C:\workshop\<Product>-<ver> ----
$c2 = [IO.File]::ReadAllText($verH, [Text.Encoding]::UTF8)
$ver = '0.0.0.0'
if ($c2 -match '(?m)#define\s+FT_VER_DOT\s+"([^"]+)"') { $ver = $Matches[1] }

$dest = Join-Path 'C:\workshop' "$prod-$ver"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# Copy every runtime dependency so the deployed folder actually runs.
$src = Join-Path $Root 'build'
Copy-Item (Join-Path $src 'flowtary.exe')        (Join-Path $dest "flowtary-$ver.exe") -Force
Copy-Item (Join-Path $src 'cpp-pinyin.dll')      (Join-Path $dest 'cpp-pinyin.dll') -Force
Copy-Item (Join-Path $src 'filedlg_hook64.dll')  (Join-Path $dest 'filedlg_hook64.dll') -Force
Copy-Item (Join-Path $src 'filedlg_hook32.dll')  (Join-Path $dest 'filedlg_hook32.dll') -Force
Copy-Item (Join-Path $src 'filedlg_agent32.exe') (Join-Path $dest 'filedlg_agent32.exe') -Force
Copy-Item (Join-Path $src 'ScreenCapture.exe')   (Join-Path $dest 'ScreenCapture.exe') -Force
Copy-Item (Join-Path $src 'ImageReader.exe')     (Join-Path $dest 'ImageReader.exe') -Force
if (Test-Path (Join-Path $src 'dict')) {
    Copy-Item (Join-Path $src 'dict') (Join-Path $dest 'dict') -Recurse -Force
}

Write-Host ("[ok] deployed to {0}" -f $dest)
exit 0