/* i18n_date.c - the date/time pattern formatter and the number formatter (I18N-016, I18N-017). */
#include <stdbool.h>
#include <string.h>

#include "i18n/i18n_date.h"
#include "i18n_internal.h"

/* English is the source; the catalog holds the other languages' words. */
static const char *const k_months[12] = {
    I18N_NOOP_C("month", "January"),   I18N_NOOP_C("month", "February"),
    I18N_NOOP_C("month", "March"),     I18N_NOOP_C("month", "April"),
    I18N_NOOP_C("month", "May"),       I18N_NOOP_C("month", "June"),
    I18N_NOOP_C("month", "July"),      I18N_NOOP_C("month", "August"),
    I18N_NOOP_C("month", "September"), I18N_NOOP_C("month", "October"),
    I18N_NOOP_C("month", "November"),  I18N_NOOP_C("month", "December"),
};

static const char *const k_months_short[12] = {
    I18N_NOOP_C("month short", "Jan"), I18N_NOOP_C("month short", "Feb"),
    I18N_NOOP_C("month short", "Mar"), I18N_NOOP_C("month short", "Apr"),
    I18N_NOOP_C("month short", "May"), I18N_NOOP_C("month short", "Jun"),
    I18N_NOOP_C("month short", "Jul"), I18N_NOOP_C("month short", "Aug"),
    I18N_NOOP_C("month short", "Sep"), I18N_NOOP_C("month short", "Oct"),
    I18N_NOOP_C("month short", "Nov"), I18N_NOOP_C("month short", "Dec"),
};

static const char *const k_weekdays[7] = {
    I18N_NOOP_C("weekday", "Sunday"),   I18N_NOOP_C("weekday", "Monday"),
    I18N_NOOP_C("weekday", "Tuesday"),  I18N_NOOP_C("weekday", "Wednesday"),
    I18N_NOOP_C("weekday", "Thursday"), I18N_NOOP_C("weekday", "Friday"),
    I18N_NOOP_C("weekday", "Saturday"),
};

static const char *const k_weekdays_short[7] = {
    I18N_NOOP_C("weekday short", "Sun"), I18N_NOOP_C("weekday short", "Mon"),
    I18N_NOOP_C("weekday short", "Tue"), I18N_NOOP_C("weekday short", "Wed"),
    I18N_NOOP_C("weekday short", "Thu"), I18N_NOOP_C("weekday short", "Fri"),
    I18N_NOOP_C("weekday short", "Sat"),
};

static const char *const k_day_periods[2] = {
    I18N_NOOP_C("day period", "AM"),
    I18N_NOOP_C("day period", "PM"),
};

typedef struct {
    char *out;
    size_t cap; /* bytes available for text (len - 1) */
    size_t pos;
    int full;
} sink_t;

/* Append s[0..n), only whole characters when it does not fit; the first chunk that does not fit
 * ends the output. */
static void put(sink_t *k, const char *s, size_t n)
{
    if (k->full) {
        return;
    }
    size_t room = k->cap - k->pos;
    if (n > room) {
        const unsigned char *p = (const unsigned char *)s;
        size_t i = 0u;
        while (i < n && p[i] != 0u) {
            size_t l = i18n_utf8_seq(p + i, 4u);
            if (l == 0u) {
                l = 1u;
            }
            if (i + l > room) {
                break;
            }
            i += l;
        }
        memcpy(k->out + k->pos, s, i);
        k->pos += i;
        k->full = 1;
        return;
    }
    memcpy(k->out + k->pos, s, n);
    k->pos += n;
}

static void put_str(sink_t *k, const char *s)
{
    put(k, s, strlen(s));
}

/* value in decimal, at least `width` digits (zero padded), with a leading '-' when negative. */
static void put_num(sink_t *k, int32_t value, unsigned width)
{
    char text[24];
    char digits[12];
    size_t n = 0u;
    size_t t = 0u;
    uint32_t mag = value < 0 ? (uint32_t)0 - (uint32_t)value : (uint32_t)value;
    do {
        digits[n++] = (char)('0' + (unsigned)(mag % 10u));
        mag /= 10u;
    } while (mag != 0u);
    if (value < 0) {
        text[t++] = '-';
    }
    for (size_t z = n; z < width && z < 10u; z++) {
        text[t++] = '0';
    }
    while (n > 0u) {
        text[t++] = digits[--n];
    }
    put(k, text, t);
}

/* A name from a table (through the catalog); out of range: the number (numeric_base + index) or
 * nothing. */
static void put_name(sink_t *k, const i18n_catalog_t *c, const char *ctx, const char *const *table,
                     unsigned index, unsigned count, int numeric, unsigned base)
{
    if (index < count) {
        put_str(k, i18n_text(c, ctx, table[index]));
    } else if (numeric) {
        put_num(k, (int32_t)(index + base), 1u);
    }
}

/* The first letter of s as a capital, in place: ASCII, Latin-1 and Latin Extended-A (the
 * letters the text fonts carry); anything else is left as it is. A capital takes the same number
 * of bytes as its small letter in these ranges, so nothing moves. */
static void capitalize_first(char *s)
{
    unsigned char *u = (unsigned char *)s;
    if (u[0] == 0u) {
        return;
    }
    if (u[0] >= 'a' && u[0] <= 'z') {
        u[0] = (unsigned char)(u[0] - 0x20u);
        return;
    }
    if (u[1] < 0x80u || u[1] > 0xBFu) {
        return;
    }
    unsigned cp = ((unsigned)(u[0] & 0x1Fu) << 6) | (unsigned)(u[1] & 0x3Fu);
    if (u[0] == 0xC3u && cp >= 0xE0u && cp <= 0xFEu && cp != 0xF7u) {
        cp -= 0x20u; /* à..þ -> À..Þ, not ÷ */
    } else if (u[0] == 0xC4u || u[0] == 0xC5u) {
        /* Latin Extended-A pairs a capital with the small letter after it; between Ĺ and ň and
         * from Ź on, the capital is the odd one. ı, ĸ, ŉ and ſ have no pair here. */
        const bool odd_capital = (cp >= 0x139u && cp <= 0x148u) || (cp >= 0x179u && cp <= 0x17Eu);
        const bool small = odd_capital ? (cp % 2u == 0u) : (cp % 2u == 1u);
        if (!small || cp == 0x131u || cp == 0x138u || cp == 0x149u || cp == 0x17Fu) {
            return;
        }
        cp -= 1u;
    } else {
        return;
    }
    u[0] = (unsigned char)(0xC0u | (cp >> 6));
    u[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
}

size_t i18n_date(char *out, size_t len, const i18n_catalog_t *c, const char *pattern,
                 const i18n_civil_t *t)
{
    if (out == NULL || len == 0u) {
        return 0u;
    }
    sink_t k = {out, len - 1u, 0u, 0};
    bool name_first = false; /* the text opens with a weekday or month name */
    const unsigned char *p = (const unsigned char *)(pattern ? pattern : "");
    while (*p != 0u && !k.full) {
        unsigned ch = *p;
        if (ch == '\'') {
            if (p[1] == '\'') {
                put(&k, "'", 1u);
                p += 2;
                continue;
            }
            p++;
            while (*p != 0u && !k.full) {
                if (p[0] == '\'') {
                    if (p[1] == '\'') {
                        put(&k, "'", 1u);
                        p += 2;
                        continue;
                    }
                    p++;
                    break;
                }
                size_t l = i18n_utf8_seq(p, 4u);
                if (l == 0u) {
                    l = 1u;
                }
                put(&k, (const char *)p, l);
                p += l;
            }
            continue;
        }
        if (t != NULL && (ch == 'E' || ch == 'M' || ch == 'd' || ch == 'y' || ch == 'h' ||
                          ch == 'H' || ch == 'm' || ch == 's' || ch == 'a')) {
            unsigned run = 1u;
            while (p[run] == ch) {
                run++;
            }
            if (k.pos == 0u && (ch == 'E' || (ch == 'M' && run >= 3u))) {
                name_first = true;
            }
            switch (ch) {
            case 'E':
                put_name(&k, c, run >= 4u ? "weekday" : "weekday short",
                         run >= 4u ? k_weekdays : k_weekdays_short, t->weekday, 7u, 0, 0u);
                break;
            case 'M':
                if (run >= 3u) {
                    unsigned idx = (unsigned)t->month - 1u; /* month 0 wraps to a large index */
                    put_name(&k, c, run >= 4u ? "month" : "month short",
                             run >= 4u ? k_months : k_months_short, idx, 12u, 1, 1u);
                } else {
                    put_num(&k, (int32_t)t->month, run);
                }
                break;
            case 'd':
                put_num(&k, (int32_t)t->day, run >= 2u ? 2u : 1u);
                break;
            case 'y':
                if (run == 2u) {
                    int32_t y = (int32_t)t->year % 100;
                    put_num(&k, y < 0 ? -y : y, 2u);
                } else {
                    put_num(&k, (int32_t)t->year, run >= 4u ? 4u : 1u);
                }
                break;
            case 'h': {
                unsigned h = t->hour;
                if (h < 24u) {
                    h = h % 12u;
                    if (h == 0u) {
                        h = 12u;
                    }
                }
                put_num(&k, (int32_t)h, run >= 2u ? 2u : 1u);
                break;
            }
            case 'H':
                put_num(&k, (int32_t)t->hour, run >= 2u ? 2u : 1u);
                break;
            case 'm':
                put_num(&k, (int32_t)t->minute, run >= 2u ? 2u : 1u);
                break;
            case 's':
                put_num(&k, (int32_t)t->second, run >= 2u ? 2u : 1u);
                break;
            default: /* 'a' */
                if (t->hour < 24u) {
                    put_name(&k, c, "day period", k_day_periods, t->hour < 12u ? 0u : 1u, 2u, 0,
                             0u);
                }
                break;
            }
            p += run;
            continue;
        }
        size_t l = i18n_utf8_seq(p, 4u);
        if (l == 0u) {
            l = 1u;
        }
        put(&k, (const char *)p, l);
        p += l;
    }
    out[k.pos] = '\0';
    if (name_first) {
        capitalize_first(out); /* I18N-016: a date on its own starts with a capital */
    }
    return k.pos;
}

size_t i18n_number(char *out, size_t len, const i18n_region_t *r, int64_t value, uint8_t decimals)
{
    if (out == NULL || len == 0u) {
        return 0u;
    }
    const char *dec = r ? r->decimal : ".";
    const char *grp = r ? r->group : ",";
    unsigned min_group = r ? r->min_grouping : 1u;
    if (decimals > 19u) {
        decimals = 19u;
    }
    uint64_t mag = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    uint64_t scale = 1u;
    for (unsigned i = 0u; i < decimals; i++) {
        scale *= 10u;
    }
    uint64_t ip = mag / scale;
    uint64_t fp = mag % scale;

    char idigits[24];
    size_t in = 0u;
    do {
        idigits[in++] = (char)('0' + (unsigned)(ip % 10u));
        ip /= 10u;
    } while (ip != 0u);

    sink_t k = {out, len - 1u, 0u, 0};
    if (value < 0) {
        put(&k, "-", 1u);
    }
    int group = !(min_group >= 2u && in < 5u);
    for (size_t i = in; i > 0u; i--) {
        put(&k, &idigits[i - 1u], 1u);
        if (group && i > 1u && ((i - 1u) % 3u) == 0u) {
            put_str(&k, grp);
        }
    }
    if (decimals > 0u) {
        put_str(&k, dec);
        char fdigits[24];
        for (size_t i = decimals; i > 0u; i--) {
            fdigits[i - 1u] = (char)('0' + (unsigned)(fp % 10u));
            fp /= 10u;
        }
        put(&k, fdigits, decimals);
    }
    out[k.pos] = '\0';
    return k.pos;
}

/* Replaces the first `from` in buf (NUL-terminated, cap bytes) with `to`; false if absent or
 * it wouldn't fit. */
static bool swap_once(char *buf, size_t cap, const char *from, const char *to)
{
    char *at = strstr(buf, from);
    if (at == NULL) {
        return false;
    }
    const size_t fl = strlen(from);
    const size_t tl = strlen(to);
    const size_t rest = strlen(at + fl);
    if ((size_t)(at - buf) + tl + rest + 1u > cap) {
        return false;
    }
    memmove(at + tl, at + fl, rest + 1u);
    memcpy(at, to, tl);
    return true;
}

size_t i18n_pattern_day_first(char *out, size_t len, const char *pattern)
{
    if (out == NULL || len == 0u) {
        return 0u;
    }
    const size_t n = i18n_utf8_copy(out, len, pattern);
    if (n + 1u < len && pattern != NULL && pattern[n] != '\0') {
        return n; /* cut: leave it as it is */
    }
    /* the year after the day loses its comma, the day goes before the month, and a weekday
     * before the day loses its comma - the English of the United Kingdom */
    static const char *const RULES[][2] = {
        {"MMMM d, y", "d MMMM y"}, {"MMM d, y", "d MMM y"}, {"MMMM d", "d MMMM"},
        {"MMM d", "d MMM"},        {"EEEE, d", "EEEE d"},   {"EEE, d", "EEE d"},
    };
    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
        (void)swap_once(out, len, RULES[i][0], RULES[i][1]);
    }
    return strlen(out);
}

size_t i18n_date_in(char *out, size_t len, const i18n_catalog_t *c, const i18n_region_t *r,
                    const char *pattern, const i18n_civil_t *t)
{
    if (c == NULL && r != NULL && r->day_first != 0u && pattern != NULL) {
        char turned[96];
        i18n_pattern_day_first(turned, sizeof(turned), pattern);
        return i18n_date(out, len, c, turned, t);
    }
    return i18n_date(out, len, c, pattern, t);
}
