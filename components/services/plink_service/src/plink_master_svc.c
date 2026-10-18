/*
 * plink_master_svc.c - the panel link's master side on a panel (docs/features/11, BL-048): the
 * master's link manager (plink_master.h) on the link task, fed by the engine and serving its
 * keypads.
 *
 *   alarm listener   on the alarm task, hands over only: a flag for a new snapshot (the link task
 *                    copies the newest with alarm_service_get_snapshot), events and one-shot sounds
 *                    into queues that never wait (a full one drops, counted)
 *   worker           its own task: the keypads' commands, code checks and code changes on the
 *                    alarm task as {ALARM_SRC_KEYPAD, n} (codes wiped after use), each keypad's
 *                    lockout read after its command, the keypads' configuration copy rebuilt when
 *                    the engine's changes (plink_cfg_for_keypads, two blobs: the manager keeps
 *                    using one while the other is made), and the supervision edges
 *                    (alarm_service_device_reachable may wait for the alarm task's queue);
 *                    the link task itself never waits for the alarm task (LINK-027)
 *   plink_dev        Smart Home for the keypads (plink_home_svc.c, BL-216): the device builds and
 *                    the keypads' device actions, answered on the worker's result queue
 *   keypad updates   plink_fwm_svc.c (BL-048): the planner on the link task, its keypads'
 *                    update stages posted to Activity by the worker
 *   link task        everything else: the manager, mDNS (announced only while in use, LINK-021),
 *                    the time and its zone, the master's reporting for the Emergency words, the
 *                    network trouble bit and Wi-Fi power save while keypads are paired, the copies
 *                    the UI and the console read
 *
 * The keypads' device numbers 1..8 are shared with the Zigbee keypads (LINK-001): the engine's
 * enabled device-table entries (read with the configuration) and Zigbee's held numbers are taken;
 * the link's own go to Zigbee whenever they change.
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm_mem.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "net_wifi.h"
#include "panel_link/plink_view.h"
#include "panel_prefs.h"
#if CONFIG_ALARM_FEATURE_HA
#include "ha_link.h"
#endif
#if CONFIG_ALARM_FEATURE_MQTT
#include "net_mqtt.h"
#endif

static const char *TAG = "plink";

#define EV_QUEUE        16u
#define SOUND_QUEUE     8u
#define JOB_SLOTS       16u /* 8 keypads, one command and one panic each (LINK-027) */
/* The worker's answers and config builds, and plink_dev's: an answer per device job, and at most
 * two builds (two buffers: one the manager uses, one waiting here). */
#define RES_QUEUE       (JOB_SLOTS + 2u + LINK_DEV_JOBS + 2u)
#define WORKER_STACK    4096u
#define WORKER_PRIORITY 4
#define TIME_CHECK_MS   5000u
#define REPORT_CHECK_MS 1000u
#define NET_CHECK_MS    1000u
#define PUBLISH_MS      500u
#define CFG_RETRY_MS    1000u
#define CLOCK_STEP_MS   2000
#define EV_HOLD_MS      1000u /* the longest an event waits for the snapshot that covers it */

typedef struct {
    uint8_t partition;
    uint8_t sound;
    uint16_t zone;
    uint32_t after_seq; /* the newest event handed over before it: its engine call's */
} sound_t;

/* ---- the alarm listener's hand-overs (alarm task -> link task) ------------------------------ */
static atomic_bool s_snap_news;
static QueueHandle_t s_ev_q;
static QueueHandle_t s_sound_q;
static atomic_uint s_dropped;
static atomic_uint s_ev_handed; /* the newest event seq on_event saw (the alarm task's order) */

/* ---- the link task's ------------------------------------------------------------------------- */
static plink_master_t *s_m;      /* PSRAM */
static alarm_snapshot_t *s_snap; /* the newest snapshot copy, PSRAM */
static bool s_have_snap;
static uint64_t s_ev_held_since; /* an event newer than s_snap waits (take_engine_news); 0: none */
static uint32_t s_ev_held_seq;   /* ... this one */
static uint32_t s_ev_pushed;     /* the newest event handed to the manager */
static alarm_event_t *s_ring;   /* events_after's copy of the engine's ring, PSRAM */
static uint32_t s_cfg_seq_done; /* the engine's config_seq the keypads' copy was made for */
static bool s_cfg_done;
static bool s_cfg_busy; /* a rebuild runs on the worker */
static uint64_t s_cfg_retry_at;
static int s_cfg_active = -1; /* the blob the manager uses */
static char s_name[PLINK_NAME_MAX + 1];
static uint8_t s_report;
static bool s_net_up;
static char s_tz[PLINK_TZ_MAX + 1];
static int64_t s_epoch_base;
static uint64_t s_mono_base;
static uint64_t s_time_at, s_report_at, s_net_at, s_publish_at;
static bool s_mdns_on, s_mdns_pair;
static uint16_t s_mdns_net;
static bool s_holding;     /* NET_WIFI_REQ_LINK and the low-latency hold */
static bool s_pair_forced; /* `plink pair force`: not closed with installer mode */
static bool s_kp_forced;   /* `plink kp force`: Option C on a bench although the setup is done */
static uint32_t s_dropped_told;
static uint32_t s_id_taken_told; /* keypads (bit d-1) whose panel id another one reports too */

/* ---- the worker's ---------------------------------------------------------------------------- */
typedef struct {
    bool used;
    uint32_t order;
    uint32_t ticket;
    uint8_t device;
    plink_cmd_t cmd; /* holds codes until the worker wipes it */
} job_t;

enum { RES_CMD = 1, RES_CONFIG, RES_DEVICES, RES_DEV_ACK };
typedef struct {
    uint8_t kind;
    uint32_t ticket;    /* RES_CMD, RES_DEV_ACK */
    plink_result_t res; /* RES_CMD */
    bool ok;            /* RES_CONFIG */
    uint8_t blob;       /* RES_CONFIG, RES_DEVICES: the buffer */
    size_t len;         /* RES_CONFIG, RES_DEVICES */
    uint32_t crc;       /* RES_CONFIG, RES_DEVICES */
    uint32_t cfg_seq;
    uint8_t dev_result; /* RES_DEV_ACK: plink_dev_result_t */
    uint8_t dev_why;    /* RES_DEV_ACK: the alarm_result_t of a refused code */
    char name[PLINK_NAME_MAX + 1];
} wres_t;

static job_t *s_jobs; /* JOB_SLOTS, PSRAM */
static SemaphoreHandle_t s_job_lock;
static uint32_t s_job_order;
static TaskHandle_t s_worker;
static QueueHandle_t s_res_q;
static atomic_bool s_cfg_job;
static atomic_uint s_cfg_job_seq;
static atomic_uint s_cfg_job_blob;
static alarm_config_t *s_cfg;         /* the engine's configuration, PSRAM, the worker's */
static alarm_config_t *s_cfg_scratch; /* plink_cfg_for_keypads' scratch, PSRAM, the worker's */
static uint8_t *s_blob[2];            /* the keypads' copies, PSRAM */
static atomic_uint s_reach_dirty;     /* supervision edges to tell the engine (bit d-1) */
static atomic_uchar s_reach_home[PLINK_KEYPADS_MAX];
static atomic_uint s_reach_state;     /* ... and what to tell it (1 reachable) */
static atomic_uint s_enabled_devices; /* the engine's enabled device-table entries (bit d-1) */

/* Each keypad's lockout, read by the worker after its commands (LINK-027, KPD-008). */
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t left_ms;
    uint64_t at_ms;
} s_lockout[PLINK_KEYPADS_MAX + 1u];

/* The device numbers shared with Zigbee (wired by the start-up). */
static uint32_t (*s_others_held)(void);          /* waits for the zone lock: the worker's */
static bool (*s_others_held_now)(uint32_t *out); /* never waits: the link task's check */
static atomic_uint s_others;   /* their last answer, refreshed by the worker every second */
static TickType_t s_others_at; /* the worker's last refresh */
static void (*s_link_held)(uint32_t mask);
static uint32_t s_told; /* the numbers Zigbee was last told the link holds (the link task's) */

/* ---- the alarm listener (on the alarm task: hand over and return) ---------------------------- */

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    QueueHandle_t q = __atomic_load_n(&s_ev_q, __ATOMIC_ACQUIRE);
    if (q != NULL && xQueueSend(q, ev, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1u); /* the keypads fill the gap with EV_REQ */
    }
    atomic_store(&s_ev_handed, ev->seq);
}

static void on_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone)
{
    (void)ctx;
    QueueHandle_t q = __atomic_load_n(&s_sound_q, __ATOMIC_ACQUIRE);
    const sound_t s = {.partition = partition,
                       .sound = (uint8_t)sound,
                       .zone = zone,
                       .after_seq = atomic_load(&s_ev_handed)};
    if (q != NULL && xQueueSend(q, &s, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1u);
    }
}

static void on_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    (void)snap;
    atomic_store(&s_snap_news, true); /* the link task copies the newest itself */
}

static const alarm_listener_t s_listener = {
    .on_event = on_event,
    .on_sound = on_sound,
    .on_snapshot = on_snapshot,
};

const alarm_listener_t *plink_service_listener(void)
{
    /* The queues exist before the listener is handed out: the alarm task may call it at once. */
    if (s_ev_q == NULL) {
        QueueHandle_t ev =
            xQueueCreateWithCaps(EV_QUEUE, sizeof(alarm_event_t), alarm_mem_slow_caps());
        QueueHandle_t snd =
            xQueueCreateWithCaps(SOUND_QUEUE, sizeof(sound_t), alarm_mem_slow_caps());
        if (ev == NULL || snd == NULL) {
            if (ev != NULL) {
                vQueueDeleteWithCaps(ev);
            }
            if (snd != NULL) {
                vQueueDeleteWithCaps(snd);
            }
            return &s_listener; /* registered, handing over nothing: the link stays quiet */
        }
        __atomic_store_n(&s_sound_q, snd, __ATOMIC_RELEASE);
        __atomic_store_n(&s_ev_q, ev, __ATOMIC_RELEASE);
    }
    return &s_listener;
}

void plink_service_set_devices_port(uint32_t (*others_held)(void),
                                    bool (*others_held_now)(uint32_t *out),
                                    void (*link_held)(uint32_t mask))
{
    s_others_held = others_held;
    s_others_held_now = others_held_now;
    s_link_held = link_held;
}

/* The Zigbee keypads' numbers, read under the zone lock - which a zone source holds while it posts
 * to the alarm task - so only off the link task (LINK-027): at start-up, then by the worker at most
 * once a second. */
static void refresh_others(void)
{
    if (s_others_held != NULL) {
        atomic_store(&s_others, s_others_held());
    }
}

/* ---- the worker ------------------------------------------------------------------------------ */

static void worker_kick(void)
{
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker);
    }
}

void msvc_kick(void)
{
    worker_kick();
}

static bool take_job(job_t *out)
{
    bool got = false;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    job_t *first = NULL;
    for (size_t i = 0; i < JOB_SLOTS; i++) {
        if (s_jobs[i].used && (first == NULL || (int32_t)(s_jobs[i].order - first->order) < 0)) {
            first = &s_jobs[i];
        }
    }
    if (first != NULL) {
        *out = *first;
        plink_wipe(first, sizeof *first); /* the codes leave the slot (LINK-018) */
        got = true;
    }
    xSemaphoreGive(s_job_lock);
    return got;
}

typedef struct {
    alarm_source_t src;
    alarm_ms_t left;
} lockout_ctx_t;

static void lockout_on_alarm_task(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    lockout_ctx_t *c = ctx;
    c->left = alarm_engine_lockout_left(e, &c->src, now);
}

void msvc_lockout_refresh(uint8_t device)
{
    lockout_ctx_t lc = {.src = {.kind = ALARM_SRC_KEYPAD, .id = device}, .left = 0};
    if (device <= PLINK_KEYPADS_MAX && alarm_service_call(lockout_on_alarm_task, &lc) == ESP_OK) {
        portENTER_CRITICAL(&s_lock_mux);
        s_lockout[device].left_ms = lc.left > UINT32_MAX ? UINT32_MAX : (uint32_t)lc.left;
        s_lockout[device].at_ms = link_now();
        portEXIT_CRITICAL(&s_lock_mux);
    }
}

static void run_job(job_t *job)
{
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = job->device};
    plink_cmd_t *c = &job->cmd;
    plink_result_t res;
    memset(&res, 0, sizeof res);
    res.req = c->req;
    alarm_result_t r = ALARM_ERR_UNAVAILABLE;
    switch (c->op) {
    case PLINK_OP_COMMAND: {
        alarm_cmd_result_t d;
        memset(&d, 0, sizeof d);
        r = alarm_service_command(&c->cmd, &src, &d);
        res.user = d.user;
        res.not_ready_zone = d.not_ready_zone;
        res.may_disarm = d.may_disarm ? 1u : 0u;
        break;
    }
    case PLINK_OP_AUTH: {
        uint8_t user = 0;
        r = alarm_service_authenticate(&src, c->cmd.code, c->cmd.partition, &user);
        res.user = user;
        break;
    }
    case PLINK_OP_CHANGE_CODE: {
        /* As the panel's own screen does it (ui_port_esp change_code): the code first, then the
         * keypad's "code + CODE + user", which keeps the user's authority and name. */
        uint8_t user = 0;
        r = alarm_service_authenticate(&src, c->cmd.code, c->cmd.partition, &user);
        if (r == ALARM_OK) {
            alarm_keyseq_result_t ks;
            memset(&ks, 0, sizeof ks);
            ks.kind = ALARM_KS_USER_SET;
            ks.cmd.partition = c->cmd.partition;
            memcpy(ks.cmd.code, c->cmd.code, sizeof ks.cmd.code);
            ks.user_no = user;
            memcpy(ks.new_code, c->new_code, sizeof ks.new_code);
            r = alarm_service_keypad_execute(&ks, &src, NULL);
            plink_wipe(&ks, sizeof ks);
        }
        res.user = user;
        break;
    }
    default: break; /* the manager checked the op: nothing else gets here */
    }
    plink_wipe(c, sizeof *c);
    res.result = (uint8_t)r;
    /* This keypad's lockout now (a wrong code may have started it): its STATUS says so. */
    msvc_lockout_refresh(job->device);
    const wres_t out = {.kind = RES_CMD, .ticket = job->ticket, .res = res};
    xQueueSend(s_res_q, &out, portMAX_DELAY); /* the link task drains it every turn */
}

void msvc_post_devices(uint8_t buf, size_t len, uint32_t crc)
{
    const wres_t out = {.kind = RES_DEVICES, .blob = buf, .len = len, .crc = crc};
    xQueueSend(s_res_q, &out, portMAX_DELAY); /* the link task drains it every turn */
}

void msvc_post_dev_ack(uint32_t ticket, uint8_t result, uint8_t why)
{
    const wres_t out = {
        .kind = RES_DEV_ACK, .ticket = ticket, .dev_result = result, .dev_why = why};
    xQueueSend(s_res_q, &out, portMAX_DELAY);
}

static void build_config(void)
{
    wres_t out;
    memset(&out, 0, sizeof out);
    out.kind = RES_CONFIG;
    out.cfg_seq = atomic_load(&s_cfg_job_seq);
    out.blob = (uint8_t)atomic_load(&s_cfg_job_blob);
    if (alarm_service_get_config(s_cfg) == ESP_OK) {
        uint32_t enabled = 0u;
        for (unsigned d = 0; d < ALARM_MAX_DEVICES && d < 32u; d++) {
            if (s_cfg->devices[d].enabled) {
                enabled |= 1u << d;
            }
        }
        atomic_store(&s_enabled_devices, enabled);
        /* The installation's name: the setup's "Name your system" names partition 1 (as
         * ha_link). */
        const char *name = s_cfg->partitions[0].name;
        snprintf(out.name, sizeof out.name, "%.*s", (int)PLINK_NAME_MAX,
                 name[0] != '\0' ? name : LINK_DEFAULT_NAME);
        out.ok = plink_cfg_for_keypads(s_cfg, s_cfg_scratch, s_blob[out.blob],
                                       ALARM_CONFIG_BLOB_MAX, &out.len, &out.crc) == ALARM_OK;
        /* the copies hold code hashes and the salt: nothing of them stays */
        plink_wipe(s_cfg, sizeof *s_cfg);
        plink_wipe(s_cfg_scratch, sizeof *s_cfg_scratch);
    }
    xQueueSend(s_res_q, &out, portMAX_DELAY);
}

static void flush_edges(void)
{
    const uint32_t dirty = atomic_exchange(&s_reach_dirty, 0u);
    const uint32_t state = atomic_load(&s_reach_state);
    for (uint8_t d = 1; d <= PLINK_KEYPADS_MAX; d++) {
        if ((dirty & (1u << (d - 1u))) != 0u) {
            alarm_service_device_reachable_at(d, atomic_load(&s_reach_home[d - 1u]),
                                              (state & (1u << (d - 1u))) != 0u);
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (xTaskGetTickCount() - s_others_at >= pdMS_TO_TICKS(1000)) {
            s_others_at = xTaskGetTickCount();
            refresh_others();
        }
        flush_edges();
        fsvc_worker(); /* the keypads' update stages for Activity (they wait for the alarm task) */
        if (atomic_exchange(&s_cfg_job, false)) {
            build_config();
        }
        job_t job;
        while (take_job(&job)) {
            run_job(&job);
            plink_wipe(&job, sizeof job);
            flush_edges(); /* a trouble edge never waits behind a queue of commands */
        }
    }
}

/* ---- the manager's port (the link task) ---------------------------------------------------- */

static bool port_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return link_send(to, frame, len);
}

/* A new keypad's number, before its pairing is answered (LINK-001): the manager chose it from the
 * Zigbee numbers the worker read up to a second ago, and Zigbee chooses from the link's numbers it
 * was last told. So the number is reserved with Zigbee at once (its later choices skip it), then
 * Zigbee's numbers are read again without waiting: one Zigbee gave out meanwhile ends this pairing
 * ("Nothing was saved"; the next try takes another). Left open: a Zigbee keypad given the same
 * number within that second while the zone lock is busy right now - the check can't be made
 * without waiting for it, and the link task never waits (LINK-027). False: not reserved. */
static bool reserve_new(uint32_t added, uint32_t numbers)
{
    if (s_link_held == NULL) {
        return true;
    }
    s_link_held(s_told | numbers);
    uint32_t zigbee = 0u;
    if (s_others_held_now != NULL && s_others_held_now(&zigbee)) {
        atomic_store(&s_others, zigbee);
        if ((zigbee & added) != 0u) {
            s_link_held(s_told); /* released */
            ESP_LOGW(TAG, "keypad number %u was just given to a Zigbee keypad: pairing ended",
                     (unsigned)__builtin_ctz(zigbee & added) + 1u);
            return false;
        }
    }
    return true;
}

static bool port_store(void *ctx, const plink_master_rec_t *rec)
{
    (void)ctx;
    uint32_t numbers = 0u;
    for (uint8_t i = 0; i < rec->count && i < PLINK_KEYPADS_MAX; i++) {
        const uint8_t d = rec->peer[i].device;
        if (d >= PLINK_DEVICE_FIRST && d <= PLINK_DEVICE_LAST) {
            numbers |= 1u << (d - 1u);
        }
    }
    const uint32_t added = numbers & ~s_told;
    if (added != 0u && !reserve_new(added, numbers)) {
        return false;
    }
    uint8_t buf[PLINK_STORE_MAX]; /* on the internal stack: NVS reads it with the cache off */
    size_t len = 0;
    bool ok = plink_store_master_encode(rec, buf, sizeof buf, &len) &&
              link_nvs_write(LINK_KEY_MASTER, buf, len) == ESP_OK;
    plink_wipe(buf, sizeof buf);
    if (!ok) {
        ESP_LOGE(TAG, "the keypads' record could not be stored");
        if (added != 0u && s_link_held != NULL) {
            s_link_held(s_told); /* the reservation goes with the pairing */
        }
    }
    return ok;
}

static bool port_run(void *ctx, uint32_t ticket, uint8_t device, const plink_cmd_t *cmd)
{
    (void)ctx;
    bool placed = false;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    for (size_t i = 0; i < JOB_SLOTS; i++) {
        if (!s_jobs[i].used) {
            s_jobs[i].used = true;
            s_jobs[i].order = s_job_order++;
            s_jobs[i].ticket = ticket;
            s_jobs[i].device = device;
            s_jobs[i].cmd = *cmd;
            placed = true;
            break;
        }
    }
    xSemaphoreGive(s_job_lock);
    if (placed) {
        worker_kick();
    }
    return placed; /* false: the keypad's retry comes */
}

static void port_reachable(void *ctx, uint8_t device, bool reachable)
{
    (void)ctx;
    if (device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST) {
        return;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (s_m->peer[i].used && s_m->peer[i].rec.device == device) {
            atomic_store(&s_reach_home[device - 1u], s_m->peer[i].rec.home);
            break;
        }
    }
    const uint32_t bit = 1u << (device - 1u);
    if (reachable) {
        atomic_fetch_or(&s_reach_state, bit);
    } else {
        atomic_fetch_and(&s_reach_state, ~bit);
    }
    atomic_fetch_or(&s_reach_dirty, bit);
    ESP_LOGW(TAG, "keypad %u %s", (unsigned)device, reachable ? "heard again" : "not responding");
    worker_kick();
}

/* The numbers others hold (LINK-001): the device table's enabled entries (from the last
 * configuration read) and the Zigbee keypads' (refresh_others, reserve_new). Never waits. */
static uint32_t port_devices_taken(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_enabled_devices) | atomic_load(&s_others);
}

static void port_held_changed(void *ctx, uint32_t held)
{
    (void)ctx;
    s_told = held;
    if (s_link_held != NULL) {
        s_link_held(held);
    }
}

static uint32_t port_lockout_left(void *ctx, uint8_t device, uint64_t now_ms)
{
    (void)ctx;
    if (device > PLINK_KEYPADS_MAX) {
        return 0u;
    }
    portENTER_CRITICAL(&s_lock_mux);
    const uint32_t left = s_lockout[device].left_ms;
    const uint64_t at = s_lockout[device].at_ms;
    portEXIT_CRITICAL(&s_lock_mux);
    const uint64_t gone = now_ms > at ? now_ms - at : 0u;
    return gone >= left ? 0u : left - (uint32_t)gone;
}

/* The newest events after after_seq, oldest first, from the engine's ring (newest first there). */
static size_t port_events_after(void *ctx, uint32_t after_seq, alarm_event_t *out, size_t max,
                                bool *gap)
{
    (void)ctx;
    const size_t n = alarm_service_recent_events(s_ring, CONFIG_ALARM_EVENT_RING);
    size_t newer = 0;
    while (newer < n && s_ring[newer].seq > after_seq) {
        newer++;
    }
    /* left out: more than max, or the ring no longer reaches back to after_seq */
    *gap = newer > max || (newer == n && n > 0u && s_ring[n - 1u].seq > after_seq + 1u);
    const size_t take = newer < max ? newer : max;
    for (size_t i = 0; i < take; i++) {
        out[i] = s_ring[take - 1u - i];
    }
    return take;
}

static void port_note(void *ctx, plink_note_t note, uint8_t device)
{
    (void)ctx;
    switch (note) {
    case PLINK_NOTE_PAIRED: ESP_LOGW(TAG, "keypad %u paired", (unsigned)device); break;
    case PLINK_NOTE_PAIR_LIMIT: ESP_LOGW(TAG, "pairing closed: 3 attempts used"); break;
    case PLINK_NOTE_PAIR_OVERLAP:
        ESP_LOGW(TAG, "pairing ended: more than one keypad at once");
        break;
    case PLINK_NOTE_REMOVED: ESP_LOGW(TAG, "keypad %u removed", (unsigned)device); break;
    case PLINK_NOTE_CLONE:
        ESP_LOGE(TAG, "a second device is using keypad %u's keys", (unsigned)device);
        break;
    case PLINK_NOTE_PROVISIONAL_DROPPED:
        ESP_LOGW(TAG, "keypad %u never connected after pairing: dropped", (unsigned)device);
        break;
    case PLINK_NOTE_STORE_FAILED: ESP_LOGE(TAG, "the keypads' record could not be written"); break;
    default: break;
    }
}

/* ---- the link task: hand-overs, timers, publishing ------------------------------------------- */

static bool installer_ok(void)
{
    if (!s_have_snap || !s_snap->program) {
        return false;
    }
    for (uint8_t p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s_snap->partitions & (1u << p)) != 0u &&
            s_snap->part[p].state != ALARM_STATE_DISARMED) {
            return false;
        }
    }
    return true;
}

/* The master's reporting for the keypads' Emergency words (UI-029), as the panel's own screen
 * works it out (ui_app.c report_state): something set up, and whether it's online. */
static uint8_t report_now(void)
{
    bool set_up = false;
    bool online = false;
#if CONFIG_ALARM_FEATURE_HA
    ha_link_status_t h;
    ha_link_get_status(&h);
    if (h.enabled && h.paired) {
        set_up = true;
        online = online || h.state == HA_LINK_CONNECTED;
    }
#endif
#if CONFIG_ALARM_FEATURE_MQTT
    net_mqtt_status_t q;
    net_mqtt_get_status(&q);
    if (q.state != NET_MQTT_OFF) {
        set_up = true;
        online = online || q.state == NET_MQTT_CONNECTED;
    }
#endif
    return !set_up ? PLINK_REPORT_NONE : online ? PLINK_REPORT_ONLINE : PLINK_REPORT_OFFLINE;
}

/* The engine's news: the newest snapshot is copied first and the events drained after it, so every
 * event the snapshot counts is pushed before the STATUS that carries its seq (docs/features/11);
 * then the one-shot sounds, then the STATUS: a keypad's voice closes the batch at the STATUS that
 * covers its events, its acknowledgement or error tone already in (LINK-028). */
static void take_engine_news(uint64_t now)
{
    const bool news = atomic_exchange(&s_snap_news, false);
    if (news) {
        alarm_service_get_snapshot(s_snap);
        s_have_snap = true;
    }
    /* An engine call hands its events over as it runs and its snapshot only after its store
     * flush: an event newer than the snapshot copied waits for the turn whose news covers it, so
     * no STATUS covers an event with the state from before it (a keypad's voice would decide on
     * that state; LINK-028). Every call that makes events ends with a new snapshot; EV_HOLD_MS
     * bounds each event's wait all the same. A sound goes with its call's events: once the event
     * handed over before it has gone, or at once when none waits. */
    QueueHandle_t evq = __atomic_load_n(&s_ev_q, __ATOMIC_ACQUIRE);
    alarm_event_t ev;
    bool held = false;
    while (evq != NULL && xQueuePeek(evq, &ev, 0) == pdTRUE) {
        if (s_have_snap && ev.seq > s_snap->last_event_seq) {
            if (s_ev_held_since == 0u || s_ev_held_seq != ev.seq) {
                s_ev_held_since = now;
                s_ev_held_seq = ev.seq;
            }
            if (now - s_ev_held_since < EV_HOLD_MS) {
                held = true;
                break;
            }
        }
        if (xQueueReceive(evq, &ev, 0) != pdTRUE) {
            break;
        }
        plink_master_event(s_m, now, &ev);
        s_ev_pushed = ev.seq;
    }
    if (!held) {
        s_ev_held_since = 0u;
    }
    QueueHandle_t sq = __atomic_load_n(&s_sound_q, __ATOMIC_ACQUIRE);
    sound_t s;
    while (sq != NULL && xQueuePeek(sq, &s, 0) == pdTRUE) {
        if (held && (int32_t)(s.after_seq - s_ev_pushed) > 0) {
            break; /* a later call's: it waits with its events */
        }
        if (xQueueReceive(sq, &s, 0) != pdTRUE) {
            break;
        }
        plink_master_sound(s_m, now, s.partition, s.sound, s.zone);
    }
    if (news) {
        plink_master_snapshot(s_m, now, s_snap, link_epoch_ms(), s_report);
        if (s_pair_forced || !plink_master_pairing(s_m)) {
            s_pair_forced = s_pair_forced && plink_master_pairing(s_m);
        } else if (!installer_ok()) {
            /* LINK-004: pairing needs the installer at the master; leaving installer mode, or
             * arming, ends it */
            plink_master_pair_cancel(s_m, now);
        }
    }
    const uint32_t dropped = atomic_load(&s_dropped);
    if (dropped != s_dropped_told) {
        s_dropped_told = dropped;
        ESP_LOGW(TAG, "%u of the engine's events or sounds not handed to the link so far",
                 (unsigned)dropped);
    }
}

static void take_results(uint64_t now)
{
    wres_t r;
    while (xQueueReceive(s_res_q, &r, 0) == pdTRUE) {
        if (r.kind == RES_CMD) {
            plink_master_result(s_m, now, r.ticket, &r.res);
            continue;
        }
        if (r.kind == RES_DEVICES) {
            hsvc_devices_in(s_m, now, r.blob, r.len, r.crc);
            continue;
        }
        if (r.kind == RES_DEV_ACK) {
            /* after the build plink_dev posted before it: the DEV_ACK's record shows the act */
            plink_master_dev_answer(s_m, now, r.ticket, r.dev_result, r.dev_why);
            continue;
        }
        s_cfg_busy = false;
        if (!r.ok) {
            s_cfg_retry_at = now + CFG_RETRY_MS;
            ESP_LOGE(TAG, "the keypads' copy of the configuration couldn't be made");
            continue;
        }
        plink_master_config(s_m, s_blob[r.blob], r.len, r.crc);
        s_cfg_active = r.blob;
        s_cfg_seq_done = r.cfg_seq;
        s_cfg_done = true;
        if (strcmp(r.name, s_name) != 0) {
            snprintf(s_name, sizeof s_name, "%s", r.name);
            plink_master_name(s_m, s_name);
        }
    }
}

/* The keypads' copy follows the engine's configuration (snapshot config_seq). */
static void ask_config(uint64_t now)
{
    if (s_cfg_busy || !s_have_snap || now < s_cfg_retry_at ||
        (s_cfg_done && s_snap->config_seq == s_cfg_seq_done)) {
        return;
    }
    s_cfg_busy = true;
    atomic_store(&s_cfg_job_seq, s_snap->config_seq);
    atomic_store(&s_cfg_job_blob, s_cfg_active == 0 ? 1u : 0u); /* never the one in use */
    atomic_store(&s_cfg_job, true);
    worker_kick();
}

static void check_time(uint64_t now, bool force)
{
    const char *tz = getenv("TZ");
    const int64_t epoch = link_epoch_ms();
    const int64_t expected = s_epoch_base != 0 ? s_epoch_base + (int64_t)(now - s_mono_base) : 0;
    const bool stepped = epoch != 0 && (expected == 0 || llabs(epoch - expected) > CLOCK_STEP_MS);
    const bool tz_changed = tz != NULL && strncmp(tz, s_tz, sizeof s_tz) != 0;
    s_epoch_base = epoch;
    s_mono_base = now;
    if (force || stepped || tz_changed) {
        if (tz != NULL) {
            snprintf(s_tz, sizeof s_tz, "%s", tz);
        }
        plink_master_time(s_m, now, epoch, s_tz);
    }
}

static void mdns_txt(bool pairing, uint16_t net)
{
    char net_hex[8];
    snprintf(net_hex, sizeof net_hex, "%04x", (unsigned)net);
    mdns_txt_item_t txt[] = {
        {"v", "1"},
        {"net", net_hex},
        {"pair", pairing ? "1" : "0"},
        {"id", g_link.panel_id},
    };
    mdns_service_txt_set(LINK_MDNS_SERVICE, LINK_MDNS_PROTO, txt, sizeof txt / sizeof txt[0]);
    s_mdns_pair = pairing;
    s_mdns_net = net;
}

/* LINK-021: announced only while the link is in use (a keypad paired or pairing open). */
static void mdns_apply(void)
{
    const bool want = plink_master_in_use(s_m);
    const bool pairing = plink_master_pairing(s_m);
    const uint16_t net = plink_master_net_id(s_m);
    if (want && !s_mdns_on) {
        if (!s_net_up || !link_mdns_init()) {
            return;
        }
        if (mdns_service_add(NULL, LINK_MDNS_SERVICE, LINK_MDNS_PROTO, PLINK_UDP_PORT, NULL, 0) ==
            ESP_OK) {
            mdns_txt(pairing, net);
            s_mdns_on = true;
        }
    } else if (!want && s_mdns_on) {
        mdns_service_remove(LINK_MDNS_SERVICE, LINK_MDNS_PROTO); /* only ours: mDNS is shared */
        s_mdns_on = false;
    } else if (s_mdns_on && (pairing != s_mdns_pair || net != s_mdns_net)) {
        mdns_txt(pairing, net);
    }
}

/* While keypads are paired: a lost network is the network trouble (NET-003, the master's own
 * supervision of them pauses meanwhile) and Wi-Fi power save stays off (LINK-024). */
static void apply_holds(void)
{
    const bool keypads = plink_master_held(s_m) != 0u;
    if (keypads != s_holding) {
        s_holding = keypads;
        net_wifi_set_required(NET_WIFI_REQ_LINK, keypads);
        net_wifi_hold_low_latency(NET_WIFI_LATENCY_LINK, keypads);
    }
}

static void publish(uint64_t now)
{
    static plink_peer_info_t peers[PLINK_KEYPADS_MAX] ALARM_SLOW_BSS; /* the link task's */
    const size_t n = plink_master_peers(s_m, now, peers, PLINK_KEYPADS_MAX);
    plink_pair_info_t pi;
    plink_master_pair_info(s_m, now, &pi);
    const int64_t epoch = link_epoch_ms();
    bool home_wanted = false;
    uint32_t id_taken = 0u;
    link_pub_lock();
    g_pub.net_id = plink_master_net_id(s_m);
    g_pub.pairing_open = plink_master_pairing(s_m);
    g_pub.keypads = n;
    g_pub.keypads_down = 0;
    for (size_t i = 0; i < n; i++) {
        const plink_peer_info_t *p = &peers[i];
        ui_link_keypad_t *k = &g_pub.keypad[i];
        memset(k, 0, sizeof *k);
        k->device = p->device;
        snprintf(k->name, sizeof k->name, "%s", p->name);
        k->home = p->home;
        k->supervision_s = p->supervision_s;
        k->connected = p->online;
        k->down = p->down;
        k->waiting = p->provisional;
        k->heard_epoch_ms = (p->heard_ago_s != UINT32_MAX && epoch != 0)
                                ? epoch - (int64_t)p->heard_ago_s * 1000
                                : 0;
        link_addr_ip(&p->addr, k->addr);
        snprintf(k->fw, sizeof k->fw, "%s", p->hb.fw);
        g_pub.kind[i] = p->kind;
        g_pub.rssi[i] = p->hb.rssi;
        g_pub.caps[i] = p->caps;
        home_wanted = home_wanted || (p->caps & PLINK_CAP_HOME) != 0u;
        /* its Smart Home and settings side, the newest settings report with it (BL-216) */
        if (g_pub.home != NULL && plink_master_keypad_home(s_m, now, p->device, &g_pub.home[i]) &&
            g_pub.home[i].id_taken && p->device >= 1u && p->device <= 32u) {
            id_taken |= 1u << (p->device - 1u);
        }
        if (p->down) {
            g_pub.keypads_down++;
        }
    }
    /* each keypad's update, for the screens, the console and MQTT (BL-048) */
    const bool fw_news = fsvc_publish_locked(peers, n);
    if (!ksvc_active()) {
        /* the setup's own pairing as a keypad, while its manager exists, is published by
         * plink_keypad_svc.c; otherwise this master's own, always */
        g_pub.pair_valid = pi.state != PLINK_PS_IDLE;
        memset(&g_pub.pair, 0, sizeof g_pub.pair);
        g_pub.pair.state = pi.state;
        g_pub.pair.fail = pi.fail;
        g_pub.pair.attempts = pi.attempts;
        g_pub.pair.left_s = pi.left_s;
        snprintf(g_pub.pair.code, sizeof g_pub.pair.code, "%s", pi.code);
        snprintf(g_pub.pair.peer_name, sizeof g_pub.pair.peer_name, "%s", pi.peer_name);
        link_addr_ip(&pi.peer, g_pub.pair.peer_addr);
        g_pub.pair.device = pi.device;
    }
    link_pub_unlock();
#if CONFIG_ALARM_FEATURE_MQTT
    if (fw_news) {
        net_mqtt_firmware_changed(); /* .../firmware's keypads (its own task sends it) */
    }
#else
    (void)fw_news;
#endif
    hsvc_wanted(home_wanted); /* plink_dev builds only while a live keypad has HOME */
    if ((id_taken & ~s_id_taken_told) != 0u) {
        /* docs/features/11 "Settings over the link": Home Assistant gets its id as null */
        ESP_LOGW(TAG,
                 "keypad %u reports the panel id a lower keypad number reports: left out "
                 "of Home Assistant's list until that changes",
                 (unsigned)__builtin_ctz(id_taken & ~s_id_taken_told) + 1u);
    }
    s_id_taken_told = id_taken;
}

void msvc_poll(uint64_t now)
{
    take_engine_news(now);
    take_results(now);
    ask_config(now);
    if (now >= s_net_at) {
        s_net_at = now + NET_CHECK_MS;
        const bool up = link_wifi_up() || link_radio_ready();
        if (up != s_net_up) {
            s_net_up = up;
            plink_master_net(s_m, now, up); /* supervision pauses while it is down (LINK-007) */
        }
        if (ksvc_active() && panel_setup_done() && !s_kp_forced) {
            ksvc_setup_end(now); /* the setup ended as the alarm panel: its keypad pairing goes */
        }
    }
    if (now >= s_time_at) {
        s_time_at = now + TIME_CHECK_MS;
        check_time(now, false);
    }
    if (now >= s_report_at) {
        s_report_at = now + REPORT_CHECK_MS;
        const uint8_t report = report_now();
        if (report != s_report) {
            s_report = report;
            if (s_have_snap) {
                plink_master_snapshot(s_m, now, s_snap, link_epoch_ms(), s_report);
            }
        }
    }
    hsvc_poll(s_m, now); /* the settings writes queued for keypads (BL-216) */
    fsvc_poll(s_m, now); /* keypad updates: the planner's orders go at this tick (BL-048) */
    plink_master_tick(s_m, now);
    mdns_apply();
    apply_holds();
    if (now >= s_publish_at) {
        s_publish_at = now + PUBLISH_MS;
        publish(now);
    }
}

uint64_t msvc_next(void)
{
    return s_m != NULL ? plink_master_next(s_m) : UINT64_MAX;
}

void msvc_input(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len)
{
    plink_master_input(s_m, now, from, frame, len);
}

uint32_t msvc_stack_free(void)
{
    return s_worker != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_worker) : 0u;
}

/* The UI's and the console's requests (the link task). */
uint8_t msvc_request(uint64_t now, const link_req_t *req)
{
    uint8_t r = UI_LINK_REQ_OK;
    switch (req->kind) {
    case UI_LINK_REQ_PAIR_OPEN:
    case LINK_REQ_PAIR_FORCE: {
#if CONFIG_ALARM_BUILD_DEV
        const bool forced = req->kind == LINK_REQ_PAIR_FORCE;
#else
        const bool forced = false;
        if (req->kind == LINK_REQ_PAIR_FORCE) {
            return UI_LINK_REQ_REFUSED; /* a bench shortcut: development builds only */
        }
#endif
        if (!forced && !installer_ok()) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        if (!s_cfg_done || ksvc_active()) {
            /* the device table's numbers aren't read yet (LINK-001), or the setup's own pairing
             * as a keypad runs (a bench's `plink kp`): one pairing at a time */
            return UI_LINK_REQ_BUSY;
        }
        worker_kick(); /* Zigbee's numbers read again soon; a new one is checked when chosen */
        if (!plink_master_pair_open(s_m, now)) {
            const uint32_t taken = port_devices_taken(NULL) | plink_master_held(s_m);
            return (taken & 0xFFu) == 0xFFu ? UI_LINK_REQ_FULL : UI_LINK_REQ_FAILED;
        }
        s_pair_forced = forced;
        break;
    }
    case UI_LINK_REQ_PAIR_DECIDE:
        if (!s_pair_forced && !req->bench && !installer_ok()) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        plink_master_pair_decide(s_m, now, req->allow);
        break;
    case UI_LINK_REQ_PAIR_STOP:
        plink_master_pair_cancel(s_m, now); /* stopping is always allowed */
        s_pair_forced = false;
        break;
    case UI_LINK_REQ_KEYPAD_SET: {
        if (!req->bench && !installer_ok()) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        if (req->home < 1u || req->home > ALARM_MAX_PARTITIONS ||
            (req->supervision_s != 0u &&
             (req->supervision_s < 60u || req->supervision_s > 3600u))) {
            return UI_LINK_REQ_REFUSED;
        }
        const bool ok =
            (req->name[0] == '\0' || plink_master_rename(s_m, req->device, req->name)) &&
            plink_master_set_home(s_m, now, req->device, req->home) &&
            plink_master_set_supervision(s_m, now, req->device, req->supervision_s);
        r = ok ? UI_LINK_REQ_OK : UI_LINK_REQ_FAILED;
        break;
    }
    case UI_LINK_REQ_KEYPAD_REMOVE:
        if (!req->bench && !installer_ok()) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        r = plink_master_remove(s_m, now, req->device) ? UI_LINK_REQ_OK : UI_LINK_REQ_FAILED;
        break;
    case UI_LINK_REQ_KP_PAIR_START:
    case LINK_REQ_KP_PAIR_AT: /* `plink kp pair <ip>` on a bench */
    case UI_LINK_REQ_KP_PAIR_CONFIRM:
    case UI_LINK_REQ_KP_PAIR_RETRY:
    case UI_LINK_REQ_KP_PAIR_STOP:
    case UI_LINK_REQ_KP_DONE:
        /* The setup making this panel a keypad (Option C): it stays the master until the pairing
         * is done, with a keypad manager of its own for the pairing on the same socket. Only a
         * panel whose setup isn't done and that has no keypads. */
        if (req->kind == UI_LINK_REQ_KP_PAIR_STOP) {
            ksvc_setup_end(now); /* always allowed; nothing when no setup pairing runs */
            break;
        }
        if ((panel_setup_done() && !s_kp_forced) || plink_master_held(s_m) != 0u ||
            plink_master_pairing(s_m)) {
            return UI_LINK_REQ_REFUSED;
        }
        if (req->kind == UI_LINK_REQ_KP_DONE) {
            return ksvc_setup_done() ? UI_LINK_REQ_OK : UI_LINK_REQ_REFUSED; /* restarts */
        }
        if (!ksvc_active() && ksvc_prepare(now, true) != ESP_OK) {
            return UI_LINK_REQ_FAILED;
        }
        r = ksvc_request(now, req);
        break;
    case LINK_REQ_KP_FORCE:
#if CONFIG_ALARM_BUILD_DEV
        s_kp_forced = true; /* a bench shortcut: development builds only */
        break;
#else
        return UI_LINK_REQ_REFUSED;
#endif
    case LINK_REQ_FW_AGAIN:
        r = fsvc_request(s_m, now, req); /* `plink fw update <n>`: ask it again now */
        break;
    default: return UI_LINK_REQ_REFUSED;
    }
    publish(now);
    return r;
}

/* ---- start ----------------------------------------------------------------------------------- */

esp_err_t msvc_prepare(uint64_t now)
{
    s_m = alarm_mem_slow_calloc(1, sizeof *s_m);
    s_snap = alarm_mem_slow_calloc(1, sizeof *s_snap);
    s_ring = alarm_mem_slow_calloc(CONFIG_ALARM_EVENT_RING, sizeof *s_ring);
    s_jobs = alarm_mem_slow_calloc(JOB_SLOTS, sizeof *s_jobs);
    s_cfg = alarm_mem_slow_calloc(1, sizeof *s_cfg);
    s_cfg_scratch = alarm_mem_slow_calloc(1, sizeof *s_cfg_scratch);
    s_blob[0] = alarm_mem_slow_malloc(ALARM_CONFIG_BLOB_MAX);
    s_blob[1] = alarm_mem_slow_malloc(ALARM_CONFIG_BLOB_MAX);
    s_job_lock = xSemaphoreCreateMutex();
    s_res_q = xQueueCreateWithCaps(RES_QUEUE, sizeof(wres_t), alarm_mem_slow_caps());
    if (s_m == NULL || s_snap == NULL || s_ring == NULL || s_jobs == NULL || s_cfg == NULL ||
        s_cfg_scratch == NULL || s_blob[0] == NULL || s_blob[1] == NULL || s_job_lock == NULL ||
        s_res_q == NULL) {
        return ESP_ERR_NO_MEM; /* at start-up only: nothing is retried, the link stays off */
    }
    if (s_ev_q == NULL) {
        ESP_LOGE(TAG, "the link's listener was never registered: keypads would see nothing");
        return ESP_ERR_INVALID_STATE;
    }

    /* The stored record: the net id and the keypads (none before the first pairing). */
    static plink_master_rec_t rec ALARM_SLOW_BSS; /* the start-up's, then wiped */
    uint8_t buf[PLINK_STORE_MAX];
    size_t len = 0;
    bool have = false;
    if (link_nvs_read(LINK_KEY_MASTER, buf, sizeof buf, &len)) {
        have = plink_store_master_decode(buf, len, &rec);
        if (!have) {
            ESP_LOGE(TAG,
                     "the stored keypads' record can't be read: keeping it, starting with none");
        }
    }
    plink_wipe(buf, sizeof buf);
    snprintf(s_name, sizeof s_name, "%s", LINK_DEFAULT_NAME); /* until the configuration is read */
    refresh_others();
    /* Smart Home and settings on keypads (BL-216): before the manager, which takes the offer. */
    const esp_err_t home_err = hsvc_prepare();
    if (home_err != ESP_OK) {
        return home_err;
    }
    /* Keypad updates (BL-048): left out alone when there is no memory for them (keypads keep
     * their versions; the rest of the link runs). */
    (void)fsvc_prepare(now);
    /* on this stack: plink_master_init keeps a copy */
    plink_master_port_t port = {
        .ctx = NULL,
        .send = port_send,
        .mtu = link_mtu,
        .store = port_store,
        .run = port_run,
        .reachable = port_reachable,
        /* No check-ins: the reachable edges carry LINK-007 alone. A check-in would only repeat
         * reachable(true) (it clears the same trouble), once per heartbeat, and the engine call
         * can wait for the alarm task's queue, which the link task never does (LINK-027). */
        .checkin = NULL,
        .devices_taken = port_devices_taken,
        .held_changed = port_held_changed,
        .lockout_left = port_lockout_left,
        .events_after = port_events_after,
        .note = port_note,
        .prefs_done = hsvc_prefs_done,
    };
    /* Each capability is offered with the code that uses it (LINK-043): HOME with a device side
     * (NULL dev_run otherwise: an action is answered UNAVAILABLE), PREFS with the UI, FW with the
     * planner's service (keypad updates go live in each session whose keypad offers it too). */
    const uint16_t caps = (uint16_t)(hsvc_caps() | fsvc_caps());
    port.dev_run = (caps & PLINK_CAP_HOME) != 0u ? hsvc_dev_run : NULL;
    port.fw_read = (caps & PLINK_CAP_FW) != 0u ? fsvc_fw_read : NULL;
    port.ctx = s_m; /* hsvc_prefs_done reads the keypad's report back (the others need no ctx) */
    plink_master_init(s_m, &g_link.crypto, &port, have ? &rec : NULL, s_name, g_link.panel_id, caps,
                      now);
    plink_wipe(&rec, sizeof rec);
    /* The manager starts with the network up; the service says how it is now, so supervision
     * pauses at once on a master whose Wi-Fi isn't there yet and resumes one period after it is
     * (LINK-007): later only the changes are told. */
    s_net_up = link_wifi_up() || link_radio_ready();
    if (!s_net_up) {
        plink_master_net(s_m, now, false);
    }
    s_told = plink_master_held(s_m);
    if (s_link_held != NULL) {
        s_link_held(s_told); /* Zigbee knows the link's numbers from the start */
    }
    atomic_store(&s_snap_news, true); /* the current state at once, not at the next change */
    check_time(now, true);
    s_report = report_now();

    if (xTaskCreatePinnedToCore(worker_task, "plink_work", WORKER_STACK, NULL, WORKER_PRIORITY,
                                &s_worker, tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "master: %u keypad(s) paired; offers Smart Home %s, settings %s, updates %s",
             (unsigned)__builtin_popcount(plink_master_held(s_m)),
             (caps & PLINK_CAP_HOME) != 0u ? "yes" : "no",
             (caps & PLINK_CAP_PREFS) != 0u ? "yes" : "no",
             (caps & PLINK_CAP_FW) != 0u ? "yes" : "no");
    return ESP_OK;
}

#endif /* CONFIG_ALARM_LINK */
