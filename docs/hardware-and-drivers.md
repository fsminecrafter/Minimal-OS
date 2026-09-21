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

The Bochs/VBE graphics driver provides a framebuffer and register BAR. The GPU
manager initializes it, after which the terminal uses graphics/text helpers for
its display. The framebuffer is also exposed to user programs through the
`SYS_GRAPHICS` syscall family.

The terminal sets a text-grid resolution over the framebuffer. Graphics commands
and SDK wrappers operate on pixels, primitives, text, resolution, and terminal
clear operations.

## USB

The UHCI path initializes the controller, builds frame lists and transfer
 descriptors, enumerates devices, and detects HID boot keyboards. USB manager
code registers the controller driver and exposes keyboard state to the terminal.

User programs use `SYS_USB` wrappers for initialization, polling, keyboard
presence, key translation, and keyboard metadata. USB polling is intentionally
explicit because the kernel must service the controller while user code is
running.

## Audio

Audio drivers are registered through the audio manager. The AC'97 path searches
for a compatible controller, configures the device, and provides a mix/update
callback. The kernel audio process updates playback independently of the
terminal.

If no compatible audio controller is present, initialization reports that state
and the rest of the system continues.

## AHCI Storage

AHCI probing identifies SATA devices and exposes sector size/capacity to the
MinimaFS disk wrapper. `initdisk` owns disk discovery and device initialization;
`format` and `mount` operate on the selected MinimaFS drive.

The filesystem uses a DMA bounce buffer for block I/O. Storage operations are
kept behind MinimaFS APIs so commands and package extraction do not manipulate
AHCI descriptors directly.

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
