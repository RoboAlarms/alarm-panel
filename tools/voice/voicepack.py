"""RVOC format 1, the voice pack, and the Ogg Opus reading that makes its clip data.

The design of record is docs/features/08-Audio-and-Annunciation/voice-first-build.md: "The voice
pack: RVOC format 1" (D08, every byte), "Stored format" (D07, the numeric IDs) and "How long a
spoken name may be" (D02). Standard library only: `voice.py verify` and `pack` run on any clone,
without a model or an encoder.

  ogg_packets / opus_stream   an Ogg Opus file (pages, segment tables, continued packets) to its
                              audio packets, pre-skip and valid samples at 16 kHz
  frame_packets / unframe     clip data: repeated u16 little-endian length + packet bytes
  build_payload / parse_payload   the unsigned RVOC payload, and a strict reader of one
  check_envelope              a signed pack file: 0xFF padding to 4096 bytes, one signature sector
"""
import hashlib
import struct
import zlib

RATE = 16000                  # sample_rate, Hz
FRAME_MS = 20
FRAME_SAMPLES = RATE * FRAME_MS // 1000   # 320: every packet decodes to exactly this many
MAGIC = b'RVOC'
FORMAT = 1
HEADER_SIZE = 128
ENTRY_SIZE = 64
VOCABULARY_SCHEMA = 1
VOCABULARY_SCHEMA_ENDINGS = 2   # a pack with endings (category 7): readers before it refuse one
CODEC_OPUS = 1
CHANNELS = 1
FLAG_DRAFT = 0x1
SECTOR = 4096
FILE_MIN = 8192
FILE_MAX = 970752             # 948 KiB: the largest sector-aligned file below 950 KiB
PAYLOAD_MAX = FILE_MAX - SECTOR   # 966656
STRINGS_MAX = 32768
CLIPS_MAX = 512
PACKETS_MAX = 501
PACKET_BYTES_MAX = 1275
PRE_SKIP_MAX = FRAME_SAMPLES
VALID_MAX = 160000            # 10 s at 16 kHz (D02)
TEXT_MAX = 47                 # display text bytes, without the NUL
KEY_MAX = 63                  # script key bytes, without the NUL
WORD_GAP_SAMPLES = RATE * 60 // 1000      # 960: between a zone name's words (D02)

# Category numbers (index entry offset 2) and the numeric-ID namespace each must use (D07, D08).
# An ending (category 7, vocabulary schema 2) is a word as it sounds at the end of a spoken name:
# the panel plays it for a name's last word instead of the word's own clip, whose form suits the
# words before it ("Back" "patio" "MOTION"). Its ID is ENDING_BASE + the word's ID.
CATEGORIES = {'position': 1, 'location': 2, 'object': 3, 'number': 4, 'status': 5, 'walk': 6,
              'ending': 7}
NAMESPACES = {'position': (0x0001, 0x7FFF), 'location': (0x0001, 0x7FFF),
              'object': (0x0001, 0x7FFF), 'number': (0x0001, 0x7FFF),
              'status': (0x8001, 0x80FF), 'walk': (0x8101, 0x81FF), 'ending': (0xC001, 0xFFFF)}
SELECTABLE = ('position', 'location', 'object', 'number')
ENDING_BASE = 0xC000


def ending_id(word_id):
    """The ID of a word's ending; a word above 0x3FFF has none."""
    return ENDING_BASE + word_id if 0x0001 <= word_id <= 0x3FFF else None

# The 16 status and walk-test clips every pack carries, with the IDs D07 gives them.
REQUIRED_IDS = {
    'status.arming': 0x8001, 'status.armed': 0x8002, 'status.away': 0x8003,
    'status.home': 0x8004, 'status.night': 0x8005, 'status.exit_now': 0x8006,
    'status.no_entry_delay': 0x8007, 'status.disarmed': 0x8008, 'status.entry': 0x8009,
    'status.not_ready': 0x800A,
    'walk.walk_test': 0x8101, 'walk.started': 0x8102, 'walk.ended': 0x8103,
    'walk.ends_soon': 0x8104, 'walk.all_tested': 0x8105, 'walk.some_untested': 0x8106,
}

_MODES = ('status.away', 'status.home', 'status.night')
# The automatic utterances (D05), each joined with the 300 ms part pause and held to the same
# 10-second ceiling as a zone name (D02).
UTTERANCES = [
    ('walk.walk_test', 'walk.started'),
    ('walk.walk_test', 'walk.ended', 'walk.all_tested'),
    ('walk.walk_test', 'walk.ended', 'walk.some_untested'),
    ('walk.walk_test', 'walk.ends_soon'),
    *[('status.arming', m, 'status.exit_now') for m in _MODES],
    *[('status.armed', m) for m in _MODES],
    *[('status.armed', m, 'status.no_entry_delay') for m in _MODES],
    ('status.disarmed',), ('status.entry',), ('status.not_ready',),
]

_HEADER = struct.Struct('<4sHHIIII16s32sIHBBHHIIIIIHHI16x')
_ENTRY = struct.Struct('<HBxIIIIIHH32s4x')
assert _HEADER.size == HEADER_SIZE and _ENTRY.size == ENTRY_SIZE


class FormatError(ValueError):
    """Data that breaks Ogg, Opus or RVOC format 1."""


def align4(n):
    return (n + 3) & ~3


def signed_size(payload_length):
    """The signed file's size: the payload padded to a sector, then one signature sector."""
    return -(-payload_length // SECTOR) * SECTOR + SECTOR


# ---------------------------------------------------------------------------------- Ogg Opus

def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else r << 1
        t.append(r & 0xFFFFFFFF)
    return t


_CRC = _crc_table()


def ogg_crc(data):
    """Ogg's page checksum: CRC-32, polynomial 0x04C11DB7, not reflected, starting at 0."""
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC[((crc >> 24) ^ b) & 0xFF]
    return crc


def ogg_page(payload_segments, granule, serial, seq, flags):
    """One Ogg page (for tests and fixtures): `payload_segments` are (bytes, ends_packet)."""
    lacing, body = bytearray(), bytearray()
    for data, ends in payload_segments:
        n = len(data)
        lacing += b'\xff' * (n // 255)
        if ends:
            lacing.append(n % 255)
        elif n % 255:
            raise ValueError('a piece that continues on the next page is a multiple of 255 bytes')
        body += data
    page = bytearray(b'OggS' + struct.pack('<BBqIIIB', 0, flags, granule, serial, seq, 0,
                                           len(lacing)) + lacing + body)
    struct.pack_into('<I', page, 22, ogg_crc(page))
    return bytes(page)


def ogg_packets(data):
    """The packets of a single-stream Ogg file and the last page's granule position.

    Checks every page's capture pattern, version, checksum, sequence and flags, joins packets
    continued across pages, and refuses a truncated file. Raises FormatError."""
    pos, packets, partial = 0, [], None
    serial = seq = granule = None
    eos = False
    while pos < len(data):
        if eos:
            raise FormatError('data after the end-of-stream page')
        if len(data) - pos < 27 or data[pos:pos + 4] != b'OggS':
            raise FormatError(f'no Ogg page at byte {pos}')
        ver, flags, gran, ser, pseq, crc, nseg = struct.unpack_from('<BBqIIIB', data, pos + 4)
        if ver != 0 or flags & ~0x7:
            raise FormatError(f'Ogg page at byte {pos}: version {ver}, flags {flags:#x}')
        lacing = data[pos + 27:pos + 27 + nseg]
        end = pos + 27 + nseg + sum(lacing)
        if len(lacing) < nseg or end > len(data):
            raise FormatError(f'Ogg page at byte {pos} is cut off')
        page = bytearray(data[pos:end])
        page[22:26] = b'\0\0\0\0'
        if ogg_crc(page) != crc:
            raise FormatError(f'Ogg page at byte {pos}: checksum differs')
        if bool(flags & 0x2) != (serial is None):
            raise FormatError(f'Ogg page at byte {pos}: beginning-of-stream flag misplaced')
        if serial is None:
            serial, seq = ser, pseq
        elif ser != serial:
            raise FormatError('more than one Ogg stream')
        elif pseq != (seq + 1) & 0xFFFFFFFF:
            raise FormatError(f'Ogg page {seq + 1} is missing')
        seq = pseq
        if bool(flags & 0x1) != (partial is not None):
            raise FormatError(f'Ogg page at byte {pos}: continued-packet flag disagrees')
        off = pos + 27 + nseg
        for lv in lacing:
            partial = (partial or b'') + bytes(data[off:off + lv])
            off += lv
            if lv < 255:
                packets.append(partial)
                partial = None
        if gran != -1:
            granule = gran
        eos = bool(flags & 0x4)
        pos = end
    if serial is None:
        raise FormatError('no Ogg pages')
    if partial is not None:
        raise FormatError('the last packet is unfinished')
    if not eos:
        raise FormatError('no end-of-stream page: the file is cut off')
    return packets, granule


def opus_frame_samples(toc):
    """Samples one Opus frame of this TOC byte decodes to at 16 kHz (RFC 6716 section 3.1)."""
    config = toc >> 3
    if config < 12:                 # SILK-only: 10, 20, 40, 60 ms
        n48 = (480, 960, 1920, 2880)[config & 3]
    elif config < 16:               # hybrid: 10, 20 ms
        n48 = (480, 960)[config & 1]
    else:                           # CELT-only: 2.5, 5, 10, 20 ms
        n48 = (120, 240, 480, 960)[config & 3]
    return n48 // 3


def check_packets(packets, pre_skip, valid_samples):
    """D08's rules for one clip's packets and trim; returns the problems (empty when it's good)."""
    p = []
    if not 1 <= len(packets) <= PACKETS_MAX:
        p.append(f'{len(packets)} packets, not 1..{PACKETS_MAX}')
    for i, pk in enumerate(packets):
        if not 1 <= len(pk) <= PACKET_BYTES_MAX:
            p.append(f'packet {i}: {len(pk)} bytes, not 1..{PACKET_BYTES_MAX}')
            continue
        if pk[0] & 0x3:
            p.append(f'packet {i}: TOC code {pk[0] & 3}, not 0 (one frame)')
        elif opus_frame_samples(pk[0]) != FRAME_SAMPLES:
            p.append(f'packet {i}: a {opus_frame_samples(pk[0]) / 16:g} ms frame, not 20 ms')
    if not 0 <= pre_skip <= PRE_SKIP_MAX:
        p.append(f'pre-skip {pre_skip}, not 0..{PRE_SKIP_MAX}')
    if not 1 <= valid_samples <= VALID_MAX:
        p.append(f'{valid_samples} valid samples, not 1..{VALID_MAX}')
    room = FRAME_SAMPLES * len(packets)
    if pre_skip + valid_samples > room:
        p.append(f'pre-skip + valid samples {pre_skip + valid_samples} > {room} decoded')
    elif room - pre_skip - valid_samples >= FRAME_SAMPLES:
        p.append(f'{room - pre_skip - valid_samples} trailing samples discarded, a whole packet')
    return p


def opus_stream(data, input_samples=None):
    """An Ogg Opus file's (audio packets, pre_skip, valid_samples), both counts at 16 kHz.

    OpusHead's 48 kHz pre-skip and the final granule position must divide by three exactly; the
    packets must pass check_packets. Without `input_samples`, valid_samples comes from the final
    granule as RFC 7845 defines it: (granule - pre-skip) / 3. With it (the resampled length the
    encoder was given), that length is the valid_samples, and the granule must agree with it
    exactly: either as RFC 7845 says, or as FFmpeg's Ogg muxer writes it for libopus (FFmpeg 9.0
    with libavformat 63: one pre-skip more, granule = 2 * pre-skip + 3 * samples, seen on every
    clip of the en-US library). Raises FormatError."""
    packets, granule = ogg_packets(data)
    if len(packets) < 3:
        raise FormatError('no audio packets')
    head, tags, audio = packets[0], packets[1], packets[2:]
    if len(head) < 19 or head[:8] != b'OpusHead':
        raise FormatError('the first packet is not OpusHead')
    version, channels, pre48, rate, gain, family = struct.unpack_from('<BBHIhB', head, 8)
    if version >> 4 or channels != CHANNELS or family != 0 or gain != 0:
        raise FormatError(f'OpusHead: version {version}, {channels} channels, mapping {family}, '
                          f'gain {gain} (want mono, family 0, no gain)')
    if rate != RATE:
        raise FormatError(f'OpusHead: input rate {rate}, not {RATE}')
    if tags[:8] != b'OpusTags':
        raise FormatError('the second packet is not OpusTags')
    if pre48 % 3:
        raise FormatError(f'pre-skip {pre48} at 48 kHz is not a whole number of 16 kHz samples')
    if granule is None or granule <= pre48 or (granule - pre48) % 3:
        raise FormatError(f'final granule {granule} with pre-skip {pre48}: not a whole number '
                          'of 16 kHz samples')
    pre_skip, valid = pre48 // 3, (granule - pre48) // 3
    if input_samples is not None:
        if valid not in (input_samples, input_samples + pre_skip):
            raise FormatError(f'final granule {granule} does not match {input_samples} input '
                              f'samples with pre-skip {pre48}')
        valid = input_samples
    problems = check_packets(audio, pre_skip, valid)
    if problems:
        raise FormatError('; '.join(problems))
    return audio, pre_skip, valid


def frame_packets(packets):
    """Clip data: each packet as a u16 little-endian length, then its bytes."""
    out = bytearray()
    for p in packets:
        if not 1 <= len(p) <= PACKET_BYTES_MAX:
            raise FormatError(f'a {len(p)}-byte packet')
        out += struct.pack('<H', len(p)) + p
    return bytes(out)


def unframe(data):
    """Clip data back to its packets; raises FormatError on a cut-off or empty packet."""
    packets, pos = [], 0
    while pos < len(data):
        if pos + 2 > len(data):
            raise FormatError(f'a packet length cut off at byte {pos}')
        (n,) = struct.unpack_from('<H', data, pos)
        if not 1 <= n <= PACKET_BYTES_MAX:
            raise FormatError(f'packet {len(packets)}: length {n}, not 1..{PACKET_BYTES_MAX}')
        if pos + 2 + n > len(data):
            raise FormatError(f'packet {len(packets)} cut off')
        packets.append(bytes(data[pos + 2:pos + 2 + n]))
        pos += 2 + n
    return packets


# -------------------------------------------------------------------------------- the payload

def _text_ok(t):
    b = t.encode('utf-8')
    return 1 <= len(b) <= TEXT_MAX and not any(ord(c) < 0x20 or 0x7F <= ord(c) < 0xA0 for c in t)


def _key_ok(k):
    return 1 <= len(k) <= KEY_MAX and all(0x21 <= ord(c) <= 0x7E for c in k)


def id_problems(numeric_id, category):
    """Why this ID can't have this category (empty when it can)."""
    if category not in CATEGORIES:
        return [f'unknown category {category!r}']
    lo, hi = NAMESPACES[category]
    if not lo <= numeric_id <= hi:
        return [f'ID {numeric_id:#06x} is outside the {category} namespace {lo:#06x}..{hi:#06x}']
    return []


def ending_problems(entries):
    """Why these (ID, category) pairs can't share a pack: an ending whose word isn't a selectable
    entry of the same pack (empty when every ending has its word)."""
    words = {i for i, cat in entries if cat in SELECTABLE}
    return [f'ending {i:#06x} has no word {i - ENDING_BASE:#06x}' for i, cat in entries
            if cat == 'ending' and i - ENDING_BASE not in words]


def build_payload(clips, *, revision, draft, locale, voice_id, word_gap_ms, part_gap_ms,
                  nominal_bitrate):
    """The unsigned RVOC format 1 payload, byte for byte as D08 lays it out.

    `clips`: dicts with numeric_id, category (name), text, key (the script key, word.front),
    data (the framed packets), valid_samples, packet_count, pre_skip. The index is sorted by
    numeric ID; strings are each clip's display text then its script key, both NUL-ended, in
    index order; clip data follows, each start four-byte aligned with zero bytes. Raises
    FormatError when a rule can't be met."""
    if not 1 <= len(clips) <= CLIPS_MAX:
        raise FormatError(f'{len(clips)} clips, not 1..{CLIPS_MAX}')
    if not 1 <= revision <= 0xFFFFFFFF:
        raise FormatError(f'pack revision {revision}, not 1..4294967295')
    loc, vid = locale.encode('ascii'), voice_id.encode('ascii')
    if not 1 <= len(loc) <= 15 or not 1 <= len(vid) <= 31:
        raise FormatError('locale or voice ID too long')
    order = sorted(clips, key=lambda c: c['numeric_id'])
    ids = [c['numeric_id'] for c in order]
    keys = [c['key'] for c in order]
    if len(set(ids)) != len(ids) or len(set(keys)) != len(keys):
        raise FormatError('numeric IDs and script keys must be unique')
    for c in order:
        bad = id_problems(c['numeric_id'], c['category'])
        if not _text_ok(c['text']):
            bad.append(f'display text {c["text"]!r}')
        if not _key_ok(c['key']):
            bad.append(f'script key {c["key"]!r}')
        n = len(unframe(c['data']))
        if n != c['packet_count']:
            bad.append(f'{n} packets framed, {c["packet_count"]} counted')
        bad += check_packets(unframe(c['data']), c['pre_skip'], c['valid_samples'])
        if bad:
            raise FormatError(f'{c["key"]}: ' + '; '.join(bad))
    bad = ending_problems([(c['numeric_id'], c['category']) for c in order])
    if bad:
        raise FormatError('; '.join(bad))
    schema = (VOCABULARY_SCHEMA_ENDINGS if any(c['category'] == 'ending' for c in order)
              else VOCABULARY_SCHEMA)
    strings_offset = HEADER_SIZE + ENTRY_SIZE * len(order)
    strings, where = bytearray(), []
    for c in order:
        t = strings_offset + len(strings)
        strings += c['text'].encode('utf-8') + b'\0'
        k = strings_offset + len(strings)
        strings += c['key'].encode('ascii') + b'\0'
        where.append((t, k))
    if len(strings) > STRINGS_MAX:
        raise FormatError(f'{len(strings)} bytes of strings, more than {STRINGS_MAX}')
    audio_offset = align4(strings_offset + len(strings))
    audio, index = bytearray(), bytearray()
    for c, (t, k) in zip(order, where):
        audio += b'\0' * (align4(len(audio)) - len(audio))
        index += _ENTRY.pack(c['numeric_id'], CATEGORIES[c['category']], t, k,
                             audio_offset + len(audio), len(c['data']), c['valid_samples'],
                             c['packet_count'], c['pre_skip'], hashlib.sha256(c['data']).digest())
        audio += c['data']
    length = audio_offset + len(audio)
    if length > PAYLOAD_MAX:
        raise FormatError(f'payload of {length} bytes, more than {PAYLOAD_MAX}')
    header = _HEADER.pack(MAGIC, FORMAT, HEADER_SIZE, length, revision, schema,
                          FLAG_DRAFT if draft else 0, loc, vid, RATE, CODEC_OPUS, CHANNELS,
                          FRAME_MS, len(order), ENTRY_SIZE, HEADER_SIZE, strings_offset,
                          len(strings), audio_offset, len(audio), word_gap_ms, part_gap_ms,
                          nominal_bitrate)
    pad = b'\0' * (audio_offset - strings_offset - len(strings))
    return header + bytes(index) + bytes(strings) + pad + bytes(audio)


def _cstr(data, off, limit, what):
    end = data.find(b'\0', off, off + limit + 1)
    if end < 0:
        raise FormatError(f'{what} at {off} has no NUL within {limit} bytes')
    return data[off:end].decode('utf-8' if what == 'display text' else 'ascii')


def parse_payload(data):
    """A strict reader of an unsigned payload: (header dict, entry dicts). Raises FormatError.

    It checks what the panel's reader checks before using a pack, except decoding the audio."""
    if len(data) < HEADER_SIZE:
        raise FormatError('shorter than the header')
    (magic, fmt, hsize, length, revision, schema, flags, loc, vid, rate, codec, channels,
     frame_ms, count, esize, index_off, strings_off, strings_len, audio_off, audio_len,
     word_gap, part_gap, bitrate) = _HEADER.unpack_from(data)
    h = dict(payload_length=length, pack_revision=revision, vocabulary_schema=schema, flags=flags,
             locale=loc.rstrip(b'\0').decode('ascii'), voice_id=vid.rstrip(b'\0').decode('ascii'),
             clip_count=count, strings_offset=strings_off, strings_length=strings_len,
             audio_offset=audio_off, audio_length=audio_len, word_gap_ms=word_gap,
             part_gap_ms=part_gap, nominal_bitrate=bitrate)
    if magic != MAGIC or fmt != FORMAT or hsize != HEADER_SIZE:
        raise FormatError('not RVOC format 1')
    if length != len(data) or length > PAYLOAD_MAX:
        raise FormatError(f'payload_length {length}, data {len(data)}')
    if (revision == 0 or schema not in (VOCABULARY_SCHEMA, VOCABULARY_SCHEMA_ENDINGS)
            or flags & ~FLAG_DRAFT or rate != RATE
            or codec != CODEC_OPUS or channels != CHANNELS or frame_ms != FRAME_MS
            or esize != ENTRY_SIZE or index_off != HEADER_SIZE):
        raise FormatError('a header field has a value format 1 does not allow')
    if any(data[112:128]):
        raise FormatError('reserved header bytes are not zero')
    for raw, size in ((loc, 16), (vid, 32)):
        s = raw.rstrip(b'\0')
        if not s or b'\0' in s or len(s) >= size:
            raise FormatError('locale or voice ID is not NUL-terminated and zero-padded')
    if not 1 <= count <= CLIPS_MAX:
        raise FormatError(f'clip_count {count}')
    if strings_off != HEADER_SIZE + ENTRY_SIZE * count or strings_len > STRINGS_MAX:
        raise FormatError('strings region misplaced')
    if audio_off != align4(strings_off + strings_len) or audio_off + audio_len != length:
        raise FormatError('audio region misplaced')
    if any(data[strings_off + strings_len:audio_off]):
        raise FormatError('alignment bytes are not zero')
    entries, last, ends = [], -1, audio_off
    for i in range(count):
        off = HEADER_SIZE + ENTRY_SIZE * i
        (cid, cat, t, k, d, dlen, valid, packets, pre, digest) = _ENTRY.unpack_from(data, off)
        if data[off + 3] or any(data[off + 60:off + 64]):
            raise FormatError(f'entry {i}: reserved bytes are not zero')
        if cid <= last:
            raise FormatError(f'entry {i}: ID {cid:#06x} does not ascend')
        last = cid
        name = next((n for n, v in CATEGORIES.items() if v == cat), None)
        if (name is None or id_problems(cid, name)
                or (name == 'ending' and schema != VOCABULARY_SCHEMA_ENDINGS)):
            raise FormatError(f'entry {i}: ID {cid:#06x} with category {cat}')
        for o, lim in ((t, TEXT_MAX), (k, KEY_MAX)):
            if not strings_off <= o < strings_off + strings_len:
                raise FormatError(f'entry {i}: a string outside the strings region')
        text = _cstr(data[:strings_off + strings_len], t, TEXT_MAX, 'display text')
        key = _cstr(data[:strings_off + strings_len], k, KEY_MAX, 'script key')
        if not _text_ok(text) or not _key_ok(key):
            raise FormatError(f'entry {i}: bad display text or script key')
        if d % 4 or d < ends or d + dlen > length or dlen == 0:
            raise FormatError(f'entry {i}: clip data misplaced or overlapping')
        if any(data[ends:d]):
            raise FormatError(f'entry {i}: alignment bytes before the clip are not zero')
        clip = data[d:d + dlen]
        ends = d + dlen
        if hashlib.sha256(clip).digest() != digest:
            raise FormatError(f'entry {i} ({key}): SHA-256 differs')
        pk = unframe(clip)
        problems = check_packets(pk, pre, valid)
        if len(pk) != packets:
            problems.append(f'{len(pk)} packets, {packets} counted')
        if problems:
            raise FormatError(f'entry {i} ({key}): ' + '; '.join(problems))
        entries.append(dict(numeric_id=cid, category=name, text=text, key=key, data_offset=d,
                            data_length=dlen, valid_samples=valid, packet_count=packets,
                            pre_skip=pre))
    if ends != length:
        raise FormatError('bytes after the last clip')
    if len({e['key'] for e in entries}) != count:
        raise FormatError('script keys repeat')
    bad = ending_problems([(e['numeric_id'], e['category']) for e in entries])
    if bad:
        raise FormatError('; '.join(bad))
    have = {e['numeric_id'] for e in entries}
    missing = [k for k, v in REQUIRED_IDS.items() if v not in have]
    if missing or not any(e['category'] in SELECTABLE for e in entries):
        raise FormatError('missing ' + (', '.join(missing) or 'a selectable token'))
    return h, entries


def check_envelope(data, signed=True):
    """A pack file's unsigned payload, after checking its envelope; raises FormatError.

    The file is a multiple of 4096 bytes, 8192..970752; the payload (its length at offset 8) is
    padded with 0xFF to the next 4096-byte boundary, then one 4096-byte signature sector. With
    `signed`, that sector must begin with a Secure Boot v2 block (0xE7, version 2) whose CRC-32
    holds and whose digest is the SHA-256 of everything before it. Checking the signature itself
    needs the public key: the panel does that."""
    size = len(data)
    if size % SECTOR or not FILE_MIN <= size <= FILE_MAX:
        raise FormatError(f'{size} bytes: not a multiple of {SECTOR} in {FILE_MIN}..{FILE_MAX}')
    if data[:4] != MAGIC:
        raise FormatError('not an RVOC pack')
    (length,) = struct.unpack_from('<I', data, 8)
    if signed_size(length) != size:
        raise FormatError(f'payload_length {length} does not fill a {size}-byte file')
    if data[length:size - SECTOR] != b'\xff' * (size - SECTOR - length):
        raise FormatError('the padding is not all 0xFF')
    sector = data[size - SECTOR:]
    if signed:
        if sector[0] != 0xE7 or sector[1] != 0x02:
            raise FormatError('no Secure Boot v2 signature block')
        if zlib.crc32(sector[:1196]) != struct.unpack_from('<I', sector, 1196)[0]:
            raise FormatError('the signature block CRC-32 differs')
        if sector[4:36] != hashlib.sha256(data[:size - SECTOR]).digest():
            raise FormatError('the signature block signs other bytes')
    return bytes(data[:length])
