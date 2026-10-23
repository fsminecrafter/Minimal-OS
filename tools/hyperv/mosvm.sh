#!/usr/bin/env bash
# Minimal-OS test VM on Linux: the Linux counterpart of mosvm.ps1 and
# gpu-test.ps1.
#
# Linux has no Hyper-V, but QEMU/KVM can show the guest the same interface:
# the Hyper-V CPUID leaves and MSRs, the hypercall page, the SynIC, and a
# VMBus (vmbus-bridge) offering a channel (hv-balloon). That is what
# src/modules/HyperV/hyperv.c talks to, so the 'hyperv' command reports a
# hypervisor and VMBus channels just as in a Hyper-V Generation 1 VM.
# Device passthrough uses VFIO instead of Hyper-V DDA.
#
#   mosvm.sh check                      KVM, QEMU features, IOMMU groups
#   mosvm.sh run [options]              boot dist/x86_64/kernel.iso, type the
#                                       commands into COM1, log everything to
#                                       build/hyperv/serial.log, then stop
#   mosvm.sh gpu [options]              the same with a PCI device passed
#                                       through (needs root: sudo)
#   mosvm.sh return                     give lent devices back to their
#                                       drivers (recovery; needs root)
#
# The VM boots a test copy of the ISO (build/hyperv/test.iso) that starts
# Minimal-OS in RamFS mode: 0: is a RAM disk with the system files, so
# programs such as fglcube are there without a disk image. Everything the
# terminal prints is copied to COM1 ("serial-echo"), so serial.log shows
# command output too.
#
# Options:
#   --seconds N         how long the VM runs (default 90; 150 for gpu)
#   --commands A,B      terminal commands (default: hyperv,gpu,gpu nvidia,
#                       fglcube 5,fglcube 5 --shader;
#                       build/hyperv/commands.txt if present, one per line)
#   --iso PATH          ISO to boot (default dist/x86_64/kernel.iso)
#   --disk IMG          attach IMG as the SATA disk (opened with -snapshot,
#                       so the image itself is never written) and boot
#                       without RamFS, so that disk is mounted as 0:
#   --no-ramfs          boot without RamFS (0: is the first disk, if any)
#   --plain-iso         boot the ISO exactly as built (no serial echo)
#   --firmware DIR      copied into the RAM disk as 0:/firmware (a
#                       linux-firmware layout: DIR/nvidia/tu106/...). For gpu
#                       runs on an NVIDIA card the default is the card's
#                       folder from /lib/firmware/nvidia; 'none' for nothing
#   --std-vga           (gpu) keep QEMU's emulated VGA as the screen. By
#                       default a lent VGA card is the VM's only screen
#                       (x-vga), so its own VBIOS sets the display mode and
#                       Minimal-OS (and FarcadeGL) draws on the card; the
#                       picture appears on the monitor attached to it
#   --cpus N            virtual CPUs (default 2)
#   --uefi              boot with UEFI (OVMF) on a q35 machine instead of
#                       BIOS on i440fx
#   --xhci              keyboard and mouse on an xHCI controller instead of
#                       UHCI
#   --like-this-pc      a VM shaped like this computer: --uefi --xhci
#                       --no-hyperv and one virtual CPU per host thread
#   --gui               show the screen in a window (default: headless)
#   --qmp PATH          QMP socket, for driving the VM from scripts
#   --no-hyperv         plain KVM, no Hyper-V enlightenments or VMBus
#   --device BDF|ID     (gpu) the device to lend: 0000:01:00.0, or a
#                       vendor:device id such as 10de:1c81 (default: the
#                       first NVIDIA VGA device). Every function in its IOMMU
#                       group is lent with it.
#   --force             (gpu) also lend a device that drives the host's
#                       screen. The script then runs itself in the
#                       background as the systemd unit "mosvm-gpu": it stops
#                       the display manager (log out first: open windows
#                       close), lends the card, runs the VM, gives the card
#                       back and starts the display manager again. The
#                       screen is dark meanwhile; progress goes to
#                       build/hyperv/gpu-run.log.
#
# GPU passthrough needs the IOMMU on: "amd_iommu=on iommu=pt" (AMD) or
# "intel_iommu=on iommu=pt" (Intel) on the kernel command line, and in the
# firmware (SVM/VT-d, IOMMU = Enabled).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out_dir="$repo/build/hyperv"
state_file="$out_dir/lent-devices.txt"   # BDF|original driver, one per line

screen_file="$out_dir/host-screen.txt"   # display manager / consoles to restore

action="${1:-check}"
case "$action" in -h|--help|help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;; esac
[ $# -gt 0 ] && shift
orig_args=("$@")
seconds=""
commands=""
iso="$repo/dist/x86_64/kernel.iso"
disk=""
ramfs=1
plain_iso=0
firmware=""
std_vga=0
cpus=2
uefi=0
xhci=0
gui=0
qmp=""
hyperv=1
device=""
force=0

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) seconds="$2"; shift 2 ;;
        --commands) commands="$2"; shift 2 ;;
        --iso) iso="$2"; shift 2 ;;
        --disk) disk="$2"; ramfs=0; shift 2 ;;
        --no-ramfs) ramfs=0; shift ;;
        --plain-iso) plain_iso=1; ramfs=0; shift ;;
        --firmware) firmware="$2"; shift 2 ;;
        --std-vga) std_vga=1; shift ;;
        --cpus) cpus="$2"; shift 2 ;;
        --uefi) uefi=1; shift ;;
        --xhci) xhci=1; shift ;;
        --like-this-pc) uefi=1; xhci=1; hyperv=0; cpus=$(nproc); shift ;;
        --gui) gui=1; shift ;;
        --qmp) qmp="$2"; shift 2 ;;
        --no-hyperv) hyperv=0; shift ;;
        --device) device="$2"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "mosvm.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

die() { echo "mosvm.sh: $*" >&2; exit 1; }

# ------------------------------------------------------------------ check
qemu_has_device() { qemu-system-x86_64 -device help 2>/dev/null | grep -q "name \"$1\""; }

check() {
    local ok=1
    command -v qemu-system-x86_64 >/dev/null || { echo "QEMU: missing (apt install qemu-system-x86)"; ok=0; }
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then echo "KVM: usable"
    else echo "KVM: /dev/kvm not usable (load kvm_amd/kvm_intel, join the kvm group)"; ok=0; fi
    if command -v qemu-system-x86_64 >/dev/null; then
        qemu-system-x86_64 --version | head -1
        for d in vmbus-bridge hv-balloon vfio-pci; do
            if qemu_has_device "$d"; then echo "  device $d: yes"; else echo "  device $d: no"; fi
        done
    fi
    if [ -d /sys/kernel/iommu_groups ] && [ -n "$(ls /sys/kernel/iommu_groups)" ]; then
        echo "IOMMU: on ($(ls /sys/kernel/iommu_groups | wc -l) groups)"
        for dev in /sys/bus/pci/devices/*; do
            local class; class=$(cat "$dev/class")
            case "$class" in 0x03*) ;; *) continue ;; esac
            local bdf group driver vga=""
            bdf=$(basename "$dev")
            group=$(basename "$(readlink "$dev/iommu_group")")
            driver=$( [ -e "$dev/driver" ] && basename "$(readlink "$dev/driver")" || echo none)
            [ "$(cat "$dev/boot_vga" 2>/dev/null)" = 1 ] && vga=" (host screen)"
            echo "  $bdf $(cat "$dev/vendor" | cut -c3-):$(cat "$dev/device" | cut -c3-) group $group driver $driver$vga"
        done
    else
        echo "IOMMU: off (GPU passthrough needs it; see the notes at the top of this script)"
    fi
    [ "$ok" = 1 ]
}

# ---------------------------------------------------------- VFIO lending
resolve_device() {
    local want="$1"
    if [ -z "$want" ]; then
        for dev in /sys/bus/pci/devices/*; do
            [ "$(cat "$dev/vendor")" = 0x10de ] || continue
            case "$(cat "$dev/class")" in 0x03*) basename "$dev"; return ;; esac
        done
        die "no NVIDIA display device found; pass --device"
    fi
    if [ -e "/sys/bus/pci/devices/$want" ]; then echo "$want"; return; fi
    if [ -e "/sys/bus/pci/devices/0000:$want" ]; then echo "0000:$want"; return; fi
    local vendor="0x${want%%:*}" id="0x${want##*:}"
    for dev in /sys/bus/pci/devices/*; do
        if [ "$(cat "$dev/vendor")" = "$vendor" ] && [ "$(cat "$dev/device")" = "$id" ]; then
            basename "$dev"; return
        fi
    done
    die "no PCI device $want"
}

# Every endpoint in the device's IOMMU group. Bridges (class 0x06: host
# bridges such as AMD's "PCIe Dummy Host Bridge" as well as PCI bridges)
# stay with the host; VFIO does not need them.
group_members() {
    local group="/sys/bus/pci/devices/$1/iommu_group/devices"
    [ -d "$group" ] || die "$1 has no IOMMU group: is the IOMMU on? (mosvm.sh check)"
    for dev in "$group"/*; do
        case "$(cat "$dev/class")" in 0x06*) continue ;; esac
        basename "$dev"
    done
}

current_driver() {
    local dev="/sys/bus/pci/devices/$1"
    if [ -e "$dev/driver" ]; then basename "$(readlink "$dev/driver")"; fi
}

# Does the device drive a screen: the boot display, or a DRM driver bound to it?
is_screen() {
    local dev="/sys/bus/pci/devices/$1"
    [ "$(cat "$dev/boot_vga" 2>/dev/null)" = 1 ] || [ -d "$dev/drm" ]
}

# PIDs holding the device's /dev/dri nodes open (compositor, X, greeter, apps).
drm_users() {
    local nodes=() n fd target
    for n in /sys/bus/pci/devices/"$1"/drm/*; do
        [ -e "$n" ] && nodes+=("/dev/dri/$(basename "$n")")
    done
    [ ${#nodes[@]} -gt 0 ] || return 0
    for fd in /proc/[0-9]*/fd/*; do
        target=$(readlink "$fd" 2>/dev/null) || continue
        for n in "${nodes[@]}"; do
            if [ "$target" = "$n" ]; then fd=${fd#/proc/}; echo "${fd%%/*}"; fi
        done
    done | sort -un
}

# Unbinding a GPU driver while a compositor still uses it crashes the
# compositor and can leave the card dead until reboot (seen with nouveau and
# kwin_wayland). So stop the display manager, wait until nothing holds the
# card, and detach the text console from it first.
release_host_screen() {
    local bdf="$1" pid
    if [ ! -f "$screen_file" ]; then
        : > "$screen_file"
        if systemctl is-active --quiet display-manager.service; then
            echo "dm" >> "$screen_file"
        fi
    fi
    if systemctl is-active --quiet display-manager.service; then
        echo "Stopping the display manager"
        systemctl stop display-manager.service
    fi
    for _ in $(seq 30); do
        [ -z "$(drm_users "$bdf")" ] && break
        sleep 1
    done
    if [ -n "$(drm_users "$bdf")" ]; then
        echo "Still using $bdf after 30 s, not lending it:"
        for pid in $(drm_users "$bdf"); do
            echo "  $pid $(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)"
        done
        return 1
    fi
    for vt in /sys/class/vtconsole/vtcon*; do
        [ "$(cat "$vt/bind")" = 1 ] || continue
        grep -q "frame buffer" "$vt/name" || continue
        echo "Detaching the text console ($(basename "$vt"))"
        echo "vt|$(basename "$vt")" >> "$screen_file"
        echo 0 > "$vt/bind"
    done
}

restore_host_screen() {
    [ -f "$screen_file" ] || return 0
    local line
    # The driver needs a moment to bring the card back before anything draws.
    for _ in $(seq 20); do
        ls /sys/class/drm/ 2>/dev/null | grep -q '^card[0-9]*$' && break
        sleep 0.5
    done
    while IFS='|' read -r kind name; do
        if [ "$kind" = vt ] && [ -e "/sys/class/vtconsole/$name/bind" ]; then
            echo "Reattaching the text console ($name)"
            echo 1 > "/sys/class/vtconsole/$name/bind"
        fi
    done < "$screen_file"
    if grep -qx dm "$screen_file"; then
        echo "Starting the display manager"
        systemctl start display-manager.service
    fi
    rm -f "$screen_file"
}

# Runs from traps too, so a failing step (or a closed terminal) must not
# stop the rest: the subshell turns off set -e.
return_devices() (
    set +e
    if [ -f "$state_file" ]; then
        while IFS='|' read -r bdf driver; do
            [ -n "$bdf" ] || continue
            dev="/sys/bus/pci/devices/$bdf"
            now=$(current_driver "$bdf")
            echo > "$dev/driver_override"
            if [ "$now" = "$driver" ]; then
                continue             # never lent, or already back
            fi
            echo "Returning $bdf to ${driver:-no driver}"
            [ -n "$now" ] && echo "$bdf" > "$dev/driver/unbind"
            if [ -n "$driver" ] && [ -d "/sys/bus/pci/drivers/$driver" ]; then
                echo "$bdf" > "/sys/bus/pci/drivers/$driver/bind" 2>/dev/null ||
                    echo "$bdf" > /sys/bus/pci/drivers_probe
            elif [ -n "$driver" ]; then
                echo "$bdf" > /sys/bus/pci/drivers_probe
            fi
        done < "$state_file"
        rm -f "$state_file"
    fi
    true
)

# Everything back: devices to their drivers, then the screen. Also used
# by the 'return' action.
cleanup() (
    set +e
    return_devices
    restore_host_screen
    if [ -n "${SUDO_UID:-}" ] && [ -d "$out_dir" ]; then
        chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$out_dir"
    fi
    true
)

lend_devices() {
    local bdf="$1"
    [ "$(id -u)" = 0 ] || die "lending a device needs root: sudo $0 gpu ..."
    modprobe vfio-pci
    return_devices                           # leftovers from a broken run
    mkdir -p "$out_dir"
    local members; members=$(group_members "$bdf")
    for member in $members; do
        if [ "$(cat "/sys/bus/pci/devices/$member/boot_vga" 2>/dev/null)" = 1 ] && [ "$force" != 1 ]; then
            die "$member drives the host's screen; lending it blanks the screen until the VM stops. Use --force to do it anyway."
        fi
    done
    for member in $members; do
        local dev="/sys/bus/pci/devices/$member" driver=""
        [ -e "$dev/driver" ] && driver=$(basename "$(readlink "$dev/driver")")
        echo "$member|$driver" >> "$state_file"
        echo "Lending $member (was ${driver:-unbound})"
        [ -n "$driver" ] && echo "$member" > "$dev/driver/unbind"
        echo vfio-pci > "$dev/driver_override"
        echo "$member" > /sys/bus/pci/drivers_probe || true
        [ "$(current_driver "$member")" = vfio-pci ] ||
            die "$member did not bind to vfio-pci (still in use?)"
    done
}

# A screen-driving GPU is lent from a systemd unit, not from the terminal:
# stopping the display manager closes the terminal (and kills a script
# running in it), and the screen is dark until the card comes back.
lend_in_background() {
    local log="$out_dir/gpu-run.log"
    command -v systemd-run >/dev/null || die "lending the host's screen needs systemd-run"
    if systemctl is-active --quiet mosvm-gpu.service; then
        die "a GPU run is already going (systemctl status mosvm-gpu)"
    fi
    mkdir -p "$out_dir"
    : > "$log"
    systemd-run --unit=mosvm-gpu --collect --quiet \
        --setenv=MOSVM_DETACHED=1 \
        --setenv=SUDO_UID="${SUDO_UID:-}" --setenv=SUDO_GID="${SUDO_GID:-}" \
        -p StandardOutput="append:$log" -p StandardError="append:$log" \
        "$here/mosvm.sh" gpu "${orig_args[@]}"
    cat <<EOF
Started in the background (systemd unit mosvm-gpu).

In a few seconds the display manager stops: this session and its windows
close and the screen goes dark. After about ${seconds}s the card comes back
and the login screen returns. Then read:
  $log                   what the script did
  $out_dir/serial.log    what Minimal-OS printed

If the login screen does not return, switch to a text console
(Ctrl+Alt+F3) or log in over SSH and run: sudo $0 return
EOF
}

# ------------------------------------------------------ test ISO, firmware
# The NVIDIA chip of a device, as Minimal-OS names its firmware folder
# (nv_chip_name in src/modules/GPU/nv_fw.c), from PMC_BOOT_0. The kernel
# log has it when nouveau drove the card ("NVIDIA TU106 (166000a1)");
# otherwise BAR0 is read directly. Empty if unknown.
nv_chip() {
    local bdf="$1" chip boot0
    chip=$( { journalctl -k -b 0 --no-pager 2>/dev/null || dmesg 2>/dev/null; } |
            sed -n "s/.*nouveau $bdf: NVIDIA \([A-Z]*[0-9]*\) .*/\1/p" | tail -1)
    if [ -z "$chip" ] && [ -r "/sys/bus/pci/devices/$bdf/resource0" ]; then
        boot0=$(python3 -c 'import mmap, os, struct, sys
f = os.open(sys.argv[1], os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(f, 4096, mmap.MAP_SHARED, mmap.PROT_READ)
print(struct.unpack("<I", m[0:4])[0])' "/sys/bus/pci/devices/$bdf/resource0" 2>/dev/null) || boot0=""
        case $(( (${boot0:-0} >> 20) & 0x1ff )) in
            354) chip=tu102 ;; 356) chip=tu104 ;; 358) chip=tu106 ;;
            359) chip=tu117 ;; 360) chip=tu116 ;;
        esac
    fi
    echo "$chip" | tr '[:upper:]' '[:lower:]'
}

# Copies the card's linux-firmware folder (links followed: MinimaFS has
# none) to build/hyperv/firmware/nvidia/<chip>; prints the staging root.
stage_nv_firmware() {
    local bdf="$1" chip src stage="$out_dir/firmware"
    chip=$(nv_chip "$bdf")
    if [ -z "$chip" ]; then
        echo "Firmware: unknown chip for $bdf, none copied (use --firmware DIR)" >&2
        return 0
    fi
    src=/lib/firmware/nvidia/$chip
    if [ ! -d "$src" ]; then
        echo "Firmware: $src missing (install firmware-misc-nonfree), none copied" >&2
        return 0
    fi
    rm -rf "$stage"
    mkdir -p "$stage/nvidia"
    cp -rL "$src" "$stage/nvidia/$chip"
    # Compressed firmware (.zst/.xz) is unpacked; MinimaFS gets plain files.
    find "$stage" -name '*.zst' -exec zstd -qd --rm {} \; 2>/dev/null || true
    find "$stage" -name '*.xz' -exec xz -qd {} \; 2>/dev/null || true
    echo "Firmware: $src -> 0:/firmware/nvidia/$chip ($(du -sh "$stage" | cut -f1))" >&2
    echo "$stage"
}

# build/hyperv/test.iso: the kernel and installer payload of $iso, booted
# straight away with serial-echo, in RamFS mode unless --no-ramfs/--disk,
# plus $firmware (if any) as the "mosfirmware" module, which RamFS unpacks
# into 0:/firmware.
make_test_iso() {
    command -v xorriso >/dev/null && command -v grub-mkrescue >/dev/null ||
        die "the test ISO needs xorriso and grub-mkrescue (or use --no-ramfs)"
    local tree="$out_dir/iso-tree" fw_line=""
    rm -rf "$tree"
    mkdir -p "$tree/boot/grub"
    xorriso -osirrox on -indev "$iso" \
        -extract /boot/kernel.bin "$tree/boot/kernel.bin" \
        -extract /boot/mosinst.mpkg "$tree/boot/mosinst.mpkg" >/dev/null 2>&1 ||
        die "could not read boot/kernel.bin and boot/mosinst.mpkg from $iso"
    chmod -R u+w "$tree"
    if [ "$ramfs" = 1 ] && [ -n "$firmware" ] && [ "$firmware" != none ]; then
        [ -d "$firmware" ] || die "--firmware $firmware is not a directory"
        python3 "$repo/tools/mkpkg/mkpkg.py" "$firmware" "$tree/boot/mosfirmware.mpkg" \
            --store --no-cache >/dev/null || die "could not pack $firmware"
        fw_line=$'\tmodule2 /boot/mosfirmware.mpkg mosfirmware'
    fi
    # RAM disk: the system files plus room for the firmware.
    local mib=96 options="serial-echo"
    [ -n "$fw_line" ] && mib=$(( 96 + $(du -sm "$firmware" | cut -f1) * 2 ))
    [ "$ramfs" = 1 ] && options="ramfs=$mib serial-echo"
    local title="Minimal-OS (test VM: serial echo)"
    [ "$ramfs" = 1 ] && title="Minimal-OS (test VM: RamFS, serial echo)"
    cat > "$tree/boot/grub/grub.cfg" <<GRUBCFG
set timeout=0
set default=0

insmod all_video
if loadfont \${prefix}/fonts/unicode.pf2; then
	insmod gfxterm
	set gfxmode=auto
	set gfxpayload=keep
	terminal_output gfxterm
fi

menuentry "$title" {
	multiboot2 /boot/kernel.bin $options
	module2 /boot/kernel.bin moskernel
	module2 /boot/mosinst.mpkg mosinst
$fw_line
	boot
}
GRUBCFG
    grub-mkrescue -o "$out_dir/test.iso" "$tree" >/dev/null 2>&1 ||
        die "grub-mkrescue failed (try: grub-mkrescue -o $out_dir/test.iso $tree)"
}

# -------------------------------------------------------------------- run
# Hyper-V enlightenments: what a Generation 1 Hyper-V VM shows the guest.
hv_cpu="host,hv-relaxed,hv-vapic,hv-spinlocks=0x1fff,hv-vpindex,hv-runtime,hv-time,hv-synic,hv-stimer,hv-frequencies,hv-reset"

run_vm() {
    local passthrough=("$@")
    [ -f "$iso" ] || die "no ISO at $iso (make build-x86_64, or make MOSDESK=1 build-x86_64)"
    [ -r /dev/kvm ] && [ -w /dev/kvm ] || die "/dev/kvm is not usable; Hyper-V enlightenments need KVM"
    mkdir -p "$out_dir"
    local sock="$out_dir/com1.sock" log="$out_dir/serial.log"
    rm -f "$sock"

    local cmds=()
    if [ -n "$commands" ]; then
        IFS=',' read -r -a cmds <<< "$commands"
    elif [ -f "$out_dir/commands.txt" ]; then
        mapfile -t cmds < <(grep -v '^[[:space:]]*$' "$out_dir/commands.txt")
    else
        cmds=(hyperv gpu "gpu nvidia" "fglcube 5" "fglcube 5 --shader")
    fi

    local boot_iso="$iso"
    if [ "$plain_iso" != 1 ]; then
        make_test_iso
        boot_iso="$out_dir/test.iso"
    fi

    # A lent VGA card becomes the only screen: its VBIOS runs under
    # SeaBIOS (x-vga gives it the legacy VGA ranges), GRUB sets a VBE mode
    # on it, and Minimal-OS finds that framebuffer inside the card's VRAM,
    # making the card the primary adapter that FGPU/FarcadeGL draws to.
    local card_vga=0 bdf
    if [ "$std_vga" != 1 ]; then
        for bdf in "${passthrough[@]}"; do
            [ "$(cat "/sys/bus/pci/devices/$bdf/class")" = 0x030000 ] && card_vga=1
        done
    fi

    # Generation 1 equivalent: i440fx PC with BIOS boot from the CD.
    local machine=pc
    [ "$uefi" = 1 ] && machine=q35
    local args=(-machine "$machine,accel=kvm" -smp "$cpus" -m 2048M -boot d -cdrom "$boot_iso"
                -no-reboot -serial "unix:$sock,server=on,wait=on"
                -netdev user,id=net0 -device rtl8139,netdev=net0)
    if [ "$xhci" = 1 ]; then
        args+=(-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0
               -device usb-mouse,bus=xhci.0)
    else
        args+=(-device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1
               -device usb-mouse,bus=uhci.0,port=2)
    fi
    if [ "$uefi" = 1 ]; then
        local code=/usr/share/OVMF/OVMF_CODE_4M.fd vars=/usr/share/OVMF/OVMF_VARS_4M.fd
        [ -f "$code" ] && [ -f "$vars" ] || die "--uefi needs OVMF (apt install ovmf)"
        cp "$vars" "$out_dir/ovmf-vars.fd"
        args+=(-drive "if=pflash,format=raw,unit=0,readonly=on,file=$code"
               -drive "if=pflash,format=raw,unit=1,file=$out_dir/ovmf-vars.fd")
    fi
    if [ "$hyperv" = 1 ]; then
        args+=(-cpu "$hv_cpu")
        qemu_has_device vmbus-bridge && args+=(-device vmbus-bridge)
        qemu_has_device hv-balloon && args+=(-device hv-balloon,id=hvballoon)
    else
        args+=(-cpu host)
    fi
    if [ "$card_vga" = 1 ]; then
        args+=(-vga none)
        [ "$gui" = 1 ] || args+=(-display none)
    elif [ "$gui" = 1 ]; then args+=(-vga std); else args+=(-vga std -display none); fi
    [ -n "$qmp" ] && args+=(-qmp "unix:$qmp,server=on,wait=off")
    if [ -n "$disk" ]; then
        args+=(-snapshot -device ahci,id=ahci
               -drive "id=disk0,file=$disk,if=none,format=raw"
               -device ide-hd,drive=disk0,bus=ahci.0)
    fi
    for bdf in "${passthrough[@]}"; do
        if [ "$card_vga" = 1 ] && [ "$(cat "/sys/bus/pci/devices/$bdf/class")" = 0x030000 ]; then
            args+=(-device "vfio-pci,host=$bdf,x-vga=on")
        else
            args+=(-device "vfio-pci,host=$bdf")
        fi
    done

    qemu-system-x86_64 "${args[@]}" 2> "$out_dir/qemu.log" &
    local pid=$!
    # COM1 waits for vmserial.py (wait=on), so nothing printed is lost.
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
    python3 "$here/vmserial.py" "unix:$sock" "$log" --seconds "${seconds:-90}" \
        --commands "${cmds[@]}" || true
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$sock"
    echo
    echo "Serial log: $log"
    if grep -v -e 'info: QEMU waiting' -e 'terminating on signal' "$out_dir/qemu.log" 2>/dev/null | grep -q .; then
        echo "QEMU messages: $out_dir/qemu.log"
    fi
}

case "$action" in
    check) check ;;
    run) run_vm ;;
    gpu)
        target=$(resolve_device "$device")
        seconds="${seconds:-150}"
        [ "$(id -u)" = 0 ] || die "lending a device needs root: sudo $0 gpu ..."
        screen=""
        for member in $(group_members "$target"); do
            if is_screen "$member"; then screen="$member"; fi
        done
        if [ -n "$screen" ]; then
            [ "$force" = 1 ] ||
                die "$screen drives the host's screen; lending it stops the display manager and blanks the screen until the VM stops. Use --force to do it anyway."
            if [ -z "${MOSVM_DETACHED:-}" ]; then
                lend_in_background
                exit 0
            fi
        fi
        echo "mosvm.sh gpu: $(date '+%F %T'), lending the IOMMU group of $target"
        trap cleanup EXIT
        trap 'exit 130' INT TERM HUP
        if [ -n "$screen" ]; then
            release_host_screen "$screen" || exit 1
        fi
        # Firmware is read before lending, while the host driver still
        # knows the card.
        if [ "$ramfs" = 1 ] && [ -z "$firmware" ] &&
           [ "$(cat "/sys/bus/pci/devices/$target/vendor")" = 0x10de ]; then
            firmware=$(stage_nv_firmware "$target")
        fi
        lend_devices "$target"
        mapfile -t lent < <(cut -d'|' -f1 "$state_file")
        run_vm "${lent[@]}"
        ;;
    return)
        [ "$(id -u)" = 0 ] || die "returning devices needs root: sudo $0 return"
        cleanup
        ;;
    *) die "unknown action $action (check, run, gpu, return)" ;;
esac
