"""The Flasher's state and the actions the page can ask for.

It owns the port list, what is known about each device (the passive probe, the chip, the
detection and the chosen image), the firmware in use (the firmware server's, fwserver.ServerSource,
or a file opened by hand), the job runner, console sessions, the 433 MHz monitors and the clock
module's page. Everything the page shows is in snapshot(); changes are published as events.
One program per port (FLS-011): a console on a port is closed before a probe, an identify or a
job uses the port, and opened again afterwards.
"""
import datetime
import hashlib
import itertools
import json
import queue
import re
import threading
import time
import webbrowser
from collections import deque
from dataclasses import dataclass
from pathlib import Path

from . import bundle as bundle_mod
from . import clock_test
from . import fwserver
from . import parts as parts_mod
from . import paths
from .boards import detect, image_options, load_catalogue, pick_image, verdict
from .bundle import flash_size_bytes
from .chip import FAST_BAUD
from .console import ConsoleSession
from .jobs import Runner, JobFailed, PROBE_MAX_AGE, refuse_unless_idle
from .ports import PortWatcher, system_lister, windows_driver_probe
from .probe import probe as probe_port
from .rf_test import RfMonitor, is_status_line
from .serialio import PortError, serial_opener

# The Zigbee module (FLS-030): Elecrow's ESP32-H2 board, written over its own USB-C port (the chip's
# USB Serial/JTAG) and only out of its socket. The Flasher can't tell from that port whether a panel
# around it is idle, so it never opens, probes or restarts such a port by itself: only the person's
# identify, read and radio actions do, each once they tick that the module is on its own.
MODULE_USB_ID = "303A:1001"
MODULE_CHIP = "esp32h2"
# A panel's USB bridges besides the catalogue's (which lists only the boards it offers): the CH340
# (1A86:7523) of the 4" E32R40T and of other panels.
PANEL_BRIDGES = {"1A86:7523"}
TICK_FIRST = "Tick that the module is on its own first."
TICK_WHY = ("The Flasher restarts it to read or write it, and a module seated in a panel restarts the "
            "panel. Nothing was changed.")
NOT_THE_MODULE = "This is a panel, not the Zigbee module."
NOT_THE_MODULE_ADVICE = ("Take the module out of the panel and plug its own USB-C port into this computer. "
                         "Nothing was changed.")
NOT_A_MODULE_PORT = "This port isn't the Zigbee module's."
NOT_A_MODULE_PORT_ADVICE = ("On its own USB-C cable the module shows as a port of its own (an ESP32's USB). "
                            "Nothing was changed.")
NOT_A_PANEL_PORT = "This is the Zigbee module's port, not a panel's."
NOT_A_PANEL_PORT_ADVICE = "Use Advanced tools > Zigbee module for it."
RAW_OFF_REPLY = re.compile(r"^raw off")
UNPLUGGED = "That board was unplugged."
PLUG_IN_AGAIN = "Plug it in again."
DEV_ONLY = "Only a development version of RoboAlarms is available for this panel."
DEV_ONLY_ADVICE = ("It comes with test zones and developer tools. Choose it in the Flasher's version list "
                   "if you want it. Nothing was written.")
ZIGBEE_STATUS = re.compile(r"ready=(\d+) update=(\d+)% version=(.*?) ?devices=(\d+) channel=(\d+) "
                           r"pan=([0-9a-fA-F]+) jammed=(\d+)")


def parse_zigbee_status(lines):
    """`zigbee status`'s reply (zigbee_console.c): {"known": False} from firmware without it;
    {"known": True, "enabled": False, "words"} with Zigbee off; else its fields. None for no reply."""
    lines = [str(t).strip() for t in lines if str(t).strip()]
    if not lines:
        return None
    if any(t.startswith("Unrecognized command") for t in lines):
        return {"known": False}
    for i, t in enumerate(lines):
        m = ZIGBEE_STATUS.match(t)
        if m:
            words = lines[i - 1] if i > 0 else ""
            return {"known": True, "enabled": True, "words": words, "ready": m.group(1) != "0",
                    "update": int(m.group(2)), "version": m.group(3).strip(), "devices": int(m.group(4)),
                    "channel": int(m.group(5)), "pan": m.group(6).lower(), "jammed": m.group(7) != "0"}
    return {"known": True, "enabled": False, "words": lines[0]}


SETUP_STAGE = re.compile(r"^SETUP stage (\S+)( \(recovery key owed\))?, setup Wi-Fi (open|closed)")
SETUP_WIFI = re.compile(r'^SETUP label: Wi-Fi "(.*)", password (\S+); open (\S+)')
# The first builds printed a setup code too ("setup code X; panel Y"); today's print the panel only.
SETUP_CODE = re.compile(r"^SETUP label: (?:setup code (\S+); )?panel (\S*)")
SETUP_CERT = re.compile(r"^SETUP label: certificate SHA-256 (.+)$")


# The board's refusal words (the Panel Manager's, panel.js ERRORS) for setup over USB (FLS-036).
PROVISION_WORDS = {
    "bad_request": "Check the Wi-Fi name and password (8 to 64 characters).",
    "bad_name": "Choose an installer name without spaces at the start or end.",
    "weak_password": "The installer's password is 12 to 64 characters.",
    "setup_stage": "This board is set up already, or not at that step.",
    "setup_stale": "It hasn't joined the Wi-Fi yet.",
    "setup_busy": "A phone is setting it up right now. Try again in a few minutes.",
    "locked": "The panel is armed or an alarm sounds: disarm it first.",
    "not_allowed": "Create the installer's access first.",
    "setup_try_again": "The board could not save that just now. Try again.",
}


def parse_setup_label(lines):
    """`web setup`'s reply (panel_web.c, web_setup.c; FLS-035): a board without a screen prints the
    label of its setup Wi-Fi. {"known": False} from firmware without it; {"known": True,
    "headless": False} from a panel with a screen; else the label's fields. None for no reply."""
    lines = [str(t).strip() for t in lines if str(t).strip()]
    if not lines:
        return None
    if any(t.startswith(("Unrecognized command", "WEB enabled=")) for t in lines):
        return {"known": False}
    if any(t.startswith("SETUP: this panel has a screen") for t in lines):
        return {"known": True, "headless": False}
    out = {"known": True, "headless": True}
    for t in lines:
        m = SETUP_STAGE.match(t)
        if m:
            out.update(stage=m.group(1), recovery_owed=bool(m.group(2)), open=m.group(3) == "open")
        m = SETUP_WIFI.match(t)
        if m:
            out.update(ssid=m.group(1), password=m.group(2), address=m.group(3))
        m = SETUP_CODE.match(t)
        if m:
            out.update(panel=m.group(2))
            if m.group(1):
                out.update(code=m.group(1))
        m = SETUP_CERT.match(t)
        if m:
            out["fingerprint"] = m.group(1).strip()
    return out if "ssid" in out else {"known": True, "headless": True, "unreadable": True}


def _parts_page(choice):
    """Where the page sets a choice up: its own pins have their own tab (FLS-029)."""
    return ("Advanced tools > Hardware > Custom pins" if isinstance(choice, dict) and choice.get("layout") == "custom"
            else "Connected parts")


def _coalesce_key(event):
    """Events that only say what something is now: a later one of the same key replaces an earlier
    one in a stalled subscriber's queue (PERF21). None: kept as it is (a recording's result)."""
    kind = event.get("type")
    if kind in ("ports", "bundle", "firmware"):
        return (kind,)
    if kind == "job":
        return ("job", (event.get("job") or {}).get("id"))
    if kind in ("device", "parts"):
        return (kind, event.get("port"))
    if kind == "rf" and "saved" not in event and "capture_error" not in event:
        return ("rf", event.get("port"))
    return None


class Subscriber:
    """One page's events, at most `size` of them (PERF21). When it falls that far behind, its
    console rows are dropped first (the snapshot has the console's recent history), the latest
    event of each state (job, ports, parts run, device, firmware) is kept, and one `resync` event
    after them tells the page to fetch the snapshot again: nothing it needs is silently lost, and
    the producers never wait for a slow page."""

    RESYNC = {"type": "resync"}

    def __init__(self, size=2000):
        self.size = size
        self._items = deque()
        self._cond = threading.Condition()

    def put(self, event):
        with self._cond:
            if len(self._items) >= self.size:
                self._compact()
                if event.get("type") == "console":
                    self._cond.notify()
                    return      # the snapshot the page fetches has it
            if len(self._items) >= self.size:
                self._items.popleft()       # nothing left to compact: the snapshot has it
            self._items.append(event)
            self._cond.notify()

    def _compact(self):
        latest = {}
        kept = []
        for e in self._items:
            if e is self.RESYNC or e.get("type") == "console":
                continue
            key = _coalesce_key(e)
            if key is not None:
                latest[key] = len(kept)
            kept.append((key, e))
        kept = [e for i, (key, e) in enumerate(kept) if key is None or latest[key] == i]
        self._items = deque(kept[-(self.size - 2):] if self.size > 2 else [])
        self._items.append(self.RESYNC)     # after what was kept: the snapshot is newer than all of it

    def get(self, timeout=None):
        with self._cond:
            if not self._items and not self._cond.wait_for(lambda: self._items, timeout):
                raise queue.Empty
            e = self._items.popleft()
            return dict(e) if e is self.RESYNC else e

    def get_nowait(self):
        with self._cond:
            if not self._items:
                raise queue.Empty
            e = self._items.popleft()
            return dict(e) if e is self.RESYNC else e

    def empty(self):
        with self._cond:
            return not self._items

    def qsize(self):
        with self._cond:
            return len(self._items)


class EventHub:
    def __init__(self):
        self._subs = set()
        self._lock = threading.Lock()
        self.last_seen = time.monotonic()

    def subscribe(self):
        q = Subscriber(2000)
        with self._lock:
            self._subs.add(q)
        return q

    def unsubscribe(self, q):
        with self._lock:
            self._subs.discard(q)
        self.last_seen = time.monotonic()

    @property
    def listeners(self):
        return len(self._subs)

    def publish(self, kind, **payload):
        payload["type"] = kind
        with self._lock:
            subs = list(self._subs)
        for q in subs:
            q.put(payload)


@dataclass
class Device:
    port: str
    probe: object = None
    chip: object = None
    busy: str = ""          # "probing" / "identifying" / "reading" / ""
    image_key: str = ""     # chosen by hand; "" = the detected one
    board: str = ""         # named by the person when its chip fits several boards (FLS-005)
    error: dict | None = None
    # A RoboAlarms console answered on this port since it was plugged in or last written: from then
    # on a restart needs its answer, so a panel gone quiet is never restarted blind (FLS2-001).
    roboalarms: bool = False


class Controller:
    def __init__(self, lister=system_lister, opener=serial_opener, connect=None, catalogue=None,
                 backups_dir=None, captures_dir=None, logs_dir=None, boot_timeout=None, driver_probe=False,
                 source=None):
        from . import chip as chip_mod
        self.events = EventHub()
        self.lister = lister
        self.opener = opener
        self.connect = connect or (lambda port, log, cancel, fast_baud: chip_mod.connect(port, log, cancel, fast_baud))
        self.catalogue = catalogue or load_catalogue()
        self.backups_dir = Path(backups_dir or paths.backups_dir())
        self.captures_dir = Path(captures_dir or paths.captures_dir())
        self.logs_dir = Path(logs_dir or paths.logs_dir())
        self.ports = []
        self.devices = {}
        # Panels on an ESP chip's own USB (303A:1001, the ESP32-S3 board's port; FLS-035) known by
        # their USB serial number (the chip's MAC) once their chip read as the board's or a
        # RoboAlarms console answered on them: plugged in again, they are a panel, not maybe the
        # Zigbee module. Kept for this run only.
        self.native_panels = set()
        # The firmware in use: the firmware server's (source, FLS-032) unless a file was opened.
        self.source = source
        self.from_file = source is None
        self.bundle = None
        self.bundle_error = ""
        if source is not None:
            source.on_change = self._source_changed
            source.log = self.log
        self.consoles = {}
        self.rf = {}
        self.rf_polling = set()
        self._rf_pending = set()
        self._captures = {}         # port -> the number of the recording in flight there (RF21)
        self._capture_count = itertools.count(1)
        self.capture_grace = 8.0    # seconds past a recording's length its rows may take (RF22)
        self._listed = False        # a port list came: a port not in it is gone (M05)
        self._lock = threading.RLock()
        self._log_path = None
        runner_kw = {} if boot_timeout is None else {"boot_timeout": boot_timeout}
        self.runner = Runner(emit=self._job_changed, catalogue=self.catalogue, connect=self.connect,
                             probe=self._probe_for_job, backups_dir=self.backups_dir, opener=opener,
                             log_sink=self.log, on_boot_line=self._boot_line, **runner_kw)
        self._reopen_after_job = set()
        self._parts_after = {}      # port -> the parts to set up once an install/update is done
        self.parts_jobs = {}        # port -> the last "set up the parts" run (FLS-024)
        self.custom_seen = set()    # ports last read with parts on custom pins (FLS-029)
        self._probe_later = set()   # probes asked for while a job ran: they run once it ends
        # the real driver query only for the real port list; tests and --mock pass their own (or none)
        if driver_probe is False:
            driver_probe = windows_driver_probe if lister is system_lister else None
        self.watcher = PortWatcher(self._ports_changed, lister=lister, driver_probe=driver_probe)
        self._stop = threading.Event()

    # -- lifetime -------------------------------------------------------------------------------
    def start(self):
        self.watcher.start()
        threading.Thread(target=self._rf_loop, name="rf poll", daemon=True).start()
        if self.source is not None:
            self.source.start(list(self.catalogue))
        self.log("Flasher started" + (f", firmware from {self.source.client.address}" if self.source else ""))

    def stop(self):
        self._stop.set()
        self.watcher.stop()
        if self.source is not None:
            self.source.stop()
        with self._lock:
            consoles = list(self.consoles.items())
        self._raw_off(consoles)     # RF23: a listening panel stops logging every transmission
        for _, c in consoles:
            c.close()

    def log(self, text):
        try:
            if self._log_path is None:
                self.logs_dir.mkdir(parents=True, exist_ok=True)
                self._log_path = self.logs_dir / f"flasher-{datetime.date.today().isoformat()}.log"
            with open(self._log_path, "a", encoding="utf-8") as f:
                f.write(f"{datetime.datetime.now().isoformat(timespec='seconds')} {text}\n")
        except OSError:
            pass

    # -- roles (FLS-030) ------------------------------------------------------------------------
    def _usb_id(self, port):
        info = next((p for p in self.ports if p.device == port), None)
        return info.usb_id if info is not None else ""

    def _serial(self, port):
        info = next((p for p in self.ports if p.device == port), None)
        return info.serial_number if info is not None else ""

    def native_boards(self, port):
        """The catalogue boards that use this port's USB id as their own (an ESP chip's USB: the
        ESP32-S3 board on 303A:1001, FLS-035), never the Zigbee module's chip."""
        usb_id = self._usb_id(port)
        if not usb_id:
            return []
        return [b for b, info in self.catalogue.items() if isinstance(info, dict)
                and info.get("chip") != MODULE_CHIP
                and any(isinstance(u, dict) and str(u.get("usb_id", "")).upper() == usb_id
                        for u in info.get("usb", ()))]

    def _known_panel(self, port):
        """A 303A:1001 port that is a panel for sure (FLS-035): a RoboAlarms console answered on it,
        or its chip read as a board that uses that USB (chip and flash size), or it was one of those
        earlier in this run (its USB serial number, the chip's MAC). Never an ESP32-H2."""
        d = self.devices.get(port) or Device(port)
        if d.chip is not None and d.chip.chip == MODULE_CHIP:
            return False
        if d.probe is not None and d.probe.answered:
            return True
        if d.chip is not None:
            return any(d.chip.chip == self.catalogue[b]["chip"]
                       and (not d.chip.flash_size or flash_size_bytes(self.catalogue[b]["flash_size"]) == d.chip.flash_size)
                       for b in self.native_boards(port))
        serial = self._serial(port)
        return bool(serial) and (self._usb_id(port), serial) in self.native_panels

    def _remember_native(self, port):
        """A 303A:1001 port found to be a panel: known again when it is plugged back in."""
        serial = self._serial(port)
        if serial and self._usb_id(port) == MODULE_USB_ID and self._known_panel(port):
            self.native_panels.add((MODULE_USB_ID, serial))

    def is_module_port(self, port):
        """A port of an ESP chip's own USB Serial/JTAG: maybe the Zigbee module, never opened unasked
        (FLS-030) - unless it is known to be a panel on its own USB (FLS-035)."""
        return self._usb_id(port) == MODULE_USB_ID and not self._known_panel(port)

    def role(self, port):
        """(role, maybe): "panel" for a panel's USB bridge (boards.json) or a RoboAlarms console that
        answered, or a 303A:1001 port whose chip read as a board that uses that USB (the ESP32-S3
        board, FLS-035); "zigbee_module" for a 303A:1001 port once its chip reads as an ESP32-H2;
        else "unknown", with maybe "zigbee_module" while a 303A:1001 port's chip is not known."""
        d = self.devices.get(port) or Device(port)
        usb_id = self._usb_id(port)
        bridges = PANEL_BRIDGES | {u.get("usb_id", "").upper() for b in self.catalogue.values()
                                   if isinstance(b, dict) for u in b.get("usb", ()) if isinstance(u, dict)}
        if (usb_id and usb_id in bridges and usb_id != MODULE_USB_ID) or (d.probe is not None and d.probe.answered):
            return "panel", ""
        if usb_id == MODULE_USB_ID:
            if self._known_panel(port):
                return "panel", ""
            if d.chip is None:
                return "unknown", "zigbee_module"
            if d.chip.chip == MODULE_CHIP:
                return "zigbee_module", ""
        return "unknown", ""

    # -- snapshot -------------------------------------------------------------------------------
    def device_dict(self, port):
        """What the page shows of a device. With the firmware server in use, a board that is known
        has its release asked for here (ServerSource.want: once), so that it downloads while the
        person goes through the steps."""
        d = self.devices.get(port) or Device(port)
        server = self.serving()
        profile = self._profile_for if server else None
        det = detect(self.catalogue, self.bundle, d.probe, d.chip, profile=profile, say_missing=not server,
                     strict=server)
        if d.board:
            rev = d.chip.revision if d.chip is not None else (d.probe.chip_revision if d.probe else None)
            board = d.board
            auto = pick_image(self.bundle, d.board, rev,
                              (d.probe.profile if d.probe else "") if profile is None else profile(d.board), server)
        else:
            board = det.board or (d.probe.board if d.probe else "")
            auto = det.image_key
        chosen = d.image_key or auto
        options = image_options(self.bundle, self.catalogue, d.probe, d.chip)
        role, maybe = self.role(port)
        if d.image_key and self.bundle is not None and self.bundle.image(d.image_key):
            board = self.bundle.image(d.image_key).board     # the build the person chose: what it will be
        module = self.is_module_port(port)
        return {"port": port, "busy": d.busy, "error": d.error, "role": role, "maybe": maybe,
                "needs_alone": module,
                # FLS-035: the boards that may be on this port too (the ESP32-S3 board on 303A:1001):
                # the page asks the person before anything looks at it.
                "maybe_panel": self.native_boards(port) if module and role != "zigbee_module" else [],
                "probe": d.probe.to_dict() if d.probe else None,
                "chip": d.chip.to_dict() if d.chip else None,
                "detection": det.to_dict(), "image_key": chosen, "manual": bool(d.image_key or d.board),
                "options": options,
                "board": board if board in self.catalogue else "",
                "board_info": self.catalogue.get(board, None),
                "firmware": self._firmware_for(board) if server else None,
                "console_open": port in self.consoles and self.consoles[port].is_open,
                "rf_polling": port in self.rf_polling}

    def _parts_state(self, port, choice, state="running", error=None):
        return {"port": port, "state": state, "choice": parts_mod.normalise(choice), "error": error,
                "warnings": [], "found": {},
                "steps": [{"key": "send", "label": "Send the settings", "state": "waiting", "detail": ""},
                          {"key": "restart", "label": "Restart the panel", "state": "waiting", "detail": ""},
                          {"key": "check", "label": "Check it took them", "state": "waiting", "detail": ""}]}

    def _publish_parts(self, port):
        self.events.publish("parts", port=port, parts=self.parts_jobs.get(port))

    def snapshot(self):
        with self._lock:
            job = self.runner.current.to_dict() if self.runner.current else None
            return {"ports": [p.to_dict() for p in self.ports], "driver_issues": self.watcher.driver_issues,
                    "devices": {p.device: self.device_dict(p.device) for p in self.ports},
                    "bundle": self.bundle.to_dict() if self.bundle else None,
                    "bundle_error": self.bundle_error, "job": job,
                    "firmware": self.firmware_state(),
                    "catalogue": self.catalogue,
                    "consoles": {port: [l.to_dict() for l in list(c.history)[-800:]]
                                 for port, c in self.consoles.items()},
                    "rf": {port: self._rf_dict(port, m) for port, m in self.rf.items()},
                    "parts": dict(self.parts_jobs),
                    "backups": self.list_backups(),
                    "folders": {"backups": str(self.backups_dir), "captures": str(self.captures_dir),
                                "logs": str(self.logs_dir)}}

    def _present(self, port):
        """Whether `port` is in the port list now."""
        return any(p.device == port for p in self.ports)

    def _gone(self, port):
        """Whether `port` was unplugged: a port list came and it isn't in it. A late worker or
        request for it then brings none of its state back (M05)."""
        return self._listed and not self._present(port)

    def _device_changed(self, port):
        if self._present(port):
            self.events.publish("device", port=port, device=self.device_dict(port))

    # -- ports ----------------------------------------------------------------------------------
    def _ports_changed(self, ports):
        # PERF23: the consoles of ports that are gone are let go under the lock but closed after it:
        # closing joins their reader, which may be waiting for this lock (_publish_rf_soon).
        # M05: what the port's receiver test and parts run kept goes with it (a run still going is
        # dropped when it ends); _gone() keeps late workers from bringing it back.
        closing = []
        with self._lock:
            before = {p.device for p in self.ports}
            self.ports = ports
            self._listed = True
            now = {p.device for p in ports}
            for gone in before - now:
                self.devices.pop(gone, None)
                c = self.consoles.pop(gone, None)
                if c:
                    closing.append(c)
                self.rf_polling.discard(gone)
                self._rf_pending.discard(gone)
                self.rf.pop(gone, None)
                self._captures.pop(gone, None)
                self.custom_seen.discard(gone)
                run = self.parts_jobs.get(gone)
                if run is None or run.get("state") != "running":
                    self.parts_jobs.pop(gone, None)
            for p in ports:
                self.devices.setdefault(p.device, Device(p.device))
        for c in closing:
            c.close()
        self.log("ports: " + (", ".join(f"{p.device} ({p.usb_id or p.kind})" for p in ports) or "none"))
        self.events.publish("ports", ports=[p.to_dict() for p in ports], driver_issues=self.watcher.driver_issues,
                            devices={p.device: self.device_dict(p.device) for p in ports})

    # -- firmware: the server's, or a file ------------------------------------------------------
    def serving(self):
        """Whether the firmware comes from the firmware server (not a file opened by hand)."""
        return self.source is not None and not self.from_file

    def firmware_state(self):
        if self.source is None:
            return None
        state = self.source.to_dict()
        state["from_file"] = self.from_file
        return state

    def _profile_for(self, board):
        """The build a board gets from the firmware server: development builds from Dev only, every
        panel alike (FLS-033; what it runs now doesn't choose)."""
        return "dev" if self.source.channel_for(board) == "dev" else "release"

    def _firmware_for(self, board):
        """The firmware server's state for the device's board ({state, version, done, size, error,
        dev_offered...}), its download asked for (an offered development build is only looked up);
        None when no board is known."""
        if board not in self.catalogue:
            return None
        offered = self.source.dev_offered(board)
        state = self.source.want(board, fetch=not offered).to_dict()
        state["dev_offered"] = offered
        return state

    def _server_bundle(self):
        """One Bundle of every release ready to install (each board's from its channel): the images
        tagged with their release, and only the packs of those releases (Bundle.packs_for)."""
        ready = self.source.ready()
        if not ready:
            return None
        images, seen = [], set()
        for t in ready:
            for image in t.bundle.images:
                if image.key not in seen:
                    seen.add(image.key)
                    images.append(image)
        origins = {i.origin for i in images}

        def of(what):
            return [x for t in ready for x in getattr(t.bundle, what) if x.origin in origins]
        radios = next((t.bundle.radios for t in ready if t.bundle.radios), [])
        return bundle_mod.Bundle(name="", created="", source={}, signing_key_sha256="", images=images,
                                 languages=of("languages"), assets=of("assets"), voices=of("voices"),
                                 radios=list(radios))

    def _source_changed(self, what):
        """The firmware server's side changed: a release looked up, downloading or ready, newer board
        descriptions ("catalogue")."""
        with self._lock:
            if what == "catalogue":
                self.catalogue = load_catalogue()
                self.runner.catalogue = self.catalogue
            if self.serving():
                self.bundle = self._server_bundle()
        self.events.publish("firmware", firmware=self.firmware_state(),
                            bundle=self.bundle.to_dict() if self.bundle else None, bundle_error=self.bundle_error,
                            catalogue=self.catalogue,
                            devices={p.device: self.device_dict(p.device) for p in self.ports})

    def set_bundle(self, bundle, error=""):
        """Use a bundle from a file or build folders (or none, with the reason): the firmware server
        is not used until firmware_use_server()."""
        with self._lock:
            self.bundle, self.bundle_error, self.from_file = bundle, error, True
        if bundle:
            self.log(f"bundle {bundle.name}: " + ", ".join(i.key for i in bundle.images))
        self.events.publish("bundle", bundle=bundle.to_dict() if bundle else None, error=error,
                            firmware=self.firmware_state(),
                            devices={p.device: self.device_dict(p.device) for p in self.ports})

    def load_bundle_bytes(self, data, name):
        try:
            b = bundle_mod.load(data, name=name)
        except bundle_mod.BundleError as e:
            self.bundle_error = f"{name}: {e}"
            self.events.publish("bundle", bundle=self.bundle.to_dict() if self.bundle else None,
                                error=self.bundle_error, devices={})
            raise JobFailed(f"That file is not a usable firmware bundle: {e}", "") from None
        self.set_bundle(b)
        return b

    def load_bundle_path(self, path):
        return self.load_bundle_bytes(Path(path).read_bytes(), Path(path).name)

    def firmware_use_server(self):
        """Back to the firmware server's firmware after a file."""
        if self.source is None:
            raise JobFailed("This Flasher was started without the firmware server.", "")
        with self._lock:
            self.from_file, self.bundle_error = False, ""
            self.bundle = self._server_bundle()
        self.log("firmware: from the firmware server again")
        self._source_changed("targets")

    def firmware_channel(self, channel, board=None):
        """The person's version choice for this run: a channel, or "auto" for Stable, else Beta,
        else Alpha (FLS-033); for one board (the version list), or for all (Software)."""
        if self.source is None:
            raise JobFailed("This Flasher was started without the firmware server.", "")
        if board is not None and board not in self.catalogue:
            raise JobFailed("The Flasher can't get firmware for that board.", "")
        try:
            self.source.set_channel(str(channel or "auto"), board)
        except ValueError:
            raise JobFailed("Unknown release channel.", "") from None
        self.log(f"firmware: channel {channel or 'auto'}" + (f" for {board}" if board else ""))
        return {"firmware": self.firmware_state()}

    def firmware_refresh(self):
        if self.source is None:
            raise JobFailed("This Flasher was started without the firmware server.", "")
        self.source.refresh()

    def firmware_get(self, board):
        """Download a board's release now (Advanced tools > Software), or try again after a failure."""
        if self.source is None or board not in self.catalogue:
            raise JobFailed("The Flasher can't get firmware for that board.", "")
        if self.source.dev_offered(board):
            raise JobFailed(DEV_ONLY, DEV_ONLY_ADVICE)
        t = self.source.want(board, fetch=True)
        if t.state == "failed":
            self.source.retry(board)

    def open_flasher_download(self):
        """The newer Flasher's download page, in the browser: never replaced by itself."""
        program = self.source.program if self.source is not None else None
        if not program:
            raise JobFailed("There is no newer Flasher to download.", "")
        webbrowser.open(program["url"])

    def _not_ready(self, port):
        """(title, advice) for an install the firmware server's release isn't ready for."""
        fw = self.device_dict(port).get("firmware")
        if fw is None:
            return "No image is chosen for this board.", "Choose the board first. Nothing was written."
        if fw.get("dev_offered"):
            return DEV_ONLY, DEV_ONLY_ADVICE
        if fw["state"] == "failed" and fw["error"]:
            return fw["error"]["title"], fw["error"]["advice"] + " Nothing was written."
        if fw["state"] == "none":
            if self.source.picked(fw["board"]) is None:
                return "There is no RoboAlarms release for this board yet.", "Nothing was written."
            return (f"There is no RoboAlarms release for this board on {fw['channel'].capitalize()}.",
                    "Choose another version in the Flasher's version list. Nothing was written.")
        if fw["state"] == "downloading":
            return "The firmware is still downloading.", "Wait for it to finish. Nothing was written."
        if fw["state"] == "ready":
            return ("The firmware server's release has no build for this panel's chip.",
                    "Choose the build yourself under Advanced tools. Nothing was written.")
        return "The Flasher is still getting the firmware from the server.", "Wait a moment. Nothing was written."

    def _rules_problem(self, image, choice):
        """Why the release the image comes from doesn't fit the parts chosen, or ""."""
        if not self.serving() or not image.origin:
            return ""
        t = next((t for t in self.source.ready() if t.bundle is not None and t.origin == image.origin), None)
        return fwserver.rules_problem(t.rules, fwserver.peripherals_of(choice)) if t is not None else ""

    # -- looking at a device --------------------------------------------------------------------
    def _with_port_free(self, port, fn):
        """Run fn() with no console holding `port`; a console that was open reopens after."""
        c = self.consoles.get(port)
        was_open = c is not None and c.is_open
        if was_open:
            c.close("Disconnected while the Flasher uses the port.")
        try:
            return fn()
        finally:
            if was_open:
                try:
                    self.console_open(port)
                except Exception:
                    pass

    def _probe_for_job(self, port):
        return probe_port(port, opener=self.opener)

    def _check_before_restart(self, port, advice, needs_console=False, then=None):
        """FLS-004, FLS2-001: the jobs' idle gate (jobs.refuse_unless_idle) on a look at the panel
        taken now, just before anything restarts it, never on what was seen earlier. The console
        lets go of the port for the look and takes it back after. Refuses a panel that is armed, in
        installer mode, on probation or waiting for an update's confirmation, one that answers
        without saying whether it is idle, and one that doesn't answer although something on this
        port looked like RoboAlarms since it was plugged in (Device.roboalarms), or needs_console:
        the request is a console command. A board that never did (a factory demo, another program)
        may still be restarted from the Console, as before.
        then(): the restart itself, run only once the panel passed, with the board still marked
        busy, so no other look or job takes the port between the check and the reset pulse."""
        if self.is_module_port(port):
            raise JobFailed(NOT_A_PANEL_PORT, NOT_A_PANEL_PORT_ADVICE)    # FLS-030: never opened unasked
        # A job's own port only: another port is another board (the Zigbee module's, the only one
        # that may sit in a panel, is refused above), as the Console opens beside a job.
        if self.runner.busy() and self.runner.current.port == port:
            raise JobFailed("A job is using this port.", "Wait for it to finish.")
        with self._lock:
            if self._gone(port):
                raise JobFailed(UNPLUGGED, advice)    # M05: no Device brought back for it
            d = self.devices.setdefault(port, Device(port))
            if d.busy:
                raise JobFailed("The Flasher is busy with this board.", "Wait a moment.")
            d.busy = "probing"
        self._device_changed(port)
        try:
            fresh = self._with_port_free(port, lambda: self._probe_for_job(port))
            if fresh.answered:
                d.probe = fresh     # what the page shows: the panel as it is now
                d.roboalarms = True
            self.log(f"restart check {port}: " + (f"idle={fresh.idle} probation={fresh.probation} "
                                                  f"confirm={fresh.confirm_required}" if fresh.answered else
                                                  f"no RoboAlarms console (known={d.roboalarms})"))
            refuse_unless_idle(fresh, advice, running=needs_console or d.roboalarms, max_age=PROBE_MAX_AGE)
            return then() if then is not None else fresh
        finally:
            d.busy = ""
            self._device_changed(port)

    def probe(self, port, alone=False):
        """Look at the panel without restarting it. A 303A:1001 port that may be the Zigbee module
        only once the person said it is a panel of its own (alone: the ESP32-S3 board, FLS-035)."""
        if self.is_module_port(port) and not alone:
            return      # FLS-030: never opened unasked (it may sit in a panel that is armed)
        d = self.devices.setdefault(port, Device(port))
        if self.runner.busy():
            self._probe_later.add(port)
            return
        if d.busy:
            return
        d.busy, d.error = "probing", None
        self._device_changed(port)

        def work():
            try:
                result = self._with_port_free(port, lambda: probe_port(port, opener=self.opener))
                d.probe = result
                d.roboalarms = d.roboalarms or result.answered
                d.error = result.error
                self._remember_native(port)
                self.log(f"probe {port}: " + (f"{result.board or '?'} {result.version} {result.profile} "
                                              f"idle={result.idle}" if result.answered else
                                              f"no RoboAlarms console (heard={result.heard})"))
            finally:
                d.busy = ""
                self._device_changed(port)
        threading.Thread(target=work, name=f"probe {port}", daemon=True).start()

    def identify(self, port, alone=False):
        """Restart the board into its loader and read its chip. A 303A:1001 port (maybe the Zigbee
        module) only once the person ticked that it is on its own (FLS-030)."""
        if self.is_module_port(port) and not alone:
            raise JobFailed(TICK_FIRST, TICK_WHY)
        d = self.devices.setdefault(port, Device(port))
        if d.busy or self.runner.busy():
            raise JobFailed("The Flasher is busy with this board.", "Wait a moment.")
        if d.probe and d.probe.busy_reason:
            raise JobFailed(d.probe.busy_reason, "Identifying restarts the board, so it waits.")
        d.busy, d.error = "identifying", None
        self._device_changed(port)

        def work():
            def run():
                # FLS-004, FLS2-001: the same gate as every restart, on a look taken now: a panel
                # that ran RoboAlarms here and has gone quiet is not put into its loader.
                fresh = probe_port(port, opener=self.opener)
                if fresh.answered:
                    d.probe = fresh
                    d.roboalarms = True
                refuse_unless_idle(fresh, "Identifying restarts the board, so it waits.",
                                   running=d.roboalarms, max_age=PROBE_MAX_AGE)
                session = self.connect(port, lambda t: self.log(f"identify {port}: {t}"), None, FAST_BAUD)
                try:
                    d.chip = session.info
                    self._remember_native(port)
                finally:
                    session.reset()
            try:
                self._with_port_free(port, run)
                self.log(f"identify {port}: {d.chip}")
            except (JobFailed, PortError) as e:
                title = e.title if isinstance(e, JobFailed) else e.message
                d.error = {"kind": "identify", "message": title, "advice": e.advice}
            except Exception as e:
                from .chip import explain
                title, advice = explain(e)
                d.error = {"kind": "identify", "message": title, "advice": advice}
                self.log(f"identify {port} failed: {e!r}")
            finally:
                d.busy = ""
                self._device_changed(port)
        threading.Thread(target=work, name=f"identify {port}", daemon=True).start()

    def radio_identify(self, port, alone=False):
        """The Zigbee module's identify (FLS-030): the same as identify, the tick required."""
        self._module_gate(port, alone)
        return self.identify(port, alone=True)

    def _module_gate(self, port, alone):
        """Refusals before the Zigbee module's port is touched (FLS-030)."""
        if self.role(port)[0] == "panel":
            raise JobFailed(NOT_THE_MODULE, NOT_THE_MODULE_ADVICE)
        if not self.is_module_port(port):
            raise JobFailed(NOT_A_MODULE_PORT, NOT_A_MODULE_PORT_ADVICE)
        if not alone:
            raise JobFailed(TICK_FIRST, TICK_WHY)

    def radio_read(self, port, alone=False):
        """What the Zigbee module runs (FLS-030), against the bundle's radio software: connect,
        require an ESP32-H2, read its partition table and app description, restart it."""
        from .jobs import read_radio
        self._module_gate(port, alone)
        d = self.devices.setdefault(port, Device(port))
        if d.busy or self.runner.busy():
            raise JobFailed("The Flasher is busy with this board.", "Wait a moment.")
        radio = self.bundle.radio("zigbee") if self.bundle is not None else None
        d.busy, d.error = "reading", None
        self._device_changed(port)
        say = lambda t: self.log(f"radio read {port}: {t}")  # noqa: E731

        def run():
            fresh = probe_port(port, opener=self.opener)
            if fresh.error:
                raise PortError(fresh.error["kind"], fresh.error["message"], fresh.error["advice"])
            if fresh.answered:
                d.probe = fresh
                raise JobFailed(NOT_THE_MODULE, NOT_THE_MODULE_ADVICE)
            session = self.connect(port, say, None, FAST_BAUD)
            try:
                d.chip = session.info
                return read_radio(session, radio)
            finally:
                session.reset()
        try:
            result = self._with_port_free(port, run)
            say(json.dumps(result))
            return result
        except (JobFailed, PortError):
            raise
        except Exception as e:
            from .chip import explain
            say(f"failed: {e!r}")
            raise JobFailed(*explain(e)) from None
        finally:
            d.busy = ""
            self._device_changed(port)

    def select_image(self, port, key):
        d = self.devices.setdefault(port, Device(port))
        if key and (self.bundle is None or self.bundle.image(key) is None):
            raise JobFailed("That image is not in the loaded bundle.", "")
        d.image_key = key
        if not key:
            d.board = ""        # back to what the Flasher finds by itself
        self._device_changed(port)

    def select_board(self, port, board):
        """The person says which board this is, when its chip fits several (FLS-005: the CrowPanel 7"
        and 10.1"): that board's image for the chip's silicon, the running profile first. From the
        firmware server, its release is downloaded if it isn't yet."""
        d = self.devices.setdefault(port, Device(port))
        info = self.catalogue.get(board)
        if info is None:
            raise JobFailed(f"The Flasher does not install RoboAlarms on {board!r}.", "")
        if not self.serving():
            rev = d.chip.revision if d.chip is not None else (d.probe.chip_revision if d.probe else None)
            if not pick_image(self.bundle, board, rev, d.probe.profile if d.probe else ""):
                raise JobFailed(f"The loaded firmware has no build for the {info['name']} on this chip.",
                                "Open a firmware file that has one (Advanced tools › Software).")
        d.board, d.image_key = board, ""
        if self.serving():
            self._firmware_for(board)
        self._device_changed(port)

    # -- jobs -----------------------------------------------------------------------------------
    def _job_changed(self, job):
        if job["state"] not in ("running", "done") and job["kind"] in ("install", "update"):
            self._parts_not_set_up(job)
        self.events.publish("job", job=job)
        if job["state"] != "running":
            port = job["port"]
            d = self.devices.get(port)
            if d is not None:
                if (job.get("result", {}).get("probe") or {}).get("answered"):
                    d.roboalarms = True     # its check found a RoboAlarms console (FLS2-001)
                result = job.get("result", {})
                if result.get("chip"):
                    from .boards import ChipInfo
                    c = result["chip"]
                    d.chip = ChipInfo(chip=c["chip"], revision=c["revision"], flash_size=c["flash_size"],
                                      mac=c["mac"], description=c.get("description", ""),
                                      crystal_mhz=c.get("crystal_mhz", 0))
                written = any(s["key"] == "write" and s["state"] == "done" for s in job.get("steps", ()))
                if job["kind"] in ("install", "update", "restore") and (job["state"] == "done" or written):
                    d.probe = None      # it runs something new (a Stop while it started too): look again
                    # FLS2-001: d.roboalarms is never cleared, and an install or update makes it
                    # known: it now runs RoboAlarms, which may be armed whether or not its console
                    # answers in time (busy, still starting, or a release image without one). What a
                    # restore wrote is unknown: what was known before stays.
                    if job["kind"] in ("install", "update"):
                        d.roboalarms = True
                    d.image_key = ""
            if port in self._reopen_after_job or (job["kind"] in ("install", "update") and job["state"] == "done"):
                self._reopen_after_job.discard(port)
                threading.Thread(target=self._after_job, args=(port, job), daemon=True).start()
            else:
                self._device_changed(port)
            # The probes skipped while it ran (a panel plugged in meanwhile): the job's own port is
            # looked at by _after_job, and a module's port never (FLS-030).
            later, self._probe_later = self._probe_later - {port}, set()
            for other in later:
                if any(p.device == other for p in self.ports):
                    self.probe(other)

    def _parts_not_set_up(self, job):
        """An install or update that did not finish (a Stop while the panel started, a failure)
        leaves the parts chosen for it unset: say so in its advice, not silently (FLS-024)."""
        choice = self._parts_after.pop(job["port"], None)
        if choice is None:
            return
        words = (f"The connected parts you chose were not set up: once the panel runs, set them up under "
                 f"{_parts_page(choice)}.")
        error = dict(job.get("error") or {"title": "", "advice": ""})
        error["advice"] = (error.get("advice", "") + " " + words).strip()
        job["error"] = error
        cur = self.runner.current
        if cur is not None and cur.id == job["id"]:
            cur.error = error       # what the page reads again later (snapshot)

    def _after_job(self, port, job):
        try:
            self.console_open(port)
        except Exception:
            pass
        if job["kind"] in ("install", "update") and job["state"] == "done":
            # The console starts just after the "running" line the job waited for.
            for delay in (1.0, 2.5):
                time.sleep(delay)
                if self.probe_via_console(port):
                    break
        self._device_changed(port)
        choice = self._parts_after.pop(port, None)
        if choice is not None and job["kind"] in ("install", "update") and job["state"] == "done":
            try:
                self.parts_apply(port, choice)
            except JobFailed as e:
                with self._lock:
                    if self._gone(port):
                        return      # M05: unplugged meanwhile, nothing is kept for it
                    self.parts_jobs[port] = self._parts_state(port, choice, "failed",
                                                              {"title": e.title, "advice": e.advice})
                self._publish_parts(port)

    def _boot_line(self, line):
        pass   # the boot log is kept in the job's result; the console reopens right after

    def start_job(self, kind, port, **opt):
        """Check the request, then free the port and start the job. The console (and the 433 MHz
        polling) is let go only once the job is sure to start, and taken back if it doesn't: a
        refused request leaves the port as it was."""
        if self.runner.busy():
            raise JobFailed("Another job is running.", "Wait for it to finish.")
        d = self.devices.get(port)
        if d is not None and d.busy:
            raise JobFailed("The Flasher is still looking at this board.", "Wait a moment.")
        if self.is_module_port(port) and opt.get("alone") is not True:
            raise JobFailed(TICK_FIRST, TICK_WHY)      # FLS-030: before anything else looks at it
        begin = self._job_to_start(kind, port, opt)
        c = self.consoles.get(port)
        reopen = c is not None and c.is_open
        polling = port in self.rf_polling
        if reopen:
            if kind == "radio":     # the module is unplugged after it: its console stays closed
                c.close("Disconnected for the job.")
            else:
                c.close("Disconnected for the job; the console reopens when it finishes.")
                self._reopen_after_job.add(port)
        self.rf_polling.discard(port)
        try:
            return begin().to_dict()
        except Exception:
            self._parts_after.pop(port, None)
            if reopen:
                self._reopen_after_job.discard(port)
                try:
                    self.console_open(port)
                except Exception:
                    pass
            if polling:
                self.rf_polling.add(port)
            raise

    def _job_to_start(self, kind, port, opt):
        """Check a job request; returns what starts it. Nothing on the port is touched here."""
        if kind in ("install", "update"):
            if self.bundle is None:
                if self.serving():
                    raise JobFailed(*self._not_ready(port))
                raise JobFailed("No firmware bundle is loaded.", "Open one with the Firmware button.")
            key = opt.get("image_key") or (self.device_dict(port)["image_key"])
            image = self.bundle.image(key) if key else None
            if image is None:
                if self.serving():
                    raise JobFailed(*self._not_ready(port))
                raise JobFailed("No image is chosen for this board.", "Choose one under Firmware.")
            v = verdict(image, self.catalogue)   # a board the Flasher does not offer: before the port is touched
            if not v.allowed:
                raise JobFailed(v.blocked, "Nothing was written.")
            # The parts to set up once it runs (FLS-024), checked before anything is written.
            choice = opt.get("parts")
            if choice is not None:
                issues = parts_mod.problems(choice, self.catalogue.get(image.board) or {})
                if issues:
                    raise JobFailed(issues[0], f"Change it under {_parts_page(choice)}. Nothing was written.")
                choice = parts_mod.normalise(choice)
            rules = self._rules_problem(image, choice)
            if rules:
                raise JobFailed(rules, "Nothing was written.")
            # The packs of the image's own release (a file's serve every image in it).
            languages, assets, voices = self.bundle.packs_for(image)

            language = None
            if kind == "install" and opt.get("language"):
                locale = str(opt["language"])
                language = self.bundle.language(locale, image)
                if language is None:
                    raise JobFailed(f"This bundle has no language {locale!r}.",
                                    "Choose English or a language the bundle lists. Nothing was written.")
                if language.draft and image.profile == "release":
                    raise JobFailed(f"The {language.name} pack is a draft for testing, and a release "
                                    "firmware refuses it.", "Choose English or another language. "
                                    "Nothing was written.")

            def begin():
                if choice is not None:
                    self._parts_after[port] = choice
                else:
                    self._parts_after.pop(port, None)
                self.parts_jobs.pop(port, None)
                if kind == "install":
                    # A factory reset (FLS-025) is an install over RoboAlarms: the whole chip erased.
                    reset = bool(opt.get("reset"))
                    return self.runner.install(port, image, backup=bool(opt.get("backup", True)),
                                               erase_settings_ok=bool(opt.get("erase_settings_ok")),
                                               confirmed=bool(opt.get("confirmed")),
                                               title="Reset to factory defaults" if reset else None,
                                               language=language, assets=assets,
                                               # ADR-053: the language's own voice, English's by default
                                               voice=bundle_mod.pick_voice(voices,
                                                                           language.locale if language else "",
                                                                           image.profile))
                return self.runner.update(port, image, confirmed=bool(opt.get("confirmed")),
                                          downgrade_ok=bool(opt.get("downgrade_ok")),
                                          languages=languages, assets=assets, voices=voices)
            return begin
        if kind == "backup":
            board = self.device_dict(port)["detection"]["board"]
            return lambda: self.runner.backup(port, board)
        if kind == "radio":
            # The Zigbee module's radio software (FLS-030): out of its socket, the tick given.
            self._module_gate(port, opt.get("alone") is True)
            radio = self.bundle.radio("zigbee") if self.bundle is not None else None
            if radio is None:
                if self.serving():
                    raise JobFailed("The Zigbee module's software comes with the panel's firmware, which isn't "
                                    "downloaded yet.", "Choose your panel in step 1, or download its firmware "
                                    "under Advanced tools > Software. Nothing was changed.")
                raise JobFailed("The loaded bundle has no radio software for the Zigbee module.",
                                "Open a bundle that has it with the Firmware button. Nothing was changed.")
            return lambda: self.runner.radio(port, radio, backup=opt.get("backup") is True)
        if kind == "restore":
            path = Path(str(opt.get("path", "")))
            if path.parent.resolve() != self.backups_dir.resolve() or path.suffix != ".bin":
                raise JobFailed("Only backups in the Flasher's Backups folder can be restored.", "")
            return lambda: self.runner.restore(port, path, mac_ok=bool(opt.get("mac_ok")))
        raise JobFailed(f"Unknown job {kind!r}.", "")

    def cancel_job(self, job_id):
        return self.runner.cancel(job_id)

    def list_backups(self):
        out = []
        if self.backups_dir.is_dir():
            for p in sorted(self.backups_dir.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True):
                try:
                    side = json.loads(p.with_suffix(".json").read_text())
                except (OSError, ValueError):
                    side = {}
                out.append({"path": str(p), "name": p.name, "size": p.stat().st_size,
                            "created": side.get("created", ""), "chip": side.get("chip", ""),
                            "mac": side.get("mac", ""), "board": side.get("board", "")})
        return out

    # -- console --------------------------------------------------------------------------------
    def _console_lines(self, port, lines):
        self.events.publish("console", port=port, lines=[l.to_dict() for l in lines])

    def _console_closed(self, session):
        self._device_changed(session.port)

    def console_open(self, port):
        if self.is_module_port(port):
            raise JobFailed(NOT_A_PANEL_PORT, NOT_A_PANEL_PORT_ADVICE)    # FLS-030: never opened unasked
        if self.runner.busy() and self.runner.current.port == port:
            raise JobFailed("A job is using this port.", "The console reopens when it finishes.")
        c = self.consoles.get(port)
        if c is not None and c.is_open:
            return
        old = list(c.history) if c else []

        def roboalarms():
            # FLS2-001: its prompt or a DEV INFO answer, even one the probe's 2.5 s missed (a busy
            # panel) or from a build without `dev`: from now on a restart needs its answer. Not
            # for a console that is no longer the port's (PERF23).
            with self._lock:
                d = self.devices.get(port)
                if d is not None and self.consoles.get(port) is c:
                    d.roboalarms = True
        c = ConsoleSession(port, lambda lines: self._console_lines(port, lines), self._console_closed,
                           opener=self.opener, on_roboalarms=roboalarms)
        c.history.extend(old)
        with self._lock:
            if self._gone(port):
                raise JobFailed(UNPLUGGED, PLUG_IN_AGAIN)    # M05: a late request brings nothing back
            # M05: the port's monitor is kept only once its console opened on a port still listed
            monitor = self.rf.get(port) or RfMonitor()
            before = self.consoles.get(port)
            self.consoles[port] = c     # before it opens: its first lines are the port's already (PERF23)

        def to_rf(line):
            if monitor.feed(line.text):
                self._publish_rf_soon(port, c)
        c.listeners.append(to_rf)
        try:
            c.open()
        except Exception:
            with self._lock:
                if self.consoles.get(port) is c:
                    if before is None:
                        self.consoles.pop(port, None)
                    else:
                        self.consoles[port] = before
            raise
        with self._lock:
            live = self.consoles.get(port) is c and not self._gone(port)
            if live:
                self.rf.setdefault(port, monitor)
        if not live:        # unplugged (or replaced) while it opened
            c.close()
            raise JobFailed(UNPLUGGED, PLUG_IN_AGAIN)
        self._device_changed(port)

    def console_close(self, port):
        c = self.consoles.get(port)
        if c:
            self._raw_off([(port, c)])      # RF23: raw logging off before the console lets go
            c.close("Disconnected.")
        self.rf_polling.discard(port)
        self._device_changed(port)

    def _raw_off(self, consoles, budget=1.0):
        """RF23 (FLS-014): `rf raw off` to each open console whose panel logs every transmission,
        then wait for their answers: best effort, never longer than `budget` seconds in all, even
        for a panel that doesn't answer or a write that hangs (each send runs on its own thread)."""
        waits = []
        for port, c in consoles:
            m = self.rf.get(port)
            if m is None or not m.raw_on or not c.is_open:
                continue
            answered = threading.Event()

            def listen(line, answered=answered):
                if RAW_OFF_REPLY.match(line.text.strip()):
                    answered.set()

            def send(c=c):
                try:
                    c.send("rf raw off", quiet=True, quiet_match=lambda t: t.startswith("raw "))
                except Exception:
                    answered.set()      # nothing to wait for
            c.listeners.append(listen)
            threading.Thread(target=send, name=f"raw off {port}", daemon=True).start()
            waits.append((c, listen, answered))
        deadline = time.monotonic() + budget
        for c, listen, answered in waits:
            answered.wait(max(0.0, deadline - time.monotonic()))
            try:
                c.listeners.remove(listen)
            except ValueError:
                pass

    def _console(self, port):
        c = self.consoles.get(port)
        if c is None or not c.is_open:
            self.console_open(port)
            c = self.consoles[port]
        return c

    def console_send(self, port, text):
        text = str(text).strip()
        if not text:
            return
        if len(text) > 200 or any(ord(ch) < 32 for ch in text):
            raise JobFailed("That command is too long or has control characters.", "")
        self._console(port).send(text)

    def console_restart(self, port):
        d = self.devices.get(port)
        role, maybe = self.role(port)
        if role == "zigbee_module" or maybe == "zigbee_module" or (d and d.chip and d.chip.chip == MODULE_CHIP):
            # FLS-030: its reset line restarts a panel it may sit in, and the Flasher can't tell.
            raise JobFailed("The Console doesn't restart the Zigbee module.",
                            "Unplug its cable and plug it in again to restart it.")
        self._check_before_restart(port, "The panel was not restarted.",
                                   then=lambda: self._console(port).restart_panel())

    def console_save(self, port):
        c = self.consoles.get(port)
        if c is None:
            raise JobFailed("There is no console log for this port yet.", "")
        self.logs_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        safe = "".join(ch if ch.isalnum() else "_" for ch in port)
        return str(c.save(self.logs_dir / f"console-{safe}-{stamp}.txt"))

    def probe_via_console(self, port, timeout=3.0):
        """Ask a running panel what it is through an open console (after an install)."""
        from .probe import DEV_INFO, ProbeResult, parse_describe
        c = self.consoles.get(port)
        if c is None or not c.is_open:
            return
        got = threading.Event()
        result = ProbeResult(port=port, at=time.time())

        def listen(line):
            m = DEV_INFO.search(line.text)
            if m:
                try:
                    parse_describe(result, m.group(1))
                    line.quiet = True
                    got.set()
                except ValueError:
                    pass
        c.listeners.append(listen)
        try:
            c.send("dev describe", quiet=True, quiet_match=lambda t: t.startswith("DEV INFO"), quiet_for=timeout)
            got.wait(timeout)
        except Exception:
            pass
        finally:
            c.listeners.remove(listen)
        d = self.devices.setdefault(port, Device(port))
        if got.is_set():
            d.probe = result
            d.roboalarms = True
        self._device_changed(port)
        return got.is_set()

    # -- 433 MHz --------------------------------------------------------------------------------
    def _publish_rf_soon(self, port, session=None):
        """One update for a burst of lines (`rf` prints six at once). A late line from a console
        that is no longer the port's (its board unplugged, or the console replaced) starts nothing
        and publishes nothing (PERF23)."""
        with self._lock:
            if session is not None and self.consoles.get(port) is not session:
                return
            if port in self._rf_pending:
                return
            self._rf_pending.add(port)

        def flush():
            with self._lock:
                self._rf_pending.discard(port)
                live = session is None or self.consoles.get(port) is session
            m = self.rf.get(port)
            if m is not None and live:
                self.events.publish("rf", port=port, rf=self._rf_dict(port, m))
        threading.Timer(0.25, flush).start()

    def _rf_dict(self, port, m):
        """What the page shows of a port's receiver; `recording` while a Record asked for there
        is in flight (RF21: the page's Record waits for it)."""
        d = m.to_dict()
        d["recording"] = port in self._captures
        return d

    def _monitor(self, port):
        """The port's receiver monitor, its console opened first; refused for an unplugged port (M05)."""
        self._console(port)
        m = self.rf.get(port)
        if m is None:
            raise JobFailed(UNPLUGGED, PLUG_IN_AGAIN)
        return m

    def rf_start(self, port):
        self._monitor(port)
        with self._lock:
            if self._gone(port):    # unplugged since _monitor(): no stale entry
                raise JobFailed(UNPLUGGED, PLUG_IN_AGAIN)
            self.rf_polling.add(port)
        self._rf_poll_one(port)
        self._device_changed(port)

    def rf_stop(self, port):
        if port in self.rf_polling:
            self.rf_polling.discard(port)
            m = self.rf.get(port)
            c = self.consoles.get(port)
            if m and m.raw_on and c and c.is_open:
                try:
                    c.send("rf raw off", quiet=True, quiet_match=lambda t: t.startswith("raw "))
                except Exception:
                    pass
        self._device_changed(port)

    def rf_listen(self, port, on):
        m = self._monitor(port)
        if on:
            m.reset_heard()
        self._console(port).send("rf raw on" if on else "rf raw off")
        self.events.publish("rf", port=port, rf=self._rf_dict(port, m))

    def _ask(self, c, text, done, timeout=3.0):
        """Send a visible command and wait for the reply line done(line) accepts, or None."""
        got = []
        event = threading.Event()

        def listen(line):
            if not got and done(line.text.strip()):
                got.append(line.text.strip())
                event.set()
        c.listeners.append(listen)
        try:
            c.send(text)
            event.wait(timeout)
        finally:
            c.listeners.remove(listen)
        return got[0] if got else None

    def _ask_lines(self, c, text, keep, done, timeout=3.0):
        """Send a visible command; collect the reply lines keep(line) accepts until done(line)
        accepts one (it is kept too). Returns (lines, when the last arrived) or ([], None)."""
        got = []
        at = []
        event = threading.Event()

        def listen(line):
            t = line.text.strip()
            if event.is_set() or not keep(t):
                return
            got.append(t)
            if done(t):
                at.append(time.time())
                event.set()
        c.listeners.append(listen)
        try:
            c.send(text)
            event.wait(timeout)
        finally:
            c.listeners.remove(listen)
        return (got, at[0]) if at else ([], None)

    # -- connected parts (FLS-024) --------------------------------------------------------------
    def parts_read(self, port, probe_custom=True):
        """What the running panel is set up for, from `rf`, `rtc` and `zwave status` (Connected
        parts' starting point for a panel that already runs RoboAlarms), and whether it takes
        custom places (`custom_ok`, FLS-029; not asked with probe_custom=False)."""
        c = self._console(port)
        rf_done = lambda t: t.startswith(("module=", "433 MHz receiver: not", "Unrecognized command"))  # noqa: E731
        rf_lines, at = self._ask_lines(c, "rf", lambda t: True, rf_done)
        if at is None:
            raise JobFailed("The panel did not answer.", "Check the Console, then try again.")
        rtc_lines, _ = self._ask_lines(c, "rtc", lambda t: True, clock_test.is_reply_end)
        board = self.device_dict(port).get("board_info") or {}
        zw_lines = self._zwave_lines(c) if board.get("zwave") is not None else []
        # Whether it takes custom places (FLS-029): the Custom pins tab offers Apply, or Update.
        custom_ok = False
        if probe_custom and board.get("custom"):
            try:
                custom_ok = self._offers_custom(c)
            except JobFailed as e:
                self.log(f"parts read {port}: {e}")
        got = parts_mod.read_current(rf_lines, rtc_lines, board, zw_lines)
        if got.get("layout") == "custom":
            if not self._gone(port):    # M05
                self.custom_seen.add(port)
        elif probe_custom:
            self.custom_seen.discard(port)
        return {"parts": got, "custom_ok": custom_ok}

    def _offers_custom(self, c, timeout=4.0, grace=0.6):
        """The probe (FLS-029): does `rf place` list a "custom: " place? Read to the listing's end
        (and a short grace after it, wherever the "custom: " line sits), so no line of it is taken
        for the next command's reply. False without one (older firmware); raises when nothing
        answers."""
        state = {"custom": False, "reply": False, "end": None}
        event = threading.Event()

        def listen(line):
            kind = parts_mod.probe_line(line.text.strip())
            if kind == "custom":
                state["custom"] = True
                state["reply"] = True
                if state["end"] is not None:
                    event.set()
            elif kind == "end":
                if state["custom"]:
                    event.set()
                state["end"] = state["end"] or time.monotonic()
                state["reply"] = True
            elif kind == "reply":
                state["reply"] = True
        c.listeners.append(listen)
        try:
            c.send(parts_mod.PROBE)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and not event.is_set():
                if state["end"] is not None and time.monotonic() - state["end"] >= grace:
                    break
                event.wait(0.05)
        finally:
            c.listeners.remove(listen)
        if not state["custom"] and not state["reply"]:
            raise JobFailed("The panel did not answer.", f"It stopped at `{parts_mod.PROBE}`. Nothing was "
                            "changed. Check the Console, then try again.")
        return state["custom"]

    def parts_zones(self, port, choice):
        """Each zone wired to a pin the custom layout takes (FLS-029): [{"zone", "name", "gpio"}],
        from `zones` and `terminals`. Only named in the confirmation: the hardware wins, so it never
        blocks, and older firmware or no zones give []."""
        if not parts_mod.custom_gpios(choice):
            return {"zones": []}
        try:
            c = self._console(port)
            got = []
            event = threading.Event()

            def listen(line):
                t = line.text.strip("\r\n")
                got.append(t)
                if t.startswith(("an open loop reads open", "no wired terminals", "Unrecognized command")):
                    event.set()
            c.listeners.append(listen)
            try:
                c.send("zones")      # its lines have no end of their own: `terminals` follows it
                c.send("terminals")
                event.wait(6.0)
            finally:
                c.listeners.remove(listen)
            # A zone line and a terminal line can't be taken for each other: both replies are read whole.
            return {"zones": parts_mod.zones_on(choice, got, got)}
        except Exception as e:   # never a block: the confirmation goes without names
            self.log(f"parts zones {port}: {e}")
            return {"zones": []}

    def _zwave_lines(self, c):
        """`zwave status`, up to its module line (running) or its place hint (off)."""
        done = lambda t: t.startswith(("module:", "`zwave place` lists", "Unrecognized command"))  # noqa: E731
        lines, _ = self._ask_lines(c, "zwave status", lambda t: True, done)
        return lines

    def _zigbee_once(self, c, timeout=3.0):
        """One `zigbee status`: parse_zigbee_status() of its reply, or None when nothing answered.
        Its last line is the fields (or "Unrecognized command"); with Zigbee off only its words
        line comes, so the reply is read until the timeout then."""
        got = []
        event = threading.Event()

        def listen(line):
            t = line.text.strip()
            if event.is_set() or not t or t.startswith(("alarm>", "zigbee status")):
                return
            got.append(t)
            if ZIGBEE_STATUS.match(t) or t.startswith("Unrecognized command"):
                event.set()
        c.listeners.append(listen)
        try:
            c.send("zigbee status")
            event.wait(timeout)
        finally:
            c.listeners.remove(listen)
        return parse_zigbee_status(got)

    def zigbee_status(self, port, timeout=20.0, every=2.0):
        """The panel's Zigbee radio (FLS-030), from `zigbee status` on its console; asked again
        for up to `timeout` s while the radio is still starting (ready=0) after a restart."""
        c = self._console(port)
        deadline = time.monotonic() + timeout
        while True:
            got = self._zigbee_once(c)
            if got is None:
                # A panel with Zigbee off answers its words line alone: nothing to wait for.
                raise JobFailed("The panel did not answer.", "Check the Console, then try again.")
            if not got.get("known") or not got.get("enabled") or got.get("ready") \
                    or time.monotonic() + every > deadline:
                return got
            time.sleep(every)

    def setup_label(self, port, timeout=4.0):
        """The setup Wi-Fi's label of a board without a screen (FLS-035), from `web setup` on its
        console: read-only, it restarts nothing and types no code. The label (the setup Wi-Fi's name,
        its password - the same on every board - and the address) goes to the page, never to the
        Flasher's log."""
        c = self._console(port)
        got = []
        event = threading.Event()

        def listen(line):
            t = line.text.strip()
            if event.is_set() or not t or t.startswith(("alarm>", "web setup")):
                return
            got.append(t)
            if t.startswith(("SETUP label: certificate", "SETUP: ", "Unrecognized command", "WEB enabled=")):
                event.set()
        c.listeners.append(listen)
        try:
            c.send("web setup")
            event.wait(timeout)
        finally:
            c.listeners.remove(listen)
        label = parse_setup_label(got)
        if label is None:
            raise JobFailed("The panel did not answer.",
                            "A release build has no service console: look for its setup Wi-Fi on your phone.")
        self.log(f"setup label {port}: " + (f"stage {label.get('stage', '?')}, setup Wi-Fi "
                                            f"{'open' if label.get('open') else 'closed'}" if label.get("ssid")
                                            else json.dumps(label)))
        return label

    # Setup over USB (FLS-036, web_setup_usb.c): the installer's access and the home Wi-Fi, typed
    # in the Flasher instead of on a phone. A development build takes `web provision ...` on its
    # console, a release build `provision ...` on its USB port; each answers one PROVISION line.
    PROVISION_STEPS = {"accounts": 2, "network": 2, "finish": 0, "cancel": 0, "status": 0}

    @staticmethod
    def _quote(arg):
        return '"' + str(arg).replace("\\", "\\\\").replace('"', '\\"') + '"'

    def setup_provision(self, port, step, args=(), timeout=20.0):
        """One step of setup over USB: {"ok": True, ...} (key, ip, stage, wifi) or JobFailed with
        the board's word. The passwords and the recovery key never reach the log or the console's
        history."""
        if step not in self.PROVISION_STEPS or len(args) != self.PROVISION_STEPS[step]:
            raise JobFailed("That isn't a setup step.", "")
        if any(len(str(a)) > 64 or any(ord(ch) < 32 for ch in str(a)) for a in args):
            raise JobFailed("A name or password is too long or has control characters.", "")
        c = self._console(port)
        got = []
        event = threading.Event()

        def listen(line):
            t = line.text.strip()
            if not event.is_set() and t.startswith("PROVISION "):
                got.append(t)
                event.set()
        command = " ".join(["provision", step] + [self._quote(a) for a in args])
        secrets = [str(a) for a in args[1:2]] if step in ("accounts", "network") else []
        c.listeners.append(listen)
        try:
            for text in ("web " + command, command):   # development build, then release build
                c.send(text, quiet=True, quiet_match=lambda t: t.startswith("PROVISION "), quiet_for=timeout)
                if event.wait(timeout if text == command else min(timeout, 4.0)):
                    break
        finally:
            c.listeners.remove(listen)
            answer = got[0] if got else ""
            if answer.startswith("PROVISION ok key "):
                secrets.append(answer[len("PROVISION ok key "):])
            c.scrub(secrets + [command])
        if not got:
            raise JobFailed("The board did not answer.", "Check it is plugged in, then try again.")
        words = got[0].split()
        self.log(f"setup over USB {port}: {step} -> {words[1] if len(words) > 1 else '?'}")
        if words[1] == "error":
            raise JobFailed(PROVISION_WORDS.get(words[2] if len(words) > 2 else "", "The board refused it."),
                            words[2] if len(words) > 2 else "")
        if words[1] == "status":
            return {"ok": True, "stage": words[2], "wifi": words[3], "ip": "" if words[4] == "-" else words[4]}
        out = {"ok": True}
        if step == "accounts" and len(words) >= 4 and words[2] == "key":
            out["key"] = words[3]
        if step == "finish" and len(words) >= 3:
            out["ip"] = words[2]
        return out

    def _zwave_settled(self, c, timeout=20.0):
        """Wait while the Z-Wave radio is still starting after a restart (a few seconds)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not any(t.startswith("radio: starting") for t in self._zwave_lines(c)):
                return
            time.sleep(2.0)

    def parts_check(self, port, choice):
        """What is wrong with a choice for this panel's board, in words; [] when fine (FLS-029)."""
        return parts_mod.parts_check(choice, self.device_dict(port).get("board_info") or {})

    def parts_apply(self, port, choice):
        """Set the panel up for the parts wired to it: each setting stored, one restart, then
        read back (FLS-024). Runs on a thread; the page follows it through "parts" events. The
        panel is looked at before the first setting and again before the restart (FLS2-001)."""
        board = self.device_dict(port).get("board_info") or {}
        issues = parts_mod.problems(choice, board)
        if issues:
            raise JobFailed(issues[0], f"Change it under {_parts_page(choice)}.")
        d = self.devices.get(port)
        if d is not None and d.probe is not None and d.probe.busy_reason:
            raise JobFailed(d.probe.busy_reason, "Setting up the parts needs a restart.")
        if self.runner.busy():
            raise JobFailed("A job is running.", "Wait for it to finish.")
        with self._lock:
            if self._gone(port):
                raise JobFailed(UNPLUGGED, PLUG_IN_AGAIN)    # M05
            old = self.parts_jobs.get(port)
            if old is not None and old["state"] == "running":
                raise JobFailed("The parts are being set up already.", "")
            st = self._parts_state(port, choice)
            self.parts_jobs[port] = st
        threading.Thread(target=self._parts_work, args=(port, st, board), name=f"parts {port}",
                         daemon=True).start()
        self._publish_parts(port)
        return {"parts_job": st}

    def _parts_work(self, port, st, board):
        def step(key, state, detail=None):
            for s in st["steps"]:
                if s["key"] == key:
                    s["state"] = state
                    if detail is not None:
                        s["detail"] = detail
            self._publish_parts(port)
        try:
            step("send", "running")
            # FLS-004, FLS2-001: the panel as it is now, before any setting is stored.
            self._check_before_restart(port, "Setting up the parts needs a restart. Nothing was changed.",
                                       needs_console=True)
            c = self._console(port)
            cmds = parts_mod.commands(st["choice"], board, from_custom=port in self.custom_seen)
            for cmd in cmds:
                if parts_mod.is_probe(cmd):   # FLS-029: nothing is changed before this
                    if not self._offers_custom(c):
                        raise JobFailed(*parts_mod.TOO_OLD)
                    continue
                reply = self._ask(c, cmd, lambda t, cmd=cmd: parts_mod.reply_kind(cmd, t) is not None,
                                  timeout=4.0)
                if reply and parts_mod.custom_refused(cmd, reply):
                    # The receiver is on custom pins, which `rf module` can't change: put it back first.
                    back = self._ask(c, "rf place default",
                                     lambda t: parts_mod.reply_kind("rf place default", t) is not None, timeout=4.0)
                    if back and parts_mod.reply_kind("rf place default", back) == "ok":
                        reply = self._ask(c, cmd, lambda t, cmd=cmd: parts_mod.reply_kind(cmd, t) is not None,
                                          timeout=4.0)
                kind = parts_mod.reply_kind(cmd, reply) if reply else None
                if kind == "ok" or (kind == "old" and parts_mod.optional(cmd)):
                    continue
                if kind == "old":
                    raise JobFailed("This panel's firmware can't set up these parts.",
                                    "Update it first, then set them up again.")
                if reply is None:
                    raise JobFailed("The panel did not answer.",
                                    f"It stopped at `{cmd}`. Check the Console, then try again.")
                why = re.sub(r"^not (?:GPIO )?\S+: ", "", reply).rstrip(".")   # "not j7: zone 3 uses ..."
                raise JobFailed(why[0].upper() + why[1:] + ".",
                                f"The panel refused `{cmd}`; nothing after it was sent.")
            step("send", "done", f"{sum(not parts_mod.is_probe(x) for x in cmds)} settings stored")
            step("restart", "running", "the panel starts again with them")
            # Again just before the restart: it may have been armed while the settings went (FLS2-001).
            c = self._check_before_restart(port, "The settings are stored but the panel was not restarted: they "
                                                 "take effect when it next starts. Set the parts up again once "
                                                 "it is idle.", needs_console=True,
                                           then=lambda: self._restart_for_rf(port))
            time.sleep(3.0)
            deadline = time.monotonic() + 25.0
            back = False
            while time.monotonic() < deadline:
                if self.probe_via_console(port, timeout=2.0):
                    back = True
                    break
                time.sleep(1.0)
            if not back:
                raise JobFailed("The panel did not come back after its restart.",
                                "Look at its screen, then check the Console.")
            step("restart", "done", "running again")
            step("check", "running")
            if st["choice"].get("zwave") and board.get("zwave") is not None:
                self._zwave_settled(c)
            got = self.parts_read(port, probe_custom=False)["parts"]
            st["found"] = {"rf": got.get("rf_found", False), "clock": got.get("clock_found", False),
                           "zwave": got.get("zwave_found", False)}
            st["warnings"] = parts_mod.differences(st["choice"], got, board)
            step("check", "warning" if st["warnings"] else "done",
                 st["warnings"][0] if st["warnings"] else "the panel reports what was chosen")
            st["state"] = "done"
            self.log(f"parts {port}: {'; '.join(cmds)}" + (f" ({'; '.join(st['warnings'])})" if st["warnings"] else ""))
        except (JobFailed, PortError) as e:
            title = e.title if isinstance(e, JobFailed) else e.message
            for s in st["steps"]:
                if s["state"] == "running":
                    s["state"] = "failed"
            st["state"] = "failed"
            st["error"] = {"title": title, "advice": e.advice}
            self.log(f"parts {port} failed: {title}")
        except Exception as e:   # the console went away: an unplug
            for s in st["steps"]:
                if s["state"] == "running":
                    s["state"] = "failed"
            st["state"] = "failed"
            st["error"] = {"title": "The panel stopped answering.", "advice": f"{e}. Plug it in again, then retry."}
            self.log(f"parts {port} failed: {e!r}")
        finally:
            self._publish_parts(port)
            self._device_changed(port)
            with self._lock:
                if self._gone(port) and self.parts_jobs.get(port) is st:
                    del self.parts_jobs[port]       # M05: it ended after its port went

    # -- the clock module (FLS-022, docs/features/35) ------------------------------------------------
    def clock_status(self, port):
        """`rtc`: the clock module's state, and how far the panel's clock is from this computer's."""
        c = self._console(port)
        keep = lambda t: clock_test.is_status_line(t) or clock_test.is_reply_end(t)  # noqa: E731
        lines, at = self._ask_lines(c, "rtc", keep, clock_test.is_reply_end)
        if at is None:
            raise JobFailed("The panel did not answer.", "Look at Advanced tools › Console, then try again.")
        status = clock_test.parse_status(lines, at)
        self.log(f"rtc {port}: {status.get('words') or status.get('reason')}")
        return {"clock": status}

    def clock_set(self, port):
        """`rtc set <this computer's time>`, sent on this computer's second tick so the panel is
        set to within the serial line's delay (a few ms). Only a panel with a clock module that
        answers takes it: without one the time would be gone at the next power cut."""
        c = self._console(port)
        now = time.time()
        time.sleep(1.0 - (now % 1.0))
        target = int(round(time.time()))
        reply = self._ask(c, f"rtc set {target}", lambda t: clock_test.set_reply(t) is not None)
        if reply is None:
            raise JobFailed("The panel did not answer.", "Look at Advanced tools › Console, then try again.")
        ok, words = clock_test.set_reply(reply)
        if not ok:
            raise JobFailed(words[0].upper() + words[1:] + ".",
                            "Update the panel's firmware first." if "firmware" in words else "")
        self.log(f"rtc set {port}: {reply}")
        return {"reply": words, "epoch": target}

    def rf_set_pin(self, port, gpio):
        """`rf pin <gpio>|default`, then restart the panel so it takes effect (RF-008)."""
        self._check_before_restart(port, "Changing the pin needs a restart. Nothing was changed.", needs_console=True)
        c = self._console(port)
        text = "rf pin default" if gpio is None else f"rf pin {int(gpio)}"
        reply = self._ask(c, text, lambda t: t.startswith(("data on GPIO", "not GPIO", "could not store",
                                                           "rf [status", "rf pin <gpio>")))
        if reply is None:
            raise JobFailed("The panel did not answer.", "Look at Advanced tools › Console, then try again.")
        if reply.startswith("rf [status"):
            raise JobFailed("This panel's firmware cannot move the receiver's pin.",
                            "Update the panel's firmware first, then choose the pin again.")
        if not reply.startswith("data on GPIO"):
            raise JobFailed(reply[0].upper() + reply[1:] + ".", "")
        self.log(f"rf pin {port}: {reply}")
        # FLS2-001: looked at again just before the restart (the reply may have taken seconds).
        self._check_before_restart(port, "The pin is stored but the panel was not restarted: it takes effect "
                                         "when it next starts.", needs_console=True,
                                   then=lambda: self._restart_for_rf(port))
        return {"reply": reply}

    def rf_set_module(self, port, mode):
        """`rf module auto|cc1101|ook|off`, then restart the panel so it takes effect (RF-001,
        RF-020): which receiver the panel listens with - found by itself, only a CC1101 on the SPI
        pins, only a receiver module on the data pin, or none (no pin touched)."""
        if mode not in ("auto", "cc1101", "ook", "off"):
            raise JobFailed("Unknown receiver choice.", "")
        self._check_before_restart(port, "Changing the receiver needs a restart. Nothing was changed.",
                                   needs_console=True)
        c = self._console(port)
        reply = self._ask(c, f"rf module {mode}", lambda t: t.startswith(("receiver ", "rf module auto|", "rf [status")))
        if reply is None:
            raise JobFailed("The panel did not answer.", "Look at Advanced tools › Console, then try again.")
        if not reply.startswith("receiver "):
            raise JobFailed("This panel's firmware cannot change the receiver.", "Install the newer firmware first.")
        self.log(f"rf module {port}: {reply}")
        # FLS2-001: looked at again just before the restart (the reply may have taken seconds).
        self._check_before_restart(port, "The receiver choice is stored but the panel was not restarted: it "
                                         "takes effect when it next starts.", needs_console=True,
                                   then=lambda: self._restart_for_rf(port))
        return {"reply": reply}

    def _restart_for_rf(self, port):
        """The reset pulse after a receiver or parts setting; what the 433 MHz page showed is
        forgotten (it starts again with them). Returns the console."""
        c = self._console(port)
        m = self.rf.get(port)
        if m is not None:
            m.status = {}
            m.raw_on = False
        c.restart_panel()
        return c

    def rf_clear(self, port):
        m = self.rf.get(port)
        if m is None:       # nothing heard there (or unplugged: nothing is brought back, M05)
            return
        m.reset_heard()
        self.events.publish("rf", port=port, rf=self._rf_dict(port, m))

    def rf_capture(self, port, ms=3000):
        ms = max(500, min(10000, int(ms)))
        m = self._monitor(port)
        c = self._console(port)
        # RF21: one recording at a time on a port, and the request owns the one it asked for: the
        # first the panel starts after the command (its number, taken before the command goes
        # out: the "capturing" line can come back before send() returns). It is "the next one the
        # panel starts", not proof it is this request's: a late start from an earlier request, or a
        # `rf capture` typed at the console, can still be taken for it.
        with self._lock:
            if port in self._captures:
                raise JobFailed("A recording is already running on this port.", "Wait for it to finish.")
            n = next(self._capture_count)
            self._captures[port] = n
            first = m.capture_seq + 1
        try:
            c.send(f"rf capture {ms}")
        except Exception:
            self._capture_over(port, n)
            raise
        self.events.publish("rf", port=port, rf=self._rf_dict(port, m))
        threading.Thread(target=self._save_capture, args=(port, m, n, first, ms), name=f"capture {port}",
                         daemon=True).start()
        return {"recording": n}

    def _capture_over(self, port, n):
        with self._lock:
            if self._captures.get(port) == n:
                del self._captures[port]

    def _save_capture(self, port, m, n, first, ms):
        """Wait for recording number `first` and save it, only whole (RF22): finished without an
        error and every pulse the panel announced received. Else nothing is saved and the result
        says why. Each file is new: the port and the request's number in its name, created
        exclusively (RF21). An unplugged port's recording is dropped with it (M05)."""
        deadline = time.monotonic() + ms / 1000 + self.capture_grace
        cap = None
        try:
            while True:
                if self.rf.get(port) is not m:
                    self.log(f"rf capture {port}: the board was unplugged")
                    return
                if cap is None:
                    cap = m.capture_by_seq(first)
                if (cap is not None and cap["done"]) or time.monotonic() >= deadline:
                    break
                if self._stop.wait(0.05):
                    return
            problem = m.capture_problem(cap)
            if problem:
                self.log(f"rf capture {port} not saved: {problem}")
                self._capture_over(port, n)
                self.events.publish("rf", port=port, rf=self._rf_dict(port, m), capture_error=problem)
                return
            d = self.devices.get(port)
            header = {"when": datetime.datetime.now().isoformat(timespec="seconds"), "port": port,
                      "board": (d.probe.board if d and d.probe else ""),
                      "firmware": (d.probe.version if d and d.probe else ""),
                      "receiver": m.status.get("words", "")}
            try:
                path = self._write_capture(port, n, m.capture_text(header, cap))
            except OSError as e:
                self.log(f"rf capture {port} not saved: {e}")
                self._capture_over(port, n)
                self.events.publish("rf", port=port, rf=self._rf_dict(port, m),
                                    capture_error=f"The recording could not be saved: {e.strerror or e}.")
                return
            self.log(f"rf capture saved {path}")
            self._capture_over(port, n)
            self.events.publish("rf", port=port, rf=self._rf_dict(port, m), saved=str(path))
        finally:
            self._capture_over(port, n)

    def _write_capture(self, port, n, text):
        self.captures_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        safe = "".join(ch if ch.isalnum() else "_" for ch in port)
        for extra in range(100):
            path = self.captures_dir / f"rf-capture-{safe}-{stamp}-{n}{f'-{extra}' if extra else ''}.txt"
            try:
                with open(path, "x", encoding="utf-8") as f:     # never over an earlier recording
                    f.write(text)
                return path
            except FileExistsError:
                continue
        raise FileExistsError(f"no free name for {port}'s recording")

    def _rf_poll_one(self, port):
        c = self.consoles.get(port)
        if c is None or not c.is_open or self.runner.busy():
            return
        try:
            c.send("rf", quiet=True, quiet_match=is_status_line, quiet_for=1.5)
        except Exception:
            self.rf_polling.discard(port)

    def _rf_loop(self):
        while not self._stop.wait(3.0):
            for port in list(self.rf_polling):
                self._rf_poll_one(port)

    # -- folders --------------------------------------------------------------------------------
    def open_folder(self, which):
        target = {"backups": self.backups_dir, "captures": self.captures_dir, "logs": self.logs_dir}.get(which)
        if target is None:
            raise JobFailed("Unknown folder.", "")
        paths.open_folder(target)


def bundle_from_builds(build_dirs, catalogue=None):
    """A bundle straight from build folders, without writing a file (the --build option)."""
    from .make_bundle import image_from_build, _git_source
    images = [image_from_build(d, catalogue) for d in build_dirs]
    for image in images:
        for part in image.parts:
            part.path = f"{image.key}/{part.role}.bin"
            part.sha256 = hashlib.sha256(part.data).hexdigest()
    keys = {i.signing_key_sha256 for i in images}
    return bundle_mod.Bundle(name="build folders", created=datetime.datetime.now().isoformat(timespec="seconds"),
                             source=_git_source(), signing_key_sha256=keys.pop() if len(keys) == 1 else "",
                             images=images)
