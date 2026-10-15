"""docs/features/37 I18N-003, BL-001 U17: the layouts' literal XML text goes through alarm_ui_tr()
in the generated code, and tools/i18n.py extracts exactly the same texts."""
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("lvpro_gen")
i18n = load("i18n")

TR_CALL = re.compile(r'alarm_ui_tr(?:_c\("(?:[^"\\]|\\.)*",\s*|\()((?:"(?:[^"\\]|\\.)*"\s*)+)\)')


class IsUiText(unittest.TestCase):
    def test_words_are_text(self):
        for v in ("Zones", "Wi-Fi", "Arm Away", "Time zone, 24-hour clock", "PM", "km/h"):
            self.assertTrue(gen.is_ui_text(v), v)

    def test_not_text(self):
        for v in (None, "", "60 s", "80%", "%d", "12", "$title", "#accent", "", "",
                  "a", "1 x 2"):
            self.assertFalse(gen.is_ui_text(v), v)

    def test_a_comment_gives_a_context(self):
        # docs/features/37: <!-- tr-context: ... --> before an element, which LVGL Pro ignores
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "x.xml"
            path.write_text('<screen><view>\n<!-- a note -->\n<!-- tr-context: zone filter -->\n'
                            '<segment text="Open"/>\n<segment text="All"/>\n</view></screen>',
                            encoding="utf-8")
            root = gen.parse_xml(path)
            view = root.find("view")
            self.assertEqual([c.tag for c in view], ["segment", "segment"])  # comments gone
            self.assertEqual(gen.TR_CTX.get(view[0]), "zone filter")
            self.assertIsNone(gen.TR_CTX.get(view[1]))  # only the next element
        self.assertEqual(gen.tr_string("Open", "zone filter"), 'alarm_ui_tr_c("zone filter", "Open")')

    def test_tr_string_wraps_only_text(self):
        self.assertEqual(gen.tr_string("Cancel"), 'alarm_ui_tr("Cancel")')
        self.assertEqual(gen.tr_string("60 s"), '"60 s"')
        self.assertEqual(gen.tr_string(""), '"\\xEE\\x81\\xAF"')
        self.assertEqual(gen.tr_string("Installer · Tap"), 'alarm_ui_tr("Installer \\xC2\\xB7 Tap")')


class TextFontsAreMutable(unittest.TestCase):
    """docs/features/37 I18N-049: the five text fonts are not const, so a language pack's font can
    be hung behind each as its fallback; the digit, keypad and icon fonts stay const."""

    OLD = ("x\n#if LVGL_VERSION_MAJOR >= 8\nconst lv_font_t font_body_data = {\n#else\n"
           "lv_font_t font_body_data = {\n#endif\n    .fallback = NULL,\n")

    def test_one_plain_definition_and_idempotent(self):
        once = gen.font_make_mutable(self.OLD)
        self.assertIn("\nlv_font_t font_body_data = {\n    .fallback = NULL,", once)
        self.assertNotIn("const lv_font_t", once)
        self.assertNotIn("#if LVGL", once)
        self.assertEqual(gen.font_make_mutable(once), once)

    def test_the_committed_files_and_declarations(self):
        for layout in ("1024x600", "480x320"):
            base = ROOT / "components/ui/alarm_ui/layouts" / layout / "alarm_ui"
            for role in gen.TEXT_FONTS:
                text = (base / "fonts" / (role + "_data.c")).read_text(encoding="utf-8")
                self.assertRegex(text, r"\nlv_font_t %s_data = \{" % role)
                self.assertNotIn("const lv_font_t %s_data" % role, text)
            decl = (base / "alarm_ui_gen.c").read_text(encoding="utf-8")
            for role in gen.TEXT_FONTS:
                self.assertIn("extern lv_font_t %s_data;" % role, decl)
            self.assertIn("extern const lv_font_t font_digits_data;", decl)


class FontCharacters(unittest.TestCase):
    """docs/features/37 I18N-011: a text font carries Latin-1, Latin Extended-A and every
    character the translation catalogs use; icon and digit fonts are left alone."""

    def test_ranges(self):
        self.assertEqual(gen.font_ranges("0x20-0x22,0xE038"), {0x20, 0x21, 0x22, 0xE038})
        self.assertEqual(gen.font_ranges(None), set())

    def test_text_fonts_get_the_catalogs_characters(self):
        with tempfile.TemporaryDirectory() as tmp:
            chars = Path(tmp) / "i18n_chars.txt"
            chars.write_text("aé€ж\n", encoding="utf-8")
            saved = gen.I18N_CHARS
            gen.I18N_CHARS = chars
            try:
                text = {"range": "0x20-0x7E,0xA0-0x17F", "symbols": "·…"}
                # a, é are in the ranges; the euro sign and zhe are added once
                self.assertEqual(gen.font_symbols(text), "·…€ж")
                digits = {"range": "0x30-0x39"}
                self.assertEqual(gen.font_symbols(digits), "")
                icons = {"range": "0xE038,0xE053"}
                self.assertEqual(gen.font_symbols(icons), "")
            finally:
                gen.I18N_CHARS = saved

    def test_the_seven_inch_text_fonts_keep_ascii_and_the_letters_pack_has_the_rest(self):
        """OPT-010: the image's text fonts hold ASCII (and symbols, the catalogs' characters); the
        four with a `letters` attribute get U+00A0-U+017F from the letters pack, the headline none."""
        p = gen.Project(ROOT / "components/ui/alarm_ui/layouts/1024x600/alarm_ui")
        text = [f for f in p.fonts if f["name"].startswith("font_") and 0x41 in gen.font_ranges(f.get("range"))]
        self.assertEqual({f["name"] for f in text},
                         {"font_caption", "font_body", "font_label", "font_title", "font_headline"})
        for f in text:
            self.assertEqual(gen.font_ranges(f["range"]), set(range(0x20, 0x7F)), f["name"])
        self.assertEqual({f["name"] for f in gen.letters_fonts(p)},
                         {"font_caption", "font_body", "font_label", "font_title"})


class LettersFonts(unittest.TestCase):
    """docs/features/39 OPT-010: the letters pack's fonts and their fingerprint."""

    def project(self, tmp, letters="0xA0-0xA5", rng="0x20-0x22"):
        base = Path(tmp)
        (base / "fonts").mkdir()
        (base / "fonts" / "a.ttf").write_bytes(b"first typeface")
        (base / "project.xml").write_text("<project/>", encoding="utf-8")
        (base / "globals.xml").write_text(
            '<globals><fonts><bin name="font_body" src_path="fonts/a.ttf" size="19" bpp="4" '
            'range="%s" letters="%s" symbols="\u00b7" as_file="false"/>'
            '<bin name="font_x" src_path="fonts/a.ttf" size="9" as_file="false"/></fonts></globals>'
            % (rng, letters), encoding="utf-8")
        return gen.Project(base)

    def test_ranges_text(self):
        self.assertEqual(gen.ranges_text({0xA0, 0xA1, 0xA2, 0xA5}), "0xA0-0xA2,0xA5")
        self.assertEqual(gen.ranges_text(set()), "")

    def test_the_letters_are_what_the_image_font_lacks(self):
        saved = gen.I18N_CHARS
        with tempfile.TemporaryDirectory() as tmp:
            chars = Path(tmp) / "c.txt"
            chars.write_text("\u00a1\n", encoding="utf-8")  # U+00A1 comes from the catalogs
            gen.I18N_CHARS = chars
            try:
                fo = {"range": "0x20-0x7E", "letters": "0xA0-0xA5", "symbols": "\u00a3"}
                # the symbol U+00A3 and the catalog character U+00A1 stay in the image
                self.assertEqual(gen.letters_set(fo), {0xA0, 0xA2, 0xA4, 0xA5})
                self.assertEqual(gen.letters_set({"range": "0x20-0x7E"}), set())
            finally:
                gen.I18N_CHARS = saved

    def test_fnv1a(self):
        self.assertEqual(gen.fnv1a32(b""), 0x811C9DC5)
        self.assertEqual(gen.fnv1a32(b"a"), 0xE40C292C)
        self.assertEqual(gen.fnv1a32(b"foobar"), 0xBF9CF968)

    def test_fingerprint_line_format(self):
        import hashlib
        with tempfile.TemporaryDirectory() as tmp:
            p = self.project(tmp)
            fo = p.fonts[0]
            letters = gen.ranges_text(gen.letters_set(fo))
            line = "font_body|%s|19|4|0|%s\n" % (hashlib.sha256(b"first typeface").hexdigest(), letters)
            self.assertEqual(gen.letters_fingerprint(p), gen.fnv1a32(line.encode("utf-8")))

    def test_fingerprint_follows_the_typeface_the_size_and_the_letters(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = self.project(tmp)
            base = gen.letters_fingerprint(p)
            self.assertEqual(gen.letters_fingerprint(p), base)
            (Path(tmp) / "fonts" / "a.ttf").write_bytes(b"second typeface")
            self.assertNotEqual(gen.letters_fingerprint(p), base)
        with tempfile.TemporaryDirectory() as tmp:
            self.assertNotEqual(gen.letters_fingerprint(self.project(tmp, letters="0xA0-0xA6")), base)
        with tempfile.TemporaryDirectory() as tmp:
            p = self.project(tmp)
            p.fonts[0]["size"] = "20"
            self.assertNotEqual(gen.letters_fingerprint(p), base)

    def test_letters_json_and_the_c_constant(self):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            p = self.project(tmp)
            files = gen.generate(p)
            doc = json.loads(files["fonts/letters/letters.json"])
            self.assertEqual(doc["format"], 1)
            self.assertEqual(doc["fonts"], {"body": "font_body.bin"})
            self.assertEqual(doc["fingerprint"], "0x%08x" % gen.letters_fingerprint(p))
            self.assertIn("const uint32_t alarm_ui_letters_fingerprint = %su;" % doc["fingerprint"],
                          files[p.name + "_gen.c"])
            self.assertIn("extern const uint32_t alarm_ui_letters_fingerprint;", files[p.name + "_gen.h"])

    def test_no_letters_fonts_no_files(self):
        p = gen.Project(ROOT / "components/ui/alarm_ui/layouts/480x320/alarm_ui")
        files = gen.generate(p)
        self.assertNotIn("fonts/letters/letters.json", files)
        self.assertNotIn("alarm_ui_letters_fingerprint", files["alarm_ui_gen.c"])

    def test_the_committed_files_agree_with_globals_xml(self):
        """What --check holds: letters.json and the C constant match globals.xml and the TTFs, and
        every listed .bin exists."""
        import json
        base = ROOT / "components/ui/alarm_ui/layouts/1024x600/alarm_ui"
        p = gen.Project(base)
        doc = json.loads((base / "fonts/letters/letters.json").read_text(encoding="utf-8"))
        self.assertEqual(doc["fingerprint"], "0x%08x" % gen.letters_fingerprint(p))
        self.assertIn(doc["fingerprint"] + "u;", (base / "alarm_ui_gen.c").read_text(encoding="utf-8"))
        for name in doc["fonts"].values():
            self.assertTrue((base / "fonts/letters" / name).stat().st_size > 0, name)

    def test_check_catches_a_changed_range(self):
        base = ROOT / "components/ui/alarm_ui/layouts/1024x600/alarm_ui"
        p = gen.Project(base)
        p.fonts[0]["letters"] = "0xA0-0x17E"
        self.assertNotEqual(gen.generate(p)["fonts/letters/letters.json"],
                            (base / "fonts/letters/letters.json").read_text(encoding="utf-8"))


class FontLine(unittest.TestCase):
    """docs/features/39 OPT-010: a text font's line is measured over its `line` code points, so the
    layout doesn't change when letters leave the image for the letters pack."""

    C_FONT = ("    .line_height = 18,          /*The maximum line height required by the font*/\n"
              "    .base_line = 3,             /*Baseline measured from the bottom of the line*/\n"
              "#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)\n")

    def test_the_line_set_is_written_into_the_c_font(self):
        out = gen.font_set_line(self.C_FONT, 19, 4)  # the set's ascent 15, descent 4
        self.assertIn(".line_height = 19,          %s\n" % gen.LINE_NOTE, out)
        self.assertIn("    .base_line = 4,             /*Baseline", out)
        self.assertTrue(out.endswith("#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)\n"))

    def test_the_font_s_own_glyphs_win_when_taller(self):
        # own: ascent 15, descent 3; the set: ascent 14, descent 5 -> ascent 15, descent 5
        out = gen.font_set_line(self.C_FONT, 19, 5)
        self.assertIn(".line_height = 20,", out)
        self.assertIn(".base_line = 5,", out)

    def test_once_set_it_reads_the_same_again(self):
        once = gen.font_set_line(self.C_FONT, 19, 4)
        self.assertEqual(gen.font_set_line(once, 19, 4), once)

    def test_the_committed_text_fonts_keep_the_full_latin_line(self):
        """What --check holds: every font with `line` carries the measured line (and the 1024x600
        text fonts the line they had with U+00A0-U+017F in the image)."""
        base = ROOT / "components/ui/alarm_ui/layouts/1024x600/alarm_ui"
        p = gen.Project(base)
        expect = {"font_caption": (19, 4), "font_body": (26, 5), "font_label": (26, 5),
                  "font_title": (33, 6), "font_headline": (52, 10)}
        for fo in p.fonts:
            if not fo.get("line"):
                continue
            text = (base / "fonts" / ("%s_data.c" % fo["name"])).read_text(encoding="utf-8")
            self.assertIn(gen.LINE_NOTE, text, fo["name"])
            m = gen.LINE_RE.search(text)
            self.assertEqual((int(m.group(2)), int(m.group(4))), expect[fo["name"]], fo["name"])
        self.assertEqual({fo["name"] for fo in p.fonts if fo.get("line")}, set(expect))


class Generated(unittest.TestCase):
    def test_value_routes_string_type_only(self):
        p = gen.Project.__new__(gen.Project)
        p.consts = {}
        f = gen.Func(p, {"title": "string"}, [], "t")
        self.assertEqual(f.value("Open", "string"), 'alarm_ui_tr("Open")')
        self.assertEqual(f.value("$title", "string"), "title")
        self.assertEqual(f.value("60 s", "string"), '"60 s"')
        self.assertEqual(f.value("10", "int"), "10")

    def test_roller_never_animates_to_a_value(self):
        # BL-131 finding 3, UI-020: a roller's attributes, its selection set without animation
        p = gen.Project.__new__(gen.Project)
        p.consts = {"roller_h": ("int", "240", None)}
        p.components = {}
        p.subject_names = set()
        p.global_style_names = set()
        p.callbacks = []
        f = gen.Func(p, {}, [], "t")
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "x.xml"
            path.write_text('<view><lv_roller name="roll" height="#roller_h" options="1&#10;2" '
                            'visible_row_count="5" selected="1" /></view>', encoding="utf-8")
            view = gen.parse_xml(path)
        f.create("parent", view[0])
        code = "\n".join(f.lines)
        self.assertIn("lv_obj_t * roll = lv_roller_create(parent);", code)
        self.assertIn('lv_roller_set_options(roll, "1\\x0A" "2", LV_ROLLER_MODE_NORMAL);', code)
        self.assertIn("lv_roller_set_visible_row_count(roll, 5);", code)
        self.assertIn("lv_roller_set_selected(roll, 1, LV_ANIM_OFF);", code)
        self.assertIn("lv_obj_set_height(roll, ROLLER_H);", code)
        self.assertNotIn("LV_ANIM_ON", code)
        # an attribute a roller doesn't have is refused, not passed through
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "y.xml"
            path.write_text('<view><lv_roller mode="infinite" /></view>', encoding="utf-8")
            bad = gen.parse_xml(path)
        with self.assertRaises(gen.GenError):
            gen.Func(p, {}, [], "t").create("parent", bad[0])

    def test_header_declares_the_lookup(self):
        for layout in sorted((ROOT / i18n.LAYOUTS_DIR).glob("*/alarm_ui")):
            text = (layout / "alarm_ui_gen.h").read_text(encoding="utf-8")
            self.assertIn("const char * alarm_ui_tr(const char * english);", text, layout)


class ExtractorAgrees(unittest.TestCase):
    """The extractor and the generator share is_ui_text; this proves the whole extraction: the texts
    it reports for a layout are the texts its generated code passes to alarm_ui_tr."""

    def generated_texts(self, layout):
        texts = set()
        for path in layout.rglob("*_gen.c"):
            for m in TR_CALL.finditer(path.read_text(encoding="utf-8")):
                raw = m.group(1)
                # undo c_string: \xNN escapes back to UTF-8 bytes, quotes split before hex digits
                pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
                body = "".join(pieces)
                data = bytearray()
                i = 0
                while i < len(body):
                    if body.startswith("\\x", i):
                        data.append(int(body[i + 2:i + 4], 16))
                        i += 4
                    elif body[i] == "\\":
                        data += body[i + 1].encode()
                        i += 2
                    else:
                        data += body[i].encode()
                        i += 1
                texts.add(data.decode("utf-8"))
        return texts

    def test_real_layouts(self):
        rels = i18n.layout_projects(ROOT)
        self.assertEqual(len(rels), 2)
        for rel in rels:
            got = {o.msgid for o in i18n.extract_xml_layout(ROOT, rel)}
            self.assertEqual(got, self.generated_texts(ROOT / rel), rel)
            self.assertGreater(len(got), 50)

    def test_occurrences_have_an_xml_place_and_a_context_only_from_a_comment(self):
        with_ctx = {(o.ctx, o.msgid) for o in i18n.extract_xml(ROOT) if o.ctx}
        self.assertIn(("zone filter", "Open"), with_ctx)
        self.assertIn(("arming mode", "Away"), with_ctx)
        for o in i18n.extract_xml(ROOT):
            self.assertIsNone(o.plural)
            self.assertTrue(o.path.endswith(".xml"), o.path)
            self.assertTrue((ROOT / o.path).is_file(), o.path)

    def test_template_lists_the_xml_places(self):
        pot = i18n.build_template(ROOT)
        self.assertIn("#: components/ui/alarm_ui/layouts/1024x600/alarm_ui/", pot)
        self.assertIn('msgid "Acknowledge"', pot)

    def test_same_text_in_both_layouts_is_one_entry_with_both_places(self):
        entries = i18n.build_entries(i18n.extract_xml(ROOT))
        e = entries[(None, "Cancel")]
        self.assertTrue(any("/1024x600/" in p for p in e.places))
        self.assertTrue(any("/480x320/" in p for p in e.places))


if __name__ == "__main__":
    unittest.main()
