#!/usr/bin/env bash
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_PORT=${SERIAL_PORT:-5566}
TIMEOUT=${TIMEOUT:-90}
SERIAL_OUT="$WORKDIR/dlr_present_serial.log"
QEMU_BIN=${QEMU_BIN:-qemu-system-x86_64}
QEMU_PID=""
READER_PID=""

cleanup() {
    set +e
    [[ -n "$QEMU_PID" ]] && kill "$QEMU_PID" 2>/dev/null
    [[ -n "$READER_PID" ]] && kill "$READER_PID" 2>/dev/null
    exec 8>&- 2>/dev/null
    exec 9>&- 2>/dev/null
    [[ -n "$READER_PID" ]] && wait "$READER_PID" 2>/dev/null
    [[ -n "$QEMU_PID" ]] && wait "$QEMU_PID" 2>/dev/null
}
trap cleanup EXIT

echo "[dlr-test] Building kernel image..."
make -C "$WORKDIR" build-x86_64
rm -f "$SERIAL_OUT"

"$QEMU_BIN" \
    -cdrom "$WORKDIR/dist/x86_64/kernel.iso" \
    -m 512M -display none -monitor none \
    -serial "tcp:127.0.0.1:${SERIAL_PORT},server=on,wait=off" \
    -no-reboot -no-shutdown \
    -device ahci,id=ahci \
    -drive id=disk0,file="$WORKDIR/sata256.img",if=none,format=raw \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    >"$SERIAL_OUT" 2>&1 &
QEMU_PID=$!
echo "[dlr-test] QEMU PID=$QEMU_PID"

SERIAL_CONNECTED=false
for _ in $(seq 1 "$TIMEOUT"); do
    if exec 8<>"/dev/tcp/127.0.0.1/${SERIAL_PORT}"; then
        SERIAL_CONNECTED=true
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[dlr-test] FAIL: QEMU exited before opening serial port"
        tail -n 160 "$SERIAL_OUT" || true
        exit 1
    fi
    sleep 1
done
if [[ "$SERIAL_CONNECTED" != true ]]; then
    echo "[dlr-test] FAIL: serial port did not open"
    tail -n 80 "$SERIAL_OUT" || true
    exit 1
fi

cat <&8 >>"$SERIAL_OUT" &
READER_PID=$!

send() {
    printf '%s\r' "$1" >&8
}

wait_for() {
    local pattern=$1
    for _ in $(seq 1 "$TIMEOUT"); do
        if grep -Eq "$pattern" "$SERIAL_OUT" 2>/dev/null; then return 0; fi
        if grep -Eq 'PAGE FAULT|cannot present' "$SERIAL_OUT" 2>/dev/null; then return 1; fi
        sleep 1
    done
    return 1
}

if ! wait_for '=== TERMINAL STARTING ==='; then
    echo "[dlr-test] FAIL: terminal did not start"
    tail -n 120 "$SERIAL_OUT" || true
    exit 1
fi

send "n"
if ! wait_for '=== TERMINAL READY ==='; then
    echo "[dlr-test] FAIL: terminal did not become ready"
    tail -n 120 "$SERIAL_OUT" || true
    exit 1
fi

send "format"
if ! wait_for '\[PROC\] Exiting process: format@'; then
    echo "[dlr-test] FAIL: format did not complete"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
sleep 1

send "mdr ./test"
if ! wait_for '\[PROC\] Exiting process: mdr@'; then
    echo "[dlr-test] FAIL: test fixture directory was not created"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
sleep 1

send "cd ./test"
if ! wait_for '\[PROC\] Exiting process: cd@'; then
    echo "[dlr-test] FAIL: could not enter test fixture directory"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
sleep 1

send "dlr createpkg"
if ! wait_for 'Created package manifest ./example.pkg'; then
    echo "[dlr-test] FAIL: createpkg did not create the manifest"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
if ! wait_for '\[PROC\] Exiting process: 0:/programs/dlr\.run@'; then
    echo "[dlr-test] FAIL: createpkg process did not exit"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
sleep 1

send "cd .."
if ! wait_for "Terminal: Processing command: 'cd \.\.'"; then
    echo "[dlr-test] FAIL: cd .. was not processed"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
if ! wait_for '\[PROC\] Exiting process: cd@'; then
    echo "[dlr-test] FAIL: could not leave test fixture directory"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi
sleep 1

send "dlr present ./test"
if ! wait_for 'Presented\. Clients can now install it'; then
    echo "[dlr-test] FAIL: dlr present ./test did not succeed"
    tail -n 200 "$SERIAL_OUT" || true
    exit 1
fi

echo "[dlr-test] PASS: format -> mdr ./test -> dlr present ./test"
