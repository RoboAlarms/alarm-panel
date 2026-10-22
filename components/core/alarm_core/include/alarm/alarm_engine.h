/*
 * alarm_engine.h - the alarm engine.
 *
 * Usage (single thread; serialize calls through one task/queue):
 *
 *   static alarm_engine_t eng;               // caller owns storage (~16 KB default)
 *   alarm_engine_init(&eng, &cfg, &ports, restored_or_NULL, now_ms);
 *   ... on sensor input:  alarm_engine_zone_event(&eng, zone, ALARM_ZEV_FAULT, now_ms);
 *   ... on keypad input:  alarm_engine_command(&eng, &cmd, &src, now_ms, &res);
 *   ... on timer:         alarm_engine_tick(&eng, now_ms);
 *   ... schedule the next timer at alarm_engine_next_deadline(&eng).
 *
 * Time is a monotonic millisecond count supplied by the caller. Every entry
 * point first advances internal timers to 'now' (in deadline order), so a
 * late tick never reorders events.
 */
#ifndef ALARM_ENGINE_H
#define ALARM_ENGINE_H

#include "alarm/alarm_config.h"
#include "alarm/alarm_ports.h"
#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Status snapshots                                                          */
/* ------------------------------------------------------------------------ */
typedef struct {
    alarm_arm_state_t state;
    alarm_arm_level_t level;       /* ALARM_LEVEL_NONE when disarmed */
    uint8_t arm_flags;             /* ALARM_ARM_* */
    uint8_t armed_by;              /* user number, 0 = none/quick arm */
    /* Who armed it (CORE-USR-035, docs/features/41 D30): {ALARM_SRC_AUTOMATION, uid} when an
     * automation did, as the user armed_by (who saved it): read this first and never name
     * armed_by then. After a restart the kind is kept and the id is 0; {SYSTEM, 0} when
     * disarmed or not known. */
    alarm_source_t arm_source;
    uint8_t ready_levels;          /* ALARM_LEVEL_BIT(level) for each level that can arm now */
    bool ready;                    /* ready to arm AWAY ("READY") */
    bool chime;
    bool walk_test;
    bool triggered;                /* audible/visible alarm active or occurred this armed period */
    bool exit_error;               /* current entry time follows an exit error */
    bool quick_exit;               /* the exit time running restarted an armed partition's (quick
                                    * exit, a momentary-exit zone): no arming of its own */
    bool trouble_unack;            /* trouble beeps pending acknowledgement */
    alarm_sound_t sounder;         /* continuous keypad pattern */
    alarm_pattern_t bell;          /* current bell pattern */
    alarm_ms_t delay_remaining_ms; /* exit / entry time left */
    uint16_t memory;               /* alarm memory kinds: (1 << cause) | ALARM_MEM_CANCELED */
    uint16_t faults;               /* zones currently faulted */
    uint16_t bypassed;
    uint16_t alarms;               /* zones shown in alarm memory */
    uint16_t troubles;             /* zone + system + device troubles shown */
    /* The walk test (docs/features/40): the one running (walk_test), or after it ended the last
     * one's summary (walk_end_how set) until the next start or a restart. */
    alarm_ms_t walk_test_remaining_ms; /* time left while one runs */
    alarm_ms_t walk_started_ms;    /* engine time it started, 0 = none since the restart */
    uint32_t walk_seq;             /* grows with every new mark and every start (WALK-017) */
    uint16_t walk_tested;          /* zones counted in walk_total that are tested */
    uint16_t walk_total;           /* zones the test covers (WALK-007) */
    uint8_t walk_flags;            /* ALARM_WALK_F_* it started with */
    uint8_t walk_started_by;       /* user, 0 = none */
    uint8_t walk_ended_by;         /* user, 0 = the system (a codeless End, the time limit) */
    uint8_t walk_end_how;          /* alarm_walk_end_t, ALARM_WALK_END_NONE while it runs */
    alarm_source_t walk_started_src;
} alarm_partition_status_t;

typedef struct {
    uint16_t flags;        /* ALARM_ZS_* */
    uint8_t trouble;       /* active zone troubles (bit = alarm_zone_trouble_t) */
    uint8_t walk;          /* walk test mark: ALARM_WALK_ZS_* (0 = not tested) */
    uint16_t walk_first_s; /* its first mark, seconds after the test started (saturates) */
    uint8_t walk_trips;    /* trips that beeped during the test (saturates at 255) */
} alarm_zone_status_t;

/* ------------------------------------------------------------------------ */
/* Engine storage. Fields are private; use the API.                          */
/* ------------------------------------------------------------------------ */
typedef struct {
    uint8_t cond;          /* input conditions (private ZC_* bits) */
    uint8_t trouble;       /* active zone troubles */
    uint8_t trouble_mem;   /* restored troubles awaiting code+OFF */
    uint8_t swinger;       /* alarm trips this armed period */
    uint16_t flags;        /* private ZRT_* bits */
    uint8_t verify;        /* fire verification state */
    alarm_ms_t verify_until;
    alarm_ms_t cross_at;   /* last unconfirmed cross-zone trip */
    alarm_ms_t checkin;    /* last supervision check-in */
    alarm_ms_t rewatch_at; /* an automation's bypass ends then if still closed (0 = not counting) */
} alarm_zone_rt_t;

/* A zone's walk test mark (the engine keeps it; alarm_zone_status_t shows it). */
typedef struct {
    uint8_t mark;     /* ALARM_WALK_ZS_* */
    uint8_t trips;
    uint16_t first_s;
} alarm_walk_zone_rt_t;

typedef struct {
    alarm_arm_state_t state;
    alarm_arm_level_t level;
    uint8_t flags;              /* ALARM_ARM_* */
    uint8_t armed_by;
    alarm_source_t arm_source;
    /* Who bypassed the zones marked ZRT_AUTO_BYPASS (an automation, as its maker): their
     * UNBYPASS when watched again names the same (CORE-USR-033). Runtime only. */
    alarm_source_t auto_bypass_src;
    uint8_t auto_bypass_user;
    alarm_ms_t deadline;        /* exit / entry expiry */
    bool exit_fast_done;        /* status updated for the last 10 s of exit time */
    uint8_t exit_restarts;
    uint8_t exit_door;          /* 0 = untouched, 1 = open, 2 = opened and closed */
    bool quick_exit;            /* this exit time restarted an armed partition's, not an arming */
    bool entry_exit_error;
    uint16_t entry_zone;
    uint8_t followers[ALARM_ZONE_BITMAP_BYTES]; /* zones that faulted during entry time */
    alarm_ms_t recent_close_until;
    bool recent_close_sent;
    bool chime;
    bool walk_test;
    alarm_ms_t walk_test_until;
    alarm_ms_t walk_started;    /* 0 = no walk test since the restart */
    alarm_source_t walk_started_src;
    uint32_t walk_seq;
    uint16_t walk_tested;
    uint16_t walk_total;
    uint8_t walk_flags;         /* ALARM_WALK_F_* */
    uint8_t walk_warned;        /* end warnings given: bit 0 = 5 minutes, bit 1 = 1 minute */
    uint8_t walk_started_by;
    uint8_t walk_ended_by;
    uint8_t walk_end_how;       /* alarm_walk_end_t */
    uint16_t sounding;          /* causes currently sounding (1 << cause) */
    alarm_ms_t bell_until;      /* burglary / panic / aux sounding timeout, 0 = none */
    alarm_ms_t fire_until;      /* fire / CO sounding timeout, 0 = none */
    bool strobe;
    alarm_ms_t abort_until;     /* pending burglary reports release time, 0 = none */
    bool alarm_reported;        /* a burglary alarm was reported this armed period */
    alarm_ms_t cancel_until;
    uint16_t memory;            /* alarm memory kinds */
    uint16_t period_alarms;     /* causes that occurred this armed period */
    bool trouble_unack;
    alarm_sound_t last_sounder;
    alarm_pattern_t last_bell;
    bool last_strobe;
    bool dirty;
} alarm_partition_rt_t;

typedef struct {
    alarm_ms_t checkin;
} alarm_device_rt_t;

typedef struct {
    alarm_source_t src;
    bool used;
    uint8_t fails;
    alarm_ms_t last;
    alarm_ms_t locked_until;
} alarm_lockout_t;

typedef struct {
    alarm_event_t ev;
    alarm_ms_t due;
    bool used;
} alarm_pending_report_t;

/* CORE-TRBL-001, P4-013, ZW-039: who holds the one ALARM_TRBL_STORAGE. Each part of the panel
 * that stores something raises and clears its own; the trouble stands while any owner holds it.
 * Runtime only (never stored), so the numbers may grow; append new owners before COUNT. */
typedef enum {
    ALARM_STORAGE_CONFIG = 0, /* the alarm's configuration and runtime state (alarm_service) */
    ALARM_STORAGE_LOG,        /* the event log (panel_log) */
    ALARM_STORAGE_ZONES,      /* the zone bindings (zone_service) */
    ALARM_STORAGE_ZIGBEE,     /* the Zigbee devices (zigbee_service) */
    ALARM_STORAGE_ZWAVE,      /* the Z-Wave network (zwave_service) */
    ALARM_STORAGE_OTHER,      /* alarm_engine_system_trouble(ALARM_TRBL_STORAGE, ...) */
    ALARM_STORAGE_OWNER_COUNT
} alarm_storage_owner_t;

typedef struct {
    alarm_config_t cfg;
    alarm_ports_t ports;
    alarm_ms_t now;
    alarm_ms_t settle_until;      /* 0 = not settling */
    alarm_ms_t smoke_reset_until; /* 0 = smoke power normal */
    uint32_t seq;
    uint32_t sys_trouble;         /* active system troubles */
    uint32_t sys_trouble_mem;     /* restored, awaiting code+OFF */
    uint32_t dev_trouble;         /* devices missing (bit = device - 1) */
    uint32_t dev_trouble_mem;
    uint8_t dev_trouble_partition[ALARM_MAX_DEVICES]; /* scope captured when trouble starts */
    uint32_t storage_owners; /* owners holding ALARM_TRBL_STORAGE (bit = alarm_storage_owner_t);
                                runtime only, never persisted */
    uint8_t default_codes;   /* ALARM_DEFAULT_CODE_* still in use */
    bool program;            /* installer mode */
    bool arm_hold;           /* the platform holds arming (alarm_engine_hold_arming) */
    uint8_t program_user;    /* installer who entered it, 0 = power-up recovery */
    alarm_source_t program_src;
    alarm_ms_t program_until;     /* installer mode idle timeout */
    alarm_ms_t recovery_until;    /* end of the power-up recovery window, 0 = closed */
    alarm_zone_rt_t zones[ALARM_MAX_ZONES];
    alarm_walk_zone_rt_t walk[ALARM_MAX_ZONES]; /* walk test marks (never kept through a restart) */
    alarm_partition_rt_t parts[ALARM_MAX_PARTITIONS];
    alarm_device_rt_t devices[ALARM_MAX_DEVICES];
    alarm_pending_report_t pending[ALARM_MAX_PENDING_REPORTS];
    alarm_lockout_t lockouts[ALARM_MAX_SOURCES];
    bool sys_dirty;
    bool persist_dirty;
    bool config_dirty;
    uint32_t cfg_generation;      /* grows with every configuration change; in RAM only, from 0
                                     at each start (alarm_engine_config_generation) */
    bool in_call;
    bool initialized;
} alarm_engine_t;

/* ------------------------------------------------------------------------ */
/* Lifecycle and time                                                        */
/* ------------------------------------------------------------------------ */

/* Validate cfg, reset the engine and optionally restore persisted runtime state.
 * Emits ALARM_EV_SYSTEM_START. Returns ALARM_ERR_BAD_ARG on invalid input. */
alarm_result_t alarm_engine_init(alarm_engine_t *e, const alarm_config_t *cfg,
                                 const alarm_ports_t *ports,
                                 const alarm_runtime_state_t *restore, alarm_ms_t now);

/* As alarm_engine_init, recording why the panel started as the arg of ALARM_EV_SYSTEM_START
 * (PWR-007). alarm_engine_init records ALARM_START_UNKNOWN. */
alarm_result_t alarm_engine_init_ex(alarm_engine_t *e, const alarm_config_t *cfg,
                                    const alarm_ports_t *ports,
                                    const alarm_runtime_state_t *restore, alarm_ms_t now,
                                    alarm_start_reason_t reason);

/* Advance timers (exit/entry expiry, bell timeout, reports, supervision...). */
void alarm_engine_tick(alarm_engine_t *e, alarm_ms_t now);

/* Earliest time the engine needs a tick, ALARM_TIME_NEVER if idle. */
alarm_ms_t alarm_engine_next_deadline(const alarm_engine_t *e);

/* ------------------------------------------------------------------------ */
/* Inputs                                                                    */
/* ------------------------------------------------------------------------ */
void alarm_engine_zone_event(alarm_engine_t *e, uint16_t zone, alarm_zone_event_t ev,
                             alarm_ms_t now);

/* ALARM_TRBL_STORAGE given here is held by ALARM_STORAGE_OTHER (alarm_engine_storage_trouble), so
 * it can't clear what another owner still holds. */
void alarm_engine_system_trouble(alarm_engine_t *e, alarm_sys_trouble_t trouble, bool active,
                                 alarm_ms_t now);

/* CORE-TRBL-001, P4-013, ZW-039: owner raises (active) or clears its hold on ALARM_TRBL_STORAGE.
 * The trouble is raised when the first owner raises it and restored when the last one clears,
 * one event each; repeating a call, or clearing a hold never raised, changes nothing. */
void alarm_engine_storage_trouble(alarm_engine_t *e, alarm_storage_owner_t owner, bool active,
                                  alarm_ms_t now);

/* Owners holding ALARM_TRBL_STORAGE (bit = alarm_storage_owner_t). */
uint32_t alarm_engine_storage_owners(const alarm_engine_t *e);

/* CORE-ARM-013: the platform holds arming while a sensor radio can't hear its sensors on
 * purpose - its own firmware updating (docs/features/36 network.md §14). Every arm request then
 * fails ALARM_ERR_HELD (logged as ARM_FAILED) and no partition reads ready; disarming, alarms,
 * panics and everything else go on. Not kept across a restart: the platform holds it again if it
 * still must. */
void alarm_engine_hold_arming(alarm_engine_t *e, bool hold);
bool alarm_engine_arming_held(const alarm_engine_t *e);

/* Supervised device (keypad, expander, radio) heartbeat. */
void alarm_engine_device_checkin(alarm_engine_t *e, uint8_t device, alarm_ms_t now);

/* CORE-TRBL-008, ZW-038: the platform says a device can't be reached (false) or is heard again
 * (true): the same device supervision trouble and restore as a missed check-in, whatever the
 * device's supervision_s. Idempotent: only a change emits. */
void alarm_engine_device_reachable(alarm_engine_t *e, uint8_t device, bool reachable,
                                   alarm_ms_t now);
/* LINK-007: external keypads supply their assigned home partition (0 means all).
 * A restore retains the scope of the original trouble, even after reassignment. */
void alarm_engine_device_reachable_at(alarm_engine_t *e, uint8_t device, uint8_t partition,
                                      bool reachable, alarm_ms_t now);

/* ALARM_CMD_WALK_TEST (docs/features/40 WALK-001/002/025): walk_op START and EXTEND, and a
 * TOGGLE that starts, need the installer, the system master or a partition master; a TOGGLE that
 * ends needs only what a disarm of the partition needs, and END with a code the same, while END
 * without a code ends it as the system (user 0): ending only gives protection back. A START on a
 * partition that isn't disarmed, silent, out of installer mode and idle, and an END or EXTEND
 * with no test running, fail ALARM_ERR_BAD_STATE and change nothing. */
alarm_result_t alarm_engine_command(alarm_engine_t *e, const alarm_cmd_t *cmd,
                                    const alarm_source_t *src, alarm_ms_t now,
                                    alarm_cmd_result_t *result /* may be NULL */);

/* CORE-USR-027: run a command as a user the platform has already identified (a Panel Manager
 * sign-in: its password proved who it is), without a code. The same rules as that user's code
 * (CORE-USR-015's rule for button zones): the slot must hold a user (authority != NONE), the
 * user must have the command's partition (every command, silence and panic too), and the
 * command's own authority check applies exactly as for that user's code (an arm-only user
 * never disarms, a guest disarms only what the guest armed...). user 0 or an empty slot ->
 * ALARM_ERR_NOT_AUTHORIZED. cmd->code is ignored and nothing counts toward a keypad lockout.
 * Everything else is exactly alarm_engine_command()'s (duress, memory, program mode, states,
 * the error sound on a refusal); the events name the user and src. */
alarm_result_t alarm_engine_command_as(alarm_engine_t *e, const alarm_cmd_t *cmd,
                                       const alarm_source_t *src, uint8_t user,
                                       alarm_ms_t now, alarm_cmd_result_t *result /* may be NULL */);

/* Check a code (counts toward lockout). partition 0 = any partition. */
alarm_result_t alarm_engine_authenticate(alarm_engine_t *e, const alarm_source_t *src,
                                         const char *code, uint8_t partition, alarm_ms_t now,
                                         uint8_t *user_out);

/* ------------------------------------------------------------------------ */
/* Users and configuration                                                   */
/* ------------------------------------------------------------------------ */
alarm_result_t alarm_engine_user_set(alarm_engine_t *e, const alarm_source_t *src,
                                     const char *actor_code, uint8_t user_no,
                                     const char *new_code, alarm_authority_t authority,
                                     uint8_t partitions, const char *name, alarm_ms_t now);

alarm_result_t alarm_engine_user_delete(alarm_engine_t *e, const alarm_source_t *src,
                                        const char *actor_code, uint8_t user_no,
                                        alarm_ms_t now);

/* Rename an existing user or change its authority/partitions; the code stays. Same rules as
 * alarm_engine_user_set (name NULL keeps the name). */
alarm_result_t alarm_engine_user_update(alarm_engine_t *e, const alarm_source_t *src,
                                        const char *actor_code, uint8_t user_no,
                                        alarm_authority_t authority, uint8_t partitions,
                                        const char *name, alarm_ms_t now);

/* CORE-USR-028: alarm_engine_user_set(), _delete() and _update() by a user the platform has
 * already identified (a Panel Manager sign-in: its password proved who it is), without a code.
 * The actor's slot must hold a user, read again every time (user 0 or an empty slot ->
 * ALARM_ERR_NOT_AUTHORIZED); from there the rules are exactly those of the actor's code -
 * whom they manage (CORE-USR-025), what a partition master may hand out, the partitions idle.
 * Nothing counts toward a keypad lockout and a locked-out source still runs it; the events
 * name the actor and src. */
alarm_result_t alarm_engine_user_set_as(alarm_engine_t *e, const alarm_source_t *src,
                                        uint8_t actor, uint8_t user_no, const char *new_code,
                                        alarm_authority_t authority, uint8_t partitions,
                                        const char *name, alarm_ms_t now);
alarm_result_t alarm_engine_user_delete_as(alarm_engine_t *e, const alarm_source_t *src,
                                           uint8_t actor, uint8_t user_no, alarm_ms_t now);
alarm_result_t alarm_engine_user_update_as(alarm_engine_t *e, const alarm_source_t *src,
                                           uint8_t actor, uint8_t user_no,
                                           alarm_authority_t authority, uint8_t partitions,
                                           const char *name, alarm_ms_t now);

/* Replace the configuration (installer programming). Only while every
 * partition is disarmed, silent and not in walk test. */
alarm_result_t alarm_engine_set_config(alarm_engine_t *e, const alarm_config_t *cfg,
                                       alarm_ms_t now);

const alarm_config_t *alarm_engine_config(const alarm_engine_t *e);

/* The configuration's generation: it changes with every change of the configuration (a write,
 * a user change, a voice approval or withdrawal), never repeating while the panel runs. A caller
 * that showed a person something from the configuration passes it back with the request that
 * follows, so that a request based on what is no longer there is refused as stale. */
uint32_t alarm_engine_config_generation(const alarm_engine_t *e);

/* Approve a zone's spoken name (docs/features/08 voice-first-build.md D07, CORE-CFG-021): the
 * one way approval is granted (the touchscreen's Done, the Panel Manager), in the alarm task.
 * expected_generation is alarm_engine_config_generation() when the name was shown,
 * expected_binding and words the zone's binding revision and words shown, pack_revision the
 * active voice pack's revision as the trusted voice layer validated it. BUSY unless every
 * partition is idle (as alarm_engine_set_config); STALE when the generation moved on, or as
 * alarm_config_voice_approve() says. On OK the change is announced like any configuration
 * change (ALARM_EV_CONFIG_CHANGED, on_config_changed: the platform stores it). */
alarm_voice_result_t alarm_engine_voice_approve(alarm_engine_t *e, uint16_t zone_no,
                                                uint32_t expected_generation,
                                                uint32_t expected_binding,
                                                const uint16_t words[ALARM_ZONE_WORDS],
                                                uint32_t pack_revision, alarm_ms_t now);

/* Withdraw a zone's approval explicitly, the same way (alarm_config_voice_withdraw). */
alarm_voice_result_t alarm_engine_voice_withdraw(alarm_engine_t *e, uint16_t zone_no,
                                                 uint32_t expected_generation,
                                                 uint32_t expected_binding, alarm_ms_t now);

/* ------------------------------------------------------------------------ */
/* Installer mode (programming mode)                                         */
/* ------------------------------------------------------------------------ */
#define ALARM_PROGRAM_TIMEOUT_S 900 /* installer mode ends after 15 min without activity */
#define ALARM_RECOVERY_WINDOW_S 120 /* power-up recovery: installer mode without a code */

/* Enter installer mode with the installer code, or with no code (NULL or "") while the
 * power-up recovery window is open (options.power_up_program, the first
 * ALARM_RECOVERY_WINDOW_S after init). Every partition must be
 * disarmed, silent and out of walk test. While active nothing can be armed; fire, CO, 24-hour
 * zones and panics stay live. Logged and reported (program_enter). Calling it again from the
 * same source refreshes the timeout; another source gets ALARM_ERR_BAD_STATE. */
alarm_result_t alarm_engine_program_enter(alarm_engine_t *e, const alarm_source_t *src,
                                          const char *code, alarm_ms_t now, uint8_t *user_out);

/* CORE-PROG-011: installer mode for a user the platform has already identified (a Panel
 * Manager sign-in), without a code: the slot must hold the installer, read again now (anyone
 * else, user 0 or an empty slot -> ALARM_ERR_NOT_AUTHORIZED). Nothing counts toward a lockout;
 * everything else is alarm_engine_program_enter()'s (every partition idle, one source at a
 * time, logged and reported as program_enter by that user and src). */
alarm_result_t alarm_engine_program_enter_as(alarm_engine_t *e, const alarm_source_t *src,
                                             uint8_t user, alarm_ms_t now);

/* Leave installer mode. Logged and reported (program_exit). */
void alarm_engine_program_exit(alarm_engine_t *e, const alarm_source_t *src, alarm_ms_t now);

/* Installer activity: restarts the ALARM_PROGRAM_TIMEOUT_S idle timeout (set_config does too). */
void alarm_engine_program_keepalive(alarm_engine_t *e, alarm_ms_t now);

/* In installer mode, from the source that entered it: set the installer (1) or system master
 * (2) code without knowing the old one. This is how a lost installer code is replaced after the
 * power-up recovery. Logged as user_changed. */
alarm_result_t alarm_engine_program_set_code(alarm_engine_t *e, const alarm_source_t *src,
                                             uint8_t user_no, const char *new_code,
                                             alarm_ms_t now);

bool alarm_engine_program_active(const alarm_engine_t *e);

/* Time left before `src` may try a code again, 0 when it is not locked out (KPD-008). */
alarm_ms_t alarm_engine_lockout_left(const alarm_engine_t *e, const alarm_source_t *src,
                                     alarm_ms_t now);

/* Time left in the power-up recovery window, 0 when closed. */
alarm_ms_t alarm_engine_recovery_left(const alarm_engine_t *e);

/* Events the platform records through the engine so they are numbered, logged and reported
 * like engine events. Allowed: ALARM_EV_FACTORY_RESET (arg bit 0: event log erased too),
 * ALARM_EV_FIRMWARE_UPDATE (arg: alarm_fw_stage_t; a keypad's stage only from src
 * {ALARM_SRC_KEYPAD, its device number 1..ALARM_MAX_DEVICES}), ALARM_EV_BACKUP_MADE and
 * ALARM_EV_BACKUP_RESTORED and ALARM_EV_CLOCK_SET (arg 0; user = who set the clock). */
alarm_result_t alarm_engine_platform_event(alarm_engine_t *e, alarm_event_type_t type,
                                           uint8_t arg, const alarm_source_t *src, uint8_t user,
                                           alarm_ms_t now);

/* The same with a zone and a second argument, for the Z-Wave network's events (docs/features/36
 * network.md §15; zone at most ALARM_MAX_ZONES, 0 none): ALARM_EV_ZWAVE_ADDED (arg2
 * ALARM_ZW_JOIN_S2_UNAUTH..S0), _ADDED_INSECURE (arg2 NOT_SECURE, FAILED or DOWNGRADED), _REMOVED
 * (arg2 alarm_zw_removed_t), _LEFT (arg2 alarm_zw_left_t), _NETWORK_RESET and
 * _RADIO_RESTARTED (zone 0); and the Zigbee network's (docs/features/20 devices.md §11):
 * ALARM_EV_ZIGBEE_ADDED (arg2 alarm_zb_join_t), _REMOVED, _FORGOTTEN, _LEFT and _REPLACED (arg2
 * 0), _JOIN_REFUSED (zone 0, arg 0, arg2 alarm_zb_refused_t), _CHANNEL (zone 0, arg 0, arg2 the
 * channel), _NETWORK_RESET (all 0) and _RADIO_RESTARTED (zone 0, arg 0, arg2 alarm_zb_restart_t);
 * and ALARM_EV_AUTOMATION_FAILED (docs/features/41 AUT-009: zone the triggering zone or 0, arg an
 * alarm_auto_fail_t, arg2 below ALARM_AUTO_ACT_LIMIT, src {ALARM_SRC_AUTOMATION, uid}); and
 * ALARM_EV_FIRMWARE_UPDATE's ALARM_FW_KEYPAD_FAILED (zone 0, arg2 the link's why, not 0).
 * The two above go through here with zone 0 and arg2 0. */
alarm_result_t alarm_engine_platform_event_ex(alarm_engine_t *e, alarm_event_type_t type,
                                              uint16_t zone, uint8_t arg, uint8_t arg2,
                                              const alarm_source_t *src, uint8_t user,
                                              alarm_ms_t now);

/* Someone put an automation's arming of partition p off (docs/features/41 AUT-013, D26, D30;
 * CORE-USR-036): ALARM_EV_ARMING_PUT_OFF with user (whose code: 1..ALARM_MAX_USERS), source
 * {ALARM_SRC_AUTOMATION, uid} (uid not 0), arg choice and arg2 the minutes from the event's
 * time to until_epoch_ms (ALARM_PUT_OFF_HOUR's new arm time; 0 or a clock not set: none). The
 * partition must be enabled. Reported, never silent; nothing else changes. */
alarm_result_t alarm_engine_arming_put_off(alarm_engine_t *e, uint8_t partition, uint16_t uid,
                                           uint8_t user, alarm_put_off_t choice,
                                           int64_t until_epoch_ms, alarm_ms_t now);

/* An automation's notification to the phones (docs/features/41 BL-224; CORE-PROG-013):
 * ALARM_EV_AUTOMATION_NOTICE with source {ALARM_SRC_AUTOMATION, uid} (uid not 0), partition (0,
 * or an enabled one), zone (0..ALARM_MAX_ZONES), arg an alarm_auto_notice_t and arg2 its level or
 * trouble (with ALARM_AUTO_NOTICE_TRIED). Logged and reported now; nothing else changes. */
alarm_result_t alarm_engine_automation_notice(alarm_engine_t *e, uint16_t uid, uint8_t partition,
                                              uint16_t zone, uint8_t arg, uint8_t arg2,
                                              alarm_ms_t now);

/* ALARM_EV_ARMING_PUT_OFF's arg2 for an until time: whole minutes from ev_epoch_ms, rounded,
 * 1..255 (a longer wait is cut to 255); 0 when either time is unknown or until isn't later. */
uint8_t alarm_put_off_minutes(int64_t ev_epoch_ms, int64_t until_epoch_ms);
/* Its until time back, on the whole minute (the arm time it stood for); 0 when not known. */
int64_t alarm_put_off_until(const alarm_event_t *ev);

/* ------------------------------------------------------------------------ */
/* Queries                                                                   */
/* ------------------------------------------------------------------------ */
void alarm_engine_partition_status(const alarm_engine_t *e, uint8_t partition,
                                   alarm_partition_status_t *out);

void alarm_engine_zone_status(const alarm_engine_t *e, uint16_t zone, alarm_zone_status_t *out);

/* Active system troubles; *restored_unacked receives restored-but-unacknowledged. */
uint32_t alarm_engine_system_troubles(const alarm_engine_t *e, uint32_t *restored_unacked);

/* Missing supervised devices (bit = device - 1). */
uint32_t alarm_engine_device_troubles(const alarm_engine_t *e);

void alarm_engine_runtime_state(const alarm_engine_t *e, alarm_runtime_state_t *out);

/* Factory codes still in use (the UI shows a "change the default codes" reminder). */
#define ALARM_DEFAULT_CODE_INSTALLER 0x01u
#define ALARM_DEFAULT_CODE_MASTER    0x02u
uint8_t alarm_engine_default_codes(const alarm_engine_t *e);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_ENGINE_H */
