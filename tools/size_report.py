#!/usr/bin/env python3
"""size_report.py - where a firmware image's bytes go (docs/features/39 OPT-003).

Reads an ESP-IDF build folder: the GNU ld map named by project_description.json and the OTA slot
from the partition table (as tools/check_image_size.py does). It prints
  1. the categories (the CATEGORIES table below: one line per component), KB = 1,000 bytes,
     % of the OTA slot, split into code (with RAM-loaded data), read-only data and strings;
  2. our biggest functions and data (--top N; "ours" = the archives built from the folders found
     under components/*/* and the app's main);
  3. the strings per object of our archives: the linker merges every string literal into one pool
     and the map charges it to one object, so the pool is charged back to each object in proportion
     to its pre-merge size ("size before relaxing");
  4. with --fonts <layout dir>: the bytes of each generated font_*_data.c per block of characters;
  5. with --compare <other build>: the category table with a delta column and the objects that
     changed most.

    python tools/size_report.py firmware/crowpanel_p4/build_v1 --top 25
    python tools/size_report.py build_new --compare build_old
    python tools/size_report.py firmware/crowpanel_p4/build_v1 --markdown   # CI's job summary

Exit 0, or 2 when an input is missing.
"""
import argparse
import collections
import fnmatch
import glob
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_image_size  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
FALLBACK = "ESP-IDF itself"

# (category, archive pattern, object pattern), first match wins; fnmatch patterns. A new
# component is one line here; an archive nobody lists falls under FALLBACK.
CATEGORIES = [
    ("UI: screen-saver photo", "libalarm_ui.a", "saver_*"),
    ("UI: text and digit fonts", "libalarm_ui.a", "font_*"),
    ("UI: icon fonts", "libalarm_ui.a", "icons_*"),
    ("UI: screens generated from layouts", "libalarm_ui.a", "*_gen.c.obj"),
    ("UI: Z-Wave screens (hand-written)", "libalarm_ui.a", "ui_zwave*"),
    ("UI: other hand-written screens", "libalarm_ui.a", "*"),
    ("Panel Manager: files (gzip)", "libpanel_web.a", "*.S.obj"),
    ("Panel Manager: server code", "libpanel_web.a", "*"),
    ("TLS root certificate list", "libmbedtls.a", "*x509_crt_bundle*"),
    ("Crypto and TLS", "libmbedtls.a", "*"),
    ("LVGL", "liblvgl__lvgl.a", "*"),
    ("LVGL", "liblvgl_port_lib.a", "*"),
    ("Wi-Fi link to the C6 (ESP-Hosted)", "libeh_*.a", "*"),
    ("Wi-Fi link to the C6 (ESP-Hosted)", "libesp_wifi*.a", "*"),
    ("Wi-Fi link to the C6 (ESP-Hosted)", "libesp_hosted_compat.a", "*"),
    ("Z-Wave (library, service, zones)", "libzwave_*.a", "*"),
    ("Zigbee", "libesp-zigbee*.a", "*"),
    ("Zigbee", "libespressif__esp-zigbee-lib.a", "*"),
    ("Zigbee", "libopenthread.a", "*"),
    ("Zigbee", "libzigbee_*.a", "*"),
    ("Networking", "liblwip.a", "*"),
    ("Networking", "libespressif__mdns.a", "*"),
    ("Networking", "libespressif__mqtt.a", "*"),
    ("Networking", "libespressif__cjson.a", "*"),
    ("Networking", "libesp_http*.a", "*"),
    ("Networking", "libesp_https_server.a", "*"),
    ("Networking", "libhttp_parser.a", "*"),
    ("Networking", "libtcp_transport.a", "*"),
    ("Networking", "libesp_netif.a", "*"),
    ("Networking", "libesp_eth.a", "*"),
    ("Networking", "libmqtt_*_lib.a", "*"),
    ("Crypto and TLS", "libtfpsacrypto.a", "*"),
    ("Crypto and TLS", "libmbedx509.a", "*"),
    ("Crypto and TLS", "libesp-tls.a", "*"),
    ("Crypto and TLS", "libesp_security.a", "*"),
    ("Crypto and TLS", "libesp_hal_security.a", "*"),
    ("Panel services (ours)", "libplatform_esp.a", "*"),
    ("Panel services (ours)", "libpanel_*.a", "*"),
    ("Panel services (ours)", "libnet_*.a", "*"),
    ("Panel services (ours)", "libnotify.a", "*"),
    ("Panel services (ours)", "libha_link.a", "*"),
    ("Panel services (ours)", "libdev_remote.a", "*"),
    ("Panel services (ours)", "liblang_packs.a", "*"),
    ("Panel services (ours)", "libmain.a", "*"),
    ("Panel services (ours)", "libbackup*.a", "*"),
    ("Panel services (ours)", "librf_*.a", "*"),
    ("Panel services (ours)", "libcc1101_regs.a", "*"),
    ("Panel services (ours)", "libboard_*.a", "*"),
    ("Panel services (ours)", "libui_port_esp.a", "*"),
    ("Panel services (ours)", "librtc_*.a", "*"),
    ("Panel services (ours)", "libhome_control.a", "*"),
    ("Panel services (ours)", "libpack_store.a", "*"),
    ("Panel services (ours)", "libdevice_service.a", "*"),
    ("Panel services (ours)", "libpower_monitor.a", "*"),
    ("Panel services (ours)", "libhttp_redirect.a", "*"),
    ("Panel services (ours)", "libstorage.a", "*"),
    ("Panel services (ours)", "libalarm_mem.a", "*"),
    ("Panel services (ours)", "libh264_sps.a", "*"),
    ("Alarm core (ours)", "libalarm_core.a", "*"),
    ("Alarm core (ours)", "libalarm_proto.a", "*"),
    ("Alarm core (ours)", "libzone_hub.a", "*"),
    ("Alarm core (ours)", "libmsg_codec.a", "*"),
    ("Alarm core (ours)", "libsounder.a", "*"),
    ("Alarm core (ours)", "libevent_log.a", "*"),
    ("Alarm core (ours)", "libi18n.a", "*"),
    ("Remote screen video", "libespressif__esp_h264.a", "*"),
    ("Remote screen video", "libesp_driver_jpeg.a", "*"),
    ("Remote screen video", "libesp_hal_jpeg.a", "*"),
    ("SD card", "libfatfs.a", "*"),
    ("SD card", "libsdmmc.a", "*"),
    ("SD card", "libesp_driver_sd*.a", "*"),
    ("SD card", "libesp_hal_sd.a", "*"),
    ("C library, console, NVS", "libc.a", "*"),
    ("C library, console, NVS", "libconsole.a", "*"),
    ("C library, console, NVS", "libnvs_flash.a", "*"),
    ("C library, console, NVS", "libgcc.a", "*"),
    ("C library, console, NVS", "libstdc++.a", "*"),
    ("C library, console, NVS", "libesp_libc.a", "*"),
]

IMAGE_OUT = {".flash.text", ".flash.rodata", ".flash.appdesc", ".dram0.data", ".iram0.text",
             ".iram0.data", ".flash.init_array", ".flash.tdata", ".rtc.text", ".rtc.data",
             ".hp.text", ".hp.data", ".tcm.text", ".tcm.data", ".flash.rodata_noload",
             ".lp_ram.data", ".rtc.force_slow", ".iram0.text_end", ".flash_rodata_dummy"}
RODATA_OUT = (".flash.rodata", ".flash.appdesc", ".flash.init_array", ".flash.tdata")
SKIP_SECTIONS = (".debug", ".comment", ".riscv.attributes", ".note")

OUT = re.compile(r"^(\.[A-Za-z0-9_.]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
OUT_ONLY = re.compile(r"^(\.[A-Za-z0-9_.]+)$")
SEC = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
SEC_NAME = re.compile(r"^ (\S+)$")
CONT = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
PRE = re.compile(r"^\s+0x([0-9a-f]+) \(size before relaxing\)")
STR_SEC = re.compile(r"\.str\d+\.\d+$")
OBJ = re.compile(r"(?:.*[/\\])?([^/\\]+\.a)\((.+)\)$")
SYMBOL = re.compile(r"^\.(?:text|literal|rodata|srodata|data|sdata|bss|sbss)\.(.+)$")


class ReportError(Exception):
    """An input is missing or unreadable."""


def parse_map(path):
    """[(output section, input section, size, size before merging or None, object)] of the
    linker script and memory map (what comes before it is the discarded sections and the
    archive list, which are not in the image)."""
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    start = next((i for i, ln in enumerate(lines)
                  if ln.startswith("Linker script and memory map")), None)
    if start is None:
        raise ReportError("%s has no 'Linker script and memory map'" % path)
    rows, out, i, n = [], None, start, len(lines)
    while i < n:
        ln = lines[i]
        m = OUT.match(ln) or OUT_ONLY.match(ln)
        if m:
            out = m.group(1)
            i += 1
            continue
        name = None
        m = SEC.match(ln)
        if m:
            name, size, obj = m.group(1), int(m.group(3), 16), m.group(4)
        else:
            m2 = SEC_NAME.match(ln)
            if m2 and i + 1 < n:
                m3 = CONT.match(lines[i + 1])
                if m3:  # the section name was too long: its numbers are on the next line
                    name, size, obj = m2.group(1), int(m3.group(2), 16), m3.group(3)
                    i += 1
        if name:
            pre = None
            if i + 1 < n:
                mp = PRE.match(lines[i + 1])
                if mp:
                    pre = int(mp.group(1), 16)
                    i += 1
            if name.startswith(".") and not name.startswith(SKIP_SECTIONS):
                rows.append((out, name, size, pre, obj))
        i += 1
    return rows


def attribute(rows):
    """Image bytes per object: ({object: [code, rodata, strings, ram-loaded]}, merged pool bytes,
    pre-merge bytes, [(object, input section, size)] of the non-string sections).
    The merged string pool is charged back to the objects by their pre-merge sizes."""
    img = [r for r in rows if r[0] in IMAGE_OUT or (r[0] or "").startswith(
        (".flash", ".dram0.data", ".iram0.text"))]
    merged = sum(r[2] for r in img if STR_SEC.search(r[1]))
    pre_total = sum(r[3] if r[3] is not None else r[2] for r in img if STR_SEC.search(r[1]))
    per = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0.0])
    sections = []
    for out, sec, size, pre, obj in img:
        if "bss" in out:
            continue
        if STR_SEC.search(sec):
            share = pre if pre is not None else size
            per[obj][2] += share * merged / pre_total if pre_total else 0
            continue
        if out == ".flash.text":
            per[obj][0] += size
        elif out in RODATA_OUT:
            per[obj][1] += size
        else:
            per[obj][3] += size
        sections.append((obj, sec, size))
    return per, merged, pre_total, sections


def split_object(obj):
    """(archive basename, object file) of 'path/libx.a(y.c.obj)'; a bare object has no archive."""
    m = OBJ.match(obj)
    return (m.group(1), m.group(2)) if m else (os.path.basename(obj.replace("\\", "/")), "")


def categorize(archive, objname):
    for cat, apat, opat in CATEGORIES:
        if fnmatch.fnmatchcase(archive, apat) and fnmatch.fnmatchcase(objname, opat):
            return cat
    return FALLBACK


def our_archives(components_dir):
    """Archive names built from this repository: lib<folder>.a for each components/*/*, and the
    app's main."""
    names = {"libmain.a"}
    for d in glob.glob(os.path.join(str(components_dir), "*", "*")):
        if os.path.isdir(d):
            names.add("lib%s.a" % os.path.basename(d))
    return names


def category_table(per):
    agg = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0.0])
    for obj, v in per.items():
        a, o = split_object(obj)
        c = categorize(a, o)
        for i in range(4):
            agg[c][i] += v[i]
    return agg


def per_object(per):
    """{'libx.a(y.o)' with the path cut: total bytes}."""
    tot = collections.defaultdict(float)
    for obj, v in per.items():
        a, o = split_object(obj)
        tot["%s(%s)" % (a, o) if o else a] += sum(v)
    return tot


def load(build_dir):
    build_dir = Path(build_dir)
    try:
        build = check_image_size.read_build(build_dir)
    except (check_image_size.CheckError, KeyError, ValueError) as exc:
        raise ReportError(str(exc))
    if not build["map"].is_file():
        raise ReportError("%s is missing: build the firmware first" % build["map"])
    per, merged, pre_total, sections = attribute(parse_map(build["map"]))
    return {"build": build, "per": per, "merged": merged, "pre": pre_total, "sections": sections}


# ---- fonts -------------------------------------------------------------------------------

BLOCKS = ("ASCII", "Latin-1", "Latin Extended-A", "other")


def block_of(cp):
    return 0 if cp < 0x80 else 1 if cp < 0x100 else 2 if cp < 0x180 else 3


def font_blocks(src):
    """(glyph count, bitmap bytes, [bytes per block]) of one generated LVGL font source: each
    glyph costs its bitmap plus an 8-byte record; a glyph's block comes from the font's cmaps."""
    idx = [int(x) for x in re.findall(r"\.bitmap_index\s*=\s*(\d+)", src)]
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m or not idx:
        return None
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    nbytes = len(re.findall(r"0x[0-9a-fA-F]{1,2}", body))
    count = len(idx) - 1  # glyph 0 is reserved
    ends = idx[1:] + [nbytes]
    lists = {name: [int(x, 16) if x[:2].lower() == "0x" else int(x)
                    for x in re.findall(r"0[xX][0-9a-fA-F]+|\d+", vals)]
             for name, vals in re.findall(r"(unicode_list\w*)\[\]\s*=\s*\{([^}]*)\}", src)}
    block_of_gid = {}
    cmap = re.compile(r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*"
                      r"\.glyph_id_start\s*=\s*(\d+)(?:,\s*\.unicode_list\s*=\s*(\w+))?")
    for start, length, gid0, ulist in cmap.findall(src):
        start, length, gid0 = int(start), int(length), int(gid0)
        offs = lists.get(ulist) if ulist and ulist != "NULL" else None
        for k in range(length):
            cp = start + (offs[k] if offs and k < len(offs) else k)
            block_of_gid[gid0 + k] = block_of(cp)
    per = [0, 0, 0, 0]
    for gid in range(1, count + 1):
        per[block_of_gid.get(gid, 3)] += ends[gid] - ends[gid - 1] + 8
    return count, nbytes, per


def fonts_report(layout_dir):
    """[(file, glyphs, bitmap bytes, [bytes per block])] for each font_*_data.c in the folder."""
    if not os.path.isdir(str(layout_dir)):
        raise ReportError("%s is not a folder" % layout_dir)
    rows = []
    for f in sorted(glob.glob(os.path.join(str(layout_dir), "font_*_data.c"))):
        res = font_blocks(Path(f).read_text(encoding="utf-8", errors="replace"))
        if res:
            rows.append((os.path.basename(f),) + res)
    return rows


# ---- output ------------------------------------------------------------------------------

def kb(n):
    return "%.1f" % (n / 1000.0)


def delta_kb(n):
    return "%+.1f" % (n / 1000.0)


def table(title, headers, rows, md):
    out = []
    if md:
        out += ["#### " + title, "", "| " + " | ".join(headers) + " |",
                "|" + "|".join("---" if i == 0 else "---:" for i in range(len(headers))) + "|"]
        out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
        out.append("")
    else:
        widths = [max(len(str(x)) for x in col) for col in zip(headers, *rows)]
        out += [title, "  ".join(str(h).ljust(w) if i == 0 else str(h).rjust(w)
                                 for i, (h, w) in enumerate(zip(headers, widths)))]
        out += ["  ".join(str(c).ljust(w) if i == 0 else str(c).rjust(w)
                           for i, (c, w) in enumerate(zip(r, widths))) for r in rows]
        out.append("")
    return out


def category_rows(agg, slot, base=None):
    rows = []
    total = sum(sum(v) for v in agg.values())
    cats = set(agg) | set(base or {})
    zero = [0.0] * 4
    for c in sorted(cats, key=lambda k: -sum(agg.get(k, zero))):
        v = agg.get(c, zero)
        s = sum(v)
        row = [c, kb(s), "%.1f" % (s * 100.0 / slot), kb(v[0] + v[3]), kb(v[1]), kb(v[2])]
        if base is not None:
            row.append(delta_kb(s - sum(base.get(c, zero))))
        rows.append(row)
    total_row = ["TOTAL", kb(total), "%.1f" % (total * 100.0 / slot), "", "", ""]
    if base is not None:
        total_row.append(delta_kb(total - sum(sum(v) for v in base.values())))
    return rows + [total_row]


def build_report(args):
    cur = load(args.build_dir)
    slot = cur["build"]["slot"]["size"]
    ours = our_archives(args.components)
    md = args.markdown
    out = ["### Where the image's bytes go: %s %s" % (cur["build"]["project"],
                                                     cur["build"]["version"]), ""]
    out.append("Image %s bytes, OTA slot `%s` %s bytes; %s bytes attributed from the map; "
               "%s bytes of merged strings charged back to their objects (KB = 1,000 bytes)."
               % (format(cur["build"]["image_size"], ","), cur["build"]["slot"]["name"],
                  format(slot, ","), format(round(sum(sum(v) for v in cur["per"].values())), ","),
                  format(cur["merged"], ",")))
    out.append("")
    agg = category_table(cur["per"])
    heads = ["category", "KB", "% slot", "code", "data", "strings"]
    if args.compare:
        other = load(args.compare)
        base = category_table(other["per"])
        out += table("Categories against %s" % other["build"]["version"], heads + ["delta KB"],
                     category_rows(agg, slot, base), md)
        now, then = per_object(cur["per"]), per_object(other["per"])
        diffs = sorted(((now.get(k, 0) - then.get(k, 0), k) for k in set(now) | set(then)),
                       key=lambda d: -abs(d[0]))
        out += table("Objects that changed most", ["object", "now KB", "before KB", "delta KB"],
                     [[k, kb(now.get(k, 0)), kb(then.get(k, 0)), delta_kb(d)]
                      for d, k in diffs[:args.top] if round(d) != 0], md)
        return "\n".join(out)
    out += table("Categories", heads, category_rows(agg, slot), md)

    mine = [(sz, a, o, sec) for (obj, sec, sz) in cur["sections"]
            for a, o in [split_object(obj)] if a in ours]
    top = sorted(mine, key=lambda r: -r[0])[:args.top]
    rows = []
    for sz, a, o, sec in top:
        m = SYMBOL.match(sec)
        rows.append([m.group(1) if m else sec, "code" if sec.startswith((".text", ".literal"))
                     else "data", format(sz, ","), "%s(%s)" % (a, o)])
    out += table("Our biggest functions and data (bytes)", ["name", "kind", "bytes", "object"],
                 rows, md)

    strs = sorted(((v[2], k) for k, v in cur["per"].items()
                   if split_object(k)[0] in ours and v[2] > 0), key=lambda r: -r[0])
    srows = collections.defaultdict(float)
    for s, k in strs:
        a, o = split_object(k)
        srows["%s(%s)" % (a, o)] += s
    out += table("Strings per object of our archives (merged pool charged back)",
                 ["object", "KB"], [[k, kb(v)] for k, v in
                                    sorted(srows.items(), key=lambda kv: -kv[1])[:args.top]], md)
    if args.fonts:
        frows = fonts_report(args.fonts)
        tot = [0, 0, 0, 0]
        rows = []
        for name, glyphs, nbytes, per in frows:
            rows.append([name, glyphs, format(nbytes, ",")] + [format(x, ",") for x in per])
            tot = [a + b for a, b in zip(tot, per)]
        rows.append(["TOTAL", "", ""] + [format(x, ",") for x in tot])
        out += table("Fonts: bytes per block of characters (bitmaps + 8-byte glyph records)",
                     ["font", "glyphs", "bitmap"] + list(BLOCKS), rows, md)
    return "\n".join(out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("build_dir", type=Path, help="the ESP-IDF build folder")
    p.add_argument("--top", type=int, default=25, help="rows in the top lists (default %(default)s)")
    p.add_argument("--fonts", type=Path, help="folder holding the generated font_*_data.c files")
    p.add_argument("--compare", type=Path, help="another build folder to compare with")
    p.add_argument("--components", type=Path, default=REPO / "components",
                   help="our components folder (decides which archives are ours)")
    p.add_argument("--markdown", action="store_true", help="Markdown tables (CI's job summary)")
    p.add_argument("--summary", default="",
                   help="also append the report to this file (CI: $GITHUB_STEP_SUMMARY)")
    args = p.parse_args(argv)
    try:
        text = build_report(args)
    except ReportError as exc:
        print("FAIL: %s" % exc)
        return 2
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
