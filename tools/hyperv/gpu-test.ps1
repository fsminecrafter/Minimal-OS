# Lends an NVIDIA device to the Minimal-OS test VM, boots the VM, runs the
# terminal commands from build\hyperv\commands.txt, and gives the device
# back to Windows. Runs elevated (Hyper-V device assignment needs it), from
# the copy install-gpu-tasks.ps1 placed in C:\ProgramData\MinimalOS-HyperV.
#
#   -Target usb : the RTX card's USB-C controller (10DE:1ADA). The screen
#                 stays on; good for testing the passthrough path itself.
#   -Target gpu : the RTX 2060 itself (10DE:1F08). Windows has no display
#                 while the VM runs; it comes back afterwards.
#   -ReturnOnly : just give everything back (recovery).
#
# A safety task returns the device after 10 minutes even if this script
# dies half way.

param(
    [ValidateSet('usb', 'gpu')][string]$Target = 'usb',
    [string]$VMName = 'MinimalOS-Test',
    [int]$Seconds = 90,
    [switch]$ReturnOnly
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Get-Content (Join-Path $here 'repo.txt') -Raw).Trim()
$outDir = Join-Path $repo 'build\hyperv'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stateFile = Join-Path $here 'lent.txt'      # location paths currently lent

$ids = @{ usb = 'PCI\VEN_10DE&DEV_1ADA*'; gpu = 'PCI\VEN_10DE&DEV_1F08*' }

function Return-Devices {
    if (-not (Test-Path $stateFile)) { return }
    $vm = Get-VM -Name $VMName -ErrorAction SilentlyContinue
    if ($vm -and $vm.State -ne 'Off') { Stop-VM -Name $VMName -TurnOff -Force }
    foreach ($line in Get-Content $stateFile) {
        $loc, $inst = $line -split '\|', 2
        if (-not $loc) { continue }
        Write-Host "Returning $loc"
        try { Remove-VMAssignableDevice -VMName $VMName -LocationPath $loc -ErrorAction Stop } catch { Write-Host "  remove: $_" }
        try { Mount-VMHostAssignableDevice -LocationPath $loc -ErrorAction Stop } catch { Write-Host "  mount: $_" }
        Start-Sleep -Seconds 2
        try { Enable-PnpDevice -InstanceId $inst -Confirm:$false -ErrorAction Stop } catch { Write-Host "  enable: $_" }
    }
    Remove-Item $stateFile -Force
    Unregister-ScheduledTask -TaskName 'MinimalOS-GPU-Safety' -Confirm:$false -ErrorAction SilentlyContinue
}

Start-Transcript -Path (Join-Path $outDir 'gpu-test.log') -Force | Out-Null
try {
    if ($ReturnOnly) { Return-Devices; return }
    Return-Devices                                  # leftovers from a broken run

    $dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like $ids[$Target] } | Select-Object -First 1
    if (-not $dev) { throw "No device matching $($ids[$Target])" }
    $loc = ($dev | Get-PnpDeviceProperty -KeyName DEVPKEY_Device_LocationPaths).Data |
        Where-Object { $_ -like 'PCIROOT*' } | Select-Object -First 1
    Write-Host "Lending $($dev.FriendlyName) [$($dev.InstanceId)] at $loc"

    # Safety net first: give it back in 10 minutes no matter what.
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$here\gpu-test.ps1`" -ReturnOnly -VMName $VMName"
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(10)
    Register-ScheduledTask -TaskName 'MinimalOS-GPU-Safety' -Action $action -Trigger $trigger `
        -User 'SYSTEM' -RunLevel Highest -Force | Out-Null

    $vm = Get-VM -Name $VMName
    if ($vm.State -ne 'Off') { Stop-VM -Name $VMName -TurnOff -Force }

    "$loc|$($dev.InstanceId)" | Set-Content $stateFile
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Start-Sleep -Seconds 2
    Dismount-VMHostAssignableDevice -LocationPath $loc -Force
    Add-VMAssignableDevice -LocationPath $loc -VMName $VMName
    Write-Host 'Device assigned to the VM.'

    $commands = @('hyperv', 'gpu', 'gpu nvidia')
    $cmdFile = Join-Path $outDir 'commands.txt'
    if (Test-Path $cmdFile) { $commands = @(Get-Content $cmdFile | Where-Object { $_.Trim() }) }
    & (Join-Path $here 'mosvm.ps1') run -Name $VMName -Seconds $Seconds -Commands $commands `
        -Iso (Join-Path $repo 'dist\x86_64\kernel.iso')
} catch {
    Write-Host "ERROR: $_"
} finally {
    Return-Devices
    Stop-Transcript | Out-Null
}
