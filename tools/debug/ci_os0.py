#!/usr/bin/env python3
"""Boots the kernel ISO headless in QEMU (RamFS mode), runs a program from
the terminal and waits for its result line on the serial port.

Used by `make ci-os0` (tools/debug/ci_os0.sh) for the C-library syscall
self-test: `run 0:/programs/os0test.run` prints "OS0TEST PASS" or
"OS0TEST FAIL <n>" to the serial log.

How it works
  * QEMU's serial port and monitor are TCP sockets on 127.0.0.1, so the same
    script works with QEMU on Linux and on Windows (no FIFOs).
  * The GRUB menu is driven through the monitor ("sendkey"): 'down' until the
    last entry, "Minimal-OS (RamFS)", then Enter. The menu does not wrap, so
    extra presses stay on the last entry. In RamFS mode 0: is a RAM disk
    filled from the package on the ISO; no disk image is needed.
  * The command is typed over the serial port: the terminal feeds serial
    input through its normal keyboard path (terminal_poll_serial_input).

Exit code: 0 PASS, 1 FAIL, 2 timeout or QEMU trouble.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

READY_MARKERS = (b"=== TERMINAL READY ===", b"mosWM detected; skipping terminal UI")
RAMFS_MARKER = b"RamFS mode"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def connect(port, deadline):
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=2)
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


class Serial:
    """Collects everything the guest writes; also written to the log file."""

    def __init__(self, sock, log_path):
        self.sock = sock
        self.data = bytearray()
        self.lock = threading.Lock()
        self.log = open(log_path, "wb")
        self.closed = False
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        self.sock.settimeout(None)
        while True:
            try:
                chunk = self.sock.recv(4096)
            except OSError:
                chunk = b""
            if not chunk:
                self.closed = True
                return
            with self.lock:
                self.data += chunk
            self.log.write(chunk)
            self.log.flush()

    def has(self, needle, start=0):
        with self.lock:
            return self.data.find(needle, start) >= 0

    def size(self):
        with self.lock:
            return len(self.data)

    def tail(self, lines=80):
        with self.lock:
            text = self.data.decode("latin-1")
        return "\n".join(text.splitlines()[-lines:])

    def type_line(self, line):
        for ch in line:
            self.sock.sendall(ch.encode("ascii"))
            time.sleep(0.05)
        self.sock.sendall(b"\r")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso", default=os.path.join(root, "dist", "x86_64", "kernel.iso"))
    ap.add_argument("--qemu", default=os.environ.get("QEMU_BIN", "qemu-system-x86_64"))
    ap.add_argument("--accel", default=os.environ.get("QEMU_ACCEL", ""),
                    help="QEMU accelerator (default: kvm when /dev/kvm is usable, else tcg)")
    ap.add_argument("--smp", default="2")
    ap.add_argument("--mem", default="1024M")
    ap.add_argument("--cmd", default="run 0:/programs/os0test.run")
    ap.add_argument("--pass-marker", default="OS0TEST PASS")
    ap.add_argument("--fail-marker", default="OS0TEST FAIL")
    ap.add_argument("--log", default=os.path.join(root, "ci_os0_serial.log"))
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--test-timeout", type=int, default=300)
    args = ap.parse_args()

    if not os.path.isfile(args.iso):
        print(f"[ci-os0] no ISO at {args.iso} (run make build-x86_64 first)")
        return 2
    qemu = shutil.which(args.qemu) or args.qemu
    accel = args.accel or ("kvm" if os.access("/dev/kvm", os.W_OK) else "tcg")

    serial_port, monitor_port = free_port(), free_port()
    cmd = [
        qemu, "-accel", accel, "-smp", args.smp, "-m", args.mem,
        "-cdrom", args.iso, "-boot", "d", "-display", "none",
        "-serial", f"tcp:127.0.0.1:{serial_port},server=on,wait=off",
        "-monitor", f"tcp:127.0.0.1:{monitor_port},server=on,wait=off",
        "-no-reboot",
        "-S",                       # start stopped: nothing is lost before we listen
    ]
    print("[ci-os0] " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
    result = 2
    try:
        deadline = time.time() + 20
        monitor = connect(monitor_port, deadline)
        serial = Serial(connect(serial_port, deadline), args.log)

        def mon(line):
            monitor.sendall((line + "\n").encode("ascii"))

        def alive():
            return proc.poll() is None and not serial.closed

        def wait_for(needles, seconds, start=0):
            end = time.time() + seconds
            while time.time() < end:
                for n in needles:
                    if serial.has(n, start):
                        return n
                if not alive():
                    return None
                time.sleep(0.25)
            return None

        mon("cont")

        # GRUB: its 3 s countdown stops at the first key. Keys sent before the
        # menu is up are lost, so keep pressing 'down' for a while.
        end = time.time() + 8
        while time.time() < end and serial.size() == 0:
            mon("sendkey down")
            time.sleep(0.25)
        for _ in range(6):
            mon("sendkey down")
            time.sleep(0.2)
        mon("sendkey ret")

        print(f"[ci-os0] waiting for the terminal (up to {args.boot_timeout}s, accel={accel})")
        if not wait_for(READY_MARKERS, args.boot_timeout):
            print("[ci-os0] FAIL: the terminal did not come up")
            print(serial.tail())
            return 2
        if not serial.has(RAMFS_MARKER):
            print("[ci-os0] FAIL: not booted in RamFS mode (the GRUB selection missed)")
            print(serial.tail())
            return 2

        time.sleep(3)               # let terminal_update start polling the serial port
        mark = serial.size()
        print(f"[ci-os0] typing: {args.cmd}")
        serial.type_line(args.cmd)

        found = wait_for((args.pass_marker.encode(), args.fail_marker.encode()),
                         args.test_timeout, mark)
        if found == args.pass_marker.encode():
            print("[ci-os0] PASS")
            result = 0
        elif found:
            print("[ci-os0] FAIL: the program reported a failure")
            print(serial.tail(120))
            result = 1
        else:
            print(f"[ci-os0] FAIL: no result within {args.test_timeout}s")
            print(serial.tail(120))
            result = 2
        try:
            mon("quit")
        except OSError:
            pass
    except OSError as e:
        print(f"[ci-os0] FAIL: {e}")
    finally:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    return result


if __name__ == "__main__":
    sys.exit(main())
