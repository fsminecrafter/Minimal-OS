# Network boot (PXE)

Boot Minimal-OS on another PC on the same LAN, straight from
`dist/x86_64/kernel.iso` on this machine. No USB stick needed.

## Run it

```sh
make                # or make mosdesk: build the ISO as usual
make netboot        # = sudo tools/netboot/netboot.sh serve
```

Leave it running, then boot the other PC from the network. Press Ctrl+C to
stop. Rebuilt the ISO? Stop it and run `make netboot` again.

## How it works

1. `tools/netboot/netboot.sh` unpacks `/boot` from the ISO into
   `build/netboot/tftp/` and builds GRUB network images for BIOS
   (`core.0`) and UEFI (`core.efi`), with this machine's IP built in.
2. dnsmasq runs in **proxy DHCP** mode. The router still hands out IP
   addresses; dnsmasq only tells PXE clients which file to load.
3. The PC loads GRUB over TFTP. GRUB reads the ISO's own `grub.cfg` and
   loads `kernel.bin`, `mosinst.mpkg` and `mosfirmware.mpkg` over TFTP, so
   the boot menu is the same as when booting the ISO.

While it runs, the script opens UDP ports 67, 69 and 4011 in ufw on the LAN
interface and removes them again when it stops.

Settings: `NETBOOT_IFACE` (default: the default-route interface, wired
first) and `NETBOOT_ISO`.

## Network log

While `make netboot` runs, the booted PC sends its whole log back: boot
messages, everything the kernel writes to serial, program output, and
Mxplorer's console (site errors, failed loads, navigations). It appears in
the `make netboot` terminal as `[<pc ip>] ...` and is saved to
`build/netboot/logs/` (`latest.log` is the newest run).

How: the script adds `netlog=<this pc>:5140` to the kernel arguments in the
served `grub.cfg`. The kernel keeps every log line in a 512 KiB buffer
from the start of boot and sends it over UDP once the network has an
address (it runs DHCP itself if nothing else did after 20 s). Programs
write to the log, without showing it on screen, with `mos_log()` from
`minimalos.h` (file descriptor 3).

`NETBOOT_LOG_PORT=0 make netboot` turns it off; another number changes
the port. UDP 5140 is opened in ufw along with the PXE ports.

## Target PC BIOS setup (ASRock)

In the UEFI setup (press F2 or Del at power on):

- **Advanced > Network Stack Configuration**: Network Stack = Enabled,
  IPv4 PXE Support = Enabled.
- **Security > Secure Boot**: Disabled (the GRUB image is not signed).
- Save, reboot, press **F11** for the boot menu and pick
  `UEFI: PXE IPv4 Realtek ...`.

The PC must be cabled into the same network as this machine; PXE does not
work over Wi-Fi.

## Troubleshooting

- dnsmasq prints a `PXE(enp2s0) ... proxy` line when the PC asks. If nothing
  shows up, the PC is not on the same network or PXE is off in its BIOS.
- If the server's IP changes, restart `make netboot` (the IP is built into
  GRUB).
- Loading the ~68 MB of boot files over TFTP takes a little while.
