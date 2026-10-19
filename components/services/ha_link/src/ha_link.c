#include "sdkconfig.h"
#include "alarm_mem.h"
#include "esp_attr.h"
#if CONFIG_ALARM_FEATURE_HA
/*
 * ha_link.c - the local Home Assistant link (docs/features/23, M12): mDNS advertisement, one TLS
 * connection at a time on port 6054, hello, and pairing driven by the host-tested state
 * machine in alarm_proto (proto_pair). State, events and commands follow in the next step.
 *
 * One task owns everything: the listening socket, the TLS session and the pairing state.
 * The UI and the console only read ha_link_get_status() and set request flags; the task
 * picks them up within half a second (its poll interval).
 *
 * Three rules hold this file together:
 *  - Nothing the client does may own the link task. Every read and every write has a
 *    deadline, looks at the drop flag while it waits, and gives the scheduler its turn
 *    (HAI-002; "One connection, bounded" below).
 *  - Other tasks reach the link task through FreeRTOS queues and atomic flags only, never
 *    through plain shared memory. The alarm listener must stay quick and must not block,
 *    so every hand-over uses a zero timeout and drops rather than waits.
 *  - What is paired is only what reached flash. Pairing and unpairing report whether they
 *    were stored, and a revocation that could not be stored keeps being retried.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_pair.h"
#include "alarm_service.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i18n/i18n.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h" /* select() over the listening socket, SO_SNDTIMEO on the client */
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mdns.h"
#include "net_mqtt.h"
#include "net_wifi.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "panel_voice.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "device_service.h"
#include "zone_service.h"

#include "ha_link.h"
#include "ha_link_internal.h"

#if !defined(CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
#error "The Home Assistant link requires retained peer certificates for fingerprint pinning"
#endif

#if CONFIG_HA_LINK_DIAGNOSTICS
static bool ha_test_store_fail;
#endif

_Static_assert(ZSRC_REF_LEN_LINK == ZSRC_REF_LEN, "one entity_id length across the link");

static const char *TAG = "ha_link";

#define POLL_MS       500u  /* the task looks at flags and deadlines this often */
#define HANDSHAKE_MS  8000u /* a TLS handshake that takes longer is abandoned */
#define IDLE_LIMIT_MS 90000u /* protocol.md: drop a connection silent for 90 s */
#define RX_MAX        PROTO_LINK_FRAME_MAX
#define EV_QUEUE      32 /* events waiting for the link task (the alarm task posts them) */
#define CALL_QUEUE    8  /* device actions waiting to be sent (the TLS session is one task's) */
#define SET_ANSWERS   (2u * PROTO_LINK_SET_QUEUE) /* settings answers waiting for the link task */
#define PREFS_GIVE_UP_MS 3000u /* panel_prefs.h: a patch the screen hasn't taken in 3 s is given
                                * up on (UI_PREFS_TAKE_MS) */

/* One connection, bounded (HAI-002). A client that sends half a frame header, or stops
 * reading while the panel writes, must not be able to keep the link task for itself: it would
 * block reconnection, sensor updates, switching the integration off and unpairing. */
#define TX_SOCK_MS   2000u  /* SO_SNDTIMEO: lwIP hands a stalled write back this often */
#define TX_STALL_MS  10000u /* no byte accepted for this long: the client stopped reading */
#define TX_FRAME_MS  30000u /* and no single frame may take longer than this end to end */
#define RX_FRAME_MS  10000u /* a started header, then its body, each within this */
#define REVOKE_RETRY_MS 5000u /* how often an unpair that missed flash is tried again */

typedef struct {
    char entity[ZSRC_REF_LEN];
    char action[20];
    char mode[24];
    int32_t value;
    uint32_t request_id;
    bool has_value;
} ha_call_t;

/* A settings_set's answer, from whichever task has it (HAI-019): the screen's task for the
 * master's own settings, the panel link's for a keypad's. */
typedef struct {
    uint32_t ticket;
    uint8_t err;                     /* ha_link_set_t: HA_LINK_SET_SENT = the target answered */
    uint8_t why[PROTO_PREFS_F_LAST]; /* then its refusals, by id - 1 */
} ha_set_answer_t;

typedef struct {
    bool enabled;
    bool paired;
    uint8_t pinned_fp[PROTO_LINK_FP_LEN];
    char client[PROTO_LINK_CLIENT_LEN]; /* the paired client's hello string */
} ha_store_t;

/* The stored trust record, NVS "halink"/"trust". One blob written in one call, so a pairing
 * or a revocation is never half on flash, and it is what store_load() believes: a record
 * that says "not paired" cannot be talked round by anything else in the namespace.
 *
 * A stored contract like the rest (CONTRIBUTING.md): never renumber or reuse a field, only add, and
 * only in the reserved bytes. An older firmware (before this record) reads "peer"/"client"
 * instead, which is why store_pin() still writes them and store_unpair() erases them. */
#define HA_TRUST_VERSION 1u

typedef struct {
    uint8_t version;     /* HA_TRUST_VERSION; anything else is ignored */
    uint8_t paired;      /* 1 = the fingerprint below is trusted, 0 = revoked */
    uint8_t reserved[2]; /* new flags go here */
    uint32_t generation; /* +1 per change; for the log and for reading a flash dump */
    uint8_t fp[PROTO_LINK_FP_LEN];
    char client[PROTO_LINK_CLIENT_LEN];
} ha_trust_t;

/* The layout is on flash: a reorder would make an older panel read the wrong field, so the
 * build fails instead. Add fields in the reserved bytes and these stay true. */
_Static_assert(sizeof(ha_trust_t) == 88u, "the stored trust record is a fixed 88 bytes");
_Static_assert(offsetof(ha_trust_t, version) == 0u, "stored layout: version");
_Static_assert(offsetof(ha_trust_t, paired) == 1u, "stored layout: paired");
_Static_assert(offsetof(ha_trust_t, generation) == 4u, "stored layout: generation");
_Static_assert(offsetof(ha_trust_t, fp) == 8u, "stored layout: fingerprint");
_Static_assert(offsetof(ha_trust_t, client) == 40u, "stored layout: client name");

static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    nvs_handle_t nvs;
    ha_store_t store;
    uint32_t generation;    /* the stored record's, under S.lock with store */
    bool revoke_pending;    /* atomic: an unpair the flash hasn't taken yet */
    uint64_t revoke_next;   /* link task only: when to try storing it again */
    proto_ident_t ident;
    char name[ALARM_NAME_LEN]; /* partition 1's name, refreshed per connection */

    /* what the task serves (the certificate and key bytes themselves are in SB) */
    size_t cert_len;
    size_t key_len;
    uint8_t own_fp[PROTO_LINK_FP_LEN];

    char pairing_client[PROTO_LINK_CLIENT_LEN]; /* hello of the connection now pairing */
    bool connected; /* atomic: the paired client is on the line right now */

    /* state pushed to the paired client (only the link task touches these; the big copies
     * are in SB) */
    bool synced;           /* the client holds a full snapshot */
    uint32_t zw_crc;       /* the Z-Wave device list the client holds (0: none sent yet) */
    uint64_t zw_next;      /* when to look at it again */
    uint64_t status_next;
    uint64_t status_sent;  /* when the last status went */
    uint32_t ota_seq;      /* the update status it carried (panel_ota_seq) */
    uint8_t *tx;           /* PROTO_LINK_SNAPSHOT_MAX + header, PSRAM */
    /* the panel's settings and its keypads (HAI-016..018): what the client holds */
    uint32_t set_hash;     /* the settings document (0: none sent on this connection) */
    uint32_t set_seq;      /* the panel_prefs_seq, the voice and the keys it was built from */
    uint8_t set_voice;
    proto_prefs_mask_t set_can;
    uint64_t set_next;     /* not before this: at most once a second */
    uint32_t kp_hash;      /* the keypads list (0: none sent on this connection) */
    uint64_t kp_next;

    /* What other tasks hand over. The queues carry the payloads (FreeRTOS orders the copy
     * against the hand-over in its own critical section) and the flags below are read and
     * written with the atomic builtins; nothing here waits, so the alarm listener keeps its
     * "quick, never blocks" contract. Both handles are created before the alarm task or the
     * link task can reach them, and producers check for NULL all the same. */
    QueueHandle_t ev_q;   /* atomic handle: alarm_event_t, posted by the alarm task */
    QueueHandle_t call_q; /* atomic handle: ha_call_t, posted by the UI or the console */
    QueueHandle_t set_q;  /* atomic handle: ha_set_answer_t, posted by any task (HAI-019) */
    bool state_dirty;     /* atomic: set by the alarm task's listener */
    bool ev_overflow;     /* atomic: the queue filled, resend the whole picture */

    /* requests from the UI and the console (atomic; picked up by the task) */
    bool req_apply;   /* enabled changed: open or close the server */
    bool req_pair;    /* open the pairing window */
    bool req_cancel;  /* close it */
    int req_decision; /* 0 none, 1 allow, 2 reject */
    bool req_drop;    /* drop the current connection (unpair, switched off) */
    bool req_watch;   /* a zone's binding changed: send the watch list (M13) */

    bool mdns_up;
    bool server_up;
    mbedtls_net_context listen;
} S; /* internal RAM: the flags below are exchanged with atomic instructions, which the P4 is
      * not promised to honour on PSRAM (ESP-IDF's own drivers keep atomics internal) */

/* The big, task-only buffers (~13 KB): PSRAM, internal RAM is the drivers' reserve. Only the
 * link task reads and writes them, with plain loads and stores. */
static struct {
    uint8_t cert[HA_CERT_MAX];
    uint8_t key[HA_KEY_MAX];
    proto_pair_t pair;
    alarm_config_t cfg;    /* names and classes for the payloads (~10 KB) */
    alarm_snapshot_t prev; /* what the client saw last */
    alarm_snapshot_t cur;
    /* settings_set (HAI-017..019): the requests waiting or in flight, the one being read, and the
     * keypads as the port last listed them (~4 KB together) */
    proto_link_setq_t setq;
    proto_link_settings_set_t set_req;
    proto_link_keypad_t kp[PROTO_LINK_KEYPADS_MAX];
} SB ALARM_SLOW_BSS;

/* The Z-Wave devices (ZW-036): the owner's port, set before start; the list the client holds. */
#define ZW_DEV_MAX 16u
static ha_link_zw_port_t s_zw;
static bool s_have_zw;
static proto_zw_device_t s_zw_list[ZW_DEV_MAX] ALARM_SLOW_BSS;
static size_t s_zw_n;

void ha_link_set_zwave(const ha_link_zw_port_t *port)
{
    if (port != NULL && port->list != NULL && port->command != NULL) {
        s_zw = *port;
        s_have_zw = true;
    }
}

/* The link keypads' port (HAI-016..019): written once, then published with a release store, so
 * the link task that sees the flag sees the whole port. */
static ha_link_settings_port_t s_kp_port;
static bool s_kp_port_set; /* atomic */

void ha_link_set_settings(const ha_link_settings_port_t *port)
{
    if (port == NULL || port->keypads == NULL || port->set == NULL ||
        __atomic_load_n(&s_kp_port_set, __ATOMIC_ACQUIRE)) {
        return; /* once: the link task may be reading it */
    }
    s_kp_port = *port;
    __atomic_store_n(&s_kp_port_set, true, __ATOMIC_RELEASE);
}

/* The board's model, set by the app before the link starts (services never name a board,
 * docs/features/33). */
static const char *s_model = "RoboAlarms panel"; /* a string that lives forever */

void ha_link_set_model(const char *model)
{
    s_model = model != NULL ? model : "RoboAlarms panel";
}

/* ---- what crosses a task boundary (R10) ------------------------------------------------------
 * Plain reads and writes of shared fields have no ordering or visibility contract between the
 * two cores, whatever "volatile" suggests. Everything shared goes through these, with the
 * same acquire/release discipline alarm_service.c uses for its queue handle. */

static inline bool flag_get(bool *f)
{
    return __atomic_load_n(f, __ATOMIC_ACQUIRE);
}

static inline void flag_set(bool *f, bool v)
{
    __atomic_store_n(f, v, __ATOMIC_RELEASE);
}

/* Read and clear in one step: the producer may set it again the moment we let go. */
static inline bool flag_take(bool *f)
{
    return __atomic_exchange_n(f, false, __ATOMIC_ACQ_REL);
}

static inline QueueHandle_t queue_get(QueueHandle_t *q)
{
    return __atomic_load_n(q, __ATOMIC_ACQUIRE);
}

/* ---- storage ------------------------------------------------------------------------------ */

/* An erase of something that was never there is not a failure. */
static esp_err_t erased(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static void store_load(void)
{
    uint8_t on = 0;
    (void)nvs_get_u8(S.nvs, "on", &on);
    S.store.enabled = on != 0u;

    ha_trust_t rec;
    size_t len = sizeof(rec);
    if (nvs_get_blob(S.nvs, "trust", &rec, &len) == ESP_OK && len == sizeof(rec) &&
        rec.version == HA_TRUST_VERSION) {
        /* the record decides, both ways: a revocation that reached flash stays a revocation
         * even if the old "peer"/"client" keys are still lying about underneath it (R11) */
        S.generation = rec.generation;
        S.store.paired = rec.paired != 0u;
        memset(S.store.pinned_fp, 0, sizeof(S.store.pinned_fp));
        S.store.client[0] = '\0';
        if (S.store.paired) {
            memcpy(S.store.pinned_fp, rec.fp, sizeof(S.store.pinned_fp));
            snprintf(S.store.client, sizeof(S.store.client), "%.*s",
                     (int)sizeof(rec.client) - 1, rec.client); /* flash may hold no NUL */
        }
        return;
    }
    /* No record: a panel paired by firmware from before it, or never paired at all. */
    len = sizeof(S.store.pinned_fp);
    S.store.paired = nvs_get_blob(S.nvs, "peer", S.store.pinned_fp, &len) == ESP_OK &&
                     len == sizeof(S.store.pinned_fp);
    len = sizeof(S.store.client);
    if (nvs_get_str(S.nvs, "client", S.store.client, &len) != ESP_OK) {
        S.store.client[0] = '\0';
    }
}

/* Write the trust record. One blob, one commit: after this returns ESP_OK the panel will
 * still say the same thing about this client when it comes back from a power cut. */
static esp_err_t store_write(bool paired, const uint8_t *fp, const char *client)
{
#if CONFIG_HA_LINK_DIAGNOSTICS
    if (flag_get(&ha_test_store_fail)) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
#endif
    ha_trust_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = HA_TRUST_VERSION;
    rec.paired = paired ? 1u : 0u;
    rec.generation = S.generation + 1u;
    if (paired) {
        memcpy(rec.fp, fp, PROTO_LINK_FP_LEN);
        snprintf(rec.client, sizeof(rec.client), "%s", client);
    }
    esp_err_t err = nvs_set_blob(S.nvs, "trust", &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(S.nvs);
    }
    if (err == ESP_OK) {
        S.generation = rec.generation;
    }
    return err;
}

/* Pin a client. The record goes down first and decides; the old keys follow so a firmware
 * rollback still finds the pairing. Returns the first failure, and on a failure nothing in
 * RAM changed either: the caller must not tell a client it is paired when the panel could
 * not write it down (R11). Caller holds S.lock. */
static esp_err_t store_pin(const uint8_t fp[PROTO_LINK_FP_LEN], const char *client)
{
    const esp_err_t err = store_write(true, fp, client);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(S.store.pinned_fp, fp, PROTO_LINK_FP_LEN);
    snprintf(S.store.client, sizeof(S.store.client), "%s", client);
    S.store.paired = true;
    /* A pairing the user asked for last supersedes an unpair still waiting to be stored;
     * without this the retry below would wipe the record we just wrote. */
    flag_set(&S.revoke_pending, false);

    /* Copies for a firmware rollback only. Losing them costs the pairing after a downgrade,
     * which is a re-pair, not a security hole, so they don't fail the pairing. */
    esp_err_t old = nvs_set_blob(S.nvs, "peer", fp, PROTO_LINK_FP_LEN);
    if (old == ESP_OK) {
        old = nvs_set_str(S.nvs, "client", S.store.client);
    }
    if (old == ESP_OK) {
        old = nvs_commit(S.nvs);
    }
    if (old != ESP_OK) {
        ESP_LOGW(TAG, "the rollback copy of the pairing wasn't stored (%s)",
                 esp_err_to_name(old));
    }
    return ESP_OK;
}

/* Revoke the paired client. The order is chosen so that after any prefix of it - a power cut,
 * a full flash, a firmware rollback - what comes back reads as revoked or as "the revocation
 * never happened", never as "revoked, then trusted again" (R11):
 *   1. RAM, so the session is revoked whatever flash does;
 *   2. the old "peer"/"client" keys, which are all a rolled-back firmware would read;
 *   3. the record: a paired=0 record, or failing that an erased one, so the stale paired=1
 *      record left by a failed write can't speak for a client the user revoked.
 * A failure leaves revoke_pending set and the link task keeps trying. Caller holds S.lock. */
static esp_err_t store_unpair(void)
{
    S.store.paired = false;
    memset(S.store.pinned_fp, 0, sizeof(S.store.pinned_fp));
    S.store.client[0] = '\0';
#if CONFIG_HA_LINK_DIAGNOSTICS
    if (flag_get(&ha_test_store_fail)) {
        flag_set(&S.revoke_pending, true);
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
#endif

    esp_err_t err = erased(nvs_erase_key(S.nvs, "peer"));
    const esp_err_t name = erased(nvs_erase_key(S.nvs, "client"));
    esp_err_t rec = store_write(false, NULL, NULL);
    if (rec != ESP_OK) {
        rec = erased(nvs_erase_key(S.nvs, "trust")); /* no record beats a stale "paired" one */
    }
    const esp_err_t done = nvs_commit(S.nvs);
    if (err == ESP_OK) {
        err = name;
    }
    if (err == ESP_OK) {
        err = rec;
    }
    if (err == ESP_OK) {
        err = done;
    }
    flag_set(&S.revoke_pending, err != ESP_OK);
    return err;
}

/* ---- mDNS (HAI-001) ------------------------------------------------------------------------ */

static void mdns_txt(void)
{
    mdns_txt_item_t txt[] = {
        {"id", S.ident.panel_id},   {"name", S.name},        {"model", S.ident.model},
        {"fw", S.ident.fw},         {"api", "1"},            {"paired", S.store.paired ? "1" : "0"},
    };
    mdns_service_txt_set("_roboalarms", "_tcp", txt, sizeof(txt) / sizeof(txt[0]));
}

static void mdns_apply(bool advertise)
{
    if (advertise && !S.mdns_up) {
        if (mdns_init() != ESP_OK) { /* fails if the network stack isn't ready */
            return;
        }
        char host[32];
        snprintf(host, sizeof(host), "roboalarms-%s", S.ident.panel_id);
        mdns_hostname_set(host);
        mdns_instance_name_set(S.name[0] != '\0' ? S.name : "RoboAlarms Panel");
        if (mdns_service_add(NULL, "_roboalarms", "_tcp", PROTO_LINK_PORT, NULL, 0) == ESP_OK) {
            mdns_txt();
            S.mdns_up = true;
        }
    } else if (!advertise && S.mdns_up) {
        mdns_service_remove("_roboalarms", "_tcp");
        /* mDNS is shared with optional services (Feature 30). Remove our service only;
         * the process-wide responder lives until reboot. */
        S.mdns_up = false;
    }
}

/* ---- helpers ------------------------------------------------------------------------------- */

static uint64_t now_ms(void)
{
    return (uint64_t)alarm_service_now();
}

/* The link task's copy of the configuration: names and classes for the payloads. */
static void refresh_config(void)
{
    if (alarm_service_get_config(&SB.cfg) == ESP_OK) {
        xSemaphoreTake(S.lock, portMAX_DELAY);
        i18n_utf8_copy(S.name, sizeof(S.name), SB.cfg.partitions[0].name);
        xSemaphoreGive(S.lock);
    }
}

/* ---- what the alarm task reports (quick, never blocks: alarm_service.h rules) --------------- */

static void listener_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    QueueHandle_t q = queue_get(&S.ev_q);
    if (q == NULL) {
        return; /* before ha_link_start(): nothing is connected, nothing is owed */
    }
    /* A zero timeout: the alarm task never waits here. Full means the client is behind, and
     * the whole picture goes out again instead of a broken run of events. */
    if (xQueueSend(q, ev, 0) != pdTRUE) {
        flag_set(&S.ev_overflow, true);
    }
}

static void listener_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    (void)snap;
    flag_set(&S.state_dirty, true); /* the link task pulls its own copy when it pushes */
}

const alarm_listener_t *ha_link_listener(void)
{
    static const alarm_listener_t listener = {
        .on_event = listener_event,
        .on_snapshot = listener_snapshot,
    };
    return &listener;
}

/* ---- one TLS connection --------------------------------------------------------------------- */

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    uint8_t peer_fp[PROTO_LINK_FP_LEN];
    bool peer_is_paired;
    char hello_client[PROTO_LINK_CLIENT_LEN];
    uint8_t *rx; /* RX_MAX bytes, PSRAM */
    /* the frame being collected: a header may arrive one byte at a time, across polls */
    uint8_t hdr[PROTO_LINK_HDR_LEN];
    size_t hdr_got;
    uint64_t hdr_deadline; /* when a header that started arriving must be complete */
} conn_t;

/* Unpair and switching the integration off must not wait for the client: every wait in this
 * file looks at this between attempts. */
static bool cancelled(void)
{
    return flag_get(&S.req_drop);
}

/* One read attempt, at most the configured read timeout long (POLL_MS). */
typedef enum {
    RX_DATA,   /* *got bytes arrived */
    RX_QUIET,  /* nothing this poll: the client is simply silent */
    RX_CLOSED, /* closed, or broken beyond use */
} rx_t;

static rx_t read_some(conn_t *c, uint8_t *buf, size_t len, size_t *got)
{
    *got = 0;
    const int r = mbedtls_ssl_read(&c->ssl, buf, len);
    if (r > 0) {
        *got = (size_t)r;
        return RX_DATA;
    }
    if (r == MBEDTLS_ERR_SSL_TIMEOUT || r == MBEDTLS_ERR_SSL_WANT_READ ||
        r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        proto_pair_tick(&SB.pair, now_ms()); /* the pairing window keeps running meanwhile */
        return RX_QUIET;
    }
    return RX_CLOSED;
}

/* Read exactly len bytes, or give up at the deadline. Every caller passes one: a client that
 * sends the first byte of a frame and then stops must not own this task (R09). */
static bool read_exact(conn_t *c, uint8_t *buf, size_t len, uint64_t deadline)
{
    size_t got = 0;
    while (got < len) {
        if (cancelled()) {
            return false;
        }
        if (now_ms() > deadline) {
            ESP_LOGW(TAG, "a frame stopped half way: dropping the connection");
            return false;
        }
        size_t n = 0;
        if (read_some(c, buf + got, len - got, &n) == RX_CLOSED) {
            return false;
        }
        got += n; /* RX_QUIET costs a poll interval inside mbedTLS: no spinning here */
    }
    return true;
}

/* Write every byte of one buffer. false: the client is gone, has stopped reading for
 * TX_STALL_MS, took longer than the frame's deadline, or the link is being dropped.
 * SO_SNDTIMEO on the accepted socket (see connection()) turns a stalled send into
 * MBEDTLS_ERR_SSL_WANT_WRITE, so this loop always gets its turn back (R09). */
static bool write_all(conn_t *c, const uint8_t *data, size_t len, uint64_t deadline)
{
    size_t sent = 0;
    uint64_t progress = now_ms();
    while (sent < len) {
        if (cancelled()) {
            return false;
        }
        const uint64_t now = now_ms();
        if (now > deadline || now - progress > TX_STALL_MS) {
            ESP_LOGW(TAG, "the client stopped reading: dropping the connection");
            return false;
        }
        /* mbedTLS wants the same pointer and length back after a WANT_*: sent only moves on
         * bytes it actually took. */
        const int r = mbedtls_ssl_write(&c->ssl, data + sent, len - sent);
        if (r > 0) {
            sent += (size_t)r;
            progress = now_ms();
            continue;
        }
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return false; /* closed or broken */
        }
        vTaskDelay(pdMS_TO_TICKS(20)); /* give the scheduler its turn rather than spin */
    }
    return true;
}

/* One frame straight from a large buffer (a snapshot would not fit a copy on the stack).
 * Header and body share one deadline: to the client it is one frame either way. */
static bool send_raw(conn_t *c, const uint8_t *data, size_t len)
{
    uint8_t hdr[PROTO_LINK_HDR_LEN];
    proto_link_frame_header((uint32_t)len, hdr);
    const uint64_t deadline = now_ms() + TX_FRAME_MS;
    return write_all(c, hdr, sizeof(hdr), deadline) && write_all(c, data, len, deadline);
}

/* The same, for a document built in a caller's buffer. It goes out header first rather than
 * through a copy: the callers already hold a PROTO_PAYLOAD_MAX buffer each, and a second one
 * here put the deepest path (connection > serve > push_state > send_calls > here) over this
 * task's 12 KB stack. */
static bool send_doc(conn_t *c, const char *doc, size_t len)
{
    if (len > PROTO_PAYLOAD_MAX) {
        return false;
    }
    return send_raw(c, (const uint8_t *)doc, len);
}

static bool send_built(conn_t *c, pj_writer_t *w, char *buf)
{
    size_t len = pj_finish(w);
    return len > 0u && send_doc(c, buf, len);
}

static bool send_hello(conn_t *c)
{
    char *buf = (char *)S.tx; /* link-task-owned PSRAM, no nested 4 KB stack buffers */
    pj_writer_t w;
    pj_init(&w, buf, PROTO_PAYLOAD_MAX);
    xSemaphoreTake(S.lock, portMAX_DELAY);
    proto_link_hello(&w, &S.ident, S.name, S.store.paired);
    xSemaphoreGive(S.lock);
    return send_built(c, &w, buf);
}

static bool send_pair_result(conn_t *c, const char *result)
{
    char buf[128];
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_link_pair_result(&w, result);
    return send_built(c, &w, buf);
}

static void push_reset(void); /* below, with what the client holds */

/* The pairing outcome reached while this connection waits (Allow tapped, window ran out). */
static bool pairing_wait(conn_t *c)
{
    while (true) {
        proto_pair_tick(&SB.pair, now_ms());
        if (SB.pair.state == PROTO_PAIR_FAILED) {
            send_pair_result(c, SB.pair.fail);
            return false;
        }
        const int decision = __atomic_exchange_n(&S.req_decision, 0, __ATOMIC_ACQ_REL);
        if (decision != 0) {
            const char *result = proto_pair_decide(&SB.pair, decision == 1);
            if (SB.pair.state == PROTO_PAIR_DONE) {
                xSemaphoreTake(S.lock, portMAX_DELAY);
                const esp_err_t err = store_pin(SB.pair.peer_fp, c->hello_client);
                xSemaphoreGive(S.lock);
                if (err != ESP_OK) {
                    /* HAI-003: never tell a client it is paired before the panel can prove it
                     * after a restart. Nothing was pinned, so it may pair again (R11). */
                    ESP_LOGE(TAG, "the pairing wasn't stored (%s): telling the client no",
                             esp_err_to_name(err));
                    send_pair_result(c, PROTO_LINK_R_REJECTED);
                    proto_pair_close(&SB.pair);
                    return false;
                }
                c->peer_is_paired = true;
                S.synced = false;
                push_reset();
                flag_set(&S.connected, true);
                flag_set(&S.req_watch, true);
                zone_service_ha_link_state(true);
                device_service_ha_link(true);
                mdns_txt();
#if CONFIG_ALARM_FEATURE_MQTT
                net_mqtt_suppress_discovery(true); /* HAI-010 */
#endif
                ESP_LOGI(TAG, "paired with %s", c->hello_client);
                send_pair_result(c, result);
                proto_pair_close(&SB.pair);
                return true; /* the connection may go on (it usually reconnects pinned) */
            }
            send_pair_result(c, result);
            proto_pair_close(&SB.pair);
            return false;
        }
        if (flag_get(&S.req_cancel) || cancelled()) {
            send_pair_result(c, PROTO_LINK_R_REJECTED);
            return false;
        }
        /* The client is waiting for our decision and owes no application data.
         * Notice close-notify/EOF here so cancelling an HA config flow releases
         * the single connection slot instead of occupying it for two minutes. */
        uint8_t unexpected;
        size_t got;
        if (read_some(c, &unexpected, 1u, &got) != RX_QUIET) {
            return false;
        }
    }
}

/* ---- pushing state to the paired client ----------------------------------------------------- */

static void when_now(proto_when_t *when)
{
    when->now = alarm_service_now();
    /* Set by the time server, a clock module or a person (docs/features/35): 0 until then. */
    const time_t t = time(NULL);
    when->epoch_ms = t >= (time_t)1735689600 ? (int64_t)t * 1000 : 0;
}

/* Device actions from the Devices screen (docs/features/24): queued by any task, sent here. The
 * queue takes several producers safely; a zero timeout means no caller ever waits on the
 * link task. */
static bool link_send_call(void *ctx, const char *entity, const char *action, bool has_value,
                           int32_t value, const char *mode, uint32_t request_id)
{
    (void)ctx;
    QueueHandle_t q = queue_get(&S.call_q);
    if (q == NULL || !flag_get(&S.connected)) {
        return false; /* the paired Home Assistant isn't on the line */
    }
    ha_call_t call;
    memset(&call, 0, sizeof(call));
    snprintf(call.entity, sizeof(call.entity), "%s", entity);
    snprintf(call.action, sizeof(call.action), "%s", action);
    snprintf(call.mode, sizeof(call.mode), "%s", mode != NULL ? mode : "");
    call.value = value;
    call.has_value = has_value;
    call.request_id = request_id;
    /* full: the caller reports offline rather than waiting */
    return xQueueSend(q, &call, 0) == pdTRUE;
}

static bool send_calls(conn_t *c)
{
    char *buf = (char *)S.tx; /* link-task-owned PSRAM, no nested 4 KB stack buffers */
    pj_writer_t w;
    QueueHandle_t q = queue_get(&S.call_q);
    ha_call_t call;
    while (q != NULL && xQueueReceive(q, &call, 0) == pdTRUE) {
        char id[PROTO_LINK_CALL_ID_LEN];
        snprintf(id, sizeof(id), "%08x", (unsigned)call.request_id);
        pj_init(&w, buf, PROTO_PAYLOAD_MAX);
        proto_link_call(&w, id, call.entity, call.action, call.has_value, call.value,
                        call.mode[0] != '\0' ? call.mode : NULL);
        if (!send_built(c, &w, buf)) {
            return false;
        }
    }
    return true;
}

/* The zones' entity list for Home Assistant (M13): sent when a binding changes. */
static bool send_watch(conn_t *c)
{
    static char refs[ZSRC_HA_WATCH_MAX][ZSRC_REF_LEN] ALARM_SLOW_BSS; /* only the link task touches it */
    const char *ptrs[ZSRC_HA_WATCH_MAX];
    const size_t n = zone_service_ha_watch_list(refs, ZSRC_HA_WATCH_MAX);
    for (size_t i = 0; i < n; i++) {
        ptrs[i] = refs[i];
    }
    pj_writer_t w;
    pj_init(&w, (char *)S.tx, PROTO_LINK_SNAPSHOT_MAX); /* 48 ids don't fit a small buffer */
    proto_link_watch(&w, ptrs, n);
    const size_t len = pj_finish(&w);
    return len > 0u && send_raw(c, S.tx, len);
}

/* The diagnostic status (protocol.md "Optional diagnostic status"): before the first snapshot,
 * every 10 s, and as an update's progress moves. false: connection broken. */
static bool push_status(conn_t *c)
{
    const uint64_t now = now_ms();
    /* An update's progress goes out as it changes, at most once a second (OTA-026). */
    const uint32_t ota_seq = panel_ota_seq();
    const bool ota_moved = ota_seq != S.ota_seq && now >= S.status_sent + 1000u;
    if (S.synced && now < S.status_next && !ota_moved) {
        return true;
    }
    net_wifi_status_t net;
    panel_ota_status_t ota;
    net_wifi_get_status(&net);
    panel_ota_get_status(&ota);
    /* Reuse the PSRAM transmit buffer; status never carries network credentials
     * or release-server URLs, which can contain private access tokens. */
    pj_writer_t w;
    pj_init(&w, (char *)S.tx, PROTO_LINK_SNAPSHOT_MAX);
    pj_obj(&w);
    pj_kstr(&w, "t", "status");
    pj_kuint(&w, "uptime_s", now / 1000u);
    if (net.rssi < 0) {
        pj_kint(&w, "rssi", net.rssi);
    } else {
        pj_knull(&w, "rssi");
    }
    pj_kobj(&w, "update");
    pj_kstr(&w, "installed", ota.running);
    pj_kstr(&w, "latest", ota.latest);
    pj_kbool(&w, "available", ota.available);
    /* OTA-026: as the MQTT update entity (panel_app.c fw_get) */
    const bool stepless = ota.kind == PANEL_OTA_KIND_FILE || ota.kind == PANEL_OTA_KIND_WIFI_MODULE;
    pj_kbool(&w, "in_progress", ota.stage == PANEL_OTA_STAGE_DOWNLOADING ||
                                    ota.stage == PANEL_OTA_STAGE_VERIFYING ||
                                    ota.stage == PANEL_OTA_STAGE_INSTALLING ||
                                    ota.stage == PANEL_OTA_STAGE_ROLLING_BACK ||
                                    (stepless && (ota.phase == PANEL_OTA_RECEIVING ||
                                                  ota.phase == PANEL_OTA_RESTARTING)));
    pj_kint(&w, "progress",
            ota.phase == PANEL_OTA_RECEIVING &&
                    (ota.stage == PANEL_OTA_STAGE_DOWNLOADING || stepless)
                ? ota.percent
                : -1);
    pj_obj_end(&w);
    pj_obj_end(&w);
    const size_t len = pj_finish(&w);
    if (len == 0u || !send_raw(c, S.tx, len)) {
        return false;
    }
    S.status_next = now + 10000u;
    S.status_sent = now;
    S.ota_seq = ota_seq;
    return true;
}

/* FNV-1a over a document the client holds a copy of; never 0, which means "not sent on this
 * connection". */
static uint32_t doc_hash(const uint8_t *doc, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ doc[i]) * 16777619u;
    }
    return h | 1u;
}

/* The Z-Wave devices' list, when it changed (a state, a device come or gone), at most once a
 * second: the whole list every time - the client replaces what it holds. */
static bool push_zwave(conn_t *c)
{
    const uint64_t now = now_ms();
    if (!s_have_zw || now < S.zw_next) {
        return true;
    }
    S.zw_next = now + 1000u;
    s_zw_n = s_zw.list(s_zw.ctx, s_zw_list, ZW_DEV_MAX);
    pj_writer_t w;
    pj_init(&w, (char *)S.tx, PROTO_LINK_SNAPSHOT_MAX);
    proto_link_zwave_devices(&w, s_zw_list, s_zw_n);
    const size_t len = pj_finish(&w);
    const uint32_t crc = doc_hash(S.tx, len);
    if (len == 0u || crc == S.zw_crc) {
        return true;
    }
    if (!send_raw(c, S.tx, len)) {
        return false;
    }
    S.zw_crc = crc;
    return true;
}

/* ---- the panel's settings and its keypads (HAI-016..019; protocol.md "Panel settings and
 * keypads") ------------------------------------------------------------------------------------ */

/* The master's voice in the link's numbers; *present: this build has one (ui_voice_present). */
static uint8_t master_voice(bool *present)
{
    panel_voice_status_t v;
    panel_voice_status(&v);
    *present = v.state != PANEL_VOICE_OFF;
    switch (v.state) {
    case PANEL_VOICE_READY: return PROTO_LINK_VOICE_READY;
    case PANEL_VOICE_LOADING: return PROTO_LINK_VOICE_LOADING;
    default: return PROTO_LINK_VOICE_NONE; /* none in this build, no pack, or one it can't use */
    }
}

/* The fields Home Assistant may set on the master: every one its screen takes (UI-046), the
 * announcements' only where its screen shows them (a build with the voice). */
static proto_prefs_mask_t master_can(bool voice_present)
{
    const proto_prefs_mask_t voice = PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_VOLUME) |
                                     PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_ENABLED) |
                                     PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_STATUS) |
                                     PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_CHIME);
    return voice_present ? PROTO_PREFS_KNOWN : (PROTO_PREFS_KNOWN & ~voice);
}

/* The master's own settings (HAI-017): after the paired hello, then on a save or a change of its
 * voice, looked at once a second. Only what the panel stored (panel_prefs_current), never
 * defaults it hasn't loaded yet. */
static bool push_settings(conn_t *c)
{
    const uint64_t now = now_ms();
    if (now < S.set_next) {
        return true;
    }
    S.set_next = now + 1000u;
    bool present = false;
    const uint8_t voice = master_voice(&present);
    const proto_prefs_mask_t can = master_can(present);
    const uint32_t seq = panel_prefs_seq();
    if (S.set_hash != 0u && seq == S.set_seq && voice == S.set_voice && can == S.set_can) {
        return true;
    }
    panel_prefs_t prefs;
    if (!panel_prefs_current(&prefs)) {
        return true; /* the screen hasn't loaded them yet */
    }
    proto_prefs_values_t values;
    panel_prefs_to_values(&prefs, &values);
    pj_writer_t w;
    pj_init(&w, (char *)S.tx, PROTO_PAYLOAD_MAX);
    proto_link_settings(&w, &values, can, voice);
    const size_t len = pj_finish(&w);
    if (len == 0u) {
        return true;
    }
    S.set_seq = seq;
    S.set_voice = voice;
    S.set_can = can;
    const uint32_t hash = doc_hash(S.tx, len);
    if (hash == S.set_hash) {
        return true; /* a save of other options (the language): nothing new to say */
    }
    if (!send_raw(c, S.tx, len)) {
        return false;
    }
    S.set_hash = hash;
    return true;
}

/* The port's keypads in SB.kp, tidied: an id that isn't one, the master's own or a lower keypad's
 * twin is cleared (*cleared of them; NULL: not wanted). None without a port. */
static size_t keypads_list(size_t *cleared)
{
    size_t n = 0;
    if (__atomic_load_n(&s_kp_port_set, __ATOMIC_ACQUIRE)) {
        n = s_kp_port.keypads(s_kp_port.ctx, SB.kp, PROTO_LINK_KEYPADS_MAX);
        n = n < PROTO_LINK_KEYPADS_MAX ? n : PROTO_LINK_KEYPADS_MAX;
    }
    const size_t gone = proto_link_keypads_tidy(SB.kp, n, S.ident.panel_id);
    if (cleared != NULL) {
        *cleared = gone;
    }
    return n;
}

/* The link keypads (HAI-018): the whole list after the paired hello and whenever anything in it
 * changes, at most once a second, as the Z-Wave devices; an empty list from a master without
 * keypads (or before the panel link's service gives ha_link its port). */
static bool push_keypads(conn_t *c)
{
    const uint64_t now = now_ms();
    if (now < S.kp_next) {
        return true;
    }
    S.kp_next = now + 1000u;
    size_t cleared = 0;
    const size_t n = keypads_list(&cleared);
    pj_writer_t w;
    pj_init(&w, (char *)S.tx, PROTO_LINK_SNAPSHOT_MAX); /* 8 keypads take up to 8 KB */
    proto_link_keypads(&w, SB.kp, n);
    const size_t len = pj_finish(&w);
    const uint32_t hash = doc_hash(S.tx, len);
    if (len == 0u || hash == S.kp_hash) {
        return true;
    }
    if (cleared > 0u) {
        ESP_LOGW(TAG, "%u keypad id(s) published as null: not a panel id, this panel's own, or "
                      "one a lower keypad number reports too", (unsigned)cleared);
    }
    if (!send_raw(c, S.tx, len)) {
        return false;
    }
    S.kp_hash = hash;
    return true;
}

static const char *set_error(ha_link_set_t r)
{
    switch (r) {
    case HA_LINK_SET_SENT: return "";
    case HA_LINK_SET_NOT_FOUND: return PROTO_LINK_SET_E_NOT_FOUND;
    case HA_LINK_SET_OFFLINE: return PROTO_LINK_SET_E_OFFLINE;
    case HA_LINK_SET_UNSUPPORTED: return PROTO_LINK_SET_E_UNSUPPORTED;
    case HA_LINK_SET_TIMEOUT: return PROTO_LINK_SET_E_TIMEOUT;
    default: return PROTO_LINK_SET_E_BUSY;
    }
}

/* One settings_result (HAI-017): error "" with the target's refusals (why, NULL: none), or an
 * error for the whole request. */
static bool send_set_result(conn_t *c, const proto_link_settings_set_t *req, const char *error,
                            const uint8_t *why)
{
    char *buf = (char *)S.tx; /* link-task-owned PSRAM, no nested 4 KB stack buffers */
    pj_writer_t w;
    pj_init(&w, buf, PROTO_PAYLOAD_MAX);
    proto_link_settings_result(&w, req, error, why);
    return send_built(c, &w, buf);
}

/* The screen's answer for the master's own settings (panel_prefs_done_fn): on the screen's task,
 * so only the hand-over; ctx carries the request's ticket. */
static void master_prefs_done(void *ctx, uint32_t mailbox, const uint8_t why[PROTO_PREFS_F_LAST])
{
    (void)mailbox;
    ha_link_settings_done((uint32_t)(uintptr_t)ctx, why, PROTO_PREFS_F_LAST);
}

/* A settings_set (HAI-017, HAI-019): answered at once when it can't go (bad, not_found, offline,
 * unsupported, nothing to change, no room), else queued for its target's turn. false: the
 * connection broke. */
static bool settings_request(conn_t *c, proto_link_settings_set_t *req)
{
    if (req->bad) {
        return send_set_result(c, req, PROTO_LINK_SET_E_BAD, NULL);
    }
    uint8_t target = 0u; /* the master itself */
    proto_prefs_mask_t can = 0u;
    if (strcmp(req->target, S.ident.panel_id) == 0) {
        bool present = false;
        (void)master_voice(&present);
        can = master_can(present);
    } else {
        /* by its own panel id, never by device number: a number is reused after a removal */
        const size_t n = req->target[0] != '\0' ? keypads_list(NULL) : 0u;
        const proto_link_keypad_t *k = NULL;
        for (size_t i = 0; i < n; i++) {
            if (SB.kp[i].id[0] != '\0' && strcmp(SB.kp[i].id, req->target) == 0) {
                k = &SB.kp[i];
            }
        }
        if (k == NULL || k->device < 1u || k->device >= PROTO_LINK_SET_TARGETS) {
            return send_set_result(c, req, PROTO_LINK_SET_E_NOT_FOUND, NULL);
        }
        if (!k->online) {
            return send_set_result(c, req, PROTO_LINK_SET_E_OFFLINE, NULL);
        }
        if (!k->reported || k->can == 0u) {
            return send_set_result(c, req, PROTO_LINK_SET_E_UNSUPPORTED, NULL);
        }
        target = k->device;
        can = k->can;
    }
    proto_link_settings_set_accept(req, can);
    if (req->p.values.have == 0u) {
        return send_set_result(c, req, "", NULL); /* nothing to send: ok, or its refusals */
    }
    if (proto_link_setq_add(&SB.setq, now_ms(), target, req) == NULL) {
        return send_set_result(c, req, PROTO_LINK_SET_E_BUSY, NULL); /* 8 already waiting */
    }
    return true; /* the next pass of push_state sends it when its turn is due */
}

/* settings_set's answers, give-ups and turns (HAI-019). false: the connection broke. */
static bool settings_pump(conn_t *c)
{
    const uint64_t now = now_ms();
    proto_link_set_slot_t *s;
    bool ok = true;
    /* what came back: the screen's or a keypad's answer, or how a keypad's request ended */
    QueueHandle_t q = queue_get(&S.set_q);
    ha_set_answer_t a;
    while (ok && q != NULL && xQueueReceive(q, &a, 0) == pdTRUE) {
        s = proto_link_setq_find(&SB.setq, a.ticket);
        if (s == NULL) {
            continue; /* late: answered timeout already, or asked on an earlier connection */
        }
        ok = a.err == (uint8_t)HA_LINK_SET_SENT
                 ? send_set_result(c, &s->req, "", a.why)
                 : send_set_result(c, &s->req, set_error((ha_link_set_t)a.err), NULL);
        proto_link_setq_free(&SB.setq, s);
    }
    /* The master's screen hasn't taken its patch in 3 s (stuck, or none): withdraw it, every field
     * busy, as the mailbox answers a patch it took too late; when it can't be withdrawn, the
     * screen has it and its answer is on the way. */
    for (size_t i = 0; ok && i < PROTO_LINK_SET_QUEUE; i++) {
        s = &SB.setq.slot[i];
        if (s->state != PROTO_LINK_SET_FLIGHT || s->target != 0u || s->withdrawn ||
            now < s->sent_ms + PREFS_GIVE_UP_MS) {
            continue;
        }
        s->withdrawn = true;
        if (panel_prefs_remote_cancel(s->handle)) {
            uint8_t why[PROTO_PREFS_F_LAST];
            for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
                why[id - 1u] = (s->req.p.values.have & PROTO_PREFS_BIT(id)) != 0u
                                   ? (uint8_t)PROTO_PREFS_WHY_BUSY
                                   : 0u;
            }
            ok = send_set_result(c, &s->req, "", why);
            proto_link_setq_free(&SB.setq, s);
        }
    }
    /* within 7 s whatever happens: busy when it never went (or was withdrawn just now), timeout
     * when it went and nothing came back */
    while (ok && (s = proto_link_setq_expired(&SB.setq, now)) != NULL) {
        const bool unsent = s->state != PROTO_LINK_SET_FLIGHT ||
                            (s->target == 0u && !s->withdrawn &&
                             panel_prefs_remote_cancel(s->handle));
        ok = send_set_result(c, &s->req, unsent ? PROTO_LINK_SET_E_BUSY : PROTO_LINK_SET_E_TIMEOUT,
                             NULL);
        proto_link_setq_free(&SB.setq, s);
    }
    /* the turns due now: the master's own to its screen, a keypad's through the port */
    while (ok && (s = proto_link_setq_next(&SB.setq, now)) != NULL) {
        ha_link_set_t r = HA_LINK_SET_NOT_FOUND;
        if (s->target == 0u) {
            s->handle = panel_prefs_remote_submit(&s->req.p.values, master_prefs_done,
                                                  (void *)(uintptr_t)s->ticket);
            r = s->handle != 0u ? HA_LINK_SET_SENT : HA_LINK_SET_BUSY;
        } else if (__atomic_load_n(&s_kp_port_set, __ATOMIC_ACQUIRE)) {
            r = s_kp_port.set(s_kp_port.ctx, s->target, s->ticket, &s->req.p.values);
        }
        if (r != HA_LINK_SET_SENT) {
            ok = send_set_result(c, &s->req, set_error(r), NULL);
            proto_link_setq_free(&SB.setq, s);
        }
    }
    return ok;
}

/* What the client holds is per connection: a new one gets the whole Z-Wave list, the settings and
 * the keypads again, and the settings_set requests of the last one are dropped (their answers have
 * nowhere to go; a late one matches nothing). The turns go on (proto_link_setq_clear). */
static void push_reset(void)
{
    S.zw_crc = 0u;
    S.zw_next = 0u;
    S.set_hash = 0u;
    S.set_next = 0u;
    S.kp_hash = 0u;
    S.kp_next = 0u;
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        const proto_link_set_slot_t *s = &SB.setq.slot[i];
        if (s->state == PROTO_LINK_SET_FLIGHT && s->target == 0u && !s->withdrawn) {
            /* no one would give up on it later: a screen that never looks mustn't keep the
             * mailbox (one the screen has already taken is applied, as asked) */
            (void)panel_prefs_remote_cancel(s->handle);
        }
    }
    proto_link_setq_clear(&SB.setq);
    QueueHandle_t setq = queue_get(&S.set_q);
    if (setq != NULL) {
        (void)xQueueReset(setq);
    }
}

/* The alarm's own picture: queued events, then the state (a full snapshot the first time and after
 * a configuration change, a delta otherwise). false: connection broken. */
static bool push_alarm(conn_t *c)
{
    char *buf = (char *)S.tx; /* link-task-owned PSRAM, no nested 4 KB stack buffers */
    pj_writer_t w;
    QueueHandle_t q = queue_get(&S.ev_q);
    alarm_event_t ev;
    while (q != NULL && xQueueReceive(q, &ev, 0) == pdTRUE) {
        pj_init(&w, buf, PROTO_PAYLOAD_MAX);
        proto_link_event(&w, &SB.cfg, &ev, false);
        if (!send_built(c, &w, buf)) {
            return false;
        }
    }
    /* After the drain, so a drop while we were sending is still seen (it sets the flag
     * again and the next pass resends). */
    if (flag_take(&S.ev_overflow)) {
        S.synced = false; /* events were lost: give the client the whole picture again */
    }
    if (!flag_take(&S.state_dirty) && S.synced) {
        return true;
    }
    alarm_service_get_snapshot(&SB.cur);
    proto_when_t when;
    when_now(&when);
    const bool config_changed = SB.cur.config_seq != SB.prev.config_seq;
    if (config_changed) {
        refresh_config();
    }
    if (!S.synced || config_changed) {
        pj_init(&w, (char *)S.tx, PROTO_LINK_SNAPSHOT_MAX);
        proto_link_snapshot(&w, &SB.cfg, &SB.cur, &when);
        const size_t len = pj_finish(&w);
        if (len == 0u || !send_raw(c, S.tx, len)) {
            return false;
        }
        S.synced = true;
        SB.prev = SB.cur;
        return true;
    }
    uint8_t pmask = 0;
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    bool any_zone = false;
    bool troubles = SB.cur.sys_troubles != SB.prev.sys_troubles ||
                    SB.cur.sys_troubles_restored != SB.prev.sys_troubles_restored ||
                    SB.cur.dev_troubles != SB.prev.dev_troubles;
    for (uint8_t p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (SB.cur.program != SB.prev.program ||
            memcmp(&SB.cur.part[p], &SB.prev.part[p], sizeof(SB.cur.part[p])) != 0) {
            pmask |= (uint8_t)(1u << p);
        }
    }
    for (uint16_t z = 0; z < ALARM_MAX_ZONES; z++) {
        if (memcmp(&SB.cur.zone[z], &SB.prev.zone[z], sizeof(SB.cur.zone[z])) != 0) {
            zones[z / 8u] |= (uint8_t)(1u << (z % 8u));
            any_zone = true;
            if (SB.cur.zone[z].trouble != SB.prev.zone[z].trouble) {
                troubles = true; /* the troubles object lists zones too */
            }
        }
    }
    if (pmask != 0u || any_zone || troubles) {
        pj_init(&w, (char *)S.tx, PROTO_LINK_SNAPSHOT_MAX);
        proto_link_delta(&w, &SB.cfg, &SB.cur, &when, pmask, zones, troubles);
        const size_t len = pj_finish(&w);
        if (len == 0u || !send_raw(c, S.tx, len)) {
            return false;
        }
    }
    SB.prev = SB.cur;
    return true;
}

/* Everything the paired client is owed, its first snapshot before the settings and the keypads.
 * false: connection broken. */
static bool push_state(conn_t *c)
{
    if (!c->peer_is_paired || c->hello_client[0] == '\0' || S.tx == NULL) {
        return true; /* nothing is owed before the hello, or to a pairing stranger */
    }
    if (flag_take(&S.req_watch)) {
        if (!send_watch(c)) {
            return false;
        }
    }
    return send_calls(c) && push_status(c) && push_zwave(c) && push_alarm(c) &&
           push_settings(c) && push_keypads(c) && settings_pump(c);
}

/* A control entity's values as the wire carried them (proto_link_values_t, already range-checked)
 * in home_control's terms (dev_values_t: zero = not known); dsrc_ha keeps what fits the kind. */
static dev_values_t dev_values_of(const proto_link_values_t *pv)
{
    dev_values_t v;
    memset(&v, 0, sizeof(v));
    if (pv->level >= 0 && pv->level <= 100) {
        v.flags = (uint8_t)(v.flags | DEV_VAL_LEVEL);
        v.level = (uint8_t)pv->level;
    }
    if (pv->setpoint != PROTO_LINK_TEMP_UNKNOWN) {
        v.flags = (uint8_t)(v.flags | DEV_VAL_SETPOINT);
        v.setpoint = pv->setpoint;
    }
    if (pv->temp != PROTO_LINK_TEMP_UNKNOWN) {
        v.flags = (uint8_t)(v.flags | DEV_VAL_TEMP);
        v.temp = pv->temp;
    }
    static const struct {
        uint8_t wire;
        uint8_t dev;
    } MODES[] = {
        {PROTO_LINK_MODE_OFF, DEV_MODE_OFF},   {PROTO_LINK_MODE_HEAT, DEV_MODE_HEAT},
        {PROTO_LINK_MODE_COOL, DEV_MODE_COOL}, {PROTO_LINK_MODE_AUTO, DEV_MODE_AUTO},
        {PROTO_LINK_MODE_HEAT_COOL, DEV_MODE_HEAT_COOL},
    };
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        if ((pv->modes & MODES[i].wire) != 0u) {
            v.modes = (uint8_t)(v.modes | MODES[i].dev);
        }
    }
    /* "supports" (BL-171, BL-174): 0 = not known, the screen offers what the kind has */
    static const struct {
        uint8_t wire;
        uint8_t dev;
    } CAPS[] = {
        {PROTO_LINK_CAP_STOP, DEV_CAP_STOP},        {PROTO_LINK_CAP_POSITION, DEV_CAP_POSITION},
        {PROTO_LINK_CAP_PERCENTAGE, DEV_CAP_SPEED}, {PROTO_LINK_CAP_BRIGHTNESS, DEV_CAP_DIM},
        {PROTO_LINK_CAP_KNOWN, DEV_CAP_KNOWN},
    };
    for (size_t i = 0; i < sizeof(CAPS) / sizeof(CAPS[0]); i++) {
        if ((pv->caps & CAPS[i].wire) != 0u) {
            v.caps = (uint8_t)(v.caps | CAPS[i].dev);
        }
    }
    /* PROTO_LINK_HVAC_* and dev_hvac_t share their numbers (both headers say so) */
    v.hvac_action = (uint8_t)(pv->hvac_action < (uint8_t)DEV_HVAC_COUNT ? pv->hvac_action : 0u);
    v.since = pv->since;
    return v;
}

/* One message; false ends the connection. */
static bool handle(conn_t *c, const proto_link_in_t *in, const char *json, uint32_t len)
{
    char *buf = (char *)S.tx; /* link-task-owned PSRAM, no nested 4 KB stack buffers */
    pj_writer_t w;
    switch (in->type) {
    case PROTO_LINK_IN_COMMAND: {
        if (!c->peer_is_paired) {
            return false; /* commands only over the pinned connection (HAI-002) */
        }
        proto_cmd_t cmd;
        alarm_cmd_result_t res;
        memset(&res, 0, sizeof(res));
        /* panic from Home Assistant stays off until it is an option (docs/features/23) */
        alarm_result_t r = proto_cmd_parse(json, len, false, &cmd);
        if (r == ALARM_OK) {
            const alarm_source_t src = {.kind = ALARM_SRC_REMOTE, .id = 2}; /* 1 = MQTT */
            r = alarm_service_command(&cmd.cmd, &src, &res);
        }
        pj_init(&w, buf, PROTO_PAYLOAD_MAX);
        proto_link_result(&w, &cmd, r, &res);
        return send_built(c, &w, buf);
    }
    case PROTO_LINK_IN_CATALOG: { /* the entities Home Assistant shares (M13, HAI-009) */
        if (!c->peer_is_paired) {
            return false;
        }
        proto_link_catalog_t cat;
        if (!proto_link_catalog_init(&cat, json, len)) {
            return false;
        }
        if (cat.page == 0) {
            zone_service_ha_catalog_begin();
            device_service_ha_catalog_begin();
        }
        proto_link_entity_t ent;
        while (proto_link_catalog_next(&cat, &ent)) {
            if (ent.zones) {
                zsrc_ha_entity_t e;
                memset(&e, 0, sizeof(e));
                snprintf(e.id, sizeof(e.id), "%s", ent.id);
                i18n_utf8_copy(e.name, sizeof(e.name), ent.name); /* whole characters */
                i18n_utf8_copy(e.area, sizeof(e.area), ent.area);
                snprintf(e.state, sizeof(e.state), "%s", ent.state);
                e.device_class = (uint8_t)zsrc_ha_device_class(ent.class_);
                zone_service_ha_catalog_add(&e);
            }
            if (ent.control) { /* the Devices screen's half (docs/features/24) */
                const dev_values_t values = dev_values_of(&ent.values);
                device_service_ha_catalog_add(ent.id, ent.name, ent.area, ent.domain,
                                              ent.state, &values);
            }
        }
        return true;
    }
    case PROTO_LINK_IN_STATE: { /* one watched entity changed over there */
        if (!c->peer_is_paired) {
            return false;
        }
        char id[ZSRC_REF_LEN_LINK];
        char state[24];
        bool avail = true;
        proto_link_values_t pv;
        if (!proto_link_parse_state(json, len, id, state, &avail, &pv)) {
            return false;
        }
        zone_service_ha_state(id, state, avail);
        const dev_values_t values = dev_values_of(&pv);
        device_service_ha_state(id, state, avail, &values);
        return true;
    }
    case PROTO_LINK_IN_CALL_RESULT: { /* a device action came back (docs/features/24) */
        if (!c->peer_is_paired) {
            return false;
        }
        char id[PROTO_LINK_CALL_ID_LEN];
        char error[24];
        bool ok = false;
        if (!proto_link_parse_call_result(json, len, id, &ok, error)) {
            return false;
        }
        device_service_ha_result((uint32_t)strtoul(id, NULL, 16), ok, error);
        return true;
    }
    case PROTO_LINK_IN_ZWAVE_SET: { /* a Z-Wave output from Home Assistant (ZW-036) */
        if (!c->peer_is_paired) {
            return false;
        }
        char id[PROTO_LINK_CALL_ID_LEN];
        char key[PROTO_ZW_KEY_LEN];
        int level = 0;
        if (!proto_link_parse_zwave_set(json, len, id, key, &level)) {
            return false;
        }
        const proto_zw_device_t *d = NULL;
        for (size_t i = 0; i < s_zw_n; i++) {
            if (strcmp(s_zw_list[i].key, key) == 0) {
                d = &s_zw_list[i];
            }
        }
        const char *error = "";
        bool ok = false;
        if (!s_have_zw || d == NULL) {
            error = "not_taken";
        } else if (d->kind == PROTO_ZW_GARAGE && level != 0) {
            error = "not_allowed"; /* a garage door opens only at the panel (HCTL-005) */
        } else if (d->kind == PROTO_ZW_SWITCH && level != 0 && level != 100) {
            error = "not_allowed";
        } else {
            ok = s_zw.command(s_zw.ctx, key, level, now_ms());
            error = ok ? "" : "not_taken";
        }
        pj_init(&w, buf, PROTO_PAYLOAD_MAX);
        proto_link_zwave_result(&w, id, ok, error);
        S.zw_next = 0u; /* its new state soon */
        return send_built(c, &w, buf);
    }
    case PROTO_LINK_IN_SETTINGS_SET: { /* a panel's settings from Home Assistant (HAI-017) */
        if (!c->peer_is_paired) {
            return false; /* no code needed: the pinned connection is the check */
        }
        if (!proto_link_parse_settings_set(json, len, &SB.set_req)) {
            return false;
        }
        return settings_request(c, &SB.set_req);
    }
    case PROTO_LINK_IN_HELLO:
        if (in->api != PROTO_LINK_API) {
            ESP_LOGW(TAG, "client speaks api %d", (int)in->api);
            return false; /* protocol.md: an unsupported major closes the link */
        }
        snprintf(c->hello_client, sizeof(c->hello_client), "%s", in->client);
        return send_hello(c);
    case PROTO_LINK_IN_PING:
        pj_init(&w, buf, PROTO_PAYLOAD_MAX);
        proto_link_pong(&w);
        return send_built(c, &w, buf);
    case PROTO_LINK_IN_PAIR_START: {
        uint8_t nonce[PROTO_LINK_NONCE_LEN];
        esp_fill_random(nonce, sizeof(nonce));
        const char *why = NULL;
        if (!proto_pair_on_start(&SB.pair, now_ms(), in->commit, c->peer_fp, nonce, &why)) {
            send_pair_result(c, why);
            return false;
        }
        snprintf(S.pairing_client, sizeof(S.pairing_client), "%s", c->hello_client);
        pj_init(&w, buf, PROTO_PAYLOAD_MAX);
        proto_link_pair_nonce(&w, nonce);
        return send_built(c, &w, buf);
    }
    case PROTO_LINK_IN_PAIR_REVEAL: {
        const char *why = NULL;
        if (!proto_pair_on_reveal(&SB.pair, now_ms(), in->nonce, &why)) {
            send_pair_result(c, why);
            return false;
        }
        /* both screens show the code now; wait for the person at the panel */
        return pairing_wait(c);
    }
    default:
        return true; /* unknown types ride along (forward compatibility) */
    }
}

/* The message loop. One read attempt per pass, at most POLL_MS long, so the pass also gets to
 * push state, run the pairing window and see a drop request. A header that arrives in pieces
 * keeps its bytes in the connection across passes and carries a deadline of its own; nothing
 * here can wait without one (R09). */
static void serve(conn_t *c)
{
    uint64_t idle_since = now_ms();
    c->hdr_got = 0;
    while (!cancelled()) {
        proto_pair_tick(&SB.pair, now_ms());
        if (!push_state(c)) {
            return; /* the paired client is owed the truth; a broken pipe ends it */
        }
        size_t n = 0;
        if (read_some(c, c->hdr + c->hdr_got, sizeof(c->hdr) - c->hdr_got, &n) == RX_CLOSED) {
            return;
        }
        if (n == 0u) { /* quiet this poll */
            if (c->hdr_got > 0u && now_ms() > c->hdr_deadline) {
                ESP_LOGW(TAG, "a frame header stopped half way: dropping the connection");
                return;
            }
            if (c->hdr_got == 0u && now_ms() - idle_since > IDLE_LIMIT_MS) {
                ESP_LOGI(TAG, "silent for 90 s: dropping the connection");
                return;
            }
            continue;
        }
        if (c->hdr_got == 0u) {
            c->hdr_deadline = now_ms() + RX_FRAME_MS; /* the frame has started */
        }
        c->hdr_got += n;
        if (c->hdr_got < sizeof(c->hdr)) {
            continue; /* a split header: the bytes stay, the deadline runs */
        }
        c->hdr_got = 0;
        idle_since = now_ms();
        uint32_t len = 0;
        if (!proto_link_frame_length(c->hdr, &len) || len > RX_MAX) {
            return; /* protocol.md: empty or oversized closes the link */
        }
        if (!read_exact(c, c->rx, len, now_ms() + RX_FRAME_MS)) {
            return;
        }
        proto_link_in_t in;
        if (!proto_link_parse((const char *)c->rx, len, &in)) {
            return; /* not a message: close */
        }
        if (!handle(c, &in, (const char *)c->rx, len)) {
            return;
        }
    }
}

/* Accept, handshake, check the certificate rules, serve. */
static void connection(void)
{
    conn_t c;
    memset(&c, 0, sizeof(c));
    mbedtls_net_init(&c.net);
    if (mbedtls_net_accept(&S.listen, &c.net, NULL, 0, NULL) != 0) {
        return;
    }
    c.rx = alarm_mem_slow_malloc(RX_MAX);
    if (c.rx == NULL) {
        mbedtls_net_free(&c.net);
        return;
    }
    /* A bounded write strategy for this socket (R09). lwIP turns SO_SNDTIMEO into
     * EWOULDBLOCK, which the mbedTLS socket layer reports as MBEDTLS_ERR_SSL_WANT_WRITE, so a
     * client that stops reading can never park this task inside send(): write_all() gets its
     * turn back every TX_SOCK_MS and can look at the deadlines and the drop flag. */
    const struct timeval tx_to = {.tv_sec = (time_t)(TX_SOCK_MS / 1000u),
                                  .tv_usec = (suseconds_t)((TX_SOCK_MS % 1000u) * 1000u)};
    if (setsockopt(c.net.fd, SOL_SOCKET, SO_SNDTIMEO, &tx_to, sizeof(tx_to)) != 0) {
        ESP_LOGW(TAG, "no send timeout on the client socket: refusing the connection");
        free(c.rx);
        mbedtls_net_free(&c.net);
        return;
    }
    mbedtls_ssl_init(&c.ssl);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_x509_crt_init(&c.cert);
    mbedtls_pk_init(&c.key);

    if (mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_x509_crt_parse_der(&c.cert, SB.cert, S.cert_len) != 0 ||
        mbedtls_pk_parse_key(&c.key, SB.key, S.key_len, NULL, 0) != 0 ||
        mbedtls_ssl_conf_own_cert(&c.conf, &c.cert, &c.key) != 0) {
        goto out;
    }
    /* Both sides always present a certificate; nothing chains to an authority. The pinned
     * fingerprint is the real check (HAI-002), so verification stays "optional" and the
     * fingerprint decides. */
    mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_read_timeout(&c.conf, POLL_MS);
    if (mbedtls_ssl_setup(&c.ssl, &c.conf) != 0) {
        goto out;
    }
    mbedtls_ssl_set_bio(&c.ssl, &c.net, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);

    const uint64_t hs_deadline = now_ms() + HANDSHAKE_MS;
    int r;
    while ((r = mbedtls_ssl_handshake(&c.ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE &&
            r != MBEDTLS_ERR_SSL_TIMEOUT) {
            goto out;
        }
        if (now_ms() > hs_deadline || cancelled()) {
            goto out;
        }
    }

    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c.ssl);
    if (peer == NULL) {
        ESP_LOGW(TAG, "client came without a certificate");
        goto out;
    }
    proto_sha256(peer->raw.p, peer->raw.len, c.peer_fp);
    xSemaphoreTake(S.lock, portMAX_DELAY);
    const bool paired = S.store.paired;
    c.peer_is_paired = paired && memcmp(c.peer_fp, S.store.pinned_fp, PROTO_LINK_FP_LEN) == 0;
    xSemaphoreGive(S.lock);
    if (paired && !c.peer_is_paired && SB.pair.state == PROTO_PAIR_IDLE) {
        /* one paired instance (HAI-002); a stranger may only talk during a pairing window */
        ESP_LOGW(TAG, "refusing a client that isn't the paired one");
        goto out;
    }

    refresh_config();
    if (c.peer_is_paired) {
        flag_set(&S.connected, true);
        S.synced = false; /* a fresh connection starts with a full snapshot */
        push_reset();     /* ... the whole Z-Wave list, the settings and the keypads */
        /* Events from before this connection aren't replayed yet, and actions queued for a
         * dead line answer nothing. xQueueReset is atomic against a producer; one posted in
         * the same instant simply rides along, and the full snapshot that follows tells the
         * client where things stand either way. */
        QueueHandle_t evq = queue_get(&S.ev_q);
        QueueHandle_t callq = queue_get(&S.call_q);
        if (evq != NULL) {
            (void)xQueueReset(evq);
        }
        if (callq != NULL) {
            (void)xQueueReset(callq);
        }
        flag_set(&S.ev_overflow, false);
        flag_set(&S.req_watch, true); /* and with the zones' entity list (M13) */
        /* LINK UP (R03): where source-level supervision would be told the Home Assistant
         * link is alive again. */
        zone_service_ha_link_state(true);
        device_service_ha_link(true);
    }
    serve(&c);

out:
    if (c.peer_is_paired) {
        /* LINK DOWN (R03): the hub *suppresses* per-input CHECK while the whole link is
         * down, so nothing here makes the loss visible on its own - it needs one
         * source-level supervision trouble instead (docs/features/22, ZSRC). */
        zone_service_ha_link_state(false);
        device_service_ha_link(false); /* device tiles grey out (HCTL-006) */
    }
    flag_set(&S.connected, false);
    if (SB.pair.state == PROTO_PAIR_WAIT_REVEAL || SB.pair.state == PROTO_PAIR_SHOW_CODE) {
        proto_pair_close(&SB.pair); /* the attempt's connection is gone */
    }
    mbedtls_ssl_close_notify(&c.ssl);
    mbedtls_net_free(&c.net);
    mbedtls_ssl_free(&c.ssl);
    mbedtls_ssl_config_free(&c.conf);
    mbedtls_x509_crt_free(&c.cert);
    mbedtls_pk_free(&c.key);
    heap_caps_free(c.rx);
}

/* ---- the task ------------------------------------------------------------------------------- */

static bool server_open(void)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)PROTO_LINK_PORT);
    mbedtls_net_init(&S.listen);
    if (mbedtls_net_bind(&S.listen, NULL, port, MBEDTLS_NET_PROTO_TCP) != 0) {
        ESP_LOGE(TAG, "can't listen on %s", port);
        return false;
    }
    return true;
}

static void server_close(void)
{
    mbedtls_net_free(&S.listen);
}

static void apply_enabled(void)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
    const bool on = S.store.enabled;
    xSemaphoreGive(S.lock);
    if (on && !S.server_up) {
        size_t cl = 0;
        size_t kl = 0;
        if (ha_cert_ensure(S.nvs, S.ident.panel_id, SB.cert, &cl, SB.key, &kl) != ESP_OK) {
            ESP_LOGE(TAG, "no certificate: the link stays off");
            return;
        }
        S.cert_len = cl;
        S.key_len = kl;
        proto_sha256(SB.cert, S.cert_len, S.own_fp);
        refresh_config();
        if (S.tx == NULL) {
            S.tx = alarm_mem_slow_malloc(PROTO_LINK_SNAPSHOT_MAX);
        }
        if (S.tx == NULL) {
            ESP_LOGE(TAG, "no transmit buffer: the link stays off");
            return;
        }
        if (server_open()) {
            S.server_up = true;
            mdns_apply(true);
            net_wifi_set_required(NET_WIFI_REQ_HA, true); /* a lost network is now worth a trouble; never
                                          * cleared here: MQTT may need it too */
            ESP_LOGI(TAG, "listening on %d as roboalarms-%s", PROTO_LINK_PORT, S.ident.panel_id);
        }
    } else if (!on && S.server_up) {
        proto_pair_close(&SB.pair);
        server_close();
        mdns_apply(false);
        S.server_up = false;
        ESP_LOGI(TAG, "switched off");
    }
}

/* An unpair that flash refused is not finished. Keep writing it down (R11): until it is
 * stored, a restart would hand the revoked client its trust back. */
static void revoke_retry(void)
{
    if (!flag_get(&S.revoke_pending) || now_ms() < S.revoke_next) {
        return;
    }
    S.revoke_next = now_ms() + REVOKE_RETRY_MS;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    const esp_err_t err = store_unpair(); /* clears revoke_pending when it lands */
    xSemaphoreGive(S.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "the unpair is on flash now");
    }
}

static void link_task(void *arg)
{
    (void)arg;
#if CONFIG_ALARM_FEATURE_MQTT
    net_mqtt_suppress_discovery(S.store.paired); /* HAI-010: no duplicate entities */
#endif
    apply_enabled();
    while (true) {
        if (flag_take(&S.req_apply)) {
            apply_enabled();
        }
        if (flag_take(&S.req_pair)) {
            if (S.server_up) {
                proto_pair_open(&SB.pair, now_ms(), S.own_fp);
                ESP_LOGI(TAG, "pairing open for 2 minutes");
            }
        }
        if (flag_take(&S.req_cancel)) {
            proto_pair_close(&SB.pair);
        }
        revoke_retry();
        flag_set(&S.req_drop, false);
        proto_pair_tick(&SB.pair, now_ms());
        if (!S.server_up) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        /* wait for a client, half a second at a time, so the flags above stay live */
        struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)(POLL_MS * 1000u)};
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(S.listen.fd, &rd);
        int n = select(S.listen.fd + 1, &rd, NULL, NULL, &tv);
        if (n > 0 && FD_ISSET(S.listen.fd, &rd)) {
            connection();
        }
    }
}

/* ---- public API ----------------------------------------------------------------------------- */

/* Fired under the zone lock when a binding to a Home Assistant entity changes: only the
 * flag, the link task sends the fresh watch list (zone_service.h's rule). */
static void on_watch_changed(void *ctx)
{
    (void)ctx;
    flag_set(&S.req_watch, true);
}

/* Everything the link task and the other tasks share, built privately and published only
 * once every step has worked: a failure frees what it made and publishes nothing (M02). */
static esp_err_t link_prepare(void)
{
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    QueueHandle_t ev = xQueueCreate(EV_QUEUE, sizeof(alarm_event_t));
    QueueHandle_t calls = xQueueCreate(CALL_QUEUE, sizeof(ha_call_t));
    QueueHandle_t sets = xQueueCreate(SET_ANSWERS, sizeof(ha_set_answer_t));
    nvs_handle_t nvs = 0;
    uint8_t mac[6] = {0};
    esp_err_t err = ESP_ERR_NO_MEM;
    if (lock != NULL && ev != NULL && calls != NULL && sets != NULL) {
        err = nvs_open("halink", NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            /* The P4 has no local Wi-Fi MAC: its radio lives on the C6. Use the P4's
             * factory identity, available before the hosted radio has started. Never
             * advertise the all-zero identity after an unsupported MAC lookup. */
            err = esp_efuse_mac_get_default(mac);
            if (err != ESP_OK) {
                nvs_close(nvs);
            }
        }
    }
    if (err != ESP_OK) {
        if (lock != NULL) {
            vSemaphoreDelete(lock);
        }
        if (ev != NULL) {
            vQueueDelete(ev);
        }
        if (calls != NULL) {
            vQueueDelete(calls);
        }
        if (sets != NULL) {
            vQueueDelete(sets);
        }
        return err;
    }
    S.nvs = nvs;
    store_load();
    proto_panel_id_from_mac(mac, S.ident.panel_id);
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(S.ident.fw, sizeof(S.ident.fw), "%s", app->version);
    snprintf(S.ident.model, sizeof(S.ident.model), "%s", s_model);
    snprintf(S.ident.prefix, sizeof(S.ident.prefix), "%s", PROTO_PREFIX_DEFAULT);
    snprintf(S.name, sizeof(S.name), "RoboAlarms Panel");
    proto_link_setq_init(&SB.setq);
    /* The lock last: the status and the setters wait for it, and find the store loaded. */
    __atomic_store_n(&S.lock, lock, __ATOMIC_RELEASE);
    /* The hand-over queues exist before anything is told where to post (R10). They are
     * published with a release store: whoever picks the handle up sees a finished queue. */
    __atomic_store_n(&S.ev_q, ev, __ATOMIC_RELEASE);
    __atomic_store_n(&S.call_q, calls, __ATOMIC_RELEASE);
    __atomic_store_n(&S.set_q, sets, __ATOMIC_RELEASE);
    zone_service_ha_set_watch_changed(on_watch_changed, NULL);
    device_service_set_sender(link_send_call, NULL);
    return ESP_OK;
}

esp_err_t ha_link_start(void)
{
    /* Started once: S.task is set only when the link task exists. */
    if (S.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Once published, the lock, the queues and the two hooks are in other tasks' hands (the
     * status and settings calls hold the lock, the alarm task posts events, device_service calls
     * the sender), so they can't be taken back safely. Without the link task they do nothing
     * harmful - the sender refuses while no client is connected, and only the task connects one -
     * so a start whose task could not be created keeps them, and the retry uses them rather
     * than making a second set (M02). */
    if (__atomic_load_n(&S.lock, __ATOMIC_ACQUIRE) == NULL) {
        const esp_err_t err = link_prepare();
        if (err != ESP_OK) {
            return err;
        }
    }

    /* TLS handshakes run on this stack */
    if (xTaskCreate(link_task, "ha_link", 12288, NULL, 5, &S.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ha_link_get_status(ha_link_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (S.lock == NULL) {
        return;
    }
    xSemaphoreTake(S.lock, portMAX_DELAY);
    out->enabled = S.store.enabled;
    out->paired = S.store.paired;
    snprintf(out->client, sizeof(out->client),
             "%s", SB.pair.state != PROTO_PAIR_IDLE ? S.pairing_client : S.store.client);
    xSemaphoreGive(S.lock);
    out->unpair_unsaved = flag_get(&S.revoke_pending);
    snprintf(out->panel_id, sizeof(out->panel_id), "%s", S.ident.panel_id);
    out->port = PROTO_LINK_PORT;

    const uint64_t now = now_ms();
    proto_pair_t *p = &SB.pair; /* reads are safe enough for a status line */
    if (!out->enabled) {
        out->state = HA_LINK_OFF;
        return;
    }
    net_wifi_status_t net;
    net_wifi_get_status(&net);
    if (net.state != NET_WIFI_CONNECTED) {
        out->state = HA_LINK_NO_NETWORK;
        return;
    }
    if (p->state == PROTO_PAIR_SHOW_CODE) {
        out->state = HA_LINK_PAIRING_CODE;
        snprintf(out->code, sizeof(out->code), "%s", p->code);
        out->window_s = p->deadline_ms > now ? (uint32_t)((p->deadline_ms - now) / 1000u) : 0u;
        return;
    }
    if (p->state == PROTO_PAIR_WINDOW || p->state == PROTO_PAIR_WAIT_REVEAL) {
        out->state = HA_LINK_PAIRING;
        out->window_s = p->deadline_ms > now ? (uint32_t)((p->deadline_ms - now) / 1000u) : 0u;
        return;
    }
    if (!out->paired) {
        out->state = HA_LINK_READY;
    } else {
        out->state = flag_get(&S.connected) ? HA_LINK_CONNECTED : HA_LINK_PAIRED_IDLE;
    }
}

esp_err_t ha_link_set_enabled(bool on)
{
    if (S.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.store.enabled = on; /* takes effect now whatever flash says */
    esp_err_t err = nvs_set_u8(S.nvs, "on", on ? 1u : 0u);
    if (err == ESP_OK) {
        err = nvs_commit(S.nvs);
    }
    xSemaphoreGive(S.lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the link is %s now, but that wasn't stored (%s): a restart undoes it",
                 on ? "on" : "off", esp_err_to_name(err));
    }
    flag_set(&S.req_apply, true);
    if (!on) {
        flag_set(&S.req_drop, true);
    }
    return err;
}

esp_err_t ha_link_pair_begin(void)
{
    ha_link_status_t st;
    ha_link_get_status(&st);
    if (!st.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    flag_set(&S.req_pair, true);
    return ESP_OK;
}

void ha_link_pair_decide(bool allow)
{
    __atomic_store_n(&S.req_decision, allow ? 1 : 2, __ATOMIC_RELEASE);
}

void ha_link_pair_cancel(void)
{
    flag_set(&S.req_cancel, true);
}

/* A settings_set's answer, from any task: a copy into the queue, never a wait (R10). */
static void set_answer_post(uint32_t ticket, ha_link_set_t err, const uint8_t *why, size_t n)
{
    QueueHandle_t q = queue_get(&S.set_q);
    if (q == NULL || ticket == 0u) {
        return;
    }
    ha_set_answer_t a;
    memset(&a, 0, sizeof(a));
    a.ticket = ticket;
    a.err = (uint8_t)err;
    for (size_t i = 0; why != NULL && i < n && i < PROTO_PREFS_F_LAST; i++) {
        a.why[i] = why[i];
    }
    (void)xQueueSend(q, &a, 0); /* full: the request is answered "timeout" in time */
}

void ha_link_settings_done(uint32_t ticket, const uint8_t *why, size_t n)
{
    set_answer_post(ticket, HA_LINK_SET_SENT, why, n);
}

void ha_link_settings_failed(uint32_t ticket, ha_link_set_t err)
{
    set_answer_post(ticket, err != HA_LINK_SET_SENT ? err : HA_LINK_SET_BUSY, NULL, 0u);
}

esp_err_t ha_link_unpair(void)
{
    if (S.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(S.lock, portMAX_DELAY);
    const esp_err_t err = store_unpair();
    xSemaphoreGive(S.lock);
    /* The session is revoked either way: the connection goes, the fingerprint is out of RAM,
     * and mDNS stops saying "paired". Only the promise that it survives a restart depends on
     * flash, which is what this returns (R11). */
    flag_set(&S.req_drop, true);
    if (S.mdns_up) {
        mdns_txt();
    }
#if CONFIG_ALARM_FEATURE_MQTT
    net_mqtt_suppress_discovery(false); /* MQTT discovery may speak for the panel again */
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unpaired here, but not on flash (%s): trying again every %us",
                 esp_err_to_name(err), (unsigned)(REVOKE_RETRY_MS / 1000u));
    }
    return err;
}

/* ---- console -------------------------------------------------------------------------------- */

#if CONFIG_ALARM_SERVICE_CONSOLE
#include "esp_console.h"

static const char *state_word(ha_link_state_t s)
{
    switch (s) {
    case HA_LINK_OFF: return "off";
    case HA_LINK_NO_NETWORK: return "waiting for Wi-Fi";
    case HA_LINK_READY: return "ready, not paired";
    case HA_LINK_PAIRING: return "pairing window open";
    case HA_LINK_PAIRING_CODE: return "showing the pairing code";
    case HA_LINK_PAIRED_IDLE: return "paired, not connected";
    case HA_LINK_CONNECTED: return "connected";
    default: return "?";
    }
}

static int cmd_ha(int argc, char **argv)
{
#if CONFIG_HA_LINK_DIAGNOSTICS
    if (argc == 3 && strcmp(argv[1], "storefail") == 0) {
        const bool fail = strcmp(argv[2], "on") == 0;
        if (!fail && strcmp(argv[2], "off") != 0) {
            return 1;
        }
        flag_set(&ha_test_store_fail, fail);
        printf("trust-store failure injection %s\n", fail ? "on" : "off");
        return 0;
    }
#endif
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        return ha_link_set_enabled(true) == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        return ha_link_set_enabled(false) == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "pair") == 0) {
        if (ha_link_pair_begin() != ESP_OK) {
            printf("switch the link on first (ha on)\n");
            return 1;
        }
        printf("pairing open for 2 minutes: add the panel in Home Assistant now\n");
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "allow") == 0 || strcmp(argv[1], "reject") == 0)) {
        ha_link_pair_decide(argv[1][0] == 'a');
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "cancel") == 0) {
        ha_link_pair_cancel();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "unpair") == 0) {
        const esp_err_t err = ha_link_unpair();
        if (err != ESP_OK) {
            printf("revoked here, but not stored (%s): a restart would bring it back, "
                   "retrying\n", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }
    ha_link_status_t st;
    ha_link_get_status(&st);
    printf("Home Assistant link: %s\n", state_word(st.state));
    printf("  task stack minimum free: %u bytes\n",
           (unsigned)uxTaskGetStackHighWaterMark(S.task));
    printf("  panel id %s, port %u, %s\n", st.panel_id, (unsigned)st.port,
           st.enabled ? "enabled" : "disabled");
    if (st.paired) {
        printf("  paired with %s\n", st.client[0] ? st.client : "(unknown client)");
    }
    if (st.unpair_unsaved) {
        printf("  WARNING: the last unpair isn't on flash yet - a restart would undo it\n");
    }
    if (st.code[0] != '\0') {
        printf("  code on both screens: %s (%us left; ha allow / ha reject)\n", st.code,
               (unsigned)st.window_s);
    } else if (st.window_s > 0u) {
        printf("  pairing window: %us left\n", (unsigned)st.window_s);
    }
    return 0;
}

esp_err_t ha_link_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ha",
        .help = "Home Assistant link: status, on, off, pair, allow, reject, cancel, unpair",
        .hint = "[on|off|pair|allow|reject|cancel|unpair]",
        .func = cmd_ha,
    };
    return esp_console_cmd_register(&cmd);
}
#else
esp_err_t ha_link_console_register(void)
{
    return ESP_OK;
}
#endif

#endif
