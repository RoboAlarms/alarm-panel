#!/usr/bin/env python3
"""The panel link's message payloads, built independently of the C codecs (test_plink_msg.c).

Every golden payload in test_plink_msg.c is written by hand from docs/features/11 "Messages",
"Updates through the link" and "Smart Home and Home Assistant on keypads"; this script builds the
same messages from the same tables with Python's struct module, so the two are made separately,
and prints them as hex for a reviewer to compare (--check compares them itself against the C file
and against the frozen fixture plink_wire_fixture_v1.h). It also gives the digests' known answers
(LINK-018, LINK-047), SHA-256 from the 'cryptography' package: CMD's over op and the op section,
DEV_ACT's over the act section and the padded ref, never the codes.

The fixture also freezes every v1 number of the tables (capability and flag bits, op, kind, state,
last, why, result, voice and origin values), which test_plink_msg.c compares with plink_msg.h's.

    python vectors_plink_msg.py             # print every payload and the digests
    python vectors_plink_msg.py --check     # compare with test_plink_msg.c and the fixture
    python vectors_plink_msg.py --fixture   # print plink_wire_fixture_v1.h (made once, frozen)
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


def ref_padded(ref):
    """DEV_ACT's ref: its length, then its bytes zero-padded to 95."""
    b = ref.encode("utf-8")
    assert len(b) <= 95
    return bytes([len(b)]) + b + bytes(95 - len(b))


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


def dev_rec(flags, action, vflags, level, modes, hvac, setpoint, temp, since, dcaps, word):
    """A device's record section (docs/features/11 "Record section")."""
    return section(struct.pack("<BBBBBBiiIB", flags, action, vflags, level, modes, hvac, setpoint,
                               temp, since, dcaps) + string(word, 23))


def tlv(field, fmt, value):
    """A settings field: id u8, len u8, the value little-endian (proto_prefs.h's ids)."""
    b = struct.pack(fmt, value)
    return bytes([field, len(b)]) + b


def vectors():
    v = {}
    v["G_PAIR_REQ"] = (struct.pack("<IBHB", 0x11223344, 1, 0xA55A, 1) + caps(4, 128, 64, 16) +
                       seq(0x00, 32) + string("Hall", 47) + string("1.2.3", 31))
    v["G_PAIR_COMMIT"] = (struct.pack("<IBH", 0x01020304, 1, 0x0003) + seq(0x20, 32) +
                          seq(0x40, 32) + string("Harbor House", 47) + string("a1b2c3", 6))
    v["G_PAIR_NONCE"] = struct.pack("<I", 0xDEADBEEF) + seq(0x60, 16)
    v["G_PAIR_REVEAL"] = struct.pack("<I", 0xDEADBEEF) + seq(0x70, 16)
    v["G_PAIR_CONFIRM"] = struct.pack("<I", 7) + seq(0xA0, 16)
    v["G_PAIR_DONE"] = struct.pack("<IBB", 7, 3, 2) + seq(0xB0, 16)
    v["G_PAIR_FAIL"] = struct.pack("<IB", 0xABCD, 7)
    v["G_HELLO"] = (struct.pack("<BH", 1, 0x0102) + caps(2, 320, 32, 8) + seq(0x10, 16) +
                    seq(0x30, 32) + seq(0xF0, 16))
    v["G_WELCOME"] = struct.pack("<BH", 1, 0) + seq(0x10, 16) + seq(0x30, 32) + seq(0xF0, 16)
    v["G_REKEY"] = struct.pack("<I", 0x89ABCDEF)

    system = section(struct.pack("<qBBIIIBBIIBHIIIIQ", 0x0102030405060708, 0x05, 1, 9, 2, 4, 3,
                                 0x03, 120000, 30000, 2, 16, 1234, 0xCAFEBABE, 0x1234,
                                 0x12345678, 0x112233445566))
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
    v["G_HEARTBEAT_PREFS"] = v["G_HEARTBEAT"] + section(struct.pack("<I", 0x0C0FFEE0))
    v["G_TIME"] = struct.pack("<q", 1760000000000) + string("EST5EDT,M3.2.0,M11.1.0", 63)
    v["G_BYE"] = bytes([1])

    # Updates through the link.
    v["G_FW_INFO"] = section(bytes([0x07]) + string("crowpanel-7", 31) + string("1.3", 15) +
                             string("1.0", 3) + struct.pack("<H", 100) +
                             string("roboalarms", 32) + string("1.4.0", 32) +
                             struct.pack("<I", 0x00480000) + seq(0xA0, 8) + bytes(8) +
                             string("2.12.3", 23))
    v["G_FW_ORDER"] = (section(struct.pack("<IBB", 0x01020304, 4, 0) + string("1.4.0", 32) +
                               struct.pack("<I", 3600000) + seq(0x40, 32) + seq(0x80, 32)) +
                       section(string("https://fw.example.com", 95) + string("stable", 7)) +
                       section(struct.pack("<HBH", 1024, 16, 50)))
    v["G_FW_STATE"] = (section(struct.pack("<IBBBBII", 0x01020304, 4, 3, 1, 54, 1048576,
                                           3600000) + seq(0x40, 8) + string("1.4.0", 32)) +
                       section(struct.pack("<BB", 0, 2) + string("1.3.9", 32) + bytes([0x05])) +
                       section(struct.pack("<BBI", 1, 13, 3600) + string("1.5.0", 32) +
                               seq(0xA0, 8) + bytes([1])))
    v["G_FW_REQ"] = section(seq(0x40, 8) + struct.pack("<IHB", 65536, 1024, 16))
    v["G_FW_PAGE"] = seq(0x40, 8) + struct.pack("<IIH", 3600000, 65536, 4) + bytes.fromhex(
        "DEADBEEF")

    # Smart Home on keypads.
    v["G_DEV_LIST_REQ"] = struct.pack("<IIH", 0x1BADB002, 0, 1024)
    v["G_DEV_LIST_PAGE"] = struct.pack("<IIIH", 0x1BADB002, 16, 12, 4) + bytes.fromhex("CAFEF00D")
    rec_light = dev_rec(0x01, 0, 0x09, 75, 0, 0, 0, 0, 0, 0x88, "on")
    rec_heat = dev_rec(0x03, 10, 0x06, 0, 0x0B, 2, 2150, -550, 1760000000, 0x80, "heat")
    v["G_DEV_REC"] = rec_heat
    v["G_DEV_STATE"] = (section(struct.pack("<IIB", 0x1BADB002, 0x5EED5EED, 2)) + bytes([0]) +
                        rec_light + bytes([5]) + rec_heat)
    v["G_DEV_STATE_REQ"] = struct.pack("<IBB", 0x1BADB002, 0, 0)
    act_unlock = section(struct.pack("<BiBB", 5, 0, 0, 0))
    v["G_DEV_ACT"] = (struct.pack("<I", 0x0BEEF001) + act_unlock + ref_padded("lock.front_door") +
                      code("1234"))
    act_mode = section(struct.pack("<BiBB", 11, 0, 0x02, 0))
    v["G_DEV_ACT_MODE"] = (struct.pack("<I", 0x0BEEF003) + act_mode + ref_padded("climate.hall") +
                           code(""))
    v["G_DEV_ACK"] = struct.pack("<IBB", 0x0BEEF001, 2, 3)
    rec_lock = dev_rec(0x03, 5, 0, 0, 0, 0, 0, 0, 0, 0, "locked")
    v["G_DEV_ACK_REC"] = (struct.pack("<IBB", 0x0BEEF002, 0, 0) +
                          section(struct.pack("<IB", 0x1BADB002, 7) + rec_lock))
    v["G_DEV_CAT"] = section(struct.pack("<BB", 3, 1) + string("lock.front_door", 95) +
                             string("Front door", 47) + string("Hall", 47))

    # A keypad's settings: brightness 40, night starts 22:00, screen saver after 10 min.
    brightness = tlv(1, "<B", 40)
    night_start = tlv(6, "<H", 1320)
    saver_after = tlv(3, "<I", 600000)
    v["G_PREFS"] = (section(struct.pack("<I", 0x777)) +
                    section(string("a1b2c3", 6) + struct.pack("<BI", 1, 0x0007FFFF)) +
                    section(bytes([1, 11, 3])) +
                    section(bytes([3]) + brightness + night_start + saver_after))
    v["G_PREFS_REQ"] = struct.pack("<I", 0xFEEDF00D)
    v["G_PREFS_SET"] = struct.pack("<IB", 0x777, 1) + section(bytes([2]) + brightness +
                                                              tlv(11, "<B", 2))

    digests = {
        "DIGEST_ARM": sha256(bytes([1]) + op_arm),
        "DIGEST_CHANGE": sha256(bytes([3]) + op_change),
        "DIGEST_DEV_ACT": sha256(act_unlock + ref_padded("lock.front_door")),
    }
    return v, digests


# The frozen v1 fixture: one payload of every v1 type (and the variants a type has), by the codec
# row in test_plink_msg.c that reads it. NOT_PAIRED (0x13) has no payload; BEACON (0x18) is
# reserved, not v1.
FIXTURE = [
    ("PAIR_REQ", "G_PAIR_REQ"), ("PAIR_COMMIT", "G_PAIR_COMMIT"), ("PAIR_NONCE", "G_PAIR_NONCE"),
    ("PAIR_REVEAL", "G_PAIR_REVEAL"), ("PAIR_CONFIRM", "G_PAIR_CONFIRM"),
    ("PAIR_DONE", "G_PAIR_DONE"), ("PAIR_FAIL", "G_PAIR_FAIL"), ("HELLO", "G_HELLO"),
    ("WELCOME", "G_WELCOME"), ("REKEY", "G_REKEY"), ("STATUS", "G_STATUS"), ("ZONES", "G_ZONES"),
    ("ZONES_REQ", "G_ZONES_REQ"), ("CFG_REQ", "G_CFG_REQ"), ("CFG_PAGE", "G_CFG_PAGE"),
    ("EV_REQ", "G_EV_REQ"), ("EVENTS", "G_EVENTS"), ("CMD", "G_CMD"),
    ("CMD change", "G_CMD_CHANGE"), ("RESULT", "G_RESULT"), ("SOUND", "G_SOUND"),
    ("HEARTBEAT", "G_HEARTBEAT"), ("HEARTBEAT prefs", "G_HEARTBEAT_PREFS"), ("TIME", "G_TIME"),
    ("BYE", "G_BYE"), ("FW_INFO", "G_FW_INFO"), ("FW_ORDER", "G_FW_ORDER"),
    ("FW_STATE", "G_FW_STATE"), ("FW_REQ", "G_FW_REQ"), ("FW_PAGE", "G_FW_PAGE"),
    ("DEV_LIST_REQ", "G_DEV_LIST_REQ"), ("DEV_LIST_PAGE", "G_DEV_LIST_PAGE"),
    ("DEV_STATE", "G_DEV_STATE"), ("DEV_STATE_REQ", "G_DEV_STATE_REQ"), ("DEV_ACT", "G_DEV_ACT"),
    ("DEV_ACT mode", "G_DEV_ACT_MODE"), ("DEV_ACK", "G_DEV_ACK"), ("DEV_ACK rec", "G_DEV_ACK_REC"),
    ("PREFS", "G_PREFS"), ("PREFS_REQ", "G_PREFS_REQ"), ("PREFS_SET", "G_PREFS_SET"),
    ("dev record", "G_DEV_REC"), ("catalog record", "G_DEV_CAT"),
]


# Every v1 number of docs/features/11 "Messages", "STATUS", "Updates through the link" and "Smart
# Home and Home Assistant on keypads" (Capabilities, Frozen values), by its plink_msg.h name.
NUMBERS = [
    ("PLINK_PROTO", 1),
    # Capabilities.
    ("PLINK_CAP_FW", 0x0001), ("PLINK_CAP_HOME", 0x0002), ("PLINK_CAP_PREFS", 0x0004),
    # STATUS: the system section's flags, a partition record's flags, report.
    ("PLINK_SF_PROGRAM", 0x01), ("PLINK_SF_EXIT_SOUNDS", 0x02), ("PLINK_SF_UPDATING", 0x04),
    ("PLINK_SF_HOLD", 0x08), ("PLINK_SF_FIRE", 0x10), ("PLINK_SF_CO", 0x20),
    ("PLINK_PF_READY", 0x01), ("PLINK_PF_CHIME", 0x02), ("PLINK_PF_WALK_TEST", 0x04),
    ("PLINK_PF_TRIGGERED", 0x08), ("PLINK_PF_EXIT_ERROR", 0x10), ("PLINK_PF_TROUBLE_UNACK", 0x20),
    ("PLINK_PF_QUICK_EXIT", 0x40),
    ("PLINK_REPORT_NONE", 0), ("PLINK_REPORT_OFFLINE", 1), ("PLINK_REPORT_ONLINE", 2),
    # HEARTBEAT, EVENTS, CMD.
    ("PLINK_HB_MAINS_LOST", 0x01), ("PLINK_HB_BATTERY_LOW", 0x02), ("PLINK_HB_BATTERY_THERE", 0x04),
    ("PLINK_STATUS_AGE_NONE", 0xFFFF), ("PLINK_EVF_GAP", 0x01),
    ("PLINK_OP_COMMAND", 1), ("PLINK_OP_AUTH", 2), ("PLINK_OP_CHANGE_CODE", 3),
    # Updates through the link: FW_INFO can, op, kind, state, flags, last, why.
    ("PLINK_FWI_FETCH", 0x01), ("PLINK_FWI_RELAY", 0x02), ("PLINK_FWI_KEY_ACCEPT", 0x04),
    ("PLINK_FWI_CFG_UNREADABLE", 0x08),
    ("PLINK_FW_OP_INFO", 1), ("PLINK_FW_OP_PROBE", 2), ("PLINK_FW_OP_FETCH", 3),
    ("PLINK_FW_OP_RELAY", 4), ("PLINK_FW_OP_INSTALL", 5), ("PLINK_FW_OP_CANCEL", 6),
    ("PLINK_FW_KIND_APP", 0), ("PLINK_FW_KIND_C6", 1), ("PLINK_FW_KIND_LANG", 2),
    ("PLINK_FW_KIND_ASSETS", 3),
    ("PLINK_FWS_IDLE", 0), ("PLINK_FWS_PROBING", 1), ("PLINK_FWS_FETCHING", 2),
    ("PLINK_FWS_RECEIVING", 3), ("PLINK_FWS_KEY_WAIT", 4), ("PLINK_FWS_VERIFYING", 5),
    ("PLINK_FWS_READY", 6), ("PLINK_FWS_INSTALLING", 7), ("PLINK_FWS_FAILED", 8),
    ("PLINK_FWS_STOPPED", 9),
    ("PLINK_FWF_PAUSED", 0x01),
    ("PLINK_FW_LAST_NONE", 0), ("PLINK_FW_LAST_UPDATED", 1), ("PLINK_FW_LAST_ROLLED_BACK", 2),
    ("PLINK_FW_LAST_STOPPED", 3), ("PLINK_FW_LAST_FAILED", 4),
    ("PLINK_FW_WHY_OK", 0),
    ("PLINK_FW_WHY_NO_NETWORK", 1), ("PLINK_FW_WHY_NO_LOOKUP", 2), ("PLINK_FW_WHY_NO_CONNECT", 3),
    ("PLINK_FW_WHY_SERVER_ERROR", 4), ("PLINK_FW_WHY_NOT_A_SERVER", 5),
    ("PLINK_FW_WHY_UNKNOWN_DEVICE", 10), ("PLINK_FW_WHY_HELD_BACK", 11),
    ("PLINK_FW_WHY_STEPPING_STONE", 12), ("PLINK_FW_WHY_OTHER_VERSION", 13),
    ("PLINK_FW_WHY_UP_TO_DATE", 14), ("PLINK_FW_WHY_UNKNOWN_VERSION", 15),
    ("PLINK_FW_WHY_NOT_OFFERED", 16),
    ("PLINK_FW_WHY_BROKE_OFF", 20), ("PLINK_FW_WHY_TOO_SLOW", 21), ("PLINK_FW_WHY_BAD_SIZE", 22),
    ("PLINK_FW_WHY_BAD_SHA", 23), ("PLINK_FW_WHY_STALLED", 24), ("PLINK_FW_WHY_TIMEOUT", 25),
    ("PLINK_FW_WHY_NO_MEMORY", 26), ("PLINK_FW_WHY_FLASH", 27),
    ("PLINK_FW_WHY_KEY", 30), ("PLINK_FW_WHY_CHIP", 31), ("PLINK_FW_WHY_PROJECT", 32),
    ("PLINK_FW_WHY_NOT_NEWER", 33), ("PLINK_FW_WHY_SIGNATURE", 34), ("PLINK_FW_WHY_TOO_BIG", 35),
    ("PLINK_FW_WHY_UNKNOWN_KIND", 36), ("PLINK_FW_WHY_UNKNOWN_OP", 37),
    ("PLINK_FW_WHY_BUDGET", 38), ("PLINK_FW_WHY_BAD_ORDER", 40),
    ("PLINK_FW_WHY_WRONG_VERSION", 41),
    ("PLINK_FW_WHY_MASTER_BUSY", 50), ("PLINK_FW_WHY_NOT_QUIET", 51),
    ("PLINK_FW_WHY_PANIC_PENDING", 52), ("PLINK_FW_WHY_KEPT_TONE", 53),
    ("PLINK_FW_WHY_NO_STATUS", 54), ("PLINK_FW_WHY_BUSY", 55),
    ("PLINK_FW_WHY_STOPPED", 60), ("PLINK_FW_WHY_CANCELLED", 61),
    # Smart Home: a record's flags, DEV_ACT mode, DEV_ACK result, the list blob's compat.
    ("PLINK_DEVF_ONLINE", 0x01), ("PLINK_DEVF_PENDING", 0x02), ("PLINK_DEVF_FAILED", 0x04),
    ("PLINK_DEVF_UNSURE", 0x08),
    ("PLINK_DEV_MODE_OFF", 0x01), ("PLINK_DEV_MODE_HEAT", 0x02), ("PLINK_DEV_MODE_COOL", 0x04),
    ("PLINK_DEV_MODE_AUTO", 0x08),
    ("PLINK_DEV_OK", 0), ("PLINK_DEV_NEED_CODE", 1), ("PLINK_DEV_CODE_REFUSED", 2),
    ("PLINK_DEV_BUSY", 3), ("PLINK_DEV_UNREACHABLE", 4), ("PLINK_DEV_REFUSED", 5),
    ("PLINK_DEV_UNAVAILABLE", 6),
    ("PLINK_DEV_CAT_COMPAT", 1),
    # Settings: why, voice, origin.
    ("PLINK_PREFS_NOT_ALLOWED", 1), ("PLINK_PREFS_RANGE", 2), ("PLINK_PREFS_IN_USE", 3),
    ("PLINK_PREFS_HELD", 4), ("PLINK_PREFS_VOICE_NOT_READY", 5), ("PLINK_PREFS_BAD", 6),
    ("PLINK_PREFS_BUSY", 7),
    ("PLINK_VOICE_UNKNOWN", 0), ("PLINK_VOICE_READY", 1), ("PLINK_VOICE_LOADING", 2),
    ("PLINK_VOICE_NONE", 3),
    ("PLINK_ORIGIN_HA", 1), ("PLINK_ORIGIN_CONSOLE", 2),
]


def fixture_name(golden):
    return "PLINK_WIRE_V1_" + golden[2:]


def c_array(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def fixture_text(v):
    out = ["""/*
 * plink_wire_fixture_v1.h - one payload of every v1 message type of the panel link, as v1 sends
 * it (docs/features/11 "Messages", "Updates through the link", "Smart Home and Home Assistant on
 * keypads"; LINK-017, LINK-043): test_plink_msg.c checks that the decoders read each one as the
 * values it froze with them (test_wire_fixture_v1) and encode it again to exactly these bytes,
 * so a firmware that changes a v1 layout fails here, even in its encoder and decoder alike.
 * NOT_PAIRED (0x13) has no payload; BEACON (0x18) is reserved, not v1. The section helpers' own
 * layouts (a device's record, a catalog record) close the list. Then every v1 number of the
 * tables (capability and flag bits, values), which the test compares with plink_msg.h's.
 * Made independently by vectors_plink_msg.py --fixture from the documents' tables alone, never
 * from the C encoders; the values are fictional.
 * Frozen: never edit it; a v2 layout adds a new fixture beside it.
 */"""]
    for _, golden in FIXTURE:
        out.append(c_array(fixture_name(golden), v[golden]))
    out.append("")
    out.append("/* Each payload with the codec row of test_plink_msg.c that reads it. */")
    out.append("static const struct {")
    out.append("    const char *codec;")
    out.append("    const uint8_t *p;")
    out.append("    size_t len;")
    out.append("} PLINK_WIRE_V1[] = {")
    for row, golden in FIXTURE:
        name = fixture_name(golden)
        out.append(f'    {{"{row}", {name}, sizeof {name}}},')
    out.append("};")
    out.append("")
    out.append("/* Every v1 number, by its plink_msg.h name. */")
    out.append("static const struct {")
    out.append("    const char *name;")
    out.append("    uint32_t value;")
    out.append("} PLINK_WIRE_V1_NUMBERS[] = {")
    for name, value in NUMBERS:
        out.append(f'    {{"{name}", 0x{value:02X}u}},')
    out.append("};")
    return "\n".join(out) + "\n"


def c_numbers(path):
    """The PLINK_WIRE_V1_NUMBERS table of the fixture: [(name, value)]."""
    text = Path(path).read_text(encoding="utf-8")
    m = re.search(r"PLINK_WIRE_V1_NUMBERS\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if m is None:
        return []
    return [(n, int(x, 16)) for n, x in re.findall(r'\{"(\w+)", 0x([0-9A-F]+)u\}', m.group(1))]


def c_arrays(path):
    """The byte arrays named G_*, DIGEST_* or PLINK_WIRE_V1_* in a C file."""
    text = Path(path).read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = {}
    for m in re.finditer(r"static const uint8_t (\w+)\[\w*\]\s*=\s*\{(.*?)\};", text, re.S):
        out[m.group(1)] = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
    return out


def main():
    v, digests = vectors()
    if len(sys.argv) > 1 and sys.argv[1] == "--fixture":
        sys.stdout.buffer.write(fixture_text(v).encode("utf-8"))  # LF on every system
        return 0
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        here = Path(__file__)
        c = c_arrays(here.with_name("test_plink_msg.c"))
        fx = c_arrays(here.with_name("plink_wire_fixture_v1.h"))
        want = list(v.items()) + list(digests.items())
        want += [(fixture_name(g), v[g]) for _, g in FIXTURE]
        bad = 0
        for name, data in want:
            got = fx.get(name) if name.startswith("PLINK_WIRE_V1_") else c.get(name)
            if got != data:
                print(f"{name}: differs\n  py {data.hex()}\n  c  {(got or b'').hex()}")
                bad += 1
        numbers = c_numbers(here.with_name("plink_wire_fixture_v1.h"))
        if numbers != NUMBERS:
            print("PLINK_WIRE_V1_NUMBERS: differs")
            for (pn, pv), (cn, cv) in zip(NUMBERS, numbers):
                if (pn, pv) != (cn, cv):
                    print(f"  py {pn} {pv:#x}\n  c  {cn} {cv:#x}")
            if len(numbers) != len(NUMBERS):
                print(f"  py {len(NUMBERS)} numbers, c {len(numbers)}")
            bad += 1
        text = here.with_name("plink_wire_fixture_v1.h").read_bytes().decode("utf-8")
        if bad == 0 and text != fixture_text(v):
            print("plink_wire_fixture_v1.h: differs from --fixture (its text, or CR bytes)")
            bad += 1
        print("all match" if bad == 0 else f"{bad} differ")
        return 1 if bad else 0
    for name, data in list(v.items()) + list(digests.items()):
        print(f"{name} ({len(data)} bytes): {data.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
