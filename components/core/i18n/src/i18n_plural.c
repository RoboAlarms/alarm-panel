/* i18n_plural.c - CLDR 48 cardinal plural rules for non-negative integers (I18N-004). */
#include <string.h>

#include "i18n/i18n.h"

i18n_plural_cat_t i18n_plural_category(i18n_plural_rule_t r, uint32_t n)
{
    switch (r) {
    case I18N_PLURALS_OTHER:
        return I18N_OTHER;
    case I18N_PLURALS_ES:
        if (n == 1u) {
            return I18N_ONE;
        }
        return (n != 0u && n % 1000000u == 0u) ? I18N_MANY : I18N_OTHER;
    case I18N_PLURALS_FR:
        if (n == 0u || n == 1u) {
            return I18N_ONE;
        }
        return (n % 1000000u == 0u) ? I18N_MANY : I18N_OTHER;
    case I18N_PLURALS_AR: {
        if (n == 0u) {
            return I18N_ZERO;
        }
        if (n == 1u) {
            return I18N_ONE;
        }
        if (n == 2u) {
            return I18N_TWO;
        }
        uint32_t m = n % 100u;
        if (m >= 3u && m <= 10u) {
            return I18N_FEW;
        }
        return (m >= 11u) ? I18N_MANY : I18N_OTHER;
    }
    case I18N_PLURALS_HE:
        if (n == 1u) {
            return I18N_ONE;
        }
        return (n == 2u) ? I18N_TWO : I18N_OTHER;
    case I18N_PLURALS_EN:
    default:
        return (n == 1u) ? I18N_ONE : I18N_OTHER;
    }
}

size_t i18n_plural_forms(i18n_plural_rule_t r, i18n_plural_cat_t out[6])
{
    switch (r) {
    case I18N_PLURALS_OTHER:
        out[0] = I18N_OTHER;
        return 1u;
    case I18N_PLURALS_ES:
    case I18N_PLURALS_FR:
        out[0] = I18N_ONE;
        out[1] = I18N_MANY;
        out[2] = I18N_OTHER;
        return 3u;
    case I18N_PLURALS_AR:
        out[0] = I18N_ZERO;
        out[1] = I18N_ONE;
        out[2] = I18N_TWO;
        out[3] = I18N_FEW;
        out[4] = I18N_MANY;
        out[5] = I18N_OTHER;
        return 6u;
    case I18N_PLURALS_HE:
        out[0] = I18N_ONE;
        out[1] = I18N_TWO;
        out[2] = I18N_OTHER;
        return 3u;
    case I18N_PLURALS_EN:
    default:
        out[0] = I18N_ONE;
        out[1] = I18N_OTHER;
        return 2u;
    }
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* True when the tag's primary subtag (up to '-' or '_') equals `lang`. */
static bool primary_is(const char *tag, const char *lang)
{
    size_t i = 0u;
    for (; lang[i] != '\0'; i++) {
        if (lower(tag[i]) != lang[i]) {
            return false;
        }
    }
    return tag[i] == '\0' || tag[i] == '-' || tag[i] == '_';
}

static bool region_is_pt(const char *tag)
{
    const char *r = tag;
    while (*r != '\0' && *r != '-' && *r != '_') {
        r++;
    }
    if (*r == '\0') {
        return false;
    }
    r++;
    return lower(r[0]) == 'p' && lower(r[1]) == 't' && (r[2] == '\0' || r[2] == '-' || r[2] == '_');
}

i18n_plural_rule_t i18n_plural_rule(const char *locale)
{
    static const char *const other[] = {"zh", "ja", "ko", "vi", "th", "id"};
    static const char *const es[] = {"es", "it", "ca"};
    if (locale == NULL) {
        return I18N_PLURALS_EN;
    }
    for (size_t i = 0u; i < sizeof(other) / sizeof(other[0]); i++) {
        if (primary_is(locale, other[i])) {
            return I18N_PLURALS_OTHER;
        }
    }
    for (size_t i = 0u; i < sizeof(es) / sizeof(es[0]); i++) {
        if (primary_is(locale, es[i])) {
            return I18N_PLURALS_ES;
        }
    }
    if (primary_is(locale, "pt")) {
        return region_is_pt(locale) ? I18N_PLURALS_ES : I18N_PLURALS_FR;
    }
    if (primary_is(locale, "fr")) {
        return I18N_PLURALS_FR;
    }
    if (primary_is(locale, "ar")) {
        return I18N_PLURALS_AR;
    }
    if (primary_is(locale, "he")) {
        return I18N_PLURALS_HE;
    }
    return I18N_PLURALS_EN;
}
