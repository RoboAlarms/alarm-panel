/*
 * backup_check.h - a decrypted backup checked in full before anything is written
 * (docs/features/14 CFG-006): every record well formed, made on this kind of panel, only
 * entries the policy keeps (never another panel's identity), the configuration decoding and
 * validating as the engine would take it, the zone bindings decoding. A restore writes nothing
 * unless this says BACKUP_OK.
 */
#ifndef BACKUP_CHECK_H
#define BACKUP_CHECK_H

#include "alarm/alarm_config.h"
#include "backup/backup.h"
#include "zone_hub/zone_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Room for decoding the blobs (large: the caller puts it where it has space). */
typedef struct {
    alarm_config_t cfg;
    zone_hub_t hub;
} backup_scratch_t;

typedef struct {
    backup_meta_t meta;
    uint32_t entries;
    uint16_t zones;      /* zones in use in the file's configuration */
    uint16_t users;      /* user slots in use */
    bool bindings;       /* where the zones' sensors are (zones/bind) */
    bool network;        /* the Wi-Fi network (net/ssid) */
    bool zwave;          /* the Z-Wave network keys (zwave/keys) */
    char bad_ns[BACKUP_NAME_MAX + 1]; /* BACKUP_ERR_CONTENT: the entry refused */
    char bad_key[BACKUP_NAME_MAX + 1];
} backup_summary_t;

/* board: the running panel's kind (ALARM_BOARD), NULL to accept any. */
BACKUP_MUST_CHECK
backup_result_t backup_check(const uint8_t *payload, size_t len, const char *board,
                             backup_scratch_t *scratch, backup_summary_t *summary);

/* The configuration a restore stores in place of the file's alarm/config (docs/features/08
 * voice-first-build.md D07, CORE-CFG-021): the file's, decoded into scratch->cfg, with
 * alarm_config_voice_restore(current, ...) applied - every spoken-name approval cleared, fresh
 * binding revisions above everything 'current' (the running configuration, its writes stopped)
 * and the file ever handed out - and encoded into out (cap bytes; ALARM_CONFIG_BLOB_MAX is
 * enough). payload is one backup_check() accepted. BACKUP_ERR_ARG: a NULL argument or out too
 * small; BACKUP_ERR_CONTENT: no alarm/config blob, or one that doesn't decode. Nothing is to be
 * written unless this says BACKUP_OK. */
BACKUP_MUST_CHECK
backup_result_t backup_restore_config(const uint8_t *payload, size_t len,
                                      const alarm_config_t *current, backup_scratch_t *scratch,
                                      uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* BACKUP_CHECK_H */
