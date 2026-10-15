/*
 * lang_packs.c - language packs (docs/features/37 language-packs.md, I18N-039..I18N-042, I18N-048).
 *
 * The `assets` partition holds a pack store (components/core/pack_store), owned by pack_assets
 * (pack_assets.h: the one owner for language and voice packs, AUD-027). This service keeps a
 * header cache of its language entries in RAM (what lang_packs_list() reports), and loads a pack
 * the panel speaks into PSRAM: read through the flash cache (pa_read), copied, its signature
 * checked with the key the running firmware was signed with (pa_verify), parsed by i18n_pack_open() and registered in an
 * append-only table. A registered pack is never freed, so a pointer another task holds stays valid.
 *
 * Flash rule: every flash access (the partition read and mapping, the signature check, which reads
 * the running app's key with the cache off) happens only on the start-up task (app_main's) and on
 * this service's own task, both with internal-RAM stacks. lang_packs_catalog(), _list(), _known()
 * and _generation() read only RAM and may be called from any task.
 */
#include "lang_packs.h"

#include <string.h>

#include "i18n/i18n.h"
#include "sdkconfig.h"

const char *lang_result_name(lang_result_t r)
{
    static const char *const names[] = {"ok",        "off",    "size",   "signature", "format",
                                        "draft",     "english", "no_space", "locked",  "flash",
                                        "not_found", "busy",   "no_source"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "unknown";
}

#if CONFIG_ALARM_FEATURE_LANG_PACKS

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "alarm/alarm_snapshot.h"
#include "alarm_service.h"
#include "asset_packs.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h" /* ESP_ERR_IMAGE_INVALID: pa_verify's "bad signature" */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i18n/i18n_pack.h"
#include "lang_packs_internal.h"
#include "pack_assets.h"
#include "pack_store/pack_store.h"
#include "panel_ota.h"
#include "panel_prefs.h"

#define LP_SECTOR 4096u
#define LP_MAX_FILE PA_LANGUAGE_MAX_FILE
#define LP_PENDING 4
#define LP_BAD_MAX 16
#define LP_TASK_STACK 10240 /* internal RAM: TLS, an install and the signature check run on it in one chain */
#define LP_RETRY_MS 5000    /* the task looks again this soon after work the update machinery held off */
#define LP_AUTO_MS 60000    /* ... and this soon for an automatic pass the panel's lock held off */
#define LP_LOOKUP_POLL_MS 500 /* while the update server is asked for this version's languages */
#define LP_ASSET_LOOKUP_MS 30000 /* the running version's asset packs: waited for at most this long */
#define LP_LOOKUP_MS 300000 /* ... at most this long (the update task may be busy with a download) */

/* The update machinery's claim (panel_ota.h): ESP-IDF's signature check shares one flash mapping
 * handle with esp_ota_end's image check, so every check runs between claim and release (pa_verify,
 * pa_install), which keeps an update's writer out and is refused while one runs.
 *
 * The store's mutex (pa_lock) is also this service's load mutex: it covers loads, the registry's
 * writers and every use of an entry pointer from pa_find / pa_entry_at. */

static const char *TAG = "lang_packs";

/* One language of the store, as the header cache keeps it. */
typedef struct {
    lang_pack_info_t info; /* .loaded is filled in by lang_packs_list() */
    bool tried;            /* a load failed for want of memory or room: not queued again this boot */
} head_t;

/* One loaded pack. Filled, then published by storing s_count with release semantics. */
typedef struct {
    i18n_pack_t pack;
    uint8_t *buf;
    uint8_t sha[32];
    _Atomic bool hidden;
} slot_t;

static SemaphoreHandle_t s_lock; /* the header cache, the pending list, the bad list (short, RAM only) */
/* The big tables are in PSRAM (docs/bugs/B001: internal RAM is the reserve): nothing here is used
 * with the cache off. Only the mutexes, the counters and the small lists stay internal. */
static EXT_RAM_BSS_ATTR head_t s_head[LANG_PACKS_LIST_MAX];
static size_t s_head_n;
static EXT_RAM_BSS_ATTR head_t s_tmp[LANG_PACKS_LIST_MAX]; /* a rebuild's scratch, under pa_lock */
static EXT_RAM_BSS_ATTR uint8_t s_bad[LP_BAD_MAX][32]; /* blobs whose format or signature failed, by SHA-256 */
static uint8_t s_bad_state[LP_BAD_MAX];
static size_t s_bad_n;
static char s_pending[LP_PENDING][16];
static size_t s_pending_n;
static TaskHandle_t s_task;
static bool s_mounted; /* the store is mounted: install, remove and fetch may run */

/* The service task's other work, all under s_lock: a queued download, the automatic pass after a
 * release check, and a job a console asked for (waited on through s_job_done). */
static lang_fetch_status_t s_fetch;
static char s_fetch_req[16];
static bool s_fetch_queued;
static char s_want_extra[16]; /* a language asked for that the last check didn't list (s_lock) */
static uint32_t s_fetch_ticket; /* s_lock: the queued fetch waits for this version's list */
static TickType_t s_fetch_asked; /* s_lock: when it was asked */
static _Atomic bool s_auto_pending; /* set on panel_ota's task, cleared on this one */
static uint32_t s_auto_seen; /* the update source's language list this pass has seen: written on the start-up
                              * task (before the listener exists) and then on panel_ota's task only */
static _Atomic bool s_asset_pending; /* the asset packs' pass (OPT-007): as s_auto_pending */
/* This task only: per kind (letters, picture, certificates), a hash of the entry this pass installed
 * (its url and version), so one the kind's rules still find outdated is not fetched again. */
static uint32_t s_asset_done[ASSET_PACKS_KINDS];
/* This task only: the running version's asset packs, asked of the update server once per start
 * (PSRAM, kept), and its entries; SIZE_MAX until an answer came. */
static proto_fw_asset_t *s_asset_rlist;
static size_t s_asset_rn = SIZE_MAX;
static _Atomic bool s_retry; /* a load was held off by the update machinery: the task looks again soon */
static void (*s_job_fn)(void *);
static void *s_job_arg;
static SemaphoreHandle_t s_job_done; /* given when a job finished */
static SemaphoreHandle_t s_job_mu;   /* one job at a time */

static EXT_RAM_BSS_ATTR slot_t s_slots[LANG_PACKS_MAX_LOADED];
static _Atomic uint32_t s_count;
static _Atomic uint32_t s_generation;

/* ---- small helpers ---- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void lock(SemaphoreHandle_t m)
{
    (void)xSemaphoreTake(m, portMAX_DELAY);
}

static void unlock(SemaphoreHandle_t m)
{
    (void)xSemaphoreGive(m);
}

static bool release_build(void)
{
#if CONFIG_ALARM_BUILD_RELEASE
    return true;
#else
    return false;
#endif
}

static bool usable(const head_t *h)
{
    return h->info.state == LANG_PACK_READY || h->info.state == LANG_PACK_OTHER;
}

static void bump_generation(void)
{
    atomic_fetch_add_explicit(&s_generation, 1u, memory_order_relaxed);
}

/* The bad list (s_lock held): a blob whose load failed, remembered by its SHA-256 until restart. */
static int bad_find(const uint8_t sha[32])
{
    for (size_t i = 0; i < s_bad_n; i++) {
        if (memcmp(s_bad[i], sha, 32) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- the header cache ---- */

typedef struct {
    const ps_entry_t *e;
    lang_pack_info_t *i;
} head_view_t;

static void head_judge(const void *p, size_t len, void *ctx)
{
    const head_view_t *v = ctx;
    lang_pack_info_t *i = v->i;
    i18n_pack_t pack;
    if (i18n_pack_open(p, len, &pack) == I18N_PACK_OK && strcmp(pack.locale, v->e->name) == 0) {
        copy_str(i->name, sizeof(i->name), pack.name);
        copy_str(i->version, sizeof(i->version), pack.version);
        i->draft = pack.draft;
        i->missing = pack.missing;
        if (pack.draft && release_build()) {
            i->state = LANG_PACK_REFUSED;
        } else {
            i->state = i18n_pack_matches(&pack) ? LANG_PACK_READY : LANG_PACK_OTHER;
        }
    }
}

/* Reads one language entry's header through the mapping and judges it (pa_lock held). Flash:
 * internal-stack tasks only. */
static void head_build(const ps_entry_t *e, head_t *h)
{
    memset(h, 0, sizeof(*h));
    lang_pack_info_t *i = &h->info;
    copy_str(i->locale, sizeof(i->locale), e->name);
    i->size = e->length;
    i->is_default = (e->flags & PS_FLAG_DEFAULT) != 0u;
    memcpy(i->sha256, e->sha256, 32);
    i->state = LANG_PACK_DAMAGED;
    if (e->length < 2u * LP_SECTOR || e->length > LP_MAX_FILE || e->payload_length < I18N_PACK_HEADER_SIZE ||
        e->payload_length > e->length - LP_SECTOR) {
        return;
    }
    head_view_t v = {.e = e, .i = i};
    (void)pa_view(e, e->payload_length, head_judge, &v);
}

/* Rebuilds the cache from the mounted store (load mutex held or single-threaded start-up). */
static void cache_rebuild(void)
{
    size_t n = 0;
    const uint16_t count = pa_count();
    for (uint16_t k = 0; k < count; k++) {
        const ps_entry_t *e = pa_entry_at(k);
        if (e->kind != PS_KIND_LANGUAGE) {
            continue;
        }
        if (n == LANG_PACKS_LIST_MAX) {
            ESP_LOGW(TAG, "more than %d language packs in the store: the rest are ignored", LANG_PACKS_LIST_MAX);
            break;
        }
        head_build(e, &s_tmp[n]);
        n++;
    }
    lock(s_lock);
    for (size_t k = 0; k < n; k++) {
        const int b = bad_find(s_tmp[k].info.sha256);
        if (b >= 0) {
            s_tmp[k].info.state = s_bad_state[b];
        }
    }
    memcpy(s_head, s_tmp, n * sizeof(head_t));
    s_head_n = n;
    unlock(s_lock);
    bump_generation();
}

/* The cache entry for a locale, case-insensitive (s_lock held). */
static head_t *head_find(const char *locale)
{
    for (size_t i = 0; i < s_head_n; i++) {
        if (strcasecmp(s_head[i].info.locale, locale) == 0) {
            return &s_head[i];
        }
    }
    return NULL;
}

/* ---- the registry ---- */

static slot_t *slot_find(const char *locale)
{
    const uint32_t n = atomic_load_explicit(&s_count, memory_order_acquire);
    for (uint32_t i = n; i > 0; i--) { /* newest first */
        slot_t *s = &s_slots[i - 1u];
        if (atomic_load_explicit(&s->hidden, memory_order_relaxed)) {
            continue;
        }
        if (strcasecmp(s->pack.locale, locale) == 0) {
            return s;
        }
    }
    return NULL;
}

static bool slot_loaded(const uint8_t sha[32])
{
    const uint32_t n = atomic_load_explicit(&s_count, memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) {
        if (!atomic_load_explicit(&s_slots[i].hidden, memory_order_relaxed) &&
            memcmp(s_slots[i].sha, sha, 32) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- loading ---- */

/* Remembers a blob as unusable and shows it so in the cache. */
static void mark_unusable(const uint8_t sha[32], lang_pack_state_t state)
{
    lock(s_lock);
    int b = bad_find(sha);
    if (b < 0) {
        if (s_bad_n == LP_BAD_MAX) { /* full: the oldest goes */
            memmove(s_bad[0], s_bad[1], (LP_BAD_MAX - 1u) * 32u);
            memmove(&s_bad_state[0], &s_bad_state[1], LP_BAD_MAX - 1u);
            s_bad_n--;
        }
        b = (int)s_bad_n++;
        memcpy(s_bad[b], sha, 32);
    }
    s_bad_state[b] = (uint8_t)state;
    for (size_t i = 0; i < s_head_n; i++) {
        if (memcmp(s_head[i].info.sha256, sha, 32) == 0) {
            s_head[i].info.state = (uint8_t)state;
        }
    }
    unlock(s_lock);
    bump_generation();
}

static void mark_tried(const uint8_t sha[32])
{
    lock(s_lock);
    for (size_t i = 0; i < s_head_n; i++) {
        if (memcmp(s_head[i].info.sha256, sha, 32) == 0) {
            s_head[i].tried = true;
        }
    }
    unlock(s_lock);
}

/* Loads one stored pack into PSRAM, checks it and registers it. The load mutex is held; internal-
 * stack tasks only. `claimed`: the caller holds the update machinery's claim already. ESP_ERR_NOT_FINISHED:
 * the update machinery is busy, nothing was judged: try again later. A failure is logged and leaves English (the pack's words fall back entry by
 * entry), never more. */
static esp_err_t load_entry(const ps_entry_t *e, bool claimed)
{
    if (slot_loaded(e->sha256)) {
        return ESP_OK;
    }
    if (atomic_load_explicit(&s_count, memory_order_relaxed) >= LANG_PACKS_MAX_LOADED) {
        ESP_LOGW(TAG, "%s: %d packs loaded since the start: used from the next restart", e->name,
                 LANG_PACKS_MAX_LOADED);
        mark_tried(e->sha256);
        return ESP_ERR_NO_MEM;
    }
    lock(s_lock);
    const bool known_bad = bad_find(e->sha256) >= 0;
    unlock(s_lock);
    if (known_bad) {
        return ESP_ERR_INVALID_STATE;
    }
    if (e->length < 2u * LP_SECTOR || e->length > LP_MAX_FILE || (e->length % LP_SECTOR) != 0u ||
        e->payload_length < I18N_PACK_HEADER_SIZE || e->payload_length > e->length - LP_SECTOR) {
        ESP_LOGW(TAG, "%s: its size is not a pack's", e->name);
        mark_unusable(e->sha256, LANG_PACK_DAMAGED);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_aligned_alloc(4, e->length, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGW(TAG, "%s: no PSRAM for %u bytes: English", e->name, (unsigned)e->length);
        mark_tried(e->sha256);
        return ESP_ERR_NO_MEM;
    }
    if (!pa_read(e, buf)) {
        ESP_LOGW(TAG, "%s: the flash could not be read: English", e->name);
        heap_caps_free(buf);
        mark_tried(e->sha256);
        return ESP_FAIL;
    }
    /* ESP_ERR_NOT_FINISHED: the update claim was refused, nothing judged */
    const esp_err_t sig = pa_verify(buf, e->length, claimed);
    if (sig == ESP_ERR_NOT_FINISHED) {
        heap_caps_free(buf);
        return ESP_ERR_NOT_FINISHED;
    }
    if (sig != ESP_OK) {
        if (sig == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "%s: this firmware checks no signatures: refused", e->name);
            mark_unusable(e->sha256, LANG_PACK_REFUSED);
        } else if (sig == ESP_ERR_IMAGE_INVALID) { /* the one answer that means a bad signature */
            ESP_LOGW(TAG, "%s: the signature does not verify: refused", e->name);
            mark_unusable(e->sha256, LANG_PACK_DAMAGED);
        } else { /* the check itself could not run: not the pack's fault, not its verdict */
            ESP_LOGW(TAG, "%s: the signature could not be checked (%s): English for now", e->name,
                     esp_err_to_name(sig));
            mark_tried(e->sha256);
        }
        heap_caps_free(buf);
        return sig;
    }
    const uint32_t n = atomic_load_explicit(&s_count, memory_order_relaxed);
    slot_t *s = &s_slots[n];
    const i18n_pack_result_t r = i18n_pack_open(buf, e->payload_length, &s->pack);
    if (r != I18N_PACK_OK || strcmp(s->pack.locale, e->name) != 0) {
        ESP_LOGW(TAG, "%s: not a valid pack (%d): refused", e->name, (int)r);
        mark_unusable(e->sha256, LANG_PACK_DAMAGED);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (s->pack.draft && release_build()) {
        ESP_LOGW(TAG, "%s: a draft: a release firmware does not use it", e->name);
        mark_unusable(e->sha256, LANG_PACK_REFUSED);
        heap_caps_free(buf);
        return ESP_ERR_NOT_ALLOWED;
    }
    s->buf = buf;
    memcpy(s->sha, e->sha256, 32);
    atomic_store_explicit(&s->hidden, false, memory_order_relaxed);
    atomic_store_explicit(&s_count, n + 1u, memory_order_release); /* published last */
    bump_generation();
    ESP_LOGI(TAG, "%s loaded (%u bytes, version %s)", e->name, (unsigned)e->length, s->pack.version);
    return ESP_OK;
}

/* Queues a locale for the service task (s_lock held by the caller): once. */
static bool pending_add_locked(const char *locale)
{
    for (size_t i = 0; i < s_pending_n; i++) {
        if (strcmp(s_pending[i], locale) == 0) {
            return true;
        }
    }
    if (s_pending_n >= LP_PENDING) {
        return false;
    }
    copy_str(s_pending[s_pending_n], sizeof(s_pending[0]), locale);
    s_pending_n++;
    return true;
}

/* Loads the named pack (a header cache name, exact case). false: the update machinery held it off;
 * it is queued again and the task looks again soon. */
static bool load_named(const char *name)
{
    bool done = true;
    pa_lock();
    const ps_entry_t *e = pa_find(PS_KIND_LANGUAGE, name);
    if (e != NULL && load_entry(e, false) == ESP_ERR_NOT_FINISHED) {
        done = false;
    }
    pa_unlock();
    if (!done) {
        lock(s_lock);
        (void)pending_add_locked(name);
        unlock(s_lock);
        atomic_store_explicit(&s_retry, true, memory_order_relaxed);
    }
    return done;
}

/* ---- install and remove (I18N-045) ---- */

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool is_english(const char *locale)
{
    return strcasecmp(locale, "en") == 0 || strcasecmp(locale, "en-US") == 0;
}

/* Is a partition armed or an alarm sounding? The alarm task owns the engine: this reads a copy of
 * its snapshot, as the Panel Manager does. When it cannot tell, it says locked. */
static bool locked_now(void)
{
    alarm_snapshot_t *snap = psram_alloc(sizeof(*snap));
    if (snap == NULL) {
        return true;
    }
    alarm_service_get_snapshot(snap);
    const bool locked = alarm_snapshot_locked(snap);
    heap_caps_free(snap);
    return locked;
}

/* Forgets a blob the bad list holds (s_lock held): a pack just checked whole is not bad. */
static void bad_forget(const uint8_t sha[32])
{
    const int b = bad_find(sha);
    if (b < 0) {
        return;
    }
    const size_t tail = s_bad_n - (size_t)b - 1u;
    memmove(s_bad[b], s_bad[b + 1], tail * 32u);
    memmove(&s_bad_state[b], &s_bad_state[b + 1], tail);
    s_bad_n--;
}

/* Hides the loaded copies of a locale except the one with this SHA-256 (NULL: all of them). */
static void hide_slots(const char *locale, const uint8_t *keep)
{
    const uint32_t n = atomic_load_explicit(&s_count, memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) {
        slot_t *s = &s_slots[i];
        if (strcasecmp(s->pack.locale, locale) == 0 && (keep == NULL || memcmp(s->sha, keep, 32) != 0)) {
            atomic_store_explicit(&s->hidden, true, memory_order_relaxed);
        }
    }
}

/* The language's own checks on a verified file (pack_assets' validate callback): 0 (LANG_OK) to go
 * on, else the lang_result_t that refuses it. Writes the entry's name: the pack's locale. */
static int install_validate(const uint8_t *file, size_t len, uint32_t payload_length, char name[PS_NAME_MAX + 1],
                            void *ctx)
{
    (void)len;
    (void)ctx;
    if (payload_length < I18N_PACK_HEADER_SIZE) {
        return LANG_ERR_FORMAT;
    }
    i18n_pack_t pack;
    if (i18n_pack_open(file, payload_length, &pack) != I18N_PACK_OK) {
        return LANG_ERR_FORMAT;
    }
    if (pack.draft && release_build()) {
        return LANG_ERR_DRAFT;
    }
    if (is_english(pack.locale)) {
        return LANG_ERR_ENGLISH;
    }
    copy_str(name, PS_NAME_MAX + 1u, pack.locale);
    return LANG_OK;
}

/* After the table names the new pack (pack_assets' done callback: pa_lock and the update claim
 * held): the cache, and the screen when the panel speaks it. */
static void install_done(const ps_entry_t *ne, void *ctx)
{
    (void)ctx;
    char name[PS_NAME_MAX + 1];
    uint8_t sha[32];
    copy_str(name, sizeof(name), ne->name);
    memcpy(sha, ne->sha256, 32);
    const uint32_t len = ne->length;
    lock(s_lock);
    bad_forget(sha);
    unlock(s_lock);
    cache_rebuild();

    /* The panel speaks it, or an older copy is loaded: load the new one so the screen switches. */
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);
    if (strcasecmp(prefs.lang, name) == 0 || slot_find(name) != NULL) {
        const ps_entry_t *e = pa_find(PS_KIND_LANGUAGE, name);
        /* The old copy stays visible until the new one is registered (a failed load keeps it). */
        if (e != NULL && load_entry(e, true) == ESP_OK) {
            hide_slots(name, sha);
            bump_generation();
        }
    }
    ESP_LOGI(TAG, "%s installed (%u bytes)", name, (unsigned)len);
}

lang_result_t lang_packs_install(const uint8_t *file, size_t len)
{
    if (!s_mounted) {
        return LANG_ERR_OFF;
    }
    if (locked_now()) {
        return LANG_ERR_LOCKED;
    }
    if (file == NULL || len < 2u * LP_SECTOR || len > LP_MAX_FILE || (len % LP_SECTOR) != 0u) {
        return LANG_ERR_SIZE;
    }
    /* The whole transaction is pack_assets': the claim, the signature, the envelope, these checks,
     * room for the new copy beside the old one, write, read back, commit, then install_done. */
    const pa_install_t req = {.kind = PS_KIND_LANGUAGE,
                              .file = file,
                              .len = len,
                              .ff_padding = false,
                              .validate = install_validate,
                              .gate = NULL,
                              .done = install_done,
                              .ctx = NULL};
    int reject = LANG_ERR_FORMAT;
    switch (pa_install(&req, &reject)) {
    case PA_OK:
        return LANG_OK;
    case PA_ERR_OFF:
        return LANG_ERR_OFF;
    case PA_ERR_SIZE:
        return LANG_ERR_SIZE;
    case PA_ERR_BUSY:
        return LANG_ERR_BUSY;
    case PA_ERR_SIGNATURE:
        return LANG_ERR_SIGNATURE;
    case PA_ERR_FORMAT:
        return LANG_ERR_FORMAT;
    case PA_ERR_REJECTED:
        return (lang_result_t)reject;
    case PA_ERR_NO_SPACE:
        return LANG_ERR_NO_SPACE;
    case PA_ERR_CLOSED:
        return LANG_ERR_LOCKED;
    case PA_ERR_NOT_FOUND:
        return LANG_ERR_NOT_FOUND;
    case PA_ERR_FLASH:
    default:
        return LANG_ERR_FLASH;
    }
}

lang_result_t lang_packs_remove(const char *locale)
{
    if (!s_mounted) {
        return LANG_ERR_OFF;
    }
    if (locale == NULL || locale[0] == '\0') {
        return LANG_ERR_NOT_FOUND;
    }
    if (locked_now()) {
        return LANG_ERR_LOCKED;
    }
    pa_lock();
    char name[PS_NAME_MAX + 1];
    bool found = false;
    const uint16_t count = pa_count();
    for (uint16_t k = 0; k < count && !found; k++) {
        const ps_entry_t *e = pa_entry_at(k);
        if (e->kind == PS_KIND_LANGUAGE && strcasecmp(e->name, locale) == 0) {
            copy_str(name, sizeof(name), e->name);
            found = true;
        }
    }
    lang_result_t r = LANG_ERR_NOT_FOUND;
    if (found) {
        /* pa_remove looks after a failed commit, which may have landed: OK means it is gone. */
        r = pa_remove(PS_KIND_LANGUAGE, name) == PA_OK ? LANG_OK : LANG_ERR_FLASH;
        if (r == LANG_OK) {
            hide_slots(name, NULL);
            ESP_LOGI(TAG, "%s removed", name);
        }
        cache_rebuild();
    }
    pa_unlock();
    return r;
}

/* ---- checks for the console ---- */

/* The same check a load makes: the blob copied into PSRAM through the mapping, then pa_verify()
 * (the update claim held). */
static esp_err_t verify_flash(const ps_entry_t *e)
{
    if (e->length < 2u * LP_SECTOR || e->length > LP_MAX_FILE || (e->length % LP_SECTOR) != 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_aligned_alloc(4, e->length, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r = ESP_FAIL;
    if (pa_read(e, buf)) {
        r = pa_verify(buf, e->length, true);
    }
    heap_caps_free(buf);
    return r;
}

size_t lang_packs_verify_all(lang_verify_t *out, size_t max)
{
    if (!s_mounted || out == NULL) {
        return 0;
    }
    size_t n = 0;
    pa_lock();
    const bool claimed = panel_ota_claim(); /* not claimed: every answer is "not finished" */
    const uint16_t count = pa_count();
    for (uint16_t k = 0; k < count && n < max; k++) {
        const ps_entry_t *e = pa_entry_at(k);
        if (e->kind != PS_KIND_LANGUAGE) {
            continue;
        }
        copy_str(out[n].locale, sizeof(out[n].locale), e->name);
        out[n].err = claimed ? verify_flash(e) : ESP_ERR_NOT_FINISHED;
        n++;
    }
    if (claimed) {
        panel_ota_release();
    }
    pa_unlock();
    return n;
}

lang_result_t lang_packs_load_now(const char *locale)
{
    if (!s_mounted || locale == NULL) {
        return LANG_ERR_OFF;
    }
    lang_result_t r = LANG_ERR_NOT_FOUND;
    pa_lock();
    const uint16_t count = pa_count();
    for (uint16_t k = 0; k < count; k++) {
        const ps_entry_t *e = pa_entry_at(k);
        if (e->kind != PS_KIND_LANGUAGE || strcasecmp(e->name, locale) != 0) {
            continue;
        }
        lock(s_lock);
        for (size_t i = 0; i < s_head_n; i++) {
            if (memcmp(s_head[i].info.sha256, e->sha256, 32) == 0) {
                s_head[i].tried = false; /* a load that failed for want of room may be tried again */
            }
        }
        unlock(s_lock);
        switch (load_entry(e, false)) {
        case ESP_OK:
            r = LANG_OK;
            break;
        case ESP_ERR_NOT_FINISHED:
            r = LANG_ERR_BUSY;
            break;
        case ESP_ERR_NO_MEM:
            r = LANG_ERR_NO_SPACE;
            break;
        case ESP_ERR_NOT_SUPPORTED:
        case ESP_ERR_INVALID_STATE:
            r = LANG_ERR_SIGNATURE;
            break;
        case ESP_ERR_INVALID_SIZE:
        case ESP_ERR_INVALID_RESPONSE:
            r = LANG_ERR_FORMAT;
            break;
        case ESP_ERR_NOT_ALLOWED:
            r = LANG_ERR_DRAFT;
            break;
        default:
            r = LANG_ERR_FLASH;
            break;
        }
        break;
    }
    pa_unlock();
    return r;
}

/* ---- downloads (I18N-047) ---- */

/* An update is being downloaded or started (or, when `checking`, the release list is being read). */
static bool ota_busy(bool checking)
{
    panel_ota_status_t *st = psram_alloc(sizeof(*st));
    if (st == NULL) {
        return true;
    }
    panel_ota_get_status(st);
    const panel_ota_phase_t ph = st->phase;
    heap_caps_free(st);
    return ph == PANEL_OTA_RECEIVING || ph == PANEL_OTA_WAITING || ph == PANEL_OTA_RESTARTING ||
           (checking && ph == PANEL_OTA_CHECKING);
}

static proto_fw_language_t *source_list(size_t *n)
{
    proto_fw_language_t *l = psram_alloc(PANEL_OTA_LANGUAGES_MAX * sizeof(*l));
    *n = l != NULL ? panel_ota_languages(l, PANEL_OTA_LANGUAGES_MAX) : 0u;
    return l;
}

static const proto_fw_language_t *source_find(const proto_fw_language_t *l, size_t n, const char *locale)
{
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(l[i].locale, locale) == 0) {
            return &l[i];
        }
    }
    return NULL;
}

static void fetch_begin(const char *locale)
{
    lock(s_lock);
    copy_str(s_fetch.locale, sizeof(s_fetch.locale), locale);
    s_fetch.phase = 1;
    s_fetch.result = LANG_OK;
    unlock(s_lock);
    bump_generation();
}

static void fetch_finish(const char *locale, lang_result_t r)
{
    lock(s_lock);
    copy_str(s_fetch.locale, sizeof(s_fetch.locale), locale);
    s_fetch.phase = r == LANG_OK ? 2u : 3u;
    s_fetch.result = (uint8_t)r;
    unlock(s_lock);
    ESP_LOGI(TAG, "%s: download %s (%s)", locale, r == LANG_OK ? "done" : "failed", lang_result_name(r));
    bump_generation();
}

/* Service task: downloads a listed pack into PSRAM and installs it. */
static lang_result_t download_and_install(const proto_fw_language_t *l)
{
    if (l->size > LP_MAX_FILE) {
        return LANG_ERR_SIZE;
    }
    const size_t cap = l->size != 0u ? l->size : LP_MAX_FILE;
    uint8_t *buf = heap_caps_aligned_alloc(4, cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return LANG_ERR_NO_SPACE;
    }
    char why[64] = "";
    size_t got = 0;
    const esp_err_t e =
        panel_ota_download(l->url, l->size, l->has_sha256 ? l->sha256 : NULL, buf, cap, &got, why, sizeof(why));
    lang_result_t r;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s: download failed: %s (%s)", l->locale, why, esp_err_to_name(e));
        r = e == ESP_ERR_INVALID_SIZE ? LANG_ERR_SIZE : LANG_ERR_NO_SOURCE;
    } else {
        r = lang_packs_install(buf, got);
    }
    heap_caps_free(buf);
    return r;
}

/* One try for one listed language, with the status kept. */
static void fetch_one(const proto_fw_language_t *l)
{
    fetch_begin(l->locale);
    fetch_finish(l->locale, ota_busy(true) ? LANG_ERR_BUSY : download_and_install(l));
}

static void run_fetch_request(void)
{
    char loc[16];
    lock(s_lock);
    const bool queued = s_fetch_queued && s_fetch_ticket == 0u; /* else run_lookup()'s */
    if (queued) {
        memcpy(loc, s_fetch_req, sizeof(loc));
        s_fetch_queued = false;
    }
    unlock(s_lock);
    if (!queued) {
        return;
    }
    size_t n = 0;
    proto_fw_language_t *list = source_list(&n);
    const proto_fw_language_t *l = list != NULL ? source_find(list, n, loc) : NULL;
    if (l == NULL) {
        fetch_finish(loc, LANG_ERR_NO_SOURCE);
    } else {
        fetch_finish(loc, ota_busy(true) ? LANG_ERR_BUSY : download_and_install(l));
    }
    heap_caps_free(list);
}

/* A language added later (OTA-025): the update server's list for the version that runs,
 * since a check lists the packs of the release it offers. A pack is installed only once its own
 * signature checks out (OTA-016), whichever list named it. */
static void run_lookup(void)
{
    char loc[16];
    lock(s_lock);
    const uint32_t ticket = s_fetch_ticket;
    const TickType_t asked = s_fetch_asked;
    memcpy(loc, s_fetch_req, sizeof(loc));
    unlock(s_lock);
    if (ticket == 0u) {
        return;
    }
    proto_fw_language_t *got = psram_alloc(PANEL_OTA_LANGUAGES_MAX * sizeof(*got));
    size_t n = SIZE_MAX;
    if (got != NULL && !panel_ota_running_languages(ticket, got, PANEL_OTA_LANGUAGES_MAX, &n)) {
        if (xTaskGetTickCount() - asked < pdMS_TO_TICKS(LP_LOOKUP_MS)) {
            heap_caps_free(got);
            return; /* not answered yet: the task looks again soon */
        }
        n = SIZE_MAX;
        ESP_LOGW(TAG, "%s: no answer from the update server for this version's languages", loc);
    }
    lock(s_lock);
    s_fetch_ticket = 0;
    s_fetch_queued = false;
    unlock(s_lock);
    const proto_fw_language_t *l = n != SIZE_MAX ? source_find(got, n, loc) : NULL;
    proto_fw_language_t *list = NULL;
    if (l == NULL) {
        /* Not there (or a server without that list): what the last check offered, as before. */
        size_t m = 0;
        list = source_list(&m);
        l = list != NULL ? source_find(list, m, loc) : NULL;
    }
    if (l == NULL) {
        /* Nowhere yet: the next check asks for it too (OTA-025), and the pass after that check
         * fetches it if the server has it. */
        lock(s_lock);
        copy_str(s_want_extra, sizeof(s_want_extra), loc);
        unlock(s_lock);
        (void)panel_ota_check();
        fetch_finish(loc, LANG_ERR_NO_SOURCE);
    } else {
        fetch_finish(l->locale, ota_busy(true) ? LANG_ERR_BUSY : download_and_install(l));
    }
    heap_caps_free(list);
    heap_caps_free(got);
}

/* Adds a locale to the list a check sends, once (case-insensitive), English left out. */
static void want_add(char (*out)[PANEL_OTA_LOCALE_LEN], size_t max, size_t *n, const char *locale)
{
    if (locale == NULL || locale[0] == '\0' || is_english(locale) || *n >= max ||
        strlen(locale) >= PANEL_OTA_LOCALE_LEN) {
        return;
    }
    for (size_t i = 0; i < *n; i++) {
        if (strcasecmp(out[i], locale) == 0) {
            return;
        }
    }
    copy_str(out[*n], PANEL_OTA_LOCALE_LEN, locale);
    (*n)++;
}

/* The locales a check asks the update server for (OTA-018, OTA-025; panel_ota calls this on its
 * task): the panel's language, every installed pack's, and one asked for since the last check. */
static size_t wanted_locales(char (*out)[PANEL_OTA_LOCALE_LEN], size_t max)
{
    size_t n = 0;
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);
    want_add(out, max, &n, prefs.lang);
    lang_pack_info_t *heads = psram_alloc(LANG_PACKS_LIST_MAX * sizeof(*heads));
    if (heads != NULL) {
        const size_t nh = lang_packs_list(heads, LANG_PACKS_LIST_MAX);
        for (size_t i = 0; i < nh; i++) {
            want_add(out, max, &n, heads[i].locale);
        }
        heap_caps_free(heads);
    }
    char extra[sizeof(s_want_extra)];
    lock(s_lock);
    copy_str(extra, sizeof(extra), s_want_extra);
    unlock(s_lock);
    want_add(out, max, &n, extra);
    return n;
}

/* After a release check: the panel's language when it has no usable pack, and each installed pack
 * made for another version than the running one when the list has it at the running version.
 * Once per language per check; not while an update downloads or the panel is locked. */
static void run_auto_pass(void)
{
    if (!atomic_load_explicit(&s_auto_pending, memory_order_relaxed) || ota_busy(true)) {
        return; /* stays pending: the next status change looks again */
    }
    if (locked_now()) {
        return; /* stays pending: the task's wait times out and looks again */
    }
    if (!panel_ota_claim()) {
        return; /* the update machinery is busy: same */
    }
    panel_ota_release(); /* only a look: the install claims for itself */
    atomic_store_explicit(&s_auto_pending, false, memory_order_relaxed);
    size_t n = 0;
    proto_fw_language_t *list = source_list(&n);
    lang_pack_info_t *heads = psram_alloc(LANG_PACKS_LIST_MAX * sizeof(*heads));
    if (list == NULL || heads == NULL || n == 0u) {
        heap_caps_free(list);
        heap_caps_free(heads);
        return;
    }
    const size_t nh = lang_packs_list(heads, LANG_PACKS_LIST_MAX);
    const char *running = esp_app_get_description()->version;
    char rv[sizeof(heads[0].version)];
    copy_str(rv, sizeof(rv), running);
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);

    if (prefs.lang[0] != '\0' && !is_english(prefs.lang)) {
        bool have = false;
        for (size_t i = 0; i < nh; i++) {
            const lang_pack_info_t *h = &heads[i];
            have = have || (strcasecmp(h->locale, prefs.lang) == 0 &&
                            (h->state == LANG_PACK_READY || h->state == LANG_PACK_OTHER));
        }
        const proto_fw_language_t *l = have ? NULL : source_find(list, n, prefs.lang);
        if (l != NULL && !ota_busy(true)) {
            ESP_LOGI(TAG, "the panel's language %s has no pack: fetching it", l->locale);
            fetch_one(l);
        }
    }
    /* A language asked for that the previous list didn't have (lang_packs_fetch): the check that
     * followed sent it, so this list has it if the server does. */
    char extra[sizeof(s_want_extra)];
    lock(s_lock);
    copy_str(extra, sizeof(extra), s_want_extra);
    unlock(s_lock);
    if (extra[0] != '\0') {
        const proto_fw_language_t *l = source_find(list, n, extra);
        bool have = false;
        for (size_t i = 0; i < nh; i++) {
            have = have || (strcasecmp(heads[i].locale, extra) == 0 &&
                            (heads[i].state == LANG_PACK_READY || heads[i].state == LANG_PACK_OTHER));
        }
        if (l != NULL && !have && !ota_busy(true)) {
            ESP_LOGI(TAG, "%s, asked for before the server listed it: fetching it", l->locale);
            fetch_one(l);
        }
        if (l != NULL || have) {
            lock(s_lock);
            if (strcasecmp(s_want_extra, extra) == 0) {
                s_want_extra[0] = '\0';
            }
            unlock(s_lock);
        }
    }
    for (size_t i = 0; i < nh; i++) {
        const lang_pack_info_t *h = &heads[i];
        if ((h->state != LANG_PACK_READY && h->state != LANG_PACK_OTHER) || strcmp(h->version, rv) == 0) {
            continue;
        }
        const proto_fw_language_t *l = source_find(list, n, h->locale);
        if (l != NULL && strcmp(l->version, running) == 0 && !ota_busy(true)) {
            ESP_LOGI(TAG, "%s was made for %s, this is %s: fetching the matching pack", h->locale, h->version,
                     running);
            fetch_one(l);
        }
    }
    heap_caps_free(list);
    heap_caps_free(heads);
}

/* ---- asset packs (docs/features/39 asset-packs.md "Fetching", OPT-007) ---- */

static uint32_t asset_entry_hash(const proto_fw_asset_t *e)
{
    uint32_t h = 2166136261u; /* FNV-1a over the url, a separator and the version */
    for (const char *p = e->url; *p != '\0'; p++) {
        h = (h ^ (uint32_t)(unsigned char)*p) * 16777619u;
    }
    h = (h ^ 0xFFu) * 16777619u;
    for (const char *p = e->version; *p != '\0'; p++) {
        h = (h ^ (uint32_t)(unsigned char)*p) * 16777619u;
    }
    return h != 0u ? h : 1u;
}

/* Why the panel needs this kind's pack now, or NULL. *running_only: only an entry made for the
 * running version serves it (the letters' fingerprint and the certificates are the image's). */
static const char *asset_need(const asset_info_t *a, const char *rv, bool *running_only)
{
    /* none usable: none, damaged, or refused (a draft on a release image, a certificates pack made
     * for another version, a bundle TLS did not take) */
    const bool missing = a->state == ASSET_STATE_NONE || a->state == ASSET_STATE_DAMAGED ||
                         a->state == ASSET_STATE_REFUSED;
    const bool held = a->state == ASSET_STATE_READY;
    *running_only = true;
    switch (a->kind) {
    case PS_KIND_LETTERS: {
        if (missing) {
            return "none usable";
        }
        /* the image's fingerprint once the screen has asked with it; until then its version */
        const uint32_t fp = asset_packs_image_fingerprint();
        if (held && (fp != 0u ? a->match != fp : strcmp(a->version, rv) != 0)) {
            return "made for other fonts";
        }
        return NULL;
    }
    case PS_KIND_CERTS:
        if (missing) {
            return "none usable";
        }
        return held && strcmp(a->version, rv) != 0 ? "made for another version" : NULL;
    case PS_KIND_PICTURE:
        *running_only = false; /* the picture fits every version */
        return missing ? "none usable" : NULL;
    default:
        return NULL;
    }
}

/* The list's entry for a kind: the running version's first, else (when allowed) the first. */
static const proto_fw_asset_t *asset_find(const proto_fw_asset_t *l, size_t n, uint8_t kind,
                                          const char *running, bool running_only)
{
    const proto_fw_asset_t *any = NULL;
    for (size_t i = 0; i < n; i++) {
        if ((uint8_t)l[i].kind != kind) {
            continue;
        }
        if (strcmp(l[i].version, running) == 0) {
            return &l[i];
        }
        any = any != NULL ? any : &l[i];
    }
    return running_only ? NULL : any;
}

static uint32_t asset_max_file(uint8_t kind)
{
    return kind == PS_KIND_LETTERS   ? PA_LETTERS_MAX_FILE
           : kind == PS_KIND_PICTURE ? PA_PICTURE_MAX_FILE
                                     : PA_CERTS_MAX_FILE;
}

/* This task (an internal-RAM stack): downloads a listed asset pack into PSRAM and installs it
 * through asset_packs_install (the signature, the format, the kind's rules, the lock, the claim).
 * "download" when it could not be fetched (logged with the reason), else the install's result. */
static const char *asset_download_and_install(const proto_fw_asset_t *e)
{
    const uint32_t max = asset_max_file((uint8_t)e->kind);
    if (e->size > max) {
        return asset_result_name(ASSET_ERR_SIZE);
    }
    const size_t cap = e->size != 0u ? e->size : max;
    uint8_t *buf = heap_caps_aligned_alloc(4, cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return "no_memory";
    }
    char why[64] = "";
    size_t got = 0;
    const esp_err_t err =
        panel_ota_download(e->url, e->size, e->has_sha256 ? e->sha256 : NULL, buf, cap, &got, why, sizeof(why));
    const char *r = "download";
    uint8_t kind = 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "asset %s: download failed: %s (%s)", asset_kind_name((uint8_t)e->kind), why,
                 esp_err_to_name(err));
    } else {
        const asset_result_t ir = asset_packs_install(buf, got, &kind);
        r = asset_result_name(ir);
        if (ir == ASSET_OK && kind != (uint8_t)e->kind) {
            ESP_LOGW(TAG, "asset %s: the list's file was a %s pack", asset_kind_name((uint8_t)e->kind),
                     asset_kind_name(kind));
        }
    }
    heap_caps_free(buf);
    return r;
}

/* After a release check (OPT-007): for each kind the panel uses, its pack when it has none usable;
 * the letters when made for other fonts than the image's; the certificates when made for another
 * version. From the check's list, else the running version's (asked of the update server once per
 * start, once it answered). One try per kind per check; not while an update downloads or the panel
 * is locked. */
static void run_asset_pass(void)
{
    if (!atomic_load_explicit(&s_asset_pending, memory_order_relaxed) || ota_busy(true)) {
        return; /* stays pending: the next status change looks again */
    }
    if (locked_now()) {
        return; /* stays pending: the task's wait times out and looks again */
    }
    if (!panel_ota_claim()) {
        return; /* the update machinery is busy: same */
    }
    panel_ota_release(); /* only a look: the install claims for itself */
    atomic_store_explicit(&s_asset_pending, false, memory_order_relaxed);

    asset_info_t *info = psram_alloc(ASSET_PACKS_KINDS * sizeof(*info));
    proto_fw_asset_t *list = psram_alloc(PANEL_OTA_ASSETS_MAX * sizeof(*list));
    if (info == NULL || list == NULL) {
        heap_caps_free(info);
        heap_caps_free(list);
        return;
    }
    const char *running = esp_app_get_description()->version;
    char rv[sizeof(info[0].version)];
    copy_str(rv, sizeof(rv), running);
    const size_t ni = asset_packs_list(info, ASSET_PACKS_KINDS);
    const size_t n = panel_ota_assets(list, PANEL_OTA_ASSETS_MAX);

    const char *why[ASSET_PACKS_KINDS] = {NULL};
    bool running_only[ASSET_PACKS_KINDS] = {false};
    const proto_fw_asset_t *pick[ASSET_PACKS_KINDS] = {NULL};
    bool ask = false;
    for (size_t i = 0; i < ni; i++) {
        why[i] = asset_need(&info[i], rv, &running_only[i]);
        if (why[i] != NULL) {
            pick[i] = asset_find(list, n, info[i].kind, running, running_only[i]);
            ask = ask || pick[i] == NULL;
        }
    }
    if (ask && s_asset_rn == SIZE_MAX) {
        /* the check lists the release it offers: the running version's packs are asked for, once
         * per start once answered (that list does not change for a released version, and a server
         * without it would otherwise be asked at every check, holding this task) */
        if (s_asset_rlist == NULL) {
            s_asset_rlist = psram_alloc(PANEL_OTA_ASSETS_MAX * sizeof(*s_asset_rlist));
        }
        size_t rn = SIZE_MAX;
        const uint32_t ticket = s_asset_rlist != NULL ? panel_ota_running_languages_ask() : 0u;
        const TickType_t t0 = xTaskGetTickCount();
        while (ticket != 0u && !panel_ota_running_assets(ticket, s_asset_rlist, PANEL_OTA_ASSETS_MAX, &rn)) {
            if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(LP_ASSET_LOOKUP_MS)) {
                rn = SIZE_MAX;
                ESP_LOGW(TAG, "asset packs: no answer from the update server for this version's list");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(LP_LOOKUP_POLL_MS));
        }
        s_asset_rn = rn;
    }
    for (size_t i = 0; i < ni && ask && s_asset_rn != SIZE_MAX; i++) {
        if (why[i] != NULL && pick[i] == NULL) {
            pick[i] = asset_find(s_asset_rlist, s_asset_rn, info[i].kind, running, running_only[i]);
        }
    }
    for (size_t i = 0; i < ni; i++) {
        const char *name = asset_kind_name(info[i].kind);
        if (why[i] == NULL) {
            continue;
        }
        if (pick[i] == NULL) {
            ESP_LOGI(TAG, "asset %s: %s, and no release list has one for %s", name, why[i], running);
            continue;
        }
        const uint32_t h = asset_entry_hash(pick[i]);
        if (s_asset_done[i] == h) {
            continue; /* installed by this pass already: fetching it again would change nothing */
        }
        if (ota_busy(true) || locked_now()) {
            ESP_LOGI(TAG, "asset %s: held off (an update or the panel's lock): the next check tries again", name);
            continue;
        }
        ESP_LOGI(TAG, "asset %s: %s: fetching %s", name, why[i], pick[i]->version);
        const char *r = asset_download_and_install(pick[i]);
        const bool ok = strcmp(r, asset_result_name(ASSET_OK)) == 0;
        ESP_LOGI(TAG, "asset %s: download %s (%s)", name, ok ? "done" : "failed", r);
        if (ok) {
            s_asset_done[i] = h;
        }
    }
    heap_caps_free(info);
    heap_caps_free(list);
}

/* The release check finished (panel_ota's update task calls this): look at the new list soon. */
static void ota_changed(void *ctx)
{
    (void)ctx;
    const uint32_t q = panel_ota_languages_seq();
    if (q != s_auto_seen) {
        s_auto_seen = q;
        atomic_store_explicit(&s_auto_pending, true, memory_order_relaxed);
        atomic_store_explicit(&s_asset_pending, true, memory_order_relaxed);
    }
    if ((atomic_load_explicit(&s_auto_pending, memory_order_relaxed) ||
         atomic_load_explicit(&s_asset_pending, memory_order_relaxed)) &&
        s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}

static void lang_task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        lock(s_lock);
        const bool looking = s_fetch_ticket != 0u;
        unlock(s_lock);
        if (looking) {
            wait = pdMS_TO_TICKS(LP_LOOKUP_POLL_MS);
        } else if (atomic_load_explicit(&s_retry, memory_order_relaxed)) {
            wait = pdMS_TO_TICKS(LP_RETRY_MS);
        } else if (atomic_load_explicit(&s_auto_pending, memory_order_relaxed) ||
                   atomic_load_explicit(&s_asset_pending, memory_order_relaxed)) {
            wait = pdMS_TO_TICKS(LP_AUTO_MS);
        }
        (void)ulTaskNotifyTake(pdTRUE, wait);
        atomic_store_explicit(&s_retry, false, memory_order_relaxed);
        for (;;) {
            char name[16];
            lock(s_lock);
            const bool have = s_pending_n > 0;
            if (have) {
                memcpy(name, s_pending[0], sizeof(name));
                memmove(s_pending[0], s_pending[1], (LP_PENDING - 1u) * sizeof(s_pending[0]));
                s_pending_n--;
            }
            unlock(s_lock);
            if (!have) {
                break;
            }
            if (!load_named(name)) {
                break; /* held off: queued again, s_retry set */
            }
        }
        lock(s_lock);
        void (*job)(void *) = s_job_fn;
        void *job_arg = s_job_arg;
        s_job_fn = NULL;
        unlock(s_lock);
        if (job != NULL) {
            job(job_arg);
            (void)xSemaphoreGive(s_job_done);
        }
        run_fetch_request();
        run_lookup();
        run_auto_pass();
        run_asset_pass();
    }
}

uint32_t lang_packs_task_stack_free(void)
{
    return s_task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) * (uint32_t)sizeof(StackType_t) : 0u;
}

bool lang_packs_run_on_task(void (*fn)(void *), void *arg)
{
    if (s_task == NULL || s_job_mu == NULL || s_job_done == NULL || fn == NULL) {
        return false;
    }
    lock(s_job_mu);
    lock(s_lock);
    s_job_fn = fn;
    s_job_arg = arg;
    unlock(s_lock);
    (void)xTaskNotifyGive(s_task);
    lock(s_job_done);
    unlock(s_job_mu);
    return true;
}

/* ---- the public API ---- */

esp_err_t lang_packs_start(void)
{
    if (s_lock != NULL) {
        return ESP_OK; /* started */
    }
    /* The store is pack_assets' (shared with voice packs): mounted once, by whichever starts first. */
    const pa_result_t m = pa_init();
    if (m == PA_ERR_OFF) {
        ESP_LOGW(TAG, "no pack store (no assets partition, or not a store): English and the built-in catalogs only");
        return ESP_OK;
    }
    SemaphoreHandle_t lk = xSemaphoreCreateMutex();
    if (lk == NULL) {
        ESP_LOGE(TAG, "out of memory: English only");
        return ESP_OK;
    }
    /* Readers look at s_lock to tell whether the service runs. The first cache_rebuild() below takes
     * it, so it is set here, before the cache exists: start-up stays single-threaded (no service task,
     * s_mounted false) until the cache is built. */
    s_lock = lk;

    if (m != PA_OK) {
        ESP_LOGW(TAG, "the pack store could not be read (%s): English only", pa_result_name(m));
        return ESP_OK;
    }
    s_mounted = true;
    panel_ota_set_locales(wanted_locales); /* a check asks for the packs this panel wants */
    pa_lock();
    cache_rebuild();
    pa_unlock();
    ESP_LOGI(TAG, "%u language pack(s) installed", (unsigned)s_head_n);

    /* A new panel (no language stored) starts in the store's default language. */
    if (!panel_prefs_lang_stored()) {
        pa_lock(); /* the entry pointer below */
        for (size_t i = 0; i < s_head_n; i++) {
            const ps_entry_t *e = pa_find(PS_KIND_LANGUAGE, s_head[i].info.locale);
            if (e != NULL && (e->flags & PS_FLAG_DEFAULT) != 0u && usable(&s_head[i])) {
                panel_prefs_t p;
                panel_prefs_load(&p);
                copy_str(p.lang, sizeof(p.lang), e->name);
                const esp_err_t saved = panel_prefs_save(&p);
                ESP_LOGI(TAG, "no language stored: adopted the default, %s (%s)", e->name, esp_err_to_name(saved));
                break;
            }
        }
        pa_unlock();
    }
    /* The stored language, synchronously, so the screen starts in it. */
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);
    const head_t *stored = head_find(prefs.lang);
    if (stored != NULL && usable(stored)) {
        char name[16];
        copy_str(name, sizeof(name), stored->info.locale);
        load_named(name);
    } else {
        ESP_LOGI(TAG, "the stored language %s has no usable pack: English until one is installed", prefs.lang);
    }
    s_job_mu = xSemaphoreCreateMutex();
    s_job_done = xSemaphoreCreateBinary();
    if (xTaskCreate(lang_task, "lang_packs", LP_TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "no task for later loads: a pack installed now is used from the next restart");
    } else if (panel_ota_add_listener(ota_changed, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "no listener slot: languages will not follow an update by themselves");
    } else {
        ota_changed(NULL); /* a release check may have finished already */
    }
    return ESP_OK;
}

lang_result_t lang_packs_fetch(const char *locale)
{
    if (!s_mounted || s_task == NULL) {
        return LANG_ERR_OFF;
    }
    if (locale == NULL || locale[0] == '\0' || strlen(locale) >= sizeof(s_fetch_req)) {
        return LANG_ERR_NO_SOURCE;
    }
    if (is_english(locale)) {
        return LANG_ERR_ENGLISH;
    }
    size_t n = 0;
    proto_fw_language_t *list = source_list(&n);
    const proto_fw_language_t *l = list != NULL ? source_find(list, n, locale) : NULL;
    char exact[16];
    copy_str(exact, sizeof(exact), l != NULL ? l->locale : locale);
    /* The last check lists the packs of the release it offers: this version's when the
     * panel is up to date, a newer one's while an update is offered, and only the languages the
     * panel asked for. Anything else comes from the server's list for this version. */
    const esp_app_desc_t *me = esp_app_get_description();
    char running[sizeof(me->version) + 1u];
    snprintf(running, sizeof(running), "%.*s", (int)strnlen(me->version, sizeof(me->version)), me->version);
    const bool lookup = l == NULL || strcmp(l->version, running) != 0;
    const bool listed = l != NULL;
    heap_caps_free(list);
    if (ota_busy(false)) {
        return LANG_ERR_BUSY;
    }
    if (locked_now()) {
        return LANG_ERR_LOCKED;
    }
    lock(s_lock);
    if (s_fetch.phase == 1u || s_fetch_queued) {
        unlock(s_lock);
        return LANG_ERR_BUSY;
    }
    uint32_t ticket = 0;
    if (lookup) {
        ticket = panel_ota_running_languages_ask(); /* takes only panel_ota's own lock */
        if (ticket == 0u && !listed) {
            unlock(s_lock);
            return LANG_ERR_NO_SOURCE; /* no update server: nowhere to ask */
        }
    }
    s_fetch_ticket = ticket; /* 0: the check's entry, at once */
    s_fetch_asked = xTaskGetTickCount();
    copy_str(s_fetch_req, sizeof(s_fetch_req), exact);
    s_fetch_queued = true;
    copy_str(s_fetch.locale, sizeof(s_fetch.locale), exact);
    s_fetch.phase = 1;
    s_fetch.result = LANG_OK;
    unlock(s_lock);
    (void)xTaskNotifyGive(s_task);
    return LANG_OK;
}

void lang_packs_fetch_status(lang_fetch_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    lock(s_lock);
    *out = s_fetch;
    unlock(s_lock);
}

uint32_t lang_packs_free_bytes(void)
{
    return pa_free_bytes();
}

size_t lang_packs_available(lang_available_t *out, size_t max)
{
    if (out == NULL || max == 0u) {
        return 0;
    }
    size_t n = 0;
    proto_fw_language_t *list = source_list(&n);
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        copy_str(out[i].locale, sizeof(out[i].locale), list[i].locale);
        copy_str(out[i].version, sizeof(out[i].version), list[i].version);
        out[i].size = list[i].size;
    }
    heap_caps_free(list);
    return n;
}

const i18n_catalog_t *lang_packs_catalog(const char *locale)
{
    if (locale == NULL) {
        return NULL;
    }
    const slot_t *s = slot_find(locale);
    if (s != NULL) {
        return &s->pack.catalog;
    }
    const i18n_catalog_t *c = i18n_catalog(locale);
    if (c != NULL || s_lock == NULL || s_task == NULL) {
        return c;
    }
    /* Installed, usable, not loaded: queue the load (once) and answer with what there is. */
    bool wake = false;
    lock(s_lock);
    head_t *h = head_find(locale);
    if (h != NULL && usable(h) && !h->tried &&
        atomic_load_explicit(&s_count, memory_order_relaxed) < LANG_PACKS_MAX_LOADED) {
        bool queued = false;
        for (size_t i = 0; i < s_pending_n; i++) {
            if (strcmp(s_pending[i], h->info.locale) == 0) {
                queued = true;
            }
        }
        if (!queued && pending_add_locked(h->info.locale)) {
            wake = true;
        }
    }
    unlock(s_lock);
    if (wake) {
        (void)xTaskNotifyGive(s_task);
    }
    return NULL;
}

bool lang_packs_font(const char *locale, const char *role, const void **data, size_t *size)
{
    if (locale == NULL || role == NULL || data == NULL || size == NULL) {
        return false;
    }
    const slot_t *s = slot_find(locale);
    if (s == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < s->pack.font_count && i < I18N_PACK_MAX_FONTS; i++) {
        const i18n_pack_font_t *f = &s->pack.fonts[i];
        if (strncmp(f->role, role, 16u) == 0) {
            *data = f->data;
            *size = f->size;
            return true;
        }
    }
    return false;
}

size_t lang_packs_list(lang_pack_info_t *out, size_t max)
{
    if (out == NULL || s_lock == NULL) {
        return 0;
    }
    lock(s_lock);
    size_t n = s_head_n < max ? s_head_n : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_head[i].info;
        out[i].loaded = slot_loaded(out[i].sha256);
    }
    unlock(s_lock);
    return n;
}

uint32_t lang_packs_generation(void)
{
    return atomic_load_explicit(&s_generation, memory_order_relaxed);
}

bool lang_packs_known(const char *locale)
{
    if (locale == NULL) {
        return false;
    }
    if (i18n_language_known(locale)) {
        return true;
    }
    if (s_lock == NULL) {
        return false;
    }
    lock(s_lock);
    const head_t *h = head_find(locale);
    const bool known = h != NULL && usable(h);
    unlock(s_lock);
    return known;
}

#else /* !CONFIG_ALARM_FEATURE_LANG_PACKS: English and the built-in catalogs (docs/features/33 step 6) */

esp_err_t lang_packs_start(void)
{
    return ESP_OK;
}

const i18n_catalog_t *lang_packs_catalog(const char *locale)
{
    return i18n_catalog(locale);
}

size_t lang_packs_list(lang_pack_info_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

uint32_t lang_packs_generation(void)
{
    return 0;
}

bool lang_packs_known(const char *locale)
{
    return locale != NULL && i18n_language_known(locale);
}

bool lang_packs_font(const char *locale, const char *role, const void **data, size_t *size)
{
    (void)locale;
    (void)role;
    (void)data;
    (void)size;
    return false;
}

lang_result_t lang_packs_install(const uint8_t *file, size_t len)
{
    (void)file;
    (void)len;
    return LANG_ERR_OFF;
}

lang_result_t lang_packs_remove(const char *locale)
{
    (void)locale;
    return LANG_ERR_OFF;
}

lang_result_t lang_packs_fetch(const char *locale)
{
    (void)locale;
    return LANG_ERR_OFF;
}

void lang_packs_fetch_status(lang_fetch_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

uint32_t lang_packs_free_bytes(void)
{
    return 0;
}

size_t lang_packs_available(lang_available_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

#endif /* CONFIG_ALARM_FEATURE_LANG_PACKS */
