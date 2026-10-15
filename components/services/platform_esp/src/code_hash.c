/*
 * code_hash.c - PBKDF2-HMAC-SHA256 (RFC 8018) on top of PSA SHA-256.
 * The ipad/opad hash states are computed once and cloned per iteration.
 *
 * Every PSA result is checked, every operation is aborted on every exit path, and a failure is
 * never turned into a digest the engine could accept (see the failure policy in code_hash.h).
 */
#include "code_hash.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/platform_util.h" /* mbedtls_platform_zeroize: the crypto stack's secure zero */
#include "psa/crypto.h"
#include "sdkconfig.h"

#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
/* Only touched on the alarm task, inside the isolated diagnostic operation. */
static unsigned s_fail_clone_after;
static psa_status_t checked_clone(const psa_hash_operation_t *src, psa_hash_operation_t *dst)
{
    if (s_fail_clone_after != 0u && --s_fail_clone_after == 0u) {
        return PSA_ERROR_INSUFFICIENT_MEMORY;
    }
    return psa_hash_clone(src, dst);
}
#else
#define checked_clone psa_hash_clone
#endif

#define SHA_BLOCK 64u
#define SHA_LEN   32u

static const char *TAG = "code_hash";

/* Hashing itself only runs on the alarm task (the engine's port), but the gates around it are
 * read and the retry is driven from whichever task is changing a code, so these two are touched
 * with relaxed atomics. */
static bool s_ready;      /* PSA is up and the self-test passed at least once */
static uint32_t s_faults; /* hash operations that failed; only ever grows */

typedef struct {
    psa_hash_operation_t inner; /* state after absorbing key ^ ipad */
    psa_hash_operation_t outer; /* state after absorbing key ^ opad */
} hmac_ctx_t;

/* Sensitive temporaries go through the crypto stack's own zeroize, which the compiler may not
 * elide the way it may elide a memset() to a dead buffer. */
static void wipe(void *buf, size_t len)
{
    mbedtls_platform_zeroize(buf, len);
}

static psa_status_t hmac_setup(hmac_ctx_t *h, const uint8_t *key, size_t key_len)
{
    uint8_t k0[SHA_BLOCK] = {0};
    uint8_t pad[SHA_BLOCK] = {0};
    psa_status_t st = PSA_ERROR_GENERIC_ERROR;

    h->inner = psa_hash_operation_init();
    h->outer = psa_hash_operation_init();

    if (key_len > SHA_BLOCK) {
        size_t n = 0;
        st = psa_hash_compute(PSA_ALG_SHA_256, key, key_len, k0, SHA_LEN, &n);
        if (st != PSA_SUCCESS || n != SHA_LEN) {
            st = (st != PSA_SUCCESS) ? st : PSA_ERROR_GENERIC_ERROR;
            goto fail;
        }
    } else {
        memcpy(k0, key, key_len);
    }

    for (unsigned i = 0; i < SHA_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x36u);
    }
    st = psa_hash_setup(&h->inner, PSA_ALG_SHA_256);
    if (st != PSA_SUCCESS) {
        goto fail;
    }
    st = psa_hash_update(&h->inner, pad, SHA_BLOCK);
    if (st != PSA_SUCCESS) {
        goto fail;
    }
    for (unsigned i = 0; i < SHA_BLOCK; i++) {
        pad[i] = (uint8_t)(k0[i] ^ 0x5Cu);
    }
    st = psa_hash_setup(&h->outer, PSA_ALG_SHA_256);
    if (st != PSA_SUCCESS) {
        goto fail;
    }
    st = psa_hash_update(&h->outer, pad, SHA_BLOCK);
    if (st != PSA_SUCCESS) {
        goto fail;
    }
    wipe(k0, sizeof(k0));
    wipe(pad, sizeof(pad));
    return PSA_SUCCESS;

fail:
    psa_hash_abort(&h->inner); /* both handles are released before they go out of scope */
    psa_hash_abort(&h->outer);
    wipe(k0, sizeof(k0));
    wipe(pad, sizeof(pad));
    return st;
}

/* out may alias msg: msg is fully absorbed before out is written. */
static psa_status_t hmac_run(const hmac_ctx_t *h, const uint8_t *msg, size_t msg_len,
                             uint8_t out[SHA_LEN])
{
    psa_hash_operation_t op = psa_hash_operation_init();
    uint8_t inner[SHA_LEN] = {0};
    size_t n = 0;

    psa_status_t st = checked_clone(&h->inner, &op);
    if (st == PSA_SUCCESS) {
        st = psa_hash_update(&op, msg, msg_len);
    }
    if (st == PSA_SUCCESS) {
        st = psa_hash_finish(&op, inner, SHA_LEN, &n);
    }
    psa_hash_abort(&op); /* no-op after a clean finish; releases the operation after a failure */
    if (st != PSA_SUCCESS || n != SHA_LEN) {
        wipe(inner, sizeof(inner));
        return (st != PSA_SUCCESS) ? st : PSA_ERROR_GENERIC_ERROR;
    }

    op = psa_hash_operation_init(); /* only reinitialised after the abort above */
    n = 0;
    st = checked_clone(&h->outer, &op);
    if (st == PSA_SUCCESS) {
        st = psa_hash_update(&op, inner, SHA_LEN);
    }
    if (st == PSA_SUCCESS) {
        st = psa_hash_finish(&op, out, SHA_LEN, &n);
    }
    psa_hash_abort(&op);
    wipe(inner, sizeof(inner));
    if (st != PSA_SUCCESS || n != SHA_LEN) {
        wipe(out, SHA_LEN); /* never leave a half-written digest behind */
        return (st != PSA_SUCCESS) ? st : PSA_ERROR_GENERIC_ERROR;
    }
    return PSA_SUCCESS;
}

/* First 32-byte block of PBKDF2-HMAC-SHA256. salt_len <= 60. out is written only on success. */
static psa_status_t pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt,
                                  size_t salt_len, uint32_t iterations, uint8_t out[SHA_LEN])
{
    hmac_ctx_t h;
    uint8_t block_salt[64] = {0};
    uint8_t u[SHA_LEN] = {0};
    uint8_t t[SHA_LEN] = {0};

    if (salt_len + 4u > sizeof(block_salt) || iterations == 0u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    psa_status_t st = hmac_setup(&h, pw, pw_len);
    if (st != PSA_SUCCESS) {
        return st; /* hmac_setup already aborted both operations */
    }

    /* U1 = HMAC(pw, salt || INT(1)); Ui = HMAC(pw, Ui-1); T = U1 ^ U2 ^ ... */
    memcpy(block_salt, salt, salt_len);
    block_salt[salt_len + 0u] = 0;
    block_salt[salt_len + 1u] = 0;
    block_salt[salt_len + 2u] = 0;
    block_salt[salt_len + 3u] = 1;

    st = hmac_run(&h, block_salt, salt_len + 4u, u);
    if (st == PSA_SUCCESS) {
        memcpy(t, u, SHA_LEN);
        for (uint32_t it = 1; it < iterations; it++) {
            st = hmac_run(&h, u, SHA_LEN, u);
            if (st != PSA_SUCCESS) {
                break;
            }
            for (unsigned i = 0; i < SHA_LEN; i++) {
                t[i] ^= u[i];
            }
        }
    }
    if (st == PSA_SUCCESS) {
        memcpy(out, t, SHA_LEN);
    }

    psa_hash_abort(&h.inner);
    psa_hash_abort(&h.outer);
    wipe(block_salt, sizeof(block_salt));
    wipe(u, sizeof(u));
    wipe(t, sizeof(t));
    return st;
}

/* Known answers for PBKDF2-HMAC-SHA256("password", "salt", c, 32). */
static bool self_test(void)
{
    static const uint8_t c1[SHA_LEN] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c, 0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48, 0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b};
    static const uint8_t c2[SHA_LEN] = {
        0xae, 0x4d, 0x0c, 0x95, 0xaf, 0x6b, 0x46, 0xd3, 0x2d, 0x0a, 0xdf, 0xf9, 0x28, 0xf0, 0x6d, 0xd0,
        0x2a, 0x30, 0x3f, 0x8e, 0xf3, 0xc2, 0x51, 0xdf, 0xd6, 0xe2, 0xd8, 0x5a, 0x95, 0x47, 0x4c, 0x43};
    static const uint8_t c4096[SHA_LEN] = {
        0xc5, 0xe4, 0x78, 0xd5, 0x92, 0x88, 0xc8, 0x41, 0xaa, 0x53, 0x0d, 0xb6, 0x84, 0x5c, 0x4c, 0x8d,
        0x96, 0x28, 0x93, 0xa0, 0x01, 0xce, 0x4e, 0x11, 0xa4, 0x96, 0x38, 0x73, 0xaa, 0x98, 0x13, 0x4a};
    static const struct {
        uint32_t iterations;
        const uint8_t *expect;
    } vectors[] = {{1, c1}, {2, c2}, {4096, c4096}};

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t out[SHA_LEN] = {0};
        const psa_status_t st = pbkdf2_sha256((const uint8_t *)"password", 8,
                                              (const uint8_t *)"salt", 4, vectors[i].iterations,
                                              out);
        if (st != PSA_SUCCESS) {
            ESP_LOGE(TAG, "PBKDF2 self-test could not run (c=%u, psa %d)",
                     (unsigned)vectors[i].iterations, (int)st);
            return false;
        }
        if (memcmp(out, vectors[i].expect, SHA_LEN) != 0) {
            ESP_LOGE(TAG, "PBKDF2 self-test failed (c=%u)", (unsigned)vectors[i].iterations);
            return false;
        }
    }
    return true;
}

esp_err_t code_hash_init(void)
{
    if (__atomic_load_n(&s_ready, __ATOMIC_RELAXED)) {
        return ESP_OK;
    }
    const psa_status_t st = psa_crypto_init(); /* idempotent per the PSA specification */
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        return ESP_ERR_INVALID_STATE;
    }
    if (!self_test()) {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_ready, true, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "PBKDF2-HMAC-SHA256 self-test passed");
    return ESP_OK;
}

esp_err_t code_hash_status(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_RELAXED) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

uint32_t code_hash_faults(void)
{
    return __atomic_load_n(&s_faults, __ATOMIC_RELAXED);
}

/* Hashing failed. Hand back an unpredictable value so the comparison in the engine fails
 * closed, and never all zeros: an all-zero digest would match a credential that was itself
 * created while the crypto was broken.
 *
 * Readiness is deliberately NOT dropped here. It used to be, so that the next call would
 * re-initialise PSA and re-run the known-answer test - but the known-answer test proves the
 * algorithm, which cannot change while the panel runs, and re-running it costs 4099 HMAC
 * operations on the alarm task. When the failure was transient resource pressure (PSA
 * returning INSUFFICIENT_MEMORY), that retry needs more memory than the hash that just
 * failed, so it fails too - and then every later code entry re-runs it and fails again. The
 * panel latched into "no code can be verified", i.e. nobody could disarm it or open installer
 * mode until it was restarted. A failure here is one failed operation, not a broken
 * algorithm: the caller sees it through code_hash_faults() and the next attempt simply tries
 * the hash again. */
static void fail_closed(uint8_t out[SHA_LEN], psa_status_t st)
{
    __atomic_fetch_add(&s_faults, 1u, __ATOMIC_RELAXED);
    esp_fill_random(out, SHA_LEN);
    out[0] = (uint8_t)(out[0] | 1u); /* non-zero even if the RNG is broken too */
    ESP_LOGE(TAG, "code hashing failed (psa %d): this code could not be checked; "
                 "internal free=%u largest=%u", (int)st,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

esp_err_t code_hash_derive(const void *secret, size_t secret_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t out[32])
{
    if (secret == NULL || salt == NULL || code_hash_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return pbkdf2_sha256(secret, secret_len, salt, salt_len, iterations, out) == PSA_SUCCESS
               ? ESP_OK
               : ESP_FAIL;
}

void code_hash_pbkdf2(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                      uint8_t out[ALARM_CODE_HASH_LEN])
{
    (void)ctx;
    _Static_assert(ALARM_CODE_HASH_LEN == SHA_LEN, "one PBKDF2 block");
    _Static_assert(ALARM_CODE_SALT_LEN <= 60, "salt || INT(1) must fit");

    if (code == NULL) {
        fail_closed(out, PSA_ERROR_INVALID_ARGUMENT);
        return;
    }
    if (code_hash_init() != ESP_OK) { /* returns at once when it is already up */
        fail_closed(out, PSA_ERROR_BAD_STATE);
        return;
    }
    const psa_status_t st = pbkdf2_sha256((const uint8_t *)code, strlen(code), salt,
                                          ALARM_CODE_SALT_LEN, CODE_HASH_ITERATIONS, out);
    if (st != PSA_SUCCESS) {
        fail_closed(out, st);
    }
}

#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
/* Call only on the alarm task. Test data is unrelated to the engine's stored credentials. */
esp_err_t code_hash_diagnostic(unsigned fail_after)
{
    const uint8_t salt[ALARM_CODE_SALT_LEN] = {0x42, 0x30, 0x30, 0x31};
    uint8_t expected[SHA_LEN], actual[SHA_LEN];
    uint32_t faults = code_hash_faults();
    code_hash_pbkdf2(NULL, salt, "test-only-B001", expected);
    if (code_hash_faults() != faults || code_hash_status() != ESP_OK) {
        return ESP_FAIL;
    }
    if (fail_after != 0u) {
        s_fail_clone_after = fail_after;
        code_hash_pbkdf2(NULL, salt, "test-only-B001", actual);
        const bool injected = s_fail_clone_after == 0u;
        s_fail_clone_after = 0;
        if (!injected || code_hash_faults() != faults + 1u ||
            code_hash_status() != ESP_OK || memcmp(expected, actual, SHA_LEN) == 0) {
            return ESP_FAIL;
        }
        faults++;
    }
    code_hash_pbkdf2(NULL, salt, "test-only-B001", actual);
    const bool ok = code_hash_faults() == faults && code_hash_status() == ESP_OK &&
                    memcmp(expected, actual, SHA_LEN) == 0;
    wipe(expected, sizeof(expected));
    wipe(actual, sizeof(actual));
    return ok ? ESP_OK : ESP_FAIL;
}
#endif
