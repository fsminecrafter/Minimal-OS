#!/usr/bin/env python3
"""Takes freedoom1.wad and its licence out of a Freedoom release zip.

Used by 'make mosdesk-doom':
    extract_freedoom.py <freedoom-X.Y.Z.zip> <X.Y.Z> <output dir>
writes <output dir>/freedoom1.wad and <output dir>/FREEDOOM-COPYING.txt.
"""
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    archive, version, out = sys.argv[1:]
    prefix = f"freedoom-{version}/"
    with zipfile.ZipFile(archive) as z:
        for name, target in (("freedoom1.wad", "freedoom1.wad"), ("COPYING.txt", "FREEDOOM-COPYING.txt")):
            with open(f"{out}/{target}", "wb") as f:
                f.write(z.read(prefix + name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
