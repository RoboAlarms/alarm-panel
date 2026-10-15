"""A CH340 that Windows lists without a COM port is reported as a missing or failed driver (FLS-001, BL-065)."""
import sys
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import ports as ports_mod  # noqa: E402
from roboflash.ports import PortWatcher, driver_issues_from, windows_driver_probe  # noqa: E402

CH340 = r"USB\VID_1A86&PID_7523\5&0000000&0&1"   # made-up instance ids
OTHER = r"USB\VID_046D&PID_C52B\5&0000000&0&2"


def ent(device_id, code):
    return {"DeviceID": device_id, "ConfigManagerErrorCode": code, "Name": "USB Serial"}


def port(device="COM5", vid=0x1A86, pid=0x7523):
    return SimpleNamespace(device=device, description="USB-SERIAL CH340", vid=vid, pid=pid,
                           manufacturer="", serial_number="", location="")


class Collect:
    def __init__(self):
        self.calls = []

    def __call__(self, ports):
        self.calls.append([p.device for p in ports])


def wait_for(cond, secs=2.0):
    end = time.time() + secs
    while time.time() < end and not cond():
        time.sleep(0.01)
    return cond()


class ParseTests(unittest.TestCase):
    def test_no_driver(self):
        got = driver_issues_from([ent(CH340, 28)])
        self.assertEqual(got, [{"chip": "CH340", "usb_id": "1A86:7523", "code": 28, "kind": "missing"}])

    def test_failed_driver(self):
        for code in (10, 31, 43):
            self.assertEqual(driver_issues_from([ent(CH340, code)])[0]["kind"], "failed")

    def test_ignores_working_unknown_and_junk(self):
        self.assertEqual(driver_issues_from([ent(CH340, 0), ent(OTHER, 28), {"DeviceID": None},
                                             ent(r"USB\VID_303A&PID_1001\x", 28), ent("ROOT\\X", 28)]), [])

    def test_names_the_chip_from_the_table(self):
        self.assertEqual(driver_issues_from([ent(r"USB\VID_1A86&PID_55D3\x", 28)])[0]["chip"], "CH343")

    def test_composite_children_collapse(self):
        got = driver_issues_from([ent(CH340, 28), ent(CH340.replace("VID_1A86&PID_7523", "VID_1A86&PID_7523&MI_00"), 28)])
        self.assertEqual(len(got), 1)


class ProbeTests(unittest.TestCase):
    def test_not_windows_asks_nothing(self):
        with mock.patch.object(ports_mod.sys, "platform", "linux"), \
                mock.patch.object(ports_mod.subprocess, "run") as run:
            self.assertEqual(windows_driver_probe(), [])
            run.assert_not_called()

    def test_windows_reads_one_object_and_a_list(self):
        for out in ('{"DeviceID":"%s","ConfigManagerErrorCode":28}' % CH340.replace("\\", "\\\\"),
                    '[{"DeviceID":"%s","ConfigManagerErrorCode":28}]' % CH340.replace("\\", "\\\\"), ""):
            with mock.patch.object(ports_mod.sys, "platform", "win32"), \
                    mock.patch.object(ports_mod.subprocess, "run", return_value=SimpleNamespace(stdout=out)):
                got = windows_driver_probe()
            self.assertEqual([i["code"] for i in got], [] if not out else [28])

    def test_timeout_and_error_raise_so_the_watcher_stays_quiet(self):
        import subprocess
        for exc in (subprocess.TimeoutExpired("powershell", 3), OSError("no powershell")):
            with mock.patch.object(ports_mod.sys, "platform", "win32"), \
                    mock.patch.object(ports_mod.subprocess, "run", side_effect=exc):
                with self.assertRaises(Exception):
                    windows_driver_probe()


class WatcherTests(unittest.TestCase):
    def watcher(self, lister, probe, got):
        return PortWatcher(got, lister=lister, driver_probe=probe, driver_every=0)

    def test_no_driver_is_reported_when_no_board_port(self):
        got = Collect()
        w = self.watcher(lambda: [], lambda: [{"chip": "CH340", "usb_id": "1A86:7523", "code": 28, "kind": "missing"}], got)
        w.poll()
        self.assertTrue(wait_for(lambda: w.driver_issues))
        self.assertEqual(w.driver_issues[0]["kind"], "missing")
        self.assertTrue(wait_for(lambda: len(got.calls) >= 1))      # the change was announced
        self.assertEqual(w._last[-1], [("1A86:7523", 28)])

    def test_failed_driver_is_reported(self):
        w = self.watcher(lambda: [], lambda: [{"chip": "CH340", "usb_id": "1A86:7523", "code": 31, "kind": "failed"}], Collect())
        w.poll()
        self.assertTrue(wait_for(lambda: w.driver_issues))
        self.assertEqual(w.driver_issues[0]["kind"], "failed")

    def test_a_listed_port_hides_it_and_is_not_asked(self):
        calls = []
        w = self.watcher(lambda: [port()], lambda: calls.append(1) or [{"chip": "CH340", "usb_id": "x", "code": 28, "kind": "missing"}], Collect())
        w.poll()
        time.sleep(0.1)
        self.assertEqual(w.driver_issues, [])
        self.assertEqual(calls, [])

    def test_a_port_appearing_clears_it(self):
        listed = []
        w = self.watcher(lambda: listed, lambda: [{"chip": "CH340", "usb_id": "1A86:7523", "code": 28, "kind": "missing"}], Collect())
        w.poll()
        self.assertTrue(wait_for(lambda: w.driver_issues))
        listed.append(port())
        w.poll()
        self.assertEqual(w.driver_issues, [])

    def test_enumerator_error_is_quiet(self):
        def boom():
            raise RuntimeError("no powershell")
        got = Collect()
        w = self.watcher(lambda: [], boom, got)
        w.poll()
        time.sleep(0.2)
        self.assertEqual(w.driver_issues, [])
        self.assertEqual(len(got.calls), 1)

    def test_slow_enumerator_never_blocks_the_poll(self):
        w = self.watcher(lambda: [], lambda: time.sleep(1.0) or [], Collect())
        t0 = time.time()
        w.poll()
        w.poll()
        self.assertLess(time.time() - t0, 0.5)

    def test_asked_at_most_every_interval(self):
        calls = []
        now = [0.0]
        w = PortWatcher(Collect(), lister=lambda: [], driver_probe=lambda: calls.append(1) or [],
                        driver_every=5.0, clock=lambda: now[0])
        w.poll()
        self.assertTrue(wait_for(lambda: len(calls) == 1))
        now[0] = 2.0
        w.poll()
        time.sleep(0.1)
        self.assertEqual(len(calls), 1)
        now[0] = 5.5
        w.poll()
        self.assertTrue(wait_for(lambda: len(calls) == 2))


if __name__ == "__main__":
    unittest.main()
