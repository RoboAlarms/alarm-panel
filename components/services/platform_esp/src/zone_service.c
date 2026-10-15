/*
 * zone_service.c - the zone hub on the panel (see zone_service.h and docs/features/22).
 *
 * The hub lives on its own task ("zones") behind a recursive lock: sources report from their
 * own tasks (the console) or from inside the hub's tick (the wired terminals), the UI and the
 * console query and bind from theirs. The hub's zone events go to the alarm task's queue; the
 * alarm task never waits for this lock.
 */
#include "zone_service.h"
#include "alarm_mem.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "zone_hub/zone_voice_epoch.h"
#include "zone_hub/zsrc_ha.h"
#include "zone_hub/zsrc_sim.h"

static const char *TAG = "zones";
static const char *NS = "zones";
static const char *KEY = "bind";

/* The task's longest sleep. It is not only about the hub's own deadlines: a pass that stops
 * happening is how the panel notices that zone source processing has stopped, so the counter
 * below has to keep moving well inside PANEL_OTA_HEALTH_PERIOD_MS (docs/features/13 OTA-005). */
#define MAX_WAIT_MS 4000u

static zone_hub_t *s_hub;
static SemaphoreHandle_t s_lock; /* recursive: sources report from inside the hub's tick */
static TaskHandle_t s_task;
static bool s_ready;    /* the hub, its sources and its task are all up (R07) */
static uint32_t s_passes; /* completed hub ticks: proof that zone sources are still processed */
#if CONFIG_ALARM_ZSRC_SIM
static zsrc_sim_t s_sim;
#endif
#if CONFIG_ALARM_ZSRC_WIRED
static zsrc_wired_t s_wired;
#endif
#if CONFIG_ALARM_ZSRC_HA
/* Task-only catalog/cache, protected by s_lock; never used from a cache-disabled ISR. */
static ALARM_SLOW_BSS zsrc_ha_t s_ha;
#endif
static bool s_changed;
static uint32_t s_learning; /* sources listening for sensors, bit = zsrc_kind_t */

/* Why the panel's sensor input can't be trusted right now, all in one place so the two ways it
 * can happen - a source's link that has been down too long (ZSRC-015), and this service itself
 * no longer running (docs/features/13 OTA-005) - raise and clear one trouble between them. */
#define SENSOR_FAULT_STOPPED (1u << 31) /* the hub's task stopped making progress */
#define SENSOR_FAULT_LOST    (1u << 30) /* a source lost an alarm-path event */
static uint32_t s_sensor_fault;         /* other bits: 1 << zsrc_kind_t */
static uint32_t s_lost_kinds;           /* 1 << zsrc_kind_t: sources that lost one */
static bool s_sensor_trouble;           /* what the engine was last told */
#if CONFIG_ALARM_ZSRC_HA
static bool s_ha_link_up;               /* what the hub was last told about the HA link */
#endif

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_reset[ALARM_ZONE_BITMAP_BYTES]; /* zones the engine forgot, for the task */
static bool s_reset_pending;
/* A change made by a task that still held the lock (a source's own hook: Zigbee's rebind): that
 * task, and only it (another may run on a PSRAM stack, which must never write NVS), saves it
 * when it lets the lock go (zone_service_unlock), outside it. */
static TaskHandle_t s_save_task;

/* A new sensor invalidates its zone's spoken-name approval before its binding is committed
 * (docs/features/08 voice-first-build.md D07, CORE-CFG-021). The ordering, per zone:
 *  1. under s_lock, before the hub takes a changed sensor (bind or unbind), its source-change
 *     epoch moves on (zone_voice_note_change) and s_settling's bit goes up: from here approval
 *     and automatic zone speech refuse the zone (zone_service_voice_settling, lock-free);
 *  2. without s_lock, under s_store_lock: the epochs owed are captured, then one alarm-task
 *     transaction (alarm_service_voice_invalidate) copies the running configuration,
 *     invalidates them and applies it, and the store is checked durable; only then are the
 *     captured epochs marked (zone_voice_invalidated) - a zone changed again meanwhile (a
 *     second Zigbee rebind) still owes one;
 *  3. save() refuses to encode while any zone owes an invalidation, captures every epoch with
 *     the encode, and marks them stored only once NVS has committed; the bit comes down when a
 *     zone's epoch is both invalidated and stored.
 * change() from a task without s_lock runs 1-3 in turn (a refused invalidation withdraws its
 * epoch and refuses the bind: BUSY); a change made under the caller's own hold (Zigbee's
 * replacement) can't call the alarm task, so it does 1 and leaves 2-3 to zone_service_unlock.
 * A refused invalidation there leaves the bindings unstored (a restart brings back the old
 * sensor with its own approval) and the zone task tries again (s_store_owed). s_store_lock is
 * taken only without s_lock, never the other way round; the alarm task never waits for either. */
static SemaphoreHandle_t s_store_lock;
static ALARM_SLOW_BSS zone_voice_epochs_t s_voice; /* under s_lock; 2 and 3 under s_store_lock */
static uint8_t s_settling[ALARM_ZONE_BITMAP_BYTES]; /* bit zone - 1; atomic, set under s_lock */
static bool s_store_owed;                            /* atomic */
#define STORE_RETRY_MS 5000u

/* s_lock held: what s_voice says, for the readers that take no lock. */
static void publish_settling(void)
{
    for (size_t i = 0; i < ALARM_ZONE_BITMAP_BYTES; i++) {
        uint8_t b = 0u;
        for (unsigned k = 0; k < 8u && i * 8u + k < ALARM_MAX_ZONES; k++) {
            if (zone_voice_settling(&s_voice, (uint16_t)(i * 8u + k + 1u))) {
                b |= (uint8_t)(1u << k);
            }
        }
        __atomic_store_n(&s_settling[i], b, __ATOMIC_RELEASE);
    }
}

static bool ready(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE);
}

static void lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void wake(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task); /* deadlines may have moved */
    }
}

static uint64_t now_ms(void)
{
    return alarm_service_now();
}

/* ---- the hub's port ------------------------------------------------------------------------ */

static void hub_zone_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    alarm_service_zone_event(zone, ev);
}

static void hub_changed(void *ctx)
{
    (void)ctx;
    s_changed = true;
}

/* One system trouble for "the panel's sensors may not be heard", whatever the reason. It is
 * raised from the zone task or a source's task, never from the alarm task, and
 * alarm_service_system_trouble() only queues, so it never re-enters the engine.
 *
 * ALARM_TRBL_SENSOR_RADIO is an existing value whose words on screen ("Sensor radio problem -
 * wireless sensors may not be heard") describe exactly this; docs/features/22 plans a supervised
 * device per source link instead, which needs installer-configurable devices the panel does not
 * have yet. See docs/features/22 ZSRC-015. */
/* Lock-free: the bits are set from the zone task (under the hub's lock) and from the panel's
 * health task, and neither may wait for the other - a health task blocked on a wedged zone task
 * would stop reporting every other part of the panel too. The exchange makes exactly one caller
 * responsible for each transition. */
static void set_sensor_fault(uint32_t bit, bool on)
{
    if (on) {
        __atomic_or_fetch(&s_sensor_fault, bit, __ATOMIC_RELAXED);
    } else {
        __atomic_and_fetch(&s_sensor_fault, ~bit, __ATOMIC_RELAXED);
    }
    const bool want = __atomic_load_n(&s_sensor_fault, __ATOMIC_RELAXED) != 0u;
    if (__atomic_exchange_n(&s_sensor_trouble, want, __ATOMIC_ACQ_REL) != want) {
        alarm_service_system_trouble(ALARM_TRBL_SENSOR_RADIO, want);
    }
}

/* ZSRC-015: a source's whole link has been down longer than its grace while zones depend on
 * it. One trouble for the source, never a CHECK per zone - the hub decides when. */
static void hub_source_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    ESP_LOGW(TAG, "source %u: its link has been down too long (%s)", kind,
             on ? "sensors on it may not be heard" : "back");
    set_sensor_fault(kind < 31u ? (1u << kind) : 0u, on);
}

/* The panel's health check says whether this service is still processing its sources. Folded
 * into the same trouble so there is one message, not two (R18 / ZSRC-015). */
void zone_service_report_health(bool running)
{
    const uint32_t had = __atomic_load_n(&s_sensor_fault, __ATOMIC_RELAXED);
    if (((had & SENSOR_FAULT_STOPPED) != 0u) == !running) {
        return; /* called every couple of seconds: say nothing while nothing changed */
    }
    ESP_LOGE(TAG, "zone source processing %s", running ? "is running again" : "has stopped");
    set_sensor_fault(SENSOR_FAULT_STOPPED, !running);
}

/* Folded into the same trouble too: one message whatever the reason (ZSRC-015). */
void zone_service_report_lost(uint8_t kind, bool on)
{
    const uint32_t bit = kind < 30u ? (1u << kind) : 0u;
    const uint32_t now = on ? __atomic_or_fetch(&s_lost_kinds, bit, __ATOMIC_RELAXED)
                            : __atomic_and_fetch(&s_lost_kinds, ~bit, __ATOMIC_RELAXED);
    ESP_LOGW(TAG, "source %u: %s", kind,
             on ? "an event that may have carried an alarm was lost" : "no loss for a while");
    set_sensor_fault(SENSOR_FAULT_LOST, now != 0u);
}

/* ---- the sources' sink --------------------------------------------------------------------- */

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                       uint64_t now)
{
    (void)ctx;
    lock();
    zone_hub_input(s_hub, a, state, value, now);
    unlock();
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    lock();
    zone_hub_event(s_hub, a, ev, now);
    unlock();
    wake();
}

static void sink_reachable(void *ctx, const zsrc_address_t *a, bool reachable, uint64_t now)
{
    (void)ctx;
    lock();
    zone_hub_reachable(s_hub, a, reachable, now);
    unlock();
    wake();
}

static void sink_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    lock();
    zone_hub_link(s_hub, kind, up, now);
    unlock();
    wake();
}

static void sink_link_now(void *ctx, uint8_t kind, uint64_t now)
{
    (void)ctx;
    lock();
    zone_hub_link_now(s_hub, kind, now);
    unlock();
    wake();
}

/* ---- wired terminals: ESP-IDF gpio --------------------------------------------------------- */

#if CONFIG_ALARM_ZSRC_WIRED
static bool gpio_setup(void *ctx, uint8_t gpio, bool on)
{
    (void)ctx;
    if (!on) {
        return gpio_reset_pin((gpio_num_t)gpio) == ESP_OK;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ull << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io) == ESP_OK;
}

static uint8_t gpio_read(void *ctx, uint8_t gpio)
{
    (void)ctx;
    return gpio_get_level((gpio_num_t)gpio) != 0 ? 1u : 0u;
}
#endif

/* ---- storage ------------------------------------------------------------------------------- */

static esp_err_t save(void);

/* Encodes with the lock held, writes without it: an NVS commit takes tens of milliseconds and
 * must not hold up the terminals' sampling or the zone editor. */
static esp_err_t save(void)
{
    uint8_t *buf = heap_caps_malloc(ZONE_HUB_BLOB_MAX, MALLOC_CAP_DEFAULT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    size_t len = 0;
    esp_err_t err = ESP_OK;
    if (zone_voice_any_owed(&s_voice)) {
        err = ESP_ERR_NOT_FINISHED; /* D07: a zone's approval is not invalidated yet */
    } else {
        zone_voice_capture(&s_voice); /* the epochs this encode holds */
        if (zone_hub_encode(s_hub, buf, ZONE_HUB_BLOB_MAX, &len) != ZONE_HUB_OK) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    unlock();
    if (err == ESP_ERR_NOT_FINISHED) {
        heap_caps_free(buf);
        return err;
    }
    if (err == ESP_OK) {
        nvs_handle_t h;
        err = nvs_open(NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, KEY, buf, len);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err == ESP_OK) {
            lock();
            zone_voice_stored(&s_voice); /* durable: those zones stop settling (D07) */
            publish_settling();
            unlock();
        }
    }
    heap_caps_free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the zone bindings failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* D07: invalidate the spoken-name approval of these zones (bit zone - 1) in one alarm-task
 * transaction on the running configuration, stored before this returns true. Zones not in use
 * have no approval to lose. Called without s_lock. False: refused (a partition not idle, no
 * memory) or not on flash yet. */
static bool invalidate_voice(const uint8_t *zones)
{
    alarm_config_t *cfg = alarm_mem_prefer_slow_calloc(1, sizeof(*cfg)); /* ~10 KB scratch */
    if (cfg == NULL) {
        ESP_LOGE(TAG, "no memory to invalidate a spoken name in");
        return false;
    }
    unsigned n = 0u;
    const alarm_result_t r = alarm_service_voice_invalidate(zones, cfg, &n);
    const bool ok = r == ALARM_OK && (n == 0u || alarm_service_store_status() == ESP_OK);
    if (!ok) {
        ESP_LOGW(TAG, "a new sensor's spoken-name invalidation not stored (%d): the binding "
                      "waits", (int)r);
    }
    free(cfg);
    return ok;
}

/* s_store_lock held, s_lock not: every zone that owes an invalidation, invalidated (D07). Only
 * the epochs captured here are marked; a change made meanwhile still owes one. */
static bool flush_voice(void)
{
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES];
    lock();
    const bool any = zone_voice_owed(&s_voice, zones);
    unlock();
    if (!any) {
        return true;
    }
    if (!invalidate_voice(zones)) {
        return false;
    }
    lock();
    zone_voice_invalidated(&s_voice, zones);
    publish_settling();
    unlock();
    return true;
}

/* s_store_lock held, s_lock not: invalidate what is pending, then store the bindings. A zone
 * that a source's hold changes in between is caught by save() and invalidated on the next
 * round. */
static void store_bindings(void)
{
    for (int i = 0; i < 3; i++) {
        if (!flush_voice()) {
            break;
        }
        const esp_err_t err = save();
        if (err == ESP_ERR_NOT_FINISHED) {
            continue;
        }
        __atomic_store_n(&s_store_owed, false, __ATOMIC_RELEASE);
        if (err != ESP_OK) {
            /* A failed save: it works now, not after a restart; held until restart (ZW-039:
             * the zones' own hold). */
            alarm_service_storage_trouble(ALARM_STORAGE_ZONES, true);
        }
        return;
    }
    __atomic_store_n(&s_store_owed, true, __ATOMIC_RELEASE); /* the zone task tries again */
}

/* ESP_ERR_NVS_NOT_FOUND: nothing stored yet (the first start with zone sources). */
static esp_err_t load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = 0;
    uint8_t *buf = NULL;
    err = nvs_get_blob(h, KEY, NULL, &len);
    if (err == ESP_OK) {
        buf = heap_caps_malloc(len > 0u ? len : 1u, MALLOC_CAP_DEFAULT);
        err = buf != NULL ? nvs_get_blob(h, KEY, buf, &len) : ESP_ERR_NO_MEM;
    }
    nvs_close(h);
    if (err == ESP_OK && zone_hub_decode(s_hub, buf, len) != ZONE_HUB_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    heap_caps_free(buf);
    return err;
}

/* The stored bindings are unreadable: the zones have no sensor, so they must show CHECK
 * instead of looking ready while nothing can ever fault them (docs/features/22 ZSRC-010). */
static void lost_bindings(void)
{
    alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_DEFAULT);
    if (cfg == NULL || alarm_service_get_config(cfg) != ESP_OK) {
        heap_caps_free(cfg);
        return;
    }
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    unsigned n = 0;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED) {
            zones[(z - 1u) / 8u] |= (uint8_t)(1u << ((z - 1u) % 8u));
            n++;
        }
    }
    heap_caps_free(cfg);
    zone_hub_lost(s_hub, zones, now_ms());
    ESP_LOGE(TAG, "%u zones have no sensor until one is picked again: they show CHECK", n);
}

/* Before zone sources, every zone was driven from the console: keep it that way. */
static void migrate(void)
{
    unsigned n = 0;
#if CONFIG_ALARM_ZSRC_SIM
    alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_DEFAULT);
    if (cfg != NULL && alarm_service_get_config(cfg) == ESP_OK) {
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (cfg->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
                continue;
            }
            zone_hub_binding_t b;
            memset(&b, 0, sizeof(b));
            b.in[ZSRC_ROLE_MAIN].addr = zsrc_sim_address(z);
            n += zone_hub_bind(s_hub, z, &b, now_ms()) == ZONE_HUB_OK;
        }
    }
    heap_caps_free(cfg);
#endif
    save(); /* also when empty: the first start is done */
    ESP_LOGI(TAG, "first start with zone sources: %u zones bound to the simulator", n);
}

/* ---- configuration changes and the task ------------------------------------------------------ */

static void on_zones_reset(void *ctx, const uint8_t *zones)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < ALARM_ZONE_BITMAP_BYTES; i++) {
        s_reset[i] |= zones[i];
    }
    s_reset_pending = true;
    portEXIT_CRITICAL(&s_mux);
    wake();
}

static const alarm_listener_t LISTENER = {.on_zones_reset = on_zones_reset};

const alarm_listener_t *zone_service_listener(void)
{
    return &LISTENER;
}

static void zone_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t reset[ALARM_ZONE_BITMAP_BYTES];
        bool resync = false;
        portENTER_CRITICAL(&s_mux);
        if (s_reset_pending) {
            memcpy(reset, s_reset, sizeof(reset));
            memset(s_reset, 0, sizeof(s_reset));
            s_reset_pending = false;
            resync = true;
        }
        portEXIT_CRITICAL(&s_mux);

        lock();
        const uint64_t now = now_ms();
        if (resync) {
            zone_hub_resync(s_hub, reset, now);
        }
        zone_hub_tick(s_hub, now);
        const uint64_t next = zone_hub_next_deadline(s_hub, now_ms());
        unlock();
        /* D07: bindings a refused invalidation left unstored. This task's stack is internal
         * RAM, so it may write NVS; the rare retry holds up the next pass by one commit. */
        static uint64_t s_retry_at;
        if (__atomic_load_n(&s_store_owed, __ATOMIC_ACQUIRE) && now_ms() >= s_retry_at &&
            xSemaphoreTake(s_store_lock, 0) == pdTRUE) {
            s_retry_at = now_ms() + STORE_RETRY_MS;
            if (__atomic_load_n(&s_store_owed, __ATOMIC_ACQUIRE)) {
                store_bindings();
            }
            xSemaphoreGive(s_store_lock);
        }
        /* One completed pass over the sources. Nothing here needs a sensor to change, so a
         * panel with a quiet house still proves it is processing its zone sources. */
        __atomic_add_fetch(&s_passes, 1u, __ATOMIC_RELAXED);
#if ALARM_BENCH_FAULT_ZONES_STOP
        /* Bench fault image (cmake/alarm_firmware.cmake): the task stops for good three minutes
         * in, after the update's self-test; the health check must report it. */
        if (now_ms() > 180000u) {
            ESP_LOGE(TAG, "BENCH FAULT IMAGE: the zone service's task stops now, on purpose");
            vTaskSuspend(NULL);
        }
#endif

        TickType_t wait = pdMS_TO_TICKS(MAX_WAIT_MS);
        if (next != UINT64_MAX) {
            const uint64_t t = now_ms();
            const uint64_t ms = next > t ? next - t : 0u;
            wait = pdMS_TO_TICKS((uint32_t)(ms < MAX_WAIT_MS ? ms : MAX_WAIT_MS));
        }
        ulTaskNotifyTake(pdTRUE, wait > 0 ? wait : 1);
    }
}

/* R07: the hub and its lock are built privately and only published once they exist, and the
 * task is created before any source is registered - so a failure at any step gives everything
 * back with nothing half-started watching a hub that is about to be freed, and the call can
 * simply be made again. */
esp_err_t zone_service_start(const zone_service_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_hub == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    SemaphoreHandle_t lk = xSemaphoreCreateRecursiveMutex();
    if (s_store_lock == NULL) {
        s_store_lock = xSemaphoreCreateMutex(); /* kept if a later step fails: reused */
        zone_voice_init(&s_voice); /* what is loaded is stored: nothing owed or settling */
    }
    zone_hub_t *hub = alarm_mem_prefer_slow_calloc(1, sizeof(*hub));
    if (lk == NULL || hub == NULL || s_store_lock == NULL) {
        if (lk != NULL) {
            vSemaphoreDelete(lk);
        }
        heap_caps_free(hub);
        ESP_LOGE(TAG, "not enough memory for the zone hub");
        return ESP_ERR_NO_MEM;
    }

    const zone_hub_port_t port = {.ctx = NULL, .zone_event = hub_zone_event,
                                  .changed = hub_changed,
                                  .source_trouble = hub_source_trouble};
    zone_hub_init(hub, &port);
    /* The sinks and the task reach the hub through the globals; nothing outside can call in
     * until a source is registered, which happens after the task exists. */
    s_lock = lk;
    s_hub = hub;
    if (xTaskCreatePinnedToCore(zone_task, "zones", 6144, NULL, CONFIG_ALARM_TASK_PRIORITY - 1,
                                &s_task, tskNO_AFFINITY) != pdPASS) {
        s_hub = NULL;
        s_lock = NULL;
        s_task = NULL;
        vSemaphoreDelete(lk);
        heap_caps_free(hub);
        ESP_LOGE(TAG, "no task for the zone hub");
        return ESP_ERR_NO_MEM;
    }

    const zsrc_sink_t sink = {.ctx = NULL, .input = sink_input, .event = sink_event,
                              .reachable = sink_reachable, .link = sink_link,
                              .link_now = sink_link_now};
    lock(); /* the task is already ticking the hub: registration goes under the same lock */
    const uint64_t now = now_ms();
#if CONFIG_ALARM_ZSRC_SIM
    zsrc_sim_init(&s_sim, &sink);
    zone_hub_register(s_hub, zsrc_sim_ops(), &s_sim, now);
#endif
#if CONFIG_ALARM_ZSRC_WIRED
    if (cfg != NULL && cfg->terminals != NULL && cfg->terminal_count > 0u) {
        const zsrc_gpio_port_t gpio = {.ctx = NULL, .setup = gpio_setup, .read = gpio_read};
        zsrc_wired_init(&s_wired, cfg->terminals, cfg->terminal_count, &gpio, &sink);
        zone_hub_register(s_hub, zsrc_wired_ops(), &s_wired, now);
    }
#else
    (void)cfg;
#endif
#if CONFIG_ALARM_ZSRC_HA
    zsrc_ha_init(&s_ha, &sink);
    zone_hub_register(s_hub, zsrc_ha_ops(), &s_ha, now);
#endif

    const esp_err_t err = load();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        migrate();
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "stored zone bindings unusable (%s): the stored copy is kept until a "
                      "binding changes", esp_err_to_name(err));
        alarm_service_storage_trouble(ALARM_STORAGE_ZONES, true); /* held until restart */
        lost_bindings();
    } else if (s_hub->keep_lost != 0u) {
        ESP_LOGW(TAG, "%u bytes of bindings from a newer firmware don't fit: kept in storage "
                      "until a binding changes", (unsigned)s_hub->keep_lost);
    }
    s_changed = false;
    zone_hub_start(s_hub, now_ms());
    unsigned bound = 0;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        bound += zone_hub_get(s_hub, z, &b);
    }
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE); /* everything above is in place */
    unlock();
    wake();

    ESP_LOGI(TAG, "zone sources running: %u zones with a sensor (hub %u bytes)", bound,
             (unsigned)sizeof(*s_hub));
    return ESP_OK;
}

uint32_t zone_service_passes(void)
{
    return __atomic_load_n(&s_passes, __ATOMIC_RELAXED);
}

/* ---- the simulator ------------------------------------------------------------------------- */

bool zone_service_sim(uint16_t input, alarm_zone_event_t ev)
{
#if CONFIG_ALARM_ZSRC_SIM
    if (!ready()) {
        return false;
    }
    lock();
    const zsrc_address_t a = zsrc_sim_address(input);
    const bool bound = zone_hub_zone_of(s_hub, &a) != 0u;
    if (bound) {
        zsrc_sim_event(&s_sim, input, ev, now_ms());
    }
    unlock();
    wake();
    return bound;
#else
    (void)input;
    (void)ev;
    return false;
#endif
}

/* ---- the Home Assistant source (fed by ha_link, docs/features/23 M13) ----------------------------- */

/* Edge-triggered on purpose (R03). ha_link reports a connection going down from every exit
 * path, and a connection that *pairs* mid-session reaches that exit without ever having
 * reported itself up - so a bare "down" can arrive with no "up" before it. Forwarding it would
 * restart the hub's supervision clock for a link that was already known to be down and leave a
 * permanent trouble after any mid-session pairing. Only a real change is passed on, which also
 * makes this safe to call as often as ha_link likes. */
void zone_service_ha_link_state(bool up)
{
#if CONFIG_ALARM_ZSRC_HA
    if (!ready()) {
        return;
    }
    lock();
    const bool changed = up != s_ha_link_up;
    if (changed) {
        s_ha_link_up = up;
        zsrc_ha_link(&s_ha, up, now_ms());
    }
    unlock();
    if (changed) {
        wake();
    }
#else
    (void)up;
#endif
}

void zone_service_ha_catalog_begin(void)
{
#if CONFIG_ALARM_ZSRC_HA
    if (!ready()) {
        return;
    }
    lock();
    zsrc_ha_catalog_begin(&s_ha);
    unlock();
#endif
}

void zone_service_ha_catalog_add(const zsrc_ha_entity_t *e)
{
#if CONFIG_ALARM_ZSRC_HA
    if (!ready()) {
        return;
    }
    lock();
    zsrc_ha_catalog_add(&s_ha, e);
    unlock();
#else
    (void)e;
#endif
}

void zone_service_ha_state(const char *entity_id, const char *state, bool available)
{
#if CONFIG_ALARM_ZSRC_HA
    if (!ready()) {
        return;
    }
    lock();
    zsrc_ha_state(&s_ha, entity_id, state, available, now_ms());
    unlock();
    wake();
#else
    (void)entity_id;
    (void)state;
    (void)available;
#endif
}

size_t zone_service_ha_watch_list(char out[][ZSRC_REF_LEN], size_t max)
{
#if CONFIG_ALARM_ZSRC_HA
    if (!ready()) {
        return 0u;
    }
    const char *refs[ZSRC_HA_WATCH_MAX];
    lock();
    size_t n = zsrc_ha_watch_list(&s_ha, refs, ZSRC_HA_WATCH_MAX);
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        snprintf(out[i], ZSRC_REF_LEN, "%s", refs[i]); /* copied: refs die with the lock */
    }
    unlock();
    return n;
#else
    (void)out;
    (void)max;
    return 0u;
#endif
}

void zone_service_ha_set_watch_changed(void (*fn)(void *ctx), void *ctx)
{
#if CONFIG_ALARM_ZSRC_HA
    if (!ready()) {
        return; /* nothing is watching anything: the hook would have nothing to report */
    }
    /* the callback fires under the zone lock, from a bind or unbind: it must only set a
     * flag (ha_link's task picks it up), never call back into zone_service or ha_link */
    lock();
    zsrc_ha_set_watch_changed(&s_ha, fn, ctx);
    unlock();
#else
    (void)fn;
    (void)ctx;
#endif
}

/* ---- the zone editor and the console --------------------------------------------------------- */

/* A source owned elsewhere (the sensor radio, docs/features/20) joining the hub. */
zone_hub_result_t zone_service_register(const zone_source_ops_t *ops, void *ctx)
{
    if (!ready()) {
        return ZONE_HUB_ERR_ARG;
    }
    lock();
    const zone_hub_result_t r = zone_hub_register(s_hub, ops, ctx, now_ms());
    unlock();
    wake();
    return r;
}

zsrc_sink_t zone_service_sink(void)
{
    const zsrc_sink_t sink = {.ctx = NULL, .input = sink_input, .event = sink_event,
                              .reachable = sink_reachable, .link = sink_link,
                              .link_now = sink_link_now};
    return sink;
}

void zone_service_lock(void)
{
    lock();
}

void zone_service_unlock(void)
{
    unlock();
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (xSemaphoreGetMutexHolder(s_lock) != me &&
        __atomic_compare_exchange_n(&s_save_task, &me, NULL, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        /* A change made under this hold, stored now it is let go: its zone's spoken-name
         * approval is invalidated first (D07), now that the alarm task may be asked. */
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        store_bindings();
        xSemaphoreGive(s_store_lock);
    }
}

void zone_service_wake(void)
{
    wake();
}

size_t zone_service_sources(zone_hub_source_info_t *out, size_t max)
{
    if (!ready()) {
        return 0u;
    }
    lock();
    const size_t n = zone_hub_sources(s_hub, out, max);
    unlock();
    return n;
}

bool zone_service_voice_settling(uint16_t zone)
{
    if (zone < 1u || zone > ALARM_MAX_ZONES) {
        return false;
    }
    return (__atomic_load_n(&s_settling[(zone - 1u) / 8u], __ATOMIC_ACQUIRE) &
            (1u << ((zone - 1u) % 8u))) != 0u;
}

bool zone_service_all_disarmed(void)
{
    alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_DEFAULT);
    if (s == NULL) {
        return false;
    }
    alarm_service_get_snapshot(s);
    bool ok = true;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) != 0u && s->part[p].state != ALARM_STATE_DISARMED) {
            ok = false;
        }
    }
    heap_caps_free(s);
    return ok;
}

/* s_store_lock held, s_lock not: a change refused after its zone's invalidation was stored
 * (RF32: a partition left disarmed meanwhile). The binding stays as it was; a noted zone's
 * invalidation is already stored, and storing the unchanged bindings settles it, as for a bind
 * that changed nothing. The defined result (ZSRC-012): the old sensor keeps the zone but its
 * spoken name is no longer approved - approve it again, as after any sensor change. It isn't
 * restored: that is a configuration change, which an armed partition refuses, and a name left
 * unapproved is the safe side (D07). */
static zone_hub_result_t refuse_change(uint16_t zone, bool noted)
{
    if (noted) {
        store_bindings();
    }
    xSemaphoreGive(s_store_lock);
    wake();
    ESP_LOGW(TAG, "zone %u: sensor not changed: a partition armed meanwhile, or another sensor "
                  "change held the gate", zone);
    return ZONE_HUB_ERR_BUSY;
}

/* ZSRC-012: a binding changes only while every partition is disarmed. The check below is a
 * snapshot and the alarm task arms on its own, so from a task without s_lock the change itself
 * goes only under a reservation (zone_hub/zone_bind_gate.h, RF32): taken on the alarm task with
 * every partition disarmed after the slow parts (the store mutex, the spoken-name invalidation),
 * claimed under s_lock just before the hub changes, ended right after. The reservation holds
 * nothing: an arm before the claim goes ahead and the change is refused, the binding as it was.
 * Arming is held from the claim to the end - the in-memory change only. A change under the
 * caller's own hold (Zigbee's rebind) can't wait for the alarm task: its caller reserved and
 * claimed before taking its locks (zbz_panel_replace_gated, RF32/ZB21), so it goes only while
 * this task holds a claimed change (alarm_service_bind_mine) - never on the snapshot alone, and a
 * put-back under the same claim is never refused for a stale snapshot. */
static zone_hub_result_t change(uint16_t zone, const zone_hub_binding_t *b)
{
    if (!ready()) {
        return ZONE_HUB_ERR_BUSY;
    }
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    /* The caller holds the lock around us (Zigbee's rebind, from inside zbz_panel): neither the
     * alarm task nor NVS may be waited for here; zone_service_unlock does both (BL-151, D07). */
    const bool held = xSemaphoreGetMutexHolder(s_lock) == me;
    if (held ? !alarm_service_bind_mine() : !zone_service_all_disarmed()) {
        return ZONE_HUB_ERR_BUSY; /* a sensor is never swapped while the system is armed */
    }
    const bool zone_ok = zone >= 1u && zone <= ALARM_MAX_ZONES;
    zone_hub_binding_t cur;
    bool noted = false; /* a different sensor: its epoch moved on, the zone settles (D07) */
    uint32_t epoch = 0u;
    if (!held) {
        /* D07: a different sensor loses the zone's spoken-name approval first - stored, before
         * the binding is - or the bind is refused. Re-binding the same sensor (a new mapping)
         * keeps it. A binding that changes between this look and the bind below was another
         * change(), which waits for s_store_lock, or a source's hold, which notes its own. */
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        lock();
        const bool found = zone_hub_get(s_hub, zone, &cur);
        if (zone_ok && !zone_hub_same_sensors(found ? &cur : NULL, b)) {
            noted = zone_voice_note_change(&s_voice, zone);
            epoch = zone_voice_epoch(&s_voice, zone);
            publish_settling();
        }
        unlock();
        if (!flush_voice()) {
            lock();
            if (noted) {
                zone_voice_undo_change(&s_voice, zone, epoch); /* this sensor isn't taken */
                publish_settling();
            }
            unlock();
            xSemaphoreGive(s_store_lock);
            ESP_LOGW(TAG, "zone %u: sensor not changed, its spoken name couldn't be invalidated",
                     zone);
            return ZONE_HUB_ERR_BUSY;
        }
        if (!alarm_service_bind_reserve()) { /* RF32: armed since the check above */
            return refuse_change(zone, noted);
        }
    }
    lock();
    while (!held) {
        const zone_bind_claim_t c = alarm_service_bind_claim();
        if (c == ZONE_BIND_CLAIMED) {
            break;
        }
        unlock(); /* never wait under s_lock */
        if (c == ZONE_BIND_CLAIM_REFUSED) { /* RF32: armed meanwhile, or the time is up */
            alarm_service_bind_end();
            return refuse_change(zone, noted);
        }
        vTaskDelay(1); /* an engine step runs, perhaps an arm: bounded by ZONE_BIND_GATE_MS */
        lock();
    }
    if (held && zone_ok &&
        !zone_hub_same_sensors(zone_hub_get(s_hub, zone, &cur) ? &cur : NULL, b)) {
        noted = zone_voice_note_change(&s_voice, zone); /* before the hub takes it */
        epoch = zone_voice_epoch(&s_voice, zone);
        publish_settling();
    }
    s_changed = false;
    zone_hub_result_t r = ZONE_HUB_OK;
    if (b != NULL) {
        r = zone_hub_bind(s_hub, zone, b, now_ms());
    } else {
        zone_hub_unbind(s_hub, zone, now_ms());
    }
    const bool store = s_changed;
    if (held && noted && !store) {
        /* Nothing changed: the epoch is withdrawn under the same hold, before any flush could
         * capture it. */
        zone_voice_undo_change(&s_voice, zone, epoch);
        publish_settling();
    }
    unlock();
    if (!held) {
        alarm_service_bind_end(); /* RF32: arming goes again from the alarm task's next step */
    }
    if (held) {
        if (store) {
            __atomic_store_n(&s_save_task, me, __ATOMIC_RELEASE);
        }
    } else {
        /* NVS outside the lock: a commit takes tens of ms. A noted zone whose bind changed
         * nothing is stored too: its invalidation already is, and the store settles it. */
        if (store || noted) {
            store_bindings();
        }
        xSemaphoreGive(s_store_lock);
    }
    wake();
    if (r == ZONE_HUB_OK) {
        ESP_LOGI(TAG, "zone %u: %s", zone,
                 b == NULL ? "no sensor" : b->in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_SIM
                                              ? "simulator"
                                              : "sensor bound");
    }
    return r;
}

zone_hub_result_t zone_service_bind(uint16_t zone, const zone_hub_binding_t *b)
{
    if (b == NULL) {
        return ZONE_HUB_ERR_ARG;
    }
    return change(zone, b);
}

zone_hub_result_t zone_service_unbind(uint16_t zone)
{
    return change(zone, NULL);
}

bool zone_service_get(uint16_t zone, zone_hub_binding_t *out)
{
    if (!ready()) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    lock();
    const bool found = zone_hub_get(s_hub, zone, out);
    unlock();
    return found;
}

bool zone_service_info(uint16_t zone, zone_hub_info_t *out)
{
    if (!ready()) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    lock();
    const bool found = zone_hub_info(s_hub, zone, out);
    unlock();
    return found;
}

bool zone_service_learn(uint8_t kind, bool on)
{
    if (!ready()) {
        return false;
    }
    lock();
    const bool ok = zone_hub_learn(s_hub, kind, on, now_ms());
    if (ok && kind < 32u) {
        s_learning = on ? (s_learning | (1u << kind)) : (s_learning & ~(1u << kind));
    }
    unlock();
    wake();
    return ok;
}

bool zone_service_learning(uint8_t kind)
{
    return kind < 32u && (s_learning & (1u << kind)) != 0u;
}

size_t zone_service_candidates(uint8_t kind, const char *query, uint32_t filter,
                               zsrc_candidate_t *out, size_t max)
{
    if (!ready()) {
        return 0u;
    }
    lock();
    const size_t n = zone_hub_candidates(s_hub, kind, query, filter, out, max);
    unlock();
    return n;
}

const char *zone_service_result_name(zone_hub_result_t r)
{
    switch (r) {
    case ZONE_HUB_OK: return "ok";
    case ZONE_HUB_ERR_ARG: return "bad_argument";
    case ZONE_HUB_ERR_NO_SOURCE: return "no_such_source";
    case ZONE_HUB_ERR_IN_USE: return "sensor_in_use";
    case ZONE_HUB_ERR_FULL: return "full";
    case ZONE_HUB_ERR_REFUSED: return "no_such_sensor";
    case ZONE_HUB_ERR_BUSY: return "armed";
    default: return "?";
    }
}
