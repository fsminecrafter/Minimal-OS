#!/usr/bin/env bash
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_PORT=${SERIAL_PORT:-5566}
TIMEOUT=${TIMEOUT:-90}
SERIAL_OUT="$WORKDIR/dlr_present_serial.log"
QEMU_BIN=${QEMU_BIN:-qemu-system-x86_64}
QEMU_PID=""

cleanup() {
    if [[ -n "$QEMU_PID" ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "[dlr-test] Building kernel image..."
make -C "$WORKDIR" build-x86_64
rm -f "$SERIAL_OUT"

"$QEMU_BIN" \
    -cdrom "$WORKDIR/dist/x86_64/kernel.iso" \
    -m 512M -nographic -serial "tcp::${SERIAL_PORT},server,nowait" \
    -no-reboot -no-shutdown \
    -device ahci,id=ahci \
    -drive id=disk0,file="$WORKDIR/sata256.img",if=none,format=raw \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    >"$SERIAL_OUT" 2>&1 &
QEMU_PID=$!
echo "[dlr-test] QEMU PID=$QEMU_PID"

for _ in $(seq 1 "$TIMEOUT"); do
    if nc -z 127.0.0.1 "$SERIAL_PORT" 2>/dev/null; then break; fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[dlr-test] FAIL: QEMU exited before opening serial port"
        tail -n 160 "$SERIAL_OUT" || true
        exit 1
    fi
    sleep 1
done
if ! nc -z 127.0.0.1 "$SERIAL_PORT" 2>/dev/null; then
    echo "[dlr-test] FAIL: serial port did not open"
    tail -n 80 "$SERIAL_OUT" || true
    exit 1
fi

coproc SERIAL { nc 127.0.0.1 "$SERIAL_PORT"; }
exec {SERIAL_READ}<&"${SERIAL[0]}"
cat <&"$SERIAL_READ" | tee -a "$SERIAL_OUT" >/dev/null &
READER_PID=$!

send() {
    printf '%s\n' "$1" >&"${SERIAL[1]}"
}

wait_for() {
    local pattern=$1
    for _ in $(seq 1 "$TIMEOUT"); do
        if grep -Eq "$pattern" "$SERIAL_OUT" 2>/dev/null; then return 0; fi
        if grep -Eq 'PAGE FAULT|ALLOC FAILED|MinimaFS: Invalid path|cannot present' "$SERIAL_OUT" 2>/dev/null; then return 1; fi
        sleep 1
    done
    return 1
}

if ! wait_for 'Minimal OS|Storage manager initialized|Terminal: Command ready' ; then
    echo "[dlr-test] FAIL: boot did not reach the terminal"
    tail -n 120 "$SERIAL_OUT" || true
    exit 1
fi

send "format"
if ! wait_for 'Format succeeded\.|Mount succeeded\.'; then
    echo "[dlr-test] FAIL: format did not complete"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi

send "mdr ./test"
if ! wait_for 'Directory created:'; then
    echo "[dlr-test] FAIL: test fixture directory was not created"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi

send "cd ./test"
if ! wait_for 'Command ready:'; then
    echo "[dlr-test] FAIL: could not enter test fixture directory"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi

send "dlr createpkg"
if ! wait_for 'Created package manifest ./example.pkg'; then
    echo "[dlr-test] FAIL: createpkg did not create the manifest"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi

send "cd .."
if ! wait_for 'Command ready:'; then
    echo "[dlr-test] FAIL: could not leave test fixture directory"
    tail -n 160 "$SERIAL_OUT" || true
    exit 1
fi

send "dlr present ./test"
if ! wait_for 'Presented\. Clients can now install it'; then
    echo "[dlr-test] FAIL: dlr present ./test did not succeed"
    tail -n 200 "$SERIAL_OUT" || true
    exit 1
fi

kill "$READER_PID" 2>/dev/null || true
echo "[dlr-test] PASS: format -> mdr ./test -> dlr present ./test"
