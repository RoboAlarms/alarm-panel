"""OPT-012: asset packs (`RAST` format 1, docs/features/39 asset-packs.md) - the writer
(tools/asset_pack.py) and the Flasher's reader (roboflash/packstore.py read_asset), against the
frozen fixture the C reader (components/core/pack_store asset_pack.h) reads too."""
import contextlib
import importlib.util
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "flasher"))

from roboflash import packstore as ps  # noqa: E402
from roboflash.imagefile import key_digest_from_pem  # noqa: E402

SPEC = importlib.util.spec_from_file_location("asset_pack", ROOT / "tools" / "asset_pack.py")
ap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ap)

FIXTURE_H = ROOT / "components" / "core" / "pack_store" / "test" / "asset_pack_fixture_v1.h"
# The inputs the fixture was made from (its comment)
FIXTURE_PARTS = [("caption", 1, bytes(range(0x10, 0x15))), ("body", 1, bytes(range(0xA0, 0xA8))),
                 ("title", 1, b"\xF0\xF1\xF2")]
FIXTURE_MATCH = 0x1C2B3A49


def fixture_bytes():
    text = FIXTURE_H.read_text()
    text = text[text.index("ASSET_PACK_FIXTURE_V1[244]"):]
    data = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", text))
    assert len(data) == 244
    return data


def have_espsecure():
    try:
        import espsecure  # noqa: F401
        return True
    except ImportError:
        return False


def patched(data, at, raw):
    b = bytearray(data)
    b[at:at + len(raw)] = raw
    return bytes(b)


class FixtureTests(unittest.TestCase):
    """OPT-012: the writer makes the fixture's bytes; the reader reads its fields."""

    def test_writer_reproduces_the_fixture(self):
        payload = ap.build_payload(ps.KIND_LETTERS, "letters", "0.9.0", FIXTURE_PARTS, FIXTURE_MATCH)
        self.assertEqual(payload, fixture_bytes())

    def test_the_tool_writes_the_fixture_as_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            args = ["build", "--kind", "letters", "--version", "0.9.0", "--out", d,
                    "--match", "0x1C2B3A49"]
            for name, _, data in FIXTURE_PARTS:
                (Path(d) / name).write_bytes(data)
                args += ["--part", "%s=font:%s" % (name, Path(d) / name)]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ap.main(args), 0)
            data = (Path(d) / "letters.roboasset").read_bytes()
            self.assertEqual(len(data), 2 * 4096)
            self.assertEqual(data[:244], fixture_bytes())
            self.assertEqual(data[244:], b"\xff" * (len(data) - 244))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ap.main(["info", str(Path(d) / "letters.roboasset")]), 0)
            self.assertIn("name     letters", out.getvalue())
            self.assertIn("match    0x1c2b3a49", out.getvalue())
            self.assertIn("caption", out.getvalue())

    def test_reader_reads_the_fixture(self):
        info = ps.read_asset(ap.unsigned_file(fixture_bytes()))
        self.assertEqual((info.name, info.kind, info.version, info.match, info.draft,
                          info.payload_length, info.signing_key_sha256),
                         ("letters", 3, "0.9.0", FIXTURE_MATCH, False, 244, ""))
        self.assertEqual([(p.name, p.type, p.offset, p.size) for p in info.parts],
                         [("caption", 1, 224, 5), ("body", 1, 232, 8), ("title", 1, 240, 3)])

    def test_kinds_are_the_c_ones(self):
        h = (ROOT / "components" / "core" / "pack_store" / "include" / "pack_store"
             / "pack_store.h").read_text()
        for name, value in (("LETTERS", 3), ("PICTURE", 4), ("CERTS", 5), ("WEB", 6)):
            self.assertIn("PS_KIND_%s = %d" % (name, value), h)
            self.assertEqual(getattr(ps, "KIND_" + name), value)
        self.assertEqual(ps.ASSET_STORE_NAMES, {3: "letters", 4: "saver", 5: "certs", 6: "web"})


class RefusalTests(unittest.TestCase):
    """OPT-012: read_asset refuses what asset_pack_open() refuses (one or more per rule)."""

    def refused(self, payload, words=None):
        with self.assertRaises(ps.StoreError) as cm:
            ps.read_asset(ap.unsigned_file(payload))
        if words:
            self.assertRegex(str(cm.exception), words)
        with self.assertRaises(ps.StoreError):
            ps.read_asset_payload(payload)

    def test_envelope(self):
        f = ap.unsigned_file(fixture_bytes())
        for bad in (f[:-1], f + b"\xff", f[:4096], b"", f[:-4096] + f):
            with self.assertRaises(ps.StoreError):
                ps.read_asset(bad)
        with self.assertRaisesRegex(ps.StoreError, "padding"):
            ps.read_asset(patched(f, 300, b"\0"))
        with self.assertRaisesRegex(ps.StoreError, "length"):
            ps.read_asset(patched(f, 8, struct.pack("<I", 4097)))

    def test_every_truncation(self):
        fx = fixture_bytes()
        for n in range(len(fx)):
            self.refused(fx[:n])
            if n >= 12:
                self.refused(patched(fx[:n], 8, struct.pack("<I", n)))

    def test_header_fields(self):
        fx = fixture_bytes()
        self.refused(patched(fx, 0, b"RLNG"), "magic")
        self.refused(patched(fx, 4, struct.pack("<H", 2)), "format")
        self.refused(patched(fx, 6, struct.pack("<H", 127)), "format")
        for n in (0, 243, 245, 0xFFFFFFFF):
            self.refused(patched(fx, 8, struct.pack("<I", n)))
        for k in (0, 1, 2, 7, 255):
            self.refused(patched(fx, 12, bytes([k])), "kind")
        for k in (4, 5, 6):
            self.assertEqual(ps.read_asset_payload(patched(fx, 12, bytes([k]))).kind, k)
        for f in (2, 0x80, 0xFE):
            self.refused(patched(fx, 13, bytes([f])), "flags")
        self.assertTrue(ps.read_asset_payload(patched(fx, 13, b"\x01")).draft)
        for c in (0, 17, 0xFFFF):
            self.refused(patched(fx, 14, struct.pack("<H", c)), "parts")
        self.refused(patched(fx, 14, struct.pack("<H", 2)), "layout")
        self.refused(patched(fx, 14, struct.pack("<H", 16)), "shorter")
        for off in (0, 127, 129, 160):
            self.refused(patched(fx, 52, struct.pack("<I", off)), "parts offset")
        for i in (56, 100, 127):
            self.refused(patched(fx, i, b"\x01"), "reserved")
        self.assertEqual(ps.read_asset_payload(patched(fx, 48, b"\xff" * 4)).match, 0xFFFFFFFF)

    def name_field(self, at, words):
        fx = fixture_bytes()
        for raw in (b"\0" * 16, b"a" * 16, b"\0" * 15 + b"x", b"a b\0", b" \0", b"a\x1f\0",
                    b"a\x7f\0", b"a\xc3\xa9\0"):
            self.refused(patched(fx, at, raw), words)
        ps.read_asset_payload(patched(fx, at, b"~" * 15))

    def test_names_and_versions(self):
        self.name_field(16, "name")
        self.name_field(32, "version")
        self.name_field(128, "name")
        self.name_field(160, "name")

    def test_part_fields(self):
        fx = fixture_bytes()
        self.refused(patched(fx, 192, b"caption\0"), "repeats the name")
        for t in (0, 4, 0xFFFFFFFF):
            self.refused(patched(fx, 128 + 16, struct.pack("<I", t)), "type")
        self.assertEqual(ps.read_asset_payload(patched(fx, 128 + 16, struct.pack("<I", 3))).parts[0].type, 3)
        for off in (220, 223, 225, 228, 0, 0xFFFFFFFC):
            self.refused(patched(fx, 128 + 20, struct.pack("<I", off)), "layout")
        self.refused(patched(fx, 128 + 24, struct.pack("<I", 0)), "empty")
        for size in (4, 9, 0xFFFFFFFF):
            self.refused(patched(fx, 128 + 24, struct.pack("<I", size)), "layout")
        self.refused(patched(fx, 192 + 24, struct.pack("<I", 5)), "layout")
        self.refused(patched(fx, 192 + 24, struct.pack("<I", 2)), "alignment")
        for i in range(28, 32):
            self.refused(patched(fx, 160 + i, b"\x01"), "reserved")

    def test_layout(self):
        fx = fixture_bytes()
        self.refused(patched(fx, 160 + 20, struct.pack("<I", 228)), "layout")  # an overlap
        self.refused(patched(patched(fx, 128 + 20, struct.pack("<I", 232)), 160 + 20,
                             struct.pack("<I", 224)), "layout")  # swapped
        gap = bytearray(fx[:232] + b"\0" * 4 + fx[232:])
        gap[160 + 20:160 + 24] = struct.pack("<I", 236)
        gap[192 + 20:192 + 24] = struct.pack("<I", 244)
        gap[8:12] = struct.pack("<I", 248)
        self.refused(bytes(gap), "layout")
        for i in (229, 230, 231, 243):
            self.refused(patched(fx, i, b"\x01"), "alignment")
        self.refused(patched(fx + b"\0\0\0\0", 8, struct.pack("<I", 248)), "end")


class WriterTests(unittest.TestCase):
    """OPT-012: the writer's kinds, its refusals and the signed round trip."""

    def run_build(self, d, *args):
        with contextlib.redirect_stderr(io.StringIO()) as err, \
                contextlib.redirect_stdout(io.StringIO()):
            code = ap.main(["build", "--version", "1.0.0", "--out", str(d)] + list(args))
        return code, err.getvalue()

    def test_each_kind_writes_its_store_name(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "x").write_bytes(b"\xff\xd8\xff\xd9")
            self.assertEqual(self.run_build(d, "--kind", "picture", "--draft", "--part",
                                            "photo=jpeg:%s" % (Path(d) / "x"))[0], 0)
            info = ps.read_asset((Path(d) / "saver.roboasset").read_bytes())
            self.assertEqual((info.kind, info.name, info.draft, info.version), (4, "saver", True, "1.0.0"))
            self.assertEqual(self.run_build(d, "--kind", "certificates", "--part",
                                            "bundle=certs:%s" % (Path(d) / "x"))[0], 0)
            info = ps.read_asset((Path(d) / "certs.roboasset").read_bytes())
            self.assertEqual((info.kind, info.name, info.parts[0].type), (5, "certs", 3))

    def test_kind_rules_are_refused(self):
        with tempfile.TemporaryDirectory() as d:
            x = Path(d) / "x"
            x.write_bytes(b"abc")
            big = Path(d) / "big"
            big.write_bytes(b"\0" * (128 * 1024))
            big2 = Path(d) / "big2"
            big2.write_bytes(b"\0" * (256 * 1024))
            big4 = Path(d) / "big4"
            big4.write_bytes(b"\0" * (512 * 1024))
            bad = [
                ("--kind", "letters", "--part", "headline=font:%s" % x),
                ("--kind", "letters", "--part", "body=jpeg:%s" % x),
                ("--kind", "letters"),
                ("--kind", "letters", "--part", "body=font:%s" % x, "--part", "body=font:%s" % x),
                ("--kind", "picture", "--part", "image=jpeg:%s" % x),
                ("--kind", "picture", "--part", "photo=font:%s" % x),
                ("--kind", "picture", "--part", "photo=jpeg:%s" % x, "--match", "1"),
                ("--kind", "certificates", "--part", "bundle=certs:%s" % x,
                 "--part", "photo=jpeg:%s" % x),
                ("--kind", "certificates", "--part", "bundle=certs:%s" % big),  # over 128 KB
                ("--kind", "letters", "--part", "body=font:%s" % big2),  # over 256 KB
                ("--kind", "picture", "--part", "photo=jpeg:%s" % big4),  # over 512 KB
                ("--kind", "letters", "--part", "body=gif:%s" % x),
                ("--kind", "letters", "--part", "body=font"),
                ("--kind", "letters", "--part", "body=font:%s" % (Path(d) / "missing")),
                ("--kind", "letters", "--part", "body=font:%s" % (Path(d) / "empty")),
            ]
            (Path(d) / "empty").write_bytes(b"")
            for args in bad:
                code, err = self.run_build(d, *args)
                self.assertEqual(code, 2, args)
                self.assertIn("asset_pack:", err)
            self.assertFalse(list(Path(d).glob("*.roboasset")))
            code, _ = self.run_build(Path(d), "--kind", "letters", "--version", "1 0",
                                     "--part", "body=font:%s" % x)
            self.assertEqual(code, 2)
            code, _ = self.run_build(Path(d), "--kind", "letters", "--version", "x" * 16,
                                     "--part", "body=font:%s" % x)
            self.assertEqual(code, 2)

    def test_the_flashers_board_descriptions(self):  # FLS-034
        builtin = ROOT / "tools" / "flasher" / "roboflash" / "boards.json"
        version = json.loads(builtin.read_text(encoding="utf-8"))["version"]
        with tempfile.TemporaryDirectory() as d:
            code, err = self.run_build(d, "--kind", "boards", "--version", version, "--part", "boards=json:%s" % builtin)
            self.assertEqual(code, 0, err)
            data = (Path(d) / "boards.roboasset").read_bytes()
            info, doc = ps.read_boards_pack(data)
            self.assertEqual((info.kind, info.name, info.version, info.draft), (0x80, "boards", version, False))
            self.assertEqual(doc, json.loads(builtin.read_text(encoding="utf-8")))
            with self.assertRaises(ps.StoreError):
                ps.read_asset(data)                     # no panel takes it
            with self.assertRaises(ps.StoreError):
                ps.read_boards_pack((Path(d) / "boards.roboasset").read_bytes()[:-1])

            def part(name, doc_or_bytes):
                f = Path(d) / name
                f.write_bytes(doc_or_bytes if isinstance(doc_or_bytes, bytes) else json.dumps(doc_or_bytes).encode())
                return str(f)
            good = {"format": 1, "version": "2", "boards": {"crowpanel_p4": {"name": "x", "chip": "esp32p4",
                                                                            "flash_size": "16MB"}}}
            bad = [
                ("--version", "2", "--draft", "--part", "boards=json:%s" % part("a", good)),
                ("--version", "3", "--part", "boards=json:%s" % part("b", good)),           # the file says 2
                ("--version", "2", "--part", "board=json:%s" % part("c", good)),
                ("--version", "2", "--part", "boards=font:%s" % part("d", good)),
                ("--version", "2", "--part", "boards=json:%s" % part("e", dict(good, boards={}))),
                ("--version", "2", "--part", "boards=json:%s" % part("f", dict(good, format=0))),
                ("--version", "2", "--part", "boards=json:%s" % part("g", b"{not json")),
                ("--version", "2", "--match", "1", "--part", "boards=json:%s" % part("h", good)),
            ]
            for args in bad:
                code, err = self.run_build(Path(d) / "out", "--kind", "boards", *args)
                self.assertEqual(code, 2, args)
                self.assertIn("asset_pack:", err)

    def test_sixteen_parts_and_sizes_around_alignment(self):
        parts = [("p%d" % i, 1, bytes([i + 1]) * (i + 1)) for i in range(16)]
        info = ps.read_asset(ap.unsigned_file(ap.build_payload(6, "web", "1", parts)))
        self.assertEqual(len(info.parts), 16)
        self.assertTrue(all(p.offset % 4 == 0 for p in info.parts))
        with self.assertRaises(ValueError):
            ap.build_payload(6, "web", "1", parts + [("p16", 1, b"x")])

    @unittest.skipUnless(have_espsecure(), "espsecure is not importable")
    def test_signed_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            key = Path(d) / "k.pem"
            r = subprocess.run([sys.executable, "-m", "espsecure", "generate_signing_key", "--version",
                                "2", "--scheme", "rsa3072", str(key)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            for name, _, data in FIXTURE_PARTS:
                (Path(d) / name).write_bytes(data)
            out = Path(d) / "out"
            args = ["--kind", "letters", "--key", str(key), "--match", str(FIXTURE_MATCH)]
            for name, _, _ in FIXTURE_PARTS:
                args += ["--part", "%s=font:%s" % (name, Path(d) / name)]
            code, err = self.run_build(out, *args)
            self.assertEqual(code, 0, err)
            data = (out / "letters.roboasset").read_bytes()
            info = ps.read_asset(data)
            self.assertEqual(info.signing_key_sha256, key_digest_from_pem(key))
            self.assertNotEqual(info.signing_key_sha256, "")
            self.assertEqual(data[:244], patched(fixture_bytes(), 32, b"1.0.0"))
            r = subprocess.run([sys.executable, "-m", "espsecure", "verify_signature", "--version",
                                "2", "--keyfile", str(key), str(out / "letters.roboasset")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)


if __name__ == "__main__":
    unittest.main()
