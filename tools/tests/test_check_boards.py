"""docs/features/33 step 9: board manifests (tools/check_boards.py, cmake/alarm_board.cmake)."""
import importlib.util
import io
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_boards", TOOLS / "check_boards.py")
cb = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cb)

FULL = {
    "IDF_TARGET": '"esp32"',
    "ALARM_UI_LAYOUT": '"480x320"',
    "ALARM_MAX_ZONES": "64",
    "ALARM_MAX_USERS": "32",
    "ALARM_FEATURE_UI": "y",
    "ALARM_FEATURE_MQTT": "y",
    "ALARM_FEATURE_NOTIFY": "n",
    "ALARM_FEATURE_HA": "n",
    "ALARM_PANEL_WEB": "n",
    "ALARM_DEV_REMOTE": "n",
    "ALARM_FEATURE_STORAGE": "n",
    "ALARM_FEATURE_PHOTO_SAVER": "n",
    "ALARM_FEATURE_RTC": "n",
    "ALARM_HOME_CONTROL": "n",
    "ALARM_FEATURE_AUTOMATIONS": "n",
    "ALARM_LINK": "n",
    "ALARM_ZSRC_RF": "y",
    "ALARM_ZSRC_ZIGBEE": "n",
    "ALARM_ZSRC_ZWAVE": "n",
    "ALARM_ZWAVE_S0": "n",
    "ALARM_ZWAVE_LR": "n",
    "PANEL_STORAGE_SDMMC": "n",
    "PANEL_STORAGE_SDSPI": "n",
}


def defaults_text(values):
    lines = []
    for k, v in values.items():
        lines.append("# CONFIG_%s is not set" % k if v == "n" else "CONFIG_%s=%s" % (k, v))
    return "\n".join(lines) + "\n"


class Tree:
    def __init__(self):
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)

    def close(self):
        self._dir.cleanup()

    def layout(self, name):
        d = self.root / "components" / "ui" / "alarm_ui" / "layouts" / name / "alarm_ui"
        d.mkdir(parents=True)
        (d / "file_list_gen.cmake").write_text("", encoding="utf-8")

    def board(self, name, values, main_requires=None, component=True):
        fw = self.root / "firmware" / name
        (fw / "main").mkdir(parents=True)
        (fw / "sdkconfig.defaults").write_text(defaults_text(values), encoding="utf-8")
        reqs = main_requires if main_requires is not None else "panel_app board_%s" % name
        (fw / "main" / "CMakeLists.txt").write_text(
            "idf_component_register(SRCS app_main.c REQUIRES %s)\n" % reqs, encoding="utf-8")
        if component:
            (self.root / "components" / "boards" / ("board_" + name)).mkdir(parents=True)

    def problems(self, name):
        return cb.check_board(self.root, self.root / "firmware" / name)


class Parsing(unittest.TestCase):
    def test_set_unset_quotes_and_the_last_line_wins(self):
        values = cb.parse_defaults(textwrap.dedent('''
            # a comment
            CONFIG_A=y
            # CONFIG_B is not set
            CONFIG_C="with spaces"
            CONFIG_D=12
            CONFIG_A=n
            #CONFIG_E is not set
            CONFIG_lower=1
        '''))
        self.assertEqual(values, {"A": "n", "B": "n", "C": "with spaces", "D": "12", "E": "n"})


class Manifests(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.close)
        self.t.layout("480x320")

    def test_a_complete_manifest_passes(self):
        self.t.board("tiny", FULL)
        self.assertEqual(self.t.problems("tiny"), [])
        out = io.StringIO()
        self.assertEqual(cb.run(self.t.root, out), 0, out.getvalue())

    def test_every_option_must_be_stated(self):
        values = dict(FULL)
        del values["ALARM_FEATURE_NOTIFY"]
        self.t.board("tiny", values)
        self.assertEqual(self.t.problems("tiny"),
                         ["tiny: CONFIG_ALARM_FEATURE_NOTIFY is not stated in sdkconfig.defaults"])

    def test_the_layout_must_exist_when_there_is_a_ui(self):
        self.t.board("tiny", dict(FULL, ALARM_UI_LAYOUT='"800x480"'))
        self.assertIn("tiny: the UI layout components/ui/alarm_ui/layouts/800x480/alarm_ui does not exist", self.t.problems("tiny"))
        self.t.board("odd", dict(FULL, ALARM_UI_LAYOUT='"wide"'))
        self.assertIn("odd: CONFIG_ALARM_UI_LAYOUT='wide' is not <width>x<height>",
                      self.t.problems("odd"))
        self.t.board("headless", dict(FULL, ALARM_UI_LAYOUT='"800x480"', ALARM_FEATURE_UI="n"))
        self.assertEqual(self.t.problems("headless"), [])

    def test_heavy_features_need_psram(self):
        self.t.board("tiny", dict(FULL, ALARM_FEATURE_HA="y", ALARM_PANEL_WEB="y"))
        problems = self.t.problems("tiny")
        self.assertIn("tiny: CONFIG_ALARM_FEATURE_HA needs PSRAM, which this board's manifest "
                      "doesn't turn on", problems)
        self.assertIn("tiny: CONFIG_ALARM_PANEL_WEB needs PSRAM, which this board's manifest "
                      "doesn't turn on", problems)
        self.t.board("big", dict(FULL, ALARM_FEATURE_HA="y", ALARM_PANEL_WEB="y", SPIRAM="y"))
        self.assertEqual(self.t.problems("big"), [])

    def test_the_panel_link_needs_psram(self):
        # docs/features/11: the keypads' configuration copies live in PSRAM.
        self.t.board("tiny", dict(FULL, ALARM_LINK="y"))
        self.assertEqual(self.t.problems("tiny"),
                         ["tiny: CONFIG_ALARM_LINK needs PSRAM, which this board's manifest "
                          "doesn't turn on"])
        self.t.board("big", dict(FULL, ALARM_LINK="y", SPIRAM="y"))
        self.assertEqual(self.t.problems("big"), [])

    def test_devices_need_the_home_assistant_link(self):
        self.t.board("big", dict(FULL, SPIRAM="y", ALARM_HOME_CONTROL="y"))
        self.assertEqual(self.t.problems("big"),
                         ["big: CONFIG_ALARM_HOME_CONTROL needs CONFIG_ALARM_FEATURE_HA"])

    def test_zwave_needs_psram_and_its_options_need_zwave(self):
        # docs/features/36 Z8: the network's state lives in PSRAM; S0 and Long Range are
        # options of the Z-Wave source.
        self.t.board("tiny", dict(FULL, ALARM_ZSRC_ZWAVE="y"))
        self.assertEqual(self.t.problems("tiny"),
                         ["tiny: CONFIG_ALARM_ZSRC_ZWAVE needs PSRAM, which this board's manifest "
                          "doesn't turn on"])
        self.t.board("big", dict(FULL, SPIRAM="y", ALARM_ZWAVE_S0="y", ALARM_ZWAVE_LR="y"))
        self.assertEqual(self.t.problems("big"),
                         ["big: CONFIG_ALARM_ZWAVE_S0 needs CONFIG_ALARM_ZSRC_ZWAVE",
                          "big: CONFIG_ALARM_ZWAVE_LR needs CONFIG_ALARM_ZSRC_ZWAVE"])
        self.t.board("zw", dict(FULL, SPIRAM="y", ALARM_ZSRC_ZWAVE="y", ALARM_ZWAVE_LR="y"))
        self.assertEqual(self.t.problems("zw"), [])

    def test_storage_needs_an_sd_driver(self):
        self.t.board("tiny", dict(FULL, ALARM_FEATURE_STORAGE="y"))
        self.assertEqual(len(self.t.problems("tiny")), 1)
        self.t.board("spi", dict(FULL, ALARM_FEATURE_STORAGE="y", PANEL_STORAGE_SDSPI="y"))
        self.assertEqual(self.t.problems("spi"), [])

    def test_capacities_in_range(self):
        self.t.board("tiny", dict(FULL, ALARM_MAX_ZONES="0", ALARM_MAX_USERS="lots"))
        problems = self.t.problems("tiny")
        self.assertIn("tiny: CONFIG_ALARM_MAX_ZONES=0 is outside 1..999", problems)
        self.assertIn("tiny: CONFIG_ALARM_MAX_USERS=lots is outside 2..99", problems)

    def test_main_requires_its_own_board_and_no_other(self):
        self.t.board("tiny", FULL, main_requires="panel_app board_other board_api")
        problems = self.t.problems("tiny")
        self.assertIn("tiny: main/CMakeLists.txt doesn't require its board component board_tiny",
                      problems)
        self.assertIn("tiny: main/CMakeLists.txt names another board's component board_other",
                      problems)
        self.t.board("orphan", FULL, component=False)
        self.assertIn("orphan: no board component components/boards/board_orphan",
                      self.t.problems("orphan"))

    def test_the_table_lists_every_board(self):
        self.t.board("tiny", FULL)
        self.t.board("small", dict(FULL, ALARM_FEATURE_MQTT="n"))
        out = io.StringIO()
        cb.table(self.t.root, out)
        text = out.getvalue()
        self.assertIn("| Option | small | tiny |", text)
        self.assertIn("| `ALARM_FEATURE_MQTT` | n | y |", text)


class BuiltOnAnother(unittest.TestCase):
    """A firmware built on another board's project (ALARM_BOARD_BASE) and the family library its
    board component requires (ALARM_BOARD_FAMILY): firmware/README.md, "A board built on
    another"."""

    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.close)
        self.t.layout("480x320")

    def project(self, name, values, base=None, family=None, requires=""):
        self.t.board(name, values)
        lines = ["set(ALARM_BOARD %s)" % name]
        if family:
            lines.append("set(ALARM_BOARD_FAMILY %s)" % family)
        if base:
            lines.append("set(ALARM_BOARD_BASE %s)" % base)
        (self.t.root / "firmware" / name / "CMakeLists.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8")
        (self.t.root / "components" / "boards" / ("board_" + name) / "CMakeLists.txt").write_text(
            "idf_component_register(REQUIRES board_api %s)\n" % requires, encoding="utf-8")

    def test_the_base_manifest_comes_first(self):
        self.project("big", FULL)
        self.project("bigger", {"ALARM_MAX_ZONES": "128"}, base="big")
        values = cb.read_manifest(self.t.root / "firmware" / "bigger")
        self.assertEqual((values["ALARM_MAX_ZONES"], values["ALARM_FEATURE_MQTT"]), ("128", "y"))
        self.assertEqual(self.t.problems("bigger"), [])

    def test_a_base_exists_and_has_no_base(self):
        self.project("a", FULL)
        self.project("b", {}, base="a")
        self.project("c", {}, base="b")
        self.assertIn("c: its base firmware/b is built on another project (one level only)",
                      self.t.problems("c"))
        self.project("d", FULL, base="nowhere")
        self.assertIn("d: its base firmware/nowhere does not exist", self.t.problems("d"))

    def test_the_family_is_the_one_its_board_component_requires(self):
        (self.t.root / "components" / "boards" / "kin_family").mkdir(parents=True)
        self.project("one", FULL, family="kin_family", requires="kin_family")
        self.assertEqual(self.t.problems("one"), [])
        self.project("two", FULL, requires="kin_family  # the family")
        self.assertIn("two: board_two requires kin_family, which CMakeLists.txt doesn't name in "
                      "ALARM_BOARD_FAMILY", self.t.problems("two"))
        self.project("three", FULL, family="kin_family", requires="# kin_family")
        self.assertIn("three: ALARM_BOARD_FAMILY kin_family, which board_three doesn't require",
                      self.t.problems("three"))
        self.project("four", FULL, family="lost_family", requires="lost_family")
        self.assertIn("four: no family library components/boards/lost_family",
                      self.t.problems("four"))


class CMakeAgrees(unittest.TestCase):
    """The PC builds read manifests with cmake/alarm_board.cmake: it must read what Python reads."""

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not on PATH")
    def test_cmake_reads_the_same_values(self):
        sample = textwrap.dedent('''
            # a comment
            CONFIG_A=y
            # CONFIG_B is not set
            CONFIG_C="with spaces"
            CONFIG_D=12
            CONFIG_A=n
            #CONFIG_E is not set
        ''')
        with tempfile.TemporaryDirectory() as d:
            manifest = Path(d) / "sdkconfig.defaults"
            manifest.write_text(sample, encoding="utf-8")
            script = Path(d) / "read.cmake"
            script.write_text(textwrap.dedent('''
                include("%s")
                alarm_read_manifest("%s" M)
                foreach(o IN LISTS M_OPTIONS)
                    message(NOTICE "${o}=${M_${o}}")
                endforeach()
            ''' % ((TOOLS.parent / "cmake" / "alarm_board.cmake").as_posix(), manifest.as_posix())),
                encoding="utf-8")
            run = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            read = dict(line.split("=", 1) for line in run.stderr.strip().splitlines())
        self.assertEqual(read, cb.parse_defaults(sample))

    def profile(self, board, repo=None):
        """cmake/alarm_board.cmake's profile of a board (in this repository, or in repo):
        (exit code, output)."""
        with tempfile.TemporaryDirectory() as d:
            script = Path(d) / "profile.cmake"
            script.write_text(textwrap.dedent('''
                include("%s")
                %s
                alarm_board_profile(%s)
                message(NOTICE "layout=${ALARM_BOARD_LAYOUT}")
                message(NOTICE "size=${ALARM_BOARD_WIDTH}x${ALARM_BOARD_HEIGHT}")
                message(NOTICE "features=${ALARM_BOARD_UI_FEATURES}")
                message(NOTICE "budget=${ALARM_BOARD_LV_BUDGET_KB} dpi=${ALARM_BOARD_LV_DPI}")
                message(NOTICE "defs=${ALARM_BOARD_DEFINITIONS}")
            ''' % ((TOOLS.parent / "cmake" / "alarm_board.cmake").as_posix(),
                   'set(ALARM_REPO "%s")' % Path(repo).as_posix() if repo else "", board)),
                encoding="utf-8")
            run = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
        return run.returncode, run.stderr

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not on PATH")
    def test_the_p4_profile_is_its_manifest(self):
        code, out = self.profile("crowpanel_p4")
        self.assertEqual(code, 0, out)
        self.assertIn("layout=1024x600", out)
        self.assertIn("size=1024x600", out)
        self.assertIn("features=NOTIFY;INTEGRATIONS;STORAGE;PHOTO_SAVER", out)
        self.assertIn("budget=0 dpi=160", out)  # PSRAM: not checked
        self.assertIn("CONFIG_ALARM_FEATURE_HA=1", out)
        self.assertIn("CONFIG_ALARM_PANEL_WEB=1", out)

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not on PATH")
    def test_the_4in_profile_is_its_manifest(self):
        code, out = self.profile("e32r40t")
        self.assertEqual(code, 0, out)
        self.assertIn("layout=480x320", out)
        self.assertIn("size=480x320", out)
        self.assertIn("features=INTEGRATIONS", out)  # MQTT only
        self.assertIn("dpi=146", out)
        self.assertIn("budget=96 dpi=146", out)
        self.assertIn("CONFIG_ALARM_FEATURE_MQTT=1", out)
        self.assertIn("CONFIG_ALARM_FEATURE_HA=0", out)
        self.assertIn("CONFIG_ALARM_PANEL_WEB=0", out)

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not on PATH")
    def test_the_10in_profile_is_the_7ins(self):
        # firmware/crowpanel_10 is built on crowpanel_p4 and changes none of its features.
        code10, out10 = self.profile("crowpanel_10")
        code7, out7 = self.profile("crowpanel_p4")
        self.assertEqual((code10, code7), (0, 0), out10 + out7)
        self.assertEqual(out10, out7)

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not on PATH")
    def test_a_profile_reads_the_base_first(self):
        with tempfile.TemporaryDirectory() as d:
            fw = Path(d) / "firmware"
            (fw / "base").mkdir(parents=True)
            (fw / "base" / "sdkconfig.defaults").write_text(defaults_text(FULL), encoding="utf-8")
            (fw / "wide").mkdir()
            (fw / "wide" / "CMakeLists.txt").write_text("set(ALARM_BOARD_BASE base)\n",
                                                       encoding="utf-8")
            (fw / "wide" / "sdkconfig.defaults").write_text(
                'CONFIG_ALARM_UI_LAYOUT="800x480"\n', encoding="utf-8")
            code, out = self.profile("wide", repo=d)
        self.assertEqual(code, 0, out)
        self.assertIn("layout=800x480", out)
        self.assertIn("features=INTEGRATIONS", out)  # the base's MQTT

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not on PATH")
    def test_a_headless_board_has_no_pc_profile(self):
        with tempfile.TemporaryDirectory() as d:
            fw = Path(d) / "firmware" / "headless"
            fw.mkdir(parents=True)
            values = dict(FULL, ALARM_FEATURE_UI="n")
            (fw / "sdkconfig.defaults").write_text(defaults_text(values), encoding="utf-8")
            code, out = self.profile("headless", repo=d)
        self.assertNotEqual(code, 0)
        self.assertIn("no touch UI", out)


class Repository(unittest.TestCase):
    def test_every_board_in_this_repository_is_consistent(self):
        out = io.StringIO()
        self.assertEqual(cb.run(cb.ROOT, out), 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
