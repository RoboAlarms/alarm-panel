"""docs/features/39 OPT-003: where a firmware image's bytes go (tools/size_report.py), on a small
frozen linker map (fixtures/size_report.map)."""
import importlib.util
import io
import json
import struct
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("size_report", TOOLS / "size_report.py")
sr = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sr)

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "size_report.map"
SLOT = 0x10000


def partition_table():
    entry = struct.pack("<2sBBII16sI", b"\xaa\x50", 0, 0x10, 0x10000, SLOT, b"ota_0", 0)
    blob = entry + b"\xeb\xeb" + b"\xff" * 14 + b"\0" * 16
    return blob + b"\xff" * (0xC00 - len(blob))


def make_build(root, name, map_text, version="1.0"):
    """A build folder as ESP-IDF leaves it, around the given map text."""
    build = Path(root) / name
    (build / "partition_table").mkdir(parents=True)
    (build / "partition_table" / "partition-table.bin").write_bytes(partition_table())
    (build / "fixture.bin").write_bytes(b"\0" * 5000)
    (build / "fixture.map").write_text(map_text, encoding="utf-8")
    (build / "project_description.json").write_text(json.dumps({
        "project_name": "fixture", "project_version": version, "target": "esp32p4",
        "app_bin": "fixture.bin", "app_elf": "fixture.elf"}), encoding="utf-8")
    return build


def run(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        code = sr.main([str(a) for a in argv])
    return code, out.getvalue()


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.rows = sr.parse_map(FIXTURE)
        self.per, self.merged, self.pre, self.sections = sr.attribute(self.rows)

    def test_discarded_sections_before_the_memory_map_are_not_counted(self):
        self.assertNotIn("unused_helper", " ".join(s for _, s, _ in self.sections))
        self.assertTrue(all(r[0] is not None for r in self.rows))

    def test_wrapped_section_name_takes_its_numbers_from_the_next_line(self):
        wrapped = [s for s in self.sections if s[1].startswith(".text.a_function_with")]
        self.assertEqual([s[2] for s in wrapped], [0x80])

    def test_merged_strings_are_charged_back_by_pre_merge_size(self):
        self.assertEqual((self.merged, self.pre), (0x60, 0x80))
        strings = {o: v[2] for o, v in self.per.items()}
        ui = "esp-idf/alarm_ui/libalarm_ui.a(ui_main.c.obj)"
        core = "esp-idf/alarm_core/libalarm_core.a(core.c.obj)"
        lvgl = "esp-idf/lvgl__lvgl/liblvgl__lvgl.a(lv_obj.c.obj)"
        self.assertAlmostEqual(strings[ui], 0x40 * 0.75)
        self.assertAlmostEqual(strings[core], 0x20 * 0.75)
        self.assertAlmostEqual(strings[lvgl], 0x20 * 0.75)
        self.assertAlmostEqual(sum(strings.values()), self.merged)

    def test_ram_loaded_data_counts_and_bss_does_not(self):
        core = self.per["esp-idf/alarm_core/libalarm_core.a(core.c.obj)"]
        self.assertEqual(core[3], 0x10)  # .dram0.data
        self.assertEqual(core[0], 0x100)
        total = sum(sum(v) for v in self.per.values())
        self.assertAlmostEqual(total, 0x100 + 0x300 + 0x80 + 0x200 + 0x400 + 0x60 + 0x10 + 4)

    def test_backslash_in_an_archive_path_is_cut(self):
        self.assertEqual(sr.split_object("toolchain/lib\\libc.a(libc_environ.c.o)"),
                         ("libc.a", "libc_environ.c.o"))


class CategoryTests(unittest.TestCase):
    def test_two_archives_land_in_different_categories(self):
        per, *_ = sr.attribute(sr.parse_map(FIXTURE))
        agg = sr.category_table(per)
        self.assertAlmostEqual(sum(agg["Alarm core (ours)"]), 0x100 + 0x10 + 0x20 * 0.75)
        self.assertAlmostEqual(sum(agg["LVGL"]), 0x200 + 0x20 * 0.75)
        self.assertAlmostEqual(sum(agg["UI: text and digit fonts"]), 0x400)
        self.assertAlmostEqual(sum(agg["UI: other hand-written screens"]),
                               0x300 + 0x80 + 0x40 * 0.75)
        self.assertEqual(sum(agg["C library, console, NVS"]), 4)

    def test_an_unlisted_archive_is_esp_idf_itself(self):
        self.assertEqual(sr.categorize("libnew_thing.a", "x.obj"), sr.FALLBACK)

    def test_our_archives_come_from_the_component_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "core" / "alarm_core").mkdir(parents=True)
            (Path(tmp) / "ui" / "alarm_ui").mkdir(parents=True)
            self.assertEqual(sr.our_archives(tmp),
                             {"libmain.a", "libalarm_core.a", "libalarm_ui.a"})


class FontTests(unittest.TestCase):
    SRC = """
static const uint8_t glyph_bitmap[] = {
  /* U+0020 */ 0x00, 0x00,
  /* U+0041 */ 0x11, 0x22, 0x33,
  /* U+00E9 */ 0x44,
  /* U+20AC */ 0x55, 0x66, 0x77, 0x88
};
static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
  {.bitmap_index = 0}, {.bitmap_index = 0}, {.bitmap_index = 2},
  {.bitmap_index = 5}, {.bitmap_index = 6}
};
static const uint16_t unicode_list_2[] = {0x0, 0x1};
static const lv_font_fmt_txt_cmap_t cmaps[] = {
  {.range_start = 32, .range_length = 1, .glyph_id_start = 1},
  {.range_start = 65, .range_length = 1, .glyph_id_start = 2},
  {.range_start = 233, .range_length = 1, .glyph_id_start = 3},
  {.range_start = 8364, .range_length = 1, .glyph_id_start = 4}
};
"""

    def test_bytes_follow_the_cmaps_not_assumed_counts(self):
        glyphs, nbytes, per = sr.font_blocks(self.SRC)
        self.assertEqual((glyphs, nbytes), (4, 10))
        # glyph sizes 2, 3, 1, 4 bytes, plus an 8-byte record each
        self.assertEqual(per, [(2 + 8) + (3 + 8), 1 + 8, 0, 4 + 8])

    def test_fonts_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "font_x_data.c").write_text(self.SRC, encoding="utf-8")
            rows = sr.fonts_report(tmp)
        self.assertEqual([r[0] for r in rows], ["font_x_data.c"])


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        (root / "components" / "core" / "alarm_core").mkdir(parents=True)
        (root / "components" / "ui" / "alarm_ui").mkdir(parents=True)
        self.components = root / "components"
        text = FIXTURE.read_text(encoding="utf-8")
        self.a = make_build(root, "a", text, "1.0")
        # variant B: ui_build grew by 0x100 and the string pool is the same
        self.b = make_build(root, "b", text.replace("0x300 esp-idf/alarm_ui", "0x400 esp-idf/alarm_ui")
                            .replace("0x700\n", "0x800\n"), "1.1")

    def test_report_prints_categories_with_percent_of_slot(self):
        code, out = run(self.a, "--components", self.components)
        self.assertEqual(code, 0)
        self.assertIn("LVGL", out)
        self.assertIn("Our biggest functions and data", out)
        self.assertIn("ui_build", out)
        self.assertIn("libalarm_ui.a(ui_main.c.obj)", out)
        self.assertNotIn("esp-idf/", out)
        total = [ln for ln in out.splitlines() if ln.startswith("TOTAL")][0]
        self.assertIn("4.3", total.split())  # 2,804 bytes of a 65,536 slot is 2.5 %

    def test_markdown(self):
        code, out = run(self.a, "--components", self.components, "--markdown")
        self.assertEqual(code, 0)
        self.assertIn("| category | KB | % slot |", out)
        self.assertIn("|---|---:|", out)

    def test_compare_shows_the_delta(self):
        code, out = run(self.b, "--components", self.components, "--compare", self.a)
        self.assertEqual(code, 0)
        self.assertIn("delta KB", out)
        self.assertIn("Objects that changed most", out)
        changed = [ln for ln in out.splitlines() if ln.startswith("libalarm_ui.a(ui_main.c.obj)")]
        self.assertEqual(len(changed), 1)
        self.assertTrue(changed[0].rstrip().endswith("+0.3"))  # 256 bytes is 0.3 KB

    def test_missing_build_exits_2(self):
        code, out = run(Path(self.tmp.name) / "nothing")
        self.assertEqual(code, 2)
        self.assertIn("FAIL", out)

    def test_missing_map_exits_2(self):
        (self.a / "fixture.map").unlink()
        code, _ = run(self.a)
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
