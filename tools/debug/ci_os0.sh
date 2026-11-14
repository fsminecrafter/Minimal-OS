#!/usr/bin/env bash
# Boots the kernel ISO headless in QEMU in RamFS mode, runs the OS0 self-test
# program from the terminal and waits for "OS0TEST PASS" / "OS0TEST FAIL" in the
# serial log.  Exit 0 on PASS, 1 on FAIL, 2 on timeout / QEMU problems.
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
#                picks "Minimal-OS (RamFS, noavx)", the entry above the RamFS one;
#                GRUB_UP=4 picks "Minimal-OS (RamFS, threads1cpu)" (threads pinned).
#  * TEST_TIMEOUT  seconds to wait for the verdict (default 600).
#  * QEMU_BIN    the QEMU binary.
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_OUT="$WORKDIR/ci_os0_serial.log"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_EXTRA="${QEMU_EXTRA:-}"
GRUB_UP="${GRUB_UP:-0}"
ISO="$WORKDIR/dist/x86_64/kernel.iso"
BOOT_TIMEOUT=120
TEST_TIMEOUT="${TEST_TIMEOUT:-600}"
TEST_CMD="run 0:/programs/os0test.run"

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

echo "[ci-os0] Building kernel..."
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
    echo "[ci-os0] KVM available, using it"
else
    echo "[ci-os0] No writable /dev/kvm, using TCG (slow)"
fi

echo "[ci-os0] Starting QEMU (headless), serial log: $SERIAL_OUT, extra args: ${QEMU_EXTRA:-none}"
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
echo "[ci-os0] QEMU PID=$QEMU_PID"

fail_tail() {
    echo "[ci-os0] Last 100 lines of $SERIAL_OUT:"
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
echo "[ci-os0] Selected the RamFS GRUB entry, waiting for the terminal (up to ${BOOT_TIMEOUT}s)"

# "=== TERMINAL READY ===" is printed once the prompt is up.  If the package contains the
# desktop (mosWM), the terminal UI is skipped, but the terminal task still polls serial.
READY=""
for ((t = 0; t < BOOT_TIMEOUT; t++)); do
    if grep -aqF "=== TERMINAL READY ===" "$SERIAL_OUT" 2>/dev/null; then READY=1; break; fi
    if grep -aqF "mosWM detected; skipping terminal UI" "$SERIAL_OUT" 2>/dev/null; then READY=1; break; fi
    qemu_alive || { echo "[ci-os0] FAIL: QEMU exited during boot"; fail_tail; exit 2; }
    sleep 1
done
if [ -z "$READY" ]; then
    echo "[ci-os0] FAIL: terminal not ready within ${BOOT_TIMEOUT}s"
    fail_tail
    exit 2
fi
if ! grep -aqF "RamFS mode" "$SERIAL_OUT"; then
    echo "[ci-os0] WARNING: no 'RamFS mode' marker in the log; the GRUB selection may have missed"
fi
echo "[ci-os0] Terminal ready"

# Let the terminal_update task start polling the serial port before typing.
sleep 3

echo "[ci-os0] Typing: $TEST_CMD"
serial_type "$TEST_CMD"
if ! wait_for "Command ready: '$TEST_CMD'" 20; then
    echo "[ci-os0] WARNING: command echo not seen in the log, typing it once more"
    serial_type "$TEST_CMD"
fi

echo "[ci-os0] Waiting for OS0TEST PASS/FAIL (up to ${TEST_TIMEOUT}s)"
RESULT=""
for ((t = 0; t < TEST_TIMEOUT; t++)); do
    # The verdict is the line that ends right after PASS/FAIL; a failing check
    # prints "OS0TEST FAIL <name>: ..." and the run goes on to the other checks.
    if grep -aqE "OS0TEST PASS[[:space:]]*$" "$SERIAL_OUT" 2>/dev/null; then RESULT=PASS; break; fi
    if grep -aqE "OS0TEST FAIL[[:space:]]*$" "$SERIAL_OUT" 2>/dev/null; then RESULT=FAIL; break; fi
    qemu_alive || { echo "[ci-os0] FAIL: QEMU exited during the test"; fail_tail; exit 2; }
    sleep 1
done

case "$RESULT" in
    PASS)
        grep -a "OS0TEST" "$SERIAL_OUT" | grep -av "OS0TEST ok" || true
        echo "[ci-os0] PASS"
        exit 0
        ;;
    FAIL)
        echo "[ci-os0] FAIL: the self-test reported a failure"
        grep -a "OS0TEST FAIL" "$SERIAL_OUT" || true
        exit 1
        ;;
    *)
        echo "[ci-os0] FAIL: no OS0TEST result within ${TEST_TIMEOUT}s"
        fail_tail
        exit 2
        ;;
esac
