/*
 * zbz_codes.c - install codes: parsing what was typed or scanned, the CRC-16/X.25 check, and the
 * codes waiting for their devices with their stored blob (zbz_codes.h; docs/features/20
 * devices.md §8; ZB-015). Portable C11, no heap.
 */
#include "zigbee_zones/zbz_codes.h"

#include <string.h>

uint16_t zbz_code_crc(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ bytes[i]);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
        }
    }
    return (uint16_t)(crc ^ 0xFFFFu);
}

static bool is_separator(char c)
{
    return c == ' ' || c == '-' || c == ':' || c == '\t';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* The hex digits of text[0..n) with separators skipped, into digits[] (values 0..15). false on
 * any other character or more than `cap` digits. */
static bool collect_digits(const char *text, size_t n, uint8_t *digits, size_t cap, size_t *count)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = text[i];
        if (is_separator(c)) {
            continue;
        }
        int v = hex_value(c);
        if (v < 0 || k == cap) {
            return false;
        }
        digits[k++] = (uint8_t)v;
    }
    *count = k;
    return true;
}

static bool code_length_ok(size_t len)
{
    return len == 8u || len == 10u || len == 14u || len == 18u;
}

static zbz_code_result_t code_check(const uint8_t *code, size_t len)
{
    if (!code_length_ok(len)) {
        return ZBZ_CODE_BAD_LENGTH;
    }
    uint16_t want = (uint16_t)((unsigned)code[len - 2u] | ((unsigned)code[len - 1u] << 8));
    return zbz_code_crc(code, len - 2u) == want ? ZBZ_CODE_OK : ZBZ_CODE_CHECKSUM;
}

static zbz_code_result_t code_from_digits(const uint8_t *digits, size_t count,
                                          uint8_t out[ZBZ_CODE_MAX], uint8_t *len)
{
    if (count % 2u != 0u || !code_length_ok(count / 2u)) {
        return ZBZ_CODE_BAD_LENGTH;
    }
    uint8_t code[ZBZ_CODE_MAX];
    size_t n = count / 2u;
    for (size_t i = 0; i < n; i++) {
        code[i] = (uint8_t)(((unsigned)digits[2u * i] << 4) | digits[2u * i + 1u]);
    }
    zbz_code_result_t r = code_check(code, n);
    if (r == ZBZ_CODE_OK) {
        memcpy(out, code, n);
        *len = (uint8_t)n;
    }
    memset(code, 0, sizeof(code));
    return r;
}

static bool ieee_usable(uint64_t ieee)
{
    return ieee != 0u && ieee != UINT64_MAX;
}

static zbz_code_result_t ieee_from_digits(const uint8_t *digits, size_t count, uint64_t *ieee)
{
    if (count != 16u) {
        return ZBZ_CODE_BAD_ADDRESS;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < 16u; i++) {
        v = (v << 4) | (uint64_t)digits[i];
    }
    if (!ieee_usable(v)) {
        return ZBZ_CODE_BAD_ADDRESS;
    }
    *ieee = v;
    return ZBZ_CODE_OK;
}

zbz_code_result_t zbz_code_parse(const char *text, uint8_t out[ZBZ_CODE_MAX], uint8_t *len)
{
    uint8_t digits[2u * ZBZ_CODE_MAX];
    size_t count = 0;
    if (text == NULL || out == NULL || len == NULL ||
        !collect_digits(text, strlen(text), digits, sizeof(digits), &count)) {
        return ZBZ_CODE_BAD_LENGTH;
    }
    zbz_code_result_t r = code_from_digits(digits, count, out, len);
    memset(digits, 0, sizeof(digits));
    return r;
}

zbz_code_result_t zbz_ieee_parse(const char *text, uint64_t *ieee)
{
    if (text == NULL || ieee == NULL) {
        return ZBZ_CODE_BAD_ADDRESS;
    }
    while (is_separator(*text)) {
        text++;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    uint8_t digits[16];
    size_t count = 0;
    if (!collect_digits(text, strlen(text), digits, sizeof(digits), &count)) {
        return ZBZ_CODE_BAD_ADDRESS;
    }
    return ieee_from_digits(digits, count, ieee);
}

/* The value of a QR field "<key>:<value>" among the '$'-separated fields of text, up to the next
 * '$' or '%' (where the optional trailing fields start). NULL when the field isn't there. */
static const char *qr_field(const char *text, char key, size_t *n)
{
    const char *p = text;
    while (p != NULL && *p != '\0') {
        if ((p[0] == key || p[0] == (char)(key + ('a' - 'A'))) && p[1] == ':') {
            const char *v = p + 2;
            size_t k = 0;
            while (v[k] != '\0' && v[k] != '$' && v[k] != '%') {
                k++;
            }
            *n = k;
            return v;
        }
        p = strchr(p, '$');
        if (p != NULL) {
            p++;
        }
    }
    return NULL;
}

zbz_code_result_t zbz_code_parse_qr(const char *text, uint64_t *ieee, uint8_t out[ZBZ_CODE_MAX],
                                    uint8_t *len)
{
    if (text == NULL || ieee == NULL || out == NULL || len == NULL) {
        return ZBZ_CODE_BAD_LENGTH;
    }
    uint8_t digits[16u + 2u * ZBZ_CODE_MAX];
    size_t count = 0;
    zbz_code_result_t r;
    uint64_t address = 0;
    size_t zn = 0, in = 0;
    const char *z = qr_field(text, 'Z', &zn);
    const char *i = qr_field(text, 'I', &in);
    if (z != NULL || i != NULL) {
        /* The Zigbee 3.0 form: "Z:<address>$I:<code>", other fields and a %... tail ignored. */
        if (z == NULL || i == NULL) {
            return ZBZ_CODE_BAD_LENGTH;
        }
        if (!collect_digits(z, zn, digits, 16u, &count)) {
            return ZBZ_CODE_BAD_ADDRESS;
        }
        r = ieee_from_digits(digits, count, &address);
        if (r != ZBZ_CODE_OK) {
            return r;
        }
        if (!collect_digits(i, in, digits, 2u * ZBZ_CODE_MAX, &count)) {
            return ZBZ_CODE_BAD_LENGTH;
        }
        r = code_from_digits(digits, count, out, len);
    } else {
        /* A bare "<address> <code>": the first 16 hex digits are the address, the rest the code. */
        if (!collect_digits(text, strlen(text), digits, sizeof(digits), &count) || count <= 16u) {
            return ZBZ_CODE_BAD_LENGTH;
        }
        r = ieee_from_digits(digits, 16u, &address);
        if (r == ZBZ_CODE_OK) {
            r = code_from_digits(digits + 16, count - 16u, out, len);
        }
    }
    memset(digits, 0, sizeof(digits));
    if (r == ZBZ_CODE_OK) {
        *ieee = address;
    }
    return r;
}

void zbz_codes_init(zbz_codes_t *c)
{
    memset(c, 0, sizeof(*c));
}

static size_t index_of(const zbz_codes_t *c, uint64_t ieee)
{
    for (size_t i = 0; i < c->count; i++) {
        if (c->codes[i].ieee == ieee) {
            return i;
        }
    }
    return SIZE_MAX;
}

zbz_code_result_t zbz_codes_add(zbz_codes_t *c, uint64_t ieee, const uint8_t *code, uint8_t len,
                                uint32_t now_s)
{
    if (!ieee_usable(ieee)) {
        return ZBZ_CODE_BAD_ADDRESS;
    }
    if (code == NULL) {
        return ZBZ_CODE_BAD_LENGTH;
    }
    zbz_code_result_t r = code_check(code, len);
    if (r != ZBZ_CODE_OK) {
        return r;
    }
    size_t at = index_of(c, ieee);
    if (at == SIZE_MAX) {
        if (c->count >= ZBZ_CODES_MAX) {
            return ZBZ_CODE_FULL;
        }
        at = c->count++;
    }
    zbz_code_t *e = &c->codes[at];
    memset(e, 0, sizeof(*e));
    e->ieee = ieee;
    e->len = len;
    memcpy(e->code, code, len);
    e->added_at = now_s;
    c->dirty = true;
    return ZBZ_CODE_OK;
}

zbz_code_result_t zbz_codes_forget(zbz_codes_t *c, uint64_t ieee)
{
    size_t at = index_of(c, ieee);
    if (at == SIZE_MAX) {
        return ZBZ_CODE_NOT_FOUND;
    }
    memmove(&c->codes[at], &c->codes[at + 1u], (c->count - at - 1u) * sizeof(c->codes[0]));
    c->count--;
    memset(&c->codes[c->count], 0, sizeof(c->codes[0])); /* no copy of the code is left behind */
    c->dirty = true;
    return ZBZ_CODE_OK;
}

const zbz_code_t *zbz_codes_find(const zbz_codes_t *c, uint64_t ieee)
{
    size_t at = index_of(c, ieee);
    return at == SIZE_MAX ? NULL : &c->codes[at];
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4u; i++) {
        v |= (uint32_t)p[i] << (8u * i);
    }
    return v;
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

size_t zbz_codes_encode(const zbz_codes_t *c, uint8_t *out, size_t cap)
{
    size_t need = 2u;
    for (size_t i = 0; i < c->count; i++) {
        need += 1u + 8u + 1u + c->codes[i].len + 4u;
    }
    if (out == NULL || cap < need || c->count > ZBZ_CODES_MAX) {
        return 0;
    }
    size_t at = 0;
    out[at++] = ZBZ_CODES_COMPAT;
    out[at++] = (uint8_t)c->count;
    for (size_t i = 0; i < c->count; i++) {
        const zbz_code_t *e = &c->codes[i];
        out[at++] = (uint8_t)(8u + 1u + e->len + 4u); /* what follows this byte */
        put_u64(&out[at], e->ieee);
        at += 8u;
        out[at++] = e->len;
        memcpy(&out[at], e->code, e->len);
        at += e->len;
        put_u32(&out[at], e->added_at);
        at += 4u;
    }
    return at;
}

/* The entries of a blob whose header is already checked, into `got`; false when damaged. */
static bool decode_entries(zbz_codes_t *got, const uint8_t *buf, size_t len, size_t count)
{
    size_t at = 2u;
    for (size_t i = 0; i < count; i++) {
        if (at >= len) {
            return false;
        }
        size_t entry = buf[at++];
        if (entry < 8u + 1u || at + entry > len) {
            return false;
        }
        const uint8_t *p = &buf[at];
        uint8_t code_len = p[8];
        /* A later format may append fields inside an entry: the entry's length covers them. */
        if (8u + 1u + (size_t)code_len + 4u > entry || code_check(p + 9, code_len) != ZBZ_CODE_OK) {
            return false;
        }
        zbz_code_t *e = &got->codes[got->count++];
        e->ieee = get_u64(p);
        e->len = code_len;
        memcpy(e->code, p + 9, code_len);
        e->added_at = get_u32(p + 9 + code_len);
        if (!ieee_usable(e->ieee) || index_of(got, e->ieee) != got->count - 1u) {
            return false; /* a zero address, or the same device twice */
        }
        at += entry;
    }
    return at == len;
}

bool zbz_codes_decode(zbz_codes_t *c, const uint8_t *buf, size_t len)
{
    zbz_codes_init(c);
    if (buf == NULL || len < 2u || buf[0] == 0u || buf[0] > ZBZ_CODES_COMPAT ||
        buf[1] > ZBZ_CODES_MAX) {
        return false;
    }
    zbz_codes_t got;
    zbz_codes_init(&got);
    bool ok = decode_entries(&got, buf, len, buf[1]);
    if (ok) {
        *c = got;
    }
    memset(&got, 0, sizeof(got)); /* no copy of a code is left on the stack */
    return ok;
}
