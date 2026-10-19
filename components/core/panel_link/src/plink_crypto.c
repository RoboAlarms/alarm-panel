/*
 * plink_crypto.c - the panel link's cryptography (see include/panel_link/plink_crypto.h;
 * docs/features/11 "Pairing", "Sessions"; LINK-003, LINK-004, LINK-015): SHA-256 over pieces,
 * HMAC-SHA256 (RFC 2104) and HKDF-SHA256 (RFC 5869) on alarm_proto's streaming SHA-256; X25519
 * with the all-zero check and AES-128-CCM (zwave_host's) on the port's three primitives; the
 * start-up self-check.
 */
#include "panel_link/plink_crypto.h"

#include <string.h>

#include "alarm_proto/proto_sha256.h"
#include "zwave_host/zw_crypto.h"

#define HMAC_BLOCK 64u                       /* SHA-256's block */
#define HKDF_MAX   (255u * PLINK_SHA256_LEN) /* RFC 5869 §2.3: L <= 255 HashLen */

/* The base point, u = 9 (RFC 7748 §4.1), little-endian. */
static const uint8_t BASE[PLINK_X25519_LEN] = {9};

/* ---- hygiene ------------------------------------------------------------------------------ */

bool plink_ct_equal(const void *a, const void *b, size_t len)
{
    if (len == 0u) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    /* Volatile reads and no early exit: every byte is read whatever the data says. */
    const volatile uint8_t *va = a;
    const volatile uint8_t *vb = b;
    unsigned diff = 0u;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned)va[i] ^ (unsigned)vb[i];
    }
    return diff == 0u;
}

void plink_wipe(void *p, size_t len)
{
    if (p == NULL) {
        return;
    }
    volatile uint8_t *v = p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0u;
    }
}

/* ---- SHA-256, HMAC, HKDF ------------------------------------------------------------------ */

static void sha_parts(proto_sha256_ctx_t *s, const plink_iov_t *parts, size_t n)
{
    for (size_t i = 0; parts != NULL && i < n; i++) {
        proto_sha256_update(s, parts[i].p, parts[i].len);
    }
}

void plink_sha256(const plink_iov_t *parts, size_t n, uint8_t out[PLINK_SHA256_LEN])
{
    proto_sha256_ctx_t s;
    proto_sha256_init(&s);
    sha_parts(&s, parts, n);
    proto_sha256_final(&s, out);
    plink_wipe(&s, sizeof s); /* a transcript may hold a secret */
}

/* HMAC in pieces (RFC 2104): the inner hash started on K0 ^ ipad, the outer on K0 ^ opad. Both
 * hold key material: hmac_final wipes them. */
typedef struct {
    proto_sha256_ctx_t inner;
    proto_sha256_ctx_t outer;
} hmac_t;

static void hmac_init(hmac_t *m, const uint8_t *key, size_t key_len)
{
    uint8_t k0[HMAC_BLOCK] = {0};
    uint8_t pad[HMAC_BLOCK];
    if (key == NULL) {
        key_len = 0u;
    }
    if (key_len > HMAC_BLOCK) {
        /* A key longer than a block is hashed first; a shorter one is padded with zeros (so an
         * empty key is HashLen zeros too, HKDF's default salt). */
        proto_sha256_ctx_t s;
        proto_sha256_init(&s);
        proto_sha256_update(&s, key, key_len);
        proto_sha256_final(&s, k0);
        plink_wipe(&s, sizeof s);
    } else if (key_len > 0u) {
        memcpy(k0, key, key_len);
    }
    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x36u);
    }
    proto_sha256_init(&m->inner);
    proto_sha256_update(&m->inner, pad, sizeof pad);
    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x5Cu);
    }
    proto_sha256_init(&m->outer);
    proto_sha256_update(&m->outer, pad, sizeof pad);
    plink_wipe(k0, sizeof k0);
    plink_wipe(pad, sizeof pad);
}

/* out = H(K0 ^ opad || H(K0 ^ ipad || message)); written last, so out may be a key or a piece. */
static void hmac_final(hmac_t *m, uint8_t out[PLINK_SHA256_LEN])
{
    uint8_t ih[PLINK_SHA256_LEN];
    proto_sha256_final(&m->inner, ih);
    proto_sha256_update(&m->outer, ih, sizeof ih);
    proto_sha256_final(&m->outer, out);
    plink_wipe(ih, sizeof ih);
    plink_wipe(m, sizeof *m);
}

void plink_hmac(const uint8_t *key, size_t key_len, const plink_iov_t *parts, size_t n,
                uint8_t out[PLINK_SHA256_LEN])
{
    hmac_t m;
    hmac_init(&m, key, key_len);
    sha_parts(&m.inner, parts, n);
    hmac_final(&m, out);
}

void plink_hkdf_extract(const uint8_t *salt, size_t salt_len, const plink_iov_t *ikm, size_t n,
                        uint8_t prk[PLINK_SHA256_LEN])
{
    /* No salt is HashLen zeros (RFC 5869 §2.2), which HMAC's zero padding makes of an empty key. */
    plink_hmac(salt, salt != NULL ? salt_len : 0u, ikm, n, prk);
}

bool plink_hkdf_expand(const uint8_t prk[PLINK_SHA256_LEN], const plink_iov_t *info, size_t n,
                       uint8_t *out, size_t len)
{
    if (prk == NULL || out == NULL || len == 0u || len > HKDF_MAX) {
        plink_wipe(out, len);
        return false;
    }
    /* T(i) = HMAC(PRK, T(i-1) || info || i), T(0) empty; OKM = T(1) || T(2) ... cut to len. The
     * key is taken once, so out may overlap prk. */
    hmac_t base, m;
    hmac_init(&base, prk, PLINK_SHA256_LEN);
    uint8_t t[PLINK_SHA256_LEN];
    size_t t_len = 0u;
    uint8_t counter = 0u;
    for (size_t done = 0; done < len;) {
        counter++;
        m = base;
        proto_sha256_update(&m.inner, t, t_len);
        sha_parts(&m.inner, info, n);
        proto_sha256_update(&m.inner, &counter, 1u);
        hmac_final(&m, t);
        t_len = sizeof t;
        const size_t take = len - done < sizeof t ? len - done : sizeof t;
        memcpy(&out[done], t, take);
        done += take;
    }
    plink_wipe(&base, sizeof base);
    plink_wipe(t, sizeof t);
    return true;
}

/* ---- X25519 ------------------------------------------------------------------------------- */

bool plink_x25519_keypair(const plink_crypto_t *c, uint8_t priv[PLINK_X25519_LEN],
                          uint8_t pub[PLINK_X25519_LEN])
{
    /* priv is 32 random bytes as they come: X25519 clamps them (RFC 7748 §5). The public key goes
     * through a buffer of its own, so the port never sees an output overlapping its input. */
    uint8_t p[PLINK_X25519_LEN] = {0};
    bool ok = c != NULL && c->random != NULL && c->x25519 != NULL && priv != NULL && pub != NULL;
    ok = ok && c->random(c->ctx, priv, PLINK_X25519_LEN);
    ok = ok && c->x25519(c->ctx, p, priv, BASE);
    if (ok) {
        memcpy(pub, p, sizeof p);
    } else {
        plink_wipe(priv, priv != NULL ? PLINK_X25519_LEN : 0u);
        plink_wipe(pub, pub != NULL ? PLINK_X25519_LEN : 0u);
    }
    plink_wipe(p, sizeof p);
    return ok;
}

bool plink_x25519_shared(const plink_crypto_t *c, const uint8_t priv[PLINK_X25519_LEN],
                         const uint8_t peer[PLINK_X25519_LEN], uint8_t shared[PLINK_X25519_LEN])
{
    static const uint8_t zero[PLINK_X25519_LEN] = {0};
    uint8_t s[PLINK_X25519_LEN] = {0};
    bool ok = c != NULL && c->x25519 != NULL && priv != NULL && peer != NULL && shared != NULL;
    ok = ok && c->x25519(c->ctx, s, priv, peer);
    /* A small-order peer key makes the secret all zero whatever priv is (RFC 7748 §6.1): checked
     * over every byte, without an early exit. Whether it was zero is public; the bytes are not. */
    ok = ok && !plink_ct_equal(s, zero, sizeof s);
    if (ok) {
        memcpy(shared, s, sizeof s);
    } else {
        plink_wipe(shared, shared != NULL ? PLINK_X25519_LEN : 0u);
    }
    plink_wipe(s, sizeof s);
    return ok;
}

/* ---- AES-128-CCM -------------------------------------------------------------------------- */

/* The port as zwave_host takes it: the same three primitives, field by field. */
static zw_crypto_port_t zw_port(const plink_crypto_t *c)
{
    zw_crypto_port_t p = {0};
    p.ctx = c->ctx;
    p.aes128_encrypt = c->aes128_encrypt;
    p.x25519 = c->x25519;
    p.random = c->random;
    return p;
}

bool plink_ccm_seal(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                    const uint8_t nonce[PLINK_CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[PLINK_TAG_LEN])
{
    /* No port: zwave_host refuses it and zeroes ct and tag. */
    const zw_crypto_port_t p = c != NULL ? zw_port(c) : (zw_crypto_port_t){0};
    return zw_aes_ccm_encrypt(c != NULL ? &p : NULL, key, nonce, aad, aad_len, pt, len, ct, tag);
}

bool plink_ccm_open(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                    const uint8_t nonce[PLINK_CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t len, const uint8_t tag[PLINK_TAG_LEN], uint8_t *pt)
{
    const zw_crypto_port_t p = c != NULL ? zw_port(c) : (zw_crypto_port_t){0};
    return zw_aes_ccm_decrypt(c != NULL ? &p : NULL, key, nonce, aad, aad_len, ct, len, tag, pt);
}

/* ---- random ------------------------------------------------------------------------------- */

bool plink_random(const plink_crypto_t *c, void *buf, size_t len)
{
    bool ok = c != NULL && c->random != NULL && (buf != NULL || len == 0u);
    if (ok && len > 0u) {
        ok = c->random(c->ctx, buf, len);
    }
    if (!ok) {
        plink_wipe(buf, len);
    }
    return ok;
}

/* ---- the start-up self-check -------------------------------------------------------------- */

/* RFC 7748 §6.1: Alice's private key, Bob's public key, their shared secret. (zwave_host's check
 * proves Alice's public key from the base point; this proves the agreement through
 * plink_x25519_shared.) */
static const uint8_t ALICE_PRIV[32] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t BOB_PUB[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
};
static const uint8_t SHARED[32] = {
    0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1, 0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
    0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33, 0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
};

/* RFC 3610 packet vector #1 (M = 8, L = 2): key, nonce, 8 bytes of associated data and 23 of
 * message (bytes 00..1e), the ciphertext and the tag. */
static const uint8_t CCM_KEY[16] = {
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
};
static const uint8_t CCM_NONCE[13] = {
    0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
};
static const uint8_t CCM_CT[23] = {
    0x58, 0x8c, 0x97, 0x9a, 0x61, 0xc6, 0x63, 0xd2, 0xf0, 0x66, 0xd0, 0xc2,
    0xc0, 0xf9, 0x89, 0x80, 0x6d, 0x5f, 0x6b, 0x61, 0xda, 0xc3, 0x84,
};
static const uint8_t CCM_TAG[8] = {0x17, 0xe8, 0xd1, 0x2c, 0xfd, 0xf9, 0x26, 0xe0};

/* RFC 4231 test case 2: HMAC-SHA-256("Jefe", "what do ya want for nothing?"). */
static const uint8_t HMAC_TC2[32] = {
    0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
    0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
};

/* RFC 5869 A.1: salt 00..0c, info f0..f9, IKM 22 x 0b; the PRK and the 42-byte OKM. */
static const uint8_t HKDF_PRK[32] = {
    0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf, 0x0d, 0xdc, 0x3f, 0x0d, 0xc4, 0x7b, 0xba, 0x63,
    0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f, 0x9c, 0x31, 0x22, 0xec, 0x84, 0x4a, 0xd7, 0xc2, 0xb3, 0xe5,
};
static const uint8_t HKDF_OKM[42] = {
    0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36,
    0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56,
    0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65,
};

static bool agreement_ok(const plink_crypto_t *c)
{
    uint8_t s[32] = {0};
    const bool ok =
        plink_x25519_shared(c, ALICE_PRIV, BOB_PUB, s) && plink_ct_equal(s, SHARED, sizeof s);
    plink_wipe(s, sizeof s);
    return ok;
}

/* Sealed only, never opened: opening would run the same key and nonce through the port twice,
 * which the host test port reports as a reused nonce. */
static bool ccm_ok(const plink_crypto_t *c)
{
    uint8_t in[31], ct[23] = {0}, tag[8] = {0};
    for (size_t i = 0; i < sizeof in; i++) {
        in[i] = (uint8_t)i;
    }
    return plink_ccm_seal(c, CCM_KEY, CCM_NONCE, in, 8u, &in[8], sizeof ct, ct, tag) &&
           plink_ct_equal(ct, CCM_CT, sizeof ct) && plink_ct_equal(tag, CCM_TAG, sizeof tag);
}

static bool hmac_ok(void)
{
    static const char key[] = "Jefe";
    static const char msg[] = "what do ya want for nothing?";
    const plink_iov_t part = {msg, sizeof msg - 1u};
    uint8_t mac[32];
    plink_hmac((const uint8_t *)key, sizeof key - 1u, &part, 1u, mac);
    return plink_ct_equal(mac, HMAC_TC2, sizeof mac);
}

static bool hkdf_ok(void)
{
    uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
    memset(ikm, 0x0b, sizeof ikm);
    for (size_t i = 0; i < sizeof salt; i++) {
        salt[i] = (uint8_t)i;
    }
    for (size_t i = 0; i < sizeof info; i++) {
        info[i] = (uint8_t)(0xf0u + i);
    }
    const plink_iov_t ikm_part = {ikm, sizeof ikm};
    const plink_iov_t info_part = {info, sizeof info};
    plink_hkdf_extract(salt, sizeof salt, &ikm_part, 1u, prk);
    return plink_ct_equal(prk, HKDF_PRK, sizeof prk) &&
           plink_hkdf_expand(prk, &info_part, 1u, okm, sizeof okm) &&
           plink_ct_equal(okm, HKDF_OKM, sizeof okm);
}

bool plink_crypto_check(const plink_crypto_t *c)
{
    if (c == NULL || c->aes128_encrypt == NULL || c->x25519 == NULL || c->random == NULL) {
        return false;
    }
    /* zwave_host's check: FIPS-197 C.1, RFC 7748 §5.2's first vector, §6.1's Alice from the base
     * point, two random draws neither zero nor equal. Then the agreement, CCM, HMAC and HKDF on
     * this build's own code; it stops at the first failure. */
    const zw_crypto_port_t p = zw_port(c);
    return zw_crypto_port_check(&p) && agreement_ok(c) && ccm_ok(c) && hmac_ok() && hkdf_ok();
}
