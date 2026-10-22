"""Talks to a VM's COM port: logs everything the guest prints and types
terminal commands into it.

    python vmserial.py \\\\.\\pipe\\NAME-com1 serial.log --seconds 60 --commands gpu hyperv
    python3 vmserial.py unix:build/hyperv/com1.sock serial.log --commands hyperv

The port is a Hyper-V named pipe on Windows (mosvm.ps1), or a QEMU serial
socket on Linux (mosvm.sh): "unix:PATH" or "tcp:HOST:PORT".

Commands are sent once the guest has been quiet for a moment (the
terminal is waiting), one at a time, each after the previous one's output
has settled.
"""
import argparse
import ctypes
import os
import socket
import sys
import time


class SocketPort:
    """A QEMU serial port exposed as a socket (Linux)."""

    def __init__(self, spec, deadline):
        kind, _, rest = spec.partition(":")
        while True:
            try:
                if kind == "unix":
                    self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    self.sock.connect(rest)
                else:
                    host, _, port = rest.rpartition(":")
                    self.sock = socket.create_connection((host or "127.0.0.1", int(port)))
                break
            except OSError as error:
                if time.time() > deadline:
                    sys.exit(f"could not open {spec} ({error})")
                time.sleep(0.2)
        self.sock.setblocking(False)
        self.closed = False
        self.buffer = b""

    def available(self):
        if not self.buffer and not self.closed:
            try:
                data = self.sock.recv(65536)
                if data:
                    self.buffer = data
                else:
                    self.closed = True
            except BlockingIOError:
                pass
            except OSError:
                self.closed = True
        if self.closed and not self.buffer:
            return -1                               # socket closed: VM stopped
        return len(self.buffer)

    def read(self, n):
        data, self.buffer = self.buffer[:n], self.buffer[n:]
        return data

    def write(self, data):
        self.sock.setblocking(True)
        try:
            self.sock.sendall(data)
        except OSError:
            self.closed = True
        self.sock.setblocking(False)

    def close(self):
        self.sock.close()


def open_port(spec, deadline):
    if spec.startswith(("unix:", "tcp:")):
        return SocketPort(spec, deadline)
    if os.name != "nt":
        sys.exit("named pipes only exist on Windows; use unix:PATH or tcp:HOST:PORT")
    return PipePort(spec, deadline)


class PipePort:
    """A Hyper-V COM port named pipe (Windows)."""

    def __init__(self, path, deadline):
        self.h = open_pipe(path, deadline)

    def available(self):
        return available(self.h)

    def read(self, n):
        return read(self.h, n)

    def write(self, data):
        write(self.h, data)

    def close(self):
        k32.CloseHandle(self.h)


if os.name == "nt":
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
    ap.add_argument("pipe", help="\\\\.\\pipe\\NAME, unix:PATH or tcp:HOST:PORT")
    ap.add_argument("log")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--commands", nargs="*", default=[])
    ap.add_argument("--quiet-before-command", type=float, default=4.0)
    args = ap.parse_args()

    deadline = time.time() + args.seconds
    port = open_port(args.pipe, deadline)
    pending = list(args.commands)
    last_rx = time.time()
    seen_output = False
    with open(args.log, "wb") as out:
        while time.time() < deadline:
            n = port.available()
            if n < 0:
                break
            if n:
                data = port.read(min(n, 65536))
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
                    port.write(ch.encode())
                    time.sleep(0.02)
                last_rx = time.time()
                continue
            time.sleep(0.05)
    port.close()


if __name__ == "__main__":
    main()
