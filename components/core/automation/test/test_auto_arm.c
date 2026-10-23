/* Arming and disarming automations (phase C; docs/features/41 DESIGN.md section 3 "Arming" 1-7,
 * "Disarming" 1-5): the warning on the monotonic clock and the whole-warning rule (AUT-012, D5-D7),
 * the put-off on every warning (AUT-013, D20, D26, D27), one warning at a time, "Bypass open zones"
 * and the installer option (AUT-014, AUT-015, D29), the disarm (AUT-011, D23), the partition gap
 * and the one-hop rule (AUT-007), and the validator's phase C rules (AUT-016, D24). The clock is
 * walked through auto_clock as the platform would, with EST5EDT's 2026 tables. */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "automation/auto_clock.h"
#include "automation/auto_run.h"
#include "home_control/home_control.h"
#include "rtc_clock/ds3231.h"

#define MONO0 UINT64_C(5000000)
#define SEC UINT64_C(1000)
#define MIN_ UINT64_C(60000)
#define MAX_LOG 64u

/* ---- time zones (as test_auto_clock.c's) ---- */

static int64_t utc(int32_t y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    rtc_civil_t c = {y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s, 0};
    int64_t e = 0;
    TEST_ASSERT_TRUE(rtc_epoch_from_civil(&c, &e));
    return e;
}

/* EST5EDT with the 2026 change dates, and a zone whose spring change is at midnight (UTC-3, then
 * UTC-2 from 2026-11-01 00:00 local: that date starts at 01:00). */
typedef enum { TZ_EST5EDT, TZ_MIDNIGHT } tz_t;

static int64_t tz_offset_s(tz_t tz, int64_t epoch_s)
{
    if (tz == TZ_MIDNIGHT) {
        return epoch_s >= utc(2026, 11, 1, 3, 0, 0) ? -2 * 3600 : -3 * 3600;
    }
    const int64_t on = utc(2026, 3, 8, 7, 0, 0), off = utc(2026, 11, 1, 6, 0, 0);
    return (epoch_s >= on && epoch_s < off) ? -4 * 3600 : -5 * 3600;
}

static auto_local_t reading(tz_t tz, int64_t epoch_s)
{
    rtc_civil_t c;
    rtc_civil_from_epoch(epoch_s + tz_offset_s(tz, epoch_s), &c);
    auto_local_t lt;
    TEST_ASSERT_TRUE(auto_local_from_civil(&c, epoch_s, true, &lt));
    return lt;
}

/* The epoch of a local time that exists once (none near a change). */
static int64_t local(tz_t tz, int32_t y, unsigned mo, unsigned d, unsigned h, unsigned mi)
{
    const int64_t as_utc = utc(y, mo, d, h, mi, 0);
    int64_t e = as_utc + 5 * 3600;
    for (int k = 0; k < 4; k++) {
        e = as_utc - tz_offset_s(tz, e);
    }
    return e;
}

/* ---- the fake ports ---- */

typedef struct {
    tz_t tz;
    uint64_t mono;
    int64_t epoch;
    auto_clock_t clock;
    /* arm */
    unsigned n_arm;
    uint16_t arm_uid;
    uint8_t arm_p, arm_level, arm_flags, arm_maker;
    uint64_t arm_mono[8];
    alarm_result_t arm_res;
    bool arm_already;
    uint16_t arm_zone;
    /* disarm */
    unsigned n_disarm;
    uint8_t disarm_p, disarm_maker;
    uint64_t disarm_mono[8];
    alarm_result_t disarm_res;
    bool disarm_already;
    /* warning */
    unsigned n_warn;
    auto_warning_t warn[MAX_LOG];
    int64_t warn_epoch[MAX_LOG];
    uint64_t warn_mono[MAX_LOG];
    /* failed */
    unsigned n_failed;
    uint8_t failed_reason, failed_action;
    /* put_off */
    unsigned n_put_off;
    uint16_t po_uid;
    uint8_t po_p, po_user, po_choice;
    int64_t po_until;
    /* maker_ok */
    unsigned n_maker;
    bool maker;
    bool option;
    /* devices */
    unsigned n_act;
    uint32_t next_req;
    /* PL04: the revocation gate */
    auto_revoke_t revoked;
    unsigned n_begin, n_end;
} fake_t;

static fake_t F;
static auto_run_t R;
static auto_rule_t RULES[AUTO_MAX];
static size_t NR;
static auto_zone_t Z[ALARM_MAX_ZONES];
static auto_env_t ENV;

static alarm_result_t port_arm(void *ctx, uint16_t uid, uint8_t p, uint8_t level, uint8_t flags,
                               uint8_t maker, bool *already, uint16_t *zone)
{
    fake_t *f = ctx;
    if (f->n_arm < 8u) {
        f->arm_mono[f->n_arm] = f->mono;
    }
    f->n_arm++;
    f->arm_uid = uid;
    f->arm_p = p;
    f->arm_level = level;
    f->arm_flags = flags;
    f->arm_maker = maker;
    *already = f->arm_already;
    *zone = f->arm_zone;
    return f->arm_res;
}

static alarm_result_t port_disarm(void *ctx, uint16_t uid, uint8_t p, uint8_t maker, bool *already)
{
    (void)uid;
    fake_t *f = ctx;
    if (f->n_disarm < 8u) {
        f->disarm_mono[f->n_disarm] = f->mono;
    }
    f->n_disarm++;
    f->disarm_p = p;
    f->disarm_maker = maker;
    *already = f->disarm_already;
    return f->disarm_res;
}

static void port_warning(void *ctx, const auto_warning_t *w)
{
    fake_t *f = ctx;
    TEST_ASSERT_LESS_THAN_UINT(MAX_LOG, f->n_warn);
    f->warn[f->n_warn] = *w;
    f->warn_epoch[f->n_warn] = f->epoch;
    f->warn_mono[f->n_warn] = f->mono;
    f->n_warn++;
}

static void port_failed(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action, uint16_t zone)
{
    (void)uid, (void)zone;
    fake_t *f = ctx;
    f->n_failed++;
    f->failed_reason = reason;
    f->failed_action = dev_action;
}

static void port_put_off(void *ctx, uint16_t uid, uint8_t partition, uint8_t user, uint8_t choice,
                         int64_t until_epoch_ms)
{
    fake_t *f = ctx;
    f->n_put_off++;
    f->po_uid = uid;
    f->po_p = partition;
    f->po_user = user;
    f->po_choice = choice;
    f->po_until = until_epoch_ms;
}

static bool port_maker(void *ctx, uint8_t user, bool *option_on)
{
    (void)user;
    fake_t *f = ctx;
    f->n_maker++;
    *option_on = f->option;
    return f->maker;
}

static int port_act(void *ctx, const auto_action_t *a, bool less_secure, uint32_t *req)
{
    (void)a, (void)less_secure;
    fake_t *f = ctx;
    f->n_act++;
    *req = ++f->next_req;
    return HC_OK;
}

static bool port_kind(void *ctx, uint8_t source, const char *ref, uint8_t *kind)
{
    (void)ctx, (void)source, (void)ref;
    *kind = DEV_LIGHT;
    return true;
}

static bool port_begin(void *ctx, uint16_t uid)
{
    fake_t *f = ctx;
    TEST_ASSERT_EQUAL_UINT(f->n_begin, f->n_end);
    f->n_begin++;
    return !auto_revoke_has(&f->revoked, uid);
}

static void port_end(void *ctx)
{
    fake_t *f = ctx;
    f->n_end++;
    TEST_ASSERT_EQUAL_UINT(f->n_begin, f->n_end);
}

static const auto_warning_t *last_warn(void)
{
    TEST_ASSERT_NOT_EQUAL(0u, F.n_warn);
    return &F.warn[F.n_warn - 1u];
}

/* ---- the rig ---- */

void setUp(void)
{
    memset(&F, 0, sizeof F);
    F.tz = TZ_EST5EDT;
    F.mono = MONO0;
    F.arm_res = ALARM_OK;
    F.disarm_res = ALARM_OK;
    F.maker = true;
    F.option = true;
    auto_clock_init(&F.clock);
    const auto_ports_t p = {.ctx = &F,
                            .device_act = port_act,
                            .device_kind = port_kind,
                            .arm = port_arm,
                            .warning = port_warning,
                            .failed = port_failed,
                            .maker_ok = port_maker,
                            .disarm = port_disarm,
                            .put_off = port_put_off,
                            .send_begin = port_begin,
                            .send_end = port_end};
    auto_run_init(&R, &p, F.mono);
    memset(RULES, 0, sizeof RULES);
    NR = 0;
    memset(Z, 0, sizeof Z);
    Z[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    ENV = (auto_env_t){0x03u, true, Z, true, false, 0u};
}

void tearDown(void) {}

static void load(void)
{
    auto_run_rules(&R, RULES, NR, &ENV);
}

static auto_rule_t *arm_rule(uint16_t uid, uint8_t trig, uint8_t p, uint8_t level, uint8_t warn)
{
    auto_rule_t *r = &RULES[NR++];
    memset(r, 0, sizeof *r);
    r->uid = uid;
    r->slot = (uint8_t)NR;
    r->made_by = 2u;
    r->n = 1u;
    r->when.kind = trig;
    if (trig == AUTO_TRIG_ZONE_OPENED) {
        r->when.zone = 1u;
    }
    r->then[0].kind = AUTO_ACT_ARM;
    r->then[0].value = -1;
    r->then[0].partition = p;
    r->then[0].arm_level = level;
    r->then[0].warn_min = warn;
    return r;
}

static auto_rule_t *disarm_rule(uint16_t uid, uint8_t trig, uint8_t p)
{
    auto_rule_t *r = &RULES[NR++];
    memset(r, 0, sizeof *r);
    r->uid = uid;
    r->slot = (uint8_t)NR;
    r->made_by = 2u;
    r->n = 1u;
    r->when.kind = trig;
    if (trig == AUTO_TRIG_ZONE_OPENED) {
        r->when.zone = 1u;
    }
    r->then[0].kind = AUTO_ACT_DISARM;
    r->then[0].value = -1;
    r->then[0].partition = p;
    r->then[0].confirmed = 1u; /* a door trigger asks for it (D24) */
    return r;
}

/* One reading of the clock as the platform makes it, then the runner's tick. */
static void step(void)
{
    const auto_local_t lt = reading(F.tz, F.epoch);
    auto_span_t sp[2];
    const size_t n = auto_clock_step(&F.clock, F.mono, &lt, sp);
    auto_run_time(&R, &F.clock, &lt, sp, n, F.mono);
    auto_run_tick(&R, F.mono);
}

static void begin(int64_t epoch)
{
    F.epoch = epoch;
    step();
}

/* Wall and monotonic clocks advance together to `until`, a reading every step_s seconds. */
static void walk(int64_t until, int64_t step_s)
{
    while (F.epoch < until) {
        const int64_t d = F.epoch + step_s <= until ? step_s : until - F.epoch;
        F.epoch += d;
        F.mono += (uint64_t)d * SEC;
        step();
    }
}

/* The wall clock is set by delta seconds; the monotonic one moves on one second. */
static void set_clock(int64_t delta_s)
{
    F.epoch += delta_s;
    F.mono += SEC;
    step();
}

static void occ(uint8_t kind, uint8_t partition, uint8_t by_auto)
{
    const auto_occ_t o = {kind, partition, ALARM_LEVEL_AWAY, 0, kind == AUTO_TRIG_ZONE_OPENED ? 1u : 0u,
                          F.mono, by_auto};
    const auto_local_t lt = reading(F.tz, F.epoch);
    auto_run_occurrence(&R, &o, &lt, F.mono);
}

static void advance(uint64_t ms)
{
    F.mono += ms;
    F.epoch += (int64_t)(ms / SEC);
    auto_run_tick(&R, F.mono);
}

static auto_status_t status_of(uint16_t uid)
{
    auto_status_t st;
    auto_run_status(&R, uid, &st);
    return st;
}

static auto_put_off_t put_off(uint16_t uid, uint8_t choice, uint8_t user, bool may_disarm)
{
    const auto_local_t lt = reading(F.tz, F.epoch);
    return auto_run_put_off(&R, uid, choice, user, may_disarm, &lt, F.mono);
}

/* ---- the warning (AUT-012) ---- */

/* AUT-012, D5-D7: a 22:30 Night arming warns at 22:25 (a steady state, its time, its level) and
 * arms a whole warning later on the monotonic clock, as its maker, with no flags. */
static void test_whole_warning(void)
{
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 24) + 55, 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 25) + 5, 5);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_warn);
    const auto_warning_t *w = last_warn();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, w->state);
    TEST_ASSERT_EQUAL_UINT16(1u, w->uid);
    TEST_ASSERT_EQUAL_UINT8(1u, w->partition);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_NIGHT, w->level);
    TEST_ASSERT_EQUAL_UINT8(5u, w->warn_min);
    TEST_ASSERT_EQUAL_UINT8(0u, w->bypass);
    TEST_ASSERT_EQUAL_UINT8(0u, w->put_off_by);
    const int64_t at = local(TZ_EST5EDT, 2026, 6, 10, 22, 30) * 1000;
    TEST_ASSERT_INT64_WITHIN(5000, at, w->arm_at_epoch_ms);
    TEST_ASSERT_EQUAL_UINT64(F.warn_mono[0] + 5u * MIN_, w->arm_at_ms);
    auto_warning_t now;
    auto_run_warning(&R, 1, &now);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, now.state);
    TEST_ASSERT_EQUAL_UINT64(w->arm_at_ms, auto_run_next_deadline(&R, F.mono));

    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 29) + 55, 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 31), 5);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_TRUE(F.arm_mono[0] >= F.warn_mono[0] + 5u * MIN_);
    TEST_ASSERT_TRUE(F.arm_mono[0] < F.warn_mono[0] + 5u * MIN_ + 5u * SEC);
    TEST_ASSERT_EQUAL_UINT16(1u, F.arm_uid);
    TEST_ASSERT_EQUAL_UINT8(1u, F.arm_p);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_NIGHT, F.arm_level);
    TEST_ASSERT_EQUAL_UINT8(0u, F.arm_flags);
    TEST_ASSERT_EQUAL_UINT8(2u, F.arm_maker);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status_of(1).result);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed); /* its own minute passing during the warning is no
                                               "no warning" */
    /* the next day, once again */
    walk(local(TZ_EST5EDT, 2026, 6, 11, 22, 31), 30);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
}

/* AUT-012: a clock set forward (or back) during the warning never shortens it: the arm is due a
 * whole warning after it began, on the monotonic clock. */
static void test_clock_jump_keeps_the_warning(void)
{
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_AWAY, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 25) + 5, 5);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_warn);
    const uint64_t start = F.warn_mono[0];
    set_clock(4 * 60); /* the clock jumps to 22:29: its arm minute comes a minute later */
    walk(F.epoch + 3 * 60, 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm); /* 22:30 has passed on the wall clock */
    while (F.mono + 5u * SEC < start + 5u * MIN_) {
        F.epoch += 5;
        F.mono += 5u * SEC;
        step();
    }
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    walk(F.epoch + 10, 5);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_TRUE(F.arm_mono[0] >= start + 5u * MIN_);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);

    /* and set back by 3 minutes during the next day's warning: still a whole warning, once */
    walk(local(TZ_EST5EDT, 2026, 6, 11, 22, 25) + 5, 5);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    const uint64_t start2 = F.warn_mono[F.n_warn - 1u];
    set_clock(-3 * 60);
    walk(F.epoch + 4 * 60 + 50, 5);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    walk(F.epoch + 20, 5);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_arm);
    TEST_ASSERT_TRUE(F.arm_mono[1] >= start2 + 5u * MIN_);
    walk(F.epoch + 10 * 60, 5);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_arm); /* 22:25-22:30 again on the wall clock: not again */
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
}

/* AUT-012, DESIGN.md "Arming" 3: without a whole warning there is no arm that day - after a
 * restart that lost the warning, and when one reading crosses both the warning's minute and the
 * arm's (the clock set across them). The status says so and the failed port gets no_warning. */
static void test_no_whole_warning_no_arm(void)
{
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    /* a restart at 22:27: the runner and its clock start afresh */
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 27));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 40), 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    const auto_status_t st = status_of(1);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NO_WARNING, st.result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_NO_WARNING, st.reason);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_failed);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_FAIL_NO_WARNING, F.failed_reason);
    TEST_ASSERT_EQUAL_UINT8(ALARM_AUTO_ACT_ARM, F.failed_action);

    /* the next day the clock is set from 22:24 to 22:31 in one step (under T2's 10 minutes) */
    walk(local(TZ_EST5EDT, 2026, 6, 11, 22, 24), 30);
    set_clock(7 * 60);
    walk(F.epoch + 15 * 60, 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn); /* no late warning either */
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NO_WARNING, status_of(1).result);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_failed); /* a day later: not deduplicated */

    /* the warning lost to a restart in the middle of it: no arm, no_warning */
    setUp();
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 12, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 12, 22, 26), 5);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    const auto_ports_t p = R.port;
    auto_run_init(&R, &p, F.mono); /* the restart */
    auto_clock_init(&F.clock);
    load();
    walk(local(TZ_EST5EDT, 2026, 6, 12, 22, 40), 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NO_WARNING, status_of(1).result);
}

/* AUT-012: a 00:02 Monday arming warns on Sunday at 23:57 and arms on the time. */
static void test_warning_before_midnight(void)
{
    auto_rule_t *r = arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_AWAY, 5);
    r->when.minute = 2u;
    r->when.days = AUTO_DAY_BIT(1);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 13, 23, 50)); /* Saturday: nothing */
    walk(local(TZ_EST5EDT, 2026, 6, 14, 0, 10), 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    walk(local(TZ_EST5EDT, 2026, 6, 14, 23, 56) + 50, 10); /* Sunday */
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    walk(local(TZ_EST5EDT, 2026, 6, 14, 23, 57) + 10, 10);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_warn);
    walk(local(TZ_EST5EDT, 2026, 6, 15, 0, 1) + 50, 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    walk(local(TZ_EST5EDT, 2026, 6, 15, 0, 3), 10);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
}

/* ---- the put-off (AUT-013) ---- */

/* AUT-013, D20, D26: "An hour later" on every warning - the planned time + 60 minutes, the card
 * gone meanwhile, the warning again 5 minutes before; each put-off told to the put_off port (the
 * event) and kept in the status. */
static void test_put_off_an_hour_twice(void)
{
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 26), 5);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_HOUR, 3, false));
    const int64_t at1 = local(TZ_EST5EDT, 2026, 6, 10, 23, 30) * 1000;
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_HOUR, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT8(3u, last_warn()->put_off_by);
    TEST_ASSERT_INT64_WITHIN(5000, at1, last_warn()->arm_at_epoch_ms);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_put_off);
    TEST_ASSERT_EQUAL_UINT16(1u, F.po_uid);
    TEST_ASSERT_EQUAL_UINT8(1u, F.po_p);
    TEST_ASSERT_EQUAL_UINT8(3u, F.po_user);
    TEST_ASSERT_EQUAL_UINT8(AUTO_PUT_OFF_HOUR, F.po_choice);
    TEST_ASSERT_INT64_WITHIN(5000, at1, F.po_until);
    auto_status_t st = status_of(1);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_PUT_OFF, st.result);
    TEST_ASSERT_EQUAL_UINT8(3u, st.user);
    TEST_ASSERT_EQUAL_UINT8(AUTO_PUT_OFF_HOUR, st.choice);
    TEST_ASSERT_INT64_WITHIN(5000, at1, st.until_epoch_ms);

    const unsigned warns = F.n_warn;
    walk(local(TZ_EST5EDT, 2026, 6, 10, 23, 24) + 50, 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed); /* 22:30 passed while put off: no "no warning" */
    TEST_ASSERT_EQUAL_UINT(warns, F.n_warn);
    walk(local(TZ_EST5EDT, 2026, 6, 10, 23, 25) + 10, 10);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state); /* D20: 5 minutes before */
    TEST_ASSERT_INT64_WITHIN(5000, at1, last_warn()->arm_at_epoch_ms);

    /* every warning: again, by another code */
    walk(local(TZ_EST5EDT, 2026, 6, 10, 23, 26), 10);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_HOUR, 4, false));
    const int64_t at2 = local(TZ_EST5EDT, 2026, 6, 11, 0, 30) * 1000;
    TEST_ASSERT_INT64_WITHIN(5000, at2, F.po_until);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_put_off);
    walk(local(TZ_EST5EDT, 2026, 6, 11, 0, 29) + 50, 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    walk(local(TZ_EST5EDT, 2026, 6, 11, 0, 31), 10);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status_of(1).result);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
}

/* AUT-013, D26, D27: "Not tonight" holds this automation until 6:00 AM local, through either
 * daylight-saving night (EST5EDT 2026: the fall-back and the spring-forward), and the line goes
 * at 6:00 AM by the wall clock. */
static void check_not_tonight(int32_t y, unsigned mo, unsigned d, unsigned mo2, unsigned d2,
                              int64_t six_am)
{
    setUp();
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, y, mo, d, 22, 20));
    walk(local(TZ_EST5EDT, y, mo, d, 22, 26), 5);
    const auto_local_t then = reading(TZ_EST5EDT, F.epoch);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_NIGHT, 3, true));
    const auto_warning_t *w = last_warn();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NIGHT, w->state);
    TEST_ASSERT_EQUAL_UINT8(3u, w->put_off_by);
    TEST_ASSERT_EQUAL_INT32(then.day_no + 1, w->until_day);
    TEST_ASSERT_EQUAL_UINT16(360u, w->until_minute);
    TEST_ASSERT_EQUAL_UINT8(AUTO_PUT_OFF_NIGHT, F.po_choice);
    TEST_ASSERT_EQUAL_INT64(0, F.po_until);
    TEST_ASSERT_EQUAL_UINT8(1u, status_of(1).night);
    TEST_ASSERT_TRUE(auto_run_wants_clock(&R));
    const unsigned warns = F.n_warn;
    walk(six_am - 30, 30);
    TEST_ASSERT_EQUAL_UINT(warns, F.n_warn);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NIGHT, R.e[0].arm.state);
    walk(six_am + 30, 30);
    TEST_ASSERT_EQUAL_UINT(warns + 1u, F.n_warn);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    TEST_ASSERT_TRUE(F.warn_epoch[F.n_warn - 1u] >= six_am);
    TEST_ASSERT_EQUAL_UINT16(360u, reading(TZ_EST5EDT, F.warn_epoch[F.n_warn - 1u]).minute);
    TEST_ASSERT_FALSE(auto_run_wants_clock(&R));
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
    /* and it arms that evening as usual */
    walk(local(TZ_EST5EDT, y, mo2, d2, 22, 31), 60);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
}

static void test_not_tonight_until_six_across_dst(void)
{
    /* fall back: 2026-11-01 06:00 EST is 11:00 UTC (the night is an hour longer) */
    check_not_tonight(2026, 10, 31, 11, 1, utc(2026, 11, 1, 11, 0, 0));
    /* spring forward: 2026-03-08 06:00 EDT is 10:00 UTC (an hour shorter) */
    check_not_tonight(2026, 3, 7, 3, 8, utc(2026, 3, 8, 10, 0, 0));
}

/* AUT-013: for an arm time from 6 AM to 6 PM the choice reads "Not today" and holds until
 * midnight - also where the date's midnight is skipped (a zone that springs forward at 00:00:
 * it ends at the date's first minute, 01:00). */
static void test_not_today_until_midnight_across_dst(void)
{
    F.tz = TZ_MIDNIGHT;
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_AWAY, 5)->when.minute = 10u * 60u;
    load();
    begin(local(TZ_MIDNIGHT, 2026, 10, 31, 9, 50));
    walk(local(TZ_MIDNIGHT, 2026, 10, 31, 9, 56), 5);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_NIGHT, 3, true));
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_DAY, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT16(0u, last_warn()->until_minute);
    TEST_ASSERT_EQUAL_UINT8(0u, status_of(1).night);
    const unsigned warns = F.n_warn;
    const int64_t midnight = utc(2026, 11, 1, 3, 0, 0); /* 00:00 UTC-3 = 01:00 UTC-2 */
    walk(midnight - 60, 60);
    TEST_ASSERT_EQUAL_UINT(warns, F.n_warn);
    walk(midnight + 60, 60);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    TEST_ASSERT_EQUAL_INT64(midnight, F.warn_epoch[F.n_warn - 1u]);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);

    /* in EST5EDT, until 24:00 on the day it springs forward */
    setUp();
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_AWAY, 5)->when.minute = 10u * 60u;
    load();
    begin(local(TZ_EST5EDT, 2026, 3, 8, 9, 50));
    walk(local(TZ_EST5EDT, 2026, 3, 8, 9, 56), 5);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_NIGHT, 3, true));
    walk(utc(2026, 3, 9, 4, 0, 0) - 60, 60); /* 2026-03-09 00:00 EDT = 04:00 UTC */
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_DAY, last_warn()->state);
    walk(utc(2026, 3, 9, 4, 0, 0) + 60, 60);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
}

/* AUT-013 (decided 2026-10-05): "Not tonight" only with a code that may disarm the partition,
 * and with the clock set; another code gets the hour. Nothing is put off without a warning. */
static void test_put_off_refusals(void)
{
    arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 5);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_NO_WARNING, put_off(1, AUTO_PUT_OFF_HOUR, 3, true));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    const unsigned warns = F.n_warn;
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_NOT_ALLOWED, put_off(1, AUTO_PUT_OFF_NIGHT, 3, false));
    const auto_local_t unset = {0};
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_NO_CLOCK,
                          auto_run_put_off(&R, 1, AUTO_PUT_OFF_NIGHT, 3, true, &unset, F.mono));
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_NO_CLOCK,
                          auto_run_put_off(&R, 1, AUTO_PUT_OFF_NIGHT, 3, true, NULL, F.mono));
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_BAD_ARG, put_off(1, AUTO_PUT_OFF_HOUR, 0, true));
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_BAD_ARG, put_off(1, 3, 3, true));
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_NO_WARNING, put_off(9, AUTO_PUT_OFF_HOUR, 3, true));
    TEST_ASSERT_EQUAL_UINT(warns, F.n_warn);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_put_off);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, R.e[0].arm.state);
    /* the hour, with the same code */
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_HOUR, 3, false));
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_NO_WARNING, put_off(1, AUTO_PUT_OFF_NIGHT, 3, true));
    TEST_ASSERT_EQUAL_UINT(1u, F.n_put_off);
}

/* ---- one warning at a time (DESIGN.md "Arming" 4) ---- */

/* DESIGN.md 3.4, AUT-012: while an automation warns or is put off its new triggers start
 * nothing; once it has armed, the next trigger warns again. */
static void test_one_warning_at_a_time(void)
{
    arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 3);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_warn);
    const uint64_t due = last_warn()->arm_at_ms;
    advance(40u * SEC); /* past the zone's 30 s cooldown */
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_warn);
    TEST_ASSERT_EQUAL_UINT64(due, R.e[0].arm.arm_at);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_HOUR, 3, false));
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_HOUR, last_warn()->state); /* still put off */
    TEST_ASSERT_EQUAL_UINT(2u, F.n_warn);
    advance(due + 55u * MIN_ - F.mono); /* D20: the warning again 5 minutes before */
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    advance(due + 60u * MIN_ - F.mono);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT64(due + 60u * MIN_, F.arm_mono[0]);
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state); /* a new one */
    TEST_ASSERT_EQUAL_UINT8(0u, last_warn()->put_off_by);
}

/* DESIGN.md 3.4: turning it off, deleting it or changing its Arm ends its warning or its hold at
 * once (the warning port told); nothing arms afterwards. */
static void test_off_delete_and_edit_end_it(void)
{
    auto_rule_t *r = arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 2);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    r->off = true;
    load();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT16(1u, last_warn()->uid);
    advance(5u * MIN_);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);

    r->off = false;
    load();
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_HOUR, 3, false));
    NR = 0; /* deleted while put off */
    load();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT16(1u, last_warn()->uid);
    advance(70u * MIN_);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);

    /* an edit to another level ends it; one that changes nothing it announced keeps it */
    r = arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 2);
    load();
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    const unsigned warns = F.n_warn;
    r->made_by = 3u;
    load();
    TEST_ASSERT_EQUAL_UINT(warns, F.n_warn);
    r->then[0].arm_level = ALARM_LEVEL_STAY;
    load();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    advance(5u * MIN_);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
}

/* ---- at arm time (AUT-014) ---- */

/* AUT-014, D29 (decided 2026-10-05): "Bypass open zones" sends ALARM_ARM_BYPASS_FAULTED only
 * while the installer option is on - read live with the maker at arm time - and a stored one
 * with the option off arms without bypassing (never "can't run"). */
static void test_bypass_only_with_the_option(void)
{
    auto_rule_t *r = arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 1);
    r->then[0].bypass_open = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(r, &ENV));
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(1u, last_warn()->bypass); /* D28: the card says so */
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARM_BYPASS_FAULTED, F.arm_flags);

    /* the installer turned it off since the env was read: no bypass */
    F.option = false;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(0u, F.arm_flags);

    /* the env says off: the rule still runs, without bypassing; maker_ok isn't asked */
    F.option = true;
    ENV.less_secure = false;
    load();
    const unsigned asked = F.n_maker;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(0u, last_warn()->bypass);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(3u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(0u, F.arm_flags);
    TEST_ASSERT_EQUAL_UINT(asked, F.n_maker);

    /* without "Bypass open zones": never the flag */
    ENV.less_secure = true;
    r->then[0].bypass_open = 0u;
    load();
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(4u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(0u, F.arm_flags);
}

/* AUT-014, D29 (decided 2026-10-05): the bypass flag goes only with a maker who still may and
 * the option on now; a missing maker check is "no" - the rule still arms, without bypassing. */
static void test_bypass_needs_the_maker(void)
{
    auto_rule_t *r = arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 1);
    r->then[0].bypass_open = 1u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    F.maker = false; /* the maker was deleted or demoted: the engine refuses; never the flag */
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(0u, F.arm_flags);

    /* no maker check at all */
    F.maker = true;
    const auto_ports_t p = {.ctx = &F, .device_act = port_act, .device_kind = port_kind,
                            .arm = port_arm, .warning = port_warning, .failed = port_failed,
                            .maker_ok = NULL, .disarm = port_disarm, .put_off = port_put_off};
    auto_run_init(&R, &p, F.mono);
    load();
    step();
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(0u, F.arm_flags);
}

/* D20, DESIGN.md 3.4: after an hour's put-off the warning comes back for a whole 5 minutes even
 * when the tick that starts it is late, and the put-off note goes with it. */
static void test_rewarning_is_whole(void)
{
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 26), 5);
    TEST_ASSERT_EQUAL_INT(AUTO_POFF_OK, put_off(1, AUTO_PUT_OFF_HOUR, 3, false));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 23, 24), 10);
    /* the next tick comes 3 minutes late: 23:27, three minutes before the planned 23:30 */
    advance(3u * MIN_);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT8(0u, last_warn()->put_off_by);
    const uint64_t started = F.warn_mono[F.n_warn - 1u];
    walk(local(TZ_EST5EDT, 2026, 6, 10, 23, 31), 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm); /* not at 23:30: the warning had 3 minutes */
    walk(local(TZ_EST5EDT, 2026, 6, 10, 23, 33), 10);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_TRUE(F.arm_mono[0] >= started + 5u * MIN_);
}

/* DESIGN.md "Arming" 5: someone arms the partition during the warning - its banner and chime
 * end at once, noted "already armed"; nothing is sent at the arm time. */
static void test_armed_during_the_warning(void)
{
    arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5)->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 26), 5);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    occ(AUTO_TRIG_ARMED, 2, 0); /* another partition: nothing */
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    occ(AUTO_TRIG_ARMED, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ALREADY, status_of(1).result);
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 32), 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
}

/* DESIGN.md 3.4: an edit of its arming during a time rule's warning ends the warning, and the
 * arm minute then says "no warning" instead of passing silently. */
static void test_edit_during_warning_says_so(void)
{
    auto_rule_t *r = arm_rule(1, AUTO_TRIG_TIME, 1, ALARM_LEVEL_NIGHT, 5);
    r->when.minute = 22u * 60u + 30u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 22, 20));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 26), 5);
    r->then[0].arm_level = ALARM_LEVEL_STAY;
    load();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    walk(local(TZ_EST5EDT, 2026, 6, 10, 22, 31), 10);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NO_WARNING, status_of(1).result);
}

/* AUT-014, DESIGN.md "Arming" 5-6: what the panel answers is the status - already armed (noted
 * only), not ready with its zone, refused with its reason; no failed event for any of them (the
 * engine's ARM_FAILED is the report). */
static void test_arm_answers(void)
{
    arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 1);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    F.arm_res = ALARM_ERR_BAD_STATE;
    F.arm_already = true;
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ALREADY, status_of(1).result);

    F.arm_res = ALARM_ERR_NOT_READY;
    F.arm_already = false;
    F.arm_zone = 7u;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    auto_status_t st = status_of(1);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NOT_READY, st.result);
    TEST_ASSERT_EQUAL_UINT16(7u, st.zone);

    F.arm_res = ALARM_ERR_NOT_AUTHORIZED;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    st = status_of(1);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ARM_REFUSED, st.result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_NOT_AUTHORIZED, st.alarm_res);
    TEST_ASSERT_EQUAL_UINT16(0u, st.zone);
    TEST_ASSERT_EQUAL_UINT(3u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed);
    TEST_ASSERT_EQUAL_INT(AUTO_TRY_ARM, auto_run_try(&R, 1, F.mono)); /* never tried */

    /* no arm port: refused as unavailable */
    R.port.arm = NULL;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    st = status_of(1);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ARM_REFUSED, st.result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_UNAVAILABLE, st.alarm_res);

    /* phase A's env: nothing runs */
    R.port.arm = port_arm;
    ENV.arm_supported = false;
    load();
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(3u, F.n_arm);
}

/* ---- the disarm (DESIGN.md "Disarming" 1-3, AUT-011) ---- */

/* AUT-011, D23: a 10:00 Disarm disarms at its time with no warning, as its maker; already
 * disarmed is only noted; a refusal is the status alone; the installer option and the maker are
 * read live like a less secure device action (AUT-015). */
static void test_disarm(void)
{
    disarm_rule(1, AUTO_TRIG_TIME, 1)->when.minute = 10u * 60u;
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 9, 58));
    walk(local(TZ_EST5EDT, 2026, 6, 10, 9, 59) + 55, 5);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_disarm);
    walk(local(TZ_EST5EDT, 2026, 6, 10, 10, 0) + 5, 5);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_disarm);
    TEST_ASSERT_EQUAL_UINT8(1u, F.disarm_p);
    TEST_ASSERT_EQUAL_UINT8(2u, F.disarm_maker);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_DONE, status_of(1).result);

    setUp();
    disarm_rule(1, AUTO_TRIG_ZONE_OPENED, 1);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 9, 0));
    F.disarm_already = true;
    F.disarm_res = ALARM_ERR_BAD_STATE;
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ALREADY, status_of(1).result);
    F.disarm_already = false;
    F.disarm_res = ALARM_ERR_NOT_AUTHORIZED; /* e.g. its alarm sounds (D22) */
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_ARM_REFUSED, status_of(1).result);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_NOT_AUTHORIZED, status_of(1).alarm_res);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_failed); /* "Disarming" 3: no event */
    TEST_ASSERT_EQUAL_UINT(2u, F.n_disarm);
    /* the option off now: never sent */
    F.option = false;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_LESS_SECURE_OFF, status_of(1).result);
    /* the maker is no longer a master or installer: never sent */
    F.option = true;
    F.maker = false;
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_MAKER_GONE, status_of(1).result);
    /* the env's option off: never sent, maker_ok not asked */
    F.maker = true;
    ENV.less_secure = false;
    load();
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_LESS_SECURE_OFF, status_of(1).result);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_disarm);
}

/* PL04 / AUT-010: an arm or a disarm due while its automation's edit has answered but the
 * runner hasn't the new rules waits at the gate: turned off, it never goes (the warning ends with
 * the rules); an edit that changes nothing it does lets it go once they come. */
static void test_revoked_arm_and_disarm_wait(void)
{
    auto_rule_t *r = arm_rule(1, AUTO_TRIG_ZONE_OPENED, 1, ALARM_LEVEL_AWAY, 2);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 21, 0));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    r->off = true;
    auto_revoke_add(&F.revoked, 1); /* the edit answered */
    advance(2u * MIN_ + SEC);
    advance(SEC);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT64(F.mono, auto_run_next_deadline(&R, F.mono)); /* tried again at once */
    auto_revoke_clear(&F.revoked); /* the task copies the rules */
    load();
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_NONE, last_warn()->state);
    advance(5u * MIN_);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);

    /* an edit that changes nothing it does (another maker): armed once the rules come */
    r->off = false;
    load();
    advance(40u * SEC);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(AUTO_WARN_ON, last_warn()->state);
    r->made_by = 3u;
    auto_revoke_add(&F.revoked, 1);
    advance(2u * MIN_ + SEC);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_arm);
    auto_revoke_clear(&F.revoked);
    load();
    advance(SEC);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(3u, F.arm_maker);

    /* a disarm: turned off before the wake, never sent */
    setUp();
    disarm_rule(1, AUTO_TRIG_ZONE_OPENED, 1);
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 9, 0));
    RULES[0].off = true;
    auto_revoke_add(&F.revoked, 1);
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    advance(SEC);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_disarm);
    auto_revoke_clear(&F.revoked);
    load();
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_disarm);
    TEST_ASSERT_EQUAL_UINT8(AUTO_RES_NONE, status_of(1).result);
    TEST_ASSERT_EQUAL_UINT(F.n_begin, F.n_end);
}

/* ---- loops (AUT-007) ---- */

/* AUT-007: 10 s between two automatic arms or disarms of one partition (the later one waits),
 * and an ARMED or DISARMED an automation caused starts device actions (one hop) but never
 * another automation's arm or disarm. */
static void test_partition_gap_and_one_hop(void)
{
    disarm_rule(1, AUTO_TRIG_ZONE_OPENED, 1);
    disarm_rule(2, AUTO_TRIG_ZONE_OPENED, 1);
    /* 3: after partition 1 is disarmed, arm partition 2; 4: and turn a light off */
    arm_rule(3, AUTO_TRIG_DISARMED, 2, ALARM_LEVEL_AWAY, 1)->when.partitions = 0x01u;
    auto_rule_t *light = &RULES[NR++];
    memset(light, 0, sizeof *light);
    light->uid = 4u;
    light->n = 1u;
    light->when.kind = AUTO_TRIG_DISARMED;
    light->then[0].kind = AUTO_ACT_DEVICE;
    light->then[0].source = DSRC_HA;
    light->then[0].dev_kind = DEV_LIGHT;
    light->then[0].dev_action = ACT_OFF;
    light->then[0].value = -1;
    strcpy(light->then[0].ref, "light.hall");
    /* 5: after partition 2 is armed, disarm partition 1 (another partition: allowed) */
    disarm_rule(5, AUTO_TRIG_ARMED, 1)->when.partitions = 0x02u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&RULES[4], &ENV));
    load();
    begin(local(TZ_EST5EDT, 2026, 6, 10, 9, 0));
    occ(AUTO_TRIG_ZONE_OPENED, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_disarm);
    TEST_ASSERT_EQUAL_UINT64(F.mono + 10u * SEC, auto_run_next_deadline(&R, F.mono));
    advance(9u * SEC);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_disarm);
    advance(SEC);
    TEST_ASSERT_EQUAL_UINT(2u, F.n_disarm);
    TEST_ASSERT_EQUAL_UINT64(F.disarm_mono[0] + 10u * SEC, F.disarm_mono[1]);

    /* the DISARMED that disarm made: the light goes off, partition 2 doesn't arm */
    occ(AUTO_TRIG_DISARMED, 1, 0);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_act);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    /* one the event marks as an automation's: the same */
    advance(20u * SEC);
    occ(AUTO_TRIG_DISARMED, 1, 1);
    TEST_ASSERT_EQUAL_UINT(0u, F.n_warn);
    /* a person's disarm: partition 2's arming warns */
    advance(20u * SEC);
    occ(AUTO_TRIG_DISARMED, 1, 0);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_warn);
    advance(MIN_);
    TEST_ASSERT_EQUAL_UINT(1u, F.n_arm);
    TEST_ASSERT_EQUAL_UINT8(2u, F.arm_p);
    /* the ARMED that arm made doesn't disarm partition 1; a person's arming of 2 does */
    const unsigned disarms = F.n_disarm;
    occ(AUTO_TRIG_ARMED, 2, 0);
    TEST_ASSERT_EQUAL_UINT(disarms, F.n_disarm);
    advance(20u * SEC);
    occ(AUTO_TRIG_ARMED, 2, 0);
    TEST_ASSERT_EQUAL_UINT(disarms + 1u, F.n_disarm);
}

/* ---- the validator's phase C rules (AUT-011, AUT-014 to AUT-016, D24, D25, D29) ---- */

static void test_validator(void)
{
    /* AUT-016: a Disarm after an armed trigger of its own partition, or of any, is refused */
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.made_by = 2u;
    r.n = 1u;
    r.when.kind = AUTO_TRIG_ARMED;
    r.then[0].kind = AUTO_ACT_DISARM;
    r.then[0].value = -1;
    r.then[0].partition = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNDOES_ARMING, auto_rule_check(&r, &ENV));
    r.when.partitions = 0x01u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNDOES_ARMING, auto_rule_check(&r, &ENV));
    r.when.partitions = 0x03u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNDOES_ARMING, auto_rule_check(&r, &ENV));
    r.when.partitions = 0x02u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    /* a disarmed or time trigger: fine */
    r.when.kind = AUTO_TRIG_DISARMED;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    /* AUT-011: arming and disarming only where the firmware runs them */
    auto_env_t a = ENV;
    a.arm_supported = false;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNSUPPORTED, auto_rule_check(&r, &a));

    /* decided 2026-10-05: an unlock or an open after an armed trigger needs the confirm; a
     * time of day or a disarmed trigger none */
    memset(&r, 0, sizeof r);
    r.made_by = 2u;
    r.n = 1u;
    r.when.kind = AUTO_TRIG_ARMED;
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = DEV_LOCK;
    r.then[0].dev_action = ACT_UNLOCK;
    r.then[0].value = -1;
    strcpy(r.then[0].ref, "lock.front_door");
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NEEDS_CONFIRM, auto_rule_check(&r, &ENV));
    r.then[0].confirmed = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    r.then[0].confirmed = 0u;
    r.when.kind = AUTO_TRIG_DISARMED;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    r.when.kind = AUTO_TRIG_TIME;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    r.then[0].dev_kind = DEV_COVER;
    r.then[0].dev_action = ACT_OPEN;
    r.when.kind = AUTO_TRIG_ARMED;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NEEDS_CONFIRM, auto_rule_check(&r, &ENV));
    r.then[0].dev_action = ACT_CLOSE; /* not less secure: none */
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));

    /* D29: "Bypass open zones" only on an Arm, only while the option is on */
    memset(&r, 0, sizeof r);
    r.made_by = 2u;
    r.n = 1u;
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 1350u;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].value = -1;
    r.then[0].partition = 1u;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5u;
    r.then[0].bypass_open = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    a = ENV;
    a.less_secure = false;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &a));
    r.then[0].bypass_open = 0u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &a)); /* arming itself isn't less secure */
    r.then[0].bypass_open = 2u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &ENV));
    r.then[0].kind = AUTO_ACT_DISARM;
    r.then[0].arm_level = 0u;
    r.then[0].warn_min = 0u;
    r.then[0].bypass_open = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &ENV));
    r.then[0].bypass_open = 0u;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &ENV));
    memset(&r.then[0], 0, sizeof r.then[0]);
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = DEV_LIGHT;
    r.then[0].dev_action = ACT_OFF;
    r.then[0].value = -1;
    strcpy(r.then[0].ref, "light.hall");
    r.then[0].bypass_open = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &ENV));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_whole_warning);
    RUN_TEST(test_clock_jump_keeps_the_warning);
    RUN_TEST(test_no_whole_warning_no_arm);
    RUN_TEST(test_warning_before_midnight);
    RUN_TEST(test_put_off_an_hour_twice);
    RUN_TEST(test_not_tonight_until_six_across_dst);
    RUN_TEST(test_not_today_until_midnight_across_dst);
    RUN_TEST(test_put_off_refusals);
    RUN_TEST(test_one_warning_at_a_time);
    RUN_TEST(test_off_delete_and_edit_end_it);
    RUN_TEST(test_bypass_only_with_the_option);
    RUN_TEST(test_bypass_needs_the_maker);
    RUN_TEST(test_rewarning_is_whole);
    RUN_TEST(test_armed_during_the_warning);
    RUN_TEST(test_edit_during_warning_says_so);
    RUN_TEST(test_arm_answers);
    RUN_TEST(test_disarm);
    RUN_TEST(test_revoked_arm_and_disarm_wait);
    RUN_TEST(test_partition_gap_and_one_hop);
    RUN_TEST(test_validator);
    return UNITY_END();
}
