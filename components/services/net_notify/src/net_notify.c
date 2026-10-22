#include "sdkconfig.h"
#include "i18n/i18n.h"
#include "ui_backend/ui_why_words.h"
#include "alarm_mem.h"
#if CONFIG_ALARM_FEATURE_NOTIFY
/*
 * net_notify.c - pushing notifications to a phone (see net_notify.h, docs/features/26-28).
 *
 * One task ("notify") owns both services. The engine's listener only queues; the task decides
 * (components/core/notify), builds the request (components/core/notify) and posts it. Everything that can
 * be got wrong without a network is host-tested elsewhere, so what is left here is transport.
 */
#include "net_notify.h"

#include "esp_attr.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "alarm/alarm_config.h"
#include "alarm_proto/proto_report.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "notify/notify_gate.h"
#include "notify/notify_http.h"
#include "notify/notify_words.h"
#include "nvs.h"
#include "panel_log.h"

static const char *TAG = "notify";

#define NS_SHARED "notify"
#define NS_NTFY   "ntfy"
#define NS_PUSH   "pushover"

#define TASK_STACK    8192u /* esp_http_client + mbedTLS run inline here, as in panel_ota */
#define TASK_PRIORITY 4
#define DEBOUNCE_MS   200u
#define ACKED_SAVE_MS 5000u
#define HTTP_TIMEOUT  10000
/* NOTE-014/035: the gap between requests and the backoff after a failure are
 * NOTIFY_SEND_GAP_MS and notify_backoff() (notify/notify_http.h), host-tested. */
#define DIGEST_RETRY_MS 30000u /* the outage digest could not be sent: try again after this */
#define CA_MAX        4096u
#define TEST_WAIT_MS  10000u
#define NTFY_HOSTED   "https://ntfy.sh"
#define LIVE_QUEUE    8
#define BODY_SCRATCH  512u /* enough of a refusal to quote back to the owner */

#define NB_REPORT   (1u << 0)
#define NB_SETTINGS (1u << 1)
#define NB_NETWORK  (1u << 2)
#define NB_TEST     (1u << 3)
#define NB_CANCEL   (1u << 4)

/* ---- settings ------------------------------------------------------------------------------- */

typedef struct {
    bool enabled;
    bool self_hosted;
    char server[96];
    char topic[64];
    char token[64];
    uint8_t tls;
    uint8_t cats;
    bool silent_too;
} ntfy_cfg_t;

typedef struct {
    bool enabled;
    char user_key[48];
    char app_token[48];
    uint8_t cats;
    bool silent_too;
    bool emergency;
    uint16_t retry_s;
    uint16_t expire_s;
    char sound[16];
    char device[26];
} push_cfg_t;

/* ---- per-service runtime -------------------------------------------------------------------- */

typedef struct {
    proto_report_t rep;
    notify_gate_t gate;
    proto_report_port_t port;
    uint32_t acked_saved;
    bool acked_dirty;
    uint32_t sent;
    uint32_t today;
    alarm_ms_t last_ok;
    bool ever_sent;
    net_notify_state_t state;
    net_notify_fail_t fail;
    char detail[80];
    uint8_t retry_n;
    alarm_ms_t retry_at;
    alarm_ms_t last_send;
    int32_t quota_left; /* Pushover's X-Limit-App-Remaining, -1 = not told */
    int msg_id;
    alarm_ms_t digest_retry_at; /* the outage digest failed to send: not before this */
} provider_t;

static ntfy_cfg_t s_ntfy;
static push_cfg_t s_push;
static provider_t s_prov[NET_NOTIFY_PROVIDERS] ALARM_SLOW_BSS; /* task-only: slow memory */
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static QueueHandle_t s_live; /* reports the log couldn't keep (LOG-010) */
static alarm_config_t *s_cfg;
static bool s_have_cfg;
static uint32_t s_cfg_seq;
static alarm_snapshot_t s_snap ALARM_SLOW_BSS;
static notify_request_t *s_req;
static char *s_resp;
static char *s_ca;
static uint8_t s_cancel_partitions; /* bit n-1: partition n needs its repeat cancelled */
static alarm_ms_t s_cancel_retry_at;  /* task-only: a cancel failed, not again before this */
static uint8_t s_cancel_retry_n;      /* task-only: cancels failed in a row */
/* [n-1]: the seq of the disarm, cancel or silence that last dealt with partition n's alarm
 * (PUSH-005). Written by the alarm task, read by ours; RAM only. */
static uint32_t s_dealt_seq[8];
static uint8_t s_live_done; /* task-only: bit p - service p is finished with s_live's head (S02) */
static volatile bool s_validate_pending; /* a Pushover key is waiting to be checked */
static volatile uint8_t s_test_provider = 0xFFu;
static volatile bool s_test_done;
static volatile bool s_test_ok;
static char s_test_why[80];

/* The task's own copies. It must never hold s_lock across an HTTPS request - the UI polls
 * the status once a second on the LVGL thread, and a post can take the full timeout - so
 * the settings are copied here under the lock and read without it afterwards. */
static ntfy_cfg_t s_ntfy_run;
static push_cfg_t s_push_run;
static net_notify_status_t s_status[NET_NOTIFY_PROVIDERS]; /* what the UI reads */

static void wake(uint32_t bits)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* ---- NVS -------------------------------------------------------------------------------------*/

static void str_get(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

static void u8_get(nvs_handle_t h, const char *key, uint8_t *out)
{
    uint8_t v = 0;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *out = v;
    }
}

static void defaults(void)
{
    memset(&s_ntfy, 0, sizeof(s_ntfy));
    memset(&s_push, 0, sizeof(s_push));
    s_ntfy.cats = NOTIFY_CATS_DEFAULT;
    s_push.cats = NOTIFY_CATS_DEFAULT;
    s_push.emergency = true; /* PUSH-004 */
    s_push.retry_s = 60u;
    s_push.expire_s = 1800u;
    snprintf(s_push.sound, sizeof(s_push.sound), "siren");
}

/* An unguessable topic: the whole security of a hosted ntfy is that nobody else knows it
 * (NTFY-004). No look-alike characters, so it can be read off the screen. */
void net_notify_new_topic(char *out, size_t len)
{
    static const char ALPHABET[] = "abcdefghijkmnpqrstuvwxyz23456789";
    char tail[9];
    for (unsigned i = 0; i < 8u; i++) {
        tail[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1u)];
    }
    tail[8] = '\0';
    snprintf(out, len, "roboalarms-%s", tail);
}

static void load_settings(void)
{
    defaults();
    nvs_handle_t h;
    if (nvs_open(NS_NTFY, NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0;
        u8_get(h, "on", &on);
        s_ntfy.enabled = on != 0u;
        uint8_t own = 0;
        u8_get(h, "own", &own);
        s_ntfy.self_hosted = own != 0u;
        str_get(h, "base", s_ntfy.server, sizeof(s_ntfy.server));
        str_get(h, "topic", s_ntfy.topic, sizeof(s_ntfy.topic));
        str_get(h, "tok", s_ntfy.token, sizeof(s_ntfy.token));
        u8_get(h, "tls", &s_ntfy.tls);
        u8_get(h, "cats", &s_ntfy.cats);
        uint8_t sil = 0;
        u8_get(h, "silnt", &sil);
        s_ntfy.silent_too = sil != 0u;
        nvs_close(h);
    }
    if (s_ntfy.topic[0] == '\0') {
        /* A panel that has never been set up still arrives ready (NTFY-003). */
        net_notify_new_topic(s_ntfy.topic, sizeof(s_ntfy.topic));
    }
    if (nvs_open(NS_PUSH, NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0;
        u8_get(h, "on", &on);
        s_push.enabled = on != 0u;
        str_get(h, "usr", s_push.user_key, sizeof(s_push.user_key));
        str_get(h, "app", s_push.app_token, sizeof(s_push.app_token));
        u8_get(h, "cats", &s_push.cats);
        uint8_t sil = 0;
        u8_get(h, "silnt", &sil);
        s_push.silent_too = sil != 0u;
        uint8_t emer = 1u;
        u8_get(h, "emer", &emer);
        s_push.emergency = emer != 0u;
        uint16_t v = 0;
        if (nvs_get_u16(h, "retry", &v) == ESP_OK && v != 0u) {
            s_push.retry_s = v;
        }
        if (nvs_get_u16(h, "exp", &v) == ESP_OK && v != 0u) {
            s_push.expire_s = v;
        }
        str_get(h, "snd", s_push.sound, sizeof(s_push.sound));
        str_get(h, "dev", s_push.device, sizeof(s_push.device));
        nvs_close(h);
    }
}

/* Writes one service's settings; stops at the first key that fails (code review S03). The keys
 * are separate NVS entries, so a failure can leave some written: the setters then write back
 * what is running, so a restart finds the settings the panel was using, as far as the storage
 * allows. */
static esp_err_t store_ntfy(const ntfy_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_NTFY, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* Off is written first and on last: a failure part way through must never leave the
     * service switched on with the server or topic it had before (code review). */
    if (!c->enabled) {
        err = nvs_set_u8(h, "on", 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "own", c->self_hosted ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "base", c->server);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "topic", c->topic);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "tok", c->token);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tls", c->tls);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "cats", c->cats);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "silnt", c->silent_too ? 1u : 0u);
    }
    if (err == ESP_OK && c->enabled) {
        err = nvs_set_u8(h, "on", 1u);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t store_push(const push_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_PUSH, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* Off first, on last, as in store_ntfy(): never on with the keys it had before. */
    if (!c->enabled) {
        err = nvs_set_u8(h, "on", 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "usr", c->user_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "app", c->app_token);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "cats", c->cats);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "silnt", c->silent_too ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "emer", c->emergency ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "retry", c->retry_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "exp", c->expire_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "snd", c->sound);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "dev", c->device);
    }
    if (err == ESP_OK && c->enabled) {
        err = nvs_set_u8(h, "on", 1u);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static const char *acked_key(uint8_t p)
{
    return p == NET_NOTIFY_NTFY ? "ackedn" : "ackedp";
}

static uint32_t load_acked(uint8_t p)
{
    nvs_handle_t h;
    uint32_t v = 0u;
    bool have = false;
    if (nvs_open(NS_SHARED, NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_u32(h, acked_key(p), &v) == ESP_OK;
        nvs_close(h);
    }
    /* Without a saved position, start from now: switching a service on must not push the whole
     * event log to somebody's phone (NOTE-011). */
    return have ? v : panel_log_newest_index();
}

static void save_acked(uint8_t p, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS_SHARED, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, acked_key(p), v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- HTTP ------------------------------------------------------------------------------------ */

/* Pushover tells how many messages the application may still send this month in a response
 * header; the client hands each header to this while it reads the answer (PUSH-011). */
static esp_err_t header_seen(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data != NULL && e->header_key != NULL &&
        e->header_value != NULL && strcasecmp(e->header_key, "X-Limit-App-Remaining") == 0) {
        char *end = NULL;
        const long v = strtol(e->header_value, &end, 10);
        if (end != e->header_value && v >= 0 && v <= 1000000) {
            *(int32_t *)e->user_data = (int32_t)v;
        }
    }
    return ESP_OK;
}

/* Posts one built request. Returns the HTTP status, or -1 when it never got that far.
 * 'body' takes as much of the answer as fits, for quoting a refusal back to the owner. */
static int post(const notify_request_t *req, bool own_ca, char *body, size_t body_len,
                int32_t *quota_left)
{
    esp_http_client_config_t cfg = {
        .url = req->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .user_agent = "RoboAlarms-panel",
        .keep_alive_enable = true,
        .event_handler = quota_left != NULL ? header_seen : NULL,
        .user_data = quota_left,
    };
    if (own_ca && s_ca != NULL && s_ca[0] != '\0') {
        cfg.cert_pem = s_ca;
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    int status = -1;
    esp_http_client_set_header(c, "Content-Type", req->content_type);
    if (req->auth[0] != '\0') {
        esp_http_client_set_header(c, "Authorization", req->auth);
    }
    if (esp_http_client_open(c, (int)req->body_len) == ESP_OK) {
        const int wrote = req->body_len > 0u
                              ? esp_http_client_write(c, req->body, req->body_len)
                              : 0;
        if (wrote >= 0 && (size_t)wrote == req->body_len &&
            esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            if (body != NULL && body_len > 0u) {
                const int n = esp_http_client_read(c, body, (int)body_len - 1);
                body[n > 0 ? (size_t)n : 0u] = '\0';
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return status;
}

static void classify(provider_t *pr, int status, const char *body)
{
    if (status == 401 || status == 403) {
        pr->fail = NET_NOTIFY_FAIL_AUTH;
    } else if (status == 429) {
        pr->fail = NET_NOTIFY_FAIL_QUOTA;
    } else if (status < 0) {
        pr->fail = NET_NOTIFY_FAIL_UNREACHABLE;
    } else {
        pr->fail = NET_NOTIFY_FAIL_OTHER;
    }
    if (body != NULL && body[0] != '\0') {
        snprintf(pr->detail, sizeof(pr->detail), "%.79s", body);
    } else if (status > 0) {
        I18N_FORMAT(pr->detail, sizeof(pr->detail), UI_WHY_SERVICE_ANSWERED, status);
    } else {
        snprintf(pr->detail, sizeof(pr->detail), "%s", UI_WHY_NO_ANSWER);
    }
}

/* ---- building and sending one notification --------------------------------------------------- */

static void ntfy_cfg_for(notify_ntfy_cfg_t *out)
{
    out->base = s_ntfy_run.self_hosted && s_ntfy_run.server[0] != '\0' ? s_ntfy_run.server
                                                                      : NTFY_HOSTED;
    out->topic = s_ntfy_run.topic;
    out->token = s_ntfy_run.token[0] != '\0' ? s_ntfy_run.token : NULL;
}

static void push_cfg_for(notify_pushover_cfg_t *out)
{
    out->user_key = s_push_run.user_key;
    out->app_token = s_push_run.app_token;
    out->sound = s_push_run.sound[0] != '\0' ? s_push_run.sound : NULL;
    out->device = s_push_run.device[0] != '\0' ? s_push_run.device : NULL;
    out->emergency = s_push_run.emergency;
    out->retry_s = s_push_run.retry_s;
    out->expire_s = s_push_run.expire_s;
}

/* One notification, all the way out. true: the service took it. */
static bool send_words(uint8_t p, const notify_words_t *w, notify_prio_t prio, uint8_t partition)
{
    provider_t *pr = &s_prov[p];
    bool built;
    if (p == NET_NOTIFY_NTFY) {
        notify_ntfy_cfg_t c;
        ntfy_cfg_for(&c);
        built = notify_build_ntfy(&c, w, prio, s_req);
    } else {
        notify_pushover_cfg_t c;
        push_cfg_for(&c);
        char tag[24];
        snprintf(tag, sizeof(tag), "alarm-%u", (unsigned)partition);
        built = notify_build_pushover(&c, w, prio, partition != 0u ? tag : NULL, s_req);
    }
    if (!built) {
        snprintf(pr->detail, sizeof(pr->detail), "Not set up");
        pr->fail = NET_NOTIFY_FAIL_OTHER;
        return false;
    }
    pr->state = NET_NOTIFY_SENDING;
    s_resp[0] = '\0';
    const int status = post(s_req,
                            p == NET_NOTIFY_NTFY && s_ntfy_run.tls == NET_NOTIFY_TLS_OWN_CA,
                            s_resp, BODY_SCRATCH, p == NET_NOTIFY_PUSHOVER ? &pr->quota_left
                                                                           : NULL);
    memset(s_req, 0, sizeof(*s_req)); /* the body carried a credential */
    pr->last_send = alarm_service_now();
    if (status >= 200 && status < 300) {
        pr->sent++;
        pr->today++;
        pr->ever_sent = true;
        pr->last_ok = pr->last_send;
        pr->fail = NET_NOTIFY_FAIL_NONE;
        pr->detail[0] = '\0';
        pr->retry_n = 0u;
        pr->state = NET_NOTIFY_OK;
        return true;
    }
    classify(pr, status, s_resp);
    pr->state = NET_NOTIFY_FAILED;
    /* A 4xx will never succeed however often it is tried: Pushover says so, and ntfy means it
     * too. Treat it as delivered-as-far-as-we-can so it doesn't block everything behind it.
     * Anything else (no answer, a 429, a 5xx) is tried again (NOTE-014). */
    const bool permanent = notify_http_outcome(status) == NOTIFY_HTTP_REFUSED;
    ESP_LOGW(TAG, "%s refused (%d)%s", p == NET_NOTIFY_NTFY ? "ntfy" : "pushover", status,
             permanent ? "; not trying that one again" : "");
    return permanent;
}

/* PUSH-006: Pushover is the only one that can say whether a key belongs to an account.
 * A 200 with "status":1 means yes; anything else is quoted back to the owner. */
static bool validate_user_key(char *why, size_t why_len)
{
    if (s_push_run.user_key[0] == '\0' || s_push_run.app_token[0] == '\0') {
        return true; /* nothing to check yet */
    }
    memset(s_req, 0, sizeof(*s_req));
    snprintf(s_req->url, sizeof(s_req->url),
             "https://api.pushover.net/1/users/validate.json");
    snprintf(s_req->content_type, sizeof(s_req->content_type),
             "application/x-www-form-urlencoded");
    s_req->body_len = (size_t)snprintf(s_req->body, sizeof(s_req->body),
                                       "token=%s&user=%s", s_push_run.app_token,
                                       s_push_run.user_key);
    s_resp[0] = '\0';
    const int status = post(s_req, false, s_resp, BODY_SCRATCH, NULL);
    memset(s_req, 0, sizeof(*s_req)); /* it carried both credentials */
    if (status >= 200 && status < 300 && strstr(s_resp, "\"status\":1") != NULL) {
        return true;
    }
    if (why != NULL) {
        if (status == 400 && strstr(s_resp, "user") != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Pushover doesn't know that user key"));
        } else if (status == 400) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Pushover refused the application token"));
        } else if (status < 0) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Couldn't reach Pushover to check it"));
        } else {
            I18N_FORMAT(why, why_len, UI_WHY_PUSHOVER_ANSWERED, status);
        }
    }
    return false;
}

/* PUSH-005: the alarm is dealt with, so the phones must stop repeating. Returns the partitions
 * whose cancel is still owed: no answer, a 429 or a 5xx is tried again with the backoff, as any
 * notification is (NOTE-014, code review S08); a 4xx refusal is final, nothing would change by
 * repeating it. */
static uint8_t cancel_repeats(uint8_t partitions)
{
    if (!s_push_run.enabled || !s_push_run.emergency || s_push_run.app_token[0] == '\0') {
        return 0u;
    }
    uint8_t owed = 0u;
    for (unsigned i = 0; i < 8u; i++) {
        if ((partitions & (1u << i)) == 0u) {
            continue;
        }
        notify_pushover_cfg_t c;
        push_cfg_for(&c);
        char tag[24];
        snprintf(tag, sizeof(tag), "alarm-%u", i + 1u);
        if (notify_build_pushover_cancel(&c, tag, s_req)) {
            const int status = post(s_req, false, NULL, 0u, NULL);
            memset(s_req, 0, sizeof(*s_req));
            const notify_http_outcome_t out = notify_http_outcome(status);
            if (out == NOTIFY_HTTP_TAKEN) {
                ESP_LOGI(TAG, "cancelled the repeat for partition %u (%d)", i + 1u, status);
            } else if (out == NOTIFY_HTTP_RETRY) {
                ESP_LOGW(TAG, "cancelling the repeat for partition %u failed (%d); trying again",
                         i + 1u, status);
                owed = (uint8_t)(owed | (1u << i));
            } else {
                ESP_LOGW(TAG, "Pushover refused to cancel the repeat for partition %u (%d)",
                         i + 1u, status);
            }
        }
    }
    return owed;
}

/* ---- the event log as the queue -------------------------------------------------------------- */

static void refresh_config(void)
{
    alarm_service_get_snapshot(&s_snap);
    if ((!s_have_cfg || s_snap.config_seq != s_cfg_seq) &&
        alarm_service_get_config(s_cfg) == ESP_OK) {
        s_have_cfg = true;
        s_cfg_seq = s_snap.config_seq;
    }
}

static uint8_t cats_of(uint8_t p)
{
    return p == NET_NOTIFY_NTFY ? s_ntfy_run.cats : s_push_run.cats;
}

static bool silent_of(uint8_t p)
{
    return p == NET_NOTIFY_NTFY ? s_ntfy_run.silent_too : s_push_run.silent_too;
}

/* PUSH-005: an alarm sent after its partition's disarm, cancel or silence (its request failed,
 * or the network was down) is not sent as a repeating emergency the cancel went before. */
static notify_prio_t prio_now(notify_prio_t prio, uint32_t seq, uint8_t partition)
{
    const uint8_t part = partition >= 1u && partition <= 8u ? partition : 1u;
    return notify_priority_dealt(prio, seq, __atomic_load_n(&s_dealt_seq[part - 1u],
                                                            __ATOMIC_ACQUIRE));
}

/* The transport behind notify_gate_deliver(): true when the service took it or refused it for
 * good, false when it is worth trying again. */
static bool offer_send(void *ctx, const alarm_event_t *ev)
{
    const uint8_t p = *(const uint8_t *)ctx;
    refresh_config();
    notify_words_t w;
    if (!notify_words(NULL /* the panel's language: G3 (BL-004) */, s_have_cfg ? s_cfg : NULL, ev,
                      true, &w)) {
        return true; /* nothing to say: nothing a retry would change */
    }
    return send_words(p, &w, prio_now(notify_priority(ev), ev->seq, ev->partition),
                      ev->partition);
}

/* Offer one event to one service: the gate decides, and only a SEND costs a request. A send
 * that failed is not recorded by the gate, so the retry is a real request again rather than a
 * duplicate of itself (code review S02, NOTE-014). false: try again later. */
static bool offer(uint8_t p, const alarm_event_t *ev, alarm_ms_t now)
{
    provider_t *pr = &s_prov[p];
    notify_gate_set(&pr->gate, cats_of(p), silent_of(p));
    return notify_gate_deliver(&pr->gate, ev, now, offer_send, &p);
}

static int rp_publish_for(uint8_t p, const elog_entry_t *marker, bool replayed)
{
    provider_t *pr = &s_prov[p];
    (void)replayed;
    if (!offer(p, &marker->ev, alarm_service_now())) {
        return -1; /* try again on the next poll */
    }
    pr->msg_id = pr->msg_id < 0x7FFFFFF0 ? pr->msg_id + 1 : 1;
    return pr->msg_id;
}

static int rp_publish_ntfy(void *ctx, const elog_entry_t *m, bool replayed)
{
    (void)ctx;
    return rp_publish_for(NET_NOTIFY_NTFY, m, replayed);
}

static int rp_publish_push(void *ctx, const elog_entry_t *m, bool replayed)
{
    (void)ctx;
    return rp_publish_for(NET_NOTIFY_PUSHOVER, m, replayed);
}

static bool marker_filter(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_REPORTED;
}

static size_t rp_read_after(void *ctx, uint32_t after, elog_entry_t *out, size_t max,
                            uint32_t scan, uint32_t *next_after)
{
    (void)ctx;
    return panel_log_read_after(after, marker_filter, NULL, out, max, scan, next_after);
}

static uint32_t rp_newest(void *ctx)
{
    (void)ctx;
    return panel_log_newest_index();
}

/* Appends sep + text at `at` and returns the new length, never past cap - 1: a line that
 * doesn't fit is cut, and the caller sees the buffer is full. */
static size_t append_text(char *buf, size_t cap, size_t at, const char *sep, const char *text)
{
    if (at >= cap - 1u) {
        return cap - 1u;
    }
    const int n = snprintf(buf + at, cap - at, "%s%s", sep, text);
    if (n < 0) {
        return at;
    }
    return (size_t)n >= cap - at ? cap - 1u : at + (size_t)n;
}

/* NOTE-036: coming back after a long outage, the log is digested rather than replayed. Alarms
 * still go one by one, up to five; everything else becomes one line. A six-hour outage can cost
 * at most seven notifications, whatever happened in it. */
static bool catch_up(uint8_t p, alarm_ms_t now)
{
    provider_t *pr = &s_prov[p];
    const uint32_t newest = panel_log_newest_index();
    if (newest <= pr->rep.acked || newest - pr->rep.acked <= NOTIFY_DIGEST_MIN) {
        return true; /* near enough: the ordinary replay handles it */
    }
    bool delivered = true; /* Wi-Fi up is not the service reachable: nothing is written off
                            * as sent until the service took it, or the outage's alarms would
                            * be lost to a router that came up before its uplink */
    refresh_config();
    elog_entry_t batch[8];
    uint32_t after = pr->rep.acked;
    unsigned alarms = 0u;
    unsigned others = 0u;
    char first_lines[3][NOTIFY_TITLE_LEN];
    unsigned named = 0u;
    for (unsigned round = 0; round < 64u; round++) {
        uint32_t next = after;
        /* A read that found no marker has still moved on when it stopped at its scan limit in
         * a long run of other records: only a read that moves nowhere is the end. */
        const size_t n = panel_log_read_after(after, marker_filter, NULL, batch,
                                              sizeof(batch) / sizeof(batch[0]),
                                              PROTO_REPORT_SCAN, &next);
        for (size_t i = 0; i < n; i++) {
            const alarm_event_t *ev = &batch[i].ev;
            const notify_cat_t cat = notify_category(ev->type);
            /* an automation's notification goes past the categories (BL-224) */
            if (!notify_gate_wanted(cats_of(p), ev)) {
                continue;
            }
            if ((ev->flags & ALARM_EVF_SILENT) && !silent_of(p)) {
                continue;
            }
            if (cat == NOTIFY_CAT_ALARM && alarms < NOTIFY_DIGEST_ALARMS) {
                notify_words_t w;
                if (notify_words(NULL /* the panel's language: G3 (BL-004) */,
                                 s_have_cfg ? s_cfg : NULL, ev, true, &w)) {
                    delivered = send_words(p, &w,
                                           prio_now(notify_priority(ev), ev->seq, ev->partition),
                                           ev->partition) &&
                                delivered;
                    alarms++;
                }
                continue;
            }
            if (named < 3u) {
                notify_words_t w;
                if (notify_words(NULL /* the panel's language: G3 (BL-004) */,
                                 s_have_cfg ? s_cfg : NULL, ev, true, &w)) {
                    snprintf(first_lines[named++], NOTIFY_TITLE_LEN, "%s", w.title);
                }
            }
            others++;
        }
        if (next == after) {
            break;
        }
        after = next;
    }
    if (others > 0u) {
        notify_words_t d;
        memset(&d, 0, sizeof(d));
        d.cat = NOTIFY_CAT_SYSTEM;
        snprintf(d.title, sizeof(d.title), "%u things happened while the panel was offline",
                 others);
        size_t at = 0u;
        for (unsigned i = 0; i < named; i++) {
            /* snprintf() returns what it wanted to write, not what fitted: clamp, or the
             * next write starts past the end with a size that wrapped (three 30-character
             * lines were enough). */
            at = append_text(d.body, sizeof(d.body), at, at > 0u ? " · " : "", first_lines[i]);
            if (at >= sizeof(d.body) - 1u) {
                break;
            }
        }
        if (others > named && at < sizeof(d.body) - 1u) {
            snprintf(d.body + at, sizeof(d.body) - at, "%sand %u more", at > 0u ? " · " : "",
                     others - named);
        }
        delivered = send_words(p, &d, NOTIFY_PRIO_NORMAL, 0u) && delivered;
    }
    if (!delivered) {
        ESP_LOGW(TAG, "%s: the outage digest did not get through; trying again shortly",
                 p == NET_NOTIFY_NTFY ? "ntfy" : "pushover");
        return false;
    }
    ESP_LOGI(TAG, "%s: digested %u events from the outage (%u alarms sent)",
             p == NET_NOTIFY_NTFY ? "ntfy" : "pushover", others, alarms);
    /* Everything read is accounted for; anything after it (the rounds ran out, or it happened
     * while this ran) goes out one by one through the ordinary replay, never written off. */
    proto_report_init(&pr->rep, &pr->port, after, true, now);
    proto_report_connected(&pr->rep, now);
    pr->acked_dirty = true;
    return true;
}

/* ---- the engine's listener (alarm task: queue only, never block) ----------------------------- */

static void on_report(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    (void)ev;
    wake(NB_REPORT);
}

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    /* An alarm that has been dealt with must stop repeating on the phones (PUSH-005). */
    if (ev->type == ALARM_EV_DISARMED || ev->type == ALARM_EV_ALARM_CANCELED ||
        ev->type == ALARM_EV_ALARM_SILENCED) {
        const uint8_t part = ev->partition >= 1u && ev->partition <= 8u ? ev->partition : 1u;
        /* Before the cancel is owed: an alarm of this partition still waiting to be sent is
         * from now on sent as over, never as an emergency the cancel already passed. */
        __atomic_store_n(&s_dealt_seq[part - 1u], ev->seq, __ATOMIC_RELEASE);
        __atomic_fetch_or(&s_cancel_partitions, (uint8_t)(1u << (part - 1u)), __ATOMIC_ACQ_REL);
        wake(NB_CANCEL);
    }
}

/* LOG-010: a reportable event the log couldn't keep. Take it for immediate delivery. */
static bool on_report_lost(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (s_live == NULL || (!s_ntfy.enabled && !s_push.enabled)) {
        return false;
    }
    if (xQueueSend(s_live, ev, 0) != pdTRUE) {
        return false;
    }
    wake(NB_REPORT);
    return true;
}

static const alarm_listener_t LISTENER = {
    .ctx = NULL,
    .on_event = on_event,
    .on_report = on_report,
};

const alarm_listener_t *net_notify_listener(void)
{
    return &LISTENER;
}

/* ---- the task -------------------------------------------------------------------------------- */

static bool enabled_of(uint8_t p)
{
    if (p == NET_NOTIFY_NTFY) {
        return s_ntfy_run.enabled && s_ntfy_run.topic[0] != '\0';
    }
    return s_push_run.enabled && s_push_run.user_key[0] != '\0' &&
           s_push_run.app_token[0] != '\0';
}

static bool online(void)
{
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    return ws.state == NET_WIFI_CONNECTED;
}

/* NOTE-014: a growing, jittered gap. *n counts the failures in a row. */
static alarm_ms_t backoff(uint8_t *n, alarm_ms_t now)
{
    return notify_backoff(n, now, esp_random());
}

static void retry_later(provider_t *pr, alarm_ms_t now)
{
    pr->retry_at = backoff(&pr->retry_n, now);
}

/* One service's turn with the live queue's head (LOG-010): only once its gap and its backoff
 * are over; a failed request backs it off. true: it is finished with the event. */
static bool live_try(void *ctx, uint8_t p)
{
    const alarm_event_t *ev = ctx;
    provider_t *pr = &s_prov[p];
    const alarm_ms_t now = alarm_service_now();
    if (!notify_may_send(now, pr->retry_at, pr->last_send)) {
        return false;
    }
    if (offer(p, ev, now)) {
        return true;
    }
    retry_later(pr, alarm_service_now());
    return false;
}

/* Takes a copy of what the UI last stored. The only place the task touches s_lock, and it
 * holds it for two structure copies. */
static void sync_settings(void)
{
    lock();
    s_ntfy_run = s_ntfy;
    s_push_run = s_push;
    unlock();
}

/* What the UI reads, published once a pass so a status poll never waits for a post. */
static void publish_status(alarm_ms_t now, bool net)
{
    net_notify_status_t snap[NET_NOTIFY_PROVIDERS];
    for (uint8_t p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
        const provider_t *pr = &s_prov[p];
        net_notify_status_t *o = &snap[p];
        memset(o, 0, sizeof(*o));
        o->state = enabled_of(p) ? pr->state : NET_NOTIFY_OFF;
        if (o->state != NET_NOTIFY_OFF && !net && o->state != NET_NOTIFY_FAILED) {
            o->state = NET_NOTIFY_NO_NETWORK;
        }
        o->fail = pr->fail;
        o->sent = pr->sent;
        o->today = pr->gate.day_count;
        o->limit = NOTIFY_DAY_HARD;
        o->quota_left = pr->quota_left;
        o->ever_sent = pr->ever_sent;
        o->waiting = proto_report_waiting(&pr->rep);
        o->ago_s = pr->ever_sent && now > pr->last_ok
                       ? (uint32_t)((now - pr->last_ok) / 1000u)
                       : 0u;
        snprintf(o->detail, sizeof(o->detail), "%s", pr->detail);
    }
    lock();
    memcpy(s_status, snap, sizeof(s_status));
    unlock();
}

static void apply_settings(alarm_ms_t now)
{
    sync_settings();
    for (uint8_t p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
        provider_t *pr = &s_prov[p];
        const bool on = enabled_of(p);
        proto_report_enable(&pr->rep, on, now);
        notify_gate_set(&pr->gate, cats_of(p), silent_of(p));
        if (!on) {
            pr->state = NET_NOTIFY_OFF;
            pr->fail = NET_NOTIFY_FAIL_NONE;
            pr->detail[0] = '\0';
        } else if (pr->state == NET_NOTIFY_OFF) {
            pr->state = pr->ever_sent ? NET_NOTIFY_OK : NET_NOTIFY_READY;
        }
    }
    net_wifi_set_required(NET_WIFI_REQ_NOTIFY, s_ntfy_run.enabled || s_push_run.enabled);
}

static void run_test(alarm_ms_t now)
{
    const uint8_t p = s_test_provider;
    if (p >= NET_NOTIFY_PROVIDERS) {
        return;
    }
    s_test_why[0] = '\0';
    bool ok = false;
    if (!enabled_of(p)) {
        snprintf(s_test_why, sizeof(s_test_why), "%s", I18N_NOOP_C("why", "Switch it on and fill it in first"));
    } else if (!online()) {
        snprintf(s_test_why, sizeof(s_test_why), "%s", I18N_NOOP_C("why", "The panel isn't on the network"));
    } else {
        notify_words_t w;
        memset(&w, 0, sizeof(w));
        w.cat = NOTIFY_CAT_SYSTEM;
        snprintf(w.title, sizeof(w.title), "Test from your alarm panel");
        snprintf(w.body, sizeof(w.body), "If you can read this, alerts are working.");
        /* A test is the owner asking, so it goes past the rate limits (NOTE-023). */
        ok = send_words(p, &w, NOTIFY_PRIO_NORMAL, 0u);
        if (!ok) {
            snprintf(s_test_why, sizeof(s_test_why), "%.79s",
                     s_prov[p].detail[0] ? s_prov[p].detail : I18N_NOOP_C("why", "It didn't go"));
        }
    }
    (void)now;
    s_test_provider = 0xFFu;
    lock();
    s_test_ok = ok;
    s_test_done = true; /* set last, and under the lock the reader takes */
    unlock();
}

static void notify_task(void *arg)
{
    (void)arg;
    alarm_ms_t last_acked_save = 0;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
        const alarm_ms_t now = alarm_service_now();

        if (bits & NB_SETTINGS) {
            apply_settings(now);
        }
        const bool net = online();

        if (bits & NB_TEST) {
            run_test(now);
        }
        if (s_validate_pending) {
            s_validate_pending = false;
            s_test_why[0] = '\0';
            const bool ok = net ? validate_user_key(s_test_why, sizeof(s_test_why)) : true;
            lock();
            s_test_ok = ok; /* offline: nothing to check it against, the send will say */
            s_test_done = true;
            unlock();
        }
        /* The cancel is owed until the network has taken it, not only on the pass its
         * notification bit arrived: the owner who disarms while Wi-Fi is down must still stop
         * the phones ringing once it is back. */
        if (bits & NB_CANCEL) {
            s_cancel_retry_at = 0u; /* a new disarm or silence goes now, not after a backoff */
        }
        if (net && now >= s_cancel_retry_at &&
            __atomic_load_n(&s_cancel_partitions, __ATOMIC_ACQUIRE) != 0u) {
            const uint8_t parts = __atomic_exchange_n(&s_cancel_partitions, 0u, __ATOMIC_ACQ_REL);
            const uint8_t owed = cancel_repeats(parts);
            if (owed != 0u) {
                __atomic_fetch_or(&s_cancel_partitions, owed, __ATOMIC_ACQ_REL);
                s_cancel_retry_at = backoff(&s_cancel_retry_n, now); /* S08 */
            } else {
                s_cancel_retry_n = 0u;
            }
        }

        /* A report the log could not keep goes out live or not at all (LOG-010) - but "not
         * at all" means a provider refused it, not that the network happened to be down on
         * this pass: it stays queued until someone has taken it or nobody is there to. */
        /* Each service switched on gets its own delivery or its own refusal (S02): one taking
         * it doesn't write it off for another whose request failed, and a failed one waits for
         * its backoff (NOTE-014), not the next pass. */
        alarm_event_t live;
        while (net && s_live != NULL && xQueuePeek(s_live, &live, 0) == pdTRUE) {
            uint8_t wanted = 0u;
            for (uint8_t p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
                if (enabled_of(p)) {
                    wanted = (uint8_t)(wanted | (1u << p));
                }
            }
            if (!notify_fan_out(&s_live_done, wanted, live_try, &live)) {
                break; /* somebody still owes it: try again on a later pass */
            }
            xQueueReceive(s_live, &live, 0);
        }

        for (uint8_t p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
            provider_t *pr = &s_prov[p];
            if (!enabled_of(p)) {
                continue;
            }
            if (!net) {
                if (pr->state != NET_NOTIFY_FAILED) {
                    pr->state = NET_NOTIFY_NO_NETWORK;
                }
                proto_report_disconnected(&pr->rep, now);
                continue;
            }
            if (pr->rep.connected == false) {
                if (now < pr->digest_retry_at) {
                    continue;
                }
                if (!catch_up(p, now)) {
                    pr->digest_retry_at = now + DIGEST_RETRY_MS;
                    continue;
                }
                proto_report_connected(&pr->rep, now);
                if (pr->state == NET_NOTIFY_NO_NETWORK) {
                    pr->state = pr->ever_sent ? NET_NOTIFY_OK : NET_NOTIFY_READY;
                }
            }
            if (notify_gate_paused(&pr->gate)) {
                pr->state = NET_NOTIFY_PAUSED;
            }
            if (!notify_may_send(now, pr->retry_at, pr->last_send)) {
                continue; /* NOTE-014 backoff, NOTE-035 gap (a live send this pass counts) */
            }
            /* One poll per wake: publish() posts over HTTPS and blocks while it does. */
            const uint32_t before = pr->rep.acked;
            if (proto_report_poll(&pr->rep, now)) {
                if (pr->rep.inflight != 0u) {
                    proto_report_acked(&pr->rep, pr->rep.msg_id, now);
                }
                wake(NB_REPORT); /* there may be more */
            } else if (pr->state == NET_NOTIFY_FAILED) {
                retry_later(pr, now);
            }
            if (pr->rep.acked != before) {
                pr->acked_dirty = true;
            }

            /* A follow-up stays owed until its request reached its end (S02): one that failed
             * is returned again on a later pass, after the backoff. */
            notify_rollup_t roll;
            if (notify_gate_follow_up(&pr->gate, now, &roll)) {
                refresh_config();
                notify_words_t w;
                memset(&w, 0, sizeof(w));
                w.cat = NOTIFY_CAT_ALARM;
                char names[3][ALARM_NAME_LEN + 8];
                size_t at = 0u;
                for (unsigned i = 0; i < roll.n; i++) {
                    notify_zone_name(NULL /* the panel's language: G3 (BL-004) */,
                                     s_have_cfg ? s_cfg : NULL, roll.zones[i], names[i],
                                     sizeof(names[i]));
                    at = append_text(w.body, sizeof(w.body), at, at > 0u ? ", " : "", names[i]);
                    if (at >= sizeof(w.body) - 1u) {
                        break;
                    }
                }
                if (roll.total > roll.n && at < sizeof(w.body) - 1u) {
                    char listed[sizeof(w.body)];
                    snprintf(listed, sizeof(listed), "%s", w.body);
                    const unsigned more = (unsigned)(roll.total - roll.n);
                    /* TR: The end of a push notification body that lists zones by name; {0} is the
                     * list, {1} how many more zones are not named. At most 95 bytes. */
                    (void)I18N_FORMAT(w.body, sizeof(w.body),
                                      i18n_plural(NULL /* the panel's language: G3 (BL-004) */,
                                                  NULL, "{0} and {1} more", "{0} and {1} more",
                                                  more),
                                      listed, more);
                }
                /* TR: The title of a push notification for an alarm that hit several zones; {0} is
                 * the alarm's title ("Burglary alarm"), {1} the number of zones. At most 63 bytes. */
                (void)I18N_FORMAT(w.title, sizeof(w.title),
                                  i18n_plural(NULL /* the panel's language: G3 (BL-004) */, NULL,
                                              "{0} \xC2\xB7 {1} zone", "{0} \xC2\xB7 {1} zones",
                                              (uint32_t)roll.total),
                                  notify_cause_title(NULL /* the panel's language: G3 (BL-004) */,
                                                     (alarm_cause_t)roll.cause),
                                  (unsigned)roll.total);
                if (send_words(p, &w, prio_now(NOTIFY_PRIO_URGENT, roll.seq, roll.partition),
                               roll.partition)) {
                    notify_gate_follow_up_sent(&pr->gate, roll.partition, now);
                } else if (now >= pr->retry_at) {
                    retry_later(pr, now); /* unless this pass's poll already backed off */
                }
            }

            if (pr->acked_dirty && now - last_acked_save >= ACKED_SAVE_MS) {
                save_acked(p, pr->rep.acked);
                pr->acked_dirty = false;
                last_acked_save = now;
            }
        }
        publish_status(now, net);
    }
}

/* ---- public API ------------------------------------------------------------------------------ */

void net_notify_ntfy_get(net_notify_ntfy_t *out)
{
    memset(out, 0, sizeof(*out));
    lock();
    out->enabled = s_ntfy.enabled;
    out->self_hosted = s_ntfy.self_hosted;
    snprintf(out->server, sizeof(out->server), "%s", s_ntfy.server);
    snprintf(out->topic, sizeof(out->topic), "%s", s_ntfy.topic);
    out->token_set = s_ntfy.token[0] != '\0';
    out->ca_loaded = s_ca != NULL && s_ca[0] != '\0';
    out->cats = s_ntfy.cats;
    out->silent_too = s_ntfy.silent_too;
    unlock();
}

esp_err_t net_notify_ntfy_set(const net_notify_ntfy_t *in, bool change_token, char *why,
                              size_t why_len)
{
    if (in->enabled && in->topic[0] == '\0') {
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Set a topic first"));
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (in->enabled && in->self_hosted && in->server[0] == '\0') {
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Set your server's address first"));
        }
        return ESP_ERR_INVALID_ARG;
    }
    /* S03: the running settings change only once the new ones are stored, so a failed save
     * neither reports "saved" nor runs on settings a restart would forget. */
    lock();
    ntfy_cfg_t next = s_ntfy;
    next.enabled = in->enabled;
    next.self_hosted = in->self_hosted;
    snprintf(next.server, sizeof(next.server), "%s", in->server);
    snprintf(next.topic, sizeof(next.topic), "%s", in->topic);
    next.cats = in->cats;
    next.silent_too = in->silent_too;
    if (change_token) {
        snprintf(next.token, sizeof(next.token), "%s", in->token);
    }
    const esp_err_t err = store_ntfy(&next);
    if (err == ESP_OK) {
        s_ntfy = next;
    } else {
        (void)store_ntfy(&s_ntfy); /* put back what is running, as far as the storage lets us */
    }
    unlock();
    memset(&next, 0, sizeof(next)); /* it carried the token */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ntfy settings not saved: %s", esp_err_to_name(err));
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Couldn't save the settings"));
        }
        return err;
    }
    wake(NB_SETTINGS);
    return ESP_OK;
}

/* What the UI stored (s_ntfy, s_push), under the lock: a service is set up when it is on and has
 * what it needs to send (the task's enabled_of, on the stored settings). */
uint8_t net_notify_services(void)
{
    uint8_t mask = 0u;
    lock();
    if (s_ntfy.enabled && s_ntfy.topic[0] != '\0') {
        mask |= (uint8_t)(1u << NET_NOTIFY_NTFY);
    }
    if (s_push.enabled && s_push.user_key[0] != '\0' && s_push.app_token[0] != '\0') {
        mask |= (uint8_t)(1u << NET_NOTIFY_PUSHOVER);
    }
    unlock();
    return mask;
}

bool net_notify_any_service(void)
{
    return net_notify_services() != 0u;
}

void net_notify_pushover_get(net_notify_pushover_t *out)
{
    memset(out, 0, sizeof(*out));
    lock();
    out->enabled = s_push.enabled;
    out->user_key_set = s_push.user_key[0] != '\0';
    out->app_token_set = s_push.app_token[0] != '\0';
    out->cats = s_push.cats;
    out->silent_too = s_push.silent_too;
    out->emergency = s_push.emergency;
    snprintf(out->sound, sizeof(out->sound), "%s", s_push.sound);
    snprintf(out->device, sizeof(out->device), "%s", s_push.device);
    unlock();
}

/* A user key or an application token is 30 characters of letters and digits. */
static bool key_shaped(const char *s)
{
    size_t n = 0u;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return n == 30u;
}

esp_err_t net_notify_pushover_set(const net_notify_pushover_t *in, bool change_user,
                                  bool change_token, char *why, size_t why_len)
{
    if (change_user && in->user_key[0] != '\0' && !key_shaped(in->user_key)) {
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "A user key is 30 letters and digits"));
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (change_token && in->app_token[0] != '\0' && !key_shaped(in->app_token)) {
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "An application token is 30 letters and digits"));
        }
        return ESP_ERR_INVALID_ARG;
    }
    const bool have_user = change_user ? in->user_key[0] != '\0' : s_push.user_key[0] != '\0';
    const bool have_app = change_token ? in->app_token[0] != '\0' : s_push.app_token[0] != '\0';
    if (in->enabled && (!have_user || !have_app)) {
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Both the user key and the application token are needed"));
        }
        return ESP_ERR_INVALID_ARG;
    }
    /* S03: as for ntfy, the running settings change only once the new ones are stored. */
    lock();
    push_cfg_t next = s_push;
    next.enabled = in->enabled;
    next.cats = in->cats;
    next.silent_too = in->silent_too;
    next.emergency = in->emergency;
    snprintf(next.sound, sizeof(next.sound), "%s", in->sound);
    snprintf(next.device, sizeof(next.device), "%s", in->device);
    if (change_user) {
        snprintf(next.user_key, sizeof(next.user_key), "%s", in->user_key);
    }
    if (change_token) {
        snprintf(next.app_token, sizeof(next.app_token), "%s", in->app_token);
    }
    const esp_err_t err = store_push(&next);
    if (err == ESP_OK) {
        s_push = next;
    } else {
        (void)store_push(&s_push); /* put back what is running, as far as the storage lets us */
    }
    unlock();
    memset(&next, 0, sizeof(next)); /* it carried both credentials */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Pushover settings not saved: %s", esp_err_to_name(err));
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Couldn't save the settings"));
        }
        return err;
    }
    if (change_user || change_token) {
        s_validate_pending = true; /* PUSH-006 */
    }
    wake(NB_SETTINGS);
    return ESP_OK;
}

/* Reads the snapshot the task publishes, so the LVGL thread never waits behind a post. */
void net_notify_get_status(uint8_t provider, net_notify_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (provider >= NET_NOTIFY_PROVIDERS) {
        return;
    }
    lock();
    *out = s_status[provider];
    unlock();
}

esp_err_t net_notify_test_start(uint8_t provider, char *why, size_t why_len)
{
    if (provider >= NET_NOTIFY_PROVIDERS || s_task == NULL) {
        if (why != NULL) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Notifications aren't running"));
        }
        return ESP_ERR_INVALID_ARG;
    }
    s_test_done = false;
    s_test_ok = false;
    s_test_provider = provider;
    wake(NB_TEST);
    return ESP_OK;
}

bool net_notify_test_result(bool *ok, char *why, size_t why_len)
{
    lock();
    const bool done = s_test_done;
    if (done) {
        if (ok != NULL) {
            *ok = s_test_ok;
        }
        if (why != NULL && why_len > 0u) {
            snprintf(why, why_len, "%s", s_test_why);
        }
        s_test_done = false; /* taken */
    }
    unlock();
    return done;
}

/* S03: the running copy changes only once the change is stored; an NVS entry is replaced
 * whole, so a failure leaves the old certificate both stored and in use. */
esp_err_t net_notify_set_ca(const char *pem)
{
    const bool clear = pem == NULL || pem[0] == '\0';
    if (!clear &&
        (strstr(pem, "-----BEGIN CERTIFICATE-----") == NULL || strlen(pem) >= CA_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_NTFY, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (clear) {
        err = nvs_erase_key(h, "ca");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK; /* none was stored */
        }
    } else {
        err = nvs_set_str(h, "ca", pem);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ntfy certificate not saved: %s", esp_err_to_name(err));
        return err;
    }
    lock();
    if (s_ca != NULL) {
        if (clear) {
            s_ca[0] = '\0';
        } else {
            snprintf(s_ca, CA_MAX, "%s", pem);
        }
    }
    unlock();
    wake(NB_SETTINGS);
    return ESP_OK;
}

/* ---- start ----------------------------------------------------------------------------------- */

static void start_failed(void)
{
    panel_log_remove_lost_handler(on_report_lost, NULL);
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    if (s_live != NULL) {
        vQueueDelete(s_live);
        s_live = NULL;
    }
    free(s_cfg);
    s_cfg = NULL;
    free(s_req);
    s_req = NULL;
    free(s_resp);
    s_resp = NULL;
    free(s_ca);
    s_ca = NULL;
}

esp_err_t net_notify_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_live = xQueueCreate(LIVE_QUEUE, sizeof(alarm_event_t));
    s_cfg = alarm_mem_slow_malloc(sizeof(alarm_config_t));
    s_req = alarm_mem_slow_malloc(sizeof(notify_request_t));
    s_resp = alarm_mem_slow_malloc(BODY_SCRATCH);
    s_ca = alarm_mem_slow_calloc(1, CA_MAX);
    if (s_lock == NULL || s_live == NULL || s_cfg == NULL || s_req == NULL || s_resp == NULL ||
        s_ca == NULL) {
        start_failed();
        return ESP_ERR_NO_MEM;
    }
    load_settings();
    nvs_handle_t h;
    if (nvs_open(NS_NTFY, NVS_READONLY, &h) == ESP_OK) {
        size_t n = CA_MAX;
        if (nvs_get_str(h, "ca", s_ca, &n) != ESP_OK) {
            s_ca[0] = '\0';
        }
        nvs_close(h);
    }

    s_ntfy_run = s_ntfy;
    s_push_run = s_push;
    const alarm_ms_t now = alarm_service_now();
    for (uint8_t p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
        provider_t *pr = &s_prov[p];
        pr->port.ctx = NULL;
        pr->port.read_after = rp_read_after;
        pr->port.newest_index = rp_newest;
        pr->port.publish = p == NET_NOTIFY_NTFY ? rp_publish_ntfy : rp_publish_push;
        pr->quota_left = -1;
        proto_report_init(&pr->rep, &pr->port, load_acked(p), enabled_of(p), now);
        notify_gate_init(&pr->gate, cats_of(p), silent_of(p), now);
        pr->state = enabled_of(p) ? NET_NOTIFY_READY : NET_NOTIFY_OFF;
    }

    if (panel_log_add_lost_handler(on_report_lost, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "no lost-report slot left: a dropped report won't reach a phone");
    }
    if (xTaskCreate(notify_task, "notify", TASK_STACK, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
        start_failed();
        return ESP_ERR_NO_MEM;
    }
    net_wifi_set_required(NET_WIFI_REQ_NOTIFY, s_ntfy.enabled || s_push.enabled);
    ESP_LOGI(TAG, "ntfy %s, pushover %s", s_ntfy.enabled ? "on" : "off",
             s_push.enabled ? "on" : "off");
    return ESP_OK;
}

/* ---- service console -------------------------------------------------------------------------*/

static const char *state_word(net_notify_state_t s)
{
    switch (s) {
    case NET_NOTIFY_NO_NETWORK: return "waiting for Wi-Fi";
    case NET_NOTIFY_READY: return "ready";
    case NET_NOTIFY_SENDING: return "sending";
    case NET_NOTIFY_OK: return "working";
    case NET_NOTIFY_FAILED: return "failed";
    case NET_NOTIFY_PAUSED: return "paused (too many today)";
    default: return "off";
    }
}

static int cmd_notify(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        const uint8_t p = (argc >= 3 && strcmp(argv[2], "pushover") == 0) ? NET_NOTIFY_PUSHOVER
                                                                          : NET_NOTIFY_NTFY;
        char why[80] = "";
        if (net_notify_test_start(p, why, sizeof(why)) != ESP_OK) {
            printf("%s\n", why);
            return 0;
        }
        bool ok = false;
        for (unsigned waited = 0; waited < TEST_WAIT_MS; waited += 100u) {
            if (net_notify_test_result(&ok, why, sizeof(why))) {
                printf("%s\n", ok ? "sent - check your phone" : why);
                return 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        printf("still trying\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "topic") == 0) {
        char why[80] = "";
        net_notify_ntfy_t s;
        net_notify_ntfy_get(&s);
        if (strcmp(argv[2], "new") == 0) {
            net_notify_new_topic(s.topic, sizeof(s.topic));
        } else {
            snprintf(s.topic, sizeof(s.topic), "%s", argv[2]);
        }
        if (net_notify_ntfy_set(&s, false, why, sizeof(why)) != ESP_OK) {
            printf("%s\n", why);
            return 0;
        }
        printf("topic %s\n", s.topic);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "ntfy") == 0 &&
        (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        char why[80] = "";
        net_notify_ntfy_t s;
        net_notify_ntfy_get(&s);
        s.enabled = strcmp(argv[2], "on") == 0;
        if (net_notify_ntfy_set(&s, false, why, sizeof(why)) != ESP_OK) {
            printf("%s\n", why);
            return 0;
        }
        printf("ntfy %s\n", s.enabled ? "on" : "off");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "pushover") == 0 &&
        (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        char why[80] = "";
        net_notify_pushover_t s;
        net_notify_pushover_get(&s);
        s.enabled = strcmp(argv[2], "on") == 0;
        if (net_notify_pushover_set(&s, false, false, why, sizeof(why)) != ESP_OK) {
            printf("%s\n", why);
            return 0;
        }
        printf("pushover %s\n", s.enabled ? "on" : "off");
        return 0;
    }
    /* Which kinds of event, as the five bits the pages set (NOTE-037). */
    if (argc >= 4 && strcmp(argv[1], "cats") == 0) {
        const uint8_t mask = (uint8_t)strtoul(argv[3], NULL, 0);
        char why[80] = "";
        if (strcmp(argv[2], "pushover") == 0) {
            net_notify_pushover_t s;
            net_notify_pushover_get(&s);
            s.cats = mask;
            (void)net_notify_pushover_set(&s, false, false, why, sizeof(why));
        } else {
            net_notify_ntfy_t s;
            net_notify_ntfy_get(&s);
            s.cats = mask;
            (void)net_notify_ntfy_set(&s, false, why, sizeof(why));
        }
        printf("%s cats 0x%02x%s%s\n", argv[2], mask, why[0] ? " - " : "", why);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        printf("notify                      what each service is doing\n"
               "notify ntfy on|off          switch ntfy on or off\n"
               "notify pushover on|off      switch Pushover on or off\n"
               "notify topic new|<name>     roll or set the ntfy topic\n"
               "notify cats <svc> <mask>    which kinds of event (bit 0 alarm, 1 trouble,\n"
               "                            2 arming, 3 zone activity, 4 system)\n"
               "notify test [ntfy|pushover] send one now, past the rate limits\n"
               "Tokens and the Pushover credentials are typed on the screen, not here.\n");
        return 0;
    }
    for (uint8_t p = 0; p < NET_NOTIFY_PROVIDERS; p++) {
        net_notify_status_t st;
        net_notify_get_status(p, &st);
        printf("%-9s %-24s sent %" PRIu32 ", today %" PRIu32 "%s%s\n",
               p == NET_NOTIFY_NTFY ? "ntfy" : "pushover", state_word(st.state), st.sent,
               st.today, st.detail[0] ? ", " : "", st.detail);
    }
    /* Never the token, never the key: only whether one is stored (NOTE-022). */
    lock();
    printf("ntfy     %s, topic %.3s***, token %s, ca %s\n",
           s_ntfy.self_hosted ? (s_ntfy.server[0] ? s_ntfy.server : "own server, no address")
                              : NTFY_HOSTED,
           s_ntfy.topic, s_ntfy.token[0] ? "set" : "none",
           (s_ca != NULL && s_ca[0]) ? "loaded" : "none");
    printf("pushover user %s, app token %s, repeat alarms %s, sound %s\n",
           s_push.user_key[0] ? "set" : "none", s_push.app_token[0] ? "set" : "none",
           s_push.emergency ? "on" : "off", s_push.sound[0] ? s_push.sound : "default");
    unlock();
    printf("stack high-water %u bytes\n",
           s_task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(s_task) : 0u);
    return 0;
}

esp_err_t net_notify_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "notify",
        .help = "Phone notifications: status, send a test, roll the ntfy topic",
        .hint = "[ntfy|pushover on|off] [topic new|<name>] [cats <svc> <mask>] [test] [help]",
        .func = cmd_notify,
    };
    return esp_console_cmd_register(&cmd);
}

#else /* !CONFIG_ALARM_FEATURE_NOTIFY: no phone services in this firmware */

#include "net_notify.h"

uint8_t net_notify_services(void)
{
    return 0u;
}

bool net_notify_any_service(void)
{
    return false;
}

#endif
