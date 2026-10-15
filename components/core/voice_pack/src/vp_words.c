/*
 * vp_words.c - "Use zone name" and the word lists of the spoken-name editor (docs/features/08
 * voice-first-build.md D03). See include/voice_pack/vp_words.h.
 */
#include "voice_pack/vp_words.h"

#include <string.h>

/* ---- Characters ---- */
static unsigned lc(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned)c + 32u : (unsigned)c;
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* A word character: ASCII letters and digits, and every non-ASCII byte (it never matches, so it is
 * reported as unmatched rather than silently dropped). Everything else separates words. */
static bool is_wc(unsigned char c)
{
    return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80u;
}

/* ---- Use zone name ---- */
typedef struct {
    vp_name_match_t *out;
    const vp_pack_t *pack;
} match_t;

static void emit(match_t *m, uint16_t id)
{
    vp_name_match_t *o = m->out;
    if (o->entries < 0xFFFFu) {
        o->entries++;
    }
    if (o->count < VP_RECIPE_MAX) {
        o->ids[o->count++] = id;
    }
}

static void unmatched(match_t *m, size_t start, size_t len)
{
    vp_name_match_t *o = m->out;
    if (o->unmatched_total < 0xFFFFu) {
        o->unmatched_total++;
    }
    if (o->span_count < VP_MATCH_SPANS_MAX) {
        o->span[o->span_count].start = (uint16_t)start;
        o->span[o->span_count].length = (uint16_t)len;
        o->span_count++;
    }
}

/* If the words of `text` equal the name's words from `pos`, the offset after the last of them,
 * else 0. A phrase ends where a name word ends, never inside one. */
static size_t match_phrase(const char *text, const char *name, size_t nlen, size_t pos)
{
    const unsigned char *t = (const unsigned char *)text;
    const unsigned char *n = (const unsigned char *)name;
    size_t p = pos;
    bool any = false;
    for (;;) {
        while (*t != 0u && !is_wc(*t)) {
            t++;
        }
        if (*t == 0u) {
            break;
        }
        any = true;
        while (is_wc(*t)) {
            if (p >= nlen || !is_wc(n[p]) || lc(*t) != lc(n[p])) {
                return 0;
            }
            t++;
            p++;
        }
        if (p < nlen && is_wc(n[p])) {
            return 0; /* the name's word goes on */
        }
        size_t end = p;
        while (*t != 0u && !is_wc(*t)) {
            t++;
        }
        if (*t == 0u) {
            return any ? end : 0;
        }
        while (p < nlen && !is_wc(n[p])) {
            p++;
        }
        if (p >= nlen) {
            return 0; /* the phrase has more words than the name has left */
        }
    }
    return 0;
}

/* The clip ID of the number entry with script key "number.<value>", or 0. */
static uint16_t number_id(const vp_pack_t *pack, unsigned value)
{
    char key[16] = "number.";
    char digits[8];
    size_t nd = 0;
    unsigned v = value;
    do {
        digits[nd++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && nd < sizeof digits);
    size_t k = 7;
    while (nd > 0u) {
        key[k++] = digits[--nd];
    }
    key[k] = '\0';
    uint16_t cursor = 0;
    vp_clip_t c;
    while (vp_next_in_category(pack, (uint8_t)VP_CAT_NUMBER, &cursor, &c)) {
        if (strcmp(c.key, key) == 0) {
            return c.id;
        }
    }
    return 0;
}

/* A digit run of `len` digits at name+pos into number values (at most 4); the count, or 0 when
 * the run is not readable. */
static size_t number_parts(const char *run, size_t len, unsigned parts[4])
{
    if (len > 3u) {
        return 0;
    }
    if (len > 1u && run[0] == '0') { /* leading zero: digit by digit */
        for (size_t i = 0; i < len; i++) {
            parts[i] = (unsigned)(run[i] - '0');
        }
        return len;
    }
    unsigned v = 0;
    for (size_t i = 0; i < len; i++) {
        v = v * 10u + (unsigned)(run[i] - '0');
    }
    size_t n = 0;
    if (v == 0u) {
        parts[n++] = 0;
        return n;
    }
    if (v >= 100u) {
        parts[n++] = v / 100u;
        parts[n++] = 100u;
        v %= 100u;
    }
    if (v > 20u) {
        parts[n++] = (v / 10u) * 10u;
        if (v % 10u != 0u) {
            parts[n++] = v % 10u;
        }
    } else if (v > 0u) {
        parts[n++] = v;
    }
    return n;
}

static void match_digits(match_t *m, const char *name, size_t pos, size_t len)
{
    unsigned parts[4];
    uint16_t ids[4];
    size_t n = number_parts(name + pos, len, parts);
    bool ok = n > 0u;
    for (size_t i = 0; ok && i < n; i++) {
        ids[i] = number_id(m->pack, parts[i]);
        ok = ids[i] != 0u;
    }
    if (!ok) {
        unmatched(m, pos, len);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        emit(m, ids[i]);
    }
}

bool vp_match_name(const vp_pack_t *pack, const char *name, vp_name_match_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->result = VP_MATCH_NO_WORDS;
    if (pack == NULL || pack->base == NULL || name == NULL) {
        return false;
    }
    size_t nlen = 0;
    while (name[nlen] != '\0' && nlen <= VP_NAME_SCAN_MAX) {
        nlen++;
    }
    if (nlen > VP_NAME_SCAN_MAX) {
        out->result = VP_MATCH_TOO_MANY;
        return false;
    }

    match_t m = {out, pack};
    size_t words = 0;
    size_t pos = 0;
    for (;;) {
        while (pos < nlen && !is_wc((unsigned char)name[pos])) {
            pos++;
        }
        if (pos >= nlen) {
            break;
        }
        size_t end = pos;
        bool digits_only = true;
        while (end < nlen && is_wc((unsigned char)name[end])) {
            digits_only = digits_only && is_digit((unsigned char)name[end]);
            end++;
        }
        words++;
        if (digits_only) {
            match_digits(&m, name, pos, end - pos);
            pos = end;
            continue;
        }
        size_t best_end = 0;
        uint16_t best_id = 0;
        uint16_t cursor = 0;
        vp_clip_t c;
        while (vp_next_in_category(pack, 0, &cursor, &c)) {
            size_t e = match_phrase(c.text, name, nlen, pos);
            if (e > best_end) { /* longest wins; the lowest ID wins a tie */
                best_end = e;
                best_id = c.id;
            }
        }
        if (best_end > 0u) {
            emit(&m, best_id);
            pos = best_end;
        } else {
            unmatched(&m, pos, end - pos);
            pos = end;
        }
    }

    if (words == 0u) {
        out->result = VP_MATCH_NO_WORDS;
    } else if (out->unmatched_total > 0u) {
        out->result = VP_MATCH_UNMATCHED;
    } else if (out->entries > VP_RECIPE_MAX) {
        out->result = VP_MATCH_TOO_MANY;
    } else {
        vp_recipe_check_t chk;
        if (vp_check_recipe(pack, out->ids, out->count, &chk)) {
            out->result = VP_MATCH_OK;
        } else {
            out->result = chk.verdict == VP_RECIPE_TOO_LONG ? VP_MATCH_TOO_LONG : VP_MATCH_UNMATCHED;
        }
    }
    return out->result == VP_MATCH_OK;
}

/* ---- Word lists ---- */
/* Case-insensitive ASCII order of two display texts. */
static int cmp_ci(const char *a, const char *b)
{
    for (;;) {
        unsigned ca = lc((unsigned char)*a);
        unsigned cb = lc((unsigned char)*b);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        if (ca == 0u) {
            return 0;
        }
        a++;
        b++;
    }
}

/* `prefix` (plen bytes) starts the text or any word of it, case-insensitively. */
static bool prefix_at(const char *text, const char *prefix, size_t plen)
{
    for (size_t i = 0; i < plen; i++) {
        if (text[i] == '\0' || lc((unsigned char)text[i]) != lc((unsigned char)prefix[i])) {
            return false;
        }
    }
    return true;
}

static bool word_matches(const char *text, const char *prefix, size_t plen)
{
    if (plen == 0u) {
        return true;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        bool start = is_wc((unsigned char)text[i]) && (i == 0u || !is_wc((unsigned char)text[i - 1u]));
        if ((i == 0u || start) && prefix_at(text + i, prefix, plen)) {
            return true;
        }
    }
    return false;
}

bool vp_list_words(const vp_pack_t *pack, uint8_t category, const char *prefix, uint16_t page,
                   vp_word_page_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->page = page;
    if (pack == NULL || pack->base == NULL) {
        return false;
    }
    if (category != 0u && !vp_category_selectable(category)) {
        return true; /* status and walk clips never appear: an empty list */
    }
    if (prefix == NULL) {
        prefix = "";
    }
    while (*prefix == ' ' || *prefix == '\t') {
        prefix++;
    }
    size_t plen = strlen(prefix);
    while (plen > 0u && (prefix[plen - 1u] == ' ' || prefix[plen - 1u] == '\t')) {
        plen--;
    }

    /* The total. */
    uint32_t total = 0;
    uint16_t cursor = 0;
    vp_clip_t c;
    while (vp_next_in_category(pack, category, &cursor, &c)) {
        if (word_matches(c.text, prefix, plen)) {
            total++;
        }
    }
    out->total = (uint16_t)total;
    out->pages = (uint16_t)((total + VP_WORDS_PAGE - 1u) / VP_WORDS_PAGE);

    /* Repeated "the next one after the last": the order is (text, ID), so a page needs no memory
     * but its output. */
    uint32_t first = (uint32_t)page * VP_WORDS_PAGE;
    uint32_t rank = 0;
    bool have_last = false;
    vp_clip_t last = {0};
    while (rank < total && rank < first + VP_WORDS_PAGE) {
        bool have_best = false;
        vp_clip_t best = {0};
        cursor = 0;
        while (vp_next_in_category(pack, category, &cursor, &c)) {
            if (!word_matches(c.text, prefix, plen)) {
                continue;
            }
            if (have_last) {
                int d = cmp_ci(c.text, last.text);
                if (d < 0 || (d == 0 && c.id <= last.id)) {
                    continue;
                }
            }
            if (have_best) {
                int d = cmp_ci(c.text, best.text);
                if (d > 0 || (d == 0 && c.id > best.id)) {
                    continue;
                }
            }
            best = c;
            have_best = true;
        }
        if (!have_best) {
            break; /* never: total counted the same clips */
        }
        if (rank >= first) {
            out->ids[out->count++] = best.id;
        }
        last = best;
        have_last = true;
        rank++;
    }
    return true;
}
