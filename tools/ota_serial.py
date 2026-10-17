#!/usr/bin/env python3
"""Upload a signed firmware image to a running panel over its service console (docs/features/13,
OTA-013), without a network and without the boot button.

The panel checks the image while it arrives (project, chip, version), verifies its signature at
the end and restarts into it as soon as the system is disarmed and quiet. After the restart the
new firmware runs a 60 s self-test; if that fails, the previous firmware comes back by itself.

Needs pyserial (it comes with ESP-IDF's Python):
    C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools/ota_serial.py -p COM4 \\
        firmware/crowpanel_p4/build_v1/alarm_crowpanel_p4.bin

The bench only (a development build on the master; docs/features/11, the bench copy): --copy sends
a keypad of another model's image into the master's copy in memory instead - nothing is written to
flash and nothing restarts there - and the master relays it to that keypad at once. For a bench
whose update server has no development builds: update the master first (without --copy), then:
    ... tools/ota_serial.py -p COM6 --copy \
        firmware/crowpanel_10/build_v1_<version>_dev/alarm_crowpanel_10.bin

Protocol (one line each way, stop-and-wait):
    host:  ota recv <size> <sha256 hex> [force]   (--copy: ota copy <size> <sha256 hex>)
    panel: OTA READY <max bytes per line>
    host:  <seq> <crc32 hex8> <base64>   (seq from 0; zlib CRC-32 of the decoded bytes)
    panel: OTA OK <seq> | OTA RETRY <expected seq> | OTA FAIL <reason>
    host:  END
    panel: OTA DONE <version> | OTA FAIL <reason>   (--copy: OTA DONE <version> keypad <n>)
"""
import argparse
import base64
import hashlib
import re
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: run this with ESP-IDF's Python (see the docstring)")

REPLY = re.compile(r"OTA (READY|OK|RETRY|FAIL|DONE)\s*(.*)")


class Link:
    def __init__(self, port, baud, log):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        # Don't touch the reset / boot lines: opening the port must not restart the panel.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.buf = b""
        self.log = log

    def send(self, text):
        self.ser.write(text.encode("ascii"))
        self.ser.flush()

    def line(self, timeout):
        """Next complete line (without its end), or None after timeout seconds."""
        end = time.monotonic() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                raw, self.buf = self.buf[:nl], self.buf[nl + 1:]
                text = raw.decode("utf-8", "replace").rstrip("\r")
                if self.log:
                    self.log.write(text + "\n")
                return text
            if time.monotonic() >= end:
                return None
            self.buf += self.ser.read(4096)

    def reply(self, timeout, kinds):
        """Next protocol reply of one of 'kinds' as (kind, arg); other lines are skipped."""
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return None
            text = self.line(left)
            if text is None:
                return None
            m = REPLY.search(text)
            if m and m.group(1) in kinds:
                return m.group(1), m.group(2).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", help="signed application image (build_*/alarm_crowpanel_p4.bin)")
    ap.add_argument("-p", "--port", required=True, help="serial port, e.g. COM4 or /dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--force", action="store_true",
                    help="also install an older or equal version (installer's choice)")
    ap.add_argument("--copy", action="store_true",
                    help="the bench: a keypad of another model's image into the master's copy "
                         "(development builds; the master relays it to that keypad)")
    ap.add_argument("--tamper", action="store_true",
                    help="test only: flip one byte in the middle; the panel must refuse it")
    ap.add_argument("--watch", type=int, default=0, metavar="S",
                    help="print the panel's output for S seconds afterwards (restart, self-test)")
    ap.add_argument("--log", help="write everything the panel printed to this file")
    args = ap.parse_args()
    if args.copy and args.force:
        ap.error("--force is for this panel's own image, not --copy")

    data = bytearray(open(args.image, "rb").read())
    if args.tamper:
        data[len(data) // 2] ^= 0x01
    sha = hashlib.sha256(data).hexdigest()
    log = open(args.log, "w", encoding="utf-8") if args.log else None
    link = Link(args.port, args.baud, log)

    link.send("\r")
    time.sleep(0.5)
    link.ser.reset_input_buffer()
    link.buf = b""
    print(f"Sending {args.image}: {len(data)} bytes, sha256 {sha[:16]}...")
    if args.copy:
        link.send(f"ota copy {len(data)} {sha}\r")
    else:
        link.send(f"ota recv {len(data)} {sha}{' force' if args.force else ''}\r")
    r = link.reply(20, {"READY", "FAIL"})
    if r is None:
        sys.exit("The panel didn't answer (is the service console on this port?)")
    if r[0] == "FAIL":
        sys.exit(f"The panel refused: {r[1]}")
    chunk = min(int(r[1] or 1024), 1024)

    count = (len(data) + chunk - 1) // chunk
    seq = 0
    retries = 0
    shown = -1
    started = time.monotonic()
    while seq < count:
        piece = bytes(data[seq * chunk:(seq + 1) * chunk])
        line = f"{seq} {zlib.crc32(piece) & 0xFFFFFFFF:08x} {base64.b64encode(piece).decode()}\n"
        link.send(line)
        while True:
            r = link.reply(3, {"OK", "RETRY", "FAIL"})
            if r is None or r[0] == "RETRY":
                retries += 1
                if retries > 50:
                    link.send("ABORT\n")
                    sys.exit("Too many retries: check the cable")
                if r is not None:
                    seq = int(r[1])
                break
            if r[0] == "FAIL":
                sys.exit(f"The panel stopped the upload: {r[1]}")
            if int(r[1]) == seq:
                seq += 1
                retries = 0
                break
            # an answer to an earlier line: keep waiting for this one
        percent = seq * 100 // count
        if percent // 5 != shown:
            shown = percent // 5
            rate = seq * chunk / max(time.monotonic() - started, 0.1) / 1024
            print(f"  {percent:3d} %  ({rate:.1f} KB/s)", flush=True)

    link.send("END\n")
    r = link.reply(60, {"DONE", "FAIL"})
    if r is None:
        sys.exit("No answer after the upload")
    if r[0] == "FAIL":
        print(f"The panel refused the image: {r[1]}")
        code = 2
    elif args.copy:
        print(f"Held: version {r[1]}. The master relays it now ('plink fw' there shows it).")
        code = 0
    else:
        print(f"Verified: version {r[1]}. The panel restarts into it once the system is idle.")
        code = 0
    if args.watch > 0:
        end = time.monotonic() + args.watch
        while time.monotonic() < end:
            text = link.line(end - time.monotonic())
            if text is not None:
                print(f"| {text}")
    if log:
        log.close()
    sys.exit(code)


if __name__ == "__main__":
    main()
