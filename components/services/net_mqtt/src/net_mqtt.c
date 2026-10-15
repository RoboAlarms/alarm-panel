#include "sdkconfig.h"
#include "i18n/i18n.h"
#include "alarm_mem.h"
#if CONFIG_ALARM_FEATURE_MQTT
/*
 * net_mqtt.c - the panel's MQTT client (see net_mqtt.h, docs/features/10).
 *
 * One task ("mqtt") owns the client and does all publishing: it wakes on engine changes,
 * reports, broker events and once a second. The esp-mqtt event handler only records what
 * happened and queues acknowledgements and commands for the task.
 */
#include "net_mqtt.h"

#include "esp_attr.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_strings.h"
#include "alarm_proto/proto.h"
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_report.h"
#include "alarm_proto/proto_zb.h"
#include "alarm_proto/proto_zw.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net_wifi.h"
#include "nvs.h"
#include "panel_log.h"

static const char *TAG = "mqtt";

/* The board's model for discovery, set by the app before the task starts (services never
 * name a board, docs/features/33). */
static const char *s_model = "RoboAlarms panel"; /* a string that lives forever */

#define NVS_NS         "mqtt"
#define CA_MAX         4096u
#define TASK_STACK     8192u
#define TASK_PRIORITY  4
#define DEBOUNCE_MS    200u    /* MQTT-004 */
#define HEARTBEAT_MS   60000u  /* MQTT-012 */
#define REPUBLISH_MS   600000u /* MQTT-012 */
#define ACKED_SAVE_MS  5000u
#define REMOTE_SOURCE  1u      /* alarm_source_t id of MQTT commands: one lockout slot */
#define POLLS_PER_WAKE 16
#define RETRY_MS       2000u   /* after a client that wouldn't start (short of memory) */
#define RETRY_MAX_MS   60000u  /* ... doubling up to here, with jitter */

#define NB_SNAPSHOT     (1u << 0)
#define NB_REPORT       (1u << 1)
#define NB_SETTINGS     (1u << 2)
#define NB_CONNECTED    (1u << 3)
#define NB_DISCONNECTED (1u << 4)
#define NB_ACK          (1u << 5)
#define NB_COMMAND      (1u << 6)
#define NB_FIRMWARE     (1u << 7)
#define NB_DISCOVERY    (1u << 8) /* the local Home Assistant link paired or unpaired */

#define DISC_EXTRA_BASE     (1u << 0) /* trouble sensor and event entity */
#define DISC_EXTRA_FIRMWARE (1u << 1) /* update entity */
#define DISC_EXTRA_ZIGBEE   (1u << 2) /* the Zigbee sensor (ZB-026); older firmware ignores it */

typedef struct {
    char topic[PROTO_TOPIC_LEN];
    char payload[512];
    size_t len;
    bool retained;    /* the broker kept it: an old message, never run (MQTT-020) */
    alarm_ms_t at;    /* when it arrived */
} command_t;

/* What Home Assistant was told about (retained discovery), kept in NVS so deleted zones and an
 * old prefix or panel id can be retracted after a restart. */
typedef struct {
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES];
    uint8_t parts;
    uint8_t extra; /* DISC_EXTRA_* */
} disc_map_t;

/* ---- shared with the API (s_lock) ------------------------------------------------------------ */
static SemaphoreHandle_t s_lock;
static net_mqtt_settings_t s_set; /* stored settings, with the password */
static char *s_ca;                /* stored CA (PEM), NULL = none */
static bool s_skip_history;       /* just enabled: don't send what happened before */
static TaskHandle_t s_task;
static QueueHandle_t s_acks;
static QueueHandle_t s_cmds;
static QueueHandle_t s_live; /* reports while the event log is unusable (no replay then) */
static volatile bool s_connected;
static volatile net_mqtt_fail_t s_fail;
static volatile uint32_t s_sent;
static volatile bool s_waiting;

/* ---- the task's own state -------------------------------------------------------------------- */
static esp_mqtt_client_handle_t s_client;
static bool s_cfg_blocked;    /* these settings can't work (own CA missing, topic too long):
                               * only new settings help, so don't keep retrying them */
static alarm_ms_t s_retry_at; /* earliest next attempt after a start that failed for want of
                               * resources (R13); 0 = now */
static uint8_t s_retry_n;     /* consecutive such failures, for the backoff */
static net_mqtt_settings_t s_run;
static char *s_run_ca;
static proto_ident_t s_id;
static char s_avail[PROTO_TOPIC_LEN];
static char s_client_id[48];
static alarm_config_t *s_cfg;
static uint32_t s_cfg_seq;
static bool s_have_cfg;
static alarm_snapshot_t s_snap ALARM_SLOW_BSS; /* task-only: slow memory */
static char *s_buf; /* PROTO_PAYLOAD_MAX */
static proto_report_t s_rep;
static uint32_t s_part_crc[ALARM_MAX_PARTITIONS];
static uint32_t s_state_crc[ALARM_MAX_PARTITIONS];
static uint32_t s_zone_crc[ALARM_MAX_ZONES];
static uint32_t s_trbl_crc;
static uint32_t s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES + 4];
#define DISC_CRC_ZIGBEE (ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES + 3u)
static disc_map_t s_disc;
static char s_disc_prefix[PROTO_PREFIX_LEN];
static char s_disc_id[PROTO_PANEL_ID_LEN];
static bool s_comm_fail;
static net_mqtt_fw_port_t s_fw; /* set before start, read-only afterwards */
/* Z-Wave devices (MQTT-018..020): the owner's port, what it lists now, and what Home Assistant
 * was told about - kept in NVS ("zwdisc") so a device removed while the panel was off is
 * retracted after a restart. */
#define ZW_DEV_MAX 16u
static net_mqtt_zw_port_t s_zw;
static bool s_have_zw;
static proto_zw_device_t s_zw_now[ZW_DEV_MAX] ALARM_SLOW_BSS;
static size_t s_zw_n;
typedef struct {
    char key[PROTO_ZW_KEY_LEN];
    uint8_t kind;
} zw_said_t;
static zw_said_t s_zw_said[ZW_DEV_MAX];
static uint32_t s_zw_crc[ZW_DEV_MAX][3]; /* per said entry: document, state, availability */
static bool s_have_fw;
static uint32_t s_fw_crc;
/* Zigbee (MQTT-021, MQTT-022; ZB-026): the owner's port, the state as last published, the zones'
 * battery and signal as last read (by zone). */
static net_mqtt_zb_port_t s_zb;
static bool s_have_zb;
static uint32_t s_zb_crc;
static alarm_ms_t s_zb_at;
static uint32_t s_zb_changes;
static net_mqtt_zb_diag_t s_zb_diag[ALARM_MAX_ZONES] ALARM_SLOW_BSS;
static size_t s_zb_diag_n;

static void notify(uint32_t bits)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

/* HAI-010 (docs/features/23): while the local Home Assistant link is paired, discovery stays
 * retracted so the panel doesn't appear twice; everything else about MQTT is untouched. */
static volatile bool s_disc_suppressed;

void net_mqtt_suppress_discovery(bool suppressed)
{
    if (s_disc_suppressed != suppressed) {
        s_disc_suppressed = suppressed;
        notify(NB_DISCOVERY);
    }
}

/* ---- settings in NVS ------------------------------------------------------------------------- */

static void defaults(net_mqtt_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->tls = NET_MQTT_TLS_PUBLIC;
    s->discovery = true;
    s->commands = true;
    snprintf(s->prefix, sizeof(s->prefix), "%s", PROTO_PREFIX_DEFAULT);
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        /* keep the default */
    }
}

static bool get_flag(nvs_handle_t h, const char *key, bool def)
{
    uint8_t v;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v != 0u : def;
}

static void load_settings(void)
{
    defaults(&s_set);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    s_set.enabled = get_flag(h, "on", s_set.enabled);
    get_str(h, "host", s_set.host, sizeof(s_set.host));
    uint16_t port;
    if (nvs_get_u16(h, "port", &port) == ESP_OK) {
        s_set.port = port;
    }
    get_str(h, "user", s_set.user, sizeof(s_set.user));
    get_str(h, "pass", s_set.password, sizeof(s_set.password));
    uint8_t tls;
    if (nvs_get_u8(h, "tls", &tls) == ESP_OK && tls <= NET_MQTT_TLS_OWN_CA) {
        s_set.tls = tls;
    }
    s_set.discovery = get_flag(h, "disc", s_set.discovery);
    s_set.commands = get_flag(h, "cmds", s_set.commands);
    s_set.allow_panic = get_flag(h, "panic", s_set.allow_panic);
    get_str(h, "prefix", s_set.prefix, sizeof(s_set.prefix));
    get_str(h, "id", s_set.panel_id, sizeof(s_set.panel_id));
    size_t len = 0;
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

static esp_err_t store_settings(const net_mqtt_settings_t *s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u8(h, "on", s->enabled);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "host", s->host);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "port", s->port);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "user", s->user);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", s->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tls", s->tls);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "disc", s->discovery);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "cmds", s->commands);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "panic", s->allow_panic);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "prefix", s->prefix);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "id", s->panel_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool load_acked(uint32_t *out)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_u32(h, "acked", out) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

static void save_acked(uint32_t acked)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "acked", acked);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_disc(void)
{
    nvs_handle_t h;
    memset(&s_disc, 0, sizeof(s_disc));
    s_disc_prefix[0] = '\0';
    s_disc_id[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_disc);
    if (nvs_get_blob(h, "dmap", &s_disc, &len) != ESP_OK || len != sizeof(s_disc)) {
        memset(&s_disc, 0, sizeof(s_disc));
    }
    get_str(h, "dpfx", s_disc_prefix, sizeof(s_disc_prefix));
    get_str(h, "did", s_disc_id, sizeof(s_disc_id));
    nvs_close(h);
}

static void save_disc(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "dmap", &s_disc, sizeof(s_disc));
        nvs_set_str(h, "dpfx", s_disc_prefix);
        nvs_set_str(h, "did", s_disc_id);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- identity, clock, publishing ------------------------------------------------------------- */

static void make_ident(const net_mqtt_settings_t *s, proto_ident_t *id)
{
    memset(id, 0, sizeof(*id));
    snprintf(id->prefix, sizeof(id->prefix), "%s", s->prefix[0] ? s->prefix : PROTO_PREFIX_DEFAULT);
    if (s->panel_id[0] != '\0') {
        snprintf(id->panel_id, sizeof(id->panel_id), "%s", s->panel_id);
    } else {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        proto_panel_id_from_mac(mac, id->panel_id);
    }
    snprintf(id->fw, sizeof(id->fw), "%s", esp_app_get_description()->version);
    snprintf(id->model, sizeof(id->model), "%s", s_model);
}

void net_mqtt_set_model(const char *model)
{
    s_model = model != NULL ? model : "RoboAlarms panel";
}

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1735689600) { /* before 2025: the clock isn't set */
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int publish(const char *topic, const char *data, size_t len, int qos, bool retain)
{
    if (s_client == NULL || !s_connected) {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, data, (int)len, qos, retain ? 1 : 0);
}

static uint32_t crc_of(const char *s, size_t n)
{
    return alarm_crc32(0u, s, n) | 1u; /* never 0: 0 means "not published" */
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

/* ---- state (retained) ------------------------------------------------------------------------ */

/* The zones' states (MQTT-005), each with its Zigbee device's battery and signal (MQTT-022). */
static void publish_zones(bool force)
{
    s_zb_diag_n = 0u;
    if (s_have_zb) {
        if (s_zb.changes != NULL) {
            s_zb_changes = s_zb.changes(s_zb.ctx);
        }
        s_zb_diag_n = s_zb.zone_diags(s_zb.ctx, s_zb_diag, ALARM_MAX_ZONES);
        if (s_zb_diag_n > ALARM_MAX_ZONES) {
            s_zb_diag_n = ALARM_MAX_ZONES;
        }
    }
    char topic[PROTO_TOPIC_LEN];
    pj_writer_t w;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (s_cfg->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
            continue;
        }
        proto_zone_diag_t diag = {PROTO_BATTERY_UNKNOWN, PROTO_SIGNAL_UNKNOWN};
        bool fed = false;
        for (size_t i = 0; i < s_zb_diag_n; i++) {
            if (s_zb_diag[i].zone == z) {
                diag.battery = s_zb_diag[i].battery_pct;
                diag.signal = s_zb_diag[i].signal;
                fed = true;
                break;
            }
        }
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_zone_state_diag(&w, s_cfg, &s_snap, z, fed ? &diag : NULL);
        const size_t n = pj_finish(&w);
        const uint32_t crc = crc_of(s_buf, n);
        if (n > 0u && (force || crc != s_zone_crc[z - 1u]) &&
            proto_topic_zone(&s_id, z, topic, sizeof(topic)) &&
            publish(topic, s_buf, n, 1, true) >= 0) {
            s_zone_crc[z - 1u] = crc;
        }
    }
}

/* ~/zigbee/state (MQTT-021): retained, only when it changed unless forced. */
static void publish_zb_state(bool force)
{
    net_mqtt_zb_state_t st;
    if (!s_have_zb || !s_zb.state(s_zb.ctx, &st)) {
        return;
    }
    const proto_zb_state_t ps = {.radio = st.radio,
                                 .channel = st.channel,
                                 .devices = st.devices,
                                 .not_heard = st.not_heard,
                                 .jammed = st.jammed};
    pj_writer_t w;
    pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
    proto_zb_state(&w, &ps);
    const size_t n = pj_finish(&w);
    const uint32_t crc = crc_of(s_buf, n);
    char topic[PROTO_TOPIC_LEN];
    if (n > 0u && (force || crc != s_zb_crc) &&
        proto_topic(&s_id, PROTO_ZB_STATE_SUFFIX, topic, sizeof(topic)) &&
        publish(topic, s_buf, n, 1, true) >= 0) {
        s_zb_crc = crc;
    }
}

/* Once a second at most while connected (ZB-026): the network's state when it changed, and the
 * zones' battery and signal when the devices said something new. */
static void zb_sync(alarm_ms_t now)
{
    if (!s_have_zb || !s_connected || !s_have_cfg || (s_zb_at != 0 && now - s_zb_at < 1000)) {
        return;
    }
    s_zb_at = now;
    publish_zb_state(false);
    if (s_zb.changes != NULL && s_zb.changes(s_zb.ctx) != s_zb_changes) {
        publish_zones(false); /* only the zones whose payload changed go out */
    }
}

static void publish_state(bool force, alarm_ms_t now)
{
    refresh_config();
    if (!s_have_cfg) {
        return;
    }
    char topic[PROTO_TOPIC_LEN];
    pj_writer_t w;
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        if ((s_snap.partitions & (1u << (p - 1u))) == 0u) {
            continue;
        }
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_partition_status(&w, s_cfg, &s_snap, p, NULL);
        size_t n = pj_finish(&w);
        const uint32_t crc = crc_of(s_buf, n);
        if (n > 0u && (force || crc != s_part_crc[p - 1u])) {
            const proto_when_t when = {now, wall_ms()};
            pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
            proto_partition_status(&w, s_cfg, &s_snap, p, &when);
            n = pj_finish(&w);
            if (n > 0u && proto_topic_partition(&s_id, p, "status", topic, sizeof(topic)) &&
                publish(topic, s_buf, n, 1, true) >= 0) {
                s_part_crc[p - 1u] = crc;
            }
        }
        const char *ha = proto_ha_state(&s_snap.part[p - 1u]);
        const uint32_t hcrc = crc_of(ha, strlen(ha));
        if ((force || hcrc != s_state_crc[p - 1u]) &&
            proto_topic_partition(&s_id, p, "state", topic, sizeof(topic)) &&
            publish(topic, ha, strlen(ha), 1, true) >= 0) {
            s_state_crc[p - 1u] = hcrc;
        }
    }
    publish_zones(force);
    publish_zb_state(force);
    pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
    proto_troubles(&w, s_cfg, &s_snap);
    const size_t n = pj_finish(&w);
    const uint32_t crc = crc_of(s_buf, n);
    if (n > 0u && (force || crc != s_trbl_crc) && proto_topic(&s_id, "/troubles", topic, sizeof(topic)) &&
        publish(topic, s_buf, n, 1, true) >= 0) {
        s_trbl_crc = crc;
    }
}

/* ---- Home Assistant discovery (MQTT-009) ----------------------------------------------------- */

static void publish_config(const proto_ident_t *id, const char *component, const char *object,
                           const char *json, size_t len, uint32_t *crc_slot);

/* ---- Z-Wave devices the panel switches (MQTT-018..020) ---------------------------------------- */

static void zw_load_said(void)
{
    nvs_handle_t h;
    memset(s_zw_said, 0, sizeof(s_zw_said));
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_zw_said);
        if (nvs_get_blob(h, "zwdisc", s_zw_said, &len) != ESP_OK || len != sizeof(s_zw_said)) {
            memset(s_zw_said, 0, sizeof(s_zw_said));
        }
        nvs_close(h);
    }
    for (size_t i = 0; i < ZW_DEV_MAX; i++) {
        s_zw_said[i].key[PROTO_ZW_KEY_LEN - 1u] = '\0';
    }
}

static void zw_save_said(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "zwdisc", s_zw_said, sizeof(s_zw_said));
        nvs_commit(h);
        nvs_close(h);
    }
}

static const proto_zw_device_t *zw_now(const char *key)
{
    for (size_t i = 0; i < s_zw_n; i++) {
        if (strcmp(s_zw_now[i].key, key) == 0) {
            return &s_zw_now[i];
        }
    }
    return NULL;
}

/* Announce what the owner has, publish what changed, retract what is gone (or everything when
 * discovery is off). Once a second while connected; documents and states only when they change. */
static void zw_sync(void)
{
    if (!s_have_zw || !s_connected) {
        return;
    }
    s_zw_n = s_zw.list(s_zw.ctx, s_zw_now, ZW_DEV_MAX);
    const bool disc_on = s_run.discovery && !s_disc_suppressed;
    bool changed = false;
    char topic[PROTO_TOPIC_LEN];
    for (size_t j = 0; j < ZW_DEV_MAX; j++) {
        zw_said_t *s = &s_zw_said[j];
        if (s->key[0] == '\0' || (disc_on && zw_now(s->key) != NULL)) {
            continue;
        }
        const char *component = proto_zw_component(s->kind);
        if (component != NULL) {
            publish_config(&s_id, component, s->key, NULL, 0u, NULL);
        }
        if (proto_zw_topic(&s_id, s->key, "state", topic, sizeof(topic))) {
            publish(topic, "", 0u, 1, true); /* its retained state and availability go too */
        }
        if (proto_zw_topic(&s_id, s->key, "available", topic, sizeof(topic))) {
            publish(topic, "", 0u, 1, true);
        }
        memset(s, 0, sizeof(*s));
        memset(s_zw_crc[j], 0, sizeof(s_zw_crc[j]));
        changed = true;
    }
    for (size_t i = 0; disc_on && i < s_zw_n; i++) {
        const proto_zw_device_t *d = &s_zw_now[i];
        const char *component = proto_zw_component(d->kind);
        if (component == NULL) {
            continue;
        }
        size_t j = ZW_DEV_MAX;
        size_t free_j = ZW_DEV_MAX;
        for (size_t k = 0; k < ZW_DEV_MAX; k++) {
            if (strcmp(s_zw_said[k].key, d->key) == 0) {
                j = k;
            } else if (s_zw_said[k].key[0] == '\0' && free_j == ZW_DEV_MAX) {
                free_j = k;
            }
        }
        if (j == ZW_DEV_MAX) {
            if (free_j == ZW_DEV_MAX) {
                continue;
            }
            j = free_j;
            memcpy(s_zw_said[j].key, d->key, sizeof(s_zw_said[j].key)); /* the same size */
            s_zw_said[j].key[PROTO_ZW_KEY_LEN - 1u] = 0;
            s_zw_said[j].kind = d->kind;
            memset(s_zw_crc[j], 0, sizeof(s_zw_crc[j]));
            changed = true;
        } else if (s_zw_said[j].kind != d->kind) {
            publish_config(&s_id, proto_zw_component(s_zw_said[j].kind), d->key, NULL, 0u, NULL);
            s_zw_said[j].kind = d->kind; /* a relay made a garage door: another component */
            memset(s_zw_crc[j], 0, sizeof(s_zw_crc[j]));
            changed = true;
        }
        pj_writer_t w;
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_ha_zw_device(&w, &s_id, d);
        size_t n = pj_finish(&w);
        if (n > 0u) {
            publish_config(&s_id, component, d->key, s_buf, n, &s_zw_crc[j][0]);
        }
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_zw_state(&w, d);
        n = pj_finish(&w);
        uint32_t crc = crc_of(s_buf, n);
        if (n > 0u && crc != s_zw_crc[j][1] &&
            proto_zw_topic(&s_id, d->key, "state", topic, sizeof(topic)) &&
            publish(topic, s_buf, n, 1, true) >= 0) {
            s_zw_crc[j][1] = crc;
        }
        const char *avail = d->online ? "online" : "offline";
        crc = crc_of(avail, strlen(avail));
        if (crc != s_zw_crc[j][2] &&
            proto_zw_topic(&s_id, d->key, "available", topic, sizeof(topic)) &&
            publish(topic, avail, strlen(avail), 1, true) >= 0) {
            s_zw_crc[j][2] = crc;
        }
    }
    if (changed) {
        zw_save_said();
    }
}

/* ~/zwave/<key>/set: a Z-Wave device's command, parsed strictly for its kind (MQTT-020). */
static void zw_command(const command_t *cmd, const char *leaf)
{
    char key[PROTO_ZW_KEY_LEN];
    const char *start = leaf + 7; /* "/zwave/" */
    const char *slash = strchr(start, '/');
    if (!s_have_zw || slash == NULL || strcmp(slash, "/set") != 0 ||
        (size_t)(slash - start) >= sizeof(key)) {
        return;
    }
    memcpy(key, start, (size_t)(slash - start));
    key[slash - start] = '\0';
    const proto_zw_device_t *d = zw_now(key);
    int level = 0;
    const alarm_result_t r =
        d != NULL ? proto_zw_cmd_parse(cmd->payload, cmd->len, d->kind, &level) : ALARM_ERR_BAD_ARG;
    const bool taken = r == ALARM_OK && s_zw.command(s_zw.ctx, key, level, cmd->at);
    ESP_LOGI(TAG, "Home Assistant command for %s: %s", key,
             r != ALARM_OK ? "refused" : taken ? "sent" : "not taken");
}

void net_mqtt_set_zwave(const net_mqtt_zw_port_t *port)
{
    if (port != NULL && port->list != NULL && port->command != NULL) {
        s_zw = *port;
        s_have_zw = true;
    }
}

static void publish_config(const proto_ident_t *id, const char *component, const char *object,
                           const char *json, size_t len, uint32_t *crc_slot)
{
    char topic[PROTO_TOPIC_LEN];
    if (!proto_ha_topic(id, component, object, topic, sizeof(topic))) {
        return;
    }
    if (json == NULL) { /* retract */
        publish(topic, "", 0u, 1, true);
        if (crc_slot != NULL) {
            *crc_slot = 0u; /* announcing it again must send it again (B018) */
        }
        return;
    }
    const uint32_t crc = crc_of(json, len);
    if (*crc_slot == crc) {
        return;
    }
    if (publish(topic, json, len, 1, true) >= 0) {
        *crc_slot = crc;
    }
}

static bool bit_set(const uint8_t *map, unsigned i)
{
    return (map[i / 8u] & (1u << (i % 8u))) != 0u;
}

static void discovery_sync(void)
{
    if (!s_have_cfg) {
        return;
    }
    disc_map_t enrolled;
    memset(&enrolled, 0, sizeof(enrolled));
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        if (s_cfg->zones[z].type != ALARM_ZT_NOT_USED) {
            enrolled.zones[z / 8u] = (uint8_t)(enrolled.zones[z / 8u] | (1u << (z % 8u)));
        }
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (s_cfg->partitions[p].enabled) {
            enrolled.parts = (uint8_t)(enrolled.parts | (1u << p));
        }
    }
    enrolled.extra = (uint8_t)(DISC_EXTRA_BASE | (s_have_fw ? DISC_EXTRA_FIRMWARE : 0u));
    net_mqtt_zb_state_t zb;
    if (s_have_zb && s_zb.state(s_zb.ctx, &zb)) {
        enrolled.extra = (uint8_t)(enrolled.extra | DISC_EXTRA_ZIGBEE);
    }
    const bool disc_on = s_run.discovery && !s_disc_suppressed; /* HAI-010 */
    disc_map_t want = enrolled;
    if (!disc_on) {
        memset(&want, 0, sizeof(want));
    }

    /* Retract what was announced before and isn't wanted now, or was announced under another
     * prefix or panel id. */
    const bool same = s_disc_prefix[0] == '\0' ||
                      (strcmp(s_disc_prefix, s_id.prefix) == 0 && strcmp(s_disc_id, s_id.panel_id) == 0);
    proto_ident_t old = s_id;
    if (s_disc_prefix[0] != '\0') {
        snprintf(old.prefix, sizeof(old.prefix), "%s", s_disc_prefix);
        snprintf(old.panel_id, sizeof(old.panel_id), "%s", s_disc_id);
    }
    char object[8];
    char topic[PROTO_TOPIC_LEN];
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        if (bit_set(s_disc.zones, z) && (!same || !bit_set(want.zones, z))) {
            snprintf(object, sizeof(object), "z%u", z + 1u);
            publish_config(&old, "binary_sensor", object, NULL, 0u,
                           &s_disc_crc[ALARM_MAX_PARTITIONS + z]);
            if ((!same || !bit_set(enrolled.zones, z)) &&
                proto_topic_zone(&old, (uint16_t)(z + 1u), topic, sizeof(topic))) {
                publish(topic, "", 0u, 1, true); /* the zone's retained state goes too */
                s_zone_crc[z] = 0u;              /* and comes back with the zone (B018) */
            }
        }
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s_disc.parts & (1u << p)) != 0u && (!same || (want.parts & (1u << p)) == 0u)) {
            snprintf(object, sizeof(object), "p%u", p + 1u);
            publish_config(&old, "alarm_control_panel", object, NULL, 0u, &s_disc_crc[p]);
        }
    }
    if ((s_disc.extra & DISC_EXTRA_BASE) != 0u && (!same || (want.extra & DISC_EXTRA_BASE) == 0u)) {
        publish_config(&old, "binary_sensor", "trouble", NULL, 0u,
                       &s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES]);
        publish_config(&old, "event", "event", NULL, 0u,
                       &s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES + 1u]);
    }
    if ((s_disc.extra & DISC_EXTRA_FIRMWARE) != 0u &&
        (!same || (want.extra & DISC_EXTRA_FIRMWARE) == 0u)) {
        publish_config(&old, "update", "firmware", NULL, 0u,
                       &s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES + 2u]);
    }
    if ((s_disc.extra & DISC_EXTRA_ZIGBEE) != 0u &&
        (!same || (want.extra & DISC_EXTRA_ZIGBEE) == 0u)) {
        publish_config(&old, "sensor", PROTO_ZB_OBJECT, NULL, 0u, &s_disc_crc[DISC_CRC_ZIGBEE]);
    }

    /* Announce (unchanged documents aren't sent again during a connection). */
    if (disc_on) {
        const proto_ha_opts_t opts = {s_run.commands, s_cfg->opt.remote_arm_without_code,
                                      s_run.allow_panic};
        pj_writer_t w;
        for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
            if ((want.parts & (1u << (p - 1u))) == 0u) {
                continue;
            }
            pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
            proto_ha_panel(&w, &s_id, s_cfg, p, &opts);
            snprintf(object, sizeof(object), "p%u", (unsigned)p);
            const size_t n = pj_finish(&w);
            if (n > 0u) {
                publish_config(&s_id, "alarm_control_panel", object, s_buf, n, &s_disc_crc[p - 1u]);
            }
        }
        for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
            if (!bit_set(want.zones, z - 1u)) {
                continue;
            }
            pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
            proto_ha_zone(&w, &s_id, s_cfg, z);
            snprintf(object, sizeof(object), "z%u", (unsigned)z);
            const size_t n = pj_finish(&w);
            if (n > 0u) {
                publish_config(&s_id, "binary_sensor", object, s_buf, n,
                               &s_disc_crc[ALARM_MAX_PARTITIONS + z - 1u]);
            }
        }
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_ha_trouble(&w, &s_id);
        size_t n = pj_finish(&w);
        if (n > 0u) {
            publish_config(&s_id, "binary_sensor", "trouble", s_buf, n,
                           &s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES]);
        }
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_ha_event(&w, &s_id);
        n = pj_finish(&w);
        if (n > 0u) {
            publish_config(&s_id, "event", "event", s_buf, n,
                           &s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES + 1u]);
        }
        if ((want.extra & DISC_EXTRA_FIRMWARE) != 0u) {
            pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
            proto_ha_update(&w, &s_id, s_run.commands);
            n = pj_finish(&w);
            if (n > 0u) {
                publish_config(&s_id, "update", "firmware", s_buf, n,
                               &s_disc_crc[ALARM_MAX_PARTITIONS + ALARM_MAX_ZONES + 2u]);
            }
        }
        if ((want.extra & DISC_EXTRA_ZIGBEE) != 0u) {
            pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
            proto_ha_zb(&w, &s_id);
            n = pj_finish(&w);
            if (n > 0u) {
                publish_config(&s_id, "sensor", PROTO_ZB_OBJECT, s_buf, n,
                               &s_disc_crc[DISC_CRC_ZIGBEE]);
            }
        }
    }

    if (memcmp(&want, &s_disc, sizeof(want)) != 0 || !same || s_disc_prefix[0] == '\0') {
        s_disc = want;
        snprintf(s_disc_prefix, sizeof(s_disc_prefix), "%s", s_id.prefix);
        snprintf(s_disc_id, sizeof(s_disc_id), "%s", s_id.panel_id);
        save_disc();
    }
}

/* ---- firmware (OTA-010) ---------------------------------------------------------------------- */

static void publish_firmware(void)
{
    if (!s_have_fw) {
        return;
    }
    net_mqtt_fw_t fw;
    memset(&fw, 0, sizeof(fw));
    fw.percent = -1;
    s_fw.get(s_fw.ctx, &fw);
    const proto_fw_state_t st = {
        .installed = fw.installed,
        .latest = fw.latest,
        .title = "RoboAlarms Panel firmware",
        .release_url = fw.release_url,
        .in_progress = fw.in_progress,
        .percent = fw.percent,
    };
    char topic[PROTO_TOPIC_LEN];
    pj_writer_t w;
    pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
    proto_fw_state(&w, &st);
    const size_t n = pj_finish(&w);
    const uint32_t crc = n > 0u ? crc_of(s_buf, n) : 0u;
    if (n > 0u && crc != s_fw_crc && proto_topic(&s_id, "/firmware", topic, sizeof(topic)) &&
        publish(topic, s_buf, n, 1, true) >= 0) {
        s_fw_crc = crc;
    }
}

/* ---- events (MQTT-005, -006) ----------------------------------------------------------------- */

static bool marker_filter(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_REPORTED;
}

static size_t rp_read_after(void *ctx, uint32_t after, elog_entry_t *out, size_t max, uint32_t scan,
                            uint32_t *next_after)
{
    (void)ctx;
    return panel_log_read_after(after, marker_filter, NULL, out, max, scan, next_after);
}

static uint32_t rp_newest(void *ctx)
{
    (void)ctx;
    return panel_log_newest_index();
}

static int publish_event(const alarm_event_t *ev, bool replayed, int qos)
{
    if (!s_have_cfg) {
        return -1; /* the zone and user names aren't loaded: try again on the next poll */
    }
    char topic[PROTO_TOPIC_LEN];
    pj_writer_t w;
    pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
    proto_event(&w, s_cfg, ev, replayed);
    const size_t n = pj_finish(&w);
    if (n == 0u || !proto_topic(&s_id, "/event", topic, sizeof(topic))) {
        return -1;
    }
    return publish(topic, s_buf, n, qos, false);
}

static int rp_publish(void *ctx, const elog_entry_t *marker, bool replayed)
{
    (void)ctx;
    const int id = publish_event(&marker->ev, replayed, 1);
    return id > 0 ? id : -1;
}

static const proto_report_port_t REPORT_PORT = {NULL, rp_read_after, rp_newest, rp_publish};

/* Reports the event log can't stand behind: everything while the log is unusable, and single
 * ones the log couldn't keep (panel_log's lost-report hook, R12). Sent once, never replayed. */
static void publish_live(void)
{
    alarm_event_t ev;
    while (s_connected && xQueuePeek(s_live, &ev, 0) == pdTRUE) {
        if (publish_event(&ev, false, 1) < 0) {
            break;
        }
        xQueueReceive(s_live, &ev, 0);
        s_sent++;
    }
}

static uint32_t unsent_count(void)
{
    /* Markers still to publish, plus what the log couldn't keep and is waiting live (R12). */
    uint32_t count = s_live != NULL ? (uint32_t)uxQueueMessagesWaiting(s_live) : 0u;
    uint32_t after = s_rep.acked;
    elog_entry_t e[4];
    for (unsigned i = 0; i < 32u; i++) {
        uint32_t next = after;
        count += (uint32_t)panel_log_read_after(after, marker_filter, NULL, e, 4, 512, &next);
        if (next == after) {
            break;
        }
        after = next;
    }
    return count;
}

/* ---- commands (MQTT-007, -008, -014) --------------------------------------------------------- */

static void handle_command(command_t *cmd)
{
    if (!s_run.commands) {
        return;
    }
    if (cmd->retained) {
        /* MQTT-020: a message the broker kept is from some time ago - never run it (an arm, a
         * disarm or a switch someone once asked for must not happen now) */
        ESP_LOGW(TAG, "a retained command on %s was ignored", cmd->topic);
        memset(cmd, 0, sizeof(*cmd));
        return;
    }
    char base[PROTO_TOPIC_LEN];
    if (!proto_topic(&s_id, "", base, sizeof(base))) {
        return;
    }
    const size_t blen = strlen(base);
    if (strncmp(cmd->topic, base, blen) != 0) {
        return;
    }
    const char *leaf = cmd->topic + blen;
    const alarm_source_t src = {.kind = ALARM_SRC_REMOTE, .id = REMOTE_SOURCE};
    alarm_cmd_result_t res = {0};

    if (strcmp(leaf, "/cmd") == 0) {
        proto_cmd_t pc;
        alarm_result_t r = proto_cmd_parse(cmd->payload, cmd->len, s_run.allow_panic, &pc);
        if (r == ALARM_OK) {
            r = alarm_service_command(&pc.cmd, &src, &res);
        }
        ESP_LOGI(TAG, "command %s: %s", pc.action[0] ? pc.action : "?", alarm_result_name(r));
        char topic[PROTO_TOPIC_LEN];
        pj_writer_t w;
        pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
        proto_cmd_result(&w, &pc, r, r == ALARM_OK || r == ALARM_ERR_NOT_READY ? &res : NULL);
        const size_t n = pj_finish(&w);
        if (n > 0u && proto_topic(&s_id, "/cmd/result", topic, sizeof(topic))) {
            publish(topic, s_buf, n, 1, false);
        }
        memset(&pc, 0, sizeof(pc)); /* the code */
    } else if (strncmp(leaf, "/partition/", 11) == 0) {
        char *end = NULL;
        const long p = strtol(leaf + 11, &end, 10);
        if (p >= 1 && p <= ALARM_MAX_PARTITIONS && end != NULL && strcmp(end, "/set") == 0) {
            alarm_cmd_t ac;
            alarm_result_t r = proto_ha_cmd_parse(cmd->payload, cmd->len, (uint8_t)p,
                                                  s_run.allow_panic, &ac);
            if (r == ALARM_OK) {
                r = alarm_service_command(&ac, &src, &res);
            }
            ESP_LOGI(TAG, "Home Assistant command for partition %ld: %s", p, alarm_result_name(r));
            memset(&ac, 0, sizeof(ac));
        }
    } else if (strncmp(leaf, "/zwave/", 7) == 0) {
        zw_command(cmd, leaf);
    } else if (strcmp(leaf, "/firmware/set") == 0 && s_have_fw) {
        if (cmd->len == 7u && memcmp(cmd->payload, "install", 7) == 0) {
            const esp_err_t err = s_fw.install(s_fw.ctx);
            ESP_LOGI(TAG, "firmware install requested: %s", esp_err_to_name(err));
        }
    }
    memset(cmd, 0, sizeof(*cmd));
}

/* ---- the client ------------------------------------------------------------------------------ */

static net_mqtt_fail_t classify(const esp_mqtt_error_codes_t *e)
{
    if (e == NULL) {
        return NET_MQTT_FAIL_OTHER;
    }
    if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        return (e->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                e->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)
                   ? NET_MQTT_FAIL_AUTH
                   : NET_MQTT_FAIL_OTHER;
    }
    if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        const esp_err_t t = e->esp_tls_last_esp_err;
        if (t == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME || t == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST ||
            t == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || t == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET) {
            return NET_MQTT_FAIL_UNREACHABLE;
        }
        if (e->esp_tls_cert_verify_flags != 0 || t == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED ||
            t == ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED) {
            return NET_MQTT_FAIL_TLS;
        }
        return t == ESP_OK ? NET_MQTT_FAIL_UNREACHABLE : NET_MQTT_FAIL_OTHER;
    }
    return NET_MQTT_FAIL_OTHER;
}

/* Runs in the esp-mqtt task: record and hand over, nothing else. */
static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t ev = data;
    static command_t cmd; /* only this task writes it; the queue copies it */
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_fail = NET_MQTT_FAIL_NONE;
        notify(NB_CONNECTED);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        notify(NB_DISCONNECTED);
        break;
    case MQTT_EVENT_PUBLISHED:
        xQueueSend(s_acks, &ev->msg_id, 0);
        notify(NB_ACK);
        break;
    case MQTT_EVENT_DATA:
        if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len &&
            ev->topic_len > 0 && (size_t)ev->topic_len < sizeof(cmd.topic) &&
            (size_t)ev->data_len < sizeof(cmd.payload)) {
            memcpy(cmd.topic, ev->topic, (size_t)ev->topic_len);
            cmd.topic[ev->topic_len] = '\0';
            memcpy(cmd.payload, ev->data, (size_t)ev->data_len);
            cmd.payload[ev->data_len] = '\0';
            cmd.len = (size_t)ev->data_len;
            cmd.retained = ev->retain;
            cmd.at = alarm_service_now();
            if (xQueueSend(s_cmds, &cmd, 0) != pdTRUE) {
                ESP_LOGW(TAG, "too many commands at once: one on %s dropped", cmd.topic);
            }
            memset(&cmd, 0, sizeof(cmd));
            notify(NB_COMMAND);
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
        publish(s_avail, "offline", 7u, 1, true); /* a clean disconnect skips the will */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
}

/* R13: esp-mqtt refusing to start is nearly always a shortage of memory or a task slot, not a
 * mistake in the settings. Keep trying, backing off to a minute with jitter (several panels
 * coming back after a brownout shouldn't retry in lockstep), and leave the failure visible in
 * the status; after 5 minutes without the broker the comm_fail trouble follows on its own. */
static void retry_later(alarm_ms_t now)
{
    uint32_t delay = RETRY_MS;
    for (uint8_t i = 0; i < s_retry_n && delay < RETRY_MAX_MS; i++) {
        delay *= 2u;
    }
    if (delay > RETRY_MAX_MS) {
        delay = RETRY_MAX_MS;
    }
    delay += esp_random() % (delay / 4u);
    if (s_retry_n < 255u) {
        s_retry_n++;
    }
    s_retry_at = now + delay;
    s_fail = NET_MQTT_FAIL_OTHER;
    ESP_LOGW(TAG, "the client wouldn't start (attempt %u); trying again in %" PRIu32 " ms",
             (unsigned)s_retry_n, delay);
}

static void client_start(alarm_ms_t now)
{
    make_ident(&s_run, &s_id);
    if (!proto_topic(&s_id, "/availability", s_avail, sizeof(s_avail))) {
        s_cfg_blocked = true; /* the prefix and panel id don't fit a topic: change them */
        s_fail = NET_MQTT_FAIL_OTHER;
        ESP_LOGE(TAG, "topic prefix and panel id are too long together");
        return;
    }
    snprintf(s_client_id, sizeof(s_client_id), "alarmpanel-%s", s_id.panel_id);
    const bool tls = s_run.tls != NET_MQTT_TLS_OFF;
    esp_mqtt_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.broker.address.hostname = s_run.host;
    cfg.broker.address.port = s_run.port != 0u ? s_run.port : (tls ? 8883u : 1883u);
    cfg.broker.address.transport = tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    if (s_run.tls == NET_MQTT_TLS_PUBLIC) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    } else if (s_run.tls == NET_MQTT_TLS_OWN_CA) {
        if (s_run_ca == NULL) {
            s_fail = NET_MQTT_FAIL_TLS;
            s_cfg_blocked = true; /* nothing to verify with: retrying would change nothing */
            ESP_LOGW(TAG, "own certificate authority chosen but none loaded (console: mqtt ca)");
            return;
        }
        cfg.broker.verification.certificate = s_run_ca;
    }
    cfg.credentials.client_id = s_client_id;
    cfg.credentials.username = s_run.user[0] ? s_run.user : NULL;
    cfg.credentials.authentication.password = s_run.password[0] ? s_run.password : NULL;
    cfg.session.last_will.topic = s_avail; /* MQTT-003 */
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.msg_len = 7;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.session.keepalive = 30;
    cfg.network.reconnect_timeout_ms = 10000;
    cfg.network.timeout_ms = 10000;
    cfg.buffer.size = PROTO_PAYLOAD_MAX;
    cfg.buffer.out_size = PROTO_PAYLOAD_MAX;
    cfg.outbox.limit = 64u * 1024u; /* unacknowledged messages kept in RAM; the log keeps events */
    cfg.task.stack_size = 6144;
    cfg.task.priority = 5;
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        retry_later(now);
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        esp_mqtt_client_destroy(s_client); /* client, its task and the outbox all go back */
        s_client = NULL;
        retry_later(now);
        return;
    }
    ESP_LOGI(TAG, "connecting to %s:%" PRIu32 " (%s) as %s", s_run.host, cfg.broker.address.port,
             tls ? "TLS" : "not encrypted", s_client_id);
}

static void on_connected(alarm_ms_t now)
{
    ESP_LOGI(TAG, "connected; topics under %s/%s", s_id.prefix, s_id.panel_id);
    s_retry_n = 0; /* the next shortage starts from the short delay again */
    publish(s_avail, "online", 6u, 1, true);
    if (s_run.commands) {
        char topic[PROTO_TOPIC_LEN];
        if (proto_topic(&s_id, "/partition/+/set", topic, sizeof(topic))) {
            esp_mqtt_client_subscribe_single(s_client, topic, 1);
        }
        if (proto_topic(&s_id, "/cmd", topic, sizeof(topic))) {
            esp_mqtt_client_subscribe_single(s_client, topic, 1);
        }
        if (s_have_fw && proto_topic(&s_id, "/firmware/set", topic, sizeof(topic))) {
            esp_mqtt_client_subscribe_single(s_client, topic, 1);
        }
        if (s_have_zw && proto_topic(&s_id, "/zwave/+/set", topic, sizeof(topic))) {
            esp_mqtt_client_subscribe_single(s_client, topic, 1);
        }
    }
    memset(s_zw_crc, 0, sizeof(s_zw_crc)); /* everything again on a new connection */
    memset(s_disc_crc, 0, sizeof(s_disc_crc));
    memset(s_part_crc, 0, sizeof(s_part_crc));
    memset(s_state_crc, 0, sizeof(s_state_crc));
    memset(s_zone_crc, 0, sizeof(s_zone_crc));
    s_trbl_crc = 0;
    s_fw_crc = 0;
    s_zb_crc = 0;
    refresh_config();
    discovery_sync();
    publish_state(true, now);
    publish_firmware();
    zw_sync();
    panel_log_flush(500); /* what became due before the connection counts as replayed */
    proto_report_connected(&s_rep, now);
}

static void apply_settings(alarm_ms_t now)
{
    net_mqtt_settings_t next;
    char *ca = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    next = s_set;
    if (s_ca != NULL) {
        ca = strdup(s_ca);
    }
    const bool skip = s_skip_history;
    s_skip_history = false;
    xSemaphoreGive(s_lock);

    client_stop();
    s_run = next;
    free(s_run_ca);
    s_run_ca = ca;
    s_cfg_blocked = false; /* R13: a settings change always gets one immediate attempt */
    s_retry_at = 0;
    s_retry_n = 0;
    s_fail = NET_MQTT_FAIL_NONE;
    make_ident(&s_run, &s_id);
    if (skip) {
        xQueueReset(s_live); /* nothing from before counts now */
        s_rep.acked = panel_log_newest_index(); /* only what happens from now on */
        s_rep.acked_dirty = true;
    }
    proto_report_disconnected(&s_rep, now);
    proto_report_enable(&s_rep, s_run.enabled && s_run.host[0] != '\0', now);
    net_wifi_set_required(NET_WIFI_REQ_MQTT, s_run.enabled);
}

/* ---- the task -------------------------------------------------------------------------------- */

static void mqtt_task(void *arg)
{
    (void)arg;
    uint32_t acked = 0;
    if (!load_acked(&acked)) {
        acked = panel_log_newest_index(); /* first start: the history stays in the panel */
        save_acked(acked);
    }
    load_disc();
    zw_load_said();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_run = s_set;
    s_run_ca = s_ca != NULL ? strdup(s_ca) : NULL;
    xSemaphoreGive(s_lock);
    make_ident(&s_run, &s_id);
    proto_report_init(&s_rep, &REPORT_PORT, acked, s_run.enabled && s_run.host[0] != '\0',
                      alarm_service_now());
    net_wifi_set_required(NET_WIFI_REQ_MQTT, s_run.enabled);

    alarm_ms_t heartbeat_at = 0;
    alarm_ms_t republish_at = 0;
    alarm_ms_t state_due = 0;
    alarm_ms_t acked_saved_at = 0;
    bool state_pending = true;
    bool more = false;

    for (;;) {
        uint32_t bits = 0;
        const TickType_t wait = more ? 0 : pdMS_TO_TICKS(state_pending ? DEBOUNCE_MS : 1000u);
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        const alarm_ms_t now = alarm_service_now();
        more = false;

        if ((bits & NB_SETTINGS) != 0u) {
            apply_settings(now);
        }
        net_wifi_status_t ws;
        net_wifi_get_status(&ws);
        if (s_client == NULL && !s_cfg_blocked && now >= s_retry_at && s_run.enabled &&
            s_run.host[0] != '\0' && ws.state == NET_WIFI_CONNECTED) {
            client_start(now);
        }
        if ((bits & NB_CONNECTED) != 0u && s_connected) {
            on_connected(now);
            heartbeat_at = now;
            republish_at = now + REPUBLISH_MS;
        }
        if ((bits & NB_DISCONNECTED) != 0u && !s_connected) {
            proto_report_disconnected(&s_rep, now);
        }
        if ((bits & NB_SNAPSHOT) != 0u && !state_pending) {
            state_pending = true;
            state_due = now + DEBOUNCE_MS;
        }
        int msg_id;
        while (xQueueReceive(s_acks, &msg_id, 0) == pdTRUE) {
            const uint32_t before = s_rep.acked;
            proto_report_acked(&s_rep, msg_id, now);
            if (s_rep.acked != before) {
                s_sent++;
            }
        }
        command_t cmd;
        while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
            handle_command(&cmd);
        }

        if (s_connected) {
            if (state_pending && now >= state_due) {
                const uint32_t cfg_seq = s_cfg_seq;
                publish_state(false, now);
                if (s_cfg_seq != cfg_seq) {
                    discovery_sync(); /* names, zones or options changed */
                }
                state_pending = false;
            }
            if ((bits & NB_FIRMWARE) != 0u) {
                publish_firmware();
            }
            if ((bits & NB_DISCOVERY) != 0u) {
                discovery_sync(); /* while disconnected, the next connect does this anyway */
            }
            refresh_config();
            zw_sync(); /* the Z-Wave devices' states (MQTT-019): only what changed goes out */
            zb_sync(now); /* the Zigbee network and its zones' devices (MQTT-021, MQTT-022) */
            if (panel_log_ready()) {
                if ((bits & NB_REPORT) != 0u) {
                    panel_log_flush(300);
                }
                for (int i = 0; i < POLLS_PER_WAKE; i++) {
                    if (!proto_report_poll(&s_rep, now)) {
                        break;
                    }
                    more = i == POLLS_PER_WAKE - 1;
                }
            }
            /* R12: reports the log couldn't keep go out here, with or without a log, so a lost
             * marker can't take an alarm report with it. */
            publish_live();
            if (now >= heartbeat_at) {
                net_wifi_status_t hw;
                net_wifi_get_status(&hw);
                const proto_heartbeat_t hb = {
                    .uptime_s = (uint64_t)(esp_timer_get_time() / 1000000),
                    .fw = s_id.fw,
                    .ip = hw.ip,
                    .rssi = hw.rssi,
                    .free_heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024u),
                    .boot = panel_log_boot(),
                    .unsent = unsent_count(),
                };
                char topic[PROTO_TOPIC_LEN];
                pj_writer_t w;
                pj_init(&w, s_buf, PROTO_PAYLOAD_MAX);
                proto_heartbeat(&w, &hb);
                const size_t n = pj_finish(&w);
                if (n > 0u && proto_topic(&s_id, "/heartbeat", topic, sizeof(topic))) {
                    publish(topic, s_buf, n, 0, false);
                }
                heartbeat_at = now + HEARTBEAT_MS;
            }
            if (now >= republish_at) { /* MQTT-012: retained status at least every 10 min */
                memset(s_part_crc, 0, sizeof(s_part_crc));
                for (size_t j = 0; j < ZW_DEV_MAX; j++) {
                    s_zw_crc[j][1] = 0u; /* the devices' states too */
                    s_zw_crc[j][2] = 0u;
                }
                s_trbl_crc = 0;
                s_fw_crc = 0;
                publish_firmware();
                state_pending = true;
                state_due = now;
                republish_at = now + REPUBLISH_MS;
            }
        }
        if (s_rep.acked_dirty && now - acked_saved_at >= ACKED_SAVE_MS) {
            save_acked(s_rep.acked);
            s_rep.acked_dirty = false;
            acked_saved_at = now;
        }
        const bool fail = proto_report_comm_fail(&s_rep, now); /* MQTT-011 */
        if (fail != s_comm_fail) {
            s_comm_fail = fail;
            alarm_service_system_trouble(ALARM_TRBL_COMM_FAIL, fail);
        }
        s_waiting = proto_report_waiting(&s_rep) ||
                    (s_live != NULL && uxQueueMessagesWaiting(s_live) != 0u);
    }
}

/* ---- listener -------------------------------------------------------------------------------- */

static void on_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    (void)snap;
    notify(NB_SNAPSHOT);
}

static void on_report(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (!panel_log_ready() && s_live != NULL) {
        xQueueSend(s_live, ev, 0);
    }
    notify(NB_REPORT);
}

/* R12: the event log couldn't keep this report's marker, so nothing will ever replay it.
 * Take it over and publish it live instead - exactly one of the two paths owns a report, so
 * the broker never sees it twice. Runs on the alarm task or the log's writer task: no
 * blocking, no engine calls. Returning false leaves the log to count it as never delivered. */
static bool on_report_lost(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (s_live == NULL || xQueueSend(s_live, ev, 0) != pdTRUE) {
        return false;
    }
    notify(NB_REPORT);
    return true;
}

static const alarm_listener_t s_listener = {
    .on_report = on_report,
    .on_snapshot = on_snapshot,
};

const alarm_listener_t *net_mqtt_listener(void)
{
    return &s_listener;
}

/* ---- API ------------------------------------------------------------------------------------- */

void net_mqtt_set_zigbee(const net_mqtt_zb_port_t *port)
{
    if (s_task == NULL && port != NULL && port->state != NULL && port->zone_diags != NULL) {
        s_zb = *port;
        s_have_zb = true;
    }
}

void net_mqtt_set_firmware(const net_mqtt_fw_port_t *port)
{
    if (s_task == NULL && port != NULL && port->get != NULL && port->install != NULL) {
        s_fw = *port;
        s_have_fw = true;
    }
}

void net_mqtt_firmware_changed(void)
{
    notify(NB_FIRMWARE);
}

/* Gives back everything net_mqtt_start() built, so a failed start leaks nothing and can be
 * tried again (R07, R13). */
static void start_failed(SemaphoreHandle_t lock, QueueHandle_t acks, QueueHandle_t cmds,
                         QueueHandle_t live, char *buf, alarm_config_t *cfg)
{
    panel_log_remove_lost_handler(on_report_lost, NULL);
    s_lock = NULL;
    s_acks = NULL;
    s_cmds = NULL;
    s_live = NULL;
    s_buf = NULL;
    s_cfg = NULL;
    free(s_ca);
    s_ca = NULL;
    if (lock != NULL) {
        vSemaphoreDelete(lock);
    }
    if (acks != NULL) {
        vQueueDelete(acks);
    }
    if (cmds != NULL) {
        vQueueDeleteWithCaps(cmds);
    }
    if (live != NULL) {
        vQueueDelete(live);
    }
    free(buf);
    free(cfg);
}

esp_err_t net_mqtt_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    QueueHandle_t acks = xQueueCreate(32, sizeof(int));
    /* esp-mqtt calls on_mqtt_event() holding the lock this task needs to publish each result,
     * so a burst (an automation arming several partitions) can't be waited out there: it has to
     * fit. ~630 bytes a command, in slow memory (only tasks touch it). */
    QueueHandle_t cmds = xQueueCreateWithCaps(alarm_mem_has_psram() ? 16u : 4u, sizeof(command_t),
                                              alarm_mem_slow_caps());
    QueueHandle_t live = xQueueCreate(16, sizeof(alarm_event_t));
    char *buf = alarm_mem_slow_malloc(PROTO_PAYLOAD_MAX);
    alarm_config_t *cfg = alarm_mem_slow_malloc(sizeof(alarm_config_t));
    if (lock == NULL || acks == NULL || cmds == NULL || live == NULL || buf == NULL ||
        cfg == NULL) {
        start_failed(lock, acks, cmds, live, buf, cfg);
        return ESP_ERR_NO_MEM;
    }
    s_lock = lock;
    s_acks = acks;
    s_cmds = cmds;
    s_live = live;
    s_buf = buf;
    s_cfg = cfg;
    load_settings();
    (void)panel_log_add_lost_handler(on_report_lost, NULL); /* R12 */
    if (xTaskCreatePinnedToCore(mqtt_task, "mqtt", TASK_STACK, NULL, TASK_PRIORITY, &s_task,
                                tskNO_AFFINITY) != pdPASS) {
        s_task = NULL;
        start_failed(lock, acks, cmds, live, buf, cfg);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s", s_set.enabled ? "enabled" : "off (Settings > Integrations > MQTT)");
    return ESP_OK;
}

void net_mqtt_get_settings(net_mqtt_settings_t *out)
{
    if (s_lock == NULL) {
        defaults(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_set;
    xSemaphoreGive(s_lock);
    out->password_set = out->password[0] != '\0';
    memset(out->password, 0, sizeof(out->password));
}

static bool host_ok(const char *host)
{
    for (const char *p = host; *p != '\0'; p++) {
        if (*p <= ' ' || *p > '~' || *p == '/' || *p == '@') {
            return false;
        }
    }
    return true;
}

/* Its fixed answers are marked I18N_NOOP_C("why", ...): the Panel Manager gets the English, the
 * touchscreen looks the sentence up in its language (docs/features/37). */
static void reason(char *why, size_t len, const char *text)
{
    if (why != NULL && len > 0u) {
        snprintf(why, len, "%s", text);
    }
}

esp_err_t net_mqtt_set_settings(const net_mqtt_settings_t *in, bool change_password, char *why,
                                size_t why_len)
{
    if (s_lock == NULL) {
        reason(why, why_len, I18N_NOOP_C("why", "Not started"));
        return ESP_ERR_INVALID_STATE;
    }
    if (!host_ok(in->host)) {
        reason(why, why_len, I18N_NOOP_C("why", "An address has no spaces, like homeassistant.local"));
        return ESP_ERR_INVALID_ARG;
    }
    if (in->enabled && in->host[0] == '\0') {
        reason(why, why_len, I18N_NOOP_C("why", "Set the broker's address first"));
        return ESP_ERR_INVALID_ARG;
    }
    if (in->prefix[0] != '\0' && !proto_prefix_valid(in->prefix)) {
        reason(why, why_len, I18N_NOOP_C("why", "Letters, digits, - and _ only, words split by /"));
        return ESP_ERR_INVALID_ARG;
    }
    if (in->panel_id[0] != '\0' && !proto_panel_id_valid(in->panel_id)) {
        reason(why, why_len, I18N_NOOP_C("why", "Lowercase letters, digits, - and _ only"));
        return ESP_ERR_INVALID_ARG;
    }
    if (in->tls > NET_MQTT_TLS_OWN_CA) {
        reason(why, why_len, I18N_NOOP_C("why", "Unknown security setting"));
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    net_mqtt_settings_t next = s_set;
    if (in->enabled && !s_set.enabled) {
        s_skip_history = true;
    }
    next.enabled = in->enabled;
    snprintf(next.host, sizeof(next.host), "%s", in->host);
    next.port = in->port;
    snprintf(next.user, sizeof(next.user), "%s", in->user);
    if (change_password) {
        snprintf(next.password, sizeof(next.password), "%s", in->password);
    }
    next.tls = in->tls;
    next.discovery = in->discovery;
    next.commands = in->commands;
    next.allow_panic = in->allow_panic;
    snprintf(next.prefix, sizeof(next.prefix), "%s",
             in->prefix[0] ? in->prefix : PROTO_PREFIX_DEFAULT);
    snprintf(next.panel_id, sizeof(next.panel_id), "%s", in->panel_id);
    const esp_err_t err = store_settings(&next);
    if (err == ESP_OK) {
        s_set = next;
    }
    memset(&next, 0, sizeof(next));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        reason(why, why_len, I18N_NOOP_C("why", "Couldn't save the settings"));
        return err;
    }
    notify(NB_SETTINGS);
    return ESP_OK;
}

void net_mqtt_get_status(net_mqtt_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool enabled = s_set.enabled && s_set.host[0] != '\0';
    const bool tls = s_set.tls != NET_MQTT_TLS_OFF;
    const unsigned port = s_set.port != 0u ? s_set.port : (tls ? 8883u : 1883u);
    snprintf(out->broker, sizeof(out->broker), "%s:%u", s_set.host, port);
    out->ca_loaded = s_ca != NULL;
    xSemaphoreGive(s_lock);
    snprintf(out->panel_id, sizeof(out->panel_id), "%s", s_id.panel_id);
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    out->fail = s_fail;
    out->sent = s_sent;
    out->waiting = s_waiting;
    if (!enabled) {
        out->state = NET_MQTT_OFF;
    } else if (ws.state != NET_WIFI_CONNECTED) {
        out->state = NET_MQTT_NO_NETWORK;
    } else if (s_connected) {
        out->state = NET_MQTT_CONNECTED;
        out->fail = NET_MQTT_FAIL_NONE;
    } else {
        out->state = s_fail != NET_MQTT_FAIL_NONE ? NET_MQTT_FAILED : NET_MQTT_CONNECTING;
    }
}

esp_err_t net_mqtt_set_ca(const char *pem)
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
    xSemaphoreGive(s_lock);
    notify(NB_SETTINGS);
    return ESP_OK;
}

/* ---- service console ------------------------------------------------------------------------- */

static const char *const STATE_TEXT[] = {"off", "waiting for Wi-Fi", "connecting", "connected",
                                         "failed"};
static const char *const FAIL_TEXT[] = {"", "unreachable", "user name or password refused",
                                        "certificate not trusted", "other"};
static const char *const TLS_TEXT[] = {"off (home network only)", "public certificate",
                                       "own certificate authority"};

static bool on_off(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
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
    const esp_err_t err = net_mqtt_set_ca(pem);
    printf("%s\n", err == ESP_OK ? "certificate stored" : esp_err_to_name(err));
    free(pem);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_mqtt(int argc, char **argv)
{
    net_mqtt_settings_t s;
    net_mqtt_get_settings(&s);
    if (argc < 2) {
        net_mqtt_status_t st;
        net_mqtt_get_status(&st);
        printf("%s%s%s, broker %s, user '%s', password %s, security %s%s\n",
               STATE_TEXT[st.state], st.fail != NET_MQTT_FAIL_NONE ? ": " : "",
               FAIL_TEXT[st.fail], st.broker, s.user, s.password_set ? "set" : "none",
               TLS_TEXT[s.tls], st.ca_loaded ? " (CA loaded)" : "");
        printf("topics %s/%s, discovery %s, commands %s, panic %s, %" PRIu32 " events sent%s\n",
               s.prefix, st.panel_id, s.discovery ? "on" : "off", s.commands ? "on" : "off",
               s.allow_panic ? "on" : "off", st.sent, st.waiting ? ", some waiting" : "");
        return 0;
    }
    bool change_password = false;
    bool flag;
    const char *what = argv[1];
    if (on_off(what, &flag)) {
        s.enabled = flag;
    } else if (strcmp(what, "broker") == 0 && argc >= 3) {
        snprintf(s.host, sizeof(s.host), "%s", argv[2]);
        s.port = argc >= 4 ? (uint16_t)atoi(argv[3]) : 0u;
    } else if (strcmp(what, "user") == 0 && argc >= 3) {
        snprintf(s.user, sizeof(s.user), "%s", strcmp(argv[2], "-") == 0 ? "" : argv[2]);
        if (argc >= 4) {
            snprintf(s.password, sizeof(s.password), "%s", argv[3]);
            change_password = true;
        }
    } else if (strcmp(what, "password") == 0) {
        snprintf(s.password, sizeof(s.password), "%s", argc >= 3 ? argv[2] : "");
        change_password = true;
    } else if (strcmp(what, "tls") == 0 && argc >= 3) {
        if (strcmp(argv[2], "off") == 0) {
            s.tls = NET_MQTT_TLS_OFF;
        } else if (strcmp(argv[2], "public") == 0) {
            s.tls = NET_MQTT_TLS_PUBLIC;
        } else if (strcmp(argv[2], "ca") == 0) {
            s.tls = NET_MQTT_TLS_OWN_CA;
        } else {
            printf("tls off | public | ca\n");
            return 1;
        }
    } else if (strcmp(what, "ca") == 0) {
        if (argc >= 3 && strcmp(argv[2], "clear") == 0) {
            return net_mqtt_set_ca(NULL) == ESP_OK ? 0 : 1;
        }
        return read_ca();
    } else if (strcmp(what, "discovery") == 0 && argc >= 3 && on_off(argv[2], &flag)) {
        s.discovery = flag;
    } else if (strcmp(what, "commands") == 0 && argc >= 3 && on_off(argv[2], &flag)) {
        s.commands = flag;
    } else if (strcmp(what, "panic") == 0 && argc >= 3 && on_off(argv[2], &flag)) {
        s.allow_panic = flag;
    } else if (strcmp(what, "prefix") == 0 && argc >= 3) {
        snprintf(s.prefix, sizeof(s.prefix), "%s", argv[2]);
    } else if (strcmp(what, "id") == 0 && argc >= 3) {
        snprintf(s.panel_id, sizeof(s.panel_id), "%s", strcmp(argv[2], "auto") == 0 ? "" : argv[2]);
    } else {
        printf("mqtt [on|off | broker <host> [port] | user <name|-> [password] | password [p] |\n"
               "      tls off|public|ca | ca [clear] | discovery on|off | commands on|off |\n"
               "      panic on|off | prefix <p> | id <id|auto>]\n");
        return 1;
    }
    char why[96] = "";
    const esp_err_t err = net_mqtt_set_settings(&s, change_password, why, sizeof(why));
    memset(&s, 0, sizeof(s));
    if (err != ESP_OK) {
        printf("%s\n", why[0] ? why : esp_err_to_name(err));
        return 1;
    }
    printf("saved\n");
    return 0;
}

esp_err_t net_mqtt_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "mqtt",
        .help = "MQTT broker: status and settings (mqtt help)",
        .hint = "[on|off|broker|user|password|tls|ca|discovery|commands|panic|prefix|id]",
        .func = cmd_mqtt,
    };
    return esp_console_cmd_register(&cmd);
}

#endif
