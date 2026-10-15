"""docs/features/07 UI-010, BL-132: the appearances. tools/lvpro_gen.py turns the layout's
themes.xml into a palette table that COLOR_* read, nudges a value only in the bits a 16-bit
screen drops, keeps every palette value standing for one colour name (a switch finds colours by
their value), and adds a theme's style changes; mistakes in themes.xml fail the generator."""
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
LAYOUT = ROOT / "components/ui/alarm_ui/layouts/1024x600/alarm_ui"

spec = importlib.util.spec_from_file_location("lvpro_gen", TOOLS / "lvpro_gen.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def rgb565(v):
    return ((v >> 19) & 0x1F, (v >> 10) & 0x3F, (v >> 3) & 0x1F)


GLOBALS = """<globals>
  <consts>
    <color name="color_bg" value="0x0B0F14" />
    <color name="color_text" value="0xF1F5F9" />
    <color name="color_on_tone" value="0x0B0F14" />
  </consts>
  <subjects><color name="subject_tone" value="0x0B0F14" /></subjects>
  <styles>
    <style name="style_card" bg_color="#color_bg" border_width="1" />
    <style name="style_backdrop" bg_color="0x000000" />
  </styles>
</globals>"""

COMPONENT = """<component>
  <styles><style name="style_row" bg_color="#color_bg" text_color="#color_text" /></styles>
  <view extends="lv_obj"><style name="style_row" /></view>
</component>"""

THEMES = """<themes>
  <theme name="dark" title="Dark">
    <color name="color_bg" value="0x0B0F14" /><color name="color_text" value="0xF1F5F9" />
    <color name="color_on_tone" value="0x0B0F14" />
  </theme>
  <theme name="light" title="Light" saver="dark">
    <color name="color_bg" value="0xFFFFFF" /><color name="color_text" value="0x0F172A" />
    <color name="color_on_tone" value="0xFFFFFF" />
  </theme>
  <styles themes="light">
    <style name="style_card" border_width="2" outline_color="#color_text" />
    <style component="row" name="style_row" outline_width="2" />
  </styles>
</themes>"""


def project(tmp, themes=THEMES, globals_xml=GLOBALS, component=COMPONENT):
    d = Path(tmp) / "proj"
    (d / "components/forms/row").mkdir(parents=True)
    (d / "screens").mkdir()
    (d / "project.xml").write_text('<project><targets><target name="default" /></targets></project>',
                                   encoding="utf-8")
    (d / "globals.xml").write_text(globals_xml, encoding="utf-8")
    (d / "components/forms/row/row.xml").write_text(component, encoding="utf-8")
    if themes is not None:
        (d / "themes.xml").write_text(themes, encoding="utf-8")
    return d


class Nudges(unittest.TestCase):
    def test_a_nudge_never_shows_on_a_16_bit_screen(self):
        # UI-010: Dark looks exactly as before: a nudged value is the same RGB565 pixel.
        for v in (0x000000, 0xFFFFFF, 0x0B0F14, 0x123456):
            for d in gen.NUDGES:
                self.assertEqual(rgb565(v), rgb565(v ^ d), hex(d))
        self.assertEqual(len(set(gen.NUDGES)), 255)

    def test_values_stand_for_one_name_and_avoid_literals(self):
        a = gen.Theme("a", "A", False, "a", {"bg": 0x0B0F14, "tone": 0x0B0F14, "text": 0xFFFFFF})
        b = gen.Theme("b", "B", False, "a", {"bg": 0xFFFFFF, "tone": 0x0B0F14, "text": 0x0B0F14})
        gen.nudge_palettes([a, b], ["bg", "tone", "text"], {0x000000, 0xFFFFFF})
        self.assertEqual(a.stored["bg"], 0x0B0F14)  # the first keeps its value
        owner = {}
        for t in (a, b):
            for name, v in t.stored.items():
                self.assertEqual(owner.setdefault(v, name), name, "0x%06X stands for two names" % v)
                self.assertNotIn(v, {0x000000, 0xFFFFFF})
                self.assertEqual(rgb565(v), rgb565(t.colors[name]))


class Generated(unittest.TestCase):
    def test_palette_constants_registry_and_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = gen.Project(project(tmp))
            files = gen.generate(p)
        h, c = files["proj_gen.h"], files["proj_gen.c"]
        self.assertIn("#define COLOR_BG (proj_palette[PROJ_COLOR_BG])", h)
        self.assertIn("#define PROJ_THEMES 1", h)  # what the hand-written switch is built on
        self.assertIn("PROJ_THEME_LIGHT, /* Light */", h)
        self.assertIn("const lv_color_t * proj_palette = proj_palettes[0];", c)
        # Dark's color_on_tone shares color_bg's value: nudged, the approved value beside it.
        self.assertIn("[PROJ_COLOR_BG] = LV_COLOR_MAKE(0x0B, 0x0F, 0x14),\n", c)
        self.assertRegex(c, r"\[PROJ_COLOR_ON_TONE\] = LV_COLOR_MAKE\([^)]*\), /\* 0x0B0F14 \*/")
        self.assertIn("const uint8_t proj_theme_saver[PROJ_THEME_COUNT] = {PROJ_THEME_DARK, PROJ_THEME_DARK};", c)
        self.assertIn("    &subject_tone,", c)
        # A theme's style change keeps the style's own value for the other themes, or none.
        self.assertIn("{PROJ_STYLE_CARD, LV_STYLE_BORDER_WIDTH, 0x02, PROJ_VALUE_INT, PROJ_VALUE_INT, 2, 1},", c)
        self.assertIn("{PROJ_STYLE_CARD, LV_STYLE_OUTLINE_COLOR, 0x02, PROJ_VALUE_COLOR, PROJ_VALUE_NONE, PROJ_COLOR_TEXT, 0},", c)
        self.assertIn("{PROJ_STYLE_ROW_ROW, LV_STYLE_OUTLINE_WIDTH, 0x02, PROJ_VALUE_INT, PROJ_VALUE_NONE, 2, 0},", c)
        # Every style joins the registry when it is initialised, global and component alike.
        self.assertIn("proj_style_track(&style_card, PROJ_STYLE_CARD);", c)
        self.assertIn("proj_style_track(&style_row, PROJ_STYLE_ROW_ROW);",
                      files["components/forms/row/row_gen.c"])

    def test_without_themes_xml_one_dark_theme(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = gen.Project(project(tmp, themes=None))
            files = gen.generate(p)
        self.assertIn("PROJ_THEME_DARK = 0, /* Dark */\n    PROJ_THEME_COUNT", files["proj_gen.h"])
        self.assertIn("proj_style_override_count = 0;", files["proj_gen.c"])

    def test_mistakes_fail(self):
        bad = {
            "lacks": THEMES.replace('<color name="color_on_tone" value="0xFFFFFF" />', ""),
            "must be globals.xml's colours": THEMES.replace('value="0xF1F5F9"', 'value="0xF1F5F8"'),
            "no such style": THEMES.replace('name="style_card" border_width', 'name="style_nope" border_width'),
            "can't change with the theme": THEMES.replace('outline_width="2" />', 'width="20" />'),
            "which is no theme": THEMES.replace('saver="dark"', 'saver="dusk"'),
            "not a colour of globals.xml": THEMES.replace('outline_color="#color_text"', 'outline_color="#color_x"'),
        }
        for why, text in bad.items():
            with self.subTest(why), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(gen.GenError, re.escape(why)):
                    gen.Project(project(tmp, themes=text))


class TheLayout(unittest.TestCase):
    def test_the_seven_inch_palettes(self):
        p = gen.Project(LAYOUT)
        self.assertEqual([t.name for t in p.themes], ["dark", "light", "contrast", "contrast_light"])
        owner = {}
        for t in p.themes:
            for name in p.colors:
                v = t.stored[name]
                self.assertEqual(owner.setdefault(v, name), name, "%s: 0x%06X" % (t.name, v))
                self.assertEqual(rgb565(v), rgb565(t.colors[name]), "%s %s" % (t.name, name))
        dark = p.themes[0]
        # Dark is today's (color_text_3 raised, approved 2026-10-01) and keeps its exact values
        # but for the three that share one with another name.
        self.assertEqual(dark.colors["color_text_3"], 0x808DA0)
        self.assertEqual(sorted(n for n in p.colors if dark.stored[n] != dark.colors[n]),
                         ["color_key_special", "color_knob", "color_on_tone"])
        self.assertEqual({t.name: t.saver for t in p.themes},
                         {"dark": "dark", "light": "dark", "contrast": "contrast",
                          "contrast_light": "contrast"})


if __name__ == "__main__":
    unittest.main()
