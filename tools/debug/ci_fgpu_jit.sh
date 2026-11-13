#!/usr/bin/env bash
# Boots the kernel ISO headless in QEMU in RamFS mode, types `gpu jit` (the FGPU shader
# JIT self-check: interpreter against SSE4.1/AVX2 machine code, also on the helper cores)
# and waits for "FGPU JIT selfcheck: PASS|FAIL|SKIP" in the serial log. Derived from
# ci_os0.sh (same pipes and GRUB handling; see there). Exit 0 on PASS or SKIP, 1 on FAIL,
# 2 on timeout / QEMU problems.
#   QEMU_EXTRA="-cpu max -smp 4" bash tools/debug/ci_fgpu_jit.sh     # JIT on (AVX2 under TCG)
#   bash tools/debug/ci_fgpu_jit.sh                                  # default qemu64: SKIP
#   GRUB_UP=3 bash tools/debug/ci_fgpu_jit.sh                        # nojit entry: SKIP
#
# How it works
#  * Serial (COM1) is a QEMU "pipe" chardev with a logfile: everything the guest
#    writes lands in $SERIAL_OUT, and bytes written to $TMP/ser.in reach the
#    guest.  The terminal polls COM1 and feeds the bytes through the normal
#    keyboard path (terminal_poll_serial_input), so commands are typed over the
#    serial line.  The keyboard layout is therefore irrelevant for the command.
#  * The only thing typed through the QEMU monitor ("sendkey") is the GRUB menu
#    selection of the last entry, "Minimal-OS (RamFS)": 'down' presses (the menu
#    does not wrap, so extra presses stay on the last entry) and Enter.  Those
#    keys are layout independent.  RamFS mode needs no disk image: 0: is a RAM
#    disk filled from the embedded package, which includes 0:/programs/os0test.run.
#
# Environment
#  * QEMU_EXTRA  extra QEMU arguments, appended to the command line (word split),
#                e.g. QEMU_EXTRA="-cpu max -smp 4". The default CPU (qemu64) has no AVX.
#  * GRUB_UP     number of 'up' presses after selecting the last GRUB entry; GRUB_UP=1
#                picks "Minimal-OS (RamFS, noavx)", the entry above the RamFS one.
#  * QEMU_BIN    the QEMU binary.
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_OUT="$WORKDIR/ci_fgpu_jit_serial.log"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_EXTRA="${QEMU_EXTRA:-}"
GRUB_UP="${GRUB_UP:-0}"
ISO="$WORKDIR/dist/x86_64/kernel.iso"
BOOT_TIMEOUT=120
TEST_TIMEOUT=${TEST_TIMEOUT:-900}
TEST_CMD="${TEST_CMD:-gpu jit}"

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

echo "[ci-fgpu-jit] Building kernel..."
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
    echo "[ci-fgpu-jit] KVM available, using it"
else
    echo "[ci-fgpu-jit] No writable /dev/kvm, using TCG (slow)"
fi

echo "[ci-fgpu-jit] Starting QEMU (headless), serial log: $SERIAL_OUT, extra args: ${QEMU_EXTRA:-none}"
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
echo "[ci-fgpu-jit] QEMU PID=$QEMU_PID"

fail_tail() {
    echo "[ci-fgpu-jit] Last 100 lines of $SERIAL_OUT:"
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
echo "[ci-fgpu-jit] Selected the RamFS GRUB entry, waiting for the terminal (up to ${BOOT_TIMEOUT}s)"

# "=== TERMINAL READY ===" is printed once the prompt is up.  If the package contains the
# desktop (mosWM), the terminal UI is skipped, but the terminal task still polls serial.
READY=""
for ((t = 0; t < BOOT_TIMEOUT; t++)); do
    if grep -aqF "=== TERMINAL READY ===" "$SERIAL_OUT" 2>/dev/null; then READY=1; break; fi
    if grep -aqF "mosWM detected; skipping terminal UI" "$SERIAL_OUT" 2>/dev/null; then READY=1; break; fi
    qemu_alive || { echo "[ci-fgpu-jit] FAIL: QEMU exited during boot"; fail_tail; exit 2; }
    sleep 1
done
if [ -z "$READY" ]; then
    echo "[ci-fgpu-jit] FAIL: terminal not ready within ${BOOT_TIMEOUT}s"
    fail_tail
    exit 2
fi
if ! grep -aqF "RamFS mode" "$SERIAL_OUT"; then
    echo "[ci-fgpu-jit] WARNING: no 'RamFS mode' marker in the log; the GRUB selection may have missed"
fi
echo "[ci-fgpu-jit] Terminal ready"

# Let the terminal_update task start polling the serial port before typing.
sleep 3

echo "[ci-fgpu-jit] Typing: $TEST_CMD"
serial_type "$TEST_CMD"
if ! wait_for "Command ready: '$TEST_CMD'" 20; then
    echo "[ci-fgpu-jit] WARNING: command echo not seen in the log, typing it once more"
    serial_type "$TEST_CMD"
fi

echo "[ci-fgpu-jit] Waiting for the verdict (up to ${TEST_TIMEOUT}s)"
RESULT=""
for ((t = 0; t < TEST_TIMEOUT; t++)); do
    if grep -aqE "FGPU JIT selfcheck: PASS" "$SERIAL_OUT" 2>/dev/null; then RESULT=PASS; break; fi
    if grep -aqE "FGPU JIT selfcheck: SKIP" "$SERIAL_OUT" 2>/dev/null; then RESULT=SKIP; break; fi
    if grep -aqE "FGPU JIT selfcheck: FAIL" "$SERIAL_OUT" 2>/dev/null; then RESULT=FAIL; break; fi
    qemu_alive || { echo "[ci-fgpu-jit] FAIL: QEMU exited during the test"; fail_tail; exit 2; }
    sleep 1
done
grep -a "FGPU JIT\|^cube-like\|^misc-builtins\|^  AVX2\|^  SSE4.1\|^    " "$SERIAL_OUT" | head -40 || true
case "$RESULT" in
    PASS|SKIP) echo "[ci-fgpu-jit] $RESULT"; exit 0 ;;
    FAIL) echo "[ci-fgpu-jit] FAIL"; exit 1 ;;
    *) echo "[ci-fgpu-jit] FAIL: no verdict within ${TEST_TIMEOUT}s"; fail_tail; exit 2 ;;
esac
