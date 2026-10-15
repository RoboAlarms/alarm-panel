#!/usr/bin/env python3
"""Zigbee device signatures: reviewed JSON rows -> a C table for the host tests.

docs/features/20 testing.md section 2. Each file in components/core/zigbee_zones/test/signatures/
is one real device as it describes itself (node descriptor, power source, Basic manufacturer and
model, each endpoint's profile, device id, input and output clusters, and the IAS Zone Type an
IAS endpoint reads back) and what the panel must make of it (`expect`): its role, router or
sleepy, the enrollment path, the supervision window, the quirks-table row it matches, and the
endpoints described to the zone source as inputs, each with its ZSRC_ZB_CL_* clusters, zone type
and device class.

Run with no arguments to write zb_signatures.h beside the rows; --check fails (exit 1) when the
header is missing or stale, as tools/lvpro_gen.py --check does. CI runs --check.

A row is refused (exit 2, every problem listed) when it isn't what the table promises:
  - addresses: `ieee` a locally administered 64-bit address (bit 0x02 of its top byte set, the
    fictional kind, AGENTS.md "Conventions"), unique; `nwk` 0x0001..0xFFF7, unique;
  - endpoints 1..240 or 242 (Green Power), unique, at most ZSRC_ZB_EP_MAX; at most 16 clusters a
    list (ZSRC_ZB_EP_CLUSTERS_MAX);
  - `expect.router` is what the node descriptor says (a router, or receiver on when idle) and
    `expect.sleepy` its opposite;
  - an input's clusters are exactly the ZSRC_ZB_CL_* bits of its endpoint's input clusters (the
    mirror of zsrc_zb_cluster_bit), its zone type the endpoint's, its endpoint Home Automation;
  - only a sensor has inputs, and a sensor has at least one (a siren, keypad or fob goes to its
    own page: devices.md section 5, NOT_A_SENSOR);
  - names from the lists below; text printable ASCII.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIG_DIR = ROOT / "components" / "core" / "zigbee_zones" / "test" / "signatures"
HEADER_NAME = "zb_signatures.h"

EP_MAX = 8            # ZSRC_ZB_EP_MAX
EP_CLUSTERS_MAX = 16  # ZSRC_ZB_EP_CLUSTERS_MAX
PROFILE_HA = 0x0104

# zsrc_zigbee.h: ZCL cluster id -> ZSRC_ZB_CL_* name (zsrc_zb_cluster_bit).
CLUSTER_BITS = {
    0x0500: "IAS_ZONE",
    0x0006: "ON_OFF",
    0x0406: "OCCUPANCY",
    0x0402: "TEMP",
    0x0001: "POWER_CFG",
    0x0020: "POLL_CTRL",
    0xEF00: "TUYA",
    0x0502: "IAS_WD",
    0x0501: "IAS_ACE",
    0x0003: "IDENTIFY",
    0x0000: "BASIC",
    0x0019: "OTA",
    0x0B05: "DIAGNOSTICS",
}
CLUSTER_NAMES = set(CLUSTER_BITS.values())

LOGICAL = {"coordinator": "ZSRC_ZB_LOGICAL_COORDINATOR", "router": "ZSRC_ZB_LOGICAL_ROUTER",
           "end_device": "ZSRC_ZB_LOGICAL_END_DEVICE"}
MAC_RX_ON_IDLE = 0x08

# zbz_role_t (zbz_registry.h), by value: the generated header can't include zigbee_zones.
ROLES = ["unknown", "sensor", "siren", "keypad", "keyfob", "repeater", "other"]
ENROLL = ["none", "standard", "no_enroll", "unsupported"]
CLASSES = {
    "generic": "ALARM_DEVCLASS_GENERIC", "door": "ALARM_DEVCLASS_DOOR",
    "window": "ALARM_DEVCLASS_WINDOW", "garage_door": "ALARM_DEVCLASS_GARAGE_DOOR",
    "motion": "ALARM_DEVCLASS_MOTION", "glass_break": "ALARM_DEVCLASS_GLASS_BREAK",
    "smoke": "ALARM_DEVCLASS_SMOKE", "heat": "ALARM_DEVCLASS_HEAT", "co": "ALARM_DEVCLASS_CO",
    "gas": "ALARM_DEVCLASS_GAS", "water": "ALARM_DEVCLASS_WATER",
    "temperature": "ALARM_DEVCLASS_TEMPERATURE", "vibration": "ALARM_DEVCLASS_VIBRATION",
    "panic_button": "ALARM_DEVCLASS_PANIC_BUTTON", "keyfob": "ALARM_DEVCLASS_KEYFOB",
}


class RowError(Exception):
    pass


def _int(value, what: str, bits: int) -> int:
    """A number written as an int or a "0x..." string, in range for `bits`."""
    if isinstance(value, bool):
        raise RowError(f"{what}: not a number")
    if isinstance(value, str):
        try:
            n = int(value, 16) if value.lower().startswith("0x") else int(value, 10)
        except ValueError:
            raise RowError(f"{what}: {value!r} is not a number") from None
    elif isinstance(value, int):
        n = value
    else:
        raise RowError(f"{what}: not a number")
    if not 0 <= n < (1 << bits):
        raise RowError(f"{what}: {value!r} is out of range for {bits} bits")
    return n


def _text(value, what: str, allow_null: bool = False) -> str | None:
    if value is None and allow_null:
        return None
    if not isinstance(value, str):
        raise RowError(f"{what}: not text")
    if len(value) > 31:
        raise RowError(f"{what}: longer than 31 characters (the 32-byte buffers)")
    if any(not (0x20 <= ord(c) < 0x7F) for c in value):
        raise RowError(f"{what}: only printable ASCII")
    return value


def _get(obj: dict, key: str, what: str):
    if not isinstance(obj, dict) or key not in obj:
        raise RowError(f"{what}: missing '{key}'")
    return obj[key]


def cluster_names(in_clusters: list[int]) -> list[str]:
    """The ZSRC_ZB_CL_* names an endpoint's input clusters give, in bit order."""
    names = {CLUSTER_BITS[c] for c in in_clusters if c in CLUSTER_BITS}
    return [n for n in CLUSTER_BITS.values() if n in names]


def load_row(path: Path) -> dict:
    """One file, checked and normalised; RowError on any problem."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RowError(f"not readable JSON: {exc}") from None
    if not isinstance(raw, dict):
        raise RowError("not a JSON object")
    row: dict = {"file": path.name}
    row["device"] = _long_text(_get(raw, "device", "device"), "device")
    row["tests"] = _long_text(_get(raw, "tests", "tests"), "tests")
    _long_text(_get(raw, "provenance", "provenance"), "provenance")
    ieee = _int(_get(raw, "ieee", "ieee"), "ieee", 64)
    if ieee == 0 or ((ieee >> 56) & 0x02) == 0:
        raise RowError(f"ieee: {raw['ieee']} is not a locally administered (fictional) address")
    row["ieee"] = ieee
    nwk = _int(_get(raw, "nwk", "nwk"), "nwk", 16)
    if not 0x0001 <= nwk <= 0xFFF7:
        raise RowError(f"nwk: {raw['nwk']} is not a device's short address")
    row["nwk"] = nwk

    node = _get(raw, "node", "node")
    logical = _get(node, "logical_type", "node")
    if logical not in LOGICAL:
        raise RowError(f"node.logical_type: {logical!r} is not one of {sorted(LOGICAL)}")
    row["logical"] = logical
    row["mac_caps"] = _int(_get(node, "mac_caps", "node"), "node.mac_caps", 8)
    row["manufacturer_code"] = _int(_get(node, "manufacturer_code", "node"),
                                    "node.manufacturer_code", 16)
    row["server_mask"] = _int(_get(node, "server_mask", "node"), "node.server_mask", 16)
    row["power_source"] = _int(_get(raw, "power_source", "power_source"), "power_source", 8)
    basic = _get(raw, "basic", "basic")
    row["manufacturer"] = _text(_get(basic, "manufacturer", "basic"), "basic.manufacturer")
    row["model"] = _text(_get(basic, "model", "basic"), "basic.model")

    eps = _get(raw, "endpoints", "endpoints")
    if not isinstance(eps, list) or not eps:
        raise RowError("endpoints: a non-empty list")
    if len(eps) > EP_MAX:
        raise RowError(f"endpoints: more than {EP_MAX}")
    row["endpoints"] = []
    seen_ep = set()
    for i, e in enumerate(eps):
        what = f"endpoints[{i}]"
        n = _int(_get(e, "endpoint", what), f"{what}.endpoint", 8)
        if not (1 <= n <= 240 or n == 242):
            raise RowError(f"{what}.endpoint: {n} is not 1..240 or 242")
        if n in seen_ep:
            raise RowError(f"{what}.endpoint: {n} twice")
        seen_ep.add(n)
        lists = {}
        for key in ("in", "out"):
            lst = _get(e, key, what)
            if not isinstance(lst, list):
                raise RowError(f"{what}.{key}: a list")
            if len(lst) > EP_CLUSTERS_MAX:
                raise RowError(f"{what}.{key}: more than {EP_CLUSTERS_MAX} clusters")
            lists[key] = [_int(c, f"{what}.{key}", 16) for c in lst]
        zt = e.get("zone_type")
        row["endpoints"].append({
            "endpoint": n,
            "profile": _int(_get(e, "profile", what), f"{what}.profile", 16),
            "device_id": _int(_get(e, "device_id", what), f"{what}.device_id", 16),
            "in": lists["in"],
            "out": lists["out"],
            "zone_type": None if zt is None else _int(zt, f"{what}.zone_type", 16),
        })

    exp = _get(raw, "expect", "expect")
    role = _get(exp, "role", "expect")
    if role not in ROLES:
        raise RowError(f"expect.role: {role!r} is not one of {ROLES}")
    enroll = _get(exp, "enroll", "expect")
    if enroll not in ENROLL:
        raise RowError(f"expect.enroll: {enroll!r} is not one of {ENROLL}")
    router = _get(exp, "router", "expect")
    sleepy = _get(exp, "sleepy", "expect")
    if not isinstance(router, bool) or not isinstance(sleepy, bool):
        raise RowError("expect.router, expect.sleepy: true or false")
    listening = logical in ("router", "coordinator") or (row["mac_caps"] & MAC_RX_ON_IDLE) != 0
    if router != listening:
        raise RowError(f"expect.router: {router} but the node descriptor says "
                       f"{'it listens' if listening else 'it sleeps'}")
    if sleepy == router:
        raise RowError("expect.sleepy: must be the opposite of expect.router")
    sup = _get(exp, "supervision_s", "expect")
    if isinstance(sup, bool) or not isinstance(sup, int) or not 1 <= sup <= 86400:
        raise RowError("expect.supervision_s: 1..86400 seconds")
    quirk = _get(exp, "quirk", "expect")
    if quirk is not None:
        quirk = {"manufacturer": _text(_get(quirk, "manufacturer", "expect.quirk"),
                                       "expect.quirk.manufacturer", allow_null=True),
                 "model": _text(_get(quirk, "model", "expect.quirk"), "expect.quirk.model",
                                allow_null=True)}
    inputs = _get(exp, "inputs", "expect")
    if not isinstance(inputs, list):
        raise RowError("expect.inputs: a list")
    if role == "sensor" and not inputs:
        raise RowError("expect.inputs: a sensor has at least one input")
    if role != "sensor" and inputs:
        raise RowError(f"expect.inputs: a {role} has none (it goes to its own page)")
    by_ep = {e["endpoint"]: e for e in row["endpoints"]}
    row["inputs"] = []
    for i, it in enumerate(inputs):
        what = f"expect.inputs[{i}]"
        n = _int(_get(it, "endpoint", what), f"{what}.endpoint", 8)
        if n not in by_ep:
            raise RowError(f"{what}.endpoint: {n} is not one of the device's endpoints")
        e = by_ep[n]
        if e["profile"] != PROFILE_HA:
            raise RowError(f"{what}.endpoint: {n} is not a Home Automation endpoint")
        names = _get(it, "clusters", what)
        if not isinstance(names, list) or any(c not in CLUSTER_NAMES for c in names):
            raise RowError(f"{what}.clusters: names from {sorted(CLUSTER_NAMES)}")
        want = cluster_names(e["in"])
        if sorted(names) != sorted(want) or len(set(names)) != len(names):
            raise RowError(f"{what}.clusters: {names} but endpoint {n}'s input clusters give "
                           f"{want}")
        zt = it.get("zone_type")
        zt = None if zt is None else _int(zt, f"{what}.zone_type", 16)
        if zt != e["zone_type"]:
            raise RowError(f"{what}.zone_type: not endpoint {n}'s")
        cls = _get(it, "class", what)
        if cls not in CLASSES:
            raise RowError(f"{what}.class: {cls!r} is not one of {sorted(CLASSES)}")
        row["inputs"].append({"endpoint": n, "clusters": want, "zone_type": zt, "class": cls})
    row.update(role=role, enroll=enroll, router=router, sleepy=sleepy, supervision_s=sup,
               quirk=quirk)
    return row


def _long_text(value, what: str) -> str:
    if not isinstance(value, str) or not value:
        raise RowError(f"{what}: non-empty text")
    if any(not (0x20 <= ord(c) < 0x7F) for c in value):
        raise RowError(f"{what}: only printable ASCII")
    return value


def load_rows(sig_dir: Path) -> tuple[list[dict], list[str]]:
    """Every row, sorted by file name, and every problem found (file: problem)."""
    rows, problems = [], []
    files = sorted(sig_dir.glob("*.json"))
    if not files:
        problems.append(f"{sig_dir}: no signature rows")
    for path in files:
        try:
            rows.append(load_row(path))
        except RowError as exc:
            problems.append(f"{path.name}: {exc}")
    ieees, nwks = {}, {}
    for r in rows:
        for key, seen in (("ieee", ieees), ("nwk", nwks)):
            if r[key] in seen:
                problems.append(f"{r['file']}: {key} {r[key]:#x} is also {seen[r[key]]}'s")
            seen[r[key]] = r["file"]
    return rows, problems


def _c_str(s: str | None) -> str:
    if s is None:
        return "NULL"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _bits(names: list[str]) -> str:
    if not names:
        return "0u"
    return "(uint16_t)(" + " | ".join(f"ZSRC_ZB_CL_{n}" for n in names) + ")"


def _list(values: list[int]) -> str:
    return "{" + ", ".join(f"0x{v:04X}u" for v in values) + "}" if values else "{0}"


def render(rows: list[dict]) -> str:
    out = []
    w = out.append
    w("/*")
    w(" * zb_signatures.h - GENERATED by tools/zb_signatures.py from the JSON rows beside it: do not")
    w(" * edit.")
    w(" * Regenerate: python tools/zb_signatures.py   Check (CI): python tools/zb_signatures.py --check")
    w(" *")
    w(" * Zigbee device signatures (docs/features/20 testing.md section 2): each row is a device as it")
    w(" * describes itself and what the panel must make of it. Fictional addresses only.")
    w(" */")
    w("#ifndef ZB_SIGNATURES_H")
    w("#define ZB_SIGNATURES_H")
    w("")
    w("#include <stdbool.h>")
    w("#include <stddef.h>")
    w("#include <stdint.h>")
    w("")
    w('#include "zone_hub/zsrc_zigbee.h"')
    w("")
    w("/* The enrollment path the panel takes. */")
    w("#define ZBSIG_ENROLL_NONE        0u /* no IAS Zone: nothing to enroll */")
    w("#define ZBSIG_ENROLL_STANDARD    1u /* CIE write, then its Enroll Request answered */")
    w("#define ZBSIG_ENROLL_NO_ENROLL   2u /* CIE write; it reports without enrolling (quirk) */")
    w("#define ZBSIG_ENROLL_UNSUPPORTED 3u /* nothing asked: ZSRC_ZB_QUIRK_UNSUPPORTED */")
    w("")
    w("/* The role, by value as zbz_role_t (zigbee_zones/zbz_registry.h, which this header can't")
    w(" * include; the converter test of ZB-M3 asserts they agree). */")
    for i, r in enumerate(ROLES):
        w(f"#define ZBSIG_ROLE_{r.upper():<9}{i}u")
    w("")
    w("typedef struct {")
    w("    uint8_t endpoint;")
    w("    uint16_t profile;")
    w("    uint16_t device_id;")
    w("    uint8_t in_count, out_count;")
    w("    uint16_t in[ZSRC_ZB_EP_CLUSTERS_MAX];  /* ZCL cluster ids, as the descriptor lists them */")
    w("    uint16_t out[ZSRC_ZB_EP_CLUSTERS_MAX];")
    w("    uint16_t zone_type;                    /* its IAS Zone Type; ZSRC_ZB_ZONE_TYPE_NONE */")
    w("} zbsig_ep_t;")
    w("")
    w("/* An endpoint described to the zone source (zsrc_zb_device_seen), as it must end up. */")
    w("typedef struct {")
    w("    uint8_t endpoint;")
    w("    uint16_t clusters;     /* ZSRC_ZB_CL_* */")
    w("    uint16_t zone_type;")
    w("    uint8_t device_class;  /* alarm_device_class_t */")
    w("} zbsig_input_t;")
    w("")
    w("typedef struct {")
    w("    const char *file;")
    w("    const char *device;")
    w("    uint64_t ieee;")
    w("    uint16_t nwk;")
    w("    zsrc_zb_node_desc_t node;")
    w("    uint8_t power_source;  /* Basic 0x0007 */")
    w("    const char *manufacturer;")
    w("    const char *model;")
    w("    uint8_t ep_count;")
    w("    zbsig_ep_t ep[ZSRC_ZB_EP_MAX];")
    w("    /* what the panel must make of it */")
    w("    uint8_t role;          /* ZBSIG_ROLE_* */")
    w("    bool router;           /* it listens: a router, or receiver on when idle */")
    w("    bool sleepy;")
    w("    uint8_t enroll;        /* ZBSIG_ENROLL_* */")
    w("    uint32_t supervision_s;")
    w("    bool quirk;            /* it matches a row of zb_quirks.c: the one below */")
    w("    const char *quirk_manufacturer;")
    w("    const char *quirk_model;")
    w("    uint8_t input_count;")
    w("    zbsig_input_t input[ZSRC_ZB_EP_MAX];")
    w("} zbsig_t;")
    w("")
    w("static const zbsig_t ZB_SIGNATURES[] = {")
    for r in rows:
        w("    {")
        w(f"        .file = {_c_str(r['file'])},")
        w(f"        .device = {_c_str(r['device'])},")
        w(f"        .ieee = 0x{r['ieee']:016X}ull,")
        w(f"        .nwk = 0x{r['nwk']:04X}u,")
        w(f"        .node = {{.logical_type = {LOGICAL[r['logical']]},")
        w(f"                 .mac_caps = 0x{r['mac_caps']:02X}u,")
        w(f"                 .manufacturer_code = {r['manufacturer_code']}u,")
        w(f"                 .server_mask = 0x{r['server_mask']:04X}u}},")
        w(f"        .power_source = {r['power_source']}u,")
        w(f"        .manufacturer = {_c_str(r['manufacturer'])},")
        w(f"        .model = {_c_str(r['model'])},")
        w(f"        .ep_count = {len(r['endpoints'])}u,")
        w("        .ep = {")
        for e in r["endpoints"]:
            zt = "ZSRC_ZB_ZONE_TYPE_NONE" if e["zone_type"] is None else f"0x{e['zone_type']:04X}u"
            w(f"            {{.endpoint = {e['endpoint']}u,")
            w(f"             .profile = 0x{e['profile']:04X}u,")
            w(f"             .device_id = 0x{e['device_id']:04X}u,")
            w(f"             .in_count = {len(e['in'])}u,")
            w(f"             .out_count = {len(e['out'])}u,")
            w(f"             .in = {_list(e['in'])},")
            w(f"             .out = {_list(e['out'])},")
            w(f"             .zone_type = {zt}}},")
        w("        },")
        w(f"        .role = ZBSIG_ROLE_{r['role'].upper()},")
        w(f"        .router = {'true' if r['router'] else 'false'},")
        w(f"        .sleepy = {'true' if r['sleepy'] else 'false'},")
        w(f"        .enroll = ZBSIG_ENROLL_{r['enroll'].upper()},")
        w(f"        .supervision_s = {r['supervision_s']}u,")
        q = r["quirk"]
        w(f"        .quirk = {'true' if q is not None else 'false'},")
        w(f"        .quirk_manufacturer = {_c_str(q['manufacturer'] if q else None)},")
        w(f"        .quirk_model = {_c_str(q['model'] if q else None)},")
        w(f"        .input_count = {len(r['inputs'])}u,")
        if r["inputs"]:
            w("        .input = {")
            for it in r["inputs"]:
                zt = "ZSRC_ZB_ZONE_TYPE_NONE" if it["zone_type"] is None else f"0x{it['zone_type']:04X}u"
                w(f"            {{.endpoint = {it['endpoint']}u,")
                w(f"             .clusters = {_bits(it['clusters'])},")
                w(f"             .zone_type = {zt},")
                w(f"             .device_class = {CLASSES[it['class']]}}},")
            w("        },")
        w("    },")
    w("};")
    w("")
    w("#define ZB_SIGNATURE_COUNT (sizeof(ZB_SIGNATURES) / sizeof(ZB_SIGNATURES[0]))")
    w("")
    w("#endif /* ZB_SIGNATURES_H */")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="fail when the header is missing or stale instead of writing it")
    ap.add_argument("--dir", type=Path, default=SIG_DIR, help="the signature rows (default: %(default)s)")
    args = ap.parse_args(argv)
    rows, problems = load_rows(args.dir)
    if problems:
        for p in problems:
            print(f"zb_signatures: {p}", file=sys.stderr)
        return 2
    text = render(rows)
    header = args.dir / HEADER_NAME
    current = header.read_text(encoding="utf-8") if header.exists() else None
    if args.check:
        if current != text:
            print(f"zb_signatures: {header.name} is stale: run python tools/zb_signatures.py",
                  file=sys.stderr)
            return 1
        print(f"zb_signatures: {header.name} matches {len(rows)} rows")
        return 0
    if current != text:
        header.write_text(text, encoding="utf-8", newline="\n")
        print(f"zb_signatures: wrote {header.name} ({len(rows)} rows)")
    else:
        print(f"zb_signatures: {header.name} already up to date ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
