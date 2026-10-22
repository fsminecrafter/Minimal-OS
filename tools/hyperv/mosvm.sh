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
# Options:
#   --seconds N         how long the VM runs (default 60; 90 for gpu)
#   --commands A,B      terminal commands (default: hyperv,gpu,gpu nvidia;
#                       build/hyperv/commands.txt if present, one per line)
#   --iso PATH          ISO to boot (default dist/x86_64/kernel.iso)
#   --disk IMG          attach IMG as the SATA disk (opened with -snapshot,
#                       so the image itself is never written)
#   --gui               show the screen in a window (default: headless)
#   --qmp PATH          QMP socket, for driving the VM from scripts
#   --no-hyperv         plain KVM, no Hyper-V enlightenments or VMBus
#   --device BDF|ID     (gpu) the device to lend: 0000:01:00.0, or a
#                       vendor:device id such as 10de:1c81 (default: the
#                       first NVIDIA VGA device). Every function in its IOMMU
#                       group is lent with it.
#   --force             (gpu) also lend a device that drives the host's
#                       screen; the screen goes dark until it is returned
#
# GPU passthrough needs the IOMMU on: "amd_iommu=on iommu=pt" (AMD) or
# "intel_iommu=on iommu=pt" (Intel) on the kernel command line, and in the
# firmware (SVM/VT-d, IOMMU = Enabled).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out_dir="$repo/build/hyperv"
state_file="$out_dir/lent-devices.txt"   # BDF|original driver, one per line

action="${1:-check}"
[ $# -gt 0 ] && shift
seconds=""
commands=""
iso="$repo/dist/x86_64/kernel.iso"
disk=""
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
        --disk) disk="$2"; shift 2 ;;
        --gui) gui=1; shift ;;
        --qmp) qmp="$2"; shift 2 ;;
        --no-hyperv) hyperv=0; shift ;;
        --device) device="$2"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

# Every endpoint in the device's IOMMU group (bridges stay with the host).
group_members() {
    local group="/sys/bus/pci/devices/$1/iommu_group/devices"
    [ -d "$group" ] || die "$1 has no IOMMU group: is the IOMMU on? (mosvm.sh check)"
    for dev in "$group"/*; do
        case "$(cat "$dev/class")" in 0x0604*) continue ;; esac
        basename "$dev"
    done
}

return_devices() {
    [ -f "$state_file" ] || return 0
    while IFS='|' read -r bdf driver; do
        [ -n "$bdf" ] || continue
        local dev="/sys/bus/pci/devices/$bdf"
        echo "Returning $bdf to ${driver:-no driver}"
        [ -e "$dev/driver" ] && echo "$bdf" > "$dev/driver/unbind" || true
        echo > "$dev/driver_override" || true
        if [ -n "$driver" ] && [ -d "/sys/bus/pci/drivers/$driver" ]; then
            echo "$bdf" > "/sys/bus/pci/drivers/$driver/bind" 2>/dev/null ||
                echo "$bdf" > /sys/bus/pci/drivers_probe || true
        else
            echo "$bdf" > /sys/bus/pci/drivers_probe || true
        fi
    done < "$state_file"
    rm -f "$state_file"
}

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
        echo "$member" > /sys/bus/pci/drivers_probe
        [ "$(basename "$(readlink "$dev/driver")")" = vfio-pci ] ||
            die "$member did not bind to vfio-pci (still in use?)"
    done
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
        cmds=(hyperv gpu "gpu nvidia")
    fi

    # Generation 1 equivalent: i440fx PC with BIOS boot from the CD.
    local args=(-machine pc,accel=kvm -smp 2 -m 2048M -boot d -cdrom "$iso"
                -no-reboot -serial "unix:$sock,server=on,wait=on"
                -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1
                -device usb-mouse,bus=uhci.0,port=2 -netdev user,id=net0
                -device rtl8139,netdev=net0)
    if [ "$hyperv" = 1 ]; then
        args+=(-cpu "$hv_cpu")
        qemu_has_device vmbus-bridge && args+=(-device vmbus-bridge)
        qemu_has_device hv-balloon && args+=(-device hv-balloon,id=hvballoon)
    else
        args+=(-cpu host)
    fi
    if [ "$gui" = 1 ]; then args+=(-vga std); else args+=(-vga std -display none); fi
    [ -n "$qmp" ] && args+=(-qmp "unix:$qmp,server=on,wait=off")
    if [ -n "$disk" ]; then
        args+=(-snapshot -device ahci,id=ahci
               -drive "id=disk0,file=$disk,if=none,format=raw"
               -device ide-hd,drive=disk0,bus=ahci.0)
    fi
    for bdf in "${passthrough[@]}"; do args+=(-device "vfio-pci,host=$bdf"); done

    qemu-system-x86_64 "${args[@]}" 2> "$out_dir/qemu.log" &
    local pid=$!
    # COM1 waits for vmserial.py (wait=on), so nothing printed is lost.
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
    python3 "$here/vmserial.py" "unix:$sock" "$log" --seconds "${seconds:-60}" \
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
        seconds="${seconds:-90}"
        trap return_devices EXIT INT TERM
        lend_devices "$target"
        mapfile -t lent < <(cut -d'|' -f1 "$state_file")
        run_vm "${lent[@]}"
        ;;
    return)
        [ "$(id -u)" = 0 ] || die "returning devices needs root: sudo $0 return"
        return_devices
        ;;
    *) die "unknown action $action (check, run, gpu, return)" ;;
esac
