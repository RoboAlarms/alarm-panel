/*
 * pack_assets.c - the one owner of the `assets` pack store, for language and voice packs
 * (pack_assets.h; docs/features/08 voice-first-build.md "Installing a voice pack", AUD-027).
 *
 * The portable store (components/core/pack_store) does the table, the free-space admission and
 * the install transaction's order (ps_put_blob: write, read back, then commit); this file gives it
 * the partition and adds what is ESP-IDF's: the mutex, the flash mapping, the signature check and
 * the update machinery's claim.
 */
#include "pack_assets.h"

#include "sdkconfig.h"

const char *pa_result_name(pa_result_t r)
{
    static const char *const names[] = {"ok",       "off",      "size",   "busy",  "signature", "format",
                                        "rejected", "no_space", "closed", "flash", "not_found"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "unknown";
}

#if defined(CONFIG_ALARM_FEATURE_LANG_PACKS) || defined(CONFIG_ALARM_FEATURE_VOICE)

#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "panel_ota.h"
#include "psa/crypto.h"

/* The signature check exists only in a firmware signed apps are checked by (Secure Boot V2, or
 * signed apps without it); any other firmware refuses every pack. */
#if CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT || CONFIG_SECURE_BOOT_V2_ENABLED
#include "esp_secure_boot.h"
#define PA_CAN_VERIFY 1
#else
#define PA_CAN_VERIFY 0
#endif

#define PA_CHUNK (256u * 1024u)

static const char *TAG = "pack_assets";

static const esp_partition_t *s_part;
static EXT_RAM_BSS_ATTR ps_store_t s_store; /* the table and one sector buffer: task-only, off the internal RAM */
static SemaphoreHandle_t s_mu;              /* recursive: the store, and the flash work of its users */
static bool s_tried;
static pa_result_t s_init;
static _Atomic bool s_ready;
static _Atomic uint32_t s_free_bytes;
static _Atomic uint32_t s_generation;

/* ---- the flash port of the pack store ---- */

static bool fl_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, offset, buf, len) == ESP_OK;
}

static bool fl_erase(void *ctx, uint32_t offset, uint32_t len)
{
    (void)ctx;
    return esp_partition_erase_range(s_part, offset, len) == ESP_OK;
}

static bool fl_write(void *ctx, uint32_t offset, const void *buf, size_t len)
{
    (void)ctx;
    return esp_partition_write(s_part, offset, buf, len) == ESP_OK;
}

/* After a mount or a commit (mutex held). */
static void refresh(void)
{
    atomic_store_explicit(&s_free_bytes, ps_free_sectors(&s_store) * PS_SECTOR, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_generation, 1u, memory_order_relaxed);
}

pa_result_t pa_init(void)
{
    if (s_tried) {
        return s_init;
    }
    s_tried = true;
    s_init = PA_ERR_OFF;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no assets partition");
        return s_init;
    }
    const uint32_t sectors = s_part->size / PS_SECTOR;
    if (sectors < 3u || sectors > 0xFFFFFu) {
        ESP_LOGW(TAG, "the assets partition (%u bytes) is not a pack store", (unsigned)s_part->size);
        return s_init;
    }
    s_mu = xSemaphoreCreateRecursiveMutex();
    if (s_mu == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return s_init;
    }
    const ps_flash_t flash = {.ctx = NULL, .sectors = sectors, .read = fl_read, .erase = fl_erase, .write = fl_write};
    pa_lock();
    const ps_result_t m = ps_mount(&s_store, &flash);
    if (m == PS_OK) {
        refresh();
        ESP_LOGI(TAG, "pack store mounted: sequence %u, %u entries, %u free sectors", (unsigned)s_store.sequence,
                 (unsigned)s_store.count, (unsigned)ps_free_sectors(&s_store));
    }
    pa_unlock();
    if (m != PS_OK) {
        ESP_LOGW(TAG, "the pack store could not be read (%d)", (int)m);
        s_init = PA_ERR_FLASH;
        return s_init;
    }
    s_init = PA_OK;
    atomic_store_explicit(&s_ready, true, memory_order_release);
    return s_init;
}

bool pa_ready(void)
{
    return atomic_load_explicit(&s_ready, memory_order_acquire);
}

void pa_lock(void)
{
    if (s_mu != NULL) {
        (void)xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    }
}

void pa_unlock(void)
{
    if (s_mu != NULL) {
        (void)xSemaphoreGiveRecursive(s_mu);
    }
}

const ps_entry_t *pa_find(uint8_t kind, const char *name)
{
    return pa_ready() ? ps_find(&s_store, kind, name) : NULL;
}

uint16_t pa_count(void)
{
    return pa_ready() ? s_store.count : 0u;
}

const ps_entry_t *pa_entry_at(uint16_t i)
{
    return pa_ready() && i < s_store.count ? &s_store.entries[i] : NULL;
}

/* ---- reading ---- */

bool pa_read(const ps_entry_t *e, void *dst)
{
    if (!pa_ready() || e == NULL || dst == NULL) {
        return false;
    }
    uint8_t *out = dst;
    const uint32_t base = e->first_sector * PS_SECTOR;
    for (uint32_t off = 0; off < e->length; off += PA_CHUNK) {
        uint32_t n = e->length - off;
        if (n > PA_CHUNK) {
            n = PA_CHUNK;
        }
        const void *p = NULL;
        esp_partition_mmap_handle_t handle;
        if (esp_partition_mmap(s_part, base + off, n, ESP_PARTITION_MMAP_DATA, &p, &handle) != ESP_OK) {
            return false;
        }
        memcpy(out + off, p, n);
        esp_partition_munmap(handle);
    }
    return true;
}

bool pa_view(const ps_entry_t *e, uint32_t len, void (*fn)(const void *p, size_t len, void *ctx), void *ctx)
{
    if (!pa_ready() || e == NULL || fn == NULL || len == 0u || len > e->length) {
        return false;
    }
    const void *p = NULL;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(s_part, e->first_sector * PS_SECTOR, len, ESP_PARTITION_MMAP_DATA, &p, &handle) !=
        ESP_OK) {
        return false;
    }
    fn(p, len, ctx);
    esp_partition_munmap(handle);
    return true;
}

/* ---- the signature ---- */

static esp_err_t check_signature(const uint8_t *buf, uint32_t len)
{
#if PA_CAN_VERIFY
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    uint8_t digest[32];
    size_t digest_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, buf, len - PS_SECTOR, digest, sizeof(digest), &digest_len) !=
            PSA_SUCCESS ||
        digest_len != sizeof(digest)) {
        return ESP_FAIL;
    }
    const void *block = buf + (len - PS_SECTOR);
    return esp_secure_boot_verify_sbv2_signature_block((const ets_secure_boot_signature_t *)block, digest, NULL);
#else
    (void)buf;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t pa_verify(const uint8_t *buf, uint32_t len, bool claimed)
{
    if (buf == NULL || len < 2u * PS_SECTOR || (len % PS_SECTOR) != 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!claimed && !panel_ota_claim()) {
        return ESP_ERR_NOT_FINISHED;
    }
    const esp_err_t r = check_signature(buf, len);
    if (!claimed) {
        panel_ota_release();
    }
    return r;
}

/* ---- installing and removing ---- */

static uint32_t max_file(uint8_t kind)
{
    switch (kind) {
    case PS_KIND_LANGUAGE:
        return PA_LANGUAGE_MAX_FILE;
    case PS_KIND_VOICE:
        return PA_VOICE_MAX_FILE;
    case PS_KIND_LETTERS:
        return PA_LETTERS_MAX_FILE;
    case PS_KIND_PICTURE:
        return PA_PICTURE_MAX_FILE;
    case PS_KIND_CERTS:
        return PA_CERTS_MAX_FILE;
    default:
        return 0u;
    }
}

static bool sha256_of(const uint8_t *p, size_t len, uint8_t out[32])
{
    size_t n = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, p, len, out, 32, &n) == PSA_SUCCESS && n == 32u;
}

/* Between the update claim and its release. */
static pa_result_t install_claimed(const pa_install_t *req, int *reject)
{
    const esp_err_t sig = check_signature(req->file, (uint32_t)req->len);
    if (sig != ESP_OK) {
        /* only these two mean "not signed for this panel"; the check failing to run is a flash-side fault */
        return sig == ESP_ERR_IMAGE_INVALID || sig == ESP_ERR_NOT_SUPPORTED ? PA_ERR_SIGNATURE : PA_ERR_FLASH;
    }
    ps_entry_t e;
    memset(&e, 0, sizeof(e));
    if (ps_envelope(req->file, req->len, req->ff_padding, &e.payload_length) != PS_OK) {
        return PA_ERR_FORMAT;
    }
    if (req->validate != NULL) {
        const int v = req->validate(req->file, req->len, e.payload_length, e.name, req->ctx);
        if (v != 0) {
            if (reject != NULL) {
                *reject = v;
            }
            return PA_ERR_REJECTED;
        }
        e.name[PS_NAME_MAX] = '\0';
    }
    if (!sha256_of(req->file, req->len, e.sha256)) {
        return PA_ERR_FLASH;
    }
    e.kind = req->kind;
    pa_lock();
    const ps_result_t r = ps_put_blob(&s_store, &e, req->file, req->len, req->gate, req->ctx);
    refresh(); /* also after a failure: a failed commit re-reads the flash, which may hold either table */
    if (r == PS_OK && req->done != NULL) {
        const ps_entry_t *now = ps_find(&s_store, e.kind, e.name);
        if (now != NULL) {
            req->done(now, req->ctx);
        }
    }
    pa_unlock();
    switch (r) {
    case PS_OK:
        ESP_LOGI(TAG, "kind %u %s stored (%u bytes)", (unsigned)e.kind, e.name, (unsigned)req->len);
        return PA_OK;
    case PS_ERR_NO_SPACE:
    case PS_ERR_FULL:
        return PA_ERR_NO_SPACE; /* the working copy stays until the new one is in: room for both */
    case PS_ERR_CLOSED:
        return PA_ERR_CLOSED;
    default:
        ESP_LOGW(TAG, "kind %u %s: not stored (%d): the working copy stays", (unsigned)e.kind, e.name, (int)r);
        return PA_ERR_FLASH;
    }
}

pa_result_t pa_install(const pa_install_t *req, int *reject)
{
    if (!pa_ready()) {
        return PA_ERR_OFF;
    }
    if (req == NULL || req->file == NULL || req->len < PA_MIN_FILE || req->len > max_file(req->kind) ||
        (req->len % PS_SECTOR) != 0u) {
        return PA_ERR_SIZE;
    }
    if (!panel_ota_claim()) {
        return PA_ERR_BUSY; /* an update receives, waits or restarts */
    }
    const pa_result_t r = install_claimed(req, reject);
    panel_ota_release();
    return r;
}

pa_result_t pa_remove(uint8_t kind, const char *name)
{
    if (!pa_ready()) {
        return PA_ERR_OFF;
    }
    if (name == NULL) {
        return PA_ERR_NOT_FOUND;
    }
    pa_lock();
    pa_result_t r = PA_ERR_NOT_FOUND;
    if (ps_find(&s_store, kind, name) != NULL) {
        const ps_result_t c = ps_commit_remove(&s_store, kind, name);
        /* A failed commit may have landed (the store re-read the flash): look, don't assume. */
        r = ps_find(&s_store, kind, name) == NULL ? PA_OK : PA_ERR_FLASH;
        if (r == PA_OK && c != PS_OK) {
            ESP_LOGW(TAG, "kind %u %s: the commit reported %d but landed", (unsigned)kind, name, (int)c);
        }
        refresh();
    }
    pa_unlock();
    return r;
}

uint32_t pa_free_bytes(void)
{
    return atomic_load_explicit(&s_free_bytes, memory_order_relaxed);
}

uint32_t pa_generation(void)
{
    return atomic_load_explicit(&s_generation, memory_order_relaxed);
}

#else /* neither language packs nor voice: no store */

pa_result_t pa_init(void)
{
    return PA_ERR_OFF;
}

bool pa_ready(void)
{
    return false;
}

void pa_lock(void) {}

void pa_unlock(void) {}

const ps_entry_t *pa_find(uint8_t kind, const char *name)
{
    (void)kind;
    (void)name;
    return NULL;
}

uint16_t pa_count(void)
{
    return 0u;
}

const ps_entry_t *pa_entry_at(uint16_t i)
{
    (void)i;
    return NULL;
}

bool pa_read(const ps_entry_t *e, void *dst)
{
    (void)e;
    (void)dst;
    return false;
}

bool pa_view(const ps_entry_t *e, uint32_t len, void (*fn)(const void *p, size_t len, void *ctx), void *ctx)
{
    (void)e;
    (void)len;
    (void)fn;
    (void)ctx;
    return false;
}

esp_err_t pa_verify(const uint8_t *buf, uint32_t len, bool claimed)
{
    (void)buf;
    (void)len;
    (void)claimed;
    return ESP_ERR_NOT_SUPPORTED;
}

pa_result_t pa_install(const pa_install_t *req, int *reject)
{
    (void)req;
    (void)reject;
    return PA_ERR_OFF;
}

pa_result_t pa_remove(uint8_t kind, const char *name)
{
    (void)kind;
    (void)name;
    return PA_ERR_OFF;
}

uint32_t pa_free_bytes(void)
{
    return 0u;
}

uint32_t pa_generation(void)
{
    return 0u;
}

#endif
