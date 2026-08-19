#!/usr/bin/env bash
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_OUT="$WORKDIR/ci_serial.log"
QEMU_BIN="qemu-system-x86_64"
TIMEOUT=60

echo "[ci] Building kernel..."
make -C "$WORKDIR" build-x86_64

echo "[ci] Starting QEMU (headless), logging serial to $SERIAL_OUT"
rm -f "$SERIAL_OUT"

${QEMU_BIN} -cdrom "$WORKDIR/dist/x86_64/kernel.iso" -m 512M -nographic -serial file:$SERIAL_OUT -no-reboot -no-shutdown &
QEMU_PID=$!

echo "[ci] QEMU PID=$QEMU_PID"

SUCCESS=false
SEEN_LINES=0
for i in $(seq 1 $TIMEOUT); do
    if [ -f "$SERIAL_OUT" ]; then
        # Look for known boot success markers
        if grep -q "Minimal OS" "$SERIAL_OUT" || grep -q "Storage manager initialized" "$SERIAL_OUT" || grep -q "USB keyboard available!" "$SERIAL_OUT"; then
            SUCCESS=true
            break
        fi
        SEEN_LINES=$(wc -l < "$SERIAL_OUT" || echo 0)
    fi
    sleep 1
done

echo "[ci] Stopping QEMU..."
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

if [ "$SUCCESS" = true ]; then
    echo "[ci] PASS: Boot markers found in $SERIAL_OUT"
    exit 0
else
    echo "[ci] FAIL: Boot markers not found within ${TIMEOUT}s"
    echo "[ci] Last $SEEN_LINES lines of serial output:"
    tail -n 200 "$SERIAL_OUT" || true
    exit 1
fi
