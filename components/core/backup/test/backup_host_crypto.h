/*
 * backup_host_crypto.h - the backup file's crypto port on a PC (backup.h): PBKDF2-HMAC-SHA256
 * (RFC 8018 §5.2, RFC 2104) on the project's SHA-256 (alarm_proto's proto_sha256), and
 * AES-256-GCM (FIPS-197, NIST SP 800-38D), written from the standards for the host tests and
 * checked against NIST's and Python's answers (test_backup.c). The firmware's port is PSA.
 *
 * Not for a panel: the random numbers are SplitMix64 (predictable on purpose, so a test repeats
 * itself) and the AES looks bytes up in a table (not constant-time).
 */
#ifndef BACKUP_HOST_CRYPTO_H
#define BACKUP_HOST_CRYPTO_H

#include <stdint.h>

#include "backup/backup.h"

typedef enum {
    BHC_FAIL_NONE = 0,
    BHC_FAIL_PBKDF2,
    BHC_FAIL_ENCRYPT,
    BHC_FAIL_RANDOM,
} bhc_fail_t;

typedef struct {
    uint8_t sbox[256];
    uint64_t rng;
    bhc_fail_t fail;
    uint32_t pbkdf2_calls;
} backup_host_crypto_t;

void backup_host_crypto_init(backup_host_crypto_t *hc, uint64_t seed);
backup_crypto_t backup_host_crypto_port(backup_host_crypto_t *hc);

/* The primitives, for the known-answer tests. */
void bhc_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                       uint32_t iterations, uint8_t *out, size_t out_len);
void bhc_aes256_encrypt(const backup_host_crypto_t *hc, const uint8_t key[32], const uint8_t in[16],
                        uint8_t out[16]);
void bhc_gcm_encrypt(const backup_host_crypto_t *hc, const uint8_t key[32],
                     const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]);

#endif /* BACKUP_HOST_CRYPTO_H */
