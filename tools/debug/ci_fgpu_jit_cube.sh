#!/usr/bin/env bash
# Boots the kernel ISO headless in QEMU, runs `fglcube --shader --still` (a lit, textured
# cube drawn by FGSL shaders, animation frozen) full screen from the terminal and saves
# QEMU monitor screendumps as PPM. Run it with the JIT on (QEMU_EXTRA="-cpu max") and off
# (GRUB_UP=3, the nojit entry, or the default CPU) and compare the files with cmp.
# Derived from ci_os0.sh (same pipes and GRUB handling; see there).
#   SHOT=/tmp/cube_jit.ppm QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_fgpu_jit_cube.sh
#   SHOT=/tmp/cube_nojit.ppm GRUB_UP=3 QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_fgpu_jit_cube.sh
#   cmp /tmp/cube_jit.ppm /tmp/cube_nojit.ppm && echo identical
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_OUT="$WORKDIR/ci_fgpu_jit_cube_serial.log"
SHOT="${SHOT:-/tmp/cube.ppm}"
SETTLE="${SETTLE:-120}"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_EXTRA="${QEMU_EXTRA:-}"
GRUB_UP="${GRUB_UP:-0}"
ISO="$WORKDIR/dist/x86_64/kernel.iso"
BOOT_TIMEOUT=120
TEST_TIMEOUT=${TEST_TIMEOUT:-900}
TEST_CMD="${TEST_CMD:-run 0:/programs/fglcube.run 3000 --shader --still}"

TMP=$(mktemp -d)
QEMU_PID=""
BG_PIDS=""

cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    for p in $BG_PIDS; do kill "$p" 2>/dev/null || true; done
    rm -rf "$TMP"
}
trap cleanup EXIT

echo "[ci-fgpu-jit-cube] Building kernel..."
make -C "$WORKDIR" build-x86_64

mkfifo "$TMP/ser.in" "$TMP/ser.out" "$TMP/mon.in" "$TMP/mon.out"
rm -f "$SERIAL_OUT"

# Nobody else consumes the output side of the pipes; drain them so QEMU never blocks.
cat "$TMP/ser.out" >/dev/null &
BG_PIDS="$BG_PIDS $!"
cat "$TMP/mon.out" >/dev/null &
BG_PIDS="$BG_PIDS $!"

ACCEL=()
if [ -w /dev/kvm ]; then
    ACCEL=(-accel kvm)
    echo "[ci-fgpu-jit-cube] KVM available, using it"
else
    echo "[ci-fgpu-jit-cube] No writable /dev/kvm, using TCG (slow)"
fi

echo "[ci-fgpu-jit-cube] Starting QEMU (headless), serial log: $SERIAL_OUT, extra args: ${QEMU_EXTRA:-none}"
"$QEMU_BIN" \
    "${ACCEL[@]}" \
    -smp 2 -m "${QEMU_MEM:-1024M}" \
    -cdrom "$ISO" -boot d \
    -display none \
    -chardev "pipe,id=ser0,path=$TMP/ser,logfile=$SERIAL_OUT" -serial chardev:ser0 \
    -chardev "pipe,id=mon0,path=$TMP/mon" -mon chardev=mon0,mode=readline \
    -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0 \
    -no-reboot -no-shutdown \
    $QEMU_EXTRA &
QEMU_PID=$!
echo "[ci-fgpu-jit-cube] QEMU PID=$QEMU_PID"

fail_tail() {
    echo "[ci-fgpu-jit-cube] Last 100 lines of $SERIAL_OUT:"
    tail -n 100 "$SERIAL_OUT" 2>/dev/null || echo "(no serial log)"
}

qemu_alive() { kill -0 "$QEMU_PID" 2>/dev/null; }

monitor_cmd() { printf '%s\n' "$1" > "$TMP/mon.in"; }

# Type a line into the guest over the serial port, one character at a time.
serial_type() {
    local line="$1" i ch
    for ((i = 0; i < ${#line}; i++)); do
        ch="${line:$i:1}"
        printf '%s' "$ch" > "$TMP/ser.in"
        sleep 0.05
    done
    printf '\r' > "$TMP/ser.in"
}

# wait_for <fixed string> <seconds>: returns 0 when the string shows up in the serial log.
wait_for() {
    local pattern="$1" secs="$2" i
    for ((i = 0; i < secs; i++)); do
        if [ -f "$SERIAL_OUT" ] && grep -aqF -- "$pattern" "$SERIAL_OUT"; then
            return 0
        fi
        qemu_alive || return 1
        sleep 1
    done
    return 1
}

# --- GRUB: choose "Minimal-OS (RamFS)" (last menu entry) -----------------------------
# The menu timeout is 3 s; the first key press stops the countdown.  Keys that arrive
# before GRUB is up may be lost, so press several times.
sleep 2
for _ in 1 2 3 4 5 6 7 8; do
    monitor_cmd "sendkey down"
    sleep 0.4
done
for ((i = 0; i < GRUB_UP; i++)); do
    monitor_cmd "sendkey up"
    sleep 0.4
done
monitor_cmd "sendkey ret"
echo "[ci-fgpu-jit-cube] Selected the RamFS GRUB entry, waiting for the terminal (up to ${BOOT_TIMEOUT}s)"

# "=== TERMINAL READY ===" is printed once the prompt is up.  If the package contains the
# desktop (mosWM), the terminal UI is skipped, but the terminal task still polls serial.
READY=""
for ((t = 0; t < BOOT_TIMEOUT; t++)); do
    if grep -aqF "=== TERMINAL READY ===" "$SERIAL_OUT" 2>/dev/null; then READY=1; break; fi
    if grep -aqF "mosWM detected; skipping terminal UI" "$SERIAL_OUT" 2>/dev/null; then READY=1; break; fi
    qemu_alive || { echo "[ci-fgpu-jit-cube] FAIL: QEMU exited during boot"; fail_tail; exit 2; }
    sleep 1
done
if [ -z "$READY" ]; then
    echo "[ci-fgpu-jit-cube] FAIL: terminal not ready within ${BOOT_TIMEOUT}s"
    fail_tail
    exit 2
fi
if ! grep -aqF "RamFS mode" "$SERIAL_OUT"; then
    echo "[ci-fgpu-jit-cube] WARNING: no 'RamFS mode' marker in the log; the GRUB selection may have missed"
fi
echo "[ci-fgpu-jit-cube] Terminal ready"

# Let the terminal_update task start polling the serial port before typing.
sleep 3

echo "[ci-fgpu-jit-cube] Typing: $TEST_CMD"
serial_type "$TEST_CMD"
if ! wait_for "Command ready: '$TEST_CMD'" 20; then
    echo "[ci-fgpu-jit-cube] WARNING: command echo not seen in the log, typing it once more"
    serial_type "$TEST_CMD"
fi

echo "[ci-fgpu-jit-cube] Letting the cube render for ${SETTLE}s"
sleep "$SETTLE"
qemu_alive || { echo "[ci-fgpu-jit-cube] FAIL: QEMU exited"; fail_tail; exit 2; }
monitor_cmd "screendump $SHOT"
sleep 5
sleep 40
monitor_cmd "screendump $SHOT.2"
sleep 5
grep -a "GPU:\|fglcube\|hw-3d\|FGPU" "$SERIAL_OUT" | head -10 || true
ls -l "$SHOT" "$SHOT.2" || { echo "[ci-fgpu-jit-cube] FAIL: no screendump"; exit 2; }
if cmp -s "$SHOT" "$SHOT.2"; then echo "[ci-fgpu-jit-cube] two screendumps 40 s apart are identical (the scene is still)"; else echo "[ci-fgpu-jit-cube] WARNING: the two screendumps differ (still rendering?)"; fi
md5sum "$SHOT" "$SHOT.2"
exit 0
