#!/usr/bin/env bash
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN=${QEMU_BIN:-qemu-system-x86_64}
BASE_DISK=${BASE_DISK:-"$WORKDIR/sata256.img"}
SERIAL_A=${SERIAL_A:-5566}
SERIAL_B=${SERIAL_B:-5567}
DLR_MCAST=${DLR_MCAST:-230.0.0.1:12345}
TIMEOUT=${TIMEOUT:-120}
OUT_A="$WORKDIR/dlr_mos_server_serial.log"
OUT_B="$WORKDIR/dlr_mos_client_serial.log"
PIDS=()
FD_A=8
FD_B=9

cleanup() {
    set +e
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    exec 8>&- 9>&-
    rm -f "$WORKDIR/mos-dlr-server.img" "$WORKDIR/mos-dlr-client.img"
}
trap cleanup EXIT

make -C "$WORKDIR" build-x86_64 >/dev/null
cp "$BASE_DISK" "$WORKDIR/mos-dlr-server.img"
cp "$BASE_DISK" "$WORKDIR/mos-dlr-client.img"
rm -f "$OUT_A" "$OUT_B"

boot() {
    local serial=$1 drive=$2 out=$3 net=$4 mac=$5
    "$QEMU_BIN" -cdrom "$WORKDIR/dist/x86_64/kernel.iso" -m 512M -display none -monitor none \
        -serial "tcp:127.0.0.1:${serial},server=on,wait=off" -no-reboot -no-shutdown \
        -device ahci,id=ahci -drive "id=disk0,file=${drive},if=none,format=raw" \
        -device ide-hd,drive=disk0,bus=ahci.0 \
        -netdev "socket,id=${net},mcast=${DLR_MCAST}" -device "rtl8139,netdev=${net},mac=${mac}" >"$out" 2>&1 &
    PIDS+=("$!")
}

wait_serial() {
    local serial=$1 fd=$2 out=$3
    for _ in $(seq 1 "$TIMEOUT"); do
        if nc -z 127.0.0.1 "$serial" 2>/dev/null; then
            eval "exec ${fd}<>/dev/tcp/127.0.0.1/${serial}"
            eval "cat <&${fd} >>\"${out}\" &"
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for() {
    local out=$1 pattern=$2
    for _ in $(seq 1 "$TIMEOUT"); do
        grep -Eq "$pattern" "$out" 2>/dev/null && return 0
        grep -Eq 'PAGE FAULT|GENERAL PROTECTION|UNHANDLED CPU EXCEPTION' "$out" 2>/dev/null && return 1
        sleep 1
    done
    return 1
}

send() { sleep 1; printf '%s\r' "$2" >&"$1"; }
wait_client_command() {
    wait_for "$OUT_B" '\[PROC\] Exiting process: 0:/programs/dlr\.run@'
    sleep 3
}
prepare_guest() {
    local fd=$1 out=$2
    wait_for "$out" '=== TERMINAL READY ==='
    wait_for "$out" 'Found 1 SATA drive\(s\)' || return 1
    sleep 1
    send "$fd" n
    for _ in 1 2 3 4 5; do
        send "$fd" format
        if wait_for "$out" 'Format succeeded\. Calling minimafs_mount\.\.\.'; then
            wait_for "$out" '\[PROC\] Exiting process: format@'
            return 0
        fi
        wait_for "$out" '\[PROC\] Exiting process: format@' || true
    done
    return 1
}

boot "$SERIAL_A" "$WORKDIR/mos-dlr-server.img" "$OUT_A" mosserver 52:54:00:12:34:56
wait_serial "$SERIAL_A" "$FD_A" "$OUT_A"
prepare_guest "$FD_A" "$OUT_A"

# Start the second AHCI guest only after the first guest has opened its
# terminal and formatted its disk. This avoids a QEMU/kernel device-
# enumeration race that can leave one VM with an uninitialized AHCI
# controller.
client_ready=0
for attempt in 1 2 3; do
    attempt_out="$WORKDIR/dlr_mos_client_serial.$attempt.log"
    rm -f "$attempt_out"
    boot "$SERIAL_B" "$WORKDIR/mos-dlr-client.img" "$attempt_out" mosclient 52:54:00:12:34:57
    client_pid=${PIDS[${#PIDS[@]}-1]}
    if wait_serial "$SERIAL_B" "$FD_B" "$attempt_out" &&
       wait_for "$attempt_out" '=== TERMINAL READY ==='; then
        OUT_B="$attempt_out"
        prepare_guest "$FD_B" "$OUT_B"
        client_ready=1
        break
    fi
    kill "$client_pid" 2>/dev/null || true
    wait "$client_pid" 2>/dev/null || true
    exec 9>&-
done
if [[ "$client_ready" != 1 ]]; then
    echo '[dlr-mos-test] FAIL: client VM did not reach terminal ready' >&2
    tail -n 120 "$attempt_out" >&2
    exit 1
fi

send "$FD_A" 'mdr ./pkg'
wait_for "$OUT_A" '\[PROC\] Exiting process: mdr@'
send "$FD_A" 'cd ./pkg'
wait_for "$OUT_A" '\[PROC\] Exiting process: cd@'
send "$FD_A" 'insert hello.txt "Hello from MOS server!"'
wait_for "$OUT_A" '\[PROC\] Exiting process: insert@'
send "$FD_A" 'dlr createpkg'
wait_for "$OUT_A" 'Created package manifest ./example.pkg'
wait_for "$OUT_A" '\[PROC\] Exiting process: 0:/programs/dlr\.run@'
send "$FD_A" 'cd ..'
wait_for "$OUT_A" '\[PROC\] Exiting process: cd@'
send "$FD_A" 'dlr present ./pkg'
wait_for "$OUT_A" 'Presented\. Clients can now install it'
send "$FD_A" 'dlr serve --name mos-server --port 4242'
wait_for "$OUT_A" "serving 'mos-server' on port 4242"

echo '[dlr-mos-test] waiting 20 seconds for the server broadcast loop'
sleep 20
echo '[dlr-mos-test] client scan'
send "$FD_B" 'dlr scan'
if ! wait_for "$OUT_B" "found '"; then
    echo '[dlr-mos-test] FAIL: client scan found no DLR server' >&2
    tail -n 120 "$OUT_B" >&2
    exit 1
fi
wait_client_command

echo '[dlr-mos-test] client list'
send "$FD_B" 'dlr list'
wait_for "$OUT_B" "Packages on 'mos-server'"
wait_for "$OUT_B" 'example.*1\.0\.0'
wait_client_command

echo '[dlr-mos-test] client search'
send "$FD_B" 'dlr search "example"'
wait_for "$OUT_B" "Results for 'example'"
wait_for "$OUT_B" 'example.*1\.0\.0'
wait_client_command

echo '[dlr-mos-test] client install'
send "$FD_B" 'dlr install example'
wait_for "$OUT_B" 'Download verified \(SHA-256 matches\)'
wait_for "$OUT_B" 'Done\.'
wait_client_command

echo '[dlr-mos-test] client download'
send "$FD_B" 'dlr download example'
wait_for "$OUT_B" 'Download verified \(SHA-256 matches\)'
wait_for "$OUT_B" 'Saved .*example\.mpkg'

echo '[dlr-mos-test] PASS: present, scan, list, search, install, and download'