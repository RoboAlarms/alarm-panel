/*
 * test_asset_pack.c - the asset pack reader, `RAST` format 1 (components/core/pack_store
 * asset_pack.h): the frozen fixture, every truncation, a mutation of every header and part field,
 * layout gaps, overlaps and misplaced parts, alignment bytes, unknown kinds, types and flags,
 * names and versions, part counts and overflowing offsets.
 * Requirement: OPT-012 (docs/features/39-Optimizations, asset-packs.md "The pack file (format 1)");
 * asset_pack_made_for: the certificates rule, OPT-013; the kinds' rules and the picture's JPEG
 * check (asset_pack_check_kind, asset_jpeg_fits): OPT-006.
 */
#include <stdint.h>
#include <string.h>

#include "asset_pack_fixture_v1.h"
#include "pack_store/asset_pack.h"
#include "unity.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

#define BUF 2048u

typedef union {
    uint32_t align;
    uint8_t b[BUF];
} buf_t;

static buf_t g_buf;
static asset_pack_t g_pack;

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* A fresh copy of the fixture in g_buf (the rest zero); returns its length. */
static size_t fixture(void)
{
    memset(g_buf.b, 0, sizeof g_buf.b);
    memcpy(g_buf.b, ASSET_PACK_FIXTURE_V1, sizeof ASSET_PACK_FIXTURE_V1);
    return sizeof ASSET_PACK_FIXTURE_V1;
}

static asset_pack_result_t open_n(size_t n)
{
    return asset_pack_open(g_buf.b, n, &g_pack);
}

/* The fixture with one change made by the caller, then opened: a macro so the line shows. */
#define EXPECT(code, ...)                                                                           \
    do {                                                                                            \
        size_t n_ = fixture();                                                                      \
        uint8_t *b = g_buf.b;                                                                       \
        (void)b;                                                                                    \
        __VA_ARGS__;                                                                                \
        TEST_ASSERT_EQUAL_STRING(asset_pack_result_name(code), asset_pack_result_name(open_n(n_))); \
    } while (0)

/* A canonical pack built here: `count` parts named p0.. of type 1, part i of size sizes[i] filled
 * with 0x5A. Returns its length. */
static size_t build(uint8_t kind, uint32_t count, const uint32_t *sizes)
{
    memset(g_buf.b, 0, sizeof g_buf.b);
    uint8_t *b = g_buf.b;
    memcpy(b, "RAST", 4u);
    wr16(b + 4, 1u);
    wr16(b + 6, 128u);
    b[12] = kind;
    wr16(b + 14, count);
    memcpy(b + 16, "letters", 7u);
    memcpy(b + 32, "0.9.0", 5u);
    wr32(b + 52, 128u);
    uint32_t pos = 128u + 32u * count;
    for (uint32_t i = 0u; i < count; i++) {
        uint8_t *r = b + 128u + 32u * i;
        r[0] = 'p';
        r[1] = (uint8_t)('a' + i);
        wr32(r + 16, 1u);
        pos = (pos + 3u) & ~3u;
        wr32(r + 20, pos);
        wr32(r + 24, sizes[i]);
        memset(b + pos, 0x5A, sizes[i]);
        pos += sizes[i];
    }
    pos = (pos + 3u) & ~3u;
    wr32(b + 8, pos);
    return pos;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* In build()'s pack: the pack's name, and part i's name and type. */
static void set_name(const char *name)
{
    memset(g_buf.b + 16, 0, 16u);
    memcpy(g_buf.b + 16, name, strlen(name));
}

static void set_part(uint32_t i, const char *name, uint32_t type)
{
    uint8_t *r = g_buf.b + 128u + 32u * i;
    memset(r, 0, 16u);
    memcpy(r, name, strlen(name));
    wr32(r + 16, type);
}

static uint8_t *part_data(uint32_t i)
{
    return g_buf.b + rd32(g_buf.b + 128u + 32u * i + 20u);
}

/* The start of a JPEG: SOI, APP0 and DHT (each with 2 bytes of data), then a frame header `marker`
 * (0xC0 a baseline one) of h x w with one component. Returns its length (27). */
static size_t jpeg(uint8_t *d, uint32_t marker, uint32_t h, uint32_t w)
{
    static const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46, 0xFF, 0xC4, 0x00, 0x04, 0x00, 0x00};
    memcpy(d, head, sizeof head);
    uint8_t *f = d + sizeof head;
    const uint8_t frame[13] = {0xFF, (uint8_t)marker, 0x00, 0x0B, 8, (uint8_t)(h >> 8), (uint8_t)(h & 0xFFu),
                               (uint8_t)(w >> 8), (uint8_t)(w & 0xFFu), 1, 1, 0x11, 0};
    memcpy(f, frame, sizeof frame);
    return sizeof head + sizeof frame;
}

/* A picture pack whose one part `photo` is jpeg()'s, opened; returns its length. */
static size_t picture(uint32_t marker, uint32_t h, uint32_t w)
{
    uint8_t jp[32];
    const uint32_t sizes[1] = {(uint32_t)jpeg(jp, marker, h, w)};
    const size_t n = build(PS_KIND_PICTURE, 1u, sizes);
    set_name("saver");
    set_part(0u, "photo", ASSET_PART_JPEG);
    memcpy(part_data(0u), jp, sizes[0]);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    return n;
}

/* ------------------------------------------------------------------ the fixture */

/* OPT-012: the frozen fixture opens, field by field. */
static void test_fixture_opens_with_its_fields(void)
{
    const size_t n = fixture();
    TEST_ASSERT_EQUAL_UINT(ASSET_PACK_FIXTURE_V1_LENGTH, n);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_PTR(g_buf.b, g_pack.payload);
    TEST_ASSERT_EQUAL_UINT32(ASSET_PACK_FIXTURE_V1_LENGTH, g_pack.payload_length);
    TEST_ASSERT_EQUAL_UINT(ASSET_PACK_FIXTURE_V1_KIND, g_pack.kind);
    TEST_ASSERT_EQUAL_UINT(PS_KIND_LETTERS, g_pack.kind);
    TEST_ASSERT_EQUAL_UINT(0u, g_pack.flags);
    TEST_ASSERT_FALSE(g_pack.draft);
    TEST_ASSERT_FALSE(asset_pack_is_draft(&g_pack));
    TEST_ASSERT_EQUAL_STRING(ASSET_PACK_FIXTURE_V1_NAME, g_pack.name);
    TEST_ASSERT_EQUAL_STRING(ASSET_PACK_FIXTURE_V1_VERSION, g_pack.version);
    TEST_ASSERT_EQUAL_HEX32(ASSET_PACK_FIXTURE_V1_MATCH, g_pack.match);
    TEST_ASSERT_EQUAL_UINT(ASSET_PACK_FIXTURE_V1_PARTS, g_pack.part_count);
    TEST_ASSERT_EQUAL_STRING("letters", asset_pack_store_name(g_pack.kind));

    static const char *const names[3] = {"caption", "body", "title"};
    static const uint32_t offs[3] = {224u, 232u, 240u};
    static const uint32_t sizes[3] = {5u, 8u, 3u};
    static const uint8_t first[3] = {0x10u, 0xA0u, 0xF0u};
    for (uint16_t i = 0u; i < 3u; i++) {
        const asset_part_t *p = asset_pack_part(&g_pack, i);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_STRING(names[i], p->name);
        TEST_ASSERT_EQUAL_UINT32(ASSET_PART_FONT, p->type);
        TEST_ASSERT_EQUAL_PTR(g_buf.b + offs[i], p->data);
        TEST_ASSERT_EQUAL_UINT32(sizes[i], p->size);
        for (uint32_t k = 0u; k < sizes[i]; k++) {
            TEST_ASSERT_EQUAL_HEX8((uint8_t)(first[i] + k), p->data[k]);
        }
        TEST_ASSERT_EQUAL_PTR(p, asset_pack_find(&g_pack, names[i]));
    }
    TEST_ASSERT_NULL(asset_pack_part(&g_pack, 3u));
    TEST_ASSERT_NULL(asset_pack_find(&g_pack, "label"));
    TEST_ASSERT_NULL(asset_pack_find(&g_pack, "bod"));
    TEST_ASSERT_NULL(asset_pack_find(&g_pack, NULL));
    TEST_ASSERT_NULL(asset_pack_find(NULL, "body"));
    TEST_ASSERT_NULL(asset_pack_part(NULL, 0u));
}

/* OPT-013: made_for is an exact match of the version. */
static void test_made_for_is_exact(void)
{
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(fixture()));
    TEST_ASSERT_TRUE(asset_pack_made_for(&g_pack, "0.9.0"));
    TEST_ASSERT_FALSE(asset_pack_made_for(&g_pack, "0.9.1"));
    TEST_ASSERT_FALSE(asset_pack_made_for(&g_pack, "0.9"));
    TEST_ASSERT_FALSE(asset_pack_made_for(&g_pack, "0.9.0 "));
    TEST_ASSERT_FALSE(asset_pack_made_for(&g_pack, ""));
    TEST_ASSERT_FALSE(asset_pack_made_for(&g_pack, NULL));
    TEST_ASSERT_FALSE(asset_pack_made_for(NULL, "0.9.0"));
    asset_pack_t zero;
    memset(&zero, 0, sizeof zero);
    TEST_ASSERT_FALSE(asset_pack_made_for(&zero, "0.9.0"));
    TEST_ASSERT_FALSE(asset_pack_is_draft(NULL));
}

/* OPT-012: arguments: NULL, a misaligned payload, a refusal zeroes *out. */
static void test_arguments(void)
{
    const size_t n = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_ARG, asset_pack_open(NULL, n, &g_pack));
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_ARG, asset_pack_open(g_buf.b, n, NULL));
    memmove(g_buf.b + 1, g_buf.b, n);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_ARG, asset_pack_open(g_buf.b + 1, n, &g_pack));
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(fixture()));
    g_buf.b[0] = 'X';
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_MAGIC, open_n(n));
    TEST_ASSERT_NULL(g_pack.name);
    TEST_ASSERT_NULL(g_pack.payload);
    TEST_ASSERT_EQUAL_UINT(0u, g_pack.part_count);
#if SIZE_MAX > UINT32_MAX
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_ARG, asset_pack_open(g_buf.b, (size_t)UINT32_MAX + 1u, &g_pack));
#endif
}

/* OPT-012: every result has its own name. */
static void test_result_names(void)
{
    for (int a = ASSET_PACK_OK; a <= ASSET_PACK_ERR_PADDING; a++) {
        const char *na = asset_pack_result_name((asset_pack_result_t)a);
        TEST_ASSERT_NOT_NULL(na);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(na, "unknown"));
        for (int c = a + 1; c <= ASSET_PACK_ERR_PADDING; c++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(na, asset_pack_result_name((asset_pack_result_t)c)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("unknown", asset_pack_result_name((asset_pack_result_t)99));
    TEST_ASSERT_EQUAL_STRING("saver", asset_pack_store_name(PS_KIND_PICTURE));
    TEST_ASSERT_EQUAL_STRING("certs", asset_pack_store_name(PS_KIND_CERTS));
    TEST_ASSERT_EQUAL_STRING("web", asset_pack_store_name(PS_KIND_WEB));
    TEST_ASSERT_NULL(asset_pack_store_name(PS_KIND_LANGUAGE));
    TEST_ASSERT_NULL(asset_pack_store_name(PS_KIND_VOICE));
    TEST_ASSERT_NULL(asset_pack_store_name(7u));
    TEST_ASSERT_NULL(asset_pack_store_name(0u));
}

/* ------------------------------------------------------------------ truncations */

/* OPT-012: every truncation is refused, with the length field as it was and set to match. */
static void test_every_truncation_is_refused(void)
{
    const size_t full = fixture();
    for (size_t n = 0u; n < full; n++) {
        fixture();
        TEST_ASSERT_NOT_EQUAL(ASSET_PACK_OK, open_n(n));
        if (n >= 12u) {
            wr32(g_buf.b + 8, (uint32_t)n);
            const asset_pack_result_t r = open_n(n);
            TEST_ASSERT_NOT_EQUAL(ASSET_PACK_OK, r);
            if (n >= 128u) {
                TEST_ASSERT_NOT_EQUAL(ASSET_PACK_ERR_LENGTH, r);
            }
        }
    }
    fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_SHORT, open_n(127u));
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_SHORT, open_n(0u));
    /* the header is whole but the part table is not */
    fixture();
    wr32(g_buf.b + 8, 200u);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_SHORT, open_n(200u));
    /* one byte too many, with the field set to match: not where the layout ends */
    fixture();
    wr32(g_buf.b + 8, 245u);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_LAYOUT, open_n(245u));
    /* four zero bytes more: still not where the layout ends */
    fixture();
    wr32(g_buf.b + 8, 248u);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_LAYOUT, open_n(248u));
    /* the last part's padding left off */
    fixture();
    wr32(g_buf.b + 8, 243u);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_LAYOUT, open_n(243u));
}

/* ------------------------------------------------------------------ the header's fields */

/* OPT-012: magic, format, header size, payload length. */
static void test_header_magic_format_length(void)
{
    for (size_t i = 0u; i < 4u; i++) {
        EXPECT(ASSET_PACK_ERR_MAGIC, b[i] ^= 0x01u);
        EXPECT(ASSET_PACK_ERR_MAGIC, b[i] = 0u);
    }
    EXPECT(ASSET_PACK_ERR_MAGIC, memcpy(b, "RLNG", 4u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 4, 0u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 4, 2u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 4, 0x0101u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 6, 0u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 6, 127u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 6, 129u));
    EXPECT(ASSET_PACK_ERR_FORMAT, wr16(b + 6, 256u));
    static const uint32_t lens[] = {0u, 128u, 240u, 243u, 245u, 248u, 4096u, 0x7FFFFFFFu, 0xFFFFFFFFu};
    for (size_t i = 0u; i < sizeof lens / sizeof lens[0]; i++) {
        EXPECT(ASSET_PACK_ERR_LENGTH, wr32(b + 8, lens[i]));
    }
    EXPECT(ASSET_PACK_ERR_LENGTH, b[11] = 0x80u);
}

/* OPT-012: kinds 3-6 open, every other kind is refused; flag bit 0 is the draft, others refused. */
static void test_header_kind_and_flags(void)
{
    for (unsigned k = 0u; k < 256u; k++) {
        const asset_pack_result_t want = (k >= 3u && k <= 6u) ? ASSET_PACK_OK : ASSET_PACK_ERR_KIND;
        EXPECT(want, b[12] = (uint8_t)k);
        if (want == ASSET_PACK_OK) {
            TEST_ASSERT_EQUAL_UINT(k, g_pack.kind);
        }
    }
    EXPECT(ASSET_PACK_OK, b[13] = 0x01u);
    TEST_ASSERT_TRUE(g_pack.draft);
    TEST_ASSERT_TRUE(asset_pack_is_draft(&g_pack));
    TEST_ASSERT_EQUAL_UINT(1u, g_pack.flags);
    for (unsigned f = 2u; f < 256u; f++) {
        EXPECT(ASSET_PACK_ERR_FLAGS, b[13] = (uint8_t)f);
    }
}

/* OPT-012: part count 0, over 16, and a count that disagrees with the table. */
static void test_header_part_count(void)
{
    EXPECT(ASSET_PACK_ERR_COUNT, wr16(b + 14, 0u));
    EXPECT(ASSET_PACK_ERR_COUNT, wr16(b + 14, 17u));
    EXPECT(ASSET_PACK_ERR_COUNT, wr16(b + 14, 0x0103u));
    EXPECT(ASSET_PACK_ERR_COUNT, wr16(b + 14, 0xFFFFu));
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr16(b + 14, 2u)); /* the parts would start at 192 */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr16(b + 14, 1u));
    EXPECT(ASSET_PACK_ERR_SHORT, wr16(b + 14, 4u)); /* a table to 256 in 244 bytes */
    EXPECT(ASSET_PACK_ERR_SHORT, wr16(b + 14, 16u)); /* 640 > 244 */

    uint32_t sizes[17];
    for (size_t i = 0u; i < 17u; i++) {
        sizes[i] = (uint32_t)i + 1u;
    }
    size_t n = build(PS_KIND_LETTERS, 16u, sizes);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_UINT(16u, g_pack.part_count);
    TEST_ASSERT_NOT_NULL(asset_pack_part(&g_pack, 15u));
    TEST_ASSERT_EQUAL_UINT32(16u, asset_pack_part(&g_pack, 15u)->size);
    TEST_ASSERT_NULL(asset_pack_part(&g_pack, 16u));
    n = build(PS_KIND_LETTERS, 17u, sizes);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_ERR_COUNT, open_n(n));
    n = build(PS_KIND_WEB, 1u, sizes);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_UINT(1u, g_pack.part_count);
}

/* The name and version fields share these rules: refused with `code` at offset `at`. */
static void check_name_field(size_t at, asset_pack_result_t code)
{
    EXPECT(code, memset(b + at, 0, 16u));                     /* empty */
    EXPECT(code, memset(b + at, 'a', 16u));                   /* not NUL-terminated */
    EXPECT(code, b[at + 15u] = 'x');                          /* a stray byte after the NUL */
    EXPECT(code, b[at + 8u] = 0x01u);
    EXPECT(code, b[at + 1u] = ' ');                           /* a space */
    EXPECT(code, b[at] = ' ');
    EXPECT(code, b[at + 1u] = 0x1Fu);                         /* not printable */
    EXPECT(code, b[at + 1u] = 0x7Fu);
    EXPECT(code, b[at + 1u] = 0x80u);
    EXPECT(code, b[at + 1u] = 0xC3u);
    EXPECT(code, b[at + 1u] = 0xFFu);
    EXPECT(ASSET_PACK_OK, memset(b + at, '~', 15u));          /* 15 characters */
    EXPECT(ASSET_PACK_OK, memset(b + at, 0, 16u); b[at] = '!'); /* 1 character */
}

/* OPT-012: the pack's name and the version: empty, unterminated, not printable, with a space. */
static void test_header_name_and_version(void)
{
    check_name_field(16u, ASSET_PACK_ERR_NAME);
    check_name_field(32u, ASSET_PACK_ERR_VERSION);
    EXPECT(ASSET_PACK_OK, memset(b + 16, 0, 16u); memcpy(b + 16, "saver", 5u); memset(b + 32, 0, 16u); memcpy(b + 32, "1.2.3-dev+g1", 12u));
    TEST_ASSERT_EQUAL_STRING("saver", g_pack.name);
    TEST_ASSERT_EQUAL_STRING("1.2.3-dev+g1", g_pack.version);
}

/* OPT-012: match is carried as it is; parts offset 128 only; every reserved byte 0. */
static void test_header_match_parts_offset_reserved(void)
{
    EXPECT(ASSET_PACK_OK, wr32(b + 48, 0u));
    TEST_ASSERT_EQUAL_HEX32(0u, g_pack.match);
    EXPECT(ASSET_PACK_OK, wr32(b + 48, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, g_pack.match);
    static const uint32_t offs[] = {0u, 4u, 127u, 129u, 132u, 160u, 224u, 0x80000080u, 0xFFFFFFFFu};
    for (size_t i = 0u; i < sizeof offs / sizeof offs[0]; i++) {
        EXPECT(ASSET_PACK_ERR_PARTS_OFFSET, wr32(b + 52, offs[i]));
    }
    for (size_t i = 56u; i < 128u; i++) {
        EXPECT(ASSET_PACK_ERR_RESERVED, b[i] = 0x01u);
        EXPECT(ASSET_PACK_ERR_RESERVED, b[i] = 0xFFu);
    }
}

/* ------------------------------------------------------------------ the part records */

/* OPT-012: every field of every part record. */
static void test_part_fields(void)
{
    for (size_t i = 0u; i < 3u; i++) {
        const size_t r = 128u + 32u * i;
        check_name_field(r, ASSET_PACK_ERR_PART_NAME);
        /* type: 0 and unknown refused, the three known open */
        EXPECT(ASSET_PACK_ERR_PART_TYPE, wr32(b + r + 16u, 0u));
        EXPECT(ASSET_PACK_ERR_PART_TYPE, wr32(b + r + 16u, 4u));
        EXPECT(ASSET_PACK_ERR_PART_TYPE, wr32(b + r + 16u, 0x101u));
        EXPECT(ASSET_PACK_ERR_PART_TYPE, wr32(b + r + 16u, 0xFFFFFFFFu));
        EXPECT(ASSET_PACK_OK, wr32(b + r + 16u, ASSET_PART_JPEG));
        TEST_ASSERT_EQUAL_UINT32(ASSET_PART_JPEG, g_pack.parts[i].type);
        EXPECT(ASSET_PACK_OK, wr32(b + r + 16u, ASSET_PART_CERTS));
        /* offset: anything but the canonical place */
        static const int32_t deltas[] = {-8, -4, -3, -1, 1, 2, 4, 8};
        for (size_t d = 0u; d < sizeof deltas / sizeof deltas[0]; d++) {
            EXPECT(ASSET_PACK_ERR_LAYOUT,
                   wr32(b + r + 20u, (uint32_t)((int32_t)(224u + 8u * i) + deltas[d])));
        }
        EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + r + 20u, 0u));
        EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + r + 20u, 128u));
        EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + r + 20u, 0xFFFFFFFCu));
        /* size: 0 refused; one that moves the next part or runs past the end */
        EXPECT(ASSET_PACK_ERR_PART_SIZE, wr32(b + r + 24u, 0u));
        EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + r + 24u, 9u));
        EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + r + 24u, 0xFFFFFFFFu)); /* overflows 32 bits */
        EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + r + 24u, 0xFFFFFF10u));
        /* reserved */
        for (size_t k = 28u; k < 32u; k++) {
            EXPECT(ASSET_PACK_ERR_RESERVED, b[r + k] = 0x01u);
        }
    }
    /* sizes that keep the layout canonical (the zero bytes become the part's own) */
    EXPECT(ASSET_PACK_OK, wr32(b + 128u + 24u, 8u));
    TEST_ASSERT_EQUAL_UINT32(8u, g_pack.parts[0].size);
    EXPECT(ASSET_PACK_OK, wr32(b + 192u + 24u, 4u));
    /* a size that shortens a part out of its place */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + 128u + 24u, 4u));
    /* a size that leaves part bytes as alignment bytes (0xF2 after a title of 2) */
    EXPECT(ASSET_PACK_ERR_PADDING, wr32(b + 192u + 24u, 2u));
    /* repeated names */
    EXPECT(ASSET_PACK_ERR_PART_NAME, memcpy(b + 192u, "caption", 8u));
    EXPECT(ASSET_PACK_ERR_PART_NAME, memset(b + 160u, 0, 16u); memcpy(b + 160u, "caption", 7u));
    EXPECT(ASSET_PACK_OK, memcpy(b + 192u, "Title", 5u)); /* names are case-sensitive */
}

/* ------------------------------------------------------------------ the layout */

/* OPT-012: gaps, overlaps, misplaced parts and the alignment bytes. */
static void test_layout(void)
{
    /* a gap of 4 before a part, the payload grown to match */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + 160u + 20u, 236u); wr32(b + 192u + 20u, 244u);
           memmove(b + 236u, b + 232u, 12u); wr32(b + 8, 248u); n_ = 248u);
    /* an overlap: body starts inside caption */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + 160u + 20u, 228u));
    /* two parts swapped in place but not in the table */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + 128u + 20u, 232u); wr32(b + 160u + 20u, 224u));
    /* a part placed over the part table */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + 128u + 20u, 192u));
    /* a part placed over the header */
    EXPECT(ASSET_PACK_ERR_LAYOUT, wr32(b + 128u + 20u, 0u); wr32(b + 128u + 24u, 5u));
    /* alignment bytes after a part and after the last */
    for (size_t i = 229u; i < 232u; i++) {
        EXPECT(ASSET_PACK_ERR_PADDING, b[i] = 0x01u);
        EXPECT(ASSET_PACK_ERR_PADDING, b[i] = 0xFFu);
    }
    EXPECT(ASSET_PACK_ERR_PADDING, b[243] = 0x01u);
    /* the part data itself is the caller's: any byte opens */
    for (size_t i = 224u; i < 243u; i++) {
        if (i < 229u || (i >= 232u && i < 243u)) {
            EXPECT(ASSET_PACK_OK, b[i] = (uint8_t)~b[i]);
        }
    }
    /* parts built here: sizes 1..4 around the alignment */
    static const uint32_t sizes[4] = {1u, 2u, 3u, 4u};
    size_t n = build(PS_KIND_PICTURE, 4u, sizes);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    for (uint16_t i = 0u; i < 4u; i++) {
        TEST_ASSERT_EQUAL_UINT(0u, (uintptr_t)asset_pack_part(&g_pack, i)->data & 3u);
    }
    TEST_ASSERT_EQUAL_UINT32(128u + 128u + 4u * 4u, g_pack.payload_length);
}

/* ------------------------------------------------------------------ the kinds' rules */

/* OPT-006: the letters: its kind and name, the draft only on a release firmware, fonts only. */
static void test_kind_letters(void)
{
    const uint32_t sizes[2] = {5u, 7u};
    size_t n = build(PS_KIND_LETTERS, 2u, sizes);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_OK, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, "letters", true));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_OK, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, NULL, false));
    /* the store entry is another kind's, or has another name */
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_KIND, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NAME, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, "saver", false));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_KIND, asset_pack_check_kind(NULL, PS_KIND_LETTERS, NULL, false));

    /* the pack names itself after another kind */
    n = build(PS_KIND_LETTERS, 2u, sizes);
    set_name("saver");
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NAME, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, NULL, false));

    /* a draft: refused by a release firmware only */
    n = build(PS_KIND_LETTERS, 2u, sizes);
    g_buf.b[13] = ASSET_PACK_FLAG_DRAFT;
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_OK, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, NULL, false));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_DRAFT, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, NULL, true));

    /* a part that is not a font; a role of any name is fine */
    n = build(PS_KIND_LETTERS, 2u, sizes);
    set_part(0u, "future", ASSET_PART_FONT);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_OK, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, NULL, true));
    set_part(1u, "body", ASSET_PART_JPEG);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_FONTS, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, NULL, false));

    /* the web pages open as a pack but are not taken yet */
    n = build(PS_KIND_WEB, 1u, sizes);
    set_name("web");
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_TAKEN, asset_pack_check_kind(&g_pack, PS_KIND_WEB, NULL, false));
}

/* OPT-006: the picture: one `photo` part, a baseline JPEG of at most 1024 x 600, no match. */
static void test_kind_picture(void)
{
    size_t n = picture(0xC0u, 600u, 1024u);
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_OK, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, "saver", true));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_KIND, asset_pack_check_kind(&g_pack, PS_KIND_LETTERS, "letters", true));

    /* too wide, too high, progressive */
    (void)picture(0xC0u, 600u, 1025u);
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_PHOTO_SIZE, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));
    (void)picture(0xC0u, 601u, 1024u);
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_PHOTO_SIZE, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));
    (void)picture(0xC2u, 600u, 1024u);
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_PHOTO_SIZE, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));

    /* another part name, another type, a match */
    n = picture(0xC0u, 480u, 800u);
    set_part(0u, "image", ASSET_PART_JPEG);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_PHOTO, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));
    set_part(0u, "photo", ASSET_PART_FONT);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_PHOTO, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));
    set_part(0u, "photo", ASSET_PART_JPEG);
    wr32(g_buf.b + 48, 1u);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_PHOTO, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));

    /* a second part beside the photo */
    uint8_t jp[32];
    const uint32_t sizes[2] = {(uint32_t)jpeg(jp, 0xC0u, 600u, 1024u), 4u};
    n = build(PS_KIND_PICTURE, 2u, sizes);
    set_name("saver");
    set_part(0u, "photo", ASSET_PART_JPEG);
    memcpy(part_data(0u), jp, sizes[0]);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_PHOTO, asset_pack_check_kind(&g_pack, PS_KIND_PICTURE, NULL, false));
}

/* OPT-006: the certificates: one `bundle` part, no match; the version is the loader's rule. */
static void test_kind_certs(void)
{
    const uint32_t sizes[1] = {12u};
    const size_t n = build(PS_KIND_CERTS, 1u, sizes);
    set_name("certs");
    set_part(0u, "bundle", ASSET_PART_CERTS);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_OK, asset_pack_check_kind(&g_pack, PS_KIND_CERTS, "certs", true));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NAME, asset_pack_check_kind(&g_pack, PS_KIND_CERTS, "letters", true));
    set_part(0u, "list", ASSET_PART_CERTS);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_BUNDLE, asset_pack_check_kind(&g_pack, PS_KIND_CERTS, NULL, false));
    set_part(0u, "bundle", ASSET_PART_FONT);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_BUNDLE, asset_pack_check_kind(&g_pack, PS_KIND_CERTS, NULL, false));
    set_part(0u, "bundle", ASSET_PART_CERTS);
    wr32(g_buf.b + 48, 7u);
    TEST_ASSERT_EQUAL_INT(ASSET_PACK_OK, open_n(n));
    TEST_ASSERT_EQUAL_INT(ASSET_RULE_NOT_BUNDLE, asset_pack_check_kind(&g_pack, PS_KIND_CERTS, NULL, false));
}

/* OPT-006: the JPEG check: a baseline frame header within the bounds, every length bounded, every
 * truncation refused. */
static void test_jpeg_fits(void)
{
    uint8_t d[32];
    size_t n = jpeg(d, 0xC0u, 600u, 1024u);
    TEST_ASSERT_TRUE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1023u, 600u));
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 599u));
    for (size_t cut = 0u; cut < n; cut++) {
        TEST_ASSERT_FALSE_MESSAGE(asset_jpeg_fits(d, (uint32_t)cut, 1024u, 600u), "a truncated JPEG");
    }
    TEST_ASSERT_FALSE(asset_jpeg_fits(NULL, 27u, 1024u, 600u));

    /* no width or height */
    n = jpeg(d, 0xC0u, 0u, 1024u);
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));
    n = jpeg(d, 0xC0u, 600u, 0u);
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));

    /* other frame types, and the scan or the end before any frame */
    static const uint32_t other[] = {0xC1u, 0xC2u, 0xC3u, 0xC5u, 0xC9u, 0xCAu, 0xCFu, 0xDAu, 0xD9u};
    for (size_t i = 0u; i < sizeof other / sizeof other[0]; i++) {
        n = jpeg(d, other[i], 600u, 1024u);
        TEST_ASSERT_FALSE_MESSAGE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u), "not a baseline frame");
    }

    /* fill bytes, a restart and a TEM marker are passed over */
    static const uint8_t filled[] = {0xFF, 0xD8, 0xFF, 0xFF, 0xD0, 0xFF, 0x01, 0xFF, 0xC0, 0x00,
                                     0x0B, 8,    0x02, 0x58, 0x04, 0x00, 1,    1,    0x11, 0};
    TEST_ASSERT_TRUE(asset_jpeg_fits(filled, (uint32_t)sizeof filled, 1024u, 600u));

    /* no SOI; a byte where a marker belongs; a length under 2; a frame header under 8 bytes */
    n = jpeg(d, 0xC0u, 600u, 1024u);
    d[1] = 0xD9u;
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));
    n = jpeg(d, 0xC0u, 600u, 1024u);
    d[8] = 0x00u;
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));
    n = jpeg(d, 0xC0u, 600u, 1024u);
    d[5] = 0x01u;
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));
    n = jpeg(d, 0xC0u, 600u, 1024u);
    d[17] = 0x07u;
    TEST_ASSERT_FALSE(asset_jpeg_fits(d, (uint32_t)n, 1024u, 600u));
}

/* OPT-006: every rule has its own reason; a pass has none. */
static void test_rule_reasons(void)
{
    TEST_ASSERT_EQUAL_STRING("", asset_rule_reason(ASSET_RULE_OK));
    for (int a = (int)ASSET_RULE_KIND; a <= (int)ASSET_RULE_NOT_BUNDLE; a++) {
        TEST_ASSERT_TRUE(asset_rule_reason((asset_rule_t)a)[0] != '\0');
        for (int b = a + 1; b <= (int)ASSET_RULE_NOT_BUNDLE; b++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(asset_rule_reason((asset_rule_t)a), asset_rule_reason((asset_rule_t)b)));
        }
    }
    TEST_ASSERT_NOT_NULL(asset_rule_reason((asset_rule_t)99));
}

int main(void);
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixture_opens_with_its_fields);
    RUN_TEST(test_made_for_is_exact);
    RUN_TEST(test_arguments);
    RUN_TEST(test_result_names);
    RUN_TEST(test_every_truncation_is_refused);
    RUN_TEST(test_header_magic_format_length);
    RUN_TEST(test_header_kind_and_flags);
    RUN_TEST(test_header_part_count);
    RUN_TEST(test_header_name_and_version);
    RUN_TEST(test_header_match_parts_offset_reserved);
    RUN_TEST(test_part_fields);
    RUN_TEST(test_layout);
    RUN_TEST(test_kind_letters);
    RUN_TEST(test_kind_picture);
    RUN_TEST(test_kind_certs);
    RUN_TEST(test_jpeg_fits);
    RUN_TEST(test_rule_reasons);
    return UNITY_END();
}
