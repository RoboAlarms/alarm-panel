/*
 * zwave_service.c - Z-Wave sensors through a Zooz ZAC93 (see zwave_service.h, docs/features/36).
 *
 * The zwave task owns the network: zwave_zones' zwz_panel (zw_net, the bridge to the zone source,
 * what the pages show and ask for, the log, what is stored). Every call into it - the link's news,
 * the requests, the polls - is made with the zone service's lock held, because the bridge feeds
 * the Z-Wave zone source, whose state the zone task's hub ticks too; the pages read it under the
 * same lock from their own task. NVS is written and events are logged after unlocking: an NVS
 * commit takes tens of milliseconds, and logging waits for the alarm task.
 *
 * Requests from the touch UI and the console go to the zwave task one at a time, each waiting for
 * its answer (REQUEST_MS at most): they start or answer network operations, which then go on by
 * themselves; the pages follow them through zwave_service_ui_progress().
 *
 * NVS "zwave" (network.md §2; the formats in zwz_panel.h): keys, gen, nodes, cfg, chk - the
 * network's, zwz_panel's to decide - and place, rstpin - where the radio is wired, the console's.
 */
#include "zwave_service.h"
#include "zwave_internal.h"

#if CONFIG_ALARM_ZSRC_ZWAVE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "alarm/alarm_strings.h"
#include "alarm_mem.h"
#include "alarm_service.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "zone_service.h"
#include "zwave_proto/zw_gbl.h"
#include "zwave_zones/zwz_store.h"

static const char *TAG = "zwave";
static const char *NS = "zwave";

#define TASK_STACK       8192 /* internal: it writes NVS (docs/bugs/B001) */
#define TASK_PRIORITY    (CONFIG_ALARM_TASK_PRIORITY - 2) /* below the zone task's */
#define MAX_WAIT_MS      1000u  /* the pages' countdowns; an input a zone newly watches */
#define REQUEST_MS       2000u
#define RESET_PULSE_MS   20u    /* hardware.md §5 */
#define RESTART_EXPECT_MS 5000u
#define NODES_BLOB_MAX   ZWZ_NODES_BLOB_MAX /* the cache at its fullest (network.md §2) */
#define NVS_TELL_MS      10000u /* ZW-039: NVS's free entries told at least this often */

static struct {
    bool started;              /* the zone source is registered */
    bool running;              /* a place: the tasks and the network run */
    zwave_service_config_t cfg;
    char place[16];
    char where[64];
    zwave_service_rules_t rules;
    TaskHandle_t task;
    zw_crypto_port_t crypto;
    zwsvc_crypto_t crypto_check; /* its self-test at start (a place chosen) */
    SemaphoreHandle_t req_lock; /* one request at a time */
    SemaphoreHandle_t req_done; /* the zwave task answered */
    zwz_store_t store; /* what persist() waits for after a refused write (zwz_store.h) */
    bool storage_trouble;
    zwsvc_stats_t stats;
} S;

/* The network (84 KB), the zone source (23 KB) and the node cache's encoding (51 KB): tasks
 * only, so PSRAM. */
static ALARM_SLOW_BSS zwz_panel_t s_panel;
static bool s_arming_held; /* what the engine was last told (alarm_service_hold_arming) */
/* The installer's .gbl on its way to the zwave task (zwave_service_radio_file), under s_mux. */
static uint8_t *s_file;
static size_t s_file_len;
static TickType_t s_file_at; /* when it was placed */
static ALARM_SLOW_BSS zsrc_zw_t s_src;
static ALARM_SLOW_BSS uint8_t s_blob[NODES_BLOB_MAX];
static ALARM_SLOW_BSS zwf_t s_feedback;
/* Keypad control (zw_keyctl.h, ZW-031): the assignments, and the commands an assigned keypad sent,
 * copied under the zone lock by keyctl_claim and run by keyctl_run after it, when this task may
 * wait for the alarm task. A copy holds a PIN only from its claim to its run, then is erased. */
static ALARM_SLOW_BSS zwkc_t s_keyctl;
#define KEYCTL_BOX 2u
#define KEYCTL_CMD_MAX 64u
static ALARM_SLOW_BSS struct {
    bool used;
    uint8_t slot;
    uint32_t home;
    uint16_t generation;
    zw_s2_rx_t rx;
    uint8_t cmd[KEYCTL_CMD_MAX];
} s_keyctl_box[KEYCTL_BOX];
static QueueHandle_t s_feedback_view, s_feedback_events;
static uint32_t s_feedback_dropped;

/* Alarm task callbacks never take the zone lock, wait for radio work or generate audio.
 * Latest state has its own overwrite mailbox: event saturation cannot lose disarm/silence. */
/* SmartStart listens only while every partition is disarmed (network.md §4.2): kept from the
 * engine's snapshots, read by zwz_panel's port in the zwave task. */
static bool s_disarmed;

static bool port_disarmed(void *ctx)
{
    (void)ctx;
    return __atomic_load_n(&s_disarmed, __ATOMIC_RELAXED);
}

/* ZW-038 (CORE-TRBL-008): an assigned keypad that may arm and disarm can't be reached, or is
 * heard again - the engine's device trouble for its ZWKC_DEVICE. Only queued: the zone lock is
 * held. */
static void port_keypad_reachable(void *ctx, uint8_t device, bool reachable)
{
    (void)ctx;
    alarm_service_device_reachable(device, reachable);
}

/* A device's Time or Date Get (ZW-024): the panel's local time (its time zone), when its clock is
 * set - from 2025 on, the touch UI's own rule - else "clock not set". */
static bool port_local_time(void *ctx, zwz_local_time_t *out)
{
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec < 1735689600LL) {
        return false;
    }
    struct tm tm;
    const time_t t = tv.tv_sec;
    if (localtime_r(&t, &tm) == NULL) {
        return false;
    }
    out->year = (uint16_t)(tm.tm_year + 1900);
    out->month = (uint8_t)(tm.tm_mon + 1);
    out->day = (uint8_t)tm.tm_mday;
    out->hour = (uint8_t)tm.tm_hour;
    out->minute = (uint8_t)tm.tm_min;
    out->second = (uint8_t)tm.tm_sec;
    return true;
}

static void feedback_snapshot(void *ctx, const alarm_snapshot_t *snapshot)
{
    (void)ctx;
    bool disarmed = true;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((snapshot->partitions & (1u << p)) != 0u &&
            snapshot->part[p].state != ALARM_STATE_DISARMED) {
            disarmed = false;
        }
    }
    __atomic_store_n(&s_disarmed, disarmed, __ATOMIC_RELAXED);
    zwf_view_t view;
    zwf_view(snapshot, &view);
    (void)xQueueOverwrite(s_feedback_view, &view);
    if (S.running) { xTaskNotifyGive(S.task); }
}

static void feedback_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (ev->type != ALARM_EV_ARM_FAILED && ev->type != ALARM_EV_INVALID_CODE && ev->type != ALARM_EV_LOCKOUT) { return; }
    if (xQueueSend(s_feedback_events, ev, 0) != pdTRUE) {
        __atomic_fetch_add(&s_feedback_dropped, 1u, __ATOMIC_RELAXED);
    }
    if (S.running) { xTaskNotifyGive(S.task); }
}

static void feedback_done(void *ctx, uint32_t tag, zw_s2_result_t result)
{
    (void)ctx;
    (void)zwf_done(&s_feedback, tag, result, alarm_service_now());
}

/* Under the zone lock (zwz_panel's command port): an assigned keypad's command is copied, never
 * run here. Full: not taken (its Supervision says so, and the keypad may send it again). */
static bool keyctl_claim(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    size_t slot = ZWKC_SLOTS;
    if (!zwkc_claims(&s_keyctl, &s_panel.net, rx, &slot) || rx->len > KEYCTL_CMD_MAX) {
        return false;
    }
    const zw_node_record_t *rec = zw_node_find_const(&s_panel.net.nodes, rx->node);
    for (size_t i = 0; rec != NULL && i < KEYCTL_BOX; i++) {
        if (!s_keyctl_box[i].used) {
            s_keyctl_box[i].used = true;
            s_keyctl_box[i].slot = (uint8_t)slot;
            s_keyctl_box[i].home = s_panel.net.keys_home_id;
            s_keyctl_box[i].generation = rec->generation;
            s_keyctl_box[i].rx = *rx;
            memcpy(s_keyctl_box[i].cmd, rx->command, rx->len);
            s_keyctl_box[i].rx.command = s_keyctl_box[i].cmd;
            return true;
        }
    }
    return false;
}

/* The adapter's port: the alarm engine's own command, on its own task. UNAVAILABLE means the
 * answer is not known (nothing may have run): not a result, so a retry may run it. */
static bool keyctl_command(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *src,
                           zwk_reply_t *reply)
{
    (void)ctx;
    reply->result = alarm_service_command(cmd, src, &reply->detail);
    return reply->result != ALARM_ERR_UNAVAILABLE;
}

/* Without the zone lock: each claimed command run through the engine, its copy erased first. */
static void keyctl_run(void)
{
    for (size_t i = 0; i < KEYCTL_BOX; i++) {
        uint8_t cmd[KEYCTL_CMD_MAX];
        zw_s2_rx_t rx;
        uint8_t slot = 0u;
        uint32_t home = 0u;
        uint16_t generation = 0u;
        zone_service_lock();
        const bool used = s_keyctl_box[i].used;
        if (used) {
            rx = s_keyctl_box[i].rx;
            memcpy(cmd, s_keyctl_box[i].cmd, sizeof cmd);
            rx.command = cmd;
            slot = s_keyctl_box[i].slot;
            home = s_keyctl_box[i].home;
            generation = s_keyctl_box[i].generation;
            zw_wipe(&s_keyctl_box[i], sizeof(s_keyctl_box[i]));
        }
        zone_service_unlock();
        if (!used) {
            continue;
        }
        zwk_reply_t reply;
        const zwk_result_t r = zwkc_run(&s_keyctl, slot, home, generation, &rx, alarm_service_now(),
                                        &reply);
        zw_wipe(cmd, sizeof cmd);
        zw_wipe(&rx, sizeof rx);
        /* never the PIN: which keypad, and what the engine said */
        ESP_LOGI(TAG, "keypad slot %u: %s%s", (unsigned)slot + 1u,
                 r == ZWK_HANDLED ? "command " : r == ZWK_DUPLICATE ? "repeat of " : "",
                 r == ZWK_HANDLED || r == ZWK_DUPLICATE ? alarm_result_name(reply.result)
                 : r == ZWK_IGNORED                     ? "ignored (not a command it may give)"
                 : r == ZWK_INVALID                     ? "refused (malformed)"
                 : r == ZWK_BUSY                        ? "not run (the alarm task was busy)"
                                                        : "refused (not the assigned device)");
    }
}

/* Called by the network owner with zone lock held. Only this task mutates the bridge. */
static void feedback_poll(void)
{
    zwf_view_t view;
    if (xQueueReceive(s_feedback_view, &view, 0) == pdTRUE) { zwf_snapshot(&s_feedback, &view); }
    alarm_event_t ev;
    for (unsigned i = 0; i < 16u && xQueueReceive(s_feedback_events, &ev, 0) == pdTRUE; i++) {
        zwf_event(&s_feedback, &ev);
    }
    zwf_poll(&s_feedback, &s_panel.net, alarm_service_now(), NULL, NULL);
}

/* The request being handed to the zwave task (under s_mux). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool pending;
    uint32_t seq;
    zwsvc_req_t req;
    uint32_t done_seq;
    ui_zw_req_result_t result;
} s_rq;

static uint64_t now_ms(void)
{
    return alarm_service_now();
}

/* ---- NVS "zwave" -------------------------------------------------------------------------------- */

static esp_err_t store_blob(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, buf, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

/* ZW-039 (docs/features/20 devices.md "The NVS budget"): NVS's free entries, as Zigbee reads them;
 * unreadable: not told (no limit), as the PC. */
static uint32_t nvs_free_entries(void)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return ZWZ_NVS_NOT_TOLD;
    }
    return st.available_entries > UINT32_MAX - 1u ? UINT32_MAX - 1u
                                                  : (uint32_t)st.available_entries;
}

/* The budget as NVS has it now, to the panel (the zone lock taken here): at start, after each
 * store pass, before an Add or a Replace goes on, and every NVS_TELL_MS (a SmartStart join asks
 * nobody first; other owners write NVS too, and so do this file's keypad and setting writes). */
static void tell_nvs_free(void)
{
    const uint32_t entries = nvs_free_entries();
    zone_service_lock();
    zwz_panel_nvs_free(&s_panel, entries);
    zone_service_unlock();
}

/* The most a stored value is read at (zwz_store_read_max): twice today's largest and some, never
 * under what the firmware before read, so a longer blob from newer firmware (an OTA rollback)
 * still reads and its decoder takes what it knows (network.md §2); anything longer is refused as
 * damaged. */
#define LOAD_BLOB_MAX(today) zwz_store_read_max((size_t)(today))

static void free_blob(uint8_t *buf, size_t len)
{
    if (buf != NULL) {
        zw_wipe(buf, len); /* the keys come through here too */
        heap_caps_free(buf);
    }
}

/* A stored value, read whole at its stored length: into `room` (the caller's, `room_cap` bytes)
 * when it fits there, else into a buffer of its own (give it back with release_blob). NULL and
 * *len 0 when it was never stored - or when it is unreadable or longer than max, which is logged
 * and then treated as never stored, as before (what a refused node cache should do instead is
 * the owner's question). The verdict is zwz_store_read_verdict's, host-tested. */
static uint8_t *load_blob_into(const char *key, size_t max, uint8_t *room, size_t room_cap,
                               size_t *len)
{
    *len = 0u;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) { /* not found: nothing stored in "zwave" yet */
            ESP_LOGE(TAG, "stored %s unreadable: NVS refused to open (%s)", key,
                     esp_err_to_name(err));
        }
        return NULL;
    }
    size_t stored = 0u;
    err = nvs_get_blob(h, key, NULL, &stored);
    uint8_t *buf = NULL;
    switch (zwz_store_read_verdict(err != ESP_ERR_NVS_NOT_FOUND, err == ESP_OK, stored, max)) {
    case ZWZ_STORE_READ_ABSENT:
        break;
    case ZWZ_STORE_READ_REFUSED:
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stored %s unreadable (%s)", key, esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "stored %s refused: %u bytes, more than the %u this firmware reads",
                     key, (unsigned)stored, (unsigned)max);
        }
        break;
    case ZWZ_STORE_READ_OK:
    default:
        if (room != NULL && stored <= room_cap) {
            buf = room;
        } else {
            /* the node cache is big: PSRAM; the small ones (the keys among them) internal */
            buf = heap_caps_malloc_prefer(stored, 2,
                                          stored > 1024u ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL,
                                          MALLOC_CAP_DEFAULT);
        }
        if (buf == NULL) {
            ESP_LOGE(TAG, "stored %s unread: no memory for its %u bytes", key, (unsigned)stored);
        } else {
            size_t got = stored;
            err = nvs_get_blob(h, key, buf, &got);
            if (err != ESP_OK || got != stored) {
                ESP_LOGE(TAG, "stored %s unreadable (%s)", key, esp_err_to_name(err));
                if (buf != room) {
                    free_blob(buf, stored);
                }
                buf = NULL;
            } else {
                *len = stored;
            }
        }
        break;
    }
    nvs_close(h);
    return buf;
}

static uint8_t *load_blob(const char *key, size_t max, size_t *len)
{
    return load_blob_into(key, max, NULL, 0u, len);
}

/* load_blob_into's buffer back: wiped and freed when it was its own, left alone when it was the
 * caller's room. */
static void release_blob(uint8_t *buf, size_t len, const uint8_t *room)
{
    if (buf != room) {
        free_blob(buf, len);
    }
}

static uint16_t load_u16(const char *key)
{
    nvs_handle_t h;
    uint16_t v = 0u;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, key, &v) != ESP_OK) {
            v = 0u;
        }
        nvs_close(h);
    }
    return v;
}

static esp_err_t erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
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
    return err;
}

esp_err_t zwsvc_store_str(const char *key, const char *value)
{
    if (value == NULL) {
        return erase(key);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

esp_err_t zwsvc_store_u8(const char *key, int value)
{
    if (value < 0 || value > 255) {
        return erase(key);
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, key, (uint8_t)value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

bool zwave_service_stored_place(char *out, size_t len)
{
    nvs_handle_t h;
    bool found = false;
    if (out == NULL || len == 0u) {
        return false;
    }
    out[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        found = nvs_get_str(h, "place", out, &n) == ESP_OK && out[0] != '\0' &&
                strcmp(out, "off") != 0;
        nvs_close(h);
    }
    if (!found) {
        out[0] = '\0';
    }
    return found;
}

/* The custom place's pins (ZW-046): never reuse a key. */
static const char *const CUSTOM_KEYS[] = {"c_tx", "c_rx", "c_reset"};

static bool stored_is_custom(void)
{
    char place[16];
    return zwave_service_stored_place(place, sizeof place) && strcmp(place, "custom") == 0;
}

zwave_custom_t zwave_service_stored_custom(int16_t *tx, int16_t *rx, int16_t *reset)
{
    *tx = -1;
    *rx = -1;
    *reset = -1;
    if (!stored_is_custom()) {
        return ZWAVE_CUSTOM_NONE;
    }
    nvs_handle_t h;
    uint8_t v[3] = {0xFFu, 0xFFu, 0xFFu};
    bool ok = nvs_open(NS, NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_u8(h, CUSTOM_KEYS[0], &v[0]) == ESP_OK &&
             nvs_get_u8(h, CUSTOM_KEYS[1], &v[1]) == ESP_OK && v[0] <= 63u && v[1] <= 63u;
        const esp_err_t r = nvs_get_u8(h, CUSTOM_KEYS[2], &v[2]);
        ok = ok && (r == ESP_ERR_NVS_NOT_FOUND || (r == ESP_OK && v[2] <= 63u));
        if (r != ESP_OK) {
            v[2] = 0xFFu;
        }
        nvs_close(h);
    }
    if (!ok) {
        return ZWAVE_CUSTOM_DAMAGED;
    }
    *tx = (int16_t)v[0];
    *rx = (int16_t)v[1];
    *reset = v[2] <= 63u ? (int16_t)v[2] : (int16_t)-1;
    return ZWAVE_CUSTOM_STORED;
}

static esp_err_t erase_in(nvs_handle_t h, const char *key)
{
    const esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t zwsvc_store_custom(int16_t tx, int16_t rx, int16_t reset)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = erase_in(h, "place");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = erase_in(h, "rstpin");
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, CUSTOM_KEYS[0], (uint8_t)tx);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, CUSTOM_KEYS[1], (uint8_t)rx);
    }
    if (err == ESP_OK) {
        err = reset >= 0 ? nvs_set_u8(h, CUSTOM_KEYS[2], (uint8_t)reset) : erase_in(h, CUSTOM_KEYS[2]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "place", "custom");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t zwsvc_forget_custom(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < sizeof CUSTOM_KEYS / sizeof CUSTOM_KEYS[0] && err == ESP_OK; i++) {
        err = erase_in(h, CUSTOM_KEYS[i]);
    }
    if (err == ESP_OK) {
        err = erase_in(h, "rstpin");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int16_t zwave_service_stored_reset_pin(void)
{
    nvs_handle_t h;
    uint8_t v = 0u;
    bool found = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_u8(h, "rstpin", &v) == ESP_OK;
        nvs_close(h);
    }
    return found ? (int16_t)v : (int16_t)-1;
}

void zwave_service_rules(const zwave_service_rules_t *rules)
{
    S.rules = *rules;
}

/* ---- the network's port -------------------------------------------------------------------------- */

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return zw_link_write(bytes, len);
}

/* Open drain: low to reset, released otherwise - never driven high (hardware.md §5). */
static void port_reset_line(void *ctx, bool asserted)
{
    (void)ctx;
    (void)gpio_set_level((gpio_num_t)S.cfg.reset, asserted ? 0u : 1u);
}

static bool port_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    return zone_service_get(zone, out);
}

/* What the panel answers about itself (command-classes.md §5). */
/* ZW-035: where a garage's tilt sensor says the door is - its zone's input, known, reachable and
 * not CHECK; anything else is unknown, and the garage is never pressed blind. */
static zwo_position_t port_position(void *ctx, uint16_t zone)
{
    (void)ctx;
    zone_hub_info_t in;
    if (!zone_service_info(zone, &in) || !in.known || in.unreachable || in.check ||
        in.link_down) {
        return ZWO_POS_UNKNOWN;
    }
    return in.active ? ZWO_POS_OPEN : ZWO_POS_CLOSED;
}

static void identity(zw_cc_panel_identity_t *id)
{
    memset(id, 0, sizeof *id);
    const esp_app_desc_t *app = esp_app_get_description();
    char *end = NULL;
    const unsigned long major = strtoul(app->version, &end, 10);
    const unsigned long minor = end != NULL && *end == '.' ? strtoul(end + 1, NULL, 10) : 0u;
    id->firmware_major = (uint8_t)(major <= 255u ? major : 255u);
    id->firmware_minor = (uint8_t)(minor <= 255u ? minor : 255u);
    id->manufacturer_id = 0xFFFFu; /* until the project has a manufacturer ID */
    id->product_type = 0xFFFFu;
    id->product_id = 0xFFFFu;
    id->zwave_plus_role_type = 0x00u; /* central static controller */
    id->zwave_plus_node_type = 0x00u;
    id->installer_icon = 0x0500u;     /* generic gateway */
    id->user_icon = 0x0500u;
}

/* ---- the zwave task --------------------------------------------------------------------------- */

static void handle_news(uint8_t kind, const uint8_t *buf, size_t n, uint64_t now)
{
    switch (kind) {
    case ZW_LINK_CONTROL: zwz_panel_on_control(&s_panel, now, buf[0]); break;
    case ZW_LINK_FRAME:
        if (n >= 5u) {
            const zw_frame_t f = {.type = buf[2],
                                  .func = buf[3],
                                  .payload = n > 5u ? &buf[4] : NULL,
                                  .payload_len = n - 5u,
                                  .raw = buf,
                                  .raw_len = n};
            zwz_panel_on_frame(&s_panel, now, &f);
        }
        break;
    case ZW_LINK_TEXT: zwz_panel_on_text(&s_panel, now, buf, n); break;
    default: break;
    }
}

static bool take_news(void)
{
    uint8_t buf[ZW_LINK_NEWS_MAX];
    uint8_t kind = 0u;
    bool any = false;
    size_t n;
    while ((n = zw_link_read(&kind, buf, sizeof buf)) > 0u) {
        any = true;
        zone_service_lock();
        handle_news(kind, buf, n, now_ms());
        zone_service_unlock();
    }
    return any;
}

static ui_zw_req_result_t run_request(const zwsvc_req_t *req)
{
    ui_zw_req_result_t r = UI_ZW_REQ_INVALID;
    switch ((zwsvc_req_kind_t)req->kind) {
    case ZWSVC_REQ_FEEDBACK: {
        if (!req->allowed) { return UI_ZW_REQ_NOT_ALLOWED; }
        if ((req->feedback_slot >= ZWF_TARGETS && req->feedback_slot != ZWSVC_SLOT_ANY) ||
            req->feedback_partition > ALARM_MAX_PARTITIONS) {
            return UI_ZW_REQ_INVALID;
        }
        alarm_snapshot_t *snapshot = heap_caps_malloc(sizeof(*snapshot), MALLOC_CAP_DEFAULT);
        if (snapshot == NULL) { return UI_ZW_REQ_BUSY; }
        alarm_service_get_snapshot(snapshot);
        const bool quiet = !alarm_snapshot_locked(snapshot);
        const bool enabled = req->feedback_partition == 0u ||
            (snapshot->partitions & (1u << (req->feedback_partition - 1u))) != 0u;
        heap_caps_free(snapshot);
        if (!quiet) { return UI_ZW_REQ_NOT_ALLOWED; }
        if (!enabled) { return UI_ZW_REQ_INVALID; }
        uint8_t blob[ZWF_BLOB_SIZE];
        /* Copy outside the live bridge, persist first; a failed write changes nothing. */
        zwf_t *candidate = heap_caps_malloc(sizeof(*candidate), MALLOC_CAP_DEFAULT);
        if (candidate == NULL) { return UI_ZW_REQ_BUSY; }
        zone_service_lock();
        *candidate = s_feedback;
        size_t slot = req->feedback_slot;
        bool assigned = true;
        if (slot == ZWSVC_SLOT_ANY) {
            /* the device page names the device by node and generation: the one it showed */
            slot = req->feedback_partition == 0u ? zwf_slot_of(candidate, req->ui.node)
                                                 : zwf_slot_for(candidate, req->ui.node);
            assigned = zwz_panel_generation(&s_panel, req->ui.node) == req->ui.generation &&
                       (slot < ZWF_TARGETS || req->feedback_partition == 0u);
        }
        const bool nothing = assigned && slot >= ZWF_TARGETS; /* off, and it had no slot */
        if (assigned && !nothing) {
            assigned = zwf_assign(candidate, slot, &s_panel.net, req->ui.node,
                                  req->feedback_partition);
        }
        zone_service_unlock();
        if (nothing) {
            heap_caps_free(candidate);
            return UI_ZW_REQ_OK;
        }
        r = UI_ZW_REQ_INVALID;
        if (assigned && zwf_encode(candidate, blob, sizeof(blob)) != 0u) {
            if (store_blob("keypad_fb", blob, sizeof(blob)) == ESP_OK) {
                zone_service_lock();
                s_feedback = *candidate;
                zone_service_unlock();
                r = UI_ZW_REQ_OK;
            } else { r = UI_ZW_REQ_BUSY; }
        }
        heap_caps_free(candidate);
        break;
    }
    case ZWSVC_REQ_CONTROL: {
        /* Granting a keypad control: installer mode (the UI) or the console, everything disarmed
         * and quiet; stored before it counts; the node the page showed. */
        if (!req->allowed) { return UI_ZW_REQ_NOT_ALLOWED; }
        if ((req->feedback_slot >= ZWKC_SLOTS && req->feedback_slot != ZWSVC_SLOT_ANY) ||
            req->feedback_partition > ALARM_MAX_PARTITIONS) {
            return UI_ZW_REQ_INVALID;
        }
        alarm_snapshot_t *snapshot = heap_caps_malloc(sizeof(*snapshot), MALLOC_CAP_DEFAULT);
        if (snapshot == NULL) { return UI_ZW_REQ_BUSY; }
        alarm_service_get_snapshot(snapshot);
        const bool quiet = !alarm_snapshot_locked(snapshot); /* allowed says disarmed */
        const bool enabled = req->feedback_partition == 0u ||
            (snapshot->partitions & (1u << (req->feedback_partition - 1u))) != 0u;
        heap_caps_free(snapshot);
        if (!quiet) { return UI_ZW_REQ_NOT_ALLOWED; }
        if (!enabled) { return UI_ZW_REQ_INVALID; }
        uint8_t blob[ZWKC_BLOB_SIZE];
        zwkc_t *candidate = heap_caps_malloc(sizeof(*candidate), MALLOC_CAP_DEFAULT);
        if (candidate == NULL) { return UI_ZW_REQ_BUSY; }
        zone_service_lock();
        *candidate = s_keyctl;
        size_t slot = req->feedback_slot;
        bool assigned = true;
        if (slot == ZWSVC_SLOT_ANY) {
            slot = req->feedback_partition == 0u ? zwkc_slot_of(candidate, req->ui.node)
                                                 : zwkc_slot_for(candidate, req->ui.node);
            assigned = zwz_panel_generation(&s_panel, req->ui.node) == req->ui.generation &&
                       (slot < ZWKC_SLOTS || req->feedback_partition == 0u);
        }
        const bool nothing = assigned && slot >= ZWKC_SLOTS;
        if (assigned && !nothing) {
            assigned = zwkc_assign(candidate, slot, &s_panel.net, req->ui.node,
                                   req->feedback_partition, req->control_panic);
        }
        zone_service_unlock();
        r = nothing ? UI_ZW_REQ_OK : UI_ZW_REQ_INVALID;
        if (assigned && !nothing && zwkc_encode(candidate, blob, sizeof(blob)) != 0u) {
            if (store_blob("keypad_ctl", blob, sizeof(blob)) == ESP_OK) {
                zone_service_lock();
                s_keyctl = *candidate;
                zone_service_unlock();
                r = UI_ZW_REQ_OK;
            } else { r = UI_ZW_REQ_BUSY; }
        }
        zw_wipe(candidate, sizeof(*candidate));
        heap_caps_free(candidate);
        break;
    }
    case ZWSVC_REQ_FW: r = zwsvc_fw_request(req, !req->console); break;
    case ZWSVC_REQ_RADIO_FILE: {
        taskENTER_CRITICAL(&s_mux);
        uint8_t *file = s_file;
        const size_t len = s_file_len;
        s_file = NULL;
        s_file_len = 0u;
        taskEXIT_CRITICAL(&s_mux);
        r = zwsvc_fw_radio_file(file, len, req->allowed, req->ui.user);
        break;
    }
    case ZWSVC_REQ_HA:
        zone_service_lock();
        r = zwz_panel_ha_command(&s_panel, now_ms(), req->ha_key, req->ha_level, req->ha_received)
                ? UI_ZW_REQ_OK
                : UI_ZW_REQ_INVALID;
        zone_service_unlock();
        break;
    case ZWSVC_REQ_UI:
        if (req->ui.kind == UI_ZW_REQ_STOP && zwsvc_fw_cancel()) {
            r = UI_ZW_REQ_OK; /* a device's software download stopped: nothing sent */
            break;
        }
        if (req->ui.kind == UI_ZW_REQ_ADD || req->ui.kind == UI_ZW_REQ_REPLACE) {
            tell_nvs_free(); /* ZW-039: the room as it is now, not as last written */
        }
        zone_service_lock();
        r = zwz_panel_request(&s_panel, now_ms(), &req->ui, req->allowed);
        zone_service_unlock();
        break;
    case ZWSVC_REQ_INTERVIEW:
        if (!req->allowed) {
            return UI_ZW_REQ_NOT_ALLOWED;
        }
        zone_service_lock();
        r = zwz_panel_interview_result(&s_panel, now_ms(), req->ui.node);
        zone_service_unlock();
        break;
    case ZWSVC_REQ_POWER: {
        if (req->power_set && !req->allowed) {
            return UI_ZW_REQ_NOT_ALLOWED;
        }
        zone_service_lock();
        if (!req->power_set) {
            r = zwz_panel_read_power(&s_panel, now_ms()) ? UI_ZW_REQ_OK : UI_ZW_REQ_UNSUPPORTED;
        } else {
            const zw_net_result_t res = zwz_panel_set_power(&s_panel, now_ms(), req->allowed,
                                                            req->power_lr, req->power_dbm10);
            r = res == ZW_NET_STARTED || res == ZW_NET_UNCHANGED ? UI_ZW_REQ_OK
                : res == ZW_NET_NOT_ALLOWED                     ? UI_ZW_REQ_NOT_ALLOWED
                : res == ZW_NET_BUSY                            ? UI_ZW_REQ_BUSY
                : res == ZW_NET_LINK_DOWN                       ? UI_ZW_REQ_NO_RADIO
                                                                : UI_ZW_REQ_INVALID;
        }
        zone_service_unlock();
        break;
    }
    case ZWSVC_REQ_PULSE:
        if (!req->allowed) {
            return UI_ZW_REQ_NOT_ALLOWED;
        }
        if (S.cfg.reset < 0) {
            return UI_ZW_REQ_UNSUPPORTED;
        }
        zone_service_lock();
        zwz_panel_expect_restart(&s_panel, now_ms(), RESTART_EXPECT_MS);
        zone_service_unlock();
        port_reset_line(NULL, true);
        vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
        port_reset_line(NULL, false);
        r = UI_ZW_REQ_OK;
        break;
    default: break;
    }
    return r;
}

static bool take_request(void)
{
    zwsvc_req_t req;
    uint32_t seq = 0u;
    bool have = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_rq.pending) {
        req = s_rq.req;
        seq = s_rq.seq;
        s_rq.pending = false;
        have = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!have) {
        return false;
    }
    const ui_zw_req_result_t r = run_request(&req);
    taskENTER_CRITICAL(&s_mux);
    s_rq.done_seq = seq;
    s_rq.result = r;
    taskEXIT_CRITICAL(&s_mux);
    (void)xSemaphoreGive(S.req_done);
    return true;
}

/* zwz_store's port: one NVS handle for a pass, opened at its first write and closed after it. */
typedef struct {
    nvs_handle_t h;
    bool open;
} store_nvs_t;

static bool store_open(store_nvs_t *n)
{
    if (!n->open) {
        n->open = nvs_open(NS, NVS_READWRITE, &n->h) == ESP_OK;
    }
    return n->open;
}

static bool store_set_blob(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    store_nvs_t *n = ctx;
    return store_open(n) && nvs_set_blob(n->h, key, data, len) == ESP_OK;
}

static bool store_set_u16(void *ctx, const char *key, uint16_t v)
{
    store_nvs_t *n = ctx;
    return store_open(n) && nvs_set_u16(n->h, key, v) == ESP_OK;
}

static bool store_commit(void *ctx)
{
    store_nvs_t *n = ctx;
    return n->open && nvs_commit(n->h) == ESP_OK;
}

static void store_lock(void *ctx)
{
    (void)ctx;
    zone_service_lock();
}

static void store_unlock(void *ctx)
{
    (void)ctx;
    zone_service_unlock();
}

/* Every pass that wrote: counted. ZW-039: Z-Wave's own hold on the storage trouble is set on
 * every failed pass and cleared after the first pass where everything stored; the engine keeps
 * the trouble while any other owner (the configuration, the log, ...) still holds it. */
static void store_trouble(void *ctx, bool failed)
{
    (void)ctx;
    S.stats.stores++;
    if (failed) {
        S.stats.store_failures++;
        S.storage_trouble = true;
        alarm_service_storage_trouble(ALARM_STORAGE_ZWAVE, true);
    } else if (S.storage_trouble) {
        S.storage_trouble = false;
        alarm_service_storage_trouble(ALARM_STORAGE_ZWAVE, false);
    }
}

/* What changed, to NVS after unlocking: keys first, the counter before the cache (zwz_store.h). */
static void persist(void)
{
    static ALARM_SLOW_BSS uint8_t prov[ZW_PROV_BLOB_MAX]; /* the zwave task's alone */
    store_nvs_t nvs = {.open = false};
    const zwz_store_port_t port = {.ctx = &nvs,
                                   .lock = store_lock,
                                   .unlock = store_unlock,
                                   .set_blob = store_set_blob,
                                   .set_u16 = store_set_u16,
                                   .commit = store_commit,
                                   .trouble = store_trouble,
                                   .nodes_buf = s_blob,
                                   .nodes_cap = sizeof s_blob,
                                   .prov_buf = prov,
                                   .prov_cap = sizeof prov};
    const uint8_t failed = zwz_store_run(&S.store, &s_panel, now_ms(), &port);
    if (nvs.open) {
        nvs_close(nvs.h);
        tell_nvs_free(); /* ZW-039: after each store pass */
    }
    if (failed != 0u) {
        ESP_LOGE(TAG, "storing the Z-Wave network failed (0x%02x): tried again in %u s",
                 (unsigned)failed, (unsigned)(ZWZ_STORE_RETRY_MS / 1000u));
    }
}

/* The network's events into the log (network.md §15) - after unlocking: it waits for the alarm
 * task. As the local touchscreen when a user asked, else as the panel itself. */
static void log_events(void)
{
    zwz_log_t logs[ZWZ_PANEL_LOG_MAX];
    size_t n = 0u;
    zone_service_lock();
    while (n < ZWZ_PANEL_LOG_MAX && zwz_panel_take_log(&s_panel, &logs[n])) {
        n++;
    }
    zone_service_unlock();
    for (size_t i = 0; i < n; i++) {
        const zwz_log_t *l = &logs[i];
        const alarm_source_t src = l->user != 0u
                                       ? (alarm_source_t){.kind = ALARM_SRC_KEYPAD,
                                                          .id = ALARM_KEYPAD_LOCAL}
                                       : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
        const esp_err_t err = alarm_service_platform_event((alarm_event_type_t)l->type, l->zone,
                                                           l->arg, l->arg2, &src, l->user);
        S.stats.events++;
        if (err != ESP_OK) {
            S.stats.events_failed++;
            ESP_LOGE(TAG, "event %u not logged (%s)", (unsigned)l->type, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "%s: node %u zone %u (%u)", alarm_event_name((alarm_event_type_t)l->type),
                     (unsigned)l->arg, (unsigned)l->zone, (unsigned)l->arg2);
        }
    }
}

/* A file whose request was withdrawn unanswered (zwsvc_submit's REQUEST_MS): freed here - never
 * one whose request may still be on its way (placed less than REQUEST_MS + 1 s ago). */
static void drop_withdrawn_file(void)
{
    uint8_t *drop = NULL;
    taskENTER_CRITICAL(&s_mux);
    if (s_file != NULL && !s_rq.pending &&
        xTaskGetTickCount() - s_file_at > pdMS_TO_TICKS(REQUEST_MS + 1000u)) {
        drop = s_file;
        s_file = NULL;
        s_file_len = 0u;
    }
    taskEXIT_CRITICAL(&s_mux);
    heap_caps_free(drop);
}

/* ---- the panel's own devices: commands posted by device_service (zwave_service_output_post) ---- */

/* Waiting at most. One event can start every automation that listens for it (16, two actions
 * each), all posted in one pass: a short queue would answer the later ones "unreachable" with the
 * radio up. 16 x ~64 B, in PSRAM. */
#define OUTPUT_POSTS 16u

typedef struct {
    char key[PROTO_ZW_KEY_LEN];
    int level;
    uint64_t issued_ms;
    uint32_t request_id;
    zwave_service_output_done_fn done;
    void *ctx;
} output_post_t;

static QueueHandle_t s_out_q; /* created before the zwave task; never deleted */

/* The output's index for a key, or -1; *garage its garage's index or -1. Under the zone lock. */
static int output_by_key(const char *key, int *garage)
{
    *garage = -1;
    const zw_module_facts_t *f = zw_host_facts(&s_panel.net.host);
    for (size_t i = 0; f != NULL && i < ZWO_OUTPUTS; i++) {
        const zwo_output_t *u = &s_panel.outputs.out[i];
        char k[PROTO_ZW_KEY_LEN];
        if (!u->used || !proto_zw_key(f->home_id, u->node, u->generation, u->endpoint, k) ||
            strcmp(k, key) != 0) {
            continue;
        }
        for (size_t g = 0; g < ZWO_GARAGES; g++) {
            if (s_panel.outputs.garage[g].used && s_panel.outputs.garage[g].output == i) {
                *garage = (int)g;
            }
        }
        return (int)i;
    }
    return -1;
}

static const char *output_error(zwo_result_t r)
{
    switch (r) {
    case ZWO_OK:
    case ZWO_ALREADY: return "";
    case ZWO_ERR_BUSY: return "busy";
    case ZWO_ERR_OFFLINE: return "offline";
    case ZWO_ERR_SEND: return "send_failed";
    case ZWO_ERR_TIMEOUT: return "timeout"; /* sent, not confirmed: unknown (dsrc_zw, HCTL-012) */
    default: return "failed"; /* unknown, stale, a level it can't take, position unknown */
    }
}

/* PL02: the callers of started commands, until their end - by zwz_panel_output_wait's slot. The
 * zwave task's alone. s_ended: the ends taken under the zone lock, answered after it. */
typedef struct {
    output_post_t post;
    zwo_result_t result;
} output_end_t;
static ALARM_SLOW_BSS output_post_t s_waits[ZWZ_OUT_WAITS];
static ALARM_SLOW_BSS output_end_t s_ended[ZWZ_OUT_WAITS];

/* Under the zone lock: the commands that ended since the last pass. How many. */
static size_t take_ended(void)
{
    size_t n = 0u;
    size_t slot = 0u;
    uint32_t request = 0u;
    zwo_result_t r = ZWO_OK;
    while (n < ZWZ_OUT_WAITS && zwz_panel_output_ended(&s_panel, &slot, &request, &r)) {
        if (slot < ZWZ_OUT_WAITS) {
            if (s_waits[slot].request_id == request) {
                s_ended[n].post = s_waits[slot];
                s_ended[n].result = r;
                n++;
            }
            memset(&s_waits[slot], 0, sizeof s_waits[slot]);
        }
    }
    return n;
}

/* With no lock of this service held (the answer takes device_service's): each caller once. */
static void answer_ended(size_t n)
{
    for (size_t k = 0; k < n; k++) {
        const output_post_t *p = &s_ended[k].post;
        const zwo_result_t r = s_ended[k].result;
        if (r != ZWO_OK) {
            ESP_LOGW(TAG, "device command for %s ended (%d)", p->key, (int)r);
        }
        if (p->done != NULL) {
            p->done(p->ctx, p->request_id, r == ZWO_OK, zwo_end_word(r)); /* HCTL-012 */
        }
    }
}

/* One posted command: run under the zone lock. A refusal (or "already there") is answered at
 * once after it; a started one at its end (PL02: take_ended). The answer takes device_service's
 * lock, which is never held while this one is taken. */
static bool take_output(void)
{
    output_post_t p;
    if (s_out_q == NULL || xQueueReceive(s_out_q, &p, 0) != pdTRUE) {
        return false;
    }
    zwo_result_t r = ZWO_ERR_UNKNOWN;
    int w = -1;
    bool untracked = false;
    zone_service_lock();
    const uint64_t t = now_ms();
    int g = -1;
    const int i = output_by_key(p.key, &g);
    if (i >= 0) {
        const zwo_output_t *u = &s_panel.outputs.out[i];
        if (g >= 0) {
            /* ZW-035: the output layer presses only when the sensor says the door is the other
             * way (ZWO_ERR_POSITION when it can't tell) - never blind. User 0: the panel itself. */
            r = p.level == 0 || p.level == 100
                    ? zwz_panel_garage_move(&s_panel, t, u->node, u->generation, u->endpoint,
                                            p.level == 100, p.issued_ms, 0u)
                    : ZWO_ERR_ARG;
        } else {
            r = zwz_panel_output_set(&s_panel, t, u->node, u->generation, u->endpoint, p.level,
                                     p.issued_ms);
        }
        if (r == ZWO_OK && p.done != NULL) {
            w = zwz_panel_output_wait(&s_panel, u->node, u->generation, u->endpoint,
                                      p.request_id);
            if (w >= 0 && (size_t)w < ZWZ_OUT_WAITS) {
                s_waits[w] = p;
            } else {
                w = -1; /* never (a wait per output and garage) */
                untracked = true;
            }
        }
    }
    zone_service_unlock();
    if (w >= 0) {
        return true; /* answered at its end */
    }
    if (untracked) {
        /* started, but its end can't be followed: never "done" - it may happen or not, the
         * unknown outcome (HCTL-012), not a refusal (it was sent) */
        ESP_LOGW(TAG, "device command for %s started, its end not followed", p.key);
        p.done(p.ctx, p.request_id, false, zwo_end_word(ZWO_ERR_TIMEOUT));
        return true;
    }
    const bool ok = r == ZWO_OK || r == ZWO_ALREADY;
    if (!ok) {
        ESP_LOGW(TAG, "device command for %s refused (%d)", p.key, (int)r);
    }
    if (p.done != NULL) {
        p.done(p.ctx, p.request_id, ok, output_error(r));
    }
    return true;
}

static void zwave_task(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* the link is open */
    tell_nvs_free(); /* ZW-039: before the radio is up (a SmartStart join may come first) */
    uint64_t told_at = now_ms();
    zone_service_lock();
    zwz_panel_start(&s_panel, now_ms());
    zone_service_unlock();
    for (;;) {
        if (now_ms() - told_at >= NVS_TELL_MS) {
            tell_nvs_free();
            told_at = now_ms();
        }
        bool moved = take_news();
        moved = take_request() || moved;
        moved = take_output() || moved;
        zone_service_lock();
        zwz_panel_poll(&s_panel, now_ms());
        feedback_poll();
        const uint64_t next = zwz_panel_next_deadline(&s_panel);
        const bool held = zwz_panel_arming_held(&s_panel, now_ms());
        const size_t ended = take_ended();
        zone_service_unlock();
        answer_ended(ended); /* PL02: posted commands' ends, outside the lock */
        if (held != s_arming_held && alarm_service_hold_arming(held)) {
            s_arming_held = held; /* network.md §14, CORE-ARM-013: the radio's own update */
            ESP_LOGW(TAG, "arming %s", held ? "held: the Z-Wave radio is updating" : "no longer held");
        }
        if (moved) {
            zone_service_wake(); /* the zone source may have news for the hub */
        }
        persist();
        log_events();
        keyctl_run();
        zwsvc_fw_poll();
        drop_withdrawn_file();
        const uint64_t t = now_ms();
        uint64_t wait = MAX_WAIT_MS;
        if (next != UINT64_MAX) {
            wait = next > t ? next - t : 0u;
            if (wait > MAX_WAIT_MS) {
                wait = MAX_WAIT_MS;
            }
        }
        if (s_out_q != NULL && uxQueueMessagesWaiting(s_out_q) > 0u) {
            wait = 0u; /* one posted command a pass: the next goes on the next tick, not in 1 s */
        }
        const TickType_t ticks = pdMS_TO_TICKS((uint32_t)wait);
        (void)ulTaskNotifyTake(pdTRUE, ticks > 0 ? ticks : 1); /* never a busy loop */
    }
}

/* ---- requests ---------------------------------------------------------------------------------- */

ui_zw_req_result_t zwsvc_submit(const zwsvc_req_t *req)
{
    if (!S.running || req == NULL) {
        return UI_ZW_REQ_NO_RADIO;
    }
    (void)xSemaphoreTake(S.req_lock, portMAX_DELAY);
    (void)xSemaphoreTake(S.req_done, 0); /* an answer nobody waited for any more */
    taskENTER_CRITICAL(&s_mux);
    const uint32_t seq = ++s_rq.seq;
    s_rq.req = *req;
    s_rq.pending = true;
    taskEXIT_CRITICAL(&s_mux);
    S.stats.requests++;
    xTaskNotifyGive(S.task);
    ui_zw_req_result_t result = UI_ZW_REQ_BUSY;
    bool answered = false;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(REQUEST_MS);
    for (;;) {
        const TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= limit || xSemaphoreTake(S.req_done, limit - spent) != pdTRUE) {
            break;
        }
        taskENTER_CRITICAL(&s_mux);
        answered = s_rq.done_seq == seq;
        if (answered) {
            result = s_rq.result;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (answered) {
            break;
        }
    }
    if (!answered) {
        taskENTER_CRITICAL(&s_mux);
        if (s_rq.pending && s_rq.seq == seq) {
            s_rq.pending = false; /* never taken: withdrawn */
        }
        taskEXIT_CRITICAL(&s_mux);
        S.stats.request_timeouts++;
    }
    (void)xSemaphoreGive(S.req_lock);
    return result;
}

/* ZW-005, ZW-016, ZW-018: installer mode with every partition disarmed. */
static bool installer_and_disarmed(void);

bool zwsvc_installer_and_disarmed(void)
{
    return installer_and_disarmed();
}

uint64_t zwsvc_now_ms(void)
{
    return now_ms();
}

static bool installer_and_disarmed(void)
{
    alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_DEFAULT);
    if (s == NULL) {
        return false;
    }
    alarm_service_get_snapshot(s);
    bool ok = s->program;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) != 0u && s->part[p].state != ALARM_STATE_DISARMED) {
            ok = false;
        }
    }
    heap_caps_free(s);
    return ok;
}

/* ---- the touch UI's calls ------------------------------------------------------------------------ */

bool zwave_service_ui_status(ui_zw_status_t *out)
{
    memset(out, 0, sizeof *out);
    if (!S.started) {
        return false;
    }
    if (!S.running) {
        /* no place chosen; or one is, but the crypto self-test failed: "Radio · Can't be used",
         * not "Choose where it is wired" */
        out->radio = S.crypto_check == ZWSVC_CRYPTO_FAILED ? UI_ZW_RADIO_UNUSABLE
                                                           : UI_ZW_RADIO_NOT_CONNECTED;
        out->check_in_s = ZW_NET_CHECK_IN_S;
        return true;
    }
    zone_service_lock();
    zwz_panel_status(&s_panel, now_ms(), out);
    zone_service_unlock();
    zwsvc_fw_radio_status(out);
    return true;
}

size_t zwave_service_ui_devices(ui_zw_device_t *out, size_t max)
{
    if (!S.running) {
        return 0u;
    }
    zone_service_lock();
    const size_t n = zwz_panel_devices(&s_panel, now_ms(), out, max);
    zone_service_unlock();
    return n;
}

bool zwave_service_ui_device(uint16_t node, uint16_t generation, ui_zw_device_page_t *out)
{
    if (!S.running) {
        memset(out, 0, sizeof *out);
        return false;
    }
    zone_service_lock();
    const bool found = zwz_panel_device(&s_panel, now_ms(), node, generation, out);
    if (found) {
        zwsvc_fw_page(node, generation, out);
    }
    zone_service_unlock();
    return found;
}

ui_zw_req_result_t zwave_service_radio_file(const uint8_t *data, size_t len, bool allowed,
                                            uint8_t user)
{
    if (data == NULL || len == 0u || len > ZW_GBL_MAX) {
        return UI_ZW_REQ_INVALID;
    }
    /* a copy the zwave task takes (s_file): the caller's buffer may go the moment this returns,
     * even when the answer is late */
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        return UI_ZW_REQ_BUSY;
    }
    memcpy(copy, data, len);
    bool placed = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_file == NULL) {
        s_file = copy;
        s_file_len = len;
        s_file_at = xTaskGetTickCount();
        placed = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!placed) {
        heap_caps_free(copy);
        return UI_ZW_REQ_BUSY;
    }
    zwsvc_req_t r = {.kind = ZWSVC_REQ_RADIO_FILE, .allowed = allowed && zone_service_all_disarmed()};
    r.ui.user = user;
    return zwsvc_submit(&r);
}

ui_zw_req_result_t zwave_service_request(const ui_zw_request_t *req, bool allowed)
{
    if (req == NULL) {
        return UI_ZW_REQ_INVALID;
    }
    zwsvc_req_t r = {.kind = ZWSVC_REQ_UI, .allowed = allowed && zone_service_all_disarmed(),
                     .ui = *req};
    if (req->kind == UI_ZW_REQ_FW_CHECK || req->kind == UI_ZW_REQ_FW_INSTALL) {
        r.kind = ZWSVC_REQ_FW; /* the update service (zw_fwsvc.c); the caller decided who asks */
        r.console = true;
    }
    return zwsvc_submit(&r);
}

bool zwave_service_diag(zwave_diag_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!S.running) {
        return false;
    }
    zone_service_lock();
    const zwz_panel_t *p = &s_panel;
    const zw_net_t *net = &p->net;
    const zw_module_facts_t *f = zw_host_facts(&p->net.host);
    out->up = zw_host_link(&p->net.host) == ZW_LINK_UP;
    out->home_id = f->home_id;
    out->node_id = f->node_id;
    out->suc_node_id = f->suc_node_id;
    out->chip_type = f->chip_type;
    out->chip_version = f->chip_version;
    out->library_type = f->library_type;
    out->api_version = f->api_version;
    memcpy(out->library, f->library, sizeof(out->library));
    out->library[sizeof(out->library) - 1u] = '\0';
    out->manufacturer = f->manufacturer;
    out->product_type = f->product_type;
    out->product_id = f->product_id;
    out->region_known = f->region_known;
    out->region = f->region;
    const zw_jam_t *j = zw_net_jam(net);
    out->noise_known = j->has_reading;
    out->channels = j->channels < 4u ? j->channels : 4u;
    memcpy(out->noise, j->rssi, out->channels);
    out->jam_threshold = j->threshold;
    out->jammed = zw_jam_jammed(j);
    const zw_net_power_t *pw = zw_net_power(net);
    out->power_known = pw->known;
    out->lr_power_known = pw->lr_known;
    out->power_dbm10 = pw->dbm10;
    out->power_cal_dbm10 = pw->cal_dbm10;
    out->lr_power_dbm10 = pw->lr_dbm10;
    out->power_cap_set = p->setup.cfg.power_cap_set;
    out->lr_cap_set = p->setup.cfg.lr_cap_set;
    out->power_cap_dbm10 = p->setup.cfg.power_cap_dbm10;
    out->lr_cap_dbm10 = p->setup.cfg.lr_cap_dbm10;
    const zw_host_stats_t *hs = zw_host_stats(&p->net.host);
    out->link_ups = hs->link_ups;
    out->link_downs = hs->link_downs;
    out->restarts = hs->restarts;
    out->recoveries = hs->recoveries;
    out->bootloader = hs->bootloader;
    const zw_net_stats_t *ns = zw_net_stats(net);
    out->foreign = ns->foreign;
    out->malformed = ns->malformed;
    out->not_singlecast = ns->not_singlecast;
    const zw_node_cache_t *cache = &net->nodes;
    const size_t n = zw_node_count(cache);
    for (size_t i = 0; i < n && out->node_count < ZWAVE_DIAG_NODES; i++) {
        const zw_node_record_t *rec = zw_node_at_const(cache, i);
        const zw_s2_node_stats_t *st = rec != NULL ? zw_s2_node_stats(&net->s2, rec->node_id)
                                                   : NULL;
        if (rec == NULL) {
            continue;
        }
        out->nodes[out->node_count].node = rec->node_id;
        if (st != NULL) {
            out->nodes[out->node_count].wrong_level = st->wrong_level;
            out->nodes[out->node_count].decrypt_failures = st->decrypt_failures;
            out->nodes[out->node_count].duplicates = st->duplicates;
        }
        out->node_count++;
    }
    zone_service_unlock();
    return true;
}

ui_zw_req_result_t zwave_service_ui_request(const ui_zw_request_t *req)
{
    if (req == NULL) {
        return UI_ZW_REQ_INVALID;
    }
    zwsvc_req_t r = {.kind = ZWSVC_REQ_UI, .allowed = installer_and_disarmed(), .ui = *req};
    if (req->kind == UI_ZW_REQ_CONTROL) {
        /* a keypad's buttons: the persisted control assignment (`zwave keypad-control`) */
        r.kind = ZWSVC_REQ_CONTROL;
        r.feedback_slot = ZWSVC_SLOT_ANY;
        r.feedback_partition = req->partition;
        r.control_panic = req->panic_mask;
    }
    if (req->kind == UI_ZW_REQ_FW_CHECK || req->kind == UI_ZW_REQ_FW_INSTALL) {
        r.kind = ZWSVC_REQ_FW; /* the update service: zw_fwsvc.c */
    }
    if (req->kind == UI_ZW_REQ_FEEDBACK) {
        /* a keypad's sounds and lights: the persisted output assignment (`zwave keypad-feedback`),
         * never incoming control */
        r.kind = ZWSVC_REQ_FEEDBACK;
        r.feedback_slot = ZWSVC_SLOT_ANY;
        r.feedback_partition = req->partition;
    }
    return zwsvc_submit(&r);
}

size_t zwave_service_ha_devices(void *out, size_t max)
{
    if (!S.running || out == NULL) {
        return 0u;
    }
    zone_service_lock();
    const size_t n = zwz_panel_ha_devices(&s_panel, now_ms(), out, max);
    zone_service_unlock();
    return n;
}

void zwave_service_action_ms(const void *devices, size_t n, uint32_t *ms)
{
    if (ms == NULL) {
        return;
    }
    memset(ms, 0, n * sizeof(ms[0]));
    if (!S.running || devices == NULL) {
        return;
    }
    const proto_zw_device_t *d = devices;
    zone_service_lock();
    for (size_t i = 0; i < n; i++) {
        int g = -1;
        if (output_by_key(d[i].key, &g) >= 0 && g >= 0) {
            ms[i] = zwo_garage_budget_ms(&s_panel.outputs, (size_t)g);
        }
    }
    zone_service_unlock();
}

bool zwave_service_ha_command(const char *key, int level, uint64_t received_ms)
{
    if (key == NULL || strlen(key) >= PROTO_ZW_KEY_LEN) {
        return false;
    }
    zwsvc_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = ZWSVC_REQ_HA;
    snprintf(r.ha_key, sizeof r.ha_key, "%s", key);
    r.ha_level = level;
    r.ha_received = received_ms;
    return zwsvc_submit(&r) == UI_ZW_REQ_OK;
}

bool zwave_service_output_post(const char *key, int level, uint64_t issued_ms,
                               uint32_t request_id, zwave_service_output_done_fn done, void *ctx)
{
    if (!S.running || s_out_q == NULL || key == NULL || strlen(key) >= PROTO_ZW_KEY_LEN ||
        level < 0 || level > 100) {
        return false;
    }
    output_post_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.key, sizeof p.key, "%s", key);
    p.level = level;
    p.issued_ms = issued_ms;
    p.request_id = request_id;
    p.done = done;
    p.ctx = ctx;
    if (xQueueSend(s_out_q, &p, 0) != pdTRUE) {
        return false; /* full: refused now, never waited for */
    }
    xTaskNotifyGive(S.task);
    return true;
}

bool zwave_service_link_up(void)
{
    if (!S.running) {
        return false;
    }
    zone_service_lock();
    const bool up = zw_host_link(&s_panel.net.host) == ZW_LINK_UP;
    zone_service_unlock();
    return up;
}

/* ---- SmartStart (network.md §4.2): the pages' and the Panel Manager's calls ------------------------ */

static bool installer_mode(void)
{
    alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_DEFAULT);
    if (s == NULL) {
        return false;
    }
    alarm_service_get_snapshot(s);
    const bool ok = s->program;
    heap_caps_free(s);
    return ok;
}

size_t zwave_service_ui_prov(ui_zw_prov_t *out, size_t max)
{
    if (!S.running || out == NULL) {
        return 0u;
    }
    zone_service_lock();
    const size_t n = zwz_panel_prov(&s_panel, out, max);
    zone_service_unlock();
    return n;
}

/* The list is data only (no radio): changed under the lock here, then the zwave task stores it
 * and listens (or stops) at its next poll. Registering needs installer mode, not "disarmed": a
 * registered device joins only while everything is disarmed. */
ui_zw_prov_result_t zwsvc_prov_register(const char *code, const char *name, bool allowed)
{
    if (!S.running) {
        return UI_ZW_PROV_NOT_FOUND;
    }
    zone_service_lock();
    const ui_zw_prov_result_t r = zwz_panel_prov_register(&s_panel, now_ms(), code, name, allowed);
    zone_service_unlock();
    xTaskNotifyGive(S.task);
    return r;
}

ui_zw_prov_result_t zwsvc_prov_change(const uint8_t dsk[16], uint8_t op, uint8_t arg,
                                      bool allowed)
{
    if (!S.running) {
        return UI_ZW_PROV_NOT_FOUND;
    }
    zone_service_lock();
    const ui_zw_prov_result_t r = zwz_panel_prov_change(&s_panel, now_ms(), dsk, op, arg, allowed);
    zone_service_unlock();
    xTaskNotifyGive(S.task);
    return r;
}

ui_zw_prov_result_t zwave_service_prov_register(const char *code, const char *name, bool allowed)
{
    return zwsvc_prov_register(code, name, allowed);
}

ui_zw_prov_result_t zwave_service_prov_change(const uint8_t dsk[16], uint8_t op, uint8_t arg,
                                              bool allowed)
{
    return zwsvc_prov_change(dsk, op, arg, allowed);
}

ui_zw_prov_result_t zwave_service_ui_prov_register(const char *code, const char *name)
{
    return zwsvc_prov_register(code, name, installer_mode());
}

ui_zw_prov_result_t zwave_service_ui_prov_change(const uint8_t dsk[16], uint8_t op, uint8_t arg)
{
    return zwsvc_prov_change(dsk, op, arg, installer_mode());
}

void zwave_service_ui_progress(ui_zw_progress_t *out)
{
    if (!S.running) {
        memset(out, 0, sizeof *out);
        return;
    }
    zone_service_lock();
    zwz_panel_progress(&s_panel, now_ms(), out);
    zone_service_unlock();
}

/* ---- for the console ----------------------------------------------------------------------------- */

bool zwsvc_started(void)
{
    return S.started;
}

bool zwsvc_running(void)
{
    return S.running;
}

zwsvc_crypto_t zwsvc_crypto_check(void)
{
    return S.crypto_check;
}

const zwave_service_config_t *zwsvc_config(void)
{
    return &S.cfg;
}

const zwave_service_rules_t *zwsvc_rules(void)
{
    return &S.rules;
}

void zwsvc_stats(zwsvc_stats_t *out)
{
    *out = S.stats;
    const TaskHandle_t t = S.task;
    out->stack_free = t != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(t) : 0u;
    out->stack_size = t != NULL ? (uint32_t)TASK_STACK : 0u;
    zw_link_stack(&out->link_stack_free, &out->link_stack_size);
    zw_fwsvc_stack(&out->fw_stack_free, &out->fw_stack_size);
}

zwz_panel_t *zwsvc_panel(void)
{
    return &s_panel;
}

const zwf_t *zwsvc_feedback(void) { return &s_feedback; }

const zwkc_t *zwsvc_keyctl(void) { return &s_keyctl; }

zsrc_zw_t *zwsvc_source(void)
{
    return &s_src;
}

/* ---- start ------------------------------------------------------------------------------------- */

/* What the network starts with: what NVS holds. */
static void load_setup(zwz_panel_setup_t *setup, zwz_checkin_t *checkins, size_t *checkin_n)
{
    memset(setup, 0, sizeof *setup);
    size_t n = 0u;
    uint8_t *buf = load_blob(ZWZ_STORE_KEY_CFG, LOAD_BLOB_MAX(ZWZ_CFG_BLOB), &n);
    if (buf != NULL && !zwz_cfg_decode(&setup->cfg, buf, n)) {
        ESP_LOGW(TAG, "stored settings unreadable: the defaults");
    }
    free_blob(buf, n);
    buf = load_blob(ZWZ_STORE_KEY_KEYS, LOAD_BLOB_MAX(ZWZ_KEYS_BLOB), &n);
    if (buf != NULL && !zwz_keys_decode(&setup->keys, buf, n)) {
        /* keys lost: zw_net then uses none of the module's nodes (network.md §2) */
        ESP_LOGE(TAG, "stored network keys unreadable");
    }
    free_blob(buf, n); /* wiped */
    setup->generation = load_u16(ZWZ_STORE_KEY_GEN);
    /* ZW-024, network.md §9: jamming is watched on every panel, Long Range or not (the 908 MHz
     * jammer must raise a trouble in the image that ships) */
    setup->jam = true;
#if CONFIG_ALARM_ZWAVE_LR
    setup->long_range = true; /* phase 3 */
#endif
    identity(&setup->identity);
    *checkin_n = 0u;
    buf = load_blob(ZWZ_STORE_KEY_CHK, LOAD_BLOB_MAX(ZWZ_CHECKINS_BLOB), &n);
    if (buf != NULL && !zwz_checkins_decode(checkins, ZWZ_PANEL_CHECKINS, checkin_n, buf, n)) {
        ESP_LOGW(TAG, "stored check-ins unreadable: the default for every device");
    }
    free_blob(buf, n);
}

static esp_err_t reset_line_setup(int16_t gpio)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ull << gpio,
        .mode = GPIO_MODE_OUTPUT_OD, /* low or released, never driven high */
        .pull_up_en = GPIO_PULLUP_DISABLE, /* the module pulls it up itself */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)gpio, 1u), TAG, "reset line");
    return gpio_config(&io);
}

esp_err_t zwave_service_start(const zwave_service_config_t *cfg)
{
    if (S.started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "no configuration");
    S.cfg = *cfg;
    S.cfg.reset = cfg->place != NULL ? cfg->reset : (int16_t)-1;
    snprintf(S.place, sizeof S.place, "%s", cfg->place != NULL ? cfg->place : "");
    snprintf(S.where, sizeof S.where, "%s", cfg->where != NULL ? cfg->where : "");
    S.cfg.place = cfg->place != NULL ? S.place : NULL;
    S.cfg.where = S.where;

    /* The source joins the hub either way: zones bound to Z-Wave devices keep their bindings. */
    const zsrc_sink_t sink = zone_service_sink();
    zsrc_zw_init(&s_src, &sink);
    const zone_hub_result_t hr = zone_service_register(zsrc_zw_ops(), &s_src);
    if (hr != ZONE_HUB_OK) {
        ESP_LOGE(TAG, "the zone hub refused the Z-Wave source (%s)", zone_service_result_name(hr));
        return ESP_ERR_INVALID_STATE;
    }
    S.started = true;
    if (cfg->place == NULL) {
        ESP_LOGI(TAG, "Z-Wave radio off: no place chosen (`zwave place`), no pin touched");
        return ESP_OK;
    }

    /* ZW-008: the crypto port is proved on this panel's own hardware before the network relies
     * on it - the standards' answers for AES-128 and X25519, a random source that is not stuck.
     * Failed (or PSA would not start): the radio stays off, no pin touched, as with no place;
     * `zwave status` says so. */
    const bool crypto_ok = zw_psa_port(&S.crypto) && zw_crypto_port_check(&S.crypto);
    S.crypto_check = crypto_ok ? ZWSVC_CRYPTO_PASSED : ZWSVC_CRYPTO_FAILED;
    if (!crypto_ok) {
        ESP_LOGE(TAG, "crypto self-test FAILED (AES-128, X25519 or the random source through PSA): "
                      "the Z-Wave radio stays off");
        return ESP_FAIL;
    }
    S.req_lock = xSemaphoreCreateMutex();
    S.req_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(S.req_lock != NULL && S.req_done != NULL, ESP_ERR_NO_MEM, TAG,
                        "request queue");
    if (S.cfg.reset >= 0) {
        ESP_RETURN_ON_ERROR(reset_line_setup(S.cfg.reset), TAG, "reset line GPIO %d",
                            (int)S.cfg.reset);
    }

    zwz_panel_setup_t *setup = heap_caps_calloc(1, sizeof(*setup), MALLOC_CAP_DEFAULT);
    zwz_checkin_t *checkins = heap_caps_calloc(ZWZ_PANEL_CHECKINS, sizeof(*checkins),
                                               MALLOC_CAP_DEFAULT);
    if (setup == NULL || checkins == NULL) {
        heap_caps_free(setup);
        heap_caps_free(checkins);
        return ESP_ERR_NO_MEM;
    }
    size_t checkin_n = 0u;
    load_setup(setup, checkins, &checkin_n);
    const zwz_panel_port_t port = {.ctx = NULL,
                                   .write = port_write,
                                   .reset_line = S.cfg.reset >= 0 ? port_reset_line : NULL,
                                   .binding = port_binding,
                                   .send_done = feedback_done,
                                   .command = keyctl_claim,
                                   .zone_position = port_position,
                                   .disarmed = port_disarmed,
                                   .local_time = port_local_time,
                                   .keypad_reachable = port_keypad_reachable};
    static const zwk_port_t keyctl_port = {.ctx = NULL, .command = keyctl_command};
    zwkc_init(&s_keyctl, &keyctl_port);
    size_t keyctl_len = 0u;
    uint8_t *keyctl_blob = load_blob("keypad_ctl", LOAD_BLOB_MAX(ZWKC_BLOB_SIZE), &keyctl_len);
    if (keyctl_blob != NULL && !zwkc_decode(&s_keyctl, keyctl_blob, keyctl_len)) {
        ESP_LOGE(TAG, "stored keypad control assignments invalid: every keypad's control is off");
    }
    free_blob(keyctl_blob, keyctl_len);
    /* read before the zone lock: NVS and the heap are not for under it. The node cache goes into
     * s_blob, as before (nothing uses it before the zwave task starts): only a longer one from
     * newer firmware takes the heap, so a cache that reads today cannot be lost to a full heap. */
    size_t len = 0u;
    uint8_t *nodes_blob = load_blob_into(ZWZ_STORE_KEY_NODES, LOAD_BLOB_MAX(NODES_BLOB_MAX), s_blob,
                                         sizeof s_blob, &len);
    size_t garage_len = 0u;
    uint8_t *garage = load_blob(ZWZ_STORE_KEY_GARAGE, LOAD_BLOB_MAX(ZWO_GARAGE_BLOB), &garage_len);
    size_t prov_len = 0u;
    uint8_t *prov = load_blob(ZWZ_STORE_KEY_PROV, LOAD_BLOB_MAX(ZW_PROV_BLOB_MAX), &prov_len);
    zone_service_lock();
    zwz_panel_init(&s_panel, &port, setup, checkins, checkin_n, &s_src, &S.crypto);
    zwz_panel_set_feedback(&s_panel, &s_feedback); /* the device page's "Sounds and lights" */
    zwz_panel_set_keyctl(&s_panel, &s_keyctl);     /* ... and "Arm and disarm from its buttons" */
    const bool loaded = nodes_blob == NULL || zwz_panel_load_nodes(&s_panel, nodes_blob, len);
    const bool garage_ok = garage == NULL || zwz_panel_load_garage(&s_panel, garage, garage_len);
    const bool prov_ok = prov == NULL || zwz_panel_load_prov(&s_panel, prov, prov_len);
    const size_t nodes = zw_node_count(&s_panel.net.nodes);
    zone_service_unlock();
    release_blob(nodes_blob, len, s_blob);
    free_blob(garage, garage_len);
    free_blob(prov, prov_len);
    zw_wipe(setup, sizeof(*setup));
    heap_caps_free(setup);
    heap_caps_free(checkins);
    if (!loaded) {
        ESP_LOGE(TAG, "the stored node cache is damaged: devices are interviewed again");
    }
    if (!garage_ok) {
        ESP_LOGE(TAG, "the stored garage doors are damaged: set them again on their relay's page");
    }
    if (!prov_ok) {
        ESP_LOGE(TAG, "the stored SmartStart list is damaged: register its devices again");
    }

    zwf_init(&s_feedback);
    size_t feedback_len = 0u;
    uint8_t *feedback_blob = load_blob("keypad_fb", LOAD_BLOB_MAX(ZWF_BLOB_SIZE), &feedback_len);
    if (feedback_blob != NULL && !zwf_decode(&s_feedback, feedback_blob, feedback_len)) {
        ESP_LOGE(TAG, "stored keypad feedback assignments invalid: disabled");
    }
    free_blob(feedback_blob, feedback_len);
    s_feedback_view = xQueueCreate(1, sizeof(zwf_view_t));
    s_feedback_events = xQueueCreate(16, sizeof(alarm_event_t));
    ESP_RETURN_ON_FALSE(s_feedback_view != NULL && s_feedback_events != NULL, ESP_ERR_NO_MEM, TAG,
                        "keypad feedback mailbox");
    const alarm_listener_t listener = {.on_event = feedback_event, .on_snapshot = feedback_snapshot};
    ESP_RETURN_ON_ERROR(alarm_service_add_listener(&listener), TAG, "keypad feedback listener");
    alarm_snapshot_t *snapshot = heap_caps_malloc(sizeof(*snapshot), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_NO_MEM, TAG, "keypad feedback snapshot");
    alarm_service_get_snapshot(snapshot);
    feedback_snapshot(NULL, snapshot);
    heap_caps_free(snapshot);

    if (s_out_q == NULL) {
        /* PSRAM when there is any (internal RAM is the reserve, docs/bugs/B001): no ISR uses it */
        s_out_q = xQueueCreateWithCaps(OUTPUT_POSTS, sizeof(output_post_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_out_q == NULL) {
            s_out_q = xQueueCreate(OUTPUT_POSTS, sizeof(output_post_t));
        }
    }
    ESP_RETURN_ON_FALSE(s_out_q != NULL, ESP_ERR_NO_MEM, TAG, "device command queue");
    if (xTaskCreatePinnedToCore(zwave_task, "zwave", TASK_STACK, NULL, TASK_PRIORITY, &S.task,
                                tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = zw_link_start(CONFIG_ALARM_ZWAVE_UART_NUM, S.cfg.tx, S.cfg.rx, S.task);
    if (err != ESP_OK) {
        vTaskDelete(S.task);
        S.task = NULL;
        ESP_LOGE(TAG, "UART%d on GPIO %d/%d would not open (%s): the radio stays off",
                 CONFIG_ALARM_ZWAVE_UART_NUM, (int)S.cfg.tx, (int)S.cfg.rx, esp_err_to_name(err));
        return err;
    }
    S.running = true;
    xTaskNotifyGive(S.task); /* the network starts */
    ESP_LOGI(TAG, "Z-Wave radio on the %s (%s place): UART%d TX GPIO %d, RX GPIO %d, reset %s%d; "
                  "%u device(s) stored; crypto self-test passed",
             S.where, S.place, CONFIG_ALARM_ZWAVE_UART_NUM, (int)S.cfg.tx, (int)S.cfg.rx,
             S.cfg.reset >= 0 ? "GPIO " : "none ", (int)S.cfg.reset, (unsigned)nodes);
    return ESP_OK;
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
