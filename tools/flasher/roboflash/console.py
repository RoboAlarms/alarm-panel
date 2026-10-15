"""The panel's service console, live, without restarting it (FLS-013).

The port opens with DTR and RTS released; a reader thread turns bytes into lines. Commands go
out with a carriage return, as tools/panel_console.py sends them. Background questions (the
433 MHz page asks `rf` every few seconds) are sent "quietly": their replies are still recorded
and parsed, but the console view hides them unless asked to show everything. The only way this
module restarts the panel is restart(), the console's explicit Restart button.
"""
import collections
import threading
import time
from dataclasses import dataclass, asdict

from .serialio import open_quiet, serial_opener, pulse_reset, explain, ANSI

PROMPT = "alarm> "
DEV_INFO_SIGN = "DEV INFO {"     # with the prompt, printed only by a RoboAlarms console (FLS2-001)
MAX_LINE = 4096     # bytes; the panel's longest console line (a JSON reply) is far shorter (FLS2-006)


@dataclass
class Line:
    seq: int
    t: float
    text: str
    level: str      # I/W/E/D/V for ESP-IDF log lines, "" otherwise
    source: str     # panel / you / tool
    quiet: bool = False

    def to_dict(self):
        return asdict(self)


def _level(text):
    if len(text) > 3 and text[0] in "IWEDV" and text[1:3] == " (":
        return text[0]
    return ""


class ConsoleSession:
    def __init__(self, port, on_lines, on_closed=None, opener=serial_opener, history=4000, on_roboalarms=None):
        self.port = port
        # on_roboalarms(): called once, at the first prompt or DEV INFO answer the panel prints
        self._on_roboalarms = on_roboalarms
        self.roboalarms = False
        self._on_lines = on_lines
        self._on_closed = on_closed
        self._opener = opener
        self._serial = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._seq = 0
        self.history = collections.deque(maxlen=history)
        self.listeners = []          # fn(line) for every panel line, quiet or not
        self._quiet_until = 0.0
        self._quiet_words = set()
        self._quiet_match = None
        self._echo = ("", 0.0)
        self._pending = bytearray()  # bytes since the last line break, at most MAX_LINE
        self.error = None

    @property
    def is_open(self):
        return self._serial is not None and not self._stop.is_set()

    def open(self):
        try:
            self._serial = open_quiet(self.port, opener=self._opener)
        except Exception as e:
            raise explain(e, self.port) from None
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name=f"console {self.port}", daemon=True)
        self._thread.start()
        self._note(f"Connected to {self.port} (the panel was not restarted).")

    def close(self, why=""):
        self._stop.set()
        s, self._serial = self._serial, None
        if s is not None:
            try:
                s.close()
            except Exception:
                pass
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=1.0)
        if why:
            self._note(why)

    def _add(self, text, source, level="", quiet=False):
        with self._lock:
            self._seq += 1
            line = Line(self._seq, time.time(), text, level, source, quiet)
            self.history.append(line)
        return line

    def _note(self, text):
        self._on_lines([self._add(text, "tool")])

    def send(self, text, quiet=False, quiet_match=None, quiet_for=1.5):
        """Type a command. quiet: hide the echo and the reply lines quiet_match(line) accepts."""
        if not self.is_open:
            raise RuntimeError("the console is not open")
        if quiet:
            self._quiet_until = time.monotonic() + quiet_for
            self._quiet_words = {text.strip(), PROMPT + text.strip(), PROMPT.strip()}
            self._quiet_match = quiet_match
        else:
            self._on_lines([self._add(text, "you")])
            self._echo = (text.strip(), time.monotonic() + 2.0)   # the console repeats it back
        try:
            self._serial.write(text.encode("utf-8") + b"\r")
        except Exception as e:
            err = explain(e, self.port)
            self.close(err.message)
            raise err from None

    def restart_panel(self):
        if not self.is_open:
            raise RuntimeError("the console is not open")
        pulse_reset(self._serial)
        self._note("Restarted the panel (its reset line, like its reset button).")

    def _is_quiet(self, text):
        stripped = text.strip()
        echo, until = self._echo
        if echo and stripped == echo and time.monotonic() <= until:
            self._echo = ("", 0.0)
            return True
        if time.monotonic() > self._quiet_until:
            return False
        if stripped in self._quiet_words or stripped == "":
            return True
        return bool(self._quiet_match and self._quiet_match(stripped))

    def _seen(self, text):
        """A sign of a RoboAlarms console in what the panel printed: its prompt or a DEV INFO answer."""
        if not self.roboalarms and (text.startswith(PROMPT) or DEV_INFO_SIGN in text):
            self.roboalarms = True
            if self._on_roboalarms:
                try:
                    self._on_roboalarms()
                except Exception:
                    pass

    def _panel_line(self, raw):
        text = ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r")
        self._seen(text)
        if text.startswith(PROMPT) and text.strip() != PROMPT.strip():
            text = text[len(PROMPT):]
        line = self._add(text, "panel", _level(text), self._is_quiet(text))
        for fn in list(self.listeners):
            try:
                fn(line)
            except Exception:
                pass
        return line

    def _run(self):
        # FLS2-006: what has come without a line break yet is kept to MAX_LINE bytes. Past that it is
        # shown as cut lines, with one warning until a line break comes, so a stream that never sends
        # one (a binary stream, another baud rate) can't grow without end.
        self._pending = buf = bytearray()
        cut = False
        while not self._stop.is_set():
            s = self._serial
            if s is None:
                break
            try:
                chunk = s.read(4096)
            except Exception as e:
                if not self._stop.is_set():
                    err = explain(e, self.port)
                    self.error = {"kind": err.kind, "message": err.message, "advice": err.advice}
                    self.close(err.message)
                    if self._on_closed:
                        self._on_closed(self)
                break
            if not chunk:
                continue
            out = []
            while chunk:
                take = chunk[:MAX_LINE]
                chunk = chunk[MAX_LINE:]
                buf += take
                while True:
                    end = buf.find(b"\n")
                    if end >= 0:
                        raw = bytes(buf[:end])
                        del buf[:end + 1]
                        out.append(self._panel_line(raw))
                        cut = False
                    elif len(buf) >= MAX_LINE:
                        raw = bytes(buf[:MAX_LINE])
                        del buf[:MAX_LINE]
                        if not cut:
                            out.append(self._add(f"The panel sent more than {MAX_LINE} bytes without a line break: "
                                                 "they are shown cut into lines. Is this a RoboAlarms panel "
                                                 "on its console port?", "tool"))
                            cut = True
                        out.append(self._panel_line(raw))
                    else:
                        break
            if buf.startswith(PROMPT.encode()):
                self._seen(PROMPT)      # the prompt waits for a command, without a line break
            if out:
                self._on_lines(out)

    def save(self, path, include_quiet=True):
        with open(path, "w", encoding="utf-8") as f:
            for line in list(self.history):
                if line.quiet and not include_quiet:
                    continue
                stamp = time.strftime("%H:%M:%S", time.localtime(line.t))
                who = {"you": "> ", "tool": "# "}.get(line.source, "")
                f.write(f"{stamp} {who}{line.text}\n")
        return path
