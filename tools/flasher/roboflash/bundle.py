"""A firmware bundle (FLS-015): one zip file, `*.roboflash`, holding `manifest.json` and, for
each image, the four parts `idf.py flash` writes (bootloader, partition table, otadata, app).

Opening a bundle checks everything before anything can be flashed from it: the manifest's
format, that every part is present with its SHA-256, safe names inside the zip, and that the
app's own header agrees with what the manifest says about it (chip, silicon range, project,
version, ELF hash). make_bundle.py writes bundles; this module reads them.

A bundle may also carry a radio module's own software in the optional list `radios` (the
Zigbee module's ESP32-H2, firmware/radios/h2_rcp): unsigned (the H2 has no secure boot), each
part checked by its SHA-256 and the app's header against the manifest. The list is apart from
`images` so that a Flasher that knows no radios ignores it and still opens the bundle; radios
never take part in choosing or installing a panel's image.
"""
import hashlib
import io
import json
import logging
import struct
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

from .imagefile import parse_app, rev_text, chip_name, NO_MAX_REV
from .packstore import read_pack, read_asset, read_voice, KIND_LETTERS, KIND_PICTURE, KIND_CERTS, StoreError

FORMAT = 1
# The asset packs (docs/features/39 asset-packs.md) this Flasher installs: store kinds 3-5.
ASSET_KINDS = (KIND_LETTERS, KIND_PICTURE, KIND_CERTS)
EXTENSION = ".roboflash"
ROLES = ("bootloader", "partition-table", "otadata", "app")
MAX_TOTAL = 64 * 1024 * 1024
# The radios this Flasher knows: kind -> what its software must be. The offsets are where the
# chip's ROM and bootloader look (boards.json has the same under the board's "zigbee" "radio").
RADIO_KINDS = {
    "zigbee": {"chip": "esp32h2", "project": "roboalarms_h2_rcp",
               "parts": {"bootloader": 0x0, "partition-table": 0x8000, "app": 0x10000}},
}

log = logging.getLogger(__name__)


class BundleError(ValueError):
    pass


def flash_size_bytes(text):
    """'16MB' -> 16777216."""
    text = str(text).strip().upper()
    if text.endswith("MB") and text[:-2].isdigit():
        return int(text[:-2]) * 1024 * 1024
    if text.endswith("KB") and text[:-2].isdigit():
        return int(text[:-2]) * 1024
    raise BundleError(f"flash size {text!r}")


@dataclass
class Part:
    role: str
    offset: int
    path: str
    sha256: str
    data: bytes = field(repr=False, default=b"")

    @property
    def md5(self):
        return hashlib.md5(self.data).hexdigest()


@dataclass
class Image:
    board: str
    board_name: str
    variant: str
    profile: str
    project: str
    version: str
    elf_sha256: str
    chip: str
    chip_rev_min: int
    chip_rev_max: int
    flash_size: str
    flash_mode: str
    flash_freq: str
    partition_table_sha256: str
    signing_key_sha256: str
    parts: list
    origin: str = ""     # the firmware server's release it came from (fwserver), "" from a file

    @property
    def key(self):
        return f"{self.board}-{self.variant}-{self.profile}"

    @property
    def flash_bytes(self):
        return flash_size_bytes(self.flash_size)

    def part(self, role):
        return next(p for p in self.parts if p.role == role)

    def accepts_rev(self, rev):
        return self.chip_rev_min <= rev <= self.chip_rev_max

    def silicon_text(self):
        return _silicon_text(self.chip_rev_min, self.chip_rev_max)

    def to_dict(self):
        return {"key": self.key, "board": self.board, "board_name": self.board_name,
                "variant": self.variant, "profile": self.profile, "project": self.project,
                "version": self.version, "elf_sha256": self.elf_sha256, "chip": self.chip,
                "chip_name": chip_name(self.chip), "chip_rev_min": self.chip_rev_min,
                "chip_rev_max": self.chip_rev_max, "silicon": self.silicon_text(),
                "flash_size": self.flash_size, "signing_key_sha256": self.signing_key_sha256,
                "size": sum(len(p.data) for p in self.parts), "origin": self.origin}

    def manifest_entry(self):
        return {"board": self.board, "board_name": self.board_name, "variant": self.variant,
                "project": self.project, "profile": self.profile, "version": self.version,
                "elf_sha256": self.elf_sha256, "chip": self.chip,
                "chip_rev_min": self.chip_rev_min, "chip_rev_max": self.chip_rev_max,
                "flash_size": self.flash_size, "flash_mode": self.flash_mode,
                "flash_freq": self.flash_freq,
                "partition_table_sha256": self.partition_table_sha256,
                "signing_key_sha256": self.signing_key_sha256,
                "files": [{"role": p.role, "offset": f"{p.offset:#x}", "path": p.path,
                           "sha256": p.sha256} for p in self.parts]}


def _silicon_text(rev_min, rev_max):
    if rev_max >= NO_MAX_REV:
        return f"silicon {rev_text(rev_min)} or newer"
    if rev_min <= 100:
        return f"silicon up to {rev_text(rev_max)}"
    return f"silicon {rev_text(rev_min)} to {rev_text(rev_max)}"


@dataclass
class Radio:
    """A radio module's own software (FLS-015): never a panel image, never signed."""
    kind: str
    chip: str
    project: str
    version: str
    elf_sha256: str
    chip_rev_min: int
    chip_rev_max: int
    flash_size: str
    flash_mode: str
    flash_freq: str
    parts: list
    rcp_version: str = ""      # the OpenThread RCP's own version string, when the build gave it
    source: dict = field(default_factory=dict)   # {commit, dirty} of its source folder

    @property
    def flash_bytes(self):
        return flash_size_bytes(self.flash_size)

    def part(self, role):
        return next(p for p in self.parts if p.role == role)

    def accepts_rev(self, rev):
        return self.chip_rev_min <= rev <= self.chip_rev_max

    def silicon_text(self):
        return _silicon_text(self.chip_rev_min, self.chip_rev_max)

    def to_dict(self):
        return {"kind": self.kind, "version": self.version, "chip": self.chip,
                "chip_name": chip_name(self.chip), "project": self.project,
                "elf_sha256": self.elf_sha256, "chip_rev_min": self.chip_rev_min,
                "chip_rev_max": self.chip_rev_max, "silicon": self.silicon_text(),
                "rcp_version": self.rcp_version, "source": self.source,
                "flash_size": self.flash_size, "size": sum(len(p.data) for p in self.parts)}

    def manifest_entry(self):
        entry = {"kind": self.kind, "chip": self.chip, "project": self.project, "version": self.version,
                 "elf_sha256": self.elf_sha256, "chip_rev_min": self.chip_rev_min,
                 "chip_rev_max": self.chip_rev_max, "flash_size": self.flash_size,
                 "flash_mode": self.flash_mode, "flash_freq": self.flash_freq, "source": self.source,
                 "parts": [{"role": p.role, "offset": f"{p.offset:#x}", "path": p.path,
                            "sha256": p.sha256} for p in self.parts]}
        if self.rcp_version:
            entry["rcp_version"] = self.rcp_version
        return entry


@dataclass
class Language:
    """A language pack carried in the bundle (docs/features/37 language-packs.md)."""
    locale: str
    name: str
    version: str
    draft: bool
    path: str
    sha256: str
    data: bytes = field(repr=False, default=b"")
    origin: str = ""     # as Image.origin: the packs of one release go with its images

    @property
    def size(self):
        return len(self.data)

    @property
    def md5(self):
        return hashlib.md5(self.data).hexdigest()

    def to_dict(self):
        return {"locale": self.locale, "name": self.name, "version": self.version,
                "draft": self.draft, "size": self.size, "origin": self.origin}

    def manifest_entry(self):
        return {"locale": self.locale, "name": self.name, "version": self.version,
                "draft": self.draft, "path": self.path, "sha256": self.sha256, "size": self.size}


@dataclass
class Asset:
    """An asset pack (letters, saver picture, certificates) carried in the bundle (OPT-007)."""
    kind: int
    name: str
    version: str
    draft: bool
    path: str
    sha256: str
    data: bytes = field(repr=False, default=b"")
    origin: str = ""

    @property
    def size(self):
        return len(self.data)

    def to_dict(self):
        return {"kind": self.kind, "name": self.name, "version": self.version,
                "draft": self.draft, "size": self.size, "origin": self.origin}

    def manifest_entry(self):
        return {"kind": self.kind, "name": self.name, "version": self.version, "draft": self.draft,
                "path": self.path, "sha256": self.sha256, "size": self.size}


@dataclass
class Voice:
    """A voice pack (spoken zone names and messages, docs/features/08 D08) carried in the bundle
    (BL-204). Its revision is its own, not the firmware's version."""
    locale: str
    voice_id: str
    revision: int
    draft: bool
    path: str
    sha256: str
    data: bytes = field(repr=False, default=b"")
    origin: str = ""

    @property
    def size(self):
        return len(self.data)

    def label(self):
        return f"{self.locale} {self.voice_id} r{self.revision}"

    def to_dict(self):
        return {"locale": self.locale, "voice_id": self.voice_id, "revision": self.revision,
                "draft": self.draft, "size": self.size, "origin": self.origin}

    def manifest_entry(self):
        return {"locale": self.locale, "voice_id": self.voice_id, "revision": self.revision,
                "draft": self.draft, "path": self.path, "sha256": self.sha256, "size": self.size}


ENGLISH = "en-US"     # built into every image: it has no language pack, only its voice


def pick_voice(voices, locale="", profile=""):
    """The voice for a panel in `locale` ("" is English, built in): that language's own voice and
    never another's (ADR-053: a language and its voice are one unit), the highest revision; None
    when the language has none. Locales match as the firmware server matches them, exactly but
    for letter case (es-MX never takes es-US's). A draft never for a release image."""
    want = (locale or ENGLISH).lower()
    found = [v for v in voices if v.locale.lower() == want and not (v.draft and profile == "release")]
    return max(found, key=lambda v: v.revision) if found else None


@dataclass
class Bundle:
    name: str
    created: str
    source: dict
    signing_key_sha256: str
    images: list
    languages: list = field(default_factory=list)
    assets: list = field(default_factory=list)   # Asset: installed after the firmware (OPT-007)
    voices: list = field(default_factory=list)   # Voice: one installed on a new panel (BL-204)
    radios: list = field(default_factory=list)   # Radio: never among `images`, never chosen for a panel

    def language(self, locale, image=None):
        langs = self.packs_for(image)[0] if image is not None else self.languages
        return next((l for l in langs if l.locale == locale), None)

    def packs_for(self, image):
        """(languages, assets, voices) that go with `image`: its own release's (the same origin),
        or all of a file's, whose packs serve every image in it."""
        mine = lambda p: p.origin == image.origin   # noqa: E731
        return ([l for l in self.languages if mine(l)], [a for a in self.assets if mine(a)],
                [v for v in self.voices if mine(v)])

    def radio(self, kind):
        return next((r for r in self.radios if r.kind == kind), None)

    def image(self, key):
        return next((i for i in self.images if i.key == key), None)

    def version_text(self):
        versions = sorted({i.version for i in self.images})
        profiles = sorted({i.profile for i in self.images})
        return f"{', '.join(versions)} ({', '.join(profiles)})"

    def to_dict(self):
        return {"name": self.name, "created": self.created, "source": self.source,
                "signing_key_sha256": self.signing_key_sha256, "version": self.version_text(),
                "images": [i.to_dict() for i in self.images],
                "languages": [l.to_dict() for l in self.languages],
                "assets": [a.to_dict() for a in self.assets],
                "voices": [v.to_dict() for v in self.voices],
                "radios": [r.to_dict() for r in self.radios]}


def _safe_member(path):
    p = PurePosixPath(path)
    if p.is_absolute() or ".." in p.parts or "\\" in path or ":" in path or not p.parts:
        raise BundleError(f"unsafe path in bundle: {path!r}")
    return str(p)


def _int(value, what):
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        raise BundleError(f"{what}: {value!r} is not a number") from None


# -- where an image's parts may go (FLS2-002) ---------------------------------------------------
# A part is written where the manifest says, so the offsets are checked against what the chip and
# the image's own partition table allow before anything can be written: a bundle whose otadata
# sits on the settings (NVS) would otherwise erase them in an Update. A layout this Flasher does
# not know is refused, never trusted.
SECTOR = 0x1000
APP_ALIGN = 0x10000                 # ESP-IDF puts an app partition on a 64 KB boundary
# Where each chip's ROM reads the second-stage bootloader (ESP-IDF's CONFIG_BOOTLOADER_OFFSET_IN_FLASH).
BOOTLOADER_AT = {"esp32": 0x1000, "esp32s2": 0x1000, "esp32p4": 0x2000, "esp32c5": 0x2000,
                 "esp32c2": 0x0, "esp32c3": 0x0, "esp32c6": 0x0, "esp32c61": 0x0, "esp32h2": 0x0,
                 "esp32s3": 0x0}
PT_TYPE_APP, PT_TYPE_DATA = 0x00, 0x01
PT_SUB_FACTORY, PT_SUB_OTA_0, PT_SUB_OTADATA = 0x00, 0x10, 0x00


@dataclass
class Partition:
    label: str
    type: int
    subtype: int
    offset: int
    size: int

    @property
    def end(self):
        return self.offset + self.size


def read_partition_table(data):
    """[Partition] from an ESP-IDF partition table's bytes: entries up to the MD5 entry (checked
    when there is one) or the erased end. BundleError for anything else."""
    data = bytes(data)
    if len(data) > SECTOR:
        raise BundleError(f"the partition table is larger ({len(data)} bytes) than its 4 KB sector")
    parts = []
    for pos in range(0, len(data) - 31, 32):
        rec = data[pos:pos + 32]
        if rec[:2] == b"\xeb\xeb":
            if rec[16:32] != hashlib.md5(data[:pos]).digest():
                raise BundleError("the partition table's MD5 does not match its entries")
            break
        if rec == b"\xff" * 32:
            break
        if rec[:2] != b"\xaa\x50":
            raise BundleError("the partition table is not an ESP-IDF partition table")
        ptype, subtype, offset, size = struct.unpack_from("<BBII", rec, 2)
        label = rec[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
        parts.append(Partition(label, ptype, subtype, offset, size))
    if not parts:
        raise BundleError("the partition table has no partitions")
    return parts


def _where(offset):
    return f"{offset:#x}" if offset >= 0 else f"-{-offset:#x}"


def check_layout(image):
    """BundleError unless every part of `image` goes where the chip and its own partition table put
    it (FLS2-002): the bootloader at the chip's offset, the partition table after it, otadata on the
    table's otadata partition and its size, the app at the start of the partition the bootloader
    starts (factory, else ota_0) and inside it; all on sectors, apart, and inside the flash."""
    flash = image.flash_bytes
    who = image.key
    parts = sorted(image.parts, key=lambda p: p.offset)
    for p in parts:
        if p.offset < 0:
            raise BundleError(f"{who}: the {p.role} has a negative offset ({_where(p.offset)})")
        if p.offset % SECTOR:
            raise BundleError(f"{who}: the {p.role} at {_where(p.offset)} is not on a 4 KB sector")
        if not p.data:
            raise BundleError(f"{who}: the {p.role} is empty")
        if p.offset + len(p.data) > flash:
            raise BundleError(f"{who}: the {p.role} at {_where(p.offset)} reaches past the end of a "
                              f"{image.flash_size} flash")
    for a, b in zip(parts, parts[1:]):
        if a.offset + len(a.data) > b.offset:
            raise BundleError(f"{who}: the {a.role} at {_where(a.offset)} and the {b.role} at "
                              f"{_where(b.offset)} overlap")
    boot, pt = image.part("bootloader"), image.part("partition-table")
    want = BOOTLOADER_AT.get(image.chip)
    if want is None:
        raise BundleError(f"{who}: this Flasher does not know where an {image.chip} starts its bootloader")
    if boot.offset != want:
        raise BundleError(f"{who}: the bootloader is at {_where(boot.offset)}; an "
                          f"{chip_name(image.chip)} starts it from {want:#x}")
    table = read_partition_table(pt.data)
    table_end = pt.offset + SECTOR
    for q in sorted(table, key=lambda q: q.offset):
        if q.size == 0 or q.offset % SECTOR or q.size % SECTOR:
            raise BundleError(f"{who}: the partition {q.label} ({_where(q.offset)}, {q.size:#x} bytes) "
                              "is not whole 4 KB sectors")
        if q.type == PT_TYPE_APP and q.offset % APP_ALIGN:
            raise BundleError(f"{who}: the app partition {q.label} at {_where(q.offset)} is not on a 64 KB boundary")
        if q.offset < table_end:
            raise BundleError(f"{who}: the partition {q.label} at {_where(q.offset)} lies before the end of the "
                              f"partition table's sector ({table_end:#x})")
        if q.end > flash:
            raise BundleError(f"{who}: the partition {q.label} reaches past the end of a {image.flash_size} flash")
    ordered = sorted(table, key=lambda q: q.offset)
    for a, b in zip(ordered, ordered[1:]):
        if a.end > b.offset:
            raise BundleError(f"{who}: the partitions {a.label} and {b.label} overlap")
    otadata = [q for q in table if q.type == PT_TYPE_DATA and q.subtype == PT_SUB_OTADATA]
    if len(otadata) != 1:
        raise BundleError(f"{who}: the partition table has no otadata partition" if not otadata
                          else f"{who}: the partition table has more than one otadata partition")
    ota = image.part("otadata")
    if ota.offset != otadata[0].offset or len(ota.data) != otadata[0].size:
        raise BundleError(f"{who}: the otadata part is at {_where(ota.offset)} ({len(ota.data)} bytes), but the "
                          f"partition table puts otadata at {otadata[0].offset:#x} ({otadata[0].size} bytes)")
    apps = {q.subtype: q for q in reversed(table) if q.type == PT_TYPE_APP}
    home = apps.get(PT_SUB_FACTORY) or apps.get(PT_SUB_OTA_0)
    if home is None:
        raise BundleError(f"{who}: the partition table has no factory or ota_0 app partition")
    app = image.part("app")
    if app.offset != home.offset:
        raise BundleError(f"{who}: the app part is at {_where(app.offset)}, but the partition table's "
                          f"{home.label} starts at {home.offset:#x}")
    if len(app.data) > home.size:
        raise BundleError(f"{who}: the app ({len(app.data)} bytes) does not fit its partition {home.label} "
                          f"({home.size} bytes)")
    return image


def load(source, name=None):
    """A verified Bundle from a path or bytes. Raises BundleError with the reason."""
    if isinstance(source, (bytes, bytearray)):
        stream = io.BytesIO(source)
        name = name or "bundle"
    else:
        stream = Path(source)
        name = name or stream.name
    try:
        zf = zipfile.ZipFile(stream)
    except (zipfile.BadZipFile, OSError) as e:
        raise BundleError(f"not a firmware bundle ({e})") from None
    with zf:
        try:
            manifest, images, languages, radios, assets, voices = _read(zf)
        except BundleError:
            raise
        except (zipfile.BadZipFile, zlib.error, EOFError, OSError, RuntimeError, NotImplementedError) as e:
            # A file cut short or changed on the way (a copy or download that went wrong): zipfile
            # reports it only when the damaged part is read, as one of these.
            raise BundleError(f"the file is damaged ({e}): copy or download it again") from None
    if not images:
        raise BundleError("the bundle holds no images")
    keys = [i.key for i in images]
    if len(set(keys)) != len(keys):
        raise BundleError("the bundle holds the same image twice")
    signing = str(manifest.get("signing_key_sha256", ""))
    for lang in languages:
        if read_pack(lang.data).signing_key_sha256 != signing:
            raise BundleError(f"{lang.path} is signed with another key than the firmware")
    versions = {i.version for i in images}
    for asset in assets:
        if read_asset(asset.data).signing_key_sha256 != signing:
            raise BundleError(f"{asset.path} is signed with another key than the firmware")
        if asset.version not in versions:
            raise BundleError(f"{asset.path} is made for version {asset.version}, the images are "
                              f"{', '.join(sorted(versions))}")
    for voice in voices:
        if read_voice(voice.data).signing_key_sha256 != signing:
            raise BundleError(f"{voice.path} is signed with another key than the firmware")
    return Bundle(name=name, created=str(manifest.get("created", "")),
                  source=manifest.get("source") or {},
                  signing_key_sha256=signing, images=images, languages=languages, radios=radios,
                  assets=assets, voices=voices)


def _read(zf):
    """(manifest, images, languages, radios, assets, voices) from an open bundle; BundleError for anything a bundle must not be."""
    if sum(i.file_size for i in zf.infolist()) > MAX_TOTAL:
        raise BundleError("bundle is larger than any firmware could be")
    try:
        manifest = json.loads(zf.read("manifest.json"))
    except KeyError:
        raise BundleError("no manifest.json: not a firmware bundle") from None
    except ValueError as e:
        raise BundleError(f"manifest.json is not valid JSON ({e})") from None
    if not isinstance(manifest, dict):
        raise BundleError("manifest.json does not describe a firmware bundle")
    if manifest.get("format") != FORMAT:
        raise BundleError(f"bundle format {manifest.get('format')!r}; this Flasher reads format {FORMAT}. "
                          "A newer Flasher may be needed.")
    entries = manifest.get("images") or []
    if not isinstance(entries, list):
        raise BundleError("manifest.json does not list its images")
    return (manifest, [_load_image(zf, entry) for entry in entries], _load_languages(zf, manifest),
            _load_radios(zf, manifest), _load_assets(zf, manifest), _load_voices(zf, manifest))


def _load_languages(zf, manifest):
    entries = manifest.get("languages") or []
    if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries):
        raise BundleError("manifest.json does not list its languages properly")
    languages = []
    for entry in entries:
        member = _safe_member(str(entry.get("path", "")))
        try:
            data = zf.read(member)
        except KeyError:
            raise BundleError(f"{member} is missing from the bundle") from None
        digest = hashlib.sha256(data).hexdigest()
        if digest != str(entry.get("sha256", "")).lower():
            raise BundleError(f"{member} is damaged (its checksum does not match)")
        if "size" in entry and _int(entry["size"], f"{member} size") != len(data):
            raise BundleError(f"{member} is damaged (its size does not match)")
        try:
            info = read_pack(data)
        except ValueError as e:
            raise BundleError(f"{member}: {e}") from None
        for what, got, want in (("locale", info.locale, str(entry.get("locale", ""))),
                                ("version", info.version, str(entry.get("version", ""))),
                                ("draft flag", info.draft, bool(entry.get("draft", False)))):
            if got != want:
                raise BundleError(f"{member}: the pack's {what} ({got}) is not what the manifest "
                                  f"says ({want})")
        if any(l.locale == info.locale for l in languages):
            raise BundleError(f"the bundle holds the language {info.locale} twice")
        languages.append(Language(locale=info.locale, name=info.name, version=info.version,
                                  draft=info.draft, path=member, sha256=digest, data=data))
    return languages


def _load_assets(zf, manifest):
    entries = manifest.get("assets") or []
    if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries):
        raise BundleError("manifest.json does not list its asset packs properly")
    assets = []
    for entry in entries:
        member = _safe_member(str(entry.get("path", "")))
        try:
            data = zf.read(member)
        except KeyError:
            raise BundleError(f"{member} is missing from the bundle") from None
        digest = hashlib.sha256(data).hexdigest()
        if digest != str(entry.get("sha256", "")).lower():
            raise BundleError(f"{member} is damaged (its checksum does not match)")
        if "size" in entry and _int(entry["size"], f"{member} size") != len(data):
            raise BundleError(f"{member} is damaged (its size does not match)")
        try:
            info = read_asset(data)
        except StoreError as e:
            raise BundleError(f"{member}: {e}") from None
        for what, got, want in (("kind", info.kind, _int(entry.get("kind", -1), f"{member} kind")),
                                ("name", info.name, str(entry.get("name", ""))),
                                ("version", info.version, str(entry.get("version", ""))),
                                ("draft flag", info.draft, bool(entry.get("draft", False)))):
            if got != want:
                raise BundleError(f"{member}: the pack's {what} ({got}) is not what the manifest "
                                  f"says ({want})")
        if any(a.kind == info.kind and a.name == info.name for a in assets):
            raise BundleError(f"the bundle holds the asset pack {info.name} twice")
        assets.append(Asset(kind=info.kind, name=info.name, version=info.version, draft=info.draft,
                            path=member, sha256=digest, data=data))
    return assets


def _load_voices(zf, manifest):
    """The bundle's voice packs (BL-204): a Flasher before them ignores the list."""
    entries = manifest.get("voices") or []
    if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries):
        raise BundleError("manifest.json does not list its voices properly")
    voices = []
    for entry in entries:
        member = _safe_member(str(entry.get("path", "")))
        try:
            data = zf.read(member)
        except KeyError:
            raise BundleError(f"{member} is missing from the bundle") from None
        digest = hashlib.sha256(data).hexdigest()
        if digest != str(entry.get("sha256", "")).lower():
            raise BundleError(f"{member} is damaged (its checksum does not match)")
        if "size" in entry and _int(entry["size"], f"{member} size") != len(data):
            raise BundleError(f"{member} is damaged (its size does not match)")
        try:
            info = read_voice(data)
        except StoreError as e:
            raise BundleError(f"{member}: {e}") from None
        for what, got, want in (("locale", info.locale, str(entry.get("locale", ""))),
                                ("voice", info.voice_id, str(entry.get("voice_id", ""))),
                                ("revision", info.revision, _int(entry.get("revision", -1), f"{member} revision")),
                                ("draft flag", info.draft, bool(entry.get("draft", False)))):
            if got != want:
                raise BundleError(f"{member}: the pack's {what} ({got}) is not what the manifest "
                                  f"says ({want})")
        if any(v.locale == info.locale and v.voice_id == info.voice_id for v in voices):
            raise BundleError(f"the bundle holds the voice {info.locale} {info.voice_id} twice")
        voices.append(Voice(locale=info.locale, voice_id=info.voice_id, revision=info.revision,
                            draft=info.draft, path=member, sha256=digest, data=data))
    return voices


def _read_part(zf, f):
    """A Part from a manifest file entry, its bytes checked by SHA-256."""
    member = _safe_member(str(f.get("path", "")))
    try:
        data = zf.read(member)
    except KeyError:
        raise BundleError(f"{member} is missing from the bundle") from None
    digest = hashlib.sha256(data).hexdigest()
    if digest != str(f.get("sha256", "")).lower():
        raise BundleError(f"{member} is damaged (its checksum does not match)")
    return Part(role=f["role"], offset=_int(f.get("offset"), f"{member} offset"),
                path=member, sha256=digest, data=data)


def _load_radios(zf, manifest):
    entries = manifest.get("radios") or []
    if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries):
        raise BundleError("manifest.json does not list its radios properly")
    radios = []
    for entry in entries:
        kind = str(entry.get("kind", ""))
        if kind not in RADIO_KINDS:
            log.warning("bundle: skipped the software for a radio this Flasher does not know (%r)", kind)
            continue
        if not _layout_known(kind, entry):
            log.warning("bundle: skipped the %s radio's software: its parts are not the ones this Flasher "
                        "installs (a newer Flasher's bundle)", kind)
            continue
        radio = _load_radio(zf, kind, entry)
        if any(r.kind == kind for r in radios):
            raise BundleError(f"the bundle holds the {kind} radio's software twice")
        radios.append(radio)
    return radios


def _radio_parts(kind, entry):
    files = entry.get("parts")
    if not isinstance(files, list) or not all(isinstance(f, dict) and "role" in f for f in files):
        raise BundleError(f"the {kind} radio without its parts")
    return files


def _layout_known(kind, entry):
    """Whether a radio's parts are the ones this Flasher writes, at their offsets. Another layout is
    a newer Flasher's: that radio is skipped and the rest of the bundle used."""
    want = RADIO_KINDS[kind]["parts"]
    files = _radio_parts(kind, entry)
    if sorted(str(f["role"]) for f in files) != sorted(want):
        return False
    return all(_int(f.get("offset"), f"the {kind} radio's {f['role']} offset") == want[f["role"]]
               for f in files)


def _load_radio(zf, kind, entry):
    """A Radio from its entry, whose layout _layout_known() has accepted."""
    want = RADIO_KINDS[kind]
    what = f"the {kind} radio"
    parts = sorted((_read_part(zf, f) for f in _radio_parts(kind, entry)), key=lambda p: p.offset)
    source = entry.get("source") or {}
    try:
        radio = Radio(kind=kind, chip=str(entry["chip"]), project=str(entry["project"]),
                      version=str(entry["version"]), elf_sha256=str(entry["elf_sha256"]),
                      chip_rev_min=_int(entry["chip_rev_min"], "chip_rev_min"),
                      chip_rev_max=_int(entry["chip_rev_max"], "chip_rev_max"),
                      flash_size=str(entry["flash_size"]), flash_mode=str(entry.get("flash_mode", "keep")),
                      flash_freq=str(entry.get("flash_freq", "keep")), parts=parts,
                      rcp_version=str(entry.get("rcp_version", "")),
                      source=source if isinstance(source, dict) else {})
    except KeyError as e:
        raise BundleError(f"{what} without {e.args[0]}") from None
    for field_name in ("chip", "project"):
        if getattr(radio, field_name) != want[field_name]:
            raise BundleError(f"{what}: {field_name} {getattr(radio, field_name)!r}, "
                              f"a {kind} radio's is {want[field_name]!r}")
    flash_size_bytes(radio.flash_size)
    try:
        app = parse_app(radio.part("app").data)
    except ValueError as e:
        raise BundleError(f"{what}: the app part is not usable ({e})") from None
    for name, got, expected in (("chip", app.chip, radio.chip), ("project", app.project, radio.project),
                                ("version", app.version, radio.version),
                                ("ELF hash", app.elf_sha256, radio.elf_sha256),
                                ("oldest silicon", app.min_rev, radio.chip_rev_min),
                                ("newest silicon", app.max_rev, radio.chip_rev_max)):
        if got != expected:
            raise BundleError(f"{what}: the app's {name} ({got}) is not what the manifest says ({expected})")
    end = max(p.offset + len(p.data) for p in parts)
    if end > radio.flash_bytes:
        raise BundleError(f"{what}: the parts reach past the end of a {radio.flash_size} flash")
    return radio


def _load_image(zf, entry):
    try:
        files = entry["files"]
        roles = [f["role"] for f in files]
    except (KeyError, TypeError):
        raise BundleError("an image without its files") from None
    if not all(isinstance(f, dict) for f in files):
        raise BundleError("an image without its files")
    if sorted(roles) != sorted(ROLES):
        raise BundleError(f"an image must have exactly the parts {', '.join(ROLES)}")
    parts = []
    for f in files:
        member = _safe_member(str(f.get("path", "")))
        try:
            data = zf.read(member)
        except KeyError:
            raise BundleError(f"{member} is missing from the bundle") from None
        digest = hashlib.sha256(data).hexdigest()
        if digest != str(f.get("sha256", "")).lower():
            raise BundleError(f"{member} is damaged (its checksum does not match)")
        parts.append(Part(role=f["role"], offset=_int(f.get("offset"), f"{member} offset"),
                          path=member, sha256=digest, data=data))
    parts.sort(key=lambda p: p.offset)
    try:
        image = Image(board=str(entry["board"]), board_name=str(entry.get("board_name") or entry["board"]),
                      variant=str(entry["variant"]), profile=str(entry["profile"]),
                      project=str(entry["project"]), version=str(entry["version"]),
                      elf_sha256=str(entry["elf_sha256"]), chip=str(entry["chip"]),
                      chip_rev_min=_int(entry["chip_rev_min"], "chip_rev_min"),
                      chip_rev_max=_int(entry["chip_rev_max"], "chip_rev_max"),
                      flash_size=str(entry["flash_size"]), flash_mode=str(entry.get("flash_mode", "keep")),
                      flash_freq=str(entry.get("flash_freq", "keep")),
                      partition_table_sha256=str(entry.get("partition_table_sha256", "")),
                      signing_key_sha256=str(entry.get("signing_key_sha256", "")), parts=parts)
    except KeyError as e:
        raise BundleError(f"an image without {e.args[0]}") from None
    flash_size_bytes(image.flash_size)
    try:
        app = parse_app(image.part("app").data)
    except ValueError as e:
        raise BundleError(f"{image.key}: the app part is not usable ({e})") from None
    for what, got, want in (("chip", app.chip, image.chip), ("project", app.project, image.project),
                            ("version", app.version, image.version),
                            ("ELF hash", app.elf_sha256, image.elf_sha256),
                            ("oldest silicon", app.min_rev, image.chip_rev_min),
                            ("newest silicon", app.max_rev, image.chip_rev_max)):
        if got != want:
            raise BundleError(f"{image.key}: the app's {what} ({got}) is not what the manifest says ({want})")
    pt = image.part("partition-table")
    if image.partition_table_sha256 and image.partition_table_sha256 != pt.sha256:
        raise BundleError(f"{image.key}: partition table checksum differs from the manifest")
    return check_layout(image)


def write(path, images, created, source, signing_key_sha256, languages=(), radios=(), assets=(),
          voices=()):
    """Write a bundle; the image parts' (and languages' and radios') `path` and `sha256` are filled in here."""
    manifest = {"format": FORMAT, "created": created, "source": source,
                "signing_key_sha256": signing_key_sha256, "images": []}
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".part")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zf:
        for image in images:
            for part in image.parts:
                part.path = f"{image.key}/{part.role}.bin"
                part.sha256 = hashlib.sha256(part.data).hexdigest()
                zf.writestr(part.path, part.data)
            manifest["images"].append(image.manifest_entry())
        if languages:
            manifest["languages"] = []
        for lang in languages:
            lang.path = f"lang/{lang.locale}.robolang"
            lang.sha256 = hashlib.sha256(lang.data).hexdigest()
            zf.writestr(lang.path, lang.data)
            manifest["languages"].append(lang.manifest_entry())
        if assets:
            manifest["assets"] = []
        for asset in assets:
            asset.path = f"assets/{asset.name}.roboasset"
            asset.sha256 = hashlib.sha256(asset.data).hexdigest()
            zf.writestr(asset.path, asset.data)
            manifest["assets"].append(asset.manifest_entry())
        if voices:
            manifest["voices"] = []
        for voice in voices:
            voice.path = f"voices/{voice.locale}-{voice.voice_id}-r{voice.revision}.robovoice"
            voice.sha256 = hashlib.sha256(voice.data).hexdigest()
            zf.writestr(voice.path, voice.data)
            manifest["voices"].append(voice.manifest_entry())
        if radios:
            manifest["radios"] = []
        for radio in radios:
            for part in radio.parts:
                part.path = f"radios/{radio.kind}/{part.role}.bin"
                part.sha256 = hashlib.sha256(part.data).hexdigest()
                zf.writestr(part.path, part.data)
            manifest["radios"].append(radio.manifest_entry())
        zf.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
    tmp.replace(path)
    return path
