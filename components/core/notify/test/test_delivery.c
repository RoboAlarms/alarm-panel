/*
 * test_delivery.c - the gate and the reporter together, the way net_notify drives them
 * (components/core/notify notify_gate_deliver(), components/core/alarm_proto report.c).
 *
 * Testing the gate and the reporter apart missed code review finding S02: a first attempt that
 * failed had already been recorded by the gate, so the retry was folded or dropped as a
 * duplicate and the reporter acknowledged it without a second request. Here a real event log in
 * RAM, a real proto_report_t and the real gate run against a scripted HTTP transport.
 * Requirements: NOTE-014 (a 4xx is final, a 5xx/429/timeout is retried), NOTE-010 (an event
 * reaches the phone once the service is back), NOTE-030/-033/-034/-037 (the gate's limits still
 * hold), PUSH-005 (a cancel is retried until it is reached) - docs/features/26, /28. S02, S08.
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_report.h"
#include "notify/notify_gate.h"
#include "notify/notify_http.h"
#include "unity.h"

void setUp(void);
void tearDown(void);

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

/* ---- the scripted service -------------------------------------------------------------------- */

#define SCRIPT_MAX 16u

static int SCRIPT[SCRIPT_MAX]; /* the HTTP status of each request in turn; past the end, 200 */
static unsigned N_SCRIPT;
static unsigned N_REQ;         /* requests made */
static alarm_event_t REQ[SCRIPT_MAX];
static unsigned N_TAKEN;       /* requests the service answered 2xx */

static void script(const int *statuses, unsigned n)
{
    memcpy(SCRIPT, statuses, n * sizeof(statuses[0]));
    N_SCRIPT = n;
}

static int next_status(void)
{
    const int status = N_REQ < N_SCRIPT ? SCRIPT[N_REQ] : 200;
    N_REQ++;
    if (status >= 200 && status < 300) {
        N_TAKEN++;
    }
    return status;
}

/* send_words() in net_notify: true when the service took it or refused it for good. */
static bool transport(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (N_REQ < SCRIPT_MAX) {
        REQ[N_REQ] = *ev;
    }
    return notify_http_outcome(next_status()) != NOTIFY_HTTP_RETRY;
}

/* ---- the reporter, as net_notify wires it ---------------------------------------------------- */

static notify_gate_t GATE;
static proto_report_t R;
static uint64_t NOW;
static int NEXT_ID;

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

/* rp_publish_for(): offer the marker's event; -1 leaves it for the next poll. */
static int p_publish(void *ctx, const elog_entry_t *marker, bool replayed)
{
    (void)ctx;
    (void)replayed;
    if (!notify_gate_deliver(&GATE, &marker->ev, NOW, transport, NULL)) {
        return -1;
    }
    return ++NEXT_ID;
}

static const proto_report_port_t PORT = {NULL, p_read_after, p_newest, p_publish};

/* One pass of the worker: one poll, acknowledged at once when it published. */
static bool pass(void)
{
    if (!proto_report_poll(&R, NOW)) {
        return false;
    }
    if (R.inflight != 0u) {
        proto_report_acked(&R, R.msg_id, NOW);
    }
    return true;
}

static void drain(void)
{
    for (unsigned guard = 0; guard < 100u && pass(); guard++) {
    }
}

static uint32_t marker(alarm_event_t ev)
{
    ev.flags = (uint8_t)(ev.flags | ALARM_EVF_REPORT);
    TEST_ASSERT_EQUAL_INT(0, elog_append(&LOG, ELOG_KIND_REPORTED, &ev));
    return elog_newest_index(&LOG);
}

static alarm_event_t ev_of(alarm_event_type_t type)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.partition = 1u;
    return ev;
}

static alarm_event_t alarm_in(uint16_t zone)
{
    alarm_event_t ev = ev_of(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_BURGLARY;
    ev.zone = zone;
    return ev;
}

static alarm_event_t trouble_in(uint16_t zone)
{
    alarm_event_t ev = ev_of(ALARM_EV_TROUBLE);
    ev.arg2 = 1u;
    ev.arg = 2u;
    ev.zone = zone;
    return ev;
}

void setUp(void)
{
    memset(MEM, 0xFF, sizeof(MEM));
    TEST_ASSERT_EQUAL_INT(0, elog_open(&LOG, &FLASH, 1));
    memset(SCRIPT, 0, sizeof(SCRIPT));
    memset(REQ, 0, sizeof(REQ));
    N_SCRIPT = 0u;
    N_REQ = 0u;
    N_TAKEN = 0u;
    NEXT_ID = 0;
    NOW = 100000u;
    notify_gate_init(&GATE, NOTIFY_CATS_DEFAULT, false, NOW);
    proto_report_init(&R, &PORT, 0u, true, NOW);
    proto_report_connected(&R, NOW);
}

void tearDown(void)
{
}

/* ---- S02: a failed send is sent again, and only then acknowledged ---------------------------- */

/* NOTE-014, NOTE-033, S02: a one-zone burglary whose first request timed out or met a 5xx is
 * requested again (not folded into its own rollup) and acknowledged only once it was taken. */
static void test_failed_alarm_is_sent_again(void)
{
    const int statuses[] = {-1, 503, 429};
    script(statuses, 3u);
    const uint32_t at = marker(alarm_in(3u));

    for (unsigned attempt = 1u; attempt <= 3u; attempt++) {
        TEST_ASSERT_FALSE(pass());
        TEST_ASSERT_EQUAL_UINT(attempt, N_REQ);
        TEST_ASSERT_TRUE_MESSAGE(R.acked < at, "a failed alarm was written off as delivered");
        NOW += 15000u; /* the worker's backoff */
    }
    TEST_ASSERT_TRUE(pass());
    TEST_ASSERT_EQUAL_UINT(4u, N_REQ);
    TEST_ASSERT_EQUAL_UINT(1u, N_TAKEN);
    TEST_ASSERT_EQUAL_UINT16(3u, REQ[3].zone);
    TEST_ASSERT_EQUAL_UINT32(at, R.acked);

    /* Taken once, it is recorded once: the same zone again is folded in, and a single zone
     * still makes no follow-up (NOTE-033). */
    NOW += 1000u;
    marker(alarm_in(3u));
    drain();
    TEST_ASSERT_EQUAL_UINT(4u, N_REQ);
    notify_rollup_t r;
    TEST_ASSERT_FALSE(notify_gate_follow_up(&GATE, NOW + NOTIFY_ROLLUP_MS, &r));
}

/* NOTE-014, NOTE-034, S02: a trouble whose first request failed is not dropped as a duplicate of
 * itself; once taken, the same trouble within the hour still is. */
static void test_failed_trouble_is_sent_again(void)
{
    const int statuses[] = {500};
    script(statuses, 1u);
    const uint32_t at = marker(trouble_in(4u));

    TEST_ASSERT_FALSE(pass());
    TEST_ASSERT_EQUAL_UINT(1u, N_REQ);
    TEST_ASSERT_TRUE(R.acked < at);
    NOW += 15000u;
    TEST_ASSERT_TRUE(pass());
    TEST_ASSERT_EQUAL_UINT(2u, N_REQ);
    TEST_ASSERT_EQUAL_UINT32(at, R.acked);

    NOW += 60000u;
    marker(trouble_in(4u));
    drain();
    TEST_ASSERT_EQUAL_UINT(2u, N_REQ); /* NOTE-034: not said twice within the hour */
}

/* NOTE-014: a 4xx (other than 429) will never succeed, so it is final: acknowledged after one
 * request, and recorded as said (the same trouble is not sent again within the hour). */
static void test_refusal_is_final(void)
{
    const int statuses[] = {400};
    script(statuses, 1u);
    const uint32_t at = marker(trouble_in(4u));
    TEST_ASSERT_TRUE(pass());
    TEST_ASSERT_EQUAL_UINT(1u, N_REQ);
    TEST_ASSERT_EQUAL_UINT32(at, R.acked);

    NOW += 60000u;
    marker(trouble_in(4u));
    drain();
    TEST_ASSERT_EQUAL_UINT(1u, N_REQ);
}

/* NOTE-037: an event the owner doesn't want costs no request and is acknowledged at once, even
 * while the service is failing. */
static void test_filtered_event_needs_no_request(void)
{
    const int statuses[] = {503, 503};
    script(statuses, 2u);
    alarm_event_t bypass = ev_of(ALARM_EV_BYPASS);
    bypass.zone = 2u;
    const uint32_t at = marker(bypass);
    TEST_ASSERT_TRUE(pass());
    TEST_ASSERT_EQUAL_UINT(0u, N_REQ);
    TEST_ASSERT_EQUAL_UINT32(at, R.acked);
}

/* NOTE-030: retrying one event costs it no tokens, and distinct events are still limited. */
static void test_retries_spend_no_tokens(void)
{
    const int statuses[] = {503, 503, 503, 503, 503, 503, 503, 503, 503, 503, 503, 503};
    script(statuses, 12u);
    alarm_event_t armed = ev_of(ALARM_EV_ARMED);
    armed.user = 1u;
    const uint32_t at = marker(armed);
    for (unsigned i = 0; i < 12u; i++) {
        TEST_ASSERT_FALSE(pass()); /* every retry is a real request */
    }
    TEST_ASSERT_EQUAL_UINT(12u, N_REQ);
    TEST_ASSERT_TRUE(pass());
    TEST_ASSERT_EQUAL_UINT32(at, R.acked);
    TEST_ASSERT_EQUAL_UINT(13u, N_REQ);

    /* Nine more distinct ones fill the burst of ten; the eleventh is held back. */
    for (unsigned i = 0; i < NOTIFY_BUCKET_MAX; i++) {
        marker(armed);
    }
    drain();
    TEST_ASSERT_EQUAL_UINT(13u + NOTIFY_BUCKET_MAX - 1u, N_REQ);
}

/* NOTE-033, S02: a follow-up whose request failed stays owed and is sent again; once taken it is
 * not sent a third time. */
static void test_failed_follow_up_stays_pending(void)
{
    marker(alarm_in(1u));
    drain();
    NOW += 1000u;
    marker(alarm_in(5u));
    drain();
    TEST_ASSERT_EQUAL_UINT(1u, N_REQ);

    NOW += NOTIFY_ROLLUP_MS;
    notify_rollup_t r;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r));
    TEST_ASSERT_EQUAL_UINT16(2u, r.total);
    TEST_ASSERT_EQUAL_UINT8(1u, r.partition);
    /* ... its request fails: nothing is recorded, so it is still there. */
    const uint16_t day = GATE.day_count;
    NOW += 15000u;
    memset(&r, 0, sizeof(r));
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r));
    TEST_ASSERT_EQUAL_UINT16(2u, r.total);
    TEST_ASSERT_EQUAL_UINT16(5u, r.zones[1]);
    TEST_ASSERT_EQUAL_UINT16(day, GATE.day_count);
    /* ... it is taken. */
    notify_gate_follow_up_sent(&GATE, r.partition, NOW);
    TEST_ASSERT_EQUAL_UINT16(day + 1u, GATE.day_count);
    TEST_ASSERT_FALSE(notify_gate_follow_up(&GATE, NOW + 1000u, &r));
}

/* NOTE-033: a follow-up still owed when the next alarm's rollup closes is not lost; the two are
 * said as one. */
static void test_owed_follow_up_takes_the_next_one_in(void)
{
    marker(alarm_in(1u));
    drain();
    NOW += 1000u;
    marker(alarm_in(2u));
    drain();
    NOW += NOTIFY_ROLLUP_MS;
    notify_rollup_t r;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r)); /* ... and its request fails */

    /* A new alarm in the same partition, after the window, with another zone folded in. */
    NOW += NOTIFY_ALARM_MIN_GAP_MS;
    marker(alarm_in(3u));
    drain();
    NOW += 1000u;
    marker(alarm_in(4u));
    drain();
    TEST_ASSERT_EQUAL_UINT(2u, N_REQ);
    NOW += NOTIFY_ROLLUP_MS;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r));
    TEST_ASSERT_EQUAL_UINT16(4u, r.total);
    TEST_ASSERT_EQUAL_UINT8(NOTIFY_ROLLUP_ZONES, r.n);
    TEST_ASSERT_EQUAL_UINT16(1u, r.zones[0]);
    TEST_ASSERT_EQUAL_UINT16(2u, r.zones[1]);
    TEST_ASSERT_EQUAL_UINT16(3u, r.zones[2]);
    notify_gate_follow_up_sent(&GATE, r.partition, NOW);
    TEST_ASSERT_FALSE(notify_gate_follow_up(&GATE, NOW, &r));
}

/* NOTE-031: a follow-up owed when the day's hard cap is reached is not sent. */
static void test_owed_follow_up_respects_the_hard_cap(void)
{
    marker(alarm_in(1u));
    drain();
    NOW += 1000u;
    marker(alarm_in(2u));
    drain();
    NOW += NOTIFY_ROLLUP_MS;
    notify_rollup_t r;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r));
    GATE.day_count = NOTIFY_DAY_HARD;
    TEST_ASSERT_FALSE(notify_gate_follow_up(&GATE, NOW + 1000u, &r));
}

/* The one-step calls are the two-step calls back to back (the gate's own tests use them). */
static void test_offer_is_admit_then_sent(void)
{
    alarm_event_t t = trouble_in(7u);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_admit(&GATE, &t, NOW));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_admit(&GATE, &t, NOW)); /* nothing kept */
    notify_gate_sent(&GATE, &t, NOW);
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_admit(&GATE, &t, NOW));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_DROP, notify_gate_offer(&GATE, &t, NOW));
}

/* ---- S08: what an answer means for trying again ---------------------------------------------- */

/* NOTE-014, PUSH-005, S08: 2xx is taken; a 4xx other than 429 is final; no answer, a 429 and a
 * 5xx are tried again - for a notification and for a cancel_by_tag alike. */
static void test_http_outcome(void)
{
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_TAKEN, notify_http_outcome(200));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_TAKEN, notify_http_outcome(204));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_REFUSED, notify_http_outcome(400));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_REFUSED, notify_http_outcome(401));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_REFUSED, notify_http_outcome(403));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_REFUSED, notify_http_outcome(404));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_REFUSED, notify_http_outcome(499));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(429));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(500));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(503));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(599));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(-1)); /* no answer, a timeout */
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(0));
    TEST_ASSERT_EQUAL_INT(NOTIFY_HTTP_RETRY, notify_http_outcome(302));
}

/* ---- code review: the rollup, the day, the pace, the live queue, the stale emergency ------- */

/* NOTE-033, S02: an alarm sent right after a rollup's minute, before the worker's
 * notify_gate_follow_up() ran (the poll comes first in a pass, e.g. as Wi-Fi returns), opens the
 * next rollup in its place - and the zones the old one took in are still said. */
static void test_alarm_after_the_minute_keeps_the_follow_up(void)
{
    marker(alarm_in(1u));
    drain();
    NOW += 1000u;
    marker(alarm_in(2u));
    drain();
    TEST_ASSERT_EQUAL_UINT(1u, N_REQ);

    NOW += NOTIFY_ROLLUP_MS; /* the minute is up; nobody has asked for the follow-up yet */
    marker(alarm_in(3u));
    drain();
    TEST_ASSERT_EQUAL_UINT(2u, N_REQ); /* the new alarm went */

    notify_rollup_t r;
    TEST_ASSERT_TRUE_MESSAGE(notify_gate_follow_up(&GATE, NOW, &r),
                             "the old rollup's follow-up was cleared by the new alarm");
    TEST_ASSERT_EQUAL_UINT8(1u, r.partition);
    TEST_ASSERT_EQUAL_UINT16(2u, r.total);
    TEST_ASSERT_EQUAL_UINT16(1u, r.zones[0]);
    TEST_ASSERT_EQUAL_UINT16(2u, r.zones[1]);
    notify_gate_follow_up_sent(&GATE, r.partition, NOW);
    TEST_ASSERT_FALSE(notify_gate_follow_up(&GATE, NOW, &r)); /* the new one is still running */
}

/* NOTE-032/-033, S02: the same, when the new alarm is held by the gap between alarms (another
 * partition's alarm went a moment ago) rather than sent. */
static void test_held_alarm_after_the_minute_keeps_the_follow_up(void)
{
    marker(alarm_in(1u));
    drain();
    NOW += 1000u;
    marker(alarm_in(2u));
    drain();
    NOW += NOTIFY_ROLLUP_MS - 10000u;
    alarm_event_t other = alarm_in(9u);
    other.partition = 2u;
    marker(other);
    drain();
    TEST_ASSERT_EQUAL_UINT(2u, N_REQ);

    NOW += 10000u; /* partition 1's minute is up, partition 2's alarm went 10 s ago */
    marker(alarm_in(3u));
    drain();
    TEST_ASSERT_EQUAL_UINT(2u, N_REQ); /* held by NOTE-032 */

    notify_rollup_t r;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r));
    TEST_ASSERT_EQUAL_UINT8(1u, r.partition);
    TEST_ASSERT_EQUAL_UINT16(2u, r.total);
    TEST_ASSERT_EQUAL_UINT16(2u, r.zones[1]);
    notify_gate_follow_up_sent(&GATE, r.partition, NOW);
    /* ... and the held one is said by its own follow-up when its minute is up. */
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW + NOTIFY_ROLLUP_MS, &r));
    TEST_ASSERT_EQUAL_UINT8(1u, r.partition);
    TEST_ASSERT_EQUAL_UINT16(3u, r.zones[0]);
}

/* NOTE-031, S02: a follow-up owed across midnight of the rolling day is not dropped by
 * yesterday's hard cap. */
static void test_owed_follow_up_survives_yesterdays_cap(void)
{
    marker(alarm_in(1u));
    drain();
    NOW += 1000u;
    marker(alarm_in(2u));
    drain();
    GATE.day_count = NOTIFY_DAY_HARD; /* the day ran out while the follow-up was owed */
    notify_rollup_t r;
    TEST_ASSERT_TRUE(notify_gate_paused(&GATE));
    NOW = GATE.day_start + NOTIFY_DAY_MS;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW, &r));
    TEST_ASSERT_EQUAL_UINT16(2u, r.total);
    TEST_ASSERT_FALSE(notify_gate_paused(&GATE));
}

/* NOTE-014: the gap after a failure grows from 2 s to a minute, with up to a quarter more as
 * jitter, and a run of failures doesn't overflow it. */
static void test_backoff_grows_with_jitter(void)
{
    uint8_t fails = 0u;
    const uint32_t want[] = {2000u, 4000u, 8000u, 16000u, 32000u, 60000u, 60000u};
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        TEST_ASSERT_EQUAL_UINT64(1000u + want[i], notify_backoff(&fails, 1000u, 0u));
        TEST_ASSERT_EQUAL_UINT8(i + 1u, fails);
    }
    fails = 0u;
    const uint64_t jittered = notify_backoff(&fails, 0u, 0xFFFFFFFFu);
    TEST_ASSERT_TRUE(jittered >= 2000u && jittered < 2500u);
    fails = 254u;
    notify_backoff(&fails, 0u, 7u);
    notify_backoff(&fails, 0u, 7u);
    TEST_ASSERT_EQUAL_UINT8(255u, fails);
    TEST_ASSERT_EQUAL_UINT64(60000u + 7u, notify_backoff(&fails, 0u, 7u));
}

/* NOTE-014/-035: a service is sent to only after its backoff and its gap; a request that went
 * later in this same pass (last_send after now) counts as too soon. */
static void test_may_send(void)
{
    TEST_ASSERT_TRUE(notify_may_send(10000u, 0u, 0u));
    TEST_ASSERT_FALSE(notify_may_send(10000u, 10001u, 0u));
    TEST_ASSERT_TRUE(notify_may_send(10001u, 10001u, 0u));
    TEST_ASSERT_FALSE(notify_may_send(10000u, 0u, 10000u - NOTIFY_SEND_GAP_MS + 1u));
    TEST_ASSERT_TRUE(notify_may_send(10000u, 0u, 10000u - NOTIFY_SEND_GAP_MS));
    TEST_ASSERT_FALSE(notify_may_send(10000u, 0u, 10500u));
}

/* ---- LOG-010's live queue: one event, two services, each with its own fate ------------------ */

typedef struct {
    notify_gate_t gate;
    const int *script; /* the HTTP status of each request; past the end, 200 */
    unsigned n_script;
    unsigned req;
    uint8_t fails;
    uint64_t retry_at;
    uint64_t last_send;
} service_t;

static service_t SVC[2];

static bool svc_send(void *ctx, const alarm_event_t *ev)
{
    service_t *s = ctx;
    (void)ev;
    const int status = s->req < s->n_script ? s->script[s->req] : 200;
    s->req++;
    s->last_send = NOW;
    const notify_http_outcome_t out = notify_http_outcome(status);
    if (out == NOTIFY_HTTP_TAKEN) {
        s->fails = 0u;
    }
    return out != NOTIFY_HTTP_RETRY;
}

/* live_try() in net_notify: wait for this service's pace, offer it through its gate, and back
 * off when the request failed. */
static bool live_try(void *ctx, uint8_t n)
{
    const alarm_event_t *ev = ctx;
    service_t *s = &SVC[n];
    if (!notify_may_send(NOW, s->retry_at, s->last_send)) {
        return false;
    }
    if (notify_gate_deliver(&s->gate, ev, NOW, svc_send, s)) {
        return true;
    }
    s->retry_at = notify_backoff(&s->fails, NOW, 0u);
    return false;
}

static void services(const int *a, unsigned na, const int *b, unsigned nb)
{
    memset(SVC, 0, sizeof(SVC));
    SVC[0].script = a;
    SVC[0].n_script = na;
    SVC[1].script = b;
    SVC[1].n_script = nb;
    notify_gate_init(&SVC[0].gate, NOTIFY_CATS_DEFAULT, false, NOW);
    notify_gate_init(&SVC[1].gate, NOTIFY_CATS_DEFAULT, false, NOW);
}

/* S02, NOTE-014, LOG-010: one service taking a live event doesn't write it off for the other,
 * whose request failed; the failing one is tried again only after its growing backoff, never
 * once a pass, and the one that took it is not asked twice. */
static void test_live_event_reaches_each_service(void)
{
    static const int fail_twice[] = {-1, 503};
    services(NULL, 0u, fail_twice, 2u);
    alarm_event_t ev = trouble_in(4u);
    uint8_t done = 0u;

    TEST_ASSERT_FALSE(notify_fan_out(&done, 0x3u, live_try, &ev));
    TEST_ASSERT_EQUAL_UINT(1u, SVC[0].req);
    TEST_ASSERT_EQUAL_UINT(1u, SVC[1].req);
    const uint64_t first_gap = SVC[1].retry_at - NOW;
    TEST_ASSERT_EQUAL_UINT64(NOTIFY_RETRY_MS, first_gap);

    /* Passes a second apart, inside the backoff: no request to anyone. */
    for (unsigned i = 0; i < 3u; i++) {
        NOW += 500u;
        TEST_ASSERT_FALSE(notify_fan_out(&done, 0x3u, live_try, &ev));
    }
    TEST_ASSERT_EQUAL_UINT(1u, SVC[0].req);
    TEST_ASSERT_EQUAL_UINT(1u, SVC[1].req);

    NOW = SVC[1].retry_at;
    TEST_ASSERT_FALSE(notify_fan_out(&done, 0x3u, live_try, &ev));
    TEST_ASSERT_EQUAL_UINT(2u, SVC[1].req);
    TEST_ASSERT_EQUAL_UINT64(2u * NOTIFY_RETRY_MS, SVC[1].retry_at - NOW); /* it grew */

    NOW = SVC[1].retry_at;
    TEST_ASSERT_TRUE(notify_fan_out(&done, 0x3u, live_try, &ev));
    TEST_ASSERT_EQUAL_UINT(1u, SVC[0].req);
    TEST_ASSERT_EQUAL_UINT(3u, SVC[1].req);
    TEST_ASSERT_EQUAL_UINT8(0u, done); /* ready for the next event */
}

/* S02, NOTE-014/-037: a refusal (4xx) or the owner's filter finishes a service with the event as
 * surely as taking it does; nobody switched on finishes it at once; a service switched off while
 * it waited stops holding it. */
static void test_live_event_terminal_outcomes(void)
{
    static const int refused[] = {400};
    static const int failing[] = {503, 503, 503};
    alarm_event_t ev = trouble_in(4u);
    uint8_t done = 0u;

    services(refused, 1u, NULL, 0u);
    SVC[1].gate.cats = 0u; /* this owner wants no troubles */
    TEST_ASSERT_TRUE(notify_fan_out(&done, 0x3u, live_try, &ev));
    TEST_ASSERT_EQUAL_UINT(1u, SVC[0].req);
    TEST_ASSERT_EQUAL_UINT(0u, SVC[1].req);

    TEST_ASSERT_TRUE(notify_fan_out(&done, 0x0u, live_try, &ev));

    services(NULL, 0u, failing, 3u);
    TEST_ASSERT_FALSE(notify_fan_out(&done, 0x3u, live_try, &ev));
    TEST_ASSERT_EQUAL_UINT8(0x1u, done);
    TEST_ASSERT_TRUE(notify_fan_out(&done, 0x1u, live_try, &ev)); /* service 1 switched off */
    TEST_ASSERT_EQUAL_UINT(1u, SVC[0].req);
    TEST_ASSERT_EQUAL_UINT8(0u, done);
}

/* PUSH-005, code review: an alarm whose request kept failing until after its disarm (and the
 * cancel_by_tag that went with it) still reaches the phone, but not as a repeating emergency
 * nothing would cancel. A newer alarm - or a rollup that took a zone in after the disarm - still
 * is one. */
static void test_dealt_alarm_is_not_an_emergency(void)
{
    alarm_event_t ev = alarm_in(1u);
    ev.seq = 40u;
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_URGENT,
                          notify_priority_dealt(notify_priority(&ev), ev.seq, 0u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_URGENT,
                          notify_priority_dealt(notify_priority(&ev), ev.seq, 39u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_HIGH,
                          notify_priority_dealt(notify_priority(&ev), ev.seq, 41u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_HIGH, notify_priority_dealt(NOTIFY_PRIO_URGENT,
                                                                  0xFFFFFFF0u, 5u)); /* wrapped */
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_NORMAL, notify_priority_dealt(NOTIFY_PRIO_NORMAL, 1u, 9u));

    const notify_pushover_cfg_t c = {"uQiRzpo4DXghDmr9QzzfQu27cmVRsG",
                                     "azGDORePK8gMaC0QOYAMyEEuzJnyUi", NULL, NULL, true, 60u,
                                     1800u};
    notify_words_t w;
    memset(&w, 0, sizeof(w));
    w.cat = NOTIFY_CAT_ALARM;
    snprintf(w.title, sizeof(w.title), "Burglary alarm");
    snprintf(w.body, sizeof(w.body), "Zone 1");
    notify_request_t req;
    TEST_ASSERT_TRUE(notify_build_pushover(
        &c, &w, notify_priority_dealt(notify_priority(&ev), ev.seq, 41u), "alarm-1", &req));
    TEST_ASSERT_NULL(strstr(req.body, "priority=2"));
    TEST_ASSERT_NOT_NULL(strstr(req.body, "priority=1"));

    /* A rollup carries its newest event, through a fold and through owing. */
    alarm_event_t a = alarm_in(1u);
    a.seq = 50u;
    alarm_event_t b = alarm_in(2u);
    b.seq = 52u;
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_SEND, notify_gate_offer(&GATE, &a, NOW));
    TEST_ASSERT_EQUAL_INT(NOTIFY_ACT_HOLD, notify_gate_offer(&GATE, &b, NOW + 1000u));
    notify_rollup_t r;
    TEST_ASSERT_TRUE(notify_gate_follow_up(&GATE, NOW + NOTIFY_ROLLUP_MS, &r));
    TEST_ASSERT_EQUAL_UINT32(52u, r.seq);
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_HIGH, notify_priority_dealt(NOTIFY_PRIO_URGENT, r.seq, 53u));
    TEST_ASSERT_EQUAL_INT(NOTIFY_PRIO_URGENT,
                          notify_priority_dealt(NOTIFY_PRIO_URGENT, r.seq, 51u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_failed_alarm_is_sent_again);
    RUN_TEST(test_failed_trouble_is_sent_again);
    RUN_TEST(test_refusal_is_final);
    RUN_TEST(test_filtered_event_needs_no_request);
    RUN_TEST(test_retries_spend_no_tokens);
    RUN_TEST(test_failed_follow_up_stays_pending);
    RUN_TEST(test_owed_follow_up_takes_the_next_one_in);
    RUN_TEST(test_owed_follow_up_respects_the_hard_cap);
    RUN_TEST(test_offer_is_admit_then_sent);
    RUN_TEST(test_http_outcome);
    RUN_TEST(test_alarm_after_the_minute_keeps_the_follow_up);
    RUN_TEST(test_held_alarm_after_the_minute_keeps_the_follow_up);
    RUN_TEST(test_owed_follow_up_survives_yesterdays_cap);
    RUN_TEST(test_backoff_grows_with_jitter);
    RUN_TEST(test_may_send);
    RUN_TEST(test_live_event_reaches_each_service);
    RUN_TEST(test_live_event_terminal_outcomes);
    RUN_TEST(test_dealt_alarm_is_not_an_emergency);
    return UNITY_END();
}
