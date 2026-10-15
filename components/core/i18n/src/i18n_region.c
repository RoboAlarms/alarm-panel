/* i18n_region.c - the curated regions (I18N-017). */
#include <stdbool.h>
#include <stddef.h>

#include "i18n/i18n.h"
#include "i18n/i18n_date.h"

/*
 * Sorted by tag. Sources (CLDR 48): the clock is supplement timeData "_preferred" (h/K = 12 h,
 * H/k = 24 h), the first day is weekData firstDay, the separators, minimum grouping digits and
 * the temperature unit come from the region's main language's latn number symbols and
 * measurementSystem data.
 * Columns: tag, hour24, first_day, decimal, group, min_grouping, celsius, name, day_first
 * (CLDR en-XX medium dates: "MMM d" in the US and Canada, "d MMM" elsewhere).
 * U+202F (narrow no-break space) is "\xE2\x80\xAF"; U+00A0 is "\xC2\xA0".
 */
/* TR: a country or region, as the lists of regions show it (the panel's Language and region, and
 * the Panel Manager). At most 47 bytes */
static const i18n_region_t k_regions[] = {
    {"AE", 0, 1, ".", ",", 1, 1, I18N_NOOP_C("region", "United Arab Emirates"), 1},
    {"AU", 0, 1, ".", ",", 1, 1, I18N_NOOP_C("region", "Australia"), 1},
    {"BR", 1, 0, ",", ".", 1, 1, I18N_NOOP_C("region", "Brazil"), 1},
    {"CA", 0, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "Canada"), 0},
    {"CN", 1, 1, ".", ",", 1, 1, I18N_NOOP_C("region", "China"), 1},
    {"DE", 1, 1, ",", ".", 1, 1, I18N_NOOP_C("region", "Germany"), 1},
    {"EG", 0, 6, ".", ",", 1, 1, I18N_NOOP_C("region", "Egypt"), 1},
    {"ES", 1, 1, ",", ".", 2, 1, I18N_NOOP_C("region", "Spain"), 1},
    {"FR", 1, 1, ",", "\xE2\x80\xAF", 1, 1, I18N_NOOP_C("region", "France"), 1},
    {"GB", 1, 1, ".", ",", 1, 1, I18N_NOOP_C("region", "United Kingdom"), 1},
    {"IL", 1, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "Israel"), 1},
    {"IN", 0, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "India"), 1}, /* CLDR groups Indian numbers 12,34,567; this table has threes only */
    {"IT", 1, 1, ",", ".", 2, 1, I18N_NOOP_C("region", "Italy"), 1},
    {"JP", 1, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "Japan"), 1},
    {"KR", 0, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "South Korea"), 1},
    {"MX", 0, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "Mexico"), 1},
    {"NL", 1, 1, ",", ".", 1, 1, I18N_NOOP_C("region", "Netherlands"), 1},
    {"PT", 1, 0, ",", "\xC2\xA0", 2, 1, I18N_NOOP_C("region", "Portugal"), 1},
    {"SA", 0, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "Saudi Arabia"), 1},
    {"TW", 0, 0, ".", ",", 1, 1, I18N_NOOP_C("region", "Taiwan"), 1},
    {"US", 0, 0, ".", ",", 1, 0, I18N_NOOP_C("region", "United States"), 0},
};

#define REGION_N (sizeof(k_regions) / sizeof(k_regions[0]))

static unsigned upper(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ? (unsigned)c - 32u : (unsigned)c;
}

static int is_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

const i18n_region_t *i18n_region(const char *tag)
{
    if (tag == NULL) {
        return NULL;
    }
    const unsigned char *p = (const unsigned char *)tag;
    /* A bare two-letter tag is a region; otherwise skip the language subtag, then look for the
     * first two-letter subtag (a script has four letters, a variant more). */
    if (!(is_alpha(p[0]) && p[0] != 0u && is_alpha(p[1]) && p[2] == 0u)) {
        while (*p != 0u && *p != '-' && *p != '_') {
            p++;
        }
        const unsigned char *found = NULL;
        while (*p != 0u && found == NULL) {
            p++; /* the separator */
            const unsigned char *s = p;
            while (*p != 0u && *p != '-' && *p != '_') {
                p++;
            }
            if ((size_t)(p - s) == 2u && is_alpha(s[0]) && is_alpha(s[1])) {
                found = s;
            }
        }
        if (found == NULL) {
            return NULL;
        }
        p = found;
    }
    for (size_t i = 0u; i < REGION_N; i++) {
        if (upper(p[0]) == (unsigned)(unsigned char)k_regions[i].tag[0] &&
            upper(p[1]) == (unsigned)(unsigned char)k_regions[i].tag[1]) {
            return &k_regions[i];
        }
    }
    return NULL;
}

size_t i18n_region_count(void)
{
    return REGION_N;
}

const i18n_region_t *i18n_region_at(size_t i)
{
    return i < REGION_N ? &k_regions[i] : NULL;
}

const i18n_region_t *i18n_region_for(const char *region, const char *lang)
{
    const i18n_region_t *r = i18n_region(region);
    if (r == NULL) {
        r = i18n_region(lang);
    }
    return r != NULL ? r : i18n_region("US");
}

bool i18n_clock_24h(uint8_t clock, const i18n_region_t *r)
{
    if (clock == I18N_CLOCK_24H) {
        return true;
    }
    if (clock == I18N_CLOCK_12H) {
        return false;
    }
    return r != NULL && r->hour24 != 0u;
}
