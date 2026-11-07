#!/usr/bin/env python3
"""
bench_tls_flood.py - SEC-012 on a bench panel (docs/bugs/B033): flood its Panel Manager with TLS
handshakes from this PC and check that it stays on the network and its alarm-server link stays up.

    python tools/bench_tls_flood.py -p <PORT> --host 192.0.2.34 --seconds 120

Three floods run together: --clients connections that reconnect for every request (phones, a
browser gone wrong), --stalled connections that open TCP and never finish the handshake, and a
plain request now and then that has to get an answer. Meanwhile the service console is asked for
`server`, `memory` and `web` every few seconds. It passes when:
  - the link reports "connected" in every reading (when it was connected at the start),
  - internal RAM's minimum never falls below the gate's floor (web_tls_gate.h WEB_TLS_MIN_FREE),
  - the Wi-Fi co-processor never stops answering (no "stopped answering" line).
The longest wait for an answer is shown, not judged: this PC is the flood's address too, so its own
requests are rationed with it. Try a phone on the same Wi-Fi during the run to see another
address get in.
A bench panel only: never one that protects a house. Needs ESP-IDF's Python (pyserial).
"""

import argparse
import http.client
import re
import socket
import ssl
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: run this with ESP-IDF's Python")

MIN_FREE = 32768  # web_tls_gate.h WEB_TLS_MIN_FREE
CTX = ssl._create_unverified_context()


class Flood:
    def __init__(self, host, seconds):
        self.host = host
        self.end = time.time() + seconds
        self.lock = threading.Lock()
        self.counts = {"answered": 0, "refused": 0, "stalled": 0}
        self.last_answer = time.time()

    def reconnecting(self):
        while time.time() < self.end:
            try:
                c = http.client.HTTPSConnection(self.host, 443, context=CTX, timeout=5)
                c.request("GET", "/api/v1/status")
                c.getresponse().read()
                c.close()
                with self.lock:
                    self.counts["answered"] += 1
            except (OSError, http.client.HTTPException):
                with self.lock:
                    self.counts["refused"] += 1

    def stalled(self):
        while time.time() < self.end:
            try:
                s = socket.create_connection((self.host, 443), timeout=5)
                time.sleep(4)  # open, say nothing: the handshake waits for its timeout
                s.close()
                with self.lock:
                    self.counts["stalled"] += 1
            except OSError:
                time.sleep(0.2)

    def prober(self):
        while time.time() < self.end:
            try:
                c = http.client.HTTPSConnection(self.host, 443, context=CTX, timeout=10)
                c.request("GET", "/api/v1/status")
                c.getresponse().read()
                c.close()
                self.last_answer = time.time()
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(3)


def console(port, seconds, every):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = s.rts = False
    s.open()
    out, buf = [], b""
    end = time.time() + seconds
    nxt = 0.0
    while time.time() < end:
        if time.time() >= nxt:
            for cmd in ("server", "memory", "web"):
                s.write((cmd + "\r").encode())
                time.sleep(0.4)
            nxt = time.time() + every
        buf += s.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            out.append((time.time(), raw.decode("utf-8", "replace").rstrip("\r")))
    s.close()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("-p", "--port", required=True)
    ap.add_argument("--host", required=True)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--clients", type=int, default=16)
    ap.add_argument("--stalled", type=int, default=4)
    args = ap.parse_args()

    f = Flood(args.host, args.seconds)
    threads = [threading.Thread(target=f.reconnecting) for _ in range(args.clients)]
    threads += [threading.Thread(target=f.stalled) for _ in range(args.stalled)]
    threads.append(threading.Thread(target=f.prober))
    gaps = []

    def watch_answers():
        while time.time() < f.end:
            gaps.append(time.time() - f.last_answer)
            time.sleep(1)

    threads.append(threading.Thread(target=watch_answers))
    for t in threads:
        t.daemon = True
        t.start()
    lines = console(args.port, args.seconds, 6)
    for t in threads:
        t.join(timeout=10)

    link = [ln for _, ln in lines if re.match(r"^(connected|idle|connecting|showing a code|claimed|offline)", ln)]
    mins = [int(m.group(1)) for _, ln in lines for m in [re.search(r"internal: .*minimum=(\d+)", ln)] if m]
    gate = [ln for _, ln in lines if ln.startswith("WEB handshakes")]
    silent = [ln for _, ln in lines if "stopped answering" in ln]
    print("flood:", f.counts)
    print("link readings:", " ".join(sorted(set(link))) or "none", "(%d)" % len(link))
    print("internal minimum:", min(mins) if mins else "?", "bytes")
    print("gate:", gate[-1] if gate else "?")
    print("longest wait for an answer from this PC (rationed with the flood): %.0f s" % (max(gaps) if gaps else 0))
    failures = []
    if link and link[0] == "connected" and any(not ln.startswith("connected") for ln in link):
        failures.append("the alarm-server link dropped")
    if mins and min(mins) < MIN_FREE:
        failures.append("internal RAM fell below the gate's floor")
    if silent:
        failures.append("the Wi-Fi co-processor stopped answering")
    print("PASS" if not failures else "FAIL: " + "; ".join(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
