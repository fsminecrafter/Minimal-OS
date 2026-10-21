# One-time setup for testing Minimal-OS in Hyper-V (run as Administrator).
#
#  - installs Hyper-V and its PowerShell module,
#  - adds the current user to "Hyper-V Administrators" so VMs can be
#    created and started from a normal (non-elevated) shell.
#
# A reboot is needed afterwards. Run from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\hyperv\setup-hyperv.ps1

$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated (Administrator) PowerShell.'
}

# The user who should manage the VMs: the one who started the elevated
# shell (same account when elevating through UAC).
$user = "$env:USERDOMAIN\$env:USERNAME"

Write-Host 'Enabling Hyper-V (platform + management tools)...'
$result = Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -All -NoRestart

# "Hyper-V Administrators" by well-known SID: the group name is localized.
$group = (New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-578').Translate([Security.Principal.NTAccount]).Value
$groupName = $group.Split('\')[-1]
try {
    Add-LocalGroupMember -Group $groupName -Member $user -ErrorAction Stop
    Write-Host "Added $user to '$groupName'."
} catch {
    if ($_.Exception.Message -match 'already a member') { Write-Host "$user is already in '$groupName'." }
    else { throw }
}

Write-Host ''
if ($result.RestartNeeded) {
    Write-Host 'Done. Reboot Windows to finish installing Hyper-V.' -ForegroundColor Yellow
} else {
    Write-Host 'Done. Sign out and back in so the group membership applies.' -ForegroundColor Yellow
}
Write-Host 'GPU passthrough also needs the IOMMU: in the ASRock BIOS (F2) check'
Write-Host '  Advanced > AMD CBS > NBIO Common Options > IOMMU = Enabled (not Auto).'
