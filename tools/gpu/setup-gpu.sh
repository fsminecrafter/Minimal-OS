#!/usr/bin/env bash
# Set up NVIDIA GPU firmware for Minimal-OS.
#
# Minimal-OS's NVIDIA driver needs NVIDIA's signed firmware files, the same
# ones Linux (nouveau) loads. NVIDIA publishes them for redistribution in
# linux-firmware. This tool collects the files for one card into firmware/
# (laid out as they go on the system volume). The next 'make' packs them into
# the ISO as boot/mosfirmware.mpkg, and the kernel unpacks it into
# 0:/firmware when it sets up a system volume (RamFS boot, 'format',
# 'install').
#
#   tools/gpu/setup-gpu.sh                    RTX 2060 (the default)
#   tools/gpu/setup-gpu.sh "RTX 2080 Super"   a card by name
#   tools/gpu/setup-gpu.sh tu116              a chip by name
#   tools/gpu/setup-gpu.sh 10de:1f08          a PCI id (see lspci -nn)
#   tools/gpu/setup-gpu.sh --detect           the NVIDIA card in this PC
#   tools/gpu/setup-gpu.sh --list             supported cards and chips
#   tools/gpu/setup-gpu.sh --remove           delete firmware/ (builds go
#                                             back to having no firmware)
#
# Options:
#   --source DIR    take files from DIR (a linux-firmware tree; default
#                   /lib/firmware, falling back to a download)
#   --download      always download from git.kernel.org
#   --out DIR       where to put them (default: <repo>/firmware)
#
# Several cards can be set up one after another; each chip gets its own
# folder. The files are not part of the repository (firmware/ is ignored by
# git); see firmware/nvidia/LICENCE.nvidia for NVIDIA's terms.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out="$repo/firmware"
source_dir=/lib/firmware
force_download=0
detect=0
target=""

# Must match NV_GSP_FW_VERSION in src/intf/x86_64/nv_hw.h.
fw_version=$(sed -n 's/^#define NV_GSP_FW_VERSION "\([^"]*\)".*/\1/p' \
    "$repo/src/intf/x86_64/nv_hw.h")
[ -n "$fw_version" ] || fw_version=535.113.01
upstream=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain

die() { echo "setup-gpu: $*" >&2; exit 1; }
say() { echo "setup-gpu: $*"; }

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

list_cards() {
    cat <<'EOF'
Supported (Turing, booted through GSP firmware):
  tu102   RTX 2080 Ti, TITAN RTX, Quadro RTX 6000/8000
  tu104   RTX 2080, RTX 2080 Super, RTX 2070 Super, Quadro RTX 4000/5000, Tesla T4
  tu106   RTX 2060, RTX 2060 Super, RTX 2060 12GB, RTX 2070
  tu116   GTX 1660, GTX 1660 Ti, GTX 1660 Super, GTX 1650 Super
  tu117   GTX 1650, GTX 1650 Ti

Not supported yet: GTX 10 (Pascal) and older, RTX 30 and newer.
(The kernel recognises GTX 10 cards, but only Turing firmware loading
is written.)
EOF
}

# Card name -> chip. Lowercase, spaces and dashes removed, "geforce" dropped.
chip_from_name() {
    local n
    n=$(echo "$1" | tr '[:upper:]' '[:lower:]' | sed 's/geforce//; s/nvidia//; s/[ _-]//g')
    case "$n" in
        tu102|tu104|tu106|tu116|tu117) echo "$n" ;;
        rtx2080ti|titanrtx|quadrortx6000|quadrortx8000|rtx6000|rtx8000) echo tu102 ;;
        rtx2080super|rtx2080|rtx2070super|quadrortx5000|quadrortx4000|rtx5000|rtx4000|teslat4|t4) echo tu104 ;;
        rtx2060super|rtx2060*|rtx2070) echo tu106 ;;
        gtx1660*|gtx1650super) echo tu116 ;;
        gtx1650*) echo tu117 ;;
        gt10*|gtx10*|gtx9*|gtx7*|titanx*|titanxp) echo pascal-or-older ;;
        rtx30*|rtx40*|rtx50*|rtxa*) echo newer ;;
        *) echo "" ;;
    esac
}

# PCI device id (hex, no 0x) -> chip, by NVIDIA's device id ranges.
chip_from_pci() {
    local id=$((16#$1))
    if   (( id >= 0x1e00 && id <= 0x1e7f )); then echo tu102
    elif (( id >= 0x1e80 && id <= 0x1eff )); then echo tu104
    elif (( id >= 0x1f00 && id <= 0x1f7f )); then echo tu106
    elif (( id >= 0x1f80 && id <= 0x1fff )); then echo tu117
    elif (( id >= 0x2180 && id <= 0x21ff )); then echo tu116
    elif (( id >= 0x1100 && id <= 0x1dff )); then echo pascal-or-older
    elif (( id >= 0x2200 )); then echo newer
    else echo ""
    fi
}

resolve_chip() {
    local t=$1 chip
    if [[ "$t" =~ ^(10[dD][eE]:)?([0-9a-fA-F]{4})$ ]]; then
        chip=$(chip_from_pci "${BASH_REMATCH[2]}")
    else
        chip=$(chip_from_name "$t")
    fi
    case "$chip" in
        tu1*) echo "$chip" ;;
        pascal-or-older) die "'$t' is a GTX 10 (Pascal) or older card: Minimal-OS only loads Turing (RTX 20 / GTX 16) firmware so far" ;;
        newer) die "'$t' is an RTX 30 or newer card: not supported yet (only Turing: RTX 20 / GTX 16)" ;;
        *) die "unknown card '$t' (try --list, a chip name like tu106, or a PCI id like 10de:1f08)" ;;
    esac
}

detect_chip() {
    command -v lspci >/dev/null || die "--detect needs lspci (pciutils)"
    local line id
    line=$(lspci -nn -d 10de: | grep -Ei 'vga|3d|display' | head -n 1 || true)
    [ -n "$line" ] || die "no NVIDIA graphics card in this PC; name the card instead (e.g. \"RTX 2060\")"
    say "found: ${line#*: }" >&2
    id=$(echo "$line" | sed -n 's/.*\[10de:\([0-9a-fA-F]\{4\}\)\].*/\1/p')
    resolve_chip "$id"
}

# The folder linux-firmware actually keeps a chip's GSP files in (the
# others are symbolic links to it).
upstream_gsp_dir() {
    case "$1" in
        tu102|tu104|tu106) echo tu102 ;;
        tu116|tu117) echo tu116 ;;
    esac
}

# Copy one file from a linux-firmware tree (plain, .zst or .xz).
copy_local() {
    local src=$1 dst=$2
    if [ -f "$src" ]; then cp -L "$src" "$dst"
    elif [ -f "$src.zst" ]; then
        command -v zstd >/dev/null || die "$src.zst needs zstd to unpack"
        zstd -qdf "$src.zst" -o "$dst"
    elif [ -f "$src.xz" ]; then
        command -v xz >/dev/null || die "$src.xz needs xz to unpack"
        xz -qdc "$src.xz" > "$dst"
    else
        return 1
    fi
}

download() {
    local url=$1 dst=$2
    if command -v curl >/dev/null; then
        curl -fsSL --retry 2 -o "$dst" "$url"
    elif command -v wget >/dev/null; then
        wget -q -O "$dst" "$url"
    else
        die "downloading needs curl or wget"
    fi
}

# Checks what the kernel's parser (nv_fw.c) checks first: NVIDIA's .bin
# container magic, or an ELF image for GSP-RM.
check_file() {
    local f=$1 kind=$2 magic
    [ -s "$f" ] || die "$f is empty"
    if [ "$kind" = elf ]; then
        magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
        [ "$magic" = 7f454c46 ] || die "$f is not an ELF image (bad download?)"
    else
        magic=$(head -c 2 "$f" | od -An -tx1 | tr -d ' \n')
        [ "$magic" = de10 ] || die "$f is not an NVIDIA firmware file (bad download?)"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --list) list_cards; exit 0 ;;
        --detect) detect=1 ;;
        --download) force_download=1 ;;
        --source) shift; [ $# -gt 0 ] || die "--source needs a folder"; source_dir=$1 ;;
        --out) shift; [ $# -gt 0 ] || die "--out needs a folder"; out=$1 ;;
        --remove)
            rm -rf "$out"
            say "removed $out; builds no longer include GPU firmware"
            exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) [ -z "$target" ] || target="$target "; target="$target$1" ;;
    esac
    shift
done

if [ "$detect" = 1 ]; then
    chip=$(detect_chip)
else
    chip=$(resolve_chip "${target:-RTX 2060}")
fi

# The four files nv_fw.c's manifest asks for, and their kind.
files=(booter_load:bin booter_unload:bin bootloader:bin gsp:elf)

stage=$(mktemp -d "${TMPDIR:-/tmp}/mos-gpufw.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/gsp"

say "card chip: $chip, GSP firmware $fw_version"
from=""
for entry in "${files[@]}"; do
    name=${entry%%:*} kind=${entry##*:}
    file="$name-$fw_version.bin"
    dst="$stage/gsp/$file"
    if [ "$force_download" = 0 ] &&
        copy_local "$source_dir/nvidia/$chip/gsp/$file" "$dst"; then
        from="$source_dir"
    else
        # GTX 16 cards have their own booter_* files; their boot loader and
        # GSP-RM image are links to tu102's, so fall back to tu102.
        say "downloading $file ..."
        url="$upstream/nvidia/$(upstream_gsp_dir "$chip")/gsp/$file"
        if ! download "$url" "$dst" 2>/dev/null; then
            url="$upstream/nvidia/tu102/gsp/$file"
            download "$url" "$dst" || die "could not download $file from linux-firmware"
        fi
        from="git.kernel.org linux-firmware"
    fi
    check_file "$dst" "$kind"
done

# NVIDIA's redistribution licence goes along with the files.
licence="$stage/LICENCE.nvidia"
if ! download "$upstream/LICENSES/LICENCE.nvidia" "$licence" 2>/dev/null; then
    rm -f "$licence"
    say "note: could not fetch LICENCE.nvidia (offline?); the firmware is still set up"
fi

mkdir -p "$out/nvidia"
rm -rf "$out/nvidia/$chip"
mkdir -p "$out/nvidia/$chip"
cp -a "$stage/gsp" "$out/nvidia/$chip/"
[ -f "$licence" ] && cp "$licence" "$out/nvidia/LICENCE.nvidia"

say "copied from $from into ${out#$repo/}/nvidia/$chip ($(du -sh "$out/nvidia/$chip" | cut -f1)):"
for entry in "${files[@]}"; do
    f="$out/nvidia/$chip/gsp/${entry%%:*}-$fw_version.bin"
    printf '  0:/firmware/nvidia/%s/gsp/%-32s %s bytes\n' "$chip" "$(basename "$f")" "$(stat -c %s "$f")"
done
cat <<EOF

Next:
  make mosdesk            build the ISO (with MOSdesk) including the firmware
  make run-mosdesk        or build and boot it in QEMU (QEMU has no NVIDIA card)
Then boot dist/x86_64/kernel.iso on the PC with the card (any entry; RamFS
needs no disk) and run 'gpu nvidia' to check the files and the GPU's state.
EOF
