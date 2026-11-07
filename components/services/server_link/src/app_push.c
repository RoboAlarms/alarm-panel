#include "sdkconfig.h"
#if CONFIG_ALARM_FEATURE_SERVER
/*
 * app_push.c - the panel's own notifications to paired phones through push.roboalarms.com (alarm-server's
 * direct-mode.md and push-api.md; BL-225). Each message is encrypted for one phone, so the relay can't
 * read it. The event log is the queue and the words and limits are notify's, as for ntfy and Pushover
 * (NOTE-002, NOTE-012, NOTE-014, NOTE-030..033). With no phone asking for push the task only sleeps.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "alarm_mem.h"
#include "alarm_proto/proto_phone.h"
#include "alarm_proto/proto_report.h"
#include "alarm_proto/proto_server.h"
#include "alarm_service.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "notify/notify_gate.h"
#include "notify/notify_http.h"
#include "notify/notify_words.h"
#include "nvs.h"
#include "panel_log.h"
#include "psa/crypto.h"
#include "server_link.h"
#include "server_link_internal.h"
#include "sodium.h"

static const char *TAG = "apppush";

#define TASK_STACK   12288u
#define BODY_MAX     (PROTO_PHONE_MAX * 4600u)
#define PLAIN_MAX    1024u
#define CT_MAX       (PLAIN_MAX + 12u + 16u)
#define ANSWER_MAX   2048u
#define RETRY_MS     5000u
#define RETRY_MAX_MS 300000u
#define SAVE_MS      5000u
#define POLLS        8

static TaskHandle_t s_task;
static volatile bool s_want_sender; /* a phone asked who this panel is to the relay */
static proto_phone_t s_phones[PROTO_PHONE_MAX] ALARM_SLOW_BSS;
static size_t s_phone_n;
static uint32_t s_phones_rev;
static notify_gate_t s_gate ALARM_SLOW_BSS;
static proto_report_t s_rep;
static alarm_config_t *s_cfg;
static uint32_t s_cfg_seq;
static bool s_have_cfg;
static alarm_snapshot_t s_snap ALARM_SLOW_BSS;
static char *s_body;
static char *s_answer;
static char s_sender[48];
static int64_t s_clock_fix_s; /* the relay's time minus ours, after a clock-skew answer */
static alarm_ms_t s_retry_at;
static uint32_t s_retry_ms = RETRY_MS;
static int s_msg_id;
static volatile bool s_acked_pending;
static uint32_t s_sent;

/* ---- the relay ------------------------------------------------------------------------------- */

typedef struct {
    size_t len;
    int64_t relay_date_s;
} answer_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    answer_t *a = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && a->len + (size_t)e->data_len < ANSWER_MAX) {
        memcpy(s_answer + a->len, e->data, (size_t)e->data_len);
        a->len += (size_t)e->data_len;
        s_answer[a->len] = '\0';
    }
    return ESP_OK;
}

/* A signed request (RFC 9421). Returns the HTTP status, < 0 when nothing came back. */
static int relay_post(const char *path, const char *body, size_t len, const char *keyid)
{
    char relay[PROTO_SRV_HOST_LEN + 16];
    bool own_ca = false;
    server_link_relay_base(relay, sizeof(relay), &own_ca);
    char uri[sizeof(relay) + 32];
    snprintf(uri, sizeof(uri), "%s%s", relay, path);

    char digest[64];
    char params[192];
    char sig_input[200];
    char sig_hdr[120];
    static char base[512];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (!proto_push_digest(body, len, digest, sizeof(digest)) ||
        !proto_push_sig_params((int64_t)tv.tv_sec + s_clock_fix_s, keyid, params, sizeof(params))) {
        return -1;
    }
    const size_t bn = proto_push_sig_base("POST", uri, digest, params, base, sizeof(base));
    uint8_t sig[64];
    char sig_b64[96];
    if (bn == 0u || !server_link_sign(base, bn, sig) ||
        proto_push_b64(sig, sizeof(sig), sig_b64, sizeof(sig_b64)) == 0u) {
        return -1;
    }
    snprintf(sig_input, sizeof(sig_input), "sig1=%s", params);
    snprintf(sig_hdr, sizeof(sig_hdr), "sig1=:%s:", sig_b64);

    answer_t a = {0};
    s_answer[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = uri,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = on_http,
        .user_data = &a,
        .disable_auto_redirect = true,
    };
    if (own_ca) {
        cfg.cert_pem = server_link_ca();
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Content-Digest", digest);
    esp_http_client_set_header(c, "Signature-Input", sig_input);
    esp_http_client_set_header(c, "Signature", sig_hdr);
    esp_http_client_set_post_field(c, body, (int)len);
    const esp_err_t err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    /* PUSH-009: a wrong clock is told the relay's time. Ours is corrected by the server link or NTP;
     * until then the Date header's offset is good enough for "created". */
    if (status == 401 && strstr(s_answer, "clock-skew") != NULL) {
        ESP_LOGW(TAG, "the relay says our clock is off");
    }
    return status;
}

static bool json_string(const char *json, const char *key, char *out, size_t cap)
{
    pj_reader_t r;
    pj_read_init(&r, json, strlen(json));
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char k[16];
    bool ok = false;
    while (pj_next_member(&r, k, sizeof(k))) {
        if (strcmp(k, key) == 0 && pj_get_str(&r, out, cap)) {
            ok = true;
        }
    }
    return ok;
}

/* The first request: our public key is the key id, and the relay answers our sender id. */
static bool ensure_sender(void)
{
    if (s_sender[0] != '\0') {
        return true;
    }
    nvs_handle_t h;
    if (nvs_open(SERVER_LINK_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_sender);
        if (nvs_get_str(h, "psender", s_sender, &len) != ESP_OK) {
            s_sender[0] = '\0';
        }
        nvs_close(h);
    }
    if (s_sender[0] != '\0') {
        return true;
    }
    uint8_t pub[32];
    char kid[PROTO_SRV_B64_KEY_LEN];
    server_link_public_key(pub);
    (void)proto_srv_b64url(pub, sizeof(pub), kid, sizeof(kid));
    char body[128];
    const int n = snprintf(body, sizeof(body), "{\"kind\":\"panel\",\"software\":\"%s\"}", "RoboAlarms panel");
    const int status = relay_post("/v1/senders", body, (size_t)n, kid);
    if ((status != 200 && status != 201) || !json_string(s_answer, "sender", s_sender, sizeof(s_sender))) {
        ESP_LOGW(TAG, "couldn't register with the relay (%d)", status);
        s_sender[0] = '\0';
        return false;
    }
    if (nvs_open(SERVER_LINK_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "psender", s_sender);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "registered with the relay as %s", s_sender);
    return true;
}

/* ---- one message ------------------------------------------------------------------------------ */

/* nonce || AES-256-GCM(key, plain, aad) (push-api.md "The encrypted message"). */
static size_t seal(const uint8_t key[32], const char *aad, size_t aad_len, const char *plain,
                   size_t plain_len, uint8_t *out, size_t cap)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    mbedtls_svc_key_id_t id;
    if (psa_import_key(&attr, key, 32u, &id) != PSA_SUCCESS || cap < 12u) {
        return 0u;
    }
    esp_fill_random(out, 12u);
    size_t n = 0;
    const psa_status_t st = psa_aead_encrypt(id, PSA_ALG_GCM, out, 12u, (const uint8_t *)aad, aad_len,
                                             (const uint8_t *)plain, plain_len, out + 12u, cap - 12u, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? n + 12u : 0u;
}

static proto_phone_level_t level_of(const alarm_event_t *ev, notify_prio_t prio)
{
    if (ev->type == ALARM_EV_ALARM) {
        return PROTO_PUSH_ALARM;
    }
    if (ev->type == ALARM_EV_DISARMED && prio >= NOTIFY_PRIO_HIGH) {
        return PROTO_PUSH_ALARM_OVER;
    }
    return prio >= NOTIFY_PRIO_HIGH ? PROTO_PUSH_TROUBLE : PROTO_PUSH_ACTIVITY;
}

/* true: the relay took it or refused it for good. */
static bool send_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    static notify_words_t w ALARM_SLOW_BSS;
    if (!s_have_cfg || !notify_words(NULL, s_cfg, ev, true, &w)) {
        return true;
    }
    if (!ensure_sender()) {
        return false;
    }
    const proto_phone_level_t level = level_of(ev, notify_priority(ev));
    const char *level_word = proto_push_level_name(level);
    char panel[PROTO_SRV_ID_LEN];
    server_link_panel_id(panel);
    char ref[24];
    char at[24];
    char id[48];
    char collapse[48];
    snprintf(ref, sizeof(ref), "evt_%" PRIu32, ev->seq);
    (void)proto_iso8601(ev->epoch_ms, at);
    snprintf(id, sizeof(id), "%s-%" PRIu32, panel, ev->seq);
    snprintf(collapse, sizeof(collapse), "%s-%u", panel, (unsigned)ev->partition);

    static char plain[PLAIN_MAX] ALARM_SLOW_BSS; /* the push task's */
    const proto_push_plain_t p = {.title = w.title,
                                  .body = w.body,
                                  .loc = panel,
                                  .loc_name = NULL, /* API-012: nothing that names the home */
                                  .kind = level == PROTO_PUSH_ALARM ? "alarm" : "event",
                                  .ref = ref,
                                  .at = at,
                                  .category = level == PROTO_PUSH_ALARM ? "alarm" : "none",
                                  .thread = panel,
                                  .sound = (ev->flags & ALARM_EVF_SILENT) != 0u ? "none"
                                           : level == PROTO_PUSH_ALARM          ? "alarm"
                                                                                : "none"};
    pj_writer_t pw;
    pj_init(&pw, plain, sizeof(plain));
    proto_push_plain(&pw, &p);
    const size_t plain_len = pj_finish(&pw);
    if (plain_len == 0u) {
        return true;
    }
    pj_writer_t b;
    pj_init(&b, s_body, BODY_MAX);
    pj_obj(&b);
    pj_kstr(&b, "id", id);
    pj_kstr(&b, "level", level_word);
    pj_kstr(&b, "collapse", collapse);
    pj_karr(&b, "to");
    size_t recipients = 0;
    static uint8_t ct[CT_MAX] ALARM_SLOW_BSS;
    static char ct_b64[CT_MAX * 2u] ALARM_SLOW_BSS;
    for (size_t i = 0; i < s_phone_n; i++) {
        const proto_phone_t *ph = &s_phones[i];
        if (ph->handle[0] == '\0' || (ph->levels & (1u << level)) == 0u) {
            continue;
        }
        /* Only what this phone's user may see: their own partitions and the system. */
        const alarm_user_t *u =
            ph->user >= 1u && ph->user <= ALARM_MAX_USERS ? &s_cfg->users[ph->user - 1u] : NULL;
        if (u == NULL || !proto_phone_hears(u->authority, u->partitions, ev->partition)) {
            continue;
        }
        char aad[PROTO_PHONE_HANDLE_LEN + 32];
        const size_t al = proto_push_aad(ph->handle, level_word, aad, sizeof(aad));
        const size_t cn = al != 0u ? seal(ph->push_key, aad, al, plain, plain_len, ct, sizeof(ct)) : 0u;
        if (cn == 0u || proto_push_b64(ct, cn, ct_b64, sizeof(ct_b64)) == 0u) {
            continue;
        }
        pj_obj(&b);
        pj_kstr(&b, "handle", ph->handle);
        pj_kstr(&b, "kid", ph->kid);
        pj_kstr(&b, "ct", ct_b64);
        pj_obj_end(&b);
        recipients++;
    }
    pj_arr_end(&b);
    pj_obj_end(&b);
    const size_t len = pj_finish(&b);
    if (recipients == 0u) {
        return true; /* nobody wants this level */
    }
    if (len == 0u) {
        return true; /* can't happen within the sizes above: never block the queue on it */
    }
    const int status = relay_post("/v1/send", s_body, len, s_sender);
    if (status == 200) {
        s_sent++;
        ESP_LOGI(TAG, "seq %" PRIu32 " (%s) to %u phone(s)", ev->seq, level_word, (unsigned)recipients);
        /* Per-phone results: a "gone" handle is removed from the phone's record. */
        for (size_t i = 0; i < s_phone_n; i++) {
            proto_phone_t *ph = &s_phones[i];
            char want[PROTO_PHONE_HANDLE_LEN + 32];
            snprintf(want, sizeof(want), "\"handle\":\"%s\",\"status\":\"gone\"", ph->handle);
            if (ph->handle[0] != '\0' && strstr(s_answer, want) != NULL) {
                char pid[PROTO_PHONE_ID_LEN];
                proto_phone_id(ph->pub, pid);
                (void)server_link_phone_set_push(pid, "", "", NULL, 0u);
            }
        }
        return true;
    }
    ESP_LOGW(TAG, "the relay answered %d", status);
    return status >= 400 && status < 500 && status != 429 && status != 401;
}

/* ---- the queue -------------------------------------------------------------------------------- */

static bool marker_filter(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_REPORTED;
}

static size_t rp_read_after(void *ctx, uint32_t after, elog_entry_t *out, size_t max, uint32_t scan,
                            uint32_t *next)
{
    (void)ctx;
    return panel_log_read_after(after, marker_filter, NULL, out, max, scan, next);
}

static uint32_t rp_newest(void *ctx)
{
    (void)ctx;
    return panel_log_newest_index();
}

static int rp_publish(void *ctx, const elog_entry_t *m, bool replayed)
{
    (void)ctx;
    (void)replayed;
    if (!notify_gate_deliver(&s_gate, &m->ev, alarm_service_now(), send_event, NULL)) {
        return -1;
    }
    s_msg_id = s_msg_id < 0x7FFFFFF0 ? s_msg_id + 1 : 1;
    s_acked_pending = true;
    return s_msg_id;
}

static const proto_report_port_t PORT = {NULL, rp_read_after, rp_newest, rp_publish};

static void load_position(uint32_t *acked)
{
    nvs_handle_t h;
    bool have = false;
    if (nvs_open(SERVER_LINK_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_u32(h, "packed", acked) == ESP_OK;
        nvs_close(h);
    }
    if (!have) {
        *acked = panel_log_newest_index(); /* NOTE-011: the history stays in the panel */
    }
}

static void save_position(uint32_t acked)
{
    nvs_handle_t h;
    if (nvs_open(SERVER_LINK_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "packed", acked);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void push_task(void *arg)
{
    (void)arg;
    uint32_t acked;
    load_position(&acked);
    const alarm_ms_t start = alarm_service_now();
    notify_gate_init(&s_gate, 0xFFu, true, start);
    proto_report_init(&s_rep, &PORT, acked, false, start);
    s_phones_rev = UINT32_MAX;
    alarm_ms_t saved_at = 0;
    bool up = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        const alarm_ms_t now = alarm_service_now();
        if (server_link_phone_revision() != s_phones_rev) {
            s_phones_rev = server_link_phone_revision();
            s_phone_n = server_link_phone_list(s_phones, PROTO_PHONE_MAX);
        }
        bool wanted = false;
        for (size_t i = 0; i < s_phone_n; i++) {
            wanted = wanted || s_phones[i].handle[0] != '\0';
        }
        net_wifi_status_t ws;
        net_wifi_get_status(&ws);
        if (s_want_sender && ws.state == NET_WIFI_CONNECTED && ensure_sender()) {
            s_want_sender = false;
        }
        const bool go = wanted && ws.state == NET_WIFI_CONNECTED;
        if (!wanted) {
            /* Nobody to tell: what happens now isn't owed to a phone paired later (NOTE-011). */
            s_rep.acked = panel_log_newest_index();
            up = false;
            continue;
        }
        if (go != up) {
            up = go;
            if (go) {
                proto_report_connected(&s_rep, now);
            } else {
                proto_report_disconnected(&s_rep, now);
            }
        }
        if (!go || now < s_retry_at) {
            continue;
        }
        alarm_service_get_snapshot(&s_snap);
        if ((!s_have_cfg || s_snap.config_seq != s_cfg_seq) &&
            alarm_service_get_config(s_cfg) == ESP_OK) {
            s_have_cfg = true;
            s_cfg_seq = s_snap.config_seq;
        }
        for (int i = 0; i < POLLS; i++) {
            const bool did = proto_report_poll(&s_rep, now);
            if (s_acked_pending) {
                /* The relay's answer is the acknowledgement. */
                s_acked_pending = false;
                proto_report_acked(&s_rep, s_rep.msg_id, now);
                s_retry_ms = RETRY_MS;
            } else if (s_rep.waiting && !did) {
                s_retry_at = now + s_retry_ms - s_retry_ms / 5u + esp_random() % (2u * s_retry_ms / 5u + 1u);
                s_retry_ms = s_retry_ms >= RETRY_MAX_MS / 2u ? RETRY_MAX_MS : s_retry_ms * 2u;
                break;
            }
            if (!did) {
                break;
            }
        }
        if (s_rep.acked_dirty && now - saved_at >= SAVE_MS) {
            save_position(s_rep.acked);
            s_rep.acked_dirty = false;
            saved_at = now;
        }
    }
}

bool server_link_push_sender(char *out, size_t cap)
{
    if (s_sender[0] != '\0') {
        snprintf(out, cap, "%s", s_sender);
        return true;
    }
    s_want_sender = true; /* the link's task starts us on its next look */
    return false;
}

void app_push_review(void)
{
    static uint32_t seen = UINT32_MAX;
    if (s_task != NULL || (server_link_phone_revision() == seen && !s_want_sender)) {
        return;
    }
    seen = server_link_phone_revision();
    s_phone_n = server_link_phone_list(s_phones, PROTO_PHONE_MAX);
    bool wanted = s_want_sender;
    for (size_t i = 0; i < s_phone_n; i++) {
        wanted = wanted || s_phones[i].handle[0] != '\0';
    }
    if (!wanted) {
        return;
    }
    if (s_cfg == NULL) {
        s_cfg = alarm_mem_slow_malloc(sizeof(alarm_config_t));
    }
    if (s_body == NULL) {
        s_body = alarm_mem_slow_malloc(BODY_MAX);
    }
    if (s_answer == NULL) {
        s_answer = alarm_mem_slow_malloc(ANSWER_MAX);
    }
    if (s_cfg == NULL || s_body == NULL || s_answer == NULL ||
        xTaskCreatePinnedToCore(push_task, "apppush", TASK_STACK, NULL, 3, &s_task, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the push service: no alerts through the relay");
        seen = UINT32_MAX; /* tried again on the next look */
        s_task = NULL;
    }
}

#endif
