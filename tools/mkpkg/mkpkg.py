#!/usr/bin/env python3
"""
mkpkg.py - build a MinimalOS .mpkg package archive from a directory.

Usage:
    python3 mkpkg.py <source-dir> <output.mpkg> [--store] [--verbose]

Every file and directory under <source-dir> is added to the archive,
using its path relative to <source-dir> as the entry name. Files are
LZSS-compressed by default (matching x86_64/lzss.h's decoder in the
kernel); pass --store to disable compression and store files as-is.

See src/intf/x86_64/pkgformat.h for the exact on-disk layout this
produces.
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"MPKG0001"
VERSION = 1
HEADER_SIZE = 16
NAME_SIZE = 128
ENTRY_SIZE = 148  # name[128] + 5 * uint32

FLAG_DIRECTORY = 1

METHOD_STORE = 0
METHOD_LZSS = 1

N = 4096       # window size     - MUST match LZSS_N in src/impl/compression/lzss.c
F = 18         # max match len   - MUST match LZSS_F
THRESHOLD = 2  # match threshold - MUST match LZSS_THRESHOLD


def lzss_compress(data: bytes) -> bytes:
    """Encode `data` for x86_64/lzss.h's lzss_decompress().

    A straightforward (not speed-optimized) LZSS encoder: for every
    position it brute-force searches the preceding N bytes for the
    longest match (up to F bytes), producing the classic
    Okumura-style ring-buffer LZSS bitstream the kernel decoder
    expects. Fine for the package sizes this tool is meant for; not
    meant for compressing large media files.
    """
    out = bytearray()
    flag_byte = 0
    flag_bit = 0
    chunk = bytearray()

    def flush():
        nonlocal flag_byte, flag_bit, chunk
        if flag_bit > 0:
            out.append(flag_byte)
            out.extend(chunk)
            flag_byte = 0
            flag_bit = 0
            chunk = bytearray()

    pos = 0
    n = len(data)
    while pos < n:
        max_len = min(F, n - pos)
        best_len = 0
        best_src = 0

        if max_len >= THRESHOLD + 1:
            window_start = max(0, pos - N)
            for src in range(window_start, pos):
                length = 0
                while length < max_len and data[src + length] == data[pos + length]:
                    length += 1
                if length > best_len:
                    best_len = length
                    best_src = src
                    if length >= max_len:
                        break

        if best_len > THRESHOLD:
            match_pos = (N - F + best_src) & (N - 1)
            length_field = best_len - (THRESHOLD + 1)
            chunk.append(match_pos & 0xFF)
            chunk.append(((match_pos >> 8) << 4) | (length_field & 0x0F))
            # flag bit stays 0 for a match
            flag_bit += 1
            pos += best_len
        else:
            flag_byte |= (1 << flag_bit)
            chunk.append(data[pos])
            flag_bit += 1
            pos += 1

        if flag_bit == 8:
            flush()

    flush()
    return bytes(out)


def collect_entries(source: Path):
    entries = []
    for path in sorted(source.rglob("*")):
        rel = path.relative_to(source).as_posix()
        entries.append((rel, path.is_dir(), path))
    return entries


def _write_progress(current: int, total: int, name: str, interactive: bool):
    width = 30
    filled = width * current // total
    bar = "=" * filled + ">" + " " * (width - filled - 1)
    line = f"mkpkg: [{bar}] {current}/{total} {name}"
    if interactive:
        print(f"\r{line:<100}", end="", file=sys.stderr, flush=True)
    else:
        print(line, file=sys.stderr)


def build(source: Path, output: Path, store: bool, verbose: bool = False):
    if not source.is_dir():
        raise ValueError(f"source is not a directory: {source}")

    entries = collect_entries(source)
    encoded_entries = []
    payload = bytearray()
    table_size = HEADER_SIZE + len(entries) * ENTRY_SIZE
    offset = table_size

    interactive = sys.stderr.isatty()
    total_entries = len(entries)

    for index, (rel, is_dir, path) in enumerate(entries, start=1):
        name_bytes = rel.encode("utf-8")
        if len(name_bytes) >= NAME_SIZE:
            raise ValueError(f"entry name too long (max {NAME_SIZE - 1} bytes): {rel}")

        if is_dir:
            encoded_entries.append((name_bytes, FLAG_DIRECTORY, METHOD_STORE, 0, 0, 0))
            if verbose:
                print(f"mkpkg: {rel}/ (directory)", file=sys.stderr)
            _write_progress(index, total_entries, rel + "/", interactive)
            continue

        raw = path.read_bytes()
        if store:
            method, data = METHOD_STORE, raw
        else:
            compressed = lzss_compress(raw)
            if len(compressed) < len(raw):
                method, data = METHOD_LZSS, compressed
            else:
                # Compression didn't help (small/incompressible file) -
                # store raw rather than pay the decode cost for nothing.
                method, data = METHOD_STORE, raw

        entry_offset = offset
        payload.extend(data)
        offset += len(data)

        encoded_entries.append((name_bytes, 0, method, len(raw), len(data), entry_offset))

        if verbose:
            method_name = "lzss" if method == METHOD_LZSS else "store"
            print(f"mkpkg: {rel} ({method_name}, {len(raw)} -> {len(data)} bytes)",
                  file=sys.stderr)
        _write_progress(index, total_entries, rel, interactive)

    if interactive:
        print(file=sys.stderr)

    with open(output, "wb") as f:
        f.write(struct.pack("<8sII", MAGIC, VERSION, len(encoded_entries)))
        for name_bytes, flags, method, uncompressed_size, compressed_size, data_offset in encoded_entries:
            f.write(name_bytes.ljust(NAME_SIZE, b"\0"))
            f.write(struct.pack("<IIIII", flags, method, uncompressed_size,
                                compressed_size, data_offset))
        f.write(payload)

    total_raw = sum(e[3] for e in encoded_entries)
    total_packed = sum(e[4] for e in encoded_entries)
    note = " (stored, no compression)" if store else ""
    print(f"Wrote {output}: {len(encoded_entries)} entries, "
          f"{total_raw} -> {total_packed} bytes{note}")


def main():
    parser = argparse.ArgumentParser(description="Build a MinimalOS .mpkg package")
    parser.add_argument("source", type=Path, help="directory to package")
    parser.add_argument("output", type=Path, help="output .mpkg file")
    parser.add_argument("--store", action="store_true",
                        help="store files as-is instead of LZSS-compressing them")
    parser.add_argument("--verbose", action="store_true",
                        help="print each entry and its compression result")
    args = parser.parse_args()

    try:
        build(args.source, args.output, args.store, args.verbose)
    except (OSError, ValueError) as error:
        print(f"mkpkg: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
