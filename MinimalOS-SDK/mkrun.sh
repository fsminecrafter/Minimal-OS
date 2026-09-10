#!/bin/bash
# Usage: mkrun.sh <main.elf> <output-name.run>
set -e
ELF="$1"
OUT="$2"
if [ -z "$ELF" ] || [ -z "$OUT" ]; then
    echo "Usage: mkrun.sh <main.elf> <output-name.run>"
    exit 1
fi
mkdir -p "$OUT/Resources"
cp "$ELF" "$OUT/main.elf"
echo "Created $OUT - copy its contents onto a MinimaFS image with"
echo "tools/minimafs-explorer/mfs-explorer.py (New File -> paste bytes),"
echo "or a future CLI injector."