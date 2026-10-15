"""The 433 MHz receiver page (FLS-014): what `rf` says, which sensors were heard, and captures.

It reads the same lines a person would see at the console (docs/features/25 "First bench"):
  `rf`          the status block (receiver words, module/cc1101, capture, decoder, source, last);
  `rf raw on`   one log line per decoded transmission:
                `I (1234) rf: EV1527 A2B3C code 5 (A2B3C5, T 350 us)`;
  `rf capture`  `capturing N ms ...`, `pulses N (...)`, then lines of H/L durations.
"""
import re
import time
from collections import deque
from dataclasses import dataclass, field

WORDS = [
    (re.compile(r"^Receiver module on GPIO (\d+), (\d+) edges/s, line (\d+)% high$"), "module"),
    (re.compile(r"^CC1101 at ([\d.]+) MHz, (\S+), noise (-?\d+) dBm$"), "cc1101"),
    (re.compile(r"^No receiver module drives GPIO (\d+) yet$"), "missing"),
    (re.compile(r"^No CC1101 answers on SPI \(GPIO (\d+) is its data\)$"), "missing"),
    (re.compile(r"^No receiver yet: no CC1101, GPIO (\d+) quiet$"), "missing"),
    (re.compile(r"^433 MHz receiver: not started$"), "stopped"),
    (re.compile(r"^433 MHz receiver: off \(.*\)$"), "off"),
    (re.compile(r"^433 MHz receiver: not in this firmware$"), "absent"),
    (re.compile(r"^(.+): jammed$"), "jammed"),
]
# receiver= is "CC1101", "receiver module" (two words) or "none"
MODULE = re.compile(r"^module=(\S+) receiver=(.+?) edges/s=(\d+) high=(\d+)% storms=(\d+)")
CC1101 = re.compile(r"^cc1101 version=0x([0-9A-Fa-f]+) state=(\S+)")
CAPTURE = re.compile(r"^capture receptions=(\d+) symbols=(\d+) overruns=(\d+)")
DECODER = re.compile(r"^decoder pulses=(\d+) frames=(\d+) rejected=(\d+) glitches=(\d+)")
SOURCE = re.compile(r"^source transmissions=(\d+) unknown=(\d+) bursts=(\d+)")
LAST = re.compile(r"^last (.+) \(([0-9A-F]+), T (\d+) us\) at (\d+) ms")
# Any protocol the firmware decodes (EV1527, HT6P20B, HT12E, KeeLoq, arctech; "RF" from old
# firmware), then the flags it may add: ", PT2262 shape", ", battery low",
# ", common factory address".
DECODED = re.compile(r"(?:^[IWE] \(\d+\) rf: )?([A-Za-z][A-Za-z0-9]*) ([0-9A-F]+) code ([0-9A-F]+) "
                     r"\(([0-9A-F]+), T (\d+) us((?:, [a-zA-Z0-9 ]+)*)\)")
MODULE_SET = re.compile(r"^receiver (auto|cc1101|ook|off) from the next restart")
PLACE = re.compile(r" place=(\S+)")
CAPTURING = re.compile(r"^capturing (\d+) ms")
PULSES = re.compile(r"^pulses (\d+) \(")
PULSE_ROW = re.compile(r"^(?:[HL]\d+ ?)+$")
RAW_STATE = re.compile(r"^raw (on|off)")
# A stored layout of the person's own pins (RF-025), and why it isn't used this start
CUSTOM = re.compile(r"^custom (?:[a-z0-9]+=\d+ ?)+$|^custom place not used: ")


def is_status_line(text):
    """True for the lines `rf` prints (the ones a quiet poll hides from the console)."""
    return (any(p.match(text) for p, _ in WORDS) or bool(MODULE.match(text) or CC1101.match(text)
            or CAPTURE.match(text) or DECODER.match(text) or SOURCE.match(text) or LAST.match(text)
            or CUSTOM.match(text)))


@dataclass
class Heard:
    protocol: str
    device: str
    codes: dict = field(default_factory=dict)   # code -> times heard
    count: int = 0
    first: float = 0.0
    last: float = 0.0
    last_code: str = ""
    unit_us: int = 0
    pt2262: bool = False
    battery_low: bool = False
    common: bool = False   # an address many transmitters share from the factory (RF-018)

    def to_dict(self):
        return {"protocol": self.protocol, "device": self.device, "codes": self.codes,
                "count": self.count, "first": self.first, "last": self.last,
                "last_code": self.last_code, "unit_us": self.unit_us, "pt2262": self.pt2262,
                "battery_low": self.battery_low, "common": self.common}


class RfMonitor:
    def __init__(self, clock=time.time):
        self.clock = clock
        self.status = {}
        self.heard = {}
        self.raw_on = False
        self.capture = None       # {"ms", "expected", "pulses": [], "done", "started", "seq"}
        # RF21: each recording the panel starts has its own number, and the last few are kept,
        # so the request that asked for one saves that one, not whichever came after it.
        self.capture_seq = 0
        self.recent = deque(maxlen=4)
        self.updated = 0.0

    def reset_heard(self):
        self.heard = {}

    def feed(self, text):
        """Take one console line; True when it changed what the page shows."""
        text = text.strip()
        m = LAST.match(text)   # before DECODED: `rf`'s "last ..." line quotes a transmission too
        if m:
            self.status["last"] = {"words": m.group(1), "code": m.group(2), "unit_us": int(m.group(3))}
            return True
        m = DECODED.search(text)
        if m:
            proto, device, code, _full, unit, flags = m.groups()
            flags = flags or ""
            key = f"{proto} {device}"
            now = self.clock()
            h = self.heard.get(key)
            if h is None:
                h = self.heard[key] = Heard(protocol=proto, device=device, first=now)
            h.count += 1
            h.codes[code] = h.codes.get(code, 0) + 1
            h.last = now
            h.last_code = code
            h.unit_us = int(unit)
            h.pt2262 = "PT2262 shape" in flags
            h.battery_low = "battery low" in flags
            h.common = "common factory address" in flags
            self.updated = now
            return True
        m = MODULE_SET.match(text)
        if m:
            self.status["module_next"] = m.group(1)
            return True
        for pattern, kind in WORDS:
            m = pattern.match(text)
            if m:
                s = {"words": text, "kind": kind, "at": self.clock()}
                if kind == "module":
                    s.update(gpio=int(m.group(1)), edges_per_s=int(m.group(2)), high_pct=int(m.group(3)))
                elif kind == "cc1101":
                    s.update(mhz=m.group(1), state=m.group(2), noise_dbm=int(m.group(3)))
                elif kind == "missing":
                    s.update(gpio=int(m.group(1)))
                self.status = s
                return True
        for pattern, keys in ((MODULE, ("module", "receiver", "edges_per_s", "high_pct", "storms")),
                              (CAPTURE, ("receptions", "symbols", "overruns")),
                              (DECODER, ("pulses", "frames", "rejected", "glitches")),
                              (SOURCE, ("transmissions", "unknown", "bursts"))):
            m = pattern.match(text)
            if m:
                for k, v in zip(keys, m.groups()):
                    self.status[k] = int(v) if v.isdigit() else v
                if pattern is MODULE:   # where it runs (RF-019): "default" or "j7"; older firmware says nothing
                    p = PLACE.search(text)
                    self.status["place"] = p.group(1) if p else "default"
                return True
        m = RAW_STATE.match(text)
        if m:
            self.raw_on = m.group(1) == "on"
            return True
        return self._feed_capture(text)

    def _feed_capture(self, text):
        m = CAPTURING.match(text)
        if m:
            self.capture_seq += 1
            self.capture = {"ms": int(m.group(1)), "expected": None, "pulses": [], "done": False,
                            "started": self.clock(), "seq": self.capture_seq}
            self.recent.append(self.capture)
            return True
        if self.capture is None or self.capture["done"]:
            return False
        m = PULSES.match(text)
        if m:
            self.capture["expected"] = int(m.group(1))
            if self.capture["expected"] == 0:
                self.capture["done"] = True
            return True
        if self.capture["expected"] is not None and PULSE_ROW.match(text):
            self.capture["pulses"].extend(text.split())
            if len(self.capture["pulses"]) >= self.capture["expected"]:
                self.capture["done"] = True
            return True
        if text.startswith("the data pin is not being captured") or text == "no memory":
            self.capture.update(done=True, error=text)
            return True
        return False

    def capture_by_seq(self, seq):
        """The recording numbered seq, while it is one of the last few; else None (RF21)."""
        return next((c for c in self.recent if c.get("seq") == seq), None)

    @staticmethod
    def capture_problem(c):
        """"" for a whole recording: finished without an error, every pulse the panel announced
        received. Else why it is not one, in words (RF22)."""
        if not c:
            return "The panel did not start recording."
        if c.get("error"):
            return f"The panel could not record: {c['error']}."
        if c.get("expected") is None:
            return "The panel's recording did not arrive."
        got = len(c.get("pulses", []))
        if got < c["expected"]:
            return f"{got} of the {c['expected']} pulses the panel recorded arrived."
        if got > c["expected"]:
            return f"The panel announced {c['expected']} pulses, and {got} arrived."
        if not got:
            return "No pulses were recorded."
        if not c.get("done"):
            return "The panel's recording did not finish."
        return ""

    def capture_text(self, header, capture=None):
        """The file the Flasher saves: the header, the counts and whether the recording arrived
        whole (RF22; `#` lines are comments to the decoder's tests), then the pulse rows."""
        c = (capture if capture is not None else self.capture) or {}
        pulses = c.get("pulses", [])
        rows = [" ".join(pulses[i:i + 16]) for i in range(0, len(pulses), 16)]
        lines = [f"# {k}: {v}" for k, v in header.items()]
        problem = self.capture_problem(c)
        expected = c.get("expected")
        lines.append(f"# expected pulses: {'unknown' if expected is None else expected}")
        lines.append(f"# received pulses: {len(pulses)}")
        lines.append("# status: complete" if not problem else f"# status: incomplete - {problem}")
        lines.append(f"# pulses {len(pulses)} (H high, L low, microseconds; 0 = the line went quiet)")
        return "\n".join(lines + rows) + "\n"

    def to_dict(self):
        return {"status": self.status, "raw_on": self.raw_on,
                "heard": sorted((h.to_dict() for h in self.heard.values()), key=lambda h: -h["last"]),
                "capture": None if self.capture is None else {
                    "ms": self.capture["ms"], "expected": self.capture["expected"],
                    "count": len(self.capture["pulses"]), "done": self.capture["done"],
                    "error": self.capture.get("error", "")}}
