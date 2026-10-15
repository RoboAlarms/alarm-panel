/*
 * alarm_store.c - NVS persistence for alarm configuration and runtime state.
 */
#include "alarm_store.h"

#include <string.h>

#include "alarm/alarm_codec.h"
#include "code_hash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "alarm_store";
static const char *NS = "alarm";

static void set_name(char *dst, const char *src)
{
    strlcpy(dst, src, ALARM_NAME_LEN);
}

#if CONFIG_ALARM_DEMO_ZONES
static void demo_zone(alarm_config_t *cfg, uint16_t z, alarm_zone_type_t type,
                      alarm_device_class_t cls, const char *name)
{
    alarm_zone_config_defaults(&cfg->zones[z - 1u], z, type, 1);
    cfg->zones[z - 1u].device_class = (uint8_t)cls;
    set_name(cfg->zones[z - 1u].name, name);
}
#endif

esp_err_t alarm_store_default_config(alarm_config_t *cfg)
{
    /* Creating credentials needs crypto that is known to work: a code hashed by a broken PSA
     * could never be entered again, and must never be written to flash. */
    const esp_err_t crypto = code_hash_init();
    if (crypto != ESP_OK) {
        ESP_LOGE(TAG, "code hashing unavailable (%s): refusing to create the factory codes",
                 esp_err_to_name(crypto));
        return crypto;
    }
    const uint32_t faults = code_hash_faults();

    alarm_config_defaults(cfg);
    esp_fill_random(cfg->code_salt, sizeof(cfg->code_salt));
    alarm_config_set_default_users(cfg, code_hash_pbkdf2, NULL);
    if (code_hash_faults() != faults) {
        ESP_LOGE(TAG, "code hashing failed while creating the factory codes");
        return ESP_ERR_INVALID_STATE;
    }
    set_name(cfg->partitions[0].name, "Home");

#if CONFIG_ALARM_DEMO_ZONES
    /* Simulator zones for bring-up; drive them from the console ("zone 1 open"). */
    demo_zone(cfg, 1, ALARM_ZT_ENTRY_EXIT_1, ALARM_DEVCLASS_DOOR, "Front Door");
    demo_zone(cfg, 2, ALARM_ZT_ENTRY_EXIT_2, ALARM_DEVCLASS_GARAGE_DOOR, "Garage Door");
    demo_zone(cfg, 3, ALARM_ZT_PERIMETER, ALARM_DEVCLASS_WINDOW, "Living Room Window");
    demo_zone(cfg, 4, ALARM_ZT_PERIMETER, ALARM_DEVCLASS_DOOR, "Back Door");
    demo_zone(cfg, 5, ALARM_ZT_INTERIOR_FOLLOWER, ALARM_DEVCLASS_MOTION, "Hallway Motion");
    demo_zone(cfg, 6, ALARM_ZT_FIRE, ALARM_DEVCLASS_SMOKE, "Kitchen Smoke");
    demo_zone(cfg, 7, ALARM_ZT_CO, ALARM_DEVCLASS_CO, "Hallway CO");
    demo_zone(cfg, 8, ALARM_ZT_24H_AUX, ALARM_DEVCLASS_WATER, "Basement Water");
#endif
    return ESP_OK;
}

/* Reads up to *len bytes; *len receives the stored size. */
static esp_err_t load_blob_len(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err;
}

static esp_err_t load_blob(const char *key, void *buf, size_t size)
{
    size_t len = size;
    esp_err_t err = load_blob_len(key, buf, &len);
    if (err == ESP_OK && len != size) {
        err = ESP_ERR_INVALID_SIZE; /* layout changed: treat as absent */
    }
    return err;
}

static esp_err_t save_blob(const char *key, const void *buf, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, buf, size);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t alarm_store_load_config(alarm_config_t *cfg, alarm_cfg_origin_t *origin)
{
    *origin = ALARM_CFG_UNAVAILABLE; /* nothing is written to *cfg until it is known good */

    uint8_t *blob = heap_caps_malloc(ALARM_CONFIG_BLOB_MAX, MALLOC_CAP_DEFAULT);
    if (blob == NULL) {
        ESP_LOGE(TAG, "no memory (%u bytes) to read the configuration: stored copy kept",
                 (unsigned)ALARM_CONFIG_BLOB_MAX);
        return ESP_ERR_NO_MEM;
    }
    size_t len = ALARM_CONFIG_BLOB_MAX;
    const esp_err_t err = load_blob_len("config", blob, &len);
    const bool decoded = err == ESP_OK && alarm_config_decode(blob, len, cfg) == ALARM_OK;
    heap_caps_free(blob);
    if (decoded) {
        *origin = ALARM_CFG_STORED;
        return ESP_OK;
    }

    /* Only two outcomes justify running on anything other than the stored configuration, and
     * both are positively identified: the namespace/key is absent (first boot), or the blob
     * was read but this firmware cannot make sense of it. Anything else - no memory, NVS not
     * mounted, a read error - is a failure to find out what is stored, and must never
     * authorise replacing it. */
    const bool absent = err == ESP_ERR_NVS_NOT_FOUND;
    const bool unreadable = err == ESP_OK /* decode rejected it */ ||
                            err == ESP_ERR_NVS_INVALID_LENGTH /* longer than this build reads */;
    if (!absent && !unreadable) {
        ESP_LOGE(TAG, "could not read the stored configuration (%s): keeping it, not starting "
                      "on defaults",
                 esp_err_to_name(err));
        return err;
    }

    const esp_err_t derr = alarm_store_default_config(cfg);
    if (derr != ESP_OK) {
        return derr; /* origin stays ALARM_CFG_UNAVAILABLE: *cfg is not usable */
    }
    if (absent) {
        *origin = ALARM_CFG_CREATED;
        return alarm_store_save_config(cfg);
    }
    ESP_LOGE(TAG, "stored configuration unusable (%s): running on factory defaults, "
                  "stored copy kept",
             err == ESP_OK ? "damaged or from a newer firmware" : esp_err_to_name(err));
    *origin = ALARM_CFG_DEFAULTED;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t alarm_store_save_config(const alarm_config_t *cfg)
{
    uint8_t *blob = heap_caps_malloc(ALARM_CONFIG_BLOB_MAX, MALLOC_CAP_DEFAULT);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    esp_err_t err = (alarm_config_encode(cfg, blob, ALARM_CONFIG_BLOB_MAX, &len) == ALARM_OK)
                        ? save_blob("config", blob, len)
                        : ESP_ERR_INVALID_SIZE;
    heap_caps_free(blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving configuration failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* A failed read is reported like "nothing stored": the caller then starts disarmed, which is
 * the safe direction (a stale armed state must never come back from a read error). */
bool alarm_store_load_runtime(alarm_runtime_state_t *rt)
{
    const esp_err_t err = load_blob("rt", rt, sizeof(*rt));
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "reading the runtime state failed (%s): starting disarmed",
                     esp_err_to_name(err));
        }
        return false;
    }
    return rt->version == ALARM_RUNTIME_VERSION;
}

esp_err_t alarm_store_save_runtime(const alarm_runtime_state_t *rt)
{
    return save_blob("rt", rt, sizeof(*rt));
}
