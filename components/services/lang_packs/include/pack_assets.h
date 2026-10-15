/*
 * pack_assets.h - the one owner of the `assets` partition's pack store (docs/features/08
 * voice-first-build.md "Installing a voice pack", AUD-027; the store: docs/features/37
 * language-packs.md, I18N-044). Language packs (kind 1), voice packs (kind 2) and the asset packs
 * (kinds 3-5: letters, picture, certificates; docs/features/39 asset-packs.md) all go through
 * it: it holds the only ps_store_t, the partition, the mutex, the signature check and the update
 * machinery's claim. Nothing else mounts or writes the partition.
 *
 * Built when language packs or voice are on (CONFIG_ALARM_FEATURE_LANG_PACKS or
 * CONFIG_ALARM_FEATURE_VOICE); otherwise every call answers "off".
 *
 * Flash rule: everything here that reads or writes flash (pa_init, pa_read, pa_view, pa_verify,
 * pa_install, pa_remove) runs only on a task with an INTERNAL-RAM stack (flash writes and the
 * signature check run with the cache off; not alarm_mem_task_create). pa_ready, pa_free_bytes and
 * pa_generation read only RAM, from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pack_store/pack_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A pack file's size bounds, by kind: a non-zero multiple of 4096, at least two sectors (some
 * payload and the signature sector), at most this. */
#define PA_MIN_FILE 8192u
#define PA_LANGUAGE_MAX_FILE (2u * 1024u * 1024u)
#define PA_VOICE_MAX_FILE 970752u
/* The asset packs (docs/features/39 asset-packs.md "The kinds", OPT-006): the whole signed file. */
#define PA_LETTERS_MAX_FILE (256u * 1024u)
#define PA_PICTURE_MAX_FILE (512u * 1024u)
#define PA_CERTS_MAX_FILE (128u * 1024u)

typedef enum {
    PA_OK = 0,
    PA_ERR_OFF,       /* no store (option off, no partition, not a store's size, unreadable) */
    PA_ERR_SIZE,      /* outside the kind's bounds, not a multiple of 4096, or an unknown kind */
    PA_ERR_BUSY,      /* the update machinery's claim was refused (an update runs) */
    PA_ERR_SIGNATURE, /* not signed with this firmware's key, or this firmware checks none */
    PA_ERR_FORMAT,    /* the envelope: payload length at offset 8, sector padding, one signature sector */
    PA_ERR_REJECTED,  /* the kind's validate callback refused it: its code is in *reject */
    PA_ERR_NO_SPACE,  /* no free run holds the whole new copy beside the working one, or the table is full */
    PA_ERR_CLOSED,    /* the gate closed before the erase or before the table commit: nothing committed */
    PA_ERR_FLASH,     /* erase, write, read-back, hashing, the signature check itself, or the commit failed */
    PA_ERR_NOT_FOUND  /* no such entry */
} pa_result_t;

/* "ok" "off" "size" "busy" "signature" "format" "rejected" "no_space" "closed" "flash" "not_found" */
const char *pa_result_name(pa_result_t r);

/* Finds the `assets` partition, creates the mutex and mounts the store; once (later calls return
 * the first answer). Call it from the start-up task, from whichever of language packs and voice
 * starts first. PA_OK: mounted. PA_ERR_OFF: no partition, not a store's size, or no memory.
 * PA_ERR_FLASH: the partition could not be read. */
pa_result_t pa_init(void);

/* The store is mounted. RAM only. */
bool pa_ready(void);

/* The one mutex (recursive). Hold it while using a pointer from pa_find / pa_entry_at: a commit
 * (any kind's install or removal) moves the entries. pa_install and pa_remove take it themselves;
 * language packs also hold it over their loads. Never take it while holding a lock a holder of
 * this one waits for. */
void pa_lock(void);
void pa_unlock(void);

/* The entry of this kind and name (exact case), or NULL. pa_lock held. */
const ps_entry_t *pa_find(uint8_t kind, const char *name);

/* The table's entries, every kind, in table order. pa_lock held. */
uint16_t pa_count(void);
const ps_entry_t *pa_entry_at(uint16_t i);

/* Copies a stored blob whole (e->length bytes) into dst, through the flash mapping in chunks.
 * The bytes are not checked: pa_verify() them. pa_lock held (e stays valid). */
bool pa_read(const ps_entry_t *e, void *dst);

/* Maps the first `len` bytes of a stored blob (at most its length) and calls fn on them; the
 * mapping is gone when fn returns: keep no pointer into it. false when it could not be mapped
 * (fn not called). pa_lock held. */
bool pa_view(const ps_entry_t *e, uint32_t len, void (*fn)(const void *p, size_t len, void *ctx), void *ctx);

/* Checks a whole pack file in RAM: SHA-256 over buf[0 .. len - 4096) against the signature sector
 * at the end, with the key the running firmware was signed with (len a multiple of 4096, at least
 * two sectors). The check shares a flash mapping with the update's image check, so it runs under
 * the update machinery's claim: `claimed` says the caller holds it already; otherwise it is taken
 * here and ESP_ERR_NOT_FINISHED means it was refused (nothing judged: try again later).
 * ESP_OK: it verifies. ESP_ERR_IMAGE_INVALID: a bad signature. ESP_ERR_NOT_SUPPORTED: this
 * firmware checks no signatures (every pack is refused). Anything else: the check could not run. */
esp_err_t pa_verify(const uint8_t *buf, uint32_t len, bool claimed);

typedef struct {
    uint8_t kind; /* PS_KIND_LANGUAGE, PS_KIND_VOICE, PS_KIND_LETTERS, PS_KIND_PICTURE or PS_KIND_CERTS */
    const uint8_t *file; /* the whole signed file: 4-byte aligned (PSRAM is fine), valid during the call */
    size_t len;
    bool ff_padding; /* the padding between payload and signature sector must be 0xFF */
    /* The kind's own checks on the verified file (signature and envelope are good); writes the
     * entry's name (1..15 printable ASCII). Returns 0 to go on, else a code of the kind's own
     * that pa_install hands back through *reject. Update claim held, not the mutex. */
    int (*validate)(const uint8_t *file, size_t len, uint32_t payload_length, char name[PS_NAME_MAX + 1],
                    void *ctx);
    /* Re-checks the caller's gate (armed, alarm, tests, maintenance...) before the erase and again
     * just before the table commit: false aborts with PS_ERR_CLOSED, nothing committed. NULL:
     * always open. Mutex and claim held: keep it short, take no lock a holder of pa_lock waits for. */
    ps_gate_fn gate;
    /* After the table names the new blob (e: the entry now in the table), mutex and claim held:
     * the kind activates it (rebuilds a cache, loads it). May be NULL. */
    void (*done)(const ps_entry_t *e, void *ctx);
    void *ctx;
} pa_install_t;

/* Installs (or replaces, same kind and name) a pack from a whole file, as one transaction:
 * size bounds for the kind; the update claim (PA_ERR_BUSY); the signature (PA_ERR_SIGNATURE, or
 * PA_ERR_FLASH when the check could not run); the envelope (PA_ERR_FORMAT); validate
 * (PA_ERR_REJECTED); then, under the mutex, ps_put_blob(): admitted only when free sectors hold the
 * whole new copy beside the working one (PA_ERR_NO_SPACE), gate, erase, write, read back and
 * compare, gate, commit; then done. Any failure before the commit leaves the working copy and
 * every other entry as they were. The caller checks its own gate first (and its result codes);
 * `reject` may be NULL. */
pa_result_t pa_install(const pa_install_t *req, int *reject);

/* Removes an entry (exact case) with a new table. PA_ERR_NOT_FOUND, or PA_ERR_FLASH when the
 * entry is still there afterwards (a failed commit that did not land). No update claim. */
pa_result_t pa_remove(uint8_t kind, const char *name);

/* Free blob sectors x 4096, as of the last mount or commit (0 without a store). A pack needs a
 * free run that long in one piece. RAM only. */
uint32_t pa_free_bytes(void);

/* Changes with every commit (an install or a removal of any kind). RAM only. */
uint32_t pa_generation(void);

#ifdef __cplusplus
}
#endif
