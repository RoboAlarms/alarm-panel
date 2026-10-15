#!/usr/bin/env python3
"""Minify one Panel Manager file before it is gzipped into the firmware image (BL-122, F7).

    minify_asset.py <in> <out>
    minify_asset.py --all <web dir> <out dir>      the six shipped files, for the page tests

Conservative on purpose, Python standard library only, nothing renamed:
  - comments are removed, indentation and trailing spaces are stripped, empty lines are dropped;
  - line breaks are kept (joining lines would change what automatic semicolon insertion does);
  - the inside of strings, template literals and regular-expression literals is copied byte for
    byte. Where the JavaScript scanner can't decide, the rest of the file is kept as it is.
"""
import re
import sys
from pathlib import Path

ASSETS = ("panel.html", "panel.css", "panel.js", "panel-pages.js", "panel-keypad.js", "panel-words.js")

# After one of these words a "/" starts a regular expression; after any other word it divides.
_REGEX_AFTER_WORD = {"return", "typeof", "case", "in", "of", "delete", "void", "throw", "new", "else",
                     "do", "instanceof", "yield", "await"}
# After one of these characters a "/" starts a regular expression.
_REGEX_AFTER_CHAR = set("([{},;:=!&|?+-*%<>~^")
_WORD = re.compile(r"[A-Za-z0-9_$\u0080-￿]+")


class _Undecided(Exception):
    pass


class _Js:
    def __init__(self, text):
        self.t = text
        self.out = []
        self.bol = True      # at the start of an output line (top level only)
        self.last = None     # last significant token: a word, or one character

    # -- helpers ------------------------------------------------------------------------------
    def _trim(self):
        while self.out and self.out[-1] in " \t\r":
            self.out.pop()

    def _newline(self, nested):
        if nested:
            self.out.append("\n")
            return
        self._trim()
        if not self.bol:
            self.out.append("\n")
            self.bol = True

    def _emit(self, s):
        self.out.append(s)
        self.bol = False

    def _string(self, i):
        t, q, j = self.t, self.t[i], i + 1
        while j < len(t):
            c = t[j]
            if c == "\\":
                j += 2
            elif c == q:
                return j + 1
            elif c == "\n":
                raise _Undecided
            else:
                j += 1
        raise _Undecided

    def _regex(self, i):
        t, j, in_class = self.t, i + 1, False
        while j < len(t):
            c = t[j]
            if c == "\n":
                raise _Undecided
            if c == "\\":
                j += 2
                continue
            if c == "[":
                in_class = True
            elif c == "]":
                in_class = False
            elif c == "/" and not in_class:
                return j + 1
            j += 1
        raise _Undecided

    def _template(self, i):
        """Copy a template literal from its opening backtick; returns the index after it."""
        t, j = self.t, i + 1
        self.out.append("`")
        while j < len(t):
            c = t[j]
            if c == "\\":
                self.out.append(t[j:j + 2])
                j += 2
            elif c == "`":
                self.out.append("`")
                return j + 1
            elif c == "$" and t.startswith("${", j):
                self.out.append("${")
                saved = (self.last, self.bol)
                j = self._code(j + 2, True)           # stops after the matching }
                self.out.append("}")
                self.last, self.bol = saved[0], False
            else:
                self.out.append(c)
                j += 1
        raise _Undecided

    # -- the scanner --------------------------------------------------------------------------
    def _code(self, i, nested):
        t, n, depth = self.t, len(self.t), 0
        while i < n:
            c = t[i]
            if c == "\n":
                self._newline(nested)
                i += 1
            elif c in " \t\r":
                if not (self.bol and not nested):
                    self.out.append(c)
                i += 1
            elif t.startswith("//", i):
                j = t.find("\n", i)
                i = n if j < 0 else j
            elif t.startswith("/*", i):
                j = t.find("*/", i + 2)
                if j < 0:
                    raise _Undecided
                if "\n" in t[i:j] and not nested:
                    self._newline(nested)      # a comment holding a line break still breaks the line
                elif "\n" in t[i:j]:
                    self.out.append("\n")
                elif not self.bol:
                    self.out.append(" ")
                i = j + 2
            elif c in "'\"":
                j = self._string(i)
                self._emit(t[i:j])
                self.last = "str"
                i = j
            elif c == "`":
                self.bol = False
                i = self._template(i)
                self.last = "str"
            elif c == "/":
                prev2 = "".join(self.out[-2:])
                regex = (self.last is None or self.last in _REGEX_AFTER_WORD
                         or (len(self.last) == 1 and self.last in _REGEX_AFTER_CHAR
                             and prev2 not in ("++", "--")))
                if regex:
                    j = self._regex(i)
                    self._emit(t[i:j])
                    self.last = "str"
                    i = j
                else:
                    self._emit(c)
                    self.last = "/"
                    i += 1
            else:
                m = _WORD.match(t, i)
                if m:
                    self._emit(m.group())
                    word = m.group()
                    self.last = "id" if self.last == "." else word
                    i = m.end()
                    continue
                if nested:
                    if c == "{":
                        depth += 1
                    elif c == "}":
                        if depth == 0:
                            return i + 1
                        depth -= 1
                self._emit(c)
                self.last = c
                i += 1
        if nested:
            raise _Undecided
        return i


def minify_js(text):
    s = _Js(text)
    # Find where the scanner gives up, so the rest of the file can be kept as it was.
    try:
        s._code(0, False)
        out = "".join(s.out)
    except _Undecided:
        out = None
    if out is None:
        # Redo, line by line from the start, until the first line that can't be scanned.
        lines = text.split("\n")
        for cut in range(len(lines), 0, -1):
            head = "\n".join(lines[:cut]) + "\n"
            s = _Js(head)
            try:
                s._code(0, False)
            except _Undecided:
                continue
            return "".join(s.out) + "\n".join(lines[cut:])
        return text
    return out.rstrip("\n") + "\n" if out.strip() else ""


def minify_css(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "'\"":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("url(", i):
            j = text.find(")", i)
            j = n if j < 0 else j + 1
            out.append(text[i:j])
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    lines = ("".join(out)).split("\n")
    return "".join(ln.strip() + "\n" for ln in lines if ln.strip())


_BLOCK = re.compile(r"<(script|style|pre|textarea)\b([^>]*)>(.*?)</\1\s*>", re.S | re.I)


def minify_html(text):
    # The blocks that keep their text are set aside, the rest loses comments, indentation and
    # empty lines, then the blocks go back in.
    held = []

    def hold(m):
        tag, attrs, body = m.group(1).lower(), m.group(2), m.group(3)
        if tag == "script" and not re.search(r"\btype\s*=", attrs, re.I):
            body = minify_js(body)
        elif tag == "style":
            body = minify_css(body)
        held.append(text[m.start():m.start(3)] + body + text[m.end(3):m.end()])
        return "\x00%d\x00" % (len(held) - 1)

    plain = _BLOCK.sub(hold, text)
    plain = re.sub(r"<!--.*?-->", "", plain, flags=re.S)
    out = "\n".join(ln.strip() for ln in plain.split("\n") if ln.strip()) + "\n"
    return re.sub(r"\x00(\d+)\x00", lambda m: held[int(m.group(1))], out)


def minify(text, kind):
    if kind == "js":
        return minify_js(text)
    if kind == "css":
        return minify_css(text)
    if kind == "html":
        return minify_html(text)
    raise ValueError("kind must be js, css or html")


def kind_of(path):
    return {".js": "js", ".css": "css", ".html": "html"}.get(Path(path).suffix.lower())


def minify_file(src, dst):
    kind = kind_of(src)
    data = Path(src).read_bytes().decode("utf-8")
    Path(dst).write_bytes((minify(data, kind) if kind else data).encode("utf-8"))


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "--all":
        out = Path(a[2])
        out.mkdir(parents=True, exist_ok=True)
        for name in ASSETS:
            minify_file(Path(a[1]) / name, out / name)
    elif len(a) == 2:
        minify_file(a[0], a[1])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
