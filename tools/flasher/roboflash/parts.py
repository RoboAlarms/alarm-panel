"""Connected parts (FLS-024): what is wired to the panel, as the page chooses it, the console
commands that set the panel up for it (docs/features/25 RF-008, RF-019, RF-020; docs/features/35 RTC-013;
docs/features/36 hardware.md §4), and what a running panel says it is set up for.

A choice, as the page sends it:
    {"rf": "none" | "module" | "cc1101" | "auto",
     "rf_place": "socket" | "j7",   receiver module: where DATA is; CC1101: where the whole board is
     "rf_pin": 3,                   receiver module on J7: the IO pin DATA is on
     "clock": true, "clock_place": "j13" | "j7",
     "zigbee": false,               the ESP32-H2 module in the wireless socket: nothing to send yet,
                                    but it takes the socket
     "zwave": false, "zwave_place": "socket" | "j7",   the ZAC93 Z-Wave radio, on its serial line
     "layout": "places" | "custom"}  "places" (the default): the named places above
With "layout": "custom" (FLS-029, Advanced tools > Hardware > Custom pins) every part is on the
board GPIOs the person wired it to, from the board's reachable pins (boards.json "custom"):
     "rf": "none" | "module" | "cc1101" (never "auto"), "clock_place": "j13" | "custom",
     "zwave_place": "custom" (set by normalise), the Zigbee module only in its socket, and
     "pins": {"rf": {"sck", "mosi", "miso", "cs", "gdo0"} (CC1101) or {"data"} (receiver module),
              "clock": {"sda", "scl"} (clock_place "custom"),
              "zwave": {"tx", "rx", "reset"}}  tx and rx the panel's side; reset may be None
The same rules run in the page (web/app.js) for its pin check; these are the ones that count.
"""
import re

RF_KINDS = ("none", "module", "cc1101", "auto")
LAYOUTS = ("places", "custom")
# The pin keys of each part on custom pins, in the console's order (the contract in FLS-029).
CUSTOM_KEYS = {"cc1101": ("sck", "mosi", "miso", "cs", "gdo0"), "module": ("data",),
               "clock": ("sda", "scl"), "zwave": ("tx", "rx", "reset")}
_ROLE = {"cc1101": "CC1101 {}", "module": "Receiver module {}", "clock": "Clock {}", "zwave": "Z-Wave {}"}
TOO_OLD = ("This panel's RoboAlarms is too old for custom pins.",
           "Update it (Software) with these pins: the layout is set after the update.")
DEFAULT = {"rf": "none", "rf_place": "socket", "rf_pin": None, "clock": False, "clock_place": "j13",
           "zigbee": False, "zwave": False, "zwave_place": "socket"}


def normalise(choice):
    """The choice with every key, and values of the right type."""
    c = dict(DEFAULT)
    for k in DEFAULT:
        if isinstance(choice, dict) and k in choice:
            c[k] = choice[k]
    c["rf"] = str(c["rf"])
    c["rf_place"] = str(c["rf_place"])
    c["clock_place"] = str(c["clock_place"])
    c["zwave_place"] = str(c["zwave_place"])
    c["clock"] = bool(c["clock"])
    c["zigbee"] = bool(c["zigbee"])
    c["zwave"] = bool(c["zwave"])
    try:
        c["rf_pin"] = None if c["rf_pin"] is None else int(c["rf_pin"])
    except (TypeError, ValueError):
        c["rf_pin"] = -1
    c["layout"] = str(choice.get("layout", "places")) if isinstance(choice, dict) else "places"
    if c["layout"] == "custom":
        if c["zwave"]:
            c["zwave_place"] = "custom"
        given = choice.get("pins") if isinstance(choice.get("pins"), dict) else {}
        c["pins"] = {part: {k: _gpio(v.get(k)) for k in CUSTOM_KEYS[kind]}
                     for part, kind in _custom_parts(c)
                     for v in [given.get(part) if isinstance(given.get(part), dict) else {}]}
        if not c["pins"] and c["rf"] != "auto" and (not c["clock"] or c["clock_place"] == "j13"):
            # Nothing on its own pins: the same as named places, which is how the panel reads it
            # back (the receiver off, the clock on the I2C connector, no Z-Wave radio). A clock
            # whose place isn't chosen yet stays custom, so parts_check asks for it.
            c["layout"] = "places"
            c["clock_place"] = "j13"
            del c["pins"]
    return c


def _gpio(v):
    """A pin as chosen: None while it isn't, -1 when it isn't a number."""
    if v is None or v == "":
        return None
    try:
        return int(v)
    except (TypeError, ValueError):
        return -1


def _custom_parts(c):
    """(part, kind) for each part a custom layout puts on chosen pins."""
    out = []
    if c["rf"] in ("cc1101", "module"):
        out.append(("rf", c["rf"]))
    if c["clock"] and c["clock_place"] == "custom":
        out.append(("clock", "clock"))
    if c["zwave"]:
        out.append(("zwave", "zwave"))
    return out


def _roles(c):
    """(role in words, gpio or None) for every pin a custom layout chooses; the Z-Wave reset line
    only when it has one."""
    out = []
    for part, kind in _custom_parts(c):
        for k in CUSTOM_KEYS[kind]:
            g = c["pins"][part][k]
            if part == "zwave" and k == "reset" and g is None:
                continue
            out.append((_ROLE[kind].format({"data": "DATA", "reset": "reset"}.get(k, k.upper())), g))
    return out


def reachable(board):
    """{gpio: label} of the pins a person can wire their own parts to (boards.json "custom")."""
    return {p["gpio"]: p["label"] for p in (((board or {}).get("custom") or {}).get("pins") or [])}


def custom_gpios(choice):
    """The board GPIOs a custom layout takes (none for named places)."""
    c = normalise(choice)
    if c["layout"] != "custom":
        return set()
    return {g for _, g in _roles(c) if isinstance(g, int) and g >= 0}


def parts_check(choice, board):
    """What is wrong with a choice, in words; [] when fine (FLS-029). For a custom layout: every
    pin chosen, each one once, reachable on this board, the clock's place chosen, one 433 MHz
    radio, never "Not sure". A zone's wired terminal never blocks a pin (the hardware wins: the
    zone is only named in the confirmation). Named places: problems()."""
    c = normalise(choice)
    if c["layout"] != "custom":
        return problems(c, board)
    board = board or {}
    reach = reachable(board)
    out = []
    if not reach:
        return ["This board has no pins for your own wiring: choose its named places."]
    if c["rf"] == "auto":
        out.append("“Not sure” can't be used with your own pins: choose the receiver.")
    elif c["rf"] not in ("none", "module", "cc1101"):
        out.append("Unknown receiver choice.")
    if c["clock"]:
        if board.get("rtc") is None:
            out.append("This board has no place for a clock module.")
        elif c["clock_place"] not in ("j13", "custom"):
            out.append("The clock: choose the I2C connector or your own pins.")
    if c["zwave"] and board.get("zwave") is None:
        out.append("This board has no place for a Z-Wave radio.")
    if out:
        return out
    roles = _roles(c)
    missing = sum(1 for _, g in roles if g is None)
    if missing:
        out.append(f"{missing} pin{'s' if missing != 1 else ''} still to choose.")
    for role, g in roles:
        if g is not None and g not in reach:
            out.append(f"{role} can't be on {'GPIO ' + str(g) if g >= 0 else 'that pin'}: choose one of "
                       + ", ".join(reach[p] for p in sorted(reach)) + ".")
    by_gpio = {}
    for role, g in roles:
        if g is not None and g in reach:
            by_gpio.setdefault(g, []).append(role)
    for g, used in sorted(by_gpio.items()):
        if len(used) > 1:
            times = "twice" if len(used) == 2 else f"{len(used)} times"
            out.append(f"{reach[g]} is chosen {times}: " + ", ".join(used[:-1]) + " and " + used[-1] + ".")
    return out


def problems(choice, board):
    """What is wrong with a choice for this board (its catalogue entry), in words; [] when fine."""
    c = normalise(choice)
    if c["layout"] == "custom":
        return parts_check(c, board)
    if c["layout"] not in LAYOUTS:
        return ["Unknown layout."]
    rf = (board or {}).get("rf") or {}
    rtc = (board or {}).get("rtc")
    out = []
    if c["rf"] not in RF_KINDS:
        out.append("Unknown receiver choice.")
        return out
    name = {"module": "The receiver module", "cc1101": "The CC1101"}.get(c["rf"])
    if name:
        if c["rf_place"] not in ("socket", "j7"):
            out.append(f"{name}: unknown place.")
        elif c["rf_place"] == "socket" and c["zigbee"]:
            out.append(f"{name} can't use the radio socket: the Zigbee module is in it.")
        elif c["rf"] == "cc1101" and c["rf_place"] == "j7" and "j7" not in (rf.get("places") or {}):
            out.append("This board has no J7 place for a CC1101.")
        elif c["rf"] == "module" and c["rf_place"] == "j7" and c["rf_pin"] not in rf.get("pins", []):
            out.append("The receiver's DATA must be on one of the J7 pins the panel offers.")
    if c["zigbee"] and board and board.get("zigbee") is None:
        out.append("This board has no socket for the Zigbee module.")
    if c["rf"] == "auto" and c["zigbee"]:
        out.append("“Not sure” can't be used with the Zigbee module fitted: the search would probe "
                   "its pins. Choose the receiver.")
    if c["clock"]:
        if rtc is None:
            out.append("This board has no place for a clock module.")
        elif c["clock_place"] not in ("j13", "j7"):
            out.append("The clock: unknown place.")
        elif c["clock_place"] == "j7" and "j7" not in (rtc.get("places") or {}):
            out.append("This board has no J7 place for the clock module.")
    if c["zwave"]:
        zw = (board or {}).get("zwave")
        if zw is None:
            out.append("This board has no place for a Z-Wave radio.")
        elif c["zwave_place"] not in (zw.get("places") or {}):
            out.append("The Z-Wave radio: unknown place.")
        elif c["zwave_place"] == "socket" and c["zigbee"]:
            out.append("The Z-Wave radio can't use the radio socket: the Zigbee module is in it.")
        elif (c["zwave_place"] == "j7" and c["clock"] and c["clock_place"] == "j7"
              and _zwave_pins(board, "j7") & _clock_j7_pins(board)):
            out.append("The Z-Wave radio and the clock module can't both use J7 IO27 and IO28: "
                       "put the clock on the I2C connector.")
    return out


def recommended(choice, board):
    """Where each part goes when only what is connected was chosen (the guided setup, BL-117).

    J7 for the receiver module (DATA on the board's IO3 when it offers it) and the clock (their own
    wiring); the radio socket for one radio while it is free, the CC1101 first (on J7 it takes five
    wired-zone terminals, the Z-Wave radio none), else J7; the clock on the I2C connector when the
    Z-Wave radio takes its J7 pins. The page's recommend() (web/app.js) is the same; every mix of
    parts passes problems() (tests/test_flasher_parts.py).

    A board wired from tables ("wiring": "table", the ESP32-S3 board; FLS-035) has one place for
    each part: the receiver on its own pins (the default place), the clock on its own (j13, the
    default), the Z-Wave radio on its one place."""
    c = normalise(choice)
    rf = (board or {}).get("rf") or {}
    pins = rf.get("pins") or []
    zw_places = ((board or {}).get("zwave") or {}).get("places") or {}
    if (board or {}).get("wiring") == "table":
        if c["rf"] in ("module", "cc1101"):
            c["rf_place"], c["rf_pin"] = "socket", None
        if c["zwave"] and zw_places and c["zwave_place"] not in zw_places:
            c["zwave_place"] = next(iter(zw_places))
        if c["clock"]:
            c["clock_place"] = "j13"
        return c
    socket_free = not c["zigbee"]
    if c["rf"] == "module":
        if pins:
            c["rf_place"] = "j7"
            if c["rf_pin"] not in pins:
                c["rf_pin"] = 3 if 3 in pins else pins[0]
        else:
            c["rf_place"] = "socket"
    elif c["rf"] == "cc1101":
        if socket_free or "j7" not in (rf.get("places") or {}):
            c["rf_place"] = "socket"
            socket_free = False
        else:
            c["rf_place"] = "j7"
    if c["zwave"]:
        if socket_free and "socket" in zw_places:
            c["zwave_place"] = "socket"
            socket_free = False
        elif "j7" in zw_places:
            c["zwave_place"] = "j7"
    if c["clock"]:
        j7_ok = "j7" in ((((board or {}).get("rtc")) or {}).get("places") or {})
        clash = c["zwave"] and c["zwave_place"] == "j7" and bool(_zwave_pins(board, "j7") & _clock_j7_pins(board))
        c["clock_place"] = "j7" if j7_ok and not clash else "j13"
    return c


def _zwave_pins(board, place):
    p = (((board or {}).get("zwave") or {}).get("places") or {}).get(place) or {}
    return {p[k] for k in ("tx", "rx") if isinstance(p.get(k), int)}


def _clock_j7_pins(board):
    p = ((((board or {}).get("rtc") or {}).get("places") or {}).get("j7")) or {}
    return {p[k] for k in ("sda", "scl") if isinstance(p.get(k), int)}


def commands(choice, board, from_custom=False):
    """The console commands, in order, that set the panel up for a choice (check it first).

    The data pin is put back before a CC1101 moves to J7 and the place before a module's pin moves,
    so every command is checked by the panel against the board as it will be. The Z-Wave radio's
    place is taken away first and chosen last, so its pins never block the clock's or the
    receiver's new place (the panel checks theirs against the stored Z-Wave place too).

    A custom layout (FLS-029): the probe `rf place` first (its listing has a "custom: " line on
    firmware that takes custom places), then every part taken away, then each custom place set.
    Named places on a panel last read with custom ones (from_custom): every part taken away
    first too, then today's sequence (only firmware that takes custom places gets this)."""
    c = normalise(choice)
    has_zwave = (board or {}).get("zwave") is not None
    if c["layout"] != "custom" and from_custom:
        away = (["zwave place off"] if has_zwave else []) + ["rf module off", "rf pin default", "rf place default"]
        if (board or {}).get("rtc") is not None:
            away.append("rtc place default")
        named = commands(c, board)
        return away + (named[1:] if has_zwave else named)
    if c["layout"] == "custom":
        cmds = [PROBE] + (["zwave place off"] if has_zwave else [])
        cmds += ["rf module off", "rf pin default", "rf place default"]
        if (board or {}).get("rtc") is not None:
            cmds.append("rtc place default")
        for part, kind in _custom_parts(c):
            pins = c["pins"][part]
            keys = " ".join(f"{k}={pins[k]}" for k in CUSTOM_KEYS[kind] if pins[k] is not None)
            cmds.append({"rf": "rf", "clock": "rtc", "zwave": "zwave"}[part] + " place custom " + keys)
        return cmds
    cmds = ["zwave place off"] if has_zwave else []
    if c["rf"] == "none":
        cmds += ["rf module off", "rf pin default", "rf place default"]
    elif c["rf"] == "auto":
        cmds += ["rf module auto", "rf pin default", "rf place default"]
    elif c["rf"] == "module":
        cmds += ["rf module ook", "rf place default",
                 f"rf pin {c['rf_pin']}" if c["rf_place"] == "j7" else "rf pin default"]
    elif c["rf"] == "cc1101":
        cmds += ["rf module cc1101", "rf pin default",
                 "rf place j7" if c["rf_place"] == "j7" else "rf place default"]
    if (board or {}).get("rtc") is not None:
        cmds.append("rtc place j7" if c["clock"] and c["clock_place"] == "j7" else "rtc place default")
    if has_zwave and c["zwave"]:
        cmds.append(f"zwave place {c['zwave_place']}")
    return cmds


PROBE = "rf place"


def is_probe(cmd):
    """The listing that tells a firmware with custom places from an older one (not a setting)."""
    return cmd == PROBE


def probe_line(line):
    """For a console line after the probe: "custom" (this firmware takes custom places), "end"
    (the listing is over, or the command is unknown), "reply" (part of the listing), or None."""
    if line.startswith("custom: "):
        return "custom"
    if line.startswith(("`rf place <place>`", "Unrecognized command")) or line.startswith(_OLD):
        return "end"
    if line.startswith(("place ", "default: ", "the receiver is not running")):
        return "reply"
    return None


def optional(cmd):
    """A command a firmware without it may skip: taking away a Z-Wave radio it can't have."""
    return cmd == "zwave place off"


def custom_refused(cmd, line):
    """`rf module <m>` refused because the receiver is on custom pins: `rf place default` first."""
    return _family(cmd) == "rf module" and line.startswith("not ") and "your own pins" in line


# What each command answers when it did it, and when it didn't (rf_service.c, panel_clock.c).
_OK = {"rf module": ("receiver ",), "rf pin": ("data on GPIO",), "rf place": ("place ",), "rtc place": ("place ",),
       "zwave place": ("place ",)}
_NO = {"rf module": ("rf module auto|",), "rf pin": ("not GPIO", "could not store", "rf pin <gpio>"),
       "rf place": ("not ", "could not store"), "rtc place": ("not ", "could not store"),
       "zwave place": ("not ", "could not store")}
_OLD = ("rf [status", "rtc [status]", "Clock module: not in this firmware", "Unrecognized command",
        "433 MHz receiver: not in this firmware")


def _family(cmd):
    return " ".join(cmd.split()[:2])


def reply_kind(cmd, line):
    """For a console line after cmd: "ok", "refused", "old" (this firmware can't), or None (not
    the reply)."""
    fam = _family(cmd)
    if line.startswith(_OLD):
        return "old"
    if " place custom " in f" {cmd} ":
        if line.startswith("place custom from the next restart"):
            return "ok"
        if line.startswith(("not custom: ", "not ", "could not store")):
            return "refused"
        return None
    if custom_refused(cmd, line):
        return "refused"
    if line.startswith(_OK.get(fam, ())):
        return "ok"
    if line.startswith(_NO.get(fam, ())):
        return "refused"
    return None


# ---- what a running panel is set up for -------------------------------------------------------
_MODULE = re.compile(r"^module=(\S+) receiver=.+? place=(\S+)")
_MODULE_OLD = re.compile(r"^module=(\S+) receiver=")
_GPIO = re.compile(r"GPIO (\d+)")
_RTC = re.compile(r"^rtc module=(\S+) .* place=(\S+)")
_RTC_OLD = re.compile(r"^rtc module=(\S+) ")


_ZW_RUNNING = re.compile(r"^place (\S+): ")
_ZW_NEXT = re.compile(r"^(?:from the next restart: place (\S+?),|place (\S+) from the next restart)")


def read_zwave(lines):
    """The Z-Wave radio's stored place and whether it answers, from `zwave status`:
    (place or None for none, found). Firmware without Z-Wave: (None, False)."""
    place, found = None, False
    for line in lines:
        m = _ZW_NEXT.match(line)
        if m:
            place = m.group(1) or m.group(2)
            place = None if place == "off" else place
            continue
        m = _ZW_RUNNING.match(line)
        if m and place is None:
            place = m.group(1)
        if line.startswith("radio: working"):
            found = True
    return place, found


def read_current(rf_lines, rtc_lines, board, zwave_lines=()):
    """The choice a panel's `rf`, `rtc` and `zwave status` replies describe (the Zigbee module
    can't be seen), with three facts added: rf_found (a receiver answers), clock_found (a clock
    module does) and zwave_found (the Z-Wave radio does)."""
    rf = (board or {}).get("rf") or {}
    c = dict(DEFAULT)
    c["rf_found"] = c["clock_found"] = False
    mode, place, gpio = None, "default", None
    for line in rf_lines:
        m = _MODULE.match(line) or _MODULE_OLD.match(line)
        if m:
            mode = m.group(1)
            if m.re is _MODULE:
                place = m.group(2)
        elif line.startswith(("Receiver module on GPIO", "No receiver module drives GPIO", "No CC1101 answers",
                              "No receiver yet")):
            g = _GPIO.search(line)
            gpio = int(g.group(1)) if g else None
            c["rf_found"] = line.startswith("Receiver module on GPIO")
        elif line.startswith("CC1101 at "):
            c["rf_found"] = True
        elif line.startswith("433 MHz receiver: off"):
            mode = "off"
    c["rf"] = {"off": "none", "ook": "module", "cc1101": "cc1101", "auto": "auto"}.get(mode, "none")
    if c["rf"] == "cc1101":
        c["rf_place"] = "j7" if place == "j7" else "socket"
    elif c["rf"] == "module":
        own = rf.get("default_gpio")
        if gpio is not None and gpio != own and gpio in rf.get("pins", []):
            c["rf_place"], c["rf_pin"] = "j7", gpio
    for line in rtc_lines:
        m = _RTC.match(line) or _RTC_OLD.match(line)
        if m:
            module = m.group(1)
            place = m.group(2) if m.re is _RTC else "default"
            c["clock_place"] = "j7" if place == "j7" else "j13"
            c["clock_found"] = module in ("ok", "lost", "failed", "other")
            c["clock"] = c["clock_found"] or place == "j7"
    zw_place, c["zwave_found"] = read_zwave(zwave_lines)
    c["zwave"] = zw_place is not None
    if zw_place is not None:
        c["zwave_place"] = zw_place
    _read_custom(c, rf_lines, rtc_lines, zwave_lines)
    return c


_CUSTOM = re.compile(r"^custom((?: [a-z0-9]+=-?\d+)+)$")
_NOT_USED = "custom place not used: "
_PART_WORDS = {"rf": "the 433 MHz receiver", "clock": "the clock module", "zwave": "the Z-Wave radio"}


def _custom_pins(lines):
    """({key: gpio} of the "custom k=n ..." line, or None; the reasons of "custom place not used: ")."""
    pins, why = None, []
    for line in lines:
        m = _CUSTOM.match(line.strip())
        if m:
            pins = {k: int(v) for k, v in (kv.split("=") for kv in m.group(1).split())}
        elif line.startswith(_NOT_USED):
            why.append(line[len(_NOT_USED):].strip().rstrip("."))
    return pins, why


def _read_custom(c, rf_lines, rtc_lines, zwave_lines):
    """The custom places a panel has stored (FLS-029): layout "custom" with its pins, and in
    "notes" each one the panel refused at start (it left that part off)."""
    pins, notes = {}, []
    for part, lines in (("rf", rf_lines), ("clock", rtc_lines), ("zwave", zwave_lines)):
        got, why = _custom_pins(lines)
        if got is not None:
            pins[part] = got
        for w in why:
            notes.append(f"The panel doesn't use your pins for {_PART_WORDS[part]} ({w}), so it left it off.")
    if any(re.search(r"\bplace=custom\b", line) for line in rf_lines) and "rf" not in pins:
        pins["rf"] = {}
    if not pins:
        return
    c["layout"] = "custom"
    if pins.get("rf"):
        c["rf"] = "cc1101" if "sck" in pins["rf"] else "module"
        pins["rf"] = {k: pins["rf"].get(k) for k in CUSTOM_KEYS[c["rf"]]}
    elif "rf" in pins:   # place=custom, but its keys can't be read: the panel says why in notes
        del pins["rf"]
    elif c["rf"] == "auto":
        c["rf"] = "none"
    if "clock" in pins:
        c["clock"], c["clock_place"] = True, "custom"
        pins["clock"] = {k: pins["clock"].get(k) for k in CUSTOM_KEYS["clock"]}
    if "zwave" in pins:
        c["zwave"], c["zwave_place"] = True, "custom"
        pins["zwave"] = {k: pins["zwave"].get(k) for k in CUSTOM_KEYS["zwave"]}
    c["pins"] = pins
    if notes:
        c["notes"] = notes


def differences(want, got, board):
    """What the panel reports that differs from the choice, in words; [] when it matches."""
    want, rf = normalise(want), (board or {}).get("rf") or {}
    if want["layout"] == "custom" or got.get("layout") == "custom":
        return _custom_differences(want, got, board)
    out = []
    if not same(dict(want, clock=False, zwave=False), dict(normalise(got), clock=False, zwave=False)):
        words = {"none": "switched off", "auto": "looking for either receiver",
                 "module": "a receiver module", "cc1101": "a CC1101"}.get(got.get("rf"), "unknown")
        if got.get("rf") == "module":
            words += f" with DATA on GPIO {got['rf_pin'] if got.get('rf_pin') else rf.get('default_gpio')}"
        elif got.get("rf") == "cc1101" and (board or {}).get("wiring") == "table":
            words += " on " + ((rf.get("cc1101") or {}).get("where") or "its own pins")
        elif got.get("rf") == "cc1101":
            words += " on J7" if got.get("rf_place") == "j7" else " on the radio socket"
        out.append(f"The panel reports its 433 MHz receiver as {words}.")
    if want["clock"] and got.get("clock_place") != want["clock_place"]:
        out.append("The panel looks for the clock module somewhere else: "
                   + ("the J7 header." if got.get("clock_place") == "j7" else "the I2C connector."))
    elif want["clock"] and not got.get("clock_found"):
        rtc = (board or {}).get("rtc") or {}
        where = ((rtc.get("places") or {}).get("j7", {}).get("where") if want["clock_place"] == "j7"
                 else rtc.get("where")) or "its connector"
        out.append(f"No clock module answered on the {where}: check its four wires, then restart the panel.")
    zw = (board or {}).get("zwave")
    if zw is not None:
        where = (((zw.get("places") or {}).get(want["zwave_place"]) or {}).get("where")
                 or "place chosen")
        if want["zwave"] and not got.get("zwave"):
            out.append("The panel has no Z-Wave radio set up: its firmware may be too old for it.")
        elif want["zwave"] and got.get("zwave_place") != want["zwave_place"]:
            out.append("The panel looks for the Z-Wave radio somewhere else.")
        elif want["zwave"] and not got.get("zwave_found"):
            out.append(f"The Z-Wave radio doesn't answer on the {where}: check its four wires (the "
                       "panel's TX to the radio's RX, RX to its TX), then restart the panel.")
        elif not want["zwave"] and got.get("zwave"):
            out.append("The panel still has a Z-Wave radio set up.")
    return out


def same(a, b):
    """Two choices set the panel up the same way (what `rf`/`rtc` can show: not the Zigbee module)."""
    a, b = normalise(a), normalise(b)
    if a["layout"] == "custom" or b["layout"] == "custom":
        return (a["layout"] == b["layout"] and all(a[k] == b[k] for k in ("rf", "clock", "zwave"))
                and (not a["clock"] or a["clock_place"] == b["clock_place"])
                and a["pins"] == b["pins"])
    keys = ["rf", "clock", "zwave"]
    if a["rf"] in ("module", "cc1101"):
        keys.append("rf_place")
    if a["rf"] == "module" and a["rf_place"] == "j7":
        keys.append("rf_pin")
    if a["clock"]:
        keys.append("clock_place")
    if a["zwave"]:
        keys.append("zwave_place")
    return all(a[k] == b[k] for k in keys)


def _custom_differences(want, got, board):
    """differences() for a custom layout (FLS-029): each part where it was put, its refusal
    notes, and whether the clock module and the Z-Wave radio answer."""
    out = []
    if want["layout"] != "custom":
        return ["The panel still has parts on your own pins."]
    if got.get("layout") != "custom":
        return ["The panel doesn't report your own pins: its RoboAlarms may be too old for them."]
    gp = got.get("pins") or {}
    wp = want["pins"]
    if got.get("rf") != want["rf"] or gp.get("rf") != wp.get("rf"):
        words = {"none": "switched off", "module": "a receiver module", "cc1101": "a CC1101"}.get(got.get("rf"), "unknown")
        if gp.get("rf"):
            words += " on " + ", ".join(f"{k.upper()} GPIO {v}" for k, v in gp["rf"].items())
        out.append(f"The panel reports its 433 MHz receiver as {words}.")
    if want["clock"] and (got.get("clock_place") != want["clock_place"] or gp.get("clock") != wp.get("clock")):
        out.append("The panel looks for the clock module somewhere else.")
    elif want["clock"] and not got.get("clock_found"):
        out.append("No clock module answered on the pins chosen: check its four wires, then restart the panel.")
    elif not want["clock"] and got.get("clock_place") == "custom":
        out.append("The panel still has a clock module on your own pins.")
    if want["zwave"] and (not got.get("zwave") or gp.get("zwave") != wp.get("zwave")):
        out.append("The panel looks for the Z-Wave radio somewhere else.")
    elif want["zwave"] and not got.get("zwave_found"):
        out.append("The Z-Wave radio doesn't answer on the pins chosen: check its wires (the panel's TX to "
                   "the radio's RX, RX to its TX), then restart the panel.")
    elif not want["zwave"] and got.get("zwave"):
        out.append("The panel still has a Z-Wave radio set up.")
    return list(got.get("notes") or []) + out


# ---- zones on the pins a layout takes (named in the confirmation, never a block) --------------
_TERMINAL = re.compile(r"^GPIO (\d+)\b.*\bzone (\d+)\s*$")
_ZONE_TYPES = ("not_used|entry_exit_1|entry_exit_2|perimeter|interior_follower|trouble_day_alarm_night|"
               "24h_silent|24h_audible|24h_aux|fire|interior_delay|monitor|carbon_monoxide|fire_verified|"
               "arm_stay|arm_away|disarm|no_response|silent_burglary|momentary_exit|keyswitch|unknown")
_ZONE = re.compile(r"^\s*(\d+) p\d+ (.*\S) +(?:" + _ZONE_TYPES + r") +\S+")


def zones_on(choice, terminal_lines, zone_lines):
    """[{"zone": 3, "name": "Back door", "gpio": 4}] for each zone wired (`terminals`) to a pin
    the custom layout takes; names from `zones`. [] for named places, or older firmware."""
    take = custom_gpios(choice)
    names = {}
    for line in zone_lines:
        m = _ZONE.match(line)
        if m:
            names[int(m.group(1))] = m.group(2).strip()
    out = []
    for line in terminal_lines:
        m = _TERMINAL.match(line.strip())
        if m and int(m.group(1)) in take:
            z = int(m.group(2))
            out.append({"zone": z, "name": names.get(z, ""), "gpio": int(m.group(1))})
    return sorted(out, key=lambda e: e["zone"])
