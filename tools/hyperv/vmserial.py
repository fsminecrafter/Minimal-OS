"""Talks to a Hyper-V VM's COM port (a named pipe): logs everything the
guest prints and types terminal commands into it.

    python vmserial.py \\\\.\\pipe\\NAME-com1 serial.log --seconds 60 --commands gpu hyperv

Commands are sent once the guest has been quiet for a moment (the
terminal is waiting), one at a time, each after the previous one's output
has settled. Windows only (uses the pipe through kernel32).
"""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
                              ctypes.POINTER(wintypes.DWORD)]
k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
INVALID = wintypes.HANDLE(-1).value


def open_pipe(path, deadline):
    while True:
        h = k32.CreateFileW(path, 0xC0000000, 0, None, 3, 0, None)  # read|write, OPEN_EXISTING
        if h and h != INVALID:
            return h
        if time.time() > deadline:
            sys.exit(f"could not open {path} (error {ctypes.get_last_error()})")
        time.sleep(0.2)


def available(h):
    n = wintypes.DWORD(0)
    if not k32.PeekNamedPipe(h, None, 0, None, ctypes.byref(n), None):
        return -1                                   # pipe closed: VM stopped
    return n.value


def read(h, n):
    buf = ctypes.create_string_buffer(n)
    got = wintypes.DWORD(0)
    k32.ReadFile(h, buf, n, ctypes.byref(got), None)
    return buf.raw[: got.value]


def write(h, data):
    done = wintypes.DWORD(0)
    k32.WriteFile(h, data, len(data), ctypes.byref(done), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pipe")
    ap.add_argument("log")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--commands", nargs="*", default=[])
    ap.add_argument("--quiet-before-command", type=float, default=4.0)
    args = ap.parse_args()

    deadline = time.time() + args.seconds
    h = open_pipe(args.pipe, deadline)
    pending = list(args.commands)
    last_rx = time.time()
    seen_output = False
    with open(args.log, "wb") as out:
        while time.time() < deadline:
            n = available(h)
            if n < 0:
                break
            if n:
                data = read(h, min(n, 65536))
                out.write(data)
                out.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
                last_rx = time.time()
                seen_output = True
                continue
            if pending and seen_output and time.time() - last_rx > args.quiet_before_command:
                cmd = pending.pop(0)
                out.write(f"\n[vmserial] >>> {cmd}\n".encode())
                for ch in cmd + "\r":
                    write(h, ch.encode())
                    time.sleep(0.02)
                last_rx = time.time()
                continue
            time.sleep(0.05)
    k32.CloseHandle(h)


if __name__ == "__main__":
    main()
