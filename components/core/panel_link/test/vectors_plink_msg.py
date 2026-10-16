#!/usr/bin/env python3
"""The panel link's message payloads, built independently of the C codecs (test_plink_msg.c).

Every golden payload in test_plink_msg.c is written by hand from docs/features/11 "Messages";
this script builds the same messages from the same table with Python's struct module, so the two
are made separately, and prints them as hex for a reviewer to compare (--check compares them
itself against the C file). It also gives the CMD digest's known answer (LINK-018), SHA-256 from
the 'cryptography' package over op and the op section, never the codes.

    python vectors_plink_msg.py           # print every payload and the digests
    python vectors_plink_msg.py --check   # compare with the arrays in test_plink_msg.c
"""
import re
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes


def seq(first, n):
    return bytes((first + i) & 0xFF for i in range(n))


def string(s, max_len):
    b = s.encode("utf-8")
    assert len(b) <= max_len
    return bytes([len(b)]) + b


def section(body):
    assert len(body) <= 255
    return bytes([len(body)]) + body


def caps(partitions, zones, users, devices):
    return section(struct.pack("<BHBB", partitions, zones, users, devices))


def code(digits):
    b = digits.encode("ascii")
    return bytes([len(b)]) + b + bytes(10 - len(b))


def sha256(data):
    h = hashes.Hash(hashes.SHA256())
    h.update(data)
    return h.finalize()


def part_record(number, f):
    body = struct.pack(
        "<BBBBBHBBBBIHHHHHIIIIHHBBBBBH",
        f["state"], f["level"], f["arm_flags"], f["armed_by"], f["arm_src_kind"], f["arm_src_id"],
        f["ready_levels"], f["flags"], f["sounder"], f["bell"], f["delay_left_ms"], f["memory"],
        f["faults"], f["bypassed"], f["alarms"], f["troubles"], f["walk_left_ms"],
        f["walk_age_ms"], f["walk_id"], f["walk_seq"], f["walk_tested"], f["walk_total"],
        f["walk_flags"], f["walk_started_by"], f["walk_ended_by"], f["walk_end_how"],
        f["walk_src_kind"], f["walk_src_id"])
    assert len(body) == 52
    return bytes([number]) + section(body)


def event_record(seq_no, mono, epoch, typ, partition, zone, user, kind, ident, arg, arg2, flags):
    rec = struct.pack("<IQqHBHBBHBBB", seq_no, mono, epoch, typ, partition, zone, user, kind,
                      ident, arg, arg2, flags)
    assert len(rec) == 32
    return rec


def zero_part():
    keys = ["state", "level", "arm_flags", "armed_by", "arm_src_kind", "arm_src_id",
            "ready_levels", "flags", "sounder", "bell", "delay_left_ms", "memory", "faults",
            "bypassed", "alarms", "troubles", "walk_left_ms", "walk_age_ms", "walk_id",
            "walk_seq", "walk_tested", "walk_total", "walk_flags", "walk_started_by",
            "walk_ended_by", "walk_end_how", "walk_src_kind", "walk_src_id"]
    return {k: 0 for k in keys}


def vectors():
    v = {}
    v["G_PAIR_REQ"] = (struct.pack("<IBHB", 0x11223344, 1, 0xA55A, 1) + caps(4, 128, 64, 16) +
                       seq(0x00, 32) + string("Hall", 47) + string("1.2.3", 31))
    v["G_PAIR_COMMIT"] = (struct.pack("<IBH", 0x01020304, 1, 0x0003) + seq(0x20, 32) +
                          seq(0x40, 32) + string("Harbor House", 47) + string("a1b2c3", 6))
    v["G_PAIR_NONCE"] = struct.pack("<I", 0xDEADBEEF) + seq(0x60, 16)
    v["G_PAIR_CONFIRM"] = struct.pack("<I", 7) + seq(0xA0, 16)
    v["G_PAIR_DONE"] = struct.pack("<IBB", 7, 3, 2) + seq(0xB0, 16)
    v["G_PAIR_FAIL"] = struct.pack("<IB", 0xABCD, 7)
    v["G_HELLO"] = (struct.pack("<BH", 1, 0x0102) + caps(2, 320, 32, 8) + seq(0x10, 16) +
                    seq(0x30, 32) + seq(0xF0, 16))
    v["G_WELCOME"] = struct.pack("<BH", 1, 0) + seq(0x10, 16) + seq(0x30, 32) + seq(0xF0, 16)
    v["G_REKEY"] = struct.pack("<I", 0x89ABCDEF)

    system = section(struct.pack("<qBBIIIBBIIBHIIII", 0x0102030405060708, 0x05, 1, 9, 2, 4, 3,
                                 0x03, 120000, 30000, 2, 16, 1234, 0xCAFEBABE, 0x1234,
                                 0x12345678))
    p1 = dict(state=3, level=1, arm_flags=0x02, armed_by=5, arm_src_kind=1, arm_src_id=3,
              ready_levels=0x0E, flags=0x2D, sounder=4, bell=1, delay_left_ms=31000,
              memory=0x8002, faults=2, bypassed=1, alarms=3, troubles=4, walk_left_ms=60000,
              walk_age_ms=1000, walk_id=0x00ABCDEF, walk_seq=0x11, walk_tested=5, walk_total=9,
              walk_flags=1, walk_started_by=2, walk_ended_by=0, walk_end_how=0, walk_src_kind=1,
              walk_src_id=4)
    p3 = zero_part()
    p3.update(ready_levels=0x0E, flags=0x01)
    v["G_STATUS"] = system + part_record(1, p1) + part_record(3, p3)

    v["G_ZONES"] = (struct.pack("<IHHB", 0x0BADF00D, 5, 2, 7) +
                    struct.pack("<HBBHB", 0x0201, 0x01, 0x05, 300, 2) +
                    struct.pack("<HBBHB", 0x0002, 0, 0, 0, 0))
    v["G_ZONES_REQ"] = struct.pack("<HH", 1, 128)
    v["G_CFG_REQ"] = struct.pack("<IIH", 0xCAFEBABE, 0x400, 0x400)
    v["G_CFG_PAGE"] = struct.pack("<IIIH", 0xCAFEBABE, 16, 12, 4) + bytes.fromhex("DEADBEEF")
    v["G_EV_REQ"] = struct.pack("<IB", 1234, 50)
    v["G_EVENTS"] = (bytes([0x00, 2, 32]) +
                     event_record(0x4D1, 123456, 0x1122334455667788, 1, 1, 0, 5, 1, 3, 1, 2, 1) +
                     event_record(0x4D2, 123556, -1, 6, 1, 7, 0, 3, 7, 0, 0, 0))
    op_arm = section(struct.pack("<BBBBHBBB", 0, 2, 2, 0x01, 0, 0, 0, 0))
    v["G_CMD"] = struct.pack("<IB", 0x00C0FFEE, 1) + op_arm + code("1234") + code("")
    op_change = section(struct.pack("<BBBBHBBB", 0, 1, 0, 0, 0, 0, 0, 0))
    v["G_CMD_CHANGE"] = (struct.pack("<IB", 42, 3) + op_change + code("123456") +
                         code("9876543210"))
    v["G_RESULT"] = struct.pack("<IBBHB", 0x00C0FFEE, 6, 5, 0x0102, 1)
    v["G_SOUND"] = struct.pack("<BBH", 2, 12, 10)
    v["G_HEARTBEAT"] = struct.pack("<BbIH", 0x05, -61, 86400, 0xFFFF) + string("0.9.1", 31)
    v["G_TIME"] = struct.pack("<q", 1760000000000) + string("EST5EDT,M3.2.0,M11.1.0", 63)
    v["G_BYE"] = bytes([1])

    digests = {
        "DIGEST_ARM": sha256(bytes([1]) + op_arm),
        "DIGEST_CHANGE": sha256(bytes([3]) + op_change),
    }
    return v, digests


def c_arrays(path):
    """The byte arrays named G_* and DIGEST_* in the C test file."""
    text = Path(path).read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = {}
    for m in re.finditer(r"static const uint8_t (\w+)\[\w*\]\s*=\s*\{(.*?)\};", text, re.S):
        out[m.group(1)] = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
    return out


def main():
    v, digests = vectors()
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        c = c_arrays(Path(__file__).with_name("test_plink_msg.c"))
        bad = 0
        for name, data in list(v.items()) + list(digests.items()):
            if c.get(name) != data:
                print(f"{name}: differs\n  py {data.hex()}\n  c  {c.get(name, b'').hex()}")
                bad += 1
        print("all match" if bad == 0 else f"{bad} differ")
        return 1 if bad else 0
    for name, data in list(v.items()) + list(digests.items()):
        print(f"{name} ({len(data)} bytes): {data.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
