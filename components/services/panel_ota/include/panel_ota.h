/*
 * panel_ota.h - firmware updates for this panel and its Wi-Fi co-processor (docs/features/13).
 *
 * - Images are signed (RSA-3072, signed apps without eFuse Secure Boot): esp_ota_end() rejects
 *   anything not signed with the key of the running image (OTA-001).
 * - A second signing key: the one key the installer accepted for the update server is tried
 *   behind ESP-IDF's own check, never instead of it, and fails closed (OTA-020).
 * - One writer (panel_ota_begin/write/end) takes images from any transport: the update
 *   server (HTTP(S) download), the serial console ("ota recv", tools/ota_serial.py) and later
 *   the Home Assistant link. Checks: project name, chip, version (older only when forced).
 * - A new image is started only when the system is idle (proto_fw_install_allowed, OTA-004):
 *   the state is asked of the alarm task again after the announcement and the log flush, and
 *   the boot partition is switched in the last moment before the restart, so arming or an
 *   alarm during those seconds only costs the attempt and an unplanned restart keeps the old
 *   image.
 * - The first boot of a new image is on probation (OTA-005): every required part of the panel
 *   must keep reporting healthy (see panel_ota_healthy) for at least PANEL_OTA_CONFIRM_MS,
 *   otherwise it marks itself invalid and the previous image returns. A crash or reset during
 *   probation also returns the previous image (bootloader).
 * - Every step is an event in the log and on MQTT (ALARM_EV_FIRMWARE_UPDATE, OTA-010).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "alarm_proto/ota_selftest.h" /* the self-test's times and rule (OTA-005, LINK-041) */
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_fwsrv.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PANEL_OTA_CONFIRM_MS, PANEL_OTA_CONFIRM_MAX_MS, the heartbeat contract for panel_ota_healthy()
 * (PANEL_OTA_HEALTH_PERIOD_MS, _STALE_MS, _SPAN_MS, _GRACE_MS) and PANEL_OTA_KEYPADS_SPAN_MS are
 * in alarm_proto/ota_selftest.h, so the host tests check the self-test with the panel's times. */
#define PANEL_OTA_URL_LEN    192u
#define PANEL_OTA_SERVER_LEN 96u  /* a domain (OTA-017), or a development address set at the console */
#define PANEL_OTA_KEY_NAME_LEN 48u

typedef enum {
    PANEL_OTA_IDLE = 0,
    PANEL_OTA_CHECKING,    /* asking the update server (OTA-018) */
    PANEL_OTA_RECEIVING,   /* writing a new image into the other slot */
    PANEL_OTA_WAITING,     /* image verified: restarts as soon as the system is idle */
    PANEL_OTA_RESTARTING,
    PANEL_OTA_FAILED,      /* the last check or install failed: see error */
} panel_ota_phase_t;

/* What an update run is doing (OTA-026). A plain check (Check now, the daily one, its retries) has
 * no stage: phase PANEL_OTA_CHECKING alone. */
typedef enum {
    PANEL_OTA_STAGE_NONE = 0,
    PANEL_OTA_STAGE_DOWNLOADING,  /* connecting (bytes 0) or writing the image into the other slot */
    PANEL_OTA_STAGE_VERIFYING,    /* its length, SHA-256 and signature */
    PANEL_OTA_STAGE_WAITING,      /* verified: installs once everything is disarmed and quiet (reason) */
    PANEL_OTA_STAGE_INSTALLING,   /* the restart is announced (about 3 s): RESTARTING */
    PANEL_OTA_STAGE_STOPPING,     /* a stop was asked; the run ends within seconds (OTA-027) */
    PANEL_OTA_STAGE_ROLLING_BACK, /* the self-test failed: restarting with `previous` (OTA-031) */
    PANEL_OTA_STAGE_WIFI_MODULE,  /* the Wi-Fi co-processor's own update: no steps, no stop */
    PANEL_OTA_STAGE_FILE,         /* a developer's file (upload, console, panelctl): no steps, no stop */
} panel_ota_stage_t;

/* Where the running update's image comes from, set when the run starts (OTA-026). */
typedef enum {
    PANEL_OTA_KIND_NONE = 0,
    PANEL_OTA_KIND_RELEASE,     /* the update server */
    PANEL_OTA_KIND_FILE,        /* a developer's file */
    PANEL_OTA_KIND_WIFI_MODULE, /* the cp_fw partition, to the co-processor */
    PANEL_OTA_KIND_LINK,        /* from the alarm panel: its master relays it over the panel link
                                   (docs/features/11 RELAY), with steps and Stop */
} panel_ota_kind_t;

/* How the last update run ended (OTA-029), kept until it no longer helps anyone:
 * - STOPPED and FAILED: until the next Install or check;
 * - ROLLED_BACK: until an install replaces the failed version or a newer one is offered;
 * - UPDATED: for 24 hours by the clock (rebuilt from the event log's newest "updated" record, so it
 *   survives the restart), or until a newer version is offered. */
typedef enum {
    PANEL_OTA_LAST_NONE = 0,
    PANEL_OTA_LAST_UPDATED,
    PANEL_OTA_LAST_ROLLED_BACK,
    PANEL_OTA_LAST_STOPPED,
    PANEL_OTA_LAST_FAILED,
} panel_ota_last_result_t;

typedef struct {
    panel_ota_last_result_t result;
    char version[32];       /* the version the run was for */
    char from[32];          /* the version that ran before it */
    uint32_t at;            /* Unix time it ended, 0 = unknown (no clock) */
    char reason[64];        /* FAILED: why, in the panel's words; otherwise "" */
    alarm_source_t by_src;  /* STOPPED: who stopped it; otherwise who asked for it */
    uint8_t by_user;        /* that person's user number, 0 = none or the system */
    uint32_t failed;        /* ROLLED_BACK: the self-test's parts that failed (panel_ota_health_t;
                               PANEL_OTA_OK_KEYPADS: no keypad answered, docs/features/11 P2
                               decision 15); 0 = not known (a crash, an older firmware's record) */
} panel_ota_last_t;

typedef struct {
    panel_ota_phase_t phase;
    char running[32];            /* this image's version */
    char latest[32];             /* newest release from the last check, "" = never checked */
    bool available;              /* latest is newer than running */
    char notes_url[PANEL_OTA_URL_LEN];
    char source[PANEL_OTA_URL_LEN]; /* BL-186: the update server's address, "" = none; use server */
    int percent;                 /* PANEL_OTA_RECEIVING: 0..100, -1 = unknown size */
    char ready[32];              /* PANEL_OTA_WAITING: the verified image's version */
    char waiting_for[40];        /* PANEL_OTA_WAITING: what holds the restart back */
    char error[64];              /* PANEL_OTA_FAILED: why */
    bool probation;              /* first boot of an update, not confirmed yet */
    char rolled_back[32];        /* an update to this version failed and was undone, "" = no */
    char cp_running[24];         /* Wi-Fi co-processor firmware, "" = unknown */
    char cp_image[24];           /* co-processor image waiting in "cp_fw", "" = none */
    uint32_t checked;            /* Unix time the last check finished (either way), 0 = never */

    /* ---- the run (OTA-026..OTA-031) ---- */
    panel_ota_stage_t stage;
    panel_ota_kind_t kind;
    char target[32];             /* the version being installed, "" = none */
    char previous[32];           /* what a failed self-test returns to ("" when not known) */
    uint32_t bytes;              /* DOWNLOADING: bytes written so far */
    uint32_t size;               /* DOWNLOADING: the image's size, 0 = not known yet */
    char reason[64];             /* while a run waits or is held: what holds it ("The system is
                                    armed", "Trying again shortly"); "" when nothing does */
    bool failed_install;         /* PANEL_OTA_FAILED: an install failed (false: a check did) */
    panel_ota_last_t last;

    /* ---- the update server (OTA-017..OTA-025) ---- */
    char server[PANEL_OTA_SERVER_LEN]; /* its domain ("roboalarms.com"), "" = none */
    bool official;               /* it is this build's own (CONFIG_PANEL_OTA_DEFAULT_DOMAIN) */
    char channel[8];             /* "stable", "beta", "alpha" or "dev" (OTA-019) */
    char installed_channel[8];   /* the channel the running version is on now, "" = not known */
    bool withdrawn;              /* its publisher withdrew the running version (OTA-024) */
    bool critical;               /* the offered version is (or leads to) an important fix */
    uint32_t seq;                /* changes whenever anything above does */
} panel_ota_status_t;

/* What the self-test waits for after an update (OTA-005). Stored nowhere, but app_main and the
 * subsystems agree on it: append new bits, never renumber the ones below. */
typedef enum {
    PANEL_OTA_OK_ENGINE = 1u << 0,  /* the alarm engine runs */
    PANEL_OTA_OK_LOG = 1u << 1,     /* the event log is mounted */
    PANEL_OTA_OK_UI = 1u << 2,      /* the display and UI task draw */
    PANEL_OTA_OK_SENSORS = 1u << 3, /* zone source processing is actually running */
    PANEL_OTA_OK_TOUCH = 1u << 4,   /* local touch input is usable */
    PANEL_OTA_OK_LINK = 1u << 5,    /* a link keypad hears its master (docs/features/11 LINK-020) */
    PANEL_OTA_OK_KEYPADS = 1u << 6, /* a master's keypads answer (LINK-041): required only once
                                       panel_ota_set_keypads_check() registered the question */
} panel_ota_health_t;

#define PANEL_OTA_HEALTH_COUNT 7u /* bits defined above (append a bit, then raise this) */

/* After alarm_service_start(), from a task with an internal-RAM stack: logs an undone update once,
 * starts probation when this is the first boot of an update, loads the update server and its
 * accepted key, and starts the update task (checks the server daily). 'required' is the set of panel_ota_health_t the self-test needs.
 *
 * The mask is this hardware profile's contract: a build that leaves PANEL_OTA_OK_SENSORS or
 * PANEL_OTA_OK_TOUCH out declares that the panel protects and is operated some other way, and
 * that the other way is itself in the mask. A panel with no zones configured still reports
 * PANEL_OTA_OK_SENSORS: the bit says the zone source processing runs, not that sensors exist. */
esp_err_t panel_ota_start(uint32_t required);

/* This panel's "device" in a check when CONFIG_PANEL_OTA_DEVICE is empty: the board's id (a string
 * that lives forever). Call before panel_ota_start(). */
void panel_ota_set_device(const char *id);

/* A part of the panel reports that it is still doing its work. Safe and quick from any task
 * (no lock, no blocking), also before panel_ota_start().
 *
 * This is a heartbeat, not a one-off: call it from the task that does the work, every time it
 * makes real progress and at least every PANEL_OTA_HEALTH_PERIOD_MS. "Progress" must not need
 * a person - the touch driver reports when a poll of the controller succeeds, not when a
 * finger arrives. A part that reports once and then stops fails the self-test (OTA-005) and
 * is reported as a fault at any other time too (panel_ota_health_stale). */
void panel_ota_healthy(panel_ota_health_t what);

/* Required parts (panel_ota_start's mask) that are not reporting: never did, or went quiet
 * for longer than PANEL_OTA_HEALTH_STALE_MS. 0 = everything runs. Watched all the time, not
 * only on probation, so the panel can say it isn't protecting the house. */
uint32_t panel_ota_health_stale(void);

void panel_ota_get_status(panel_ota_status_t *out);
uint32_t panel_ota_seq(void);

/* Called (on the update task) whenever the status changes. Up to four listeners. */
esp_err_t panel_ota_add_listener(void (*fn)(void *ctx), void *ctx);

/* Exclusive use of the update machinery (the writer, a co-processor update, or a language pack's
 * signature check - ESP-IDF's check shares one flash mapping handle with esp_ota_end's, docs/features/37
 * language-packs.md). false while an update receives, waits or restarts; release what you claimed. */
bool panel_ota_claim(void);
void panel_ota_release(void);

/* ---- the update server (OTA-017..OTA-025, BL-186) -------------------------------------------- */

/* The server and the key updates from it must be signed with (OTA-020). */
typedef struct {
    char domain[PANEL_OTA_SERVER_LEN]; /* "roboalarms.com"; a development address set at the
                                          console shows as typed */
    bool official;                     /* this build's own server */
    char key_name[PANEL_OTA_KEY_NAME_LEN]; /* the server's name for the key, "" = not known yet */
    uint8_t key_sha256[32];            /* that key's digest the way ESP-IDF computes it */
    bool has_key;
} panel_ota_server_t;
void panel_ota_get_server(panel_ota_server_t *out);

typedef enum {
    PANEL_OTA_SRV_NONE = 0,  /* no check asked, or it was forgotten */
    PANEL_OTA_SRV_CHECKING,
    PANEL_OTA_SRV_DONE,
} panel_ota_srv_state_t;

typedef enum {
    PANEL_OTA_SRV_OK = 0,
    PANEL_OTA_SRV_UNREACHABLE,  /* no answer, no such name, or a certificate the panel refuses */
    PANEL_OTA_SRV_NOT_A_SERVER, /* it answered, but not like a firmware server */
    PANEL_OTA_SRV_NO_FIRMWARE,  /* its catalogue doesn't have this board, chip or build */
    PANEL_OTA_SRV_BAD_DOMAIN,   /* what was typed is not a domain */
} panel_ota_srv_result_t;

/* A new server's check (OTA-018, OTA-020): it is asked for its signing keys and for firmware for
 * this panel before anything is kept. */
typedef struct {
    panel_ota_srv_state_t state;
    char domain[PANEL_OTA_SERVER_LEN];     /* as normalised ("example.net") */
    panel_ota_srv_result_t result;         /* PANEL_OTA_SRV_DONE */
    char key_name[PANEL_OTA_KEY_NAME_LEN]; /* PANEL_OTA_SRV_OK: the key its firmware for this
                                              panel is signed with */
    uint8_t key_sha256[32];
    bool trusted;                          /* the panel trusts that key already: no warning */
    uint32_t seq;                          /* changes with every new check and its end */
} panel_ota_srv_check_t;

/* Asynchronous: checks a server the installer typed (a domain; "https://" and a trailing "/" are
 * dropped). ESP_ERR_INVALID_ARG: not a domain; ESP_ERR_INVALID_STATE: an update runs (from Install
 * to the end of the self-test, OTA-030). A new check replaces an unfinished one. */
esp_err_t panel_ota_server_check(const char *typed);
void panel_ota_server_check_get(panel_ota_srv_check_t *out);
/* The installer went back: the check's answer is no longer wanted. */
void panel_ota_server_check_forget(void);

/* Keeps the checked server (OTA-020, OTA-021): only when the last check finished OK for this very
 * domain and key_sha256 equals the key it found, so what the installer saw is what is trusted.
 * factory_installer_code: the installer code is still the factory one; a key the panel doesn't
 * trust yet is then refused (the service console passes false: whoever has the cable can flash
 * the panel anyway). ESP_OK: accepted, and the check is used up; the update task stores it moments
 * later (never the caller's task: it may have its stack in PSRAM), logs ALARM_FW_SERVER_CHANGED
 * with who did it, and checks at once. Going back to the running image's key keeps no extra key.
 * ESP_ERR_INVALID_STATE: no matching finished check, or an update runs (OTA-030);
 * ESP_ERR_NOT_ALLOWED: the factory installer code. */
esp_err_t panel_ota_server_commit(const char *domain, const uint8_t key_sha256[32],
                                  bool factory_installer_code, const alarm_source_t *src,
                                  uint8_t user);

/* Development only, at the service console: the same check as panel_ota_server_check for an
 * http(s) address taken as it is ("http://192.0.2.10:8080"; a trailing "/" is dropped), kept the
 * same way with panel_ota_server_commit (domain = the address as the check shows it). Not offered
 * at the panel or in the Panel Manager. ESP_ERR_INVALID_ARG: not an http(s) address. */
esp_err_t panel_ota_server_check_dev(const char *url);

/* The channel the panel follows (OTA-019): "stable", "beta", "alpha" or "dev". The status shows it
 * at once; the update task stores it and checks. ESP_ERR_INVALID_ARG: not a channel;
 * ESP_ERR_INVALID_STATE: an update runs (OTA-030). */
esp_err_t panel_ota_set_channel(const char *channel, const alarm_source_t *src, uint8_t user);

/* The facts a check sends about this panel (OTA-018): its board revision ("1.1", "" = unknown)
 * and the add-on parts fitted ("cc1101", "zac93"). Call before panel_ota_start(); copied. */
void panel_ota_set_facts(const char *board_rev, const char *const *peripherals, size_t count);

#define PANEL_OTA_LOCALE_LEN 16u
/* The locales whose language packs this panel wants (OTA-018, OTA-025), sent with every check:
 * the language service registers the question (it depends on panel_ota, not the other way). Called
 * on the update task (internal-RAM stack); writes at most max locales, returns how many. A language
 * wanted later asks for a check (panel_ota_check), which then sends it. */
typedef size_t (*panel_ota_locales_fn)(char (*out)[PANEL_OTA_LOCALE_LEN], size_t max);
void panel_ota_set_locales(panel_ota_locales_fn fn);

/* Asynchronous: ask the update server now (OTA-018). */
esp_err_t panel_ota_check(void);

/* Asynchronous: download the release found by the last check (checking first when needed)
 * and restart into it once the system is idle. allow_older installs an older release too.
 * OTA-030: only when nothing runs - ESP_ERR_INVALID_STATE ("busy") during a run or the self-test;
 * when the update machinery is briefly held (a language pack's check) the request waits up to
 * 30 s for it, then fails with "Another update is running" (never dropped silently).
 * ESP_ERR_NOT_ALLOWED: a keypad whose master's version isn't known yet (PROTO_FW_CAP_UNKNOWN,
 * panel_ota_set_version_cap); under PROTO_FW_CAP_AT only up to its master's version installs. */
esp_err_t panel_ota_install(const alarm_source_t *src, uint8_t user, bool allow_older);

/* Stops a release update before the panel switches to it (OTA-027): while it downloads, is
 * verified, waits, or in the seconds before the restart (the restart is given up). The stage turns
 * "stopping" at once; the run ends within seconds (up to a minute on a stalled connection), the
 * download is thrown away, ALARM_FW_STOPPED is logged with who stopped it and `last` says
 * "stopped". Allowed while armed. ESP_OK also while it is already stopping;
 * ESP_ERR_NOT_FOUND: nothing runs; ESP_ERR_NOT_SUPPORTED: a Wi-Fi module update or a developer's
 * file, which can't be stopped this way. A keypad's ordered runs (docs/features/11) stop the same
 * way, a relay too: its writer is closed by the update task, and the link task's next
 * panel_ota_write is refused. */
esp_err_t panel_ota_stop(const alarm_source_t *src, uint8_t user);

/* Gives up a verified image that is waiting for the system to go idle. */
void panel_ota_cancel(void);
/* The same, only when the waiting install was prepared over the network (Feature 30): for
 * when the developer's access is revoked or switched off while it waits. */
void panel_ota_cancel_remote(void);
/* Forgets a verified image altogether (nothing "ready" to apply any more): for an image the
 * caller refuses after verification, e.g. one built for another firmware profile. Does
 * nothing while an image is being written, restarted into, or waited on for a remote
 * install. */
void panel_ota_discard(void);

/* ---- the writer (serial upload, the panel link's relay, later the Home Assistant link) ------- */

/* size: the image's exact length. sha256 (NULL = none) is checked at the end. Fails when
 * another update is running. */
esp_err_t panel_ota_begin(uint32_t size, const uint8_t *sha256, bool allow_older);
/* Appends the next bytes. The image header is checked as soon as it has arrived. why gets a
 * short reason on failure (may be NULL). */
esp_err_t panel_ota_write(const void *data, size_t len, char *why, size_t why_len);
/* Completes and verifies the image (length, SHA-256, signature). version gets its version. */
esp_err_t panel_ota_end(char *version, size_t version_len, char *why, size_t why_len);
void panel_ota_abort(void);
/* Restart into the image from panel_ota_end() as soon as the system is idle. */
esp_err_t panel_ota_apply(const alarm_source_t *src, uint8_t user);
bool panel_ota_system_idle(const char **why);

/* Who answers "is the system idle?" on a panel that runs no alarm engine of its own: a keypad of
 * the panel link answers from its master's status (docs/features/11 "Roles", OTA-004). Once set,
 * it is asked instead of the alarm task by every update, the co-processor's included: true when
 * an update may start and restart now; false with *why a literal (marked I18N_NOOP_C("update",
 * ...)) saying what holds it. Called on the update and console tasks (internal-RAM stacks): it
 * must not wait for long (one NVS write at most). Set it before panel_ota_start(); NULL fn: the
 * alarm task again. */
typedef bool (*panel_ota_idle_fn)(void *ctx, const char **why);
void panel_ota_set_idle_gate(panel_ota_idle_fn fn, void *ctx);
/* Remote-dev probation: target ELF digest and opaque operation survive the restart. */
esp_err_t panel_ota_remote_prepare(const char *operation, const uint8_t expected_elf[32]);
bool panel_ota_remote_confirm(const char *operation);
void panel_ota_remote_operation(char out[33], bool *required);

/* ---- Wi-Fi co-processor (OTA-007) ------------------------------------------------------------ */

/* Streams the image in the "cp_fw" partition to the co-processor, then restarts the panel once the
 * system is idle (OTA-004; the C6 keeps its old image until then).
 * Only when the system is idle and the image differs from what runs (or force). */
esp_err_t panel_ota_cp_update(const alarm_source_t *src, uint8_t user, bool force, char *why,
                              size_t why_len);

/* ---- language packs (docs/features/37 language-packs.md, OTA-016) --------------------------- */

#define PANEL_OTA_LANGUAGES_MAX 16u

/* The language packs the update server offered at the last successful check (at most max, in the
 * answer's order); returns how many. Safe from any task. */
size_t panel_ota_languages(proto_fw_language_t *out, size_t max);
/* Changes with every successful release check (even when the list is the same); the asset packs'
 * list (panel_ota_assets) changes with it. */
uint32_t panel_ota_languages_seq(void);

/* ---- asset packs (docs/features/39 asset-packs.md "Fetching", OPT-007) -------------------- */

#define PANEL_OTA_ASSETS_MAX 4u

/* The asset packs ("assets") the update server offered at the last successful check, kept with the
 * languages and under the same sequence number (panel_ota_languages_seq): at most max, in the
 * answer's order; returns how many. Safe from any task. */
size_t panel_ota_assets(proto_fw_asset_t *out, size_t max);
/* The asset packs of the running version's answer to panel_ota_running_languages_ask (the same
 * request, the same ticket): as panel_ota_running_languages, *n == SIZE_MAX when the request
 * failed, 0 when the answer lists none. Safe from any task. */
bool panel_ota_running_assets(uint32_t ticket, proto_fw_asset_t *out, size_t max, size_t *n);

/* OTA-025: asks the update server for the language packs of the version this panel runs
 * (GET <base>/api/v1/languages), for a language added later - a check lists the packs of the
 * release it offers, which can be a newer one. Asynchronous: the update task asks within a second
 * or two (after a download it is busy with). Returns the request's ticket, or 0 when there is no
 * update server or the service isn't running. Safe from any task. */
uint32_t panel_ota_running_languages_ask(void);
/* The answer to `ticket` (or to a later request): false while it isn't in. true: *n packs copied
 * to out (at most max; 0 = the server lists none for this version), or *n == SIZE_MAX when the
 * server couldn't be asked or didn't answer with a list. Safe from any task. */
bool panel_ota_running_languages(uint32_t ticket, proto_fw_language_t *out, size_t max, size_t *n);

/* Downloads one file (http or https: redirects, the certificate bundle) into out (cap bytes) and
 * checks its length against size (0 = take the server's) and its SHA-256 against sha256 (NULL =
 * none). got gets the length, why a short reason on failure (may be NULL). Blocking: a task with
 * an internal-RAM stack that may wait for the network, never while an update downloads. */
esp_err_t panel_ota_download(const char *url, uint32_t size, const uint8_t *sha256, uint8_t *out,
                             size_t cap, size_t *got, char *why, size_t why_len);

/* ---- updates through the panel link (docs/features/11 "Updates through the link") ---------- *
 *
 * Nothing here runs unless a caller uses it: a stand-alone panel and a master without keypads
 * update as before. The why numbers (FW_STATE's why, the same as panel_link's PLINK_FW_WHY_*) are
 * proto_fwsrv_why_t for a check's outcomes (0-19) and proto_fw_why_t for the rest (20-61). */

/* A keypad paired with a master whose session has PLINK_CAP_FW runs no scheduled check of its own
 * (LINK-029): PROBE is its check. false stops the daily check and its retries; a check a person
 * asks for, and the one after a new server or channel, still run. On by default. Any task. */
void panel_ota_set_checks(bool on);

/* Never past its master (LINK-029, decision 5). PROTO_FW_CAP_AT: the panel's own check sends
 * want = version (FWS-028) and offers nothing newer, and every writer (the console's "ota recv",
 * tools/ota_serial.py, a developer's file) refuses a newer image before its first flash write
 * with "<master> runs <version>: update it first". PROTO_FW_CAP_UNKNOWN (paired, its master's
 * version not heard yet): nothing is offered, Install is refused and every writer refuses any
 * image ("Updates come from <master>"). PROTO_FW_CAP_NONE: no cap (unpaired, a stand-alone panel,
 * a master). Only a cable (the USB bootloader) goes past. master: its name for those words (NULL
 * or "": "the alarm panel"); both strings are copied. Any task; nothing is stored (the keypad
 * keeps its master's version in NVS link/mver itself). */
void panel_ota_set_version_cap(proto_fw_cap_t cap, const char *version, const char *master);
/* The cap in force (version and master may be NULL; each takes 32 bytes). */
proto_fw_cap_t panel_ota_get_version_cap(char *version, char *master);

/* A master puts off its own restart while a keypad installs or self-tests (OTA-032, LINK-030):
 * while set, every restart into a new image or after the Wi-Fi module's update waits, and its
 * status says reason (waiting_for and reason). reason: a literal that lives for ever, marked
 * I18N_NOOP_C("update", ...) by the caller ("A keypad is checking its update"); NULL lifts the
 * hold. A rollback after a failed self-test never waits. Any task; cheap to call from a poll
 * loop: only a change of the hold (another reason, set or lifted) wakes the update task. */
void panel_ota_set_restart_hold(const char *reason);

/* LINK-041 (decision 3): on a master with keypads its self-test needs one of them back. Once a
 * question is registered, PANEL_OTA_OK_KEYPADS is required: on probation the update task beats
 * it only while fn says a keypad that was online before "installing" has a session again and
 * sends heartbeats (or that there is no such keypad); outside probation it always beats. A keypad
 * needs time to come back after its master's restart, so on probation, until
 * PANEL_OTA_CONFIRM_MAX_MS, no answer yet (or a pause in the answers) keeps the self-test waiting,
 * never fails it; answers covering PANEL_OTA_KEYPADS_SPAN_MS prove the part (ota_selftest.h).
 * Only a part still unanswered at PANEL_OTA_CONFIRM_MAX_MS rolls back. A rollback with that part
 * missing is kept in `last.failed`, so the rolled-back page can say "no keypad answered during
 * its self-test" (P2 decision 15). fn runs on the update task about once a second: quick, no
 * waiting, no flash. NULL: no question, the bit is no longer required. */
typedef bool (*panel_ota_keypads_fn)(void *ctx);
void panel_ota_set_keypads_check(panel_ota_keypads_fn fn, void *ctx);

/* LINK-030 (docs/features/11 "Restarting together"): a master's keypads download alongside it and
 * restart with it.
 *   - hold: what holds its restart into version (its own update, waiting) for its keypads - one
 *     still downloading it, one installing alone or on its self-test - a literal that lives for
 *     ever, marked I18N_NOOP_C("update", ...), or NULL. Asked with every gate (the update task's,
 *     and the alarm task's last word): quick, no waiting, no lock the alarm task may want.
 *   - installs_out: once that restart has passed its gate (everything disarmed, after the 3 s
 *     announcement), asked about every 100 ms for at most PANEL_OTA_WITH_MS whether its keypads
 *     have their INSTALL; panel_ota_restart_soon() is true meanwhile (the caller's planner sends
 *     INSTALL to every READY keypad then). Then the gate once more - hold no longer counts, nor
 *     panel_ota_set_restart_hold's: keypads restarting with it never block it - and the restart.
 * The update task, quick. NULL (or a NULL entry): none. Set it before the first restart; any task. */
#define PANEL_OTA_WITH_MS 5000u
typedef struct {
    const char *(*hold)(void *ctx, const char *version);
    bool (*installs_out)(void *ctx);
    void *ctx;
} panel_ota_with_t;
void panel_ota_set_restart_with(const panel_ota_with_t *with);
bool panel_ota_restart_soon(void);

/* This panel's "device" as its checks name it (CONFIG_PANEL_OTA_DEVICE, else the board's id from
 * panel_ota_set_device): the device a keypad of the same model names in its FW_INFO. Any task. */
const char *panel_ota_device(void);

/* The facts a keypad hands its master in FW_INFO (docs/features/11 "Messages"). Any task. */
typedef struct {
    char device[32];
    char board_rev[16];
    char chip_rev[4];        /* "v1", "v3"; "" when not known */
    uint16_t chip_rev_full;  /* esp_chip_info's revision (100 for v1.0) */
    char project[33];
    char version[32];        /* what runs */
    uint32_t slot;           /* the OTA slot's size */
    uint8_t own_key[32];     /* the running image's signing key (has_own_key) */
    bool has_own_key;
    uint8_t accepted_key[32]; /* the key accepted for the update server (has_accepted_key) */
    bool has_accepted_key;
    char cp_version[24];     /* the Wi-Fi co-processor's, "" not known */
} panel_ota_facts_t;
void panel_ota_get_facts(panel_ota_facts_t *out);

/* -- the keypad's side: its master's orders ("The keypad") -- */

/* An order, as the keypad's core took it. Copied. FETCH and RELAY: the order's server and key become
 * this panel's trust record when they differ (LINK-036, decision 1: changes from the master go to
 * its keypads; nobody is asked) - used at once, stored by the update task. The channel serves that
 * run only. */
typedef struct {
    const char *version; /* V: the master's target; the image must be exactly V. An ordered run is
                            capped at V, not at the version its master runs (panel_ota_set_version
                            _cap): the master's target may be ahead of what it runs */
    const char *server;  /* PROBE, FETCH: the update server for this run (a domain, or a development
                            address as `ota server check` takes it); NULL or "": this panel's own */
    const char *channel; /* PROBE, FETCH: the channel for this run; NULL or "": this panel's own */
    const char *master;  /* its master's name, for "Waiting for <master>"; NULL or "": "the alarm
                            panel" */
    const uint8_t *key;  /* FETCH, RELAY: the image's signing key the order names (ESP-IDF digest,
                            32 bytes); NULL or all zeros: not known. This panel's own key keeps no
                            extra key; another becomes its accepted one */
} panel_ota_order_t;

/* PROBE: one check (POST /api/v1/check) with this panel's facts and want = V, at the order's
 * server and channel, on the update task within a second (a download under way first). Its
 * answer is kept as any check's answer is (the status' latest; an offer from another server than
 * this panel's own is never kept for Install). The outcome: panel_ota_order_get() probe_*. With
 * no server to ask (the order names none and this panel has none, or the named one can't be an
 * address) the probe is answered NO_LOOKUP 2 without asking, so its master relays.
 * ESP_ERR_INVALID_ARG: no version, or a channel that can't be one; ESP_ERR_INVALID_STATE: not
 * started. A new probe replaces one not yet asked. */
esp_err_t panel_ota_order_probe(const panel_ota_order_t *o);

/* FETCH: the release run (as Install's), for V only: a check with want = V at the order's server
 * and channel, whose offer must be exactly V (else OTHER_VERSION 13, or the server's why); its
 * signing key must be this panel's own or its accepted one - the order's, once taken (else KEY 30,
 * before any download); the download into the other slot, its length, SHA-256 and signature
 * (panel_ota_end), and the image's version V (else WRONG_VERSION 41, discarded). Then READY:
 * panel_ota_apply's wait, with the restart held ("Waiting for <master>") until
 * panel_ota_order_install(V). Stop (OTA-027) works as on any release run. ESP_ERR_INVALID_ARG:
 * BAD_ORDER (no version, a bad channel, or no server it can use: neither the order's nor this
 * panel's own); ESP_ERR_INVALID_VERSION: V not newer than what runs (NOT_NEWER);
 * ESP_ERR_INVALID_STATE:
 * another run, or the self-test (BUSY): a run ended by panel_ota_order_cancel() is over within a
 * second (the update task's next tick), and an order before then is refused BUSY. */
esp_err_t panel_ota_order_fetch(const panel_ota_order_t *o);

/* RELAY: a run of PANEL_OTA_KIND_LINK through the writer, as "ota recv" (stage DOWNLOADING, bytes
 * of size; Stop works). size: the order's exact length, sha256 its digest (required). The caller
 * (the keypad's link task: an internal-RAM stack, it writes flash) then feeds the bytes in order
 * with panel_ota_write; the first PROTO_FW_HEADER_LEN are held until the header passes (magic,
 * chip, revision range, project, newer, the cap; allow_older is never set), so a wrong image is
 * refused before any flash is written. panel_ota_end checks the length, SHA-256 and signature
 * (the keypad's own keys, OTA-020), then the version V (WRONG_VERSION, discarded), and goes on to
 * READY by itself (panel_ota_apply, the restart held for INSTALL). The order's server is not
 * used, so a relay needs none. Errors as panel_ota_order_fetch (BAD_ORDER: no sha256, no
 * version, or a bad channel), and TOO_BIG (ESP_ERR_INVALID_SIZE) for a size larger than the slot.
 * Every failure's why: panel_ota_order_get(). */
esp_err_t panel_ota_order_relay(const panel_ota_order_t *o, uint32_t size,
                                const uint8_t sha256[32]);

/* INSTALL: the master's go for V. The READY image restarts once the gate passes (asked twice,
 * OTA-004, OTA-028); an INSTALL that comes before READY counts when it is. ESP_ERR_NOT_FOUND: no
 * ordered run for that version. */
esp_err_t panel_ota_order_install(const char *version);

/* CANCEL, removal, or another image's order: the ordered run ends - stopped while it runs, its
 * READY image discarded - with why CANCELLED (61) and `last` stopped. Not once the boot slot has
 * switched (then only the self-test decides). ESP_ERR_NOT_FOUND: no ordered run. A READY image
 * that waits 24 h without INSTALL is discarded the same way, with why TIMEOUT (25). Asynchronous:
 * the run is over at the update task's next tick (within a second). */
esp_err_t panel_ota_order_cancel(void);

/* The keypad's core gave up a relay it feeds (STALLED 24, TIMEOUT 25, or another why of its own):
 * the writer is closed and the run ends failed, with that why and `last` failed. Any task.
 * ESP_ERR_NOT_FOUND: no relay is being written. */
esp_err_t panel_ota_order_fail(uint8_t why);

typedef struct {
    bool active;          /* an ordered run (FETCH or RELAY) runs or waits for INSTALL */
    bool relay;           /* it is a RELAY (PANEL_OTA_KIND_LINK); else a FETCH (RELEASE) */
    bool ready;           /* verified and waiting for INSTALL (phase WAITING) */
    bool install;         /* that INSTALL has come */
    char version[32];     /* the version ordered */
    uint8_t why;          /* how the last ordered run ended: 0 while one runs, or it went well */
    /* PROBE */
    bool probing;         /* asked, not answered yet */
    bool probed;          /* answered: the fields below */
    uint8_t probe_why;    /* proto_fwsrv_why_t (and NO_MEMORY 26) */
    uint64_t probe_ms;    /* when it was answered, alarm_service_now() */
    char offered[32];     /* the version offered, "" = none */
    uint8_t offered_key[32]; /* its signing key's ESP-IDF digest */
    bool offered_has_key;
    bool key_trusted;     /* that key is this panel's own or its accepted one (none named: true) */
    uint32_t seq;         /* changes with each of the above */
} panel_ota_order_status_t;
void panel_ota_order_get(panel_ota_order_status_t *out);

/* LINK-036: a key (ESP-IDF digest) this panel trusts: the running image's own or the one its
 * installer accepted (OTA-020). */
bool panel_ota_key_trusted(const uint8_t key_sha256[32]);

/* LINK-034 ("On trial with the master"): after an install its master ordered, a keypad stays on
 * probation until its master runs that version confirmed. panel_ota keeps "ordered install of V"
 * in NVS across the restart (written with the attempt); on the self-test that follows,
 * panel_ota_ordered_trial() is true with V, and fn - the link's trial, PANEL_OTA_TRIAL_* (the
 * panel link's plink_fw_trial_t numbers) - decides with the parts' heartbeats: WAIT holds the
 * confirmation (past PANEL_OTA_CONFIRM_MAX_MS too: the trial ends by itself within 10 min), FAIL
 * rolls back at once, PASS lets the parts decide. fn runs on the update task about once a second:
 * quick, no waiting. NULL, or no ordered install: the self-test as on any panel. */
enum {
    PANEL_OTA_TRIAL_NONE = 0,
    PANEL_OTA_TRIAL_WAIT,
    PANEL_OTA_TRIAL_PASS,
    PANEL_OTA_TRIAL_FAIL,
};
typedef uint8_t (*panel_ota_trial_fn)(void *ctx);
void panel_ota_set_trial_check(panel_ota_trial_fn fn, void *ctx);
/* On probation after an install its master ordered: true, and version (32 bytes, may be NULL) V. */
bool panel_ota_ordered_trial(char *version);

/* -- the master's side: its image sources ("The master") -- */

/* The running image, for the relay from the master's own slot. Worked out once after the image
 * is confirmed (never on probation), on the update task: its exact size by the header walk
 * (proto_fw_image_size), its SHA-256 over those bytes, its version, project, chip revision range
 * and signing key. No flash is written. */
typedef struct {
    uint32_t size;            /* the image's exact length in the running slot */
    uint8_t sha256[32];       /* of those bytes (id8 = its first 8) */
    char version[33];
    char project[33];
    uint16_t chip_id;         /* PROTO_FW_CHIP_* */
    uint16_t min_rev_full;    /* the chip revisions it runs on (proto_fw_chip_rev_fits) */
    uint16_t max_rev_full;
    uint8_t key_sha256[32];   /* its signing key (block 0) as ESP-IDF names it */
    bool has_key;
} panel_ota_image_t;
/* ESP_OK; ESP_ERR_NOT_FINISHED: not worked out yet (the first call asks for it: within seconds
 * once confirmed); ESP_FAIL: the slot doesn't hold an image the walk understands. Any task. */
esp_err_t panel_ota_running_image(panel_ota_image_t *out);
/* Reads the running image through 256 KB windows of the running partition (esp_partition_mmap;
 * one stays mapped while in use, unmapped after 30 s unused). No flash is written, but mapping a
 * window disables the cache for a moment on the P4, as a flash write does: only from a task with
 * an internal-RAM stack (the link task). ESP_ERR_NOT_FINISHED: the image isn't worked out yet;
 * ESP_ERR_INVALID_SIZE: past its end. */
esp_err_t panel_ota_running_read(uint32_t offset, void *out, size_t len);

/* Its own update in its spare slot - a release downloaded and checked, waiting for the restart - for
 * the relay of the master's target (docs/features/11 "Getting the image"): its exact size, SHA-256
 * (worked out as it was written), version, project and chip revision range. ESP_OK;
 * ESP_ERR_NOT_FOUND: none waits (nothing downloaded, a developer's file, or the run ended). Any
 * task. */
esp_err_t panel_ota_spare_image(panel_ota_image_t *out);
/* Reads that image through windows of the spare partition, as panel_ota_running_read reads the
 * running one (only from a task with an internal-RAM stack: the link task). ESP_ERR_NOT_FOUND: no
 * such image now; ESP_ERR_INVALID_SIZE: past its end. */
esp_err_t panel_ota_spare_read(uint32_t offset, void *out, size_t len);

/* A copy for a keypad the master's own slot can't serve: at most 85 % of the 4,718,592 B slot. */
#define PANEL_OTA_COPY_MAX 4010803u

/* Asynchronous, on the update task: a check at this panel's own server and channel with the
 * keypad's facts (device, board_rev, chip_rev, project, version; the rest ignored) and want = V,
 * then the offered image downloaded into PSRAM - never into this panel's own spare slot, which
 * esp_ota_set_boot_partition would boot. One copy at a time: a new one replaces it, and ends a
 * download of the one before that is still under way. Freed when panel_ota_copy_free() is
 * called, after 30 min unused, or the moment this panel's own run starts (Install, a developer's
 * file): panel_ota_copy_read() refuses from then on. ESP_ERR_INVALID_ARG: no device, chip_rev,
 * project or version; ESP_ERR_INVALID_STATE: not started, or this panel's own run is under way -
 * but its release downloaded and checked, waiting for the restart: its keypads' copies of that
 * version are fetched meanwhile. */
esp_err_t panel_ota_fetch_for(const proto_fwsrv_facts_t *facts, const char *version);

typedef enum {
    PANEL_OTA_COPY_NONE = 0,
    PANEL_OTA_COPY_FETCHING,
    PANEL_OTA_COPY_READY,
    PANEL_OTA_COPY_FAILED,
} panel_ota_copy_state_t;

typedef struct {
    panel_ota_copy_state_t state;
    uint8_t why;              /* FAILED: proto_fwsrv_why_t, or proto_fw_why_t (20-29, TOO_BIG) */
    char device[32];          /* whose facts it was fetched with */
    char chip_rev[8];
    char project[33];
    char version[32];         /* V */
    uint32_t size;            /* READY: its length */
    uint8_t sha256[32];       /* READY: its digest (id8 = its first 8) */
    uint8_t key_sha256[32];   /* the signing key the server named */
    bool has_key;
    uint32_t seq;             /* changes with each of the above */
} panel_ota_copy_t;
void panel_ota_copy_get(panel_ota_copy_t *out);
/* Reads the READY copy whose digest starts with id8 (NULL: whichever is ready). ESP_ERR_NOT_FOUND:
 * no such copy; ESP_ERR_INVALID_SIZE: past its end. Any task. */
esp_err_t panel_ota_copy_read(const uint8_t *id8, uint32_t offset, void *out, size_t len);
void panel_ota_copy_free(void);

/* Registers the "ota" and "c6" service console commands (after the console started). */
esp_err_t panel_ota_console_register(void);

#ifdef __cplusplus
}
#endif
