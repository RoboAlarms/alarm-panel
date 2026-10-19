/*
 * plink_crypto.h - the panel link's cryptography (docs/features/11 "Pairing", "Sessions";
 * LINK-003, LINK-004, LINK-015).
 *
 * Built from three primitives the platform lends through a port - AES-128 on one block, X25519
 * and a random generator, the same three zwave_host uses - plus portable SHA-256 (alarm_proto),
 * HMAC-SHA256 (RFC 2104) and HKDF-SHA256 (RFC 5869) on top. AES-128-CCM with a 13-byte nonce and
 * an 8-byte tag is zwave_host's (zw_aes_ccm_*), proved on the PC against RFC 3610. The firmware
 * fills the port from PSA (plink_service) and proves it at every start (plink_crypto_check); the
 * host tests use zwave_host/test/zw_host_crypto.c; host/link_sim uses the OS's random numbers.
 *
 * Portable C11: no heap, no globals. Functions returning bool return false when an argument is
 * invalid or a port call failed; their outputs are then zeroed. Secrets on the stack are wiped
 * before returning, MACs and tags are compared in constant time.
 */
#ifndef PLINK_CRYPTO_H
#define PLINK_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PLINK_MUST_CHECK __attribute__((warn_unused_result))
#else
#define PLINK_MUST_CHECK
#endif

#define PLINK_SHA256_LEN 32u
#define PLINK_KEY_LEN    32u /* K_pair, K_conf, K_hs, K_fin, PRKs */
#define PLINK_AES_LEN    16u /* the session's data keys */
#define PLINK_X25519_LEN 32u
#define PLINK_NONCE_LEN  16u /* n_k, n_m (pairing and handshake) */
#define PLINK_MAC_LEN    16u /* mac1, mac2, mac_k, mac_m: HMAC-SHA256 cut to 16 bytes */
#define PLINK_CCM_NONCE_LEN 13u
#define PLINK_TAG_LEN    8u

/* The port: the same three primitives as zwave_host's zw_crypto_port_t (zw_crypto.h), each
 * returning false when it failed.
 *   aes128_encrypt  out = AES-128(key, in), one block (FIPS-197). Firmware: PSA ECB.
 *   x25519          out = X25519(scalar, point) (RFC 7748 §5; no all-zero check: the helpers
 *                   below do that). Firmware: psa_raw_key_agreement on a Montgomery-255 key.
 *   random          len bytes from a cryptographically secure generator. Firmware:
 *                   psa_generate_random. A port whose generator is not secure (the seeded host
 *                   test port) sets `insecure`, and the link then talks only to loopback peers
 *                   (host/link_sim, docs/features/11 "Design"). */
typedef struct {
    void *ctx;
    bool (*aes128_encrypt)(void *ctx, const uint8_t key[16], const uint8_t in[16],
                           uint8_t out[16]);
    bool (*x25519)(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
    bool insecure;
} plink_crypto_t;

/* A piece of a message: the hashes and MACs below take a list of them, so a transcript is never
 * copied into one buffer. p may be NULL when len is 0. */
typedef struct {
    const void *p;
    size_t len;
} plink_iov_t;

/* SHA-256 over the concatenation of n pieces. */
void plink_sha256(const plink_iov_t *parts, size_t n, uint8_t out[PLINK_SHA256_LEN]);

/* HMAC-SHA256 (RFC 2104, RFC 4231) of the concatenated pieces under key (any length). */
void plink_hmac(const uint8_t *key, size_t key_len, const plink_iov_t *parts, size_t n,
                uint8_t out[PLINK_SHA256_LEN]);

/* HKDF-Extract (RFC 5869 §2.2): prk = HMAC(salt, ikm), the ikm the concatenated pieces. salt may
 * be NULL (len 0: HashLen zeros). */
void plink_hkdf_extract(const uint8_t *salt, size_t salt_len, const plink_iov_t *ikm, size_t n,
                        uint8_t prk[PLINK_SHA256_LEN]);

/* HKDF-Expand (RFC 5869 §2.3): len bytes (at most 255 * 32) from prk and the concatenated info
 * pieces. false (out zeroed) when len is out of range. */
PLINK_MUST_CHECK
bool plink_hkdf_expand(const uint8_t prk[PLINK_SHA256_LEN], const plink_iov_t *info, size_t n,
                       uint8_t *out, size_t len);

/* A fresh X25519 key pair: priv from the port's generator, pub = X25519(priv, 9). */
PLINK_MUST_CHECK
bool plink_x25519_keypair(const plink_crypto_t *c, uint8_t priv[PLINK_X25519_LEN],
                          uint8_t pub[PLINK_X25519_LEN]);

/* shared = X25519(priv, peer); false (shared zeroed) when the result is all zero, which a
 * small-order peer key gives (RFC 7748 §6.1), checked in constant time. */
PLINK_MUST_CHECK
bool plink_x25519_shared(const plink_crypto_t *c, const uint8_t priv[PLINK_X25519_LEN],
                         const uint8_t peer[PLINK_X25519_LEN], uint8_t shared[PLINK_X25519_LEN]);

/* AES-128-CCM with a 13-byte nonce and an 8-byte tag (RFC 3610 L = 2, M = 8), through
 * zw_aes_ccm_encrypt/decrypt on this port. ct == pt works in place. open: pt is all zero when
 * the tag fails (and, in place, so is the ciphertext). */
PLINK_MUST_CHECK
bool plink_ccm_seal(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                    const uint8_t nonce[PLINK_CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[PLINK_TAG_LEN]);
PLINK_MUST_CHECK
bool plink_ccm_open(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                    const uint8_t nonce[PLINK_CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t len, const uint8_t tag[PLINK_TAG_LEN], uint8_t *pt);

/* len random bytes from the port. */
PLINK_MUST_CHECK
bool plink_random(const plink_crypto_t *c, void *buf, size_t len);

/* Constant-time comparison: true when equal. */
bool plink_ct_equal(const void *a, const void *b, size_t len);

/* Zeroes a secret so the compiler can't drop the write. */
void plink_wipe(void *p, size_t len);

/* The port works: FIPS-197 C.1, RFC 7748 §5.2 and §6.1, RFC 3610 packet vector #1 through
 * plink_ccm_seal, RFC 4231 test case 2 and RFC 5869 A.1, and two random draws that are neither
 * zero nor equal. The firmware runs it before the link starts and stays off when it fails. */
PLINK_MUST_CHECK
bool plink_crypto_check(const plink_crypto_t *c);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_CRYPTO_H */
