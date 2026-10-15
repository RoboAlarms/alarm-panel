"""Synthetic firmware images and bundles for the host tests and the --mock window: real
ESP-IDF header layouts (the same offsets imagefile.py reads), made-up contents."""
import hashlib
import random
import struct
import tempfile
import zlib
from pathlib import Path

from . import bundle as bundle_mod
from .bundle import Image, Part
from .imagefile import APP_DESC_MAGIC, CHIP_IDS

CHIP_ID = {v: k for k, v in CHIP_IDS.items()}
LAYOUT = {  # role -> offset, as the builds' flasher_args.json have them
    "esp32p4": {"bootloader": 0x2000, "partition-table": 0xC000, "otadata": 0x2D000, "app": 0x30000},
    "esp32": {"bootloader": 0x1000, "partition-table": 0xC000, "otadata": 0x1D000, "app": 0x20000},
}
TEST_KEY = bytes(range(256)) + bytes(range(132))   # 388 bytes: "n" and "e" of a made-up key
# The partition tables of firmware/crowpanel_p4 and firmware/e32r40t as ESP-IDF lays them out:
# (label, type, subtype, offset, size). LAYOUT's otadata and app offsets are theirs (FLS2-002).
P4_TABLE = [("nvs", 1, 0x02, 0xD000, 0x20000), ("otadata", 1, 0x00, 0x2D000, 0x2000),
            ("phy_init", 1, 0x01, 0x2F000, 0x1000), ("ota_0", 0, 0x10, 0x30000, 0x480000),
            ("ota_1", 0, 0x11, 0x4B0000, 0x480000), ("nvs_keys", 1, 0x04, 0x930000, 0x1000),
            ("assets", 1, 0x83, 0x931000, 0x280000), ("cp_fw", 1, 0x06, 0xBB1000, 0x180000),
            ("fw_cache", 1, 0x06, 0xD31000, 0x200000), ("eventlog", 1, 0x06, 0xF31000, 0x60000),
            ("coredump", 1, 0x03, 0xF91000, 0x10000)]
E32_TABLE = [("nvs", 1, 0x02, 0xD000, 0x10000), ("otadata", 1, 0x00, 0x1D000, 0x2000),
             ("phy_init", 1, 0x01, 0x1F000, 0x1000), ("ota_0", 0, 0x10, 0x20000, 0x1E0000),
             ("ota_1", 0, 0x11, 0x200000, 0x1E0000), ("eventlog", 1, 0x06, 0x3E0000, 0x20000)]
# A synthetic image's table by default: the real one without `assets`, so that only the tests that
# ask for one (table=P4_TABLE, or their own) install packs.
DEFAULT_TABLES = {"esp32p4": [t for t in P4_TABLE if t[0] != "assets"], "esp32": E32_TABLE}


def synthetic_app(chip="esp32p4", min_rev=1, max_rev=199, project="alarm_crowpanel_p4-dev",
                  version="0.6.0", size=192 * 1024, signed=True, key=TEST_KEY, seed=1):
    rnd = random.Random(seed)
    b = bytearray(rnd.getrandbits(8) for _ in range(size))
    b[0] = 0xE9
    struct.pack_into("<H", b, 12, CHIP_ID[chip])
    b[14] = min_rev // 100
    struct.pack_into("<H", b, 15, min_rev)
    struct.pack_into("<H", b, 17, max_rev)
    struct.pack_into("<I", b, 32, APP_DESC_MAGIC)
    b[48:80] = version.encode().ljust(32, b"\0")
    b[80:112] = project.encode().ljust(32, b"\0")
    b[144:176] = b"v6.1".ljust(32, b"\0")
    b[176:208] = hashlib.sha256((project + version + str(seed)).encode()).digest()
    if signed:
        b += b"\xff" * ((-len(b)) % 4096)
        b += signature_sector(key)
    return bytes(b)


def signature_sector(key=TEST_KEY):
    """The 4 KiB a signed image ends with: one Secure Boot v2 block (made-up signature) with the
    CRC-32 espsecure checks."""
    sector = bytearray(b"\xff" * 4096)
    sector[0:4] = b"\xe7\x02\x00\x00"
    sector[36:36 + 388] = key
    struct.pack_into("<I", sector, 1196, zlib.crc32(bytes(sector[:1196])))
    return bytes(sector)


def synthetic_image(board="crowpanel_p4", board_name="CrowPanel 7\"", chip="esp32p4", min_rev=1,
                    max_rev=199, profile="dev", version="0.6.0", flash_size="16MB", app_size=192 * 1024,
                    pt_seed=7, key=TEST_KEY, table=None):
    """`table`: the partition table's entries (DEFAULT_TABLES by default); another `pt_seed` adds a
    partition, which lays the flash out differently."""
    project = f"alarm_{board}" + ("" if profile == "legacy" else f"-{profile}")
    app = synthetic_app(chip, min_rev, max_rev, project, version, app_size, key=key, seed=min_rev + 3)
    rnd = random.Random(pt_seed)
    entries = list(table if table is not None else DEFAULT_TABLES[chip])
    if pt_seed != 7:
        end = max(offset + size for _, _, _, offset, size in entries)
        entries.append((f"spare{pt_seed}", 1, 0x06, end, 0x1000))
    pieces = {"bootloader": bytes(rnd.getrandbits(8) for _ in range(20 * 1024)),
              "partition-table": partition_table(entries),
              "otadata": b"\xff" * 8192, "app": app}
    parts = [Part(role, LAYOUT[chip][role], f"{role}.bin", hashlib.sha256(data).hexdigest(), data)
             for role, data in pieces.items()]
    parts.sort(key=lambda p: p.offset)
    from .imagefile import parse_app
    a = parse_app(app)
    return Image(board=board, board_name=board_name, variant=f"v{max_rev // 100}", profile=profile,
                 project=project, version=version, elf_sha256=a.elf_sha256, chip=chip,
                 chip_rev_min=min_rev, chip_rev_max=max_rev, flash_size=flash_size, flash_mode="dio",
                 flash_freq="80m", partition_table_sha256=hashlib.sha256(pieces["partition-table"]).hexdigest(),
                 signing_key_sha256=a.signing_key_sha256, parts=parts)


def standard_images(version="0.6.0", app_size=192 * 1024):
    return [synthetic_image(version=version, app_size=app_size),
            synthetic_image(min_rev=301, max_rev=399, version=version, app_size=app_size),
            synthetic_image(board="e32r40t", board_name="E32R40T 4\"", chip="esp32", min_rev=300,
                            max_rev=399, profile="legacy", version=version, flash_size="4MB",
                            app_size=app_size),
            synthetic_image(board="crowpanel_10", board_name="CrowPanel 10.1\"", version=version,
                            app_size=app_size)]


RADIO_PROJECT = "roboalarms_h2_rcp"
RADIO_VERSION = "1.0.0"


def partition_table(entries, md5=True):
    """ESP-IDF partition table bytes (0xC00) from [(label, type, subtype, offset, size)], ending with
    the MD5 entry gen_esp32part.py adds (md5=False: without it)."""
    table = bytearray(b"\xff" * 0xC00)
    for i, (label, ptype, subtype, offset, size) in enumerate(entries):
        struct.pack_into("<HBBII16sI", table, i * 32, 0x50AA, ptype, subtype, offset, size,
                         label.encode().ljust(16, b"\0"), 0)
    if md5:
        at = len(entries) * 32
        table[at:at + 32] = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(bytes(table[:at])).digest()
    return bytes(table)


RADIO_TABLE = [("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
               ("factory", 0, 0, 0x10000, 0x1F0000)]


def radio_flash(project=RADIO_PROJECT, version=RADIO_VERSION, size=4 * 1024 * 1024, seed=21, table=RADIO_TABLE):
    """A Zigbee module's whole flash: a bootloader, a partition table and an app (FLS-030)."""
    flash = bytearray(b"\xff" * size)
    flash[0:4] = b"\xe9\x03\x02\x10"
    pt = partition_table(table)
    flash[0x8000:0x8000 + len(pt)] = pt
    app = synthetic_app("esp32h2", 0, 199, project, version, 48 * 1024, signed=False, seed=seed)
    flash[0x10000:0x10000 + len(app)] = app
    return flash


def synthetic_radio(version=RADIO_VERSION, project=RADIO_PROJECT, chip="esp32h2", min_rev=0, max_rev=199):
    """The Zigbee module's software as a bundle carries it (bundle.Radio): three parts, unsigned."""
    app = synthetic_app(chip, min_rev, max_rev, project, version, 64 * 1024, signed=False, seed=11)
    pieces = {"bootloader": b"\xe9" + bytes(16 * 1024 - 1), "partition-table": partition_table(RADIO_TABLE),
              "app": app}
    offsets = bundle_mod.RADIO_KINDS["zigbee"]["parts"]
    parts = [Part(role, offsets[role], "", "", data) for role, data in pieces.items()]
    from .imagefile import parse_app
    return bundle_mod.Radio(kind="zigbee", chip=chip, project=project, version=version,
                            elf_sha256=parse_app(app).elf_sha256, chip_rev_min=min_rev, chip_rev_max=max_rev,
                            flash_size="2MB", flash_mode="dio", flash_freq="48m", parts=parts,
                            source={"commit": "test", "dirty": False})


def bundle_bytes(images, signing="test", radios=()):
    with tempfile.TemporaryDirectory() as d:
        path = bundle_mod.write(Path(d) / "test.roboflash", images, "2026-09-24T00:00:00+00:00",
                                {"commit": "test"}, images[0].signing_key_sha256, (), radios)
        return path.read_bytes()


def standard_bundle(version="0.6.0", app_size=192 * 1024, radio=True):
    """The panels' images, and (radio=True) the Zigbee module's software, so --mock has one."""
    radios = [synthetic_radio()] if radio else []
    return bundle_mod.load(bundle_bytes(standard_images(version, app_size), radios=radios),
                           name=f"test-{version}.roboflash")


# -- packs and a firmware server, for --mock and the tests ---------------------------------------
TOOLS = Path(__file__).resolve().parents[2]      # tools/ of a checkout (not in an installed Flasher)
TEST_KEY_DIGEST = hashlib.sha256(TEST_KEY).hexdigest()


def _tool(name):
    """tools/<name>.py of the checkout, which makes the packs (i18n.py, asset_pack.py)."""
    import importlib.util
    import sys
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        sys.modules[name] = module
    return sys.modules[name]


def language_file(locale="es-US", version="0.6.0", draft=False, key=TEST_KEY):
    """A signed-looking .robolang (made-up signature) of i18n.py's fixture catalogue."""
    i18n = _tool("i18n")
    cat = i18n.fixture_catalog()
    keys = [i18n.key_hash(c, m) for c, m, _, _ in i18n.FIXTURE_ITEMS]
    payload = i18n.build_pack_payload(locale, i18n.FIXTURE_NAME, cat.rule, draft, i18n.catalog_layout(cat),
                                      keys, version)
    return i18n.unsigned_pack_file(payload)[:-4096] + signature_sector(key)


def asset_file(kind, version="0.6.0", draft=False, size=5000, key=TEST_KEY):
    """A signed-looking .roboasset (made-up signature) of made-up bytes."""
    from . import packstore
    ap = _tool("asset_pack")
    part = {packstore.KIND_LETTERS: ("latin", packstore.PART_FONT),
            packstore.KIND_PICTURE: ("photo", packstore.PART_JPEG),
            packstore.KIND_CERTS: ("bundle", packstore.PART_CERTS)}[kind]
    payload = ap.build_payload(kind, packstore.ASSET_STORE_NAMES[kind], version,
                               [(part[0], part[1], bytes([kind]) * size)], draft=draft)
    return payload + b"\xff" * (-len(payload) % 4096) + signature_sector(key)


def voice_file(revision=2, locale="en-US", voice_id=None, draft=False, key=TEST_KEY):
    """A signed-looking .robovoice (made-up signature) of the frozen RVOC fixture with its identity changed."""
    import re
    from . import packstore
    fixture = TOOLS.parent / "components" / "core" / "voice_pack" / "test" / "voice_pack_fixture_v2.h"
    body = fixture.read_text(encoding="utf-8").split("voice_pack_fixture_v2[", 1)[1].split("{", 1)[1]
    p = bytearray(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body.split("}", 1)[0]))
    struct.pack_into("<I", p, 12, revision)
    struct.pack_into("<I", p, 20, packstore.VOICE_FLAG_DRAFT if draft else 0)
    p[24:40] = locale.encode().ljust(16, b"\0")
    if voice_id is not None:
        p[40:72] = voice_id.encode().ljust(32, b"\0")
    return bytes(p) + b"\xff" * (-len(p) % 4096) + signature_sector(key)


class FakeServer:
    """A firmware server in memory with fwserver.Client's interface (address, url, get_json, fetch),
    for --mock and the tests. Releases come from add(), newest last; `offline` makes it unreachable
    (what it answered and sent before is still there, as Client's cache keeps it); `delay` (seconds a
    file) slows downloads so the page shows them."""

    def __init__(self, address="https://firmware.example", delay=0.0):
        self.address = address
        self.base = address + "/"
        self.releases = []          # newest first
        self.packs = {}             # (board, version, project) -> {"languages": [...], "assets": [...]}
        self.files = {}             # url -> bytes
        self.flasher = {"boards": None, "programs": []}
        self.offline = False
        self.delay = delay
        self.asked = []
        self.fetched = []
        self._said = {}
        self._kept = set()

    def url(self, path, params=None):
        import urllib.parse
        url = urllib.parse.urljoin(self.base, str(path))
        if params:
            url += "?" + urllib.parse.urlencode({k: v for k, v in params.items() if v not in (None, "")})
        return url

    def file(self, data):
        sha = hashlib.sha256(data).hexdigest()
        url = f"/dl/{sha[:16]}"
        self.files[url] = bytes(data)
        return {"url": url, "size": len(data), "sha256": sha}

    def add(self, board, channel, version, bundle, project=None, languages=(), voices=(), assets=(),
            key=None, rules=None, chip_revs=("v1", "v3")):
        """languages, voices: [(locale, file bytes)]; assets: [(kind name, file bytes)]."""
        project = project or f"alarm_{board}-{'dev' if channel == 'dev' else 'release'}"
        key = key or {"name": "Test key", "sha256": "", "flasher_sha256": TEST_KEY_DIGEST}
        entry = {"project": project, "rules": rules or {}, "chip_revs": list(chip_revs),
                 "languages": [loc for loc, _ in languages], "signing_key": key, **self.file(bundle)}
        self.releases.insert(0, {"version": version, "board": board, "channel": channel,
                                 "published": "2026-10-12T00:00:00Z", "critical": False, "notes": "",
                                 "notes_url": f"{self.address}/releases/{board}/{version}", "bundles": [entry]})
        langs = {}
        for loc, data in voices:
            langs.setdefault(loc, {"locale": loc})["voice"] = {"revision": 1, "schema": 1, **self.file(data)}
        for loc, data in languages:
            langs.setdefault(loc, {"locale": loc}).update(self.file(data))
        self.packs[(board, version, project)] = {
            "languages": sorted(langs.values(), key=lambda e: (e["locale"] != "en-US", e["locale"])),
            "assets": [{"kind": kind, **self.file(data)} for kind, data in assets]}
        return entry

    def _answer(self, path, q):
        from .fwserver import channel_sees
        if path == "/api/v1/boards":
            boards = sorted({r["board"] for r in self.releases})
            return {"boards": [{"id": b, "aliases": [], "projects": [f"alarm_{b}-release", f"alarm_{b}-dev"],
                                "dev_projects": [f"alarm_{b}-dev"],
                                "chip_revs": [{"id": "v1", "silicon_min": 1, "silicon_max": 199},
                                              {"id": "v3", "silicon_min": 301, "silicon_max": 399}]}
                               for b in boards], "peripherals": []}
        if path == "/api/v1/flasher":
            return self.flasher
        if path == "/api/v1/releases":
            out = []
            for r in self.releases:
                if r["board"] == q.get("device") and channel_sees(q.get("channel"), r["channel"]):
                    bundles = [b for b in r["bundles"] if not q.get("project") or b["project"] == q["project"]]
                    if bundles:
                        out.append({**r, "bundles": bundles})
            return {"releases": out}
        if path == "/api/v1/languages":
            got = self.packs.get((q.get("device"), q.get("version"), q.get("project")))
            return {"version": q.get("version"), **(got or {"languages": [], "assets": []})}
        raise KeyError(path)

    def get_json(self, path, params=None):
        from .fwserver import ServerError
        params = dict(params or {})
        self.asked.append((path, params))
        url = self.url(path, params)
        if self.offline:
            if url in self._said:
                return self._said[url], True
            raise ServerError("The Flasher can't reach the firmware server.", f"{self.address}: offline.")
        try:
            answer = self._answer(path, params)
        except KeyError:
            raise ServerError("The firmware server refused the request (404).", url) from None
        self._said[url] = answer
        return answer, False

    def fetch(self, url, size, sha256, what="a file", progress=None):
        import time
        from .fwserver import ServerError
        if self.offline and sha256 not in self._kept:
            raise ServerError(f"The download of {what} stopped.", "offline. Check again: it goes on where it stopped.")
        data = self.files.get(url) if not url.startswith(self.base) else self.files.get("/" + url[len(self.base):])
        if data is None:
            raise ServerError(f"The firmware server would not send {what} (404).", url)
        self.fetched.append(url)
        if self.delay and sha256 not in self._kept:
            for n in range(1, 9):
                time.sleep(self.delay / 8)
                if progress:
                    progress(len(data) * n // 8, size)
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha256:
            raise ServerError(f"The download of {what} is damaged.", "Check again to download it again.")
        self._kept.add(sha256)
        if progress:
            progress(size, size)
        return data


def mock_server(delay=1.5, app_size=192 * 1024):
    """The --mock window's firmware server: for the CrowPanel 7", 0.6.0 on stable (release builds) and
    dev (development builds) and 0.6.1 on beta; for the 10.1" only the development build (the
    Flasher offers it, FLS-033). Each with the saver picture, letters and certificates, English's
    voice and Spanish with its voice. Without the checkout's tools (an installed Flasher) the
    releases carry no packs."""
    srv = FakeServer(delay=delay)
    boards = {"crowpanel_p4": ('CrowPanel 7"', [(1, 199), (301, 399)]), "crowpanel_10": ('CrowPanel 10.1"', [(1, 199)])}
    for channel, version, profile in (("stable", "0.6.0", "release"), ("beta", "0.6.1", "release"),
                                      ("dev", "0.6.0", "dev")):
        dev = profile == "dev"
        try:
            packs = {"languages": [("es-US", language_file("es-US", version, draft=dev))],
                     "voices": [("en-US", voice_file(3 if dev else 2, "en-US", draft=dev)),
                                ("es-US", voice_file(1, "es-US", "es_us_test", draft=dev))],
                     "assets": [(name, asset_file(kind, version)) for name, kind in
                                (("letters", 3), ("picture", 4), ("certificates", 5))]}
        except Exception:
            packs = {}
        for board, (name, revs) in boards.items():
            if board == "crowpanel_10" and channel != "dev":
                continue
            images = [synthetic_image(board=board, board_name=name, min_rev=lo, max_rev=hi, profile=profile,
                                      version=version, app_size=app_size) for lo, hi in revs]
            srv.add(board, channel, version, bundle_bytes(images, radios=[synthetic_radio()]),
                    chip_revs=[f"v{hi // 100}" for _, hi in revs], **packs)
    return srv
