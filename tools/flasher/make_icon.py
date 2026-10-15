#!/usr/bin/env python3
"""Wrap a PNG in a Windows .ico (Windows Vista and later read PNG icons), for the Flasher's
program icon. No imaging library needed.

    python make_icon.py logo.png out.ico
"""
import struct
import sys
from pathlib import Path


def png_size(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    return struct.unpack(">II", data[16:24])


def main(src, dst):
    png = Path(src).read_bytes()
    w, h = png_size(png)
    if w > 256 or h > 256:
        raise SystemExit("an icon is at most 256x256")
    header = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(png), 6 + 16)
    Path(dst).write_bytes(header + entry + png)


if __name__ == "__main__":
    main(*sys.argv[1:3])
