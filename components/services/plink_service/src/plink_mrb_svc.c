/*
 * plink_mrb_svc.c - the remote backend on a master (BL-223; docs/features/11 "The master's settings
 * and installer menu on keypads", LINK-057..LINK-065): its keypads' calls of this panel's pages.
 *
 * The manager hands each call over on the link task (mrbsvc_run: copied, the worker kicked); the
 * worker runs it with ui_remote_serve on this panel's own backend - ui_port_esp's, as the Panel
 * Manager takes it - with installer mode, the users and the code calls acting as the keypad,
 * {ALARM_SRC_KEYPAD, n}; the grant each keypad holds lives here, on the worker. Its answer, and a
 * grant that changed, go back to the link task through a queue of their own (mrbsvc_poll), never
 * the commands'. All buffers are PSRAM, allocated once by mrbsvc_prepare: without them the master
 * doesn't offer PLINK_CAP_RB.
 */
#include "sdkconfig.h"

#if CONFIG_ALARM_LINK

#include <stdatomic.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_service.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "plink_internal.h"
#include "ui_port_esp.h"
#include "ui_remote/ui_remote.h"

static const char *TAG = "plink_rb";

#define RB_SMALL       1024u /* a call's arguments kept in its slot; a longer one takes the big one */
#define RB_RES_QUEUE   (PLINK_KEYPADS_MAX * 2u)
#define RB_GRANT_EVERY 1000u /* ms between the grants' upkeep */

_Static_assert(PLINK_RB_NO_ANSWER == (unsigned)UR_NO_ANSWER &&
                   PLINK_RB_BUSY == (unsigned)UR_BUSY && PLINK_RB_TOO_BIG == (unsigned)UR_TOO_BIG &&
                   PLINK_RB_GONE == (unsigned)UR_GONE && PLINK_RB_OK == (unsigned)UR_OK,
               "the link's statuses are ui_remote's");

/* One keypad's call waiting for the worker (one per keypad: the manager runs one at a time). */
typedef struct {
    bool used;
    uint32_t ticket;
    uint16_t call;
    uint32_t cfg_crc; /* the keypads' configuration when it came */
    bool same_fw;     /* the keypad runs this firmware's version */
    size_t len;
    bool big;         /* its arguments are in s_big */
    uint8_t args[RB_SMALL];
} rb_job_t;

typedef struct {
    uint8_t kind; /* 1 an answer, 2 a grant */
    uint8_t device;
    uint32_t ticket;
    uint8_t status;
    size_t len; /* the answer's bytes, in s_ans[device - 1] */
    plink_rb_grant_t grant;
} rb_res_t;

static bool s_ready;
static uint8_t *s_up;            /* the manager's upload buffer (UR_ARGS_MAX) */
static uint8_t *s_mans;          /* the manager's answers (PLINK_KEYPADS_MAX x UR_ANSWER_MAX) */
static uint8_t *s_ans;           /* the worker's answers, per keypad, until the link task copies */
static uint8_t *s_big;           /* one long call's arguments (UR_ARGS_MAX) */
static atomic_bool s_big_used;
static rb_job_t *s_jobs;         /* PLINK_KEYPADS_MAX */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_res_q;
static ui_remote_server_t *s_srv; /* the worker's */
static ui_remote_grant_t s_grant[PLINK_KEYPADS_MAX];  /* the worker's */
static plink_rb_grant_t s_sent[PLINK_KEYPADS_MAX];   /* the worker's: what the link was told */
static TickType_t s_grant_at;
static ui_backend_t s_base; /* this panel's backend (ui_port_esp's) */
static alarm_source_t s_src; /* the keypad the worker's call is for */

/* ---- the backend, as the keypad (on the worker) ---- */

static alarm_result_t as_program_enter(void *ctx, const char *code, uint8_t *user)
{
    (void)ctx;
    return alarm_service_program_enter(&s_src, code, user);
}

static void as_program_exit(void *ctx)
{
    (void)ctx;
    alarm_service_program_exit_owned(&s_src); /* only the keypad's own session */
}

static void as_program_keepalive(void *ctx)
{
    (void)ctx;
    alarm_service_program_keepalive();
}

static alarm_result_t as_program_set_code(void *ctx, uint8_t user, const char *code)
{
    (void)ctx;
    return alarm_service_program_set_code(&s_src, user, code);
}

static alarm_result_t as_user_set(void *ctx, const char *actor, uint8_t user, const char *code,
                                  alarm_authority_t a, uint8_t parts, const char *name)
{
    (void)ctx;
    return alarm_service_user_set(&s_src, actor, user, code, a, parts, name);
}

static alarm_result_t as_user_update(void *ctx, const char *actor, uint8_t user,
                                     alarm_authority_t a, uint8_t parts, const char *name)
{
    (void)ctx;
    return alarm_service_user_update(&s_src, actor, user, a, parts, name);
}

static alarm_result_t as_user_delete(void *ctx, const char *actor, uint8_t user)
{
    (void)ctx;
    return alarm_service_user_delete(&s_src, actor, user);
}

static void backend_for(uint8_t device, ui_backend_t *be)
{
    s_src = (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = device};
    *be = s_base;
    be->program_enter = as_program_enter;
    be->program_exit = as_program_exit;
    be->program_keepalive = as_program_keepalive;
    be->program_set_code = as_program_set_code;
    if (s_base.user_set != NULL) {
        be->user_set = as_user_set;
        be->user_update = as_user_update;
        be->user_delete = as_user_delete;
    }
}

/* ---- the start ---- */

uint16_t mrbsvc_prepare(void)
{
    if (s_ready) {
        return PLINK_CAP_RB;
    }
    s_up = alarm_mem_slow_malloc(UR_ARGS_MAX);
    s_big = alarm_mem_slow_malloc(UR_ARGS_MAX);
    s_mans = alarm_mem_slow_malloc((size_t)PLINK_KEYPADS_MAX * UR_ANSWER_MAX);
    s_ans = alarm_mem_slow_malloc((size_t)PLINK_KEYPADS_MAX * UR_ANSWER_MAX);
    s_jobs = alarm_mem_slow_malloc(sizeof(rb_job_t) * PLINK_KEYPADS_MAX);
    s_srv = alarm_mem_slow_malloc(sizeof(ui_remote_server_t));
    s_res_q = xQueueCreateWithCaps(RB_RES_QUEUE, sizeof(rb_res_t), alarm_mem_slow_caps());
    if (s_up == NULL || s_big == NULL || s_mans == NULL || s_ans == NULL || s_jobs == NULL ||
        s_srv == NULL || s_res_q == NULL) {
        ESP_LOGW(TAG, "no memory: keypads can't open this panel's settings");
        return 0;
    }
    memset(s_jobs, 0, sizeof(rb_job_t) * PLINK_KEYPADS_MAX);
    ui_port_esp_backend(&s_base);
    s_ready = true;
    return PLINK_CAP_RB;
}

void mrbsvc_attach(plink_master_t *m)
{
    if (s_ready) {
        plink_master_rb_attach(m, s_up, UR_ARGS_MAX, s_mans, UR_ANSWER_MAX);
    }
}

/* ---- the manager's port (the link task) ---- */

bool mrbsvc_run(void *ctx, uint32_t ticket, uint8_t device, uint16_t call, const uint8_t *args,
                size_t len)
{
    plink_master_t *m = ctx;
    if (!s_ready || device == 0u || device > PLINK_KEYPADS_MAX || len > UR_ARGS_MAX) {
        return false;
    }
    const bool big = len > RB_SMALL;
    if (big && atomic_exchange(&s_big_used, true)) {
        return false; /* another keypad's long call: this one's resend comes back */
    }
    /* the keypad's version against this one's (a configuration from another could drop fields) */
    bool same = false;
    plink_peer_info_t peers[PLINK_KEYPADS_MAX];
    const size_t n = plink_master_peers(m, link_now(), peers, PLINK_KEYPADS_MAX);
    for (size_t i = 0; i < n; i++) {
        if (peers[i].device == device) {
            same = strcmp(peers[i].hb.fw, g_link.fw) == 0;
        }
    }
    rb_job_t *j = &s_jobs[device - 1u];
    portENTER_CRITICAL(&s_mux);
    const bool busy = j->used;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        if (big) {
            atomic_store(&s_big_used, false);
        }
        return false;
    }
    memcpy(big ? s_big : j->args, args, len);
    portENTER_CRITICAL(&s_mux);
    j->ticket = ticket;
    j->call = call;
    j->cfg_crc = m->cfg_crc;
    j->same_fw = same;
    j->len = len;
    j->big = big;
    j->used = true;
    portEXIT_CRITICAL(&s_mux);
    msvc_kick();
    return true;
}

/* Answers and grants to the manager, each turn. */
void mrbsvc_poll(plink_master_t *m, uint64_t now)
{
    rb_res_t r;
    while (s_res_q != NULL && xQueueReceive(s_res_q, &r, 0) == pdTRUE) {
        if (r.kind == 1u) {
            uint8_t *a = s_ans + (size_t)(r.device - 1u) * UR_ANSWER_MAX;
            plink_master_rb_answer(m, now, r.ticket, r.status, a, r.len);
            plink_wipe(a, r.len);
        } else {
            plink_master_rb_grant(m, r.device, &r.grant);
        }
    }
}

/* ---- the worker ---- */

static atomic_uchar s_holder; /* the keypad holding installer mode, 0 none (any task reads) */

uint8_t mrbsvc_holder(void)
{
    return atomic_load(&s_holder);
}

void mrbsvc_auth(uint8_t device, uint8_t user)
{
    if (!s_ready || device == 0u || device > PLINK_KEYPADS_MAX || user == 0u ||
        user > ALARM_MAX_USERS || alarm_service_get_config(&s_srv->mine) != ESP_OK) {
        return;
    }
    const uint8_t a = (uint8_t)s_srv->mine.users[user - 1u].authority;
    plink_wipe(&s_srv->mine, sizeof s_srv->mine);
    ui_remote_grant_user(&s_grant[device - 1u], user, a, link_now());
}

static void run_jobs(void)
{
    for (uint8_t d = 1; d <= PLINK_KEYPADS_MAX; d++) {
        rb_job_t *j = &s_jobs[d - 1u];
        portENTER_CRITICAL(&s_mux);
        const bool used = j->used;
        portEXIT_CRITICAL(&s_mux);
        if (!used) {
            continue;
        }
        ui_backend_t be;
        backend_for(d, &be);
        uint8_t *args = j->big ? s_big : j->args;
        uint8_t *a = s_ans + (size_t)(d - 1u) * UR_ANSWER_MAX;
        size_t n = 0;
        const ui_remote_status_t st = ui_remote_serve(s_srv, &be, &s_grant[d - 1u], j->cfg_crc,
                                                      j->same_fw, j->call, args, j->len, a,
                                                      UR_ANSWER_MAX, &n, link_now());
        ESP_LOGI(TAG, "keypad %u: call 0x%04x: %u", (unsigned)d, (unsigned)j->call, (unsigned)st);
        plink_wipe(args, j->len); /* a code or a password, perhaps */
        rb_res_t r;
        memset(&r, 0, sizeof r);
        r.kind = 1u;
        r.device = d;
        r.ticket = j->ticket;
        r.status = (uint8_t)st;
        r.len = n;
        const bool big = j->big;
        portENTER_CRITICAL(&s_mux);
        j->used = false;
        portEXIT_CRITICAL(&s_mux);
        if (big) {
            atomic_store(&s_big_used, false);
        }
        (void)xQueueSend(s_res_q, &r, portMAX_DELAY); /* the link task drains it every turn */
    }
}

/* Each keypad's grant: its idle time, installer mode ended elsewhere, the keypad gone a minute
 * (LINK-059, LINK-060); what changed goes to the keypad (RB_GRANT). */
static void grants(void)
{
    alarm_snapshot_t *snap = &s_srv->snap;
    alarm_service_get_snapshot(snap);
    for (uint8_t d = 1; d <= PLINK_KEYPADS_MAX; d++) {
        bool known = false, gone = false;
        link_pub_lock();
        for (size_t i = 0; i < g_pub.keypads && i < PLINK_KEYPADS_MAX; i++) {
            const ui_link_keypad_t *k = &g_pub.keypad[i];
            if (k->device == d) {
                known = true;
                const int64_t now_ms = link_epoch_ms();
                gone = !k->connected &&
                       (k->heard_epoch_ms == 0 || (now_ms != 0 && now_ms - k->heard_epoch_ms >= 60000));
            }
        }
        link_pub_unlock();
        ui_remote_grant_t *g = &s_grant[d - 1u];
        if (!known) {
            memset(g, 0, sizeof *g);
            continue;
        }
        ui_backend_t be;
        backend_for(d, &be);
        ui_remote_grant_tick(&be, g, gone, link_now());
        plink_rb_grant_t want;
        memset(&want, 0, sizeof want);
        want.user = g->user;
        want.authority = g->authority;
        want.installer = g->installer;
        if (g->user != 0u) {
            const uint64_t idle = link_now() - g->last_ms;
            want.idle_left_s =
                (uint16_t)(idle < UR_GRANT_IDLE_MS ? (UR_GRANT_IDLE_MS - idle) / 60000u * 60u : 0u);
        }
        if (snap->program) {
            /* who holds installer mode: this keypad, or somewhere else (the panel itself, the
             * Panel Manager, another keypad) - the screen names the keypad it is */
            want.holder_kind = (uint8_t)ALARM_SRC_KEYPAD;
            want.holder_id = 0u;
            for (uint8_t o = 1; o <= PLINK_KEYPADS_MAX; o++) {
                if (s_grant[o - 1u].installer) {
                    want.holder_id = o;
                }
            }
        }
        uint8_t holder = 0;
        for (uint8_t o = 1; o <= PLINK_KEYPADS_MAX; o++) {
            holder = s_grant[o - 1u].installer ? o : holder;
        }
        atomic_store(&s_holder, holder);
        want.areas = ui_remote_areas(&be);
        if (memcmp(&want, &s_sent[d - 1u], sizeof want) != 0) {
            s_sent[d - 1u] = want;
            rb_res_t r;
            memset(&r, 0, sizeof r);
            r.kind = 2u;
            r.device = d;
            r.grant = want;
            if (xQueueSend(s_res_q, &r, 0) != pdTRUE) {
                memset(&s_sent[d - 1u], 0, sizeof s_sent[d - 1u]); /* full: told next second */
            }
        }
    }
}

void mrbsvc_worker(void)
{
    if (!s_ready) {
        return;
    }
    run_jobs();
    if (xTaskGetTickCount() - s_grant_at >= pdMS_TO_TICKS(RB_GRANT_EVERY)) {
        s_grant_at = xTaskGetTickCount();
        grants();
    }
}

#endif /* CONFIG_ALARM_LINK */
