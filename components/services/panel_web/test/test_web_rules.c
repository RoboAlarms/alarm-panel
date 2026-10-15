/* web_rules on the PC (code review): M01 - a JSON child that can't be attached is freed and
 * the builder can tell; S06 (docs/features/31 WEB-016) - which events a partition-restricted
 * viewer's Activity shows; PERF11 - one WebSocket message keeps its time limit and stops when
 * asked, however the connection takes it. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "web_rules.h"

/* ---- M01 ----------------------------------------------------------------------------------- */
static long s_live;    /* allocations not yet freed */
static long s_allowed; /* allocations left before every further one fails; <0: no limit */

static void *counting_malloc(size_t n)
{
    if (s_allowed == 0)
        return NULL;
    if (s_allowed > 0)
        s_allowed--;
    void *p = malloc(n);
    if (p)
        s_live++;
    return p;
}
static void counting_free(void *p)
{
    if (p)
        s_live--;
    free(p);
}

void setUp(void)
{
    cJSON_Hooks hooks = {counting_malloc, counting_free};
    cJSON_InitHooks(&hooks);
    s_allowed = -1;
    s_live = 0;
}
void tearDown(void)
{
    cJSON_InitHooks(NULL);
}

/* M01: attached, the child goes with its parent. */
static void test_attach_hands_the_child_over(void)
{
    cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
    TEST_ASSERT_TRUE(web_attach(a, NULL, cJSON_CreateNumber(30)));
    TEST_ASSERT_TRUE(web_attach(o, "entry_delay_s", a));
    TEST_ASSERT_EQUAL_INT(30, cJSON_GetArrayItem(cJSON_GetObjectItem(o, "entry_delay_s"), 0)->valueint);
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(0, s_live);
}

/* M01: no memory for the member's name (the reproduced add_clock case): false, and the child
 * is freed, not lost. */
static void test_attach_frees_the_child_when_the_key_cannot_be_stored(void)
{
    cJSON *o = cJSON_CreateObject(), *k = cJSON_CreateObject();
    cJSON_AddStringToObject(k, "module", "ok");
    const long before = s_live;
    s_allowed = 0;
    TEST_ASSERT_FALSE(web_attach(o, "clock", k));
    s_allowed = -1;
    TEST_ASSERT_LESS_THAN_INT(before, s_live);
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(0, s_live);
}

/* M01: a parent or a child that was never made. */
static void test_attach_without_parent_or_child(void)
{
    TEST_ASSERT_FALSE(web_attach(NULL, "wifi", cJSON_CreateObject()));
    TEST_ASSERT_FALSE(web_attach(NULL, NULL, cJSON_CreateString("h264")));
    cJSON *o = cJSON_CreateObject();
    TEST_ASSERT_FALSE(web_attach(o, "wifi", NULL));
    TEST_ASSERT_FALSE(web_attach(o, NULL, NULL));
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(0, s_live);
}

/* A response as panel_web.c's builders now make one (status: access, capabilities with nested
 * arrays, the clock): NULL - "Memory busy" - when an attachment failed. */
static cJSON *build(void)
{
    cJSON *o = cJSON_CreateObject();
    bool ok = true;
    cJSON *access = cJSON_CreateObject();
    cJSON_AddBoolToObject(access, "users", true);
    ok &= web_attach(o, "access", access);
    cJSON *caps = cJSON_CreateObject(), *video = cJSON_CreateArray();
    ok &= web_attach(video, NULL, cJSON_CreateString("h264"));
    ok &= web_attach(video, NULL, cJSON_CreateString("jpeg"));
    ok &= web_attach(caps, "video", video);
    ok &= web_attach(o, "capabilities", caps);
    cJSON *k = cJSON_CreateObject();
    cJSON_AddStringToObject(k, "module", "ok");
    cJSON_AddNumberToObject(k, "temp_c", 21.5);
    ok &= web_attach(o, "clock", k);
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
/* M01 acceptance: every allocation of the builder fails in turn, the root's and the
 * containers' included; whatever it answers, nothing is left after its cleanup, many times
 * over (a panel short of memory polling), and the control builds it all. */
static void test_builder_leaks_nothing_whichever_allocation_fails(void)
{
    s_allowed = -1;
    cJSON *full = build();
    TEST_ASSERT_NOT_NULL(full);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(full, "clock"));
    cJSON_Delete(full);
    TEST_ASSERT_EQUAL_INT(0, s_live);
    for (unsigned round = 0; round < 50; round++) {
        for (long allowed = 0;; allowed++) {
            s_allowed = allowed;
            cJSON *o = build();
            const bool whole = s_allowed > 0; /* allocations were left over: none failed */
            s_allowed = -1;
            if (o) /* the response, sent and freed by json() */
                cJSON_Delete(o);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_live, "a tree was lost");
            if (whole) {
                TEST_ASSERT_NOT_NULL(o);
                break;
            }
            TEST_ASSERT_TRUE(allowed < 100);
        }
    }
}

/* ---- S06 (WEB-016) -------------------------------------------------------------------------- */
static alarm_config_t s_cfg;

static alarm_event_t event(alarm_event_type_t type, uint8_t partition, uint16_t zone)
{
    alarm_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.partition = partition;
    e.zone = zone;
    return e;
}
static void two_partitions(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.zones[0].type = ALARM_ZT_PERIMETER;
    s_cfg.zones[0].partition = 1;
    s_cfg.zones[1].type = ALARM_ZT_PERIMETER;
    s_cfg.zones[1].partition = 2;
}

/* WEB-016, S06: alarm, disarm and zone events of partition 2 aren't shown to a viewer of
 * partition 1 (a standard user or a partition master), and the other way round. */
static void test_partition_events_only_to_their_viewers(void)
{
    two_partitions();
    const alarm_event_t alarm2 = event(ALARM_EV_ALARM, 2, 2), disarm1 = event(ALARM_EV_DISARMED, 1, 0);
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &alarm2, 0x01));
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &alarm2, 0x02));
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &disarm1, 0x01));
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &disarm1, 0x02));
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &alarm2, 0x03));
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &alarm2, 0x00)); /* no partitions: none */
    const alarm_event_t bad = event(ALARM_EV_ALARM, ALARM_MAX_PARTITIONS + 1, 0);
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &bad, 0x7F)); /* fail closed */
}

/* WEB-016, S06: the installer and the system master (viewer_partitions() 0xFF) keep the whole
 * log. */
static void test_installer_and_system_master_see_everything(void)
{
    two_partitions();
    const alarm_event_t e[] = {event(ALARM_EV_ALARM, 2, 2), event(ALARM_EV_DISARMED, 1, 0),
                               event(ALARM_EV_USER_CHANGED, 0, 0), event(ALARM_EV_TROUBLE, 0, 2),
                               event(ALARM_EV_ALARM, ALARM_MAX_PARTITIONS + 1, 0)};
    for (size_t i = 0; i < sizeof(e) / sizeof(e[0]); i++)
        TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &e[i], 0xFF));
}

/* WEB-016, S06: the system's own events (partition 0: user changes, power, a device's trouble)
 * show to everyone, as the touchscreen's Activity does; one naming a zone shows only to that
 * zone's partition. */
static void test_system_events(void)
{
    two_partitions();
    const alarm_event_t user = event(ALARM_EV_USER_CHANGED, 0, 0);
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &user, 0x01));
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &user, 0x00));
    alarm_event_t zone2 = event(ALARM_EV_TROUBLE, 0, 2);
    zone2.arg2 = ALARM_TRBL_FAMILY_ZONE;
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &zone2, 0x01));
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &zone2, 0x02));
    alarm_event_t device = event(ALARM_EV_TROUBLE_RESTORE, 0, 2); /* device 2, not zone 2 */
    device.arg2 = ALARM_TRBL_FAMILY_DEVICE;
    TEST_ASSERT_TRUE(web_history_visible(&s_cfg, &device, 0x01));
    const alarm_event_t nowhere = event(ALARM_EV_TROUBLE, 0, 3); /* zone 3: no partition */
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &nowhere, 0x01));
    const alarm_event_t beyond = event(ALARM_EV_TROUBLE, 0, ALARM_MAX_ZONES + 1);
    TEST_ASSERT_FALSE(web_history_visible(&s_cfg, &beyond, 0x0F));
}

/* S06: the filter the log is read with; silent events stay notify_words()'s to drop. */
static void test_keep_filters_events_only(void)
{
    two_partitions();
    web_history_view_t v1 = {&s_cfg, 0x01};
    elog_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.kind = ELOG_KIND_EVENT;
    entry.ev = event(ALARM_EV_ALARM, 2, 2);
    TEST_ASSERT_FALSE(web_history_keep(&v1, &entry));
    entry.ev.partition = 1;
    entry.ev.zone = 1;
    entry.ev.flags = ALARM_EVF_SILENT;
    TEST_ASSERT_TRUE(web_history_keep(&v1, &entry));
    entry.kind = (uint8_t)(ELOG_KIND_EVENT + 1); /* not an event: as without a filter */
    entry.ev.partition = 2;
    TEST_ASSERT_TRUE(web_history_keep(&v1, &entry));
}

/* ---- PERF11 ------------------------------------------------------------------------------- */
#define BUDGET_US (15000 * 1000LL) /* web_screen.c SEND_WAIT_MS */
#define STEP_US   (100 * 1000LL)   /* each write takes this long */
typedef struct {
    int64_t now;
    size_t per_call;  /* bytes a write takes; 0: would block */
    int fail_at;      /* the write that breaks (1-based), 0: none */
    int stop_at;      /* stop asked during this write, 0: never */
    int stop_in_pause;
    int writes, pauses;
    size_t sent;
    bool stop;
} conn_t;
static int fake_write(void *ctx, const uint8_t *p, size_t n)
{
    (void)p;
    conn_t *c = ctx;
    c->writes++;
    c->now += STEP_US;
    if (c->stop_at && c->writes == c->stop_at)
        c->stop = true;
    if (c->fail_at && c->writes == c->fail_at)
        return -1;
    const size_t w = c->per_call < n ? c->per_call : n;
    c->sent += w;
    return (int)w;
}
static int64_t fake_now(void *ctx)
{
    return ((conn_t *)ctx)->now;
}
static bool fake_stopping(void *ctx)
{
    return ((conn_t *)ctx)->stop;
}
static void fake_pause(void *ctx)
{
    conn_t *c = ctx;
    c->pauses++;
    c->now += 1000;
    if (c->stop_in_pause && c->pauses == c->stop_in_pause)
        c->stop = true;
}
static uint8_t s_frame[16 * 1024];
static bool send(conn_t *c)
{
    const web_send_io_t io = {fake_write, fake_now, fake_stopping, fake_pause, c, 0};
    return web_send_all(&io, s_frame, sizeof(s_frame), c->now + BUDGET_US);
}

/* PERF11: a connection that takes everything at once - the control. */
static void test_send_fast(void)
{
    conn_t c = {.now = 0, .per_call = sizeof(s_frame)};
    TEST_ASSERT_TRUE(send(&c));
    TEST_ASSERT_EQUAL_INT(1, c.writes);
    TEST_ASSERT_EQUAL_size_t(sizeof(s_frame), c.sent);
}

/* PERF11: 64 bytes a write, 100 ms each (25.6 s for the frame): abandoned once the 15 s are
 * over, within one write's time - before, it ran to the end. */
static void test_send_slow_progress_keeps_the_limit(void)
{
    conn_t c = {.now = 0, .per_call = 64};
    TEST_ASSERT_FALSE(send(&c));
    TEST_ASSERT_LESS_THAN_size_t(sizeof(s_frame), c.sent);
    TEST_ASSERT_TRUE(c.now <= BUDGET_US + STEP_US);
}

/* PERF11: a connection that never takes anything (WANT_READ / WANT_WRITE) waits between tries
 * and gives up at the limit. */
static void test_send_would_block_gives_up(void)
{
    conn_t c = {.now = 0, .per_call = 0};
    TEST_ASSERT_FALSE(send(&c));
    TEST_ASSERT_EQUAL_size_t(0, c.sent);
    TEST_ASSERT_EQUAL_INT(c.writes, c.pauses);
    TEST_ASSERT_TRUE(c.now <= BUDGET_US + STEP_US + 1000);
}

/* PERF11: a broken connection ends it at once. */
static void test_send_error_ends_it(void)
{
    conn_t c = {.now = 0, .per_call = 64, .fail_at = 3};
    TEST_ASSERT_FALSE(send(&c));
    TEST_ASSERT_EQUAL_INT(3, c.writes);
}

/* PERF11: a stop asked while parts are going out (the reproduction: at the fifth write) ends
 * it before the next write; and one asked while waiting, before the next try. */
static void test_send_stop_is_seen_after_every_write(void)
{
    conn_t c = {.now = 0, .per_call = 64, .stop_at = 5};
    TEST_ASSERT_FALSE(send(&c));
    TEST_ASSERT_EQUAL_INT(5, c.writes);
    conn_t d = {.now = 0, .per_call = 0, .stop_in_pause = 2};
    TEST_ASSERT_FALSE(send(&d));
    TEST_ASSERT_EQUAL_INT(2, d.writes);
    conn_t e = {.now = 0, .per_call = sizeof(s_frame), .stop = true};
    TEST_ASSERT_FALSE(send(&e)); /* asked before it began: nothing goes */
    TEST_ASSERT_EQUAL_INT(0, e.writes);
}

/* PERF11 (repair): esp-tls's write keeps sending everything it is handed, record after record,
 * as long as any of it goes - 1 s per KB here, a reader taking a little at a time. Handed the
 * whole 16 KB picture it held the worker 16 s; in 1 KB pieces the limit is checked between
 * them, so the message ends within the limit plus one piece. */
#define PIECE 1024u
typedef struct {
    int64_t now;
    size_t largest, sent;
    int writes;
} tls_t;
static int tls_write(void *ctx, const uint8_t *p, size_t n)
{
    (void)p;
    tls_t *t = ctx;
    t->writes++;
    t->largest = n > t->largest ? n : t->largest;
    t->now += (int64_t)n * 1000000 / 1024; /* all of it goes, slowly */
    t->sent += n;
    return (int)n;
}
static int64_t tls_now(void *ctx)
{
    return ((tls_t *)ctx)->now;
}
static bool tls_stopping(void *ctx)
{
    (void)ctx;
    return false;
}
static void tls_pause(void *ctx)
{
    (void)ctx;
}
static void test_send_tls_write_in_pieces_keeps_the_limit(void)
{
    tls_t t = {0};
    const web_send_io_t io = {.write = tls_write, .now_us = tls_now, .stopping = tls_stopping,
                              .pause = tls_pause, .ctx = &t, .piece = PIECE};
    const int64_t budget = 3500 * 1000LL; /* 3.5 s */
    TEST_ASSERT_FALSE(web_send_all(&io, s_frame, sizeof(s_frame), budget));
    TEST_ASSERT_EQUAL_size_t(PIECE, t.largest);
    TEST_ASSERT_EQUAL_INT(4, t.writes); /* 0, 1, 2 and 3 s: the fifth would start past 3.5 s */
    TEST_ASSERT_TRUE(t.now <= budget + 1000000);
    /* The whole picture in pieces, with time enough: every byte, none handed twice. */
    tls_t u = {0};
    const web_send_io_t all = {.write = tls_write, .now_us = tls_now, .stopping = tls_stopping,
                               .pause = tls_pause, .ctx = &u, .piece = PIECE};
    TEST_ASSERT_TRUE(web_send_all(&all, s_frame, sizeof(s_frame), BUDGET_US + BUDGET_US));
    TEST_ASSERT_EQUAL_size_t(sizeof(s_frame), u.sent);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(s_frame) / PIECE), u.writes);
    /* A last piece shorter than the rest. */
    tls_t v = {0};
    const web_send_io_t odd = {.write = tls_write, .now_us = tls_now, .stopping = tls_stopping,
                               .pause = tls_pause, .ctx = &v, .piece = PIECE};
    TEST_ASSERT_TRUE(web_send_all(&odd, s_frame, PIECE + 7u, BUDGET_US));
    TEST_ASSERT_EQUAL_size_t(PIECE + 7u, v.sent);
    TEST_ASSERT_EQUAL_INT(2, v.writes);
}

/* PERF11: a write that claims more than it was given is a broken connection; nothing to send
 * is done at once. */
static int overlong_write(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    (void)p;
    return (int)n + 1;
}
static void test_send_overlong_write_is_broken(void)
{
    conn_t c = {.now = 0};
    const web_send_io_t io = {overlong_write, fake_now, fake_stopping, fake_pause, &c, 0};
    TEST_ASSERT_FALSE(web_send_all(&io, s_frame, sizeof(s_frame), BUDGET_US));
    TEST_ASSERT_TRUE(web_send_all(&io, s_frame, 0, BUDGET_US));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_attach_hands_the_child_over);
    RUN_TEST(test_attach_frees_the_child_when_the_key_cannot_be_stored);
    RUN_TEST(test_attach_without_parent_or_child);
    RUN_TEST(test_builder_leaks_nothing_whichever_allocation_fails);
    RUN_TEST(test_partition_events_only_to_their_viewers);
    RUN_TEST(test_installer_and_system_master_see_everything);
    RUN_TEST(test_system_events);
    RUN_TEST(test_keep_filters_events_only);
    RUN_TEST(test_send_fast);
    RUN_TEST(test_send_slow_progress_keeps_the_limit);
    RUN_TEST(test_send_would_block_gives_up);
    RUN_TEST(test_send_error_ends_it);
    RUN_TEST(test_send_stop_is_seen_after_every_write);
    RUN_TEST(test_send_overlong_write_is_broken);
    RUN_TEST(test_send_tls_write_in_pieces_keeps_the_limit);
    return UNITY_END();
}
