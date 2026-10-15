"""Look at a panel without restarting it (FLS-002, FLS-003).

The port opens with DTR and RTS released, then the Flasher asks the service console:
  `dev describe`  -> `DEV INFO {json}`: board, version, profile, silicon, idle, probation
                     (the P4's legacy and dev builds);
  `status`/`ota`  -> partition states and the firmware version, for a build without `dev`
                     (the E32R40T today).
Nothing answering means no RoboAlarms firmware with a console: a factory demo, other firmware,
or a release build (no console by design, docs/features/31). Only read-only commands are sent.
"""
import json
import re
import time
from dataclasses import dataclass, field, asdict

from .serialio import open_quiet, serial_opener, read_lines_until, explain, is_write_timeout

DEV_INFO = re.compile(r"DEV INFO (\{.*\})\s*$")
PARTITION = re.compile(r'^Partition (\d+) "([^"]*)": ([a-z_]+)')
FIRMWARE = re.compile(r"^Firmware: (\S+)")
BOARD_LINE = re.compile(r"board: (.+) \(([a-z0-9_]+)\)\s*$")
UNRECOGNIZED = "Unrecognized command"


@dataclass
class ProbeResult:
    port: str
    answered: bool = False     # a RoboAlarms console answered
    heard: bool = False        # the port produced any bytes at all
    board: str = ""
    version: str = ""
    profile: str = ""
    chip_revision: int | None = None
    idle: bool | None = None   # disarmed and not in installer mode; None = unknown
    probation: bool = False
    confirm_required: bool = False   # a network update waits for its confirmation
    operation: str = ""              # the last network update's ID: kept after it ends
    panel_id: str = ""
    elf: str = ""
    states: list = field(default_factory=list)
    error: dict | None = None
    at: float = 0.0

    @property
    def busy_reason(self):
        """Why the panel must not be restarted now, or ""."""
        if not self.answered:
            return ""
        if self.idle is False:
            armed = [s for s in self.states if s != "disarmed"]
            if armed:
                return f"The panel is {armed[0].replace('_', ' ')}: disarm it first."
            return "The panel is busy (armed, in installer mode, or an alarm is showing): finish that first."
        if self.probation or self.confirm_required:
            return "The panel is checking a new firmware (probation): wait a minute and look again."
        return ""

    def to_dict(self):
        d = asdict(self)
        d["busy_reason"] = self.busy_reason
        return d


def parse_describe(result, text):
    info = json.loads(text)
    result.answered = True
    result.board = str(info.get("board", ""))
    result.version = str(info.get("version", ""))
    result.profile = str(info.get("profile", ""))
    rev = info.get("chip_revision")
    result.chip_revision = int(rev) if isinstance(rev, (int, float)) else None
    result.idle = bool(info.get("idle")) if "idle" in info else None
    result.probation = bool(info.get("probation", False))
    result.confirm_required = bool(info.get("confirm_required", False))
    result.operation = str(info.get("operation", "") or "")
    result.panel_id = str(info.get("id", ""))
    result.elf = str(info.get("elf", ""))


def parse_status(result, lines):
    states = []
    installer = False
    for line in lines:
        m = PARTITION.match(line.strip())
        if m:
            states.append(m.group(3))
        if "installer" in line.lower() and "code" not in line.lower():
            installer = True
    if states:
        result.answered = True
        result.states = states
        result.idle = all(s == "disarmed" for s in states) and not installer


def probe(port, opener=serial_opener, timeout=2.5):
    result = ProbeResult(port=port, at=time.time())
    try:
        s = open_quiet(port, opener=opener)
    except Exception as e:
        err = explain(e, port)
        result.error = {"kind": err.kind, "message": err.message, "advice": err.advice}
        return result
    try:
        s.write(b"\r")
        early, _ = read_lines_until(s, lambda line: False, 0.4)
        result.heard = bool(early) or bool(s.in_waiting if hasattr(s, "in_waiting") else 0)
        s.reset_input_buffer()
        s.write(b"dev describe\r")
        lines, hit = read_lines_until(
            s, lambda line: bool(DEV_INFO.search(line)) or UNRECOGNIZED in line, timeout)
        result.heard = result.heard or bool(lines)
        if hit and DEV_INFO.search(hit):
            try:
                parse_describe(result, DEV_INFO.search(hit).group(1))
            except ValueError:
                pass
            else:
                if result.idle is not True:
                    # Not idle: the partition states say how, for the words on screen. No "idle"
                    # in the answer: they are all that says it (FLS2-001; still unknown without them).
                    s.write(b"status\r")
                    lines, _ = read_lines_until(s, lambda line: line.startswith("Network:"), 2.0)
                    idle = result.idle
                    parse_status(result, lines)
                    if idle is not None:
                        result.idle = idle
                return result
        if hit and UNRECOGNIZED in hit:
            # A RoboAlarms console without `dev`: ask what state it is in, and its version.
            s.write(b"status\r")
            lines, _ = read_lines_until(s, lambda line: line.startswith("Network:"), 2.0)
            parse_status(result, lines)
            s.write(b"ota\r")
            lines, hit = read_lines_until(s, lambda line: bool(FIRMWARE.match(line)), 1.5)
            if hit:
                result.answered = True
                result.version = FIRMWARE.match(hit).group(1)
            for line in lines:
                m = BOARD_LINE.search(line)
                if m:
                    result.board = m.group(2)
        return result
    except Exception as e:
        if is_write_timeout(e):
            return result   # nothing reads this port, so no RoboAlarms console (the Zigbee module, B027)
        err = explain(e, port)
        result.error = {"kind": err.kind, "message": err.message, "advice": err.advice}
        return result
    finally:
        try:
            s.close()
        except Exception:
            pass
