#!/usr/bin/env bash
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN=${QEMU_BIN:-qemu-system-x86_64}
HTTPS_PORT=${HTTPS_PORT:-8443}
SERIAL_PORT=${SERIAL_PORT:-5556}
TIMEOUT=${TIMEOUT:-60}
TMPDIR=$(mktemp -d)
QEMU_PID=""
TLS_PID=""
SERIAL_READER_PID=""
SERIAL_LOG="$WORKDIR/tools/debug/https_test.log"
TLS_LOG="$WORKDIR/tools/debug/https_server.log"

cleanup() {
    set +e
    [[ -n "$QEMU_PID" ]] && kill "$QEMU_PID" 2>/dev/null
    [[ -n "$TLS_PID" ]] && kill "$TLS_PID" 2>/dev/null
    [[ -n "$SERIAL_READER_PID" ]] && kill "$SERIAL_READER_PID" 2>/dev/null
    exec 8>&- 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo "[https-test] Building kernel..."
make -C "$WORKDIR" build-x86_64 >/dev/null

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj "/CN=10.0.2.2" \
    -keyout "$TMPDIR/key.pem" -out "$TMPDIR/cert.pem" >/dev/null 2>&1
openssl s_server -trace -accept "$HTTPS_PORT" -tls1_3 \
    -cert "$TMPDIR/cert.pem" -key "$TMPDIR/key.pem" -www \
    >"$TLS_LOG" 2>&1 &
TLS_PID=$!

rm -f "$SERIAL_LOG"
"$QEMU_BIN" -cdrom "$WORKDIR/dist/x86_64/kernel.iso" -m 512M \
    -display none -monitor none \
    -serial "tcp:127.0.0.1:${SERIAL_PORT},server=on,wait=off" \
    -no-reboot -no-shutdown \
    -netdev "user,id=net0" \
    -device rtl8139,netdev=net0 -usb -device usb-kbd >/dev/null 2>&1 &
QEMU_PID=$!

sleep 1
coproc SERIAL { nc 127.0.0.1 "$SERIAL_PORT"; }
# Coprocess descriptors are not inherited by background jobs.  Duplicate
# them first so the reader and send_text retain their ends of the connection.
exec 8<&"${SERIAL[0]}"
exec 9>&"${SERIAL[1]}"
cat <&8 > "$SERIAL_LOG" &
SERIAL_READER_PID=$!
send_text() { printf '%s\r' "$1" >&9; }
wait_for() {
    local pattern="$1"
    for _ in $(seq 1 "$TIMEOUT"); do
        grep -Eq "$pattern" "$SERIAL_LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

wait_for_https_result() {
    for _ in $(seq 1 "$TIMEOUT"); do
        if grep -Eq "wget: TLS connection failed" "$SERIAL_LOG" 2>/dev/null; then
            return 1
        fi
        if grep -Eq "wget: TLS handshake completed" "$SERIAL_LOG" 2>/dev/null && \
           grep -Eq "wget: download completed" "$SERIAL_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# terminal_program_entry asks whether to mount a disk before it starts the
# command loop.  The test has no disk image, so decline that prompt over the
# serial console as soon as the terminal begins.
wait_for "=== TERMINAL STARTING ===" || { echo "[https-test] terminal did not start"; exit 1; }
send_text "n"
wait_for "=== TERMINAL READY ===" || { echo "[https-test] terminal did not become ready"; exit 1; }
send_text "dhcp"
wait_for "DHCP: bound" || { echo "[https-test] DHCP failed"; exit 1; }
sleep 1
send_text "wget https://10.0.2.2:${HTTPS_PORT}/"

if wait_for_https_result; then
    echo "[https-test] HTTPS wget passed"
    exit 0
fi

echo "[https-test] HTTPS wget failed"
tail -n 100 "$SERIAL_LOG"
echo "[https-test] OpenSSL server output:"
tail -n 100 "$TLS_LOG"
exit 1
