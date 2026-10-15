/*
 * panel_ota_internal.h - shared between the files of panel_ota (not a public API).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "panel_ota.h"

/* proto_fw_install_allowed(), asked of the alarm task itself so the answer and the engine's
 * state belong together (OTA-004). Blocks for one round trip to the alarm task, so call it
 * from the update or console task, never from a listener. */
bool panel_ota_system_idle(const char **why);

/* Copies a fixed-size, maybe unterminated version / name field. */
void panel_ota_version_copy(char *out, size_t cap, const char *field, size_t field_len);

/* The Wi-Fi module's image is sent and activated (cp_ota.c): the run waits (stage WIFI_MODULE,
 * phase WAITING) and keeps the update machinery until the update task restarts the panel, once
 * the system is idle, through the same announcement and gates as a release (OTA-004, OTA-028).
 * Update task only. */
void panel_ota_cp_sent(const alarm_source_t *src, uint8_t user);

void panel_ota_set_progress(panel_ota_phase_t phase, int percent, const char *error);
void panel_ota_set_cp_image(const char *version);
/* A co-processor update was claimed: its run (stage WIFI_MODULE, OTA-026) lasts until
 * panel_ota_release() or the restart. */
void panel_ota_cp_started(const char *target, const char *previous);

/* ---- co-processor (cp_ota.c) ---- */
void panel_ota_cp_init(void);       /* at start: looks at the "cp_fw" partition */
void panel_ota_cp_tick(void);       /* update task, once a second: automatic update (OTA-007) */
void panel_ota_cp_run(void);        /* update task: a requested co-processor update */
void panel_ota_task_notify_cp(void); /* wake the update task for panel_ota_cp_run() */

/* ---- the trusted keys (ota_trust.c, OTA-020) ---- */
/* Replaces the RAM copy of the accepted key (NULL = none). Only from the start-up task or the
 * update task while no update runs and nothing holds the update machinery. */
void panel_ota_trust_set(const uint8_t *key_sha256);
/* The digest of the running image's signing key (block 0), as ESP-IDF's check trusts it. Reads
 * flash: call from a task with an internal-RAM stack. */
bool panel_ota_trust_own_key(uint8_t out[32]);
