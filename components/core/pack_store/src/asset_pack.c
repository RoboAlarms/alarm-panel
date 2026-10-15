/* asset_pack.c - the strict reader of an asset pack's payload, `RAST` format 1 (OPT-012;
 * docs/features/39 asset-packs.md "The pack file (format 1)"). Reads nothing outside the bytes
 * given, allocates nothing; the parts are used in place, so the payload must be 4-byte aligned on
 * a little-endian machine. */
#include <stdint.h>
#include <string.h>

#include "pack_store/asset_pack.h"

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

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* A 16-byte name field: 1..15 characters from '!' to '~' (printable ASCII without the space),
 * then NUL bytes to the end of the field. */
static bool name_ok(const uint8_t *p)
{
    size_t n = 0u;
    while (n < 16u && p[n] != 0u) {
        if (p[n] < 0x21u || p[n] > 0x7Eu) {
            return false;
        }
        n++;
    }
    return n >= 1u && n <= ASSET_PACK_NAME_MAX && all_zero(p + n, 16u - n);
}

/* v rounded up to a multiple of 4, in 64 bits so that nothing wraps */
static uint64_t align4(uint32_t v)
{
    return ((uint64_t)v + 3u) & ~(uint64_t)3u;
}

static asset_pack_result_t open_checked(const uint8_t *b, uint32_t length, asset_pack_t *out)
{
    if (length < ASSET_PACK_HEADER_SIZE) {
        return ASSET_PACK_ERR_SHORT;
    }
    if (b[0] != 0x52u || b[1] != 0x41u || b[2] != 0x53u || b[3] != 0x54u) {
        return ASSET_PACK_ERR_MAGIC;
    }
    if (rd16(b + 4) != ASSET_PACK_FORMAT || rd16(b + 6) != ASSET_PACK_HEADER_SIZE) {
        return ASSET_PACK_ERR_FORMAT;
    }
    if (rd32(b + 8) != length) {
        return ASSET_PACK_ERR_LENGTH;
    }
    const unsigned kind = (unsigned)b[12];
    if (kind < (unsigned)PS_KIND_LETTERS || kind > (unsigned)PS_KIND_WEB) {
        return ASSET_PACK_ERR_KIND;
    }
    const unsigned flags = (unsigned)b[13];
    if ((flags & ~ASSET_PACK_FLAG_DRAFT) != 0u) {
        return ASSET_PACK_ERR_FLAGS;
    }
    const uint32_t count = rd16(b + 14);
    if (count == 0u || count > ASSET_PACK_MAX_PARTS) {
        return ASSET_PACK_ERR_COUNT;
    }
    if (!name_ok(b + 16)) {
        return ASSET_PACK_ERR_NAME;
    }
    if (!name_ok(b + 32)) {
        return ASSET_PACK_ERR_VERSION;
    }
    if (rd32(b + 52) != ASSET_PACK_HEADER_SIZE) {
        return ASSET_PACK_ERR_PARTS_OFFSET;
    }
    if (!all_zero(b + 56, 72u)) {
        return ASSET_PACK_ERR_RESERVED;
    }
    uint32_t pos = ASSET_PACK_HEADER_SIZE + ASSET_PACK_PART_SIZE * count; /* at most 640 */
    if (pos > length) {
        return ASSET_PACK_ERR_SHORT;
    }

    for (uint32_t i = 0u; i < count; i++) {
        const uint8_t *r = b + ASSET_PACK_HEADER_SIZE + ASSET_PACK_PART_SIZE * i;
        if (!name_ok(r)) {
            return ASSET_PACK_ERR_PART_NAME;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (memcmp(r, b + ASSET_PACK_HEADER_SIZE + ASSET_PACK_PART_SIZE * j, 16u) == 0) {
                return ASSET_PACK_ERR_PART_NAME;
            }
        }
        const uint32_t type = rd32(r + 16);
        if (type < (uint32_t)ASSET_PART_FONT || type > (uint32_t)ASSET_PART_CERTS) {
            return ASSET_PACK_ERR_PART_TYPE;
        }
        const uint32_t off = rd32(r + 20);
        const uint32_t size = rd32(r + 24);
        if (size == 0u) {
            return ASSET_PACK_ERR_PART_SIZE;
        }
        if (rd32(r + 28) != 0u) {
            return ASSET_PACK_ERR_RESERVED;
        }
        if ((uint64_t)off != align4(pos) || (uint64_t)off + size > length) {
            return ASSET_PACK_ERR_LAYOUT;
        }
        if (!all_zero(b + pos, off - pos)) {
            return ASSET_PACK_ERR_PADDING;
        }
        out->parts[i].name = (const char *)r;
        out->parts[i].type = type;
        out->parts[i].data = b + off;
        out->parts[i].size = size;
        pos = off + size;
    }
    /* the payload ends at the last part's end rounded up to 4 */
    if ((uint64_t)length != align4(pos)) {
        return ASSET_PACK_ERR_LAYOUT;
    }
    if (!all_zero(b + pos, length - pos)) {
        return ASSET_PACK_ERR_PADDING;
    }

    out->payload = b;
    out->payload_length = length;
    out->kind = (uint8_t)kind;
    out->flags = (uint8_t)flags;
    out->draft = (flags & ASSET_PACK_FLAG_DRAFT) != 0u;
    out->name = (const char *)(b + 16);
    out->version = (const char *)(b + 32);
    out->match = rd32(b + 48);
    out->part_count = (uint16_t)count;
    return ASSET_PACK_OK;
}

asset_pack_result_t asset_pack_open(const void *payload, size_t len, asset_pack_t *out)
{
    if (out == NULL) {
        return ASSET_PACK_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    if (payload == NULL || !little_endian() || ((uintptr_t)payload & 3u) != 0u) {
        return ASSET_PACK_ERR_ARG;
    }
#if SIZE_MAX > UINT32_MAX
    if (len > UINT32_MAX) {
        return ASSET_PACK_ERR_ARG;
    }
#endif
    const asset_pack_result_t r = open_checked((const uint8_t *)payload, (uint32_t)len, out);
    if (r != ASSET_PACK_OK) {
        memset(out, 0, sizeof *out);
    }
    return r;
}

const char *asset_pack_result_name(asset_pack_result_t r)
{
    switch (r) {
    case ASSET_PACK_OK: return "ok";
    case ASSET_PACK_ERR_ARG: return "argument";
    case ASSET_PACK_ERR_SHORT: return "short";
    case ASSET_PACK_ERR_MAGIC: return "magic";
    case ASSET_PACK_ERR_FORMAT: return "format";
    case ASSET_PACK_ERR_LENGTH: return "length";
    case ASSET_PACK_ERR_KIND: return "kind";
    case ASSET_PACK_ERR_FLAGS: return "flags";
    case ASSET_PACK_ERR_COUNT: return "part count";
    case ASSET_PACK_ERR_NAME: return "name";
    case ASSET_PACK_ERR_VERSION: return "version";
    case ASSET_PACK_ERR_PARTS_OFFSET: return "parts offset";
    case ASSET_PACK_ERR_RESERVED: return "reserved";
    case ASSET_PACK_ERR_PART_NAME: return "part name";
    case ASSET_PACK_ERR_PART_TYPE: return "part type";
    case ASSET_PACK_ERR_PART_SIZE: return "part size";
    case ASSET_PACK_ERR_LAYOUT: return "layout";
    case ASSET_PACK_ERR_PADDING: return "padding";
    }
    return "unknown";
}

const asset_part_t *asset_pack_part(const asset_pack_t *p, uint16_t i)
{
    if (p == NULL || i >= p->part_count || i >= ASSET_PACK_MAX_PARTS) {
        return NULL;
    }
    return &p->parts[i];
}

const asset_part_t *asset_pack_find(const asset_pack_t *p, const char *name)
{
    if (p == NULL || name == NULL) {
        return NULL;
    }
    for (uint16_t i = 0u; i < p->part_count && i < ASSET_PACK_MAX_PARTS; i++) {
        if (p->parts[i].name != NULL && strcmp(p->parts[i].name, name) == 0) {
            return &p->parts[i];
        }
    }
    return NULL;
}

bool asset_pack_is_draft(const asset_pack_t *p)
{
    return p != NULL && p->draft;
}

bool asset_pack_made_for(const asset_pack_t *p, const char *version)
{
    if (p == NULL || p->version == NULL || version == NULL || version[0] == '\0') {
        return false;
    }
    return strcmp(p->version, version) == 0;
}

const char *asset_pack_store_name(uint8_t kind)
{
    switch (kind) {
    case PS_KIND_LETTERS: return "letters";
    case PS_KIND_PICTURE: return "saver";
    case PS_KIND_CERTS: return "certs";
    case PS_KIND_WEB: return "web";
    default: return NULL;
    }
}

/* ---- the kinds' rules ---- */

bool asset_jpeg_fits(const uint8_t *d, uint32_t n, uint32_t max_w, uint32_t max_h)
{
    if (d == NULL || n < 4u || d[0] != 0xFFu || d[1] != 0xD8u) {
        return false;
    }
    uint32_t i = 2u;
    while (i + 4u <= n) {
        if (d[i] != 0xFFu) {
            return false;
        }
        const uint32_t m = d[i + 1u];
        if (m == 0xFFu) { /* a fill byte */
            i++;
            continue;
        }
        if (m == 0x01u || (m >= 0xD0u && m <= 0xD7u)) { /* no length */
            i += 2u;
            continue;
        }
        const uint32_t len = ((uint32_t)d[i + 2u] << 8) | (uint32_t)d[i + 3u];
        if (len < 2u || len > n - i - 2u) {
            return false;
        }
        if (m == 0xC0u) {
            if (len < 8u) {
                return false;
            }
            const uint32_t h = ((uint32_t)d[i + 5u] << 8) | (uint32_t)d[i + 6u];
            const uint32_t w = ((uint32_t)d[i + 7u] << 8) | (uint32_t)d[i + 8u];
            return w > 0u && h > 0u && w <= max_w && h <= max_h;
        }
        /* another frame type (extended, progressive, lossless, arithmetic), or the scan or the end
         * before any frame */
        if ((m >= 0xC1u && m <= 0xCFu && m != 0xC4u && m != 0xC8u && m != 0xCCu) || m == 0xDAu || m == 0xD9u) {
            return false;
        }
        i += 2u + len;
    }
    return false;
}

asset_rule_t asset_pack_check_kind(const asset_pack_t *p, uint8_t kind, const char *entry, bool release)
{
    if (p == NULL || p->kind != kind || p->name == NULL) {
        return ASSET_RULE_KIND;
    }
    if (kind != PS_KIND_LETTERS && kind != PS_KIND_PICTURE && kind != PS_KIND_CERTS) {
        return ASSET_RULE_NOT_TAKEN;
    }
    const char *store = asset_pack_store_name(kind);
    if (strcmp(p->name, store) != 0 || (entry != NULL && strcmp(entry, store) != 0)) {
        return ASSET_RULE_NAME;
    }
    if (p->draft && release) {
        return ASSET_RULE_DRAFT;
    }
    if (kind == PS_KIND_LETTERS) {
        /* fonts only, named by role */
        for (uint16_t i = 0u; i < p->part_count; i++) {
            if (p->parts[i].type != ASSET_PART_FONT) {
                return ASSET_RULE_NOT_FONTS;
            }
        }
        return ASSET_RULE_OK;
    }
    if (kind == PS_KIND_PICTURE) {
        const asset_part_t *photo = asset_pack_find(p, "photo");
        if (p->part_count != 1u || photo == NULL || photo->type != ASSET_PART_JPEG || p->match != 0u) {
            return ASSET_RULE_NOT_PHOTO;
        }
        return asset_jpeg_fits(photo->data, photo->size, ASSET_PICTURE_MAX_W, ASSET_PICTURE_MAX_H)
                   ? ASSET_RULE_OK
                   : ASSET_RULE_PHOTO_SIZE;
    }
    const asset_part_t *bundle = asset_pack_find(p, "bundle");
    if (p->part_count != 1u || bundle == NULL || bundle->type != ASSET_PART_CERTS || p->match != 0u) {
        return ASSET_RULE_NOT_BUNDLE;
    }
    return ASSET_RULE_OK;
}

const char *asset_rule_reason(asset_rule_t r)
{
    switch (r) {
    case ASSET_RULE_OK: return "";
    case ASSET_RULE_KIND: return "the header's kind is not the entry's";
    case ASSET_RULE_NOT_TAKEN: return "a kind this firmware does not take";
    case ASSET_RULE_NAME: return "its name is not the kind's";
    case ASSET_RULE_DRAFT: return "a draft: a release firmware does not use it";
    case ASSET_RULE_NOT_FONTS: return "a part is not a font";
    case ASSET_RULE_NOT_PHOTO: return "not one `photo` JPEG part";
    case ASSET_RULE_PHOTO_SIZE: return "the photo is not a baseline JPEG of at most 1024 x 600";
    case ASSET_RULE_NOT_BUNDLE: return "not one `bundle` certificates part";
    default: return "?";
    }
}
