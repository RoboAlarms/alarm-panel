/*
 * plink_home_svc.c - Smart Home and settings for the master's link keypads (docs/features/11
 * "Smart Home and Home Assistant on keypads"; LINK-044..051, BL-216).
 *
 *   plink_dev    its own task, a 4 KB PSRAM stack at priority 3 (below plink_work's 4: the
 *                keypads' alarm commands come first; LINK-049). It copies the registry
 *                (device_service_list) and builds the device list blob and its records
 *                (plink_home) into one of two alternating PSRAM buffers - when
 *                device_service_seq() moves (at most every 250 ms), at once after a device action,
 *                and every 2 s to compare, in case a change was missed - only while a live keypad
 *                has PLINK_CAP_HOME. It runs the keypads' device actions (8 slots) through the one
 *                judge of both screens, device_service_person_act({ALARM_SRC_KEYPAD, n}, the
 *                keypad's home partition; LINK-048), wipes the code copy, and reads that keypad's
 *                lockout after an action that carried a code (only a code moves it), so its STATUS
 *                shows one. Both go to the link task on the worker's result queue: the build
 *                right after an action (RES_DEVICES) before its answer (RES_DEV_ACK), so the
 *                DEV_ACK's record already shows the action under way. It never writes flash or
 *                NVS (its stack is PSRAM) and reaches the engine only through alarm_service's
 *                calls, which run on the alarm task.
 *   link task    hands the builds to the manager (plink_master_home), the answers
 *                (plink_master_dev_answer) and the settings writes plink_service_prefs_set queued
 *                (plink_master_prefs_set); a write's end comes back through port.prefs_done to its
 *                caller's done, after the keypad's report is published.
 *   any task     plink_service_prefs_set (a queue that never waits) and plink_service_keypads
 *                (a copy under link_pub_lock).
 *
 * The buffers: plink_dev builds only into a buffer it may write - neither the one the manager uses
 * (plink_master_home's blob stays valid until the next call) nor one a build waiting in the queue
 * names. s_dfree holds those: plink_dev takes one, the link task gives back the one it stopped
 * using. s_dev_lock covers a build against the console's copy.
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "device_service.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "panel_link/plink_home.h"

static const char *TAG = "plink";

/* The numbers both sides share. */
_Static_assert((int)DS_PERSON_OK == (int)PLINK_DEV_OK &&
                   (int)DS_PERSON_NEED_CODE == (int)PLINK_DEV_NEED_CODE &&
                   (int)DS_PERSON_CODE_REFUSED == (int)PLINK_DEV_CODE_REFUSED &&
                   (int)DS_PERSON_BUSY == (int)PLINK_DEV_BUSY &&
                   (int)DS_PERSON_UNREACHABLE == (int)PLINK_DEV_UNREACHABLE &&
                   (int)DS_PERSON_REFUSED == (int)PLINK_DEV_REFUSED,
               "the judge's answers are DEV_ACK's results");
_Static_assert((int)PLINK_SERVICE_PSET_OK == (int)PLINK_PSET_OK &&
                   (int)PLINK_SERVICE_PSET_NOT_FOUND == (int)PLINK_PSET_NOT_FOUND &&
                   (int)PLINK_SERVICE_PSET_OFFLINE == (int)PLINK_PSET_OFFLINE &&
                   (int)PLINK_SERVICE_PSET_UNSUPPORTED == (int)PLINK_PSET_UNSUPPORTED &&
                   (int)PLINK_SERVICE_PSET_BUSY == (int)PLINK_PSET_BUSY &&
                   (int)PLINK_SERVICE_PSET_TIMEOUT == (int)PLINK_PSET_TIMEOUT &&
                   (int)PLINK_SERVICE_PSET_BAD == (int)PLINK_PSET_BAD,
               "plink_service_pset_t is the link's plink_pset_t");
_Static_assert(PLINK_SERVICE_ORIGIN_HA == PLINK_ORIGIN_HA &&
                   PLINK_SERVICE_ORIGIN_CONSOLE == PLINK_ORIGIN_CONSOLE,
               "the origins are PREFS_SET's");
_Static_assert(PLINK_SERVICE_PREFS_BODY_MAX <= PLINK_PREFS_SET_BODY_MAX,
               "every field v1 knows fits one PREFS_SET");
_Static_assert(PROTO_LINK_KEYPADS_MAX == PLINK_KEYPADS_MAX, "Home Assistant's list holds them all");
_Static_assert(PROTO_LINK_NAME_LEN >= PLINK_NAME_MAX + 1u &&
                   PROTO_LINK_FW_LEN >= PLINK_FW_MAX + 1u &&
                   PROTO_PANEL_ID_LEN >= PLINK_PANEL_ID_LEN + 1u,
               "a keypad's name, firmware and panel id fit Home Assistant's");
_Static_assert(PROTO_LINK_VOICE_UNKNOWN == PLINK_VOICE_UNKNOWN &&
                   PROTO_LINK_VOICE_READY == PLINK_VOICE_READY &&
                   PROTO_LINK_VOICE_LOADING == PLINK_VOICE_LOADING &&
                   PROTO_LINK_VOICE_NONE == PLINK_VOICE_NONE,
               "PREFS' voice is Home Assistant's");

#define PSET_QUEUE   4u     /* settings writes waiting for the link task */
#define PSET_LOST_MS 10000u /* a write the manager never ended (it always does: a safety net) */

/* ---- settings writes (any task -> the link task -> the caller's done) ------------------------ */

typedef struct {
    uint8_t device;
    uint8_t origin;
    uint8_t len;
    uint8_t fields[PLINK_SERVICE_PREFS_BODY_MAX];
    uint32_t ticket;
    plink_service_prefs_done_fn done;
    void *ctx;
} pset_item_t;

static QueueHandle_t s_pset_q; /* PSRAM; published once, with a release store */

/* The write in flight to each keypad (the manager allows one), by device: the link task's. */
typedef struct {
    bool busy;
    uint32_t req;
    uint32_t ticket;
    uint64_t since;
    plink_service_prefs_done_fn done;
    void *ctx;
} pset_slot_t;
static pset_slot_t s_pset[PLINK_KEYPADS_MAX + 1u] ALARM_SLOW_BSS;
static uint32_t s_pset_req; /* the last req: from a random base at each start (LINK-018) */

static void pset_end(plink_service_prefs_done_fn done, void *ctx, uint32_t ticket, uint8_t result,
                     const uint8_t *why)
{
    static const uint8_t none[PROTO_PREFS_F_LAST];
    if (done != NULL) {
        done(ctx, ticket, (plink_service_pset_t)result, why != NULL ? why : none);
    }
}

plink_service_pset_t plink_service_prefs_set(uint8_t device, uint8_t origin, const uint8_t *fields,
                                             size_t len, plink_service_prefs_done_fn done,
                                             void *ctx, uint32_t ticket)
{
    if (fields == NULL || len == 0u || len > PLINK_SERVICE_PREFS_BODY_MAX) {
        return PLINK_SERVICE_PSET_BAD;
    }
    QueueHandle_t q = __atomic_load_n(&s_pset_q, __ATOMIC_ACQUIRE);
    if (device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST || q == NULL) {
        return PLINK_SERVICE_PSET_NOT_FOUND;
    }
    pset_item_t it;
    memset(&it, 0, sizeof it);
    it.device = device;
    it.origin = origin;
    it.len = (uint8_t)len;
    memcpy(it.fields, fields, len);
    it.ticket = ticket;
    it.done = done;
    it.ctx = ctx;
    return xQueueSend(q, &it, 0) == pdTRUE ? PLINK_SERVICE_PSET_OK : PLINK_SERVICE_PSET_BUSY;
}

void hsvc_poll(plink_master_t *m, uint64_t now)
{
    /* a write the manager never ended would hold its keypad BUSY for good: never expected */
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        pset_slot_t *s = &s_pset[d];
        if (s->busy && now - s->since >= PSET_LOST_MS) {
            ESP_LOGE(TAG, "keypad %u's settings write never ended: given up", (unsigned)d);
            const pset_slot_t was = *s;
            memset(s, 0, sizeof *s);
            pset_end(was.done, was.ctx, was.ticket, PLINK_PSET_TIMEOUT, NULL);
        }
    }
    QueueHandle_t q = __atomic_load_n(&s_pset_q, __ATOMIC_ACQUIRE);
    pset_item_t it;
    while (q != NULL && xQueueReceive(q, &it, 0) == pdTRUE) {
        uint8_t why = PLINK_PSET_BUSY;
        bool sent = false;
        uint32_t req = 0u;
        if (!s_pset[it.device].busy) {
            if (++s_pset_req == 0u) {
                s_pset_req = 1u; /* never 0: both codecs refuse it */
            }
            req = s_pset_req;
            sent =
                plink_master_prefs_set(m, now, it.device, req, it.origin, it.fields, it.len, &why);
        }
        if (sent) {
            pset_slot_t *s = &s_pset[it.device];
            s->busy = true;
            s->req = req;
            s->ticket = it.ticket;
            s->since = now;
            s->done = it.done;
            s->ctx = it.ctx;
        } else {
            pset_end(it.done, it.ctx, it.ticket, why, NULL);
        }
    }
}

void hsvc_prefs_done(void *ctx, uint8_t device, const plink_prefs_done_t *d)
{
    plink_master_t *m = ctx; /* the manager (msvc_prepare's port.ctx) */
    if (d == NULL || device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST ||
        !s_pset[device].busy || s_pset[device].req != d->req) {
        return; /* not a write of this service's (never) */
    }
    const pset_slot_t was = s_pset[device];
    memset(&s_pset[device], 0, sizeof s_pset[device]);
    uint8_t why[PROTO_PREFS_F_LAST];
    memset(why, 0, sizeof why);
    for (size_t i = 0; d->result == PLINK_PSET_OK && i < d->refused_count; i++) {
        const uint8_t id = d->refused[2u * i];
        if (id >= 1u && id <= PROTO_PREFS_F_LAST) {
            why[id - 1u] = (uint8_t)proto_prefs_why_read(d->refused[2u * i + 1u]);
        }
    }
    /* Its report as it stands now (the manager kept the answer's), published before the caller
     * hears the end: what it reads next holds the values the keypad stores. */
    link_pub_lock();
    for (size_t i = 0; m != NULL && g_pub.home != NULL && i < g_pub.keypads; i++) {
        if (g_pub.keypad[i].device == device) {
            (void)plink_master_keypad_home(m, link_now(), device, &g_pub.home[i]);
        }
    }
    link_pub_unlock();
    pset_end(was.done, was.ctx, was.ticket, d->result, why);
}

/* ---- Home Assistant's keypads list (any task) ------------------------------------------------ */

size_t plink_service_keypads(proto_link_keypad_t *out, size_t max)
{
    if (out == NULL || max == 0u || g_link.role != PLINK_SERVICE_MASTER) {
        return 0u;
    }
    size_t n = 0u;
    link_pub_lock();
    for (size_t i = 0; g_pub.running && i < g_pub.keypads && i < PLINK_KEYPADS_MAX && n < max;
         i++) {
        const ui_link_keypad_t *k = &g_pub.keypad[i];
        proto_link_keypad_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->device = k->device;
        if (k->name[0] != '\0') {
            snprintf(o->name, sizeof o->name, "%s", k->name);
        } else {
            snprintf(o->name, sizeof o->name, "Keypad %u", (unsigned)k->device); /* the screen's */
        }
        o->home = k->home;
        snprintf(o->fw, sizeof o->fw, "%s", k->fw);
        o->rssi = g_pub.rssi[i] < 0 ? g_pub.rssi[i] : 0; /* 0 or more: not known */
        o->online = k->connected;
        if (g_pub.home == NULL) {
            continue;
        }
        const plink_keypad_home_t *h = &g_pub.home[i];
        if (h->device != k->device) {
            continue; /* not published yet */
        }
        o->online = h->online;
        const plink_prefs_report_t *r = &h->prefs;
        if (!r->have) {
            continue; /* no report yet (or no PREFS): id, settings and voice null, can [] */
        }
        snprintf(o->id, sizeof o->id, "%s", r->panel_id); /* "" while a lower keypad has it */
        if (h->online && (h->caps & PLINK_CAP_PREFS) == 0u) {
            /* Its live session no longer shares PREFS (older firmware since): the report is from
             * before, and nothing asks for another. The same device for Home Assistant (its id
             * kept), its settings unknown and none settable, so a settings_set is answered
             * unsupported at once (HAI-018). Offline, the report stays: a keypad switched off only
             * goes unavailable. */
            continue;
        }
        o->reported = true;
        o->can = r->accepts & PROTO_PREFS_KNOWN;
        o->voice = r->voice;
        proto_prefs_decoded_t dec;
        if (proto_prefs_decode(r->fields, r->fields_len, PROTO_PREFS_KNOWN, &dec)) {
            o->settings = dec.values;
        }
    }
    link_pub_unlock();
    return n;
}

/* ---- Smart Home: plink_dev ------------------------------------------------------------------- */

#if CONFIG_ALARM_HOME_CONTROL

#define DEV_STACK        4096u /* PSRAM: never writes flash or NVS */
#define DEV_PRIORITY     3     /* below plink_work (4): the keypads' alarm commands first */
#define DEV_BUILD_GAP_MS 250u  /* builds on device_service_seq at most this often */
#define DEV_CHECK_MS     2000u /* a build to compare, in case a change was missed */
#define DEV_IDLE_MS      1000u /* plink_dev's wait while no keypad has HOME */
#define DEV_BUF_WAIT_MS  1000u /* an action's build waits for a free buffer at most this long */

typedef struct {
    bool used;
    uint32_t order;
    uint32_t ticket;
    uint8_t device;
    uint8_t home;
    plink_dev_act_t act; /* holds the code until plink_dev wipes it */
} dev_job_t;

typedef struct {
    size_t len;
    uint32_t crc;
    uint32_t states;
    size_t rows;
    uint64_t built_ms;
} dev_meta_t;

static bool s_home_asked; /* plink_service_set_home: the registry runs */
static bool s_home;       /* the device side started: PLINK_CAP_HOME is offered */
static TaskHandle_t s_dev;
static dev_job_t *s_djobs; /* LINK_DEV_JOBS, PSRAM */
static SemaphoreHandle_t s_djob_lock;
static uint32_t s_djob_order;
static hc_device_t *s_rows;          /* the registry copy, PSRAM, plink_dev's */
static uint8_t *s_dblob[2];          /* PSRAM */
static plink_dev_recs_t *s_drecs[2]; /* PSRAM */
static dev_meta_t s_dmeta[2];        /* written under s_dev_lock */
static SemaphoreHandle_t s_dev_lock; /* a build against the console's copy */
static atomic_uint s_dfree;          /* bit b: plink_dev may write buffer b */
static atomic_int s_dactive;         /* the buffer the manager uses (-1: none yet) */
static atomic_bool s_dwanted;        /* a live keypad has HOME (the link task's publish) */
static atomic_uint s_dbuilds;        /* builds handed over */
/* plink_dev's own */
static uint32_t s_dseq;        /* device_service_seq() at the last build */
static uint64_t s_dbuilt_at;   /* the last build (link_now) */
static uint64_t s_dchecked_at; /* ... and the last one compared (0: build at once) */
static bool s_dposted;         /* a build was handed over */
static uint32_t s_dposted_list, s_dposted_states; /* ... its CRCs */
static bool s_dtold_bad;                          /* a build that failed was logged */

void plink_service_set_home(bool registry_runs)
{
    s_home_asked = registry_runs;
}

/* A buffer plink_dev may write, taken (-1: none now: one the manager uses, one waiting). */
static int take_free(void)
{
    unsigned f = atomic_load(&s_dfree);
    while (f != 0u) {
        const unsigned b = (f & 1u) != 0u ? 0u : 1u;
        if (atomic_compare_exchange_weak(&s_dfree, &f, f & ~(1u << b))) {
            return (int)b;
        }
    }
    return -1;
}

/* Copies the registry, builds the list and its records into a free buffer and hands them over
 * when they differ from the last handed over (after an action: waiting up to 1 s for a buffer,
 * so its answer follows a build made after it). */
static void build_and_post(bool after_act)
{
    const uint64_t start = link_now();
    int b = take_free();
    while (b < 0 && after_act && link_now() - start < DEV_BUF_WAIT_MS) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)); /* the link task frees one each turn */
        b = take_free();
    }
    if (b < 0) {
        return; /* a build already waits for the link task: the next wake builds again */
    }
    /* the counter first: a change while the registry is copied builds again */
    const uint32_t seq = device_service_seq();
    xSemaphoreTake(s_dev_lock, portMAX_DELAY);
    const size_t rows = device_service_list(s_rows, HC_MAX_DEVICES);
    /* BL-220: a keypad shows the house's rooms - each device's room as its area, in the rooms'
     * order - so its tabs are the alarm panel's (HCTL-013) */
    device_service_home_apply(s_rows, rows);
    size_t len = 0u;
    uint32_t crc = 0u;
    const bool ok = plink_home_list_build(s_rows, rows, s_dblob[b], PLINK_DEV_BLOB_MAX, &len, &crc);
    plink_home_recs_build(s_rows, rows, s_drecs[b]);
    const uint32_t states = plink_home_states_crc(s_drecs[b]);
    const uint64_t now = link_now();
    s_dmeta[b] =
        (dev_meta_t){.len = len, .crc = crc, .states = states, .rows = rows, .built_ms = now};
    xSemaphoreGive(s_dev_lock);
    s_dseq = seq;
    s_dbuilt_at = now;
    s_dchecked_at = now;
    if (!ok || (s_dposted && crc == s_dposted_list && states == s_dposted_states)) {
        atomic_fetch_or(&s_dfree, 1u << (unsigned)b); /* nothing new: the buffer goes back */
        if (!ok && !s_dtold_bad) {
            s_dtold_bad = true;
            ESP_LOGE(TAG, "the keypads' device list couldn't be built (%u devices)",
                     (unsigned)rows);
        }
        return;
    }
    s_dtold_bad = false;
    s_dposted = true;
    s_dposted_list = crc;
    s_dposted_states = states;
    msvc_post_devices((uint8_t)b, len, crc);
}

static bool take_dev_job(dev_job_t *out)
{
    bool got = false;
    xSemaphoreTake(s_djob_lock, portMAX_DELAY);
    dev_job_t *first = NULL;
    for (size_t i = 0; i < LINK_DEV_JOBS; i++) {
        if (s_djobs[i].used && (first == NULL || (int32_t)(s_djobs[i].order - first->order) < 0)) {
            first = &s_djobs[i];
        }
    }
    if (first != NULL) {
        *out = *first;
        plink_wipe(first, sizeof *first); /* the code leaves the slot (LINK-018) */
        got = true;
    }
    xSemaphoreGive(s_djob_lock);
    return got;
}

/* One keypad's device action (LINK-047, LINK-048), judged as the master's own screen's: the
 * keypad n of the session ({KEYPAD, n}, never a number from the frame) and its home partition. */
static void run_dev_job(dev_job_t *job)
{
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = job->device};
    const bool with_code = job->act.code[0] != '\0';
    alarm_result_t why = ALARM_OK;
#if CONFIG_ALARM_BUILD_DEV
    const uint64_t t0 = link_now();
#endif
    const ds_person_t r =
        device_service_person_act(&src, job->home, job->act.ref, job->act.action, job->act.value,
                                  job->act.mode, with_code ? job->act.code : NULL, &why);
    plink_wipe(job->act.code, sizeof job->act.code);
    if (with_code) {
        msvc_lockout_refresh(job->device); /* a wrong code may have started its lockout */
    }
    build_and_post(true); /* the record shows it under way: handed over before the answer */
    msvc_post_dev_ack(job->ticket, (uint8_t)r, (uint8_t)why);
#if CONFIG_ALARM_BUILD_DEV
    /* the HIL's "within 1 s of the master" (docs/features/11 "Test and acceptance") */
    ESP_LOGI(TAG, "home: keypad %u %s action %u -> %u (why %u) in %u ms", (unsigned)job->device,
             job->act.ref, (unsigned)job->act.action, (unsigned)r, (unsigned)why,
             (unsigned)(link_now() - t0));
#endif
}

static void dev_task(void *arg)
{
    (void)arg;
    for (;;) {
        const bool wanted = atomic_load(&s_dwanted);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wanted ? DEV_BUILD_GAP_MS : DEV_IDLE_MS));
        dev_job_t job;
        while (take_dev_job(&job)) {
            run_dev_job(&job);
            plink_wipe(&job, sizeof job);
        }
        if (!atomic_load(&s_dwanted)) {
            s_dchecked_at = 0u; /* the first keypad with HOME gets a build at once */
            continue;
        }
        const uint64_t now = link_now();
        const bool moved = device_service_seq() != s_dseq;
        if (!s_dposted || s_dchecked_at == 0u || now - s_dchecked_at >= DEV_CHECK_MS ||
            (moved && now - s_dbuilt_at >= DEV_BUILD_GAP_MS)) {
            build_and_post(false);
        }
    }
}

bool hsvc_dev_run(void *ctx, uint32_t ticket, uint8_t device, uint8_t home,
                  const plink_dev_act_t *act)
{
    (void)ctx;
    if (!s_home || act == NULL) {
        return false;
    }
    bool placed = false;
    xSemaphoreTake(s_djob_lock, portMAX_DELAY);
    for (size_t i = 0; i < LINK_DEV_JOBS; i++) {
        if (!s_djobs[i].used) {
            s_djobs[i].used = true;
            s_djobs[i].order = s_djob_order++;
            s_djobs[i].ticket = ticket;
            s_djobs[i].device = device;
            s_djobs[i].home = home;
            s_djobs[i].act = *act; /* the manager wipes its own copy of the code */
            placed = true;
            break;
        }
    }
    xSemaphoreGive(s_djob_lock);
    if (placed) {
        xTaskNotifyGive(s_dev);
    }
    return placed; /* false: no room now, the keypad's resend comes back */
}

void hsvc_devices_in(plink_master_t *m, uint64_t now, uint8_t buf, size_t len, uint32_t crc)
{
    if (!s_home || buf > 1u) {
        return;
    }
    plink_master_home(m, now, s_dblob[buf], len, crc, s_drecs[buf]);
    const int old = atomic_exchange(&s_dactive, (int)buf);
    if (old >= 0 && old != (int)buf) {
        atomic_fetch_or(&s_dfree, 1u << (unsigned)old); /* the manager no longer reads it */
    }
    atomic_fetch_add(&s_dbuilds, 1u);
    xTaskNotifyGive(s_dev); /* an action's build may be waiting for that buffer */
}

void hsvc_wanted(bool wanted)
{
    if (s_home && !atomic_exchange(&s_dwanted, wanted) && wanted) {
        xTaskNotifyGive(s_dev); /* a keypad with HOME: a build at once */
    }
}

uint32_t hsvc_stack_free(void)
{
    return s_dev != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_dev) : 0u;
}

bool hsvc_dev_copy(uint8_t *blob, size_t cap, plink_dev_recs_t *recs, hsvc_dev_info_t *info)
{
    memset(info, 0, sizeof *info);
    info->offered = s_home;
    info->wanted = atomic_load(&s_dwanted);
    info->builds = atomic_load(&s_dbuilds);
    if (!s_home) {
        return false;
    }
    bool have = false;
    xSemaphoreTake(s_dev_lock, portMAX_DELAY); /* plink_dev doesn't build meanwhile */
    const int a = atomic_load(&s_dactive);
    if (a >= 0 && s_dmeta[a].len <= cap) {
        memcpy(blob, s_dblob[a], s_dmeta[a].len);
        *recs = *s_drecs[a];
        info->list_crc = s_dmeta[a].crc;
        info->states_crc = s_dmeta[a].states;
        info->len = s_dmeta[a].len;
        info->rows = s_dmeta[a].rows;
        info->built_ms = s_dmeta[a].built_ms;
        have = true;
    }
    xSemaphoreGive(s_dev_lock);
    return have;
}

/* The device side (start-up): its PSRAM and its task; nothing is kept when any part is missing. */
static esp_err_t dev_prepare(void)
{
    s_djobs = alarm_mem_slow_calloc(LINK_DEV_JOBS, sizeof *s_djobs);
    s_rows = alarm_mem_slow_calloc(HC_MAX_DEVICES, sizeof *s_rows);
    for (size_t b = 0; b < 2u; b++) {
        s_dblob[b] = alarm_mem_slow_malloc(PLINK_DEV_BLOB_MAX);
        s_drecs[b] = alarm_mem_slow_calloc(1, sizeof *s_drecs[b]);
    }
    /* PSRAM, as the queues: internal RAM is the alarm's reserve; only the task's TCB goes there */
    s_djob_lock = xSemaphoreCreateMutexWithCaps(alarm_mem_slow_caps());
    s_dev_lock = xSemaphoreCreateMutexWithCaps(alarm_mem_slow_caps());
    atomic_store(&s_dfree, 3u);
    atomic_store(&s_dactive, -1);
    if (s_djobs != NULL && s_rows != NULL && s_dblob[0] != NULL && s_dblob[1] != NULL &&
        s_drecs[0] != NULL && s_drecs[1] != NULL && s_djob_lock != NULL && s_dev_lock != NULL &&
        alarm_mem_task_create(dev_task, "plink_dev", DEV_STACK, NULL, DEV_PRIORITY, &s_dev) ==
            pdPASS) {
        return ESP_OK;
    }
    free(s_djobs);
    free(s_rows);
    for (size_t b = 0; b < 2u; b++) {
        free(s_dblob[b]);
        free(s_drecs[b]);
        s_dblob[b] = NULL;
        s_drecs[b] = NULL;
    }
    s_djobs = NULL;
    s_rows = NULL;
    if (s_djob_lock != NULL) {
        vSemaphoreDeleteWithCaps(s_djob_lock);
        s_djob_lock = NULL;
    }
    if (s_dev_lock != NULL) {
        vSemaphoreDeleteWithCaps(s_dev_lock);
        s_dev_lock = NULL;
    }
    return ESP_ERR_NO_MEM;
}

#else /* !CONFIG_ALARM_HOME_CONTROL: no devices to offer */

static const bool s_home = false;

void plink_service_set_home(bool registry_runs)
{
    (void)registry_runs;
}

bool hsvc_dev_run(void *ctx, uint32_t ticket, uint8_t device, uint8_t home,
                  const plink_dev_act_t *act)
{
    (void)ctx;
    (void)ticket;
    (void)device;
    (void)home;
    (void)act;
    return false;
}

void hsvc_devices_in(plink_master_t *m, uint64_t now, uint8_t buf, size_t len, uint32_t crc)
{
    (void)m;
    (void)now;
    (void)buf;
    (void)len;
    (void)crc;
}

void hsvc_wanted(bool wanted)
{
    (void)wanted;
}

uint32_t hsvc_stack_free(void)
{
    return 0u;
}

bool hsvc_dev_copy(uint8_t *blob, size_t cap, plink_dev_recs_t *recs, hsvc_dev_info_t *info)
{
    (void)blob;
    (void)cap;
    (void)recs;
    memset(info, 0, sizeof *info);
    return false;
}

#endif /* CONFIG_ALARM_HOME_CONTROL */

/* ---- start ----------------------------------------------------------------------------------- */

uint16_t hsvc_caps(void)
{
    uint16_t caps = s_home ? PLINK_CAP_HOME : 0u;
#if CONFIG_ALARM_FEATURE_UI
    caps |= PLINK_CAP_PREFS; /* a keypad's settings travel with a UI to apply them */
#endif
    return caps;
}

esp_err_t hsvc_prepare(void)
{
    plink_keypad_home_t *pub = alarm_mem_slow_calloc(PLINK_KEYPADS_MAX, sizeof *pub);
    QueueHandle_t q = xQueueCreateWithCaps(PSET_QUEUE, sizeof(pset_item_t), alarm_mem_slow_caps());
    if (pub == NULL || q == NULL) {
        free(pub);
        if (q != NULL) {
            vQueueDeleteWithCaps(q);
        }
        return ESP_ERR_NO_MEM;
    }
    link_pub_lock();
    g_pub.home = pub;
    link_pub_unlock();
    s_pset_req = esp_random();
    __atomic_store_n(&s_pset_q, q, __ATOMIC_RELEASE);
#if CONFIG_ALARM_HOME_CONTROL
    if (s_home_asked) {
        s_home = dev_prepare() == ESP_OK;
        if (!s_home) {
            ESP_LOGE(TAG, "no room for Smart Home on keypads: the link runs without it");
        }
    }
#endif
    return ESP_OK;
}

#endif /* CONFIG_ALARM_LINK */
