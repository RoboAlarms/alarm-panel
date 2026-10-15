/* i18n.c - the key hash, catalog lookup and the built-in catalog table (I18N-001, I18N-002,
 * I18N-004). */
#include <string.h>

#include "i18n/i18n.h"
#include "i18n_builtin.h"

uint32_t i18n_hash(const char *ctx, const char *english)
{
    uint32_t h = 0x811C9DC5u;
    if (ctx != NULL && ctx[0] != '\0') {
        for (const unsigned char *p = (const unsigned char *)ctx; *p != 0u; p++) {
            h = (h ^ (uint32_t)*p) * 0x01000193u;
        }
        h = (h ^ 0x04u) * 0x01000193u;
    }
    if (english != NULL) {
        for (const unsigned char *p = (const unsigned char *)english; *p != 0u; p++) {
            h = (h ^ (uint32_t)*p) * 0x01000193u;
        }
    }
    return h;
}

static const char *find(const i18n_catalog_t *c, const char *ctx, const char *english)
{
    if (c == NULL || c->entries == NULL || c->strings == NULL || english == NULL) {
        return NULL;
    }
    uint32_t h = i18n_hash(ctx, english);
    uint32_t lo = 0u;
    uint32_t hi = c->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t v = c->entries[mid].hash;
        if (v == h) {
            return c->strings + (c->entries[mid].text & I18N_ENTRY_OFFSET);
        }
        if (v < h) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const char *i18n_text(const i18n_catalog_t *c, const char *ctx, const char *english)
{
    const char *t = find(c, ctx, english);
    return t != NULL ? t : english;
}

const char *i18n_plural(const i18n_catalog_t *c, const char *ctx, const char *one, const char *other,
                        uint32_t n)
{
    const char *t = find(c, ctx, one);
    if (t == NULL) {
        return n == 1u ? one : other;
    }
    i18n_plural_rule_t rule = (i18n_plural_rule_t)c->plural_rule;
    i18n_plural_cat_t forms[6];
    size_t count = i18n_plural_forms(rule, forms);
    i18n_plural_cat_t want = i18n_plural_category(rule, n);
    size_t idx = count - 1u;
    for (size_t i = 0u; i < count; i++) {
        if (forms[i] == want) {
            idx = i;
            break;
        }
    }
    for (size_t i = 0u; i < idx; i++) {
        t += strlen(t) + 1u;
    }
    return t;
}

/* ---- the Panel Manager's entries as JSON (I18N-031) ---- */

static const char HEX[] = "0123456789abcdef";

typedef struct {
    char *out;
    size_t len;
    size_t n;
    bool ok;
} jw_t;

static void jw_raw(jw_t *w, const char *s)
{
    for (; w->ok && *s != '\0'; s++) {
        if (w->n + 1u >= w->len) {
            w->ok = false;
            return;
        }
        w->out[w->n++] = *s;
    }
}

static void jw_string(jw_t *w, const char *s)
{
    jw_raw(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; w->ok && *p != 0u; p++) {
        char esc[7] = {0};
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*p;
        } else if (*p < 0x20u) {
            memcpy(esc, "\\u00", 4u);
            esc[4] = HEX[*p >> 4];
            esc[5] = HEX[*p & 0x0Fu];
        } else {
            esc[0] = (char)*p; /* UTF-8 passes through */
        }
        jw_raw(w, esc);
    }
    jw_raw(w, "\"");
}

static void jw_entry(jw_t *w, const i18n_catalog_t *c, const i18n_entry_t *e, bool first)
{
    char key[14];
    size_t k = 0u;
    if (!first) {
        key[k++] = ',';
    }
    key[k++] = '"';
    for (unsigned shift = 32u; shift > 0u; shift -= 4u) {
        key[k++] = HEX[(e->hash >> (shift - 4u)) & 0x0Fu];
    }
    key[k++] = '"';
    key[k++] = ':';
    key[k] = '\0';
    jw_raw(w, key);
    const char *text = c->strings + (e->text & I18N_ENTRY_OFFSET);
    if ((e->text & I18N_ENTRY_PLURAL) == 0u) {
        jw_string(w, text);
        return;
    }
    i18n_plural_cat_t cats[6];
    const size_t forms = i18n_plural_forms((i18n_plural_rule_t)c->plural_rule, cats);
    jw_raw(w, "[");
    for (size_t i = 0u; i < forms; i++) {
        if (i > 0u) {
            jw_raw(w, ",");
        }
        jw_string(w, text);
        text += strlen(text) + 1u;
    }
    jw_raw(w, "]");
}

#define WEB_WROTE 0x80000000u /* in *pos: an entry is out, so the next one starts with a comma */

size_t i18n_web_json(const i18n_catalog_t *c, uint32_t *pos, char *out, size_t len)
{
    if (c == NULL || pos == NULL || out == NULL || len == 0u) {
        return 0u;
    }
    jw_t w = {out, len, 0u, true};
    uint32_t step = *pos & ~WEB_WROTE; /* 0 the start, 1..count the entries, count + 1 the end */
    bool wrote = (*pos & WEB_WROTE) != 0u;
    const uint32_t count = c->entries != NULL && c->strings != NULL ? c->count : 0u;
    if (step == 0u) {
        static const char *const NAMES[] = {"zero", "one", "two", "few", "many", "other"};
        i18n_plural_cat_t cats[6];
        const size_t n = i18n_plural_forms((i18n_plural_rule_t)c->plural_rule, cats);
        jw_raw(&w, "{\"locale\":");
        jw_string(&w, c->locale != NULL ? c->locale : "");
        jw_raw(&w, ",\"plural\":[");
        for (size_t i = 0u; i < n; i++) {
            if (i > 0u) {
                jw_raw(&w, ",");
            }
            jw_string(&w, NAMES[cats[i]]);
        }
        jw_raw(&w, "],\"entries\":{");
        if (!w.ok) {
            out[0] = '\0';
            return 0u; /* len can't hold even the start */
        }
        step = 1u;
    }
    while (step <= count) {
        const i18n_entry_t *e = &c->entries[step - 1u];
        if ((e->text & I18N_ENTRY_WEB) != 0u) {
            const size_t before = w.n;
            jw_entry(&w, c, e, !wrote);
            if (w.ok) {
                wrote = true;
            } else if (before > 0u) {
                w.n = before; /* the next call starts with it */
                break;
            } else {
                w.n = 0u; /* too long even alone: left out */
                w.ok = true;
            }
        }
        step++;
    }
    if (step == count + 1u) {
        const size_t before = w.n;
        jw_raw(&w, "}}");
        if (w.ok) {
            step = count + 2u;
        } else {
            w.n = before;
        }
    }
    *pos = step | (wrote ? WEB_WROTE : 0u);
    out[w.n] = '\0';
    return w.n;
}

static bool same_locale(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a;
        char y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
}

const i18n_catalog_t *i18n_catalog(const char *locale)
{
    if (locale == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < i18n_builtin_count; i++) {
        if (i18n_builtin[i] != NULL && same_locale(i18n_builtin[i]->locale, locale)) {
            return i18n_builtin[i];
        }
    }
    return NULL;
}

size_t i18n_catalog_count(void)
{
    return i18n_builtin_count;
}

const i18n_catalog_t *i18n_catalog_at(size_t i)
{
    return i < i18n_builtin_count ? i18n_builtin[i] : NULL;
}

/* A built-in catalog is a variant of English, the source (en-GB); the test languages' private
 * regions (en-XA) are not. */
static bool english_variant_built_in(void)
{
    for (size_t i = 0u; i < i18n_builtin_count; i++) {
        const char *l = i18n_builtin[i] != NULL ? i18n_builtin[i]->locale : NULL;
        if (l != NULL && (l[0] == 'e' || l[0] == 'E') && (l[1] == 'n' || l[1] == 'N') &&
            (l[2] == '-' || l[2] == '_') && l[3] != 'X' && l[3] != 'x') {
            return true;
        }
    }
    return false;
}

const char *i18n_language_name(const i18n_catalog_t *c)
{
    if (c == NULL && english_variant_built_in()) {
        return "English (US)"; /* beside "English (UK)", each named in itself */
    }
    /* TR: the name of this language, written in it ("Español", "Français", "English (UK)"), as
     * the lists of languages show it on the panel and in the Panel Manager. At most 31 bytes */
    return i18n_text(c, "this language", "English");
}

bool i18n_language_known(const char *lang)
{
    if (lang == NULL || lang[0] == '\0') {
        return true;
    }
    const bool en = (lang[0] == 'e' || lang[0] == 'E') && (lang[1] == 'n' || lang[1] == 'N') &&
                    (lang[2] == '\0' || lang[2] == '-' || lang[2] == '_');
    return en || i18n_catalog(lang) != NULL;
}
