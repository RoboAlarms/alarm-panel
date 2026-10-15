#!/usr/bin/env python3
"""check_image_size.py - a firmware image fits its OTA slot with room to grow (docs/features/19 CYD-013).

Reads an ESP-IDF build folder: the signed app image named in project_description.json and the
partition table the build generated (partition_table/partition-table.bin). The image has to fit
the smallest app partition (both OTA slots take the same image) at no more than --limit percent
of it, so that the next releases still fit without a new partition table (which wipes a panel).
With --ram it adds the static memory use from the linker map (esp_idf_size, part of ESP-IDF's
Python environment); in CI the report goes to the job summary as well.

    python tools/check_image_size.py firmware/e32r40t/build            # exit 1 when over
    python tools/check_image_size.py firmware/e32r40t/build --ram --limit 85
"""
import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

DEFAULT_LIMIT = 85.0  # percent of the OTA slot (CYD-013)

# The binary partition table (ESP-IDF components/partition_table/gen_esp32part.py): 32-byte
# entries, an optional MD5 entry, then 0xFF padding.
ENTRY = struct.Struct("<2sBBII16sI")
ENTRY_MAGIC = b"\xaa\x50"
MD5_MAGIC = b"\xeb\xeb"
TYPE_APP = 0x00


class CheckError(Exception):
    """An input is missing or unreadable."""


def parse_partitions(blob):
    """[{name, type, subtype, offset, size}] from a binary partition table."""
    parts = []
    for pos in range(0, len(blob) - ENTRY.size + 1, ENTRY.size):
        magic, ptype, subtype, offset, size, label, _flags = ENTRY.unpack_from(blob, pos)
        if magic == ENTRY_MAGIC:
            name = label.split(b"\0", 1)[0].decode("ascii", "replace")
            parts.append({"name": name, "type": ptype, "subtype": subtype,
                          "offset": offset, "size": size})
        elif magic == MD5_MAGIC:
            continue
        else:
            break  # 0xFFFF: the end of the table
    return parts


def app_slot(parts):
    """The smallest app partition: every slot must take the image."""
    apps = [p for p in parts if p["type"] == TYPE_APP]
    if not apps:
        raise CheckError("the partition table has no app partition")
    return min(apps, key=lambda p: p["size"])


def read_build(build_dir):
    """What the check needs from a build folder."""
    build_dir = Path(build_dir)
    desc_file = build_dir / "project_description.json"
    table_file = build_dir / "partition_table" / "partition-table.bin"
    for f in (desc_file, table_file):
        if not f.is_file():
            raise CheckError("%s is missing: build the firmware first" % f)
    desc = json.loads(desc_file.read_text(encoding="utf-8"))
    image_file = build_dir / desc["app_bin"]
    if not image_file.is_file():
        raise CheckError("%s is missing: build the firmware first" % image_file)
    return {
        "project": desc.get("project_name", "?"),
        "version": desc.get("project_version", "?"),
        "target": desc.get("target", "?"),
        "image": image_file,
        "image_size": image_file.stat().st_size,
        "map": build_dir / (Path(desc.get("app_elf", "")).stem + ".map"),
        "slot": app_slot(parse_partitions(table_file.read_bytes())),
    }


def verdict(image_size, slot_size, limit):
    """(percent of the slot, within the limit)."""
    percent = 100.0 * image_size / slot_size
    return percent, percent <= limit


def ram_rows(size_json):
    """[(memory, used, total)] from esp_idf_size --format json2, flash left out (one "Flash"
    region on the P4, "Flash Code" and "Flash Data" on the ESP32)."""
    rows = []
    for region in size_json.get("layout", []):
        if str(region.get("name", "")).startswith("Flash"):
            continue
        rows.append((region.get("name", "?"), int(region.get("used", 0)),
                     int(region.get("total", 0))))
    return rows


def static_ram(map_file):
    """ram_rows() for a linker map, or None when esp_idf_size isn't available."""
    if not Path(map_file).is_file():
        return None
    try:
        out = subprocess.run([sys.executable, "-m", "esp_idf_size", "--format", "json2",
                              str(map_file)], capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return ram_rows(json.loads(out.stdout))


def report(build, limit, ram):
    """The Markdown report and whether the image is within the limit."""
    slot = build["slot"]
    percent, ok = verdict(build["image_size"], slot["size"], limit)
    lines = [
        "### %s %s (%s)" % (build["project"], build["version"], build["target"]),
        "",
        "| | Bytes | |",
        "|---|---:|---|",
        "| Image `%s` | %s | %.1f %% of the slot (limit %.0f %%): %s |"
        % (build["image"].name, format(build["image_size"], ","), percent, limit,
           "OK" if ok else "**TOO BIG**"),
        "| OTA slot `%s` | %s | %s free |"
        % (slot["name"], format(slot["size"], ","),
           format(slot["size"] - build["image_size"], ",")),
    ]
    if ram is not None:
        for name, used, total in ram:
            share = " (%.1f %%)" % (100.0 * used / total) if total else ""
            lines.append("| Static %s | %s | of %s%s |"
                         % (name, format(used, ","), format(total, ","), share))
    return "\n".join(lines) + "\n", ok


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("build_dir", type=Path, help="the ESP-IDF build folder")
    parser.add_argument("--limit", type=float, default=DEFAULT_LIMIT,
                        help="largest share of the OTA slot, percent (default %(default)s)")
    parser.add_argument("--ram", action="store_true",
                        help="add static memory use from the linker map (esp_idf_size)")
    parser.add_argument("--summary", default=os.environ.get("GITHUB_STEP_SUMMARY", ""),
                        help="append the report to this file (default: $GITHUB_STEP_SUMMARY; "
                             "empty: none)")
    args = parser.parse_args(argv)
    try:
        build = read_build(args.build_dir)
    except (CheckError, KeyError, ValueError) as exc:
        print("FAIL: %s" % exc)
        return 1
    ram = static_ram(build["map"]) if args.ram else None
    text, ok = report(build, args.limit, ram)
    print(text, end="")
    if args.ram and ram is None:
        print("(static RAM not reported: no linker map or no esp_idf_size)")
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
