/*
 * notify_gate.h - what actually reaches a phone (docs/features/26).
 *
 * An alarm panel can produce a lot of events in a few seconds: eight zones tripping during one
 * burglary, a flapping sensor, six hours of arming and disarming replayed after the Wi-Fi comes
 * back. None of that should arrive as eight, or sixty, or two hundred separate alerts. The gate
 * decides, for one event, whether it is sent, folded into one that is already running, or
 * dropped - and it is pure: no clock of its own, no I/O, no allocation, so every rule below is
 * a host test.
 *
 * Portable C11. All state is in the caller-owned notify_gate_t (RAM only, never stored).
 */
#ifndef NOTIFY_GATE_H
#define NOTIFY_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_types.h"
#include "notify/notify_words.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOTE-030: a burst of 10, then one every 30 s. Well inside ntfy.sh's 60-per-burst and
 * Pushover's monthly allowance, and enough that a normal evening is never held back. */
#define NOTIFY_BUCKET_MAX       10u
#define NOTIFY_BUCKET_REFILL_MS 30000u

/* NOTE-031: a rolling day. Past the soft cap only alarms get out; past the hard cap nothing
 * does, and the integration's page says so rather than failing quietly. */
#define NOTIFY_DAY_MS   86400000u
#define NOTIFY_DAY_SOFT 200u
#define NOTIFY_DAY_HARD 250u

/* NOTE-032/033: the first alarm goes immediately; every other zone that trips in the next
 * minute is folded into a single follow-up, which names three zones and counts the rest. */
#define NOTIFY_ALARM_MIN_GAP_MS 20000u
#define NOTIFY_ROLLUP_MS        60000u
#define NOTIFY_ROLLUP_ZONES     3u

/* NOTE-036: coming back after an outage is digested, never replayed. More than this many
 * markers behind means a digest; alarms still go one by one, up to NOTIFY_DIGEST_ALARMS, so a
 * six-hour outage costs at most seven notifications however long it was. */
#define NOTIFY_DIGEST_MIN    8u
#define NOTIFY_DIGEST_ALARMS 5u

/* NOTE-034: the same trouble, on the same zone, is not repeated within the hour. */
#define NOTIFY_TRBL_COLLAPSE_MS 3600000u
#define NOTIFY_TRBL_SLOTS       16u

#define NOTIFY_CAT_BIT(c) ((uint8_t)(1u << (unsigned)(c)))
/* NOTE-037: alarms, troubles and arming on; zone activity and system events off. */
#define NOTIFY_CATS_DEFAULT                                                            \
    (NOTIFY_CAT_BIT(NOTIFY_CAT_ALARM) | NOTIFY_CAT_BIT(NOTIFY_CAT_TROUBLE) |           \
     NOTIFY_CAT_BIT(NOTIFY_CAT_ARMING))

typedef enum {
    NOTIFY_ACT_DROP = 0, /* not wanted, already said, or over a limit */
    NOTIFY_ACT_SEND,     /* send this one now */
    NOTIFY_ACT_HOLD,     /* folded into an alarm already running; watch notify_gate_follow_up() */
} notify_act_t;

/* An alarm in progress, and the zones it has taken in so far. */
typedef struct {
    bool open;
    bool follow_up;   /* at least one more zone tripped after the first alert went out */
    uint8_t cause;    /* alarm_cause_t of the first one */
    uint8_t partition;
    uint8_t n;        /* zones recorded in zones[] */
    uint16_t total;   /* zones seen, including any past the end of zones[] */
    uint16_t zones[NOTIFY_ROLLUP_ZONES];
    uint64_t opened;
    uint32_t seq;     /* the newest event taken in (PUSH-005: notify_priority_dealt()) */
} notify_rollup_t;

typedef struct {
    uint32_t id; /* family, kind and zone of a trouble already reported */
    uint64_t at;
} notify_trouble_slot_t;

typedef struct {
    uint8_t cats;      /* NOTIFY_CAT_BIT(...) of what the owner wants */
    bool silent_too;   /* NOTE-038: send duress and a silent panic as well (default off) */
    uint8_t tokens;
    uint64_t refilled;
    uint16_t day_count;
    uint64_t day_start;
    uint64_t last_alarm;
    notify_rollup_t rollup[ALARM_MAX_PARTITIONS];
    /* A closed rollup whose follow-up has not been taken yet (follow_up set), kept until
     * notify_gate_follow_up_sent() so a failed request is not lost (S02). RAM only. */
    notify_rollup_t owed[ALARM_MAX_PARTITIONS];
    notify_trouble_slot_t trouble[NOTIFY_TRBL_SLOTS];
} notify_gate_t;

void notify_gate_init(notify_gate_t *g, uint8_t cats, bool silent_too, uint64_t now);

/* NOTE-037, NOTE-042: does the owner want this event, by its category (cats: NOTIFY_CAT_BIT of
 * what is switched on)? An automation's notification (ALARM_EV_AUTOMATION_NOTICE) always is: the
 * person asked for it when they made the automation (docs/features/41 BL-224). Every path that
 * filters by category asks this (the gate, the digest after an outage). */
bool notify_gate_wanted(uint8_t cats, const alarm_event_t *ev);

/* The owner changed what they want to hear about. Counters and windows are kept. */
void notify_gate_set(notify_gate_t *g, uint8_t cats, bool silent_too);

/* Admission and delivery, apart (NOTE-014, code review S02). A request can fail and be tried
 * again; the gate must not count it as said until it was. So:
 *  - notify_gate_admit() decides. DROP and HOLD are final and recorded at once (a HOLD folds
 *    the zone into a running alarm, whose follow-up is sent by its own path). SEND records
 *    nothing: offered again before it went, the same event is admitted again, as if new.
 *  - notify_gate_sent() records a SEND once its delivery reached its end: the service took it,
 *    or refused it for good. Only then does it open the alarm's rollup, spend a token, count
 *    towards the day, or mark a trouble as said. Call it with the same event, before any other
 *    event is admitted. */
notify_act_t notify_gate_admit(notify_gate_t *g, const alarm_event_t *ev, uint64_t now);
void notify_gate_sent(notify_gate_t *g, const alarm_event_t *ev, uint64_t now);

/* Both at once, for a send that cannot fail: what to do with one event, right now. */
notify_act_t notify_gate_offer(notify_gate_t *g, const alarm_event_t *ev, uint64_t now);

/* One delivery attempt: admit, and on SEND call send(ctx, ev), which returns true when the
 * service took it or refused it for good (a 2xx, or a 4xx other than 429) and false when it is
 * worth trying again (no answer, a 429 or a 5xx). true: the event is finished with (dropped,
 * folded in, taken or refused) and recorded; false: nothing was recorded - offer it again. */
typedef bool (*notify_send_fn)(void *ctx, const alarm_event_t *ev);
bool notify_gate_deliver(notify_gate_t *g, const alarm_event_t *ev, uint64_t now,
                         notify_send_fn send, void *ctx);

/* One event that several services must each finish with on their own (LOG-010's live queue,
 * code review S02): one service taking it doesn't write it off for another whose request
 * failed. *done keeps, per service (bit n: service n), who has finished with it; wanted is the
 * services switched on now. try_one(ctx, n) is called for each wanted one not done yet, and
 * returns true when that service is finished with the event (taken, refused for good, dropped
 * or folded in by its gate) and false when not yet (its turn hasn't come: NOTIFY_SEND_GAP_MS or
 * its backoff, or the request failed). true: every wanted service is finished with it - drop
 * it, *done is cleared for the next one. Nobody wanted is finished at once. */
typedef bool (*notify_try_fn)(void *ctx, uint8_t service);
bool notify_fan_out(uint8_t *done, uint8_t wanted, notify_try_fn try_one, void *ctx);

/* A follow-up that is due (a rollup whose minute is up, or one whose request failed). true:
 * *out holds it, and it stays due - returned again by the next call - until
 * notify_gate_follow_up_sent() says its request reached its end. One still owed when the next
 * rollup of its partition closes takes that one's zones in. Never blocks, never allocates. */
bool notify_gate_follow_up(notify_gate_t *g, uint64_t now, notify_rollup_t *out);
void notify_gate_follow_up_sent(notify_gate_t *g, uint8_t partition, uint64_t now);

/* Both at once, for a send that cannot fail: a rollup whose minute is up. true: *out holds it
 * and one follow-up should be sent. Call once a second or so. */
bool notify_gate_tick(notify_gate_t *g, uint64_t now, notify_rollup_t *out);

/* Past the hard cap for the day: nothing is being sent, and the page should say so. */
bool notify_gate_paused(const notify_gate_t *g);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_GATE_H */
