"""Opening a port without restarting the board behind it (FLS-002), and plain words for why a
port would not open (FLS-011, FLS-012).

On these boards RTS drives EN (reset) and DTR drives the boot strap, so both are released
*before* the port opens, as tools/panel_console.py does. Windows honours that; Linux and macOS
drivers may pulse the lines while opening (docs/features/34 open question).
"""
import re
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def serial_opener():
    import serial
    return serial.Serial()


# A write that the other end never takes fails after this many seconds instead of hanging: the
# Zigbee module's USB Serial/JTAG port takes nothing while its radio software runs (B027).
WRITE_TIMEOUT = 2.0


def open_quiet(port, baud=115200, timeout=0.05, opener=serial_opener):
    s = opener()
    s.port = port
    s.baudrate = baud
    s.timeout = timeout
    s.write_timeout = WRITE_TIMEOUT
    s.dtr = False   # boot strap released
    s.rts = False   # EN released
    s.open()
    return s


def pulse_reset(s):
    """Restart the board as its reset button would (RTS = EN), never into download mode."""
    s.dtr = False
    s.rts = True
    time.sleep(0.2)
    s.rts = False


class PortError(Exception):
    """A port problem in words: `kind` is busy / permission / missing / other."""

    def __init__(self, kind, message, advice=""):
        super().__init__(message)
        self.kind = kind
        self.message = message
        self.advice = advice


def is_write_timeout(exc):
    """pyserial's SerialTimeoutException: nothing on the port took a write within WRITE_TIMEOUT."""
    return type(exc).__name__ == "SerialTimeoutException" or "write timeout" in str(exc).lower()


def explain(exc, port):
    """PortError for an exception raised while opening or using `port`."""
    if isinstance(exc, PortError):
        return exc
    if is_write_timeout(exc):
        return PortError("other", f"Nothing on {port} takes what the Flasher sends.",
                         "Unplug the board, plug it back in and try again.")
    text = str(exc)
    low = text.lower()
    if "access is denied" in low or "resource busy" in low or "being used" in low or "errno 16" in low:
        return PortError("busy", f"{port} is in use by another program.",
                         "Close anything else that talks to the board (Arduino IDE, PuTTY, a serial "
                         "monitor, idf.py monitor, another Flasher window), then try again.")
    if "permission denied" in low or "errno 13" in low:
        advice = "Your user may not open serial ports."
        if sys.platform.startswith("linux"):
            advice = ("Add yourself to the dialout group once (sudo usermod -aG dialout $USER), "
                      "log out and back in, then try again.")
        return PortError("permission", f"Not allowed to open {port}.", advice)
    if ("could not open port" in low and ("filenotfound" in low or "no such file" in low
                                          or "cannot find" in low)) or "device not configured" in low \
            or "device disconnected" in low or "clearcommerror failed" in low:
        return PortError("missing", f"{port} is gone: the board was unplugged.",
                         "Plug it back in and choose it again.")
    return PortError("other", f"{port}: {text}", "Unplug the board, plug it back in and try again.")


def read_lines_until(s, done, timeout, on_line=None, stop=None):
    """Read text lines from `s` until done(line) is true or `timeout` seconds pass.
    Returns (all lines, the line that matched or None). ANSI colours are removed.
    stop(), when given, is asked before every read, whether a line came or not: true ends the
    wait at once, so within one read timeout of the port (PERF22)."""
    buf = b""
    lines = []
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if stop is not None and stop():
            return lines, None
        chunk = s.read(4096)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r")
            lines.append(line)
            if on_line:
                on_line(line)
            if done(line):
                return lines, line
    tail = ANSI.sub("", buf.decode("utf-8", "replace")).strip()
    if tail:
        lines.append(tail)
        if done(tail):
            return lines, tail
    return lines, None
