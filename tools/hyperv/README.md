# Testing Minimal-OS under Hyper-V

The kernel's Hyper-V support (`src/modules/HyperV`) finds the hypervisor
through CPUID, enables the hypercall page and the SynIC, and talks VMBus.
The `hyperv` terminal command reports what it found. These scripts boot
`dist/x86_64/kernel.iso` in a test VM, type terminal commands into COM1 and
save everything the guest prints to `build/hyperv/serial.log`.

## Windows: real Hyper-V

| Script | What it does |
| --- | --- |
| `setup-hyperv.ps1` | One-time setup, as Administrator: installs Hyper-V, adds you to "Hyper-V Administrators" |
| `wsl-build.sh` | Builds the ISO inside WSL from a Windows checkout |
| `mosvm.ps1 create` | Makes the `MinimalOS-Test` VM (Generation 1, COM1 on a named pipe) |
| `mosvm.ps1 run` | Boots it, runs the commands (`-Commands hyperv,gpu`), logs COM1, turns it off |
| `install-gpu-tasks.ps1`, `gpu-test.ps1` | Lend the NVIDIA card (or its USB-C controller) to the VM with DDA, then give it back |

## Linux: QEMU/KVM with Hyper-V enlightenments

Linux has no Hyper-V, but KVM can show a guest the same interface: the
Hyper-V CPUID leaves and MSRs, the hypercall page, the SynIC, and a VMBus
(QEMU's `vmbus-bridge`) that offers a channel (`hv-balloon`, dynamic
memory). Minimal-OS sees "running under Hyper-V" and connects to VMBus just
as it does in a Generation 1 VM. Device passthrough uses VFIO instead of DDA.

```sh
tools/hyperv/mosvm.sh check                       # KVM, QEMU devices, IOMMU groups
make run-hyperv                                   # build, then boot with a window
tools/hyperv/mosvm.sh run --commands hyperv,gpu   # headless, 90 s, log to build/hyperv/serial.log
tools/hyperv/mosvm.sh run --like-this-pc          # UEFI, xHCI, one vCPU per host thread
tools/hyperv/mosvm.sh run --disk sata256.img      # with a disk (opened read-only, -snapshot)
sudo tools/hyperv/mosvm.sh gpu --device 10de:1c81 # lend a PCI device with VFIO
sudo tools/hyperv/mosvm.sh return                 # give lent devices back (recovery)
```

Needs `qemu-system-x86_64` (QEMU 8.2 or later for `hv-balloon`) and access
to `/dev/kvm` (the `kvm` group). `make run-hyperv HYPERV_ARGS="--seconds 120"`
passes options through. Commands come from `--commands`, else
`build/hyperv/commands.txt` (one per line), else `hyperv`, `gpu`,
`gpu nvidia`, `fglcube 5` and `fglcube 5 --shader`. They are typed only
after the terminal prints `=== TERMINAL READY ===`.

The VM boots `build/hyperv/test.iso`, made from the built ISO: no GRUB
menu, the `serial-echo` option (command output lands in `serial.log`) and
RamFS, so `0:/programs` exists without a disk. `--disk` or `--no-ramfs`
boot without RamFS; `--plain-iso` boots the ISO unchanged. `mosvm.sh
--help` lists every option.

Expected `serial.log` lines:

```
hyperv: running under Hyper-V, privileges 0x...
hyperv: hypercalls and SynIC ready, VP 0
vmbus: InitiateContact post failed, status 18
vmbus: connected, protocol 4.0
vmbus: offer relid 1 dynmem {525074dc-8985-46e2-8057-a307dc18a502}
```

QEMU implements VMBus up to protocol 4.0, so the guest's first attempt
(the newer protocol, on another connection id) is refused and it falls back;
real Hyper-V accepts the first one. QEMU offers no synthetic storage,
network or video channels, and no Hyper-V vPCI channel: a passed-through
device appears on the ordinary PCI bus, so `hv_pci.c` is only exercised on
real Hyper-V.

Booting with `--disk` under KVM used to stop for minutes after the AHCI
IDENTIFY: port resets waited a fixed number of register reads, each slow
in a VM, on every empty port. Resets now wait in milliseconds (fixed
2026-10-22).

### GPU passthrough (`mosvm.sh gpu`)

`gpu` binds the device and every other endpoint in its IOMMU group to
`vfio-pci`, boots the VM with them attached, and rebinds them to their
drivers when the VM stops, even if the script is interrupted (`return`
does the same by hand). It needs root and the IOMMU:

- kernel command line: `amd_iommu=on iommu=pt` (AMD) or `intel_iommu=on iommu=pt` (Intel);
- firmware: SVM / VT-d and IOMMU enabled.

A device that drives the host's screen (`check` marks it "host screen") is
refused unless you add `--force`. With `--force` the script runs itself in
the background as the systemd unit `mosvm-gpu`, because the desktop and
the terminal it was started from are about to go away. It:

1. stops the display manager (the session closes, so save your work), and
   waits until nothing holds `/dev/dri/card*` open;
2. detaches the text console from the card;
3. lends the card, runs the VM (the monitor stays dark), gives the card back;
4. reattaches the console and starts the display manager again.

By default the card is the VM's only screen (`x-vga`): its own VBIOS
sets the display mode, the picture appears on the monitor attached to
the card, and Minimal-OS draws on it, so `fglcube` exercises FarcadeGL on
the NVIDIA adapter (`--std-vga` keeps QEMU's emulated screen instead).
For an NVIDIA card the firmware folder from `/lib/firmware/nvidia/<chip>`
is copied into the RAM disk as `0:/firmware/nvidia/<chip>`, which
`gpu nvidia` checks.

What it did goes to `build/hyperv/gpu-run.log`; the guest's output to
`serial.log` as usual. If the login screen does not come back, use a text
console (Ctrl+Alt+F3) or SSH and run `sudo tools/hyperv/mosvm.sh return`.

Do not unbind the driver of a GPU a compositor is still drawing with:
on 2026-10-22 doing so (nouveau under kwin_wayland, RTX 2060 as the only
GPU) crashed kwin, killed the script mid-way and left the card unusable
until a reboot. That is why the steps above stop the display manager first.
