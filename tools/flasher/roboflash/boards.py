"""Which board is this, which image fits it, and which images may be chosen by hand
(FLS-005, FLS-006).

Evidence, strongest first:
  - the running firmware names its board (`dev describe`): "sure";
  - the chip, read through its ROM loader, matches exactly one catalogue board by chip family
    and flash size: "likely"; several: "ambiguous" (the person chooses: the CrowPanel 7" and
    10.1" share a main board, a chip and a USB bridge);
  - nothing matches: "none"; nothing known yet: "unknown".
The USB bridge never selects anything. Which silicon build fits comes from each image's own
header (min/max revision), so a new P4 variant needs no code here.

A board the catalogue knows but does not offer ("offered": false, the unfinished 4" E32R40T) is
never chosen: its images are refused with the reason, and a chip that looks like it is told so.

The catalogue is the board descriptions in use: the `boards.json` built into the Flasher, or newer
ones from the firmware server (fwserver.py), checked with check_document() and put in
use with use_document(), so a new board reaches the Flasher without a new Flasher.
"""
import json
import re
import threading
from dataclasses import dataclass, field
from pathlib import Path

from .bundle import flash_size_bytes
from .imagefile import chip_name, rev_text
from .paths import RESOURCES


BUILTIN = RESOURCES / "boards.json"
BOARDS_FORMAT = 1           # the board descriptions' format this Flasher reads
_BOARD_ID = re.compile(r"^[a-z0-9_]{1,32}$")
_active = None              # the descriptions in use; None until first read: the built-in file
_active_lock = threading.Lock()


def builtin_document():
    return json.loads(BUILTIN.read_text(encoding="utf-8"))


def document():
    """The board descriptions in use (the built-in file read once, never over newer ones put in use
    meanwhile by the firmware server's thread)."""
    global _active
    doc = _active
    if doc is None:
        with _active_lock:
            if _active is None:
                _active = builtin_document()
            doc = _active
    return doc


def use_document(doc):
    """Put board descriptions (checked by check_document) in use; None goes back to the built-in."""
    global _active
    with _active_lock:
        _active = doc


def version_key(text):
    """'2026.10.12' -> (2026, 10, 12), compared part by part, numbers as numbers; a
    version that isn't one sorts first."""
    parts = str(text or "").split(".")
    if not all(p.isdigit() for p in parts):
        return ()
    return tuple(int(p) for p in parts)


def check_document(doc):
    """Why board descriptions (the firmware server's) can't be used, or "": the format this Flasher
    reads, a version, and boards by id, each with a name, a chip and a flash size."""
    if not isinstance(doc, dict):
        return "they are not a JSON object"
    fmt = doc.get("format")
    if not isinstance(fmt, int) or isinstance(fmt, bool) or not 1 <= fmt <= BOARDS_FORMAT:
        return f"format {fmt!r}; this Flasher reads format {BOARDS_FORMAT}"
    if not version_key(doc.get("version")):
        return "no version"
    boards = doc.get("boards")
    if not isinstance(boards, dict) or not boards:
        return "no boards"
    for board, info in boards.items():
        if not _BOARD_ID.match(str(board)) or not isinstance(info, dict):
            return f"the board {board!r} is not described properly"
        if not all(isinstance(info.get(k), str) for k in ("name", "chip", "flash_size")):
            return f"the board {board!r} has no name, chip or flash size"
        try:
            flash_size_bytes(info["flash_size"])
        except ValueError:
            return f"the board {board!r} has the flash size {info['flash_size']!r}"
    return ""


def _boards(path=None):
    if path is not None:
        return json.loads(Path(path).read_text(encoding="utf-8"))["boards"]
    return document()["boards"]


def load_catalogue(path=None):
    """The boards the Flasher installs."""
    return {b: info for b, info in _boards(path).items() if info.get("offered", True)}


def not_offered(path=None):
    """{board: the full entry} for the boards the Flasher knows but does not install."""
    return {b: info for b, info in _boards(path).items() if not info.get("offered", True)}


def _unoffered_reason(board, known=None):
    info = (known if known is not None else not_offered()).get(board)
    if info is None:
        return ""
    return info.get("not_offered") or f"The Flasher does not install the {info.get('name', board)} yet."


@dataclass
class ChipInfo:
    """What the ROM loader told us."""
    chip: str            # "esp32p4"
    revision: int        # 103 = v1.3
    flash_size: int      # bytes, 0 when unknown
    mac: str
    description: str = ""
    crystal_mhz: int = 0

    def to_dict(self):
        return {"chip": self.chip, "chip_name": chip_name(self.chip), "revision": self.revision,
                "revision_text": rev_text(self.revision), "flash_size": self.flash_size,
                "flash_mb": self.flash_size // (1024 * 1024) if self.flash_size else 0,
                "mac": self.mac, "description": self.description, "crystal_mhz": self.crystal_mhz}


@dataclass
class Detection:
    confidence: str                  # sure / likely / ambiguous / none / unknown
    board: str = ""                  # the board, when sure or likely
    candidates: list = field(default_factory=list)
    image_key: str = ""              # the preselected image, or ""
    reason: str = ""

    def to_dict(self):
        return {"confidence": self.confidence, "board": self.board, "candidates": self.candidates,
                "image_key": self.image_key, "reason": self.reason}


@dataclass
class Verdict:
    allowed: bool
    blocked: str = ""                # why it may not be flashed
    confirm: str = ""                # what the person must acknowledge first

    def to_dict(self):
        return {"allowed": self.allowed, "blocked": self.blocked, "confirm": self.confirm}


def _chip_rev(probe, chip):
    if chip is not None:
        return chip.revision
    if probe is not None and probe.chip_revision is not None:
        return probe.chip_revision
    return None


def pick_image(bundle, board, rev, profile="", strict=False):
    """The image for `board` whose silicon range holds `rev`, the running profile first; with
    strict, that profile only (the firmware server's builds: a release panel never gets a
    development build because its own is still downloading)."""
    if bundle is None:
        return ""
    fits = [i for i in bundle.images if i.board == board and (rev is None or i.accepts_rev(rev))
            and not (strict and i.profile != profile)]
    if rev is None and len({(i.chip_rev_min, i.chip_rev_max) for i in fits}) > 1:
        return ""   # several silicon builds and no revision to choose by
    for image in fits:
        if profile and image.profile == profile:
            return image.key
    return fits[0].key if fits else ""


def _fits(info, chip):
    return info["chip"] == chip.chip and (not chip.flash_size
                                          or flash_size_bytes(info["flash_size"]) == chip.flash_size)


def detect(catalogue, bundle, probe=None, chip=None, unoffered=None, profile=None, say_missing=True,
           strict=False):
    """unoffered: {board: entry} the Flasher knows but does not install (not_offered()).
    profile: the build to prefer (default: the one the panel runs), or a function of the board that
    says it; with strict the only one taken.
    say_missing=False leaves out "this bundle has no image for it" (the firmware server's is still
    on its way)."""
    unoffered = not_offered() if unoffered is None else unoffered
    rev = _chip_rev(probe, chip)

    def prof(board):
        if profile is None:
            return probe.profile if probe else ""
        return profile(board) if callable(profile) else profile
    if probe is not None and probe.board:
        if probe.board in catalogue:
            name = catalogue[probe.board]["name"]
            key = pick_image(bundle, probe.board, rev, prof(probe.board), strict)
            reason = f"Its firmware says it is a {name}"
            if rev is not None:
                reason += f" (silicon {rev_text(rev)})"
            if not key and say_missing:
                reason += "; this bundle has no image for it"
            return Detection("sure", probe.board, [probe.board], key, reason + ".")
        if probe.board in unoffered:
            return Detection("none", "", [], "",
                             f"Its firmware says it is a {unoffered[probe.board]['name']}. "
                             + _unoffered_reason(probe.board, unoffered))
        return Detection("none", "", [], "",
                         f"Its firmware says it is a {probe.board!r}, which this Flasher does not know.")
    if chip is not None:
        matches = [b for b, info in catalogue.items() if _fits(info, chip)]
        facts = f"{chip_name(chip.chip)}, silicon {rev_text(chip.revision)}"
        if chip.flash_size:
            facts += f", {chip.flash_size // (1024 * 1024)} MB flash"
        if len(matches) == 1:
            board = matches[0]
            key = pick_image(bundle, board, rev, prof(board), strict)
            reason = f"Looks like a {catalogue[board]['name']}: {facts}."
            if not key and say_missing:
                reason += " This bundle has no image for it."
            return Detection("likely", board, matches, key, reason)
        if matches:
            names = " and the ".join(catalogue[b]["name"] for b in matches)
            return Detection("ambiguous", "", matches, "",
                             f"{facts}: that fits the {names}, which look the same to the Flasher. "
                             "Choose which one it is.")
        known = [b for b, info in unoffered.items() if _fits(info, chip)]
        if known:
            names = " or ".join(unoffered[b]["name"] for b in known)
            return Detection("none", "", [], "",
                             f"{facts}: it may be a {names}. " + _unoffered_reason(known[0], unoffered))
        return Detection("none", "", [], "",
                         f"{facts}: there is no RoboAlarms firmware for this board yet.")
    if probe is not None and probe.answered:
        return Detection("unknown", "", list(catalogue), "",
                         "RoboAlarms firmware answers, but does not say which board it runs on. "
                         "Identify the chip, or choose the board.")
    return Detection("unknown", "", list(catalogue), "",
                     "Not identified yet.")


def verdict(image, catalogue, probe=None, chip=None):
    """May `image` be flashed onto this board? Blocks what cannot work; asks about what might not."""
    if image.board not in catalogue:
        why = _unoffered_reason(image.board)
        return Verdict(False, blocked=why or f"The Flasher does not know the board {image.board!r}.")
    board = catalogue.get(image.board, {})
    name = board.get("name", image.board)
    if chip is not None:
        if chip.chip != image.chip:
            return Verdict(False, blocked=f"This image is for an {chip_name(image.chip)}; "
                                          f"the board has an {chip_name(chip.chip)}.")
        if chip.flash_size and chip.flash_size < image.flash_bytes:
            return Verdict(False, blocked=f"This image needs {image.flash_size} of flash; "
                                          f"the board has {chip.flash_size // (1024 * 1024)} MB.")
    confirms = []
    rev = _chip_rev(probe, chip)
    if rev is not None and not image.accepts_rev(rev):
        confirms.append(f"This image is made for {image.silicon_text()}, and this chip is "
                        f"{rev_text(rev)}. It will most likely not start (a black screen); "
                        f"installing the right image afterwards fixes it.")
    if probe is not None and probe.board and probe.board != image.board:
        other = catalogue.get(probe.board, {}).get("name", probe.board)
        confirms.append(f"The panel's firmware says it is a {other}, not a {name}.")
    return Verdict(True, confirm=" ".join(confirms))


def image_options(bundle, catalogue, probe=None, chip=None):
    """Every image in the bundle with whether it may be chosen, for the override list."""
    if bundle is None:
        return []
    out = []
    for image in bundle.images:
        v = verdict(image, catalogue, probe, chip)
        d = image.to_dict()
        d.update(v.to_dict())
        out.append(d)
    return out
