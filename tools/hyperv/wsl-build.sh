#!/bin/bash
# Builds Minimal-OS from a Windows checkout inside WSL.
#
# A Windows checkout (core.autocrlf=true) has CRLF line endings, which break
# the shell and Python build scripts, and building on /mnt/* is slow. So the
# working tree is mirrored to ~/mos-build (scripts converted to LF), built
# there, and the ISO is copied back to dist/x86_64/ in the checkout.
#
#   wsl -d Ubuntu -- bash /mnt/d/Projects/Minimal-OS/tools/hyperv/wsl-build.sh [make target...]
set -euo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
dst="$HOME/mos-build"
export PATH="$HOME/cross/bin:/opt/cross/bin:$PATH"

mkdir -p "$dst"
rsync -a --delete --exclude '.git' --exclude '/build' --exclude '/dist' \
      --exclude '*.img' "$src/" "$dst/"
find "$dst" -type f \( -name '*.sh' -o -name '*.py' -o -name 'Makefile' -o -name '*.mk' \
     -o -name '*.cfg' -o -name '*.ld' \) -exec sed -i 's/\r$//' {} +

cd "$dst"
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(build-x86_64)
make -j"$(nproc)" "${targets[@]}"

mkdir -p "$src/dist/x86_64"
cp dist/x86_64/kernel.iso "$src/dist/x86_64/kernel.iso"
echo "ISO: $src/dist/x86_64/kernel.iso"
