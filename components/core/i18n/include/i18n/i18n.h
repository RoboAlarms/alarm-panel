/*
 * i18n.h - translation lookup, plural rules, the {n} formatter and UTF-8 helpers
 * (docs/features/37-Globalization). Portable C11: no malloc, no globals, no ESP-IDF.
 *
 * English is the source text and has no table: a lookup without a catalog, or a text the
 * catalog lacks, gives the English back. A translation is never a printf format: the
 * formatter knows only {0}..{9} (I18N-003, I18N-008).
 */
#ifndef I18N_I18N_H
#define I18N_I18N_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- markers for tables the extractor must see (they change nothing at run time) ---- */
#define I18N_NOOP(s) (s)
#define I18N_NOOP_C(ctx, s) (s)
#define NOTR(s) (s) /* text deliberately not translated */

/* ---- the key ---- */

/* 32-bit FNV-1a over ctx, one 0x04 byte, then english; without a context only english. */
uint32_t i18n_hash(const char *ctx, const char *english);

/* ---- plurals (CLDR 48 cardinal rules, non-negative integers) ---- */

typedef enum {
    I18N_PLURALS_EN,    /* one n=1; other */
    I18N_PLURALS_OTHER, /* other only (zh, ja, ko, vi, th, id) */
    I18N_PLURALS_ES,    /* one n=1; many n!=0 and n%1000000==0; other */
    I18N_PLURALS_FR,    /* one n=0,1; many n!=0 and n%1000000==0; other */
    I18N_PLURALS_AR,
    I18N_PLURALS_HE
} i18n_plural_rule_t;

typedef enum { I18N_ZERO, I18N_ONE, I18N_TWO, I18N_FEW, I18N_MANY, I18N_OTHER } i18n_plural_cat_t;

i18n_plural_cat_t i18n_plural_category(i18n_plural_rule_t r, uint32_t n);
/* The rule's categories in CLDR order (zero, one, two, few, many, other): the order of a
 * plural entry's forms in a catalog and of msgstr[i] in a .po. Returns how many. */
size_t i18n_plural_forms(i18n_plural_rule_t r, i18n_plural_cat_t out[6]);
/* By the primary language subtag, case-insensitive; pt-PT differs from pt; unknown -> EN. */
i18n_plural_rule_t i18n_plural_rule(const char *locale);

/* ---- the catalog: constant data the generator emits ---- */

typedef struct {
    uint32_t hash; /* i18n_hash(ctx, English; the singular for a plural) */
    uint32_t text; /* byte offset into the catalog's string block, with the flags below */
} i18n_entry_t;

/* Flags in an entry's text (the lookup ignores them): I18N_ENTRY_WEB, an entry the Panel Manager's
 * pages use - the panel sends those to a browser as JSON (i18n_web_json), so its words take no
 * second copy in the image (I18N-031); I18N_ENTRY_PLURAL, an entry with plural forms. */
#define I18N_ENTRY_WEB    0x80000000u
#define I18N_ENTRY_PLURAL 0x40000000u
#define I18N_ENTRY_OFFSET 0x3FFFFFFFu

/* Entries sorted by hash ascending, no duplicates. A plural entry's forms are stored back to
 * back (each NUL-terminated) in i18n_plural_forms() order. preview=1 marks a draft language
 * (development builds only). */
typedef struct i18n_catalog {
    const char *locale;
    uint8_t plural_rule; /* an i18n_plural_rule_t */
    uint8_t preview;
    uint32_t count;
    const i18n_entry_t *entries;
    const char *strings;
} i18n_catalog_t;

/* c NULL (English) or not found: english itself. Never NULL for a non-NULL english. */
const char *i18n_text(const i18n_catalog_t *c, const char *ctx, const char *english);
/* c NULL or not found: n == 1 ? one : other. Found: the form for the catalog's rule and n. */
const char *i18n_plural(const i18n_catalog_t *c, const char *ctx, const char *one, const char *other,
                        uint32_t n);

/* The built-in catalogs. Exact BCP 47 match, case-insensitive; English and unknown: NULL. */
const i18n_catalog_t *i18n_catalog(const char *locale);
size_t i18n_catalog_count(void);
const i18n_catalog_t *i18n_catalog_at(size_t i);
/* Catalog c's Panel Manager entries as one JSON document, written a piece at a time so a web
 * server can send it in chunks without holding it (I18N-031):
 *   {"locale":"es-US","plural":["one","many","other"],"entries":{"0a1b2c3d":"Texto",
 *    "4e5f6a7b":["forma 1","forma 2","forma 3"]}}
 * A key is the entry's hash in 8 lower-case hex digits (the page hashes its English the same
 * way); plural forms in i18n_plural_forms() order. *pos starts at 0; each call writes whole pieces
 * into out (NUL-terminated) and returns their length, 0 once the document is complete (or c, pos
 * or out is NULL). With len of at least 128 the document is always whole; an entry too long for
 * len by itself is left out (the page shows its English). */
size_t i18n_web_json(const i18n_catalog_t *c, uint32_t *pos, char *out, size_t len);

/* A language the panel can show (I18N-015): English (any "en" tag, or "") or a built-in catalog. */
bool i18n_language_known(const char *lang);
/* The language's name in itself ("Español"; c NULL: "English"), for the language lists (I18N-012). */
const char *i18n_language_name(const i18n_catalog_t *c);

/* ---- the formatter ---- */

enum { I18N_ARG_STR = 1, I18N_ARG_INT = 2, I18N_ARG_UINT = 3 };

typedef struct {
    uint8_t kind;
    union {
        const char *s;
        int64_t i;
        uint64_t u;
    } v;
} i18n_arg_t;

i18n_arg_t i18n_arg_str(const char *s);
i18n_arg_t i18n_arg_int(int64_t i);
i18n_arg_t i18n_arg_uint(uint64_t u);

#define I18N_ARG(x)                                                                                \
    _Generic((x), char *: i18n_arg_str, const char *: i18n_arg_str, char: i18n_arg_int,           \
             signed char: i18n_arg_int, short: i18n_arg_int, int: i18n_arg_int,                   \
             long: i18n_arg_int, long long: i18n_arg_int, _Bool: i18n_arg_uint,                   \
             unsigned char: i18n_arg_uint, unsigned short: i18n_arg_uint,                         \
             unsigned int: i18n_arg_uint, unsigned long: i18n_arg_uint,                           \
             unsigned long long: i18n_arg_uint)(x)

/* {0}..{9} replaced (in any order, repeated); "{{" -> "{", "}}" -> "}"; a placeholder whose
 * index >= nargs stays as written; everything else (a lone "{", "%s", "%n") is copied as is.
 * Integers in plain decimal. Always NUL-terminates when len > 0; a full buffer is cut on a whole
 * UTF-8 character. Returns strlen(out). */
size_t i18n_format(char *out, size_t len, const char *pattern, const i18n_arg_t *args, size_t nargs);

#define I18N_NARGS_(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, N, ...) N
#define I18N_NARGS(...) I18N_NARGS_(__VA_ARGS__, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define I18N_MAP_1(a) I18N_ARG(a)
#define I18N_MAP_2(a, ...) I18N_ARG(a), I18N_MAP_1(__VA_ARGS__)
#define I18N_MAP_3(a, ...) I18N_ARG(a), I18N_MAP_2(__VA_ARGS__)
#define I18N_MAP_4(a, ...) I18N_ARG(a), I18N_MAP_3(__VA_ARGS__)
#define I18N_MAP_5(a, ...) I18N_ARG(a), I18N_MAP_4(__VA_ARGS__)
#define I18N_MAP_6(a, ...) I18N_ARG(a), I18N_MAP_5(__VA_ARGS__)
#define I18N_MAP_7(a, ...) I18N_ARG(a), I18N_MAP_6(__VA_ARGS__)
#define I18N_MAP_8(a, ...) I18N_ARG(a), I18N_MAP_7(__VA_ARGS__)
#define I18N_MAP_9(a, ...) I18N_ARG(a), I18N_MAP_8(__VA_ARGS__)
#define I18N_MAP_10(a, ...) I18N_ARG(a), I18N_MAP_9(__VA_ARGS__)
#define I18N_CAT_(a, b) a##b
#define I18N_CAT(a, b) I18N_CAT_(a, b)
#define I18N_MAP(...) I18N_CAT(I18N_MAP_, I18N_NARGS(__VA_ARGS__))(__VA_ARGS__)

/* I18N_FORMAT(out, len, pattern, a, b, ...): one to ten arguments, each wrapped by I18N_ARG. */
#define I18N_FORMAT(out, len, pattern, ...)                                                        \
    i18n_format((out), (len), (pattern), (const i18n_arg_t[]){I18N_MAP(__VA_ARGS__)},             \
                (size_t)I18N_NARGS(__VA_ARGS__))

/* A sentence with a name in it whose own words are never cut (I18N-010): pattern with {0} = name,
 * the name whole where the sentence fits len - 1 bytes, else shortened on a whole character
 * (trailing spaces trimmed) and ended with "…" (U+2026). The room left by the pattern's own words
 * is shared by every {0} it holds (a translation may repeat it); a name of more than 255 bytes is
 * shortened anyway. Other placeholders stay as written. Where the words alone don't fit, they are
 * cut as i18n_format cuts. Returns strlen(out). */
size_t i18n_format_fit(char *out, size_t len, const char *pattern, const char *name);

/* ---- UTF-8 (a bad byte counts as one character; nothing reads past the NUL or len;
 * a NULL string is "") ---- */

/* Bytes of s (at most max_bytes, at most strlen) to keep so the prefix ends on a whole character. */
size_t i18n_utf8_fit(const char *s, size_t max_bytes);
/* Like strlcpy but cut on a whole character; returns the bytes copied (without the NUL). */
size_t i18n_utf8_copy(char *dst, size_t dst_len, const char *src);
size_t i18n_utf8_chars(const char *s);
/* Strict: no overlong forms, no surrogates, nothing above U+10FFFF, no truncated sequence. */
bool i18n_utf8_valid(const char *s, size_t len);

/* A locale tag as a pack or a setting may hold: 2-15 characters of [A-Za-z0-9-]. NULL or "" is not. */
bool i18n_tag_valid(const char *tag);

/* Case-insensitive matching for names and searches (I18N-009): capitals of ASCII, Latin-1,
 * Latin Extended-A, Greek and Cyrillic fold to their small letters ("KÜCHE" is "küche", "İ" is
 * "i"); anything else compares as it is. a_len / b_len bound the bytes looked at. */
bool i18n_utf8_equal_fold(const char *a, size_t a_len, const char *b, size_t b_len);
/* needle somewhere in haystack, folded as above; an empty needle is found. */
bool i18n_utf8_contains_fold(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif

#endif /* I18N_I18N_H */
