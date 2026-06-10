#!/usr/bin/env python3
"""
wavtoheader.py

Convert a PCM WAV file into a C header containing:
- raw audio bytes
- sample rate
- channel count
- bits per sample
- data length

Usage:
    python3 wavtoheader.py input.wav output.h
    python3 wavtoheader.py input.wav output.h --name my_sound
"""

from __future__ import annotations

import argparse
import os
import sys
import wave
from pathlib import Path


def sanitize_identifier(name: str) -> str:
    out = []
    for i, ch in enumerate(name):
        if ch.isalnum() or ch == "_":
            if i == 0 and ch.isdigit():
                out.append("_")
            out.append(ch)
        else:
            out.append("_")
    ident = "".join(out)
    return ident or "sound"


def format_bytes_as_c_array(data: bytes, bytes_per_line: int = 12) -> str:
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i : i + bytes_per_line]
        line = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append("    " + line)
    return ",\n".join(lines)


def read_wav(path: Path):
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        sample_rate = wf.getframerate()
        sample_width = wf.getsampwidth()  # bytes per sample per channel
        frames = wf.getnframes()
        comptype = wf.getcomptype()
        compname = wf.getcompname()
        data = wf.readframes(frames)

    if comptype != "NONE":
        raise ValueError(
            f"Unsupported WAV compression: {comptype} ({compname}). "
            "This script only supports uncompressed PCM WAV files."
        )

    bits_per_sample = sample_width * 8
    return {
        "channels": channels,
        "sample_rate": sample_rate,
        "sample_width": sample_width,
        "bits_per_sample": bits_per_sample,
        "frames": frames,
        "data": data,
    }


def write_header(
    input_path: Path,
    output_path: Path,
    symbol_name: str,
    wav_info: dict,
) -> None:
    guard = sanitize_identifier(output_path.stem).upper() + "_H"
    array_name = sanitize_identifier(symbol_name)

    data = wav_info["data"]
    channels = wav_info["channels"]
    sample_rate = wav_info["sample_rate"]
    bits_per_sample = wav_info["bits_per_sample"]
    frames = wav_info["frames"]

    c_array = format_bytes_as_c_array(data)

    header = f"""\
#ifndef {guard}
#define {guard}

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {{
#endif

static const uint8_t {array_name}[] = {{
{c_array}
}};

static const size_t {array_name}_len = {len(data)};
static const uint32_t {array_name}_sample_rate = {sample_rate};
static const uint16_t {array_name}_channels = {channels};
static const uint16_t {array_name}_bits_per_sample = {bits_per_sample};
static const uint32_t {array_name}_frames = {frames};

#ifdef __cplusplus
}}
#endif

#endif /* {guard} */
"""

    output_path.write_text(header, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert a PCM WAV file into a C header."
    )
    parser.add_argument("input", help="Input WAV file")
    parser.add_argument("output", help="Output header file")
    parser.add_argument(
        "--name",
        default=None,
        help="Symbol name for the generated array (default: derived from input file name)",
    )

    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)

    if not input_path.exists():
        print(f"error: input file not found: {input_path}", file=sys.stderr)
        return 1

    symbol_name = args.name or input_path.stem

    try:
        wav_info = read_wav(input_path)
        write_header(input_path, output_path, symbol_name, wav_info)
    except wave.Error as e:
        print(f"error: invalid WAV file: {e}", file=sys.stderr)
        return 1
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(f"Wrote {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())