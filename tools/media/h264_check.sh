#!/bin/sh
# Checks the H.264 decoder (Minimal-OS-SDK/include/mosmedia_h264.h) against
# ffmpeg: x264 encodes test videos with many different settings, both
# decoders decode them, and every byte of every frame must be the same
# (H.264 decoding is exact). Needs ffmpeg with libx264. Run: make test-h264
set -eu
cd "$(dirname "$0")/../.."
SDK=${SDK:-Minimal-OS-SDK/include}
OUT=build/h264_check
mkdir -p "$OUT"
${HOSTCC:-cc} -std=c11 -O2 -idirafter "$SDK" tools/media/h264_check.c -o "$OUT/h264_check"

fail=0
n=0
check() {   # name, source filter, frames, x264 parameters
    n=$((n + 1))
    f="$OUT/$1"
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$2" -frames:v "$3" -c:v libx264 \
        -profile:v baseline -pix_fmt yuv420p -x264-params "aud=1:$4" -f h264 "$f.h264"
    ffmpeg -hide_banner -loglevel error -y -i "$f.h264" -fps_mode passthrough -f rawvideo -pix_fmt yuv420p "$f.ref.yuv"
    if "$OUT/h264_check" "$f.h264" "$f.yuv" >"$f.log" 2>&1 && cmp -s "$f.yuv" "$f.ref.yuv"; then
        printf '  ok    %-28s %s, %s bytes\n' "$1" "$(cat "$f.log")" "$(wc -c <"$f.h264" | tr -d ' ')"
    else
        printf '  FAIL  %-28s %s\n' "$1" "$(tail -1 "$f.log")"
        fail=$((fail + 1))
    fi
    rm -f "$f.yuv" "$f.ref.yuv"
}

T=testsrc2=size=320x240:rate=25
M=mandelbrot=size=320x240:rate=25
L=life=size=320x240:rate=25:mold=10:ratio=0.3:death_color=#C83232:life_color=#00ff00
check basic            "$T" 40 "keyint=20"
check cropped-190x110  "testsrc2=size=190x110:rate=25" 30 "keyint=15"
check tiny-16x16       "testsrc2=size=16x16:rate=25" 20 "keyint=8"
check wide-640x48      "testsrc2=size=640x48:rate=25" 20 "keyint=8"
check all-partitions   "$M" 40 "keyint=40:partitions=all:subme=7:me=umh"
check four-references  "$M" 50 "keyint=50:ref=4:mixed-refs=1:partitions=all:subme=7"
check sixteen-refs     "$L" 60 "keyint=60:ref=16:mixed-refs=1:partitions=all"
check four-slices      "$T" 30 "keyint=10:slices=4"
check slices-refs      "$M" 30 "keyint=30:slices=3:ref=3:partitions=all"
check best-quality     "$T" 12 "keyint=6:qp=1"
check lossy-qp51       "$T" 30 "keyint=15:qp=51"
check qp-ramp          "$M" 40 "keyint=40:qpmin=10:qpmax=45:bitrate=150:vbv-maxrate=150:vbv-bufsize=30"
check no-deblock       "$T" 30 "keyint=10:no-deblock=1"
check deblock-offsets  "$M" 30 "keyint=15:deblock=-3,3"
check deblock-strong   "$L" 30 "keyint=15:deblock=6,-6:qp=40"
check constrained-intra "$M" 30 "keyint=10:constrained-intra=1"
check intra-only       "$M" 12 "keyint=1"
check noise            "nullsrc=size=160x120:rate=25,geq=random(1)*255:128:128" 10 "keyint=5:qp=4"
check noise-inter      "nullsrc=size=160x120:rate=25,geq=random(X/8+Y/8*20)*255:128+64*sin(N):128" 20 "keyint=20:qp=20"
check no-skip-fast     "$L" 40 "keyint=40:no-fast-pskip=1:no-dct-decimate=1:subme=9:trellis=2:partitions=all"
check ultrafast        "$T" 40 "keyint=20:subme=0:me=dia:partitions=none"
check long-gop         "$M" 120 "keyint=120:ref=2"
check hd-720p          "testsrc2=size=1280x720:rate=25" 8 "keyint=8"
if [ "$fail" -ne 0 ]; then echo "$fail of $n streams differ from ffmpeg"; exit 1; fi
echo "h264_check: all $n streams decode exactly as ffmpeg decodes them"
