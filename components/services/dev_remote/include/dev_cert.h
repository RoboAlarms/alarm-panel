#pragma once
#include "nvs.h"
#define DEV_CERT_MAX 1024
#define DEV_KEY_MAX 256
esp_err_t dev_cert_ensure(nvs_handle_t nvs, const char *panel_id, uint8_t cert[DEV_CERT_MAX],
                         size_t *cert_len, uint8_t key[DEV_KEY_MAX], size_t *key_len);
