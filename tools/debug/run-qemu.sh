#!/bin/bash
set -e

KERNEL_ELF="kernel.bin"
KERNEL_ISO="kernel.iso"
SYMBOL="start"
PORT=1234

# Extract symbol address
ADDR_HEX=$(nm -n "$KERNEL_ELF" | grep " $SYMBOL" | awk '{print $1}')
ADDR=0x$ADDR_HEX

echo "[INFO] $SYMBOL located at $ADDR"

# Write GDB script
cat <<EOF > /tmp/gdb_startup.gdb
file $KERNEL_ELF
target remote :$PORT
b *$ADDR
commands
  echo \n[🛑 Breakpoint hit at $SYMBOL ($ADDR)]\n
  source tools/trace_fast.py
  tracefast
end
c
EOF

# Start QEMU (paused, waiting for GDB)
echo "[INFO] Starting QEMU..."
qemu-system-x86_64 \
  -cdrom "$KERNEL_ISO" \
  -S -gdb tcp::$PORT \
  -m 512M \
  -serial stdio \
  -no-reboot \
  -no-shutdown &

QEMU_PID=$!
sleep 1

# Start GDB
echo "[INFO] Launching GDB..."
gdb -x /tmp/gdb_startup.gdb

kill $QEMU_PID
