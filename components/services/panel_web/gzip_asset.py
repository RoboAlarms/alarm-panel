#!/usr/bin/env python3
"""gzip one Panel Manager file for the firmware image (called by this component's CMakeLists).

    gzip_asset.py <in> <out>

The text is minified first, by file extension (minify_asset.py). The header carries no file name and no time, so the same file always gives the same bytes: a
10-byte header, the deflate stream, then CRC-32 and length (panel_web.c relies on that layout
when it inflates a file for a client that doesn't take gzip).
"""
import gzip
import sys
from pathlib import Path

import minify_asset


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    kind = minify_asset.kind_of(src)
    data = src.read_bytes()
    if kind:
        data = minify_asset.minify(data.decode("utf-8"), kind).encode("utf-8")
    dst.write_bytes(gzip.compress(data, compresslevel=9, mtime=0))


if __name__ == "__main__":
    main()
