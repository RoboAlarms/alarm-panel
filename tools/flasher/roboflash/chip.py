"""The chip, through its ROM loader and esptool (FLS-004, FLS-010).

This is the only module that imports esptool, and it reaches esptool only through the calls
named in ALLOWED_CMDS and ALLOWED_LOADER: identify, read, erase, write, checksum, reset. There
is deliberately no way from here to burn eFuses, enable secure boot or flash encryption, or
touch another chip; tools/tests/test_flasher_safety.py fails if anything else appears.

Connecting restarts the board into its loader (RTS/DTR), so callers gate it on the panel being
idle (jobs.py). Progress arrives through a small esptool logger installed for the session.
"""
import threading

from .boards import ChipInfo
from .imagefile import chip_key

ALLOWED_CMDS = {"detect_chip", "run_stub", "attach_flash", "detect_flash_size", "read_flash",
                "erase_flash", "write_flash", "reset_chip"}
ALLOWED_LOADER = {"get_chip_revision", "get_chip_description", "read_mac", "get_crystal_freq",
                  "flash_md5sum", "change_baud", "CHIP_NAME", "IS_STUB", "_port"}

CONNECT_BAUD = 115200
FAST_BAUD = 460800
READ_BAUD = 921600

_SESSION_LOCK = threading.Lock()   # esptool's logger is process-wide: one session at a time


class Cancelled(Exception):
    pass


def _flash_size_bytes(text):
    if not text:
        return 0
    text = text.upper()
    if text.endswith("MB"):
        return int(text[:-2]) * 1024 * 1024
    if text.endswith("KB"):
        return int(text[:-2]) * 1024
    return 0


def _make_logger(sink):
    from esp_pylib.logger import EspLogBase

    class FlasherLog(EspLogBase):
        """esptool's words go to the job log; its progress bars to the current step."""

        def __init__(self):
            self.on_progress = None   # not "progress": esp_pylib's progress() context manager must stay reachable

        def _say(self, *args):
            text = " ".join(str(a) for a in args).strip()
            if text:
                sink(text)

        def print(self, *args, **kwargs):
            self._say(*args)

        def err(self, *args, suggestion=None):
            self._say("error:", *args)

        def warn(self, *args, suggestion=None):
            self._say("warning:", *args)

        def note(self, *args):
            self._say(*args)

        def hint(self, *args):
            self._say(*args)

        def debug(self, *args):
            pass

        def stage(self, finish=False):
            pass

        def set_verbosity(self, mode):
            pass

        def progress_bar(self, cur_iter, total_iters, prefix="", suffix="", bar_length=30):
            if self.on_progress is not None:
                self.on_progress(cur_iter, total_iters)

    return FlasherLog()


class EsptoolSession:
    """One connection to one chip's loader. Use as a context manager; close() never restarts
    the chip, reset() does."""

    def __init__(self, port, log=lambda text: None, cancel=None, fast_baud=FAST_BAUD):
        self.port = port
        self.log = log
        self.cancel = cancel or threading.Event()
        self.fast_baud = fast_baud
        self.esp = None
        self.info = None
        self._logger = None
        self._saved = None
        self._locked = False

    # -- the session ----------------------------------------------------------------------------
    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    def connect(self):
        import esptool.cmds as cmds
        from esp_pylib.logger import EspLog
        _SESSION_LOCK.acquire()
        self._locked = True
        try:
            self._logger = _make_logger(self.log)
            self._saved = EspLog.instance
            EspLog.instance = self._logger
            esp = cmds.detect_chip(self.port, CONNECT_BAUD, "default-reset", False, 3)
            self.esp = esp
            esp = cmds.run_stub(esp)
            self.esp = esp
            if self.fast_baud and self.fast_baud != CONNECT_BAUD:
                try:
                    esp.change_baud(self.fast_baud)
                except Exception as e:  # some bridges refuse a speed: stay at the safe one
                    self.log(f"staying at {CONNECT_BAUD} baud ({e})")
            cmds.attach_flash(esp)
            size = cmds.detect_flash_size(esp)
            try:
                mac = esp.read_mac("BASE_MAC")
            except TypeError:
                mac = esp.read_mac()
            mac_text = ":".join(f"{b:02x}" for b in mac) if mac else ""
            try:
                crystal = int(esp.get_crystal_freq())
            except Exception:
                crystal = 0
            self.info = ChipInfo(chip=chip_key(esp.CHIP_NAME), revision=int(esp.get_chip_revision()),
                                 flash_size=_flash_size_bytes(size), mac=mac_text,
                                 description=str(esp.get_chip_description()), crystal_mhz=crystal)
            return self.info
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            if self.esp is not None and self.esp._port is not None:
                self.esp._port.close()
        except Exception:
            pass
        self.esp = None
        if self._saved is not None:
            from esp_pylib.logger import EspLog
            EspLog.instance = self._saved
            self._saved = None
        if self._locked:
            self._locked = False
            _SESSION_LOCK.release()

    # -- work -----------------------------------------------------------------------------------
    def set_baud(self, baud):
        """Change speed mid-session (the backup reads fast, writes go at the usual speed)."""
        if self.esp is not None and baud != self.fast_baud:
            self.esp.change_baud(baud)
            self.fast_baud = baud

    def reconnect(self, baud):
        """Connect again from the start at another speed. After a garbled read the loader is
        still mid-reply, so a speed change on that stream fails too (a CH340K at 921600)."""
        self.close()
        self.fast_baud = baud
        return self.connect()

    def _progress(self, fn):
        def report(done, total):
            if self.cancel.is_set():
                raise Cancelled()
            if fn:
                fn(done, total)
        self._logger.on_progress = report

    def read(self, offset, size, progress=None):
        import esptool.cmds as cmds
        self._progress(progress)
        try:
            return cmds.read_flash(self.esp, offset, size)
        finally:
            self._logger.on_progress = None

    def erase_all(self, progress=None):
        import esptool.cmds as cmds
        if self.cancel.is_set():
            raise Cancelled()
        self._progress(progress)
        try:
            cmds.erase_flash(self.esp)
        finally:
            self._logger.on_progress = None
        if progress:
            progress(1, 1)

    def write(self, parts, flash_mode="keep", flash_freq="keep", progress=None):
        """Write [(offset, bytes)]; esptool checks each part's MD5 after writing it.
        progress(done, total) counts bytes across all the parts."""
        import esptool.cmds as cmds
        total = sum(len(d) for _, d in parts) or 1
        base = 0
        for offset, data in parts:
            if self.cancel.is_set():
                raise Cancelled()
            size = len(data)
            self._progress(lambda cur, tot, b=base, s=size: progress and progress(
                b + (s * cur // tot if tot else 0), total))
            try:
                cmds.write_flash(self.esp, [(offset, data)], flash_freq=flash_freq,
                                 flash_mode=flash_mode, flash_size="keep")
            finally:
                self._logger.on_progress = None
            base += size
            if progress:
                progress(base, total)

    def md5(self, offset, size):
        if self.cancel.is_set():
            raise Cancelled()
        digest = self.esp.flash_md5sum(offset, size)
        return digest.hex() if isinstance(digest, (bytes, bytearray)) else str(digest).lower()

    def reset(self):
        """Restart the chip into its firmware, then let go of the port."""
        import esptool.cmds as cmds
        try:
            if self.esp is not None:
                cmds.reset_chip(self.esp, "hard-reset")
        finally:
            self.close()


def connect(port, log=lambda text: None, cancel=None, fast_baud=FAST_BAUD):
    session = EsptoolSession(port, log=log, cancel=cancel, fast_baud=fast_baud)
    session.connect()
    return session


def explain(exc):
    """(title, advice) for an esptool failure."""
    text = str(exc)
    low = text.lower()
    if "failed to connect" in low or "no serial data received" in low or "wrong boot mode" in low:
        return ("The board did not answer its loader.",
                "Check the USB cable carries data (some are charge-only) and the board's power "
                "switch, if it has one, is on, then try again. If it still fails, hold BOOT, press "
                "and release RESET, release BOOT, and try again.")
    if "could not open" in low or "access is denied" in low or "permission" in low or "busy" in low:
        return ("The port could not be opened.",
                "Close other programs that use the board (Arduino IDE, PuTTY, a serial monitor), "
                "or unplug and replug it.")
    if "md5" in low or "digest" in low or "verify" in low:
        return ("What was written does not read back the same.",
                "Try again; if it keeps failing, try another USB cable or port. The board is not "
                "harmed and can always be written again.")
    if "serial" in low or "port" in low or "timed out" in low or "timeout" in low:
        return ("The connection to the board was lost.",
                "Check the cable and plug it back in, then try again. Nothing is harmed: the "
                "board's loader always answers, so the install can simply be repeated.")
    return ("esptool reported a problem: " + text.splitlines()[0][:200] if text else "esptool failed.",
            "Try again. If it keeps happening, save the log (Open logs) and add it to a bug report.")
