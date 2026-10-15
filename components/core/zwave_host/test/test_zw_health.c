/*
 * test_zw_health.c - wake-up handling and supervision per node (zwave_host/src/zw_health.c,
 * docs/features/36 network.md §7-§8; ZW-011, ZW-012). Every test drives a fake clock by passing
 * whatever 'now' it likes to each call - zw_health does no I/O of its own, so there is nothing
 * else to fake.
 */
#include <string.h>

#include "unity.h"
#include "zwave_host/zw_health.h"

void setUp(void) {}
void tearDown(void) {}

#define HOUR_MS (3600ull * 1000ull)
#define MIN_MS (60ull * 1000ull)

/* True if 'acts[0..n)' contains 'kind' for 'node_id' - for a poll() covering several nodes, where
 * more than one can legitimately have something due at the same instant. */
static bool has_action(const zw_health_action_t *acts, size_t n, zw_health_action_kind_t kind,
                        uint16_t node_id)
{
    for (size_t i = 0; i < n; i++) {
        if (acts[i].kind == kind && acts[i].node_id == node_id) {
            return true;
        }
    }
    return false;
}

/* ================================================================================================
 * Interval choice (§7): clamp, round, step 0.
 * ================================================================================================ */

static void test_choose_interval(void)
{
    /* within range, already on the step: unchanged */
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_choose_interval(3600u, 30u, 86400u, 30u));
    /* below the minimum: clamped up */
    TEST_ASSERT_EQUAL_UINT32(30u, zw_health_choose_interval(10u, 30u, 86400u, 30u));
    /* above the maximum: clamped down */
    TEST_ASSERT_EQUAL_UINT32(86400u, zw_health_choose_interval(200000u, 30u, 86400u, 30u));
    /* off the step: rounded to the nearest, relative to the minimum */
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_choose_interval(3612u, 0u, 86400u, 30u));
    TEST_ASSERT_EQUAL_UINT32(3630u, zw_health_choose_interval(3616u, 0u, 86400u, 30u));
    /* rounding never pushes the result back out of range */
    TEST_ASSERT_EQUAL_UINT32(86400u, zw_health_choose_interval(86399u, 0u, 86400u, 30u));
    /* step 0: no rounding, just clamped */
    TEST_ASSERT_EQUAL_UINT32(12345u, zw_health_choose_interval(12345u, 0u, 86400u, 0u));
    /* interval 0 ("wake only by hand") passes straight through, clamp and step do not apply */
    TEST_ASSERT_EQUAL_UINT32(0u, zw_health_choose_interval(0u, 30u, 86400u, 30u));
}

/* ================================================================================================
 * Supervision, sleeping devices (§8's table, row 1) - to the millisecond.
 * ================================================================================================ */

static void test_sleeping_unreachable_at_exactly_t_plus_10min(void)
{
    zw_health_t h;
    zw_health_init(&h);
    TEST_ASSERT_TRUE(zw_health_add(&h, 5u, false, (uint32_t)(HOUR_MS / 1000ull)));

    const uint64_t heard_at = 1000000ull;
    zw_health_action_t act[4];
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_heard(&h, 5u, heard_at, act, 4u)); /* already reachable */

    const uint64_t deadline = heard_at + HOUR_MS + 10ull * MIN_MS;
    TEST_ASSERT_EQUAL_UINT64(deadline, zw_health_next_deadline(&h));

    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, deadline - 1u, act, 4u));
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 5u));

    TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, deadline, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_UNREACHABLE, act[0].kind);
    TEST_ASSERT_EQUAL_UINT16(5u, act[0].node_id);
    TEST_ASSERT_FALSE(zw_health_is_reachable(&h, 5u));

    /* emitted exactly once: polling again at the same or a later time reports nothing more */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, deadline + HOUR_MS, act, 4u));

    /* reachable again on any frame */
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_heard(&h, 5u, deadline + HOUR_MS, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_REACHABLE, act[0].kind);
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 5u));
}

static void test_sleeping_report_counts_as_heard(void)
{
    zw_health_t h;
    zw_health_init(&h);
    TEST_ASSERT_TRUE(zw_health_add(&h, 5u, false, 3600u));
    zw_health_action_t act[4];
    zw_health_heard(&h, 5u, 0u, act, 4u);
    const uint64_t deadline = HOUR_MS + 10ull * MIN_MS;
    /* a fresh frame just before the deadline pushes it out by the same amount again */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_heard(&h, 5u, deadline - 1u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, deadline, act, 4u)); /* would have been due, but reset */
    TEST_ASSERT_EQUAL_UINT64(deadline - 1u + HOUR_MS + 10ull * MIN_MS, zw_health_next_deadline(&h));
}

/* ================================================================================================
 * Supervision, listening devices (§8's table, row 2): the ping at 15 min, the 2nd failure,
 * reachable again.
 * ================================================================================================ */

static void test_listening_two_strikes(void)
{
    zw_health_t h;
    zw_health_init(&h);
    TEST_ASSERT_TRUE(zw_health_add(&h, 9u, true, 0u)); /* interval is meaningless for listening */
    zw_health_action_t act[4];
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_heard(&h, 9u, 0u, act, 4u));

    const uint64_t ping1_due = 15ull * MIN_MS;
    TEST_ASSERT_EQUAL_UINT64(ping1_due, zw_health_next_deadline(&h));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, ping1_due - 1u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, ping1_due, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_PING, act[0].kind);
    /* outstanding: not pinged again while waiting for the outcome */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, ping1_due + HOUR_MS, act, 4u));

    /* 1st failure: no action yet, one more chance 15 min later */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 9u, ZW_HEALTH_PING_TAG, false, ping1_due, act, 4u));
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 9u));
    const uint64_t ping2_due = ping1_due + 15ull * MIN_MS;
    TEST_ASSERT_EQUAL_UINT64(ping2_due, zw_health_next_deadline(&h));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, ping2_due - 1u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, ping2_due, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_PING, act[0].kind);

    /* 2nd failure: declared unreachable now, not before */
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_sent(&h, 9u, ZW_HEALTH_PING_TAG, false, ping2_due, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_UNREACHABLE, act[0].kind);
    TEST_ASSERT_FALSE(zw_health_is_reachable(&h, 9u));

    /* the cycle continues (another ping 15 min later); a success at any point is reachable again */
    const uint64_t ping3_due = ping2_due + 15ull * MIN_MS;
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, ping3_due, act, 4u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_sent(&h, 9u, ZW_HEALTH_PING_TAG, true, ping3_due, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_REACHABLE, act[0].kind);
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 9u));
}

static void test_listening_ack_counts_heard_but_sleeping_ack_does_not(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 9u, true, 0u);
    zw_health_add(&h, 5u, false, 3600u);
    zw_health_action_t act[4];
    zw_health_heard(&h, 9u, 0u, act, 4u);
    zw_health_heard(&h, 5u, 0u, act, 4u);

    /* an ACK just before the listening node's ping would be due: postpones it */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_acked(&h, 9u, 15ull * MIN_MS - 1u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, 15ull * MIN_MS, act, 4u));

    /* an ACK for the sleeping node changes nothing (§8: "every ACK" is listening-only) */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_acked(&h, 5u, 100u, act, 4u));
    const uint64_t sleep_deadline = HOUR_MS + 10ull * MIN_MS; /* unaffected by the ACK at t=100 */
    /* node 9 (listening) is also overdue for a ping by this point: check node 5 specifically */
    const size_t n = zw_health_poll(&h, sleep_deadline, act, 4u);
    TEST_ASSERT_TRUE(has_action(act, n, ZW_HEALTH_ACTION_UNREACHABLE, 5u));
}

/* ================================================================================================
 * Interval 0: never supervised, choose_interval leaves it alone (also covered above), never
 * pinged either.
 * ================================================================================================ */

static void test_interval_zero_never_supervised(void)
{
    zw_health_t h;
    zw_health_init(&h);
    TEST_ASSERT_TRUE(zw_health_add(&h, 5u, false, 0u));
    zw_health_action_t act[4];
    zw_health_heard(&h, 5u, 0u, act, 4u);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zw_health_next_deadline(&h));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, 365ull * 24ull * HOUR_MS, act, 4u));
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 5u));
}

/* ================================================================================================
 * Restart (§7/§8 "nothing heard before counts").
 * ================================================================================================ */

static void test_restart(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u);  /* sleeping */
    zw_health_add(&h, 9u, true, 0u);      /* listening */
    zw_health_action_t act[4];
    zw_health_heard(&h, 5u, 0u, act, 4u);
    zw_health_heard(&h, 9u, 0u, act, 4u);

    /* long silence, as if the panel had been off */
    const uint64_t restart_at = 30ull * 24ull * HOUR_MS;
    zw_health_restart(&h, restart_at);

    /* no action is reported by the restart itself: no new evidence either way */
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 5u));
    TEST_ASSERT_TRUE(zw_health_is_reachable(&h, 9u));

    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, restart_at, act, 4u));

    /* listening: verified soon (the stated fixed delay), not after a full 15 min of silence -
     * checked first, chronologically, so it is consumed (ping_outstanding) before the sleeping
     * node's own later deadline below, keeping the two nodes' checks independent */
    const uint64_t ping_deadline = restart_at + ZW_HEALTH_RESTART_PING_DELAY_MS;
    TEST_ASSERT_TRUE(ZW_HEALTH_RESTART_PING_DELAY_MS < 15ull * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, ping_deadline - 1u, act, 4u));
    size_t n = zw_health_poll(&h, ping_deadline, act, 4u);
    TEST_ASSERT_TRUE(has_action(act, n, ZW_HEALTH_ACTION_PING, 9u));

    /* sleeping: the full T + 10 min again, from the restart - not already overdue */
    const uint64_t sleep_deadline = restart_at + HOUR_MS + 10ull * MIN_MS;
    n = zw_health_poll(&h, sleep_deadline, act, 4u);
    TEST_ASSERT_TRUE(has_action(act, n, ZW_HEALTH_ACTION_UNREACHABLE, 5u));
}

/* ================================================================================================
 * The wake-up queue (§7): 8 max, replacement by kind, interview first, NoAck requeue.
 * ================================================================================================ */

static void test_queue_limit_and_replacement(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u);

    const uint8_t d1[1] = {0x11u};
    for (uint32_t k = 0; k < ZW_HEALTH_QUEUE_PER_NODE_MAX; k++) {
        TEST_ASSERT_TRUE(zw_health_queue_put(&h, 5u, k, false, d1, sizeof(d1)));
    }
    TEST_ASSERT_EQUAL_UINT(ZW_HEALTH_QUEUE_PER_NODE_MAX, zw_health_queue_len(&h, 5u));
    /* the 9th distinct kind is refused: the node's own 8 slots are full */
    TEST_ASSERT_FALSE(zw_health_queue_put(&h, 5u, 999u, false, d1, sizeof(d1)));

    /* a newer command of the same kind replaces the older one, not a 9th entry */
    const uint8_t d2[2] = {0x22u, 0x33u};
    TEST_ASSERT_TRUE(zw_health_queue_put(&h, 5u, 3u, false, d2, sizeof(d2)));
    TEST_ASSERT_EQUAL_UINT(ZW_HEALTH_QUEUE_PER_NODE_MAX, zw_health_queue_len(&h, 5u));

    /* ZW13: what a replacement overwrites can be read first (zw_net tells its owner), and reading
     * it leaves it queued; another node's or an absent kind's is not found */
    uint8_t got[4] = {0};
    size_t got_len = 0u;
    TEST_ASSERT_TRUE(zw_health_queue_peek(&h, 5u, 3u, got, sizeof(got), &got_len));
    TEST_ASSERT_EQUAL_UINT(2u, got_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(d2, got, 2u);
    TEST_ASSERT_TRUE(zw_health_queue_peek(&h, 5u, 3u, got, 1u, &got_len)); /* cut to the room */
    TEST_ASSERT_EQUAL_UINT(2u, got_len);
    TEST_ASSERT_EQUAL_UINT(ZW_HEALTH_QUEUE_PER_NODE_MAX, zw_health_queue_len(&h, 5u));
    TEST_ASSERT_FALSE(zw_health_queue_peek(&h, 6u, 3u, got, sizeof(got), &got_len));
    TEST_ASSERT_FALSE(zw_health_queue_peek(&h, 5u, 999u, got, sizeof(got), &got_len));

    TEST_ASSERT_TRUE(zw_health_queue_cancel(&h, 5u, 0u));
    TEST_ASSERT_EQUAL_UINT(ZW_HEALTH_QUEUE_PER_NODE_MAX - 1u, zw_health_queue_len(&h, 5u));
    TEST_ASSERT_FALSE(zw_health_queue_cancel(&h, 5u, 0u)); /* already gone */

    /* ZW_HEALTH_PING_TAG is reserved: never a valid queue kind */
    TEST_ASSERT_FALSE(zw_health_queue_put(&h, 5u, ZW_HEALTH_PING_TAG, false, d1, sizeof(d1)));
    /* an entry too large for the plaintext pool is refused */
    uint8_t big[ZW_HEALTH_QUEUE_ENTRY_BYTES + 1u];
    memset(big, 0, sizeof(big));
    TEST_ASSERT_FALSE(zw_health_queue_put(&h, 5u, 500u, false, big, sizeof(big)));
}

static void test_queue_interview_first_then_order(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u);
    zw_health_action_t act[4];
    zw_health_wakeup(&h, 5u, 0u, act, 4u); /* nothing queued yet: arms No More Information, that's fine */

    const uint8_t da[1] = {0xAAu};
    const uint8_t db[1] = {0xBBu};
    const uint8_t dc[1] = {0xCCu};
    TEST_ASSERT_TRUE(zw_health_queue_put(&h, 5u, 1u, false, da, 1u)); /* an ordinary command first */
    TEST_ASSERT_TRUE(zw_health_queue_put(&h, 5u, 2u, true, db, 1u));  /* then an interview step */
    TEST_ASSERT_TRUE(zw_health_queue_put(&h, 5u, 3u, false, dc, 1u)); /* then another ordinary one */

    uint32_t kind;
    uint8_t data[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    size_t len;
    bool is_step;

    /* the interview step (kind 2) comes first despite being queued second */
    TEST_ASSERT_TRUE(zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step));
    TEST_ASSERT_EQUAL_UINT32(2u, kind);
    TEST_ASSERT_TRUE(is_step);
    TEST_ASSERT_EQUAL_UINT(1u, len);
    TEST_ASSERT_EQUAL_HEX8(0xBBu, data[0]);

    /* one at a time: nothing else while this one is outstanding */
    TEST_ASSERT_FALSE(zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step));
    /* already reachable (a fresh node starts that way): delivering it reports no new transition */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 5u, 2u, true, 10u, act, 4u));

    /* then the two ordinary ones, oldest first */
    TEST_ASSERT_TRUE(zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step));
    TEST_ASSERT_EQUAL_UINT32(1u, kind);
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 5u, 1u, true, 20u, act, 4u));

    TEST_ASSERT_TRUE(zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step));
    TEST_ASSERT_EQUAL_UINT32(3u, kind);
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 5u, 3u, true, 30u, act, 4u));

    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(&h, 5u));
}

static void test_queue_noack_requeues_sleeping_only(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u); /* sleeping */
    zw_health_add(&h, 9u, true, 0u);     /* listening */
    zw_health_action_t act[4];
    zw_health_wakeup(&h, 5u, 0u, act, 4u);
    zw_health_heard(&h, 9u, 0u, act, 4u);

    const uint8_t d[1] = {0x01u};
    TEST_ASSERT_TRUE(zw_health_queue_put(&h, 5u, 1u, false, d, 1u));
    TEST_ASSERT_TRUE(zw_health_queue_put(&h, 9u, 1u, false, d, 1u));

    uint32_t kind;
    uint8_t data[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    size_t len;
    bool is_step;
    TEST_ASSERT_TRUE(zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step));
    /* a listening node's queue entry is only ever taken once it is "awake" too - give it a wake */
    zw_health_wakeup(&h, 9u, 0u, act, 4u);
    TEST_ASSERT_TRUE(zw_health_queue_take(&h, 9u, &kind, data, sizeof(data), &len, &is_step));

    /* NoAck to the sleeping device: the command goes back in the queue, the node counts as asleep */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 5u, 1u, false, 100u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_queue_len(&h, 5u));
    /* it is available to take again (a fresh wake-up), still there, not lost */
    zw_health_wakeup(&h, 5u, 200u, act, 4u);
    TEST_ASSERT_TRUE(zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step));
    TEST_ASSERT_EQUAL_UINT32(1u, kind);

    /* NoAck to the listening node: not the queue's job to retry it - it is simply gone */
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 9u, 1u, false, 100u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(&h, 9u));
}

/* ================================================================================================
 * Wake Up No More Information: exactly 250 ms, suppressed while interviewing, in repair, or at
 * interval 0.
 * ================================================================================================ */

static void test_no_more_info_at_exactly_250ms(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u);
    zw_health_action_t act[4];
    zw_health_wakeup(&h, 5u, 0u, act, 4u);

    const uint8_t d[1] = {0x01u};
    zw_health_queue_put(&h, 5u, 1u, false, d, 1u);
    uint32_t kind;
    uint8_t data[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    size_t len;
    bool is_step;
    zw_health_queue_take(&h, 5u, &kind, data, sizeof(data), &len, &is_step);

    const uint64_t done_at = 1000u;
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_sent(&h, 5u, 1u, true, done_at, act, 4u)); /* already reachable */
    const uint64_t nmi_due = done_at + ZW_HEALTH_NO_MORE_INFO_MS;
    TEST_ASSERT_EQUAL_UINT64(nmi_due, zw_health_next_deadline(&h));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, nmi_due - 1u, act, 4u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, nmi_due, act, 4u));
    TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_NO_MORE_INFO, act[0].kind);
    TEST_ASSERT_EQUAL_UINT16(5u, act[0].node_id);
}

static void test_no_more_info_suppressed_while_busy_or_interval_zero(void)
{
    zw_health_action_t act[4];

    /* interviewing */
    {
        zw_health_t h;
        zw_health_init(&h);
        zw_health_add(&h, 5u, false, 3600u);
        zw_health_wakeup(&h, 5u, 0u, act, 4u); /* nothing queued: arms it for +250ms right away */
        zw_health_set_busy(&h, 5u, true, false);
        /* next_deadline() still reports the unrelated sleeping-unreachable deadline (interviewing
         * suppresses only No More Information, not supervision) - contributes no *urgent* one */
        TEST_ASSERT_TRUE(zw_health_next_deadline(&h) > ZW_HEALTH_NO_MORE_INFO_MS);
        TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, ZW_HEALTH_NO_MORE_INFO_MS, act, 4u));
        zw_health_set_busy(&h, 5u, false, false);
        TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, ZW_HEALTH_NO_MORE_INFO_MS, act, 4u));
        TEST_ASSERT_EQUAL(ZW_HEALTH_ACTION_NO_MORE_INFO, act[0].kind);
    }
    /* route repair */
    {
        zw_health_t h;
        zw_health_init(&h);
        zw_health_add(&h, 5u, false, 3600u);
        zw_health_wakeup(&h, 5u, 0u, act, 4u);
        zw_health_set_busy(&h, 5u, false, true);
        TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, ZW_HEALTH_NO_MORE_INFO_MS, act, 4u));
        zw_health_set_busy(&h, 5u, false, false);
        TEST_ASSERT_EQUAL_UINT(1u, zw_health_poll(&h, ZW_HEALTH_NO_MORE_INFO_MS, act, 4u));
    }
    /* interval 0: never sent at all */
    {
        zw_health_t h;
        zw_health_init(&h);
        zw_health_add(&h, 5u, false, 0u);
        zw_health_wakeup(&h, 5u, 0u, act, 4u);
        TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zw_health_next_deadline(&h));
        TEST_ASSERT_EQUAL_UINT(0u, zw_health_poll(&h, 365ull * 24ull * HOUR_MS, act, 4u));
    }
}

/* ================================================================================================
 * The late wake-up (§7): more than 5 min later than the interval re-reads it.
 * ================================================================================================ */

static void test_late_wakeup_rereads_interval(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u);
    zw_health_action_t act[4];
    zw_health_heard(&h, 5u, 0u, act, 4u);

    /* on time (within the 5 minute allowance): no re-read */
    const uint64_t on_time = HOUR_MS + 5ull * MIN_MS;
    size_t n = zw_health_wakeup(&h, 5u, on_time, act, 4u);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(ZW_HEALTH_ACTION_REREAD_INTERVAL, act[i].kind);
    }

    zw_health_heard(&h, 5u, 0u, act, 4u); /* reset for a clean second case */
    const uint64_t late = HOUR_MS + 5ull * MIN_MS + 1u; /* one ms past the allowance */
    n = zw_health_wakeup(&h, 5u, late, act, 4u);
    bool saw_reread = false;
    for (size_t i = 0; i < n; i++) {
        if (act[i].kind == ZW_HEALTH_ACTION_REREAD_INTERVAL) {
            saw_reread = true;
        }
    }
    TEST_ASSERT_TRUE(saw_reread);
}

static void test_wakeup_reports_send_queue_when_something_is_queued(void)
{
    zw_health_t h;
    zw_health_init(&h);
    zw_health_add(&h, 5u, false, 3600u);
    const uint8_t d[1] = {0x01u};
    zw_health_queue_put(&h, 5u, 1u, false, d, 1u);

    zw_health_action_t act[4];
    const size_t n = zw_health_wakeup(&h, 5u, 0u, act, 4u);
    bool saw_send_queue = false;
    for (size_t i = 0; i < n; i++) {
        if (act[i].kind == ZW_HEALTH_ACTION_SEND_QUEUE) {
            saw_send_queue = true;
        }
    }
    TEST_ASSERT_TRUE(saw_send_queue);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_choose_interval);
    RUN_TEST(test_sleeping_unreachable_at_exactly_t_plus_10min);
    RUN_TEST(test_sleeping_report_counts_as_heard);
    RUN_TEST(test_listening_two_strikes);
    RUN_TEST(test_listening_ack_counts_heard_but_sleeping_ack_does_not);
    RUN_TEST(test_interval_zero_never_supervised);
    RUN_TEST(test_restart);
    RUN_TEST(test_queue_limit_and_replacement);
    RUN_TEST(test_queue_interview_first_then_order);
    RUN_TEST(test_queue_noack_requeues_sleeping_only);
    RUN_TEST(test_no_more_info_at_exactly_250ms);
    RUN_TEST(test_no_more_info_suppressed_while_busy_or_interval_zero);
    RUN_TEST(test_late_wakeup_rereads_interval);
    RUN_TEST(test_wakeup_reports_send_queue_when_something_is_queued);
    return UNITY_END();
}
