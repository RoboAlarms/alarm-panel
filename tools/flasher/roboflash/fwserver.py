"""The firmware server (FLS-032 to FLS-034; the update server's API): where the
Flasher gets the panels' firmware, the language packs, voices and asset packs that go with it, newer
board descriptions and word of a newer Flasher, so that none of them is built into the Flasher.

  Client        the server's public API over HTTPS, its downloads kept in a Cache by SHA-256: a
                file is fetched once, a download that stopped resumes where it stopped (Range,
                If-Range on the server's ETag), and the last answers are remembered so that what
                was downloaded before is there when the server can't be reached.
  ServerSource  what the Flasher offers from the server: per board and release channel, the newest
                release's bundle with its languages, voices and asset packs (a "target"), checked
                before it is offered; plus the board descriptions and the newer-Flasher notice.

What is trusted (decided 2026-10-12): a release whose signing key isn't built into the Flasher
(TRUSTED_KEYS) is refused before anything is downloaded, and every image and pack downloaded must
carry a signature that checks out with a trusted key (imagefile.verified_key_digest). The parts
that are not signed (bootloader, partition table, otadata) are covered by the SHA-256 the server
gave, over HTTPS; plain HTTP is accepted only for a server on this computer (a test server).
A file opened by hand (Advanced tools > Software) is checked as before (bundle.load).
"""
import hashlib
import json
import os
import platform
import queue
import re
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

from . import __version__
from . import boards as boards_mod
from . import bundle as bundle_mod
from . import packstore
from . import paths
from .imagefile import verified_key_digest

DEFAULT_URL = "https://firmware.roboalarms.com"
ENV_URL = "ROBOALARMS_FIRMWARE_SERVER"
CHANNELS = ("stable", "beta", "alpha", "dev")
# The release the Flasher takes by itself (decided 2026-10-12): the newest Stable, else Beta, else
# Alpha. A development build only once the person says so: the page asks first, since it comes with
# test zones and the developer tools.
AUTOMATIC = ("stable", "beta", "alpha")
FOUND = ("available", "downloading", "ready")
# Which releases each channel offers: a channel sees itself and the steadier ones; dev
# builds are a world of their own.
CHANNEL_SEES = {"stable": ("stable",), "beta": ("beta", "stable"), "alpha": ("alpha", "beta", "stable"),
                "dev": ("dev",)}
# The release keys the Flasher installs from the server: the Flasher's digest of the key (the SHA-256
# of the signature block's n and e, imagefile.public_key_digest) -> its name.
TRUSTED_KEYS = {"5d2a2ffe298a3250df960597aba743c907d932268f8d56b3827f5324795ae905": "RoboAlarms Release"}
TIMEOUT = 20
MAX_ANSWER = 4 * 1024 * 1024
MAX_FILE = bundle_mod.MAX_TOTAL
CACHE_LIMIT = 768 * 1024 * 1024
REMEMBERED = 200
CHUNK = 64 * 1024
ASSET_KINDS = {"letters": packstore.KIND_LETTERS, "picture": packstore.KIND_PICTURE,
               "certificates": packstore.KIND_CERTS}
_SHA = re.compile(r"^[0-9a-f]{64}$")
_SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
LOOPBACK = ("localhost", "127.0.0.1", "::1")


class ServerError(Exception):
    """What went wrong, in words for the page: a title and what to do."""

    def __init__(self, title, advice=""):
        super().__init__(title)
        self.title = title
        self.advice = advice

    def to_dict(self):
        return {"title": self.title, "advice": self.advice}


def check_url(url):
    """The server's address, cleaned; ValueError when it can't be one: https, or http only for a
    server on this computer."""
    url = str(url or "").strip().rstrip("/")
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("https", "http") or not parts.hostname:
        raise ValueError(f"{url!r} is not a web address (https://...)")
    if parts.scheme == "http" and parts.hostname not in LOOPBACK:
        raise ValueError(f"{url}: the firmware server must be reached over https "
                         "(plain http only for a test server on this computer)")
    if parts.query or parts.fragment:
        raise ValueError(f"{url}: give the server's address without ? or #")
    return url


def transport_ok(url, came_from=None):
    """Whether the Flasher may ask `url` (FLS-032): https, or plain http to this computer for a test
    server. A redirect (`came_from`, FLS2-005) meets the same rule and never goes from https down
    to http, even to this computer."""
    parts = urllib.parse.urlsplit(str(url))
    try:
        host = parts.hostname
    except ValueError:
        return False
    if parts.scheme == "https":
        return bool(host)
    if parts.scheme != "http" or host not in LOOPBACK:
        return False
    return came_from is None or urllib.parse.urlsplit(str(came_from)).scheme == "http"


class PolicyRedirects(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to where transport_ok() allows, checked before the next request goes
    out (FLS2-005); urllib's own limits (hops, loops) still hold."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not transport_ok(newurl, req.full_url):
            try:
                fp.close()
            except Exception:
                pass
            raise ServerError("The firmware server sent the Flasher somewhere it won't go.",
                              f"It answered {req.full_url} with a redirect ({code}) to {newurl}, which is not "
                              "an https address. Nothing was downloaded from there.")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def server_url(given=None):
    """--server, else ROBOALARMS_FIRMWARE_SERVER, else the project's server."""
    return check_url(given or os.environ.get(ENV_URL) or DEFAULT_URL)


def platform_id():
    """This computer as the server names a Flasher program's platform."""
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine)
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux" if sys.platform.startswith("linux")
                                                       else sys.platform)
    return f"{system}-{arch}"


def semver(text):
    m = _SEMVER.match(str(text or ""))
    return tuple(int(n) for n in m.groups()) if m else None


def _ssl_context():
    """The system's certificates, and certifi's when it is there (a frozen macOS build has no others)."""
    ctx = ssl.create_default_context()
    try:
        import certifi
        ctx.load_verify_locations(certifi.where())
    except Exception:
        pass
    return ctx


def _reason(e):
    r = getattr(e, "reason", None) or e
    return str(getattr(r, "strerror", None) or r)


class Cache:
    """Downloads kept by SHA-256 (each checked again when used), and the server's last answers."""

    def __init__(self, root=None, limit=CACHE_LIMIT):
        self.root = Path(root or paths.cache_dir())
        self.limit = limit
        self._lock = threading.Lock()
        self._answers = None

    def _dir(self):
        try:
            self.root.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            raise ServerError("The Flasher can't keep its downloads.", f"{self.root}: {e.strerror or e}") from None
        return self.root

    def path(self, sha256):
        return self.root / sha256

    def part_path(self, sha256):
        return self._dir() / (sha256 + ".part")

    def get(self, sha256, size):
        """The cached file's bytes when it is whole and right, else None (a damaged one is dropped)."""
        p = self.path(sha256)
        try:
            if p.stat().st_size != size:
                return None
            data = p.read_bytes()
        except OSError:
            return None
        if hashlib.sha256(data).hexdigest() != sha256:
            p.unlink(missing_ok=True)
            return None
        try:
            os.utime(p)       # used now: the last to go when the cache is pruned
        except OSError:
            pass
        return data

    def put(self, sha256, part):
        os.replace(part, self.path(sha256))
        self.prune(keep={sha256})

    def prune(self, keep=()):
        """The least recently used files go once the cache is over its limit."""
        try:
            files = [p for p in self.root.iterdir() if _SHA.match(p.name)]
            files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
            total = 0
            for p in files:
                total += p.stat().st_size
                if total > self.limit and p.name not in keep:
                    p.unlink(missing_ok=True)
        except OSError:
            pass

    def _answers_path(self):
        return self.root / "answers.json"

    def _load_answers(self):
        if self._answers is None:
            try:
                self._answers = json.loads(self._answers_path().read_text(encoding="utf-8"))
                if not isinstance(self._answers, dict):
                    self._answers = {}
            except (OSError, ValueError):
                self._answers = {}
        return self._answers

    def answer(self, url):
        with self._lock:
            return self._load_answers().get(url)

    def remember(self, url, answer):
        with self._lock:
            answers = self._load_answers()
            answers.pop(url, None)
            answers[url] = answer
            while len(answers) > REMEMBERED:
                answers.pop(next(iter(answers)))
            try:
                tmp = self._dir() / "answers.json.tmp"
                tmp.write_text(json.dumps(answers), encoding="utf-8")
                os.replace(tmp, self._answers_path())
            except (OSError, ServerError):
                pass


class Client:
    """The update server's public API."""

    def __init__(self, base_url, cache=None, timeout=TIMEOUT):
        self.base = check_url(base_url) + "/"
        self.cache = cache or Cache()
        self.timeout = timeout
        self._ctx = _ssl_context()
        self._opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=self._ctx),
                                                   PolicyRedirects())
        self.agent = f"RoboAlarms-Flasher/{__version__}"

    @property
    def address(self):
        return self.base.rstrip("/")

    def url(self, path, params=None):
        url = urllib.parse.urljoin(self.base, str(path))
        if params:
            url += "?" + urllib.parse.urlencode({k: v for k, v in params.items() if v not in (None, "")})
        return url

    def _open(self, url, headers=None):
        if not transport_ok(url):
            raise ServerError("The firmware server sent the Flasher somewhere it won't go.",
                              f"{url} is not an https address. Nothing was downloaded.")
        req = urllib.request.Request(url, headers={"User-Agent": self.agent, **(headers or {})})
        return self._opener.open(req, timeout=self.timeout)   # every redirect checked (PolicyRedirects)

    def get_json(self, path, params=None):
        """(answer, offline): the server's answer, or the one remembered from last time (offline=True)
        when it can't be reached. ServerError when there is neither."""
        url = self.url(path, params)
        try:
            with self._open(url, {"Accept": "application/json"}) as r:
                data = r.read(MAX_ANSWER + 1)
            if len(data) > MAX_ANSWER:
                raise ServerError("The firmware server's answer is too large.", url)
            answer = json.loads(data)
        except urllib.error.HTTPError as e:
            raise ServerError(f"The firmware server refused the request ({e.code}).", _error_words(e, url)) from None
        except (urllib.error.URLError, OSError) as e:
            remembered = self.cache.answer(url)
            if remembered is not None:
                return remembered, True
            raise ServerError("The Flasher can't reach the firmware server.",
                              f"{self.address}: {_reason(e)}. Check this computer's internet connection, "
                              "then check again.") from None
        except ValueError:
            raise ServerError("The firmware server's answer can't be read.", url) from None
        if not isinstance(answer, dict):
            raise ServerError("The firmware server's answer can't be read.", url)
        self.cache.remember(url, answer)
        return answer, False

    def fetch(self, url, size, sha256, what="a file", progress=None):
        """The file at `url` (relative to the server) that the server says has `size` bytes and this
        SHA-256: from the cache, or downloaded, resumed where an earlier download stopped, and checked."""
        sha256 = str(sha256 or "").lower()
        if not _SHA.match(sha256) or not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= MAX_FILE:
            raise ServerError("The firmware server describes a file wrongly.", f"{what}: size {size!r}, "
                              f"SHA-256 {sha256!r}. Nothing was downloaded.")
        data = self.cache.get(sha256, size)
        if data is not None:
            if progress:
                progress(size, size)
            return data
        url = self.url(url)
        part = self.cache.part_path(sha256)
        have = part.stat().st_size if part.exists() else 0
        if have >= size:
            part.unlink(missing_ok=True)
            have = 0
        headers = {"Range": f"bytes={have}-", "If-Range": f'"{sha256}"'} if have else {}
        try:
            with self._open(url, headers) as r:
                resumed = have and r.status == 206 and str(r.headers.get("Content-Range", "")).startswith(f"bytes {have}-")
                got = have if resumed else 0
                with open(part, "ab" if resumed else "wb") as f:
                    while True:
                        chunk = r.read(CHUNK)
                        if not chunk:
                            break
                        got += len(chunk)
                        if got > size:
                            break
                        f.write(chunk)
                        if progress:
                            progress(got, size)
        except urllib.error.HTTPError as e:
            if e.code == 416:
                part.unlink(missing_ok=True)
            raise ServerError(f"The firmware server would not send {what} ({e.code}).",
                              _error_words(e, url)) from None
        except (urllib.error.URLError, OSError) as e:
            raise ServerError(f"The download of {what} stopped.",
                              f"{_reason(e)}. Check again: it goes on where it stopped.") from None
        data = part.read_bytes()
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha256:
            part.unlink(missing_ok=True)
            raise ServerError(f"The download of {what} is damaged.",
                              "It is not what the firmware server says it is. Check again to download it again.")
        try:
            self.cache.put(sha256, part)
        except OSError:
            pass
        return data


def _error_words(e, url):
    """The server's own words from an error answer ({"error": ...}), or the address."""
    try:
        body = json.loads(e.read(65536) or b"{}")
        words = body.get("error") if isinstance(body, dict) else None
        if isinstance(words, dict):
            words = words.get("message") or words.get("title")
        if isinstance(words, str) and words:
            return words[:300]
    except Exception:
        pass
    return url


def channel_sees(channel, release_channel):
    return release_channel in CHANNEL_SEES.get(channel, ())


def rules_problem(rules, peripherals):
    """Why a bundle's rules (requires, excludes, board_revs) don't fit the parts chosen
    (server peripheral ids, or None when not chosen), or ""."""
    rules = rules if isinstance(rules, dict) else {}
    if rules.get("board_revs"):
        return "This release is made for particular board revisions, which the Flasher can't check."
    requires = list(rules.get("requires") or ())
    excludes = list(rules.get("excludes") or ())
    if not requires and not excludes:
        return ""
    if peripherals is None:
        return "This release depends on the parts connected to the panel. Choose them under Connected parts."
    have = set(peripherals)
    missing = [p for p in requires if p not in have]
    if missing:
        return f"This release needs {', '.join(missing)}, which isn't among the parts you chose."
    clash = [p for p in excludes if p in have]
    if clash:
        return f"This release doesn't work with {', '.join(clash)}, which is among the parts you chose."
    return ""


def peripherals_of(choice):
    """The server's peripheral ids (its catalogue) for a Connected parts choice (parts.normalise)."""
    if not isinstance(choice, dict):
        return None
    out = []
    rf = choice.get("rf")
    if rf == "module":
        out.append("rf433")
    elif rf == "cc1101":
        out.append("cc1101")
    elif rf == "auto":
        out.append("unknown")
    if choice.get("clock"):
        out.append("ds3231")
    if choice.get("zigbee"):
        out.append("crowpanel_h2")
    if choice.get("zwave"):
        out.append("zac93")
    return out


def choose(answer, project, channel):
    """(release, bundle) from a releases answer, newest first: the first release on a channel the
    asked one offers with a bundle for `project`, a bundle without rules first (one for every
    panel); (None, None) when there is none."""
    for release in answer.get("releases") or ():
        if not isinstance(release, dict) or not channel_sees(channel, release.get("channel")):
            continue
        bundles = [b for b in release.get("bundles") or () if isinstance(b, dict) and b.get("project") == project]
        if bundles:
            plain = [b for b in bundles if not any((b.get("rules") or {}).values())]
            return release, (plain or bundles)[0]
    return None, None


@dataclass
class Target:
    """One board's newest release on one channel, from asking to ready to install."""
    board: str
    channel: str
    project: str = ""
    release_channel: str = ""   # the channel the release found is on (a channel sees the steadier ones)
    state: str = "waiting"      # waiting, checking, available, downloading, ready, none, failed
    version: str = ""
    published: str = ""
    notes_url: str = ""
    critical: bool = False
    bundle_sha256: str = ""
    rules: dict = field(default_factory=dict)
    size: int = 0               # what the download comes to: bundle and packs
    done: int = 0
    error: dict = None
    offline: bool = False       # the server could not be reached: from what was downloaded before
    fetch: bool = False         # download it, not only look (a board is here, or asked for)
    queued: bool = False
    bundle: object = None       # the checked bundle_mod.Bundle once ready

    @property
    def key(self):
        return (self.board, self.channel)

    @property
    def origin(self):
        return f"{self.board} {self.version} ({self.channel})"

    def to_dict(self):
        return {"board": self.board, "channel": self.channel, "project": self.project, "state": self.state,
                "release_channel": self.release_channel, "version": self.version, "published": self.published, "notes_url": self.notes_url,
                "critical": self.critical, "size": self.size, "done": self.done, "error": self.error,
                "offline": self.offline, "ready_version": self.ready_version}

    @property
    def ready_version(self):
        """The version ready to install (the newest one may still be on its way)."""
        return self.bundle.images[0].version if self.bundle is not None and self.bundle.images else ""


class ServerSource:
    """What the Flasher offers from the firmware server. One worker thread asks and downloads, in
    the order things were asked for; on_change(what) ("targets" or "catalogue") tells the controller.

    Each board's release is looked up on every channel; channel_for() says which one it installs
    from (FLS-033).

    `verify` gives the trusted-key digest a file's signature checks out with ("" when none); --mock
    and the tests give their own with test keys."""

    def __init__(self, client, on_change=None, log=None, trusted=None, verify=verified_key_digest, frozen=None):
        self.client = client
        self.on_change = on_change or (lambda what: None)
        self.log = log or (lambda text: None)
        self.trusted = TRUSTED_KEYS if trusted is None else trusted
        self.verify = verify
        self.frozen = getattr(sys, "frozen", False) if frozen is None else frozen
        self.pick = None                # the channel the person chose, for this run; None: AUTOMATIC
        self.picks = {}                 # board -> the channel chosen for it alone (None: AUTOMATIC)
        self.catalogue = None           # the server's boards answer (GET /api/v1/boards)
        self.boards_version = ""        # newer board descriptions put in use from the server
        self.program = None             # {"version", "url"}: a newer Flasher for this computer
        self.checked = None             # when the server last answered (time.time())
        self.offline = False
        self.error = None               # the start-up's own trouble ({"title", "advice"})
        self.targets = {}
        self._lock = threading.RLock()
        self._work = queue.Queue()
        self._thread = None
        self._last_note = 0.0

    # -- asked from the controller --------------------------------------------------------------
    def start(self, boards=()):
        """Ask for the board descriptions, the newer-Flasher notice and the server's catalogue,
        then look up (not download) each board's newest release on every channel."""
        self._work.put(self._start_up)
        for board in boards:
            self.look_up(board)
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, name="firmware server", daemon=True)
            self._thread.start()

    def stop(self):
        self._work.put(None)

    def look_up(self, board):
        for channel in CHANNELS:
            self.want(board, channel, fetch=False)

    def set_channel(self, channel, board=None):
        """The person's choice for this run, or None ("" or "auto") for the automatic order: for one
        board (the page's version list), else for every board (Software, which clears the boards'
        own). Not remembered: the next run starts from the automatic order again."""
        channel = None if channel in (None, "", "auto") else channel
        if channel is not None and channel not in CHANNELS:
            raise ValueError(f"unknown channel {channel!r}")
        with self._lock:
            if board:
                self.picks[board] = channel
            else:
                self.pick, self.picks = channel, {}
        self.on_change("targets")

    def picked(self, board):
        """The channel the person chose for `board`, or None."""
        with self._lock:
            return self.picks.get(board) if board in self.picks else self.pick

    def channel_for(self, board):
        """The channel `board`'s firmware comes from: the person's pick, else the first of Stable,
        Beta and Alpha with a release (one still being asked, or that can't be asked, holds the
        order there), else Dev, which is only offered (dev_offered)."""
        with self._lock:
            if self.picked(board):
                return self.picked(board)
            for channel in AUTOMATIC:
                t = self.targets.get((board, channel))
                if t is None or t.state != "none":
                    return channel
            return "dev"

    def dev_offered(self, board):
        """Whether `board` has nothing but a development build, which the person hasn't chosen: the
        page offers it, and nothing installs until they say so."""
        with self._lock:
            t = self.targets.get((board, "dev"))
            return not self.picked(board) and self.channel_for(board) == "dev" and t is not None and t.state in FOUND

    def want(self, board, channel=None, fetch=True):
        """The target for `board` on `channel` (channel_for's by default), looked up and, with
        fetch, downloaded. Asking again does nothing until it failed (or was refreshed)."""
        channel = channel or self.channel_for(board)
        with self._lock:
            t = self.targets.get((board, channel))
            if t is None:
                t = self.targets[(board, channel)] = Target(board, channel)
            again = (fetch and not t.fetch and t.state in ("available", "waiting", "checking")) or t.state == "waiting"
            t.fetch = t.fetch or fetch
            if t.queued or not again:
                return t
            t.queued = True
        self._work.put(lambda: self._prepare(t))
        return t

    def retry(self, board, channel=None):
        with self._lock:
            t = self.targets.get((board, channel or self.channel_for(board)))
            if t is None or t.queued:
                return
            t.queued = True
            t.fetch = True
        self._work.put(lambda: self._prepare(t))

    def refresh(self):
        """Check again: the start-up questions, then every target (what is ready stays in use
        until a newer release replaces it)."""
        self._work.put(self._start_up)
        with self._lock:
            todo = [t for t in self.targets.values() if not t.queued]
            for t in todo:
                t.queued = True
        for t in todo:
            self._work.put(lambda t=t: self._prepare(t))

    def target(self, board, channel=None):
        with self._lock:
            return self.targets.get((board, channel or self.channel_for(board)))

    def ready(self):
        """The targets ready to install from: each board's on its channel_for, never an offered
        development build."""
        with self._lock:
            return sorted((t for t in self.targets.values() if t.bundle is not None
                           and t.channel == self.channel_for(t.board) and not self.dev_offered(t.board)),
                          key=lambda t: t.board)

    def to_dict(self):
        with self._lock:
            boards = sorted({t.board for t in self.targets.values()})
            return {"address": self.client.address, "pick": self.pick, "channels": list(CHANNELS),
                    "automatic": list(AUTOMATIC),
                    "picked": {b: self.picked(b) for b in boards},
                    "chosen": {b: self.channel_for(b) for b in boards},
                    "dev_offered": [b for b in boards if self.dev_offered(b)],
                    "checked": self.checked, "offline": self.offline, "error": self.error,
                    "boards_version": self.boards_version, "program": self.program,
                    "targets": [t.to_dict() for t in sorted(self.targets.values(),
                                                            key=lambda t: (t.board, CHANNELS.index(t.channel)))]}

    # -- the worker -----------------------------------------------------------------------------
    def _run(self):
        while True:
            job = self._work.get()
            if job is None:
                return
            try:
                job()
            except Exception as e:      # never the end of the worker
                self.log(f"firmware server: {e!r}")

    def _changed(self, what="targets", throttle=False):
        now = time.monotonic()
        if throttle and now - self._last_note < 0.25:
            return
        self._last_note = now
        self.on_change(what)

    def _start_up(self):
        try:
            catalogue, offline = self.client.get_json("/api/v1/boards")
            with self._lock:
                self.catalogue = catalogue
                self.offline = offline
                self.error = None
                if not offline:
                    self.checked = time.time()
        except ServerError as e:
            with self._lock:
                self.error = e.to_dict()
                self.offline = True
            self.log(f"firmware server: {e.title} {e.advice}")
        try:
            # An older server has no such question: the Flasher goes on without.
            flasher, _ = self.client.get_json("/api/v1/flasher", {"boards_format": boards_mod.BOARDS_FORMAT})
            self._board_descriptions(flasher.get("boards"))
            self._program(flasher.get("programs"))
        except ServerError as e:
            self.log(f"firmware server: no board descriptions or Flasher news: {e.title}")
        self._changed("targets")

    def _board_descriptions(self, entry):
        """Newer board descriptions put in use when they check out; trouble is logged and
        the Flasher goes on with the ones it has."""
        if not isinstance(entry, dict):
            return
        version = str(entry.get("version", ""))
        have = boards_mod.document().get("version")
        if boards_mod.version_key(version) <= boards_mod.version_key(have):
            return
        try:
            data = self.client.fetch(entry.get("url", ""), entry.get("size"), entry.get("sha256"),
                                     "the board descriptions")
            if self.verify(data) not in self.trusted:
                raise ServerError("The board descriptions are not signed with a key this Flasher trusts.")
            info, doc = packstore.read_boards_pack(data)
            why = boards_mod.check_document(doc)
            if why:
                raise ServerError(f"The board descriptions can't be used: {why}.")
            if info.version != version or str(doc.get("version")) != version:
                raise ServerError(f"The board descriptions say version {doc.get('version')}, the server {version}.")
        except (ServerError, packstore.StoreError) as e:
            self.log(f"firmware server: board descriptions {version}: {getattr(e, 'title', e)}")
            return
        boards_mod.use_document(doc)
        with self._lock:
            self.boards_version = version
        self.log(f"firmware server: board descriptions {version} in use (built in: {have})")
        self._changed("catalogue")
        for board in boards_mod.load_catalogue():
            self.look_up(board)

    def _program(self, programs):
        """The newer-Flasher notice: only for a Flasher that was installed (not run from
        the source), for this computer, newer than this one. The Flasher never replaces itself."""
        mine = platform_id()
        best = None
        for p in programs or ():
            if not isinstance(p, dict) or p.get("platform") != mine or semver(p.get("version")) is None:
                continue
            if best is None or semver(p["version"]) > semver(best["version"]):
                best = p
        newer = best is not None and self.frozen and semver(__version__) is not None \
            and semver(best["version"]) > semver(__version__)
        url = self.client.url(best.get("url", "")) if newer else ""
        with self._lock:
            self.program = {"version": best["version"], "url": url} if newer and url.startswith("https://") else None

    def _server_board(self, board):
        for b in (self.catalogue or {}).get("boards") or ():
            if isinstance(b, dict) and (b.get("id") == board or board in (b.get("aliases") or ())):
                return b
        return None

    def project(self, board, channel):
        """The project a channel's builds for `board` are: a dev channel's are development builds."""
        profile = "dev" if channel == "dev" else "release"
        want = f"alarm_{board}-{profile}"
        entry = self._server_board(board)
        if entry:
            projects = [p for p in entry.get("projects") or () if isinstance(p, str)]
            dev = set(entry.get("dev_projects") or ())
            if want not in projects:
                pool = [p for p in projects if (p in dev) == (profile == "dev")]
                if pool:
                    return pool[0]
        return want

    def _prepare(self, t):
        with self._lock:
            t.queued = False
            t.state = "checking" if t.bundle is None else t.state
            t.error = None
        self._changed()
        try:
            self._prepare_target(t)
        except Exception as e:
            if not isinstance(e, ServerError):
                e = ServerError("The Flasher couldn't get this firmware ready.", f"{e!r}. Check again.")
            with self._lock:
                t.error = e.to_dict()
                t.state = "ready" if t.bundle is not None else "failed"
            self.log(f"firmware server: {t.board} ({t.channel}): {e.title} {e.advice}")
        self._changed()

    def _prepare_target(self, t):
        t.project = self.project(t.board, t.channel)
        answer, offline = self.client.get_json("/api/v1/releases", {"device": t.board, "channel": t.channel,
                                                                     "project": t.project})
        release, entry = choose(answer, t.project, t.channel)
        if release is None:
            with self._lock:
                t.state, t.bundle, t.version, t.release_channel, t.offline = "none", None, "", "", offline
            return
        key = entry.get("signing_key") or {}
        if key.get("flasher_sha256") not in self.trusted:
            raise ServerError(f"RoboAlarms {release.get('version')} for this board is signed with a key this "
                              f"Flasher doesn't trust ({key.get('name') or 'unnamed'}).",
                              "The Flasher installs only firmware signed with the RoboAlarms release key. "
                              "Nothing was downloaded.")
        if t.bundle is not None and t.bundle_sha256 == entry.get("sha256"):
            with self._lock:
                t.state, t.offline = "ready", offline
            return
        with self._lock:
            t.version = str(release.get("version", ""))
            t.release_channel = str(release.get("channel", ""))
            t.published = str(release.get("published", ""))
            t.notes_url = str(release.get("notes_url", ""))
            t.critical = bool(release.get("critical"))
            t.rules = entry.get("rules") or {}
            t.offline = offline
            t.size = entry.get("size") if isinstance(entry.get("size"), int) else 0
            t.done = 0
            if not t.fetch:
                t.state = "available" if t.bundle is None else t.state
                return
            t.state = "downloading"
        self._changed()
        langs = self._languages(t, entry)
        files = [e for e in langs.get("languages") or () if isinstance(e, dict) and e.get("url")]
        voices = [e["voice"] for e in langs.get("languages") or ()
                  if isinstance(e, dict) and isinstance(e.get("voice"), dict)]
        assets = [e for e in langs.get("assets") or () if isinstance(e, dict) and e.get("kind") in ASSET_KINDS]
        with self._lock:
            t.size += sum(e.get("size") for e in files + voices + assets if isinstance(e.get("size"), int))
        b = self._bundle(t, entry)
        b.languages = [self._language(t, e) for e in files]
        b.voices = [self._voice(t, e) for e in voices]
        b.assets = [self._asset(t, e) for e in assets]
        versions = {i.version for i in b.images}
        for a in b.assets:
            if a.version not in versions:
                raise ServerError(f"The {a.name} pack is made for version {a.version}, the firmware is "
                                  f"{', '.join(sorted(versions))}.", "Check again later. Nothing was installed.")
        b.name = f"firmware server {t.version} ({t.channel})"
        with self._lock:
            t.bundle, t.bundle_sha256, t.state = b, entry.get("sha256"), "ready"
        self.log(f"firmware server: {t.board} {t.version} ({t.channel}) ready: "
                 + ", ".join([i.key for i in b.images] + [l.locale for l in b.languages]
                             + [v.label() for v in b.voices] + [a.name for a in b.assets]))

    def _languages(self, t, entry):
        revs = [r for r in entry.get("chip_revs") or () if isinstance(r, str)]
        if not revs:
            server_board = self._server_board(t.board) or {}
            revs = [r.get("id") for r in server_board.get("chip_revs") or () if isinstance(r, dict) and r.get("id")]
        if not revs:
            return {}
        answer, _ = self.client.get_json("/api/v1/languages", {"device": t.board, "chip_rev": revs[0],
                                                               "version": t.version, "project": t.project})
        return answer

    def _progress(self, t, base):
        def step(got, size):
            with self._lock:
                t.done = base + got
            self._changed(throttle=True)
        return step

    def _get(self, t, e, what):
        base = t.done
        data = self.client.fetch(e.get("url", ""), e.get("size"), e.get("sha256"), what, self._progress(t, base))
        with self._lock:
            t.done = base + len(data)
        if self.verify(data) not in self.trusted:
            raise ServerError(f"{what[0].upper() + what[1:]} is not signed with a key this Flasher trusts.",
                              "Nothing was installed from this release.")
        return data

    def _bundle(self, t, entry):
        what = f"RoboAlarms {t.version}"
        data = self.client.fetch(entry.get("url", ""), entry.get("size"), entry.get("sha256"), what,
                                 self._progress(t, 0))
        with self._lock:
            t.done = len(data)
        try:
            b = bundle_mod.load(data, name=what)
        except bundle_mod.BundleError as e:
            raise ServerError(f"{what} from the firmware server can't be used.", str(e)) from None
        mine = [i for i in b.images if i.board == t.board and i.project == t.project and i.version == t.version]
        if not mine:
            raise ServerError(f"{what} from the firmware server has no build for this board.",
                              f"It holds {', '.join(i.key for i in b.images)}.")
        for image in mine:
            if self.verify(image.part("app").data) not in self.trusted:
                raise ServerError(f"{what} ({image.key}) is not signed with a key this Flasher trusts.",
                                  "Nothing was installed from this release.")
            image.origin = t.origin
        b.images, b.languages, b.assets, b.voices = mine, [], [], []
        return b

    def _language(self, t, e):
        locale = str(e.get("locale", ""))
        data = self._get(t, e, f"the {locale} language pack")
        try:
            info = packstore.read_pack(data)
        except ValueError as err:
            raise ServerError(f"The {locale} language pack can't be used.", str(err)) from None
        if info.locale.lower() != locale.lower():
            raise ServerError(f"The {locale} language pack is for {info.locale}.", "Nothing was installed from it.")
        return bundle_mod.Language(locale=info.locale, name=info.name, version=info.version, draft=info.draft,
                                   path=str(e.get("url", "")), sha256=hashlib.sha256(data).hexdigest(), data=data,
                                   origin=t.origin)

    def _voice(self, t, e):
        data = self._get(t, e, "a voice")
        try:
            info = packstore.read_voice(data)
        except packstore.StoreError as err:
            raise ServerError("A voice from the firmware server can't be used.", str(err)) from None
        return bundle_mod.Voice(locale=info.locale, voice_id=info.voice_id, revision=info.revision, draft=info.draft,
                                path=str(e.get("url", "")), sha256=hashlib.sha256(data).hexdigest(), data=data,
                                origin=t.origin)

    def _asset(self, t, e):
        kind = e["kind"]
        data = self._get(t, e, f"the {kind} pack")
        try:
            info = packstore.read_asset(data)
        except packstore.StoreError as err:
            raise ServerError(f"The {kind} pack can't be used.", str(err)) from None
        if info.kind != ASSET_KINDS[kind]:
            raise ServerError(f"The {kind} pack is another kind of pack ({info.kind}).", "Nothing was installed from it.")
        return bundle_mod.Asset(kind=info.kind, name=info.name, version=info.version, draft=info.draft,
                                path=str(e.get("url", "")), sha256=hashlib.sha256(data).hexdigest(), data=data,
                                origin=t.origin)
