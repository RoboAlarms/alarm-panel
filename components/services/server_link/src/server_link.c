#include "sdkconfig.h"
#if CONFIG_ALARM_FEATURE_SERVER
/*
 * server_link.c - the panel's link to an alarm-server (see server_link.h).
 *
 * One task ("server") owns the client and does all publishing: it wakes on engine changes, messages
 * from the server and once a second. The esp-mqtt event handler only copies what arrived. A sign-in
 * is good for one connect (the counter only goes up), so every try makes a new client with a new
 * password instead of using esp-mqtt's own reconnect.
 */
#include "server_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>

#include "alarm/alarm_strings.h"
#include "alarm_mem.h"
#include "alarm_proto/proto_link.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net_wifi.h"
#include "notify/notify_words.h"
#include "nvs.h"
#include "panel_clock.h"
#include "panel_log.h"
#include "server_link_internal.h"
#include "sodium.h"

static const char *TAG = "server";

#define NVS_NS          "srvlink"
#define CA_MAX          4096u
#define TASK_STACK      8192u /* internal RAM: NVS writes, the signature */
#define TASK_PRIORITY   4
#define IN_MAX          4096u
#define MQTT_BUF        (IN_MAX + 64u) /* over the PSRAM threshold: internal RAM is the reserve (B001) */
#define BUF_MAX         PROTO_LINK_SNAPSHOT_MAX
#define IN_QUEUE_LEN    8u
#define DELTA_MS        250u      /* at most 4 a second (API-007) */
#define HEALTH_MS       60000u
#define HELLO_MS        3600000u  /* the clock is measured again every hour */
#define RETRY_MIN_MS    1000u
#define RETRY_MAX_MS    300000u
#define CONNECT_MAX_MS  30000u
#define SAVE_MS         5000u
#define POLLS_PER_WAKE  16
#define LINK_GRACE_MS   600000u   /* the first people_link after Allow needs no step-up */
#define KEEPALIVE_S     60
#define REQ_STACK       16384u    /* the Panel Manager's handlers: the HTTP worker's size */

#define NB_SNAPSHOT (1u << 0)
#define NB_EVENT    (1u << 1)
#define NB_IN       (1u << 2)
#define NB_UP       (1u << 3)
#define NB_DOWN     (1u << 4)
#define NB_ORDER    (1u << 5) /* an API call left something for the task (below) */

typedef enum {
    SUB_NONE = 0,
    SUB_START,     /* connect and ask for a code */
    SUB_WAIT_CODE,
    SUB_CODE,
    SUB_CLAIM,
    SUB_ALLOWED,   /* Allow was pressed: waiting for the task to send it */
    SUB_DENIED,
    SUB_CANCEL,
} link_sub_t;

typedef struct {
    char topic[56];
    char payload[IN_MAX];
    size_t len;
    bool retained;
} in_msg_t;

/* ---- shared with the API (s_lock) ------------------------------------------------------------ */
static SemaphoreHandle_t s_lock;
static char s_host[PROTO_SRV_HOST_LEN] = SERVER_LINK_DEFAULT_HOST;
static uint16_t s_port;
static char *s_ca;
static bool s_linked;
static char s_location[48];
static uint8_t s_link_user;      /* whose code opened Connect */
static proto_srv_ceiling_t s_ceiling;
static link_sub_t s_sub;
static proto_srv_link_code_t s_code;
static proto_srv_link_claim_t s_claim;
static alarm_ms_t s_code_until;
static alarm_ms_t s_allowed_at;
static bool s_unlink_notice;     /* tell the server, then drop the connection */
static bool s_settings_changed;
static bool s_ceiling_changed;
static TaskHandle_t s_task;
static QueueHandle_t s_in;
static const char *s_model = "RoboAlarms panel";
static char s_relay[PROTO_SRV_HOST_LEN + 16] = "https://push.roboalarms.com";
static server_link_cert_fn s_cert_fn;
static uint8_t s_pk[crypto_sign_PUBLICKEYBYTES];
static uint8_t s_sk[crypto_sign_SECRETKEYBYTES];
static uint8_t s_seal_pk[crypto_box_PUBLICKEYBYTES];
static char s_id[PROTO_SRV_ID_LEN];

/* ---- volatile status ------------------------------------------------------------------------- */
static volatile bool s_connected;
static volatile server_link_fail_t s_fail;
static volatile uint32_t s_sent;
static volatile bool s_waiting;
static volatile int64_t s_offset_ms;
static volatile uint64_t s_heard_ms;

/* ---- the task's own state -------------------------------------------------------------------- */
static esp_mqtt_client_handle_t s_client;
static char s_password[PROTO_SRV_PASSWORD_LEN];
static char s_client_id[32];
static char s_uri[PROTO_SRV_HOST_LEN + 24];
static char s_will_topic[56];
static char s_run_ca_pem[CA_MAX + 1u] EXT_RAM_BSS_ATTR;
static alarm_ms_t s_try_at;
static alarm_ms_t s_tried_at;
static uint32_t s_retry_ms = RETRY_MIN_MS;
static bool s_old_software;
static uint64_t s_counter;
static char *s_buf;
static alarm_config_t *s_cfg;
static uint32_t s_cfg_seq;
static bool s_have_cfg;
static alarm_snapshot_t s_snap ALARM_SLOW_BSS;
static alarm_snapshot_t s_prev ALARM_SLOW_BSS;
static bool s_synced;
static in_msg_t s_in_msg ALARM_SLOW_BSS;
static proto_srv_in_t s_parsed ALARM_SLOW_BSS;
static proto_srv_seen_t s_seen ALARM_SLOW_BSS;
/* Who the recent commands and requests were for, by the events they caused (panel-link.md "event":
 * actor). Under s_lock: the request task notes, the server task reads. */
static proto_srv_actors_t s_actors ALARM_SLOW_BSS;
static proto_srvq_t s_q;
static bool s_comm_fail;
static bool s_clock_by_server;
static uint8_t s_sync_steps; /* after down/time: what is still owed */
static volatile alarm_ms_t s_offline_until; /* the console's outage, for a replay test */
static server_link_req_fn s_req_fn;
static struct {
    volatile bool busy, done;
    char id[PROTO_SRV_CMD_ID_LEN];
    char method[8];
    char path[64];
    uint8_t as;
    bool fresh;
    uint16_t source;
    proto_srv_actor_t actor; /* whom the server named: the events it causes carry it */
    char *body;
    size_t body_len;
    int status;
    char *out;
    size_t out_len;
} s_req;

#define SYNC_SNAPSHOT (1u << 0)
#define SYNC_PEOPLE   (1u << 1)
#define SYNC_CEILING  (1u << 2)
#define SYNC_HEALTH   (1u << 3)

static void notify(uint32_t bits)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

/* ---- NVS ------------------------------------------------------------------------------------- */

static esp_err_t store_str(const char *key, const char *value)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t store_ceiling(const proto_srv_ceiling_t *c)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, "ceiling", c, sizeof(*c));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* The link record and the ceiling in one commit: linked or not, never in between (API-004). */
static esp_err_t store_link(bool linked, const char *location, uint8_t user,
                            const proto_srv_ceiling_t *c)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u8(h, "linked", linked);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "loc", location);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "user", user);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "ceiling", c, sizeof(*c));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_settings(void)
{
    proto_srv_ceiling_defaults(&s_ceiling);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_host);
    if (nvs_get_str(h, "host", s_host, &len) != ESP_OK) {
        snprintf(s_host, sizeof(s_host), "%s", SERVER_LINK_DEFAULT_HOST);
    }
    uint16_t port;
    if (nvs_get_u16(h, "port", &port) == ESP_OK) {
        s_port = port;
    }
    uint8_t v;
    s_linked = nvs_get_u8(h, "linked", &v) == ESP_OK && v != 0u;
    if (nvs_get_u8(h, "user", &v) == ESP_OK) {
        s_link_user = v;
    }
    len = sizeof(s_relay);
    if (nvs_get_str(h, "relay", s_relay, &len) != ESP_OK) {
        snprintf(s_relay, sizeof(s_relay), "https://push.roboalarms.com");
    }
    len = sizeof(s_location);
    if (nvs_get_str(h, "loc", s_location, &len) != ESP_OK) {
        s_location[0] = '\0';
    }
    len = sizeof(s_ceiling);
    if (nvs_get_blob(h, "ceiling", &s_ceiling, &len) != ESP_OK || len != sizeof(s_ceiling)) {
        proto_srv_ceiling_defaults(&s_ceiling);
    }
    len = 0;
    if (nvs_get_blob(h, "ca", NULL, &len) == ESP_OK && len > 0u && len <= CA_MAX) {
        s_ca = malloc(len + 1u);
        if (s_ca != NULL && nvs_get_blob(h, "ca", s_ca, &len) == ESP_OK) {
            s_ca[len] = '\0';
        } else {
            free(s_ca);
            s_ca = NULL;
        }
    }
    nvs_close(h);
}

/* RFC 8032 section 7.1, test 1, and a sealed box opened again. */
static esp_err_t crypto_check(void)
{
    static const uint8_t SEED[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                                     0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                                     0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    static const uint8_t SIG_START[8] = {0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72};
    uint8_t pk[crypto_sign_PUBLICKEYBYTES];
    uint8_t sk[crypto_sign_SECRETKEYBYTES];
    uint8_t sig[crypto_sign_BYTES];
    if (crypto_sign_seed_keypair(pk, sk, SEED) != 0 ||
        crypto_sign_detached(sig, NULL, (const uint8_t *)"", 0u, sk) != 0 ||
        memcmp(sig, SIG_START, sizeof(SIG_START)) != 0 ||
        crypto_sign_verify_detached(sig, (const uint8_t *)"", 0u, pk) != 0) {
        return ESP_FAIL;
    }
    uint8_t box_pk[crypto_box_PUBLICKEYBYTES];
    uint8_t box_sk[crypto_box_SECRETKEYBYTES];
    uint8_t ct[crypto_box_SEALBYTES + 4u];
    uint8_t pt[4];
    if (crypto_sign_ed25519_pk_to_curve25519(box_pk, pk) != 0 ||
        crypto_sign_ed25519_sk_to_curve25519(box_sk, sk) != 0 ||
        crypto_box_seal(ct, (const uint8_t *)"1234", 4u, box_pk) != 0 ||
        crypto_box_seal_open(pt, ct, sizeof(ct), box_pk, box_sk) != 0 ||
        memcmp(pt, "1234", 4u) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* The panel's key: made once, never exported, never in a backup (backup_policy.c doesn't list this
 * namespace), gone with a factory reset. */
static esp_err_t load_or_make_key(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    size_t len = sizeof(s_sk);
    esp_err_t err = nvs_get_blob(h, "sk", s_sk, &len);
    if (err == ESP_OK && len == sizeof(s_sk)) {
        crypto_sign_ed25519_sk_to_pk(s_pk, s_sk);
    } else {
        uint8_t seed[crypto_sign_SEEDBYTES];
        esp_fill_random(seed, sizeof(seed));
        crypto_sign_seed_keypair(s_pk, s_sk, seed);
        sodium_memzero(seed, sizeof(seed));
        err = nvs_set_blob(h, "sk", s_sk, sizeof(s_sk));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    uint64_t c = 0;
    if (nvs_get_u64(h, "counter", &c) == ESP_OK) {
        s_counter = c;
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    if (crypto_sign_ed25519_pk_to_curve25519(s_seal_pk, s_pk) != 0) {
        return ESP_FAIL;
    }
    proto_srv_id(s_pk, s_id);
    return ESP_OK;
}

static void load_position(uint32_t *acked, uint32_t *acked_seq)
{
    nvs_handle_t h;
    *acked = 0;
    *acked_seq = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, "acked", acked);
        (void)nvs_get_u32(h, "ackseq", acked_seq);
        nvs_close(h);
    }
}

static void save_position(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "acked", s_q.acked);
        nvs_set_u32(h, "ackseq", s_q.acked_seq);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- publishing ------------------------------------------------------------------------------ */

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1735689600) { /* before 2025: the clock isn't set */
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint8_t clock_now(void)
{
    panel_clock_status_t st;
    panel_clock_get(&st);
    switch (st.source) {
    case CLOCK_SRC_MODULE:
        return PROTO_SRV_CLOCK_RTC;
    case CLOCK_SRC_INTERNET:
        return PROTO_SRV_CLOCK_NTP;
    case CLOCK_SRC_MANUAL:
        return s_clock_by_server ? PROTO_SRV_CLOCK_SERVER : PROTO_SRV_CLOCK_MANUAL;
    case CLOCK_SRC_NONE:
    default:
        /* A firmware without the clock module has time from the network or none. */
        return wall_ms() > 0 ? PROTO_SRV_CLOCK_NTP : PROTO_SRV_CLOCK_NONE;
    }
}

static bool publish(const char *suffix, const char *data, size_t len, bool retain)
{
    char topic[56];
    if (s_client == NULL || !s_connected || !proto_srv_topic(s_id, suffix, topic, sizeof(topic))) {
        return false;
    }
    return esp_mqtt_client_publish(s_client, topic, data, (int)len, 1, retain ? 1 : 0) >= 0;
}

static bool publish_w(const char *suffix, pj_writer_t *w)
{
    const size_t n = pj_finish(w);
    return n > 0u && publish(suffix, s_buf, n, false);
}

static void refresh_config(void)
{
    alarm_service_get_snapshot(&s_snap);
    if ((!s_have_cfg || s_snap.config_seq != s_cfg_seq) &&
        alarm_service_get_config(s_cfg) == ESP_OK) {
        s_have_cfg = true;
        s_cfg_seq = s_snap.config_seq;
    }
}

static void send_hello(void)
{
    static const char *const CAPS[] = {"walk_test", "sealed_code", "people_link"};
    char pub[PROTO_SRV_B64_KEY_LEN];
    char seal[PROTO_SRV_B64_KEY_LEN];
    char tz[48] = "";
    (void)proto_srv_b64url(s_pk, sizeof(s_pk), pub, sizeof(pub));
    (void)proto_srv_b64url(s_seal_pk, sizeof(s_seal_pk), seal, sizeof(seal));
    /* F3: the seal key, signed with the link key, so a server in the middle can't swap in its own
     * and read the codes the app seals to it. */
    char signed_bytes[16 + PROTO_SRV_B64_KEY_LEN];
    uint8_t sig[crypto_sign_BYTES];
    char seal_sig[2u * crypto_sign_BYTES];
    const size_t sn = proto_srv_seal_signed(seal, signed_bytes, sizeof(signed_bytes));
    if (sn == 0u || crypto_sign_detached(sig, NULL, (const uint8_t *)signed_bytes, sn, s_sk) != 0 ||
        !proto_srv_b64url(sig, sizeof(sig), seal_sig, sizeof(seal_sig))) {
        seal_sig[0] = '\0';
    }
    net_wifi_get_timezone(tz, sizeof(tz));
    elog_entry_t oldest;
    uint32_t next;
    const uint32_t oldest_seq =
        panel_log_read_after(0u, NULL, NULL, &oldest, 1u, 0u, &next) == 1u ? oldest.ev.seq : 0u;
    const proto_srv_hello_t h = {.id = s_id,
                                 .model = s_model,
                                 .fw = esp_app_get_description()->version,
                                 .caps = CAPS,
                                 .caps_n = sizeof(CAPS) / sizeof(CAPS[0]),
                                 .pub = pub,
                                 .seal_pub = seal,
                                 .seal_sig = seal_sig,
                                 .linked = s_linked,
                                 .boot = panel_log_boot(),
                                 .uptime_ms = uptime_ms(),
                                 .now_ms = wall_ms(),
                                 .clock = proto_srv_clock_name(clock_now()),
                                 .tz = tz,
                                 .last_seq = panel_log_last_seq(),
                                 .oldest_seq = oldest_seq,
                                 .path = "wifi"};
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_hello(&w, &h);
    publish_w("/up/hello", &w);
}

static const char *panel_tz(char *tz, size_t cap)
{
    tz[0] = '\0';
    net_wifi_get_timezone(tz, cap);
    return tz;
}

static void send_snapshot(void)
{
    char tz[48];
    const proto_when_t when = {.now = alarm_service_now(), .epoch_ms = wall_ms(), .stamp = true, .boot = panel_log_boot()};
    const proto_srv_panel_t panel = {.name = s_model,
                                     .tz = panel_tz(tz, sizeof(tz)),
                                     .region = "",
                                     .fw = esp_app_get_description()->version,
                                     .power = "mains",
                                     .battery_pct = -1,
                                     .setup_mode = s_snap.program};
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_snapshot(&w, s_cfg, &s_snap, &when, &panel);
    if (publish_w("/up/state", &w)) {
        s_prev = s_snap;
        s_synced = true;
    }
}

/* Only what changed, the same comparison as the Home Assistant link. */
static void send_delta(void)
{
    uint8_t pmask = 0;
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    bool any = false;
    bool troubles = s_snap.sys_troubles != s_prev.sys_troubles ||
                    s_snap.sys_troubles_restored != s_prev.sys_troubles_restored ||
                    s_snap.dev_troubles != s_prev.dev_troubles;
    for (uint8_t p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (s_snap.program != s_prev.program ||
            memcmp(&s_snap.part[p], &s_prev.part[p], sizeof(s_snap.part[p])) != 0) {
            pmask |= (uint8_t)(1u << p);
        }
    }
    for (uint16_t z = 0; z < ALARM_MAX_ZONES; z++) {
        if (memcmp(&s_snap.zone[z], &s_prev.zone[z], sizeof(s_snap.zone[z])) != 0) {
            zones[z / 8u] |= (uint8_t)(1u << (z % 8u));
            any = true;
            if (s_snap.zone[z].trouble != s_prev.zone[z].trouble) {
                troubles = true; /* the troubles object lists zones too */
            }
        }
    }
    if (pmask == 0u && !any && !troubles) {
        return;
    }
    const proto_when_t when = {.now = alarm_service_now(), .epoch_ms = wall_ms(), .stamp = true, .boot = panel_log_boot()};
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_link_delta(&w, s_cfg, &s_snap, &when, pmask, zones, troubles);
    if (publish_w("/up/state", &w)) {
        s_prev = s_snap;
    }
}

static void send_people(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const proto_srv_ceiling_t c = s_ceiling;
    xSemaphoreGive(s_lock);
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_people(&w, s_cfg, &c, NULL);
    publish_w("/up/state", &w);
}

static void send_ceiling(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const proto_srv_ceiling_t c = s_ceiling;
    xSemaphoreGive(s_lock);
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_ceiling(&w, &c);
    publish_w("/up/state", &w);
}

static void send_health(void)
{
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    const proto_srv_health_t h = {.power = "mains",
                                  .battery_pct = -1,
                                  .rssi = ws.rssi,
                                  .uptime_s = uptime_ms() / 1000u,
                                  .heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024u),
                                  .clock = proto_srv_clock_name(clock_now()),
                                  .path = "wifi"};
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_health(&w, &h);
    publish_w("/up/state", &w);
}

/* ---- events ---------------------------------------------------------------------------------- */

static bool event_filter(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_EVENT;
}

static size_t q_read_after(void *ctx, uint32_t after, elog_entry_t *out, size_t max, uint32_t scan,
                           uint32_t *next_after)
{
    (void)ctx;
    return panel_log_read_after(after, event_filter, NULL, out, max, scan, next_after);
}

static uint32_t q_newest(void *ctx)
{
    (void)ctx;
    return panel_log_newest_index();
}

static bool q_publish(void *ctx, const elog_entry_t *e, bool replayed)
{
    (void)ctx;
    if (!s_have_cfg) {
        return false; /* the zone and user names aren't loaded: on the next poll */
    }
    static notify_words_t words ALARM_SLOW_BSS;
    /* The server decides who is told what, so silent events carry their words too (API-015). */
    const bool have = notify_words(NULL, s_cfg, &e->ev, true, &words);
    proto_srv_actor_t actor;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const proto_srv_actor_t *a = proto_srv_actors_find(&s_actors, &e->ev);
    if (a != NULL) {
        actor = *a;
    }
    xSemaphoreGive(s_lock);
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_event(&w, s_cfg, e, replayed, have ? words.title : NULL, have ? words.body : NULL,
                    a != NULL ? &actor : NULL);
    if (!publish_w("/up/event", &w)) {
        return false;
    }
    s_sent++;
    return true;
}

static const proto_srvq_port_t QUEUE_PORT = {NULL, q_read_after, q_newest, q_publish};

/* ---- commands -------------------------------------------------------------------------------- */

static bool remote_eligible(uint8_t user)
{
    if (user < 1u || user > ALARM_MAX_USERS) {
        return false;
    }
    const alarm_authority_t a = s_cfg->users[user - 1u].authority;
    if (user == ALARM_USER_INSTALLER) {
        return a == ALARM_AUTH_INSTALLER;
    }
    /* web_login_eligible()'s rule: arm-only, guest and duress codes have no remote sign-in. */
    return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_PARTITION_MASTER || a == ALARM_AUTH_STANDARD;
}

static alarm_svc_when_t to_when(proto_srv_when_t w)
{
    switch (w) {
    case PROTO_SRV_WHEN_DISARMED:
        return ALARM_SVC_WHEN_DISARMED;
    case PROTO_SRV_WHEN_NOT_DISARMED:
        return ALARM_SVC_WHEN_NOT_DISARMED;
    case PROTO_SRV_WHEN_ANY:
    default:
        return ALARM_SVC_WHEN_ANY;
    }
}

/* F21: users deleted since the server task last looked (bit user - 1). A slot emptied and given to
 * someone else between two reviews looks the same in the configuration, so the deletion itself
 * takes the slot's phones. Set on the alarm task, which mustn't wait: atomic. */
static _Atomic uint64_t s_deleted_users;

/* Sealed codes opened lately, by a hash of the sealed bytes: one the server kept and sends again is
 * refused while its time would still pass (proto_srv_sealed_code()). Task-only. */
#define SEALED_SEEN 8u
#define PEOPLE_BODY_MAX 192u /* POST people with an opened code: name, authority, partitions, code */
static struct {
    uint8_t hash[16];
    alarm_ms_t until;
} s_sealed_seen[SEALED_SEEN];
static uint8_t s_sealed_next;

static bool sealed_again(const uint8_t *ct, size_t n, alarm_ms_t now)
{
    uint8_t h[16];
    crypto_generichash(h, sizeof(h), ct, n, NULL, 0);
    for (size_t i = 0; i < SEALED_SEEN; i++) {
        if (s_sealed_seen[i].until > now && sodium_memcmp(h, s_sealed_seen[i].hash, sizeof(h)) == 0) {
            return true;
        }
    }
    memcpy(s_sealed_seen[s_sealed_next].hash, h, sizeof(h));
    s_sealed_seen[s_sealed_next].until = now + 2u * PROTO_SRV_SEALED_MS;
    s_sealed_next = (uint8_t)((s_sealed_next + 1u) % SEALED_SEEN);
    return false;
}

/* Opens a code sealed to this panel's key (API-010): only here, and forgotten after. It must be for
 * this action and target (a command's partition, a person's slot), typed moments ago, and not seen
 * before. */
static bool open_sealed_for(const char *sealed, const char *action, unsigned target, char *code,
                            size_t cap)
{
    uint8_t ct[PROTO_SRV_SEALED_LEN];
    char plain[PROTO_SRV_SEALED_LEN];
    uint8_t curve_sk[crypto_box_SECRETKEYBYTES];
    const size_t n = proto_srv_unb64url(sealed, ct, sizeof(ct));
    if (n <= crypto_box_SEALBYTES || n - crypto_box_SEALBYTES >= sizeof(plain)) {
        return false;
    }
    bool ok = crypto_sign_ed25519_sk_to_curve25519(curve_sk, s_sk) == 0 &&
              crypto_box_seal_open((uint8_t *)plain, ct, n, s_seal_pk, curve_sk) == 0;
    sodium_memzero(curve_sk, sizeof(curve_sk));
    if (ok) {
        plain[n - crypto_box_SEALBYTES] = '\0';
        ok = proto_srv_sealed_for(plain, wall_ms(), action, target, code, cap) &&
             !sealed_again(ct, n, alarm_service_now());
    }
    sodium_memzero(plain, sizeof(plain));
    if (!ok) {
        sodium_memzero(code, cap);
    }
    return ok;
}

static bool open_sealed(const proto_srv_command_t *c, char *code, size_t cap)
{
    return open_sealed_for(c->sealed_code, c->action, c->partition, code, cap);
}

/* MOB-044: POST people with a person's code the app sealed to this panel. The code is opened here,
 * for that slot only, and the Panel Manager's request gets it as "code" (panel_web people_code()).
 * Any other POST people goes on as it came. 0, or the status to answer with why. */
static int people_unseal(char **body, size_t *len, const char **why)
{
    static proto_srv_people_t p; /* task-only: the request is read on the link task */
    proto_srv_people_parse(*body, *len, &p);
    if (p.kind == PROTO_SRV_PEOPLE_OTHER) {
        return 0;
    }
    int status = 0;
    char code[ALARM_CODE_MAX_LEN + 1] = "";
    char *out = NULL;
    if (p.kind == PROTO_SRV_PEOPLE_BAD) {
        status = 400;
        *why = "bad_request";
    } else if (!open_sealed_for(p.sealed_code, p.kind == PROTO_SRV_PEOPLE_CREATE ? "create" : "set_code",
                                p.kind == PROTO_SRV_PEOPLE_CREATE ? 0u : p.number, code, sizeof(code))) {
        status = 400;
        *why = "bad_code";
    } else if ((out = alarm_mem_slow_malloc(PEOPLE_BODY_MAX)) == NULL) {
        status = 503;
        *why = "busy";
    } else {
        pj_writer_t w;
        pj_init(&w, out, PEOPLE_BODY_MAX);
        pj_obj(&w);
        if (p.kind == PROTO_SRV_PEOPLE_CREATE) {
            pj_kstr(&w, "action", "create");
            pj_kstr(&w, "name", p.name);
            pj_kuint(&w, "authority", (uint64_t)p.authority);
            pj_kuint(&w, "partitions", p.partitions);
        } else {
            pj_kstr(&w, "action", "set_code");
            pj_kuint(&w, "number", p.number);
        }
        pj_kstr(&w, "code", code);
        pj_obj_end(&w);
        const size_t n = pj_finish(&w);
        if (n == 0u) {
            free(out);
            status = 400;
            *why = "bad_request";
        } else {
            sodium_memzero(*body, *len);
            free(*body);
            *body = out;
            *len = n;
        }
    }
    sodium_memzero(code, sizeof(code));
    sodium_memzero(&p, sizeof(p));
    return status;
}

static void answer_command(const proto_srv_result_t *r)
{
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_result(&w, r);
    const size_t n = pj_finish(&w);
    if (n == 0u) {
        return;
    }
    proto_srv_seen_add(&s_seen, r->id, s_buf, n, alarm_service_now());
    publish("/up/res", s_buf, n, false);
}

static void handle_command(const proto_srv_command_t *c)
{
    const alarm_ms_t now = alarm_service_now();
    const proto_srv_seen_entry_t *seen = proto_srv_seen_find(&s_seen, c->id, now);
    if (seen != NULL) {
        if (seen->len > 0u) {
            publish("/up/res", seen->res, seen->len, false); /* never twice (API-008) */
        }
        return;
    }
    proto_srv_result_t r = {.id = c->id, .action = c->action, .partition = c->partition};
    alarm_cmd_t cmd;
    proto_srv_when_t when;
    proto_srv_act_t act;
    if (!proto_srv_command_map(c, &cmd, &when, &act)) {
        r.result = alarm_result_name(ALARM_ERR_BAD_ARG);
        answer_command(&r);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const proto_srv_refusal_t refusal = proto_srv_check(&s_ceiling, c->as, act, wall_ms());
    const bool ask_code = s_ceiling.ask_code;
    xSemaphoreGive(s_lock);
    if (refusal != PROTO_SRV_OK || !remote_eligible(c->as)) {
        r.result = refusal != PROTO_SRV_OK ? proto_srv_refusal_name(refusal) : "not_allowed";
        ESP_LOGW(TAG, "%s as user %u refused: %s", c->action, (unsigned)c->as, r.result);
        answer_command(&r);
        return;
    }
    if (strcmp(c->action, "clear") == 0 && c->as == ALARM_USER_INSTALLER) {
        r.result = alarm_result_name(ALARM_ERR_NOT_AUTHORIZED); /* BL-187, as the Panel Manager */
        answer_command(&r);
        return;
    }
    const alarm_source_t src = {.kind = ALARM_SRC_REMOTE,
                                .id = strcmp(c->actor_via, "website") == 0
                                          ? ALARM_SRC_ID_SERVER_WEB
                                          : ALARM_SRC_ID_SERVER_APP};
    alarm_cmd_result_t res = {0};
    alarm_result_t e;
    alarm_service_get_snapshot(&s_snap);
    const uint32_t before_seq = s_snap.last_event_seq; /* the events after it are this command's */
    const proto_srv_run_t run = proto_srv_command_run(act, ask_code, c->sealed_code[0] != '\0');
    if (run == PROTO_SRV_RUN_CODE_REQUIRED) {
        e = ALARM_ERR_CODE_REQUIRED;
    } else if (run == PROTO_SRV_RUN_WITH_CODE) {
        /* The person's own code, so a duress code is a duress disarm with its silent report. */
        if (!open_sealed(c, cmd.code, sizeof(cmd.code))) {
            e = ALARM_ERR_INVALID_CODE;
        } else {
            e = alarm_service_command(&cmd, &src, &res);
        }
        sodium_memzero(cmd.code, sizeof(cmd.code));
    } else {
        e = alarm_service_command_as(&cmd, &src, c->as, to_when(when), &res);
    }
    ESP_LOGI(TAG, "user %u by %s: partition %u %s: %s", (unsigned)c->as, c->actor_via,
             (unsigned)c->partition, c->action, alarm_result_name(e));
    alarm_service_get_snapshot(&s_snap);
    proto_srv_actor_t actor = {0};
    snprintf(actor.account, sizeof(actor.account), "%s", c->actor_account);
    snprintf(actor.name, sizeof(actor.name), "%s", c->actor_name);
    snprintf(actor.via, sizeof(actor.via), "%s", c->actor_via);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    proto_srv_actors_note(&s_actors, before_seq, s_snap.last_event_seq, &actor);
    xSemaphoreGive(s_lock);
    char words[ALARM_NAME_LEN + 16] = "";
    if (e == ALARM_ERR_NOT_READY && res.not_ready_zone >= 1u &&
        res.not_ready_zone <= ALARM_MAX_ZONES) {
        snprintf(words, sizeof(words), "%s is open", s_cfg->zones[res.not_ready_zone - 1u].name);
    }
    r.result = alarm_result_name(e);
    r.user = res.user != 0u ? res.user : c->as;
    r.zone = res.not_ready_zone;
    r.words = words[0] != '\0' ? words : NULL;
    const alarm_partition_status_t *ps = &s_snap.part[c->partition - 1u];
    r.state = proto_ha_state(ps);
    r.level = ps->level != ALARM_LEVEL_NONE ? alarm_level_name(ps->level) : NULL;
    r.delay_left_ms = ps->delay_remaining_ms;
    answer_command(&r);
}

static void answer_res(const char *id, int status, const char *error)
{
    pj_writer_t w;
    pj_init(&w, s_buf, BUF_MAX);
    proto_srv_res(&w, id, status, error);
    publish_w("/up/res", &w);
}

/* Runs on its own task: the Panel Manager's handlers want the HTTP worker's stack, and that much
 * internal RAM is only worth holding while a request runs. One at a time. */
static void req_task(void *arg)
{
    (void)arg;
    char *out = NULL;
    size_t out_len = 0;
    static alarm_snapshot_t snap ALARM_SLOW_BSS; /* this task's: one request at a time */
    alarm_service_get_snapshot(&snap);
    const uint32_t before_seq = snap.last_event_seq; /* the events after it are this request's */
    const int status = s_req_fn(s_req.as, s_req.fresh, s_req.source, s_req.method, s_req.path,
                                s_req.body, s_req.body_len, &out, &out_len);
    alarm_service_get_snapshot(&snap);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    proto_srv_actors_note(&s_actors, before_seq, snap.last_event_seq, &s_req.actor);
    xSemaphoreGive(s_lock);
    s_req.status = status;
    s_req.out = out;
    s_req.out_len = out_len;
    s_req.done = true;
    notify(NB_ORDER);
    vTaskDelete(NULL);
}

static void req_finish(void)
{
    /* {"t":"res","id":…,"status":n,"body":<the handler's JSON>}: its bytes pass through. */
    const size_t idlen = strlen(s_req.id);
    const size_t need = s_req.out_len + idlen + 64u;
    if (s_req.out != NULL && s_req.out_len > 0u && s_req.out[0] == '{' && need <= BUF_MAX) {
        const int n = snprintf(s_buf, BUF_MAX, "{\"t\":\"res\",\"id\":\"%s\",\"status\":%d,\"body\":",
                               s_req.id, s_req.status);
        memcpy(s_buf + n, s_req.out, s_req.out_len);
        s_buf[(size_t)n + s_req.out_len] = '}';
        publish("/up/res", s_buf, (size_t)n + s_req.out_len + 1u, false);
    } else {
        answer_res(s_req.id, s_req.status, need > BUF_MAX ? "too_long" : NULL);
    }
    free(s_req.out);
    if (s_req.body != NULL) {
        sodium_memzero(s_req.body, s_req.body_len); /* it can hold a person's code (MOB-044) */
    }
    free(s_req.body);
    memset(&s_req, 0, sizeof(s_req));
}

static void handle_req(const proto_srv_req_t *q)
{
    if (s_req_fn == NULL) {
        answer_res(q->id, 404, "not_built"); /* "this panel can't do that yet" (panel-link.md) */
        return;
    }
    const char *why = proto_srv_req_refusal(q->method, q->path, q->fresh);
    if (why != NULL) {
        answer_res(q->id, 403, why);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const proto_srv_refusal_t refusal =
        proto_srv_check(&s_ceiling, q->as, proto_srv_req_act(q->method, q->path), wall_ms());
    xSemaphoreGive(s_lock);
    if (refusal != PROTO_SRV_OK || !remote_eligible(q->as)) {
        answer_res(q->id, 403, refusal != PROTO_SRV_OK ? proto_srv_refusal_name(refusal)
                                                       : "not_allowed");
        return;
    }
    if (s_req.busy) {
        answer_res(q->id, 503, "busy");
        return;
    }
    memset(&s_req, 0, sizeof(s_req));
    snprintf(s_req.id, sizeof(s_req.id), "%s", q->id);
    snprintf(s_req.method, sizeof(s_req.method), "%s", q->method);
    snprintf(s_req.path, sizeof(s_req.path), "%s", q->path);
    s_req.as = q->as;
    s_req.fresh = q->fresh;
    s_req.actor = q->actor;
    s_req.source = ALARM_SRC_ID_SERVER_APP;
    if (q->body != NULL) {
        s_req.body = alarm_mem_slow_malloc(q->body_len + 1u);
        if (s_req.body == NULL) {
            answer_res(q->id, 503, "busy");
            return;
        }
        memcpy(s_req.body, q->body, q->body_len);
        s_req.body[q->body_len] = '\0';
        s_req.body_len = q->body_len;
        const char *why = NULL;
        const int refused = strcmp(q->method, "POST") == 0 && strcmp(q->path, "people") == 0
                                ? people_unseal(&s_req.body, &s_req.body_len, &why)
                                : 0;
        if (refused != 0) {
            sodium_memzero(s_req.body, s_req.body_len);
            free(s_req.body);
            memset(&s_req, 0, sizeof(s_req));
            answer_res(q->id, refused, why);
            return;
        }
    }
    s_req.busy = true;
    if (xTaskCreatePinnedToCore(req_task, "server_req", REQ_STACK, NULL, TASK_PRIORITY - 1, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        free(s_req.body);
        memset(&s_req, 0, sizeof(s_req));
        answer_res(q->id, 503, "busy");
    }
}

/* D6: the server can't widen what the person at the panel allowed. Turning access off is always
 * honoured; turning it on needs the person who just pressed Allow, or an Owner's fresh step-up. */
static void handle_people_link(const proto_srv_people_link_t *l)
{
    if (l->on && !remote_eligible(l->user)) {
        answer_res(l->id, 403, "not_allowed");
        return;
    }
    const alarm_ms_t now = alarm_service_now();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool first = s_allowed_at != 0u && now - s_allowed_at < LINK_GRACE_MS &&
                       l->user == s_link_user;
    const bool owner = l->fresh && l->as >= 1u && l->as <= ALARM_MAX_USERS &&
                       s_cfg->users[l->as - 1u].authority == ALARM_AUTH_MASTER &&
                       proto_srv_check(&s_ceiling, l->as, PROTO_SRV_ACT_PEOPLE, wall_ms()) ==
                           PROTO_SRV_OK;
    esp_err_t err = ESP_OK;
    const bool allowed = !l->on || first || owner;
    if (allowed) {
        proto_srv_ceiling_t next = s_ceiling;
        proto_srv_user_set_remote(&next, l->user, l->on, l->until_ms);
        next.rev++;
        err = store_ceiling(&next);
        if (err == ESP_OK) {
            s_ceiling = next;
        }
    }
    xSemaphoreGive(s_lock);
    if (!allowed) {
        answer_res(l->id, 403, "not_allowed");
    } else if (err != ESP_OK) {
        answer_res(l->id, 503, "not_saved");
    } else {
        ESP_LOGI(TAG, "user %u remote sign-in %s", (unsigned)l->user, l->on ? "on" : "off");
        answer_res(l->id, 200, NULL);
        s_sync_steps |= SYNC_PEOPLE | SYNC_CEILING;
    }
}

/* ---- linking and unlinking ------------------------------------------------------------------- */

static void client_stop(void);

static void wipe_link_record(const char *why)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    proto_srv_ceiling_defaults(&s_ceiling);
    s_linked = false;
    s_location[0] = '\0';
    s_link_user = 0u;
    s_allowed_at = 0u;
    const esp_err_t err = store_link(false, "", 0u, &s_ceiling);
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "unlinked (%s)%s", why, err == ESP_OK ? "" : ": not saved");
    proto_srvq_enable(&s_q, false, alarm_service_now());
}

static void on_time(const proto_srv_time_t *t)
{
    const alarm_ms_t now = alarm_service_now();
    proto_srv_clock_t c;
    proto_srv_clock_check(t->server_ms, t->echo_uptime_ms, uptime_ms(), wall_ms(), &c);
    if (c.trusted) {
        s_offset_ms = c.offset_ms;
    }
    if (c.set_clock) {
        /* API-018: logged by the clock (panel_clock's "by hand, server"). Old records keep theirs. */
        const esp_err_t err = panel_clock_set(c.server_now, "server");
        ESP_LOGW(TAG, "clock off by %" PRId64 " ms: %s", c.offset_ms,
                 err == ESP_OK ? "set from the server" : esp_err_to_name(err));
        if (err == ESP_OK) {
            s_clock_by_server = true;
        } else {
            struct timeval tv = {.tv_sec = c.server_now / 1000,
                                 .tv_usec = (suseconds_t)(c.server_now % 1000) * 1000};
            settimeofday(&tv, NULL);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool linked = s_linked;
    const link_sub_t sub = s_sub;
    if (linked && t->location_id[0] != '\0' && strcmp(t->location_id, s_location) != 0) {
        snprintf(s_location, sizeof(s_location), "%s", t->location_id);
        store_str("loc", s_location);
    }
    xSemaphoreGive(s_lock);
    if (linked) {
        if (!s_q.running) {
            proto_srvq_time(&s_q, t->ack_seq, now);
            s_sync_steps = SYNC_SNAPSHOT | SYNC_PEOPLE | SYNC_CEILING | SYNC_HEALTH;
        }
    } else if (sub == SUB_START) {
        pj_writer_t w;
        pj_init(&w, s_buf, BUF_MAX);
        proto_srv_link_start(&w);
        if (publish_w("/up/link", &w)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_sub = SUB_WAIT_CODE;
            xSemaphoreGive(s_lock);
        }
    }
}

static void end_linking(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sub = SUB_NONE;
    memset(&s_code, 0, sizeof(s_code));
    memset(&s_claim, 0, sizeof(s_claim));
    xSemaphoreGive(s_lock);
}

/* The orders the API left for this task. */
static void run_orders(alarm_ms_t now)
{
    pj_writer_t w;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const link_sub_t sub = s_sub;
    const bool notice = s_unlink_notice;
    const bool settings = s_settings_changed;
    const bool ceiling = s_ceiling_changed;
    const alarm_ms_t code_until = s_code_until;
    const uint8_t user = s_link_user;
    char claim[sizeof(s_claim.claim)];
    snprintf(claim, sizeof(claim), "%s", s_claim.claim);
    s_unlink_notice = false;
    s_settings_changed = false;
    s_ceiling_changed = false;
    xSemaphoreGive(s_lock);

    if (notice) {
        if (s_connected) {
            pj_init(&w, s_buf, BUF_MAX);
            proto_srv_unlink(&w);
            publish_w("/up/link", &w);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        proto_srvq_enable(&s_q, false, now);
        client_stop();
    }
    if (settings) {
        client_stop();
        s_retry_ms = RETRY_MIN_MS;
        s_try_at = 0;
        s_old_software = false;
    }
    if (ceiling) {
        s_sync_steps |= SYNC_PEOPLE | SYNC_CEILING;
    }
    if (sub == SUB_ALLOWED) {
        pj_init(&w, s_buf, BUF_MAX);
        proto_srv_link_done(&w, claim, true, user);
        publish_w("/up/link", &w);
        /* NOTE-011: a first link starts from now. */
        panel_log_flush(500);
        proto_srvq_enable(&s_q, true, now);
        proto_srvq_start_from_now(&s_q, panel_log_last_seq());
        proto_srvq_time(&s_q, 0u, now);
        s_sync_steps = SYNC_SNAPSHOT | SYNC_PEOPLE | SYNC_CEILING | SYNC_HEALTH;
        end_linking();
        ESP_LOGI(TAG, "linked");
    } else if (sub == SUB_DENIED) {
        pj_init(&w, s_buf, BUF_MAX);
        proto_srv_link_done(&w, claim, false, 0u);
        publish_w("/up/link", &w);
        vTaskDelay(pdMS_TO_TICKS(200));
        end_linking();
        client_stop();
    } else if (sub == SUB_CANCEL ||
               ((sub == SUB_CODE || sub == SUB_WAIT_CODE) && code_until != 0u && now >= code_until)) {
        if (s_connected) {
            pj_init(&w, s_buf, BUF_MAX);
            proto_srv_link_cancel(&w);
            publish_w("/up/link", &w);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        end_linking();
        client_stop();
    }
}

static void handle_in(const in_msg_t *m)
{
    if (m->retained) {
        ESP_LOGW(TAG, "a retained message on %s was ignored", m->topic); /* MQTT-020 */
        return;
    }
    const size_t tl = strlen(m->topic);
    const bool is_ack = tl >= 9u && strcmp(m->topic + tl - 9u, "/down/ack") == 0;
    const bool is_time = tl >= 10u && strcmp(m->topic + tl - 10u, "/down/time") == 0;
    proto_srv_parse(m->payload, m->len, is_ack, is_time, &s_parsed);
    s_heard_ms = alarm_service_now();
    const alarm_ms_t now = alarm_service_now();
    switch (s_parsed.type) {
    case PROTO_SRV_IN_TIME:
        on_time(&s_parsed.u.time);
        break;
    case PROTO_SRV_IN_ACK:
        proto_srvq_ack(&s_q, s_parsed.u.ack_seq, now);
        break;
    case PROTO_SRV_IN_RESYNC:
        s_sync_steps |= SYNC_SNAPSHOT;
        break;
    case PROTO_SRV_IN_UNSUPPORTED:
        ESP_LOGW(TAG, "the server says this panel needs a software update");
        s_fail = SERVER_LINK_FAIL_OLD;
        s_old_software = true;
        client_stop();
        break;
    case PROTO_SRV_IN_LINK_CODE:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_code = s_parsed.u.link_code;
        s_code_until = now + (alarm_ms_t)(s_code.expires_s != 0u ? s_code.expires_s : 600u) * 1000u;
        s_sub = SUB_CODE;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "link code %s (%s)", s_parsed.u.link_code.code, s_parsed.u.link_code.url);
        break;
    case PROTO_SRV_IN_LINK_CLAIM:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_sub == SUB_CODE) {
            s_claim = s_parsed.u.link_claim;
            s_sub = SUB_CLAIM;
            s_code_until = now + 600000u;
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "claimed by %s for %s", s_parsed.u.link_claim.account_name,
                 s_parsed.u.link_claim.location_name);
        break;
    case PROTO_SRV_IN_LINKED:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_linked) {
            snprintf(s_location, sizeof(s_location), "%s", s_parsed.u.location_id);
            store_str("loc", s_location);
        }
        xSemaphoreGive(s_lock);
        break;
    case PROTO_SRV_IN_UNLINK:
        if (s_linked) {
            wipe_link_record("by the server");
            client_stop();
        }
        break;
    case PROTO_SRV_IN_COMMAND:
        if (s_linked && s_have_cfg) {
            handle_command(&s_parsed.u.command);
        }
        break;
    case PROTO_SRV_IN_REQ:
        if (s_linked && s_have_cfg) {
            handle_req(&s_parsed.u.req);
        }
        break;
    case PROTO_SRV_IN_PEOPLE_LINK:
        if (s_linked && s_have_cfg) {
            handle_people_link(&s_parsed.u.people_link);
        }
        break;
    case PROTO_SRV_IN_BAD:
        ESP_LOGW(TAG, "a message that doesn't read on %s", m->topic);
        break;
    case PROTO_SRV_IN_CONTACTS:
    case PROTO_SRV_IN_UNKNOWN:
    default:
        break; /* unknown types are ignored (protocol.md) */
    }
    sodium_memzero(&s_parsed, sizeof(s_parsed));
}

/* ---- the client ------------------------------------------------------------------------------ */

static server_link_fail_t classify(const esp_mqtt_error_codes_t *e)
{
    if (e == NULL) {
        return SERVER_LINK_FAIL_OTHER;
    }
    if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        return SERVER_LINK_FAIL_REFUSED;
    }
    if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        const esp_err_t t = e->esp_tls_last_esp_err;
        if (t == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME ||
            t == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST ||
            t == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || t == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET) {
            return SERVER_LINK_FAIL_UNREACHABLE;
        }
        if (e->esp_tls_cert_verify_flags != 0 || t == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED ||
            t == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED) {
            return SERVER_LINK_FAIL_TLS;
        }
        return t == ESP_OK ? SERVER_LINK_FAIL_UNREACHABLE : SERVER_LINK_FAIL_OTHER;
    }
    return SERVER_LINK_FAIL_OTHER;
}

/* Runs in the esp-mqtt task: record and hand over, nothing else. */
static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t ev = data;
    static in_msg_t msg ALARM_SLOW_BSS; /* only this task writes it; the queue copies it */
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_fail = SERVER_LINK_FAIL_NONE;
        notify(NB_UP);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        notify(NB_DOWN);
        break;
    case MQTT_EVENT_DATA:
        if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len &&
            ev->topic_len > 0 && (size_t)ev->topic_len < sizeof(msg.topic) &&
            (size_t)ev->data_len < sizeof(msg.payload)) {
            memcpy(msg.topic, ev->topic, (size_t)ev->topic_len);
            msg.topic[ev->topic_len] = '\0';
            memcpy(msg.payload, ev->data, (size_t)ev->data_len);
            msg.payload[ev->data_len] = '\0';
            msg.len = (size_t)ev->data_len;
            msg.retained = ev->retain;
            if (xQueueSend(s_in, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "too many messages at once: one on %s dropped", msg.topic);
            }
            sodium_memzero(&msg, sizeof(msg));
            notify(NB_IN);
        } else {
            ESP_LOGW(TAG, "a message too long or in pieces was dropped");
        }
        break;
    case MQTT_EVENT_ERROR:
        s_fail = classify(ev->error_handle);
        ESP_LOGW(TAG, "connection problem %d (tls 0x%x)", (int)s_fail,
                 ev->error_handle != NULL ? (unsigned)ev->error_handle->esp_tls_last_esp_err : 0u);
        break;
    default:
        break;
    }
}

static void client_stop(void)
{
    if (s_client == NULL) {
        return;
    }
    if (s_connected) {
        char online[24];
        pj_writer_t w;
        pj_init(&w, online, sizeof(online));
        proto_srv_online(&w, false);
        const size_t n = pj_finish(&w);
        publish("/up/online", online, n, true); /* a clean disconnect skips the will */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
    proto_srvq_disconnected(&s_q, alarm_service_now());
}

/* The next try: 1 s doubling to 5 minutes, +-20 % (panel-link.md "Transport"). */
static void back_off(alarm_ms_t now)
{
    const uint32_t spread = s_retry_ms / 5u;
    s_try_at = now + s_retry_ms - spread + esp_random() % (2u * spread + 1u);
    s_retry_ms = s_retry_ms >= RETRY_MAX_MS / 2u ? RETRY_MAX_MS : s_retry_ms * 2u;
}

static void client_start(alarm_ms_t now)
{
    s_tried_at = now;
    /* Each connect signs a new counter, written down first so one is never used twice. */
    const uint64_t counter = s_counter + 1u;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u64(h, "counter", counter);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't save the sign-in counter: %s", esp_err_to_name(err));
        s_fail = SERVER_LINK_FAIL_OTHER;
        back_off(now);
        return;
    }
    s_counter = counter;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    char host[PROTO_SRV_HOST_LEN];
    snprintf(host, sizeof(host), "%s", s_host);
    const uint16_t port = s_port != 0u ? s_port : 443u;
    const bool own_ca = s_ca != NULL;
    if (own_ca) {
        snprintf(s_run_ca_pem, sizeof(s_run_ca_pem), "%s", s_ca);
    }
    xSemaphoreGive(s_lock);

    uint8_t signed_bytes[PROTO_SRV_SIGN_MAX + PROTO_SRV_HOST_LEN];
    const size_t sn = proto_srv_sign_bytes(host, s_id, counter, signed_bytes, sizeof(signed_bytes));
    uint8_t sig[crypto_sign_BYTES];
    if (sn == 0u || crypto_sign_detached(sig, NULL, signed_bytes, sn, s_sk) != 0 ||
        !proto_srv_password(counter, sig, s_pk, s_password, sizeof(s_password))) {
        s_fail = SERVER_LINK_FAIL_OTHER;
        back_off(now);
        return;
    }
    snprintf(s_uri, sizeof(s_uri), "wss://%s:%u/mqtt", host, (unsigned)port);
    snprintf(s_client_id, sizeof(s_client_id), "ra-%s", s_id);
    (void)proto_srv_topic(s_id, "/up/online", s_will_topic, sizeof(s_will_topic));

    esp_mqtt_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.broker.address.uri = s_uri;
    if (own_ca) {
        cfg.broker.verification.certificate = s_run_ca_pem;
    } else {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.session.protocol_ver = MQTT_PROTOCOL_V_5;
    cfg.credentials.client_id = s_client_id;
    cfg.credentials.username = s_id;
    cfg.credentials.authentication.password = s_password;
    cfg.session.last_will.topic = s_will_topic;
    cfg.session.last_will.msg = "{\"online\":false}";
    cfg.session.last_will.msg_len = 16;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.session.keepalive = KEEPALIVE_S;
    cfg.network.disable_auto_reconnect = true;
    cfg.network.timeout_ms = 10000;
    cfg.buffer.size = MQTT_BUF;
    cfg.buffer.out_size = MQTT_BUF;
    cfg.outbox.limit = 128u * 1024u; /* unacknowledged messages; the log keeps events */
    cfg.task.stack_size = 6144;
    cfg.task.priority = 5;
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        s_fail = SERVER_LINK_FAIL_OTHER;
        back_off(now);
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_fail = SERVER_LINK_FAIL_OTHER;
        back_off(now);
        return;
    }
    ESP_LOGI(TAG, "connecting to %s as %s", s_uri, s_id);
}

static void on_connected(alarm_ms_t now)
{
    char topic[56];
    ESP_LOGI(TAG, "connected");
    s_retry_ms = RETRY_MIN_MS;
    s_synced = false;
    if (proto_srv_topic(s_id, "/down/#", topic, sizeof(topic))) {
        esp_mqtt_client_subscribe_single(s_client, topic, 1);
    }
    char online[24];
    pj_writer_t w;
    pj_init(&w, online, sizeof(online));
    proto_srv_online(&w, true);
    const size_t n = pj_finish(&w);
    publish("/up/online", online, n, true);
    refresh_config();
    panel_log_flush(500); /* what became due before the connection counts as replayed */
    proto_srvq_connected(&s_q, now);
    send_hello();
}

/* ---- the task -------------------------------------------------------------------------------- */

static void server_task(void *arg)
{
    (void)arg;
    uint32_t acked;
    uint32_t acked_seq;
    load_position(&acked, &acked_seq);
    proto_srvq_init(&s_q, &QUEUE_PORT, acked, acked_seq, s_linked, alarm_service_now());

    alarm_ms_t delta_due = 0;
    alarm_ms_t health_at = 0;
    alarm_ms_t hello_at = 0;
    alarm_ms_t saved_at = 0;
    bool delta_pending = false;
    bool more = false;
    bool wifi_wanted = false;

    for (;;) {
        uint32_t bits = 0;
        const TickType_t wait = more ? 0 : pdMS_TO_TICKS(delta_pending ? DELTA_MS : 1000u);
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        const alarm_ms_t now = alarm_service_now();
        more = false;
        panel_log_set_clock(clock_now());

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const link_sub_t sub = s_sub;
        const bool linked = s_linked;
        xSemaphoreGive(s_lock);
        const bool wanted = linked || sub != SUB_NONE;
        if (wanted != wifi_wanted) {
            wifi_wanted = wanted;
            net_wifi_set_required(NET_WIFI_REQ_SERVER, wanted);
        }

        run_orders(now);
        app_push_review();
        if (s_req.done) {
            req_finish();
        }
        if (now < s_offline_until && s_client != NULL) {
            client_stop();
            s_try_at = s_offline_until;
        }

        net_wifi_status_t ws;
        net_wifi_get_status(&ws);
        if (s_client == NULL && wanted && !s_old_software && now >= s_try_at &&
            ws.state == NET_WIFI_CONNECTED) {
            client_start(now);
        }
        /* One connect that never came to anything: no event will say so. */
        if (s_client != NULL && !s_connected && now - s_tried_at >= CONNECT_MAX_MS) {
            bits |= NB_DOWN;
        }
        if ((bits & NB_UP) != 0u && s_connected) {
            on_connected(now);
            health_at = now + HEALTH_MS;
            hello_at = now + HELLO_MS;
        }
        if ((bits & NB_DOWN) != 0u && !s_connected && s_client != NULL) {
            client_stop();
            back_off(now);
            ESP_LOGW(TAG, "disconnected; trying again in %" PRIu64 " ms", s_try_at - now);
        }
        while (xQueueReceive(s_in, &s_in_msg, 0) == pdTRUE) {
            handle_in(&s_in_msg);
        }
        sodium_memzero(&s_in_msg, sizeof(s_in_msg));
        if ((bits & NB_SNAPSHOT) != 0u && !delta_pending) {
            delta_pending = true;
            delta_due = now + DELTA_MS;
        }

        /* DIR-004: a phone goes with its user as soon as the user changes, with or without the
         * server, so a slot given to someone else never inherits the last person's phones. */
        const uint64_t deleted = atomic_exchange(&s_deleted_users, 0u);
        if (deleted != 0u) {
            phones_forget_users(deleted);
        }
        const uint32_t cfg_seq = s_cfg_seq;
        bool cfg_changed = false;
        if ((s_connected && linked) || phones_any()) {
            refresh_config();
            cfg_changed = s_have_cfg && s_cfg_seq != cfg_seq;
            if (cfg_changed) {
                phones_review(s_cfg);
            }
        }
        if (s_connected && linked) {
            if (cfg_changed) {
                s_sync_steps |= SYNC_SNAPSHOT | SYNC_PEOPLE;
                pj_writer_t w;
                pj_init(&w, s_buf, BUF_MAX);
                proto_srv_config(&w, "alarm", s_cfg_seq); /* WEB-027's nudge */
                publish_w("/up/state", &w);
            }
            if (s_q.running && s_have_cfg) {
                if ((s_sync_steps & SYNC_SNAPSHOT) != 0u) {
                    send_snapshot();
                    s_sync_steps &= (uint8_t)~SYNC_SNAPSHOT;
                }
                if ((s_sync_steps & SYNC_PEOPLE) != 0u) {
                    send_people();
                    s_sync_steps &= (uint8_t)~SYNC_PEOPLE;
                }
                if ((s_sync_steps & SYNC_CEILING) != 0u) {
                    send_ceiling();
                    s_sync_steps &= (uint8_t)~SYNC_CEILING;
                }
                if ((s_sync_steps & SYNC_HEALTH) != 0u) {
                    send_health();
                    s_sync_steps &= (uint8_t)~SYNC_HEALTH;
                }
                if (delta_pending && now >= delta_due && s_synced) {
                    send_delta();
                    delta_pending = false;
                }
                if (panel_log_ready()) {
                    if ((bits & NB_EVENT) != 0u) {
                        panel_log_flush(300);
                    }
                    for (int i = 0; i < POLLS_PER_WAKE; i++) {
                        if (!proto_srvq_poll(&s_q, now)) {
                            break;
                        }
                        more = i == POLLS_PER_WAKE - 1;
                    }
                }
            }
            if (now >= health_at) {
                s_sync_steps |= SYNC_HEALTH;
                health_at = now + HEALTH_MS;
            }
            if (now >= hello_at) {
                send_hello(); /* the clock again; the queue keeps going */
                hello_at = now + HELLO_MS;
            }
        }
        if (s_q.acked_dirty && now - saved_at >= SAVE_MS) {
            save_position();
            s_q.acked_dirty = false;
            saved_at = now;
        }
        const bool fail = proto_srvq_comm_fail(&s_q, now); /* MQTT-011's rule */
        if (fail != s_comm_fail) {
            s_comm_fail = fail;
            alarm_service_comm_fail(ALARM_COMM_FAIL_SERVER, fail);
        }
        s_waiting = linked && (s_q.window_n != 0u || s_q.sent < panel_log_newest_index());
    }
}

/* ---- listener -------------------------------------------------------------------------------- */

static void on_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    (void)snap;
    notify(NB_SNAPSHOT);
}

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (ev->type == ALARM_EV_USER_DELETED && ev->arg >= 1u && ev->arg <= ALARM_MAX_USERS) {
        atomic_fetch_or(&s_deleted_users, (uint64_t)1u << (ev->arg - 1u));
    }
    notify(NB_EVENT);
}

static const alarm_listener_t s_listener = {
    .on_event = on_event,
    .on_snapshot = on_snapshot,
};

const alarm_listener_t *server_link_listener(void)
{
    return &s_listener;
}

/* ---- API ------------------------------------------------------------------------------------- */

void server_link_panel_id(char out[PROTO_SRV_ID_LEN])
{
    snprintf(out, PROTO_SRV_ID_LEN, "%s", s_id);
}

bool server_link_sign(const void *msg, size_t len, uint8_t sig[64])
{
    return crypto_sign_detached(sig, NULL, msg, len, s_sk) == 0;
}

void server_link_public_key(uint8_t pub[32])
{
    memcpy(pub, s_pk, 32u);
}

esp_err_t server_link_set_relay(const char *base)
{
    if (s_lock == NULL || base == NULL || strncmp(base, "https://", 8) != 0 ||
        strlen(base) >= sizeof(s_relay)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = store_str("relay", base);
    if (err == ESP_OK) {
        snprintf(s_relay, sizeof(s_relay), "%s", base);
    }
    xSemaphoreGive(s_lock);
    return err;
}

void server_link_relay_base(char *out, size_t cap, bool *own_ca)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, cap, "%s", s_relay);
    *own_ca = s_ca != NULL;
    xSemaphoreGive(s_lock);
}

const char *server_link_ca(void)
{
    return s_ca;
}

void server_link_set_cert_source(server_link_cert_fn fn)
{
    s_cert_fn = fn;
}

bool server_link_cert_sha256(uint8_t sha256[32])
{
    return s_cert_fn != NULL && s_cert_fn(sha256);
}

void server_link_set_req_handler(server_link_req_fn fn)
{
    s_req_fn = fn;
}

void server_link_set_model(const char *model)
{
    s_model = model != NULL ? model : "RoboAlarms panel";
}

esp_err_t server_link_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Not sodium_init(): it only picks among implementations this chip has one of, and calling it
     * links every one of them (about 80 KB). crypto_check() proves what is used instead. */
    ESP_RETURN_ON_ERROR(crypto_check(), TAG, "Ed25519 self-test");
    s_lock = xSemaphoreCreateMutex();
    s_in = xQueueCreateWithCaps(IN_QUEUE_LEN, sizeof(in_msg_t), alarm_mem_slow_caps());
    s_buf = alarm_mem_slow_malloc(BUF_MAX);
    s_cfg = alarm_mem_slow_malloc(sizeof(alarm_config_t));
    if (s_lock == NULL || s_in == NULL || s_buf == NULL || s_cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    load_settings();
    ESP_RETURN_ON_ERROR(load_or_make_key(), TAG, "the panel's key");
    if (xTaskCreatePinnedToCore(server_task, "server", TASK_STACK, NULL, TASK_PRIORITY, &s_task,
                                tskNO_AFFINITY) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(phones_init(), TAG, "phones");
    ESP_LOGI(TAG, "panel %s, %s", s_id, s_linked ? "linked" : "not linked");
    return ESP_OK;
}

void server_link_get_status(server_link_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    const alarm_ms_t now = alarm_service_now();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->linked = s_linked;
    snprintf(out->host, sizeof(out->host), "%s", s_host);
    out->port = s_port;
    out->ca_loaded = s_ca != NULL;
    snprintf(out->location_id, sizeof(out->location_id), "%s", s_location);
    const link_sub_t sub = s_sub;
    if (sub == SUB_CODE || sub == SUB_CLAIM) {
        snprintf(out->code, sizeof(out->code), "%s", s_code.code);
        snprintf(out->url, sizeof(out->url), "%s", s_code.url);
        out->code_left_s = s_code_until > now ? (uint32_t)((s_code_until - now) / 1000u) : 0u;
    }
    if (sub == SUB_CLAIM) {
        snprintf(out->account_name, sizeof(out->account_name), "%s", s_claim.account_name);
        snprintf(out->account_email, sizeof(out->account_email), "%s",
                 s_claim.account_email_masked);
        snprintf(out->location_name, sizeof(out->location_name), "%s", s_claim.location_name);
    }
    xSemaphoreGive(s_lock);
    snprintf(out->panel_id, sizeof(out->panel_id), "%s", s_id);
    out->fail = s_fail;
    out->sent = s_sent;
    out->waiting = s_waiting;
    out->clock_offset_ms = s_offset_ms;
    out->last_heard_ms = s_heard_ms;
    if (!out->linked && sub == SUB_NONE) {
        out->state = SERVER_LINK_IDLE;
    } else if (ws.state != NET_WIFI_CONNECTED) {
        out->state = SERVER_LINK_NO_NETWORK;
    } else if (!s_connected) {
        out->state = s_fail != SERVER_LINK_FAIL_NONE ? SERVER_LINK_FAILED : SERVER_LINK_CONNECTING;
    } else if (sub == SUB_START || sub == SUB_WAIT_CODE) {
        out->state = SERVER_LINK_WAIT_CODE;
    } else if (sub == SUB_CODE) {
        out->state = SERVER_LINK_SHOW_CODE;
    } else if (sub == SUB_CLAIM) {
        out->state = SERVER_LINK_CLAIMED;
    } else {
        out->state = SERVER_LINK_CONNECTED;
    }
}

esp_err_t server_link_set_host(const char *host, uint16_t port)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char norm[PROTO_SRV_HOST_LEN];
    if (host == NULL || strchr(host, ' ') != NULL || strchr(host, '/') != NULL ||
        !proto_srv_host_norm(host, norm, sizeof(norm))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_linked) { /* moving means unlink, change the address, link again */
        nvs_handle_t h;
        err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "host", norm);
            if (err == ESP_OK) {
                err = nvs_set_u16(h, "port", port);
            }
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err == ESP_OK) {
            snprintf(s_host, sizeof(s_host), "%s", norm);
            s_port = port;
            s_settings_changed = true;
        }
    }
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
    return err;
}

esp_err_t server_link_set_ca(const char *pem)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t len = pem != NULL ? strlen(pem) : 0u;
    if (len > CA_MAX || (len > 0u && strstr(pem, "-----BEGIN CERTIFICATE-----") == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = len > 0u ? nvs_set_blob(h, "ca", pem, len) : nvs_erase_key(h, "ca");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_ca);
    s_ca = len > 0u ? strdup(pem) : NULL;
    s_settings_changed = true;
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
    return ESP_OK;
}

esp_err_t server_link_begin(uint8_t user)
{
    if (s_lock == NULL || user < 1u || user > ALARM_MAX_USERS) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_linked && s_sub == SUB_NONE) {
        s_sub = SUB_START;
        s_link_user = user;
        s_code_until = alarm_service_now() + 120000u; /* the server has 2 minutes to answer */
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
    return err;
}

void server_link_cancel(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sub != SUB_NONE && s_sub != SUB_ALLOWED) {
        s_sub = SUB_CANCEL;
    }
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
}

void server_link_decide(bool allow)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sub == SUB_CLAIM) {
        if (allow) {
            /* The record first, in one write, then the answer (panel-link.md step 5). The person
             * whose code opened Connect may act through the server from now on. */
            proto_srv_ceiling_t next;
            proto_srv_ceiling_defaults(&next);
            proto_srv_user_set_remote(&next, s_link_user, true, 0);
            next.rev = 1u;
            if (store_link(true, "", s_link_user, &next) == ESP_OK) {
                s_ceiling = next;
                s_linked = true;
                s_allowed_at = alarm_service_now();
                s_sub = SUB_ALLOWED;
            } else {
                ESP_LOGE(TAG, "the link couldn't be saved: refused");
                s_sub = SUB_DENIED;
            }
        } else {
            s_sub = SUB_DENIED;
        }
    }
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
}

esp_err_t server_link_unlink(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was = s_linked;
    proto_srv_ceiling_t def;
    proto_srv_ceiling_defaults(&def);
    const esp_err_t err = store_link(false, "", 0u, &def);
    s_linked = false; /* for this session whatever the flash said */
    s_location[0] = '\0';
    s_link_user = 0u;
    s_allowed_at = 0u;
    s_ceiling = def;
    s_sub = SUB_NONE;
    s_unlink_notice = was;
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
    return err;
}

void server_link_get_ceiling(proto_srv_ceiling_t *out)
{
    proto_srv_ceiling_defaults(out);
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        *out = s_ceiling;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t server_link_set_ceiling(const proto_srv_ceiling_t *in)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    proto_srv_ceiling_t next = *in;
    next.rev = s_ceiling.rev + 1u;
    const esp_err_t err = store_ceiling(&next);
    if (err == ESP_OK) {
        s_ceiling = next;
        s_ceiling_changed = true;
    }
    xSemaphoreGive(s_lock);
    notify(NB_ORDER);
    return err;
}

/* ---- service console ------------------------------------------------------------------------- */

static const char *const STATE_TEXT[] = {"idle (not linked)", "waiting for the network", "connecting",
                                         "failed, trying again", "waiting for a code",
                                         "showing a code", "claimed: server allow|deny", "connected"};
static const char *const FAIL_TEXT[] = {"", "unreachable", "sign-in refused",
                                        "certificate not trusted", "software too old", "other"};

static bool console_owner(const char *code, uint8_t *user)
{
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_CONSOLE};
    alarm_authority_t a = ALARM_AUTH_NONE;
    const alarm_result_t r = alarm_service_authenticate_as(&src, code, 0u, user, &a);
    if (r != ALARM_OK) {
        printf("%s\n", alarm_result_name(r));
        return false;
    }
    if (a != ALARM_AUTH_MASTER && a != ALARM_AUTH_INSTALLER) {
        printf("an Owner or Admin code is needed\n");
        return false;
    }
    return true;
}

static int read_ca(void)
{
    printf("Paste the PEM certificate(s), then a line with a single '.':\n");
    char *pem = calloc(1, CA_MAX + 1u);
    if (pem == NULL) {
        return 1;
    }
    size_t used = 0;
    char line[128];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strcmp(line, ".\n") == 0 || strcmp(line, ".\r\n") == 0 || strcmp(line, ".") == 0) {
            break;
        }
        const size_t n = strlen(line);
        if (used + n > CA_MAX) {
            printf("too long (at most %u bytes)\n", (unsigned)CA_MAX);
            free(pem);
            return 1;
        }
        memcpy(pem + used, line, n);
        used += n;
    }
    const esp_err_t err = server_link_set_ca(pem);
    printf("%s\n", err == ESP_OK ? "certificate stored" : esp_err_to_name(err));
    free(pem);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_phone(int argc, char **argv)
{
    uint8_t user = 0;
    esp_err_t err = ESP_OK;
    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        static proto_phone_t list[PROTO_PHONE_MAX] ALARM_SLOW_BSS;
        const size_t n = server_link_phone_list(list, PROTO_PHONE_MAX);
        for (size_t i = 0; i < n; i++) {
            char id[PROTO_PHONE_ID_LEN];
            proto_phone_id(list[i].pub, id);
            printf("%s user %u \"%s\"%s\n", id, (unsigned)list[i].user, list[i].name,
                   list[i].handle[0] != '\0' ? ", push" : "");
        }
        char name[PROTO_PHONE_NAME_LEN];
        if (server_link_phone_pair_asking(name, sizeof(name), &user)) {
            printf("add \"%s\" for user %u? server phone allow|deny\n", name, (unsigned)user);
        }
        printf("%u phone(s)\n", (unsigned)n);
        return 0;
    }
    const char *what = argv[2];
    if (strcmp(what, "pair") == 0 && argc >= 4) {
        const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_CONSOLE};
        alarm_authority_t a = ALARM_AUTH_NONE;
        const alarm_result_t r = alarm_service_authenticate_as(&src, argv[3], 0u, &user, &a);
        if (r != ALARM_OK) {
            printf("%s\n", alarm_result_name(r));
            return 1;
        }
        if (a != ALARM_AUTH_INSTALLER && a != ALARM_AUTH_MASTER && a != ALARM_AUTH_PARTITION_MASTER &&
            a != ALARM_AUTH_STANDARD) {
            printf("this code can't have a phone\n");
            return 1;
        }
        uint8_t fp[32];
        if (s_cert_fn == NULL || !s_cert_fn(fp)) {
            printf("the Panel Manager's certificate isn't ready: turn the Web server on first\n");
            return 1;
        }
        net_wifi_status_t ws;
        net_wifi_get_status(&ws);
        char uri[256];
        err = server_link_phone_pair_begin(user, fp, ws.ip, uri, sizeof(uri));
        if (err == ESP_OK) {
            printf("%s\n", uri);
            return 0;
        }
    } else if (strcmp(what, "allow") == 0) {
        err = server_link_phone_pair_decide(true);
    } else if (strcmp(what, "deny") == 0) {
        err = server_link_phone_pair_decide(false);
    } else if (strcmp(what, "cancel") == 0) {
        server_link_phone_pair_cancel();
    } else if (strcmp(what, "remove") == 0 && argc >= 4) {
        err = server_link_phone_remove(argv[3]);
    } else {
        printf("server phone [list | pair <code> | allow | deny | cancel | remove <id>]\n");
        return 1;
    }
    printf("%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_server(int argc, char **argv)
{
    if (argc < 2) {
        static server_link_status_t st ALARM_SLOW_BSS;
        server_link_get_status(&st);
        printf("%s%s%s\n", STATE_TEXT[st.state], st.fail != SERVER_LINK_FAIL_NONE ? ": " : "",
               st.fail != SERVER_LINK_FAIL_NONE ? FAIL_TEXT[st.fail] : "");
        printf("server %s:%u%s, panel %s, location %s\n", st.host,
               st.port != 0u ? (unsigned)st.port : 443u, st.ca_loaded ? " (own CA)" : "",
               st.panel_id, st.location_id[0] ? st.location_id : "none");
        printf("%" PRIu32 " events sent%s, clock offset %" PRId64 " ms\n", st.sent,
               st.waiting ? ", some waiting" : "", st.clock_offset_ms);
        if (st.code[0] != '\0') {
            printf("code %s, %s, %" PRIu32 " s left\n", st.code, st.url, st.code_left_s);
        }
        if (st.account_name[0] != '\0') {
            printf("link to %s (%s) as %s?\n", st.account_name, st.account_email, st.location_name);
        }
        proto_srv_ceiling_t c;
        server_link_get_ceiling(&c);
        printf("ceiling rev %" PRIu32 ": disarm %s, setup %s, people %s, ask code %s, users", c.rev,
               c.remote_disarm ? "on" : "off", c.remote_setup ? "on" : "off",
               c.remote_people ? "on" : "off", c.ask_code ? "on" : "off");
        for (unsigned u = 1u; u <= ALARM_MAX_USERS; u++) {
            if (proto_srv_user_remote(&c, (uint8_t)u)) {
                printf(" %u", u);
            }
        }
        printf("\n");
        return 0;
    }
    const char *what = argv[1];
    uint8_t user = 0;
    esp_err_t err = ESP_OK;
    if (strcmp(what, "phone") == 0) {
        return cmd_phone(argc, argv);
    }
    if (strcmp(what, "offline") == 0 && argc >= 3) {
        s_offline_until = alarm_service_now() + (alarm_ms_t)atoi(argv[2]) * 1000u;
        notify(NB_ORDER);
    } else if (strcmp(what, "relay") == 0 && argc >= 3) {
        err = server_link_set_relay(argv[2]);
    } else if (strcmp(what, "host") == 0 && argc >= 3) {
        err = server_link_set_host(argv[2], argc >= 4 ? (uint16_t)atoi(argv[3]) : 0u);
    } else if (strcmp(what, "ca") == 0) {
        if (argc >= 3 && strcmp(argv[2], "clear") == 0) {
            err = server_link_set_ca(NULL);
        } else {
            return read_ca();
        }
    } else if (strcmp(what, "link") == 0 && argc >= 3) {
        if (!console_owner(argv[2], &user)) {
            return 1;
        }
        err = server_link_begin(user);
    } else if (strcmp(what, "allow") == 0) {
        server_link_decide(true);
    } else if (strcmp(what, "deny") == 0) {
        server_link_decide(false);
    } else if (strcmp(what, "cancel") == 0) {
        server_link_cancel();
    } else if (strcmp(what, "unlink") == 0 && argc >= 3) {
        if (!console_owner(argv[2], &user)) {
            return 1;
        }
        err = server_link_unlink();
    } else if (strcmp(what, "ceiling") == 0 && argc >= 5) {
        if (!console_owner(argv[2], &user)) {
            return 1;
        }
        proto_srv_ceiling_t c;
        server_link_get_ceiling(&c);
        const bool on = strcmp(argv[4], "on") == 0;
        if (strcmp(argv[3], "disarm") == 0) {
            c.remote_disarm = on;
        } else if (strcmp(argv[3], "setup") == 0) {
            c.remote_setup = on;
        } else if (strcmp(argv[3], "people") == 0) {
            c.remote_people = on;
        } else if (strcmp(argv[3], "ask_code") == 0) {
            c.ask_code = on;
        } else if (atoi(argv[3]) >= 1 && atoi(argv[3]) <= ALARM_MAX_USERS) {
            proto_srv_user_set_remote(&c, (uint8_t)atoi(argv[3]), on, 0);
        } else {
            printf("server ceiling <code> disarm|setup|people|ask_code|<user> on|off\n");
            return 1;
        }
        err = server_link_set_ceiling(&c);
    } else {
        printf("server [host <h> [port] | ca [clear] | link <code> | allow | deny | cancel |\n"
               "        unlink <code> | ceiling <code> disarm|setup|people|ask_code|<user> on|off |\n"
               "        relay <https://host> | offline <s> | phone ...]\n");
        return 1;
    }
    printf("%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

esp_err_t server_link_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "server",
        .help = "The link to an alarm-server: status, linking, the remote ceiling (server help)",
        .hint = "[host|ca|link|allow|deny|cancel|unlink|ceiling|relay|phone]",
        .func = cmd_server,
    };
    return esp_console_cmd_register(&cmd);
}

#endif
