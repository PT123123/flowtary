param(
    [int]$Port = 47782,
    [string]$Mutex = 'Local\Flowtary.SearchDaemonAB',
    [string]$CacheDir = (Join-Path $env:LOCALAPPDATA 'Flowtary-ab'),
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\flowtary-search.exe')
)
$env:FT_DF_DAEMON_PORT = "$Port"
$env:FT_DF_DAEMON_MUTEX = $Mutex
$env:FT_DF_CACHE_DIR = $CacheDir
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot '..\logs') | Out-Null
$workDir = Split-Path -Parent $Exe
Start-Process -FilePath $Exe -ArgumentList '--serve' -WorkingDirectory $workDir `
    -WindowStyle Hidden -RedirectStandardError (Join-Path $PSScriptRoot '..\logs\ab_daemon_err.txt')
Start-Sleep -Seconds 2
Get-Process flowtary-search -ErrorAction SilentlyContinue |
    Select-Object Id, ProcessName, Path | Format-Table -AutoSize
