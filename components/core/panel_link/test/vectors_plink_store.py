#!/usr/bin/env python3
"""vectors_plink_store.py - the panel link's stored records (plink_store.h), built from the
layout written in docs/features/11 and include/panel_link/plink_store.h alone, never from the C
encoder, so the frozen fixture plink_store_fixture_v1.h is made independently: test_plink_store.c
checks that the C decoder reads these bytes as the values below and that the C encoder writes
exactly them.

    python vectors_plink_store.py      prints the two arrays of plink_store_fixture_v1.h

Values are fictional (docs: "Harbor House", no real panel, key or network).
"""
import struct
import zlib


def text(s: str) -> bytes:
    """A string: a length byte, then its UTF-8 bytes."""
    b = s.encode("utf-8")
    return bytes([len(b)]) + b


def record(kind: str, payload: bytes) -> bytes:
    """'P' 'L', kind, format 1, compat 1, payload_len u16, CRC-32 (zlib) u32, payload."""
    return (b"PL" + bytes([ord(kind), 1, 1]) +
            struct.pack("<HI", len(payload), zlib.crc32(payload)) + payload)


def peer(device, home, kind, flags, supervision_s, k_pair, paired_epoch_ms, name) -> bytes:
    """One keypad at the master: its length byte, then its fields in plink_peer_rec_t's order."""
    body = (struct.pack("<BBBBH", device, home, kind, flags, supervision_s) + k_pair +
            struct.pack("<q", paired_epoch_ms) + text(name))
    return bytes([len(body)]) + body


KEY_1 = bytes(range(0x00, 0x20))
KEY_3 = bytes(range(0xA0, 0xC0))

MASTER = record("M", struct.pack("<HB", 0x5A3C, 2) +
                peer(1, 1, 1, 0x00, 60, KEY_1, 1790000000000, "Hall keypad") +
                peer(3, 2, 3, 0x01, 300, KEY_3, 0, "Bureau étage"))

KEYPAD = record("K", struct.pack("<HBB", 0x5A3C, 3, 2) + KEY_3 + text("Harbor House") +
                text("3F9A1C"))


def c_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


if __name__ == "__main__":
    print(c_array("PLINK_STORE_MASTER_V1", MASTER))
    print(c_array("PLINK_STORE_KEYPAD_V1", KEYPAD))
