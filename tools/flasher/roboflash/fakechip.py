"""A chip in memory with the same session interface as chip.EsptoolSession, for the host
tests and the --mock window: flash contents, timing and failures are scriptable."""
import hashlib
import threading
import time

from .boards import ChipInfo
from .chip import Cancelled, READ_BAUD


class FakeBoard:
    """The board on the other end of a fake port."""

    def __init__(self, info, flash=None, seconds_per_mb=0.0, fail_connect=None,
                 unplug_after_write_bytes=None, read_error=None, corrupt_writes=(), fast_read_error=None):
        self.info = info
        self.flash = bytearray(flash if flash is not None else b"\xff" * info.flash_size)
        self.seconds_per_mb = seconds_per_mb
        self.fail_connect = fail_connect
        self.unplug_after_write_bytes = unplug_after_write_bytes
        self.read_error = read_error
        self.fast_read_error = fast_read_error   # a read at READ_BAUD fails, and the stream stays broken
        self.corrupt_writes = set(corrupt_writes)   # offsets whose write lands with its first byte flipped
        self.connects = 0
        self.resets = 0
        self.erases = 0
        self.writes = []    # [(offset, size)]
        self.reads = []

    def session(self, port, log=lambda text: None, cancel=None):
        return FakeSession(self, port, log, cancel or threading.Event())

    def connect(self, port, log=lambda text: None, cancel=None, fast_baud=None):
        """A connected session, as chip.connect() returns one."""
        s = self.session(port, log, cancel)
        s.connect()
        s.fast_baud = fast_baud
        return s


class FakeSession:
    def __init__(self, board, port, log, cancel):
        self.board = board
        self.port = port
        self.log = log
        self.cancel = cancel
        self.info = None
        self.fast_baud = None
        self.broken = False

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    def connect(self):
        self.board.connects += 1
        if self.board.fail_connect:
            raise RuntimeError(self.board.fail_connect)
        self.log(f"Connected to {self.board.info.chip} on {self.port}")
        self.info = self.board.info
        return self.info

    def close(self):
        pass

    def set_baud(self, baud):
        if self.broken:
            raise RuntimeError("Serial data stream stopped: Possible serial noise or corruption.")
        self.fast_baud = baud

    def reconnect(self, baud):
        self.broken = False
        self.fast_baud = baud
        return self.connect()

    def _tick(self, done, total, progress, size):
        if self.cancel.is_set():
            raise Cancelled()
        if self.board.seconds_per_mb:
            time.sleep(self.board.seconds_per_mb * size / (1024 * 1024))
        if progress:
            progress(done, total)

    def read(self, offset, size, progress=None):
        self.board.reads.append((offset, size))
        if self.board.read_error:
            raise RuntimeError(self.board.read_error)
        if self.board.fast_read_error and self.fast_baud == READ_BAUD:
            self.broken = True
            raise RuntimeError(self.board.fast_read_error)
        step = 256 * 1024
        for done in range(0, size, step):
            self._tick(min(done + step, size), size, progress, min(step, size - done))
        return bytes(self.board.flash[offset:offset + size])

    def erase_all(self, progress=None):
        self.board.erases += 1
        self._tick(1, 1, progress, len(self.board.flash) // 8)
        self.board.flash[:] = b"\xff" * len(self.board.flash)

    def write(self, parts, flash_mode="keep", flash_freq="keep", progress=None):
        total = sum(len(d) for _, d in parts) or 1
        written = 0
        for offset, data in parts:
            step = 64 * 1024
            for i in range(0, len(data), step):
                chunk = data[i:i + step]
                limit = self.board.unplug_after_write_bytes
                if limit is not None and written + len(chunk) > limit:
                    raise OSError("ClearCommError failed: the device disconnected")
                if i == 0 and offset in self.board.corrupt_writes:
                    chunk = bytes([chunk[0] ^ 0xFF]) + bytes(chunk[1:])   # one write lands damaged
                self.board.flash[offset + i:offset + i + len(chunk)] = chunk
                written += len(chunk)
                self._tick(written, total, progress, len(chunk))
            self.board.writes.append((offset, len(data)))

    def md5(self, offset, size):
        return hashlib.md5(bytes(self.board.flash[offset:offset + size])).hexdigest()

    def reset(self):
        self.board.resets += 1


def p4_v13(**kw):
    return FakeBoard(ChipInfo(chip="esp32p4", revision=103, flash_size=16 * 1024 * 1024,
                              mac="30:ed:a0:12:34:56", description="ESP32-P4 (revision v1.3)"), **kw)


def esp32_v31(**kw):
    return FakeBoard(ChipInfo(chip="esp32", revision=301, flash_size=4 * 1024 * 1024,
                              mac="8c:94:df:00:00:01", description="ESP32-D0WD-V3 (revision v3.1)"), **kw)


def h2_v01(flash=None, **kw):
    """A Zigbee module (Elecrow's ESP32-H2 board) on its own USB port; fictional MAC (FLS-030)."""
    return FakeBoard(ChipInfo(chip="esp32h2", revision=1, flash_size=4 * 1024 * 1024,
                              mac="74:4d:bd:00:00:01", description="ESP32-H2 (revision v0.1)"),
                     flash=flash, **kw)


def s3_v02(**kw):
    """An ESP32-S3 on its own USB Serial/JTAG port (303A:1001 too): not a Zigbee module."""
    return FakeBoard(ChipInfo(chip="esp32s3", revision=2, flash_size=8 * 1024 * 1024,
                              mac="f4:12:fa:00:00:09", description="ESP32-S3 (QFN56) (revision v0.2)"), **kw)
