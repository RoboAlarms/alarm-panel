"""Golden frames for test_plink_frame.c, made independently of the C code with Python's
`cryptography` (OpenSSL AESCCM, 8-byte tag) from docs/features/11 "Frame format (v1)":

    header (17 B, little-endian): 'A' 'L', version 1, type, flags, net_id u16, sender u16,
                                  sid u32, seq u32
    sealed frame: header || CCM(payload) || tag, the header the associated data
    nonce (13 B): dir ('M' 0x4D from the master, 'K' 0x4B from a keypad) || sid || sender ||
                  net_id || seq

Prints each frame as a C array to paste into the test. Run it from anywhere:

    python components/core/panel_link/test/vectors_plink_frame.py
"""

import struct

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

SEALED = 0x01
FROM_MASTER = 0x02


def header(ftype, flags, net_id, sender, sid, seq):
    return b"AL" + struct.pack("<BBBHHII", 1, ftype, flags, net_id, sender, sid, seq)


def nonce(flags, net_id, sender, sid, seq):
    direction = 0x4D if flags & FROM_MASTER else 0x4B
    return struct.pack("<BIHHI", direction, sid, sender, net_id, seq)


def sealed(key, ftype, flags, net_id, sender, sid, seq, payload):
    hdr = header(ftype, flags, net_id, sender, sid, seq)
    n = nonce(flags, net_id, sender, sid, seq)
    assert len(hdr) == 17 and len(n) == 13
    return hdr + AESCCM(key, tag_length=8).encrypt(n, payload, hdr), n


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
    return f"static const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};"


# A plain header: PAIR_REQ from a keypad not paired yet, net id not known.
print(c_array("HDR_PAIR_REQ", header(0x01, 0x00, 0x0000, 0xFFFF, 0, 0)))
# A sealed header: STATUS from the master.
print(c_array("HDR_STATUS", header(0x20, SEALED | FROM_MASTER, 0x1234, 0, 0xA1B2C3D4, 0x01020304)))

# Frame 1: STATUS from the master, 40 bytes of payload (byte i = 5 i + 1), key 40..4f.
key1 = bytes(range(0x40, 0x50))
pay1 = bytes((5 * i + 1) % 256 for i in range(40))
f1, n1 = sealed(key1, 0x20, SEALED | FROM_MASTER, 0x1234, 0, 0xA1B2C3D4, 0x01020304, pay1)
print(c_array("NONCE_1", n1))
print(c_array("FRAME_1", f1))

# Frame 2: HEARTBEAT from keypad 3, 9 bytes of payload (byte i = 0x90 + i), key 50..5f.
key2 = bytes(range(0x50, 0x60))
pay2 = bytes(range(0x90, 0x99))
f2, n2 = sealed(key2, 0x2A, SEALED, 0x1234, 3, 0xA1B2C3D4, 7, pay2)
print(c_array("NONCE_2", n2))
print(c_array("FRAME_2", f2))

# Frame 3: ZONES_REQ from keypad 8 with no payload at all (the tag alone), key 60..6f.
key3 = bytes(range(0x60, 0x70))
f3, n3 = sealed(key3, 0x22, SEALED, 0xFFFE, 8, 0x00000001, 0x7FFFFFFF, b"")
print(c_array("FRAME_3", f3))
