/* ha_link_internal.h - between ha_link.c and ha_cert.c only. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

#define HA_CERT_MAX 1024
#define HA_KEY_MAX  256

/* Loads the panel's certificate and key (DER) from NVS, generating them on first use. */
esp_err_t ha_cert_ensure(nvs_handle_t nvs, const char *panel_id, uint8_t cert[HA_CERT_MAX],
                         size_t *cert_len, uint8_t key[HA_KEY_MAX], size_t *key_len);
