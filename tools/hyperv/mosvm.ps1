# Minimal-OS test VM in Hyper-V.
#
#   mosvm.ps1 create            make the VM (Generation 1, boots dist\x86_64\kernel.iso)
#   mosvm.ps1 run [-Seconds N] [-Commands 'gpu','hyperv']
#                               boot it, type the commands into the terminal over
#                               COM1, save everything it prints to build\hyperv\serial.log,
#                               then turn it off
#   mosvm.ps1 stop | remove | status
#
# Needs Hyper-V and membership in "Hyper-V Administrators" (setup-hyperv.ps1).
# GPU passthrough: gpu-lend.ps1 / gpu-return.ps1.

param(
    [Parameter(Position = 0)][ValidateSet('create', 'run', 'stop', 'remove', 'status')]
    [string]$Action = 'status',
    [int]$Seconds = 60,
    [string[]]$Commands = @('hyperv', 'gpu', 'gpu nvidia'),
    [string]$Name = 'MinimalOS-Test',
    [string]$Iso
)

$ErrorActionPreference = 'Stop'
# 'a','b' through powershell -File arrives as one string "a,b".
$Commands = @($Commands | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
# The installed copy (C:\ProgramData\MinimalOS-HyperV) records the repo.
$repoFile = Join-Path $PSScriptRoot 'repo.txt'
if (Test-Path $repoFile) { $repo = (Get-Content $repoFile -Raw).Trim() }
if (-not $Iso) { $Iso = Join-Path $repo 'dist\x86_64\kernel.iso' }
$pipe = "\\.\pipe\$Name-com1"
$logDir = Join-Path $repo 'build\hyperv'

function Get-TestVM { Get-VM -Name $Name -ErrorAction SilentlyContinue }

switch ($Action) {
    'create' {
        if (Get-TestVM) { Write-Host "VM '$Name' already exists."; break }
        # Gen 1: BIOS boot, emulated PIT/PS2/serial, which Minimal-OS
        # already drives. Static memory and TurnOff are required for DDA.
        New-VM -Name $Name -Generation 1 -MemoryStartupBytes 2GB -NoVHD | Out-Null
        Set-VM -Name $Name -ProcessorCount 2 -StaticMemory -CheckpointType Disabled `
            -AutomaticStopAction TurnOff -AutomaticStartAction Nothing
        # Room for passed-through BARs (an RTX 2060: 16 MiB + 256 MiB + 32 MiB).
        Set-VM -Name $Name -GuestControlledCacheTypes $true `
            -LowMemoryMappedIoSpace 1GB -HighMemoryMappedIoSpace 32GB
        Get-VMNetworkAdapter -VMName $Name | Remove-VMNetworkAdapter
        if (Test-Path $Iso) { Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $Iso }
        Set-VMBios -VMName $Name -StartupOrder @('CD', 'IDE', 'LegacyNetworkAdapter', 'Floppy')
        Set-VMComPort -VMName $Name -Number 1 -Path $pipe
        Write-Host "Created '$Name' (COM1 -> $pipe)."
    }
    'run' {
        $vm = Get-TestVM
        if (-not $vm) { throw "No VM '$Name'. Run: mosvm.ps1 create" }
        if ($vm.State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
        Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $Iso
        New-Item -ItemType Directory -Force $logDir | Out-Null
        $log = Join-Path $logDir 'serial.log'
        Start-VM -Name $Name
        try {
            & python (Join-Path $PSScriptRoot 'vmserial.py') $pipe $log --seconds $Seconds `
                --commands @Commands
        } finally {
            Stop-VM -Name $Name -TurnOff -Force -ErrorAction SilentlyContinue
        }
        Write-Host "Serial log: $log"
    }
    'stop' { Stop-VM -Name $Name -TurnOff -Force }
    'remove' {
        if (Get-TestVM) { Stop-VM -Name $Name -TurnOff -Force -ErrorAction SilentlyContinue; Remove-VM -Name $Name -Force }
    }
    'status' {
        $vm = Get-TestVM
        if (-not $vm) { Write-Host "No VM '$Name'."; break }
        $vm | Format-List Name, State, Generation, ProcessorCount, MemoryStartup
        Get-VMAssignableDevice -VMName $Name | Format-Table InstanceID, LocationPath
    }
}
