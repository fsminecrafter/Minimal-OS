# Hardware And Drivers

## Driver Manager Pattern

Hardware support is registered during kernel startup through manager APIs. A
manager selects an available driver, initializes it, and exposes a stable
subsystem interface to the rest of the kernel. This keeps device-specific code
out of terminal commands and protocol implementations.

Relevant interfaces are in `src/intf/x86_64` and implementations are in
`src/modules`:

- `network_manager.h` / network drivers;
- `gpu_manager.h` / framebuffer drivers;
- `usb_manager.h` / host-controller and device drivers;
- audio manager interfaces;
- storage manager and AHCI interfaces.

A missing device is normally a supported degraded state. Graphics can continue
headless, USB can fall back to PS/2, and audio can report that no compatible
controller exists.

## PCI And MMIO

PCI enumeration discovers devices by bus, device, and function. Driver code
reads vendor/device IDs and maps BARs through the kernel MMIO allocator rather
than using arbitrary virtual addresses.

The MMIO subsystem reserves a high virtual range, creates page-table entries,
and maps device registers or framebuffer memory. Device mappings must remain
page aligned and should use the mapping helpers rather than direct identity
assumptions.

## Graphics

The display itself still comes from the linear framebuffer the bootloader
set up (VBE on BIOS, GOP on UEFI); the Bochs/VBE driver can also set modes
on QEMU's `1234:1111` device. Framebuffers are mapped write-combining (PAT
entry 1), which makes pixel writes to a real graphics card much faster.

On top of that, `gpucore.c` turns each display-class PCI device into a GPU
adapter with video memory: total VRAM, the PCI BAR that is the CPU's window
onto it, where the visible framebuffer sits in VRAM, and a VRAM allocator
for the rest. Drivers:

| Driver | Devices | What it knows |
| --- | --- | --- |
| `gpu_nvidia.c` | NVIDIA, tuned for GTX 10 (Pascal) and RTX 20 / GTX 16 (Turing) | chip and board name, VRAM size from the memory controller, BAR1 window, temperature |
| `gpu_bochs.c` | QEMU/Bochs standard VGA | VRAM size, page flipping |
| `gpu_generic.c` | everything else | the framebuffer's BAR as VRAM window |

Drawing commands go through the FGPU command processor (`SYS_GPU`,
`fgpu.c`), whose current backend renders 3D on the CPU. See
[GPU and 3D](gpu.md).

## USB

UHCI and OHCI (USB 1.1) and xHCI (USB 3, every recent PC) controllers are
driven. Directly attached HID boot keyboards and mice work on all three;
hubs, EHCI, and USB storage are not supported yet. On xHCI the kernel takes
the controller over from the firmware (legacy hand-off) and polls its event
ring from the timer interrupt.

PS/2 keyboards work too: the 8042 controller is initialised at boot and its
scan codes are converted to the same HID reports, so layouts and key repeat
behave the same as with USB.

User programs use `SYS_USB` wrappers for initialization, polling, keyboard
presence, key translation, and keyboard metadata.

## Audio

Audio drivers are registered through the audio manager. The AC'97 path searches
for a compatible controller, configures the device, and provides a mix/update
callback. The kernel audio process updates playback independently of the
terminal.

If no compatible audio controller is present, initialization reports that state
and the rest of the system continues.

## Storage

Disks are block devices (`blockdev.c`): every SATA disk on every AHCI
controller (`sata0`, `sata1`, ...) and every NVMe namespace (`nvme0`, ...).
MBR and GPT partitions become child devices (`sata0p1`, `nvme0p2`).
MinimaFS works on any block device, so a volume can be a whole disk (the
original layout) or a partition. See [Storage and packages](storage-and-packages.md).

The filesystem uses a DMA bounce buffer for block I/O; the NVMe driver
builds PRP lists and is polled.

## Adding A Driver

A new driver normally needs:

1. a hardware-facing interface structure and callbacks;
2. a manager registration call during `kernel_main()`;
3. PCI/device detection and resource mapping;
4. an implementation under `src/modules` or the relevant subsystem;
5. a fallback behavior when the device is absent;
6. build configuration in `config.mk` if the module is optional.

Keep interrupt handlers short, protect shared state with the appropriate local
interrupt and SMP locking rules, and expose only subsystem-level operations to
commands and user syscalls.
