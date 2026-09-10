#!/usr/bin/env python3
"""Pack a .run application directory into one file.

The resulting file is imported into MinimaFS as an ordinary file.  Its
payload format is defined by src/intf/x86_64/runfile.h.
"""

import argparse
import struct
from pathlib import Path

MAGIC = b"MINIRUN1"
VERSION = 1
HEADER_SIZE = 16
ENTRY_SIZE = 72
NAME_SIZE = 64


def pack(source: Path, output: Path) -> None:
    if not source.is_dir():
        raise ValueError(f"source is not a directory: {source}")

    files = sorted(path for path in source.rglob("*") if path.is_file())
    if not any(path.relative_to(source).as_posix() == "main.elf" for path in files):
        raise ValueError(".run source must contain main.elf")

    entries = []
    payload = bytearray()
    table_size = HEADER_SIZE + len(files) * ENTRY_SIZE
    for path in files:
        name = path.relative_to(source).as_posix()
        name_bytes = name.encode("utf-8")
        if len(name_bytes) >= NAME_SIZE:
            raise ValueError(f"bundle path is too long: {name}")
        data = path.read_bytes()
        entries.append((name_bytes, table_size + len(payload), len(data)))
        payload.extend(data)

    archive = bytearray(struct.pack("<8sII", MAGIC, VERSION, len(entries)))
    for name, offset, size in entries:
        archive.extend(name.ljust(NAME_SIZE, b"\0"))
        archive.extend(struct.pack("<II", offset, size))
    archive.extend(payload)
    output.write_bytes(archive)


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack a MinimalOS .run bundle")
    parser.add_argument("source", type=Path, help="directory containing main.elf and resources")
    parser.add_argument("output", type=Path, help="output .run file")
    args = parser.parse_args()
    pack(args.source, args.output)


if __name__ == "__main__":
    main()
