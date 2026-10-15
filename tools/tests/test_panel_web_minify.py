"""BL-122 (F7): the Panel Manager's files are minified before gzip, and nothing they mean changes."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PANEL_WEB = ROOT / "components/services/panel_web"
sys.path.insert(0, str(PANEL_WEB))
import minify_asset  # noqa: E402

js = lambda s: minify_asset.minify(s, "js")  # noqa: E731
css = lambda s: minify_asset.minify(s, "css")  # noqa: E731


def find_node():
    found = shutil.which("node")
    if found:
        return found
    local = os.environ.get("LOCALAPPDATA")
    if local:
        for p in sorted((Path(local) / "Programs").glob("node-*/node.exe")):
            return str(p)
    return None


class MinifyJs(unittest.TestCase):
    def test_comments_and_indentation_go_lines_stay(self):
        self.assertEqual(js("  a = 1;   // one\n\n  /* two */\n  b = 2;\n"), "a = 1;\nb = 2;\n")

    def test_division_is_not_a_regex(self):
        src = "x = a / b / c; // note\n"
        self.assertEqual(js(src), "x = a / b / c;\n")

    def test_regex_with_slash_in_class_and_quote(self):
        src = 'x = /[/]\\/"/g; // c\ny = 1;\n'
        self.assertEqual(js(src), 'x = /[/]\\/"/g;\ny = 1;\n')

    def test_regex_after_return(self):
        src = "function f(s) {\n  return /\\/\\/x/.test(s); // c\n}\n"
        self.assertEqual(js(src), "function f(s) {\nreturn /\\/\\/x/.test(s);\n}\n")

    def test_comment_markers_inside_strings(self):
        src = "a = '// not'; b = \"/* not */\"; c = 'http://x.test/a';\n"
        self.assertEqual(js(src), src)

    def test_template_with_slashes_kept(self):
        src = "a = `http://x // y /* z */`; // gone\n"
        self.assertEqual(js(src), "a = `http://x // y /* z */`;\n")

    def test_template_keeps_its_lines_and_indent(self):
        src = "a = `one\n    two`;\n  b = 1;\n"
        self.assertEqual(js(src), "a = `one\n    two`;\nb = 1;\n")

    def test_nested_template_and_comment_in_substitution(self):
        src = "a = `x${ f(`y${ '}' } //z`) /* c */ + 1}w`; // end\nb = 2;\n"
        out = js(src)
        self.assertNotIn("/* c */", out)
        self.assertIn("`y${ '}' } //z`", out)
        self.assertTrue(out.endswith("w`;\nb = 2;\n"))

    def test_block_comment_with_newline_still_breaks_the_line(self):
        self.assertEqual(js("a\n/* x\n y */ b\n"), "a\nb\n")
        self.assertEqual(js("a /* x\n y */ b\n"), "a\nb\n")

    def test_comment_between_tokens_does_not_join_them(self):
        self.assertEqual(js("return/**/x\n"), "return x\n")

    def test_undecided_keeps_the_rest_as_it_was(self):
        src = "a = 1; // c\n  b = 'oops\n  // kept\n"
        self.assertEqual(js(src), "a = 1;\n  b = 'oops\n  // kept\n")

    def test_idempotent(self):
        src = ("a = /x\\/y/g; // c\n"
               "b = `t ${ 1 /* c */ } u`;\n"
               "  if (a / 2 / 3) {\n"
               "    c = \"it's\";\n"
               "  }\n")
        once = js(src)
        self.assertEqual(js(once), once)


class MinifyCss(unittest.TestCase):
    def test_comments_removed(self):
        self.assertEqual(css("/* a */\n.x {\n  color: red; /* b */\n}\n"), ".x {\ncolor: red;\n}\n")

    def test_content_string_and_url_kept(self):
        src = '.a::before { content: "/*"; }\n.b { background: url(//x/y.png); }\n'
        self.assertEqual(css(src), src)

    def test_idempotent(self):
        once = css("/* a */\n  .x { content: '/*'; }\n\n")
        self.assertEqual(css(once), once)


class MinifyHtml(unittest.TestCase):
    def test_comments_and_indentation(self):
        src = "<div>\n  <!-- gone -->\n  <p>hi</p>\n\n</div>\n"
        self.assertEqual(minify_asset.minify(src, "html"), "<div>\n<p>hi</p>\n</div>\n")

    def test_pre_kept(self):
        src = "<pre>\n  a\n\n  b\n</pre>\n"
        self.assertEqual(minify_asset.minify(src, "html"), src)


class ShippedFiles(unittest.TestCase):
    def test_every_file_minifies_smaller_and_twice_the_same(self):
        for name in minify_asset.ASSETS:
            text = (PANEL_WEB / "web" / name).read_text(encoding="utf-8")
            once = minify_asset.minify(text, minify_asset.kind_of(name))
            self.assertLess(len(once), len(text), name)
            self.assertEqual(minify_asset.minify(once, minify_asset.kind_of(name)), once, name)

    def test_every_minified_script_still_parses(self):
        node = find_node()
        if not node:
            self.skipTest("Node not found")
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([sys.executable, str(PANEL_WEB / "minify_asset.py"), "--all",
                            str(PANEL_WEB / "web"), tmp], check=True)
            for name in minify_asset.ASSETS:
                if name.endswith(".js"):
                    r = subprocess.run([node, "--check", str(Path(tmp) / name)], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, name + "\n" + r.stderr)

    def test_gzip_asset_gives_the_minified_bytes(self):
        import gzip
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "x.gz"
            subprocess.run([sys.executable, str(PANEL_WEB / "gzip_asset.py"),
                            str(PANEL_WEB / "web/panel-words.js"), str(out)], check=True)
            want = minify_asset.minify((PANEL_WEB / "web/panel-words.js").read_text(encoding="utf-8"), "js")
            self.assertEqual(gzip.decompress(out.read_bytes()).decode("utf-8"), want)


if __name__ == "__main__":
    unittest.main()
