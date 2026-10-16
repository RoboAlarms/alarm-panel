/*
 * device_service.c - home_control on the panel (device_service.h, docs/features/24): one lock
 * around the registry and its device sources. The panel's own Z-Wave outputs (dsrc_zw,
 * docs/features/41 DESIGN section 4 "Z-Wave (phase B)") are registered first and read from
 * zwave_service once a second by device_service_tick; their commands are posted to the zwave
 * task, whose answer comes back through zw_done. Home Assistant's entities (dsrc_ha) follow;
 * ha_link feeds catalog, states and results in and sends the calls out from its own task.
 *
 * Lock order: zwave_service takes the zone lock and never calls in here while holding it; this
 * file never holds the device lock while calling zwave_service (except the post, which takes no
 * lock and never blocks).
 */
#include "sdkconfig.h"
#include "alarm_mem.h"

#if CONFIG_ALARM_HOME_CONTROL

#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "home_control/dsrc_ha.h"
#include "rtc_clock/clock_keeper.h"
#if CONFIG_ALARM_ZSRC_ZWAVE
#include "alarm_proto/proto_zw.h"
#include "home_control/dsrc_zw.h"
#include "zwave_service.h"
#endif

#include "alarm_service.h"
#include "device_service.h"

static const char *TAG = "devices";

static struct {
    SemaphoreHandle_t lock;
    hc_t *hc;      /* ~35 KB with its staging area: PSRAM, like the zone hub */
    dsrc_ha_t *ha; /* ~15 KB of device rows; NULL without Home Assistant (FEATURE_HA off) */
#if CONFIG_ALARM_ZSRC_ZWAVE
    dsrc_zw_t *zw;     /* ~8 KB: the panel's own outputs, at most DSRC_ZW_MAX */
    struct zw_feed *zwf; /* ~4 KB: device_service_tick's copies of zwave_service's list */
    /* when the command being acted on was asked for (alarm_service_now() ms); written and read
     * under the lock, within one act call (zw_sender reads it as received_ms, ZWO_STALE_MS) */
    uint64_t act_issued_ms;
#endif
    bool ready;    /* every part above exists: nothing here is touched before that (R07) */
    device_service_send_fn send;
    void *send_ctx;
    device_service_result_fn on_result; /* read under the lock; set under it once the service
                                         * runs (before that nothing can report) */
    void *on_result_ctx;
    /* AUT-006 (device_service_source_settled): when ha_link came up and when the first catalog
     * since then began, in alarm_service_now() ms; 0 = not since the last drop. Atomics: written
     * by ha_link's task, read by the automation task. */
    uint64_t ha_up_ms;
    uint64_t ha_catalog_ms;
    /* device_service_seq: moved under the device lock (hc's port) or right after an act, read
     * lock-free by anyone. Atomics. */
    uint32_t seq;
} S;

/* Long enough for a fresh link's catalog and first states to land (DESIGN.md section 3). */
#define SOURCE_SETTLE_MS UINT64_C(30000)

/* 0 is "not yet": a time of exactly 0 ms after boot is stored as 1. */
static uint64_t stamp_now(void)
{
    const uint64_t now = (uint64_t)alarm_service_now();
    return now != 0u ? now : 1u;
}

/* The lock alone never meant the registry was usable: a start that ran out of memory left a
 * lock with a null registry behind it, and every call below dereferenced it. One ready flag,
 * published last and read first, is what the callers check now. */
static bool ready(void)
{
    return __atomic_load_n(&S.ready, __ATOMIC_ACQUIRE);
}

static void lock(void)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(S.lock);
}

/* device_service_seq moves: release, so a reader that sees the new number and then lists (under
 * the lock) sees at least what moved it. */
static void seq_bump(void)
{
    (void)__atomic_add_fetch(&S.seq, 1u, __ATOMIC_RELEASE);
}

/* ---- hc's port ------------------------------------------------------------------------------ */

/* A source reported a change (its list, a state, its link), or a send its source refused at once
 * cleared the failure a row showed (hc_act, inside device_service_act or _act_auto): the registry
 * re-reads it under this same lock, so a list taken after the counter moved has it. The screen
 * still polls once a second; the panel link's device builds follow the counter (docs/features/11,
 * BL-216). */
static void port_changed(void *ctx)
{
    (void)ctx;
    seq_bump();
}

/* Runs under the device lock (hc calls it from inside device_service_*): the listener only
 * copies and enqueues (device_service.h). Every end of an action moves the counter, hc_tick's
 * "timeout" included, which changes the row without a port.changed. */
static void port_result(void *ctx, uint32_t request_id, const char *ref, bool ok,
                        const char *error)
{
    (void)ctx;
    seq_bump();
    if (!ok) {
        ESP_LOGW(TAG, "%s: %s", ref, error[0] != '\0' ? error : "failed");
    }
    if (S.on_result != NULL) {
        S.on_result(S.on_result_ctx, request_id, ref, ok, error);
    }
}

/* ---- the sender dsrc_ha uses (ha_link queues it for its task) ------------------------------- */

static bool sender(void *ctx, const char *entity, const char *action, bool has_value,
                   int32_t value, const char *mode, uint32_t request_id)
{
    (void)ctx;
    return S.send != NULL &&
           S.send(S.send_ctx, entity, action, has_value, value, mode, request_id);
}

/* ---- the panel's own Z-Wave outputs (dsrc_zw) ------------------------------------------------ */

#if CONFIG_ALARM_ZSRC_ZWAVE
/* The list as zwave_service gave it last time and this time: compared, so the registry hears only
 * of a change. Used by device_service_tick alone (the health task), outside the lock. */
struct zw_feed {
    proto_zw_device_t now[DSRC_ZW_MAX];
    proto_zw_device_t last[DSRC_ZW_MAX];
    uint32_t act_now[DSRC_ZW_MAX]; /* each one's action wait (a garage's whole move), 0: default */
    uint32_t act_last[DSRC_ZW_MAX];
    size_t n_last;
    bool fed;    /* the registry has had a list */
    bool up;     /* the link as last told */
};

/* The zwave task's answer to a posted command: exactly once per request (zwave_service.h). It
 * holds no zwave_service lock here, so taking the device lock can't deadlock. */
static void zw_done(void *ctx, uint32_t request_id, bool ok, const char *error)
{
    (void)ctx;
    lock();
    dsrc_zw_result(S.zw, request_id, ok, error);
    unlock();
}

/* dsrc_zw's sender, called under the device lock from hc_act / hc_act_auto: posts and returns at
 * once (zwave_service_ha_command would block up to 2 s, holding this lock). received_ms is when
 * the act was asked (S.act_issued_ms), so ZWO_STALE_MS still refuses a command the zwave task
 * reaches too late (docs/features/41 DESIGN section 4's failure table). */
static bool zw_sender(void *ctx, const char *key, int level, uint32_t request_id)
{
    (void)ctx;
    const uint64_t issued = S.act_issued_ms != 0u ? S.act_issued_ms : stamp_now();
    return zwave_service_output_post(key, level, issued, request_id, zw_done, NULL);
}

/* Once a second from device_service_tick: zwave_service's list and link read first (they take the
 * zone lock), then the registry fed under the device lock only when something changed. */
static void zw_feed(void)
{
    struct zw_feed *f = S.zwf;
    const bool up = zwave_service_link_up();
    const size_t n = zwave_service_ha_devices(f->now, DSRC_ZW_MAX);
    zwave_service_action_ms(f->now, n, f->act_now); /* a garage's travel time can change alone */
    const bool same_list = f->fed && n == f->n_last &&
                           (n == 0u || (memcmp(f->now, f->last, n * sizeof(f->now[0])) == 0 &&
                                        memcmp(f->act_now, f->act_last,
                                               n * sizeof(f->act_now[0])) == 0));
    if (same_list && up == f->up) {
        return;
    }
    lock();
    if (up != f->up || !f->fed) {
        dsrc_zw_link(S.zw, up); /* before the list: an output is online only with the link */
    }
    dsrc_zw_begin(S.zw);
    for (size_t i = 0; i < n; i++) {
        const proto_zw_device_t *d = &f->now[i];
        dsrc_zw_add(S.zw, d->key, d->kind, d->name, d->level, d->online, d->garage,
                    f->act_now[i]);
    }
    dsrc_zw_end(S.zw);
    hc_refresh(S.hc);
    unlock();
    memcpy(f->last, f->now, n * sizeof(f->now[0]));
    memcpy(f->act_last, f->act_now, n * sizeof(f->act_now[0]));
    f->n_last = n;
    f->up = up;
    f->fed = true;
}
#endif

/* ---- public --------------------------------------------------------------------------------- */

/* Both objects are large and go to slow memory when there is any, internal RAM otherwise. */
static void *big_calloc(size_t size)
{
    return alarm_mem_prefer_slow_calloc(1, size);
}

/* R07: everything is built privately and the globals are published only once all of it
 * succeeded, so a failure leaves nothing allocated and nothing half-initialised behind and can
 * simply be tried again. */
esp_err_t device_service_start(void)
{
    ESP_RETURN_ON_FALSE(!ready() && S.lock == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "already started");
    SemaphoreHandle_t lk = xSemaphoreCreateMutex();
    hc_t *hc = big_calloc(sizeof(*hc));
    bool ok = lk != NULL && hc != NULL;
#if CONFIG_ALARM_FEATURE_HA
    dsrc_ha_t *ha = big_calloc(sizeof(*ha));
    ok = ok && ha != NULL;
#else
    dsrc_ha_t *ha = NULL; /* no Home Assistant link in this build: no source to feed */
#endif
#if CONFIG_ALARM_ZSRC_ZWAVE
    dsrc_zw_t *zw = big_calloc(sizeof(*zw));
    struct zw_feed *zwf = big_calloc(sizeof(*zwf));
    ok = ok && zw != NULL && zwf != NULL;
#endif
    if (!ok) {
        if (lk != NULL) {
            vSemaphoreDelete(lk);
        }
        heap_caps_free(hc);
        heap_caps_free(ha);
#if CONFIG_ALARM_ZSRC_ZWAVE
        heap_caps_free(zw);
        heap_caps_free(zwf);
#endif
        ESP_LOGE(TAG, "not enough memory for the device registry");
        return ESP_ERR_NO_MEM;
    }
    const hc_port_t port = {.ctx = NULL, .changed = port_changed, .result = port_result};
    hc_init(hc, &port);
#if CONFIG_ALARM_ZSRC_ZWAVE
    /* First: hc_refresh fills HC_MAX_DEVICES in registration order, so the panel's own outputs
     * are always listed and Home Assistant's long list takes what is left (dsrc_zw.h). */
    dsrc_zw_init(zw, hc_sink(hc));
    dsrc_zw_set_sender(zw, zw_sender, NULL);
    hc_register(hc, dsrc_zw_ops(), zw);
#endif
    if (ha != NULL) {
        dsrc_ha_init(ha, hc_sink(hc));
        dsrc_ha_set_sender(ha, sender, NULL);
        hc_register(hc, dsrc_ha_ops(), ha);
    }

    S.lock = lk;
    S.hc = hc;
    S.ha = ha;
#if CONFIG_ALARM_ZSRC_ZWAVE
    S.zw = zw;
    S.zwf = zwf;
#endif
    __atomic_store_n(&S.ready, true, __ATOMIC_RELEASE); /* everything above is in place first */
    ESP_LOGI(TAG, "device registry running (%u devices at most)", (unsigned)HC_MAX_DEVICES);
    return ESP_OK;
}

/* HCTL-012: a device command whose reply is simply lost, with no disconnect to notice, would
 * otherwise leave that device busy for ever. Called about once a second by the panel's health
 * task; hc_tick only expires deadlines, so it is cheap and never calls the engine. */
void device_service_tick(uint64_t now_ms)
{
    if (!ready()) {
        return;
    }
#if CONFIG_ALARM_ZSRC_ZWAVE
    zw_feed(); /* outside the lock: it reads zwave_service first */
#endif
    lock();
    hc_tick(S.hc, now_ms);
    unlock();
}

void device_service_set_sender(device_service_send_fn fn, void *ctx)
{
    S.send = fn;
    S.send_ctx = ctx;
}

/* The panel's clock in Unix seconds, 0 while it isn't set (CLOCK_EPOCH_MIN_S): a link drop's
 * "Offline since" (dsrc_ha_link_at, HCTL-006). */
static uint32_t epoch_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t s = (int64_t)tv.tv_sec;
    return s >= CLOCK_EPOCH_MIN_S && s <= (int64_t)UINT32_MAX ? (uint32_t)s : 0u;
}

void device_service_ha_link(bool up)
{
    if (!ready() || S.ha == NULL) {
        return;
    }
    if (!up) {
        __atomic_store_n(&S.ha_up_ms, UINT64_C(0), __ATOMIC_RELAXED);
        __atomic_store_n(&S.ha_catalog_ms, UINT64_C(0), __ATOMIC_RELAXED);
    } else if (__atomic_load_n(&S.ha_up_ms, __ATOMIC_RELAXED) == 0u) {
        __atomic_store_n(&S.ha_up_ms, stamp_now(), __ATOMIC_RELAXED);
    }
    lock();
    dsrc_ha_link_at(S.ha, up, epoch_s());
    hc_refresh(S.hc);
    unlock();
}

void device_service_ha_catalog_begin(void)
{
    if (!ready() || S.ha == NULL) {
        return;
    }
    if (__atomic_load_n(&S.ha_up_ms, __ATOMIC_RELAXED) != 0u &&
        __atomic_load_n(&S.ha_catalog_ms, __ATOMIC_RELAXED) == 0u) {
        __atomic_store_n(&S.ha_catalog_ms, stamp_now(), __ATOMIC_RELAXED);
    }
    lock();
    dsrc_ha_catalog_begin(S.ha);
    unlock();
}

bool device_service_source_settled(uint8_t source, uint64_t now_ms)
{
    if (!ready() || source != (uint8_t)DSRC_HA) {
        return false; /* Z-Wave is phase B; Zigbee isn't a device source yet */
    }
    const uint64_t up = __atomic_load_n(&S.ha_up_ms, __ATOMIC_RELAXED);
    const uint64_t cat = __atomic_load_n(&S.ha_catalog_ms, __ATOMIC_RELAXED);
    return up != 0u && cat != 0u && now_ms >= up + SOURCE_SETTLE_MS &&
           now_ms >= cat + SOURCE_SETTLE_MS;
}

void device_service_ha_catalog_add(const char *entity_id, const char *name, const char *area,
                                   const char *domain, const char *state,
                                   const dev_values_t *values)
{
    if (!ready() || S.ha == NULL) {
        return;
    }
    lock();
    dsrc_ha_catalog_add(S.ha, entity_id, name, area, domain, state, values);
    hc_refresh(S.hc);
    unlock();
}

void device_service_ha_state(const char *entity_id, const char *state, bool available,
                             const dev_values_t *values)
{
    if (!ready() || S.ha == NULL) {
        return;
    }
    lock();
    dsrc_ha_state(S.ha, entity_id, state, available, values);
    hc_refresh(S.hc);
    unlock();
}

void device_service_ha_result(uint32_t request_id, bool ok, const char *error)
{
    if (!ready() || S.ha == NULL) {
        return; /* no Home Assistant source: nothing of its could be pending */
    }
    lock();
    dsrc_ha_result(S.ha, request_id, ok, error); /* HCTL-012: it knows which words are unknown */
    unlock();
}

size_t device_service_list(hc_device_t *out, size_t max)
{
    if (!ready()) {
        return 0u;
    }
    lock();
    const hc_device_t *rows[HC_MAX_DEVICES];
    size_t n = hc_list(S.hc, rows, HC_MAX_DEVICES);
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = *rows[i];
    }
    unlock();
    return n;
}

bool device_service_get(const char *ref, hc_device_t *out)
{
    if (!ready() || ref == NULL) {
        return false;
    }
    lock();
    const hc_device_t *d = hc_get(S.hc, ref);
    if (d != NULL) {
        *out = *d;
    }
    unlock();
    return d != NULL;
}

hc_result_t device_service_act(const char *ref, dev_action_t action, int32_t value,
                               const char *mode)
{
    if (!ready()) {
        return HC_ERR_OFFLINE;
    }
    const uint64_t asked = stamp_now(); /* before the lock: a wait for it counts as late */
    lock();
#if CONFIG_ALARM_ZSRC_ZWAVE
    S.act_issued_ms = asked;
#else
    (void)asked;
#endif
    const hc_result_t r = hc_act(S.hc, ref, action, value, mode);
#if CONFIG_ALARM_ZSRC_ZWAVE
    S.act_issued_ms = 0u;
#endif
    unlock();
    if (r == HC_OK) {
        seq_bump(); /* the row shows it pending: the one change hc_act tells no port of */
    }
    return r;
}

hc_result_t device_service_act_auto(const char *ref, dev_action_t action, int32_t value,
                                    const char *mode, bool less_secure, uint32_t *request_id)
{
    if (request_id != NULL) {
        *request_id = 0;
    }
    if (!ready()) {
        return HC_ERR_OFFLINE;
    }
    /* received_ms for the Z-Wave source: the moment the automation sends (auto_run calls this
     * on the automation task at the time it decides to send). Not the trigger's time: a send the
     * runner holds on purpose (the device gap, the cap, "Turn it off after") isn't a late one. */
    const uint64_t asked = stamp_now();
    lock();
#if CONFIG_ALARM_ZSRC_ZWAVE
    S.act_issued_ms = asked;
#else
    (void)asked;
#endif
    const hc_result_t r = hc_act_auto(S.hc, ref, action, value, mode, less_secure, request_id);
#if CONFIG_ALARM_ZSRC_ZWAVE
    S.act_issued_ms = 0u;
#endif
    unlock();
    if (r == HC_OK) {
        seq_bump(); /* as device_service_act */
    }
    return r;
}

uint32_t device_service_seq(void)
{
    return __atomic_load_n(&S.seq, __ATOMIC_ACQUIRE);
}

/* The touchscreen's own judge until BL-216 (ui_port_esp.c's dev_act), moved here unchanged so a
 * link keypad's action is judged by the same code (docs/features/11 "Device actions"). */
ds_person_t device_service_person_act(const alarm_source_t *src, uint8_t partition,
                                      const char *ref, uint8_t action, int32_t value,
                                      uint8_t mode_bit, const char *code, alarm_result_t *why)
{
    if (why != NULL) {
        *why = ALARM_OK;
    }
    hc_device_t d;
    if (src == NULL || ref == NULL || !device_service_get(ref, &d) ||
        d.info.source == (uint8_t)DSRC_ZWAVE ||
        !hc_action_valid((dev_kind_t)d.info.kind, (dev_action_t)action)) {
        return DS_PERSON_REFUSED;
    }
    /* HCTL-005, judged here and not by the screen: an unlock or an opening needs a code the
     * engine accepts, a wrong one counting toward the keypad's lockout like any other. */
    if (hc_needs_code((dev_kind_t)d.info.kind, (dev_action_t)action, value)) {
        if (code == NULL || code[0] == '\0') {
            return DS_PERSON_NEED_CODE;
        }
        uint8_t user = 0;
        alarm_authority_t authority = ALARM_AUTH_NONE;
        alarm_result_t r = alarm_service_authenticate_as(src, code, partition, &user, &authority);
        /* Until the owner rules otherwise (BL-045), the codes that may disarm at will may
         * unlock: an arm-only or a guest code may not. A duress code unlocks as asked, without
         * the silent duress report disarming sends (decided in BL-045). */
        if (r == ALARM_OK && (authority == ALARM_AUTH_ARM_ONLY || authority == ALARM_AUTH_GUEST)) {
            r = ALARM_ERR_NOT_AUTHORIZED;
        }
        if (r != ALARM_OK) {
            if (why != NULL) {
                *why = r;
            }
            return DS_PERSON_CODE_REFUSED;
        }
        /* The event with who did it, and src, is HCTL-007's (docs/features/24 Tasks): not logged
         * yet. */
        (void)user;
    }
    /* A thermostat's mode comes as one DEV_MODE_* bit: the source gets its word. */
    const char *mode = NULL;
    if (action == (uint8_t)ACT_SET_MODE) {
        mode = hc_mode_word(mode_bit);
        if (mode == NULL) {
            return DS_PERSON_REFUSED;
        }
    }
    switch (device_service_act(ref, (dev_action_t)action, value, mode)) {
    case HC_OK: return DS_PERSON_OK;
    case HC_ERR_BUSY: return DS_PERSON_BUSY;
    case HC_ERR_OFFLINE: return DS_PERSON_UNREACHABLE;
    default: return DS_PERSON_REFUSED;
    }
}

void device_service_set_result_listener(device_service_result_fn fn, void *ctx)
{
    if (!ready()) {
        /* nothing can report before the registry exists: keep it for when it does */
        S.on_result = fn;
        S.on_result_ctx = ctx;
        return;
    }
    lock();
    S.on_result = fn;
    S.on_result_ctx = ctx;
    unlock();
}

/* ---- console -------------------------------------------------------------------------------- */

#if CONFIG_ALARM_SERVICE_CONSOLE
#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"

static int cmd_devices(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "act") == 0) {
        /* devices act <ref> <action-number> [value] - a service tool, the UI has the words. A
         * mode change (ACT_SET_MODE) takes the mode's word as its value, the words the screen's
         * mode keys send: off, heat, cool, auto (dsrc_ha sends "heat_cool" for auto where the
         * thermostat has only that). */
        const dev_action_t a = (dev_action_t)atoi(argv[3]);
        const char *mode = NULL;
        int32_t value = argc >= 5 ? atoi(argv[4]) : -1;
        if (a == ACT_SET_MODE) {
            static const char *const MODES[] = {"off", "heat", "cool", "auto"};
            for (size_t i = 0; argc >= 5 && i < sizeof(MODES) / sizeof(MODES[0]); i++) {
                if (strcmp(argv[4], MODES[i]) == 0) {
                    mode = MODES[i];
                }
            }
            if (mode == NULL) {
                printf("usage: devices act <ref> %d off|heat|cool|auto\n", (int)ACT_SET_MODE);
                return 1;
            }
            value = 0;
        }
        const hc_result_t r = device_service_act(argv[2], a, value, mode);
        printf("%s\n", r == HC_OK ? "sent (the result follows in the log)" : "refused");
        return r == HC_OK ? 0 : 1;
    }
    hc_device_t *rows = alarm_mem_slow_malloc(HC_MAX_DEVICES * sizeof(*rows));
    if (rows == NULL) {
        printf("not enough PSRAM for device list\n");
        return 1;
    }
    const size_t n = device_service_list(rows, HC_MAX_DEVICES);
    printf("%u device%s\n", (unsigned)n, n == 1u ? "" : "s");
    for (size_t i = 0; i < n; i++) {
        /* After the columns: what it can do when its source says (" can:-" none of them), what
         * a thermostat is doing, and since when (Unix s), each only when known (BL-171, BL-174). */
        const dev_values_t *v = &rows[i].info.values;
        char more[96] = "";
        size_t m = 0;
        if ((v->caps & DEV_CAP_KNOWN) != 0u) {
            static const struct {
                uint8_t cap;
                const char *word;
            } CAN[] = {{DEV_CAP_STOP, "stop"},
                       {DEV_CAP_POSITION, "position"},
                       {DEV_CAP_SPEED, "speed"},
                       {DEV_CAP_DIM, "dim"}};
            m += (size_t)snprintf(more + m, sizeof(more) - m, " can:");
            bool any = false;
            for (size_t k = 0; k < sizeof(CAN) / sizeof(CAN[0]) && m < sizeof(more); k++) {
                if ((v->caps & CAN[k].cap) != 0u) {
                    m += (size_t)snprintf(more + m, sizeof(more) - m, "%s%s", any ? "," : "",
                                          CAN[k].word);
                    any = true;
                }
            }
            if (!any && m < sizeof(more)) {
                m += (size_t)snprintf(more + m, sizeof(more) - m, "-");
            }
        }
        static const char *const HVAC[] = {"",     "off", "heating",    "cooling",   "drying",
                                           "idle", "fan", "preheating", "defrosting"};
        if (v->hvac_action != 0u && v->hvac_action < sizeof(HVAC) / sizeof(HVAC[0]) &&
            m < sizeof(more)) {
            m += (size_t)snprintf(more + m, sizeof(more) - m, " hvac:%s", HVAC[v->hvac_action]);
        }
        if (v->since != 0u && m < sizeof(more)) {
            m += (size_t)snprintf(more + m, sizeof(more) - m, " since:%lu",
                                  (unsigned long)v->since);
        }
        printf("  %-40s %-12s %-10s%s%s%s\n", rows[i].info.ref, rows[i].info.name,
               rows[i].info.state[0] != '\0' ? rows[i].info.state : "-",
               rows[i].info.online ? "" : " (offline)", rows[i].pending != 0u ? " (busy)" : "",
               more);
    }
    heap_caps_free(rows);
    return 0;
}

esp_err_t device_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "devices",
        .help = "Devices (Z-Wave, Home Assistant): list, or act <ref> <action#> [value] "
                "(action 11, set mode: the value is off, heat, cool or auto)",
        .hint = "[act <ref> <action#> [value|mode]]",
        .func = cmd_devices,
    };
    return esp_console_cmd_register(&cmd);
}
#else
esp_err_t device_service_console_register(void)
{
    return ESP_OK;
}
#endif

#else /* !CONFIG_ALARM_HOME_CONTROL: the panel has no Devices screen (HCTL-010) */

#include "device_service.h"

esp_err_t device_service_start(void)
{
    return ESP_OK;
}
void device_service_tick(uint64_t now_ms)
{
    (void)now_ms;
}
void device_service_ha_link(bool up)
{
    (void)up;
}
void device_service_ha_catalog_begin(void) {}
bool device_service_source_settled(uint8_t source, uint64_t now_ms)
{
    (void)source;
    (void)now_ms;
    return false;
}
void device_service_ha_catalog_add(const char *entity_id, const char *name, const char *area,
                                   const char *domain, const char *state,
                                   const dev_values_t *values)
{
    (void)entity_id;
    (void)name;
    (void)area;
    (void)domain;
    (void)state;
    (void)values;
}
void device_service_ha_state(const char *entity_id, const char *state, bool available,
                             const dev_values_t *values)
{
    (void)entity_id;
    (void)state;
    (void)available;
    (void)values;
}
void device_service_ha_result(uint32_t request_id, bool ok, const char *error)
{
    (void)request_id;
    (void)ok;
    (void)error;
}
void device_service_set_sender(device_service_send_fn fn, void *ctx)
{
    (void)fn;
    (void)ctx;
}
uint32_t device_service_seq(void)
{
    return 0u;
}
ds_person_t device_service_person_act(const alarm_source_t *src, uint8_t partition,
                                      const char *ref, uint8_t action, int32_t value,
                                      uint8_t mode_bit, const char *code, alarm_result_t *why)
{
    (void)src;
    (void)partition;
    (void)ref;
    (void)action;
    (void)value;
    (void)mode_bit;
    (void)code;
    if (why != NULL) {
        *why = ALARM_OK;
    }
    return DS_PERSON_REFUSED; /* no registry: no such device */
}
esp_err_t device_service_console_register(void)
{
    return ESP_OK;
}

#endif /* CONFIG_ALARM_HOME_CONTROL */
