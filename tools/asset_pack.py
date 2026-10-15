#!/usr/bin/env python3
"""Build and read asset packs, `RAST` format 1 (docs/features/39 asset-packs.md, OPT-012).

    python tools/asset_pack.py build --kind letters|picture|certificates|boards --version V --out DIR
        [--key PEM] [--draft] [--match 0x...] --part NAME=TYPE:PATH [--part ...]
    python tools/asset_pack.py info FILE

`build` writes DIR/<store name>.roboasset (letters.roboasset, saver.roboasset, certs.roboasset,
boards.roboasset: the kind fixes the name): the payload padded with 0xFF to a multiple of 4096
bytes, then one 4096-byte signature sector - signed with `python -m espsecure sign_data --version 2`
when --key is given, else all 0xFF (as tools/i18n.py does for an unsigned language pack). TYPE is
font, jpeg, certs or json. The parts are laid out in the order given.

`--kind boards` is the Flasher's board descriptions (kind 0x80, which no panel keeps, served by the
update server): one part `boards=json:tools/flasher/roboflash/boards.json`, never a draft, its
version the file's own `version` (`2026.10.12`), signed with the release key so every Flasher takes
it:

    python tools/asset_pack.py build --kind boards --version 2026.10.12 --out build/flasher
        --key secrets/ota_signing_key.pem --part boards=json:tools/flasher/roboflash/boards.json

(one command). The file is read back with the Flasher's
read_asset (roboflash/packstore.py, the twin of the firmware's asset_pack_open()) before it is
written, and the kind's own rules are checked: its size bound, its part names and types.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "flasher"))

from roboflash import packstore as ps  # noqa: E402

SECTOR = ps.SECTOR
PART_TYPES = {"font": ps.PART_FONT, "jpeg": ps.PART_JPEG, "certs": ps.PART_CERTS, "json": ps.PART_JSON}
TYPE_NAMES = {v: k for k, v in PART_TYPES.items()}
LETTER_ROLES = ("caption", "body", "label", "title")
# --kind: (store kind, the whole file at most, in bytes)
KINDS = {
    "letters": (ps.KIND_LETTERS, 256 * 1024),
    "picture": (ps.KIND_PICTURE, 512 * 1024),
    "certificates": (ps.KIND_CERTS, 128 * 1024),
    "boards": (ps.KIND_BOARDS, 1024 * 1024),
}
KIND_NAMES = {kind: name for name, (kind, _) in KINDS.items()}
KIND_NAMES[ps.KIND_WEB] = "web"
STORE_NAMES = {**ps.ASSET_STORE_NAMES, ps.KIND_BOARDS: ps.BOARDS_NAME}
BOARDS_MAX = 256            # the update server takes up to 256 boards


def build_payload(kind, name, version, parts, match=0, draft=False):
    """The unsigned payload in the canonical layout. parts: [(name, type, bytes)] in table order.
    Raises ValueError when the result is not a valid pack."""
    for what, text in (("name", name), ("version", version)):
        if not isinstance(text, str) or not 1 <= len(text.encode("utf-8", "replace")) <= 15:
            raise ValueError("the pack's %s %r must be 1-15 characters" % (what, text))
    if not 1 <= len(parts) <= ps.ASSET_MAX_PARTS:
        raise ValueError("a pack holds 1-%d parts, not %d" % (ps.ASSET_MAX_PARTS, len(parts)))
    if not 0 <= match <= 0xFFFFFFFF:
        raise ValueError("match 0x%x does not fit 32 bits" % match)
    pos = ps.ASSET_HEADER + ps.ASSET_PART * len(parts)
    table, body = b"", b""
    for pname, ptype, data in parts:
        if not 1 <= len(pname.encode("utf-8", "replace")) <= 15:
            raise ValueError("the part name %r must be 1-15 characters" % pname)
        at = (pos + 3) & ~3
        body += b"\0" * (at - pos) + bytes(data)
        table += pname.encode("utf-8").ljust(16, b"\0") + struct.pack("<IIII", ptype, at, len(data), 0)
        pos = at + len(data)
    end = (pos + 3) & ~3
    body += b"\0" * (end - pos)
    header = (ps.ASSET_MAGIC + struct.pack("<HHIBBH", ps.ASSET_FORMAT, ps.ASSET_HEADER, end, kind,
                                           ps.ASSET_FLAG_DRAFT if draft else 0, len(parts))
              + name.encode("utf-8").ljust(16, b"\0") + version.encode("utf-8").ljust(16, b"\0")
              + struct.pack("<II", match, ps.ASSET_HEADER) + b"\0" * 72)
    assert len(header) == ps.ASSET_HEADER
    payload = header + table + body
    try:
        (ps.read_boards_payload if kind == ps.KIND_BOARDS else ps.read_asset_payload)(payload)
    except ps.StoreError as exc:
        raise ValueError(str(exc)) from None
    return payload


def unsigned_file(payload):
    """What espsecure sign_data would write, with an all-0xFF signature sector."""
    return payload + b"\xff" * (-len(payload) % SECTOR) + b"\xff" * SECTOR


def check_kind(kind_name, parts, match, version=None, draft=False):
    """The kind's own rules (asset-packs.md "The kinds") on [(name, type, bytes)]; ValueError."""
    if kind_name not in KINDS:
        raise ValueError("unknown kind %r (one of %s)" % (kind_name, ", ".join(KINDS)))
    if kind_name == "boards":
        check_boards(parts, version, draft)
        if match:
            raise ValueError("match is for the letters only; a boards pack carries 0")
        return
    if kind_name == "letters":
        for pname, ptype, _ in parts:
            if pname not in LETTER_ROLES:
                raise ValueError("a letters part is named by its role (%s), not %r"
                                 % (", ".join(LETTER_ROLES), pname))
            if ptype != ps.PART_FONT:
                raise ValueError("the letters part %r must be a font" % pname)
        return
    want = {"picture": ("photo", ps.PART_JPEG), "certificates": ("bundle", ps.PART_CERTS)}[kind_name]
    if [(n, t) for n, t, _ in parts] != [want]:
        raise ValueError("a %s pack holds one part %r of type %s" % (kind_name, want[0],
                                                                    TYPE_NAMES[want[1]]))
    if match:
        raise ValueError("match is for the letters only; a %s pack carries 0" % kind_name)


def check_boards(parts, version, draft):
    """The Flasher's board descriptions: one JSON part `boards` whose top level has a whole `format`
    of at least 1 and `boards`, an object of at most 256 boards by id, and whose own
    `version`, when it has one, is the pack's."""
    if [(n, t) for n, t, _ in parts] != [(ps.BOARDS_NAME, ps.PART_JSON)]:
        raise ValueError("a boards pack holds one part 'boards' of type json")
    if draft:
        raise ValueError("board descriptions are never a draft: every Flasher is offered the newest")
    try:
        doc = json.loads(parts[0][2].decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ValueError("the boards part is not UTF-8 JSON: %s" % exc) from None
    fmt = doc.get("format") if isinstance(doc, dict) else None
    if not isinstance(fmt, int) or isinstance(fmt, bool) or fmt < 1:
        raise ValueError("the boards part's top level needs a whole `format` of at least 1")
    if not isinstance(doc.get("boards"), dict) or not 1 <= len(doc["boards"]) <= BOARDS_MAX:
        raise ValueError("the boards part's `boards` must be an object of 1-%d boards by id" % BOARDS_MAX)
    if "version" in doc and doc["version"] != version:
        raise ValueError("the file says version %r; make the pack with --version %s"
                         % (doc["version"], doc["version"]))


def parse_part(spec):
    """NAME=TYPE:PATH into (name, type, bytes); ValueError."""
    pname, eq, rest = spec.partition("=")
    tname, colon, path = rest.partition(":")
    if not eq or not colon or not pname or not path:
        raise ValueError("--part wants NAME=TYPE:PATH, not %r" % spec)
    if tname not in PART_TYPES:
        raise ValueError("unknown part type %r (one of %s)" % (tname, ", ".join(PART_TYPES)))
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        raise ValueError("cannot read the part %s: %s" % (path, exc)) from None
    return pname, PART_TYPES[tname], data


def _check_file(data, payload):
    info = ps.read_boards_pack(data)[0] if payload[12] == ps.KIND_BOARDS else ps.read_asset(data)
    if data[:len(payload)] != payload:
        raise ValueError("the written file does not hold the payload it was made from")
    return info


def build(kind_name, version, out_dir, parts, key=None, draft=False, match=0):
    """Write out_dir/<store name>.roboasset; returns its path. ValueError / RuntimeError."""
    check_kind(kind_name, parts, match, version, draft)
    kind, limit = KINDS[kind_name]
    name = STORE_NAMES[kind]
    payload = build_payload(kind, name, version, parts, match, draft)
    size = len(unsigned_file(payload))
    if size > limit:
        raise ValueError("the %s pack would be %d bytes; the kind holds at most %d"
                         % (kind_name, size, limit))
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / (name + ".roboasset")
    with tempfile.TemporaryDirectory() as tmp:
        made = Path(tmp) / out.name
        if key:
            _sign(payload, key, made)
        else:
            made.write_bytes(unsigned_file(payload))
        data = made.read_bytes()
        try:
            _check_file(data, payload)
        except ps.StoreError as exc:
            raise ValueError("the written pack does not read back: %s" % exc) from None
        if len(data) > limit:
            raise ValueError("the signed %s pack is %d bytes; the kind holds at most %d"
                             % (kind_name, len(data), limit))
        tmp_out = out.with_name(out.name + ".tmp")
        shutil.copyfile(made, tmp_out)
        os.replace(tmp_out, out)
    return out


def _sign(payload, key, out):
    try:
        import espsecure  # noqa: F401
    except ImportError:
        raise RuntimeError("espsecure is not importable by %s; install esptool or run this with "
                           "the ESP-IDF Python (needed to sign with --key)" % sys.executable)
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "payload.bin"
        src.write_bytes(payload)
        cmd = [sys.executable, "-m", "espsecure", "sign_data", "--version", "2",
               "--keyfile", str(key), "--output", str(out), str(src)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError("espsecure sign_data failed: %s" % (res.stderr or res.stdout).strip())


def describe(info):
    """The lines `info` prints."""
    lines = ["name     %s" % info.name,
             "kind     %d (%s)" % (info.kind, KIND_NAMES.get(info.kind, "?")),
             "version  %s" % info.version,
             "match    0x%08x" % info.match,
             "draft    %s" % ("yes" if info.draft else "no"),
             "payload  %d bytes" % info.payload_length,
             "signed   %s" % (info.signing_key_sha256 or "no (all-0xFF signature sector)"),
             "parts    %d" % len(info.parts)]
    for p in info.parts:
        lines.append("  %-15s %-5s offset %-7d size %d"
                     % (p.name, TYPE_NAMES.get(p.type, str(p.type)), p.offset, p.size))
    return lines


def _match(text):
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a number (0x... or decimal)" % text) from None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build", help="write <store name>.roboasset")
    p.add_argument("--kind", required=True, choices=list(KINDS))
    p.add_argument("--version", required=True,
                   help="the firmware version it is made with (boards: the descriptions' own version)")
    p.add_argument("--out", required=True, help="the folder to write into")
    p.add_argument("--key", help="sign with this PEM key (espsecure); else an all-0xFF signature")
    p.add_argument("--draft", action="store_true", help="made by a development build")
    p.add_argument("--match", type=_match, default=0,
                   help="the letters: the fonts' fingerprint (0 for the other kinds)")
    p.add_argument("--part", action="append", default=[], metavar="NAME=TYPE:PATH",
                   help="a part (TYPE font, jpeg, certs or json), repeatable, in table order")
    p = sub.add_parser("info", help="print what a .roboasset file holds")
    p.add_argument("file")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            parts = [parse_part(s) for s in args.part]
            out = build(args.kind, args.version, args.out, parts, args.key, args.draft, args.match)
            print("wrote %s" % out)
        else:
            data = Path(args.file).read_bytes()
            info = ps.read_boards_pack(data)[0] if data[12:13] == bytes([ps.KIND_BOARDS]) else ps.read_asset(data)
            print("\n".join(describe(info)))
    except (ValueError, RuntimeError, OSError) as exc:
        print("asset_pack: %s" % exc, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
