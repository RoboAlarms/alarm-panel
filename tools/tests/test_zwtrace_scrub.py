"""docs/features/36 "Test and acceptance": tools/zwtrace_scrub.py makes a recording of the panel's
Z-Wave link safe to commit as a host-test fixture (components/core/zwave_zones/test/traces)."""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
SPEC = importlib.util.spec_from_file_location("zwtrace_scrub", TOOLS / "zwtrace_scrub.py")
zs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(zs)

TRACES = ROOT / "components" / "core" / "zwave_zones" / "test" / "traces"
REAL = bytes.fromhex("d1e2f3a4")     # a home ID as a real module would have it (made up)
OTHER = bytes.fromhex("d5e6f7a8")    # the module's next, after a network reset
FAKE1 = bytes.fromhex("cafe0001")
FAKE2 = bytes.fromhex("cafe0002")
FAKE_ZAC93_HOME = bytes.fromhex("c0ffee01")


def frame(type_, func, payload):
    body = bytes([len(payload) + 3, type_, func]) + bytes(payload)
    chk = 0xFF
    for b in body:
        chk ^= b
    return bytes([0x01]) + body + bytes([chk])


def controller_id(home, node=1):
    return frame(0x01, 0x20, home + bytes([node]))


def recording(*entries, head=("# panelctl, RoboAlarms 0.9.0",)):
    """zwtrace text: entries as (dir, bytes) or raw lines (str)."""
    lines = ["zwtrace 1", *head]
    seq = 0
    for e in entries:
        if isinstance(e, str):
            lines.append(e)
            continue
        seq += 1
        lines.append("%d %d %s %s" % (seq, 100 * seq, e[0], e[1].hex()))
    return "\n".join(lines) + "\n"


def entries_of(text):
    return [v for k, v in zs.parse(text) if k == "entry"]


class HomeIds(unittest.TestCase):
    def test_the_home_id_is_replaced_wherever_it_appears(self):
        cmd = frame(0x00, 0x04, bytes([0x00, 0x05, 0x06, 0x71, 0x05]) + REAL + b"\x00")
        text = recording(("ctx", b"\x15"), ("tx", frame(0x00, 0x20, b"")), ("crx", b"\x06"),
                         ("rx", controller_id(REAL)), ("ctx", b"\x06"), ("rx", cmd),
                         ("text", b"\xfe" + REAL + b"\x00"))
        out, s = zs.scrub(text)
        self.assertNotIn(REAL.hex(), out)
        es = entries_of(out)
        self.assertEqual(controller_id(FAKE1), bytes(es[3].data))
        self.assertTrue(zs.good(es[5].data))            # its checksum recomputed
        self.assertEqual(FAKE1, bytes(es[5].data[9:13]))
        self.assertEqual(b"\xfe" + FAKE1 + b"\x00", bytes(es[6].data))
        self.assertEqual(3, s.places)
        # nothing else moved
        self.assertEqual([e.data for e in entries_of(text)][:3], [e.data for e in es][:3])
        self.assertEqual(entries_of(text)[5].data[:9], es[5].data[:9])
        self.assertEqual(entries_of(text)[5].data[13:-1], es[5].data[13:-1])

    def test_each_network_gets_its_own_fictional_home_id(self):
        text = recording(("rx", controller_id(REAL)), ("rx", controller_id(OTHER)),
                         ("rx", controller_id(REAL)))
        out, _ = zs.scrub(text)
        es = entries_of(out)
        self.assertEqual([FAKE1, FAKE2, FAKE1], [bytes(e.data[4:8]) for e in es])

    def test_a_damaged_answer_stays_damaged_and_a_cut_one_keeps_its_length(self):
        damaged = bytearray(controller_id(REAL))
        damaged[-1] ^= 0x5A                          # a bad checksum, as the line damaged it
        cut = controller_id(REAL)[:6]                # SOF LEN TYPE FUNC and two bytes of it
        text = recording(("rx", bytes(damaged)), ("ctx", b"\x15"), ("rx", controller_id(REAL)),
                         ("rx", cut))
        out, _ = zs.scrub(text)
        es = entries_of(out)
        self.assertNotIn(REAL.hex(), out)
        self.assertEqual(FAKE1, bytes(es[0].data[4:8]))
        self.assertFalse(zs.good(es[0].data))
        # damaged by the same difference as before
        self.assertEqual(0x5A, es[0].data[-1] ^ zs.checksum(es[0].data))
        self.assertEqual(6, len(es[3].data))
        self.assertEqual(FAKE1[:2], bytes(es[3].data[4:6]))

    def test_a_home_id_it_cannot_reach_refuses_the_file(self):
        # a frame cut off right after SOF whose next bytes are the home ID: outside any payload
        text = recording(("rx", controller_id(REAL)), ("rx", b"\x01" + REAL))
        with self.assertRaises(LookupError):
            zs.scrub(text)


class DeviceIdentifiers(unittest.TestCase):
    def test_smartstart_home_ids_are_replaced_and_keep_their_relation(self):
        nwi = bytes.fromhex("e1223344")               # DSK-derived: top bits 11, low bit 0
        auth = bytes.fromhex("21223345")              # the same DSK: top bits 00, low bit 1
        update = frame(0x00, 0x49, bytes([0x85, 0x00, 0x00]) + nwi + bytes([0x03, 0x04, 0x07, 0x01]))
        request = frame(0x00, 0x4A, bytes([0x48, 0x07]) + nwi + auth)
        out, s = zs.scrub(recording(("rx", update), ("tx", request)))
        es = entries_of(out)
        self.assertEqual(bytes.fromhex("c0d54b02"), bytes(es[0].data[7:11]))
        self.assertEqual(bytes.fromhex("c0d54b02"), bytes(es[1].data[6:10]))
        self.assertEqual(bytes.fromhex("00d54b03"), bytes(es[1].data[10:14]))
        self.assertTrue(zs.good(es[0].data) and zs.good(es[1].data))
        self.assertEqual(3, s.smartstart)

    def test_s2_public_keys_are_replaced(self):
        key = bytes(range(0x40, 0x60))
        blanked = b"\x00\x00" + bytes(range(0x42, 0x60))   # the PIN left out
        device = frame(0x00, 0x04, bytes([0x00, 0x02, 3 + 32, 0x9F, 0x08, 0x00]) + key + b"\xc4")
        pin_less = frame(0x00, 0x04, bytes([0x00, 0x02, 3 + 32, 0x9F, 0x08, 0x00]) + blanked)
        panel = frame(0x00, 0x13, bytes([0x02, 3 + 32, 0x9F, 0x08, 0x01]) + key + b"\x25\x07")
        out, s = zs.scrub(recording(("rx", device), ("rx", pin_less), ("tx", panel)))
        es = entries_of(out)
        self.assertEqual(zs.FAKE_KEY, bytes(es[0].data[10:42]))
        self.assertEqual(b"\x00\x00" + zs.FAKE_KEY[2:], bytes(es[1].data[10:42]))
        self.assertEqual(zs.FAKE_KEY, bytes(es[2].data[9:41]))
        self.assertTrue(all(zs.good(e.data) for e in es))
        self.assertEqual(3, s.keys)
        self.assertNotIn(key[4:].hex(), out)


    def test_clear_credentials_are_blanked(self):
        # ZW-030: an Entry Control PIN in the clear, directly and inside Supervision; a User Code
        # set sent to a lock; an ordinary report left as it was
        pin = b"4729" + bytes([0xFF]) * 12
        entry = bytes([0x6F, 0x01, 0x07, 0x02, 0x03, 16]) + pin
        direct = frame(0x00, 0x04, bytes([0x00, 0x05, len(entry)]) + entry + bytes([0xC4]))
        supervised = bytes([0x6C, 0x01, 0x05, len(entry)]) + entry
        wrapped = frame(0x00, 0x04, bytes([0x00, 0x05, len(supervised)]) + supervised)
        code = bytes([0x63, 0x01, 0x03, 0x01]) + b"4729"
        sent = frame(0x00, 0x13, bytes([0x07, len(code)]) + code + bytes([0x25, 0x07]))
        door = frame(0x00, 0x04, bytes([0x00, 0x05, 9, 0x71, 0x05, 0, 0, 0, 0xFF, 6, 0x16, 0]))
        out, s = zs.scrub(recording(("rx", direct), ("rx", wrapped), ("tx", sent), ("rx", door)))
        es = entries_of(out)
        self.assertNotIn(b"4729".hex(), out)
        self.assertEqual(3, s.credentials)
        self.assertTrue(all(zs.good(e.data) for e in es))
        self.assertEqual(bytes([0x6F, 0x01]), bytes(es[0].data[7:9]))  # which command stays
        self.assertEqual(door, bytes(es[3].data))


class Lines(unittest.TestCase):
    def test_only_harmless_comments_stay(self):
        text = recording(
            ("ctx", b"\x15"), "# panel restarted", "lost 2", "# at Pat's house, panel 7F3A9C",
            ("ctx", b"\x15"),
            head=("# panelctl, RoboAlarms 0.9.0", "# module firmware 1.50 sdk 7.22.1 chip 8",
                  "# captured 2026-09-27", "# serial 5C:CF:7F:00:11:22",
                  zs.MARK_PREFIX + " 0: an earlier run"))
        out, s = zs.scrub(text)
        lines = out.splitlines()
        self.assertEqual(["zwtrace 1", zs.MARK, "# panelctl, RoboAlarms 0.9.0",
                          "# module firmware 1.50 sdk 7.22.1 chip 8", "# captured 2026-09-27",
                          "1 100 ctx 15", "# panel restarted", "lost 2", "2 200 ctx 15"], lines)
        self.assertEqual(2, s.dropped)

    def test_scrubbing_twice_changes_nothing(self):
        text = recording(("rx", controller_id(REAL)), ("rx", controller_id(OTHER)),
                         ("text", REAL), "# note: bench in the garage")
        once, _ = zs.scrub(text)
        twice, s = zs.scrub(once)
        self.assertEqual(once, twice)
        self.assertEqual(0, s.places)

    def test_what_it_cannot_parse_is_refused(self):
        bad = [
            "", "zwtrace 2\n", "ZWTRACE 1\n", "# zwtrace 1\n",
            "zwtrace 1\n1 0 xx 06\n", "zwtrace 1\n1 0 tx 0103000\n", "zwtrace 1\n1 0 tx 01030g07\n",
            "zwtrace 1\n1 0 tx\n", "zwtrace 1\n1 0 tx 06 06\n", "zwtrace 1\n1 x tx 06\n",
            "zwtrace 1\n1 0 crx 0606\n", "zwtrace 1\nlost\n", "zwtrace 1\nlost many\n",
            "zwtrace 1\n1 0 rx " + "00" * 1025 + "\n",
        ]
        for text in bad:
            with self.subTest(text=text[:40]):
                with self.assertRaises(zs.Refused):
                    zs.scrub(text)
        zs.scrub("zwtrace 1\r\n\r\n1 0 tx 01030007FB\r\n")  # Windows line ends, upper case: fine


class CommandLine(unittest.TestCase):
    def run_main(self, text, raw=None):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.zwtrace", Path(d) / "out.zwtrace"
            src.write_bytes(raw if raw is not None else text.encode("utf-8"))
            err, out = io.StringIO(), io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                code = zs.main([str(src), str(dst)])
            written = dst.read_text(encoding="utf-8") if dst.exists() else None
            return code, written, err.getvalue(), out.getvalue()

    def test_it_writes_the_scrubbed_copy(self):
        text = recording(("rx", controller_id(REAL)))
        code, written, _, said = self.run_main(text)
        self.assertEqual(0, code)
        self.assertEqual(zs.scrub(text)[0], written)
        self.assertIn("1 home ID(s) found", said)

    def test_a_file_it_cannot_parse_writes_nothing(self):
        code, written, err, _ = self.run_main("zwtrace 1\n1 0 tx 0103000\n")
        self.assertEqual(2, code)
        self.assertIsNone(written)
        self.assertIn("line 2", err)
        code, written, err, _ = self.run_main("", raw=b"zwtrace 1\n\xff\xfe\n")
        self.assertEqual(2, code)
        self.assertIsNone(written)

    def test_a_home_id_left_behind_writes_nothing(self):
        code, written, err, _ = self.run_main(
            recording(("rx", controller_id(REAL)), ("rx", b"\x01" + REAL)))
        self.assertEqual(3, code)
        self.assertIsNone(written)
        self.assertIn("nothing written", err)


class Fixtures(unittest.TestCase):
    def test_every_fixture_went_through_it(self):
        files = sorted(TRACES.glob("*.zwtrace")) + sorted((TRACES / "bad").glob("*.zwtrace"))
        self.assertGreaterEqual(len(files), 5)
        for path in files:
            with self.subTest(fixture=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertEqual(zs.MARK, text.splitlines()[1])
                self.assertEqual(text, zs.scrub(text)[0])     # already scrubbed: unchanged
                self.assertNotIn(FAKE_ZAC93_HOME.hex(), text)  # the fake's own was replaced


if __name__ == "__main__":
    unittest.main()
