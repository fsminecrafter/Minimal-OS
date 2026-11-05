# Build Targets

The default Make target is `build-x86_64`. The build uses the
`x86_64-elf` cross-compiler at `$HOME/cross/bin` and host tools including Make,
NASM, Python 3, GRUB utilities, and xorriso.

## Common targets

| Command | Result |
| --- | --- |
| `make` or `make build-x86_64` | Build and link the kernel, package resources, and produce `dist/x86_64/kernel.iso` |
| `make minimaSSL-slib` | Build the SDK minimaSSL library used by the package build |
| `make test-host` | Compile and run the host-side C tests without booting the OS |
| `make ci-test` | Build the ISO and boot a headless QEMU smoke test |
| `make ci-interact` | Build and run the scripted serial-console test |
| `make run` | Build the ISO, create the install target image if needed, and boot the ISO in QEMU |
| `make mosdesk` | Build the ISO with MOSdesk included |
| `make run-mosdesk` | Build and run the MOSdesk ISO in QEMU |
| `make netboot` | Serve the built ISO for PXE boot on the LAN; requires elevated network setup |
| `make run-installed` | Boot `INSTALL_DISK` as a BIOS-installed system |
| `make run-installed-uefi` | Boot `INSTALL_DISK` with OVMF UEFI firmware |
| `make run-hyperv` | Run the Hyper-V-oriented QEMU test environment |
| `make clean` | Remove build outputs and all files ending in `.run` under the repository |

The full build and debugging overview is in
[Build and Debugging](build-and-debugging.md). The host and QEMU test coverage
is detailed in [Testing](testing.md).

## Build products

The kernel link output is `dist/x86_64/kernel.bin`. The build copies it into
`targets/x86_64/iso/boot/kernel.bin`, embeds the install and firmware packages,
then writes the bootable ISO to `dist/x86_64/kernel.iso`.

The resource package inputs are:

```text
src/resources/install1 -> 0:/etc
src/resources/install2 -> 0:/programs
src/resources/install3 -> 0:/services
```

The source resources are not build output. Edit them in place; the Makefile
repackages them as part of the kernel build.

## QEMU disk images

The default `make run` expects `sata256.img` to exist for the guest's primary
disk. Create a blank image if you do not already have one:

```sh
fallocate -l 256M sata256.img
```

The second disk for the installer is `install-target.img`. It is created
sparsely by Make when needed and defaults to 8 GiB. It is separate from the
primary disk image.

Useful overrides include:

```sh
make run INSTALL_DISK=install-target.img INSTALL_DISK_SIZE=16G
make run QEMU_SOUND=hda
make run-two QEMU_TWO_MCAST=230.0.0.1:12345
```

`run-two` needs `sata256.img`; if `sata256-2.img` does not exist, the target
copies the first image as its starting point. Use disposable test images for
filesystem and installer experiments.

## MOSdesk build

`make mosdesk` builds the desktop ISO; `make run-mosdesk` builds and launches
it. The targets build MOSdesk programs against this checkout's SDK. The
`MOSDESK_DIR` variable can point to another MOSdesk checkout, and
`MOSDESK_USE_LOCAL=1` tells the build to use already checked-out submodule
contents without fetching pinned revisions.

NVIDIA firmware is an optional build input. `make gpu-setup` downloads firmware
for the default supported card; choose another supported model with, for
example, `make gpu-setup GPU="GTX 1660 Ti"`. See
[GPU documentation](gpu.md#firmware-files) before distributing an image that
contains firmware.

## QEMU variants

The Makefile has additional targets for hardware and debugging scenarios:

| Target | Use |
| --- | --- |
| `run-audio` | Capture QEMU audio to WAV files |
| `run-sdl` | Use QEMU's SDL audio backend |
| `run-alsa` | Use QEMU's ALSA audio backend |
| `run-ps2` | Boot with PS/2 input instead of the configured USB devices |
| `run-int` | Log interrupt activity |
| `run-de` | Keep QEMU running after guest errors for debugging |
| `run-dex` | Start QEMU paused with the GDB stub enabled |
| `run-two` | Start two guests on a multicast network |

Some audio and network variants assume host audio devices, tap interfaces, or
other local setup. Check the target command in the root Makefile before using
one on a host with different devices.

## Cleanup

`make clean` removes `build/`, `dist/`, `Minimal-OS-SDK/build`, and generated
minimaSSL bundles. It also executes `find . -type f -name '*.run' -delete`,
which removes every `.run` file beneath the repository, including any
hand-authored or otherwise valuable bundle. Back up or move `.run` files you
need before running `make clean`.

The clean target does not remove disk images such as `sata256.img` or
`install-target.img`.
