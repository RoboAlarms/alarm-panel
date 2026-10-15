/*
 * test_i18n.c - lookup, plurals, the {n} formatter and the UTF-8 helpers (components/core/i18n).
 * Requirements: I18N-001..005, I18N-008, I18N-010, I18N-037, I18N-039 (docs/features/37-Globalization)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "i18n/i18n.h"
#include "unity.h"

/* The catalog table as a release build compiles it (I18N_NO_DRAFTS, cmake/alarm_firmware.cmake),
 * renamed so it sits beside the development table these tests link (I18N-005). */
#define I18N_NO_DRAFTS
#define i18n_builtin       release_builtin
#define i18n_builtin_count release_builtin_count
#include "../catalogs/i18n_catalogs_gen.c"
#undef i18n_builtin
#undef i18n_builtin_count
#undef I18N_NO_DRAFTS

/* The same table with I18N_NO_BUILTIN: empty (I18N-039). */
#define I18N_NO_BUILTIN
#define i18n_builtin       nobuiltin_builtin
#define i18n_builtin_count nobuiltin_builtin_count
#include "../catalogs/i18n_catalogs_gen.c"
#undef i18n_builtin
#undef i18n_builtin_count
#undef I18N_NO_BUILTIN

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

#define E_ACUTE "\xC3\xA9"
#define EURO "\xE2\x82\xAC"
#define SMILE "\xF0\x9F\x98\x80"

/* ---- a small catalog built in the test ---- */

typedef struct {
    i18n_entry_t entries[8];
    char strings[512];
    size_t used;
    uint32_t count;
    i18n_catalog_t cat;
} tcat_t;

static void tcat_init(tcat_t *t, const char *locale, i18n_plural_rule_t rule)
{
    memset(t, 0, sizeof(*t));
    t->cat.locale = locale;
    t->cat.plural_rule = (uint8_t)rule;
    t->cat.entries = t->entries;
    t->cat.strings = t->strings;
}

/* forms: `n` strings stored back to back, each NUL-terminated; entries kept sorted by hash. */
static void tcat_add(tcat_t *t, const char *ctx, const char *english, const char *const *forms, size_t n)
{
    i18n_entry_t e;
    e.hash = i18n_hash(ctx, english);
    e.text = (uint32_t)t->used;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(forms[i]) + 1u;
        memcpy(t->strings + t->used, forms[i], l);
        t->used += l;
    }
    size_t at = t->count;
    while (at > 0 && t->entries[at - 1u].hash > e.hash) {
        t->entries[at] = t->entries[at - 1u];
        at--;
    }
    t->entries[at] = e;
    t->count++;
    t->cat.count = t->count;
}

static void tcat_one(tcat_t *t, const char *ctx, const char *english, const char *text)
{
    tcat_add(t, ctx, english, &text, 1u);
}

static void tcat_flag(tcat_t *t, const char *ctx, const char *english, uint32_t flags)
{
    const uint32_t h = i18n_hash(ctx, english);
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->entries[i].hash == h) {
            t->entries[i].text |= flags;
        }
    }
}

/* The whole document, read in pieces of at most len bytes. */
static size_t web_json_all(const i18n_catalog_t *c, size_t len, char *doc, size_t doc_len)
{
    char piece[1024];
    uint32_t pos = 0u;
    size_t used = 0u;
    size_t n;
    TEST_ASSERT_TRUE(len <= sizeof(piece));
    while ((n = i18n_web_json(c, &pos, piece, len)) > 0u) {
        TEST_ASSERT_TRUE(n < len);
        TEST_ASSERT_EQUAL_size_t(n, strlen(piece));
        TEST_ASSERT_TRUE(used + n < doc_len);
        memcpy(doc + used, piece, n);
        used += n;
    }
    doc[used] = '\0';
    return used;
}

/* ---- the hash ---- */

/* I18N-001 */
void test_hash_vectors(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x811c9dc5u, i18n_hash(NULL, ""));
    TEST_ASSERT_EQUAL_HEX32(0xe40c292cu, i18n_hash(NULL, "a"));
    TEST_ASSERT_EQUAL_HEX32(0xbf9cf968u, i18n_hash(NULL, "foobar"));
    TEST_ASSERT_EQUAL_HEX32(0xe40c292cu, i18n_hash("", "a"));
    /* context: bytes of ctx, one 0x04, then the English */
    TEST_ASSERT_EQUAL_HEX32(i18n_hash(NULL, "verb\x04Open"), i18n_hash("verb", "Open"));
    TEST_ASSERT_NOT_EQUAL(i18n_hash("verb", "Open"), i18n_hash("state", "Open"));
    TEST_ASSERT_NOT_EQUAL(i18n_hash(NULL, "Open"), i18n_hash("verb", "Open"));
}

/* ---- lookup ---- */

/* I18N-001. No allocation (I18N-037): the API has no allocator and the component links no malloc. */
void test_lookup_and_fallback(void)
{
    tcat_t t;
    tcat_init(&t, "fr", I18N_PLURALS_FR);
    tcat_one(&t, NULL, "Armed", "Arm" E_ACUTE);
    tcat_one(&t, NULL, "Disarmed", "D" E_ACUTE "sarm" E_ACUTE);

    TEST_ASSERT_EQUAL_STRING("Arm" E_ACUTE, i18n_text(&t.cat, NULL, "Armed"));
    TEST_ASSERT_EQUAL_STRING("D" E_ACUTE "sarm" E_ACUTE, i18n_text(&t.cat, "", "Disarmed"));
    /* not in the catalog: the English itself (same pointer) */
    const char *english = "Not translated";
    TEST_ASSERT_TRUE(i18n_text(&t.cat, NULL, english) == english);
    /* English has no table */
    TEST_ASSERT_TRUE(i18n_text(NULL, NULL, english) == english);
    /* a context that the catalog lacks does not match the context-free entry */
    TEST_ASSERT_EQUAL_STRING("Armed", i18n_text(&t.cat, "other", "Armed"));
}

#define BIG_N 100u

typedef struct {
    i18n_entry_t entries[BIG_N];
    char strings[BIG_N * 8u];
    char keys[BIG_N][8];
} big_t;

/* I18N-001, I18N-037: a binary search over 100 entries finds each one and misses cleanly below
 * the first hash, above the last and between two (the lookup allocates nothing: no allocator). */
void test_lookup_hundred_entries(void)
{
    static big_t b;
    memset(&b, 0, sizeof(b));
    for (uint32_t i = 0; i < BIG_N; i++) {
        snprintf(b.keys[i], sizeof(b.keys[i]), "key%u", (unsigned)i);
        uint32_t at = i;
        uint32_t h = i18n_hash(NULL, b.keys[i]);
        snprintf(b.strings + i * 8u, 8u, "T%u", (unsigned)i);
        i18n_entry_t e = {h, i * 8u};
        while (at > 0u && b.entries[at - 1u].hash > h) {
            b.entries[at] = b.entries[at - 1u];
            at--;
        }
        b.entries[at] = e;
    }
    for (uint32_t i = 1; i < BIG_N; i++) {
        TEST_ASSERT_TRUE(b.entries[i - 1u].hash < b.entries[i].hash); /* no duplicates */
    }
    i18n_catalog_t cat = {"xx", (uint8_t)I18N_PLURALS_FR, 0, BIG_N, b.entries, b.strings};
    for (uint32_t i = 0; i < BIG_N; i++) {
        char want[8];
        snprintf(want, sizeof(want), "T%u", (unsigned)i);
        TEST_ASSERT_EQUAL_STRING(want, i18n_text(&cat, NULL, b.keys[i]));
    }
    /* find absent keys: below the first hash, above the last, and inside a gap */
    const uint32_t lo = b.entries[0].hash;
    const uint32_t hi = b.entries[BIG_N - 1u].hash;
    char below[16] = "";
    char above[16] = "";
    char between[16] = "";
    for (unsigned j = 0; j < 100000u; j++) {
        char m[16];
        snprintf(m, sizeof(m), "miss%u", j);
        uint32_t h = i18n_hash(NULL, m);
        if (h < lo && below[0] == '\0') {
            strcpy(below, m);
        } else if (h > hi && above[0] == '\0') {
            strcpy(above, m);
        } else if (h > lo && h < hi && between[0] == '\0') {
            strcpy(between, m);
        }
    }
    TEST_ASSERT_TRUE(below[0] != '\0' && above[0] != '\0' && between[0] != '\0');
    const char *keys3[3] = {below, above, between};
    for (int k = 0; k < 3; k++) {
        TEST_ASSERT_TRUE(i18n_text(&cat, NULL, keys3[k]) == keys3[k]); /* the English itself */
    }
}

/* I18N-031: the Panel Manager's entries as JSON - only those flagged for it, escaped, a plural
 * entry's forms as an array - the same document whatever size of pieces it is written in; the
 * lookup ignores the flags. */
void test_web_json(void)
{
    tcat_t t;
    tcat_init(&t, "fr", I18N_PLURALS_FR);
    tcat_one(&t, NULL, "Armed", "Arm" E_ACUTE);            /* the screen's only */
    tcat_one(&t, NULL, "Say \"hi\"", "Dis \"salut\"\\\n"); /* the page's: escaped */
    const char *forms[3] = {"{0} zone", "{0} de zones", "{0} zones"};
    tcat_add(&t, "count", "{0} zone", forms, 3u);
    tcat_flag(&t, NULL, "Say \"hi\"", I18N_ENTRY_WEB);
    tcat_flag(&t, "count", "{0} zone", I18N_ENTRY_WEB | I18N_ENTRY_PLURAL);

    TEST_ASSERT_EQUAL_STRING("Dis \"salut\"\\\n", i18n_text(&t.cat, NULL, "Say \"hi\""));
    TEST_ASSERT_EQUAL_STRING("{0} zones", i18n_plural(&t.cat, "count", "{0} zone", "{0} zones", 5u));
    TEST_ASSERT_EQUAL_STRING("{0} de zones",
                             i18n_plural(&t.cat, "count", "{0} zone", "{0} zones", 1000000u));

    char want[512];
    int n = snprintf(want, sizeof(want), "{\"locale\":\"fr\",\"plural\":[\"one\",\"many\",\"other\"],"
                                         "\"entries\":{");
    bool first = true;
    for (uint32_t i = 0; i < t.count; i++) {
        const uint32_t h = t.entries[i].hash;
        const char *value = NULL;
        if (h == i18n_hash(NULL, "Say \"hi\"")) {
            value = "\"Dis \\\"salut\\\"\\\\\\u000a\"";
        } else if (h == i18n_hash("count", "{0} zone")) {
            value = "[\"{0} zone\",\"{0} de zones\",\"{0} zones\"]";
        }
        if (value != NULL) {
            n += snprintf(want + n, sizeof(want) - (size_t)n, "%s\"%08x\":%s", first ? "" : ",",
                          (unsigned)h, value);
            first = false;
        }
    }
    snprintf(want + n, sizeof(want) - (size_t)n, "}}");

    char doc[1024];
    for (size_t len = 128u; len <= 1024u; len += 7u) {
        web_json_all(&t.cat, len, doc, sizeof(doc));
        TEST_ASSERT_EQUAL_STRING(want, doc);
    }
    TEST_ASSERT_TRUE(strstr(doc, "Arm" E_ACUTE) == NULL); /* not the page's */

    /* too short to start, NULLs: nothing */
    uint32_t pos = 0u;
    char tiny[16];
    TEST_ASSERT_EQUAL_size_t(0u, i18n_web_json(&t.cat, &pos, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_size_t(0u, i18n_web_json(NULL, &pos, doc, sizeof(doc)));
    TEST_ASSERT_EQUAL_size_t(0u, i18n_web_json(&t.cat, NULL, doc, sizeof(doc)));

    /* an entry longer than a piece by itself is left out; the document stays whole */
    tcat_t big;
    tcat_init(&big, "de", I18N_PLURALS_EN);
    char longer[200];
    memset(longer, 'x', sizeof(longer) - 1u);
    longer[sizeof(longer) - 1u] = '\0';
    tcat_one(&big, NULL, "Long", longer);
    tcat_one(&big, NULL, "Short", "Kurz");
    tcat_flag(&big, NULL, "Long", I18N_ENTRY_WEB);
    tcat_flag(&big, NULL, "Short", I18N_ENTRY_WEB);
    web_json_all(&big.cat, 128u, doc, sizeof(doc));
    char short_only[128];
    snprintf(short_only, sizeof(short_only),
             "{\"locale\":\"de\",\"plural\":[\"one\",\"other\"],\"entries\":{\"%08x\":\"Kurz\"}}",
             (unsigned)i18n_hash(NULL, "Short"));
    TEST_ASSERT_EQUAL_STRING(short_only, doc);
    web_json_all(&big.cat, 512u, doc, sizeof(doc));
    TEST_ASSERT_NOT_NULL(strstr(doc, longer));
}

/* I18N-002 */
void test_context_separates_meanings(void)
{
    tcat_t t;
    tcat_init(&t, "fr", I18N_PLURALS_FR);
    tcat_one(&t, "verb", "Open", "Ouvrir");
    tcat_one(&t, "state", "Open", "Ouvert");
    TEST_ASSERT_EQUAL_STRING("Ouvrir", i18n_text(&t.cat, "verb", "Open"));
    TEST_ASSERT_EQUAL_STRING("Ouvert", i18n_text(&t.cat, "state", "Open"));
    TEST_ASSERT_EQUAL_STRING("Open", i18n_text(&t.cat, NULL, "Open"));
}

/* I18N-001: the built-in table holds the committed catalogs (Spanish, es-US, since G4) and ends
 * in NULL; English and unknown are NULL. I18N-012: a language is named in itself. */
void test_builtin_catalogs(void)
{
    TEST_ASSERT_NULL(i18n_catalog("en-US"));
    TEST_ASSERT_NULL(i18n_catalog("xx"));
    TEST_ASSERT_NULL(i18n_catalog(NULL));
    const i18n_catalog_t *es = i18n_catalog("es-US");
    TEST_ASSERT_NOT_NULL(es);
    TEST_ASSERT_EQUAL_STRING("es-US", es->locale);
    TEST_ASSERT_EQUAL_STRING("Espa" "\xC3\xB1" "ol", i18n_language_name(es));
    TEST_ASSERT_EQUAL_STRING("Abierta", i18n_text(es, "zone state", "Open"));
    TEST_ASSERT_TRUE(i18n_catalog_count() >= 1u);
    TEST_ASSERT_NULL(i18n_catalog_at(i18n_catalog_count()));
}

/* I18N-005: a release build keeps every reviewed catalog and no draft; its table ends in NULL */
void test_release_leaves_out_drafts(void)
{
    size_t reviewed = 0u;
    for (size_t i = 0u; i < i18n_catalog_count(); i++) {
        reviewed += i18n_catalog_at(i)->preview ? 0u : 1u;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)reviewed, (uint32_t)release_builtin_count);
    for (size_t i = 0u; i < release_builtin_count; i++) {
        TEST_ASSERT_NOT_NULL(release_builtin[i]);
        TEST_ASSERT_EQUAL_UINT8(0, release_builtin[i]->preview);
    }
    TEST_ASSERT_NULL(release_builtin[release_builtin_count]);
}

/* I18N-039: a build with I18N_NO_BUILTIN has no built-in language. */
void test_no_builtin_table_is_empty(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)nobuiltin_builtin_count);
    TEST_ASSERT_NULL(nobuiltin_builtin[0]);
    TEST_ASSERT_TRUE(i18n_catalog_count() > 0u); /* the normal table is unaffected */
}

/* I18N-015: a stored language the firmware can't show is refused (the panel falls back to
 * English); English needs no catalog. */
void test_language_known(void)
{
    TEST_ASSERT_TRUE(i18n_language_known("en-US"));
    TEST_ASSERT_TRUE(i18n_language_known("EN_gb"));
    TEST_ASSERT_TRUE(i18n_language_known("en"));
    TEST_ASSERT_TRUE(i18n_language_known(""));
    TEST_ASSERT_TRUE(i18n_language_known(NULL));
    TEST_ASSERT_TRUE(i18n_language_known("es-US")); /* the committed catalog */
    TEST_ASSERT_FALSE(i18n_language_known("fr-CA")); /* no catalog for it */
    TEST_ASSERT_FALSE(i18n_language_known("eng"));
    TEST_ASSERT_FALSE(i18n_language_known("e"));
    /* I18N-012, I18N-038: each language in its own name; with British English built in, the
     * source is "English (US)" beside "English (UK)" */
    TEST_ASSERT_EQUAL_STRING("English (US)", i18n_language_name(NULL));
    const i18n_catalog_t *gb = i18n_catalog("en-GB");
    TEST_ASSERT_NOT_NULL(gb);
    TEST_ASSERT_EQUAL_STRING("English (UK)", i18n_language_name(gb));
    TEST_ASSERT_EQUAL_STRING("Alarm cancelled", i18n_text(gb, NULL, "Alarm canceled"));
    TEST_ASSERT_EQUAL_STRING("Arm Away", i18n_text(gb, NULL, "Arm Away")); /* the source's text */
}

/* ---- plurals ---- */

static void expect_range(i18n_plural_rule_t r, uint32_t from, uint32_t to, i18n_plural_cat_t cat)
{
    for (uint32_t n = from; n <= to; n++) {
        TEST_ASSERT_EQUAL_MESSAGE(cat, i18n_plural_category(r, n), "plural category");
    }
}

/* I18N-004 */
void test_plural_categories_per_rule(void)
{
    /* en */
    expect_range(I18N_PLURALS_EN, 1, 1, I18N_ONE);
    expect_range(I18N_PLURALS_EN, 0, 0, I18N_OTHER);
    expect_range(I18N_PLURALS_EN, 2, 16, I18N_OTHER);
    expect_range(I18N_PLURALS_EN, 100, 100, I18N_OTHER);
    expect_range(I18N_PLURALS_EN, 1000000, 1000000, I18N_OTHER);
    /* es, it */
    expect_range(I18N_PLURALS_ES, 1, 1, I18N_ONE);
    expect_range(I18N_PLURALS_ES, 0, 0, I18N_OTHER);
    expect_range(I18N_PLURALS_ES, 2, 16, I18N_OTHER);
    expect_range(I18N_PLURALS_ES, 100, 100, I18N_OTHER);
    expect_range(I18N_PLURALS_ES, 1000, 1000, I18N_OTHER);
    expect_range(I18N_PLURALS_ES, 1000000, 1000000, I18N_MANY);
    expect_range(I18N_PLURALS_ES, 2000000, 2000000, I18N_MANY);
    /* fr */
    expect_range(I18N_PLURALS_FR, 0, 1, I18N_ONE);
    expect_range(I18N_PLURALS_FR, 2, 17, I18N_OTHER);
    expect_range(I18N_PLURALS_FR, 100, 100, I18N_OTHER);
    expect_range(I18N_PLURALS_FR, 1000, 1000, I18N_OTHER);
    expect_range(I18N_PLURALS_FR, 1000000, 1000000, I18N_MANY);
    /* ar */
    expect_range(I18N_PLURALS_AR, 0, 0, I18N_ZERO);
    expect_range(I18N_PLURALS_AR, 1, 1, I18N_ONE);
    expect_range(I18N_PLURALS_AR, 2, 2, I18N_TWO);
    expect_range(I18N_PLURALS_AR, 3, 10, I18N_FEW);
    expect_range(I18N_PLURALS_AR, 103, 110, I18N_FEW);
    expect_range(I18N_PLURALS_AR, 1003, 1003, I18N_FEW);
    expect_range(I18N_PLURALS_AR, 11, 26, I18N_MANY);
    expect_range(I18N_PLURALS_AR, 111, 111, I18N_MANY);
    expect_range(I18N_PLURALS_AR, 1011, 1011, I18N_MANY);
    expect_range(I18N_PLURALS_AR, 100, 102, I18N_OTHER);
    expect_range(I18N_PLURALS_AR, 200, 202, I18N_OTHER);
    expect_range(I18N_PLURALS_AR, 600, 600, I18N_OTHER);
    expect_range(I18N_PLURALS_AR, 1000, 1000, I18N_OTHER);
    expect_range(I18N_PLURALS_AR, 100000, 100000, I18N_OTHER);
    /* he */
    expect_range(I18N_PLURALS_HE, 1, 1, I18N_ONE);
    expect_range(I18N_PLURALS_HE, 2, 2, I18N_TWO);
    expect_range(I18N_PLURALS_HE, 0, 0, I18N_OTHER);
    expect_range(I18N_PLURALS_HE, 3, 17, I18N_OTHER);
    expect_range(I18N_PLURALS_HE, 100, 100, I18N_OTHER);
    expect_range(I18N_PLURALS_HE, 1000000, 1000000, I18N_OTHER);
    /* zh, ja, ko */
    expect_range(I18N_PLURALS_OTHER, 0, 15, I18N_OTHER);
    expect_range(I18N_PLURALS_OTHER, 100, 100, I18N_OTHER);
    expect_range(I18N_PLURALS_OTHER, 1000000, 1000000, I18N_OTHER);
}

/* I18N-004 */
void test_plural_forms_order(void)
{
    i18n_plural_cat_t f[6];
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_plural_forms(I18N_PLURALS_EN, f));
    TEST_ASSERT_EQUAL(I18N_ONE, f[0]);
    TEST_ASSERT_EQUAL(I18N_OTHER, f[1]);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_plural_forms(I18N_PLURALS_OTHER, f));
    TEST_ASSERT_EQUAL(I18N_OTHER, f[0]);
    for (int r = I18N_PLURALS_ES; r <= I18N_PLURALS_FR; r++) {
        TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_plural_forms((i18n_plural_rule_t)r, f));
        TEST_ASSERT_EQUAL(I18N_ONE, f[0]);
        TEST_ASSERT_EQUAL(I18N_MANY, f[1]);
        TEST_ASSERT_EQUAL(I18N_OTHER, f[2]);
    }
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)i18n_plural_forms(I18N_PLURALS_AR, f));
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL(i, f[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_plural_forms(I18N_PLURALS_HE, f));
    TEST_ASSERT_EQUAL(I18N_ONE, f[0]);
    TEST_ASSERT_EQUAL(I18N_TWO, f[1]);
    TEST_ASSERT_EQUAL(I18N_OTHER, f[2]);
}

/* I18N-004 */
void test_plural_rule_by_locale(void)
{
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("en"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("en-US"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("de"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("nl"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("sv"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("da"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("nb"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("fi"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("el"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("xx-YY"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule(NULL));
    const char *other[] = {"zh", "ja", "ko", "vi", "th", "id", "zh-Hant", "JA"};
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++) {
        TEST_ASSERT_EQUAL(I18N_PLURALS_OTHER, i18n_plural_rule(other[i]));
    }
    TEST_ASSERT_EQUAL(I18N_PLURALS_ES, i18n_plural_rule("es"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_ES, i18n_plural_rule("es-MX"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_ES, i18n_plural_rule("it"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_ES, i18n_plural_rule("ca"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_ES, i18n_plural_rule("pt-PT"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_ES, i18n_plural_rule("pt-pt"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_FR, i18n_plural_rule("pt"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_FR, i18n_plural_rule("pt-BR"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_FR, i18n_plural_rule("fr"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_FR, i18n_plural_rule("FR-CA"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_AR, i18n_plural_rule("ar"));
    TEST_ASSERT_EQUAL(I18N_PLURALS_HE, i18n_plural_rule("he"));
    /* a longer subtag is not the language ("fra" is not "fr") */
    TEST_ASSERT_EQUAL(I18N_PLURALS_EN, i18n_plural_rule("fra"));
}

/* I18N-004 */
void test_plural_entry_selects_form(void)
{
    tcat_t fr;
    tcat_init(&fr, "fr", I18N_PLURALS_FR);
    const char *fr_forms[] = {"{0} zone", "{0} de zones", "{0} zones"};
    tcat_add(&fr, NULL, "{0} zone", fr_forms, 3u);
    TEST_ASSERT_EQUAL_STRING("{0} zone", i18n_plural(&fr.cat, NULL, "{0} zone", "{0} zones", 0));
    TEST_ASSERT_EQUAL_STRING("{0} zone", i18n_plural(&fr.cat, NULL, "{0} zone", "{0} zones", 1));
    TEST_ASSERT_EQUAL_STRING("{0} zones", i18n_plural(&fr.cat, NULL, "{0} zone", "{0} zones", 2));
    TEST_ASSERT_EQUAL_STRING("{0} de zones", i18n_plural(&fr.cat, NULL, "{0} zone", "{0} zones", 1000000));

    tcat_t ar;
    tcat_init(&ar, "ar", I18N_PLURALS_AR);
    const char *ar_forms[] = {"Z", "O", "T", "F", "M", "X"};
    tcat_add(&ar, "ctx", "{0} zone", ar_forms, 6u);
    const uint32_t ns[] = {0, 1, 2, 5, 15, 100};
    const char *want[] = {"Z", "O", "T", "F", "M", "X"};
    for (size_t i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], i18n_plural(&ar.cat, "ctx", "{0} zone", "{0} zones", ns[i]));
    }

    tcat_t zh;
    tcat_init(&zh, "zh", I18N_PLURALS_OTHER);
    const char *zh_forms[] = {"Q"};
    tcat_add(&zh, NULL, "{0} zone", zh_forms, 1u);
    TEST_ASSERT_EQUAL_STRING("Q", i18n_plural(&zh.cat, NULL, "{0} zone", "{0} zones", 1));
    TEST_ASSERT_EQUAL_STRING("Q", i18n_plural(&zh.cat, NULL, "{0} zone", "{0} zones", 7));

    /* English (no catalog) and a missing entry: n == 1 ? one : other */
    TEST_ASSERT_EQUAL_STRING("{0} zone", i18n_plural(NULL, NULL, "{0} zone", "{0} zones", 1));
    TEST_ASSERT_EQUAL_STRING("{0} zones", i18n_plural(NULL, NULL, "{0} zone", "{0} zones", 0));
    TEST_ASSERT_EQUAL_STRING("{0} door", i18n_plural(&fr.cat, NULL, "{0} door", "{0} doors", 1));
    TEST_ASSERT_EQUAL_STRING("{0} doors", i18n_plural(&fr.cat, NULL, "{0} door", "{0} doors", 3));
}

/* ---- the formatter ---- */

static char buf[64];

/* I18N-003, I18N-008 */
void test_format_placeholders(void)
{
    i18n_arg_t a[] = {i18n_arg_int(3), i18n_arg_str("Alex")};
    TEST_ASSERT_EQUAL_UINT32(16, (uint32_t)i18n_format(buf, sizeof(buf), "{1} has {0} zones", a, 2));
    TEST_ASSERT_EQUAL_STRING("Alex has 3 zones", buf);
    /* repeated and reordered */
    i18n_format(buf, sizeof(buf), "{0}{1}{0}-{1}", a, 2);
    TEST_ASSERT_EQUAL_STRING("3Alex3-Alex", buf);
    /* a missing argument stays as written */
    i18n_format(buf, sizeof(buf), "{0} {2} {9}", a, 2);
    TEST_ASSERT_EQUAL_STRING("3 {2} {9}", buf);
    /* braces */
    i18n_format(buf, sizeof(buf), "{{0}} {{ }} {0}", a, 2);
    TEST_ASSERT_EQUAL_STRING("{0} { } 3", buf);
    /* not a placeholder: copied as is */
    i18n_format(buf, sizeof(buf), "{ {a} {10} {0 } } {", a, 2);
    TEST_ASSERT_EQUAL_STRING("{ {a} {10} {0 } } {", buf);
    /* no arguments at all */
    i18n_format(buf, sizeof(buf), "x {0}", NULL, 0);
    TEST_ASSERT_EQUAL_STRING("x {0}", buf);
    /* a translation is never a printf format */
    i18n_format(buf, sizeof(buf), "%s %n %d 100% {0}", a, 2);
    TEST_ASSERT_EQUAL_STRING("%s %n %d 100% 3", buf);
    /* a NULL string prints as nothing */
    i18n_arg_t z[] = {i18n_arg_str(NULL)};
    i18n_format(buf, sizeof(buf), "[{0}]", z, 1);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
    /* a NULL pattern is empty */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_format(buf, sizeof(buf), NULL, a, 2));
}

/* I18N-003 */
void test_format_numbers(void)
{
    i18n_arg_t a[] = {i18n_arg_int(INT64_MIN), i18n_arg_uint(UINT64_MAX), i18n_arg_int(0), i18n_arg_int(-7)};
    i18n_format(buf, sizeof(buf), "{0} {1} {2} {3}", a, 4);
    TEST_ASSERT_EQUAL_STRING("-9223372036854775808 18446744073709551615 0 -7", buf);
}

/* I18N-003: the macro picks the kind by type */
void test_format_macro(void)
{
    char c = 'x';
    (void)c;
    size_t n = 12u;
    unsigned char u8 = 200u;
    signed char s8 = -5;
    long long ll = -1234567890123LL;
    const char *name = "Pat";
    I18N_FORMAT(buf, sizeof(buf), "{0}|{1}|{2}|{3}|{4}|{5}", 3, "Alex", n, u8, s8, ll);
    TEST_ASSERT_EQUAL_STRING("3|Alex|12|200|-5|-1234567890123", buf);
    I18N_FORMAT(buf, sizeof(buf), "Hi {0}", name);
    TEST_ASSERT_EQUAL_STRING("Hi Pat", buf);
    I18N_FORMAT(buf, sizeof(buf), "{0}", -1);
    TEST_ASSERT_EQUAL_STRING("-1", buf);
    I18N_FORMAT(buf, sizeof(buf), "{0}", 4000000000u);
    TEST_ASSERT_EQUAL_STRING("4000000000", buf);
    I18N_FORMAT(buf, sizeof(buf), "{9}{0}{1}{2}{3}{4}{5}{6}{7}{8}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    TEST_ASSERT_EQUAL_STRING("9012345678", buf);
}

/* I18N-010 */
void test_format_truncates_on_a_character(void)
{
    char b[8];
    i18n_arg_t none[1] = {i18n_arg_int(0)};
    /* 2-byte character */
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_format(b, 3, "a" E_ACUTE, none, 0));
    TEST_ASSERT_EQUAL_STRING("a", b);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_format(b, 4, "a" E_ACUTE, none, 0));
    TEST_ASSERT_EQUAL_STRING("a" E_ACUTE, b);
    /* 3-byte */
    for (size_t len = 3; len <= 4; len++) {
        TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_format(b, len, "ab" EURO, none, 0));
        TEST_ASSERT_EQUAL_STRING("ab", b);
    }
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_format(b, 5, "ab" EURO, none, 0));
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)i18n_format(b, 6, "ab" EURO, none, 0));
    TEST_ASSERT_EQUAL_STRING("ab" EURO, b);
    /* 4-byte */
    for (size_t len = 2; len <= 5; len++) {
        TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_format(b, len, "a" SMILE, none, 0));
        TEST_ASSERT_EQUAL_STRING("a", b);
    }
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)i18n_format(b, 6, "a" SMILE, none, 0));
    /* from an argument, and in the middle of a longer pattern */
    i18n_arg_t s[] = {i18n_arg_str("x" SMILE "y")};
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_format(b, 4, "{0}", s, 1));
    TEST_ASSERT_EQUAL_STRING("x", b);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_format(b, 4, "{0}zzzz", s, 1));
    /* len 1: empty; len 0: nothing written */
    b[0] = 'Q';
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_format(b, 1, "abc", none, 0));
    TEST_ASSERT_EQUAL_CHAR('\0', b[0]);
    b[0] = 'Q';
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_format(b, 0, "abc", none, 0));
    TEST_ASSERT_EQUAL_CHAR('Q', b[0]);
    /* a number that does not fit is cut, and nothing after it is written */
    i18n_arg_t big[] = {i18n_arg_int(123456)};
    i18n_format(b, 4, "{0}", big, 1);
    TEST_ASSERT_EQUAL_STRING("123", b);
    i18n_format(b, 5, "n{0}z", big, 1);
    TEST_ASSERT_EQUAL_STRING("n123", b);
}

/* I18N-010 */
void test_format_invalid_utf8_is_safe(void)
{
    char b[8];
    i18n_arg_t none[1] = {i18n_arg_int(0)};
    /* a truncated sequence at the end of the pattern is copied byte by byte, never past the NUL */
    size_t n = i18n_format(b, sizeof(b), "a\xE2\x82", none, 0);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)strlen(b));
    n = i18n_format(b, sizeof(b), "\xFF\xC0\x80z", none, 0);
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)n);
    /* a bad byte counts as one: it can be cut */
    n = i18n_format(b, 3, "\xFF\xFF\xFF", none, 0);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)n);
}

/* I18N-003, I18N-010 (BL-171): a sentence with a name keeps its own words whole; the name is
 * shortened with "…" on a whole character, the room shared by every {0} the pattern holds */
void test_format_fit_keeps_the_words(void)
{
#define ELLIPSIS "\xE2\x80\xA6"
#define N_TILDE  "\xC3\x91"
    char b[60];
    /* the name whole where it fits */
    TEST_ASSERT_EQUAL_UINT32(21, (uint32_t)i18n_format_fit(b, sizeof(b), "{0} didn't run", "Good Night"));
    TEST_ASSERT_EQUAL_STRING("Good Night didn't run", b);
    /* a long name: shortened, the outcome kept, the whole within len - 1 */
    i18n_format_fit(b, sizeof(b), "Couldn't confirm {0} turned off", "Living Room Lamp Over The Reading Chair By Door");
    TEST_ASSERT_EQUAL_STRING("Couldn't confirm Living Room Lamp Over The Re" ELLIPSIS " turned off", b);
    TEST_ASSERT_EQUAL_UINT32(59, (uint32_t)strlen(b));
    /* a cut after a space: the space goes */
    char s[20];
    i18n_format_fit(s, sizeof(s), "{0} ok", "abcdefghijkl nopqrstuv");
    TEST_ASSERT_EQUAL_STRING("abcdefghijkl" ELLIPSIS " ok", s);
    /* a translation that repeats the name: the room is shared, the words survive */
    char r[40];
    TEST_ASSERT_EQUAL_UINT32(39, (uint32_t)i18n_format_fit(r, sizeof(r), "{0}: {0} didn't run", "Good Night Scene Long"));
    TEST_ASSERT_EQUAL_STRING("Good Night" ELLIPSIS ": Good Night" ELLIPSIS " didn't run", r);
    /* a multibyte name is cut on a whole character */
    char m[26];
    i18n_format_fit(m, sizeof(m), "{0} didn't open",
                    N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE);
    TEST_ASSERT_EQUAL_STRING(N_TILDE N_TILDE N_TILDE N_TILDE N_TILDE ELLIPSIS " didn't open", m);
    TEST_ASSERT_TRUE(i18n_utf8_valid(m, strlen(m)));
    /* room for "…" alone; escaped braces aren't the name; no placeholder; NULL is "" */
    char t[5];
    i18n_format_fit(t, sizeof(t), "{0}!", "abcdef");
    TEST_ASSERT_EQUAL_STRING(ELLIPSIS "!", t);
    i18n_format_fit(b, sizeof(b), "{{0}} {0}", "Hall");
    TEST_ASSERT_EQUAL_STRING("{0} Hall", b);
    i18n_format_fit(b, sizeof(b), "Nothing needs you", "Hall");
    TEST_ASSERT_EQUAL_STRING("Nothing needs you", b);
    i18n_format_fit(b, sizeof(b), "[{0}]", NULL);
    TEST_ASSERT_EQUAL_STRING("[]", b);
    /* words that don't fit by themselves are cut as i18n_format cuts them; len 0 writes nothing */
    char w[8];
    i18n_format_fit(w, sizeof(w), "{0} didn't run", "Good Night");
    TEST_ASSERT_EQUAL_STRING(" didn't", w);
    w[0] = 'Q';
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_format_fit(w, 0, "{0}", "x"));
    TEST_ASSERT_EQUAL_CHAR('Q', w[0]);
#undef ELLIPSIS
#undef N_TILDE
}

/* ---- UTF-8 helpers ---- */

/* I18N-010 */
void test_utf8_fit(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_fit("", 5));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_utf8_fit("abc", 10));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_utf8_fit("abc", 2));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_fit("abc", 0));
    /* 2-byte */
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_utf8_fit("a" E_ACUTE "b", 2));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_utf8_fit("a" E_ACUTE "b", 3));
    /* 3-byte */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_fit(EURO, 1));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_fit(EURO, 2));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_utf8_fit(EURO, 3));
    /* 4-byte */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_fit(SMILE, 3));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)i18n_utf8_fit(SMILE, 4));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)i18n_utf8_fit(SMILE, 100));
    /* invalid: a bad byte is one, a truncated string ends at its NUL */
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_utf8_fit("\xE2\x82", 5));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_utf8_fit("\xFF" "a", 1));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_utf8_fit("\x80\x80\x80", 2));
}

/* I18N-010 */
void test_utf8_copy(void)
{
    char d[8];
    memset(d, 'Q', sizeof(d));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_copy(d, 0, "abc"));
    TEST_ASSERT_EQUAL_CHAR('Q', d[0]);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_copy(d, 1, "abc"));
    TEST_ASSERT_EQUAL_STRING("", d);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_utf8_copy(d, 8, "abc"));
    TEST_ASSERT_EQUAL_STRING("abc", d);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)i18n_utf8_copy(d, 3, "a" E_ACUTE));
    TEST_ASSERT_EQUAL_STRING("a", d);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_utf8_copy(d, 4, "ab" EURO));
    TEST_ASSERT_EQUAL_STRING("ab", d);
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)i18n_utf8_copy(d, 5, SMILE));
    TEST_ASSERT_EQUAL_STRING(SMILE, d);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_copy(d, 4, SMILE));
    TEST_ASSERT_EQUAL_STRING("", d);
}

/* I18N-010 */
void test_utf8_chars(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_chars(""));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_utf8_chars("abc"));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)i18n_utf8_chars("a" E_ACUTE EURO SMILE));
    /* each bad byte is a character; a truncated sequence never reads past the NUL */
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_utf8_chars("\xE2\x82"));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)i18n_utf8_chars("\xC0\x80"));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)i18n_utf8_chars("\xFF" "a" "\x80"));
}

/* I18N-010 */
void test_utf8_valid(void)
{
    TEST_ASSERT_TRUE(i18n_utf8_valid("", 0));
    TEST_ASSERT_TRUE(i18n_utf8_valid("abc", 3));
    TEST_ASSERT_TRUE(i18n_utf8_valid("a" E_ACUTE EURO SMILE, 10));
    TEST_ASSERT_TRUE(i18n_utf8_valid("\xC2\x80", 2));         /* U+0080 */
    TEST_ASSERT_TRUE(i18n_utf8_valid("\xED\x9F\xBF", 3));     /* U+D7FF */
    TEST_ASSERT_TRUE(i18n_utf8_valid("\xEE\x80\x80", 3));     /* U+E000 */
    TEST_ASSERT_TRUE(i18n_utf8_valid("\xF4\x8F\xBF\xBF", 4)); /* U+10FFFF */
    /* truncated by len */
    TEST_ASSERT_FALSE(i18n_utf8_valid(EURO, 2));
    TEST_ASSERT_FALSE(i18n_utf8_valid(SMILE, 3));
    TEST_ASSERT_FALSE(i18n_utf8_valid(E_ACUTE, 1));
    TEST_ASSERT_TRUE(i18n_utf8_valid(EURO, 3));
    /* stray continuation, bad lead, overlong, surrogate, above U+10FFFF */
    TEST_ASSERT_FALSE(i18n_utf8_valid("\x80", 1));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xFF", 1));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xC0\x80", 2));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xC1\xBF", 2));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xE0\x80\x80", 3));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xE0\x9F\xBF", 3));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xED\xA0\x80", 3));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xED\xBF\xBF", 3));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xF0\x80\x80\x80", 4));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xF0\x8F\xBF\xBF", 4));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xF4\x90\x80\x80", 4));
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xF5\x80\x80\x80", 4));
    /* a continuation byte cut short by the NUL of a string */
    TEST_ASSERT_FALSE(i18n_utf8_valid("\xE2\x82", 3));
}

/* I18N-010: a NULL string is "" */
void test_utf8_null_is_empty(void)
{
    char d[4] = {'x', 'x', 'x', 'x'};
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_fit(NULL, 5));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_chars(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_copy(d, sizeof(d), NULL));
    TEST_ASSERT_EQUAL_STRING("", d);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)i18n_utf8_copy(d, 0, NULL));
}

/* I18N-009: names and searches match whatever the case, in the scripts the fonts carry. */
static void test_utf8_fold(void)
{
    const char *kuche = "K\xC3\x9C" "CHE";         /* KÜCHE */
    const char *small = "k\xC3\xBC" "che";         /* küche */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold(kuche, strlen(kuche), small, strlen(small)));
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("Front Door", 10, "front door", 10));
    TEST_ASSERT_FALSE(i18n_utf8_equal_fold("Front Door", 10, "front doors", 11));
    TEST_ASSERT_FALSE(i18n_utf8_equal_fold(small, strlen(small), "kuche", 5)); /* ü is not u */
    /* Latin Extended-A pairs, both ways round, and the odd ones out. */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("\xC5\x81\xC3\x93" "D\xC5\xB9", 7, "\xC5\x82\xC3\xB3" "d\xC5\xBA", 7));
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("\xC4\xB0", 2, "i", 1));            /* İ */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("\xC5\xB8", 2, "\xC3\xBF", 2));    /* Ÿ ÿ */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("\xC4\xBF", 2, "\xC5\x80", 2));    /* Ŀ ŀ */
    TEST_ASSERT_FALSE(i18n_utf8_equal_fold("\xC3\x97", 2, "\xC3\xB7", 2));   /* × is not ÷ */
    /* Greek and Cyrillic. */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("\xCE\xA9", 2, "\xCF\x89", 2));    /* Ω ω */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("\xD0\x94\xD0\x9E\xD0\x9C", 6,
                                          "\xD0\xB4\xD0\xBE\xD0\xBC", 6)); /* ДОМ дом */
    /* The lengths bound the bytes compared; a NUL ends a string early. */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("ABCxyz", 3, "abc", 3));
    TEST_ASSERT_FALSE(i18n_utf8_equal_fold("AB", 5, "abc", 3));
    /* A broken byte matches only itself. */
    TEST_ASSERT_TRUE(i18n_utf8_equal_fold("a\xFF", 2, "A\xFF", 2));
    TEST_ASSERT_FALSE(i18n_utf8_equal_fold("a\xFF", 2, "A\xFE", 2));

    TEST_ASSERT_TRUE(i18n_utf8_contains_fold("Sal\xC3\xB3n principal", "SAL\xC3\x93N"));
    TEST_ASSERT_TRUE(i18n_utf8_contains_fold("binary_sensor.k\xC3\xBC" "che_door", "K\xC3\x9C" "CHE"));
    TEST_ASSERT_TRUE(i18n_utf8_contains_fold("anything", ""));
    TEST_ASSERT_TRUE(i18n_utf8_contains_fold("anything", NULL));
    TEST_ASSERT_FALSE(i18n_utf8_contains_fold("Salon", "sal\xC3\xB3n"));
    TEST_ASSERT_FALSE(i18n_utf8_contains_fold(NULL, "x"));
    TEST_ASSERT_FALSE(i18n_utf8_contains_fold("ab", "abc"));
}

int main(void);
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hash_vectors);
    RUN_TEST(test_lookup_and_fallback);
    RUN_TEST(test_lookup_hundred_entries);
    RUN_TEST(test_context_separates_meanings);
    RUN_TEST(test_web_json);
    RUN_TEST(test_builtin_catalogs);
    RUN_TEST(test_release_leaves_out_drafts);
    RUN_TEST(test_no_builtin_table_is_empty);
    RUN_TEST(test_language_known);
    RUN_TEST(test_plural_categories_per_rule);
    RUN_TEST(test_plural_forms_order);
    RUN_TEST(test_plural_rule_by_locale);
    RUN_TEST(test_plural_entry_selects_form);
    RUN_TEST(test_format_placeholders);
    RUN_TEST(test_format_numbers);
    RUN_TEST(test_format_macro);
    RUN_TEST(test_format_truncates_on_a_character);
    RUN_TEST(test_format_invalid_utf8_is_safe);
    RUN_TEST(test_format_fit_keeps_the_words);
    RUN_TEST(test_utf8_fit);
    RUN_TEST(test_utf8_copy);
    RUN_TEST(test_utf8_chars);
    RUN_TEST(test_utf8_valid);
    RUN_TEST(test_utf8_fold);
    RUN_TEST(test_utf8_null_is_empty);
    return UNITY_END();
}
