"""FLS-002, FLS-003, FLS-013, FLS-014, FLS-018, FLS-022: looking at a panel without restarting
it, the console, the 433 MHz page's parsing, the clock module's page, and the local server's
token and host checks."""
import json
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import clock_test, testkit  # noqa: E402
from roboflash.console import ConsoleSession  # noqa: E402
from roboflash.mock import MockWorld  # noqa: E402
from roboflash.probe import probe  # noqa: E402
from roboflash.rf_test import RfMonitor, is_status_line  # noqa: E402
from roboflash.serialio import WRITE_TIMEOUT, explain, open_quiet  # noqa: E402


class RecordingSerial:
    def __init__(self):
        self.at_open = None
        self.dtr = self.rts = True   # pyserial's defaults would assert both

    def open(self):
        self.at_open = (self.dtr, self.rts)


class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.world = MockWorld(testkit.standard_bundle())
        time.sleep(2.0)   # the mock panels finish starting

    def test_port_opens_with_reset_and_boot_lines_released(self):
        s = RecordingSerial()
        open_quiet("COM4", opener=lambda: s)
        self.assertEqual(s.at_open, (False, False))
        self.assertEqual(s.write_timeout, WRITE_TIMEOUT)   # B027: a write never hangs

    def test_a_port_that_takes_no_writes_does_not_answer(self):
        """B027: the Zigbee module's USB Serial/JTAG takes nothing while its radio software runs; a
        write there used to wait for ever ("Check it" stayed on "Looking at it..."). The probe
        now takes the stalled write as nothing answering, without an error."""
        r = probe("COM9", opener=self.world.opener, timeout=1.0)
        self.assertEqual((r.answered, r.error), (False, None))
        import serial
        stalled = serial.SerialTimeoutException("Write timeout")

        class StalledSerial(RecordingSerial):
            def write(self, data):
                raise stalled

            def close(self):
                pass
        r = probe("COM9", opener=StalledSerial, timeout=1.0)
        self.assertEqual((r.answered, r.error), (False, None))
        self.assertIn("takes what the Flasher sends", explain(stalled, "COM9").message)

    def test_factory_board_does_not_answer(self):
        r = probe("COM5", opener=self.world.opener, timeout=1.0)
        self.assertFalse(r.answered)
        self.assertEqual(r.busy_reason, "")

    def test_idle_and_armed_panels(self):
        r = probe("COM6", opener=self.world.opener)
        self.assertEqual((r.answered, r.board, r.version, r.profile, r.chip_revision, r.idle),
                         (True, "crowpanel_p4", "0.5.9", "dev", 103, True))
        self.assertEqual(r.busy_reason, "")
        armed = probe("COM7", opener=self.world.opener)
        self.assertEqual(armed.states, ["armed"])
        self.assertIn("armed", armed.busy_reason)

    def test_a_finished_network_update_is_not_busy(self):
        """Seen on a panel: `operation` keeps the last network update's ID after it
        ends; only idle, probation and confirm_required say the panel is busy (as panelctl)."""
        from roboflash.probe import ProbeResult, parse_describe
        r = ProbeResult(port="COM4")
        parse_describe(r, '{"board":"crowpanel_p4","idle":true,"probation":false,'
                          '"confirm_required":false,"operation":"d04567c828d95da76a146c33d9b875be"}')
        self.assertEqual(r.busy_reason, "")
        parse_describe(r, '{"board":"crowpanel_p4","idle":true,"probation":false,"confirm_required":true}')
        self.assertIn("probation", r.busy_reason)

    def test_port_errors_in_words(self):
        r = probe("COM99", opener=self.world.opener)
        self.assertEqual(r.error["kind"], "missing")
        self.assertEqual(explain(OSError("could not open port 'COM4': PermissionError(13, 'Access is denied.')"),
                                 "COM4").kind, "busy")


class ConsoleTests(unittest.TestCase):
    def test_commands_and_quiet_polls(self):
        world = MockWorld(testkit.standard_bundle())
        time.sleep(2.0)
        got = []
        c = ConsoleSession("COM6", got.extend, opener=world.opener)
        c.open()
        try:
            c.send("status")
            c.send("rf", quiet=True, quiet_match=is_status_line)
            time.sleep(0.8)
        finally:
            c.close()
        shown = [l.text for l in got if not l.quiet and l.source == "panel"]
        hidden = [l.text for l in got if l.quiet]
        self.assertTrue(any(t.startswith('Partition 1 "House": disarmed') for t in shown), shown)
        self.assertNotIn("status", shown)     # the echo is not shown twice
        self.assertTrue(any(t.startswith("Receiver module on GPIO") for t in hidden), hidden)
        self.assertFalse(any(t.startswith("Receiver module") for t in shown))

    def test_a_stream_without_line_breaks_stays_bounded(self):  # FLS2-006 (FLS-013)
        """2 MiB without a newline (a binary stream, a wrong baud rate): the console keeps at most
        one line's worth pending, shows the bytes as cut lines with one warning, and recovers on
        the next real line."""
        import roboflash.console as console_mod
        chunks = [b"\x00\xfeA" * 1365 + b"A"] * 512 + [b"I (1) main: back to normal\r\nalarm> st", b"atus\r\n"]
        session = {}
        biggest = [0]

        class Stream:
            port = baudrate = timeout = write_timeout = None
            dtr = rts = False

            def open(self):
                pass

            def read(self, n=1):
                c = session.get("c")
                if c is not None:
                    biggest[0] = max(biggest[0], len(c._pending))
                if chunks:
                    return chunks.pop(0)
                time.sleep(0.01)
                return b""

            def close(self):
                pass

        got = []
        c = ConsoleSession("COM6", got.extend, opener=Stream)
        session["c"] = c
        c.open()
        deadline = time.time() + 20
        while chunks and time.time() < deadline:
            time.sleep(0.01)
        time.sleep(0.1)
        started = time.time()
        c.close()
        self.assertLess(time.time() - started, 1.5)
        self.assertEqual(chunks, [])
        self.assertLessEqual(biggest[0], console_mod.MAX_LINE)
        panel = [l for l in got if l.source == "panel"]
        warnings = [l for l in got if l.source == "tool" and "without a line break" in l.text]
        self.assertEqual(len(warnings), 1)
        self.assertTrue(all(len(l.text.encode("utf-8")) <= 3 * console_mod.MAX_LINE for l in panel))
        self.assertEqual(panel[-2].text, "I (1) main: back to normal")
        self.assertEqual(panel[-2].level, "I")
        self.assertEqual(panel[-1].text, "status")             # the prompt still comes off
        self.assertLessEqual(len(c.history), c.history.maxlen)


class RfPinTests(unittest.TestCase):
    """RF-008 from the Flasher's side: `rf pin`, then a restart; refusals come back in words."""

    def test_move_the_receiver_to_io3(self):
        from roboflash.controller import Controller
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                r = c.rf_set_pin("COM6", 3)
                self.assertTrue(r["reply"].startswith("data on GPIO 3"))
                time.sleep(2.2)                       # the panel restarted (its reset line)
                c.rf_start("COM6")
                time.sleep(1.0)
                self.assertEqual(c.rf["COM6"].status.get("gpio"), 3)
                with self.assertRaisesRegex(JobFailed, "Not GPIO 25: this board offers GPIO 10"):
                    c.rf_set_pin("COM6", 25)
                c.rf_set_pin("COM6", None)
                self.assertIsNone(world.devices["COM6"].rf_pin_stored)
            finally:
                c.stop()


class RefusedJobTests(unittest.TestCase):
    """A job request the Flasher refuses leaves the port as it was. The console (and the 433 MHz
    polling) was once let go before the request was checked, so a refusal - no firmware, a
    wiring mistake, a backup from elsewhere - left it closed, saying it would reopen after a job
    that never ran."""

    def test_a_refused_job_keeps_the_console(self):
        from roboflash.controller import Controller
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                c.rf_start("COM6")       # the console opens and the 433 MHz page polls it
                refusals = [
                    ("No firmware bundle", dict(kind="install")),
                    ("Zigbee module is in it", dict(kind="install", image_key="crowpanel_p4-v1-dev",
                                                    parts={"rf": "module", "rf_place": "socket", "zigbee": True})),
                    ("Backups folder", dict(kind="restore", path=str(Path(tmp).parent / "x.bin"))),
                ]
                for words, request in refusals:
                    if "image_key" in request:
                        c.set_bundle(testkit.standard_bundle())
                    kind = request.pop("kind")
                    with self.subTest(words), self.assertRaisesRegex(JobFailed, words):
                        c.start_job(kind, "COM6", **request)
                    self.assertTrue(c.consoles["COM6"].is_open, words)
                    self.assertNotIn("COM6", c._reopen_after_job)
                    self.assertIn("COM6", c.rf_polling)
                    self.assertNotIn("COM6", c._parts_after)
            finally:
                c.stop()


class RfModuleTests(unittest.TestCase):
    """RF-001 from the Flasher's side: which receiver the panel listens with, then a restart."""

    def test_choose_the_cc1101(self):
        from roboflash.controller import Controller
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                r = c.rf_set_module("COM6", "cc1101")
                self.assertTrue(r["reply"].startswith("receiver cc1101"))
                time.sleep(2.2)                       # the panel restarted
                c.rf_start("COM6")
                time.sleep(1.0)
                self.assertEqual((c.rf["COM6"].status.get("kind"), c.rf["COM6"].status.get("module")),
                                 ("cc1101", "cc1101"))
                with self.assertRaisesRegex(JobFailed, "Unknown receiver"):
                    c.rf_set_module("COM6", "sx1276")
            finally:
                c.stop()


class ResetGateTests(unittest.TestCase):
    """FLS-004, FLS2-001: every path that restarts a panel - the Console's Restart, Connected
    parts (before its first setting and again before its restart), the receiver's pin and module,
    and the command line's identify - looks at the panel just before, with the jobs' idle gate. A
    panel armed, in installer mode, on probation or waiting for an update's confirmation since it
    was last looked at, or gone quiet although it ran RoboAlarms, is refused: no loader
    connection, no reset pulse, no setting stored."""

    PROBE_COMMANDS = {"", "dev describe", "status", "ota"}

    def setUp(self):
        from roboflash.controller import Controller
        self.tmp = tempfile.TemporaryDirectory()
        self.world = MockWorld(testkit.standard_bundle())
        self.connects = []

        def connect(port, log, cancel, fast_baud):
            self.connects.append(port)
            return self.world.connect(port, log, cancel, fast_baud)
        self.connect = connect
        self.c = Controller(lister=self.world.lister, opener=self.world.opener, connect=connect,
                            backups_dir=self.tmp.name, captures_dir=self.tmp.name, logs_dir=self.tmp.name)
        self.boots = {}
        self.commands = {}
        self.state = {}
        for port in ("COM5", "COM6"):
            self.instrument(self.world.devices[port])
        time.sleep(2.0)

    def tearDown(self):
        self.c.stop()
        self.tmp.cleanup()

    def instrument(self, dev):
        """Count the restarts (EN released) and the commands; answer `dev describe` in the state
        the test sets, or not at all ("silent")."""
        port = dev.port
        self.boots[port], self.commands[port], self.state[port] = 0, [], "idle"
        real_boot, real_command = dev.boot, dev.command

        def boot():
            self.boots[port] += 1
            real_boot()

        def command(cmd):
            self.commands[port].append(cmd)
            state = self.state[port]
            if state == "silent":
                return
            if cmd == "dev describe" and state in ("installer", "probation", "confirm"):
                if time.monotonic() - dev.booted_at < 1.9 or dev.installed() is None:
                    return
                inst = dev.installed()
                info = {"board": inst["board"], "version": inst["version"], "profile": inst["profile"],
                        "idle": state != "installer", "probation": state == "probation",
                        "confirm_required": state == "confirm"}
                dev.emit(cmd + "\r\n" + "DEV INFO " + json.dumps(info) + "\r\n")
                return
            real_command(cmd)
        dev.boot, dev.command = boot, command

    def discover_idle(self, port="COM6"):
        self.c.probe(port)
        deadline = time.time() + 10
        d = None
        while time.time() < deadline:
            d = self.c.devices.get(port)
            if d is not None and d.probe is not None and not d.busy:
                break
            time.sleep(0.05)
        return d

    def wait_parts(self, port):
        deadline = time.time() + 30
        while self.c.parts_jobs[port]["state"] == "running" and time.time() < deadline:
            time.sleep(0.05)
        return self.c.parts_jobs[port]

    def entry_points(self):
        """(name, fn) for each path that restarts a panel; fn() returns the refusal's title."""
        from roboflash.app import run_identify
        from roboflash.jobs import JobFailed
        from types import SimpleNamespace

        def raising(fn):
            def run():
                with self.assertRaises(JobFailed) as e:
                    fn()
                return e.exception.title
            return run

        def parts():
            self.c.parts_apply("COM6", {"rf": "cc1101", "rf_place": "j7"})
            st = self.wait_parts("COM6")
            self.assertEqual(st["state"], "failed")
            return st["error"]["title"]

        def cli():
            import contextlib
            import io
            err = io.StringIO()
            self.c.console_close("COM6")    # the command line is another program (FLS-011)
            with contextlib.redirect_stderr(err):
                code = run_identify(SimpleNamespace(port="COM6", verbose=False),
                                    probe=lambda port: probe(port, opener=self.world.opener),
                                    connect=lambda port, log=None: self.connect(port, log, None, 0))
            self.assertEqual(code, 1)
            return err.getvalue()
        return [("console restart", raising(lambda: self.c.console_restart("COM6"))),
                ("parts", parts),
                ("rf pin", raising(lambda: self.c.rf_set_pin("COM6", 3))),
                ("rf module", raising(lambda: self.c.rf_set_module("COM6", "cc1101"))),
                ("command-line identify", cli)]

    def test_every_restart_path_refuses_a_panel_that_is_no_longer_idle(self):
        d = self.discover_idle()
        self.assertTrue(d.probe.answered and d.probe.idle, d.probe)
        seen_idle = d.probe
        self.c.console_open("COM6")
        dev = self.world.devices["COM6"]
        expected = {"armed": "armed", "installer": "installer mode", "probation": "probation",
                    "confirm": "probation", "silent": "did not answer"}
        for state, words in expected.items():
            self.state["COM6"] = state
            dev.armed = state == "armed"
            for name, refused in self.entry_points():
                if state == "silent" and name == "command-line identify":
                    continue    # with nothing to remember, a quiet panel looks like a factory board
                with self.subTest(state=state, path=name):
                    d.probe = seen_idle             # what the Flasher saw earlier: idle, now stale
                    boots = self.boots["COM6"]
                    self.commands["COM6"].clear()
                    self.assertIn(words, refused())
                    self.assertEqual(self.boots["COM6"], boots)     # no reset pulse
                    self.assertEqual(self.connects, [])             # no loader connection
                    self.assertLessEqual(set(self.commands["COM6"]), self.PROBE_COMMANDS)   # nothing stored
        self.assertEqual((dev.rf_pin_stored, dev.rf_module_stored, dev.rf_place_stored), (None, "auto", "default"))

    def test_parts_check_again_before_their_restart(self):
        self.discover_idle()
        dev = self.world.devices["COM6"]
        real_command = dev.command

        def arm_while_sending(cmd):
            real_command(cmd)
            if cmd.startswith("rf place"):
                dev.armed = True        # armed at the panel while the settings go
        dev.command = arm_while_sending
        boots = self.boots["COM6"]
        self.c.parts_apply("COM6", {"rf": "cc1101", "rf_place": "j7"})
        st = self.wait_parts("COM6")
        self.assertEqual(st["state"], "failed")
        self.assertIn("armed", st["error"]["title"])
        self.assertIn("not restarted", st["error"]["advice"])
        self.assertEqual([s["state"] for s in st["steps"]], ["done", "failed", "waiting"])
        self.assertEqual(self.boots["COM6"], boots)

    def test_an_idle_panel_and_a_board_without_a_console_still_restart(self):
        self.discover_idle()
        boots = self.boots["COM6"]
        self.c.console_restart("COM6")              # freshly idle: restarted as asked
        self.assertEqual(self.boots["COM6"], boots + 1)
        # A factory demo, like a release image without a console (BL-211), never answered: its
        # Console's Restart works as before.
        self.discover_idle("COM5")
        self.assertFalse(self.c.devices["COM5"].probe.answered)
        boots = self.boots["COM5"]
        self.c.console_restart("COM5")
        self.assertEqual(self.boots["COM5"], boots + 1)
        # The command line's identify reads a board that is idle or has no console.
        from roboflash.app import run_identify
        from types import SimpleNamespace
        import contextlib
        import io
        self.c.console_close("COM6")    # one program per port (FLS-011): the command line is another
        self.c.console_close("COM5")
        for port in ("COM6", "COM5"):
            time.sleep(2.0)      # COM6 restarted above: its console answers again
            with contextlib.redirect_stdout(io.StringIO()):
                code = run_identify(SimpleNamespace(port=port, verbose=False),
                                    probe=lambda p: probe(p, opener=self.world.opener),
                                    connect=lambda p, log=None: self.connect(p, log, None, 0))
            self.assertEqual(code, 0, port)
        self.assertEqual(self.connects, ["COM6", "COM5"])

    def test_an_old_look_is_refused(self):
        """Only the gate's max_age, as a unit: the controller always looks again just before a
        restart (_check_before_restart), so no entry point hands it an old look."""
        from roboflash.jobs import JobFailed, PROBE_MAX_AGE, refuse_unless_idle
        from roboflash.probe import ProbeResult
        old = ProbeResult(port="COM6", answered=True, idle=True, at=time.time() - PROBE_MAX_AGE - 1)
        with self.assertRaisesRegex(JobFailed, "out of date"):
            refuse_unless_idle(old, max_age=PROBE_MAX_AGE)
        refuse_unless_idle(old)     # the jobs take the look they just made

    def restart_refused(self, port, words):
        from roboflash.jobs import JobFailed
        boots = self.boots[port]
        with self.assertRaises(JobFailed) as e:
            self.c.console_restart(port)
        self.assertIn(words, e.exception.title)
        self.assertEqual(self.boots[port], boots)       # no reset pulse

    def test_a_console_that_answers_without_its_idle_state_is_refused(self):
        # FLS-004, FLS2-001: `ota` answered but `status` listed no partition; `dev describe` without
        # "idle". Both are RoboAlarms, and neither says the panel is idle.
        for replies in ({"dev describe": ["Unrecognized command"], "status": ["Network: offline"],
                         "ota": ["Firmware: 0.3.0"]},
                        {"dev describe": ['DEV INFO {"board":"crowpanel_p4","version":"0.4.0"}'],
                         "status": ["Network: offline"]}):
            with self.subTest(replies=replies["dev describe"][0]):
                self.c._probe_for_job = lambda port, r=replies: probe(port, opener=lambda: ScriptedSerial(r))
                self.restart_refused("COM6", "did not say whether it is idle")

    def test_a_written_board_stays_known_to_run_roboalarms(self):
        # FLS2-001: the factory demo on COM5 never answered, so its Console restarts it - until the
        # Flasher has written RoboAlarms on it, finished or Stopped while it started. A restore (what
        # it wrote is unknown) changes nothing.
        from roboflash.controller import Device
        d = self.c.devices.setdefault("COM5", Device("COM5"))

        def job(kind, state):
            return {"id": "j1", "kind": kind, "port": "COM5", "title": "", "image_key": "", "state": state,
                    "error": None if state == "done" else {"title": "Stopped.", "advice": "It was written."},
                    "result": {}, "steps": [{"key": "write", "state": "done"}], "log": []}
        self.c._job_changed(job("restore", "done"))
        self.assertFalse(d.roboalarms)
        boots = self.boots["COM5"]
        self.c.console_restart("COM5")
        self.assertEqual(self.boots["COM5"], boots + 1)
        for kind, state in (("install", "cancelled"), ("update", "done")):
            with self.subTest(kind=kind, state=state):
                d.roboalarms = False
                self.c._job_changed(job(kind, state))
                self.assertTrue(d.roboalarms)
                self.restart_refused("COM5", "did not answer")

    def test_a_prompt_on_the_console_makes_the_panel_known(self):
        # FLS2-001: the probe never had an answer (a busy panel, or a release image), but its console
        # printed the RoboAlarms prompt: from then on a quiet panel is not restarted blind.
        from roboflash.controller import Device
        d = self.c.devices.setdefault("COM6", Device("COM6"))
        self.assertFalse(d.roboalarms)
        self.c.console_open("COM6")
        self.state["COM6"] = "silent"
        self.world.devices["COM6"].emit("alarm> ")      # the prompt, without a line break
        deadline = time.time() + 2
        while not d.roboalarms and time.time() < deadline:
            time.sleep(0.02)
        self.assertTrue(d.roboalarms)
        self.restart_refused("COM6", "did not answer")

    def test_the_web_identify_refuses_a_known_panel_gone_quiet(self):
        # FLS-004, FLS2-001: the page's identify uses the same gate: no loader connection.
        d = self.discover_idle()
        self.state["COM6"] = "silent"
        self.c.identify("COM6")
        deadline = time.time() + 10
        while (d.busy or d.error is None) and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn("did not answer", d.error["message"])
        self.assertEqual(self.connects, [])

    def test_receiver_settings_check_again_before_their_restart(self):
        # FLS-004, FLS2-001: armed at the panel while `rf pin` / `rf module` was stored: no restart.
        from roboflash.jobs import JobFailed
        self.discover_idle()
        dev = self.world.devices["COM6"]
        instrumented = dev.command
        for word, change, stored, value in (
                ("rf pin", lambda: self.c.rf_set_pin("COM6", 3), lambda: dev.rf_pin_stored, 3),
                ("rf module", lambda: self.c.rf_set_module("COM6", "cc1101"), lambda: dev.rf_module_stored, "cc1101")):
            with self.subTest(command=word):
                dev.armed = False
                time.sleep(2.0)     # the console answers again after the last restart

                def arm_while_storing(cmd, word=word):
                    instrumented(cmd)
                    if cmd.startswith(word):
                        dev.armed = True
                dev.command = arm_while_storing
                boots = self.boots["COM6"]
                with self.assertRaises(JobFailed) as e:
                    change()
                self.assertIn("armed", e.exception.title)
                self.assertIn("not restarted", e.exception.advice)
                self.assertEqual(self.boots["COM6"], boots)
                self.assertEqual(stored(), value)       # stored, for the next start
                dev.command = instrumented

    def test_the_board_stays_busy_until_the_reset_pulse(self):
        # FLS2-001: no other look or job takes the port between the check and the pulse.
        self.discover_idle()
        dev = self.world.devices["COM6"]
        seen = []
        counted = dev.boot

        def boot():
            seen.append(self.c.devices["COM6"].busy)
            counted()
        dev.boot = boot
        self.c.console_restart("COM6")
        self.assertEqual(seen, ["probing"])
        self.assertEqual(self.c.devices["COM6"].busy, "")

    def test_a_job_blocks_restarts_on_its_own_port_only(self):
        # FLS2-001: another port is another board; the job's own port is never reset under it.
        from types import SimpleNamespace
        from roboflash.jobs import JobFailed
        self.discover_idle()
        self.c.runner.current = SimpleNamespace(id="j1", port="COM5", state="running")
        try:
            with self.assertRaisesRegex(JobFailed, "job is using this port"):
                self.c.console_restart("COM5")
            boots = self.boots["COM6"]
            self.c.console_restart("COM6")
            self.assertEqual(self.boots["COM6"], boots + 1)
        finally:
            self.c.runner.current = None

    def test_a_stopped_install_says_its_parts_were_not_set_up(self):
        # FLS-024, PERF22: a Stop while the panel started drops the parts queued for it, in words.
        self.c._parts_after["COM5"] = {"rf": "cc1101"}
        job = {"id": "j1", "kind": "install", "port": "COM5", "title": "", "image_key": "", "state": "cancelled",
               "error": {"title": "Stopped.", "advice": "The firmware was already written."}, "result": {},
               "steps": [{"key": "write", "state": "done"}], "log": []}
        self.c._job_changed(job)
        self.assertIn("were not set up", job["error"]["advice"])
        self.assertIn("Connected parts", job["error"]["advice"])
        self.assertNotIn("COM5", self.c._parts_after)


class ScriptedSerial:
    """A console that answers each command with the given lines (FLS2-001's unknown idle state)."""

    def __init__(self, replies):
        self.replies = replies
        self.out = bytearray()
        self.dtr = self.rts = False

    def open(self):
        pass

    def close(self):
        pass

    def write(self, data):
        for line in self.replies.get(data.decode().strip(), ()):
            self.out += (line + "\r\n").encode()
        return len(data)

    def read(self, n=1):
        if not self.out:
            time.sleep(0.01)
            return b""
        chunk = bytes(self.out[:n])
        del self.out[:n]
        return chunk

    def reset_input_buffer(self):
        self.out.clear()

    @property
    def in_waiting(self):
        return len(self.out)


class UnknownIdleTests(unittest.TestCase):
    """FLS-004, FLS2-001: a RoboAlarms console that answers without saying whether the panel is
    idle is refused by the gate every restart shares (the jobs' check too); an answer without
    "idle" takes it from the partition states when `status` lists them."""

    def look(self, replies):
        return probe("COM6", opener=lambda: ScriptedSerial(replies), timeout=1.0)

    def test_ota_without_partitions_and_describe_without_idle_are_refused(self):
        import tempfile as tf
        from roboflash.jobs import Job, JobFailed, Runner, Step, refuse_unless_idle
        for replies in ({"dev describe": ["Unrecognized command"], "status": ["Network: offline"],
                         "ota": ["Firmware: 0.3.0"]},
                        {"dev describe": ['DEV INFO {"board":"crowpanel_p4","version":"0.4.0"}'],
                         "status": ["Network: offline"]}):
            with self.subTest(replies=replies["dev describe"][0]):
                pr = self.look(replies)
                self.assertEqual((pr.answered, pr.idle, pr.busy_reason), (True, None, ""))
                with self.assertRaisesRegex(JobFailed, "did not say whether it is idle"):
                    refuse_unless_idle(pr)
                with tf.TemporaryDirectory() as tmp:
                    runner = Runner(lambda job: None, {}, None, lambda port: pr, tmp)
                    job = Job(kind="update", port="COM6", title="", steps=[Step("check", "Check the panel")])
                    with self.assertRaisesRegex(JobFailed, "did not say whether it is idle"):
                        runner._check_idle(job)     # install and update: nothing connected or written

    def test_describe_without_idle_takes_the_partition_states(self):
        from roboflash.jobs import refuse_unless_idle
        describe = ['DEV INFO {"board":"crowpanel_p4","version":"0.4.0"}']
        idle = self.look({"dev describe": describe,
                          "status": ['Partition 1 "House": disarmed', "Network: offline"]})
        self.assertEqual((idle.answered, idle.idle), (True, True))
        refuse_unless_idle(idle)
        armed = self.look({"dev describe": describe,
                           "status": ['Partition 1 "House": armed_away', "Network: offline"]})
        self.assertEqual(armed.idle, False)
        self.assertIn("armed away", armed.busy_reason)


class ConsoleSignTests(unittest.TestCase):
    """FLS2-001: a RoboAlarms console's prompt or DEV INFO answer marks it, once; other output doesn't."""

    def test_signs(self):
        for data, sign in ((b"vendor demo v1.0 starting\r\nI (25) main_task: Calling app_main()\r\n", False),
                           (b'DEV INFO {"board":"crowpanel_p4"}\r\n', True),
                           (b"W (1780) wifi: not configured\r\nalarm> ", True)):
            with self.subTest(data=data):
                calls = []
                s = ScriptedSerial({})
                s.out += data
                c = ConsoleSession("COM6", lambda lines: None, opener=lambda: s,
                                   on_roboalarms=lambda: calls.append(1))
                c.open()
                time.sleep(0.3)
                c.close()
                self.assertEqual((c.roboalarms, len(calls)), (sign, int(sign)))


class UnplugTests(unittest.TestCase):
    """PERF23 (FLS-001): unplugging boards whose consoles are busy publishing 433 MHz lines lists
    the ports again well within 2 s; the consoles' readers are never left waiting for the lock of
    the code that closes them, and a late line from a removed port publishes nothing."""

    def run_unplug(self, count):
        import queue
        import threading
        from types import SimpleNamespace
        from roboflash.controller import Controller
        from roboflash.ports import classify
        names = [f"COM{20 + i}" for i in range(count)]
        lines = {n: queue.Queue() for n in names}

        class InputSerial:
            def __init__(self):
                self.port = self.baudrate = self.timeout = self.write_timeout = None
                self.dtr = self.rts = False
                self.closed = False

            def open(self):
                pass

            def close(self):
                self.closed = True

            def read(self, n=1):
                try:
                    return lines[self.port].get(timeout=0.005)
                except queue.Empty:
                    return b""
        serials = []

        def opener():
            serials.append(InputSerial())
            return serials[-1]
        ports = [classify(SimpleNamespace(device=n, vid=0x1A86, pid=0x7522, description="USB-SERIAL CH340"))
                 for n in names]
        with tempfile.TemporaryDirectory() as tmp:
            c = Controller(lister=lambda: [], opener=opener, backups_dir=tmp, captures_dir=tmp, logs_dir=tmp,
                           driver_probe=None)
            c._ports_changed(ports)
            for n in names:
                c.console_open(n)
            consoles = [c.consoles[n] for n in names]
            events = c.events.subscribe()
            # The schedule the review arranged: each reader is inside its 433 MHz callback, about to
            # take the controller's lock, when the port list changes under that lock.
            reached = {n: threading.Event() for n in names}
            go = threading.Event()
            real = c._publish_rf_soon

            def held_back(port, session=None):
                reached[port].set()
                go.wait(5)
                return real(port) if session is None else real(port, session)
            c._publish_rf_soon = held_back
            for n in names:
                lines[n].put(b"I (1000) rf: EV1527 ABCDEF code 1 (ABCDEF1, T 350 us)\n")
            self.assertTrue(all(e.wait(2) for e in reached.values()))

            class Releasing(dict):
                def pop(self, *a):
                    go.set()            # inside the locked section: the readers now wait for the lock
                    time.sleep(0.05)
                    return super().pop(*a)
            c.devices = Releasing(c.devices)
            started = time.monotonic()
            c._ports_changed([])
            took = time.monotonic() - started
            alive = [x for x in consoles if x._thread.is_alive()]
            snapshot_started = time.monotonic()
            c.snapshot()
            snapshot_took = time.monotonic() - snapshot_started
            time.sleep(0.4)     # longer than an rf flush timer
            rf_events = []
            while not events.empty():
                e = events.get_nowait()
                if e["type"] == "rf":
                    rf_events.append(e)
            state = (dict(c.consoles), set(c._rf_pending), all(s.closed for s in serials))
            # A rapid reconnect of the same port opens a new console.
            c.devices = dict(c.devices)
            c._ports_changed(ports[:1])
            c.console_open(names[0])
            reopened = c.consoles[names[0]].is_open
            c.stop()
        return took, alive, snapshot_took, rf_events, state, reopened

    def test_unplugging_busy_consoles_is_quick(self):
        for count in (1, 3):
            with self.subTest(consoles=count):
                took, alive, snapshot_took, rf_events, (consoles, pending, closed), reopened = self.run_unplug(count)
                self.assertLess(took, 0.9)      # FLS-001: 2 s from the plug; was 1 s per console
                self.assertEqual(alive, [])
                self.assertLess(snapshot_took, 0.5)
                self.assertEqual(rf_events, [])     # nothing published for removed ports
                self.assertEqual((consoles, pending, closed), ({}, set(), True))
                self.assertTrue(reopened)

    def test_an_ordinary_unplug(self):
        from types import SimpleNamespace
        from roboflash.controller import Controller
        from roboflash.ports import classify
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, backups_dir=tmp, captures_dir=tmp,
                           logs_dir=tmp, driver_probe=None)
            ports = [classify(SimpleNamespace(device="COM6", vid=0x1A86, pid=0x7522, description="CH340"))]
            c._ports_changed(ports)
            time.sleep(2.0)
            c.console_open("COM6")
            reader = c.consoles["COM6"]._thread
            started = time.monotonic()
            c._ports_changed([])
            self.assertLess(time.monotonic() - started, 0.5)
            self.assertFalse(reader.is_alive())
            self.assertNotIn("COM6", c.consoles)
            c.stop()


class ClockTests(unittest.TestCase):
    """FLS-022 (docs/features/35 RTC-010): what `rtc` says, and setting the clock from this computer."""

    FIRMWARE = [
        "Clock module: keeping time, 24.25 C",
        "rtc module=ok source=internet trusted=1 lost_at_start=0 temp_c4=97 offset_ms=-12 writes=3 "
        "write_failures=0 slews=1 jumps=0 epoch=1790260205",
        "panel clock 2026-09-24 14:30:05 UTC (time server)",
    ]

    def test_the_status_lines_of_the_firmware(self):
        # The same lines components/services/panel_clock/src/panel_clock.c prints.
        self.assertTrue(all(clock_test.is_status_line(t) for t in self.FIRMWARE))
        self.assertTrue(clock_test.is_reply_end(self.FIRMWARE[1]))
        st = clock_test.parse_status(self.FIRMWARE, 1790260200.4)
        self.assertTrue(st["available"])
        self.assertEqual((st["module"], st["source"], st["temp_c"], st["offset_ms"]), ("ok", "internet", 24.25, -12))
        self.assertEqual(st["difference_s"], 5)
        self.assertTrue(st["settable"] and st["internet_trusted"] and st["clock_set"])
        self.assertEqual(st["module_words"], "Keeping time")

    def test_no_module_and_no_support(self):
        st = clock_test.parse_status([
            "Clock module: not fitted (none answered on the I2C connector (PH2.0, 4 pins) at start-up)",
            "rtc module=absent source=none trusted=0 lost_at_start=0 temp_c4=0 offset_ms=? writes=0 "
            "write_failures=0 slews=0 jumps=0 epoch=0"], 1790260200)
        self.assertTrue(st["available"])
        self.assertFalse(st["settable"] or st["clock_set"])
        self.assertIsNone(st["difference_s"])
        self.assertIsNone(st["offset_ms"])
        self.assertIsNone(st["temp_c"])
        for line in ("Clock module: not in this firmware", "Unrecognized command"):
            self.assertTrue(clock_test.is_reply_end(line))
            st = clock_test.parse_status([line], 0)
            self.assertFalse(st["available"])
            self.assertIn("no clock-module support", st["reason"])

    def test_set_replies(self):
        self.assertEqual(clock_test.set_reply("clock set to 2026-09-24 14:30:05 UTC; the module follows at the next second"),
                         (True, "Set to 2026-09-24 14:30:05 UTC"))
        ok, words = clock_test.set_reply("refused: no clock module answers, so the time would be lost at the next power cut")
        self.assertFalse(ok)
        self.assertTrue(words.startswith("no clock module answers"))
        self.assertEqual(clock_test.set_reply("Unrecognized command")[0], False)
        self.assertIsNone(clock_test.set_reply("I (123) clock: module set to 2026-09-24 14:30:05 UTC"))

    def test_set_the_clock_from_this_computer(self):
        from roboflash.controller import Controller
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            world = MockWorld(testkit.standard_bundle())
            c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            time.sleep(2.0)
            try:
                st = c.clock_status("COM6")["clock"]
                self.assertEqual(st["module"], "ok")
                self.assertLessEqual(abs(st["difference_s"] + 37), 1)   # the pretend panel is 37 s behind
                r = c.clock_set("COM6")
                self.assertTrue(r["reply"].startswith("Set to "))
                st = c.clock_status("COM6")["clock"]
                self.assertLessEqual(abs(st["difference_s"]), 1)
                self.assertEqual(st["source"], "manual")
                # A panel without a module refuses, in words.
                with self.assertRaisesRegex(JobFailed, "No clock module answers"):
                    c.clock_set("COM7")
            finally:
                c.stop()


class RfTests(unittest.TestCase):
    def test_status_block(self):
        m = RfMonitor()
        for line in ["Receiver module on GPIO 3, 1840 edges/s, line 42% high",
                     "module=ook receiver=yes edges/s=1840 high=42% storms=0",
                     "capture receptions=412 symbols=98231 overruns=0",
                     "decoder pulses=98231 frames=36 rejected=4 glitches=112",
                     "source transmissions=9 unknown=2 bursts=9",
                     "last EV1527 A2B3C code 5 (A2B3C5, T 350 us) at 12345 ms"]:
            self.assertTrue(is_status_line(line), line)
            self.assertTrue(m.feed(line))
        s = m.status
        self.assertEqual((s["kind"], s["gpio"], s["edges_per_s"], s["frames"], s["transmissions"]),
                         ("module", 3, 1840, 36, 9))
        self.assertEqual(s["last"]["code"], "A2B3C5")
        self.assertEqual(m.heard, {})   # the status block's "last" line is not a new transmission
        m.feed("No receiver module drives GPIO 3 yet")
        self.assertEqual((m.status["kind"], m.status["gpio"]), ("missing", 3))
        m.feed("433 MHz receiver: not in this firmware")
        self.assertEqual(m.status["kind"], "absent")

    def test_heard_sensors_are_grouped(self):
        m = RfMonitor(clock=iter(range(100, 200)).__next__)
        m.feed("I (1234) rf: EV1527 A2B3C code 5 (A2B3C5, T 350 us)")
        m.feed("I (2234) rf: EV1527 A2B3C code E (A2B3CE, T 352 us)")
        m.feed("RF 00123 code 3 (001233, T 400 us, PT2262 shape)")
        self.assertFalse(m.feed("I (3000) wifi: something else"))
        heard = {h["device"]: h for h in m.to_dict()["heard"]}
        self.assertEqual(heard["A2B3C"]["codes"], {"5": 1, "E": 1})
        self.assertEqual(heard["A2B3C"]["count"], 2)
        self.assertTrue(heard["00123"]["pt2262"])

    def test_every_protocol_and_its_flags(self):
        """RF-012/RF-018: the firmware names each protocol and adds what else it heard."""
        m = RfMonitor(clock=iter(range(100, 200)).__next__)
        self.assertTrue(m.feed("I (10) rf: HT6P20B 2A3B4C code 1 (A8ED35, T 452 us)"))
        self.assertTrue(m.feed("I (20) rf: KeeLoq 0ABCDEF code 2 (ABCDEF, T 401 us, battery low)"))
        self.assertTrue(m.feed("I (30) rf: EV1527 55555 code 1 (555551, T 350 us, PT2262 shape, common factory address)"))
        heard = {h["device"]: h for h in m.to_dict()["heard"]}
        self.assertEqual(heard["2A3B4C"]["protocol"], "HT6P20B")
        self.assertTrue(heard["0ABCDEF"]["battery_low"])
        self.assertTrue(heard["55555"]["pt2262"] and heard["55555"]["common"])
        self.assertFalse(heard["2A3B4C"]["common"])

    def test_the_status_block_of_a_real_receiver_module(self):
        """The firmware prints receiver=receiver module (two words): still a status line."""
        m = RfMonitor()
        line = "module=ook receiver=receiver module edges/s=1840 high=42% storms=0"
        self.assertTrue(is_status_line(line))
        m.feed(line)
        self.assertEqual((m.status["module"], m.status["receiver"], m.status["edges_per_s"]),
                         ("ook", "receiver module", 1840))
        m.feed("source transmissions=9 unknown=2 bursts=0 learning=0 jammed=0 confirmed=7 replays=1")
        self.assertEqual(m.status["transmissions"], 9)

    def test_capture(self):
        m = RfMonitor()
        m.feed("capturing 3000 ms: trigger the sensor now")
        self.assertFalse(m.capture["done"])
        m.feed("pulses 18 (H high, L low, microseconds; 0 = the line went quiet)")
        m.feed("H350 L10850 H350 L1050 H1050 L350 H350 L1050 H1050 L350 H350 L1050 H1050 L350 H350 L1050")
        m.feed("H1050 L0")
        self.assertTrue(m.capture["done"])
        text = m.capture_text({"port": "COM4"})
        self.assertIn("# port: COM4", text)
        self.assertIn("H1050 L0", text)


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from roboflash.controller import Controller
        from roboflash.server import FlasherServer
        cls.tmp = tempfile.TemporaryDirectory()
        world = MockWorld()
        cls.c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                           backups_dir=cls.tmp.name, captures_dir=cls.tmp.name, logs_dir=cls.tmp.name)
        cls.server = FlasherServer(cls.c)
        cls.server.serve_in_thread()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def call(self, path, token=None, host=None, data=None):
        req = urllib.request.Request(self.base + path, data=data, method="POST" if data is not None else "GET")
        if token:
            req.add_header("X-Flasher-Token", token)
        if host:
            req.add_header("Host", host)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def test_page_is_public_the_api_is_not(self):
        status, body = self.call("/")
        self.assertEqual(status, 200)
        self.assertIn(b"RoboAlarms", body)
        self.assertEqual(self.call("/api/state")[0], 403)
        self.assertEqual(self.call("/api/state", token="wrong")[0], 403)
        status, body = self.call("/api/state", token=self.server.token)
        self.assertEqual(status, 200)
        self.assertIn("ports", json.loads(body)["state"])

    def test_other_hosts_are_refused(self):
        self.assertEqual(self.call("/api/state", token=self.server.token, host="evil.example:80")[0], 403)
        self.assertEqual(self.call("/", host="rebind.example")[0], 403)

    def test_actions(self):
        self.assertEqual(self.call("/api/nope", token=self.server.token, data=b"{}")[0], 404)
        self.assertEqual(self.call("/api/probe", data=b"{}")[0], 403)
        status, body = self.call("/api/bundle", token=self.server.token, data=b"not a bundle")
        self.assertFalse(json.loads(body)["ok"])
        status, body = self.call("/api/bundle", token=self.server.token, data=testkit.bundle_bytes(testkit.standard_images()))
        self.assertTrue(json.loads(body)["ok"])
        status, body = self.call("/api/job", token=self.server.token,
                                 data=json.dumps({"kind": "restore", "port": "COM5", "path": "C:/Windows/x.bin"}).encode())
        self.assertIn("Backups folder", json.loads(body)["error"]["title"])


if __name__ == "__main__":
    unittest.main()
