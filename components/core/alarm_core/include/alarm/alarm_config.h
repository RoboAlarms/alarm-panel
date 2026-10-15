/*
 * alarm_config.h - installer/user programmed configuration and the persisted
 * runtime state ("power up in previous state").
 *
 * Both structures are plain data so the platform can store them as blobs
 * (NVS on ESP-IDF). Bump the version constants when the layout changes.
 */
#ifndef ALARM_CONFIG_H
#define ALARM_CONFIG_H

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_CONFIG_VERSION  1
#define ALARM_RUNTIME_VERSION 1

/* Factory default codes. Change them after install. */
#define ALARM_DEFAULT_INSTALLER_CODE "69420"  /* user 01 */
#define ALARM_DEFAULT_MASTER_CODE    "123456" /* user 02 */

/* A spoken name holds at most this many vocabulary entries (D02). Token IDs: 0 = none,
 * 1..ALARM_VOICE_TOKEN_SELECTABLE_MAX may be chosen for a name (recording-script.csv
 * numeric_id); the rest are the panel's own status and walk-test words. */
#define ALARM_ZONE_WORDS                    8u
#define ALARM_VOICE_TOKEN_SELECTABLE_MAX    0x7FFFu

/* Code hash function (same contract as alarm_ports_t.hash_code). */
typedef void (*alarm_hash_fn)(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                              uint8_t out[ALARM_CODE_HASH_LEN]);

/* System-wide options. Defaults follow SIA CP-01. */
typedef struct {
    uint8_t code_min_len;          /* minimum digits for new codes, 4..10 (default 4) */
    uint8_t abort_window_s;        /* burglary report delay 0..45 (default 30) */
    uint16_t cancel_window_s;      /* >= 300 (default 300) */
    uint16_t bell_timeout_s;       /* 0 = sound until disarmed (default 240) */
    bool fire_bell_timeout;        /* false: fire/CO sound until silenced (default) */
    uint8_t swinger_trips;         /* alarms per zone per armed period 1..6 (default 2) */
    uint16_t cross_zone_window_s;  /* default 300 */
    uint16_t fire_verify_window_s; /* default 90 */
    uint8_t smoke_reset_s;         /* smoke power interruption (default 6) */
    uint8_t exit_restart_limit;    /* automatic exit restarts on re-entry (default 1) */
    uint16_t recent_closing_s;     /* default 120 */
    bool auto_stay;                /* away -> stay when nobody left (default true) */
    bool exit_sounds;              /* exit progress beeps (default true) */
    bool quick_arm;                /* keypad arming without a code (default false) */
    bool quick_exit;               /* exit restart without a code in stay/night (default true) */
    bool remote_arm_without_code;  /* MQTT/HA may arm without a code (default false) */
    bool power_up_previous_state;  /* restore arming after power loss (default true) */
    uint16_t power_up_settle_s;    /* zone alarms ignored after boot (default 60) */
    uint8_t lockout_attempts;      /* invalid codes before lockout, 0 = off (default 5) */
    uint16_t lockout_s;            /* lockout duration (default 300) */
    uint16_t walk_test_timeout_s;  /* default 3600 */
    uint32_t trouble_beep_mask;    /* system troubles that beep (bit = alarm_sys_trouble_t) */
    alarm_panic_t panic_keys[3];   /* keypad panic keys A, B, C */
    bool relaxed_limits;           /* allow values outside CP-01 ranges (dev / special cases) */
    bool power_up_program;         /* installer mode without a code for ALARM_RECOVERY_WINDOW_S
                                      after power-up (default true) */
    bool chime_needs_code;         /* chime on/off asks for a code (default true);
                                      off = one touch */
    bool auto_less_secure;         /* automations may disarm, unlock, open and stop (docs/features/41
                                      AUT-015, "Allow less secure automations"; installer only,
                                      default true) */
    uint32_t voice_revision_counter; /* the last voice binding revision handed out (codec 0x1E,
                                        CORE-CFG-021): only grows, never wraps, never reused */
} alarm_options_t;

typedef struct {
    bool enabled;
    char name[ALARM_NAME_LEN];
    uint16_t exit_delay_s;   /* 45..255 (default 60) */
    uint16_t entry_delay1_s; /* 30..240 (default 30) */
    uint16_t entry_delay2_s; /* 30..240 (default 45) */
} alarm_partition_config_t;

typedef struct {
    alarm_zone_type_t type;
    uint8_t partition;      /* 1-based */
    uint16_t flags;         /* ALARM_ZF_* */
    uint8_t device_class;   /* alarm_device_class_t */
    uint8_t user;           /* button / keyswitch zones act as this user */
    uint8_t swinger_trips;  /* 0 = system default */
    uint16_t cross_zone;    /* partner zone, 0 = none */
    uint16_t supervision_s; /* max seconds between check-ins, 0 = unsupervised */
    char name[ALARM_NAME_LEN];
    /* The zone's spoken name (docs/features/08 voice-first-build.md D01, D07; CORE-CFG-020):
     * up to ALARM_ZONE_WORDS vocabulary token IDs, each a little-endian u16 in two bytes (read
     * and write them with alarm_zone_word()/alarm_zone_set_word(), never as a uint16_t array);
     * a nonzero prefix, then zeros. All zeros: no spoken name. */
    uint8_t spoken_words[2u * ALARM_ZONE_WORDS];
    uint32_t voice_binding_revision;  /* this zone's identity, 0 = none yet (CORE-CFG-021) */
    uint32_t voice_approved_revision; /* the binding revision approved, 0 = not approved */
    uint32_t voice_pack_revision;     /* the voice pack revision it was approved with */
} alarm_zone_config_t;

typedef struct {
    alarm_authority_t authority; /* ALARM_AUTH_NONE = empty slot */
    uint8_t partitions;          /* bit 0 = partition 1 */
    uint8_t code_hash[ALARM_CODE_HASH_LEN];
    char name[ALARM_NAME_LEN];
} alarm_user_t;

typedef struct {
    bool enabled;
    uint8_t partition;      /* home partition, 0 = all */
    uint16_t supervision_s; /* 0 = unsupervised */
    char name[ALARM_NAME_LEN];
} alarm_device_config_t;

typedef struct {
    uint16_t version;
    alarm_options_t opt;
    uint8_t code_salt[ALARM_CODE_SALT_LEN];
    alarm_partition_config_t partitions[ALARM_MAX_PARTITIONS];
    alarm_zone_config_t zones[ALARM_MAX_ZONES];
    alarm_user_t users[ALARM_MAX_USERS];
    alarm_device_config_t devices[ALARM_MAX_DEVICES];
} alarm_config_t;

/* Persisted runtime state. Exit/entry delays are stored as ARMED. */
typedef struct {
    uint8_t state;    /* alarm_arm_state_t */
    uint8_t level;    /* alarm_arm_level_t */
    uint8_t flags;    /* ALARM_ARM_* */
    uint8_t armed_by; /* user number */
    uint16_t memory;  /* alarm memory kinds */
    bool chime;
    /* Who armed it (CORE-USR-035): the arm source's kind + 1, 0 = not known. It sits in the
     * byte that was this struct's tail padding, so the stored blob keeps its size and version:
     * a state an older firmware wrote reads 0 here (it zeroes the whole struct), and an older
     * firmware ignores the byte. Only the kind is kept: an automation's uid isn't. */
    uint8_t arm_src;
} alarm_partition_runtime_t;

typedef struct {
    uint16_t version;
    alarm_partition_runtime_t part[ALARM_MAX_PARTITIONS];
    uint8_t bypassed[ALARM_ZONE_BITMAP_BYTES];
    uint8_t in_memory[ALARM_ZONE_BITMAP_BYTES];
    uint32_t event_seq; /* last event sequence number handed out */
} alarm_runtime_state_t;

/* Fill with defaults: partition 1 enabled, no zones, no users.
 * Call alarm_config_set_default_users() afterwards for the factory codes. */
void alarm_config_defaults(alarm_config_t *cfg);

/* Default settings for a zone of the given type (chime, device class, name). */
void alarm_zone_config_defaults(alarm_zone_config_t *zone, uint16_t zone_no,
                                alarm_zone_type_t type, uint8_t partition);

/* Install the factory users: 01 installer (ALARM_DEFAULT_INSTALLER_CODE) and
 * 02 system master (ALARM_DEFAULT_MASTER_CODE), both with every partition. */
void alarm_config_set_default_users(alarm_config_t *cfg, alarm_hash_fn hash, void *hash_ctx);

/* Store a user slot (the code hash comes from ports.hash_code). */
void alarm_config_set_user(alarm_config_t *cfg, uint8_t user_no, alarm_authority_t authority,
                           uint8_t partitions, const char *name,
                           const uint8_t hash[ALARM_CODE_HASH_LEN]);

/* Check ranges and consistency. On failure writes a reason into why (if given), in English for
 * the console, logs and MQTT ("zone 3: has an invalid type"). */
alarm_result_t alarm_config_validate(const alarm_config_t *cfg, char *why, size_t why_len);

/* What the check found (CORE-CFG-019): a code and the partition, zone, user, device or panic key
 * it concerns (1-based; a panic key 0-based; the value for the options' ranges), so a screen can
 * say it in its own language (docs/features/37). Codes are never renumbered. */
typedef enum {
    ALARM_CFG_FINE = 0,
    ALARM_CFG_NULL, /* = 1 */
    ALARM_CFG_VERSION,
    ALARM_CFG_CODE_LENGTH,
    ALARM_CFG_ABORT_WINDOW,
    ALARM_CFG_CANCEL_WINDOW,
    ALARM_CFG_SWINGER,
    ALARM_CFG_FIRE_TIMES,
    ALARM_CFG_PANIC_KEY,
    ALARM_CFG_PARTITION_NAME,
    ALARM_CFG_EXIT_DELAY,
    ALARM_CFG_ENTRY_DELAY,
    ALARM_CFG_DELAY_TOO_LONG,
    ALARM_CFG_NO_PARTITION,
    ALARM_CFG_ZONE_TYPE,
    ALARM_CFG_ZONE_NAME,
    ALARM_CFG_ZONE_PARTITION,
    ALARM_CFG_ZONE_CROSS,
    ALARM_CFG_ZONE_USER,
    ALARM_CFG_USER_AUTHORITY,
    ALARM_CFG_USER_NAME,
    ALARM_CFG_USER_INSTALLER,
    ALARM_CFG_USER_MASTER,
    ALARM_CFG_USER_ROLE,
    ALARM_CFG_USER_PARTITIONS,
    ALARM_CFG_USER_SAME_CODE,
    ALARM_CFG_DEVICE_NAME,
    ALARM_CFG_DEVICE_PARTITION,
    ALARM_CFG_ZONE_WORDS, /* a spoken name with a hole or a word that can't be chosen
                             (CORE-CFG-020) */
    ALARM_CFG_PROBLEM_COUNT
} alarm_config_problem_t;

typedef struct {
    alarm_config_problem_t problem; /* ALARM_CFG_FINE when it passes */
    unsigned number;
} alarm_config_why_t;

/* The same check as alarm_config_validate(), answering with a code (why may be NULL). */
alarm_result_t alarm_config_check(const alarm_config_t *cfg, alarm_config_why_t *why);

/* May user 'actor' manage user slot 'target' as it stands now: change its code or name,
 * delete it, give it a browser sign-in? The engine's own rule (docs/features/03): the installer
 * manages everyone, the system master everyone but the installer, a partition master itself
 * and the standard, arm-only, guest and duress users (or empty slots) within its partitions;
 * nobody else manages anyone. The touchscreen and the Panel Manager ask the same question. */
bool alarm_config_user_manages(const alarm_config_t *cfg, uint8_t actor, uint8_t target);

/* Zone type helpers. */
bool alarm_zone_type_valid(int type);
bool alarm_zone_type_is_burglary(alarm_zone_type_t type); /* takes part in arming/readiness */
bool alarm_zone_type_is_fire(alarm_zone_type_t type);
bool alarm_zone_type_is_24h(alarm_zone_type_t type);
/* A walk test started with walk_flags (ALARM_WALK_F_*) covers zones of this type and counts them
 * in its total (docs/features/40 WALK-007); fire and CO stay live and are never counted. */
bool alarm_zone_type_walk_counts(alarm_zone_type_t type, uint8_t walk_flags);
bool alarm_zone_armed_in_level(const alarm_zone_config_t *zone, alarm_arm_level_t level);
bool alarm_zone_bypassable(const alarm_zone_config_t *zone);

/* A configuration change from 'before' to 'after' makes the engine forget the zone's input
 * conditions and troubles (a new, removed, retyped or moved zone): whoever feeds the zone tells
 * it again (docs/features/22 zone_hub_resync). */
bool alarm_zone_config_resets_state(const alarm_zone_config_t *before,
                                    const alarm_zone_config_t *after);

/* ---- A zone's spoken name and its revisions (docs/features/08 voice-first-build.md D01, D07;
 * CORE-CFG-020, CORE-CFG-021) --------------------------------------------------------------- */

/* Word i (0-based) of the zone's spoken name; 0 past the end or for i >= ALARM_ZONE_WORDS. */
uint16_t alarm_zone_word(const alarm_zone_config_t *zone, unsigned i);
/* Set word i (0-based); does nothing for i >= ALARM_ZONE_WORDS. Keeping the prefix shape is the
 * caller's job (alarm_config_check() refuses a hole). */
void alarm_zone_set_word(alarm_zone_config_t *zone, unsigned i, uint16_t token);
/* How many words the spoken name has (the length of its nonzero prefix). */
unsigned alarm_zone_word_count(const alarm_zone_config_t *zone);
/* Is the word list well formed: a prefix of 1..ALARM_VOICE_TOKEN_SELECTABLE_MAX, then zeros?
 * (repeats allowed; all zeros is fine). The 10 s length needs the voice pack: not checked here. */
bool alarm_zone_words_valid(const alarm_zone_config_t *zone);

/* What a voice approval or withdrawal says (CORE-CFG-021). */
typedef enum {
    ALARM_VOICE_OK = 0,
    ALARM_VOICE_BAD_ARG,     /* no such zone, not used, no pack revision, words empty or malformed */
    ALARM_VOICE_STALE,       /* the configuration generation, binding revision or words the caller
                                saw are no longer the stored ones: read again, ask again */
    ALARM_VOICE_UNAVAILABLE, /* the zone has no binding revision (a migrated zone whose words were
                                never written) or the revision counter is exhausted */
    ALARM_VOICE_BUSY,        /* the engine: a partition is not idle (as alarm_engine_set_config) */
} alarm_voice_result_t;

/* Apply the identity rules (D01, D07) to one zone going from 'old' (stored) to 'next' (what a
 * client wrote), both the same slot. Nothing a client writes can grant, replace or restore
 * approval: approval moves only through alarm_config_voice_approve() and
 * alarm_config_voice_withdraw() (the engine's alarm_engine_voice_approve/_withdraw).
 *  - next not used: its words and three revisions become zero (the counter is untouched);
 *  - a new identity - the slot was not used, the display name or the partition changed, or
 *    next's binding revision differs from old's (a client asking for a new identity, as
 *    alarm_config_voice_invalidate_zone() on its copy does; it can only lose approval that
 *    way) - next keeps its words, gets a new binding revision from *counter (0 once the counter
 *    is exhausted) and no approval. A rename back doesn't restore approval;
 *  - the same identity (a type change included): the binding revision stays old's (a migrated
 *    zone without one gets one when its words first change). Then words changed -> no approval;
 *    words the same -> old's approved and pack revisions, whatever next carries (a replayed or
 *    forged approval, or a zero, changes nothing).
 * *counter only grows, and never past UINT32_MAX. */
void alarm_zone_voice_reconcile(const alarm_zone_config_t *old, alarm_zone_config_t *next,
                                uint32_t *counter);

/* The same for a whole configuration: every zone, with next's counter replaced by old's plus
 * what was handed out (a client's counter is never trusted). alarm_engine_set_config() runs it
 * on every write (touchscreen, Panel Manager, MQTT). */
void alarm_config_voice_reconcile(const alarm_config_t *old, alarm_config_t *next);

/* Approve a zone's spoken name, in the stored configuration (the engine's wrapper runs it in
 * the alarm task after checking the configuration generation). The caller says which binding
 * revision and which words (ALARM_ZONE_WORDS tokens, zero-padded) it showed the person, and the
 * revision of the voice pack the trusted voice layer validated them against. BAD_ARG: cfg or
 * words NULL, zone out of range or not used, pack_revision 0, or those words empty or
 * malformed; UNAVAILABLE: the zone has no binding revision or the counter is exhausted; STALE:
 * the binding revision or the words are not the stored ones. On OK, approved = binding and
 * pack = pack_revision; nothing else changes. */
alarm_voice_result_t alarm_config_voice_approve(alarm_config_t *cfg, uint16_t zone_no,
                                                uint32_t expected_binding,
                                                const uint16_t words[ALARM_ZONE_WORDS],
                                                uint32_t pack_revision);

/* Withdraw a zone's approval explicitly (approved and pack revisions 0; words and binding
 * stay). BAD_ARG: cfg NULL, zone out of range or not used; STALE: the binding revision is not
 * the stored one. Withdrawing a name that isn't approved is OK. */
alarm_voice_result_t alarm_config_voice_withdraw(alarm_config_t *cfg, uint16_t zone_no,
                                                 uint32_t expected_binding);

/* A backup restore (docs/features/14 CFG-006): 'restored' is the file's configuration, about to
 * replace 'current' (the running one, captured once its writes are stopped). Its counter
 * becomes the largest of both counters and of every binding and approved revision in both
 * configurations; every approval and pack revision is cleared; every zone in use gets a fresh
 * binding revision from that counter (0 once exhausted; never wrapping); a zone not used loses
 * its words and revisions. An older file can then neither lower the counter, so that a
 * revision handed out before is handed out again, nor bring an approval back. */
void alarm_config_voice_restore(const alarm_config_t *current, alarm_config_t *restored);

/* The zone's sensor changed (docs/features/22): a new binding revision from the counter and no
 * approval; the words stay. ALARM_ERR_BAD_ARG for a zone out of range or not used. When the
 * counter is exhausted the binding revision becomes 0 and the result is still ALARM_OK. The
 * caller stores the result (through alarm_engine_set_config(), which honours it as a new
 * identity) before it commits the new source binding (D07). */
alarm_result_t alarm_config_voice_invalidate_zone(alarm_config_t *cfg, uint16_t zone_no);

/* The same for every zone in 'zones' (bit zone - 1, ALARM_ZONE_BITMAP_BYTES long) that is in
 * use; zones not in use have no approval to lose and are skipped. Nothing else in cfg changes.
 * The platform runs it on a copy of the running configuration that it writes back in the same
 * alarm-task call (alarm_service_voice_invalidate), so no edit can land in between and be
 * reverted by a stale copy. Returns how many zones it invalidated (0 for cfg or zones NULL). */
unsigned alarm_config_voice_invalidate_zones(alarm_config_t *cfg, const uint8_t *zones);

/* Is the zone's spoken name approved for its current identity: nonempty, well formed, and
 * approved == binding != 0? (Whether the pack has its words is the annunciator's question.) */
bool alarm_zone_voice_approved(const alarm_zone_config_t *zone);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_CONFIG_H */
