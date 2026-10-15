/*
 * voice_pack.h - the reader of an RVOC format 1 voice pack (docs/features/08 voice-first-build.md
 * "The voice pack: RVOC format 1", D08). A stored format: a pack one producer writes must read on
 * every later firmware; components/core/voice_pack/test/voice_pack_fixture_v1.h is frozen.
 *
 * The caller hands over memory it keeps (the pack in flash, mapped, or a file in RAM); the reader
 * copies nothing out of it but the header's fields, allocates nothing and keeps no state. Every
 * accessor works on a pack that vp_open() (or vp_open_signed()) accepted, and only while its
 * memory stays put.
 *
 * vp_open() checks everything D08 asks of the payload: header values, non-overflowing and
 * non-overlapping regions, alignment and zero bytes, the index (ascending unique IDs, categories in
 * their namespace, text and script keys), every clip's framing, Opus packet sample counts, sample
 * metadata and SHA-256, and the coverage (all 16 status and walk IDs, at least one selectable
 * token). It does not check the signature (the platform does, before), does not decode audio, and
 * does not decide policy: it reports the DRAFT flag, the locale and the voice; whether a release
 * build or the en-US-only first build accepts them is the caller's choice (AUD-024).
 *
 * Plain C11, portable (components/core rules).
 */
#ifndef VOICE_PACK_VOICE_PACK_H
#define VOICE_PACK_VOICE_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_pack/voice_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The format's constants (D08) ---- */
#define VP_MAGIC "RVOC"
#define VP_FORMAT 1u
#define VP_HEADER_SIZE 128u
#define VP_INDEX_ENTRY_SIZE 64u
#define VP_VOCABULARY_SCHEMA 1u
/* Schema 2 adds endings (category 7, AUD-031): a word as it sounds at the end of a spoken name,
 * ID VP_ENDING_BASE + the word's. A reader before it refuses a schema 2 pack (VP_ERR_VERSION). */
#define VP_VOCABULARY_SCHEMA_ENDINGS 2u
#define VP_FLAG_DRAFT 0x00000001u
#define VP_SAMPLE_RATE 16000u
#define VP_CODEC_OPUS 1u
#define VP_CHANNELS 1u
#define VP_FRAME_MS 20u
#define VP_FRAME_SAMPLES 320u        /* one 20 ms Opus packet at 16 kHz */
#define VP_CLIPS_MAX 512u
#define VP_STRINGS_MAX 32768u
#define VP_WORD_GAP_MS 20u           /* what the supplied packs carry between words of a phrase */
#define VP_PART_GAP_MS 300u          /* ... and before a new sentence */
#define VP_WORD_GAP_MAX_MS 100u      /* a pack's word gap: 0-100 ms */
#define VP_PART_GAP_MIN_MS 100u      /* a pack's part gap: 100-1000 ms */
#define VP_PART_GAP_MAX_MS 1000u
#define VP_NOMINAL_BITRATE 32000u
#define VP_LOCALE_SIZE 16u           /* NUL-terminated, zero-padded */
#define VP_VOICE_ID_SIZE 32u         /* NUL-terminated, zero-padded */
#define VP_TEXT_MAX 47u              /* display text bytes, without its NUL */
#define VP_KEY_MAX 63u               /* script key bytes, without its NUL */
#define VP_PACKET_MAX 1275u          /* bytes of one Opus packet (RFC 6716) */
#define VP_PACKETS_MAX 501u          /* packets in one clip */
#define VP_CLIP_SAMPLES_MAX 160000u  /* 10 s at 16 kHz */

/* The signed file: payload, 0xFF to a 4096-byte boundary, one 4096-byte SBV2 signature sector. */
#define VP_SECTOR 4096u
#define VP_FILE_MIN 8192u
#define VP_FILE_MAX 970752u
#define VP_PAYLOAD_MAX 966656u

/* ---- A spoken name (D02) ---- */
#define VP_RECIPE_MAX 8u                 /* selectable entries in one spoken name */
#define VP_SPOKEN_SAMPLES_MAX 160000u    /* 10 s, joins included */

/* Index categories: 1-4 selectable tokens (ID 0x0001-0x7FFF), 5 status, 6 walk test, 7 a word's
 * ending (schema 2; ID 0xC001-0xFFFF, never selectable). */
typedef enum {
    VP_CAT_POSITION = 1,
    VP_CAT_LOCATION = 2,
    VP_CAT_OBJECT = 3,
    VP_CAT_NUMBER = 4,
    VP_CAT_STATUS = 5,
    VP_CAT_WALK = 6,
    VP_CAT_ENDING = 7,
} vp_category_t;

/* Why vp_open() refused a pack. Values are for logs and tests, not stored. */
typedef enum {
    VP_OK = 0,
    VP_ERR_ARG,          /* NULL buffer or output */
    VP_ERR_FILE_SIZE,    /* signed file: not a multiple of 4096, or outside 8192..970752 */
    VP_ERR_PADDING,      /* signed file: payload + 0xFF padding + one signature sector != file */
    VP_ERR_TRUNCATED,    /* shorter than the header, or payload_length != the bytes given */
    VP_ERR_MAGIC,
    VP_ERR_VERSION,      /* format, header_size, vocabulary_schema or index_entry_size */
    VP_ERR_HEADER,       /* a fixed header value: sample rate, codec, channels, frame, gaps, bitrate */
    VP_ERR_REVISION,     /* pack_revision 0 */
    VP_ERR_FLAGS,        /* a flag other than DRAFT */
    VP_ERR_RESERVED,     /* a reserved header or index byte not zero */
    VP_ERR_IDENTITY,     /* locale or voice_id: empty, unterminated, not printable ASCII, not zero-padded */
    VP_ERR_CLIP_COUNT,   /* clip_count outside 1..512 */
    VP_ERR_LAYOUT,       /* regions out of range, overlapping, misaligned, or leaving bytes unaccounted */
    VP_ERR_ALIGN_BYTES,  /* an alignment byte not zero */
    VP_ERR_ORDER,        /* index IDs not strictly ascending (or ID 0) */
    VP_ERR_CATEGORY,     /* category unknown or not matching its ID namespace */
    VP_ERR_TEXT,         /* display text: out of the strings region, length, UTF-8, control character */
    VP_ERR_KEY,          /* script key: out of the strings region, length, not printable ASCII */
    VP_ERR_KEY_DUPLICATE,
    VP_ERR_CLIP_RANGE,   /* clip data outside the audio region, misaligned, or out of index order */
    VP_ERR_FRAMING,      /* packet lengths: 0, > 1275, past the clip, bytes left, count mismatch */
    VP_ERR_PACKET,       /* an Opus packet that is not one 20 ms frame (TOC code 0) */
    VP_ERR_SAMPLES,      /* valid_samples, pre_skip, packet_count or trailing discard out of range */
    VP_ERR_HASH,         /* a clip's SHA-256 differs */
    VP_ERR_COVERAGE,     /* a status or walk ID missing, or no selectable token */
    VP_ERR_ENDING,       /* an ending whose word is not a selectable entry of the pack */
} vp_err_t;

/* Where vp_open() stopped: the code, and the index entry (VP_NO_ENTRY for the header/layout). */
#define VP_NO_ENTRY 0xFFFFu
typedef struct {
    vp_err_t err;
    uint16_t entry;
} vp_problem_t;

/* An accepted pack: its header's fields and the memory it lives in. */
typedef struct {
    const uint8_t *base;          /* the payload's first byte (the magic) */
    uint32_t payload_length;
    uint32_t pack_revision;
    uint32_t flags;
    char locale[VP_LOCALE_SIZE];  /* e.g. "en-US"; NUL-terminated */
    char voice_id[VP_VOICE_ID_SIZE];
    uint16_t clip_count;
    uint16_t word_gap_ms;
    uint16_t part_gap_ms;
    uint32_t nominal_bitrate;
    uint16_t selectable_count;    /* entries in categories 1-4 */
    uint32_t vocabulary_schema;   /* 1, or 2 with endings */
    uint16_t ending_count;        /* entries in category 7 */
} vp_pack_t;

/* One clip, pointing into the pack. */
typedef struct {
    uint16_t id;
    uint8_t category;             /* vp_category_t */
    const char *text;             /* display text, UTF-8, NUL-terminated (data: never translated) */
    const char *key;              /* script key, e.g. "word.front" */
    const uint8_t *data;          /* u16 LE packet_length + packet, packet_count times */
    uint32_t data_length;
    uint32_t valid_samples;       /* at 16 kHz, after pre_skip */
    uint16_t packet_count;
    uint16_t pre_skip;            /* at 16 kHz, discarded from the first decoded samples */
    const uint8_t *sha256;        /* 32 bytes */
} vp_clip_t;

/* Walks a clip's packets. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} vp_packet_iter_t;

/* Validates a payload (header .. end of audio) of exactly `len` bytes; fills *out only on VP_OK.
 * `why` may be NULL. */
vp_err_t vp_open(const uint8_t *payload, size_t len, vp_pack_t *out, vp_problem_t *why);

/* Validates a whole signed file as stored: its size, its 0xFF padding after payload_length and
 * one trailing signature sector (whose contents the platform checks, not this), then vp_open()
 * on the payload. */
vp_err_t vp_open_signed(const uint8_t *file, size_t len, vp_pack_t *out, vp_problem_t *why);

/* A short name for an error, for logs ("hash", "coverage"...). */
const char *vp_err_name(vp_err_t err);

static inline bool vp_is_draft(const vp_pack_t *pack)
{
    return (pack->flags & VP_FLAG_DRAFT) != 0u;
}

/* True when the category is a selectable vocabulary category (1-4). */
bool vp_category_selectable(uint8_t category);

/* The clip at index position `index` (0..clip_count-1), in ascending ID order. */
bool vp_clip_at(const vp_pack_t *pack, uint16_t index, vp_clip_t *out);

/* Finds a clip by ID (binary search). False when the pack has no such ID. */
bool vp_find(const vp_pack_t *pack, uint16_t id, vp_clip_t *out);

/* The ID of a word's ending (VP_ENDING_BASE + the word's ID), or VP_TOKEN_NONE for an ID that can
 * have none (0, or above VP_ENDING_WORD_LAST). */
uint16_t vp_ending_id(uint16_t word_id);

/* A word's ending, when the pack has one (AUD-031). */
bool vp_find_ending(const vp_pack_t *pack, uint16_t word_id, vp_clip_t *out);

/* The next clip of `category` at or after index position *cursor, ascending ID; *cursor moves past
 * it. Category 0 means any selectable category (1-4). Start with *cursor = 0. False at the end. */
bool vp_next_in_category(const vp_pack_t *pack, uint8_t category, uint16_t *cursor, vp_clip_t *out);

/* The clip's packets: vp_packets_begin(), then vp_packet_next() until it returns false. */
void vp_packets_begin(const vp_clip_t *clip, vp_packet_iter_t *it);
bool vp_packet_next(vp_packet_iter_t *it, const uint8_t **packet, uint16_t *length);

/* Decoded samples at 16 kHz of one Opus packet from its TOC byte (RFC 6716 section 3.1), for a
 * code 0 packet (one frame) of 1..1275 bytes: 40, 80, 160, 320, 640 or 960. 0 for codes 1-3, an
 * empty or oversized packet. A pack's packets must all give VP_FRAME_SAMPLES. */
uint32_t vp_opus_samples_16k(const uint8_t *packet, size_t length);

/* ---- Checking a spoken name (D02) ---- */
typedef enum {
    VP_TOKEN_OK = 0,
    VP_TOKEN_MISSING,          /* the pack has no clip with this ID (0 included) */
    VP_TOKEN_NOT_SELECTABLE,   /* a status or walk clip */
} vp_token_state_t;

typedef enum {
    VP_RECIPE_OK = 0,
    VP_RECIPE_EMPTY,           /* no entries: no spoken name */
    VP_RECIPE_TOO_MANY,        /* more than 8 entries */
    VP_RECIPE_BAD_TOKEN,       /* an entry is missing or not selectable (see state[]) */
    VP_RECIPE_TOO_LONG,        /* sum(valid_samples) + 960 * (n - 1) > 160000 */
} vp_recipe_verdict_t;

typedef struct {
    vp_recipe_verdict_t verdict;
    vp_token_state_t state[VP_RECIPE_MAX];   /* for the first min(n, 8) entries */
    uint32_t total_samples;                  /* of the known selectable entries, joins included */
} vp_recipe_check_t;

/* Checks up to eight token IDs against the pack: each exists and is selectable, and the whole
 * spoken name fits 10 s, its last word counted as its ending when the pack has one (as it plays,
 * AUD-031). Repeated IDs are allowed. Never truncates: an overlong recipe is refused.
 * Returns verdict == VP_RECIPE_OK. */
bool vp_check_recipe(const vp_pack_t *pack, const uint16_t *ids, size_t n, vp_recipe_check_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_PACK_VOICE_PACK_H */
