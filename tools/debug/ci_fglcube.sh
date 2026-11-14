#!/usr/bin/env bash
# Boots the kernel ISO headless in QEMU (RamFS mode) twice, once with the binned
# CPU rasterizer (the default) and once with the previous immediate one (the
# 'fgpuold' boot flag, GRUB entry "Minimal-OS (RamFS, fgpuold)"), runs the FarcadeGL
# demo `fglcube --still` from the terminal over serial, takes a screenshot with the
# QEMU monitor (screendump), converts it to PNG and compares the two pictures.
#
#   bash tools/debug/ci_fglcube.sh             builds the kernel first
#   NO_BUILD=1 bash tools/debug/ci_fglcube.sh  uses the existing dist/x86_64/kernel.iso
#
# Exit 0: both pictures show a cube and they match (see MAX_DIFF_PPM), 1: they differ
# or no cube was drawn, 2: QEMU/boot problems or timeout.
#
# Environment
#  * FGLCUBE_ARGS   extra fglcube arguments, e.g. "--shader" or "--gouraud" (default none)
#  * OUT_DIR        where the screenshots go (default dist/ci_fglcube)
#  * MAX_DIFF_PPM   allowed share of differing pixels in parts per million (default 2000).
#                   The two rasterizers agree exactly on everything except the 1/256 pixel
#                   snapping of triangle edges of a perspective picture.
#  * SCALE_CHECK   N (2 or 3): with FGLCUBE_ARGS="--scale N", also check that the screenshot
#                   is built of aligned NxN blocks of one color (nearest-neighbour upscale)
#  * ONLY           "new" or "old": run just that mode (no comparison)
#  * QEMU_EXTRA, QEMU_BIN, QEMU_MEM  as in ci_os0.sh
# One QEMU runs at a time. The "old" run picks the GRUB entry two above the last one
# (GRUB_UP=2: ci_os0.sh's GRUB_UP=1 is still the noavx entry).
set -euo pipefail

WORKDIR=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_EXTRA="${QEMU_EXTRA:-}"
OUT_DIR="${OUT_DIR:-$WORKDIR/dist/ci_fglcube}"
FGLCUBE_ARGS="${FGLCUBE_ARGS:-}"
MAX_DIFF_PPM="${MAX_DIFF_PPM:-2000}"
ISO="$WORKDIR/dist/x86_64/kernel.iso"
BOOT_TIMEOUT=120
READY_TIMEOUT=${READY_TIMEOUT:-600}
TEST_CMD="run 0:/programs/fglcube.run 600 --still $FGLCUBE_ARGS"

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

if [ -z "${NO_BUILD:-}" ]; then
    echo "[ci-fglcube] Building kernel..."
    make -C "$WORKDIR" build-x86_64
fi
mkdir -p "$OUT_DIR"

ACCEL=()
if [ -w /dev/kvm ]; then ACCEL=(-accel kvm); echo "[ci-fglcube] KVM available"; else echo "[ci-fglcube] TCG (slow)"; fi

# run_one <label> <grub_up> <expected rasterizer line>
run_one() {
    local label="$1" grub_up="$2" expect="$3"
    local serial="$OUT_DIR/serial_$label.log" ppm="$OUT_DIR/$label.ppm"
    rm -f "$serial" "$ppm"
    local d="$TMP/$label"
    mkdir -p "$d"
    mkfifo "$d/ser.in" "$d/ser.out" "$d/mon.in" "$d/mon.out"
    cat "$d/ser.out" >/dev/null & BG_PIDS="$BG_PIDS $!"
    cat "$d/mon.out" >/dev/null & BG_PIDS="$BG_PIDS $!"

    echo "[ci-fglcube] ($label) starting QEMU, GRUB_UP=$grub_up"
    "$QEMU_BIN" "${ACCEL[@]}" \
        -smp 2 -m "${QEMU_MEM:-1024M}" -cdrom "$ISO" -boot d -display none \
        -chardev "pipe,id=ser0,path=$d/ser,logfile=$serial" -serial chardev:ser0 \
        -chardev "pipe,id=mon0,path=$d/mon" -mon chardev=mon0,mode=readline \
        -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0 \
        -no-reboot -no-shutdown $QEMU_EXTRA &
    QEMU_PID=$!

    local i t
    monitor_cmd() { printf '%s\n' "$1" > "$d/mon.in"; }
    qemu_alive() { kill -0 "$QEMU_PID" 2>/dev/null; }
    serial_type() {
        local line="$1" ch
        for ((i = 0; i < ${#line}; i++)); do ch="${line:$i:1}"; printf '%s' "$ch" > "$d/ser.in"; sleep 0.05; done
        printf '\r' > "$d/ser.in"
    }
    fail() { echo "[ci-fglcube] ($label) FAIL: $1"; tail -n 60 "$serial" 2>/dev/null || true; exit 2; }

    sleep 2
    for _ in 1 2 3 4 5 6 7 8; do monitor_cmd "sendkey down"; sleep 0.4; done
    for ((i = 0; i < grub_up; i++)); do monitor_cmd "sendkey up"; sleep 0.4; done
    monitor_cmd "sendkey ret"

    local ready=""
    for ((t = 0; t < BOOT_TIMEOUT; t++)); do
        if grep -aqF "=== TERMINAL READY ===" "$serial" 2>/dev/null; then ready=1; break; fi
        qemu_alive || fail "QEMU exited during boot"
        sleep 1
    done
    [ -n "$ready" ] || fail "terminal not ready within ${BOOT_TIMEOUT}s"
    grep -aqF "$expect" "$serial" || fail "boot log has no '$expect' (wrong GRUB entry?)"
    sleep 3

    echo "[ci-fglcube] ($label) typing: $TEST_CMD"
    serial_type "$TEST_CMD"
    if ! ( for ((t = 0; t < 20; t++)); do grep -aqF "Command ready: '$TEST_CMD'" "$serial" && exit 0; sleep 1; done; exit 1 ); then
        serial_type "$TEST_CMD"
    fi

    local shown=""
    for ((t = 0; t < READY_TIMEOUT; t++)); do
        if grep -aqF "FGLCUBE READY" "$serial" 2>/dev/null; then shown=1; break; fi
        if grep -aqE "fglcube: (screen is busy|could not|this kernel)|GPU error" "$serial" 2>/dev/null; then fail "fglcube reported an error"; fi
        qemu_alive || fail "QEMU exited while fglcube ran"
        sleep 1
    done
    [ -n "$shown" ] || fail "no FGLCUBE READY within ${READY_TIMEOUT}s"
    sleep 3                      # more frames: the picture is the same every frame
    monitor_cmd "screendump $ppm"
    for ((t = 0; t < 30; t++)); do [ -s "$ppm" ] && break; sleep 1; done
    sleep 1
    [ -s "$ppm" ] || fail "screendump produced nothing"
    echo "[ci-fglcube] ($label) screenshot: $ppm"
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
}

if [ "${ONLY:-}" != "old" ]; then
    run_one new 0 "fgpu: CPU rasterizer: binned"
fi
if [ "${ONLY:-}" != "new" ]; then
    run_one old 2 "fgpu: CPU rasterizer: immediate (fgpuold)"
fi

# PNG conversion, cube check and comparison.
SCALE_CHECK="${SCALE_CHECK:-}" python3 -I - "$OUT_DIR" "$MAX_DIFF_PPM" "${ONLY:-}" <<'EOF'
import sys, zlib, struct, os
out, max_ppm, only = sys.argv[1], int(sys.argv[2]), sys.argv[3]
scale_check = int(os.environ.get('SCALE_CHECK') or 0)

def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(None, 4)
    assert parts[0] == b'P6'
    w, h = int(parts[1]), int(parts[2])
    pix = data[len(data) - w * h * 3:]
    return w, h, pix

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, body):
        c = struct.pack('>I', len(body)) + tag + body
        return c + struct.pack('>I', zlib.crc32(tag + body) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) \
          + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

imgs = {}
for label in ('new', 'old'):
    p = os.path.join(out, label + '.ppm')
    # With ONLY set, a screenshot of the other mode is left from an earlier run.
    if only and label != only:
        continue
    if os.path.exists(p):
        w, h, pix = read_ppm(p)
        write_png(os.path.join(out, label + '.png'), w, h, pix)
        imgs[label] = (w, h, pix)
        # The cube scene has a dark background; count pixels that are not near it.
        bg = pix[0:3]
        n = sum(1 for i in range(0, len(pix), 3) if abs(pix[i] - bg[0]) + abs(pix[i+1] - bg[1]) + abs(pix[i+2] - bg[2]) > 24)
        frac = n / (w * h)
        print(f"{label}: {w}x{h}, {100*frac:.1f}% of the screen differs from the background, PNG {os.path.join(out, label + '.png')}")
        if frac < 0.02:
            print(f"FAIL: {label} picture shows (almost) nothing")
            sys.exit(1)
        if scale_check:
            k = scale_check
            bad = tot = 0
            for by in range(0, h - k + 1, k):
                for bx in range(0, w - k + 1, k):
                    ref = pix[(by * w + bx) * 3:(by * w + bx) * 3 + 3]
                    tot += 1
                    if any(pix[((by + dy) * w + bx + dx) * 3:((by + dy) * w + bx + dx) * 3 + 3] != ref
                           for dy in range(k) for dx in range(k)):
                        bad += 1
            print(f"{label}: {bad} of {tot} {k}x{k} blocks are not one color")
            if bad:
                print("FAIL: picture is not a clean %dx upscale" % k)
                sys.exit(1)
if len(imgs) == 2:
    (w, h, a), (w2, h2, b) = imgs['new'], imgs['old']
    if (w, h) != (w2, h2):
        print("FAIL: picture sizes differ"); sys.exit(1)
    diff = 0
    worst = 0
    for i in range(0, len(a), 3):
        if a[i:i+3] != b[i:i+3]:
            diff += 1
            worst = max(worst, abs(a[i]-b[i]), abs(a[i+1]-b[i+1]), abs(a[i+2]-b[i+2]))
    ppm = diff * 1000000 // (w * h)
    print(f"compare: {diff} of {w*h} pixels differ ({ppm} ppm, largest channel difference {worst}); allowed {max_ppm} ppm")
    if ppm > max_ppm:
        print("FAIL: binned and immediate rendering differ")
        sys.exit(1)
print("[ci-fglcube] PASS")
EOF
