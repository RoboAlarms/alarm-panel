/*
 * alarm_store.h - NVS persistence for the alarm configuration and runtime state.
 * Namespace "alarm": "config" = the versioned configuration blob (alarm/alarm_codec.h),
 * "rt" = alarm_runtime_state_t (fixed layout, ALARM_RUNTIME_VERSION).
 *
 * Nothing here ever replaces stored data to recover from a failure it could not diagnose:
 * only a namespace that is positively absent leads to a write of the factory configuration.
 */
#pragma once

#include <stdbool.h>

#include "alarm/alarm_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the configuration in *cfg came from. Anything but ALARM_CFG_UNAVAILABLE means *cfg is
 * filled in and usable. */
typedef enum {
    ALARM_CFG_STORED = 0,  /* read back from NVS */
    ALARM_CFG_CREATED,     /* nothing was stored: factory configuration, written to NVS */
    ALARM_CFG_DEFAULTED,   /* stored data unreadable: factory configuration, stored copy kept */
    ALARM_CFG_UNAVAILABLE, /* could not find out what is stored: *cfg must not be used */
} alarm_cfg_origin_t;

/* Factory configuration: CP-01 defaults, random salt, installer 69420 / master 123456,
 * partition "Home" and (with CONFIG_ALARM_DEMO_ZONES) a few simulator zones.
 * Needs working code hashing (code_hash_init): ESP_ERR_INVALID_STATE otherwise, and then *cfg
 * must not be used - a user whose hash came out of broken crypto could never be used again. */
esp_err_t alarm_store_default_config(alarm_config_t *cfg);

/* Loads the stored configuration and says where the result came from (*origin, never NULL).
 *   ESP_OK                 - *cfg is the stored configuration, or the factory one just written
 *                            on a first boot (*origin = ALARM_CFG_CREATED).
 *   ESP_ERR_INVALID_STATE  - something is stored but this firmware cannot read it (damaged, or
 *                            written by a newer firmware before a rollback): *cfg is the
 *                            factory configuration and the stored copy is left untouched.
 *   anything else          - the load itself failed (no memory, NVS unusable). *cfg is NOT
 *                            filled in, *origin is ALARM_CFG_UNAVAILABLE and nothing was
 *                            written: the caller must retry or refuse to start, never reset. */
esp_err_t alarm_store_load_config(alarm_config_t *cfg, alarm_cfg_origin_t *origin);
esp_err_t alarm_store_save_config(const alarm_config_t *cfg);

/* Returns false when nothing (valid) is stored, and when the read failed: the caller starts
 * disarmed, which is the safe direction. */
bool alarm_store_load_runtime(alarm_runtime_state_t *rt);
esp_err_t alarm_store_save_runtime(const alarm_runtime_state_t *rt);

#ifdef __cplusplus
}
#endif
