"""docs/features/33 step 2: the tier rules checker (tools/check_architecture.py) on small trees."""
import importlib.util
import io
import tempfile
import textwrap
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "check_architecture", Path(__file__).resolve().parents[1] / "check_architecture.py")
ca = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ca)


class Tree:
    """A throwaway repository with components/<tier>/<name>/."""

    def __init__(self):
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)

    def close(self):
        self._dir.cleanup()

    def component(self, tier, name, cmake="", files=None, readme=True, tests=None):
        path = self.root / "components" / tier / name
        path.mkdir(parents=True)
        (path / "CMakeLists.txt").write_text(textwrap.dedent(cmake), encoding="utf-8")
        if readme:
            (path / "README.md").write_text("# %s\n" % name, encoding="utf-8")
        if tests if tests is not None else tier == "core":
            (path / "test").mkdir()
            (path / "test" / "CMakeLists.txt").write_text("", encoding="utf-8")
        for rel, text in (files or {}).items():
            f = path / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(textwrap.dedent(text), encoding="utf-8")
        return path

    def write_index(self, skip=()):
        """components/README.md naming every component except <skip>."""
        names = [p.name for p in (self.root / "components").glob("*/*") if p.is_dir()]
        (self.root / "components" / "README.md").write_text(
            "".join("- `%s`\n" % n for n in sorted(names) if n not in skip), encoding="utf-8")

    def run(self, exceptions=None, index=True):
        if index:
            self.write_index()
        out = io.StringIO()
        code = ca.run(self.root, exceptions or {}, out)
        return code, out.getvalue()

    def violations(self):
        return ca.check(ca.discover(self.root))


def register(requires="", priv=""):
    return "idf_component_register(SRCS x.c INCLUDE_DIRS include REQUIRES %s PRIV_REQUIRES %s)\n" % (
        requires, priv)


class TierRules(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.close)
        # A small, valid project every test starts from.
        self.t.component("core", "engine", register(),
                         {"include/engine/engine.h": "int engine_run(void);\n",
                          "src/engine.c": '#include "engine/engine.h"\nint engine_run(void){return 0;}\n'})
        self.t.component("services", "svc", register("engine esp_timer"),
                         {"include/svc.h": "void svc(void);\n",
                          "src/svc.c": '#include "svc.h"\n#include "esp_log.h"\n#include "engine/engine.h"\n'})
        self.t.component("ui", "screens", register("engine lvgl__lvgl"),
                         {"src/screens.c": '#include "lvgl.h"\n#include "engine/engine.h"\n'})

    def kinds(self):
        return sorted((v.component, v.kind, v.target) for v in self.t.violations())

    def test_a_clean_tree_passes(self):
        code, out = self.t.run()
        self.assertEqual(code, 0, out)
        self.assertIn("OK: 3 components", out)

    def test_core_may_not_require_esp_idf(self):
        self.t.component("core", "bad", register("esp_timer"))
        self.assertIn(("bad", "requires", "esp_timer"), self.kinds())

    def test_core_may_link_libm_on_the_host(self):
        self.t.component("core", "maths", "add_library(maths STATIC m.c)\n"
                                           "target_link_libraries(maths PUBLIC m)\n")
        self.assertEqual(self.kinds(), [])

    def test_core_may_not_include_platform_or_lvgl_headers(self):
        self.t.component("core", "bad", register(),
                         {"src/a.c": '#include "esp_log.h"\n#include <freertos/task.h>\n#include "lvgl.h"\n'})
        kinds = self.kinds()
        self.assertIn(("bad", "include", "esp_log.h"), kinds)
        self.assertIn(("bad", "include", "freertos/task.h"), kinds)
        self.assertIn(("bad", "include", "lvgl.h"), kinds)

    def test_core_may_not_allocate_but_comments_and_strings_do_not_count(self):
        self.t.component("core", "alloc", register(),
                         {"src/a.c": 'void f(void){ /* 0 = free (slot) */ const char *s = "free(x)";\n'
                                 '  // malloc(1) in a comment\n  void *p = malloc(4); (void)s; (void)p; }\n'})
        heap = [v for v in self.t.violations() if v.kind == "heap"]
        self.assertEqual([(v.target, v.where) for v in heap], [("malloc", "src/a.c")])
        self.assertIn("src/a.c:3", str(heap[0]))

    def test_every_component_needs_a_readme_and_core_needs_tests(self):
        self.t.component("core", "undocumented", register(), readme=False, tests=False)
        kinds = self.kinds()
        self.assertIn(("undocumented", "readme", "README.md"), kinds)
        self.assertIn(("undocumented", "tests", "test"), kinds)

    def test_sources_and_private_headers_go_in_src_not_at_the_root(self):
        self.t.component("services", "loose", register(),
                         {"loose.c": "", "loose_internal.h": "", "include/loose.h": "",
                          "src/fine.c": "", "images/logo_png.c": ""})
        layout = sorted(v.target for v in self.t.violations() if v.kind == "layout")
        self.assertEqual(layout, ["loose.c", "loose_internal.h"])

    def test_services_may_not_name_a_board_but_may_use_the_contract(self):
        self.t.component("boards", "board_api", register(), {"include/board.h": ""})
        self.t.component("boards", "board_x", register("board_api esp_lcd"),
                         {"include/board_pins.h": ""})
        self.t.component("services", "sd", register("board_api board_x"),
                         {"src/sd.c": '#include "board.h"\n#include "board_pins.h"\n'})
        kinds = self.kinds()
        self.assertIn(("sd", "requires", "board_x"), kinds)
        self.assertIn(("sd", "include", "board_pins.h"), kinds)
        self.assertNotIn(("sd", "requires", "board_api"), kinds)
        self.assertNotIn(("sd", "include", "board.h"), kinds)
        self.assertNotIn(("board_x", "requires", "board_api"), kinds)

    def test_boards_may_not_depend_on_services_or_ui(self):
        self.t.component("boards", "board_y", register("svc screens lvgl__lvgl"))
        kinds = self.kinds()
        self.assertIn(("board_y", "requires", "svc"), kinds)
        self.assertIn(("board_y", "requires", "screens"), kinds)
        self.assertNotIn(("board_y", "requires", "lvgl__lvgl"), kinds)

    def test_a_family_library_serves_its_boards_only(self):
        """components/boards/<name>_family: what the boards on one main board share."""
        self.t.component("boards", "board_api", register(), {"include/board_api.h": ""})
        self.t.component("boards", "panel_family", register("board_api esp_lcd lvgl__lvgl"),
                         {"include/panel.h": "", "src/p.c": '#include "board_api.h"\n'})
        self.t.component("boards", "board_seven", register("board_api panel_family"),
                         {"include/board.h": '#include "panel.h"\n'})
        self.t.component("boards", "board_ten", register("board_api panel_family"),
                         {"include/board.h": '#include "panel.h"\n',
                          "include/board_pins.h": ""})
        self.t.component("boards", "reaching_family", register("board_ten"),
                         {"src/r.c": '#include "board_pins.h"\n'})
        self.t.component("services", "svc_family", register("panel_family"),
                         {"src/s.c": '#include "panel.h"\n'})
        self.t.component("app", "app_family", register("panel_family"))
        kinds = self.kinds()
        for board in ("board_seven", "board_ten", "panel_family"):
            self.assertEqual([k for k in kinds if k[0] == board], [], board)
        self.assertIn(("reaching_family", "requires", "board_ten"), kinds)
        self.assertIn(("reaching_family", "include", "board_pins.h"), kinds)
        self.assertIn(("svc_family", "requires", "panel_family"), kinds)
        self.assertIn(("svc_family", "include", "panel.h"), kinds)
        self.assertIn(("app_family", "requires", "panel_family"), kinds)

    def test_ui_may_not_reach_services_or_esp_idf(self):
        self.t.component("ui", "page", register("engine svc"),
                         {"src/page.c": '#include "svc.h"\n#include "sdkconfig.h"\n'})
        kinds = self.kinds()
        self.assertIn(("page", "requires", "svc"), kinds)
        self.assertIn(("page", "include", "svc.h"), kinds)
        self.assertIn(("page", "include", "sdkconfig.h"), kinds)

    def test_only_the_ui_may_use_placement_attributes(self):
        self.t.component("ui", "big", register("engine"), {"src/state.c": '#include "esp_attr.h"\n'})
        self.t.component("core", "big_core", register(), {"src/s.c": '#include "esp_attr.h"\n'})
        kinds = self.kinds()
        self.assertNotIn(("big", "include", "esp_attr.h"), kinds)
        self.assertIn(("big_core", "include", "esp_attr.h"), kinds)

    def test_services_and_drivers_may_not_use_lvgl(self):
        self.t.component("drivers", "touch", register("esp_driver_spi lvgl__lvgl"))
        self.t.component("services", "draw", register("lvgl__lvgl"), {"src/d.c": '#include "lvgl.h"\n'})
        kinds = self.kinds()
        self.assertIn(("touch", "requires", "lvgl__lvgl"), kinds)
        self.assertIn(("draw", "requires", "lvgl__lvgl"), kinds)
        self.assertIn(("draw", "include", "lvgl.h"), kinds)

    def test_drivers_may_not_depend_on_boards_or_services(self):
        self.t.component("boards", "board_z", register())
        self.t.component("drivers", "chip", register("board_z svc esp_driver_spi"))
        kinds = self.kinds()
        self.assertIn(("chip", "requires", "board_z"), kinds)
        self.assertIn(("chip", "requires", "svc"), kinds)
        self.assertNotIn(("chip", "requires", "esp_driver_spi"), kinds)

    def test_board_options_stay_with_boards_and_app_options_stay_out(self):
        self.t.component("boards", "board_q", register(),
                         {"src/b.c": "#if CONFIG_BOARD_GUARD\n#endif\n#if CONFIG_ALARM_WEB\n#endif\n"})
        self.t.component("services", "leaky", register(),
                         {"src/l.c": "/* CONFIG_BOARD_IN_A_COMMENT */\n#if CONFIG_BOARD_GUARD\n#endif\n"
                                 "#if CONFIG_ALARM_WEB\n#endif\n"})
        kinds = self.kinds()
        self.assertIn(("board_q", "option", "CONFIG_ALARM_WEB"), kinds)
        self.assertNotIn(("board_q", "option", "CONFIG_BOARD_GUARD"), kinds)
        self.assertIn(("leaky", "option", "CONFIG_BOARD_GUARD"), kinds)
        self.assertNotIn(("leaky", "option", "CONFIG_BOARD_IN_A_COMMENT"), kinds)
        self.assertNotIn(("leaky", "option", "CONFIG_ALARM_WEB"), kinds)

    def test_the_app_tier_composes_everything_but_a_board(self):
        self.t.component("boards", "board_api", register(), {"include/board_api.h": ""})
        self.t.component("boards", "board_w", register("board_api"), {"include/board_w.h": ""})
        self.t.component("app", "start", register("engine svc screens board_api lvgl__lvgl esp_timer"),
                         {"src/s.c": '#include "board_api.h"\n#include "lvgl.h"\n#include "esp_log.h"\n'})
        self.t.component("app", "start_bad", register("board_w"),
                         {"src/b.c": '#include "board_w.h"\n#if CONFIG_BOARD_X\n#endif\n'})
        kinds = self.kinds()
        self.assertEqual([k for k in kinds if k[0] == "start"], [])
        self.assertIn(("start_bad", "requires", "board_w"), kinds)
        self.assertIn(("start_bad", "include", "board_w.h"), kinds)
        self.assertIn(("start_bad", "option", "CONFIG_BOARD_X"), kinds)

    def test_services_and_the_ui_may_not_use_the_app(self):
        self.t.component("app", "start", register("svc"), {"include/start.h": ""})
        self.t.component("services", "upward", register("start"), {"src/u.c": '#include "start.h"\n'})
        kinds = self.kinds()
        self.assertIn(("upward", "requires", "start"), kinds)
        self.assertIn(("upward", "include", "start.h"), kinds)

    def test_tests_folders_are_not_checked(self):
        self.t.component("core", "tested", register(),
                         {"test/test_x.c": '#include "esp_log.h"\nvoid *p(void){return malloc(1);}\n'})
        self.assertEqual(self.kinds(), [])

    def test_the_index_must_name_every_component(self):
        self.t.write_index(skip=("svc",))
        code, out = self.t.run(index=False)
        self.assertEqual(code, 1)
        self.assertIn("svc: not listed in components/README.md", out)
        (self.t.root / "components" / "README.md").unlink()
        code, out = self.t.run(index=False)
        self.assertIn("components/README.md is missing", out)

    def test_a_component_name_used_twice_is_refused(self):
        self.t.component("drivers", "engine", register())
        with self.assertRaises(SystemExit):
            ca.discover(self.t.root)


class CMakeParsing(unittest.TestCase):
    def test_requirements_across_lines_variables_and_branches(self):
        text = textwrap.dedent('''
            set(extra_reqs alpha "beta")
            if(CONFIG_X)
                list(APPEND extra_reqs gamma)
            endif()
            idf_component_register(SRCS "a.c"
                                        "b.c"
                                   INCLUDE_DIRS include
                                   REQUIRES delta ${extra_reqs}
                                   PRIV_REQUIRES epsilon)  # REQUIRES commented_out
            target_link_libraries(${COMPONENT_LIB} PRIVATE idf::zeta)
        ''')
        self.assertEqual(ca.cmake_requirements(text),
                         {"alpha", "beta", "gamma", "delta", "epsilon", "zeta"})

    def test_host_branch_links_count_and_keywords_do_not(self):
        text = textwrap.dedent('''
            if(ESP_PLATFORM)
                idf_component_register(SRCS a.c REQUIRES core_a)
            else()
                add_library(me STATIC a.c)
                target_link_libraries(me PUBLIC core_b PRIVATE m)
                target_compile_options(me PRIVATE -Wall)
            endif()
        ''')
        self.assertEqual(ca.cmake_requirements(text), {"core_a", "core_b", "m"})

    def test_hash_inside_a_quoted_string_is_not_a_comment(self):
        self.assertEqual(ca.strip_cmake_comments('set(x "a#b") # c'), 'set(x "a#b") ')

    def test_c_comments_keep_line_numbers(self):
        text = "a\n/* one\ntwo */\nb // c\n"
        self.assertEqual(ca.strip_c_comments(text).count("\n"), text.count("\n"))


class Exceptions(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.close)
        self.t.component("core", "engine", register("esp_timer"),
                         {"port/p.c": '#include "esp_log.h"\n', "src/s.c": '#include "esp_err.h"\n'})

    def test_an_exception_silences_its_violation_only(self):
        code, out = self.t.run({("engine", "requires", "esp_timer"): "step 9"})
        self.assertEqual(code, 1)
        self.assertNotIn("requires esp_timer", out)
        self.assertIn("esp_log.h", out)

    def test_file_patterns_match_the_source_file(self):
        code, out = self.t.run({("engine", "requires", "esp_timer"): "step 9",
                                ("engine", "include", "file:port/*"): "step 5"})
        self.assertEqual(code, 1)
        self.assertNotIn("esp_log.h", out)
        self.assertIn("esp_err.h", out)

    def test_a_stale_exception_fails_so_the_list_only_shrinks(self):
        code, out = self.t.run({("engine", "requires", "esp_timer"): "step 9",
                                ("engine", "include", "*"): "step 5",
                                ("engine", "requires", "gone"): "done already"})
        self.assertEqual(code, 1)
        self.assertIn("STALE EXCEPTION  engine requires gone", out)


class Output(unittest.TestCase):
    def test_list_and_graph_name_every_component(self):
        t = Tree()
        self.addCleanup(t.close)
        t.component("core", "engine", register())
        t.component("services", "svc", register("engine"))
        listing, graph = io.StringIO(), io.StringIO()
        ca.print_list(t.root, listing)
        ca.print_graph(t.root, graph)
        self.assertIn("svc", listing.getvalue())
        self.assertIn("-> engine", listing.getvalue())
        self.assertIn("svc --> engine", graph.getvalue())
        self.assertIn("subgraph core", graph.getvalue())


class Repository(unittest.TestCase):
    def test_this_repository_follows_its_rules(self):
        out = io.StringIO()
        code = ca.run(ca.ROOT, ca.KNOWN_EXCEPTIONS, out)
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
