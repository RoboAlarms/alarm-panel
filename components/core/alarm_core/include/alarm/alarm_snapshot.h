/*
 * alarm_snapshot.h - everything a keypad shows, in one copyable struct.
 *
 * The platform takes a snapshot after each engine call and hands copies to the UI (and later
 * to remote keypads), so readers never touch the engine itself.
 */
#ifndef ALARM_SNAPSHOT_H
#define ALARM_SNAPSHOT_H

#include "alarm/alarm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t seq;         /* owner's change counter (left unchanged by alarm_engine_snapshot) */
    uint32_t config_seq;  /* owner's configuration counter (left unchanged) */
    alarm_ms_t taken_ms;  /* engine time; delay_remaining_ms counts down from here */
    uint8_t partitions;   /* enabled partitions, bit 0 = partition 1 */
    alarm_partition_status_t part[ALARM_MAX_PARTITIONS]; /* zeroed when disabled */
    uint32_t sys_troubles;          /* active system troubles (bit = alarm_sys_trouble_t) */
    uint32_t sys_troubles_restored; /* restored, waiting for code + OFF */
    uint32_t dev_troubles;          /* missing devices (bit = device - 1) */
    uint8_t default_codes;          /* ALARM_DEFAULT_CODE_* still in use */
    bool program;                   /* installer mode: nothing can be armed */
    uint32_t recovery_left_ms;      /* power-up recovery window left at taken_ms, 0 = closed */
    alarm_ms_t local_lockout_until; /* the touchscreen's lockout ends at this engine time, 0 = not
                                     * locked (KPD-008); the UI counts down from taken_ms */
    uint16_t zones_used;            /* highest configured zone number, 0 = none */
    alarm_zone_status_t zone[ALARM_MAX_ZONES]; /* zeroed for unused zones */
    uint32_t last_event_seq;        /* sequence number of the newest event */
    bool exit_sounds;               /* the configuration's exit_sounds option now: the voice decides
                                     * an arming announcement on the alarm task's value, never a
                                     * cached copy (docs/features/08 D05) */
} alarm_snapshot_t;

/* Fill everything except seq and config_seq from the engine's current state. */
void alarm_engine_snapshot(const alarm_engine_t *e, alarm_snapshot_t *out);

/* Is any enabled partition armed (from its exit time on)? */
bool alarm_snapshot_any_armed(const alarm_snapshot_t *s);
/* Locked down for settings (docs/features/14, UI-022): a partition armed from its exit time on, or
 * an alarm, fire, CO or auxiliary sounder going. Settings, chime, codes and every other change
 * wait until everything is disarmed and quiet - on the touchscreen and in the Panel Manager. */
bool alarm_snapshot_locked(const alarm_snapshot_t *s);

/* One line of the Troubles list (UI-030, BL-137): the touchscreen and the Panel Manager walk the
 * same list. family is ALARM_TRBL_FAMILY_*; kind is an alarm_sys_trouble_t (system), an
 * alarm_zone_trouble_t (zone) or ALARM_TRBL_DEVICE_SUPERVISION (keypad); number is 0 for the
 * system, else the keypad's or the zone's 1-based number. */
typedef struct {
    uint8_t family;
    uint8_t kind;
    uint16_t number;
    bool restored; /* cleared, kept until a code + OFF */
} alarm_trouble_item_t;

/* Walks every trouble the snapshot holds, in an order that doesn't move about: the system's, the
 * keypads' (active only), then the zones' by number; the active ones first, then the restored.
 * partitions filters the zones (bit p-1 = partition p; 0xFF = all of them) by their partition in
 * cfg; the system's and the keypads' always count. Returns how many there are altogether and
 * fills the first max of them into out; out may be NULL with max 0 to only count. */
size_t alarm_snapshot_troubles(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                               uint8_t partitions, alarm_trouble_item_t *out, size_t max);

/* The same walk, filling lines [first, first + max) into out: a caller with a small buffer reads
 * the list a piece at a time (the touchscreen's Troubles list, BL-140). Returns how many there
 * are altogether. */
size_t alarm_snapshot_troubles_from(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                                    uint8_t partitions, size_t first, alarm_trouble_item_t *out,
                                    size_t max);

/* A fingerprint of that same list (FNV-1a over each line's family, kind, number and restored, in
 * order): it changes when a line comes, goes or is restored, so a viewer knows its copy is stale
 * even when the count stays the same. */
uint32_t alarm_snapshot_troubles_sig(const alarm_snapshot_t *s, const alarm_config_t *cfg,
                                     uint8_t partitions);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_SNAPSHOT_H */
