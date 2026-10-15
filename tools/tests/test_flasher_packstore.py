"""I18N-044 / I18N-045: the pack store's format in Python, against the frozen fixture the C code
reads too; pack files' headers; ESP-IDF's partition table."""
import hashlib
import importlib.util
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
from roboflash import testkit  # noqa: E402
from roboflash.imagefile import key_digest_from_pem  # noqa: E402

SPEC = importlib.util.spec_from_file_location("i18n", ROOT / "tools" / "i18n.py")
i18n = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(i18n)

FIXTURE_H = ROOT / "components" / "core" / "pack_store" / "test" / "pack_store_fixture_v1.h"
SECTORS = 640


def fixture_bytes():
    text = FIXTURE_H.read_text()
    text = text[text.index("PACK_STORE_FIXTURE_V1[4096]"):]
    data = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", text))
    assert len(data) == 4096
    return data


def entry(name, first, sectors, kind=1, flags=0, payload=100):
    return ps.Entry(kind, flags, first, sectors * 4096, payload,
                    hashlib.sha256(b"fixture " + name.encode()).digest(), name)


def blob(n=1, fill=0x5A):
    return bytes([fill]) * (4096 * n)


def garbage():
    return bytes((i * 37 + 11) & 0xFF for i in range(4096))


class FixtureTests(unittest.TestCase):
    """I18N-044: the same bytes the C code's test decodes."""

    def test_decode_field_by_field(self):
        seq, entries = ps.decode_table(fixture_bytes(), SECTORS)
        self.assertEqual(seq, 7)
        want = [(1, 1, 2, 5, 19877, "es-US"), (1, 0, 7, 3, 11234, "en-GB"),
                (2, 0, 12, 100, 401234, "en-US"), (9, 0x80, 200, 2, 8000, "future")]
        got = [(e.kind, e.flags, e.first_sector, e.length // 4096, e.payload_length, e.name)
               for e in entries]
        self.assertEqual(got, want)
        for e in entries:
            self.assertEqual(e.sha256, hashlib.sha256(b"fixture " + e.name.encode()).digest())

    def test_reencode_is_byte_identical(self):
        seq, entries = ps.decode_table(fixture_bytes(), SECTORS)
        self.assertEqual(ps.encode_table(entries, seq, SECTORS), fixture_bytes())

    def test_another_partition_size_or_damage_refuses_it(self):
        self.assertIsNone(ps.decode_table(fixture_bytes(), SECTORS + 1))
        for at in (0, 4, 6, 8, 12, 16, 28, 40, 100, 280):
            b = bytearray(fixture_bytes())
            b[at] ^= 0x01
            self.assertIsNone(ps.decode_table(bytes(b), SECTORS), at)
        self.assertIsNone(ps.decode_table(fixture_bytes()[:4095], SECTORS))

    def test_rules_refused_even_with_a_right_crc(self):
        def table(entries):
            return ps._encode(entries, 1, SECTORS)   # no validation: what a foreign writer could write
        ok = entry("es-US", 2, 5)
        for bad in (entry("a", 1, 1),                           # before sector 2
                    entry("a", 2, 0),                           # empty
                    entry("a", 639, 2),                         # past the partition
                    ps.Entry(1, 0, 2, 4097, 1, bytes(32), "a"),  # not whole sectors
                    entry("bad\x01", 10, 1),                    # not printable
                    entry("x" * 16, 10, 1)):                    # no NUL room
            try:
                t = table([ok, bad])
            except (ValueError, struct.error, UnicodeEncodeError):
                continue
            self.assertIsNone(ps.decode_table(t, SECTORS), bad)
        self.assertIsNone(ps.decode_table(table([ok, entry("b", 4, 3)]), SECTORS))   # overlap
        self.assertIsNone(ps.decode_table(table([ok, entry("es-US", 20, 1)]), SECTORS))   # repeat
        self.assertIsNotNone(ps.decode_table(table([ok, entry("es-US", 20, 1, kind=2)]), SECTORS))

    def test_encode_refuses_what_decode_would(self):
        with self.assertRaises(ValueError):
            ps.encode_table([entry("a", 2, 5), entry("b", 4, 3)], 1, SECTORS)
        with self.assertRaises(ValueError):
            ps.encode_table([entry("a", 1, 5)], 1, SECTORS)
        with self.assertRaises(ValueError):
            ps.encode_table([entry("%d" % i, 2 + i, 1) for i in range(64)], 1, SECTORS)
        empty = ps.encode_table([], 3, SECTORS)
        self.assertEqual(ps.decode_table(empty, SECTORS), (3, []))
        self.assertEqual(empty[32:], b"\xff" * 4064)


class MountTests(unittest.TestCase):
    def table(self, seq, *entries):
        return ps.encode_table(list(entries), seq, SECTORS)

    def test_empty_garbage_and_erased(self):
        for a, b in ((garbage(), garbage()), (b"\xff" * 4096, b"\xff" * 4096)):
            s = ps.Store.mount(a, b, SECTORS)
            self.assertEqual((s.sequence, s.current, s.entries), (0, -1, []))

    def test_higher_sequence_equal_and_single(self):
        a = self.table(4, entry("a", 2, 1))
        b = self.table(5, entry("b", 2, 1))
        s = ps.Store.mount(a, b, SECTORS)
        self.assertEqual((s.sequence, s.current, [e.name for e in s.entries]), (5, 1, ["b"]))
        s = ps.Store.mount(b, a, SECTORS)
        self.assertEqual((s.sequence, s.current), (5, 0))
        s = ps.Store.mount(a, self.table(4, entry("b", 2, 1)), SECTORS)
        self.assertEqual((s.current, s.entries[0].name), (0, "a"))        # equal: A
        s = ps.Store.mount(garbage(), a, SECTORS)
        self.assertEqual((s.current, s.sequence), (1, 4))
        s = ps.Store.mount(a, garbage(), SECTORS)
        self.assertEqual((s.current, s.sequence), (0, 4))


class AllocTests(unittest.TestCase):
    def store(self, *entries, sectors=20):
        return ps.Store(sectors, 1, 0, list(entries))

    def test_first_fit_with_holes(self):
        s = self.store(entry("a", 2, 2), entry("b", 6, 3))          # free: 4-5, 9-19
        self.assertEqual(s.alloc(4096), 4)
        self.assertEqual(s.alloc(2 * 4096), 4)
        self.assertEqual(s.alloc(3 * 4096), 9)
        self.assertEqual(s.alloc(11 * 4096), 9)
        with self.assertRaises(ValueError):
            s.alloc(12 * 4096)
        self.assertEqual(self.store().alloc(4096), 2)

    def test_no_space_and_bad_length(self):
        s = self.store(entry("a", 2, 18))
        with self.assertRaises(ValueError):
            s.alloc(4096)
        for n in (0, 100, -4096):
            with self.assertRaises(ValueError):
                s.alloc(n)

    def test_free_sectors(self):
        self.assertEqual(self.store(entry("a", 2, 5)).free_sectors(), 13)


class PlanTests(unittest.TestCase):
    def meta(self, name="es-US", flags=0):
        return ps.Entry(ps.KIND_LANGUAGE, flags, 0, 0, 77, b"", name)

    def apply(self, plan, flash):
        for off, data in plan.writes:
            flash[off:off + len(data)] = data

    def test_empty_store_blob_then_table_a(self):
        s = ps.Store(SECTORS)
        b = blob(3)
        plan = s.plan_put(self.meta(), b)
        self.assertEqual([(o, len(d)) for o, d in plan.writes], [(2 * 4096, 3 * 4096), (0, 4096)])
        self.assertEqual(plan.writes[0][1], b)
        seq, entries = ps.decode_table(plan.writes[1][1], SECTORS)
        self.assertEqual(seq, 1)
        e = entries[0]
        self.assertEqual((e.first_sector, e.length, e.payload_length, e.name, e.sha256),
                         (2, 3 * 4096, 77, "es-US", hashlib.sha256(b).digest()))
        self.assertEqual((plan.store.sequence, plan.store.current), (1, 0))
        self.assertEqual((s.sequence, s.current, s.entries), (0, -1, []))   # the old store is untouched

    def test_table_goes_to_the_copy_that_is_not_current(self):
        s = ps.Store(SECTORS, 5, 0, [entry("a", 2, 2)])
        plan = s.plan_put(self.meta("b"), blob())
        self.assertEqual(plan.writes[0][0], 4 * 4096)            # never over a current blob
        self.assertEqual(plan.writes[1][0], 4096)                # into B
        self.assertEqual(ps.decode_table(plan.writes[1][1], SECTORS)[0], 6)
        nxt = plan.store.plan_put(self.meta("c"), blob())
        self.assertEqual(nxt.writes[-1][0], 0)                   # and back to A
        self.assertEqual([e.name for e in nxt.store.entries], ["a", "b", "c"])

    def test_replace_goes_last_and_its_old_sectors_stay_untouched(self):
        s = ps.Store(SECTORS, 2, 1, [entry("es-US", 2, 2, flags=1), entry("en-GB", 4, 2)])
        plan = s.plan_put(self.meta("es-US", flags=1), blob(2))
        self.assertEqual(plan.writes[0][0], 6 * 4096)
        self.assertEqual([e.name for e in plan.store.entries], ["en-GB", "es-US"])
        self.assertEqual(plan.store.entries[-1].first_sector, 6)
        self.assertEqual(plan.store.entries[-1].flags, 1)        # flags as given
        self.assertEqual(plan.writes[1][0], 0)

    def test_overlap_and_full_are_refused(self):
        s = ps.Store(10, 1, 0, [entry("a", 2, 8)])
        with self.assertRaises(ValueError):
            s.plan_put(self.meta("b"), blob())                   # no room
        with self.assertRaises(ValueError):
            s.plan_put(self.meta("a"), blob())                   # replacing still needs fresh sectors
        full = ps.Store(200, 1, 0, [entry("%d" % i, 2 + i, 1) for i in range(63)])
        with self.assertRaises(ValueError):
            full.plan_put(self.meta("new"), blob())
        replaced = full.plan_put(self.meta("5"), blob())         # a 64th entry is only refused new
        self.assertEqual(len(replaced.store.entries), 63)
        with self.assertRaises(ValueError):
            s.plan_put(ps.Entry(0, 0, 0, 0, 0, b"", "x"), blob())   # no kind
        with self.assertRaises(ValueError):
            s.plan_put(self.meta("x" * 16), blob())
        with self.assertRaises(ValueError):
            s.plan_put(self.meta("ok"), b"\0" * 100)

    def test_plan_then_apply_then_mount_round_trips(self):
        flash = bytearray(b"\xff" * (SECTORS * 4096))
        s = ps.Store.mount(bytes(flash[:4096]), bytes(flash[4096:8192]), SECTORS)
        for name in ("es-US", "en-GB", "fr-CA"):
            plan = s.plan_put(self.meta(name), blob(2, len(name)))
            self.apply(plan, flash)
            s = plan.store
        m = ps.Store.mount(bytes(flash[:4096]), bytes(flash[4096:8192]), SECTORS)
        self.assertEqual([(e.name, e.first_sector) for e in m.entries],
                         [("es-US", 2), ("en-GB", 4), ("fr-CA", 6)])
        self.assertEqual((m.sequence, m.current), (3, 0))

    def test_default_sets_clears_and_keeps_other_kinds_and_flags(self):
        s = ps.Store(SECTORS, 7, 0, [entry("a", 2, 1, flags=1), entry("b", 3, 1, flags=0x80),
                                     entry("v", 4, 1, kind=2, flags=1), entry("z", 5, 1, flags=0x81)])
        plan = s.plan_default(1, "b")
        self.assertEqual(len(plan.writes), 1)
        self.assertEqual(plan.writes[0][0], 4096)
        flags = {e.name: e.flags for e in plan.store.entries}
        self.assertEqual(flags, {"a": 0, "b": 0x81, "v": 1, "z": 0x80})
        self.assertEqual(plan.store.sequence, 8)
        self.assertEqual(ps.decode_table(plan.writes[0][1], SECTORS)[0], 8)
        clear = s.plan_default(1, "")
        self.assertEqual({e.name: e.flags for e in clear.store.entries}["a"], 0)

    def test_default_with_nothing_to_change_writes_nothing(self):
        s = ps.Store(SECTORS, 7, 0, [entry("a", 2, 1, flags=1), entry("b", 3, 1)])
        plan = s.plan_default(1, "a")
        self.assertEqual(plan.writes, [])
        self.assertIs(plan.store, s)
        with self.assertRaises(ValueError):
            s.plan_default(1, "nope")

    def test_sequence_ffffffff_writes_one_then_erases_the_old_copy(self):
        s = ps.Store(SECTORS, 0xFFFFFFFF, 1, [entry("a", 2, 1)])
        plan = s.plan_put(self.meta("b"), blob())
        self.assertEqual(len(plan.writes), 3)
        self.assertEqual(plan.writes[1][0], 0)
        self.assertEqual(ps.decode_table(plan.writes[1][1], SECTORS)[0], 1)
        self.assertEqual(plan.writes[2], (4096, b"\xff" * 4096))
        self.assertEqual((plan.store.sequence, plan.store.current), (1, 0))
        d = ps.Store(SECTORS, 0xFFFFFFFF, 0, [entry("a", 2, 1, flags=1)]).plan_default(1, "")
        self.assertEqual([(o, len(x)) for o, x in d.writes], [(4096, 4096), (0, 4096)])


class PackFileTests(unittest.TestCase):
    def make(self, locale="fr-CA", version="0.7.0", draft=False):
        cat = i18n.fixture_catalog()
        payload, keys = i18n.fixture_payload()
        if (locale, version, draft) != ("fr-CA", "0.7.0", False):
            payload = i18n.build_pack_payload(locale, i18n.FIXTURE_NAME, cat.rule, draft,
                                              i18n.catalog_layout(cat), keys, version)
        return payload, i18n.unsigned_pack_file(payload)

    def test_fields(self):
        payload, f = self.make()
        info = ps.read_pack(f)
        self.assertEqual((info.locale, info.name, info.version, info.draft, info.payload_length),
                         ("fr-CA", "Français", "0.7.0", False, len(payload)))
        self.assertEqual(info.signing_key_sha256, "")
        self.assertEqual(info.missing, 2)
        self.assertEqual(info.template_fingerprint, struct.unpack_from("<I", payload, 80)[0])
        self.assertTrue(ps.read_pack(self.make(draft=True)[1]).draft)

    def test_signature_sector_digest_matches_the_key(self):
        _, f = self.make()
        f = f[:-4096] + testkit.signature_sector()
        self.assertEqual(ps.read_pack(f).signing_key_sha256,
                         ps.signature_key_digest(testkit.synthetic_app()))

    def test_signed_with_espsecure(self):
        try:
            import espsecure  # noqa: F401
        except ImportError:
            self.skipTest("espsecure is not importable")
        payload, _ = self.make()
        with tempfile.TemporaryDirectory() as d:
            key, src, out = Path(d) / "k.pem", Path(d) / "p.bin", Path(d) / "p.robolang"
            r = subprocess.run([sys.executable, "-m", "espsecure", "generate_signing_key", "--version",
                                "2", "--scheme", "rsa3072", str(key)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            src.write_bytes(payload)
            r = subprocess.run([sys.executable, "-m", "espsecure", "sign_data", "--version", "2",
                                "--keyfile", str(key), "--output", str(out), str(src)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            info = ps.read_pack(out.read_bytes())
            self.assertEqual(info.signing_key_sha256, key_digest_from_pem(key))
            self.assertNotEqual(info.signing_key_sha256, "")
            self.assertEqual(info.locale, "fr-CA")

    def test_bad_files_are_refused_in_words(self):
        payload, f = self.make()

        def bad(data, words):
            with self.assertRaisesRegex(ValueError, words):
                ps.read_pack(data)
        bad(f[:-1], "sector")
        bad(f[:4096], "sector")
        bad(b"", "sector")
        bad(b"XXXX" + f[4:], "magic")
        bad(f[:4] + b"\x02\x00" + f[6:], "format")
        bad(f[:6] + b"\x40\x00" + f[8:], "header size")
        bad(f[:8] + struct.pack("<I", len(f) - 4096 + 1) + f[12:], "length")
        bad(f[:8] + struct.pack("<I", 64) + f[12:], "length")
        mod = bytearray(f)
        mod[len(payload)] = 0
        bad(bytes(mod), "padding")
        for at, words in ((12, "locale"), (28, "name"), (60, "plural"), (61, "plural"), (62, "plural"),
                          (88, "version"), (112, "reserved")):
            mod = bytearray(f)
            mod[at:at + 1] = b"\x07" if at in (60, 61, 62, 112) else b"\x00" if at in (12, 28, 88) else b"\x01"
            bad(bytes(mod), words)
        mod = bytearray(f)
        mod[12:28] = b"a".ljust(16, b"\0")          # one character: not a tag
        bad(bytes(mod), "locale")
        mod = bytearray(f)
        mod[28:32] = b"\xff\xfe\x00\x00"
        bad(bytes(mod), "name")
        mod = bytearray(f)
        mod[88:104] = b"1.0\x01".ljust(16, b"\0")
        bad(bytes(mod), "version")


def partition_table(*parts):
    out = b""
    for typ, sub, off, size, label in parts:
        out += b"\xaa\x50" + bytes([typ, sub]) + struct.pack("<II", off, size) \
            + label.encode().ljust(16, b"\0") + struct.pack("<I", 0)
    return out


class PartitionTests(unittest.TestCase):
    def test_find_by_label(self):
        t = partition_table((1, 2, 0x9000, 0x6000, "nvs"), (0, 0x10, 0x30000, 0x400000, "ota_0"),
                            (1, 0x40, 0x830000, 0x200000, "assets"))
        t += b"\xeb\xeb" + b"\xff" * 14 + b"\x00" * 16          # the MD5 entry
        t += partition_table((0, 0, 1, 1, "assets"))            # after the end: never found
        t = t.ljust(3072, b"\xff")
        self.assertEqual(ps.find_partition(t, "assets"), (0x830000, 0x200000))
        self.assertEqual(ps.find_partition(t, "nvs"), (0x9000, 0x6000))
        self.assertIsNone(ps.find_partition(t, "storage"))
        self.assertIsNone(ps.find_partition(t, "asset"))

    def test_empty_and_erased(self):
        self.assertIsNone(ps.find_partition(b"", "assets"))
        self.assertIsNone(ps.find_partition(b"\xff" * 3072, "assets"))


if __name__ == "__main__":
    unittest.main()
