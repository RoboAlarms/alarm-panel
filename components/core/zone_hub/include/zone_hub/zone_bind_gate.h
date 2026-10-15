/*
 * zone_bind_gate.h - a sensor binding changes only while every partition is disarmed, and
 * arming can't pass between that check and the change (docs/features/22 ZSRC-012, review RF32).
 *
 * The check and the change run on different tasks: the engine's state is the alarm task's, the
 * hub's is the zone lock's, and the alarm task never waits for the zone lock. So the change is a
 * reservation in three steps, and arming always wins over a reservation that may not happen:
 *  1. reserve - on the alarm task, in one engine step: every enabled partition is disarmed and no
 *     other reservation stands, so the gate is RESERVED. Arming is NOT held: an arm in any later
 *     engine step goes ahead and cancels the reservation (zone_bind_gate_step_end).
 *  2. claim - by the changer, under the zone lock, never waiting, within ZONE_BIND_GATE_MS of
 *     the reservation: RESERVED becomes CHANGING. Refused when an arm cancelled it or its time is
 *     up: the binding is never touched. Asked while an engine step runs (STEPPING: that step may
 *     be an arm), it answers RETRY: let the zone lock go, wait a tick, try again.
 *  3. end - by the changer after the in-memory change (or after a refused claim): IDLE.
 * Only CHANGING holds arming (CORE-ARM-013's hold), from the alarm task's next engine step until
 * the end: the in-memory change and nothing else. Slow work (store mutex waits, spoken-name
 * invalidation, NVS) stays before step 1 or after step 3. A CHANGING never lapses (the race would
 * come back); one older than ZONE_BIND_GATE_STALL_MS is reported once (zone_bind_gate_stalled).
 *
 * The alarm task brackets every engine step: zone_bind_gate_step_begin before (RESERVED becomes
 * STEPPING; true while CHANGING: hold arming for this step) and zone_bind_gate_step_end after
 * (STEPPING goes back to RESERVED while every partition is still disarmed, else IDLE).
 *
 * A caller that must take another lock before the zone lock (a source's own, e.g. Zigbee's
 * device replacement) reserves first, holding no lock, then takes its lock and the zone lock and
 * claims: the reservation needs no lock, and the claim never waits under either. The Zigbee
 * replacement claims once for all its rebinds, the put-back of a refused one included, and its
 * disruptive requests (a channel change, a network reset) use the same gate (review ZB21).
 *
 * The gate has no owner of its own: end idles whatever it holds. With more than one changer the
 * caller lets one at a time reserve until its end (the panel's alarm_service does), so a refused
 * claim's end never ends another's change.
 *
 * Portable and lock-free of its own: claim, end and the alarm task's calls race on the state, so
 * the caller makes each call atomic with the others (a spinlock around it on the panel).
 */
#ifndef ZONE_BIND_GATE_H
#define ZONE_BIND_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A reservation not claimed within this long is refused: the claim's retries are bounded. */
#define ZONE_BIND_GATE_MS 1000u
/* A change still in progress this long is reported once: arming is held while it lasts. */
#define ZONE_BIND_GATE_STALL_MS 5000u

typedef enum {
    ZONE_BIND_GATE_IDLE = 0,
    ZONE_BIND_GATE_RESERVED, /* every partition disarmed, not yet claimed: never holds arming */
    ZONE_BIND_GATE_STEPPING, /* RESERVED while an engine step runs: can't be claimed now */
    ZONE_BIND_GATE_CHANGING, /* claimed: the change is being made; arming held, never lapses */
} zone_bind_gate_state_t;

typedef enum {
    ZONE_BIND_CLAIMED = 0,  /* change the binding now, then end */
    ZONE_BIND_CLAIM_RETRY,  /* an engine step runs: let the zone lock go, try again shortly */
    ZONE_BIND_CLAIM_REFUSED /* armed meanwhile, or the time is up: end, change nothing */
} zone_bind_claim_t;

/* Why a reservation was or wasn't made (zone_bind_gate_try): a caller answers ARMED with its
 * "not allowed" and may try a BUSY gate again, a bounded number of times (ZB21). */
typedef enum {
    ZONE_BIND_RESERVED = 0, /* reserved: claim it, then end */
    ZONE_BIND_ARMED,        /* a partition is not disarmed: nothing reserved */
    ZONE_BIND_BUSY          /* another change holds the gate: nothing reserved, try again later */
} zone_bind_reserve_t;

typedef struct {
    zone_bind_gate_state_t state;
    uint64_t at;   /* RESERVED/STEPPING: reserved at; CHANGING: claimed at (ms) */
    bool reported; /* CHANGING: its stall was reported */
} zone_bind_gate_t;

void zone_bind_gate_init(zone_bind_gate_t *g);

/* Every enabled partition is disarmed (exit time, armed and entry time are not). */
bool zone_bind_gate_all_disarmed(const alarm_engine_t *e);

/* Step 1, on the alarm task in one engine step: RESERVED when all_disarmed and the gate is
 * idle; false (nothing changed) otherwise. */
bool zone_bind_gate_take(zone_bind_gate_t *g, bool all_disarmed, uint64_t now);

/* zone_bind_gate_take, saying why not: ARMED when a partition is not disarmed (whatever the
 * gate holds), else BUSY when the gate is not idle. */
zone_bind_reserve_t zone_bind_gate_try(zone_bind_gate_t *g, bool all_disarmed, uint64_t now);

/* zone_bind_gate_take with the engine read here. */
bool zone_bind_gate_reserve(zone_bind_gate_t *g, const alarm_engine_t *e, uint64_t now);

/* Step 2, by the changer under the zone lock. */
zone_bind_claim_t zone_bind_gate_claim(zone_bind_gate_t *g, uint64_t now);

/* Step 3, by the changer, claimed or not: the gate is idle again. */
void zone_bind_gate_end(zone_bind_gate_t *g);

/* The alarm task, before each engine step: true when arming is held for this step (a change is
 * being made). A reservation is STEPPING until zone_bind_gate_step_end. */
bool zone_bind_gate_step_begin(zone_bind_gate_t *g);

/* The alarm task, after each engine step, with every partition's state after it: a STEPPING
 * reservation stands again while all are disarmed, else it is cancelled (IDLE). */
void zone_bind_gate_step_end(zone_bind_gate_t *g, bool all_disarmed);

/* The alarm task: true once when a change has been in progress ZONE_BIND_GATE_STALL_MS. */
bool zone_bind_gate_stalled(zone_bind_gate_t *g, uint64_t now);

/* When the alarm task must wake to report a stall; UINT64_MAX when none can be due. */
uint64_t zone_bind_gate_deadline(const zone_bind_gate_t *g);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_BIND_GATE_H */
