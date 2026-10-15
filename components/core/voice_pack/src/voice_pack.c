/*
 * voice_pack.c - the RVOC format 1 reader (docs/features/08 voice-first-build.md D08). See
 * include/voice_pack/voice_pack.h. Every offset is checked in 64-bit arithmetic before use, so a
 * hostile header cannot wrap a range; nothing past `len` is ever read.
 */
#include "voice_pack/voice_pack.h"

#include <string.h>

#include "alarm_proto/proto_link.h" /* proto_sha256: the project's one portable SHA-256 */

/* Header offsets */
enum {
    H_MAGIC = 0,
    H_FORMAT = 4,
    H_HEADER_SIZE = 6,
    H_PAYLOAD_LENGTH = 8,
    H_PACK_REVISION = 12,
    H_VOCAB_SCHEMA = 16,
    H_FLAGS = 20,
    H_LOCALE = 24,
    H_VOICE_ID = 40,
    H_SAMPLE_RATE = 72,
    H_CODEC = 76,
    H_CHANNELS = 78,
    H_FRAME_MS = 79,
    H_CLIP_COUNT = 80,
    H_INDEX_ENTRY_SIZE = 82,
    H_INDEX_OFFSET = 84,
    H_STRINGS_OFFSET = 88,
    H_STRINGS_LENGTH = 92,
    H_AUDIO_OFFSET = 96,
    H_AUDIO_LENGTH = 100,
    H_WORD_GAP = 104,
    H_PART_GAP = 106,
    H_BITRATE = 108,
    H_RESERVED = 112,
};

/* Index entry offsets */
enum {
    E_ID = 0,
    E_CATEGORY = 2,
    E_RESERVED1 = 3,
    E_TEXT = 4,
    E_KEY = 8,
    E_DATA = 12,
    E_DATA_LENGTH = 16,
    E_VALID_SAMPLES = 20,
    E_PACKET_COUNT = 24,
    E_PRE_SKIP = 26,
    E_SHA256 = 28,
    E_RESERVED2 = 60,
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != v) {
            return false;
        }
    }
    return true;
}

static vp_err_t fail(vp_problem_t *why, vp_err_t err, uint32_t entry)
{
    if (why) {
        why->err = err;
        why->entry = (uint16_t)entry;
    }
    return err;
}

const char *vp_err_name(vp_err_t err)
{
    switch (err) {
    case VP_OK: return "ok";
    case VP_ERR_ARG: return "argument";
    case VP_ERR_FILE_SIZE: return "file size";
    case VP_ERR_PADDING: return "padding";
    case VP_ERR_TRUNCATED: return "truncated";
    case VP_ERR_MAGIC: return "magic";
    case VP_ERR_VERSION: return "version";
    case VP_ERR_HEADER: return "header";
    case VP_ERR_REVISION: return "revision";
    case VP_ERR_FLAGS: return "flags";
    case VP_ERR_RESERVED: return "reserved";
    case VP_ERR_IDENTITY: return "identity";
    case VP_ERR_CLIP_COUNT: return "clip count";
    case VP_ERR_LAYOUT: return "layout";
    case VP_ERR_ALIGN_BYTES: return "alignment bytes";
    case VP_ERR_ORDER: return "order";
    case VP_ERR_CATEGORY: return "category";
    case VP_ERR_TEXT: return "text";
    case VP_ERR_KEY: return "key";
    case VP_ERR_KEY_DUPLICATE: return "duplicate key";
    case VP_ERR_CLIP_RANGE: return "clip range";
    case VP_ERR_FRAMING: return "framing";
    case VP_ERR_PACKET: return "packet";
    case VP_ERR_SAMPLES: return "samples";
    case VP_ERR_HASH: return "hash";
    case VP_ERR_COVERAGE: return "coverage";
    case VP_ERR_ENDING: return "ending";
    }
    return "?";
}

bool vp_category_selectable(uint8_t category)
{
    return category >= (uint8_t)VP_CAT_POSITION && category <= (uint8_t)VP_CAT_NUMBER;
}

/* The ID namespace a category belongs in (D07); endings only in a schema 2 pack. */
static bool category_matches(uint16_t id, uint8_t category, uint32_t schema)
{
    if (vp_category_selectable(category)) {
        return id >= VP_TOKEN_FIRST && id <= VP_TOKEN_LAST;
    }
    if (category == (uint8_t)VP_CAT_ENDING) {
        return schema == VP_VOCABULARY_SCHEMA_ENDINGS && id >= VP_ENDING_FIRST;
    }
    if (category == (uint8_t)VP_CAT_STATUS) {
        return id >= VP_STATUS_FIRST && id <= VP_STATUS_LAST;
    }
    if (category == (uint8_t)VP_CAT_WALK) {
        return id >= VP_WALK_FIRST && id <= VP_WALK_LAST;
    }
    return false;
}

/* A fixed-size, NUL-terminated, zero-padded identity field of printable ASCII without spaces. */
static bool identity_ok(const uint8_t *p, size_t size, char *out)
{
    size_t n = 0;
    while (n < size && p[n] != 0u) {
        if (p[n] < 0x21u || p[n] > 0x7Eu) {
            return false;
        }
        n++;
    }
    if (n == 0u || n >= size || !all_bytes(p + n, size - n, 0u)) {
        return false;
    }
    memcpy(out, p, n + 1u);
    return true;
}

/* The length of the NUL-terminated string at absolute offset `off`, which must start and end
 * (NUL included) inside [lo, hi) and have 1..max bytes. SIZE_MAX when it doesn't. */
static size_t string_in(const uint8_t *base, uint32_t off, uint32_t lo, uint32_t hi, size_t max)
{
    if (off < lo || off >= hi) {
        return SIZE_MAX;
    }
    size_t avail = (size_t)(hi - off);
    size_t limit = max + 1u < avail ? max + 1u : avail;
    const uint8_t *s = base + off;
    for (size_t i = 0; i < limit; i++) {
        if (s[i] == 0u) {
            return i == 0u ? SIZE_MAX : i;
        }
    }
    return SIZE_MAX;
}

/* Valid UTF-8 (no overlongs, surrogates or code points past U+10FFFF) without C0, DEL or C1
 * control characters. */
static bool text_ok(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        uint32_t cp;
        size_t more;
        if (c < 0x80u) {
            cp = c;
            more = 0;
        } else if (c >= 0xC2u && c <= 0xDFu) {
            cp = c & 0x1Fu;
            more = 1;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            cp = c & 0x0Fu;
            more = 2;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            cp = c & 0x07u;
            more = 3;
        } else {
            return false;
        }
        if (more > n - i - 1u) {
            return false;
        }
        for (size_t k = 1; k <= more; k++) {
            unsigned b = s[i + k];
            if ((b & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if ((more == 2u && cp < 0x800u) || (more == 3u && (cp < 0x10000u || cp > 0x10FFFFu)) ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        if (cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu)) {
            return false;
        }
        i += more + 1u;
    }
    return true;
}

static bool key_ok(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20u || s[i] > 0x7Eu) {
            return false;
        }
    }
    return true;
}

uint32_t vp_opus_samples_16k(const uint8_t *packet, size_t length)
{
    if (packet == NULL || length == 0u || length > VP_PACKET_MAX) {
        return 0;
    }
    unsigned toc = packet[0];
    if ((toc & 0x3u) != 0u) {
        return 0; /* codes 1-3: several frames (or a count byte); the pack carries one frame each */
    }
    unsigned config = toc >> 3;
    if (config < 12u) { /* SILK-only: 10, 20, 40, 60 ms */
        static const uint16_t silk[4] = {160u, 320u, 640u, 960u};
        return silk[config & 3u];
    }
    if (config < 16u) { /* hybrid: 10, 20 ms */
        return (config & 1u) ? 320u : 160u;
    }
    static const uint16_t celt[4] = {40u, 80u, 160u, 320u}; /* CELT-only: 2.5, 5, 10, 20 ms */
    return celt[config & 3u];
}

/* Walks a clip's framing: exactly `count` packets of 1..1275 bytes, each one 20 ms frame, using
 * every byte. */
static vp_err_t check_packets(const uint8_t *data, uint32_t length, uint16_t count)
{
    uint32_t pos = 0;
    uint32_t seen = 0;
    while (pos < length) {
        if (length - pos < 2u) {
            return VP_ERR_FRAMING;
        }
        uint16_t plen = rd16(data + pos);
        pos += 2u;
        if (plen == 0u || plen > VP_PACKET_MAX || plen > length - pos) {
            return VP_ERR_FRAMING;
        }
        if (++seen > count) {
            return VP_ERR_FRAMING;
        }
        if (vp_opus_samples_16k(data + pos, plen) != VP_FRAME_SAMPLES) {
            return VP_ERR_PACKET;
        }
        pos += plen;
    }
    return seen == count ? VP_OK : VP_ERR_FRAMING;
}

/* Bytes [from, to) of the payload are alignment: fewer than four, all zero. */
static vp_err_t check_gap(const uint8_t *base, uint64_t from, uint64_t to, vp_err_t range_err)
{
    if (to < from || to - from >= 4u) {
        return range_err;
    }
    return all_bytes(base + from, (size_t)(to - from), 0u) ? VP_OK : VP_ERR_ALIGN_BYTES;
}

static void entry_to_clip(const uint8_t *base, const uint8_t *e, vp_clip_t *out)
{
    out->id = rd16(e + E_ID);
    out->category = e[E_CATEGORY];
    out->text = (const char *)(base + rd32(e + E_TEXT));
    out->key = (const char *)(base + rd32(e + E_KEY));
    out->data = base + rd32(e + E_DATA);
    out->data_length = rd32(e + E_DATA_LENGTH);
    out->valid_samples = rd32(e + E_VALID_SAMPLES);
    out->packet_count = rd16(e + E_PACKET_COUNT);
    out->pre_skip = rd16(e + E_PRE_SKIP);
    out->sha256 = e + E_SHA256;
}

vp_err_t vp_open(const uint8_t *b, size_t len, vp_pack_t *out, vp_problem_t *why)
{
    const uint32_t H = VP_NO_ENTRY;
    if (why) {
        why->err = VP_OK;
        why->entry = VP_NO_ENTRY;
    }
    if (b == NULL || out == NULL) {
        return fail(why, VP_ERR_ARG, H);
    }
    if (len < VP_HEADER_SIZE) {
        return fail(why, VP_ERR_TRUNCATED, H);
    }
    if (memcmp(b + H_MAGIC, VP_MAGIC, 4) != 0) {
        return fail(why, VP_ERR_MAGIC, H);
    }
    const uint32_t schema = rd32(b + H_VOCAB_SCHEMA);
    if (rd16(b + H_FORMAT) != VP_FORMAT || rd16(b + H_HEADER_SIZE) != VP_HEADER_SIZE ||
        (schema != VP_VOCABULARY_SCHEMA && schema != VP_VOCABULARY_SCHEMA_ENDINGS) ||
        rd16(b + H_INDEX_ENTRY_SIZE) != VP_INDEX_ENTRY_SIZE) {
        return fail(why, VP_ERR_VERSION, H);
    }
    uint32_t payload_length = rd32(b + H_PAYLOAD_LENGTH);
    if ((uint64_t)payload_length != (uint64_t)len) {
        return fail(why, VP_ERR_TRUNCATED, H);
    }
    if (payload_length > VP_PAYLOAD_MAX) {
        return fail(why, VP_ERR_LAYOUT, H);
    }
    if (rd32(b + H_PACK_REVISION) == 0u) {
        return fail(why, VP_ERR_REVISION, H);
    }
    uint32_t flags = rd32(b + H_FLAGS);
    if ((flags & ~VP_FLAG_DRAFT) != 0u) {
        return fail(why, VP_ERR_FLAGS, H);
    }
    if (!all_bytes(b + H_RESERVED, VP_HEADER_SIZE - H_RESERVED, 0u)) {
        return fail(why, VP_ERR_RESERVED, H);
    }
    vp_pack_t p;
    memset(&p, 0, sizeof p);
    if (!identity_ok(b + H_LOCALE, VP_LOCALE_SIZE, p.locale) ||
        !identity_ok(b + H_VOICE_ID, VP_VOICE_ID_SIZE, p.voice_id)) {
        return fail(why, VP_ERR_IDENTITY, H);
    }
    if (rd32(b + H_SAMPLE_RATE) != VP_SAMPLE_RATE || rd16(b + H_CODEC) != VP_CODEC_OPUS ||
        b[H_CHANNELS] != VP_CHANNELS || b[H_FRAME_MS] != VP_FRAME_MS ||
        rd16(b + H_WORD_GAP) > VP_WORD_GAP_MAX_MS || rd16(b + H_PART_GAP) < VP_PART_GAP_MIN_MS ||
        rd16(b + H_PART_GAP) > VP_PART_GAP_MAX_MS ||
        rd32(b + H_BITRATE) != VP_NOMINAL_BITRATE) {
        return fail(why, VP_ERR_HEADER, H);
    }
    uint16_t n = rd16(b + H_CLIP_COUNT);
    if (n == 0u || n > VP_CLIPS_MAX) {
        return fail(why, VP_ERR_CLIP_COUNT, H);
    }

    /* Regions, in this order with only alignment between them: header, index, strings, audio. */
    uint64_t index_end = (uint64_t)VP_HEADER_SIZE + (uint64_t)n * VP_INDEX_ENTRY_SIZE;
    uint64_t strings_off = rd32(b + H_STRINGS_OFFSET);
    uint64_t strings_len = rd32(b + H_STRINGS_LENGTH);
    uint64_t audio_off = rd32(b + H_AUDIO_OFFSET);
    uint64_t audio_len = rd32(b + H_AUDIO_LENGTH);
    uint64_t strings_end = strings_off + strings_len;
    uint64_t audio_end = audio_off + audio_len;
    if (rd32(b + H_INDEX_OFFSET) != VP_HEADER_SIZE || strings_len == 0u ||
        strings_len > VP_STRINGS_MAX || audio_len == 0u || (strings_off % 4u) != 0u ||
        (audio_off % 4u) != 0u || index_end > len || strings_end > len || audio_end != len) {
        return fail(why, VP_ERR_LAYOUT, H);
    }
    vp_err_t err = check_gap(b, index_end, strings_off, VP_ERR_LAYOUT);
    if (err == VP_OK) {
        err = check_gap(b, strings_end, audio_off, VP_ERR_LAYOUT);
    }
    if (err != VP_OK) {
        return fail(why, err, H);
    }

    uint32_t s_lo = (uint32_t)strings_off;
    uint32_t s_hi = (uint32_t)strings_end;
    uint64_t audio_pos = audio_off; /* clips follow each other in index order */
    uint32_t prev_id = 0;
    unsigned n_status = 0, n_walk = 0, n_select = 0, n_ending = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b + VP_HEADER_SIZE + (size_t)i * VP_INDEX_ENTRY_SIZE;
        uint16_t id = rd16(e + E_ID);
        uint8_t cat = e[E_CATEGORY];
        if (id == 0u || id <= prev_id) {
            return fail(why, VP_ERR_ORDER, i);
        }
        prev_id = id;
        if (!category_matches(id, cat, schema)) {
            return fail(why, VP_ERR_CATEGORY, i);
        }
        if (e[E_RESERVED1] != 0u || !all_bytes(e + E_RESERVED2, VP_INDEX_ENTRY_SIZE - E_RESERVED2, 0u)) {
            return fail(why, VP_ERR_RESERVED, i);
        }
        uint32_t text_off = rd32(e + E_TEXT);
        size_t tl = string_in(b, text_off, s_lo, s_hi, VP_TEXT_MAX);
        if (tl == SIZE_MAX || !text_ok(b + text_off, tl)) {
            return fail(why, VP_ERR_TEXT, i);
        }
        uint32_t key_off = rd32(e + E_KEY);
        size_t kl = string_in(b, key_off, s_lo, s_hi, VP_KEY_MAX);
        if (kl == SIZE_MAX || !key_ok(b + key_off, kl)) {
            return fail(why, VP_ERR_KEY, i);
        }
        uint32_t valid = rd32(e + E_VALID_SAMPLES);
        uint16_t packets = rd16(e + E_PACKET_COUNT);
        uint16_t pre = rd16(e + E_PRE_SKIP);
        uint64_t decoded = (uint64_t)packets * VP_FRAME_SAMPLES;
        if (packets == 0u || packets > VP_PACKETS_MAX || pre > VP_FRAME_SAMPLES || valid == 0u ||
            valid > VP_CLIP_SAMPLES_MAX || (uint64_t)pre + valid > decoded ||
            decoded - pre - valid >= VP_FRAME_SAMPLES) {
            return fail(why, VP_ERR_SAMPLES, i);
        }
        uint64_t d_off = rd32(e + E_DATA);
        uint64_t d_len = rd32(e + E_DATA_LENGTH);
        if ((d_off % 4u) != 0u || d_len == 0u || d_off + d_len > audio_end) {
            return fail(why, VP_ERR_CLIP_RANGE, i);
        }
        err = check_gap(b, audio_pos, d_off, VP_ERR_CLIP_RANGE);
        if (err != VP_OK) {
            return fail(why, err, i);
        }
        audio_pos = d_off + d_len;
        err = check_packets(b + d_off, (uint32_t)d_len, packets);
        if (err != VP_OK) {
            return fail(why, err, i);
        }
        uint8_t digest[32];
        proto_sha256(b + d_off, (size_t)d_len, digest);
        if (memcmp(digest, e + E_SHA256, sizeof digest) != 0) {
            return fail(why, VP_ERR_HASH, i);
        }
        if (cat == (uint8_t)VP_CAT_STATUS) {
            n_status++;
        } else if (cat == (uint8_t)VP_CAT_WALK) {
            n_walk++;
        } else if (cat == (uint8_t)VP_CAT_ENDING) {
            n_ending++;
        } else {
            n_select++;
        }
    }
    err = check_gap(b, audio_pos, audio_end, VP_ERR_LAYOUT);
    if (err != VP_OK) {
        return fail(why, err, H);
    }
    /* Script keys are unique (up to 512 x 512 / 2 short compares, once per install). */
    for (uint32_t i = 1; i < n; i++) {
        const char *ki = (const char *)b + rd32(b + VP_HEADER_SIZE + (size_t)i * VP_INDEX_ENTRY_SIZE + E_KEY);
        for (uint32_t j = 0; j < i; j++) {
            const char *kj =
                (const char *)b + rd32(b + VP_HEADER_SIZE + (size_t)j * VP_INDEX_ENTRY_SIZE + E_KEY);
            if (strcmp(ki, kj) == 0) {
                return fail(why, VP_ERR_KEY_DUPLICATE, i);
            }
        }
    }
    /* IDs are unique and each status/walk category holds only its own namespace, so the counts
     * prove every one of the 16 is present. */
    if (n_status != VP_STATUS_COUNT || n_walk != VP_WALK_COUNT || n_select == 0u) {
        return fail(why, VP_ERR_COVERAGE, H);
    }
    /* Every ending's word is a selectable entry (AUD-031): IDs ascend, so a binary search. */
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b + VP_HEADER_SIZE + (size_t)i * VP_INDEX_ENTRY_SIZE;
        if (e[E_CATEGORY] != (uint8_t)VP_CAT_ENDING) {
            continue;
        }
        const uint16_t word = (uint16_t)(rd16(e + E_ID) - VP_ENDING_BASE);
        uint32_t lo = 0, hi = n;
        const uint8_t *found = NULL;
        while (lo < hi && found == NULL) {
            const uint32_t mid = lo + (hi - lo) / 2u;
            const uint8_t *m = b + VP_HEADER_SIZE + (size_t)mid * VP_INDEX_ENTRY_SIZE;
            const uint16_t at = rd16(m + E_ID);
            if (at == word) {
                found = m;
            } else if (at < word) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (found == NULL || !vp_category_selectable(found[E_CATEGORY])) {
            return fail(why, VP_ERR_ENDING, i);
        }
    }

    p.base = b;
    p.payload_length = payload_length;
    p.pack_revision = rd32(b + H_PACK_REVISION);
    p.flags = flags;
    p.clip_count = n;
    p.word_gap_ms = rd16(b + H_WORD_GAP);
    p.part_gap_ms = rd16(b + H_PART_GAP);
    p.nominal_bitrate = rd32(b + H_BITRATE);
    p.selectable_count = (uint16_t)n_select;
    p.vocabulary_schema = schema;
    p.ending_count = (uint16_t)n_ending;
    *out = p;
    return VP_OK;
}

vp_err_t vp_open_signed(const uint8_t *file, size_t len, vp_pack_t *out, vp_problem_t *why)
{
    if (why) {
        why->err = VP_OK;
        why->entry = VP_NO_ENTRY;
    }
    if (file == NULL || out == NULL) {
        return fail(why, VP_ERR_ARG, VP_NO_ENTRY);
    }
    if ((len % VP_SECTOR) != 0u || len < VP_FILE_MIN || len > VP_FILE_MAX) {
        return fail(why, VP_ERR_FILE_SIZE, VP_NO_ENTRY);
    }
    uint32_t payload_length = rd32(file + H_PAYLOAD_LENGTH);
    if (payload_length < VP_HEADER_SIZE || payload_length > VP_PAYLOAD_MAX) {
        return fail(why, VP_ERR_PADDING, VP_NO_ENTRY);
    }
    uint64_t padded = ((uint64_t)payload_length + VP_SECTOR - 1u) / VP_SECTOR * VP_SECTOR;
    if (padded + VP_SECTOR != (uint64_t)len ||
        !all_bytes(file + payload_length, (size_t)(padded - payload_length), 0xFFu)) {
        return fail(why, VP_ERR_PADDING, VP_NO_ENTRY);
    }
    return vp_open(file, payload_length, out, why);
}

static const uint8_t *entry_at(const vp_pack_t *pack, uint32_t index)
{
    return pack->base + VP_HEADER_SIZE + (size_t)index * VP_INDEX_ENTRY_SIZE;
}

bool vp_clip_at(const vp_pack_t *pack, uint16_t index, vp_clip_t *out)
{
    if (pack == NULL || pack->base == NULL || out == NULL || index >= pack->clip_count) {
        return false;
    }
    entry_to_clip(pack->base, entry_at(pack, index), out);
    return true;
}

bool vp_find(const vp_pack_t *pack, uint16_t id, vp_clip_t *out)
{
    if (pack == NULL || pack->base == NULL || out == NULL) {
        return false;
    }
    uint32_t lo = 0, hi = pack->clip_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint16_t at = rd16(entry_at(pack, mid) + E_ID);
        if (at == id) {
            entry_to_clip(pack->base, entry_at(pack, mid), out);
            return true;
        }
        if (at < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return false;
}

uint16_t vp_ending_id(uint16_t word_id)
{
    if (word_id < VP_TOKEN_FIRST || word_id > VP_ENDING_WORD_LAST) {
        return VP_TOKEN_NONE;
    }
    return (uint16_t)(VP_ENDING_BASE + word_id);
}

bool vp_find_ending(const vp_pack_t *pack, uint16_t word_id, vp_clip_t *out)
{
    const uint16_t id = vp_ending_id(word_id);
    return id != VP_TOKEN_NONE && vp_find(pack, id, out) && out->category == (uint8_t)VP_CAT_ENDING;
}

bool vp_next_in_category(const vp_pack_t *pack, uint8_t category, uint16_t *cursor, vp_clip_t *out)
{
    if (pack == NULL || pack->base == NULL || cursor == NULL || out == NULL) {
        return false;
    }
    for (uint32_t i = *cursor; i < pack->clip_count; i++) {
        uint8_t cat = entry_at(pack, i)[E_CATEGORY];
        if (category == 0u ? vp_category_selectable(cat) : cat == category) {
            entry_to_clip(pack->base, entry_at(pack, i), out);
            *cursor = (uint16_t)(i + 1u);
            return true;
        }
    }
    *cursor = pack->clip_count;
    return false;
}

void vp_packets_begin(const vp_clip_t *clip, vp_packet_iter_t *it)
{
    it->p = clip->data;
    it->end = clip->data + clip->data_length;
}

bool vp_packet_next(vp_packet_iter_t *it, const uint8_t **packet, uint16_t *length)
{
    if (it->p == NULL || it->end - it->p < 2) {
        return false;
    }
    uint16_t plen = rd16(it->p);
    if ((size_t)(it->end - it->p - 2) < plen) {
        return false; /* never on an opened pack */
    }
    *packet = it->p + 2;
    *length = plen;
    it->p += 2u + plen;
    return true;
}

bool vp_check_recipe(const vp_pack_t *pack, const uint16_t *ids, size_t n, vp_recipe_check_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (n == 0u || ids == NULL || pack == NULL) {
        out->verdict = VP_RECIPE_EMPTY;
        return false;
    }
    size_t m = n < VP_RECIPE_MAX ? n : VP_RECIPE_MAX;
    bool bad = false;
    uint32_t total = 0;
    size_t known = 0;
    for (size_t i = 0; i < m; i++) {
        vp_clip_t c;
        if (ids[i] == 0u || !vp_find(pack, ids[i], &c)) {
            out->state[i] = VP_TOKEN_MISSING;
            bad = true;
        } else if (!vp_category_selectable(c.category)) {
            out->state[i] = VP_TOKEN_NOT_SELECTABLE;
            bad = true;
        } else {
            out->state[i] = VP_TOKEN_OK;
            vp_clip_t end;
            if (i + 1u == n && vp_find_ending(pack, ids[i], &end)) {
                c = end; /* the last word plays as its ending (AUD-031) */
            }
            total += c.valid_samples; /* at most 8 x 160000: no overflow */
            known++;
        }
    }
    if (known > 1u) {
        total += (uint32_t)pack->word_gap_ms * (VP_SAMPLE_RATE / 1000u) * (uint32_t)(known - 1u);
    }
    out->total_samples = total;
    if (n > VP_RECIPE_MAX) {
        out->verdict = VP_RECIPE_TOO_MANY;
    } else if (bad) {
        out->verdict = VP_RECIPE_BAD_TOKEN;
    } else if (total > VP_SPOKEN_SAMPLES_MAX) {
        out->verdict = VP_RECIPE_TOO_LONG;
    } else {
        out->verdict = VP_RECIPE_OK;
    }
    return out->verdict == VP_RECIPE_OK;
}
