/*
 * panel_ota.c - firmware updates (see panel_ota.h, docs/features/13).
 *
 * One task ("ota") owns the slow work: release checks, downloads, the probation timer after
 * an update and the wait for an idle system before a restart. The writer can also be driven
 * by another task (the console's serial upload); s_lock guards the shared state.
 *
 * Two things this file is careful about:
 *   - Whether the system may be restarted is asked of the alarm task itself
 *     (alarm_service_call), so the answer and the engine's state belong together, and it is
 *     asked again in the last moment before the boot slot is switched (OTA-004, R17). The
 *     alarm task is never held while anything slow runs here, so a life-safety event is never
 *     waiting on an update.
 *   - A failed release check is only the last result on screen: it never stops the panel from
 *     looking again (OTA-015, R16).
 *   - The update server (OTA-017..OTA-025) is a domain kept with the one signing key the installer
 *     accepted for it (NVS "trust"); everything that changes it is stored by the update task,
 *     never by the caller's task (a task with its stack in PSRAM must not write NVS).
 */
#include "panel_ota.h"
#include "alarm_mem.h"
#include "i18n/i18n.h"
#include "ui_backend/ui_why_words.h"

#include "esp_attr.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm/alarm_snapshot.h"
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_proto/proto_json.h"
#include "alarm_service.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "nvs.h"
#include "alarm_proto/ota_retry.h"
#include "panel_log.h"
#include "panel_ota_internal.h"
#include "alarm_proto/ota_profile.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define NVS_NS        "ota"
#define TASK_STACK    8192u
#define TASK_PRIORITY 3
#define ANSWER_MAX    8192u /* a check answer: the release, its notes and the language packs */
#define KEYS_MAX      4096u /* GET /api/v1/keys: a few keys with their PEM */
#define REQUEST_MAX   2048u /* the check's request body */
#define HTTP_BUF      4096u
/* reading an answer's body (OTA-027, body_fill()) */
#define BODY_PIECE     256u
#define BODY_READ_MS   1000
#define BODY_WINDOW_MS 60000u
#define BODY_FLOOR     (60u * 1024u) /* bytes per window */
#define SMALL_BODY_MS  60000u        /* a check's, the keys' or a language list's whole body */
#define FIRST_CHECK_MS (3u * 60u * 1000u)
#define RESTART_HOLD_MS 30000u /* after a restart was given up: let the house settle first */
#define HEADER_LEN \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

#define NB_CHECK   (1u << 0)
#define NB_INSTALL (1u << 1)
#define NB_APPLY   (1u << 2)
#define NB_STATUS  (1u << 3)
#define NB_CP      (1u << 4)
#define NB_SERVER  (1u << 5) /* a server check, a kept server or a channel to store */
#define NB_LANGS   (1u << 6) /* the running version's language packs were asked for */

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define CHIP_REV "v1"
#elif CONFIG_IDF_TARGET_ESP32P4
#define CHIP_REV "v3"
#else
#define CHIP_REV NULL
#endif

/* This panel's "device" in a check (OTA-018): CONFIG_PANEL_OTA_DEVICE when set - the product name
 * the update server's catalogue uses ("crowpanel_dhe04107d") - else the board's id
 * (board_info()->id, passed in by the app; docs/features/33 step 8). */
static const char *s_device = "";

void panel_ota_set_device(const char *id)
{
    s_device = id != NULL ? id : "";
}

static const char *device_name(void)
{
    return CONFIG_PANEL_OTA_DEVICE[0] != '\0' ? CONFIG_PANEL_OTA_DEVICE : s_device;
}

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
} listener_t;

/* The image being written. */
typedef struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    uint32_t size;
    uint32_t written; /* bytes received (the header is held back until it's checked) */
    uint8_t header[HEADER_LEN];
    bool header_ok;
    bool allow_older;
    bool has_sha;
    uint8_t sha[32];
    psa_hash_operation_t hash;
    char version[32];
} writer_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static panel_ota_status_t s_st;
static listener_t s_listeners[4];
static uint32_t s_required;
static uint64_t s_start_ms;
static uint64_t s_probation_until_ms;
static uint64_t s_probation_deadline_ms;
static writer_t s_w;
static bool s_busy; /* a writer session or a co-processor update runs */
static bool s_remote_install;
static const esp_partition_t *s_ready;
static alarm_source_t s_apply_src;
static uint8_t s_apply_user;
static volatile bool s_cancelled;   /* give up the restart at the next gate (R17) */
static volatile bool s_slot_selected; /* esp_ota_set_boot_partition() ran, no restart yet */
static uint64_t s_restart_hold_ms;  /* don't try to restart again before this */
/* The Wi-Fi module's image is sent (and activated): the panel restarts once everything is quiet
 * (OTA-004). Until then the C6 keeps running its old image and the run keeps the machinery. */
static bool s_cp_restart;
static alarm_source_t s_cp_src;
static uint8_t s_cp_user;
static proto_fw_artifact_t s_art;   /* from the last check (the update task only) */
static bool s_have_art;
static bool s_install_older;
static alarm_source_t s_install_src;
static uint8_t s_install_user;
static uint32_t s_check_fails;      /* release checks that failed in a row (update task) */
/* The manifest's language packs from the last good check (under the lock). In PSRAM: 16 entries
 * are 5.5 KB and nothing here runs with the cache off (docs/bugs/B001). */
static EXT_RAM_BSS_ATTR proto_fw_language_t s_langs[PANEL_OTA_LANGUAGES_MAX];
static size_t s_langs_n;
static uint32_t s_langs_seq;       /* the asset packs' list too: they change together */
/* The asset packs from the same check (OPT-007), under the lock. */
static EXT_RAM_BSS_ATTR proto_fw_asset_t s_assets[PANEL_OTA_ASSETS_MAX];
static size_t s_assets_n;
/* A language added later (OTA-025): the running version's packs, asked of the update task.
 * Under the lock; s_rlangs_n is SIZE_MAX when the last request failed. */
static EXT_RAM_BSS_ATTR proto_fw_language_t s_rlangs[PANEL_OTA_LANGUAGES_MAX];
static size_t s_rlangs_n;
static EXT_RAM_BSS_ATTR proto_fw_asset_t s_rassets[PANEL_OTA_ASSETS_MAX]; /* the same answer's */
static size_t s_rassets_n;
static uint32_t s_rlangs_asked; /* the newest request's ticket (0 = never asked) */
static uint32_t s_rlangs_done;  /* the newest answered */
static volatile bool s_check_now;   /* the server or channel changed: check at the next tick */

/* The update server (OTA-017..OTA-025), under the lock unless said otherwise. */
static proto_fwsrv_trust_t s_trust;  /* the server in use and the key accepted for it */
static uint8_t s_own_key[32];        /* the running image's signing key (set once at start) */
static bool s_own_key_ok;
static char s_own_key_name[PANEL_OTA_KEY_NAME_LEN]; /* its name on the server, "" = not known */
static panel_ota_srv_check_t s_srv;  /* a new server's check */
static bool s_srv_pending;           /* asked; the update task hasn't started it */
static proto_fwsrv_trust_t s_commit; /* a server kept, for the update task to store */
static bool s_commit_pending;
static alarm_source_t s_commit_src;
static uint8_t s_commit_user;
static bool s_chan_pending;          /* the channel changed: the update task stores it */
/* The facts a check sends (set before the start, then read only). */
#define FACT_PARTS_MAX 4u
static char s_board_rev[16];
static char s_parts[FACT_PARTS_MAX][16];
static size_t s_parts_n;
static volatile panel_ota_locales_fn s_locales_fn;

static bool run_busy_locked(void);

/* The run (OTA-026..OTA-031). s_stop is read without the lock by the update task between the
 * steps of a download (the writer's state is the update task's alone while it writes, so a stop
 * can't abort it from another task: the flag asks the update task to). */
static volatile bool s_stop;
static alarm_source_t s_stop_src;   /* who asked (under the lock) */
static uint8_t s_stop_user;
static bool s_install_pending;      /* Install accepted, the update task hasn't started it yet */
static uint64_t s_install_wait_until; /* it waits for a held update machinery until then (OTA-030) */
static panel_ota_kind_t s_ready_kind; /* where the verified image s_ready came from */
static uint64_t s_updated_mono;     /* last UPDATED without a clock: when, in uptime */
static uint32_t s_listened_seq;     /* the status the listeners last saw (update task) */
static uint64_t s_listened_ms;

/* What the update task keeps in NVS about a run, so it outlives the restart. One layout for the
 * three keys below; a record of another size or schema is ignored (an older or newer firmware's),
 * never misread. Written only from tasks with an internal-RAM stack (the update task, start-up).
 *   "try": the attempt, written after the last gate, just before the boot slot switches: the
 *          version it starts, the one it replaces and the image's ELF digest. The new image reads
 *          `previous` from it; the old one, back after a rollback, reports the version only when
 *          the failed slot still holds that very image (OTA-031, B: a later download overwrites
 *          the slot and must not be reported as a second failed update).
 *   "upd": the update this image confirmed (OTA-029 `last` UPDATED, rebuilt at boot with the
 *          event log's "updated" record for its time).
 *   "rbv": a rollback seen, kept until a newer version is offered or an update is confirmed. */
#define OTA_REC_SCHEMA 1u
typedef struct {
    uint8_t schema;
    uint8_t by_kind;  /* alarm_source_t of whoever asked */
    uint16_t by_id;
    uint8_t by_user;
    uint8_t pad[3];
    uint32_t at;      /* Unix time, 0 = unknown */
    char version[32];
    char from[32];
    uint8_t elf[32];
} ota_rec_t;

/* Health heartbeats (OTA-005). Written by whichever task does the work, read by the update
 * task; one aligned 32-bit word each, so no lock is needed on either side. Times are
 * alarm_service_now() truncated to 32 bits and always compared as differences (wrap-safe);
 * "first" 0 means the part has never reported. */
static volatile uint32_t s_health;                                 /* bits ever reported */
static volatile uint32_t s_health_first[PANEL_OTA_HEALTH_COUNT];
static volatile uint32_t s_health_last[PANEL_OTA_HEALTH_COUNT];
static volatile uint32_t s_health_stale; /* required bits not reporting (update task writes) */

static const char *const HEALTH_NAME[PANEL_OTA_HEALTH_COUNT] = {
    "the alarm engine", "the event log", "the screen", "sensor input", "touch",
};

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void notify(uint32_t bits)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

/* Call with the lock held after changing s_st. */
static void changed_locked(void)
{
    s_st.seq++;
    notify(NB_STATUS);
}

static void set_phase(panel_ota_phase_t phase, const char *error)
{
    lock();
    s_st.phase = phase;
    if (phase != PANEL_OTA_RECEIVING) {
        s_st.percent = -1;
    }
    if (phase != PANEL_OTA_WAITING) {
        s_st.waiting_for[0] = '\0';
    }
    snprintf(s_st.error, sizeof(s_st.error), "%s", error != NULL ? error : "");
    changed_locked();
    unlock();
}

/* The fixed reasons are marked I18N_NOOP_C("update", ...) where they are written: the console,
 * the Panel Manager and MQTT get the English, the touchscreen looks the sentence up in its
 * language (docs/features/37). Reasons with a value in them stay English. */
static void reason(char *why, size_t len, const char *text)
{
    if (why != NULL && len > 0u) {
        snprintf(why, len, "%s", text);
    }
}

/* ---- may the panel restart? (OTA-004) -------------------------------------------------------- */

typedef struct {
    bool allowed;
    const char *why; /* a literal from proto_fw_install_allowed(), so it outlives the call */
} idle_gate_t;

/* Big enough to keep off an 8 KB stack. Used inside gate_on_alarm_task() and
 * restart_on_alarm_task(), which the alarm task
 * runs one at a time, and by the fall-back below, which only happens when that task isn't
 * running at all - so there is never a second user. */
static alarm_snapshot_t s_gate_snap ALARM_SLOW_BSS; /* task-only: slow memory */

/* Runs on the alarm task: nothing can arm, sound or expire between the snapshot and the
 * answer. Short and allocation-free - the engine must never wait on an update. */
static void gate_on_alarm_task(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    idle_gate_t *g = (idle_gate_t *)ctx;
    alarm_engine_snapshot(e, &s_gate_snap);
    g->allowed = proto_fw_install_allowed(&s_gate_snap, &g->why);
}

bool panel_ota_system_idle(const char **why)
{
    idle_gate_t g = {.allowed = false, .why = ""};
    if (alarm_service_call(gate_on_alarm_task, &g) != ESP_OK) {
        /* The alarm task isn't running (bring-up): the published snapshot is all there is. */
        alarm_service_get_snapshot(&s_gate_snap);
        g.allowed = proto_fw_install_allowed(&s_gate_snap, &g.why);
    }
    if (why != NULL) {
        *why = g.why;
    }
    return g.allowed;
}

/* The same question plus "has anyone given up on this update?" (R17). */
static bool restart_allowed(const char **why)
{
    if (s_stop) {
        *why = "Stopped"; /* never shown: the stop ends the run */
        return false;
    }
    if (s_cancelled) {
        *why = I18N_NOOP_C("update", "Cancelled");
        return false;
    }
    return panel_ota_system_idle(why);
}

/* ---- the restart itself, on the alarm task (OTA-004, R17, S07) -------------------------------- */

/* A restart asked of the alarm task finds other messages waiting behind it this many times in a
 * row: it is put off like a busy house. Each new ask queues behind those messages, so the engine
 * has seen them by the time it is answered; only a steady stream keeps it from going. */
#define FINAL_TRIES 10u

typedef struct {
    const char *said; /* logged just before the restart */
    bool queued;      /* out: other messages were waiting behind the ask */
    const char *why;  /* out: why not (a literal) */
} final_gate_t;

/* Runs on the alarm task, so no engine input can be handled between the check and the restart:
 * the very state that answered "idle" is the one the panel restarts in. Nothing waits here and
 * nothing is written - the boot slot was switched before (on the update task) and is switched
 * back there if the answer is no. esp_restart() doesn't return; the update task's call never
 * comes back. */
static void restart_on_alarm_task(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    final_gate_t *g = (final_gate_t *)ctx;
    if (s_stop) {
        g->why = "Stopped"; /* never shown: the stop ends the run */
        return;
    }
    if (s_cancelled) {
        g->why = I18N_NOOP_C("update", "Cancelled");
        return;
    }
    alarm_engine_snapshot(e, &s_gate_snap);
    if (!proto_fw_install_allowed(&s_gate_snap, &g->why)) {
        return;
    }
    if (alarm_service_waiting() != 0u) {
        g->queued = true; /* an input or a command handed in already: let the engine see it */
        g->why = "messages waiting for the alarm task";
        return;
    }
    ESP_LOGW(TAG, "%s", g->said);
    esp_restart();
}

/* The last gate and the restart in one step on the alarm task (S07). Returns only when the panel
 * did not restart; why says why (a literal). Without an alarm task (bring-up) nothing but this
 * start-up can change the engine, and the old way stands: the gate, then the restart here. */
static void restart_via_alarm_task(const char *said, const char **why)
{
    for (unsigned i = 0; i < FINAL_TRIES; i++) {
        final_gate_t g = {.said = said, .queued = false, .why = ""};
        const esp_err_t err = alarm_service_call(restart_on_alarm_task, &g);
        if (err == ESP_ERR_INVALID_STATE) {
            if (restart_allowed(why)) {
                ESP_LOGW(TAG, "%s", said);
                esp_restart();
            }
            return;
        }
        if (err != ESP_OK) {
            *why = "the alarm task did not answer";
            return;
        }
        *why = g.why;
        if (!g.queued) {
            return; /* not idle, stopped or cancelled */
        }
    }
    ESP_LOGW(TAG, "the alarm task stayed busy: restart put off");
}

void panel_ota_version_copy(char *out, size_t cap, const char *field, size_t field_len)
{
    snprintf(out, cap, "%.*s", (int)strnlen(field, field_len), field);
}

/* ---- status ---------------------------------------------------------------------------------- */

void panel_ota_get_status(panel_ota_status_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->percent = -1;
        return;
    }
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    lock();
    snprintf(s_st.cp_running, sizeof(s_st.cp_running), "%s", ws.coproc_fw);
    *out = s_st;
    unlock();
}

uint32_t panel_ota_seq(void)
{
    return s_st.seq;
}

esp_err_t panel_ota_add_listener(void (*fn)(void *ctx), void *ctx)
{
    for (size_t i = 0; i < sizeof(s_listeners) / sizeof(s_listeners[0]); i++) {
        if (s_listeners[i].fn == NULL) {
            s_listeners[i].ctx = ctx;
            s_listeners[i].fn = fn;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

/* The update task only. Listeners also run during a download and before the restart (OTA-026),
 * not only between the update task's jobs. */
static void run_listeners(void)
{
    s_listened_seq = s_st.seq;
    s_listened_ms = alarm_service_now();
    for (size_t i = 0; i < sizeof(s_listeners) / sizeof(s_listeners[0]); i++) {
        if (s_listeners[i].fn != NULL) {
            s_listeners[i].fn(s_listeners[i].ctx);
        }
    }
}

/* From a loop on the update task: at most once a second, and only when something changed. */
static void run_listeners_if_changed(void)
{
    if (s_st.seq != s_listened_seq && alarm_service_now() - s_listened_ms >= 1000u) {
        run_listeners();
    }
}

/* Unix time now, 0 while the clock isn't set. */
static uint32_t now_unix(void)
{
    const time_t now = time(NULL);
    return now > 1600000000 ? (uint32_t)now : 0u;
}

/* A run starts (OTA-026): call with the lock held. */
static void run_begin_locked(panel_ota_kind_t kind, panel_ota_stage_t stage, const char *target,
                             const char *previous)
{
    s_st.kind = kind;
    s_st.stage = stage;
    snprintf(s_st.target, sizeof(s_st.target), "%s", target != NULL ? target : "");
    snprintf(s_st.previous, sizeof(s_st.previous), "%s", previous != NULL ? previous : "");
    s_st.bytes = 0;
    s_st.size = 0;
    s_st.reason[0] = '\0';
    s_st.failed_install = false;
}

/* The run is over, whichever way: call with the lock held. During the self-test `previous`
 * stays: it is what a failure returns to. */
static void run_end_locked(void)
{
    s_st.kind = PANEL_OTA_KIND_NONE;
    s_st.stage = PANEL_OTA_STAGE_NONE;
    s_st.target[0] = '\0';
    if (!s_st.probation) {
        s_st.previous[0] = '\0';
    }
    s_st.bytes = 0;
    s_st.size = 0;
    s_st.reason[0] = '\0';
}

/* OTA-029: call with the lock held. */
static void set_last_locked(panel_ota_last_result_t result, const char *version, const char *from,
                            uint32_t at, const char *why, const alarm_source_t *by, uint8_t user)
{
    panel_ota_last_t *l = &s_st.last;
    memset(l, 0, sizeof(*l));
    l->result = result;
    snprintf(l->version, sizeof(l->version), "%s", version != NULL ? version : "");
    snprintf(l->from, sizeof(l->from), "%s", from != NULL ? from : "");
    l->at = at;
    snprintf(l->reason, sizeof(l->reason), "%s", why != NULL ? why : "");
    l->by_src = by != NULL ? *by : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    l->by_user = user;
    if (result == PANEL_OTA_LAST_UPDATED) {
        s_updated_mono = alarm_service_now();
    }
}

/* ---- what outlives the restart (ota_rec_t) --------------------------------------------------- */

static bool rec_get(const char *key, ota_rec_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    const esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*out) || out->schema != OTA_REC_SCHEMA) {
        memset(out, 0, sizeof(*out));
        return false; /* none, or another firmware's layout: ignored, never misread */
    }
    out->version[sizeof(out->version) - 1u] = '\0';
    out->from[sizeof(out->from) - 1u] = '\0';
    return true;
}

static void rec_put(const char *key, const ota_rec_t *rec)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, key, rec, sizeof(*rec)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void rec_erase(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(h, key) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void rec_fill(ota_rec_t *r, const char *version, const char *from, const uint8_t *elf,
                     const alarm_source_t *by, uint8_t user, uint32_t at)
{
    memset(r, 0, sizeof(*r));
    r->schema = OTA_REC_SCHEMA;
    if (by != NULL) {
        r->by_kind = by->kind;
        r->by_id = by->id;
    }
    r->by_user = user;
    r->at = at;
    snprintf(r->version, sizeof(r->version), "%s", version != NULL ? version : "");
    snprintf(r->from, sizeof(r->from), "%s", from != NULL ? from : "");
    if (elf != NULL) {
        memcpy(r->elf, elf, sizeof(r->elf));
    }
}

static alarm_source_t rec_by(const ota_rec_t *r)
{
    return (alarm_source_t){.kind = r->by_kind, .id = r->by_id};
}

/* ---- health heartbeats (OTA-005) ------------------------------------------------------------- */

void panel_ota_healthy(panel_ota_health_t what)
{
    uint32_t now = (uint32_t)alarm_service_now();
    if (now == 0u) {
        now = 1u; /* 0 is reserved for "never reported" */
    }
    for (unsigned b = 0; b < PANEL_OTA_HEALTH_COUNT; b++) {
        if (((uint32_t)what & (1u << b)) == 0u) {
            continue;
        }
        if (s_health_first[b] == 0u) {
            s_health_first[b] = now;
        }
        s_health_last[b] = now;
    }
    s_health |= (uint32_t)what;
}

uint32_t panel_ota_health_stale(void)
{
    return s_health_stale;
}

/* When each heartbeat stamp was last seen to move, in 64-bit time (the update task only). */
static uint32_t s_health_seen[PANEL_OTA_HEALTH_COUNT];
static uint64_t s_health_moved[PANEL_OTA_HEALTH_COUNT];

/* Required parts that are not reporting: never did, or have gone quiet. The update task only. */
static uint32_t health_missing(uint64_t now)
{
    const uint32_t now_ms = (uint32_t)now;
    uint32_t missing = 0;
    for (unsigned b = 0; b < PANEL_OTA_HEALTH_COUNT; b++) {
        const uint32_t bit = 1u << b;
        const uint32_t last = s_health_last[b];
        if (last != s_health_seen[b] || s_health_moved[b] == 0u) {
            s_health_seen[b] = last;
            s_health_moved[b] = now;
        }
        if ((s_required & bit) == 0u) {
            continue;
        }
        /* The heartbeats are stamped by other tasks on the other core, so one may land after
         * now_ms was read: a signed age treats that as fresh, where the unsigned difference
         * wrapped to four billion ms and reported a working part as missing - which, on
         * probation, rolled a good image back. */
        const int32_t age = (int32_t)(now_ms - last);
        /* The same 32-bit stamp of a part silent for 24.8 days reads as "ahead", and so as
         * fresh: the stopped part would look as if it ran again. When the stamp last moved is
         * kept in 64 bits above, so a part that stays silent stays missing (sampled about once
         * a second, hence the generous bound). */
        const bool still = now - s_health_moved[b] > 2u * (uint64_t)PANEL_OTA_HEALTH_STALE_MS;
        if (s_health_first[b] == 0u || age > (int32_t)PANEL_OTA_HEALTH_STALE_MS || still) {
            missing |= bit;
        }
    }
    return missing;
}

/* Required parts that report, but haven't been reporting long enough to count as running. */
static uint32_t health_unproven(void)
{
    uint32_t young = 0;
    for (unsigned b = 0; b < PANEL_OTA_HEALTH_COUNT; b++) {
        const uint32_t bit = 1u << b;
        if ((s_required & bit) == 0u || s_health_first[b] == 0u) {
            continue;
        }
        if ((uint32_t)(s_health_last[b] - s_health_first[b]) < PANEL_OTA_HEALTH_SPAN_MS) {
            young |= bit;
        }
    }
    return young;
}

static void health_text(uint32_t mask, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (unsigned b = 0; b < PANEL_OTA_HEALTH_COUNT && n + 1u < cap; b++) {
        if ((mask & (1u << b)) == 0u) {
            continue;
        }
        const int w = snprintf(out + n, cap - n, "%s%s", n != 0u ? ", " : "", HEALTH_NAME[b]);
        if (w < 0) {
            break;
        }
        n += (size_t)w;
        if (n + 1u >= cap) {
            break;
        }
    }
}

/* R18: a part of the panel that stops running is a fault at any time, not only on probation.
 * The update task can only say so; turning it into a system trouble belongs to the platform
 * (panel_ota_health_stale). */
static void health_watch(uint64_t now)
{
    if (s_required == 0u || now < s_start_ms + PANEL_OTA_HEALTH_GRACE_MS) {
        return; /* everything is still coming up */
    }
    const uint32_t missing = health_missing(now);
    if (missing == s_health_stale) {
        return;
    }
    char what[96];
    if (missing != 0u) {
        health_text(missing, what, sizeof(what));
        ESP_LOGE(TAG, "not running: %s - the panel may not be protecting the house", what);
    } else {
        ESP_LOGW(TAG, "every part of the panel reports again");
    }
    s_health_stale = missing;
}

/* ---- settings -------------------------------------------------------------------------------- */

/* The status' copy of the server (call with the lock held). source keeps the address for the
 * callers that still read it (BL-186). */
static void server_to_status_locked(void)
{
    snprintf(s_st.server, sizeof(s_st.server), "%s", s_trust.server);
    s_st.official = s_trust.server[0] != '\0' &&
                    strcmp(s_trust.server, CONFIG_PANEL_OTA_DEFAULT_DOMAIN) == 0;
    if (!proto_fwsrv_base_url(s_trust.server, s_st.source, sizeof(s_st.source))) {
        s_st.source[0] = '\0';
    }
}

/* This build's own server with no extra key (OTA-017, OTA-020). */
static void trust_default(proto_fwsrv_trust_t *t)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->server, sizeof(t->server), "%s", CONFIG_PANEL_OTA_DEFAULT_DOMAIN);
}

/* At start, on the start-up task: the trust record and the channel. A damaged or missing record
 * is this build's server with no extra key; the old manifest source ("src") is not read. */
static void load_settings(void)
{
    uint8_t blob[PROTO_FWSRV_TRUST_BLOB_MAX];
    size_t len = sizeof(blob);
    bool have = false;
    bool stored = false;
    char channel[sizeof(s_st.channel)] = "";
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "trust", blob, &len) == ESP_OK) {
            stored = true;
            have = proto_fwsrv_trust_decode(blob, len, &s_trust);
        }
        size_t cl = sizeof(channel);
        if (nvs_get_str(h, "chan", channel, &cl) != ESP_OK || !proto_fwsrv_channel_valid(channel)) {
            channel[0] = '\0';
        }
        nvs_close(h);
    }
    char base[PROTO_FWSRV_BASE_MAX];
    if (have && !proto_fwsrv_base_url(s_trust.server, base, sizeof(base))) {
        have = false;
    }
    if (!have) {
        if (stored) {
            ESP_LOGW(TAG, "the stored update server is damaged: using %s",
                     CONFIG_PANEL_OTA_DEFAULT_DOMAIN);
        }
        trust_default(&s_trust);
    }
    snprintf(s_st.channel, sizeof(s_st.channel), "%s", channel[0] != '\0' ? channel : "stable");
    panel_ota_trust_set(s_trust.has_key ? s_trust.key_sha256 : NULL);
    server_to_status_locked(); /* before the task starts: nobody else reads it yet */
}

void panel_ota_set_facts(const char *board_rev, const char *const *peripherals, size_t count)
{
    snprintf(s_board_rev, sizeof(s_board_rev), "%s", board_rev != NULL ? board_rev : "");
    s_parts_n = 0;
    for (size_t i = 0; peripherals != NULL && i < count && s_parts_n < FACT_PARTS_MAX; i++) {
        if (peripherals[i] != NULL && peripherals[i][0] != '\0') {
            snprintf(s_parts[s_parts_n], sizeof(s_parts[0]), "%s", peripherals[i]);
            s_parts_n++;
        }
    }
}

void panel_ota_set_locales(panel_ota_locales_fn fn)
{
    s_locales_fn = fn;
}

void panel_ota_get_server(panel_ota_server_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    lock();
    snprintf(out->domain, sizeof(out->domain), "%s", s_trust.server);
    out->official = s_st.official;
    if (s_trust.has_key) {
        snprintf(out->key_name, sizeof(out->key_name), "%s", s_trust.key_name);
        memcpy(out->key_sha256, s_trust.key_sha256, sizeof(out->key_sha256));
        out->has_key = true;
    } else if (s_own_key_ok) {
        snprintf(out->key_name, sizeof(out->key_name), "%s", s_own_key_name);
        memcpy(out->key_sha256, s_own_key, sizeof(out->key_sha256));
        out->has_key = true;
    }
    unlock();
}

/* ---- the writer ------------------------------------------------------------------------------ */

static bool check_header(writer_t *w, char *why, size_t why_len)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)w->header;
    const esp_app_desc_t *d =
        (const esp_app_desc_t *)(w->header + sizeof(esp_image_header_t) +
                                 sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *me = esp_app_get_description();
    char buf[96];
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        reason(why, why_len, I18N_NOOP_C("update", "Not a firmware image"));
        return false;
    }
    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        reason(why, why_len, I18N_NOOP_C("update", "Firmware for another chip"));
        return false;
    }
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    if (chip.revision < ih->min_chip_rev_full ||
        (ih->max_chip_rev_full != 0u && chip.revision > ih->max_chip_rev_full)) {
        reason(why, why_len, I18N_NOOP_C("update", "Firmware for another chip revision"));
        return false;
    }
    char candidate_project[33], running_project[33];
    panel_ota_version_copy(candidate_project, sizeof(candidate_project), d->project_name, sizeof(d->project_name));
    panel_ota_version_copy(running_project, sizeof(running_project), me->project_name, sizeof(me->project_name));
    if (!ota_profile_accepts(running_project, candidate_project)) {
        char project[33];
        panel_ota_version_copy(project, sizeof(project), d->project_name,
                               sizeof(d->project_name));
        snprintf(buf, sizeof(buf), "Firmware for another device (%s)", project);
        reason(why, why_len, buf);
        return false;
    }
    panel_ota_version_copy(w->version, sizeof(w->version), d->version, sizeof(d->version));
    if (!proto_version_is_upgrade(me->version, w->version, w->allow_older)) { /* OTA-014 */
        snprintf(buf, sizeof(buf), "%s is not newer than %s", w->version, me->version);
        reason(why, why_len, buf);
        return false;
    }
    return true;
}

/* release: the update task's own download (its run is already under way); otherwise a developer's
 * file, which is a run of its own (stage FILE throughout, OTA-026). */
static esp_err_t writer_begin(uint32_t size, const uint8_t *sha256, bool allow_older, bool release)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(part != NULL, ESP_ERR_NOT_FOUND, TAG, "no update slot");
    ESP_RETURN_ON_FALSE(size > HEADER_LEN && size <= part->size, ESP_ERR_INVALID_SIZE, TAG,
                        "size %" PRIu32 " (slot %" PRIu32 ")", size, (uint32_t)part->size);
    lock();
    /* An image waiting for the system to go idle is not busy in the writer's sense, but it is
     * about to be booted from this very slot: cancel it first, never overwrite it underneath
     * try_restart(). Nor a slot the boot slot still points at (a switch back that failed):
     * the next restart starts it whatever happens here. */
    const bool busy = s_busy || s_slot_selected || s_st.probation ||
                      s_st.phase == PANEL_OTA_RESTARTING ||
                      s_st.phase == PANEL_OTA_WAITING ||
                      (!release && (s_st.stage != PANEL_OTA_STAGE_NONE || s_install_pending));
    if (!busy) {
        s_busy = true;
        if (!release) {
            run_begin_locked(PANEL_OTA_KIND_FILE, PANEL_OTA_STAGE_FILE, "", s_st.running);
        }
    }
    unlock();
    ESP_RETURN_ON_FALSE(!busy, ESP_ERR_INVALID_STATE, TAG, "another update is running");

    memset(&s_w, 0, sizeof(s_w));
    s_w.part = part;
    s_w.size = size;
    s_w.allow_older = allow_older;
    if (sha256 != NULL) {
        memcpy(s_w.sha, sha256, sizeof(s_w.sha));
        s_w.has_sha = true;
    }
    s_w.hash = psa_hash_operation_init();
    esp_err_t err = psa_hash_setup(&s_w.hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        /* A ready image in this slot is overwritten now. */
        lock();
        s_ready = NULL;
        s_st.ready[0] = '\0';
        unlock();
        err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_w.handle);
    }
    if (err != ESP_OK) {
        psa_hash_abort(&s_w.hash);
        lock();
        s_busy = false;
        if (!release) {
            run_end_locked();
        }
        changed_locked();
        unlock();
        ESP_LOGE(TAG, "begin: %s", esp_err_to_name(err));
        return err;
    }
    s_w.active = true;
    lock();
    s_st.phase = PANEL_OTA_RECEIVING;
    s_st.percent = 0;
    s_st.error[0] = '\0';
    if (s_st.stage == PANEL_OTA_STAGE_DOWNLOADING) {
        s_st.bytes = 0;
        s_st.size = size;
    }
    changed_locked();
    unlock();
    ESP_LOGI(TAG, "receiving %" PRIu32 " bytes into %s", size, part->label);
    return ESP_OK;
}

esp_err_t panel_ota_begin(uint32_t size, const uint8_t *sha256, bool allow_older)
{
    return writer_begin(size, sha256, allow_older, false);
}

/* A developer's file is a run of its own: it ends when its writer fails (call with the lock held).
 * A release run is ended by the update task, which knows why. */
static void file_run_end_locked(bool failed)
{
    if (s_st.kind == PANEL_OTA_KIND_FILE) {
        run_end_locked();
        s_st.failed_install = failed;
    }
}

static void writer_fail(const char *why)
{
    if (s_w.active) {
        esp_ota_abort(s_w.handle);
        psa_hash_abort(&s_w.hash);
        s_w.active = false;
    }
    lock();
    s_busy = false;
    file_run_end_locked(why != NULL);
    unlock();
    set_phase(why != NULL ? PANEL_OTA_FAILED : PANEL_OTA_IDLE, why);
    if (why != NULL) {
        ESP_LOGW(TAG, "update failed: %s", why);
    }
}

esp_err_t panel_ota_write(const void *data, size_t len, char *why, size_t why_len)
{
    if (!s_w.active) {
        reason(why, why_len, I18N_NOOP_C("update", "No update in progress"));
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint64_t)s_w.written + len > s_w.size) {
        reason(why, why_len, I18N_NOOP_C("update", "More data than announced"));
        writer_fail("More data than announced");
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *p = data;
    psa_hash_update(&s_w.hash, p, len);
    esp_err_t err = ESP_OK;
    if (!s_w.header_ok) {
        const size_t take = len < HEADER_LEN - s_w.written ? len : HEADER_LEN - s_w.written;
        memcpy(s_w.header + s_w.written, p, take);
        s_w.written += (uint32_t)take;
        p += take;
        len -= take;
        if (s_w.written < HEADER_LEN) {
            return ESP_OK;
        }
        char buf[64];
        if (!check_header(&s_w, buf, sizeof(buf))) {
            reason(why, why_len, buf);
            writer_fail(buf);
            return ESP_ERR_INVALID_VERSION;
        }
        s_w.header_ok = true;
        lock();
        if (s_st.target[0] == '\0') { /* a file says what it is only here */
            snprintf(s_st.target, sizeof(s_st.target), "%s", s_w.version);
            changed_locked();
        }
        unlock();
        err = esp_ota_write(s_w.handle, s_w.header, HEADER_LEN);
    }
    if (err == ESP_OK && len > 0u) {
        err = esp_ota_write(s_w.handle, p, len);
        s_w.written += (uint32_t)len;
    }
    if (err != ESP_OK) {
        reason(why, why_len, I18N_NOOP_C("update", "Writing to flash failed"));
        ESP_LOGE(TAG, "write: %s", esp_err_to_name(err));
        writer_fail("Writing to flash failed");
        return err;
    }
    const int percent = (int)((uint64_t)s_w.written * 100u / s_w.size);
    lock();
    if (s_st.stage == PANEL_OTA_STAGE_DOWNLOADING) {
        s_st.bytes = s_w.written; /* seq moves with the whole percent below, not every piece */
    }
    if (percent != s_st.percent) {
        s_st.percent = percent;
        changed_locked();
    }
    unlock();
    return ESP_OK;
}

esp_err_t panel_ota_end(char *version, size_t version_len, char *why, size_t why_len)
{
    if (!s_w.active) {
        reason(why, why_len, I18N_NOOP_C("update", "No update in progress"));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_w.written != s_w.size || !s_w.header_ok) {
        reason(why, why_len, I18N_NOOP_C("update", "The image is incomplete"));
        writer_fail("The image is incomplete");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t sha[32];
    size_t n = 0;
    const bool hashed = psa_hash_finish(&s_w.hash, sha, sizeof(sha), &n) == PSA_SUCCESS;
    if (!hashed || (s_w.has_sha && memcmp(sha, s_w.sha, sizeof(sha)) != 0)) {
        esp_ota_abort(s_w.handle);
        s_w.active = false;
        reason(why, why_len, I18N_NOOP_C("update", "Checksum mismatch: the download is damaged"));
        lock();
        s_busy = false;
        file_run_end_locked(true);
        unlock();
        set_phase(PANEL_OTA_FAILED, "Checksum mismatch: the download is damaged");
        return ESP_ERR_INVALID_CRC;
    }
    /* Checks the whole image, its hash and its signature (OTA-001). */
    const esp_err_t err = esp_ota_end(s_w.handle);
    s_w.active = false;
    if (err != ESP_OK) {
        const char *text = err == ESP_ERR_OTA_VALIDATE_FAILED
                               ? I18N_NOOP_C("update", "Signature or image check failed")
                               : I18N_NOOP_C("update", "Finishing the update failed");
        ESP_LOGE(TAG, "end: %s", esp_err_to_name(err));
        reason(why, why_len, text);
        lock();
        s_busy = false;
        file_run_end_locked(true);
        unlock();
        set_phase(PANEL_OTA_FAILED, text);
        return err;
    }
    lock();
    s_ready = s_w.part;
    s_ready_kind = s_st.kind == PANEL_OTA_KIND_RELEASE ? PANEL_OTA_KIND_RELEASE : PANEL_OTA_KIND_FILE;
    snprintf(s_st.ready, sizeof(s_st.ready), "%s", s_w.version);
    s_busy = false;
    s_st.phase = PANEL_OTA_IDLE;
    s_st.percent = -1;
    if (s_st.kind == PANEL_OTA_KIND_FILE) {
        run_end_locked(); /* verified and kept: a run again once someone applies it */
    }
    changed_locked();
    unlock();
    if (version != NULL && version_len > 0u) {
        snprintf(version, version_len, "%s", s_w.version);
    }
    ESP_LOGI(TAG, "image %s verified in %s", s_w.version, s_w.part->label);
    return ESP_OK;
}

void panel_ota_abort(void)
{
    if (s_w.active) {
        writer_fail(NULL);
    }
}

esp_err_t panel_ota_apply(const alarm_source_t *src, uint8_t user)
{
    lock();
    const bool ready = s_ready != NULL && s_st.phase != PANEL_OTA_RESTARTING && !s_cp_restart;
    if (ready) {
        s_apply_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
        s_apply_user = user;
        s_st.phase = PANEL_OTA_WAITING;
        s_st.error[0] = '\0';
        s_cancelled = false;
        s_restart_hold_ms = 0;
        if (s_ready_kind == PANEL_OTA_KIND_RELEASE) {
            s_st.stage = PANEL_OTA_STAGE_WAITING; /* the release run goes on (OTA-026) */
            s_st.bytes = 0;
            s_st.size = 0;
        } else if (s_st.kind != PANEL_OTA_KIND_FILE) {
            run_begin_locked(PANEL_OTA_KIND_FILE, PANEL_OTA_STAGE_FILE, s_st.ready, s_st.running);
        }
        changed_locked();
    }
    unlock();
    ESP_RETURN_ON_FALSE(ready, ESP_ERR_INVALID_STATE, TAG, "no verified image");
    notify(NB_APPLY);
    return ESP_OK;
}

/* Puts the boot slot back to the image that runs (R17, S07): after the alarm task said no to the
 * restart, or a cancel while the slot is switched. A selected slot left behind would start the
 * new image at the next unplanned reset. If the switch back itself fails, the slot stays
 * selected (false; a stop stays refused, and the "try" record stays so either image reports
 * the attempt): the next reset starts the new image on probation (OTA-005), which confirms it
 * or rolls back to this one. */
static bool release_boot_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_err_t err =
        running != NULL ? esp_ota_set_boot_partition(running) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boot slot not put back to %s (%s): the next restart starts the new image, "
                      "on probation", running != NULL ? running->label : "?", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "boot slot back to %s", running->label);
    lock();
    s_slot_selected = false;
    unlock();
    return true;
}

/* A remote install that was prepared (Feature 30) and then dropped must not leave "armed" in
 * NVS: the next boot of the same image, however it was installed, would wait for a remote
 * confirmation nobody is going to send and roll back. */
static void remote_disarm(void)
{
    nvs_handle_t h;
    if (nvs_open("devota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "armed", 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void panel_ota_cancel(void)
{
    lock();
    bool disarm = false;
    if (s_remote_install && !s_w.active) { s_remote_install = false; s_busy = false; disarm = true; }
    if (s_cp_restart) {
        /* The Wi-Fi module's new image is in and can't be taken back: its restart stays due. */
    } else if (s_st.phase == PANEL_OTA_WAITING) {
        s_st.phase = PANEL_OTA_IDLE;
        if (!s_w.active) s_busy = false;
        s_st.waiting_for[0] = '\0';
        run_end_locked();
        changed_locked();
    } else if (s_st.phase == PANEL_OTA_RESTARTING) {
        s_cancelled = true; /* the update task gives up at its next gate */
    }
    const bool selected = s_slot_selected;
    unlock();
    if (disarm) {
        remote_disarm();
    }
    if (selected) {
        (void)release_boot_slot(); /* it logs a failure; the update task's handoff sees s_cancelled */
    }
}

void panel_ota_cancel_remote(void)
{
    lock();
    const bool remote = s_remote_install;
    unlock();
    if (remote) {
        panel_ota_cancel();
    }
}

void panel_ota_discard(void)
{
    lock();
    const bool can = !s_w.active && s_st.phase != PANEL_OTA_RESTARTING && !s_remote_install &&
                     !s_cp_restart; /* the Wi-Fi module's restart isn't a verified image */
    if (can && (s_ready != NULL || s_st.phase == PANEL_OTA_WAITING)) {
        s_ready = NULL;
        s_st.ready[0] = '\0';
        if (s_st.phase == PANEL_OTA_WAITING) {
            s_st.phase = PANEL_OTA_IDLE;
            s_busy = false;
            s_st.waiting_for[0] = '\0';
        }
        if (s_st.kind == PANEL_OTA_KIND_FILE || s_st.kind == PANEL_OTA_KIND_RELEASE) {
            run_end_locked();
        }
        changed_locked();
    }
    unlock();
}

/* ---- Stop (OTA-027) -------------------------------------------------------------------------- */

esp_err_t panel_ota_stop(const alarm_source_t *src, uint8_t user)
{
    if (s_lock == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_OK;
    lock();
    const panel_ota_stage_t stage = s_st.stage;
    if (stage == PANEL_OTA_STAGE_NONE && !s_install_pending) {
        err = ESP_ERR_NOT_FOUND;
    } else if (s_st.kind == PANEL_OTA_KIND_FILE || s_st.kind == PANEL_OTA_KIND_WIFI_MODULE ||
               stage == PANEL_OTA_STAGE_ROLLING_BACK || s_slot_selected) {
        /* a file, the Wi-Fi module, or past the boot slot's switch: only the self-test decides */
        err = ESP_ERR_NOT_SUPPORTED;
    } else if (stage != PANEL_OTA_STAGE_STOPPING) {
        s_stop_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
        s_stop_user = user;
        s_stop = true;
        s_st.stage = PANEL_OTA_STAGE_STOPPING;
        s_st.reason[0] = '\0';
        changed_locked(); /* also wakes the update task */
    }
    unlock();
    return err;
}

/* Ends a stopped run (the update task, with no writer active): nothing stays ready, Install
 * downloads again, and `last` says who stopped it. Never "failed". */
static void finish_stop(void)
{
    lock();
    const alarm_source_t by = s_stop_src;
    const uint8_t user = s_stop_user;
    char target[32];
    snprintf(target, sizeof(target), "%s", s_st.target);
    s_ready = NULL;
    s_st.ready[0] = '\0';
    s_st.phase = PANEL_OTA_IDLE;
    s_st.percent = -1;
    s_st.waiting_for[0] = '\0';
    s_st.error[0] = '\0';
    s_install_pending = false;
    s_cancelled = false;
    s_restart_hold_ms = 0;
    set_last_locked(PANEL_OTA_LAST_STOPPED, target, s_st.running, now_unix(), "", &by, user);
    run_end_locked();
    s_stop = false;
    changed_locked();
    unlock();
    ESP_LOGW(TAG, "update to %s stopped", target[0] != '\0' ? target : "?");
    alarm_service_firmware_event(ALARM_FW_STOPPED, &by, user);
}

/* An install that failed (OTA-029: the next Install or check clears it). */
static void install_failed(const char *why)
{
    lock();
    char target[32];
    snprintf(target, sizeof(target), "%s", s_st.target);
    s_st.phase = PANEL_OTA_FAILED;
    s_st.percent = -1;
    s_st.waiting_for[0] = '\0';
    snprintf(s_st.error, sizeof(s_st.error), "%s", why != NULL ? why : "");
    set_last_locked(PANEL_OTA_LAST_FAILED, target, s_st.running, now_unix(), s_st.error,
                    &s_install_src, s_install_user);
    run_end_locked();
    s_st.failed_install = true;
    changed_locked();
    unlock();
    ESP_LOGW(TAG, "install failed: %s", why != NULL ? why : "");
}

/* Shows why the restart still hasn't happened, without churning the status. */
static void hold_back(const char *why)
{
    lock();
    if (s_st.phase == PANEL_OTA_WAITING &&
        (strcmp(s_st.waiting_for, why) != 0 || strcmp(s_st.reason, why) != 0)) {
        snprintf(s_st.waiting_for, sizeof(s_st.waiting_for), "%s", why);
        snprintf(s_st.reason, sizeof(s_st.reason), "%s", why);
        changed_locked();
    }
    unlock();
}

/* The house stopped being quiet, or someone stopped the update, while the restart was announced
 * (OTA-028): it is put off - logged, so "installing" never stands alone - and tried again 30 s
 * after everything is quiet. Nothing was switched, so there is nothing to undo. */
static void give_up_restart(const char *why, const alarm_source_t *src, uint8_t user)
{
    lock();
    const bool stopped = s_stop;
    const bool cancelled = s_cancelled || s_remote_install;
    const bool disarm = s_remote_install;
    if (s_remote_install) { s_remote_install = false; s_busy = false; }
    if (s_st.phase == PANEL_OTA_RESTARTING && !stopped) {
        const char *again = I18N_NOOP_C("update", "Trying again shortly");
        s_st.phase = cancelled ? PANEL_OTA_IDLE : PANEL_OTA_WAITING;
        snprintf(s_st.waiting_for, sizeof(s_st.waiting_for), "%s", cancelled ? "" : again);
        if (cancelled) {
            run_end_locked();
        } else {
            if (s_st.kind == PANEL_OTA_KIND_RELEASE) {
                s_st.stage = PANEL_OTA_STAGE_WAITING;
            }
            snprintf(s_st.reason, sizeof(s_st.reason), "%s", again);
        }
        changed_locked();
    }
    s_cancelled = false;
    s_restart_hold_ms = alarm_service_now() + RESTART_HOLD_MS;
    unlock();
    if (disarm) {
        remote_disarm();
    }
    ESP_LOGW(TAG, "restart put off (%s)%s", why,
             stopped ? "" : cancelled ? "" : "; the verified image stays ready");
    alarm_service_firmware_event(ALARM_FW_RESTART_PUT_OFF, src, user);
    if (stopped) {
        finish_stop();
    }
}

/* OTA-028: the 3 s after "installing" was said. The event log is flushed inside it; a stop, a
 * cancel, arming or an alarm ends it early (false, why says which). */
#define RESTART_ANNOUNCE_MS 3000u
static bool restart_hold(const char **why)
{
    const uint64_t start = alarm_service_now();
    panel_log_flush(RESTART_ANNOUNCE_MS - 500u);
    for (;;) {
        if (!restart_allowed(why)) {
            return false;
        }
        if (alarm_service_now() - start >= RESTART_ANNOUNCE_MS) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* Waiting for an idle system (OTA-004), then switch slots and restart.
 *
 * Order matters (R17): the announcement, the log flush and the 3 s hold all run while the old
 * image is still the one that boots, and the state is asked of the alarm task again right after
 * them. Only then is the boot slot switched - esp_ota_set_boot_partition() checks the whole image
 * first, which takes a while - and the last word is the alarm task's own (S07): it checks the
 * engine and its queue and restarts the panel itself, so no input can be handled between that
 * check and the restart; if the house isn't quiet, the slot is switched back here. Someone
 * arming, an alarm starting or a stop during those seconds costs the attempt, not the
 * protection; a power cut before the switch keeps the old firmware, one after it starts the new
 * image on probation (OTA-005). */
static void try_restart(void)
{
    lock();
    /* Not while the Wi-Fi module's run waits (s_cp_restart): it owns WAITING then, and a verified
     * image still staged (an upload not applied, a cancelled release) was never chosen. */
    const bool waiting =
        s_st.phase == PANEL_OTA_WAITING && s_ready != NULL && !s_stop && !s_cp_restart;
    const esp_partition_t *part = s_ready;
    const alarm_source_t src = s_apply_src;
    const uint8_t user = s_apply_user;
    const uint64_t hold = s_restart_hold_ms;
    const bool remote_install = s_remote_install;
    unlock();
    if (!waiting) {
        return;
    }
    const char *why = "";
    if (!restart_allowed(&why)) {
        if (remote_install) { panel_ota_cancel(); return; }
        if (hold != 0u) {
            /* put off once already: 30 s after everything is quiet again (OTA-028) */
            lock();
            s_restart_hold_ms = alarm_service_now() + RESTART_HOLD_MS;
            unlock();
        }
        hold_back(why);
        return;
    }
    if (alarm_service_now() < hold) {
        hold_back(I18N_NOOP_C("update", "Trying again shortly")); /* an attempt was just given up */
        return;
    }
    lock();
    /* The round trip to the alarm task above took time: the image may have been cancelled or
     * replaced meanwhile. Only the same verified image, still waiting, gets restarted. */
    const bool still_waiting =
        s_st.phase == PANEL_OTA_WAITING && s_ready == part && !s_stop && !s_cp_restart;
    if (still_waiting) {
        s_st.phase = PANEL_OTA_RESTARTING;
        s_st.waiting_for[0] = '\0';
        s_st.reason[0] = '\0';
        if (s_st.kind == PANEL_OTA_KIND_RELEASE) {
            s_st.stage = PANEL_OTA_STAGE_INSTALLING;
        }
        changed_locked();
    }
    char from[32];
    snprintf(from, sizeof(from), "%s", s_st.running);
    unlock();
    if (!still_waiting) {
        return;
    }
    alarm_service_firmware_event(ALARM_FW_INSTALLING, &src, user);
    run_listeners(); /* MQTT and Home Assistant see the install before the panel goes */
    bool go = restart_hold(&why) && restart_allowed(&why); /* the last gate */
    if (go) {
        lock();
        go = !s_stop && !s_cancelled;
        if (go) {
            s_slot_selected = true; /* from here on a stop is refused (OTA-027) */
        }
        unlock();
    }
    if (!go) {
        give_up_restart(why, &src, user);
        return;
    }
    /* The attempt, for the next boot of either image (ota_rec_t "try"). A few ms of NVS. */
    esp_app_desc_t d;
    ota_rec_t att;
    const bool described = esp_ota_get_partition_description(part, &d) == ESP_OK;
    char version[32] = "";
    if (described) {
        panel_ota_version_copy(version, sizeof(version), d.version, sizeof(d.version));
    }
    rec_fill(&att, version, from, described ? d.app_elf_sha256 : NULL, &src, user, now_unix());
    if (described) {
        rec_put("try", &att);
    }
    /* The gate once more after the NVS write (which can take tens of ms), before the slower
     * switch: an arming in that window costs nothing to undo. The last word is the alarm
     * task's, after the switch (restart_via_alarm_task). */
    if (!restart_allowed(&why)) {
        lock();
        s_slot_selected = false;
        unlock();
        rec_erase("try");
        give_up_restart(why, &src, user);
        return;
    }
    const esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        lock();
        s_slot_selected = false;
        unlock();
        rec_erase("try");
        ESP_LOGE(TAG, "set boot partition: %s", esp_err_to_name(err));
        lock();
        const panel_ota_kind_t kind = s_st.kind;
        unlock();
        if (kind == PANEL_OTA_KIND_RELEASE) {
            install_failed("The new image can't be started");
        } else {
            lock();
            run_end_locked();
            s_st.failed_install = true;
            unlock();
            set_phase(PANEL_OTA_FAILED, "The new image can't be started");
        }
        return;
    }
    char said[48];
    snprintf(said, sizeof(said), "restarting into %s", part->label);
    restart_via_alarm_task(said, &why);
    /* Still here: the house stopped being quiet, or someone stopped or cancelled the update,
     * after the slot was switched. Back to the image that runs, then put off as above. */
    if (release_boot_slot()) {
        rec_erase("try");
    }
    give_up_restart(why, &src, user);
}

/* ---- the Wi-Fi module's restart (OTA-004, OTA-028) ------------------------------------------- */

void panel_ota_cp_sent(const alarm_source_t *src, uint8_t user)
{
    lock();
    s_cp_restart = true;
    s_cp_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    s_cp_user = user;
    s_restart_hold_ms = 0;
    s_st.phase = PANEL_OTA_WAITING; /* the stage stays WIFI_MODULE */
    s_st.percent = -1;
    s_st.error[0] = '\0';
    changed_locked();
    unlock();
}

/* Someone armed, an alarm started or a walk test began during the announcement: the restart is
 * put off like a release's (logged, tried again 30 s after everything is quiet). Nothing was
 * switched on the panel; the C6 runs its old image until the restart. */
static void cp_put_off(const char *why, const alarm_source_t *src, uint8_t user)
{
    lock();
    const char *again = I18N_NOOP_C("update", "Trying again shortly");
    s_st.phase = PANEL_OTA_WAITING;
    snprintf(s_st.waiting_for, sizeof(s_st.waiting_for), "%s", again);
    snprintf(s_st.reason, sizeof(s_st.reason), "%s", again);
    s_restart_hold_ms = alarm_service_now() + RESTART_HOLD_MS;
    changed_locked();
    unlock();
    ESP_LOGW(TAG, "restart put off (%s); the Wi-Fi module's new image waits for it", why);
    alarm_service_firmware_event(ALARM_FW_RESTART_PUT_OFF, src, user);
}

/* The update task, once a tick: the same gates as try_restart(), without a boot slot. The
 * announcement and its 3 s hold run first, the alarm task is asked again right after them, and
 * it restarts the panel itself if the house is still quiet (S07). Nothing here waits for the
 * house to be quiet: a
 * busy house costs one tick, not the update task. */
static void cp_try_restart(void)
{
    lock();
    const bool pending = s_cp_restart && s_st.phase == PANEL_OTA_WAITING;
    const alarm_source_t src = s_cp_src;
    const uint8_t user = s_cp_user;
    const uint64_t hold = s_restart_hold_ms;
    unlock();
    if (!pending) {
        return;
    }
    const char *why = "";
    if (!restart_allowed(&why)) {
        if (hold != 0u) {
            lock();
            s_restart_hold_ms = alarm_service_now() + RESTART_HOLD_MS;
            unlock();
        }
        hold_back(why);
        return;
    }
    if (alarm_service_now() < hold) {
        hold_back(I18N_NOOP_C("update", "Trying again shortly"));
        return;
    }
    lock();
    s_st.phase = PANEL_OTA_RESTARTING;
    s_st.waiting_for[0] = '\0';
    s_st.reason[0] = '\0';
    changed_locked();
    unlock();
    alarm_service_firmware_event(ALARM_FW_CP_UPDATED, &src, user);
    run_listeners(); /* MQTT and Home Assistant see it before the panel goes */
    if (restart_hold(&why) && restart_allowed(&why)) {
        /* the last gate and the restart on the alarm task (S07); the C6 restarts with the panel */
        restart_via_alarm_task("the Wi-Fi module's image is in: restarting the panel", &why);
    }
    cp_put_off(why, &src, user);
}

/* ---- release server (OTA-002, OTA-003) ------------------------------------------------------- */

/* body (NULL = a GET): POSTed as JSON, again after a redirect. status (may be NULL) gets the
 * server's answer when it isn't 200, 0 when there was none (no connection, DNS, TLS, timeout).
 * stoppable: a release download, which a stop (OTA-027) ends between redirects and after each
 * blocking call (NULL; the caller sees s_stop). */
static esp_http_client_handle_t http_open(const char *url, const char *body, size_t body_len,
                                          int64_t *length, int *status_out, char *why,
                                          size_t why_len, bool stoppable)
{
    if (status_out != NULL) {
        *status_out = 0;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .user_agent = "RoboAlarms-panel",
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach, /* https, also after a redirect */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        return NULL;
    }
    if (body != NULL) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    for (int hop = 0; hop < 6; hop++) { /* release hosts redirect to their storage */
        if (stoppable && s_stop) {
            break;
        }
        esp_err_t err = esp_http_client_open(c, body != NULL ? (int)body_len : 0);
        if (stoppable && s_stop) {
            break;
        }
        size_t sent = 0;
        while (err == ESP_OK && body != NULL && sent < body_len) {
            const int n = esp_http_client_write(c, body + sent, (int)(body_len - sent));
            if (n <= 0) {
                err = ESP_FAIL;
            } else {
                sent += (size_t)n;
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
            reason(why, why_len, I18N_NOOP_C("update", "The update server can't be reached"));
            break;
        }
        *length = esp_http_client_fetch_headers(c);
        if (stoppable && s_stop) {
            break;
        }
        const int status = esp_http_client_get_status_code(c);
        if (status <= 0) {
            reason(why, why_len, I18N_NOOP_C("update", "The update server can't be reached"));
            break;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            /* The redirect's body is never read: ESP-IDF's flush would wait for all of it, however
             * long the server takes. Closing drops it, and the bytes that came with the headers
             * are thrown away so they can't open the next answer. ESP-IDF refuses an https
             * address that redirects to anything but https. */
            const esp_err_t moved = esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            esp_http_client_clear_response_buffer(c);
            if (moved != ESP_OK) {
                ESP_LOGW(TAG, "redirect refused: %s", esp_err_to_name(moved));
                reason(why, why_len, I18N_NOOP_C("update", "The update server can't be reached"));
                break;
            }
            continue;
        }
        if (status == 200) {
            esp_http_client_set_timeout_ms(c, BODY_READ_MS); /* the body: see body_fill() */
            return c;
        }
        if (status_out != NULL) {
            *status_out = status;
        }
        char buf[48];
        I18N_FORMAT(buf, sizeof(buf), UI_WHY_UPDATE_ANSWERED, status);
        reason(why, why_len, buf);
        break;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return NULL;
}

static bool network_up(void)
{
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    return ws.state == NET_WIFI_CONNECTED;
}

typedef enum {
    FETCH_OK = 0,
    FETCH_NO_ANSWER, /* no connection, no such name, a certificate refused, a timeout */
    FETCH_REFUSED,   /* it answered, but not 200 */
    FETCH_UNREADABLE /* the answer broke off, was too long, or there was no memory for it */
} fetch_t;

/* ---- reading an answer's body (OTA-027) ----
 *
 * esp_http_client_read() keeps reading inside one call for as long as bytes keep arriving, each
 * transport read waiting up to the client's timeout: a server trickling a byte every few seconds
 * would hold one call - and a stop - for hours. So the body is read in small pieces with a short
 * timeout (a piece already decrypted in memory comes back at once), a stop is looked at between
 * them, and a body that falls below the floor is given up. The floor, 1 KB/s averaged over each
 * minute, is far under any connection that can carry an update (a 3 MB image takes under an hour
 * even at the floor) and covers "no progress at all for a minute" too. Left: one TLS record (at
 * most 16 KB) that a server sends a byte at a time keeps one read inside mbedTLS until it is whole;
 * the socket's 15 s receive timeout applies to each byte. */

typedef enum {
    BODY_FULL = 0, /* the buffer is full */
    BODY_END,      /* the body ended (whether it was whole is the caller's question) */
    BODY_BROKE,    /* the connection failed */
    BODY_SLOW,     /* below the floor, or past the deadline */
    BODY_STOPPED,  /* a stop was asked (stoppable reads only) */
} body_t;

typedef struct {
    bool stoppable;        /* a release download: s_stop ends it */
    uint64_t deadline;     /* 0 = none */
    uint64_t window_start;
    uint32_t window_bytes;
} body_reader_t;

static void body_reader_init(body_reader_t *b, bool stoppable, uint32_t total_ms)
{
    const uint64_t now = alarm_service_now();
    b->stoppable = stoppable;
    b->deadline = total_ms != 0u ? now + total_ms : 0u;
    b->window_start = now;
    b->window_bytes = 0;
}

/* Reads until buf has len bytes (*n), the body ends, or it has to give up. */
static body_t body_fill(esp_http_client_handle_t c, body_reader_t *b, char *buf, size_t len,
                        size_t *n)
{
    *n = 0;
    while (*n < len) {
        if (b->stoppable && s_stop) {
            return BODY_STOPPED;
        }
        const uint64_t now = alarm_service_now();
        if (b->deadline != 0u && now >= b->deadline) {
            return BODY_SLOW;
        }
        if (now - b->window_start >= BODY_WINDOW_MS) {
            if (b->window_bytes < BODY_FLOOR) {
                return BODY_SLOW;
            }
            b->window_start = now;
            b->window_bytes = 0;
        }
        const size_t room = len - *n;
        const int r = esp_http_client_read(c, buf + *n, (int)(room < BODY_PIECE ? room : BODY_PIECE));
        if (r == -ESP_ERR_HTTP_EAGAIN) {
            continue; /* nothing within BODY_READ_MS */
        }
        if (r < 0) {
            return BODY_BROKE;
        }
        if (r == 0) {
            return BODY_END;
        }
        *n += (size_t)r;
        b->window_bytes += (uint32_t)r;
    }
    return BODY_FULL;
}

/* GET (body NULL) or POST url and read the whole answer into a new NUL-terminated buffer of at
 * most max - 1 bytes (*out; the caller frees it). */
static fetch_t http_fetch(const char *url, const char *body, size_t body_len, size_t max, char **out,
                          size_t *len, char *why, size_t why_len)
{
    *out = NULL;
    *len = 0;
    int64_t length = 0;
    int status = 0;
    esp_http_client_handle_t c = http_open(url, body, body_len, &length, &status, why, why_len, false);
    if (c == NULL) {
        return status > 0 ? FETCH_REFUSED : FETCH_NO_ANSWER;
    }
    char *buf = malloc(max);
    size_t n = 0;
    bool ok = false;
    body_t got = BODY_BROKE;
    if (buf != NULL) {
        body_reader_t rd;
        body_reader_init(&rd, false, SMALL_BODY_MS);
        got = body_fill(c, &rd, buf, max - 1u, &n); /* full: too long, not an answer it reads */
        ok = got == BODY_END && esp_http_client_is_complete_data_received(c);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) {
        reason(why, why_len, buf == NULL       ? I18N_NOOP_C("update", "Out of memory")
                             : got == BODY_SLOW ? I18N_NOOP_C("update", "The connection is too slow")
                                                : I18N_NOOP_C("update", "The release list could not be read"));
        free(buf);
        return FETCH_UNREADABLE;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return FETCH_OK;
}

/* POST <base>/api/v1/check with this panel's facts (OTA-018). The update task only. */
static fetch_t post_check(const char *base, const char *channel, char **json, size_t *len, char *why,
                          size_t why_len)
{
    *json = NULL;
    *len = 0;
    char url[PROTO_FWSRV_BASE_MAX + 16u];
    snprintf(url, sizeof(url), "%s/api/v1/check", base);
    /* The locales whose packs the panel wants (lang_packs registers the question). On the heap:
     * the TLS handshake below needs this task's stack. */
    typedef struct {
        char locales[PANEL_OTA_LANGUAGES_MAX][PANEL_OTA_LOCALE_LEN];
        const char *lp[PANEL_OTA_LANGUAGES_MAX];
        const char *pp[FACT_PARTS_MAX];
    } facts_buf_t;
    facts_buf_t *fb = calloc(1, sizeof(*fb));
    if (fb == NULL) {
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        return FETCH_UNREADABLE;
    }
    char (*locales)[PANEL_OTA_LOCALE_LEN] = fb->locales;
    const char **lp = fb->lp;
    const char **pp = fb->pp;
    const panel_ota_locales_fn fn = s_locales_fn;
    size_t nl = fn != NULL ? fn(locales, PANEL_OTA_LANGUAGES_MAX) : 0u;
    if (nl > PANEL_OTA_LANGUAGES_MAX) {
        nl = PANEL_OTA_LANGUAGES_MAX;
    }
    for (size_t i = 0; i < nl; i++) {
        locales[i][PANEL_OTA_LOCALE_LEN - 1u] = '\0';
        lp[i] = locales[i];
    }
    for (size_t i = 0; i < s_parts_n; i++) {
        pp[i] = s_parts[i];
    }
    const esp_app_desc_t *me = esp_app_get_description();
    char project[sizeof(me->project_name) + 1u];
    char version[sizeof(me->version) + 1u];
    panel_ota_version_copy(project, sizeof(project), me->project_name, sizeof(me->project_name));
    panel_ota_version_copy(version, sizeof(version), me->version, sizeof(me->version));
    const proto_fwsrv_facts_t facts = {
        .channel = channel,
        .device = device_name(),
        .board_rev = s_board_rev,
        .chip_rev = CHIP_REV,
        .project = project,
        .version = version,
        .peripherals = pp,
        .peripheral_count = s_parts_n,
        .languages = lp,
        .language_count = nl,
    };
    char *body = malloc(REQUEST_MAX);
    if (body == NULL) {
        free(fb);
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        return FETCH_UNREADABLE;
    }
    pj_writer_t w;
    pj_init(&w, body, REQUEST_MAX);
    proto_fwsrv_check_request(&w, &facts);
    const size_t n = pj_finish(&w);
    free(fb); /* the request has the words now */
    fetch_t r = FETCH_UNREADABLE;
    if (n == 0u) {
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory")); /* the request didn't fit */
    } else {
        r = http_fetch(url, body, n, ANSWER_MAX, json, len, why, why_len);
    }
    free(body);
    return r;
}

/* A check's answer into the status, the image for Install and the language packs (OTA-016,
 * OTA-018, OTA-019, OTA-024, OTA-025). The update task only. */
static void take_answer(const proto_fwsrv_answer_t *a, const char *json, size_t len, const char *base)
{
    const bool update = a->reason == PROTO_FWSRV_UPDATE;
    if (update) {
        s_art = a->image;
        s_have_art = true;
    } else {
        s_have_art = false;
    }
    lock();
    const char *running = s_st.running;
    if (update) {
        snprintf(s_st.latest, sizeof(s_st.latest), "%s", a->image.version);
        snprintf(s_st.notes_url, sizeof(s_st.notes_url), "%s", a->image.notes_url);
        s_st.available = proto_version_is_upgrade(running, a->image.version, false);
        s_st.critical = a->critical;
    } else {
        /* up to date, or nothing it may install yet: the newest it can have is what runs. A
         * server that doesn't know this panel offers nothing at all. */
        snprintf(s_st.latest, sizeof(s_st.latest), "%s",
                 a->reason == PROTO_FWSRV_UNKNOWN_DEVICE ? "" : running);
        s_st.notes_url[0] = '\0';
        s_st.available = false;
        s_st.critical = false;
    }
    snprintf(s_st.installed_channel, sizeof(s_st.installed_channel), "%s", a->installed_channel);
    s_st.withdrawn = a->installed_withdrawn;
    if (a->has_key && a->key_name[0] != '\0' && s_own_key_ok &&
        memcmp(a->key_sha256, s_own_key, sizeof(s_own_key)) == 0) {
        snprintf(s_own_key_name, sizeof(s_own_key_name), "%s", a->key_name);
    }
    const size_t nl = proto_fwsrv_languages(json, len, base, s_langs, PANEL_OTA_LANGUAGES_MAX);
    s_langs_n = nl == SIZE_MAX ? 0u : nl;
    const size_t na = proto_fwsrv_assets(json, len, base, s_assets, PANEL_OTA_ASSETS_MAX);
    s_assets_n = na == SIZE_MAX ? 0u : na;
    s_langs_seq++;
    changed_locked();
    unlock();
    if (a->reason == PROTO_FWSRV_UNKNOWN_DEVICE) {
        ESP_LOGW(TAG, "the update server has no firmware for %s (%s)", device_name(),
                 CHIP_REV != NULL ? CHIP_REV : "?");
    } else {
        ESP_LOGI(TAG, "release %s (running %s)%s%s%s", update ? a->image.version : "-", running,
                 s_st.available ? ": update available" : "", s_st.critical ? ", important fix" : "",
                 s_st.withdrawn ? "; the running version was withdrawn" : "");
    }
}

/* The release check (OTA-018): asks the server in use with the channel. An answer of any kind is
 * a result (unknown device too: error stays ""); no answer or a damaged one is a failure that
 * OTA-015's schedule retries. */
static bool do_check(char *why, size_t why_len)
{
    char base[PROTO_FWSRV_BASE_MAX];
    char channel[sizeof(s_st.channel)];
    lock();
    const bool have = s_trust.server[0] != '\0' && proto_fwsrv_base_url(s_trust.server, base, sizeof(base));
    snprintf(channel, sizeof(channel), "%s", s_st.channel);
    unlock();
    if (!have) {
        reason(why, why_len, I18N_NOOP_C("update", "No update source set"));
        return false;
    }
    if (!network_up()) {
        reason(why, why_len, I18N_NOOP_C("update", "No network"));
        return false;
    }
    set_phase(PANEL_OTA_CHECKING, NULL);
    char *json = NULL;
    size_t len = 0;
    bool ok = false;
    if (post_check(base, channel, &json, &len, why, why_len) == FETCH_OK) {
        proto_fwsrv_answer_t *a = calloc(1, sizeof(*a));
        if (a == NULL) {
            reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        } else if (proto_fwsrv_check_answer(json, len, base, a) != ALARM_OK) {
            reason(why, why_len, I18N_NOOP_C("update", "The release list is damaged"));
        } else {
            take_answer(a, json, len, base);
            ok = true;
        }
        free(a);
    }
    free(json);
    /* The check is over, whichever way it went: say when (0 while the clock isn't set). */
    const time_t now = time(NULL);
    lock();
    s_st.checked = now > 1600000000 ? (uint32_t)now : 0u;
    s_st.seq++;
    unlock();
    return ok;
}

/* ---- another update server (OTA-018, OTA-020, OTA-021) ---------------------------------------- */

static esp_err_t server_check_start(const char *server, bool dev)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (run_busy_locked()) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t seq = s_srv.seq + 1u;
    memset(&s_srv, 0, sizeof(s_srv));
    s_srv.seq = seq;
    if (server == NULL) {
        s_srv.state = PANEL_OTA_SRV_DONE;
        s_srv.result = PANEL_OTA_SRV_BAD_DOMAIN;
        s_srv_pending = false;
    } else {
        s_srv.state = PANEL_OTA_SRV_CHECKING;
        snprintf(s_srv.domain, sizeof(s_srv.domain), "%s", server);
        s_srv_pending = true;
    }
    changed_locked();
    unlock();
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "checking the update server %s%s", server, dev ? " (development address)" : "");
    notify(NB_SERVER);
    return ESP_OK;
}

esp_err_t panel_ota_server_check(const char *typed)
{
    char domain[PANEL_OTA_SERVER_LEN];
    const bool good = typed != NULL && proto_fwsrv_domain(typed, domain, sizeof(domain));
    return server_check_start(good ? domain : NULL, false);
}

esp_err_t panel_ota_server_check_dev(const char *url)
{
    char base[PANEL_OTA_SERVER_LEN];
    const bool good = url != NULL && strstr(url, "://") != NULL &&
                      proto_fwsrv_base_url(url, base, sizeof(base));
    return server_check_start(good ? base : NULL, true);
}

void panel_ota_server_check_get(panel_ota_srv_check_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_srv;
    unlock();
}

void panel_ota_server_check_forget(void)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    const uint32_t seq = s_srv.seq + 1u;
    memset(&s_srv, 0, sizeof(s_srv));
    s_srv.seq = seq;
    s_srv_pending = false;
    changed_locked();
    unlock();
}

/* The check itself, on the update task: the server's keys, then firmware for this panel. */
static panel_ota_srv_result_t server_probe(const char *server, const char *channel,
                                           const uint8_t (*trusted)[32], size_t n_trusted,
                                           proto_fwsrv_key_t *key, bool *key_trusted)
{
    char base[PROTO_FWSRV_BASE_MAX];
    if (!proto_fwsrv_base_url(server, base, sizeof(base))) {
        return PANEL_OTA_SRV_BAD_DOMAIN;
    }
    if (!network_up()) {
        return PANEL_OTA_SRV_UNREACHABLE;
    }
    char url[PROTO_FWSRV_BASE_MAX + 16u];
    snprintf(url, sizeof(url), "%s/api/v1/keys", base);
    char why[64] = "";
    char *json = NULL;
    size_t len = 0;
    fetch_t f = http_fetch(url, NULL, 0, KEYS_MAX, &json, &len, why, sizeof(why));
    if (f != FETCH_OK) {
        ESP_LOGW(TAG, "%s: keys: %s", server, why);
        return f == FETCH_NO_ANSWER ? PANEL_OTA_SRV_UNREACHABLE : PANEL_OTA_SRV_NOT_A_SERVER;
    }
    proto_fwsrv_key_t *keys = calloc(PROTO_FWSRV_KEYS_MAX, sizeof(*keys));
    const size_t nk = keys != NULL ? proto_fwsrv_keys(json, len, keys, PROTO_FWSRV_KEYS_MAX) : SIZE_MAX;
    free(json);
    if (nk == SIZE_MAX || nk == 0u) {
        free(keys);
        return PANEL_OTA_SRV_NOT_A_SERVER;
    }
    f = post_check(base, channel, &json, &len, why, sizeof(why));
    if (f != FETCH_OK) {
        free(keys);
        ESP_LOGW(TAG, "%s: check: %s", server, why);
        return f == FETCH_NO_ANSWER ? PANEL_OTA_SRV_UNREACHABLE : PANEL_OTA_SRV_NOT_A_SERVER;
    }
    proto_fwsrv_answer_t *a = calloc(1, sizeof(*a));
    panel_ota_srv_result_t r = PANEL_OTA_SRV_NOT_A_SERVER;
    if (a != NULL && proto_fwsrv_check_answer(json, len, base, a) == ALARM_OK) {
        if (a->reason == PROTO_FWSRV_UNKNOWN_DEVICE) {
            r = PANEL_OTA_SRV_NO_FIRMWARE;
        } else {
            proto_fwsrv_key_t offered;
            memset(&offered, 0, sizeof(offered));
            snprintf(offered.name, sizeof(offered.name), "%s", a->key_name);
            memcpy(offered.sha256, a->key_sha256, sizeof(offered.sha256));
            if (proto_fwsrv_key_to_show(&offered, a->has_key, keys, nk, trusted, n_trusted, key)) {
                r = PANEL_OTA_SRV_OK;
                *key_trusted = false;
                for (size_t i = 0; i < n_trusted; i++) {
                    *key_trusted = *key_trusted || memcmp(key->sha256, trusted[i], 32u) == 0;
                }
            }
        }
    }
    free(a);
    free(json);
    free(keys);
    return r;
}

static void server_check_tick(void)
{
    char server[PANEL_OTA_SERVER_LEN];
    char channel[sizeof(s_st.channel)];
    uint8_t trusted[2][32];
    size_t nt = 0;
    lock();
    const bool go = s_srv_pending && s_srv.state == PANEL_OTA_SRV_CHECKING;
    s_srv_pending = false;
    const uint32_t seq = s_srv.seq;
    snprintf(server, sizeof(server), "%s", s_srv.domain);
    snprintf(channel, sizeof(channel), "%s", s_st.channel);
    if (s_own_key_ok) {
        memcpy(trusted[nt++], s_own_key, 32u);
    }
    if (s_trust.has_key) {
        memcpy(trusted[nt++], s_trust.key_sha256, 32u);
    }
    unlock();
    if (!go) {
        return;
    }
    proto_fwsrv_key_t key;
    memset(&key, 0, sizeof(key));
    bool key_trusted = false;
    const panel_ota_srv_result_t r = server_probe(server, channel, trusted, nt, &key, &key_trusted);
    char fp[PROTO_FWSRV_FINGERPRINT_LEN];
    proto_fwsrv_fingerprint(key.sha256, fp);
    ESP_LOGI(TAG, "update server %s: result %d, key %s (%s)%s", server, (int)r, key.name, fp,
             key_trusted ? ", trusted" : "");
    lock();
    if (s_srv.seq == seq && s_srv.state == PANEL_OTA_SRV_CHECKING) { /* not replaced or forgotten */
        s_srv.state = PANEL_OTA_SRV_DONE;
        s_srv.result = r;
        if (r == PANEL_OTA_SRV_OK) {
            snprintf(s_srv.key_name, sizeof(s_srv.key_name), "%s", key.name);
            memcpy(s_srv.key_sha256, key.sha256, sizeof(s_srv.key_sha256));
            s_srv.trusted = key_trusted;
        }
        s_srv.seq++;
        changed_locked();
    }
    unlock();
}

esp_err_t panel_ota_server_commit(const char *domain, const uint8_t key_sha256[32],
                                  bool factory_installer_code, const alarm_source_t *src,
                                  uint8_t user)
{
    if (domain == NULL || key_sha256 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    lock();
    const bool own = s_own_key_ok && memcmp(key_sha256, s_own_key, 32u) == 0;
    const bool trusted = own || (s_trust.has_key && memcmp(key_sha256, s_trust.key_sha256, 32u) == 0);
    if (run_busy_locked() || s_commit_pending) {
        err = ESP_ERR_INVALID_STATE; /* OTA-030 */
    } else if (s_srv.state != PANEL_OTA_SRV_DONE || s_srv.result != PANEL_OTA_SRV_OK ||
               strcmp(s_srv.domain, domain) != 0 ||
               memcmp(s_srv.key_sha256, key_sha256, 32u) != 0) {
        err = ESP_ERR_INVALID_STATE; /* only exactly what was checked */
    } else if (!trusted && factory_installer_code) {
        err = ESP_ERR_NOT_ALLOWED; /* OTA-021 */
    } else {
        memset(&s_commit, 0, sizeof(s_commit));
        snprintf(s_commit.server, sizeof(s_commit.server), "%s", s_srv.domain);
        if (!own) { /* the running image's key is trusted anyway: no key kept for it */
            snprintf(s_commit.key_name, sizeof(s_commit.key_name), "%s", s_srv.key_name);
            memcpy(s_commit.key_sha256, s_srv.key_sha256, 32u);
            s_commit.has_key = true;
        } else if (s_srv.key_name[0] != '\0') {
            snprintf(s_own_key_name, sizeof(s_own_key_name), "%s", s_srv.key_name);
        }
        s_commit_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
        s_commit_user = user;
        s_commit_pending = true;
        const uint32_t seq = s_srv.seq + 1u; /* what was checked is now kept */
        memset(&s_srv, 0, sizeof(s_srv));
        s_srv.seq = seq;
        changed_locked();
    }
    unlock();
    if (err == ESP_OK) {
        notify(NB_SERVER);
    }
    return err;
}

/* The update task: stores a kept server (OTA-020, OTA-023). Waits while the update machinery is
 * held (a language pack's check reads the accepted key), and holds it while storing, so no check
 * or writer starts until the accepted key has changed; an Install waits for this in turn. */
static void commit_tick(void)
{
    lock();
    const bool go = s_commit_pending && !s_busy;
    if (go) {
        s_busy = true;
    }
    const proto_fwsrv_trust_t t = s_commit;
    const alarm_source_t src = s_commit_src;
    const uint8_t user = s_commit_user;
    unlock();
    if (!go) {
        return;
    }
    uint8_t blob[PROTO_FWSRV_TRUST_BLOB_MAX];
    const size_t n = proto_fwsrv_trust_encode(&t, blob, sizeof(blob));
    esp_err_t err = n != 0u ? ESP_OK : ESP_ERR_INVALID_SIZE;
    nvs_handle_t h;
    if (err == ESP_OK) {
        err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "trust", blob, n);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    lock();
    s_commit_pending = false;
    s_busy = false;
    if (err != ESP_OK && (s_st.phase == PANEL_OTA_IDLE || s_st.phase == PANEL_OTA_FAILED)) {
        /* Answered as kept a moment ago: say it wasn't, where a failed check is said. */
        s_st.phase = PANEL_OTA_FAILED;
        s_st.failed_install = false;
        snprintf(s_st.error, sizeof(s_st.error), "%s",
                 I18N_NOOP_C("update", "The new update server could not be saved"));
        changed_locked();
    }
    if (err == ESP_OK) {
        s_trust = t;
        server_to_status_locked();
        panel_ota_trust_set(t.has_key ? t.key_sha256 : NULL);
        s_st.latest[0] = '\0';
        s_st.available = false;
        s_st.notes_url[0] = '\0';
        s_st.critical = false;
        s_st.withdrawn = false;
        s_st.installed_channel[0] = '\0';
        s_check_now = true; /* OTA-023: checks at once */
        changed_locked();
    }
    unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the update server could not be stored (%s)", esp_err_to_name(err));
        return;
    }
    s_have_art = false;
    char fp[PROTO_FWSRV_FINGERPRINT_LEN];
    proto_fwsrv_fingerprint(t.key_sha256, fp);
    ESP_LOGW(TAG, "update server: %s%s%s%s", t.server, t.has_key ? ", accepted key " : "",
             t.has_key ? fp : "", t.has_key ? "" : " (the running image's key)");
    alarm_service_firmware_event(ALARM_FW_SERVER_CHANGED, &src, user);
}

esp_err_t panel_ota_set_channel(const char *channel, const alarm_source_t *src, uint8_t user)
{
    (void)src, (void)user;
    if (channel == NULL || !proto_fwsrv_channel_valid(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const bool busy = run_busy_locked(); /* OTA-030 */
    if (!busy) {
        snprintf(s_st.channel, sizeof(s_st.channel), "%s", channel);
        s_chan_pending = true;
        changed_locked();
    }
    unlock();
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    notify(NB_SERVER);
    return ESP_OK;
}

/* The update task: stores the channel and checks at once (OTA-019). */
static void channel_tick(void)
{
    char channel[sizeof(s_st.channel)];
    lock();
    const bool go = s_chan_pending;
    s_chan_pending = false;
    snprintf(channel, sizeof(channel), "%s", s_st.channel);
    unlock();
    if (!go) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "chan", channel);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the channel could not be stored (%s)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "update channel: %s", channel);
    s_have_art = false;
    s_check_now = true;
}

size_t panel_ota_languages(proto_fw_language_t *out, size_t max)
{
    if (out == NULL || s_lock == NULL) {
        return 0;
    }
    lock();
    const size_t n = s_langs_n < max ? s_langs_n : max;
    memcpy(out, s_langs, n * sizeof(out[0]));
    unlock();
    return n;
}

size_t panel_ota_assets(proto_fw_asset_t *out, size_t max)
{
    if (out == NULL || s_lock == NULL) {
        return 0;
    }
    lock();
    const size_t n = s_assets_n < max ? s_assets_n : max;
    memcpy(out, s_assets, n * sizeof(out[0]));
    unlock();
    return n;
}

uint32_t panel_ota_languages_seq(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    lock();
    const uint32_t q = s_langs_seq;
    unlock();
    return q;
}

uint32_t panel_ota_running_languages_ask(void)
{
    if (s_lock == NULL || s_task == NULL) {
        return 0;
    }
    lock();
    const bool have = s_trust.server[0] != '\0';
    uint32_t ticket = 0;
    if (have) {
        s_rlangs_asked = s_rlangs_asked + 1u != 0u ? s_rlangs_asked + 1u : 1u;
        ticket = s_rlangs_asked;
    }
    unlock();
    if (have) {
        notify(NB_LANGS);
    }
    return ticket;
}

bool panel_ota_running_assets(uint32_t ticket, proto_fw_asset_t *out, size_t max, size_t *n)
{
    if (s_lock == NULL || ticket == 0u || n == NULL) {
        return false;
    }
    lock();
    const bool done = s_rlangs_done != 0u && (int32_t)(s_rlangs_done - ticket) >= 0;
    if (done) {
        if (s_rlangs_n == SIZE_MAX) {
            *n = SIZE_MAX; /* the request failed: no list of either kind */
        } else {
            *n = s_rassets_n < max ? s_rassets_n : max;
            if (out != NULL && *n > 0u) {
                memcpy(out, s_rassets, *n * sizeof(out[0]));
            }
        }
    }
    unlock();
    return done;
}

bool panel_ota_running_languages(uint32_t ticket, proto_fw_language_t *out, size_t max, size_t *n)
{
    if (s_lock == NULL || ticket == 0u || n == NULL) {
        return false;
    }
    lock();
    /* answered when the newest answer is this request's or a later one's (tickets only grow,
     * wrapping past 0 after four billion requests) */
    const bool done = s_rlangs_done != 0u && (int32_t)(s_rlangs_done - ticket) >= 0;
    if (done) {
        if (s_rlangs_n == SIZE_MAX) {
            *n = SIZE_MAX;
        } else {
            *n = s_rlangs_n < max ? s_rlangs_n : max;
            if (out != NULL && *n > 0u) {
                memcpy(out, s_rlangs, *n * sizeof(out[0]));
            }
        }
    }
    unlock();
    return done;
}

/* The update task: GET <base>/api/v1/languages for the version that runs. Same transport
 * as the check: https when the server is, the certificate bundle, bounded reads (OTA-027). */
static void running_langs_tick(void)
{
    char base[PROTO_FWSRV_BASE_MAX];
    lock();
    const uint32_t ticket = s_rlangs_asked;
    /* Not during the self-test or while a restart is due: a slow server could hold the update
     * task for minutes. It stays asked; lang_packs falls back once its own wait runs out. */
    const bool later = s_st.probation || s_cp_restart || s_st.phase == PANEL_OTA_RESTARTING ||
                       (s_st.phase == PANEL_OTA_WAITING && s_ready != NULL);
    const bool pending = ticket != s_rlangs_done && !later;
    const bool have = s_trust.server[0] != '\0' &&
                      proto_fwsrv_base_url(s_trust.server, base, sizeof(base));
    unlock();
    if (!pending) {
        return;
    }
    char why[64] = "";
    size_t n = SIZE_MAX;
    char *json = NULL;
    size_t len = 0;
    const size_t url_cap = PROTO_FWSRV_BASE_MAX + 256u;
    char *url = have ? malloc(url_cap) : NULL;
    if (!have) {
        reason(why, sizeof(why), I18N_NOOP_C("update", "No update source set"));
    } else if (url == NULL) {
        reason(why, sizeof(why), I18N_NOOP_C("update", "Out of memory"));
    } else if (!network_up()) {
        reason(why, sizeof(why), I18N_NOOP_C("update", "No network"));
    } else {
        const esp_app_desc_t *me = esp_app_get_description();
        char project[sizeof(me->project_name) + 1u];
        char version[sizeof(me->version) + 1u];
        panel_ota_version_copy(project, sizeof(project), me->project_name, sizeof(me->project_name));
        panel_ota_version_copy(version, sizeof(version), me->version, sizeof(me->version));
        const proto_fwsrv_facts_t facts = {
            .device = device_name(),
            .chip_rev = CHIP_REV,
            .project = project,
            .version = version,
        };
        if (!proto_fwsrv_languages_url(base, &facts, url, url_cap)) {
            reason(why, sizeof(why), "This panel can't ask for its languages");
        } else if (http_fetch(url, NULL, 0, ANSWER_MAX, &json, &len, why, sizeof(why)) == FETCH_OK) {
            lock(); /* the list, its count and the ticket change together */
            n = proto_fwsrv_languages(json, len, base, s_rlangs, PANEL_OTA_LANGUAGES_MAX);
            const size_t na = proto_fwsrv_assets(json, len, base, s_rassets, PANEL_OTA_ASSETS_MAX);
            s_rassets_n = na == SIZE_MAX ? 0u : na;
            s_rlangs_n = n;
            s_rlangs_done = ticket;
            unlock();
            if (n == SIZE_MAX) {
                reason(why, sizeof(why), I18N_NOOP_C("update", "The release list is damaged"));
            }
        }
    }
    free(json);
    free(url);
    if (n == SIZE_MAX) {
        ESP_LOGW(TAG, "languages for this version: %s", why);
    } else {
        ESP_LOGI(TAG, "languages for this version: %u, asset packs: %u", (unsigned)n,
                 (unsigned)s_rassets_n);
    }
    lock();
    s_rlangs_n = n; /* a failure; after an answer, the same values again */
    s_rlangs_done = ticket; /* a newer request asked meanwhile is answered at the next tick */
    unlock();
}

esp_err_t panel_ota_download(const char *url, uint32_t size, const uint8_t *sha256, uint8_t *out,
                             size_t cap, size_t *got, char *why, size_t why_len)
{
    if (got != NULL) {
        *got = 0;
    }
    if (url == NULL || out == NULL || cap == 0u || size > cap) {
        reason(why, why_len, "The download does not fit");
        return ESP_ERR_INVALID_ARG;
    }
    if (!network_up()) {
        reason(why, why_len, I18N_NOOP_C("update", "No network"));
        return ESP_ERR_INVALID_STATE;
    }
    int64_t length = 0;
    esp_http_client_handle_t c = http_open(url, NULL, 0, &length, NULL, why, why_len, false);
    if (c == NULL) {
        return ESP_FAIL;
    }
    uint32_t want = size;
    if (want == 0u && length > 0 && (uint64_t)length <= cap) {
        want = (uint32_t)length;
    }
    if (want == 0u || (length > 0 && (uint64_t)length != want)) {
        reason(why, why_len, "The download has the wrong size");
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }
    psa_hash_operation_t hash = psa_hash_operation_init();
    const bool hashing = sha256 != NULL;
    if (hashing && psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    uint32_t total = 0;
    esp_err_t err = ESP_OK;
    body_reader_t rd;
    body_reader_init(&rd, false, 0u);
    while (total < want) {
        const uint32_t room = want - total;
        size_t n = 0;
        const body_t b = body_fill(c, &rd, (char *)out + total, room < HTTP_BUF ? room : HTTP_BUF, &n);
        if (hashing && n > 0u) {
            (void)psa_hash_update(&hash, out + total, n);
        }
        total += (uint32_t)n;
        if (b == BODY_SLOW) {
            reason(why, why_len, I18N_NOOP_C("update", "The connection is too slow"));
            err = ESP_FAIL;
            break;
        }
        if (b != BODY_FULL && total < want) {
            reason(why, why_len, "The download broke off");
            err = ESP_FAIL;
            break;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (hashing) {
        if (err != ESP_OK) {
            psa_hash_abort(&hash);
        } else {
            uint8_t sha[32];
            size_t n = 0;
            if (psa_hash_finish(&hash, sha, sizeof(sha), &n) != PSA_SUCCESS || n != sizeof(sha) ||
                memcmp(sha, sha256, sizeof(sha)) != 0) {
                reason(why, why_len, I18N_NOOP_C("update", "Checksum mismatch: the download is damaged"));
                err = ESP_ERR_INVALID_CRC;
            }
        }
    }
    if (err == ESP_OK && got != NULL) {
        *got = total;
    }
    return err;
}

/* A stop was asked (OTA-027): the download reads this between its steps. */
static bool stop_asked(void)
{
    return s_stop;
}

/* The release run's download, verification and hand-over to the wait (OTA-026). Every way out
 * ends the run: waiting (the image is verified), stopped (finish_stop) or failed (install_failed).
 * The stop flag is read between redirects, after each blocking call, between the 4 KB pieces,
 * before a broken read counts as a failure and after the verification. */
static void do_install(void)
{
    char why[64] = "";
    if (stop_asked()) {
        finish_stop();
        return;
    }
    if (!s_have_art && !do_check(why, sizeof(why))) {
        if (stop_asked()) {
            finish_stop();
        } else {
            install_failed(why);
        }
        return;
    }
    const char *running = esp_app_get_description()->version;
    if (!s_have_art || !proto_version_is_upgrade(running, s_art.version, s_install_older)) {
        /* the server offers nothing for this panel, or nothing newer */
        lock();
        run_end_locked();
        unlock();
        set_phase(PANEL_OTA_IDLE, NULL);
        ESP_LOGI(TAG, "%s is installed; nothing to do", running);
        return;
    }
    if (stop_asked()) {
        finish_stop();
        return;
    }
    if (!network_up()) {
        install_failed(I18N_NOOP_C("update", "No network"));
        return;
    }
    lock();
    snprintf(s_st.target, sizeof(s_st.target), "%s", s_art.version);
    s_st.bytes = 0;
    s_st.size = s_art.size;
    unlock();
    set_phase(PANEL_OTA_RECEIVING, NULL); /* stage DOWNLOADING, bytes 0: connecting */
    run_listeners_if_changed();
    ESP_LOGI(TAG, "downloading %s from %s", s_art.version, s_art.url);
    int64_t length = 0;
    esp_http_client_handle_t c = http_open(s_art.url, NULL, 0, &length, NULL, why, sizeof(why), true);
    if (c == NULL) {
        if (stop_asked()) {
            finish_stop();
        } else {
            install_failed(why);
        }
        return;
    }
    uint32_t size = s_art.size;
    if (size == 0u && length > 0) {
        size = (uint32_t)length;
    }
    esp_err_t err = ESP_OK;
    const char *fail = NULL;
    if (size == 0u || (length > 0 && (uint64_t)length != size)) {
        fail = "The download has the wrong size";
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = writer_begin(size, s_art.has_sha256 ? s_art.sha256 : NULL, s_install_older, true);
        if (err != ESP_OK) {
            fail = "Another update is running";
        }
    }
    char *buf = err == ESP_OK ? malloc(HTTP_BUF) : NULL;
    if (err == ESP_OK && buf == NULL) {
        panel_ota_abort();
        fail = I18N_NOOP_C("update", "Out of memory");
        err = ESP_ERR_NO_MEM;
    }
    uint32_t got = 0;
    bool stopped = false;
    body_reader_t rd;
    body_reader_init(&rd, true, 0u);
    while (err == ESP_OK && got < size) {
        if (stop_asked()) {
            stopped = true;
            break;
        }
        const uint32_t room = size - got;
        size_t n = 0;
        const body_t b = body_fill(c, &rd, buf, room < HTTP_BUF ? room : HTTP_BUF, &n);
        if (b == BODY_STOPPED || stop_asked()) { /* also before a stalled read counts as a failure */
            stopped = true;
            break;
        }
        if (n > 0u) {
            err = panel_ota_write(buf, n, why, sizeof(why)); /* sets FAILED itself */
            if (err != ESP_OK) {
                fail = why;
                break;
            }
            got += (uint32_t)n;
            run_listeners_if_changed();
        }
        if (b == BODY_SLOW) {
            panel_ota_abort();
            fail = I18N_NOOP_C("update", "The connection is too slow");
            err = ESP_FAIL;
        } else if (b != BODY_FULL && got < size) {
            panel_ota_abort();
            fail = "The download broke off";
            err = ESP_FAIL;
        }
    }
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (stopped) {
        panel_ota_abort();
        finish_stop();
        return;
    }
    if (err != ESP_OK) {
        install_failed(fail != NULL ? fail : why);
        return;
    }
    lock();
    s_st.stage = PANEL_OTA_STAGE_VERIFYING;
    changed_locked();
    unlock();
    run_listeners_if_changed();
    char version[32];
    if (panel_ota_end(version, sizeof(version), why, sizeof(why)) != ESP_OK) {
        if (stop_asked()) {
            finish_stop(); /* nothing stays ready either way */
        } else {
            install_failed(why);
        }
        return;
    }
    if (stop_asked()) {
        finish_stop(); /* verified, but nothing stays ready: Install downloads again */
        return;
    }
    if (strcmp(version, s_art.version) != 0) {
        ESP_LOGW(TAG, "the release list says %s, the image is %s", s_art.version, version);
    }
    panel_ota_apply(&s_install_src, s_install_user); /* stage WAITING */
}

esp_err_t panel_ota_check(void)
{
    ESP_RETURN_ON_FALSE(s_task != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    notify(NB_CHECK);
    return ESP_OK;
}

/* OTA-030: an update run, its self-test, or anything that leads to one (call with the lock held). */
static bool run_busy_locked(void)
{
    return s_st.stage != PANEL_OTA_STAGE_NONE || s_st.kind != PANEL_OTA_KIND_NONE ||
           s_install_pending || s_st.probation || s_remote_install || s_w.active ||
           s_st.phase == PANEL_OTA_RECEIVING || s_st.phase == PANEL_OTA_WAITING ||
           s_st.phase == PANEL_OTA_RESTARTING;
}

#define INSTALL_WAIT_MS 30000u /* for update machinery held by a language pack's check (OTA-030) */

esp_err_t panel_ota_install(const alarm_source_t *src, uint8_t user, bool allow_older)
{
    ESP_RETURN_ON_FALSE(s_task != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    lock();
    /* OTA-030: one run at a time, and none during the self-test. Only the language packs' brief
     * hold (s_busy with no run) is waited for. Whoever gets here first under the lock wins. */
    const bool busy = run_busy_locked();
    if (!busy) {
        s_install_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
        s_install_user = user;
        s_install_older = allow_older;
        s_install_pending = true;
        s_install_wait_until = alarm_service_now() + INSTALL_WAIT_MS;
        s_stop = false;
        if (s_st.last.result == PANEL_OTA_LAST_STOPPED || s_st.last.result == PANEL_OTA_LAST_FAILED) {
            memset(&s_st.last, 0, sizeof(s_st.last)); /* OTA-029: until the next Install */
        }
        run_begin_locked(PANEL_OTA_KIND_RELEASE, PANEL_OTA_STAGE_DOWNLOADING, "", s_st.running);
        s_st.error[0] = '\0';
        if (s_st.phase == PANEL_OTA_FAILED) {
            s_st.phase = PANEL_OTA_IDLE;
        }
        changed_locked();
    }
    unlock();
    ESP_RETURN_ON_FALSE(!busy, ESP_ERR_INVALID_STATE, TAG, "an update is running");
    notify(NB_INSTALL);
    return ESP_OK;
}

/* The update task: starts an accepted Install once the update machinery is free (OTA-030). */
static void install_tick(uint64_t now)
{
    lock();
    const bool pending = s_install_pending;
    const bool held = s_busy || s_commit_pending;
    if (pending && !held) {
        s_install_pending = false;
    }
    unlock();
    if (!pending) {
        return;
    }
    if (stop_asked()) {
        finish_stop();
        return;
    }
    if (held) {
        if (now >= s_install_wait_until) {
            lock();
            s_install_pending = false;
            unlock();
            install_failed("Another update is running");
        }
        return; /* the next tick, at most a second later */
    }
    do_install();
}

/* A stop that found no download under way (waiting, or between steps) ends on the next tick. */
static void stop_tick(void)
{
    lock();
    const bool finish = s_stop && !s_install_pending && !s_w.active &&
                        s_st.phase != PANEL_OTA_RESTARTING;
    unlock();
    if (finish) {
        finish_stop();
    }
}

/* ---- self-test and rollback (OTA-005) -------------------------------------------------------- */

/* Newest firmware_update record of this stage in the event log (OTA-029), at most `scan` records
 * back: its Unix time (0 = none or no clock then) and the boot it was written in. */
typedef struct {
    uint8_t stage;
} fw_filter_t;

static bool fw_record(void *ctx, const elog_entry_t *e)
{
    const fw_filter_t *f = (const fw_filter_t *)ctx;
    return e->kind == ELOG_KIND_EVENT && e->ev.type == ALARM_EV_FIRMWARE_UPDATE &&
           e->ev.arg == f->stage;
}

static uint32_t log_newest_fw(alarm_fw_stage_t stage, uint32_t *boot)
{
    *boot = 0;
    if (!panel_log_ready()) {
        return 0;
    }
    fw_filter_t f = {.stage = (uint8_t)stage};
    elog_entry_t e;
    if (panel_log_read(UINT32_MAX, fw_record, &f, &e, 1, 2048, NULL) != 1u) {
        return 0;
    }
    *boot = e.boot;
    return e.ev.epoch_ms > 0 ? (uint32_t)(e.ev.epoch_ms / 1000) : 0u;
}

/* At start-up, not on probation: an update that failed its self-test is logged once and kept
 * as `last` (OTA-005, OTA-031), and an update this image confirmed in the last 24 hours is
 * rebuilt as `last` UPDATED (OTA-029).
 *
 * Only an attempt this firmware recorded just before the switch ("try") whose very image is in
 * the failed slot is reported: once the panel is back on the old image, the next download
 * overwrites that slot, so the slot alone would report a version that never ran. */
static void note_rollback(void)
{
    ota_rec_t att, rb;
    const bool have_try = rec_get("try", &att);
    bool have_rb = rec_get("rbv", &rb);
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    const bool bad_desc = bad != NULL && esp_ota_get_partition_description(bad, &d) == ESP_OK;
    if (have_try) {
        if (bad_desc && memcmp(d.app_elf_sha256, att.elf, sizeof(att.elf)) == 0) {
            uint32_t boot = 0;
            const uint32_t at = log_newest_fw(ALARM_FW_ROLLING_BACK, &boot);
            rb = att;
            snprintf(rb.from, sizeof(rb.from), "%s", s_st.running);
            /* its "rolling back" record, from the boot before this one (a crash has none) */
            rb.at = boot != 0u && boot + 1u == panel_log_boot() ? at : 0u;
            rec_put("rbv", &rb);
            have_rb = true;
            ESP_LOGW(TAG, "the update to %s failed its self-test: back on %s", rb.version,
                     s_st.running);
            alarm_service_firmware_event(ALARM_FW_ROLLED_BACK, NULL, 0);
        }
        rec_erase("try"); /* reported, or it never ran (a power cut before the switch) */
    } else if (!have_rb && bad_desc) {
        /* What an older firmware recorded: its "rb" key names the failed image it reported. */
        uint8_t key[8];
        memcpy(key, d.app_elf_sha256, sizeof(key));
        key[0] ^= (uint8_t)bad->address;
        uint8_t seen[8] = {0};
        size_t len = sizeof(seen);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            const bool known = nvs_get_blob(h, "rb", seen, &len) == ESP_OK &&
                               len == sizeof(seen) && memcmp(seen, key, sizeof(key)) == 0;
            nvs_close(h);
            if (known) {
                char version[32];
                panel_ota_version_copy(version, sizeof(version), d.version, sizeof(d.version));
                rec_fill(&rb, version, s_st.running, d.app_elf_sha256, NULL, 0, 0);
                rec_put("rbv", &rb);
                have_rb = true;
            }
        }
    }
    if (have_rb) {
        snprintf(s_st.rolled_back, sizeof(s_st.rolled_back), "%s", rb.version);
        const alarm_source_t by = rec_by(&rb);
        set_last_locked(PANEL_OTA_LAST_ROLLED_BACK, rb.version, rb.from, rb.at, "", &by,
                        rb.by_user);
        return;
    }
    ota_rec_t upd;
    if (rec_get("upd", &upd) &&
        memcmp(upd.elf, esp_app_get_description()->app_elf_sha256, sizeof(upd.elf)) == 0) {
        uint32_t boot = 0;
        const uint32_t at = log_newest_fw(ALARM_FW_UPDATED, &boot);
        if (at != 0u) { /* without its time it can't be kept for 24 hours by the clock */
            const alarm_source_t by = rec_by(&upd);
            set_last_locked(PANEL_OTA_LAST_UPDATED, upd.version, upd.from, at, "", &by,
                            upd.by_user);
        }
    }
}

/* During the self-test: what a failure returns to (OTA-026 `previous`), from the attempt the old
 * image recorded, else from the other slot (an update from a firmware that recorded none). */
static void probation_load(void)
{
    ota_rec_t att;
    if (rec_get("try", &att) &&
        memcmp(att.elf, esp_app_get_description()->app_elf_sha256, sizeof(att.elf)) == 0) {
        snprintf(s_st.previous, sizeof(s_st.previous), "%s", att.from);
        return;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (other != NULL && esp_ota_get_partition_description(other, &d) == ESP_OK) {
        panel_ota_version_copy(s_st.previous, sizeof(s_st.previous), d.version,
                               sizeof(d.version));
    }
}

/* The self-test passed: `last` UPDATED (OTA-029), kept for the next boots of this image. */
static void note_updated(void)
{
    ota_rec_t att, upd;
    const bool have_try = rec_get("try", &att) &&
        memcmp(att.elf, esp_app_get_description()->app_elf_sha256, sizeof(att.elf)) == 0;
    lock();
    char from[32];
    snprintf(from, sizeof(from), "%s", s_st.previous);
    const alarm_source_t by = have_try ? rec_by(&att)
                                       : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    const uint8_t user = have_try ? att.by_user : 0u;
    rec_fill(&upd, s_st.running, from, esp_app_get_description()->app_elf_sha256, &by, user,
             now_unix());
    set_last_locked(PANEL_OTA_LAST_UPDATED, s_st.running, from, upd.at, "", &by, user);
    s_st.previous[0] = '\0';
    s_st.rolled_back[0] = '\0';
    changed_locked();
    unlock();
    rec_put("upd", &upd);
    rec_erase("try");
    rec_erase("rbv"); /* an install replaced the failed version */
}

/* OTA-029: how long `last` is kept, after a check and as time passes (the update task). */
static void last_after_check(bool ok)
{
    lock();
    panel_ota_last_t *l = &s_st.last;
    bool forget_rb = false;
    if (l->result == PANEL_OTA_LAST_STOPPED || l->result == PANEL_OTA_LAST_FAILED) {
        memset(l, 0, sizeof(*l)); /* until the next check */
    } else if (ok && s_st.latest[0] != '\0' && l->result != PANEL_OTA_LAST_NONE &&
               proto_version_is_upgrade(l->version, s_st.latest, false)) {
        forget_rb = l->result == PANEL_OTA_LAST_ROLLED_BACK;
        memset(l, 0, sizeof(*l)); /* a newer version is offered */
    }
    if (ok && s_st.rolled_back[0] != '\0' &&
        proto_version_is_upgrade(s_st.rolled_back, s_st.latest, false)) {
        s_st.rolled_back[0] = '\0';
        forget_rb = true;
    }
    if (!ok) {
        s_st.failed_install = false; /* a check's failure, not an install's */
    }
    changed_locked();
    unlock();
    if (forget_rb) {
        rec_erase("rbv");
    }
}

static void last_tick(uint64_t now)
{
    if (s_st.last.result != PANEL_OTA_LAST_UPDATED) {
        return;
    }
    const uint32_t t = now_unix();
    const bool old = s_st.last.at != 0u ? (t != 0u && t >= s_st.last.at + 86400u)
                                        : now - s_updated_mono >= 86400000u;
    if (old) {
        lock();
        memset(&s_st.last, 0, sizeof(s_st.last));
        changed_locked();
        unlock();
    }
}

/* A restart switches back only while this image is unconfirmed and the other slot holds an
 * application; otherwise it would only restart into the same failing version. */
static bool restart_rolls_back(void)
{
    esp_ota_img_states_t state;
    esp_app_desc_t desc;
    const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY && prev != NULL &&
           esp_ota_get_partition_description(prev, &desc) == ESP_OK;
}

/* OTA-005: the new image is kept only when every required part of the panel has been
 * reporting all through probation - not merely started once (R18). A part that never reports
 * or falls silent fails straight away; one that came up late gets until
 * PANEL_OTA_CONFIRM_MAX_MS to prove it keeps running. */
static void probation_tick(uint64_t now)
{
    static uint32_t shown = 0xFFFFFFFFu;
    if (!s_st.probation || now < s_probation_until_ms) {
        return;
    }
    const uint32_t missing = health_missing(now);
    const uint32_t unproven = health_unproven();
    char what[96];
    char remote_op[33];
    bool remote_wait;
    panel_ota_remote_operation(remote_op, &remote_wait);
    if (missing == 0u && unproven == 0u && !remote_wait) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "self-test passed: %s confirmed (%s)", s_st.running, esp_err_to_name(err));
        if (err != ESP_OK) return;
        lock();
        s_st.probation = false;
        changed_locked();
        unlock();
        alarm_service_firmware_event(ALARM_FW_UPDATED, NULL, 0);
        note_updated();
        nvs_handle_t gate;
        if (nvs_open("devota", NVS_READWRITE, &gate) == ESP_OK) {
            nvs_set_u8(gate, "armed", 0); nvs_commit(gate); nvs_close(gate);
        }
        /* Feature 31: normal firmware retains Installer-controlled remote access.
         * OTA success must never rotate certificates or erase workstation enrollment. */
        return;
    }
    if (missing == 0u && now < s_probation_deadline_ms) {
        if (unproven != shown) {
            shown = unproven;
            health_text(unproven, what, sizeof(what));
            ESP_LOGI(TAG, "self-test: waiting for %s to keep reporting", what);
        }
        return;
    }
    /* Said and logged once: a switch back that fails is tried again once a minute, quietly, so
     * it never fills the event log over the alarm's own history. */
    static bool told;
    static uint64_t retry_ms;
    if (told && now < retry_ms) {
        return;
    }
    if (!told) {
        told = true;
        health_text(missing != 0u ? missing : unproven, what, sizeof(what));
        ESP_LOGE(TAG,
                 "self-test failed: %s not running (health 0x%" PRIx32 ", need 0x%" PRIx32
                 "): rolling back",
                 what, (uint32_t)s_health, s_required);
        /* OTA-031: say why the panel restarts, armed or not, before it does. */
        lock();
        s_st.phase = PANEL_OTA_RESTARTING;
        s_st.stage = PANEL_OTA_STAGE_ROLLING_BACK;
        s_st.kind = PANEL_OTA_KIND_NONE;
        snprintf(s_st.target, sizeof(s_st.target), "%s", s_st.running);
        changed_locked();
        unlock();
        run_listeners();
        alarm_service_firmware_event(ALARM_FW_ROLLING_BACK, NULL, 0);
        panel_log_flush(2000);
    }
    const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot(); /* returns on failure */
    if (err == ESP_ERR_OTA_ROLLBACK_FAILED && restart_rolls_back()) {
        /* After a change of update server the previous version is signed with a key this image
         * no longer accepts, and ESP-IDF's check refuses it. The bootloader checks no signature
         * at boot: an image restarted before it was confirmed is marked ABORTED there, and the
         * previous version starts - the switch back a power cut would make. */
        ESP_LOGE(TAG, "rolling back by a restart: the previous version's key isn't accepted here");
        panel_log_flush(2000);
        esp_restart();
    }
    ESP_LOGE(TAG, "rolling back failed: %s; trying again in a minute", esp_err_to_name(err));
    retry_ms = now + 60000u;
}

/* Kept in the OTA component so probation remains enforced even if dev_remote fails to start. */
static struct { uint32_t schema; uint8_t elf[32]; char op[33]; } s_remote;
static bool s_remote_required;
static bool s_remote_confirmed;

esp_err_t panel_ota_remote_prepare(const char *operation, const uint8_t expected_elf[32])
{
    if (operation == NULL || strlen(operation) != 32u) return ESP_ERR_INVALID_ARG;
    lock();
    esp_app_desc_t desc;
    esp_err_t err = s_ready == NULL || s_busy ? ESP_ERR_INVALID_STATE :
        esp_ota_get_partition_description(s_ready, &desc);
    if (err == ESP_OK && (memcmp(expected_elf, desc.app_elf_sha256, 32) != 0 ||
        strcmp(desc.project_name, esp_app_get_description()->project_name) != 0)) err = ESP_ERR_INVALID_VERSION;
    if (err == ESP_OK) {
        memset(&s_remote, 0, sizeof(s_remote));
        s_remote.schema = 1;
        memcpy(s_remote.elf, expected_elf, 32);
        memcpy(s_remote.op, operation, 33);
        nvs_handle_t h;
        err = nvs_open("devota", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "pending", &s_remote, sizeof(s_remote));
            if (err == ESP_OK) err = nvs_set_u8(h, "armed", 1);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        if (err == ESP_OK) { s_busy = true; s_remote_install = true; }
    }
    unlock();
    return err;
}

void panel_ota_remote_operation(char out[33], bool *required)
{
    lock();
    memcpy(out, s_remote.op, 33);
    *required = s_remote_required && !s_remote_confirmed;
    unlock();
}

bool panel_ota_remote_confirm(const char *operation)
{
    lock();
    const bool ok = s_st.probation && s_remote_required && strcmp(operation, s_remote.op) == 0;
    if (ok) s_remote_confirmed = true;
    unlock();
    return ok;
}

static void remote_probation_load(void)
{
    nvs_handle_t h;
    if (nvs_open("devota", NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof(s_remote);
    const esp_err_t err = nvs_get_blob(h, "pending", &s_remote, &n);
    uint8_t armed = 0;
    nvs_get_u8(h, "armed", &armed);
    if (!s_st.probation) { nvs_set_u8(h, "armed", 0); nvs_commit(h); }
    nvs_close(h);
    if (err != ESP_OK || n != sizeof(s_remote) || s_remote.schema != 1 || s_remote.op[32] != 0) {
        memset(&s_remote, 0, sizeof(s_remote));
        return;
    }
    s_remote_required = armed == 1 && s_st.probation &&
        memcmp(s_remote.elf, esp_app_get_description()->app_elf_sha256, 32) == 0;
}

/* ---- the task -------------------------------------------------------------------------------- */

/* OTA-015: FAILED is the last result, not a state the scheduler has to get out of. A check may
 * start whenever nothing is being written, waiting or restarted - a failure never blocks one,
 * and a verified image waiting for an idle system is never thrown away by one. */
static bool check_can_start(void)
{
    lock();
    const bool ok = !s_busy && !s_install_pending && s_st.stage == PANEL_OTA_STAGE_NONE &&
                    (s_st.phase == PANEL_OTA_IDLE || s_st.phase == PANEL_OTA_FAILED);
    unlock();
    return ok;
}

static uint32_t check_interval_ms(void)
{
    return (uint32_t)CONFIG_PANEL_OTA_CHECK_HOURS * 3600000u;
}

static void ota_task(void *arg)
{
    (void)arg;
    uint64_t next_check = FIRST_CHECK_MS;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
        if ((bits & NB_STATUS) != 0u || s_st.seq != s_listened_seq) {
            run_listeners();
        }
        const uint64_t now = alarm_service_now();
        probation_tick(now);
        health_watch(now);
        last_tick(now);
        stop_tick();
        commit_tick();       /* NB_SERVER wakes these three */
        channel_tick();
        server_check_tick();
        if (s_check_now) {
            s_check_now = false;
            s_check_fails = 0;
            next_check = now;
            if (s_st.server[0] != '\0') {
                bits |= NB_CHECK;
            }
        }
        if (CONFIG_PANEL_OTA_CHECK_HOURS > 0 && now >= next_check) {
            if (s_st.server[0] == '\0') {
                next_check = now + check_interval_ms();
            } else if (!check_can_start() || !network_up()) {
                /* busy, installing, or no network yet: this isn't a failed check */
                next_check = now + PANEL_OTA_RETRY_MIN_MS;
            } else {
                bits |= NB_CHECK;
                next_check = now + check_interval_ms();
            }
        }
        if ((bits & NB_CHECK) != 0u && check_can_start()) {
            char why[64] = "";
            const bool ok = do_check(why, sizeof(why));
            set_phase(ok ? PANEL_OTA_IDLE : PANEL_OTA_FAILED, ok ? NULL : why);
            last_after_check(ok);
            const uint64_t done = alarm_service_now();
            if (ok) {
                s_check_fails = 0;
                next_check = done + check_interval_ms();
            } else {
                if (s_check_fails < UINT16_MAX) {
                    s_check_fails++;
                }
                const uint32_t wait = panel_ota_retry_delay_ms(s_check_fails,
                                                               check_interval_ms());
                next_check = done + wait;
                ESP_LOGW(TAG, "update check failed (%s): looking again in %u min", why,
                         (unsigned)(wait / 60000u));
            }
        }
        install_tick(alarm_service_now()); /* NB_INSTALL wakes it */
        running_langs_tick();                /* NB_LANGS wakes it */
        if ((bits & NB_CP) != 0u) {
            panel_ota_cp_run();
        }
        panel_ota_cp_tick();
        try_restart();
        cp_try_restart();
    }
}

void panel_ota_task_notify_cp(void)
{
    notify(NB_CP);
}

bool panel_ota_claim(void)
{
    lock();
    /* PANEL_OTA_WAITING too: a verified image waiting for an idle system owns the phase, and a
     * co-processor update taking it over would strand that image (R16/R17). */
    const bool free_now = !s_busy && !s_slot_selected && s_st.phase != PANEL_OTA_RESTARTING &&
                          s_st.phase != PANEL_OTA_RECEIVING && s_st.phase != PANEL_OTA_WAITING &&
                          s_st.stage == PANEL_OTA_STAGE_NONE && !s_install_pending;
    if (free_now) {
        s_busy = true;
    }
    unlock();
    return free_now;
}

void panel_ota_release(void)
{
    lock();
    s_busy = false;
    if (s_st.kind == PANEL_OTA_KIND_WIFI_MODULE) { /* the Wi-Fi module's run ended */
        run_end_locked();
        changed_locked();
    }
    unlock();
}

void panel_ota_cp_started(const char *target, const char *previous)
{
    lock();
    run_begin_locked(PANEL_OTA_KIND_WIFI_MODULE, PANEL_OTA_STAGE_WIFI_MODULE, target, previous);
    changed_locked();
    unlock();
}

void panel_ota_set_progress(panel_ota_phase_t phase, int percent, const char *error)
{
    lock();
    s_st.phase = phase;
    s_st.percent = percent;
    snprintf(s_st.error, sizeof(s_st.error), "%s", error != NULL ? error : "");
    changed_locked();
    unlock();
}

void panel_ota_set_cp_image(const char *version)
{
    lock();
    if (strcmp(s_st.cp_image, version) != 0) {
        snprintf(s_st.cp_image, sizeof(s_st.cp_image), "%s", version);
        changed_locked();
    }
    unlock();
}

esp_err_t panel_ota_start(uint32_t required)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "started twice");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "lock");
    s_required = required;
    s_start_ms = alarm_service_now();
    memset(&s_st, 0, sizeof(s_st));
    s_st.percent = -1;
    const esp_app_desc_t *me = esp_app_get_description();
    panel_ota_version_copy(s_st.running, sizeof(s_st.running), me->version, sizeof(me->version));
    load_settings();
    s_own_key_ok = panel_ota_trust_own_key(s_own_key); /* reads flash: on the start-up task */

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_st.probation = true;
        s_probation_until_ms = s_start_ms + PANEL_OTA_CONFIRM_MS;
        s_probation_deadline_ms = s_start_ms + PANEL_OTA_CONFIRM_MAX_MS;
        ESP_LOGW(TAG, "first start of %s: confirming after a %u s self-test (need 0x%" PRIx32 ")",
                 s_st.running, (unsigned)(PANEL_OTA_CONFIRM_MS / 1000u), s_required);
        probation_load();
    } else {
        note_rollback();
    }
    ESP_LOGI(TAG, "firmware %s in %s, updates from %s (%s)%s", s_st.running,
             running != NULL ? running->label : "?", s_st.server[0] != '\0' ? s_st.server : "nowhere",
             s_st.channel, s_trust.has_key ? " with an accepted key" : "");
    remote_probation_load();
    panel_ota_cp_init();
    const BaseType_t ok = xTaskCreate(ota_task, "ota", TASK_STACK, NULL, TASK_PRIORITY, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
