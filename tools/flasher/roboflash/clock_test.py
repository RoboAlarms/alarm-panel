"""The clock page (FLS-022): what `rtc` says about the battery-backed clock module (docs/features/35).

It reads the lines a person would see at the console:
  `rtc`           `Clock module: keeping time, 24.25 C` (words for people), then
                  `rtc module=ok source=internet trusted=1 ... epoch=1790260205` (the facts), then
                  `panel clock 2026-09-24 14:30:05 UTC (time server)`;
  `rtc set <s>`   `clock set to 2026-09-24 14:30:05 UTC; the module follows at the next second`
                  or `refused: ...`.
Firmware without clock-module support answers `Clock module: not in this firmware`, and firmware
older than that `Unrecognized command`.
"""
import re

WORDS = re.compile(r"^Clock module: (.+)$")
FACTS = re.compile(r"^rtc module=(\S+) source=(\S+) trusted=(\d) lost_at_start=(\d) temp_c4=(-?\d+) "
                   r"offset_ms=(\S+) writes=(\d+) write_failures=(\d+) slews=(\d+) jumps=(\d+) "
                   r"epoch=(-?\d+)")
PANEL = re.compile(r"^panel clock (.+) \((.+)\)$")
SET_OK = re.compile(r"^clock set to (.+?);")
REFUSED = re.compile(r"^refused: (.+)$")
NOT_BUILT = "Clock module: not in this firmware"
UNKNOWN = "Unrecognized command"
USAGE = "rtc [status]"

MODULE_WORDS = {"ok": "Keeping time", "lost": "Waiting for the time", "failed": "Not answering",
                "other": "Another chip at its address", "absent": "Not fitted"}
SOURCE_WORDS = {"none": "Not set yet", "module": "The clock module", "internet": "The time server",
                "manual": "Set by hand"}


def is_status_line(text):
    """True for the lines `rtc` prints (the ones a quiet poll hides from the console)."""
    return bool(WORDS.match(text) or FACTS.match(text) or PANEL.match(text))


def is_reply_end(text):
    """The line after which `rtc` has said all it will: the facts, or that it can't."""
    return bool(FACTS.match(text)) or text in (NOT_BUILT, UNKNOWN)


def parse_status(lines, computer_epoch):
    """The page's view of `rtc`'s lines. computer_epoch: this computer's time (seconds) when the
    facts line arrived; the difference is the panel's clock minus it (whole seconds: the panel
    prints whole seconds)."""
    out = {"available": False, "words": "", "module": "", "reason": ""}
    for raw in lines:
        text = raw.strip()
        if text == NOT_BUILT or text == UNKNOWN:
            out["reason"] = "This panel's firmware has no clock-module support."
            return out
        m = WORDS.match(text)
        if m:
            out["words"] = m.group(1)
            continue
        m = FACTS.match(text)
        if m:
            (module, source, trusted, lost, temp_c4, offset, writes, write_failures, slews, jumps,
             epoch) = m.groups()
            epoch = int(epoch)
            out.update(
                available=True, module=module, module_words=MODULE_WORDS.get(module, module),
                source=source, source_words=SOURCE_WORDS.get(source, source),
                internet_trusted=trusted == "1", lost_at_start=lost == "1",
                temp_c=int(temp_c4) / 4 if module in ("ok", "lost") else None,
                offset_ms=None if offset == "?" else int(offset),
                writes=int(writes), write_failures=int(write_failures), slews=int(slews),
                jumps=int(jumps), epoch=epoch,
                clock_set=epoch >= 1735689600,
                difference_s=(epoch - int(computer_epoch)) if epoch >= 1735689600 else None,
                settable=module in ("ok", "lost"))
    return out


def set_reply(text):
    """(ok, words) for the reply to `rtc set`, or None for a line that isn't one."""
    text = text.strip()
    m = SET_OK.match(text)
    if m:
        return True, f"Set to {m.group(1)}"
    m = REFUSED.match(text)
    if m:
        return False, m.group(1)
    if text in (NOT_BUILT, UNKNOWN) or text.startswith(USAGE):
        return False, "This panel's firmware has no clock-module support"
    return None
