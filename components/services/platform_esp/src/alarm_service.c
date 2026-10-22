/*
 * alarm_service.c - the alarm task: owns the engine, serializes every input, persists state
 * and publishes status snapshots. See alarm_service.h for the threading rules.
 */
#include "alarm_service.h"
#include "alarm_mem.h"

#include "esp_attr.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "alarm/alarm_strings.h"
#include "alarm_store.h"
#include "code_hash.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "zone_hub/zone_bind_gate.h"

static const char *TAG = "alarm";

#define QUEUE_LEN     32
/* The listeners the firmware registers at most, every feature on. Raise it with each new
 * alarm_service_add_listener call (Grep it across components/ and firmware/):
 *   panel_app.c     1 the event log, 2 the sounder, 3 MQTT, 4 Home Assistant, 5 the zone hub,
 *                   6 notifications, 7 the automations (docs/features/41 DESIGN.md section 5),
 *                   10 the panel link's master side (plink_service, docs/features/11)
 *   board (before_engine, crowpanel_p4 app_main.c)  8 the Panel Manager's sign-ins (web_login)
 *   zwave_service.c 9 Z-Wave's keypad feedback (added after the engine starts) */
#define ALARM_LISTENERS_USED 10
#define MAX_LISTENERS 12 /* the rest is headroom */
_Static_assert(MAX_LISTENERS >= ALARM_LISTENERS_USED,
               "every listener the firmware registers needs a slot: raise MAX_LISTENERS");
#define EVENT_RING    CONFIG_ALARM_EVENT_RING
#define MAX_WAIT_MS   60000u
#define EPOCH_2025_S  1735689600LL

/* A write that fails is retried, backing off so a broken flash cannot turn every zone event
 * into a stalled alarm task. */
#define STORE_RETRY_MIN_MS 1000u
#define STORE_RETRY_MAX_MS 60000u
#define STORE_LOAD_TRIES   3u    /* start-up only, off the alarm task */
#define STORE_LOAD_WAIT_MS 250u

typedef enum {
    MSG_ZONE,
    MSG_TROUBLE,
    MSG_CHECKIN,
    MSG_CALL,
    MSG_REACHABLE,
    MSG_STORAGE
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    union {
        struct {
            uint16_t zone;
            alarm_zone_event_t ev;
        } zone;
        struct {
            alarm_sys_trouble_t trouble;
            bool active;
        } trouble;
        uint8_t device;
        struct {
            uint8_t device;
            bool reachable;
            uint8_t partition; /* UINT8_MAX: use the configured device scope */
        } reach;
        struct {
            alarm_storage_owner_t owner;
            bool active;
        } storage;
        struct {
            alarm_service_fn fn;
            void *ctx;
            SemaphoreHandle_t done;
        } call;
    };
} msg_t;

static uint32_t hash_faults(void *ctx)
{
    (void)ctx;
    return code_hash_faults();
}

/* The engine, its snapshot and the event ring live in PSRAM: only tasks touch them (the alarm
 * task, and the others under s_lock), never an ISR or the flash-writing code that runs with the
 * cache off. Internal RAM is the reserve the crypto and the network drivers draw on
 * (docs/bugs/B001). */
static alarm_engine_t s_eng ALARM_SLOW_BSS;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock; /* snapshot, event ring, listener table */

static alarm_listener_t s_listeners[MAX_LISTENERS];
static size_t s_listener_count;

static alarm_snapshot_t s_snap ALARM_SLOW_BSS;
static uint32_t s_config_seq = 1;
static alarm_event_t s_ring[EVENT_RING] ALARM_SLOW_BSS;
static size_t s_ring_head;
static size_t s_ring_count;

/* Set by engine callbacks, handled after the engine call returns (alarm task only). */
static bool s_status_dirty = true;
static bool s_frozen; /* factory reset or restore under way: never write NVS again */
static bool s_dormant; /* the keypad role: no engine, inputs dropped (alarm_service_set_dormant) */
static uint32_t s_min_event_seq;

/* Durability. The engine's state in RAM is authoritative and is never rolled back because a
 * write failed - a disarmed panel stays disarmed even when flash refuses. Instead the revision
 * the engine has reached (s_*_rev) and the revision NVS holds (s_*_saved) are tracked apart,
 * and a write that failed stays outstanding until it succeeds. Coalescing keeps the newest
 * revision: s_rt_pending always holds the latest runtime state and the configuration is read
 * from the engine at write time. Alarm task only, except for the relaxed reads in
 * alarm_service_store_status(). */
static alarm_runtime_state_t s_rt_pending;
static uint32_t s_rt_rev;
static uint32_t s_rt_saved;
static uint32_t s_cfg_rev;
static uint32_t s_cfg_saved;
static alarm_ms_t s_store_retry_at;    /* 0 = no retry scheduled */
static uint32_t s_store_backoff_ms;
static esp_err_t s_store_err = ESP_OK; /* last write error while something is outstanding */
static bool s_store_trouble;           /* ALARM_TRBL_STORAGE is raised */
static bool s_store_sticky;            /* stored data we could not use: keep saying so */

/* Arming is held (CORE-ARM-013) while a sensor radio holds it (s_radio_hold) or a zone binding
 * change is being made (s_bind_hold: the gate was CHANGING when this engine step began;
 * zone_hub/zone_bind_gate.h, ZSRC-012, RF32). A reservation not yet claimed never holds arming:
 * an arm goes ahead and cancels it. Reserved here on the alarm task, claimed and ended by
 * zone_service from its own task; s_gate_mux makes each gate call atomic, nothing waits under it.
 * An arm refused for a binding change says what the radio's hold says ("Wait: a sensor radio is
 * updating", notify_words.c): the change is to which radio sensor a zone listens to, it lasts
 * milliseconds, and the advice - wait, then arm - is the right one; no new words to translate. */
static bool s_radio_hold;     /* alarm task only */
static bool s_bind_hold;      /* alarm task only */
static bool s_bind_stepping;  /* alarm task only: this step began with a reservation standing */
static zone_bind_gate_t s_gate; /* zero: idle */
static portMUX_TYPE s_gate_mux = portMUX_INITIALIZER_UNLOCKED;
/* The gate's owner (ZB21): the task whose reserve succeeded, until its end - one changer at a
 * time, so a refused claim's end never ends another task's change (the gate itself has no
 * owner). Taken by reserve holding no lock (reserve's own rule), waited for at most
 * BIND_OWNER_WAIT_MS; the alarm task never takes it. Holders: zone_service's change (which
 * waits for it with its store mutex held) and the Zigbee task (zbz_panel_*_gated), which ends
 * its change before zone_service_unlock may take that store mutex - so no owner ever waits for
 * the store mutex, and the two never wait for each other. */
#define BIND_OWNER_WAIT_MS 500u
static StaticSemaphore_t s_bind_owner_buf;
static SemaphoreHandle_t s_bind_owner; /* set once by alarm_service_start */

alarm_ms_t alarm_service_now(void)
{
    return (alarm_ms_t)(esp_timer_get_time() / 1000);
}

static bool dormant(void)
{
    return __atomic_load_n(&s_dormant, __ATOMIC_ACQUIRE);
}

void alarm_service_set_dormant(void)
{
    __atomic_store_n(&s_dormant, true, __ATOMIC_RELEASE);
}

bool alarm_service_dormant(void)
{
    return dormant();
}

/* What a synchronous call answers when the alarm task never ran it: unavailable on a panel with
 * no engine (the keypad role), otherwise the engine is in no state to run it (not started). */
static alarm_result_t not_run(void)
{
    return dormant() ? ALARM_ERR_UNAVAILABLE : ALARM_ERR_BAD_STATE;
}

/* Alarm task: the engine holds arming exactly while the radio holds it or a change is made. */
static void apply_arm_hold(void)
{
    const bool want = s_radio_hold || s_bind_hold;
    if (alarm_engine_arming_held(&s_eng) != want) {
        alarm_engine_hold_arming(&s_eng, want);
    }
}

/* Alarm task, before every engine step (RF32). A reservation becomes STEPPING, so it can't be
 * claimed while this step - perhaps an arm - runs; a change claimed earlier holds arming for the
 * step. Only this task reserves and only from RESERVED can a change be claimed, so the gate can't
 * become CHANGING while a step runs. A change past ZONE_BIND_GATE_STALL_MS is logged once: it is
 * never lapsed (the race would come back), and no system trouble exists for such stalls (a full
 * alarm queue logs only too). */
static void bind_step_begin(void)
{
    const alarm_ms_t now = alarm_service_now();
    portENTER_CRITICAL(&s_gate_mux);
    s_bind_hold = zone_bind_gate_step_begin(&s_gate);
    s_bind_stepping = s_gate.state == ZONE_BIND_GATE_STEPPING;
    const bool stalled = zone_bind_gate_stalled(&s_gate, now);
    portEXIT_CRITICAL(&s_gate_mux);
    if (stalled) {
        ESP_LOGE(TAG, "a zone sensor change has held arming for %u s: its task is stuck",
                 (unsigned)(ZONE_BIND_GATE_STALL_MS / 1000u));
    }
    apply_arm_hold();
}

/* Alarm task, after every engine step: a reservation stands again while every partition is still
 * disarmed; an arm in this step cancelled it (the changer's claim is refused). */
static void bind_step_end(void)
{
    if (!s_bind_stepping) {
        return;
    }
    s_bind_stepping = false;
    const bool disarmed = zone_bind_gate_all_disarmed(&s_eng); /* read outside the spinlock */
    portENTER_CRITICAL(&s_gate_mux);
    zone_bind_gate_step_end(&s_gate, disarmed);
    portEXIT_CRITICAL(&s_gate_mux);
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ---- engine ports (run inside engine calls, on the alarm task) --------------------- */

static void log_event(const alarm_event_t *ev)
{
    const char *arg = "";
    switch (ev->type) {
    case ALARM_EV_ARMED:
        arg = alarm_level_name((alarm_arm_level_t)ev->arg);
        break;
    case ALARM_EV_ALARM:
        arg = alarm_cause_name((alarm_cause_t)ev->arg);
        break;
    case ALARM_EV_ARM_FAILED:
    case ALARM_EV_INVALID_CODE:
        arg = alarm_result_name((alarm_result_t)ev->arg);
        break;
    case ALARM_EV_TROUBLE:
    case ALARM_EV_TROUBLE_RESTORE:
        arg = (ev->arg2 == ALARM_TRBL_FAMILY_SYSTEM)
                  ? alarm_sys_trouble_name((alarm_sys_trouble_t)ev->arg)
                  : (ev->arg2 == ALARM_TRBL_FAMILY_ZONE)
                        ? alarm_zone_trouble_name((alarm_zone_trouble_t)ev->arg)
                        : "device_supervision";
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "#%" PRIu32 " %s p%u z%u u%u %s%s", ev->seq, alarm_event_name(ev->type),
             ev->partition, ev->zone, ev->user, arg,
             (ev->flags & ALARM_EVF_REPORT) ? " [report]" : "");
}

static void port_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    lock();
    s_ring[s_ring_head] = *ev;
    s_ring_head = (s_ring_head + 1u) % EVENT_RING;
    if (s_ring_count < EVENT_RING) {
        s_ring_count++;
    }
    const size_t n = s_listener_count;
    unlock();

    log_event(ev);
    for (size_t i = 0; i < n; i++) {
        if (s_listeners[i].on_event != NULL) {
            s_listeners[i].on_event(s_listeners[i].ctx, ev);
        }
    }
    s_status_dirty = true;
}

static void port_report(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    for (size_t i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].on_report != NULL) {
            s_listeners[i].on_report(s_listeners[i].ctx, ev);
        }
    }
}

static void port_status(void *ctx, uint8_t partition)
{
    (void)ctx;
    (void)partition;
    s_status_dirty = true;
}

static void port_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone)
{
    (void)ctx;
    for (size_t i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].on_sound != NULL) {
            s_listeners[i].on_sound(s_listeners[i].ctx, partition, sound, zone);
        }
    }
}

static void port_output(void *ctx, alarm_output_t output, uint8_t partition,
                        alarm_pattern_t pattern)
{
    (void)ctx;
    ESP_LOGI(TAG, "output %d p%u -> pattern %d", (int)output, partition, (int)pattern);
    for (size_t i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].on_output != NULL) {
            s_listeners[i].on_output(s_listeners[i].ctx, output, partition, pattern);
        }
    }
}

static void port_persist(void *ctx, const alarm_runtime_state_t *state)
{
    (void)ctx;
    s_rt_pending = *state; /* newest wins: a coalesced write stores the latest revision */
    s_rt_rev++;
}

static void port_config_changed(void *ctx)
{
    (void)ctx;
    s_cfg_rev++;    /* changed in RAM; whether it reached flash is tracked by s_cfg_saved */
    s_config_seq++; /* listeners re-read the configuration whether or not the write lands */
    s_status_dirty = true;
}

/* PWR-007: why the chip restarted, in the engine's portable words. */
static alarm_start_reason_t start_reason_now(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return ALARM_START_POWER_ON;
    case ESP_RST_SW:
        return ALARM_START_SOFTWARE;
    case ESP_RST_PANIC:
        return ALARM_START_PANIC;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return ALARM_START_WATCHDOG;
    case ESP_RST_BROWNOUT:
        return ALARM_START_BROWNOUT;
    case ESP_RST_EXT:
        return ALARM_START_EXTERNAL;
    case ESP_RST_UNKNOWN:
        return ALARM_START_UNKNOWN;
    default:
        return ALARM_START_OTHER;
    }
}

static int64_t port_epoch_ms(void *ctx)
{
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec < EPOCH_2025_S) {
        return 0; /* clock not set yet */
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- after every engine call ------------------------------------------------------- */

static void refresh_snapshot(void)
{
    lock();
    alarm_snapshot_t *s = &s_snap;
    /* A binding change holds arming for milliseconds (RF32): never shown as "not ready" - the
     * readiness people and Home Assistant see would twitch (UI-020, UI-023). Only the radio's
     * hold, which lasts, is shown. */
    const bool bind_hold = alarm_engine_arming_held(&s_eng) && !s_radio_hold;
    if (bind_hold) {
        alarm_engine_hold_arming(&s_eng, false);
    }
    alarm_engine_snapshot(&s_eng, s);
    if (bind_hold) {
        alarm_engine_hold_arming(&s_eng, true);
    }
    s->config_seq = s_config_seq;
    s->seq++;
    const size_t n = s_listener_count;
    unlock();

    /* s_snap is only written by this task, so listeners may read it without the lock. */
    for (size_t i = 0; i < n; i++) {
        if (s_listeners[i].on_snapshot != NULL) {
            s_listeners[i].on_snapshot(s_listeners[i].ctx, &s_snap);
        }
    }
}

/* Raise or clear the storage trouble. Calling the engine here is allowed: after_engine() runs
 * after the engine call has returned, on the alarm task - it is not a port re-entering it. */
static void store_trouble(bool active)
{
    const bool want = active || s_store_sticky;
    if (s_store_trouble == want) {
        return;
    }
    s_store_trouble = want;
    alarm_engine_storage_trouble(&s_eng, ALARM_STORAGE_CONFIG, want, alarm_service_now());
}

static void store_settled(void)
{
    s_store_err = ESP_OK;
    s_store_backoff_ms = 0u;
    s_store_retry_at = 0u;
}

static void store_retry_later(void)
{
    if (s_store_backoff_ms == 0u) {
        s_store_backoff_ms = STORE_RETRY_MIN_MS;
    } else if (s_store_backoff_ms < STORE_RETRY_MAX_MS / 2u) {
        s_store_backoff_ms *= 2u;
    } else {
        s_store_backoff_ms = STORE_RETRY_MAX_MS;
    }
    s_store_retry_at = alarm_service_now() + s_store_backoff_ms;
}

/* Writes what the engine changed, and keeps whatever did not land outstanding. Nothing here
 * ever changes the engine's state: a failed write is a storage problem, not a reason to undo
 * an arm or a disarm. */
static void store_flush(void)
{
    const bool rt_due = s_rt_rev != s_rt_saved;
    const bool cfg_due = s_cfg_rev != s_cfg_saved;
    if (!rt_due && !cfg_due) {
        store_settled();
        store_trouble(false);
        return;
    }
    if (s_store_retry_at != 0u && alarm_service_now() < s_store_retry_at) {
        return; /* backing off; zone events and commands keep being served meanwhile */
    }

    esp_err_t err = ESP_OK;
    if (rt_due) {
        const uint32_t rev = s_rt_rev; /* anything the engine changes later stays outstanding */
        const alarm_runtime_state_t state = s_rt_pending;
        const esp_err_t e = alarm_store_save_runtime(&state);
        if (e == ESP_OK) {
            s_rt_saved = rev;
        } else {
            ESP_LOGE(TAG, "saving runtime state failed: %s", esp_err_to_name(e));
            err = e;
        }
    }
    if (cfg_due) {
        const uint32_t rev = s_cfg_rev;
        const esp_err_t e = alarm_store_save_config(alarm_engine_config(&s_eng));
        if (e == ESP_OK) {
            s_cfg_saved = rev;
            s_store_sticky = false; /* the stored copy is readable again */
        } else {
            err = e; /* alarm_store_save_config logs the reason */
        }
    }

    if (err != ESP_OK) {
        s_store_err = err;
        store_retry_later();
        store_trouble(true);
        return;
    }
    store_settled();
    /* Anything the engine changed while the write ran is still outstanding; the trouble clears
     * on the pass that finds nothing left to write. */
    if (s_rt_rev == s_rt_saved && s_cfg_rev == s_cfg_saved) {
        store_trouble(false);
    }
}

static void after_engine(void)
{
    if (s_frozen) {
        /* Factory reset under way: nothing the engine does reaches NVS any more. */
        s_rt_saved = s_rt_rev;
        s_cfg_saved = s_cfg_rev;
        store_settled();
    } else {
        store_flush();
    }
    if (s_status_dirty) {
        s_status_dirty = false;
        refresh_snapshot();
    }
}

/* ---- the task ---------------------------------------------------------------------- */

static TickType_t wait_ticks(void)
{
    alarm_ms_t deadline = alarm_engine_next_deadline(&s_eng);
    if (s_store_retry_at != 0u && s_store_retry_at < deadline) {
        deadline = s_store_retry_at; /* also wake up to retry an outstanding write */
    }
    portENTER_CRITICAL(&s_gate_mux);
    const uint64_t stall_at = zone_bind_gate_deadline(&s_gate);
    portEXIT_CRITICAL(&s_gate_mux);
    if (stall_at < deadline) {
        deadline = stall_at; /* RF32: report a binding change that holds arming too long */
    }
    if (deadline == ALARM_TIME_NEVER) {
        return pdMS_TO_TICKS(MAX_WAIT_MS);
    }
    const alarm_ms_t now = alarm_service_now();
    if (deadline <= now) {
        return 0;
    }
    alarm_ms_t ms = deadline - now;
    if (ms > MAX_WAIT_MS) {
        ms = MAX_WAIT_MS;
    }
    return pdMS_TO_TICKS((uint32_t)ms) + 1; /* round up: never wake before the deadline */
}

static void handle(const msg_t *m)
{
    const alarm_ms_t now = alarm_service_now();
    switch (m->kind) {
    case MSG_ZONE:
        alarm_engine_zone_event(&s_eng, m->zone.zone, m->zone.ev, now);
        break;
    case MSG_TROUBLE:
        alarm_engine_system_trouble(&s_eng, m->trouble.trouble, m->trouble.active, now);
        break;
    case MSG_CHECKIN:
        alarm_engine_device_checkin(&s_eng, m->device, now);
        break;
    case MSG_CALL:
        m->call.fn(&s_eng, now, m->call.ctx);
        break;
    case MSG_REACHABLE:
        if (m->reach.partition == UINT8_MAX) {
            alarm_engine_device_reachable(&s_eng, m->reach.device, m->reach.reachable, now);
        } else {
            alarm_engine_device_reachable_at(&s_eng, m->reach.device, m->reach.partition,
                                             m->reach.reachable, now);
        }
        break;
    case MSG_STORAGE:
        alarm_engine_storage_trouble(&s_eng, m->storage.owner, m->storage.active, now);
        break;
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    for (;;) {
        msg_t m;
        SemaphoreHandle_t done = NULL;
        if (xQueueReceive(s_queue, &m, wait_ticks()) == pdTRUE) {
            bind_step_begin(); /* RF32: before the step - it may be an arm */
            handle(&m);
            bind_step_end();
            if (m.kind == MSG_CALL) {
                done = m.call.done;
            }
        } else {
            bind_step_begin();
            alarm_engine_tick(&s_eng, alarm_service_now());
            bind_step_end();
        }
        after_engine();
        if (done != NULL) {
            xSemaphoreGive(done); /* results, stored state and snapshot are final now */
        }
    }
}

/* ---- public API -------------------------------------------------------------------- */

/* The queue exists from the first input on, so troubles reported before the engine starts
 * (storage, co-processor) wait in it instead of being lost. */
static bool ensure_queue(void)
{
    if (__atomic_load_n(&s_queue, __ATOMIC_ACQUIRE) == NULL) {
        QueueHandle_t q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
        if (q == NULL) {
            return false;
        }
        QueueHandle_t expected = NULL;
        if (!__atomic_compare_exchange_n(&s_queue, &expected, q, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            vQueueDelete(q); /* another task created it first */
        }
    }
    return true;
}

void alarm_service_set_min_event_seq(uint32_t seq)
{
    s_min_event_seq = seq;
}

esp_err_t alarm_service_add_listener(const alarm_listener_t *listener)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    lock();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (s_listener_count < MAX_LISTENERS) {
        s_listeners[s_listener_count] = *listener;
        s_listener_count++;
        err = ESP_OK;
    }
    const size_t count = s_listener_count;
    unlock();
    if (__atomic_load_n(&s_task, __ATOMIC_ACQUIRE) != NULL) {
        /* the start line counted those before it: say each one added since */
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "listener added: %u of %u", (unsigned)count, (unsigned)MAX_LISTENERS);
        } else {
            ESP_LOGE(TAG, "listener refused: all %u taken", (unsigned)MAX_LISTENERS);
        }
    }
    return err;
}

esp_err_t alarm_service_start(void)
{
    if (s_task != NULL || dormant()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL || !ensure_queue()) {
        return ESP_ERR_NO_MEM;
    }
    if (__atomic_load_n(&s_bind_owner, __ATOMIC_ACQUIRE) == NULL) {
        __atomic_store_n(&s_bind_owner, xSemaphoreCreateMutexStatic(&s_bind_owner_buf),
                         __ATOMIC_RELEASE);
    }
    /* Without working crypto no code can be created or checked, and the factory configuration
     * cannot even be built. Coming up anyway would mean a panel nobody can disarm, so this is
     * a start-up failure: the caller restarts us (and an update on probation rolls back). */
    esp_err_t err = code_hash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "code hashing is not working (%s): not starting the alarm engine",
                 esp_err_to_name(err));
        return err;
    }

    alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_DEFAULT);
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(cfg, 0, sizeof(*cfg)); /* never hand uninitialised bytes to the engine */

    /* A load that failed for want of resources says nothing about what is stored, so it must
     * never end in factory defaults. Retry a few times - this runs at start-up, before the
     * alarm task exists - and otherwise refuse to start rather than reset the installation. */
    alarm_cfg_origin_t origin = ALARM_CFG_UNAVAILABLE;
    for (unsigned attempt = 1u; attempt <= STORE_LOAD_TRIES; attempt++) {
        err = alarm_store_load_config(cfg, &origin);
        if (origin != ALARM_CFG_UNAVAILABLE) {
            break;
        }
        ESP_LOGW(TAG, "configuration not readable (%s), attempt %u of %u", esp_err_to_name(err),
                 attempt, (unsigned)STORE_LOAD_TRIES);
        if (attempt < STORE_LOAD_TRIES) {
            vTaskDelay(pdMS_TO_TICKS(STORE_LOAD_WAIT_MS));
        }
    }
    if (origin == ALARM_CFG_UNAVAILABLE) {
        ESP_LOGE(TAG, "cannot read the stored configuration (%s): not starting the alarm "
                      "engine; the stored installation is left untouched",
                 esp_err_to_name(err));
        heap_caps_free(cfg);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }
    if (origin == ALARM_CFG_CREATED) {
        ESP_LOGW(TAG, "first boot: factory configuration (installer %s, master %s)",
                 ALARM_DEFAULT_INSTALLER_CODE, ALARM_DEFAULT_MASTER_CODE);
    }
    /* Stored data this firmware could not use, or a first-boot write that did not land: say so
     * on the screen and keep saying it until a configuration write succeeds. */
    s_store_sticky = (origin == ALARM_CFG_DEFAULTED) || (err != ESP_OK);

    alarm_runtime_state_t rt;
    const bool have_rt = alarm_store_load_runtime(&rt);
    if (!have_rt) {
        memset(&rt, 0, sizeof(rt)); /* disarmed, nothing bypassed: same as no restore */
        rt.version = ALARM_RUNTIME_VERSION;
    }
    if (rt.event_seq < s_min_event_seq) {
        rt.event_seq = s_min_event_seq; /* the log saw later events than the stored state */
    }

    const alarm_start_reason_t start_reason = start_reason_now();
    const alarm_ports_t ports = {
        .ctx = NULL,
        .on_event = port_event,
        .on_report = port_report,
        .on_status = port_status,
        .on_sound = port_sound,
        .on_output = port_output,
        .on_persist = port_persist,
        .on_config_changed = port_config_changed,
        .hash_code = code_hash_pbkdf2,
        .hash_faults = hash_faults,
        .get_epoch_ms = port_epoch_ms,
    };
    alarm_result_t r = alarm_engine_init_ex(&s_eng, cfg, &ports, &rt, alarm_service_now(), start_reason);
    if (r != ALARM_OK && have_rt) {
        ESP_LOGW(TAG, "stored runtime state rejected (%s); starting disarmed",
                 alarm_result_name(r));
        const uint32_t seq = rt.event_seq;
        memset(&rt, 0, sizeof(rt));
        rt.version = ALARM_RUNTIME_VERSION;
        rt.event_seq = seq;
        r = alarm_engine_init_ex(&s_eng, cfg, &ports, &rt, alarm_service_now(), start_reason);
    }
    if (r != ALARM_OK && origin == ALARM_CFG_STORED) {
        /* The stored blob decoded but the engine will not run on it. Fall back to the factory
         * configuration in RAM only: writing it here would erase the installation - users,
         * zones, codes - because of one bad field, and the stored copy is the only chance of
         * recovering it. It is replaced the first time the installer saves a good one. */
        ESP_LOGE(TAG, "stored configuration rejected (%s); running on factory defaults, "
                      "stored copy kept",
                 alarm_result_name(r));
        if (alarm_store_default_config(cfg) == ESP_OK) {
            memset(&rt, 0, sizeof(rt));
            rt.version = ALARM_RUNTIME_VERSION;
            rt.event_seq = s_min_event_seq;
            r = alarm_engine_init_ex(&s_eng, cfg, &ports, &rt, alarm_service_now(), start_reason);
            s_store_sticky = true;
        }
    }
    heap_caps_free(cfg);
    if (r != ALARM_OK) {
        ESP_LOGE(TAG, "the alarm engine would not start (%s)", alarm_result_name(r));
        return ESP_FAIL;
    }
    after_engine();

    if (xTaskCreatePinnedToCore(alarm_task, "alarm", 8192, NULL, CONFIG_ALARM_TASK_PRIORITY,
                                &s_task, tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "alarm engine running (%u zones, %u users max, engine %u bytes, %u of %u "
                  "listeners so far)",
             (unsigned)ALARM_MAX_ZONES, (unsigned)ALARM_MAX_USERS, (unsigned)sizeof(s_eng),
             (unsigned)s_listener_count, (unsigned)MAX_LISTENERS);
    return ESP_OK;
}

/* Zone events and troubles are state, not notifications: a dropped one would leave the engine
 * believing the opposite forever, so they wait for room instead (the alarm task always drains).
 * Check-ins may be dropped: a missed one is caught by the next. */
static void post(const msg_t *m)
{
    if (dormant()) {
        return; /* no engine in this role: nobody would ever read it */
    }
    if (!ensure_queue()) {
        ESP_LOGE(TAG, "no alarm queue: input dropped (kind %d)", (int)m->kind);
        return;
    }
    const TickType_t wait = m->kind == MSG_CHECKIN ? pdMS_TO_TICKS(200) : portMAX_DELAY;
    if (xQueueSend(s_queue, m, wait) != pdTRUE) {
        ESP_LOGE(TAG, "alarm queue full: input dropped (kind %d)", (int)m->kind);
    }
}

void alarm_service_zone_event(uint16_t zone, alarm_zone_event_t ev)
{
    const msg_t m = {.kind = MSG_ZONE, .zone = {.zone = zone, .ev = ev}};
    post(&m);
}

void alarm_service_system_trouble(alarm_sys_trouble_t trouble, bool active)
{
    const msg_t m = {.kind = MSG_TROUBLE, .trouble = {.trouble = trouble, .active = active}};
    post(&m);
}

/* OUT-020: the bell trouble stands while any kind of siren holds it (Zigbee, Z-Wave, wired,
 * 433 MHz): one owner clearing its own must not clear another's. The roll-up and its post are
 * one step under a mutex, so two owners can't post out of order. */
static portMUX_TYPE s_bell_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_bell_lock_buf;
static SemaphoreHandle_t s_bell_lock;
static uint32_t s_bell_owners; /* bit n: owner n holds the bell trouble */
static int s_bell_told = -1;   /* the bell trouble as last posted, -1 never */

void alarm_service_bell_trouble(alarm_bell_owner_t owner, bool active)
{
    if ((unsigned)owner >= (unsigned)ALARM_BELL_OWNER_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_bell_mux);
    if (s_bell_lock == NULL) {
        s_bell_lock = xSemaphoreCreateMutexStatic(&s_bell_lock_buf);
    }
    portEXIT_CRITICAL(&s_bell_mux);
    xSemaphoreTake(s_bell_lock, portMAX_DELAY);
    const uint32_t bit = 1u << (unsigned)owner;
    s_bell_owners = active ? (s_bell_owners | bit) : (s_bell_owners & ~bit);
    const int any = (s_bell_owners != 0u) ? 1 : 0;
    if (any != s_bell_told) {
        s_bell_told = any;
        alarm_service_system_trouble(ALARM_TRBL_BELL, any != 0);
    }
    xSemaphoreGive(s_bell_lock);
}

void alarm_service_storage_trouble(alarm_storage_owner_t owner, bool active)
{
    /* a trouble: waits for room rather than being dropped (ZW-039) */
    const msg_t m = {.kind = MSG_STORAGE, .storage = {.owner = owner, .active = active}};
    post(&m);
}

void alarm_service_device_checkin(uint8_t device)
{
    const msg_t m = {.kind = MSG_CHECKIN, .device = device};
    post(&m);
}

void alarm_service_device_reachable(uint8_t device, bool reachable)
{
    alarm_service_device_reachable_at(device, UINT8_MAX, reachable);
}

void alarm_service_device_reachable_at(uint8_t device, uint8_t partition, bool reachable)
{
    /* An edge must not be dropped (CORE-TRBL-008). */
    const msg_t m = {.kind = MSG_REACHABLE,
                     .reach = {.device = device, .reachable = reachable, .partition = partition}};
    post(&m);
}

static void hold_fn(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)e;
    (void)now;
    s_radio_hold = ctx != NULL;
    apply_arm_hold(); /* a binding change being made keeps its own hold */
    s_status_dirty = true; /* the partitions' readiness changed */
}

bool alarm_service_hold_arming(bool hold)
{
    return alarm_service_post(hold_fn, hold ? (void *)&s_eng : NULL) == ESP_OK;
}

/* On the alarm task, one engine step: every partition disarmed, so the reservation stands. It
 * holds nothing: an arm in a later step cancels it (bind_step_end). */
static void bind_reserve_fn(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    const bool disarmed = zone_bind_gate_all_disarmed(e); /* read outside the spinlock */
    portENTER_CRITICAL(&s_gate_mux);
    const zone_bind_reserve_t r = zone_bind_gate_try(&s_gate, disarmed, now);
    portEXIT_CRITICAL(&s_gate_mux);
    *(zone_bind_reserve_t *)ctx = r;
}

/* The calling task owns the gate (its reserve succeeded, its end not yet come). */
static bool bind_owned(void)
{
    SemaphoreHandle_t owner = __atomic_load_n(&s_bind_owner, __ATOMIC_ACQUIRE);
    return owner != NULL && xSemaphoreGetMutexHolder(owner) == xTaskGetCurrentTaskHandle();
}

zone_bind_reserve_t alarm_service_bind_reserve_why(void)
{
    SemaphoreHandle_t owner = __atomic_load_n(&s_bind_owner, __ATOMIC_ACQUIRE);
    if (owner == NULL || bind_owned()) {
        return ZONE_BIND_BUSY; /* not started, or this task holds one already: one at a time */
    }
    if (xSemaphoreTake(owner, pdMS_TO_TICKS(BIND_OWNER_WAIT_MS)) != pdTRUE) {
        return ZONE_BIND_BUSY; /* another change holds the gate: the caller may ask again */
    }
    zone_bind_reserve_t r = ZONE_BIND_BUSY;
    if (alarm_service_call(bind_reserve_fn, &r) != ESP_OK) {
        r = ZONE_BIND_BUSY;
    }
    if (r != ZONE_BIND_RESERVED) {
        xSemaphoreGive(owner); /* nothing reserved: nothing to end */
    }
    return r;
}

bool alarm_service_bind_reserve(void)
{
    return alarm_service_bind_reserve_why() == ZONE_BIND_RESERVED;
}

zone_bind_claim_t alarm_service_bind_claim(void)
{
    if (!bind_owned()) {
        return ZONE_BIND_CLAIM_REFUSED; /* no reservation of this task's: never another's */
    }
    const alarm_ms_t now = alarm_service_now();
    portENTER_CRITICAL(&s_gate_mux);
    const zone_bind_claim_t c = zone_bind_gate_claim(&s_gate, now);
    portEXIT_CRITICAL(&s_gate_mux);
    return c;
}

void alarm_service_bind_end(void)
{
    if (!bind_owned()) {
        return; /* not this task's gate: another's change goes on */
    }
    portENTER_CRITICAL(&s_gate_mux);
    zone_bind_gate_end(&s_gate);
    portEXIT_CRITICAL(&s_gate_mux);
    xSemaphoreGive(__atomic_load_n(&s_bind_owner, __ATOMIC_ACQUIRE));
}

bool alarm_service_bind_mine(void)
{
    if (!bind_owned()) {
        return false;
    }
    portENTER_CRITICAL(&s_gate_mux);
    const bool changing = s_gate.state == ZONE_BIND_GATE_CHANGING;
    portEXIT_CRITICAL(&s_gate_mux);
    return changing;
}

esp_err_t alarm_service_call(alarm_service_fn fn, void *ctx)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dormant() || s_task == NULL || xTaskGetCurrentTaskHandle() == s_task) {
        return ESP_ERR_INVALID_STATE; /* no engine, not started, or called from a listener */
    }
    StaticSemaphore_t sem_buf;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&sem_buf);
    const msg_t m = {.kind = MSG_CALL, .call = {.fn = fn, .ctx = ctx, .done = done}};
    if (xQueueSend(s_queue, &m, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done);
        return ESP_FAIL;
    }
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return ESP_OK;
}

/* The same message with no semaphore: handle() runs fn and alarm_task() gives nothing back. */
esp_err_t alarm_service_post(alarm_service_fn fn, void *ctx)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dormant() || __atomic_load_n(&s_task, __ATOMIC_ACQUIRE) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const msg_t m = {.kind = MSG_CALL, .call = {.fn = fn, .ctx = ctx, .done = NULL}};
    return xQueueSend(s_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

size_t alarm_service_waiting(void)
{
    QueueHandle_t q = __atomic_load_n(&s_queue, __ATOMIC_ACQUIRE);
    return q != NULL ? (size_t)uxQueueMessagesWaiting(q) : 0u;
}

esp_err_t alarm_service_store_status(void)
{
    const bool rt_due = __atomic_load_n(&s_rt_rev, __ATOMIC_RELAXED) !=
                        __atomic_load_n(&s_rt_saved, __ATOMIC_RELAXED);
    const bool cfg_due = __atomic_load_n(&s_cfg_rev, __ATOMIC_RELAXED) !=
                         __atomic_load_n(&s_cfg_saved, __ATOMIC_RELAXED);
    if (!rt_due && !cfg_due) {
        return __atomic_load_n(&s_store_sticky, __ATOMIC_RELAXED) ? ESP_ERR_INVALID_STATE
                                                                  : ESP_OK;
    }
    const esp_err_t err = __atomic_load_n(&s_store_err, __ATOMIC_RELAXED);
    return (err != ESP_OK) ? err : ESP_ERR_NOT_FINISHED;
}

/* Creating or changing a code needs crypto that is known to work: a hash produced by a broken
 * PSA can never be entered again. code_hash_init() retries after a transient failure. */
static alarm_result_t crypto_ready(const char *what)
{
    if (dormant()) {
        return ALARM_ERR_UNAVAILABLE; /* no engine to check a code with: nothing to hash for */
    }
    const esp_err_t err = code_hash_init();
    if (err == ESP_OK) {
        return ALARM_OK;
    }
    ESP_LOGE(TAG, "code hashing unavailable (%s): %s refused", esp_err_to_name(err), what);
    /* Not BAD_STATE: that means "the alarm is not in a state to allow this", and the installer
     * screen says so in those words ("Disarm first, and silence any alarm"). Crypto being
     * unavailable is not about the alarm's state at all, and telling someone to disarm a panel
     * that is already disarmed leaves them with nothing to try (B001). */
    return ALARM_ERR_UNAVAILABLE;
}

/* The engine samples hash_faults around each individual hash on its own task. Checking
 * around alarm_service_call here is too late to prevent lockout or storing a failed hash,
 * and can accidentally observe a different caller's operation. */

typedef struct {
    const alarm_cmd_t *cmd;
    const alarm_source_t *src;
    alarm_cmd_result_t *res;
    uint8_t user;          /* do_command_as: the already-identified user */
    alarm_svc_when_t when; /* do_command_as: the state the command needs */
    alarm_result_t r;
} cmd_ctx_t;

static void do_command(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    cmd_ctx_t *c = ctx;
    c->r = alarm_engine_command(e, c->cmd, c->src, now, c->res);
}

alarm_result_t alarm_service_command(const alarm_cmd_t *cmd, const alarm_source_t *src,
                                     alarm_cmd_result_t *result)
{
    cmd_ctx_t c = {.cmd = cmd, .src = src, .res = result, .r = not_run()};
    alarm_service_call(do_command, &c);
    return c.r;
}

/* Whether the partition is in the state `when` asks for, read on the alarm task. An alarm
 * sounding while disarmed (the keypad's sounder on an alarm pattern) counts as not disarmed: a
 * disarm - code + OFF - is what silences a burglary or panic siren there. The same rule as the
 * Panel Manager's status "alarm" (panel_web.c partition_status) for a disarmed partition. */
static bool command_state_ok(const alarm_engine_t *e, uint8_t partition, alarm_svc_when_t when)
{
    if (when == ALARM_SVC_WHEN_ANY)
        return true;
    alarm_partition_status_t ps;
    alarm_engine_partition_status(e, partition, &ps);
    const bool disarmed = ps.state == ALARM_STATE_DISARMED;
    if (when == ALARM_SVC_WHEN_DISARMED)
        return disarmed && !alarm_engine_program_active(e);
    const bool sounding = ps.sounder == ALARM_SOUND_ALARM || ps.sounder == ALARM_SOUND_FIRE ||
                          ps.sounder == ALARM_SOUND_CO || ps.sounder == ALARM_SOUND_AUX;
    return !disarmed || sounding;
}

static void do_command_as(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    cmd_ctx_t *c = ctx;
    if (!command_state_ok(e, c->cmd->partition, c->when)) {
        c->r = ALARM_ERR_BAD_STATE;
        return;
    }
    c->r = alarm_engine_command_as(e, c->cmd, c->src, c->user, now, c->res);
}

/* No crypto_ready() here: nothing is hashed, the caller already proved who the user is. */
alarm_result_t alarm_service_command_as(const alarm_cmd_t *cmd, const alarm_source_t *src,
                                        uint8_t user, alarm_svc_when_t when,
                                        alarm_cmd_result_t *result)
{
    if (result)
        memset(result, 0, sizeof(*result));
    cmd_ctx_t c = {.cmd = cmd, .src = src, .res = result, .user = user, .when = when,
                   .r = not_run()};
    alarm_service_call(do_command_as, &c);
    return c.r;
}

typedef struct {
    const alarm_source_t *src;
    const char *code;
    uint8_t partition;
    uint8_t user;
    alarm_authority_t authority;
    alarm_result_t r;
} auth_ctx_t;

static void do_auth(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    auth_ctx_t *c = ctx;
    c->r = alarm_engine_authenticate(e, c->src, c->code, c->partition, now, &c->user);
    c->authority = ALARM_AUTH_NONE;
    if (c->r == ALARM_OK && c->user >= 1u && c->user <= ALARM_MAX_USERS) {
        c->authority = alarm_engine_config(e)->users[c->user - 1u].authority;
    }
}

alarm_result_t alarm_service_authenticate(const alarm_source_t *src, const char *code,
                                          uint8_t partition, uint8_t *user_out)
{
    auth_ctx_t c = {.src = src, .code = code, .partition = partition, .r = not_run()};
    /* Refuse rather than hash the code into an unpredictable value that would fail as a wrong
     * code and count towards the keypad lockout. Either way nobody gets in. */
    const alarm_result_t ready = crypto_ready("checking a code");
    if (ready != ALARM_OK) {
        return ready;
    }
    alarm_service_call(do_auth, &c);
    if (user_out != NULL) {
        *user_out = c.user;
    }
    return c.r;
}

alarm_result_t alarm_service_authenticate_as(const alarm_source_t *src, const char *code,
                                             uint8_t partition, uint8_t *user_out,
                                             alarm_authority_t *authority_out)
{
    auth_ctx_t c = {.src = src, .code = code, .partition = partition, .r = not_run()};
    const alarm_result_t ready = crypto_ready("checking a code");
    if (ready != ALARM_OK) {
        return ready;
    }
    alarm_service_call(do_auth, &c);
    if (user_out != NULL) {
        *user_out = c.user;
    }
    if (authority_out != NULL) {
        *authority_out = c.authority;
    }
    return c.r;
}

typedef struct {
    const alarm_source_t *src;
    const char *actor;
    uint8_t user_no;
    const char *new_code;
    alarm_authority_t authority;
    uint8_t partitions;
    const char *name;
    uint8_t as_user; /* CORE-USR-028: the signed-in actor of the _as calls */
    alarm_result_t r;
} user_ctx_t;

static void do_user_set(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    user_ctx_t *c = ctx;
    c->r = alarm_engine_user_set(e, c->src, c->actor, c->user_no, c->new_code, c->authority,
                                 c->partitions, c->name, now);
}

static void do_user_delete(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    user_ctx_t *c = ctx;
    c->r = alarm_engine_user_delete(e, c->src, c->actor, c->user_no, now);
}

alarm_result_t alarm_service_user_set(const alarm_source_t *src, const char *actor_code,
                                      uint8_t user_no, const char *new_code,
                                      alarm_authority_t authority, uint8_t partitions,
                                      const char *name)
{
    user_ctx_t c = {.src = src, .actor = actor_code, .user_no = user_no, .new_code = new_code,
                    .authority = authority, .partitions = partitions, .name = name,
                    .r = not_run()};
    if (crypto_ready("setting a user code") != ALARM_OK) {
        return ALARM_ERR_UNAVAILABLE;
    }
    alarm_service_call(do_user_set, &c);
    return c.r;
}

alarm_result_t alarm_service_user_delete(const alarm_source_t *src, const char *actor_code,
                                         uint8_t user_no)
{
    user_ctx_t c = {.src = src, .actor = actor_code, .user_no = user_no,
                    .r = not_run()};
    alarm_service_call(do_user_delete, &c);
    return c.r;
}

static void do_user_update(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    user_ctx_t *c = ctx;
    c->r = alarm_engine_user_update(e, c->src, c->actor, c->user_no, c->authority, c->partitions,
                                    c->name, now);
}

alarm_result_t alarm_service_user_update(const alarm_source_t *src, const char *actor_code,
                                         uint8_t user_no, alarm_authority_t authority,
                                         uint8_t partitions, const char *name)
{
    user_ctx_t c = {.src = src, .actor = actor_code, .user_no = user_no, .authority = authority,
                    .partitions = partitions, .name = name, .r = not_run()};
    alarm_service_call(do_user_update, &c);
    return c.r;
}

static void do_user_set_as(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    user_ctx_t *c = ctx;
    c->r = alarm_engine_user_set_as(e, c->src, c->as_user, c->user_no, c->new_code, c->authority,
                                    c->partitions, c->name, now);
}

alarm_result_t alarm_service_user_set_as(const alarm_source_t *src, uint8_t actor,
                                         uint8_t user_no, const char *new_code,
                                         alarm_authority_t authority, uint8_t partitions,
                                         const char *name)
{
    user_ctx_t c = {.src = src, .as_user = actor, .user_no = user_no, .new_code = new_code,
                    .authority = authority, .partitions = partitions, .name = name,
                    .r = not_run()};
    if (crypto_ready("setting a user code") != ALARM_OK) {
        return ALARM_ERR_UNAVAILABLE;
    }
    alarm_service_call(do_user_set_as, &c);
    return c.r;
}

static void do_user_update_as(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    user_ctx_t *c = ctx;
    c->r = alarm_engine_user_update_as(e, c->src, c->as_user, c->user_no, c->authority,
                                       c->partitions, c->name, now);
}

alarm_result_t alarm_service_user_update_as(const alarm_source_t *src, uint8_t actor,
                                            uint8_t user_no, alarm_authority_t authority,
                                            uint8_t partitions, const char *name)
{
    user_ctx_t c = {.src = src, .as_user = actor, .user_no = user_no, .authority = authority,
                    .partitions = partitions, .name = name, .r = not_run()};
    alarm_service_call(do_user_update_as, &c);
    return c.r;
}

static void do_user_delete_as(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    user_ctx_t *c = ctx;
    c->r = alarm_engine_user_delete_as(e, c->src, c->as_user, c->user_no, now);
}

alarm_result_t alarm_service_user_delete_as(const alarm_source_t *src, uint8_t actor,
                                            uint8_t user_no)
{
    user_ctx_t c = {.src = src, .as_user = actor, .user_no = user_no,
                    .r = not_run()};
    alarm_service_call(do_user_delete_as, &c);
    return c.r;
}

typedef struct {
    const alarm_source_t *src;
    const char *code;
    uint8_t user_no;
    uint8_t user;
    alarm_result_t r;
} program_ctx_t;

static void do_program_enter(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    program_ctx_t *c = ctx;
    c->r = alarm_engine_program_enter(e, c->src, c->code, now, &c->user);
}

alarm_result_t alarm_service_program_enter(const alarm_source_t *src, const char *code,
                                           uint8_t *user_out)
{
    program_ctx_t c = {.src = src, .code = code, .r = not_run()};
    /* An empty code is the power-up recovery window, which hashes nothing. */
    const bool hashes = code != NULL && code[0] != '\0';
    if (hashes) {
        const alarm_result_t ready = crypto_ready("checking a code");
        if (ready != ALARM_OK) {
            return ready;
        }
    }
    alarm_service_call(do_program_enter, &c);
    if (user_out != NULL) {
        *user_out = c.user;
    }
    return c.r;
}

static void do_program_enter_as(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    program_ctx_t *c = ctx;
    c->r = alarm_engine_program_enter_as(e, c->src, c->user, now);
}

alarm_result_t alarm_service_program_enter_as(const alarm_source_t *src, uint8_t user)
{
    program_ctx_t c = {.src = src, .user = user, .r = not_run()};
    alarm_service_call(do_program_enter_as, &c);
    return c.r;
}

static void do_program_exit(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    alarm_engine_program_exit(e, ((program_ctx_t *)ctx)->src, now);
}

void alarm_service_program_exit(const alarm_source_t *src)
{
    program_ctx_t c = {.src = src};
    alarm_service_call(do_program_exit, &c);
}

static void do_program_exit_owned(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    const alarm_source_t *src = ((program_ctx_t *)ctx)->src;
    if (src && e->program && e->program_src.kind == src->kind && e->program_src.id == src->id)
        alarm_engine_program_exit(e, src, now);
}

void alarm_service_program_exit_owned(const alarm_source_t *src)
{
    program_ctx_t c = {.src = src};
    alarm_service_call(do_program_exit_owned, &c);
}

static void do_program_keepalive(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)ctx;
    alarm_engine_program_keepalive(e, now);
}

void alarm_service_program_keepalive(void)
{
    alarm_service_call(do_program_keepalive, NULL);
}

static void do_program_set_code(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    program_ctx_t *c = ctx;
    c->r = alarm_engine_program_set_code(e, c->src, c->user_no, c->code, now);
}

alarm_result_t alarm_service_program_set_code(const alarm_source_t *src, uint8_t user_no,
                                              const char *new_code)
{
    program_ctx_t c = {.src = src, .code = new_code, .user_no = user_no,
                       .r = not_run()};
    if (crypto_ready("setting a user code") != ALARM_OK) {
        return ALARM_ERR_UNAVAILABLE;
    }
    alarm_service_call(do_program_set_code, &c);
    return c.r;
}

typedef struct {
    const alarm_keyseq_result_t *ks;
    const alarm_source_t *src;
    alarm_cmd_result_t *res;
    alarm_result_t r;
} keypad_ctx_t;

static void do_keypad(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    keypad_ctx_t *c = ctx;
    const alarm_keyseq_result_t *k = c->ks;
    switch (k->kind) {
    case ALARM_KS_COMMAND:
        c->r = alarm_engine_command(e, &k->cmd, c->src, now, c->res);
        break;
    case ALARM_KS_USER_SET: {
        const alarm_config_t *cfg = alarm_engine_config(e);
        const uint8_t part = (k->cmd.partition >= 1u && k->cmd.partition <= ALARM_MAX_PARTITIONS)
                                 ? k->cmd.partition
                                 : 1u;
        alarm_authority_t auth = ALARM_AUTH_STANDARD;
        uint8_t parts = (uint8_t)(1u << (part - 1u));
        const char *name = NULL; /* NULL keeps an existing user's name */
        char new_name[ALARM_NAME_LEN];
        if (k->user_no >= 1u && k->user_no <= ALARM_MAX_USERS &&
            cfg->users[k->user_no - 1u].authority != ALARM_AUTH_NONE) {
            auth = cfg->users[k->user_no - 1u].authority;
            parts = cfg->users[k->user_no - 1u].partitions;
        } else {
            snprintf(new_name, sizeof(new_name), "User %02u", (unsigned)k->user_no);
            name = new_name;
        }
        c->r = alarm_engine_user_set(e, c->src, k->cmd.code, k->user_no, k->new_code, auth, parts,
                                     name, now);
        break;
    }
    case ALARM_KS_USER_DELETE:
        c->r = alarm_engine_user_delete(e, c->src, k->cmd.code, k->user_no, now);
        break;
    default:
        c->r = ALARM_ERR_BAD_ARG;
        break;
    }
}

alarm_result_t alarm_service_keypad_execute(const alarm_keyseq_result_t *ks,
                                            const alarm_source_t *src,
                                            alarm_cmd_result_t *result)
{
    keypad_ctx_t c = {.ks = ks, .src = src, .res = result, .r = not_run()};
    const bool creates_code = ks != NULL && ks->kind == ALARM_KS_USER_SET;
    if (creates_code && crypto_ready("setting a user code") != ALARM_OK) {
        return ALARM_ERR_UNAVAILABLE;
    }
    alarm_service_call(do_keypad, &c);
    return c.r;
}

typedef struct {
    const alarm_config_t *cfg;
    alarm_result_t r;
} cfg_ctx_t;

static void do_set_config(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    cfg_ctx_t *c = ctx;
    uint8_t reset[ALARM_ZONE_BITMAP_BYTES] = {0};
    bool any = false;
    const alarm_config_t *old = alarm_engine_config(e);
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (alarm_zone_config_resets_state(&old->zones[z - 1u], &c->cfg->zones[z - 1u])) {
            reset[(z - 1u) / 8u] |= (uint8_t)(1u << ((z - 1u) % 8u));
            any = true;
        }
    }
    c->r = alarm_engine_set_config(e, c->cfg, now);
    if (c->r == ALARM_OK) {
        port_config_changed(NULL); /* the engine does not raise it for a whole-config write */
        for (size_t i = 0; any && i < s_listener_count; i++) {
            if (s_listeners[i].on_zones_reset != NULL) {
                s_listeners[i].on_zones_reset(s_listeners[i].ctx, reset);
            }
        }
    }
}

alarm_result_t alarm_service_set_config(const alarm_config_t *cfg)
{
    cfg_ctx_t c = {.cfg = cfg, .r = not_run()};
    alarm_service_call(do_set_config, &c);
    return c.r;
}

typedef struct {
    cfg_ctx_t config;
    const uint8_t *zones;
    alarm_config_t *scratch;
    unsigned n;
} voice_inv_ctx_t;
/* D07: copy, invalidate and apply in one alarm-task call, so no edit can land between the read
 * and the write and be reverted; the write is set_config's own (its zone resets, the config
 * bookkeeping and the store after_engine() runs before this call returns). */
static void do_voice_invalidate(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    voice_inv_ctx_t *c = ctx;
    memcpy(c->scratch, alarm_engine_config(e), sizeof(*c->scratch));
    c->n = alarm_config_voice_invalidate_zones(c->scratch, c->zones);
    if (c->n == 0u) {
        c->config.r = ALARM_OK; /* none in use: no approval to lose, nothing to write */
        return;
    }
    c->config.cfg = c->scratch;
    do_set_config(e, now, &c->config);
}

alarm_result_t alarm_service_voice_invalidate(const uint8_t *zones, alarm_config_t *scratch,
                                              unsigned *written)
{
    if (written != NULL) {
        *written = 0u;
    }
    if (zones == NULL || scratch == NULL) {
        return ALARM_ERR_BAD_ARG;
    }
    voice_inv_ctx_t c = {.config = {.cfg = NULL, .r = not_run()}, .zones = zones,
                         .scratch = scratch, .n = 0u};
    alarm_service_call(do_voice_invalidate, &c);
    if (written != NULL && c.config.r == ALARM_OK) {
        *written = c.n;
    }
    return c.config.r;
}

typedef struct {
    cfg_ctx_t config;
    const alarm_source_t *src;
    uint8_t user;
    uint32_t revision;
} remote_cfg_ctx_t;
/* CORE-PROG-011: the signed-in installer's programming, one alarm-task transaction. */
static void do_program_apply_as(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    remote_cfg_ctx_t *c = ctx;
    if (e->program || c->revision != s_config_seq) {
        c->config.r = ALARM_ERR_BAD_STATE;
        return;
    }
    c->config.r = alarm_engine_program_enter_as(e, c->src, c->user, now);
    if (c->config.r == ALARM_OK) {
        do_set_config(e, now, &c->config);
        alarm_engine_program_exit(e, c->src, now);
    }
}
alarm_result_t alarm_service_program_apply_as(const alarm_source_t *src, uint8_t user,
    uint32_t expected_revision, const alarm_config_t *cfg)
{
    remote_cfg_ctx_t c = {.config = {.cfg=cfg, .r=not_run()}, .src=src,
        .user=user, .revision=expected_revision};
    alarm_service_call(do_program_apply_as, &c);
    return c.config.r;
}

static void do_get_config(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    memcpy(ctx, alarm_engine_config(e), sizeof(alarm_config_t));
}

esp_err_t alarm_service_get_config(alarm_config_t *out)
{
    return alarm_service_call(do_get_config, out);
}

uint32_t alarm_service_snapshot_seq(void)
{
    return __atomic_load_n(&s_snap.seq, __ATOMIC_RELAXED);
}

/* Safe before (and without) a start: a panel whose storage could not be read still brings the
 * service console up, and asking it for the state must report "nothing" rather than take a
 * lock that was never created. */
void alarm_service_get_snapshot(alarm_snapshot_t *out)
{
    if (s_lock == NULL || dormant()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_snap;
    unlock();
}

size_t alarm_service_recent_events(alarm_event_t *out, size_t max)
{
    if (s_lock == NULL || dormant()) {
        return 0u;
    }
    lock();
    size_t n = (max < s_ring_count) ? max : s_ring_count;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(s_ring_head + EVENT_RING - 1u - i) % EVENT_RING];
    }
    unlock();
    return n;
}

typedef struct {
    const alarm_source_t *src;
    uint8_t user;
    bool erase_log;
} reset_ctx_t;

static void do_factory_reset(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    const reset_ctx_t *c = ctx;
    alarm_engine_platform_event(e, ALARM_EV_FACTORY_RESET, c->erase_log ? 1u : 0u, c->src,
                                c->user, now);
    s_frozen = true; /* from here on nothing is written back to NVS */
}

esp_err_t alarm_service_factory_reset(const alarm_source_t *src, uint8_t user, bool erase_log)
{
    reset_ctx_t c = {.src = src, .user = user, .erase_log = erase_log};
    return alarm_service_call(do_factory_reset, &c);
}

typedef struct {
    bool restoring;
    const alarm_source_t *src;
    uint8_t user;
    alarm_result_t result;
} backup_ctx_t;

static void do_backup_event(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    backup_ctx_t *c = ctx;
    c->result = alarm_engine_platform_event(
        e, c->restoring ? ALARM_EV_BACKUP_RESTORED : ALARM_EV_BACKUP_MADE, 0u, c->src, c->user,
        now);
    if (c->restoring) {
        s_frozen = true; /* the file's configuration goes in next: nothing of ours over it */
    }
}

esp_err_t alarm_service_backup_event(bool restoring, const alarm_source_t *src, uint8_t user)
{
    static const alarm_source_t system = {.kind = ALARM_SRC_SYSTEM, .id = 0};
    backup_ctx_t c = {.restoring = restoring, .src = src != NULL ? src : &system, .user = user};
    ESP_RETURN_ON_ERROR(alarm_service_call(do_backup_event, &c), TAG, "backup event");
    return c.result == ALARM_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}

typedef struct {
    alarm_fw_stage_t stage;
    const alarm_source_t *src;
    uint8_t user;
    alarm_result_t result;
} firmware_ctx_t;

static void do_firmware_event(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    firmware_ctx_t *c = ctx;
    c->result = alarm_engine_platform_event(e, ALARM_EV_FIRMWARE_UPDATE, (uint8_t)c->stage,
                                            c->src, c->user, now);
}

esp_err_t alarm_service_firmware_event(alarm_fw_stage_t stage, const alarm_source_t *src,
                                       uint8_t user)
{
    static const alarm_source_t system = {.kind = ALARM_SRC_SYSTEM, .id = 0};
    firmware_ctx_t c = {.stage = stage, .src = src != NULL ? src : &system, .user = user};
    ESP_RETURN_ON_ERROR(alarm_service_call(do_firmware_event, &c), TAG, "firmware event");
    return c.result == ALARM_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}

typedef struct {
    alarm_event_type_t type;
    uint16_t zone;
    uint8_t arg, arg2;
    const alarm_source_t *src;
    uint8_t user;
    alarm_result_t result;
} platform_ctx_t;

static void do_platform_event(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    platform_ctx_t *c = ctx;
    c->result = alarm_engine_platform_event_ex(e, c->type, c->zone, c->arg, c->arg2, c->src,
                                               c->user, now);
}

esp_err_t alarm_service_platform_event(alarm_event_type_t type, uint16_t zone, uint8_t arg,
                                       uint8_t arg2, const alarm_source_t *src, uint8_t user)
{
    static const alarm_source_t system = {.kind = ALARM_SRC_SYSTEM, .id = 0};
    platform_ctx_t c = {.type = type, .zone = zone, .arg = arg, .arg2 = arg2,
                        .src = src != NULL ? src : &system, .user = user};
    ESP_RETURN_ON_ERROR(alarm_service_call(do_platform_event, &c), TAG, "platform event");
    return c.result == ALARM_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}

typedef struct {
    uint16_t uid;
    uint8_t partition;
    uint8_t user;
    alarm_put_off_t choice;
    int64_t until_epoch_ms;
    alarm_result_t result;
} put_off_ctx_t;

static void do_arming_put_off(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    put_off_ctx_t *c = ctx;
    c->result = alarm_engine_arming_put_off(e, c->partition, c->uid, c->user, c->choice,
                                            c->until_epoch_ms, now);
}

esp_err_t alarm_service_arming_put_off(uint16_t uid, uint8_t partition, uint8_t user,
                                       alarm_put_off_t choice, int64_t until_epoch_ms)
{
    put_off_ctx_t c = {.uid = uid, .partition = partition, .user = user, .choice = choice,
                       .until_epoch_ms = until_epoch_ms};
    ESP_RETURN_ON_ERROR(alarm_service_call(do_arming_put_off, &c), TAG, "put-off event");
    return c.result == ALARM_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}

typedef struct {
    uint16_t uid;
    uint8_t partition;
    uint16_t zone;
    uint8_t arg;
    uint8_t arg2;
    alarm_result_t result;
} auto_notice_ctx_t;

static void do_automation_notice(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    auto_notice_ctx_t *c = ctx;
    c->result = alarm_engine_automation_notice(e, c->uid, c->partition, c->zone, c->arg, c->arg2,
                                               now);
}

esp_err_t alarm_service_automation_notice(uint16_t uid, uint8_t partition, uint16_t zone,
                                          uint8_t arg, uint8_t arg2)
{
    auto_notice_ctx_t c = {.uid = uid, .partition = partition, .zone = zone, .arg = arg,
                           .arg2 = arg2, .result = ALARM_ERR_BAD_ARG};
    ESP_RETURN_ON_ERROR(alarm_service_call(do_automation_notice, &c), TAG, "automation notice");
    return c.result == ALARM_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}
