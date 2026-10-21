# Installs the GPU passthrough test so it can be started without a UAC
# prompt (run once as Administrator, and again after gpu-test.ps1,
# mosvm.ps1 or vmserial.py change).
#
# The scripts are copied to C:\ProgramData\MinimalOS-HyperV, which only
# administrators can change, and two scheduled tasks run them elevated:
#
#   schtasks /run /tn MinimalOS-GPU-Test-USB   lend the RTX's USB-C controller
#   schtasks /run /tn MinimalOS-GPU-Test-GPU   lend the RTX 2060 (screen goes dark meanwhile)
#
# Output: build\hyperv\serial.log and build\hyperv\gpu-test.log.

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated (Administrator) PowerShell.'
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dest = 'C:\ProgramData\MinimalOS-HyperV'
New-Item -ItemType Directory -Force $dest | Out-Null

# Administrators and SYSTEM: full control; everyone else: read only.
$acl = New-Object Security.AccessControl.DirectorySecurity
$acl.SetAccessRuleProtection($true, $false)
foreach ($sid in 'S-1-5-32-544', 'S-1-5-18') {
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
        (New-Object Security.Principal.SecurityIdentifier $sid), 'FullControl',
        'ContainerInherit,ObjectInherit', 'None', 'Allow')))
}
$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule(
    (New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-545'), 'ReadAndExecute',
    'ContainerInherit,ObjectInherit', 'None', 'Allow')))
Set-Acl -Path $dest -AclObject $acl

foreach ($f in 'gpu-test.ps1', 'mosvm.ps1', 'vmserial.py') {
    Copy-Item (Join-Path $PSScriptRoot $f) $dest -Force
}
Set-Content -Path (Join-Path $dest 'repo.txt') -Value $repo

$user = "$env:USERDOMAIN\$env:USERNAME"
foreach ($target in 'USB', 'GPU') {
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$dest\gpu-test.ps1`" -Target $($target.ToLower())"
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 15) -AllowStartIfOnBatteries
    $p = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskName "MinimalOS-GPU-Test-$target" -Action $action -Settings $settings `
        -Principal $p -Force | Out-Null
}
Write-Host "Installed to $dest. Start a test with: schtasks /run /tn MinimalOS-GPU-Test-USB"
