"""A pretend desk of boards for the --mock window and the tests: ports, serial lines and chips
that behave like the real ones, driven by the fake flash contents. Install an image onto the
factory CrowPanel and it boots, answers `dev describe` with the new version, and its 433 MHz
receiver reports sensors, exactly through the code paths real hardware uses.

    python tools/flasher/run.py --mock
"""
import json
import random
import threading
import time
from types import SimpleNamespace

from .fakechip import FakeBoard, FakeSession
from .boards import ChipInfo
from .imagefile import parse_app
from .jobs import version_tuple as _vt

MB = 1024 * 1024
# What each CrowPanel P4 board's firmware answers with (its board_pins.h): the J7 place of the
# clock and the Z-Wave radio, the socket's UART, J7's IO pins a custom place may use (FLS-029)
# and its I2C connector. Board V1.2 (the 10.1") swapped J7's 27/28 with the socket's 53/54.
P4_PINS = {
    "crowpanel_p4": {"j7": (27, 28), "socket": (53, 54), "reach": (2, 3, 4, 5, 27, 28, 49, 50, 51, 52),
                     "i2c": "I2C connector (PH2.0, 4 pins)"},
    "crowpanel_10": {"j7": (53, 54), "socket": (27, 28), "reach": (2, 3, 4, 5, 49, 50, 51, 52, 53, 54),
                     "i2c": "I2C connector J13 (HY2.0, 4 pins)"},
}
CUSTOM_KEYS = {"rf": (("sck", "mosi", "miso", "cs", "gdo0"), ("data",)), "rtc": (("sda", "scl"),),
               "zwave": (("tx", "rx"), ("tx", "rx", "reset"))}
PART_NAME = {"rf": "433 MHz receiver", "rtc": "clock module", "zwave": "Z-Wave radio"}
# What a real panel's `zigbee status` prints as version= (the RCP's Spinel version, with spaces)
MOCK_RCP_VERSION = "openthread-esp32/0000000-0000000; esp32h2;  2026-10-01 00:00:00 UTC"


class SerialTimeoutException(OSError):
    """pyserial's write timeout, by name (serialio.is_write_timeout)."""


class MockSerialException(OSError):
    pass


class MockDevice:
    def __init__(self, port, description, vid, pid, chip=None, board_id="", board_name="",
                 armed=False, flash=None, bluetooth=False, sensors=None, clock="ok", custom_pins=True,
                 radio_module=False):
        self.port = port
        self.radio_module = radio_module   # a chip on its own USB Serial/JTAG, no RoboAlarms console (FLS-030)
        self.zigbee_version = ""           # what a panel's `zigbee status` reports: the bundle's radio
        self.raw = SimpleNamespace(device=port, description=description, vid=vid, pid=pid,
                                   manufacturer="wch.cn" if vid == 0x1A86 else "", serial_number="",
                                   location="1-5.2", hwid="")
        self.board_id = board_id
        self.board_name = board_name
        self.armed = armed
        self.bluetooth = bluetooth
        self.sensors = sensors or []
        self.board = None
        if chip is not None:
            self.board = MockBoard(self, chip, flash=flash, seconds_per_mb=0.5)
        self.lock = threading.Condition()
        self.out = bytearray()
        self.pending = []           # [(release_at, bytes)]
        self.inbuf = b""
        self.holder = None
        self.raw_on = False
        self.next_rf = 0.0
        self.booted_at = time.monotonic()
        self.rf_pin_stored = None     # `rf pin`, kept like NVS across restarts
        self.rf_pin = None            # what the running firmware listens on
        self.rf_module_stored = "auto"   # `rf module`, kept like NVS
        self.rf_module = "auto"
        self.rf_place_stored = "default"  # `rf place`, kept like NVS
        self.rf_place = "default"
        self.clock_place_stored = "default"  # `rtc place`
        self.clock_place = "default"
        self.zwave_place_stored = None     # `zwave place`: None = off
        self.zwave_place = None
        self.zwave_fitted = True           # a ZAC93 answers wherever the panel looks for it
        self.custom_ok = custom_pins       # firmware that takes custom places (FLS-029); False: older
        self.custom_stored = {}            # part ("rf", "rtc", "zwave") -> {key: gpio}, kept like NVS
        self.custom = {}                   # what the running firmware uses
        self.custom_why = {}               # part -> why its stored custom place was refused at start
        self.clock_module = clock      # a DS3231 on the I2C connector: ok, lost, failed, absent
        self.clock_offset = -37.0      # the panel's clock minus this computer's, seconds
        self.clock_source = "module" if clock == "ok" else "none"

    # -- what runs on it ------------------------------------------------------------------------
    def installed(self):
        if self.board is None:
            return None
        for off in (0x30000, 0x20000):
            try:
                a = parse_app(bytes(self.board.flash[off:off + 512]))
            except ValueError:
                continue
            board = a.project[len("alarm_"):].split("-", 1)[0] if a.project.startswith("alarm_") else ""
            profile = a.project.rsplit("-", 1)[1] if "-" in a.project else "legacy"
            return {"project": a.project, "version": a.version, "board": board, "profile": profile,
                    "elf": a.elf_sha256}
        return None

    # -- the wire -------------------------------------------------------------------------------
    def emit(self, text, delay=0.0):
        with self.lock:
            data = text.encode()
            if delay:
                self.pending.append((time.monotonic() + delay, data))
            else:
                self.out += data
            self.lock.notify_all()

    def boot(self):
        with self.lock:
            self.out.clear()
            self.pending.clear()
            self.raw_on = False
        self.booted_at = time.monotonic()
        inst = self.installed()
        if self.board is None:
            return
        own = 10 if inst and inst["board"] in P4_PINS else 35
        self.rf_place = self.rf_place_stored
        self.clock_place = self.clock_place_stored
        self.zwave_place = self.zwave_place_stored
        if self.rf_place == "j7":
            self.rf_pin = 5
        else:
            self.rf_pin = self.rf_pin_stored if self.rf_pin_stored is not None else own
        self.rf_module = self.rf_module_stored
        # Custom places (FLS-029): checked again at start; a refused one leaves its part off.
        self.custom, self.custom_why = {}, {}
        for part, pins in self.custom_stored.items():
            why = self._custom_refusal(part, pins)
            if why:
                self.custom_why[part] = why
            else:
                self.custom[part] = pins
        if self.rf_place == "custom":
            pins = self.custom.get("rf")
            self.rf_module = "off" if pins is None else ("cc1101" if "sck" in pins else "ook")
            self.rf_pin = pins.get("data", pins.get("gdo0")) if pins else None
        if self.clock_place == "custom" and "rtc" not in self.custom:
            self.clock_place = "off"
        if self.zwave_place == "custom" and "zwave" not in self.custom:
            self.zwave_place = None
        chip = self.board.info
        self.emit(f"ESP-ROM:{chip.chip}-20230612\r\nrst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)\r\n", 0.05)
        if self.radio_module:
            self.emit("I (25) main_task: Calling app_main()\r\n", 0.3)
            return      # an RCP or another program: nothing answers a console
        if inst is None:
            self.emit("vendor demo v1.0 starting\r\n", 0.4)
            return
        self.emit(f"I (812) main: chip revision: v{chip.revision // 100}.{chip.revision % 100}\r\n", 0.3)
        self.emit(f"I (905) main: board: {self.board_name} ({inst['board']})\r\n", 0.6)
        self.emit("I (1502) alarm: engine running: 64 zones, 32 users\r\n", 1.0)
        self.emit("W (1780) wifi: not configured\r\n", 1.2)
        self.emit(f"I (2410) main: {self.board_name} running: internal RAM free 275 KB (largest block 176 KB)\r\n", 1.8)
        self.emit("alarm> ", 1.9)

    def pump(self):
        now = time.monotonic()
        with self.lock:
            due = [p for p in self.pending if p[0] <= now]
            if due:
                self.pending = [p for p in self.pending if p[0] > now]
                for _, data in sorted(due):
                    self.out += data
        inst = self.installed()
        if self.raw_on and inst and self.sensors and now >= self.next_rf:
            self.next_rf = now + random.uniform(1.5, 3.5)
            device, code = random.choice(self.sensors)
            ms = int((now - self.booted_at) * 1000)
            self.emit(f"I ({ms}) rf: EV1527 {device} code {code:X} ({device}{code:X}, T {random.randint(330, 370)} us)\r\n")

    def read(self, n, timeout):
        deadline = time.monotonic() + (timeout or 0)
        while True:
            self.pump()
            with self.lock:
                if self.out:
                    data = bytes(self.out[:n])
                    del self.out[:n]
                    return data
                left = deadline - time.monotonic()
                if left <= 0:
                    return b""
                self.lock.wait(min(left, 0.05))

    def write(self, data):
        self.inbuf += data
        while b"\r" in self.inbuf:
            line, self.inbuf = self.inbuf.split(b"\r", 1)
            self.command(line.decode(errors="replace").strip())

    # -- custom places (FLS-029: the console contract) ------------------------------------------
    def _pins(self):
        inst = self.installed()
        return P4_PINS.get(inst["board"] if inst else "", P4_PINS["crowpanel_p4"])

    def _taken(self, part):
        """{gpio: what} for the pins the other parts' stored places take."""
        out = {}
        j7 = self._pins()["j7"]
        if part != "rf":
            if self.rf_place_stored == "custom":
                out.update({g: f"433 MHz receiver {k.upper()}" for k, g in self.custom_stored.get("rf", {}).items()})
            elif self.rf_place_stored == "j7":
                out.update({52: "CC1101 SCK", 51: "CC1101 MOSI", 50: "CC1101 MISO", 49: "CC1101 CS", 5: "CC1101 GDO0"})
            elif self.rf_pin_stored is not None:
                out[self.rf_pin_stored] = "433 MHz receiver DATA"
        if part != "rtc":
            if self.clock_place_stored == "custom":
                out.update({g: f"clock module {k.upper()}" for k, g in self.custom_stored.get("rtc", {}).items()})
            elif self.clock_place_stored == "j7":
                out.update(dict(zip(j7, ("clock module SDA", "clock module SCL"))))
        if part != "zwave":
            if self.zwave_place_stored == "custom":
                out.update({g: f"Z-Wave radio {k.upper()}" for k, g in self.custom_stored.get("zwave", {}).items()})
            elif self.zwave_place_stored == "j7":
                out.update(dict(zip(j7, ("Z-Wave radio TX", "Z-Wave radio RX"))))
        return out

    def _custom_parse(self, part, words):
        """`k=g ...` as the panel reads it: ({key: gpio} in the console's order, None) or (None, why):
        the keys, then reachable pins, each once, none another part's stored place takes."""
        seen = {}
        for w in words:
            k, eq, v = w.partition("=")
            if not eq or k not in {x for keys in CUSTOM_KEYS[part] for x in keys}:
                return None, f"{w} is not one of its keys"
            if k in seen:
                return None, f"{k}= is given twice"
            if not v.isdigit():
                return None, f"{k}= is not a pin number"
            seen[k] = int(v)
        keys = next((keys for keys in CUSTOM_KEYS[part] if set(keys) == set(seen)), None)
        if keys is None:
            want = min(CUSTOM_KEYS[part], key=lambda keys: len(set(keys) ^ set(seen)))
            return None, "it needs " + " ".join(f"{k}=" for k in want)
        reach = self._pins()["reach"]
        for k in keys:
            if seen[k] not in reach:
                return None, (f"GPIO {seen[k]} can't be reached on this board (J7: "
                              + ", ".join(f"IO{p}" for p in reach) + ")")
        by = {}
        for k in keys:
            by.setdefault(seen[k], []).append(k.upper())
        for g, used in by.items():
            if len(used) > 1:
                return None, f"GPIO {g} is used twice ({' and '.join(used)})"
        taken = self._taken(part)
        for k in keys:
            if seen[k] in taken:
                return None, f"GPIO {seen[k]} is the {taken[seen[k]]}"
        return {k: seen[k] for k in keys}, None

    def _custom_refusal(self, part, pins):
        """Why a stored custom place is not used at start, or None."""
        return self._custom_parse(part, [f"{k}={v}" for k, v in pins.items()])[1]

    def _custom_command(self, part, words):
        """`<part> place custom k=g ...`: stored when the panel accepts it."""
        pins, why = self._custom_parse(part, words)
        if why:
            return "not custom: " + why
        self.custom_stored[part] = pins
        return "place custom from the next restart (`restart`)"

    def _custom_line(self, part):
        pins = self.custom_stored.get(part)
        return "custom " + " ".join(f"{k}={v}" for k, v in pins.items()) if pins else None

    def _custom_lines(self, part, say):
        line = self._custom_line(part)
        if line:
            say(line)
        if part in self.custom_why:
            say(f"custom place not used: {self.custom_why[part]}")

    # -- the console ----------------------------------------------------------------------------
    def command(self, cmd):
        inst = self.installed()
        if inst is None or (time.monotonic() - self.booted_at) < 1.9:
            return      # a factory demo (or firmware still starting) has no console
        say = lambda text: self.emit(text + "\r\n")  # noqa: E731
        self.emit(cmd + "\r\n")                       # the console echoes
        if cmd == "dev describe":
            if inst["board"] not in P4_PINS:
                say("Unrecognized command")
            else:
                info = {"api": 1, "id": "e8f60a" + self.port[-2:].rjust(6, "0"), "board": inst["board"],
                        "profile": inst["profile"], "chip_revision": self.board.info.revision,
                        "version": inst["version"], "elf": inst["elf"], "idle": not self.armed,
                        "probation": False, "operation": ""}
                say("DEV INFO " + json.dumps(info, separators=(",", ":")))
        elif cmd == "status":
            say('Partition 1 "House": ' + ("armed away" if self.armed else "disarmed"))
            say("  READY, chime on, sounder quiet")
            say("Network: offline  , time not set")
        elif cmd == "ota":
            say(f"Firmware: {inst['version']}")
        elif cmd.startswith("rf pin"):
            own, offered = (10, [2, 3, 4, 5, 49, 50, 51, 52]) if inst["board"] in P4_PINS else (35, [39, 25, 32])
            arg = cmd.split()[2] if len(cmd.split()) > 2 else ""
            if arg == "default":
                self.rf_pin_stored = None
                say(f"data on GPIO {own} (wireless-module socket) from the next restart (`restart`)")
            elif arg.isdigit() and self.custom_ok and int(arg) in self._taken("rf"):
                say(f"not GPIO {arg}: it is the {self._taken('rf')[int(arg)]}")
            elif arg.isdigit() and (int(arg) in offered or int(arg) == own):
                self.rf_pin_stored = None if int(arg) == own else int(arg)
                say(f"data on GPIO {arg} from the next restart (`restart`); it is no longer offered as a wired terminal")
            elif arg.isdigit():
                say(f"not GPIO {arg}: this board offers GPIO {own} (wireless-module socket) or a J7 header terminal (`terminals`)")
            else:
                say(f"data on GPIO {self.rf_pin} (wireless-module socket)")
        elif cmd.startswith("rf module"):
            arg = cmd.split()[2] if len(cmd.split()) > 2 else ""
            if arg in ("auto", "cc1101", "ook") and self.rf_place_stored == "custom":
                say(f"not {arg}: the receiver is on your own pins (`rf place custom`): `rf place default` first")
            elif arg in ("auto", "cc1101", "ook", "off"):
                if arg == "off" and self.rf_place_stored == "custom":
                    self.rf_place_stored = "default"
                    self.custom_stored.pop("rf", None)
                self.rf_module_stored = arg
                say(f"receiver {arg} from the next restart (`restart`)")
            else:
                say("rf module auto|cc1101|ook|off")
        elif cmd.startswith("rf place"):
            arg = cmd.split()[2] if len(cmd.split()) > 2 else ""
            custom = self.custom_ok and inst["board"] in P4_PINS
            if arg == "default":
                self.rf_place_stored = "default"
                self.custom_stored.pop("rf", None)
                say("place default (wireless-module socket) from the next restart (`restart`)")
            elif arg == "j7" and custom and {52, 51, 50, 49, 5} & set(self._taken("rf")):
                g = min({52, 51, 50, 49, 5} & set(self._taken("rf")))
                say(f"not j7: GPIO {g} is the {self._taken('rf')[g]}")
            elif arg == "j7" and inst["board"] in P4_PINS:
                self.rf_place_stored = "j7"
                self.custom_stored.pop("rf", None)
                say("place j7 from the next restart (`restart`); its pins are no longer offered as wired terminals")
            elif arg == "custom" and custom:
                reply = self._custom_command("rf", cmd.split()[3:])
                if reply.startswith("place "):
                    self.rf_place_stored, self.rf_module_stored = "custom", "off"
                say(reply)
            elif arg:
                say(f"not {arg}: this board has no place called {arg} (`rf place` lists them)")
            else:
                say(f"place {self.rf_place}: " + {"j7": "J7 header", "custom": "your own pins"}.get(
                    self.rf_place, "wireless-module socket"))
                if self.rf_place_stored != self.rf_place:
                    say(f"place {self.rf_place_stored} from the next restart")
                say("default: wireless-module socket")
                if inst["board"] in P4_PINS:
                    say("j7: J7 header, IO52 (SCK), IO51 (MOSI), IO50 (MISO), IO49 (CS), IO5 (GDO0)")
                if custom:
                    say("custom: your own pins on J7 (`rf place custom sck= mosi= miso= cs= gdo0=` or "
                        "`rf place custom data=`)")
                    self._custom_lines("rf", say)
                say("`rf place <place>` moves the receiver there, `rf place default` puts it back")
        elif cmd in ("rf", "rf status"):
            gpio = self.rf_pin
            edges = random.randint(1700, 2100)
            high = random.randint(38, 46)
            if self.rf_module == "off":
                say("433 MHz receiver: off (`rf module` turns it on)")
                self._custom_lines("rf", say)
                say(f"module=off receiver=none edges/s=0 high=0% storms=0 place={self.rf_place}")
                say("cc1101 version=0x00 state=-(0xFF) spi_errors=0")
            elif self.rf_module == "cc1101":
                say(f"CC1101 at 433.920 MHz, rx, noise {random.randint(-100, -94)} dBm")
                self._custom_lines("rf", say)
                say(f"module=cc1101 receiver=CC1101 edges/s={edges} high={high}% storms=0 place={self.rf_place}")
                say("cc1101 version=0x14 state=rx(0x0D) spi_errors=0")
            else:
                say(f"Receiver module on GPIO {gpio}, {edges} edges/s, line {high}% high")
                self._custom_lines("rf", say)
                say(f"module={self.rf_module} receiver=receiver module edges/s={edges} high={high}% storms=0 "
                    f"place={self.rf_place}")
            say("capture receptions=412 symbols=98231 overruns=0")
            say("decoder pulses=98231 frames=36 rejected=4 glitches=112")
            say("source transmissions=9 unknown=2 bursts=9 learning=0 jammed=0 confirmed=7 replays=0")
        elif cmd == "rf raw on":
            self.raw_on = True
            self.next_rf = time.monotonic() + 1.0
            say("raw on: every transmission is logged")
        elif cmd == "rf raw off":
            self.raw_on = False
            say("raw off")
        elif cmd.startswith("rf capture"):
            try:
                ms = int(cmd.split()[2])
            except (IndexError, ValueError):
                ms = 3000
            say(f"capturing {ms} ms: trigger the sensor now")
            pulses = ["H350", "L10850"] + [f"{'H' if i % 2 == 0 else 'L'}{random.choice((350, 1050))}"
                                           for i in range(48)]
            rows = [" ".join(pulses[i:i + 16]) for i in range(0, len(pulses), 16)]
            text = f"pulses {len(pulses)} (H high, L low, microseconds; 0 = the line went quiet)\r\n"
            text += "\r\n".join(rows) + "\r\n"
            self.emit(text, ms / 1000 + 0.3)
        elif cmd in ("rtc", "rtc status"):
            pins = self._pins()
            now = time.time() + self.clock_offset
            known = self.clock_source != "none"
            where = {"j7": "J7 header, IO{} (SDA) and IO{} (SCL)".format(*pins["j7"]), "custom": "your own pins"}.get(
                self.clock_place, pins["i2c"])
            words = {"ok": "keeping time, 24.25 C", "lost": "lost the time, 23.75 C", "failed": "not answering",
                     "absent": f"not fitted (none answered on the {where} at start-up)"}
            module = "absent" if self.clock_place == "off" else self.clock_module
            say("Clock module: " + ("off: its custom place is not used" if self.clock_place == "off"
                                    else words[self.clock_module]))
            self._custom_lines("rtc", say)
            temp = {"ok": 97, "lost": 95}.get(module, 0)
            say(f"rtc module={module} source={self.clock_source} trusted=0 lost_at_start="
                f"{int(self.clock_module == 'lost')} temp_c4={temp} offset_ms=? writes=0 write_failures=0 "
                f"slews=0 jumps=0 epoch={int(now) if known else 0} "
                f"place={'custom' if self.clock_place == 'off' else self.clock_place}")
            say("panel clock " + (time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(now)) if known else "not set")
                + f" ({self.clock_source})")
        elif cmd.startswith("rtc place"):
            arg = cmd.split()[2] if len(cmd.split()) > 2 else ""
            pins = self._pins()
            j7 = set(pins["j7"])
            custom = self.custom_ok and inst["board"] in P4_PINS
            if arg == "custom" and custom:
                reply = self._custom_command("rtc", cmd.split()[3:])
                if reply.startswith("place "):
                    self.clock_place_stored = "custom"
                say(reply)
            elif arg == "default":
                self.clock_place_stored = "default"
                self.custom_stored.pop("rtc", None)
                say(f"place default ({pins['i2c']}) from the next restart (`restart`)")
            elif arg == "j7" and inst["board"] in P4_PINS and self.zwave_place_stored == "j7":
                say(f"not j7: GPIO {pins['j7'][0]} (Z-Wave radio TX) is used twice")
            elif arg == "j7" and inst["board"] in P4_PINS and (j7 & set(self._taken("rtc"))):
                g = min(j7 & set(self._taken("rtc")))
                say(f"not j7: GPIO {g} is the {self._taken('rtc')[g]}")
            elif arg == "j7" and inst["board"] in P4_PINS:
                self.clock_place_stored = "j7"
                self.custom_stored.pop("rtc", None)
                say("place j7 from the next restart (`restart`)")
            elif arg:
                say(f"not {arg}: this board has no place called {arg} (`rtc place` lists them)")
            else:
                say(f"place {self.clock_place}: " + {"j7": "J7 header, IO{} (SDA) and IO{} (SCL)".format(*pins["j7"]),
                                                     "custom": "your own pins"}.get(
                    self.clock_place, pins["i2c"]))
                if custom:
                    say("custom: your own pins on J7 (`rtc place custom sda= scl=`)")
                    self._custom_lines("rtc", say)
        elif cmd.startswith("zwave place") and inst["board"] in P4_PINS:
            arg = cmd.split()[2] if len(cmd.split()) > 2 else ""
            pins = self._pins()
            j7 = set(pins["j7"])
            where = {"socket": "wireless-module socket", "j7": "J7 header, IO{} (TX) and IO{} (RX)".format(*pins["j7"])}
            custom = self.custom_ok
            if arg == "custom" and custom:
                reply = self._custom_command("zwave", cmd.split()[3:])
                if reply.startswith("place "):
                    self.zwave_place_stored = "custom"
                say(reply)
            elif arg == "off":
                self.zwave_place_stored = None
                self.custom_stored.pop("zwave", None)
                say("place off from the next restart (`restart`): no pin is touched")
            elif arg == "j7" and self.clock_place_stored == "j7":
                say(f"not j7: GPIO {pins['j7'][0]} (clock module SDA) is used twice")
            elif arg == "j7" and (j7 & set(self._taken("zwave"))):
                g = min(j7 & set(self._taken("zwave")))
                say(f"not j7: GPIO {g} is the {self._taken('zwave')[g]}")
            elif arg in where:
                self.zwave_place_stored = arg
                self.custom_stored.pop("zwave", None)
                say(f"place {arg} from the next restart (`restart`); a reset line stays where it was "
                    "(`zwave reset-pin`)")
            elif arg:
                say(f"not {arg}: this board has no place called {arg} (`zwave place` lists them)")
            else:
                say(f"place {self.zwave_place}: {dict(where, custom='your own pins')[self.zwave_place]}"
                    if self.zwave_place else "place off")
                for k, v in where.items():
                    say(f"{k}: {v} (TX GPIO {pins[k][0]}, RX {pins[k][1]})")
                if custom:
                    say("custom: your own pins on J7 (`zwave place custom tx= rx= [reset=]`)")
                    self._custom_lines("zwave", say)
                say("`zwave place <place>` wires it there, `zwave place off` takes it away")
        elif cmd == "zwave status" and inst["board"] in P4_PINS:
            if self.zwave_place is None:
                say("Z-Wave radio off: no place chosen")
                say("crypto self-test: not run (no place chosen)")
                if self.zwave_place_stored:
                    say(f"place {self.zwave_place_stored} from the next restart")
                self._custom_lines("zwave", say)
                say("`zwave place` lists the places this board offers")
            else:
                pins = self._pins()["socket" if self.zwave_place == "socket" else "j7"]
                reset = "none"
                if self.zwave_place == "custom":
                    own = self.custom["zwave"]
                    pins, reset = (own["tx"], own["rx"]), (f"GPIO {own['reset']}" if "reset" in own else "none")
                    say(f"place custom: your own pins - UART3 TX GPIO {pins[0]}, RX GPIO {pins[1]}, reset line {reset}")
                else:
                    say(f"place {self.zwave_place}: somewhere - UART3 TX GPIO {pins[0]}, RX GPIO {pins[1]}, "
                        "reset line none")
                if self.zwave_place_stored != self.zwave_place:
                    say(f"from the next restart: place {self.zwave_place_stored or 'off'}, reset line -1")
                self._custom_lines("zwave", say)
                say("radio: " + ("working" if self.zwave_fitted else
                                 "not answering: check its wires; the panel keeps trying"))
                say("crypto self-test: passed")
                say("module: firmware 1.10, Z-Wave 7.18.3 (static controller, library type 7), chip 08 v1")
        elif cmd == "restart":
            say("restarting")
            threading.Timer(0.2, self.boot).start()
            return
        elif cmd.startswith("rtc set"):
            arg = cmd.split()[2] if len(cmd.split()) > 2 else ""
            if self.clock_module not in ("ok", "lost"):
                say("refused: no clock module answers, so the time would be lost at the next power cut")
            elif not arg.isdigit() or not 1735689600 <= int(arg) <= 4102444799:
                say("refused: that is not a time between 2025 and 2099")
            else:
                self.clock_offset = int(arg) - time.time()
                self.clock_source = "manual"
                self.clock_module = "ok"
                say("clock set to " + time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(int(arg)))
                    + "; the module follows at the next second")
        elif cmd == "zones":
            say("  1 p1 Hall motion              interior_follower  motion       normal")
            say("      sensor: 433 MHz EV1527 A2B3C code 5")
            say("  3 p1 Back door                perimeter          door         normal")
            say("      sensor: GPIO 4 on the J7 header")
        elif cmd == "terminals" and inst["board"] in P4_PINS:
            taken = set(self._taken(""))   # the terminals a stored place takes are no longer offered
            for g in (2, 3, 4, 5, 49, 50, 51, 52):
                if g not in taken or g == 4:   # the hardware wins: zone 3 keeps its binding
                    say(f"GPIO {g:<3} J7 header  {'closed':<7}" + (" zone 3" if g == 4 else ""))
            say("an open loop reads open; wire each contact between its GPIO and GND (3.3 V logic)")
        elif cmd == "zigbee status" and inst["board"] in P4_PINS and _vt(inst["version"]) >= (0, 6, 0):
            # zigbee_console.c's lines (FLS-030); older firmware has no `zigbee`. The radio needs a
            # few seconds after a restart.
            ready = time.monotonic() - self.booted_at > 4.0
            say("Sensor radio ready" if ready else "Sensor radio starting")
            say(f"ready={int(ready)} update={100 if ready else 0}% version={self.zigbee_version if ready else ''} "
                f"devices=0 channel=15 pan=1a2b jammed=0")
        elif cmd == "help":
            say("status  zones  terminals  rf  rtc  power  health  display  ota  dev  restart")
        elif cmd == "":
            pass
        else:
            say("Unrecognized command")
        self.emit("alarm> ", 0.02)


class MockBoard(FakeBoard):
    def __init__(self, device, info, **kw):
        super().__init__(info, **kw)
        self.device = device

    def session(self, port, log=lambda text: None, cancel=None):
        return MockSession(self, port, log, cancel or threading.Event())


class MockSession(FakeSession):
    def connect(self):
        if self.board.device.holder is not None:
            raise MockSerialException(f"could not open port '{self.port}': PermissionError(13, 'Access is denied.')")
        time.sleep(0.6)
        return super().connect()

    def reset(self):
        super().reset()
        self.board.device.boot()


class MockSerial:
    """The parts of serial.Serial the Flasher uses."""

    def __init__(self, world):
        self.world = world
        self.port = None
        self.baudrate = 115200
        self.timeout = 0.05
        self.dtr = False
        self._rts = False
        self.device = None

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        was = self._rts
        self._rts = bool(value)
        if self.device is not None and was and not self._rts:
            self.device.boot()      # EN released: the chip restarts

    @property
    def in_waiting(self):
        return len(self.device.out) if self.device else 0

    def open(self):
        dev = self.world.devices.get(self.port)
        if dev is None:
            raise MockSerialException(f"could not open port '{self.port}': FileNotFoundError(2, 'The system cannot find the file specified.')")
        if dev.holder is not None:
            raise MockSerialException(f"could not open port '{self.port}': PermissionError(13, 'Access is denied.', None, 5)")
        dev.holder = self
        self.device = dev

    def close(self):
        if self.device is not None and self.device.holder is self:
            self.device.holder = None
        self.device = None

    def read(self, n=1):
        if self.device is None:
            raise MockSerialException("port is closed")
        if self.world.devices.get(self.port) is not self.device:
            raise MockSerialException("ClearCommError failed: the device disconnected")
        return self.device.read(n, self.timeout)

    def write(self, data):
        if self.device is None:
            raise MockSerialException("port is closed")
        if getattr(self.device, "radio_module", False):
            # A Zigbee module's USB Serial/JTAG takes nothing while its software runs: the write
            # waits, then fails after write_timeout (B027); without one it never returns.
            if getattr(self, "write_timeout", None) is None:
                raise AssertionError(f"a write to {self.port} without a write timeout would hang")
            raise SerialTimeoutException("Write timeout")
        self.device.write(bytes(data))
        return len(data)

    def reset_input_buffer(self):
        if self.device:
            with self.device.lock:
                self.device.out.clear()


class MockWorld:
    def __init__(self, bundle=None, no_driver=None, custom_pins=True, radio="factory"):
        """custom_pins=False: every panel runs a RoboAlarms from before custom places (FLS-029).
        radio (FLS-030), a chip on a 303A:1001 port: "factory" (COM9, the Zigbee module with
        Espressif's OpenThread CLI), "older" (COM10, ours at 0.9.0), "s3" (COM11, an ESP32-S3, not a
        module), "all" (the three) or None."""
        self.devices = {}
        self.no_driver = no_driver      # "missing" / "failed": a CH340 Windows can't use, no ports at all
        if no_driver:
            return
        factory = bytearray(b"\xff" * (16 * MB))
        factory[0x10000:0x10004] = b"\xe9\x03\x02\x20"   # a vendor demo: not an app we can read
        p4 = lambda: ChipInfo(chip="esp32p4", revision=103, flash_size=16 * MB,  # noqa: E731
                              mac="30:ed:a0:12:34:5" + str(len(self.devices)), description="ESP32-P4 (revision v1.3)")
        sensors = [("A2B3C", 5), ("A2B3C", 0xE), ("1F0D7", 1)]
        self.add(MockDevice("COM5", "USB-SERIAL CH340 (COM5)", 0x1A86, 0x7522, chip=p4(),
                            board_name="CrowPanel Advanced 7in (ESP32-P4) V1.1", flash=factory, sensors=sensors))
        if bundle is not None:
            old = self._flash_with(bundle, "crowpanel_p4", 103, version="0.5.9")
            self.add(MockDevice("COM6", "USB-SERIAL CH340 (COM6)", 0x1A86, 0x7522, chip=p4(),
                                board_name="CrowPanel Advanced 7in (ESP32-P4) V1.1", flash=old, sensors=sensors))
            armed = self._flash_with(bundle, "crowpanel_p4", 103)
            self.add(MockDevice("COM7", "USB-SERIAL CH340 (COM7)", 0x1A86, 0x7522, chip=p4(),
                                board_name="CrowPanel Advanced 7in (ESP32-P4) V1.1", armed=True, flash=armed,
                                clock="absent"))
            if any(i.board == "crowpanel_10" for i in bundle.images):
                ten = self._flash_with(bundle, "crowpanel_10", 103)
                self.add(MockDevice("COM12", "USB-SERIAL CH340 (COM12)", 0x1A86, 0x7522, chip=p4(),
                                    board_name="CrowPanel Advanced 10.1in (ESP32-P4) V1.2", flash=ten,
                                    sensors=sensors))
        self.add(MockDevice("COM8", "Silicon Labs CP210x USB to UART Bridge (COM8)", 0x10C4, 0xEA60,
                            chip=ChipInfo(chip="esp32s3", revision=2, flash_size=8 * MB, mac="f4:12:fa:00:00:08",
                                          description="ESP32-S3 (QFN56) (revision v0.2)")))
        self.add(MockDevice("COM1", "Communications Port (COM1)", None, None))
        self.add(MockDevice("COM4", "Standard Serial over Bluetooth link (COM4)", None, None, bluetooth=True))
        from .testkit import radio_flash
        h2 = lambda n: ChipInfo(chip="esp32h2", revision=1, flash_size=4 * MB,  # noqa: E731
                                mac=f"74:4d:bd:00:00:0{n}", description="ESP32-H2 (revision v0.1)")
        jtag = "USB JTAG/serial debug unit ({})"
        if radio in ("factory", "all"):
            self.add(MockDevice("COM9", jtag.format("COM9"), 0x303A, 0x1001, chip=h2(1), radio_module=True,
                                flash=radio_flash(project="esp_ot_cli", version="v5.4.1")))
        if radio in ("older", "all"):
            self.add(MockDevice("COM10", jtag.format("COM10"), 0x303A, 0x1001, chip=h2(2), radio_module=True,
                                flash=radio_flash(version="0.9.0")))
        if radio in ("s3", "all"):
            self.add(MockDevice("COM11", jtag.format("COM11"), 0x303A, 0x1001, radio_module=True,
                                chip=ChipInfo(chip="esp32s3", revision=2, flash_size=8 * MB, mac="f4:12:fa:00:00:0b",
                                              description="ESP32-S3 (QFN56) (revision v0.2)")))
        zigbee = bundle.radio("zigbee") if bundle is not None else None
        for dev in self.devices.values():
            dev.custom_ok = custom_pins
            dev.zigbee_version = ((zigbee.rcp_version if zigbee is not None and zigbee.rcp_version else "")
                                  or MOCK_RCP_VERSION)[:63]
            dev.boot()

    def _flash_with(self, bundle, board, rev, version=None):
        from .testkit import synthetic_image
        flash = bytearray(b"\xff" * (16 * MB))
        image = next(i for i in bundle.images if i.board == board and i.accepts_rev(rev))
        if version:
            image = synthetic_image(board=board, board_name=image.board_name, version=version,
                                    profile=image.profile)
        for p in image.parts:
            flash[p.offset:p.offset + len(p.data)] = p.data
        return flash

    def add(self, dev):
        self.devices[dev.port] = dev

    def lister(self):
        return [d.raw for d in self.devices.values()]

    def driver_probe(self):
        if not self.no_driver:
            return []
        return [{"chip": "CH340", "usb_id": "1A86:7523", "kind": self.no_driver,
                 "code": 28 if self.no_driver == "missing" else 31}]

    def opener(self):
        return MockSerial(self)

    def connect(self, port, log, cancel, fast_baud):
        dev = self.devices.get(port)
        if dev is None:
            raise MockSerialException(f"could not open port '{port}': FileNotFoundError")
        if dev.board is None:
            time.sleep(1.0)
            raise RuntimeError(f"Failed to connect to Espressif device: No serial data received.")
        return dev.board.connect(port, log, cancel, fast_baud)
