/* auto_watch - the observer (docs/features/41 DESIGN.md section 2, AUT-003, AUT-004): turns the
 * engine's events and the snapshot taken after each engine call into occurrences (auto_occ_t)
 * for the runner.
 *
 * Runs on the alarm task, inside the automation listener: it never blocks, never calls anything
 * and holds no pointer to its inputs. The caller owns the state (small: no heap, no globals).
 *
 * What it reports, in short (the full rules are DESIGN.md "How occurrences are produced"):
 * - ARMED: a partition's snapshot state entering ARMED from DISARMED or EXIT_DELAY, never from
 *   ENTRY_DELAY; once per arming at a level (a latch clears on DISARMED). The first snapshot is a
 *   baseline, so an armed state restored at power-up reports nothing (B1).
 * - DISARMED: each ALARM_EV_DISARMED event with a partition 1..ALARM_MAX_PARTITIONS.
 * - ALARM / FIRE: ALARM_EV_ALARM without ALARM_EVF_SILENT, with an allowed cause and a partition
 *   1..ALARM_MAX_PARTITIONS; once per
 *   alarm episode per partition (the latch clears on a snapshot showing the partition disarmed
 *   and its sounder off ALARM, FIRE, CO and AUX). ALARM_EV_DURESS is never one.
 * - ZONE_OPENED / ZONE_CLOSED: ALARM_ZS_FAULT edges between snapshots, for every zone; which
 *   zones may start an automation is the runner's check (auto_zone_usable). Not before the settle
 *   time (B2), re-baselined on a configuration change or a zones reset (B3), suppressed in
 *   installer mode and during a walk test (B4), dropped next to trouble, tamper or supervision
 *   flags (B5). A walk test in any partition holds every zone: the snapshot carries no zone
 *   partition.
 * - TROUBLE (BL-224): a trouble that starts - the power (AC loss), the panel's battery, a siren
 *   (the bell trouble), a sensor's supervision, low battery or loop trouble (one occurrence per
 *   zone that gets one), a keypad or device not heard (one per device), any other system trouble
 *   but the clock - cause an auto_trouble_t. Never while it lasts, never for what the first
 *   snapshot shows, nor during the settle time, installer mode or a walk test (those take a new
 *   baseline), nor at a configuration change (re-baselined).
 *
 * Each call returns how many occurrences it wrote into out (never more than max); any more that
 * were due are counted in missed (B6).
 */
#ifndef AUTOMATION_AUTO_WATCH_H
#define AUTOMATION_AUTO_WATCH_H

#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One occurrence. kind is an auto_trig_t; partition 1-based for ARMED, DISARMED, ALARM and FIRE
 * (an event without a valid partition makes none), 0 for zone occurrences: not known here, the
 * runner reads the zone's partition from the configuration; level an
 * alarm_arm_level_t (ARMED only); cause an alarm_cause_t (ALARM, FIRE), an auto_trouble_t
 * (TROUBLE); zone 1-based (zone triggers, the zone that raised an alarm, a sensor's trouble);
 * at the snapshot's taken_ms or the event's mono_ms. */
typedef struct {
    uint8_t kind;
    uint8_t partition;
    uint8_t level;
    uint8_t cause;
    uint16_t zone;
    alarm_ms_t at;
    uint8_t by_auto;  /* 1: an automation's own arm or disarm caused it (a DISARMED event whose
                         source is ALARM_SRC_AUTOMATION): it starts no arm or disarm (AUT-007) */
} auto_occ_t;

/* The observer's state. Fields are private except missed, which the caller reads (and may
 * clear) for the console. */
typedef struct {
    uint32_t missed;          /* occurrences that didn't fit in out (B6) */
    alarm_ms_t settle_at;     /* no zone occurrences before this engine time (B2) */
    uint32_t config_seq;      /* the last snapshot's configuration counter (B3) */
    uint8_t based;            /* 1 once the first snapshot was seen (B1) */
    uint8_t prev_state[ALARM_MAX_PARTITIONS];  /* alarm_arm_state_t in the last snapshot */
    uint8_t armed_level[ALARM_MAX_PARTITIONS]; /* the level ARMED fired for; 0 = none */
    uint8_t episode[ALARM_MAX_PARTITIONS];     /* ALARM and FIRE already fired this episode */
    uint8_t known[ALARM_ZONE_BITMAP_BYTES];    /* zone has a baseline */
    uint8_t fault[ALARM_ZONE_BITMAP_BYTES];    /* its FAULT bit in the last snapshot */
    uint8_t bad[ALARM_ZONE_BITMAP_BYTES];      /* trouble, tamper or supervision then (B5) */
    /* BL-224: the troubles in the last snapshot (valid while trbl_based) */
    uint8_t trbl_based;
    uint32_t trbl_sys;                         /* sys_troubles */
    uint32_t trbl_dev;                         /* dev_troubles */
    uint8_t trbl_zone[ALARM_ZONE_BITMAP_BYTES]; /* a zone with a sensor trouble */
} auto_watch_t;

void auto_watch_init(auto_watch_t *w, alarm_ms_t now, uint32_t settle_ms);
size_t auto_watch_event(auto_watch_t *w, const alarm_event_t *ev, auto_occ_t *out, size_t max);
size_t auto_watch_snapshot(auto_watch_t *w, const alarm_snapshot_t *s, auto_occ_t *out,
                           size_t max);
/* The on_zones_reset hook's bitmap (bit z-1 for zone z, ALARM_ZONE_BITMAP_BYTES long): those
 * zones take their next snapshot as a new baseline. NULL does nothing. */
void auto_watch_zones_reset(auto_watch_t *w, const uint8_t *zone_bits);

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_AUTO_WATCH_H */
