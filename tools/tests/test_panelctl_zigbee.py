"""docs/features/20 devices.md §12 (BL-143, ZB-M6): panelctl's Zigbee developer commands, tested
against a fake panel that speaks the GET zigbee / POST zigbee contract (zigbee_dev.c) - no
network, no real device. Fictional data only (IEEE addresses 02ab:cd00:...)."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest

SPEC = importlib.util.spec_from_file_location("panelctl", Path(__file__).resolve().parents[1] / "panelctl.py")
p = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(p)


def make_device(id=1, ieee="02abcd0000000001", name="Front door", model="Door/window sensor",
                kind="sensor", state="ready", battery=87, low=False, heard_s=40, signal="good",
                join="install_code", zones=(3,), unreachable=False):
    return {"id": id, "ieee": ieee, "name": name, "model": model, "state": state, "kind": kind,
            "power": "battery", "router": False, "battery": battery, "low": low, "heard_s": heard_s,
            "signal": signal, "lqi": 180, "rssi": -60, "unreachable": unreachable, "join": join,
            "setup_done": 6, "setup_total": 6, "zones": list(zones)}


class FakeZigbeePanel:
    """GET zigbee / POST zigbee as zigbee_dev.c answers them."""
    def __init__(self):
        self.radio = {"state": "working", "paused": "none", "jammed": False, "channel": 15,
                      "pan": 4660, "ext_pan": "02abcd00000000ff", "ieee": "02abcd0000000100",
                      "version": "rcp-1.0", "noise": -92, "survey": [None, None, None, None],
                      "quietest": 0, "restarts": 0, "learn_s": 0, "storage_full": False,
                      "devices": 0, "not_heard": 0, "codes_waiting": 0, "stack_free": 2900,
                      "stack_size": 8192, "nvs_free": 3200}
        self.devices = []
        self.progress = {"op": "none", "step": "idle", "result": "none", "seconds": 0, "id": 0,
                         "old_id": 0, "joined": [], "zones": []}
        self.codes = []
        self.posts = []
        self.answer = "ok"

    def request(self, method, endpoint, data=None, headers=None):
        if method == "GET" and endpoint == "zigbee":
            self.radio["devices"] = len(self.devices)
            return json.dumps({"api": 1, "radio": dict(self.radio), "devices": self.devices,
                               "progress": self.progress, "codes": self.codes}).encode()
        if method == "POST" and endpoint == "zigbee":
            self.posts.append(json.loads(data))
            return json.dumps({"result": self.answer}).encode()
        raise AssertionError(f"unexpected request {method} {endpoint}")


class FlakyPanel:
    def __init__(self, statuses):
        self.statuses = list(statuses)
        self.calls = 0

    def request(self, method, endpoint, data=None, headers=None):
        self.calls += 1
        if self.statuses:
            raise p.DeviceError(self.statuses.pop(0), "scripted")
        return json.dumps({"api": 1, "radio": {}, "devices": [], "progress": {}, "codes": []}).encode()


def run(panel, **args):
    with contextlib.redirect_stdout(io.StringIO()) as out:
        p.zigbee_command(SimpleNamespace(**args), panel)
    return out.getvalue()


class RouteTests(unittest.TestCase):
    # ZB-025/devices.md §12: a firmware without Zigbee answers 404, said in words
    def test_404_reports_no_zigbee(self):
        with self.assertRaisesRegex(RuntimeError, "no Zigbee"):
            p.zigbee_status(FlakyPanel([404]))
        with self.assertRaisesRegex(RuntimeError, "no Zigbee"):
            p.zigbee_post(FlakyPanel([404]), "add")

    def test_503_is_retried(self):
        panel = FlakyPanel([503])
        with patch.object(p.time, "sleep"):
            p.zigbee_status(panel)
        self.assertEqual(panel.calls, 2)


class StatusTests(unittest.TestCase):
    # ZB-011: the device list shows signal, battery, how it joined and its zones
    def test_status_text_lists_radio_devices_and_stack(self):
        panel = FakeZigbeePanel()
        panel.devices.append(make_device())
        text = p.zigbee_status_text(json.loads(panel.request("GET", "zigbee")))
        self.assertIn("Radio: working", text)
        self.assertIn("channel 15", text)
        self.assertIn("2900 of 8192 bytes", text)  # devices.md §16: the stack's high-water mark
        self.assertIn("Front door", text)
        self.assertIn("battery 87%", text)
        self.assertIn("joined install_code", text)
        self.assertIn("zones [3]", text)

    # ZB-003/ZB-024 (devices.md §16): alarm-path events the radio's queue lost are shown, never
    # only logged; an older firmware without the counters reads as none lost
    def test_status_text_shows_lost_alarm_events(self):
        panel = FakeZigbeePanel()
        text = p.zigbee_status_text(json.loads(panel.request("GET", "zigbee")))
        self.assertNotIn("LOST", text)
        self.assertNotIn("dropped", text)
        panel.radio.update(events_dropped=5, alarm_lost=2)
        text = p.zigbee_status_text(json.loads(panel.request("GET", "zigbee")))
        self.assertIn("ALARM EVENTS LOST 2", text)
        self.assertIn("radio events dropped 5", text)

    def test_status_text_unknowns_and_no_network(self):
        panel = FakeZigbeePanel()
        panel.radio.update(channel=None, noise=None, state="paused", paused="no_answer")
        panel.devices.append(make_device(battery=-1, heard_s=-1, zones=(), unreachable=True))
        text = p.zigbee_status_text(json.loads(panel.request("GET", "zigbee")))
        self.assertIn("paused: no_answer", text)
        self.assertIn("channel none", text)
        self.assertIn("battery unknown", text)
        self.assertIn("heard never", text)
        self.assertIn("zones [none]", text)
        self.assertIn("UNREACHABLE", text)

    def test_status_text_shows_operation_and_codes_waiting(self):
        panel = FakeZigbeePanel()
        panel.progress = {"op": "add", "step": "listening", "result": "none", "seconds": 170,
                          "id": 0, "old_id": 0, "joined": [{"id": 4, "step": "reading",
                                                            "result": "none", "asleep": False}],
                          "zones": []}
        panel.codes = [{"ieee": "02abcd0000000009", "added": 0}]
        text = p.zigbee_status_text(json.loads(panel.request("GET", "zigbee")))
        self.assertIn("add · listening", text)
        self.assertIn("170s left", text)
        self.assertIn("joined 4", text)
        self.assertIn("02abcd0000000009", text)

    def test_status_json_flag(self):
        doc = json.loads(run(FakeZigbeePanel(), zigbee_command="status", json=True))
        self.assertEqual(doc["radio"]["state"], "working")


class RequestTests(unittest.TestCase):
    # ZB-013: operations by registry id
    def test_id_ops_send_the_id(self):
        for verb in ("remove", "replace", "ping", "identify", "setup"):
            panel = FakeZigbeePanel()
            self.assertEqual(run(panel, zigbee_command=verb, id=7).strip(), "ok")
            self.assertEqual(panel.posts, [{"op": verb, "id": 7}])

    def test_plain_ops(self):
        for verb in ("add", "keep", "stop", "survey", "restart", "retry", "dismiss"):
            panel = FakeZigbeePanel()
            run(panel, zigbee_command=verb)
            self.assertEqual(panel.posts, [{"op": verb}])

    def test_refusal_exits_1_with_its_word(self):
        panel = FakeZigbeePanel()
        panel.answer = "not_allowed"
        with contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit) as caught:
                p.zigbee_command(SimpleNamespace(zigbee_command="add"), panel)
        self.assertEqual(caught.exception.code, 1)
        self.assertEqual(out.getvalue().strip(), "not_allowed")

    # ZB-013: forget is destructive - refused locally without --confirm
    def test_forget_needs_confirm(self):
        panel = FakeZigbeePanel()
        with self.assertRaises(RuntimeError):
            p.zigbee_command(SimpleNamespace(zigbee_command="forget", id=2, confirm=False), panel)
        self.assertEqual(panel.posts, [])
        run(panel, zigbee_command="forget", id=2, confirm=True)
        self.assertEqual(panel.posts, [{"op": "forget", "id": 2, "confirm": True}])

    # ZB-016: a channel move is 11..26 and confirmed
    def test_channel_range_and_confirm(self):
        panel = FakeZigbeePanel()
        for bad in (10, 27, 0):
            with self.assertRaises(ValueError):
                p.zigbee_command(SimpleNamespace(zigbee_command="channel", channel=bad, confirm=True), panel)
        with self.assertRaises(RuntimeError):
            p.zigbee_command(SimpleNamespace(zigbee_command="channel", channel=20, confirm=False), panel)
        self.assertEqual(panel.posts, [])
        run(panel, zigbee_command="channel", channel=20, confirm=True)
        self.assertEqual(panel.posts, [{"op": "channel", "channel": 20, "confirm": True}])

    # ZB-017: the network reset is never sent over the network (its installer code stays local)
    def test_reset_network_is_refused_locally(self):
        panel = FakeZigbeePanel()
        with self.assertRaisesRegex(RuntimeError, "never over the network"):
            p.zigbee_command(SimpleNamespace(zigbee_command="reset-network"), panel)
        self.assertEqual(panel.posts, [])

    # ZB-015: install codes - the address checked locally, the code never printed back
    def test_code_add_normalises_the_address_and_never_echoes_the_code(self):
        panel = FakeZigbeePanel()
        out = run(panel, zigbee_command="code-add", code="83FED3407A939723A5C639B26916D505C3B5",
                  ieee="0x02AB:CD00:0000:0001")
        self.assertEqual(panel.posts, [{"op": "code_add", "code": "83FED3407A939723A5C639B26916D505C3B5",
                                        "ieee": "02abcd0000000001"}])
        self.assertNotIn("83FED", out)

    def test_add_code_without_address_leaves_it_to_the_qr_text(self):
        panel = FakeZigbeePanel()
        run(panel, zigbee_command="add-code", code="Z:02ABCD0000000001$I:83FED3407A93", ieee=None)
        self.assertEqual(panel.posts, [{"op": "add_code", "code": "Z:02ABCD0000000001$I:83FED3407A93"}])

    def test_bad_addresses_and_empty_code_send_nothing(self):
        panel = FakeZigbeePanel()
        for bad in ("02abcd00000001", "02abcd00000000011", "zzabcd0000000001", ""):
            with self.assertRaises(ValueError):
                p.zigbee_command(SimpleNamespace(zigbee_command="code-forget", ieee=bad), panel)
        with self.assertRaises(ValueError):
            p.zigbee_command(SimpleNamespace(zigbee_command="code-add", code="  ", ieee=None), panel)
        self.assertEqual(panel.posts, [])
        run(panel, zigbee_command="code-forget", ieee="02-ab-cd-00-00-00-00-01")
        self.assertEqual(panel.posts, [{"op": "code_forget", "ieee": "02abcd0000000001"}])

    def test_rename(self):
        panel = FakeZigbeePanel()
        run(panel, zigbee_command="rename", id=3, name="Back door")
        self.assertEqual(panel.posts, [{"op": "rename", "id": 3, "name": "Back door"}])


if __name__ == "__main__":
    unittest.main()
