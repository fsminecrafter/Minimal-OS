# Installing

## From the terminal

Boot the ISO (CD/USB stick), then:

```text
disks                      find the target disk
install sata1              asks for confirmation, then erases sata1
install nvme0 --mbr --yes  MBR instead of GPT, no question
install sata1 --size 64G   leave the rest of the disk unpartitioned
install --system sata1p3 --boot sata1p1     use existing partitions
```

The installed disk boots on both BIOS and UEFI PCs:

| GPT (default) | MBR (`--mbr`, disks up to 2 TiB) |
| --- | --- |
| p1 EFI System Partition, FAT32, 256 MiB | p1 FAT32 boot partition, 256 MiB |
| p2 BIOS boot partition (GRUB core) | p2 MinimaFS system |
| p3 MinimaFS system | GRUB core in the gap before p1 |

The FAT32 partition holds GRUB for UEFI (`EFI/BOOT/BOOTX64.EFI`), the
kernel, the install payload and `boot/grub/grub.cfg`, which passes
`root=<GUID of the system partition>` so the kernel mounts it as `0:`.
The system partition gets the same files `format` installs. An installed
system can install again.

## From MOSdesk

Run `make run-mosdesk`, sign in and open **Installer**. It lists the
disks (the system disk cannot be chosen), lets you pick GPT or MBR, asks
for your account password and shows the progress. `make run` attaches an
empty 8 GiB `install-target.img` as a second disk for this; afterwards
`make run-installed` (BIOS) or `make run-installed-uefi` (needs the
`ovmf` package) boots it without the CD.

## Building

The ISO is BIOS + UEFI bootable and the installer can write UEFI boot
files when the build machine has `grub-pc-bin`, `grub-efi-amd64-bin`,
`xorriso` and `mtools` installed.

## Real hardware notes

What has been made to work for real PCs:

- more than 1 GiB of RAM, memory holes and ACPI tables above 1 GiB,
- UEFI boot (GOP framebuffer, PS/2 controller initialised by the OS),
- USB 3 (xHCI) keyboards and mice, PS/2 keyboards,
- SATA (AHCI, all controllers and ports) and NVMe disks, GPT, large disks,
- write-combining framebuffer mapping.

Not yet: USB hubs and USB storage, EHCI, network cards other than the
RTL8139 and RTL8111/8168, audio other than AC'97/PC speaker, GPU acceleration.
