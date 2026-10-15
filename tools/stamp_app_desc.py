#!/usr/bin/env python3
"""stamp_app_desc.py - give a freshly linked app ELF its own build date and time (BL-176).

ESP-IDF fills esp_app_desc's date and time from __DATE__ and __TIME__ when it compiles
esp_app_desc.c, and an incremental build compiles that file only when its own inputs change: an
image rebuilt after a change elsewhere kept the date of an older build, and System details
showed it. The firmware build (cmake/alarm_firmware.cmake, alarm_stamp_build_date()) runs this
right after each link of the app, so it costs nothing on a build that doesn't relink. It writes
the link's local time into the descriptor's time and date fields in the ELF, in __TIME__'s and
__DATE__'s own format ("15:41:07", "Oct  6 2026"), before elf2image takes the ELF's SHA-256 and
makes the image. SOURCE_DATE_EPOCH, when set, gives the time in UTC instead, as GCC does. A
descriptor without a date (CONFIG_APP_COMPILE_TIME_DATE off, as in a reproducible build) is
left as it is.

    python tools/stamp_app_desc.py build/alarm_crowpanel_p4-dev.elf
"""
import os
import struct
import sys
import time
from pathlib import Path

SYMBOL = b"esp_app_desc"
MAGIC = 0xABCD5432  # ESP_APP_DESC_MAGIC_WORD
DESC_SIZE = 256  # sizeof(esp_app_desc_t)
TIME_FIELD = (80, 16)  # esp_app_desc_t.time: offset, size
DATE_FIELD = (96, 16)  # esp_app_desc_t.date
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

SHT_SYMTAB = 2
SHT_NOBITS = 8
SHN_LORESERVE = 0xFF00


class StampError(Exception):
    """The ELF has no app descriptor this script can stamp."""


def _layout(data):
    """(section headers' offset, entry size and count, section header struct, symbol struct,
    symbol field order: value, size, section) for a little-endian ELF."""
    if data[:4] != b"\x7fELF" or len(data) < 0x40:
        raise StampError("not an ELF file")
    if data[5] != 1:
        raise StampError("not a little-endian ELF")
    if data[4] == 1:  # ELF32
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
        sh, sym, order = struct.Struct("<10I"), struct.Struct("<IIIBBH"), (1, 2, 5)
    elif data[4] == 2:  # ELF64
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        sh, sym, order = struct.Struct("<IIQQQQIIQQ"), struct.Struct("<IBBHQQ"), (4, 5, 3)
    else:
        raise StampError("unknown ELF class")
    return shoff, shentsize, shnum, sh, sym, order


def find_app_desc(data):
    """The file offset of the esp_app_desc_t the symbol esp_app_desc names."""
    shoff, shentsize, shnum, sh, sym, (value_i, size_i, shndx_i) = _layout(data)
    if shentsize != sh.size or shoff + shnum * sh.size > len(data):
        raise StampError("unreadable section headers")
    sections = [sh.unpack_from(data, shoff + i * sh.size) for i in range(shnum)]
    # Section header fields: name, type, flags, addr, offset, size, link, info, align, entsize.
    for s_type, s_off, s_size, s_link in ((s[1], s[4], s[5], s[6]) for s in sections):
        if s_type != SHT_SYMTAB or s_link >= shnum:
            continue
        strtab = data[sections[s_link][4]:sections[s_link][4] + sections[s_link][5]]
        # Every offset where the name reads exactly "esp_app_desc": the linker may store it as
        # the tail of a longer name.
        names, pos = set(), strtab.find(SYMBOL + b"\0")
        while pos >= 0:
            names.add(pos)
            pos = strtab.find(SYMBOL + b"\0", pos + 1)
        if not names:
            continue
        for entry in sym.iter_unpack(data[s_off:s_off + s_size - s_size % sym.size]):
            shndx = entry[shndx_i]
            if entry[0] not in names or shndx == 0 or shndx >= min(shnum, SHN_LORESERVE):
                continue
            _, d_type, _, d_addr, d_off, d_size = sections[shndx][:6]
            rel = entry[value_i] - d_addr
            if (d_type == SHT_NOBITS or entry[size_i] != DESC_SIZE or rel < 0
                    or rel + DESC_SIZE > d_size or d_off + rel + DESC_SIZE > len(data)):
                raise StampError("esp_app_desc is not a 256-byte descriptor in the file")
            offset = d_off + rel
            if struct.unpack_from("<I", data, offset)[0] != MAGIC:
                raise StampError("esp_app_desc has no ESP_APP_DESC_MAGIC_WORD")
            return offset
    raise StampError("no esp_app_desc symbol")


def build_time(env=None):
    """The time to stamp: SOURCE_DATE_EPOCH in UTC when set (as GCC does), else local time."""
    env = os.environ if env is None else env
    epoch = env.get("SOURCE_DATE_EPOCH", "").strip()
    if epoch:
        return time.gmtime(int(epoch))
    return time.localtime()


def fields(when):
    """(__TIME__, __DATE__) for a struct_time, in the compiler's C-locale format."""
    return (f"{when.tm_hour:02d}:{when.tm_min:02d}:{when.tm_sec:02d}",
            f"{MONTHS[when.tm_mon - 1]} {when.tm_mday:2d} {when.tm_year}")


def stamp(path, when):
    """Write `when` into the ELF's descriptor; (time, date) written, or None if it has no date."""
    path = Path(path)
    data = path.read_bytes()
    offset = find_app_desc(data)
    old = [data[offset + o:offset + o + n] for o, n in (TIME_FIELD, DATE_FIELD)]
    if all(f.strip(b"\0") == b"" for f in old):
        return None  # built without a date: leave it so
    new = fields(when)
    with path.open("r+b") as f:
        for (o, n), text in zip((TIME_FIELD, DATE_FIELD), new):
            f.seek(offset + o)
            f.write(text.encode("ascii").ljust(n, b"\0"))
    return new


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: " + __doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    try:
        done = stamp(argv[0], build_time())
    except (OSError, ValueError, StampError, struct.error) as e:  # ValueError: SOURCE_DATE_EPOCH
        print(f"stamp_app_desc: {argv[0]}: {e}", file=sys.stderr)
        return 1
    if done:
        print(f"stamp_app_desc: {Path(argv[0]).name} built {done[1]} {done[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
