/*
 * panel_log.c - the event log on the "eventlog" partition: esp_partition backend for
 * components/core/event_log, a queue fed by the alarm task and a low-priority writer task.
 *
 * Nothing here may block the alarm task, and nothing here may quietly lose a record
 * (docs/features/05 LOG-010): the queue keeps slots back for reportable events so a burst of
 * ordinary ones can't push a report out, the writer tries a failed append again, a reportable
 * event that still can't be kept is handed over for live delivery, and whatever is lost after
 * that is counted in NVS and holds the storage trouble until someone clears it. One later
 * successful write never erases the evidence.
 */
#include "panel_log.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "log";

#define QUEUE_LEN        64
#define CRITICAL_RESERVE 16u   /* queue slots only reportable events may take */
#define TASK_PRIORITY    3
#define WRITE_TRIES      5u    /* attempts per record while the flash looks healthy */
#define WRITE_TRIES_BAD  2u    /* ... once it is known to be failing: fail fast instead */
#define RETRY_BASE_MS    20u   /* 20, 40, 80, 160 ms between attempts */
#define IDLE_POLL_MS     1000u /* the writer looks at the counters at least this often */

typedef struct {
    uint8_t kind;
    alarm_event_t ev;
} item_t;

static const esp_partition_t *s_part;
static elog_t s_log;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static uint32_t s_pending;      /* queued, not yet on flash */
static uint32_t s_passes;       /* completed writer-task passes: proof the log still runs */
static uint32_t s_lost;         /* records that never reached the flash (kept in NVS) */
static uint32_t s_lost_reports; /* of those, reportable events nobody could deliver */
static bool s_loss_dirty;       /* the counters moved and aren't in NVS yet */
static bool s_failing;          /* appends are failing right now (writer task, or start) */
static bool s_trouble;          /* the storage trouble as the engine last heard it */
/* Filled at start-up only, and read from both the alarm task and the writer task: the count
 * is published with release/acquire so a slot's contents are in place before it is seen. */
static struct {
    panel_log_lost_fn fn;
    void *ctx;
} s_lost_handlers[PANEL_LOG_LOST_HANDLERS];
static uint8_t s_lost_handler_count;

/* ---- flash backend -------------------------------------------------------------------------- */

static int p_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, off, buf, len) == ESP_OK ? 0 : -1;
}

static int p_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    return esp_partition_write(s_part, off, buf, len) == ESP_OK ? 0 : -1;
}

static int p_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    return esp_partition_erase_range(s_part, off, len) == ESP_OK ? 0 : -1;
}

/* ---- boot counter --------------------------------------------------------------------------- */

static uint32_t stored_boot(void)
{
    nvs_handle_t h;
    uint32_t boot = 0;
    if (nvs_open("log", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "boot", &boot);
        nvs_close(h);
    }
    return boot;
}

static void store_boot(uint32_t boot)
{
    nvs_handle_t h;
    if (nvs_open("log", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "boot", boot);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- what was lost (LOG-010) ----------------------------------------------------------------- */

static void load_loss(void)
{
    nvs_handle_t h;
    if (nvs_open("log", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "lost", &s_lost);
        nvs_get_u32(h, "lostrep", &s_lost_reports);
        nvs_close(h);
    }
}

static void store_loss(void)
{
    nvs_handle_t h;
    if (nvs_open("log", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "lost", __atomic_load_n(&s_lost, __ATOMIC_RELAXED));
        nvs_set_u32(h, "lostrep", __atomic_load_n(&s_lost_reports, __ATOMIC_RELAXED));
        nvs_commit(h);
        nvs_close(h);
    }
}

/* The storage trouble stands while appends are failing, and latches while records are counted
 * lost: a later successful write must not erase that an alarm report went missing. */
static void update_trouble(void)
{
    const bool want = s_failing || __atomic_load_n(&s_lost, __ATOMIC_RELAXED) != 0u;
    if (want != s_trouble) {
        s_trouble = want;
        alarm_service_storage_trouble(ALARM_STORAGE_LOG, want); /* the log's own hold (ZW-039) */
    }
}

/* A record that will never reach the flash. Safe to call from the alarm task (a listener): it
 * only touches counters and the hand-over hook. Telling the engine and writing NVS is left to
 * the writer task - alarm_service_system_trouble() posts to the queue the alarm task drains,
 * so a listener must never call it. */
static void count_lost(const item_t *it)
{
    bool taken = false;
    if ((elog_kind_t)it->kind == ELOG_KIND_REPORTED) {
        /* Every destination is offered it: the broker taking it says nothing about whether a
         * phone got it. It is lost only when nobody could take it. */
        const uint8_t n = __atomic_load_n(&s_lost_handler_count, __ATOMIC_ACQUIRE);
        for (uint8_t i = 0; i < n; i++) {
            if (s_lost_handlers[i].fn != NULL &&
                s_lost_handlers[i].fn(s_lost_handlers[i].ctx, &it->ev)) {
                taken = true;
            }
        }
        if (!taken) {
            __atomic_add_fetch(&s_lost_reports, 1u, __ATOMIC_RELAXED);
        }
    }
    const uint32_t lost = __atomic_add_fetch(&s_lost, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_loss_dirty, true, __ATOMIC_RELAXED);
    ESP_LOGE(TAG, "event #%" PRIu32 " not kept (%" PRIu32 " lost, %" PRIu32 " reports gone)%s",
             it->ev.seq, lost, __atomic_load_n(&s_lost_reports, __ATOMIC_RELAXED),
             taken ? "; reporting it live instead" : "");
}

/* ---- writer --------------------------------------------------------------------------------- */

/* Appends one record and tries again after a failed write (the flash may have been busy). This
 * is the low-priority writer task, so nobody else waits while it retries. */
static void write_item(const item_t *it)
{
    const unsigned tries = s_failing ? WRITE_TRIES_BAD : WRITE_TRIES;
    for (unsigned n = 0; n < tries; n++) {
        if (n > 0u) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_BASE_MS << (n - 1u)));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int err = elog_append(&s_log, (elog_kind_t)it->kind, &it->ev);
        xSemaphoreGive(s_lock);
        if (err == 0) {
            s_failing = false;
            return;
        }
        ESP_LOGE(TAG, "writing event #%" PRIu32 " failed (%d), try %u of %u", it->ev.seq, err,
                 n + 1u, tries);
    }
    s_failing = true;
    count_lost(it);
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        item_t it;
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(IDLE_POLL_MS)) == pdTRUE) {
            write_item(&it);
            __atomic_sub_fetch(&s_pending, 1u, __ATOMIC_RELAXED);
        }
        if (__atomic_exchange_n(&s_loss_dirty, false, __ATOMIC_RELAXED)) {
            store_loss(); /* what was lost outlives the next successful write, and a restart */
        }
        update_trouble();
        /* One completed pass, at most IDLE_POLL_MS apart even with nothing to write: this is
         * what the start-up health check watches to see that the log is still running
         * (docs/features/13 OTA-005, panel_ota_healthy). */
        __atomic_add_fetch(&s_passes, 1u, __ATOMIC_RELAXED);
    }
}

static void queue(elog_kind_t kind, const alarm_event_t *ev)
{
    if (s_queue == NULL) {
        return; /* no log at all: net_mqtt reports live while panel_log_ready() is false */
    }
    const item_t it = {.kind = (uint8_t)kind, .ev = *ev};
    /* LOG-010: a burst of ordinary events may not push a reportable one out, so the last slots
     * of the queue are kept for reports. */
    if (kind != ELOG_KIND_REPORTED &&
        uxQueueSpacesAvailable(s_queue) <= (UBaseType_t)CRITICAL_RESERVE) {
        count_lost(&it);
        return;
    }
    __atomic_add_fetch(&s_pending, 1u, __ATOMIC_RELAXED);
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        __atomic_sub_fetch(&s_pending, 1u, __ATOMIC_RELAXED);
        count_lost(&it);
    }
}

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    queue(ELOG_KIND_EVENT, ev);
}

static void on_report(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    alarm_event_t marker = *ev;
    marker.mono_ms = alarm_service_now(); /* when it became reportable */
    queue(ELOG_KIND_REPORTED, &marker);
}

static const alarm_listener_t s_listener = {
    .on_event = on_event,
    .on_report = on_report,
};

/* ---- API ------------------------------------------------------------------------------------ */

/* Unwinds a failed start: nothing is left allocated or half-published, so it can be tried
 * again, and the storage trouble says the log isn't keeping anything. */
static void start_failed(SemaphoreHandle_t lock)
{
    if (lock != NULL) {
        vSemaphoreDelete(lock);
    }
    memset(&s_log, 0, sizeof(s_log));
    s_part = NULL;
    s_failing = true;
    update_trouble(); /* no writer task yet: tell the engine from here */
}

esp_err_t panel_log_start(void)
{
    if (s_queue != NULL) {
        return ESP_ERR_INVALID_STATE; /* already running */
    }
    /* Everything is built privately and only published once all of it worked (R07). */
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    load_loss(); /* what an earlier boot lost is still lost */
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      "eventlog");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "no \"eventlog\" partition: events are not kept");
        start_failed(lock);
        return ESP_ERR_NOT_FOUND;
    }
    const elog_flash_t flash = {
        .ctx = NULL,
        .size = (uint32_t)s_part->size,
        .sector_size = (uint32_t)s_part->erase_size,
        .read = p_read,
        .write = p_write,
        .erase = p_erase,
    };
    const int err = elog_open(&s_log, &flash, 0);
    if (err != 0) {
        ESP_LOGE(TAG, "event log unusable (%d)", err);
        start_failed(lock);
        return ESP_FAIL;
    }
    /* The boot number only ever grows, even when NVS was erased and the log kept. */
    uint32_t boot = stored_boot();
    elog_entry_t newest;
    if (elog_read(&s_log, UINT32_MAX, NULL, NULL, &newest, 1, 0, NULL) == 1u &&
        newest.boot > boot) {
        boot = newest.boot;
    }
    s_log.boot = boot + 1u;
    store_boot(s_log.boot);

    QueueHandle_t q = xQueueCreate(QUEUE_LEN, sizeof(item_t));
    if (q == NULL) {
        start_failed(lock);
        return ESP_ERR_NO_MEM;
    }
    s_lock = lock;
    s_queue = q; /* the writer task needs both before it runs */
    if (xTaskCreatePinnedToCore(writer_task, "log", 4096, NULL, TASK_PRIORITY, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        s_queue = NULL;
        s_lock = NULL;
        vQueueDelete(q);
        start_failed(lock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%" PRIu32 " events kept (at least %" PRIu32 "), last #%" PRIu32 ", boot %" PRIu32,
             s_log.count, elog_capacity(&s_log), s_log.last_seq, s_log.boot);
    if (s_lost != 0u) {
        ESP_LOGE(TAG, "%" PRIu32 " record(s) lost earlier, %" PRIu32 " of them undelivered "
                      "reports: the storage trouble stands until it is cleared",
                 s_lost, s_lost_reports);
    }
    return ESP_OK;
}

const alarm_listener_t *panel_log_listener(void)
{
    return &s_listener;
}

uint32_t panel_log_last_seq(void)
{
    return s_log.ready ? s_log.last_seq : 0u;
}

uint32_t panel_log_boot(void)
{
    return s_log.boot;
}

bool panel_log_flush(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    while (__atomic_load_n(&s_pending, __ATOMIC_RELAXED) != 0u) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

size_t panel_log_read(uint32_t before, elog_filter_fn filter, void *filter_ctx,
                      elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before)
{
    if (s_lock == NULL || !s_log.ready) {
        if (next_before != NULL) {
            *next_before = 0;
        }
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = elog_read(&s_log, before, filter, filter_ctx, out, max, scan, next_before);
    xSemaphoreGive(s_lock);
    return n;
}

size_t panel_log_read_after(uint32_t after, elog_filter_fn filter, void *filter_ctx,
                            elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_after)
{
    if (s_lock == NULL || !s_log.ready) {
        if (next_after != NULL) {
            *next_after = after;
        }
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = elog_read_after(&s_log, after, filter, filter_ctx, out, max, scan, next_after);
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t panel_log_newest_index(void)
{
    if (s_lock == NULL || !s_log.ready) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t newest = elog_newest_index(&s_log);
    xSemaphoreGive(s_lock);
    return newest;
}

bool panel_log_ready(void)
{
    return s_log.ready;
}

uint32_t panel_log_passes(void)
{
    return __atomic_load_n(&s_passes, __ATOMIC_RELAXED);
}

void panel_log_stats(uint32_t *count, uint32_t *capacity)
{
    *count = s_log.ready ? s_log.count : 0u;
    *capacity = s_log.ready ? elog_capacity(&s_log) : 0u;
}

esp_err_t panel_log_add_lost_handler(panel_log_lost_fn fn, void *ctx)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t n = s_lost_handler_count;
    if (n >= PANEL_LOG_LOST_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    s_lost_handlers[n].fn = fn;
    s_lost_handlers[n].ctx = ctx;
    /* The slot is filled before the count that publishes it. */
    __atomic_store_n(&s_lost_handler_count, (uint8_t)(n + 1u), __ATOMIC_RELEASE);
    return ESP_OK;
}

void panel_log_remove_lost_handler(panel_log_lost_fn fn, void *ctx)
{
    for (uint8_t i = 0; i < s_lost_handler_count; i++) {
        if (s_lost_handlers[i].fn == fn && s_lost_handlers[i].ctx == ctx) {
            s_lost_handlers[i].fn = NULL; /* the count only grows; an empty slot is skipped */
            s_lost_handlers[i].ctx = NULL;
            return;
        }
    }
}

void panel_log_loss(uint32_t *records, uint32_t *reports)
{
    if (records != NULL) {
        *records = __atomic_load_n(&s_lost, __ATOMIC_RELAXED);
    }
    if (reports != NULL) {
        *reports = __atomic_load_n(&s_lost_reports, __ATOMIC_RELAXED);
    }
}

void panel_log_clear_loss(void)
{
    __atomic_store_n(&s_lost, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_lost_reports, 0u, __ATOMIC_RELAXED);
    store_loss();
    /* The writer stores it again (harmless) and drops the storage trouble with it. */
    __atomic_store_n(&s_loss_dirty, true, __ATOMIC_RELAXED);
}

esp_err_t panel_log_clear(void)
{
    if (s_lock == NULL || !s_log.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    panel_log_flush(2000);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int err = elog_clear(&s_log);
    xSemaphoreGive(s_lock);
    if (err == 0) {
        panel_log_clear_loss(); /* the history is gone on purpose: so is what it lost */
    }
    return err == 0 ? ESP_OK : ESP_FAIL;
}
