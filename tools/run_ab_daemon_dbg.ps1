$root = Split-Path -Parent $PSScriptRoot
$env:FT_DF_DAEMON_PORT = '47782'
$env:FT_DF_DAEMON_MUTEX = 'Local\Flowtary.SearchDaemonAB'
$env:FT_DF_CACHE_DIR = (Join-Path $env:LOCALAPPDATA 'Flowtary-ab')
$env:FT_DF_DEBUG = '1'
$log = Join-Path $root 'build\ab_daemon.log'
Start-Process -FilePath (Join-Path $root 'build\flowtary-search.exe') `
  -ArgumentList '--serve' -NoNewWindow `
  -RedirectStandardError $log -PassThru | Select-Object -ExpandProperty Id
