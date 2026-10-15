/*
 * zw_health.h - wake-up handling and supervision per node (docs/features/36, network.md §7
 * "Sleeping devices and the wake-up queue" and §8 "Supervision: is the device still there?";
 * ZW-011, ZW-012). A pure decision module: it sends nothing and does no I/O itself - every call
 * either changes `zw_health_t`'s own state or returns actions for the caller (`zw_net`, a later
 * milestone) to actually carry out (build the command, send it through zw_transport, and report
 * the outcome back in). zwave_host depends on nothing else in this repository, and zw_health
 * depends on nothing else in zwave_host either: it never touches zw_net, zw_s2, zw_incl, zw_host,
 * zw_transport or a node's cache record directly, so it is exercised here against a fake clock
 * alone, the same way zw_transport is exercised against the fake ZAC93's wire.
 *
 * Who calls what: only the Z-Wave task ever touches a `zw_health_t` (the same one-task-owns-it
 * rule as `zw_host.h`/`zw_transport.h`). `zw_net` (or its start-up/interview code) calls the node
 * lifecycle functions when a device is added, removed, or its wake-up interval is negotiated or
 * changed; it calls the event functions as frames, callbacks and interview/repair state changes
 * arrive from below; it calls `zw_health_poll()` once per task iteration (after delivering any
 * news, so a state change already reflects in the same poll) and sleeps until
 * `zw_health_next_deadline()` or the next piece of news, whichever comes first.
 * `zw_health_choose_interval()` is a free function: any caller, any time, no `zw_health_t` needed.
 */
#ifndef ZW_HEALTH_H
#define ZW_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits --------------------------------------------------------------------------------------
 * ZW_HEALTH_NODES_MAX matches zw_nodes.h's own ZW_NODES_MAX (the node cache's ceiling) by
 * convention, not by including it: zw_health has no dependency of its own, so it states the same
 * number again rather than reaching into a sibling header for one constant.
 * The wake-up queue is a shared pool, not 64 x 8 separate arrays (most nodes queue nothing most of
 * the time): ZW_HEALTH_QUEUE_POOL_SIZE entries in all, ZW_HEALTH_QUEUE_PER_NODE_MAX of them for
 * any one node (network.md §7), each up to ZW_HEALTH_QUEUE_ENTRY_BYTES - the plaintext command
 * bytes before S2 encapsulation (a Notification Set, a Configuration Set, an interview step's
 * request... comfortably under 48 bytes; S2's own overhead is added later, downstream). */
#define ZW_HEALTH_NODES_MAX 64u
#define ZW_HEALTH_QUEUE_POOL_SIZE 128u
#define ZW_HEALTH_QUEUE_PER_NODE_MAX 8u
#define ZW_HEALTH_QUEUE_ENTRY_BYTES 48u

/* §7: "a device already configured with interval 0 ('wake only by hand') is left alone and gets
 * no supervision". Used both as a wake-up interval value and, since it can never legitimately be
 * anything else, as the sentinel for "never change it, never supervise it". */
#define ZW_HEALTH_INTERVAL_NEVER 0u

/* §8's timings, spelled out (a table row's own numbers, not tuned constants): */
#define ZW_HEALTH_SLEEP_GRACE_MS (10u * 60u * 1000u)     /* T + this, without any frame: unreachable */
#define ZW_HEALTH_PING_IDLE_MS (15u * 60u * 1000u)       /* silence before a listening node is pinged */
#define ZW_HEALTH_PING_RETRY_MS (15u * 60u * 1000u)      /* the 2nd ping, this long after the 1st fails */
#define ZW_HEALTH_LATE_WAKEUP_MS (5u * 60u * 1000u)      /* later than the interval by this: re-read it */
#define ZW_HEALTH_NO_MORE_INFO_MS 250u                   /* after the last queued send, nothing left */
/* Restart (§7/§8 "nothing heard before counts"): a listening node most last have been heard from
 * this long ago (a fiction) so zw_health_restart() need not special-case its ping timer - the
 * normal ZW_HEALTH_PING_IDLE_MS rule then naturally comes due this many ms after the restart. */
#define ZW_HEALTH_RESTART_PING_DELAY_MS 5000u

/* A ping's outcome is reported through zw_health_sent() like any queued command's, with this
 * reserved tag - never a valid queue "kind" (see zw_health_queue_put()). */
#define ZW_HEALTH_PING_TAG 0xFFFFFFFFu

/* ---- interval choice (network.md §7, pure, no zw_health_t needed) --------------------------------
 * "Clamped to the device's minimum and maximum and rounded to its step." 0 ('wake only by hand')
 * passes straight through, never clamped or rounded away from 0: it is not a real interval to
 * negotiate, it means there is nothing to negotiate. A step of 0 (a capabilities report that never
 * really occurs, but must not crash) is treated as "no rounding needed"; rounding is to the
 * nearest step relative to 'min_s', ties rounding up, re-clamped afterwards in case rounding
 * pushed the value back out of range. */
uint32_t zw_health_choose_interval(uint32_t wanted_s, uint32_t min_s, uint32_t max_s, uint32_t step_s);

/* ---- actions: what zw_health_poll() (and the event functions) hand back to the caller ------------ */

typedef enum {
    ZW_HEALTH_ACTION_PING,            /* send No Operation to node_id; report via zw_health_sent()
                                          with tag ZW_HEALTH_PING_TAG */
    ZW_HEALTH_ACTION_UNREACHABLE,     /* node_id just became unreachable (§8) */
    ZW_HEALTH_ACTION_REACHABLE,       /* node_id is reachable again (§8) */
    ZW_HEALTH_ACTION_SEND_QUEUE,      /* node_id is awake: zw_health_queue_take() it empty, one at a
                                          time, reporting each through zw_health_sent() */
    ZW_HEALTH_ACTION_NO_MORE_INFO,    /* send Wake Up No More Information to node_id */
    ZW_HEALTH_ACTION_REREAD_INTERVAL, /* node_id woke more than 5 min late: read its interval again */
} zw_health_action_kind_t;

typedef struct {
    zw_health_action_kind_t kind;
    uint16_t node_id;
} zw_health_action_t;

/* ---- the module itself -----------------------------------------------------------------------
 * Caller-owned, no heap, like every portable component here: declare a plain `zw_health_t` (as a
 * `static`/global, not a task's small stack - the wake-up queue pool alone is several KB) and call
 * zw_health_init() before anything else. */

typedef struct {
    bool used;                 /* false: a free slot in the shared pool */
    bool in_flight;            /* zw_health_queue_take() gave this one out; awaiting zw_health_sent() */
    uint16_t node_id;
    uint32_t kind;             /* the caller's replace-if-same key */
    bool is_interview_step;    /* taken before any other kind */
    uint32_t seq;              /* insertion order, for FIFO among equal priority */
    uint8_t data[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    uint8_t data_len;
} zw_health_queue_entry_t;

typedef struct {
    uint16_t node_id;          /* 0: a free slot */
    bool is_listening;
    uint32_t interval_s;       /* ZW_HEALTH_INTERVAL_NEVER = wake only by hand */
    bool reachable;            /* last reported state (§8) */

    uint64_t last_heard_ms;    /* last proof of life: a frame, an ACK (listening), a delivered send */

    /* listening: §8's two-strike ping */
    bool ping_outstanding;
    bool ping_failed_once;
    uint64_t first_fail_ms;

    /* sleeping: §7's wake-up queue draining */
    bool awake;
    bool no_more_info_pending;
    uint64_t no_more_info_due_ms;
    bool interviewing;
    bool in_repair;
} zw_health_node_t;

typedef struct {
    zw_health_node_t nodes[ZW_HEALTH_NODES_MAX];
    size_t node_count;
    zw_health_queue_entry_t pool[ZW_HEALTH_QUEUE_POOL_SIZE];
    uint32_t next_seq;
} zw_health_t;

void zw_health_init(zw_health_t *h);

/* ---- node lifecycle --------------------------------------------------------------------------- */

/* false when 'node_id' is already present, is 0, or the table is at ZW_HEALTH_NODES_MAX. Starts
 * reachable (assumed, not yet disproven) and, for a sleeping node, asleep with an empty queue. */
bool zw_health_add(zw_health_t *h, uint16_t node_id, bool is_listening, uint32_t interval_s);
/* Drops the node and every queue entry of its. true if 'node_id' was present. */
bool zw_health_remove(zw_health_t *h, uint16_t node_id);
/* Changes a node's own wake-up interval (already chosen - see zw_health_choose_interval()) without
 * touching anything else about it. false if 'node_id' is not present. */
bool zw_health_set_interval(zw_health_t *h, uint16_t node_id, uint32_t interval_s);
bool zw_health_is_listening(const zw_health_t *h, uint16_t node_id, bool *out);
uint32_t zw_health_interval(const zw_health_t *h, uint16_t node_id);
/* The state zw_health last reported for 'node_id' (true until proven otherwise - see
 * zw_health_add()); false (not reachable) is also returned when 'node_id' is not present. */
bool zw_health_is_reachable(const zw_health_t *h, uint16_t node_id);

/* ---- events in (§8's "heard from" column, and the wake-up/interview/repair state §7 needs) ------- */

/* Any frame from the node (a report, a nonce request, a wake-up notification...): proof of life
 * for every kind of node. Resets the sleeping unreachable timer / the listening ping timer alike.
 * Writes at most 1 action (REACHABLE, once, if the node was unreachable); returns how many. */
size_t zw_health_heard(zw_health_t *h, uint16_t node_id, uint64_t now_ms, zw_health_action_t *out,
                        size_t max);
/* The module ACKed something the panel sent, with no frame of the node's own otherwise: proof of
 * life for a *listening* node only (§8's table lists "every ACK" for listening devices alone); a
 * no-op (0 actions) for a sleeping one or an unknown node. Otherwise exactly zw_health_heard(). */
size_t zw_health_acked(zw_health_t *h, uint16_t node_id, uint64_t now_ms, zw_health_action_t *out,
                        size_t max);
/* A Wake Up Notification, a node information frame, or a Get other than a Nonce Get from an asleep
 * node (network.md §7; a report is no wake-up): proof of life, and the node is now awake. Emits REACHABLE (as zw_health_heard() would),
 * REREAD_INTERVAL first when this is more than ZW_HEALTH_LATE_WAKEUP_MS later than expected, and
 * then either SEND_QUEUE (something is queued) or arms Wake Up No More Information for
 * ZW_HEALTH_NO_MORE_INFO_MS from now (nothing was). Writes at most 2 actions; returns how many. */
size_t zw_health_wakeup(zw_health_t *h, uint16_t node_id, uint64_t now_ms, zw_health_action_t *out,
                         size_t max);
/* The outcome of one send: either a ping (tag == ZW_HEALTH_PING_TAG) or the queue entry currently
 * taken for 'node_id' (tag == that entry's own "kind", see zw_health_queue_put()) - a mismatched
 * tag is ignored. A delivered send is proof of life. An undelivered ping counts toward §8's two
 * failures; an undelivered queue entry to a *sleeping* node goes back into its queue and the node
 * counts as asleep again (§7) - to a listening node it is simply dropped (its own reachability is
 * the ping's job, not the queue's). Writes at most 1 action (UNREACHABLE or REACHABLE); returns
 * how many. */
size_t zw_health_sent(zw_health_t *h, uint16_t node_id, uint32_t tag, bool delivered, uint64_t now_ms,
                       zw_health_action_t *out, size_t max);
/* Whether 'node_id' is mid-interview or mid-route-repair right now: while either is true, Wake Up
 * No More Information is held back (§7) even once its queue is empty and 250 ms have passed. */
void zw_health_set_busy(zw_health_t *h, uint16_t node_id, bool interviewing, bool in_repair);

/* A panel or module restart: "nothing heard before counts" (§7/§8). Every sleeping node's
 * unreachable timer restarts as if just heard from (its full T + 10 min again); every listening
 * node is due for a verification ping ZW_HEALTH_RESTART_PING_DELAY_MS from now rather than the
 * usual 15 min of silence. No action is reported by this call itself (no new evidence either
 * way) - reachability changes, if any, surface from the pings and frames that follow. */
void zw_health_restart(zw_health_t *h, uint64_t now_ms);

/* ---- the wake-up queue (network.md §7) ------------------------------------------------------------
 * 'kind' is the caller's own key for "this queued command replaces an older one of the same
 * kind" (e.g. cc << 16 | command << 8 | parameter for a Configuration Set) - it is opaque to
 * zw_health, except that ZW_HEALTH_PING_TAG is reserved and must never be used as one. */

/* Adds or replaces (by 'node_id' + 'kind') a queue entry; 'is_interview_step' entries are always
 * taken before any other kind (§5, §7). False when 'node_id' is not present, 'data_len' is over
 * ZW_HEALTH_QUEUE_ENTRY_BYTES, 'kind' == ZW_HEALTH_PING_TAG, the node's own 8 slots are full and
 * 'kind' does not match one of them, or the shared pool has nothing free. */
bool zw_health_queue_put(zw_health_t *h, uint16_t node_id, uint32_t kind, bool is_interview_step,
                          const uint8_t *data, size_t data_len);
/* Removes 'node_id'+'kind' if queued (whether or not it is the one currently taken - see below);
 * true if one was found. */
bool zw_health_queue_cancel(zw_health_t *h, uint16_t node_id, uint32_t kind);
/* Copies the data of 'node_id'+'kind' if queued (up to 'out_cap' bytes; '*out_len' its length),
 * leaving it queued: what a replacement is about to overwrite. False if none is. */
bool zw_health_queue_peek(const zw_health_t *h, uint16_t node_id, uint32_t kind, uint8_t *out_data,
                          size_t out_cap, size_t *out_len);
size_t zw_health_queue_len(const zw_health_t *h, uint16_t node_id);

/* Hands back the next entry to send for 'node_id' - interview steps first, then insertion order -
 * and marks it "in flight": no further take() for this node succeeds until zw_health_sent()
 * reports its outcome (§7's "takes entries one at a time"). False when nothing is available (the
 * queue is empty, or one is already in flight). '*out_kind' becomes the tag to report back. */
bool zw_health_queue_take(zw_health_t *h, uint16_t node_id, uint32_t *out_kind, uint8_t *out_data,
                           size_t out_cap, size_t *out_len, bool *out_is_interview_step);

/* ---- polling -------------------------------------------------------------------------------------
 * Runs everything time alone can trigger (a listening node's next ping, a sleeping node's
 * unreachable deadline, a No More Information whose 250 ms is up) and writes the resulting
 * actions to 'out' (capacity 'max'), returning how many - as many as fit; call again with a
 * larger 'max' (or a fresh poll at the same 'now_ms') to see the rest, mirroring the event
 * functions' own "at most N, returns how many" shape. */
size_t zw_health_poll(zw_health_t *h, uint64_t now_ms, zw_health_action_t *out, size_t max);

/* When zw_health_poll() next has work on its own (0 = at once, UINT64_MAX = not before an event
 * function reports news) - a busy node's held-back No More Information contributes no deadline
 * until it stops being busy. */
uint64_t zw_health_next_deadline(const zw_health_t *h);

#ifdef __cplusplus
}
#endif

#endif /* ZW_HEALTH_H */
