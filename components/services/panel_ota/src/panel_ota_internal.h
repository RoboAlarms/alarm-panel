/*
 * panel_ota_internal.h - shared between the files of panel_ota (not a public API).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "esp_partition.h"
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

/* ---- updates through the panel link (docs/features/11) ---- */
/* The words the cap refuses with ("<master> runs 0.6.2: update it first"), "" without a cap:
 * the console's. Any task. */
void panel_ota_cap_words(char *out, size_t len);

/* ---- a master's image sources (ota_source.c, docs/features/11 "The master") ---- */
/* This panel's own run starts (Install, a developer's file): a copy for a keypad is of the
 * version that runs now, so it goes at once - freed, a copy asked for forgotten and a copy's
 * download under way ended - before the run's own download needs the memory. Any task; call it
 * after the run was accepted under panel_ota's lock, with that lock no longer held (it takes the
 * sources' lock, which panel_ota_fetch_for holds while it asks panel_ota_run_busy()). */
void panel_ota_source_own_run(void);
/* The update task, every tick: the running image once confirmed, a copy asked for, freeing an
 * unused copy (30 min) or window (30 s). asked: NB_SOURCE woke it. */
void panel_ota_source_tick(uint64_t now, bool asked);
/* Wakes the update task for panel_ota_source_tick(). */
void panel_ota_notify_source(void);
/* The running image is confirmed (not on probation). */
bool panel_ota_confirmed(void);
/* An update run of this panel's own, its self-test, or not started (OTA-030). */
bool panel_ota_run_busy(void);
/* Its own release, downloaded and checked, waits for the restart in its spare slot (not on its
 * self-test): its keypads' copies of that version may be fetched meanwhile. Any task. */
bool panel_ota_own_waiting(void);
/* len bytes at offset of partition part through the sources' windows (panel_ota_spare_read):
 * from a task with an internal-RAM stack. */
esp_err_t panel_ota_source_part_read(const esp_partition_t *part, uint32_t offset, void *out,
                                     size_t len);
/* A check at this panel's own server and channel with a keypad's facts and want; *a gets the
 * answer. Returns its why (proto_fwsrv_check_why's, or the reach and memory classes). The update
 * task only. */
uint8_t panel_ota_check_other(const proto_fwsrv_facts_t *facts, const char *want,
                              proto_fwsrv_answer_t *a);
/* panel_ota_download with a flag that ends it (stop, may be NULL) and its why (code: the panel
 * link's numbers). The update task only. */
esp_err_t panel_ota_download_stoppable(const char *url, uint32_t size, const uint8_t *sha256,
                                       uint8_t *out, size_t cap, size_t *got,
                                       const volatile bool *stop, uint8_t *code);

/* ---- the trusted keys (ota_trust.c, OTA-020) ---- */
/* Replaces the RAM copy of the accepted key (NULL = none). Only from the start-up task or the
 * update task while no update runs and nothing holds the update machinery. */
void panel_ota_trust_set(const uint8_t *key_sha256);
/* The digest of the running image's signing key (block 0), as ESP-IDF's check trusts it. Reads
 * flash: call from a task with an internal-RAM stack. */
bool panel_ota_trust_own_key(uint8_t out[32]);
