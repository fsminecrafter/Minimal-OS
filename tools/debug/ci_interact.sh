#!/usr/bin/env bash
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
SERIAL_OUT="$WORKDIR/ci_serial_interact.log"
QEMU_BIN="qemu-system-x86_64"
PORT=5555
TIMEOUT=90
DRIVE=0
DELAY=2
JUNIT_OUT="$WORKDIR/tools/debug/ci_interact.xml"
AUTO_MOUNT=false

# Parse CLI args
while [ $# -gt 0 ]; do
    case "$1" in
        --drive) DRIVE="$2"; shift 2;;
        --delay) DELAY="$2"; shift 2;;
        --port) PORT="$2"; shift 2;;
        --timeout) TIMEOUT="$2"; shift 2;;
        --junit) JUNIT_OUT="$2"; shift 2;;
        --auto-mount) AUTO_MOUNT=true; shift 1;;
        -h|--help) echo "Usage: $0 [--drive N] [--delay S] [--port P] [--timeout S] [--junit FILE] [--auto-mount]"; exit 0;;
        *) echo "Unknown arg: $1"; exit 2;;
    esac
done

echo "[ci-interact] Building kernel..."
make -C "$WORKDIR" build-x86_64

echo "[ci-interact] Starting QEMU (headless) on TCP serial port $PORT"
rm -f "$SERIAL_OUT"

${QEMU_BIN} -cdrom "$WORKDIR/dist/x86_64/kernel.iso" -m 512M -nographic -serial tcp::${PORT},server,nowait -no-reboot -no-shutdown &
QEMU_PID=$!
echo "[ci-interact] QEMU PID=$QEMU_PID"

echo "[ci-interact] Waiting for serial TCP port to open..."
for i in $(seq 1 $TIMEOUT); do
    if nc -z 127.0.0.1 $PORT 2>/dev/null; then
        echo "[ci-interact] serial port open"
        break
    fi
    if [ $i -eq $TIMEOUT ]; then
        echo "[ci-interact] timeout waiting for serial port"
        kill $QEMU_PID || true
        exit 2
    fi
    sleep 1
done

echo "[ci-interact] Connecting to serial port and logging output to $SERIAL_OUT"
coproc NC { nc 127.0.0.1 $PORT; }

# Reader: capture serial output
cat <&"${NC[0]}" | tee "$SERIAL_OUT" &
READER_PID=$!

# Helper to send a line
send() {
    local line="$1"
    echo "[ci-interact] SEND: $line"
    printf '%s\n' "$line" >&"${NC[1]}"
}

write_junit_header() {
    mkdir -p "$(dirname "$JUNIT_OUT")"
    cat > "$JUNIT_OUT" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<testsuites>
  <testsuite name="ci-interact" tests="6">
EOF
}

write_junit_footer() {
    cat >> "$JUNIT_OUT" <<EOF
  </testsuite>
</testsuites>
EOF
}

write_junit_case() {
    local name="$1"; local status="$2"; local msg="$3"
    if [ "$status" = "pass" ]; then
        cat >> "$JUNIT_OUT" <<EOF
    <testcase classname="ci-interact" name="$name" />
EOF
    else
        # escape XML
        msg=$(printf "%s" "$msg" | sed -e 's/&/&amp;/g' -e "s/</&lt;/g" -e "s/>/&gt;/g")
        cat >> "$JUNIT_OUT" <<EOF
    <testcase classname="ci-interact" name="$name">
      <failure>$msg</failure>
    </testcase>
EOF
    fi
}

# Wait for boot marker
echo "[ci-interact] Waiting for 'Minimal OS' boot marker..."
for i in $(seq 1 $TIMEOUT); do
    if grep -q "Minimal OS" "$SERIAL_OUT" 2>/dev/null; then
        echo "[ci-interact] Boot marker seen"
        break
    fi
    sleep 1
done

sleep 2

write_junit_header

# Helper: wait for a prompt or text
wait_for() {
    local pattern="$1"; local timeout_sec="$2"; local seen=0
    for i in $(seq 1 $timeout_sec); do
        if grep -q "$pattern" "$SERIAL_OUT" 2>/dev/null; then
            seen=1
            break
        fi
        sleep 1
    done
    return $seen
}

# 1) Answer mount prompt when it appears
echo "[ci-interact] Waiting for mount prompt or terminal ready..."
if wait_for "Mount disk?" $((TIMEOUT/2)); then
    if [ "$AUTO_MOUNT" = true ]; then
        send "y"
    else
        send "n"
    fi
    echo "[ci-interact] Replied to mount prompt"
else
    echo "[ci-interact] Mount prompt not seen; continuing"
fi

# Small pause before commands
sleep $DELAY

# Sequence of commands to run (with expected success regex)
declare -a CMDS
declare -a REGS
CMDS+=("initdisk")
REGS+=("MinimaFS device wrapper created|=== Initializing Storage System ===|Found [0-9]+ SATA drive")
CMDS+=("format")
REGS+=("Format (OK|succeeded|complete)|Format succeeded|Format complete")
CMDS+=("mount")
REGS+=("Mount succeded|Mount succeeded|Filesystem mounted")
CMDS+=("createmusicfile")
REGS+=("createmusicfile: write complete|music.adi written|Done music file created")
CMDS+=("memsize")
REGS+=("Executing\.\.\.|Converting\.\.\.")

for idx in "${!CMDS[@]}"; do
    cmd="${CMDS[$idx]}"
    pattern="${REGS[$idx]}"
    # append drive arg if supported
    fullcmd="$cmd"
    if [ "$cmd" = "initdisk" ] || [ "$cmd" = "createmusicfile" ] || [ "$cmd" = "format" ] || [ "$cmd" = "mount" ]; then
        fullcmd="$cmd $DRIVE"
    fi

    echo "[ci-interact] Sending command: $fullcmd"
    send "$fullcmd"

    # wait for success pattern up to TIMEOUT
    if wait_for "$pattern" $TIMEOUT; then
        echo "[ci-interact] Command '$cmd' detected success pattern"
        write_junit_case "$cmd" "pass" ""
    else
        echo "[ci-interact] Command '$cmd' did NOT detect pattern within timeout"
        # capture tail for failure message
        tail_out=$(tail -n 50 "$SERIAL_OUT" || true)
        write_junit_case "$cmd" "fail" "$tail_out"
    fi

    sleep $DELAY
done

write_junit_footer

echo "[ci-interact] Commands completed, gathering final output..."

echo "[ci-interact] Stopping QEMU..."
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

# Give reader time to flush
sleep 1
kill $READER_PID 2>/dev/null || true || true

echo "[ci-interact] Serial output (tail):"
tail -n 200 "$SERIAL_OUT" || true

# Basic success heuristics: look for memsize markers or createmusicfile write complete
echo "[ci-interact] JUnit results written to $JUNIT_OUT"

# Exit code: 0 if all tests passed
if grep -q "<failure>" "$JUNIT_OUT"; then
    echo "[ci-interact] Some tests failed"
    exit 1
else
    echo "[ci-interact] All tests passed"
    exit 0
fi
