/*
 * plink_view.h - the panel link's status on both sides (docs/features/11 "STATUS", "Roles";
 * LINK-008, LINK-019, LINK-026): the master builds a keypad's STATUS from its snapshot, the
 * keypad rebuilds an alarm_snapshot_t on its own clock; the keypads' copy of the configuration
 * and its lenient decode.
 */
#ifndef PLINK_VIEW_H
#define PLINK_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the master ---------------------------------------------------------------------------- */

/* What a STATUS carries besides the snapshot. */
typedef struct {
    int64_t epoch_ms;         /* the master's wall clock now, 0 = not set */
    uint8_t home;             /* the keypad's home partition */
    uint32_t lockout_left_ms; /* that keypad's own lockout left now (alarm_engine_lockout_left) */
    uint8_t report;           /* PLINK_REPORT_* */
    uint32_t cfg_crc;
    uint32_t cfg_len;
    uint16_t zones_cap;       /* the keypad's zone slots (its HELLO's capacities); 0 = not known */
} plink_status_extra_t;

/* A keypad's STATUS from the master's snapshot at engine time now (the clock of
 * snap->taken_ms): every duration is worked out at now, never below 0 (docs/features/11
 * "STATUS"); walk_id is the raw walk start (0 none; a start whose low 32 bits are 0 reads 1);
 * zones_crc covers zones 1..min(zones_used, x->zones_cap) - the zones that keypad can hold, all
 * of them when zones_cap is 0 (docs/features/11 "Zone record"); one record per enabled
 * partition. */
void plink_status_build(const alarm_snapshot_t *snap, uint64_t now_ms,
                        const plink_status_extra_t *x, plink_status_t *out);

/* ---- the keypad ---------------------------------------------------------------------------- */

/* The keypad's picture of the master, rebuilt on the keypad's own clock. The keypad manager owns
 * one; seq and config_seq are its own counters (the UI requires snapshot_seq() == snap.seq). */
typedef struct {
    alarm_snapshot_t snap;
    bool have_status;
    uint8_t home;
    int64_t epoch_ms;     /* the master's clock at the last status */
    uint64_t epoch_at_ms; /* local time it arrived */
    uint8_t report;
    uint32_t cfg_crc;     /* what the master has */
    uint32_t cfg_len;
    uint32_t zones_crc;   /* what the master has */
    uint32_t walk_id[ALARM_MAX_PARTITIONS]; /* the master's walk start last mapped ... */
    alarm_ms_t walk_local[ALARM_MAX_PARTITIONS]; /* ... to this local start (0 none) */
} plink_view_t;

void plink_view_init(plink_view_t *v);

/* Applies a STATUS that arrived at local time now: taken_ms = now; the delay, walk and recovery
 * left as sent; the keypad's own lockout as local_lockout_until = now + left (0 when not locked);
 * each partition's walk start mapped once per distinct walk_id to now - walk_age_ms (at least 1);
 * partitions not in the mask zeroed; zones beyond zones_used zeroed. Records for partitions this
 * build doesn't have, or not in the mask, are ignored; a partition in the mask whose record this
 * frame doesn't carry keeps its last one, counted down to now. Within one countdown (the same
 * delay state, a walk test still running, a lockout not yet over) a frame that reads less than a
 * second longer than what is showing - the network's delay varied - keeps the count, so what the
 * keypad shows never goes up (UI-023). Does not touch seq or config_seq. */
void plink_view_apply_status(plink_view_t *v, const plink_status_t *st, uint64_t now_ms);

/* Applies the records of a ZONES message (zones beyond ALARM_MAX_ZONES ignored). */
void plink_view_apply_zones(plink_view_t *v, const plink_zones_t *z);

/* The CRC of the keypad's zones 1..zones_used (as far as this build holds them), to compare with
 * the master's zones_crc. */
uint32_t plink_view_zones_crc(const plink_view_t *v);

/* ---- the configuration copy (LINK-019) ------------------------------------------------------ */

/* The master's copy for keypads: cfg copied into scratch, every used user's code hash replaced by
 * a filler unique to its slot (no byte of a real hash), the salt zeroed, duress users made
 * standard; then encoded (alarm_config_encode) into out. *crc is the blob's CRC-32 (cfg_crc).
 * ALARM_ERR_BAD_ARG when out is too small. */
alarm_result_t plink_cfg_for_keypads(const alarm_config_t *cfg, alarm_config_t *scratch,
                                     uint8_t *out, size_t cap, size_t *len, uint32_t *crc);

/* The keypad's decode of that blob: lenient (alarm_config_decode_lenient): no validation, an
 * index beyond this build's capacities skipped. */
alarm_result_t plink_cfg_decode(const uint8_t *blob, size_t len, alarm_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_VIEW_H */
