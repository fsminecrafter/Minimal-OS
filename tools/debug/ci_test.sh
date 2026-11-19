#!/usr/bin/env bash
# Headless boot smoke test (make ci-test): builds the ISO, boots it in QEMU and
# waits for the kernel terminal's "=== TERMINAL READY ===" line on the serial
# port. A kernel panic or an unhandled CPU exception fails at once.
#
#   CI_TIMEOUT   seconds to wait (default 120)
#   CI_QEMU      the QEMU binary (default qemu-system-x86_64)
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_OUT="$WORKDIR/ci_serial.log"
QEMU_BIN="${CI_QEMU:-qemu-system-x86_64}"
TIMEOUT="${CI_TIMEOUT:-120}"
READY="=== TERMINAL READY ==="
BROKEN="KERNEL PANIC|UNHANDLED CPU EXCEPTION"

echo "[ci] Building kernel..."
make -C "$WORKDIR" build-x86_64

echo "[ci] Starting QEMU (headless), logging serial to $SERIAL_OUT"
rm -f "$SERIAL_OUT"

# KVM when the host has it (much faster); TCG otherwise.
ACCEL=()
if [ -w /dev/kvm ]; then ACCEL=(-enable-kvm -cpu host); fi

"$QEMU_BIN" "${ACCEL[@]}" -smp 2 -cdrom "$WORKDIR/dist/x86_64/kernel.iso" -m 1024M -display none \
    -serial "file:$SERIAL_OUT" -no-reboot -no-shutdown &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT

echo "[ci] QEMU PID=$QEMU_PID, waiting up to ${TIMEOUT}s for \"$READY\""

RESULT=timeout
for _ in $(seq 1 "$TIMEOUT"); do
    if [ -f "$SERIAL_OUT" ]; then
        if grep -aqE "$BROKEN" "$SERIAL_OUT"; then RESULT=broken; break; fi
        if grep -aqF "$READY" "$SERIAL_OUT"; then RESULT=ready; break; fi
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then RESULT=exited; break; fi
    sleep 1
done

echo "[ci] Stopping QEMU..."
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
trap - EXIT

case "$RESULT" in
    ready)
        echo "[ci] PASS: the kernel terminal is ready ($(grep -ac '' "$SERIAL_OUT") lines of serial output)"
        exit 0 ;;
    broken)
        echo "[ci] FAIL: the kernel stopped:"
        grep -aE -A20 "$BROKEN" "$SERIAL_OUT" | head -40 ;;
    exited)
        echo "[ci] FAIL: QEMU exited before the terminal was ready" ;;
    *)
        echo "[ci] FAIL: no \"$READY\" within ${TIMEOUT}s" ;;
esac
echo "[ci] Last lines of serial output:"
tail -n 60 "$SERIAL_OUT" || true
exit 1
