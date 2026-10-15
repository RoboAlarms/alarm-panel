"""FLS-029: custom pins. The engine's rules for a layout on the board GPIOs a person wired their
own parts to, the probe that tells older firmware apart, the commands and their order, the
read-back (stored, used or refused), the zones named in the confirmation, and Install / Update
carrying the layout through to the read-back - against the mock panel, which follows the console
contract the firmware implements."""
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import parts, testkit  # noqa: E402
from roboflash.mock import MockWorld  # noqa: E402

BOARDS = json.loads((Path(__file__).resolve().parents[1] / "flasher" / "roboflash" / "boards.json")
                    .read_text(encoding="utf-8"))["boards"]
BOARD = BOARDS["crowpanel_p4"]

CC1101 = {"sck": 51, "mosi": 50, "miso": 52, "cs": 49, "gdo0": 5}
FULL = {"layout": "custom", "rf": "cc1101", "clock": True, "clock_place": "custom", "zwave": True,
        "pins": {"rf": CC1101, "clock": {"sda": 2, "scl": 3}, "zwave": {"tx": 27, "rx": 28, "reset": 4}}}


def _choice(**kw):
    c = json.loads(json.dumps(FULL))
    c.update(kw)
    return c


class CustomRuleTests(unittest.TestCase):
    """FLS-029: parts_check, the rules the tab and start_job share."""

    def test_a_whole_layout_is_fine(self):
        self.assertEqual(parts.parts_check(FULL, BOARD), [])
        self.assertEqual(parts.problems(FULL, BOARD), [])     # problems() runs the same rules
        ook = {"layout": "custom", "rf": "module", "pins": {"rf": {"data": 4}}, "zigbee": True}
        self.assertEqual(parts.parts_check(ook, BOARD), [])    # the Zigbee module only takes its socket
        no_reset = _choice(pins=dict(FULL["pins"], zwave={"tx": 27, "rx": 28, "reset": None}))
        self.assertEqual(parts.parts_check(no_reset, BOARD), [])
        clock_j13 = {"layout": "custom", "rf": "none", "clock": True, "clock_place": "j13"}
        self.assertEqual(parts.parts_check(clock_j13, BOARD), [])

    def test_each_refusal(self):
        cases = [
            ("3 pins still to choose", _choice(pins={"rf": {"sck": 51, "mosi": 50}, "clock": {"sda": 2, "scl": 3},
                                                      "zwave": {"tx": 27, "rx": 28}})),
            ("1 pin still to choose", _choice(pins=dict(FULL["pins"], clock={"sda": 2}))),
            ("IO50 is chosen twice: CC1101 MOSI and Z-Wave TX",
             _choice(pins=dict(FULL["pins"], zwave={"tx": 50, "rx": 28}))),
            ("IO2 is chosen 3 times: CC1101 GDO0, Clock SDA and Z-Wave reset",
             _choice(pins=dict(FULL["pins"], rf=dict(CC1101, gdo0=2), zwave={"tx": 27, "rx": 28, "reset": 2}))),
            ("CC1101 SCK can't be on GPIO 8", _choice(pins=dict(FULL["pins"], rf=dict(CC1101, sck=8)))),
            ("Clock SDA can't be on GPIO 25", _choice(pins=dict(FULL["pins"], clock={"sda": 25, "scl": 3}))),
            ("can't be on that pin", _choice(pins=dict(FULL["pins"], clock={"sda": "IO2", "scl": 3}))),
            ("“Not sure” can't be used with your own pins", _choice(rf="auto")),
            ("The clock: choose the I2C connector or your own pins", _choice(clock_place="j7")),
            ("This board has no pins for your own wiring", FULL),
            ("Unknown layout", {"layout": "mixed"}),
        ]
        for words, choice in cases:
            board = BOARDS["e32r40t"] if words.startswith("This board has no pins") else BOARD
            with self.subTest(words):
                got = parts.parts_check(choice, board)
                self.assertTrue(got and any(words in g for g in got), got)
                self.assertEqual(parts.problems(choice, board), got)

    def test_nothing_on_its_own_pins_is_named_places(self):
        """FLS-029: a custom layout with no part on its own pins reads back as named places, so it
        is one (no false "too old" warning after it is applied)."""
        for choice in ({"layout": "custom", "rf": "none", "clock": True, "clock_place": "j13"},
                       {"layout": "custom", "rf": "none"}):
            c = parts.normalise(choice)
            self.assertEqual(c["layout"], "places")
            self.assertNotIn("pins", c)
            got = {"rf": "none", "clock": choice.get("clock", False), "clock_place": "j13",
                   "clock_found": True}
            self.assertTrue(parts.same(choice, got))
            self.assertEqual(parts.differences(choice, got, BOARD), [])
        # ...but a clock whose place isn't chosen yet is still asked for, and auto is still refused
        self.assertIn("The clock: choose the I2C connector or your own pins.",
                      parts.parts_check({"layout": "custom", "rf": "none", "clock": True, "clock_place": ""}, BOARD))
        self.assertTrue(parts.parts_check({"layout": "custom", "rf": "auto"}, BOARD))

    def test_normalise_keeps_a_custom_choice(self):
        c = parts.normalise(dict(FULL, zwave_place="socket", rf_place="j7"))
        self.assertEqual((c["layout"], c["zwave_place"]), ("custom", "custom"))
        self.assertEqual(c["pins"], {"rf": CC1101, "clock": {"sda": 2, "scl": 3},
                                     "zwave": {"tx": 27, "rx": 28, "reset": 4}})
        self.assertEqual(parts.normalise(c), c)               # what the page gets back, it can send again
        # pins of a part not in the layout are dropped; numbers as text are numbers
        c = parts.normalise({"layout": "custom", "rf": "module", "pins": {"rf": {"data": "4"}, "clock": {"sda": 2}}})
        self.assertEqual(c["pins"], {"rf": {"data": 4}})
        # named places: the layout is "places", and nothing else changes
        self.assertEqual(parts.normalise({"rf": "none"})["layout"], "places")
        self.assertNotIn("pins", parts.normalise({"rf": "none"}))
        self.assertEqual(parts.custom_gpios(FULL), {51, 50, 52, 49, 5, 2, 3, 27, 28, 4})
        self.assertEqual(parts.custom_gpios({"rf": "module", "rf_place": "j7", "rf_pin": 4}), set())


class CustomCommandTests(unittest.TestCase):
    def test_the_custom_sequence(self):
        """FLS-029: the probe, everything taken away, then each custom place."""
        self.assertEqual(parts.commands(FULL, BOARD), [
            "rf place",
            "zwave place off", "rf module off", "rf pin default", "rf place default", "rtc place default",
            "rf place custom sck=51 mosi=50 miso=52 cs=49 gdo0=5",
            "rtc place custom sda=2 scl=3",
            "zwave place custom tx=27 rx=28 reset=4"])
        ook = {"layout": "custom", "rf": "module", "clock": True, "clock_place": "j13", "zwave": True,
               "pins": {"rf": {"data": 4}, "zwave": {"tx": 27, "rx": 28}}}
        self.assertEqual(parts.commands(ook, BOARD)[-2:], ["rf place custom data=4", "zwave place custom tx=27 rx=28"])
        # Nothing on its own pins: named places, the way the panel reads it back (no probe needed).
        self.assertEqual(parts.commands({"layout": "custom", "rf": "none"}, BOARD),
                         ["zwave place off", "rf module off", "rf pin default", "rf place default",
                          "rtc place default"])
        self.assertTrue(parts.is_probe("rf place"))
        self.assertFalse(parts.is_probe("rf place default"))

    def test_named_places_after_custom_ones_take_them_away_first(self):
        """FLS-029: leaving custom pins, every part is taken away before a named place is checked
        against them; the named sequence follows unchanged."""
        named = {"rf": "module", "rf_place": "j7", "rf_pin": 3, "clock": True, "clock_place": "j13"}
        self.assertEqual(parts.commands(named, BOARD, from_custom=True), [
            "zwave place off", "rf module off", "rf pin default", "rf place default", "rtc place default",
            "rf module ook", "rf place default", "rf pin 3", "rtc place default"])
        self.assertEqual(parts.commands(named, BOARD), [
            "zwave place off", "rf module ook", "rf place default", "rf pin 3", "rtc place default"])

    def test_named_places_are_unchanged(self):
        """FLS-024's sequences, byte for byte (a golden list; also test_flasher_parts.py), with or
        without "layout": "places"."""
        golden = [
            ({"rf": "module", "rf_place": "j7", "rf_pin": 3, "clock": True},
             ["zwave place off", "rf module ook", "rf place default", "rf pin 3", "rtc place default"]),
            ({"rf": "cc1101", "rf_place": "j7", "clock": True, "clock_place": "j7"},
             ["zwave place off", "rf module cc1101", "rf pin default", "rf place j7", "rtc place j7"]),
            ({"rf": "none", "zwave": True, "zwave_place": "socket"},
             ["zwave place off", "rf module off", "rf pin default", "rf place default", "rtc place default",
              "zwave place socket"]),
        ]
        for choice, want in golden:
            self.assertEqual(parts.commands(choice, BOARD), want, choice)
            self.assertEqual(parts.commands(dict(choice, layout="places", pins={"rf": {"data": 4}}), BOARD), want)

    def test_replies(self):
        cmd = "rf place custom data=4"
        self.assertEqual(parts.reply_kind(cmd, "place custom from the next restart (`restart`)"), "ok")
        self.assertEqual(parts.reply_kind(cmd, "not custom: GPIO 50 is used twice (MOSI and CS)"), "refused")
        self.assertEqual(parts.reply_kind("zwave place custom tx=27 rx=28", "not custom: tx= is given twice"), "refused")
        self.assertEqual(parts.reply_kind("rtc place custom sda=2 scl=3", "rtc [status] | rtc set <epoch>"), "old")
        self.assertIsNone(parts.reply_kind(cmd, "place default: wireless-module socket"))
        refusal = "not ook: the receiver is on your own pins (`rf place custom`): `rf place default` first"
        self.assertEqual(parts.reply_kind("rf module ook", refusal), "refused")
        self.assertTrue(parts.custom_refused("rf module ook", refusal))
        self.assertEqual(parts.probe_line("custom: your own pins on J7 (`rf place custom data=`)"), "custom")
        self.assertEqual(parts.probe_line("`rf place <place>` moves the receiver there, `rf place default` puts "
                                          "it back"), "end")
        self.assertEqual(parts.probe_line("Unrecognized command"), "end")


RF_CUSTOM = ["CC1101 at 433.920 MHz, rx, noise -97 dBm",
             "custom sck=51 mosi=50 miso=52 cs=49 gdo0=5",
             "module=cc1101 receiver=CC1101 edges/s=1900 high=40% storms=0 place=custom"]
RTC_CUSTOM = ["Clock module: keeping time, 24.25 C", "custom sda=2 scl=3",
              "rtc module=ok source=module trusted=0 lost_at_start=0 temp_c4=97 offset_ms=? writes=0 "
              "write_failures=0 slews=0 jumps=0 epoch=1790260205 place=custom"]
ZW_CUSTOM = ["place custom: your own pins - UART3 TX GPIO 27, RX GPIO 28, reset line GPIO 4",
             "custom tx=27 rx=28 reset=4", "radio: working", "crypto self-test: passed",
             "module: firmware 1.10, Z-Wave 7.18.3 (static controller, library type 7), chip 08 v1"]


class CustomReadTests(unittest.TestCase):
    def test_the_round_trip(self):
        """FLS-029: what the panel prints for a stored custom layout reads back as that layout."""
        got = parts.read_current(RF_CUSTOM, RTC_CUSTOM, BOARD, ZW_CUSTOM)
        self.assertEqual(got["layout"], "custom")
        self.assertEqual(got["pins"], FULL["pins"])
        self.assertTrue(got["rf_found"] and got["clock_found"] and got["zwave_found"])
        self.assertNotIn("notes", got)
        self.assertTrue(parts.same(FULL, got))
        self.assertEqual(parts.differences(FULL, got, BOARD), [])
        self.assertEqual(parts.problems(got, BOARD), [])       # a Reset sends it back as it was read
        self.assertEqual(parts.normalise(got)["pins"], FULL["pins"])
        other = _choice(pins=dict(FULL["pins"], rf=dict(CC1101, gdo0=3)))
        self.assertFalse(parts.same(other, got))
        self.assertEqual(parts.differences(other, got, BOARD),
                         ["The panel reports its 433 MHz receiver as a CC1101 on SCK GPIO 51, MOSI GPIO 50, "
                          "MISO GPIO 52, CS GPIO 49, GDO0 GPIO 5."])
        no_reset = parts.read_current([], [], BOARD, ["custom tx=27 rx=28", "`zwave place` lists the places this "
                                                                             "board offers"])
        self.assertEqual(no_reset["pins"]["zwave"], {"tx": 27, "rx": 28, "reset": None})

    def test_a_refused_layout_says_so(self):
        """Failing closed: the part is off, the stored keys still read, and the reason is a note."""
        rf = ["433 MHz receiver: off (`rf module` turns it on)", "custom data=7",
              "custom place not used: GPIO 7 can't be reached on this board",
              "module=off receiver=none edges/s=0 high=0% storms=0 place=custom"]
        got = parts.read_current(rf, [], BOARD, [])
        self.assertEqual((got["layout"], got["rf"], got["pins"]["rf"]), ("custom", "module", {"data": 7}))
        self.assertEqual(got["notes"], ["The panel doesn't use your pins for the 433 MHz receiver (GPIO 7 can't be "
                                        "reached on this board), so it left it off."])
        want = {"layout": "custom", "rf": "module", "pins": {"rf": {"data": 7}}}
        self.assertEqual(parts.differences(want, got, BOARD)[0], got["notes"][0])

    def test_named_places_read_as_today(self):
        rf = ["module=cc1101 receiver=CC1101 edges/s=0 high=0% storms=0 place=j7"]
        got = parts.read_current(rf, [], BOARD, [])
        self.assertNotIn("layout", got)
        self.assertNotIn("pins", got)
        self.assertEqual(parts.differences({"layout": "custom", "rf": "module", "pins": {"rf": {"data": 4}}}, got, BOARD),
                         ["The panel doesn't report your own pins: its RoboAlarms may be too old for them."])
        self.assertFalse(parts.same({"layout": "custom", "rf": "cc1101", "pins": {"rf": CC1101}}, got))

    def test_zones_on_the_pins(self):
        terminals = ["GPIO 2   J7 header  closed", "GPIO 4   J7 header  open    zone 3",
                     "GPIO 51  J7 header  closed  zone 12",
                     "an open loop reads open; wire each contact between its GPIO and GND (3.3 V logic)"]
        zones = ["  3 p1 Back door                perimeter          door         normal",
                 "      sensor: GPIO 4 on the J7 header",
                 " 12 p2 Shed perimeter beam      perimeter          motion       fault bypassed"]
        got = parts.zones_on(FULL, terminals + zones, terminals + zones)
        self.assertEqual(got, [{"zone": 3, "name": "Back door", "gpio": 4},
                               {"zone": 12, "name": "Shed perimeter beam", "gpio": 51}])
        self.assertEqual(parts.zones_on({"rf": "module", "rf_place": "j7", "rf_pin": 4}, terminals, zones), [])
        self.assertEqual(parts.zones_on(FULL, ["Unrecognized command"], ["Unrecognized command"]), [])


def _wait_parts(test, c, port, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and (c.parts_jobs.get(port) or {}).get("state") in (None, "running"):
        time.sleep(0.2)
    return c.parts_jobs[port]


class CustomPanelTests(unittest.TestCase):
    """FLS-029 against the mock panel, which keeps the console contract."""

    def _controller(self, tmp, **world):
        from roboflash.controller import Controller
        self.world = MockWorld(testkit.standard_bundle(), **world)
        c = Controller(lister=self.world.lister, opener=self.world.opener, connect=self.world.connect,
                       backups_dir=tmp, captures_dir=tmp, logs_dir=tmp, boot_timeout=10)
        time.sleep(2.0)
        return c

    def test_apply_custom_then_back_to_named_places(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                dev = self.world.devices["COM6"]
                dev.clock_place_stored = "j7"     # taken away before the custom places are set
                self.assertEqual(c.parts_check("COM6", FULL), [])
                self.assertIn("1 pin still to choose", c.parts_check("COM6", _choice(pins=dict(FULL["pins"],
                                                                                         clock={"sda": 2})))[0])
                c.parts_apply("COM6", FULL)
                st = _wait_parts(self, c, "COM6")
                self.assertEqual(st["state"], "done", st)
                self.assertEqual(st["warnings"], [])
                self.assertEqual((dev.rf_module, dev.rf_place, dev.clock_place, dev.zwave_place),
                                 ("cc1101", "custom", "custom", "custom"))
                self.assertEqual(dev.rf_module_stored, "off")    # older firmware never opens SPI
                got = c.parts_read("COM6")["parts"]
                self.assertEqual((got["layout"], got["pins"]), ("custom", FULL["pins"]))
                # Back to named places: `rf module cc1101` is refused while custom, so the place goes first.
                c.parts_apply("COM6", {"rf": "cc1101", "rf_place": "j7", "clock": True, "clock_place": "j13"})
                time.sleep(0.3)
                st = _wait_parts(self, c, "COM6")
                self.assertEqual(st["state"], "done", st)
                self.assertEqual((dev.rf_module, dev.rf_place, dev.clock_place, dev.zwave_place),
                                 ("cc1101", "j7", "default", None))
                self.assertEqual(dev.custom_stored, {})
                self.assertNotIn("layout", c.parts_read("COM6")["parts"])
            finally:
                c.stop()

    def test_named_places_after_custom_pins_that_clash(self):
        """FLS-029: the clock on IO3 (custom), then a receiver module on J7 IO3 (named): the custom
        places go first, so the named pin isn't refused half way."""
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                dev = self.world.devices["COM6"]
                c.parts_apply("COM6", FULL)
                self.assertEqual(_wait_parts(self, c, "COM6")["state"], "done")
                self.assertEqual(c.parts_read("COM6")["parts"]["layout"], "custom")
                c.parts_apply("COM6", {"rf": "module", "rf_place": "j7", "rf_pin": 3, "clock": True,
                                       "clock_place": "j13"})
                time.sleep(0.3)
                st = _wait_parts(self, c, "COM6")
                self.assertEqual(st["state"], "done", st)
                self.assertEqual((dev.rf_module_stored, dev.rf_pin_stored, dev.clock_place_stored, dev.custom_stored),
                                 ("ook", 3, "default", {}))
                # without the take-away the mock refuses it, as the firmware does
                c.parts_apply("COM6", FULL)
                _wait_parts(self, c, "COM6")
                c.custom_seen.discard("COM6")
                c.parts_apply("COM6", {"rf": "module", "rf_place": "j7", "rf_pin": 3})
                time.sleep(0.3)
                self.assertEqual(_wait_parts(self, c, "COM6")["state"], "failed")
            finally:
                c.stop()

    def test_a_probe_without_an_answer_offers_no_apply(self):
        """FLS-029: parts_read never fails on the probe: a panel that doesn't say gets custom_ok
        False (Update with these pins), and its parts are still read."""
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                dev = self.world.devices["COM6"]
                real = dev.command
                dev.command = lambda cmd: None if cmd == "rf place" else real(cmd)
                got = c.parts_read("COM6")
                self.assertFalse(got["custom_ok"])
                self.assertIn("rf", got["parts"])
            finally:
                c.stop()

    def test_a_refusal_stops_and_says_why(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                dev = self.world.devices["COM6"]
                real = dev.command

                def refuse(cmd):
                    if cmd.startswith("rtc place custom"):
                        dev.emit(cmd + "\r\nnot custom: GPIO 2 is the 433 MHz receiver DATA\r\nalarm> ")
                        return
                    real(cmd)
                dev.command = refuse
                c.parts_apply("COM6", FULL)
                st = _wait_parts(self, c, "COM6")
                self.assertEqual(st["state"], "failed")
                self.assertEqual(st["error"]["title"], "GPIO 2 is the 433 MHz receiver DATA.")
                self.assertIn("rtc place custom sda=2 scl=3", st["error"]["advice"])
                self.assertNotIn("zwave", dev.custom_stored)      # nothing after it was sent
                # the panel's own check: a pin it can't reach is refused (fails closed)
                dev.command = real
                lines = []
                c._console("COM6").listeners.append(lambda line: lines.append(line.text.strip()))
                c.console_send("COM6", "rf place custom data=7")
                time.sleep(0.5)
                self.assertTrue(any(t.startswith("not custom: GPIO 7 can't be reached") for t in lines), lines)
            finally:
                c.stop()

    def test_a_stored_layout_the_panel_refuses_at_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            try:
                dev = self.world.devices["COM6"]
                dev.rf_place_stored, dev.rf_module_stored = "custom", "off"
                dev.custom_stored["rf"] = {"data": 7}             # damaged: not a pin this board offers
                dev.boot()
                time.sleep(2.5)
                c.probe("COM6")
                time.sleep(1.5)
                self.assertEqual(dev.rf_module, "off")            # never the default pins
                got = c.parts_read("COM6")["parts"]
                self.assertEqual((got["layout"], got["pins"]["rf"]), ("custom", {"data": 7}))
                self.assertIn("GPIO 7 can't be reached", got["notes"][0])
            finally:
                c.stop()

    def test_older_firmware_stops_after_the_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp, custom_pins=False)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                dev = self.world.devices["COM6"]
                sent = []
                real = dev.command
                dev.command = lambda cmd: (sent.append(cmd), real(cmd))
                before = (dev.rf_module_stored, dev.rf_place_stored, dev.clock_place_stored, dev.zwave_place_stored)
                c.parts_apply("COM6", FULL)
                st = _wait_parts(self, c, "COM6")
                self.assertEqual(st["state"], "failed")
                self.assertEqual((st["error"]["title"], st["error"]["advice"]), parts.TOO_OLD)
                self.assertEqual([s for s in sent if s.startswith(("rf", "rtc", "zwave"))], ["rf place"])
                self.assertEqual(before, (dev.rf_module_stored, dev.rf_place_stored, dev.clock_place_stored,
                                          dev.zwave_place_stored))
                # The tab asks before anything is sent: Update with these pins, not Apply.
                self.assertFalse(c.parts_read("COM6")["custom_ok"])
            finally:
                c.stop()

    def test_zones_named_in_the_confirmation(self):
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                self.assertEqual(c.parts_zones("COM6", FULL)["zones"], [{"zone": 3, "name": "Back door", "gpio": 4}])
                self.assertTrue(c.parts_read("COM6")["custom_ok"])      # the tab offers Apply
                self.assertEqual(c.parts_zones("COM6", _choice(zwave=False))["zones"], [])
                self.assertEqual(c.parts_zones("COM6", {"rf": "module", "rf_place": "j7", "rf_pin": 4})["zones"], [])
                self.assertEqual(c.parts_zones("COM8", FULL)["zones"], [])   # no RoboAlarms: never a block
            finally:
                c.stop()

    def test_install_and_update_carry_the_layout(self):
        """Install on a new board and Update an older RoboAlarms, each with the layout: set after
        the job with the same probe, sequence, restart and read-back."""
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            c = self._controller(tmp)
            c.set_bundle(testkit.standard_bundle())
            try:
                for kind, port, opt in (("update", "COM6", {}), ("install", "COM5", {"backup": False})):
                    with self.subTest(kind):
                        c.probe(port)
                        time.sleep(1.5)
                        deadline = time.monotonic() + 20
                        while c.devices[port].busy and time.monotonic() < deadline:
                            time.sleep(0.2)
                        key = c.device_dict(port)["image_key"] or "crowpanel_p4-v1-dev"
                        with self.assertRaisesRegex(JobFailed, "pins still to choose"):
                            c.start_job(kind, port, image_key=key, parts=_choice(pins={}), **opt)
                        try:
                            c.start_job(kind, port, image_key=key, parts=FULL, **opt)
                        except JobFailed as e:
                            self.fail(f"{e.title} {e.advice}")
                        self.assertEqual(c._parts_after[port]["layout"], "custom")
                        deadline = time.monotonic() + 60
                        while c.runner.busy() and time.monotonic() < deadline:
                            time.sleep(0.2)
                        self.assertEqual(c.runner.current.state, "done", c.runner.current.error)
                        st = _wait_parts(self, c, port, timeout=60)
                        self.assertEqual(st["state"], "done", st)
                        self.assertEqual(st["choice"]["pins"], FULL["pins"])
                        got = c.parts_read(port)["parts"]
                        self.assertEqual((got["layout"], got["pins"]), ("custom", FULL["pins"]))
            finally:
                c.stop()


if __name__ == "__main__":
    unittest.main()
