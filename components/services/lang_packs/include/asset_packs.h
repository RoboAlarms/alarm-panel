/*
 * asset_packs.h - the asset packs on the panel (docs/features/39 asset-packs.md "The panel",
 * OPT-006): the letters (store kind 3, `letters`), the saver's picture (kind 4, `saver`) and the
 * certificates (kind 5, `certs`). Each is a signed `RAST` pack in the pack store, which pack_assets
 * owns (pack_assets.h). At start each installed one is copied into a 4-aligned PSRAM buffer, its
 * signature checked with the running firmware's key, opened with asset_pack_open() and held to its
 * kind's rules (the header's kind and name equal the store entry's; the parts; a draft refused on a
 * release firmware; the certificates only when made for the running version), then kept until
 * restart and offered to the screen and, later, to TLS.
 *
 * A pack installed after the start (asset_packs_install) loads at once for the letters and the
 * picture, and asset_packs_generation() changes; the certificates take effect at the next start
 * (their state says so). A replaced copy is never freed (a reader may hold a pointer into it): a
 * kind loads at most ASSET_PACKS_MAX_LOADS copies between restarts, after that a new one waits for
 * the next start.
 *
 * Built with ALARM_FEATURE_LANG_PACKS (the pack store's option); otherwise every call answers
 * "none" and asset_packs_install() "off".
 *
 * Flash rule (as pack_assets.h): asset_packs_start(), asset_packs_install() and
 * asset_packs_verify_all() read flash with the cache off or write it: call them only on a task
 * with an INTERNAL-RAM stack (the start-up task, the dev link's server, the language service's
 * task), never on an alarm_mem_task_create stack. Every other call reads RAM only, from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASSET_PACKS_KINDS 3      /* letters, picture, certificates: store kinds 3-5 */
#define ASSET_PACKS_MAX_LOADS 4  /* copies a kind loads between restarts */

typedef enum {
    ASSET_STATE_NONE = 0,  /* nothing installed (the kind's fallback: asset-packs.md "The kinds") */
    ASSET_STATE_READY,     /* checked and offered */
    ASSET_STATE_REFUSED,   /* whole and signed, but not for this panel: a draft on a release
                            * firmware, certificates made for another version, or a firmware that
                            * checks no signatures */
    ASSET_STATE_DAMAGED,   /* the signature, the format or the kind's rules failed */
    ASSET_STATE_RESTART,   /* installed after the start (the certificates), or the kind loaded
                            * ASSET_PACKS_MAX_LOADS copies already: used from the next start */
    ASSET_STATE_UNCHECKED  /* could not be checked (no memory, the flash read, an update held the
                            * check): not judged, tried again at the next start */
} asset_state_t;

/* "none" "ready" "refused" "damaged" "needs_restart" "unchecked" */
const char *asset_state_name(asset_state_t s);

/* "letters" "picture" "certificates" (the release manifest's words), or "unknown". */
const char *asset_kind_name(uint8_t kind);

typedef struct {
    uint8_t kind;        /* PS_KIND_LETTERS, PS_KIND_PICTURE or PS_KIND_CERTS */
    char name[16];       /* the store entry's name: "letters", "saver", "certs" */
    asset_state_t state;
    bool draft;          /* the pack checked last was made by a development build */
    char version[16];    /* the firmware version it was made with; "" when not read */
    uint32_t match;      /* the letters: the fonts' fingerprint; 0 for the others */
    uint32_t bytes;      /* the stored file's size; 0 when none is installed */
    char why[48];        /* plain English, why it is not ready; "" when ready or none */
} asset_info_t;

typedef enum {
    ASSET_OK = 0,
    ASSET_ERR_OFF,       /* no pack store (option off, no partition) */
    ASSET_ERR_SIZE,      /* not a multiple of 4096, under two sectors, or over the kind's bound */
    ASSET_ERR_SIGNATURE, /* not signed with this firmware's key, or this firmware checks none */
    ASSET_ERR_FORMAT,    /* the envelope, the RAST payload, or the kind's parts */
    ASSET_ERR_KIND,      /* a kind this firmware does not take, or a name other than the kind's */
    ASSET_ERR_DRAFT,     /* a draft pack offered to a release firmware */
    ASSET_ERR_LOCKED,    /* a partition is armed or an alarm sounds */
    ASSET_ERR_BUSY,      /* the update machinery's claim was refused: try again later */
    ASSET_ERR_NO_SPACE,  /* no free run holds the new copy beside the working one */
    ASSET_ERR_FLASH      /* erase, write, read-back, the signature check itself or the commit */
} asset_result_t;

/* "ok" "off" "size" "signature" "format" "kind" "draft" "locked" "busy" "no_space" "flash" */
const char *asset_result_name(asset_result_t r);

/* Mounts the store (pa_init, shared with the language and voice packs) and loads every kind's
 * installed pack; once. Always ESP_OK: a missing or refused pack leaves its kind's fallback.
 * Last, it hands a certificates pack made for this version to esp_crt_bundle_set() (OPT-013); one
 * that fails that check becomes refused, and TLS keeps the image's common list.
 * The start-up task, after lang_packs_start() and before anything opens a TLS connection. */
esp_err_t asset_packs_start(void);

/* The state of each kind, letters, picture, certificates in that order: at most max, the count
 * (0 without a store). RAM only. */
size_t asset_packs_list(asset_info_t *out, size_t max);

/* The store's free bytes (pa_free_bytes), 0 without a store. RAM only. */
uint32_t asset_packs_free_bytes(void);

/* Changes whenever a kind's offered copy changes (a load). RAM only. */
uint32_t asset_packs_generation(void);

/* The letters pack's LVGL binary font for a text role ("caption", "body", "label", "title"):
 * bytes valid until restart. False when no letters pack is ready or it has no part for the role.
 * RAM only. */
bool asset_packs_font(const char *role, const void **data, size_t *size);

/* The picture pack's JPEG part of this name ("photo"): bytes valid until restart. RAM only. */
bool asset_packs_picture(const char *name, const void **data, size_t *size);

/* A letters pack is ready and its `match` equals the fingerprint (the image's
 * alarm_ui_letters_fingerprint, which the screen passes in: this tier cannot see the UI's
 * generated layout). RAM only. */
bool asset_packs_letters_match(uint32_t fingerprint);

/* The image's letters fingerprint: the build's (ALARM_LETTERS_FINGERPRINT, from the layout's
 * letters.json), else the one asset_packs_letters_match() was last asked with (0 = not known).
 * The fetch (OPT-007) compares the installed letters with it. RAM only. */
uint32_t asset_packs_image_fingerprint(void);

/* The certificates pack's bundle (an ESP-IDF certificate bundle), when one made for the running
 * version was loaded at start: bytes valid until restart. RAM only. */
bool asset_packs_certs(const void **data, size_t *size);

/* Installs (or replaces) an asset pack from a whole signed .roboasset file (4-byte aligned, valid
 * during the call): refused while armed or in an alarm (ASSET_ERR_LOCKED); the kind from the
 * file's header; the size bound; then pack_assets' transaction (the update claim, the signature,
 * the envelope with 0xFF padding, the RAST format and the kind's rules, room for both copies,
 * write, read back, commit). The letters and the picture load at once; the certificates at the
 * next start. *kind (may be NULL) gets the file's kind when it was read. Internal-RAM stack. */
asset_result_t asset_packs_install(const uint8_t *file, size_t len, uint8_t *kind);

typedef struct {
    uint8_t kind;
    char name[16];
    /* ESP_OK: the signature, the format and the kind's rules all hold. ESP_ERR_IMAGE_INVALID: the
     * signature. ESP_ERR_INVALID_RESPONSE: the format or the kind's rules. ESP_ERR_NOT_FINISHED:
     * an update held the check. ESP_ERR_INVALID_VERSION: a certificates pack made for another
     * firmware version (the load refuses it too). ESP_ERR_NO_MEM, ESP_FAIL (the flash read),
     * ESP_ERR_NOT_SUPPORTED (this firmware checks no signatures). */
    esp_err_t err;
} asset_verify_t;

/* Checks every installed asset pack again from flash: at most max, the count. Internal-RAM stack
 * (the console runs it on the language service's task). Changes no state. */
size_t asset_packs_verify_all(asset_verify_t *out, size_t max);

#ifdef __cplusplus
}
#endif
