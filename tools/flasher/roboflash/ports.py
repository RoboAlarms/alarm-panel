"""The serial ports that are plugged in, and which of them look like an ESP32 board (FLS-001).

A USB bridge is a hint, never proof: both boards we support use a CH340, and so do thousands of
other gadgets. It decides what is listed first and what may be probed, not which firmware.
"""
import json
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, asdict

# USB-serial bridges found on ESP32 boards, and the ESP chips' own USB serial.
KNOWN_BRIDGES = {
    (0x1A86, 0x7523): "CH340",
    (0x1A86, 0x7522): "CH340K",
    (0x1A86, 0x55D3): "CH343",
    (0x1A86, 0x55D4): "CH9102",
    (0x10C4, 0xEA60): "CP210x",
    (0x0403, 0x6001): "FT232R",
    (0x0403, 0x6010): "FT2232",
    (0x0403, 0x6014): "FT232H",
    (0x0403, 0x6015): "FT231X",
    (0x303A, 0x1001): "ESP USB serial",
    (0x303A, 0x0002): "ESP USB serial",
}


@dataclass
class PortInfo:
    device: str
    description: str
    vid: int | None
    pid: int | None
    manufacturer: str
    serial_number: str
    location: str
    kind: str          # "esp": a known ESP bridge; "usb": another USB device; "other": not USB
    bridge: str        # the bridge's name for "esp", else ""

    @property
    def usb_id(self):
        return f"{self.vid:04X}:{self.pid:04X}" if self.vid is not None and self.pid is not None else ""

    def to_dict(self):
        d = asdict(self)
        d["usb_id"] = self.usb_id
        return d


def classify(raw):
    """PortInfo from a pyserial ListPortInfo (or anything with the same attributes)."""
    vid = getattr(raw, "vid", None)
    pid = getattr(raw, "pid", None)
    device = str(getattr(raw, "device", ""))
    description = str(getattr(raw, "description", "") or "")
    if description in ("n/a", device):
        description = ""
    bridge = KNOWN_BRIDGES.get((vid, pid), "") if vid is not None else ""
    bluetooth = "bluetooth" in (device + " " + description).lower()
    if bridge and not bluetooth:
        kind = "esp"
    elif vid is not None and not bluetooth:
        kind = "usb"
    else:
        kind = "other"
    return PortInfo(device=device, description=description, vid=vid, pid=pid,
                    manufacturer=str(getattr(raw, "manufacturer", "") or ""),
                    serial_number=str(getattr(raw, "serial_number", "") or ""),
                    location=str(getattr(raw, "location", "") or ""),
                    kind=kind, bridge=bridge)


def _natural(text):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", text)]


ORDER = {"esp": 0, "usb": 1, "other": 2}


def sort_ports(ports):
    return sorted(ports, key=lambda p: (ORDER[p.kind], _natural(p.device)))


def system_lister():
    from serial.tools import list_ports
    return list_ports.comports()


def list_ports(lister=system_lister):
    seen = {}
    for raw in lister():
        info = classify(raw)
        # macOS lists a device as both /dev/cu.* and /dev/tty.*: keep the call-up one.
        if info.device.startswith("/dev/tty.") and info.device.replace("/dev/tty.", "/dev/cu.", 1) in seen:
            continue
        seen[info.device] = info
    return sort_ports(seen.values())


# A USB bridge that Windows lists without a COM port: no driver (problem code 28) or one that
# would not start (10, 31, ...). The question is asked of Windows' device list, never of a port.
_VIDPID = re.compile(r"VID_([0-9A-F]{4})&PID_([0-9A-F]{4})", re.I)
_QUERY = ("Get-CimInstance Win32_PnPEntity -Filter 'ConfigManagerErrorCode <> 0' | "
          "Select-Object DeviceID,ConfigManagerErrorCode,Name | ConvertTo-Json -Compress")


def driver_issues_from(entities):
    """[{"chip", "usb_id", "code", "kind"}] from Win32_PnPEntity-like dicts (DeviceID,
    ConfigManagerErrorCode); kind is "missing" (code 28) or "failed" (any other code)."""
    found = {}
    for e in entities:
        m = _VIDPID.search(str(e.get("DeviceID", "")))
        code = e.get("ConfigManagerErrorCode")
        if not m or not isinstance(code, int) or code == 0:
            continue
        vid, pid = int(m.group(1), 16), int(m.group(2), 16)
        chip = KNOWN_BRIDGES.get((vid, pid))
        if chip is None or chip == "ESP USB serial":
            continue
        found[(vid, pid, code)] = {"chip": chip, "usb_id": f"{vid:04X}:{pid:04X}", "code": code,
                                   "kind": "missing" if code == 28 else "failed"}
    return sorted(found.values(), key=lambda i: (i["usb_id"], i["code"]))


def windows_driver_probe(timeout=3.0):
    """Driver problems of known bridges, asked of PowerShell (hidden window, time-boxed).
    [] anywhere but Windows; raises on any failure (the watcher stays quiet)."""
    if sys.platform != "win32":
        return []
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", _QUERY],
                         capture_output=True, text=True, timeout=timeout, creationflags=flags,
                         check=True).stdout.strip()
    if not out:
        return []
    data = json.loads(out)
    return driver_issues_from(data if isinstance(data, list) else [data])


class PortWatcher:
    """Polls the port list and calls on_change(ports) whenever it differs (FLS-001: within 2 s).

    While no likely board port is listed it also asks driver_probe (at most every driver_every
    seconds, on its own thread so the port poll never waits) for a known bridge Windows has no
    working driver for; the answer is in driver_issues, and a change calls on_change too."""

    def __init__(self, on_change, lister=system_lister, interval=1.0,
                 driver_probe=windows_driver_probe, driver_every=5.0, clock=time.monotonic):
        self._on_change = on_change
        self._lister = lister
        self._interval = interval
        self._driver_probe = driver_probe
        self._driver_every = driver_every
        self._clock = clock
        self._driver_next = 0.0
        self._driver_busy = False
        self._ports = []
        self._lock = threading.Lock()
        self.driver_issues = []
        self._stop = threading.Event()
        self._last = None
        self._thread = None

    def poll(self):
        try:
            ports = list_ports(self._lister)
        except Exception:
            return
        self._ports = ports
        if any(p.kind == "esp" for p in ports):
            self.driver_issues = []
        else:
            self._maybe_probe_drivers()
        key = [(p.device, p.vid, p.pid, p.serial_number, p.location) for p in ports]
        key.append([(i["usb_id"], i["code"]) for i in self.driver_issues])
        with self._lock:
            if key == self._last:
                return
            self._last = key
        self._on_change(ports)

    def _maybe_probe_drivers(self):
        now = self._clock()
        if self._driver_probe is None or self._driver_busy or now < self._driver_next:
            return
        self._driver_next = now + self._driver_every
        self._driver_busy = True
        threading.Thread(target=self._probe_drivers, name="drivers", daemon=True).start()

    def _probe_drivers(self):
        try:
            issues = list(self._driver_probe())
        except Exception:
            issues = []
        finally:
            self._driver_busy = False
        if any(p.kind == "esp" for p in self._ports):
            issues = []
        if issues != self.driver_issues:
            self.driver_issues = issues
            self.poll()

    def start(self):
        self.poll()
        self._thread = threading.Thread(target=self._run, name="ports", daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.wait(self._interval):
            self.poll()

    def stop(self):
        self._stop.set()
