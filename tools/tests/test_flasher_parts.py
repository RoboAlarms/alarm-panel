"""FLS-024, FLS-025: the connected parts (what is wired to the panel, the commands that set it up
for them, what a running panel says) and the factory reset's job. docs/features/25 RF-019/RF-020,
docs/features/35 RTC-013 from the Flasher's side."""
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


class RuleTests(unittest.TestCase):
    """The same rules the page's pin check shows (web/app.js), in the engine that applies them."""

    def test_the_layouts_the_research_names_are_fine(self):
        for choice in (
                {"rf": "module", "rf_place": "j7", "rf_pin": 3, "clock": True, "clock_place": "j13"},
                {"rf": "cc1101", "rf_place": "socket", "clock": True, "clock_place": "j13"},
                {"rf": "cc1101", "rf_place": "j7", "clock": True, "clock_place": "j7", "zigbee": True},
                {"rf": "none"}, {"rf": "auto"}, {}):
            self.assertEqual(parts.problems(choice, BOARD), [], choice)

    def test_every_suggested_layout_is_fine(self):
        """The page offers boards.json's layouts as one-click choices: each must pass the rules."""
        self.assertTrue(BOARD["layouts"])
        for layout in BOARD["layouts"]:
            self.assertEqual(parts.problems(layout["parts"], BOARD), [], layout["title"])

    def test_the_flashers_own_places_fit_every_mix_of_parts(self):
        """BL-117: the guided setup asks what is connected, never where; parts.recommended() (and
        the page's recommend()) must place any mix without a conflict, on every board offered
        (FLS-035: the ESP32-S3 board's parts each have one place; it has no Zigbee socket)."""
        for name, board in BOARDS.items():
            if not board.get("offered", True):
                continue
            for rf in ("none", "module", "cc1101"):
                for clock in (False, True) if board.get("rtc") else (False,):
                    for zwave in (False, True) if board.get("zwave") else (False,):
                        for zigbee in (False, True) if board.get("zigbee") else (False,):
                            what = {"rf": rf, "clock": clock, "zwave": zwave, "zigbee": zigbee}
                            placed = parts.recommended(what, board)
                            self.assertEqual(parts.problems(placed, board), [], (name, what, placed))

    def test_every_boards_suggested_layouts_are_fine(self):
        for name, board in BOARDS.items():
            for layout in board.get("layouts", []):
                self.assertEqual(parts.problems(layout["parts"], board), [], (name, layout["title"]))

    def test_the_recommended_places(self):
        """J7 for the receiver module (IO3) and the clock; one radio in the socket while it is free."""
        r = parts.recommended({"rf": "module", "clock": True}, BOARD)
        self.assertEqual((r["rf_place"], r["rf_pin"], r["clock_place"]), ("j7", 3, "j7"))
        self.assertEqual(parts.recommended({"rf": "module", "rf_pin": 5}, BOARD)["rf_pin"], 5)   # a pin it offers stays
        r = parts.recommended({"rf": "cc1101", "zwave": True, "clock": True}, BOARD)
        self.assertEqual((r["rf_place"], r["zwave_place"], r["clock_place"]), ("socket", "j7", "j13"))
        r = parts.recommended({"rf": "module", "zwave": True, "clock": True}, BOARD)
        self.assertEqual((r["zwave_place"], r["clock_place"]), ("socket", "j7"))
        r = parts.recommended({"rf": "cc1101", "zwave": True, "clock": True, "zigbee": True}, BOARD)
        self.assertEqual((r["rf_place"], r["zwave_place"], r["clock_place"]), ("j7", "j7", "j13"))

    def test_the_zigbee_module_takes_the_socket(self):
        self.assertIn("Zigbee module is in it",
                      parts.problems({"rf": "module", "rf_place": "socket", "zigbee": True}, BOARD)[0])
        self.assertIn("Zigbee module is in it",
                      parts.problems({"rf": "cc1101", "rf_place": "socket", "zigbee": True}, BOARD)[0])
        self.assertIn("can't be used with the Zigbee module",
                      parts.problems({"rf": "auto", "zigbee": True}, BOARD)[0])
        self.assertEqual(parts.problems({"rf": "module", "rf_place": "j7", "rf_pin": 3, "zigbee": True}, BOARD), [])

    def test_the_zwave_radio_and_its_neighbours(self):
        """docs/features/36 hardware.md §4: the socket's serial line while no Zigbee module is in
        it, or J7 IO27/IO28 while the clock isn't there; the panel's zw_place_ok says the same."""
        self.assertEqual(parts.problems({"zwave": True, "zwave_place": "socket"}, BOARD), [])
        self.assertEqual(parts.problems({"zwave": True, "zwave_place": "j7", "clock": True, "clock_place": "j13"}, BOARD), [])
        self.assertEqual(parts.problems({"zwave": True, "zwave_place": "socket", "rf": "cc1101", "rf_place": "socket"},
                                        BOARD), [])
        self.assertIn("Zigbee module is in it",
                      parts.problems({"zwave": True, "zwave_place": "socket", "zigbee": True}, BOARD)[0])
        self.assertIn("can't both use J7 IO27 and IO28",
                      parts.problems({"zwave": True, "zwave_place": "j7", "clock": True, "clock_place": "j7"}, BOARD)[0])
        self.assertIn("unknown place", parts.problems({"zwave": True, "zwave_place": "roof"}, BOARD)[0])
        no_zw = dict(BOARD)
        del no_zw["zwave"]
        self.assertIn("no place for a Z-Wave radio", parts.problems({"zwave": True}, no_zw)[0])
        self.assertEqual(parts.problems({"zwave": False, "zwave_place": "roof"}, no_zw), [])

    def test_only_offered_pins_and_known_places(self):
        self.assertTrue(parts.problems({"rf": "module", "rf_place": "j7", "rf_pin": 25}, BOARD))
        self.assertTrue(parts.problems({"rf": "module", "rf_place": "j7", "rf_pin": "x"}, BOARD))
        self.assertTrue(parts.problems({"rf": "cc1101", "rf_place": "roof"}, BOARD))
        self.assertTrue(parts.problems({"rf": "sx1262"}, BOARD))
        self.assertTrue(parts.problems({"clock": True, "clock_place": "j9"}, BOARD))
        no_places = json.loads(json.dumps(BOARD))
        del no_places["rf"]["places"]
        del no_places["rtc"]["places"]
        self.assertIn("no J7 place for a CC1101", parts.problems({"rf": "cc1101", "rf_place": "j7"}, no_places)[0])
        self.assertIn("no J7 place for the clock", parts.problems({"clock": True, "clock_place": "j7"}, no_places)[0])
        no_rtc = dict(BOARD)
        del no_rtc["rtc"]
        self.assertIn("no place for a clock", parts.problems({"clock": True}, no_rtc)[0])


class CommandTests(unittest.TestCase):
    def test_each_choice_and_its_order(self):
        """The pin is put back before a CC1101 moves to J7 and the place before a module's pin
        moves, so the panel checks each against the board as it will be."""
        z = ["zwave place off"]   # the Z-Wave radio's place goes first and comes back last
        cases = [
            ({"rf": "none"}, z + ["rf module off", "rf pin default", "rf place default", "rtc place default"]),
            ({"rf": "auto"}, z + ["rf module auto", "rf pin default", "rf place default", "rtc place default"]),
            ({"rf": "module", "rf_place": "socket"},
             z + ["rf module ook", "rf place default", "rf pin default", "rtc place default"]),
            ({"rf": "module", "rf_place": "j7", "rf_pin": 3, "clock": True},
             z + ["rf module ook", "rf place default", "rf pin 3", "rtc place default"]),
            ({"rf": "cc1101", "rf_place": "j7", "clock": True, "clock_place": "j7"},
             z + ["rf module cc1101", "rf pin default", "rf place j7", "rtc place j7"]),
            ({"rf": "cc1101", "rf_place": "socket", "clock": False, "clock_place": "j7"},
             z + ["rf module cc1101", "rf pin default", "rf place default", "rtc place default"]),
            ({"rf": "none", "clock": True, "clock_place": "j13", "zwave": True, "zwave_place": "j7"},
             z + ["rf module off", "rf pin default", "rf place default", "rtc place default", "zwave place j7"]),
            ({"rf": "none", "zwave": True, "zwave_place": "socket"},
             z + ["rf module off", "rf pin default", "rf place default", "rtc place default", "zwave place socket"]),
        ]
        for choice, want in cases:
            self.assertEqual(parts.commands(choice, BOARD), want, choice)
        no_rtc = dict(BOARD)
        del no_rtc["rtc"]
        del no_rtc["zwave"]
        self.assertEqual(parts.commands({"rf": "none"}, no_rtc), ["rf module off", "rf pin default", "rf place default"])
        self.assertTrue(parts.optional("zwave place off"))
        self.assertFalse(parts.optional("zwave place j7"))

    def test_replies(self):
        self.assertEqual(parts.reply_kind("rf module off", "receiver off from the next restart (`restart`)"), "ok")
        self.assertEqual(parts.reply_kind("rf place j7", "place j7 from the next restart (`restart`); its pins "
                                                          "are no longer offered as wired terminals"), "ok")
        self.assertEqual(parts.reply_kind("rf place j7", "not j7: zone 3 uses GPIO 52 as a wired terminal: "
                                                          "give that zone another terminal first"), "refused")
        self.assertEqual(parts.reply_kind("rf pin 3", "data on GPIO 3 from the next restart (`restart`)"), "ok")
        self.assertEqual(parts.reply_kind("rf pin 25", "not GPIO 25: this board offers GPIO 10"), "refused")
        self.assertEqual(parts.reply_kind("rtc place j7", "place j7 from the next restart (`restart`)"), "ok")
        # firmware from before these commands
        self.assertEqual(parts.reply_kind("rf place j7", "rf [status|retry|freq <kHz>|module auto|cc1101|ook|raw on|off|"
                                                          "capture <ms>]"), "old")
        self.assertEqual(parts.reply_kind("rf module off", "rf module auto|cc1101|ook"), "refused")
        self.assertEqual(parts.reply_kind("rtc place j7", "rtc [status] | rtc set <epoch seconds> | rtc check"), "old")
        self.assertIsNone(parts.reply_kind("rf pin 3", "W (812345) wifi: beacon timeout"))
        self.assertEqual(parts.reply_kind("zwave place j7", "place j7 from the next restart (`restart`); a reset "
                                                            "line stays where it was (`zwave reset-pin`)"), "ok")
        self.assertEqual(parts.reply_kind("zwave place off", "place off from the next restart (`restart`): no pin "
                                                             "is touched"), "ok")
        self.assertEqual(parts.reply_kind("zwave place j7", "not j7: zone 2 uses GPIO 27 as a wired terminal: give "
                                                            "that zone another terminal first"), "refused")
        self.assertEqual(parts.reply_kind("zwave place off", "Unrecognized command"), "old")


class ReadTests(unittest.TestCase):
    """What a running panel is set up for, from the lines `rf` and `rtc` print."""

    def test_a_module_on_j7_and_a_clock_on_j13(self):
        got = parts.read_current(
            ["Receiver module on GPIO 3, 12 edges/s, line 3% high",
             "module=ook receiver=receiver module edges/s=12 high=3% storms=0 place=default"],
            ["Clock module: keeping time, 24.25 C",
             "rtc module=ok source=internet trusted=1 lost_at_start=0 temp_c4=97 offset_ms=4 writes=2 "
             "write_failures=0 slews=0 jumps=0 epoch=1790260205 place=default"], BOARD)
        self.assertEqual((got["rf"], got["rf_place"], got["rf_pin"], got["clock"], got["clock_place"]),
                         ("module", "j7", 3, True, "j13"))
        self.assertTrue(got["rf_found"] and got["clock_found"])

    def test_a_cc1101_and_a_clock_on_j7_that_does_not_answer(self):
        got = parts.read_current(
            ["No CC1101 answers on SPI (GPIO 5 is its data)",
             "module=cc1101 receiver=none edges/s=0 high=0% storms=0 place=j7"],
            ["Clock module: not fitted (none answered on the J7 header, IO27 (SDA) and IO28 (SCL) at start-up)",
             "rtc module=absent source=none trusted=0 lost_at_start=0 temp_c4=0 offset_ms=? writes=0 "
             "write_failures=0 slews=0 jumps=0 epoch=0 place=j7"], BOARD)
        self.assertEqual((got["rf"], got["rf_place"], got["clock"], got["clock_place"]), ("cc1101", "j7", True, "j7"))
        self.assertFalse(got["rf_found"] or got["clock_found"])
        want = {"rf": "cc1101", "rf_place": "j7", "clock": True, "clock_place": "j7"}
        self.assertEqual(parts.differences(want, got, BOARD),
                         ["No clock module answered on the J7 header, IO27 (SDA) and IO28 (SCL): check its four "
                          "wires, then restart the panel."])

    def test_the_zwave_radio_from_its_status(self):
        running = ["place j7: J7 header, IO27 (TX) and IO28 (RX) - UART3 TX GPIO 27, RX GPIO 28, reset line none",
                   "radio: working", "crypto self-test: passed",
                   "module: firmware 1.10, Z-Wave 7.18.3 (static controller, library type 7), chip 08 v1"]
        self.assertEqual(parts.read_zwave(running), ("j7", True))
        moved = running[:1] + ["from the next restart: place socket, reset line -1",
                               "radio: not answering: check its wires; the panel keeps trying"]
        self.assertEqual(parts.read_zwave(moved), ("socket", False))
        taken_away = running[:1] + ["from the next restart: place off, reset line -1", "radio: working"]
        self.assertEqual(parts.read_zwave(taken_away)[0], None)
        off = ["Z-Wave radio off: no place chosen", "crypto self-test: not run (no place chosen)",
               "place socket from the next restart", "`zwave place` lists the places this board offers"]
        self.assertEqual(parts.read_zwave(off), ("socket", False))
        self.assertEqual(parts.read_zwave(["Unrecognized command"]), (None, False))
        got = parts.read_current(["module=off receiver=none edges/s=0 high=0% storms=0 place=default"], [], BOARD,
                                 running[:1] + ["radio: not answering: check its wires; the panel keeps trying"])
        self.assertEqual((got["zwave"], got["zwave_place"], got["zwave_found"]), (True, "j7", False))
        self.assertEqual(parts.differences({"zwave": True, "zwave_place": "j7"}, got, BOARD),
                         ["The Z-Wave radio doesn't answer on the J7 header, IO27 (TX) and IO28 (RX): check its four "
                          "wires (the panel's TX to the radio's RX, RX to its TX), then restart the panel."])
        self.assertEqual(parts.differences({"zwave": False}, got, BOARD), ["The panel still has a Z-Wave radio set up."])
        none = parts.read_current(["module=off receiver=none edges/s=0 high=0% storms=0 place=default"], [], BOARD, [])
        self.assertEqual(parts.differences({"zwave": True, "zwave_place": "socket"}, none, BOARD),
                         ["The panel has no Z-Wave radio set up: its firmware may be too old for it."])

    def test_off_and_older_firmware(self):
        off = parts.read_current(["433 MHz receiver: off (`rf module` turns it on)",
                                  "module=off receiver=none edges/s=0 high=0% storms=0 place=default"], [], BOARD)
        self.assertEqual((off["rf"], off["clock"]), ("none", False))
        old = parts.read_current(["Receiver module on GPIO 10, 900 edges/s, line 40% high",
                                  "module=auto receiver=receiver module edges/s=900 high=40% storms=0"],
                                 ["rtc module=absent source=none trusted=0 lost_at_start=0 temp_c4=0 offset_ms=? "
                                  "writes=0 write_failures=0 slews=0 jumps=0 epoch=0"], BOARD)
        self.assertEqual((old["rf"], old["rf_place"], old["clock"], old["clock_place"]), ("auto", "socket", False, "j13"))
        self.assertEqual(parts.differences({"rf": "cc1101", "rf_place": "j7"}, old, BOARD),
                         ["The panel reports its 433 MHz receiver as looking for either receiver."])


class ApplyTests(unittest.TestCase):
    """The whole run against a pretend panel: settings, one restart, read back (FLS-024)."""

    def test_the_recommended_layout_on_a_running_panel(self):
        from roboflash.controller import Controller
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                choice = {"rf": "cc1101", "rf_place": "j7", "clock": True, "clock_place": "j7", "zigbee": True}
                with self.assertRaisesRegex(JobFailed, "Zigbee module is in it"):
                    c.parts_apply("COM6", dict(choice, rf_place="socket"))
                c.parts_apply("COM6", choice)
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and c.parts_jobs["COM6"]["state"] == "running":
                    time.sleep(0.2)
                st = c.parts_jobs["COM6"]
                self.assertEqual(st["state"], "done", st)
                dev = world.devices["COM6"]
                self.assertEqual((dev.rf_module, dev.rf_place, dev.rf_pin, dev.clock_place),
                                 ("cc1101", "j7", 5, "j7"))
                self.assertEqual([s["state"] for s in st["steps"]], ["done", "done", "done"])
                self.assertEqual(c.parts_read("COM6")["parts"]["rf_place"], "j7")
            finally:
                c.stop()

    def test_the_zwave_radio_moves_to_j7_after_the_clock_leaves_it(self):
        """The clock's J7 place is given up before the Z-Wave radio takes the pins (the panel
        refuses them twice), and the radio is read back answering."""
        from roboflash.controller import Controller
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                dev = world.devices["COM6"]
                dev.clock_place_stored = dev.clock_place = "j7"
                c.parts_apply("COM6", {"rf": "none", "clock": True, "clock_place": "j13", "zwave": True,
                                       "zwave_place": "j7"})
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and c.parts_jobs["COM6"]["state"] == "running":
                    time.sleep(0.2)
                st = c.parts_jobs["COM6"]
                self.assertEqual(st["state"], "done", st)
                self.assertEqual((dev.clock_place, dev.zwave_place), ("default", "j7"))
                self.assertTrue(st["found"]["zwave"])
                self.assertEqual(st["warnings"], [])
                got = c.parts_read("COM6")["parts"]
                self.assertEqual((got["zwave"], got["zwave_place"]), (True, "j7"))
            finally:
                c.stop()

    def test_refusal_stops_and_says_why(self):
        from roboflash.controller import Controller
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                real = world.devices["COM6"].command

                def refuse_place(cmd):
                    if cmd == "rf place j7":
                        world.devices["COM6"].emit(cmd + "\r\nnot j7: zone 3 uses GPIO 52 as a wired terminal: give "
                                                   "that zone another terminal first\r\nalarm> ")
                        return
                    real(cmd)
                world.devices["COM6"].command = refuse_place
                c.parts_apply("COM6", {"rf": "cc1101", "rf_place": "j7"})
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline and c.parts_jobs["COM6"]["state"] == "running":
                    time.sleep(0.2)
                st = c.parts_jobs["COM6"]
                self.assertEqual(st["state"], "failed")
                self.assertIn("Zone 3 uses GPIO 52", st["error"]["title"])
                self.assertEqual([s["state"] for s in st["steps"]], ["failed", "waiting", "waiting"])
                self.assertNotEqual(world.devices["COM6"].rf_place_stored, "j7")
            finally:
                c.stop()


class ResetTests(unittest.TestCase):
    """FLS-025: a factory reset is an install over RoboAlarms: the settings go, the whole chip is
    erased and written again, and it asks for the erase to be confirmed."""

    def test_reset_needs_the_erase_confirmed_and_erases_the_chip(self):
        from roboflash.controller import Controller
        with tempfile.TemporaryDirectory() as tmp:
            bundle = testkit.standard_bundle()
            world = MockWorld(bundle)
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp, boot_timeout=10)
            c.set_bundle(bundle)
            time.sleep(2.0)
            try:
                c.probe("COM6")
                time.sleep(1.5)
                key = c.device_dict("COM6")["image_key"]
                job = c.start_job("install", "COM6", image_key=key, backup=False, reset=True)
                self.assertEqual(job["title"], "Reset to factory defaults")
                while c.runner.busy():
                    time.sleep(0.2)
                self.assertEqual(c.runner.current.state, "failed")   # not confirmed: nothing written
                self.assertIn("erases its codes", c.runner.current.error["title"])
                world.devices["COM6"].board.flash[0x9000:0x9010] = b"settings-go-here"
                job = c.start_job("install", "COM6", image_key=key, backup=False, reset=True,
                                  erase_settings_ok=True)
                deadline = time.monotonic() + 60
                while c.runner.busy() and time.monotonic() < deadline:
                    time.sleep(0.2)
                self.assertEqual(c.runner.current.state, "done", c.runner.current.error)
                self.assertEqual(bytes(world.devices["COM6"].board.flash[0x9000:0x9010]), b"\xff" * 16)
                self.assertIn("erase", [s.key for s in c.runner.current.steps])
            finally:
                c.stop()


if __name__ == "__main__":
    unittest.main()
