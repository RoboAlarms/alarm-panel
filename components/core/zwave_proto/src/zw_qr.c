/*
 * zw_qr.c - the Z-Wave QR code (see zw_qr.h). The digits are copied once, whitespace dropped,
 * then read field by field with a cursor that never passes their end.
 */
#include "zwave_proto/zw_qr.h"

#include <string.h>

#include "zwave_proto/zw_sha1.h"

typedef struct {
    const char *d;
    size_t n;
    size_t pos;
} cur_t;

/* `count` digits as a number (count <= 9). False when fewer are left. */
static bool take(cur_t *c, size_t count, uint32_t *out)
{
    if (c->n - c->pos < count) {
        return false;
    }
    uint32_t v = 0u;
    for (size_t i = 0u; i < count; i++) {
        v = v * 10u + (uint32_t)(c->d[c->pos + i] - '0');
    }
    c->pos += count;
    *out = v;
    return true;
}

/* A 5-digit group that must fit 16 bits. */
static zw_qr_result_t take16(cur_t *c, uint16_t *out)
{
    uint32_t v;
    if (!take(c, 5u, &v)) {
        return ZW_QR_TOO_SHORT;
    }
    if (v > 0xFFFFu) {
        return ZW_QR_BAD_NUMBER;
    }
    *out = (uint16_t)v;
    return ZW_QR_OK;
}

#define TRY(x)                                                                                     \
    do {                                                                                           \
        const zw_qr_result_t r_ = (x);                                                             \
        if (r_ != ZW_QR_OK) {                                                                      \
            return r_;                                                                             \
        }                                                                                          \
    } while (0)

/* One block's data, `len` digits, already known to be within the code. */
static zw_qr_result_t block(zw_qr_t *q, unsigned type, const char *data, size_t len,
                            bool *seen_type, bool *seen_id)
{
    cur_t c = {data, len, 0u};
    uint16_t w;
    switch (type) {
    case ZW_QR_TLV_PRODUCT_TYPE:
        if (len != 10u) {
            return ZW_QR_BAD_NUMBER;
        }
        TRY(take16(&c, &w));
        q->generic = (uint8_t)(w >> 8);
        q->specific = (uint8_t)w;
        TRY(take16(&c, &q->icon));
        *seen_type = true;
        return ZW_QR_OK;
    case ZW_QR_TLV_PRODUCT_ID:
        if (len != 20u) {
            return ZW_QR_BAD_NUMBER;
        }
        TRY(take16(&c, &q->manufacturer));
        TRY(take16(&c, &q->product_type));
        TRY(take16(&c, &q->product_id));
        TRY(take16(&c, &q->app_version));
        *seen_id = true;
        return ZW_QR_OK;
    case ZW_QR_TLV_MAX_INTERVAL: {
        uint32_t v;
        if (len != 2u || !take(&c, 2u, &v)) {
            return ZW_QR_BAD_NUMBER;
        }
        q->has_max_interval = true;
        q->max_interval_s = (uint16_t)(v * 128u);
        return ZW_QR_OK;
    }
    case ZW_QR_TLV_UUID16: {
        uint32_t f;
        if (len != 42u || !take(&c, 2u, &f)) {
            return ZW_QR_BAD_NUMBER;
        }
        q->uuid_format = (uint8_t)f;
        for (size_t g = 0u; g < 8u; g++) {
            TRY(take16(&c, &w));
            q->uuid[g * 2u] = (uint8_t)(w >> 8);
            q->uuid[g * 2u + 1u] = (uint8_t)w;
        }
        q->has_uuid = true;
        return ZW_QR_OK;
    }
    case ZW_QR_TLV_PROTOCOLS: {
        uint32_t v;
        if (len == 0u || len > 5u || !take(&c, len, &v) || v > 0xFFu) {
            return ZW_QR_BAD_NUMBER;
        }
        q->protocols = (uint8_t)v;
        return ZW_QR_OK;
    }
    default:
        return ZW_QR_OK; /* unknown: the caller has checked it is not critical */
    }
}

static zw_qr_result_t parse(const char *digits, size_t n, zw_qr_t *q)
{
    cur_t c = {digits, n, 0u};
    uint32_t v;
    (void)take(&c, 2u, &v);
    if (v != 90u) {
        return ZW_QR_NOT_ZWAVE;
    }
    if (n < ZW_QR_MIN_DIGITS) {
        return ZW_QR_TOO_SHORT;
    }
    (void)take(&c, 2u, &v);
    if (v != ZW_QR_VERSION_S2 && v != ZW_QR_VERSION_SMARTSTART) {
        return ZW_QR_VERSION;
    }
    q->version = (uint8_t)v;
    uint32_t sum;
    (void)take(&c, 5u, &sum);
    uint8_t h[20];
    zw_sha1(&digits[9], n - 9u, h);
    if (sum != (((uint32_t)h[0] << 8) | (uint32_t)h[1])) {
        return ZW_QR_CHECKSUM;
    }
    (void)take(&c, 3u, &v);
    if (v > 0xFFu) {
        return ZW_QR_BAD_NUMBER;
    }
    q->requested_keys = (uint8_t)v;
    for (size_t g = 0u; g < 8u; g++) {
        uint16_t w;
        TRY(take16(&c, &w));
        q->dsk[g * 2u] = (uint8_t)(w >> 8);
        q->dsk[g * 2u + 1u] = (uint8_t)w;
    }
    q->protocols = ZW_QR_PROTO_CLASSIC;
    bool seen_type = false;
    bool seen_id = false;
    while (c.pos < c.n) {
        uint32_t tc;
        uint32_t len;
        if (!take(&c, 2u, &tc) || !take(&c, 2u, &len) || c.n - c.pos < len) {
            return ZW_QR_TOO_SHORT;
        }
        const unsigned type = (unsigned)(tc >> 1);
        const bool critical = (tc & 1u) != 0u;
        if (type > ZW_QR_TLV_PROTOCOLS && critical) {
            return ZW_QR_CRITICAL;
        }
        TRY(block(q, type, &c.d[c.pos], len, &seen_type, &seen_id));
        c.pos += len;
    }
    return seen_type && seen_id ? ZW_QR_OK : ZW_QR_MISSING;
}

zw_qr_result_t zw_qr_parse(const char *text, zw_qr_t *out)
{
    if (out == NULL) {
        return ZW_QR_NOT_ZWAVE;
    }
    memset(out, 0, sizeof *out);
    if (text == NULL) {
        return ZW_QR_NOT_ZWAVE;
    }
    char digits[ZW_QR_MAX_DIGITS];
    size_t n = 0u;
    for (const char *p = text; *p != '\0'; p++) {
        const char ch = *p;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch < '0' || ch > '9') {
            return ZW_QR_NOT_ZWAVE;
        }
        if (n == sizeof digits) {
            return ZW_QR_TOO_LONG;
        }
        digits[n++] = ch;
    }
    if (n < 2u) {
        return n == 0u ? ZW_QR_NOT_ZWAVE : ZW_QR_TOO_SHORT;
    }
    zw_qr_t q;
    memset(&q, 0, sizeof q);
    const zw_qr_result_t r = parse(digits, n, &q);
    if (r == ZW_QR_OK) {
        *out = q;
    }
    return r;
}
