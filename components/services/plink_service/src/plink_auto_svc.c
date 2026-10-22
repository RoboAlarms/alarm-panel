/*
 * plink_auto_svc.c - an automation's arming warnings on keypads (docs/features/11 LINK-056,
 * docs/features/41 AUT-012/AUT-013).
 *
 * On the master: the worker copies the automations' warning table (automation_service_warnings)
 * once a second, and the link task hands it to the manager (plink_master_auto_warn), which sends
 * it to the keypads that take it. The link task never waits for the automations' lock (LINK-027).
 * A keypad's put-off runs on the worker like any command (run_job: the code checked, then
 * automation_service_put_off).
 *
 * On a keypad: the link task publishes the master's newest table; the screen's backend reads it
 * (auto_warnings: Home's card and the status bar, as on the alarm panel's own screen) and sends a
 * put-off as a command (auto_put_off); the keypad chimes once a minute while its partition's
 * warning is on, in the background as the master does (D22). A master without a screen - where
 * the keypads are the only screens in the house - needs this most.
 */
#include "sdkconfig.h"
#if CONFIG_ALARM_LINK

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "plink_internal.h"
#include "sounder.h"
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
#include "alarm_service.h"
#include "automation_service.h"
#endif

#define CHIME_EVERY_MS 60000u /* as the master's (automation_service.c CHIME_EVERY_MS) */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- the master's side ----------------------------------------------------------------------- */

static plink_auto_warn_t s_table;  /* under s_mux: the newest copy, arm_in_ms as of s_taken */
static uint64_t s_taken;           /* link_now() when copied (0: none yet) */
static uint64_t s_fed_at;          /* link task: the last plink_master_auto_warn */

uint16_t masvc_caps(void)
{
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    return PLINK_CAP_AUTO;
#else
    return 0u;
#endif
}

void masvc_worker(void)
{
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    static auto_warning_t w[PLINK_AUTO_MAX];
    const size_t n = automation_service_warnings(w, PLINK_AUTO_MAX);
    const uint64_t runner_now = (uint64_t)alarm_service_now(); /* the runner's own clock */
    plink_auto_warn_t t;
    memset(&t, 0, sizeof t);
    t.count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        plink_auto_rec_t *r = &t.rec[i];
        r->uid = w[i].uid;
        r->partition = w[i].partition;
        r->level = w[i].level;
        r->state = w[i].state;
        r->warn_min = w[i].warn_min;
        r->bypass = w[i].bypass;
        r->put_off_by = w[i].put_off_by;
        r->arm_at_epoch_ms = w[i].arm_at_epoch_ms;
        const uint64_t in = w[i].arm_at_ms > runner_now ? w[i].arm_at_ms - runner_now : 0u;
        r->arm_in_ms = in > UINT32_MAX ? UINT32_MAX : (uint32_t)in;
        r->until_day = w[i].until_day;
        r->until_minute = w[i].until_minute;
    }
    const uint64_t at = link_now();
    portENTER_CRITICAL(&s_mux);
    s_table = t;
    s_taken = at;
    portEXIT_CRITICAL(&s_mux);
#endif
}

void masvc_poll(plink_master_t *m, uint64_t now)
{
    if (masvc_caps() == 0u || now - s_fed_at < 1000u) {
        return;
    }
    plink_auto_warn_t t;
    uint64_t taken;
    portENTER_CRITICAL(&s_mux);
    t = s_table;
    taken = s_taken;
    portEXIT_CRITICAL(&s_mux);
    if (taken == 0u) {
        return; /* the worker hasn't copied it yet */
    }
    const uint64_t age = now > taken ? now - taken : 0u;
    for (size_t i = 0; i < t.count; i++) {
        t.rec[i].arm_in_ms = t.rec[i].arm_in_ms > age ? (uint32_t)(t.rec[i].arm_in_ms - age) : 0u;
    }
    plink_master_auto_warn(m, now, &t);
    s_fed_at = now;
}

/* The worker: a keypad's put-off (PLINK_OP_PUT_OFF): the code checked as the panel's own screen
 * checks it (ALARM_CMD_CODE_CHECK on the warning's partition, from the keypad source), then the
 * put-off by that code's user. */
alarm_result_t masvc_put_off(const plink_cmd_t *c, const alarm_source_t *src, plink_result_t *res)
{
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    alarm_cmd_t check = c->cmd;
    check.type = ALARM_CMD_CODE_CHECK;
    alarm_cmd_result_t d;
    memset(&d, 0, sizeof d);
    const alarm_result_t r = alarm_service_command(&check, src, &d);
    plink_wipe(&check, sizeof check);
    res->user = d.user;
    res->may_disarm = d.may_disarm ? 1u : 0u;
    if (r == ALARM_OK) {
        res->not_ready_zone =
            (uint16_t)automation_service_put_off(c->cmd.zone, c->cmd.walk_op, d.user, d.may_disarm);
    }
    return r;
#else
    (void)c;
    (void)src;
    (void)res;
    return ALARM_ERR_UNAVAILABLE;
#endif
}

/* ---- the keypad's side ----------------------------------------------------------------------- */

static plink_auto_warn_t s_seen;   /* under s_mux: the master's newest table ... */
static bool s_have;                /* ... when there is one */
static uint64_t s_seen_at;         /* ... as of then (link_now) */
static uint64_t s_chime_next;      /* link task */
static bool s_chiming;

uint16_t kasvc_caps(void)
{
#if CONFIG_ALARM_FEATURE_UI
    return PLINK_CAP_AUTO; /* Home's card shows them; a keypad without a screen has nowhere */
#else
    return 0u;
#endif
}

void kasvc_poll(plink_keypad_t *k, uint64_t now, uint8_t partition)
{
    plink_auto_warn_t t;
    const bool have = plink_keypad_auto_warn(k, now, &t);
    portENTER_CRITICAL(&s_mux);
    s_seen = t;
    s_have = have;
    s_seen_at = now;
    portEXIT_CRITICAL(&s_mux);
    /* The chime, once a minute while this keypad's partition's warning is on (AUT-012). */
    bool on = false;
    for (size_t i = 0; have && i < t.count; i++) {
        on = on || (t.rec[i].partition == partition && t.rec[i].state == AUTO_WARN_ON);
    }
    if (!on) {
        s_chiming = false;
        return;
    }
    if (!s_chiming || now >= s_chime_next) {
        sounder_play_background(ALARM_SOUND_CHIME);
        s_chiming = true;
        s_chime_next = now + CHIME_EVERY_MS;
    }
}

size_t kasvc_warnings(auto_warning_t *out, size_t max)
{
    plink_auto_warn_t t;
    bool have;
    uint64_t at;
    portENTER_CRITICAL(&s_mux);
    t = s_seen;
    have = s_have;
    at = s_seen_at;
    portEXIT_CRITICAL(&s_mux);
    if (!have || out == NULL) {
        return 0u;
    }
    const uint64_t now = link_now();
    const uint64_t age = now > at ? now - at : 0u;
    size_t n = 0;
    for (size_t i = 0; i < t.count && n < max; i++, n++) {
        const plink_auto_rec_t *r = &t.rec[i];
        auto_warning_t *w = &out[n];
        memset(w, 0, sizeof(*w));
        w->uid = r->uid;
        w->partition = r->partition;
        w->level = r->level;
        w->state = r->state;
        w->warn_min = r->warn_min;
        w->bypass = r->bypass;
        w->put_off_by = r->put_off_by;
        w->arm_at_epoch_ms = r->arm_at_epoch_ms;
        w->arm_at_ms = now + (r->arm_in_ms > age ? r->arm_in_ms - age : 0u);
        w->until_day = r->until_day;
        w->until_minute = r->until_minute;
    }
    return n;
}

#endif /* CONFIG_ALARM_LINK */
