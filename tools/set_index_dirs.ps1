param([string[]]$Dirs = @('C:\workshop','C:\software'))
$v = ($Dirs -join "`r`n")
Set-ItemProperty -Path HKCU:\Software\Flowtary -Name IndexDirs -Value $v -Type String
$cur = (Get-ItemProperty HKCU:\Software\Flowtary -Name IndexDirs).IndexDirs
Write-Output ("IndexDirs=" + ($cur -replace "`r`n", ' | '))
