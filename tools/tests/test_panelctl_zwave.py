"""docs/features/36 Z9: panelctl's Z-Wave developer commands, tested against a fake panel that
speaks the GET zwave / GET zwave/trace / POST zwave contract - no network, no real device."""
import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest

SPEC = importlib.util.spec_from_file_location("panelctl", Path(__file__).resolve().parents[1] / "panelctl.py")
p = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(p)


def make_device(node=2, generation=1, model="Zooz ZSE41 open/close sensor", security="s2_access",
                 battery=90, battery_low=False, heard_s=120, state="ready", unreachable=False,
                 listening=False, zones=(1,), check_in_s=3600, check_in_next_s=0):
    return {"node": node, "generation": generation, "model": model, "security": security,
            "listening": listening, "battery": battery, "battery_low": battery_low, "heard_s": heard_s,
            "state": state, "unreachable": unreachable, "zones": list(zones),
            "check_in_s": check_in_s, "check_in_next_s": check_in_next_s}


class FakeClock:
    """A controllable clock: time.sleep(s) advances it instead of really waiting."""
    def __init__(self):
        self.t = 0.0

    def monotonic(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds


class FakeZwavePanel:
    """A minimal Z-Wave panel: GET zwave / GET zwave/trace / POST zwave (docs/features/36 Z9),
    as a small state machine (listening -> stopped, restart through starting -> working)."""
    def __init__(self, stuck_restart=False, stop_is_broken=False):
        self.radio = {"state": "working", "place": "j7", "firmware": "1.50", "sdk": "7.22.1", "chip": 8,
                      "region": "us", "home_id": "c0ffee01", "node_id": 1, "devices": 0,
                      "check_in_s": 3600, "update_advised": False, "crypto": "passed"}
        self.devices = []
        self.progress = {"op": "none", "step": "idle", "seconds_left": 0, "result": "none",
                         "node": 0, "generation": 0, "requested": [], "dsk": ""}
        self.force_result = None
        self.last_post = None
        self._frames = []
        self._next_seq = 1
        self._lost = 0
        self._restart_left = 0
        self._pulse_supported = True
        self._stuck_restart = stuck_restart
        self._stop_is_broken = stop_is_broken

    def emit(self, direction, hexstr):
        self._frames.append({"seq": self._next_seq, "t": self._next_seq * 10, "dir": direction, "hex": hexstr})
        self._next_seq += 1

    def _status_doc(self):
        self.radio["devices"] = len(self.devices)
        return {"api": 1, "radio": dict(self.radio), "devices": [dict(d) for d in self.devices],
                "progress": dict(self.progress)}

    def _trace_doc(self, since):
        if since > self._next_seq:
            # `since` is past anything this epoch could have produced: the panel restarted
            # (sequence numbers restart at 1 each boot); answer starts at the oldest entry.
            batch = self._frames[:64]
        else:
            batch = [f for f in self._frames if f["seq"] >= since][:64]
        nxt = (batch[-1]["seq"] + 1) if batch else since
        lost, self._lost = self._lost, 0
        return {"api": 1, "next": nxt, "lost": lost, "frames": batch}

    def _tick_restart(self):
        if self._restart_left > 0:
            self._restart_left -= 1
            if self._restart_left == 0:
                self.radio["state"] = "working"
                self.emit("rx", "010f000a0300010100055e9f6c568f0189")

    def request(self, method, endpoint, data=None, headers=None):
        if method == "GET" and endpoint == "zwave":
            self._tick_restart()
            return json.dumps(self._status_doc()).encode()
        if method == "GET" and endpoint.startswith("zwave/trace"):
            since = int(endpoint.split("since=", 1)[1])
            return json.dumps(self._trace_doc(since)).encode()
        if method == "POST" and endpoint == "zwave":
            self.last_post = json.loads(data)
            return json.dumps(self._op(self.last_post)).encode()
        raise AssertionError(f"unexpected request {method} {endpoint}")

    def _op(self, body):
        if self.force_result is not None:
            return {"result": self.force_result}
        op = body.get("op")
        if op == "restart":
            self.radio["state"] = "starting"
            self._restart_left = 10 ** 9 if self._stuck_restart else 2
            return {"result": "ok"}
        if op == "pulse_reset":
            if not self._pulse_supported:
                return {"result": "unsupported"}
            self.radio["state"] = "starting"
            self._restart_left = 10 ** 9 if self._stuck_restart else 2
            return {"result": "ok"}
        if op in ("add", "remove"):
            self.progress = {"op": op, "step": "listening", "seconds_left": 60, "result": "none",
                             "node": 0, "generation": 0, "requested": [], "dsk": ""}
            return {"result": "ok"}
        if op == "stop":
            # A real panel reads "done" with its result until dismissed; only an explicit stop
            # gives "stopped" (the window running out on its own gives nothing_joined/removed,
            # not modelled here since panelctl always stops the window itself).
            if not self._stop_is_broken and self.progress.get("step") == "listening":
                self.progress = {**self.progress, "step": "done", "result": "stopped"}
            return {"result": "ok"}
        if op == "dismiss":
            self.progress = {"op": "none", "step": "idle", "seconds_left": 0, "result": "none",
                             "node": 0, "generation": 0, "requested": [], "dsk": ""}
            return {"result": "ok"}
        if op in ("allow", "pin", "keep_listening", "insecure", "region", "check_in",
                  "reset_network", "ping", "remove_failed", "setup"):
            return {"result": "ok"}
        return {"result": "unsupported"}


class ScriptedSensorPanel(FakeZwavePanel):
    """For the --sensor guided pass: the FIRST add/remove (the plain check's own window, steps
    c/d) behaves like the base class; the SECOND add/remove (the sensor pass's own) walks a fixed
    step script instead - listening -> ask_allow -> ask_pin -> securing -> reading -> done/added,
    and listening -> done/removed - one step advance per GET zwave poll, parked at ask_allow/
    ask_pin until the matching POST arrives."""
    ADD_SCRIPT = ["listening", "ask_allow", "ask_pin", "securing", "reading", "done"]
    REMOVE_SCRIPT = ["listening", "done"]

    def __init__(self):
        super().__init__()
        self._add_calls = 0
        self._remove_calls = 0
        self._script = None
        self._script_pos = 0
        self._step_observed = False
        self._interview_polls_left = 0
        self.added_node = 3
        self.added_generation = 1

    def _status_doc(self):
        self._advance_script()
        self._advance_interview()
        return super()._status_doc()

    def _advance_script(self):
        if not self._script:
            return
        step = self._script[self._script_pos]
        if step in ("ask_allow", "ask_pin", "done"):
            return  # waits for an explicit ack, or is terminal
        if self._step_observed:
            self._script_pos += 1
            self._step_observed = False
            new_step = self._script[self._script_pos]
            self.progress["step"] = new_step
            if new_step == "done":
                self._finish_scripted_op()
        else:
            self._step_observed = True

    def _finish_scripted_op(self):
        if self.progress["op"] == "add":
            self.progress["result"] = "added"
            self.progress["node"] = self.added_node
            self.progress["generation"] = self.added_generation
            self.devices.append(make_device(node=self.added_node, generation=self.added_generation,
                                            state="setting_up"))
            self._interview_polls_left = 2
        elif self.progress["op"] == "remove":
            self.progress["result"] = "removed"
            self.devices[:] = [d for d in self.devices if d["node"] != self.added_node]

    def _advance_interview(self):
        for d in self.devices:
            if d["state"] == "setting_up" and self._interview_polls_left > 0:
                self._interview_polls_left -= 1
                if self._interview_polls_left == 0:
                    d["state"] = "ready"

    def _op(self, body):
        op = body.get("op")
        if op == "add":
            self._add_calls += 1
            if self._add_calls == 1:
                return super()._op(body)  # the main check's own plain window (step c)
            self._script, self._script_pos, self._step_observed = list(self.ADD_SCRIPT), 0, False
            self.progress = {"op": "add", "step": self._script[0], "seconds_left": 60, "result": "none",
                             "node": 0, "generation": 0, "requested": ["access", "auth"],
                             "dsk": "12345-11111-22222-33333"}
            return {"result": "ok"}
        if op == "remove":
            self._remove_calls += 1
            if self._remove_calls == 1:
                return super()._op(body)  # the main check's own plain window (step d)
            self._script, self._script_pos, self._step_observed = list(self.REMOVE_SCRIPT), 0, False
            self.progress = {"op": "remove", "step": self._script[0], "seconds_left": 60, "result": "none",
                             "node": 0, "generation": 0, "requested": [], "dsk": ""}
            return {"result": "ok"}
        if op == "stop":
            self._script = None  # abandon script tracking; behave like the plain base class
            return super()._op(body)
        if op == "allow" and self.progress.get("step") == "ask_allow":
            self._script_pos += 1
            self._step_observed = False
            self.progress["step"] = self._script[self._script_pos]
            return {"result": "ok"}
        if op == "pin" and self.progress.get("step") == "ask_pin":
            self._script_pos += 1
            self._step_observed = False
            self.progress["step"] = self._script[self._script_pos]
            return {"result": "ok"}
        if op == "check_in":
            for d in self.devices:
                if d["node"] == body.get("node"):
                    d["check_in_next_s"] = body.get("seconds")
            return {"result": "ok"}
        return super()._op(body)


class FlakyPanel:
    """Raises a queued sequence of DeviceError statuses before answering normally."""
    def __init__(self, statuses):
        self.statuses = list(statuses)
        self.calls = 0

    def request(self, method, endpoint, data=None, headers=None):
        self.calls += 1
        if self.statuses:
            raise p.DeviceError(self.statuses.pop(0), "simulated")
        return json.dumps({"api": 1, "radio": {}, "devices": [], "progress": {},
                           "next": 1, "lost": 0, "frames": []}).encode()


class RouteErrorTests(unittest.TestCase):
    """A trace route is dev-build-only; state/requests exist in any Z-Wave build; 503 is transient."""
    def test_404_on_status_reports_no_zwave(self):
        panel = FlakyPanel([404])
        with self.assertRaisesRegex(RuntimeError, "no Z-Wave"):
            p.zwave_status(panel)

    def test_404_on_trace_reports_dev_builds_only(self):
        panel = FlakyPanel([404])
        with self.assertRaisesRegex(RuntimeError, "dev builds only"):
            p.zwave_trace_poll(panel, 0)

    def test_404_on_post_reports_no_zwave(self):
        panel = FlakyPanel([404])
        with self.assertRaisesRegex(RuntimeError, "no Z-Wave"):
            p.zwave_post(panel, "add")

    def test_503_is_retried_then_succeeds(self):
        panel = FlakyPanel([503])
        with patch.object(p.time, "sleep"):
            p.zwave_request(panel, "GET", "zwave")
        self.assertEqual(panel.calls, 2)

    def test_persistent_503_eventually_raises(self):
        panel = FlakyPanel([503, 503, 503])
        with patch.object(p.time, "sleep"):
            with self.assertRaises(p.DeviceError):
                p.zwave_request(panel, "GET", "zwave")

    def test_other_statuses_are_not_retried_or_translated(self):
        panel = FlakyPanel([400])
        with self.assertRaises(p.DeviceError):
            p.zwave_request(panel, "GET", "zwave")
        self.assertEqual(panel.calls, 1)


class StatusFormatTests(unittest.TestCase):
    def test_status_text_reports_radio_and_device_row(self):
        panel = FakeZwavePanel()
        panel.devices.append(make_device())
        text = p.zwave_status_text(json.loads(panel.request("GET", "zwave")))
        self.assertIn("working", text)
        self.assertIn("Zooz ZSE41", text)
        self.assertIn("90%", text)
        self.assertIn("zones [1]", text)

    def test_status_text_handles_unknown_battery_and_never_heard(self):
        panel = FakeZwavePanel()
        panel.devices.append(make_device(battery=-1, heard_s=-1, zones=()))
        text = p.zwave_status_text(json.loads(panel.request("GET", "zwave")))
        self.assertIn("battery unknown", text)
        self.assertIn("heard never", text)
        self.assertIn("zones [none]", text)

    def test_status_text_shows_running_operation(self):
        panel = FakeZwavePanel()
        panel.progress = {"op": "add", "step": "listening", "seconds_left": 52, "result": "none",
                          "node": 0, "generation": 0, "requested": ["access", "auth"], "dsk": ""}
        text = p.zwave_status_text(json.loads(panel.request("GET", "zwave")))
        self.assertIn("add", text)
        self.assertIn("listening", text)
        self.assertIn("52s left", text)
        self.assertIn("requested access,auth", text)

    def test_status_cli_prints_json_with_flag(self):
        panel = FakeZwavePanel()
        args = SimpleNamespace(zwave_command="status", json=True)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            p.zwave_command(args, panel)
        doc = json.loads(out.getvalue())
        self.assertEqual(doc["radio"]["state"], "working")


class TraceFormatTests(unittest.TestCase):
    def test_trace_poll_reports_lost_and_advances_next(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "0102")
        panel.emit("tx", "0304")
        panel._lost = 3
        doc = p.zwave_trace_poll(panel, 1)
        self.assertEqual(doc["lost"], 3)
        self.assertEqual([f["seq"] for f in doc["frames"]], [1, 2])
        self.assertEqual(doc["next"], 3)

    def test_trace_line_format(self):
        self.assertEqual(p.zwave_trace_line({"seq": 12, "t": 83412, "dir": "rx", "hex": "0105"}),
                         "12 83412 rx 0105")

    def test_trace_header_has_module_facts_and_no_ids(self):
        panel = FakeZwavePanel()
        header = p.zwave_trace_header(panel)
        self.assertEqual(header[0], "zwtrace 1")
        self.assertTrue(all(line.startswith("#") for line in header[1:]))
        joined = "\n".join(header)
        self.assertIn("1.50", joined)
        self.assertIn("7.22.1", joined)
        self.assertIn("8", joined)
        self.assertNotIn("c0ffee01", joined)  # never the home ID

    def test_trace_cmd_writes_header_once_and_all_replays_from_zero(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "AA")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "t.zwtrace"
            args = SimpleNamespace(output=str(out), all=True, follow=False)
            p.zwave_trace_cmd(args, panel)
            p.zwave_trace_cmd(args, panel)
            content = out.read_text().splitlines()
            self.assertEqual(content.count("zwtrace 1"), 1)
            self.assertIn("1 10 rx AA", content)

    def test_trace_cmd_reports_lost_line(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "AA")
        panel._lost = 5
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "t.zwtrace"
            args = SimpleNamespace(output=str(out), all=True, follow=False)
            p.zwave_trace_cmd(args, panel)
            self.assertIn("lost 5", out.read_text().splitlines())

    def test_trace_default_skips_existing_backlog_then_shows_new_frames(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "AA")  # already buffered before the command starts: must not be printed
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "live.zwtrace"
            args = SimpleNamespace(output=str(out), all=False, follow=True)
            calls = []

            def fake_sleep(_):
                if not calls:
                    calls.append(1)
                    panel.emit("tx", "BB")  # arrives only after the tool starts watching
                else:
                    raise KeyboardInterrupt

            with patch.object(p.time, "sleep", fake_sleep):
                with self.assertRaises(KeyboardInterrupt):
                    p.zwave_trace_cmd(args, panel)
            content = out.read_text().splitlines()
            self.assertNotIn("1 10 rx AA", content)
            self.assertIn("2 20 tx BB", content)

    def test_trace_default_skips_a_backlog_larger_than_one_batch(self):
        panel = FakeZwavePanel()
        for i in range(140):  # more than two full 64-entry batches
            panel.emit("rx", f"{i:02x}")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "t.zwtrace"
            args = SimpleNamespace(output=str(out), all=False, follow=False)
            p.zwave_trace_cmd(args, panel)
            content = out.read_text().splitlines()
            seq_lines = [line for line in content if line[:1].isdigit()]
            self.assertEqual(seq_lines, [])  # the whole backlog was drained silently, nothing printed
        with tempfile.TemporaryDirectory() as d:
            out2 = Path(d) / "live.zwtrace"
            args2 = SimpleNamespace(output=str(out2), all=False, follow=True)
            calls = []

            def fake_sleep(_):
                if not calls:
                    calls.append(1)
                    panel.emit("tx", "NEW")  # arrives only after the tool finished draining and started watching
                else:
                    raise KeyboardInterrupt

            with patch.object(p.time, "sleep", fake_sleep):
                with self.assertRaises(KeyboardInterrupt):
                    p.zwave_trace_cmd(args2, panel)
            content2 = out2.read_text().splitlines()
            seq_lines2 = [line for line in content2 if line[:1].isdigit()]
            self.assertEqual(len(seq_lines2), 1)
            self.assertTrue(seq_lines2[0].endswith("tx NEW"))

    def test_sequence_regression_is_marked_as_a_panel_restart(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "AA")
        panel.emit("rx", "BB")
        # The panel rebooted: sequence numbers restart at 1, old history is gone.
        panel._frames = []
        panel._next_seq = 1
        panel.emit("tx", "CC")
        since, lines, doc = p.zwave_trace_fetch(panel, 3)  # we were at seq 3 before the reboot
        self.assertLess(doc["next"], 3)
        self.assertIn("# panel restarted", lines)
        self.assertIn("1 10 tx CC", lines)

    def test_normal_idle_poll_is_never_mistaken_for_a_restart(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "AA")
        # Caught up exactly to the next not-yet-assigned sequence number: not a restart.
        since, lines, doc = p.zwave_trace_fetch(panel, panel._next_seq)
        self.assertEqual(lines, [])
        self.assertEqual(doc["next"], panel._next_seq)

    def test_trace_cmd_drains_a_full_batch_immediately_even_without_follow(self):
        panel = FakeZwavePanel()
        for i in range(70):
            panel.emit("rx", f"{i:02x}")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "t.zwtrace"
            args = SimpleNamespace(output=str(out), all=True, follow=False)
            p.zwave_trace_cmd(args, panel)
            content = out.read_text().splitlines()
            seq_lines = [line for line in content if line[:1].isdigit()]
            self.assertEqual(len(seq_lines), 70)  # both the 64-batch and the 6-entry remainder

    def test_damaged_frame_is_kept_as_is_without_crashing(self):
        panel = FakeZwavePanel()
        panel.emit("rx", "bad-checksum-short")  # not valid hex; must still round-trip untouched
        doc = p.zwave_trace_poll(panel, 0)
        line = p.zwave_trace_line(doc["frames"][0])
        self.assertEqual(line, "1 10 rx bad-checksum-short")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "t.zwtrace"
            args = SimpleNamespace(output=str(out), all=True, follow=False)
            p.zwave_trace_cmd(args, panel)  # must not raise
            self.assertIn("1 10 rx bad-checksum-short", out.read_text().splitlines())


class ArgumentValidationTests(unittest.TestCase):
    def test_pin_requires_exactly_five_digits(self):
        panel = FakeZwavePanel()
        for bad in ("1234", "123456", "12a45", "", " 12345"):
            with self.assertRaises(ValueError):
                p.zwave_command(SimpleNamespace(zwave_command="pin", pin=bad), panel)
        self.assertIsNone(panel.last_post)

    def test_pin_valid_posts_and_prints_result(self):
        panel = FakeZwavePanel()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            p.zwave_command(SimpleNamespace(zwave_command="pin", pin="12345"), panel)
        self.assertEqual(panel.last_post, {"op": "pin", "pin": "12345"})
        self.assertEqual(out.getvalue().strip(), "ok")

    def test_reset_network_refused_locally_without_confirm(self):
        panel = FakeZwavePanel()
        with self.assertRaises(RuntimeError):
            p.zwave_command(SimpleNamespace(zwave_command="reset-network", confirm=False), panel)
        self.assertIsNone(panel.last_post)

    def test_reset_network_with_confirm_sends_it(self):
        panel = FakeZwavePanel()
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="reset-network", confirm=True), panel)
        self.assertEqual(panel.last_post, {"op": "reset_network", "confirm": True})

    def test_allow_without_keys_omits_the_field_so_the_panel_grants_what_was_asked(self):
        panel = FakeZwavePanel()
        panel.progress["requested"] = ["access", "auth"]
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="allow", keys=None), panel)
        self.assertEqual(panel.last_post, {"op": "allow"})

    def test_allow_with_explicit_keys_sends_them(self):
        panel = FakeZwavePanel()
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="allow", keys="access,auth"), panel)
        self.assertEqual(panel.last_post, {"op": "allow", "keys": ["access", "auth"]})

    def test_allow_rejects_unknown_key(self):
        panel = FakeZwavePanel()
        with self.assertRaises(ValueError):
            p.zwave_command(SimpleNamespace(zwave_command="allow", keys="access,bogus"), panel)
        self.assertIsNone(panel.last_post)

    def test_check_in_new_device_uses_node_zero_no_lookup(self):
        panel = FakeZwavePanel()
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="check-in", node="new", duration="4h"), panel)
        self.assertEqual(panel.last_post, {"op": "check_in", "node": 0, "generation": 0, "seconds": 14400})

    def test_check_in_numeric_node_looks_up_generation(self):
        panel = FakeZwavePanel()
        panel.devices.append(make_device(node=7, generation=4))
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="check-in", node="7", duration="1h"), panel)
        self.assertEqual(panel.last_post, {"op": "check_in", "node": 7, "generation": 4, "seconds": 3600})

    def test_check_in_unknown_node_raises_before_posting(self):
        panel = FakeZwavePanel()
        with self.assertRaises(RuntimeError):
            p.zwave_command(SimpleNamespace(zwave_command="check-in", node="9", duration="1h"), panel)
        self.assertIsNone(panel.last_post)

    def test_node_lookup_generation(self):
        status = {"devices": [make_device(node=2, generation=3)]}
        self.assertEqual(p.zwave_generation(status, 2), 3)
        with self.assertRaises(RuntimeError):
            p.zwave_generation(status, 99)

    def test_ping_looks_up_generation_before_posting(self):
        panel = FakeZwavePanel()
        panel.devices.append(make_device(node=5, generation=2))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            p.zwave_command(SimpleNamespace(zwave_command="ping", node=5), panel)
        self.assertEqual(panel.last_post, {"op": "ping", "node": 5, "generation": 2})
        self.assertEqual(out.getvalue().strip(), "ok")

    def test_ping_unknown_node_raises_before_any_post(self):
        panel = FakeZwavePanel()
        with self.assertRaises(RuntimeError):
            p.zwave_command(SimpleNamespace(zwave_command="ping", node=42), panel)
        self.assertIsNone(panel.last_post)

    def test_remove_failed_looks_up_generation(self):
        panel = FakeZwavePanel()
        panel.devices.append(make_device(node=3, generation=6))
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="remove-failed", node=3), panel)
        self.assertEqual(panel.last_post, {"op": "remove_failed", "node": 3, "generation": 6})

    def test_setup_looks_up_generation(self):
        panel = FakeZwavePanel()
        panel.devices.append(make_device(node=2, generation=1))
        with contextlib.redirect_stdout(io.StringIO()):
            p.zwave_command(SimpleNamespace(zwave_command="setup", node=2), panel)
        self.assertEqual(panel.last_post, {"op": "setup", "node": 2, "generation": 1})

    def test_pin_range_refused_before_sending(self):
        for bad in ("65536", "99999", "1234", "12a45"):
            panel = FakeZwavePanel()
            with self.assertRaises(ValueError):
                p.zwave_command(SimpleNamespace(zwave_command="pin", pin=bad), panel)
            self.assertIsNone(panel.last_post)
        for good in ("00000", "65535"):
            panel = FakeZwavePanel()
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_command(SimpleNamespace(zwave_command="pin", pin=good), panel)
            self.assertEqual(panel.last_post, {"op": "pin", "pin": good})

    def test_simple_ops_translate_hyphen_to_underscore(self):
        panel = FakeZwavePanel()
        for verb, op in (("add", "add"), ("keep-listening", "keep_listening"), ("pulse-reset", "pulse_reset")):
            panel.progress = {"op": "none", "step": "idle", "seconds_left": 0, "result": "none",
                              "node": 0, "generation": 0, "requested": [], "dsk": ""}
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_command(SimpleNamespace(zwave_command=verb), panel)
            self.assertEqual(panel.last_post["op"], op)

    def test_result_other_than_ok_exits_nonzero_and_prints_the_word(self):
        panel = FakeZwavePanel()
        panel.force_result = "busy"
        with contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit) as cm:
                p.zwave_command(SimpleNamespace(zwave_command="add"), panel)
        self.assertEqual(cm.exception.code, 1)
        self.assertEqual(out.getvalue().strip(), "busy")

    def test_cli_rejects_unknown_check_in_duration_and_region(self):
        root = Path(__file__).resolve().parents[2]
        for extra in (["zwave", "check-in", "2", "30m"], ["zwave", "region", "mars"]):
            result = subprocess.run([sys.executable, str(root / "tools" / "panelctl.py"), *extra],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)


class BenchCheckTests(unittest.TestCase):
    def test_starting_is_not_a_listening_window(self):
        panel, clock = FakeZwavePanel(), FakeClock()
        original = panel._op
        def never_listens(body):
            result = original(body)
            if body["op"] == "add":
                panel.progress["step"] = "starting"
            return result
        panel._op = never_listens
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), \
             patch.object(p.time, "monotonic", clock.monotonic):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                p.zwave_check(SimpleNamespace(output_dir=d, sensor=False, yes=True), panel)
            steps = json.loads((Path(d) / "steps.json").read_text())
            self.assertEqual(next(s for s in steps if s["step"] == "add: listening")["result"], "FAIL")

    def test_stale_working_status_does_not_prove_a_reset(self):
        for operation, step_name in (("restart", "restart: back to working"),
                                     ("pulse_reset", "pulse-reset: radio returned")):
            with self.subTest(operation=operation):
                panel, clock = FakeZwavePanel(), FakeClock()
                original = panel._op
                panel._op = lambda body: ({"result": "ok"} if body["op"] == operation
                                          else original(body))
                with tempfile.TemporaryDirectory() as d, \
                     patch.object(p.time, "sleep", clock.sleep), \
                     patch.object(p.time, "monotonic", clock.monotonic):
                    with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                        p.zwave_check(SimpleNamespace(output_dir=d, sensor=False, yes=True), panel)
                    steps = json.loads((Path(d) / "steps.json").read_text())
                    step = next(s for s in steps if s["step"] == step_name)
                    self.assertEqual(step["result"], "FAIL")
                    self.assertIn("SerialAPIStarted=False", step["reason"])

    def test_restart_evidence_rejects_partial_or_corrupt_frames(self):
        good = "010f000a0300010100055e9f6c568f0189"
        self.assertTrue(p.zwave_started_frame({"dir": "rx", "hex": good}))
        for direction, data in (("tx", good), ("rx", good[:-2]),
                                ("rx", good[:-2] + "00"), ("rx", "not hex")):
            self.assertFalse(p.zwave_started_frame({"dir": direction, "hex": data}))

    def test_refuses_to_run_without_confirmation(self):
        panel = FakeZwavePanel()
        with tempfile.TemporaryDirectory() as d:
            args = SimpleNamespace(output_dir=d, sensor=False, yes=False)
            with patch("builtins.input", side_effect=EOFError):
                with self.assertRaises(RuntimeError):
                    with contextlib.redirect_stdout(io.StringIO()):
                        p.zwave_check(args, panel)

    def test_yes_flag_skips_the_interactive_prompt(self):
        panel = FakeZwavePanel()
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic), \
             patch("builtins.input", side_effect=AssertionError("should not prompt with --yes")):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_check(args, panel)  # must not raise

    def test_pass_path_writes_report_and_traces_with_no_failures(self):
        panel = FakeZwavePanel()
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_check(args, panel)
            out_dir = Path(d)
            steps = json.loads((out_dir / "steps.json").read_text())
            self.assertTrue(steps)
            self.assertTrue(all(s["result"] != "FAIL" for s in steps))
            self.assertTrue((out_dir / "report.md").exists())
            self.assertTrue((out_dir / "check.zwtrace").exists())
            self.assertTrue((out_dir / "startup.zwtrace").exists())
            self.assertTrue((out_dir / "status_initial.json").exists())
            self.assertTrue((out_dir / "status_final.json").exists())
            self.assertIn("zwtrace 1", (out_dir / "startup.zwtrace").read_text().splitlines())

    def test_fails_when_radio_not_answering(self):
        panel = FakeZwavePanel()
        panel.radio["state"] = "not_answering"
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    p.zwave_check(args, panel)
            self.assertEqual(cm.exception.code, 1)
            steps = json.loads((Path(d) / "steps.json").read_text())
            step = next(s for s in steps if s["step"] == "status: radio state")
            self.assertEqual(step["result"], "FAIL")

    def test_fails_when_crypto_self_test_failed(self):
        panel = FakeZwavePanel()
        panel.radio["crypto"] = "failed"
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    p.zwave_check(args, panel)
            self.assertEqual(cm.exception.code, 1)
            steps = json.loads((Path(d) / "steps.json").read_text())
            step = next(s for s in steps if s["step"] == "status: crypto self-test")
            self.assertEqual(step["result"], "FAIL")

    def test_fails_when_restart_never_comes_back(self):
        panel = FakeZwavePanel(stuck_restart=True)
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    p.zwave_check(args, panel)
            self.assertEqual(cm.exception.code, 1)
            steps = json.loads((Path(d) / "steps.json").read_text())
            step = next(s for s in steps if s["step"] == "restart: back to working")
            self.assertEqual(step["result"], "FAIL")

    def test_warns_but_does_not_fail_when_pulse_reset_unsupported(self):
        panel = FakeZwavePanel()
        panel._pulse_supported = False
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_check(args, panel)  # WARN must not raise SystemExit
            steps = json.loads((Path(d) / "steps.json").read_text())
            step = next(s for s in steps if s["step"] == "pulse-reset")
            self.assertEqual(step["result"], "WARN")

    def test_sensor_pass_can_be_skipped_step_by_step(self):
        panel = FakeZwavePanel()
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic), \
             patch("builtins.input", return_value="s"):
            args = SimpleNamespace(output_dir=d, sensor=True, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_check(args, panel)  # must not raise or hang
            steps = json.loads((Path(d) / "steps.json").read_text())
            sensor_step = next(s for s in steps if s["step"] == "sensor: add")
            self.assertEqual(sensor_step["result"], "WARN")

    def test_add_and_remove_windows_pass_with_done_and_stopped(self):
        panel = FakeZwavePanel()
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_check(args, panel)  # all PASS: must not raise
            steps = json.loads((Path(d) / "steps.json").read_text())
        add_ended = next(s for s in steps if s["step"] == "add: ended")
        remove_ended = next(s for s in steps if s["step"] == "remove: ended")
        self.assertEqual(add_ended["result"], "PASS")
        self.assertIn("step=done", add_ended["reason"])
        self.assertIn("result=stopped", add_ended["reason"])
        self.assertEqual(remove_ended["result"], "PASS")
        self.assertIn("step=done", remove_ended["reason"])
        self.assertIn("result=stopped", remove_ended["reason"])

    def test_add_window_fails_if_the_panel_stays_listening_after_stop(self):
        panel = FakeZwavePanel(stop_is_broken=True)
        clock = FakeClock()
        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic):
            args = SimpleNamespace(output_dir=d, sensor=False, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    p.zwave_check(args, panel)
            self.assertEqual(cm.exception.code, 1)
            steps = json.loads((Path(d) / "steps.json").read_text())
        step = next(s for s in steps if s["step"] == "add: ended")
        self.assertEqual(step["result"], "FAIL")
        self.assertIn("step=listening", step["reason"])

    def test_scripted_sensor_pass_all_steps_pass(self):
        panel = ScriptedSensorPanel()
        clock = FakeClock()

        def fake_input(prompt=""):
            if "Enter its 5-digit PIN" in prompt:
                return "12345"
            if "Press Enter once you've woken the sensor" in prompt:
                # The device wakes up and takes the new check-in interval.
                device = next(d for d in panel.devices if d["node"] == panel.added_node)
                device["check_in_s"] = device["check_in_next_s"]
                device["check_in_next_s"] = 0
                return ""
            if "Open then close the sensor" in prompt:
                panel.emit("rx", "AA")  # a real radio frame, as if the contact just changed
                return ""
            return ""  # every other (Enter to continue, 's' to skip) prompt: proceed

        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic), \
             patch("builtins.input", side_effect=fake_input):
            args = SimpleNamespace(output_dir=d, sensor=True, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                p.zwave_check(args, panel)  # all PASS: must not raise
            steps = json.loads((Path(d) / "steps.json").read_text())
        sensor_steps = [s for s in steps if s["step"].startswith("sensor:")]
        self.assertTrue(sensor_steps)
        for step in sensor_steps:
            self.assertEqual(step["result"], "PASS", step)
        names = {s["step"] for s in sensor_steps}
        self.assertEqual(names, {"sensor: added", "sensor: interview finished", "sensor: heard",
                                 "sensor: check-in waits for its wake-up", "sensor: check-in changed",
                                 "sensor: removed"})

    def test_scripted_sensor_pass_check_in_fails_if_device_never_takes_it(self):
        panel = ScriptedSensorPanel()
        clock = FakeClock()

        def fake_input(prompt=""):
            if "Enter its 5-digit PIN" in prompt:
                return "12345"
            if "Open then close the sensor" in prompt:
                panel.emit("rx", "AA")
                return ""
            return ""  # "Press Enter once you've woken the sensor" - the device never wakes

        with tempfile.TemporaryDirectory() as d, \
             patch.object(p.time, "sleep", clock.sleep), patch.object(p.time, "monotonic", clock.monotonic), \
             patch("builtins.input", side_effect=fake_input):
            args = SimpleNamespace(output_dir=d, sensor=True, yes=True)
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    p.zwave_check(args, panel)
            self.assertEqual(cm.exception.code, 1)
            steps = json.loads((Path(d) / "steps.json").read_text())
        step = next(s for s in steps if s["step"] == "sensor: check-in changed")
        self.assertEqual(step["result"], "FAIL")
        waits_step = next(s for s in steps if s["step"] == "sensor: check-in waits for its wake-up")
        self.assertEqual(waits_step["result"], "PASS")  # the request itself was accepted fine


if __name__ == "__main__":
    unittest.main()
