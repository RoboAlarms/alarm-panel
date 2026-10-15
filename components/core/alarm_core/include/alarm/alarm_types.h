/*
 * alarm_types.h - shared enums and small value types for the portable alarm core.
 *
 * The core is plain C11: no heap, no RTOS, no ESP-IDF headers. All externally
 * visible numbers (partition, zone, user, device) are 1-based; 0 always means
 * "none" / "system".
 */
#ifndef ALARM_TYPES_H
#define ALARM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Capacities. Override per target with -D (or Kconfig on ESP-IDF).          */
/* ------------------------------------------------------------------------ */
#ifndef ALARM_MAX_PARTITIONS
#define ALARM_MAX_PARTITIONS 4
#endif
#ifndef ALARM_MAX_ZONES
#define ALARM_MAX_ZONES 128
#endif
#ifndef ALARM_MAX_USERS
#define ALARM_MAX_USERS 64
#endif
#ifndef ALARM_MAX_DEVICES
#define ALARM_MAX_DEVICES 16 /* supervised keypads / expanders / radios */
#endif
#ifndef ALARM_MAX_SOURCES
#define ALARM_MAX_SOURCES 16 /* command sources tracked for code lockout */
#endif
#ifndef ALARM_MAX_PENDING_REPORTS
#define ALARM_MAX_PENDING_REPORTS 24 /* reports held back by the abort window */
#endif

#if ALARM_MAX_PARTITIONS < 1 || ALARM_MAX_PARTITIONS > 8
#error "ALARM_MAX_PARTITIONS must be 1..8 (partition masks are 8 bits)"
#endif
#if ALARM_MAX_ZONES < 1 || ALARM_MAX_ZONES > 999
#error "ALARM_MAX_ZONES must be 1..999 (keypad entry uses up to 3 digits)"
#endif
#if ALARM_MAX_USERS < 2 || ALARM_MAX_USERS > 99
#error "ALARM_MAX_USERS must be 2..99 (keypad entry uses 2-digit user numbers)"
#endif
#if ALARM_MAX_DEVICES < 1 || ALARM_MAX_DEVICES > 32
#error "ALARM_MAX_DEVICES must be 1..32 (device troubles are a 32-bit mask)"
#endif

/* Names of zones, users, partitions and devices: UTF-8, bytes including the NUL
 * (docs/features/37 I18N-009, open question 2: 48, decided 2026-09-28). Firmware before
 * 2026-09-29 stored and read 24; the codec still writes a whole-character cut of that length
 * where such firmware looks, and the whole name in a field it skips (codec.c). */
#ifndef ALARM_NAME_LEN
#define ALARM_NAME_LEN 48
#endif
#define ALARM_NAME_LEGACY_LEN 24
#if ALARM_NAME_LEN < ALARM_NAME_LEGACY_LEN || ALARM_NAME_LEN > 128
#error "ALARM_NAME_LEN must be 24..128"
#endif
#define ALARM_CODE_MIN_LEN  4  /* codes are 4-10 digits, chosen per user (iPhone-style) */
#define ALARM_CODE_MAX_LEN  10
#define ALARM_CODE_HASH_LEN 32
#define ALARM_CODE_SALT_LEN 16

#define ALARM_ZONE_BITMAP_BYTES ((ALARM_MAX_ZONES + 7) / 8)

/* Monotonic milliseconds supplied by the caller. 64 bits: never wraps. */
typedef uint64_t alarm_ms_t;
#define ALARM_TIME_NEVER UINT64_MAX

/* ------------------------------------------------------------------------ */
/* Partition arming                                                          */
/* ------------------------------------------------------------------------ */
typedef enum {
    ALARM_STATE_DISARMED = 0,
    ALARM_STATE_EXIT_DELAY,  /* arming: exit time running */
    ALARM_STATE_ARMED,
    ALARM_STATE_ENTRY_DELAY, /* armed, entry time running */
} alarm_arm_state_t;

typedef enum {
    ALARM_LEVEL_NONE = 0,
    ALARM_LEVEL_AWAY,  /* everything armed */
    ALARM_LEVEL_STAY,  /* perimeter + entry/exit; interior bypassed ("Home") */
    ALARM_LEVEL_NIGHT, /* STAY + interior zones flagged ALARM_ZF_NIGHT */
} alarm_arm_level_t;

/* Arm flags. INSTANT = STAY + NO_ENTRY_DELAY, MAXIMUM = AWAY + NO_ENTRY_DELAY. */
#define ALARM_ARM_NO_ENTRY_DELAY 0x01u
#define ALARM_ARM_SILENT_EXIT    0x02u /* mute exit beeps; exit time doubles */
#define ALARM_ARM_BYPASS_FAULTED 0x04u /* command only: bypass not-ready zones, then arm */

/* ALARM_EV_ARMED's arg2: the ALARM_ARM_* mode flags in bits 0-2 and, in bits 3-7, how many zones
 * an automation bypassed at that arming (docs/features/41 AUT-014, CORE-USR-032): 0 = none,
 * ALARM_ARMED_BYPASSED_MAX = that many or more. Readers that mask the flags ignore it. */
#define ALARM_ARMED_BYPASSED_SHIFT 3u
#define ALARM_ARMED_BYPASSED_MAX   31u
#define ALARM_ARMED_BYPASSED(arg2) ((uint8_t)((unsigned)(arg2) >> ALARM_ARMED_BYPASSED_SHIFT))

/* Bitmask of levels, used by partition status "ready_levels". */
#define ALARM_LEVEL_BIT(level) ((uint8_t)(1u << (unsigned)(level)))

/* ------------------------------------------------------------------------ */
/* Zones                                                                     */
/* ------------------------------------------------------------------------ */
/* Zone type numbers (stored in the configuration). */
typedef enum {
    ALARM_ZT_NOT_USED = 0,
    ALARM_ZT_ENTRY_EXIT_1 = 1,
    ALARM_ZT_ENTRY_EXIT_2 = 2,
    ALARM_ZT_PERIMETER = 3,
    ALARM_ZT_INTERIOR_FOLLOWER = 4,
    ALARM_ZT_DAY_NIGHT = 5, /* trouble by day / alarm by night */
    ALARM_ZT_24H_SILENT = 6,
    ALARM_ZT_24H_AUDIBLE = 7,
    ALARM_ZT_24H_AUX = 8,
    ALARM_ZT_FIRE = 9,
    ALARM_ZT_INTERIOR_DELAY = 10,
    ALARM_ZT_MONITOR = 12,
    ALARM_ZT_CO = 14,
    ALARM_ZT_FIRE_VERIFIED = 16,
    ALARM_ZT_ARM_STAY = 20, /* button: arm stay */
    ALARM_ZT_ARM_AWAY = 21, /* button: arm away */
    ALARM_ZT_DISARM = 22,   /* button: disarm */
    ALARM_ZT_NO_RESPONSE = 23,
    ALARM_ZT_SILENT_BURGLARY = 24,
    ALARM_ZT_MOMENTARY_EXIT = 29,
    ALARM_ZT_KEYSWITCH = 77,
} alarm_zone_type_t;

/* Zone configuration flags. */
#define ALARM_ZF_CHIME          0x0001u /* chime while disarmed (if partition chime is on) */
#define ALARM_ZF_NIGHT          0x0002u /* interior zone armed in NIGHT level */
#define ALARM_ZF_BYPASSABLE     0x0004u /* allow bypass of a 24-hour zone */
#define ALARM_ZF_NO_REPORT      0x0008u /* log locally, never report remotely */
#define ALARM_ZF_SWINGER_EXEMPT 0x0010u /* never shut down by swinger suppression */

/* Presentation hint for UIs / Home Assistant device classes. */
typedef enum {
    ALARM_DEVCLASS_GENERIC = 0,
    ALARM_DEVCLASS_DOOR,
    ALARM_DEVCLASS_WINDOW,
    ALARM_DEVCLASS_GARAGE_DOOR,
    ALARM_DEVCLASS_MOTION,
    ALARM_DEVCLASS_GLASS_BREAK,
    ALARM_DEVCLASS_SMOKE,
    ALARM_DEVCLASS_HEAT,
    ALARM_DEVCLASS_CO,
    ALARM_DEVCLASS_GAS,
    ALARM_DEVCLASS_WATER,
    ALARM_DEVCLASS_TEMPERATURE,
    ALARM_DEVCLASS_VIBRATION,
    ALARM_DEVCLASS_PANIC_BUTTON,
    ALARM_DEVCLASS_KEYFOB,
    ALARM_DEVCLASS_KEYSWITCH,
} alarm_device_class_t;

/* Inputs reported by sensor sources (wired, radio, keypad-attached, simulator). */
typedef enum {
    ALARM_ZEV_RESTORE = 0, /* loop normal (closed / idle) */
    ALARM_ZEV_FAULT,       /* loop faulted (open / motion / smoke) */
    ALARM_ZEV_TROUBLE,     /* loop or sensor trouble (e.g. fire loop open circuit) */
    ALARM_ZEV_TROUBLE_RESTORE,
    ALARM_ZEV_TAMPER,
    ALARM_ZEV_TAMPER_RESTORE,
    ALARM_ZEV_LOW_BATTERY,
    ALARM_ZEV_LOW_BATTERY_RESTORE,
    ALARM_ZEV_CHECKIN,     /* supervision heartbeat only */
    ALARM_ZEV_UNREACHABLE, /* the source can't reach the sensor: supervision trouble (CHECK) now */
    ALARM_ZEV_REACHABLE,   /* the sensor answers again: clears that CHECK, like a check-in */
    /* The sensor reports its own test (a smoke detector's test button over Z-Wave or Zigbee):
     * during a walk test it marks the zone tested without an alarm, fire and CO included
     * (docs/features/40 WALK-007); otherwise it is only a check-in. Append only. */
    ALARM_ZEV_TEST,
} alarm_zone_event_t;

/* Zone status flags (alarm_zone_status_t.flags). */
#define ALARM_ZS_FAULT       0x0001u
#define ALARM_ZS_BYPASSED    0x0002u
#define ALARM_ZS_ALARM       0x0004u /* in alarm memory */
#define ALARM_ZS_TROUBLE     0x0008u /* loop / sensor trouble (CHECK) */
#define ALARM_ZS_TAMPER      0x0010u
#define ALARM_ZS_LOW_BATTERY 0x0020u
#define ALARM_ZS_SUPERVISION 0x0040u /* missed check-in (CHECK) */
#define ALARM_ZS_SWINGER     0x0080u /* shut down by swinger suppression */
#define ALARM_ZS_VERIFYING   0x0100u /* fire alarm verification in progress */
#define ALARM_ZS_NOT_READY   0x0200u /* blocks arming of at least one level */
#define ALARM_ZS_TROUBLE_MEM 0x0400u /* restored trouble awaiting code+OFF */

/* A zone's walk test mark (alarm_zone_status_t.walk, docs/features/40 WALK-006/008): kept from
 * a walk test's first trip of the zone until the partition's next walk test starts, a change
 * of the zone's type or partition, or a restart. */
#define ALARM_WALK_ZS_TESTED     0x01u /* tested: a fault, a button press or the sensor's test */
#define ALARM_WALK_ZS_COVER      0x02u /* its cover (tamper) was tested */
#define ALARM_WALK_ZS_KIND_SHIFT 2u    /* bits 2-3: how it was first marked (alarm_walk_mark_t) */
#define ALARM_WALK_ZS_KIND(w) ((alarm_walk_mark_t)(((unsigned)(w) >> ALARM_WALK_ZS_KIND_SHIFT) & 3u))

/* Zone trouble kinds (bit numbers in zone trouble masks, and event arg). */
typedef enum {
    ALARM_ZTRBL_LOOP = 0,    /* sensor / loop trouble */
    ALARM_ZTRBL_TAMPER,      /* tamper while disarmed (or 24h zone) */
    ALARM_ZTRBL_LOW_BATTERY, /* sensor battery */
    ALARM_ZTRBL_SUPERVISION, /* missed check-in */
    ALARM_ZTRBL_DAY_FAULT,   /* trouble-by-day zone faulted while disarmed */
    ALARM_ZTRBL_COUNT
} alarm_zone_trouble_t;

/* ------------------------------------------------------------------------ */
/* System troubles (bit numbers in a 32-bit mask)                            */
/* ------------------------------------------------------------------------ */
typedef enum {
    ALARM_TRBL_AC_LOSS = 0,
    ALARM_TRBL_LOW_BATTERY,  /* panel battery low */
    ALARM_TRBL_COMM_FAIL,    /* reports not reaching MQTT / server (FC) */
    ALARM_TRBL_NETWORK,      /* Wi-Fi / LAN down */
    ALARM_TRBL_CLOCK_NOT_SET,
    ALARM_TRBL_COPROCESSOR,  /* Wi-Fi co-processor (C6) failure */
    ALARM_TRBL_SENSOR_RADIO, /* sensor radio (H2) failure or RF jam */
    ALARM_TRBL_BELL,         /* siren supervision */
    ALARM_TRBL_PANEL_TAMPER,
    ALARM_TRBL_STORAGE,
    ALARM_TRBL_COUNT
} alarm_sys_trouble_t;

/* Event arg for trouble events that concern a supervised device. */
#define ALARM_TRBL_DEVICE_SUPERVISION 0xF0u

/* ------------------------------------------------------------------------ */
/* Alarms                                                                    */
/* ------------------------------------------------------------------------ */
typedef enum {
    ALARM_CAUSE_NONE = 0,
    ALARM_CAUSE_BURGLARY,        /* entry expired, perimeter, interior */
    ALARM_CAUSE_SILENT_BURGLARY,
    ALARM_CAUSE_EXIT_ERROR,      /* entry time after an exit error expired */
    ALARM_CAUSE_TAMPER,          /* tamper on an armed burglary zone */
    ALARM_CAUSE_24H_SILENT,
    ALARM_CAUSE_24H_AUDIBLE,
    ALARM_CAUSE_24H_AUX,         /* keypad sounding only */
    ALARM_CAUSE_FIRE,
    ALARM_CAUSE_CO,
    ALARM_CAUSE_PANIC_POLICE,    /* audible police panic */
    ALARM_CAUSE_PANIC_SILENT,    /* silent hold-up panic */
    ALARM_CAUSE_PANIC_FIRE,
    ALARM_CAUSE_PANIC_MEDICAL,   /* keypad sounding only */
    ALARM_CAUSE_COUNT
} alarm_cause_t;

/* Partition alarm-memory mask: bit (1 << cause), plus: */
#define ALARM_MEM_CANCELED 0x8000u /* alarm was canceled/aborted by a disarm */

typedef enum {
    ALARM_PANIC_POLICE = 0,
    ALARM_PANIC_SILENT,
    ALARM_PANIC_FIRE,
    ALARM_PANIC_MEDICAL,
} alarm_panic_t;

/* ------------------------------------------------------------------------ */
/* Sounds and outputs                                                        */
/* ------------------------------------------------------------------------ */
typedef enum {
    /* continuous keypad patterns (partition status "sounder") */
    ALARM_SOUND_NONE = 0,
    ALARM_SOUND_TROUBLE,      /* trouble beeps until acknowledged */
    ALARM_SOUND_EXIT,         /* slow exit beeps */
    ALARM_SOUND_EXIT_FAST,    /* last 10 s of exit time */
    ALARM_SOUND_ENTRY,        /* entry delay */
    ALARM_SOUND_ENTRY_MEMORY, /* rapid: an alarm happened while armed */
    ALARM_SOUND_AUX,          /* 24h aux / medical: keypad only */
    ALARM_SOUND_ALARM,        /* burglary / audible panic */
    ALARM_SOUND_CO,           /* temporal-4 */
    ALARM_SOUND_FIRE,         /* temporal-3 */
    /* one-shot sounds (ports.on_sound) */
    ALARM_SOUND_ACK,          /* single beep: command accepted */
    ALARM_SOUND_ERROR,        /* long beep: rejected / invalid code */
    ALARM_SOUND_CHIME,        /* chime (zone given: voice chime possible) */
    ALARM_SOUND_WALK_TEST,    /* zone faulted during walk test */
    ALARM_SOUND_WALK_TEST_WARNING, /* double beep: the walk test ends in 5 or 1 minutes */
} alarm_sound_t;

typedef enum {
    ALARM_OUT_BELL = 0,    /* per partition siren */
    ALARM_OUT_STROBE,      /* per partition, on from alarm until disarm */
    ALARM_OUT_SMOKE_RESET, /* system (partition 0): interrupt smoke power */
} alarm_output_t;

typedef enum {
    ALARM_PAT_OFF = 0,
    ALARM_PAT_STEADY,     /* burglary / audible panic */
    ALARM_PAT_TEMPORAL3,  /* fire */
    ALARM_PAT_TEMPORAL4,  /* CO */
    ALARM_PAT_TEST,       /* short bell test pulse (walk test start) */
    ALARM_PAT_ON,         /* strobe / smoke reset active */
} alarm_pattern_t;

/* ------------------------------------------------------------------------ */
/* Users and command sources                                                 */
/* ------------------------------------------------------------------------ */
typedef enum {
    ALARM_AUTH_NONE = 0, /* empty user slot */
    ALARM_AUTH_INSTALLER,
    ALARM_AUTH_MASTER,           /* system master */
    ALARM_AUTH_PARTITION_MASTER,
    ALARM_AUTH_STANDARD,
    ALARM_AUTH_ARM_ONLY,
    ALARM_AUTH_GUEST,            /* may only disarm if they armed */
    ALARM_AUTH_DURESS,           /* arms/disarms normally + silent duress event */
} alarm_authority_t;

#define ALARM_USER_INSTALLER 1 /* user 01 */
#define ALARM_USER_MASTER    2 /* user 02 */

typedef enum {
    ALARM_SRC_SYSTEM = 0, /* engine / platform internal */
    ALARM_SRC_KEYPAD,     /* local or remote keypad; id = device number */
    ALARM_SRC_REMOTE,     /* MQTT / Home Assistant / server; id = client slot */
    ALARM_SRC_ZONE,       /* keyfob button or keyswitch zone; id = zone */
    ALARM_SRC_SCHEDULE,
    /* An automation (docs/features/41); id = the automation's uid. The engine lets only an arm
     * with no flags (or only BYPASS_FAULTED: the automation's "Bypass open zones", 41 D29, open
     * burglary zones only) and a disarm through, run as the person who saved it (AUT-011,
     * ADR-050, CORE-USR-029); never panic, silence, a bypass command or a code check. */
    ALARM_SRC_AUTOMATION,
} alarm_source_kind_t;

typedef struct {
    uint8_t kind; /* alarm_source_kind_t */
    uint16_t id;
} alarm_source_t;

#define ALARM_SRC_ID_PANEL_MANAGER 31u /* ALARM_SRC_REMOTE id of the browser Panel Manager */

/* Keypad source ids (alarm_source_t.id with ALARM_SRC_KEYPAD). Remote keypads use their
 * device number 1..ALARM_MAX_DEVICES. */
#define ALARM_KEYPAD_LOCAL   0u      /* this panel's own touchscreen */
#define ALARM_KEYPAD_CONSOLE 0xFFFEu /* serial console (bring-up and service) */

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */
typedef enum {
    ALARM_CMD_ARM = 0,
    ALARM_CMD_DISARM,       /* code + OFF: disarm / silence / clear memory / ack troubles */
    ALARM_CMD_SILENCE,      /* OFF without code: silence fire/CO/aux + trouble beeps */
    ALARM_CMD_BYPASS,
    ALARM_CMD_UNBYPASS,
    ALARM_CMD_QUICK_BYPASS, /* bypass every not-ready zone */
    ALARM_CMD_CHIME,        /* toggle chime */
    ALARM_CMD_EXIT_RESTART, /* quick exit / [*] while armed STAY or NIGHT */
    ALARM_CMD_PANIC,
    ALARM_CMD_WALK_TEST,
    /* Check a code and change nothing (docs/features/41 AUT-013's put-off; CORE-USR-034): it
     * authenticates as a disarm by that code would - a wrong code counts toward the source's
     * lockout, a locked-out source is refused - and answers the code's user and whether that user
     * may disarm the partition (alarm_cmd_result_t). No event of its own: a wrong code is
     * INVALID_CODE (and LOCKOUT) with the error sound, as for any code, and a duress code emits
     * the silent ALARM_EV_DURESS a disarm by it would. Never from ALARM_SRC_AUTOMATION. */
    ALARM_CMD_CODE_CHECK,
} alarm_cmd_type_t;

/* ALARM_CMD_WALK_TEST's operation (docs/features/40 WALK-002). A zeroed command toggles, as
 * the keypad's code + TEST always has. */
typedef enum {
    ALARM_WALK_TOGGLE = 0, /* start when none runs, else end */
    ALARM_WALK_START,      /* only while disarmed, silent, out of installer mode, not testing */
    ALARM_WALK_END,        /* only while testing */
    ALARM_WALK_EXTEND,     /* only while testing: the full limit again */
} alarm_walk_op_t;

/* ALARM_CMD_WALK_TEST's start options (walk_flags, read by a start only). */
#define ALARM_WALK_F_LIFE24 0x01u /* "Include panic and 24-hour zones": they beep, not alarm */

typedef struct {
    alarm_cmd_type_t type;
    uint8_t partition;       /* 1-based target partition */
    alarm_arm_level_t level; /* ARM */
    uint8_t arm_flags;       /* ARM: ALARM_ARM_* */
    uint16_t zone;           /* BYPASS / UNBYPASS */
    alarm_panic_t panic;     /* PANIC */
    char code[ALARM_CODE_MAX_LEN + 1]; /* digits; empty when not supplied */
    uint8_t walk_op;         /* WALK_TEST: alarm_walk_op_t */
    uint8_t walk_flags;      /* WALK_TEST start: ALARM_WALK_F_* */
} alarm_cmd_t;

typedef enum {
    ALARM_OK = 0,
    ALARM_ERR_BAD_ARG,
    ALARM_ERR_INVALID_CODE,
    ALARM_ERR_CODE_REQUIRED,
    ALARM_ERR_LOCKED_OUT,
    ALARM_ERR_NOT_AUTHORIZED,
    ALARM_ERR_NOT_READY,
    ALARM_ERR_NOT_BYPASSABLE,
    ALARM_ERR_BAD_STATE,
    ALARM_ERR_CODE_IN_USE,
    ALARM_ERR_DISABLED,
    /* The platform could not do the work behind this request (for example code hashing was
     * not available), so the answer is unknown rather than "no". Append only: these numbers
     * are stored in the event log as ALARM_EV_ARM_FAILED's arg. */
    ALARM_ERR_UNAVAILABLE,
    /* Arming is held by the platform while something the panel protects with can't work: a
     * sensor radio updating its own firmware (docs/features/36 network.md §14). */
    ALARM_ERR_HELD,
} alarm_result_t;

typedef struct {
    uint8_t user;            /* authenticated user (0 = none) */
    uint16_t not_ready_zone; /* first zone blocking an arm request */
    /* ALARM_CMD_CODE_CHECK: the user may disarm the command's partition - the installer and a
     * guest only while it is armed by them (CORE-USR-002/003), so never while it is disarmed.
     * Appended (positional initialisers of the first two stay valid); false for other commands. */
    bool may_disarm;
} alarm_cmd_result_t;

/* Why the panel started (PWR-007). Stored as the arg of ALARM_EV_SYSTEM_START in the event log:
 * append new values only, never renumber. */
typedef enum {
    ALARM_START_UNKNOWN = 0,  /* not known (also what older firmware recorded) */
    ALARM_START_POWER_ON = 1,
    ALARM_START_SOFTWARE = 2, /* a restart the firmware asked for: update, restore, esp_restart */
    ALARM_START_PANIC = 3,    /* a crash, an abort or a program exit */
    ALARM_START_WATCHDOG = 4, /* task, interrupt or RTC watchdog */
    ALARM_START_BROWNOUT = 5,
    ALARM_START_EXTERNAL = 6, /* reset pin */
    ALARM_START_OTHER = 7,
} alarm_start_reason_t;

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */
/* The event log stores these numbers: append new types only, never reorder. */
typedef enum {
    ALARM_EV_SYSTEM_START = 0,
    ALARM_EV_ARMED,            /* arg = level, arg2 = arm flags + ALARM_ARMED_BYPASSED() */
    ALARM_EV_ARM_FAILED,       /* arg = alarm_result_t */
    ALARM_EV_DISARMED,
    ALARM_EV_AUTO_STAY,        /* away arming converted to stay (nobody left) */
    ALARM_EV_EXIT_RESTART,
    ALARM_EV_ENTRY_DELAY,      /* entry time started by zone */
    ALARM_EV_ALARM,            /* arg = alarm_cause_t */
    ALARM_EV_ALARM_RESTORE,    /* zone in alarm returned to normal */
    ALARM_EV_EXIT_ERROR,
    ALARM_EV_RECENT_CLOSING,
    ALARM_EV_ALARM_ABORTED,    /* disarmed inside abort window: nothing was reported */
    ALARM_EV_ALARM_CANCELED,   /* disarmed inside cancel window after a report */
    ALARM_EV_ALARM_SILENCED,
    ALARM_EV_BELL_TIMEOUT,
    ALARM_EV_SWINGER_SHUTDOWN,
    ALARM_EV_MEMORY_CLEARED,
    ALARM_EV_BYPASS,
    ALARM_EV_UNBYPASS,
    ALARM_EV_TROUBLE,          /* arg = zone/system trouble kind, zone or device set */
    ALARM_EV_TROUBLE_RESTORE,
    ALARM_EV_TROUBLES_ACKED,
    ALARM_EV_DURESS,
    ALARM_EV_INVALID_CODE,
    ALARM_EV_LOCKOUT,
    ALARM_EV_CHIME_ON,
    ALARM_EV_CHIME_OFF,
    ALARM_EV_WALK_TEST_START,
    ALARM_EV_WALK_TEST_END,
    ALARM_EV_WALK_TEST_ZONE,
    ALARM_EV_MONITOR_FAULT,
    ALARM_EV_MONITOR_RESTORE,
    ALARM_EV_USER_CHANGED,     /* code added or changed; user = actor, arg = target */
    ALARM_EV_USER_DELETED,
    ALARM_EV_CONFIG_CHANGED,
    ALARM_EV_USER_UPDATED,     /* name/authority/partitions changed, code kept; arg = target */
    ALARM_EV_PROGRAM_ENTER,    /* installer mode; arg 1 = power-up recovery without a code */
    ALARM_EV_PROGRAM_EXIT,     /* arg 1 = ended by the idle timeout */
    ALARM_EV_FACTORY_RESET,    /* platform, just before erasing; arg bit 0 = event log too */
    ALARM_EV_FIRMWARE_UPDATE,  /* platform; arg = alarm_fw_stage_t */
    /* The Z-Wave network (docs/features/36 network.md §15), recorded by the platform. arg is the
     * device's node ID (0 when it is over 255: Long Range); zone the first zone it fed (0: none). */
    ALARM_EV_ZWAVE_ADDED,           /* a device joined, encrypted; arg2 = alarm_zw_join_t */
    ALARM_EV_ZWAVE_ADDED_INSECURE,  /* a device joined without encryption; arg2 = alarm_zw_join_t */
    ALARM_EV_ZWAVE_REMOVED,         /* the installer removed a device; arg2 = alarm_zw_removed_t */
    ALARM_EV_ZWAVE_LEFT,            /* a device left by itself; arg2 = alarm_zw_left_t */
    ALARM_EV_ZWAVE_NETWORK_RESET,   /* the network was reset: every device must be added again */
    ALARM_EV_ZWAVE_RADIO_RESTARTED, /* the radio restarted by itself; arg = the wake-up reason of
                                     * the last restart, arg2 = how many restarts it stands for
                                     * (ZW-041: the first logged at once, then one an hour at
                                     * most; 255 means 255 or more; 0 in records before the
                                     * count: one) */
    ALARM_EV_ZWAVE_SOFTWARE,        /* a device's software update (ZW-033); arg2 = alarm_zw_sw_t */
    ALARM_EV_ZWAVE_GARAGE,          /* a garage door by its Z-Wave relay (ZW-035); zone its tilt
                                     * sensor's; arg2 = alarm_zw_garage_t */
    ALARM_EV_ZWAVE_JAMMING,         /* a device reported radio jamming (Home Security "RF jamming
                                     * detected", network.md §9: its view, not the network's) */
    ALARM_EV_BACKUP_MADE,           /* platform: the installer downloaded a backup file (the
                                     * secrets left the panel, encrypted; docs/features/14) */
    ALARM_EV_BACKUP_RESTORED,       /* platform: a backup file was restored; the panel restarts */
    ALARM_EV_WALK_TEST_EXTEND,      /* the walk test's limit starts again (WALK-004); user, source */
    ALARM_EV_WALK_TEST_WARNING,     /* the walk test ends soon (WALK-004); arg = minutes left */
    ALARM_EV_CLOCK_SET,             /* platform: the clock was set by hand (RTC-014); user = who
                                     * set it, 0 the console (the system); arg 0 */
    /* The Zigbee network (docs/features/20 devices.md §11, ZB-019), recorded by the platform.
     * A device's events: arg its registry id (0 when it is over 255), zone the first zone it fed
     * (0: none); user the installer who asked, 0 the panel. */
    ALARM_EV_ZIGBEE_ADDED,          /* a device joined; arg2 = alarm_zb_join_t */
    ALARM_EV_ZIGBEE_REMOVED,        /* the installer removed it: asked to leave, it left */
    ALARM_EV_ZIGBEE_FORGOTTEN,      /* the installer removed it without its answer */
    ALARM_EV_ZIGBEE_LEFT,           /* it left by itself */
    ALARM_EV_ZIGBEE_REPLACED,       /* another device took over its zones; arg the new one's id */
    ALARM_EV_ZIGBEE_JOIN_REFUSED,   /* a stranger was turned away (zone 0, arg 0); arg2 =
                                     * alarm_zb_refused_t; at most once an hour per address */
    ALARM_EV_ZIGBEE_CHANNEL,        /* the network moved; arg2 = the channel (11..26) */
    ALARM_EV_ZIGBEE_NETWORK_RESET,  /* the network was reset: every device must be added again */
    ALARM_EV_ZIGBEE_RADIO_RESTARTED, /* arg2 = alarm_zb_restart_t (zone 0, arg 0) */
    ALARM_EV_AUTOMATION_FAILED,     /* platform: an automation's Lock or Close, or its arming,
                                     * didn't happen (docs/features/41 AUT-009, CORE-PROG-012);
                                     * zone = the triggering zone or 0; arg = alarm_auto_fail_t;
                                     * arg2 = the dev_action_t (ALARM_AUTO_ACT_*, 0 for arming);
                                     * src = {ALARM_SRC_AUTOMATION, the automation's uid} */
    ALARM_EV_ARMING_PUT_OFF,        /* someone put an automation's arming off (docs/features/41
                                     * AUT-013, D26, D30; CORE-USR-036): user = whose code;
                                     * src = {ALARM_SRC_AUTOMATION, uid}; partition; zone 0;
                                     * arg = alarm_put_off_t; arg2 = minutes from epoch_ms to the
                                     * new arm time (ALARM_PUT_OFF_HOUR; 0 = not known or none) */
    ALARM_EV_COUNT
} alarm_event_type_t;

/* How a Z-Wave device joined (ALARM_EV_ZWAVE_ADDED / _ADDED_INSECURE arg2, network.md §4.1).
 * Stored in the event log: append only. */
typedef enum {
    ALARM_ZW_JOIN_NOT_SECURE = 0, /* it has no security */
    ALARM_ZW_JOIN_S2_UNAUTH,      /* Security 2 without a PIN */
    ALARM_ZW_JOIN_S2_AUTH,        /* Security 2 with the PIN on its label */
    ALARM_ZW_JOIN_S2_ACCESS,      /* Security 2 Access Control */
    ALARM_ZW_JOIN_S0,             /* the older S0 */
    ALARM_ZW_JOIN_FAILED,         /* its key exchange failed: not encrypted */
    ALARM_ZW_JOIN_DOWNGRADED,     /* it supports more than it got: not encrypted */
    ALARM_ZW_JOIN_COUNT
} alarm_zw_join_t;

/* How a Z-Wave device was removed (ALARM_EV_ZWAVE_REMOVED arg2). Append only. */
typedef enum {
    ALARM_ZW_REMOVED_EXCLUDED = 0, /* "Remove a device": it was reset through the radio */
    ALARM_ZW_REMOVED_FAILED,       /* "Remove it: it's broken or gone" */
} alarm_zw_removed_t;

/* Why a Z-Wave device left (ALARM_EV_ZWAVE_LEFT arg2). Append only. */
typedef enum {
    ALARM_ZW_LEFT_RESET_LOCALLY = 0, /* it was reset with its own button */
    ALARM_ZW_LEFT_MISSING,           /* the radio no longer lists it */
} alarm_zw_left_t;

/* A Z-Wave device's software update (ALARM_EV_ZWAVE_SOFTWARE arg2). Append only. */
typedef enum {
    ALARM_ZW_SW_STARTED = 0, /* the installer started it */
    ALARM_ZW_SW_UPDATED,     /* the device took its new software */
    ALARM_ZW_SW_FAILED,      /* it didn't (it keeps its old software, as a rule) */
    /* The Z-Wave radio's own firmware (docs/features/36 network.md §14; arg 0, zone 0): */
    ALARM_ZW_SW_RADIO_STARTED,
    ALARM_ZW_SW_RADIO_UPDATED,
    ALARM_ZW_SW_RADIO_FAILED,
} alarm_zw_sw_t;

/* A garage door moved through its Z-Wave relay (ALARM_EV_ZWAVE_GARAGE arg2). Append only. */
typedef enum {
    ALARM_ZW_GARAGE_OPEN_ASKED = 0, /* someone asked it to open (user: who) */
    ALARM_ZW_GARAGE_CLOSE_ASKED,
    ALARM_ZW_GARAGE_OPENED,         /* its tilt sensor says it got there */
    ALARM_ZW_GARAGE_CLOSED,
    ALARM_ZW_GARAGE_FAILED,         /* it didn't get there, or its relay didn't let go */
} alarm_zw_garage_t;

/* How a Zigbee device joined (ALARM_EV_ZIGBEE_ADDED arg2, docs/features/20 devices.md §11): the
 * numbers of zone_hub's zsrc_zb_join_t, which the registry stores. Stored in the event log:
 * append only, in step with zsrc_zb_join_t. */
typedef enum {
    ALARM_ZB_JOIN_UNKNOWN = 0,   /* joined before the panel recorded it, or found again */
    ALARM_ZB_JOIN_INSTALL_CODE,  /* its link key came from the install code on its label */
    ALARM_ZB_JOIN_STANDARD_KEY,  /* the Zigbee 3.0 well-known link key */
    ALARM_ZB_JOIN_COUNT
} alarm_zb_join_t;

/* Why the panel turned a Zigbee device away (ALARM_EV_ZIGBEE_JOIN_REFUSED arg2): the numbers of
 * zsrc_zb_report_t's u.refused.why. Append only. */
typedef enum {
    ALARM_ZB_REFUSED_NOT_SAID = 0, /* the radio didn't say why */
    ALARM_ZB_REFUSED_NO_CODE,      /* no install code matched while codes are required */
    ALARM_ZB_REFUSED_REJOIN,       /* a device the panel doesn't know tried to rejoin */
    ALARM_ZB_REFUSED_COUNT
} alarm_zb_refused_t;

/* Why the Zigbee radio restarted (ALARM_EV_ZIGBEE_RADIO_RESTARTED arg2). Append only. */
typedef enum {
    ALARM_ZB_RESTART_ASKED = 0, /* the installer restarted it */
    ALARM_ZB_RESTART_BY_ITSELF, /* it restarted by itself; the panel set it up again */
    ALARM_ZB_RESTART_PAUSED,    /* the restart guard paused it after too many restarts */
    ALARM_ZB_RESTART_COUNT
} alarm_zb_restart_t;

/* Why an automation's action didn't happen (ALARM_EV_AUTOMATION_FAILED arg). Stored in the event
 * log and named in the JSON (alarm_auto_fail_name): append only. 0 is no reason (refused). */
typedef enum {
    ALARM_AUTO_FAIL_UNREACHABLE = 1, /* the device couldn't be reached */
    ALARM_AUTO_FAIL_NO_ANSWER,       /* it didn't answer in time */
    ALARM_AUTO_FAIL_REFUSED,         /* it said no */
    ALARM_AUTO_FAIL_MISSING,         /* it isn't on the panel any more */
    ALARM_AUTO_FAIL_BUSY,            /* it was busy */
    ALARM_AUTO_FAIL_NOT_ALLOWED,     /* the action isn't allowed for an automation */
    ALARM_AUTO_FAIL_NO_WARNING,      /* arming: no time to warn first */
    ALARM_AUTO_FAIL_COUNT
} alarm_auto_fail_t;

/* The action of ALARM_EV_AUTOMATION_FAILED (arg2): home_control's dev_action_t numbers, which
 * alarm_core can't include (host/ui_preview/ui_words_test.c checks that they agree). */
#define ALARM_AUTO_ACT_ARM   0u  /* arming (ACT_NONE) */
#define ALARM_AUTO_ACT_LOCK  4u  /* ACT_LOCK */
#define ALARM_AUTO_ACT_CLOSE 7u  /* ACT_CLOSE */
#define ALARM_AUTO_ACT_LIMIT 13u /* arg2 below this; at most dev_action_t's ACT_COUNT */

/* ALARM_EV_ARMING_PUT_OFF's arg: what the person chose (docs/features/41 D26). HOUR and NIGHT
 * are automation's auto_put_off_choice_t numbers; DAY is NIGHT's "Not today" (an arm time from
 * 6 AM to 6 PM: no arming before midnight, AUT-013). Stored in the event log and named in the
 * JSON (alarm_put_off_name): append only. */
typedef enum {
    ALARM_PUT_OFF_HOUR = 1,  /* an hour later */
    ALARM_PUT_OFF_NIGHT = 2, /* not tonight (no arming before 6:00 AM) */
    ALARM_PUT_OFF_DAY = 3,   /* not today (no arming before midnight) */
    ALARM_PUT_OFF_COUNT
} alarm_put_off_t;

#define ALARM_ZB_CHANNEL_MIN 11u /* ALARM_EV_ZIGBEE_CHANNEL's arg2: an IEEE 802.15.4 channel */
#define ALARM_ZB_CHANNEL_MAX 26u

/* ALARM_EV_FIRMWARE_UPDATE stages (event arg). Append only. */
typedef enum {
    ALARM_FW_INSTALLING = 0,  /* a verified image is about to be started (the panel restarts) */
    ALARM_FW_UPDATED,         /* the new image passed its self-test and is kept */
    ALARM_FW_ROLLED_BACK,     /* the new image failed; the previous one runs again */
    ALARM_FW_CP_UPDATED,      /* the Wi-Fi co-processor was updated */
    ALARM_FW_CP_FAILED,       /* the Wi-Fi co-processor update failed */
    ALARM_FW_SERVER_CHANGED,  /* the installer chose another update server (OTA-023) */
    ALARM_FW_STOPPED,         /* someone stopped an update before the restart (OTA-027) */
    ALARM_FW_ROLLING_BACK,    /* the self-test failed: restarting with the previous version (OTA-031) */
    ALARM_FW_RESTART_PUT_OFF, /* "installing" was logged, but the restart was put off (OTA-028) */
} alarm_fw_stage_t;

/* How a zone was first marked in a walk test (ALARM_EV_WALK_TEST_ZONE arg, and the kind bits of
 * ALARM_WALK_ZS_*). Stored in the event log: append only. */
typedef enum {
    ALARM_WALK_MARK_FAULT = 0,   /* the zone faulted */
    ALARM_WALK_MARK_COVER,       /* its cover came off (tamper) */
    ALARM_WALK_MARK_BUTTON,      /* a button, keyfob or keyswitch press */
    ALARM_WALK_MARK_SENSOR_TEST, /* the sensor reported its own test (ALARM_ZEV_TEST) */
} alarm_walk_mark_t;

/* How the last walk test ended (alarm_partition_status_t.walk_end_how). Append only. */
typedef enum {
    ALARM_WALK_END_NONE = 0,   /* one is running, or none since the restart */
    ALARM_WALK_END_COMMAND,    /* End, or the keypad's code + TEST */
    ALARM_WALK_END_DISARM,     /* code + OFF, or a keyfob's Disarm (WALK-025) */
    ALARM_WALK_END_TIME_LIMIT, /* walk_test_timeout_s ran out */
} alarm_walk_end_t;

/* Event flags. */
#define ALARM_EVF_REPORT 0x01u /* meant for remote reporting (MQTT / server) */
#define ALARM_EVF_SILENT 0x02u /* never annunciate locally (duress, silent panic) */
#define ALARM_EVF_SYSTEM 0x04u /* system-wide (partition may be 0) */

/* Event arg2 for ALARM_EV_TROUBLE*: which trouble family "arg" belongs to. */
#define ALARM_TRBL_FAMILY_ZONE   0u
#define ALARM_TRBL_FAMILY_SYSTEM 1u
#define ALARM_TRBL_FAMILY_DEVICE 2u

typedef struct {
    uint32_t seq;        /* engine-assigned, strictly increasing */
    alarm_ms_t mono_ms;  /* monotonic time the event happened */
    int64_t epoch_ms;    /* wall clock from ports.get_epoch_ms, 0 = unknown */
    alarm_event_type_t type;
    uint8_t partition;   /* 0 = system */
    uint16_t zone;       /* 0 = none; device number for device troubles */
    uint8_t user;        /* 0 = none */
    alarm_source_t source;
    uint8_t arg;         /* type specific (cause, level, trouble kind, result) */
    uint8_t arg2;        /* type specific (arm flags, trouble family) */
    uint8_t flags;       /* ALARM_EVF_* */
} alarm_event_t;

#ifdef __cplusplus
}
#endif

#endif /* ALARM_TYPES_H */
