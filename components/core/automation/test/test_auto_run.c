/* auto_run (docs/features/41 DESIGN.md sections 2, 3 and 5): occurrences and crossed minutes in,
 * device actions out through fake ports that record each send and answer later.
 * HCTL-009: the triggers and their filters; AUT-006: sending and outcomes; AUT-007: loops and
 * cascades (the device gap, the cap, no action makes an occurrence); AUT-002 (HCTL-011): the live
 * kind and the registry's refusals at run time; AUT-015: the installer option and the maker
 * re-checked before each less secure send. */
#include <stdio.h>
#include <string.h>

#include "automation/auto_run.h"
#include "automation/auto_watch.h"
#include "automation/automation.h"
#include "home_control/home_control.h"
#include "sim.h"

#define T0 UINT64_C(1000000)
#define S_(s) ((uint64_t)(s) * UINT64_C(1000))
#define MAX_SENDS 64u
#define MAX_DEVS 24u

typedef struct {
    char ref[DEV_REF_LEN];
    uint8_t action;
    int16_t value;
    bool less_secure;  /* the flag device_act was given */
    uint32_t req;
    uint64_t at;
} send_t;

typedef struct {
    char ref[DEV_REF_LEN];
    uint8_t source;
    uint8_t kind;
    uint8_t listed;
    int answer;  /* hc_result_t device_act returns */
} fdev_t;

typedef struct {
    send_t sends[MAX_SENDS];
    unsigned n_sends;  /* HC_OK sends */
    unsigned n_calls;  /* every device_act call */
    fdev_t devs[MAX_DEVS];
    unsigned n_devs;
    uint32_t next_req;
    bool settled;
    uint64_t now;
    unsigned n_failed;
    uint16_t failed_uid;
    uint8_t failed_reason, failed_action;
    uint16_t failed_zone;
    unsigned n_arm;
    bool maker_gone[ALARM_MAX_USERS + 1u]; /* maker_ok says no for these slots */
    unsigned n_maker;                      /* maker_ok calls */
    uint8_t maker_asked;
    bool live_off;                         /* maker_ok reads the installer option as off */
    /* PL04: the revocation gate, as the service keeps it */
    auto_revoke_t revoked;
    unsigned n_begin, n_end;
    unsigned in_gate;                      /* between send_begin and send_end */
    bool gated;                            /* the gate's ports are installed */
    uint16_t revoke_on_send;               /* revoke this uid during the next send (an edit
                                              answered while the send is in progress) */
} fake_t;

static fake_t F;
static auto_run_t R;
static auto_zone_t Z[ALARM_MAX_ZONES];
static auto_env_t ENV;
static auto_rule_t RULES[AUTO_MAX];
static size_t NR;

static fdev_t *dev_find(uint8_t source, const char *ref)
{
    for (unsigned i = 0; i < F.n_devs; i++) {
        if (F.devs[i].source == source && strcmp(F.devs[i].ref, ref) == 0) {
            return &F.devs[i];
        }
    }
    return NULL;
}

/* The registry lists this device (or changes its kind or its answer). */
static fdev_t *dev(uint8_t source, const char *ref, uint8_t kind)
{
    fdev_t *d = dev_find(source, ref);
    if (d == NULL) {
        TEST_ASSERT_LESS_THAN_UINT(MAX_DEVS, F.n_devs);
        d = &F.devs[F.n_devs++];
        memset(d, 0, sizeof(*d));
        strcpy(d->ref, ref);
        d->source = source;
        d->answer = HC_OK;
    }
    d->kind = kind;
    d->listed = 1u;
    return d;
}

static int port_act(void *ctx, const auto_action_t *a, bool less_secure, uint32_t *req)
{
    fake_t *f = ctx;
    f->n_calls++;
    if (f->gated) {
        TEST_ASSERT_EQUAL_UINT(1, f->in_gate); /* PL04: every send inside the gate */
    }
    if (f->revoke_on_send != 0u) {
        auto_revoke_add(&f->revoked, f->revoke_on_send);
        f->revoke_on_send = 0u;
    }
    const fdev_t *d = dev_find(a->source, a->ref);
    const int ans = d == NULL || !d->listed ? (int)HC_ERR_UNKNOWN : d->answer;
    if (ans != (int)HC_OK) {
        *req = 0;
        return ans;
    }
    *req = ++f->next_req;
    TEST_ASSERT_LESS_THAN_UINT(MAX_SENDS, f->n_sends);
    send_t *s = &f->sends[f->n_sends++];
    strcpy(s->ref, a->ref);
    s->action = a->dev_action;
    s->value = a->value;
    s->less_secure = less_secure;
    s->req = *req;
    s->at = f->now;
    return HC_OK;
}

static bool port_kind(void *ctx, uint8_t source, const char *ref, uint8_t *kind)
{
    (void)ctx;
    const fdev_t *d = dev_find(source, ref);
    if (d == NULL || !d->listed) {
        return false;
    }
    *kind = d->kind;
    return true;
}

static bool port_settled(void *ctx, uint8_t source)
{
    (void)source;
    return ((fake_t *)ctx)->settled;
}

static void port_failed(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action, uint16_t zone)
{
    fake_t *f = ctx;
    f->n_failed++;
    f->failed_uid = uid;
    f->failed_reason = reason;
    f->failed_action = dev_action;
    f->failed_zone = zone;
}

static alarm_result_t port_arm(void *ctx, uint16_t uid, uint8_t p, uint8_t level, uint8_t flags,
                               uint8_t maker, bool *already, uint16_t *zone)
{
    (void)flags, (void)uid, (void)p, (void)level, (void)maker, (void)already, (void)zone;
    ((fake_t *)ctx)->n_arm++;
    return ALARM_OK;
}

static bool port_maker(void *ctx, uint8_t user, bool *option_on)
{
    fake_t *f = ctx;
    TEST_ASSERT_TRUE(*option_on); /* asked only while the env's option is on */
    if (f->live_off) {
        *option_on = false;
    }
    f->n_maker++;
    f->maker_asked = user;
    return user >= 1u && user <= ALARM_MAX_USERS && !f->maker_gone[user];
}

/* PL04: the gate as the service's: revoked uids wait; begin and end always pair. */
static bool port_begin(void *ctx, uint16_t uid)
{
    fake_t *f = ctx;
    TEST_ASSERT_EQUAL_UINT(0, f->in_gate);
    f->n_begin++;
    f->in_gate++;
    return !auto_revoke_has(&f->revoked, uid);
}

static void port_end(void *ctx)
{
    fake_t *f = ctx;
    TEST_ASSERT_EQUAL_UINT(1, f->in_gate);
    f->n_end++;
    f->in_gate--;
}

static void start(void)
{
    memset(&F, 0, sizeof(F));
    F.now = T0;
    F.settled = true;
    F.gated = true;
    const auto_ports_t p = {&F,       port_act, port_settled, port_kind,  port_arm, NULL,
                            port_failed, port_maker, NULL,    NULL,       port_begin, port_end, NULL, NULL, NULL, NULL, NULL};
    auto_run_init(&R, &p, T0);
    memset(Z, 0, sizeof(Z));
    Z[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    Z[1] = (auto_zone_t){ALARM_ZT_PERIMETER, 2, ALARM_DEVCLASS_WINDOW};
    Z[2] = (auto_zone_t){ALARM_ZT_INTERIOR_FOLLOWER, 1, ALARM_DEVCLASS_MOTION};
    Z[5] = (auto_zone_t){ALARM_ZT_FIRE, 1, ALARM_DEVCLASS_SMOKE};
    ENV = (auto_env_t){0x03u, false, Z, true, false, 0u};
    NR = 0;
    memset(RULES, 0, sizeof(RULES));
}

static void load(void)
{
    auto_run_rules(&R, RULES, NR, &ENV);
}

static void action(auto_action_t *a, uint8_t source, uint8_t kind, uint8_t act, const char *ref)
{
    memset(a, 0, sizeof(*a));
    a->kind = AUTO_ACT_DEVICE;
    a->source = source;
    a->dev_kind = kind;
    a->dev_action = act;
    a->value = -1;
    a->confirmed = 1;
    strcpy(a->ref, ref);
    strcpy(a->name, "Device");
}

/* A new rule (uid = its position + 1) with one Home Assistant action, the device listed. */
static auto_rule_t *rule(uint8_t trig, uint8_t kind, uint8_t act, const char *ref)
{
    TEST_ASSERT_LESS_THAN_UINT(AUTO_MAX, NR);
    auto_rule_t *r = &RULES[NR];
    memset(r, 0, sizeof(*r));
    r->uid = (uint16_t)(NR + 1u);
    r->slot = (uint8_t)(NR + 1u);
    r->n = 1;
    r->when.kind = trig;
    if (trig == AUTO_TRIG_ZONE_OPENED || trig == AUTO_TRIG_ZONE_CLOSED) {
        r->when.zone = 1;
    }
    action(&r->then[0], DSRC_HA, kind, act, ref);
    dev(DSRC_HA, ref, kind);
    NR++;
    return r;
}

static auto_local_t clock_at(uint16_t minute)
{
    auto_local_t lt = {true, 1767225600 + 86400 * 10, 20464, minute, 3};
    return lt;
}

static void occ(uint8_t kind, uint8_t partition, uint8_t level, uint16_t zone, uint64_t now)
{
    F.now = now;
    const auto_occ_t o = {kind, partition, level, 0, zone, now, 0};
    const auto_local_t lt = clock_at(600);
    auto_run_occurrence(&R, &o, &lt, now);
}

static void tick(uint64_t now)
{
    F.now = now;
    auto_run_tick(&R, now);
}

/* Tick every second from..to (inclusive). */
static void run_until(uint64_t from, uint64_t to)
{
    for (uint64_t t = from; t <= to; t += UINT64_C(1000)) {
        tick(t);
    }
}

static auto_status_t status(uint16_t uid)
{
    auto_status_t st;
    auto_run_status(&R, uid, &st);
    return st;
}

#define EXPECT_RES(uid, res) TEST_ASSERT_EQUAL_UINT8((res), status(uid).result)

void setUp(void)
{
    start();
}

void tearDown(void) {}

/* ---- matching (HCTL-009) ------------------------------------------------------------------- */

/* HCTL-009 / AUT-006: a door opens -> the light's Turn on goes out once, with value -1; the
 * answer makes it done; it is never sent again however long the runner ticks. */
static void test_zone_opened_sends_once(void)
{
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    EXPECT_RES(1, AUTO_RES_NONE);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.porch", F.sends[0].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, F.sends[0].action);
    TEST_ASSERT_EQUAL_INT16(-1, F.sends[0].value);
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + 300);
    EXPECT_RES(1, AUTO_RES_DONE);
    TEST_ASSERT_EQUAL_INT64(((int64_t)1767225600 + 86400 * 10) * 1000 + 300, status(1).at_epoch_ms);
    run_until(T0, T0 + S_(120));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0 + S_(120)));
    /* Another zone, or a zone close, does nothing. */
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 3, T0 + S_(200));
    occ(AUTO_TRIG_ZONE_CLOSED, 0, 0, 1, T0 + S_(201));
    run_until(T0 + S_(201), T0 + S_(220));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
}

/* HCTL-009: the partition mask (a zone's from the zones), the ARMED level mask. */
static void test_partition_and_level_masks(void)
{
    auto_rule_t *a = rule(AUTO_TRIG_ARMED, DEV_LOCK, ACT_LOCK, "lock.front");
    a->when.partitions = 0x01u;
    a->when.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY);
    auto_rule_t *z = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.hall");
    z->when.partitions = 0x02u; /* zone 1 is in partition 1 */
    auto_rule_t *w = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.study");
    w->when.zone = 2;
    w->when.partitions = 0x02u; /* zone 2 is in partition 2 */
    load();
    occ(AUTO_TRIG_ARMED, 2, ALARM_LEVEL_AWAY, 0, T0);
    occ(AUTO_TRIG_ARMED, 1, ALARM_LEVEL_STAY, 0, T0 + 1);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + 2);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + 3);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    occ(AUTO_TRIG_ARMED, 1, ALARM_LEVEL_AWAY, 0, T0 + 4);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 2, T0 + 5);
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[0].ref);
    TEST_ASSERT_EQUAL_STRING("light.study", F.sends[1].ref);
}

/* HCTL-009 / HCTL-011: a zone retyped to smoke stops its automation, whether the zones come in
 * through auto_run_rules (the check) or the runner checks them at the occurrence. */
static void test_zone_retyped_stops(void)
{
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    Z[0] = (auto_zone_t){ALARM_ZT_FIRE, 1, ALARM_DEVCLASS_SMOKE};
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    EXPECT_RES(1, AUTO_RES_NONE);
    Z[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    load();
    /* The runner's own copy, retyped behind its back: the match re-checks it. */
    R.zones[0].dev_class = ALARM_DEVCLASS_PANIC_BUTTON;
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    /* A motion zone may open but never "close". */
    auto_rule_t *c = rule(AUTO_TRIG_ZONE_CLOSED, DEV_LIGHT, ACT_OFF, "light.hall");
    c->when.zone = 3;
    load();
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_INVALID, auto_run_try(&R, c->uid, T0 + S_(50)));
}

/* HCTL-009: "Only between" 22:00-06:00 wraps past midnight; with a window and no valid clock
 * the occurrence does nothing and the status says so. */
static void test_window_wrap_and_invalid_clock(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch");
    r->when.win_from = 22 * 60 + 1;
    r->when.win_to = 6 * 60 + 1;
    load();
    const uint16_t minutes[] = {23 * 60, 5 * 60 + 59, 6 * 60, 12 * 60, 21 * 60 + 59, 22 * 60, 0};
    const bool inside[] = {true, true, false, false, false, true, true};
    unsigned want = 0;
    for (size_t i = 0; i < sizeof(minutes) / sizeof(minutes[0]); i++) {
        const uint64_t now = T0 + S_(20 * i);
        F.now = now;
        const auto_occ_t o = {AUTO_TRIG_DISARMED, 1, 0, 0, 0, now, 0};
        const auto_local_t lt = clock_at(minutes[i]);
        auto_run_occurrence(&R, &o, &lt, now);
        want += inside[i] ? 1u : 0u;
        TEST_ASSERT_EQUAL_UINT_MESSAGE(want, F.n_calls, "window");
    }
    const auto_occ_t o = {AUTO_TRIG_DISARMED, 1, 0, 0, 0, T0 + S_(500), 0};
    auto_local_t bad = clock_at(23 * 60);
    bad.valid = false;
    auto_run_occurrence(&R, &o, &bad, T0 + S_(500));
    auto_run_occurrence(&R, &o, NULL, T0 + S_(501));
    TEST_ASSERT_EQUAL_UINT(want, F.n_calls);
    EXPECT_RES(1, AUTO_RES_NO_CLOCK);
    TEST_ASSERT_EQUAL_INT64(0, status(1).at_epoch_ms);
    /* Without a window an invalid clock doesn't matter. */
    RULES[0].when.win_from = 0;
    RULES[0].when.win_to = 0;
    load();
    auto_run_occurrence(&R, &o, NULL, T0 + S_(520));
    TEST_ASSERT_EQUAL_UINT(want + 1u, F.n_calls);
}

/* HCTL-009 / AUT-007: a zone trigger runs at most every 30 s; a repeat inside only restarts a
 * running "Turn it off after" timer; the Off goes to the same device with value -1. */
static void test_cooldown_and_off_timer_restart(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch");
    r->then[0].off_after_min = 5;
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_UINT64(T0 + AUTO_RESULT_WAIT_MS, auto_run_next_deadline(&R, T0)); /* result */
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + 200);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(300), auto_run_next_deadline(&R, T0 + 200));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(20));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(320), auto_run_next_deadline(&R, T0 + S_(20)));
    tick(T0 + S_(300));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    tick(T0 + S_(320));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.porch", F.sends[1].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[1].action);
    TEST_ASSERT_EQUAL_INT16(-1, F.sends[1].value);
    /* After the cooldown it runs again, and its timer starts again. */
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(400));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, F.sends[2].action);
    run_until(T0 + S_(401), T0 + S_(700));
    TEST_ASSERT_EQUAL_UINT(4, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[3].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(700), F.sends[3].at);
}

/* HCTL-009: a door closing runs 10 s later only if it didn't open meanwhile. */
static void test_close_hold_cancelled_by_reopen(void)
{
    rule(AUTO_TRIG_ZONE_CLOSED, DEV_LOCK, ACT_LOCK, "lock.front");
    load();
    occ(AUTO_TRIG_ZONE_CLOSED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(10), auto_run_next_deadline(&R, T0));
    tick(T0 + S_(5));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(6));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0 + S_(6)));
    run_until(T0 + S_(6), T0 + S_(20));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    occ(AUTO_TRIG_ZONE_CLOSED, 0, 0, 1, T0 + S_(21));
    tick(T0 + S_(30));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    tick(T0 + S_(31));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_LOCK, F.sends[0].action);
    /* Another close inside the 30 s cooldown: nothing. */
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(35));
    occ(AUTO_TRIG_ZONE_CLOSED, 0, 0, 1, T0 + S_(36));
    run_until(T0 + S_(36), T0 + S_(50));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
}

/* ---- sending and outcomes (AUT-006) -------------------------------------------------------- */

/* AUT-006: a Home Assistant device not listed yet is held and tried every 2 s; once it appears
 * it is sent exactly once. */
static void test_ha_hold_then_sent_when_listed(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    F.devs[0].listed = 0;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls); /* unlisted: the registry isn't asked */
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(2), auto_run_next_deadline(&R, T0));
    run_until(T0 + S_(1), T0 + S_(8));
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(10), auto_run_next_deadline(&R, T0 + S_(8)));
    F.devs[0].listed = 1;
    tick(T0 + S_(9));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    tick(T0 + S_(10));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    run_until(T0 + S_(11), T0 + S_(59));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
    EXPECT_RES(1, AUTO_RES_NONE);
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + S_(60));
    EXPECT_RES(1, AUTO_RES_DONE);
}

/* AUT-006: after 30 s held, missing only when the ref is still unlisted and the source settled;
 * otherwise unreachable. Offline from the registry is held the same way; nothing is sent. */
static void test_ha_hold_missing_or_unreachable(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.front");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.hall");
    load();
    F.devs[0].listed = 0;
    F.devs[1].listed = 0;
    F.devs[2].answer = HC_ERR_OFFLINE;
    F.settled = true;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    run_until(T0 + S_(1), T0 + S_(29));
    EXPECT_RES(1, AUTO_RES_NONE);
    F.settled = false; /* rule 1's final check sees an unsettled source ... */
    tick(T0 + S_(30));
    EXPECT_RES(1, AUTO_RES_UNREACHABLE);
    EXPECT_RES(2, AUTO_RES_UNREACHABLE);
    EXPECT_RES(3, AUTO_RES_UNREACHABLE);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_UNREACHABLE, status(1).reason);
    TEST_ASSERT_EQUAL_UINT(16, F.n_calls); /* light.hall: tried at 0, 2, .. 30 s */
    TEST_ASSERT_EQUAL_UINT(0, F.n_sends);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0 + S_(30)));
    /* ... and with a settled source an unlisted ref is missing. */
    F.settled = true;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(100));
    run_until(T0 + S_(101), T0 + S_(130));
    EXPECT_RES(1, AUTO_RES_MISSING);
    EXPECT_RES(2, AUTO_RES_MISSING);
    EXPECT_RES(3, AUTO_RES_UNREACHABLE); /* listed, but offline */
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_MISSING, status(1).reason);
    TEST_ASSERT_EQUAL_UINT(0, F.n_sends);
}

/* AUT-006: a source other than Home Assistant (Z-Wave's place, phase B) fails at once. */
static void test_other_source_fails_at_once(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "zw.5.lock");
    r->then[0].source = DSRC_ZIGBEE;
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "zw.6.light")->then[0].source = DSRC_ZIGBEE;
    dev(DSRC_ZIGBEE, "zw.5.lock", DEV_LOCK)->answer = HC_ERR_OFFLINE;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_UNREACHABLE);
    EXPECT_RES(2, AUTO_RES_MISSING); /* not listed in that source */
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
}

/* AUT-006: the DESIGN's case - Z-Wave (DSRC_ZWAVE) offline or unknown fails at once, not held. */
static void test_zwave_source_fails_at_once(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "zw.5.lock")->then[0].source = DSRC_ZWAVE;
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "zw.6.light")->then[0].source = DSRC_ZWAVE;
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "zw.7.light")->then[0].source = DSRC_ZWAVE;
    dev(DSRC_ZWAVE, "zw.5.lock", DEV_LOCK)->answer = HC_ERR_OFFLINE;
    dev(DSRC_ZWAVE, "zw.7.light", DEV_LIGHT)->answer = HC_ERR_UNKNOWN;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_UNREACHABLE);
    EXPECT_RES(2, AUTO_RES_MISSING);
    EXPECT_RES(3, AUTO_RES_MISSING);
    TEST_ASSERT_EQUAL_UINT(1, F.n_failed); /* the lock */
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_UNREACHABLE, F.failed_reason);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0));
    run_until(T0, T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(2, F.n_calls);
    TEST_ASSERT_EQUAL_UINT(0, F.n_sends);
}

/* AUT-006 / HCTL-011: busy fails at once; ARG is "can't run"; NOT_ALLOWED is the canary. */
static void test_busy_arg_not_allowed(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.a");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.b");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.c");
    load();
    F.devs[0].answer = HC_ERR_BUSY;
    F.devs[1].answer = HC_ERR_ARG;
    F.devs[2].answer = HC_ERR_NOT_ALLOWED;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_BUSY);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_BUSY, status(1).reason);
    EXPECT_RES(2, AUTO_RES_CANT_RUN);
    EXPECT_RES(3, AUTO_RES_NOT_ALLOWED);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_NOT_ALLOWED, status(3).reason);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0));
    run_until(T0, T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(3, F.n_calls);
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed); /* lights: no failed event */
}

/* HCTL-011 / AUT-009: the canary on a Lock or Close reaches the failed port with reason
 * not_allowed (program.c accepts it for those two); a light's only sets the status. */
static void test_not_allowed_lock_reported(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.front");
    rule(AUTO_TRIG_DISARMED, DEV_COVER, ACT_CLOSE, "cover.garage");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.c");
    load();
    for (unsigned i = 0; i < F.n_devs; i++) {
        F.devs[i].answer = HC_ERR_NOT_ALLOWED;
    }
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_NOT_ALLOWED);
    EXPECT_RES(3, AUTO_RES_NOT_ALLOWED);
    TEST_ASSERT_EQUAL_UINT(2, F.n_failed);
    TEST_ASSERT_EQUAL_UINT16(2, F.failed_uid);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_NOT_ALLOWED, F.failed_reason);
    TEST_ASSERT_EQUAL_UINT8(ACT_CLOSE, F.failed_action);
    TEST_ASSERT_EQUAL_UINT(0, F.n_sends);
}

/* AUT-002 layer 4: without a device_kind port the live kind can't be checked - nothing is
 * sent, and the status says can't run. */
static void test_no_kind_port_cant_run(void)
{
    const auto_ports_t p = {&F,   port_act,    port_settled, NULL,
                            port_arm, NULL,   port_failed,  port_maker, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    F.gated = false;
    auto_run_init(&R, &p, T0);
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    EXPECT_RES(1, AUTO_RES_CANT_RUN);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(20)));
    run_until(T0, T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0 + S_(40)));
}

/* AUT-006: results match by request id, in any order; an unknown id is ignored; no answer
 * within AUTO_RESULT_WAIT_MS of the send is no_answer; a refusal is refused. */
static void test_request_id_round_trip_and_no_answer(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.a");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.b");
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.c");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_OFF, "light.d");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(4, F.n_sends);
    auto_run_result(&R, 9999, true, 0, T0 + 10);
    auto_run_result(&R, 0, true, 0, T0 + 10);
    auto_run_result(&R, F.sends[1].req, true, 0, T0 + 20);
    auto_run_result(&R, F.sends[0].req, false, AUTO_WHY_REFUSED, T0 + 30);
    auto_run_result(&R, F.sends[3].req, false, AUTO_WHY_NO_ANSWER, T0 + 30);
    auto_run_result(&R, F.sends[1].req, false, AUTO_WHY_REFUSED, T0 + 40); /* answered already */
    EXPECT_RES(1, AUTO_RES_REFUSED);
    EXPECT_RES(2, AUTO_RES_DONE);
    EXPECT_RES(3, AUTO_RES_NONE);
    EXPECT_RES(4, AUTO_RES_NO_ANSWER);
    /* AUT-009: never before a Z-Wave garage's whole move could end (232 s at its longest) */
    TEST_ASSERT_TRUE(AUTO_RESULT_WAIT_MS >= UINT64_C(232000));
    tick(T0 + AUTO_RESULT_WAIT_MS - 1);
    EXPECT_RES(3, AUTO_RES_NONE);
    tick(T0 + AUTO_RESULT_WAIT_MS);
    EXPECT_RES(3, AUTO_RES_NO_ANSWER);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_NO_ANSWER, status(3).reason);
    TEST_ASSERT_EQUAL_UINT(1, F.n_failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_LOCK, F.failed_action);
    /* A late answer changes nothing. */
    auto_run_result(&R, F.sends[2].req, true, 0, T0 + AUTO_RESULT_WAIT_MS + S_(1));
    EXPECT_RES(3, AUTO_RES_NO_ANSWER);
    TEST_ASSERT_EQUAL_UINT(4, F.n_calls);
}

/* HCTL-011 layer 4: a ref whose live kind differs from the stored one is never sent. */
static void test_live_kind_change_not_sent(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    dev(DSRC_HA, "light.porch", DEV_LOCK);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    EXPECT_RES(1, AUTO_RES_CANT_RUN);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0));
    /* Try it now is checked the same way. */
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(1)));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    /* A held send whose device comes back as another kind isn't sent either. */
    F.devs[0].listed = 0;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(10));
    F.devs[0].listed = 1;
    run_until(T0 + S_(11), T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    EXPECT_RES(1, AUTO_RES_CANT_RUN);
}

/* AUT-006: a newer occurrence replaces the automation's older held send (latest wins). */
static void test_newer_occurrence_replaces_held(void)
{
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.all");
    load();
    F.devs[0].listed = 0;
    occ(AUTO_TRIG_ALARM, 1, 0, 5, T0);
    occ(AUTO_TRIG_ALARM, 1, 0, 6, T0 + S_(3));
    occ(AUTO_TRIG_ALARM, 1, 0, 7, T0 + S_(5));
    F.devs[0].listed = 1;
    run_until(T0 + S_(6), T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
}

/* AUT-006: a held send for an ARMED or DISARMED occurrence is dropped when that partition's
 * state changes again; another partition's change keeps it. */
static void test_held_arming_send_dropped(void)
{
    rule(AUTO_TRIG_ARMED, DEV_LOCK, ACT_LOCK, "lock.front");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.hall")->when.partitions = 0x02u;
    load();
    F.devs[0].listed = 0;
    occ(AUTO_TRIG_ARMED, 1, ALARM_LEVEL_AWAY, 0, T0);
    occ(AUTO_TRIG_DISARMED, 2, 0, 0, T0 + S_(1));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends); /* light.hall */
    /* partition 2's change kept it: its next try is still the deadline, not light.hall's result */
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(2), auto_run_next_deadline(&R, T0 + S_(1)));
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(4));
    F.devs[0].listed = 1;
    run_until(T0 + S_(5), T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    EXPECT_RES(1, AUTO_RES_NONE); /* dropped unsent: "Last run" keeps what it had */
}

/* ---- loops and cascades (AUT-007) ---------------------------------------------------------- */

/* AUT-007: 10 s between two automation actions on one device; the later one waits, it isn't
 * dropped. Other devices aren't held up. */
static void test_device_gap_two_automations(void)
{
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.hall");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_OFF, "light.hall");
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_OFF, "light.porch");
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(3));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.porch", F.sends[1].ref);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(10), auto_run_next_deadline(&R, T0 + S_(3)));
    tick(T0 + S_(10) - 1);
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    tick(T0 + S_(10));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.hall", F.sends[2].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
}

/* AUT-007: at most 12 actions sent per rolling 60 s panel-wide; the 13th ends held_back. */
static void test_cap_thirteenth_held_back(void)
{
    char ref[16];
    for (unsigned i = 0; i < 13u; i++) {
        snprintf(ref, sizeof(ref), "light.l%u", i);
        rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, ref);
    }
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    EXPECT_RES(13, AUTO_RES_HELD_BACK);
    TEST_ASSERT_EQUAL_UINT8(0, status(13).reason);
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
    /* Still full 59.999 s later; free again at 60 s. */
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 13, T0 + S_(60) - 1));
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 13, T0 + S_(60)));
    TEST_ASSERT_EQUAL_UINT(13, F.n_sends);
}

/* AUT-007: a Lock or Close that didn't happen calls the failed port at most once an hour per
 * automation, unless the reason changes; lights never do. */
static void test_failed_port_hourly(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ALARM, DEV_LOCK, ACT_LOCK, "lock.front");
    rule(AUTO_TRIG_ALARM, DEV_COVER, ACT_CLOSE, "cover.garage");
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.porch");
    (void)r;
    load();
    for (unsigned i = 0; i < F.n_devs; i++) {
        F.devs[i].answer = HC_ERR_BUSY;
    }
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0);
    TEST_ASSERT_EQUAL_UINT(2, F.n_failed);
    TEST_ASSERT_EQUAL_UINT16(2, F.failed_uid);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_BUSY, F.failed_reason);
    TEST_ASSERT_EQUAL_UINT8(ACT_CLOSE, F.failed_action);
    TEST_ASSERT_EQUAL_UINT16(0, F.failed_zone); /* not a zone trigger */
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(600));
    TEST_ASSERT_EQUAL_UINT(2, F.n_failed);
    F.devs[0].answer = HC_OK;
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(1200));
    auto_run_result(&R, F.sends[0].req, false, AUTO_WHY_REFUSED, T0 + S_(1201));
    TEST_ASSERT_EQUAL_UINT(3, F.n_failed); /* the reason changed */
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_REFUSED, F.failed_reason);
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(3600));
    TEST_ASSERT_EQUAL_UINT(4, F.n_failed); /* the cover: an hour since */
    TEST_ASSERT_EQUAL_UINT16(2, F.failed_uid);
}

/* AUT-007: a zone trigger's failed lock carries the zone. */
static void test_failed_port_carries_zone(void)
{
    rule(AUTO_TRIG_ZONE_CLOSED, DEV_LOCK, ACT_LOCK, "lock.front");
    load();
    F.devs[0].answer = HC_ERR_BUSY;
    occ(AUTO_TRIG_ZONE_CLOSED, 0, 0, 1, T0);
    tick(T0 + S_(10));
    TEST_ASSERT_EQUAL_UINT(1, F.n_failed);
    TEST_ASSERT_EQUAL_UINT16(1, F.failed_zone);
}

/* AUT-006: Try it now runs a device rule at once, turned off or outside its trigger; it keeps
 * the device gap; it refuses an arming rule, an unknown uid and a rule that doesn't pass. */
static void test_try_it_now(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_TIME, DEV_SCENE, ACT_RUN, "scene.night");
    r->when.minute = 600;
    r->off = true;
    auto_rule_t *bad = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.x");
    bad->when.zone = 6; /* smoke */
    auto_rule_t *arm = &RULES[NR++];
    memset(arm, 0, sizeof(*arm));
    arm->uid = 3;
    arm->n = 1;
    arm->made_by = 2;
    arm->when.kind = AUTO_TRIG_TIME;
    arm->when.minute = 1350;
    arm->then[0].kind = AUTO_ACT_ARM;
    arm->then[0].partition = 1;
    arm->then[0].arm_level = ALARM_LEVEL_NIGHT;
    arm->then[0].warn_min = 5;
    auto_rule_t *newer = rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.y");
    newer->rec = AUTO_REC_NEWER;
    load();
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_UNKNOWN, auto_run_try(&R, 99, T0));
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_UNKNOWN, auto_run_try(&R, 0, T0));
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_INVALID, auto_run_try(&R, 2, T0));
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_ARM, auto_run_try(&R, 3, T0));
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_INVALID, auto_run_try(&R, 4, T0));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    TEST_ASSERT_EQUAL_UINT(0, F.n_arm);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_RUN, F.sends[0].action);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(2)));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    tick(T0 + S_(10));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    /* Turned off, its trigger never runs it. */
    const auto_span_t sp = {20464, 3, 599, 601};
    auto_run_time(&R, NULL, NULL, &sp, 1, T0 + S_(30));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
}

/* ---- time rules ---------------------------------------------------------------------------- */

/* HCTL-009 / AUT-005: time rules through auto_run_time: a midnight split runs each date's
 * rule once, on its weekday; the same spans again run nothing; a generation change forgets
 * the runs after the floor and keeps the earlier ones. */
static void test_time_rules_midnight_and_gen_floor(void)
{
    auto_rule_t *late = rule(AUTO_TRIG_TIME, DEV_LIGHT, ACT_OFF, "light.a");
    late->when.minute = 1439;
    auto_rule_t *mid = rule(AUTO_TRIG_TIME, DEV_LIGHT, ACT_OFF, "light.b");
    mid->when.minute = 0;
    mid->when.days = AUTO_DAY_BIT(5); /* Friday */
    auto_rule_t *thu = rule(AUTO_TRIG_TIME, DEV_LIGHT, ACT_OFF, "light.c");
    thu->when.minute = 0;
    thu->when.days = AUTO_DAY_BIT(4); /* Thursday: not this midnight */
    auto_rule_t *morning = rule(AUTO_TRIG_TIME, DEV_LIGHT, ACT_OFF, "light.d");
    morning->when.minute = 8 * 60;
    load();
    auto_clock_t c;
    auto_clock_init(&c);
    const auto_span_t m8 = {1000, 4, 479, 481};
    auto_run_time(&R, &c, NULL, &m8, 1, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    const auto_span_t split[2] = {{1000, 4, 1438, 1439}, {1001, 5, 0, 1}};
    auto_run_time(&R, &c, NULL, split, 2, T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.a", F.sends[1].ref);
    TEST_ASSERT_EQUAL_STRING("light.b", F.sends[2].ref);
    auto_run_time(&R, &c, NULL, split, 2, T0 + S_(120));
    auto_run_time(&R, &c, NULL, &m8, 1, T0 + S_(120));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    /* The clock went back to 1000 / 23:00: the 23:59 and 00:00 runs are forgotten, 08:00's
     * stays. */
    c.gen++;
    c.floor = 1000 * 1440 + 23 * 60;
    auto_run_time(&R, &c, NULL, NULL, 0, T0 + S_(180));
    auto_run_time(&R, &c, NULL, split, 2, T0 + S_(240));
    auto_run_time(&R, &c, NULL, &m8, 1, T0 + S_(240));
    TEST_ASSERT_EQUAL_UINT(5, F.n_sends);
    /* The same generation again clears nothing. */
    auto_run_time(&R, &c, NULL, split, 2, T0 + S_(300));
    TEST_ASSERT_EQUAL_UINT(5, F.n_sends);
    /* A changed time forgets its last run; an unchanged one keeps it across a reload. */
    RULES[0].when.minute = 1438;
    load();
    auto_run_time(&R, &c, NULL, split, 2, T0 + S_(360));
    TEST_ASSERT_EQUAL_UINT(6, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.a", F.sends[5].ref);
}

/* AUT-006: the clock's reading gives the status its time; without it, 0. */
static void test_status_time_from_clock(void)
{
    rule(AUTO_TRIG_TIME, DEV_LIGHT, ACT_OFF, "light.a")->when.minute = 600;
    load();
    auto_local_t lt = clock_at(600);
    const auto_span_t sp = {20464, 3, 600, 600};
    auto_run_time(&R, NULL, &lt, &sp, 1, T0);
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + 500);
    TEST_ASSERT_EQUAL_INT64(lt.epoch_s * 1000 + 500, status(1).at_epoch_ms);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).action_index);
}

/* ---- the rules changing -------------------------------------------------------------------- */

/* AUT-006: deleting an automation cancels its held sends and its off timer; turning one off
 * lets its off timer finish but starts nothing new; the second action's status is its own. */
static void test_rules_change(void)
{
    auto_rule_t *a = rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.a");
    a->then[0].off_after_min = 10;
    a->n = 2;
    action(&a->then[1], DSRC_HA, DEV_LIGHT, ACT_ON, "light.held");
    auto_rule_t *b = rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.b");
    b->then[0].off_after_min = 10;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    auto_run_result(&R, F.sends[1].req, true, 0, T0 + 10);
    auto_run_result(&R, F.sends[0].req, false, AUTO_WHY_REFUSED, T0 + 20);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).action_index);
    /* light.held isn't listed: rule 1's second action is held. */
    EXPECT_RES(1, AUTO_RES_REFUSED);
    /* Delete 1, turn 2 off. */
    RULES[0] = RULES[1];
    RULES[0].off = true;
    NR = 1;
    load();
    EXPECT_RES(1, AUTO_RES_NONE);
    EXPECT_RES(2, AUTO_RES_DONE); /* kept */
    dev(DSRC_HA, "light.held", DEV_LIGHT);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(100));
    run_until(T0 + S_(1), T0 + S_(700));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.b", F.sends[2].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(600), F.sends[2].at);
    /* nothing left but the Off's own result wait */
    TEST_ASSERT_EQUAL_UINT64(F.sends[2].at + AUTO_RESULT_WAIT_MS,
                             auto_run_next_deadline(&R, T0 + S_(700)));
}

/* AUT-006: a second action's outcome is reported with its index; actions run in order. */
static void test_two_actions_in_order(void)
{
    auto_rule_t *a = rule(AUTO_TRIG_ARMED, DEV_LOCK, ACT_LOCK, "lock.front");
    a->n = 2;
    action(&a->then[1], DSRC_HA, DEV_SCENE, ACT_RUN, "scene.away");
    dev(DSRC_HA, "scene.away", DEV_SCENE)->answer = HC_ERR_BUSY;
    load();
    occ(AUTO_TRIG_ARMED, 1, ALARM_LEVEL_AWAY, 0, T0);
    TEST_ASSERT_EQUAL_UINT(2, F.n_calls);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[0].ref);
    EXPECT_RES(1, AUTO_RES_BUSY);
    TEST_ASSERT_EQUAL_UINT8(1, status(1).action_index);
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + 50);
    EXPECT_RES(1, AUTO_RES_DONE);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).action_index);
}

/* AUT-007: only rules that loaded, are on and pass the check run (newer, broken, off). */
static void test_only_runnable_rules_run(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.a")->rec = AUTO_REC_NEWER;
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.b")->rec = AUTO_REC_BROKEN;
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.c")->off = true;
    rule(AUTO_TRIG_DISARMED, DEV_SCRIPT, ACT_RUN, "script.d"); /* fails the check */
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_UNLOCK, "lock.d0"); /* no maker: fails the check */
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.e");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
    TEST_ASSERT_EQUAL_STRING("light.e", F.sends[0].ref);
}

/* AUT-007 by construction, the runner's part: every trigger kind x action kind. Arming and
 * disarming pass the check after any trigger (D23), and never run in phase A (the arm port is
 * never called); a device rule's run reaches only the device port. That a device action makes no engine event is
 * proved end to end, with the engine-reaching port wired to a real engine (test_auto_e2e.c). */
static void test_no_action_makes_an_occurrence(void)
{
    for (uint8_t trig = 1; trig < AUTO_TRIG_COUNT; trig++) {
        for (uint8_t act = AUTO_ACT_DEVICE; act <= AUTO_ACT_DISARM; act++) {
            if (act == 3u) {
                continue; /* reserved for an output */
            }
            start();
            auto_rule_t *r = rule(trig, DEV_LIGHT, ACT_ON, "light.a");
            r->when.minute = 600;
            if (act == AUTO_ACT_ARM) {
                memset(&r->then[0], 0, sizeof(r->then[0]));
                r->made_by = 2;
                r->then[0].kind = AUTO_ACT_ARM;
                r->then[0].value = -1;
                r->then[0].partition = 1;
                r->then[0].arm_level = ALARM_LEVEL_NIGHT;
                r->then[0].warn_min = 5;
                auto_env_t c = ENV;
                c.arm_supported = true;
                TEST_ASSERT_EQUAL_INT_MESSAGE(AUTO_OK, auto_rule_check(r, &c), "arm with arming");
                TEST_ASSERT_NOT_EQUAL(AUTO_OK, auto_rule_check(r, &ENV));
            }
            if (act == AUTO_ACT_DISARM) {
                memset(&r->then[0], 0, sizeof(r->then[0]));
                r->made_by = 2;
                r->then[0].kind = AUTO_ACT_DISARM;
                r->then[0].value = -1;
                r->then[0].partition = 1;
                r->then[0].confirmed = 1; /* a door or alarm trigger asks for it (D24) */
                auto_env_t c = ENV;
                c.arm_supported = true;
                /* AUT-016: a disarm after an armed trigger would undo the arming: refused;
                 * D22: after an alarm or a fire it would silence it: never allowed */
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    trig == AUTO_TRIG_ARMED                               ? AUTO_ERR_UNDOES_ARMING
                    : (trig == AUTO_TRIG_ALARM || trig == AUTO_TRIG_FIRE) ? AUTO_ERR_NOT_ALLOWED
                                                                          : AUTO_OK,
                    auto_rule_check(r, &c), "disarm, phase C");
                TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNSUPPORTED, auto_rule_check(r, &ENV));
            }
            load();
            occ(trig, 1, ALARM_LEVEL_AWAY, 1, T0);
            const auto_span_t sp = {20464, 3, 600, 600};
            auto_run_time(&R, NULL, NULL, &sp, 1, T0);
            tick(T0 + S_(10));
            TEST_ASSERT_EQUAL_UINT_MESSAGE(act == AUTO_ACT_DEVICE ? 1u : 0u, F.n_sends, "sends");
            TEST_ASSERT_EQUAL_UINT(0, F.n_arm);
            TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
        }
    }
}

/* ---- review follow-ups: the Off's own send, edits, deletes, steps back -------------------- */

/* AUT-006 / AUT-007: the Off waits for the device gap and goes when it ends. */
static void test_off_waits_for_gap(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(55));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    tick(T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(65), auto_run_next_deadline(&R, T0 + S_(60)));
    run_until(T0 + S_(61), T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(65), F.sends[2].at);
}

/* AUT-006: an Off that fell due is dropped only when a newer On to that device was sent after
 * it; that On's own timer then owns the light. */
static void test_newer_on_sent_owns_the_off(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(55));
    tick(T0 + S_(60)); /* the Off is due; it waits for the gap */
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(62));
    run_until(T0 + S_(63), T0 + S_(124));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(65), F.sends[2].at);
    tick(T0 + S_(125));
    TEST_ASSERT_EQUAL_UINT(4, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[3].action);
}

/* AUT-006 / AUT-007: On sent; its timer ends; the Off waits for the gap; a new
 * occurrence arrives and its On is held back by the cap -> the Off is still sent, once the
 * cap's window allows. An Off is never lost. */
static void test_off_survives_held_back_on(void)
{
    char ref[16];
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.porch");
    for (unsigned i = 0; i < 10u; i++) {
        snprintf(ref, sizeof(ref), "light.x%u", i);
        rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, ref);
    }
    rule(AUTO_TRIG_FIRE, DEV_LIGHT, ACT_ON, "light.z");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(55)); /* the gap on the porch light; the cap fills */
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    tick(T0 + S_(60));                          /* the Off falls due and waits for the gap */
    occ(AUTO_TRIG_FIRE, 1, 0, 6, T0 + S_(61));  /* the cap stays full until 115 s */
    TEST_ASSERT_EQUAL_UINT(13, F.n_sends);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(62));
    tick(T0 + S_(65));
    EXPECT_RES(1, AUTO_RES_HELD_BACK);
    TEST_ASSERT_EQUAL_UINT(13, F.n_sends);
    run_until(T0 + S_(66), T0 + S_(200));
    TEST_ASSERT_EQUAL_UINT(14, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.porch", F.sends[13].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[13].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(115), F.sends[13].at);
    /* nothing left but the sends' own result waits */
    TEST_ASSERT_EQUAL_UINT64(T0 + AUTO_RESULT_WAIT_MS, auto_run_next_deadline(&R, T0 + S_(200)));
}

/* HCTL-009: an edit that moves the trigger (zone, kind or partitions) clears the
 * close hold and the cooldown; an edit that keeps it keeps them. */
static void test_edit_trigger_clears_hold_and_cooldown(void)
{
    rule(AUTO_TRIG_ZONE_CLOSED, DEV_LOCK, ACT_LOCK, "lock.front");
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.hall");
    load();
    occ(AUTO_TRIG_ZONE_CLOSED, 0, 0, 1, T0);
    RULES[0].when.zone = 2;
    load();
    run_until(T0 + S_(1), T0 + S_(20));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(30));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    RULES[1].when.zone = 2;
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 2, T0 + S_(31));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends); /* the device gap holds it ... */
    tick(T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends); /* ... but the old cooldown didn't stop it */
    /* An edit that keeps the trigger keeps its cooldown. */
    strcpy(RULES[1].then[0].name, "Hall");
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 2, T0 + S_(55));
    run_until(T0 + S_(55), T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(2, F.n_calls);
}

/* AUT-006: an edit that changes an action drops its waiting send, so the old device
 * never gets the old command; an unchanged action keeps its held send. */
static void test_edit_action_drops_waiting_send(void)
{
    auto_rule_t *a = rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.a");
    a->n = 2;
    action(&a->then[1], DSRC_HA, DEV_LIGHT, ACT_ON, "light.keep");
    dev(DSRC_HA, "light.keep", DEV_LIGHT)->listed = 0;
    dev(DSRC_HA, "light.b", DEV_LIGHT);
    load();
    F.devs[0].listed = 0;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    strcpy(RULES[0].then[0].ref, "light.b");
    load();
    F.devs[0].listed = 1;
    dev(DSRC_HA, "light.keep", DEV_LIGHT);
    run_until(T0 + S_(1), T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.keep", F.sends[0].ref);
    /* Another command to the same device is another action too. */
    F.devs[0].listed = 0;
    RULES[0].n = 1;
    strcpy(RULES[0].then[0].ref, "light.a");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(100));
    RULES[0].then[0].dev_action = ACT_OFF;
    load();
    F.devs[0].listed = 1;
    run_until(T0 + S_(101), T0 + S_(140));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
}

/* AUT-006: an off timer running for the device an edit replaced keeps its own copy
 * and still turns that device off; the new device's On doesn't take it over. */
static void test_edit_action_keeps_old_off_timer(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.c")->then[0].off_after_min = 1;
    dev(DSRC_HA, "light.d", DEV_LIGHT);
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    strcpy(RULES[0].then[0].ref, "light.d");
    RULES[0].then[0].off_after_min = 0;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(30)); /* light.d, left on: no timer of its own */
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.d", F.sends[1].ref);
    run_until(T0 + S_(31), T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.c", F.sends[2].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(60), F.sends[2].at);
}

/* AUT-006: an edited action whose On starts a timer while the old device's still runs: the old
 * device's Off goes at once, then the new On, and each device is turned off. */
static void test_edit_action_timer_slot_conflict(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.c")->then[0].off_after_min = 5;
    dev(DSRC_HA, "light.d", DEV_LIGHT);
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    strcpy(RULES[0].then[0].ref, "light.d");
    RULES[0].then[0].off_after_min = 1;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(30));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.c", F.sends[1].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[1].action);
    run_until(T0 + S_(31), T0 + S_(200));
    TEST_ASSERT_EQUAL_UINT(4, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.d", F.sends[2].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(32), F.sends[2].at);
    TEST_ASSERT_EQUAL_STRING("light.d", F.sends[3].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[3].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(92), F.sends[3].at);
}

/* AUT-006: a result that arrives after its automation was deleted changes nothing, not even a
 * new automation given the same uid. */
static void test_result_after_delete(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.front");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    const uint32_t req = F.sends[0].req;
    NR = 0;
    load();
    auto_run_result(&R, req, false, AUTO_WHY_REFUSED, T0 + S_(1));
    EXPECT_RES(1, AUTO_RES_NONE);
    NR = 1;
    load();
    auto_run_result(&R, req, false, AUTO_WHY_REFUSED, T0 + S_(2));
    run_until(T0 + S_(2), T0 + S_(90));
    EXPECT_RES(1, AUTO_RES_NONE);
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
}

/* AUT-006: turning an automation off drops its held sends. */
static void test_turned_off_drops_held(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.a");
    load();
    F.devs[0].listed = 0;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    RULES[0].off = true;
    load();
    F.devs[0].listed = 1;
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0 + S_(1)));
    run_until(T0 + S_(1), T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
}

/* AUT-007: a now older than the times recorded never lifts the cap or the failed
 * port's hourly limit. */
static void test_now_steps_back(void)
{
    char ref[16];
    for (unsigned i = 0; i < 12u; i++) {
        snprintf(ref, sizeof(ref), "light.l%u", i);
        rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, ref);
    }
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.m");
    rule(AUTO_TRIG_FIRE, DEV_LOCK, ACT_LOCK, "lock.front");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(10));
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0);
    EXPECT_RES(13, AUTO_RES_HELD_BACK);
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    F.devs[13].answer = HC_ERR_BUSY;
    occ(AUTO_TRIG_FIRE, 1, 0, 6, T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(1, F.n_failed);
    occ(AUTO_TRIG_FIRE, 1, 0, 6, T0 + S_(50));
    TEST_ASSERT_EQUAL_UINT(1, F.n_failed);
}

/* AUT-006: an edit removes "Turn it off after" (the same light): the next On is sent without a
 * timer and stops the running one - the person's newest rule says "stay on"; no Off ever goes. */
static void test_edit_removes_off_after_on_stops_timer(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    RULES[0].then[0].off_after_min = 0;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(30));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, F.sends[1].action);
    run_until(T0 + S_(31), T0 + S_(200));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2, F.n_sends, "an Off went after the timer was removed");
}

/* AUT-006: after an edit removes "Turn it off after", a new run whose On isn't sent (busy, or a
 * repeat inside the zone cooldown) doesn't restart the old timer: its Off goes when it was due. */
static void test_edit_removes_off_after_no_restart(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.hall")->then[0].off_after_min = 1;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    RULES[0].then[0].off_after_min = 0;
    RULES[1].then[0].off_after_min = 0;
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(20)); /* inside the cooldown */
    F.devs[0].answer = HC_ERR_BUSY;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(30)); /* not sent: busy */
    EXPECT_RES(1, AUTO_RES_BUSY);
    F.devs[0].answer = HC_OK;
    run_until(T0 + S_(31), T0 + S_(200));
    TEST_ASSERT_EQUAL_UINT(4, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[3].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(60), F.sends[2].at);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(60), F.sends[3].at);
}

/* AUT-006 / AUT-007: an edited action's On waits behind the old device's Off at most 30 s from
 * its first wait (the Off waits for the cap, then is held offline): it ends unreachable and is
 * never sent; the old device's Off still goes on trying. */
static void test_on_behind_old_off_bounded(void)
{
    char ref[16];
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.c")->then[0].off_after_min = 5;
    for (unsigned i = 0; i < 11u; i++) {
        snprintf(ref, sizeof(ref), "light.x%u", i);
        rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, ref);
    }
    dev(DSRC_HA, "light.d", DEV_LIGHT);
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0 + S_(5)); /* the cap is full until T0 + 60 s */
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    strcpy(RULES[0].then[0].ref, "light.d");
    RULES[0].then[0].off_after_min = 1;
    load();
    F.devs[0].answer = HC_ERR_OFFLINE; /* light.c */
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(30));
    TEST_ASSERT_EQUAL_UINT(12, F.n_sends);
    run_until(T0 + S_(31), T0 + S_(59));
    EXPECT_RES(1, AUTO_RES_NONE);
    tick(T0 + S_(60));
    EXPECT_RES(1, AUTO_RES_UNREACHABLE);
    run_until(T0 + S_(61), T0 + S_(200));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(12, F.n_sends, "the On waited past its 30 s");
    /* nothing left but the sends' own result waits */
    TEST_ASSERT_EQUAL_UINT64(T0 + AUTO_RESULT_WAIT_MS, auto_run_next_deadline(&R, T0 + S_(200)));
}

/* AUT-006: an edit that changes only "Turn it off after" keeps the waiting On; it is sent once
 * and its timer has the new length. */
static void test_edit_off_after_length_kept_on(void)
{
    rule(AUTO_TRIG_DISARMED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    rule(AUTO_TRIG_ALARM, DEV_LIGHT, ACT_ON, "light.porch");
    load();
    occ(AUTO_TRIG_ALARM, 1, 0, 9, T0);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(2)); /* waits for the gap until T0 + 10 s */
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    RULES[0].then[0].off_after_min = 2;
    load();
    run_until(T0 + S_(3), T0 + S_(300));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, F.sends[1].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(10), F.sends[1].at);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(130), F.sends[2].at);
}

/* ---- the installer option and the maker (AUT-015, D23, D25) ------------------------------ */

/* A rule that unlocks lock.front when partition 1 is disarmed, saved by user 1. */
static auto_rule_t *unlock_rule(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_UNLOCK, "lock.front");
    r->made_by = 1;
    return r;
}

/* AUT-015: an unlock is sent with the option on (device_act told so); with it off the rule is
 * kept and still matches, but the unlock ends LESS_SECURE_OFF - never sent, no failed event -
 * while its Lock-only neighbour runs on; a reload in between changes nothing; turned back on
 * (a new env), it runs again. */
static void test_less_secure_option_at_send(void)
{
    unlock_rule();
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.back");
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[0].ref);
    TEST_ASSERT_EQUAL_UINT8(ACT_UNLOCK, F.sends[0].action);
    TEST_ASSERT_TRUE(F.sends[0].less_secure);
    TEST_ASSERT_EQUAL_UINT(1, F.n_maker);
    TEST_ASSERT_EQUAL_UINT8(1, F.maker_asked);
    TEST_ASSERT_TRUE(F.sends[1].less_secure);

    /* the option off: the same rules, a new env */
    const auto_rule_t kept = RULES[0];
    ENV.less_secure = false;
    load();
    load(); /* a reload */
    TEST_ASSERT_EQUAL_MEMORY(&kept, &RULES[0], sizeof kept);
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(20));
    EXPECT_RES(1, AUTO_RES_LESS_SECURE_OFF);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).reason);
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends); /* only the Lock */
    TEST_ASSERT_EQUAL_STRING("lock.back", F.sends[2].ref);
    TEST_ASSERT_FALSE(F.sends[2].less_secure);
    TEST_ASSERT_EQUAL_UINT(1, F.n_maker); /* not asked: the option said no first */
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
    run_until(T0 + S_(20), T0 + S_(80));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);

    /* turned back on: it runs again */
    ENV.less_secure = true;
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(90));
    TEST_ASSERT_EQUAL_UINT(5, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[3].ref);
    TEST_ASSERT_TRUE(F.sends[3].less_secure);
}

/* AUT-015, D23: before a less secure send the maker must still be a system master or installer
 * (maker_ok): if not, MAKER_GONE - never sent, no failed event; restored, it runs again. Without
 * the port, no. A Lock never asks. */
static void test_maker_gone(void)
{
    unlock_rule();
    auto_rule_t *open = rule(AUTO_TRIG_DISARMED, DEV_COVER, ACT_OPEN, "cover.garage");
    open->made_by = 3;
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.back");
    load();
    F.maker_gone[1] = true;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_MAKER_GONE);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).reason);
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends); /* the open (user 3) and the Lock */
    TEST_ASSERT_EQUAL_STRING("cover.garage", F.sends[0].ref);
    TEST_ASSERT_EQUAL_STRING("lock.back", F.sends[1].ref);
    TEST_ASSERT_EQUAL_UINT(2, F.n_maker);
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
    F.maker_gone[1] = false;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(20));
    TEST_ASSERT_EQUAL_UINT(5, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[2].ref);

    /* no maker_ok port: never sent */
    start();
    const auto_ports_t p = {&F,          port_act, port_settled, port_kind, port_arm, NULL,
                            port_failed, NULL,     NULL,         NULL,      NULL,     NULL, NULL, NULL, NULL, NULL, NULL};
    F.gated = false;
    auto_run_init(&R, &p, T0);
    unlock_rule();
    load();
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_MAKER_GONE);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
}

/* AUT-015: Try it now is held to the option and the maker too; a disarming automation is never
 * tried (phase C's own path). */
static void test_try_it_now_less_secure(void)
{
    unlock_rule();
    auto_rule_t *d = &RULES[NR++];
    memset(d, 0, sizeof(*d));
    d->uid = 2;
    d->n = 1;
    d->made_by = 2;
    d->when.kind = AUTO_TRIG_TIME;
    d->when.minute = 600;
    d->then[0].kind = AUTO_ACT_DISARM;
    d->then[0].value = -1;
    d->then[0].partition = 1;
    ENV.less_secure = false;
    load();
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0));
    EXPECT_RES(1, AUTO_RES_LESS_SECURE_OFF);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_ARM, auto_run_try(&R, 2, T0));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    ENV.less_secure = true;
    load();
    F.maker_gone[1] = true;
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(1)));
    EXPECT_RES(1, AUTO_RES_MAKER_GONE);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    F.maker_gone[1] = false;
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(2)));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_UINT(0, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
}

/* AUT-015 layer 4 (DESIGN.md section 3): the installer option is read again with the maker
 * before each less secure send, so an Off the env hasn't caught up with yet (the task reloads
 * after the wake's occurrences) still stops the very next unlock - LESS_SECURE_OFF, nothing
 * sent; a Lock never asks; back on, it runs. */
static void test_option_read_live_at_send(void)
{
    unlock_rule();
    rule(AUTO_TRIG_DISARMED, DEV_LOCK, ACT_LOCK, "lock.back");
    load(); /* the env says on */
    F.live_off = true;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0);
    EXPECT_RES(1, AUTO_RES_LESS_SECURE_OFF);
    TEST_ASSERT_EQUAL_UINT(1, F.n_maker);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends); /* the Lock alone */
    TEST_ASSERT_EQUAL_STRING("lock.back", F.sends[0].ref);
    TEST_ASSERT_EQUAL_UINT(0, F.n_failed);
    F.live_off = false;
    occ(AUTO_TRIG_DISARMED, 1, 0, 0, T0 + S_(20));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[1].ref);
    TEST_ASSERT_TRUE(F.sends[1].less_secure);
}

/* AUT-015 layer 4 through Try it now (AUT-006): the same live read before the send stops a
 * tried unlock the env still allows - LESS_SECURE_OFF, tried 1, nothing sent; back on, it runs. */
static void test_option_read_live_at_try(void)
{
    unlock_rule();
    load(); /* the env says on */
    F.live_off = true;
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0));
    EXPECT_RES(1, AUTO_RES_LESS_SECURE_OFF);
    TEST_ASSERT_EQUAL_UINT8(1, status(1).tried);
    TEST_ASSERT_EQUAL_UINT(1, F.n_maker);
    TEST_ASSERT_EQUAL_UINT(0, F.n_sends);
    F.live_off = false;
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(1)));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_TRUE(F.sends[0].less_secure);
}

/* AUT-006: a triggered run that restarts a tried On's "Turn it off after" owns the Off from then
 * on: the Off's outcome says tried 0, not the try's 1. */
static void test_restarted_off_is_the_runs(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch");
    r->then[0].off_after_min = 1;
    load();
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0));
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + S_(1));
    TEST_ASSERT_EQUAL_UINT8(1, status(1).tried);
    /* the door opens 10 s later: the On again, and the timer restarted by that run */
    occ(AUTO_TRIG_ZONE_OPENED, 1, 0, 1, T0 + S_(10));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    auto_run_result(&R, F.sends[1].req, false, AUTO_WHY_REFUSED, T0 + S_(11));
    EXPECT_RES(1, AUTO_RES_REFUSED);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).tried);
    run_until(T0 + S_(12), T0 + S_(75));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[2].action);
    auto_run_result(&R, F.sends[2].req, true, 0, T0 + S_(76));
    EXPECT_RES(1, AUTO_RES_DONE);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).tried);
}

/* AUT-006: the status says whether its outcome came from Try it now (tried 1) or from the
 * trigger (tried 0); the Off of a tried "Turn it off after" is part of the try. */
static void test_status_says_tried(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch");
    r->then[0].off_after_min = 1;
    load();
    TEST_ASSERT_EQUAL_UINT8(0, status(1).tried);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    auto_run_result(&R, F.sends[0].req, true, 0, T0 + S_(1));
    EXPECT_RES(1, AUTO_RES_DONE);
    TEST_ASSERT_EQUAL_UINT8(1, status(1).tried);
    run_until(T0 + S_(2), T0 + S_(61));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[1].action);
    auto_run_result(&R, F.sends[1].req, false, AUTO_WHY_REFUSED, T0 + S_(62));
    EXPECT_RES(1, AUTO_RES_REFUSED);
    TEST_ASSERT_EQUAL_UINT8(1, status(1).tried);
    /* the door opens: a triggered run */
    occ(AUTO_TRIG_ZONE_OPENED, 1, 0, 1, T0 + S_(80));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    auto_run_result(&R, F.sends[2].req, true, 0, T0 + S_(81));
    EXPECT_RES(1, AUTO_RES_DONE);
    TEST_ASSERT_EQUAL_UINT8(0, status(1).tried);
    /* tried again, and an outcome the runner decides itself (the device gap is no outcome;
     * missing is) */
    F.devs[0].listed = 0u;
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, T0 + S_(200)));
    run_until(T0 + S_(200), T0 + S_(240));
    EXPECT_RES(1, AUTO_RES_MISSING);
    TEST_ASSERT_EQUAL_UINT8(1, status(1).tried);
}

/* ---- revocation (PL04, AUT-010: turned off, deleted or changed, it starts nothing more) ----- */

/* What the service does at a successful edit: the uid revoked in the step that publishes it. */
static void revoke(uint16_t uid)
{
    auto_revoke_add(&F.revoked, uid);
}

/* What the service's reload does: the set emptied as the new rules are copied, then the rules. */
static void reload_rules(void)
{
    auto_revoke_clear(&F.revoked);
    load();
}

/* A door that unlocks lock.front (a less secure action, confirmed), saved by user 1. */
static auto_rule_t *door_unlock_rule(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED, DEV_LOCK, ACT_UNLOCK, "lock.front");
    r->made_by = 1;
    return r;
}

/* PL04 / AUT-010: the set's own rules: a uid, uid 0 or more than AUTO_MAX uids is every one;
 * cleared, none. */
static void test_revoke_set(void)
{
    auto_revoke_t v;
    auto_revoke_clear(&v);
    TEST_ASSERT_FALSE(auto_revoke_has(&v, 1));
    auto_revoke_add(&v, 7);
    auto_revoke_add(&v, 7);
    TEST_ASSERT_TRUE(auto_revoke_has(&v, 7));
    TEST_ASSERT_FALSE(auto_revoke_has(&v, 8));
    TEST_ASSERT_EQUAL_UINT8(1, v.n);
    auto_revoke_add(&v, 0);
    TEST_ASSERT_TRUE(auto_revoke_has(&v, 8));
    auto_revoke_clear(&v);
    for (uint16_t u = 1; u <= AUTO_MAX; u++) {
        auto_revoke_add(&v, u);
    }
    TEST_ASSERT_FALSE(auto_revoke_has(&v, 500));
    auto_revoke_add(&v, 500); /* no room: every one */
    TEST_ASSERT_TRUE(auto_revoke_has(&v, 501));
    auto_revoke_clear(&v);
    TEST_ASSERT_FALSE(auto_revoke_has(&v, 500));
}

/* PL04 / AUT-010: a door Unlock turned off, its edit answered, then the door opens before the
 * task wakes. In the task's order (the rules first, then the occurrences) the occurrence finds
 * it off; with the old order (the occurrence before the rules), the gate keeps the unlock until
 * the rules come, which drop it. Never sent, "Last run" unchanged. */
static void test_off_before_wake_sends_nothing(void)
{
    door_unlock_rule();
    load();
    /* the task's order */
    RULES[0].off = 1;
    revoke(1);
    reload_rules();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    run_until(T0, T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    /* the occurrence first: held at the gate, then dropped by the rules */
    RULES[0].off = 0;
    revoke(1);
    reload_rules();
    RULES[0].off = 1;
    revoke(1);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(100), auto_run_next_deadline(&R, T0 + S_(100)));
    tick(T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    reload_rules();
    run_until(T0 + S_(100), T0 + S_(160));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    EXPECT_RES(1, AUTO_RES_NONE);
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

/* PL04 / AUT-010: deleted ("It stops at once"), its edit answered, the door opens before the
 * wake: nothing is sent, in either order. */
static void test_delete_before_wake_sends_nothing(void)
{
    door_unlock_rule();
    load();
    revoke(1);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    run_until(T0, T0 + S_(5));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    NR = 0;
    reload_rules();
    run_until(T0 + S_(5), T0 + S_(60));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(61));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, auto_run_next_deadline(&R, T0 + S_(61)));
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

/* PL04: an edit that replaces the action (another device) while a run waits at the gate: the
 * old action is never sent, the next run sends only the new one. An edit that leaves the action
 * as it was (a new name, a new time) only delays it until the rules come: sent once. */
static void test_edit_replacing_action_sends_only_new(void)
{
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch");
    dev(DSRC_HA, "light.hall", DEV_LIGHT);
    load();
    revoke(1);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    strcpy(RULES[0].then[0].ref, "light.hall");
    reload_rules();
    run_until(T0, T0 + S_(31));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(40));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.hall", F.sends[0].ref);
    /* renamed only: waits for the rules, then goes once */
    revoke(1);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(80));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
    strcpy(RULES[0].then[0].name, "Hall light");
    RULES[0].changed_at = 1800000000u;
    reload_rules();
    run_until(T0 + S_(80), T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.hall", F.sends[1].ref);
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

/* PL04: an edit that moves the trigger (the door Unlock from zone 1 to zone 3) while zone 1's run
 * waits at the gate: that run belongs to the old version and never goes, though the action is
 * the same; zone 3 then runs it. */
static void test_edit_moving_trigger_drops_gated_run(void)
{
    door_unlock_rule();
    load();
    revoke(1);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    RULES[0].when.zone = 3;
    reload_rules();
    run_until(T0, T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(0, F.n_calls);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 3, T0 + S_(61));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("lock.front", F.sends[0].ref);
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

/* PL04: a send held for an offline Home Assistant device stops trying once its automation is
 * turned off - not one more try after the edit answered. */
static void test_held_retry_stops_after_revoke(void)
{
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch")->made_by = 1;
    F.devs[0].answer = HC_ERR_OFFLINE;
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    run_until(T0, T0 + S_(4));
    const unsigned tries = F.n_calls;
    TEST_ASSERT_GREATER_THAN_UINT(1, tries);
    F.devs[0].answer = HC_OK; /* it would go now */
    RULES[0].off = 1;
    revoke(1);
    run_until(T0 + S_(5), T0 + S_(8));
    TEST_ASSERT_EQUAL_UINT(tries, F.n_calls);
    reload_rules();
    run_until(T0 + S_(8), T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(tries, F.n_calls);
    TEST_ASSERT_EQUAL_UINT(0, F.n_sends);
    EXPECT_RES(1, AUTO_RES_NONE);
}

/* PL04: an edit that lands while the task is mid-run (here: answered during the first action's
 * send) stops the second action of the same run, a less secure Unlock. Deleted, it never goes. */
static void test_two_actions_stop_mid_run(void)
{
    auto_rule_t *r = rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.hall");
    r->made_by = 1;
    r->n = 2;
    action(&r->then[1], DSRC_HA, DEV_LOCK, ACT_UNLOCK, "lock.front");
    dev(DSRC_HA, "lock.front", DEV_LOCK);
    load();
    F.revoke_on_send = 1; /* the edit answers while light.hall's send is in progress */
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    TEST_ASSERT_EQUAL_STRING("light.hall", F.sends[0].ref);
    run_until(T0, T0 + S_(5));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls);
    NR = 0;
    reload_rules();
    run_until(T0 + S_(5), T0 + S_(60));
    TEST_ASSERT_EQUAL_UINT(1, F.n_calls); /* the sent On stays sent; the Unlock never goes */
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

/* PL04 / AUT-006: a "Turn it off after" timer already running when its automation is turned
 * off still turns the device off - after the rules come, if it fell due while the edit waited
 * for them; and at its time when they came first. */
static void test_off_timer_still_fires_after_turn_off(void)
{
    rule(AUTO_TRIG_ZONE_OPENED, DEV_LIGHT, ACT_ON, "light.porch")->then[0].off_after_min = 1;
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    /* turned off: the edit answered, the Off falls due before the rules come */
    RULES[0].off = 1;
    revoke(1);
    run_until(T0 + S_(1), T0 + S_(61));
    TEST_ASSERT_EQUAL_UINT(1, F.n_sends);
    reload_rules();
    tick(T0 + S_(61));
    TEST_ASSERT_EQUAL_UINT(2, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[1].action);
    TEST_ASSERT_EQUAL_STRING("light.porch", F.sends[1].ref);
    /* the rules first: the Off at its time */
    RULES[0].off = 0;
    revoke(1);
    reload_rules();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0, 1, T0 + S_(100));
    TEST_ASSERT_EQUAL_UINT(3, F.n_sends);
    RULES[0].off = 1;
    revoke(1);
    reload_rules();
    run_until(T0 + S_(101), T0 + S_(160));
    TEST_ASSERT_EQUAL_UINT(4, F.n_sends);
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, F.sends[3].action);
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zone_opened_sends_once);
    RUN_TEST(test_partition_and_level_masks);
    RUN_TEST(test_zone_retyped_stops);
    RUN_TEST(test_window_wrap_and_invalid_clock);
    RUN_TEST(test_cooldown_and_off_timer_restart);
    RUN_TEST(test_close_hold_cancelled_by_reopen);
    RUN_TEST(test_ha_hold_then_sent_when_listed);
    RUN_TEST(test_ha_hold_missing_or_unreachable);
    RUN_TEST(test_other_source_fails_at_once);
    RUN_TEST(test_busy_arg_not_allowed);
    RUN_TEST(test_request_id_round_trip_and_no_answer);
    RUN_TEST(test_live_kind_change_not_sent);
    RUN_TEST(test_newer_occurrence_replaces_held);
    RUN_TEST(test_held_arming_send_dropped);
    RUN_TEST(test_device_gap_two_automations);
    RUN_TEST(test_cap_thirteenth_held_back);
    RUN_TEST(test_failed_port_hourly);
    RUN_TEST(test_failed_port_carries_zone);
    RUN_TEST(test_try_it_now);
    RUN_TEST(test_time_rules_midnight_and_gen_floor);
    RUN_TEST(test_status_time_from_clock);
    RUN_TEST(test_rules_change);
    RUN_TEST(test_two_actions_in_order);
    RUN_TEST(test_only_runnable_rules_run);
    RUN_TEST(test_no_action_makes_an_occurrence);
    RUN_TEST(test_zwave_source_fails_at_once);
    RUN_TEST(test_not_allowed_lock_reported);
    RUN_TEST(test_no_kind_port_cant_run);
    RUN_TEST(test_off_waits_for_gap);
    RUN_TEST(test_newer_on_sent_owns_the_off);
    RUN_TEST(test_off_survives_held_back_on);
    RUN_TEST(test_edit_trigger_clears_hold_and_cooldown);
    RUN_TEST(test_edit_action_drops_waiting_send);
    RUN_TEST(test_edit_action_keeps_old_off_timer);
    RUN_TEST(test_edit_action_timer_slot_conflict);
    RUN_TEST(test_result_after_delete);
    RUN_TEST(test_turned_off_drops_held);
    RUN_TEST(test_now_steps_back);
    RUN_TEST(test_edit_removes_off_after_on_stops_timer);
    RUN_TEST(test_edit_removes_off_after_no_restart);
    RUN_TEST(test_on_behind_old_off_bounded);
    RUN_TEST(test_edit_off_after_length_kept_on);
    RUN_TEST(test_less_secure_option_at_send);
    RUN_TEST(test_maker_gone);
    RUN_TEST(test_option_read_live_at_send);
    RUN_TEST(test_option_read_live_at_try);
    RUN_TEST(test_restarted_off_is_the_runs);
    RUN_TEST(test_status_says_tried);
    RUN_TEST(test_try_it_now_less_secure);
    RUN_TEST(test_revoke_set);
    RUN_TEST(test_off_before_wake_sends_nothing);
    RUN_TEST(test_delete_before_wake_sends_nothing);
    RUN_TEST(test_edit_replacing_action_sends_only_new);
    RUN_TEST(test_edit_moving_trigger_drops_gated_run);
    RUN_TEST(test_held_retry_stops_after_revoke);
    RUN_TEST(test_two_actions_stop_mid_run);
    RUN_TEST(test_off_timer_still_fires_after_turn_off);
    return UNITY_END();
}
