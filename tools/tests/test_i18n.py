"""docs/features/37 I18N-003, -004, -005, -008, -023, -024, -034, -035, -036: tools/i18n.py extract,
merge, check (both sides), build and pseudo."""
import contextlib
import importlib.util
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("i18n", TOOLS / "i18n.py")
i18n = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(i18n)


def ex(src, path="a.c", problems=None):
    return i18n.extract_c(src, path, problems)


def keys(src):
    return [(o.ctx, o.msgid, o.plural) for o in ex(src)]


class Tree:
    """A temporary repo root with sources, a template and a MARKED list."""

    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / i18n.PO_DIR).mkdir(parents=True)

    def close(self):
        self.tmp.cleanup()

    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(text.encode("utf-8"))

    def marked(self, *paths):
        self.write(i18n.MARKED_PATH, "# test\n" + "".join(p + "\n" for p in paths))

    def run(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = i18n.main(["--root", str(self.root)] + list(argv))
        return code, out.getvalue()

    def pot(self):
        return (self.root / i18n.POT_PATH).read_text(encoding="utf-8")


class Base(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.close)


class HashTest(unittest.TestCase):
    def test_vectors(self):
        self.assertEqual(i18n.fnv1a32(b""), 0x811C9DC5)
        self.assertEqual(i18n.fnv1a32(b"a"), 0xE40C292C)
        self.assertEqual(i18n.fnv1a32(b"foobar"), 0xBF9CF968)

    def test_context_is_part_of_the_key(self):
        self.assertEqual(i18n.key_hash(None, "foobar"), 0xBF9CF968)
        self.assertEqual(i18n.key_hash("door", "Open"), i18n.fnv1a32(b"door\x04Open"))


class ExtractMarkers(unittest.TestCase):
    def test_every_form(self):
        src = '''
        a = TR("One");
        b = TR_C("door", "Open");
        c = TRN("{0} zone open", "{0} zones open", n);
        d = TRN_C("z", "{0} sensor", "{0} sensors", n);
        e = TR_NOOP("Noop");
        f = TR_NOOP_C("c2", "Noop2");
        g = I18N_NOOP("Port");
        h = I18N_NOOP_C("c3", "Port2");
        '''
        self.assertEqual(keys(src), [
            (None, "One", None), ("door", "Open", None),
            (None, "{0} zone open", "{0} zones open"),
            ("z", "{0} sensor", "{0} sensors"),
            (None, "Noop", None), ("c2", "Noop2", None),
            (None, "Port", None), ("c3", "Port2", None)])

    def test_direct_calls_with_null_context_and_complex_first_argument(self):
        src = '''
        s = i18n_text(pick(cat, (1, 2)), NULL, "Hello");
        t = i18n_text(ui->cat[f(a, b)], "ctx", "World");
        u = i18n_plural(g(x, (y)), NULL, "{0} day", "{0} days", n);
        v = i18n_plural(cat, "c", "{0} hour", "{0} hours", n);
        '''
        self.assertEqual(keys(src), [
            (None, "Hello", None), ("ctx", "World", None),
            (None, "{0} day", "{0} days"), ("c", "{0} hour", "{0} hours")])

    def test_non_literals_are_skipped(self):
        src = 'a = TR(name); b = TR_C(ctx, "x"); c = TRN("a", other, n); d = i18n_text(c, k, "x");'
        self.assertEqual(keys(src), [])

    def test_nested_markers_are_both_found(self):
        self.assertEqual(keys('x = i18n_text(f(TR("Inner")), NULL, "Outer");'),
                         [(None, "Outer", None), (None, "Inner", None)])  # source order

    def test_concatenation_and_escapes(self):
        src = r'''x = TR("a" "b"
                   "c\n\t\"q\" \\ \x41" "\101");'''
        self.assertEqual(keys(src), [(None, "abc\n\t\"q\" \\ AA", None)])

    def test_nul_ends_the_text_like_in_c(self):
        self.assertEqual(keys(r'x = TR("ab\0cd");'), [(None, "ab", None)])
        self.assertEqual(keys(r'x = TR("ab\x00" "cd");'), [(None, "ab", None)])

    def test_universal_character_names(self):
        self.assertEqual(keys(r'x = TR("caf\u00e9 \U0001F600 \u20AC");'),
                         [(None, "caf\u00e9 \U0001F600 \u20AC", None)])
        self.assertEqual(keys(r'x = TR("a\u0000b");'), [(None, "a", None)])

    def test_empty_context_is_no_context(self):
        # I18N-001: C's i18n_hash("", x) is i18n_hash(NULL, x)
        self.assertEqual(keys('x = TR_C("", "Open"); y = i18n_text(c, "", "Shut");'),
                         [(None, "Open", None), (None, "Shut", None)])
        self.assertEqual(i18n.key_hash(ex('x = TR_C("", "a");')[0].ctx, "a"), i18n.fnv1a32(b"a"))
        t = Tree()
        try:
            t.write("components/a/x.c", 'x = TR_C("", "Open"); y = TR("Open");')
            t.run("extract")
            self.assertNotIn("msgctxt", t.pot())
            self.assertEqual(t.pot().count('msgid "Open"'), 1)
        finally:
            t.close()

    def test_null_forms_of_no_context(self):
        self.assertEqual(keys('a = i18n_text(c, (NULL), "A"); b = i18n_text(c, 0, "B");'
                              'd = TR_C(NULL, "D");'),
                         [(None, "A", None), (None, "B", None), (None, "D", None)])
        self.assertEqual(keys('a = TR_C("NULL", "A");'), [("NULL", "A", None)])

    def test_literals_that_are_not_plain_are_problems(self):
        # a prefixed literal or a conditional is an error, not accepted (u8"..." included)
        for src in ('x = TR(c ? "Open" : "Closed");', 'x = TR(u8"Hi");', 'x = TR(L"Hi");',
                    'x = TR(u"Hi");', 'x = TR(U"Hi");', 'x = TRN("a", u8"b", n);',
                    'x = TR_C(c ? "a" : "b", "x");', 'x = TR_C(u8"a", "x");',
                    'x = TR("a" u8"b");'):
            problems = []
            self.assertEqual(ex("\n" + src, problems=problems), [], src)
            self.assertEqual(len(problems), 1, src)
            self.assertEqual(problems[0][:2], ("a.c", 2), src)

    def test_variables_are_not_problems(self):
        problems = []
        ex('x = TR(name); y = TR_C(ctx, "x"); z = TRN("a", other, n);', problems=problems)
        self.assertEqual(problems, [])

    def test_problems_reach_check_and_extract(self):
        t = Tree()
        try:
            t.write("components/a/x.c", 'int a;\nx = TR(c ? "Open" : "Closed");\n')
            code, out = t.run("extract")
            self.assertIn("components/a/x.c:2: warning:", out)
            code, out = t.run("check")
            self.assertEqual(code, 1)
            self.assertIn("components/a/x.c:2:", out)
        finally:
            t.close()

    def test_utf8_and_hex_bytes(self):
        self.assertEqual(keys('x = TR("caf\u00e9 \\xc3\\xa9");'), [(None, "caf\u00e9 \u00e9", None)])

    def test_multi_line_call_and_line_number(self):
        occ = ex('\n\nx = TRN(\n  "{0} a",\n  "{0} b",\n  n);')
        self.assertEqual((occ[0].line, occ[0].plural), (3, "{0} b"))

    def test_markers_in_macros_and_initialisers(self):
        src = '''#define LABEL(x) TR(x "!")
#define HELP TR("Help me")
static const char *k[] = { TR_NOOP("First"), TR_NOOP("Second") };'''
        self.assertEqual([k[1] for k in keys(src)], ["Help me", "First", "Second"])

    def test_comments_are_ignored(self):
        src = '''// TR("no")
        /* TR("nor this")
           TR("or this") */
        x = TR("yes"); // TR("tail")'''
        self.assertEqual([k[1] for k in keys(src)], ["yes"])

    def test_marker_definitions_are_ignored(self):
        src = '''#define TR(s) ui_tr(s)
#define TR_C(c, s) ui_trc(c, s)
#define I18N_NOOP(s) (s)
#define TRN(a, b, n) ui_trn(a, b, n)
const char *i18n_text(const void *c, const char *ctx, const char *english);
x = TR("Real");'''
        self.assertEqual([k[1] for k in keys(src)], ["Real"])

    def test_not_a_marker_word(self):
        self.assertEqual(keys('x = STR("no"); y = TRX("no"); TR ("spaced");'),
                         [(None, "spaced", None)])

    def test_empty_text_is_not_extracted(self):
        self.assertEqual(keys('x = TR("");'), [])


class ExtractNotes(unittest.TestCase):
    def test_note_above_and_same_line(self):
        src = '''/* TR: Home, the line under the ring. At most 40 bytes. */
        a = TR("Ready");
        b = TR("Other"); // TR: on the line
        c = TR("Third");
        // TR: unrelated
        '''
        occ = ex(src)
        self.assertEqual(occ[0].notes, ["Home, the line under the ring. At most 40 bytes."])
        self.assertEqual(occ[0].budget, 40)
        self.assertEqual(occ[1].notes, ["on the line"])
        self.assertEqual(occ[2].notes, [])  # a trailing note belongs to its own line only

    def test_note_above_a_wrapped_statement(self):
        src = '''/* TR: At most 4 bytes. */
        ui_toast(
            TR("Long text"));
        // TR: for the pair
        f(a,
          TR("One"), TR("Two"));
        int k[] = {
            // TR: first only
            TR_NOOP("First"),
            TR_NOOP("Second"),
        };
        y = TR("Alone");
        '''
        occ = ex(src)
        self.assertEqual([(o.msgid, o.notes) for o in occ], [
            ("Long text", ["At most 4 bytes."]), ("One", ["for the pair"]), ("Two", ["for the pair"]),
            ("First", ["first only"]), ("Second", []), ("Alone", [])])
        self.assertEqual(len(i18n.check_budgets(occ)), 1)

    def test_multi_line_block_note_and_plain_comments(self):
        src = '''/* not a note */
        /* TR: first line
         * second line */
        a = TR("X");'''
        occ = ex(src)
        self.assertEqual(occ[0].notes, ["first line second line"])

    def test_notes_merge_without_repeats_and_lowest_budget(self):
        t = Tree()
        try:
            t.write("components/x/a.c", '// TR: Same. At most 9 bytes.\nx = TR("Hi");\n')
            t.write("components/x/b.c", '// TR: Same. At most 9 bytes.\ny = TR("Hi");\n'
                                        '// TR: Other\nz = TR("Hi");\n')
            entries = i18n.build_entries(i18n.extract_all(t.root))
            e = entries[(None, "Hi")]
            self.assertEqual(e.notes, ["Same. At most 9 bytes.", "Other"])
            self.assertEqual(e.places, ["components/x/a.c", "components/x/b.c"])
        finally:
            t.close()


class ExtractNoteRepairs(unittest.TestCase):
    def test_note_after_a_case_label(self):
        src = '''switch (m) {
        case A:
            // TR: title one
            f(m == X ? TR("One") : TR("Two"),
              y);
            break;
        default:
            /* TR: title three */
            g(z,
              TR("Three"));
        }'''
        occ = ex(src)
        self.assertEqual([(o.msgid, o.notes) for o in occ],
                         [("One", ["title one"]), ("Two", ["title one"]), ("Three", ["title three"])])

    def test_note_above_a_table_reaches_its_elements(self):
        src = '''/* TR: the report line. At most 9 bytes. */
        static const char *const WORDS[] = {
            TR_NOOP("First"),
            // TR: own note
            TR_NOOP("Second"),
            TR_NOOP("Third"
                    "part"),
        };
        y = TR("Outside");
        '''
        occ = {o.msgid: o for o in ex(src)}
        self.assertEqual(occ["First"].notes, ["the report line. At most 9 bytes."])
        self.assertEqual(occ["First"].budget, 9)
        self.assertEqual(occ["Second"].notes, ["own note"])
        self.assertEqual(occ["Thirdpart"].notes, ["the report line. At most 9 bytes."])
        self.assertEqual(occ["Outside"].notes, [])

    def test_note_above_a_nested_table(self):
        src = '''// TR: rows
        static const row_t ROWS[] = {
            {1, TR_NOOP("A")},
            {2, TR_NOOP("B")},
        };'''
        self.assertEqual([o.notes for o in ex(src)], [["rows"], ["rows"]])


class BlockNotes(unittest.TestCase):
    def test_note_above_a_switch_reaches_its_cases(self):
        src = '''void f(int m) {
        /* TR: state word. At most 9 bytes. */
        switch (m) {
        case 1: return TR("Silent");
        case 2:
            // TR: own note
            return TR("Loud");
        default: break;
        }
        }'''
        occ = {o.msgid: o for o in ex(src)}
        self.assertEqual(occ["Silent"].notes, ["state word. At most 9 bytes."])
        self.assertEqual(occ["Silent"].budget, 9)
        self.assertEqual(occ["Loud"].notes, ["own note"])

    def test_nested_blocks_use_the_nearest_note(self):
        src = '''void f(int m, int k) {
        // TR: outer
        switch (m) {
        case 1:
            // TR: inner
            if (k) {
                x = TR("A");
            }
            if (k) {
                x = TR("B");
            }
            break;
        }
        }'''
        occ = {o.msgid: o.notes for o in ex(src)}
        self.assertEqual(occ["A"], ["inner"])
        self.assertEqual(occ["B"], ["outer"])

    def test_if_and_else(self):
        src = '''void f(int k) {
        // TR: yes
        if (k) {
            x = TR("Yes");
        }
        // TR: no
        else {
            x = TR("No");
        }
        }'''
        occ = {o.msgid: o.notes for o in ex(src)}
        self.assertEqual(occ["Yes"], ["yes"])
        self.assertEqual(occ["No"], ["no"])

    def test_function_body_does_not_pass_its_comment(self):
        src = '''/* TR: not for the body */
        void f(int k) {
            x = TR("Body");
        }'''
        self.assertEqual([o.notes for o in ex(src)], [[]])


class StructTableNotes(unittest.TestCase):
    def test_note_above_a_table_of_an_inline_struct(self):
        src = '''void f(void) {
        /* TR: a title. At most 9 bytes. */
        static const struct {
            int icon;
            const char *title;
        } table[2] = {
            [0] = {1, TR_NOOP("One")},
            [1] = {2, TR_NOOP("Two")},
        };
        }'''
        self.assertEqual([(o.notes, o.budget) for o in ex(src)],
                         [(["a title. At most 9 bytes."], 9)] * 2)


class CallNotes(unittest.TestCase):
    def test_direct_argument_gets_note_and_budget(self):
        occ = ex('void f(void) { ui_toast(TR("Saved")); ui_set_str(&s, TR("Hi")); }')
        self.assertEqual(occ[0].notes, ["A toast (one line over the screen). At most 127 bytes."])
        self.assertEqual(occ[0].budget, 127)
        self.assertEqual((occ[1].notes, occ[1].budget), (["At most 127 bytes."], 127))

    def test_per_argument_budget(self):
        occ = ex('ui_pad_ask(h, g, TR("Title"), TR("Hint"), t, fn); '
                 'ui_pad_ask_new(h, g, TR("New"), fn);')
        self.assertEqual([o.budget for o in occ], [63, 95, 63])

    def test_not_direct_or_unknown_or_own_budget(self):
        occ = ex('ui_toast(cond ? TR("A") : TR("B"));\n'
                 'ui_toast(fmt(TR("Nested")));\nother(TR("Unknown"));\n'
                 '/* TR: At most 20 bytes. */\nui_toast(TR("Own"));\n'
                 'ui_pad_ask(h, g, t, TR("Hint"), NULL, fn);\n')
        got = {o.msgid: o.budget for o in occ}
        self.assertEqual(got, {"A": 127, "B": 127, "Nested": None, "Unknown": None, "Own": 20,
                               "Hint": 95})

    def test_budget_is_checked(self):
        occ = ex('ui_pad_ask_new(h, g, TR("' + "x" * 64 + '"), fn);')
        self.assertEqual(len(i18n.check_budgets(occ)), 1)


class NoteGaps(Base):
    def test_summary_and_list(self):
        self.t.write("components/a/x.c",
                     '/* TR: Noted. At most 9 bytes. */\na = TR("Both");\n'
                     '// TR: only a note\nb = TR("Note only");\n'
                     'ui_toast(TR("Auto"));\nc = TR("Neither");\n')
        self.t.run("extract")
        code, out = self.t.run("check")
        self.assertEqual(code, 0)
        self.assertIn("1 of 4 entries have no translator note, 2 have no byte budget", out)
        code, out = self.t.run("check", "--notes")
        self.assertEqual(code, 0)
        self.assertIn('components/a/x.c:6: no note: "Neither"', out)
        self.assertIn('components/a/x.c:4: no budget: "Note only"', out)
        self.assertNotIn("no note: \"Both\"", out)


class Template(Base):
    def test_header_only(self):
        code, _ = self.t.run("extract")
        self.assertEqual(code, 0)
        pot = self.t.pot()
        self.assertIn('"Project-Id-Version: RoboAlarms\\n"', pot)
        self.assertIn('"Content-Type: text/plain; charset=UTF-8\\n"', pot)
        self.assertNotIn("POT-Creation-Date", pot)
        self.assertNotIn("\r", pot)

    def test_entries_scan_rules_order_and_dedup(self):
        t = self.t
        t.write("components/b/src/two.c", 'x = TR("Shared"); y = TR("Second");')
        t.write("components/a/src/one.c",
                '/* TR: Note "quoted". */\nx = TR("First \\"q\\"\\n");\ny = TR("Shared");\n'
                'z = TR_C("ctx", "Shared"); w = TRN("{0} a", "{0} b", n);\nv = TR("Shared");')
        t.write("components/a/test/skipped.c", 'x = TR("In test");')
        t.write("components/a/src/skip_gen.c", 'x = TR("Generated");')
        t.write("components/a/third_party/lib.c", 'x = TR("Third");')
        t.write("components/a/fonts/f.c", 'x = TR("Font");')
        t.write("components/a/src/readme.txt", 'x = TR("Text");')
        t.write("firmware/x/main.c", 'x = TR("Outside components");')
        self.assertEqual(t.run("extract")[0], 0)
        pot = t.pot()
        self.assertNotIn("In test", pot)
        self.assertNotIn("Generated", pot)
        self.assertNotIn("Third", pot)
        self.assertNotIn("Font", pot)
        self.assertNotIn("Text", pot)
        self.assertNotIn("Outside", pot)
        self.assertEqual(pot.count('msgid "Shared"'), 2)  # one plain, one with a context
        self.assertLess(pot.index('msgid "First'), pot.index('msgid "Shared"'))
        self.assertLess(pot.index('msgid "Shared"'), pot.index('msgid "Second"'))
        self.assertIn('#. Note "quoted".\n#: components/a/src/one.c\nmsgid "First \\"q\\"\\n"\n', pot)
        self.assertIn('#: components/a/src/one.c\n#: components/b/src/two.c\nmsgid "Shared"\n', pot)
        self.assertIn('msgctxt "ctx"\nmsgid "Shared"\nmsgstr ""', pot)
        self.assertIn('msgid "{0} a"\nmsgid_plural "{0} b"\nmsgstr[0] ""\nmsgstr[1] ""', pot)
        self.assertNotIn(":1", pot.replace("components/", ""))  # no line numbers in #:

    def test_deterministic(self):
        self.t.write("components/a/x.c", 'x = TR("A"); y = TR("B");')
        self.t.run("extract")
        first = self.t.pot()
        self.t.run("extract")
        self.assertEqual(first, self.t.pot())

    def test_check_flag(self):
        self.t.write("components/a/x.c", 'x = TR("A");')
        code, out = self.t.run("extract", "--check")  # no file yet
        self.assertEqual(code, 1)
        self.assertIn("out of date", out)
        self.assertFalse((self.t.root / i18n.POT_PATH).exists())  # wrote nothing
        self.t.run("extract")
        self.assertEqual(self.t.run("extract", "--check")[0], 0)
        self.t.write("components/a/x.c", 'x = TR("A"); y = TR("B");')
        before = self.t.pot()
        self.assertEqual(self.t.run("extract", "--check")[0], 1)
        self.assertEqual(before, self.t.pot())


class Checks(Base):
    def sources(self, src, path="components/a/x.c"):
        self.t.write(path, src)

    def check(self):
        self.t.run("extract")
        return self.t.run("check")

    def test_clean_tree_passes(self):
        self.sources('x = TR("Hello {0}"); y = TRN("{0} day", "{0} days", n);')
        code, out = self.check()
        self.assertEqual((code, out.strip().split("\n")[-1]), (0, "i18n check: ok (2 entries)"))

    def test_template_out_of_date(self):
        self.sources('x = TR("A");')
        self.t.run("extract")
        self.sources('x = TR("A"); y = TR("B");')
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn("messages.pot:1: template is out of date", out)

    def test_collision_real(self):
        # "costarring" and "liquid" share FNV-1a 32 (0x5e4daa9d)
        self.assertEqual(i18n.fnv1a32(b"costarring"), i18n.fnv1a32(b"liquid"))
        self.sources('x = TR("costarring"); y = TR("liquid");')
        code, out = self.check()
        self.assertEqual(code, 1)
        self.assertIn("hash collision 0x5e4daa9d", out)
        self.assertIn("components/a/x.c:1:", out)

    def test_collision_fixed_by_context(self):
        self.sources('x = TR("costarring"); y = TR_C("c", "liquid");')
        self.assertEqual(self.check()[0], 0)

    def test_collision_with_injected_hash(self):
        entries = i18n.build_entries(ex('x = TR("A"); y = TR("B"); z = TR("C");'))
        self.assertEqual(i18n.check_collisions(entries), [])
        errors = i18n.check_collisions(entries, hashfn=lambda ctx, msgid: 7 if msgid != "A" else 1)
        self.assertEqual(len(errors), 1)
        self.assertIn('"B"', errors[0][2])

    def test_two_plurals(self):
        self.sources('x = TRN("{0} a", "{0} b", n);\ny = TRN("{0} a", "{0} c", n);')
        code, out = self.check()
        self.assertEqual(code, 1)
        self.assertIn("components/a/x.c:2:", out)
        self.assertIn("two plurals", out)

    def test_plain_and_plural_of_one_key(self):
        self.sources('x = TR("{0} zone");\ny = TRN("{0} zone", "{0} zones", n);')
        code, out = self.check()
        self.assertEqual(code, 1)
        self.assertIn("components/a/x.c:1:", out)
        self.assertIn("plural at components/a/x.c:2", out)

    def test_plain_and_plural_with_a_context_apart_is_fine(self):
        self.sources('x = TR("{0} zone");\ny = TRN_C("c", "{0} zone", "{0} zones", n);')
        self.assertEqual(self.check()[0], 0)

    def test_same_plural_twice_is_fine(self):
        self.sources('x = TRN("{0} a", "{0} b", n);\ny = TRN("{0} a", "{0} b", m);')
        self.assertEqual(self.check()[0], 0)

    def test_placeholders(self):
        good = ['{0} and {9}', '{{literal}} {1}', 'none']
        bad = ['{name}', '{10}', '{}', '{', 'a } b', '{0']
        for s in good:
            entries = i18n.build_entries(ex('x = TR("%s");' % s))
            self.assertEqual(i18n.check_placeholders(entries), [], s)
        for s in bad:
            entries = i18n.build_entries(ex('x = TR("%s");' % s))
            self.assertEqual(len(i18n.check_placeholders(entries)), 1, s)

    def test_plural_needs_every_placeholder(self):
        ok = i18n.build_entries(ex('x = TRN("One zone", "{0} zones", n);'))
        self.assertEqual(i18n.check_placeholders(ok), [])
        also = i18n.build_entries(ex('x = TRN("{0} of {1}", "{0} of {1} all", n);'))
        self.assertEqual(i18n.check_placeholders(also), [])
        bad = i18n.build_entries(ex('x = TRN("{0} of {1}", "{0} zones", n);'))
        errors = i18n.check_placeholders(bad)
        self.assertEqual(len(errors), 1)
        self.assertIn("lacks {1}", errors[0][2])

    def test_printf_and_markup(self):
        bad = ["Zone %d open", "%s", "%-5.2f", "%lu items", "%zu", "100%%", "%n", "%5.1lf",
               "<b>Hi</b>", "a <br> b", "</p>", "R&amp;D", "&#169; x", "&#x2014;"]
        good = ["Zone {0} open", "50 percent", "a < b and c > d", "Tom & Jerry", "rate: 5%", "100% of them", "50% done", "a % b"]
        for s in bad:
            entries = i18n.build_entries(ex('x = TR("%s");' % s.replace("%", "%")))
            self.assertEqual(len(i18n.check_printf_and_markup(entries)), 1, s)
        for s in good:
            entries = i18n.build_entries(ex('x = TR("%s");' % s))
            self.assertEqual(i18n.check_printf_and_markup(entries), [], s)

    def test_js_markers(self):
        src = ("const a = t('Overview');\n"
               "// TR: the page's title\n"
               "b(tc('zone filter', \"Open\"), tn('{0} zone', '{0} zones', n));\n"
               "const c = {title: tx(`Display and sound`)};\n"
               "d(t(name), t(`Hi ${x}`), obj.t('not ours'), t('It\\'s \\u00e9'));\n")
        occ = i18n.extract_js(src, "components/services/panel_web/web/x.js")
        got = set((o.ctx, o.msgid, o.plural) for o in occ)
        self.assertEqual(got, set([(None, "Overview", None), ("zone filter", "Open", None),
                                      (None, "{0} zone", "{0} zones"),
                                      (None, "Display and sound", None),
                                      (None, "It's \u00e9", None)]))
        tc = [o for o in occ if o.ctx == "zone filter"][0]
        self.assertEqual(tc.notes, ["the page's title"])

    def test_object_names_are_not_words(self):
        for s in ("detail_label", "sheet_kb", "title_label", "zw_row_3"):
            entries = i18n.build_entries(ex('x = TR("%s");' % s))
            self.assertEqual(len(i18n.check_identifiers(entries)), 1, s)
        for s in ("Detail", "Wi-Fi", "Zone {0}", "the label", "e-mail", "a_b c"):
            entries = i18n.build_entries(ex('x = TR("%s");' % s))
            self.assertEqual(i18n.check_identifiers(entries), [], s)

    def test_printf_in_plural(self):
        entries = i18n.build_entries(ex('x = TRN("{0} a", "%d a", n);'))
        self.assertEqual(len(i18n.check_printf_and_markup(entries)), 1)

    def test_budget(self):
        over = ex('/* TR: At most 5 bytes. */\nx = TR("Toolong");')
        self.assertEqual(len(i18n.check_budgets(over)), 1)
        exact = ex('/* TR: At most 5 bytes. */\nx = TR("Fives");')
        self.assertEqual(i18n.check_budgets(exact), [])
        counted = ex('/* TR: At most 5 bytes. */\nx = TR("{0} ab {1}");')  # placeholders not counted
        self.assertEqual(i18n.check_budgets(counted), [])
        multi = ex('/* TR: At most 3 bytes. */\nx = TR("\u00e9\u00e9");')  # 4 bytes in UTF-8
        self.assertEqual(len(i18n.check_budgets(multi)), 1)
        plural = ex('/* TR: At most 3 bytes. */\nx = TRN("{0} a", "{0} abc", n);')
        self.assertEqual(len(i18n.check_budgets(plural)), 1)
        self.assertEqual(i18n.check_budgets(ex('x = TR("No budget at all");')), [])

    def test_budget_through_the_command(self):
        self.sources('/* TR: At most 3 bytes. */\nx = TR("Long");')
        code, out = self.check()
        self.assertEqual(code, 1)
        self.assertIn("components/a/x.c:2:", out)
        self.assertIn("4 bytes", out)


class Unmarked(Base):
    def marked_check(self, src):
        self.t.write("components/u/x.c", src)
        self.t.marked("components/u/x.c")
        self.t.run("extract")
        return self.t.run("check")

    def test_reported(self):
        code, out = self.marked_check('void f(void) {\n  show("Cancel arming");\n}\n')
        self.assertEqual(code, 1)
        self.assertIn('components/u/x.c:2: unmarked text "Cancel arming"', out)

    def test_capitalised_single_word_and_lower_with_space(self):
        code, out = self.marked_check('a("Friday"); b("open now"); c("ok");')
        self.assertEqual(out.count("unmarked text"), 2)

    def test_not_word_like(self):
        code, out = self.marked_check(
            'a("subject_view"); b("%d"); c("x"); d("ui_app"); e(" "); f("12:30"); g("ab");')
        self.assertEqual(code, 0)

    def test_inside_markers_and_notr(self):
        code, out = self.marked_check(
            'a(TR("Cancel arming")); b(NOTR("Fixed Words")); c(TRN("A day", "B days", n));'
            'd(i18n_text(cat, NULL, "Some words"));')
        self.assertEqual((code, out.count("unmarked")), (0, 0))

    def test_exempt_calls_and_include(self):
        code, out = self.marked_check(
            '#include "some file.h"\n'
            'void f(void) {\n'
            '  LV_LOG_WARN("Lost the link");\n'
            '  ESP_LOGI(TAG, "Going to sleep now");\n'
            '  printf("Hello world");\n'
            '  fprintf(stderr, "Hello world");\n'
            '  if (strcmp(s, "Open door") == 0) {}\n'
            '  strncmp(s, "Open door", 4); strstr(s, "Some Text"); strcasecmp(s, "Some Text");\n'
            '  puts("Some Text"); lv_obj_find_by_name(root, "Zone Card");\n'
            '}\n')
        self.assertEqual((code, out.strip().split("\n")[-1]), (0, "i18n check: ok (0 entries)"))

    def test_marker_in_wrapped_call_still_exempts_and_others_report(self):
        code, out = self.marked_check('x(lv_label_set_text(l, TR("Fine words")), "Bad words");')
        self.assertEqual(out.count("unmarked text"), 1)
        self.assertIn("Bad words", out)

    def test_comments_do_not_count(self):
        code, out = self.marked_check('/* "Some words" */ // "More words"\nint x;')
        self.assertEqual(code, 0)

    def test_only_marked_files_are_checked(self):
        self.t.write("components/u/x.c", 'a("Unmarked words");')
        self.t.write("components/u/y.c", 'a("Also unmarked");')
        self.t.marked("components/u/y.c")
        self.t.run("extract")
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn("y.c:1", out)
        self.assertNotIn("x.c", out)

    def test_marked_file_missing(self):
        self.t.marked("components/u/nope.c")
        self.t.run("extract")
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn("MARKED:2: components/u/nope.c is listed but does not exist", out)

    def test_marked_file_comments_and_blank_lines(self):
        self.t.write(i18n.MARKED_PATH, "# one\n\ncomponents/u/x.c  # trailing\n")
        self.t.write("components/u/x.c", 'a("Some words");')
        self.t.run("extract")
        self.assertEqual(self.t.run("check")[0], 1)

    def test_unmarked_all_is_a_report(self):
        self.t.write("components/u/x.c", 'a("Some words"); b(TR("Marked one"));')
        self.t.write("components/u/y.c", 'a("Other words");')
        code, out = self.t.run("check", "--unmarked-all")
        self.assertEqual(code, 0)
        self.assertIn("components/u/x.c:1: unmarked text \"Some words\"", out)
        self.assertIn("components/u/y.c:1:", out)
        self.assertIn("2 unmarked literals in 2 files", out)


class PanelManagerWords(unittest.TestCase):
    """docs/features/37 I18N-031, I18N-034: the Panel Manager's JavaScript and page are held to the
    same rule as the C modules: every word a person reads goes through the catalog."""

    PATH = "components/services/panel_web/web/x.js"

    def unmarked(self, src, loose=False):
        return [s for _, _, s in i18n.unmarked_js(src, self.PATH, loose)]

    def test_lexer_follows_templates_and_regular_expressions(self):
        src = "a = `x ${b ? `y ${c}` : '}'} z`; d = /['\"`]/g.test(e) / 2; // 'Not words'\n"
        kinds = [(k, v) for k, v, _ in i18n.lex_js(src)]
        self.assertIn(("tpl", ["x ", " z"]), kinds)
        self.assertIn(("tpl", ["y ", ""]), kinds)
        self.assertIn(("str", "}"), kinds)
        self.assertTrue(any(k == "re" for k, _ in kinds))
        self.assertNotIn(("str", "Not words"), kinds)

    def test_words_outside_markers_are_found(self):
        self.assertEqual(self.unmarked("toast('Save changes');"), ["Save changes"])
        self.assertEqual(self.unmarked("x = `<h2>Recent activity</h2><p>${esc(y)}</p>`;"), ["Recent activity"])
        self.assertEqual(self.unmarked('x = `<span title="Arm-only codes">-</span>`;'), ["Arm-only codes"])
        self.assertEqual(self.unmarked("x = `Disarmed. ${n} zones open.`;"), ["Disarmed.", "zones open."])

    def test_marked_fixed_and_not_words_are_not(self):
        src = ("t('Save changes'); ht('Recent activity'); tc('zone filter', 'Open'); tn('A day', 'B days', n);\n"
               "hx('Open {0}.', link); tx('Overview'); nt('Home Assistant');\n"
               "$('#dialog-content input'); x.querySelector('.card .row'); api('config/Display');\n"
               "headers = {'Content-Type': 'application/json'}; if (e.name === 'TimeoutError') {}\n"
               "switch (k) { case 'Escape': break; }\n"
               "x = `<div class=\"${cls}\" data-x=\"${v}\">${ht('Fine words')}</div>`;\n"
               "y = a ? 'danger solid' : 'primary';\n")
        self.assertEqual(self.unmarked(src), [])

    def test_loose_finds_lower_case_phrases(self):
        self.assertEqual(self.unmarked("x = 'just now';"), [])
        self.assertEqual(self.unmarked("x = 'just now';", loose=True), ["just now"])

    def test_page_words(self):
        html = ('<a data-t>Skip to content</a>\n<nav aria-label="Pages" data-t-label></nav>\n'
                '<span><span translate="no">RoboAlarms</span><small data-t>Panel Manager</small></span>\n'
                '<p>Plain words here</p><button aria-label="Open menu"></button>\n')
        got = [(o.line, o.msgid) for o in i18n.extract_html(html, "panel.html")]
        self.assertEqual(got, [(1, "Skip to content"), (2, "Pages"), (3, "Panel Manager")])
        found = [s for _, _, s in i18n.unmarked_html(html, "panel.html")]
        self.assertEqual(found, ["Plain words here", "Open menu"])

    def test_the_new_markers_are_extracted(self):
        occ = i18n.extract_js("ht('Recent activity'); hx('Open {0}.', link); nt('Zigbee');", self.PATH)
        self.assertEqual(sorted(o.msgid for o in occ), ["Open {0}.", "Recent activity"])


ROOT = TOOLS.parent
PLURAL_C = ROOT / "components/core/i18n/src/i18n_plural.c"

SRC = ('a(TR("Arm Away")); b(TR_C("door", "Open")); c(TR_C("page", "Open")); '
       '/* TR: Count on Home. At most 20 bytes. */ d(TRN("{0} zone open", "{0} zones open", n)); '
       'e(TR("Café {0}"));')


def decode_c_literals(text):
    """The bytes of every C string literal in text, joined (octal and simple escapes)."""
    out = bytearray()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        i = 0
        while i < len(lit):
            c = lit[i]
            i += 1
            if c != "\\":
                out += c.encode("utf-8")
            elif lit[i] in "01234567":
                j = i
                while j < len(lit) and j < i + 3 and lit[j] in "01234567":
                    j += 1
                out.append(int(lit[i:j], 8))
                i = j
            else:
                out += {"n": b"\n", "t": b"\t", "r": b"\r"}.get(lit[i], lit[i].encode())
                i += 1
    return bytes(out)


def read_generated_c(text, flags=None):
    """([(hash, offset)], string block bytes, preview, locale, rule) of a generated catalog file;
    with a dict for `flags`, each entry's flag names by hash go into it."""
    found = re.findall(r"\{0x([0-9a-f]{8})u, (\d+)u((?: \| I18N_ENTRY_[A-Z]+)*)\}", text)
    entries = [(int(h, 16), int(o)) for h, o, _ in found]
    if flags is not None:
        flags.update({int(h, 16): re.findall(r"I18N_ENTRY_[A-Z]+", f) for h, _, f in found})
    block = text.split("static const char strings[] =")[1].split("    ;")[0]
    m = re.search(r'"([^"]+)", (\w+), (\d), (\d+)u, entries, strings,', text)
    return entries, decode_c_literals(block), int(m.group(3)), m.group(1), m.group(2)


class PluralMirror(unittest.TestCase):
    """I18N-004: the Python rule table cannot drift from i18n_plural.c."""

    def setUp(self):
        self.c = PLURAL_C.read_text(encoding="utf-8")

    def test_language_lists_match(self):
        other = re.search(r"other\[\] = \{([^}]*)\}", self.c).group(1)
        es = re.search(r"es\[\] = \{([^}]*)\}", self.c).group(1)
        self.assertEqual(tuple(re.findall(r'"(\w+)"', other)), i18n.LANGS_OTHER)
        self.assertEqual(tuple(re.findall(r'"(\w+)"', es)), i18n.LANGS_ES)
        named = set(re.findall(r'primary_is\(locale, "(\w+)"\)', self.c))
        self.assertEqual(named, {"pt", "fr", "ar", "he"})

    def test_forms_match(self):
        body = self.c.split("i18n_plural_forms(")[1].split("static char lower")[0]
        found = {}
        pending = []
        forms = []
        for line in body.splitlines():
            m = re.search(r"case I18N_PLURALS_(\w+):", line)
            if m:
                pending.append(m.group(1))
            m = re.search(r"out\[(\d)\] = I18N_(\w+);", line)
            if m:
                forms.append(m.group(2).lower())
            if "return" in line and forms:
                for name in pending:
                    found[name] = tuple(forms)
                pending, forms = [], []
            elif "return" in line:
                pending = []
        for key, (enum, cats, _) in i18n.PLURAL_RULES.items():
            self.assertEqual(found[key], cats, key)
            self.assertEqual(enum, "I18N_PLURALS_" + key)

    def test_locale_to_rule(self):
        cases = {"en-US": "EN", "es-US": "ES", "it": "ES", "ca-ES": "ES", "fr-CA": "FR",
                 "pt-BR": "FR", "pt": "FR", "pt-PT": "ES", "pt_pt": "ES", "AR-EG": "AR",
                 "he": "HE", "zh-Hans": "OTHER", "ja": "OTHER", "de": "EN", "en-XA": "EN",
                 "ar-XB": "AR"}
        for tag, rule in cases.items():
            self.assertEqual(i18n.plural_rule(tag), rule, tag)

    def test_header_says_the_number_of_forms(self):
        for key, (_, cats, _) in i18n.PLURAL_RULES.items():
            locale = {"EN": "en", "OTHER": "zh", "ES": "es", "FR": "fr", "AR": "ar", "HE": "he"}[key]
            self.assertIn("nplurals=%d;" % len(cats), i18n.po_header(locale))


class PoFiles(Base):
    def setUp(self):
        super().setUp()
        self.t.write("components/u/x.c", SRC)
        self.t.run("extract")

    def po(self, locale="es-US"):
        return (self.t.root / i18n.po_path(locale)).read_text(encoding="utf-8")

    def fill(self, old, new, locale="es-US"):
        text = self.po(locale)
        self.assertIn(old, text)
        self.t.write(i18n.po_path(locale), text.replace(old, new, 1))

    def new(self, locale="es-US"):
        code, out = self.t.run("merge", "--new", locale)
        self.assertEqual(code, 0, out)

    def translate_arm(self):
        self.fill('msgid "Arm Away"\nmsgstr ""', 'msgid "Arm Away"\nmsgstr "Armar fuera"')

    def test_new_language_from_the_template(self):
        self.new()
        text = self.po()
        self.assertIn('"Language: es-US\\n"', text)
        self.assertIn('"Plural-Forms: nplurals=3; plural=', text)
        self.assertIn('#: components/u/x.c\nmsgctxt "door"\nmsgid "Open"\nmsgstr ""', text)
        self.assertIn('msgid_plural "{0} zones open"\nmsgstr[0] ""\nmsgstr[1] ""\nmsgstr[2] ""', text)
        self.assertIn("#. Count on Home. At most 20 bytes.", text)

    def test_translator_comments_flags_and_previous_lines_survive_merge(self):
        self.new()
        text = self.po()
        text = text.replace(
            '#: components/u/x.c\nmsgid "Arm Away"',
            '# Ask Pat about this one\n#: components/u/x.c\n#, c-format, no-wrap\n'
            '#| msgid "Arm"\nmsgid "Arm Away"')
        text += '\n#~| msgid "Old"\n#~ msgid "Gone"\n#~ msgstr "Ido"\n'
        self.t.write(i18n.po_path("es-US"), text)
        self.assertEqual(self.t.run("merge")[0], 0)
        out = self.po()
        self.assertIn("# Ask Pat about this one\n", out)
        self.assertIn("#, c-format, no-wrap\n", out)
        self.assertIn('#| msgid "Arm"\nmsgid "Arm Away"', out)
        self.assertIn('#~| msgid "Old"\n#~ msgid "Gone"', out)
        self.assertEqual(self.t.run("merge")[0], 0)
        self.assertEqual(self.po(), out)

    def test_empty_context_is_no_context(self):
        self.new()
        text = self.po().replace('msgid "Arm Away"', 'msgctxt ""\nmsgid "Arm Away"')
        entries = i18n.parse_po(text, "x.po", [])[1]
        arm = [e for e in entries if e.msgid == "Arm Away"][0]
        self.assertIsNone(arm.ctx)
        self.assertNotIn("msgctxt \"\"", i18n.render_po_entry(arm))
        self.t.write(i18n.po_path("es-US"), text)
        self.assertEqual(self.t.run("merge")[0], 0)
        self.assertNotIn('msgctxt ""', self.po())

    def test_new_refuses_bad_names_and_existing_files(self):
        self.assertEqual(self.t.run("merge", "--new", "en-XA")[0], 1)
        self.assertEqual(self.t.run("merge", "--new", "not a tag")[0], 1)
        self.new()
        self.assertEqual(self.t.run("merge", "--new", "es-US")[0], 1)
        self.assertEqual(self.t.run("merge", "fr-CA")[0], 1)

    def test_merge_is_idempotent_and_keeps_translations(self):
        self.new()
        self.translate_arm()
        first = self.po()
        self.assertEqual(self.t.run("merge")[0], 0)
        self.assertEqual(self.po(), first)
        self.assertIn('msgstr "Armar fuera"', first)

    def test_gone_entries_become_obsolete_and_come_back(self):
        self.new()
        self.translate_arm()
        self.t.write("components/u/x.c", SRC.replace('a(TR("Arm Away")); ', ""))
        self.t.run("extract")
        self.t.run("merge")
        gone = self.po()
        self.assertIn('#~ msgid "Arm Away"\n#~ msgstr "Armar fuera"', gone)
        self.assertTrue(gone.rstrip().endswith('#~ msgstr "Armar fuera"'))
        self.t.run("merge")
        self.assertEqual(self.po(), gone)
        self.t.write("components/u/x.c", SRC)
        self.t.run("extract")
        self.t.run("merge")
        self.assertIn('msgid "Arm Away"\nmsgstr "Armar fuera"', self.po())
        self.assertNotIn("#~", self.po())

    def test_changed_source_carries_over_as_fuzzy(self):
        self.new()
        self.translate_arm()
        self.t.write("components/u/x.c", SRC.replace("Arm Away", "Arm Away now"))
        self.t.run("extract")
        self.t.run("merge")
        text = self.po()
        self.assertIn('# review: changed source\n#, fuzzy\nmsgid "Arm Away now"\n'
                      'msgstr "Armar fuera"', text)
        self.assertNotIn("#~", text)
        self.t.run("merge")
        self.assertEqual(self.po(), text)

    def test_unrelated_new_text_is_not_carried(self):
        self.new()
        self.translate_arm()
        self.t.write("components/u/x.c", SRC.replace("Arm Away", "Disarm"))
        self.t.run("extract")
        self.t.run("merge")
        self.assertIn('msgid "Disarm"\nmsgstr ""', self.po())
        self.assertIn('#~ msgid "Arm Away"', self.po())

    def test_round_trip_of_quotes_escapes_and_utf8(self):
        self.t.write("components/u/y.c", r'z(TR("Say \"hi\"\n\t{{x}} \\ é"));')
        self.t.run("extract")
        self.new()
        text = self.po()
        errors = []
        _, entries = i18n.parse_po(text, "x.po", errors)
        self.assertEqual(errors, [])
        self.assertIn('Say "hi"\n\t{{x}} \\ é', [e.msgid for e in entries])
        self.assertEqual(i18n.render_po("es-US", entries), text)

    def test_multiline_strings_and_gettext_style_files_parse(self):
        text = ('msgid ""\nmsgstr ""\n"Language: es\\n"\n"Plural-Forms: nplurals=3; plural=0;\\n"\n\n'
                '#, fuzzy\nmsgid "A "\n"b"\nmsgstr "x "\n"y"\n')
        errors = []
        header, entries = i18n.parse_po(text, "x.po", errors)
        self.assertEqual(errors, [])
        self.assertEqual(header["Language"], "es")
        self.assertEqual((entries[0].msgid, entries[0].msgstr, entries[0].fuzzy), ("A b", ["x y"], True))


class Build(Base):
    def setUp(self):
        super().setUp()
        self.t.write("components/u/x.c", SRC)
        self.t.run("extract")
        self.t.run("merge", "--new", "es-US")
        text = (self.t.root / i18n.po_path("es-US")).read_text(encoding="utf-8")
        text = text.replace('msgid "Arm Away"\nmsgstr ""', 'msgid "Arm Away"\nmsgstr "Armar fuera"')
        text = text.replace('msgid "Café {0}"\nmsgstr ""', 'msgid "Café {0}"\nmsgstr "¿Café? {0} \\"ñ\\""')
        text = text.replace('msgid_plural "{0} zones open"\nmsgstr[0] ""\nmsgstr[1] ""\nmsgstr[2] ""',
                            'msgid_plural "{0} zones open"\nmsgstr[0] "{0} zona abierta"\n'
                            'msgstr[1] "{0} de zonas abiertas"\nmsgstr[2] "{0} zonas abiertas"')
        self.t.write(i18n.po_path("es-US"), text)

    def read(self, rel):
        return (self.t.root / rel).read_bytes()

    def test_generated_tables_are_sorted_and_point_at_the_texts(self):
        self.assertEqual(self.t.run("build")[0], 0)
        entries, block, preview, locale, rule = read_generated_c(
            self.read(i18n.catalog_c_path("es-US")).decode("utf-8"))
        self.assertEqual((locale, rule, preview), ("es-US", "I18N_PLURALS_ES", 1))
        self.assertEqual([h for h, _ in entries], sorted(h for h, _ in entries))
        self.assertEqual(len(set(h for h, _ in entries)), 3)
        texts = {}
        for h, off in entries:
            texts[h] = block[off:block.index(b"\0", off)].decode("utf-8")
        self.assertEqual(texts[i18n.key_hash(None, "Arm Away")], "Armar fuera")
        self.assertEqual(texts[i18n.key_hash(None, "Café {0}")], '¿Café? {0} "ñ"')
        # the plural's forms sit back to back, in the rule's order: one, many, other
        off = dict(entries)[i18n.key_hash(None, "{0} zone open")]
        forms = block[off:].split(b"\0")[:3]
        self.assertEqual([f.decode() for f in forms],
                         ["{0} zona abierta", "{0} de zonas abiertas", "{0} zonas abiertas"])
        self.assertNotIn(i18n.key_hash("door", "Open"), texts)  # untranslated: left out

    def test_web_writes_what_a_panel_sends(self):
        # I18N-031: for the mock, the Panel Manager's entries as a panel's i18n_web_json sends them,
        # outside the tree the generated C lives in
        self.t.write("components/services/panel_web/web/x.js", "t('Arm Away');")
        self.t.run("extract")
        self.t.run("merge", "es-US")
        text = (self.t.root / i18n.po_path("es-US")).read_text(encoding="utf-8")
        self.t.write(i18n.po_path("es-US"),
                     text.replace('msgid "Arm Away"\nmsgstr ""', 'msgid "Arm Away"\nmsgstr "Armar fuera"'))
        self.assertEqual(self.t.run("web")[0], 0)
        doc = json.loads(self.read("build/i18n/web/es-US.json").decode("utf-8"))
        self.assertEqual(doc["locale"], "es-US")
        self.assertEqual(doc["entries"], {"%08x" % i18n.key_hash(None, "Arm Away"): "Armar fuera"})
        self.assertEqual(list((self.t.root / i18n.CATALOG_DIR).glob("*.json")), [])

    def test_only_drafts_are_left_out_of_a_release(self):
        # I18N-005: a reviewed, complete catalog is in every build; a draft only without
        # I18N_NO_DRAFTS (development builds), and the table still ends in NULL
        text = i18n.render_catalogs_list([i18n.Catalog("es-US", 1, []), i18n.Catalog("fr-CA", 0, [])])
        self.assertIn("#ifndef I18N_NO_DRAFTS\n    &i18n_catalog_es_US, /* draft */\n#endif\n", text)
        self.assertIn("#endif\n    &i18n_catalog_fr_CA,\n    NULL,\n};", text)
        self.assertEqual(text.count("#ifndef"), 1)

    def test_hash_vectors_reach_the_table(self):
        cat = i18n.Catalog("es-US", 1, [(None, "a", ["x"], False), (None, "foobar", ["y"], False),
                                        (None, "", ["z"], False)])
        self.assertEqual([r[0] for r in i18n.catalog_layout(cat)], [0x811C9DC5, 0xBF9CF968, 0xE40C292C])

    def test_list_and_characters_and_no_second_copy_for_the_web(self):
        self.t.run("build")
        text = self.read(i18n.LIST_PATH).decode("utf-8")
        # I18N-005: this es-US is a draft, so a release build (I18N_NO_DRAFTS) leaves it out
        self.assertIn("#ifndef I18N_NO_DRAFTS\n    &i18n_catalog_es_US, /* draft */\n#endif\n    NULL,",
                      text)
        self.assertIn("i18n_builtin_count = sizeof i18n_builtin / sizeof i18n_builtin[0] - 1u;", text)
        chars = self.read(i18n.CHARS_PATH).decode("utf-8")
        self.assertEqual(chars, "".join(sorted(set(chars.strip("\n")))) + "\n")
        self.assertIn("¿", chars)
        # I18N-031: the panel sends the Panel Manager its entries from these tables; no JSON copy
        self.assertEqual(list(self.t.root.rglob("*.json")), [])

    def test_the_page_entries_are_flagged_and_plurals_marked(self):
        # I18N-031: an entry used in the Panel Manager's folder is flagged for the web, whatever
        # else uses it; a plural entry is flagged as one
        self.t.write("components/services/panel_web/web/x.js", "t('Arm Away'); tc('door', 'Open');")
        self.t.run("extract")
        self.t.run("merge")
        text = self.read(i18n.po_path("es-US")).decode("utf-8")
        self.t.write(i18n.po_path("es-US"), text.replace(
            'msgctxt "door"\nmsgid "Open"\nmsgstr ""', 'msgctxt "door"\nmsgid "Open"\nmsgstr "Abierta"'))
        self.t.run("build")
        flags = {}
        read_generated_c(self.read(i18n.catalog_c_path("es-US")).decode("utf-8"), flags)
        self.assertEqual(flags[i18n.key_hash(None, "Arm Away")], ["I18N_ENTRY_WEB"])
        self.assertEqual(flags[i18n.key_hash("door", "Open")], ["I18N_ENTRY_WEB"])
        self.assertEqual(flags[i18n.key_hash(None, "Café {0}")], [])
        self.assertEqual(flags[i18n.key_hash(None, "{0} zone open")], ["I18N_ENTRY_PLURAL"])

    def test_web_json_is_what_the_panel_sends(self):
        # the mock's file for a test language: hashed keys, only the page's entries, forms as lists
        cat = i18n.Catalog("es-US", 1, [(None, "Arm Away", ["Armar fuera"], False),
                                        ("door", "Open", ["Abierta"], False),
                                        (None, "{0} zone", ["a", "b", "c"], True)],
                           web={(None, "Arm Away"), (None, "{0} zone")})
        doc = json.loads(i18n.render_web_json(cat))
        self.assertEqual((doc["locale"], doc["plural"]), ("es-US", ["one", "many", "other"]))
        self.assertEqual(doc["entries"], {"%08x" % i18n.key_hash(None, "Arm Away"): "Armar fuera",
                                          "%08x" % i18n.key_hash(None, "{0} zone"): ["a", "b", "c"]})
        self.assertEqual(list(doc["entries"]), sorted(doc["entries"], key=lambda k: int(k, 16)))

    def test_build_is_deterministic_and_check_writes_nothing(self):
        self.assertEqual(self.t.run("build", "--check")[0], 1)
        self.assertFalse((self.t.root / i18n.CATALOG_DIR).exists())
        self.t.run("build")
        paths = (i18n.LIST_PATH, i18n.catalog_c_path("es-US"), i18n.CHARS_PATH)
        first = {p: self.read(p) for p in paths}
        code, out = self.t.run("build", "--check")
        self.assertEqual(code, 0, out)
        self.t.run("build")
        self.assertEqual({p: self.read(p) for p in paths}, first)

    def test_check_lists_what_would_change(self):
        self.t.run("build")
        p = self.t.root / i18n.catalog_c_path("es-US")
        p.write_bytes(p.read_bytes() + b"/* edited */\n")
        code, out = self.t.run("build", "--check")
        self.assertEqual(code, 1)
        self.assertIn("i18n_es_US_gen.c: would change", out)
        self.assertTrue(p.read_bytes().endswith(b"/* edited */\n"))

    def test_removed_language_removes_its_files(self):
        self.t.run("build")
        (self.t.root / i18n.po_path("es-US")).unlink()
        code, out = self.t.run("build", "--check")
        self.assertEqual(code, 1)
        self.assertIn("no .po file asks for it", out)
        self.t.run("build")
        self.assertFalse((self.t.root / i18n.catalog_c_path("es-US")).exists())
        self.assertFalse((self.t.root / i18n.CHARS_PATH).exists())
        self.assertEqual(self.read(i18n.LIST_PATH), i18n.render_catalogs_list([]).encode())

    def test_the_repository_catalogs_reproduce_the_committed_list(self):
        problems = []
        cats = i18n.load_catalogs(ROOT, problems)
        self.assertEqual(problems, [])
        self.assertEqual(i18n.render_catalogs_list(cats).encode(), (ROOT / i18n.LIST_PATH).read_bytes())

    def test_preview_clears_only_when_complete_and_reviewed(self):
        pot = i18n.template_entries(self.t.root)
        keys = [e.key for e in pot]
        entries = []
        for t in pot:
            e = i18n._new_entry(t, 3)
            e.msgstr = ["x"] * len(e.msgstr)
            e.review = "sam 2026-09-01"
            entries.append(e)
        self.assertEqual(i18n.make_catalog("es-US", entries, keys).preview, 0)
        entries[0].fuzzy = True
        self.assertEqual(i18n.make_catalog("es-US", entries, keys).preview, 1)
        entries[0].fuzzy = False
        entries[0].review = "draft"
        self.assertEqual(i18n.make_catalog("es-US", entries, keys).preview, 1)
        entries[0].review = "sam 2026-09-01"
        entries[1].msgstr = [""] * len(entries[1].msgstr)
        self.assertEqual(i18n.make_catalog("es-US", entries, keys).preview, 1)
        self.assertEqual(i18n.make_catalog("es-US", entries[2:], keys).preview, 1)

    def test_partial_plural_is_left_out(self):
        e = i18n.PoEntry()
        e.msgid, e.plural, e.msgstr = "{0} day", "{0} days", ["{0} dia", "", "{0} dias"]
        self.assertEqual(i18n.make_catalog("es-US", [e], []).items, [])

    def _table_texts(self, locale="es-US"):
        entries, block, _, _, _ = read_generated_c(
            self.read(i18n.catalog_c_path(locale)).decode("utf-8"))
        return {h: block[off:].split(b"\0")[0].decode("utf-8") for h, off in entries}

    def test_malformed_entries_are_left_out_of_the_build(self):
        # a plural with two forms, and a key the template lacks
        text = self.read(i18n.po_path("es-US")).decode("utf-8")
        text = text.replace('msgstr[2] "{0} zonas abiertas"', "")
        text += '\nmsgid "Not there"\nmsgstr "Nada"\n'
        self.t.write(i18n.po_path("es-US"), text)
        code, out = self.t.run("build")
        self.assertEqual(code, 0, out)
        self.assertIn("es-US.po:", out)
        self.assertEqual(out.count("warning"), 2, out)
        texts = self._table_texts()
        self.assertEqual(sorted(texts.values()), ["Armar fuera", "¿Café? {0} \"ñ\""])

    def test_plain_entry_for_a_plural_key_is_left_out(self):
        text = self.read(i18n.po_path("es-US")).decode("utf-8")
        text = text.replace('msgid_plural "{0} zones open"\nmsgstr[0] "{0} zona abierta"\n'
                            'msgstr[1] "{0} de zonas abiertas"\nmsgstr[2] "{0} zonas abiertas"',
                            'msgstr "solo"')
        self.t.write(i18n.po_path("es-US"), text)
        code, out = self.t.run("build")
        self.assertEqual(code, 0, out)
        self.assertNotIn(i18n.key_hash(None, "{0} zone open"), self._table_texts())

    def test_no_template_is_never_complete(self):
        e = i18n.PoEntry()
        e.msgid, e.msgstr, e.review = "a", ["b"], "sam 2026-09-01"
        self.assertEqual(i18n.make_catalog("es-US", [e], []).preview, 1)
        (self.t.root / i18n.POT_PATH).write_bytes(b"")
        code, out = self.t.run("build")
        self.assertEqual(code, 1)
        self.assertIn("messages.pot", out)
        (self.t.root / i18n.POT_PATH).unlink()
        self.assertEqual(self.t.run("build")[0], 1)

    def test_bad_locale_names_stop_the_build(self):
        good = self.read(i18n.po_path("es-US"))
        for name in ("es_US", "es-USA", "e-US", "es-US-x"):
            self.t.write(i18n.po_path(name), good.decode("utf-8"))
            code, out = self.t.run("build")
            self.assertEqual(code, 1, name)
            self.assertIn("%s.po" % name, out)
            (self.t.root / i18n.po_path(name)).unlink()
        for name in ("es-us", "ES-US", "es_US"):
            self.assertFalse(i18n.LOCALE_RE.match(name), name)
        for name in ("zh-Hans", "es-419", "fr", "pt-BR", "zh-Hant"):
            self.assertTrue(i18n.LOCALE_RE.match(name), name)

    def test_empty_catalog_shape(self):
        text = i18n.render_catalog_c(i18n.Catalog("fr-CA", 1, []), "x.po")
        self.assertIn("{0u, 0u}", text)
        self.assertIn("I18N_PLURALS_FR, 1, 0u,", text)

    def test_collision_stops_the_build(self):
        cat = i18n.Catalog("es-US", 1, [(None, "a", ["x"], False), (None, "a", ["y"], False)])
        with self.assertRaises(ValueError):
            i18n.catalog_layout(cat)

    def test_c_literals_never_swallow_digits(self):
        self.assertEqual(i18n.c_string("é1\"?\\".encode() + b"\0" + b"2"),
                         '"\\303\\2511\\"\\?\\\\\\0002"')


class TranslationChecks(Base):
    def setUp(self):
        super().setUp()
        self.t.write("components/u/x.c",
                     '/* TR: At most 12 bytes. */ a(TR("Hello {0} and {1}")); '
                     'b(TRN("{0} door", "{0} doors", n)); c(TR("Plain"));')
        self.t.run("extract")
        self.t.run("merge", "--new", "es-US")
        self.text = (self.t.root / i18n.po_path("es-US")).read_text(encoding="utf-8")

    def check(self, old, new, *extra):
        text = self.text.replace(old, new, 1) if old else self.text
        if old:
            self.assertNotEqual(text, self.text, old)
        self.t.write(i18n.po_path("es-US"), text)
        return self.t.run("check", *extra)

    HELLO = 'msgid "Hello {0} and {1}"\nmsgstr ""'
    PLAIN = 'msgid "Plain"\nmsgstr ""'
    DOORS = 'msgid_plural "{0} doors"\nmsgstr[0] ""\nmsgstr[1] ""\nmsgstr[2] ""'

    def test_untranslated_file_is_fine(self):
        code, out = self.check(None, None)
        self.assertEqual(code, 0, out)

    def test_placeholders_must_match(self):
        code, out = self.check(self.HELLO, 'msgid "Hello {0} and {1}"\nmsgstr "Hola {0}"')
        self.assertEqual(code, 1)
        self.assertIn("uses {0} but the source has {0}, {1}", out)
        code, out = self.check(self.HELLO, 'msgid "Hello {0} and {1}"\nmsgstr "{1} y {0}"')
        self.assertEqual(code, 0, out)
        code, out = self.check(self.PLAIN, 'msgid "Plain"\nmsgstr "Llano {0}"')
        self.assertEqual(code, 1)
        code, out = self.check(self.PLAIN, 'msgid "Plain"\nmsgstr "Llano {x}"')
        self.assertIn("only {0}..{9}", out)

    def test_plural_forms(self):
        code, out = self.check(self.DOORS, 'msgid_plural "{0} doors"\nmsgstr[0] "una puerta"\n'
                               'msgstr[1] "{0} de puertas"\nmsgstr[2] "{0} puertas"')
        self.assertEqual(code, 0, out)  # a form other than "other" may leave the number out
        code, out = self.check(self.DOORS, 'msgid_plural "{0} doors"\nmsgstr[0] "{0} puerta"\n'
                               'msgstr[1] "{0} de puertas"\nmsgstr[2] "puertas"')
        self.assertEqual(code, 1)
        self.assertIn("uses no placeholder but the source has {0}", out)
        code, out = self.check(self.DOORS, 'msgid_plural "{0} doors"\nmsgstr[0] "{1} puerta"\n'
                               'msgstr[1] ""\nmsgstr[2] "{0} puertas"')
        self.assertEqual(code, 1)

    def test_wrong_number_of_forms(self):
        code, out = self.check(self.DOORS, 'msgid_plural "{0} doors"\nmsgstr[0] "{0} puerta"\n'
                               'msgstr[1] "{0} puertas"')
        self.assertEqual(code, 1)
        self.assertIn("has 2 plural forms, es-US needs 3", out)

    def test_arabic_has_six_forms(self):
        self.t.run("merge", "--new", "ar-EG")
        text = (self.t.root / i18n.po_path("ar-EG")).read_text(encoding="utf-8")
        self.assertEqual(text.count("msgstr["), 6)
        self.assertIn("nplurals=6;", text)

    def test_printf_and_markup_refused(self):
        code, out = self.check(self.PLAIN, 'msgid "Plain"\nmsgstr "%s llano"')
        self.assertEqual(code, 1)
        self.assertIn('printf conversion "%s"', out)
        code, out = self.check(self.PLAIN, 'msgid "Plain"\nmsgstr "<b>Llano</b>"')
        self.assertEqual(code, 1)
        self.assertIn("markup", out)
        code, out = self.check(self.PLAIN, 'msgid "Plain"\nmsgstr "100 % llano"')
        self.assertEqual(code, 0, out)

    def test_byte_budget_counts_bytes_without_placeholders(self):
        code, out = self.check(self.HELLO, 'msgid "Hello {0} and {1}"\nmsgstr "{0}{1} ñññññññ"')
        self.assertEqual(code, 1)  # 1 + 7 * 2 = 15 bytes > 12
        self.assertIn("is 15 bytes, the note allows 12", out)
        code, out = self.check(self.HELLO, 'msgid "Hello {0} and {1}"\nmsgstr "{0}{1} ñññññ"')
        self.assertEqual(code, 0, out)  # 1 + 10 = 11

    def test_duplicate_entries(self):
        self.t.write(i18n.po_path("es-US"), self.text + '\nmsgid "Plain"\nmsgstr ""\n')
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn('"Plain" is also at line', out)

    def test_entry_missing_from_the_template_and_kind_change(self):
        self.t.write(i18n.po_path("es-US"), self.text + '\nmsgid "Ghost"\nmsgstr "Fantasma"\n')
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn('"Ghost" is not in the template; run merge', out)
        code, out = self.check(self.PLAIN, 'msgid "Plain"\nmsgid_plural "Plains"\nmsgstr[0] "a"\nmsgstr[1] "b"')
        self.assertEqual(code, 1)
        self.assertIn("plural here but plain in the template", out)

    def test_header_must_match_the_language(self):
        self.t.write(i18n.po_path("es-US"), self.text.replace("nplurals=3", "nplurals=2"))
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn("nplurals=3 for es-US", out)

    def full_po(self, review="sam 2026-09-01", fuzzy=False):
        entries = []
        for t in i18n.template_entries(self.t.root):
            e = i18n._new_entry(t, 3)
            e.msgstr = ["{0} {1}" if "{1}" in t.msgid else "{0} x" if "{0}" in (t.plural or t.msgid)
                        else "Llano"] * len(e.msgstr)
            e.review, e.fuzzy = review, fuzzy
            entries.append(e)
        return entries

    def release(self, entries):
        self.t.write(i18n.po_path("es-US"), i18n.render_po("es-US", entries))
        return self.t.run("check", "--release", "es-US")

    def test_release_accepts_a_complete_reviewed_language(self):
        code, out = self.release(self.full_po())
        self.assertEqual((code, out.strip().split("\n")[-1]), (0, "i18n check: ok (3 entries)"), out)

    def test_release_refuses_missing_draft_fuzzy_and_unreviewed(self):
        entries = self.full_po()
        entries[2].msgstr = [""]
        code, out = self.release(entries)
        self.assertEqual(code, 1)
        self.assertIn('"Plain" is not translated', out)
        self.assertIn("i18n check: 1 error(s) (release es-US)", out)
        entries = self.full_po(review="draft")
        code, out = self.release(entries)
        self.assertEqual(code, 1)
        self.assertEqual(out.count("has no review"), 3)
        entries = self.full_po(fuzzy=True)
        code, out = self.release(entries)
        self.assertEqual(code, 1)
        self.assertEqual(out.count("is still fuzzy"), 3)
        entries = self.full_po(review=None)
        self.assertEqual(self.release(entries)[0], 1)
        entries = self.full_po(review="changed source")
        self.assertEqual(self.release(entries)[0], 1)

    def test_release_of_a_missing_language(self):
        code, out = self.t.run("check", "--release", "de-DE")
        self.assertEqual(code, 1)
        self.assertIn("no such language file", out)


class EnglishVariant(Base):
    """A variant of the source language (en-GB) holds only what it writes differently: an entry it
    leaves empty is the English itself (docs/features/37 I18N-038, British English)."""

    def setUp(self):
        super().setUp()
        self.t.write("components/u/x.c", 'a(TR("Alarm canceled")); b(TR("Arm Away"));')
        self.t.run("extract")
        self.t.run("merge", "--new", "en-GB")

    def put(self, english, text, review="draft"):
        rel = i18n.po_path("en-GB")
        po = i18n.read_po(self.t.root, rel)[1]
        for e in po:
            if e.msgid == english:
                e.msgstr, e.fuzzy, e.review = [text], review == "draft", review
        self.t.write(rel, i18n.render_po("en-GB", po))
        return po

    def test_an_american_spelling_needs_its_entry(self):
        code, out = self.t.run("check")
        self.assertEqual(code, 1)
        self.assertIn('"Alarm canceled" writes "canceled", which en-GB spells "cancelled"', out)
        self.put("Alarm canceled", "Alarm cancelled")
        code, out = self.t.run("check")
        self.assertEqual(code, 0, out)

    def test_it_ships_when_its_own_entries_are_reviewed(self):
        keys = [t.key for t in i18n.template_entries(self.t.root)]
        po = self.put("Alarm canceled", "Alarm cancelled")
        self.assertEqual(i18n.make_catalog("en-GB", po, keys).preview, 1)  # still a draft
        po = self.put("Alarm canceled", "Alarm cancelled", review="sam 2026-09-30")
        cat = i18n.make_catalog("en-GB", po, keys)
        self.assertEqual(cat.preview, 0)  # "Arm Away" is the source's own text
        self.assertEqual([item[1] for item in cat.items], ["Alarm canceled"])
        code, out = self.t.run("check", "--release", "en-GB")
        self.assertEqual(code, 0, out)
        # a language other than English still needs every entry
        self.assertEqual(i18n.make_catalog("es-US", po, keys).preview, 1)


class Pseudo(Base):
    def setUp(self):
        super().setUp()
        self.t.write("components/u/x.c",
                     'a(TR("Arm Away {0}")); b(TRN("{0} door", "{0} doors", n)); c(TR("{{x}} Hi"));'
                     ' d(TR_C("date pattern", "h:mm a"));'
                     ' /* TR: a title. At most 24 bytes */ e(TR("Choose a zone name"));')
        self.t.run("extract")
        self.assertEqual(self.t.run("pseudo", "--build")[0], 0)

    def out(self, name):
        return (self.t.root / "build/i18n/pseudo" / name).read_text(encoding="utf-8")

    def entries(self, locale):
        return {e.msgid: e for e in i18n.parse_po(self.out(locale + ".po"), "p", [])[1]}

    def test_en_xa(self):
        by = self.entries("en-XA")
        arm = by["Arm Away {0}"].msgstr[0]
        self.assertTrue(arm.startswith("[Åŕm Åŵáý {0}"), arm)
        self.assertTrue(arm.endswith("]"))
        self.assertEqual(i18n.placeholders(arm)[0], {"0"})
        self.assertGreaterEqual(len(arm), int(len("Arm Away {0}") * 1.3))
        self.assertIn("{{x}}", by["{{x}} Hi"].msgstr[0])
        self.assertEqual(len(by["{0} door"].msgstr), 2)

    def test_ar_xb_wraps_runs_and_leaves_placeholders(self):
        by = self.entries("ar-XB")
        self.assertEqual(by["Arm Away {0}"].msgstr[0], "\u202eArm Away \u202c{0}")
        # "{{" and "}}" are escaped braces: placeholders, never altered; the x between is text
        self.assertEqual(by["{{x}} Hi"].msgstr[0], "{{\u202ex\u202c}}\u202e Hi\u202c")
        self.assertEqual(len(by["{0} door"].msgstr), 6)
        self.assertEqual(by["{0} door"].msgstr[1], "{0}\u202e door\u202c")  # "one" is the singular
        self.assertEqual(by["{0} door"].msgstr[5], "{0}\u202e doors\u202c")

    def test_pseudo_keeps_the_budget(self):
        # I18N-023: a test language models a translation that passes the check, so a cut the
        # preview finds is a budget that was never written down, not the pseudo-language's
        for locale in i18n.PSEUDO_LOCALES:
            s = self.entries(locale)["Choose a zone name"].msgstr[0]
            self.assertLessEqual(len(s.encode("utf-8")), 24, (locale, s))
        self.assertTrue(self.entries("en-XA")["Choose a zone name"].msgstr[0].startswith("["))

    def test_date_patterns_stay_english(self):
        # CLDR pattern letters, not words: accented or wrapped they would print as themselves
        for locale in i18n.PSEUDO_LOCALES:
            self.assertEqual(self.entries(locale)["h:mm a"].msgstr, ["h:mm a"], locale)

    def test_pseudo_passes_the_translation_checks(self):
        template = {e.key: e for e in i18n.template_entries(self.t.root)}
        saved = i18n.PO_DIR
        i18n.PO_DIR = "build/i18n/pseudo"
        try:
            for locale in i18n.PSEUDO_LOCALES:
                self.assertEqual(i18n.check_po_file(self.t.root, locale, template), [], locale)
        finally:
            i18n.PO_DIR = saved

    def test_c_tables(self):
        self.assertIn("i18n_catalog_en_XA", self.out("i18n_catalogs_gen.c"))
        self.assertIn("i18n_catalog_ar_XB", self.out("i18n_catalogs_gen.c"))
        entries, _, preview, locale, rule = read_generated_c(self.out("i18n_en_XA_gen.c"))
        self.assertEqual((locale, rule, preview, len(entries)), ("en-XA", "I18N_PLURALS_EN", 1, 5))
        self.assertEqual(read_generated_c(self.out("i18n_ar_XB_gen.c"))[4], "I18N_PLURALS_AR")

    def test_pseudo_never_touches_components(self):
        self.assertEqual(i18n.po_locales(self.t.root), [])
        self.assertFalse((self.t.root / i18n.CATALOG_DIR).exists())

    def test_pseudo_files_in_the_po_folder_are_not_languages(self):
        self.t.write(i18n.po_path("en-XA"), 'msgid ""\nmsgstr ""\n')
        self.assertEqual(i18n.po_locales(self.t.root), [])


def have_espsecure():
    try:
        import espsecure  # noqa: F401
        return True
    except ImportError:
        return False


class Pack(Base):
    """I18N-039, I18N-040: tools/i18n.py pack, the template fingerprint file and the frozen
    fixture pack."""

    SRC = ('a(TR("Arm Away")); b(TR_C("this language", "English")); '
           'c(TRN("{0} zone open", "{0} zones open", n));')

    def setUp(self):
        super().setUp()
        self.t.write("components/u/x.c", self.SRC)
        self.t.run("extract")
        self.t.run("merge", "--new", "es-US")

    def translate(self, locale="es-US", name="Español"):
        path = self.t.root / i18n.po_path(locale)
        text = path.read_text(encoding="utf-8")
        text = text.replace('msgctxt "this language"\nmsgid "English"\nmsgstr ""',
                            'msgctxt "this language"\nmsgid "English"\nmsgstr "%s"' % name)
        text = text.replace('msgid "Arm Away"\nmsgstr ""', 'msgid "Arm Away"\nmsgstr "Armar fuera"')
        path.write_text(text, encoding="utf-8")

    def pack(self, *extra):
        out = self.t.root / "out"
        return self.t.run("pack", "--version", "0.7.0", "--out", str(out), *extra), out

    def test_payload_layout(self):
        # I18N-040: the header, the entries and the strings exactly as the spec says
        self.translate()
        (code, text), out = self.pack()
        self.assertEqual(code, 0, text)
        self.assertIn("unsigned", text)
        data = (out / "es-US.robolang").read_bytes()
        self.assertEqual(len(data) % 4096, 0)
        self.assertEqual(data[-4096:], b"\xff" * 4096)
        n = struct.unpack_from("<I", data, 8)[0]
        self.assertEqual(data[n:-4096], b"\xff" * (len(data) - 4096 - n))
        self.assertEqual(data[:4], b"RLNG")
        self.assertEqual(struct.unpack_from("<HH", data, 4), (1, 128))
        self.assertEqual(data[12:28].rstrip(b"\0"), b"es-US")
        self.assertEqual(data[28:60].rstrip(b"\0").decode("utf-8"), "Español")
        self.assertEqual(data[60], list(i18n.PLURAL_RULES).index("ES"))
        self.assertEqual(data[61], 1)  # a draft: not everything translated
        self.assertEqual(data[62:64], b"\0\0")
        count, eoff, soff, ssize = struct.unpack_from("<IIII", data, 64)
        self.assertEqual((count, eoff, soff), (2, 128, 128 + 16))
        self.assertEqual(n, soff + ssize)
        hashes = [struct.unpack_from("<I", data, eoff + 8 * i)[0] for i in range(count)]
        self.assertEqual(hashes, sorted(hashes))
        self.assertEqual(data[88:104].rstrip(b"\0"), b"0.7.0")
        self.assertEqual(struct.unpack_from("<II", data, 104), (0, 0))
        self.assertEqual(data[112:128], b"\0" * 16)
        keys = i18n.template_hashes(self.t.root)
        self.assertEqual(struct.unpack_from("<II", data, 80), (i18n.template_fingerprint(keys), 1))

    def test_fingerprint_and_generated_file(self):
        # I18N-039: the fingerprint is FNV-1a over the sorted hashes, little-endian
        self.assertEqual(i18n.template_fingerprint([]), 0x811C9DC5)
        self.assertEqual(i18n.template_fingerprint([0x01020304]), i18n.fnv1a32(b"\x04\x03\x02\x01"))
        self.assertEqual(i18n.template_fingerprint([2, 1]), i18n.template_fingerprint([1, 2]))
        self.t.run("build")
        text = (self.t.root / i18n.TEMPLATE_C_PATH).read_text(encoding="utf-8")
        keys = i18n.template_hashes(self.t.root)
        self.assertIn("i18n_template_fingerprint = 0x%08xu;" % i18n.template_fingerprint(keys), text)
        self.assertIn("i18n_template_count = %du;" % len(keys), text)
        # never stale, even with no .po file
        (self.t.root / i18n.po_path("es-US")).unlink()
        code, out = self.t.run("build", "--check")
        self.assertEqual(code, 1)
        self.assertNotIn("template_gen", out)
        self.t.run("build")
        self.assertTrue((self.t.root / i18n.TEMPLATE_C_PATH).exists())
        self.assertEqual(self.t.run("build", "--check")[0], 0)

    def test_errors_and_release(self):
        self.translate()
        (code, text), _ = self.pack("--locale", "fr-CA")
        self.assertEqual(code, 1)
        self.assertIn("no .po file for fr-CA", text)
        (code, text), out = self.pack("--release")  # es-US is a draft
        self.assertEqual(code, 0)
        self.assertFalse((out / "es-US.robolang").exists())
        code, text = self.t.run("pack", "--version", "badé", "--out", str(out))
        self.assertEqual(code, 1)
        self.assertIn("version", text)
        code, text = self.t.run("pack", "--version", "1" * 16, "--out", str(out))
        self.assertEqual(code, 1)

    def test_a_name_is_required_and_bounded(self):
        (code, text), _ = self.pack()  # the name is not translated
        self.assertEqual(code, 1)
        self.assertIn("no translation of the language's own name", text)
        self.translate(name="x" * 32)
        (code, text), _ = self.pack()
        self.assertEqual(code, 1)
        self.assertIn("32 bytes", text)

    def test_source_variant_has_nothing_missing(self):
        cat = i18n.Catalog("en-GB", 0, [("this language", "English", ["English (UK)"], False)])
        payload = i18n.pack_payload_for(cat, "English (UK)", [1, 2, 3], "1.0.0")
        self.assertEqual(struct.unpack_from("<I", payload, 84)[0], 0)
        self.assertEqual(payload[60], 0)  # English: rule 0

    def test_fonts_slot_in_after_the_strings(self):
        cat = i18n.fixture_catalog()
        rows = i18n.catalog_layout(cat)
        plain = i18n.build_pack_payload("fr-CA", "Fr", "FR", False, rows, [], "1")
        withf = i18n.build_pack_payload("fr-CA", "Fr", "FR", False, rows, [], "1", fonts=b"ABCDE")
        off, size = struct.unpack_from("<II", withf, 104)
        self.assertEqual(off % 4, 0)
        self.assertEqual((size, withf[off:off + size]), (5, b"ABCDE"))
        self.assertEqual(struct.unpack_from("<I", withf, 8)[0], len(withf))
        self.assertEqual(withf[:len(plain)][128:], plain[128:])

    def test_fonts_table_offsets_and_bytes(self):
        """I18N-049: the table's offsets count from the payload start, 4-aligned, no overlap."""
        rows = i18n.catalog_layout(i18n.fixture_catalog())
        fa, fb = b"A" * 7, b"B" * 10
        pl = i18n.build_pack_payload("fr-CA", "Fr", "FR", False, rows, [], "1",
                                     fonts=[("body", fa), ("title", fb)])
        off, size = struct.unpack_from("<II", pl, 104)
        self.assertEqual(off % 4, 0)
        self.assertEqual(struct.unpack_from("<I", pl, off)[0], 2)
        recs = []
        for i in range(2):
            r = pl[off + 4 + 24 * i: off + 28 + 24 * i]
            recs.append((r[:16].rstrip(b"\0").decode(), *struct.unpack_from("<II", r, 16)))
        self.assertEqual([r[0] for r in recs], ["body", "title"])
        (_, o1, s1), (_, o2, s2) = recs
        self.assertEqual((s1, s2), (7, 10))
        self.assertEqual((o1 % 4, o2 % 4), (0, 0))
        self.assertEqual(o1, off + 4 + 48)
        self.assertEqual(o2, o1 + 8)
        self.assertEqual((pl[o1:o1 + 7], pl[o2:o2 + 10]), (fa, fb))
        self.assertEqual(size, o2 + 10 - off)
        self.assertEqual(len(pl), off + size)
        self.assertEqual(struct.unpack_from("<I", pl, 8)[0], len(pl))

    def test_fonts_roles_are_checked(self):
        with self.assertRaises(ValueError):
            i18n.pack_fonts_section([("body", b"x"), ("body", b"y")], 0)
        with self.assertRaises(ValueError):
            i18n.pack_fonts_section([("digits", b"x")], 0)
        with self.assertRaises(ValueError):
            i18n.parse_font_args(["nope"])

    def test_pack_font_option_writes_the_fonts(self):
        self.translate()
        f = self.t.root / "body.bin"
        f.write_bytes(b"FONTDATA")
        (code, text), out = self.pack("--font", "body=%s" % f)
        self.assertEqual(code, 0, text)
        data = (out / "es-US.robolang").read_bytes()
        off, size = struct.unpack_from("<II", data, 104)
        self.assertGreater(size, 0)
        self.assertEqual(data[off + 4:off + 8], b"body")
        o, s = struct.unpack_from("<II", data, off + 4 + 16)
        self.assertEqual(data[o:o + s], b"FONTDATA")
        for bad in (["--font", "body=%s" % f, "--font", "body=%s" % f], ["--font", "digits=%s" % f]):
            (code, text), _ = self.pack(*bad)
            self.assertEqual(code, 1)

    @unittest.skipUnless(have_espsecure(), "espsecure is not importable")
    def test_signed_round_trip(self):
        self.translate()
        key = self.t.root / "key.pem"
        res = subprocess.run([sys.executable, "-m", "espsecure", "generate_signing_key", "--version",
                              "2", "--scheme", "rsa3072", str(key)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        (code, text), out = self.pack("--key", str(key))
        self.assertEqual(code, 0, text)
        self.assertNotIn("unsigned", text)
        signed = out / "es-US.robolang"
        res = subprocess.run([sys.executable, "-m", "espsecure", "verify_signature", "--version", "2",
                              "--keyfile", str(key), str(signed)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        data = signed.read_bytes()
        self.assertEqual(len(data) % 4096, 0)
        unsigned = i18n.unsigned_pack_file(i18n.fixture_payload()[0])
        self.assertEqual(len(unsigned) % 4096, 0)
        self.assertEqual(data[:4], b"RLNG")

    def test_frozen_fixture_is_current(self):
        # I18N-040: the committed fixture pack is what the tool makes from its hard-coded input
        committed = (ROOT / i18n.FIXTURE_PATH).read_bytes().decode("utf-8")
        self.assertEqual(committed, i18n.render_pack_fixture_header())
        payload, keys = i18n.fixture_payload()
        self.assertEqual(payload[:4], b"RLNG")
        self.assertEqual(struct.unpack_from("<I", payload, 8)[0], len(payload))
        self.assertEqual(struct.unpack_from("<I", payload, 84)[0], 2)  # missing


class RealTree(unittest.TestCase):
    def test_build_check_on_the_repository(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = i18n.main(["--root", str(ROOT), "build", "--check"])
        self.assertEqual(code, 0, out.getvalue())

    def test_cmake_compiles_every_generated_catalog(self):
        text = (ROOT / "components/core/i18n/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("catalogs/*_gen.c", text)


class Inventory(Base):
    POT = (
        'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n\n'
        '#: components/ui/a/x.c\nmsgctxt "button"\nmsgid "Arm"\nmsgstr ""\n\n'
        '#. the title. At most 20 bytes\n#: components/ui/a/x.c\nmsgid "Settings"\nmsgstr ""\n\n'
        '#: components/services/panel_web/web/p.js\nmsgid "{0} zone"\nmsgid_plural "{0} zones"\n'
        'msgstr[0] ""\nmsgstr[1] ""\n\n'
        '#: components/core/notify/src/n.c\nmsgid "Alarm"\nmsgstr ""\n\n'
        '#: components/core/i18n/src/d.c\nmsgid "May"\nmsgstr ""\n'
    )

    def test_groups_by_surface_and_file(self):
        self.t.write(i18n.POT_PATH, self.POT)
        self.t.write(i18n.FLASHER_WEB + "/app.js", "h('b', { text: 'Guided setup' });")
        code, out = self.t.run("inventory")
        self.assertEqual(code, 0)
        text = (self.t.root / i18n.INVENTORY_PATH).read_text(encoding="utf-8")
        self.assertIn("Entries: Touchscreen 2, Panel Manager 1, Notifications 1, Other 1, Flasher 1.", text)
        self.assertLess(text.index("## Touchscreen"), text.index("## Panel Manager"))
        self.assertLess(text.index("## Notifications"), text.index("## Other"))
        self.assertIn('- "Arm" | context "button" | components/ui/a/x.c', text)
        self.assertIn('- "Settings" | max 20 bytes | components/ui/a/x.c', text)
        self.assertIn('- "{0} zone / {0} zones" | components/services/panel_web/web/p.js', text)
        self.assertIn("## Flasher", text)
        self.assertIn('- "Guided setup" | line 1', text)
        first = text
        self.t.run("inventory")
        self.assertEqual((self.t.root / i18n.INVENTORY_PATH).read_text(encoding="utf-8"), first)

    def test_no_template_is_an_error(self):
        self.assertEqual(self.t.run("inventory")[0], 1)


if __name__ == "__main__":
    unittest.main()
