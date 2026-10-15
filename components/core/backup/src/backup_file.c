/*
 * backup_file.c - the backup file's payload records and its encryption (backup.h).
 */
#include "backup/backup.h"

#include <string.h>

/* ---- little-endian helpers ------------------------------------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0u) {
        *v++ = 0u;
    }
}

/* ---- names and values ----------------------------------------------------------------------- */

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n <= max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* An NVS namespace or key: 1..15 printable ASCII characters. */
static bool name_ok(const char *s, size_t n)
{
    if (n == 0u || n > BACKUP_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch < 0x21u || ch > 0x7Eu) {
            return false;
        }
    }
    return true;
}

static bool text_ok(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20u || ch > 0x7Eu) {
            return false;
        }
    }
    return true;
}

/* The length a value of this type must have; 0 for the variable-length types. */
static uint32_t fixed_len(uint8_t type)
{
    switch (type) {
    case BACKUP_T_U8:
    case BACKUP_T_I8: return 1u;
    case BACKUP_T_U16:
    case BACKUP_T_I16: return 2u;
    case BACKUP_T_U32:
    case BACKUP_T_I32: return 4u;
    case BACKUP_T_U64:
    case BACKUP_T_I64: return 8u;
    default: return 0u;
    }
}

static bool value_ok(uint8_t type, const uint8_t *value, uint32_t len)
{
    if (type < BACKUP_T_U8 || type > BACKUP_T_BLOB) {
        return false;
    }
    if (len > 0u && value == NULL) {
        return false;
    }
    const uint32_t want = fixed_len(type);
    if (want != 0u) {
        return len == want;
    }
    if (type == BACKUP_T_STR) {
        if (len > BACKUP_STR_MAX) {
            return false; /* NVS couldn't hold it */
        }
        for (uint32_t i = 0; i < len; i++) {
            if (value[i] == 0u) {
                return false; /* a string carries no NUL: NVS would cut it there */
            }
        }
        return true;
    }
    return len <= BACKUP_BLOB_MAX;
}

/* ---- the writer ----------------------------------------------------------------------------- */

static void w_bytes(backup_writer_t *w, const void *p, size_t n)
{
    if (w->failed || n > w->cap - w->len) {
        w->failed = true;
        return;
    }
    if (n > 0u) {
        memcpy(w->buf + w->len, p, n);
    }
    w->len += n;
}

static void w_u8(backup_writer_t *w, uint8_t v)
{
    w_bytes(w, &v, 1u);
}

static void w_u32(backup_writer_t *w, uint32_t v)
{
    uint8_t b[4];
    put_u32(b, v);
    w_bytes(w, b, 4u);
}

static void w_str(backup_writer_t *w, const char *s, size_t max)
{
    const size_t n = bounded_len(s, max);
    if (n > max) {
        w->failed = true;
        return;
    }
    w_u8(w, (uint8_t)n);
    w_bytes(w, s, n);
}

void backup_writer_init(backup_writer_t *w, uint8_t *buf, size_t cap, const backup_meta_t *meta)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = buf != NULL ? cap : 0u;
    w->failed = buf == NULL || meta == NULL;
    if (w->failed) {
        return;
    }
    if (!text_ok(meta->firmware, bounded_len(meta->firmware, BACKUP_TEXT_MAX)) ||
        !text_ok(meta->board, bounded_len(meta->board, BACKUP_TEXT_MAX))) {
        w->failed = true;
        return;
    }
    w_u8(w, BACKUP_REC_META);
    w_u8(w, BACKUP_FORMAT);
    w_u32(w, (uint32_t)meta->created_s);
    w_u32(w, (uint32_t)(meta->created_s >> 32));
    w_str(w, meta->firmware, BACKUP_TEXT_MAX);
    w_str(w, meta->board, BACKUP_TEXT_MAX);
}

bool backup_writer_add(backup_writer_t *w, const backup_entry_t *e)
{
    if (w == NULL || e == NULL || w->failed) {
        if (w != NULL) {
            w->failed = true;
        }
        return false;
    }
    const size_t ns_len = bounded_len(e->ns, BACKUP_NAME_MAX);
    const size_t key_len = bounded_len(e->key, BACKUP_NAME_MAX);
    if (!name_ok(e->ns, ns_len) || !name_ok(e->key, key_len) ||
        !value_ok(e->type, e->value, e->len)) {
        w->failed = true;
        return false;
    }
    w_u8(w, BACKUP_REC_ENTRY);
    w_str(w, e->ns, BACKUP_NAME_MAX);
    w_str(w, e->key, BACKUP_NAME_MAX);
    w_u8(w, e->type);
    w_u32(w, e->len);
    w_bytes(w, e->value, e->len);
    if (!w->failed) {
        w->count++;
    }
    return !w->failed;
}

backup_result_t backup_writer_finish(backup_writer_t *w, size_t *len)
{
    if (w == NULL || len == NULL) {
        return BACKUP_ERR_ARG;
    }
    w_u8(w, BACKUP_REC_END);
    w_u32(w, w->count);
    if (w->failed || w->len > BACKUP_PAYLOAD_MAX) {
        *len = 0u;
        return BACKUP_ERR_ARG;
    }
    *len = w->len;
    return BACKUP_OK;
}

/* ---- the reader ----------------------------------------------------------------------------- */

static bool r_bytes(backup_reader_t *r, const uint8_t **p, size_t n)
{
    if (n > r->len - r->pos) {
        return false;
    }
    *p = r->buf + r->pos;
    r->pos += n;
    return true;
}

static bool r_u8(backup_reader_t *r, uint8_t *v)
{
    const uint8_t *p;
    if (!r_bytes(r, &p, 1u)) {
        return false;
    }
    *v = p[0];
    return true;
}

static bool r_u32(backup_reader_t *r, uint32_t *v)
{
    const uint8_t *p;
    if (!r_bytes(r, &p, 4u)) {
        return false;
    }
    *v = get_u32(p);
    return true;
}

/* A [str] into out (max characters); name: the stricter NVS name rules. */
static bool r_str(backup_reader_t *r, char *out, size_t max, bool name)
{
    uint8_t n8;
    const uint8_t *p;
    if (!r_u8(r, &n8)) {
        return false;
    }
    const size_t n = (unsigned)n8;
    if (n > max || !r_bytes(r, &p, n)) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return name ? name_ok(out, n) : text_ok(out, n);
}

backup_result_t backup_reader_init(backup_reader_t *r, const uint8_t *payload, size_t len,
                                   backup_meta_t *meta)
{
    if (r == NULL || payload == NULL || meta == NULL) {
        return BACKUP_ERR_ARG;
    }
    memset(r, 0, sizeof(*r));
    memset(meta, 0, sizeof(*meta));
    r->buf = payload;
    r->len = len;
    uint8_t type;
    uint8_t format;
    uint32_t lo;
    uint32_t hi;
    if (len > BACKUP_PAYLOAD_MAX || !r_u8(r, &type) || type != BACKUP_REC_META ||
        !r_u8(r, &format) || format != BACKUP_FORMAT || !r_u32(r, &lo) || !r_u32(r, &hi) ||
        !r_str(r, meta->firmware, BACKUP_TEXT_MAX, false) ||
        !r_str(r, meta->board, BACKUP_TEXT_MAX, false)) {
        return BACKUP_ERR_FORMAT;
    }
    meta->created_s = ((uint64_t)hi << 32) | lo;
    return BACKUP_OK;
}

backup_result_t backup_reader_next(backup_reader_t *r, backup_entry_t *e, bool *done)
{
    if (r == NULL || e == NULL || done == NULL) {
        return BACKUP_ERR_ARG;
    }
    *done = false;
    memset(e, 0, sizeof(*e));
    for (;;) {
        if (r->ended) {
            *done = true;
            return BACKUP_OK;
        }
        uint8_t type;
        if (!r_u8(r, &type)) {
            return BACKUP_ERR_FORMAT; /* no END: cut short */
        }
        if (type == BACKUP_REC_END) {
            uint32_t count;
            if (!r_u32(r, &count) || count != r->count || r->pos != r->len) {
                return BACKUP_ERR_FORMAT;
            }
            r->ended = true;
            *done = true;
            return BACKUP_OK;
        }
        if (type == BACKUP_REC_ENTRY) {
            const uint8_t *value;
            if (!r_str(r, e->ns, BACKUP_NAME_MAX, true) ||
                !r_str(r, e->key, BACKUP_NAME_MAX, true) || !r_u8(r, &e->type) ||
                !r_u32(r, &e->len) || !r_bytes(r, &value, e->len) ||
                !value_ok(e->type, value, e->len)) {
                memset(e, 0, sizeof(*e));
                return BACKUP_ERR_FORMAT;
            }
            e->value = value;
            r->count++;
            return BACKUP_OK;
        }
        if (type == BACKUP_REC_META) {
            return BACKUP_ERR_FORMAT; /* one META, first */
        }
        /* A record type from a newer firmware: len u32 | bytes, skipped. */
        uint32_t skip;
        const uint8_t *p;
        if (!r_u32(r, &skip) || !r_bytes(r, &p, skip)) {
            return BACKUP_ERR_FORMAT;
        }
    }
}

/* ---- the file ------------------------------------------------------------------------------- */

bool backup_password_ok(const char *password)
{
    if (password == NULL) {
        return false;
    }
    const size_t n = bounded_len(password, BACKUP_PASSWORD_MAX);
    return n >= BACKUP_PASSWORD_MIN && n <= BACKUP_PASSWORD_MAX && text_ok(password, n);
}

static bool port_ok(const backup_crypto_t *c)
{
    return c != NULL && c->pbkdf2_sha256 != NULL && c->gcm_encrypt != NULL &&
           c->gcm_decrypt != NULL && c->random != NULL;
}

static void make_header(uint8_t h[BACKUP_HEADER_LEN], uint32_t iterations, const uint8_t salt[16],
                        const uint8_t nonce[12], uint32_t payload_len)
{
    memcpy(h, BACKUP_MAGIC, 4u);
    h[4] = BACKUP_FORMAT;
    h[5] = BACKUP_KDF_PBKDF2;
    h[6] = BACKUP_AEAD_GCM;
    h[7] = 0u;
    put_u32(h + 8, iterations);
    memcpy(h + 12, salt, BACKUP_SALT_LEN);
    memcpy(h + 28, nonce, BACKUP_NONCE_LEN);
    put_u32(h + 40, payload_len);
}

static bool derive(const backup_crypto_t *c, const char *password, const uint8_t salt[16],
                   uint32_t iterations, uint8_t key[32])
{
    const size_t n = bounded_len(password, BACKUP_PASSWORD_MAX);
    if (!c->pbkdf2_sha256(c->ctx, (const uint8_t *)password, n, salt, BACKUP_SALT_LEN, iterations,
                          key, BACKUP_KEY_LEN)) {
        wipe(key, BACKUP_KEY_LEN);
        return false;
    }
    return true;
}

backup_result_t backup_seal_with(const backup_crypto_t *c, const char *password,
                                 uint32_t iterations, const uint8_t salt[16],
                                 const uint8_t nonce[12], const uint8_t *payload, size_t len,
                                 uint8_t *out, size_t cap, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (!port_ok(c) || salt == NULL || nonce == NULL || payload == NULL || out == NULL ||
        out_len == NULL || len == 0u || len > BACKUP_PAYLOAD_MAX ||
        cap < len + BACKUP_OVERHEAD) {
        return BACKUP_ERR_ARG;
    }
    if (!backup_password_ok(password)) {
        return BACKUP_ERR_PASSWORD;
    }
    if (iterations == 0u) {
        iterations = BACKUP_ITER_DEFAULT;
    }
    if (iterations < BACKUP_ITER_MIN || iterations > BACKUP_ITER_MAX) {
        return BACKUP_ERR_ARG;
    }
    uint8_t key[BACKUP_KEY_LEN];
    if (!derive(c, password, salt, iterations, key)) {
        return BACKUP_ERR_CRYPTO;
    }
    make_header(out, iterations, salt, nonce, (uint32_t)len);
    const bool ok = c->gcm_encrypt(c->ctx, key, nonce, out, BACKUP_HEADER_LEN, payload, len,
                                   out + BACKUP_HEADER_LEN, out + BACKUP_HEADER_LEN + len);
    wipe(key, sizeof(key));
    if (!ok) {
        wipe(out, len + BACKUP_OVERHEAD);
        return BACKUP_ERR_CRYPTO;
    }
    *out_len = len + BACKUP_OVERHEAD;
    return BACKUP_OK;
}

backup_result_t backup_seal(const backup_crypto_t *c, const char *password, uint32_t iterations,
                            const uint8_t *payload, size_t len, uint8_t *out, size_t cap,
                            size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (!port_ok(c)) {
        return BACKUP_ERR_ARG;
    }
    uint8_t salt[BACKUP_SALT_LEN];
    uint8_t nonce[BACKUP_NONCE_LEN];
    if (!c->random(c->ctx, salt, sizeof(salt)) || !c->random(c->ctx, nonce, sizeof(nonce))) {
        return BACKUP_ERR_CRYPTO;
    }
    return backup_seal_with(c, password, iterations, salt, nonce, payload, len, out, cap, out_len);
}

backup_result_t backup_peek(const uint8_t *file, size_t len, uint32_t *payload_len,
                            uint32_t *iterations)
{
    if (file == NULL || len < BACKUP_OVERHEAD) {
        return BACKUP_ERR_FORMAT;
    }
    if (memcmp(file, BACKUP_MAGIC, 4u) != 0 || file[4] != BACKUP_FORMAT ||
        file[5] != BACKUP_KDF_PBKDF2 || file[6] != BACKUP_AEAD_GCM || file[7] != 0u) {
        return BACKUP_ERR_FORMAT;
    }
    const uint32_t iter = get_u32(file + 8);
    const uint32_t plen = get_u32(file + 40);
    if (iter < BACKUP_ITER_MIN || iter > BACKUP_ITER_MAX || plen == 0u ||
        plen > BACKUP_PAYLOAD_MAX || (size_t)plen + BACKUP_OVERHEAD != len) {
        return BACKUP_ERR_FORMAT;
    }
    if (payload_len != NULL) {
        *payload_len = plen;
    }
    if (iterations != NULL) {
        *iterations = iter;
    }
    return BACKUP_OK;
}

backup_result_t backup_open(const backup_crypto_t *c, const char *password, const uint8_t *file,
                            size_t len, uint8_t *payload, size_t cap, size_t *payload_len)
{
    if (payload_len != NULL) {
        *payload_len = 0u;
    }
    if (!port_ok(c) || password == NULL || payload == NULL || payload_len == NULL) {
        return BACKUP_ERR_ARG;
    }
    uint32_t plen;
    uint32_t iterations;
    const backup_result_t r = backup_peek(file, len, &plen, &iterations);
    if (r != BACKUP_OK) {
        return r;
    }
    if (cap < plen) {
        return BACKUP_ERR_ARG;
    }
    if (bounded_len(password, BACKUP_PASSWORD_MAX) > BACKUP_PASSWORD_MAX) {
        return BACKUP_ERR_DECRYPT; /* no file was ever sealed with it */
    }
    uint8_t key[BACKUP_KEY_LEN];
    if (!derive(c, password, file + 12, iterations, key)) {
        return BACKUP_ERR_CRYPTO;
    }
    const bool ok = c->gcm_decrypt(c->ctx, key, file + 28, file, BACKUP_HEADER_LEN,
                                   file + BACKUP_HEADER_LEN, plen, file + BACKUP_HEADER_LEN + plen,
                                   payload);
    wipe(key, sizeof(key));
    if (!ok) {
        wipe(payload, plen);
        return BACKUP_ERR_DECRYPT;
    }
    *payload_len = plen;
    return BACKUP_OK;
}

const char *backup_result_name(backup_result_t r)
{
    switch (r) {
    case BACKUP_OK: return "ok";
    case BACKUP_ERR_ARG: return "bad argument";
    case BACKUP_ERR_PASSWORD: return "password too short or too long";
    case BACKUP_ERR_FORMAT: return "not a backup file this firmware can read";
    case BACKUP_ERR_DECRYPT: return "wrong password, or the file was changed";
    case BACKUP_ERR_CRYPTO: return "encryption failed";
    case BACKUP_ERR_BOARD: return "made on another kind of panel";
    case BACKUP_ERR_CONTENT: return "holds something this panel refuses";
    }
    return "?";
}
