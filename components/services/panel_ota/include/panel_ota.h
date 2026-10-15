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
#include "alarm_proto/proto_fw.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_OTA_CONFIRM_MS 60000u /* shortest probation after an update (OTA-005) */
#define PANEL_OTA_URL_LEN    192u
#define PANEL_OTA_SERVER_LEN 96u  /* a domain (OTA-017), or a development address set at the console */
#define PANEL_OTA_KEY_NAME_LEN 48u

/* Probation is extended while every required part still reports but hasn't been reporting
 * long enough yet (a subsystem that comes up late). It never lasts beyond this. */
#define PANEL_OTA_CONFIRM_MAX_MS 180000u

/* The heartbeat contract for panel_ota_healthy() (OTA-005). */
#define PANEL_OTA_HEALTH_PERIOD_MS 5000u  /* report at least this often, from the working task */
#define PANEL_OTA_HEALTH_STALE_MS  15000u /* silent for longer = that part is not running */
#define PANEL_OTA_HEALTH_SPAN_MS   30000u /* reports must cover this much before confirming */
#define PANEL_OTA_HEALTH_GRACE_MS  30000u /* after the start: parts may still be coming up */

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
} panel_ota_health_t;

#define PANEL_OTA_HEALTH_COUNT 5u /* bits defined above (append a bit, then raise this) */

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
 * 30 s for it, then fails with "Another update is running" (never dropped silently). */
esp_err_t panel_ota_install(const alarm_source_t *src, uint8_t user, bool allow_older);

/* Stops a release update before the panel switches to it (OTA-027): while it downloads, is
 * verified, waits, or in the seconds before the restart (the restart is given up). The stage turns
 * "stopping" at once; the run ends within seconds (up to a minute on a stalled connection), the
 * download is thrown away, ALARM_FW_STOPPED is logged with who stopped it and `last` says
 * "stopped". Allowed while armed. ESP_OK also while it is already stopping;
 * ESP_ERR_NOT_FOUND: nothing runs; ESP_ERR_NOT_SUPPORTED: a Wi-Fi module update or a developer's
 * file, which can't be stopped this way. */
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

/* ---- the writer (serial upload, later the Home Assistant link) ------------------------------ */

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

/* Registers the "ota" and "c6" service console commands (after the console started). */
esp_err_t panel_ota_console_register(void);

#ifdef __cplusplus
}
#endif
