/*
 * test_vp_words.c - the spoken-name editor's portable logic (docs/features/08 voice-first-build.md
 * D03): "Use zone name" (vp_match_name) and the word lists (vp_list_words). AUD-026 (the pack's
 * vocabulary, read by display text and script key).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "voice_pack/vp_words.h"
#include "vp_test_pack.h"
#include "vp_vocab_en_us.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

static vp_pack_t pack;

/* A small pack: positions, rooms, objects and all the number entries. IDs ascend with the table. */
static const struct { uint8_t cat; const char *text; const char *key; } small[] = {
    {1, "Front", "word.front"},        {1, "Back", "word.back"},
    {2, "Living room", "word.living_room"}, {2, "Living", "word.living"},
    {2, "Room", "word.room"},          {2, "Garage", "word.garage"},
    {2, "Garden", "word.garden"},      {3, "Door", "word.door"},
    {3, "Motion", "word.motion"},      {3, "Smoke detector", "word.smoke_detector"},
    {3, "Smoke", "word.smoke"},        {3, "Floor", "word.floor"},
    {3, "Dining room", "word.dining_room"},
};
#define NSMALL (sizeof small / sizeof small[0])
static const unsigned num_vals[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                    20, 30, 40, 50, 60, 70, 80, 90, 100};
static const char *const num_text[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight",
                                       "Nine", "Ten", "Eleven", "Twelve", "Thirteen", "Fourteen",
                                       "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen",
                                       "Twenty", "Thirty", "Forty", "Fifty", "Sixty", "Seventy",
                                       "Eighty", "Ninety", "Hundred"};
static char keybuf[64][16];

static void small_pack(void)
{
    static vpt_clip_t clips[128];
    unsigned n = 0;
    for (unsigned i = 0; i < NSMALL; i++) {
        vpt_clip_t c = {(uint16_t)(n + 1u), small[i].cat, small[i].text, small[i].key, 0};
        clips[n++] = c;
    }
    for (unsigned i = 0; i < sizeof num_vals / sizeof num_vals[0]; i++) {
        snprintf(keybuf[i], sizeof keybuf[i], "number.%u", num_vals[i]);
        vpt_clip_t c = {(uint16_t)(n + 1u), 4, num_text[i], keybuf[i], 0};
        clips[n++] = c;
    }
    TEST_ASSERT_EQUAL_INT(VP_OK, vpt_build(clips, n, &pack));
}

static const char *text_of(uint16_t id)
{
    static vp_clip_t c;
    TEST_ASSERT_TRUE(vp_find(&pack, id, &c));
    return c.text;
}

/* Joins the texts of a match's ids with spaces. */
static const char *say(const vp_name_match_t *m)
{
    static char s[256];
    s[0] = '\0';
    for (unsigned i = 0; i < m->count; i++) {
        if (i) {
            strcat(s, " ");
        }
        strcat(s, text_of(m->ids[i]));
    }
    return s;
}

static void expect_say(const char *name, const char *want)
{
    vp_name_match_t m;
    bool ok = vp_match_name(&pack, name, &m);
    char msg[160];
    snprintf(msg, sizeof msg, "name \"%s\": result %d, said \"%s\"", name, (int)m.result, say(&m));
    TEST_ASSERT_TRUE_MESSAGE(ok, msg);
    TEST_ASSERT_EQUAL_INT(VP_MATCH_OK, m.result);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, say(&m), msg);
}

/* AUD-026: words, longest phrase, case, separators */
static void test_match_words(void)
{
    small_pack();
    expect_say("Front door", "Front Door");
    expect_say("  fRONT   DOOR ", "Front Door");
    expect_say("front-door", "Front Door");
    expect_say("Front, door!", "Front Door");
    expect_say("Living room", "Living room");               /* the compound, not Living + Room */
    expect_say("living   ROOM motion", "Living room Motion");
    expect_say("Living room-motion", "Living room Motion");
    expect_say("Living", "Living");
    expect_say("Living motion", "Living Motion");           /* not a compound */
    expect_say("Smoke detector", "Smoke detector");
    expect_say("Smoke door", "Smoke Door");
    expect_say("Room room", "Room Room");                   /* repeats allowed */
}

/* AUD-026: digit runs */
static void test_match_numbers(void)
{
    small_pack();
    expect_say("7", "Seven");
    expect_say("0", "Zero");
    expect_say("10", "Ten");
    expect_say("20", "Twenty");
    expect_say("21", "Twenty One");
    expect_say("42", "Forty Two");
    expect_say("99", "Ninety Nine");
    expect_say("100", "One Hundred");
    expect_say("101", "One Hundred One");
    expect_say("115", "One Hundred Fifteen");
    expect_say("128", "One Hundred Twenty Eight");
    expect_say("300", "Three Hundred");
    expect_say("999", "Nine Hundred Ninety Nine");
    expect_say("Door 2", "Door Two");
    expect_say("Door-12", "Door Twelve");
    expect_say("one", "One");                                /* a word that is also a number entry */
    /* leading zeros: digit by digit */
    expect_say("007", "Zero Zero Seven");
    expect_say("05", "Zero Five");
    expect_say("00", "Zero Zero");
    expect_say("010", "Zero One Zero");
}

static void expect_fail(const char *name, vp_match_result_t result, unsigned spans, unsigned total)
{
    vp_name_match_t m;
    TEST_ASSERT_FALSE(vp_match_name(&pack, name, &m));
    char msg[80];
    snprintf(msg, sizeof msg, "name \"%s\"", name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(result, m.result, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(spans, m.span_count, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(total, m.unmatched_total, msg);
}

static void expect_span(const char *name, unsigned k, const char *want)
{
    vp_name_match_t m;
    (void)vp_match_name(&pack, name, &m);
    TEST_ASSERT_TRUE(k < m.span_count);
    TEST_ASSERT_EQUAL_UINT(strlen(want), m.span[k].length);
    TEST_ASSERT_EQUAL_INT(0, strncmp(name + m.span[k].start, want, m.span[k].length));
}

/* AUD-026: nothing partial is ever returned as OK; unmatched spans are named */
static void test_match_refusals(void)
{
    small_pack();
    expect_fail("", VP_MATCH_NO_WORDS, 0, 0);
    expect_fail("  - ,. ", VP_MATCH_NO_WORDS, 0, 0);
    expect_fail("Front door 2nd floor", VP_MATCH_UNMATCHED, 1, 1);
    expect_span("Front door 2nd floor", 0, "2nd");
    expect_fail("B2", VP_MATCH_UNMATCHED, 1, 1);               /* digits glued to letters: one span */
    expect_span("B2", 0, "B2");
    expect_fail("Front Zzz door Qq", VP_MATCH_UNMATCHED, 2, 2);
    expect_span("Front Zzz door Qq", 0, "Zzz");
    expect_span("Front Zzz door Qq", 1, "Qq");
    expect_fail("Caf\xC3\xA9", VP_MATCH_UNMATCHED, 1, 1);     /* non-ASCII is reported, not dropped */
    expect_fail("1000", VP_MATCH_UNMATCHED, 1, 1);
    expect_fail("0007", VP_MATCH_UNMATCHED, 1, 1);
    expect_fail("12345", VP_MATCH_UNMATCHED, 1, 1);
    expect_span("Door 1000", 0, "1000");
    expect_fail("a b c d e f", VP_MATCH_UNMATCHED, 4, 6);      /* four kept, six counted */

    /* too many entries: 9 words */
    vp_name_match_t m;
    TEST_ASSERT_FALSE(vp_match_name(&pack, "Door door door door door door door door door", &m));
    TEST_ASSERT_EQUAL_INT(VP_MATCH_TOO_MANY, m.result);
    TEST_ASSERT_EQUAL_UINT(8, m.count);
    TEST_ASSERT_EQUAL_UINT(9, m.entries);
    /* exactly eight is fine */
    expect_say("Door door door door door door door door", "Door Door Door Door Door Door Door Door");
    /* a number can push a name over the limit: 128 is four entries */
    TEST_ASSERT_FALSE(vp_match_name(&pack, "Front Door 128 Back Floor Door", &m));
    TEST_ASSERT_EQUAL_INT(VP_MATCH_TOO_MANY, m.result);
    TEST_ASSERT_FALSE(vp_match_name(NULL, "Door", &m));
    TEST_ASSERT_FALSE(vp_match_name(&pack, NULL, &m));
    char longname[300];
    memset(longname, 'a', sizeof longname - 1u);
    longname[sizeof longname - 1u] = '\0';
    TEST_ASSERT_FALSE(vp_match_name(&pack, longname, &m));
    TEST_ASSERT_EQUAL_INT(VP_MATCH_TOO_MANY, m.result);
}

static void test_match_missing_number(void)
{
    /* a pack without number.100: 100 and 300 are unmatched, 42 still reads */
    static vpt_clip_t clips[8];
    clips[0] = (vpt_clip_t){1, 3, "Door", "word.door", 0};
    clips[1] = (vpt_clip_t){2, 4, "Four", "number.4", 0};
    clips[2] = (vpt_clip_t){3, 4, "Two", "number.2", 0};
    clips[3] = (vpt_clip_t){4, 4, "Forty", "number.40", 0};
    clips[4] = (vpt_clip_t){5, 4, "One", "number.1", 0};
    TEST_ASSERT_EQUAL_INT(VP_OK, vpt_build(clips, 5, &pack));
    expect_say("42", "Forty Two");
    expect_fail("100", VP_MATCH_UNMATCHED, 1, 1);
    expect_fail("Door 143", VP_MATCH_UNMATCHED, 1, 1);
}

/* AUD-026 / D02: the 10 s budget decides TOO_LONG */
static void test_match_too_long(void)
{
    static vpt_clip_t clips[4];
    clips[0] = (vpt_clip_t){1, 3, "Door", "word.door", 80000};
    clips[1] = (vpt_clip_t){2, 3, "Floor", "word.floor", 80000u - VP_WORD_GAP_MS * 16u};  /* the pack's gap */
    clips[2] = (vpt_clip_t){3, 3, "Motion", "word.motion", 80000u - VP_WORD_GAP_MS * 16u + 1u};
    TEST_ASSERT_EQUAL_INT(VP_OK, vpt_build(clips, 3, &pack));
    vp_name_match_t m;
    expect_say("Door floor", "Door Floor");                 /* exactly 160000 */
    TEST_ASSERT_FALSE(vp_match_name(&pack, "Door motion", &m));
    TEST_ASSERT_EQUAL_INT(VP_MATCH_TOO_LONG, m.result);
    TEST_ASSERT_EQUAL_UINT(2, m.count);
}

/* AUD-026: the real vocabulary, from recording-script.csv (vp_vocab_en_us.h) */
static const char *full_keys[VP_VOCAB_EN_US_COUNT];

static void full_pack(void)
{
    static vpt_clip_t clips[VP_VOCAB_EN_US_COUNT];
    for (unsigned i = 0; i < VP_VOCAB_EN_US_COUNT; i++) {
        clips[i] = (vpt_clip_t){(uint16_t)(i + 1u), vp_vocab_en_us[i].category, vp_vocab_en_us[i].text,
                                vp_vocab_en_us[i].key, 0};
        full_keys[i] = vp_vocab_en_us[i].key;
    }
    TEST_ASSERT_EQUAL_INT(VP_OK, vpt_build(clips, VP_VOCAB_EN_US_COUNT, &pack));
    TEST_ASSERT_EQUAL_UINT(VP_VOCAB_EN_US_COUNT, pack.selectable_count);
}

static void test_full_vocabulary(void)
{
    full_pack();
    for (unsigned i = 0; i < VP_VOCAB_EN_US_COUNT; i++) {
        vp_name_match_t m;
        const char *t = vp_vocab_en_us[i].text;
        bool ok = vp_match_name(&pack, t, &m);
        char msg[96];
        snprintf(msg, sizeof msg, "\"%s\" -> \"%s\" (%d)", t, say(&m), (int)m.result);
        TEST_ASSERT_TRUE_MESSAGE(ok, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, m.count, msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(t, text_of(m.ids[0]), msg);
    }
    expect_say("Front door", "Front Door");
    expect_say("Living room motion", "Living room Motion");
    expect_say("Living room 128", "Living room One Hundred Twenty Eight");
    expect_fail("Front door 2nd floor", VP_MATCH_UNMATCHED, 1, 1);
    expect_span("Front door 2nd floor", 0, "2nd");
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* AUD-026: word lists */
static void expect_list(uint8_t cat, const char *prefix, uint16_t page, unsigned count, unsigned total,
                        unsigned pages)
{
    vp_word_page_t w;
    TEST_ASSERT_TRUE(vp_list_words(&pack, cat, prefix, page, &w));
    char msg[80];
    snprintf(msg, sizeof msg, "cat %u prefix \"%s\" page %u", cat, prefix ? prefix : "(null)", page);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(count, w.count, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(total, w.total, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(pages, w.pages, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(page, w.page, msg);
}

static void test_list_small(void)
{
    small_pack();
    vp_word_page_t w;
    /* prefix of the display text or of any word inside it */
    TEST_ASSERT_TRUE(vp_list_words(&pack, 0, "room", 0, &w));
    TEST_ASSERT_EQUAL_UINT(3, w.count);   /* Dining room, Living room, Room */
    TEST_ASSERT_EQUAL_STRING("Dining room", text_of(w.ids[0]));
    TEST_ASSERT_EQUAL_STRING("Living room", text_of(w.ids[1]));
    TEST_ASSERT_EQUAL_STRING("Room", text_of(w.ids[2]));
    TEST_ASSERT_TRUE(vp_list_words(&pack, 0, "GAR", 0, &w));
    TEST_ASSERT_EQUAL_UINT(2, w.count);
    TEST_ASSERT_EQUAL_STRING("Garage", text_of(w.ids[0]));
    TEST_ASSERT_EQUAL_STRING("Garden", text_of(w.ids[1]));
    expect_list(0, "oom", 0, 0, 0, 0);    /* not the middle of a word */
    expect_list(0, "  smoke  ", 0, 2, 2, 1);
    expect_list(0, "living r", 0, 1, 1, 1);
    expect_list(1, "", 0, 2, 2, 1);       /* Position: Back, Front */
    TEST_ASSERT_TRUE(vp_list_words(&pack, 1, NULL, 0, &w));
    TEST_ASSERT_EQUAL_STRING("Back", text_of(w.ids[0]));
    TEST_ASSERT_EQUAL_STRING("Front", text_of(w.ids[1]));
    expect_list(2, "room", 0, 2, 2, 1);   /* in Rooms and places only */
    expect_list(5, "", 0, 0, 0, 0);       /* status and walk never */
    expect_list(6, "", 0, 0, 0, 0);
    expect_list(0, "", 9, 0, 13u + 29u, 1);  /* past the end: empty, totals set */
    TEST_ASSERT_FALSE(vp_list_words(NULL, 0, "", 0, &w));
    TEST_ASSERT_FALSE(vp_list_words(&pack, 0, "", 0, NULL));
}

static void test_list_full(void)
{
    full_pack();
    expect_list(0, "", 0, 64, 221, 4);
    expect_list(0, NULL, 1, 64, 221, 4);
    expect_list(0, "", 2, 64, 221, 4);
    expect_list(0, "", 3, 29, 221, 4);
    expect_list(0, "", 4, 0, 221, 4);
    expect_list(1, "", 0, 31, 31, 1);
    expect_list(2, "", 0, 64, 77, 2);
    expect_list(3, "", 1, 20, 84, 2);
    expect_list(4, "", 0, 29, 29, 1);

    /* every selectable word once, in case-insensitive alphabetical order, ties by ID */
    static bool seen[VP_VOCAB_EN_US_COUNT + 1u];
    const char *prev = NULL;
    uint16_t prev_id = 0;
    for (uint16_t pg = 0; pg < 4; pg++) {
        vp_word_page_t w;
        TEST_ASSERT_TRUE(vp_list_words(&pack, 0, "", pg, &w));
        for (unsigned i = 0; i < w.count; i++) {
            TEST_ASSERT_FALSE(seen[w.ids[i]]);
            seen[w.ids[i]] = true;
            const char *t = text_of(w.ids[i]);
            if (prev != NULL) {
                int d = ci_cmp(prev, t);
                TEST_ASSERT_TRUE(d < 0 || (d == 0 && prev_id < w.ids[i]));
            }
            prev = t;
            prev_id = w.ids[i];
        }
    }
    for (unsigned id = 1; id <= VP_VOCAB_EN_US_COUNT; id++) {
        TEST_ASSERT_TRUE(seen[id]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_match_words);
    RUN_TEST(test_match_numbers);
    RUN_TEST(test_match_refusals);
    RUN_TEST(test_match_missing_number);
    RUN_TEST(test_match_too_long);
    RUN_TEST(test_full_vocabulary);
    RUN_TEST(test_list_small);
    RUN_TEST(test_list_full);
    return UNITY_END();
}
