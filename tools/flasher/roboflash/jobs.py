"""Install, update, back up and restore, one at a time, with their gates (FLS-004, FLS-007,
FLS-008, FLS-009, FLS-012).

Every job starts by looking at the panel without restarting it: a panel that is armed, in
installer mode, mid-update or on probation is never restarted. Then it connects to the chip's
loader and checks the image fits the chip (boards.verdict). An install backs the flash up first
and checks the backup file before anything is erased. An update keeps settings, so it refuses
when the partition table on the chip differs from the image's. After writing, each part's MD5 is
read back, the board restarts, and the boot log is watched for the firmware's "board:" and
"running" lines.
"""
import datetime
import hashlib
import json
import re
import struct
import threading
import time
import uuid
from dataclasses import dataclass, field, asdict
from pathlib import Path

from . import bundle as bundle_mod, packstore
from .boards import verdict
from .chip import Cancelled, FAST_BAUD, READ_BAUD
from .imagefile import rev_text, chip_name, parse_app
from .serialio import PortError, open_quiet, serial_opener, read_lines_until

BOOT_BOARD = re.compile(r"board: (.+) \(([a-z0-9_]+)\)\s*$")
BOOT_RUNNING = re.compile(r" running: internal RAM free")
BOOT_CRASH = re.compile(r"Guru Meditation|abort\(\) was called|Backtrace:|rst:0x[0-9a-f]+ \((?:TG|RTC)WDT")
# ESP-IDF's "Brownout detector was triggered", the ROM's brownout reset reasons: too little power.
BOOT_BROWNOUT = re.compile(r"(?i)brown.?out")
BOOT_TIMEOUT = 45.0


class JobFailed(Exception):
    def __init__(self, title, advice=""):
        super().__init__(title)
        self.title = title
        self.advice = advice


IDLE_ADVICE = "Nothing was changed. Try again once the panel is idle."
NO_ANSWER = "The panel did not answer, so the Flasher can't tell whether it is idle."
NO_IDLE = "The panel answered but did not say whether it is idle, so the Flasher can't tell."
PROBE_MAX_AGE = 10.0    # seconds: an older look at the panel says nothing about it now


def refuse_unless_idle(pr, advice=IDLE_ADVICE, running=False, max_age=None):
    """The idle gate before anything restarts a board (FLS-004, FLS2-001), on a look at it taken
    just before (probe.probe): PortError for a port that did not open; JobFailed for a panel that
    is armed, in installer mode, on probation or waiting for an update's confirmation, and for a
    RoboAlarms console that answered without saying whether it is idle. With running=True (a
    panel known to run RoboAlarms, or a request only its console answers) also when no console
    answered; with max_age, when the look is older than that many seconds. A board without a
    RoboAlarms console passes otherwise: a factory demo, other firmware, or a release image
    without a console (BL-211)."""
    if pr.error:
        raise PortError(pr.error["kind"], pr.error["message"], pr.error["advice"])
    if max_age is not None and (not pr.at or time.time() - pr.at > max_age):
        raise JobFailed("The Flasher's look at the panel is out of date.", advice)
    reason = pr.busy_reason
    if reason:
        raise JobFailed(reason, advice)
    if pr.answered and pr.idle is None:
        # Its `ota` answered but `status` listed no partition, or `dev describe` had no "idle".
        raise JobFailed(NO_IDLE, advice + " Look at it in Advanced tools › Console (`status`).")
    if running and not pr.answered:
        raise JobFailed(NO_ANSWER, advice + " If it is stuck, unplug it and plug it back in.")
    return pr


@dataclass
class Step:
    key: str
    label: str
    state: str = "waiting"    # waiting / running / done / skipped / failed / warning
    progress: float = 0.0
    detail: str = ""


@dataclass
class Job:
    kind: str
    port: str
    title: str
    steps: list
    image_key: str = ""
    id: str = field(default_factory=lambda: uuid.uuid4().hex[:8])
    state: str = "running"    # running / done / failed / cancelled
    error: dict | None = None
    result: dict = field(default_factory=dict)
    log: list = field(default_factory=list)
    started: float = field(default_factory=time.time)
    finished: float = 0.0
    cancel: threading.Event = field(default_factory=threading.Event, repr=False)

    def step(self, key):
        return next(s for s in self.steps if s.key == key)

    def to_dict(self):
        return {"id": self.id, "kind": self.kind, "port": self.port, "title": self.title,
                "image_key": self.image_key, "state": self.state, "error": self.error,
                "result": self.result, "steps": [asdict(s) for s in self.steps],
                "log": self.log[-200:], "started": self.started, "finished": self.finished}


def version_tuple(text):
    return tuple(int(n) for n in re.findall(r"\d+", str(text).split("-")[0])[:3])


# The Zigbee module's flash (FLS-030): ESP-IDF's partition table and app description.
PT_OFFSET = 0x8000
PT_SIZE = 0xC00
PT_ENTRY = 32
PT_MAGIC = 0x50AA
PT_MD5_MAGIC = 0xEBEB
PT_APP, PT_FACTORY, PT_OTA_0 = 0x00, 0x00, 0x10
APP_READ = 0x20 + 256          # the image and segment headers, then esp_app_desc_t
FACTORY_PROJECTS = ("ot_rcp", "esp_ot_cli", "esp_ot_rcp", "ot_cli", "ot_br")   # Espressif's OpenThread examples
OUR_RADIO_PROJECT = "roboalarms_h2_rcp"     # bundle.RADIO_KINDS["zigbee"]["project"]
RELEASE_VERSION = re.compile(r"^\d+\.\d+\.\d+")


def app_offset(table):
    """The app partition (factory, else ota_0) in a partition table's bytes, or None for an
    erased or invalid table."""
    found = {}
    for at in range(0, len(table) - PT_ENTRY + 1, PT_ENTRY):
        magic, ptype, subtype, offset, _size = struct.unpack_from("<HBBII", table, at)
        if magic != PT_MAGIC:
            break       # 0xFFFF ends the table; 0xEBEB is its MD5 entry
        if ptype == PT_APP and subtype in (PT_FACTORY, PT_OTA_0):
            found.setdefault(subtype, offset)
    return found.get(PT_FACTORY, found.get(PT_OTA_0))


def check_image_layout(image):
    """JobFailed, before any port is touched, unless every part of `image` goes where its own
    partition table and the chip put it (FLS2-002; bundle.check_layout)."""
    try:
        bundle_mod.check_layout(image)
    except bundle_mod.BundleError as e:
        raise JobFailed(f"This firmware would be written in the wrong places ({e}).",
                        "Use a bundle made from a normal build. Nothing was written.") from None


_NOT_IN_NAME = re.compile(r"[^A-Za-z0-9_-]+")
NAME_MAX = 48


def file_part(text, fallback):
    """`text` as one safe piece of a file name (FLS2-003): letters, digits, '-' and '_' only (so no
    path separator, drive, dot or NUL), at most NAME_MAX characters; `fallback` when nothing is
    left. A panel reports its board over serial: it must never pick where a backup goes."""
    name = _NOT_IN_NAME.sub("_", str(text or "")).strip("_-")[:NAME_MAX]
    return name or fallback


def classify_radio(project, version, elf, radio):
    """(runs, older, same) for an app's description against the bundle's radio (or None)."""
    if project == (radio.project if radio is not None else OUR_RADIO_PROJECT):
        runs = "ours"
    elif project in FACTORY_PROJECTS or project.startswith(("esp_ot_", "ot_")):
        runs = "factory"
    else:
        runs = "other"
    if runs != "ours" or radio is None:
        return runs, False, False
    # A build before the module had a version (a commit name, e.g. "171f515-dirty") is older.
    older = not RELEASE_VERSION.match(str(version)) or version_tuple(version) < version_tuple(radio.version)
    same = version == radio.version and (not radio.elf_sha256 or elf == radio.elf_sha256)
    return runs, older, same


def read_radio(session, radio):
    """What a connected module runs (FLS-030), read through the session's allow-listed reads:
    {"chip", "revision", "runs": ours/factory/other/empty, "project", "version", "older", "same"}.
    Raises JobFailed, nothing changed, when the chip isn't the radio's."""
    info = session.info
    want = radio.chip if radio is not None else "esp32h2"
    if info.chip != want:
        raise JobFailed(f"This isn't a Zigbee module (it is an {chip_name(info.chip)}).", "Nothing was changed.")
    out = {"chip": info.chip, "revision": info.revision, "runs": "empty", "project": "", "version": "",
           "older": False, "same": False, "radio_version": radio.version if radio is not None else ""}
    at = app_offset(session.read(PT_OFFSET, PT_SIZE))
    if at is None:
        return out
    head = session.read(at, APP_READ)
    try:
        app = parse_app(head)
    except ValueError:
        out["runs"] = "empty" if head[:1] == b"\xff" else "other"
        return out
    out["project"], out["version"] = app.project, app.version
    out["runs"], out["older"], out["same"] = classify_radio(app.project, app.version, app.elf_sha256, radio)
    return out


def watch_boot(port, timeout=BOOT_TIMEOUT, opener=serial_opener, on_line=None, cancel=None):
    """Read the boot log without restarting anything; stop at the firmware's "running" line.
    A set `cancel` (Stop) ends the wait within one open retry (0.3 s) or one serial read (the
    port's 0.05 s timeout), line or not, with found["cancelled"] (PERF22)."""
    found = {"board": "", "board_name": "", "running": False, "crashed": "", "brownout": "", "lines": [],
             "cancelled": False}
    stopped = cancel.is_set if cancel is not None else (lambda: False)
    deadline = time.monotonic() + timeout
    s = None
    while s is None and time.monotonic() < deadline and not stopped():
        try:
            s = open_quiet(port, opener=opener)
        except Exception:
            # the bridge may re-enumerate after a reset
            if cancel is not None:
                cancel.wait(0.3)
            else:
                time.sleep(0.3)
    if s is None:
        if stopped():
            found["cancelled"] = True
        else:
            found["crashed"] = "the port did not come back after the restart"
        return found
    try:
        def done(line):
            if on_line:
                on_line(line)
            m = BOOT_BOARD.search(line)
            if m:
                found["board_name"], found["board"] = m.group(1), m.group(2)
            if BOOT_CRASH.search(line) and not found["crashed"]:
                found["crashed"] = line.strip()
            if BOOT_BROWNOUT.search(line) and not found["brownout"]:
                found["brownout"] = line.strip()
            if BOOT_RUNNING.search(line):
                found["running"] = True
                return True
            return False
        lines, _ = read_lines_until(s, done, max(0.5, deadline - time.monotonic()), stop=stopped)
        found["lines"] = lines[-400:]
        found["cancelled"] = not found["running"] and stopped()
    finally:
        try:
            s.close()
        except Exception:
            pass
    return found


class Runner:
    """Runs one job at a time on a thread and reports it through emit(job_dict)."""

    def __init__(self, emit, catalogue, connect, probe, backups_dir, opener=serial_opener,
                 boot_timeout=BOOT_TIMEOUT, log_sink=None, on_boot_line=None):
        self.emit = emit
        self.catalogue = catalogue
        self.connect = connect          # (port, log, cancel, fast_baud) -> session
        self.probe = probe              # (port) -> ProbeResult
        self.backups_dir = Path(backups_dir)
        self.opener = opener
        self.boot_timeout = boot_timeout
        self.log_sink = log_sink        # fn(text) for the session log file
        self.on_boot_line = on_boot_line
        self.current = None
        self._last_emit = 0.0
        self._lock = threading.Lock()

    # -- plumbing -------------------------------------------------------------------------------
    def busy(self):
        return self.current is not None and self.current.state == "running"

    def _say(self, job, text):
        stamp = time.strftime("%H:%M:%S")
        job.log.append(f"{stamp} {text}")
        if self.log_sink:
            self.log_sink(f"[{job.kind} {job.port}] {text}")

    def _push(self, job, force=True):
        now = time.monotonic()
        if force or now - self._last_emit > 0.2:
            self._last_emit = now
            self.emit(job.to_dict())

    def _begin(self, job, key, detail=""):
        s = job.step(key)
        s.state, s.progress, s.detail = "running", 0.0, detail
        self._say(job, s.label + (f": {detail}" if detail else ""))
        self._push(job)
        return s

    def _end(self, job, key, detail=None, state="done"):
        s = job.step(key)
        s.state = state
        s.progress = 1.0 if state == "done" else s.progress
        if detail is not None:
            s.detail = detail
            self._say(job, f"{s.label}: {detail}")
        self._push(job)

    def _progress_fn(self, job, key):
        s = job.step(key)

        def report(done, total):
            s.progress = min(1.0, done / total) if total else 0.0
            self._push(job, force=False)
        return report

    def start(self, kind, port, title, steps, work, image_key=""):
        with self._lock:
            if self.busy():
                raise JobFailed("Another job is running.", "Wait for it to finish.")
            job = Job(kind=kind, port=port, title=title, image_key=image_key,
                      steps=[Step(k, label) for k, label in steps])
            self.current = job
        threading.Thread(target=self._run, args=(job, work), name=f"job {kind}", daemon=True).start()
        self._push(job)
        return job

    def cancel(self, job_id):
        job = self.current
        if job and job.id == job_id and job.state == "running":
            job.cancel.set()
            return True
        return False

    def _run(self, job, work):
        running_step = lambda: next((s for s in job.steps if s.state == "running"), None)  # noqa: E731
        try:
            work(job)
            if job.state == "running":
                job.state = "done"
        except Cancelled:
            step = running_step()
            job.state = "cancelled"
            harmed = step is not None and step.key in ("erase", "write", "verify")
            if step is not None and step.key == "boot":
                # PERF22: only the wait for it to start was stopped; the firmware is on the board.
                job.error = {"title": "Stopped.",
                             "advice": "The firmware was already written and the panel restarted; the Flasher "
                                       "stopped waiting for it to start. Look at its screen, or at Advanced "
                                       "tools › Console."}
                step.state = "skipped"
                step = None
            else:
                job.error = {"title": "Stopped.",
                             "advice": ("The board has no working firmware now. Run the same job again "
                                        "when you are ready: trying again cannot harm it.") if harmed else
                                       "Nothing was changed on the board."}
            if step:
                step.state = "failed"
        except (JobFailed, PortError) as e:
            step = running_step()
            if step:
                step.state = "failed"
            job.state = "failed"
            title = e.title if isinstance(e, JobFailed) else e.message
            job.error = {"title": title, "advice": e.advice}
        except Exception as e:  # esptool and serial failures
            from .chip import explain
            step = running_step()
            if step:
                step.state = "failed"
            title, advice = explain(e)
            if step is not None and step.key in ("erase", "write"):
                advice += " Until it is written again, the board has no working firmware."
            job.state = "failed"
            job.error = {"title": title, "advice": advice}
            self._say(job, f"error: {e!r}")
        finally:
            job.finished = time.time()
            if job.error:
                self._say(job, f"{job.state}: {job.error['title']} {job.error['advice']}")
            else:
                self._say(job, job.state)
            self._push(job)

    # -- shared steps ---------------------------------------------------------------------------
    def _check_idle(self, job, need_firmware=False):
        self._begin(job, "check", "looking at the panel without restarting it")
        pr = refuse_unless_idle(self.probe(job.port))
        if need_firmware and not pr.answered:
            raise JobFailed("The panel's firmware does not answer, so it cannot be updated in place.",
                            "Use Install instead (it erases settings), or look at its log in Advanced "
                            "tools › Console.")
        if pr.answered:
            what = f"RoboAlarms {pr.version or ''} {pr.profile or ''}".strip()
            self._end(job, "check", f"{what}, idle")
        else:
            self._end(job, "check", "no RoboAlarms firmware answered")
        job.result["probe"] = pr.to_dict()
        return pr

    def _connect(self, job, image, pr, confirmed, fast_baud):
        self._begin(job, "connect", "restarting the board into its loader")
        session = self.connect(job.port, lambda t: self._say(job, t), job.cancel, fast_baud)
        info = session.info
        job.result["chip"] = info.to_dict()
        facts = f"{chip_name(info.chip)} {rev_text(info.revision)}"
        if info.flash_size:
            facts += f", {info.flash_size // (1024 * 1024)} MB flash"
        facts += f", MAC {info.mac}"
        if image is not None:
            v = verdict(image, self.catalogue, pr, info)
            if not v.allowed:
                session.reset()
                raise JobFailed(v.blocked, "Choose the image made for this board. Nothing was written.")
            if v.confirm and not confirmed:
                session.reset()
                raise JobFailed(v.confirm, "Tick “I understand” on the Install step to go ahead. Nothing was "
                                           "written.")
        self._end(job, "connect", facts)
        return session

    def _backup(self, job, session, board_hint, module=""):
        info = session.info
        size = info.flash_size
        if not size:
            raise JobFailed("The flash size could not be read, so it cannot be backed up.",
                            "Try again; a backup needs the size.")
        self._begin(job, "backup", f"reading {size // (1024 * 1024)} MB")
        try:
            data = session.read(0, size, self._progress_fn(job, "backup"))
        except Cancelled:
            raise
        except Exception as e:
            if getattr(session, "fast_baud", None) == READ_BAUD:
                self._say(job, f"reading at {READ_BAUD} baud failed ({e}); again at {FAST_BAUD}")
                session.reconnect(FAST_BAUD)
                data = session.read(0, size, self._progress_fn(job, "backup"))
            else:
                raise
        if len(data) != size:
            raise JobFailed("The backup came back short.", "Try again. Nothing was erased.")
        self.backups_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        mac = re.sub(r"[^0-9a-f]", "", str(info.mac).lower())[:12] or "nomac"
        name = f"{file_part(board_hint, file_part(info.chip, 'board'))}-{mac}-{stamp}"
        path = self.backups_dir / f"{name}.bin"
        home = self.backups_dir.resolve()
        for p in (path, path.with_suffix(".json")):     # FLS2-003: a link placed there can't lead out
            if p.resolve().parent != home:
                raise JobFailed(f"The backup {p.name} would be saved outside {self.backups_dir}.",
                                "Nothing was erased. Remove what is in its place and try again.")
        digest = hashlib.sha256(data).hexdigest()
        path.write_bytes(data)
        side = {"format": 1, "board": board_hint, "chip": info.chip, "revision": info.revision,
                "mac": info.mac, "flash_size": size, "sha256": digest,
                "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds")}
        if module:
            side["module"] = module     # FLS-030: a copy of the Zigbee module, not of a panel
        path.with_suffix(".json").write_text(json.dumps(side, indent=2) + "\n")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise JobFailed(f"The backup file {path} does not read back correctly.",
                            "Check the disk has room and try again. Nothing was erased.")
        job.result["backup"] = {"path": str(path), "sha256": digest, "size": size}
        self._end(job, "backup", f"saved {path.name}")
        session.set_baud(FAST_BAUD)
        return path

    def _write(self, job, session, image, parts):
        self._begin(job, "write", ", ".join(p.role for p in parts))
        session.write([(p.offset, p.data) for p in parts], image.flash_mode, image.flash_freq,
                      self._progress_fn(job, "write"))
        self._end(job, "write", f"{sum(len(p.data) for p in parts)} bytes")
        self._begin(job, "verify", "reading back each part's checksum")
        for i, p in enumerate(parts):
            if session.md5(p.offset, len(p.data)) != p.md5:
                raise JobFailed(f"The {p.role} does not read back as written.",
                                "Try again, with another USB cable or port if it repeats.")
            job.step("verify").progress = (i + 1) / len(parts)
            self._push(job, force=False)
        self._end(job, "verify", "every part reads back as written")

    def _restart_and_watch(self, job, session, image):
        self._begin(job, "restart")
        session.reset()
        self._end(job, "restart", "")
        self._begin(job, "boot", "waiting for the panel to start")
        found = watch_boot(job.port, self.boot_timeout, self.opener, self.on_boot_line, job.cancel)
        job.result["boot"] = {k: v for k, v in found.items() if k != "lines"}
        if found["cancelled"]:
            raise Cancelled()       # PERF22: Stop while it was watched (_run says what is done)
        if found["running"]:
            words = f"running: {found['board_name'] or found['board'] or 'the panel'}"
            if image is not None and found["board"] and found["board"] != image.board:
                self._end(job, "boot", words + f" (expected {image.board})", state="warning")
                job.result["warning"] = (f"The panel says it is {found['board']}, but the image was for "
                                         f"{image.board}.")
            else:
                self._end(job, "boot", words)
            return
        why = found["brownout"] or found["crashed"] or f"no \"running\" line within {int(self.boot_timeout)} s"
        self._end(job, "boot", why, state="warning")
        board = self.catalogue.get(image.board if image is not None else found["board"]) or {}
        more = (board.get("connect") or {}).get("more_power") or "Give it more power, then restart it."
        if found["brownout"]:
            words = f"The firmware is written, but the panel ran short of power as it started ({why}). {more}"
        elif found["crashed"]:
            words = (f"The firmware is written, but the panel did not report starting ({why}). Look at its "
                     "screen; Advanced tools › Console shows its log.")
        else:
            words = (f"The firmware is written, but the panel did not report starting ({why}). Look at its "
                     f"screen: if it stayed black or the panel keeps restarting, it is most likely short of "
                     f"power. {more} "
                     "Advanced tools › Console shows its log.")
        job.result["warning"] = words

    # -- language packs (docs/features/37 language-packs.md "The Flasher") ---------------------------
    def _assets(self, image):
        """(offset, sectors) of the image's `assets` partition, or None when it has none."""
        found = packstore.find_partition(image.part("partition-table").data, "assets")
        if found is None or found[1] // packstore.SECTOR < 3:
            return None
        return found[0], found[1] // packstore.SECTOR

    def _mount(self, session, base, sectors):
        both = session.read(base, 2 * packstore.SECTOR)
        return packstore.Store.mount(both[:packstore.SECTOR], both[packstore.SECTOR:2 * packstore.SECTOR],
                                     sectors)

    def _carry_out(self, job, session, image, base, plan, what):
        """The plan's writes in order, each one read back (MD5) before the next: the table is never
        written before the blob it names checked out."""
        for offset, data in plan.writes:
            session.write([(base + offset, data)], image.flash_mode, image.flash_freq)
            if session.md5(base + offset, len(data)) != hashlib.md5(data).hexdigest():
                raise JobFailed(f"{what} does not read back as written.",
                                "Try again, with another USB cable or port if it repeats. The language "
                                "store was left as it was before this write.")
        return plan.store

    def _install_language(self, job, session, image, language, base, sectors):
        self._begin(job, "language", f"{language.name} ({language.locale})")
        store = self._mount(session, base, sectors)
        entry = packstore.Entry(packstore.KIND_LANGUAGE, 0, 0, 0, packstore.read_pack(language.data).payload_length,
                                b"", language.locale)
        try:
            store = self._carry_out(job, session, image, base, store.plan_put(entry, language.data),
                                    f"The {language.name} pack")
            self._carry_out(job, session, image, base,
                            store.plan_default(packstore.KIND_LANGUAGE, language.locale),
                            "The language store's table")
        except packstore.StoreError as e:
            raise JobFailed(f"The {language.name} pack cannot be stored: {e}.", "") from None
        self._end(job, "language", f"{language.name} installed and set as the panel's language")

    def _install_assets(self, job, session, image, assets, base, sectors):
        """Every asset pack the bundle carries, after the firmware and the language, none flagged
        default (OPT-007). A pack that does not fit is skipped and said: the firmware is installed
        and the panel fetches the pack later."""
        self._begin(job, "assets")
        store = self._mount(session, base, sectors)
        done, missed = [], []
        for a in assets:
            entry = packstore.Entry(a.kind, 0, 0, 0, packstore.read_asset(a.data).payload_length, b"", a.name)
            try:
                store = self._carry_out(job, session, image, base, store.plan_put(entry, a.data),
                                        f"The {a.name} pack")
            except packstore.StoreError as e:
                missed.append(a.name)
                self._say(job, f"{a.name}: not stored ({e}); the firmware is installed and the panel "
                               "fetches it later")
                continue
            done.append(a.name)
        if missed:
            job.result["assets_not_stored"] = missed
        self._end(job, "assets", ("installed: " + ", ".join(done) if done else "none installed")
                  + ("; did not fit: " + ", ".join(missed) if missed else ""))

    def _update_assets(self, job, session, image, assets, base, sectors):
        """Each installed asset pack whose version differs from the bundle's is replaced; one the
        panel lacks is installed; an entry of a kind this Flasher does not know is left alone."""
        self._begin(job, "assets")
        store = self._mount(session, base, sectors)
        done, missed = [], []
        for a in assets:
            if a.draft and image.profile == "release":
                self._say(job, f"{a.name}: the bundle's pack is a draft, which a release firmware "
                               "refuses; left as it is")
                continue
            e = store.find(a.kind, a.name)
            if e is not None and not packstore.payload_fits(e):
                # FLS2-004: a length its blob can't hold is never read; the pack is replaced.
                have = None
            elif e is not None:
                try:
                    have = packstore.read_asset_payload(
                        session.read(base + e.first_sector * packstore.SECTOR,
                                     -(-e.payload_length // 4) * 4)[:e.payload_length]).version
                except (ValueError, packstore.StoreError):
                    have = None
                if have == a.version:
                    self._say(job, f"{a.name}: already at {a.version}")
                    continue
            else:
                have = None
            entry = packstore.Entry(a.kind, e.flags if e else 0, 0, 0,
                                    packstore.read_asset(a.data).payload_length, b"", a.name)
            try:
                store = self._carry_out(job, session, image, base, store.plan_put(entry, a.data),
                                        f"The {a.name} pack")
            except packstore.StoreError as err:
                missed.append(a.name)
                self._say(job, f"{a.name}: not stored ({err}); the panel fetches it later")
                continue
            done.append(a.name)
            self._say(job, f"{a.name}: installed {a.version}" if e is None
                      else f"{a.name}: {have or 'unreadable'} to {a.version}")
        if missed:
            job.result["assets_not_stored"] = missed
        self._end(job, "assets", ("brought up to date: " + ", ".join(done)) if done
                  else "nothing to bring up to date")

    def _install_voice(self, job, session, image, voice, base, sectors):
        """The bundle's voice, as the store's one kind-2 entry ("primary", D08), after the other
        packs. A voice that does not fit is skipped and said: the panel works without one, and the
        panel records the pack's identity when it first loads it (docs/features/08)."""
        self._begin(job, "voice", voice.label())
        store = self._mount(session, base, sectors)
        entry = packstore.Entry(packstore.KIND_VOICE, 0, 0, 0, packstore.read_voice(voice.data).payload_length,
                                b"", packstore.VOICE_STORE_NAME)
        try:
            self._carry_out(job, session, image, base, store.plan_put(entry, voice.data), "The voice pack")
        except packstore.StoreError as e:
            job.result["voice_not_stored"] = voice.label()
            self._end(job, "voice", f"not stored ({e}): the panel works without a voice",
                      state="warning")
            return
        self._end(job, "voice", f"installed {voice.label()}")

    def _update_voice(self, job, session, image, voices, base, sectors):
        """The voice of the panel's language (ADR-053: a language and its voice are one unit): the
        language the store marks as the panel's (the one it was installed in), else the language of
        the voice it has when that language is installed (or English), else English. The panel
        keeps its chosen language in its settings, which the loader can't read: this is what the
        flash says. That language's voice is installed when the panel has none, has another
        language's or another voice, or an older revision; the same voice at the same revision is
        left (a revision is an identity: the panel refuses other audio under it), and so is a
        newer one, and the voice the panel has when the firmware has none for the language."""
        self._begin(job, "voice")
        store = self._mount(session, base, sectors)
        e = store.find(packstore.KIND_VOICE, packstore.VOICE_STORE_NAME)
        have = None
        if e is not None:
            try:
                have = packstore.read_voice_header(session.read(base + e.first_sector * packstore.SECTOR,
                                                                packstore.VOICE_HEADER))
            except packstore.StoreError:
                have = None      # replaced by the language's voice, when there is one
        installed = [x.name for x in store.entries if x.kind == packstore.KIND_LANGUAGE]
        default = next((x.name for x in store.entries
                        if x.kind == packstore.KIND_LANGUAGE and x.flags & packstore.FLAG_DEFAULT), "")
        locale = default or (have.locale if have is not None and (
            have.locale.lower() == bundle_mod.ENGLISH.lower()
            or have.locale.lower() in (n.lower() for n in installed)) else bundle_mod.ENGLISH)
        new = bundle_mod.pick_voice(voices, locale, image.profile)
        if new is None:
            self._end(job, "voice", f"no {locale} voice in this firmware; " + (
                f"the panel keeps {have.label()}" if have is not None else
                "the panel's voice pack cannot be read; left as it is" if e is not None else "the panel has none"))
            return
        if have is not None and (have.locale.lower(), have.voice_id) == (new.locale.lower(), new.voice_id) \
                and have.revision >= new.revision:
            self._end(job, "voice", f"already {have.label()}" if new.revision == have.revision
                      else f"{have.label()} is newer than the firmware's r{new.revision}; left as it is")
            return
        entry = packstore.Entry(packstore.KIND_VOICE, e.flags if e else 0, 0, 0,
                                packstore.read_voice(new.data).payload_length, b"", packstore.VOICE_STORE_NAME)
        try:
            self._carry_out(job, session, image, base, store.plan_put(entry, new.data), "The voice pack")
        except packstore.StoreError as err:
            self._end(job, "voice", f"not stored ({err}); the panel keeps its voice", state="warning")
            return
        self._end(job, "voice", f"installed {new.label()}" if have is None
                  else f"r{have.revision} to r{new.revision} ({new.locale} {new.voice_id})"
                  if (have.locale, have.voice_id) == (new.locale, new.voice_id)
                  else f"{have.label()} replaced by {new.label()}, the {locale} voice")

    def _update_languages(self, job, session, image, languages, base, sectors):
        self._begin(job, "language")
        store = self._mount(session, base, sectors)
        done = []
        for e in [e for e in store.entries if e.kind == packstore.KIND_LANGUAGE]:
            new = next((l for l in languages if l.locale == e.name), None)
            if new is None:
                self._say(job, f"{e.name}: not in this bundle, left as it is")
                continue
            if new.draft and image.profile == "release":
                self._say(job, f"{e.name}: the bundle's pack is a draft, which a release firmware refuses; "
                               "left as it is")
                continue
            try:
                have = packstore.read_pack_header(
                    session.read(base + e.first_sector * packstore.SECTOR, packstore.PACK_HEADER)).version
            except ValueError:
                have = None
            if have == new.version:
                self._say(job, f"{e.name}: already at {new.version}")
                continue
            entry = packstore.Entry(packstore.KIND_LANGUAGE, e.flags, 0, 0,
                                    packstore.read_pack(new.data).payload_length, b"", e.name)
            try:
                store = self._carry_out(job, session, image, base, store.plan_put(entry, new.data),
                                        f"The {new.name} pack")
            except packstore.StoreError as err:
                raise JobFailed(f"The {new.name} pack cannot be stored: {err}.", "") from None
            done.append(e.name)
            self._say(job, f"{e.name}: {have or 'unreadable'} to {new.version}")
        self._end(job, "language", ("brought up to date: " + ", ".join(done)) if done
                  else "nothing to bring up to date")

    # -- the jobs -------------------------------------------------------------------------------
    def install(self, port, image, backup=True, erase_settings_ok=False, confirmed=False, title=None,
                language=None, assets=(), voice=None):
        """`language`: a bundle.Language to set the panel up in (None or "": English, nothing written
        to the `assets` partition). `assets`: the bundle's asset packs, written after the firmware
        (and the language) when the image has an `assets` partition. `voice`: a bundle.Voice
        (bundle.pick_voice), written last."""
        language = language or None
        check_image_layout(image)
        if voice is not None and voice.draft and image.profile == "release":
            raise JobFailed(f"The voice {voice.label()} is a draft for testing, and a release firmware "
                            "refuses it.", "Use a bundle made from a final build. Nothing was written.")
        packs = [a for a in (assets or ()) if a.kind in bundle_mod.ASSET_KINDS]
        for a in packs:
            if a.draft and image.profile == "release":
                raise JobFailed(f"The {a.name} pack is a draft for testing, and a release firmware "
                                "refuses it.", "Use a bundle made from a final build. Nothing was written.")
        assets = None
        if language is not None:
            if language.draft and image.profile == "release":
                raise JobFailed(f"The {language.name} pack is a draft for testing, and a release firmware "
                                "refuses it.", "Choose English or another language. Nothing was written.")
            assets = self._assets(image)
            if assets is None:
                raise JobFailed("This firmware has no partition for languages.",
                                "Install in English. Nothing was written.")
        steps = [("check", "Check the panel"), ("connect", "Connect to the chip")]
        if backup:
            steps.append(("backup", "Back up the whole flash"))
        steps += [("erase", "Erase the flash"), ("write", "Write the firmware"),
                  ("verify", "Check what was written")]
        if language is not None:
            steps.append(("language", "Install the panel's language"))
        if packs and assets is None:
            assets = self._assets(image)
        if packs and assets is not None:
            steps.append(("assets", "Install the letters, picture and certificates"))
        else:
            packs = []
        if voice is not None and assets is None:
            assets = self._assets(image)
        if voice is not None and assets is not None:
            steps.append(("voice", "Install the voice"))
        else:
            voice = None
        steps += [("restart", "Restart the panel"), ("boot", "Wait for it to start")]

        def work(job):
            pr = self._check_idle(job)
            if pr.answered and not erase_settings_ok:
                raise JobFailed("This panel already runs RoboAlarms firmware, and an install erases its "
                                "codes, users, zones and Wi-Fi.",
                                "Use Update to keep them, or confirm erasing them.")
            session = self._connect(job, image, pr, confirmed, READ_BAUD if backup else FAST_BAUD)
            try:
                if backup:
                    self._backup(job, session, image.board)
                self._begin(job, "erase", "the whole chip")
                session.erase_all(self._progress_fn(job, "erase"))
                self._end(job, "erase", "done")
                self._write(job, session, image, image.parts)
                if language is not None:
                    self._install_language(job, session, image, language, *assets)
                if packs:
                    self._install_assets(job, session, image, packs, *assets)
                if voice is not None:
                    self._install_voice(job, session, image, voice, *assets)
                self._restart_and_watch(job, session, image)
            finally:
                session.close()
        return self.start("install", port, title or f"Install {image.board_name} {image.version}", steps,
                          work, image.key)

    def update(self, port, image, confirmed=False, downgrade_ok=False, languages=(), assets=(), voices=()):
        """`languages`: the bundle's packs; every language installed on the panel is brought to
        their version. `voices`: the bundle's voices (_update_voice says which is written)."""
        check_image_layout(image)
        languages = list(languages or ())
        voices = list(voices or ())
        packs = [a for a in (assets or ()) if a.kind in bundle_mod.ASSET_KINDS]
        assets = self._assets(image) if languages or packs or voices else None
        steps = [("check", "Check the panel"), ("connect", "Connect to the chip"),
                 ("compare", "Compare the layout"), ("write", "Write the firmware"),
                 ("verify", "Check what was written")]
        if assets is not None:
            if languages:
                steps.append(("language", "Bring the languages up to date"))
            if packs:
                steps.append(("assets", "Bring the letters, picture and certificates up to date"))
            if voices:
                steps.append(("voice", "Bring the voice up to date"))
        steps += [("restart", "Restart the panel"), ("boot", "Wait for it to start")]

        def work(job):
            pr = self._check_idle(job, need_firmware=True)
            if pr.version and version_tuple(image.version) < version_tuple(pr.version) and not downgrade_ok:
                raise JobFailed(f"The panel runs {pr.version}, newer than {image.version}.",
                                "Start the update again and confirm going back to the older version.")
            session = self._connect(job, image, pr, confirmed, FAST_BAUD)
            try:
                self._begin(job, "compare", "partition table and bootloader")
                pt = image.part("partition-table")
                if session.md5(pt.offset, len(pt.data)) != pt.md5:
                    session.reset()
                    raise JobFailed("This firmware lays the flash out differently from the one on the panel, "
                                    "so it cannot be updated in place.",
                                    "Install instead: it erases codes, users, zones and Wi-Fi. "
                                    "Nothing was written.")
                parts = [image.part("otadata"), image.part("app")]
                boot = image.part("bootloader")
                same_boot = session.md5(boot.offset, len(boot.data)) == boot.md5
                if not same_boot:
                    parts.insert(0, boot)
                self._end(job, "compare", "same layout" + ("" if same_boot else "; new bootloader"))
                self._write(job, session, image, sorted(parts, key=lambda p: p.offset))
                if assets is not None:
                    if languages:
                        self._update_languages(job, session, image, languages, *assets)
                    if packs:
                        self._update_assets(job, session, image, packs, *assets)
                    if voices:
                        self._update_voice(job, session, image, voices, *assets)
                self._restart_and_watch(job, session, image)
            finally:
                session.close()
        return self.start("update", port, f"Update to {image.version}", steps, work, image.key)

    def backup(self, port, board_hint=""):
        steps = [("check", "Check the panel"), ("connect", "Connect to the chip"),
                 ("backup", "Back up the whole flash"), ("restart", "Restart the panel")]

        def work(job):
            pr = self._check_idle(job)
            session = self._connect(job, None, pr, True, READ_BAUD)
            try:
                self._backup(job, session, board_hint or pr.board)
                self._begin(job, "restart")
                session.reset()
                self._end(job, "restart", "")
            finally:
                session.close()
        return self.start("backup", port, "Back up the flash", steps, work)

    def radio(self, port, radio, backup=False):
        """The Zigbee module's radio software (FLS-030), the module out of its socket on its own USB
        port: refuse a panel, require the radio's chip and silicon, keep a copy only when asked,
        erase it, write and check the three parts, restart it. No idle check, boot watch or console:
        the port may come back as another one after the restart, so the outcome is job.result."""
        steps = [("connect", "Connect to the module")]
        if backup:
            steps.append(("backup", "Keep a copy of what it runs"))
        steps += [("erase", "Erase it"), ("write", "Write the radio software"),
                  ("verify", "Check what was written"), ("restart", "Restart it")]

        def work(job):
            self._begin(job, "connect", "making sure no panel answers on this port")
            pr = self.probe(job.port)
            if pr.error:
                raise PortError(pr.error["kind"], pr.error["message"], pr.error["advice"])
            if pr.answered:
                raise JobFailed("This is a panel, not the Zigbee module.",
                                "Take the module out of the panel and plug its own USB-C port into this "
                                "computer. Nothing was changed.")
            session = self.connect(job.port, lambda t: self._say(job, t), job.cancel,
                                   READ_BAUD if backup else FAST_BAUD)
            try:
                info = session.info
                job.result["chip"] = info.to_dict()
                if info.chip != radio.chip:
                    session.reset()
                    raise JobFailed(f"This isn't a Zigbee module (it is an {chip_name(info.chip)}).",
                                    "Nothing was changed.")
                if not radio.accepts_rev(info.revision):
                    session.reset()
                    raise JobFailed(f"This module's chip is {rev_text(info.revision)}, and the radio software "
                                    f"is made for {radio.silicon_text()}.", "Nothing was changed.")
                if info.flash_size and radio.flash_bytes > info.flash_size:
                    session.reset()
                    raise JobFailed(f"This module has {info.flash_size // (1024 * 1024)} MB of flash, and the "
                                    f"radio software needs {radio.flash_size}.", "Nothing was changed.")
                self._end(job, "connect", f"{chip_name(info.chip)} {rev_text(info.revision)}, MAC {info.mac}")
                if backup:
                    self._backup(job, session, "zigbee_module", module="zigbee")
                self._begin(job, "erase", "the whole chip")
                session.erase_all(self._progress_fn(job, "erase"))
                self._end(job, "erase", "done")
                self._write(job, session, radio, sorted(radio.parts, key=lambda p: p.offset))
                self._begin(job, "restart")
                session.reset()
                self._end(job, "restart", "")
                job.result.update({"radio_version": radio.version, "next": "Unplug its cable and put it aside"})
            finally:
                session.close()
        return self.start("radio", port, f"Install the Zigbee module's radio software {radio.version}", steps, work)

    def restore(self, port, path, mac_ok=False):
        path = Path(path)
        steps = [("check", "Check the panel"), ("connect", "Connect to the chip"),
                 ("write", "Write the backup"), ("verify", "Check what was written"),
                 ("restart", "Restart the panel")]

        def work(job):
            try:
                side = json.loads(path.with_suffix(".json").read_text())
            except (OSError, ValueError):
                raise JobFailed("This backup has no description file next to it (.json).",
                                "Only backups the Flasher made can be restored here.") from None
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != side.get("sha256") or len(data) != side.get("flash_size"):
                raise JobFailed("The backup file is damaged (its checksum does not match).", "")
            pr = self._check_idle(job)
            session = self._connect(job, None, pr, True, FAST_BAUD)
            try:
                info = session.info
                if info.chip != side.get("chip") or (info.flash_size and info.flash_size != len(data)):
                    session.reset()
                    raise JobFailed(f"This backup is from an {chip_name(side.get('chip', '?'))} with "
                                    f"{len(data) // (1024 * 1024)} MB of flash; this board is different.",
                                    "Nothing was written.")
                if info.mac != side.get("mac") and not mac_ok:
                    session.reset()
                    raise JobFailed(f"This backup was taken from another board (MAC {side.get('mac')}).",
                                    "Restore it again and confirm it goes onto this board. Nothing was written.")
                from .bundle import Part
                whole = Part("backup", 0, path.name, side["sha256"], data)
                self._begin(job, "write", f"{len(data) // (1024 * 1024)} MB")
                session.write([(0, data)], "keep", "keep", self._progress_fn(job, "write"))
                self._end(job, "write", "done")
                self._begin(job, "verify")
                if session.md5(0, len(data)) != whole.md5:
                    raise JobFailed("The restored flash does not read back as written.", "Try again.")
                self._end(job, "verify", "reads back as written")
                self._begin(job, "restart")
                session.reset()
                self._end(job, "restart", "")
            finally:
                session.close()
        return self.start("restore", port, f"Restore {path.name}", steps, work)
