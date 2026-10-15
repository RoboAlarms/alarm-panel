"""docs/features/19 CYD-007: every screen layout keeps the UI contract (tools/check_ui_contract.py)."""
import importlib.util
import io
import tempfile
import textwrap
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_ui_contract", TOOLS / "check_ui_contract.py")
cuc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cuc)

GLOBALS = """<globals>
	<consts>
		<int name="space_sm" value="{space}" />
		<int name="only_{size}" value="1" />
	</consts>
	<fonts>
		<bin name="font_label" src_path="fonts/x.ttf" size="16" />
	</fonts>
	<subjects>
		<int name="subject_view" value="0" />
		<string name="subject_title" value="" />
	</subjects>
	<styles>
		<style name="style_active" bg_opa="100%" />
	</styles>
</globals>
"""

ROW = """<component>
	<api>
		<prop name="glyph" type="string" default="x" />
		<prop name="title" type="string" default="t" />
	</api>
	<view extends="lv_obj">
		<lv_label name="title_label" text="$title" />
	</view>
</component>
"""

SCREEN = """<screen>
	<view extends="lv_obj">
		<lv_obj name="pad_hint">
			<event_cb callback="ui_on_pad_key" trigger="clicked" />
		</lv_obj>
		<lv_obj name="{own}" />
	</view>
</screen>
"""

ALARM_UI_C = """#include "alarm_ui.h"
void ui_on_pad_key(lv_event_t * e)
{
}
"""

CODE = """/* "comment_only" and SPACE_XL in a comment are not uses */
static void f(void)
{
    ui_find(root, "pad_hint");
    ui_find(root, "title_label");
    set(SPACE_SM, font_label, &style_active);
}
"""


class Tree:
    def __init__(self):
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)
        src = self.root / "components/ui/alarm_ui/src"
        src.mkdir(parents=True)
        (src / "ui_app.c").write_text(CODE, encoding="utf-8")

    def close(self):
        self._dir.cleanup()

    def layout(self, size, space=8, own="own_a", row=ROW, screen=SCREEN, alarm_c=ALARM_UI_C,
               display=None):
        d = self.root / "components" / "ui" / "alarm_ui" / "layouts" / size / "alarm_ui"
        (d / "components/forms/list_row").mkdir(parents=True)
        (d / "screens").mkdir(parents=True)
        w, h = display or size.split("x")
        (d / "project.xml").write_text(
            '<project><targets><target name="t"><display width="%s" height="%s" />'
            "</target></targets></project>" % (w, h), encoding="utf-8")
        (d / "globals.xml").write_text(GLOBALS.format(space=space, size=size.replace("x", "_")),
                                       encoding="utf-8")
        (d / "components/forms/list_row/list_row.xml").write_text(row, encoding="utf-8")
        (d / "screens/screen_main.xml").write_text(screen.format(own=own), encoding="utf-8")
        (d / "alarm_ui.c").write_text(alarm_c, encoding="utf-8")
        (d / "alarm_ui.h").write_text("#pragma once\n", encoding="utf-8")
        return d

    def check(self, **kw):
        kw.setdefault("on_hold", {})   # these tests are about the contract, not the hold list
        out = io.StringIO()
        return cuc.check(self.root, out, **kw), out.getvalue()


class Contract(unittest.TestCase):
    def setUp(self):
        self.t = Tree()

    def tearDown(self):
        self.t.close()

    def two(self, **second):
        self.t.layout("1024x600")
        return self.t.layout("480x320", **second)

    def test_layouts_that_differ_only_in_their_own_design_pass(self):
        # different constant values, an own constant and an own object name each
        self.two(space=4, own="own_b")
        code, out = self.t.check()
        self.assertEqual(code, 0, out)
        self.assertIn("2 layout(s)", out)

    def test_a_missing_subject_is_found(self):
        d = self.two()
        g = (d / "globals.xml").read_text(encoding="utf-8")
        (d / "globals.xml").write_text(g.replace('<string name="subject_title" value="" />', ""),
                                       encoding="utf-8")
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("subject subject_title exists only in 1024x600", out)

    def test_a_subject_of_another_type_is_found(self):
        d = self.two()
        g = (d / "globals.xml").read_text(encoding="utf-8")
        (d / "globals.xml").write_text(
            g.replace('<string name="subject_title" value="" />',
                      '<int name="subject_title" value="0" />'), encoding="utf-8")
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("subject subject_title is string in one and int in the other", out)

    def test_a_component_whose_props_changed_order_is_found(self):
        swapped = ROW.replace('<prop name="glyph" type="string" default="x" />\n\t\t<prop name="title" type="string" default="t" />',
                              '<prop name="title" type="string" default="t" />\n\t\t<prop name="glyph" type="string" default="x" />')
        self.assertNotEqual(swapped, ROW)
        self.two(row=swapped)
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("component list_row's API differs", out)

    def test_a_missing_component_is_found(self):
        self.t.layout("1024x600")
        d = self.t.layout("480x320")
        (d / "components/forms/list_row/list_row.xml").unlink()
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("component list_row exists only in 1024x600", out)

    def test_the_hand_written_part_is_one_copy(self):
        self.two(alarm_c=ALARM_UI_C + "\n/* drifted */\n")
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("alarm_ui.c differs", out)

    def test_line_endings_alone_are_not_a_difference(self):
        d = self.two()
        (d / "alarm_ui.c").write_bytes(ALARM_UI_C.replace("\n", "\r\n").encode("utf-8"))
        code, out = self.t.check()
        self.assertEqual(code, 0, out)

    def test_a_callback_the_xml_names_must_be_defined(self):
        self.two(screen=SCREEN.replace("ui_on_pad_key", "ui_on_nothing"))
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("callback ui_on_nothing is named in the XML but not defined", out)

    def test_an_object_the_code_looks_up_must_exist_in_every_layout(self):
        self.two(screen=SCREEN.replace('name="pad_hint"', 'name="hint"'))
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn('480x320: the code looks up an object named "pad_hint"', out)

    def test_names_inside_components_count(self):
        self.two(row=ROW.replace('name="title_label"', 'name="caption"'))
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn('an object named "title_label"', out)

    def test_a_global_the_code_names_must_exist_in_every_layout(self):
        d = self.two()
        g = (d / "globals.xml").read_text(encoding="utf-8")
        (d / "globals.xml").write_text(g.replace('<style name="style_active" bg_opa="100%" />', ""),
                                       encoding="utf-8")
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("the code uses style_active", out)

    def test_comments_are_not_uses(self):
        need = cuc.contract(cuc.layouts(self.two() and self.t.root), self.t.root)
        self.assertNotIn("SPACE_XL", need["globals"])
        self.assertIn("SPACE_SM", need["globals"])
        self.assertEqual(need["objects"], ["pad_hint", "title_label"])

    def test_the_display_must_match_the_folder(self):
        self.two(display=("320", "480"))
        code, out = self.t.check()
        self.assertEqual(code, 1)
        self.assertIn("480x320: project.xml's display is 320x480", out)

    def test_the_listing_names_what_the_code_uses(self):
        self.two()
        out = io.StringIO()
        self.assertEqual(cuc.listing(self.t.root, out, on_hold={}), 0)
        self.assertIn("Objects the code looks up (2): pad_hint, title_label", out.getvalue())

    def test_a_layout_on_hold_is_left_out_until_asked_for(self):
        """A board whose work is blocked (docs/features/19): its layout may fall behind the UI code
        without failing the check, and --all still checks it."""
        self.two(screen=SCREEN.replace('name="pad_hint"', 'name="hint"'))   # 480x320 drifted
        hold = {"480x320": "on hold for the test"}
        code, out = self.t.check(on_hold=hold)
        self.assertEqual(code, 0, out)
        self.assertIn("SKIPPED  480x320: on hold for the test", out)
        self.assertIn("1 layout(s) keep the UI contract (1024x600)", out)
        code, out = self.t.check(on_hold=hold, include_on_hold=True)
        self.assertEqual(code, 1)
        self.assertIn('480x320: the code looks up an object named "pad_hint"', out)

    def test_named_layouts_only(self):
        self.two(screen=SCREEN.replace('name="pad_hint"', 'name="hint"'))
        code, out = self.t.check(only={"1024x600"})
        self.assertEqual(code, 0, out)
        code, out = self.t.check(only={"480x320"})
        self.assertEqual(code, 1)
        code, out = self.t.check(only={"800x480"})
        self.assertEqual(code, 1)
        self.assertIn("no layout 800x480 under ui/", out)

    def test_main_takes_the_options(self):
        self.two()
        self.assertEqual(cuc.main(["--root", str(self.t.root), "--layout", "1024x600"]), 0)
        self.assertEqual(cuc.main(["--root", str(self.t.root), "--all", "--list"]), 0)


class Repository(unittest.TestCase):
    def test_every_layout_in_this_repository_keeps_the_contract(self):
        """Every layout not on hold (ON_HOLD: a board whose work is blocked)."""
        out = io.StringIO()
        self.assertEqual(cuc.check(cuc.ROOT, out), 0, out.getvalue())

    def test_what_is_on_hold_is_a_layout_that_exists(self):
        names = {lay.name for lay in cuc.layouts(cuc.ROOT)}
        self.assertLessEqual(set(cuc.ON_HOLD), names, "an ON_HOLD entry names no layout")
        self.assertIn("1024x600", names)
        self.assertNotIn("1024x600", cuc.ON_HOLD)   # the 7" is never left out


if __name__ == "__main__":
    unittest.main()
