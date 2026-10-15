/*
 * i18n_date.h - regions, the date/time pattern formatter and the number formatter
 * (docs/features/37-Globalization, I18N-016, I18N-017). Portable C11: no malloc, no libc time
 * zones; the caller hands in the civil time it already worked out.
 *
 * The region supplies only the clock, the first day of the week, the number separators and the
 * temperature unit. Month and day names, AM/PM and the date patterns are catalog entries
 * (English is the source text), so a language owns its own words and word order.
 */
#ifndef I18N_I18N_DATE_H
#define I18N_I18N_DATE_H

#include <stddef.h>
#include <stdint.h>

#include "i18n/i18n.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t year;
    uint8_t month; /* 1-12 */
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday; /* 0 Sunday..6 */
} i18n_civil_t;

typedef struct {
    const char *tag;      /* ISO 3166 "US" */
    uint8_t hour24;       /* the region's default clock: 1 = 24 h */
    uint8_t first_day;    /* 0 Sunday, 1 Monday, 6 Saturday */
    char decimal[4];      /* UTF-8 */
    char group[4];        /* UTF-8 */
    uint8_t min_grouping; /* 1, or 2: no separator in 4-digit numbers */
    uint8_t celsius;      /* 1 = Celsius, 0 = Fahrenheit */
    const char *name;     /* in English ("Germany"); i18n_text(c, "region", name) in c's language */
    uint8_t day_first;    /* English dates put the day first here: "30 Sep" (United Kingdom) */
} i18n_region_t;

/* "US", or the region of a BCP 47 tag ("es-US"); case-insensitive; unknown -> NULL. */
const i18n_region_t *i18n_region(const char *tag);
size_t i18n_region_count(void);
const i18n_region_t *i18n_region_at(size_t i);
/* The region a panel formats with (I18N-016): the one chosen, else the language's ("es-US" ->
 * US), else the US. Never NULL. */
const i18n_region_t *i18n_region_for(const char *region, const char *lang);

/* The clock choice, a stored preference (panel_prefs "clockmode", ui_options_t.clock_mode):
 * never renumber. */
enum { I18N_CLOCK_AUTO = 0, I18N_CLOCK_12H = 1, I18N_CLOCK_24H = 2 };
/* 24 h or not (I18N-016): the choice, or with I18N_CLOCK_AUTO (and anything unknown) the
 * region's own clock; r NULL is the US, 12 h. */
bool i18n_clock_24h(uint8_t clock, const i18n_region_t *r);

/*
 * Pattern letters (a CLDR subset): EEEE weekday, EEE weekday short, MMMM month, MMM month short,
 * MM / M month number, dd / d day, y / yyyy year, h / hh 12-hour (12 for 0 and 12), H / HH 24-hour,
 * mm / m minute, ss / s second, a day period ("AM" / "PM"). 'text' is literal, '' an apostrophe;
 * any other character (UTF-8 too) is copied. A field out of range prints as a number or nothing,
 * never from outside a table. c NULL gives the English names. A result that opens with a weekday
 * or month name starts with a capital (CLDR's stand-alone form: "Sábado, 19 de septiembre"; a
 * name further on keeps the catalog's letter), so a date belongs on its own, not inside a
 * sentence. NUL-terminated; a full buffer is cut on a whole character; returns strlen(out).
 */
size_t i18n_date(char *out, size_t len, const i18n_catalog_t *c, const char *pattern,
                 const i18n_civil_t *t);

/*
 * The same, in the region's order (I18N-016). A language's catalog writes its own order into its
 * patterns; English has no catalog, so its US patterns ("MMM d") are turned around where the region
 * puts the day first ("d MMM": the United Kingdom, Germany - everywhere but the US and Canada).
 * r NULL is the US.
 */
size_t i18n_date_in(char *out, size_t len, const i18n_catalog_t *c, const i18n_region_t *r,
                    const char *pattern, const i18n_civil_t *t);
/* The pattern turned day first: "MMM d, h:mm a" -> "d MMM, h:mm a", "EEEE, MMMM d" -> "EEEE d
 * MMMM", "MMM d, y" -> "d MMM y". Other letters and literal text stay. Returns strlen(out). */
size_t i18n_pattern_day_first(char *out, size_t len, const char *pattern);

/* value scaled by 10^decimals (215, 1 -> "21.5" / "21,5"); thousands grouped in threes with the
 * region's separator (none below 10000 when min_grouping is 2); r NULL -> US; decimals over 19
 * are treated as 19. */
size_t i18n_number(char *out, size_t len, const i18n_region_t *r, int64_t value, uint8_t decimals);

#ifdef __cplusplus
}
#endif

#endif /* I18N_I18N_DATE_H */
