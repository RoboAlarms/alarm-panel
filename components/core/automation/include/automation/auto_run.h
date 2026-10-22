/* auto_run - the runner (docs/features/41 DESIGN.md sections 2, 3 and 5; AUT-006, AUT-007,
 * AUT-015, HCTL-009, HCTL-011): matches occurrences (auto_watch.h) and crossed minutes (auto_clock.h)
 * against the automations, sends their device actions through a port and keeps each one's
 * outcome as a RAM status.
 *
 * Runs on the automation task. It never blocks: everything that waits (a held send, a close
 * hold, the device gap, a result, "Turn it off after") is a deadline the caller sleeps until
 * (auto_run_next_deadline) and then hands to auto_run_tick. It holds no pointer to what it is
 * given: the rules and the zones are copied in. Ports must not call back into the runner (a
 * device result comes later through auto_run_result). Caller-owned state, no heap, no globals:
 * an auto_run_t is about 24 KB and may live in PSRAM.
 *
 * In short:
 * - Only rules that loaded (AUTO_REC_OK), are turned on and pass auto_rule_check run. The check
 *   is made as if the installer option were on: the option never stops a rule as a whole.
 * - A less secure device action (an unlock, an open, a stop: hc_auto_less_secure) is re-checked
 *   before each send (AUT-015, layer 4): with the option off it ends LESS_SECURE_OFF, and when
 *   the person who saved it no longer holds a system master or installer slot (maker_ok) it ends
 *   MAKER_GONE - never sent, no failed event, and the record is kept as it is; turned back on
 *   (a new env through auto_run_rules) it runs again. Try it now is held to the same. The
 *   option is read again with the maker (maker_ok), so an installer's Off stops the very next
 *   send, before the task has a new env.
 * - Arming and disarming (phase C, auto_arm.c; AUT-011 to AUT-014, DESIGN.md section 3 "Arming"
 *   and "Disarming"), when the env says arm_supported:
 *   - An Arm action's trigger starts a warning of warn_min minutes (the warning port); the arm is
 *     due warn_min minutes later on the monotonic clock, so a clock change never shortens it. A
 *     time-of-day Arm starts its warning warn_min minutes before its minute (a day earlier past
 *     midnight), so it arms on the time. Its minute crossed with no warning running (the clock
 *     set across it, a restart, the rule added or turned on too late, the warning's and the
 *     arm's minute crossed in one reading) is no arm that day: status NO_WARNING and the failed
 *     port (no_warning). A warning that starts late (a small clock step across its minute) still
 *     lasts warn_min minutes: the arm comes that much after the time.
 *   - At arm time the arm port, as the person who saved it; ALARM_ARM_BYPASS_FAULTED only with
 *     "Bypass open zones" and the installer option on now (maker_ok's option, read then). The
 *     answer is the status: DONE, ALREADY (armed already: nothing done), NOT_READY (its zone),
 *     ARM_REFUSED (the alarm_result_t).
 *   - One warning at a time: while an automation warns or is put off, its new triggers start no
 *     other arming. Turning it off, deleting it or changing its trigger or its Arm ends the
 *     warning or the hold at once (the warning port, state NONE).
 *   - Put off (auto_run_put_off) on every warning: an hour (the planned time + 60 minutes; the
 *     warning starts again 5 minutes before) or not tonight / not today (no arming by it before
 *     6:00 AM local, or before midnight for an arm time from 6 AM to 6 PM); a restart forgets it.
 *   - A Disarm runs at its trigger with no warning (the disarm port, as its maker), re-checked
 *     like a less secure device action (the option and maker_ok). ALREADY when disarmed; a
 *     refusal is ARM_REFUSED and raises nothing here.
 *   - AUT-007: 10 s between two automatic arms or disarms of one partition (a later one waits);
 *     an ARMED or DISARMED that an automation's own arm or disarm caused (auto_occ_t.by_auto, or
 *     the runner's own note of what it did) starts device actions only, never an arm or disarm.
 * - Matching: the trigger kind; the partition mask (0 = any; a zone trigger uses the zone's
 *   partition); the ARMED level mask; a zone trigger's zone, re-checked with auto_zone_usable;
 *   the "Only between" window [win_from - 1, win_to - 1) in local minutes, wrapping past
 *   midnight (with a window and no valid clock nothing runs: AUTO_RES_NO_CLOCK).
 * - Repeats: zone triggers 30 s per rule (a repeat inside only restarts a running "Turn it off
 *   after" timer, as a new run does below); ZONE_CLOSED waits 10 s and runs only if the zone didn't open again (the
 *   window is checked at the close; the 30 s cooldown starts when the hold ends); TIME at most
 *   once per local date (auto_time_due).
 * - Sending (AUT-006): an action that was sent (HC_OK) is never sent again. HC_ERR_OFFLINE and
 *   HC_ERR_UNKNOWN on Home Assistant are held and tried every 2 s for up to 30 s (then missing or
 *   unreachable); on any other source they fail at once; HC_ERR_BUSY fails at once; ARG is
 *   "can't run"; NOT_ALLOWED (a canary) is not_allowed and, for a Lock or Close, goes to the
 *   failed port. A ref the registry doesn't list is HC_ERR_UNKNOWN without asking it; a ref
 *   whose live kind differs from the stored one is never sent (can't run, AUT-002 layer 4).
 *   No result within AUTO_RESULT_WAIT_MS (4 min) of the send is no_answer.
 * - A held send for an ARMED or DISARMED occurrence is dropped when that partition's state
 *   changes again: nothing was sent, so "Last run" keeps the previous outcome.
 * - "Turn it off after": the timer starts when the On is accepted (HC_OK). A new run restarts
 *   it, with the action's current length, only while the action still has a "Turn it off after"
 *   and the timer is for the action's current device; an On without a timer sent (HC_OK) to that
 *   device stops it (the newest rule says "stay on"). When it ends the Off is a send of its own
 *   that a new run never overwrites; it waits for the device gap and for the cap (it is never
 *   held_back: an Off is never lost, unless its automation is deleted) and is sent unless a
 *   newer On to the same device was sent (HC_OK) after it fell due, whose own timer then owns
 *   the device. An On that would start a timer while the action's timer still runs or waits for
 *   another device (after an edit) first makes that device's Off due at once and waits until it
 *   has gone, at most 30 s from its first wait; then it ends held_back if the cap is full, else
 *   unreachable.
 * - Loops (AUT-007): 10 s between two automation actions on one device (a later one waits); at
 *   most 12 actions sent per rolling 60 s panel-wide (the rest end held_back). A now_ms earlier
 *   than a time already recorded counts as no time passed: it never lifts a limit.
 * - Without devices (BL-224, auto_panel.c):
 *   - A wired output (the output port): on or off at once, "Turn it off after" as a device's
 *     (its Off is never lost, a new run restarts the timer); 10 s between two actions on one
 *     output (a later one waits), counted in the panel-wide cap. A refusal is UNREACHABLE.
 *   - The door chime (the chime port, as its maker): DONE, ALREADY (it was so), ARM_REFUSED.
 *   - A notification (the notify port): DONE, or NO_SERVICE when no phone service is set up; at
 *     most one a minute from one automation (more are dropped, the status kept).
 *   - Remind to arm, at its time: the partition disarmed (the part_armed port) shows the note
 *     (the remind port, on) and, with its option, a notification; DONE, or ALREADY when armed.
 *     The note ends (the remind port, off) when that partition is armed, at 6:00 AM local (the
 *     next one for a reminder from 6:00 AM on), or when the automation is changed, turned off or
 *     deleted; a restart forgets it.
 *   - "Still armed", at its time: armed, a notification (DONE or NO_SERVICE); disarmed, ALREADY.
 *   - Try it now runs an output, the chime and a notification (marked tried), never a reminder
 *     or "still armed" (AUTO_TRY_ARM).
 */
#ifndef AUTOMATION_AUTO_RUN_H
#define AUTOMATION_AUTO_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "automation/auto_clock.h"
#include "automation/auto_watch.h"
#include "automation/automation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTO_ZONE_COOLDOWN_MS UINT64_C(30000)   /* zone triggers, per rule */
#define AUTO_CLOSE_HOLD_MS    UINT64_C(10000)   /* ZONE_CLOSED: still closed this long later */
#define AUTO_HOLD_RETRY_MS    UINT64_C(2000)    /* a held send is tried again this often */
#define AUTO_HOLD_MAX_MS      UINT64_C(30000)   /* ... for this long */
/* no result by then: no_answer. Past home_control's longest wait for a device: a Z-Wave garage
 * door's whole move at its longest travel time (zw_outputs.h zwo_garage_budget_ms, 232 s). */
#define AUTO_RESULT_WAIT_MS   UINT64_C(240000)
#define AUTO_DEVICE_GAP_MS    UINT64_C(10000)   /* AUT-007: between two actions on one device */
#define AUTO_CAP_ACTIONS      12u               /* AUT-007: at most this many sent ... */
#define AUTO_CAP_WINDOW_MS    UINT64_C(60000)   /* ... per rolling window */
#define AUTO_FAIL_DEDUP_MS    UINT64_C(3600000) /* the failed port: once per automation per hour */
#define AUTO_RUN_FLIGHTS      16u               /* sends awaiting a result */
#define AUTO_RUN_GAPS         16u               /* devices remembered for the gap */

/* What an action's outcome was (the "Last run" status). RAM only. */
typedef enum {
    AUTO_RES_NONE = 0,     /* hasn't run since the panel started */
    AUTO_RES_DONE,
    AUTO_RES_REFUSED,      /* sent; the device or its source said no */
    AUTO_RES_NO_ANSWER,    /* sent; no answer in time, or its connection dropped */
    AUTO_RES_UNREACHABLE,  /* never sent: its source couldn't reach it for 30 s */
    AUTO_RES_MISSING,      /* never sent: the device isn't listed any more */
    AUTO_RES_BUSY,         /* never sent: a person was acting on it */
    AUTO_RES_HELD_BACK,    /* never sent: too many actions ran at once (AUT-007) */
    AUTO_RES_NOT_ALLOWED,  /* never sent: the registry refused it (a canary, HCTL-011) */
    AUTO_RES_CANT_RUN,     /* never sent: the device is now a different kind; pick it again */
    AUTO_RES_NO_CLOCK,     /* didn't run: it has an "Only between" window and the clock isn't set */
    AUTO_RES_LESS_SECURE_OFF, /* never sent: an unlock, open or stop while "Allow less secure
                                 automations" is off (AUT-015) */
    AUTO_RES_MAKER_GONE,   /* never sent: the person who saved it is no longer a system master
                              or installer (AUT-015, D23) */
    /* Arming and disarming (phase C): DONE when it armed or disarmed */
    AUTO_RES_ALREADY,      /* the partition was armed (or disarmed) already: nothing done */
    AUTO_RES_NOT_READY,    /* not armed: a zone was open (auto_status_t.zone; AUT-014) */
    AUTO_RES_NO_WARNING,   /* not armed: its time came with no whole warning (AUT-012) */
    AUTO_RES_ARM_REFUSED,  /* the panel refused the arm or disarm (auto_status_t.alarm_res) */
    AUTO_RES_PUT_OFF,      /* put off by user (choice, until: AUT-013, D20) */
    AUTO_RES_NO_SERVICE,   /* BL-224: a notification with no phone service set up: not sent */
} auto_res_t;

typedef struct {
    uint8_t result;        /* auto_res_t */
    uint8_t action_index;  /* which action (0-based) */
    uint8_t reason;        /* alarm_auto_fail_t for a failure, 0 otherwise */
    uint8_t user;          /* PUT_OFF: who put it off; 0 otherwise */
    uint8_t tried;         /* AUT-006: 1 when this outcome came from Try it now, 0 from a
                              trigger (or "Turn it off after" of either: an Off keeps its On's) */
    int64_t at_epoch_ms;   /* when, while the clock was valid; else 0 */
    /* phase C (zero otherwise) */
    uint16_t zone;         /* NOT_READY: the zone the panel named; 0 = not told */
    uint8_t alarm_res;     /* ARM_REFUSED: the alarm_result_t */
    uint8_t choice;        /* PUT_OFF: auto_put_off_choice_t */
    uint8_t night;         /* PUT_OFF with NIGHT: 1 "not tonight" (until 6:00 AM), 0 "not today"
                              (until midnight) */
    int64_t until_epoch_ms; /* PUT_OFF with HOUR: the new arm time, while the clock is valid */
} auto_status_t;

/* The answer a sent action got (auto_run_result's why when !ok). */
typedef enum {
    AUTO_WHY_REFUSED = 0,  /* "failed", "not_allowed": the device or its source said no */
    AUTO_WHY_NO_ANSWER,    /* "timeout", "offline", dropped by a refresh */
} auto_why_t;

/* Try it now (auto_run_try). */
typedef enum {
    AUTO_TRY_OK = 0,      /* its actions were started; the outcome comes in the status */
    AUTO_TRY_UNKNOWN,     /* no automation with that uid */
    AUTO_TRY_ARM,         /* an arming or disarming automation, a reminder or "still armed":
                             never tried (they mean something only at their trigger) */
    AUTO_TRY_INVALID,     /* it didn't load, or it doesn't pass auto_rule_check */
} auto_try_t;

/* An arming automation's warning, or its hold after a put-off (auto_warning_t.state). */
typedef enum {
    AUTO_WARN_NONE = 0,  /* nothing: the card, the banner and the put-off line go */
    AUTO_WARN_ON,        /* warning now: the banner, Home's card, the chime once a minute */
    AUTO_WARN_HOUR,      /* put off an hour: no card; the warning starts again 5 minutes before
                            arm_at ("Arms Night at 11:30 PM · put off by Pat") */
    AUTO_WARN_NIGHT,     /* put off until 6:00 AM ("Won't arm tonight · put off by Pat") */
    AUTO_WARN_DAY,       /* put off until midnight ("Won't arm today · put off by Pat") */
} auto_warn_state_t;

/* Put off: the person's choice (D26). Also ALARM_EV_ARMING_PUT_OFF's arg (DESIGN.md section 7:
 * 1 an hour, 2 the night or the day). */
typedef enum {
    AUTO_PUT_OFF_HOUR = 1,
    AUTO_PUT_OFF_NIGHT = 2,  /* "Not tonight", or "Not today" for an arm time from 6 AM to 6 PM */
} auto_put_off_choice_t;

/* auto_run_put_off's answer. */
typedef enum {
    AUTO_POFF_OK = 0,
    AUTO_POFF_NO_WARNING,  /* that automation isn't warning now */
    AUTO_POFF_NOT_ALLOWED, /* NIGHT with a code that may not disarm that partition (decided
                              2026-10-05): only the hour */
    AUTO_POFF_NO_CLOCK,    /* NIGHT while the clock isn't set: only the hour */
    AUTO_POFF_BAD_ARG,     /* no user, or an unknown choice */
} auto_put_off_t;

/* BL-224: what a notification is about (auto_notice_t.what); the platform makes its words. */
typedef enum {
    AUTO_NOTICE_EVENT = 0,       /* the trigger ("Front Door opened", "The power went off") */
    AUTO_NOTICE_REMIND = 1,      /* "Home isn't armed yet" */
    AUTO_NOTICE_STILL_ARMED = 2, /* "Home is still armed at 4:00 PM" */
} auto_notice_kind_t;

typedef struct {
    uint16_t uid;
    uint8_t what;       /* auto_notice_kind_t */
    uint8_t trig;       /* EVENT: the trigger's auto_trig_t */
    uint8_t partition;  /* ARMED, DISARMED, ALARM, FIRE; REMIND and STILL_ARMED: theirs */
    uint8_t level;      /* ARMED: alarm_arm_level_t */
    uint8_t cause;      /* ALARM, FIRE: alarm_cause_t; TROUBLE: auto_trouble_t */
    uint8_t tried;      /* 1: from Try it now */
    uint16_t zone;      /* zone triggers; a sensor's trouble; the zone that raised an alarm */
    uint16_t minute;    /* TIME, REMIND, STILL_ARMED: the minute of the day */
} auto_notice_t;

/* BL-224: a reminder's steady note on Home and the Overview (the remind port's mailbox, RAM). */
typedef struct {
    uint16_t uid;
    uint8_t partition;
    uint8_t on;         /* 1: show it; 0: it goes */
    uint16_t minute;    /* the reminder's minute of the day ("Reminder at 11:00 PM") */
} auto_remind_t;

/* Threads: like every auto_run_* call, auto_run_put_off, auto_run_warning and auto_run_status run
 * on the task that owns the runner (the platform's automation task); the UI posts to that task
 * and reads only the copies the warning port made.
 *
 * The arming warning's mailbox (the UI's and the Panel Manager's; RAM only): sent through the
 * warning port at every change of one automation's warning, the whole state each time. */
typedef struct {
    uint8_t partition;
    uint8_t level;            /* alarm_arm_level_t AWAY, STAY or NIGHT */
    uint8_t put_off_by;       /* user who put it off; 0 = not put off */
    int64_t arm_at_epoch_ms;  /* the planned arm time, while the clock is valid; else 0 */
    /* phase C, appended */
    uint16_t uid;             /* the automation */
    uint8_t state;            /* auto_warn_state_t */
    uint8_t warn_min;         /* its warning, minutes */
    uint8_t bypass;           /* 1: open zones will be bypassed (D28): "Bypass open zones" and
                                 the installer option on, as the runner knows it now */
    uint64_t arm_at_ms;       /* the planned arm time, monotonic (ON and HOUR) */
    int32_t until_day;        /* NIGHT, DAY: the hold ends at this local date (day_no) ... */
    uint16_t until_minute;    /* ... and minute (360, or 0 for midnight) */
} auto_warning_t;

typedef struct {
    void *ctx;
    /* Send one action (device_service_act_auto with less_secure, the installer option): returns
     * an hc_result_t; *request_id the id auto_run_result will carry when HC_OK. */
    int (*device_act)(void *ctx, const auto_action_t *a, bool less_secure,
                      uint32_t *request_id);
    /* The source has been up and listed for 30 s or more (missing vs unreachable). NULL: no. */
    bool (*source_settled)(void *ctx, uint8_t source);
    /* The registry lists this ref now: *kind its live dev_kind_t. False: not listed. NULL: the
     * live kind can't be checked (AUT-002 layer 4), so nothing is sent: can't run. */
    bool (*device_kind)(void *ctx, uint8_t source, const char *ref, uint8_t *kind);
    /* Phase C (may be NULL; never called while the env says !arm_supported). Arm partition p
     * at level as the user maker, source {AUTOMATION, uid}, flags 0 or ALARM_ARM_BYPASS_FAULTED
     * (alarm_service_command_as with ALARM_SVC_WHEN_DISARMED): *already true when the partition
     * wasn't disarmed (nothing done, nothing logged), *zone the not-ready zone when refused for
     * one. NULL: ARM_REFUSED (ALARM_ERR_UNAVAILABLE). */
    alarm_result_t (*arm)(void *ctx, uint16_t uid, uint8_t p, uint8_t level, uint8_t flags,
                          uint8_t maker, bool *already, uint16_t *zone);
    /* A warning or a hold began, changed or ended (auto_warning_t.state). NULL: none. */
    void (*warning)(void *ctx, const auto_warning_t *w);
    /* A Lock or Close action ended refused, no_answer, unreachable, missing, busy or
     * not_allowed: raise
     * ALARM_EV_AUTOMATION_FAILED (reason an alarm_auto_fail_t, dev_action the action, zone the
     * triggering zone or 0). At most once per automation per hour unless the reason changes.
     * NULL: none. */
    void (*failed)(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action, uint16_t zone);
    /* AUT-015 / D23 (DESIGN.md section 3, layer 4): read live, in one read at send time before
     * each less secure action: returns whether user slot `user` still holds a system master or
     * installer (ALARM_AUTH_MASTER or ALARM_AUTH_INSTALLER), and sets *option_on to the
     * installer option "Allow less secure automations" now (the runner fills it with the
     * env's first: a port that can't tell leaves it). Option off: nothing is sent
     * (LESS_SECURE_OFF), whatever the env said. Called only while the env's option is on.
     * NULL: no. */
    bool (*maker_ok)(void *ctx, uint8_t user, bool *option_on);
    /* Phase C. Disarm partition p as the user maker, source {AUTOMATION, uid}: *already true
     * when it was disarmed already (nothing done). NULL: ARM_REFUSED (ALARM_ERR_UNAVAILABLE). */
    alarm_result_t (*disarm)(void *ctx, uint16_t uid, uint8_t p, uint8_t maker, bool *already);
    /* Phase C. Someone put an arming off (AUT-013): raise ALARM_EV_ARMING_PUT_OFF (user, source
     * {AUTOMATION, uid}, partition, arg choice). until_epoch_ms: HOUR's new arm time while the
     * clock is valid, else 0 (NIGHT: the words need no time). NULL: none. */
    void (*put_off)(void *ctx, uint16_t uid, uint8_t partition, uint8_t user, uint8_t choice,
                    int64_t until_epoch_ms);
    /* PL04, the revocation gate: called right before each send the runner commits for automation
     * uid - a device action, an Off of "Turn it off after", an arm, a disarm - with send_end once
     * after it, whatever it answered (after the send when it answered true). False: uid was
     * changed, turned off or deleted after the rules this runner was last given (auto_run_rules),
     * so nothing is sent now: the send waits, unchanged, and is tried again at once
     * (auto_run_next_deadline) until auto_run_rules brings uid's current version, which decides
     * as auto_run_rules says (a turned-off one's actions go, its running off timer's Off still
     * goes; a deleted one's all go; a changed action's waiting send goes; the rest is sent). The
     * platform keeps a lock across send_begin..send_end that an edit takes before it answers,
     * and marks uid revoked (auto_revoke_t) in the same step that publishes the edit, so no send
     * of the old version starts once the edit has answered. NULL: never revoked. */
    bool (*send_begin)(void *ctx, uint16_t uid);
    void (*send_end)(void *ctx);
    /* BL-224 (each may be NULL: that action then ends UNREACHABLE, or CANT_RUN for a reminder):
     * Wired output `output` (1..AUTO_OUTPUTS) on or off; false: it can't be now (it isn't "For
     * automations" any more, or its pin failed). */
    bool (*output)(void *ctx, uint8_t output, bool on);
    /* Partition p's door chime on or off as the user maker, source {AUTOMATION, uid}: *already
     * true when it was so (nothing done). */
    alarm_result_t (*chime)(void *ctx, uint16_t uid, uint8_t p, bool on, uint8_t maker,
                            bool *already);
    /* Send a notification to the phone services; false: none is set up (nothing sent). */
    bool (*notify)(void *ctx, const auto_notice_t *n);
    /* Partition p now: 1 armed (exit time and alarms included), 0 disarmed, -1 can't tell. */
    int (*part_armed)(void *ctx, uint8_t p);
    /* A reminder's note began or ended (the mailbox; the platform chimes once when one begins). */
    void (*remind)(void *ctx, const auto_remind_t *m);
} auto_ports_t;

/* PL04: the automations changed, turned off or deleted since the runner was last given the rules
 * - the platform's side of the send_begin port, kept under its lock: add at each edit (in the
 * same step that publishes it), clear when the edited rules are copied for auto_run_rules. */
typedef struct {
    uint16_t uid[AUTO_MAX];
    uint8_t n;
    uint8_t all;  /* 1: every automation (uid 0 added, or more than AUTO_MAX uids) */
} auto_revoke_t;

void auto_revoke_clear(auto_revoke_t *v);
/* uid 0: every automation (an edit that can't name it, e.g. a delete by slot). */
void auto_revoke_add(auto_revoke_t *v, uint16_t uid);
bool auto_revoke_has(const auto_revoke_t *v, uint16_t uid);

/* ---- private state (fields may change; read it only through the calls below) ---------- */
typedef struct {
    auto_action_t act;   /* what to send (a copy) */
    uint64_t next_ms;    /* try at or after this */
    uint64_t held_from;  /* the first held try */
    uint16_t zone;       /* the triggering zone, 0 = none */
    uint64_t behind_from; /* an On's first wait behind its action's other Off */
    uint8_t waiting;     /* 1: to be sent */
    uint8_t held;        /* 1: held (offline or unknown on Home Assistant) */
    uint8_t behind;      /* 1: behind_from is set */
    uint8_t tried;       /* 1: started by Try it now (auto_status_t.tried) */
} auto_send_t;

typedef struct {
    auto_send_t s;       /* the action's own send */
    uint8_t occ_kind;    /* the occurrence's auto_trig_t (ARMED/DISARMED holds are dropped) */
    uint8_t occ_part;
} auto_job_t;

typedef struct {
    auto_send_t s;       /* the Off (a copy of the device it turned on, made ACT_OFF);
                          * s.waiting: due and not sent yet - never overwritten by a new run */
    uint64_t at;
    uint64_t len_ms;
    uint8_t running;
} auto_offtimer_t;

/* An arming automation's warning, hold or waiting disarm. */
typedef struct {
    uint8_t state;          /* auto_warn_state_t */
    uint8_t user;           /* who put it off */
    uint8_t disarm_wait;    /* 1: a Disarm waits for the partition gap */
    uint8_t disarm_idx;
    uint64_t arm_at;        /* ON: the arm is due; HOUR: the new planned time */
    uint64_t rewarn_at;     /* HOUR: the warning starts again */
    uint64_t hold_cap;      /* NIGHT, DAY: ends by this at the latest (no clock: see auto_arm.c) */
    uint64_t disarm_at;
    int32_t until_day;
    uint16_t until_minute;
    int32_t arm_run;        /* a time trigger's arm minute: its run memory (auto_time_due) */
} auto_arming_t;

/* BL-224: an automation's outputs, notifications and reminder (auto_panel.c). */
typedef struct {
    uint64_t off_at[AUTO_ACTIONS];   /* an output's "Turn it off after" ends */
    uint64_t wait_at[AUTO_ACTIONS];  /* an output action waiting for its gap: send at */
    uint64_t notify_at;              /* the last notification sent */
    uint64_t note_cap;               /* the note ends by this at the latest (no clock) */
    int32_t remind_day;              /* the note ends at this local date ... */
    uint16_t remind_minute;          /* ... and minute */
    uint8_t off_running[AUTO_ACTIONS];
    uint8_t off_output[AUTO_ACTIONS];/* which output the timer turns off */
    uint8_t off_waiting[AUTO_ACTIONS];/* its Off is due, waiting for the gap */
    uint8_t off_tried[AUTO_ACTIONS];
    uint8_t waiting[AUTO_ACTIONS];   /* 1: the action's output send waits */
    uint8_t wait_tried[AUTO_ACTIONS];
    uint8_t notified;                /* notify_at is set */
    uint8_t remind_on;               /* the note shows */
    uint8_t remind_part;
} auto_panel_t;

typedef struct {
    auto_rule_t rule;
    auto_arming_t arm;
    uint8_t used;
    uint8_t runnable;    /* loaded, on and checked */
    uint8_t valid;       /* loaded and checked (Try it now) */
    uint8_t cooling;
    uint8_t closing;
    uint8_t failed_any;
    uint8_t failed_reason;
    uint64_t cool_until;
    uint64_t close_at;
    uint64_t failed_ms;
    int32_t last_run;
    auto_job_t job[AUTO_ACTIONS];
    auto_offtimer_t off[AUTO_ACTIONS];
    auto_status_t st;
    auto_panel_t pn;     /* BL-224 */
} auto_entry_t;

typedef struct {
    uint64_t deadline;
    uint32_t req;
    uint16_t uid;
    uint16_t zone;
    uint8_t used;
    uint8_t idx;
    uint8_t dev_action;
    uint8_t tried;
} auto_flight_t;

typedef struct {
    uint64_t at;
    uint8_t used;
    uint8_t source;
    char ref[DEV_REF_LEN];
} auto_gap_t;

typedef struct {
    auto_ports_t port;
    auto_entry_t e[AUTO_MAX];
    auto_flight_t flight[AUTO_RUN_FLIGHTS];
    auto_gap_t gap[AUTO_RUN_GAPS];
    uint64_t cap[AUTO_CAP_ACTIONS];  /* the latest sends' times, a ring */
    uint8_t cap_next;  /* the oldest when cap_used is full */
    uint8_t cap_used;
    uint8_t partitions_enabled;
    uint8_t less_secure;    /* the env's installer option (AUT-015) */
    uint8_t have_epoch;
    uint8_t own_arm;        /* bit p-1: this runner armed partition p; its ARMED is its own */
    uint8_t own_disarm;     /* bit p-1: this runner disarmed p; its DISARMED is its own */
    uint8_t part_used;      /* bit p-1: part_at[p-1] holds a time */
    uint64_t part_at[ALARM_MAX_PARTITIONS]; /* the last automatic arm or disarm (AUT-007) */
    uint32_t gen;           /* auto_clock_gen after the previous auto_run_time */
    int64_t epoch_base_ms;  /* epoch ms - now ms, while the clock is valid */
    auto_zone_t zones[ALARM_MAX_ZONES];
    uint64_t out_at[AUTO_OUTPUTS];  /* BL-224: the last automation action on each wired output */
    uint8_t out_used;               /* bit n-1: out_at[n-1] holds a time */
} auto_run_t;

void auto_run_init(auto_run_t *r, const auto_ports_t *p, uint64_t now_ms);

/* The current automations (copied in; at most AUTO_MAX, uid 0 and repeated uids ignored) and
 * the live zones, partitions and installer option (copied in). An automation that disappears loses its held
 * sends, its off timers and its pending results; one turned off lets a running off timer and
 * its Off finish and drops its held sends. The rest keep their status, cooldown and run memory,
 * except: a changed time trigger forgets its last run; a changed trigger kind, zone or
 * partitions clears its close hold and cooldown; a changed action (source, ref, kind, action,
 * value or mode) drops that action's waiting send, while an off timer already running for the
 * old device keeps its own copy and still turns it off. A changed "Turn it off after" alone keeps
 * a waiting On, which takes the new length when it is sent (it is sent once). */
void auto_run_rules(auto_run_t *r, const auto_rule_t *rules, size_t n, const auto_env_t *env);

/* One occurrence; lt the local time now (NULL or !valid: not set). */
void auto_run_occurrence(auto_run_t *r, const auto_occ_t *o, const auto_local_t *lt,
                         uint64_t now_ms);

/* The spans one auto_clock_step returned, with the clock after that step (its gen and floor,
 * auto_clock.h's contract) and the reading it took (lt may be NULL). */
void auto_run_time(auto_run_t *r, const auto_clock_t *c, const auto_local_t *lt,
                   const auto_span_t *s, size_t n, uint64_t now_ms);

/* A sent action's answer, by request id (unknown ids are ignored); why an auto_why_t. */
void auto_run_result(auto_run_t *r, uint32_t request_id, bool ok, uint8_t why, uint64_t now_ms);

/* Due retries, close holds, gaps, result timeouts and off timers. */
void auto_run_tick(auto_run_t *r, uint64_t now_ms);

/* The earliest of those deadlines (now_ms when one is due); UINT64_MAX when none. */
uint64_t auto_run_next_deadline(const auto_run_t *r, uint64_t now_ms);

/* Try it now: run a device automation's actions at once, turned off or not; the trigger,
 * window and cooldown are ignored, the device gap, the cap, the installer option and the maker
 * are not. Its outcomes carry auto_status_t.tried = 1 (AUT-006). */
auto_try_t auto_run_try(auto_run_t *r, uint16_t uid, uint64_t now_ms);

/* Put off automation uid's arming while it warns (AUT-013, D26), by user (the code's, checked by
 * the caller with ALARM_CMD_CODE_CHECK). HOUR: the planned time + 60 minutes, the warning again 5
 * minutes before; as often as it warns. NIGHT: no arming by it before 6:00 AM local when the
 * planned time is from 18:00 to 06:00, else before midnight ("Not today"); only when may_disarm
 * (the code may disarm that partition) and lt is a valid reading. Calls the put_off and warning
 * ports and sets the status (PUT_OFF). Partition-wide put-off is the caller's loop over the
 * warnings it shows: a card is one automation's. */
auto_put_off_t auto_run_put_off(auto_run_t *r, uint16_t uid, uint8_t choice, uint8_t user,
                                bool may_disarm, const auto_local_t *lt, uint64_t now_ms);

/* Automation uid's warning or hold now (state NONE when none, or an unknown uid). */
void auto_run_warning(const auto_run_t *r, uint16_t uid, auto_warning_t *out);

/* True while a hold waits for a local time (NIGHT, DAY): step the clock (auto_run_time) at
 * least once a minute meanwhile, even with no time trigger, so the hold ends on time. */
bool auto_run_wants_clock(const auto_run_t *r);

/* The automation's last outcome (all zero, AUTO_RES_NONE, for an unknown uid). */
void auto_run_status(const auto_run_t *r, uint16_t uid, auto_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_AUTO_RUN_H */
