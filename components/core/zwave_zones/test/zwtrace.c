/*
 * zwtrace.c - reading a "zwtrace 1" recording (see zwtrace.h).
 */
#include "zwtrace.h"

#include <stdio.h>
#include <string.h>

#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_frame.h"

#define LINE_MAX_CHARS (2u * ZWT_ENTRY_MAX + 64u)

static const char *const KINDS[] = {"rx", "tx", "crx", "ctx", "text"};

static zwt_result_t refuse(zwt_trace_t *t, zwt_result_t r, uint32_t line, const char *why)
{
    t->error_line = line;
    (void)snprintf(t->error, sizeof t->error, "line %u: %s", (unsigned)line, why);
    return r;
}

static void begin(zwt_trace_t *t)
{
    t->n = 0u;
    t->used = 0u;
    t->lines = 0u;
    t->lost = 0u;
    t->restarts = 0u;
    t->scrubbed = false;
    t->error_line = 0u;
    t->error[0] = '\0';
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* The next token of s[*i..n): its start and length; false when none is left. */
static bool token(const char *s, size_t n, size_t *i, const char **tok, size_t *len)
{
    while (*i < n && is_space(s[*i])) {
        (*i)++;
    }
    if (*i >= n) {
        return false;
    }
    *tok = &s[*i];
    const size_t start = *i;
    while (*i < n && !is_space(s[*i])) {
        (*i)++;
    }
    *len = *i - start;
    return true;
}

static bool decimal(const char *s, size_t n, uint64_t max, uint64_t *out)
{
    uint64_t v = 0u;
    if (n == 0u || n > 20u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        const uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (max - d) / 10u) {
            return false;
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

static int nibble(char c)
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

static bool same(const char *s, size_t n, const char *word)
{
    return strlen(word) == n && memcmp(s, word, n) == 0;
}

static zwt_entry_t *add_entry(zwt_trace_t *t, uint8_t kind, uint32_t line)
{
    if (t->n >= ZWT_ENTRIES_MAX) {
        return NULL;
    }
    zwt_entry_t *e = &t->e[t->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->line = line;
    e->off = (uint32_t)t->used;
    return e;
}

/* One line (without its end of line), numbered from 1. */
static zwt_result_t parse_line(zwt_trace_t *t, const char *s, size_t n, uint32_t line)
{
    while (n > 0u && (s[n - 1u] == '\r' || is_space(s[n - 1u]))) {
        n--;
    }
    t->lines = line;
    if (line == 1u) {
        if (same(s, n, "zwtrace 1")) {
            return ZWT_OK;
        }
        if (n > 8u && memcmp(s, "zwtrace ", 8u) == 0) {
            return refuse(t, ZWT_ERR_HEADER, line, "a zwtrace version this reader does not know");
        }
        return refuse(t, ZWT_ERR_HEADER, line, "not a zwtrace recording (the first line must be "
                                               "\"zwtrace 1\")");
    }
    size_t i = 0u;
    while (i < n && is_space(s[i])) {
        i++;
    }
    if (i == n) {
        return ZWT_OK; /* a blank line */
    }
    if (s[i] == '#') {
        const size_t mark = strlen(ZWT_SCRUBBED_MARK);
        if (same(&s[i], n - i, "# panel restarted")) {
            if (add_entry(t, ZWT_RESTART, line) == NULL) {
                return refuse(t, ZWT_ERR_FULL, line, "too many entries");
            }
            t->restarts++;
        } else if (n - i >= mark && memcmp(&s[i], ZWT_SCRUBBED_MARK, mark) == 0) {
            t->scrubbed = true;
        }
        return ZWT_OK;
    }
    const char *tok[5];
    size_t len[5];
    size_t count = 0u;
    while (count < 5u && token(s, n, &i, &tok[count], &len[count])) {
        count++;
    }
    uint64_t v = 0u;
    if (count == 2u && same(tok[0], len[0], "lost")) {
        if (!decimal(tok[1], len[1], UINT32_MAX, &v)) {
            return refuse(t, ZWT_ERR_LINE, line, "\"lost\" needs a count");
        }
        zwt_entry_t *e = add_entry(t, ZWT_LOST, line);
        if (e == NULL) {
            return refuse(t, ZWT_ERR_FULL, line, "too many entries");
        }
        e->lost = (uint32_t)v;
        t->lost += (uint32_t)v;
        return ZWT_OK;
    }
    if (count != 4u) {
        return refuse(t, ZWT_ERR_LINE, line, "expected <seq> <t_ms> <dir> <hex>, \"lost <n>\" or a "
                                             "# comment");
    }
    uint64_t seq = 0u;
    uint64_t at = 0u;
    if (!decimal(tok[0], len[0], UINT32_MAX, &seq) || !decimal(tok[1], len[1], UINT64_MAX, &at)) {
        return refuse(t, ZWT_ERR_LINE, line, "the sequence number or the time is not a number");
    }
    uint8_t kind = 0xFFu;
    for (uint8_t k = 0u; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        if (same(tok[2], len[2], KINDS[k])) {
            kind = k;
        }
    }
    if (kind == 0xFFu) {
        return refuse(t, ZWT_ERR_LINE, line, "the direction is none of rx, tx, crx, ctx, text");
    }
    if (len[3] == 0u || len[3] % 2u != 0u) {
        return refuse(t, ZWT_ERR_LINE, line, "the bytes are not whole hex pairs");
    }
    const size_t bytes = len[3] / 2u;
    if (bytes > ZWT_ENTRY_MAX) {
        return refuse(t, ZWT_ERR_LINE, line, "an entry longer than the reader takes");
    }
    if ((kind == ZWT_CRX || kind == ZWT_CTX) && bytes != 1u) {
        return refuse(t, ZWT_ERR_LINE, line, "an ACK, NAK or CAN is one byte");
    }
    if (bytes > ZWT_BYTES_MAX - t->used) {
        return refuse(t, ZWT_ERR_FULL, line, "too many bytes");
    }
    zwt_entry_t *e = add_entry(t, kind, line);
    if (e == NULL) {
        return refuse(t, ZWT_ERR_FULL, line, "too many entries");
    }
    for (size_t b = 0; b < bytes; b++) {
        const int hi = nibble(tok[3][2u * b]);
        const int lo = nibble(tok[3][2u * b + 1u]);
        if (hi < 0 || lo < 0) {
            t->n--; /* not taken */
            return refuse(t, ZWT_ERR_LINE, line, "the bytes are not hex");
        }
        t->bytes[t->used + b] = (uint8_t)((unsigned)hi << 4 | (unsigned)lo);
    }
    e->seq = (uint32_t)seq;
    e->t = at;
    e->len = (uint16_t)bytes;
    t->used += bytes;
    return ZWT_OK;
}

static zwt_result_t finish(zwt_trace_t *t)
{
    if (t->lines == 0u) {
        return refuse(t, ZWT_ERR_HEADER, 1u, "empty: not a zwtrace recording");
    }
    return ZWT_OK;
}

zwt_result_t zwt_parse(zwt_trace_t *t, const char *text, size_t len)
{
    begin(t);
    uint32_t line = 0u;
    size_t start = 0u;
    while (start < len) {
        size_t end = start;
        while (end < len && text[end] != '\n') {
            end++;
        }
        line++;
        const zwt_result_t r = parse_line(t, &text[start], end - start, line);
        if (r != ZWT_OK) {
            return r;
        }
        start = end + 1u;
    }
    return finish(t);
}

zwt_result_t zwt_load(zwt_trace_t *t, const char *path)
{
    begin(t);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        (void)snprintf(t->error, sizeof t->error, "cannot open the file");
        return ZWT_ERR_FILE;
    }
    char buf[LINE_MAX_CHARS + 2u];
    uint32_t line = 0u;
    zwt_result_t r = ZWT_OK;
    while (r == ZWT_OK && fgets(buf, (int)sizeof buf, f) != NULL) {
        size_t n = strlen(buf);
        line++;
        if (n > 0u && buf[n - 1u] == '\n') {
            n--;
        } else if (!feof(f)) {
            r = refuse(t, ZWT_ERR_LINE, line, "a line longer than the reader takes");
            break;
        }
        r = parse_line(t, buf, n, line);
    }
    if (r == ZWT_OK && ferror(f)) {
        (void)snprintf(t->error, sizeof t->error, "the file could not be read");
        r = ZWT_ERR_FILE;
    }
    (void)fclose(f);
    return r == ZWT_OK ? finish(t) : r;
}

const uint8_t *zwt_bytes(const zwt_trace_t *t, const zwt_entry_t *e)
{
    return &t->bytes[e->off];
}

bool zwt_frame_ok(const uint8_t *b, size_t n)
{
    return n >= 5u && b[0] == ZW_SOF && b[1] >= 3u && n == (size_t)b[1] + 2u &&
           zw_frame_checksum(&b[1], b[1]) == b[n - 1u];
}

size_t zwt_hex(const uint8_t *b, size_t n, char *out, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    if (cap == 0u) {
        return 0u;
    }
    const bool fits = 2u * n + 1u <= cap;
    const size_t pairs = fits ? n : (cap >= 4u ? (cap - 4u) / 2u : 0u); /* room for "..." */
    size_t w = 0u;
    for (size_t i = 0; i < pairs; i++) {
        out[w++] = digits[b[i] >> 4];
        out[w++] = digits[b[i] & 15u];
    }
    for (size_t k = 0; !fits && k < 3u && w + 1u < cap; k++) {
        out[w++] = '.';
    }
    out[w] = '\0';
    return w;
}

void zwt_module(const zwt_trace_t *t, zwt_module_t *out)
{
    memset(out, 0, sizeof *out);
    zw_nid_width_t w = ZW_NID_8;
    for (size_t i = 0; i < t->n; i++) {
        const zwt_entry_t *e = &t->e[i];
        const uint8_t *b = zwt_bytes(t, e);
        if (e->kind == ZWT_TX && zwt_frame_ok(b, e->len) && b[3] == ZW_FUNC_SERIAL_API_SETUP &&
            e->len >= 7u && b[4] == ZW_SETUP_SET_NODE_ID_TYPE) {
            w = b[5] == 0x02u ? ZW_NID_16 : ZW_NID_8; /* taken once answered: close enough */
        }
        if (e->kind != ZWT_RX || !zwt_frame_ok(b, e->len)) {
            continue;
        }
        const uint8_t *p = &b[4];
        const size_t pn = (size_t)e->len - 5u;
        if (b[2] == ZW_TYPE_REQUEST && b[3] == ZW_FUNC_SERIAL_API_STARTED) {
            out->started++;
            w = ZW_NID_8; /* a restart returns to 8-bit node IDs */
            continue;
        }
        if (b[2] != ZW_TYPE_RESPONSE) {
            continue;
        }
        switch (b[3]) {
        case ZW_FUNC_GET_SERIAL_API_CAPABILITIES: {
            zw_api_api_caps_rsp_t c;
            if (zw_api_api_caps_rsp_parse(p, pn, &c)) {
                out->has_caps = true;
                out->firmware_major = c.app_version_major;
                out->firmware_minor = c.app_version_minor;
                out->manufacturer = c.manufacturer_id;
                out->product_type = c.product_type;
                out->product_id = c.product_id;
            }
            break;
        }
        case ZW_FUNC_GET_SERIAL_API_INIT_DATA: {
            zw_api_init_data_rsp_t d;
            if (zw_api_init_data_rsp_parse(p, pn, &d)) {
                out->has_init = true;
                out->chip_type = d.chip_type;
            }
            break;
        }
        case ZW_FUNC_GET_PROTOCOL_VERSION: {
            zw_api_protocol_version_rsp_t v;
            if (zw_api_protocol_version_rsp_parse(p, pn, &v)) {
                out->has_sdk = true;
                out->sdk_major = v.major;
                out->sdk_minor = v.minor;
                out->sdk_patch = v.patch;
            }
            break;
        }
        case ZW_FUNC_SERIAL_API_SETUP: {
            zw_api_api_setup_rsp_t s;
            uint8_t region = 0u;
            if (zw_api_api_setup_rsp_parse(p, pn, &s) &&
                zw_api_setup_rsp_u8(&s, ZW_SETUP_GET_RF_REGION, &region)) {
                out->has_region = true;
                out->region = region;
            }
            break;
        }
        case ZW_FUNC_GET_CONTROLLER_ID: {
            zw_api_controller_id_rsp_t c;
            if (zw_api_controller_id_rsp_parse(p, pn, w, &c)) {
                out->has_home = true;
                out->home_id = (uint32_t)c.home_id[0] << 24 | (uint32_t)c.home_id[1] << 16 |
                               (uint32_t)c.home_id[2] << 8 | (uint32_t)c.home_id[3];
                out->node_id = c.own_node_id;
            }
            break;
        }
        default:
            break;
        }
    }
}
