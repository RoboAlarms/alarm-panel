/*
 * test_plink_vfeed.c - what a keypad's voice hears of the panel link (plink_vfeed.h): the master's
 * pushed events, sounds and status turned into the calls an alarm panel's alarm task makes -
 * baseline, event, sound, output, commit - on a fake keypad (its snapshot, its event ring, its
 * link state) and a recording port. Nothing before the first ONLINE; a baseline's floor that no
 * backfill, catch-up or late hole fill passes; covered events only, in seq order, once; the hold
 * for events ahead of their STATUS and for a hole, 300 ms at most; the settle that lets an ACK or
 * ERROR join its batch and commits a chime; LOST, a new home, a master's log started again and
 * the voice's own request; the walk test's bell; stale events; an exit time's end long after it;
 * the deadlines.
 * Requirements: LINK-028 (docs/features/11); WALK-003's bell test before the walk-test start
 * (docs/features/40), D05 (docs/features/08 voice-first-build.md).
 */
#include <string.h>

#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_keypad.h"
#include "panel_link/plink_vfeed.h"
#include "unity.h"

#define EPOCH0 1790000000000ll
#define MONO0  7000000ull /* the master's engine time when the keypad's clock reads 0 */

/* ---- the recording port ---------------------------------------------------------------------- */

typedef enum { C_BASELINE = 1, C_PAUSE, C_EVENT, C_SOUND, C_OUTPUT, C_SNAPSHOT } call_t;

typedef struct {
    uint8_t call;
    uint8_t partition;
    uint32_t seq;      /* an event's seq; a snapshot's (or baseline's) last_event_seq */
    uint32_t snap_seq; /* a snapshot's seq */
    uint16_t type;
    uint8_t sound;
    uint16_t zone;
    uint32_t ticket;
    uint8_t output, pattern;
} rec_t;

static rec_t LOG[256];
static unsigned N;

static rec_t *add(uint8_t call)
{
    TEST_ASSERT_TRUE(N < sizeof LOG / sizeof LOG[0]);
    rec_t *r = &LOG[N++];
    memset(r, 0, sizeof *r);
    r->call = call;
    return r;
}

static void p_baseline(void *ctx, uint8_t partition, const alarm_snapshot_t *snap)
{
    (void)ctx;
    rec_t *r = add(C_BASELINE);
    r->partition = partition;
    r->seq = snap->last_event_seq;
    r->snap_seq = snap->seq;
}

static void p_pause(void *ctx)
{
    (void)ctx;
    (void)add(C_PAUSE);
}

static void p_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    rec_t *r = add(C_EVENT);
    r->seq = ev->seq;
    r->type = (uint16_t)ev->type;
    r->partition = ev->partition;
}

static void p_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone,
                    uint32_t ticket)
{
    (void)ctx;
    rec_t *r = add(C_SOUND);
    r->partition = partition;
    r->sound = (uint8_t)sound;
    r->zone = zone;
    r->ticket = ticket;
}

static void p_output(void *ctx, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    (void)ctx;
    rec_t *r = add(C_OUTPUT);
    r->output = (uint8_t)output;
    r->partition = partition;
    r->pattern = (uint8_t)pattern;
}

static void p_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    rec_t *r = add(C_SNAPSHOT);
    r->seq = snap->last_event_seq;
    r->snap_seq = snap->seq;
}

/* ---- the fake keypad ------------------------------------------------------------------------- */

static plink_vfeed_t F;
static uint64_t T;
static bool ONLINE;
static uint8_t HOME;
static bool RESYNC;
static bool CLOCK; /* the master's wall clock is known */
static bool MONO; /* the STATUS carries the master's engine time */
static alarm_snapshot_t SNAP;
static alarm_event_t RING[PLINK_EVENTS_KEPT]; /* oldest first, as the keypad keeps them */
static size_t NR;
static alarm_event_t VIEW[2u * PLINK_EVENTS_KEPT]; /* newest first */

static int64_t epoch_now(void)
{
    return EPOCH0 + (int64_t)T;
}

static uint64_t mono_now(void)
{
    return MONO0 + T;
}

/* An event the master pushed (or a pull brought), stamped `age_ms` before now. */
static void got_aged(uint32_t seq, alarm_event_type_t type, uint8_t partition, uint64_t age_ms)
{
    TEST_ASSERT_TRUE(NR < PLINK_EVENTS_KEPT);
    size_t i = NR;
    while (i > 0u && RING[i - 1u].seq > seq) {
        i--;
    }
    memmove(&RING[i + 1u], &RING[i], (NR - i) * sizeof RING[0]);
    memset(&RING[i], 0, sizeof RING[0]);
    RING[i].seq = seq;
    RING[i].type = type;
    RING[i].partition = partition;
    RING[i].epoch_ms = epoch_now() - (int64_t)age_ms;
    RING[i].mono_ms = mono_now() - age_ms;
    NR++;
}

static void got(uint32_t seq, alarm_event_type_t type)
{
    got_aged(seq, type, 1u, 0u);
}

/* A STATUS applied: the snapshot's seq rises, its last_event_seq says what it covers, and it was
 * taken now (the keypad's clock, as plink_view rebuilds it). */
static void status(uint32_t last)
{
    SNAP.last_event_seq = last;
    SNAP.seq++;
    SNAP.taken_ms = T;
}

static void poll(void)
{
    for (size_t i = 0; i < NR; i++) {
        VIEW[i] = RING[NR - 1u - i];
    }
    const plink_vfeed_view_t v = {ONLINE, HOME,   &SNAP, VIEW, NR, CLOCK ? epoch_now() : 0,
                                  RESYNC, MONO ? mono_now() : 0u};
    RESYNC = false;
    plink_vfeed_poll(&F, T, &v);
}

static void sound(alarm_sound_t s, uint16_t zone, uint32_t ticket)
{
    const plink_sound_t m = {HOME, (uint8_t)s, zone};
    plink_vfeed_sound(&F, T, &m, ticket);
}

static void expect_calls(unsigned n)
{
    TEST_ASSERT_EQUAL_UINT(n, N);
}

static void expect_event(unsigned i, uint32_t seq)
{
    TEST_ASSERT_TRUE(i < N);
    TEST_ASSERT_EQUAL_UINT8(C_EVENT, LOG[i].call);
    TEST_ASSERT_EQUAL_UINT32(seq, LOG[i].seq);
}

static unsigned count(uint8_t call)
{
    unsigned c = 0;
    for (unsigned i = 0; i < N; i++) {
        c += LOG[i].call == call;
    }
    return c;
}

/* An ONLINE keypad of partition 1 holding events 1..10 (a backfill), baselined. */
static void online_at_10(void)
{
    for (uint32_t s = 1u; s <= 10u; s++) {
        got(s, ALARM_EV_DISARMED);
    }
    status(10u);
    ONLINE = true;
    poll();
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[N - 1u].call);
    N = 0u;
}

void setUp(void)
{
    const plink_vfeed_port_t port = {NULL,        p_baseline, p_pause,   p_event,
                                     p_sound,     p_output,   p_snapshot};
    plink_vfeed_init(&F, &port);
    memset(LOG, 0, sizeof LOG);
    N = 0u;
    T = 100000u;
    ONLINE = false;
    HOME = 1u;
    RESYNC = false;
    CLOCK = true;
    MONO = true;
    memset(&SNAP, 0, sizeof SNAP);
    SNAP.partitions = 0x01u;
    NR = 0u;
}

void tearDown(void) {}

/* ---- tests ----------------------------------------------------------------------------------- */

/* LINK-028: nothing is said, paused or baselined before the link is first ONLINE; a sound then
 * is dropped, and nothing waits. */
static void test_nothing_before_the_first_online(void)
{
    got(1u, ALARM_EV_ARMED);
    status(1u);
    for (int i = 0; i < 5; i++) {
        poll();
        sound(ALARM_SOUND_ACK, 0u, 3u);
        T += 100u;
    }
    expect_calls(0u);
    TEST_ASSERT_FALSE(plink_vfeed_live(&F));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
    /* a home that is no partition is as good as offline */
    ONLINE = true;
    HOME = 0u;
    poll();
    expect_calls(0u);
    TEST_ASSERT_FALSE(plink_vfeed_live(&F));
}

/* LINK-028: entering ONLINE gives one silent baseline of the home partition; its last_event_seq
 * is the floor, and nothing at or below it is ever fed - the backfill it came with, a catch-up,
 * a hole filled after it. */
static void test_baseline_sets_the_floor(void)
{
    for (uint32_t s = 1u; s <= 10u; s++) {
        got(s, ALARM_EV_ARMED);
    }
    status(10u);
    ONLINE = true;
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[0].call);
    TEST_ASSERT_EQUAL_UINT8(1u, LOG[0].partition);
    TEST_ASSERT_EQUAL_UINT32(10u, LOG[0].seq);
    TEST_ASSERT_TRUE(plink_vfeed_live(&F));
    T += 1000u;
    poll();
    expect_calls(1u); /* nothing new: nothing fed, no commit */
    /* a STATUS repeated: a commit of the same state, no event */
    status(10u);
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[1].call);
    TEST_ASSERT_EQUAL_UINT(0u, count(C_EVENT));
    /* the ring loses its oldest and an old one comes back (a pull's answer): never fed */
    NR = 0u;
    for (uint32_t s = 4u; s <= 10u; s++) {
        got(s, ALARM_EV_DISARMED);
    }
    status(10u);
    poll();
    TEST_ASSERT_EQUAL_UINT(0u, count(C_EVENT));
    /* the next new one is */
    got(11u, ALARM_EV_DISARMED);
    status(11u);
    poll();
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    TEST_ASSERT_EQUAL_UINT(1u, count(C_EVENT));
    expect_event(N - 2u, 11u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[N - 1u].call);
}

/* LINK-028: events pushed ahead of their STATUS wait for it; then they are fed oldest first, each
 * once - a repeated STATUS, the same seq twice in the ring's copy - and the batch commits. */
static void test_covered_events_in_order_once(void)
{
    online_at_10();
    got(11u, ALARM_EV_ARMED);
    got(12u, ALARM_EV_BYPASS);
    poll(); /* ahead of the snapshot: held */
    T += 50u;
    poll();
    expect_calls(0u);
    status(12u);
    poll();
    expect_calls(2u);
    expect_event(0u, 11u);
    expect_event(1u, 12u);
    /* no sound came with them: an ACK may still follow its STATUS */
    T += PLINK_VFEED_SETTLE_MS - 1u;
    poll();
    expect_calls(2u);
    T += 1u;
    poll();
    expect_calls(3u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[2].call);
    TEST_ASSERT_EQUAL_UINT32(12u, LOG[2].seq);
    /* repeated STATUS: one commit each, no event again */
    status(12u);
    poll();
    status(12u);
    poll();
    TEST_ASSERT_EQUAL_UINT(2u, count(C_EVENT));
    TEST_ASSERT_EQUAL_UINT(3u, count(C_SNAPSHOT));
    /* the same seq twice, out of order, in what the keypad hands over: once, in order */
    N = 0u;
    got(13u, ALARM_EV_DISARMED);
    got(14u, ALARM_EV_ENTRY_DELAY);
    status(14u);
    VIEW[0] = RING[NR - 2u]; /* 13 */
    VIEW[1] = RING[NR - 1u]; /* 14 */
    VIEW[2] = RING[NR - 2u]; /* 13 again */
    const plink_vfeed_view_t v = {true, 1u, &SNAP, VIEW, 3u, epoch_now(), false, mono_now()};
    plink_vfeed_poll(&F, T, &v);
    expect_calls(2u);
    expect_event(0u, 13u);
    expect_event(1u, 14u);
}

/* LINK-028: a covered event the ring lacks holds the batch while a pull fills it - fed in order
 * when it comes in time; after 300 ms what is there goes, and the late one is never said. */
static void test_missing_event_holds_then_is_never_said(void)
{
    online_at_10();
    /* filled within the hold: both, in order */
    got(12u, ALARM_EV_ARMED);
    status(12u);
    poll();
    expect_calls(0u);
    T += 120u;
    got(11u, ALARM_EV_BYPASS); /* the pull's answer */
    poll();
    expect_calls(2u);
    expect_event(0u, 11u);
    expect_event(1u, 12u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    expect_calls(3u);
    /* not filled: 300 ms, then the rest without it */
    N = 0u;
    got(14u, ALARM_EV_DISARMED);
    status(14u);
    poll();
    T += PLINK_VFEED_HOLD_MS - 1u;
    poll();
    expect_calls(0u);
    T += 1u;
    poll();
    expect_calls(1u);
    expect_event(0u, 14u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[1].call);
    /* it comes at last: below the floor, never said */
    got(13u, ALARM_EV_ARMED);
    for (int i = 0; i < 5; i++) {
        T += 100u;
        poll();
    }
    expect_calls(2u);
    /* and a later batch holds again */
    N = 0u;
    got(16u, ALARM_EV_DISARMED);
    status(16u);
    poll();
    expect_calls(0u);
}

/* LINK-028: an ACK that comes before its STATUS (the master's order: EVENTS, SOUND, STATUS) is
 * fed at once and holds the commit until the STATUS covers its event, which then commits at once;
 * an ERROR that comes after its STATUS (an older master) joins the batch and closes it. */
static void test_sound_before_its_status_holds_the_commit(void)
{
    online_at_10();
    got(11u, ALARM_EV_ARMED);
    sound(ALARM_SOUND_ACK, 0u, 5u);
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_SOUND, LOG[0].call);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_ACK, LOG[0].sound);
    TEST_ASSERT_EQUAL_UINT32(5u, LOG[0].ticket);
    poll();
    T += 150u; /* past the settle: the event ahead still holds it */
    poll();
    expect_calls(1u);
    status(11u);
    poll();
    expect_calls(3u);
    expect_event(1u, 11u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[2].call);
    TEST_ASSERT_EQUAL_UINT32(11u, LOG[2].seq);
    /* the STATUS first, its ERROR 40 ms later */
    N = 0u;
    got(12u, ALARM_EV_ARM_FAILED);
    status(12u);
    poll();
    expect_calls(1u);
    T += 40u;
    sound(ALARM_SOUND_ERROR, 0u, 6u);
    poll();
    expect_calls(3u);
    TEST_ASSERT_EQUAL_UINT8(C_SOUND, LOG[1].call);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[2].call);
}

/* LINK-028: a chime is a SOUND with no STATUS: it commits once the settle is over. */
static void test_sound_alone_commits_after_the_settle(void)
{
    online_at_10();
    sound(ALARM_SOUND_CHIME, 3u, 9u);
    TEST_ASSERT_EQUAL_UINT64(T + PLINK_VFEED_SETTLE_MS, plink_vfeed_next(&F));
    poll();
    T += PLINK_VFEED_SETTLE_MS - 1u;
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT16(3u, LOG[0].zone);
    T += 1u;
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[1].call);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
}

/* LINK-028: a snapshot that brings no new fact (exit time ending: "Armed away") commits at
 * once. */
static void test_snapshot_alone_commits_at_once(void)
{
    online_at_10();
    SNAP.part[0].state = ALARM_STATE_ARMED;
    status(10u);
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[0].call);
    TEST_ASSERT_EQUAL_UINT32(SNAP.seq, LOG[0].snap_seq);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
    poll();
    expect_calls(1u);
}

/* LINK-028: leaving ONLINE pauses the voice once and drops whatever was held; a sound while LOST
 * is dropped; ONLINE again gives a new baseline whose floor covers what came meanwhile. */
static void test_lost_pauses_once_and_online_baselines_again(void)
{
    online_at_10();
    got(11u, ALARM_EV_ARMED);
    sound(ALARM_SOUND_ACK, 0u, 5u);
    poll(); /* held: its STATUS is lost with the link */
    expect_calls(1u);
    ONLINE = false;
    poll();
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_PAUSE, LOG[1].call);
    TEST_ASSERT_FALSE(plink_vfeed_live(&F));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
    sound(ALARM_SOUND_CHIME, 2u, 6u);
    expect_calls(2u);
    /* back: the catch-up and its STATUS came in while it reconnected */
    got(12u, ALARM_EV_DISARMED);
    status(12u);
    T += 40000u;
    ONLINE = true;
    poll();
    expect_calls(3u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[2].call);
    TEST_ASSERT_EQUAL_UINT32(12u, LOG[2].seq);
    for (int i = 0; i < 5; i++) {
        T += 100u;
        poll();
    }
    expect_calls(3u);
    TEST_ASSERT_EQUAL_UINT(0u, count(C_EVENT));
}

/* LINK-028 (D05: a keypad speaks for its home partition): moved to another home, a baseline of
 * the new one; a master's log that started again (last_event_seq below the floor), a baseline
 * whose floor is the new last_event_seq. */
static void test_new_home_or_log_restart_baselines(void)
{
    online_at_10();
    HOME = 2u;
    SNAP.partitions = 0x03u;
    status(10u);
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[0].call);
    TEST_ASSERT_EQUAL_UINT8(2u, LOG[0].partition);
    /* the log starts again: the ring is dropped, the STATUS says 2 */
    N = 0u;
    NR = 0u;
    got(1u, ALARM_EV_DISARMED);
    got(2u, ALARM_EV_ARMED);
    status(2u);
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[0].call);
    TEST_ASSERT_EQUAL_UINT32(2u, LOG[0].seq);
    got(3u, ALARM_EV_DISARMED);
    status(3u);
    poll();
    expect_calls(2u);
    expect_event(1u, 3u);
}

/* LINK-028: the voice's own request for a baseline (after its queue overflowed) is answered only
 * while ONLINE: entering ONLINE gives one anyway. */
static void test_resync_is_a_baseline_only_while_online(void)
{
    RESYNC = true;
    poll();
    expect_calls(0u);
    online_at_10();
    RESYNC = true;
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[0].call);
    ONLINE = false;
    poll();
    RESYNC = true;
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_PAUSE, LOG[1].call);
}

/* WALK-003, LINK-028: a walk test started on the home partition comes with the bell's test pulse
 * just before its event, as the engine emits it ("Walk test started" waits for it, WALK-016);
 * one on another partition comes without. */
static void test_walk_test_start_gets_the_bell_first(void)
{
    online_at_10();
    got(11u, ALARM_EV_WALK_TEST_START);
    got_aged(12u, ALARM_EV_WALK_TEST_START, 2u, 0u);
    status(12u);
    poll();
    expect_calls(3u);
    TEST_ASSERT_EQUAL_UINT8(C_OUTPUT, LOG[0].call);
    TEST_ASSERT_EQUAL_UINT8(ALARM_OUT_BELL, LOG[0].output);
    TEST_ASSERT_EQUAL_UINT8(1u, LOG[0].partition);
    TEST_ASSERT_EQUAL_UINT8(ALARM_PAT_TEST, LOG[0].pattern);
    expect_event(1u, 11u);
    expect_event(2u, 12u);
}

/* LINK-028: an event the master made more than 5 s before its time now (every frame lost for a
 * while, the link not LOST) is never said, and the floor moves past it: by the master's engine
 * time, which every STATUS carries, the wall clock not needed; by the wall clock when a master
 * sends no engine time; an event from before the master's restart (ahead of its time now) is no
 * news either; with neither, the events' age can't be known. */
static void test_stale_events_are_never_said(void)
{
    online_at_10();
    got_aged(11u, ALARM_EV_ARMED, 1u, PLINK_VFEED_STALE_MS + 1000u);
    got(12u, ALARM_EV_DISARMED);
    status(12u);
    poll();
    expect_calls(1u);
    expect_event(0u, 12u);
    TEST_ASSERT_EQUAL_UINT32(1u, F.stale);
    /* the engine time alone */
    CLOCK = false;
    got_aged(13u, ALARM_EV_ARMED, 1u, 60000u);
    got(14u, ALARM_EV_DISARMED);
    status(14u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    TEST_ASSERT_EQUAL_UINT32(2u, F.stale);
    expect_event(N - 1u, 14u);
    /* the wall clock alone */
    CLOCK = true;
    MONO = false;
    got_aged(15u, ALARM_EV_ARMED, 1u, 60000u);
    status(15u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    TEST_ASSERT_EQUAL_UINT32(3u, F.stale);
    /* from before the master's restart: its engine time is ahead of the master's now */
    MONO = true;
    got_aged(16u, ALARM_EV_ARMED, 1u, 0u);
    RING[NR - 1u].mono_ms = mono_now() + PLINK_VFEED_STALE_MS + 1000u;
    status(16u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    TEST_ASSERT_EQUAL_UINT32(4u, F.stale);
    TEST_ASSERT_EQUAL_UINT(2u, count(C_EVENT));
    /* both: the engine time decides - a fresh event with a wall clock gone wrong is said */
    CLOCK = true;
    got_aged(17u, ALARM_EV_DISARMED, 1u, 0u);
    RING[NR - 1u].epoch_ms = epoch_now() - 60000;
    status(17u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    TEST_ASSERT_EQUAL_UINT32(4u, F.stale);
    TEST_ASSERT_EQUAL_UINT(3u, count(C_EVENT));
    /* neither: no way to tell */
    CLOCK = false;
    MONO = false;
    got_aged(18u, ALARM_EV_ARMED, 1u, 60000u);
    status(18u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    TEST_ASSERT_EQUAL_UINT(4u, count(C_EVENT));
    expect_event(N - 1u, 18u);
}

/* LINK-028: the end of an exit time has no event; the commit that brings it more than 5 s after
 * the keypad's own countdown ended (the STATUS saying so and both its repeats lost) is a silent
 * baseline. In time, it commits; a baseline taken in the exit time counts down too. */
static void test_an_exit_end_long_after_is_a_silent_baseline(void)
{
    online_at_10();
    SNAP.part[0].state = ALARM_STATE_EXIT_DELAY;
    SNAP.part[0].delay_remaining_ms = 30000u;
    status(10u);
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[0].call);
    /* the exit time ended 6 s ago by the keypad's countdown */
    T += 30000u + PLINK_VFEED_STALE_MS + 1000u;
    SNAP.part[0].state = ALARM_STATE_ARMED;
    SNAP.part[0].delay_remaining_ms = 0u;
    status(10u);
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[1].call);
    TEST_ASSERT_EQUAL_UINT32(1u, F.late);
    /* a later STATUS of the armed state: a commit */
    T += 10000u;
    status(10u);
    poll();
    expect_calls(3u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[2].call);

    /* in time: a baseline in an exit time, then its end 1 s after the countdown's */
    N = 0u;
    SNAP.part[0].state = ALARM_STATE_EXIT_DELAY;
    SNAP.part[0].delay_remaining_ms = 20000u;
    status(10u);
    RESYNC = true;
    poll();
    expect_calls(1u);
    TEST_ASSERT_EQUAL_UINT8(C_BASELINE, LOG[0].call);
    T += 21000u;
    SNAP.part[0].state = ALARM_STATE_ARMED;
    SNAP.part[0].delay_remaining_ms = 0u;
    status(10u);
    poll();
    expect_calls(2u);
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[1].call);
    TEST_ASSERT_EQUAL_UINT32(1u, F.late);

    /* an exit time restarted (CP-01: in again while it runs): the commit that brings its longer
     * countdown moves the end, so Armed at the new end is in time */
    N = 0u;
    SNAP.part[0].state = ALARM_STATE_EXIT_DELAY;
    SNAP.part[0].delay_remaining_ms = 30000u;
    status(10u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    T += 25000u;
    SNAP.part[0].delay_remaining_ms = 60000u;
    status(10u);
    poll();
    T += 60000u + 200u;
    SNAP.part[0].state = ALARM_STATE_ARMED;
    SNAP.part[0].delay_remaining_ms = 0u;
    status(10u);
    poll();
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[N - 1u].call);
    TEST_ASSERT_EQUAL_UINT32(1u, F.late);

    /* at the edge: 4.9 s after the countdown's end it commits, 5.1 s after it is a baseline */
    for (int late = 0; late <= 1; late++) {
        SNAP.part[0].state = ALARM_STATE_EXIT_DELAY;
        SNAP.part[0].delay_remaining_ms = 10000u;
        status(10u);
        T += PLINK_VFEED_SETTLE_MS;
        poll();
        T += 10000u + (late ? PLINK_VFEED_STALE_MS + 100u : PLINK_VFEED_STALE_MS - 100u);
        SNAP.part[0].state = ALARM_STATE_ARMED;
        SNAP.part[0].delay_remaining_ms = 0u;
        status(10u);
        poll();
        TEST_ASSERT_EQUAL_UINT8(late ? C_BASELINE : C_SNAPSHOT, LOG[N - 1u].call);
        TEST_ASSERT_EQUAL_UINT32(1u + (uint32_t)late, F.late);
    }

    /* disarmed in the exit time, then armed at once long after: no exit time ends, a commit */
    SNAP.part[0].state = ALARM_STATE_EXIT_DELAY;
    SNAP.part[0].delay_remaining_ms = 10000u;
    status(10u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    SNAP.part[0].state = ALARM_STATE_DISARMED;
    SNAP.part[0].delay_remaining_ms = 0u;
    status(10u);
    T += PLINK_VFEED_SETTLE_MS;
    poll();
    T += 60000u;
    SNAP.part[0].state = ALARM_STATE_ARMED;
    status(10u);
    poll();
    TEST_ASSERT_EQUAL_UINT8(C_SNAPSHOT, LOG[N - 1u].call);
    TEST_ASSERT_EQUAL_UINT32(2u, F.late);
}

/* LINK-028: plink_vfeed_next says when a hold or a settle ends, nothing otherwise. */
static void test_next_is_the_hold_or_the_settle(void)
{
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
    online_at_10();
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
    got(11u, ALARM_EV_ARMED);
    poll();
    const uint64_t t0 = T;
    TEST_ASSERT_EQUAL_UINT64(t0 + PLINK_VFEED_HOLD_MS, plink_vfeed_next(&F));
    T += 100u;
    status(11u);
    poll();
    TEST_ASSERT_EQUAL_UINT64(T + PLINK_VFEED_SETTLE_MS, plink_vfeed_next(&F));
    T += 30u;
    sound(ALARM_SOUND_ACK, 0u, 2u);
    TEST_ASSERT_EQUAL_UINT64(T, plink_vfeed_next(&F)); /* the batch is whole */
    poll();
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, plink_vfeed_next(&F));
    TEST_ASSERT_EQUAL_UINT32(1u, F.commits);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_before_the_first_online);
    RUN_TEST(test_baseline_sets_the_floor);
    RUN_TEST(test_covered_events_in_order_once);
    RUN_TEST(test_missing_event_holds_then_is_never_said);
    RUN_TEST(test_sound_before_its_status_holds_the_commit);
    RUN_TEST(test_sound_alone_commits_after_the_settle);
    RUN_TEST(test_snapshot_alone_commits_at_once);
    RUN_TEST(test_lost_pauses_once_and_online_baselines_again);
    RUN_TEST(test_new_home_or_log_restart_baselines);
    RUN_TEST(test_resync_is_a_baseline_only_while_online);
    RUN_TEST(test_walk_test_start_gets_the_bell_first);
    RUN_TEST(test_stale_events_are_never_said);
    RUN_TEST(test_an_exit_end_long_after_is_a_silent_baseline);
    RUN_TEST(test_next_is_the_hold_or_the_settle);
    return UNITY_END();
}
