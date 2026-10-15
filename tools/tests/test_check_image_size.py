"""docs/features/19 CYD-013, docs/features/33 step 10: a firmware image fits its OTA slot
(tools/check_image_size.py)."""
import importlib.util
import io
import json
import struct
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_image_size", TOOLS / "check_image_size.py")
cis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cis)

K = 1024


def entry(name, ptype, subtype, offset, size):
    return struct.pack("<2sBBII16sI", b"\xaa\x50", ptype, subtype, offset, size,
                       name.encode("ascii"), 0)


def table(parts, md5=True):
    """A binary partition table as gen_esp32part.py writes it: entries, MD5 entry, 0xFF."""
    blob = b"".join(entry(*p) for p in parts)
    if md5:
        blob += b"\xeb\xeb" + b"\xff" * 14 + b"\0" * 16
    return blob + b"\xff" * (0xC00 - len(blob))


# firmware/e32r40t/partitions.csv
E32_PARTS = [
    ("nvs", 1, 0x02, 0xD000, 0x10000),
    ("otadata", 1, 0x00, 0x1D000, 0x2000),
    ("phy_init", 1, 0x01, 0x1F000, 0x1000),
    ("ota_0", 0, 0x10, 0x20000, 1920 * K),
    ("ota_1", 0, 0x11, 0x200000, 1920 * K),
    ("eventlog", 1, 0x06, 0x3E0000, 128 * K),
]


def make_build(root, image_size, parts=E32_PARTS, write_image=True):
    build = Path(root) / "build"
    (build / "partition_table").mkdir(parents=True)
    (build / "partition_table" / "partition-table.bin").write_bytes(table(parts))
    (build / "project_description.json").write_text(json.dumps({
        "project_name": "alarm_e32r40t", "project_version": "0.6.0", "target": "esp32",
        "app_bin": "alarm_e32r40t.bin", "app_elf": "alarm_e32r40t.elf"}))
    if write_image:
        (build / "alarm_e32r40t.bin").write_bytes(b"\0" * image_size)
    return build


def run(argv):
    out = io.StringIO()
    with redirect_stdout(out):
        code = cis.main(argv)
    return code, out.getvalue()


class PartitionTable(unittest.TestCase):
    def test_reads_every_entry_and_stops_at_the_end(self):
        parts = cis.parse_partitions(table(E32_PARTS))
        self.assertEqual([p["name"] for p in parts],
                         ["nvs", "otadata", "phy_init", "ota_0", "ota_1", "eventlog"])
        self.assertEqual(parts[3]["offset"], 0x20000)
        self.assertEqual(parts[3]["size"], 1920 * K)

    def test_without_an_md5_entry(self):
        self.assertEqual(len(cis.parse_partitions(table(E32_PARTS, md5=False))), 6)

    def test_the_slot_is_the_smallest_app_partition(self):
        parts = cis.parse_partitions(table([
            ("factory", 0, 0x00, 0x10000, 1024 * K),
            ("ota_0", 0, 0x10, 0x110000, 1536 * K),
        ]))
        self.assertEqual(cis.app_slot(parts)["name"], "factory")

    def test_no_app_partition_is_an_error(self):
        with self.assertRaises(cis.CheckError):
            cis.app_slot(cis.parse_partitions(table(E32_PARTS[:3])))


class Verdict(unittest.TestCase):
    def test_limit_is_inclusive(self):
        self.assertEqual(cis.verdict(85, 100, 85.0), (85.0, True))
        self.assertFalse(cis.verdict(86, 100, 85.0)[1])


class CommandLine(unittest.TestCase):
    def test_an_image_within_the_limit_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, 1_000_000)
            code, out = run([str(build)])
        self.assertEqual(code, 0)
        self.assertIn("alarm_e32r40t 0.6.0 (esp32)", out)
        self.assertIn("1,000,000", out)
        self.assertIn("50.9 % of the slot", out)
        self.assertIn("`ota_0`", out)

    def test_an_image_over_the_limit_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, int(1920 * K * 0.90))
            code, out = run([str(build)])
        self.assertEqual(code, 1)
        self.assertIn("TOO BIG", out)

    def test_the_limit_can_be_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, int(1920 * K * 0.90))
            self.assertEqual(run([str(build), "--limit", "95"])[0], 0)

    def test_an_image_larger_than_the_slot_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, 1920 * K + 1)
            self.assertEqual(run([str(build), "--limit", "100"])[0], 1)

    def test_a_missing_build_fails_with_a_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out = run([str(Path(tmp) / "nothing")])
        self.assertEqual(code, 1)
        self.assertIn("build the firmware first", out)

    def test_a_missing_image_fails_with_a_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, 0, write_image=False)
            code, out = run([str(build)])
        self.assertEqual(code, 1)
        self.assertIn("alarm_e32r40t.bin is missing", out)

    def test_the_report_is_appended_to_the_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, 1_000_000)
            summary = Path(tmp) / "summary.md"
            summary.write_text("earlier\n")
            run([str(build), "--summary", str(summary)])
            text = summary.read_text()
        self.assertTrue(text.startswith("earlier\n### alarm_e32r40t"))
        self.assertIn("| Image `alarm_e32r40t.bin` |", text)

    def test_an_empty_summary_means_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, 1_000_000)
            self.assertEqual(run([str(build), "--summary", ""])[0], 0)

    def test_ram_without_a_map_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = make_build(tmp, 1_000_000)
            code, out = run([str(build), "--ram"])
        self.assertEqual(code, 0)
        self.assertIn("static RAM not reported", out)


class StaticRam(unittest.TestCase):
    SIZE_JSON = {"layout": [
        {"name": "Flash", "total": 0, "used": 1200000},
        {"name": "Flash Code", "total": 0, "used": 900000},
        {"name": "Flash Data", "total": 0, "used": 260000},
        {"name": "DIRAM", "total": 180000, "used": 90000},
        {"name": "IRAM", "total": 131072, "used": 65536},
    ]}

    def test_rows_leave_flash_out(self):
        self.assertEqual(cis.ram_rows(self.SIZE_JSON),
                         [("DIRAM", 90000, 180000), ("IRAM", 65536, 131072)])

    def test_rows_appear_in_the_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = cis.read_build(make_build(tmp, 1_000_000))
            text, ok = cis.report(build, 85.0, cis.ram_rows(self.SIZE_JSON))
        self.assertTrue(ok)
        self.assertIn("| Static DIRAM | 90,000 | of 180,000 (50.0 %) |", text)


if __name__ == "__main__":
    unittest.main()
