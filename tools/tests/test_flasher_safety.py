"""FLS-010: the Flasher can reach esptool only through its allow-list. Nothing in it can burn
eFuses, turn on secure boot or flash encryption, or reach espefuse/espsecure; every firmware
board has an entry in the Flasher's catalogue; and its power advice is the board maker's
(FLS-026)."""
import ast
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / "tools" / "flasher" / "roboflash"
sys.path.insert(0, str(PKG.parent))

from roboflash.chip import ALLOWED_CMDS, ALLOWED_LOADER  # noqa: E402

FORBIDDEN_MODULES = {"espefuse", "espsecure", "esptool.cmds.burn_efuse"}
FORBIDDEN_WORDS = ("burn_efuse", "burn_key", "secure_boot", "flash_encryption", "encrypt_files",
                   "ignore_flash_enc_efuse", "write_protect_efuse")


class SafetyTests(unittest.TestCase):
    def test_chip_uses_only_the_allowed_esptool_calls(self):
        tree = ast.parse((PKG / "chip.py").read_text(encoding="utf-8"))
        cmds, loader = set(), set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id == "cmds":
                cmds.add(node.attr)
            if isinstance(node, ast.Attribute):
                v = node.value
                if (isinstance(v, ast.Name) and v.id == "esp") or (isinstance(v, ast.Attribute) and v.attr == "esp"):
                    loader.add(node.attr)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "write_flash":
                self.assertFalse({k.arg for k in node.keywords} & {"encrypt", "encrypt_files", "force",
                                                                   "ignore_flash_enc_efuse"})
        self.assertLessEqual(cmds, ALLOWED_CMDS, cmds - ALLOWED_CMDS)
        self.assertLessEqual(loader, ALLOWED_LOADER, loader - ALLOWED_LOADER)
        self.assertNotIn("burn", " ".join(ALLOWED_CMDS | ALLOWED_LOADER))

    def test_the_esptool_logger_keeps_esptools_progress(self):
        """esptool reads and writes with `with log.progress(total=...)`: the Flasher's logger must
        not hide that method (it once did, with an attribute of the same name)."""
        try:
            import esp_pylib.logger  # noqa: F401  (comes with esptool 5)
        except ImportError:
            self.skipTest("esptool is not installed here")
        from roboflash.chip import _make_logger
        said, seen = [], []
        log = _make_logger(said.append)
        log.on_progress = lambda cur, total: seen.append((cur, total))
        with log.progress(total=100, unit="B") as task:
            task.update(advance=40)
            task.update(advance=60)
        log.print("Hash of data verified.")
        self.assertEqual(seen[-1], (100, 100))
        self.assertIn("Hash of data verified.", said)

    def test_no_module_reaches_efuse_or_secure_boot_tools(self):
        for path in PKG.rglob("*.py"):
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    names = {a.name for a in node.names}
                elif isinstance(node, ast.ImportFrom):
                    names = {node.module or ""}
                else:
                    continue
                for name in names:
                    self.assertFalse(any(name == f or name.startswith(f + ".") for f in FORBIDDEN_MODULES),
                                     f"{path.name} imports {name}")
            code = path.read_text(encoding="utf-8")
            for word in FORBIDDEN_WORDS:
                self.assertNotIn(word, code, f"{path.name} mentions {word}")

    def test_every_firmware_board_is_in_the_catalogue(self):
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        # A panel board's project sets ALARM_BOARD; firmware/c6_cp (the C6 co-processor) does not.
        firmware = {p.name for p in (ROOT / "firmware").iterdir() if (p / "CMakeLists.txt").exists()
                    and "set(ALARM_BOARD " in (p / "CMakeLists.txt").read_text(encoding="utf-8")}
        self.assertEqual(firmware - set(boards), set(), "add them to tools/flasher/roboflash/boards.json")
        for name, info in boards.items():
            for key in ("name", "long_name", "chip", "flash_size", "usb", "download_mode", "offered"):
                self.assertIn(key, info, f"{name}: {key}")
            self.assertIsInstance(info["offered"], bool, name)
            if not info["offered"]:
                self.assertTrue(info.get("not_offered"), f"{name}: say why it is not offered")

    def test_the_power_advice_is_the_makers(self):
        """FLS-026: a CrowPanel is plugged in the way Elecrow's wiki says - POWER on, this computer
        on UART0, the USB2.0 port on a supply as well (8 to 10 W with the screen on) - and a panel's
        own supply stays connected: the V1.1 schematic gives each 5 V input its own diode (DSS34),
        so none of it reaches the computer. Nothing tells the person to unplug a supply."""
        from roboflash.chip import explain
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        conn = boards["crowpanel_p4"]["connect"]
        self.assertEqual((conn["port"], conn["power_port"]), ("UART0", "USB2.0"))
        steps = " ".join(t["do"] + " " + t["why"] for t in conn["steps"])
        for words in ("POWER switch to ON", "marked UART0", "phone charger into the port marked USB2.0",
                      "own power supply? Keep it connected"):
            self.assertIn(words, steps)
        for name, info in boards.items():
            c = info.get("connect")
            if c is None:
                continue
            for key in ("port", "steps", "not_showing", "before_install", "more_power"):
                self.assertTrue(c.get(key), f"{name}: connect.{key}")
            for t in c["steps"] + c["not_showing"]:
                self.assertEqual(set(t), {"do", "why"}, name)
            self.assertNotIn("unplug", json.dumps(c).lower(), name)
            self.assertNotIn("power_note", info, f"{name}: the old advice is connect now")
        advice = explain(Exception("Failed to connect to ESP32-P4: No serial data received."))[1]
        self.assertNotIn("unplug", advice.lower())
        self.assertIn("power switch", advice)

    def test_the_unfinished_4_inch_is_not_offered(self):
        """Decided 2026-09-24: the E32R40T (docs/features/19) stays out of the Flasher until its work
        resumes. Set "offered": true in boards.json then, and change this test."""
        from roboflash.boards import load_catalogue, not_offered
        self.assertNotIn("e32r40t", load_catalogue())
        self.assertIn("e32r40t", not_offered())
        self.assertIn("crowpanel_p4", load_catalogue())

    def test_receiver_pins_match_the_firmware(self):
        """The pins the Flasher offers for `rf pin` are the board's wired terminals (RF-008)."""
        import re
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        for name, info in boards.items():
            m = re.search(r"#define BOARD_WIRED_TERMINALS\s+\{([^}]*)\}", board_pins_text(name))
            if m:
                terminals = [int(x) for x in m.group(1).split(",")]
                self.assertEqual(sorted(info["rf"]["pins"]), sorted(terminals), name)

    def test_the_receiver_wiring_is_the_firmwares(self):
        """Every pin the Flasher draws for a CC1101 or a receiver module is the one the firmware
        uses: the board description's .rf (board_desc.c, through board_pins.h)."""
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        for name, info in boards.items():
            rf = board_rf(name)
            want = info["rf"]
            self.assertEqual(want["default_gpio"], rf["data"], f"{name}: the data pin")
            cc = want.get("cc1101")
            self.assertIsNotNone(cc, f"{name}: the CC1101's wiring")
            self.assertEqual(cc["spi"], {k: rf[k] for k in ("sck", "mosi", "miso", "cs")}, name)
            self.assertEqual(cc["gdo0"], rf["data"], f"{name}: GDO0 is the data pin")
            by_role = {"SI (MOSI)": rf["mosi"], "SO (MISO)": rf["miso"], "SCLK": rf["sck"],
                       "CSn": rf["cs"], "GDO0": rf["data"]}
            for wire in cc.get("wires", []):
                if "gpio" in wire:
                    self.assertEqual(wire["gpio"], by_role.get(wire["pin"]), f"{name}: {wire}")
                if wire["pin"] == "VCC":
                    self.assertEqual(wire["to"], "3V3", f"{name}: a CC1101 is a 3.3 V part")
            for wire in want.get("module_wires", []):
                if wire["pin"] == "DATA":
                    self.assertEqual(wire["gpio"], rf["data"], name)
                if wire["pin"] == "VCC":
                    self.assertEqual(wire["to"], "3V3", f"{name}: DATA swings to VCC")


    def test_the_clock_module_wiring_is_the_firmwares(self):
        """FLS-022: the clock module's SDA and SCL the Flasher draws are the board description's
        .i2c (board_desc.c, through board_pins.h); a board without a connector draws none."""
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        for name, info in boards.items():
            i2c = board_block(name, "i2c")
            rtc = info.get("rtc")
            if i2c["sda"] == "BOARD_NO_PIN":
                self.assertIsNone(rtc, f"{name}: no I2C connector, so no clock module to wire")
                continue
            self.assertIsNotNone(rtc, f"{name}: its I2C connector takes a clock module")
            self.assertEqual((rtc["sda"], rtc["scl"]), (i2c["sda"], i2c["scl"]), name)
            by_pin = {"SDA": i2c["sda"], "SCL": i2c["scl"]}
            for wire in rtc["wires"]:
                if wire["pin"] in by_pin:
                    self.assertEqual(wire["gpio"], by_pin[wire["pin"]], f"{name}: {wire}")
                if wire["pin"] == "VCC":
                    self.assertEqual(wire["to"], "3V3", f"{name}: the bus is 3.3 V")


    def test_the_other_places_are_the_firmwares(self):
        """FLS-024, BRD-014, BRD-015, BRD-016: every other place the Flasher draws for a CC1101
        (rf.places), the clock module (rtc.places) or the Z-Wave radio (zwave.places) is one the
        board description offers (RF_PLACES, I2C_PLACES, ZWAVE_PLACES in board_desc.c), with the
        same pins - and the firmware offers no place the Flasher leaves out."""
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        for name, info in boards.items():
            fw_rf = board_places(name, "RF_PLACES")
            drawn_rf = info["rf"].get("places", {})
            self.assertEqual(sorted(drawn_rf), sorted(fw_rf), f"{name}: receiver places")
            for pid, place in fw_rf.items():
                want = drawn_rf[pid]
                self.assertEqual(want["spi"], {k: place[k] for k in ("sck", "mosi", "miso", "cs")}, f"{name} {pid}")
                self.assertEqual(want["gdo0"], place["data"], f"{name} {pid}: GDO0")
                by_role = {"SI (MOSI)": place["mosi"], "SO (MISO)": place["miso"], "SCLK": place["sck"],
                           "CSn": place["cs"], "GDO0": place["data"]}
                for wire in want["wires"]:
                    if "gpio" in wire:
                        self.assertEqual(wire["gpio"], by_role.get(wire["pin"]), f"{name} {pid}: {wire}")
                    if wire["pin"] == "VCC":
                        self.assertEqual(wire["to"], "3V3", f"{name} {pid}: a CC1101 is a 3.3 V part")
            fw_zw = board_places(name, "ZWAVE_PLACES")
            drawn_zw = (info.get("zwave") or {}).get("places", {})
            self.assertEqual(sorted(drawn_zw), sorted(fw_zw), f"{name}: Z-Wave radio places")
            for pid, place in fw_zw.items():
                want = drawn_zw[pid]
                self.assertEqual((want["tx"], want["rx"]), (place["tx"], place["rx"]), f"{name} {pid}")
                by_pin = {"RX": place["tx"], "TX": place["rx"]}   # crossed: its RX on the panel's TX
                for wire in want["wires"]:
                    if wire["pin"] in by_pin:
                        self.assertEqual(wire["gpio"], by_pin[wire["pin"]], f"{name} {pid}: {wire}")
                    if wire["pin"] == "VCC":
                        self.assertEqual(wire["to"], "3V3", f"{name} {pid}: the ZAC93 is a 3.3 V part")
            fw_i2c = board_places(name, "I2C_PLACES")
            drawn_i2c = (info.get("rtc") or {}).get("places", {})
            self.assertEqual(sorted(drawn_i2c), sorted(fw_i2c), f"{name}: clock module places")
            for pid, place in fw_i2c.items():
                want = drawn_i2c[pid]
                self.assertEqual((want["sda"], want["scl"], want["port"]),
                                 (place["sda"], place["scl"], place["port"]), f"{name} {pid}")
                by_pin = {"SDA": place["sda"], "SCL": place["scl"]}
                for wire in want["wires"]:
                    if wire["pin"] in by_pin:
                        self.assertEqual(wire["gpio"], by_pin[wire["pin"]], f"{name} {pid}: {wire}")
                    if wire["pin"] == "VCC":
                        self.assertEqual(wire["to"], "3V3", f"{name} {pid}: the bus is 3.3 V")

    def test_the_radio_socket_is_drawn_as_printed(self):
        """The Zigbee module's socket is drawn with each board's own printed labels (boards.json
        "socket_printed"): two rows of seven, the serial pair the Z-Wave radio's socket place uses
        at the top (IO53/IO54 on the 7" V1.1, IO27/IO28 on the 10.1" V1.2), and the pads the
        CC1101 and the receiver module use in it."""
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        for name, info in boards.items():
            if "zigbee" not in info:
                continue
            rows = info["zigbee"].get("socket_printed")
            self.assertTrue(rows, f"{name}: zigbee.socket_printed")
            self.assertEqual([len(r) for r in rows], [7, 7], name)
            labels = set(rows[0] + rows[1])
            z = info["zwave"]["places"]["socket"]
            self.assertEqual((rows[0][0], rows[1][0]), (f"IO{z['tx']}", f"IO{z['rx']}"), name)
            for w in info["rf"]["cc1101"]["wires"] + info["rf"]["module_wires"]:
                if "gpio" in w and w["to"].startswith("socket pad"):
                    self.assertIn(f"IO{w['gpio']}", labels, f"{name}: {w}")

    def test_the_custom_pins_are_the_firmwares(self):
        """FLS-029: the pins the Flasher offers for custom places (boards.json "custom") are the
        ones the firmware accepts (REACHABLE in board_desc.c), with the same labels and connector."""
        import re
        boards = json.loads((PKG / "boards.json").read_text(encoding="utf-8"))["boards"]
        checked = []
        for name, info in boards.items():
            src = ROOT / "components" / "boards" / f"board_{name}" / "src" / "board_desc.c"
            block = re.search(r"static const board_reach_t REACHABLE\[\] = \{(.*?)\n\};",
                              src.read_text(encoding="utf-8"), re.S) if src.exists() else None
            if block is None:
                self.assertNotIn("custom", info, f"{name}: custom pins the firmware does not list")
                continue
            fw = [{"gpio": int(g), "label": label, "connector": conn}
                  for g, label, conn in re.findall(r'\{(\d+), "(IO\d+)", "(J7)"\}', block.group(1))]
            self.assertTrue(fw, f"{name}: no entries parsed from REACHABLE")
            self.assertEqual(info["custom"]["pins"], fw, name)
            checked.append(name)
        self.assertEqual(sorted(checked), ["crowpanel_10", "crowpanel_p4"])


def board_rf(name):
    return board_block(name, "rf")


def board_pins_text(name):
    """components/boards/board_<name>/include/board_pins.h, with the headers it includes from a
    board family's library (crowpanel_p4_pins.h) in front of it."""
    import re
    boards = ROOT / "components" / "boards"
    text = (boards / f"board_{name}" / "include" / "board_pins.h").read_text(encoding="utf-8")
    shared = []
    for inc in re.findall(r'^#include\s+"([^"]+)"', text, re.M):
        found = sorted(boards.glob(f"*/include/{inc}"))
        assert len(found) == 1, f"{name}: {inc} found {len(found)} times under components/boards"
        shared.append(found[0].read_text(encoding="utf-8"))
    return "\n".join(shared + [text])


def board_defines(name):
    """Every numeric BOARD_* define the board's pins header gives, its family's included."""
    import re
    return {m.group(1): int(m.group(2)) for m in re.finditer(
        r"^#define\s+(BOARD_[A-Z0-9_]+)\s+(-?\d+)\b", board_pins_text(name), re.M)}


def board_places(name, array):
    """The entries of RF_PLACES or I2C_PLACES in components/boards/board_<name>/src/board_desc.c,
    resolved through its board_pins.h: {"j7": {"sck": 52, ...}} / {"j7": {"port": 1, "sda": 27,
    "scl": 28}}; {} when the board offers no other place."""
    import re
    base = ROOT / "components" / "boards" / f"board_{name}"
    defines = board_defines(name)
    m = re.search(array + r"\[\]\s*=\s*\{(.*?)\n\};", (base / "src" / "board_desc.c").read_text(encoding="utf-8"), re.S)
    if not m:
        return {}
    value = lambda v: int(v) if re.fullmatch(r"-?\d+", v) else defines.get(v, v)  # noqa: E731
    out = {}
    if array == "RF_PLACES":
        for pid, body in re.findall(r'\{"(\w+)",\s*\{(.*?)\}\}', m.group(1), re.S):
            out[pid] = {f: value(v) for f, v in re.findall(r"\.(\w+)\s*=\s*([A-Za-z0-9_-]+)", body)}
    elif array == "ZWAVE_PLACES":
        for pid, rest in re.findall(r'\{"(\w+)",\s*([^}]*)\}', m.group(1)):
            tx, rx = [value(x.strip()) for x in rest.split(",")[:2]]
            out[pid] = {"tx": tx, "rx": rx}
    else:
        for pid, rest in re.findall(r'\{"(\w+)",\s*([^}]*)\}', m.group(1)):
            port, sda, scl = [value(x.strip()) for x in rest.split(",")[:3]]
            out[pid] = {"port": port, "sda": sda, "scl": scl}
    return out


def board_block(name, block_name):
    """A block (.rf, .i2c) of components/boards/board_<name>/src/board_desc.c, with every macro
    resolved through its board_pins.h: {"sck": 8, "mosi": 6, "miso": 7, "cs": 9, "data": 10, ...}."""
    import re
    base = ROOT / "components" / "boards" / f"board_{name}"
    defines = board_defines(name)
    block = re.search(r"\." + block_name + r"\s*=\s*\{(.*?)\}", (base / "src" / "board_desc.c").read_text(encoding="utf-8"), re.S)
    assert block, f"{name}: no .{block_name} in board_desc.c"
    out = {}
    for field, value in re.findall(r"\.(\w+)\s*=\s*([A-Za-z0-9_-]+)", block.group(1)):
        out[field] = int(value) if re.fullmatch(r"-?\d+", value) else defines.get(value, value)
    return out


if __name__ == "__main__":
    unittest.main()
