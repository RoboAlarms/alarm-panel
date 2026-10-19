#!/usr/bin/env python3
"""vectors_plink_view.py - the zone record and zones_crc of the panel link's STATUS
(docs/features/11 "Zone record": flags u16, trouble u8, walk u8, walk_first_s u16, walk_trips u8,
little-endian; zones_crc = the CRC-32 (zlib) of the records of zones 1..zones_used), made from the
document alone with Python's zlib, for test_plink_view.c test_zone_record_vectors.

    python vectors_plink_view.py
"""
import struct
import zlib

# (flags, trouble, walk, walk_first_s, walk_trips) for zones 1, 2, 3
ZONES = [
    (0x0201, 0x08, 0x05, 0x1234, 7),
    (0x0000, 0x00, 0x00, 0x0000, 0),
    (0x0441, 0x01, 0x0D, 0xFFFF, 255),
]


def record(z) -> bytes:
    return struct.pack("<HBBHB", *z)


if __name__ == "__main__":
    blob = b"".join(record(z) for z in ZONES)
    print("records:", ", ".join(f"0x{b:02X}" for b in blob))
    print(f"zones_crc(1..3) = 0x{zlib.crc32(blob):08X}")
    print(f"zones_crc(1..1) = 0x{zlib.crc32(record(ZONES[0])):08X}")
    print(f"zones_crc(none) = 0x{zlib.crc32(b''):08X}")
