/*
 * zw_crypto.h - the Z-Wave host's S2 and S0 cryptography (docs/features/36, security.md §2-3,
 * §6 "Establishing a SPAN" and §10; requirement ZW-008).
 *
 * Everything is built from three primitives the platform lends through a port: AES-128
 * encryption of one block, X25519 and a random number generator. CMAC, CCM, OFB, S0's CBC-MAC,
 * the CTR_DRBG, the CKDF key derivations and the SPAN nonces are portable code on top of them,
 * proved on the PC against security.md §11's known answers and the NIST/RFC vectors (test/).
 * The firmware fills the port from PSA (mbedTLS 4) at milestone Z8 and proves it on the panel at
 * every start (zw_crypto_port_check); the host tests and the PC simulator fill it from
 * test/zw_host_crypto.c.
 *
 * Portable C11: no heap, no globals. Every function takes the port first and returns false when
 * an argument is invalid or a port call failed; its outputs are then zeroed (a generator's state
 * is left as it was). Intermediate secrets are wiped from the stack before returning, tags are
 * compared in constant time and no branch depends on secret data.
 */
#ifndef ZW_CRYPTO_H
#define ZW_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ignoring the result of a crypto call is a bug: GCC and Clang warn about it (-Werror hosts
 * refuse to build). */
#if defined(__GNUC__) || defined(__clang__)
#define ZW_CRYPTO_MUST_CHECK __attribute__((warn_unused_result))
#else
#define ZW_CRYPTO_MUST_CHECK
#endif

#define ZW_AES_KEY_LEN      16u /* AES-128 keys: network keys, KeyCCM, PRKs, S0 keys */
#define ZW_AES_BLOCK_LEN    16u
#define ZW_X25519_LEN       32u /* private keys, public keys, shared secrets */
#define ZW_S2_PSTRING_LEN   32u /* personalization string of a key (security.md §3) */
#define ZW_S2_EI_LEN        16u /* sender / receiver entropy input */
#define ZW_S2_MEI_LEN       32u /* mixed entropy input */
#define ZW_S2_NONCE_LEN     13u /* CCM nonce: 15 - L with L = 2 */
#define ZW_S2_TAG_LEN        8u /* CCM tag (M = 8) */
#define ZW_S0_MAC_LEN        8u
#define ZW_CCM_AAD_MAX  0xFEFFu /* longest associated data with the two-byte length encoding */
#define ZW_CCM_TEXT_MAX 0xFFFFu /* longest message a two-byte length field (L = 2) can carry */
#define ZW_CTR_DRBG_MAX_REQUEST 65536u /* bytes per Generate (SP 800-90A: 2^19 bits) */

/*
 * The port: three primitives, each returning false when it failed. The portable code never
 * passes an output that overlaps an input of the same call.
 *   aes128_encrypt  out = AES-128(key, in), one block (FIPS-197).
 *                   Firmware: PSA_ALG_ECB_NO_PADDING.
 *   x25519          out = X25519(scalar, point) (RFC 7748 §5: clamps the scalar, ignores the
 *                   point's top bit, no all-zero check - zw_s2_shared_secret does that).
 *                   Firmware: psa_raw_key_agreement(PSA_ALG_ECDH) on a Montgomery-255 key.
 *   random          len bytes from a cryptographically secure generator.
 *                   Firmware: psa_generate_random.
 */
typedef struct {
    void *ctx;
    bool (*aes128_encrypt)(void *ctx, const uint8_t key[16], const uint8_t in[16],
                           uint8_t out[16]);
    bool (*x25519)(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
} zw_crypto_port_t;

/* ---- AES modes (security.md §2) ------------------------------------------------------------ */

/* AES-CMAC (RFC 4493, NIST SP 800-38B): mac = CMAC(key, msg). msg may be NULL when len is 0. */
ZW_CRYPTO_MUST_CHECK
bool zw_aes_cmac(const zw_crypto_port_t *port, const uint8_t key[16], const uint8_t *msg,
                 size_t len, uint8_t mac[16]);

/* AES-CCM (RFC 3610, NIST SP 800-38C) with a 13-byte nonce (L = 2) and an 8-byte tag (M = 8):
 * ct = the encryption of pt, tag = its encrypted tag over aad and pt. aad_len at most
 * ZW_CCM_AAD_MAX, len at most ZW_CCM_TEXT_MAX; aad / pt / ct may be NULL when their length is 0.
 * ct == pt encrypts in place; no other overlap is allowed. */
ZW_CRYPTO_MUST_CHECK
bool zw_aes_ccm_encrypt(const zw_crypto_port_t *port, const uint8_t key[16],
                        const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[8]);

/* The reverse: pt = the decryption of ct, true only when tag authenticates nonce, aad and ct
 * (compared in constant time). On false pt is all zero: no unauthenticated byte leaves.
 * pt == ct decrypts in place, but then a failure zeroes the ciphertext too: to try several
 * nonces on one frame (security.md §6), decrypt into a separate buffer. */
ZW_CRYPTO_MUST_CHECK
bool zw_aes_ccm_decrypt(const zw_crypto_port_t *port, const uint8_t key[16],
                        const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, size_t len, const uint8_t tag[8], uint8_t *pt);

/* AES-OFB (NIST SP 800-38A §6.4): out = in XOR the keystream S1 = E(key, iv),
 * Si = E(key, Si-1), the last block truncated; the same call decrypts. out == in works in
 * place. */
ZW_CRYPTO_MUST_CHECK
bool zw_aes_ofb(const zw_crypto_port_t *port, const uint8_t key[16], const uint8_t iv[16],
                const uint8_t *in, size_t len, uint8_t *out);

/* S0's CBC-MAC: data zero-padded to a multiple of 16, AES-CBC with a zero IV, the first 8 bytes
 * of the last ciphertext block. len must be at least 1. */
ZW_CRYPTO_MUST_CHECK
bool zw_s0_cbc_mac(const zw_crypto_port_t *port, const uint8_t key[16], const uint8_t *data,
                   size_t len, uint8_t mac[8]);

/* ---- CTR_DRBG (security.md §2: NIST SP 800-90A, AES-128, no derivation function) ----------- */

/* Key and V; no reseed counter (the S2 generators are never reseeded). Holds secrets: wipe it
 * with zw_wipe() when done. */
typedef struct {
    uint8_t key[16];
    uint8_t v[16];
} zw_ctr_drbg_t;

/* Instantiate(entropy, personalization): Key = 0, V = 0, Update(entropy XOR personalization).
 * personalization may be NULL (all zero). On false *d is left as it was. */
ZW_CRYPTO_MUST_CHECK
bool zw_ctr_drbg_instantiate(const zw_crypto_port_t *port, zw_ctr_drbg_t *d,
                             const uint8_t entropy[32], const uint8_t personalization[32]);

/* Generate(n), 1 <= n <= ZW_CTR_DRBG_MAX_REQUEST: V+1 and a block of E(Key, V) until n bytes,
 * then Update() with no data. On false *d is left as it was (nothing was drawn). */
ZW_CRYPTO_MUST_CHECK
bool zw_ctr_drbg_generate(const zw_crypto_port_t *port, zw_ctr_drbg_t *d, uint8_t *out, size_t n);

/* ---- Key derivation, CKDF (security.md §3) ------------------------------------------------- */

/* Network key expansion of a permanent network key: KeyCCM = T1, PString = T2 | T3,
 * KeyMPAN = T4 with T1 = CMAC(PNK, 55x15 | 01), Ti = CMAC(PNK, Ti-1 | 55x15 | i). */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_network_key_expand(const zw_crypto_port_t *port, const uint8_t pnk[16],
                              uint8_t key_ccm[16], uint8_t pstring[32], uint8_t key_mpan[16]);

/* Temporary key extract: PRK = CMAC(33x16, shared | pub_including | pub_joining) - the
 * including node's (the panel's) public key first, on both sides. */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_temp_key_extract(const zw_crypto_port_t *port, const uint8_t shared[32],
                            const uint8_t pub_including[32], const uint8_t pub_joining[32],
                            uint8_t prk[16]);

/* Temporary key expansion: temp KeyCCM = T1, temp PString = T2 | T3 with
 * T1 = CMAC(PRK, 88x15 | 01), Ti = CMAC(PRK, Ti-1 | 88x15 | i). */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_temp_key_expand(const zw_crypto_port_t *port, const uint8_t prk[16],
                           uint8_t key_ccm[16], uint8_t pstring[32]);

/* NoncePRK = CMAC(26x16, sender_ei | receiver_ei). */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_nonce_prk(const zw_crypto_port_t *port, const uint8_t sender_ei[16],
                     const uint8_t receiver_ei[16], uint8_t prk[16]);

/* Mixed entropy input: MEI = T1 | T2 with T1 = CMAC(NoncePRK, 88x15 | 00 | 88x15 | 01),
 * T2 = CMAC(NoncePRK, T1 | 88x15 | 02). */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_mei(const zw_crypto_port_t *port, const uint8_t nonce_prk[16], uint8_t mei[32]);

/* ---- SPAN nonces (security.md §6 "Establishing a SPAN") ------------------------------------ */

/* Establishes a SPAN: Instantiate(MEI(NoncePRK(sender_ei, receiver_ei)), pstring) with the
 * class's personalization string (or the temporary one). The sender of the frame carrying the
 * SPAN extension supplies sender_ei; receiver_ei came in the other side's Nonce Report. On
 * false *drbg is left as it was. */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_span_instantiate(const zw_crypto_port_t *port, zw_ctr_drbg_t *drbg,
                            const uint8_t sender_ei[16], const uint8_t receiver_ei[16],
                            const uint8_t pstring[32]);

/* The SPAN's next nonce: the first 13 bytes of Generate(16). On false *drbg is left as it was
 * (refused for want of an output, it draws nothing). */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_span_next_nonce(const zw_crypto_port_t *port, zw_ctr_drbg_t *drbg, uint8_t nonce[13]);

/* ---- Keys (security.md §2 X25519, §10 S0) -------------------------------------------------- */

/* A fresh S2 key pair: priv = 32 random bytes (X25519 clamps them), pub = X25519(priv, 9). */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_keypair(const zw_crypto_port_t *port, uint8_t priv[32], uint8_t pub[32]);

/* shared = X25519(priv, peer_pub); refuses an all-zero result (a small-order point,
 * RFC 7748 §6.1), checked without an early exit. */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_shared_secret(const zw_crypto_port_t *port, const uint8_t priv[32],
                         const uint8_t peer_pub[32], uint8_t shared[32]);

/* S0's keys from its network key: AuthKey = E(key, 55x16), EncKey = E(key, AAx16). */
ZW_CRYPTO_MUST_CHECK
bool zw_s0_keys(const zw_crypto_port_t *port, const uint8_t s0key[16], uint8_t auth_key[16],
                uint8_t enc_key[16]);

/* ---- The port's own check ------------------------------------------------------------------ */

/* A self-test of a platform's port before anything relies on it (the firmware runs it at every
 * start: its PSA port is proved on the panel's own hardware): AES-128 on FIPS-197 appendix C.1's
 * vector; X25519 on RFC 7748 §5.2's first vector, and §6.1's Alice - her public key from her
 * private key and the base point 9, the way zw_s2_keypair derives public keys; the random source:
 * two 16-byte draws, neither all zero nor equal. False when the port or a primitive is missing, a
 * call fails or an answer is wrong; it stops at the first. The vectors are public; the draws are
 * wiped. */
ZW_CRYPTO_MUST_CHECK
bool zw_crypto_port_check(const zw_crypto_port_t *port);

/* ---- Hygiene ------------------------------------------------------------------------------- */

/* True when the n bytes at a and b are equal; the time depends on n only. */
ZW_CRYPTO_MUST_CHECK
bool zw_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* Zeroes n bytes through a volatile pointer, so the compiler cannot drop the writes. */
void zw_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CRYPTO_H */
