#!/usr/bin/env python3
"""docs/features/37: the translation catalog tool (extract, merge, check, build, pseudo, web).

  i18n.py extract [--check]   scan the C sources into components/core/i18n/po/messages.pot
  i18n.py merge [<locale>...] [--new <locale>]
                              bring the .po files up to the template (or create one)
  i18n.py check [--unmarked-all] [--release <locale>]
                              source-side checks: the template is current, no hash collisions,
                              placeholders, plural forms, no printf/markup, byte budgets, and the
                              unmarked-text heuristic for the files listed in po/MARKED;
                              translation side: the same rules for every .po file; --release
                              also wants <locale> complete, not fuzzy and reviewed
  i18n.py build [--check]     .po files -> C tables (the Panel Manager's entries flagged), character list
  i18n.py pack --version <V> --out <dir> [--key <pem>] [--release] [--locale <tag> ...]
                              one signed language pack file (<locale>.robolang) per .po file
                              (docs/features/37 language packs)
  i18n.py pseudo [--build]    en-XA and ar-XB test languages into build/i18n/pseudo/
  i18n.py web [--out <dir>]   the .po files' Panel Manager words as a panel sends them
                              (/i18n/<locale>.json) into build/i18n/web/, for the mock
  i18n.py inventory           every screen's words by surface and file (and the Flasher's, best
                              effort) into build/i18n/inventory.md

Standard library only. One extractor function per source kind (EXTRACTORS), one function per check.
"""
import argparse
import difflib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PO_DIR = "components/core/i18n/po"
POT_PATH = PO_DIR + "/messages.pot"
MARKED_PATH = PO_DIR + "/MARKED"

# Marker -> (index of the context argument or None, indexes of the literal text arguments).
# i18n_text / i18n_plural take a catalog first (any expression, skipped); their context may be NULL.
MARKERS = {
    "TR": (None, (0,)),
    "TR_C": (0, (1,)),
    "TRN": (None, (0, 1)),
    "TRN_C": (0, (1, 2)),
    "TR_NOOP": (None, (0,)),
    "TR_NOOP_C": (0, (1,)),
    "I18N_NOOP": (None, (0,)),
    "I18N_NOOP_C": (0, (1,)),
    "i18n_text": (1, (2,)),
    "i18n_plural": (1, (2, 3)),
}
FIXED = "NOTR"
# Calls whose string arguments are not words for people (rule 7). Extend as the UI units need.
EXEMPT_CALLS = frozenset("""
    LV_LOG_ERROR LV_LOG_WARN LV_LOG_INFO LV_LOG_USER LV_LOG_TRACE LV_LOG
    ESP_LOGE ESP_LOGW ESP_LOGI ESP_LOGD ESP_LOGV ESP_EARLY_LOGE ESP_EARLY_LOGW ESP_EARLY_LOGI
    ESP_LOGE_ONCE printf fprintf snprintf_log puts strcmp strncmp strstr strcasecmp strncasecmp
    lv_obj_find_by_name _Static_assert
""".split())
# Automatic translator notes (I18N-023, I18N-035): a helper name -> {argument index or "*": (note,
# budget)}, applied to a marker that is a direct argument of that call when its entry has no
# "At most N bytes" of its own. Only helpers whose destination was read in the source; the budget is
# destination size - 1 (a string subject is UI_SUBJECT_STRING_LENGTH = 128 bytes: 127).
_SUBJECT = 127
CALL_NOTES = {
    # ui_toast -> ui_set_str(&subject_toast); ui_set_str -> lv_subject_copy_string (128 bytes)
    "ui_toast": {"*": ("A toast (one line over the screen). At most 127 bytes.", _SUBJECT)},
    "ui_set_str": {"*": ("At most 127 bytes.", _SUBJECT)},
    # ui_pad_error -> ui_set_str(&subject_pad_hint)
    "ui_pad_error": {"*": ("The code pad's hint line. At most 127 bytes.", _SUBJECT)},
    # ask() copies into S.pad_ask_title[64] and S.pad_ask_hint[96] (ui_internal.h)
    "ui_pad_ask": {2: ("The code pad's title. At most 63 bytes.", 63),
                   3: ("The code pad's hint line. At most 95 bytes.", 95)},
    "ui_pad_ask_new": {2: ("The code pad's title. At most 63 bytes.", 63)},
    # ui_forms.c: title, note, ok and cancel all go through ui_set_str into sheet subjects
    "ui_confirm": {"*": ("A confirmation sheet. At most 127 bytes.", _SUBJECT)},
    "ui_confirm_choice": {"*": ("A confirmation sheet. At most 127 bytes.", _SUBJECT)},
    "ui_text_sheet": {0: ("The sheet's title. At most 127 bytes.", _SUBJECT),
                      1: ("The sheet's note. At most 127 bytes.", _SUBJECT)},
    "ui_text_sheet_kind": {0: ("The sheet's title. At most 127 bytes.", _SUBJECT),
                           1: ("The sheet's note. At most 127 bytes.", _SUBJECT)},
    "ui_pick_sheet": {0: ("The sheet's title. At most 127 bytes.", _SUBJECT),
                      1: ("The sheet's note. At most 127 bytes.", _SUBJECT)},
    "ui_confirm_primary": {"*": ("A confirmation sheet with a blue OK. At most 127 bytes.", _SUBJECT)},
    "ui_time_sheet_open": {0: ("The time and days sheet's title (one line). At most 127 bytes.", _SUBJECT),
                           1: ("The time and days sheet's note, under its title. At most 127 bytes.",
                               _SUBJECT)},
    "ui_window_sheet_open": {0: ("The From and To sheet's title (one line). At most 127 bytes.", _SUBJECT)},
    # ...their result lines and a row's third line are LVGL labels (no fixed buffer: no budget)
    "ui_time_sheet_result": {0: ("The line under the time and days sheet: the whole sentence, or what is "
                                 "missing (wraps).", None)},
    "ui_window_sheet_result": {0: ("The line under the From and To sheet (one line; a long one ends in "
                                   "...).", None)},
    "ui_row_sub": {1: ("A row's third line, under its detail, in a warning colour (wraps).", None)},
    # ui_forms.c: rows, sections and paragraphs set LVGL labels (no fixed buffer: no budget). A row's
    # labels are one line each and end in "..." when too long for the row.
    "ui_row_link": {3: ("A list row's title (one line; a long one ends in ...).", None),
                    4: ("A list row's detail line, under its title (one line).", None),
                    5: ("A list row's value, at its right end (keep it short).", None)},
    "ui_row_toggle": {1: ("A switch row's title (one line).", None),
                      2: ("A switch row's detail line, under its title (one line).", None)},
    "ui_row_number": {1: ("A number row's title (one line).", None),
                      2: ("A number row's detail line, under its title (one line).", None),
                      9: ("A number row's value when the number is zero (keep it short).", None)},
    "ui_row_level": {2: ("A slider row's title (one line).", None)},
    "ui_row_action": {2: ("A button row's text (one line).", None)},
    "ui_row_value": {1: ("A list row's title (one line).", None),
                     2: ("A list row's value, at its right end (keep it short).", None)},
    "ui_row_hold": {1: ("A press-and-hold button's text (one line).", None)},
    "ui_section": {1: ("A section heading above a group of rows (one line).", None)},
    "ui_paragraph": {1: ("An explanation under a group of rows (wraps over several lines).", None)},
    # ui_wizard.c: side() and footer() go through ui_set_str into the setup's subjects
    "side": {1: ("First-time setup: the step's title on the left. At most 127 bytes.", _SUBJECT),
             2: ("First-time setup: the explanation under the step's title. At most 127 bytes.",
                 _SUBJECT)},
    "footer": {0: ("First-time setup: the main button, bottom right. At most 127 bytes.", _SUBJECT)},
    # ui_settings.c: the title is copied into s_ask_title[32], the reason into the page's note
    "ui_settings_locked": {2: ("A locked page's name, on the code pad that opens it. At most 31 bytes.",
                               31),
                           3: ("A locked page: why it needs a code, under its title. At most 127 bytes.",
                               _SUBJECT)},
}
# A marker copied straight into a Settings page's own title or note (build_page in ui_settings.c:
# title[48], and the note goes to a string subject): i18n_utf8_copy(title|note, len, TR(...)).
COPY_NOTES = {
    "title": ("A Settings page's title. At most 47 bytes.", 47),
    "note": ("A Settings page: the line under its title. At most 127 bytes.", _SUBJECT),
}
SKIP_DIRECTIVES = {"include", "error", "pragma", "warning"}
SKIP_DIRS = {"test", "third_party", "fonts"}
EXTS = (".c", ".h")

FNV_BASIS = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv1a32(data):
    h = FNV_BASIS
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def key_hash(ctx, msgid):
    data = msgid.encode("utf-8")
    if ctx is not None:
        data = ctx.encode("utf-8") + b"\x04" + data
    return fnv1a32(data)


# ---------------------------------------------------------------- the C lexer

class Comment:
    def __init__(self, first, last, text, trailing):
        self.first = first
        self.last = last
        self.trailing = trailing  # code precedes it on its first line
        self.text = text


_ESC = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
        "\\": 92, '"': 34, "'": 39, "?": 63}
_IDENT = re.compile(r"[A-Za-z_]\w*")
_NUMBER = re.compile(r"\.?\d[\w.]*")
_DIRECTIVE = re.compile(r"#[ \t]*(\w+)?[ \t]*(\w+)?")


def _skip_logical_line(text, i):
    """Index of the newline ending the logical line that starts at i (backslash continues)."""
    n = len(text)
    while i < n:
        j = text.find("\n", i)
        if j < 0:
            return n
        if j > 0 and text[j - 1] == "\\":
            i = j + 1
            continue
        if j > 1 and text[j - 1] == "\r" and text[j - 2] == "\\":
            i = j + 1
            continue
        return j
    return n


def _read_string(text, i):
    """Decode the literal starting at the quote at i: (value, index after it, lines spanned)."""
    n = len(text)
    out = bytearray()
    i += 1
    lines = 0
    while i < n and text[i] != '"' and text[i] != "\n":
        c = text[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        if i >= n:
            break
        c = text[i]
        if c == "\n":
            lines += 1
            i += 1
        elif c == "\r" and text[i + 1:i + 2] == "\n":
            lines += 1
            i += 2
        elif c in _ESC:
            out.append(_ESC[c])
            i += 1
        elif c in "01234567":
            j = i
            while j < n and j < i + 3 and text[j] in "01234567":
                j += 1
            out.append(int(text[i:j], 8) & 0xFF)
            i = j
        elif c == "x":
            j = i + 1
            while j < n and text[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(text[i + 1:j] or "0", 16) & 0xFF)
            i = j
        elif c in "uU":
            width = 4 if c == "u" else 8
            digits = text[i + 1:i + 1 + width]
            if len(digits) == width and all(d in "0123456789abcdefABCDEF" for d in digits):
                cp = int(digits, 16)
                try:
                    out += chr(cp).encode("utf-8")
                except (ValueError, UnicodeEncodeError):
                    out += "�".encode("utf-8")
                i += 1 + width
            else:
                out += c.encode("utf-8")
                i += 1
        else:
            out += c.encode("utf-8")
            i += 1
    return out.decode("utf-8", errors="replace"), i + 1, lines


def lex_c(text, marker_names):
    """Tokens (kind, value, line) and comments of a C file. Adjacent literals are one token.
    #include lines and the #define lines of the markers themselves are skipped."""
    tokens = []
    comments = []
    n = len(text)
    i = 0
    line = 1
    code = False
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            code = False
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            comments.append(Comment(line, line, text[i + 2:j], code))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            body = text[i + 2:(n if j < 0 else j)]
            lines = text.count("\n", i, end)
            comments.append(Comment(line, line + lines, body, code))
            line += lines
            i = end
            continue
        if c == "#" and not code:
            m = _DIRECTIVE.match(text, i)
            word, name = m.group(1), m.group(2)
            if word in SKIP_DIRECTIVES or (word == "define" and name in marker_names):
                j = _skip_logical_line(text, i)
                line += text.count("\n", i, j)
                i = j
                continue
            i += 1
            code = True
            continue
        code = True
        if c == '"':
            value, i, lines = _read_string(text, i)
            if tokens and tokens[-1][0] == "str":
                tokens[-1] = ("str", tokens[-1][1] + value, tokens[-1][2])
            else:
                tokens.append(("str", value, line))
            line += lines
            continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            tokens.append(("char", text[i:j + 1], line))
            i = j + 1
            continue
        m = _IDENT.match(text, i)
        if m:
            tokens.append(("id", m.group(), line))
            i = m.end()
            continue
        m = _NUMBER.match(text, i)
        if m:
            tokens.append(("num", m.group(), line))
            i = m.end()
            continue
        tokens.append(("op", c, line))
        i += 1
    return tokens, comments


# ------------------------------------------------------------ extraction (C)

class Occurrence:
    def __init__(self, path, line, ctx, msgid, plural, notes, budget):
        self.path = path
        self.line = line
        self.ctx = ctx
        self.msgid = msgid
        self.plural = plural
        self.notes = notes
        self.budget = budget

    @property
    def key(self):
        return (self.ctx, self.msgid)


_BUDGET = re.compile(r"\bat most (\d+) bytes\b", re.I)


def _comment_note(comment):
    """The text after 'TR:' in a comment, or None."""
    lines = []
    for raw in comment.text.split("\n"):
        lines.append(raw.strip().lstrip("*").strip())
    text = " ".join(t for t in lines if t)
    m = re.match(r"TR:\s*(.*)$", text, re.S)
    if not m:
        return None
    note = m.group(1).strip()
    return re.sub(r"\s*\*/\s*$", "", note) or None


def _statement_line(tokens, i):
    """Line of the first token of the statement (or initialiser element) holding tokens[i]."""
    depth = 0
    comma = None  # nearest depth-0 comma, valid only when the enclosing bracket is a '{'
    j = i - 1
    while j >= 0:
        kind, value, _ = tokens[j]
        if kind == "op":
            if value in ")]":
                depth += 1
            elif value in "([":
                if depth:
                    depth -= 1
                else:
                    comma = None  # inside a call: its commas separate arguments
            elif depth == 0 and value == "," and comma is None:
                comma = j
            elif depth == 0 and value in ";{}":
                if value != "{":
                    comma = None
                break
            elif depth == 0 and value == ":" and _is_label_colon(tokens, j):
                comma = None  # a `case X:` / `default:` label ends the statement before it
                break
        j -= 1
    return tokens[(comma if comma is not None else j) + 1][2]


def _is_label_colon(tokens, j):
    """True when tokens[j] is the colon closing a `case X:` or `default:` label (not a ?: colon)."""
    k = j - 1
    depth = 0
    while k >= 0:
        kind, value, _ = tokens[k]
        if kind == "op":
            if value in ")]":
                depth += 1
            elif value in "([":
                depth -= 1
                if depth < 0:
                    return False
            elif depth == 0 and value in ";{}:?":
                break
        k -= 1
    first = tokens[k + 1]
    return first[0] == "id" and (first[1] == "case" or (first[1] == "default" and k + 2 == j))


def _enclosing_call(tokens, i):
    """(name, argument index, first argument's identifier or None) of the call whose argument list
    directly holds tokens[i], or None."""
    depth = 0
    commas = 0
    j = i - 1
    while j >= 0:
        kind, value, _ = tokens[j]
        if kind == "op":
            if value in ")]}":
                depth += 1
            elif value in "([{":
                if depth:
                    depth -= 1
                elif value == "(":
                    if j and tokens[j - 1][0] == "id":
                        first = (tokens[j + 1][1] if j + 2 < len(tokens) and tokens[j + 1][0] == "id"
                                 and tokens[j + 2][1] == "," else None)
                        return tokens[j - 1][1], commas, first
                    return None
                else:
                    return None
            elif depth == 0 and value == ",":
                commas += 1
            elif depth == 0 and value == ";":
                return None
        j -= 1
    return None


def _declaration_line(tokens, j):
    """Line where the declaration whose initialiser opens at tokens[j] starts; an inline
    `struct {...}` / `union` / `enum` body before the declarator belongs to the declaration."""
    line = _statement_line(tokens, j)
    k = j - 1
    while k >= 0 and tokens[k][2] >= line:
        if tokens[k][:2] == ("op", "}"):
            depth = 0
            m = k
            while m >= 0:
                if tokens[m][0] == "op" and tokens[m][1] in "}":
                    depth += 1
                elif tokens[m][0] == "op" and tokens[m][1] == "{":
                    depth -= 1
                    if depth == 0:
                        break
                m -= 1
            head = [t[1] for t in tokens[max(m - 2, 0):m]]
            if m > 0 and ("struct" in head or "union" in head or "enum" in head):
                return _statement_line(tokens, m)
            return line
        k -= 1
    return line


def _is_designator(tokens, eq):
    """True when tokens[eq] (an '=') follows a designator (`[n]` or `.field`) inside an
    initialiser list, not a declarator such as `NAME[] =`."""
    if eq >= 2 and tokens[eq - 2][:2] == ("op", ".") or eq >= 3 and tokens[eq - 2][0] == "id"             and tokens[eq - 3][:2] == ("op", "."):
        return True
    if eq >= 1 and tokens[eq - 1][:2] == ("op", "]"):
        depth = 0
        k = eq - 1
        while k >= 0:
            if tokens[k][:2] == ("op", "]"):
                depth += 1
            elif tokens[k][:2] == ("op", "["):
                depth -= 1
                if depth == 0:
                    break
            k -= 1
        return k >= 1 and tokens[k - 1][1] in ("{", ",")
    return False


def _initialiser_line(tokens, i):
    """Line of the declaration whose `= { ... }` initialiser list holds tokens[i], or None."""
    depth = 0
    j = i - 1
    while j >= 0:
        kind, value, _ = tokens[j]
        if kind == "op":
            if value in ")]}":
                depth += 1
            elif value in "([{":
                if depth:
                    depth -= 1
                elif value == "{":
                    prev = tokens[j - 1][1] if j else ""
                    designated = prev == "=" and _is_designator(tokens, j - 1)
                    if prev == "=" and not designated:  # `[i] = {` / `.f = {` are elements
                        return _declaration_line(tokens, j)
                    if prev not in ("{", ",") and not designated:
                        return None
                else:
                    return None
            elif depth == 0 and value == ";":
                return None
        j -= 1
    return None


def _notes_for(comments, line, start=None):
    """Notes on the marker's line, alone above it, or alone above the statement that holds it."""
    notes = []
    for cm in comments:
        above = not cm.trailing and (cm.last == line - 1 or (start is not None and cm.last == start - 1))
        if cm.last == line or cm.first <= line <= cm.last or above:
            note = _comment_note(cm)
            if note:
                notes.append(note)
    return notes


_BLOCK_KEYWORDS = ("switch", "if", "for", "while")


def _block_notes(tokens, comments, i):
    """The note standing alone above the nearest enclosing switch/if/else/for/while/do statement
    of tokens[i]. Stops at a function body or a declaration's initialiser (they have their own
    rules); a `case`'s own note wins because the caller only asks when the marker has none."""
    depth = 0
    j = i - 1
    while j >= 0:
        kind, value, _ = tokens[j]
        if kind == "op":
            if value in ")]}":
                depth += 1
            elif value in "([{":
                if depth:
                    depth -= 1
                elif value == "{":
                    prev = tokens[j - 1] if j else ("op", "", 0)
                    keyword = False
                    if prev[:2] == ("id", "else") or prev[:2] == ("id", "do"):
                        keyword = True
                    elif prev[:2] == ("op", ")"):
                        depth2 = 0
                        k = j - 1
                        while k >= 0:
                            if tokens[k][0] == "op":
                                if tokens[k][1] == ")":
                                    depth2 += 1
                                elif tokens[k][1] == "(":
                                    depth2 -= 1
                                    if depth2 == 0:
                                        break
                            k -= 1
                        if k > 0 and tokens[k - 1][0] == "id" and tokens[k - 1][1] in _BLOCK_KEYWORDS:
                            keyword = True
                        else:
                            return []  # a function body
                    elif prev[:2] == ("op", "=") or prev[:2] == ("op", ","):
                        return []  # a declaration's initialiser
                    if keyword:
                        line = _statement_line(tokens, j)
                        notes = [n for cm in comments if not cm.trailing and cm.last == line - 1
                                 for n in [_comment_note(cm)] if n]
                        if notes:
                            return notes
        j -= 1
    return []


def _matching_paren(tokens, i):
    """Index of the ')' closing the '(' at i, or None."""
    depth = 0
    for j in range(i, len(tokens)):
        kind, value, _ = tokens[j]
        if kind == "op":
            if value in "([{":
                depth += 1
            elif value in ")]}":
                depth -= 1
                if depth == 0:
                    return j
    return None


def _split_args(tokens):
    args = [[]]
    depth = 0
    for tok in tokens:
        if tok[0] == "op" and tok[1] in "([{":
            depth += 1
        elif tok[0] == "op" and tok[1] in ")]}":
            depth -= 1
        if tok[0] == "op" and tok[1] == "," and depth == 0:
            args.append([])
        else:
            args[-1].append(tok)
    return args


def _literal(arg):
    """The text of exactly one (possibly concatenated) plain literal, or None. C ends the text at
    the first NUL character (a backslash-zero escape included): what follows never reaches a reader."""
    if len(arg) == 1 and arg[0][0] == "str":
        return arg[0][1].split(chr(0), 1)[0]
    return None


_NO_CONTEXT = (["NULL"], ["(", "NULL", ")"], ["0"])


def extract_c(text, path, problems=None):
    """Occurrences of every marker in a C source, in order of appearance. A marker whose text or
    context holds a string literal that is not exactly one plain literal (a conditional, u8"..." or
    another prefix) is added to `problems` as (path, line, message), never dropped silently."""
    if problems is None:
        problems = []
    tokens, comments = lex_c(text, set(MARKERS) | {FIXED})
    found = []
    for i in range(len(tokens) - 1):
        kind, name, line = tokens[i]
        if kind != "id" or name not in MARKERS or tokens[i + 1][:2] != ("op", "("):
            continue
        end = _matching_paren(tokens, i + 1)
        if end is None:
            continue
        args = _split_args(tokens[i + 2:end])
        ctx_at, text_at = MARKERS[name]
        if max(text_at + (ctx_at,) if ctx_at is not None else text_at) >= len(args):
            continue
        ctx = None
        if ctx_at is not None:
            arg = args[ctx_at]
            if not any(t[0] == "str" for t in arg) and [t[1] for t in arg] in _NO_CONTEXT:
                ctx = None
            else:
                ctx = _literal(arg)
                if ctx is None:
                    if any(t[0] == "str" for t in arg):
                        problems.append((path, line, "%s: the context must be one plain string "
                                         "literal (or NULL), not an expression or a prefixed "
                                         "literal" % name))
                    continue
                if ctx == "":
                    ctx = None
        words = [_literal(args[k]) for k in text_at]
        bad = [k for k in text_at if _literal(args[k]) is None and any(t[0] == "str" for t in args[k])]
        if bad:
            problems.append((path, line, "%s: the text must be one plain string literal (adjacent "
                             "literals are fine), not a conditional, u8\"...\" or another "
                             "prefixed literal" % name))
            continue
        if any(w is None for w in words) or not words[0]:
            continue
        notes = _notes_for(comments, line, _statement_line(tokens, i))
        if not notes:
            # a note alone above a table declaration reaches every element without a note of its own
            decl = _initialiser_line(tokens, i)
            if decl is not None:
                notes = [n for cm in comments
                         if not cm.trailing and cm.last == decl - 1
                         for n in [_comment_note(cm)] if n]
            else:
                notes = _block_notes(tokens, comments, i)
        budget = None
        for note in notes:
            m = _BUDGET.search(note)
            if m:
                budget = int(m.group(1)) if budget is None else min(budget, int(m.group(1)))
        if budget is None:
            call = _enclosing_call(tokens, i)
            auto = CALL_NOTES.get(call[0]) if call else None
            if auto is not None:
                note, size = auto.get(call[1], auto.get("*", (None, None)))
                if note is not None:
                    notes = notes + [note]
                    budget = size
            elif call and call[0] == "i18n_utf8_copy" and call[1] == 2 and call[2] in COPY_NOTES:
                note, size = COPY_NOTES[call[2]]
                notes = notes + [note]
                budget = size
        found.append(Occurrence(path, line, ctx, words[0], words[1] if len(words) > 1 else None,
                                notes, budget))
    return found


# ------------------------------------------------------------ extraction (layout XML)

LAYOUTS_DIR = "components/ui/alarm_ui/layouts"
XML_NOTE = "Text from the layout XML, no context."


def _lvpro_gen():
    """tools/lvpro_gen.py: it owns the rule for which XML text is text for people (is_ui_text) and
    the tables of which attributes are strings, so the generator and this extractor agree."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("lvpro_gen", Path(__file__).resolve().parent / "lvpro_gen.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def layout_projects(root):
    """Repo-relative POSIX folders of every LVGL Pro project (a folder with a project.xml) under
    the layouts, sorted."""
    base = Path(root) / LAYOUTS_DIR
    if not base.is_dir():
        return []
    return sorted(p.parent.relative_to(root).as_posix() for p in base.glob("*/*/project.xml"))


def extract_xml_layout(root, rel_dir):
    """Occurrences of the literal text in one layout's XML: exactly the string values the generator
    routes through alarm_ui_tr - a label's or text area's text, a textarea placeholder, a component
    property's value (or its default where an instance leaves it out) and a string subject's
    initial value - that is_ui_text() accepts. The place is the XML file that holds the text; a
    `<!-- tr-context: ... -->` comment before an element gives its texts a context."""
    gen = _lvpro_gen()
    project = gen.Project(Path(root) / rel_dir)
    found = []

    def add(path, text, ctx=None):
        if text and gen.is_ui_text(text):
            found.append(Occurrence(path, 0, ctx, text, None, [XML_NOTE], None))

    def walk(view, path):
        for elem in view.iter():
            comp = project.components.get(elem.tag)
            if comp is not None:
                for pname, ptype, pdefault in comp.props:
                    if ptype != "string":
                        continue
                    if pname in elem.attrib:
                        add(path, elem.attrib[pname], gen.TR_CTX.get(elem))
                    elif pdefault is not None:
                        add(comp_paths[elem.tag], pdefault)
                continue
            for key, (_tmpl, typ) in gen.WIDGET_ATTRS.get(elem.tag, {}).items():
                if typ == "string" and key in elem.attrib:
                    add(path, elem.attrib[key], gen.TR_CTX.get(elem))

    comp_paths = {}
    files = []
    base = Path(root) / rel_dir
    for path in sorted((base / "components").rglob("*.xml")):
        tree = gen.parse_xml(path)  # the generator's reading: tr-context comments included
        if tree.tag == "component":
            comp_paths[path.stem] = path.relative_to(root).as_posix()
        files.append((path, tree))
    for path in sorted((base / "screens").glob("*.xml")):
        files.append((path, gen.parse_xml(path)))
    g = (base / "globals.xml").relative_to(root).as_posix()
    for typ, _name, val, _attrib in project.subjects:
        if typ == "string":
            add(g, val)
    for path, tree in files:
        view = tree.find("view")
        if tree.tag in ("component", "screen") and view is not None:
            walk(view, path.relative_to(root).as_posix())
    return found


def extract_xml(root, problems=None):
    out = []
    for rel in layout_projects(root):
        out += extract_xml_layout(root, rel)
    return out


# ------------------------------------------------------------ extraction (Panel Manager JS)

# panel-words.js: t(text, ...), tc(ctx, text, ...), tn(one, other, n, ...) translate; ht(text, ...)
# translates and escapes for HTML, hx(text, ...) too around values that are HTML; tx(text) only
# marks. The texts must be plain quoted literals
# ('...' or "..."; a `...` without ${}); anything else is a value and is skipped, as a variable is
# in TR().
JS_MARKERS = {"t": (None, 1), "tc": (0, 2), "tn": (None, 2), "tx": (None, 1), "ht": (None, 1),
              "hx": (None, 1)}
JS_DIRS = ("components/services/panel_web/web",)
_JS_CALL = re.compile(r"(?<![\w$.])(tc|tn|tx|ht|hx|t)\(")


def used_by_web(places):
    """An entry the Panel Manager's pages use: one of its places is in their folder. The panel
    sends those entries to a browser (I18N_ENTRY_WEB, i18n_web_json)."""
    return any(p.split(":")[0].rsplit("/", 1)[0] in JS_DIRS for p in places)
_JS_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", "'": "'", '"': '"', "`": "`", "0": "\0"}


def _js_literal(text, i):
    """(value, index after it) of the quoted literal at text[i], or None."""
    if i >= len(text) or text[i] not in "'\"`":
        return None
    quote, out, j = text[i], [], i + 1
    while j < len(text):
        c = text[j]
        if c == quote:
            return "".join(out), j + 1
        if c == "\\" and j + 1 < len(text):
            n = text[j + 1]
            if n == "u" and text[j + 2:j + 3] != "{":
                out.append(chr(int(text[j + 2:j + 6], 16)))
                j += 6
                continue
            out.append(_JS_ESCAPES.get(n, n))
            j += 2
            continue
        if quote == "`" and text.startswith("${", j):
            return None
        if c == "\n" and quote != "`":
            return None
        out.append(c)
        j += 1
    return None


def _js_notes(lines, line):
    """The `// TR: ...` comment lines just above a marker (1-based line)."""
    notes, k = [], line - 2
    while k >= 0 and lines[k].strip().startswith("//"):
        notes.insert(0, lines[k].strip()[2:].strip())
        k -= 1
    text = " ".join(notes)
    return [text[3:].strip()] if text.startswith("TR:") else []


def extract_js(text, path, problems=None):
    found = []
    lines = text.split("\n")
    for m in _JS_CALL.finditer(text):
        ctx_at, count = JS_MARKERS[m.group(1)]
        args, pos = [], m.end()
        for k in range(count):
            while pos < len(text) and text[pos] in " \t\r\n":
                pos += 1
            lit = _js_literal(text, pos)
            if lit is None:
                break
            args.append(lit[0])
            pos = lit[1]
            while pos < len(text) and text[pos] in " \t\r\n":
                pos += 1
            if k < count - 1:
                if pos >= len(text) or text[pos] != ",":
                    break
                pos += 1
        if len(args) < count:
            continue  # a value, not words
        line = text.count("\n", 0, m.start()) + 1
        if m.group(1) == "tc":
            ctx, msgid, plural = args[0] or None, args[1], None
        elif m.group(1) == "tn":
            ctx, msgid, plural = None, args[0], args[1]
        else:
            ctx, msgid, plural = None, args[0], None
        if not msgid:
            continue
        found.append(Occurrence(path, line, ctx, msgid, plural, _js_notes(lines, line), None))
    return found


def extract_html(text, path, problems=None):
    """panel.html's own words: the text of an element marked data-t and the aria-label of one marked
    data-t-label (panel-words.js translatePage)."""
    from html.parser import HTMLParser
    found = []

    class Reader(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.open = None  # (tag, line, [text])

        def handle_starttag(self, tag, attrs):
            a = dict(attrs)
            line = self.getpos()[0]
            if "data-t-label" in a and a.get("aria-label"):
                found.append(Occurrence(path, line, None, a["aria-label"], None, [], None))
            if "data-t" in a:
                self.open = (tag, line, [])

        def handle_data(self, data):
            if self.open:
                self.open[2].append(data)

        def handle_endtag(self, tag):
            if self.open and tag == self.open[0]:
                words = " ".join("".join(self.open[2]).split())
                if words:
                    found.append(Occurrence(path, self.open[1], None, words, None, [], None))
                self.open = None

    Reader().feed(text)
    return found


# ------------------------------------------------ unmarked words in the Panel Manager's files

# Before these a "/" starts a regular expression, not a division.
_JS_REGEX_AFTER = {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw",
                   "case", "do", "else", "yield", "await"}
_JS_PUNCT = re.compile(r">>>=|\.\.\.|===|!==|\*\*=|<<=|>>=|>>>|&&=|\|\|=|\?\?=|=>|==|!=|<=|>=|&&|"
                       r"\|\||\?\?|\?\.|\+\+|--|[-+*/%&|^]=|<<|>>|\*\*|[{}()\[\];,<>+\-*/%&|^!~?:=.@#]")
_JS_ID = re.compile(r"[A-Za-z_$][\w$]*")
_JS_NUM = re.compile(r"0[xX][0-9a-fA-F_]+n?|0[bB][01_]+n?|0[oO][0-7_]+n?|(?:\d[\d_]*\.?[\d_]*|\.\d[\d_]*)(?:[eE][-+]?\d+)?n?")


def lex_js(text):
    """Tokens (kind, value, line) of JavaScript: 'id', 'num', 'str' (a quoted literal's value),
    'tpl' (a template literal's text parts, a list; the tokens of its ${} expressions follow it),
    're', 'op'. Comments are dropped. Enough for finding words, not a full parser."""
    tokens = []
    n = len(text)
    i = 0
    braces = []  # per open "{": the template token whose ${ it opened, or None

    def line_at(pos):
        return text.count("\n", 0, pos) + 1

    def template_part(j, tok):
        """Read a template's text from j to its end or the next ${; returns the index after."""
        out = []
        while j < n:
            c = text[j]
            if c == "`":
                tok[1].append("".join(out))
                return j + 1
            if c == "$" and text.startswith("${", j):
                tok[1].append("".join(out))
                braces.append(tok)
                return j + 2
            if c == "\\" and j + 1 < n:
                out.append(_JS_ESCAPES.get(text[j + 1], text[j + 1]))
                j += 2
                continue
            out.append(c)
            j += 1
        tok[1].append("".join(out))
        return j

    def regex_allowed():
        if not tokens:
            return True
        kind, value, _ = tokens[-1]
        if kind in ("num", "str", "tpl", "re"):
            return False
        if kind == "id":
            return value in _JS_REGEX_AFTER
        return value not in (")", "]", "}")

    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "'\"":
            lit = _js_literal(text, i)
            if lit is None:  # unterminated: skip the quote
                i += 1
                continue
            tokens.append(("str", lit[0], line_at(i)))
            i = lit[1]
        elif c == "`":
            tok = ("tpl", [], line_at(i))
            tokens.append(tok)
            i = template_part(i + 1, tok)
        elif c == "/" and regex_allowed():
            j, in_class = i + 1, False
            while j < n and text[j] != "\n":
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "[":
                    in_class = True
                elif text[j] == "]":
                    in_class = False
                elif text[j] == "/" and not in_class:
                    break
                j += 1
            j += 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            tokens.append(("re", text[i:j], line_at(i)))
            i = j
        elif c == "{":
            braces.append(None)
            tokens.append(("op", "{", line_at(i)))
            i += 1
        elif c == "}":
            owner = braces.pop() if braces else None
            if owner is not None:  # the end of a ${ }: the template goes on
                i = template_part(i + 1, owner)
            else:
                tokens.append(("op", "}", line_at(i)))
                i += 1
        else:
            m = _JS_ID.match(text, i)
            if m:
                tokens.append(("id", m.group(), line_at(i)))
                i = m.end()
                continue
            m = _JS_NUM.match(text, i)
            if m and m.group():
                tokens.append(("num", m.group(), line_at(i)))
                i = m.end()
                continue
            m = _JS_PUNCT.match(text, i)
            if m:
                tokens.append(("op", m.group(), line_at(i)))
                i = m.end()
                continue
            i += 1  # anything else (a stray character) is skipped
    return tokens


# Calls whose text arguments are never shown: the page's markers, fixed text (nt), selectors,
# requests, storage, events and attributes by name.
JS_FIXED = "nt"
JS_EXEMPT_CALLS = frozenset("""
    $ $$ querySelector querySelectorAll closest matches getElementById fetch api changeNetwork
    addEventListener removeEventListener dispatchEvent createElement getItem setItem removeItem
    setRequestHeader open send getAttribute hasAttribute removeAttribute toggleAttribute
    replaceState pushState has get set delete add remove toggle contains includes startsWith
    endsWith indexOf split join replace replaceAll test match exec padStart padEnd
    RegExp Error_code console log warn error debug go poll icon tag_kind encodeURIComponent
    decodeURIComponent JSON parse stringify postMessage Worker import require
""".split())
_JS_COMPARE = {"===", "!==", "==", "!="}
_SHOWN_ATTR = re.compile(r'\b(?:title|aria-label|placeholder|alt)="([^"\x00]*)"')
_TAG = re.compile(r"<[^<>]*>")
_ENTITY = re.compile(r"&[A-Za-z][A-Za-z0-9]*;|&#[xX]?[0-9A-Fa-f]+;")


def visible_text(s):
    """The pieces of a string or template (its ${} gaps as NUL) that a person would read: the text
    between tags and the shown attributes (title, aria-label, placeholder, alt)."""
    shown = _SHOWN_ATTR.findall(s)
    rest = _ENTITY.sub(" ", _TAG.sub("\x00", s))
    return [p.strip() for p in shown + rest.split("\x00") if p.strip()]


def js_words(s, loose=False):
    """Words a person reads, not a class name, key or identifier. Strict (the check): a capital
    followed by a small letter, or two words ending like a sentence. Loose: any two letters with a
    space."""
    if not re.search(r"[A-Za-z]{2}", s):
        return False
    if loose:
        return " " in s or bool(re.search(r"[A-Z][a-z]", s))
    return bool(re.search(r"[A-Z][a-z]", s)) or bool(re.search(r"[a-z] [a-z].*[.!?…:]$", s))


def unmarked_js(text, path, loose=False):
    """(path, line, text) for every word-like literal or template text of JavaScript outside the
    markers (t, tc, tn, ht, tx), nt() and the exempt calls; object keys, comparisons and case labels
    are not words."""
    tokens = lex_js(text)
    stack = []
    found = []
    markers = set(JS_MARKERS) | {JS_FIXED}
    for i, (kind, value, line) in enumerate(tokens):
        if kind == "op" and value == "(":
            stack.append(tokens[i - 1][1] if i and tokens[i - 1][0] == "id" else None)
            continue
        if kind == "op" and value == ")":
            if stack:
                stack.pop()
            continue
        if kind not in ("str", "tpl"):
            continue
        if any(f in markers or f in JS_EXEMPT_CALLS for f in stack if f):
            continue
        prev = tokens[i - 1] if i else ("op", "", 0)
        nxt = tokens[i + 1] if i + 1 < len(tokens) else ("op", "", 0)
        if kind == "str":
            if nxt[1] == ":" and prev[1] in ("{", ","):
                continue  # an object's key
            if prev[1] in _JS_COMPARE or nxt[1] in _JS_COMPARE or prev[1] == "case":
                continue
            if value == "use strict":
                continue
            pieces = visible_text(value)
        else:
            pieces = visible_text("\x00".join(value))
        for p in pieces:
            if js_words(p, loose):
                found.append((path, line, p))
    return found


def unmarked_html(text, path, loose=False):
    """(path, line, text) for panel.html text a person reads that is neither marked (data-t,
    data-t-label) nor fixed (translate="no")."""
    from html.parser import HTMLParser
    found = []

    class Reader(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.stack = []  # (tag, marked, fixed)

        def handle_starttag(self, tag, attrs):
            a = dict(attrs)
            line = self.getpos()[0]
            fixed = a.get("translate") == "no" or any(s[2] for s in self.stack)
            for name in ("title", "aria-label", "placeholder", "alt"):
                v = a.get(name)
                if v and js_words(v, loose) and not fixed and not (name == "aria-label" and "data-t-label" in a):
                    found.append((path, line, v))
            if tag not in ("meta", "link", "img", "br", "input", "hr", "source"):
                self.stack.append((tag, "data-t" in a, fixed))

        def handle_endtag(self, tag):
            while self.stack:
                if self.stack.pop()[0] == tag:
                    break

        def handle_data(self, data):
            if not self.stack or self.stack[-1][0] in ("script", "style", "title"):
                return
            if any(s[1] or s[2] for s in self.stack):
                return
            words = " ".join(data.split())
            if words and js_words(words, loose):
                found.append((path, self.getpos()[0], words))

    Reader().feed(text)
    return found


# One extractor per source kind: (file extensions, function).
EXTRACTORS = [(EXTS, extract_c), ((".js",), extract_js), ((".html",), extract_html)]


def scan_files(root):
    """Repo-relative POSIX paths of the sources to scan, sorted."""
    base = Path(root) / "components"
    out = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        rel_dir = Path(dirpath).relative_to(root).as_posix()
        for name in filenames:
            js = name.endswith((".js", ".html")) and rel_dir in JS_DIRS  # the Panel Manager's, not mocks
            if not (name.endswith(EXTS) or js) or name.endswith(("_gen.c", "_gen.h")):
                continue
            out.append((Path(dirpath) / name).relative_to(root).as_posix())
    return sorted(out)


def read_source(root, rel):
    return (Path(root) / rel).read_text(encoding="utf-8", errors="replace")


def extract_all(root, problems=None):
    occurrences = []
    for rel in scan_files(root):
        for exts, fn in EXTRACTORS:
            if rel.endswith(exts):
                occurrences += fn(read_source(root, rel), rel, problems)
    occurrences += extract_xml(root, problems)
    return occurrences


class Entry:
    def __init__(self, occ):
        self.ctx = occ.ctx
        self.msgid = occ.msgid
        self.plural = None
        self.places = []
        self.notes = []
        self.budget = None
        self.first = occ


def build_entries(occurrences):
    """Merge occurrences by (ctx, msgid), in order of first appearance."""
    entries = {}
    for occ in occurrences:
        e = entries.get(occ.key)
        if e is None:
            e = entries[occ.key] = Entry(occ)
        if occ.plural is not None and e.plural is None:
            e.plural = occ.plural
        if occ.path not in e.places:
            e.places.append(occ.path)
        for note in occ.notes:
            if note not in e.notes:
                e.notes.append(note)
        if occ.budget is not None:
            e.budget = occ.budget if e.budget is None else min(e.budget, occ.budget)
    return entries


# ------------------------------------------------------------------ the template

HEADER = ('msgid ""\nmsgstr ""\n'
          '"Project-Id-Version: RoboAlarms\\n"\n'
          '"MIME-Version: 1.0\\n"\n'
          '"Content-Type: text/plain; charset=UTF-8\\n"\n'
          '"Content-Transfer-Encoding: 8bit\\n"\n')


def po_quote(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("\\%03o" % ord(ch))
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def render_pot(entries):
    parts = [HEADER]
    for e in entries.values():
        lines = []
        for note in e.notes:
            lines.append("#. " + note)
        for place in e.places:
            lines.append("#: " + place)
        if e.ctx is not None:
            lines.append("msgctxt " + po_quote(e.ctx))
        lines.append("msgid " + po_quote(e.msgid))
        if e.plural is not None:
            lines.append("msgid_plural " + po_quote(e.plural))
            lines.append('msgstr[0] ""')
            lines.append('msgstr[1] ""')
        else:
            lines.append('msgstr ""')
        parts.append("\n".join(lines) + "\n")
    return "\n".join(parts)


def build_template(root):
    return render_pot(build_entries(extract_all(root)))


def _read_bytes(path):
    try:
        return Path(path).read_bytes()
    except OSError:
        return None


def cmd_extract(root, check_only):
    root = Path(root)
    want = build_template(root).encode("utf-8")
    pot = root / POT_PATH
    problems = []
    extract_all(root, problems)
    for path, line, msg in problems:
        print("%s:%d: warning: %s" % (path, line, msg))
    if check_only:
        if _read_bytes(pot) != want:
            print("%s:1: template is out of date; run tools/i18n.py extract" % POT_PATH)
            return 1
        print("%s is up to date" % POT_PATH)
        return 0
    pot.parent.mkdir(parents=True, exist_ok=True)
    with open(pot, "wb") as f:
        f.write(want)
    print("wrote %s (%d entries)" % (POT_PATH, len(build_entries(extract_all(root)))))
    return 0


# ------------------------------------------------------------------------ checks
# Each check returns a list of (path, line, message).

def check_template_current(root):
    if _read_bytes(Path(root) / POT_PATH) != build_template(root).encode("utf-8"):
        return [(POT_PATH, 1, "template is out of date; run tools/i18n.py extract")]
    return []


def check_collisions(entries, hashfn=key_hash):
    seen = {}
    errors = []
    for e in entries.values():
        h = hashfn(e.ctx, e.msgid)
        other = seen.get(h)
        if other is not None:
            errors.append((e.first.path, e.first.line,
                           "hash collision 0x%08x: %s and %s; give one of them a context"
                           % (h, _show(other), _show(e))))
        else:
            seen[h] = e
    return errors


def check_plain_vs_plural(occurrences):
    """One key must not be plain in one place and plural in another (a plural entry serves form 0)."""
    plain = {}
    plural = {}
    for occ in occurrences:
        (plain if occ.plural is None else plural).setdefault(occ.key, occ)
    errors = []
    for key, p in plural.items():
        q = plain.get(key)
        if q is not None:
            errors.append((q.path, q.line, '"%s" is plain here but plural at %s:%d; use another '
                           'text or a context' % (q.msgid, p.path, p.line)))
    return errors


def _show(e):
    return '"%s"' % e.msgid if e.ctx is None else '"%s" (context "%s")' % (e.msgid, e.ctx)


def check_plural_conflicts(occurrences):
    plural = {}
    errors = []
    for occ in occurrences:
        if occ.plural is None:
            continue
        first = plural.setdefault(occ.key, occ.plural)
        if first != occ.plural:
            errors.append((occ.path, occ.line,
                           '"%s" is used with two plurals: "%s" and "%s"'
                           % (occ.msgid, first, occ.plural)))
    return errors


_PLACEHOLDER = re.compile(r"\{\{|\}\}|\{(\d)\}|[{}]")


def placeholders(s):
    """(set of placeholder numbers, list of bad brace texts)."""
    found = set()
    bad = []
    for m in _PLACEHOLDER.finditer(s):
        if m.group(1) is not None:
            found.add(m.group(1))
        elif m.group() in ("{", "}"):
            bad.append(m.group())
    return found, bad


def check_placeholders(entries):
    errors = []
    for e in entries.values():
        where = (e.first.path, e.first.line)
        singular, bad = placeholders(e.msgid)
        if bad:
            errors.append(where + ('%s: only {0}..{9}, {{ and }} may use a brace' % _show(e),))
        if e.plural is not None:
            plural, bad = placeholders(e.plural)
            if bad:
                errors.append(where + ('plural of %s: only {0}..{9}, {{ and }} may use a brace'
                                       % _show(e),))
            missing = sorted(singular - plural)
            if missing:
                errors.append(where + ("plural of %s lacks {%s}" % (_show(e), "}, {".join(missing)),))
    return errors


_PRINTF = re.compile(r"%[-+#0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcspn%]")
_MARKUP = re.compile(r"</?[A-Za-z][^<>]*>|&[A-Za-z][A-Za-z0-9]*;|&#[xX]?[0-9A-Fa-f]+;")


def check_printf_and_markup(entries):
    errors = []
    for e in entries.values():
        for what, s in (("text", e.msgid), ("plural", e.plural)):
            if s is None:
                continue
            m = _PRINTF.search(s)
            if m:
                errors.append((e.first.path, e.first.line,
                               'printf conversion "%s" in %s of %s; use {0}' % (m.group(), what,
                                                                                _show(e))))
            m = _MARKUP.search(s)
            if m:
                errors.append((e.first.path, e.first.line,
                               'markup "%s" in %s of %s; markup is not translatable text'
                               % (m.group(), what, _show(e))))
    return errors


# An object's name ("detail_label", "sheet_kb") marked as a word: translated, the UI looks for an
# object that isn't there (found by the en-XA walk, 2026-09-29). Words have no underscores.
_IDENTIFIER = re.compile(r"^[a-z][a-z0-9]*(_[a-z0-9]+)+$")


def check_identifiers(entries):
    return [(e.first.path, e.first.line,
             "%s looks like an object's name, not words: don't mark it (NOTR or no TR)" % _show(e))
            for e in entries.values() if _IDENTIFIER.match(e.msgid)]


def check_budgets(occurrences):
    errors = []
    for occ in occurrences:
        if occ.budget is None:
            continue
        for s in (occ.msgid, occ.plural):
            if s is None:
                continue
            bare = _PLACEHOLDER.sub(lambda m: "{" if m.group() == "{{" else
                                    "}" if m.group() == "}}" else "", s)
            size = len(bare.encode("utf-8"))
            if size > occ.budget:
                errors.append((occ.path, occ.line, '"%s" is %d bytes, the note allows %d'
                               % (s, size, occ.budget)))
    return errors


def looks_like_words(s):
    return bool(re.search(r"[A-Za-z]{2}", s)) and (" " in s or bool(re.search(r"[A-Z][a-z]", s)))


def unmarked_literals(text, path):
    """(path, line, literal) for every word-like literal outside a marker, NOTR() or exempt call."""
    tokens, _ = lex_c(text, set(MARKERS) | {FIXED})
    stack = []
    found = []
    for i, (kind, value, line) in enumerate(tokens):
        if kind == "op" and value == "(":
            prev = tokens[i - 1][1] if i and tokens[i - 1][0] == "id" else None
            stack.append(prev)
        elif kind == "op" and value == ")":
            if stack:
                stack.pop()
        elif kind == "str" and looks_like_words(value):
            if any(f in MARKERS or f == FIXED or f in EXEMPT_CALLS for f in stack if f):
                continue
            found.append((path, line, value))
    return found


def read_marked(root):
    """(list of (path, line)) from po/MARKED."""
    text = _read_bytes(Path(root) / MARKED_PATH)
    if text is None:
        return []
    out = []
    for n, raw in enumerate(text.decode("utf-8").splitlines(), 1):
        raw = raw.split("#", 1)[0].strip()
        if raw:
            out.append((raw, n))
    return out


def check_unmarked(root, paths=None, loose=False):
    errors = []
    if paths is None:
        paths = []
        for rel, n in read_marked(root):
            if not (Path(root) / rel).is_file():
                errors.append((MARKED_PATH, n, "%s is listed but does not exist" % rel))
            else:
                paths.append(rel)
    for rel in paths:
        if rel.endswith(".js"):
            found, how = unmarked_js(read_source(root, rel), rel, loose), "t(), ht() or nt()"
        elif rel.endswith(".html"):
            found, how = unmarked_html(read_source(root, rel), rel, loose), 'data-t or translate="no"'
        else:
            found, how = unmarked_literals(read_source(root, rel), rel), "TR() or NOTR()"
        for path, line, lit in found:
            shown = lit if len(lit) <= 60 else lit[:57] + "..."
            errors.append((path, line, 'unmarked text "%s": mark it with %s' % (shown, how)))
    return errors


def note_gaps(entries):
    """(entries with no translator note, entries with no byte budget)."""
    entries = list(entries.values())
    return [e for e in entries if not e.notes], [e for e in entries if e.budget is None]


def cmd_check(root, unmarked_all, release=None, list_notes=False):
    if unmarked_all:
        errors = check_unmarked(root, scan_files(root))
        for path, line, msg in errors:
            print("%s:%d: %s" % (path, line, msg))
        print("%d unmarked literals in %d files" % (len(errors), len(scan_files(root))))
        return 0
    problems = []
    occurrences = extract_all(root, problems)
    entries = build_entries(occurrences)
    errors = (problems + check_template_current(root) + check_collisions(entries)
              + check_plural_conflicts(occurrences) + check_plain_vs_plural(occurrences) + check_placeholders(entries)
              + check_printf_and_markup(entries) + check_identifiers(entries) + check_budgets(occurrences)
              + check_unmarked(root) + check_translations(root))
    if release:
        errors += check_release(root, release)
    for path, line, msg in errors:
        print("%s:%d: %s" % (path, line, msg))
    no_note, no_budget = note_gaps(entries)
    print("i18n check: warning: %d of %d entries have no translator note, %d have no byte budget "
          "(check --notes lists them)" % (len(no_note), len(entries), len(no_budget)))
    if list_notes:
        for what, group in (("no note", no_note), ("no budget", no_budget)):
            for e in group:
                print("%s:%d: %s: %s" % (e.first.path, e.first.line, what, _show(e)))
    if errors:
        print("i18n check: %d error(s)%s" % (len(errors), " (release %s)" % release if release else ""))
        return 1
    print("i18n check: ok (%d entries)" % len(entries))
    return 0


# ------------------------------------------------------- the plural rules (mirror of i18n_plural.c)

_MANY = "n != 0 && n % 1000000 == 0"
# rule key -> (C enumerator, categories in CLDR order, plural expression for the .po header)
PLURAL_RULES = {
    "EN": ("I18N_PLURALS_EN", ("one", "other"), "(n != 1) ? 1 : 0"),
    "OTHER": ("I18N_PLURALS_OTHER", ("other",), "0"),
    "ES": ("I18N_PLURALS_ES", ("one", "many", "other"),
           "(n == 1) ? 0 : (%s) ? 1 : 2" % _MANY),
    "FR": ("I18N_PLURALS_FR", ("one", "many", "other"),
           "(n == 0 || n == 1) ? 0 : (%s) ? 1 : 2" % _MANY),
    "AR": ("I18N_PLURALS_AR", ("zero", "one", "two", "few", "many", "other"),
           "(n == 0) ? 0 : (n == 1) ? 1 : (n == 2) ? 2 : (n % 100 >= 3 && n % 100 <= 10) ? 3 : "
           "(n % 100 >= 11) ? 4 : 5"),
    "HE": ("I18N_PLURALS_HE", ("one", "two", "other"), "(n == 1) ? 0 : (n == 2) ? 1 : 2"),
}
LANGS_OTHER = ("zh", "ja", "ko", "vi", "th", "id")
LANGS_ES = ("es", "it", "ca")
PSEUDO_LOCALES = ("en-XA", "ar-XB")
# language (2-3 lower letters), optional -Script (4 letters), optional -REGION (2 upper or 3 digits)
LOCALE_RE = re.compile(r"^[a-z]{2,3}(-[A-Za-z]{4})?(-(?:[A-Z]{2}|[0-9]{3}))?$")


def plural_rule(locale):
    """The rule key for a BCP 47 tag, exactly as i18n_plural_rule() picks it."""
    parts = re.split(r"[-_]", locale, maxsplit=1)
    primary = parts[0].lower()
    if primary in LANGS_OTHER:
        return "OTHER"
    if primary in LANGS_ES:
        return "ES"
    if primary == "pt":
        region = parts[1] if len(parts) > 1 else ""
        return "ES" if re.match(r"(?i)pt(?:[-_]|$)", region) else "FR"
    if primary == "fr":
        return "FR"
    if primary == "ar":
        return "AR"
    if primary == "he":
        return "HE"
    return "EN"


# ------------------------------------------------------------------ reading .po files

class PoEntry:
    def __init__(self):
        self.ctx = None
        self.msgid = ""
        self.plural = None
        self.msgstr = []
        self.notes = []
        self.places = []
        self.review = None
        self.fuzzy = False
        self.comments = []  # translator comments ("# text"), kept as written
        self.flags = []     # flags other than fuzzy ("c-format", "no-wrap"...)
        self.prev = []      # "#| ..." previous-msgid lines, text after the bar
        self.obsolete = False
        self.line = 0

    @property
    def key(self):
        return (self.ctx, self.msgid)

    def translated(self):
        return bool(self.msgstr) and all(s != "" for s in self.msgstr)


_UNESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"'}


def _unquote(text, path, line, errors):
    text = text.strip()
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        errors.append((path, line, "expected a quoted string"))
        return ""
    body = text[1:-1]
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        i += 1
        if c != "\\":
            out.append(c)
        elif i >= len(body):
            out.append("\\")
        elif body[i] in _UNESC:
            out.append(_UNESC[body[i]])
            i += 1
        elif body[i] in "01234567":
            j = i
            while j < len(body) and j < i + 3 and body[j] in "01234567":
                j += 1
            out.append(chr(int(body[i:j], 8)))
            i = j
        else:
            out.append(body[i])
            i += 1
    return "".join(out)


_KEYWORD = re.compile(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+(".*)$')


def parse_po(text, path, errors):
    """(header dict, PoEntry list - active and obsolete, in file order). Problems go to `errors`."""
    entries = []
    state = {"cur": None, "field": None, "started": False}

    def finish():
        cur = state["cur"]
        if cur is not None and (state["started"] or cur.notes or cur.places):
            if cur.ctx == "":
                cur.ctx = None  # as in C, an empty context is no context
            entries.append(cur)
        state.update(cur=None, field=None, started=False)

    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip()
        if not line.strip():
            finish()
            continue
        obsolete = line.startswith("#~")
        if obsolete:
            line = line[2:].strip()
        cur = state["cur"]
        is_prev = line.startswith("|") if obsolete else line.startswith("#|")
        if line.startswith("#") or is_prev:
            if cur is not None and cur.msgstr:
                finish()
                cur = None
            if cur is None:
                cur = state["cur"] = PoEntry()
                cur.line = n
            body = line[1:]
            if is_prev:
                cur.prev.append(line[1:] if obsolete else line[2:])
            elif body.startswith(". ") or body == ".":
                cur.notes.append(body[2:])
            elif body.startswith(": "):
                cur.places += body[2:].split()
            elif body.startswith(","):
                for flag in (f.strip() for f in body[1:].split(",")):
                    if flag == "fuzzy":
                        cur.fuzzy = True
                    elif flag and flag not in cur.flags:
                        cur.flags.append(flag)
            elif body.startswith(" review:"):
                cur.review = body[len(" review:"):].strip()
            else:
                cur.comments.append(body[1:] if body.startswith(" ") else body)
            continue
        if line.startswith('"'):
            if cur is None or state["field"] is None:
                errors.append((path, n, "a string without a keyword before it"))
                continue
            value = _unquote(line, path, n, errors)
            kind, idx = state["field"]
            if kind == "ctx":
                cur.ctx += value
            elif kind == "id":
                cur.msgid += value
            elif kind == "plural":
                cur.plural += value
            else:
                cur.msgstr[idx] += value
            continue
        m = _KEYWORD.match(line)
        if not m:
            errors.append((path, n, "cannot read this line"))
            continue
        word = m.group(1)
        if word in ("msgctxt", "msgid") and cur is not None and cur.msgstr:
            finish()
            cur = None
        if cur is None:
            cur = state["cur"] = PoEntry()
            cur.line = n
        if obsolete:
            cur.obsolete = True
        state["started"] = True
        value = _unquote(m.group(3), path, n, errors)
        if word == "msgctxt":
            cur.ctx = value
            state["field"] = ("ctx", 0)
        elif word == "msgid":
            cur.msgid = value
            state["field"] = ("id", 0)
        elif word == "msgid_plural":
            cur.plural = value
            state["field"] = ("plural", 0)
        else:
            idx = int(m.group(2)) if m.group(2) is not None else 0
            while len(cur.msgstr) <= idx:
                cur.msgstr.append("")
            cur.msgstr[idx] = value
            state["field"] = ("str", idx)
    finish()
    header = {}
    keep = []
    for e in entries:
        if not header and e.msgid == "" and e.ctx is None and not e.obsolete and e.msgstr:
            for row in e.msgstr[0].split("\n"):
                k, sep, v = row.partition(":")
                if sep:
                    header[k.strip()] = v.strip()
            continue
        if not e.msgstr:
            e.msgstr = [""] * (2 if e.plural is not None else 1)
        keep.append(e)
    return header, keep


def read_po(root, rel):
    """(header, entries, errors) of a repo-relative .po/.pot file; None when it does not exist."""
    raw = _read_bytes(Path(root) / rel)
    if raw is None:
        return None
    errors = []
    header, entries = parse_po(raw.decode("utf-8", errors="replace"), rel, errors)
    return header, entries, errors


def po_locales(root):
    """Locales that have a .po file (pseudo-locales never count), sorted."""
    base = Path(root) / PO_DIR
    if not base.is_dir():
        return []
    return sorted(p.stem for p in base.glob("*.po") if p.stem not in PSEUDO_LOCALES)


def po_path(locale):
    return "%s/%s.po" % (PO_DIR, locale)


def template_entries(root):
    """The active entries of messages.pot, or None when there is no template."""
    tpl = read_po(root, POT_PATH)
    return None if tpl is None else [e for e in tpl[1] if not e.obsolete]


# ------------------------------------------------------------------ writing .po files

def po_header(locale):
    rule = PLURAL_RULES[plural_rule(locale)]
    return (HEADER + '"Language: %s\\n"\n' % locale
            + '"Plural-Forms: nplurals=%d; plural=%s;\\n"\n' % (len(rule[1]), rule[2]))


def render_po_entry(e):
    lines = [("# " + c) if c else "#" for c in e.comments]
    lines += ["#. " + n for n in e.notes] + ["#: " + p for p in e.places]
    if e.review:
        lines.append("# review: " + e.review)
    flags = (["fuzzy"] if e.fuzzy else []) + list(e.flags)
    if flags:
        lines.append("#, " + ", ".join(flags))
    lines += [("#~|" if e.obsolete else "#|") + p for p in e.prev]
    pre = "#~ " if e.obsolete else ""
    if e.ctx is not None:
        lines.append(pre + "msgctxt " + po_quote(e.ctx))
    lines.append(pre + "msgid " + po_quote(e.msgid))
    if e.plural is not None:
        lines.append(pre + "msgid_plural " + po_quote(e.plural))
        for i, s in enumerate(e.msgstr):
            lines.append(pre + "msgstr[%d] %s" % (i, po_quote(s)))
    else:
        lines.append(pre + "msgstr " + po_quote(e.msgstr[0] if e.msgstr else ""))
    return "\n".join(lines) + "\n"


def render_po(locale, entries):
    return "\n".join([po_header(locale)] + [render_po_entry(e) for e in entries])


# ------------------------------------------------------------------------------ merge

def _pad_forms(forms, count):
    return list(forms) + [""] * (count - len(forms))


def _new_entry(t, count):
    e = PoEntry()
    e.ctx, e.msgid, e.plural = t.ctx, t.msgid, t.plural
    e.notes, e.places = list(t.notes), list(t.places)
    e.msgstr = [""] * (count if t.plural is not None else 1)
    return e


def _same_kind(a, b):
    return (a.plural is None) == (b.plural is None)


def _carry(e, source):
    """Give e the translation of source (padded to e's number of forms)."""
    e.msgstr = _pad_forms(source.msgstr, len(e.msgstr)) if e.plural is not None else source.msgstr[:1]
    e.comments, e.flags, e.prev = list(source.comments), list(source.flags), list(source.prev)


def merge_entries(template, old, locale):
    """(merged entries, stats): template order first, then the obsolete ones."""
    count = len(PLURAL_RULES[plural_rule(locale)][1])
    old_active = {e.key: e for e in old if not e.obsolete}
    old_obsolete = {e.key: e for e in old if e.obsolete}
    tkeys = {t.key for t in template}
    gone = [e for e in old if not e.obsolete and e.key not in tkeys]
    consumed = set()
    stats = {"new": 0, "fuzzy": 0}
    out = []
    for t in template:
        e = _new_entry(t, count)
        prev = old_active.get(t.key) or old_obsolete.get(t.key)
        if prev is not None and _same_kind(prev, t):
            _carry(e, prev)
            e.review, e.fuzzy = prev.review, prev.fuzzy
            out.append(e)
            continue
        best, ratio = None, 0.0
        for g in gone:
            if id(g) in consumed or g.ctx != t.ctx or not _same_kind(g, t) or not any(g.msgstr):
                continue
            r = difflib.SequenceMatcher(None, g.msgid, t.msgid).ratio()
            if r >= 0.8 and r > ratio:
                best, ratio = g, r
        if best is not None:
            consumed.add(id(best))
            _carry(e, best)
            e.fuzzy = True
            e.review = "changed source"
            stats["fuzzy"] += 1
        else:
            stats["new"] += 1
        out.append(e)
    obsolete = [e for e in old if e.obsolete and e.key not in tkeys]
    obsolete += [g for g in gone if id(g) not in consumed]
    for g in obsolete:
        g.obsolete = True
    stats["obsolete"] = len(obsolete)
    return out + obsolete, stats


def cmd_merge(root, locales, new):
    root = Path(root)
    template = template_entries(root)
    if template is None:
        print("%s: no template; run tools/i18n.py extract" % POT_PATH)
        return 1
    if new:
        if not LOCALE_RE.match(new) or new in PSEUDO_LOCALES:
            print("merge: %s is not a language tag we can use (like es-US, fr-CA, zh-Hans)" % new)
            return 1
        if (root / po_path(new)).exists():
            print("%s exists; run merge without --new" % po_path(new))
            return 1
        locales = [new]
    elif not locales:
        locales = po_locales(root)
        if not locales:
            print("merge: no .po files (create one with: merge --new <locale>)")
            return 0
    status = 0
    for locale in locales:
        rel = po_path(locale)
        current = read_po(root, rel)
        if current is None and not new:
            print("%s does not exist; create it with: merge --new %s" % (rel, locale))
            status = 1
            continue
        old = current[1] if current else []
        for path, line, msg in (current[2] if current else []):
            print("%s:%d: %s" % (path, line, msg))
        merged, stats = merge_entries(template, old, locale)
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(render_po(locale, merged).encode("utf-8"))
        print("%s: %d entries (%d new, %d changed source, %d obsolete)"
              % (rel, len(template), stats["new"], stats["fuzzy"], stats["obsolete"]))
    return status


# ----------------------------------------------------------------------------- build

_REVIEWED = re.compile(r"^(?!draft\b)\S+ \d{4}-\d{2}-\d{2}$")


def is_reviewed(e):
    return not e.fuzzy and bool(e.review) and bool(_REVIEWED.match(e.review))


# English is the source. A variant of it (en-GB) holds only what it writes differently; an entry
# it leaves empty is the source's own text, so it ships when the entries it has are reviewed.
SOURCE_LANGUAGE = "en"


def is_source_variant(locale):
    return re.split(r"[-_]", locale, maxsplit=1)[0].lower() == SOURCE_LANGUAGE


# The words a variant spells differently from the source (whole words, any capitalization). An
# entry of the template that uses one needs an entry in that variant's file, even one that keeps
# the word (a name like "SIL Open Font License"): check_variant_words.
VARIANT_WORDS = {
    "en-GB": {
        "canceled": "cancelled", "canceling": "cancelling", "enroll": "enrol", "enrolls": "enrols",
        "color": "colour", "colors": "colours", "center": "centre", "centers": "centres",
        "gray": "grey", "favorite": "favourite", "favorites": "favourites",
        "behavior": "behaviour", "neighbor": "neighbour", "neighbors": "neighbours",
        "honor": "honour", "license": "licence", "meter": "metre", "meters": "metres",
        "liter": "litre", "fiber": "fibre", "defense": "defence", "catalog": "catalogue",
        "analyze": "analyse", "analyzed": "analysed", "organize": "organise",
        "organized": "organised", "recognize": "recognise", "recognized": "recognised",
        "authorize": "authorise", "authorized": "authorised", "customize": "customise",
        "customized": "customised", "synchronize": "synchronise", "synchronized": "synchronised",
        "synchronizing": "synchronising", "initialize": "initialise", "initialized": "initialised",
        "minimize": "minimise", "maximize": "maximise", "prioritize": "prioritise",
        "summarize": "summarise", "realize": "realise", "apologize": "apologise",
        "optimize": "optimise", "optimized": "optimised",
    },
}


class Catalog:
    def __init__(self, locale, preview, items, web=()):
        self.locale = locale
        self.rule = plural_rule(locale)
        self.preview = preview
        self.items = items  # (ctx, msgid, [forms], is_plural), translated entries only
        self.web = set(web)  # (ctx, msgid) of the entries the Panel Manager uses

    @property
    def symbol(self):
        return "i18n_catalog_" + self.locale.replace("-", "_")


def well_formed(e, template, locale):
    """None when the translated entry e may be built, else the reason it may not.
    `template` maps key -> template PoEntry."""
    t = template.get(e.key)
    if t is None:
        return "is not in the template"
    if not _same_kind(t, e):
        return "is %s here but %s in the template" % (
            "plain" if e.plural is None else "plural", "plain" if t.plural is None else "plural")
    if e.plural is not None:
        need = len(PLURAL_RULES[plural_rule(locale)][1])
        if len(e.msgstr) != need:
            return "has %d plural forms, %s needs %d" % (len(e.msgstr), locale, need)
    return None


def make_catalog(locale, entries, template_keys, force_preview=False, template=None, path=None):
    """A catalog from .po entries. preview stays 1 unless every template key is translated,
    not fuzzy and reviewed (I18N-005), and there is a template at all. With `template` (key ->
    PoEntry) an entry of the wrong kind or form count, or one the template lacks, is left out
    with a warning naming path:line."""
    have = {e.key: e for e in entries if not e.obsolete}
    items = []
    web = set()
    for e in entries:
        if e.obsolete or not e.translated():
            continue
        why = well_formed(e, template, locale) if template is not None else None
        if why:
            print("%s:%d: warning: %s %s; left out of the build" % (path or po_path(locale), e.line,
                                                                    _show_po(e), why))
            del have[e.key]
            continue
        items.append((e.ctx, e.msgid, list(e.msgstr), e.plural is not None))
        source = template.get(e.key) if template is not None else None
        if used_by_web((source or e).places):
            web.add(e.key)
    if is_source_variant(locale):
        # a variant ships when every entry it has is reviewed; the others are the source's text
        mine = [e for e in have.values() if e.translated()]
        complete = bool(template_keys) and bool(mine) and all(is_reviewed(e) for e in mine)
    else:
        complete = bool(template_keys) and all(
            k in have and have[k].translated() and is_reviewed(have[k]) for k in template_keys)
    return Catalog(locale, 1 if (force_preview or not complete) else 0, items, web)


def c_string(data):
    """A C literal for bytes: octal for anything not plain ASCII (never swallows a digit after it)."""
    out = []
    for b in data:
        ch = chr(b)
        if ch in '\\"?':
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif b < 0x20 or b >= 0x7F:
            out.append("\\%03o" % b)
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


ENTRY_FLAGS = (("I18N_ENTRY_PLURAL", 0x40000000), ("I18N_ENTRY_WEB", 0x80000000))


def catalog_layout(cat):
    """[(hash, text offset, [form bytes], [flag names])] sorted by hash; the string block holds the
    forms in entry order. Raises ValueError on a hash collision."""
    rows = []
    seen = {}
    offset = 0
    for ctx, msgid, forms, is_plural in cat.items:
        h = key_hash(ctx, msgid)
        if h in seen:
            raise ValueError("%s: hash collision 0x%08x between %r and %r; give one a context"
                             % (cat.locale, h, seen[h], msgid))
        seen[h] = msgid
        data = [f.encode("utf-8") + b"\0" for f in forms]
        flags = (["I18N_ENTRY_PLURAL"] if is_plural else []) + (
            ["I18N_ENTRY_WEB"] if (ctx, msgid) in cat.web else [])
        rows.append((h, offset, data, flags))
        offset += sum(len(d) for d in data)
    if offset > 0x3FFFFFFF:
        raise ValueError("%s: the string block is too big for I18N_ENTRY_OFFSET" % cat.locale)
    return sorted(rows, key=lambda r: r[0])


def render_catalog_c(cat, source):
    rows = catalog_layout(cat)
    lines = ["/* Generated by `tools/i18n.py build` - do not edit. Source: %s */" % source,
             '#include "i18n/i18n.h"', "",
             "/* The texts are one string literal, longer than the 4095 characters ISO C promises; GCC",
             " * and Clang take any length, and -Wpedantic would say so (the host build). */",
             '#pragma GCC diagnostic ignored "-Woverlength-strings"', "",
             "extern const i18n_catalog_t %s;" % cat.symbol, "",
             "static const i18n_entry_t entries[] = {"]
    if rows:
        lines += ["    {0x%08xu, %du%s}," % (h, off, "".join(" | " + f for f in flags))
                  for h, off, _, flags in rows]
    else:
        lines.append("    {0u, 0u}, /* nothing translated yet; count is 0 */")
    lines += ["};", "", "static const char strings[] ="]
    if rows:
        for _, _, data, _ in sorted(rows, key=lambda r: r[1]):
            lines.append("    " + " ".join(c_string(d) for d in data))
    else:
        lines.append('    ""')
    lines += ["    ;", "", "const i18n_catalog_t %s = {" % cat.symbol,
              '    "%s", %s, %d, %du, entries, strings,' % (cat.locale, PLURAL_RULES[cat.rule][0],
                                                            cat.preview, len(rows)),
              "};", ""]
    return "\n".join(lines)


def render_catalogs_list(cats):
    """The table of built-in catalogs. A draft (preview) catalog sits inside #ifndef I18N_NO_DRAFTS,
    which a release build defines (cmake/alarm_firmware.cmake), so a released panel never offers a
    language nobody reviewed (I18N-005); the table ends in NULL, so it is never empty."""
    if not cats:
        return ("/* Generated by `tools/i18n.py build` - do not edit. A build with I18N_NO_BUILTIN has no\n"
                " * built-in language (docs/features/37 language packs). */\n"
                '#include "i18n_builtin.h"\n\n'
                "#ifdef I18N_NO_BUILTIN\n"
                "const i18n_catalog_t *const i18n_builtin[] = {NULL};\n"
                "const size_t i18n_builtin_count = 0;\n"
                "#else\n"
                "const i18n_catalog_t *const i18n_builtin[] = {NULL};\n"
                "const size_t i18n_builtin_count = 0;\n"
                "#endif\n")
    lines = ["/* Generated by `tools/i18n.py build` - do not edit. A draft catalog is left out when",
             " * I18N_NO_DRAFTS is defined (a release build, docs/features/37 I18N-005); with",
             " * I18N_NO_BUILTIN the table is empty (a build whose languages are all packs). */",
             '#include "i18n_builtin.h"', ""]
    lines += ["extern const i18n_catalog_t %s;" % c.symbol for c in cats]
    lines += ["", "#ifdef I18N_NO_BUILTIN",
              "const i18n_catalog_t *const i18n_builtin[] = {NULL};",
              "const size_t i18n_builtin_count = 0;",
              "#else", "const i18n_catalog_t *const i18n_builtin[] = {"]
    for c in cats:
        if c.preview:
            lines += ["#ifndef I18N_NO_DRAFTS", "    &%s, /* draft */" % c.symbol, "#endif"]
        else:
            lines.append("    &%s," % c.symbol)
    lines += ["    NULL,", "};",
              "const size_t i18n_builtin_count = sizeof i18n_builtin / sizeof i18n_builtin[0] - 1u;",
              "#endif", ""]
    return "\n".join(lines)


def render_web_json(cat):
    """The catalog's Panel Manager entries as the panel sends them (i18n_web_json): keyed by the
    hash in 8 hex digits, plural forms as a list. Only the mock serves a file like this (the test
    languages); a panel builds it from its tables, so the words take no second copy (I18N-031)."""
    entries = {}
    for ctx, msgid, forms, is_plural in sorted(cat.items, key=lambda i: key_hash(i[0], i[1])):
        if (ctx, msgid) in cat.web:
            entries["%08x" % key_hash(ctx, msgid)] = forms if is_plural else forms[0]
    doc = {"locale": cat.locale, "plural": list(PLURAL_RULES[cat.rule][1]), "entries": entries}
    return json.dumps(doc, ensure_ascii=False, separators=(",", ":")) + "\n"


def character_list(cats):
    chars = set()
    for cat in cats:
        for _, _, forms, _ in cat.items:
            for f in forms:
                chars.update(ch for ch in f if ord(ch) >= 0x20 and ord(ch) != 0x7F)
    return "".join(sorted(chars)) + "\n"


CATALOG_DIR = "components/core/i18n/catalogs"
CHARS_PATH = CATALOG_DIR + "/i18n_chars.txt"
LIST_PATH = CATALOG_DIR + "/i18n_catalogs_gen.c"
TEMPLATE_C_PATH = CATALOG_DIR + "/i18n_template_gen.c"


def catalog_c_path(locale):
    return "%s/i18n_%s_gen.c" % (CATALOG_DIR, locale.replace("-", "_"))


def load_catalogs(root, problems):
    """The catalogs of every .po file, sorted by locale."""
    locales = po_locales(root)
    seen = {}
    for locale in locales:
        rel = po_path(locale)
        if not LOCALE_RE.match(locale):
            raise ValueError("%s: %s is not a language tag we can build (like es-US, fr-CA, zh-Hans)"
                             % (rel, locale))
        symbol = locale.replace("-", "_")
        if symbol in seen:
            raise ValueError("%s: names the same catalog as %s" % (rel, seen[symbol]))
        seen[symbol] = rel
    template = template_entries(root) or []
    if locales and not template:
        raise ValueError("%s is missing or empty, so the .po files cannot be built; "
                         "run tools/i18n.py extract" % POT_PATH)
    tmap = {t.key: t for t in template}
    cats = []
    for locale in locales:
        _, entries, errors = read_po(root, po_path(locale))
        problems += errors
        cats.append(make_catalog(locale, entries, list(tmap), template=tmap, path=po_path(locale)))
    return cats


def expected_outputs(root, problems):
    """{repo-relative path: bytes} of every generated file for the .po files in the tree."""
    cats = load_catalogs(root, problems)
    out = {LIST_PATH: render_catalogs_list(cats).encode("utf-8"),
           TEMPLATE_C_PATH: render_template_c(root).encode("utf-8")}
    for cat in cats:
        out[catalog_c_path(cat.locale)] = render_catalog_c(cat, po_path(cat.locale)).encode("utf-8")
    if cats:
        out[CHARS_PATH] = character_list(cats).encode("utf-8")
    return out


def stale_outputs(root, want):
    """Generated files on disk that no .po file asks for."""
    found = []
    base = Path(root)
    for pattern in (CATALOG_DIR + "/i18n_*_gen.c", CHARS_PATH):
        for p in sorted(base.glob(pattern)):
            rel = p.relative_to(base).as_posix()
            if rel not in want and rel != TEMPLATE_C_PATH:
                found.append(rel)
    return found


def cmd_build(root, check_only):
    root = Path(root)
    problems = []
    try:
        want = expected_outputs(root, problems)
    except ValueError as exc:
        print("build: %s" % exc)
        return 1
    for path, line, msg in problems:
        print("%s:%d: %s" % (path, line, msg))
    if problems:
        return 1
    stale = stale_outputs(root, want)
    changed = [rel for rel, data in sorted(want.items()) if _read_bytes(root / rel) != data]
    if check_only:
        for rel in changed:
            print("%s: would change; run tools/i18n.py build" % rel)
        for rel in stale:
            print("%s: no .po file asks for it; run tools/i18n.py build" % rel)
        if changed or stale:
            return 1
        print("generated catalogs are up to date (%d files)" % len(want))
        return 0
    for rel in changed:
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(want[rel])
    for rel in stale:
        (root / rel).unlink()
    print("build: %d files written, %d removed" % (len(changed), len(stale)))
    return 0

# ---------------------------------------------------------------------------- language packs
# docs/features/37 language-packs.md "The pack file (format 1)"

PACK_MAGIC = b"RLNG"
PACK_FORMAT = 1
PACK_HEADER_SIZE = 128
PACK_SECTOR = 4096
PACK_LOCALE_RE = re.compile(r"^[A-Za-z0-9-]{2,15}$")


def template_hashes(root):
    """The key hash of every template entry (an empty list without a template)."""
    return [key_hash(e.ctx, e.msgid) for e in (template_entries(root) or [])]


def template_fingerprint(hashes):
    """32-bit FNV-1a over the key hashes, sorted ascending, each as 4 bytes little-endian."""
    return fnv1a32(b"".join(struct.pack("<I", h) for h in sorted(hashes)))


def render_template_c(root):
    """The firmware's own template fingerprint and size (catalogs/i18n_template_gen.c)."""
    hashes = template_hashes(root)
    return ("/* Generated by `tools/i18n.py build` - do not edit. Source: %s */\n"
            '#include "i18n/i18n_pack.h"\n\n'
            "const uint32_t i18n_template_fingerprint = 0x%08xu;\n"
            "const uint32_t i18n_template_count = %du;\n"
            % (POT_PATH, template_fingerprint(hashes), len(hashes)))


def pack_name(cat):
    """The language's name in itself: the catalog's translation of ("this language", "English")."""
    for ctx, msgid, forms, _ in cat.items:
        if ctx == "this language" and msgid == "English":
            name = forms[0]
            break
    else:
        raise ValueError("%s: no translation of the language's own name (context 'this language', "
                         "'English')" % cat.locale)
    size = len(name.encode("utf-8"))
    if not 1 <= size <= 31:
        raise ValueError("%s: the language's name %r is %d bytes; a pack holds 1-31"
                         % (cat.locale, name, size))
    return name


PACK_FONT_ROLES = ("caption", "body", "label", "title", "headline")


def pack_fonts_section(fonts, start):
    """The fonts section (spec "The pack file"): count, then 24-byte records {role[16], offset
    from the payload start, size}, then each font at a 4-aligned offset. start is where the
    section lands in the payload (4-aligned); fonts a list of (role, bytes)."""
    if not 1 <= len(fonts) <= 8:
        raise ValueError("a pack holds 1-8 fonts, not %d" % len(fonts))
    if start % 4:
        raise ValueError("the fonts section must start 4-aligned")
    seen = set()
    for role, data in fonts:
        if role not in PACK_FONT_ROLES:
            raise ValueError("unknown font role %r (one of %s)" % (role, ", ".join(PACK_FONT_ROLES)))
        if role in seen:
            raise ValueError("font role %r given twice" % role)
        if not data:
            raise ValueError("font %r is empty" % role)
        seen.add(role)
    pos = start + 4 + 24 * len(fonts)
    table = struct.pack("<I", len(fonts))
    body = b""
    for role, data in fonts:
        pos_a = (pos + 3) & ~3
        body += b"\0" * (pos_a - pos) + data
        table += role.encode("ascii").ljust(16, b"\0") + struct.pack("<II", pos_a, len(data))
        pos = pos_a + len(data)
    return table + body


def build_pack_payload(locale, name, rule, draft, rows, template_keys, version, fonts=b"",
                       source_variant=None):
    """The unsigned payload of a pack. rows is catalog_layout() (hash, text offset, [form bytes],
    [flag names]) sorted by hash; rule the PLURAL_RULES key; template_keys the template's key
    hashes; fonts (optional) the fonts section placed after the strings: either its bytes, or a
    list of (role, LVGL binary font bytes) that pack_fonts_section() lays out."""
    if not PACK_LOCALE_RE.match(locale):
        raise ValueError("%r is not a pack locale (2-15 letters, digits and '-')" % locale)
    raw_name = name.encode("utf-8")
    if not 1 <= len(raw_name) <= 31:
        raise ValueError("the name %r is %d bytes; a pack holds 1-31" % (name, len(raw_name)))
    if not 1 <= len(version) <= 15 or any(not 0x20 <= ord(c) <= 0x7E for c in version):
        raise ValueError("version %r must be 1-15 printable ASCII characters" % version)
    if source_variant is None:
        source_variant = is_source_variant(locale)
    flag_bits = dict(ENTRY_FLAGS)
    entries = b"".join(struct.pack("<II", h, off | sum(flag_bits[f] for f in flags))
                       for h, off, _, flags in rows)
    strings = b"".join(d for _, _, data, _ in sorted(rows, key=lambda r: r[1]) for d in data)
    have = {r[0] for r in rows}
    missing = 0 if source_variant else sum(1 for k in set(template_keys) if k not in have)
    entries_off = PACK_HEADER_SIZE
    strings_off = entries_off + len(entries)
    length = strings_off + len(strings)
    fonts_off = fonts_size = 0
    tail = b""
    if fonts and not isinstance(fonts, (bytes, bytearray)):
        fonts_off = (length + 3) & ~3
        fonts = pack_fonts_section(fonts, fonts_off)
    if fonts:
        fonts_off = (length + 3) & ~3
        tail = b"\0" * (fonts_off - length) + fonts
        fonts_size = len(fonts)
        length = fonts_off + fonts_size
    header = (PACK_MAGIC + struct.pack("<HHI", PACK_FORMAT, PACK_HEADER_SIZE, length)
              + locale.encode("ascii").ljust(16, b"\0") + raw_name.ljust(32, b"\0")
              + struct.pack("<BBH", list(PLURAL_RULES).index(rule), 1 if draft else 0, 0)
              + struct.pack("<IIII", len(rows), entries_off, strings_off, len(strings))
              + struct.pack("<II", template_fingerprint(template_keys), missing)
              + version.encode("ascii").ljust(16, b"\0")
              + struct.pack("<II", fonts_off, fonts_size) + b"\0" * 16)
    assert len(header) == PACK_HEADER_SIZE
    return header + entries + strings + tail


def pack_payload_for(cat, name, template_keys, version, fonts=None):
    return build_pack_payload(cat.locale, name, cat.rule, bool(cat.preview), catalog_layout(cat),
                              template_keys, version, fonts or b"")


def unsigned_pack_file(payload):
    """What espsecure sign_data would write, with an all-0xFF signature sector."""
    padded = payload + b"\xff" * (-len(payload) % PACK_SECTOR)
    return padded + b"\xff" * PACK_SECTOR


def sign_pack(payload, key, out):
    """Write the signed pack file with espsecure; raises RuntimeError with the reason."""
    try:
        import espsecure  # noqa: F401
    except ImportError:
        raise RuntimeError("espsecure is not importable by %s; install esptool or run this with "
                           "the ESP-IDF Python (needed to sign with --key)" % sys.executable)
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "payload.bin"
        src.write_bytes(payload)
        cmd = [sys.executable, "-m", "espsecure", "sign_data", "--version", "2",
               "--keyfile", str(key), "--output", str(out), str(src)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError("espsecure sign_data failed: %s" % (res.stderr or res.stdout).strip())


def parse_font_args(specs):
    """--font role=path, repeatable, into [(role, bytes)]; raises ValueError."""
    fonts = []
    for spec in specs or []:
        role, eq, path = spec.partition("=")
        if not eq or not path:
            raise ValueError("--font wants role=path, not %r" % spec)
        if role not in PACK_FONT_ROLES:
            raise ValueError("unknown font role %r (one of %s)" % (role, ", ".join(PACK_FONT_ROLES)))
        if any(r == role for r, _ in fonts):
            raise ValueError("font role %r given twice" % role)
        try:
            fonts.append((role, Path(path).read_bytes()))
        except OSError as exc:
            raise ValueError("cannot read the font %s: %s" % (path, exc))
    return fonts


def cmd_pack(root, version, out, key, release, locales, font_specs=None):
    root = Path(root)
    try:
        fonts = parse_font_args(font_specs)
    except ValueError as exc:
        print("pack: %s" % exc)
        return 1
    problems = []
    try:
        cats = load_catalogs(root, problems)
    except ValueError as exc:
        print("pack: %s" % exc)
        return 1
    for path, line, msg in problems:
        print("%s:%d: %s" % (path, line, msg))
    if problems:
        return 1
    known = {c.locale: c for c in cats}
    for loc in locales or []:
        if loc not in known:
            print("pack: no .po file for %s (have: %s)" % (loc, ", ".join(known) or "none"))
            return 1
    hashes = template_hashes(root)
    outdir = Path(out)
    outdir.mkdir(parents=True, exist_ok=True)
    if key is None:
        print("pack: warning: no --key, the packs are unsigned; a panel refuses an unsigned pack")
    written = 0
    for cat in cats:
        if locales and cat.locale not in locales:
            continue
        if release and cat.preview:
            print("pack: %s is a draft, left out of a release" % cat.locale)
            continue
        try:
            payload = pack_payload_for(cat, pack_name(cat), hashes, version, fonts)
        except ValueError as exc:
            print("pack: %s" % exc)
            return 1
        target = outdir / (cat.locale + ".robolang")
        if key is None:
            target.write_bytes(unsigned_pack_file(payload))
        else:
            try:
                sign_pack(payload, key, target)
            except RuntimeError as exc:
                print("pack: %s" % exc)
                return 1
        print("pack: wrote %s (%d bytes of payload)" % (target, len(payload)))
        written += 1
    print("pack: %d files written" % written)
    return 0


# The frozen fixture pack (components/core/i18n/test/pack_fixture_v1.h): made from this small
# hard-coded input, never from the .po files, so a change to the format shows as a difference.
FIXTURE_PATH = "components/core/i18n/test/pack_fixture_v1.h"
FIXTURE_VERSION = "0.7.0"
FIXTURE_NAME = "Français"
FIXTURE_ITEMS = [  # (ctx, English, [forms], is_plural)
    (None, "Armed", ["Armé"], False),
    ("door", "Open", ["Ouvert"], False),
    (None, "{0} zone", ["{0} zone", "{0} de zones", "{0} zones"], True),
    (None, "Settings", ["Réglages"], False),
    (None, "Thumbs up", ["Pouce \U0001F44D"], False),
]
FIXTURE_WEB = {(None, "Settings")}
FIXTURE_EXTRA_TEMPLATE = [(None, "Disarmed"), (None, "Entry delay")]


def fixture_catalog():
    return Catalog("fr-CA", 0, FIXTURE_ITEMS, FIXTURE_WEB)


def fixture_payload():
    cat = fixture_catalog()
    keys = [key_hash(c, m) for c, m, _, _ in FIXTURE_ITEMS]
    keys += [key_hash(c, m) for c, m in FIXTURE_EXTRA_TEMPLATE]
    return build_pack_payload("fr-CA", FIXTURE_NAME, cat.rule, False, catalog_layout(cat), keys,
                              FIXTURE_VERSION), keys


def _ascii(text):
    """A text in double quotes for a comment: ASCII only, other characters as U+XXXX; None as -."""
    if text is None:
        return "-"
    return '"' + "".join(c if 0x20 <= ord(c) < 0x7F else "U+%04X" % ord(c) for c in text) + '"'


def render_pack_fixture_header():
    payload, keys = fixture_payload()
    lines = ["/* Frozen fixture: a language pack (format 1) made independently of the firmware, from a",
             " * small hard-coded input by tools/i18n.py (fixture_payload). NEVER EDIT THIS FILE: when the",
             " * format changes, add pack_fixture_v2.h next to it. The payload is unpadded and unsigned.",
             " *",
             " * locale fr-CA, name %s, plural rule I18N_PLURALS_FR, not a draft, version %s,"
             % (_ascii(FIXTURE_NAME), FIXTURE_VERSION),
             " * template fingerprint 0x%08x over %d keys, missing 2 (Disarmed, Entry delay)."
             % (template_fingerprint(keys), len(keys)),
             " *",
             " * ctx | English | translation (forms separated by ' / ') | hash | flags"]
    for ctx, msgid, forms, is_plural in FIXTURE_ITEMS:
        flags = (["I18N_ENTRY_PLURAL"] if is_plural else []) + (
            ["I18N_ENTRY_WEB"] if (ctx, msgid) in FIXTURE_WEB else [])
        lines.append(" *   %s | %s | %s | 0x%08x | %s" % (
            _ascii(ctx), _ascii(msgid), " / ".join(_ascii(f) for f in forms),
            key_hash(ctx, msgid), " | ".join(flags) or "-"))
    lines += [" */", "#include <stdint.h>", "",
              "static const uint8_t pack_fixture_v1[] = {"]
    for i in range(0, len(payload), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in payload[i:i + 12]) + ",")
    lines += ["};", ""]
    return "\n".join(lines)


# ---------------------------------------------------------------------------- pseudo

_ACCENTS = dict(zip("AaCcDdEeGgHhIiJjKkLlNnOoRrSsTtUuWwYyZz",
                    "ÅáÇçÐðÉéĜĝĤĥÍíĴĵĶķĹĺÑñÓóŔŕŠšŢţÚúŴŵÝýŽž"))
_TOKEN = re.compile(r"(\{\{|\}\}|\{\d\})")
RLO, PDF = "\u202e", "\u202c"


def _fits(s, budget):
    return budget is None or _bare_bytes(s) <= budget


def _pseudo_en(s, budget=None):
    """Accented look-alikes, about 40 % longer, in brackets so a cut shows. Within the entry's
    byte budget, like any translation (I18N-023): less padding, then no accents, then English."""
    parts = _TOKEN.split(s)
    text_len = sum(len(p) for i, p in enumerate(parts) if i % 2 == 0)
    if text_len == 0:
        return s
    body = "".join(p if i % 2 else "".join(_ACCENTS.get(c, c) for c in p)
                   for i, p in enumerate(parts))
    for pad in range(-(-text_len * 2 // 5), -1, -1):
        out = "[" + body + "·" * pad + "]"
        if _fits(out, budget):
            return out
    return "[" + s + "]" if _fits("[" + s + "]", budget) else s


def _pseudo_ar(s, budget=None):
    """Each run of text between placeholders forced right to left."""
    out = "".join(p if i % 2 or not p else RLO + p + PDF for i, p in enumerate(_TOKEN.split(s)))
    return out if _fits(out, budget) else s


# Entries that are CLDR date and time patterns ("h:mm a"), not words: a test language keeps
# them as they are - its letters would be read as pattern letters or print as themselves.
PATTERN_CONTEXTS = ("date pattern",)


def pseudo_entries(template, locale, fn):
    cats = PLURAL_RULES[plural_rule(locale)][1]
    out = []
    for t in template:
        e = _new_entry(t, len(cats))
        budget = _budget(t)
        if t.ctx in PATTERN_CONTEXTS:
            e.msgstr = [t.msgid] * (1 if t.plural is None else len(cats))
        elif t.plural is None:
            e.msgstr = [fn(t.msgid, budget)]
        else:
            e.msgstr = [fn(t.msgid if c == "one" else t.plural, budget) for c in cats]
        out.append(e)
    return out


def cmd_pseudo(root, build):
    root = Path(root)
    template = template_entries(root)
    if template is None:
        print("%s: no template; run tools/i18n.py extract" % POT_PATH)
        return 1
    outdir = root / "build/i18n/pseudo"
    outdir.mkdir(parents=True, exist_ok=True)
    cats = []
    for locale, fn in (("en-XA", _pseudo_en), ("ar-XB", _pseudo_ar)):
        entries = pseudo_entries(template, locale, fn)
        (outdir / (locale + ".po")).write_bytes(render_po(locale, entries).encode("utf-8"))
        print("wrote build/i18n/pseudo/%s.po (%d entries)" % (locale, len(entries)))
        cats.append(make_catalog(locale, entries, [], force_preview=True))
    if build:
        (outdir / "i18n_catalogs_gen.c").write_bytes(render_catalogs_list(cats).encode("utf-8"))
        for cat in cats:
            name = "i18n_%s_gen.c" % cat.locale.replace("-", "_")
            (outdir / name).write_bytes(render_catalog_c(cat, "the template").encode("utf-8"))
            (outdir / (cat.locale + ".json")).write_bytes(render_web_json(cat).encode("utf-8"))
            print("wrote build/i18n/pseudo/%s and %s.json" % (name, cat.locale))
    return 0


def cmd_web(root, out):
    """The committed catalogs' Panel Manager entries as a panel sends them (/i18n/<locale>.json),
    for the mock (MOCK_I18N=build/i18n/web): a language tried in the Panel Manager on the PC.
    Never committed: a panel builds the same JSON from its tables (I18N-031)."""
    root = Path(root)
    problems = []
    try:
        cats = load_catalogs(root, problems)
    except ValueError as exc:
        print("web: %s" % exc)
        return 1
    for path, line, msg in problems:
        print("%s:%d: %s" % (path, line, msg))
    if problems:
        return 1
    outdir = root / out
    outdir.mkdir(parents=True, exist_ok=True)
    for cat in cats:
        (outdir / (cat.locale + ".json")).write_bytes(render_web_json(cat).encode("utf-8"))
        print("wrote %s/%s.json" % (Path(out).as_posix(), cat.locale))
    return 0


# ------------------------------------------------------------------ the inventory of words

INVENTORY_PATH = "build/i18n/inventory.md"
INVENTORY_SURFACES = [
    ("Touchscreen", "components/ui/"),
    ("Panel Manager", "components/services/panel_web/web/"),
    ("Notifications", "components/core/notify/"),
]
FLASHER_WEB = "tools/flasher/roboflash/web"


def inventory_surface(place):
    for name, prefix in INVENTORY_SURFACES:
        if place.startswith(prefix):
            return name
    return "Other"


def inventory_line(e):
    text = e.msgid if e.plural is None else "%s / %s" % (e.msgid, e.plural)
    text = text.replace("\n", "\\n")
    parts = ["- %s" % json.dumps(text, ensure_ascii=False)]
    if e.ctx:
        parts.append("context %s" % json.dumps(e.ctx, ensure_ascii=False))
    budget = _budget(e)
    if budget is not None:
        parts.append("max %d bytes" % budget)
    if e.places:
        parts.append(e.places[0])
    return " | ".join(parts)


def flasher_words(root):
    """{file: [(line, text)]}: the Flasher's own words, which are not in the catalog yet. Best
    effort with the unmarked-text scanners: the visible text of its page (index.html) and the
    word-like string and template literals of its scripts, minus class names, keys and selectors."""
    out = {}
    base = Path(root) / FLASHER_WEB
    if not base.is_dir():
        return out
    for path in sorted(base.iterdir()):
        rel = "%s/%s" % (FLASHER_WEB, path.name)
        text = path.read_bytes().decode("utf-8", "replace")
        if path.suffix == ".js":
            found = unmarked_js(text, rel)
        elif path.suffix == ".html":
            found = unmarked_html(text, rel)
        else:
            continue
        seen = []
        for _, line, word in found:
            if (line, word) not in seen:
                seen.append((line, word))
        if seen:
            out[rel] = seen
    return out


def render_inventory(entries, flasher):
    groups = {}
    for e in entries:
        place = e.places[0] if e.places else ""
        groups.setdefault(inventory_surface(place), {}).setdefault(place.split(":")[0], []).append(e)
    names = [s[0] for s in INVENTORY_SURFACES] + ["Other"]
    lines = ["# Inventory of the words", "",
             "Generated by `python tools/i18n.py inventory` from the template; do not edit.", ""]
    counts = ["%s %d" % (n, sum(len(v) for v in groups.get(n, {}).values())) for n in names]
    counts.append("Flasher %d" % sum(len(v) for v in flasher.values()))
    lines += ["Entries: " + ", ".join(counts) + ".", ""]
    for name in names:
        files = groups.get(name)
        if not files:
            continue
        lines += ["## " + name, ""]
        for path in sorted(files):
            lines += ["### " + (path or "(no source)"), ""]
            lines += [inventory_line(e) for e in files[path]]
            lines.append("")
    lines += ["## Flasher", "",
              "Not in the catalog yet (BL-047). Best effort: the visible text of the page and the",
              "word-like literals of the scripts in `%s`; a string built from pieces may be missed." % FLASHER_WEB, ""]
    for path in sorted(flasher):
        lines += ["### " + path, ""]
        lines += ["- %s | line %d" % (json.dumps(w, ensure_ascii=False), n) for n, w in flasher[path]]
        lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def cmd_inventory(root):
    """Every word of every screen, grouped by surface and file, into build/i18n/inventory.md
    (docs/features/38 wording rules). Always rewrites; never committed."""
    entries = template_entries(root)
    if entries is None:
        print("inventory: there is no %s; run extract first" % POT_PATH)
        return 1
    text = render_inventory(entries, flasher_words(root))
    out = Path(root) / INVENTORY_PATH
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(text.encode("utf-8"))
    print("wrote %s (%d entries)" % (INVENTORY_PATH, len(entries)))
    return 0


# -------------------------------------------------------------- checks: the translations

def _budget(entry):
    budget = None
    for note in entry.notes:
        m = _BUDGET.search(note)
        if m:
            budget = int(m.group(1)) if budget is None else min(budget, int(m.group(1)))
    return budget


def _bare_bytes(s):
    bare = _PLACEHOLDER.sub(lambda m: "{" if m.group() == "{{" else "}" if m.group() == "}}" else "", s)
    return len(bare.encode("utf-8"))


def _set_text(s):
    return "{" + "}, {".join(sorted(s)) + "}" if s else "no placeholder"


def _show_po(e):
    return '"%s"' % e.msgid if e.ctx is None else '"%s" (context "%s")' % (e.msgid, e.ctx)


def check_po_file(root, locale, template):
    """Translation-side errors of one .po file. `template` maps key -> PoEntry, None: no template."""
    rel = po_path(locale)
    header, entries, errors = read_po(root, rel)
    errors = list(errors)
    cats = PLURAL_RULES[plural_rule(locale)][1]
    m = re.search(r"nplurals\s*=\s*(\d+)", header.get("Plural-Forms", ""))
    if not m or int(m.group(1)) != len(cats):
        errors.append((rel, 1, "the header's Plural-Forms must say nplurals=%d for %s; run merge"
                       % (len(cats), locale)))
    seen = {}
    for e in entries:
        if e.obsolete:
            continue
        if e.key in seen:
            errors.append((rel, e.line, "%s is also at line %d" % (_show_po(e), seen[e.key])))
            continue
        seen[e.key] = e.line
        if not any(e.msgstr):
            continue
        t = template.get(e.key) if template is not None else e
        if t is None:
            errors.append((rel, e.line, "%s is not in the template; run merge" % _show_po(e)))
            continue
        if not _same_kind(t, e):
            errors.append((rel, e.line, "%s is %s here but %s in the template; run merge"
                           % (_show_po(e), "plain" if e.plural is None else "plural",
                              "plain" if t.plural is None else "plural")))
            continue
        if e.plural is not None and len(e.msgstr) != len(cats):
            errors.append((rel, e.line, "%s has %d plural forms, %s needs %d"
                           % (_show_po(e), len(e.msgstr), locale, len(cats))))
        budget = _budget(t)
        for i, s in enumerate(e.msgstr):
            if s == "":
                continue
            got, bad = placeholders(s)
            if bad:
                errors.append((rel, e.line, "%s: only {0}..{9}, {{ and }} may use a brace"
                               % _show_po(e)))
            if e.plural is None:
                want, exact = placeholders(t.msgid)[0], True
            else:
                want, exact = placeholders(t.plural)[0], i < len(cats) and cats[i] == "other"
            if (got != want) if exact else not got <= want:
                errors.append((rel, e.line, "%s uses %s but the source has %s"
                               % (_show_po(e), _set_text(got), _set_text(want))))
            pm = _PRINTF.search(s)
            if pm:
                errors.append((rel, e.line, 'printf conversion "%s" in %s; use {0}'
                               % (pm.group(), _show_po(e))))
            mm = _MARKUP.search(s)
            if mm:
                errors.append((rel, e.line, 'markup "%s" in %s; markup is not translatable text'
                               % (mm.group(), _show_po(e))))
            if budget is not None and _bare_bytes(s) > budget:
                errors.append((rel, e.line, '%s: "%s" is %d bytes, the note allows %d'
                               % (_show_po(e), s, _bare_bytes(s), budget)))
    return errors


def check_variant_words(root, locales, tpl):
    """A variant of the source (en-GB) has an entry for every template entry that uses a word it
    spells differently, so a new "canceled" in the source can't reach a British screen."""
    errors = []
    for locale in locales:
        words = VARIANT_WORDS.get(locale)
        if not words or tpl is None:
            continue
        rel = po_path(locale)
        have = {e.key: e for e in read_po(root, rel)[1] if not e.obsolete and e.translated()}
        pattern = re.compile(r"\b(%s)\b" % "|".join(sorted(words, key=len, reverse=True)), re.I)
        for t in tpl:
            m = pattern.search(t.msgid + " " + (t.plural or ""))
            if m and t.key not in have:
                errors.append((rel, 1, '%s writes "%s", which %s spells "%s": give it an entry'
                               % (_show_po(t), m.group(1), locale, words[m.group(1).lower()])))
    return errors


def check_translations(root):
    locales = po_locales(root)
    if not locales:
        return []
    tpl = template_entries(root)
    template = None if tpl is None else {e.key: e for e in tpl}
    errors = []
    for locale in locales:
        errors += check_po_file(root, locale, template)
    return errors + check_variant_words(root, locales, tpl)


def check_release(root, locale):
    """Problems that keep a language out of a release: incomplete, fuzzy or not reviewed."""
    rel = po_path(locale)
    current = read_po(root, rel)
    if current is None:
        return [(rel, 1, "no such language file")]
    have = {e.key: e for e in current[1] if not e.obsolete}
    errors = []
    variant = is_source_variant(locale)
    for t in template_entries(root) or []:
        e = have.get(t.key)
        if variant and (e is None or not any(e.msgstr)):
            continue  # the source's own text
        if e is None or not e.translated():
            errors.append((rel, e.line if e else 1, "%s is not translated" % _show_po(t)))
        elif e.fuzzy:
            errors.append((rel, e.line, "%s is still fuzzy" % _show_po(e)))
        elif not is_reviewed(e):
            errors.append((rel, e.line, "%s has no review (# review: <handle> <date>)" % _show_po(e)))
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("extract", help="write the template from the sources")
    p.add_argument("--check", action="store_true", help="fail when the file would change")
    p = sub.add_parser("check", help="source-side checks")
    p.add_argument("--unmarked-all", action="store_true",
                   help="report word-like literals in every scanned file (exit 0)")
    p.add_argument("--release", metavar="LOCALE",
                   help="also require the language complete, not fuzzy and reviewed")
    p.add_argument("--notes", action="store_true",
                   help="list the entries with no translator note or no byte budget")
    p = sub.add_parser("merge", help="bring .po files up to the template")
    p.add_argument("locales", nargs="*")
    p.add_argument("--new", metavar="LOCALE", help="create a .po file for a new language")
    p = sub.add_parser("build", help="write the C tables and the character list")
    p.add_argument("--check", action="store_true", help="write nothing; fail when a file would change")
    p = sub.add_parser("pack", help="write the language pack files (<locale>.robolang)")
    p.add_argument("--version", required=True, help="the firmware version (1-15 printable ASCII)")
    p.add_argument("--out", required=True, help="the folder to write the packs to")
    p.add_argument("--key", help="an RSA-3072 PEM key to sign with (espsecure)")
    p.add_argument("--release", action="store_true", help="leave out draft languages")
    p.add_argument("--locale", action="append", help="only this language (repeatable)")
    p.add_argument("--font", action="append", metavar="ROLE=PATH",
                   help="an LVGL binary font for a text font (caption, body, label, title, headline) "
                        "put in the packs being written (repeatable)")
    p = sub.add_parser("pseudo", help="write the en-XA and ar-XB test languages to build/")
    p.add_argument("--build", action="store_true", help="also write their C tables")
    p = sub.add_parser("web", help="write the languages' Panel Manager JSON to build/ for the mock")
    p.add_argument("--out", default="build/i18n/web", help="the folder, from the repository root")
    sub.add_parser("inventory", help="write every screen's words, by surface, to build/i18n/inventory.md")
    args = parser.parse_args(argv)
    if args.command == "inventory":
        return cmd_inventory(args.root)
    if args.command == "extract":
        return cmd_extract(args.root, args.check)
    if args.command == "merge":
        return cmd_merge(args.root, args.locales, args.new)
    if args.command == "build":
        return cmd_build(args.root, args.check)
    if args.command == "pack":
        return cmd_pack(args.root, args.version, args.out, args.key, args.release, args.locale, args.font)
    if args.command == "pseudo":
        return cmd_pseudo(args.root, args.build)
    if args.command == "web":
        return cmd_web(args.root, args.out)
    return cmd_check(args.root, args.unmarked_all, args.release, args.notes)


if __name__ == "__main__":
    sys.exit(main())
