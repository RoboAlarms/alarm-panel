/*
 * test_voice_pack.c - the RVOC format 1 reader (docs/features/08 voice-first-build.md D08, D02, D07).
 *
 * AUD-026: the frozen fixture opens and reads back; the ID registry's constants never change; every
 * rejection reason on a copy of the fixture patched byte by byte. AUD-013: bounded, versioned,
 * integrity-checked (sizes, format/flags of a newer producer refused, SHA-256 per clip, the signed
 * file's padding). AUD-024: the locale and voice are reported, not decided, by the reader.
 * AUD-031: the frozen vocabulary schema 2 fixture (endings) opens; endings are never selectable,
 * need their word and their namespace, and only a schema 2 pack may hold them.
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "unity.h"
#include "voice_pack/voice_pack.h"
#include "voice_pack_fixture_v1.h"
#include "voice_pack_fixture_v2.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

#define FIX voice_pack_fixture_v1
#define FIX_LEN ((uint32_t)sizeof(voice_pack_fixture_v1))
#define E(i) (VP_HEADER_SIZE + (uint32_t)(i) * VP_INDEX_ENTRY_SIZE)

static uint8_t buf[VP_FILE_MAX];
static uint32_t blen;

static void w8(uint32_t off, uint32_t v) { buf[off] = (uint8_t)v; }
static void w16(uint32_t off, uint32_t v)
{
    buf[off] = (uint8_t)(v & 0xFFu);
    buf[off + 1u] = (uint8_t)((v >> 8) & 0xFFu);
}
static void w32(uint32_t off, uint32_t v)
{
    for (unsigned k = 0; k < 4u; k++) {
        buf[off + k] = (uint8_t)((v >> (8u * k)) & 0xFFu);
    }
}
static uint32_t r16(uint32_t off) { return (uint32_t)buf[off] | ((uint32_t)buf[off + 1u] << 8); }
static uint32_t r32(uint32_t off)
{
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1u] << 8) | ((uint32_t)buf[off + 2u] << 16) |
           ((uint32_t)buf[off + 3u] << 24);
}

static void fresh(void)
{
    memset(buf, 0, sizeof buf);
    memcpy(buf, FIX, FIX_LEN);
    blen = FIX_LEN;
}

/* Recomputes entry i's SHA-256 after its clip data was patched. */
static void rehash(uint32_t i)
{
    proto_sha256(buf + r32(E(i) + 12u), r32(E(i) + 16u), buf + E(i) + 28u);
}

/* The index position of an ID in the fixture. */
static uint32_t pos_of(uint32_t id)
{
    for (uint32_t i = 0; i < VOICE_PACK_FIXTURE_V1_CLIPS; i++) {
        if (r16(E(i)) == id) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE("id not in fixture");
    return 0;
}

static void expect(vp_err_t err, uint32_t entry)
{
    vp_pack_t p;
    vp_problem_t why;
    vp_err_t got = vp_open(buf, blen, &p, &why);
    char msg[64];
    snprintf(msg, sizeof msg, "want %s, got %s", vp_err_name(err), vp_err_name(got));
    TEST_ASSERT_EQUAL_INT_MESSAGE(err, got, msg);
    TEST_ASSERT_EQUAL_INT(err, why.err);
    TEST_ASSERT_EQUAL_UINT16(entry, why.entry);
}

#define HDR VP_NO_ENTRY

static void fresh2(void)
{
    memset(buf, 0, sizeof buf);
    memcpy(buf, voice_pack_fixture_v2, sizeof voice_pack_fixture_v2);
    blen = (uint32_t)sizeof voice_pack_fixture_v2;
}

/* AUD-026: the frozen format 1 fixture opens, with every header field read back. */
static void test_fixture_v1_opens(void)
{
    vp_pack_t p;
    vp_problem_t why;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(FIX, FIX_LEN, &p, &why));
    TEST_ASSERT_EQUAL_INT(VP_OK, why.err);
    TEST_ASSERT_EQUAL_PTR(FIX, p.base);
    TEST_ASSERT_EQUAL_UINT32(FIX_LEN, p.payload_length);
    TEST_ASSERT_EQUAL_UINT32(1, p.pack_revision);
    TEST_ASSERT_EQUAL_UINT32(0, p.flags);
    TEST_ASSERT_FALSE(vp_is_draft(&p));
    TEST_ASSERT_EQUAL_UINT16(VOICE_PACK_FIXTURE_V1_CLIPS, p.clip_count);
    TEST_ASSERT_EQUAL_UINT16(4, p.selectable_count);
    TEST_ASSERT_EQUAL_UINT16(60, p.word_gap_ms);
    TEST_ASSERT_EQUAL_UINT16(300, p.part_gap_ms);
    TEST_ASSERT_EQUAL_UINT32(32000, p.nominal_bitrate);
    /* AUD-024: the pack names its locale and voice */
    TEST_ASSERT_EQUAL_STRING("en-US", p.locale);
    TEST_ASSERT_EQUAL_STRING("qwen3-designed_female", p.voice_id);
}

/* AUD-026, AUD-013: the signed file as stored - payload, 0xFF to 4096, one signature sector. */
static void test_signed_file(void)
{
    vp_pack_t p;
    vp_problem_t why;
    fresh();
    memset(buf + FIX_LEN, 0xFF, 4096u - FIX_LEN);
    memset(buf + 4096u, 0xA5, 4096u); /* the signature sector: the platform's to check */
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open_signed(buf, 8192u, &p, &why));
    TEST_ASSERT_EQUAL_UINT32(FIX_LEN, p.payload_length);

    TEST_ASSERT_EQUAL_INT(VP_ERR_FILE_SIZE, vp_open_signed(buf, 8191u, &p, &why));
    TEST_ASSERT_EQUAL_INT(VP_ERR_FILE_SIZE, vp_open_signed(buf, 4096u, &p, &why));
    TEST_ASSERT_EQUAL_INT(VP_ERR_FILE_SIZE, vp_open_signed(buf, VP_FILE_MAX + 4096u, &p, &why));
    /* one sector too many: the signature is not the last sector after the padding */
    memset(buf + 8192u, 0xA5, 4096u);
    TEST_ASSERT_EQUAL_INT(VP_ERR_PADDING, vp_open_signed(buf, 12288u, &p, &why));
    /* a padding byte not 0xFF */
    buf[FIX_LEN + 10u] = 0xFE;
    TEST_ASSERT_EQUAL_INT(VP_ERR_PADDING, vp_open_signed(buf, 8192u, &p, &why));
    buf[FIX_LEN + 10u] = 0xFF;
    /* payload_length impossible or past the largest payload */
    w32(8, 100);
    TEST_ASSERT_EQUAL_INT(VP_ERR_PADDING, vp_open_signed(buf, 8192u, &p, &why));
    w32(8, VP_PAYLOAD_MAX + 1u);
    TEST_ASSERT_EQUAL_INT(VP_ERR_PADDING, vp_open_signed(buf, VP_FILE_MAX, &p, &why));
    /* payload_length longer than the real payload: the padding reads as payload */
    w32(8, FIX_LEN + 4u);
    TEST_ASSERT_EQUAL_INT(VP_ERR_LAYOUT, vp_open_signed(buf, 8192u, &p, &why));
    TEST_ASSERT_EQUAL_INT(VP_ERR_ARG, vp_open_signed(NULL, 8192u, &p, &why));
}

/* AUD-026: the ID registry's fixed status and walk IDs (D07) never change. */
static void test_registry_ids_frozen(void)
{
    static const uint16_t want[16] = {0x8001, 0x8002, 0x8003, 0x8004, 0x8005, 0x8006,
                                      0x8007, 0x8008, 0x8009, 0x800A, 0x8101, 0x8102,
                                      0x8103, 0x8104, 0x8105, 0x8106};
    const uint16_t got[16] = {VP_ID_ARMING, VP_ID_ARMED, VP_ID_AWAY, VP_ID_HOME,
                              VP_ID_NIGHT, VP_ID_EXIT_NOW, VP_ID_NO_ENTRY_DELAY, VP_ID_DISARMED,
                              VP_ID_ENTRY, VP_ID_NOT_READY, VP_ID_WALK_TEST, VP_ID_WALK_STARTED,
                              VP_ID_WALK_ENDED, VP_ID_WALK_ENDS_SOON, VP_ID_WALK_ALL_TESTED,
                              VP_ID_WALK_SOME_UNTESTED};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, got, 16);
    TEST_ASSERT_EQUAL_UINT16(0x7FFF, VP_TOKEN_LAST);
    TEST_ASSERT_EQUAL_UINT(10, VP_STATUS_COUNT);
    TEST_ASSERT_EQUAL_UINT(6, VP_WALK_COUNT);
    /* the fixture's status/walk entries carry the registry's script keys */
    vp_pack_t p;
    vp_clip_t c;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(FIX, FIX_LEN, &p, NULL));
    TEST_ASSERT_TRUE(vp_find(&p, VP_ID_EXIT_NOW, &c));
    TEST_ASSERT_EQUAL_STRING("status.exit_now", c.key);
    TEST_ASSERT_TRUE(vp_find(&p, VP_ID_WALK_SOME_UNTESTED, &c));
    TEST_ASSERT_EQUAL_STRING("walk.some_untested", c.key);
}

/* AUD-026: find by ID, the picker's walk by category, and a clip's packets. */
static void test_accessors(void)
{
    vp_pack_t p;
    vp_clip_t c;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(FIX, FIX_LEN, &p, NULL));
    for (uint16_t i = 0; i < p.clip_count; i++) {
        vp_clip_t a, b;
        TEST_ASSERT_TRUE(vp_clip_at(&p, i, &a));
        TEST_ASSERT_TRUE(vp_find(&p, a.id, &b));
        TEST_ASSERT_EQUAL_PTR(a.data, b.data);
    }
    TEST_ASSERT_FALSE(vp_clip_at(&p, p.clip_count, &c));
    static const uint16_t absent[] = {0, 2, 31, 194, 0x7FFF, 0x8000, 0x800B, 0x8100, 0x8107, 0xFFFF};
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; i++) {
        TEST_ASSERT_FALSE(vp_find(&p, absent[i], &c));
    }

    TEST_ASSERT_TRUE(vp_find(&p, 109, &c));
    TEST_ASSERT_EQUAL_UINT8(VP_CAT_OBJECT, c.category);
    TEST_ASSERT_EQUAL_STRING("Door", c.text);
    TEST_ASSERT_EQUAL_STRING("word.door", c.key);
    TEST_ASSERT_EQUAL_UINT32(209, c.valid_samples);
    TEST_ASSERT_EQUAL_UINT16(104, c.pre_skip);
    TEST_ASSERT_EQUAL_UINT16(1, c.packet_count);

    /* the picker: selectable words only, ascending ID */
    static const uint16_t words[] = {1, 32, 109, 193};
    uint16_t cur = 0;
    size_t k = 0;
    while (vp_next_in_category(&p, 0, &cur, &c)) {
        TEST_ASSERT_LESS_THAN(4, k);
        TEST_ASSERT_EQUAL_UINT16(words[k++], c.id);
    }
    TEST_ASSERT_EQUAL(4, k);
    cur = 0;
    TEST_ASSERT_TRUE(vp_next_in_category(&p, VP_CAT_LOCATION, &cur, &c));
    TEST_ASSERT_EQUAL_STRING("Basement", c.text);
    TEST_ASSERT_FALSE(vp_next_in_category(&p, VP_CAT_LOCATION, &cur, &c));
    cur = 0;
    k = 0;
    while (vp_next_in_category(&p, VP_CAT_STATUS, &cur, &c)) {
        k++;
    }
    TEST_ASSERT_EQUAL(10, k);
    cur = 0;
    TEST_ASSERT_FALSE(vp_next_in_category(&p, 7, &cur, &c));

    /* packets: "Front" is SILK 9 B, SILK 1275 B (the largest), hybrid 5 B */
    TEST_ASSERT_TRUE(vp_find(&p, 1, &c));
    static const uint16_t lens[] = {9, 1275, 5};
    vp_packet_iter_t it;
    const uint8_t *pkt;
    uint16_t len;
    vp_packets_begin(&c, &it);
    k = 0;
    while (vp_packet_next(&it, &pkt, &len)) {
        TEST_ASSERT_LESS_THAN(3, k);
        TEST_ASSERT_EQUAL_UINT16(lens[k++], len);
        TEST_ASSERT_EQUAL_UINT32(VP_FRAME_SAMPLES, vp_opus_samples_16k(pkt, len));
    }
    TEST_ASSERT_EQUAL(3, k);
    TEST_ASSERT_EQUAL_UINT16(3, c.packet_count);
}

/* AUD-026: an Opus packet's sample count from its TOC byte (RFC 6716 section 3.1). */
static void test_opus_toc(void)
{
    uint8_t pkt[1276];
    memset(pkt, 0, sizeof pkt);
    /* code 0: config << 3 | stereo << 2 | 0 */
    static const struct { unsigned config; uint32_t samples; } t[] = {
        {0, 160}, {1, 320}, {2, 640}, {3, 960},       /* SILK NB 10/20/40/60 ms */
        {5, 320}, {9, 320}, {11, 960},                /* SILK MB, WB */
        {12, 160}, {13, 320}, {14, 160}, {15, 320},   /* hybrid SWB, FB 10/20 ms */
        {16, 40}, {17, 80}, {18, 160}, {19, 320},     /* CELT NB 2.5/5/10/20 ms */
        {23, 320}, {27, 320}, {28, 40}, {31, 320},    /* CELT WB, SWB, FB */
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        for (unsigned s = 0; s < 2u; s++) {
            pkt[0] = (uint8_t)((t[i].config << 3) | (s << 2));
            TEST_ASSERT_EQUAL_UINT32(t[i].samples, vp_opus_samples_16k(pkt, 1));
            TEST_ASSERT_EQUAL_UINT32(t[i].samples, vp_opus_samples_16k(pkt, 1275));
        }
    }
    for (unsigned code = 1; code < 4u; code++) { /* two frames, two frames, signalled count */
        pkt[0] = (uint8_t)((31u << 3) | code);
        TEST_ASSERT_EQUAL_UINT32(0, vp_opus_samples_16k(pkt, 3));
    }
    pkt[0] = (uint8_t)(31u << 3);
    TEST_ASSERT_EQUAL_UINT32(0, vp_opus_samples_16k(pkt, 0));
    TEST_ASSERT_EQUAL_UINT32(0, vp_opus_samples_16k(pkt, 1276));
    TEST_ASSERT_EQUAL_UINT32(0, vp_opus_samples_16k(NULL, 1));
}

/* AUD-013, AUD-026: a short, foreign or newer file is refused before anything else is read. */
static void test_reject_header(void)
{
    vp_pack_t p;
    fresh();
    TEST_ASSERT_EQUAL_INT(VP_ERR_ARG, vp_open(NULL, FIX_LEN, &p, NULL));
    TEST_ASSERT_EQUAL_INT(VP_ERR_ARG, vp_open(buf, FIX_LEN, NULL, NULL));
    blen = 127; expect(VP_ERR_TRUNCATED, HDR);
    blen = FIX_LEN - 1u; expect(VP_ERR_TRUNCATED, HDR);
    blen = FIX_LEN + 4u; expect(VP_ERR_TRUNCATED, HDR); /* payload_length says less */
    fresh(); buf[3] = 'X'; expect(VP_ERR_MAGIC, HDR);
    fresh(); w16(4, 2); expect(VP_ERR_VERSION, HDR);     /* a newer format */
    fresh(); w16(6, 132); expect(VP_ERR_VERSION, HDR);   /* header_size */
    fresh(); w32(16, 0); expect(VP_ERR_VERSION, HDR);    /* vocabulary_schema */
    fresh(); w32(16, 3); expect(VP_ERR_VERSION, HDR);    /* a newer vocabulary */
    fresh(); w32(16, 2); expect(VP_OK, HDR);             /* schema 2 without endings (AUD-031) */
    fresh(); w16(82, 72); expect(VP_ERR_VERSION, HDR);   /* index_entry_size */
    fresh(); w32(8, VP_PAYLOAD_MAX + 4u); blen = VP_PAYLOAD_MAX + 4u; expect(VP_ERR_LAYOUT, HDR);
    fresh(); w32(12, 0); expect(VP_ERR_REVISION, HDR);
    fresh(); w32(20, 2); expect(VP_ERR_FLAGS, HDR);      /* a flag this reader doesn't know */
    fresh(); w32(20, 0x80000001u); expect(VP_ERR_FLAGS, HDR);
    fresh(); w8(112, 1); expect(VP_ERR_RESERVED, HDR);
    fresh(); w8(127, 0x80); expect(VP_ERR_RESERVED, HDR);
    fresh(); w32(72, 48000); expect(VP_ERR_HEADER, HDR);
    fresh(); w16(76, 2); expect(VP_ERR_HEADER, HDR);     /* codec */
    fresh(); w8(78, 2); expect(VP_ERR_HEADER, HDR);      /* channels */
    fresh(); w8(79, 10); expect(VP_ERR_HEADER, HDR);     /* frame_ms */
    fresh(); w16(104, 101); expect(VP_ERR_HEADER, HDR);  /* word_gap_ms over 100 */
    fresh(); w16(106, 99); expect(VP_ERR_HEADER, HDR);   /* part_gap_ms under 100 */
    fresh(); w16(106, 1001); expect(VP_ERR_HEADER, HDR); /* part_gap_ms over 1000 */
    fresh(); w32(108, 24000); expect(VP_ERR_HEADER, HDR);
    fresh(); w16(80, 0); expect(VP_ERR_CLIP_COUNT, HDR);
    fresh(); w16(80, 513); expect(VP_ERR_CLIP_COUNT, HDR);
}

/* AUD-026: a pack carries its own gaps within bounds (word 0-100 ms, part 100-1000 ms); the
 * supplied packs join a phrase's words by 20 ms (decided 2026-10-09), the fixture by 60. */
static void test_gaps_within_bounds(void)
{
    static const uint16_t gaps[][2] = {{0, 100}, {20, 300}, {100, 1000}};
    for (unsigned i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        vp_pack_t p;
        fresh();
        w16(104, gaps[i][0]);
        w16(106, gaps[i][1]);
        TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
        TEST_ASSERT_EQUAL_UINT16(gaps[i][0], p.word_gap_ms);
        TEST_ASSERT_EQUAL_UINT16(gaps[i][1], p.part_gap_ms);
    }
}

/* AUD-024: the reader reports a DRAFT pack and another locale; accepting them is the policy's. */
static void test_draft_and_identity(void)
{
    vp_pack_t p;
    fresh();
    w32(20, VP_FLAG_DRAFT);
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
    TEST_ASSERT_TRUE(vp_is_draft(&p));
    fresh();
    memcpy(buf + 24, "es-US", 5);
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
    TEST_ASSERT_EQUAL_STRING("es-US", p.locale);
    fresh(); memset(buf + 24, 0, 16); expect(VP_ERR_IDENTITY, HDR);          /* empty */
    fresh(); memset(buf + 24, 'a', 16); expect(VP_ERR_IDENTITY, HDR);        /* unterminated */
    fresh(); w8(24 + 10, 'x'); expect(VP_ERR_IDENTITY, HDR);                 /* not zero-padded */
    fresh(); w8(24 + 2, ' '); expect(VP_ERR_IDENTITY, HDR);                  /* not printable */
    fresh(); memset(buf + 40, 'v', 32); expect(VP_ERR_IDENTITY, HDR);        /* voice unterminated */
    fresh(); w8(40, 0x07); expect(VP_ERR_IDENTITY, HDR);
}

/* AUD-013, AUD-026: regions in range, in order, aligned, with zero alignment bytes. */
static void test_reject_layout(void)
{
    const uint32_t so = VOICE_PACK_FIXTURE_V1_STRINGS_OFFSET;
    const uint32_t ao = VOICE_PACK_FIXTURE_V1_AUDIO_OFFSET;
    fresh(); w32(84, 132); expect(VP_ERR_LAYOUT, HDR);              /* index_offset */
    fresh(); w32(88, so + 2u); expect(VP_ERR_LAYOUT, HDR);          /* strings misaligned */
    fresh(); w32(88, so + 4u); expect(VP_ERR_LAYOUT, HDR);          /* bytes unaccounted */
    fresh(); w32(88, so - 4u); expect(VP_ERR_LAYOUT, HDR);          /* strings over the index */
    fresh(); w32(92, 0); expect(VP_ERR_LAYOUT, HDR);
    fresh(); w32(92, VP_STRINGS_MAX + 1u); expect(VP_ERR_LAYOUT, HDR);
    fresh(); w32(92, r32(92) + 4u); expect(VP_ERR_LAYOUT, HDR);     /* strings over the audio */
    fresh(); w32(96, ao + 2u); expect(VP_ERR_LAYOUT, HDR);          /* audio misaligned */
    fresh(); w32(100, r32(100) + 1u); expect(VP_ERR_LAYOUT, HDR);   /* audio past the payload */
    fresh(); w32(100, r32(100) - 1u); expect(VP_ERR_LAYOUT, HDR);   /* audio short of it */
    fresh(); w32(96, 0xFFFFFFFCu); expect(VP_ERR_LAYOUT, HDR);      /* no wrap-around */
    fresh(); w32(100, 0xFFFFFFFFu); expect(VP_ERR_LAYOUT, HDR);
    fresh(); w32(92, 0xFFFFFFFFu); expect(VP_ERR_LAYOUT, HDR);
    fresh(); w16(80, VOICE_PACK_FIXTURE_V1_CLIPS - 1u); expect(VP_ERR_LAYOUT, HDR); /* 64 bytes unaccounted */
    fresh(); w8(ao - 1u, 1); expect(VP_ERR_ALIGN_BYTES, HDR);       /* strings-to-audio alignment */
    /* "Front" is 1295 bytes of clip data: one alignment byte before "Basement" */
    fresh(); w8(r32(E(0) + 12u) + 1295u, 0x55); expect(VP_ERR_ALIGN_BYTES, 1);
    /* 1-3 zero bytes after the last clip are alignment; 4 or a nonzero one are not */
    fresh(); w32(8, FIX_LEN + 3u); w32(100, r32(100) + 3u); blen = FIX_LEN + 3u;
    expect(VP_OK, HDR);
    w8(FIX_LEN + 1u, 9); expect(VP_ERR_ALIGN_BYTES, HDR);
    fresh(); w32(8, FIX_LEN + 4u); w32(100, r32(100) + 4u); blen = FIX_LEN + 4u;
    expect(VP_ERR_LAYOUT, HDR);
}

/* AUD-026: the index - ascending unique IDs, categories in their namespace, zero reserved bytes. */
static void test_reject_index(void)
{
    fresh(); w16(E(0), 0); expect(VP_ERR_ORDER, 0);                 /* ID 0 is none */
    fresh(); w16(E(1), 1); expect(VP_ERR_ORDER, 1);                 /* duplicate */
    fresh(); w16(E(1), 300); expect(VP_ERR_ORDER, 2);               /* descending */
    fresh(); w8(E(0) + 2u, 0); expect(VP_ERR_CATEGORY, 0);
    fresh(); w8(E(0) + 2u, 7); expect(VP_ERR_CATEGORY, 0);
    fresh(); w8(E(0) + 2u, VP_CAT_STATUS); expect(VP_ERR_CATEGORY, 0); /* a token as status */
    uint32_t armed = pos_of(VP_ID_ARMING);
    fresh(); w8(E(armed) + 2u, VP_CAT_POSITION); expect(VP_ERR_CATEGORY, armed); /* status as word */
    fresh(); w8(E(armed) + 2u, VP_CAT_WALK); expect(VP_ERR_CATEGORY, armed);
    uint32_t nr = pos_of(VP_ID_NOT_READY);
    fresh(); w16(E(nr), 0x800B); expect(VP_ERR_CATEGORY, nr);       /* outside the status range */
    fresh(); w16(E(nr), 0x8100); w8(E(nr) + 2u, VP_CAT_WALK); expect(VP_ERR_CATEGORY, nr);
    fresh(); w8(E(2) + 3u, 1); expect(VP_ERR_RESERVED, 2);
    fresh(); w8(E(2) + 60u, 1); expect(VP_ERR_RESERVED, 2);
    fresh(); w8(E(2) + 63u, 1); expect(VP_ERR_RESERVED, 2);
}

/* AUD-026: display text (UTF-8, 1-47 bytes, no control characters) and script keys. */
static void test_reject_text_and_keys(void)
{
    vp_pack_t p;
    vp_clip_t c;
    const uint32_t so = VOICE_PACK_FIXTURE_V1_STRINGS_OFFSET;
    const uint32_t se = so + r32(92);
    fresh();
    const uint32_t t0 = r32(E(0) + 4u); /* "Front" */
    /* UTF-8 is accepted: "Fr" e-acute "t"; a four-byte character */
    memcpy(buf + t0, "Fr\xC3\xA9t", 5);
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
    TEST_ASSERT_TRUE(vp_find(&p, 1, &c));
    TEST_ASSERT_EQUAL_STRING("Fr\xC3\xA9t", c.text);
    fresh(); memcpy(buf + t0, "F\xF0\x9F\x98\x80", 5);
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));

    static const char *const bad[] = {
        "Fr\x07nt",        /* C0 control */
        "Fr\x7Fnt",        /* DEL */
        "Fr\xC2\x85t",     /* C1 control (NEL) */
        "Fro\xFFt",        /* not UTF-8 */
        "Fr\xC0\x80t",     /* overlong NUL */
        "F\xED\xA0\x80t",  /* surrogate */
        "F\xF4\x90\x80\x80", /* past U+10FFFF */
        "Fron\xC3",        /* truncated sequence */
        "Fr\xC3t\xA9",     /* bad continuation */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fresh();
        memcpy(buf + t0, bad[i], 5);
        expect(VP_ERR_TEXT, 0);
    }
    fresh(); w8(t0, 0); expect(VP_ERR_TEXT, 0);                    /* empty */
    fresh(); w32(E(0) + 4u, so - 4u); expect(VP_ERR_TEXT, 0);      /* before the strings */
    fresh(); w32(E(0) + 4u, se); expect(VP_ERR_TEXT, 0);           /* past them */
    fresh(); w32(E(0) + 4u, 0xFFFFFFFFu); expect(VP_ERR_TEXT, 0);
    /* the last string runs to the region's end without a NUL */
    uint32_t last = VOICE_PACK_FIXTURE_V1_CLIPS - 1u;
    fresh(); w8(se - 1u, 'x'); expect(VP_ERR_KEY, last);
    /* 47 bytes is the longest text, 48 too long: written over "Not ready..." and what follows */
    uint32_t nr = pos_of(VP_ID_NOT_READY);
    uint32_t tn = r32(E(nr) + 4u);
    fresh(); memset(buf + tn, 'a', 47); w8(tn + 47u, 0);
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
    fresh(); memset(buf + tn, 'a', 48); w8(tn + 48u, 0); expect(VP_ERR_TEXT, nr);

    uint32_t k0 = r32(E(0) + 8u); /* "word.front" */
    fresh(); w8(k0 + 4u, 0x7F); expect(VP_ERR_KEY, 0);
    fresh(); w8(k0 + 4u, 0x80); expect(VP_ERR_KEY, 0);
    fresh(); w8(k0 + 4u, '\t'); expect(VP_ERR_KEY, 0);
    fresh(); w8(k0, 0); expect(VP_ERR_KEY, 0);
    fresh(); w32(E(0) + 8u, 4); expect(VP_ERR_KEY, 0);             /* in the header */
    /* 63 bytes is the longest key, 64 too long. The run overwrites the token strings after
     * "word.front" up to "Arming": "Basement" gets a 5-byte text from the run's end, "Arming"
     * shares "Front"; the other texts and keys are unique runs of 'k' */
    fresh(); memset(buf + k0, 'k', 63); w8(k0 + 63u, 0); w32(E(1) + 4u, k0 + 63u - 5u);
    w32(E(pos_of(VP_ID_ARMING)) + 4u, t0);
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
    fresh(); memset(buf + k0, 'k', 64); w8(k0 + 64u, 0); expect(VP_ERR_KEY, 0);

    /* duplicate script keys */
    fresh(); w32(E(3) + 8u, r32(E(1) + 8u)); expect(VP_ERR_KEY_DUPLICATE, 3);
    /* a shared display text is fine */
    fresh(); w32(E(3) + 4u, r32(E(1) + 4u));
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));
}

/* AUD-013, AUD-026: clip ranges, framing, packets, sample metadata, SHA-256. */
static void test_reject_clips(void)
{
    const uint32_t ao = VOICE_PACK_FIXTURE_V1_AUDIO_OFFSET;
    uint32_t d0, d1;
    fresh(); d0 = r32(E(0) + 12u); d1 = r32(E(1) + 12u);
    TEST_ASSERT_EQUAL_UINT32(ao, d0);
    fresh(); w32(E(1) + 12u, d1 + 2u); expect(VP_ERR_CLIP_RANGE, 1);       /* misaligned */
    fresh(); w32(E(1) + 12u, d1 + 4u); expect(VP_ERR_CLIP_RANGE, 1);       /* a gap */
    fresh(); w32(E(1) + 12u, d0); expect(VP_ERR_CLIP_RANGE, 1);            /* overlaps clip 0 */
    fresh(); w32(E(0) + 12u, ao - 4u); expect(VP_ERR_CLIP_RANGE, 0);       /* in the strings */
    fresh(); w32(E(0) + 16u, 0); expect(VP_ERR_CLIP_RANGE, 0);
    fresh(); w32(E(0) + 16u, FIX_LEN); expect(VP_ERR_CLIP_RANGE, 0);       /* past the audio */
    fresh(); w32(E(0) + 12u, 0xFFFFFFFCu); expect(VP_ERR_CLIP_RANGE, 0);
    fresh(); w32(E(0) + 16u, 0xFFFFFFFFu); expect(VP_ERR_CLIP_RANGE, 0);
    /* clips in another order than the index: clip 1 moved behind clip 2 */
    fresh(); w32(E(1) + 12u, r32(E(2) + 12u) + 4u); expect(VP_ERR_CLIP_RANGE, 1);

    /* framing: "Front" = [9][TOC..][1275][TOC..][5][TOC..] */
    fresh(); w16(d0, 0); expect(VP_ERR_FRAMING, 0);                        /* empty packet */
    fresh(); w16(d0 + 2u + 9u, 1276); expect(VP_ERR_FRAMING, 0);           /* over 1275 */
    fresh(); w16(d0 + 2u + 9u + 2u + 1275u, 6); expect(VP_ERR_FRAMING, 0); /* past the clip */
    fresh(); w32(E(0) + 16u, 1296); expect(VP_ERR_FRAMING, 0);             /* a stray byte */
    fresh(); w32(E(0) + 16u, 1297); expect(VP_ERR_FRAMING, 0);             /* runs into clip 1 */
    fresh(); w16(E(0) + 24u, 4); w32(E(0) + 20u, 1000); expect(VP_ERR_FRAMING, 0); /* 3 of 4 */
    fresh(); w16(E(0) + 24u, 2); w32(E(0) + 20u, 500); expect(VP_ERR_FRAMING, 0);  /* 3 of 2 */

    /* each packet one 20 ms frame */
    static const uint8_t bad_toc[] = {
        0x48 | 1, 0x48 | 2, 0x48 | 3, /* codes 1-3 */
        0x40, 0x50, 0x58,             /* SILK WB 10/40/60 ms */
        0x70, 0xF0, 0xE8, 0xE0,       /* hybrid FB 10, CELT FB 10/5/2.5 ms */
    };
    for (size_t i = 0; i < sizeof bad_toc; i++) {
        fresh(); w8(d0 + 2u + 9u + 2u, bad_toc[i]); rehash(0); expect(VP_ERR_PACKET, 0);
    }

    /* samples: Front has 3 packets, pre_skip 104, valid 756 (trailing 100) */
    fresh(); w16(E(0) + 24u, 0); expect(VP_ERR_SAMPLES, 0);
    fresh(); w16(E(0) + 24u, 502); expect(VP_ERR_SAMPLES, 0);
    fresh(); w16(E(0) + 26u, 321); expect(VP_ERR_SAMPLES, 0);
    fresh(); w32(E(0) + 20u, 0); expect(VP_ERR_SAMPLES, 0);
    fresh(); w32(E(0) + 20u, 160001); expect(VP_ERR_SAMPLES, 0);
    fresh(); w32(E(0) + 20u, 960u - 104u + 1u); expect(VP_ERR_SAMPLES, 0); /* past the end */
    fresh(); w32(E(0) + 20u, 960u - 104u); expect(VP_OK, HDR);           /* no trailing */
    fresh(); w32(E(0) + 20u, 960u - 104u - 319u); expect(VP_OK, HDR);    /* 319 trailing */
    fresh(); w32(E(0) + 20u, 960u - 104u - 320u); expect(VP_ERR_SAMPLES, 0); /* a whole packet */
    fresh(); w16(E(0) + 26u, 320); w32(E(0) + 20u, 640); expect(VP_OK, HDR); /* pre_skip 320 */

    /* SHA-256 of the length-prefixed data, the prefix included */
    fresh(); w8(d0 + 5u, (uint32_t)buf[d0 + 5u] ^ 1u); expect(VP_ERR_HASH, 0);
    fresh(); w8(E(0) + 28u + 31u, (uint32_t)buf[E(0) + 28u + 31u] ^ 0x80u); expect(VP_ERR_HASH, 0);
    uint32_t last = VOICE_PACK_FIXTURE_V1_CLIPS - 1u;
    fresh(); w8(FIX_LEN - 1u, (uint32_t)buf[FIX_LEN - 1u] ^ 1u); expect(VP_ERR_HASH, last);
}

/* AUD-026: all ten status and six walk IDs, and at least one selectable token. */
static void test_reject_coverage(void)
{
    /* the first status clip becomes token 0x7FFF: still ascending, nine status clips left */
    uint32_t a = pos_of(VP_ID_ARMING);
    fresh(); w16(E(a), 0x7FFF); w8(E(a) + 2u, VP_CAT_NUMBER); expect(VP_ERR_COVERAGE, HDR);
    uint32_t w = pos_of(VP_ID_WALK_SOME_UNTESTED);
    fresh(); w16(E(w), 0x8105); expect(VP_ERR_ORDER, w);
    /* a pack without its four tokens: their entries, and their clips at the audio's start, cut */
    fresh();
    const uint32_t n = VOICE_PACK_FIXTURE_V1_CLIPS;
    const uint32_t so = VOICE_PACK_FIXTURE_V1_STRINGS_OFFSET;
    const uint32_t ao = VOICE_PACK_FIXTURE_V1_AUDIO_OFFSET;
    const uint32_t cut_index = 4u * VP_INDEX_ENTRY_SIZE;
    const uint32_t cut_audio = r32(E(4) + 12u) - ao;
    static uint8_t out[sizeof(voice_pack_fixture_v1)];
    uint32_t o = 0;
    memcpy(out, buf, VP_HEADER_SIZE);
    o = VP_HEADER_SIZE;
    for (uint32_t i = 4; i < n; i++) {
        memcpy(out + o, buf + E(i), VP_INDEX_ENTRY_SIZE);
        o += VP_INDEX_ENTRY_SIZE;
    }
    memcpy(out + o, buf + so, ao - so);
    o += ao - so;
    memcpy(out + o, buf + r32(E(4) + 12u), FIX_LEN - r32(E(4) + 12u));
    o += FIX_LEN - r32(E(4) + 12u);
    memcpy(buf, out, o);
    blen = o;
    w32(8, o);
    w16(80, n - 4u);
    w32(88, so - cut_index);
    w32(96, ao - cut_index);
    w32(100, r32(100) - cut_audio);
    for (uint32_t i = 0; i < n - 4u; i++) {
        w32(E(i) + 4u, r32(E(i) + 4u) - cut_index);
        w32(E(i) + 8u, r32(E(i) + 8u) - cut_index);
        w32(E(i) + 12u, r32(E(i) + 12u) - cut_index - cut_audio);
    }
    expect(VP_ERR_COVERAGE, HDR);
    /* coverage is checked last: every entry of the cut pack passed */
}

/* D02: a spoken name - up to eight selectable entries, sum(samples) + 960 * (n - 1) <= 160000. */
static void test_recipe(void)
{
    vp_pack_t p;
    vp_recipe_check_t r;
    fresh();
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(buf, blen, &p, NULL));

    const uint16_t front_door[] = {1, 109};
    TEST_ASSERT_TRUE(vp_check_recipe(&p, front_door, 2, &r));
    TEST_ASSERT_EQUAL_INT(VP_RECIPE_OK, r.verdict);
    TEST_ASSERT_EQUAL_UINT32(756u + 209u + 960u, r.total_samples);
    const uint16_t one[] = {193};
    TEST_ASSERT_TRUE(vp_check_recipe(&p, one, 1, &r));
    TEST_ASSERT_EQUAL_UINT32(321u, r.total_samples);
    const uint16_t eight[] = {1, 1, 32, 32, 109, 109, 193, 193}; /* repeats allowed */
    TEST_ASSERT_TRUE(vp_check_recipe(&p, eight, 8, &r));
    TEST_ASSERT_EQUAL_UINT32(2u * (756u + 536u + 209u + 321u) + 7u * 960u, r.total_samples);
    const uint16_t nine[] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    TEST_ASSERT_FALSE(vp_check_recipe(&p, nine, 9, &r));
    TEST_ASSERT_EQUAL_INT(VP_RECIPE_TOO_MANY, r.verdict);
    TEST_ASSERT_FALSE(vp_check_recipe(&p, front_door, 0, &r));
    TEST_ASSERT_EQUAL_INT(VP_RECIPE_EMPTY, r.verdict);

    const uint16_t mixed[] = {1, 2, VP_ID_ARMED, 0, VP_ID_WALK_TEST, 32};
    TEST_ASSERT_FALSE(vp_check_recipe(&p, mixed, 6, &r));
    TEST_ASSERT_EQUAL_INT(VP_RECIPE_BAD_TOKEN, r.verdict);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_OK, r.state[0]);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_MISSING, r.state[1]);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_NOT_SELECTABLE, r.state[2]);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_MISSING, r.state[3]);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_NOT_SELECTABLE, r.state[4]);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_OK, r.state[5]);

    /* the 10 s budget, exactly: the index is patched after opening to reach it (the arithmetic
     * reads only valid_samples) */
    const uint16_t two[] = {1, 32};
    w32(E(0) + 20u, 80000u);
    w32(E(1) + 20u, 80000u - 960u);
    TEST_ASSERT_TRUE(vp_check_recipe(&p, two, 2, &r));
    TEST_ASSERT_EQUAL_UINT32(160000u, r.total_samples);
    w32(E(1) + 20u, 80000u - 960u + 1u);
    TEST_ASSERT_FALSE(vp_check_recipe(&p, two, 2, &r));
    TEST_ASSERT_EQUAL_INT(VP_RECIPE_TOO_LONG, r.verdict);
    TEST_ASSERT_EQUAL_UINT32(160001u, r.total_samples);
}

/* AUD-031: the frozen schema 2 fixture opens: its endings are counted apart from the words, found
 * by their word, never listed or accepted as a word, and a spoken name's last word is counted as
 * its ending in the 10-second budget, as it plays. */
static void test_fixture_v2_endings(void)
{
    vp_pack_t p;
    vp_problem_t why;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(voice_pack_fixture_v2, sizeof voice_pack_fixture_v2, &p, &why));
    TEST_ASSERT_EQUAL_UINT32(VP_VOCABULARY_SCHEMA_ENDINGS, p.vocabulary_schema);
    TEST_ASSERT_EQUAL_UINT32(2u, p.pack_revision);
    TEST_ASSERT_EQUAL_UINT16(VOICE_PACK_FIXTURE_V2_CLIPS, p.clip_count);
    TEST_ASSERT_EQUAL_UINT16(3u, p.selectable_count);
    TEST_ASSERT_EQUAL_UINT16(2u, p.ending_count);
    TEST_ASSERT_EQUAL_UINT16(40u, p.word_gap_ms);
    TEST_ASSERT_EQUAL_UINT16(0xC06Du, vp_ending_id(109u));
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, vp_ending_id(VP_ENDING_WORD_LAST));
    TEST_ASSERT_EQUAL_UINT16(VP_TOKEN_NONE, vp_ending_id(0u));
    TEST_ASSERT_EQUAL_UINT16(VP_TOKEN_NONE, vp_ending_id(VP_ENDING_WORD_LAST + 1u));
    vp_clip_t c;
    TEST_ASSERT_TRUE(vp_find_ending(&p, 109u, &c));
    TEST_ASSERT_EQUAL_UINT8(VP_CAT_ENDING, c.category);
    TEST_ASSERT_EQUAL_STRING("Door", c.text);
    TEST_ASSERT_EQUAL_STRING("word.door.end", c.key);
    TEST_ASSERT_EQUAL_UINT32(520u, c.valid_samples);
    TEST_ASSERT_TRUE(vp_find_ending(&p, 1u, &c));
    TEST_ASSERT_EQUAL_STRING("word.front.end", c.key);
    TEST_ASSERT_FALSE(vp_find_ending(&p, 193u, &c)); /* "Zero" has none */
    TEST_ASSERT_FALSE(vp_find_ending(&p, 0u, &c));
    vp_pack_t v1;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(FIX, FIX_LEN, &v1, NULL));
    TEST_ASSERT_EQUAL_UINT32(VP_VOCABULARY_SCHEMA, v1.vocabulary_schema);
    TEST_ASSERT_EQUAL_UINT16(0u, v1.ending_count);
    TEST_ASSERT_FALSE(vp_find_ending(&v1, 109u, &c)); /* a schema 1 pack: the words as they are */

    uint16_t cursor = 0;
    unsigned words = 0;
    while (vp_next_in_category(&p, 0u, &cursor, &c)) {
        TEST_ASSERT_TRUE(vp_category_selectable(c.category));
        words++;
    }
    TEST_ASSERT_EQUAL_UINT(3u, words);
    cursor = 0;
    TEST_ASSERT_TRUE(vp_next_in_category(&p, VP_CAT_ENDING, &cursor, &c));
    TEST_ASSERT_EQUAL_UINT16(0xC001u, c.id);

    vp_recipe_check_t r;
    const uint16_t front_door[2] = {1u, 109u};
    TEST_ASSERT_TRUE(vp_check_recipe(&p, front_door, 2u, &r));
    TEST_ASSERT_EQUAL_UINT32(200u + 520u + 40u * 16u, r.total_samples); /* "Door" as its ending */
    const uint16_t door_zero[2] = {109u, 193u};
    TEST_ASSERT_TRUE(vp_check_recipe(&p, door_zero, 2u, &r));
    TEST_ASSERT_EQUAL_UINT32(210u + 300u + 40u * 16u, r.total_samples); /* "Door" mid-name */
    const uint16_t an_ending[1] = {0xC06Du};
    TEST_ASSERT_FALSE(vp_check_recipe(&p, an_ending, 1u, &r));
    TEST_ASSERT_EQUAL_INT(VP_RECIPE_BAD_TOKEN, r.verdict);
    TEST_ASSERT_EQUAL_INT(VP_TOKEN_NOT_SELECTABLE, r.state[0]);
}

/* AUD-031: only a schema 2 pack may hold an ending; an ending lives in 0xC001-0xFFFF and needs its
 * word, a selectable entry of the same pack; no other category moves into its namespace. */
static void test_reject_endings(void)
{
    /* index: 1, 0x6D, 0xC1, ten status, six walk, then 0xC001 (19) and 0xC06D (20) */
    fresh2(); expect(VP_OK, HDR);
    fresh2(); w32(16, VP_VOCABULARY_SCHEMA); expect(VP_ERR_CATEGORY, 19u);
    fresh2(); w32(16, 3); expect(VP_ERR_VERSION, HDR);
    fresh2(); w16(E(20), 0xC06Eu); expect(VP_ERR_ENDING, 20u);      /* no word 0x6E */
    fresh2(); w16(E(1), 0x6Cu); expect(VP_ERR_ENDING, 20u);         /* "Door" renumbered */
    fresh2(); w16(E(19), VP_ENDING_BASE); expect(VP_ERR_CATEGORY, 19u); /* no word 0 */
    fresh2(); w8(E(1) + 2u, VP_CAT_ENDING); expect(VP_ERR_CATEGORY, 1u);
    fresh2(); w8(E(20) + 2u, VP_CAT_OBJECT); expect(VP_ERR_CATEGORY, 20u);
    fresh2(); w8(E(20) + 2u, 8u); expect(VP_ERR_CATEGORY, 20u);     /* a category to come */
    fresh2(); w8(E(20) + 3u, 1u); expect(VP_ERR_RESERVED, 20u);
}

static void test_err_names(void)
{
    for (int e = VP_OK; e <= VP_ERR_ENDING; e++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", vp_err_name((vp_err_t)e)));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixture_v1_opens);
    RUN_TEST(test_signed_file);
    RUN_TEST(test_registry_ids_frozen);
    RUN_TEST(test_accessors);
    RUN_TEST(test_opus_toc);
    RUN_TEST(test_reject_header);
    RUN_TEST(test_gaps_within_bounds);
    RUN_TEST(test_draft_and_identity);
    RUN_TEST(test_reject_layout);
    RUN_TEST(test_reject_index);
    RUN_TEST(test_reject_text_and_keys);
    RUN_TEST(test_reject_clips);
    RUN_TEST(test_reject_coverage);
    RUN_TEST(test_recipe);
    RUN_TEST(test_fixture_v2_endings);
    RUN_TEST(test_reject_endings);
    RUN_TEST(test_err_names);
    return UNITY_END();
}
