#!/usr/bin/env python3
"""Reads the 'gsplog' hex lines from a Minimal-OS network log and shows
which values point at text inside the GSP-RM firmware image.

GSP-RM's LibOS log entries are binary. Their format strings sit in a
debug file NVIDIA does not ship, but string arguments (file names,
function names, messages) point into the image's read-only data, so
looking those up often shows where GSP-RM stopped.

    tools/gpu/gsp_log_strings.py build/netboot/logs/latest.log \
        [firmware/nvidia/tu106/gsp/gsp-535.113.01.bin]
"""
import re
import struct
import sys


def sections(elf):
    """(address, size, file offset) of every section with data in the file."""
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x3A)
    out = []
    for i in range(shnum):
        base = shoff + i * shentsize
        sh_type, = struct.unpack_from("<I", elf, base + 4)
        addr, off, size = struct.unpack_from("<QQQ", elf, base + 16)
        if sh_type == 1 and addr and size:          # PROGBITS
            out.append((addr, size, off))
    return out


def inner_image(path):
    """The GSP-RM ELF inside the .fwimage section of the firmware file."""
    data = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    str_off, = struct.unpack_from("<Q", data, shoff + shstrndx * shentsize + 24)
    for i in range(shnum):
        base = shoff + i * shentsize
        name_off, = struct.unpack_from("<I", data, base)
        off, size = struct.unpack_from("<QQ", data, base + 24)
        name = data[str_off + name_off:data.index(b"\0", str_off + name_off)]
        if name == b".fwimage":
            return data[off:off + size]
    raise SystemExit("no .fwimage section in " + path)


def named_sections(elf):
    """(address, size, name) of every allocated section, with or without data."""
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    str_off, = struct.unpack_from("<Q", elf, shoff + shstrndx * shentsize + 24)
    out = []
    for i in range(shnum):
        base = shoff + i * shentsize
        name_off, = struct.unpack_from("<I", elf, base)
        addr, = struct.unpack_from("<Q", elf, base + 16)
        size, = struct.unpack_from("<Q", elf, base + 32)
        name = elf[str_off + name_off:elf.index(b"\0", str_off + name_off)].decode()
        if addr and size:
            out.append((addr, size, name))
    return out


def section_name(value, named):
    for addr, size, name in named:
        if addr <= value < addr + size:
            return name
    return None


def string_at(elf, secs, value):
    for addr, size, off in secs:
        if addr <= value < addr + size:
            start = off + (value - addr)
            end = elf.find(b"\0", start, start + 200)
            if end <= start:
                return None
            text = elf[start:end]
            if len(text) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in text):
                return text.decode()
    return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    log = sys.argv[1]
    fw = sys.argv[2] if len(sys.argv) > 2 else "firmware/nvidia/tu106/gsp/gsp-535.113.01.bin"
    elf = inner_image(fw)
    secs = sections(elf)
    secs_named = named_sections(elf)
    found = 0
    pattern = re.compile(r"gsplog (\w+)\s+(\d+):((?:\s+[0-9a-fA-F]{16})+)")
    for line in open(log, encoding="utf-8", errors="replace"):
        m = pattern.search(line)
        if not m:
            continue
        name, index = m.group(1), int(m.group(2))
        for k, word in enumerate(m.group(3).split()):
            value = int(word, 16)
            text = string_at(elf, secs, value)
            where = section_name(value, secs_named)
            note = f'  -> "{text}"' if text else (f"  (in {where})" if where else "")
            print(f"{name} {index + k:4}: {value:016x}{note}")
            found += 1
    if not found:
        print("no 'gsplog' lines in", log, "- run 'root gpu nvidia boot' on a netbooted PC first")


if __name__ == "__main__":
    main()
