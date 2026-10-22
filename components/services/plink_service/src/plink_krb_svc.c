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

#endif /* CONFIG_ALARM_LINK */
