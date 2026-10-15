#!/usr/bin/env python3
"""zwtrace_scrub.py - make a recording of the panel's Z-Wave link safe to commit (docs/features/36
"Test and acceptance"; components/core/zwave_zones/test/traces/README.md).

    python tools/zwtrace_scrub.py <in.zwtrace> <out.zwtrace>

Reads a "zwtrace 1" recording - what panelctl's `zwave trace` and `zwave check` write from the
developer link (docs/features/30 "Z-Wave") - and writes it again with nothing that identifies the
home or its devices, so it can become a host-test fixture:

  - The network's home ID, wherever it appears in a frame or a run of text. It is learned from the
    GetControllerId answers (MemoryGetId, FUNC 20: the one place the Serial API says it in plain,
    serial-api.md §6.9), and each different one gets a fictional one in turn: CAFE0001, CAFE0002...
    (a network reset gives the module a new home ID). A damaged or cut-off GetControllerId answer
    gets it by position.
  - A SmartStart device's NWI and authentication home IDs (ApplicationUpdate 85/86/87 and the
    SmartStart AddNodeToNetwork request, §6.13, §6.16): derived from its DSK, so they name the
    device; replaced by fictional ones that keep their relation.
  - An S2 Public Key Report's key (9F 08, in ApplicationCommandHandler, BridgeApplicationCommandHandler
    and SendData frames): a device's is its DSK, printed on its label. Replaced by a fixed fictional
    key; a blanked PIN (00 00 at the start) stays blank.
  - Credentials: a clear application command of a class that carries a PIN or a code (Entry
    Control 6F, User Code 63, User Credential 83, Door Lock Logging 4C, Security 0 98, Transport
    Service 55), also inside clear Multi Channel, Supervision, CRC-16 and Multi Command
    encapsulations, has its data after the command byte blanked (the panel's trace already does
    this since ZW-030; older recordings may not have).
  - Every frame it changes gets its checksum recomputed; a damaged frame stays damaged by the same
    difference, a cut-off one keeps its length. Nothing else in the entries changes.
  - Comments: only those known to carry nothing identifying stay ('# panel restarted', panelctl's
    tool, module and date lines, the fake's synthetic mark); a mark that it was scrubbed goes on
    the second line (the host tests refuse a fixture without it).

It refuses (exit status 2, a message naming the line) a file it cannot parse - it reads the format
as strictly as the host tests' reader (zwtrace.c) - and (exit status 3) writes nothing if a home ID
it replaced would still be in the output. Scrubbing a scrubbed file changes nothing.
"""
import argparse
import re
import sys
from pathlib import Path

VERSION = 1
MARK = "# scrubbed by zwtrace_scrub.py %d: home IDs, S2 keys and comments removed" % VERSION
MARK_PREFIX = "# scrubbed by zwtrace_scrub.py"

HOME_BASE = 0xCAFE0000          # the fictional home IDs: CAFE0001, CAFE0002...
SMARTSTART_CORE = (0x00, 0xD5, 0x4B)  # fictional DSK bytes 8-10; byte 11 is 2 x the device's turn
FAKE_KEY = bytes(range(0xA0, 0xC0))   # 32 bytes, plainly not a real key

ENTRY_MAX = 1024                # bytes in one entry (zwtrace.h ZWT_ENTRY_MAX)
DIRS = ("rx", "tx", "crx", "ctx", "text")
DECIMAL = re.compile(r"[0-9]{1,20}")

# Comments that carry nothing identifying (full match on the stripped line).
SAFE_COMMENTS = [re.compile(p) for p in (
    r"# panel restarted",
    r"# panelctl, RoboAlarms [0-9A-Za-z.+_-]+",
    r"# module firmware [0-9.]* sdk [0-9.]* chip -?[0-9]+",
    r"# captured [0-9]{4}-[0-9]{2}-[0-9]{2}",
    r"# synthetic, recorded by test_zwz_replay from the fake ZAC93",
)]

FUNC_APP_COMMAND = 0x04
FUNC_SERIAL_API_STARTED = 0x0A
FUNC_SERIAL_API_SETUP = 0x0B
FUNC_SEND_DATA = 0x13
FUNC_GET_CONTROLLER_ID = 0x20
FUNC_APP_UPDATE = 0x49
FUNC_ADD_NODE = 0x4A
FUNC_BRIDGE_APP_COMMAND = 0xA8
SETUP_SET_NODE_ID_TYPE = 0x80
SMARTSTART_UPDATES = (0x85, 0x86, 0x87)
ADD_NODE_SMARTSTART_DSK = 0x08


SENSITIVE_CCS = (0x6F, 0x63, 0x83, 0x4C, 0x98, 0x55)


def blank_credentials(f, start, stop, depth=0):
    """zw_redact.c's rule on f[start:stop] (a clear application command), in place: the number of
    bytes blanked."""
    if stop - start < 2:
        return 0
    if depth >= 4:
        n = sum(1 for b in f[start + 1:stop] if b)
        f[start + 1:stop] = bytes(stop - start - 1)
        return n
    cc, cmd = f[start], f[start + 1]
    if cc in SENSITIVE_CCS:
        n = sum(1 for b in f[start + 2:stop] if b)
        f[start + 2:stop] = bytes(stop - start - 2)
        return n
    if (cc, cmd) in ((0x60, 0x0D), (0x6C, 0x01)) and stop - start > 4:
        return blank_credentials(f, start + 4, stop, depth + 1)
    if (cc, cmd) == (0x56, 0x01) and stop - start > 4:
        return blank_credentials(f, start + 2, stop - 2, depth + 1)
    if (cc, cmd) == (0x8F, 0x01) and stop - start > 3:
        total, at = 0, start + 3
        for _ in range(f[start + 2]):
            if at >= stop:
                break
            n = f[at]
            if n == 0 or at + 1 + n > stop:
                total += sum(1 for b in f[at:stop] if b)
                f[at:stop] = bytes(stop - at)
                return total
            total += blank_credentials(f, at + 1, at + 1 + n, depth + 1)
            at += 1 + n
        return total
    return 0


class Refused(Exception):
    """The file cannot be read as a zwtrace 1 recording."""


class Entry:
    def __init__(self, seq, t, direction, data):
        self.seq, self.t, self.dir, self.data = seq, t, direction, bytearray(data)

    def line(self):
        return "%d %d %s %s" % (self.seq, self.t, self.dir, self.data.hex())


def parse(text):
    """The file's lines as [("comment", text) | ("lost", n) | ("entry", Entry)]."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if not lines or lines[0].rstrip(" \t\r") != "zwtrace 1":
        raise Refused("line 1: not a zwtrace 1 recording (the first line must be \"zwtrace 1\")")
    out = []
    for number, raw in enumerate(lines[1:], start=2):
        line = raw.rstrip(" \t\r")
        stripped = line.lstrip(" \t")
        if not stripped:
            continue
        if stripped.startswith("#"):
            out.append(("comment", stripped))
            continue
        tokens = re.split(r"[ \t]+", stripped)
        if len(tokens) == 2 and tokens[0] == "lost":
            if not DECIMAL.fullmatch(tokens[1]) or int(tokens[1]) > 0xFFFFFFFF:
                raise Refused("line %d: \"lost\" needs a count" % number)
            out.append(("lost", int(tokens[1])))
            continue
        if len(tokens) != 4:
            raise Refused("line %d: expected <seq> <t_ms> <dir> <hex>, \"lost <n>\" or a # comment"
                          % number)
        seq, t, direction, hexa = tokens
        if not (DECIMAL.fullmatch(seq) and DECIMAL.fullmatch(t)) or int(seq) > 0xFFFFFFFF \
                or int(t) >= 1 << 64:
            raise Refused("line %d: the sequence number or the time is not a number" % number)
        if direction not in DIRS:
            raise Refused("line %d: the direction is none of %s" % (number, ", ".join(DIRS)))
        if len(hexa) % 2 or not re.fullmatch(r"[0-9A-Fa-f]+", hexa):
            raise Refused("line %d: the bytes are not whole hex pairs" % number)
        data = bytes.fromhex(hexa)
        if len(data) > ENTRY_MAX:
            raise Refused("line %d: an entry longer than %d bytes" % (number, ENTRY_MAX))
        if direction in ("crx", "ctx") and len(data) != 1:
            raise Refused("line %d: an ACK, NAK or CAN is one byte" % number)
        out.append(("entry", Entry(int(seq), int(t), direction, data)))
    return out


# ---- frames ----------------------------------------------------------------------------------

def checksum(frame):
    """serial-api.md §1: FF XOR every byte from LEN through the last payload byte."""
    c = 0xFF
    for b in frame[1:-1]:
        c ^= b
    return c


def whole(frame):
    """SOF, a LEN of at least 3, LEN + 2 bytes: a frame with a checksum byte (good or not)."""
    return len(frame) >= 5 and frame[0] == 0x01 and frame[1] >= 3 and len(frame) == frame[1] + 2


def good(frame):
    return whole(frame) and checksum(frame) == frame[-1]


class Scrubber:
    def __init__(self):
        self.homes = {}          # real home ID bytes -> fictional
        self.devices = {}        # a SmartStart device's DSK core -> its fictional core
        self.places = 0          # home IDs replaced
        self.keys = 0            # S2 public keys replaced
        self.smartstart = 0      # SmartStart home IDs replaced
        self.dropped = 0         # comment lines dropped
        self.credentials = 0     # clear commands whose credentials were blanked

    # -- what it learns first --

    def learn(self, entries):
        for e in entries:
            f = e.data
            if e.dir == "rx" and good(f) and f[2] == 0x01 and f[3] == FUNC_GET_CONTROLLER_ID \
                    and len(f) >= 10:
                self.fictional_home(bytes(f[4:8]))

    def fictional_home(self, real):
        if real not in self.homes:
            self.homes[real] = (HOME_BASE + len(self.homes) + 1).to_bytes(4, "big")
        return self.homes[real]

    # -- the replacements --

    def replace_homes(self, buf, start, end):
        """Every learned home ID inside buf[start:end], in place."""
        for real, fake in self.homes.items():
            i = buf.find(real, start, end)
            while i >= 0:
                if real != fake:
                    buf[i:i + 4] = fake
                    self.places += 1
                i = buf.find(real, i + 4, end)

    def controller_id_answer(self, f):
        """GetControllerId's answer by position, damaged or cut off as it may be."""
        n = min(8, len(f) - 1 if whole(f) else len(f))
        if n <= 4:
            return
        present = bytes(f[4:n])
        fake = next((v for k, v in self.homes.items() if k.startswith(present)), None)
        if fake is None:  # damaged in the home ID itself: this network's fictional one all the same
            fake = next(iter(self.homes.values()), (HOME_BASE + 1).to_bytes(4, "big"))
        if present != fake[:n - 4]:
            f[4:n] = fake[:n - 4]
            self.places += 1

    def smartstart_id(self, f, at):
        """A DSK-derived home ID at f[at:at+4] (NWI or authentication): the device's fictional one,
        with the same marker bits (NWI: top bits 11, low bit 0; authentication: 00 and 1)."""
        if at + 4 > len(f):
            return
        real = bytes(f[at:at + 4])
        core = (real[0] & 0x3F, real[1], real[2], real[3] & 0xFE)
        if core not in self.devices:
            turn = len(self.devices) + 1
            self.devices[core] = SMARTSTART_CORE + ((2 * turn) & 0xFE,)
        fc = self.devices[core]
        fake = bytes(((fc[0] | (real[0] & 0xC0)), fc[1], fc[2], (fc[3] | (real[3] & 0x01))))
        if fake != real:
            f[at:at + 4] = fake
            self.smartstart += 1

    def public_key(self, f, at, end):
        """An S2 Public Key Report (9F 08 | including | key (32)) starting at f[at], within end."""
        if at + 3 + 32 > end or f[at] != 0x9F or f[at + 1] != 0x08:
            return
        key = at + 3
        fake = bytearray(FAKE_KEY)
        if f[key] == 0 and f[key + 1] == 0:
            fake[0:2] = b"\x00\x00"  # the PIN left out for an authenticated class
        if f[key:key + 32] != fake:
            f[key:key + 32] = fake
            self.keys += 1

    def command(self, f, len_at, end):
        """A clear command whose length byte is f[len_at]: its credentials blanked."""
        if len_at + 3 > end:
            return
        stop = min(len_at + 1 + f[len_at], end)
        if blank_credentials(f, len_at + 1, stop):
            self.credentials += 1

    def frame(self, e, nid):
        f = e.data
        if len(f) < 4 or f[0] != 0x01:
            self.replace_homes(f, 0, len(f))  # not a frame: look everywhere
            return
        original = bytes(f)
        was_whole = whole(f)
        end = len(f) - 1 if was_whole else len(f)  # the payload ends before the checksum
        rtype, func = f[2], f[3]
        if e.dir == "rx" and rtype == 0x01 and func == FUNC_GET_CONTROLLER_ID:
            self.controller_id_answer(f)
        if e.dir == "rx" and rtype == 0x00 and func == FUNC_APP_UPDATE and len(f) > 5 \
                and f[4] in SMARTSTART_UPDATES:
            self.smartstart_id(f, 4 + 1 + nid + 1)
        if e.dir == "tx" and rtype == 0x00 and func == FUNC_ADD_NODE and len(f) > 5 \
                and f[4] & 0x0F == ADD_NODE_SMARTSTART_DSK:
            self.smartstart_id(f, 6)
            self.smartstart_id(f, 10)
        if e.dir == "rx" and rtype == 0x00 and func == FUNC_APP_COMMAND:
            self.public_key(f, 4 + 1 + nid + 1, end)
            self.command(f, 4 + 1 + nid, end)
        if e.dir == "rx" and rtype == 0x00 and func == FUNC_BRIDGE_APP_COMMAND:
            self.public_key(f, 4 + 1 + 2 * nid + 1, end)
            self.command(f, 4 + 1 + 2 * nid, end)
        if e.dir == "tx" and rtype == 0x00 and func == FUNC_SEND_DATA:
            self.public_key(f, 4 + nid + 1, end)
            self.command(f, 4 + nid, end)
        self.replace_homes(f, 4, end)
        if was_whole and bytes(f) != original:
            # a good frame gets a good checksum; a damaged one stays damaged the same way
            f[-1] = checksum(f) ^ (original[-1] ^ checksum(original))

    def entries(self, entries):
        nid = 1  # the node-ID width: 2 after SetNodeIDType(16 bit), 1 again after a restart
        for e in entries:
            f = e.data
            if e.dir == "tx" and good(f) and f[3] == FUNC_SERIAL_API_SETUP and len(f) >= 7 \
                    and f[4] == SETUP_SET_NODE_ID_TYPE:
                nid = 2 if f[5] == 0x02 else 1
            if e.dir == "rx" and good(f) and f[2] == 0x00 and f[3] == FUNC_SERIAL_API_STARTED:
                nid = 1
            if e.dir in ("rx", "tx"):
                self.frame(e, nid)
            elif e.dir == "text":
                self.replace_homes(f, 0, len(f))

    def leaks(self, entries):
        """Where a replaced home ID would still be written, or None."""
        for e in entries:
            for real, fake in self.homes.items():
                if real != fake and real in e.data:
                    return "entry %d" % e.seq
        return None


def scrub(text):
    """The recording's text, scrubbed; and the scrubber (what it did)."""
    items = parse(text)
    entries = [v for k, v in items if k == "entry"]
    s = Scrubber()
    s.learn(entries)
    s.entries(entries)
    where = s.leaks(entries)
    if where is not None:
        raise LookupError("a home ID would still be in the output (%s)" % where)
    out = ["zwtrace 1", MARK]
    for kind, value in items:
        if kind == "entry":
            out.append(value.line())
        elif kind == "lost":
            out.append("lost %d" % value)
        elif value.startswith(MARK_PREFIX):
            continue  # an earlier run's mark: this run's is on line 2
        elif any(p.fullmatch(value) for p in SAFE_COMMENTS):
            out.append(value)
        else:
            s.dropped += 1
    return "\n".join(out) + "\n", s


def main(argv=None):
    ap = argparse.ArgumentParser(description="Make a zwtrace recording safe to commit.")
    ap.add_argument("input", help="the recording (panelctl zwave trace / zwave check)")
    ap.add_argument("output", help="the scrubbed copy to write")
    args = ap.parse_args(argv)
    try:
        text = Path(args.input).read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        print("zwtrace_scrub: %s: cannot read it: %s" % (args.input, exc), file=sys.stderr)
        return 2
    try:
        result, s = scrub(text)
    except Refused as exc:
        print("zwtrace_scrub: %s: refused, %s" % (args.input, exc), file=sys.stderr)
        return 2
    except LookupError as exc:
        print("zwtrace_scrub: %s: refused, %s; nothing written" % (args.input, exc),
              file=sys.stderr)
        return 3
    with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(result)
    print("zwtrace_scrub: %s -> %s: %d home ID(s) found, %d place(s) changed, %d S2 key(s), "
          "%d SmartStart ID(s), %d credential command(s) blanked, %d comment line(s) dropped"
          % (args.input, args.output, len(s.homes), s.places, s.keys, s.smartstart,
             s.credentials, s.dropped))
    if not s.homes:
        print("zwtrace_scrub: note: no GetControllerId answer in it, so no home ID was learned "
              "(a recording from `panelctl zwave check` has one)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
