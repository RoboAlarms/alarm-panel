#!/usr/bin/env python3
"""
panel_console.py - talk to the panel's service console from a script (docs/features/06, /21).

Opening the port does not reset the panel (RTS and DTR stay released). Commands are typed one
after another; everything the panel prints is shown with timestamps (and saved with --log).

    python tools/panel_console.py -p COM4 status "display" "ota"
    python tools/panel_console.py -p COM4 --gap 5 "display watch 60"      # a line a second
    python tools/panel_console.py -p COM4 --reset --seconds 30            # capture a boot log
    python tools/panel_console.py -p COM4 --hold-reset                    # screen off, see below

--hold-reset keeps the P4 in reset (RTS asserted) until Ctrl+C: the display stops at once. Use
it if the screen ever flashes (see CONTRIBUTING.md "Display safety").

Needs pyserial, which comes with ESP-IDF's Python:
    C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools/panel_console.py ...
Close any other program that has the port open (idf.py monitor, a terminal) first.
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: run this with ESP-IDF's Python (see the docstring)")

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def open_port(port, baud):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.05
    s.dtr = False  # BOOT strap released
    s.rts = False  # EN released: opening the port doesn't reset the panel
    s.open()
    return s


def reset(s):
    s.rts = True
    time.sleep(0.2)
    s.rts = False


def main():
    ap = argparse.ArgumentParser(description="Send console commands to the panel and show its output.")
    ap.add_argument("commands", nargs="*", help="console commands, typed one after another")
    ap.add_argument("-p", "--port", required=True, help="serial port, e.g. COM4 or /dev/ttyACM0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true", help="reset the panel first (boot log)")
    ap.add_argument("--hold-reset", action="store_true",
                    help="keep the panel in reset (display off) until Ctrl+C")
    ap.add_argument("--gap", type=float, default=2.0,
                    help="seconds between commands and after the last one (default 2)")
    ap.add_argument("--seconds", type=float, default=0,
                    help="capture at least this long in total")
    ap.add_argument("--until", help="stop as soon as a line matches this regular expression")
    ap.add_argument("--log", help="also write the output to this file")
    args = ap.parse_args()

    s = open_port(args.port, args.baud)
    if args.hold_reset:
        s.rts = True
        print("Holding the panel in reset (display off). Ctrl+C releases it and it restarts.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            s.rts = False
            s.close()
            return

    log = open(args.log, "w", encoding="utf-8") if args.log else None
    until = re.compile(args.until) if args.until else None
    if args.reset:
        reset(s)
    start = time.monotonic()
    plan = [(i * args.gap + (3.0 if args.reset else 0.3), c) for i, c in enumerate(args.commands)]
    end = max(args.seconds, (plan[-1][0] if plan else 0) + args.gap)
    buf = b""
    try:
        while time.monotonic() - start < end:
            now = time.monotonic() - start
            while plan and plan[0][0] <= now:
                cmd = plan.pop(0)[1]
                s.write((cmd + "\r").encode())
                s.flush()
                line = "[%7.2f] >>> %s" % (now, cmd)
                print(line, flush=True)
                if log:
                    log.write(line + "\n")
            buf += s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = ANSI.sub("", raw.decode("utf-8", "replace").rstrip("\r"))
                line = "[%7.2f] %s" % (time.monotonic() - start, text)
                print(line, flush=True)
                if log:
                    log.write(line + "\n")
                    log.flush()
                if until is not None and until.search(text):
                    end = 0
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        if log:
            log.close()


if __name__ == "__main__":
    main()
