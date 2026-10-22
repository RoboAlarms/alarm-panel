"""FLS-035: the ESP32-S3 board without a screen (firmware/esp32s3_devkitc, the DevKitC-1 N16R8).
It is recognised by its chip (an ESP32-S3 with 16 MB), one image fits every S3 silicon, and its
port - the chip's own USB, 303A:1001, like the Zigbee module's - is looked at only once the
person says it is their board, and taken for a panel only once its chip reads as an S3 or its
RoboAlarms answers: an ESP32-H2 is never offered its image, nor the board the H2's. Each part has
one place, and after the install the Flasher reads its setup Wi-Fi's label (`web setup`), which
restarts nothing and types no code."""
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import parts, testkit  # noqa: E402
from roboflash.boards import ChipInfo, detect, load_catalogue, pick_image, verdict  # noqa: E402
from roboflash.controller import Controller, Device, parse_setup_label  # noqa: E402
from roboflash.fakechip import h2_v01, s3_v02  # noqa: E402
from roboflash.jobs import JobFailed  # noqa: E402
from roboflash.mock import MockWorld  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402

MB = 1024 * 1024
S3 = "esp32s3_devkitc"
KEY = "esp32s3_devkitc-v0-dev"
TICK = "Tick that the module is on its own first"


def n16r8(mac="f4:12:fa:00:01:0d"):
    return ChipInfo(chip="esp32s3", revision=2, flash_size=16 * MB, mac=mac,
                    description="ESP32-S3 (QFN56) (revision v0.2)")


def raw_port(device, vid, pid, serial=""):
    return SimpleNamespace(device=device, description=device, vid=vid, pid=pid, manufacturer="",
                           serial_number=serial, location="", hwid="")


def wait(c, port, seconds=20):
    deadline = time.time() + seconds
    time.sleep(0.05)
    while c.devices[port].busy and time.time() < deadline:
        time.sleep(0.05)


def wait_job(c, seconds=60):
    deadline = time.time() + seconds
    while c.runner.busy() and time.time() < deadline:
        time.sleep(0.1)
    return c.runner.current


class CatalogueTests(unittest.TestCase):
    def setUp(self):
        self.cat = load_catalogue()
        self.bundle = testkit.standard_bundle(s3=True)

    def test_the_board_is_offered_without_a_screen(self):
        b = self.cat[S3]
        self.assertEqual((b["chip"], b["flash_size"], b["headless"], b["wiring"]), ("esp32s3", "16MB", True, "table"))
        self.assertEqual([u["usb_id"] for u in b["usb"]], ["303A:1001"])
        # Plugged in by the port marked USB, powered by the computer: nothing else (FLS-026).
        conn = b["connect"]
        self.assertEqual(conn["port"], "USB")
        self.assertNotIn("power_port", conn)
        steps = " ".join(t["do"] + " " + t["why"] for t in conn["steps"])
        self.assertIn("port marked USB", steps)
        self.assertIn("Nothing else to plug in", steps)
        self.assertTrue(b["setup"]["steps"])
        self.assertNotIn("zigbee", b)

    def test_found_by_its_chip_and_never_an_h2(self):
        d = detect(self.cat, self.bundle, chip=n16r8())
        self.assertEqual((d.confidence, d.board, d.image_key), ("likely", S3, KEY))
        h2 = detect(self.cat, self.bundle, chip=h2_v01().info)
        self.assertEqual((h2.confidence, h2.board, h2.image_key), ("none", "", ""))
        small = detect(self.cat, self.bundle, chip=s3_v02().info)      # an S3 with 8 MB: not this board
        self.assertEqual((small.confidence, small.board), ("none", ""))
        sure = detect(self.cat, self.bundle, ProbeResult(port="COM13", answered=True, board=S3, chip_revision=2))
        self.assertEqual((sure.confidence, sure.board, sure.image_key), ("sure", S3, KEY))

    def test_one_image_for_every_silicon(self):
        # No v1/v3 builds as on the P4: the image is chosen without a revision too.
        self.assertEqual(pick_image(self.bundle, S3, None), KEY)
        self.assertEqual(pick_image(self.bundle, S3, 0), KEY)
        self.assertEqual(self.bundle.image(KEY).silicon_text(), "silicon up to v0.99")

    def test_images_never_cross_between_the_board_and_the_zigbee_module(self):
        s3_image = self.bundle.image(KEY)
        on_h2 = verdict(s3_image, self.cat, chip=h2_v01().info)
        self.assertFalse(on_h2.allowed)
        self.assertIn("for an ESP32-S3; the board has an ESP32-H2", on_h2.blocked)
        p4_on_s3 = verdict(self.bundle.image("crowpanel_p4-v1-dev"), self.cat, chip=n16r8())
        self.assertFalse(p4_on_s3.allowed)
        self.assertEqual(self.bundle.radio("zigbee").chip, "esp32h2")


class RoleTests(unittest.TestCase):
    """FLS-030, FLS-035: the board's port is maybe the Zigbee module until it is known."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.opened = []

        def opener():
            opened = self.opened

            class Never:
                port = None
                dtr = rts = False
                baudrate = timeout = None

                def open(self):
                    opened.append(self.port)
                    raise OSError("could not open port: the test allows no port to open")
            return Never()
        self.ports = [raw_port("COM13", 0x303A, 0x1001, "F4:12:FA:00:01:0D"), raw_port("COM5", 0x1A86, 0x7522)]
        self.c = Controller(lister=lambda: self.ports, opener=opener, connect=lambda *a: 1 / 0,
                            backups_dir=self.tmp.name, captures_dir=self.tmp.name, logs_dir=self.tmp.name)
        self.c.watcher.poll()
        self.c.set_bundle(testkit.standard_bundle(s3=True))

    def tearDown(self):
        self.c.stop()
        self.tmp.cleanup()

    def test_maybe_the_board_until_its_chip_says(self):
        d = self.c.device_dict("COM13")
        self.assertEqual((d["role"], d["maybe"], d["needs_alone"], d["maybe_panel"]),
                         ("unknown", "zigbee_module", True, [S3]))
        self.assertEqual(self.c.device_dict("COM5")["maybe_panel"], [])
        # Nothing looks at it unasked: not the plug-in look, not identify, not a panel's job.
        self.c.probe("COM13")
        time.sleep(0.2)
        with self.assertRaisesRegex(JobFailed, TICK):
            self.c.identify("COM13")
        with self.assertRaisesRegex(JobFailed, TICK):
            self.c.start_job("install", "COM13", image_key=KEY)
        self.assertEqual(self.opened, [])
        # Read as an ESP32-H2: the Zigbee module, never offered as the board.
        self.c.devices["COM13"].chip = h2_v01().info
        d = self.c.device_dict("COM13")
        self.assertEqual((d["role"], d["needs_alone"], d["maybe_panel"]), ("zigbee_module", True, []))
        # Read as the board's ESP32-S3: a panel, no longer maybe the module; the module's tools refuse it.
        self.c.devices["COM13"].chip = n16r8()
        d = self.c.device_dict("COM13")
        self.assertEqual((d["role"], d["maybe"], d["needs_alone"], d["maybe_panel"]), ("panel", "", False, []))
        self.assertEqual((d["detection"]["board"], d["image_key"]), (S3, KEY))
        for call in (lambda: self.c.radio_read("COM13", alone=True), lambda: self.c.radio_identify("COM13", alone=True),
                     lambda: self.c.start_job("radio", "COM13", alone=True)):
            with self.assertRaisesRegex(JobFailed, "This is a panel, not the Zigbee module"):
                call()
        # Another S3 (8 MB) is not this board: still maybe the module.
        self.c.devices["COM13"].chip = s3_v02().info
        self.assertTrue(self.c.device_dict("COM13")["needs_alone"])

    def test_a_board_found_is_known_when_plugged_in_again(self):
        self.c.devices["COM13"].chip = n16r8()
        self.c._remember_native("COM13")
        self.c.devices["COM13"] = Device("COM13")          # unplugged and back: nothing read yet
        self.assertEqual(self.c.role("COM13"), ("panel", ""))
        self.assertFalse(self.c.is_module_port("COM13"))
        # Another chip on its own USB (another serial number) is still maybe the module.
        self.ports.append(raw_port("COM9", 0x303A, 0x1001, "74:4D:BD:00:00:01"))
        self.c.watcher.poll()
        self.assertTrue(self.c.is_module_port("COM9"))


class SetupLabelTests(unittest.TestCase):
    def test_the_label_as_the_firmware_prints_it(self):
        got = parse_setup_label([
            "SETUP stage unclaimed, setup Wi-Fi open",
            'SETUP label: Wi-Fi "RoboAlarms-7F3A", password Robo1234; open http://192.168.4.1/',
            "SETUP label: panel e8f60a000013",
            "SETUP label: certificate SHA-256 0123456789abcdef 0123456789abcdef 0123456789abcdef 0123456789abcdef"])
        self.assertEqual((got["ssid"], got["password"], got["address"], got.get("code"), got["stage"], got["open"]),
                         ("RoboAlarms-7F3A", "Robo1234", "http://192.168.4.1/", None, "unclaimed", True))
        self.assertEqual(parse_setup_label(["SETUP stage key (recovery key owed), setup Wi-Fi closed",
                                            'SETUP label: Wi-Fi "R", password p; open https://a/'])["recovery_owed"], True)

    def test_other_answers(self):
        self.assertEqual(parse_setup_label(["SETUP: this panel has a screen: no setup Wi-Fi"]),
                         {"known": True, "headless": False})
        self.assertEqual(parse_setup_label(["Unrecognized command"]), {"known": False})
        self.assertEqual(parse_setup_label(["WEB enabled=0 developer=0 HTTPS=443 HTTP80=off ID=x"]), {"known": False})
        self.assertIsNone(parse_setup_label([]))
        self.assertTrue(parse_setup_label(["SETUP stage ready, setup Wi-Fi closed"])["unreadable"])


class PartsTests(unittest.TestCase):
    """One place for each part: the receiver and the clock on their own pins, the Z-Wave radio on
    its one place ("header", ZWAVE_PLACES in board_desc.c)."""

    def setUp(self):
        self.board = load_catalogue()[S3]

    def test_every_part_has_its_one_place(self):
        r = parts.recommended({"rf": "cc1101", "clock": True, "zwave": True}, self.board)
        self.assertEqual((r["rf_place"], r["clock_place"], r["zwave_place"]), ("socket", "j13", "header"))
        self.assertEqual(parts.problems(r, self.board), [])
        self.assertEqual(parts.commands(r, self.board),
                         ["zwave place off", "rf module cc1101", "rf pin default", "rf place default",
                          "rtc place default", "zwave place header"])
        m = parts.recommended({"rf": "module", "rf_pin": 4}, self.board)
        self.assertEqual((m["rf_place"], m["rf_pin"]), ("socket", None))
        self.assertIn("rf pin default", parts.commands(m, self.board))

    def test_no_zigbee_module(self):
        self.assertIn("no socket for the Zigbee module", parts.problems({"zigbee": True}, self.board)[0])


class MockBoardTests(unittest.TestCase):
    """The whole run against the pretend board (run.py --mock --mock-headless): said to be the
    board, looked at, identified, installed, set up for its parts, its setup Wi-Fi read."""

    def test_from_its_makers_demo_to_its_setup_wifi(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = testkit.standard_bundle(s3=True)
            world = MockWorld(bundle, headless=True)
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp, boot_timeout=10)
            c.set_bundle(bundle)
            c.watcher.poll()
            time.sleep(2.0)
            try:
                d = c.device_dict("COM13")
                self.assertEqual((d["needs_alone"], d["maybe_panel"]), (True, [S3]))
                c.probe("COM13", alone=True)        # the person said it is their board: a look, no restart
                wait(c, "COM13")
                self.assertFalse(c.devices["COM13"].probe.answered)    # its maker's demo
                c.identify("COM13", alone=True)
                wait(c, "COM13")
                d = c.device_dict("COM13")
                self.assertEqual((d["role"], d["needs_alone"], d["detection"]["confidence"], d["image_key"]),
                                 ("panel", False, "likely", KEY))
                c.start_job("install", "COM13", image_key=KEY, backup=False)
                job = wait_job(c)
                self.assertEqual(job.state, "done", job.error)
                self.assertEqual(job.result["boot"]["board"], S3)
                c.probe("COM13")
                wait(c, "COM13")
                self.assertEqual(c.devices["COM13"].probe.board, S3)
                choice = parts.recommended({"rf": "cc1101", "clock": True, "zwave": True}, c.catalogue[S3])
                c.parts_apply("COM13", choice)
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and c.parts_jobs["COM13"]["state"] == "running":
                    time.sleep(0.2)
                st = c.parts_jobs["COM13"]
                self.assertEqual(st["state"], "done", st)
                dev = world.devices["COM13"]
                self.assertEqual((dev.rf_module, dev.rf_place, dev.clock_place, dev.zwave_place),
                                 ("cc1101", "default", "default", "header"))
                label = c.setup_label("COM13")
                self.assertEqual((label["ssid"], label.get("code"), label["open"]), ("RoboAlarms-7F3A", None, True))
                # Read-only: no restart, and the label's secrets stay out of the Flasher's log.
                log = "".join(p.read_text(encoding="utf-8") for p in Path(tmp).glob("flasher-*.log"))
                self.assertNotIn("Robo1234", log)
                # FLS-036: set up over USB instead of from a phone - the installer's access (a
                # recovery key back), the home Wi-Fi, then finish once it joined.
                with self.assertRaisesRegex(JobFailed, "12 to 64"):
                    c.setup_provision("COM13", "accounts", ["Installer", "too short"])
                got = c.setup_provision("COM13", "accounts", ["Installer", 'a "quoted" pass word'])
                self.assertEqual(got["key"], "KEYAB-CDEFG-HJKMN-PQRST-UVWXY")
                self.assertTrue(c.setup_provision("COM13", "network", ["Harbor House", "harbor house wifi"])["ok"])
                with self.assertRaisesRegex(JobFailed, "hasn't joined"):
                    c.setup_provision("COM13", "finish")
                time.sleep(1.2)
                self.assertEqual(c.setup_provision("COM13", "status")["wifi"], "connected")
                self.assertEqual(c.setup_provision("COM13", "finish")["ip"], "192.0.2.40")
                with self.assertRaisesRegex(JobFailed, "set up already"):
                    c.setup_provision("COM13", "network", ["Harbor House", "harbor house wifi"])
                # The passwords and the key never reach the log or the console's history.
                log = "".join(p.read_text(encoding="utf-8") for p in Path(tmp).glob("flasher-*.log"))
                shown = " ".join(line.text for line in c.consoles["COM13"].history)
                for secret in ("quoted", "harbor house wifi", "KEYAB-CDEFG"):
                    self.assertNotIn(secret, log)
                    self.assertNotIn(secret, shown)
                # The Zigbee module's job refuses the board.
                with self.assertRaisesRegex(JobFailed, "This is a panel, not the Zigbee module"):
                    c.start_job("radio", "COM13", alone=True)
            finally:
                c.stop()

    def test_a_panel_with_a_screen_has_no_setup_wifi(self):
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                self.assertEqual(c.setup_label("COM6"), {"known": True, "headless": False})
            finally:
                c.stop()


if __name__ == "__main__":
    unittest.main()
