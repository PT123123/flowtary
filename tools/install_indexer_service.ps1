param(
    [string]$Root,
    [switch]$ElevatedChild
)
$ErrorActionPreference = 'Stop'

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = [IO.Path]::GetFullPath($Root)
$binarySource = Join-Path $Root 'indexer\target\release\flowtary-indexer.exe'
$serviceRoot = Join-Path $env:ProgramFiles 'Flowtary'
$serviceBinary = Join-Path $serviceRoot 'flowtary-indexer.exe'
$dataRoot = Join-Path $env:ProgramData 'Flowtary'
$snapshotRoot = Join-Path $dataRoot 'Indexes'
$snapshot = Join-Path $snapshotRoot 'index-C.fti'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path -LiteralPath $binarySource -PathType Leaf)) {
    throw "Release indexer binary is missing: $binarySource"
}

if (-not $isAdmin -and -not $ElevatedChild) {
    $hostPath = (Get-Process -Id $PID).Path
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $hostPath
    $startInfo.UseShellExecute = $true
    $startInfo.Verb = 'runas'
    foreach ($arg in @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Root',$Root,'-ElevatedChild')) {
        $startInfo.ArgumentList.Add($arg)
    }
    try {
        $child = [Diagnostics.Process]::Start($startInfo)
        $child.WaitForExit()
        exit $child.ExitCode
    } catch {
        if ($_.Exception.HResult -eq -2147023673) {
            Write-Error 'Windows reported that service installation authorization was cancelled.'
        }
        throw
    }
}
if (-not $isAdmin) { throw 'The elevated installer did not receive an administrator token.' }

# Reinstallation/upgrade stops and removes the service before replacing its image.
$existing = Get-Service -Name 'FlowtaryIndexer' -ErrorAction SilentlyContinue
if ($existing) {
    & $serviceBinary service-uninstall
    if ($LASTEXITCODE -ne 0) { throw "Could not stop/remove the existing service (exit $LASTEXITCODE)." }
}

New-Item -ItemType Directory -Force -Path $serviceRoot,$snapshotRoot,(Join-Path $dataRoot 'Logs') | Out-Null

# A LocalSystem image must not live in the user-writable C:\workshop deployment tree.
$systemSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
$adminsSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
$serviceAcl = [Security.AccessControl.DirectorySecurity]::new()
$serviceAcl.SetAccessRuleProtection($true,$false)
foreach ($entry in @(
    @($systemSid,[Security.AccessControl.FileSystemRights]::FullControl),
    @($adminsSid,[Security.AccessControl.FileSystemRights]::FullControl),
    @([Security.Principal.SecurityIdentifier]::new('S-1-5-11'),[Security.AccessControl.FileSystemRights]::ReadAndExecute)
)) {
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $entry[0],$entry[1],
        [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit,
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Allow)
    $serviceAcl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $serviceRoot -AclObject $serviceAcl
Copy-Item -LiteralPath $binarySource -Destination $serviceBinary -Force

# Keep service-owned snapshots and logs private to SYSTEM and administrators.
$dataAcl = [Security.AccessControl.DirectorySecurity]::new()
$dataAcl.SetAccessRuleProtection($true,$false)
foreach ($sid in @($systemSid,$adminsSid)) {
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $sid,[Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit,
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Allow)
    $dataAcl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $dataRoot -AclObject $dataAcl

$ownerSid = $identity.User.Value
& $serviceBinary service-install --snapshot $snapshot --owner-sid $ownerSid
if ($LASTEXITCODE -ne 0) { throw "Service installation failed (exit $LASTEXITCODE)." }
Write-Host "Flowtary Index Service installed. The one-time UAC authorization covers service registration; this build serves existing snapshots and does not scan volumes."
