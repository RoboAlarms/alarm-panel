/*
 * test_report.c - store-and-forward of reportable events to the broker (alarm_proto
 * src/report.c) on a real event log in RAM and a scripted broker.
 * Requirements: MQTT-005, MQTT-006, MQTT-011, MQTT-017 (docs/features/10), LOG-006 (docs/features/05)
 */
#include <string.h>

#include "alarm_proto/proto_report.h"
#include "unity.h"

#define SECTOR 4096u
#define SIZE   (SECTOR * 4u)

static uint8_t MEM[SIZE];
static elog_t LOG;

static int f_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    memcpy(buf, MEM + off, len);
    return 0;
}

static int f_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    const uint8_t *d = buf;
    for (uint32_t i = 0; i < len; i++) {
        MEM[off + i] &= d[i];
    }
    return 0;
}

static int f_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    memset(MEM + off, 0xFF, len);
    return 0;
}

static const elog_flash_t FLASH = {NULL, SIZE, SECTOR, f_read, f_write, f_erase};

/* ---- scripted broker ------------------------------------------------------------------------- */

typedef struct {
    uint32_t seq;
    bool replayed;
    int msg_id;
} sent_t;

static sent_t SENT[64];
static unsigned N_SENT;
static int NEXT_ID;
static bool REFUSE; /* the client can't queue a publish right now */

static bool markers(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_REPORTED;
}

static size_t p_read_after(void *ctx, uint32_t after, elog_entry_t *out, size_t max,
                           uint32_t scan, uint32_t *next_after)
{
    (void)ctx;
    return elog_read_after(&LOG, after, markers, NULL, out, max, scan, next_after);
}

static uint32_t p_newest(void *ctx)
{
    (void)ctx;
    return elog_newest_index(&LOG);
}

static int p_publish(void *ctx, const elog_entry_t *marker, bool replayed)
{
    (void)ctx;
    if (REFUSE || N_SENT >= 64u) {
        return -1;
    }
    SENT[N_SENT].seq = marker->ev.seq;
    SENT[N_SENT].replayed = replayed;
    SENT[N_SENT].msg_id = ++NEXT_ID;
    return SENT[N_SENT++].msg_id;
}

static const proto_report_port_t PORT = {NULL, p_read_after, p_newest, p_publish};
static proto_report_t R;

static void event(uint32_t seq)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = seq;
    ev.type = ALARM_EV_ARMED;
    ev.flags = ALARM_EVF_REPORT;
    TEST_ASSERT_EQUAL_INT(0, elog_append(&LOG, ELOG_KIND_EVENT, &ev));
}

static void marker(uint32_t seq)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = seq;
    ev.type = ALARM_EV_ARMED;
    ev.flags = ALARM_EVF_REPORT;
    TEST_ASSERT_EQUAL_INT(0, elog_append(&LOG, ELOG_KIND_REPORTED, &ev));
}

/* Poll until the reporter has nothing to do right now. */
static void drain(alarm_ms_t now)
{
    for (unsigned guard = 0; guard < 1000u && proto_report_poll(&R, now); guard++) {
    }
}

/* The broker acknowledges everything sent so far, in order, polling in between. */
static void ack_all(alarm_ms_t now)
{
    for (unsigned i = 0; i < N_SENT; i++) {
        proto_report_acked(&R, SENT[i].msg_id, now);
        drain(now);
    }
}

void setUp(void)
{
    memset(MEM, 0xFF, sizeof(MEM));
    TEST_ASSERT_EQUAL_INT(0, elog_open(&LOG, &FLASH, 1));
    memset(SENT, 0, sizeof(SENT));
    N_SENT = 0;
    NEXT_ID = 100;
    REFUSE = false;
    proto_report_init(&R, &PORT, 0, true, 0);
}

void tearDown(void) {}

/* MQTT-005: reportable events go out once, in the order they became due (the abort window
 * can hold an alarm back behind later events), one at a time, each after the previous ack. */
static void test_publishes_in_order_once(void)
{
    proto_report_connected(&R, 1000);
    event(1);
    event(2);
    marker(2);
    event(3);
    marker(3);
    marker(1); /* the alarm, after its abort window */
    drain(1000);
    TEST_ASSERT_EQUAL_UINT(1, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(2, SENT[0].seq);
    TEST_ASSERT_FALSE(SENT[0].replayed);

    proto_report_acked(&R, 999, 1100); /* someone else's message */
    drain(1100);
    TEST_ASSERT_EQUAL_UINT(1, N_SENT);
    ack_all(1200);
    TEST_ASSERT_EQUAL_UINT(3, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(3, SENT[1].seq);
    TEST_ASSERT_EQUAL_UINT32(1, SENT[2].seq);
    proto_report_acked(&R, SENT[2].msg_id, 1300);
    drain(1300);
    TEST_ASSERT_EQUAL_UINT(3, N_SENT);
    TEST_ASSERT_FALSE(proto_report_waiting(&R));
    TEST_ASSERT_EQUAL_UINT32(elog_newest_index(&LOG), R.acked);
    TEST_ASSERT_TRUE(R.acked_dirty);
}

/* MQTT-006, LOG-006: after a reconnect the unacknowledged one is sent again, then what became
 * due while offline, oldest first and marked replayed; new ones are live again. */
static void test_replay_after_reconnect(void)
{
    proto_report_connected(&R, 0);
    marker(10);
    marker(11);
    drain(0);
    proto_report_acked(&R, SENT[0].msg_id, 10);
    drain(10);
    TEST_ASSERT_EQUAL_UINT(2, N_SENT); /* 10 acked, 11 in flight */
    proto_report_disconnected(&R, 20);
    marker(12);
    event(13);
    marker(13);
    drain(30);
    TEST_ASSERT_EQUAL_UINT(2, N_SENT); /* nothing while offline */

    proto_report_connected(&R, 40);
    marker(14);
    drain(40);
    ack_all(50);
    TEST_ASSERT_EQUAL_UINT(6, N_SENT);
    const uint32_t seqs[] = {10, 11, 11, 12, 13, 14};
    const bool replayed[] = {false, false, true, true, true, false};
    for (unsigned i = 0; i < 6u; i++) {
        TEST_ASSERT_EQUAL_UINT32(seqs[i], SENT[i].seq);
        TEST_ASSERT_EQUAL(replayed[i], SENT[i].replayed);
    }
    TEST_ASSERT_FALSE(proto_report_waiting(&R));
}

/* MQTT-006: a restart continues from the saved position; a first setup skips the history. */
static void test_restart_and_first_setup(void)
{
    marker(1);
    marker(2);
    const uint32_t saved = elog_newest_index(&LOG) - 1u; /* 1 delivered, 2 not yet */
    marker(3);
    proto_report_init(&R, &PORT, saved, true, 0);
    proto_report_connected(&R, 0);
    drain(0);
    ack_all(0);
    TEST_ASSERT_EQUAL_UINT(2, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(2, SENT[0].seq);
    TEST_ASSERT_EQUAL_UINT32(3, SENT[1].seq);
    TEST_ASSERT_TRUE(SENT[0].replayed);

    N_SENT = 0;
    proto_report_init(&R, &PORT, elog_newest_index(&LOG), true, 0);
    proto_report_connected(&R, 0);
    drain(0);
    TEST_ASSERT_EQUAL_UINT(0, N_SENT);
}

/* Records other than markers are stepped over (in bounded steps) and the position follows;
 * a cleared log resets it. A publish the client refuses is retried. */
static void test_skips_other_records_and_cleared_log(void)
{
    proto_report_connected(&R, 0);
    for (uint32_t s = 1; s <= 600; s++) {
        event(s);
    }
    marker(600);
    REFUSE = true;
    drain(0);
    TEST_ASSERT_EQUAL_UINT(0, N_SENT);
    TEST_ASSERT_TRUE(R.acked >= 600u - PROTO_REPORT_SCAN); /* moved past plain events */
    TEST_ASSERT_TRUE(proto_report_waiting(&R));
    REFUSE = false;
    drain(10);
    TEST_ASSERT_EQUAL_UINT(1, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(600, SENT[0].seq);
    ack_all(20);

    TEST_ASSERT_EQUAL_INT(0, elog_clear(&LOG));
    drain(30);
    TEST_ASSERT_EQUAL_UINT32(0, R.acked);
    marker(700);
    drain(40);
    TEST_ASSERT_EQUAL_UINT(2, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(700, SENT[1].seq);
}

/* MQTT-011: comm_fail after 5 minutes without the broker, or with a report stuck; never when
 * reporting isn't configured. An unacknowledged publish is sent again after the timeout. */
static void test_comm_fail(void)
{
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, PROTO_COMM_FAIL_MS - 1u));
    TEST_ASSERT_TRUE(proto_report_comm_fail(&R, PROTO_COMM_FAIL_MS));
    proto_report_enable(&R, false, PROTO_COMM_FAIL_MS);
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, 10u * PROTO_COMM_FAIL_MS));
    proto_report_enable(&R, true, 10u * PROTO_COMM_FAIL_MS);
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, 10u * PROTO_COMM_FAIL_MS + 1000u));

    const alarm_ms_t t0 = 20u * PROTO_COMM_FAIL_MS;
    proto_report_connected(&R, t0);
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, t0 + 10u * PROTO_COMM_FAIL_MS));
    marker(1);
    drain(t0);
    TEST_ASSERT_EQUAL_UINT(1, N_SENT);
    drain(t0 + PROTO_ACK_TIMEOUT_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(1, N_SENT);
    drain(t0 + PROTO_ACK_TIMEOUT_MS); /* no ack: sent again */
    TEST_ASSERT_EQUAL_UINT(2, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(1, SENT[1].seq);
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, t0 + PROTO_COMM_FAIL_MS - 1u));
    TEST_ASSERT_TRUE(proto_report_comm_fail(&R, t0 + PROTO_COMM_FAIL_MS));
    proto_report_acked(&R, SENT[0].msg_id, t0 + PROTO_COMM_FAIL_MS); /* the first copy: stale id */
    TEST_ASSERT_TRUE(proto_report_comm_fail(&R, t0 + PROTO_COMM_FAIL_MS));
    proto_report_acked(&R, SENT[1].msg_id, t0 + PROTO_COMM_FAIL_MS);
    drain(t0 + PROTO_COMM_FAIL_MS);
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, t0 + 3u * PROTO_COMM_FAIL_MS));

    proto_report_disconnected(&R, t0 + 4u * PROTO_COMM_FAIL_MS);
    TEST_ASSERT_FALSE(proto_report_comm_fail(&R, t0 + 5u * PROTO_COMM_FAIL_MS - 1u));
    TEST_ASSERT_TRUE(proto_report_comm_fail(&R, t0 + 5u * PROTO_COMM_FAIL_MS));
}

/* MQTT-017: this path can only resend what the log kept. A report whose marker never reached
 * the flash is invisible here - no reconnect and no restart brings it back, and the reporter
 * says nothing is waiting - so the panel must not leave it at that: panel_log hands a report it
 * could not keep straight to net_mqtt for live publishing, and counts it as an undelivered
 * report (a latching storage trouble) when even that can't take it. */
static void test_lost_marker_is_never_replayed(void)
{
    proto_report_connected(&R, 0);
    marker(10);
    /* the marker for 11 is the one the event log could not keep */
    marker(12);
    drain(0);
    ack_all(10);
    TEST_ASSERT_EQUAL_UINT(2, N_SENT);
    TEST_ASSERT_EQUAL_UINT32(10, SENT[0].seq);
    TEST_ASSERT_EQUAL_UINT32(12, SENT[1].seq);

    /* A reconnect finds nothing to catch up on. */
    proto_report_disconnected(&R, 20);
    proto_report_connected(&R, 30);
    drain(30);
    ack_all(40);
    TEST_ASSERT_EQUAL_UINT(2, N_SENT);
    TEST_ASSERT_FALSE(proto_report_waiting(&R));

    /* Nor does a restart that replays everything in the log. */
    proto_report_init(&R, &PORT, 0, true, 50);
    proto_report_connected(&R, 50);
    drain(50);
    ack_all(60);
    TEST_ASSERT_EQUAL_UINT(4, N_SENT);
    for (unsigned i = 2; i < N_SENT; i++) {
        TEST_ASSERT_TRUE(SENT[i].seq == 10u || SENT[i].seq == 12u);
        TEST_ASSERT_TRUE(SENT[i].replayed);
    }
    TEST_ASSERT_FALSE(proto_report_waiting(&R));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_publishes_in_order_once);
    RUN_TEST(test_replay_after_reconnect);
    RUN_TEST(test_restart_and_first_setup);
    RUN_TEST(test_skips_other_records_and_cleared_log);
    RUN_TEST(test_comm_fail);
    RUN_TEST(test_lost_marker_is_never_replayed);
    return UNITY_END();
}
