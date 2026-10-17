#!/usr/bin/env python3
"""
mkpkg.py - build a MinimalOS .mpkg package archive from a directory.

Usage:
    python3 mkpkg.py <source-dir> <output.mpkg> [--store] [--verbose]
                     [-j N] [--no-cache] [--force]

Every file and directory under <source-dir> is added to the archive,
using its path relative to <source-dir> as the entry name. Files are
LZSS-compressed by default (matching x86_64/lzss.h's decoder in the
kernel); pass --store to disable compression and store files as-is.

Speed:
  * Files are compressed in parallel, -j N processes (default: every
    CPU, or $MKPKG_JOBS when set).
  * Compressed files are cached by content in $MKPKG_CACHE (default
    ~/.cache/minimal-os-mkpkg), so only changed files are recompressed.
  * When the sources and options are unchanged and the output file is
    the one this tool last wrote, the output is left untouched (its
    timestamp too, so make does not relink what depends on it).
    --force rebuilds anyway; --no-cache disables both caches.

See src/intf/x86_64/pkgformat.h for the exact on-disk layout this
produces.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
from bisect import bisect_left
from concurrent.futures import ProcessPoolExecutor
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

# Bump when the encoder's output changes, so cached blobs are not reused.
ENCODER_ID = b"lzss-okumura-v1"
# Bump when the archive layout or reuse rules change.
ARCHIVE_ID = "mpkg-v1"


def lzss_compress(data: bytes) -> bytes:
    """Encode `data` for x86_64/lzss.h's lzss_decompress().

    Produces the classic Okumura-style ring-buffer LZSS bitstream the
    kernel decoder expects. For each position it picks the longest
    match in the preceding N bytes (up to F bytes), the earliest one on
    a tie. Only matches of THRESHOLD + 1 bytes or more are ever used, so
    candidates are looked up by their first three bytes instead of
    scanning the whole window; the output is the same as a brute-force
    scan.
    """
    out = bytearray()
    flag_byte = 0
    flag_bit = 0
    chunk = bytearray()
    # 3-byte prefix -> positions where it starts, in increasing order.
    chains = {}
    indexed = 0  # positions below this are in `chains`

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
        while indexed < pos and indexed + 3 <= n:
            chains.setdefault(data[indexed:indexed + 3], []).append(indexed)
            indexed += 1

        max_len = min(F, n - pos)
        best_len = 0
        best_src = 0

        if max_len >= THRESHOLD + 1:
            candidates = chains.get(data[pos:pos + 3])
            if candidates:
                window_start = max(0, pos - N)
                for i in range(bisect_left(candidates, window_start),
                               len(candidates)):
                    src = candidates[i]
                    # Cannot beat the best so far unless it matches one
                    # byte further.
                    if best_len and data[src + best_len] != data[pos + best_len]:
                        continue
                    length = 3
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


def default_jobs() -> int:
    try:
        jobs = int(os.environ.get("MKPKG_JOBS", "0"))
    except ValueError:
        jobs = 0
    return jobs if jobs > 0 else (os.cpu_count() or 1)


def default_cache_dir() -> Path:
    configured = os.environ.get("MKPKG_CACHE")
    if configured:
        return Path(configured)
    base = os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache"
    return Path(base) / "minimal-os-mkpkg"


class BlobCache:
    """Compressed files, keyed by a hash of the raw contents."""

    def __init__(self, directory):
        self.directory = directory

    def _path(self, digest: str) -> Path:
        return self.directory / "blobs" / digest[:2] / digest

    def get(self, digest: str):
        if not self.directory:
            return None
        try:
            return self._path(digest).read_bytes()
        except OSError:
            return None

    def put(self, digest: str, data: bytes):
        if not self.directory:
            return
        path = self._path(digest)
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_suffix(f".tmp{os.getpid()}")
            temporary.write_bytes(data)
            os.replace(temporary, path)
        except OSError:
            pass


def _compress_job(raw: bytes) -> bytes:
    return lzss_compress(raw)


def _stamp_path(cache_dir: Path, output: Path) -> Path:
    key = hashlib.sha256(str(output.resolve()).encode()).hexdigest()
    return cache_dir / "outputs" / f"{key}.json"


def _output_identity(output: Path):
    try:
        stat = output.stat()
    except OSError:
        return None
    return [stat.st_size, stat.st_mtime_ns]


def build(source: Path, output: Path, store: bool, verbose: bool = False,
          jobs: int = 1, cache_dir=None, force: bool = False):
    if not source.is_dir():
        raise ValueError(f"source is not a directory: {source}")

    entries = collect_entries(source)
    raws = {}
    digests = {}
    manifest = hashlib.sha256()
    manifest.update(f"{ARCHIVE_ID}|{ENCODER_ID.decode()}|store={store}\n".encode())
    for rel, is_dir, path in entries:
        name_bytes = rel.encode("utf-8")
        if len(name_bytes) >= NAME_SIZE:
            raise ValueError(f"entry name too long (max {NAME_SIZE - 1} bytes): {rel}")
        if is_dir:
            manifest.update(f"D {rel}\n".encode())
            continue
        raw = path.read_bytes()
        digest = hashlib.sha256(ENCODER_ID + raw).hexdigest()
        raws[rel] = raw
        digests[rel] = digest
        manifest.update(f"F {rel} {digest}\n".encode())
    manifest_digest = manifest.hexdigest()

    # Unchanged sources and the output is still what we wrote: keep it.
    stamp = _stamp_path(cache_dir, output) if cache_dir else None
    if stamp and not force:
        try:
            recorded = json.loads(stamp.read_text())
        except (OSError, ValueError):
            recorded = None
        if (recorded and recorded.get("manifest") == manifest_digest and
                recorded.get("output") == _output_identity(output)):
            print(f"{output} is up to date ({len(entries)} entries)")
            return

    cache = BlobCache(cache_dir)
    compressed = {}
    if not store:
        pending = []
        for rel, raw in raws.items():
            cached = cache.get(digests[rel])
            if cached is not None:
                compressed[rel] = cached
            else:
                pending.append(rel)
        if pending:
            # Largest first, so one big file does not finish last alone.
            pending.sort(key=lambda rel: len(raws[rel]), reverse=True)
            workers = max(1, min(jobs, len(pending)))
            if workers == 1:
                results = map(_compress_job, (raws[rel] for rel in pending))
            else:
                pool = ProcessPoolExecutor(max_workers=workers)
                results = pool.map(_compress_job, (raws[rel] for rel in pending))
            interactive = sys.stderr.isatty()
            for done, (rel, data) in enumerate(zip(pending, results), start=1):
                compressed[rel] = data
                cache.put(digests[rel], data)
                _write_progress(done, len(pending), rel, interactive)
            if workers > 1:
                pool.shutdown()
            if interactive:
                print(file=sys.stderr)
        if verbose:
            print(f"mkpkg: compressed {len(pending)} file(s), "
                  f"{len(raws) - len(pending)} from cache", file=sys.stderr)

    encoded_entries = []
    payload = bytearray()
    offset = HEADER_SIZE + len(entries) * ENTRY_SIZE

    for rel, is_dir, path in entries:
        name_bytes = rel.encode("utf-8")
        if is_dir:
            encoded_entries.append((name_bytes, FLAG_DIRECTORY, METHOD_STORE, 0, 0, 0))
            if verbose:
                print(f"mkpkg: {rel}/ (directory)", file=sys.stderr)
            continue

        raw = raws[rel]
        packed = compressed.get(rel)
        if packed is not None and len(packed) < len(raw):
            method, data = METHOD_LZSS, packed
        else:
            # Stored on request, or compression didn't help (small or
            # incompressible file) - store raw rather than pay the decode
            # cost for nothing.
            method, data = METHOD_STORE, raw

        encoded_entries.append((name_bytes, 0, method, len(raw), len(data), offset))
        payload.extend(data)
        offset += len(data)

        if verbose:
            method_name = "lzss" if method == METHOD_LZSS else "store"
            print(f"mkpkg: {rel} ({method_name}, {len(raw)} -> {len(data)} bytes)",
                  file=sys.stderr)

    temporary = output.with_name(output.name + ".tmp")
    with open(temporary, "wb") as f:
        f.write(struct.pack("<8sII", MAGIC, VERSION, len(encoded_entries)))
        for name_bytes, flags, method, uncompressed_size, compressed_size, data_offset in encoded_entries:
            f.write(name_bytes.ljust(NAME_SIZE, b"\0"))
            f.write(struct.pack("<IIIII", flags, method, uncompressed_size,
                                compressed_size, data_offset))
        f.write(payload)
    os.replace(temporary, output)

    if stamp:
        try:
            stamp.parent.mkdir(parents=True, exist_ok=True)
            stamp.write_text(json.dumps({"manifest": manifest_digest,
                                         "output": _output_identity(output)}))
        except OSError:
            pass

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
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs(),
                        help="compress this many files at once "
                             "(default: $MKPKG_JOBS or every CPU)")
    parser.add_argument("--no-cache", action="store_true",
                        help="neither reuse nor record cached results")
    parser.add_argument("--force", action="store_true",
                        help="rewrite the output even when nothing changed")
    args = parser.parse_args()

    try:
        build(args.source, args.output, args.store, args.verbose,
              jobs=max(1, args.jobs),
              cache_dir=None if args.no_cache else default_cache_dir(),
              force=args.force)
    except (OSError, ValueError) as error:
        print(f"mkpkg: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
