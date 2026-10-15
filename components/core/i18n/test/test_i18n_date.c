/*
 * test_i18n_date.c - regions, the date/time pattern formatter and the number formatter
 * (components/core/i18n). Requirements: I18N-016, I18N-017 (docs/features/37-Globalization)
 */
#include <stdint.h>
#include <string.h>

#include "i18n/i18n_date.h"
#include "unity.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

#define NNBSP "\xE2\x80\xAF"
#define NBSP "\xC2\xA0"

/* 2026-09-28 is a Monday; 22:41:07 */
static const i18n_civil_t k_evening = {2026, 9, 28, 22, 41, 7, 1};

static const char *fmt(const char *pattern, const i18n_civil_t *t)
{
    static char buf[96];
    (void)i18n_date(buf, sizeof buf, NULL, pattern, t);
    return buf;
}

static const char *num(const char *tag, int64_t v, uint8_t d)
{
    static char buf[96];
    (void)i18n_number(buf, sizeof buf, i18n_region(tag), v, d);
    return buf;
}

/* ---- regions (I18N-017) ---- */

static void test_region_lookup(void) /* I18N-017 */
{
    const i18n_region_t *us = i18n_region("US");
    TEST_ASSERT_NOT_NULL(us);
    TEST_ASSERT_EQUAL_STRING("US", us->tag);
    TEST_ASSERT_EQUAL_UINT8(0, us->hour24);
    TEST_ASSERT_EQUAL_UINT8(0, us->first_day);
    TEST_ASSERT_EQUAL_UINT8(0, us->celsius);
    TEST_ASSERT_TRUE(i18n_region("us") == us);
    TEST_ASSERT_TRUE(i18n_region("es-US") == us);
    TEST_ASSERT_TRUE(i18n_region("en_us") == us);
    TEST_ASSERT_TRUE(i18n_region("zh-Hant-TW") == i18n_region("TW"));
    TEST_ASSERT_TRUE(i18n_region("pt-br") == i18n_region("BR"));
    TEST_ASSERT_NULL(i18n_region("ZZ"));
    TEST_ASSERT_NULL(i18n_region("spa")); /* a bare tag is a region only with two letters */
    TEST_ASSERT_NULL(i18n_region("es-419"));
    TEST_ASSERT_NULL(i18n_region(""));
    TEST_ASSERT_NULL(i18n_region("U"));
    TEST_ASSERT_NULL(i18n_region(NULL));
}

static void test_region_table(void) /* I18N-017 */
{
    TEST_ASSERT_EQUAL_UINT(21, i18n_region_count());
    TEST_ASSERT_NULL(i18n_region_at(i18n_region_count()));
    for (size_t i = 0; i < i18n_region_count(); i++) {
        const i18n_region_t *r = i18n_region_at(i);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_EQUAL_UINT(2, strlen(r->tag));
        TEST_ASSERT_TRUE(i18n_region(r->tag) == r);
        if (i > 0) {
            TEST_ASSERT_TRUE(strcmp(i18n_region_at(i - 1)->tag, r->tag) < 0); /* sorted */
        }
        TEST_ASSERT_TRUE(r->first_day == 0 || r->first_day == 1 || r->first_day == 6);
        TEST_ASSERT_TRUE(strlen(r->decimal) >= 1 && strlen(r->group) >= 1);
        TEST_ASSERT_TRUE(r->min_grouping == 1 || r->min_grouping == 2);
        TEST_ASSERT_NOT_NULL(r->name); /* the lists show it (I18N-013) */
        TEST_ASSERT_TRUE(strlen(r->name) > 3 && strlen(r->name) <= 47);
    }
    TEST_ASSERT_EQUAL_STRING("Germany", i18n_region("DE")->name);
    /* first day: Sunday, Monday, Saturday */
    TEST_ASSERT_EQUAL_UINT8(1, i18n_region("DE")->first_day);
    TEST_ASSERT_EQUAL_UINT8(6, i18n_region("EG")->first_day);
    TEST_ASSERT_EQUAL_UINT8(1, i18n_region("AU")->first_day);
    /* the clock and the unit */
    TEST_ASSERT_EQUAL_UINT8(1, i18n_region("DE")->hour24);
    TEST_ASSERT_EQUAL_UINT8(0, i18n_region("AU")->hour24);
    TEST_ASSERT_EQUAL_UINT8(1, i18n_region("GB")->celsius);
    TEST_ASSERT_EQUAL_UINT8(1, i18n_region("CA")->celsius);
    /* separators */
    TEST_ASSERT_EQUAL_STRING(NNBSP, i18n_region("FR")->group);
    TEST_ASSERT_EQUAL_STRING(NBSP, i18n_region("PT")->group);
    TEST_ASSERT_EQUAL_STRING(",", i18n_region("DE")->decimal);
    TEST_ASSERT_EQUAL_UINT8(2, i18n_region("ES")->min_grouping);
}

/* The region formats follow: chosen, else the language's, else the US (I18N-016). */
static void test_region_for(void) /* I18N-016 */
{
    TEST_ASSERT_TRUE(i18n_region_for("DE", "en-US") == i18n_region("DE"));
    TEST_ASSERT_TRUE(i18n_region_for("", "es-MX") == i18n_region("MX"));
    TEST_ASSERT_TRUE(i18n_region_for(NULL, "fr-FR") == i18n_region("FR"));
    TEST_ASSERT_TRUE(i18n_region_for("ZZ", "en-GB") == i18n_region("GB")); /* unknown: skipped */
    TEST_ASSERT_TRUE(i18n_region_for("", "") == i18n_region("US"));
    TEST_ASSERT_TRUE(i18n_region_for(NULL, "en-XA") == i18n_region("US")); /* a test language */
    TEST_ASSERT_TRUE(i18n_region_for(NULL, NULL) == i18n_region("US"));
}

/* Automatic follows the region, 12 h and 24 h are fixed; a panel from before the setting kept
 * only "24 h or not", read as 24 h or Automatic (docs/features/37). */
static void test_clock_choice(void) /* I18N-016 */
{
    const i18n_region_t *us = i18n_region("US");
    const i18n_region_t *de = i18n_region("DE");
    TEST_ASSERT_FALSE(i18n_clock_24h(I18N_CLOCK_AUTO, us));
    TEST_ASSERT_TRUE(i18n_clock_24h(I18N_CLOCK_AUTO, de));
    TEST_ASSERT_FALSE(i18n_clock_24h(I18N_CLOCK_12H, de));
    TEST_ASSERT_TRUE(i18n_clock_24h(I18N_CLOCK_24H, us));
    TEST_ASSERT_FALSE(i18n_clock_24h(I18N_CLOCK_AUTO, NULL));
    TEST_ASSERT_FALSE(i18n_clock_24h(7, us)); /* unknown: the region's */
    TEST_ASSERT_TRUE(i18n_clock_24h(7, de));
    TEST_ASSERT_EQUAL_INT(0, I18N_CLOCK_AUTO); /* stored numbers */
    TEST_ASSERT_EQUAL_INT(1, I18N_CLOCK_12H);
    TEST_ASSERT_EQUAL_INT(2, I18N_CLOCK_24H);
}

/* English puts the day first where the region does; a catalog writes its own order. */
static void test_date_region_order(void) /* I18N-016 */
{
    char out[64];
    TEST_ASSERT_EQUAL_UINT(13, i18n_pattern_day_first(out, sizeof(out), "MMM d, h:mm a"));
    TEST_ASSERT_EQUAL_STRING("d MMM, h:mm a", out);
    i18n_pattern_day_first(out, sizeof(out), "EEEE, MMMM d");
    TEST_ASSERT_EQUAL_STRING("EEEE d MMMM", out);
    i18n_pattern_day_first(out, sizeof(out), "EEE, MMM d, y \xC2\xB7 HH:mm");
    TEST_ASSERT_EQUAL_STRING("EEE d MMM y \xC2\xB7 HH:mm", out);
    i18n_pattern_day_first(out, sizeof(out), "HH:mm");
    TEST_ASSERT_EQUAL_STRING("HH:mm", out);

    const i18n_civil_t t = {.year = 2026, .month = 9, .day = 30, .hour = 22, .minute = 42,
                            .weekday = 3};
    i18n_date_in(out, sizeof(out), NULL, i18n_region("GB"), "MMM d, h:mm a", &t);
    TEST_ASSERT_EQUAL_STRING("30 Sep, 10:42 PM", out);
    i18n_date_in(out, sizeof(out), NULL, i18n_region("GB"), "EEEE, MMMM d", &t);
    TEST_ASSERT_EQUAL_STRING("Wednesday 30 September", out);
    i18n_date_in(out, sizeof(out), NULL, i18n_region("US"), "MMM d, h:mm a", &t);
    TEST_ASSERT_EQUAL_STRING("Sep 30, 10:42 PM", out);
    i18n_date_in(out, sizeof(out), NULL, i18n_region("CA"), "MMM d, h:mm a", &t);
    TEST_ASSERT_EQUAL_STRING("Sep 30, 10:42 PM", out);
    i18n_date_in(out, sizeof(out), NULL, NULL, "MMM d, h:mm a", &t);
    TEST_ASSERT_EQUAL_STRING("Sep 30, 10:42 PM", out);
    TEST_ASSERT_EQUAL_UINT8(1, i18n_region("DE")->day_first);
    TEST_ASSERT_EQUAL_UINT8(0, i18n_region("US")->day_first);
}

/* ---- dates (I18N-016) ---- */

static void test_date_english_outputs(void) /* I18N-016 */
{
    i18n_civil_t sep19 = {2026, 9, 19, 22, 41, 0, 6};
    TEST_ASSERT_EQUAL_STRING("Sep 19, 10:41 PM", fmt("MMM d, h:mm a", &sep19));
    TEST_ASSERT_EQUAL_STRING("10:41", fmt("h:mm", &sep19));
    TEST_ASSERT_EQUAL_STRING("22:41", fmt("HH:mm", &sep19));
    TEST_ASSERT_EQUAL_STRING("Mon, Sep 28, 2026 \xC2\xB7 10:41 PM",
                             fmt("EEE, MMM d, y \xC2\xB7 h:mm a", &k_evening));
    TEST_ASSERT_EQUAL_STRING("September", fmt("MMMM", &k_evening));
    TEST_ASSERT_EQUAL_STRING("January", fmt("MMMM", &(i18n_civil_t){2026, 1, 1, 0, 0, 0, 4}));
    TEST_ASSERT_EQUAL_STRING("10 PM", fmt("h a", &k_evening));
    TEST_ASSERT_EQUAL_STRING("Monday", fmt("EEEE", &k_evening));
    TEST_ASSERT_EQUAL_STRING("28/09/2026 22:41:07", fmt("dd/MM/yyyy HH:mm:ss", &k_evening));
    TEST_ASSERT_EQUAL_STRING("9/8/26", fmt("M/d/yy", &(i18n_civil_t){2026, 9, 8, 0, 0, 0, 2}));
}

static void test_date_twelve_hour(void) /* I18N-016 */
{
    i18n_civil_t t = {2026, 9, 28, 0, 5, 0, 1};
    TEST_ASSERT_EQUAL_STRING("12 AM", fmt("h a", &t));
    TEST_ASSERT_EQUAL_STRING("12:05 AM", fmt("h:mm a", &t));
    t.hour = 12;
    TEST_ASSERT_EQUAL_STRING("12 PM", fmt("h a", &t));
    t.hour = 7;
    TEST_ASSERT_EQUAL_STRING("07", fmt("HH", &t));
    TEST_ASSERT_EQUAL_STRING("7", fmt("H", &t));
    TEST_ASSERT_EQUAL_STRING("07 AM", fmt("hh a", &t));
    t.hour = 13;
    TEST_ASSERT_EQUAL_STRING("1 PM", fmt("h a", &t));
    t.hour = 23;
    TEST_ASSERT_EQUAL_STRING("11 PM", fmt("h a", &t));
}

static void test_date_literals_and_copy(void) /* I18N-016 */
{
    TEST_ASSERT_EQUAL_STRING("at 22h41", fmt("'at' HH'h'mm", &k_evening));
    TEST_ASSERT_EQUAL_STRING("it's 22", fmt("'it''s' HH", &k_evening));
    TEST_ASSERT_EQUAL_STRING("'22", fmt("''HH", &k_evening));
    TEST_ASSERT_EQUAL_STRING("d M y", fmt("'d M y'", &k_evening));
    TEST_ASSERT_EQUAL_STRING("2026\xE5\xB9\xB4" "9\xE6\x9C\x88" "28\xE6\x97\xA5",
                             fmt("y\xE5\xB9\xB4" "M\xE6\x9C\x88" "d\xE6\x97\xA5", &k_evening));
    TEST_ASSERT_EQUAL_STRING("x-Z.", fmt("x-Z.", &k_evening)); /* other letters are copied */
    TEST_ASSERT_EQUAL_STRING("unterminated 22", fmt("'unterminated 22", &k_evening));
    TEST_ASSERT_EQUAL_STRING("", fmt("", &k_evening));
    char buf[8] = "junk";
    TEST_ASSERT_EQUAL_UINT(0, i18n_date(buf, sizeof buf, NULL, NULL, &k_evening));
    TEST_ASSERT_EQUAL_STRING("", buf);
    /* no time: the letters are copied as they are */
    TEST_ASSERT_EQUAL_UINT(0, i18n_date(NULL, 4, NULL, "y", &k_evening));
    TEST_ASSERT_EQUAL_UINT(0, i18n_date(buf, 0, NULL, "y", &k_evening));
}

static void test_date_out_of_range(void) /* I18N-016 */
{
    i18n_civil_t t = {2026, 13, 32, 99, 99, 99, 9};
    TEST_ASSERT_EQUAL_STRING("13", fmt("MMMM", &t));
    TEST_ASSERT_EQUAL_STRING("13", fmt("MMM", &t));
    TEST_ASSERT_EQUAL_STRING("13", fmt("MM", &t));
    TEST_ASSERT_EQUAL_STRING("[]", fmt("[EEEE]", &t));
    TEST_ASSERT_EQUAL_STRING("[]", fmt("[EEE]", &t));
    TEST_ASSERT_EQUAL_STRING("[]", fmt("[a]", &t));
    TEST_ASSERT_EQUAL_STRING("99:99:99", fmt("H:mm:ss", &t));
    TEST_ASSERT_EQUAL_STRING("99", fmt("h", &t));
    t.month = 0;
    TEST_ASSERT_EQUAL_STRING("0", fmt("MMMM", &t));
    t.month = 255;
    t.weekday = 255;
    TEST_ASSERT_EQUAL_STRING("255|", fmt("MMM|EEEE", &t));
    TEST_ASSERT_EQUAL_STRING("-5", fmt("y", &(i18n_civil_t){-5, 1, 1, 0, 0, 0, 0}));
}

static void test_date_names_come_from_the_catalog(void) /* I18N-016 */
{
    /* one-entry catalog: "September" in the "month" context; the others fall back to English */
    static i18n_entry_t entries[1];
    static const char strings[] = "Septiembre";
    entries[0].hash = i18n_hash("month", "September");
    entries[0].text = 0;
    i18n_catalog_t cat = {"es", (uint8_t)I18N_PLURALS_ES, 0, 1, entries, strings};
    char buf[64];
    (void)i18n_date(buf, sizeof buf, &cat, "MMMM / MMM / EEEE", &k_evening);
    TEST_ASSERT_EQUAL_STRING("Septiembre / Sep / Monday", buf);
    /* a translated pattern reorders and reuses the same fields */
    (void)i18n_date(buf, sizeof buf, &cat, "EEE d 'de' MMMM", &k_evening);
    TEST_ASSERT_EQUAL_STRING("Mon 28 de Septiembre", buf);
}

/* A catalog with one name in it, for the capital tests below. */
static const char *one_name(const char *ctx, const char *english, const char *name,
                            const char *pattern)
{
    static i18n_entry_t entries[1];
    static char strings[32];
    static char buf[64];
    entries[0].hash = i18n_hash(ctx, english);
    entries[0].text = 0;
    strcpy(strings, name);
    const i18n_catalog_t cat = {"xx", (uint8_t)I18N_PLURALS_OTHER, 0, 1, entries, strings};
    (void)i18n_date(buf, sizeof buf, &cat, pattern, &k_evening);
    return buf;
}

/* I18N-016: a date that opens with a weekday or month name starts with a capital, as CLDR's
 * stand-alone form has it ("Lunes, 28 de septiembre" for a catalog with small names); a name further on
 * keeps the catalog's small letter, and the AM/PM word stays as it is. */
static void test_date_opens_with_a_capital(void)
{
    TEST_ASSERT_EQUAL_STRING("Septiembre", one_name("month", "September", "septiembre", "MMMM"));
    TEST_ASSERT_EQUAL_STRING("28 de septiembre",
                             one_name("month", "September", "septiembre", "d 'de' MMMM"));
    TEST_ASSERT_EQUAL_STRING("Lunes 28", one_name("weekday", "Monday", "lunes", "EEEE d"));
    TEST_ASSERT_EQUAL_STRING("el lunes", one_name("weekday", "Monday", "lunes", "'el' EEEE"));
    TEST_ASSERT_EQUAL_STRING("p. m.", one_name("day period", "PM", "p. m.", "a"));
    /* Latin-1 and Latin Extended-A capitals take the same two bytes */
    TEST_ASSERT_EQUAL_STRING("\xC3\x89t\xC3\xA9", one_name("weekday", "Monday", "\xC3\xA9t\xC3\xA9", "EEEE"));
    TEST_ASSERT_EQUAL_STRING("\xC5\x9Aroda", one_name("weekday", "Monday", "\xC5\x9Broda", "EEEE"));
    TEST_ASSERT_EQUAL_STRING("\xC5\x81ut", one_name("weekday", "Monday", "\xC5\x82ut", "EEEE"));
    /* the committed Spanish catalog, with its screen-saver pattern */
    const i18n_catalog_t *es = i18n_catalog("es-US");
    TEST_ASSERT_NOT_NULL(es);
    char buf[64];
    (void)i18n_date(buf, sizeof buf, es, "EEEE, d 'de' MMMM", &k_evening);
    TEST_ASSERT_EQUAL_STRING("Lunes, 28 de Septiembre", buf); /* the reviewer capitalizes names */
}

/* I18N-016, I18N-038: British English writes the day first from its own patterns, am/pm small and
 * "Sept", with the United Kingdom's 24-hour clock unless 12 h is chosen */
static void test_date_british_english(void)
{
    const i18n_catalog_t *gb = i18n_catalog("en-GB");
    TEST_ASSERT_NOT_NULL(gb);
    char buf[64];
    (void)i18n_date(buf, sizeof buf, gb, i18n_text(gb, "date pattern", "EEEE, MMMM d"), &k_evening);
    TEST_ASSERT_EQUAL_STRING("Monday 28 September", buf);
    (void)i18n_date(buf, sizeof buf, gb, i18n_text(gb, "date pattern", "MMM d, h:mm a"), &k_evening);
    TEST_ASSERT_EQUAL_STRING("28 Sept, 10:41 pm", buf);
    TEST_ASSERT_TRUE(i18n_clock_24h(I18N_CLOCK_AUTO, i18n_region_for(NULL, "en-GB")));
}

static void test_date_truncates_on_a_character(void) /* I18N-016 */
{
    char buf[16];
    size_t n;
    /* a 3-byte character does not fit in the last 2 bytes */
    n = i18n_date(buf, 4, NULL, "d\xE5\xB9\xB4", &k_evening);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8('2', (uint8_t)buf[0]);
    TEST_ASSERT_EQUAL_UINT8('8', (uint8_t)buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)buf[2]);
    n = i18n_date(buf, 6, NULL, "d\xE5\xB9\xB4", &k_evening);
    TEST_ASSERT_EQUAL_UINT(5, n);
    TEST_ASSERT_EQUAL_UINT(5, strlen(buf));
    n = i18n_date(buf, 5, NULL, "MMMM", &k_evening);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_STRING("Sept", buf);
    n = i18n_date(buf, 1, NULL, "MMMM", &k_evening);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ---- numbers (I18N-017) ---- */

static void test_number_decimals(void) /* I18N-017 */
{
    TEST_ASSERT_EQUAL_STRING("21.5", num("US", 215, 1));
    TEST_ASSERT_EQUAL_STRING("21,5", num("DE", 215, 1));
    TEST_ASSERT_EQUAL_STRING("0.05", num("US", 5, 2));
    TEST_ASSERT_EQUAL_STRING("-0.5", num("US", -5, 1));
    TEST_ASSERT_EQUAL_STRING("-21,5", num("FR", -215, 1));
    TEST_ASSERT_EQUAL_STRING("7", num("US", 7, 0));
    TEST_ASSERT_EQUAL_STRING("0", num("US", 0, 0));
    TEST_ASSERT_EQUAL_STRING("0.00", num("US", 0, 2));
    TEST_ASSERT_EQUAL_STRING("100.000", num("US", 100000, 3));
}

static void test_number_grouping(void) /* I18N-017 */
{
    TEST_ASSERT_EQUAL_STRING("999", num("US", 999, 0));
    TEST_ASSERT_EQUAL_STRING("1,000", num("US", 1000, 0));
    TEST_ASSERT_EQUAL_STRING("1,234,567", num("US", 1234567, 0));
    TEST_ASSERT_EQUAL_STRING("-1,234,567.89", num("US", -123456789, 2));
    TEST_ASSERT_EQUAL_STRING("1.234.567,89", num("DE", 123456789, 2));
    TEST_ASSERT_EQUAL_STRING("1" NNBSP "234" NNBSP "567", num("FR", 1234567, 0));
    /* Spanish and Portuguese: no separator in four-digit numbers */
    TEST_ASSERT_EQUAL_STRING("1234", num("ES", 1234, 0));
    TEST_ASSERT_EQUAL_STRING("12.345", num("ES", 12345, 0));
    TEST_ASSERT_EQUAL_STRING("1234,5", num("ES", 12345, 1));
    TEST_ASSERT_EQUAL_STRING("1234", num("PT", 1234, 0));
    TEST_ASSERT_EQUAL_STRING("12" NBSP "345", num("PT", 12345, 0));
    /* a region NULL is US */
    char buf[32];
    (void)i18n_number(buf, sizeof buf, NULL, 1234567, 1);
    TEST_ASSERT_EQUAL_STRING("123,456.7", buf);
}

static void test_number_extremes_and_truncation(void) /* I18N-017 */
{
    TEST_ASSERT_EQUAL_STRING("9,223,372,036,854,775,807", num("US", INT64_MAX, 0));
    TEST_ASSERT_EQUAL_STRING("-9,223,372,036,854,775,808", num("US", INT64_MIN, 0));
    TEST_ASSERT_EQUAL_STRING("0.9223372036854775807", num("US", INT64_MAX, 19));
    TEST_ASSERT_EQUAL_STRING("0.9223372036854775807", num("US", INT64_MAX, 200)); /* clamped */
    char buf[8];
    size_t n = i18n_number(buf, sizeof buf, i18n_region("FR"), 123456789, 0);
    TEST_ASSERT_EQUAL_UINT(strlen(buf), n);
    TEST_ASSERT_TRUE(i18n_utf8_valid(buf, n)); /* cut on a whole character */
    TEST_ASSERT_EQUAL_STRING("123" NNBSP "4", buf);
    n = i18n_number(buf, 6, i18n_region("FR"), 123456789, 0);
    TEST_ASSERT_EQUAL_STRING("123", buf); /* the 3-byte separator does not fit */
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT(0, i18n_number(buf, 0, NULL, 1, 0));
    TEST_ASSERT_EQUAL_UINT(0, i18n_number(NULL, 8, NULL, 1, 0));
}

int main(void);
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_region_lookup);
    RUN_TEST(test_region_table);
    RUN_TEST(test_date_english_outputs);
    RUN_TEST(test_date_twelve_hour);
    RUN_TEST(test_date_literals_and_copy);
    RUN_TEST(test_date_out_of_range);
    RUN_TEST(test_date_names_come_from_the_catalog);
    RUN_TEST(test_date_opens_with_a_capital);
    RUN_TEST(test_date_british_english);
    RUN_TEST(test_date_truncates_on_a_character);
    RUN_TEST(test_number_decimals);
    RUN_TEST(test_number_grouping);
    RUN_TEST(test_number_extremes_and_truncation);
    RUN_TEST(test_region_for);
    RUN_TEST(test_clock_choice);
    RUN_TEST(test_date_region_order);
    return UNITY_END();
}
