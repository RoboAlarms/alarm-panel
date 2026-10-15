/*
 * dev_cert.c - the panel's own TLS identity for the development service: a P-256 key and a
 * self-signed certificate, generated once and kept in NVS ("devrem"). Nothing verifies the
 * public authority chain on the PC: USB enrollment pins the panel certificate. The device
 * additionally verifies the enrolled client chain and exact leaf fingerprint (DEV-002).
 *
 * The key sits in NVS like the Wi-Fi password does (flash encryption stays off on this
 * hardware, docs/features/15): it only protects the LAN link, and pairing is what grants access.
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "dev_cert.h"

static const char *TAG = "dev_cert";

#define CERT_BUF 1024 /* a P-256 self-signed certificate is ~500 bytes DER */
#define KEY_BUF  256  /* a P-256 private key is ~121 bytes DER */

static esp_err_t generate(uint8_t *cert, size_t *cert_len, uint8_t *key, size_t *key_len,
                          const char *panel_id)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
    mbedtls_svc_key_id_t key_id;
    if (psa_generate_key(&attr, &key_id) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);

    char subject[64];
    snprintf(subject, sizeof(subject), "CN=roboalarms-%s", panel_id);
    uint8_t serial[8];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7Fu; /* a DER integer stays positive */

    if (mbedtls_pk_copy_from_psa(key_id, &pk) != 0) {
        goto out;
    }
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0 ||
        /* the fingerprint is pinned, not the dates: a generous fixed range keeps a panel
         * without a synced clock working */
        mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20760101000000") != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) {
        goto out;
    }

    /* mbedTLS writes DER at the end of the buffer. */
    int len = mbedtls_x509write_crt_der(&crt, cert, CERT_BUF);
    if (len <= 0) {
        goto out;
    }
    memmove(cert, cert + CERT_BUF - len, (size_t)len);
    *cert_len = (size_t)len;

    len = mbedtls_pk_write_key_der(&pk, key, KEY_BUF);
    if (len <= 0) {
        goto out;
    }
    memmove(key, key + KEY_BUF - len, (size_t)len);
    *key_len = (size_t)len;
    err = ESP_OK;

out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    psa_destroy_key(key_id);
    return err;
}

esp_err_t dev_cert_ensure(nvs_handle_t nvs, const char *panel_id, uint8_t cert[DEV_CERT_MAX],
                         size_t *cert_len, uint8_t key[DEV_KEY_MAX], size_t *key_len)
{
    _Static_assert(DEV_CERT_MAX >= CERT_BUF, "cert buffer");
    _Static_assert(DEV_KEY_MAX >= KEY_BUF, "key buffer");
    size_t cl = DEV_CERT_MAX;
    size_t kl = DEV_KEY_MAX;
    esp_err_t ce=nvs_get_blob(nvs,"crt",cert,&cl);
    esp_err_t ke=nvs_get_blob(nvs,"key",key,&kl);
    if (ce == ESP_OK && ke == ESP_OK && cl > 0u && kl > 0u) {
        *cert_len = cl;
        *key_len = kl;
        return ESP_OK;
    }
    /* A damaged/partially readable identity is not a first enable. Never rotate
     * established trust silently after a transient storage read failure. The one exception
     * is a half-written first enable (power lost between the two writes): one half missing
     * outright and the other fine can only be that, and an identity that never had both
     * halves was never presented to anyone, so it is made again. */
    const bool half_written = (ce == ESP_OK && ke == ESP_ERR_NVS_NOT_FOUND) ||
                              (ce == ESP_ERR_NVS_NOT_FOUND && ke == ESP_OK);
    if (!half_written && (ce != ESP_ERR_NVS_NOT_FOUND || ke != ESP_ERR_NVS_NOT_FOUND)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "generating the panel's certificate (%s)",
             half_written ? "the first enable was cut short" : "first enable");
    esp_err_t err = generate(cert, cert_len, key, key_len, panel_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "certificate generation failed");
        return err;
    }
    err = nvs_set_blob(nvs, "key", key, *key_len);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "crt", cert, *cert_len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    return err;
}
