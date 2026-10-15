/* automation_service - the automations on the panel (docs/features/41 DESIGN.md sections 5
 * "Service and threading" and 6 "Stored format", AUT-003..AUT-010): auto_store over NVS, the
 * readers and edits the touchscreen and the Panel Manager call, the engine listener, and the
 * "auto" task that runs the rules (auto_watch -> auto_run) through device_service.
 *
 * Who touches what:
 *   alarm task      the listener: s_watch, s_occ_out; posts to s_occ_q (0 timeout), notifies.
 *   "auto" task     s_run, s_clock, s_rules, s_zones, s_env: every auto_run_* call happens here
 *                   (s_zones and s_env are filled on the alarm task, through
 *                   automation_service_env's alarm_service_call, while the task waits for it;
 *                   so are the maker and the installer option before a less secure send).
 *   any edit        the save mutex, then s_save_zones (filled on the alarm task the same way),
 *                   s_store and NVS; then s_auto_lock to publish.
 *   any reader      s_auto_lock only: the published view, the statuses, the warnings.
 * Lock order: save mutex, then s_send_lock, then s_auto_lock; never the reverse. No port
 * (device_service_*, alarm_service_*) is ever called with s_auto_lock held; the save mutex is held
 * across a save's alarm_service_call (the env), which is safe: the alarm task never takes it.
 *
 * Revocation (PL04): an edit marks its automation in s_revoked in the same s_auto_lock step that
 * publishes it, holding s_send_lock; the task holds s_send_lock from the runner's send_begin
 * (which reads s_revoked) to its send_end, across one device action, arm or disarm. So once an
 * edit has answered, no send of the automation's old version starts: it waits until reload hands
 * the runner the new rules (and empties s_revoked in the step that copies them). The task reloads
 * before it runs the wake's occurrences, so one queued before an edit runs against the new rules.
 * s_send_lock is held across those ports, which never wait for an edit: the device registry's own
 * lock, or the alarm task, which takes neither the save mutex nor s_send_lock.
 *
 * Arming and disarming (phase C, DESIGN.md section 3 "Arming" and "Disarming"): the arm and
 * disarm ports run on the "auto" task through alarm_service_command_as, which posts the command
 * to the alarm task and waits for it there (alarm_service_call): the engine is only ever touched
 * on the alarm task, nothing here writes flash from this task's PSRAM stack (the alarm task logs
 * and stores), and no lock of ours is held meanwhile. The warning port copies each warning into
 * s_warn (under s_auto_lock) for the readers; the task chimes once a minute while one is ON,
 * through sounder_play_background, which plays only while nothing else sounds and gives way to
 * every other sound (the mixer's rule, D22). A put-off is a request answered by the task, as
 * Try it now is. */
#include "automation_service.h"

#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_AUTOMATIONS

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "alarm/alarm_strings.h"
#include "automation/auto_clock.h"
#include "automation/auto_watch.h"
#include "device_service.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sounder.h"

static const char *TAG = "automations";

#define AUTO_TASK_STACK    6144u  /* bytes, in PSRAM: the task never writes flash or NVS */
#define AUTO_TASK_PRIO     4u
#define OCC_QUEUE_LEN      32u    /* the listener's occurrences waiting for the task */
#define RES_QUEUE_LEN      16u    /* device results waiting for the task */
#define TRY_QUEUE_LEN      2u
#define TRY_WAIT_MS        100u
#define WAKE_TIME_RULE_MS  1000u  /* while any time rule exists (auto_clock_step's contract) */
#define WAKE_IDLE_MS       60000u
#define WAKE_MIN_MS        10u    /* a deadline already due still yields this long */
#define SETTLE_DEFAULT_MS  60000u /* alarm_config_defaults' power_up_settle_s, until start reads it */
#define CHIME_EVERY_MS     60000u /* the arming warning's chime (DESIGN.md section 3 "Arming" 2) */

/* ---- the store and its published view ------------------------------------------------------ */

/* 16 rules and the BAD records' bytes (about 15 KB): slow memory, task-only (never an ISR's).
 * Touched only under the save mutex. */
static EXT_RAM_BSS_ATTR auto_store_t s_store;
static StaticSemaphore_t s_save_buf;
static SemaphoreHandle_t s_save_lock;  /* one edit at a time, across its validate + NVS write */

/* What readers see, swapped in under s_auto_lock after each load and edit. */
static StaticSemaphore_t s_auto_buf;
static SemaphoreHandle_t s_auto_lock;  /* short copies only: never held across NVS or a port */
static EXT_RAM_BSS_ATTR auto_rule_t s_view[AUTO_MAX];
static size_t s_view_n;
static uint16_t s_view_gen;
static bool s_view_ro = true;
static bool s_rules_changed;           /* the task copies the rules again */
/* PL04: edited since the task last copied the rules (under s_auto_lock; the task empties it when
 * it copies them). Each edit adds to it holding s_send_lock, which the task holds across a send. */
static auto_revoke_t s_revoked;
static StaticSemaphore_t s_send_buf;
static SemaphoreHandle_t s_send_lock;
typedef struct {
    uint16_t uid;
    auto_status_t st;
} uid_status_t;
static EXT_RAM_BSS_ATTR uid_status_t s_status[AUTO_MAX];
static size_t s_status_n;
static bool s_started;                 /* atomic: the store loaded, the view is published */
/* The arming warnings and holds (phase C): one per automation, state != NONE, under
 * s_auto_lock. Written only by the warning port (the task); readers copy out. */
static EXT_RAM_BSS_ATTR auto_warning_t s_warn[AUTO_MAX];
static size_t s_warn_n;

/* ---- the listener's side (the alarm task's) ------------------------------------------------- */

static auto_watch_t s_watch;           /* alarm task only (about 90 bytes) */
static EXT_RAM_BSS_ATTR auto_occ_t s_occ_out[OCC_QUEUE_LEN]; /* alarm task only: one call's */
static StaticQueue_t s_occ_qbuf;
static EXT_RAM_BSS_ATTR uint8_t s_occ_store[OCC_QUEUE_LEN * sizeof(auto_occ_t)];
static QueueHandle_t s_occ_q;
static alarm_ms_t s_watch_t0;          /* when the observer was set up (its settle counts from it) */
static uint32_t s_watch_settle_ms = SETTLE_DEFAULT_MS;
static bool s_listener_ok;             /* alarm_service_add_listener took it */
/* Atomics the listener writes and the task and the console read. */
static uint32_t s_occ_dropped;         /* occurrences the queue had no room for */
static uint32_t s_watch_missed;        /* the observer's own count (auto_watch_t.missed) */
static uint32_t s_config_seq;          /* the latest snapshot's */
static bool s_clock_ok;                /* the latest snapshot had no ALARM_TRBL_CLOCK_NOT_SET */
static bool s_heard;                   /* a snapshot arrived */
static bool s_locked;                  /* atomic: the latest snapshot is locked (AUT-010) */
static TaskHandle_t s_task;            /* NULL until the task runs */

/* ---- the task's side ------------------------------------------------------------------------ */

typedef struct {
    uint32_t request_id;
    uint8_t ok;
    uint8_t why; /* auto_why_t */
} dev_res_t;

/* A request the task answers - Try it now or a put-off: seq names it (the answer carries it
 * back), deadline is when its caller stops waiting (ticks). The task runs it only before the
 * deadline and only if it can claim it (s_try_pending), so a caller told "busy" never has it run
 * afterwards. */
typedef enum { REQ_TRY = 0, REQ_PUT_OFF } req_kind_t;

typedef struct {
    uint32_t seq;
    TickType_t deadline;
    uint16_t uid;
    uint8_t kind;       /* req_kind_t */
    uint8_t choice;     /* REQ_PUT_OFF: auto_put_off_choice_t */
    uint8_t user;       /* REQ_PUT_OFF: whose code */
    uint8_t may_disarm; /* REQ_PUT_OFF: that code may disarm the partition */
} try_req_t;

static EXT_RAM_BSS_ATTR auto_run_t s_run;          /* about 24 KB */
static EXT_RAM_BSS_ATTR auto_clock_t s_clock;
static EXT_RAM_BSS_ATTR auto_rule_t s_rules[AUTO_MAX];
static EXT_RAM_BSS_ATTR auto_zone_t s_zones[ALARM_MAX_ZONES];
static EXT_RAM_BSS_ATTR auto_env_t s_env; /* task only: the env the runner has (AUT-002, AUT-015) */
static EXT_RAM_BSS_ATTR auto_zone_t s_save_zones[ALARM_MAX_ZONES]; /* under the save mutex */
static EXT_RAM_BSS_ATTR StackType_t s_stack[AUTO_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_tcb;             /* FreeRTOS wants the TCB internal */
static StaticQueue_t s_res_qbuf, s_try_qbuf;
static uint8_t s_res_store[RES_QUEUE_LEN * sizeof(dev_res_t)];
static EXT_RAM_BSS_ATTR uint8_t s_try_store[TRY_QUEUE_LEN * sizeof(try_req_t)]; /* no ISR uses it */
static QueueHandle_t s_res_q, s_try_q;
static uint32_t s_res_dropped;         /* atomic */
static StaticSemaphore_t s_try_lock_buf, s_try_done_buf;
static SemaphoreHandle_t s_try_lock, s_try_done;
static uint32_t s_try_seq;             /* under s_try_lock */
/* atomic: the seq still waiting to run, 0 none. Whoever swaps it to 0 first decides: the task
 * (it runs) or the caller (it gave up). */
static EXT_RAM_BSS_ATTR uint32_t s_try_pending;
static uint32_t s_try_answer_seq;      /* atomic: which request s_try_answer answers */
static uint8_t s_try_answer;           /* auto_try_t or auto_put_off_t */
static size_t s_rules_n;               /* task only: how many of s_rules */
static bool s_any_time_rule;           /* task only */
static bool s_chiming;                 /* task only: a warning is ON and has chimed */
static uint64_t s_chime_next;          /* task only: the next chime is due then */

/* ---- the key-value port: the "automations" namespace ---- */

static auto_kv_t kv_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTO_STORE_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return AUTO_KV_ABSENT; /* nothing stored yet */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stored %s unreadable: NVS refused to open (%s)", key, esp_err_to_name(err));
        return AUTO_KV_ERROR;
    }
    size_t stored = 0u;
    auto_kv_t res;
    err = nvs_get_blob(h, key, NULL, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        res = AUTO_KV_ABSENT;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "stored %s unreadable (%s)", key, esp_err_to_name(err));
        res = AUTO_KV_ERROR;
    } else if (stored > cap) {
        ESP_LOGW(TAG, "stored %s is %u bytes, more than this firmware writes: newer, read-only",
                 key, (unsigned)stored);
        res = AUTO_KV_TOO_BIG;
    } else {
        size_t got = stored;
        err = nvs_get_blob(h, key, buf, &got);
        if (err == ESP_OK && got == stored) {
            *len = got;
            res = AUTO_KV_OK;
        } else {
            ESP_LOGE(TAG, "stored %s unreadable (%s)", key, esp_err_to_name(err));
            res = AUTO_KV_ERROR;
        }
    }
    nvs_close(h);
    return res;
}

static bool kv_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTO_STORE_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s not stored (%s)", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool kv_erase(void *ctx, const char *key)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(AUTO_STORE_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s not erased (%s)", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/* NVS's free entries, as zwave_service reads them; unreadable: not told (no limit) */
static size_t kv_free(void *ctx)
{
    (void)ctx;
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return AUTO_NVS_NOT_TOLD;
    }
    return st.available_entries;
}

static const auto_kv_port_t s_kv = {
    .ctx = NULL, .get = kv_get, .set = kv_set, .erase = kv_erase, .free_entries = kv_free};

/* ---- the locks ---- */

/* Both mutexes and the task's queues are static: created once, from whichever comes first of
 * automation_service_listener and automation_service_start (both on the start-up task). */
static void init_once(void)
{
    if (s_save_lock != NULL) {
        return;
    }
    s_save_lock = xSemaphoreCreateMutexStatic(&s_save_buf);
    s_auto_lock = xSemaphoreCreateMutexStatic(&s_auto_buf);
    s_send_lock = xSemaphoreCreateMutexStatic(&s_send_buf);
    s_try_lock = xSemaphoreCreateMutexStatic(&s_try_lock_buf);
    s_try_done = xSemaphoreCreateBinaryStatic(&s_try_done_buf);
    s_res_q = xQueueCreateStatic(RES_QUEUE_LEN, sizeof(dev_res_t), s_res_store, &s_res_qbuf);
    s_try_q = xQueueCreateStatic(TRY_QUEUE_LEN, sizeof(try_req_t), s_try_store, &s_try_qbuf);
}

static bool started(void)
{
    return __atomic_load_n(&s_started, __ATOMIC_ACQUIRE);
}

static void view_lock(void)
{
    xSemaphoreTake(s_auto_lock, portMAX_DELAY);
}

static void view_unlock(void)
{
    xSemaphoreGive(s_auto_lock);
}

static void notify_task(void)
{
    TaskHandle_t t = __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

/* Under the save mutex: swap what readers see for the store as it is now. changed: the rules
 * may differ (the task copies them again), and automation uid (0: every one) is revoked in the
 * same step (PL04): a send of its old version that had begun has ended when this returns (the
 * send lock), and none begins after it until the task has the new rules. */
static void publish(bool changed, uint16_t uid)
{
    if (changed) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
    }
    view_lock();
    s_view_n = auto_store_list(&s_store, s_view, AUTO_MAX);
    s_view_gen = s_store.gen;
    s_view_ro = s_store.read_only;
    if (changed) {
        s_rules_changed = true;
        auto_revoke_add(&s_revoked, uid);
    }
    view_unlock();
    if (changed) {
        xSemaphoreGive(s_send_lock);
        notify_task();
    }
}

/* An edit writes flash on the caller's task: never from a PSRAM stack (AGENTS.md "Rules for the
 * firmware", DESIGN.md R13). */
static bool stack_is_internal(void)
{
    uint8_t probe = 0u;
    if (esp_ptr_external_ram(&probe)) {
        ESP_LOGE(TAG, "edit refused: the calling task's stack is in PSRAM");
        return false;
    }
    return true;
}

/* ---- the engine listener (the alarm task: never blocks, takes no lock) ---- */

static void post_occurrences(size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (xQueueSend(s_occ_q, &s_occ_out[i], 0) != pdTRUE) {
            __atomic_fetch_add(&s_occ_dropped, 1u, __ATOMIC_RELAXED);
        }
    }
    __atomic_store_n(&s_watch_missed, s_watch.missed, __ATOMIC_RELAXED);
}

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    const size_t n = auto_watch_event(&s_watch, ev, s_occ_out, OCC_QUEUE_LEN);
    post_occurrences(n);
    if (n > 0u) {
        notify_task();
    }
}

static void on_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    const bool clock_ok = (snap->sys_troubles & (1u << (unsigned)ALARM_TRBL_CLOCK_NOT_SET)) == 0u;
    const bool clock_was = __atomic_exchange_n(&s_clock_ok, clock_ok, __ATOMIC_RELAXED);
    const uint32_t seq_was = __atomic_exchange_n(&s_config_seq, snap->config_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_locked, alarm_snapshot_locked(snap), __ATOMIC_RELAXED); /* AUT-010 */
    const bool first = !__atomic_exchange_n(&s_heard, true, __ATOMIC_RELEASE);
    const size_t n = auto_watch_snapshot(&s_watch, snap, s_occ_out, OCC_QUEUE_LEN);
    post_occurrences(n);
    if (n > 0u || first || clock_was != clock_ok || seq_was != snap->config_seq) {
        notify_task();
    }
}

static void on_zones_reset(void *ctx, const uint8_t *zones)
{
    (void)ctx;
    auto_watch_zones_reset(&s_watch, zones);
}

static const alarm_listener_t s_listener = {
    .ctx = NULL, .on_event = on_event, .on_snapshot = on_snapshot, .on_zones_reset = on_zones_reset};

/* Runs on the alarm task (alarm_service_call): the configured settle time, once start has read
 * it. A new observer takes its next snapshot as a fresh baseline (B1). */
static void resettle(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)e;
    (void)now;
    auto_watch_init(&s_watch, s_watch_t0, *(const uint32_t *)ctx);
}

/* ---- the ports (called by auto_run_* on the task; no lock of ours is held) ---- */

/* PL04, the revocation gate: s_send_lock from here to port_send_end, across the one send. */
static bool port_send_begin(void *ctx, uint16_t uid)
{
    (void)ctx;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    view_lock();
    const bool revoked = auto_revoke_has(&s_revoked, uid);
    view_unlock();
    return !revoked;
}

static void port_send_end(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(s_send_lock);
}

static int port_device_act(void *ctx, const auto_action_t *a, bool less_secure,
                           uint32_t *request_id)
{
    (void)ctx;
    return (int)device_service_act_auto(a->ref, (dev_action_t)a->dev_action, a->value,
                                        a->mode[0] != '\0' ? a->mode : NULL, less_secure,
                                        request_id);
}

typedef struct {
    uint8_t user;
    bool ok;
    bool option_on;
} maker_read_t;

/* Runs on the alarm task (alarm_service_call): the slot still holds a system master or
 * installer (panel_web's ACC_SYSTEM), and the installer option now (AUT-015 layer 4). */
static void read_maker(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    maker_read_t *m = ctx;
    const alarm_config_t *c = alarm_engine_config(e);
    m->option_on = c->opt.auto_less_secure;
    m->ok = false;
    if (m->user >= 1u && m->user <= ALARM_MAX_USERS) {
        const alarm_authority_t a = c->users[m->user - 1u].authority;
        m->ok = a == ALARM_AUTH_MASTER || a == ALARM_AUTH_INSTALLER;
    }
}

typedef struct {
    uint8_t user;
    bool installer;
} installer_read_t;

/* Runs on the alarm task (alarm_service_call): whether the saver's slot holds the installer
 * (BL-187, USR-001). */
static void read_installer(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    installer_read_t *w = ctx;
    const alarm_config_t *c = alarm_engine_config(e);
    w->installer = w->user >= 1u && w->user <= ALARM_MAX_USERS &&
                   c->users[w->user - 1u].authority == ALARM_AUTH_INSTALLER;
}

/* AUT-015, D23: read live before each less secure send - the maker and the installer option in
 * one call. A configuration change also moves config_seq, but the task reloads its env only at
 * its next wake; the runner asks here at send time, so a demoted or deleted maker,
 * or the option turned off, stops the very next send. Can't read: no, and the option off. */
static bool port_maker_ok(void *ctx, uint8_t user, bool *option_on)
{
    (void)ctx;
    maker_read_t m = {.user = user, .ok = false, .option_on = false};
    if (alarm_service_call(read_maker, &m) != ESP_OK) {
        *option_on = false;
        return false;
    }
    *option_on = m.option_on;
    return m.ok;
}

static bool port_source_settled(void *ctx, uint8_t source)
{
    (void)ctx;
    return device_service_source_settled(source, (uint64_t)alarm_service_now());
}

static bool port_device_kind(void *ctx, uint8_t source, const char *ref, uint8_t *kind)
{
    (void)ctx;
    hc_device_t dev; /* about 260 bytes on the task's stack */
    if (!device_service_get(ref, &dev) || dev.info.source != source) {
        return false;
    }
    *kind = dev.info.kind;
    return true;
}

/* AUT-009: the alarm task logs and reports it; this task waits for it, never writes flash. */
static void port_failed(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action, uint16_t zone)
{
    (void)ctx;
    const alarm_source_t src = {.kind = ALARM_SRC_AUTOMATION, .id = uid};
    const esp_err_t err =
        alarm_service_platform_event(ALARM_EV_AUTOMATION_FAILED, zone, reason, dev_action, &src, 0u);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "automation %u failed (%s), not recorded (%s)", (unsigned)uid,
                 alarm_auto_fail_name((alarm_auto_fail_t)reason), esp_err_to_name(err));
    }
}

/* ---- phase C: arming and disarming (DESIGN.md section 3 "Arming" 5, 6; "Disarming") ---- */

/* Runs on the alarm task (alarm_service_call): whether partition *ctx is disarmed now. */
static void read_disarmed(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    uint8_t *p = ctx;
    alarm_partition_status_t ps;
    alarm_engine_partition_status(e, *p, &ps);
    *p = ps.state == ALARM_STATE_DISARMED ? 1u : 0u;
}

/* Arm as the maker, source the automation, only from disarmed and out of installer mode
 * (ALARM_SVC_WHEN_DISARMED, checked on the alarm task in the same call): BAD_STATE while the
 * partition isn't disarmed is "armed already", nothing done and nothing logged; it never
 * re-arms at another level. BAD_STATE while it is disarmed (installer mode) is a refusal,
 * reported as one - never "armed already" over a disarmed house. "Bypass open
 * zones" (ALARM_ARM_BYPASS_FAULTED) goes only while the installer option "Allow less secure
 * automations" is on now (decided 2026-10-05): off, the arm goes without it and fails on an
 * open zone (NOT_READY, naming it), never "can't run" - the engine refuses the flag then. */
static alarm_result_t port_arm(void *ctx, uint16_t uid, uint8_t p, uint8_t level, uint8_t flags,
                               uint8_t maker, bool *already, uint16_t *zone)
{
    (void)ctx;
    *already = false;
    *zone = 0u;
    if ((flags & ALARM_ARM_BYPASS_FAULTED) != 0u) {
        maker_read_t m = {.user = maker, .ok = false, .option_on = false};
        if (alarm_service_call(read_maker, &m) != ESP_OK || !m.option_on) {
            flags &= (uint8_t)~ALARM_ARM_BYPASS_FAULTED;
        }
    }
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = p;
    cmd.level = (alarm_arm_level_t)level;
    cmd.arm_flags = flags;
    const alarm_source_t src = {.kind = ALARM_SRC_AUTOMATION, .id = uid};
    alarm_cmd_result_t res;
    const alarm_result_t r =
        alarm_service_command_as(&cmd, &src, maker, ALARM_SVC_WHEN_DISARMED, &res);
    if (r == ALARM_ERR_BAD_STATE) {
        uint8_t disarmed = p;
        *already = alarm_service_call(read_disarmed, &disarmed) == ESP_OK && disarmed == 0u;
    } else if (r == ALARM_ERR_NOT_READY) {
        *zone = res.not_ready_zone;
    }
    ESP_LOGI(TAG, "automation %u arms partition %u: %s", (unsigned)uid, (unsigned)p,
             *already ? "armed already" : alarm_result_name(r));
    return r;
}

/* Disarm as the maker: only while not disarmed (ALARM_SVC_WHEN_NOT_DISARMED); the engine's
 * automation guard refuses it while the option is off or after an alarm in this armed period
 * (D22). BAD_STATE is "disarmed already". */
static alarm_result_t port_disarm(void *ctx, uint16_t uid, uint8_t p, uint8_t maker, bool *already)
{
    (void)ctx;
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_DISARM;
    cmd.partition = p;
    const alarm_source_t src = {.kind = ALARM_SRC_AUTOMATION, .id = uid};
    const alarm_result_t r =
        alarm_service_command_as(&cmd, &src, maker, ALARM_SVC_WHEN_NOT_DISARMED, NULL);
    *already = r == ALARM_ERR_BAD_STATE;
    ESP_LOGI(TAG, "automation %u disarms partition %u: %s", (unsigned)uid, (unsigned)p,
             *already ? "disarmed already" : alarm_result_name(r));
    return r;
}

/* The warning mailbox: the whole state of one automation's warning, kept for the readers
 * (automation_service_warnings). A short copy under s_auto_lock; nothing else is woken. */
static void port_warning(void *ctx, const auto_warning_t *w)
{
    (void)ctx;
    bool lost = false;
    view_lock();
    size_t i = 0u;
    while (i < s_warn_n && s_warn[i].uid != w->uid) {
        i++;
    }
    if (w->state == (uint8_t)AUTO_WARN_NONE) {
        if (i < s_warn_n) {
            memmove(&s_warn[i], &s_warn[i + 1u], (s_warn_n - i - 1u) * sizeof(s_warn[0]));
            s_warn_n--;
        }
    } else if (i < s_warn_n) {
        s_warn[i] = *w;
    } else if (s_warn_n < AUTO_MAX) {
        s_warn[s_warn_n++] = *w;
    } else {
        lost = true; /* more than one per automation: can't happen */
    }
    view_unlock();
    if (lost) {
        ESP_LOGW(TAG, "automation %u's warning not shown: the table is full", (unsigned)w->uid);
    } else {
        ESP_LOGI(TAG, "automation %u: warning state %u, partition %u", (unsigned)w->uid,
                 (unsigned)w->state, (unsigned)w->partition);
    }
}

/* The put-off's event (AUT-013, D30): ALARM_EV_ARMING_PUT_OFF, logged and reported by the alarm
 * task. The runner says "night" for both Not tonight and Not today; the warning it has just
 * published (port_warning, before this call) says which. */
static void port_put_off(void *ctx, uint16_t uid, uint8_t partition, uint8_t user, uint8_t choice,
                         int64_t until_epoch_ms)
{
    (void)ctx;
    alarm_put_off_t c = ALARM_PUT_OFF_HOUR;
    if (choice != (uint8_t)AUTO_PUT_OFF_HOUR) {
        c = ALARM_PUT_OFF_NIGHT;
        view_lock();
        for (size_t i = 0; i < s_warn_n; i++) {
            if (s_warn[i].uid == uid && s_warn[i].state == (uint8_t)AUTO_WARN_DAY) {
                c = ALARM_PUT_OFF_DAY;
            }
        }
        view_unlock();
    }
    const esp_err_t err = alarm_service_arming_put_off(uid, partition, user, c, until_epoch_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "automation %u's put-off not logged (%s)", (unsigned)uid, esp_err_to_name(err));
    }
}

/* HCTL-012: runs UNDER THE DEVICE LOCK on whichever task drove the registry: map and enqueue
 * only. */
static void on_device_result(void *ctx, uint32_t request_id, const char *ref, bool ok,
                             const char *error)
{
    (void)ctx;
    (void)ref;
    uint8_t why = (uint8_t)AUTO_WHY_NO_ANSWER; /* "timeout", "offline", anything else */
    if (!ok && error != NULL &&
        (strcmp(error, "failed") == 0 || strcmp(error, "not_allowed") == 0 ||
         strcmp(error, "unavailable") == 0)) {
        why = (uint8_t)AUTO_WHY_REFUSED;
    }
    const dev_res_t r = {.request_id = request_id, .ok = ok ? 1u : 0u, .why = why};
    if (xQueueSend(s_res_q, &r, 0) != pdTRUE) {
        __atomic_fetch_add(&s_res_dropped, 1u, __ATOMIC_RELAXED);
    }
    notify_task();
}

/* The local-time port (DESIGN.md section 5): the panel's POSIX TZ is already applied (net_wifi). */
static void read_local(auto_local_t *lt)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const time_t t = tv.tv_sec;
    struct tm tm;
    memset(lt, 0, sizeof(*lt));
    if (localtime_r(&t, &tm) == NULL) {
        return;
    }
    const rtc_civil_t c = {.year = tm.tm_year + 1900,
                           .month = (uint8_t)(tm.tm_mon + 1),
                           .day = (uint8_t)tm.tm_mday,
                           .hour = (uint8_t)tm.tm_hour,
                           .minute = (uint8_t)tm.tm_min,
                           .second = (uint8_t)tm.tm_sec,
                           .weekday = 0u};
    (void)auto_local_from_civil(&c, (int64_t)tv.tv_sec,
                                __atomic_load_n(&s_clock_ok, __ATOMIC_RELAXED), lt);
}

/* ---- the task ---- */

/* What the task needs of the alarm configuration: the zones' type, partition and class, the
 * enabled partitions, the installer option "Allow less secure automations" and the power-up
 * settle time. */
typedef struct {
    auto_zone_t *zones;  /* ALARM_MAX_ZONES */
    uint8_t parts;
    bool less_secure;
    uint32_t settle_ms;
} cfg_read_t;

/* Runs on the alarm task (alarm_service_call): reads the engine's configuration in place, so no
 * copy of the whole alarm_config_t is kept. */
static void read_cfg(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    cfg_read_t *out = ctx;
    const alarm_config_t *c = alarm_engine_config(e);
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        out->zones[z].type = (uint8_t)c->zones[z].type;
        out->zones[z].partition = c->zones[z].partition;
        out->zones[z].dev_class = (uint8_t)c->zones[z].device_class;
    }
    out->parts = 0u;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (c->partitions[p].enabled) {
            out->parts |= (uint8_t)(1u << p);
        }
    }
    out->less_secure = c->opt.auto_less_secure;
    out->settle_ms = (uint32_t)c->opt.power_up_settle_s * 1000u;
}

bool automation_service_env(auto_env_t *env, auto_zone_t zones[ALARM_MAX_ZONES])
{
    if (env == NULL) {
        return false;
    }
    memset(env, 0, sizeof(*env));
    if (zones == NULL) {
        return false;
    }
    cfg_read_t rd = {.zones = zones};
    if (alarm_service_call(read_cfg, &rd) != ESP_OK) {
        return false;
    }
    env->partitions_enabled = rd.parts;
    env->arm_supported = true; /* phase C: arming and disarming (AUT-011..AUT-014) */
    env->zones = zones;
    env->less_secure = rd.less_secure; /* AUT-015 */
    return true;
}

/* The rules (when an edit changed them, or force) and the zones (when the configuration changed,
 * or force) into the runner. The copies are taken under s_auto_lock; auto_run_rules runs
 * outside it. */
static void reload(bool force, uint32_t *seen_seq)
{
    view_lock();
    const bool rules_changed = s_rules_changed || force;
    s_rules_changed = false;
    size_t n = 0u;
    if (rules_changed) {
        n = s_view_n;
        memcpy(s_rules, s_view, n * sizeof(s_rules[0]));
        auto_revoke_clear(&s_revoked); /* PL04: auto_run_rules below has these, before any send */
    }
    view_unlock();
    const uint32_t seq = __atomic_load_n(&s_config_seq, __ATOMIC_RELAXED);
    const bool config_changed = force || seq != *seen_seq;
    if (!rules_changed && !config_changed) {
        return;
    }
    if (rules_changed) {
        s_rules_n = n;
    } else {
        n = s_rules_n; /* the zones alone changed: the same rules again */
    }
    if (config_changed) {
        auto_env_t fresh;
        if (automation_service_env(&fresh, s_zones)) {
            *seen_seq = seq;
            s_env = fresh;
        } else {
            ESP_LOGW(TAG, "the alarm configuration couldn't be read: zones as before");
        }
    }
    s_env.zones = s_zones;
    s_env.arm_supported = true; /* phase C */
    auto_run_rules(&s_run, s_rules, n, &s_env);
    s_any_time_rule = false;
    for (size_t i = 0; i < n; i++) {
        if (s_rules[i].rec == (uint8_t)AUTO_REC_OK && !s_rules[i].off &&
            s_rules[i].when.kind == (uint8_t)AUTO_TRIG_TIME) {
            s_any_time_rule = true;
        }
    }
}

/* Each rule's status, copied out for the readers on other tasks. */
static void publish_status(void)
{
    uid_status_t st[AUTO_MAX];
    size_t n = 0u;
    for (size_t i = 0; i < s_rules_n && n < AUTO_MAX; i++) {
        if (s_rules[i].uid != 0u) { /* the runner ignores uid 0; an unknown uid reads all zero */
            st[n].uid = s_rules[i].uid;
            auto_run_status(&s_run, st[n].uid, &st[n].st);
            n++;
        }
    }
    view_lock();
    memcpy(s_status, st, n * sizeof(st[0]));
    s_status_n = n;
    view_unlock();
}

/* The arming warning's chime (DESIGN.md section 3 "Arming" 2): at a warning's start and then
 * once a minute while any warning is ON - never during a hold (HOUR, NIGHT, DAY). Posted to the
 * sounder as a background sound: the mixer plays it only while nothing else sounds and cuts it
 * the moment anything else is asked for (snd_play_background, D22); a minute's chime that found
 * another sound playing is skipped. s_warn is written only on this task, so it is read here
 * without the lock. */
static void chime(uint64_t now)
{
    bool any_on = false;
    for (size_t i = 0; i < s_warn_n && !any_on; i++) {
        any_on = s_warn[i].state == (uint8_t)AUTO_WARN_ON;
    }
    if (!any_on) {
        s_chiming = false;
        return;
    }
    if (!s_chiming || now >= s_chime_next) {
        sounder_play_background(ALARM_SOUND_CHIME);
        s_chiming = true;
        s_chime_next = now + CHIME_EVERY_MS;
    }
}

static void auto_task(void *arg)
{
    (void)arg;
    const auto_ports_t ports = {.ctx = NULL,
                                .device_act = port_device_act,
                                .source_settled = port_source_settled,
                                .device_kind = port_device_kind,
                                .arm = port_arm,
                                .warning = port_warning,
                                .failed = port_failed,
                                .maker_ok = port_maker_ok,
                                .disarm = port_disarm,
                                .put_off = port_put_off,
                                .send_begin = port_send_begin,
                                .send_end = port_send_end};
    auto_run_init(&s_run, &ports, (uint64_t)alarm_service_now());
    auto_clock_init(&s_clock);
    uint32_t seen_seq = 0u;
    reload(true, &seen_seq);
    uint32_t logged_dropped = 0u, logged_missed = 0u, logged_res = 0u;
    for (;;) {
        const uint64_t woke = (uint64_t)alarm_service_now();
        uint64_t now = woke;
        auto_local_t lt;
        /* 1. the rules after an edit, the zones after a configuration change - before the
         * occurrences (PL04): one queued before an edit answered runs against the new rules, so
         * an automation turned off, deleted or changed never sends its old version for it */
        reload(false, &seen_seq);
        /* reload may have waited for the alarm task: the rest uses the time after it */
        now = (uint64_t)alarm_service_now();
        read_local(&lt);
        /* 2. occurrences */
        auto_occ_t o;
        while (xQueueReceive(s_occ_q, &o, 0) == pdTRUE) {
            auto_run_occurrence(&s_run, &o, &lt, now);
        }
        /* 3. device results */
        dev_res_t r;
        while (xQueueReceive(s_res_q, &r, 0) == pdTRUE) {
            auto_run_result(&s_run, r.request_id, r.ok != 0u, r.why, now);
        }
        /* Try it now, against the rules just copied. A request whose caller has stopped waiting
         * (past its deadline, or already given up) is dropped without running. */
        try_req_t t;
        while (xQueueReceive(s_try_q, &t, 0) == pdTRUE) {
            if ((int32_t)(xTaskGetTickCount() - t.deadline) >= 0) {
                continue;
            }
            uint32_t want = t.seq;
            if (!__atomic_compare_exchange_n(&s_try_pending, &want, 0u, false, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                continue; /* the caller gave up first */
            }
            uint8_t answer;
            if (t.kind == (uint8_t)REQ_PUT_OFF) {
                answer = (uint8_t)auto_run_put_off(&s_run, t.uid, t.choice, t.user,
                                                   t.may_disarm != 0u, &lt, now);
            } else {
                answer = (uint8_t)auto_run_try(&s_run, t.uid, now);
            }
            __atomic_store_n(&s_try_answer, answer, __ATOMIC_RELAXED);
            __atomic_store_n(&s_try_answer_seq, t.seq, __ATOMIC_RELEASE);
            xSemaphoreGive(s_try_done);
        }
        /* 4. the clock */
        auto_span_t spans[2];
        const size_t ns = auto_clock_step(&s_clock, now, &lt, spans);
        auto_run_time(&s_run, &s_clock, &lt, spans, ns, now);
        /* 5. retries, holds, gaps, timeouts, off timers */
        auto_run_tick(&s_run, now);
        publish_status();
        chime(now);

        const uint32_t dropped = __atomic_load_n(&s_occ_dropped, __ATOMIC_RELAXED);
        const uint32_t missed = __atomic_load_n(&s_watch_missed, __ATOMIC_RELAXED);
        const uint32_t res = __atomic_load_n(&s_res_dropped, __ATOMIC_RELAXED);
        if (dropped != logged_dropped || missed != logged_missed || res != logged_res) {
            ESP_LOGW(TAG, "lost: %u occurrence(s) the queue had no room for, %u the observer "
                          "missed, %u device result(s)",
                     (unsigned)dropped, (unsigned)missed, (unsigned)res);
            logged_dropped = dropped;
            logged_missed = missed;
            logged_res = res;
        }

        const uint64_t after = (uint64_t)alarm_service_now();
        /* The cap counts from the wake, not from the end of its work. */
        /* a NIGHT or DAY hold waits for a local time: the clock at least once a minute */
        const uint64_t full =
            s_any_time_rule || auto_run_wants_clock(&s_run) ? WAKE_TIME_RULE_MS : WAKE_IDLE_MS;
        const uint64_t took = after > woke ? after - woke : 0u;
        const uint64_t cap = full > took ? full - took : 0u;
        const uint64_t due = auto_run_next_deadline(&s_run, after);
        uint64_t wait = cap;
        if (due != UINT64_MAX) {
            wait = due > after ? due - after : 0u;
            if (wait > cap) {
                wait = cap;
            }
        }
        if (s_chiming) {
            const uint64_t to_chime = s_chime_next > after ? s_chime_next - after : 0u;
            if (to_chime < wait) {
                wait = to_chime;
            }
        }
        if (wait < WAKE_MIN_MS) {
            wait = WAKE_MIN_MS;
        }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)wait);
        if (ticks == 0u) {
            ticks = 1u;
        }
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }
}

/* ---- API ---- */

const alarm_listener_t *automation_service_listener(void)
{
    init_once();
    if (s_occ_q == NULL) {
        s_occ_q = xQueueCreateStatic(OCC_QUEUE_LEN, sizeof(auto_occ_t), s_occ_store, &s_occ_qbuf);
        s_watch_t0 = alarm_service_now();
        auto_watch_init(&s_watch, s_watch_t0, s_watch_settle_ms);
    }
    return s_occ_q != NULL && s_res_q != NULL && s_try_q != NULL ? &s_listener : NULL;
}

void automation_service_listener_added(bool ok)
{
    s_listener_ok = ok;
    if (!ok) {
        ESP_LOGE(TAG, "no engine listener: automations stay off (the alarm runs on)");
    }
}

/* The task, once: the listener was added and the alarm runs (its configuration is readable). */
static void start_task(void)
{
    if (__atomic_load_n(&s_task, __ATOMIC_ACQUIRE) != NULL) {
        return;
    }
    if (!s_listener_ok || s_occ_q == NULL) {
        ESP_LOGW(TAG, "not running: no engine listener");
        return;
    }
    cfg_read_t rd = {.zones = s_zones}; /* the task isn't running yet: s_zones is free */
    if (alarm_service_call(read_cfg, &rd) != ESP_OK) {
        ESP_LOGE(TAG, "not running: the alarm isn't running");
        return;
    }
    const uint32_t settle_ms = rd.settle_ms;
    if (settle_ms != s_watch_settle_ms) {
        uint32_t arg = settle_ms;
        if (alarm_service_call(resettle, &arg) == ESP_OK) {
            s_watch_settle_ms = settle_ms;
        }
    }
    device_service_set_result_listener(on_device_result, NULL);
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(auto_task, "auto", AUTO_TASK_STACK, NULL,
                                                   AUTO_TASK_PRIO, s_stack, &s_tcb, tskNO_AFFINITY);
    if (t == NULL) {
        device_service_set_result_listener(NULL, NULL);
        ESP_LOGE(TAG, "not running: the task wouldn't start");
        return;
    }
    __atomic_store_n(&s_task, t, __ATOMIC_RELEASE);
    xTaskNotifyGive(t);
}

esp_err_t automation_service_start(void)
{
    init_once();
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    const bool editable = auto_store_load(&s_store, &s_kv);
    size_t n = 0u;
    for (unsigned i = 0; i < AUTO_MAX; i++) {
        n += s_store.used[i] ? 1u : 0u;
    }
    publish(true, 0u); /* the task copies them all at its start */
    __atomic_store_n(&s_started, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_save_lock);
    if (editable) {
        ESP_LOGI(TAG, "%u stored, gen %u", (unsigned)n, (unsigned)s_store.gen);
    } else {
        ESP_LOGW(TAG, "%u stored, read-only: edits are refused", (unsigned)n);
    }
    start_task();
    return ESP_OK;
}

size_t automation_service_list(auto_rule_t *out, size_t max)
{
    if (!started() || out == NULL) {
        return 0u;
    }
    view_lock();
    const size_t n = s_view_n < max ? s_view_n : max;
    memcpy(out, s_view, n * sizeof(out[0]));
    view_unlock();
    return n;
}

bool automation_service_get(uint16_t uid, auto_rule_t *out)
{
    if (!started() || out == NULL || uid == 0u) {
        return false;
    }
    bool found = false;
    view_lock();
    for (size_t i = 0; i < s_view_n && !found; i++) {
        if (s_view[i].uid == uid) {
            *out = s_view[i];
            found = true;
        }
    }
    view_unlock();
    return found;
}

uint16_t automation_service_gen(void)
{
    if (!started()) {
        return 0u;
    }
    view_lock();
    const uint16_t gen = s_view_gen;
    view_unlock();
    return gen;
}

bool automation_service_read_only(void)
{
    if (!started()) {
        return true;
    }
    view_lock();
    const bool ro = s_view_ro;
    view_unlock();
    return ro;
}

static EXT_RAM_BSS_ATTR alarm_snapshot_t s_edit_snap; /* save mutex: edits_locked's fallback */

/* AUT-010: nobody changes them while armed or in alarm. The latest snapshot the listener received
 * answers; before it has heard one (or if it was never added) the alarm service's own copy does,
 * as lang_packs.c reads it. panel_web's own 409 gate is the same test on its snapshot. Called
 * holding the save mutex (the fallback's buffer is the mutex's). */
static bool edits_locked(void)
{
    if (__atomic_load_n(&s_heard, __ATOMIC_ACQUIRE)) {
        return __atomic_load_n(&s_locked, __ATOMIC_RELAXED);
    }
    alarm_service_get_snapshot(&s_edit_snap);
    return alarm_snapshot_locked(&s_edit_snap);
}

/* The edits: the save mutex across the store's validate + NVS write, then publish. The view is
 * published after every attempt: a refused record write still moved gen (auto_store.h). The lock
 * is checked once the mutex is held, so an edit queued behind another's NVS write is checked when
 * its turn comes, not before. AUTO_SAVE_OK when begun; otherwise the refusal, mutex not held. */
static auto_save_t edit_begin(void)
{
    if (!started()) {
        return AUTO_SAVE_READ_ONLY;
    }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    if (edits_locked()) {
        xSemaphoreGive(s_save_lock);
        return AUTO_SAVE_LOCKED; /* AUT-010 */
    }
    return AUTO_SAVE_OK;
}

/* uid: the automation the edit changed (PL04: revoked until the task has the new rules); 0
 * when it can't be named (every one waits for them). */
static void edit_end(auto_save_t res, uint16_t uid)
{
    publish(res == AUTO_SAVE_OK, uid);
    xSemaphoreGive(s_save_lock);
}

/* The wall clock in epoch seconds while it is valid (the latest snapshot has no
 * ALARM_TRBL_CLOCK_NOT_SET); 0 otherwise: a record's changed_at (D30) is then stored unknown. */
static uint32_t now_epoch_s(void)
{
    if (!__atomic_load_n(&s_heard, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&s_clock_ok, __ATOMIC_RELAXED)) {
        return 0u;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec < AUTO_CLOCK_EPOCH_MIN || (int64_t)tv.tv_sec > (int64_t)UINT32_MAX) {
        return 0u;
    }
    return (uint32_t)tv.tv_sec;
}

auto_save_t automation_service_save(const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                                    auto_check_t *why, uint16_t *uid_out)
{
    if (why != NULL) {
        *why = AUTO_OK;
    }
    if (!stack_is_internal()) {
        return AUTO_SAVE_FAILED;
    }
    const auto_save_t begun = edit_begin();
    if (begun != AUTO_SAVE_OK) {
        return begun;
    }
    /* AUT-002, AUT-015 (layer 2): checked against the panel as it is now, never a caller's env */
    auto_env_t env;
    if (!automation_service_env(&env, s_save_zones)) {
        ESP_LOGW(TAG, "save refused: the alarm configuration couldn't be read");
        xSemaphoreGive(s_save_lock);
        return AUTO_SAVE_FAILED;
    }
    /* BL-187: the saver's authority now, so an installer's save of an Arm or Disarm is refused */
    installer_read_t who = {.user = user, .installer = true};
    if (alarm_service_call(read_installer, &who) != ESP_OK) {
        ESP_LOGW(TAG, "save refused: the saver's authority couldn't be read");
        xSemaphoreGive(s_save_lock);
        return AUTO_SAVE_FAILED;
    }
    env.by_installer = who.installer;
    uint16_t saved = r != NULL ? r->uid : 0u;
    const auto_save_t res = auto_store_save_at(&s_store, &s_kv, r, &env, user, expected_gen,
                                               now_epoch_s(), why, &saved);
    if (res == AUTO_SAVE_OK && uid_out != NULL) {
        *uid_out = saved;
    }
    edit_end(res, saved);
    return res;
}

auto_save_t automation_service_delete(uint16_t uid, uint16_t expected_gen)
{
    if (!stack_is_internal()) {
        return AUTO_SAVE_FAILED;
    }
    const auto_save_t begun = edit_begin();
    if (begun != AUTO_SAVE_OK) {
        return begun;
    }
    const auto_save_t res = auto_store_delete(&s_store, &s_kv, uid, expected_gen);
    edit_end(res, uid);
    return res;
}

auto_save_t automation_service_delete_slot(uint8_t slot, uint16_t expected_gen)
{
    if (!stack_is_internal()) {
        return AUTO_SAVE_FAILED;
    }
    const auto_save_t begun = edit_begin();
    if (begun != AUTO_SAVE_OK) {
        return begun;
    }
    /* The slot's uid, read before the delete; a record that can't be read (uid 0, never run)
     * makes every automation wait for the new rules. */
    const uint16_t uid = slot >= 1u && slot <= AUTO_MAX && s_store.used[slot - 1u]
                             ? s_store.rules[slot - 1u].uid
                             : 0u;
    const auto_save_t res = auto_store_delete_slot(&s_store, &s_kv, slot, expected_gen);
    edit_end(res, uid);
    return res;
}

auto_save_t automation_service_set_off(uint16_t uid, bool off, uint16_t expected_gen)
{
    if (!stack_is_internal()) {
        return AUTO_SAVE_FAILED;
    }
    const auto_save_t begun = edit_begin();
    if (begun != AUTO_SAVE_OK) {
        return begun;
    }
    const auto_save_t res = auto_store_set_off(&s_store, &s_kv, uid, off, expected_gen);
    edit_end(res, uid);
    return res;
}

bool automation_service_status(uint16_t uid, auto_status_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!started() || uid == 0u) {
        return false;
    }
    bool found = false;
    view_lock();
    for (size_t i = 0; i < s_status_n && !found; i++) {
        if (s_status[i].uid == uid) {
            *out = s_status[i].st;
            found = true;
        }
    }
    for (size_t i = 0; i < s_view_n && !found; i++) {
        found = s_view[i].uid == uid; /* stored, not run yet: AUTO_RES_NONE */
    }
    view_unlock();
    return found;
}

/* Try it now's and a put-off's way to the task: busy (100, AUTOMATION_TRY_BUSY and
 * AUTOMATION_POFF_BUSY) when the task doesn't take the request within TRY_WAIT_MS; once taken,
 * its answer is waited for. */
static uint8_t task_request(try_req_t req)
{
    const uint8_t busy = 100u;
    TaskHandle_t t = __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
    if (t == NULL || t == xTaskGetCurrentTaskHandle()) {
        return busy;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(TRY_WAIT_MS);
    if (xSemaphoreTake(s_try_lock, limit) != pdTRUE) {
        return busy;
    }
    uint8_t answer = busy;
    if (++s_try_seq == 0u) {
        s_try_seq = 1u; /* 0 means "none pending" */
    }
    req.seq = s_try_seq;
    req.deadline = start + limit;
    __atomic_store_n(&s_try_pending, req.seq, __ATOMIC_RELEASE);
    if (xQueueSend(s_try_q, &req, 0) != pdTRUE) {
        __atomic_store_n(&s_try_pending, 0u, __ATOMIC_RELEASE);
        xSemaphoreGive(s_try_lock);
        return answer;
    }
    xTaskNotifyGive(t);
    bool claimed = false; /* the task took it: its answer comes once auto_run_try returns */
    for (;;) {
        const TickType_t spent = xTaskGetTickCount() - start;
        TickType_t wait = claimed ? portMAX_DELAY : (spent < limit ? limit - spent : 0u);
        if (xSemaphoreTake(s_try_done, wait) != pdTRUE) {
            uint32_t want = req.seq;
            if (__atomic_compare_exchange_n(&s_try_pending, &want, 0u, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                break; /* given up before the task took it: it never runs */
            }
            claimed = true;
            continue;
        }
        if (__atomic_load_n(&s_try_answer_seq, __ATOMIC_ACQUIRE) == req.seq) {
            answer = __atomic_load_n(&s_try_answer, __ATOMIC_RELAXED);
            break;
        }
        /* a give left over from an earlier request: keep waiting for this one's */
    }
    xSemaphoreGive(s_try_lock);
    return answer;
}

_Static_assert((unsigned)AUTOMATION_TRY_BUSY == 100u && (unsigned)AUTOMATION_POFF_BUSY == 100u,
               "task_request's busy");

auto_try_t automation_service_try(uint16_t uid)
{
    const try_req_t req = {.uid = uid, .kind = (uint8_t)REQ_TRY};
    return (auto_try_t)task_request(req);
}

auto_put_off_t automation_service_put_off(uint16_t uid, uint8_t choice, uint8_t user,
                                          bool may_disarm)
{
    const try_req_t req = {.uid = uid,
                           .kind = (uint8_t)REQ_PUT_OFF,
                           .choice = choice,
                           .user = user,
                           .may_disarm = may_disarm ? 1u : 0u};
    return (auto_put_off_t)task_request(req);
}

size_t automation_service_warnings(auto_warning_t *out, size_t max)
{
    if (!started() || out == NULL) {
        return 0u;
    }
    view_lock();
    const size_t n = s_warn_n < max ? s_warn_n : max;
    memcpy(out, s_warn, n * sizeof(out[0]));
    view_unlock();
    return n;
}

bool automation_service_warning(uint16_t uid, auto_warning_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!started() || uid == 0u) {
        return false;
    }
    bool found = false;
    view_lock();
    for (size_t i = 0; i < s_warn_n && !found; i++) {
        if (s_warn[i].uid == uid) {
            *out = s_warn[i];
            found = true;
        }
    }
    view_unlock();
    return found;
}

/* ---- console: "automations" (read-only, AUT-010; English, like "devices") ---- */

#if CONFIG_ALARM_SERVICE_CONSOLE
#include "esp_console.h"

static const char *const k_trig[AUTO_TRIG_COUNT] = {
    "none", "armed", "disarmed", "alarm", "fire", "zone_opened", "zone_closed", "time"};
static const char *const k_rec[] = {"ok", "newer", "broken", "bad"};
static const char *const k_src[] = {"none", "ha", "zigbee", "zwave"};
static const char *const k_act[ACT_COUNT] = {"none", "on",    "off",       "toggle",   "lock",
                                             "unlock", "open", "close",    "stop",     "set_level",
                                             "set_temp", "set_mode", "run"};
static const char *const k_res[] = {"none",     "done",     "refused",   "no_answer",
                                    "unreachable", "missing", "busy",    "held_back",
                                    "not_allowed", "cant_run", "no_clock", "less_secure_off",
                                    "maker_gone", "already", "not_ready", "no_warning",
                                    "arm_refused", "put_off"};

#define NAME_OF(table, i) \
    ((unsigned)(i) < sizeof(table) / sizeof((table)[0]) ? (table)[(unsigned)(i)] : "?")

/* The one rule the command prints at a time: the console task only (the REPL runs one command
 * at a time), in PSRAM rather than on its stack. */
static EXT_RAM_BSS_ATTR auto_rule_t s_con_rule;

/* The i-th rule of the published view, copied under s_auto_lock; false once the view has fewer
 * (an edit in between). */
static bool view_at(size_t i, auto_rule_t *out)
{
    view_lock();
    const bool ok = i < s_view_n;
    if (ok) {
        *out = s_view[i];
    }
    view_unlock();
    return ok;
}

static void print_minute(unsigned m)
{
    printf("%02u:%02u", (m / 60u) % 24u, m % 60u);
}

static void print_rule(const auto_rule_t *r)
{
    printf("%2u uid %-4u %-3s %-6s %s", (unsigned)r->slot, (unsigned)r->uid, r->off ? "off" : "on",
           NAME_OF(k_rec, r->rec), NAME_OF(k_trig, r->when.kind));
    const auto_trigger_t *w = &r->when;
    if (w->partitions != 0u) {
        printf(" parts 0x%02x", (unsigned)w->partitions);
    }
    if (w->kind == (uint8_t)AUTO_TRIG_ARMED && w->levels != 0u) {
        printf(" levels 0x%02x", (unsigned)w->levels);
    }
    if (w->kind == (uint8_t)AUTO_TRIG_ZONE_OPENED || w->kind == (uint8_t)AUTO_TRIG_ZONE_CLOSED) {
        printf(" zone %u", (unsigned)w->zone);
    }
    if (w->kind == (uint8_t)AUTO_TRIG_TIME) {
        static const char days[] = "MTWTFSS";
        printf(" at ");
        print_minute(w->minute);
        printf(" days ");
        for (unsigned d = 0; d < 7u; d++) {
            putchar(w->days == 0u || (w->days & (1u << d)) != 0u ? days[d] : '-');
        }
    }
    if (w->win_from != 0u && w->win_to != 0u) {
        printf(" between ");
        print_minute((unsigned)w->win_from - 1u);
        putchar('-');
        print_minute((unsigned)w->win_to - 1u);
    }
    for (unsigned i = 0; i < r->n && i < AUTO_ACTIONS; i++) {
        const auto_action_t *a = &r->then[i];
        if (a->kind == (uint8_t)AUTO_ACT_ARM) {
            printf(" | arm p%u level %u warn %u min", (unsigned)a->partition,
                   (unsigned)a->arm_level, (unsigned)a->warn_min);
            continue;
        }
        if (a->kind == (uint8_t)AUTO_ACT_DISARM) {
            printf(" | disarm p%u%s", (unsigned)a->partition, a->confirmed ? " confirmed" : "");
            continue;
        }
        printf(" | %s %s %s", NAME_OF(k_src, a->source), a->ref, NAME_OF(k_act, a->dev_action));
        if (a->value >= 0) {
            printf(" %d", (int)a->value);
        }
        if (a->mode[0] != '\0') {
            printf(" %s", a->mode);
        }
        if (a->off_after_min != 0u) {
            printf(" off after %u min", (unsigned)a->off_after_min);
        }
    }
    auto_status_t st;
    (void)automation_service_status(r->uid, &st);
    printf(" | last %s", NAME_OF(k_res, st.result));
    if (st.result != (uint8_t)AUTO_RES_NONE) {
        printf(" #%u%s", (unsigned)st.action_index + 1u, st.tried ? " tried" : "");
        if (st.reason != 0u) {
            printf(" %s", alarm_auto_fail_name((alarm_auto_fail_t)st.reason));
        }
        struct tm tm;
        const time_t t = (time_t)(st.at_epoch_ms / 1000);
        if (st.at_epoch_ms > 0 && localtime_r(&t, &tm) != NULL) {
            printf(" %04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min);
        } else {
            printf(" -");
        }
    }
    putchar('\n');
}

static int cmd_automations(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!started()) {
        printf("automations not loaded\n");
        return 1;
    }
    view_lock();
    const size_t n = s_view_n;
    view_unlock();
    printf("%u automation%s\n", (unsigned)n, n == 1u ? "" : "s");
    for (size_t i = 0; i < n; i++) {
        if (view_at(i, &s_con_rule)) {
            print_rule(&s_con_rule);
        }
    }
    TaskHandle_t t = __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
    printf("gen %u, %s; listener %s; task %s",
           (unsigned)automation_service_gen(),
           automation_service_read_only() ? "read-only" : "editable",
           s_listener_ok ? (__atomic_load_n(&s_heard, __ATOMIC_ACQUIRE) ? "registered, heard"
                                                                        : "registered")
                         : "not registered",
           t != NULL ? "running" : "not running");
    if (t != NULL) {
        printf(" (stack %u bytes free)", (unsigned)uxTaskGetStackHighWaterMark(t));
    }
    printf("\ndropped: %u occurrence(s), %u missed by the observer, %u device result(s)\n",
           (unsigned)__atomic_load_n(&s_occ_dropped, __ATOMIC_RELAXED),
           (unsigned)__atomic_load_n(&s_watch_missed, __ATOMIC_RELAXED),
           (unsigned)__atomic_load_n(&s_res_dropped, __ATOMIC_RELAXED));
    return 0;
}

esp_err_t automation_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "automations",
        .help = "The stored automations, their last outcome and the service's state (read-only)",
        .hint = NULL,
        .func = cmd_automations,
    };
    return esp_console_cmd_register(&cmd);
}
#else
esp_err_t automation_service_console_register(void)
{
    return ESP_OK;
}
#endif /* CONFIG_ALARM_SERVICE_CONSOLE */

#else /* !CONFIG_ALARM_FEATURE_AUTOMATIONS: no automations in this firmware */

#include <string.h>

const alarm_listener_t *automation_service_listener(void)
{
    return NULL;
}

void automation_service_listener_added(bool ok)
{
    (void)ok;
}

esp_err_t automation_service_start(void)
{
    return ESP_OK;
}

size_t automation_service_list(auto_rule_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0u;
}

bool automation_service_get(uint16_t uid, auto_rule_t *out)
{
    (void)uid;
    (void)out;
    return false;
}

uint16_t automation_service_gen(void)
{
    return 0u;
}

bool automation_service_read_only(void)
{
    return true;
}

bool automation_service_env(auto_env_t *env, auto_zone_t zones[ALARM_MAX_ZONES])
{
    (void)zones;
    if (env != NULL) {
        memset(env, 0, sizeof(*env));
    }
    return false;
}

auto_save_t automation_service_save(const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                                    auto_check_t *why, uint16_t *uid_out)
{
    (void)r;
    (void)user;
    (void)expected_gen;
    (void)uid_out;
    if (why != NULL) {
        *why = AUTO_OK;
    }
    return AUTO_SAVE_READ_ONLY;
}

auto_save_t automation_service_delete(uint16_t uid, uint16_t expected_gen)
{
    (void)uid;
    (void)expected_gen;
    return AUTO_SAVE_READ_ONLY;
}

auto_save_t automation_service_delete_slot(uint8_t slot, uint16_t expected_gen)
{
    (void)slot;
    (void)expected_gen;
    return AUTO_SAVE_READ_ONLY;
}

auto_save_t automation_service_set_off(uint16_t uid, bool off, uint16_t expected_gen)
{
    (void)uid;
    (void)off;
    (void)expected_gen;
    return AUTO_SAVE_READ_ONLY;
}

bool automation_service_status(uint16_t uid, auto_status_t *out)
{
    (void)uid;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

auto_try_t automation_service_try(uint16_t uid)
{
    (void)uid;
    return AUTOMATION_TRY_BUSY;
}

auto_put_off_t automation_service_put_off(uint16_t uid, uint8_t choice, uint8_t user,
                                          bool may_disarm)
{
    (void)uid;
    (void)choice;
    (void)user;
    (void)may_disarm;
    return AUTOMATION_POFF_BUSY;
}

size_t automation_service_warnings(auto_warning_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0u;
}

bool automation_service_warning(uint16_t uid, auto_warning_t *out)
{
    (void)uid;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

esp_err_t automation_service_console_register(void)
{
    return ESP_OK;
}

#endif /* CONFIG_ALARM_FEATURE_AUTOMATIONS */
