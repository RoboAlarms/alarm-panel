#!/usr/bin/env python3
"""
lvpro_gen.py - export C code from an LVGL Pro XML project, laid out the way the LVGL Pro Editor
exports it.

The Editor's own exporter (and the lvglpro CLI) needs an Editor installation or a paid licence.
This script covers the subset of the XML used in this repository and writes the same files:

    <project>_gen.h / _gen.c                  constants, fonts, global styles, subjects, init
    components/**/<name>_gen.h / _gen.c       one create function per component
    screens/<name>_gen.h / _gen.c             one create function per screen
    <project>_runs_gen.h / _gen.c             runs of statements the functions above share (compact
                                              output: this tool's, the Editor writes them inline)
    fonts/<name>_data.c                       bitmap fonts via lv_font_conv (--fonts)
    fonts/letters/*.bin, letters.json         the letters pack's fonts (--fonts; docs/features/39 OPT-010)
    file_list_gen.cmake                     source list for CMake
    <project>.h / .c, CMakeLists.txt, ...     created once if missing (never overwritten)

The project therefore builds without the Editor, and an Editor export overwrites the generated
files at any time. Unknown attributes and references are errors, so typos fail here instead of
on the device.

Usage:
    python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui                  # C code
    python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui --fonts          # C code + fonts
    python tools/lvpro_gen.py components/ui/alarm_ui/layouts/1024x600/alarm_ui --check          # fail if generated files are stale

Fonts need lv_font_conv (the converter the Editor uses): set LV_FONT_CONV to the command, e.g.
    LV_FONT_CONV="node C:/tools/node_modules/lv_font_conv/lv_font_conv.js"
otherwise "lv_font_conv" on PATH, otherwise "npx --yes lv_font_conv@1.5.3" is used.
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

# --------------------------------------------------------------------------------------------
# Type tables (LVGL 9.5 names)
# --------------------------------------------------------------------------------------------

STYLE_PROPS = {
    # size and position
    "width": "size", "height": "size", "min_width": "coord", "max_width": "coord",
    "min_height": "coord", "max_height": "coord", "x": "coord", "y": "coord",
    "align": "enum:lv_align", "length": "coord",
    # padding and margin
    "pad_all": "int", "pad_hor": "int", "pad_ver": "int", "pad_top": "int", "pad_bottom": "int",
    "pad_left": "int", "pad_right": "int", "pad_row": "int", "pad_column": "int",
    "pad_gap": "int", "margin_all": "int", "margin_hor": "int", "margin_ver": "int",
    "margin_top": "int", "margin_bottom": "int", "margin_left": "int", "margin_right": "int",
    # background
    "radius": "int", "clip_corner": "bool", "bg_color": "color", "bg_opa": "opa",
    "bg_grad_color": "color", "bg_grad_dir": "enum:lv_grad_dir", "bg_main_stop": "int",
    "bg_grad_stop": "int", "bg_main_opa": "opa", "bg_grad_opa": "opa",
    # border, outline, shadow
    "border_color": "color", "border_width": "int", "border_opa": "opa",
    "border_side": "enum:lv_border_side", "border_post": "bool",
    "outline_color": "color", "outline_width": "int", "outline_opa": "opa", "outline_pad": "int",
    "shadow_width": "int", "shadow_color": "color", "shadow_opa": "opa",
    "shadow_offset_x": "int", "shadow_offset_y": "int", "shadow_spread": "int",
    # text
    "text_color": "color", "text_opa": "opa", "text_font": "font",
    "text_align": "enum:lv_text_align", "text_letter_space": "int", "text_line_space": "int",
    "text_decor": "enum:lv_text_decor",
    # image, line, arc
    "image_opa": "opa", "image_recolor": "color", "image_recolor_opa": "opa",
    "line_width": "int", "line_color": "color", "line_opa": "opa", "line_rounded": "bool",
    "arc_width": "int", "arc_color": "color", "arc_opa": "opa", "arc_rounded": "bool",
    # layout
    "layout": "enum:lv_layout", "flex_flow": "enum:lv_flex_flow",
    "flex_main_place": "enum:lv_flex_align", "flex_cross_place": "enum:lv_flex_align",
    "flex_track_place": "enum:lv_flex_align", "flex_grow": "int",
    # misc
    "opa": "opa", "anim_duration": "int", "transform_width": "int", "transform_height": "int",
    "translate_x": "int", "translate_y": "int", "transform_scale_x": "int",
    "transform_scale_y": "int", "transform_rotation": "int", "transform_pivot_x": "coord",
    "transform_pivot_y": "coord", "recolor": "color", "recolor_opa": "opa",
}

FLAGS = {"hidden", "clickable", "click_focusable", "checkable", "scrollable", "scroll_elastic",
         "scroll_momentum", "scroll_one", "scroll_on_focus", "snappable", "press_lock",
         "event_bubble", "gesture_bubble", "adv_hittest", "ignore_layout", "floating",
         "overflow_visible", "event_trickle", "state_trickle", "user_1"}
STATES = {"checked", "focused", "focus_key", "edited", "hovered", "pressed", "scrolled",
          "disabled"}

# attribute -> (C template, type); {o} = object, {v} = value
OBJ_ATTRS = {
    "x": ("lv_obj_set_x({o}, {v});", "coord"),
    "y": ("lv_obj_set_y({o}, {v});", "coord"),
    "width": ("lv_obj_set_width({o}, {v});", "size"),
    "height": ("lv_obj_set_height({o}, {v});", "size"),
    "align": ("lv_obj_set_align({o}, {v});", "enum:lv_align"),
    "flex_flow": ("lv_obj_set_flex_flow({o}, {v});", "enum:lv_flex_flow"),
    "flex_grow": ("lv_obj_set_flex_grow({o}, {v});", "int"),
    "ext_click_area": ("lv_obj_set_ext_click_area({o}, {v});", "int"),
    "scrollbar_mode": ("lv_obj_set_scrollbar_mode({o}, {v});", "enum:lv_scrollbar_mode"),
    "bind_checked": ("lv_obj_bind_checked({o}, {v});", "subject"),
}
WIDGET_ATTRS = {
    "lv_obj": {},
    "lv_button": {},
    "lv_image": {},  # the source is set from C (the project declares no images)
    "lv_label": {
        "text": ("lv_label_set_text({o}, {v});", "string"),
        "long_mode": ("lv_label_set_long_mode({o}, {v});", "enum:lv_label_long_mode"),
    },
    "lv_arc": {
        "bg_start_angle": ("lv_arc_set_bg_start_angle({o}, {v});", "int"),
        "bg_end_angle": ("lv_arc_set_bg_end_angle({o}, {v});", "int"),
        "start_angle": ("lv_arc_set_start_angle({o}, {v});", "int"),
        "end_angle": ("lv_arc_set_end_angle({o}, {v});", "int"),
        "rotation": ("lv_arc_set_rotation({o}, {v});", "int"),
        "min_value": ("lv_arc_set_min_value({o}, {v});", "int"),
        "max_value": ("lv_arc_set_max_value({o}, {v});", "int"),
        "value": ("lv_arc_set_value({o}, {v});", "int"),
        "mode": ("lv_arc_set_mode({o}, {v});", "enum:lv_arc_mode"),
        "bind_value": ("lv_arc_bind_value({o}, {v});", "subject"),
    },
    "lv_bar": {
        "min_value": ("lv_bar_set_min_value({o}, {v});", "int"),
        "max_value": ("lv_bar_set_max_value({o}, {v});", "int"),
        "value": ("lv_bar_set_value({o}, {v}, LV_ANIM_OFF);", "int"),
        "mode": ("lv_bar_set_mode({o}, {v});", "enum:lv_bar_mode"),
        "orientation": ("lv_bar_set_orientation({o}, {v});", "enum:lv_bar_orientation"),
        "bind_value": ("lv_bar_bind_value({o}, {v});", "subject"),
    },
    "lv_slider": {
        "min_value": ("lv_slider_set_min_value({o}, {v});", "int"),
        "max_value": ("lv_slider_set_max_value({o}, {v});", "int"),
        "value": ("lv_slider_set_value({o}, {v}, LV_ANIM_OFF);", "int"),
        "mode": ("lv_slider_set_mode({o}, {v});", "enum:lv_slider_mode"),
        "orientation": ("lv_slider_set_orientation({o}, {v});", "enum:lv_slider_orientation"),
        "bind_value": ("lv_slider_bind_value({o}, {v});", "subject"),
    },
    "lv_switch": {
        "orientation": ("lv_switch_set_orientation({o}, {v});", "enum:lv_switch_orientation"),
    },
    "lv_spinner": {
        "anim_duration": ("lv_spinner_set_anim_duration({o}, {v});", "int"),
        "arc_sweep": ("lv_spinner_set_arc_sweep({o}, {v});", "int"),
    },
    "lv_textarea": {
        "text": ("lv_textarea_set_text({o}, {v});", "string"),
        "placeholder_text": ("lv_textarea_set_placeholder_text({o}, {v});", "string"),
        "one_line": ("lv_textarea_set_one_line({o}, {v});", "bool"),
        "password_mode": ("lv_textarea_set_password_mode({o}, {v});", "bool"),
        "password_show_time": ("lv_textarea_set_password_show_time({o}, {v});", "int"),
        "text_selection": ("lv_textarea_set_text_selection({o}, {v});", "bool"),
        "cursor_pos": ("lv_textarea_set_cursor_pos({o}, {v});", "int"),
    },
    "lv_keyboard": {
        "mode": ("lv_keyboard_set_mode({o}, {v});", "enum:lv_keyboard_mode"),
        "popovers": ("lv_keyboard_set_popovers({o}, {v});", "bool"),
        "textarea": ("lv_keyboard_set_textarea({o}, {v});", "lv_obj"),
    },
    # A roller's choices usually come from C (month names, hours); never animated to a value
    # (UI-020): the selection is set with LV_ANIM_OFF, and its style's anim_duration is 0.
    "lv_roller": {
        "options": ("lv_roller_set_options({o}, {v}, LV_ROLLER_MODE_NORMAL);", "string"),
        "visible_row_count": ("lv_roller_set_visible_row_count({o}, {v});", "int"),
        "selected": ("lv_roller_set_selected({o}, {v}, LV_ANIM_OFF);", "int"),
    },
}

PARTS = {"main": "LV_PART_MAIN", "scrollbar": "LV_PART_SCROLLBAR",
         "indicator": "LV_PART_INDICATOR", "knob": "LV_PART_KNOB",
         "selected": "LV_PART_SELECTED", "items": "LV_PART_ITEMS", "cursor": "LV_PART_CURSOR",
         "textarea_placeholder": "LV_PART_TEXTAREA_PLACEHOLDER"}

PROP_CTYPES = {"string": "const char *", "int": "int32_t", "color": "lv_color_t",
               "font": "const lv_font_t *", "subject": "lv_subject_t *", "bool": "bool",
               "opa": "lv_opa_t", "image": "const void *"}

C_KEYWORDS = {"int", "char", "void", "static", "const", "return", "if", "else", "for", "while",
              "switch", "case", "default", "break", "continue", "struct", "union", "enum"}


class GenError(Exception):
    pass


def enum_c(enum_name, value):
    base = enum_name[3:] if enum_name.startswith("lv_") else enum_name
    return "LV_%s_%s" % (base.upper(), value.upper())


def is_ui_text(value):
    """True for a literal string value that is text for people: it holds a run of two or more
    letters. Not `$param` or `#const` references, not glyph-only values (icon code points), not
    numbers or bare units ("60 s"). tools/i18n.py imports this so the generator (which routes such
    text through alarm_ui_tr) and the extractor (which lists it in the catalog) cannot disagree."""
    if value is None or value.startswith(("$", "#")):
        return False
    return re.search(r"[^\W\d_]{2,}", value) is not None


def tr_string(text, ctx=None):
    """C expression for literal UI text: through the lookup when it is text for people, with the
    element's translation context when a `<!-- tr-context: ... -->` comment gave it one."""
    if is_ui_text(text):
        if ctx:
            return "alarm_ui_tr_c(%s, %s)" % (c_string(ctx), c_string(text))
        return "alarm_ui_tr(%s)" % c_string(text)
    return c_string(text)


# A word that means two things ("Open": a filter's name, or a door's state) gets a context for the
# translators (docs/features/37): an XML comment right before the element,
#     <!-- tr-context: zone filter -->
#     <segment text="Open">
# which the LVGL Pro editor ignores. parse_xml() reads the comments into TR_CTX (the element ->
# its context) and drops them, so nothing else here ever sees a comment. Keyed by the element
# itself, which keeps it alive: keyed by id(), a tree dropped after parsing handed its ids on to
# later elements, and they took a stale context (CI, 2026-09-29).
TR_CTX = {}
_TR_CTX_RE = re.compile(r"^\s*tr-context:\s*(\S(?:.*\S)?)\s*$")


def parse_xml(path):
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    root = ET.parse(path, parser).getroot()

    def strip(node):
        ctx = None
        for child in list(node):
            if child.tag is ET.Comment:
                m = _TR_CTX_RE.match(child.text or "")
                if m:
                    ctx = m.group(1)
                node.remove(child)
                continue
            if ctx is not None:
                TR_CTX[child] = ctx
                ctx = None
            strip(child)

    strip(root)
    return root


def c_string(text):
    """C literal; non-ASCII as \\x escapes, split before a following hex digit."""
    out = ['"']
    data = text.encode("utf-8")
    hex_open = False
    for b in data:
        ch = chr(b)
        if b < 0x20 or b >= 0x7F:
            out.append("\\x%02X" % b)
            hex_open = True
            continue
        if hex_open and ch in "0123456789abcdefABCDEF":
            out.append('" "')
        hex_open = False
        if ch in '"\\':
            out.append("\\" + ch)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def parse_selector(sel, sep="|"):
    if sel is None or sel in ("", "0", "default", "main"):
        return "0"
    parts, states = [], []
    for token in sel.split(sep):
        token = token.strip()
        if token in ("", "default", "main"):
            continue
        if token in PARTS:
            parts.append(PARTS[token])
        elif token in STATES or token in ("any", "user_1", "user_2", "user_3", "user_4"):
            states.append("LV_STATE_" + token.upper())
        else:
            raise GenError("unknown selector '%s'" % token)
    items = parts + states
    return " | ".join(items) if items else "0"


# --------------------------------------------------------------------------------------------
# Project model
# --------------------------------------------------------------------------------------------

class Component:
    def __init__(self, name, rel_dir, root):
        self.name = name
        self.rel_dir = rel_dir  # e.g. components/common/icon_chip
        self.props = []  # (name, type, default)
        self.styles = []  # (name, [(prop, value)])
        api = root.find("api")
        if api is not None:
            for p in api.findall("prop"):
                self.props.append((p.get("name"), p.get("type"), p.get("default")))
        styles = root.find("styles")
        if styles is not None:
            for s in styles.findall("style"):
                self.styles.append((s.get("name"), style_items(s)))
        self.view = root.find("view")
        if self.view is None:
            raise GenError("%s: <view> missing" % name)


class Screen:
    def __init__(self, name, rel_dir, root):
        self.name = name
        self.rel_dir = rel_dir
        self.permanent = root.get("permanent") == "true"
        self.view = root.find("view")
        if self.view is None:
            raise GenError("%s: <view> missing" % name)


def style_items(elem, skip=()):
    items = []
    for k, v in elem.attrib.items():
        if k in ("name", "help") or k in skip:
            continue
        if k not in STYLE_PROPS:
            raise GenError("style '%s': unknown property '%s'" % (elem.get("name"), k))
        items.append((k, v))
    return items


class Project:
    def __init__(self, root_dir):
        self.dir = Path(root_dir).resolve()
        self.name = self.dir.name
        self.NAME = self.name.upper()
        proj = parse_xml(self.dir / "project.xml")
        self.lvgl_version = proj.get("lvgl_version", "9.5.0")
        self.theme = proj.get("theme", "simple")
        self.targets = [t.get("name") for t in proj.iter("target")]
        g = parse_xml(self.dir / "globals.xml")
        self.consts = {}  # name -> (type, value, help)
        consts = g.find("consts")
        if consts is not None:
            for c in consts:
                self.consts[c.get("name")] = (c.tag, c.get("value"), c.get("help"))
        self.fonts = []
        fonts = g.find("fonts")
        if fonts is not None:
            for f in fonts:
                if f.tag != "bin":
                    raise GenError("font '%s': only <bin> fonts are supported" % f.get("name"))
                if f.get("as_file", "false") != "false":
                    raise GenError("font '%s': as_file must be false" % f.get("name"))
                self.fonts.append(dict(f.attrib))
        self.font_names = {f["name"] for f in self.fonts}
        self.subjects = []  # (type, name, value, attrib)
        subjects = g.find("subjects")
        if subjects is not None:
            for s in subjects:
                if s.tag not in ("int", "string", "color"):
                    raise GenError("subject '%s': type <%s> not supported" % (s.get("name"), s.tag))
                self.subjects.append((s.tag, s.get("name"), s.get("value"), dict(s.attrib)))
        self.subject_names = {s[1] for s in self.subjects}
        self.styles = []
        styles = g.find("styles")
        if styles is not None:
            for s in styles.findall("style"):
                self.styles.append((s.get("name"), style_items(s)))
        self.global_style_names = {s[0] for s in self.styles}
        self.components = {}
        for path in sorted((self.dir / "components").rglob("*.xml")):
            root = parse_xml(path)
            if root.tag != "component":
                continue
            name = path.stem
            rel = path.parent.relative_to(self.dir).as_posix()
            if name in self.components:
                raise GenError("duplicate component '%s'" % name)
            self.components[name] = Component(name, rel, root)
        self.screens = []
        for path in sorted((self.dir / "screens").glob("*.xml")):
            root = parse_xml(path)
            if root.tag != "screen":
                continue
            self.screens.append(Screen(path.stem, "screens", root))
        self.callbacks = []
        self.colors = [n for n, (t, _, _) in self.consts.items() if t == "color"]
        self.style_ids = style_ids(self)
        self.themes = load_themes(self)


# --------------------------------------------------------------------------------------------
# Appearance (docs/features/07 UI-010, BL-132): the palettes of themes.xml beside globals.xml
# --------------------------------------------------------------------------------------------
#
# themes.xml names every colour const of globals.xml once per theme; the first theme is the
# globals' own colours (what the editor shows). The generated COLOR_* constants read the current
# palette (`alarm_ui_palette`), so a style, a component and the C code take the colours of the
# theme in force when they set them. A switch at run time (alarm_ui.c) then finds every colour
# already set by its value: each palette value stands for one colour name only, across all the
# palettes, and is never a literal colour of the XML. Where two names share a value (Dark's
# color_bg and color_on_tone), the later one is nudged in the bits RGB565 doesn't keep (the low
# three of red and blue, the low two of green): the panel and the preview draw in 16 bits, so the
# picture is the same. A <styles themes="..."> section adds properties to named styles in some
# themes (high contrast's outlines); the style keeps its own value, or none, in the others.

# The bits a 16-bit (RGB565) screen drops, smallest change first: a nudge in them doesn't show.
NUDGE_BITS = (0x000001, 0x010000, 0x000100, 0x000002, 0x020000, 0x000200, 0x000004, 0x040000)
NUDGES = [sum(b for i, b in enumerate(NUDGE_BITS) if m >> i & 1)
          for m in sorted(range(1, 256), key=lambda m: (bin(m).count("1"), m))]
COLOR_HEX_RE = re.compile(r"0x([0-9a-fA-F]{6})")


def color_enum(p, name):
    """color_bg -> ALARM_UI_COLOR_BG: the colour's index in a palette."""
    return "%s_COLOR_%s" % (p.NAME, (name[6:] if name.startswith("color_") else name).upper())


def style_enum(p, comp, name):
    base = name[6:] if name.startswith("style_") else name
    return "%s_STYLE_%s" % (p.NAME, (comp + "_" + base if comp else base).upper())


def style_ids(p):
    """(component or None, style name) -> its index name, globals first: the registry of styles a
    theme switch reaches (`<project>_styles[]`)."""
    ids = {}
    seen = set()
    for comp, styles in [(None, p.styles)] + [(c.name, c.styles) for c in p.components.values()]:
        for name, _ in styles:
            enum = style_enum(p, comp, name)
            if enum in seen:
                raise GenError("style '%s'%s: index name %s is taken" % (
                    name, " of " + comp if comp else "", enum))
            seen.add(enum)
            ids[(comp, name)] = enum
    return ids


def literal_colors(p):
    """Every literal colour the XML sets (style properties and style_* attributes): a palette value
    must never equal one, or a switch would recolour it."""
    found = {0x000000, 0xFFFFFF}

    def color_attr(k):
        prop = k[6:].partition("-")[0] if k.startswith("style_") else k
        return STYLE_PROPS.get(prop) == "color"

    def scan(elem):
        for k, v in elem.attrib.items():
            m = COLOR_HEX_RE.fullmatch(v or "")
            if m and color_attr(k):
                found.add(int(m.group(1), 16))
        for child in elem:
            scan(child)

    for _, items in p.styles + [s for c in p.components.values() for s in c.styles]:
        for k, v in items:
            m = COLOR_HEX_RE.fullmatch(v or "")
            if m and STYLE_PROPS.get(k) == "color":
                found.add(int(m.group(1), 16))
    for view in [c.view for c in p.components.values()] + [s.view for s in p.screens]:
        scan(view)
    return found


class Theme:
    def __init__(self, name, title, high, saver, colors):
        self.name, self.title, self.high, self.saver = name, title, high, saver
        self.colors = colors  # name -> 0xRRGGBB as approved
        self.stored = {}  # name -> the value in the generated palette (nudged where it must be)


def load_themes(p):
    """themes.xml beside globals.xml, or one theme of the globals' colours when there is none."""
    path = p.dir / "themes.xml"
    globals_colors = {n: int(COLOR_HEX_RE.fullmatch(p.consts[n][1]).group(1), 16) for n in p.colors
                      if COLOR_HEX_RE.fullmatch(p.consts[n][1] or "")}
    if len(globals_colors) != len(p.colors):
        raise GenError("globals.xml: a colour const is not 0xRRGGBB")
    themes = []
    overrides = []
    if path.exists():
        root = parse_xml(path)
        for t in root.findall("theme"):
            colors = {}
            for c in t.findall("color"):
                m = COLOR_HEX_RE.fullmatch(c.get("value") or "")
                if not m:
                    raise GenError("themes.xml: theme '%s': bad colour '%s'" % (t.get("name"), c.get("value")))
                if c.get("name") in colors:
                    raise GenError("themes.xml: theme '%s' names %s twice" % (t.get("name"), c.get("name")))
                colors[c.get("name")] = int(m.group(1), 16)
            if set(colors) != set(p.colors):
                missing = sorted(set(p.colors) - set(colors))
                extra = sorted(set(colors) - set(p.colors))
                raise GenError("themes.xml: theme '%s' %s" % (t.get("name"), "; ".join(
                    (["lacks " + ", ".join(missing)] if missing else []) +
                    (["names what globals.xml doesn't: " + ", ".join(extra)] if extra else []))))
            name = t.get("name") or ""
            if not re.fullmatch(r"[a-z][a-z0-9_]*", name) or name in [x.name for x in themes]:
                raise GenError("themes.xml: bad or repeated theme name '%s'" % name)
            themes.append(Theme(name, t.get("title") or name, t.get("contrast") == "high",
                                t.get("saver") or name, colors))
        if not themes:
            raise GenError("themes.xml: no <theme>")
        if themes[0].colors != globals_colors:
            diff = sorted(n for n in p.colors if themes[0].colors[n] != globals_colors[n])
            raise GenError("themes.xml: the first theme ('%s') must be globals.xml's colours; %s differ"
                           % (themes[0].name, ", ".join(diff)))
        names = [t.name for t in themes]
        for t in themes:
            if t.saver not in names:
                raise GenError("themes.xml: theme '%s' keeps the saver in '%s', which is no theme" % (t.name, t.saver))
        for sec in root.findall("styles"):
            mask = 0
            for n in (sec.get("themes") or "").split():
                if n not in names:
                    raise GenError("themes.xml: <styles themes> names '%s', which is no theme" % n)
                mask |= 1 << names.index(n)
            if not mask:
                raise GenError("themes.xml: a <styles> section names no theme")
            for s in sec.findall("style"):
                overrides.append((mask, s.get("component"), s.get("name"), style_items(s, ("component",))))
    else:
        themes.append(Theme("dark", "Dark", False, "dark", globals_colors))
    nudge_palettes(themes, p.colors, literal_colors(p))
    p.style_overrides = resolve_overrides(p, overrides, len(themes))
    return themes


def nudge_palettes(themes, order, reserved):
    """Give every (theme, colour) a stored value that stands for that colour name only."""
    owner = {}  # stored value -> colour name
    for t in themes:
        for name in order:
            v = t.colors[name]
            for cand in [v] + [v ^ d for d in NUDGES]:
                if cand not in reserved and owner.get(cand, name) == name:
                    break
            else:
                raise GenError("themes.xml: no free value near 0x%06X for %s" % (v, name))
            owner[cand] = name
            t.stored[name] = cand


def style_value_kind(p, where, prop, raw):
    """(kind, C value) of a style value in the override table."""
    typ = STYLE_PROPS[prop]
    if typ == "color":
        if raw.startswith("#"):
            if raw[1:] not in p.colors:
                raise GenError("%s: '%s' is not a colour of globals.xml" % (where, raw))
            return "COLOR", color_enum(p, raw[1:])
        m = COLOR_HEX_RE.fullmatch(raw)
        if not m:
            raise GenError("%s: bad colour '%s'" % (where, raw))
        return "HEX", "0x%s" % m.group(1).upper()
    if raw.startswith(("#", "$")):
        raise GenError("%s: %s must be a number here" % (where, prop))
    if typ == "opa":
        m = re.fullmatch(r"(\d+)%", raw)
        return "INT", str(255 * int(m.group(1)) // 100 if m else int(raw, 0))
    if typ in ("int", "coord", "bool"):
        if raw in ("true", "false"):
            return "INT", "1" if raw == "true" else "0"
        try:
            return "INT", str(int(raw, 0))
        except ValueError:
            raise GenError("%s: bad number '%s'" % (where, raw))
    raise GenError("%s: %s can't change with the theme" % (where, prop))


def resolve_overrides(p, overrides, n_themes):
    """[(mask, style index name, LV_STYLE_*, kind, value, base kind, base value)], merged."""
    out = {}
    for mask, comp, name, items in overrides:
        key = (comp, name)
        where = "themes.xml: style '%s'%s" % (name, " of " + comp if comp else "")
        if key not in p.style_ids:
            raise GenError(where + ": no such style")
        own = dict(p.styles if comp is None else p.components[comp].styles).get(name, [])
        own = dict(own)
        for prop, raw in items:
            kind, val = style_value_kind(p, where, prop, raw)
            if prop in own:
                bkind, bval = style_value_kind(p, where, prop, own[prop])
            else:
                bkind, bval = "NONE", "0"
            k = (p.style_ids[key], prop, kind, val)
            if k in out:
                out[k] = (out[k][0] | mask,) + out[k][1:]
            else:
                out[k] = (mask, bkind, bval)
    rows = []
    for (sid, prop, kind, val), (mask, bkind, bval) in out.items():
        rows.append((mask, sid, "LV_STYLE_" + prop.upper(), kind, val, bkind, bval))
    # A property the same style changes in two ways needs them in different themes.
    seen = {}
    for mask, sid, prop, *_ in rows:
        if seen.get((sid, prop), 0) & mask:
            raise GenError("themes.xml: %s's %s changes twice in one theme" % (sid, prop))
        seen[(sid, prop)] = seen.get((sid, prop), 0) | mask
    return rows


# --------------------------------------------------------------------------------------------
# Code generation for one create function
# --------------------------------------------------------------------------------------------

class Func:
    def __init__(self, project, params, local_styles, where):
        self.p = project
        self.params = dict(params)  # name -> type
        self.local_styles = set(local_styles)
        self.where = where
        self.counters = {}
        self.names = set()
        self.vars = set()  # every object variable (compaction needs to tell them from globals)
        self.lines = []

    def err(self, msg):
        raise GenError("%s: %s" % (self.where, msg))

    def emit(self, line=""):
        self.lines.append(("        " + line) if line else "")

    def var_for(self, base, name):
        if name:
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name) or name in C_KEYWORDS:
                self.err("name '%s' is not a C identifier" % name)
            if name in self.names or name in self.params:
                self.err("duplicate name '%s'" % name)
            self.names.add(name)
            self.vars.add(name)
            return name
        n = self.counters.get(base, 0)
        self.counters[base] = n + 1
        self.vars.add("%s_%d" % (base, n))
        return "%s_%d" % (base, n)

    # ---- values ----
    def value(self, raw, typ, ctx=None):
        if raw is None:
            self.err("missing value")
        if raw.startswith("$"):
            name = raw[1:]
            if name not in self.params:
                self.err("unknown property '$%s'" % name)
            return name
        if raw.startswith("#"):
            name = raw[1:]
            if name not in self.p.consts:
                self.err("unknown constant '#%s'" % name)
            return name.upper()
        if typ == "color":
            m = re.fullmatch(r"0x([0-9a-fA-F]{6})", raw)
            if m:
                return "lv_color_hex(0x%s)" % m.group(1).upper()
            m = re.fullmatch(r"0x([0-9a-fA-F]{3})", raw)
            if m:
                return "lv_color_hex3(0x%s)" % m.group(1).upper()
            self.err("bad color '%s'" % raw)
        if typ == "opa":
            m = re.fullmatch(r"(\d+)%", raw)
            if m:
                return "(255 * %s / 100)" % m.group(1)
            return str(int(raw, 0))
        if typ == "int":
            try:
                return str(int(raw, 0))
            except ValueError:
                self.err("bad integer '%s'" % raw)
        if typ in ("size", "coord"):
            m = re.fullmatch(r"(-?\d+)%", raw)
            if m:
                return "lv_pct(%s)" % m.group(1)
            if raw == "content" and typ == "size":
                return "LV_SIZE_CONTENT"
            try:
                return str(int(raw, 0))
            except ValueError:
                self.err("bad size '%s'" % raw)
        if typ == "font":
            if raw in self.p.font_names:
                return raw
            if re.fullmatch(r"lv_font_montserrat_\d+", raw):
                return "&" + raw
            self.err("unknown font '%s'" % raw)
        if typ == "bool":
            if raw in ("true", "false"):
                return raw
            m = re.fullmatch(r"\{(!?)([A-Za-z_][A-Za-z0-9_]*)\}", raw)
            if m:
                if m.group(2) not in self.params:
                    self.err("unknown property in '%s'" % raw)
                return m.group(1) + m.group(2)
            self.err("bad boolean '%s'" % raw)
        if typ.startswith("enum:"):
            if not re.fullmatch(r"[a-z0-9_]+", raw):
                self.err("bad enum value '%s'" % raw)
            return enum_c(typ[5:], raw)
        if typ == "string":
            return tr_string(raw, ctx)
        if typ == "subject":
            if raw not in self.p.subject_names:
                self.err("unknown subject '%s'" % raw)
            return "&" + raw
        if typ == "lv_obj":
            # a widget created earlier in the same component or screen, by name
            if raw not in self.names:
                self.err("'%s' must name a widget created before it" % raw)
            return raw
        if typ == "style":
            if raw in self.local_styles:
                return "&" + raw
            if raw in self.p.global_style_names:
                return "&" + raw
            self.err("unknown style '%s'" % raw)
        self.err("unsupported type '%s'" % typ)

    # ---- attributes ----
    def attrs(self, obj, tag, elem, skip):
        widget = tag if tag.startswith("lv_") else None
        pending_bind_text = None
        for k, v in elem.attrib.items():
            if k in skip or k == "help":
                continue
            if k == "name":
                continue
            if k.startswith("style_"):
                prop, _, sel = k[6:].partition("-")
                if prop not in STYLE_PROPS:
                    self.err("unknown style property '%s'" % k)
                self.emit("lv_obj_set_style_%s(%s, %s, %s);" % (
                    prop, obj, self.value(v, STYLE_PROPS[prop]), parse_selector(sel, "-")))
                continue
            if k in FLAGS:
                self.emit("lv_obj_set_flag(%s, LV_OBJ_FLAG_%s, %s);" % (
                    obj, k.upper(), self.value(v, "bool")))
                continue
            if k in STATES:
                self.emit("lv_obj_set_state(%s, LV_STATE_%s, %s);" % (
                    obj, k.upper(), self.value(v, "bool")))
                continue
            if k in OBJ_ATTRS:
                tmpl, typ = OBJ_ATTRS[k]
                self.emit(tmpl.format(o=obj, v=self.value(v, typ)))
                continue
            if widget == "lv_label" and k == "bind_text":
                pending_bind_text = v
                continue
            if widget == "lv_label" and k == "bind_text-fmt":
                continue
            wattrs = WIDGET_ATTRS.get(widget, {}) if widget else {}
            if k in wattrs:
                tmpl, typ = wattrs[k]
                self.emit(tmpl.format(o=obj, v=self.value(v, typ, TR_CTX.get(elem))))
                continue
            self.err("unknown attribute '%s' on <%s>" % (k, tag))
        if pending_bind_text is not None:
            fmt = elem.get("bind_text-fmt")
            self.emit("lv_label_bind_text(%s, %s, %s);" % (
                obj, self.value(pending_bind_text, "subject"),
                c_string(fmt) if fmt is not None else "NULL"))

    # ---- child elements ----
    def statement(self, obj, child):
        t = child.tag
        a = child.attrib
        if t == "style":
            self.emit("lv_obj_add_style(%s, %s, %s);" % (
                obj, self.value(a.get("name"), "style"), parse_selector(a.get("selector"))))
        elif t == "remove_style_all":
            self.emit("lv_obj_remove_style_all(%s);" % obj)
        elif t == "bind_style":
            self.emit("lv_obj_bind_style(%s, %s, %s, %s, %s);" % (
                obj, self.value(a.get("name"), "style"), parse_selector(a.get("selector")),
                self.value(a.get("subject"), "subject"), self.value(a.get("ref_value"), "int")))
        elif t == "bind_style_prop":
            prop = a.get("prop")
            if prop not in STYLE_PROPS:
                self.err("unknown style property '%s'" % prop)
            self.emit("lv_obj_bind_style_prop(%s, LV_STYLE_%s, %s, %s);" % (
                obj, prop.upper(), parse_selector(a.get("selector")),
                self.value(a.get("subject"), "subject")))
        elif t.startswith("bind_flag_if_") or t.startswith("bind_state_if_"):
            kind = "flag" if t.startswith("bind_flag") else "state"
            op = t.split("_if_")[1]
            if op not in ("eq", "not_eq", "gt", "ge", "lt", "le"):
                self.err("unknown binding <%s>" % t)
            what = a.get(kind)
            if kind == "flag":
                if what not in FLAGS:
                    self.err("unknown flag '%s'" % what)
                what_c = "LV_OBJ_FLAG_" + what.upper()
            else:
                if what not in STATES:
                    self.err("unknown state '%s'" % what)
                what_c = "LV_STATE_" + what.upper()
            self.emit("lv_obj_%s(%s, %s, %s, %s);" % (
                t, obj, self.value(a.get("subject"), "subject"), what_c,
                self.value(a.get("ref_value"), "int")))
        elif t == "event_cb":
            cb = a.get("callback")
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", cb or ""):
                self.err("bad callback name '%s'" % cb)
            if cb not in self.p.callbacks:
                self.p.callbacks.append(cb)
            ud = a.get("user_data")
            self.emit("lv_obj_add_event_cb(%s, %s, %s, %s);" % (
                obj, cb, enum_c("lv_event", a.get("trigger", "clicked")),
                "(void *)" + c_string(ud) if ud is not None else "NULL"))
        elif t == "subject_set_int_event":
            self.emit("lv_obj_add_subject_set_int_event(%s, %s, %s, %s);" % (
                obj, self.value(a.get("subject"), "subject"),
                enum_c("lv_event", a.get("trigger", "clicked")),
                self.value(a.get("value"), "int")))
        elif t == "subject_toggle_event":
            self.emit("lv_obj_add_subject_toggle_event(%s, %s, %s);" % (
                obj, self.value(a.get("subject"), "subject"),
                enum_c("lv_event", a.get("trigger", "clicked"))))
        else:
            return False
        return True

    def is_widget_tag(self, tag):
        return tag in WIDGET_ATTRS or tag in self.p.components

    def component_args(self, comp, elem):
        args = []
        for pname, ptype, pdefault in comp.props:
            raw = elem.get(pname, pdefault)
            if raw is None:
                if ptype == "subject" or ptype == "string" or ptype == "font" or ptype == "image":
                    args.append("NULL")
                elif ptype == "color":
                    args.append("lv_color_hex(0x000000)")
                elif ptype == "bool":
                    args.append("false")
                else:
                    args.append("0")
                continue
            args.append(self.value(raw, ptype, TR_CTX.get(elem) if pname in elem.attrib else None))
        return args

    def create(self, parent, elem, root_name_static=None):
        """Emit creation code for a widget or component element; return its variable."""
        tag = elem.tag
        name = elem.get("name")
        comp = self.p.components.get(tag)
        if comp is None and tag not in WIDGET_ATTRS:
            self.err("unknown widget or component <%s>" % tag)
        skip = {p[0] for p in comp.props} if comp else set()
        base = tag
        var = self.var_for(base, name)
        if comp:
            call = "%s_create(%s%s)" % (tag, parent, "".join(
                ", " + a for a in self.component_args(comp, elem)))
        else:
            call = "%s_create(%s)" % (tag, parent)
        start = len(self.lines)
        self.emit("lv_obj_t * %s = %s;" % (var, call))
        if root_name_static:
            self.emit("lv_obj_set_name_static(%s, %s);" % (var, c_string(root_name_static)))
        if name:
            self.emit("lv_obj_set_name(%s, %s);" % (var, c_string(name)))
        self.attrs(var, tag, elem, skip)
        self.children(var, elem)
        used = len(self.lines) - start > 1
        if not used and root_name_static is None:
            self.lines[start] = "        %s;" % call
        self.emit()
        return var

    def children(self, obj, elem):
        for child in elem:
            if not isinstance(child.tag, str):
                continue  # comment
            if self.statement(obj, child):
                continue
            if self.is_widget_tag(child.tag):
                self.create(obj, child)
                continue
            self.err("unknown element <%s>" % child.tag)


# --------------------------------------------------------------------------------------------
# File writers
# --------------------------------------------------------------------------------------------

HEADER_INCLUDES = """#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
    #include "lvgl_private.h"
#else
    #include "lvgl/lvgl.h"
    #include "lvgl/lvgl_private.h"
#endif

#if defined(LV_USE_XML) && LV_USE_XML
    #include "lv_xml/lv_xml.h"
#endif"""


def prop_ctype(typ):
    if typ.startswith("enum:"):
        return typ[5:] + "_t"
    return PROP_CTYPES[typ]


def tidy(lines):
    """Collapse runs of blank lines (nested elements each end with one)."""
    out = []
    for line in lines:
        if line == "" and out and out[-1] == "":
            continue
        out.append(line)
    while out and out[-1] == "":
        out.pop()
    return out


def rel_to_root(rel_dir):
    depth = len(Path(rel_dir).parts)
    return "/".join([".."] * depth)


def style_block(p, func, styles, indent, static, comp=None, cp=None):
    """Style declarations + one-time init (component-local or global). Each style joins the
    registry a theme switch reaches (`<project>_style_track`, in the hand-written <project>.c)."""
    out = []
    if not styles:
        return out
    if static:
        for name, _ in styles:
            out.append(indent + "static lv_style_t %s;" % name)
        out.append("")
    out.append(indent + "static bool style_inited = false;")
    out.append("")
    out.append(indent + "if (!style_inited) {")
    table = len(styles) >= 3  # init and track in a loop over a table instead of a call each
    loop = indent + "    for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {"
    if table:
        out.append(indent + "    static lv_style_t * const style_list[] = {")
        out += [indent + "        &%s," % name for name, _ in styles]
        out.append(indent + "    };")
        out.append("")
    out.append(indent + "    /*Init all styles*/")
    if table:
        out += [loop, indent + "        lv_style_init(style_list[i]);", indent + "    }"]
    else:
        out += [indent + "    lv_style_init(&%s);" % name for name, _ in styles]
    out.append("")
    sets = []
    for name, items in styles:
        for prop, val in items:
            sets.append(indent + "    lv_style_set_%s(&%s, %s);" % (
                prop, name, func.value(val, STYLE_PROPS[prop])))
    out += cp.add(func, sets, "") if cp is not None else sets
    out.append("")
    if table:
        out.append(indent + "    static const uint16_t style_ids[] = {")
        out += [indent + "        %s," % p.style_ids[(comp, name)] for name, _ in styles]
        out.append(indent + "    };")
        out += [loop, indent + "        %s_style_track(style_list[i], style_ids[i]);" % p.name,
                indent + "    }"]
    else:
        out += [indent + "    %s_style_track(&%s, %s);" % (p.name, name, p.style_ids[(comp, name)])
                for name, _ in styles]
    out.append("")
    out.append(indent + "    style_inited = true;")
    out.append(indent + "}")
    out.append("")
    return out


def gen_component(p, comp, cp):
    params = [(n, t) for n, t, _ in comp.props]
    for n, t in params:
        if t not in PROP_CTYPES and not t.startswith("enum:"):
            raise GenError("%s: property '%s' has unsupported type '%s'" % (comp.name, n, t))
    local = [s[0] for s in comp.styles]
    f = Func(p, params, local, comp.name)
    view = comp.view
    base = view.get("extends")
    if base is None:
        raise GenError("%s: <view extends> missing" % comp.name)
    fake = ET.Element(base, {k: v for k, v in view.attrib.items() if k != "extends"})
    for ch in view:
        fake.append(ch)
    root = f.create("parent", fake, root_name_static=comp.name + "_#")
    sig_params = "".join(", %s %s" % (prop_ctype(t), n) for n, t in params)
    proto = "lv_obj_t * %s_create(lv_obj_t * parent%s)" % (comp.name, sig_params)
    body = [
        "/**", " * @file %s_gen.c" % comp.name, " * @brief Template source file for LVGL objects",
        " */", "", "/*********************", " *      INCLUDES", " *********************/", "",
        '#include "%s_gen.h"' % comp.name,
        '#include "%s/%s.h"' % (rel_to_root(comp.rel_dir), p.name), "",
        "/**********************", " *   GLOBAL FUNCTIONS", " **********************/", "",
        proto, "{", '    LV_TRACE_OBJ_CREATE("begin");', "",
    ]
    body += style_block(p, f, comp.styles, "    ", True, comp.name, cp)
    body += ["", "    lv_obj_t * the_root = NULL;", "",
             "    #if %s_CHECK_COMPILE_TARGET(%s_TARGET_ALL)" % (p.NAME, p.NAME),
             "    if (%s_check_target(%s_TARGET_ALL)) {" % (p.name, p.NAME)]
    body += cp.add(f, tidy(f.lines), root)
    body += ["", "        the_root = %s;" % root, "    }", "    #endif", "",
             '    LV_TRACE_OBJ_CREATE("finished");', "", "    return the_root;", "}", ""]
    header = [
        "/**", " * @file %s_gen.h" % comp.name, " */", "",
        "#ifndef %s_H" % comp.name.upper(), "#define %s_H" % comp.name.upper(), "",
        "#ifdef __cplusplus", 'extern "C" {', "#endif", "", HEADER_INCLUDES, "",
        proto + ";", "", "#ifdef __cplusplus", '} /*extern "C"*/', "#endif", "",
        "#endif /*%s_H*/" % comp.name.upper(), ""]
    return "\n".join(header), body


def gen_screen(p, scr, cp):
    f = Func(p, [], [], scr.name)
    view = scr.view
    if view.get("extends", "lv_obj") != "lv_obj":
        raise GenError("%s: screens must extend lv_obj" % scr.name)
    f.names.add(scr.name)
    root = "lv_obj_0"
    f.vars.add(root)
    f.counters["lv_obj"] = 1
    if scr.permanent:
        f.emit("if (%s == NULL) %s = lv_obj_create(NULL);" % (scr.name, scr.name))
        f.emit("lv_obj_t * %s = %s;" % (root, scr.name))
    else:
        f.emit("lv_obj_t * %s = lv_obj_create(NULL);" % root)
    f.emit("lv_obj_set_name_static(%s, %s);" % (root, c_string(scr.name + "_#")))
    f.attrs(root, "lv_obj", view, {"extends"})
    f.emit()
    f.children(root, view)
    proto = "lv_obj_t * %s_create(void)" % scr.name
    body = [
        "/**", " * @file %s_gen.c" % scr.name, " * @brief Template source file for LVGL objects",
        " */", "", "/*********************", " *      INCLUDES", " *********************/", "",
        '#include "%s_gen.h"' % scr.name, '#include "../%s.h"' % p.name, "",
        "/**********************", " *   GLOBAL FUNCTIONS", " **********************/", "",
        proto, "{", '    LV_TRACE_OBJ_CREATE("begin");', "", "    lv_obj_t * the_root = NULL;", "",
        "    #if %s_CHECK_COMPILE_TARGET(%s_TARGET_ALL)" % (p.NAME, p.NAME),
        "    if (%s_check_target(%s_TARGET_ALL)) {" % (p.name, p.NAME)]
    body += cp.add(f, tidy(f.lines), root)
    body += ["", "        the_root = %s;" % root, "    }", "    #endif", "",
             '    LV_TRACE_OBJ_CREATE("finished");', "", "    return the_root;", "}", ""]
    header = [
        "/**", " * @file %s_gen.h" % scr.name, " */", "",
        "#ifndef %s_H" % scr.name.upper(), "#define %s_H" % scr.name.upper(), "",
        "#ifdef __cplusplus", 'extern "C" {', "#endif", "", HEADER_INCLUDES, "",
        proto + ";", "", "#ifdef __cplusplus", '} /*extern "C"*/', "#endif", "",
        "#endif /*%s_H*/" % scr.name.upper(), ""]
    return "\n".join(header), body


def gen_project(p, cp):
    N, n = p.NAME, p.name
    f = Func(p, [], [s[0] for s in p.styles], "globals.xml")
    h = ["/**", " * @file %s_gen.h" % n, " */", "", "#ifndef %s_GEN_H" % N,
         "#define %s_GEN_H" % N, "", "#ifndef UI_SUBJECT_STRING_LENGTH",
         "#define UI_SUBJECT_STRING_LENGTH 256", "#endif", "", "#ifdef __cplusplus",
         'extern "C" {', "#endif", "", HEADER_INCLUDES, "",
         "/* Prototypes for target functions, needed by responsive const definitions */", "",
         "void %s_set_target(uint32_t target);" % n, "uint32_t %s_get_target(void);" % n,
         "bool %s_check_target(uint32_t target);" % n, "",
         "/*********************", " *      DEFINES", " *********************/", "",
         "#define %s_TARGET_UNDEFINED  (0 << 1)" % N]
    for i, t in enumerate(p.targets):
        h.append("#define %s_TARGET_%s    (1 << %d)" % (N, t.upper(), i + 1))
    h += ["#define %s_TARGET_ALL        0x0FFFFFFF" % N, "",
          "/* By default compile for all targets, allowing to switch to any targets at runtime */",
          "#ifndef %s_COMPILE_TARGET" % N,
          "#define %s_COMPILE_TARGET %s_TARGET_ALL" % (N, N), "#endif", "",
          "#define %s_CHECK_COMPILE_TARGET(target) (%s_COMPILE_TARGET & (target) ? 1 : 0)" % (N, N),
          ""]
    h += theme_header(p)
    for name, (typ, val, hlp) in p.consts.items():
        if hlp:
            h += ["/**", " * " + hlp, " */"]
        ctyp = {"int": "int", "color": "color", "opa": "opa"}.get(typ)
        if ctyp is None:
            raise GenError("const '%s': type <%s> not supported" % (name, typ))
        if typ == "color":  # the current palette's (themes.xml)
            h.append("#define %s (%s_palette[%s])" % (name.upper(), n, color_enum(p, name)))
            continue
        h.append("#define %s %s" % (name.upper(), f.value(val, ctyp)))
    h += ["", "/**********************", " * GLOBAL VARIABLES", " **********************/", "",
          "/*-------------------", " * Permanent screens", " *------------------*/", ""]
    for s in p.screens:
        if s.permanent:
            h.append("extern lv_obj_t * %s;" % s.name)
    h += ["", "/*----------------", " * Global styles", " *----------------*/", ""]
    for s in p.styles:
        h.append("extern lv_style_t %s;" % s[0])
    h += ["", "/*----------------", " * Fonts", " *----------------*/", "", "/* Targets: any */"]
    for fo in p.fonts:
        h.append("extern lv_font_t * %s;" % fo["name"])
    if letters_fonts(p):
        h += ["", "/* Fingerprint of the letters pack's fonts: a pack whose `match` equals it was made",
              " * from these fonts (tools/lvpro_gen.py, docs/features/39 OPT-010) */",
              "extern const uint32_t alarm_ui_letters_fingerprint;"]
    h += ["", "/*----------------", " * Subjects", " *----------------*/", ""]
    for _, name, _, _ in p.subjects:
        h.append("extern lv_subject_t %s;" % name)
    h += ["", "/**********************", " * GLOBAL PROTOTYPES", " **********************/", "",
          "/*----------------", " * Event Callbacks", " *----------------*/", ""]
    for cb in p.callbacks:
        h.append("void %s(lv_event_t * e);" % cb)
    h += ["", "/**", " * Initialize the component library", " */", "",
          "void %s_init_gen(const char * asset_path);" % n, "",
          "/**", " * Text for people in the XML goes through this lookup (the portable UI defines it)",
          " */", "", "const char * alarm_ui_tr(const char * english);",
          "const char * alarm_ui_tr_c(const char * ctx, const char * english);", "",
          "/**********************", " *   POST INCLUDES", " **********************/", "",
          "/*Include all the widgets, components and screens of this library*/"]
    h.append('#include "%s_runs_gen.h"' % n)
    for comp in sorted(p.components.values(), key=lambda c: c.rel_dir):
        h.append('#include "%s/%s_gen.h"' % (comp.rel_dir, comp.name))
    for s in p.screens:
        h.append('#include "%s/%s_gen.h"' % (s.rel_dir, s.name))
    h += ["", "#ifdef __cplusplus", '} /*extern "C"*/', "#endif", "",
          "#endif /*%s_GEN_H*/" % N, ""]

    c = ["/**", " * @file %s_gen.c" % n, " */", "", '#include "%s_gen.h"' % n, "",
         "/**********************", " *  STATIC VARIABLES", " **********************/", "",
         "static uint32_t %s_target = %s_TARGET_ALL;" % (n, N), "",
         "/**********************", " *  GLOBAL VARIABLES", " **********************/", "",
         "/*--------------------", " *  Permanent screens", " *-------------------*/", ""]
    for s in p.screens:
        if s.permanent:
            c.append("lv_obj_t * %s = NULL;" % s.name)
    c += ["", "/*----------------", " * Fonts", " *----------------*/", ""]
    for fo in p.fonts:
        c.append("lv_font_t * %s;" % fo["name"])
        c.append("extern %slv_font_t %s_data;" % ("" if font_is_text(fo) else "const ", fo["name"]))
    if letters_fonts(p):
        c += ["", "const uint32_t alarm_ui_letters_fingerprint = 0x%08xu;" % letters_fingerprint(p)]
    c += ["", "/*----------------", " * Global styles", " *----------------*/", ""]
    for s in p.styles:
        c.append("lv_style_t %s;" % s[0])
    c += ["", "/*----------------", " * Subjects", " *----------------*/", ""]
    for _, name, _, _ in p.subjects:
        c.append("lv_subject_t %s;" % name)
    c += theme_tables(p)
    c += ["", "/**********************", " *   GLOBAL FUNCTIONS", " **********************/", "",
          "void %s_init_gen(const char * asset_path)" % n, "{", "    LV_UNUSED(asset_path);", ""]
    theme = p.theme
    if theme == "simple":
        c += ["    /* When running from the editor the theme set from the XML should overwrite this */",
              "#if !defined(LV_EDITOR_PREVIEW)", "#if LV_USE_THEME_SIMPLE",
              "    lv_display_t * disp = lv_display_get_default();",
              "    lv_theme_t * th = lv_theme_simple_init(disp);",
              "    lv_display_set_theme(disp, th);", "#else",
              '    LV_LOG_WARN("Simple theme is selected in project.xml but LV_USE_THEME_SIMPLE is disabled");',
              "#endif", "#endif /*LV_EDITOR_PREVIEW*/", ""]
    c += ["    /*----------------", "     * Fonts", "     *----------------*/", ""]
    for fo in p.fonts:
        c += ["    /* get font '%s' from a C array */" % fo["name"],
              "    %s = (lv_font_t *)&%s_data;" % (fo["name"], fo["name"])]
    c += ["", "    /*----------------", "     * Global styles", "     *----------------*/", ""]
    c += style_block(p, f, p.styles, "    ", False, None, cp)
    c += ["    /*----------------", "     * Subjects", "     *----------------*/"]
    for typ, name, val, attrib in p.subjects:
        if typ == "int":
            c.append("    lv_subject_init_int(&%s, %s);" % (name, f.value(val or "0", "int")))
            if "min_value" in attrib:
                c.append("    lv_subject_set_min_value_int(&%s, %s);" % (
                    name, f.value(attrib["min_value"], "int")))
            if "max_value" in attrib:
                c.append("    lv_subject_set_max_value_int(&%s, %s);" % (
                    name, f.value(attrib["max_value"], "int")))
        elif typ == "color":
            c.append("    lv_subject_init_color(&%s, %s);" % (name, f.value(val, "color")))
        else:
            c += ["    static char %s_buf[UI_SUBJECT_STRING_LENGTH];" % name,
                  "    static char %s_prev_buf[UI_SUBJECT_STRING_LENGTH];" % name,
                  "    lv_subject_init_string(&%s," % name,
                  "                           %s_buf," % name,
                  "                           %s_prev_buf," % name,
                  "                           UI_SUBJECT_STRING_LENGTH,",
                  "                           %s" % tr_string(val or ""), "                          );"]
    c += ["", "#if defined(LV_USE_XML) && LV_USE_XML", "    /* Register fonts */"]
    for fo in p.fonts:
        c.append('    lv_xml_register_font(NULL, "%s", %s);' % (fo["name"], fo["name"]))
    c.append("")
    c.append("    /* Register subjects */")
    for _, name, _, _ in p.subjects:
        c.append('    lv_xml_register_subject(NULL, "%s", &%s);' % (name, name))
    c += ["#endif", "", "#if defined(LV_USE_XML) && LV_USE_XML == 0",
          "    /*--------------------", "     *  Permanent screens", "     *-------------------*/",
          "    /* To allow screens to reference each other, create them all before calling the sceen create functions */"]
    for s in p.screens:
        if s.permanent:
            c.append("    %s = lv_obj_create(NULL);" % s.name)
    c.append("")
    for s in p.screens:
        if s.permanent:
            c.append("    %s_create();" % s.name)
    c += ["#endif", "}", "",
          "void %s_set_target(uint32_t target)" % n, "{", "    %s_target = target;" % n, "}", "",
          "uint32_t %s_get_target(void)" % n, "{", "    return %s_target;" % n, "}", "",
          "bool %s_check_target(uint32_t target)" % n, "{",
          "    return (%s_target & target) ? true : false;" % n, "}", ""]
    return "\n".join(h), c


def theme_header(p):
    """The palette, theme and style index names, and the tables' declarations."""
    N, n = p.NAME, p.name
    h = ["/*----------------", " * Appearance (themes.xml): COLOR_* read the current palette",
         " *----------------*/", "",
         "#define %s_THEMES 1 /* the tables below are here (tools/lvpro_gen.py, not the editor) */" % N, "",
         "typedef enum {"]
    h += ["    %s%s," % (color_enum(p, c), " = 0" if i == 0 else "") for i, c in enumerate(p.colors)]
    h += ["    %s_COLOR_COUNT" % N, "} %s_color_t;" % n, "", "typedef enum {"]
    h += ["    %s_THEME_%s%s, /* %s */" % (N, t.name.upper(), " = 0" if i == 0 else "", t.title)
          for i, t in enumerate(p.themes)]
    h += ["    %s_THEME_COUNT" % N, "} %s_theme_t;" % n, "", "typedef enum {"]
    ids = list(p.style_ids.values())
    h += ["    %s%s," % (sid, " = 0" if i == 0 else "") for i, sid in enumerate(ids)]
    h += ["    %s_STYLE_COUNT" % N, "} %s_style_id_t;" % n, "",
          "typedef enum {",
          "    %s_VALUE_NONE = 0, /* the style has none: the property is removed */" % N,
          "    %s_VALUE_INT,      /* a number */" % N,
          "    %s_VALUE_COLOR,    /* a palette index: %s_COLOR_* */" % (N, N),
          "    %s_VALUE_HEX,      /* a fixed colour, 0xRRGGBB */" % N,
          "} %s_value_kind_t;" % n, "",
          "/* A property a style takes in some themes only (themes.xml's <styles themes=...>) */",
          "typedef struct {",
          "    uint16_t style;        /* %s_STYLE_* */" % N,
          "    lv_style_prop_t prop;",
          "    uint8_t themes;        /* 1 << %s_THEME_* of each theme it is in */" % N,
          "    uint8_t kind;          /* %s_value_kind_t */" % n,
          "    uint8_t base_kind;     /* the style's own value, in the other themes */",
          "    int32_t value;",
          "    int32_t base;",
          "} %s_style_override_t;" % n, "",
          "extern const lv_color_t %s_palettes[%s_THEME_COUNT][%s_COLOR_COUNT];" % (n, N, N),
          "extern const lv_color_t * %s_palette; /* the current theme's */" % n,
          "extern const uint8_t %s_theme_saver[%s_THEME_COUNT]; /* the theme the screen saver keeps */" % (n, N),
          "extern const bool %s_theme_high_contrast[%s_THEME_COUNT];" % (n, N),
          "extern lv_style_t * %s_styles[%s_STYLE_COUNT]; /* each style, once it is initialised */" % (n, N),
          "extern const %s_style_override_t %s_style_overrides[];" % (n, n),
          "extern const uint32_t %s_style_override_count;" % n,
          "extern lv_subject_t * const %s_color_subjects[];" % n,
          "extern const uint32_t %s_color_subject_count;" % n, "",
          "/* In the hand-written %s.c: a style joins the registry and takes the theme's changes */" % n,
          "void %s_style_track(lv_style_t * style, uint32_t id);" % n, ""]
    return h


def rgb(v):
    return "LV_COLOR_MAKE(0x%02X, 0x%02X, 0x%02X)" % (v >> 16, (v >> 8) & 0xFF, v & 0xFF)


def theme_tables(p):
    N, n = p.NAME, p.name
    c = ["", "/*----------------", " * Appearance (themes.xml)", " *----------------*/", "",
         "/* A value unlike themes.xml's (the comment) is nudged in the bits RGB565 drops, so each",
         " * value stands for one colour (tools/lvpro_gen.py): the screen shows the same. */",
         "const lv_color_t %s_palettes[%s_THEME_COUNT][%s_COLOR_COUNT] = {" % (n, N, N)]
    for t in p.themes:
        c.append("    [%s_THEME_%s] = {" % (N, t.name.upper()))
        for name in p.colors:
            v, sv = t.colors[name], t.stored[name]
            c.append("        [%s] = %s,%s" % (color_enum(p, name), rgb(sv),
                                              " /* 0x%06X */" % v if sv != v else ""))
        c.append("    },")
    c += ["};", "", "const lv_color_t * %s_palette = %s_palettes[0];" % (n, n), "",
          "const uint8_t %s_theme_saver[%s_THEME_COUNT] = {%s};" % (n, N, ", ".join(
              "%s_THEME_%s" % (N, t.saver.upper()) for t in p.themes)),
          "const bool %s_theme_high_contrast[%s_THEME_COUNT] = {%s};" % (n, N, ", ".join(
              "true" if t.high else "false" for t in p.themes)), "",
          "lv_style_t * %s_styles[%s_STYLE_COUNT];" % (n, N), "",
          "const %s_style_override_t %s_style_overrides[] = {" % (n, n)]
    for mask, sid, prop, kind, val, bkind, bval in p.style_overrides:
        c.append("    {%s, %s, 0x%02X, %s_VALUE_%s, %s_VALUE_%s, %s, %s}," % (
            sid, prop, mask, N, kind, N, bkind, val, bval))
    if not p.style_overrides:
        c.append("    {0},")
    c += ["};", "",
          "const uint32_t %s_style_override_count = %d;" % (n, len(p.style_overrides)), "",
          "lv_subject_t * const %s_color_subjects[] = {" % n]
    colors = [name for typ, name, _, _ in p.subjects if typ == "color"]
    c += ["    &%s," % s for s in colors] or ["    NULL,"]
    c += ["};", "", "const uint32_t %s_color_subject_count = %d;" % (n, len(colors))]
    return c


CUSTOM_H = """/**
 * @file {n}.h
 */

#ifndef {N}_H
#define {N}_H

#ifdef __cplusplus
extern "C" {{
#endif

#include "{n}_gen.h"

/**
 * Initialize the component library
 */
void {n}_init(const char * asset_path);

#ifdef __cplusplus
}} /*extern "C"*/
#endif

#endif /*{N}_H*/
"""

CUSTOM_C = """/**
 * @file {n}.c
 */

#include "{n}.h"

void {n}_init(const char * asset_path)
{{
    {n}_init_gen(asset_path);

    /* Add your own custom code here if needed */
}}
"""

CMAKELISTS = """# Generated file, do not edit.
# This file is used to compile the UI Project created by the Editor.
# See `user_config.cmake` as a way to add your own source files here
# Only set project if this is the top-level CMakeLists.txt
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.10)
  # can be customized
  project(LVGLProject)
  set(IS_TOP_LEVEL TRUE)
else()
  set(IS_TOP_LEVEL FALSE)
endif()
# Handles giving component libraries unique names when included as subprojects.
if(DEFINED LVGL_COMPONENT_LIB_NAME)
  set(LIB_NAME ${LVGL_COMPONENT_LIB_NAME})
else()
  set(LIB_NAME lib-ui)
endif()
# Include user source files
include(${CMAKE_CURRENT_LIST_DIR}/user_config.cmake)
set(LV_EDITOR_USER_SOURCES ${LV_EDITOR_PROJECT_SOURCES})
# Reset project sources
set(LV_EDITOR_PROJECT_SOURCES)
# Include any component libraries
include(${CMAKE_CURRENT_LIST_DIR}/component_lib_list_gen.cmake)
# Include generated list of .c files
include(${CMAKE_CURRENT_LIST_DIR}/file_list_gen.cmake)
# Create the UI sources as a library
add_library(${LIB_NAME} ${LV_EDITOR_PROJECT_SOURCES})
if(LV_EDITOR_USER_SOURCES)
  add_library(${LIB_NAME}_user OBJECT ${LV_EDITOR_USER_SOURCES})
  target_link_libraries(${LIB_NAME} PUBLIC $<TARGET_OBJECTS:${LIB_NAME}_user>)
endif()
if(DEFINED LV_EDITOR_COMPONENT_LIB_LIST)
  target_link_libraries(${LIB_NAME} PUBLIC ${LV_EDITOR_COMPONENT_LIB_LIST})
endif()
if(TARGET lvgl)
  target_link_libraries(${LIB_NAME} PUBLIC lvgl)
  if(LV_EDITOR_USER_SOURCES)
    target_link_libraries(${LIB_NAME}_user PUBLIC lvgl)
  endif()
endif()
# Add the project folder as include folder
target_include_directories(${LIB_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Set include directories for the project This will include the LVGL includes
target_include_directories(${LIB_NAME} PRIVATE ${COMMON_INCLUDE_DIRS})
"""

USER_CONFIG = """# =============================================================================
# user_config.cmake - Add your custom source files here
# =============================================================================
#
# This file is included by the generated CMakeLists.txt and allows you to
# add extra source files to the project without modifying generated files
# (which may be overwritten).
#
# =============================================================================

list(APPEND LV_EDITOR_PROJECT_SOURCES
    # ${CMAKE_CURRENT_LIST_DIR}/my_file.c
)
"""


# --------------------------------------------------------------------------------------------
# Fonts
# --------------------------------------------------------------------------------------------

def font_conv_cmd():
    env = os.environ.get("LV_FONT_CONV")
    if env:
        return shlex.split(env, posix=(os.name != "nt"))
    exe = shutil.which("lv_font_conv")
    if exe:
        return [exe]
    npx = shutil.which("npx")
    if npx:
        return [npx, "--yes", "lv_font_conv@1.5.3"]
    raise GenError("lv_font_conv not found: set LV_FONT_CONV or install Node.js")


# The five text fonts are not const: a language pack's font is hung behind each as its `fallback`
# at run time (docs/features/37 I18N-049), so LVGL-generated `const lv_font_t` becomes a plain
# definition (and every declaration of these five is non-const too). The data does not change.
TEXT_FONTS = ("font_caption", "font_body", "font_label", "font_title", "font_headline")


def font_is_text(fo):
    return fo["name"] in TEXT_FONTS


FONT_CONST_RE = re.compile(
    r"#if LVGL_VERSION_MAJOR >= 8\nconst lv_font_t (\w+) = \{\n#else\nlv_font_t \1 = \{\n#endif\n")
FONT_MUTABLE_NOTE = ("/* Not const: a language pack's font is set as this font's `fallback` while the\n"
                     " * language is on (tools/lvpro_gen.py, docs/features/37). */\n")


def font_make_mutable(text):
    """lv_font_conv's const-or-not definition as one non-const definition (idempotent)."""
    return FONT_CONST_RE.sub(
        lambda m: FONT_MUTABLE_NOTE + "lv_font_t %s = {\n" % m.group(1), text)


def fix_text_fonts(p, check):
    """Make the text fonts' C files non-const; returns the files that were (or would be) changed."""
    changed = []
    for fo in p.fonts:
        if not font_is_text(fo):
            continue
        path = p.dir / "fonts" / ("%s_data.c" % fo["name"])
        if not path.exists():
            continue
        old = path.read_bytes().decode("utf-8")
        new = font_make_mutable(old)
        if new != old:
            changed.append("fonts/%s_data.c" % fo["name"])
            if not check:
                path.write_bytes(new.encode("utf-8"))
    return changed


# The big display fonts: the headline (the panel's state, a tile's title) and the clock digits.
RLE_TEXT_FONTS = ("font_headline", "font_digits")


def font_compressed(fo):
    """Icon fonts (icons_*) and the big display fonts (RLE_TEXT_FONTS) are RLE-compressed: they
    are most of the fonts' flash, shrink by half or more, and are drawn a few glyphs at a time,
    when what they show changes (docs/features/39 F2). The other text fonts stay plain: a line of
    body text unpacks every glyph on each draw. Compression is lossless (the pictures don't
    change) and needs LV_USE_FONT_COMPRESSED in every build (the firmware's sdkconfig.defaults,
    host/ui_preview/lv_conf.h)."""
    return fo["name"].startswith("icons_") or fo["name"] in RLE_TEXT_FONTS


def font_ranges(spec):
    """The code points a range attribute ("0x20-0x7E,0xA0-0x17F") covers."""
    points = set()
    for part in filter(None, (s.strip() for s in (spec or "").split(","))):
        lo, _, hi = part.partition("-")
        points.update(range(int(lo, 0), int(hi or lo, 0) + 1))
    return points


# The characters the translation catalogs use (tools/i18n.py build, docs/features/37): a text
# font carries every one of them, so a new translation can't miss a glyph (I18N-011).
I18N_CHARS = Path(__file__).resolve().parent.parent / "components/core/i18n/catalogs/i18n_chars.txt"


def font_symbols(fo):
    """A text font's (one that covers ASCII) symbols, plus the catalogs' characters it lacks."""
    symbols = fo.get("symbols") or ""
    covered = font_ranges(fo.get("range"))
    if 0x41 not in covered or not I18N_CHARS.exists():
        return symbols
    extra = [ch for ch in I18N_CHARS.read_text(encoding="utf-8").strip("\n")
             if ord(ch) not in covered and ch not in symbols]
    return symbols + "".join(extra)


# --------------------------------------------------------------------------------------------
# The letters pack's fonts (docs/features/39 OPT-010, OPT-014, asset-packs.md "The kinds")
#
# A text font with a `letters` attribute is built into the image with its `range` only (ASCII, its
# symbols and the catalogs' characters). The other code points of `letters` are made once more as a
# binary font (fonts/letters/<font name>.bin), which the asset-packs builder packs into the letters
# pack and the firmware hangs behind the font as its fallback. letters.json lists the files and the
# fingerprint of what they were made from; the same fingerprint is compiled into the image
# (alarm_ui_letters_fingerprint) so the firmware can tell a pack that matches the fonts.
# --------------------------------------------------------------------------------------------

LETTERS_DIR = "fonts/letters"
FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv1a32(data):
    """32-bit FNV-1a (the constants of the i18n hash)."""
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def letters_set(fo):
    """The code points a text font's letters font holds: its `letters` attribute without what the
    font in the image already carries (its range, its symbols, the catalogs' characters)."""
    have = font_ranges(fo.get("range")) | {ord(ch) for ch in font_symbols(fo)}
    return font_ranges(fo.get("letters")) - have


def ranges_text(points):
    """Sorted code points as a comma list of hex ranges: "0xA0-0xA3,0xA5"."""
    out, run = [], []
    for cp in sorted(points) + [None]:
        if run and (cp is None or cp != run[-1] + 1):
            out.append("0x%X" % run[0] if len(run) == 1 else "0x%X-0x%X" % (run[0], run[-1]))
            run = []
        if cp is not None:
            run.append(cp)
    return ",".join(out)


def letters_fonts(p):
    return [fo for fo in p.fonts if fo.get("letters")]


def letters_role(fo):
    return fo["name"][len("font_"):] if fo["name"].startswith("font_") else fo["name"]


def font_ttf_hash(p, fo):
    path = p.dir / fo["src_path"]
    if not path.exists():
        raise GenError("font '%s': %s not found" % (fo["name"], fo["src_path"]))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def letters_fingerprint(p):
    """The fingerprint of the letters fonts: FNV-1a over one line per font, in globals.xml order:
    name|SHA-256 of the TTF|size|bpp|compressed 0/1|the letters' ranges."""
    lines = "".join("%s|%s|%s|%s|%d|%s\n" % (
        fo["name"], font_ttf_hash(p, fo), fo["size"], fo.get("bpp", "4"),
        1 if font_compressed(fo) else 0, ranges_text(letters_set(fo))) for fo in letters_fonts(p))
    return fnv1a32(lines.encode("utf-8"))


# A text font's line - its height and baseline - is measured over the code points of its `line`
# attribute, not only over the glyphs the image carries: the layout stays the same whether a letter
# comes from the image's font or from a font behind it (the letters pack, a language's font), and
# such a letter fits its line (docs/features/39 OPT-010). lv_font_conv measures a font's line from
# the glyphs it converts, so the generator converts the `line` set once more, to a scratch binary
# font, reads its ascent and descent, and writes them into the C font.
LINE_NOTE = "/*Measured over the font's `line` code points (tools/lvpro_gen.py)*/"
LINE_RE = re.compile(r"(\.line_height = )(-?\d+),[^\n]*\n(\s*\.base_line = )(-?\d+),")


def font_line_metrics(p, fo, cmd):
    """(line height, baseline) of the font converted with its `line` and `range` code points and
    its symbols."""
    points = font_ranges(fo.get("line")) | font_ranges(fo.get("range"))
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "line.bin"
        args = cmd + ["--font", fo["src_path"], "-o", str(out), "--size", fo["size"],
                      "--bpp", fo.get("bpp", "4"), "--format", "bin", "--no-compress",
                      "--range", ranges_text(points)]
        symbols = font_symbols(fo)
        if symbols:
            args += ["--symbols", symbols]
        subprocess.run(args, cwd=p.dir, check=True, capture_output=True)
        data = out.read_bytes()
    if len(data) < 20 or data[4:8] != b"head":
        raise GenError("font '%s': lv_font_conv's binary font has no head" % fo["name"])
    ascent, descent = struct.unpack_from("<Hh", data, 16)
    return ascent - descent, -descent


def font_set_line(text, line_height, base_line):
    """The C font with the line of its `line` set, or of its own glyphs if that is taller."""
    m = LINE_RE.search(text)
    if m is None:
        raise GenError("no .line_height/.base_line in a converted font")
    own_height, own_base = int(m.group(2)), int(m.group(4))
    base = max(base_line, own_base)
    height = max(line_height - base_line, own_height - own_base) + base
    return text[:m.start()] + "%s%d,          %s\n%s%d," % (
        m.group(1), height, LINE_NOTE, m.group(3), base) + text[m.end():]


def letters_json(p):
    return json.dumps({"format": 1, "fingerprint": "0x%08x" % letters_fingerprint(p),
                       "fonts": {letters_role(fo): "%s.bin" % fo["name"]
                                 for fo in letters_fonts(p)}}, indent=2) + "\n"


def gen_fonts(p, force):
    cmd = font_conv_cmd()
    # A font is stale when its TTF, globals.xml, the catalogs' characters or this generator
    # changed after it.
    newest_input = max((p.dir / "globals.xml").stat().st_mtime, Path(__file__).stat().st_mtime,
                       I18N_CHARS.stat().st_mtime if I18N_CHARS.exists() else 0)
    for fo in p.fonts:
        out = p.dir / "fonts" / ("%s_data.c" % fo["name"])
        src = p.dir / fo["src_path"]
        if not src.exists():
            raise GenError("font '%s': %s not found" % (fo["name"], fo["src_path"]))
        bin_out = p.dir / LETTERS_DIR / ("%s.bin" % fo["name"])
        if (not force and out.exists() and out.stat().st_mtime > src.stat().st_mtime
                and out.stat().st_mtime > newest_input
                and (not fo.get("letters") or (bin_out.exists()
                                               and bin_out.stat().st_mtime > out.stat().st_mtime))):
            continue
        args = cmd + ["--font", fo["src_path"], "-o", "fonts/%s_data.c" % fo["name"],
                      "--size", fo["size"], "--bpp", fo.get("bpp", "4"), "--format", "lvgl"]
        if not font_compressed(fo):
            args.append("--no-compress")
        symbols = font_symbols(fo)
        if symbols:
            args += ["--symbols", symbols]
        if fo.get("range"):
            args += ["--range", fo["range"]]
        if fo.get("lv_fallback"):
            args += ["--lv-fallback", fo["lv_fallback"] + "_data"]
        print("  font %s (%s px)" % (fo["name"], fo["size"]))
        subprocess.run(args, cwd=p.dir, check=True)
        if fo.get("line"):
            height, base = font_line_metrics(p, fo, cmd)
            out.write_bytes(font_set_line(out.read_bytes().decode("utf-8"), height, base).encode("utf-8"))
        if fo.get("letters"):
            points = letters_set(fo)
            if points:
                bin_out.parent.mkdir(parents=True, exist_ok=True)
                args = cmd + ["--font", fo["src_path"], "-o", "%s/%s.bin" % (LETTERS_DIR, fo["name"]),
                              "--size", fo["size"], "--bpp", fo.get("bpp", "4"), "--format", "bin",
                              "--no-compress", "--range", ranges_text(points)]
                print("  letters %s (%d code points)" % (fo["name"], len(points)))
                subprocess.run(args, cwd=p.dir, check=True)
    fix_text_fonts(p, False)


# --------------------------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------------------------

# --------------------------------------------------------------------------------------------
# Compact output (BL-125 / docs/features/39 F10)
#
# The statements of one object ("create, name, a few setters") repeat from object to object.
# The Compactor finds the runs that repeat, in every function of a layout, and turns each into
# one shared function (<project>_runs_gen.c); a run that differs only in its numbers, colours,
# fonts or texts takes them as parameters. The calls and their order stay exactly as the XML gave
# them, so a screen is built as before. Names (lv_obj_set_name) are set only where the hand-written
# code looks an object up by that name.
# --------------------------------------------------------------------------------------------

CODE_FILES = [
    "components/ui/alarm_ui/src/*.c", "components/ui/alarm_ui/src/*.h",
    "components/ui/alarm_ui/include/*.h", "host/ui_preview/*.c", "host/ui_preview/*.h",
    "host/ui_sim/*.c",
]
STRING_LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
CODE_COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
WORD = re.compile(r"[A-Za-z0-9_]+")


def repo_root(start):
    for up in Path(start).resolve().parents:
        if (up / "tools" / "check_ui_contract.py").is_file():
            return up
    return None


def code_words(project_dir):
    """Every word inside a string literal of the hand-written code (and of the layout's own
    alarm_ui.c): the names it can look an object up by. None when the code isn't there (a test's
    project outside the repository): every name stays."""
    root = repo_root(project_dir)
    if root is None:
        return None
    files = []
    for pattern in CODE_FILES:
        files += sorted(root.glob(pattern))
    files.append(Path(project_dir) / "alarm_ui.c")
    words = set()
    for file in files:
        if not file.is_file():
            continue
        text = CODE_COMMENTS.sub(" ", file.read_text(encoding="utf-8", errors="replace"))
        for lit in STRING_LITERAL.findall(text):
            words.update(WORD.findall(lit))
    return words


def name_is_looked_up(name, words):
    """Does the code know this object by its name? A name ending in `#` is numbered by LVGL:
    its stem counts. A word ending in `_` is the start of a name built at run time."""
    if words is None:
        return True
    stem = name[:-1] if name.endswith("#") else name
    stem = stem.rstrip("_")
    if stem in words or name in words:
        return True
    return any(w.endswith("_") and (stem + "_").startswith(w) for w in words)


PARAM_KINDS = ("int", "color", "font", "str", "tr", "sty", "lsty", "subj", "cb")
ALWAYS_PARAM = ("tr", "lsty")  # texts to translate (kept in order); a style that is local to its function
PARAM_CTYPE = {"int": "int32_t", "color": "lv_color_t", "font": "const lv_font_t *",
               "str": "const char *", "tr": "const char *", "sty": "lv_style_t *", "lsty": "lv_style_t *",
               "subj": "lv_subject_t *", "cb": "lv_event_cb_t"}
INT_ARG = re.compile(r"-?\d+|\(255 \* \d+ / 100\)|lv_pct\(-?\d+\)|true|false|"
                     r"[A-Z_][A-Z0-9_]*(?: \| [A-Z_][A-Z0-9_]*)*")
COLOR_ARG = re.compile(r"lv_color_hex3?\(0x[0-9A-F]+\)|LV_COLOR_MAKE\(.*\)")
CREATE_LINE = re.compile(r"^        lv_obj_t \* (\w+) = (\w+)\((.*)\);$")
DECL_LINE = re.compile(r"^        lv_obj_t \* (\w+) = ")
CALL_LINE = re.compile(r"^        (\w+)\((.*)\);$")
CALL_NAME = re.compile(r"^        lv_obj_set_name(?:_static)?\((\w+), (\".*\")\);$")
MAX_RUN = 14
MIN_GAIN = 24  # estimated bytes of code a shared function has to save
COMPACT = True   # False: every statement stays where it is (the tests compare the two)


def split_args(text):
    """Top-level comma separated arguments (strings and brackets respected)."""
    out, depth, cur, i = [], 0, "", 0
    while i < len(text):
        ch = text[i]
        if ch == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            cur += text[i:j + 1]
            i = j + 1
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


class El:
    """One statement of a run: a create (`lv_obj_t * v = f(parent, ...)`) or a call on v."""
    __slots__ = ("fn", "args", "kinds", "var", "create", "line")

    def __init__(self, fn, args, kinds, var, create, line):
        self.fn, self.args, self.kinds, self.var, self.create, self.line = (
            fn, args, kinds, var, create, line)


class Helper:
    def __init__(self, index, key, occs, slots):
        self.index, self.key, self.occs, self.slots = index, key, occs, slots
        self.create = occs[0][0].create
        self.style = occs[0][0].fn.startswith("lv_style_")


class Compactor:
    def __init__(self, project):
        self.p = project
        self.words = code_words(project.dir)
        self.funcs = []     # (Func, lines, root var)
        self.result = []    # the new lines of each function
        self.helpers = []
        self.colors = {n.upper() for n, (t, _, _) in project.consts.items() if t == "color"}
        self.solved = False

    # ---- collecting ----
    def add(self, f, lines, root):
        self.funcs.append((f, lines, root))
        return ["@@BODY %d@@" % (len(self.funcs) - 1)]

    def render(self, body):
        out = []
        for line in body:
            m = re.fullmatch(r"@@BODY (\d+)@@", line)
            out += self.result[int(m.group(1))] if m else [line]
        return out

    # ---- classifying ----
    def kind_of(self, text, f=None):
        if "alarm_ui_tr" in text:
            return "tr"
        if text.startswith('"'):
            return "str"
        if text.startswith("&") and f is not None:
            if text[1:] in f.local_styles:
                return "lsty"
            if text[1:] in self.p.global_style_names:
                return "sty"
            if text[1:] in self.p.subject_names:
                return "subj"
        if text in self.p.callbacks:
            return "cb"
        if text in self.colors or COLOR_ARG.fullmatch(text):
            return "color"
        if text in self.p.font_names or re.fullmatch(r"&lv_font_montserrat_\d+", text):
            return "font"
        if text != "NULL" and INT_ARG.fullmatch(text):
            return "int"
        return "other"

    def is_local(self, f, text):
        if self.kind_of(text, f) in ("sty", "lsty"):  # a style of this function: its address is passed
            return False
        stripped = STRING_LITERAL.sub('""', text)
        names = f.vars | set(f.params) | f.local_styles | {"parent"}
        return any(w in names for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", stripped))

    def parse(self, f, line):
        """An El for a statement the run machinery can move, else None."""
        m = CREATE_LINE.match(line)
        if m:
            var, fn, rest = m.groups()
            args = split_args(rest)
            if not fn.endswith("_create") or not args or args[0] not in (f.vars | {"parent"}):
                return None
            if any(self.is_local(f, a) for a in args[1:]):
                return None
            kinds = ["parent"] + [self.kind_of(a, f) for a in args[1:]]
            return El(fn, args, kinds, var, True, line)
        m = CALL_LINE.match(line)
        if m:
            fn, rest = m.groups()
            args = split_args(rest)
            style = args and args[0].startswith("&") and args[0][1:] in f.local_styles
            if fn.endswith("_create") or not args or not (args[0] in f.vars or style):
                return None
            if any(self.is_local(f, a) for a in args[1:]):
                return None
            kinds = ["obj"] + [self.kind_of(a, f) for a in args[1:]]
            return El(fn, args, kinds, args[0], False, line)
        return None

    # ---- names ----
    def names(self, lines):
        """Keep lv_obj_set_name only where the code looks the object up; what stays moves behind
        the object's own statements (it changes none of them)."""
        out, pending, var = [], None, None
        for line in lines:
            m = CALL_NAME.match(line)
            d = DECL_LINE.match(out[-1]) if out else None
            if m and d and d.group(1) == m.group(1):
                if name_is_looked_up(json_name(m.group(2)), self.words):
                    pending, var = line, m.group(1)
                continue
            if pending is not None:
                k = CALL_LINE.match(line)
                if DECL_LINE.match(line) or k is None or k.group(1).endswith("_create")                         or split_args(k.group(2))[0] != var:
                    out.append(pending)
                    pending = None
            out.append(line)
        if pending is not None:
            out.append(pending)
        return out

    # ---- solving ----
    def solve(self):
        runs = []  # (func index, first line index, [El])
        prepared = []
        for fi, (f, lines, root) in enumerate(self.funcs):
            lines = self.names(lines)
            prepared.append(lines)
            els = [self.parse(f, ln) for ln in lines]
            i = 0
            while i < len(lines):
                e = els[i]
                if e is None:
                    i += 1
                    continue
                j = i + 1
                run = [e]
                while j < len(lines) and els[j] is not None and not els[j].create \
                        and els[j].var == e.var:
                    run.append(els[j])
                    j += 1
                if len(run) >= 2:
                    runs.append((fi, i, run))
                i = j
        used = [[False] * len(r[2]) for r in runs]
        # every window of a run, under its two keys (exact; numbers/colours/fonts/texts open)
        index = {}
        for ri, (_, _, run) in enumerate(runs if COMPACT else []):
            for s in range(len(run)):
                for n in range(2, min(MAX_RUN, len(run) - s) + 1):
                    win = run[s:s + n]
                    for key in self.keys(win):
                        index.setdefault(key, []).append((ri, s, n))
        index = {k: v for k, v in index.items() if len(v) >= 2}
        import heapq
        heap = []

        def available(key):
            got, end = [], {}
            for ri, s, n in index[key]:
                if s < end.get(ri, -1):
                    continue
                if any(used[ri][s:s + n]):
                    continue
                got.append((ri, s, n))
                end[ri] = s + n
            return got

        def score(key):
            occs = available(key)
            if len(occs) < 2:
                return None, occs, None
            wins = [runs[ri][2][s:s + n] for ri, s, n in occs]
            gain, slots = self.gain(key, wins)
            return gain, occs, slots

        for key in index:
            g, _, _ = score(key)
            if g is not None and g >= MIN_GAIN:
                heapq.heappush(heap, (-g, len(heap), key))
        chosen = {}  # (run, start) -> (helper, length)
        while heap:
            neg, tie, key = heapq.heappop(heap)
            g, occs, slots = score(key)
            if g is None or g < MIN_GAIN:
                continue
            if heap and g < -heap[0][0]:
                heapq.heappush(heap, (-g, tie, key))
                continue
            wins = [runs[ri][2][s:s + n] for ri, s, n in occs]
            helper = Helper(len(self.helpers), key, wins, slots)
            self.helpers.append(helper)
            for ri, s, n in occs:
                for k in range(s, s + n):
                    used[ri][k] = True
                chosen[(ri, s)] = (helper, n)
        # rewrite the functions
        by_func = {}
        for ri, (fi, first, run) in enumerate(runs):
            by_func.setdefault(fi, []).append((first, ri, run))
        for fi, (f, _, root) in enumerate(self.funcs):
            lines = prepared[fi]
            repl = {}  # first line index -> (length, new line)
            for first, ri, run in by_func.get(fi, []):
                s = 0
                while s < len(run):
                    hit = chosen.get((ri, s))
                    if hit is None:
                        s += 1
                        continue
                    helper, n = hit
                    repl[first + s] = (n, helper, run[s:s + n])
                    s += n
            out = []
            i = 0
            while i < len(lines):
                if i in repl:
                    n, helper, win = repl[i]
                    out.append((helper, win))
                    i += n
                else:
                    out.append(lines[i])
                    i += 1
            # a created object nobody uses afterwards is not declared (-Wunused-variable)
            calls = [x if isinstance(x, str) else "        %s;" % self.call(*x) for x in out]
            text = STRING_LITERAL.sub('""', "\n".join(calls) + "\n" + root)
            final = []
            for x, call in zip(out, calls):
                if not isinstance(x, str) and x[0].create:
                    var = x[1][0].var
                    if re.search(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(var), text):
                        call = "        lv_obj_t * %s = %s" % (var, call.strip())
                final.append(call)
            self.result.append(final)
        self.solved = True
        return self.runs_files()

    # ---- keys and costs ----
    def keys(self, win):
        exact, wild = [], []
        for e in win:
            ea, wa = [], []
            for j, a in enumerate(e.args):
                k = e.kinds[j]
                if j == 0:
                    ea.append("@")
                    wa.append("@")
                elif k in ALWAYS_PARAM:
                    ea.append("%T")
                    wa.append("%T")
                elif k in PARAM_KINDS:
                    ea.append(a)
                    wa.append("%" + k)
                else:
                    ea.append(a)
                    wa.append(a)
            exact.append((e.fn, e.create, tuple(ea)))
            wild.append((e.fn, e.create, tuple(wa)))
        return [("x", tuple(exact)), ("w", tuple(wild))]

    @staticmethod
    def arg_cost(kind):
        return {"int": 3, "color": 8, "font": 6, "str": 6, "tr": 6, "other": 6, "parent": 2,
                "sty": 6, "lsty": 6, "subj": 6, "cb": 6,
                "obj": 0}[kind]

    def gain(self, key, wins):
        """(estimated bytes saved, parameter slots) of making these equal windows one function."""
        n = len(wins)
        first = wins[0]
        slots = []  # (element, arg) that differ or are texts to translate
        for i, e in enumerate(first):
            for j in range(1, len(e.args)):
                k = e.kinds[j]
                if k in ALWAYS_PARAM:
                    slots.append((i, j))
                elif k in PARAM_KINDS and key[0] == "w":
                    if any(w[i].args[j] != e.args[j] for w in wins):
                        slots.append((i, j))
        inline = call = 0
        for w in wins:
            for e in w:
                inline += 10 + sum(self.arg_cost(k) for k in e.kinds[1:])
            call += 10 + sum(self.arg_cost(w[i].kinds[j]) for i, j in slots)
        body = 20
        slotset = set(slots)
        for i, e in enumerate(first):
            body += 10
            for j in range(1, len(e.args)):
                body += 2 if (i, j) in slotset else self.arg_cost(e.kinds[j])
        return inline - call - body, slots

    # ---- output ----
    def call(self, helper, win):
        args = [win[0].args[0]]
        args += [win[i].args[j] for i, j in helper.slots]
        return "%s_run_%d(%s)" % (self.p.name, helper.index, ", ".join(args))

    def runs_files(self):
        n, N = self.p.name, self.p.NAME
        protos, defs = [], []
        for h in self.helpers:
            first = h.occs[0]
            params = ["%s * %s" % ("lv_style_t" if h.style else "lv_obj_t", "parent" if h.create else "o")]
            names = {}
            for k, (i, j) in enumerate(h.slots):
                names[(i, j)] = "a%d" % k
                params.append("%s a%d" % (PARAM_CTYPE[first[i].kinds[j]], k))
            ret = "lv_obj_t *" if h.create else "void"
            sig = "%s %s_run_%d(%s)" % (ret, n, h.index, ", ".join(params))
            protos.append(sig + ";")
            body = [sig, "{"]
            for i, e in enumerate(first):
                args = []
                for j, a in enumerate(e.args):
                    if j == 0:
                        args.append("parent" if e.create else "o")
                    else:
                        args.append(names.get((i, j), a))
                if e.create:
                    body.append("    lv_obj_t * o = %s(%s);" % (e.fn, ", ".join(args)))
                else:
                    body.append("    %s(%s);" % (e.fn, ", ".join(args)))
            if h.create:
                body.append("    return o;")
            body += ["}", ""]
            defs += body
        header = ["/**", " * @file %s_runs_gen.h" % n, " */", "",
                  "#ifndef %s_RUNS_GEN_H" % N, "#define %s_RUNS_GEN_H" % N, "",
                  "#ifdef __cplusplus", 'extern "C" {', "#endif", "", HEADER_INCLUDES, "",
                  "/* Runs of statements that repeat in the screens and components (tools/lvpro_gen.py) */",
                  ""] + protos + ["", "#ifdef __cplusplus", '} /*extern "C"*/', "#endif", "",
                                  "#endif /*%s_RUNS_GEN_H*/" % N, ""]
        c = ["/**", " * @file %s_runs_gen.c" % n,
             " * @brief Runs of statements shared by the generated screens and components",
             " */", "", '#include "%s_gen.h"' % n, ""] + defs
        return "\n".join(header), "\n".join(c)


def json_name(lit):
    return lit[1:-1].encode("utf-8").decode("unicode_escape")


def generate(p):
    files = {}
    cp = Compactor(p)
    bodies = {}
    for comp in p.components.values():
        h, c = gen_component(p, comp, cp)
        files["%s/%s_gen.h" % (comp.rel_dir, comp.name)] = h
        bodies["%s/%s_gen.c" % (comp.rel_dir, comp.name)] = c
    for scr in p.screens:
        h, c = gen_screen(p, scr, cp)
        files["%s/%s_gen.h" % (scr.rel_dir, scr.name)] = h
        bodies["%s/%s_gen.c" % (scr.rel_dir, scr.name)] = c
    h, c = gen_project(p, cp)  # last: needs the callbacks collected above
    files["%s_gen.h" % p.name] = h
    bodies["%s_gen.c" % p.name] = c
    runs_h, runs_c = cp.solve()
    for rel, body in bodies.items():
        files[rel] = "\n".join(cp.render(body))
    if letters_fonts(p):
        files["%s/letters.json" % LETTERS_DIR] = letters_json(p)
    files["%s_runs_gen.h" % p.name] = runs_h
    files["%s_runs_gen.c" % p.name] = runs_c
    sources = sorted(k for k in files if k.endswith(".c"))
    sources += ["fonts/%s_data.c" % fo["name"] for fo in p.fonts]
    sources.append("%s.c" % p.name)
    sources = sorted(set(sources))
    files["file_list_gen.cmake"] = "list(\n  APPEND\n  LV_EDITOR_PROJECT_SOURCES\n" + "\n".join(
        "  ${CMAKE_CURRENT_LIST_DIR}/%s" % s for s in sources) + ")\n"
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("project", help="LVGL Pro project folder (contains project.xml)")
    ap.add_argument("--fonts", action="store_true", help="also convert fonts (lv_font_conv)")
    ap.add_argument("--force-fonts", action="store_true", help="reconvert every font")
    ap.add_argument("--check", action="store_true", help="fail if generated files are stale")
    args = ap.parse_args()
    try:
        p = Project(args.project)
        files = generate(p)
        once = {"%s.h" % p.name: CUSTOM_H.format(n=p.name, N=p.NAME),
                "%s.c" % p.name: CUSTOM_C.format(n=p.name, N=p.NAME),
                "CMakeLists.txt": CMAKELISTS, "user_config.cmake": USER_CONFIG,
                "component_lib_list_gen.cmake": "\n"}
        stale = []
        for rel, text in files.items():
            path = p.dir / rel
            old = path.read_text(encoding="utf-8") if path.exists() else None
            if old != text:
                stale.append(rel)
                if not args.check:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(text, encoding="utf-8", newline="\n")
        for rel, text in once.items():
            path = p.dir / rel
            if not path.exists():
                stale.append(rel)
                if not args.check:
                    path.write_text(text, encoding="utf-8", newline="\n")
        if args.check:
            stale += fix_text_fonts(p, True)
            stale += [("%s/%s.bin" % (LETTERS_DIR, fo["name"])) for fo in letters_fonts(p)
                      if letters_set(fo) and not (p.dir / LETTERS_DIR / ("%s.bin" % fo["name"])).exists()]
            stale += ["fonts/%s_data.c" % fo["name"] for fo in p.fonts if fo.get("line")
                      and (p.dir / "fonts" / ("%s_data.c" % fo["name"])).exists()
                      and LINE_NOTE not in (p.dir / "fonts" / ("%s_data.c" % fo["name"])).read_text(encoding="utf-8")]
            if stale:
                print("stale generated files:\n  " + "\n  ".join(stale))
                return 1
            print("generated files are up to date")
            return 0
        print("%s: %d components, %d screens, %d fonts, %d subjects, %d callbacks; %d files updated"
              % (p.name, len(p.components), len(p.screens), len(p.fonts), len(p.subjects),
                 len(p.callbacks), len(stale)))
        if args.fonts or args.force_fonts:
            gen_fonts(p, args.force_fonts)
        else:
            fix_text_fonts(p, False)
        return 0
    except (GenError, ET.ParseError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
