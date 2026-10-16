/*
 * plink_vfeed.h - a keypad's voice, fed from the panel link (docs/features/11 LINK-028;
 * docs/features/08 voice-first-build.md D05): which of the master's pushed events, sounds and
 * status the keypad's voice hears, and when a batch of them closes.
 *
 * On an alarm panel the alarm task hands the voice each engine call's facts - its events, its
 * one-shot sounds, its outputs - and then the state after the call (the commit). A keypad has no
 * engine: the same facts come over the link in separate datagrams, in either order, some of them
 * lost or filled in later. This unit turns what the keypad's link manager holds into the same
 * calls (the port below), by these rules:
 *   - Live only while the link is ONLINE. Leaving it pauses the voice (speech cancelled, nothing
 *     collected). Entering it, a new home partition, a master's log that started again (the
 *     snapshot's last_event_seq below the floor) or the voice's own request gives a silent
 *     baseline; its last_event_seq is the floor.
 *   - An event is fed only once a snapshot covers it (floor < seq <= last_event_seq), in seq
 *     order and once; then the floor moves to last_event_seq. A backfill, a catch-up or a hole
 *     filled after its batch is at or below the floor and never speaks, and neither does an event
 *     the master stamped more than PLINK_VFEED_STALE_MS before the master's time now (a link that
 *     dropped every frame for a while without going LOST): loss means a missing announcement,
 *     never a late one.
 *   - The commit is held, PLINK_VFEED_HOLD_MS at most, while pushed events are ahead of the
 *     snapshot (the STATUS that covers them is on its way) or covered ones are missing from the
 *     ring (a pull is under way).
 *   - A batch with events and no sound yet, or a sound and no events yet, waits
 *     PLINK_VFEED_SETTLE_MS for the rest (an ACK or ERROR that follows its STATUS; a chime has no
 *     STATUS at all); once it has both it commits at once, and a snapshot that brings no new fact
 *     commits at once.
 *   - A WALK_TEST_START of the home partition is fed with the bell test before it, as the engine
 *     emits it (there is no OUTPUT frame).
 *
 * Plain C11, no heap, no globals (components/core rules); one task owns a plink_vfeed_t (the link
 * task on a panel). The port's calls hand over and return: they never call back in.
 */
#ifndef PLINK_VFEED_H
#define PLINK_VFEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "alarm/alarm_types.h"
#include "panel_link/plink_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_VFEED_HOLD_MS   300u  /* about one EV_REQ round */
#define PLINK_VFEED_SETTLE_MS 100u  /* the master's STATUS gap: a late ACK or ERROR joins */
#define PLINK_VFEED_STALE_MS  5000u /* the voice's longest wait for a request (AUD-012) */

/* What the keypad's voice is told, as an alarm panel's alarm task tells it. */
typedef struct {
    void *ctx;
    /* A silent baseline for this partition (the keypad's home) from this state. */
    void (*baseline)(void *ctx, uint8_t partition, const alarm_snapshot_t *snap);
    /* The link is not ONLINE: cancel speech, collect nothing until the next baseline. */
    void (*pause)(void *ctx);
    void (*event)(void *ctx, const alarm_event_t *ev);
    /* ticket: the sounder's for the tone (0: not tracked or not admitted). */
    void (*sound)(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone,
                  uint32_t ticket);
    void (*output)(void *ctx, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);
    /* The state after the batch (the commit). */
    void (*snapshot)(void *ctx, const alarm_snapshot_t *snap);
} plink_vfeed_port_t;

/* What the keypad holds now (plink_keypad_info, plink_keypad_snapshot, plink_keypad_events). */
typedef struct {
    bool online;                  /* the link is ONLINE */
    uint8_t home;                 /* the keypad's home partition (1..ALARM_MAX_PARTITIONS) */
    const alarm_snapshot_t *snap; /* its rebuilt snapshot (seq, last_event_seq) */
    const alarm_event_t *ev;      /* its events, newest first */
    size_t n_ev;
    int64_t epoch_ms;             /* the master's clock now (plink_keypad_epoch_ms), 0 unknown */
    bool resync;                  /* the voice asked for a baseline */
} plink_vfeed_view_t;

typedef struct {
    /* private */
    plink_vfeed_port_t port;
    bool live;           /* a baseline was given: facts flow */
    uint8_t home;        /* the baseline's partition */
    uint32_t floor;      /* the newest event seq accounted for */
    uint32_t snap_seq;   /* the snapshot last committed (or the baseline's) */
    bool batch_sound;    /* a sound was fed since the last commit */
    bool batch_events;   /* events were fed since the last commit */
    uint64_t settle_at;  /* the open batch commits no sooner */
    bool holding;
    bool hold_spent;     /* the hold ran out: none again until its cause is gone */
    uint64_t hold_since;
    /* counters (tests, the console) */
    uint32_t baselines, pauses, events, sounds, commits, stale;
} plink_vfeed_t;

void plink_vfeed_init(plink_vfeed_t *f, const plink_vfeed_port_t *port);

/* Facts flow (the last poll gave a baseline and the link is still taken as ONLINE). The caller
 * plays a sound tracked only then, so no ticket is made that nobody reads. */
bool plink_vfeed_live(const plink_vfeed_t *f);

/* A one-shot sound from the master, just played (ticket: sounder_play_tracked's). Fed at once
 * while live; dropped otherwise. */
void plink_vfeed_sound(plink_vfeed_t *f, uint64_t now_ms, const plink_sound_t *s, uint32_t ticket);

/* Looks at what the keypad holds now: pause, baseline, feed covered events, commit. Every turn of
 * the owning task (after the sounds it took). */
void plink_vfeed_poll(plink_vfeed_t *f, uint64_t now_ms, const plink_vfeed_view_t *v);

/* When the next poll is due for a hold or a settle to end (UINT64_MAX: nothing waits). */
uint64_t plink_vfeed_next(const plink_vfeed_t *f);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_VFEED_H */
