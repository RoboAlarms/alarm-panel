/* Automations without devices (BL-224, docs/features/41 DESIGN.md section 15): the stored fields
 * and their frozen fixture (AUT-008), the validator's rules for the new trigger and actions
 * (AUT-001, AUT-002), the trouble trigger in the observer (AUT-003) and the runner's outputs,
 * door chime, notifications, reminders and "still armed" through fake ports (AUT-006, AUT-007). */
#include <string.h>

#include "unity.h"

#include "auto_fixture_v3.h"
#include "automation/auto_run.h"
#include "automation/auto_watch.h"
#include "automation/automation.h"
#include "home_control/home_control.h"

#define T0 UINT64_C(1000000)
#define S_(s) ((uint64_t)(s) * UINT64_C(1000))

static auto_zone_t Z[ALARM_MAX_ZONES];

static auto_env_t env_of(void)
{
    memset(Z, 0, sizeof Z);
    Z[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    auto_env_t e = {0x01u, true, Z, true, false, 0x02u}; /* output 2 is for automations */
    return e;
}

static auto_rule_t at_time(uint16_t minute)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.n = 1;
    r.uid = 1;
    r.made_by = 2;
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = minute;
    for (unsigned i = 0; i < AUTO_ACTIONS; i++) {
        r.then[i].value = -1;
    }
    return r;
}

static void remind(auto_action_t *a, uint8_t partition, bool notify)
{
    a->kind = AUTO_ACT_REMIND;
    a->partition = partition;
    a->option = notify ? 1u : 0u;
}

static void output(auto_action_t *a, uint8_t out, uint8_t action, uint16_t off_after)
{
    a->kind = AUTO_ACT_OUTPUT;
    a->output = out;
    a->dev_action = action;
    a->off_after_min = off_after;
}

/* ---- the fixture (AUT-008) ---- */

static void decode_ok(const uint8_t *b, size_t n, auto_rule_t *r)
{
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(b, n, r));
    uint8_t out[AUTO_RECORD_MAX];
    const size_t len = auto_encode(r, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(n, len); /* written back byte for byte */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, out, n);
    const auto_env_t env = env_of();
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(r, &env));
}

static void test_fixture_v3(void)
{
    auto_rule_t r;
    decode_ok(AUTO_FX_N, sizeof AUTO_FX_N, &r);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_REMIND, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1u, r.then[0].partition);
    TEST_ASSERT_EQUAL_UINT8(1u, r.then[0].option);

    decode_ok(AUTO_FX_O, sizeof AUTO_FX_O, &r);
    TEST_ASSERT_EQUAL_UINT8(2u, r.n);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_NOTIFY, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_OUTPUT, r.then[1].kind);
    TEST_ASSERT_EQUAL_UINT8(2u, r.then[1].output);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, r.then[1].dev_action);
    TEST_ASSERT_EQUAL_UINT16(30u, r.then[1].off_after_min);

    decode_ok(AUTO_FX_P, sizeof AUTO_FX_P, &r);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_TROUBLE, r.when.kind);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_AC, r.when.trouble);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_CHIME, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(0u, r.then[0].option);

    decode_ok(AUTO_FX_Q, sizeof AUTO_FX_Q, &r);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_CHECK_ARMED, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(0x1Fu, r.when.days);

    decode_ok(AUTO_FX_R, sizeof AUTO_FX_R, &r);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_MASTER, r.made_by);
    TEST_ASSERT_EQUAL_UINT8(ALARM_USER_INSTALLER, r.saved_by);
    TEST_ASSERT_EQUAL_UINT8(0u, r.keep_len); /* a known ignorable field, not kept as unknown */

    TEST_ASSERT_EQUAL_INT(AUTO_REC_NEWER, auto_decode(AUTO_FX_S, sizeof AUTO_FX_S, &r));
}

/* ---- the validator (AUT-001, AUT-002) ---- */

static void test_check(void)
{
    const auto_env_t env = env_of();
    auto_rule_t r = at_time(1380);
    remind(&r.then[0], 1, true);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.when.kind = AUTO_TRIG_ZONE_OPENED; /* a reminder looks at the panel at its time */
    r.when.zone = 1;
    r.when.minute = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NEEDS_TIME, auto_rule_check(&r, &env));
    r = at_time(1380);
    remind(&r.then[0], 1, false);
    r.n = 2;
    r.then[1].kind = AUTO_ACT_NOTIFY;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_ARM_ALONE, auto_rule_check(&r, &env)); /* alone */
    r = at_time(1380);
    remind(&r.then[0], 2, false); /* partition 2 isn't enabled */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_PARTITION, auto_rule_check(&r, &env));
    r.then[0].partition = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_PARTITION, auto_rule_check(&r, &env));
    r = at_time(1380);
    r.then[0].kind = AUTO_ACT_CHECK_ARMED;
    r.then[0].partition = 1;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.then[0].option = 1; /* "still armed" has no option */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));

    /* outputs: only one "For automations"; Off has no "Turn it off after" */
    r = at_time(1380);
    output(&r.then[0], 2, ACT_ON, 30);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.then[0].output = 1;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_OUTPUT, auto_rule_check(&r, &env));
    r.then[0].output = AUTO_OUTPUTS + 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    output(&r.then[0], 2, ACT_OFF, 30);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    output(&r.then[0], 2, ACT_TOGGLE, 0);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    output(&r.then[0], 2, ACT_ON, 0);
    strcpy(r.then[0].ref, "light.porch"); /* no device fields */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));

    /* a notification with an output; never two notifications, nor one output twice */
    r = at_time(1380);
    r.n = 2;
    r.then[0].kind = AUTO_ACT_NOTIFY;
    output(&r.then[1], 2, ACT_ON, 0);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.then[1] = r.then[0];
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_SAME_DEVICE, auto_rule_check(&r, &env));
    output(&r.then[0], 2, ACT_ON, 0);
    output(&r.then[1], 2, ACT_OFF, 0);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_SAME_DEVICE, auto_rule_check(&r, &env));
    r.n = 1;
    r.then[0].kind = AUTO_ACT_NOTIFY;
    r.then[0].output = 0;
    r.then[0].dev_action = ACT_NONE;
    r.then[0].partition = 1; /* a notification names no partition */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));

    /* the chime acts as the person who saved it */
    r = at_time(1320);
    r.then[0].kind = AUTO_ACT_CHIME;
    r.then[0].partition = 1;
    r.then[0].option = 1;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.made_by = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_MAKER, auto_rule_check(&r, &env));

    /* the trouble trigger: its trouble, and only on it */
    r = at_time(0);
    r.when.kind = AUTO_TRIG_TROUBLE;
    r.when.trouble = AUTO_TROUBLE_SIREN;
    r.then[0].kind = AUTO_ACT_NOTIFY;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.when.trouble = AUTO_TROUBLE_OTHER; /* a cause only, never a trigger's */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    r = at_time(600);
    r.when.trouble = AUTO_TROUBLE_AC;
    r.then[0].kind = AUTO_ACT_NOTIFY;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    r.when.trouble = 0;
    r.when.kind = AUTO_TRIG_TROUBLE; /* Remind after a trouble: needs a time */
    remind(&r.then[0], 1, false);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NEEDS_TIME, auto_rule_check(&r, &env));
}

/* BL-224: the installer may save an Arm (it acts as the system master), never a Disarm. */
static void test_installer(void)
{
    auto_env_t env = env_of();
    env.by_installer = true;
    auto_rule_t r = at_time(1380);
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_AWAY;
    r.then[0].warn_min = 5;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.then[0].kind = AUTO_ACT_DISARM;
    r.then[0].arm_level = 0;
    r.then[0].warn_min = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
}

/* ---- the observer (AUT-003) ---- */

static auto_watch_t W;
static alarm_snapshot_t SNAP;

static size_t snap(auto_occ_t *o, size_t max)
{
    return auto_watch_snapshot(&W, &SNAP, o, max);
}

static void test_watch_troubles(void)
{
    memset(&SNAP, 0, sizeof SNAP);
    auto_watch_init(&W, 0, 0);
    SNAP.taken_ms = 10;
    SNAP.sys_troubles = 1u << ALARM_TRBL_AC_LOSS; /* shown at start: a baseline, not news */
    auto_occ_t o[4];
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4));
    SNAP.sys_troubles = 0;
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4));
    SNAP.sys_troubles = 1u << ALARM_TRBL_AC_LOSS;
    TEST_ASSERT_EQUAL_size_t(1, snap(o, 4));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_TROUBLE, o[0].kind);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_AC, o[0].cause);
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4)); /* never again while it lasts */
    SNAP.sys_troubles |= 1u << ALARM_TRBL_CLOCK_NOT_SET; /* the clock is no trouble to act on */
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4));
    SNAP.sys_troubles |= (1u << ALARM_TRBL_BELL) | (1u << ALARM_TRBL_NETWORK) | (1u << ALARM_TRBL_LOW_BATTERY);
    TEST_ASSERT_EQUAL_size_t(3, snap(o, 4));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_BATTERY, o[0].cause);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_SIREN, o[1].cause);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_OTHER, o[2].cause);
    SNAP.zone[4].flags = ALARM_ZS_SUPERVISION; /* a sensor not heard: one per zone */
    TEST_ASSERT_EQUAL_size_t(1, snap(o, 4));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_SENSOR, o[0].cause);
    TEST_ASSERT_EQUAL_UINT16(5u, o[0].zone);
    SNAP.dev_troubles = 1u; /* a keypad not heard */
    TEST_ASSERT_EQUAL_size_t(1, snap(o, 4));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_SENSOR, o[0].cause);
    /* installer mode takes a new baseline: what shows when it ends is not news */
    SNAP.program = true;
    SNAP.sys_troubles = 0;
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4));
    SNAP.sys_troubles = 1u << ALARM_TRBL_AC_LOSS;
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4));
    SNAP.program = false;
    TEST_ASSERT_EQUAL_size_t(0, snap(o, 4));
}

/* ---- the runner (AUT-006, AUT-007) ---- */

typedef struct {
    unsigned n_out;
    uint8_t out_n[8];
    bool out_on[8];
    uint64_t out_at[8];
    bool out_answer;
    unsigned n_chime;
    bool chime_on;
    uint8_t chime_maker, chime_part;
    unsigned n_notify;
    auto_notice_t notice;
    bool service;
    int armed;
    unsigned n_remind;
    auto_remind_t remind;
    uint64_t now;
} fake_t;

static fake_t F;
static auto_run_t R;
static auto_rule_t RULES[AUTO_MAX];
static size_t NR;

static bool port_output(void *ctx, uint8_t out, bool on)
{
    (void)ctx;
    if (F.n_out < 8u) {
        F.out_n[F.n_out] = out;
        F.out_on[F.n_out] = on;
        F.out_at[F.n_out] = F.now;
    }
    F.n_out++;
    return F.out_answer;
}

static alarm_result_t port_chime(void *ctx, uint16_t uid, uint8_t p, bool on, uint8_t maker,
                                 bool *already)
{
    (void)ctx;
    (void)uid;
    F.n_chime++;
    F.chime_on = on;
    F.chime_part = p;
    F.chime_maker = maker;
    *already = false;
    return ALARM_OK;
}

static bool port_notify(void *ctx, const auto_notice_t *n)
{
    (void)ctx;
    if (!F.service) {
        return false;
    }
    F.n_notify++;
    F.notice = *n;
    return true;
}

static int port_armed(void *ctx, uint8_t p)
{
    (void)ctx;
    (void)p;
    return F.armed;
}

static void port_remind(void *ctx, const auto_remind_t *m)
{
    (void)ctx;
    F.n_remind++;
    F.remind = *m;
}

static void start(void)
{
    memset(&F, 0, sizeof F);
    F.now = T0;
    F.out_answer = true;
    F.service = true;
    auto_ports_t p;
    memset(&p, 0, sizeof p);
    p.output = port_output;
    p.chime = port_chime;
    p.notify = port_notify;
    p.part_armed = port_armed;
    p.remind = port_remind;
    auto_run_init(&R, &p, T0);
    memset(RULES, 0, sizeof RULES);
    NR = 0;
}

static void load(void)
{
    const auto_env_t env = env_of();
    for (size_t i = 0; i < NR; i++) {
        RULES[i].uid = (uint16_t)(i + 1u);
        RULES[i].slot = (uint8_t)(i + 1u);
        TEST_ASSERT_EQUAL_INT_MESSAGE(AUTO_OK, auto_rule_check(&RULES[i], &env), "rule");
    }
    auto_run_rules(&R, RULES, NR, &env);
}

static void advance(uint64_t ms)
{
    const uint64_t end = F.now + ms;
    for (;;) {
        uint64_t d = auto_run_next_deadline(&R, F.now);
        if (d > end) {
            break;
        }
        F.now = d;
        auto_run_tick(&R, F.now);
        if (auto_run_next_deadline(&R, F.now) == F.now) {
            F.now++; /* nothing due again at once */
        }
    }
    F.now = end;
    auto_run_tick(&R, F.now);
}

static uint8_t status(uint16_t uid)
{
    auto_status_t st;
    auto_run_status(&R, uid, &st);
    return st.result;
}

static void occur(uint8_t kind, uint8_t cause, uint16_t zone, uint8_t partition)
{
    auto_occ_t o;
    memset(&o, 0, sizeof o);
    o.kind = kind;
    o.cause = cause;
    o.zone = zone;
    o.partition = partition;
    o.level = ALARM_LEVEL_AWAY;
    const auto_local_t lt = {true, 1800000000, 20000, 1380, 3};
    auto_run_occurrence(&R, &o, &lt, F.now);
}

static void cross(int32_t day, uint16_t minute)
{
    F.now += S_(61); /* a reading a minute or more after the last */
    const auto_local_t lt = {true, 1800000000, day, minute, 3};
    const auto_span_t s = {day, 3, minute, minute};
    auto_run_time(&R, NULL, &lt, &s, 1, F.now);
}

/* A wired output on for its time, the 10 s gap, the Off never lost. */
static void test_output(void)
{
    start();
    RULES[0] = at_time(0);
    RULES[0].when.kind = AUTO_TRIG_ALARM;
    RULES[0].when.minute = 0;
    output(&RULES[0].then[0], 2, ACT_ON, 1);
    NR = 1;
    load();
    occur(AUTO_TRIG_ALARM, ALARM_CAUSE_BURGLARY, 3, 1);
    TEST_ASSERT_EQUAL_UINT(1, F.n_out);
    TEST_ASSERT_EQUAL_UINT8(2u, F.out_n[0]);
    TEST_ASSERT_TRUE(F.out_on[0]);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(1));
    advance(S_(59));
    TEST_ASSERT_EQUAL_UINT(1, F.n_out);
    advance(S_(2));
    TEST_ASSERT_EQUAL_UINT(2, F.n_out);
    TEST_ASSERT_FALSE(F.out_on[1]);
    TEST_ASSERT_EQUAL_UINT64(T0 + S_(60), F.out_at[1]);

    /* two quick runs: the second waits for the gap */
    occur(AUTO_TRIG_ALARM, ALARM_CAUSE_BURGLARY, 3, 1);
    TEST_ASSERT_EQUAL_UINT(2, F.n_out); /* the Off was 1 s ago */
    advance(S_(10));
    TEST_ASSERT_EQUAL_UINT(3, F.n_out);
    TEST_ASSERT_TRUE(F.out_on[2]);
    TEST_ASSERT_TRUE(F.out_at[2] >= F.out_at[1] + AUTO_DEVICE_GAP_MS);

    /* refused: unreachable */
    F.out_answer = false;
    advance(S_(120));
    occur(AUTO_TRIG_ALARM, ALARM_CAUSE_BURGLARY, 3, 1);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_UNREACHABLE, status(1));

    /* Try it now switches it, marked tried */
    F.out_answer = true;
    advance(S_(120));
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_OK, auto_run_try(&R, 1, F.now));
    auto_status_t st;
    auto_run_status(&R, 1, &st);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, st.result);
    TEST_ASSERT_EQUAL_UINT8(1u, st.tried);
}

/* A notification: its words' facts from the trigger; none set up; one a minute. */
static void test_notify(void)
{
    start();
    RULES[0] = at_time(0);
    RULES[0].when.kind = AUTO_TRIG_ZONE_OPENED;
    RULES[0].when.zone = 1;
    RULES[0].when.minute = 0;
    RULES[0].then[0].kind = AUTO_ACT_NOTIFY;
    NR = 1;
    load();
    occur(AUTO_TRIG_ZONE_OPENED, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_notify);
    TEST_ASSERT_EQUAL_UINT8(AUTO_NOTICE_EVENT, F.notice.what);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, F.notice.trig);
    TEST_ASSERT_EQUAL_UINT16(1u, F.notice.zone);
    TEST_ASSERT_EQUAL_UINT16(1u, F.notice.uid);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(1));
    advance(S_(31)); /* past the zone's 30 s, inside the minute: dropped */
    occur(AUTO_TRIG_ZONE_OPENED, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT(1, F.n_notify);
    advance(S_(30));
    occur(AUTO_TRIG_ZONE_OPENED, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT(2, F.n_notify);
    F.service = false;
    advance(S_(61));
    occur(AUTO_TRIG_ZONE_OPENED, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NO_SERVICE, status(1));
}

/* A trouble trigger: its own trouble, or any. */
static void test_trouble_trigger(void)
{
    start();
    RULES[0] = at_time(0);
    RULES[0].when.kind = AUTO_TRIG_TROUBLE;
    RULES[0].when.trouble = AUTO_TROUBLE_AC;
    RULES[0].then[0].kind = AUTO_ACT_NOTIFY;
    RULES[1] = RULES[0];
    RULES[1].when.trouble = AUTO_TROUBLE_ANY;
    NR = 2;
    load();
    occur(AUTO_TRIG_TROUBLE, AUTO_TROUBLE_SENSOR, 5, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NONE, status(1));
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(2));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_SENSOR, F.notice.cause);
    TEST_ASSERT_EQUAL_UINT16(5u, F.notice.zone);
    occur(AUTO_TRIG_TROUBLE, AUTO_TROUBLE_AC, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(1));
    TEST_ASSERT_EQUAL_UINT8(AUTO_TROUBLE_AC, F.notice.cause);
}

/* Remind to arm: disarmed, the note and a notification; armed, nothing; the note ends when
 * armed, at 6:00 AM, or when the automation is turned off. */
static void test_remind(void)
{
    start();
    RULES[0] = at_time(1380);
    remind(&RULES[0].then[0], 1, true);
    NR = 1;
    load();
    cross(20000, 1380);
    TEST_ASSERT_EQUAL_UINT(1, F.n_remind);
    TEST_ASSERT_EQUAL_UINT8(1u, F.remind.on);
    TEST_ASSERT_EQUAL_UINT8(1u, F.remind.partition);
    TEST_ASSERT_EQUAL_UINT16(1380u, F.remind.minute);
    TEST_ASSERT_EQUAL_UINT(1, F.n_notify);
    TEST_ASSERT_EQUAL_UINT8(AUTO_NOTICE_REMIND, F.notice.what);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(1));
    TEST_ASSERT_TRUE(auto_run_wants_clock(&R));
    occur(AUTO_TRIG_ARMED, 0, 0, 1);
    TEST_ASSERT_EQUAL_UINT(2, F.n_remind);
    TEST_ASSERT_EQUAL_UINT8(0u, F.remind.on);

    /* the next night it stays until 6:00 AM */
    cross(20001, 1380);
    TEST_ASSERT_EQUAL_UINT(3, F.n_remind);
    cross(20002, 359);
    TEST_ASSERT_EQUAL_UINT(3, F.n_remind);
    cross(20002, 360);
    TEST_ASSERT_EQUAL_UINT(4, F.n_remind);
    TEST_ASSERT_EQUAL_UINT8(0u, F.remind.on);
    TEST_ASSERT_FALSE(auto_run_wants_clock(&R));

    /* armed at its time: nothing, "already" */
    F.armed = 1;
    cross(20002, 1380);
    TEST_ASSERT_EQUAL_UINT(4, F.n_remind);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ALREADY, status(1));

    /* turned off while the note shows: it goes */
    F.armed = 0;
    cross(20003, 1380);
    TEST_ASSERT_EQUAL_UINT(5, F.n_remind);
    RULES[0].off = true;
    load();
    TEST_ASSERT_EQUAL_UINT(6, F.n_remind);
    TEST_ASSERT_EQUAL_UINT8(0u, F.remind.on);

    /* never tried */
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_ARM, auto_run_try(&R, 1, F.now));
}

/* "Still armed": a notification only while armed at its time. */
static void test_still_armed(void)
{
    start();
    RULES[0] = at_time(960);
    RULES[0].then[0].kind = AUTO_ACT_CHECK_ARMED;
    RULES[0].then[0].partition = 1;
    NR = 1;
    load();
    F.armed = 0;
    cross(20000, 960);
    TEST_ASSERT_EQUAL_UINT(0, F.n_notify);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ALREADY, status(1));
    F.armed = 1;
    cross(20001, 960);
    TEST_ASSERT_EQUAL_UINT(1, F.n_notify);
    TEST_ASSERT_EQUAL_UINT8(AUTO_NOTICE_STILL_ARMED, F.notice.what);
    TEST_ASSERT_EQUAL_UINT16(960u, F.notice.minute);
    TEST_ASSERT_EQUAL_UINT8(1u, F.notice.partition);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(1));
    F.service = false;
    cross(20002, 960);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NO_SERVICE, status(1));
    F.armed = -1;
    cross(20003, 960);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_CANT_RUN, status(1));
}

/* The door chime, as the person who saved it. */
static void test_chime(void)
{
    start();
    RULES[0] = at_time(1320);
    RULES[0].then[0].kind = AUTO_ACT_CHIME;
    RULES[0].then[0].partition = 1;
    RULES[0].then[0].option = 0;
    RULES[0].made_by = 2;
    NR = 1;
    load();
    cross(20000, 1320);
    TEST_ASSERT_EQUAL_UINT(1, F.n_chime);
    TEST_ASSERT_FALSE(F.chime_on);
    TEST_ASSERT_EQUAL_UINT8(1u, F.chime_part);
    TEST_ASSERT_EQUAL_UINT8(2u, F.chime_maker);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status(1));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixture_v3);
    RUN_TEST(test_check);
    RUN_TEST(test_installer);
    RUN_TEST(test_watch_troubles);
    RUN_TEST(test_output);
    RUN_TEST(test_notify);
    RUN_TEST(test_trouble_trigger);
    RUN_TEST(test_remind);
    RUN_TEST(test_still_armed);
    RUN_TEST(test_chime);
    return UNITY_END();
}
