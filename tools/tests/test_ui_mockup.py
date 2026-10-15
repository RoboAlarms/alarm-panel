"""ui_mockup.py: the touchscreen mockup passes its own check, a broken proposal doesn't, the
contrast check holds every appearance to WCAG (BL-132), and the PNG reader the compare uses reads
what it writes (UI-044)."""
import argparse
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ui_mockup", TOOLS / "ui_mockup.py")
um = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(um)


def run_check(shots_dir):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = um.check(argparse.Namespace(shots_dir=str(shots_dir)))
    return rc, out.getvalue()


class Check(unittest.TestCase):
    def test_the_mockup_passes(self):
        with tempfile.TemporaryDirectory() as empty:
            rc, out = run_check(empty)
        self.assertEqual(rc, 0, out)

    def test_a_broken_proposal_fails(self):
        bad = um.MOCK / "proposals" / "zz-test-broken.js"
        bad.write_text(
            "M.screen({ id: 'home-ready', area: 'x', title: 'x', proposal: { status: 'maybe' },\n"
            "  render: () => M.icon('no-such-icon') + '<b style=\"color:#ff0000\">' });\n",
            encoding="utf-8")
        try:
            with tempfile.TemporaryDirectory() as empty:
                rc, out = run_check(empty)
        finally:
            bad.unlink()
        self.assertEqual(rc, 1)
        for words in ("'home-ready' is also used", "status 'maybe'", "names no backlog row",
                      "'no-such-icon' is not a Lucide icon", "a colour of its own"):
            self.assertIn(words, out)


NEXT = um.ROOT / "docs/features/07-UI-LVGL-Pro/mockup-next"


class Themes(unittest.TestCase):
    """BL-132's appearances: every word readable in every palette (WCAG 2.x contrast)."""

    def test_contrast_is_wcags(self):
        self.assertAlmostEqual(um.contrast("#000000", "#FFFFFF"), 21.0, places=6)
        self.assertAlmostEqual(um.contrast("#FFFFFF", "#000000"), 21.0, places=6)
        self.assertAlmostEqual(um.contrast("#767676", "#FFFFFF"), 4.54, places=2)
        self.assertAlmostEqual(um.contrast("#5B6778", "#5B6778"), 1.0, places=6)

    def test_a_faint_hint_is_named(self):
        # Today's hint colour on today's screen: 3.35:1, under AA.
        out = um.contrast_problems({"high": False, "colors": {"color_bg": "#0B0F14", "color_text_3": "#5B6778",
                                                               "color_text": "#F1F5F9"}})
        self.assertEqual(out, ["color_text_3 on color_bg is 3.35:1, under 4.5:1"])

    def test_high_contrast_asks_more(self):
        colors = {"color_bg": "#000000", "color_surface": "#000000", "color_text_2": "#8A8A8A", "color_line": "#333333",
                  "color_key_special": "#000000"}
        self.assertEqual(um.contrast_problems({"high": False, "colors": colors}), [])
        high = um.contrast_problems({"high": True, "colors": colors})
        self.assertIn("color_text_2 on color_bg is 6.08:1, under 7:1", high)
        self.assertTrue(any(p.startswith("color_line on color_surface") and "outlines must show" in p for p in high), high)

    def test_the_special_keys_are_checked(self):
        # High contrast's line is an outline colour: as the fill of the keyboard's special keys
        # (style_keyboard_key_special today) it left their symbols at 1.94:1 (BL-132).
        colors = {"color_bg": "#FFFFFF", "color_surface": "#FFFFFF", "color_text_2": "#1F2937", "color_line": "#4B5563"}
        self.assertEqual(um.contrast_problems({"high": True, "colors": colors}),
                         ["color_text_2 on color_line is 1.94:1, under 7:1"])
        colors["color_key_special"] = "#E0E5EA"
        self.assertEqual(um.contrast_problems({"high": True, "colors": colors}), [])

    @unittest.skipUnless((um.LAYOUT / "themes.xml").exists(), "no appearances")
    def test_the_redesign_passes(self):
        saved = um.MOCK
        um.MOCK = NEXT
        try:
            th = um.themes()
            with tempfile.TemporaryDirectory() as empty:
                rc, out = run_check(empty)
        finally:
            um.MOCK = saved
        self.assertEqual(rc, 0, out)
        self.assertIn("dark", th)
        for name, theme in th.items():
            self.assertEqual(um.contrast_problems(theme), [], name)
            self.assertEqual(set(theme["colors"]), set(th["dark"]["colors"]), name)


class Png(unittest.TestCase):
    def test_round_trip(self):
        rgba = bytes((x * 40, y * 60, (x + y) * 20, 255) [i] for y in range(3) for x in range(4) for i in range(4))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.png"
            p.write_bytes(um.png_rgba(4, 3, rgba))
            self.assertEqual(um.png_pixels(p), (4, 3, 4, rgba))


if __name__ == "__main__":
    unittest.main()
