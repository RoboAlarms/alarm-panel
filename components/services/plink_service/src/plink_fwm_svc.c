/*
 * plink_fwm_svc.c - a master's keypad updates on a panel (docs/features/11 "Updates through the
 * link": The rules, The master, What people see; LINK-029..041; BL-048): the planner
 * (panel_link plink_fw.h) on the link task, with panel_ota's update machinery. Keypads follow
 * their master: nobody is asked anything.
 *
 * Each link task turn, before the manager's tick (fsvc_poll):
 *   - STATUS's UPDATING (plink_master_fw_self): its own update downloads or waits, or it is on its
 *     self-test;
 *   - LINK-041: while keypads are paired, its self-test needs one of them back
 *     (panel_ota_set_keypads_check): the keypads on line when its restart came (NVS link/kup,
 *     written once then) - none: always;
 *   - once a second, every 100 ms while its restart waits for its keypads' INSTALLs
 *     (panel_ota_restart_soon): the planner's step on a world made of panel_ota - target: its own
 *     update waiting in its spare slot (panel_ota_spare_image), else the version it runs; INFO's
 *     version only once confirmed; server, channel and key from its trust record; its own image
 *     (spare slot, or the running one once confirmed: panel_ota_running_image); the copy for
 *     another model (panel_ota_copy_get) - and the core (plink_master_fw_keypad); its orders go to
 *     the core, the copy is asked for (panel_ota_fetch_for, at most every 5 s) or freed, the
 *     Activity events (plink_fw_log_step) go to the worker, which posts them (the alarm task:
 *     never from here), and the rows are published.
 * Its own restart (panel_ota_set_restart_with): held while a keypad on line still downloads or
 * receives the target (decision 7: they download alongside it and restart with it; 30 min at most
 * from when its own update was ready), and while a keypad installs alone or is on its self-test
 * (plink_master_fw_busy) - asked by panel_ota at every gate from this task's published answer;
 * once it may go, INSTALL to every READY keypad, the echoes waited for (5 s at most), then it
 * restarts. The relay's bytes (port.fw_read): the spare slot (panel_ota_spare_read) or the
 * running one (panel_ota_running_read) - both map flash windows: the link task's stack is internal
 * RAM - or the copy in PSRAM (panel_ota_copy_read).
 * The bench (development builds): `ota copy` loads a keypad of another model's image into the copy
 * (panel_ota_copy_load_*); bench_judge names the keypad from the published rows, and the planner
 * relays a bench copy at once (plink_fw_copy_t.bench); it is kept while no keypad needs a copy.
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_proto/ota_profile.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "i18n/i18n.h"
#include "panel_ota.h"

static const char *TAG = "plink_fw";

#define PLAN_MS         1000u    /* the planner once a second, ... */
#define PLAN_FAST_MS    100u     /* ... and this often while its restart waits for the echoes */
#define IMAGE_ASK_MS    2000u    /* the running image asked for again until it is worked out */
#define COPY_ASK_MS     5000u    /* a copy asked for at most this often */
#define KEYPADS_WAIT_MS 1800000u /* its restart waits for keypads still getting its update */
#define UNPLANNED_MS    10000u   /* a target the planner hasn't planned yet holds it this long */
#define EV_QUEUE        16u

/* The numbers the screens and MQTT read are the planner's. */
#define SAME(a, b) ((int)(a) == (int)(b))
_Static_assert(SAME(PLINK_SERVICE_FWR_NONE, PLINK_FWR_NONE) &&
                   SAME(PLINK_SERVICE_FWR_UP_TO_DATE, PLINK_FWR_UP_TO_DATE) &&
                   SAME(PLINK_SERVICE_FWR_DOWNLOADING, PLINK_FWR_DOWNLOADING) &&
                   SAME(PLINK_SERVICE_FWR_RECEIVING, PLINK_FWR_RECEIVING) &&
                   SAME(PLINK_SERVICE_FWR_READY, PLINK_FWR_READY) &&
                   SAME(PLINK_SERVICE_FWR_RESTARTING, PLINK_FWR_RESTARTING) &&
                   SAME(PLINK_SERVICE_FWR_NOT_UPDATED, PLINK_FWR_NOT_UPDATED) &&
                   SAME(PLINK_SERVICE_FWR_NEWER, PLINK_FWR_NEWER),
               "PLINK_SERVICE_FWR_* are the planner's rows");
_Static_assert(SAME(PANEL_OTA_COPY_NONE, PLINK_FWCOPY_NONE) &&
                   SAME(PANEL_OTA_COPY_FETCHING, PLINK_FWCOPY_FETCHING) &&
                   SAME(PANEL_OTA_COPY_READY, PLINK_FWCOPY_READY) &&
                   SAME(PANEL_OTA_COPY_FAILED, PLINK_FWCOPY_FAILED),
               "the copy's states");
_Static_assert(PLINK_KEYPADS_MAX <= 8u, "the bit masks are 8 keypads");

/* What holds its own restart for its keypads (panel_ota's waiting_for and reason). */
/* TR: Software update: why this panel's restart into its update waits - one of its keypads runs
 * its own new version's self-test, or installs. At most 39 bytes. */
static const char *const HOLD_BUSY = I18N_NOOP_C("update", "A keypad is checking its update");
/* TR: Software update: why this panel's restart into its update waits - its keypads are still
 * downloading the same update, so they can restart together. At most 39 bytes. */
static const char *const HOLD_GETTING = I18N_NOOP_C("update", "Its keypads are getting the update");

/* ---- the link task's (PSRAM: only the link task touches them) -------------------------------- */
static struct {
    bool on;
    plink_fw_world_t *w;
    plink_fw_step_t *s;
    plink_fw_log_t *log;
    panel_ota_status_t *st;
    panel_ota_image_t *img;   /* the running image's facts */
    panel_ota_image_t *spare; /* its own update's, in its spare slot */
    panel_ota_copy_t *copy;
    uint32_t ota_seq;
    uint64_t ota_at;
    bool img_known;           /* worked out, or it can't be */
    uint64_t img_ask_at;
    bool have_spare;
    uint64_t spare_at;        /* its own update in its spare slot since */
    uint64_t plan_at;
    bool with_step;           /* a step since its restart came (panel_ota_restart_soon) */
    bool check_on;            /* LINK-041's question registered */
    bool kup_done;            /* link/kup written for the restart under way */
    uint64_t copy_ask_at;
    plink_fw_facts_t copy_asked;
    uint32_t steps;
    uint32_t mqtt_crc;
    QueueHandle_t evq;        /* plink_fw_log_ev_t, link task -> worker */
    uint32_t events, events_lost;
} F ALARM_SLOW_BSS;

/* ---- shared with the update task and the worker (internal RAM: atomics, a few bytes) ---------- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_planned[PLINK_FW_VERSION_MAX + 1]; /* the target the planner last planned with */
static const char *s_hold;     /* what holds its restart into that target (NULL: nothing) */
static uint64_t s_unplanned_at; /* a target not planned yet was first asked about then */
static char s_unplanned[PLINK_FW_VERSION_MAX + 1];
static volatile bool s_installs_out; /* its restart came and its INSTALLs are echoed */
static volatile uint32_t s_answering; /* LINK-041: keypads on line now (bit d-1) */
static volatile uint32_t s_bench_again; /* the bench: keypads asked again (bit d-1) */
static volatile uint32_t s_kup_need;  /* ... the ones its self-test waits for (link/kup) */

/* ---- what the others read (link_pub_lock; PSRAM) ---------------------------------------------- */
static struct {
    plink_service_fw_t sum;
    plink_service_fw_kp_t kp[PLINK_KEYPADS_MAX];
    size_t n;
    /* the console's (plink fw) */
    plink_fw_kp_t raw[PLINK_KEYPADS_MAX];
    plink_fw_row_t row[PLINK_KEYPADS_MAX];
    plink_fw_mem_t mem[PLINK_KEYPADS_MAX];
    plink_fw_image_t own;
    plink_fw_copy_t copy;
    char running[PLINK_FW_VERSION_MAX + 1];
    bool restart_soon;
    uint32_t steps, events, events_lost;
} s_out ALARM_SLOW_BSS;

static void put(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

#define PUT(dst, src) put((dst), sizeof(dst), (src))

/* ---- panel_ota's questions (the update task and the alarm task: quick, no waiting) ----------- */

/* What holds its restart into version for its keypads: the planner's answer for that target, or -
 * a target it hasn't planned yet (its own update just checked) - its keypads getting it, for
 * UNPLANNED_MS at most. "": not its own release's restart. */
static const char *with_hold(void *ctx, const char *version)
{
    (void)ctx;
    if (version == NULL || version[0] == '\0') {
        return NULL;
    }
    const uint64_t now = link_now();
    const char *why = NULL;
    portENTER_CRITICAL(&s_mux);
    if (strncmp(s_planned, version, sizeof s_planned) == 0) {
        why = s_hold;
    } else {
        if (strncmp(s_unplanned, version, sizeof s_unplanned) != 0) {
            snprintf(s_unplanned, sizeof s_unplanned, "%s", version);
            s_unplanned_at = now;
        }
        why = now - s_unplanned_at < UNPLANNED_MS ? HOLD_GETTING : NULL;
    }
    portEXIT_CRITICAL(&s_mux);
    return why;
}

static bool with_installs_out(void *ctx)
{
    (void)ctx;
    return s_installs_out;
}

/* LINK-041: a keypad on line when its restart came has a session again and is heard - or none
 * was. On the update task about once a second. */
static bool keypads_back(void *ctx)
{
    (void)ctx;
    const uint32_t need = s_kup_need;
    return need == 0u || (s_answering & need) != 0u;
}

/* ---- the world -------------------------------------------------------------------------------- */

static void image_of(plink_fw_image_t *out, const panel_ota_image_t *im, uint8_t src)
{
    memset(out, 0, sizeof *out);
    out->src = src;
    PUT(out->version, im->version);
    PUT(out->device, panel_ota_device());
    PUT(out->project, im->project);
    out->chip_min = im->min_rev_full;
    out->chip_max = im->max_rev_full;
    out->size = im->size;
    memcpy(out->sha256, im->sha256, sizeof out->sha256);
}

static void copy_of(plink_fw_copy_t *c, const panel_ota_copy_t *pc)
{
    memset(c, 0, sizeof *c);
    c->state = (uint8_t)pc->state;
    c->why = pc->why;
    PUT(c->facts.version, pc->version);
    PUT(c->facts.device, pc->device);
    PUT(c->facts.chip_rev, pc->chip_rev);
    PUT(c->facts.project, pc->project);
    c->bench = pc->state == PANEL_OTA_COPY_READY && pc->bench;
    if (pc->state == PANEL_OTA_COPY_READY) {
        plink_fw_image_t *im = &c->image;
        im->src = PLINK_FWSRC_COPY;
        PUT(im->version, pc->version);
        PUT(im->device, pc->device);
        PUT(im->project, pc->project);
        im->chip_min = 0u; /* fetched with the keypad's own facts */
        im->chip_max = 0xFFFFu;
        im->size = pc->size;
        memcpy(im->sha256, pc->sha256, sizeof im->sha256);
    }
}

static void world(plink_master_t *m, uint64_t now)
{
    plink_fw_world_t *w = F.w;
    const uint32_t seq = panel_ota_seq();
    if (seq != F.ota_seq || now >= F.ota_at) {
        F.ota_seq = seq;
        F.ota_at = now + PLAN_MS;
        panel_ota_get_status(F.st);
    }
    const bool confirmed = !F.st->probation;
    const bool spare = confirmed && panel_ota_spare_image(F.spare) == ESP_OK;
    if (spare && !F.have_spare) {
        F.spare_at = now;
        ESP_LOGI(TAG, "its own update %s is ready: its keypads get it too", F.spare->version);
    }
    F.have_spare = spare;
    PUT(w->target, spare ? F.spare->version : F.st->running);
    PUT(w->running, confirmed ? F.st->running : "");
    panel_ota_server_t srv;
    panel_ota_get_server(&srv);
    PUT(w->server, srv.domain);
    PUT(w->channel, F.st->channel);
    if (srv.has_key) {
        memcpy(w->key, srv.key_sha256, sizeof w->key);
    } else {
        memset(w->key, 0, sizeof w->key);
    }
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        (void)plink_master_fw_keypad(m, now, d, &w->kp[d - 1u]); /* zeroed: no keypad here */
    }
    /* Its running image only once a keypad with updates is in session and it is confirmed: a
     * master without such keypads reads nothing of its partition. */
    bool any_fw = false;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        any_fw = any_fw || (w->kp[i].device != 0u && w->kp[i].fw_cap);
    }
    if (!F.img_known && confirmed && any_fw && now >= F.img_ask_at) {
        F.img_ask_at = now + IMAGE_ASK_MS;
        const esp_err_t err = panel_ota_running_image(F.img);
        if (err == ESP_OK || err == ESP_FAIL) {
            F.img_known = true;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "the running image can't be relayed: keypads of this model need "
                              "their own server");
                memset(F.img, 0, sizeof *F.img);
            }
        }
    }
    if (spare) {
        image_of(&w->own, F.spare, PLINK_FWSRC_SPARE);
    } else if (confirmed && F.img_known && F.img->size != 0u) {
        image_of(&w->own, F.img, PLINK_FWSRC_RUNNING);
    } else {
        memset(&w->own, 0, sizeof w->own);
    }
    panel_ota_copy_get(F.copy);
    copy_of(&w->copy, F.copy);
}

/* ---- acting on a step ------------------------------------------------------------------------ */

static void copy_act(uint64_t now)
{
    const plink_fw_facts_t *want = &F.s->copy;
    if (want->version[0] == '\0') {
        if (F.copy->state != PANEL_OTA_COPY_NONE && !F.w->copy.bench) { /* a bench copy waits */
            panel_ota_copy_free();
            ESP_LOGI(TAG, "the copy for keypads is freed (none needs it)");
        }
        memset(&F.copy_asked, 0, sizeof F.copy_asked);
        return;
    }
    if (memcmp(&F.w->copy.facts, want, sizeof *want) == 0 && F.copy->state != PANEL_OTA_COPY_NONE) {
        return; /* held or being fetched */
    }
    if (memcmp(&F.copy_asked, want, sizeof *want) == 0 && now < F.copy_ask_at) {
        return;
    }
    /* the keypad's own facts for the check: one of its model's */
    const plink_fw_info_t *info = NULL;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX && info == NULL; i++) {
        const plink_fw_kp_t *kp = &F.w->kp[i];
        if (kp->device != 0u && kp->have_info && strcmp(kp->info.device, want->device) == 0 &&
            strcmp(kp->info.chip_rev, want->chip_rev) == 0 &&
            strcmp(kp->info.project, want->project) == 0) {
            info = &kp->info;
        }
    }
    if (info == NULL) {
        return;
    }
    const proto_fwsrv_facts_t facts = {
        .device = info->device,
        .board_rev = info->board_rev,
        .chip_rev = info->chip_rev,
        .project = info->project,
        .version = info->version,
    };
    const esp_err_t err = panel_ota_fetch_for(&facts, want->version);
    F.copy_asked = *want;
    F.copy_ask_at = now + COPY_ASK_MS;
    ESP_LOGI(TAG, "a copy of %s for model %s (%s): %s", want->version, want->device,
             want->chip_rev, esp_err_to_name(err));
}

static void events_out(void)
{
    plink_fw_log_ev_t ev[PLINK_FW_LOG_MAX];
    const size_t n = plink_fw_log_step(F.log, F.w, F.s, ev, PLINK_FW_LOG_MAX);
    for (size_t i = 0; i < n; i++) {
        if (F.evq != NULL && xQueueSend(F.evq, &ev[i], 0) == pdTRUE) {
            F.events++;
        } else {
            F.events_lost++;
            ESP_LOGW(TAG, "keypad %u's update stage %u not logged: the queue is full",
                     (unsigned)ev[i].device, (unsigned)ev[i].stage);
        }
        ESP_LOGI(TAG, "keypad %u: update stage %u (why %u)", (unsigned)ev[i].device,
                 (unsigned)ev[i].stage, (unsigned)ev[i].why);
    }
    if (n > 0u) {
        msvc_kick();
    }
}

/* LINK-041: the keypads on line when its restart came, kept for the self-test after it (one NVS
 * write a restart; the link task's internal-RAM stack). Erased once it runs confirmed with no
 * update of its own waiting. */
static void kup_tick(bool soon, bool confirmed)
{
    if (soon && !F.kup_done) {
        F.kup_done = true;
        const uint32_t mask = s_answering;
        const esp_err_t err = link_nvs_u32(LINK_KEY_KUP, mask, mask == 0u);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "link/kup: %s", esp_err_to_name(err));
        }
    } else if (!soon) {
        F.kup_done = false;
    }
    if (confirmed && s_kup_need != 0u && !F.have_spare) {
        s_kup_need = 0u;
        (void)link_nvs_u32(LINK_KEY_KUP, 0u, true);
    }
}

/* One planner step. */
static void plan(plink_master_t *m, uint64_t now)
{
    world(m, now);
    plink_fw_world_t *w = F.w;
    plink_fw_step_t *s = F.s;
    const bool soon = panel_ota_restart_soon();
    /* Its keypads download alongside it (decision 7): the rows of this step planned without the
     * restart first - an order this step sends counts. */
    w->own_restart = false;
    plink_fw_plan(w, now, s);
    bool getting = false;
    uint32_t answering = 0u;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &w->kp[i];
        if (kp->device != 0u && kp->online) {
            answering |= 1u << i;
            getting = getting || (F.have_spare && (s->row[i].state == PLINK_FWR_DOWNLOADING ||
                                                    s->row[i].state == PLINK_FWR_RECEIVING));
        }
    }
    s_answering = answering;
    const bool busy = plink_master_fw_busy(m, now);
    const char *hold = busy ? HOLD_BUSY
                       : getting && now - F.spare_at < KEYPADS_WAIT_MS ? HOLD_GETTING
                                                                       : NULL;
    if (soon) {
        w->own_restart = true;
        plink_fw_plan(w, now, s);
    }
    memcpy(w->mem, s->mem, sizeof w->mem);
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        const plink_fw_order_t *o = &s->send[d - 1u];
        if (o->op != 0u) {
            const uint32_t id = plink_master_fw_order(m, now, d, o); /* sent at the tick */
            ESP_LOGD(TAG, "keypad %u: order %" PRIu32 " op %u %s", (unsigned)d, id,
                     (unsigned)o->op, o->version);
        }
    }
    /* its INSTALLs out: echoed in a step after the first one since its restart came */
    s_installs_out = soon && F.with_step && s->installing == 0u;
    F.with_step = soon;
    portENTER_CRITICAL(&s_mux);
    snprintf(s_planned, sizeof s_planned, "%s", F.have_spare ? w->target : "");
    s_hold = hold;
    portEXIT_CRITICAL(&s_mux);
    copy_act(now);
    events_out();
    kup_tick(soon, !F.st->probation);
    F.steps++;
}

void fsvc_poll(plink_master_t *m, uint64_t now)
{
    if (!F.on) {
        return;
    }
    /* UPDATING: its own run (downloading, waiting) or its self-test - a change sends STATUS */
    const panel_ota_status_t *st = F.st;
    plink_master_fw_self(m, now, st->probation || st->kind != PANEL_OTA_KIND_NONE);
    const bool want = plink_master_held(m) != 0u;
    if (want != F.check_on) {
        F.check_on = want;
        panel_ota_set_keypads_check(want ? keypads_back : NULL, NULL);
    }
    const uint32_t again = __atomic_exchange_n(&s_bench_again, 0u, __ATOMIC_ACQ_REL);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if ((again & (1u << i)) != 0u) {
            plink_fw_again(&F.w->mem[i]); /* the bench copy: relayed at once */
            F.plan_at = now;
            ESP_LOGW(TAG, "keypad %u: a bench copy is held for it", (unsigned)(i + 1u));
        }
    }
    if (now >= F.plan_at || (panel_ota_restart_soon() && !F.with_step)) {
        plan(m, now);
        F.plan_at = now + (panel_ota_restart_soon() ? PLAN_FAST_MS : PLAN_MS);
    }
}

/* ---- the relay's bytes (port.fw_read, inside the manager's tick on the link task) ------------- */

size_t fsvc_fw_read(void *ctx, const uint8_t id8[PLINK_ID8_LEN], uint32_t offset, uint8_t *out,
                    size_t len)
{
    (void)ctx;
    if (!F.on || id8 == NULL || out == NULL || len == 0u) {
        return 0u;
    }
    const plink_fw_image_t *own = &F.w->own;
    if (own->src != PLINK_FWSRC_NONE && memcmp(id8, own->sha256, PLINK_ID8_LEN) == 0) {
        if (offset >= own->size) {
            return 0u;
        }
        const size_t n = own->size - offset < len ? own->size - offset : len;
        const esp_err_t err = own->src == PLINK_FWSRC_SPARE ? panel_ota_spare_read(offset, out, n)
                                                            : panel_ota_running_read(offset, out, n);
        return err == ESP_OK ? n : 0u;
    }
    const plink_fw_copy_t *c = &F.w->copy;
    if (c->state == PLINK_FWCOPY_READY && memcmp(id8, c->image.sha256, PLINK_ID8_LEN) == 0) {
        if (offset >= c->image.size) {
            return 0u;
        }
        const size_t n = c->image.size - offset < len ? c->image.size - offset : len;
        return panel_ota_copy_read(id8, offset, out, n) == ESP_OK ? n : 0u;
    }
    return 0u;
}

/* ---- requests (the link task): plink fw update <n> -------------------------------------------- */

uint8_t fsvc_request(plink_master_t *m, uint64_t now, const link_req_t *req)
{
    if (!F.on) {
        return UI_LINK_REQ_FAILED;
    }
    const uint8_t d = req->device;
    if (d < PLINK_DEVICE_FIRST || d > PLINK_DEVICE_LAST ||
        (plink_master_held(m) & (1u << (d - 1u))) == 0u) {
        return UI_LINK_REQ_REFUSED; /* no such keypad */
    }
    plink_fw_again(&F.w->mem[d - 1u]); /* a wait, a refusal, a going back forgotten */
    F.plan_at = now;                    /* at this turn */
    ESP_LOGW(TAG, "keypad %u asked again", (unsigned)d);
    return UI_LINK_REQ_OK;
}

/* ---- the worker: the Activity events ---------------------------------------------------------- */

void fsvc_worker(void)
{
    QueueHandle_t q = __atomic_load_n(&F.evq, __ATOMIC_ACQUIRE);
    plink_fw_log_ev_t e;
    while (q != NULL && xQueueReceive(q, &e, 0) == pdTRUE) {
        const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = e.device};
        /* {keypad, n}; ALARM_FW_KEYPAD_FAILED carries its why in arg2 (never 0) */
        const esp_err_t err =
            e.stage == (uint8_t)ALARM_FW_KEYPAD_FAILED
                ? alarm_service_platform_event(ALARM_EV_FIRMWARE_UPDATE, 0u, e.stage,
                                               e.why != 0u ? e.why : 1u, &src, 0u)
                : alarm_service_firmware_event((alarm_fw_stage_t)e.stage, &src, 0u);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "keypad %u's update stage %u: %s", (unsigned)e.device,
                     (unsigned)e.stage, esp_err_to_name(err));
        }
    }
}

/* ---- publishing (the link task, link_pub_lock held) ------------------------------------------- */

static uint32_t crc_add(uint32_t c, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        c = (c ^ b[i]) * 16777619u; /* FNV-1a: a change, not a check */
    }
    return c;
}

bool fsvc_publish_locked(const plink_peer_info_t *peers, size_t n)
{
    memset(&s_out.sum, 0, sizeof s_out.sum);
    s_out.n = 0u;
    if (!F.on) {
        return false;
    }
    s_out.sum.on = true;
    PUT(s_out.sum.target, F.w->target);
    portENTER_CRITICAL(&s_mux);
    s_out.sum.restart_waits = s_hold != NULL && F.have_spare;
    portEXIT_CRITICAL(&s_mux);
    s_out.sum.keypads = (uint8_t)n;
    uint32_t crc = 2166136261u;
    for (size_t i = 0; i < n && i < PLINK_KEYPADS_MAX; i++) {
        const plink_peer_info_t *p = &peers[i];
        const uint8_t d = p->device;
        if (d < PLINK_DEVICE_FIRST || d > PLINK_DEVICE_LAST) {
            continue;
        }
        const plink_fw_kp_t *kp = &F.w->kp[d - 1u];
        const plink_fw_row_t *r = &F.s->row[d - 1u];
        plink_service_fw_kp_t *o = &s_out.kp[s_out.n++];
        memset(o, 0, sizeof *o);
        o->device = d;
        PUT(o->version, kp->device != 0u && kp->have_info ? kp->info.version : p->hb.fw);
        o->state = kp->device != 0u ? r->state : (uint8_t)PLINK_FWR_NONE;
        o->percent = r->percent;
        o->why = r->why;
        o->online = kp->online;
        o->busy = kp->busy;
        /* MQTT's keypads [{device, version, state}] */
        crc = crc_add(crc, &d, 1u);
        crc = crc_add(crc, o->version, strlen(o->version));
        crc = crc_add(crc, &o->state, 1u);
    }
    memcpy(s_out.raw, F.w->kp, sizeof s_out.raw);
    memcpy(s_out.row, F.s->row, sizeof s_out.row);
    memcpy(s_out.mem, F.w->mem, sizeof s_out.mem);
    s_out.own = F.w->own;
    s_out.copy = F.w->copy;
    PUT(s_out.running, F.w->running);
    s_out.restart_soon = panel_ota_restart_soon();
    s_out.steps = F.steps;
    s_out.events = F.events;
    s_out.events_lost = F.events_lost;
    const bool changed = crc != F.mqtt_crc;
    F.mqtt_crc = crc;
    return changed;
}

size_t plink_service_fw(plink_service_fw_t *sum, plink_service_fw_kp_t *out, size_t max)
{
    if (sum != NULL) {
        memset(sum, 0, sizeof *sum);
    }
    if (plink_service_role() != PLINK_SERVICE_MASTER) {
        return 0u;
    }
    size_t n = 0u;
    link_pub_lock();
    if (g_pub.running && s_out.sum.on) {
        if (sum != NULL) {
            *sum = s_out.sum;
        }
        for (size_t i = 0; out != NULL && i < s_out.n && n < max; i++) {
            out[n++] = s_out.kp[i];
        }
    }
    link_pub_unlock();
    return n;
}

const char *plink_service_fw_row_name(uint8_t state)
{
    return plink_fw_row_name(state);
}

/* ---- the console (plink fw; its task) --------------------------------------------------------- */

static const char *row_word(uint8_t s)
{
    static const char *const W[] = {"-",          "up to date", "downloading", "receiving",
                                    "ready",      "restarting", "not updated yet", "newer"};
    return s < sizeof W / sizeof W[0] ? W[s] : "?";
}

static const char *run_word(uint8_t s)
{
    static const char *const W[] = {"idle",      "probing", "fetching",   "receiving", "key wait",
                                    "verifying", "ready",   "installing", "failed",    "stopped"};
    return s < sizeof W / sizeof W[0] ? W[s] : "?";
}

static const char *op_word(uint8_t op)
{
    static const char *const W[] = {"-", "info", "probe", "fetch", "relay", "install", "cancel"};
    return op < sizeof W / sizeof W[0] ? W[op] : "?";
}

int fsvc_console_show(void)
{
    static plink_service_fw_t sum EXT_RAM_BSS_ATTR; /* the console task's */
    static plink_fw_kp_t raw[PLINK_KEYPADS_MAX] EXT_RAM_BSS_ATTR;
    static plink_fw_row_t row[PLINK_KEYPADS_MAX] EXT_RAM_BSS_ATTR;
    static plink_fw_mem_t mem[PLINK_KEYPADS_MAX] EXT_RAM_BSS_ATTR;
    static plink_fw_image_t own EXT_RAM_BSS_ATTR;
    static plink_fw_copy_t copy EXT_RAM_BSS_ATTR;
    static char running[PLINK_FW_VERSION_MAX + 1] EXT_RAM_BSS_ATTR;
    bool soon = false;
    uint32_t steps = 0u, events = 0u, lost = 0u;
    link_pub_lock();
    const bool on = s_out.sum.on;
    sum = s_out.sum;
    memcpy(raw, s_out.raw, sizeof raw);
    memcpy(row, s_out.row, sizeof row);
    memcpy(mem, s_out.mem, sizeof mem);
    own = s_out.own;
    copy = s_out.copy;
    memcpy(running, s_out.running, sizeof running);
    soon = s_out.restart_soon;
    steps = s_out.steps;
    events = s_out.events;
    lost = s_out.events_lost;
    link_pub_unlock();
    if (!on) {
        printf("keypad updates: off (the link isn't running, or there was no memory for them)\n");
        return 1;
    }
    static const char *const SRC[] = {"none", "running slot", "spare slot", "copy"};
    printf("Keypad updates: target %s, this panel runs %s; its image: %s%s%s; restart %s%s\n",
           sum.target, running[0] != '\0' ? running : "(on its self-test)",
           own.src < 4u ? SRC[own.src] : "?", own.src != 0u ? " " : "",
           own.src != 0u ? own.version : "",
           soon ? "under way (INSTALL to its READY keypads)" : "not now",
           sum.restart_waits ? ", waits for a keypad" : "");
    printf("  copy: state %u%s, %s for %s %s; %" PRIu32 " step(s), %" PRIu32 " event(s), %" PRIu32
           " lost\n",
           (unsigned)copy.state, copy.bench ? " (bench)" : "", copy.facts.version,
           copy.facts.device, copy.facts.chip_rev, steps, events, lost);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *k = &raw[i];
        if (k->device == 0u) {
            continue;
        }
        printf("  %u  %-12s %-15s", (unsigned)k->device, k->have_info ? k->info.version : "?",
               row_word(row[i].state));
        if (row[i].state == PLINK_FWR_DOWNLOADING || row[i].state == PLINK_FWR_RECEIVING) {
            printf(" %3u %%", (unsigned)row[i].percent);
        }
        if (row[i].why != 0u) {
            printf(" why %u %s", (unsigned)row[i].why, proto_fw_why_name(row[i].why));
        }
        printf("%s%s%s\n", k->online ? "" : " (off line)", k->fw_cap ? "" : " (no updates)",
               k->busy ? " (restarting: supervision held)" : "");
        if (k->have_state) {
            printf("     run %s %s, %" PRIu32 "/%" PRIu32 " B, why %u%s; last %u %s%s; order %" PRIu32
                   " %s %s\n",
                   k->state.target, run_word(k->state.state), k->state.bytes, k->state.size,
                   (unsigned)k->state.why,
                   (k->state.flags & PLINK_FWF_PAUSED) != 0u ? " (paused)" : "",
                   (unsigned)k->state.last, k->state.last_version,
                   k->state.probation != 0u ? ", on its self-test" : "", k->order,
                   op_word(k->order_op), k->order_acked ? "echoed" : "outstanding");
        }
        const plink_fw_mem_t *mm = &mem[i];
        if (mm->wait_next || mm->retry_at != 0u || mm->went_back[0] != '\0') {
            printf("     %s%s%s%s\n", mm->wait_next ? "waits for the next version " : "",
                   mm->retry_at != 0u ? "tried again within the hour " : "",
                   mm->went_back[0] != '\0' ? "went back on " : "", mm->went_back);
        }
    }
    return 0;
}

/* ---- the bench copy (development builds: `ota copy`, the console task) ------------------------- */

#if CONFIG_ALARM_BUILD_DEV
/* The image's header against the keypads as last published: the first one it is for - another
 * device than this panel, a project that keypad accepts, its chip revision, its slot, newer than
 * what it runs - and only the keypads' target (the planner relays nothing else). */
static bool bench_judge(void *ctx, const proto_fw_header_t *h, uint32_t size,
                        panel_ota_copy_for_t *out, char *why, size_t why_len)
{
    (void)ctx;
    static plink_fw_kp_t raw[PLINK_KEYPADS_MAX] EXT_RAM_BSS_ATTR; /* the console task's */
    static char target[PLINK_SERVICE_FW_VERSION_MAX + 1] EXT_RAM_BSS_ATTR;
    memset(out, 0, sizeof *out);
    if (!h->image_magic || !h->app_magic) {
        snprintf(why, why_len, "Not an application image");
        return false;
    }
    const esp_app_desc_t *me = esp_app_get_description();
    if (ota_profile_accepts(me->project_name, h->project)) {
        snprintf(why, why_len, "%s is this panel's own model: send it without --copy", h->project);
        return false;
    }
    link_pub_lock();
    const bool on = s_out.sum.on;
    memcpy(raw, s_out.raw, sizeof raw);
    memcpy(target, s_out.sum.target, sizeof target);
    link_pub_unlock();
    if (!on) {
        snprintf(why, why_len, "Keypad updates are off on this panel (plink fw)");
        return false;
    }
    const plink_fw_kp_t *k = NULL;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX && k == NULL; i++) {
        const plink_fw_kp_t *c = &raw[i];
        if (c->device != 0u && c->have_info && strcmp(c->info.device, panel_ota_device()) != 0 &&
            ota_profile_accepts(c->info.project, h->project) &&
            proto_fw_chip_rev_fits(c->info.chip_rev_full, h->min_rev_full, h->max_rev_full)) {
            k = c;
        }
    }
    if (k == NULL) {
        snprintf(why, why_len, "No keypad of another model takes %s (plink fw)", h->project);
        return false;
    }
    if (k->info.slot != 0u && size > k->info.slot) {
        snprintf(why, why_len, "Too big for keypad %u's slot", (unsigned)k->device);
        return false;
    }
    if (!proto_version_is_upgrade(k->info.version, h->version, false)) {
        snprintf(why, why_len, "Keypad %u runs %s: %s isn't newer", (unsigned)k->device,
                 k->info.version, h->version);
        return false;
    }
    /* The planner takes the spare slot's update as the target at its next turn (about a second):
     * a copy sent straight after this panel's own update is judged against that slot too. */
    panel_ota_image_t spare;
    const bool spare_is_it =
        panel_ota_spare_image(&spare) == ESP_OK && strcmp(spare.version, h->version) == 0;
    if (strcmp(h->version, target) != 0 && !spare_is_it) {
        snprintf(why, why_len, "The keypads' target is %s, not %s: update this panel first",
                 target, h->version);
        return false;
    }
    out->keypad = k->device;
    PUT(out->device, k->info.device);
    PUT(out->chip_rev, k->info.chip_rev);
    PUT(out->project, k->info.project);
    PUT(out->version, h->version);
    return true;
}

static void bench_loaded(void *ctx, uint8_t keypad)
{
    (void)ctx;
    if (keypad >= PLINK_DEVICE_FIRST && keypad <= PLINK_DEVICE_LAST) {
        __atomic_or_fetch(&s_bench_again, 1u << (keypad - 1u), __ATOMIC_ACQ_REL);
    }
}
#endif

/* ---- the start-up ----------------------------------------------------------------------------- */

uint16_t fsvc_caps(void)
{
    return F.on ? (uint16_t)PLINK_CAP_FW : 0u;
}

esp_err_t fsvc_prepare(uint64_t now)
{
    F.w = alarm_mem_slow_calloc(1, sizeof *F.w);
    F.s = alarm_mem_slow_calloc(1, sizeof *F.s);
    F.log = alarm_mem_slow_calloc(1, sizeof *F.log);
    F.st = alarm_mem_slow_calloc(1, sizeof *F.st);
    F.img = alarm_mem_slow_calloc(1, sizeof *F.img);
    F.spare = alarm_mem_slow_calloc(1, sizeof *F.spare);
    F.copy = alarm_mem_slow_calloc(1, sizeof *F.copy);
    QueueHandle_t q =
        xQueueCreateWithCaps(EV_QUEUE, sizeof(plink_fw_log_ev_t), alarm_mem_slow_caps());
    if (F.w == NULL || F.s == NULL || F.log == NULL || F.st == NULL || F.img == NULL ||
        F.spare == NULL || F.copy == NULL || q == NULL) {
        ESP_LOGE(TAG, "no memory for keypad updates: keypads keep their versions");
        return ESP_ERR_NO_MEM; /* the caller goes on without them */
    }
    __atomic_store_n(&F.evq, q, __ATOMIC_RELEASE);
    panel_ota_get_status(F.st);
    F.ota_seq = panel_ota_seq();
    F.plan_at = now;
    /* LINK-041: on its self-test, the keypads on line when its restart came */
    uint32_t kup = 0u;
    if (F.st->probation && link_nvs_get_u32(LINK_KEY_KUP, &kup)) {
        s_kup_need = kup & 0xFFu;
    } else if (link_nvs_get_u32(LINK_KEY_KUP, &kup)) {
        (void)link_nvs_u32(LINK_KEY_KUP, 0u, true); /* confirmed (or back): nothing to wait for */
    }
    static const panel_ota_with_t with = {
        .hold = with_hold, .installs_out = with_installs_out, .ctx = NULL};
    panel_ota_set_restart_with(&with);
#if CONFIG_ALARM_BUILD_DEV
    static const panel_ota_copy_bench_t bench = {
        .judge = bench_judge, .loaded = bench_loaded, .ctx = NULL};
    panel_ota_set_copy_bench(&bench); /* `ota copy` on the bench */
#endif
    F.on = true;
    ESP_LOGI(TAG, "keypads follow %s%s; %s", F.st->running,
             F.st->probation ? " once its self-test passes" : "",
             s_kup_need != 0u ? "its self-test waits for a keypad" : "no keypad waited for");
    return ESP_OK;
}

#endif /* CONFIG_ALARM_LINK */
