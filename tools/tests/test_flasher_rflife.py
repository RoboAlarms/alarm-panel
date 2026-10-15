"""The Flasher's 433 MHz receiver tool over its lifetime (FLS-014) and the page's event stream.

- M05: an unplugged port leaves no receiver state behind (its monitor, what it heard, its parts
  run), and a late worker or timer for it brings none back.
- PERF21: a subscriber that stalls past the event queue's size loses console rows first, keeps the
  latest state of each job, port list, parts run and device, and is told to fetch the snapshot.
- RF21: each recording a person asks for is saved once, as its own file, with its own pulses; a
  second one on the same port waits for the first.
- RF22: only a whole recording is saved; a short, broken or missing one is reported, not saved.
- RF23: closing a console, or the Flasher, turns the panel's raw logging off first, bounded.
"""
import gc
import queue
import sys
import tempfile
import threading
import time
import unittest
import weakref
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash.controller import Controller, EventHub  # noqa: E402
from roboflash.jobs import JobFailed  # noqa: E402
from roboflash.ports import classify  # noqa: E402
from roboflash.rf_test import RfMonitor  # noqa: E402


def port_info(name):
    return classify(SimpleNamespace(device=name, vid=0x1A86, pid=0x7522, description="USB-SERIAL CH340"))


def heard_lines(count):
    return b"".join(f"I ({1000 + i}) rf: EV1527 {i:05X} code 5 ({i:05X}5, T 350 us)\n".encode() for i in range(count))


class Panel:
    """What answers on one port: the replies `rf raw on|off` and `rf capture` give, by default."""

    def __init__(self):
        self.incoming = queue.Queue()
        self.commands = []
        self.raw_on = False
        self.silent = False         # writes go through, nothing answers
        self.write_delay = 0.0      # a write that hangs (a driver stuck)
        self.capture = None         # ms -> the bytes that answer `rf capture <ms>`

    def reply(self, cmd):
        if self.silent:
            return b""
        if cmd == "rf raw on":
            self.raw_on = True
            return b"raw on: every transmission is logged\n"
        if cmd == "rf raw off":
            self.raw_on = False
            return b"raw off\n"
        if cmd.startswith("rf capture") and self.capture is not None:
            return self.capture(int(cmd.split()[2]))
        return b""


class Bench:
    """Fake serial ports for any number of panels; no hardware."""

    def __init__(self):
        self.panels = {}
        self.serials = []

    def panel(self, port):
        return self.panels.setdefault(port, Panel())

    def opener(self):
        bench = self

        class FakeSerial:
            def __init__(self):
                self.port = self.baudrate = self.timeout = self.write_timeout = None
                self.dtr = self.rts = False
                self.closed = False

            def open(self):
                self.p = bench.panel(self.port)

            def read(self, n=1):
                try:
                    return self.p.incoming.get(timeout=0.005)
                except queue.Empty:
                    return b""

            def write(self, data):
                if self.p.write_delay:
                    time.sleep(self.p.write_delay)
                cmd = data.decode().strip()
                self.p.commands.append(cmd)
                out = self.p.reply(cmd)
                if out:
                    self.p.incoming.put(out)
                return len(data)

            def close(self):
                self.closed = True
        s = FakeSerial()
        self.serials.append(s)
        return s


def wait_for(cond, seconds=3.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if cond():
            return True
        time.sleep(0.01)
    return cond()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bench = Bench()
        self.c = Controller(lister=lambda: [], opener=self.bench.opener, backups_dir=self.tmp.name,
                            captures_dir=self.tmp.name, logs_dir=self.tmp.name, driver_probe=None)
        self.c.capture_grace = 0.3      # a missing reply is given up on quickly here

    def tearDown(self):
        self.c.stop()
        self.tmp.cleanup()

    def plug(self, *names):
        self.c._ports_changed([port_info(n) for n in names])


class UnpluggedPortTests(Base):
    """M05: what a port's receiver test kept goes when the board is unplugged."""

    def test_a_hundred_ports_leave_no_monitors_or_records(self):
        # M05: 100 distinct ports, each listened to with 64 transmissions heard, then unplugged.
        refs = []
        for i in range(100):
            name = f"COM{100 + i}"
            self.plug(name)
            self.c.console_open(name)
            self.c.parts_jobs[name] = self.c._parts_state(name, {}, "done")
            self.c.custom_seen.add(name)
            self.bench.panel(name).incoming.put(heard_lines(64))
            self.assertTrue(wait_for(lambda: len(self.c.rf[name].heard) == 64), name)
            refs.append(weakref.ref(self.c.rf[name]))
            self.plug()
        time.sleep(0.4)     # longer than an rf publish timer
        gc.collect()
        snap = self.c.snapshot()
        self.assertEqual((len(self.c.rf), snap["rf"], snap["parts"], self.c.custom_seen), (0, {}, {}, set()))
        self.assertEqual(sum(r() is not None for r in refs), 0)
        self.assertTrue(all(s.closed for s in self.bench.serials))

    def test_the_same_port_name_starts_afresh(self):
        # M05: another panel on the same port name shows nothing the last one heard.
        self.plug("COM6")
        self.c.console_open("COM6")
        self.bench.panel("COM6").incoming.put(heard_lines(3))
        self.assertTrue(wait_for(lambda: len(self.c.rf["COM6"].heard) == 3))
        self.plug()
        self.plug("COM6")
        self.c.console_open("COM6")
        self.assertEqual(self.c.snapshot()["rf"]["COM6"]["heard"], [])

    def test_late_work_for_an_unplugged_port_brings_nothing_back(self):
        # M05: a publish timer, a recording and a request that come after the unplug.
        self.plug("COM6")
        self.c.console_open("COM6")
        self.bench.panel("COM6").capture = lambda ms: b"capturing 500 ms: trigger the sensor now\n"
        events = self.c.events.subscribe()
        self.c.rf_capture("COM6", 500)
        self.assertTrue(wait_for(lambda: self.c.rf["COM6"].capture is not None))
        self.bench.panel("COM6").incoming.put(heard_lines(1))     # an rf publish timer starts
        time.sleep(0.05)
        while not events.empty():
            events.get_nowait()     # what came before the unplug
        self.plug()
        for request in (lambda: self.c.rf_listen("COM6", True), lambda: self.c.console_open("COM6"),
                        lambda: self.c.rf_capture("COM6", 500)):
            with self.assertRaises(JobFailed):
                request()
        self.c.rf_clear("COM6")
        self.c.rf_stop("COM6")
        time.sleep(1.2)     # the recording's wait and the timer are over
        self.assertEqual((self.c.rf, self.c.consoles, self.c.parts_jobs), ({}, {}, {}))
        self.assertNotIn("COM6", self.c.devices)
        seen = []
        while not events.empty():
            seen.append(events.get_nowait())
        self.assertFalse([e for e in seen if e["type"] == "rf"], "nothing published for a gone port")
        self.assertEqual(list(Path(self.tmp.name).glob("rf-capture-*")), [])

    def test_a_parts_run_that_ends_after_the_unplug_is_not_kept(self):
        # M05: the parts run's worker ends after its port went: its record goes with it.
        self.plug("COM6")
        st = self.c._parts_state("COM6", {})
        self.c.parts_jobs["COM6"] = st
        self.plug()
        self.assertIn("COM6", self.c.parts_jobs, "a running run is kept until it ends")
        self.c._parts_work("COM6", st, {})
        self.assertEqual(st["state"], "failed")
        self.assertNotIn("COM6", self.c.parts_jobs)
        self.assertNotIn("COM6", self.c.devices)


class EventOverflowTests(unittest.TestCase):
    """PERF21: a stalled subscriber loses console rows, never the latest state; it is told to resync."""

    def drain(self, q):
        out = []
        while not q.empty():
            out.append(q.get_nowait())
        return out

    def test_a_stalled_subscriber_keeps_the_job_end_and_is_told_to_resync(self):
        hub = EventHub()
        q = hub.subscribe()
        hub.publish("job", job={"id": "j1", "state": "running", "port": "COM6"})
        for i in range(2500):
            hub.publish("console", port="COM6", lines=[{"seq": i, "text": "x" * 40}])
            if i % 100 == 0:
                hub.publish("device", port="COM6", device={"n": i})
        hub.publish("job", job={"id": "j1", "state": "done", "port": "COM6"})
        hub.publish("ports", ports=[], driver_issues=[], devices={})
        got = self.drain(q)
        self.assertLessEqual(len(got), 2001)
        types = [e["type"] for e in got]
        self.assertIn("resync", types)
        self.assertEqual([e["job"]["state"] for e in got if e["type"] == "job"][-1], "done")
        self.assertIn("ports", types)
        self.assertEqual([e["device"]["n"] for e in got if e["type"] == "device"][-1], 2400)
        # Resumed: events come as before, without another resync.
        hub.publish("console", port="COM6", lines=[])
        self.assertEqual([e["type"] for e in self.drain(q)], ["console"])

    def test_no_resync_without_a_loss(self):
        hub = EventHub()
        q = hub.subscribe()
        for i in range(1999):
            hub.publish("console", port="COM6", lines=[])
        self.assertNotIn("resync", [e["type"] for e in self.drain(q)])

    def test_state_events_only_are_coalesced(self):
        # PERF21: 3000 changes of one parts run while stalled: bounded, and the last is kept.
        hub = EventHub()
        q = hub.subscribe()
        for i in range(3000):
            hub.publish("parts", port="COM6", parts={"n": i})
        got = self.drain(q)
        self.assertLessEqual(len(got), 2001)
        self.assertEqual([e["parts"]["n"] for e in got if e["type"] == "parts"][-1], 2999)
        self.assertIn("resync", [e["type"] for e in got])

    def test_the_page_converges_to_the_snapshot(self):
        # PERF21: a page model that follows the stream, stalled past 2000 events while a port goes.
        with tempfile.TemporaryDirectory() as tmp:
            c = Controller(lister=lambda: [], opener=Bench().opener, backups_dir=tmp, captures_dir=tmp,
                           logs_dir=tmp, driver_probe=None)
            try:
                c._ports_changed([port_info("COM6"), port_info("COM7")])
                page = c.snapshot()
                q = c.events.subscribe()
                for i in range(2500):
                    c._console_lines("COM6", [])
                c._ports_changed([port_info("COM7")])
                for e in self.drain(q):
                    if e["type"] == "ports":
                        page["ports"], page["devices"] = e["ports"], e["devices"]
                    elif e["type"] == "resync":
                        page = c.snapshot()
                self.assertEqual(page["ports"], c.snapshot()["ports"])
                self.assertEqual(list(page["devices"]), ["COM7"])
            finally:
                c.stop()


class PageResyncModelTests(unittest.TestCase):
    """PERF21: a Python model of web/app.js dispatch()/resync() (the repo has no JS test runner; a
    stalled page catching up after a resync is also a manual check in the Browser pane). While the
    snapshot is fetched, events other than console rows are held and replayed in order after it;
    a resync asked meanwhile is coalesced into one more."""

    class Page:
        def __init__(self, fetch):
            self.fetch, self.job, self.resyncing, self.again, self.held, self.fetches = fetch, None, False, False, [], 0

        def dispatch(self, ev):
            if ev["type"] == "resync":
                if self.resyncing:
                    self.again = True
                else:
                    self.start()
            elif self.resyncing and ev["type"] != "console":
                self.held.append(ev)
            else:
                self.on(ev)

        def on(self, ev):
            if ev["type"] == "job":
                self.job = ev["job"]["state"]

        def start(self):
            self.resyncing = True
            self.pending = self.fetch()      # the snapshot, taken now; it returns later
            self.fetches += 1

        def returns(self):
            self.on({"type": "job", "job": {"state": self.pending}})
            self.resyncing = False
            held, self.held = self.held, []
            for ev in held:
                self.on(ev)
            if self.again:
                self.again = False
                self.start()

    def test_a_job_end_during_the_fetch_is_not_undone_by_the_snapshot(self):
        page = self.Page(lambda: "running")
        page.dispatch({"type": "resync"})
        page.dispatch({"type": "job", "job": {"state": "done"}})   # published after the snapshot
        page.returns()
        self.assertEqual(page.job, "done")

    def test_a_resync_asked_during_a_fetch_is_one_more_after_it(self):
        states = iter(["running", "done"])
        page = self.Page(lambda: next(states))
        page.dispatch({"type": "resync"})
        page.dispatch({"type": "resync"})
        page.dispatch({"type": "resync"})
        self.assertEqual(page.fetches, 1)
        page.returns()
        self.assertEqual(page.fetches, 2)
        page.returns()
        self.assertEqual(page.job, "done")


class CaptureTests(Base):
    """RF21, RF22 (FLS-014): each recording asked for is its own, saved once and only whole."""

    @staticmethod
    def capture(pulses, expected=None, extra=b""):
        rows = " ".join(pulses)
        n = len(pulses) if expected is None else expected
        return lambda ms: (f"capturing {ms} ms: trigger the sensor now\n"
                           f"pulses {n} (H high, L low, microseconds; 0 = the line went quiet)\n"
                           + (rows + "\n" if rows else "")).encode() + extra

    def result(self, events, port, seconds=4.0):
        """The next recording result for `port`; another port's is kept for its own call."""
        stash = self.__dict__.setdefault("stash", [])
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for e in stash:
                if e["port"] == port:
                    stash.remove(e)
                    return e
            try:
                e = events.get(timeout=0.05)
            except queue.Empty:
                continue
            if e["type"] == "rf" and ("saved" in e or "capture_error" in e):
                stash.append(e)
        self.fail(f"no recording result for {port}")

    def test_two_ports_in_the_same_second_save_two_files(self):
        # RF21: two recordings that end in the same second are two files, each with its own pulses.
        self.plug("COM6", "COM7")
        self.bench.panel("COM6").capture = self.capture(["H111", "L222"])
        self.bench.panel("COM7").capture = self.capture(["H333", "L444"])
        events = self.c.events.subscribe()
        self.c.rf_capture("COM6", 500)
        self.c.rf_capture("COM7", 500)
        a, b = self.result(events, "COM6"), self.result(events, "COM7")
        self.assertNotEqual(a["saved"], b["saved"])
        self.assertIn("H111 L222", Path(a["saved"]).read_text(encoding="utf-8"))
        self.assertIn("H333 L444", Path(b["saved"]).read_text(encoding="utf-8"))
        self.assertIn("COM6", Path(a["saved"]).name)
        self.assertEqual(len(list(Path(self.tmp.name).glob("rf-capture-*"))), 2)

    def test_one_recording_at_a_time_on_a_port(self):
        # RF21: a second request while the first is in flight is refused; after it, it runs.
        self.plug("COM6")
        self.bench.panel("COM6").capture = lambda ms: b"capturing 500 ms: trigger the sensor now\n"
        events = self.c.events.subscribe()
        self.c.rf_capture("COM6", 500)
        self.assertTrue(self.c.snapshot()["rf"]["COM6"]["recording"])
        with self.assertRaisesRegex(JobFailed, "already"):
            self.c.rf_capture("COM6", 500)
        self.assertIn("capture_error", self.result(events, "COM6"))
        self.assertFalse(self.c.snapshot()["rf"]["COM6"]["recording"])
        self.bench.panel("COM6").capture = self.capture(["H1", "L2"])
        self.c.rf_capture("COM6", 500)
        first = self.result(events, "COM6")
        self.bench.panel("COM6").capture = self.capture(["H3", "L4"])
        self.c.rf_capture("COM6", 500)
        second = self.result(events, "COM6")
        self.assertNotEqual(first["saved"], second["saved"])
        self.assertIn("H1 L2", Path(first["saved"]).read_text(encoding="utf-8"))
        self.assertIn("H3 L4", Path(second["saved"]).read_text(encoding="utf-8"))

    def test_a_later_recording_does_not_replace_the_one_asked_for(self):
        # RF21: a second recording (typed at the console) arrives before the worker looks: the
        # request still saves the first, its own.
        self.plug("COM6")
        own = self.capture(["H10", "L20"])
        other = self.capture(["H30", "L40", "H50", "L60"])
        self.bench.panel("COM6").capture = lambda ms: own(ms) + other(ms)
        events = self.c.events.subscribe()
        self.c.rf_capture("COM6", 500)
        text = Path(self.result(events, "COM6")["saved"]).read_text(encoding="utf-8")
        self.assertIn("H10 L20", text)
        self.assertNotIn("H30", text)

    def test_only_a_whole_recording_is_saved(self):
        # RF22: short rows, no pulse header, the panel's error, and more rows than announced are
        # reported and not saved; a recording of no pulses is not saved either; a whole one is.
        cases = [
            ("short", self.capture(["H350", "L1050"], expected=4), "2 of the 4 pulses"),
            ("no header", lambda ms: b"capturing 500 ms: trigger the sensor now\n", "did not arrive"),
            ("error", lambda ms: b"capturing 500 ms: trigger the sensor now\nno memory\n", "no memory"),
            ("too many", self.capture(["H1", "L2", "H3"], expected=2), "announced 2 pulses, and 3"),
            ("nothing", lambda ms: b"", "did not start"),
            ("no pulses", self.capture([], expected=0), "No pulses were recorded"),
        ]
        self.plug("COM6")
        events = self.c.events.subscribe()
        for name, script, words in cases:
            with self.subTest(name):
                self.bench.panel("COM6").capture = script
                self.c.rf_capture("COM6", 500)
                e = self.result(events, "COM6")
                self.assertNotIn("saved", e)
                self.assertIn(words, e["capture_error"])
        self.assertEqual(list(Path(self.tmp.name).glob("rf-capture-*")), [])
        for name, script, count in (("whole", self.capture(["H350", "L1050", "H350", "L0"]), 4),):
            with self.subTest(name):
                self.bench.panel("COM6").capture = script
                self.c.rf_capture("COM6", 500)
                text = Path(self.result(events, "COM6")["saved"]).read_text(encoding="utf-8")
                self.assertIn(f"# expected pulses: {count}\n# received pulses: {count}\n# status: complete\n", text)
                self.assertIn("# port: COM6", text)

    def test_the_file_says_when_a_recording_is_incomplete(self):
        # RF22: capture_text never presents a short recording as whole.
        m = RfMonitor()
        for line in ("capturing 500 ms: x", "pulses 4 (H high, L low, microseconds; 0 = the line went quiet)",
                     "H350 L1050"):
            m.feed(line)
        text = m.capture_text({"port": "COM4"})
        self.assertIn("# expected pulses: 4\n# received pulses: 2\n# status: incomplete - 2 of the 4", text)
        self.assertIn("# pulses 2 (", text)


class RawOffTests(Base):
    """RF23 (FLS-014): raw logging is turned off when the console closes and when the Flasher stops."""

    def listening(self, port, *others):
        self.plug(port, *others)
        self.c.rf_start(port)
        self.c.rf_listen(port, True)
        self.assertTrue(wait_for(lambda: self.c.rf[port].raw_on))

    def test_the_flasher_stopping_turns_raw_logging_off(self):
        self.listening("COM6")
        self.c.stop()
        self.assertIn("rf raw off", self.bench.panel("COM6").commands)
        self.assertFalse(self.bench.panel("COM6").raw_on)
        self.assertTrue(all(s.closed for s in self.bench.serials))

    def test_closing_the_console_turns_raw_logging_off(self):
        self.listening("COM6")
        self.c.console_close("COM6")
        self.assertFalse(self.bench.panel("COM6").raw_on)
        self.assertNotIn("COM6", self.c.rf_polling)

    def test_a_console_not_listening_is_closed_without_a_command(self):
        self.plug("COM6", "COM7")
        self.c.console_open("COM6")
        self.c.console_open("COM7")
        self.c.console_close("COM6")
        self.c.stop()
        self.assertEqual(self.bench.panel("COM6").commands + self.bench.panel("COM7").commands, [])

    def test_a_panel_that_does_not_answer_holds_the_stop_about_a_second(self):
        # RF23: no reply, and a write that hangs, on two listening ports: stop() is bounded.
        for name in ("COM6", "COM7"):
            self.listening(name, *{"COM6", "COM7"} - {name})
        self.bench.panel("COM6").silent = True
        self.bench.panel("COM7").write_delay = 3.0
        started = time.monotonic()
        self.c.stop()
        self.assertLess(time.monotonic() - started, 3.0)
        self.assertIn("rf raw off", self.bench.panel("COM6").commands)


if __name__ == "__main__":
    unittest.main()
