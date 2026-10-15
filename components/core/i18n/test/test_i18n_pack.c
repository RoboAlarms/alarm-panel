/*
 * test_i18n_pack.c - the language pack reader (components/core/i18n).
 * Requirements: I18N-040 (the strict reader), I18N-043 (the template fingerprint)
 * (docs/features/37-Globalization language-packs.md)
 */
#include <stdint.h>
#include <string.h>

#include "i18n/i18n.h"
#include "i18n/i18n_pack.h"
#include "pack_fixture_v1.h"
#include "unity.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

#define FIXTURE_LEN sizeof pack_fixture_v1

static uint32_t g_buf[1024]; /* aligned scratch */

static uint8_t *bytes(void)
{
    return (uint8_t *)g_buf;
}

static size_t load(void)
{
    memset(g_buf, 0, sizeof g_buf);
    memcpy(g_buf, pack_fixture_v1, FIXTURE_LEN);
    return FIXTURE_LEN;
}

static void put32(size_t off, uint32_t v)
{
    for (unsigned i = 0u; i < 4u; i++) {
        bytes()[off + i] = (uint8_t)(v >> (8u * i));
    }
}

static void put16(size_t off, unsigned v)
{
    bytes()[off] = (uint8_t)v;
    bytes()[off + 1u] = (uint8_t)(v >> 8);
}

static uint32_t get32_at(size_t off)
{
    return (uint32_t)bytes()[off] | ((uint32_t)bytes()[off + 1u] << 8) |
           ((uint32_t)bytes()[off + 2u] << 16) | ((uint32_t)bytes()[off + 3u] << 24);
}

static i18n_pack_result_t open_buf(size_t len)
{
    i18n_pack_t p;
    return i18n_pack_open(g_buf, len, &p);
}

#define EXPECT(res, len) TEST_ASSERT_EQUAL_INT((res), open_buf(len))

/* I18N-040: the fixture opens with the fields its comment lists. */
static void test_fixture_fields(void)
{
    size_t len = load();
    i18n_pack_t p;
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    TEST_ASSERT_EQUAL_STRING("fr-CA", p.locale);
    TEST_ASSERT_EQUAL_STRING("Fran\xC3\xA7" "ais", p.name);
    TEST_ASSERT_EQUAL_STRING("0.7.0", p.version);
    TEST_ASSERT_FALSE(p.draft);
    TEST_ASSERT_EQUAL_UINT32(0x7febd7c5u, p.template_fingerprint);
    TEST_ASSERT_EQUAL_UINT32(2u, p.missing);
    TEST_ASSERT_EQUAL_UINT32(234u, p.payload_length);
    TEST_ASSERT_EQUAL_UINT8(0u, p.font_count);
    TEST_ASSERT_EQUAL_UINT32(5u, p.catalog.count);
    TEST_ASSERT_EQUAL_UINT8(I18N_PLURALS_FR, p.catalog.plural_rule);
    TEST_ASSERT_EQUAL_STRING("fr-CA", p.catalog.locale);
}

/* I18N-040: its catalog answers lookups and plurals. */
static void test_fixture_catalog(void)
{
    size_t len = load();
    i18n_pack_t p;
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    const i18n_catalog_t *c = &p.catalog;
    TEST_ASSERT_EQUAL_STRING("Arm\xC3\xA9", i18n_text(c, NULL, "Armed"));
    TEST_ASSERT_EQUAL_STRING("Ouvert", i18n_text(c, "door", "Open"));
    TEST_ASSERT_EQUAL_STRING("R\xC3\xA9glages", i18n_text(c, NULL, "Settings"));
    TEST_ASSERT_EQUAL_STRING("Pouce \xF0\x9F\x91\x8D", i18n_text(c, NULL, "Thumbs up"));
    TEST_ASSERT_EQUAL_STRING("Disarmed", i18n_text(c, NULL, "Disarmed"));
    TEST_ASSERT_EQUAL_STRING("{0} zone", i18n_plural(c, NULL, "{0} zone", "{0} zones", 0u));
    TEST_ASSERT_EQUAL_STRING("{0} zone", i18n_plural(c, NULL, "{0} zone", "{0} zones", 1u));
    TEST_ASSERT_EQUAL_STRING("{0} zones", i18n_plural(c, NULL, "{0} zone", "{0} zones", 2u));
    TEST_ASSERT_EQUAL_STRING("{0} de zones",
                             i18n_plural(c, NULL, "{0} zone", "{0} zones", 1000000u));
}

/* I18N-043: the fingerprint says whether the pack was made for this firmware. */
static void test_matches(void)
{
    size_t len = load();
    i18n_pack_t p;
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    TEST_ASSERT_FALSE(i18n_pack_matches(&p));
    TEST_ASSERT_FALSE(i18n_pack_matches(NULL));
    put32(80, i18n_template_fingerprint);
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    TEST_ASSERT_TRUE(i18n_pack_matches(&p));
    TEST_ASSERT_EQUAL_UINT32(i18n_template_fingerprint, p.template_fingerprint);
}

/* I18N-040: the draft flag is read. */
static void test_draft(void)
{
    size_t len = load();
    i18n_pack_t p;
    bytes()[61] = 1u;
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    TEST_ASSERT_TRUE(p.draft);
    TEST_ASSERT_EQUAL_UINT8(1u, p.catalog.preview);
}

/* I18N-040: a payload cut at any length is refused. */
static void test_truncations(void)
{
    size_t len = load();
    for (size_t n = 0u; n < len; n++) {
        TEST_ASSERT_NOT_EQUAL_INT(I18N_PACK_OK, open_buf(n));
    }
    EXPECT(I18N_PACK_ERR_SHORT, 127u);
    EXPECT(I18N_PACK_ERR_HEADER, len - 1u);
    TEST_ASSERT_EQUAL_INT(I18N_PACK_ERR_HEADER, open_buf(len + 1u)); /* zeros after the end */
}

/* I18N-040: arguments. */
static void test_args(void)
{
    size_t len = load();
    i18n_pack_t p;
    TEST_ASSERT_EQUAL_INT(I18N_PACK_ERR_ARG, i18n_pack_open(NULL, len, &p));
    TEST_ASSERT_EQUAL_INT(I18N_PACK_ERR_ARG, i18n_pack_open(g_buf, len, NULL));
    memmove(bytes() + 1, bytes(), len);
    TEST_ASSERT_EQUAL_INT(I18N_PACK_ERR_ARG, i18n_pack_open(bytes() + 1, len, &p));
    TEST_ASSERT_EQUAL_INT(I18N_PACK_ERR_ARG, i18n_pack_open(bytes() + 2, len, &p));
}

/* I18N-040: each header field broken on its own. */
static void test_header_fields(void)
{
    size_t len = load();
    bytes()[0] = 'X';
    EXPECT(I18N_PACK_ERR_MAGIC, len);

    len = load();
    put16(4, 2u);
    EXPECT(I18N_PACK_ERR_FORMAT, len);
    len = load();
    put16(6, 127u);
    EXPECT(I18N_PACK_ERR_FORMAT, len);
    len = load();
    put16(6, 136u);
    EXPECT(I18N_PACK_ERR_FORMAT, len);

    len = load();
    put32(8, 235u);
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    put32(8, 233u);
    EXPECT(I18N_PACK_ERR_HEADER, len);

    /* locale */
    len = load();
    memset(bytes() + 12, 0, 16);
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    memset(bytes() + 12, 0, 16);
    bytes()[12] = 'f';
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[14] = '_';
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    memset(bytes() + 12, 'a', 16);
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[27] = 'x'; /* a stray byte after the NUL */
    EXPECT(I18N_PACK_ERR_HEADER, len);

    /* name */
    len = load();
    memset(bytes() + 28, 0, 32);
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    memset(bytes() + 28, 'a', 32);
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[28] = 0xFFu;
    EXPECT(I18N_PACK_ERR_HEADER, len);

    /* rule, flags, reserved */
    len = load();
    bytes()[60] = 6u;
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[61] = 0x02u;
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[61] = 0x81u;
    EXPECT(I18N_PACK_ERR_HEADER, len);
    static const size_t reserved[] = {62u, 63u, 112u, 119u, 127u};
    for (size_t i = 0u; i < sizeof reserved / sizeof reserved[0]; i++) {
        len = load();
        bytes()[reserved[i]] = 1u;
        EXPECT(I18N_PACK_ERR_HEADER, len);
    }

    /* version */
    len = load();
    memset(bytes() + 88, 0, 16);
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[88] = 0x01u;
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    bytes()[90] = 0xC3u;
    EXPECT(I18N_PACK_ERR_HEADER, len);
    len = load();
    memset(bytes() + 88, '1', 16);
    EXPECT(I18N_PACK_ERR_HEADER, len);
}

/* I18N-040: regions outside, overlapping, misaligned or overflowing. */
static void test_layout(void)
{
    size_t len = load();
    put32(68, 130u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(68, 64u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(68, 120u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(68, 236u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(68, 204u); /* the entries run into the strings */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(64, 6u); /* one entry too many: into the strings */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(64, 0x20000000u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(64, 0xFFFFFFFFu);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(76, 0xFFFFFFF0u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(76, 67u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(72, 64u);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(72, 160u); /* the strings start inside the entries */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(104, 200u); /* fonts offset without a size */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = load();
    put32(108, 8u); /* fonts size without an offset */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
}

/* I18N-040: entries. */
static void test_entries(void)
{
    uint8_t tmp[8];
    size_t len = load();
    memcpy(tmp, bytes() + 128, 8);
    memcpy(bytes() + 128, bytes() + 136, 8);
    memcpy(bytes() + 136, tmp, 8);
    EXPECT(I18N_PACK_ERR_ENTRIES, len);

    len = load();
    memcpy(bytes() + 136, bytes() + 128, 4); /* a duplicate hash */
    EXPECT(I18N_PACK_ERR_ENTRIES, len);

    len = load();
    put32(132, 66u); /* a text at the end of the block */
    EXPECT(I18N_PACK_ERR_ENTRIES, len);
    len = load();
    put32(132, 0xFFFFFFFFu);
    EXPECT(I18N_PACK_ERR_ENTRIES, len);

    len = load();
    bytes()[233] = 'x'; /* the last text loses its NUL */
    EXPECT(I18N_PACK_ERR_ENTRIES, len);

    len = load();
    bytes()[168] = 0xFFu; /* "Arme" starts with a bad byte */
    EXPECT(I18N_PACK_ERR_ENTRIES, len);
    len = load();
    bytes()[168 + 4] = 0xA9u; /* a continuation byte after "Arm\xC3" loses its lead */
    bytes()[168 + 3] = 'e';
    EXPECT(I18N_PACK_ERR_ENTRIES, len);

    /* the plural entry (third by hash) pointing at the last text: one form, two missing */
    len = load();
    put32(128 + 16 + 4, I18N_ENTRY_PLURAL | 0x37u);
    EXPECT(I18N_PACK_ERR_ENTRIES, len);
    /* a plural form with bad UTF-8 */
    len = load();
    bytes()[168 + 13 + 9] = 0xC0u;
    EXPECT(I18N_PACK_ERR_ENTRIES, len);
}

/* ---- fonts: a table built after the strings ---- */
#define FONTS_AT   236u
#define FONT_ROLES 9u

static const char *const good_roles[FONT_ROLES] = {"caption", "body", "label", "title",
                                                   "headline", "x1",  "x2",   "x3", "x4"};

/* n fonts (roles in order, 8 bytes of data each after the table); returns the payload length. */
static size_t build_fonts(unsigned n, const char *const *roles)
{
    size_t len = load();
    (void)len;
    size_t table = 4u + 24u * n;
    size_t data = FONTS_AT + table;
    memset(bytes() + 234, 0, sizeof g_buf - 234u);
    put32(FONTS_AT, n);
    for (unsigned i = 0u; i < n; i++) {
        size_t r = FONTS_AT + 4u + 24u * i;
        strncpy((char *)bytes() + r, roles[i], 15);
        put32(r + 16u, (uint32_t)(data + 8u * i));
        put32(r + 20u, 8u);
        memset(bytes() + data + 8u * i, (int)(0x10u + i), 8);
    }
    size_t total = data + 8u * n;
    put32(8, (uint32_t)total);
    put32(104, FONTS_AT);
    put32(108, (uint32_t)table);
    return total;
}

static size_t rec(unsigned i)
{
    return FONTS_AT + 4u + 24u * i;
}

/* I18N-040: a valid fonts table (one and five fonts). */
static void test_fonts_valid(void)
{
    static const char *const one[] = {"body"};
    size_t len = build_fonts(1u, one);
    i18n_pack_t p;
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    TEST_ASSERT_EQUAL_UINT8(1u, p.font_count);
    TEST_ASSERT_EQUAL_STRING("body", p.fonts[0].role);
    TEST_ASSERT_EQUAL_UINT32(8u, p.fonts[0].size);
    TEST_ASSERT_EQUAL_PTR(bytes() + FONTS_AT + 28u, p.fonts[0].data);
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));

    len = build_fonts(5u, good_roles);
    TEST_ASSERT_EQUAL_INT(I18N_PACK_OK, i18n_pack_open(g_buf, len, &p));
    TEST_ASSERT_EQUAL_UINT8(5u, p.font_count);
    TEST_ASSERT_EQUAL_STRING("headline", p.fonts[4].role);
    TEST_ASSERT_EQUAL_UINT8(0x14u, p.fonts[4].data[0]);
    /* the catalog still works beside the fonts */
    TEST_ASSERT_EQUAL_STRING("Ouvert", i18n_text(&p.catalog, "door", "Open"));
}

/* I18N-040: the fonts table, broken. Only five roles exist, so eight fonts
 * cannot be valid: the count limit is tested at 0 and 9 and the rest at 5. */
static void test_fonts_broken(void)
{
    size_t len = build_fonts(5u, good_roles);
    put32(FONTS_AT, 0u);
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(FONTS_AT, 9u);
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(FONTS_AT, 6u); /* more records than the region holds */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(108, 3u); /* a region too small for the count */
    EXPECT(I18N_PACK_ERR_FONTS, len);

    len = build_fonts(5u, good_roles);
    strcpy((char *)bytes() + rec(2), "huge");
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    strcpy((char *)bytes() + rec(3), "body"); /* a role twice */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    memset(bytes() + rec(1), 0, 16); /* an empty role */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    memset(bytes() + rec(1), 'b', 16); /* a role without its NUL */
    EXPECT(I18N_PACK_ERR_FONTS, len);

    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, (uint32_t)len - 4u); /* runs past the end */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, 0xFFFFFFFCu); /* and past 32 bits */
    put32(rec(1) + 20u, 8u);
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, 0xFFFFFFFCu);
    put32(rec(1) + 20u, 0xFFFFFFFFu);
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, (uint32_t)len - 7u); /* misaligned (and past the end) */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, get32_at(rec(1) + 16u) + 1u); /* misaligned, inside */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 20u, 0u); /* zero size */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, 168u); /* over the strings */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, 128u); /* over the entries */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, 0u); /* over the header */
    EXPECT(I18N_PACK_ERR_FONTS, len);
    len = build_fonts(5u, good_roles);
    put32(rec(1) + 16u, FONTS_AT + 8u); /* over the table */
    EXPECT(I18N_PACK_ERR_FONTS, len);
}

/* I18N-040: the fonts region itself must sit clear of the other regions. */
static void test_fonts_region(void)
{
    size_t len = build_fonts(5u, good_roles);
    put32(104, 200u); /* into the strings */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = build_fonts(5u, good_roles);
    put32(104, 64u); /* into the header */
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
    len = build_fonts(5u, good_roles);
    put32(108, 0xFFFFFFFFu);
    EXPECT(I18N_PACK_ERR_LAYOUT, len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixture_fields);
    RUN_TEST(test_fixture_catalog);
    RUN_TEST(test_matches);
    RUN_TEST(test_draft);
    RUN_TEST(test_truncations);
    RUN_TEST(test_args);
    RUN_TEST(test_header_fields);
    RUN_TEST(test_layout);
    RUN_TEST(test_entries);
    RUN_TEST(test_fonts_valid);
    RUN_TEST(test_fonts_broken);
    RUN_TEST(test_fonts_region);
    return UNITY_END();
}
