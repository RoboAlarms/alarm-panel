/*
 * code_hash.h - PBKDF2-HMAC-SHA256 for user codes (alarm_ports_t.hash_code).
 *
 * Built on PSA SHA-256 (hardware accelerated) because ESP-IDF 6.x doesn't enable PSA's
 * PBKDF2. Changing CODE_HASH_ITERATIONS invalidates every stored code.
 *
 * Failure policy: hashing never reports a success the crypto did not deliver. alarm_hash_fn
 * returns nothing, so a PSA failure increments code_hash_faults() and overwrites the output
 * with an unpredictable nonzero value. The engine's hash_faults port MUST be connected:
 * it rejects the failed operation before comparison, keypad lockout accounting or storage.
 * A failed operation does not revoke the startup self-test: code_hash_status() stays ready.
 * Startup/default-user creation also checks the counter before accepting any new hashes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODE_HASH_ITERATIONS 1024u

/* Brings PSA up and runs the known-answer self-test. Idempotent, and retried on every call
 * until it succeeds, so a transient failure can be recovered from. ESP_OK when hashing works;
 * ESP_ERR_INVALID_STATE when it does not, and then no code may be created or verified. */
esp_err_t code_hash_init(void);

/* Cheap query: ESP_OK once the startup known-answer test passed. Transient operation
 * failures are reported separately by code_hash_faults(), never latched into readiness. */
esp_err_t code_hash_status(void);

/* Number of hash operations that failed. Only ever grows: sample it before and after an
 * operation that stores a hash to find out whether that hash can be trusted. */
uint32_t code_hash_faults(void);

#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
/* Development test: alarm task only; 0 = normal repeat, 1..2048 = one failed clone. */
esp_err_t code_hash_diagnostic(unsigned fail_after);
#endif

/* PBKDF2-HMAC-SHA256, first 32-byte block, for other secrets than codes (the Panel Manager's
 * passwords). Any task. salt_len <= 60. ESP_OK only when out holds the real digest; out is
 * left untouched otherwise. Does not count toward code_hash_faults(). */
esp_err_t code_hash_derive(const void *secret, size_t secret_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t out[32]);

/* Matches alarm_hash_fn. Never silently substitutes a usable-looking hash: see the failure
 * policy above. */
void code_hash_pbkdf2(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                      uint8_t out[ALARM_CODE_HASH_LEN]);

#ifdef __cplusplus
}
#endif
