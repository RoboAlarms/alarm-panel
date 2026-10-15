"""FLS-030: the Zigbee module's radio software. The module (Elecrow's ESP32-H2 board) is written
only over its own USB port (303A:1001), out of its socket, once the person ticks that it is on
its own: the Flasher never opens such a port by itself, refuses a panel, refuses another chip
before erasing anything, keeps a copy only when asked, writes and checks the three parts, and
reads what the module runs. The panel's `zigbee status` is read from its console."""
import json
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import testkit  # noqa: E402
from roboflash.boards import ChipInfo, load_catalogue  # noqa: E402
from roboflash.controller import Controller, Device, parse_zigbee_status  # noqa: E402
from roboflash.fakechip import h2_v01, p4_v13, s3_v02  # noqa: E402
from roboflash.jobs import JobFailed, Runner, read_radio  # noqa: E402
from roboflash.mock import MockWorld  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402

MB = 1024 * 1024
TICK = "Tick that the module is on its own first"


def raw_port(device, vid, pid):
    return SimpleNamespace(device=device, description=device, vid=vid, pid=pid, manufacturer="",
                           serial_number="", location="", hwid="")


def wait(job, seconds=30):
    deadline = time.time() + seconds
    while (job.state if hasattr(job, "state") else job()) == "running" and time.time() < deadline:
        time.sleep(0.02)
    return job


class Touches:
    """An opener and a connect that record every touch of a port, and touch nothing."""

    def __init__(self):
        self.opened = []
        self.connected = []

    def opener(self):
        touches = self

        class Never:
            port = None
            dtr = rts = False
            baudrate = timeout = None

            def open(self):
                touches.opened.append(self.port)
                raise OSError("could not open port: the test allows no port to open")
        return Never()

    def connect(self, port, log, cancel, baud):
        self.connected.append(port)
        raise RuntimeError("Failed to connect: the test allows no connect")


class RoleTests(unittest.TestCase):
    """FLS-030: the role of each port is decided here, never in the page; a 303A:1001 port is
    never opened, probed or restarted without the tick."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.touch = Touches()
        self.ports = [raw_port("COM5", 0x1A86, 0x7522), raw_port("COM3", 0x1A86, 0x7523),
                      raw_port("COM9", 0x303A, 0x1001), raw_port("COM8", 0x10C4, 0xEA60)]
        self.c = Controller(lister=lambda: self.ports, opener=self.touch.opener, connect=self.touch.connect,
                            backups_dir=self.tmp.name, captures_dir=self.tmp.name, logs_dir=self.tmp.name)
        self.c.watcher.poll()
        self.c.set_bundle(testkit.standard_bundle())

    def tearDown(self):
        self.c.stop()
        self.tmp.cleanup()

    def role(self, port):
        d = self.c.device_dict(port)
        return d["role"], d["maybe"]

    def test_roles(self):
        # FLS-030: a panel's USB bridge (boards.json: CH340K, CH340C) is a panel.
        self.assertEqual(self.role("COM5"), ("panel", ""))
        self.assertEqual(self.role("COM3"), ("panel", ""))
        # A 303A:1001 port is only maybe the module until its chip reads as an ESP32-H2.
        self.assertEqual(self.role("COM9"), ("unknown", "zigbee_module"))
        self.assertTrue(self.c.device_dict("COM9")["needs_alone"])
        self.assertFalse(self.c.device_dict("COM5")["needs_alone"])
        self.c.devices["COM9"].chip = h2_v01().info
        self.assertEqual(self.role("COM9"), ("zigbee_module", ""))
        self.c.devices["COM9"].chip = s3_v02().info
        self.assertEqual(self.role("COM9"), ("unknown", ""))
        # Anything else is unknown, until a RoboAlarms console answers on it.
        self.assertEqual(self.role("COM8"), ("unknown", ""))
        self.c.devices["COM8"].probe = ProbeResult(port="COM8", answered=True, board="crowpanel_p4")
        self.assertEqual(self.role("COM8"), ("panel", ""))
        self.assertIn("role", self.c.snapshot()["devices"]["COM9"])

    def test_a_module_port_is_never_opened_without_the_tick(self):
        # FLS-030: Connect (probe, identify, a panel's jobs) never takes the module.
        self.c.probe("COM9")
        time.sleep(0.2)
        self.assertEqual(self.c.devices["COM9"].busy, "")
        with self.assertRaisesRegex(JobFailed, TICK):
            self.c.identify("COM9")
        with self.assertRaisesRegex(JobFailed, TICK):
            self.c.radio_identify("COM9", alone=False)
        with self.assertRaisesRegex(JobFailed, TICK):
            self.c.radio_read("COM9", alone=False)
        for kind, opt in (("install", {"image_key": "crowpanel_p4-v1-dev"}), ("backup", {}),
                          ("radio", {"backup": True})):
            with self.assertRaisesRegex(JobFailed, TICK):
                self.c.start_job(kind, "COM9", **opt)
        with self.assertRaisesRegex(JobFailed, "doesn't restart the Zigbee module"):
            self.c.console_restart("COM9")
        self.assertEqual((self.touch.opened, self.touch.connected), ([], []))
        self.assertIsNone(self.c.runner.current)

    def test_a_panel_is_never_taken_for_the_module(self):
        # FLS-030: the module's read and job refuse a panel's port before touching it.
        for port in ("COM5", "COM3"):
            with self.assertRaisesRegex(JobFailed, "This is a panel, not the Zigbee module"):
                self.c.start_job("radio", port, alone=True)
            with self.assertRaisesRegex(JobFailed, "This is a panel, not the Zigbee module"):
                self.c.radio_read(port, alone=True)
        self.assertEqual((self.touch.opened, self.touch.connected), ([], []))

    def test_the_module_tools_only_on_a_module_port(self):
        # FLS-030: the module's read, identify and job need its own USB (303A:1001), even ticked;
        # the tick is exactly true; no console (RF test, Zigbee status, parts, clock) opens it.
        for call in (lambda: self.c.radio_read("COM8", alone=True),
                     lambda: self.c.radio_identify("COM8", alone=True),
                     lambda: self.c.start_job("radio", "COM8", alone=True)):
            with self.assertRaisesRegex(JobFailed, "This port isn't the Zigbee module's"):
                call()
        with self.assertRaisesRegex(JobFailed, TICK):
            self.c.start_job("radio", "COM9", alone="true")
        for call in (lambda: self.c.console_open("COM9"), lambda: self.c.rf_start("COM9"),
                     lambda: self.c.zigbee_status("COM9"), lambda: self.c.parts_read("COM9"),
                     lambda: self.c.clock_status("COM9")):
            with self.assertRaisesRegex(JobFailed, "the Zigbee module's port, not a panel's"):
                call()
        self.assertEqual((self.touch.opened, self.touch.connected), ([], []))

    def test_no_radio_in_the_bundle(self):
        self.c.set_bundle(testkit.standard_bundle(radio=False))
        with self.assertRaisesRegex(JobFailed, "no radio software"):
            self.c.start_job("radio", "COM9", alone=True)
        self.assertEqual(self.touch.connected, [])

    def test_console_restart_refuses_the_module(self):
        # FLS-030: the Console's restart (its reset line) never runs for the module.
        self.c.devices["COM9"].chip = h2_v01().info
        with self.assertRaisesRegex(JobFailed, "doesn't restart the Zigbee module"):
            self.c.console_restart("COM9")
        self.c.devices["COM8"].chip = h2_v01().info     # an H2 on another bridge
        with self.assertRaisesRegex(JobFailed, "doesn't restart the Zigbee module"):
            self.c.console_restart("COM8")
        self.assertEqual(self.touch.opened, [])


class FakeSession:
    def __init__(self, board):
        self.info = board.info
        self.board = board

    def read(self, offset, size, progress=None):
        return bytes(self.board.flash[offset:offset + size])


class ReadRadioTests(unittest.TestCase):
    """FLS-030: what the module runs, from its partition table and app description."""

    def setUp(self):
        self.radio = testkit.synthetic_radio()

    def read(self, flash, board=h2_v01):
        return read_radio(FakeSession(board(flash=flash)), self.radio)

    def test_factory_ours_older_same_other_and_empty(self):
        got = self.read(testkit.radio_flash(project="esp_ot_cli", version="v5.4.1"))
        self.assertEqual((got["chip"], got["revision"], got["runs"], got["project"], got["version"],
                          got["older"], got["same"]), ("esp32h2", 1, "factory", "esp_ot_cli", "v5.4.1", False, False))
        self.assertEqual(self.read(testkit.radio_flash(project="ot_rcp", version="1"))["runs"], "factory")
        older = self.read(testkit.radio_flash(version="0.9.0"))
        self.assertEqual((older["runs"], older["older"], older["same"]), ("ours", True, False))
        # The same version and build: the bundle's own app written at 0x10000.
        flash = testkit.radio_flash()
        app = self.radio.part("app").data
        flash[0x10000:0x10000 + len(app)] = app
        same = self.read(flash)
        self.assertEqual((same["runs"], same["version"], same["older"], same["same"]), ("ours", "1.0.0", False, True))
        newer = self.read(testkit.radio_flash(version="1.2.0"))
        self.assertEqual((newer["runs"], newer["older"], newer["same"]), ("ours", False, False))
        other = self.read(testkit.radio_flash(project="hello_world", version="1.0"))
        self.assertEqual(other["runs"], "other")
        # Our build from before the module had a version (a commit name) is older, not newer.
        early = self.read(testkit.radio_flash(version="171f515-dirty"))
        self.assertEqual((early["runs"], early["older"], early["same"]), ("ours", True, False))
        # Without radio software in the bundle, ours is still ours.
        alone = read_radio(FakeSession(h2_v01(flash=testkit.radio_flash(version="0.9.0"))), None)
        self.assertEqual((alone["runs"], alone["older"], alone["same"]), ("ours", False, False))

    def test_erased_invalid_or_missing_app(self):
        self.assertEqual(self.read(bytearray(b"\xff" * 4 * MB))["runs"], "empty")
        damaged = testkit.radio_flash()
        damaged[0x8000:0x8002] = b"\x00\x00"       # not a table's magic
        self.assertEqual(self.read(damaged)["runs"], "empty")
        no_app = testkit.radio_flash(table=[("nvs", 1, 2, 0x9000, 0x6000)])
        self.assertEqual(self.read(no_app)["runs"], "empty")
        erased_app = testkit.radio_flash()
        erased_app[0x10000:0x20000] = b"\xff" * 0x10000
        self.assertEqual(self.read(erased_app)["runs"], "empty")
        not_an_app = testkit.radio_flash()
        not_an_app[0x10020:0x10024] = b"\x00" * 4      # an image without an app description
        self.assertEqual(self.read(not_an_app)["runs"], "other")
        ota = testkit.radio_flash(table=[("nvs", 1, 2, 0x9000, 0x6000), ("ota_0", 0, 0x10, 0x10000, 0x100000)])
        self.assertEqual(self.read(ota)["runs"], "ours")

    def test_another_chip_is_refused(self):
        with self.assertRaisesRegex(JobFailed, r"This isn't a Zigbee module \(it is an ESP32-S3\)"):
            read_radio(FakeSession(s3_v02()), self.radio)


class RadioJobTests(unittest.TestCase):
    """FLS-030: the radio job over a fake chip."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.radio = testkit.synthetic_radio()
        self.board = h2_v01(flash=testkit.radio_flash(project="esp_ot_cli", version="v5.4.1"))
        self.probe = ProbeResult(port="COM9")
        self.runner = Runner(emit=lambda job: None, catalogue=load_catalogue(),
                             connect=lambda port, log, cancel, baud: self.board.connect(port, log, cancel, baud),
                             probe=lambda port: self.probe, backups_dir=self.tmp.name, opener=None)

    def tearDown(self):
        self.tmp.cleanup()

    def test_erases_writes_the_three_parts_checks_and_restarts(self):
        job = wait(self.runner.radio("COM9", self.radio))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual([s.label for s in job.steps],
                         ["Connect to the module", "Erase it", "Write the radio software",
                          "Check what was written", "Restart it"])
        self.assertTrue(all(s.state == "done" for s in job.steps))
        self.assertEqual(self.board.reads, [])            # no copy unless asked (FLS-030)
        self.assertEqual(self.board.erases, 1)
        self.assertEqual(self.board.writes, [(0x0, 16 * 1024), (0x8000, 0xC00), (0x10000, 64 * 1024)])
        for p in self.radio.parts:
            self.assertEqual(bytes(self.board.flash[p.offset:p.offset + len(p.data)]), p.data)
        self.assertEqual(self.board.resets, 1)
        self.assertEqual((job.result["radio_version"], job.result["next"]),
                         ("1.0.0", "Unplug its cable and put it aside"))
        self.assertEqual(job.result["chip"]["chip"], "esp32h2")
        after = read_radio(FakeSession(self.board), self.radio)
        self.assertEqual((after["runs"], after["same"]), ("ours", True))

    def test_the_copy_only_when_asked(self):
        before = bytes(self.board.flash)
        job = wait(self.runner.radio("COM9", self.radio, backup=True))
        self.assertEqual(job.state, "done", job.error)
        self.assertIn("Keep a copy of what it runs", [s.label for s in job.steps])
        self.assertEqual(self.board.reads, [(0, 4 * MB)])
        path = Path(job.result["backup"]["path"])
        self.assertEqual(path.read_bytes(), before)
        side = json.loads(path.with_suffix(".json").read_text())
        self.assertEqual((side["board"], side["module"], side["chip"], side["mac"]),
                         ("zigbee_module", "zigbee", "esp32h2", self.board.info.mac))
        self.assertTrue(path.name.startswith("zigbee_module-744dbd000001-"))

    def test_another_chip_is_refused_with_nothing_erased(self):
        self.board = s3_v02()
        job = wait(self.runner.radio("COM9", self.radio, backup=True))
        self.assertEqual(job.state, "failed")
        self.assertEqual(job.error["title"], "This isn't a Zigbee module (it is an ESP32-S3).")
        self.assertEqual(job.error["advice"], "Nothing was changed.")
        self.assertEqual((self.board.erases, self.board.writes, self.board.reads), (0, [], []))
        self.assertEqual(self.board.resets, 1)

    def test_silicon_outside_the_radios_range_is_refused(self):
        radio = testkit.synthetic_radio(min_rev=100, max_rev=199)
        job = wait(self.runner.radio("COM9", radio))
        self.assertEqual(job.state, "failed")
        self.assertIn("v0.1", job.error["title"])
        self.assertEqual((self.board.erases, self.board.writes), (0, []))

    def test_a_panel_answering_is_refused_before_connecting(self):
        self.probe = ProbeResult(port="COM9", answered=True, board="crowpanel_p4", idle=True)
        job = wait(self.runner.radio("COM9", self.radio))
        self.assertEqual(job.state, "failed")
        self.assertEqual(job.error["title"], "This is a panel, not the Zigbee module.")
        self.assertEqual((self.board.connects, self.board.erases), (0, 0))

    def test_a_write_that_reads_back_wrong_fails(self):
        self.board = h2_v01(corrupt_writes=(0x10000,))
        job = wait(self.runner.radio("COM9", self.radio))
        self.assertEqual(job.state, "failed")
        self.assertIn("app does not read back", job.error["title"])
        self.assertEqual(self.board.resets, 0)


class ZigbeeStatusTests(unittest.TestCase):
    """FLS-030: the Test card's `zigbee status` (zigbee_console.c)."""

    def test_parse(self):
        got = parse_zigbee_status(["Sensor radio ready",
                                   "ready=1 update=100% version=1.0.0 devices=3 channel=15 pan=1A2B jammed=0",
                                   "energy=-90 dBm (jammed at -60 dBm held 10 s)"])
        self.assertEqual(got, {"known": True, "enabled": True, "words": "Sensor radio ready", "ready": True,
                               "update": 100, "version": "1.0.0", "devices": 3, "channel": 15, "pan": "1a2b",
                               "jammed": False})
        starting = parse_zigbee_status(["ready=0 update=40% version= devices=0 channel=0 pan=0000 jammed=1"])
        self.assertEqual((starting["ready"], starting["version"], starting["jammed"], starting["words"]),
                         (False, "", True, ""))
        self.assertEqual(parse_zigbee_status(["Unrecognized command"]), {"known": False})
        # A real panel prints the RCP's Spinel version, spaces and all (zigbee_console.c), cut to 63.
        real = parse_zigbee_status(["Sensor radio ready",
                                    "ready=1 update=100% version=openthread-esp32/fff9895c-b678a4f; esp32h2;  "
                                    "2026-10-05 10:00:00 UTC devices=2 channel=20 pan=BEEF jammed=0"])
        self.assertEqual((real["enabled"], real["ready"], real["version"], real["devices"], real["pan"]),
                         (True, True, "openthread-esp32/fff9895c-b678a4f; esp32h2;  2026-10-05 10:00:00 UTC",
                          2, "beef"))
        self.assertEqual(parse_zigbee_status(["Zigbee is off"]),
                         {"known": True, "enabled": False, "words": "Zigbee is off"})
        self.assertIsNone(parse_zigbee_status([]))


class MockModuleTests(unittest.TestCase):
    """FLS-030 through the mock desk: the controller's read, job and status calls."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.world = MockWorld(testkit.standard_bundle(), radio="all")
        self.c = Controller(lister=self.world.lister, opener=self.world.opener, connect=self.world.connect,
                            backups_dir=self.tmp.name, captures_dir=self.tmp.name, logs_dir=self.tmp.name)
        self.c.watcher.poll()
        self.c.set_bundle(testkit.standard_bundle())
        time.sleep(2.0)

    def tearDown(self):
        self.c.stop()
        self.tmp.cleanup()

    def test_default_desk_has_the_factory_module(self):
        ports = {r.device: (r.vid, r.pid) for r in MockWorld(testkit.standard_bundle()).lister()}
        self.assertEqual(ports["COM9"], (0x303A, 0x1001))
        self.assertNotIn("COM10", ports)

    def test_radio_read_classifies_each_module(self):
        factory = self.c.radio_read("COM9", alone=True)
        self.assertEqual((factory["chip"], factory["runs"], factory["project"]), ("esp32h2", "factory", "esp_ot_cli"))
        self.assertEqual(self.c.device_dict("COM9")["role"], "zigbee_module")
        older = self.c.radio_read("COM10", alone=True)
        self.assertEqual((older["runs"], older["version"], older["older"], older["radio_version"]),
                         ("ours", "0.9.0", True, "1.0.0"))
        with self.assertRaisesRegex(JobFailed, r"it is an ESP32-S3"):
            self.c.radio_read("COM11", alone=True)
        self.assertEqual(self.c.device_dict("COM11")["role"], "unknown")
        self.assertEqual(self.world.devices["COM11"].board.erases, 0)

    def test_the_job_on_the_older_module_then_a_skipped_probe_runs(self):
        job = self.c.start_job("radio", "COM10", alone=True)
        self.c.probe("COM6")                       # asked while the job runs: it waits for the job
        self.assertIn("COM6", self.c._probe_later)
        wait(lambda: self.c.runner.current.state, 60)
        done = self.c.runner.current
        self.assertEqual(done.state, "done", done.error)
        self.assertEqual(job["kind"], "radio")
        self.assertEqual(done.result["radio_version"], "1.0.0")
        self.assertEqual(self.c.radio_read("COM10", alone=True)["same"], True)
        deadline = time.time() + 10
        while time.time() < deadline and self.c.devices["COM6"].probe is None:
            time.sleep(0.1)
        self.assertTrue(self.c.devices["COM6"].probe.answered)
        self.assertNotIn("COM10", self.c.consoles)    # the module's port is never reopened

    def test_zigbee_status_on_panels(self):
        self.assertEqual(self.c.zigbee_status("COM6"), {"known": False})      # 0.5.9: no `zigbee`
        self.world.devices["COM7"].boot()                                    # its radio is starting
        time.sleep(2.0)
        got = self.c.zigbee_status("COM7", every=0.5)
        self.assertEqual((got["known"], got["ready"], got["channel"], got["pan"]), (True, True, 15, "1a2b"))
        self.assertTrue(got["version"].startswith("openthread-esp32/") and len(got["version"]) <= 63, got)


if __name__ == "__main__":
    unittest.main()
