/* i18n_pack.c - the strict reader of a language pack's payload (I18N-040, I18N-043;
 * docs/features/37 language-packs.md "The pack file"). Reads nothing outside the bytes given,
 * allocates nothing; the entries are used in place, so the payload must be 4-byte aligned on a
 * little-endian machine. */
#include <stdint.h>
#include <string.h>

#include "i18n/i18n.h"
#include "i18n/i18n_pack.h"

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool little_endian(void)
{
    const uint32_t one = 1u;
    uint8_t b;
    memcpy(&b, &one, 1u);
    return b == 1u;
}

/* A NUL-padded field of n bytes: the text is the bytes before the first NUL, every byte after
 * it is 0. Returns the text length, or -1 without a NUL or with a stray byte after it. */
static int padded_len(const uint8_t *p, size_t n)
{
    size_t i = 0u;
    while (i < n && p[i] != 0u) {
        i++;
    }
    if (i == n) {
        return -1;
    }
    for (size_t j = i; j < n; j++) {
        if (p[j] != 0u) {
            return -1;
        }
    }
    return (int)i;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* [a, a+an) and [b, b+bn) share a byte (empty regions share none). */
static bool overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    if (an == 0u || bn == 0u) {
        return false;
    }
    return a < b + bn && b < a + an;
}

/* The NUL-terminated valid UTF-8 text at off in the block; returns its length + 1, or 0. */
static uint32_t text_ok(const char *block, uint32_t size, uint32_t off)
{
    if (off >= size) {
        return 0u;
    }
    const void *nul = memchr(block + off, 0, size - off);
    if (nul == NULL) {
        return 0u;
    }
    uint32_t n = (uint32_t)((const char *)nul - (block + off));
    if (!i18n_utf8_valid(block + off, n)) {
        return 0u;
    }
    return n + 1u;
}

static int role_known(const uint8_t *role)
{
    static const char *const roles[] = {"caption", "body", "label", "title", "headline"};
    for (size_t i = 0u; i < sizeof roles / sizeof roles[0]; i++) {
        if (strcmp((const char *)role, roles[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static i18n_pack_result_t check_fonts(const uint8_t *b, uint32_t len, uint32_t fo, uint32_t fs,
                                      uint64_t eo, uint64_t es, uint64_t so, uint64_t ss,
                                      i18n_pack_t *out)
{
    if (fs < 4u) {
        return I18N_PACK_ERR_FONTS;
    }
    uint32_t count = rd32(b + fo);
    if (count < 1u || count > I18N_PACK_MAX_FONTS) {
        return I18N_PACK_ERR_FONTS;
    }
    uint64_t table = 4u + 24u * (uint64_t)count;
    if (table > fs) {
        return I18N_PACK_ERR_FONTS;
    }
    unsigned seen = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        const uint8_t *r = b + fo + 4u + 24u * i;
        if (padded_len(r, 16u) < 1) {
            return I18N_PACK_ERR_FONTS;
        }
        int role = role_known(r);
        if (role < 0 || (seen & (1u << role)) != 0u) {
            return I18N_PACK_ERR_FONTS;
        }
        seen |= 1u << role;
        uint32_t off = rd32(r + 16);
        uint32_t size = rd32(r + 20);
        if (size == 0u || (off & 3u) != 0u || (uint64_t)off + size > len) {
            return I18N_PACK_ERR_FONTS;
        }
        if (overlap(off, size, 0u, I18N_PACK_HEADER_SIZE) || overlap(off, size, eo, es) ||
            overlap(off, size, so, ss) || overlap(off, size, fo, table)) {
            return I18N_PACK_ERR_FONTS;
        }
        out->fonts[i].role = (const char *)r;
        out->fonts[i].data = b + off;
        out->fonts[i].size = size;
    }
    out->font_count = (uint8_t)count;
    return I18N_PACK_OK;
}

i18n_pack_result_t i18n_pack_open(const void *payload, size_t len, i18n_pack_t *out)
{
    if (payload == NULL || out == NULL || !little_endian() || ((uintptr_t)payload & 3u) != 0u) {
        return I18N_PACK_ERR_ARG;
    }
#if SIZE_MAX > UINT32_MAX
    if (len > UINT32_MAX) {
        return I18N_PACK_ERR_ARG;
    }
#endif
    memset(out, 0, sizeof *out);
    if (len < I18N_PACK_HEADER_SIZE) {
        return I18N_PACK_ERR_SHORT;
    }
    const uint8_t *b = (const uint8_t *)payload;
    if (b[0] != 0x52u || b[1] != 0x4Cu || b[2] != 0x4Eu || b[3] != 0x47u) {
        return I18N_PACK_ERR_MAGIC;
    }
    if (rd16(b + 4) != 1u || rd16(b + 6) != I18N_PACK_HEADER_SIZE) {
        return I18N_PACK_ERR_FORMAT;
    }
    const uint32_t length = (uint32_t)len;
    if (rd32(b + 8) != length) {
        return I18N_PACK_ERR_HEADER;
    }
    int n = padded_len(b + 12, 16u);
    if (n < 0 || !i18n_tag_valid((const char *)(b + 12))) {
        return I18N_PACK_ERR_HEADER;
    }
    n = padded_len(b + 28, 32u);
    if (n < 1 || !i18n_utf8_valid((const char *)(b + 28), (size_t)n)) {
        return I18N_PACK_ERR_HEADER;
    }
    if ((unsigned)b[60] > 5u || ((unsigned)b[61] & ~I18N_PACK_FLAG_DRAFT) != 0u || !all_zero(b + 62, 2u) ||
        !all_zero(b + 112, 16u)) {
        return I18N_PACK_ERR_HEADER;
    }
    n = padded_len(b + 88, 16u);
    if (n < 1) {
        return I18N_PACK_ERR_HEADER;
    }
    for (int i = 0; i < n; i++) {
        if (b[88 + i] < 0x20u || b[88 + i] > 0x7Eu) {
            return I18N_PACK_ERR_HEADER;
        }
    }

    const uint32_t count = rd32(b + 64);
    const uint32_t eoff = rd32(b + 68);
    const uint32_t soff = rd32(b + 72);
    const uint32_t ssize = rd32(b + 76);
    const uint32_t foff = rd32(b + 104);
    const uint32_t fsize = rd32(b + 108);
    const uint64_t esize = 8u * (uint64_t)count;
    if ((eoff & 3u) != 0u || eoff < I18N_PACK_HEADER_SIZE || (uint64_t)eoff + esize > length ||
        soff < I18N_PACK_HEADER_SIZE || (uint64_t)soff + ssize > length ||
        overlap(eoff, esize, soff, ssize)) {
        return I18N_PACK_ERR_LAYOUT;
    }
    const bool fonts = foff != 0u || fsize != 0u;
    if (fonts && (foff < I18N_PACK_HEADER_SIZE || fsize == 0u || (uint64_t)foff + fsize > length ||
                  overlap(foff, fsize, eoff, esize) || overlap(foff, fsize, soff, ssize))) {
        return I18N_PACK_ERR_LAYOUT;
    }

    const i18n_plural_rule_t rule = (i18n_plural_rule_t)b[60];
    i18n_plural_cat_t cats[6];
    const size_t forms = i18n_plural_forms(rule, cats);
    const char *strings = (const char *)(b + soff);
    for (uint32_t i = 0u; i < count; i++) {
        const uint8_t *e = b + eoff + 8u * i;
        uint32_t hash = rd32(e);
        uint32_t text = rd32(e + 4);
        if (i > 0u && hash <= rd32(e - 8)) {
            return I18N_PACK_ERR_ENTRIES;
        }
        uint32_t off = text & I18N_ENTRY_OFFSET;
        size_t want = (text & I18N_ENTRY_PLURAL) != 0u ? forms : 1u;
        for (size_t f = 0u; f < want; f++) {
            uint32_t used = text_ok(strings, ssize, off);
            if (used == 0u) {
                return I18N_PACK_ERR_ENTRIES;
            }
            off += used;
        }
    }

    if (fonts) {
        i18n_pack_result_t r = check_fonts(b, length, foff, fsize, eoff, esize, soff, ssize, out);
        if (r != I18N_PACK_OK) {
            memset(out, 0, sizeof *out);
            return r;
        }
    }

    out->locale = (const char *)(b + 12);
    out->name = (const char *)(b + 28);
    out->version = (const char *)(b + 88);
    out->draft = (b[61] & I18N_PACK_FLAG_DRAFT) != 0u;
    out->template_fingerprint = rd32(b + 80);
    out->missing = rd32(b + 84);
    out->payload_length = length;
    out->catalog.locale = out->locale;
    out->catalog.plural_rule = b[60];
    out->catalog.preview = out->draft ? 1u : 0u;
    out->catalog.count = count;
    out->catalog.entries = (const i18n_entry_t *)(const void *)(b + eoff);
    out->catalog.strings = strings;
    return I18N_PACK_OK;
}

bool i18n_pack_matches(const i18n_pack_t *p)
{
    return p != NULL && p->template_fingerprint == i18n_template_fingerprint;
}
