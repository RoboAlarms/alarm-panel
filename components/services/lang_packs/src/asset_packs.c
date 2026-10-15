/*
 * asset_packs.c - the asset packs on the panel (docs/features/39 asset-packs.md "The panel",
 * OPT-006): letters, picture and certificates, loaded at start from the pack store (pack_assets),
 * checked, kept in PSRAM until restart and offered to the screen. See asset_packs.h.
 *
 * Each kind keeps up to ASSET_PACKS_MAX_LOADS loaded copies (buffer and opened pack, in PSRAM),
 * filled before they are published and never changed or freed after: the offered copy is an
 * index in internal RAM, stored with release order and read with acquire, so readers take no lock.
 * The states (asset_info_t) are behind a small mutex.
 */
#include "asset_packs.h"

#include <string.h>

#include "sdkconfig.h"

const char *asset_state_name(asset_state_t s)
{
    static const char *const names[] = {"none", "ready", "refused", "damaged", "needs_restart", "unchecked"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "unknown";
}

const char *asset_result_name(asset_result_t r)
{
    static const char *const names[] = {"ok",    "off",    "size",   "signature", "format",  "kind",
                                        "draft", "locked", "busy",   "no_space",  "flash"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "unknown";
}

const char *asset_kind_name(uint8_t kind)
{
    switch (kind) {
    case 3u: /* PS_KIND_LETTERS */
        return "letters";
    case 4u: /* PS_KIND_PICTURE */
        return "picture";
    case 5u: /* PS_KIND_CERTS */
        return "certificates";
    default:
        return "unknown";
    }
}

#if CONFIG_ALARM_FEATURE_LANG_PACKS

#include <stdatomic.h>
#include <stdio.h>

#include "alarm/alarm_snapshot.h"
#include "alarm_service.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_heap_caps.h"
#include "esp_image_format.h" /* ESP_ERR_IMAGE_INVALID: pa_verify's "bad signature" */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pack_assets.h"
#include "pack_store/asset_pack.h"
#include "pack_store/pack_store.h"

static const char *TAG = "asset_packs";

#define AP_SECTOR 4096u

_Static_assert(PS_KIND_LETTERS == 3 && PS_KIND_PICTURE == 4 && PS_KIND_CERTS == 5, "the kinds asset_kind_name names");

typedef struct {
    uint8_t *buf;       /* the whole signed file, 4-aligned, PSRAM */
    asset_pack_t pack;  /* opened over buf */
} copy_t;

/* PSRAM (docs/bugs/B001): filled before publication, never changed after. */
static EXT_RAM_BSS_ATTR copy_t s_copies[ASSET_PACKS_KINDS][ASSET_PACKS_MAX_LOADS];
static EXT_RAM_BSS_ATTR asset_info_t s_info[ASSET_PACKS_KINDS]; /* s_mu */
static uint32_t s_used[ASSET_PACKS_KINDS];                      /* copies filled: pa_lock */
static _Atomic uint32_t s_offered[ASSET_PACKS_KINDS];           /* index + 1 of the offered copy, 0 none */
static _Atomic uint32_t s_generation;
static _Atomic bool s_mounted;
static _Atomic uint32_t s_image_fp; /* the fingerprint the screen asked with last (0 = not yet) */
static bool s_started; /* the start's loads are done: a new certificates pack waits for a restart */
static SemaphoreHandle_t s_mu;

/* ---- small helpers ---- */

static int slot_of(uint8_t kind)
{
    return kind >= PS_KIND_LETTERS && kind <= PS_KIND_CERTS ? (int)kind - PS_KIND_LETTERS : -1;
}

static uint32_t max_file(uint8_t kind)
{
    switch (kind) {
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

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = src != NULL ? strlen(src) : 0u;
    if (n >= cap) {
        n = cap - 1u;
    }
    if (n > 0u) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static bool release_build(void)
{
#if CONFIG_ALARM_BUILD_RELEASE
    return true;
#else
    return false;
#endif
}

static const char *running_version(void)
{
    return esp_app_get_description()->version;
}

/* Is a partition armed or an alarm sounding? A copy of the alarm task's snapshot, as the language
 * packs read it. When it cannot tell, it says locked. */
static bool locked_now(void)
{
    alarm_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap == NULL) {
        return true;
    }
    alarm_service_get_snapshot(snap);
    const bool locked = alarm_snapshot_locked(snap);
    heap_caps_free(snap);
    return locked;
}

/* Records a kind's state (any task); p: the pack read, NULL when it was not opened (its version,
 * match and draft then show as unknown, never an older pack's beside this entry's bytes). */
static void set_info(uint8_t kind, asset_state_t st, const ps_entry_t *e, const asset_pack_t *p, const char *why)
{
    const int k = slot_of(kind);
    if (k < 0 || s_mu == NULL) {
        return;
    }
    (void)xSemaphoreTake(s_mu, portMAX_DELAY);
    asset_info_t *i = &s_info[k];
    i->state = st;
    i->bytes = e != NULL ? e->length : 0u;
    if (p != NULL) {
        copy_str(i->version, sizeof(i->version), p->version);
        i->match = p->match;
        i->draft = p->draft;
    } else {
        i->version[0] = '\0';
        i->match = 0u;
        i->draft = false;
    }
    copy_str(i->why, sizeof(i->why), why);
    (void)xSemaphoreGive(s_mu);
}

/* ---- the kinds' rules (asset-packs.md "The kinds") ---- */

/* The kind's rules on an opened pack (asset_pack_check_kind, the portable core's, OPT-006):
 * ASSET_OK, else the refusal with a short reason. `entry` the store entry's name (NULL on install:
 * the name is the kind's). The certificates' version rule is the load's (an installed pack may
 * wait for its firmware). */
static asset_result_t kind_rules(const asset_pack_t *p, uint8_t kind, const char *entry, const char **why)
{
    const asset_rule_t r = asset_pack_check_kind(p, kind, entry, release_build());
    *why = asset_rule_reason(r);
    switch (r) {
    case ASSET_RULE_OK:
        return ASSET_OK;
    case ASSET_RULE_KIND:
    case ASSET_RULE_NOT_TAKEN:
    case ASSET_RULE_NAME:
        return ASSET_ERR_KIND;
    case ASSET_RULE_DRAFT:
        return ASSET_ERR_DRAFT;
    default:
        return ASSET_ERR_FORMAT;
    }
}

/* ---- loading ---- */

/* The size fields of a stored entry fit the kind. */
static bool entry_sized(const ps_entry_t *e)
{
    return e->length >= 2u * AP_SECTOR && e->length <= max_file(e->kind) && (e->length % AP_SECTOR) == 0u &&
           e->payload_length >= ASSET_PACK_HEADER_SIZE && e->payload_length <= e->length - AP_SECTOR;
}

/* Reads a stored pack into a new PSRAM buffer, checks its signature, opens it and holds it to the
 * kind's rules. ESP_OK: *buf_out (the caller's) and *p filled. Otherwise nothing is kept; *st and
 * *why say what to record. pa_lock held; an internal-RAM stack. */
static esp_err_t check_entry(const ps_entry_t *e, bool claimed, uint8_t **buf_out, asset_pack_t *p,
                             asset_state_t *st, const char **why)
{
    *buf_out = NULL;
    if (!entry_sized(e)) {
        *st = ASSET_STATE_DAMAGED;
        *why = "its size is not the kind's";
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_aligned_alloc(4, e->length, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        *st = ASSET_STATE_UNCHECKED;
        *why = "no memory to load it";
        return ESP_ERR_NO_MEM;
    }
    if (!pa_read(e, buf)) {
        heap_caps_free(buf);
        *st = ASSET_STATE_UNCHECKED;
        *why = "the flash could not be read";
        return ESP_FAIL;
    }
    const esp_err_t sig = pa_verify(buf, e->length, claimed);
    if (sig != ESP_OK) {
        heap_caps_free(buf);
        if (sig == ESP_ERR_IMAGE_INVALID) { /* the one answer that means a bad signature */
            *st = ASSET_STATE_DAMAGED;
            *why = "the signature does not verify";
        } else if (sig == ESP_ERR_NOT_SUPPORTED) {
            *st = ASSET_STATE_REFUSED;
            *why = "this firmware checks no signatures";
        } else { /* the claim refused (an update runs) or the check could not run: no verdict */
            *st = ASSET_STATE_UNCHECKED;
            *why = sig == ESP_ERR_NOT_FINISHED ? "an update held the check" : "the signature could not be checked";
        }
        return sig;
    }
    const asset_pack_result_t r = asset_pack_open(buf, e->payload_length, p);
    if (r != ASSET_PACK_OK) {
        heap_caps_free(buf);
        *st = ASSET_STATE_DAMAGED;
        *why = "not a valid asset pack";
        ESP_LOGW(TAG, "%s: %s", e->name, asset_pack_result_name(r));
        return ESP_ERR_INVALID_RESPONSE;
    }
    const asset_result_t k = kind_rules(p, e->kind, e->name, why);
    if (k != ASSET_OK) {
        heap_caps_free(buf);
        *st = k == ASSET_ERR_DRAFT ? ASSET_STATE_REFUSED : ASSET_STATE_DAMAGED;
        return ESP_ERR_INVALID_RESPONSE;
    }
    *buf_out = buf;
    return ESP_OK;
}

/* Loads the kind's stored pack (e, NULL: none installed) and offers it. pa_lock held; an
 * internal-RAM stack. A judged failure withdraws the offered copy (its bytes stay valid for a
 * reader that holds them); one that could not be judged keeps it. */
static void load_kind(uint8_t kind, const ps_entry_t *e, bool claimed)
{
    const int k = slot_of(kind);
    if (k < 0) {
        return;
    }
    if (e == NULL) {
        set_info(kind, ASSET_STATE_NONE, NULL, NULL, "");
        return;
    }
    if (s_used[k] >= ASSET_PACKS_MAX_LOADS) {
        ESP_LOGW(TAG, "%s: loaded %d times since the start: used from the next restart", e->name,
                 ASSET_PACKS_MAX_LOADS);
        set_info(kind, ASSET_STATE_RESTART, e, NULL, "used from the next restart");
        return;
    }
    copy_t *c = &s_copies[k][s_used[k]];
    asset_state_t st = ASSET_STATE_DAMAGED;
    const char *why = "";
    uint8_t *buf = NULL;
    const esp_err_t r = check_entry(e, claimed, &buf, &c->pack, &st, &why);
    if (r == ESP_OK && kind == PS_KIND_CERTS && !asset_pack_made_for(&c->pack, running_version())) {
        /* OPT-013: trust follows the firmware; the image's common list until the matching pack */
        st = ASSET_STATE_REFUSED;
        why = "made for another firmware version";
        ESP_LOGW(TAG, "%s: made for %s, this firmware is %s: the built-in list", e->name, c->pack.version,
                 running_version());
        set_info(kind, st, e, &c->pack, why);
        heap_caps_free(buf);
        memset(&c->pack, 0, sizeof(c->pack));
        atomic_store_explicit(&s_offered[k], 0u, memory_order_release);
        return;
    }
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s (%s)", e->name, why, asset_state_name(st));
        set_info(kind, st, e, NULL, why);
        memset(&c->pack, 0, sizeof(c->pack));
        if (st != ASSET_STATE_UNCHECKED && atomic_load_explicit(&s_offered[k], memory_order_relaxed) != 0u) {
            atomic_store_explicit(&s_offered[k], 0u, memory_order_release);
            atomic_fetch_add_explicit(&s_generation, 1u, memory_order_relaxed);
        }
        return;
    }
    c->buf = buf;
    s_used[k]++;
    atomic_store_explicit(&s_offered[k], s_used[k], memory_order_release); /* published last */
    atomic_fetch_add_explicit(&s_generation, 1u, memory_order_relaxed);
    set_info(kind, ASSET_STATE_READY, e, &c->pack, "");
    ESP_LOGI(TAG, "%s loaded (%u bytes, version %s%s, match 0x%08x)", e->name, (unsigned)e->length,
             c->pack.version, c->pack.draft ? ", draft" : "", (unsigned)c->pack.match);
}

static const copy_t *offered(uint8_t kind)
{
    const int k = slot_of(kind);
    if (k < 0) {
        return NULL;
    }
    const uint32_t i = atomic_load_explicit(&s_offered[k], memory_order_acquire);
    return i != 0u && i <= ASSET_PACKS_MAX_LOADS ? &s_copies[k][i - 1u] : NULL;
}

/* Hands the loaded certificates pack's bundle to TLS (OPT-013): at start only, before anything
 * opens a TLS connection. esp_crt_bundle_set() checks the bundle (offsets, count at most
 * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS) and keeps the pointer: the copy is in PSRAM until
 * restart and never freed, and the part is 4-aligned in a 4-aligned buffer, as the bundle's
 * uint32 offsets need. esp_crt_bundle_detach() would drop it back to the image's common list:
 * nothing in the firmware calls it, keep it so. Without a pack made for this version (none,
 * refused, damaged), TLS keeps the common list built into the image. pa_lock held. */
static void certs_to_tls(void)
{
    const void *data = NULL;
    size_t size = 0;
    if (!asset_packs_certs(&data, &size)) {
        ESP_LOGI(TAG, "TLS trusts the built-in common list");
        return;
    }
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    const bool aligned = ((uintptr_t)data & 3u) == 0u;
    if (aligned && esp_crt_bundle_set(data, size) == ESP_OK) {
        ESP_LOGI(TAG, "TLS trusts the certificates pack (%u bytes)", (unsigned)size);
        return;
    }
    const char *why = aligned ? "the bundle is not a valid certificate list" : "the bundle is not 4-aligned";
#else
    const char *why = "this firmware has no certificate bundle";
#endif
    const int k = slot_of(PS_KIND_CERTS);
    const copy_t *c = offered(PS_KIND_CERTS);
    ESP_LOGW(TAG, "certs: %s: the built-in common list", why);
    set_info(PS_KIND_CERTS, ASSET_STATE_REFUSED, pa_find(PS_KIND_CERTS, asset_pack_store_name(PS_KIND_CERTS)),
             c != NULL ? &c->pack : NULL, why);
    atomic_store_explicit(&s_offered[k], 0u, memory_order_release);
}

/* ---- the public API ---- */

esp_err_t asset_packs_start(void)
{
    if (s_mu != NULL) {
        return ESP_OK; /* started */
    }
    for (uint8_t kind = PS_KIND_LETTERS; kind <= PS_KIND_CERTS; kind++) {
        asset_info_t *i = &s_info[slot_of(kind)];
        memset(i, 0, sizeof(*i));
        i->kind = kind;
        copy_str(i->name, sizeof(i->name), asset_pack_store_name(kind));
    }
    s_mu = xSemaphoreCreateMutex();
    if (s_mu == NULL) {
        ESP_LOGE(TAG, "out of memory: no asset packs (the built-in fallbacks)");
        return ESP_OK;
    }
    const pa_result_t m = pa_init(); /* once, shared with the language and voice packs */
    if (m != PA_OK) {
        ESP_LOGW(TAG, "no pack store (%s): the built-in fallbacks", pa_result_name(m));
        s_started = true;
        return ESP_OK;
    }
    atomic_store(&s_mounted, true);
    pa_lock();
    for (uint8_t kind = PS_KIND_LETTERS; kind <= PS_KIND_CERTS; kind++) {
        load_kind(kind, pa_find(kind, asset_pack_store_name(kind)), false);
    }
    certs_to_tls();
    s_started = true;
    pa_unlock();
    return ESP_OK;
}

size_t asset_packs_list(asset_info_t *out, size_t max)
{
    if (out == NULL || s_mu == NULL || !atomic_load(&s_mounted)) {
        return 0;
    }
    const size_t n = max < ASSET_PACKS_KINDS ? max : ASSET_PACKS_KINDS;
    (void)xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(out, s_info, n * sizeof(*out));
    (void)xSemaphoreGive(s_mu);
    return n;
}

uint32_t asset_packs_free_bytes(void)
{
    return atomic_load(&s_mounted) ? pa_free_bytes() : 0u;
}

uint32_t asset_packs_generation(void)
{
    return atomic_load_explicit(&s_generation, memory_order_relaxed);
}

/* A part of the offered copy of a kind, of this name and type. */
static bool offered_part(uint8_t kind, const char *name, uint32_t type, const void **data, size_t *size)
{
    if (name == NULL || data == NULL || size == NULL) {
        return false;
    }
    const copy_t *c = offered(kind);
    const asset_part_t *part = c != NULL ? asset_pack_find(&c->pack, name) : NULL;
    if (part == NULL || part->type != type) {
        return false;
    }
    *data = part->data;
    *size = part->size;
    return true;
}

bool asset_packs_font(const char *role, const void **data, size_t *size)
{
    return offered_part(PS_KIND_LETTERS, role, ASSET_PART_FONT, data, size);
}

bool asset_packs_picture(const char *name, const void **data, size_t *size)
{
    return offered_part(PS_KIND_PICTURE, name, ASSET_PART_JPEG, data, size);
}

bool asset_packs_letters_match(uint32_t fingerprint)
{
    atomic_store_explicit(&s_image_fp, fingerprint, memory_order_relaxed);
    const copy_t *c = offered(PS_KIND_LETTERS);
    return c != NULL && c->pack.match == fingerprint;
}

uint32_t asset_packs_image_fingerprint(void)
{
#ifdef ALARM_LETTERS_FINGERPRINT
    return ALARM_LETTERS_FINGERPRINT; /* the build's, from the layout's letters.json */
#else
    return atomic_load_explicit(&s_image_fp, memory_order_relaxed);
#endif
}

bool asset_packs_certs(const void **data, size_t *size)
{
    return offered_part(PS_KIND_CERTS, "bundle", ASSET_PART_CERTS, data, size);
}

/* ---- installing ---- */

typedef struct {
    char version[16];
    uint32_t match;
    bool draft;
} install_ctx_t;

/* pack_assets' validate callback: the RAST format and the kind's rules on the verified file. 0 to
 * go on, else the asset_result_t that refuses it. Writes the entry's name: the kind's. */
static int install_validate(const uint8_t *file, size_t len, uint32_t payload_length, char name[PS_NAME_MAX + 1],
                            void *ctx)
{
    (void)len;
    install_ctx_t *ic = ctx;
    asset_pack_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); /* about 0.4 KB */
    if (p == NULL) {
        return ASSET_ERR_FLASH;
    }
    int result = ASSET_OK;
    const char *why = "";
    if (asset_pack_open(file, payload_length, p) != ASSET_PACK_OK) {
        result = ASSET_ERR_FORMAT;
    } else {
        result = kind_rules(p, p->kind, NULL, &why);
    }
    if (result == ASSET_OK) {
        copy_str(name, PS_NAME_MAX + 1u, p->name);
        copy_str(ic->version, sizeof(ic->version), p->version);
        ic->match = p->match;
        ic->draft = p->draft;
    } else if (why[0] != '\0') {
        ESP_LOGW(TAG, "install refused: %s", why);
    }
    heap_caps_free(p);
    return result;
}

/* pack_assets' done callback (pa_lock and the update claim held): the letters and the picture load
 * now; the certificates at the next start (OPT-013: loaded before any TLS, never swapped live). */
static void install_done(const ps_entry_t *e, void *ctx)
{
    const install_ctx_t *ic = ctx;
    const int k = slot_of(e->kind);
    if (e->kind == PS_KIND_CERTS || (k >= 0 && s_used[k] >= ASSET_PACKS_MAX_LOADS)) {
        asset_pack_t shown; /* the installed pack's details, as install_validate read them */
        memset(&shown, 0, sizeof(shown));
        shown.version = ic->version;
        shown.match = ic->match;
        shown.draft = ic->draft;
        set_info(e->kind, ASSET_STATE_RESTART, e, &shown, "installed: used from the next start");
        ESP_LOGI(TAG, "%s installed: used from the next start", e->name);
        return;
    }
    load_kind(e->kind, e, true);
}

asset_result_t asset_packs_install(const uint8_t *file, size_t len, uint8_t *kind)
{
    if (!atomic_load(&s_mounted) || !s_started) {
        return ASSET_ERR_OFF;
    }
    if (locked_now()) {
        return ASSET_ERR_LOCKED;
    }
    if (file == NULL || len < 2u * AP_SECTOR || (len % AP_SECTOR) != 0u) {
        return ASSET_ERR_SIZE;
    }
    if (memcmp(file, "RAST", 4) != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint8_t k = file[12]; /* the header's kind (asset-packs.md "The pack file") */
    if (kind != NULL) {
        *kind = k;
    }
    if (slot_of(k) < 0) {
        return ASSET_ERR_KIND;
    }
    if (len > max_file(k)) {
        return ASSET_ERR_SIZE;
    }
    install_ctx_t ic;
    memset(&ic, 0, sizeof(ic));
    const pa_install_t req = {.kind = k,
                              .file = file,
                              .len = len,
                              .ff_padding = true, /* the builder pads with 0xFF, as the Flasher's reader checks */
                              .validate = install_validate,
                              .gate = NULL,
                              .done = install_done,
                              .ctx = &ic};
    int reject = ASSET_ERR_FORMAT;
    switch (pa_install(&req, &reject)) {
    case PA_OK:
        return ASSET_OK;
    case PA_ERR_OFF:
        return ASSET_ERR_OFF;
    case PA_ERR_SIZE:
        return ASSET_ERR_SIZE;
    case PA_ERR_BUSY:
        return ASSET_ERR_BUSY;
    case PA_ERR_SIGNATURE:
        return ASSET_ERR_SIGNATURE;
    case PA_ERR_FORMAT:
        return ASSET_ERR_FORMAT;
    case PA_ERR_REJECTED:
        return (asset_result_t)reject;
    case PA_ERR_NO_SPACE:
        return ASSET_ERR_NO_SPACE;
    case PA_ERR_CLOSED:
        return ASSET_ERR_LOCKED;
    case PA_ERR_NOT_FOUND:
    case PA_ERR_FLASH:
    default:
        return ASSET_ERR_FLASH;
    }
}

size_t asset_packs_verify_all(asset_verify_t *out, size_t max)
{
    if (out == NULL || !atomic_load(&s_mounted)) {
        return 0;
    }
    size_t n = 0;
    pa_lock();
    for (uint8_t kind = PS_KIND_LETTERS; kind <= PS_KIND_CERTS && n < max; kind++) {
        const ps_entry_t *e = pa_find(kind, asset_pack_store_name(kind));
        if (e == NULL) {
            continue;
        }
        out[n].kind = kind;
        copy_str(out[n].name, sizeof(out[n].name), e->name);
        uint8_t *buf = NULL;
        asset_pack_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        asset_state_t st;
        const char *why = "";
        out[n].err = p != NULL ? check_entry(e, false, &buf, p, &st, &why) : ESP_ERR_NO_MEM;
        if (out[n].err == ESP_OK && kind == PS_KIND_CERTS && !asset_pack_made_for(p, running_version())) {
            out[n].err = ESP_ERR_INVALID_VERSION; /* OPT-013: refused at the load too */
        }
        heap_caps_free(buf);
        heap_caps_free(p);
        n++;
    }
    pa_unlock();
    return n;
}

#else /* !CONFIG_ALARM_FEATURE_LANG_PACKS: no pack store; every kind's built-in fallback */

esp_err_t asset_packs_start(void)
{
    return ESP_OK;
}

size_t asset_packs_list(asset_info_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

uint32_t asset_packs_free_bytes(void)
{
    return 0u;
}

uint32_t asset_packs_generation(void)
{
    return 0u;
}

bool asset_packs_font(const char *role, const void **data, size_t *size)
{
    (void)role;
    (void)data;
    (void)size;
    return false;
}

bool asset_packs_picture(const char *name, const void **data, size_t *size)
{
    (void)name;
    (void)data;
    (void)size;
    return false;
}

bool asset_packs_letters_match(uint32_t fingerprint)
{
    (void)fingerprint;
    return false;
}

uint32_t asset_packs_image_fingerprint(void)
{
    return 0;
}

bool asset_packs_certs(const void **data, size_t *size)
{
    (void)data;
    (void)size;
    return false;
}

asset_result_t asset_packs_install(const uint8_t *file, size_t len, uint8_t *kind)
{
    (void)file;
    (void)len;
    (void)kind;
    return ASSET_ERR_OFF;
}

size_t asset_packs_verify_all(asset_verify_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

#endif /* CONFIG_ALARM_FEATURE_LANG_PACKS */
