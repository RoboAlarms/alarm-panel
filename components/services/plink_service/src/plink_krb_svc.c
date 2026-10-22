/*
 * plink_krb_svc.c - the remote backend on a keypad (BL-223; docs/features/11 "The master's settings
 * and installer menu on keypads", LINK-057..LINK-065): the screen's entries for its master's pages
 * are ui_remote proxies whose port is here.
 *
 * A write (a save, a bind, entering installer mode) is handed to the link task, which runs it on
 * the manager (one call at a time) and gives the answer back; the caller waits up to 13 s - the
 * screen calls it from its call task (LINK-054), never its own. A read (a status, a list, a
 * device's page, progress) is answered at once from the keypad's copy of its last answer: the link
 * task asks again in the background while the screen keeps reading it (at most once a second; a
 * read the screen stopped asking for is dropped after 10 s). A read not copied yet answers
 * UR_NO_ANSWER - "Getting it from Harbor House" - and krbsvc_gen moves when an answer arrives, so
 * the page reads again (LINK-063). Writes go before reads.
 * All buffers are PSRAM, allocated once by krbsvc_prepare: without them the keypad doesn't offer
 * PLINK_CAP_RB and its master's pages stay hidden.
 */
#include "sdkconfig.h"

#if CONFIG_ALARM_LINK

#include <stdatomic.h>
#include <string.h>

#include "alarm_mem.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "plink_internal.h"
#include "ui_remote/ui_remote.h"

static const char *TAG = "plink_rb";

#define RD_SLOTS     12u    /* reads kept */
#define RD_ARGS_MAX  128u   /* a read's arguments (a zone, a node, a query) */
#define RD_FRESH_MS  1000u  /* a copy older than this is asked again ... */
#define RD_WANTED_MS 10000u /* ... while the screen read it this recently */
#define WRITE_WAIT_MS 13000u

typedef struct {
    bool used;
    uint16_t call;
    size_t args_len;
    uint8_t args[RD_ARGS_MAX];
    bool have;            /* an answer is copied */
    uint8_t status;
    size_t len;
    uint8_t *ans;         /* UR_ANSWER_MAX, PSRAM */
    uint64_t at;          /* when it was copied */
    uint64_t read_at;     /* when the screen last read it */
} rd_t;

static bool s_ready;
static uint8_t *s_kargs, *s_kans;   /* the manager's call buffers */
static ui_remote_proxy_t *s_px;     /* the proxies' scratch */
static rd_t *s_rd;                  /* RD_SLOTS */
static SemaphoreHandle_t s_lock;    /* s_rd and s_box */
static SemaphoreHandle_t s_write;   /* one write at a time */
static SemaphoreHandle_t s_done;    /* the write's answer */
static atomic_uint s_gen;           /* moves when a read's answer changes */
static struct {
    bool pending;
    uint32_t gen;
    uint16_t call;
    size_t len;
    uint8_t *args; /* UR_ARGS_MAX, PSRAM */
    uint8_t status;
    size_t ans_len;
    uint8_t *ans;  /* UR_ANSWER_MAX, PSRAM */
    uint32_t ans_gen;
} s_box;
static uint32_t s_box_gen;
/* the link task's call under way */
static struct {
    bool active;
    uint32_t req;
    bool write;
    uint32_t gen;  /* the write's */
    int slot;      /* the read's */
} s_cur;
static uint64_t s_rd_at; /* the last background read started */
static uint32_t s_grant_key; /* the grant the read copies were answered under (the link task's) */

static uint64_t now_ms(void)
{
    return link_now();
}

/* ---- the port (the caller's task) ---- */

static int rd_find(uint16_t call, const uint8_t *args, size_t len)
{
    for (int i = 0; i < (int)RD_SLOTS; i++) {
        const rd_t *r = &s_rd[i];
        if (r->used && r->call == call && r->args_len == len && memcmp(r->args, args, len) == 0) {
            return i;
        }
    }
    return -1;
}

static int rd_new(void)
{
    int free_i = -1, oldest = 0;
    for (int i = 0; i < (int)RD_SLOTS; i++) {
        if (!s_rd[i].used) {
            free_i = i;
            break;
        }
        if (s_rd[i].read_at < s_rd[oldest].read_at && !(s_cur.active && s_cur.slot == i)) {
            oldest = i;
        }
    }
    return free_i >= 0 ? free_i : oldest;
}

static ui_remote_status_t read_copy(uint16_t call, const uint8_t *args, size_t len, uint8_t *ans,
                                    size_t cap, size_t *ans_len)
{
    ui_remote_status_t st = UR_NO_ANSWER;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = rd_find(call, args, len);
    if (i < 0) {
        i = rd_new();
        rd_t *r = &s_rd[i];
        r->used = true;
        r->call = call;
        r->args_len = len;
        memcpy(r->args, args, len);
        r->have = false;
        r->at = 0u;
    }
    rd_t *r = &s_rd[i];
    r->read_at = now_ms();
    if (r->have) {
        st = (ui_remote_status_t)r->status;
        if (st == UR_OK) {
            if (r->len <= cap) {
                memcpy(ans, r->ans, r->len);
                *ans_len = r->len;
            } else {
                st = UR_TOO_BIG;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return st;
}

static ui_remote_status_t port_call(void *ctx, uint16_t call, const uint8_t *args, size_t len,
                                    uint8_t *ans, size_t cap, size_t *ans_len)
{
    (void)ctx;
    *ans_len = 0u;
    if (!s_ready || len > UR_ARGS_MAX) {
        return UR_UNAVAILABLE;
    }
    const ui_remote_info_t *info = ui_remote_info(call);
    if (info != NULL && info->read && len <= RD_ARGS_MAX) {
        return read_copy(call, args, len, ans, cap, ans_len);
    }
    xSemaphoreTake(s_write, portMAX_DELAY);
    (void)xSemaphoreTake(s_done, 0); /* the answer to one given up on */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_box.args, args, len);
    s_box.len = len;
    s_box.call = call;
    const uint32_t gen = ++s_box_gen;
    s_box.gen = gen;
    s_box.pending = true;
    xSemaphoreGive(s_lock);
    ui_remote_status_t st = UR_NO_ANSWER;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(WRITE_WAIT_MS)) == pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_box.ans_gen == gen) {
            st = (ui_remote_status_t)s_box.status;
            if (st == UR_OK) {
                if (s_box.ans_len <= cap) {
                    memcpy(ans, s_box.ans, s_box.ans_len);
                    *ans_len = s_box.ans_len;
                } else {
                    st = UR_TOO_BIG;
                }
            }
            plink_wipe(s_box.ans, s_box.ans_len);
        }
        xSemaphoreGive(s_lock);
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_box.pending && s_box.gen == gen) {
            s_box.pending = false; /* withdrawn before the link task took it */
            plink_wipe(s_box.args, s_box.len);
        }
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_write);
    return st;
}

/* ---- the link task ---- */

static atomic_uint s_cfg_crc; /* the installed configuration's, as the link task last saw it */

static uint32_t port_cfg_crc(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_cfg_crc); /* a save's base (LINK-061) */
}

static void start(plink_keypad_t *k, uint64_t now)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_box.pending) {
        s_box.pending = false;
        const uint32_t gen = s_box.gen;
        const bool ok = plink_keypad_rb_start(k, now, s_box.call, s_box.args, s_box.len, &s_cur.req);
        plink_wipe(s_box.args, s_box.len); /* the manager has its own copy now */
        if (ok) {
            s_cur.active = true;
            s_cur.write = true;
            s_cur.gen = gen;
        } else {
            s_box.status = (uint8_t)UR_UNAVAILABLE; /* no session holding RB: at once */
            s_box.ans_len = 0u;
            s_box.ans_gen = gen;
            xSemaphoreGive(s_lock);
            xSemaphoreGive(s_done);
            return;
        }
        xSemaphoreGive(s_lock);
        return;
    }
    /* a read the screen keeps asking for, oldest copy first, at most one start per 250 ms */
    int pick = -1;
    if (now >= s_rd_at + 250u) {
        for (int i = 0; i < (int)RD_SLOTS; i++) {
            const rd_t *r = &s_rd[i];
            if (!r->used || now > r->read_at + RD_WANTED_MS ||
                (r->have && now < r->at + RD_FRESH_MS)) {
                continue;
            }
            if (pick < 0 || !r->have || r->at < s_rd[pick].at) {
                pick = i;
            }
        }
    }
    if (pick >= 0) {
        rd_t *r = &s_rd[pick];
        if (plink_keypad_rb_start(k, now, r->call, r->args, r->args_len, &s_cur.req)) {
            s_cur.active = true;
            s_cur.write = false;
            s_cur.slot = pick;
            s_rd_at = now;
        }
    }
    xSemaphoreGive(s_lock);
}

void krbsvc_poll(plink_keypad_t *k, uint64_t now)
{
    if (!s_ready || k == NULL) {
        return;
    }
    atomic_store(&s_cfg_crc, plink_keypad_cfg_crc(k));
    /* a new grant (a code, installer mode entered or left, the master's groups): the copies were
     * answered under the old one - a refusal then is no answer now - so every read asks again */
    plink_rb_grant_t g;
    memset(&g, 0, sizeof g);
    (void)plink_keypad_rb_grant(k, &g);
    const uint32_t key = (uint32_t)g.user | ((uint32_t)g.installer << 8) | (g.areas << 9);
    if (key != s_grant_key) {
        s_grant_key = key;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < RD_SLOTS; i++) {
            s_rd[i].have = false;
        }
        xSemaphoreGive(s_lock);
        atomic_fetch_add(&s_gen, 1u);
    }
    if (!s_cur.active) {
        start(k, now);
        return;
    }
    size_t n = 0u;
    const uint8_t st = plink_keypad_rb_poll(k, s_cur.req, &n);
    if (st == PLINK_RB_PENDING) {
        return;
    }
    s_cur.active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cur.write) {
        if (s_box_gen == s_cur.gen) { /* the caller still waits */
            s_box.status = st;
            s_box.ans_len = st == PLINK_RB_OK && n <= UR_ANSWER_MAX ? n : 0u;
            memcpy(s_box.ans, s_kans, s_box.ans_len);
            s_box.ans_gen = s_cur.gen;
            xSemaphoreGive(s_lock);
            xSemaphoreGive(s_done);
        } else {
            xSemaphoreGive(s_lock);
        }
        plink_wipe(s_kans, n);
        ESP_LOGI(TAG, "call answered: %u", (unsigned)st);
        return;
    }
    rd_t *r = &s_rd[s_cur.slot];
    if (r->used && st != PLINK_RB_NO_ANSWER) {
        const bool changed = !r->have || r->status != st || r->len != n ||
                             (st == PLINK_RB_OK && memcmp(r->ans, s_kans, n) != 0);
        r->status = st;
        r->len = st == PLINK_RB_OK && n <= UR_ANSWER_MAX ? n : 0u;
        memcpy(r->ans, s_kans, r->len);
        r->have = true;
        r->at = now;
        if (changed) {
            atomic_fetch_add(&s_gen, 1u); /* the open page reads again */
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---- the start ---- */

uint16_t krbsvc_prepare(void)
{
    if (s_ready) {
        return PLINK_CAP_RB;
    }
    s_kargs = alarm_mem_slow_malloc(UR_ARGS_MAX);
    s_kans = alarm_mem_slow_malloc(UR_ANSWER_MAX);
    s_box.args = alarm_mem_slow_malloc(UR_ARGS_MAX);
    s_box.ans = alarm_mem_slow_malloc(UR_ANSWER_MAX);
    s_px = alarm_mem_slow_malloc(sizeof(ui_remote_proxy_t));
    s_rd = alarm_mem_slow_malloc(sizeof(rd_t) * RD_SLOTS);
    s_lock = xSemaphoreCreateMutex();
    s_write = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    bool ok = s_kargs != NULL && s_kans != NULL && s_box.args != NULL && s_box.ans != NULL &&
              s_px != NULL && s_rd != NULL && s_lock != NULL && s_write != NULL && s_done != NULL;
    if (ok) {
        memset(s_rd, 0, sizeof(rd_t) * RD_SLOTS);
        for (size_t i = 0; i < RD_SLOTS && ok; i++) {
            s_rd[i].ans = alarm_mem_slow_malloc(UR_ANSWER_MAX);
            ok = s_rd[i].ans != NULL;
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "no memory: the master's pages stay off this keypad");
        return 0;
    }
    memset(s_px, 0, sizeof *s_px);
    s_px->port = (ui_remote_port_t){.ctx = NULL, .call = port_call, .cfg_crc = port_cfg_crc};
    s_ready = true;
    return PLINK_CAP_RB;
}

void krbsvc_attach(plink_keypad_t *k)
{
    if (s_ready) {
        plink_keypad_rb_attach(k, s_kargs, UR_ARGS_MAX, s_kans, UR_ANSWER_MAX);
    }
}

void krbsvc_backend(ui_backend_t *be)
{
    if (s_ready) {
        ui_remote_proxy(be, s_px, UR_AREA_SESSION | UR_AREA_CONFIG | UR_AREA_USERS | UR_AREA_ZONES |
                                      UR_AREA_ZWAVE | UR_AREA_ZIGBEE | UR_AREA_OUTPUTS);
    }
}

uint32_t krbsvc_gen(void)
{
    return atomic_load(&s_gen);
}

uint8_t krbsvc_last(void)
{
    return s_ready ? (uint8_t)s_px->last : (uint8_t)UR_UNAVAILABLE;
}

#if CONFIG_ALARM_BUILD_DEV
/* ---- the bench: `plink rb ...` calls the master's pages as the screen does (BL-223) ---------- */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_strings.h"
#include "esp_timer.h"

static ui_backend_t s_cb; /* the console's own entries over the same proxies */
static alarm_config_t *s_ccfg;

static uint32_t ms_since(int64_t t0)
{
    return (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

/* A read: asked again until an answer is copied (at most 3 s), as the screen's page does. */
#define READ(expr)                                                                         \
    do {                                                                                   \
        for (int i_ = 0; i_ < 30; i_++) {                                                  \
            expr;                                                                          \
            if (s_px->last != UR_NO_ANSWER) {                                              \
                break;                                                                     \
            }                                                                              \
            vTaskDelay(pdMS_TO_TICKS(100));                                                \
        }                                                                                  \
    } while (0)

int krbsvc_console(int argc, char **argv)
{
    if (!s_ready) {
        printf("no remote backend on this panel\n");
        return 1;
    }
    if (s_cb.ctx == NULL) {
        krbsvc_backend(&s_cb);
    }
    const int64_t t0 = esp_timer_get_time();
    const char *w = argc >= 3 ? argv[2] : "";
    if (argc == 2) {
        link_pub_lock();
        const ui_link_info_t li = g_pub.kinfo;
        link_pub_unlock();
        printf("areas %08" PRIx32 ", installer %s, holder %u, gen %" PRIu32 ", last %u\n",
               li.remote_areas, li.remote_installer ? "this keypad" : "no", (unsigned)li.remote_holder,
               li.remote_gen, (unsigned)s_px->last);
        return 0;
    }
    if (strcmp(w, "enter") == 0 && argc == 4) {
        uint8_t user = 0;
        const alarm_result_t r = s_cb.program_enter(s_cb.ctx, argv[3], &user);
        printf("enter: %s, user %u, status %u, %" PRIu32 " ms\n", alarm_result_name(r),
               (unsigned)user, (unsigned)s_px->last, ms_since(t0));
        return 0;
    }
    if (strcmp(w, "exit") == 0) {
        s_cb.program_exit(s_cb.ctx);
        printf("exit: status %u, %" PRIu32 " ms\n", (unsigned)s_px->last, ms_since(t0));
        return 0;
    }
    if (strcmp(w, "sources") == 0) {
        ui_zone_source_t src[8];
        size_t n = 0;
        READ(n = s_cb.zone_sources(s_cb.ctx, src, 8));
        printf("%u sources (status %u):", (unsigned)n, (unsigned)s_px->last);
        for (size_t i = 0; i < n; i++) {
            printf(" kind %u%s", (unsigned)src[i].kind, src[i].usable ? "" : " (not usable)");
        }
        printf("\n");
        return 0;
    }
    if (strcmp(w, "learn") == 0 && argc == 5) {
        s_cb.zone_learn(s_cb.ctx, (uint8_t)atoi(argv[3]), strcmp(argv[4], "on") == 0);
        printf("learn: status %u, %" PRIu32 " ms\n", (unsigned)s_px->last, ms_since(t0));
        return 0;
    }
    if (strcmp(w, "cands") == 0 && argc == 4) {
        static zsrc_candidate_t c[16];
        size_t n = 0;
        READ(n = s_cb.zone_candidates(s_cb.ctx, (uint8_t)atoi(argv[3]), "", c, 16));
        printf("%u candidates (status %u):\n", (unsigned)n, (unsigned)s_px->last);
        for (size_t i = 0; i < n; i++) {
            printf("  %s id %" PRIu64 " input %u, %s%s%s, zone %u\n", c[i].name, c[i].addr.id,
                   (unsigned)c[i].addr.input, c[i].state[0] ? c[i].state : "-",
                   c[i].changed ? ", changed" : "", c[i].best ? ", best" : "", (unsigned)c[i].zone);
        }
        return 0;
    }
    if (strcmp(w, "bind") == 0 && argc >= 6) {
        zone_hub_binding_t b;
        memset(&b, 0, sizeof b);
        b.in[ZSRC_ROLE_MAIN].addr.kind = (uint8_t)atoi(argv[4]);
        b.in[ZSRC_ROLE_MAIN].addr.id = strtoull(argv[5], NULL, 0);
        b.in[ZSRC_ROLE_MAIN].addr.input = argc >= 7 ? (uint8_t)atoi(argv[6]) : 0u;
        const zone_hub_result_t r = s_cb.zone_bind(s_cb.ctx, (uint16_t)atoi(argv[3]), &b);
        printf("bind: %u, status %u, %" PRIu32 " ms\n", (unsigned)r, (unsigned)s_px->last,
               ms_since(t0));
        return 0;
    }
    if (strcmp(w, "unbind") == 0 && argc == 4) {
        const zone_hub_result_t r = s_cb.zone_unbind(s_cb.ctx, (uint16_t)atoi(argv[3]));
        printf("unbind: %u, status %u, %" PRIu32 " ms\n", (unsigned)r, (unsigned)s_px->last,
               ms_since(t0));
        return 0;
    }
    if (strcmp(w, "info") == 0 && argc == 4) {
        static zone_hub_info_t info;
        bool found = false;
        READ(found = s_cb.zone_info(s_cb.ctx, (uint16_t)atoi(argv[3]), &info));
        printf("zone %s: %s, kind %u id %" PRIu64 ", status %u\n", argv[3],
               found ? info.address : "no sensor", (unsigned)info.binding.in[0].addr.kind,
               info.binding.in[0].addr.id, (unsigned)s_px->last);
        return 0;
    }
    if (strcmp(w, "zone") == 0 && argc >= 5) {
        const unsigned z = (unsigned)atoi(argv[3]);
        if (s_ccfg == NULL) {
            s_ccfg = alarm_mem_slow_malloc(sizeof *s_ccfg);
        }
        if (s_ccfg == NULL || z == 0u || z > ALARM_MAX_ZONES || !ksvc_ui_config(s_ccfg)) {
            printf("no configuration copy here yet\n");
            return 1;
        }
        if (strcmp(argv[4], "off") == 0) {
            s_ccfg->zones[z - 1u].type = ALARM_ZT_NOT_USED;
        } else {
            alarm_zone_config_defaults(&s_ccfg->zones[z - 1u], (uint16_t)z, ALARM_ZT_PERIMETER, 1u);
            snprintf(s_ccfg->zones[z - 1u].name, sizeof s_ccfg->zones[z - 1u].name, "%s", argv[4]);
        }
        const alarm_result_t r = s_cb.set_config(s_cb.ctx, s_ccfg);
        printf("save: %s, status %u, %" PRIu32 " ms\n", alarm_result_name(r), (unsigned)s_px->last,
               ms_since(t0));
        return 0;
    }
    if (strcmp(w, "zw") == 0) {
        static ui_zw_status_t st;
        bool ok = false;
        READ(ok = s_cb.zw_status(s_cb.ctx, &st));
        printf("Z-Wave: %s, %u devices, home %08" PRIx32 ", status %u\n", ok ? "radio" : "none",
               (unsigned)st.devices, st.home_id, (unsigned)s_px->last);
        return 0;
    }
    if (strcmp(w, "zwadd") == 0 || strcmp(w, "zwstop") == 0 || strcmp(w, "zwdismiss") == 0) {
        ui_zw_request_t q;
        memset(&q, 0, sizeof q);
        q.kind = strcmp(w, "zwadd") == 0    ? UI_ZW_REQ_ADD
                 : strcmp(w, "zwstop") == 0 ? UI_ZW_REQ_STOP
                                            : UI_ZW_REQ_DISMISS;
        const ui_zw_req_result_t r = s_cb.zw_request(s_cb.ctx, &q);
        printf("%s: %u, status %u, %" PRIu32 " ms\n", w, (unsigned)r, (unsigned)s_px->last,
               ms_since(t0));
        return 0;
    }
    if (strcmp(w, "zwprog") == 0) {
        static ui_zw_progress_t p;
        READ(s_cb.zw_progress(s_cb.ctx, &p));
        printf("Z-Wave progress: op %u step %u result %u, %u s left, node %u %s, status %u\n",
               (unsigned)p.op, (unsigned)p.step, (unsigned)p.result, (unsigned)p.seconds_left,
               (unsigned)p.node, p.model, (unsigned)s_px->last);
        return 0;
    }
    if (strcmp(w, "zb") == 0) {
        static ui_zb_status_t st;
        bool ok = false;
        READ(ok = s_cb.zb_status(s_cb.ctx, &st));
        printf("Zigbee: %s, status %u\n", ok ? "radio" : "none", (unsigned)s_px->last);
        return 0;
    }
    if (strcmp(w, "out") == 0) {
        static ui_out_status_t st;
        bool ok = false;
        READ(ok = s_cb.out_status(s_cb.ctx, &st));
        printf("outputs: %s, wired %u, where \"%s\", 433 receiver %u, status %u\n",
               ok ? "yes" : "no", (unsigned)st.wired, st.where != NULL ? st.where : "",
               (unsigned)st.rf_receiver, (unsigned)s_px->last);
        return 0;
    }
    printf("plink rb [enter <code>|exit|sources|learn <kind> on|off|cands <kind>|bind <zone> <kind> "
           "<id> [input]|unbind <zone>|info <zone>|zone <n> <name>|off|zw|zwadd|zwstop|zwdismiss|zwprog|zb|out]\n");
    return 1;
}
#endif /* CONFIG_ALARM_BUILD_DEV */

#endif /* CONFIG_ALARM_LINK */
