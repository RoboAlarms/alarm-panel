/* automation - the panel's built-in automations (docs/features/41): one trigger and one or two
 * actions, run by the panel. This header is the model shared by the codec, the validator, the
 * observer (auto_watch.h), the clock (auto_clock.h) and the runner (auto_run.h).
 *
 * Portable C11: no heap, no globals, no RTOS; time comes in as milliseconds. The enums below are
 * stored in NVS (docs/features/41 DESIGN.md "Stored format"): append new values, never renumber.
 */
#ifndef AUTOMATION_AUTOMATION_H
#define AUTOMATION_AUTOMATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "home_control/device_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTO_MAX      16u  /* automations stored (AUT-001) */
#define AUTO_ACTIONS   2u  /* actions per automation (AUT-001) */
#define AUTO_KEEP_MAX 64u  /* ignorable unknown TLV bytes kept per record and written back (AUT-008) */
/* The newest device source a record may name (dsrc_kind_t has no count): raise it with the enum, or
 * a record naming the new source loads as NEWER and a save of one is refused. */
#define AUTO_SOURCE_LAST ((uint8_t)DSRC_ZWAVE)
/* The wired outputs an automation may switch (BL-224): output n is the panel's wired channel n
 * (out_wired.h OW_CHANNELS) while its function is "For automations". */
#define AUTO_OUTPUTS 4u

/* The largest record auto_encode writes (AUT-008, DESIGN.md "Stored format"): compat, every
 * trigger field (uid 4, flags, made_by, kind, partitions, levels 3 each; zone, minute 4; days 3;
 * window 4 + 4; trouble 3 = 41), two actions with every field (kind, source, dev_kind,
 * dev_action 3 each; value 4; mode 2+23, ref 2+95, name 2+47; off_after 4; confirmed, arm
 * partition, level, warning, output, option 3 each = 209 each), the ignorable fields this
 * firmware knows (bypass 3, changed_at 6, saved_by 3 = 12) and AUTO_KEEP_MAX ignorable bytes it
 * doesn't. */
#define AUTO_RECORD_MAX (1u + 41u + 2u * 209u + 12u + AUTO_KEEP_MAX)

/* What starts an automation (AUT-004, AUT-005). Stored: append only. */
typedef enum {
    AUTO_TRIG_NONE = 0,
    AUTO_TRIG_ARMED = 1,        /* a partition became armed (exit time over), once per arming */
    AUTO_TRIG_DISARMED = 2,     /* a real disarm */
    AUTO_TRIG_ALARM = 3,        /* an audible burglary, tamper, 24-hour or police panic alarm */
    AUTO_TRIG_FIRE = 4,         /* a fire, fire panic or CO alarm */
    AUTO_TRIG_ZONE_OPENED = 5,  /* a usable zone faulted */
    AUTO_TRIG_ZONE_CLOSED = 6,  /* a door, window or garage door zone restored and stayed closed */
    AUTO_TRIG_TIME = 7,         /* a local time of day on chosen weekdays */
    AUTO_TRIG_TROUBLE = 8,      /* a trouble started (BL-224): auto_trigger_t.trouble says which */
} auto_trig_t;
#define AUTO_TRIG_COUNT 9u

/* Which trouble starts an AUTO_TRIG_TROUBLE automation (auto_trigger_t.trouble), and the cause
 * of a TROUBLE occurrence (auto_occ_t.cause). Stored: append only. */
typedef enum {
    AUTO_TROUBLE_ANY = 0,      /* any of them (a trigger only) */
    AUTO_TROUBLE_AC = 1,       /* the power went off (ALARM_TRBL_AC_LOSS) */
    AUTO_TROUBLE_BATTERY = 2,  /* the panel's battery is low (ALARM_TRBL_LOW_BATTERY) */
    AUTO_TROUBLE_SENSOR = 3,   /* a sensor stopped reporting, its battery is low or its loop is in
                                  trouble; or a keypad or device stopped reporting */
    AUTO_TROUBLE_SIREN = 4,    /* a siren problem (ALARM_TRBL_BELL) */
    AUTO_TROUBLE_OTHER = 5,    /* any other system trouble but the clock (a cause only) */
} auto_trouble_t;
#define AUTO_TROUBLE_LAST AUTO_TROUBLE_SIREN /* the last one a trigger may name */

/* What an action does. Stored: append only. */
typedef enum {
    AUTO_ACT_NONE = 0,
    AUTO_ACT_DEVICE = 1,  /* a device action through the registry (hc_act_auto) */
    AUTO_ACT_ARM = 2,     /* arm a partition (phase C, AUT-011) */
    AUTO_ACT_OUTPUT = 3,  /* a wired output "For automations" on or off (BL-224, docs/features/16
                             OUT-005): output, dev_action ACT_ON or ACT_OFF, off_after_min */
    AUTO_ACT_DISARM = 4,  /* disarm a partition (phase C, AUT-011, D23): less secure (AUT-015) */
    AUTO_ACT_REMIND = 5,  /* BL-224, after a time and alone: if the partition is disarmed then, a
                             chime and a steady note on Home until it is armed or 6:00 AM; option
                             1 also sends a notification */
    AUTO_ACT_CHECK_ARMED = 6, /* BL-224, after a time and alone: if the partition is still armed
                                 then, a notification ("Home is still armed at 4:00 PM") */
    AUTO_ACT_CHIME = 7,   /* BL-224: the partition's door chime on (option 1) or off (option 0),
                             as the person who saved it */
    AUTO_ACT_NOTIFY = 8,  /* BL-224: a notification to the phone services, words from the trigger */
} auto_act_kind_t;

/* How a stored record loaded (AUT-008). */
typedef enum {
    AUTO_REC_OK = 0,
    AUTO_REC_NEWER,   /* made by newer software (a higher compat, or an unknown critical field or
                         value): shown, read-only, never run */
    AUTO_REC_BROKEN,  /* decodes but holds something forbidden or impossible: shown, never run */
    AUTO_REC_BAD,     /* can't be decoded at all: kept as stored, never overwritten */
} auto_rec_t;

/* Weekday bits for auto_trigger_t.days: bit 0 Monday .. bit 6 Sunday; 0 means every day. */
#define AUTO_DAY_BIT(weekday_mon1) ((uint8_t)(1u << ((unsigned)(weekday_mon1) - 1u)))
#define AUTO_DAYS_ALL 0x7Fu

#define AUTO_MINUTES_PER_DAY 1440u

/* The trigger. Zero is "any" or "none" for every field. */
typedef struct {
    uint8_t kind;        /* auto_trig_t */
    uint8_t partitions;  /* bit p-1 for partition p; 0 = any enabled partition */
    uint8_t levels;      /* ALARM_LEVEL_BIT(level) for AUTO_TRIG_ARMED; 0 = any level */
    uint8_t days;        /* AUTO_TRIG_TIME: AUTO_DAY_BIT per weekday; 0 = every day */
    uint16_t zone;       /* zone triggers: 1..ALARM_MAX_ZONES */
    uint16_t minute;     /* AUTO_TRIG_TIME: minute of the local day, 0..1439 */
    uint16_t win_from;   /* "Only between" (not for TIME): from minute + 1; 0 = any time */
    uint16_t win_to;     /* to minute + 1; 0 = any time; wraps past midnight; from == to refused */
    uint8_t trouble;     /* AUTO_TRIG_TROUBLE: auto_trouble_t ANY..AUTO_TROUBLE_LAST; 0 otherwise */
} auto_trigger_t;

/* An action. Strings are NUL-terminated; ref is the device's address in its source. */
typedef struct {
    uint8_t kind;           /* auto_act_kind_t */
    uint8_t source;         /* dsrc_kind_t the device came from */
    uint8_t dev_kind;       /* dev_kind_t the action was made for (re-checked before each run) */
    uint8_t dev_action;     /* dev_action_t; only hc_auto_allowed() pairs (AUT-002) */
    uint8_t confirmed;      /* 1: the person confirmed a scene, a switch's Turn on or Toggle (D8,
                               D9), or a less secure action (an unlock, an open, a stop, a disarm)
                               after a door, window, motion or alarm trigger (D24, AUT-016) */
    uint8_t partition;      /* AUTO_ACT_ARM, AUTO_ACT_DISARM: the partition to arm or disarm,
                               1..ALARM_MAX_PARTITIONS (stored as the "arm partition" field) */
    uint8_t arm_level;      /* AUTO_ACT_ARM: alarm_arm_level_t AWAY, STAY or NIGHT */
    uint8_t warn_min;       /* AUTO_ACT_ARM: minutes of warning, 1..15 (AUT-012) */
    uint8_t bypass_open;    /* AUTO_ACT_ARM: 1 = "Bypass open zones" (D29, AUT-014): saved only
                               while "Allow less secure automations" is on; a stored one bypasses
                               only while the option is on now (read at arm time), else it arms
                               without bypassing. Stored as an ignorable field: an older firmware
                               arms without bypassing */
    int16_t value;          /* the action's value; -1 = none (a light's Turn on sends no brightness) */
    uint16_t off_after_min; /* Turn on only (a device or an output): Off this many minutes later;
                               0 = leave it on */
    uint8_t output;         /* AUTO_ACT_OUTPUT: the wired output, 1..AUTO_OUTPUTS; 0 otherwise */
    uint8_t option;         /* AUTO_ACT_REMIND: 1 also a notification; AUTO_ACT_CHIME: 1 on, 0 off;
                               0 for every other action */
    char mode[DEV_MODE_LEN];  /* a mode word (a thermostat's "off") */
    char ref[DEV_REF_LEN];    /* the device's ref, 1..DEV_REF_LEN-1 bytes */
    char name[DEV_NAME_LEN];  /* the device's name when picked, shown if it goes missing */
} auto_action_t;

/* One automation, as stored and as the runner sees it. */
typedef struct {
    uint16_t uid;       /* stable id, never reused (the event source id) */
    uint8_t slot;       /* 1..AUTO_MAX: its storage key */
    uint8_t made_by;    /* the user who saved it last (every save, D23): an alarm action acts as
                           this person (AUT-011), and a less secure one runs only while this
                           slot still holds a system master or installer (AUT-015). An Arm the
                           installer saves acts as the system master (BL-224) */
    uint8_t saved_by;   /* who saved it when that isn't made_by: the installer, for an Arm set up
                           as the system master ("Set up by Installer · uses Alex's permission");
                           0 = made_by saved it. Ignorable */
    uint8_t n;         /* actions in use, 1..AUTO_ACTIONS */
    uint8_t rec;        /* auto_rec_t: how it loaded */
    bool off;           /* turned off by a person */
    auto_trigger_t when;
    auto_action_t then[AUTO_ACTIONS];
    uint32_t changed_at;  /* epoch seconds when its sentence last changed (D30: "changed since");
                             0 = not known. Set by the caller (auto_store_save_at). Ignorable */
    uint8_t keep_len;             /* ignorable fields this firmware doesn't know, kept as stored */
    uint8_t keep[AUTO_KEEP_MAX];
} auto_rule_t;

/* What the validator and the runner need to know of a zone (from alarm_config_t). */
typedef struct {
    uint8_t type;       /* alarm_zone_type_t; 0 = not used */
    uint8_t partition;  /* 1-based */
    uint8_t dev_class;  /* alarm_device_class_t */
} auto_zone_t;

/* The panel as the validator and the runner see it. */
typedef struct {
    uint8_t partitions_enabled;  /* bit p-1 for each enabled partition */
    bool arm_supported;          /* this firmware runs arming and disarming automations (phase C) */
    const auto_zone_t *zones;    /* [ALARM_MAX_ZONES], index zone - 1; NULL = none known */
    bool less_secure;            /* opt.auto_less_secure: "Allow less secure automations" (AUT-015) */
    bool by_installer;           /* a save by the installer, who never disarms (BL-187, USR-001)
                                    and whose Arm acts as the system master (BL-224): set by the
                                    save paths only, from the saver's live authority */
    uint8_t outputs;             /* bit n-1: wired output n is "For automations" (BL-224) */
} auto_env_t;

/* Why a rule can't be saved (or run). */
typedef enum {
    AUTO_OK = 0,
    AUTO_ERR_INCOMPLETE,     /* no trigger, no action, or a field the trigger needs is missing */
    AUTO_ERR_NOT_ALLOWED,    /* an action no automation may do, whatever the option (AUT-002),
                                a Disarm after an alarm or fire trigger (D22), and an Arm or
                                Disarm saved by the installer (BL-187) */
    AUTO_ERR_ZONE,           /* the zone can't start an automation (AUT-004) */
    AUTO_ERR_PARTITION,      /* a partition that isn't enabled */
    AUTO_ERR_TIME,           /* a minute or day mask out of range */
    AUTO_ERR_WINDOW,         /* a window with from == to, out of range, or on a TIME trigger */
    AUTO_ERR_ARM_ALONE,      /* arming with a second action */
    AUTO_ERR_MAKER,          /* an alarm or less secure action without the user who saved it */
    AUTO_ERR_SAME_DEVICE,    /* one device, or one alarm action, twice (AUT-007) */
    AUTO_ERR_UNSUPPORTED,    /* something this firmware can't run (arming, disarming in phase A) */
    AUTO_ERR_NEEDS_CONFIRM,  /* a less secure action after a door, window, motion or alarm
                                trigger without its confirmed flag (D24, AUT-016) */
    AUTO_ERR_LESS_SECURE_OFF, /* a less secure action, a disarm or "Bypass open zones" while the
                                 installer option is off (D25, AUT-015, decided 2026-10-05) */
    AUTO_ERR_UNDOES_ARMING,  /* a Disarm of a partition after an armed trigger of that partition
                                (or of any partition): it would undo the arming at once (AUT-016,
                                DESIGN.md "Disarming" 4) */
    AUTO_ERR_OUTPUT,         /* a wired output that isn't "For automations" (BL-224) */
    AUTO_ERR_NEEDS_TIME,     /* Remind or "still armed" after a trigger other than a time of day
                                (BL-224) */
} auto_check_t;

/* May this zone start an automation? for_close: may it start a "closes" trigger (doors, windows
 * and garage doors only). Never smoke, heat, CO, gas, panic, keyfob, keyswitch or silent zones,
 * nor a zone type this firmware doesn't list (AUT-004). */
bool auto_zone_usable(uint8_t zone_type, uint8_t dev_class, bool for_close);

/* A less secure action (AUT-015): a disarm, or a device pair hc_auto_less_secure names. */
bool auto_action_less_secure(const auto_action_t *a);

/* The two confirms, one source for the editors and the validator (DESIGN.md section 3):
 * - auto_action_needs_confirm (D8, D9, AUT-002): a device of this kind with this action is saved
 *   only with the action's confirmed flag, asked once when it is picked - a scene's Run, a
 *   switch's Turn on or Toggle (a switch may front a gate opener). Without it auto_rule_check
 *   answers AUTO_ERR_NOT_ALLOWED.
 * - auto_action_needs_trigger_confirm (D24, AUT-016): this action after this trigger is saved only
 *   with its confirmed flag - a less secure action (auto_action_less_secure: an unlock, an open,
 *   a stop, a disarm) after a door, window, motion or alarm trigger (ZONE_OPENED, ZONE_CLOSED,
 *   ALARM, FIRE), which someone outside may cause; and an unlock, an open or a stop after an
 *   armed trigger (decided 2026-10-05: "Every time Home is armed Away, Front Door Lock will
 *   unlock"). The editor asks again whenever the trigger or that action changes. Without it
 *   auto_rule_check answers AUTO_ERR_NEEDS_CONFIRM. A time of day or a disarmed trigger needs
 *   none, nor a disarm after an armed one (refused: AUTO_ERR_UNDOES_ARMING). */
bool auto_action_needs_confirm(uint8_t dev_kind, uint8_t dev_action);
bool auto_action_needs_trigger_confirm(uint8_t trig_kind, const auto_action_t *a);

/* The validator (AUT-002, AUT-015, AUT-016): every save, from the touchscreen and the browser
 * alike, and the runner's every rule go through it. */
auto_check_t auto_rule_check(const auto_rule_t *r, const auto_env_t *env);

/* Would the rule read as another sentence (D30)? True when its trigger or what an action does
 * differs: every field but uid, slot, made_by, rec, off, the confirms, changed_at and the kept
 * bytes. The caller stamps changed_at when it does. */
bool auto_rule_sentence_changed(const auto_rule_t *was, const auto_rule_t *now);

/* The stored record (AUT-008): encode returns the bytes written (0 if cap is too small); decode
 * returns how the record loaded and fills *out as far as it could. */
size_t auto_encode(const auto_rule_t *r, uint8_t *out, size_t cap);
auto_rec_t auto_decode(const uint8_t *in, size_t len, auto_rule_t *out);

/* NVS entries a record of this many bytes costs (1 + ceil(b/4000) + ceil(b/32), as zbz_panel's). */
size_t auto_nvs_entries(size_t record_bytes);

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_AUTOMATION_H */
