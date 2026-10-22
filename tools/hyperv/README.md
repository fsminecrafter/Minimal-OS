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
tools/hyperv/mosvm.sh run --commands hyperv,gpu   # headless, 60 s, log to build/hyperv/serial.log
tools/hyperv/mosvm.sh run --disk sata256.img      # with a disk (opened read-only, -snapshot)
sudo tools/hyperv/mosvm.sh gpu --device 10de:1c81 # lend a PCI device with VFIO
sudo tools/hyperv/mosvm.sh return                 # give lent devices back (recovery)
```

Needs `qemu-system-x86_64` (QEMU 8.2 or later for `hv-balloon`) and access
to `/dev/kvm` (the `kvm` group). `make run-hyperv HYPERV_ARGS="--seconds 120"`
passes options through. Commands come from `--commands`, else
`build/hyperv/commands.txt` (one per line), else `hyperv`, `gpu`,
`gpu nvidia` as on Windows.

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

Known issue (seen 2026-10-22): under KVM, booting from a formatted disk
(`--disk`) stopped at "Mounting disk" right after the AHCI IDENTIFY, while
the same image boots under plain QEMU (TCG). Runs without a disk are fine.

### GPU passthrough (`mosvm.sh gpu`)

`gpu` binds the device and every other endpoint in its IOMMU group to
`vfio-pci`, boots the VM with them attached, and rebinds them to their
drivers when the VM stops, even if the script is interrupted (`return`
does the same by hand). It needs root and the IOMMU:

- kernel command line: `amd_iommu=on iommu=pt` (AMD) or `intel_iommu=on iommu=pt` (Intel);
- firmware: SVM / VT-d and IOMMU enabled.

A device that drives the host's screen (`check` marks it "host screen") is
refused unless you add `--force`: lending it leaves the host without a
display until the VM stops, and its driver must let go of it (stop the
display manager first for `nvidia`).
