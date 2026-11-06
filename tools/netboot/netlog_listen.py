#!/usr/bin/env python3
"""Receives the Minimal-OS network log (kernel argument netlog=<ip>:<port>).

Prints every line as it arrives and appends it to a log file, one file per
run of 'make netboot':

    tools/netboot/netlog_listen.py [--port 5140] [--dir build/netboot/logs]
"""
import argparse
import datetime
import os
import socket
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5140)
    ap.add_argument("--dir", default="build/netboot/logs")
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(args.dir, f"netlog-{stamp}.log")
    latest = os.path.join(args.dir, "latest.log")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    print(f"netlog: listening on udp {args.port}, saving to {path}", flush=True)

    pending = {}            # sender -> unfinished last line
    with open(path, "a", encoding="utf-8", errors="replace") as out:
        try:
            if os.path.islink(latest) or os.path.exists(latest):
                os.remove(latest)
            os.symlink(os.path.basename(path), latest)
        except OSError:
            pass
        while True:
            data, (host, _port) = sock.recvfrom(65535)
            text = pending.pop(host, "") + data.decode("utf-8", errors="replace")
            lines = text.split("\n")
            if lines[-1]:
                pending[host] = lines[-1]
            for line in lines[:-1]:
                now = datetime.datetime.now().strftime("%H:%M:%S")
                out.write(f"{now} {host} {line}\n")
                sys.stdout.write(f"[{host}] {line}\n")
            out.flush()
            sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
