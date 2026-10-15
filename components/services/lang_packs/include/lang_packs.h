/*
 * lang_packs.h - language packs (docs/features/37 language-packs.md): every language but English
 * is a signed pack file in the `assets` partition, loaded into PSRAM when the panel speaks it.
 *
 * Readers (lang_packs_catalog, lang_packs_list, lang_packs_known, lang_packs_generation) touch
 * only RAM and may be called from any task. Everything that reads flash runs on the start-up
 * task or this service's own task, both with internal-RAM stacks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "i18n/i18n.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads per boot (a registered pack is never freed: a pointer another task holds stays valid). */
#define LANG_PACKS_MAX_LOADED 8
/* Most languages lang_packs_list() reports. */
#define LANG_PACKS_LIST_MAX 16

typedef enum {
    LANG_PACK_READY,   /* made for this firmware's English */
    LANG_PACK_OTHER,   /* made for another version: some phrases may show in English */
    LANG_PACK_REFUSED, /* a draft on a release firmware */
    LANG_PACK_DAMAGED  /* its format or its signature failed */
} lang_pack_state_t;

typedef struct {
    char locale[16];
    char name[32];    /* the language's name in itself */
    char version[16]; /* the firmware version it was made with */
    bool draft;
    uint8_t state; /* lang_pack_state_t */
    uint32_t missing;
    uint32_t size; /* as stored */
    bool loaded;
    bool is_default;
    uint8_t sha256[32]; /* of the stored blob */
} lang_pack_info_t;

/* After the engine, before the screen: mounts the store, adopts the store's default language when
 * none is stored, loads the stored language's pack and starts the service task. Never fails the
 * start-up: a problem is logged and means English (I18N-042). Returns ESP_OK in every such case. */
esp_err_t lang_packs_start(void);

/* The newest loaded pack for the locale (case-insensitive), else the catalog built into the
 * image, else NULL (English). Never blocks on flash. For an installed pack that is not loaded
 * yet it queues a load on the service task and returns what there is now; the generation
 * changes once it is loaded. */
const i18n_catalog_t *lang_packs_catalog(const char *locale);

/* A font a loaded pack carries for the locale (case-insensitive), by role: "caption", "body",
 * "label", "title" or "headline" (an LVGL binary font, I18N-049). From the newest visible loaded
 * pack, RAM only, never blocks, any task; the bytes stay valid as long as the pack is loaded
 * (a registered pack is never freed). False when there is none (or the pack option is off). */
bool lang_packs_font(const char *locale, const char *role, const void **data, size_t *size);

/* The installed language packs (RAM only), at most `max`; returns how many. */
size_t lang_packs_list(lang_pack_info_t *out, size_t max);

/* Changes with every load, install and removal. */
uint32_t lang_packs_generation(void);

/* English, a built-in catalog, or an installed pack that is neither refused nor damaged. */
bool lang_packs_known(const char *locale);

/* ---- installing, removing, downloading ---- */

typedef enum {
    LANG_OK = 0,
    LANG_ERR_OFF,       /* no pack store on this panel (option off, no partition, unreadable) */
    LANG_ERR_SIZE,      /* not a multiple of 4096, under 8192 bytes, or over 2 MB */
    LANG_ERR_SIGNATURE, /* not signed with this firmware's key (or this firmware checks none) */
    LANG_ERR_FORMAT,    /* the payload is not a valid pack */
    LANG_ERR_DRAFT,     /* a draft, on a release firmware */
    LANG_ERR_ENGLISH,   /* the locale is plain English ("en", "en-US") */
    LANG_ERR_NO_SPACE,  /* no free run of sectors that long (or the table is full) */
    LANG_ERR_LOCKED,    /* a partition is armed or an alarm sounds */
    LANG_ERR_FLASH,     /* writing, reading back or committing failed */
    LANG_ERR_NOT_FOUND, /* no such installed pack */
    LANG_ERR_BUSY,      /* another download runs, or an update is being downloaded */
    LANG_ERR_NO_SOURCE  /* the update source does not list it, or could not deliver it */
} lang_result_t;

/* "ok" "off" "size" "signature" "format" "draft" "english" "no_space" "locked" "flash" "not_found"
 * "busy" "no_source" */
const char *lang_result_name(lang_result_t r);

/* Installs (or replaces) a pack from a whole file: the payload, padded to 4096, and the signature
 * sector. `file` must be 4-byte aligned (PSRAM is fine) and stay valid during the call. Refused
 * while locked. The caller is a task with an INTERNAL-RAM stack (flash writes disable the cache;
 * not alarm_mem_task_create). When the panel speaks the locale the new pack is loaded at once
 * (the screen switches); a replaced, loaded pack is hidden. */
lang_result_t lang_packs_install(const uint8_t *file, size_t len);

/* Removes an installed pack (case-insensitive locale); a loaded copy is hidden until restart.
 * Same rules for the caller. */
lang_result_t lang_packs_remove(const char *locale);

/* Asks the service task to download and install the pack the update source lists for the locale:
 * the last check's entry when it is for the running version, else the server's list for the running
 * version, else the check's entry for another version. Returns at once: LANG_OK (queued;
 * a pack no list has ends as LANG_ERR_NO_SOURCE in the status, and the next check asks for it),
 * LANG_ERR_NO_SOURCE (no update source), LANG_ERR_BUSY, LANG_ERR_LOCKED, LANG_ERR_OFF. Watch
 * lang_packs_fetch_status(). */
lang_result_t lang_packs_fetch(const char *locale);

typedef struct {
    char locale[16];
    uint8_t phase;  /* 0 idle, 1 downloading, 2 done, 3 failed */
    uint8_t result; /* lang_result_t, once phase is 2 or 3 */
} lang_fetch_status_t;

/* The last download (automatic ones too). */
void lang_packs_fetch_status(lang_fetch_status_t *out);

/* Bytes free in the pack store for new packs: free sectors x 4096, as of the last change (0
 * without a store). A pack needs a free run that long in one piece. */
uint32_t lang_packs_free_bytes(void);

typedef struct {
    char locale[16];
    char version[32];
    uint32_t size; /* 0 = not given */
} lang_available_t;

/* The update source's languages at the last release check (at most max). */
size_t lang_packs_available(lang_available_t *out, size_t max);

/* Registers the `lang` service console command (after the console started). */
esp_err_t lang_packs_console_register(void);

#ifdef __cplusplus
}
#endif
