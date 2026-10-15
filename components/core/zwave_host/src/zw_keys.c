/*
 * zw_keys.c - S2's X25519 key pair and shared secret (docs/features/36 security.md §2, RFC 7748
 * §6.1) and S0's authentication and encryption keys (§10); see zw_crypto.h.
 */
#include "zw_crypto_internal.h"

/* The base point: u = 9, little-endian. */
static const uint8_t ZW_X25519_BASE[32] = {9};

bool zw_s2_keypair(const zw_crypto_port_t *port, uint8_t priv[32], uint8_t pub[32])
{
    uint8_t k[32] = {0}, p[32] = {0};
    const bool ok = port != NULL && port->random != NULL && port->x25519 != NULL &&
                    priv != NULL && pub != NULL && port->random(port->ctx, k, sizeof k) &&
                    port->x25519(port->ctx, p, k, ZW_X25519_BASE);
    if (ok) {
        memcpy(priv, k, sizeof k);
        memcpy(pub, p, sizeof p);
    } else {
        zw_wipe_out(priv, ZW_X25519_LEN);
        zw_wipe_out(pub, ZW_X25519_LEN);
    }
    zw_wipe(k, sizeof k);
    zw_wipe(p, sizeof p);
    return ok;
}

bool zw_s2_shared_secret(const zw_crypto_port_t *port, const uint8_t priv[32],
                         const uint8_t peer_pub[32], uint8_t shared[32])
{
    static const uint8_t zero[32] = {0};
    uint8_t s[32] = {0};
    bool ok = port != NULL && port->x25519 != NULL && priv != NULL && peer_pub != NULL &&
              shared != NULL && port->x25519(port->ctx, s, priv, peer_pub);
    /* A small-order peer point makes the result all zero whatever our key: refuse it, reading
     * every byte (RFC 7748 §6.1). Refusing is public; the bytes are not. */
    ok = ok && !zw_ct_equal(s, zero, sizeof s);
    if (ok) {
        memcpy(shared, s, sizeof s);
    } else {
        zw_wipe_out(shared, ZW_X25519_LEN);
    }
    zw_wipe(s, sizeof s);
    return ok;
}

bool zw_s0_keys(const zw_crypto_port_t *port, const uint8_t s0key[16], uint8_t auth_key[16],
                uint8_t enc_key[16])
{
    uint8_t c55[16], caa[16], a[16] = {0}, e[16] = {0};
    memset(c55, 0x55, sizeof c55);
    memset(caa, 0xAA, sizeof caa);
    const bool ok = zw_has_aes(port) && s0key != NULL && auth_key != NULL && enc_key != NULL &&
                    zw_aes_block(port, s0key, c55, a) && zw_aes_block(port, s0key, caa, e);
    if (ok) {
        memcpy(auth_key, a, sizeof a); /* AuthKey = E(key, 55 x16) */
        memcpy(enc_key, e, sizeof e);  /* EncKey = E(key, AA x16) */
    } else {
        zw_wipe_out(auth_key, ZW_AES_KEY_LEN);
        zw_wipe_out(enc_key, ZW_AES_KEY_LEN);
    }
    zw_wipe(a, sizeof a);
    zw_wipe(e, sizeof e);
    return ok;
}
