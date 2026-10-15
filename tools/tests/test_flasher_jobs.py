"""FLS-004, FLS-007, FLS-008, FLS-009, FLS-012: the jobs' order and gates, over a fake chip.
A busy panel is never restarted; nothing is erased before a verified backup; an update never
changes the flash layout; failures say what happened and what to do."""
import hashlib
import json
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import testkit  # noqa: E402
from roboflash.boards import load_catalogue  # noqa: E402
from roboflash.fakechip import p4_v13, esp32_v31  # noqa: E402
from roboflash.jobs import JobFailed, Runner, version_tuple, watch_boot  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402


class BootSerial:
    """A port that prints a boot log when opened."""

    def __init__(self, lines):
        self.data = bytearray("".join(l + "\r\n" for l in lines).encode())
        self.port = self.baudrate = self.timeout = None
        self.dtr = self.rts = False
        self.opened_with = None

    def open(self):
        self.opened_with = (self.dtr, self.rts)

    def read(self, n=1):
        if not self.data:
            time.sleep(0.01)
            return b""
        chunk = bytes(self.data[:n])
        del self.data[:n]
        return chunk

    def close(self):
        pass


GOOD_BOOT = ["ESP-ROM:esp32p4", "I (905) main: board: CrowPanel Advanced 7in (ESP32-P4) V1.1 (crowpanel_p4)",
             "I (2410) main: CrowPanel Advanced 7in (ESP32-P4) V1.1 running: internal RAM free 275 KB (largest block 176 KB)"]


class JobTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bundle = testkit.standard_bundle()
        self.v1 = self.bundle.image("crowpanel_p4-v1-dev")
        self.v3 = self.bundle.image("crowpanel_p4-v3-dev")
        self.board = p4_v13()
        self.probe = ProbeResult(port="COM4")
        self.boot = GOOD_BOOT
        self.events = []
        self.runner = Runner(emit=self.events.append, catalogue=load_catalogue(),
                             connect=lambda port, log, cancel, baud: self.board.connect(port, log, cancel, baud),
                             probe=lambda port: self.probe, backups_dir=self.tmp.name,
                             opener=lambda: BootSerial(self.boot), boot_timeout=2)
        self.connect_count = 0

    def tearDown(self):
        self.tmp.cleanup()

    def run_job(self, job):
        deadline = time.time() + 20
        while job.state == "running" and time.time() < deadline:
            time.sleep(0.01)
        return job

    def steps(self, job):
        return {s.key: s.state for s in job.steps}

    def test_install_backs_up_before_erasing_then_writes_checks_and_boots(self):
        before = bytes(self.board.flash)
        job = self.run_job(self.runner.install("COM4", self.v1, backup=True))
        self.assertEqual(job.state, "done", job.error)
        self.assertTrue(all(v == "done" for v in self.steps(job).values()), self.steps(job))
        backup = Path(job.result["backup"]["path"])
        self.assertEqual(backup.read_bytes(), before)
        side = json.loads(backup.with_suffix(".json").read_text())
        self.assertEqual((side["chip"], side["mac"], side["sha256"]),
                         ("esp32p4", self.board.info.mac, hashlib.sha256(before).hexdigest()))
        self.assertEqual(self.board.reads, [(0, 16 * 1024 * 1024)])
        self.assertEqual(self.board.erases, 1)
        self.assertEqual([o for o, _ in self.board.writes], [p.offset for p in self.v1.parts])
        for p in self.v1.parts:
            self.assertEqual(bytes(self.board.flash[p.offset:p.offset + len(p.data)]), p.data)
        self.assertEqual(self.board.resets, 1)
        self.assertEqual(job.result["boot"]["board"], "crowpanel_p4")

    def test_a_busy_panel_is_never_restarted(self):
        self.probe = ProbeResult(port="COM4", answered=True, idle=False, states=["armed"], version="0.6.0")
        job = self.run_job(self.runner.install("COM4", self.v1, erase_settings_ok=True))
        self.assertEqual(job.state, "failed")
        self.assertIn("armed", job.error["title"])
        self.assertEqual((self.board.connects, self.board.erases, self.board.writes), (0, 0, []))
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, probation=True)
        job = self.run_job(self.runner.update("COM4", self.v1))
        self.assertIn("probation", job.error["title"])
        self.assertEqual(self.board.connects, 0)

    def test_failed_backup_erases_nothing(self):
        self.board = p4_v13(read_error="Serial data stream stopped")
        job = self.run_job(self.runner.install("COM4", self.v1, backup=True))
        self.assertEqual(job.state, "failed")
        self.assertEqual(self.steps(job)["backup"], "failed")
        self.assertEqual((self.board.erases, self.board.writes), (0, []))

    def test_a_garbled_fast_backup_reconnects_slower(self):
        # The 10.1" panel's CH340K, 2026-10-12: the 921600 baud read came back short and a speed change
        # on that stream failed too, so every install with a backup stopped. It connects again instead.
        self.board = p4_v13(fast_read_error="Corrupt data, expected 0x1000 bytes but received 0xf85 bytes.")
        job = self.run_job(self.runner.install("COM4", self.v1, backup=True))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(self.steps(job)["backup"], "done")
        self.assertEqual((self.board.connects, self.board.erases), (2, 1))
        self.assertEqual(len(self.board.reads), 2)

    def test_unplug_while_writing_says_the_board_needs_an_install(self):
        self.board = p4_v13(unplug_after_write_bytes=40 * 1024)
        job = self.run_job(self.runner.install("COM4", self.v1, backup=False))
        self.assertEqual(job.state, "failed")
        self.assertEqual(self.steps(job)["write"], "failed")
        self.assertIn("no working firmware", job.error["advice"])

    def test_wrong_chip_is_blocked_and_wrong_silicon_needs_confirming(self):
        self.board = esp32_v31()
        job = self.run_job(self.runner.install("COM4", self.v1, backup=False))
        self.assertIn("ESP32-P4", job.error["title"])
        self.assertEqual((self.board.erases, self.board.writes), (0, []))
        self.board = p4_v13()
        job = self.run_job(self.runner.install("COM4", self.v3, backup=False))
        self.assertEqual(job.state, "failed")
        self.assertIn("black screen", job.error["title"])
        self.assertEqual(self.board.erases, 0)
        job = self.run_job(self.runner.install("COM4", self.v3, backup=False, confirmed=True))
        self.assertEqual(job.state, "done", job.error)

    def test_install_over_roboalarms_needs_the_settings_confirmed(self):
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.6.0")
        job = self.run_job(self.runner.install("COM4", self.v1))
        self.assertIn("erases its codes", job.error["title"])
        self.assertEqual(self.board.connects, 0)

    def flash_image(self, image):
        for p in image.parts:
            self.board.flash[p.offset:p.offset + len(p.data)] = p.data

    def test_update_keeps_settings_and_refuses_a_new_layout(self):
        old = testkit.synthetic_image(version="0.5.9")
        self.flash_image(old)
        self.board.flash[0xD000:0xD010] = b"SETTINGS-KEPT..."
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.5.9", board="crowpanel_p4")
        job = self.run_job(self.runner.update("COM4", self.v1))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(self.board.erases, 0)
        self.assertEqual(sorted(o for o, _ in self.board.writes), [0x2D000, 0x30000])   # same bootloader
        self.assertEqual(bytes(self.board.flash[0xD000:0xD010]), b"SETTINGS-KEPT...")

        self.board = p4_v13()
        self.flash_image(testkit.synthetic_image(version="0.5.9", pt_seed=99))
        job = self.run_job(self.runner.update("COM4", self.v1))
        self.assertEqual(job.state, "failed")
        self.assertIn("lays the flash out differently", job.error["title"])
        self.assertEqual(self.board.writes, [])

    def test_downgrade_asks_first(self):
        self.flash_image(self.v1)
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.7.0")
        job = self.run_job(self.runner.update("COM4", self.v1))
        self.assertIn("newer", job.error["title"])
        self.assertEqual(self.board.connects, 0)
        job = self.run_job(self.runner.update("COM4", self.v1, downgrade_ok=True))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(version_tuple("0.6.1-test"), (0, 6, 1))

    def test_stop_during_the_backup_changes_nothing(self):
        self.board = p4_v13(seconds_per_mb=0.05)
        job = self.runner.install("COM4", self.v1, backup=True)
        while job.step("backup").state != "running":
            time.sleep(0.005)
        self.runner.cancel(job.id)
        self.run_job(job)
        self.assertEqual(job.state, "cancelled")
        self.assertIn("Nothing was changed", job.error["advice"])
        self.assertEqual(self.board.erases, 0)

    def test_boot_that_never_says_running_is_a_warning(self):
        self.boot = ["ESP-ROM:esp32p4", "Guru Meditation Error: Core 0 panic'ed"]
        job = self.run_job(self.runner.install("COM4", self.v1, backup=False))
        self.assertEqual(job.state, "done")
        self.assertEqual(self.steps(job)["boot"], "warning")
        self.assertIn("Guru Meditation", job.result["warning"])
        self.assertNotIn("power", job.result["warning"])

    def test_a_panel_short_of_power_is_told_how_to_power_it(self):
        """FLS-026: on a computer's USB port alone the panel browns out as its screen comes on (8 to
        10 W). The warning says so and gives the board's own advice (boards.json connect.more_power)."""
        more = load_catalogue()["crowpanel_p4"]["connect"]["more_power"]
        self.boot = ["ESP-ROM:esp32p4", GOOD_BOOT[1], "Brownout detector was triggered", "ESP-ROM:esp32p4"]
        job = self.run_job(self.runner.install("COM4", self.v1, backup=False))
        self.assertEqual((job.state, self.steps(job)["boot"]), ("done", "warning"))
        self.assertIn("ran short of power", job.result["warning"])
        self.assertIn(more, job.result["warning"])
        self.boot = ["ESP-ROM:esp32p4"]    # nothing more: a black screen
        job = self.run_job(self.runner.install("COM4", self.v1, backup=False))
        self.assertIn("short of power", job.result["warning"])
        self.assertIn(more, job.result["warning"])

    def test_backup_and_restore(self):
        self.flash_image(self.v1)
        job = self.run_job(self.runner.backup("COM4", "crowpanel_p4"))
        self.assertEqual(job.state, "done", job.error)
        path = Path(job.result["backup"]["path"])
        self.board.flash[:] = b"\xff" * len(self.board.flash)
        job = self.run_job(self.runner.restore("COM4", path))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(bytes(self.board.flash), path.read_bytes())

        other = p4_v13()
        other.info.mac = "30:ed:a0:99:99:99"
        self.board = other
        job = self.run_job(self.runner.restore("COM4", path))
        self.assertIn("another board", job.error["title"])
        self.assertEqual(other.writes, [])
        self.board = esp32_v31()
        job = self.run_job(self.runner.restore("COM4", path, mac_ok=True))
        self.assertIn("this board is different", job.error["title"])

    def test_update_keeps_every_data_partition(self):  # FLS2-002, FLS-008: Update keeps settings
        old = testkit.synthetic_image(version="0.5.9", table=testkit.P4_TABLE)
        new = testkit.synthetic_image(table=testkit.P4_TABLE)
        self.flash_image(old)
        kept = {}
        for label, ptype, subtype, offset, size in testkit.P4_TABLE:
            if (ptype, subtype) in ((0, 0x10), (1, 0)):     # ota_0 and otadata: what Update writes
                continue
            for at in (offset, offset + size - 16):
                mark = f"KEEP {label}".encode().ljust(16, b".")
                self.board.flash[at:at + 16] = mark
                kept[at] = mark
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.5.9", board="crowpanel_p4")
        job = self.run_job(self.runner.update("COM4", new))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(sorted(o for o, _ in self.board.writes), [0x2D000, 0x30000])
        for at, mark in kept.items():
            self.assertEqual(bytes(self.board.flash[at:at + 16]), mark)

    def test_a_part_off_its_partition_is_refused_before_the_port(self):  # FLS2-002
        import dataclasses
        moved = [dataclasses.replace(p, offset=0xD000) if p.role == "otadata" else p for p in self.v1.parts]
        bad = dataclasses.replace(self.v1, parts=moved)
        self.flash_image(self.v1)
        before = bytes(self.board.flash)
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.5.9", board="crowpanel_p4")
        for start in (lambda: self.runner.update("COM4", bad),
                      lambda: self.runner.install("COM4", bad, backup=False, erase_settings_ok=True)):
            with self.assertRaisesRegex(JobFailed, "otadata"):
                start()
        self.assertEqual((self.board.connects, self.board.writes, self.board.resets), (0, [], 0))
        self.assertEqual(bytes(self.board.flash), before)

    def test_a_reported_board_name_never_leaves_the_backup_folder(self):  # FLS2-003, FLS-007
        root = Path(self.tmp.name)
        backups = root / "Backups"
        self.runner.backups_dir = backups
        self.board = esp32_v31()
        names = ("../outside", "..\\outside", "../../x", str(root / "elsewhere" / "x"), "/etc/x", "C:\\x",
                 "C:x", "\\\\server\\share\\x", "a\x00b", "", "..", ".hidden", "a<b>:|?*\"", "x" * 300,
                 "crowpanel_p4")
        for name in names:
            with self.subTest(name=name):
                self.probe = ProbeResult(port="COM4", answered=True, idle=True, board=name)
                job = self.run_job(self.runner.backup("COM4"))
                self.assertEqual(job.state, "done", job.error)
                path = Path(job.result["backup"]["path"])
                self.assertEqual(path.resolve().parent, backups.resolve())
                self.assertRegex(path.name, r"^[A-Za-z0-9_-]+-[0-9a-f]{12}-\d{8}-\d{6}\.bin$")
                side = json.loads(path.with_suffix(".json").read_text())
                self.assertEqual(side["board"], name)              # what the panel said, kept as data
        self.assertEqual([p.name for p in root.iterdir()], ["Backups"])
        self.assertTrue(path.name.startswith("crowpanel_p4-"))
        job = self.run_job(self.runner.restore("COM4", path))      # still restorable
        self.assertEqual(job.state, "done", job.error)

    def test_one_job_at_a_time(self):
        self.board = p4_v13(seconds_per_mb=0.02)
        job = self.runner.install("COM4", self.v1, backup=True)
        with self.assertRaises(Exception):
            self.runner.backup("COM4")
        self.runner.cancel(job.id)
        self.run_job(job)


class BootWatchTests(unittest.TestCase):
    def test_opens_without_reset_and_reads_the_board(self):
        ports = []

        def opener():
            s = BootSerial(GOOD_BOOT)
            ports.append(s)
            return s
        found = watch_boot("COM4", 2, opener)
        self.assertTrue(found["running"])
        self.assertEqual(found["board"], "crowpanel_p4")
        self.assertEqual(ports[0].opened_with, (False, False))


class QuietSerial:
    """A port that stays silent, sends part of a line, or keeps sending lines that never say
    "running"; read() waits its 0.05 s timeout like pyserial."""

    def __init__(self, mode):
        self.mode = mode
        self.port = self.baudrate = self.write_timeout = None
        self.timeout = 0.05
        self.dtr = self.rts = False
        self.closed = False

    def open(self):
        pass

    def read(self, n=1):
        time.sleep(self.timeout)
        if self.mode == "partial":
            return b"I (12) boot: no line break yet "
        if self.mode == "lines":
            return b"I (12) boot: still starting\r\n"
        return b""

    def close(self):
        self.closed = True


class BootStopTests(unittest.TestCase):
    """PERF22 (FLS-012): Stop while the restarted panel is watched starting ends the wait within one
    open retry or serial read, whatever the port does, and the job says truthfully that it stopped
    after the firmware was written. Without Stop, a boot timeout stays a warning on a done job."""

    BOOT = 30.0     # far longer than any wait the test allows

    def run_boot(self, mode, cancel_first=False, boot_timeout=None, stop=True):
        serials = []
        entered = threading.Event()

        def opener():
            entered.set()
            if mode == "missing":
                raise OSError("could not open port 'COM4': FileNotFoundError")
            s = BootSerial(GOOD_BOOT) if mode == "good" else QuietSerial(mode)
            serials.append(s)
            return s
        resets = []
        session = type("Session", (), {"reset": lambda self: resets.append(1), "close": lambda self: None})()
        with tempfile.TemporaryDirectory() as tmp:
            runner = Runner(lambda j: None, load_catalogue(), lambda *a: None, lambda p: ProbeResult(p), tmp,
                            opener=opener, boot_timeout=boot_timeout or self.BOOT)

            def work(job):
                if cancel_first:
                    job.cancel.set()
                runner._restart_and_watch(job, session, None)
            job = runner.start("install", "COM4", "boot only", [("restart", "Restart"), ("boot", "Boot")], work)
            self.assertTrue(cancel_first or entered.wait(5))
            stopped_at = time.monotonic()
            if stop and not cancel_first and mode != "good":
                time.sleep(0.2)
                stopped_at = time.monotonic()
                self.assertTrue(runner.cancel(job.id))
            deadline = time.monotonic() + 10
            while runner.busy() and time.monotonic() < deadline:
                time.sleep(0.005)
            took = time.monotonic() - stopped_at
            self.assertFalse(runner.busy())
            self.assertTrue(all(getattr(s, "closed", True) for s in serials if isinstance(s, QuietSerial)))
            # the next job can start at once
            again = runner.start("backup", "COM4", "next", [("x", "X")], lambda j: None)
            while runner.busy():
                time.sleep(0.005)
            self.assertEqual(again.state, "done")
        self.opened = entered.is_set()
        return job, took, resets

    def test_stop_ends_the_boot_wait_quickly_and_says_what_was_done(self):
        for mode in ("silent", "missing", "partial", "lines"):
            with self.subTest(mode):
                job, took, resets = self.run_boot(mode)
                self.assertLess(took, 1.0, mode)        # bound: one open retry (0.3 s) or read (0.05 s)
                self.assertEqual(job.state, "cancelled")
                self.assertEqual(resets, [1])           # it had restarted the panel already
                self.assertIn("already written", job.error["advice"])
                self.assertEqual({s.key: s.state for s in job.steps}, {"restart": "done", "boot": "skipped"})

    def test_a_stop_set_before_the_wait_begins(self):
        job, took, _ = self.run_boot("silent", cancel_first=True)
        self.assertFalse(self.opened)                 # the port is not even opened
        self.assertEqual(job.state, "cancelled")
        self.assertLess(took, 1.0)
        self.assertIn("already written", job.error["advice"])

    def test_without_stop_success_and_timeout_are_unchanged(self):
        job, _, _ = self.run_boot("good")
        self.assertEqual((job.state, job.error), ("done", None))
        self.assertTrue(job.result["boot"]["running"])
        job, _, _ = self.run_boot("missing", boot_timeout=0.8, stop=False)
        self.assertEqual((job.state, job.error), ("done", None))
        self.assertIn("did not report starting", job.result["warning"])
        job, _, _ = self.run_boot("silent", boot_timeout=0.8, stop=False)
        self.assertEqual(job.state, "done")
        self.assertEqual(job.step("boot").state, "warning")


if __name__ == "__main__":
    unittest.main()
