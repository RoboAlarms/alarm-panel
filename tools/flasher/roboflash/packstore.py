"""The pack store's format in Python (docs/features/37 language-packs.md, I18N-044, I18N-045).

The store sits in the partition labelled `assets`: sectors 0 and 1 are the table's two copies,
sectors 2 and up hold blobs (language packs, kept byte for byte). This module mirrors
components/core/pack_store (the same validity rules, the same bytes) and is checked against the
same frozen fixture. It also reads a `.robolang` pack file's header, a `.roboasset` asset pack
(`read_asset`, the twin of components/core/pack_store asset_pack.h, checked against its frozen
fixture), a `.robovoice` voice pack's header (`read_voice`, components/core/voice_pack's RVOC
format 1, checked against its frozen fixtures), the Flasher's own board descriptions (`read_boards_pack`,
asset pack kind 0x80, which no panel keeps) and finds a partition in ESP-IDF's partition table.

Writes are planned, not made: `Store.plan_put` and `plan_default` return the ordered flash writes
(partition offset, bytes) and the store as it will be once they are all done. The two power-safety
rules hold: a blob goes only into sectors that no entry of the current table covers, and a table
only into the copy that is not current.
"""
import hashlib
import json
import re
import struct
import zlib
from dataclasses import dataclass, field, replace

from .imagefile import signature_key_digest

SECTOR = 4096
MAX_ENTRIES = 63
KIND_LANGUAGE = 1
KIND_VOICE = 2
KIND_LETTERS = 3       # asset packs (docs/features/39 asset-packs.md): never renumber
KIND_PICTURE = 4
KIND_CERTS = 5
KIND_WEB = 6
FLAG_DEFAULT = 1
NAME_MAX = 15
MAX_SECTORS = 0xFFFFF
FORMAT = 1
MAGIC = b"RAPS"
HDR_SIZE = 32
ENTRY_SIZE = 64
SEQ_MAX = 0xFFFFFFFF


class StoreError(ValueError):
    pass


@dataclass
class Entry:
    kind: int
    flags: int
    first_sector: int
    length: int
    payload_length: int
    sha256: bytes
    name: str


def _name_ok(name):
    return 1 <= len(name) <= NAME_MAX and all(0x20 <= ord(c) <= 0x7E for c in name)


def _run_ok(first, length, sectors):
    if first < 2 or length == 0 or length % SECTOR:
        return False
    return first + length // SECTOR <= sectors


def _overlap(a, b):
    return a.first_sector < b.first_sector + b.length // SECTOR and \
        b.first_sector < a.first_sector + a.length // SECTOR


def _same(a, b):
    return a.kind == b.kind and a.name == b.name


def payload_fits(e):
    """Whether an installed entry's payload_length fits its blob (FLS2-004): a pack file's envelope,
    the payload, its 0xFF padding to a sector, then one signature sector, so at least a header's
    worth and exactly the blob's sectors but one. The table's own validity rules (decode_table,
    shared with the C reader) don't cover the length, so a reader checks it before reading that
    many bytes; an entry that fails is a pack to replace, never a length to trust."""
    padded = e.length - SECTOR
    return ASSET_HEADER <= e.payload_length <= padded and -(-e.payload_length // SECTOR) * SECTOR == padded


def _parse_entry(p):
    raw = p[48:64]
    n = raw.find(b"\0")
    if n < 0:
        return None          # no NUL inside the 16 bytes
    if n == 0 or any(not 0x20 <= c <= 0x7E for c in raw[:n]) or any(raw[n:]):
        return None
    kind, flags = p[0], p[1]
    first, length, payload = struct.unpack_from("<III", p, 4)
    return Entry(kind, flags, first, length, payload, bytes(p[16:48]), raw[:n].decode("ascii"))


def _crc(head28, entries_bytes):
    return zlib.crc32(entries_bytes, zlib.crc32(head28)) & 0xFFFFFFFF


def decode_table(sector, sectors):
    """(sequence, [Entry]) for a valid table sector of a partition of `sectors` sectors, else None."""
    sector = bytes(sector)
    if len(sector) != SECTOR or sector[:4] != MAGIC:
        return None
    fmt, esize = struct.unpack_from("<HH", sector, 4)
    if fmt != FORMAT or esize != ENTRY_SIZE:
        return None
    sequence = struct.unpack_from("<I", sector, 8)[0]
    count = struct.unpack_from("<H", sector, 12)[0]
    if count > MAX_ENTRIES or struct.unpack_from("<I", sector, 16)[0] != sectors:
        return None
    body = sector[HDR_SIZE:HDR_SIZE + count * ENTRY_SIZE]
    if _crc(sector[:28], body) != struct.unpack_from("<I", sector, 28)[0]:
        return None
    entries = []
    for i in range(count):
        e = _parse_entry(body[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE])
        if e is None or not _run_ok(e.first_sector, e.length, sectors):
            return None
        if any(_overlap(e, o) or _same(e, o) for o in entries):
            return None
        entries.append(e)
    return sequence, entries


def _entry_bytes(e):
    return (struct.pack("<BBHIII", e.kind, e.flags, 0, e.first_sector, e.length, e.payload_length)
            + bytes(e.sha256) + e.name.encode("ascii").ljust(16, b"\0"))


def _encode(entries, sequence, sectors):
    body = b"".join(_entry_bytes(e) for e in entries)
    head = (MAGIC + struct.pack("<HHIHHI", FORMAT, ENTRY_SIZE, sequence, len(entries), 0, sectors)
            + b"\0" * 8)
    out = head + struct.pack("<I", _crc(head, body)) + body
    return out + b"\xff" * (SECTOR - len(out))


def encode_table(entries, sequence, sectors):
    """One table sector (4096 bytes), byte-identical to the C encoder's."""
    entries = list(entries)
    if len(entries) > MAX_ENTRIES or not 3 <= sectors <= MAX_SECTORS:
        raise StoreError("a table holds at most 63 entries and the partition 3 or more sectors")
    if not 0 <= sequence <= SEQ_MAX:
        raise StoreError("sequence out of range")
    for i, e in enumerate(entries):
        if not _name_ok(e.name) or not _run_ok(e.first_sector, e.length, sectors) \
                or len(e.sha256) != 32:
            raise StoreError(f"entry {e.name!r} is not valid")
        if any(_overlap(e, o) or _same(e, o) for o in entries[:i]):
            raise StoreError(f"entry {e.name!r} overlaps or repeats another")
    return _encode(entries, sequence, sectors)


@dataclass
class Plan:
    writes: list              # [(partition offset, bytes)] in the order to make them
    store: "Store"            # the store once every write is done


@dataclass
class Store:
    sectors: int
    sequence: int = 0
    current: int = -1         # 0 (A), 1 (B) or -1 (empty)
    entries: list = field(default_factory=list)

    @classmethod
    def mount(cls, sector_a, sector_b, sectors):
        """Both copies valid: the higher sequence (equal: A); one: it; none: an empty store."""
        got = [decode_table(s, sectors) for s in (sector_a, sector_b)]
        if got[0] and got[1]:
            pick = 1 if got[1][0] > got[0][0] else 0
        elif got[0]:
            pick = 0
        elif got[1]:
            pick = 1
        else:
            return cls(sectors)
        return cls(sectors, got[pick][0], pick, list(got[pick][1]))

    def find(self, kind, name):
        return next((e for e in self.entries if e.kind == kind and e.name == name), None)

    def free_sectors(self):
        used = sum(e.length // SECTOR for e in self.entries)
        return max(0, self.sectors - 2 - used)

    def _covered(self, first, length):
        probe = Entry(0, 0, first, length, 0, b"", "")
        return any(_overlap(probe, e) for e in self.entries)

    def alloc(self, length):
        """The first sector of the lowest free run of `length` bytes (first fit, sector 2 up)."""
        if length <= 0 or length % SECTOR:
            raise StoreError("a blob is a whole number of sectors")
        need = length // SECTOR
        cand = 2
        moved = True
        while moved:
            moved = False
            if cand + need > self.sectors:
                raise StoreError("no room in the language store")
            for e in self.entries:
                end = e.first_sector + e.length // SECTOR
                if cand < end and e.first_sector < cand + need:
                    cand = end
                    moved = True
        return cand

    def _table_writes(self, entries):
        """(writes, store after) for a new table of `entries` into the copy that is not current."""
        wrap = self.sequence == SEQ_MAX
        seq = 1 if wrap else self.sequence + 1
        target = 1 if self.current == 0 else 0
        old = 0 if self.current < 0 else self.current
        writes = [(target * SECTOR, _encode(entries, seq, self.sectors))]
        if wrap:
            writes.append((old * SECTOR, b"\xff" * SECTOR))
        return writes, Store(self.sectors, seq, target, list(entries))

    def plan_put(self, entry, blob):
        """Install `blob` as `entry` (kind, flags, payload_length, name as given; the place,
        length and SHA-256 are filled in). A pack of the same kind and name is replaced and the new
        entry goes last."""
        blob = bytes(blob)
        if entry.kind == 0 or not _name_ok(entry.name):
            raise StoreError("an entry needs a kind and a name of 1-15 printable characters")
        if not blob or len(blob) % SECTOR:
            raise StoreError("a blob is a whole number of sectors")
        old = self.find(entry.kind, entry.name)
        if old is None and len(self.entries) >= MAX_ENTRIES:
            raise StoreError("the language store holds no more entries")
        first = self.alloc(len(blob))
        new = replace(entry, first_sector=first, length=len(blob),
                      sha256=hashlib.sha256(blob).digest())
        entries = [e for e in self.entries if e is not old] + [new]
        table, after = self._table_writes(entries)
        return Plan([(first * SECTOR, blob)] + table, after)

    def plan_default(self, kind, name):
        """Flag `name` default and clear the flag from the kind's others; no writes when nothing
        changes. A name of "" or None only clears."""
        target = None
        if name:
            target = self.find(kind, name)
            if target is None:
                raise StoreError(f"{name!r} is not in the store")
        entries = []
        change = False
        for e in self.entries:
            f = e.flags
            if e.kind == kind:
                f = (f | FLAG_DEFAULT) if e is target else (f & ~FLAG_DEFAULT & 0xFF)
            change = change or f != e.flags
            entries.append(replace(e, flags=f))
        if not change:
            return Plan([], self)
        table, after = self._table_writes(entries)
        return Plan(table, after)


# ------------------------------------------------------------------ a pack file

PACK_MAGIC = b"RLNG"
PACK_HEADER = 128
_LOCALE = re.compile(rb"^[A-Za-z0-9-]{2,15}$")


@dataclass
class PackInfo:
    locale: str
    name: str
    version: str
    draft: bool
    payload_length: int
    template_fingerprint: int
    missing: int
    signing_key_sha256: str


def _cstr(raw, what, lo, hi):
    n = raw.find(b"\0")
    if n < 0 or any(raw[n:]):
        raise ValueError(f"the pack's {what} is not zero-padded")
    if not lo <= n <= hi:
        raise ValueError(f"the pack's {what} has a wrong length")
    return raw[:n]


def read_pack(data):
    """The header of a `.robolang` file as PackInfo; ValueError in words when it is not one."""
    data = bytes(data)
    if len(data) % SECTOR or len(data) < 2 * SECTOR:
        raise ValueError("not a language pack: its size is not a whole number of 4096-byte sectors "
                         "(at least two)")
    return _read_header(data, len(data))


def read_pack_header(head):
    """PackInfo from the first 128 bytes of a pack (as stored in flash), with the file-wide checks
    (length, padding, signature key) left out: its signing_key_sha256 is empty."""
    return _read_header(bytes(head), None)


def _read_header(data, file_len):
    if len(data) < PACK_HEADER or data[:4] != PACK_MAGIC:
        raise ValueError("not a language pack (wrong magic)")
    fmt, hsize, length = struct.unpack_from("<HHI", data, 4)
    if fmt != 1:
        raise ValueError(f"language pack format {fmt}; this Flasher reads format 1")
    if hsize != PACK_HEADER:
        raise ValueError("the pack's header size is wrong")
    if file_len is not None:
        padded = file_len - SECTOR
        if length < PACK_HEADER or length > padded or (length + SECTOR - 1) // SECTOR * SECTOR != padded:
            raise ValueError("the pack's length does not match the file")
        if any(b != 0xFF for b in data[length:padded]):
            raise ValueError("the pack's padding is not erased flash (0xFF)")
    loc = _cstr(data[12:28], "locale", 0, 15)
    if not _LOCALE.match(loc):
        raise ValueError("the pack's locale is not a language tag")
    raw_name = _cstr(data[28:60], "name", 1, 31)
    try:
        name = raw_name.decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("the pack's language name is not valid text") from None
    rule, flags, reserved = struct.unpack_from("<BBH", data, 60)
    if rule > 5 or flags & ~1 or reserved:
        raise ValueError("the pack's plural rule, flags or reserved bytes are wrong")
    fingerprint, missing = struct.unpack_from("<II", data, 80)
    ver = _cstr(data[88:104], "version", 1, 15)
    if any(not 0x20 <= c <= 0x7E for c in ver):
        raise ValueError("the pack's version is not plain text")
    if any(data[112:128]):
        raise ValueError("the pack's reserved bytes are not 0")
    return PackInfo(loc.decode("ascii"), name, ver.decode("ascii"), bool(flags & 1), length,
                    fingerprint, missing, signature_key_digest(data) if file_len is not None else "")


# ------------------------------------------------------------------ an asset pack file

ASSET_MAGIC = b"RAST"
ASSET_FORMAT = 1
ASSET_HEADER = 128
ASSET_PART = 32
ASSET_MAX_PARTS = 16
ASSET_FLAG_DRAFT = 1
PART_FONT = 1          # an LVGL binary font
PART_JPEG = 2          # a baseline JPEG
PART_CERTS = 3         # an ESP-IDF certificate bundle
PART_JSON = 4          # JSON (UTF-8): only in the Flasher's board descriptions
KIND_BOARDS = 0x80     # the Flasher's board descriptions: no panel keeps the kind
BOARDS_NAME = "boards"
ASSET_STORE_NAMES = {KIND_LETTERS: "letters", KIND_PICTURE: "saver", KIND_CERTS: "certs",
                     KIND_WEB: "web"}


@dataclass
class AssetPart:
    name: str
    type: int
    offset: int
    size: int


@dataclass
class AssetInfo:
    name: str
    kind: int
    version: str
    match: int
    draft: bool
    parts: list
    payload_length: int
    signing_key_sha256: str


def _asset_name(raw):
    """A 16-byte name field: 1-15 characters '!'..'~', then NUL bytes; the text or None."""
    n = raw.find(b"\0")
    if n < 1 or any(raw[n:]) or any(not 0x21 <= c <= 0x7E for c in raw[:n]):
        return None
    return raw[:n].decode("ascii")


def read_asset(data):
    """An asset pack file (`.roboasset`, signed or with an all-0xFF signature sector) as AssetInfo;
    StoreError in words when it is not a valid one. The envelope (whole sectors, the payload length
    at offset 8, 0xFF padding, one signature sector), then the same refusals as asset_pack_open()
    (OPT-012). The kind's own rules (part names, size bounds) are not checked here."""
    data = bytes(data)
    if len(data) % SECTOR or len(data) < 2 * SECTOR:
        raise StoreError("not an asset pack: its size is not a whole number of 4096-byte sectors "
                         "(at least two)")
    length = struct.unpack_from("<I", data, 8)[0]
    padded = len(data) - SECTOR
    if length > padded or (length + SECTOR - 1) // SECTOR * SECTOR != padded:
        raise StoreError("the asset pack's length does not match the file")
    if any(b != 0xFF for b in data[length:padded]):
        raise StoreError("the asset pack's padding is not erased flash (0xFF)")
    info = read_asset_payload(data[:length])
    info.signing_key_sha256 = signature_key_digest(data)
    return info


def read_asset_payload(p):
    """The checks of asset_pack_open() on a payload (unpadded, unsigned): AssetInfo with an empty
    signing_key_sha256, or StoreError."""
    return _read_asset_payload(p, (KIND_LETTERS, KIND_PICTURE, KIND_CERTS, KIND_WEB),
                               (PART_FONT, PART_JPEG, PART_CERTS))


def read_boards_payload(p):
    """read_asset_payload()'s checks for the Flasher's board descriptions (kind 0x80, a JSON part),
    which no panel reads."""
    return _read_asset_payload(p, (KIND_BOARDS,), (PART_JSON,))


def read_boards_pack(data):
    """The Flasher's board descriptions (docs/features/39 asset-packs.md "The kinds", kind 0x80; the
    update server's board descriptions): an asset pack file no panel keeps, with one JSON part `boards`.
    Returns (AssetInfo, the parsed JSON); StoreError in words. The envelope and the header are an
    asset pack's (read_asset); the JSON's own rules (format, boards) are the caller's."""
    data = bytes(data)
    if len(data) % SECTOR or len(data) < 2 * SECTOR:
        raise StoreError("not an asset pack: its size is not a whole number of 4096-byte sectors "
                         "(at least two)")
    length = struct.unpack_from("<I", data, 8)[0]
    padded = len(data) - SECTOR
    if length > padded or (length + SECTOR - 1) // SECTOR * SECTOR != padded:
        raise StoreError("the asset pack's length does not match the file")
    if any(b != 0xFF for b in data[length:padded]):
        raise StoreError("the asset pack's padding is not erased flash (0xFF)")
    info = read_boards_payload(data[:length])
    if info.name != BOARDS_NAME or [p.name for p in info.parts] != [BOARDS_NAME]:
        raise StoreError("board descriptions are one part named 'boards' in a pack named 'boards'")
    if info.draft:
        raise StoreError("the board descriptions are marked as a draft")
    part = info.parts[0]
    try:
        doc = json.loads(data[part.offset:part.offset + part.size].decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise StoreError(f"the board descriptions are not JSON ({e})") from None
    info.signing_key_sha256 = signature_key_digest(data)
    return info, doc


def _read_asset_payload(p, kinds, types):
    p = bytes(p)
    if len(p) < ASSET_HEADER:
        raise StoreError("the asset pack is shorter than its header")
    if p[:4] != ASSET_MAGIC:
        raise StoreError("not an asset pack (wrong magic)")
    fmt, hsize, length, kind, flags, count = struct.unpack_from("<HHIBBH", p, 4)
    if fmt != ASSET_FORMAT or hsize != ASSET_HEADER:
        raise StoreError(f"asset pack format {fmt} (header {hsize}); this reads format 1 "
                         "(header 128)")
    if length != len(p):
        raise StoreError("the asset pack's payload length differs from its bytes")
    if kind not in kinds:
        raise StoreError(f"the asset pack's kind {kind} is not one of "
                         + ("3-6" if kinds[0] == KIND_LETTERS else ", ".join(f"0x{k:02x}" for k in kinds)))
    if flags & ~ASSET_FLAG_DRAFT:
        raise StoreError(f"the asset pack has unknown flags 0x{flags:02x}")
    if not 1 <= count <= ASSET_MAX_PARTS:
        raise StoreError(f"the asset pack has {count} parts (1-16)")
    name = _asset_name(p[16:32])
    if name is None:
        raise StoreError("the asset pack's name is not 1-15 printable characters without spaces")
    version = _asset_name(p[32:48])
    if version is None:
        raise StoreError("the asset pack's version is not 1-15 printable characters without "
                         "spaces")
    match, parts_off = struct.unpack_from("<II", p, 48)
    if parts_off != ASSET_HEADER:
        raise StoreError(f"the asset pack's parts offset is {parts_off}, not 128")
    if any(p[56:ASSET_HEADER]):
        raise StoreError("the asset pack's reserved bytes are not 0")
    pos = ASSET_HEADER + ASSET_PART * count
    if pos > len(p):
        raise StoreError("the asset pack is shorter than its part table")
    parts, seen = [], set()
    for i in range(count):
        r = ASSET_HEADER + ASSET_PART * i
        pname = _asset_name(p[r:r + 16])
        if pname is None:
            raise StoreError(f"part {i + 1} of the asset pack has a bad name")
        if pname in seen:
            raise StoreError(f"part {i + 1} of the asset pack repeats the name {pname!r}")
        seen.add(pname)
        ptype, off, size, reserved = struct.unpack_from("<IIII", p, r + 16)
        if ptype not in types:
            raise StoreError(f"part {pname!r} has an unknown type {ptype}")
        if size == 0:
            raise StoreError(f"part {pname!r} is empty")
        if reserved:
            raise StoreError("the asset pack's reserved bytes are not 0")
        if off != (pos + 3) & ~3 or off + size > len(p):
            raise StoreError(f"part {pname!r} is not where the layout puts it")
        if any(p[pos:off]):
            raise StoreError("the asset pack's alignment bytes are not 0")
        parts.append(AssetPart(pname, ptype, off, size))
        pos = off + size
    if len(p) != (pos + 3) & ~3:
        raise StoreError("the asset pack does not end where its last part does")
    if any(p[pos:]):
        raise StoreError("the asset pack's alignment bytes are not 0")
    return AssetInfo(name, kind, version, match, bool(flags & ASSET_FLAG_DRAFT), parts, length, "")


# ------------------------------------------------------------------ a voice pack file

VOICE_MAGIC = b"RVOC"
VOICE_FORMAT = 1
VOICE_HEADER = 128
VOICE_FLAG_DRAFT = 1
VOICE_SCHEMAS = (1, 2)            # 2 adds endings (AUD-031)
VOICE_FILE_MIN = 8192
VOICE_FILE_MAX = 970752
VOICE_STORE_NAME = "primary"      # the one kind-2 entry: a new voice replaces it (D08)


@dataclass
class VoiceInfo:
    locale: str                   # "en-US"
    voice_id: str
    revision: int                 # the pack's identity with locale and voice_id: never reused
    draft: bool
    schema: int
    payload_length: int
    payload_sha256: str           # "" when read from a header alone
    signing_key_sha256: str       # "" when read from a header alone

    def label(self):
        return f"{self.locale} {self.voice_id} r{self.revision}"


def _voice_ident(raw, what):
    """A NUL-terminated, zero-padded field of printable ASCII without spaces (D08)."""
    n = raw.find(b"\0")
    if n < 1 or any(raw[n:]) or any(not 0x21 <= c <= 0x7E for c in raw[:n]):
        raise StoreError(f"the voice pack's {what} is not 1-{len(raw) - 1} printable characters")
    return raw[:n].decode("ascii")


def read_voice_header(head):
    """VoiceInfo from a voice pack's first 128 bytes (as stored in flash): the header's identity
    and the values every reader checks first; the clips are the panel's to check (vp_open)."""
    p = bytes(head)
    if len(p) < VOICE_HEADER or p[:4] != VOICE_MAGIC:
        raise StoreError("not a voice pack (wrong magic)")
    fmt, hsize, length, revision, schema, flags = struct.unpack_from("<HHIIII", p, 4)
    if fmt != VOICE_FORMAT or hsize != VOICE_HEADER:
        raise StoreError(f"voice pack format {fmt} (header {hsize}); this Flasher reads format 1 "
                         "(header 128)")
    if schema not in VOICE_SCHEMAS:
        raise StoreError(f"the voice pack's vocabulary schema {schema} is newer than this Flasher "
                         "knows: a newer Flasher may be needed")
    if revision == 0:
        raise StoreError("the voice pack's revision is 0")
    if flags & ~VOICE_FLAG_DRAFT:
        raise StoreError(f"the voice pack has unknown flags 0x{flags:08x}")
    if length < VOICE_HEADER:
        raise StoreError("the voice pack's payload length is shorter than its header")
    return VoiceInfo(_voice_ident(p[24:40], "locale"), _voice_ident(p[40:72], "voice"), revision,
                     bool(flags & VOICE_FLAG_DRAFT), schema, length, "", "")


def read_voice(data):
    """A voice pack file (`.robovoice`: the payload, 0xFF to a sector boundary, one signature
    sector) as VoiceInfo; StoreError in words when its envelope or header is wrong."""
    data = bytes(data)
    if len(data) % SECTOR or not VOICE_FILE_MIN <= len(data) <= VOICE_FILE_MAX:
        raise StoreError(f"not a voice pack: {len(data)} bytes is not a whole number of 4096-byte "
                         f"sectors from {VOICE_FILE_MIN} to {VOICE_FILE_MAX}")
    info = read_voice_header(data[:VOICE_HEADER])
    padded = len(data) - SECTOR
    if info.payload_length > padded or -(-info.payload_length // SECTOR) * SECTOR != padded:
        raise StoreError("the voice pack's length does not match the file")
    if any(b != 0xFF for b in data[info.payload_length:padded]):
        raise StoreError("the voice pack's padding is not erased flash (0xFF)")
    info.payload_sha256 = hashlib.sha256(data[:info.payload_length]).hexdigest()
    info.signing_key_sha256 = signature_key_digest(data)
    return info


# ------------------------------------------------------------------ ESP-IDF partition table

def find_partition(table_bytes, label):
    """(offset, size) of the partition called `label` in a partition table, or None."""
    data = bytes(table_bytes)
    for pos in range(0, len(data) - 31, 32):
        rec = data[pos:pos + 32]
        if rec[:2] == b"\xeb\xeb" or rec == b"\xff" * 32:
            break
        if rec[:2] != b"\xaa\x50":
            continue
        offset, size = struct.unpack_from("<II", rec, 4)
        raw = rec[12:28]
        if raw.split(b"\0", 1)[0].decode("ascii", "replace") == label:
            return offset, size
    return None
