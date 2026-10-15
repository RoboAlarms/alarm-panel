/*
 * ui_lang.c - the language rows of Settings > Time and language (docs/features/37 G3, I18N-012,
 * I18N-013; BL-131): the language the screen speaks, the region its dates and numbers follow, and
 * the 12- or 24-hour clock. The language and the region are the household's, like the time zone
 * (a master or installer code); the clock is this keypad's and anyone's (docs/features/14).
 */
#include <stdio.h>
#include <string.h>

#include "i18n/i18n_date.h"
#include "ui_internal.h"

/* English first, then every catalog this firmware carries, each in its own name. With the
 * backend's language set (docs/features/37 language-packs.md) the catalogs are the languages
 * installed on the panel instead: the ones that can be used (ready, or made for other firmware);
 * a refused or damaged one is left out and the Panel Manager removes it. */
#define LANG_MAX 24u
#define DETAIL_MAX 96u

/* What shows these choices, built again after one: the Settings page, or the setup's step. */
static void (*s_rebuild)(void) = ui_page_rebuild;

static ui_lang_t s_shown[LANG_MAX] UI_SLOW_BSS; /* the backend's usable languages (after English) */
static size_t s_shown_n;
static char s_detail[LANG_MAX][DETAIL_MAX] UI_SLOW_BSS;
static char s_absent[80] UI_SLOW_BSS;

static bool from_backend(void)
{
    return S.be.lang_catalog != NULL && S.be.lang_list != NULL;
}

const i18n_catalog_t *ui_lang_catalog(const char *locale)
{
    return S.be.lang_catalog != NULL ? S.be.lang_catalog(S.be.ctx, locale) : i18n_catalog(locale);
}

/* Reads the backend's list again; called where a list or the page is built. */
static void lang_refresh(void)
{
    s_shown_n = 0;
    if (!from_backend()) {
        return;
    }
    static ui_lang_t all[LANG_MAX] UI_SLOW_BSS;
    const size_t n = S.be.lang_list(S.be.ctx, all, LANG_MAX - 1u);
    for (size_t i = 0; i < n && i < LANG_MAX - 1u; i++) {
        if (all[i].state != UI_LANG_READY && all[i].state != UI_LANG_OTHER) {
            continue;
        }
        s_shown[s_shown_n] = all[i];
        s_shown[s_shown_n].locale[sizeof(s_shown[0].locale) - 1u] = '\0';
        s_shown[s_shown_n].name[sizeof(s_shown[0].name) - 1u] = '\0';
        s_shown[s_shown_n].version[sizeof(s_shown[0].version) - 1u] = '\0';
        s_shown_n++;
    }
}

static bool tag_same(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        const char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + ('a' - 'A')) : *a;
        const char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + ('a' - 'A')) : *b;
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
}

static const char *lang_tag(size_t i)
{
    if (i == 0u) {
        return NOTR("en-US"); /* a BCP 47 tag, not words */
    }
    if (from_backend()) {
        return i - 1u < s_shown_n ? s_shown[i - 1u].locale : NULL;
    }
    const i18n_catalog_t *c = i18n_catalog_at(i - 1u);
    return c != NULL ? c->locale : NULL;
}

static size_t lang_count(void)
{
    const size_t n = 1u + (from_backend() ? s_shown_n : i18n_catalog_count());
    return n < LANG_MAX ? n : LANG_MAX;
}

/* Which entry of the list is the stored language: an English tag is English. */
static int32_t lang_index(void)
{
    if (from_backend()) {
        for (size_t i = 0; i < s_shown_n; i++) {
            if (tag_same(s_shown[i].locale, S.opt.lang)) {
                return (int32_t)i + 1;
            }
        }
        return 0;
    }
    const i18n_catalog_t *c = i18n_catalog(S.opt.lang);
    for (size_t i = 1u; c != NULL && i < lang_count(); i++) {
        if (i18n_catalog_at(i - 1u) == c) {
            return (int32_t)i;
        }
    }
    return 0;
}

static const char *lang_name(size_t i)
{
    if (i == 0u) {
        return i18n_language_name(NULL);
    }
    return from_backend() ? s_shown[i - 1u].name : i18n_language_name(i18n_catalog_at(i - 1u));
}

/* The line under a language: what is worth knowing first (docs/features/37 I18N-043). */
static const char *lang_detail(size_t i)
{
    if (i == 0u) {
        return NULL;
    }
    if (!from_backend()) {
        const i18n_catalog_t *c = i18n_catalog_at(i - 1u);
        /* TR: Time and language, the language list: a translation still being checked. At
         * most 63 bytes */
        return c != NULL && c->preview ? TR("A draft, for testing") : NULL;
    }
    const ui_lang_t *l = &s_shown[i - 1u];
    char *buf = s_detail[i];
    if (l->state == UI_LANG_OTHER) {
        /* TR: Time and language, the language list: this language was made for another
         * firmware version, {0}. At most 80 bytes */
        I18N_FORMAT(buf, DETAIL_MAX, TR("Made for version {0}: some phrases may show in English"),
                    l->version);
        return buf;
    }
    if (l->missing > 0u) {
        /* TR: Time and language, the language list: how many phrases this language lacks, so
         * they show in English; {0} is the number. At most 80 bytes */
        I18N_FORMAT(buf, DETAIL_MAX,
                    TRN("{0} phrase shows in English", "{0} phrases show in English", l->missing),
                    l->missing);
        return buf;
    }
    return l->draft ? TR("A draft, for testing") : NULL;
}

static void lang_chosen(int32_t index)
{
    const char *tag = index >= 0 ? lang_tag((size_t)index) : NULL;
    if (tag != NULL && strcmp(tag, S.opt.lang) != 0) {
        snprintf(S.opt.lang, sizeof(S.opt.lang), "%s", tag);
        ui_options_changed();
        ui_language_apply(); /* every screen, this page included, in the new language */
    }
    s_rebuild();
}

static void pick_language(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[LANG_MAX] UI_SLOW_BSS;
    lang_refresh();
    const size_t n = lang_count();
    for (size_t i = 0; i < n; i++) {
        choices[i].glyph = NULL;
        choices[i].tone = ui_tone_blue();
        choices[i].title = lang_name(i);
        choices[i].detail = lang_detail(i);
    }
    /* TR: Time and language, the language list's title. At most 127 bytes */
    ui_pick_sheet(TR("Language"),
                  /* TR: Time and language, under the language list's title. At most 127 bytes */
                  TR("The words on this screen. Names stay as they were typed."), choices, n,
                  lang_index(), lang_chosen);
}

/* ---- the region -------------------------------------------------------------------------------- */

static const char *region_name(const i18n_region_t *r)
{
    return i18n_text(S.catalog, "region", r->name);
}

static void region_chosen(int32_t index)
{
    /* 0: the language's; then the regions in their table's order */
    const i18n_region_t *r = index > 0 ? i18n_region_at((size_t)index - 1u) : NULL;
    if (index >= 0) {
        snprintf(S.opt.region, sizeof(S.opt.region), "%s", r != NULL ? r->tag : "");
        ui_options_changed(); /* the clock and the dates follow at once */
    }
    s_rebuild();
}

static void pick_region(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[1u + 32u] UI_SLOW_BSS;
    static char automatic[80] UI_SLOW_BSS;
    size_t n = 0;
    choices[n].glyph = NULL;
    choices[n].tone = ui_tone_blue();
    /* TR: Time and language, the region list's first entry: the language decides the region. At
     * most 63 bytes */
    choices[n].title = TR_C("region choice", "Automatic");
    /* TR: Time and language, under Automatic in the region list: the region the language gives,
     * {0}. At most 79 bytes */
    I18N_FORMAT(automatic, sizeof(automatic), TR("{0}, from the language"),
                region_name(i18n_region_for(NULL, S.opt.lang)));
    choices[n].detail = automatic;
    n++;
    int32_t selected = 0;
    for (size_t i = 0; i < i18n_region_count() && n < sizeof(choices) / sizeof(choices[0]); i++) {
        const i18n_region_t *r = i18n_region_at(i);
        choices[n].glyph = NULL;
        choices[n].tone = ui_tone_blue();
        choices[n].title = region_name(r);
        choices[n].detail = NULL;
        if (S.opt.region[0] != '\0' && i18n_region(S.opt.region) == r) {
            selected = (int32_t)n;
        }
        n++;
    }
    /* TR: Time and language, the region list's title. At most 127 bytes */
    ui_pick_sheet(TR("Region"),
                  /* TR: Time and language, under the region list's title. At most 127 bytes */
                  TR("How dates and numbers are written, the first day of the week and the clock"),
                  choices, n, selected, region_chosen);
}

/* ---- the clock --------------------------------------------------------------------------------- */

/* 14:30 on this panel's two clocks, in its language. */
static void sample(char *buf, size_t len, bool h24)
{
    const i18n_civil_t c = {.year = 2026, .month = 1, .day = 1, .hour = 14, .minute = 30};
    i18n_date(buf, len, S.catalog,
              h24 ? TR_C("date pattern", "HH:mm") : TR_C("date pattern", "h:mm a"), &c);
}

static void clock_chosen(int32_t index)
{
    if (index >= I18N_CLOCK_AUTO && index <= I18N_CLOCK_24H) {
        S.opt.clock_mode = (uint8_t)index;
        ui_options_changed();
    }
    s_rebuild();
}

static void pick_clock(void *ctx)
{
    (void)ctx;
    static char samples[3][48] UI_SLOW_BSS;
    char region[24];
    sample(region, sizeof(region), i18n_clock_24h(I18N_CLOCK_AUTO, i18n_region_for(S.opt.region, S.opt.lang)));
    /* TR: Time and language, under Automatic in the clock list: 14:30 as the region writes it,
     * {0}. At most 47 bytes */
    I18N_FORMAT(samples[0], sizeof(samples[0]), TR("{0}, from the region"), region);
    sample(samples[1], sizeof(samples[1]), false);
    sample(samples[2], sizeof(samples[2]), true);
    static ui_choice_t choices[3] UI_SLOW_BSS;
    /* TR: Time and language, the clock list: the region decides between 12 and 24 hours. At
     * most 63 bytes */
    choices[0] = (ui_choice_t){NULL, ui_tone_blue(), TR_C("clock choice", "Automatic"), samples[0]};
    /* TR: Time and language, the clock list and the Clock row: always 12 hours with AM and PM. At
     * most 63 bytes */
    choices[1] = (ui_choice_t){NULL, ui_tone_blue(), TR("12-hour"), samples[1]};
    /* TR: Time and language, the clock list and the Clock row: always 24 hours. At most 63 bytes */
    choices[2] = (ui_choice_t){NULL, ui_tone_blue(), TR("24-hour"), samples[2]};
    /* TR: Time and language, the clock list's title. At most 127 bytes */
    ui_pick_sheet(TR("Clock"),
                  /* TR: Time and language, under the clock list's title. At most 127 bytes */
                  TR("On this keypad"), choices, 3, S.opt.clock_mode <= I18N_CLOCK_24H ? S.opt.clock_mode : 0,
                  clock_chosen);
}

/* ---- the rows on Time and language (BL-131) ----------------------------------------------------- */

void ui_lang_clock_row(lv_obj_t *body)
{
    s_rebuild = ui_page_rebuild;
    char h12[24];
    char h24[24];
    char both[56];
    sample(h12, sizeof(h12), false);
    sample(h24, sizeof(h24), true);
    /* TR: Time and language, under the Clock row: 14:30 on a 12-hour clock {0} and on a 24-hour
     * clock {1}. At most 55 bytes */
    I18N_FORMAT(both, sizeof(both), TR("{0} or {1}"), h12, h24);
    /* The value is what the clock does now, the region's choice included; the list says which
     * was chosen. */
    ui_row_link(body, UI_ICON_CLOCK, ui_tone_blue(), TR("Clock"), both,
                ui_clock_24h() ? TR("24-hour") : TR("12-hour"), UI_ICON_CHEVRON_RIGHT, pick_clock, NULL);
}

/* The English name of a stored language the panel doesn't carry (an installed one has its own
 * name); a tag not in this list is shown as the tag. */
typedef struct {
    const char *code; /* the tag's language part */
    const char *name;
} absent_lang_t;

/* TR: the name of a language in English, for "{0} isn't installed" under the Language row. At
 * most 31 bytes */
static const absent_lang_t ABSENT_LANGS[] = {
    {"ar", TR_NOOP_C("language name", "Arabic")},    {"de", TR_NOOP_C("language name", "German")},
    {"es", TR_NOOP_C("language name", "Spanish")},   {"fr", TR_NOOP_C("language name", "French")},
    {"it", TR_NOOP_C("language name", "Italian")},   {"ja", TR_NOOP_C("language name", "Japanese")},
    {"ko", TR_NOOP_C("language name", "Korean")},    {"nl", TR_NOOP_C("language name", "Dutch")},
    {"pl", TR_NOOP_C("language name", "Polish")},    {"pt", TR_NOOP_C("language name", "Portuguese")},
    {"ru", TR_NOOP_C("language name", "Russian")},   {"sv", TR_NOOP_C("language name", "Swedish")},
    {"tr", TR_NOOP_C("language name", "Turkish")},   {"zh", TR_NOOP_C("language name", "Chinese")},
};

static const char *absent_name(const char *tag)
{
    const size_t n = strcspn(tag, "-_");
    for (size_t i = 0; i < sizeof(ABSENT_LANGS) / sizeof(ABSENT_LANGS[0]); i++) {
        if (strlen(ABSENT_LANGS[i].code) == n && strncmp(ABSENT_LANGS[i].code, tag, n) == 0) {
            return TR_C("language name", ABSENT_LANGS[i].name);
        }
    }
    return tag;
}

void ui_lang_rows(lv_obj_t *body)
{
    s_rebuild = ui_page_rebuild;
    lang_refresh();
    /* A stored language the panel doesn't have shows English, and the row says why. */
    const char *absent = NULL;
    if (from_backend() && lang_index() == 0 && S.opt.lang[0] != '\0' &&
        !tag_same(S.opt.lang, "en") && !tag_same(S.opt.lang, "en-US")) {
        /* TR: Time and language, under the Language row when the stored language isn't
         * installed; {0} is its name ("Italian"). At most 64 bytes */
        I18N_FORMAT(s_absent, sizeof(s_absent), TR("{0} isn't installed: the screen shows English"),
                    absent_name(S.opt.lang));
        absent = s_absent;
    }
    ui_row_link(body, UI_ICON_LANGUAGES, ui_tone_blue(), TR("Language"), absent,
                i18n_language_name(S.catalog), UI_ICON_CHEVRON_RIGHT, pick_language, NULL);
    /* The region the panel follows, its own or the language's: the list says which. */
    ui_row_link(body, UI_ICON_MAP_PIN, ui_tone_blue(), TR("Region"),
                /* TR: Time and language, under the Region row */
                TR("How dates and numbers are written"),
                region_name(i18n_region_for(S.opt.region, S.opt.lang)), UI_ICON_CHEVRON_RIGHT,
                pick_region, NULL);
    char now[48];
    char number[24];
    char looks[80];
    const int64_t ms = S.be.epoch_ms(S.be.ctx);
    ui_date_time_text(ms > 0 ? ms : 0, now, sizeof(now));
    i18n_number(number, sizeof(number), ui_region(), 123456, 1); /* 12,345.6 in the region's way */
    snprintf(looks, sizeof(looks), "%s \xC2\xB7 %s", now, number);
    /* TR: Time and language, a row showing a date, a time and a number as the panel now writes
     * them */
    ui_row_value(body, TR("Looks like"), looks);
}

/* ---- first-time setup (I18N-012) ------------------------------------------------------------- */

bool ui_lang_offered(void)
{
    lang_refresh();
    return lang_count() > 1u;
}

static void lang_row_clicked(void *ctx)
{
    lang_chosen((int32_t)(intptr_t)ctx);
}

void ui_lang_setup_build(lv_obj_t *body, void (*rebuild)(void))
{
    s_rebuild = rebuild;
    lang_refresh();
    const int32_t current = lang_index();
    for (size_t i = 0; i < lang_count(); i++) {
        lv_obj_t *row = ui_row_link(body, NULL, ui_tone_blue(), lang_name(i), lang_detail(i), NULL,
                                    (int32_t)i == current ? UI_ICON_CHECK : NULL, lang_row_clicked,
                                    (void *)(intptr_t)i);
        if ((int32_t)i == current) {
            lv_obj_set_style_text_color(ui_find(row, "trail_label"), ui_tone_blue(), 0);
        }
    }
    const i18n_region_t *r = i18n_region_for(S.opt.region, S.opt.lang);
    ui_row_link(body, UI_ICON_MAP_PIN, ui_tone_blue(), TR("Region"),
                /* TR: first-time setup's language step, under the Region row's title */
                TR("How dates and numbers look; it follows the language"), region_name(r),
                UI_ICON_CHEVRON_RIGHT, pick_region, NULL);
}
