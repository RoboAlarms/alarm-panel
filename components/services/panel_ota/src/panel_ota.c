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
#include <stddef.h>
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
#include "esp_tls_errors.h"
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
#define NB_PROBE   (1u << 7) /* a keypad's PROBE (docs/features/11) */
#define NB_SOURCE  (1u << 8) /* a master's image sources: the running image, a copy */

/* esp_app_format.h's layouts are what proto_fw_header_read() reads byte by byte. */
_Static_assert(HEADER_LEN == PROTO_FW_HEADER_LEN, "image header, segment header, app description");
_Static_assert(offsetof(esp_image_header_t, chip_id) == 12u, "esp_image_header_t.chip_id");
_Static_assert(offsetof(esp_image_header_t, min_chip_rev_full) == 15u, "min_chip_rev_full");
_Static_assert(offsetof(esp_image_header_t, max_chip_rev_full) == 17u, "max_chip_rev_full");
_Static_assert(offsetof(esp_image_header_t, hash_appended) == 23u, "hash_appended");
_Static_assert(offsetof(esp_app_desc_t, version) == 16u, "esp_app_desc_t.version");
_Static_assert(offsetof(esp_app_desc_t, project_name) == 48u, "esp_app_desc_t.project_name");
_Static_assert(CONFIG_IDF_FIRMWARE_CHIP_ID != ESP_CHIP_ID_ESP32P4 ||
                   PROTO_FW_CHIP_ESP32P4 == (unsigned)ESP_CHIP_ID_ESP32P4,
               "PROTO_FW_CHIP_ESP32P4");

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

/* ---- updates through the panel link (docs/features/11): under the lock unless said ---------- */
static volatile bool s_checks_on = true; /* panel_ota_set_checks: the scheduled checks run */
static const char *s_restart_hold_why;   /* panel_ota_set_restart_hold (atomic), NULL = none */
static proto_fw_cap_t s_cap;             /* never past its master (LINK-029) */
static char s_cap_version[32] ALARM_SLOW_BSS;
static char s_master[32] ALARM_SLOW_BSS; /* its name for words, "" = "the alarm panel" */
static panel_ota_keypads_fn s_keypads_fn; /* LINK-041 (atomic) */
static void *s_keypads_ctx;

typedef enum { ORDER_NONE = 0, ORDER_FETCH, ORDER_RELAY } order_op_t;

/* The keypad's ordered run: its master's order, for this run only. In PSRAM: only tasks read it. */
typedef struct {
    order_op_t op;
    char version[32];                  /* V */
    char base[PROTO_FWSRV_BASE_MAX];   /* FETCH: the order's server as an address */
    bool has_base;                     /* base is one: only a FETCH needs it */
    bool own_server;                   /* that is this panel's own server */
    char channel[8];                   /* FETCH: the order's channel */
    bool install;                      /* the master's INSTALL for V has come */
    bool ready;                        /* verified: waiting for INSTALL */
    uint64_t ready_ms;                 /* since when (24 h without INSTALL: discarded) */
    char waiting[64];                  /* "Waiting for <master>" (P2 decision 10) */
} order_t;
static order_t s_order ALARM_SLOW_BSS;
static volatile bool s_order_hold;      /* an ordered run's restart waits for INSTALL (atomic) */
static uint8_t s_why;                   /* how the last ordered run ended (FW_STATE's why) */
static uint8_t s_stop_why;              /* the why a stop ends with: STOPPED, CANCELLED, TIMEOUT */
/* The update task's own: the why of a writer failure, read by the writer's caller after it. */
static uint8_t s_writer_why;
/* LINK-036: an order's server and key taken into s_trust, and its channel into s_st.channel (in
 * RAM at once): the update task stores them. */
static volatile bool s_trust_save;
static volatile bool s_chan_order_save;
/* LINK-030: a master's keypads restart with it (panel_ota_set_restart_with). */
static panel_ota_with_t s_with;   /* set before the first restart */
static volatile bool s_with_set;
static char s_with_version[32];   /* the version the restart under way starts (the update task's) */
static volatile bool s_with_on;   /* its restart waits for its keypads' INSTALLs */
static volatile bool s_with_done; /* ... done: a keypad's hold no longer holds this restart */
/* LINK-034: a keypad on trial with its master after an ordered install ("ord" in NVS). */
static panel_ota_trial_fn s_trial_fn; /* (atomic) */
static void *s_trial_ctx;
static bool s_ord_trial;             /* set at the start, read by the update task */
static char s_ord_version[32];
#define ORD_KEY "ord"                /* ota_rec_t: "ordered install of V", with the attempt */
/* What leads a master's keypads (its target, the relay from its spare slot, their restart with
 * it): its own release. In a development build a developer's file (`ota recv`) leads them too: the
 * bench, whose update server has no development builds (docs/features/11, the bench copy). */
#if CONFIG_ALARM_BUILD_DEV
#define LEADS_KEYPADS(kind) ((kind) == PANEL_OTA_KIND_RELEASE || (kind) == PANEL_OTA_KIND_FILE)
#else
#define LEADS_KEYPADS(kind) ((kind) == PANEL_OTA_KIND_RELEASE)
#endif
/* Its own update in its spare slot (a release, downloaded and checked), for a master's relay:
 * valid while s_ready is still that slot and waits (under the lock). */
static struct {
    bool ok;
    const esp_partition_t *part;
    panel_ota_image_t img;
} s_spare ALARM_SLOW_BSS;

/* PROBE: asked (under the lock), then its answer. */
typedef struct {
    bool pending;
    bool no_server; /* the order named a server too long to be one: answered NO_LOOKUP */
    char version[32];
    char server[PANEL_OTA_SERVER_LEN];
    char channel[8];
} probe_req_t;
static probe_req_t s_probe_req ALARM_SLOW_BSS;
static panel_ota_order_status_t s_ost ALARM_SLOW_BSS; /* the probe's fields, why and seq */

#define ORDER_READY_MAX_MS (24u * 3600u * 1000u) /* a READY image waits this long for INSTALL */

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
    "the alarm engine", "the event log", "the screen", "sensor input", "touch", "the panel link",
    "the keypads",
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

/* A panel without an engine of its own (a link keypad): its gate answers instead. */
static panel_ota_idle_fn s_idle_fn;
static void *s_idle_ctx;

void panel_ota_set_idle_gate(panel_ota_idle_fn fn, void *ctx)
{
    s_idle_ctx = ctx;
    __atomic_store_n(&s_idle_fn, fn, __ATOMIC_RELEASE);
}

bool panel_ota_system_idle(const char **why)
{
    idle_gate_t g = {.allowed = false, .why = ""};
    const panel_ota_idle_fn gate = __atomic_load_n(&s_idle_fn, __ATOMIC_ACQUIRE);
    if (gate != NULL) {
        g.allowed = gate(s_idle_ctx, &g.why);
        if (g.why == NULL) {
            g.why = "";
        }
    } else if (alarm_service_call(gate_on_alarm_task, &g) != ESP_OK) {
        /* The alarm task isn't running (bring-up): the published snapshot is all there is. */
        alarm_service_get_snapshot(&s_gate_snap);
        g.allowed = proto_fw_install_allowed(&s_gate_snap, &g.why);
    }
    if (why != NULL) {
        *why = g.why;
    }
    return g.allowed;
}

/* What holds a restart besides the house (docs/features/11): a master's keypad installing or on
 * its self-test (OTA-032), or a keypad's ordered image waiting for its master's INSTALL. A literal,
 * or the order's own words; NULL when nothing does. Any task, the alarm task's final gate too. */
static const char *restart_held(void)
{
    const char *hold = __atomic_load_n(&s_restart_hold_why, __ATOMIC_ACQUIRE);
    if (!s_with_done) { /* keypads restarting with it never hold it (LINK-030) */
        if (hold != NULL) {
            return hold;
        }
        if (s_with_set && s_with.hold != NULL) {
            hold = s_with.hold(s_with.ctx, s_with_version); /* its keypads still get it */
            if (hold != NULL) {
                return hold;
            }
        }
    }
    return s_order_hold ? s_order.waiting : NULL;
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
    const char *held = restart_held();
    if (held != NULL) {
        *why = held;
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
    const char *held = restart_held();
    if (held != NULL) {
        g->why = held;
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
    panel_ota_cp_prepare_restart();
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
                panel_ota_cp_prepare_restart();
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

/* The keypad's ordered run is over (docs/features/11): call with the lock held. */
static void order_clear_locked(void)
{
    if (s_order.op != ORDER_NONE) {
        memset(&s_order, 0, sizeof(s_order));
        s_ost.seq++;
    }
    s_order_hold = false;
}

/* The run is over, whichever way: call with the lock held. During the self-test `previous`
 * stays: it is what a failure returns to. */
static void run_end_locked(void)
{
    order_clear_locked();
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

/* "rbh": the parts a failed self-test missed (OTA-005; PANEL_OTA_OK_KEYPADS: no keypad answered,
 * docs/features/11 P2 decision 15). Written by the failing image just before it rolls back, read
 * by the image it returns to for as long as its "rbv" stays, erased with it and when a new
 * attempt is recorded. A key of its own, so ota_rec_t keeps its layout. */
#define RBH_KEY    "rbh"
#define RBH_SCHEMA 1u
typedef struct {
    uint8_t schema;
    uint8_t pad[3];
    uint32_t failed; /* panel_ota_health_t bits */
    uint8_t elf[8];  /* the failing image's ELF digest, its first 8 bytes */
} ota_rbh_t;

static void rbh_put(uint32_t failed)
{
    ota_rbh_t r;
    memset(&r, 0, sizeof(r));
    r.schema = RBH_SCHEMA;
    r.failed = failed;
    memcpy(r.elf, esp_app_get_description()->app_elf_sha256, sizeof(r.elf));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, RBH_KEY, &r, sizeof(r)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* The parts the image with this ELF digest missed, 0 when not known. */
static uint32_t rbh_get(const uint8_t elf[32])
{
    ota_rbh_t r;
    size_t len = sizeof(r);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0u;
    }
    const esp_err_t err = nvs_get_blob(h, RBH_KEY, &r, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(r) || r.schema != RBH_SCHEMA ||
        memcmp(r.elf, elf, sizeof(r.elf)) != 0) {
        return 0u;
    }
    return r.failed;
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

/* The required parts' verdicts (ota_selftest.h), as bit masks: missing - not running (never
 * reported, or gone quiet); unproven - reporting, but not long enough yet to count as running.
 * A master's keypads (PANEL_OTA_OK_KEYPADS, LINK-041) come back only after this panel restarted:
 * on probation, until its deadline, their part may wait - no answer yet is unproven, never
 * missing - and answers covering PANEL_OTA_KEYPADS_SPAN_MS prove it. The update task only. */
static uint32_t health_judge(uint64_t now, uint32_t *unproven_out)
{
    const uint32_t now_ms = (uint32_t)now;
    const bool keypads_may_wait = s_st.probation && now < s_probation_deadline_ms;
    uint32_t missing = 0;
    uint32_t unproven = 0;
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
        const uint32_t first = s_health_first[b];
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
        const int32_t covered = (int32_t)(last - first);
        const bool keypads = bit == (uint32_t)PANEL_OTA_OK_KEYPADS;
        const panel_ota_part_t part = {
            .reported = first != 0u,
            .fresh = age <= (int32_t)PANEL_OTA_HEALTH_STALE_MS && !still,
            .covered_ms = first != 0u && covered > 0 ? (uint32_t)covered : 0u,
            .span_ms = keypads ? PANEL_OTA_KEYPADS_SPAN_MS : PANEL_OTA_HEALTH_SPAN_MS,
            .may_wait = keypads && keypads_may_wait,
        };
        switch (panel_ota_part_judge(&part)) {
        case PANEL_OTA_PART_MISSING:
            missing |= bit;
            break;
        case PANEL_OTA_PART_UNPROVEN:
            unproven |= bit;
            break;
        default:
            break;
        }
    }
    if (unproven_out != NULL) {
        *unproven_out = unproven;
    }
    return missing;
}

/* Required parts that are not running: never reported, or have gone quiet. The update task. */
static uint32_t health_missing(uint64_t now)
{
    return health_judge(now, NULL);
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

/* The words for a cap's refusal (LINK-029): English with values, as the other reasons with a
 * value in them. Call with the lock held. */
static void cap_words_locked(char *out, size_t len)
{
    if (s_cap == PROTO_FW_CAP_AT) {
        snprintf(out, len, "%s runs %s: update it first",
                 s_master[0] != '\0' ? s_master : "The alarm panel", s_cap_version);
    } else {
        snprintf(out, len, "Updates come from %s",
                 s_master[0] != '\0' ? s_master : "the alarm panel");
    }
}

/* The header, judged before any flash is written (OTA-014, docs/features/11 LINK-033): magic,
 * chip, chip revision range, project, newer, then the cap. s_writer_why gets the refusal's why. */
static bool check_header(writer_t *w, char *why, size_t why_len)
{
    proto_fw_header_t h;
    (void)proto_fw_header_read(w->header, sizeof(w->header), &h);
    const esp_app_desc_t *me = esp_app_get_description();
    char running_project[33];
    panel_ota_version_copy(running_project, sizeof(running_project), me->project_name,
                           sizeof(me->project_name));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char cap_version[sizeof(s_cap_version)];
    char buf[96];
    lock();
    proto_fw_cap_t cap = s_cap;
    snprintf(cap_version, sizeof(cap_version), "%s", s_cap_version);
    cap_words_locked(buf, sizeof(buf));
    if (s_order.op != ORDER_NONE) {
        /* An ordered run is capped at its order's version, its master's target (LINK-029): the
         * image must be exactly that version anyway (panel_ota_end). */
        cap = PROTO_FW_CAP_AT;
        snprintf(cap_version, sizeof(cap_version), "%s", s_order.version);
        snprintf(buf, sizeof(buf), "Not the version ordered (%s)", s_order.version);
    }
    unlock();
    const proto_fw_self_t self = {
        .chip_id = (uint16_t)CONFIG_IDF_FIRMWARE_CHIP_ID,
        .rev_full = chip.revision,
        .project = running_project,
        .version = s_st.running, /* set once at the start */
        .allow_older = w->allow_older,
        .cap = cap,
        .cap_version = cap_version,
    };
    const proto_fw_hdr_verdict_t v = proto_fw_header_judge(&h, &self);
    s_writer_why = (uint8_t)proto_fw_header_why(v);
    panel_ota_version_copy(w->version, sizeof(w->version), h.version, sizeof(h.version));
    switch (v) {
    case PROTO_FW_HDR_OK:
        return true;
    case PROTO_FW_HDR_NOT_AN_IMAGE:
    default:
        reason(why, why_len, I18N_NOOP_C("update", "Not a firmware image"));
        return false;
    case PROTO_FW_HDR_OTHER_CHIP:
        reason(why, why_len, I18N_NOOP_C("update", "Firmware for another chip"));
        return false;
    case PROTO_FW_HDR_OTHER_REVISION:
        reason(why, why_len, I18N_NOOP_C("update", "Firmware for another chip revision"));
        return false;
    case PROTO_FW_HDR_OTHER_PROJECT:
        snprintf(buf, sizeof(buf), "Firmware for another device (%s)", h.project);
        reason(why, why_len, buf);
        return false;
    case PROTO_FW_HDR_NOT_NEWER:
        snprintf(buf, sizeof(buf), "%s is not newer than %s", w->version, s_st.running);
        reason(why, why_len, buf);
        return false;
    case PROTO_FW_HDR_PAST_CAP:
        reason(why, why_len, buf); /* never past its master (LINK-029) */
        return false;
    }
}

/* kind RELEASE: the update task's own download (its run is already under way); FILE: a
 * developer's file, which is a run of its own (stage FILE throughout, OTA-026); LINK: a keypad's
 * relay from its master (docs/features/11), a run with steps, begun here with its order. */
static esp_err_t writer_begin(uint32_t size, const uint8_t *sha256, bool allow_older,
                              panel_ota_kind_t kind, const order_t *order)
{
    const bool release = kind == PANEL_OTA_KIND_RELEASE;
    s_writer_why = PROTO_FW_WHY_OK;
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        s_writer_why = PROTO_FW_WHY_FLASH;
    } else if (size <= HEADER_LEN || size > part->size) {
        s_writer_why = size > part->size ? PROTO_FW_WHY_TOO_BIG : PROTO_FW_WHY_BAD_SIZE;
    }
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
        if (kind == PANEL_OTA_KIND_FILE) {
            run_begin_locked(PANEL_OTA_KIND_FILE, PANEL_OTA_STAGE_FILE, "", s_st.running);
        } else if (kind == PANEL_OTA_KIND_LINK && order != NULL) {
            run_begin_locked(PANEL_OTA_KIND_LINK, PANEL_OTA_STAGE_DOWNLOADING, order->version,
                             s_st.running);
            if (s_st.last.result == PANEL_OTA_LAST_STOPPED ||
                s_st.last.result == PANEL_OTA_LAST_FAILED) {
                memset(&s_st.last, 0, sizeof(s_st.last)); /* OTA-029: until the next run */
            }
            s_order = *order;
            s_order_hold = !s_order.install;
            s_why = PROTO_FW_WHY_OK;
            s_stop = false;
            s_ost.seq++;
            changed_locked();
        }
    }
    unlock();
    if (busy) {
        s_writer_why = PROTO_FW_WHY_BUSY;
    }
    ESP_RETURN_ON_FALSE(!busy, ESP_ERR_INVALID_STATE, TAG, "another update is running");
    if (!release) {
        panel_ota_source_own_run(); /* a copy for a keypad is of the version that runs now */
    }

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
        s_writer_why = PROTO_FW_WHY_FLASH;
        lock();
        s_busy = false;
        if (kind == PANEL_OTA_KIND_LINK) {
            s_why = PROTO_FW_WHY_FLASH;
            s_ost.seq++;
        }
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
    return writer_begin(size, sha256, allow_older, PANEL_OTA_KIND_FILE, NULL);
}

/* The writer belongs to whichever task feeds it; s_wlock lets the update task close a keypad's
 * relay on a stop (OTA-027) without racing a write. Order: s_wlock, then s_lock. */
static SemaphoreHandle_t s_wlock;

static bool wlock(void)
{
    if (s_wlock == NULL) {
        return false;
    }
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    return true;
}

static void wunlock(void)
{
    xSemaphoreGive(s_wlock);
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

/* A keypad's relay ends with its writer (call with the lock held): failed with why's words and
 * code, or (text NULL) given up by its owner, which says why itself. */
static void link_run_end_locked(const char *text, uint8_t code)
{
    if (s_st.kind != PANEL_OTA_KIND_LINK) {
        return;
    }
    if (text != NULL) {
        const alarm_source_t sys = {.kind = ALARM_SRC_SYSTEM, .id = 0};
        set_last_locked(PANEL_OTA_LAST_FAILED, s_st.target, s_st.running, now_unix(), text, &sys,
                        0u);
        s_why = code != PROTO_FW_WHY_OK ? code : PROTO_FW_WHY_FLASH;
        s_ost.seq++;
    }
    run_end_locked();
    s_st.failed_install = text != NULL;
}

static void writer_fail(const char *why, uint8_t code)
{
    if (s_w.active) {
        esp_ota_abort(s_w.handle);
        psa_hash_abort(&s_w.hash);
        s_w.active = false;
    }
    s_writer_why = code;
    lock();
    s_busy = false;
    file_run_end_locked(why != NULL);
    link_run_end_locked(why, code);
    unlock();
    set_phase(why != NULL ? PANEL_OTA_FAILED : PANEL_OTA_IDLE, why);
    if (why != NULL) {
        ESP_LOGW(TAG, "update failed: %s", why);
    }
}

static esp_err_t writer_write(const void *data, size_t len, char *why, size_t why_len)
{
    if (!s_w.active) {
        reason(why, why_len, I18N_NOOP_C("update", "No update in progress"));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stop && s_st.kind == PANEL_OTA_KIND_LINK) {
        reason(why, why_len, "Stopped"); /* the update task closes the writer (stop_tick) */
        return ESP_ERR_INVALID_STATE;
    }
    if ((uint64_t)s_w.written + len > s_w.size) {
        reason(why, why_len, I18N_NOOP_C("update", "More data than announced"));
        writer_fail("More data than announced", PROTO_FW_WHY_BAD_SIZE);
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
            writer_fail(buf, s_writer_why);
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
        writer_fail("Writing to flash failed", PROTO_FW_WHY_FLASH);
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

esp_err_t panel_ota_write(const void *data, size_t len, char *why, size_t why_len)
{
    if (!wlock()) {
        reason(why, why_len, I18N_NOOP_C("update", "No update in progress"));
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = writer_write(data, len, why, why_len);
    wunlock();
    return err;
}

/* A failure in panel_ota_end, after the writer is closed (it is not active any more). */
static void end_failed(const char *text, uint8_t code)
{
    s_writer_why = code;
    lock();
    s_busy = false;
    file_run_end_locked(true);
    link_run_end_locked(text, code);
    unlock();
    set_phase(PANEL_OTA_FAILED, text);
}

static esp_err_t writer_end(char *version, size_t version_len, char *why, size_t why_len)
{
    if (!s_w.active) {
        reason(why, why_len, I18N_NOOP_C("update", "No update in progress"));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_w.written != s_w.size || !s_w.header_ok) {
        reason(why, why_len, I18N_NOOP_C("update", "The image is incomplete"));
        writer_fail("The image is incomplete", PROTO_FW_WHY_BAD_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    lock();
    const bool link = s_st.kind == PANEL_OTA_KIND_LINK;
    if (link) { /* a relay is verified here, by its owner (docs/features/11) */
        s_st.stage = PANEL_OTA_STAGE_VERIFYING;
        changed_locked();
    }
    unlock();
    uint8_t sha[32];
    size_t n = 0;
    const bool hashed = psa_hash_finish(&s_w.hash, sha, sizeof(sha), &n) == PSA_SUCCESS;
    if (!hashed || (s_w.has_sha && memcmp(sha, s_w.sha, sizeof(sha)) != 0)) {
        esp_ota_abort(s_w.handle);
        s_w.active = false;
        reason(why, why_len, I18N_NOOP_C("update", "Checksum mismatch: the download is damaged"));
        end_failed("Checksum mismatch: the download is damaged", PROTO_FW_WHY_BAD_SHA);
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
        end_failed(text, err == ESP_ERR_OTA_VALIDATE_FAILED ? PROTO_FW_WHY_SIGNATURE
                                                            : PROTO_FW_WHY_FLASH);
        return err;
    }
    /* An ordered image must be the version its master named (docs/features/11 LINK-033, check 5):
     * any other is discarded, signed or not. */
    char wrong[64] = "";
    lock();
    if (s_order.op != ORDER_NONE && strcmp(s_w.version, s_order.version) != 0) {
        snprintf(wrong, sizeof(wrong), "The image is %.20s, not %.20s", s_w.version,
                 s_order.version);
    }
    unlock();
    if (wrong[0] != '\0') {
        reason(why, why_len, wrong);
        ESP_LOGW(TAG, "%s: discarded", wrong);
        end_failed(wrong, PROTO_FW_WHY_WRONG_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    proto_fw_header_t hdr;
    const bool have_hdr = proto_fw_header_read(s_w.header, sizeof(s_w.header), &hdr);
    lock();
    s_ready = s_w.part;
    s_ready_kind = s_st.kind == PANEL_OTA_KIND_RELEASE || s_st.kind == PANEL_OTA_KIND_LINK
                       ? s_st.kind
                       : PANEL_OTA_KIND_FILE;
    /* A release of its own: its keypads' target, relayed from this slot (docs/features/11) */
    s_spare.ok = have_hdr && LEADS_KEYPADS(s_ready_kind) && s_order.op == ORDER_NONE;
    if (s_spare.ok) {
        memset(&s_spare.img, 0, sizeof(s_spare.img));
        s_spare.part = s_w.part;
        s_spare.img.size = s_w.size;
        memcpy(s_spare.img.sha256, sha, sizeof(s_spare.img.sha256));
        snprintf(s_spare.img.version, sizeof(s_spare.img.version), "%s", s_w.version);
        panel_ota_version_copy(s_spare.img.project, sizeof(s_spare.img.project), hdr.project,
                               sizeof(hdr.project));
        s_spare.img.chip_id = hdr.chip_id;
        s_spare.img.min_rev_full = hdr.min_rev_full;
        s_spare.img.max_rev_full = hdr.max_rev_full;
    }
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
    if (link) {
        /* READY: the restart waits for the master's INSTALL (restart_held) and the gate. */
        const alarm_source_t sys = {.kind = ALARM_SRC_SYSTEM, .id = 0};
        (void)panel_ota_apply(&sys, 0u);
    }
    return ESP_OK;
}

esp_err_t panel_ota_end(char *version, size_t version_len, char *why, size_t why_len)
{
    if (!wlock()) {
        reason(why, why_len, I18N_NOOP_C("update", "No update in progress"));
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = writer_end(version, version_len, why, why_len);
    wunlock();
    return err;
}

void panel_ota_abort(void)
{
    if (!wlock()) {
        return;
    }
    if (s_w.active) {
        writer_fail(NULL, PROTO_FW_WHY_OK);
    }
    wunlock();
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
        if (s_ready_kind == PANEL_OTA_KIND_RELEASE || s_ready_kind == PANEL_OTA_KIND_LINK) {
            s_st.stage = PANEL_OTA_STAGE_WAITING; /* the release run goes on (OTA-026) */
            s_st.bytes = 0;
            s_st.size = 0;
            if (s_order.op != ORDER_NONE && !s_order.ready) {
                s_order.ready = true; /* READY: waits for INSTALL, 24 h at most */
                s_order.ready_ms = alarm_service_now();
                s_ost.seq++;
            }
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
        if (s_st.kind == PANEL_OTA_KIND_FILE || s_st.kind == PANEL_OTA_KIND_RELEASE ||
            s_st.kind == PANEL_OTA_KIND_LINK) {
            run_end_locked();
        }
        changed_locked();
    }
    unlock();
}

/* ---- Stop (OTA-027) -------------------------------------------------------------------------- */

/* A stop (OTA-027) with the why an ordered run ends with: STOPPED for a person, CANCELLED for the
 * keypad's master, TIMEOUT for a READY image no INSTALL came for. Call with the lock held. */
static esp_err_t stop_locked(const alarm_source_t *src, uint8_t user, uint8_t why)
{
    const panel_ota_stage_t stage = s_st.stage;
    if (stage == PANEL_OTA_STAGE_NONE && !s_install_pending) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_st.kind == PANEL_OTA_KIND_FILE || s_st.kind == PANEL_OTA_KIND_WIFI_MODULE ||
        stage == PANEL_OTA_STAGE_ROLLING_BACK || s_slot_selected) {
        /* a file, the Wi-Fi module, or past the boot slot's switch: only the self-test decides */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (stage != PANEL_OTA_STAGE_STOPPING) {
        s_stop_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
        s_stop_user = user;
        s_stop_why = why;
        s_stop = true;
        s_st.stage = PANEL_OTA_STAGE_STOPPING;
        s_st.reason[0] = '\0';
        changed_locked(); /* also wakes the update task */
    }
    return ESP_OK;
}

esp_err_t panel_ota_stop(const alarm_source_t *src, uint8_t user)
{
    if (s_lock == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    const esp_err_t err = stop_locked(src, user, PROTO_FW_WHY_STOPPED);
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
    if (s_order.op != ORDER_NONE) { /* a keypad's ordered run: its why (docs/features/11) */
        s_why = s_stop_why != PROTO_FW_WHY_OK ? s_stop_why : PROTO_FW_WHY_STOPPED;
        s_ost.seq++;
    }
    s_stop_why = PROTO_FW_WHY_OK;
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

/* An install that failed (OTA-029: the next Install or check clears it). code: its why number,
 * which a keypad's ordered run reports (docs/features/11). */
static void install_failed(const char *why, uint8_t code)
{
    lock();
    if (s_order.op != ORDER_NONE) {
        s_why = code != PROTO_FW_WHY_OK ? code : PROTO_FW_WHY_FLASH;
        s_ost.seq++;
    }
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
    /* waiting_for is shorter than an order's "Waiting for <master>" can be: compared as it is
     * kept (cut at a whole character), or a long name would count as a change on every try. */
    char shown[sizeof(s_st.waiting_for)];
    (void)i18n_utf8_copy(shown, sizeof(shown), why);
    lock();
    if (s_st.phase == PANEL_OTA_WAITING &&
        (strcmp(s_st.waiting_for, shown) != 0 || strcmp(s_st.reason, why) != 0)) {
        snprintf(s_st.waiting_for, sizeof(s_st.waiting_for), "%s", shown);
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
            if (s_st.kind == PANEL_OTA_KIND_RELEASE || s_st.kind == PANEL_OTA_KIND_LINK) {
                s_st.stage = PANEL_OTA_STAGE_WAITING;
            }
            snprintf(s_st.reason, sizeof(s_st.reason), "%s", again);
        }
        changed_locked();
    }
    s_cancelled = false;
    s_restart_hold_ms = alarm_service_now() + RESTART_HOLD_MS;
    s_with_on = false; /* its keypads restart with its next attempt (LINK-030) */
    s_with_done = false;
    s_with_version[0] = '\0'; /* (a Wi-Fi module's restart isn't theirs to hold) */
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

/* LINK-030 ("Restarting together"): once this panel's own restart has passed its gate, its keypads
 * get their INSTALL (panel_ota_restart_soon() for the link's planner) - waited for at most
 * PANEL_OTA_WITH_MS - and their "installing" goes into the log before the restart. From here a
 * keypad's hold no longer holds this restart. false: no one asks (nothing waited). The update
 * task. */
static bool restart_with(void)
{
    if (!s_with_set || s_with.installs_out == NULL) {
        return false;
    }
    s_with_done = false;
    s_with_on = true;
    const uint64_t until = alarm_service_now() + PANEL_OTA_WITH_MS;
    while (!s_with.installs_out(s_with.ctx) && alarm_service_now() < until && !s_stop &&
           !s_cancelled) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_with_done = true;
    panel_log_flush(500u); /* the keypads' "installing" with this panel's */
    return true;
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
    /* its own release's version, for its keypads' hold ("": another restart, which they don't
     * hold) */
    snprintf(s_with_version, sizeof(s_with_version), "%s",
             LEADS_KEYPADS(s_ready_kind) ? s_st.ready : "");
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
        if (s_st.kind == PANEL_OTA_KIND_RELEASE || s_st.kind == PANEL_OTA_KIND_LINK) {
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
    if (go && restart_with()) {
        go = restart_allowed(&why); /* (the keypads' hold no longer counts) */
    }
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
        rec_erase(RBH_KEY); /* what an earlier attempt's self-test missed is not this one's */
        rec_put("try", &att);
        lock();
        const bool ordered = s_order.op != ORDER_NONE;
        unlock();
        if (ordered) {
            rec_put(ORD_KEY, &att); /* LINK-034: on trial with its master after this restart */
        }
    }
    /* The gate once more after the NVS write (which can take tens of ms), before the slower
     * switch: an arming in that window costs nothing to undo. The last word is the alarm
     * task's, after the switch (restart_via_alarm_task). */
    if (!restart_allowed(&why)) {
        lock();
        s_slot_selected = false;
        unlock();
        rec_erase("try");
        rec_erase(ORD_KEY);
        give_up_restart(why, &src, user);
        return;
    }
    const esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        lock();
        s_slot_selected = false;
        unlock();
        rec_erase("try");
        rec_erase(ORD_KEY);
        ESP_LOGE(TAG, "set boot partition: %s", esp_err_to_name(err));
        lock();
        const panel_ota_kind_t kind = s_st.kind;
        unlock();
        if (kind == PANEL_OTA_KIND_RELEASE || kind == PANEL_OTA_KIND_LINK) {
            install_failed("The new image can't be started", PROTO_FW_WHY_FLASH);
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
        rec_erase(ORD_KEY);
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
 * stop (NULL = none): a flag that ends it between redirects and after each blocking call (NULL;
 * the caller sees its flag): a release download's s_stop (OTA-027), a keypad's copy's. net (may
 * be NULL) gets why it failed in the panel link's numbers (docs/features/11 the why table):
 * NO_LOOKUP, NO_CONNECT, SERVER_ERROR or NO_MEMORY. */
static esp_http_client_handle_t http_open(const char *url, const char *body, size_t body_len,
                                          int64_t *length, int *status_out, char *why,
                                          size_t why_len, const volatile bool *stop, uint8_t *net)
{
    if (status_out != NULL) {
        *status_out = 0;
    }
    uint8_t fail = PROTO_FWSRV_WHY_SERVER_ERROR; /* too many redirects, or a stop */
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
        if (net != NULL) {
            *net = PROTO_FW_WHY_NO_MEMORY;
        }
        return NULL;
    }
    if (body != NULL) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    for (int hop = 0; hop < 6; hop++) { /* release hosts redirect to their storage */
        if (stop != NULL && *stop) {
            break;
        }
        esp_err_t err = esp_http_client_open(c, body != NULL ? (int)body_len : 0);
        if (stop != NULL && *stop) {
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
            int tls_code = 0;
            int tls_flags = 0;
            fail = esp_http_client_get_and_clear_last_tls_error(c, &tls_code, &tls_flags) ==
                           ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
                       ? PROTO_FWSRV_WHY_NO_LOOKUP
                       : PROTO_FWSRV_WHY_NO_CONNECT;
            break;
        }
        *length = esp_http_client_fetch_headers(c);
        if (stop != NULL && *stop) {
            break;
        }
        const int status = esp_http_client_get_status_code(c);
        if (status <= 0) {
            reason(why, why_len, I18N_NOOP_C("update", "The update server can't be reached"));
            fail = PROTO_FWSRV_WHY_NO_CONNECT;
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
    if (net != NULL) {
        *net = fail;
    }
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
    BODY_STOPPED,  /* a stop was asked (readers with a stop flag only) */
} body_t;

typedef struct {
    const volatile bool *stop; /* NULL, or a flag that ends it: s_stop for a release download */
    uint64_t deadline;     /* 0 = none */
    uint64_t window_start;
    uint32_t window_bytes;
} body_reader_t;

static void body_reader_init(body_reader_t *b, const volatile bool *stop, uint32_t total_ms)
{
    const uint64_t now = alarm_service_now();
    b->stop = stop;
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
        if (b->stop != NULL && *b->stop) {
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
 * most max - 1 bytes (*out; the caller frees it). net (may be NULL): why it failed, in the panel
 * link's numbers (http_open's, or NO_MEMORY, TOO_SLOW, BROKE_OFF, NOT_A_SERVER for too long). */
static fetch_t http_fetch(const char *url, const char *body, size_t body_len, size_t max, char **out,
                          size_t *len, char *why, size_t why_len, uint8_t *net)
{
    *out = NULL;
    *len = 0;
    int64_t length = 0;
    int status = 0;
    esp_http_client_handle_t c =
        http_open(url, body, body_len, &length, &status, why, why_len, NULL, net);
    if (c == NULL) {
        return status > 0 ? FETCH_REFUSED : FETCH_NO_ANSWER;
    }
    char *buf = malloc(max);
    size_t n = 0;
    bool ok = false;
    body_t got = BODY_BROKE;
    if (buf != NULL) {
        body_reader_t rd;
        body_reader_init(&rd, NULL, SMALL_BODY_MS);
        got = body_fill(c, &rd, buf, max - 1u, &n); /* full: too long, not an answer it reads */
        ok = got == BODY_END && esp_http_client_is_complete_data_received(c);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) {
        reason(why, why_len, buf == NULL       ? I18N_NOOP_C("update", "Out of memory")
                             : got == BODY_SLOW ? I18N_NOOP_C("update", "The connection is too slow")
                                                : I18N_NOOP_C("update", "The release list could not be read"));
        if (net != NULL) {
            *net = buf == NULL           ? PROTO_FW_WHY_NO_MEMORY
                   : got == BODY_SLOW    ? PROTO_FW_WHY_TOO_SLOW
                   : got == BODY_FULL    ? PROTO_FWSRV_WHY_NOT_A_SERVER /* longer than an answer */
                                         : PROTO_FW_WHY_BROKE_OFF;
        }
        free(buf);
        return FETCH_UNREADABLE;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return FETCH_OK;
}

/* POST <base>/api/v1/check (OTA-018) at the server `base` with `channel`, both for this check
 * only: with this panel's facts, or (other) a keypad's facts for its copy (docs/features/11: its
 * device, board_rev, chip_rev, project and version; its parts and languages aren't sent). want
 * (NULL or "": none) names the one release a keypad was told to install (FWS-028). net (may be
 * NULL): why it failed (http_fetch's numbers). Nothing here becomes trust. The update task only. */
static fetch_t post_check(const char *base, const char *channel, const proto_fwsrv_facts_t *other,
                          const char *want, char **json, size_t *len, char *why, size_t why_len,
                          uint8_t *net)
{
    *json = NULL;
    *len = 0;
    if (net != NULL) {
        *net = PROTO_FW_WHY_NO_MEMORY;
    }
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
    const panel_ota_locales_fn fn = other == NULL ? s_locales_fn : NULL;
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
    proto_fwsrv_facts_t facts = {
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
        .want = want,
    };
    if (other != NULL) {
        facts = (proto_fwsrv_facts_t){
            .channel = channel,
            .device = other->device,
            .board_rev = other->board_rev,
            .chip_rev = other->chip_rev,
            .project = other->project,
            .version = other->version,
            .want = want,
        };
    }
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
        r = http_fetch(url, body, n, ANSWER_MAX, json, len, why, why_len, net);
    }
    free(body);
    return r;
}

/* A check's answer into the status, the image for Install and the language packs (OTA-016,
 * OTA-018, OTA-019, OTA-024, OTA-025). The update task only. */
static void take_answer(const proto_fwsrv_answer_t *a, const char *json, size_t len, const char *base)
{
    /* Never past its master (docs/features/11 LINK-029): an offer the cap refuses is no offer. A
     * server that ignores want offers its newest. */
    lock();
    const bool capped = a->reason == PROTO_FWSRV_UPDATE &&
                        !proto_fw_cap_allows(s_cap, s_cap_version, a->image.version);
    unlock();
    if (capped) {
        ESP_LOGI(TAG, "release %s is past this panel's master: not offered", a->image.version);
    }
    const bool update = a->reason == PROTO_FWSRV_UPDATE && !capped;
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
    char want[sizeof(s_cap_version)] = ""; /* a paired keypad: its master's version (LINK-029) */
    lock();
    const bool have = s_trust.server[0] != '\0' && proto_fwsrv_base_url(s_trust.server, base, sizeof(base));
    snprintf(channel, sizeof(channel), "%s", s_st.channel);
    if (s_cap == PROTO_FW_CAP_AT) {
        snprintf(want, sizeof(want), "%s", s_cap_version);
    }
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
    const char *wanted = want[0] != '\0' ? want : NULL;
    if (post_check(base, channel, NULL, wanted, &json, &len, why, why_len, NULL) == FETCH_OK) {
        proto_fwsrv_answer_t *a = calloc(1, sizeof(*a));
        if (a == NULL) {
            reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        } else if (proto_fwsrv_check_answer_want(json, len, base, wanted, a) != ALARM_OK) {
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

/* ---- a check that names the release (docs/features/11 PROBE, FETCH, a keypad's copy) -------- */

/* A check's why in words, for the status and `last` (English with its values, as the other
 * reasons with a value; at most 63 bytes, as error and last.reason take). */
static void check_why_words(uint8_t code, const char *want, const char *offered, char *why,
                            size_t why_len)
{
    switch (code) {
    case PROTO_FWSRV_WHY_NO_NETWORK:
        reason(why, why_len, I18N_NOOP_C("update", "No network"));
        break;
    case PROTO_FWSRV_WHY_NOT_A_SERVER:
        reason(why, why_len, I18N_NOOP_C("update", "The release list is damaged"));
        break;
    case PROTO_FWSRV_WHY_UNKNOWN_DEVICE:
        reason(why, why_len,
               I18N_NOOP_C("update", "The update server has no firmware for this panel"));
        break;
    case PROTO_FWSRV_WHY_HELD_BACK:
        snprintf(why, why_len, "Held back from this panel: %.24s", want);
        break;
    case PROTO_FWSRV_WHY_STEPPING_STONE:
        snprintf(why, why_len, "%.24s needs another version first", want);
        break;
    case PROTO_FWSRV_WHY_OTHER_VERSION:
        snprintf(why, why_len, "The server offers %.16s, not %.16s", offered, want);
        break;
    case PROTO_FWSRV_WHY_UP_TO_DATE:
        reason(why, why_len, I18N_NOOP_C("update", "The update server offers nothing newer"));
        break;
    case PROTO_FWSRV_WHY_UNKNOWN_VERSION:
        snprintf(why, why_len, "No release of %.24s for this panel", want);
        break;
    case PROTO_FWSRV_WHY_NOT_OFFERED:
    case PROTO_FWSRV_WHY_REFUSED:
        snprintf(why, why_len, "%.24s is not offered to this panel", want);
        break;
    case PROTO_FW_WHY_NO_MEMORY:
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        break;
    case PROTO_FW_WHY_TOO_SLOW:
        reason(why, why_len, I18N_NOOP_C("update", "The connection is too slow"));
        break;
    default: /* not reached, or broke off */
        reason(why, why_len, I18N_NOOP_C("update", "The update server can't be reached"));
        break;
    }
}

/* One check with want at base and channel (with other's facts, or this panel's), read the way
 * FWS-028 answers it. *a gets the answer (the caller's, on the heap); json_out (may be NULL)
 * keeps the answer's text for take_answer (the caller frees it). Returns its why:
 * proto_fwsrv_check_why's (OK only for an offer of exactly want), or the reach and memory
 * classes. The update task only. */
static uint8_t check_want(const char *base, const char *channel, const proto_fwsrv_facts_t *other,
                          const char *want, proto_fwsrv_answer_t *a, char **json_out,
                          size_t *len_out)
{
    if (json_out != NULL) {
        *json_out = NULL;
        *len_out = 0;
    }
    if (!network_up()) {
        return PROTO_FWSRV_WHY_NO_NETWORK;
    }
    char why[64] = "";
    char *json = NULL;
    size_t len = 0;
    uint8_t net = PROTO_FWSRV_WHY_SERVER_ERROR;
    if (post_check(base, channel, other, want, &json, &len, why, sizeof(why), &net) != FETCH_OK) {
        ESP_LOGW(TAG, "check for %s: %s", want, why);
        free(json);
        return net;
    }
    const alarm_result_t r = proto_fwsrv_check_answer_want(json, len, base, want, a);
    const uint8_t code = (uint8_t)proto_fwsrv_check_why(r, a, want);
    if (json_out != NULL && r == ALARM_OK) {
        *json_out = json;
        *len_out = len;
    } else {
        free(json);
    }
    return code;
}

bool panel_ota_key_trusted(const uint8_t key_sha256[32])
{
    if (key_sha256 == NULL || s_lock == NULL) {
        return false;
    }
    lock();
    const bool trusted = (s_own_key_ok && memcmp(key_sha256, s_own_key, 32u) == 0) ||
                         (s_trust.has_key && memcmp(key_sha256, s_trust.key_sha256, 32u) == 0);
    unlock();
    return trusted;
}

/* PROBE (docs/features/11 "The keypad"): one check with this panel's facts and want = V at the
 * order's server and channel; its answer is kept as any check's is. The update task. */
static void probe_tick(void)
{
    probe_req_t req;
    memset(&req, 0, sizeof(req));
    char own_server[sizeof(s_trust.server)];
    char own_channel[sizeof(s_st.channel)];
    lock();
    const bool go = s_probe_req.pending && s_st.phase != PANEL_OTA_RESTARTING;
    if (go) {
        req = s_probe_req;
        s_probe_req.pending = false;
    }
    snprintf(own_server, sizeof(own_server), "%s", s_trust.server);
    snprintf(own_channel, sizeof(own_channel), "%s", s_st.channel);
    unlock();
    if (!go) {
        return;
    }
    const char *server = req.no_server ? "" : req.server[0] != '\0' ? req.server : own_server;
    const bool own = req.server[0] == '\0' || strcmp(req.server, own_server) == 0;
    char base[PROTO_FWSRV_BASE_MAX];
    uint8_t code = PROTO_FWSRV_WHY_NO_LOOKUP; /* no server to ask, or none that can be one */
    proto_fwsrv_answer_t *a = NULL;
    if (server[0] != '\0' && proto_fwsrv_base_url(server, base, sizeof(base))) {
        a = calloc(1, sizeof(*a));
        if (a == NULL) {
            code = PROTO_FW_WHY_NO_MEMORY;
        } else {
            char *json = NULL;
            size_t len = 0;
            code = check_want(base, req.channel[0] != '\0' ? req.channel : own_channel, NULL,
                              req.version, a, &json, &len);
            if (json != NULL) { /* an answer: kept as any check's is */
                take_answer(a, json, len, base);
                if (!own) {
                    s_have_art = false; /* another server's offer is never Install's */
                }
                const time_t now = time(NULL);
                lock();
                s_st.checked = now > 1600000000 ? (uint32_t)now : 0u;
                unlock();
            }
            free(json);
        }
    }
    const bool offer =
        a != NULL && code != PROTO_FWSRV_WHY_NOT_A_SERVER && a->reason == PROTO_FWSRV_UPDATE;
    const bool trusted = !offer || !a->has_key || panel_ota_key_trusted(a->key_sha256);
    lock();
    s_ost.probing = s_probe_req.pending;
    s_ost.probed = true;
    s_ost.probe_why = code;
    s_ost.probe_ms = alarm_service_now();
    memset(s_ost.offered_key, 0, sizeof(s_ost.offered_key));
    s_ost.offered_has_key = false;
    s_ost.offered[0] = '\0';
    if (offer) {
        snprintf(s_ost.offered, sizeof(s_ost.offered), "%s", a->image.version);
        memcpy(s_ost.offered_key, a->key_sha256, sizeof(s_ost.offered_key));
        s_ost.offered_has_key = a->has_key;
    }
    s_ost.key_trusted = trusted;
    s_ost.seq++;
    changed_locked();
    unlock();
    ESP_LOGI(TAG, "probe for %s: why %u%s%s", req.version, (unsigned)code,
             offer ? ", offered " : "", offer ? a->image.version : "");
    free(a);
}

/* FETCH's check (docs/features/11): want = V at the order's server and channel; the offer must be
 * exactly V, signed with a key this panel trusts (LINK-036: refused before any download). out
 * gets the image. false: why and code say what stopped it. The update task. */
static bool ordered_check(const char *version, const char *base, const char *channel,
                          proto_fw_artifact_t *out, char *why, size_t why_len, uint8_t *code)
{
    proto_fwsrv_answer_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        *code = PROTO_FW_WHY_NO_MEMORY;
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        return false;
    }
    set_phase(PANEL_OTA_CHECKING, NULL); /* the run's stage stays DOWNLOADING (connecting) */
    *code = check_want(base, channel, NULL, version, a, NULL, NULL);
    bool ok = *code == PROTO_FWSRV_WHY_OK;
    if (!ok) {
        check_why_words(*code, version, a->reason == PROTO_FWSRV_UPDATE ? a->image.version : "",
                        why, why_len);
    } else if (a->has_key && !panel_ota_key_trusted(a->key_sha256)) {
        char fp[PROTO_FWSRV_FINGERPRINT_LEN];
        proto_fwsrv_fingerprint(a->key_sha256, fp);
        snprintf(why, why_len, "Signed with a key not trusted here (%s)", fp);
        *code = PROTO_FW_WHY_KEY;
        ok = false;
    } else {
        *out = a->image;
    }
    free(a);
    return ok;
}

/* For a master's copy (ota_source.c): a check at this panel's own server and channel with a
 * keypad's facts and want. The update task. */
uint8_t panel_ota_check_other(const proto_fwsrv_facts_t *facts, const char *want,
                              proto_fwsrv_answer_t *a)
{
    char base[PROTO_FWSRV_BASE_MAX];
    char channel[sizeof(s_st.channel)];
    lock();
    const bool have =
        s_trust.server[0] != '\0' && proto_fwsrv_base_url(s_trust.server, base, sizeof(base));
    snprintf(channel, sizeof(channel), "%s", s_st.channel);
    unlock();
    if (!have) {
        return PROTO_FWSRV_WHY_NO_LOOKUP;
    }
    return check_want(base, channel, facts, want, a, NULL, NULL);
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
    fetch_t f = http_fetch(url, NULL, 0, KEYS_MAX, &json, &len, why, sizeof(why), NULL);
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
    f = post_check(base, channel, NULL, NULL, &json, &len, why, sizeof(why), NULL);
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
        } else if (http_fetch(url, NULL, 0, ANSWER_MAX, &json, &len, why, sizeof(why), NULL) ==
                   FETCH_OK) {
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

/* panel_ota_download, with a flag that ends it (NULL: none; ESP_ERR_INVALID_STATE then) and its
 * why in the panel link's numbers (code, may be NULL). */
static esp_err_t download_into(const char *url, uint32_t size, const uint8_t *sha256, uint8_t *out,
                               size_t cap, size_t *got, char *why, size_t why_len,
                               const volatile bool *stop, uint8_t *code)
{
    uint8_t whyc = PROTO_FW_WHY_OK;
    if (got != NULL) {
        *got = 0;
    }
    if (url == NULL || out == NULL || cap == 0u || size > cap) {
        reason(why, why_len, "The download does not fit");
        if (code != NULL) {
            *code = PROTO_FW_WHY_TOO_BIG;
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!network_up()) {
        reason(why, why_len, I18N_NOOP_C("update", "No network"));
        if (code != NULL) {
            *code = PROTO_FWSRV_WHY_NO_NETWORK;
        }
        return ESP_ERR_INVALID_STATE;
    }
    int64_t length = 0;
    esp_http_client_handle_t c = http_open(url, NULL, 0, &length, NULL, why, why_len, stop, code);
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
        if (code != NULL) {
            *code = length > 0 && (uint64_t)length > cap ? PROTO_FW_WHY_TOO_BIG
                                                          : PROTO_FW_WHY_BAD_SIZE;
        }
        return ESP_ERR_INVALID_SIZE;
    }
    psa_hash_operation_t hash = psa_hash_operation_init();
    const bool hashing = sha256 != NULL;
    if (hashing && psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        reason(why, why_len, I18N_NOOP_C("update", "Out of memory"));
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (code != NULL) {
            *code = PROTO_FW_WHY_NO_MEMORY;
        }
        return ESP_ERR_NO_MEM;
    }
    uint32_t total = 0;
    esp_err_t err = ESP_OK;
    body_reader_t rd;
    body_reader_init(&rd, stop, 0u);
    while (total < want) {
        const uint32_t room = want - total;
        size_t n = 0;
        const body_t b = body_fill(c, &rd, (char *)out + total, room < HTTP_BUF ? room : HTTP_BUF, &n);
        if (hashing && n > 0u) {
            (void)psa_hash_update(&hash, out + total, n);
        }
        total += (uint32_t)n;
        if (b == BODY_STOPPED) {
            reason(why, why_len, "Stopped");
            whyc = PROTO_FW_WHY_CANCELLED;
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (b == BODY_SLOW) {
            reason(why, why_len, I18N_NOOP_C("update", "The connection is too slow"));
            whyc = PROTO_FW_WHY_TOO_SLOW;
            err = ESP_FAIL;
            break;
        }
        if (b != BODY_FULL && total < want) {
            reason(why, why_len, "The download broke off");
            whyc = PROTO_FW_WHY_BROKE_OFF;
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
                whyc = PROTO_FW_WHY_BAD_SHA;
                err = ESP_ERR_INVALID_CRC;
            }
        }
    }
    if (err == ESP_OK && got != NULL) {
        *got = total;
    }
    if (code != NULL) {
        *code = whyc;
    }
    return err;
}

esp_err_t panel_ota_download(const char *url, uint32_t size, const uint8_t *sha256, uint8_t *out,
                             size_t cap, size_t *got, char *why, size_t why_len)
{
    return download_into(url, size, sha256, out, cap, got, why, why_len, NULL, NULL);
}

esp_err_t panel_ota_download_stoppable(const char *url, uint32_t size, const uint8_t *sha256,
                                       uint8_t *out, size_t cap, size_t *got,
                                       const volatile bool *stop, uint8_t *code)
{
    char why[64] = "";
    const esp_err_t err =
        download_into(url, size, sha256, out, cap, got, why, sizeof(why), stop, code);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "download: %s", why);
    }
    return err;
}

/* A stop was asked (OTA-027): the download reads this between its steps. */
static bool stop_asked(void)
{
    return s_stop;
}

/* The release run's download, verification and hand-over to the wait (OTA-026), for the image art
 * (the release Install takes, or a keypad's ordered V). Every way out ends the run: waiting (the
 * image is verified), stopped (finish_stop) or failed (install_failed). */
static void install_image(const proto_fw_artifact_t *art, bool older, char *why, size_t why_len)
{
    lock();
    snprintf(s_st.target, sizeof(s_st.target), "%s", art->version);
    s_st.bytes = 0;
    s_st.size = art->size;
    unlock();
    set_phase(PANEL_OTA_RECEIVING, NULL); /* stage DOWNLOADING, bytes 0: connecting */
    run_listeners_if_changed();
    ESP_LOGI(TAG, "downloading %s from %s", art->version, art->url);
    int64_t length = 0;
    uint8_t code = PROTO_FWSRV_WHY_SERVER_ERROR;
    esp_http_client_handle_t c =
        http_open(art->url, NULL, 0, &length, NULL, why, why_len, &s_stop, &code);
    if (c == NULL) {
        if (stop_asked()) {
            finish_stop();
        } else {
            install_failed(why, code);
        }
        return;
    }
    uint32_t size = art->size;
    if (size == 0u && length > 0) {
        size = (uint32_t)length;
    }
    esp_err_t err = ESP_OK;
    const char *fail = NULL;
    if (size == 0u || (length > 0 && (uint64_t)length != size)) {
        fail = "The download has the wrong size";
        code = PROTO_FW_WHY_BAD_SIZE;
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = writer_begin(size, art->has_sha256 ? art->sha256 : NULL, older,
                           PANEL_OTA_KIND_RELEASE, NULL);
        if (err != ESP_OK) {
            fail = "Another update is running";
            code = s_writer_why;
        }
    }
    char *buf = err == ESP_OK ? malloc(HTTP_BUF) : NULL;
    if (err == ESP_OK && buf == NULL) {
        panel_ota_abort();
        fail = I18N_NOOP_C("update", "Out of memory");
        code = PROTO_FW_WHY_NO_MEMORY;
        err = ESP_ERR_NO_MEM;
    }
    uint32_t got = 0;
    bool stopped = false;
    body_reader_t rd;
    body_reader_init(&rd, &s_stop, 0u);
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
            err = panel_ota_write(buf, n, why, why_len); /* sets FAILED itself */
            if (err != ESP_OK) {
                fail = why;
                code = s_writer_why;
                break;
            }
            got += (uint32_t)n;
            run_listeners_if_changed();
        }
        if (b == BODY_SLOW) {
            panel_ota_abort();
            fail = I18N_NOOP_C("update", "The connection is too slow");
            code = PROTO_FW_WHY_TOO_SLOW;
            err = ESP_FAIL;
        } else if (b != BODY_FULL && got < size) {
            panel_ota_abort();
            fail = "The download broke off";
            code = PROTO_FW_WHY_BROKE_OFF;
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
        install_failed(fail != NULL ? fail : why, code);
        return;
    }
    lock();
    s_st.stage = PANEL_OTA_STAGE_VERIFYING;
    changed_locked();
    unlock();
    run_listeners_if_changed();
    char version[32];
    if (panel_ota_end(version, sizeof(version), why, why_len) != ESP_OK) {
        if (stop_asked()) {
            finish_stop(); /* nothing stays ready either way */
        } else {
            install_failed(why, s_writer_why);
        }
        return;
    }
    if (stop_asked()) {
        finish_stop(); /* verified, but nothing stays ready: Install downloads again */
        return;
    }
    if (strcmp(version, art->version) != 0) {
        ESP_LOGW(TAG, "the release list says %s, the image is %s", art->version, version);
    }
    panel_ota_apply(&s_install_src, s_install_user); /* stage WAITING */
}

/* The release run (OTA-026): Install's, or a keypad's ordered FETCH (docs/features/11), for V
 * only at the order's server and channel. Every way out ends the run: waiting (the image is
 * verified), stopped (finish_stop) or failed (install_failed). The stop flag is read between
 * redirects, after each blocking call, between the 4 KB pieces, before a broken read counts as a
 * failure and after the verification. */
static void do_install(void)
{
    char why[64] = "";
    if (stop_asked()) {
        finish_stop();
        return;
    }
    lock();
    const bool ordered = s_order.op == ORDER_FETCH;
    unlock();
    if (ordered) {
        /* On the heap: the TLS handshake further down needs this task's stack. */
        typedef struct {
            char version[sizeof(s_order.version)];
            char base[PROTO_FWSRV_BASE_MAX];
            char channel[sizeof(s_order.channel)];
            proto_fw_artifact_t art;
        } fetch_job_t;
        fetch_job_t *job = calloc(1, sizeof(*job));
        uint8_t code = PROTO_FW_WHY_NO_MEMORY;
        if (job != NULL) {
            lock();
            snprintf(job->version, sizeof(job->version), "%s", s_order.version);
            snprintf(job->base, sizeof(job->base), "%s", s_order.base);
            snprintf(job->channel, sizeof(job->channel), "%s", s_order.channel);
            unlock();
        }
        if (job == NULL || !ordered_check(job->version, job->base, job->channel, &job->art, why,
                                          sizeof(why), &code)) {
            if (job == NULL) {
                reason(why, sizeof(why), I18N_NOOP_C("update", "Out of memory"));
            }
            free(job);
            if (stop_asked()) {
                finish_stop();
            } else {
                install_failed(why, code);
            }
            return;
        }
        if (stop_asked()) {
            free(job);
            finish_stop();
            return;
        }
        install_image(&job->art, false, why, sizeof(why)); /* never allow_older when ordered */
        free(job);
        return;
    }
    if (s_have_art) { /* never past its master (LINK-029): a cap set since the last check */
        lock();
        const bool allowed = proto_fw_cap_allows(s_cap, s_cap_version, s_art.version);
        unlock();
        s_have_art = allowed;
    }
    if (!s_have_art && !do_check(why, sizeof(why))) {
        if (stop_asked()) {
            finish_stop();
        } else {
            install_failed(why, PROTO_FW_WHY_OK);
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
        install_failed(I18N_NOOP_C("update", "No network"), PROTO_FWSRV_WHY_NO_NETWORK);
        return;
    }
    install_image(&s_art, s_install_older, why, sizeof(why));
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

/* A release run is accepted (call with the lock held, nothing busy): Install's, or a keypad's
 * ordered FETCH (target V). */
static void install_accept_locked(const alarm_source_t *src, uint8_t user, bool allow_older,
                                  const char *target)
{
    s_install_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    s_install_user = user;
    s_install_older = allow_older;
    s_install_pending = true;
    s_install_wait_until = alarm_service_now() + INSTALL_WAIT_MS;
    s_stop = false;
    if (s_st.last.result == PANEL_OTA_LAST_STOPPED || s_st.last.result == PANEL_OTA_LAST_FAILED) {
        memset(&s_st.last, 0, sizeof(s_st.last)); /* OTA-029: until the next Install */
    }
    run_begin_locked(PANEL_OTA_KIND_RELEASE, PANEL_OTA_STAGE_DOWNLOADING, target, s_st.running);
    s_st.error[0] = '\0';
    if (s_st.phase == PANEL_OTA_FAILED) {
        s_st.phase = PANEL_OTA_IDLE;
    }
    changed_locked();
}

esp_err_t panel_ota_install(const alarm_source_t *src, uint8_t user, bool allow_older)
{
    ESP_RETURN_ON_FALSE(s_task != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    lock();
    /* OTA-030: one run at a time, and none during the self-test. Only the language packs' brief
     * hold (s_busy with no run) is waited for. Whoever gets here first under the lock wins. */
    const bool busy = run_busy_locked();
    /* A keypad that hasn't heard its master's version installs nothing (LINK-029). */
    const bool capped = s_cap == PROTO_FW_CAP_UNKNOWN;
    if (!busy && !capped) {
        install_accept_locked(src, user, allow_older, "");
    }
    unlock();
    ESP_RETURN_ON_FALSE(!busy, ESP_ERR_INVALID_STATE, TAG, "an update is running");
    ESP_RETURN_ON_FALSE(!capped, ESP_ERR_NOT_ALLOWED, TAG, "its master's version isn't known yet");
    panel_ota_source_own_run(); /* a copy for a keypad is of the version that runs now */
    notify(NB_INSTALL);
    return ESP_OK;
}

/* ---- the keypad's side of updates through the link (docs/features/11 "The keypad") ----------- */

void panel_ota_set_checks(bool on)
{
    s_checks_on = on;
}

void panel_ota_set_version_cap(proto_fw_cap_t cap, const char *version, const char *master)
{
    if (s_lock == NULL) {
        s_cap = cap; /* before the start: nobody else reads it yet */
        snprintf(s_cap_version, sizeof(s_cap_version), "%s", version != NULL ? version : "");
        snprintf(s_master, sizeof(s_master), "%s", master != NULL ? master : "");
        return;
    }
    lock();
    const bool changed = s_cap != cap ||
                         strcmp(s_cap_version, version != NULL ? version : "") != 0 ||
                         strcmp(s_master, master != NULL ? master : "") != 0;
    s_cap = cap;
    snprintf(s_cap_version, sizeof(s_cap_version), "%s", version != NULL ? version : "");
    snprintf(s_master, sizeof(s_master), "%s", master != NULL ? master : "");
    if (changed) {
        changed_locked();
    }
    unlock();
}

proto_fw_cap_t panel_ota_get_version_cap(char *version, char *master)
{
    if (s_lock != NULL) {
        lock();
    }
    const proto_fw_cap_t cap = s_cap;
    if (version != NULL) {
        snprintf(version, sizeof(s_cap_version), "%s", s_cap_version);
    }
    if (master != NULL) {
        snprintf(master, sizeof(s_master), "%s", s_master);
    }
    if (s_lock != NULL) {
        unlock();
    }
    return cap;
}

void panel_ota_cap_words(char *out, size_t len)
{
    if (s_lock != NULL) {
        lock();
    }
    if (s_cap == PROTO_FW_CAP_NONE) {
        snprintf(out, len, "%s", "");
    } else {
        cap_words_locked(out, len);
    }
    if (s_lock != NULL) {
        unlock();
    }
}

void panel_ota_set_restart_hold(const char *reason_literal)
{
    const char *was = __atomic_exchange_n(&s_restart_hold_why, reason_literal, __ATOMIC_ACQ_REL);
    /* Only a change wakes the update task: NB_STATUS runs the listeners, and panel_app's
     * republishes the firmware state on MQTT, which a caller's poll loop must not do each turn. */
    const bool changed = was != reason_literal &&
                         (was == NULL || reason_literal == NULL || strcmp(was, reason_literal) != 0);
    if (changed) {
        notify(NB_STATUS); /* the wait's words, at the next tick */
    }
}

void panel_ota_set_restart_with(const panel_ota_with_t *with)
{
    s_with_set = false;
    __sync_synchronize();
    if (with != NULL) {
        s_with = *with;
        __sync_synchronize();
        s_with_set = true;
    }
}

bool panel_ota_restart_soon(void)
{
    return s_with_on;
}

void panel_ota_set_trial_check(panel_ota_trial_fn fn, void *ctx)
{
    s_trial_ctx = ctx;
    __atomic_store_n(&s_trial_fn, fn, __ATOMIC_RELEASE);
}

bool panel_ota_ordered_trial(char *version)
{
    if (s_lock == NULL) {
        return false;
    }
    lock();
    const bool on = s_ord_trial && s_st.probation;
    if (version != NULL) {
        snprintf(version, sizeof(s_ord_version), "%s", on ? s_ord_version : "");
    }
    unlock();
    return on;
}

const char *panel_ota_device(void)
{
    return device_name();
}

void panel_ota_get_facts(panel_ota_facts_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->device, sizeof(out->device), "%s", device_name());
    snprintf(out->board_rev, sizeof(out->board_rev), "%s", s_board_rev);
    snprintf(out->chip_rev, sizeof(out->chip_rev), "%s", CHIP_REV != NULL ? CHIP_REV : "");
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    out->chip_rev_full = chip.revision;
    const esp_app_desc_t *me = esp_app_get_description();
    panel_ota_version_copy(out->project, sizeof(out->project), me->project_name,
                           sizeof(me->project_name));
    panel_ota_version_copy(out->version, sizeof(out->version), me->version, sizeof(me->version));
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    out->slot = slot != NULL ? (uint32_t)slot->size : 0u;
    if (s_lock == NULL) {
        return;
    }
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    snprintf(out->cp_version, sizeof(out->cp_version), "%s", ws.coproc_fw);
    lock();
    if (s_own_key_ok) {
        memcpy(out->own_key, s_own_key, sizeof(out->own_key));
        out->has_own_key = true;
    }
    if (s_trust.has_key) {
        memcpy(out->accepted_key, s_trust.key_sha256, sizeof(out->accepted_key));
        out->has_accepted_key = true;
    }
    unlock();
}

/* Its own release waiting in its spare slot (call with the lock held). */
static bool spare_valid_locked(void)
{
    return s_spare.ok && s_ready != NULL && s_ready == s_spare.part &&
           LEADS_KEYPADS(s_ready_kind) &&
           (s_st.phase == PANEL_OTA_WAITING || s_st.phase == PANEL_OTA_RESTARTING) &&
           strcmp(s_st.ready, s_spare.img.version) == 0;
}

esp_err_t panel_ota_spare_image(panel_ota_image_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    const bool ok = spare_valid_locked();
    if (ok) {
        *out = s_spare.img;
    }
    unlock();
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t panel_ota_spare_read(uint32_t offset, void *out, size_t len)
{
    if (out == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    const bool ok = spare_valid_locked();
    const esp_partition_t *part = s_spare.part;
    const uint32_t size = s_spare.img.size;
    unlock();
    if (!ok) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((uint64_t)offset + len > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return panel_ota_source_part_read(part, offset, out, len);
}

bool panel_ota_own_waiting(void)
{
    if (s_lock == NULL) {
        return false;
    }
    lock();
    const bool waiting = spare_valid_locked() && !s_st.probation;
    unlock();
    return waiting;
}

void panel_ota_set_keypads_check(panel_ota_keypads_fn fn, void *ctx)
{
    s_keypads_ctx = ctx;
    __atomic_store_n(&s_keypads_fn, fn, __ATOMIC_RELEASE);
    if (fn != NULL) {
        __atomic_or_fetch(&s_required, (uint32_t)PANEL_OTA_OK_KEYPADS, __ATOMIC_RELEASE);
    } else {
        __atomic_and_fetch(&s_required, ~(uint32_t)PANEL_OTA_OK_KEYPADS, __ATOMIC_RELEASE);
    }
}

/* An order's version, channel and server, made ready for its run: false when the version or a
 * channel it names can't be one. The server is checked only where it is used: has_base says
 * whether the order's server (or this panel's own) makes an address, which only a FETCH needs - a
 * PROBE without one is answered NO_LOOKUP 2, and a RELAY uses none. */
static bool order_prepare(const panel_ota_order_t *o, order_t *out)
{
    memset(out, 0, sizeof(*out));
    if (o == NULL || o->version == NULL || o->version[0] == '\0' ||
        strlen(o->version) >= sizeof(out->version)) {
        return false;
    }
    proto_version_t v;
    if (!proto_version_parse(o->version, &v)) {
        return false;
    }
    snprintf(out->version, sizeof(out->version), "%s", o->version);
    const char *channel = o->channel != NULL ? o->channel : "";
    if (channel[0] != '\0' && !proto_fwsrv_channel_valid(channel)) {
        return false;
    }
    lock();
    const char *server = o->server != NULL && o->server[0] != '\0' ? o->server : s_trust.server;
    out->own_server = o->server == NULL || o->server[0] == '\0' ||
                      strcmp(o->server, s_trust.server) == 0;
    out->has_base =
        server[0] != '\0' && proto_fwsrv_base_url(server, out->base, sizeof(out->base));
    if (!out->has_base) {
        out->base[0] = '\0';
    }
    snprintf(out->channel, sizeof(out->channel), "%s", channel[0] != '\0' ? channel : s_st.channel);
    /* "Waiting for <master>" (P2 decision 10): the order's name for it, else the one its INFO
     * gave; English with a value, as the other reasons with one (the keypad's page builds its own
     * line). With no name at all, the fixed sentence, which the touchscreen translates. */
    const char *master = o->master != NULL && o->master[0] != '\0' ? o->master
                         : s_master[0] != '\0'                       ? s_master
                                                                     : NULL;
    if (master != NULL) {
        char words[sizeof(out->waiting) + 48u];
        snprintf(words, sizeof(words), "Waiting for %s", master);
        (void)i18n_utf8_copy(out->waiting, sizeof(out->waiting), words); /* a whole character */
    } else {
        reason(out->waiting, sizeof(out->waiting),
               I18N_NOOP_C("update", "Waiting for the alarm panel"));
    }
    unlock();
    return true;
}

/* LINK-036 (decision 1): the server and key a FETCH or RELAY names become this panel's trust
 * record when they differ - in RAM at once (the signature check uses the key), stored by the
 * update task (trust_save_tick). The running image's own key keeps no extra key. Nobody is asked:
 * only the paired master's sealed orders get here. */
static void order_adopt(const panel_ota_order_t *o)
{
    static const uint8_t none[32] = {0};
    const bool keyed = o->key != NULL && memcmp(o->key, none, sizeof(none)) != 0;
    char base[PROTO_FWSRV_BASE_MAX];
    lock();
    proto_fwsrv_trust_t t = s_trust;
    bool changed = false;
    if (o->server != NULL && o->server[0] != '\0' && strlen(o->server) < sizeof(t.server) &&
        strcmp(o->server, t.server) != 0 && proto_fwsrv_base_url(o->server, base, sizeof(base))) {
        snprintf(t.server, sizeof(t.server), "%s", o->server);
        changed = true;
    }
    if (keyed && s_own_key_ok && memcmp(o->key, s_own_key, 32u) == 0) {
        changed = changed || t.has_key;
        t.has_key = false; /* its own key: trusted anyway */
        memset(t.key_sha256, 0, sizeof(t.key_sha256));
        t.key_name[0] = '\0';
    } else if (keyed && (!t.has_key || memcmp(o->key, t.key_sha256, 32u) != 0)) {
        memcpy(t.key_sha256, o->key, 32u);
        t.has_key = true;
        t.key_name[0] = '\0';
        changed = true;
    }
    if (changed) {
        s_trust = t;
        panel_ota_trust_set(t.has_key ? t.key_sha256 : NULL);
        server_to_status_locked();
        s_trust_save = true;
        changed_locked();
    }
    /* Its master's channel too: a keypad's own Install (maintenance, the needs-update state) then
     * asks the server on the channel its master's release is on. */
    if (o->channel != NULL && proto_fwsrv_channel_valid(o->channel) &&
        strcmp(o->channel, s_st.channel) != 0) {
        snprintf(s_st.channel, sizeof(s_st.channel), "%s", o->channel);
        s_chan_order_save = true;
        changed_locked();
    }
    unlock();
    if (changed) {
        char fp[PROTO_FWSRV_FINGERPRINT_LEN];
        proto_fwsrv_fingerprint(t.key_sha256, fp);
        ESP_LOGW(TAG, "update server and key from its master's order: %s%s%s", t.server,
                 t.has_key ? ", key " : " (the running image's key)", t.has_key ? fp : "");
        notify(NB_SERVER);
    }
}

/* The update task: an order's trust record stored (OTA-020's record, "trust"). Tried again at the
 * next tick when NVS refuses it. */
static void trust_save_tick(void)
{
    if (s_chan_order_save) {
        char channel[sizeof(s_st.channel)];
        lock();
        s_chan_order_save = false;
        snprintf(channel, sizeof(channel), "%s", s_st.channel);
        unlock();
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
            ESP_LOGE(TAG, "the channel from its master's order could not be stored (%s)",
                     esp_err_to_name(err));
            s_chan_order_save = true; /* tried again */
        } else {
            ESP_LOGI(TAG, "update channel from its master's order: %s", channel);
        }
    }
    if (!s_trust_save) {
        return;
    }
    lock();
    const proto_fwsrv_trust_t t = s_trust;
    s_trust_save = false;
    unlock();
    uint8_t blob[PROTO_FWSRV_TRUST_BLOB_MAX];
    const size_t n = proto_fwsrv_trust_encode(&t, blob, sizeof(blob));
    esp_err_t err = n != 0u ? ESP_OK : ESP_ERR_INVALID_SIZE;
    nvs_handle_t h;
    if (err == ESP_OK && (err = nvs_open(NVS_NS, NVS_READWRITE, &h)) == ESP_OK) {
        err = nvs_set_blob(h, "trust", blob, n);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the update server from its master's order could not be stored (%s)",
                 esp_err_to_name(err));
        s_trust_save = n != 0u; /* tried again */
    }
}

/* A new ordered run may start: V newer than what runs, within the cap, nothing else running
 * (call with the lock held). Its why when not. */
static uint8_t order_allowed_locked(const char *version)
{
    if (!proto_version_is_upgrade(s_st.running, version, false)) {
        return PROTO_FW_WHY_NOT_NEWER; /* never older or equal (OTA-014) */
    }
    /* No stored cap here: an ordered run is capped at its order's version, its master's target,
     * which may be ahead of what its master runs (LINK-029; check_header holds it to V). */
    /* A run or the self-test (OTA-030); the language packs' brief hold of the machinery isn't one:
     * a FETCH waits for it as Install does, a RELAY's writer refuses it (BUSY). */
    if (run_busy_locked() || s_slot_selected) {
        return PROTO_FW_WHY_BUSY;
    }
    return PROTO_FW_WHY_OK;
}

static esp_err_t order_refused(uint8_t why)
{
    lock();
    if (s_order.op == ORDER_NONE) { /* a run that goes on keeps its own why */
        s_why = why;
        s_ost.seq++;
        changed_locked();
    }
    unlock();
    switch (why) {
    case PROTO_FW_WHY_BAD_ORDER:
        return ESP_ERR_INVALID_ARG;
    case PROTO_FW_WHY_NOT_NEWER:
        return ESP_ERR_INVALID_VERSION;
    case PROTO_FW_WHY_WRONG_VERSION:
    case PROTO_FW_WHY_KEY:
        return ESP_ERR_NOT_ALLOWED;
    case PROTO_FW_WHY_TOO_BIG:
        return ESP_ERR_INVALID_SIZE;
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t panel_ota_order_probe(const panel_ota_order_t *o)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    order_t ord;
    if (!order_prepare(o, &ord)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* No usable server is no refusal: probe_tick answers NO_LOOKUP 2 (not reached), and the
     * master relays. A name too long to keep is never cut into another one. */
    const char *server = o->server != NULL ? o->server : "";
    const bool fits = strlen(server) < sizeof(s_probe_req.server);
    lock();
    memset(&s_probe_req, 0, sizeof(s_probe_req));
    s_probe_req.pending = true;
    s_probe_req.no_server = !fits;
    snprintf(s_probe_req.version, sizeof(s_probe_req.version), "%s", ord.version);
    snprintf(s_probe_req.server, sizeof(s_probe_req.server), "%s", fits ? server : "");
    snprintf(s_probe_req.channel, sizeof(s_probe_req.channel), "%s", ord.channel);
    s_ost.probing = true;
    s_ost.seq++;
    changed_locked();
    unlock();
    notify(NB_PROBE);
    return ESP_OK;
}

esp_err_t panel_ota_order_fetch(const panel_ota_order_t *o)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    order_t ord;
    order_adopt(o); /* LINK-036: its master's server and key, before the check asks any */
    if (!order_prepare(o, &ord) || !ord.has_base) { /* a FETCH asks a server: it needs one */
        return order_refused(PROTO_FW_WHY_BAD_ORDER);
    }
    ord.op = ORDER_FETCH;
    lock();
    const uint8_t why = order_allowed_locked(ord.version);
    if (why == PROTO_FW_WHY_OK) {
        const alarm_source_t sys = {.kind = ALARM_SRC_SYSTEM, .id = 0};
        install_accept_locked(&sys, 0u, false, ord.version);
        s_order = ord;
        s_order_hold = !s_order.install;
        s_why = PROTO_FW_WHY_OK;
        s_ost.seq++;
    }
    unlock();
    if (why != PROTO_FW_WHY_OK) {
        return order_refused(why);
    }
    ESP_LOGI(TAG, "ordered: fetch %s", ord.version);
    notify(NB_INSTALL);
    return ESP_OK;
}

esp_err_t panel_ota_order_relay(const panel_ota_order_t *o, uint32_t size, const uint8_t sha256[32])
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    order_t ord;
    if (sha256 == NULL || !order_prepare(o, &ord)) { /* no server needed: the bytes come by link */
        return order_refused(PROTO_FW_WHY_BAD_ORDER);
    }
    order_adopt(o); /* LINK-036: its master's key checks this image's signature */
    ord.op = ORDER_RELAY;
    lock();
    const uint8_t why = order_allowed_locked(ord.version);
    unlock();
    if (why != PROTO_FW_WHY_OK) {
        return order_refused(why);
    }
    /* The writer, on the caller's task (the link task: an internal-RAM stack, it writes flash) */
    if (!wlock()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = writer_begin(size, sha256, false, PANEL_OTA_KIND_LINK, &ord);
    wunlock();
    if (err != ESP_OK) {
        return order_refused(s_writer_why != PROTO_FW_WHY_OK ? s_writer_why : PROTO_FW_WHY_FLASH);
    }
    ESP_LOGI(TAG, "ordered: relay %s, %" PRIu32 " bytes", ord.version, size);
    return ESP_OK;
}

esp_err_t panel_ota_order_install(const char *version)
{
    if (s_lock == NULL || version == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    const bool found = s_order.op != ORDER_NONE && strcmp(s_order.version, version) == 0;
    if (found && !s_order.install) {
        s_order.install = true;
        s_order_hold = false;
        s_ost.seq++;
        changed_locked(); /* wakes the update task: the restart is tried at once */
    }
    unlock();
    if (found) {
        ESP_LOGI(TAG, "ordered: install %s", version);
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Ends the ordered run with why (CANCELLED, TIMEOUT): a stop while it runs or waits. */
static esp_err_t order_end(uint8_t why)
{
    if (s_lock == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (s_order.op != ORDER_NONE) {
        const alarm_source_t sys = {.kind = ALARM_SRC_SYSTEM, .id = 0};
        err = stop_locked(&sys, 0u, why);
    }
    unlock();
    return err;
}

esp_err_t panel_ota_order_fail(uint8_t why)
{
    if (!wlock()) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    const bool relay = s_st.kind == PANEL_OTA_KIND_LINK && s_w.active;
    unlock();
    if (relay) {
        /* The words of proposal P2's keypad pages and its master's rows, which name no panel
         * (P2 decision 10: never "the alarm panel"), so the touchscreen translates them. */
        writer_fail(why == PROTO_FW_WHY_STALLED   ? I18N_NOOP_C("update", "No data for a minute")
                    : why == PROTO_FW_WHY_TIMEOUT ? I18N_NOOP_C("update", "It waited too long")
                                                  : I18N_NOOP_C("update", "The transfer failed"),
                    why);
    }
    wunlock();
    return relay ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t panel_ota_order_cancel(void)
{
    const esp_err_t err = order_end(PROTO_FW_WHY_CANCELLED);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ordered run cancelled");
    }
    return err;
}

void panel_ota_order_get(panel_ota_order_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    lock();
    *out = s_ost;
    out->active = s_order.op != ORDER_NONE;
    out->relay = s_order.op == ORDER_RELAY;
    out->ready = out->active && s_order.ready && s_ready != NULL;
    out->install = out->active && s_order.install;
    snprintf(out->version, sizeof(out->version), "%s", s_order.version);
    out->why = s_why;
    unlock();
}

/* The update task: a READY image no INSTALL came for in 24 h is discarded (LINK-035). */
static void order_tick(uint64_t now)
{
    lock();
    const bool stale = s_order.op != ORDER_NONE && s_order.ready && !s_order.install &&
                       now - s_order.ready_ms >= ORDER_READY_MAX_MS;
    unlock();
    if (stale && order_end(PROTO_FW_WHY_TIMEOUT) == ESP_OK) {
        ESP_LOGW(TAG, "the ordered image waited 24 h for its master's go: discarded");
    }
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
            install_failed("Another update is running", PROTO_FW_WHY_BUSY);
        }
        return; /* the next tick, at most a second later */
    }
    do_install();
}

/* A stop that found no download under way (waiting, or between steps) ends on the next tick. A
 * keypad's relay (docs/features/11) has its writer on the link task: it is closed here, between
 * that task's writes, and its next panel_ota_write is refused. */
static void stop_tick(void)
{
    lock();
    const bool relay = s_stop && !s_install_pending && s_st.phase != PANEL_OTA_RESTARTING &&
                       s_st.kind == PANEL_OTA_KIND_LINK;
    unlock();
    if (relay && wlock()) {
        if (s_w.active) {
            esp_ota_abort(s_w.handle);
            psa_hash_abort(&s_w.hash);
            s_w.active = false;
            lock();
            s_busy = false;
            unlock();
        }
        wunlock();
    }
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
        s_st.last.failed = rbh_get(rb.elf); /* what its self-test missed, when it said so */
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
    rec_erase(ORD_KEY); /* its trial is over */
    rec_erase("rbv"); /* an install replaced the failed version */
    rec_erase(RBH_KEY);
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
        rec_erase(RBH_KEY);
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
 * PANEL_OTA_CONFIRM_MAX_MS to prove it keeps running, and so do a master's keypads, which come
 * back only after this restart (LINK-041; ota_selftest.h). */
static void probation_tick(uint64_t now)
{
    static uint32_t shown = 0xFFFFFFFFu;
    if (!s_st.probation || now < s_probation_until_ms) {
        return;
    }
    uint32_t unproven = 0;
    const uint32_t missing = health_judge(now, &unproven);
    char what[96];
    char remote_op[33];
    bool remote_wait;
    panel_ota_remote_operation(remote_op, &remote_wait);
    /* LINK-034: after an install its master ordered, on trial with its master: WAIT holds the
     * confirmation (the trial ends by itself within 10 min), FAIL rolls back at once. */
    const panel_ota_trial_fn trial_fn = __atomic_load_n(&s_trial_fn, __ATOMIC_ACQUIRE);
    const uint8_t trial = s_ord_trial && trial_fn != NULL ? trial_fn(s_trial_ctx)
                                                          : (uint8_t)PANEL_OTA_TRIAL_NONE;
    const bool trial_wait = trial == PANEL_OTA_TRIAL_WAIT;
    panel_ota_selftest_t step = panel_ota_selftest_step(
        missing, unproven, remote_wait || trial_wait, now, s_probation_until_ms,
        trial_wait ? UINT64_MAX : s_probation_deadline_ms);
    if (trial == PANEL_OTA_TRIAL_FAIL && now >= s_probation_until_ms) {
        step = PANEL_OTA_SELFTEST_ROLL_BACK;
    }
    if (step == PANEL_OTA_SELFTEST_CONFIRM) {
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
    if (step == PANEL_OTA_SELFTEST_WAIT) {
        if (unproven != shown) {
            shown = unproven;
            health_text(unproven, what, sizeof(what));
            if (what[0] == '\0') {
                snprintf(what, sizeof(what), "its master to run %s", s_ord_version);
            }
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
        /* for the image it returns to: "no keypad answered during its self-test" (LINK-041); only
         * on a master that asked its keypads, so a panel without them is as before */
        if ((s_required & (uint32_t)PANEL_OTA_OK_KEYPADS) != 0u) {
            rbh_put(missing != 0u ? missing : unproven);
        }
        health_text(missing != 0u ? missing : unproven, what, sizeof(what));
        if (trial == PANEL_OTA_TRIAL_FAIL) {
            ESP_LOGE(TAG, "self-test failed: its master doesn't run %s (health 0x%" PRIx32
                     "): going back with it", s_ord_version, (uint32_t)s_health);
        } else {
            ESP_LOGE(TAG,
                     "self-test failed: %s not running (health 0x%" PRIx32 ", need 0x%" PRIx32
                     "): rolling back",
                     what, (uint32_t)s_health, s_required);
        }
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

/* LINK-041: a master's keypads, once it registered the question (panel_ota_set_keypads_check). On
 * probation the part beats only while a keypad answered; otherwise always, so a keypad that is
 * away is never a fault of the master's. The update task. */
static void keypads_beat(void)
{
    const panel_ota_keypads_fn fn = __atomic_load_n(&s_keypads_fn, __ATOMIC_ACQUIRE);
    if (fn != NULL && (!s_st.probation || fn(s_keypads_ctx))) {
        panel_ota_healthy(PANEL_OTA_OK_KEYPADS);
    }
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
        keypads_beat();
        probation_tick(now);
        health_watch(now);
        last_tick(now);
        stop_tick();
        commit_tick();       /* NB_SERVER wakes these four */
        trust_save_tick();
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
        /* A keypad paired with a master that orders its updates runs no schedule (LINK-029). */
        if (CONFIG_PANEL_OTA_CHECK_HOURS > 0 && s_checks_on && now >= next_check) {
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
        probe_tick();                        /* NB_PROBE wakes it */
        order_tick(alarm_service_now());
        running_langs_tick();                /* NB_LANGS wakes it */
        panel_ota_source_tick(alarm_service_now(), (bits & NB_SOURCE) != 0u);
        if ((bits & NB_CP) != 0u) {
            panel_ota_cp_run();
        }
        panel_ota_cp_tick();
        try_restart();
        cp_try_restart();
    }
}

void panel_ota_notify_source(void)
{
    notify(NB_SOURCE);
}

bool panel_ota_confirmed(void)
{
    if (s_lock == NULL) {
        return false;
    }
    lock();
    const bool confirmed = !s_st.probation;
    unlock();
    return confirmed;
}

bool panel_ota_run_busy(void)
{
    if (s_lock == NULL) {
        return true;
    }
    lock();
    const bool busy = run_busy_locked();
    unlock();
    return busy;
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
    s_wlock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_wlock != NULL, ESP_ERR_NO_MEM, TAG, "writer lock");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "lock");
    /* a keypads' question registered before the start keeps its part (LINK-041) */
    s_required = required | (__atomic_load_n(&s_keypads_fn, __ATOMIC_ACQUIRE) != NULL
                                 ? (uint32_t)PANEL_OTA_OK_KEYPADS
                                 : 0u);
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
        ota_rec_t ord;
        s_ord_trial = rec_get(ORD_KEY, &ord) &&
                      memcmp(ord.elf, me->app_elf_sha256, sizeof(ord.elf)) == 0;
        if (s_ord_trial) {
            snprintf(s_ord_version, sizeof(s_ord_version), "%s", ord.version);
            ESP_LOGW(TAG, "an install its master ordered: on trial with its master (LINK-034)");
        }
    } else {
        note_rollback();
        rec_erase(ORD_KEY); /* confirmed, or back: no trial */
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
