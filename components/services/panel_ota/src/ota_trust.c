/*
 * ota_trust.c - the second trusted signing key behind ESP-IDF's own check (OTA-020).
 *
 * Every signature check of an update image or a language pack goes through
 * esp_secure_boot_verify_sbv2_signature_block(), which trusts only the key of the running image
 * (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT). The linker wraps it (CMakeLists.txt,
 * -Wl,--wrap): ESP-IDF's check runs first and its "yes" stands. Only when it says no is the one
 * key the installer accepted for the current server tried, on the same terms ESP-IDF applies to
 * its own key: signature block 0 must be well formed (magic, version, CRC), its key's digest must
 * equal the accepted key's, and ESP-IDF's verify_rsa_signature_block() must verify the block's
 * RSA-PSS signature over the image's digest with that very key. Anything else - no accepted key,
 * a damaged block, another key, a bad signature, out of memory - returns ESP-IDF's own refusal.
 *
 * The accepted key is a RAM copy of the trust record (NVS "ota"/"trust"), loaded at start on the
 * start-up task and replaced only by the update task while no update runs and nothing holds the
 * update machinery (panel_ota.c, OTA-030), so it is read here without a lock. It is in internal
 * RAM like the rest of this file's data.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "alarm_proto/proto_fwsrv.h"
#include "esp_log.h"
#include "panel_ota_internal.h"
#include "sdkconfig.h"

#if CONFIG_SECURE_SIGNED_ON_UPDATE && CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME

#include "esp_image_format.h"
#include "esp_secure_boot.h"

static const char *TAG = "ota_trust";

/* proto_fwsrv_sigblock_key() reads the block in the layout ESP-IDF appends. */
_Static_assert(sizeof(ets_secure_boot_sig_block_t) == 1216u, "Secure Boot v2 RSA block layout");

static uint8_t s_key[32];
static volatile bool s_has_key;

void panel_ota_trust_set(const uint8_t *key_sha256)
{
    s_has_key = false;
    __sync_synchronize();
    if (key_sha256 == NULL) {
        memset(s_key, 0, sizeof(s_key));
        return;
    }
    memcpy(s_key, key_sha256, sizeof(s_key));
    __sync_synchronize();
    s_has_key = true;
}

bool panel_ota_trust_own_key(uint8_t out[32])
{
    esp_image_sig_public_key_digests_t d;
    memset(&d, 0, sizeof(d));
    /* The same digests ESP-IDF's check trusts (secure_boot_signatures_app.c): block 0 first. */
    if (esp_secure_boot_get_signature_blocks_for_running_app(true, &d) != ESP_OK || d.num_digests == 0u) {
        memset(out, 0, 32u);
        return false;
    }
    memcpy(out, d.key_digests[0], 32u);
    return true;
}

/* ESP-IDF's own, unwrapped (bootloader_support). */
esp_err_t __real_esp_secure_boot_verify_sbv2_signature_block(const ets_secure_boot_signature_t *sig_block,
                                                             const uint8_t *image_digest,
                                                             uint8_t *verified_digest);
/* bootloader_support/src/secure_boot_v2/secure_boot_rsa_signature.c: PSA RSA-PSS (SHA-256) of
 * image_digest against trusted_block's key and signature; ESP_OK only when it verifies. */
esp_err_t verify_rsa_signature_block(const ets_secure_boot_signature_t *sig_block, const uint8_t *image_digest,
                                     const ets_secure_boot_sig_block_t *trusted_block);

esp_err_t __wrap_esp_secure_boot_verify_sbv2_signature_block(const ets_secure_boot_signature_t *sig_block,
                                                             const uint8_t *image_digest,
                                                             uint8_t *verified_digest);

esp_err_t __wrap_esp_secure_boot_verify_sbv2_signature_block(const ets_secure_boot_signature_t *sig_block,
                                                             const uint8_t *image_digest,
                                                             uint8_t *verified_digest)
{
    const esp_err_t own = __real_esp_secure_boot_verify_sbv2_signature_block(sig_block, image_digest,
                                                                             verified_digest);
    if (own == ESP_OK) {
        return ESP_OK;
    }
    const esp_err_t refused = own;
    /* ESP-IDF's app-side check only ever zeroes verified_digest (it is filled by the
     * bootloader's check); the same here, whatever happens below. */
    if (verified_digest != NULL) {
        memset(verified_digest, 0, ESP_SECURE_BOOT_DIGEST_LEN);
    }
    if (sig_block == NULL || image_digest == NULL || !s_has_key) {
        return refused;
    }
    uint8_t accepted[32];
    memcpy(accepted, s_key, sizeof(accepted));
    __sync_synchronize();
    if (!s_has_key) {
        return refused; /* replaced while copied */
    }
    const ets_secure_boot_sig_block_t *blk = &sig_block->block[0];
    uint8_t digest[32];
    if (!proto_fwsrv_sigblock_key((const uint8_t *)blk, sizeof(*blk), digest)) {
        return refused;
    }
    if (memcmp(digest, accepted, sizeof(digest)) != 0) {
        return refused;
    }
    if (verify_rsa_signature_block(sig_block, image_digest, blk) != ESP_OK) {
        ESP_LOGW(TAG, "signed with the accepted key, but the signature does not verify");
        return refused;
    }
    char fp[PROTO_FWSRV_FINGERPRINT_LEN];
    proto_fwsrv_fingerprint(accepted, fp);
    ESP_LOGI(TAG, "signature verified with the key accepted for the update server (%s)", fp);
    return ESP_OK;
}

#else /* not RSA-signed updates: nothing to add a key to */

void panel_ota_trust_set(const uint8_t *key_sha256)
{
    (void)key_sha256;
}

bool panel_ota_trust_own_key(uint8_t out[32])
{
    memset(out, 0, 32u);
    return false;
}

#endif
