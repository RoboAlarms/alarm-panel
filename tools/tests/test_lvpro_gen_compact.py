"""docs/features/39 F10, BL-125: the generated screen code is compact. Runs of statements that repeat
become shared functions (<project>_runs_gen.c) and an object's name is set only where the
hand-written code looks it up. A screen is built exactly as the XML says: with the shared
functions expanded again, every statement is where it was."""
import importlib.util
import re
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
LAYOUTS = sorted((ROOT / "components/ui/alarm_ui/layouts").glob("*/alarm_ui"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("lvpro_gen")

RUN_CALL = re.compile(r"^(?:lv_obj_t \* (\w+) = )?\w+_run_(\d+)\((.*)\);$")
DECL = re.compile(r"^lv_obj_t \* (\w+) = (.*);$")
BARE_CREATE = re.compile(r"^\w+_create\(.*\);$")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def definitions(runs_c):
    """name -> (parameter names, statements) of the shared functions."""
    defs = {}
    pattern = re.compile(r"^(?:lv_obj_t \*|void) (\w+)\((.*?)\)\n\{\n(.*?)^\}", re.S | re.M)
    for m in pattern.finditer(runs_c):
        name, params, body = m.groups()
        pnames = [re.split(r"[ *]+", a.strip())[-1] for a in gen.split_args(params)]
        stmts = [ln.strip() for ln in body.splitlines()
                 if ln.strip() and not ln.strip().startswith("return")]
        defs[name] = (pnames, stmts)
    return defs


def statements(text, defs):
    """The body's statements, a shared function's call replaced by the statements it stands for."""
    out, fresh = [], 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("lv_obj_set_name", "/*", "*", "#")):
            continue
        m = RUN_CALL.match(line)
        name = re.match(r"^(?:lv_obj_t \* \w+ = )?(\w+)\(", line)
        if m and name and name.group(1) in defs:
            var, _, args = m.groups()
            pnames, stmts = defs[name.group(1)]
            values = dict(zip(pnames, gen.split_args(args)))
            if "parent" in values:  # a create: its object is `o`
                if var is None:
                    fresh += 1
                    var = "__new%d" % fresh
                values["o"] = var
            for st in stmts:
                st = WORD.sub(lambda w: values.get(w.group(0), w.group(0)), st)
                if st.startswith("lv_obj_set_name"):
                    continue
                out.append(st)
            continue
        out.append(line)
    return out


def canonical(stmts):
    """Objects numbered in order of creation, so a declared and an undeclared one compare."""
    names, out, count = {}, [], 0
    for st in stmts:
        d = DECL.match(st)
        if d:
            var, rest = d.groups()
            rest = WORD.sub(lambda w: names.get(w.group(0), w.group(0)), rest)
            count += 1
            names[var] = "#%d" % count
            out.append("CREATE " + rest)
        elif BARE_CREATE.match(st):
            count += 1
            out.append("CREATE " + WORD.sub(lambda w: names.get(w.group(0), w.group(0)), st))
        else:
            out.append(WORD.sub(lambda w: names.get(w.group(0), w.group(0)), st))
    return out


class SplitArgs(unittest.TestCase):
    def test_commas_inside_strings_and_brackets_stay(self):
        self.assertEqual(gen.split_args('o, "a, b", lv_pct(100), f(1, 2)'),
                         ["o", '"a, b"', "lv_pct(100)", "f(1, 2)"])
        self.assertEqual(gen.split_args('o, "x\\", y"'), ["o", '"x\\", y"'])
        self.assertEqual(gen.split_args(""), [])


class NamesTheCodeLooksUp(unittest.TestCase):
    def test_rule(self):
        words = {"pad_keys", "row_"}
        self.assertTrue(gen.name_is_looked_up("pad_keys", words))
        self.assertTrue(gen.name_is_looked_up("pad_keys_#", words))   # a component's numbered root
        self.assertTrue(gen.name_is_looked_up("row_3", words))        # built at run time: "row_%d"
        self.assertFalse(gen.name_is_looked_up("box_other", words))
        self.assertTrue(gen.name_is_looked_up("anything", None))      # no code to ask: keep

    def test_every_name_the_code_uses_survives(self):
        for layout in LAYOUTS:
            words = gen.code_words(layout)
            self.assertIn("pad_keys", words)
            text = "".join(f.read_text(encoding="utf-8")
                           for f in layout.rglob("*_gen.c"))
            for name in ("pad_keys", "pad_close", "title_label", "detail_label"):
                self.assertIn('"%s"' % name, text, "%s: name '%s' is gone" % (layout.parent.name, name))


class Compact(unittest.TestCase):
    def test_nothing_moves_when_the_shared_functions_are_expanded(self):
        for layout in LAYOUTS:
            p = gen.Project(layout)
            gen.COMPACT = False
            try:
                plain = gen.generate(gen.Project(layout))
            finally:
                gen.COMPACT = True
            compact = gen.generate(p)
            defs = definitions(compact["%s_runs_gen.c" % p.name])
            self.assertTrue(defs, "%s: no shared function" % layout.parent.name)
            for rel, text in plain.items():
                if not rel.endswith("_gen.c") or rel.endswith("_runs_gen.c"):
                    continue
                self.assertEqual(canonical(statements(text, {})),
                                 canonical(statements(compact[rel], defs)),
                                 "%s %s" % (layout.parent.name, rel))

    def test_fewer_calls_than_before(self):
        for layout in LAYOUTS:
            p = gen.Project(layout)
            gen.COMPACT = False
            try:
                plain = gen.generate(gen.Project(layout))
            finally:
                gen.COMPACT = True
            compact = gen.generate(p)

            def calls(files):
                return len(re.findall(r"lv_(?:obj|style)_set_\w+\(", "".join(
                    t for r, t in files.items() if r.endswith(".c"))))
            self.assertLess(calls(compact), calls(plain) * 0.7, layout.parent.name)

    def test_no_declared_object_goes_unused(self):
        # -Wunused-variable is an error in the PC build
        for layout in LAYOUTS:
            for file in layout.rglob("*_gen.c"):
                text = gen.STRING_LITERAL.sub('""', file.read_text(encoding="utf-8"))
                for var in re.findall(r"lv_obj_t \* (\w+) = ", text):
                    self.assertGreater(len(re.findall(r"\b%s\b" % var, text)), 1,
                                       "%s: %s is declared and unused" % (file.name, var))

    def test_committed_files_have_the_shared_functions(self):
        for layout in LAYOUTS:
            runs = layout / "alarm_ui_runs_gen.c"
            self.assertTrue(runs.is_file(), runs)
            self.assertIn("alarm_ui_runs_gen.c", (layout / "file_list_gen.cmake").read_text(
                encoding="utf-8"))
            self.assertIn('#include "alarm_ui_runs_gen.h"',
                          (layout / "alarm_ui_gen.h").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
