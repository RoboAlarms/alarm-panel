/*
 * engine_internal.h - private helpers shared by the engine source files.
 */
#ifndef ALARM_ENGINE_INTERNAL_H
#define ALARM_ENGINE_INTERNAL_H

#include "alarm/alarm_engine.h"

/* Zone input conditions (alarm_zone_rt_t.cond). */
#define ZC_FAULT 0x01u

/* Zone runtime flags (alarm_zone_rt_t.flags). */
#define ZRT_IN_ALARM      0x0001u /* alarmed, loop has not restored yet */
#define ZRT_MEMORY        0x0002u /* shown in alarm memory */
#define ZRT_SILENT_MEM    0x0004u /* memory came from a silent alarm: hide while armed */
#define ZRT_BYPASSED      0x0008u
#define ZRT_SWINGER       0x0010u /* shut down for the rest of the armed period */
#define ZRT_REPORTED      0x0020u /* alarm report went out: report the restore too */
#define ZRT_CROSS_PENDING 0x0040u /* first of a cross-zone pair tripped */
#define ZRT_UNREACHABLE   0x0080u /* the source can't reach the sensor: CHECK until REACHABLE */
/* An automation bypassed it at arming (docs/features/41 AUT-014): watched again once closed for
 * REWATCH_MS (CORE-USR-033). Runtime only: never in alarm_runtime_state_t, so after a restart
 * the zone stays bypassed until the disarm, as any bypass. */
#define ZRT_AUTO_BYPASS   0x0100u
#define REWATCH_MS        10000u

/* Fire verification states (alarm_zone_rt_t.verify). */
#define FV_IDLE  0u
#define FV_RESET 1u /* smoke power interrupted after the first trip */
#define FV_WAIT  2u /* waiting for a confirming trip */

/* How an event is reported remotely. */
typedef enum {
    REPORT_NONE = 0, /* log only */
    REPORT_NOW,
    REPORT_DEFERRED, /* after the partition's abort window */
} report_mode_t;

#define MS(s) ((alarm_ms_t)(s) * 1000u)
#define CAUSE_BIT(c) ((uint16_t)(1u << (unsigned)(c)))

/* Causes that never sound or show locally. */
#define SILENT_CAUSES \
    (CAUSE_BIT(ALARM_CAUSE_SILENT_BURGLARY) | CAUSE_BIT(ALARM_CAUSE_24H_SILENT) | \
     CAUSE_BIT(ALARM_CAUSE_PANIC_SILENT))
/* Silent causes that are never shown in alarm memory either. */
#define HIDDEN_CAUSES (CAUSE_BIT(ALARM_CAUSE_24H_SILENT) | CAUSE_BIT(ALARM_CAUSE_PANIC_SILENT))
#define FIRE_CAUSES   (CAUSE_BIT(ALARM_CAUSE_FIRE) | CAUSE_BIT(ALARM_CAUSE_PANIC_FIRE))
#define CO_CAUSES     (CAUSE_BIT(ALARM_CAUSE_CO))
#define AUX_CAUSES    (CAUSE_BIT(ALARM_CAUSE_24H_AUX) | CAUSE_BIT(ALARM_CAUSE_PANIC_MEDICAL))
#define LOUD_CAUSES \
    (CAUSE_BIT(ALARM_CAUSE_BURGLARY) | CAUSE_BIT(ALARM_CAUSE_EXIT_ERROR) | \
     CAUSE_BIT(ALARM_CAUSE_TAMPER) | CAUSE_BIT(ALARM_CAUSE_24H_AUDIBLE) | \
     CAUSE_BIT(ALARM_CAUSE_PANIC_POLICE))
/* Causes whose remote report waits for the abort window. */
#define ABORTABLE_CAUSES \
    (CAUSE_BIT(ALARM_CAUSE_BURGLARY) | CAUSE_BIT(ALARM_CAUSE_SILENT_BURGLARY) | \
     CAUSE_BIT(ALARM_CAUSE_EXIT_ERROR) | CAUSE_BIT(ALARM_CAUSE_TAMPER))

/* ---- small accessors ---------------------------------------------------- */
static inline alarm_partition_rt_t *eng_part(alarm_engine_t *e, uint8_t p)
{
    return &e->parts[p - 1u];
}
static inline const alarm_zone_config_t *eng_zcfg(const alarm_engine_t *e, uint16_t z)
{
    return &e->cfg.zones[z - 1u];
}
static inline alarm_zone_rt_t *eng_zrt(alarm_engine_t *e, uint16_t z)
{
    return &e->zones[z - 1u];
}
static inline bool eng_partition_valid(const alarm_engine_t *e, uint8_t p)
{
    return p >= 1u && p <= ALARM_MAX_PARTITIONS && e->cfg.partitions[p - 1u].enabled;
}
static inline bool eng_zone_valid(const alarm_engine_t *e, uint16_t z)
{
    return z >= 1u && z <= ALARM_MAX_ZONES && e->cfg.zones[z - 1u].type != ALARM_ZT_NOT_USED;
}
static inline bool bm_get(const uint8_t *bm, uint16_t z)
{
    return (bm[(z - 1u) / 8u] & (uint8_t)(1u << ((z - 1u) % 8u))) != 0u;
}
static inline void bm_set(uint8_t *bm, uint16_t z)
{
    bm[(z - 1u) / 8u] = (uint8_t)(bm[(z - 1u) / 8u] | (1u << ((z - 1u) % 8u)));
}
static inline alarm_ms_t ms_add(alarm_ms_t t, alarm_ms_t d)
{
    return (t > ALARM_TIME_NEVER - d) ? ALARM_TIME_NEVER : t + d;
}

/* ---- events.c ----------------------------------------------------------- */
void eng_emit(alarm_engine_t *e, alarm_event_type_t type, uint8_t partition, uint16_t zone,
              uint8_t user, const alarm_source_t *src, uint8_t arg, uint8_t arg2,
              uint8_t flags, report_mode_t mode);
bool eng_zone_reportable(const alarm_engine_t *e, uint16_t zone);
void eng_reports_due(alarm_engine_t *e);
alarm_ms_t eng_reports_next(const alarm_engine_t *e);
/* Drop reports held back for a partition; returns number of dropped alarms. */
unsigned eng_reports_drop(alarm_engine_t *e, uint8_t partition);

/* ---- alarms.c ----------------------------------------------------------- */
void eng_alarm(alarm_engine_t *e, uint8_t p, uint16_t zone, alarm_cause_t cause,
               const alarm_source_t *src, uint8_t user);
void eng_start_sounding(alarm_engine_t *e, uint8_t p, alarm_cause_t cause);
void eng_silence(alarm_engine_t *e, uint8_t p, uint16_t causes);
alarm_sound_t eng_sounder(const alarm_engine_t *e, uint8_t p);
alarm_pattern_t eng_bell_pattern(const alarm_partition_rt_t *pr);
void eng_sounding_timers(alarm_engine_t *e, uint8_t p);

/* ---- zones.c ------------------------------------------------------------ */
void eng_zone_input(alarm_engine_t *e, uint16_t z, alarm_zone_event_t ev);
void eng_zone_fault(alarm_engine_t *e, uint16_t z, bool quiet);
void eng_zone_set_trouble(alarm_engine_t *e, uint16_t z, alarm_zone_trouble_t kind, bool on);
bool eng_zone_blocks_level(const alarm_engine_t *e, uint16_t z, alarm_arm_level_t level);
bool eng_zone_not_ready(const alarm_engine_t *e, uint16_t z);
/* Not ready only because its loop is open: no tamper, loop or supervision trouble (AUT-014). */
bool eng_zone_only_open(const alarm_engine_t *e, uint16_t z);
void eng_smoke_reset_start(alarm_engine_t *e);
void eng_smoke_reset_end(alarm_engine_t *e);
void eng_fire_verify_due(alarm_engine_t *e);
alarm_ms_t eng_fire_verify_next(const alarm_engine_t *e);
void eng_supervision_due(alarm_engine_t *e);
alarm_ms_t eng_supervision_next(const alarm_engine_t *e);
void eng_settle_end(alarm_engine_t *e);

/* ---- arming.c ----------------------------------------------------------- */
alarm_result_t eng_arm(alarm_engine_t *e, uint8_t p, alarm_arm_level_t level, uint8_t flags,
                       uint8_t user, const alarm_source_t *src, alarm_cmd_result_t *res);
alarm_result_t eng_disarm(alarm_engine_t *e, uint8_t p, uint8_t user, const alarm_source_t *src);
void eng_exit_restart(alarm_engine_t *e, uint8_t p, bool automatic, uint8_t user,
                      const alarm_source_t *src);
void eng_start_entry(alarm_engine_t *e, uint8_t p, uint16_t z);
void eng_partition_timers(alarm_engine_t *e, uint8_t p);
void eng_button_zone(alarm_engine_t *e, uint16_t z);
alarm_ms_t eng_partition_next(const alarm_engine_t *e, uint8_t p);
/* CORE-USR-033: an automation's bypasses watched again. sync (re)starts or stops each zone's
 * 10 s wait from its state now; due ends the waits that are over; next is the earliest. */
void eng_rewatch_sync(alarm_engine_t *e);
void eng_rewatch_due(alarm_engine_t *e);
alarm_ms_t eng_rewatch_next(const alarm_engine_t *e);

/* ---- walk.c ------------------------------------------------------------- */
bool eng_walk_covers(const alarm_engine_t *e, uint16_t z, uint8_t walk_flags);
void eng_walk_start(alarm_engine_t *e, uint8_t p, uint8_t flags, uint8_t user,
                    const alarm_source_t *src);
void eng_walk_extend(alarm_engine_t *e, uint8_t p, uint8_t user, const alarm_source_t *src);
void eng_walk_end(alarm_engine_t *e, uint8_t p, uint8_t user, const alarm_source_t *src,
                  alarm_walk_end_t how);
/* Mark a zone of a partition in walk test: beeps on every trip, logs the first of each kind. */
void eng_walk_mark(alarm_engine_t *e, uint16_t z, alarm_walk_mark_t how);
/* The zone's type or partition changes: its mark goes (WALK-006, as CORE-CFG-018). */
void eng_walk_forget(alarm_engine_t *e, uint16_t z);
void eng_walk_timers(alarm_engine_t *e, uint8_t p);
alarm_ms_t eng_walk_next(const alarm_engine_t *e, uint8_t p);

/* ---- users.c ------------------------------------------------------------ */
alarm_result_t eng_auth(alarm_engine_t *e, const alarm_source_t *src, const char *code,
                        uint8_t partition, uint8_t *user_out);
bool eng_code_format_ok(const char *code);
void eng_update_default_codes(alarm_engine_t *e);
alarm_authority_t eng_user_auth(const alarm_engine_t *e, uint8_t user);
/* May this user operate this partition at all? (masters/installer: every partition) */
bool eng_user_in_partition(const alarm_engine_t *e, uint8_t user, uint8_t partition);

/* ---- program.c ---------------------------------------------------------- */
bool eng_all_idle(const alarm_engine_t *e); /* every partition disarmed, silent, no walk test */
void eng_program_end(alarm_engine_t *e, const alarm_source_t *src, uint8_t why);
alarm_ms_t eng_program_next(const alarm_engine_t *e);
void eng_program_due(alarm_engine_t *e);

/* ---- engine.c ----------------------------------------------------------- */
void eng_mark_all_dirty(alarm_engine_t *e);
void eng_sound(alarm_engine_t *e, uint8_t p, alarm_sound_t sound, uint16_t zone);
void eng_output(alarm_engine_t *e, alarm_output_t out, uint8_t p, alarm_pattern_t pat);
void eng_advance(alarm_engine_t *e, alarm_ms_t now); /* run timers up to now, in order */
void eng_finish(alarm_engine_t *e);                   /* outputs + change notifications */
/* Public entry points: eng_enter() (false: not initialized or re-entered), work, eng_leave(). */
bool eng_enter(alarm_engine_t *e, alarm_ms_t now);
void eng_leave(alarm_engine_t *e);

#endif /* ALARM_ENGINE_INTERNAL_H */
