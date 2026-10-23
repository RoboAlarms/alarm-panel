/* End to end (docs/features/41 DESIGN.md sections 2, 3 and 5): a real engine (sim.h) -> the
 * observer (auto_watch) -> the runner (auto_run) -> fake device ports that answer later.
 * HCTL-009: the triggers as a person meets them; AUT-006: each action sent once, answered;
 * AUT-007: a silent alarm and a device action make nothing happen; the failed port reaches the
 * engine as the service wires it, and its event makes no occurrence. */
#include <string.h>

#include "automation/auto_run.h"
#include "automation/auto_watch.h"
#include "automation/automation.h"
#include "home_control/home_control.h"
#include "sim.h"

#define MAX_SENDS 32u

typedef struct {
    char ref[DEV_REF_LEN];
    uint8_t action;
    int16_t value;
    uint32_t req;
    alarm_ms_t at;
} send_t;

static sim_t S;
static auto_watch_t W;
static auto_run_t R;
static alarm_snapshot_t SNAP;
static auto_zone_t Z[ALARM_MAX_ZONES];
static auto_rule_t RULES[AUTO_MAX];
static size_t NR;
static unsigned SEEN;
static send_t SENDS[MAX_SENDS];
static unsigned NS, ANSWERED;
static uint32_t NEXT_REQ;
static bool LOCKS_BUSY;
static unsigned N_ARM;

static int port_act(void *ctx, const auto_action_t *a, bool less_secure, uint32_t *req)
{
    (void)ctx;
    (void)less_secure;
    if (LOCKS_BUSY && strncmp(a->ref, "lock.", 5) == 0) {
        *req = 0;
        return HC_ERR_BUSY;
    }
    TEST_ASSERT_LESS_THAN_UINT(MAX_SENDS, NS);
    *req = ++NEXT_REQ;
    send_t *s = &SENDS[NS++];
    strcpy(s->ref, a->ref);
    s->action = a->dev_action;
    s->value = a->value;
    s->req = *req;
    s->at = S.now;
    return HC_OK;
}

static bool port_kind(void *ctx, uint8_t source, const char *ref, uint8_t *kind)
{
    (void)ctx;
    (void)source;
    if (strncmp(ref, "light.", 6) == 0) {
        *kind = DEV_LIGHT;
    } else if (strncmp(ref, "lock.", 5) == 0) {
        *kind = DEV_LOCK;
    } else if (strncmp(ref, "scene.", 6) == 0) {
        *kind = DEV_SCENE;
    } else {
        return false;
    }
    return true;
}

static bool port_settled(void *ctx, uint8_t source)
{
    (void)ctx;
    (void)source;
    return true;
}

/* As the service does: ALARM_EV_AUTOMATION_FAILED into the real engine. */
static void port_failed(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action, uint16_t zone)
{
    (void)ctx;
    const alarm_source_t src = {ALARM_SRC_AUTOMATION, uid};
    EXPECT_RESULT(ALARM_OK, alarm_engine_platform_event_ex(&S.eng, ALARM_EV_AUTOMATION_FAILED, zone,
                                                           reason, dev_action, &src, 0, S.now));
}

static alarm_result_t port_arm(void *ctx, uint16_t uid, uint8_t p, uint8_t level, uint8_t flags,
                               uint8_t maker, bool *already, uint16_t *zone)
{
    (void)flags, (void)ctx, (void)uid, (void)p, (void)level, (void)maker, (void)already, (void)zone;
    N_ARM++;
    return ALARM_ERR_NOT_AUTHORIZED;
}

static auto_local_t local_now(void)
{
    const auto_local_t lt = {true, 1767225600 + (int64_t)(S.now / 1000u), 20454, 600, 4};
    return lt;
}

/* As the service does: each engine call's events, then its snapshot, into the observer; the
 * occurrences into the runner; the device answers 300 ms later; then the runner's tick. */
static void pump(void)
{
    auto_occ_t occ[16];
    const auto_local_t lt = local_now();
    while (SEEN < S.n_events) {
        const size_t k = auto_watch_event(&W, &S.events[SEEN++], occ, 16);
        for (size_t i = 0; i < k; i++) {
            auto_run_occurrence(&R, &occ[i], &lt, S.now);
        }
    }
    alarm_engine_snapshot(&S.eng, &SNAP);
    SNAP.config_seq = 1;
    const size_t k = auto_watch_snapshot(&W, &SNAP, occ, 16);
    for (size_t i = 0; i < k; i++) {
        auto_run_occurrence(&R, &occ[i], &lt, S.now);
    }
    while (ANSWERED < NS && S.now >= SENDS[ANSWERED].at + 300u) {
        auto_run_result(&R, SENDS[ANSWERED].req, true, 0, S.now);
        ANSWERED++;
    }
    auto_run_tick(&R, S.now);
}

static void advance(unsigned seconds)
{
    for (unsigned i = 0; i < seconds; i++) {
        sim_advance(&S, SEC(1));
        pump();
    }
}

static auto_action_t *add(auto_rule_t *r, uint8_t kind, uint8_t act, const char *ref)
{
    auto_action_t *a = &r->then[r->n++];
    memset(a, 0, sizeof(*a));
    a->kind = AUTO_ACT_DEVICE;
    a->source = DSRC_HA;
    a->dev_kind = kind;
    a->dev_action = act;
    a->value = -1;
    a->confirmed = 1;
    strcpy(a->ref, ref);
    return a;
}

static auto_rule_t *rule(uint8_t trig)
{
    auto_rule_t *r = &RULES[NR];
    memset(r, 0, sizeof(*r));
    r->uid = (uint16_t)(NR + 1u);
    r->slot = (uint8_t)(NR + 1u);
    r->when.kind = trig;
    NR++;
    return r;
}

static void load(void)
{
    for (uint16_t z = 0; z < ALARM_MAX_ZONES; z++) {
        Z[z].type = (uint8_t)S.cfg.zones[z].type;
        Z[z].partition = S.cfg.zones[z].partition;
        Z[z].dev_class = S.cfg.zones[z].device_class;
    }
    const auto_env_t env = {0x01u, false, Z, true, false, 0u};
    auto_run_rules(&R, RULES, NR, &env);
    for (size_t i = 0; i < NR; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(AUTO_OK, auto_rule_check(&RULES[i], &env), "rule");
    }
}

static unsigned sends_to(const char *ref, uint8_t action)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NS; i++) {
        n += strcmp(SENDS[i].ref, ref) == 0 && SENDS[i].action == action ? 1u : 0u;
    }
    return n;
}

void setUp(void)
{
    sim_start(&S, NULL, NULL);
    auto_watch_init(&W, S.now, 0);
    const auto_ports_t p = {NULL, port_act, port_settled, port_kind, port_arm, NULL, port_failed,
                            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    auto_run_init(&R, &p, S.now);
    NR = 0;
    SEEN = 0;
    NS = 0;
    ANSWERED = 0;
    NEXT_REQ = 0;
    LOCKS_BUSY = false;
    N_ARM = 0;
    pump();
}

void tearDown(void) {}

/* HCTL-009 / AUT-006: the front door opens -> the porch light's Turn on goes out once with
 * value -1, is answered, and its Off follows five minutes later. */
static void test_door_opens_porch_light_then_off(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED);
    r->when.zone = Z_FRONT;
    add(r, DEV_LIGHT, ACT_ON, "light.porch")->off_after_min = 5;
    load();
    sim_fault(&S, Z_FRONT);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, NS);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, SENDS[0].action);
    TEST_ASSERT_EQUAL_INT16(-1, SENDS[0].value);
    advance(2);
    auto_status_t st;
    auto_run_status(&R, 1, &st);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, st.result);
    sim_restore(&S, Z_FRONT);
    pump();
    advance(297);
    TEST_ASSERT_EQUAL_UINT(1, NS);
    advance(2);
    TEST_ASSERT_EQUAL_UINT(2, NS);
    TEST_ASSERT_EQUAL_UINT(1, sends_to("light.porch", ACT_OFF));
    advance(60);
    TEST_ASSERT_EQUAL_UINT(2, NS);
}

/* HCTL-009 / AUT-006: armed Away, once exit time is over -> the lock and the scene go out
 * once each; not while exit time runs, and not for the door walked through on the way out. */
static void test_armed_away_lock_and_scene_once(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ARMED);
    r->when.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY);
    add(r, DEV_LOCK, ACT_LOCK, "lock.front");
    add(r, DEV_SCENE, ACT_RUN, "scene.away");
    load();
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    pump();
    advance(5);
    sim_fault(&S, Z_FRONT);
    pump();
    advance(3);
    sim_restore(&S, Z_FRONT);
    pump();
    advance(40);
    TEST_ASSERT_EQUAL_UINT(0, NS);
    advance(20);
    EXPECT_STATE(&S, 1, ALARM_STATE_ARMED);
    TEST_ASSERT_EQUAL_UINT(1, sends_to("lock.front", ACT_LOCK));
    TEST_ASSERT_EQUAL_UINT(1, sends_to("scene.away", ACT_RUN));
    advance(120);
    TEST_ASSERT_EQUAL_UINT(2, NS);
    auto_status_t st;
    auto_run_status(&R, 1, &st);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, st.result);
}

/* HCTL-009 / AUT-007: a silent panic and a silent zone alarm start nothing; a police panic
 * does, once. */
static void test_silent_panic_does_nothing(void)
{
    add(rule(AUTO_TRIG_ALARM), DEV_LIGHT, ACT_ON, "light.all");
    load();
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_SILENT));
    pump();
    sim_fault(&S, Z_HOLDUP);
    pump();
    advance(30);
    TEST_ASSERT_EQUAL_UINT(0, NS);
    EXPECT_RESULT(ALARM_OK, sim_panic(&S, 1, ALARM_PANIC_POLICE));
    pump();
    advance(5);
    TEST_ASSERT_EQUAL_UINT(1, NS);
}

/* AUT-007 / AUT-009: with the failed port wired as the service wires it (it raises the engine's
 * event), a device action that worked doesn't call it (this test's device port never touches
 * the engine, so only the failed-port path is proved); a Lock that failed makes exactly one
 * ALARM_EV_AUTOMATION_FAILED, and that event makes no occurrence (no further run, no further
 * send). The arm port is never called in phase A. */
static void test_device_actions_make_no_events(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED);
    r->when.zone = Z_WINDOW;
    add(r, DEV_LIGHT, ACT_ON, "light.hall");
    auto_rule_t *k = rule(AUTO_TRIG_ZONE_OPENED);
    k->when.zone = Z_FRONT;
    add(k, DEV_LOCK, ACT_LOCK, "lock.front");
    load();
    sim_fault(&S, Z_WINDOW);
    pump();
    const unsigned events = S.n_events;
    TEST_ASSERT_EQUAL_UINT(1, NS);
    advance(10);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(events, S.n_events, "a sent action called the failed port");
    TEST_ASSERT_EQUAL_UINT(1, NS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sim_count_events(&S, ALARM_EV_AUTOMATION_FAILED),
                                   "a sent action called the failed port");
    /* The lock is busy: the failed port reaches the engine once. */
    LOCKS_BUSY = true;
    sim_fault(&S, Z_FRONT);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, sim_count_events(&S, ALARM_EV_AUTOMATION_FAILED));
    const alarm_event_t *ev = sim_last_event(&S, ALARM_EV_AUTOMATION_FAILED);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_BUSY, ev->arg);
    TEST_ASSERT_EQUAL_UINT16(Z_FRONT, ev->zone);
    const unsigned after = S.n_events;
    advance(40);
    TEST_ASSERT_EQUAL_UINT(after, S.n_events);
    TEST_ASSERT_EQUAL_UINT(1, NS);
    TEST_ASSERT_EQUAL_UINT(0, N_ARM);
    auto_status_t st;
    auto_run_status(&R, 2, &st);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_BUSY, st.result);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_door_opens_porch_light_then_off);
    RUN_TEST(test_armed_away_lock_and_scene_once);
    RUN_TEST(test_silent_panic_does_nothing);
    RUN_TEST(test_device_actions_make_no_events);
    return UNITY_END();
}
