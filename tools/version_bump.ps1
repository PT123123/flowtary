# version_bump.ps1 - Auto-increment the BUILD (last) digit of src\version.h
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File version_bump.ps1 <path-to-version.h>
#
# Behavior:
#   * Reads version.h as UTF-8, bumps the last digit (FT_VER_BUILD),
#   * syncs FT_VER_COMMA and FT_VER_DOT to match,
#   * writes back as UTF-8 WITHOUT BOM (keeps rc.exe / findstr happy),
#   * leaves all other content (including Chinese comments) untouched.
param(
    [Parameter(Mandatory = $true)]
    [string]$VersionHeader
)

$VersionHeader = [IO.Path]::GetFullPath($VersionHeader)
if (-not (Test-Path $VersionHeader)) {
    Write-Error "version.h not found: $VersionHeader"
    exit 1
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$c = [IO.File]::ReadAllText($VersionHeader, [Text.Encoding]::UTF8)

if ($c -notmatch '(?m)^\s*#define\s+FT_VER_BUILD\s+(\d+)') {
    Write-Error 'FT_VER_BUILD not found in version.h'
    exit 1
}
$b = [int]$Matches[1] + 1

$c = [regex]::Replace($c, '(?m)(#define\s+FT_VER_BUILD\s+)\d+', ('${1}' + $b))
$c = [regex]::Replace($c, '(?m)(#define\s+FT_VER_COMMA\s+[\d,]+,)\d+', ('${1}' + $b))
$c = [regex]::Replace($c, '(?m)(#define\s+FT_VER_DOT\s+"[\d.]+\.)\d+"', ('${1}' + $b + '"'))

[IO.File]::WriteAllText($VersionHeader, $c, $utf8NoBom)
Write-Output ("bumped build digit to {0}" -f $b)
exit 0
