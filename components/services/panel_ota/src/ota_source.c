/*
 * ota_source.c - a master's image sources for its keypads (docs/features/11 "The master", image
 * sources): its own running image, read through windows of its partition, its own update waiting
 * in its spare slot (the same windows, of that partition), and one copy in PSRAM fetched with a
 * keypad's facts. No partition change, no fw_cache, and no flash is ever written:
 * a foreign image never goes into the master's own spare slot, which esp_ota_set_boot_partition
 * would boot.
 *
 * Nothing here runs or allocates until a caller asks (panel_ota_running_image, _running_read,
 * _fetch_for): a stand-alone panel and a master without keypads are as before. Mapping a window
 * disables the cache on the P4 for a moment, as a flash write does, so it happens only on tasks
 * with an internal-RAM stack: the update task and the master's link task.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_service.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "panel_ota_internal.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "ota_src";

#define WINDOW         (256u * 1024u)        /* a window of the running partition */
#define WINDOW_IDLE_MS 30000u                /* unmapped after this long unused */
#define COPY_IDLE_MS   (30u * 60u * 1000u)   /* a copy is freed after this long unused */
#define HASH_PIECE     4096u

typedef enum { IMG_NONE = 0, IMG_ASKED, IMG_READY, IMG_FAILED } img_state_t;

/* Everything below is under s_src, made on the first use (a stand-alone panel never makes it).
 * Order: panel_ota's writer lock, then s_src, then panel_ota's lock (panel_ota_fetch_for asks
 * panel_ota_run_busy() under s_src); nothing takes s_src while holding panel_ota's lock. */
static SemaphoreHandle_t s_src;

/* The running image (worked out once after confirmation), the window it is read through and the
 * copy's buffer. In slow memory, as the rest: only tasks touch it, never with the cache off. */
typedef struct {
    img_state_t img_state;
    const esp_partition_t *part;
    esp_partition_mmap_handle_t win_handle;
    const uint8_t *win_ptr;
    uint32_t win_off;
    uint32_t win_len;
    bool win_mapped;
    uint64_t win_used_ms;
    uint8_t *copy_buf; /* PSRAM, s_copy.size bytes while READY */
    uint64_t copy_used_ms;
} src_t;
static src_t S ALARM_SLOW_BSS;
static panel_ota_image_t s_img ALARM_SLOW_BSS;

/* The copy for a keypad: asked, then fetched by the update task into PSRAM. */
typedef struct {
    bool pending;
    char device[32];
    char board_rev[16];
    char chip_rev[8];
    char project[33];
    char version[32]; /* the keypad's running version */
    char want[32];    /* V */
} copy_req_t;
static copy_req_t s_creq ALARM_SLOW_BSS;
static panel_ota_copy_t s_copy ALARM_SLOW_BSS;
/* Ends a copy's download under way: another copy asked for, the copy freed, or this panel's own
 * run. Written only under s_src: set by those, cleared by the update task when it takes the next
 * request. Its download reads it between steps, without the lock. */
static volatile bool s_copy_stop ALARM_SLOW_BSS;

static SemaphoreHandle_t src_lock(void)
{
    SemaphoreHandle_t m = __atomic_load_n(&s_src, __ATOMIC_ACQUIRE);
    if (m != NULL) {
        return m;
    }
    SemaphoreHandle_t fresh = xSemaphoreCreateMutex();
    if (fresh == NULL) {
        return NULL;
    }
    SemaphoreHandle_t expected = NULL;
    if (!__atomic_compare_exchange_n(&s_src, &expected, fresh, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        vSemaphoreDelete(fresh); /* another task made it first */
        return expected;
    }
    return fresh;
}

static bool take(void)
{
    SemaphoreHandle_t m = src_lock();
    if (m == NULL) {
        return false;
    }
    xSemaphoreTake(m, portMAX_DELAY);
    return true;
}

static void give(void)
{
    xSemaphoreGive(s_src);
}

/* ---- the running image ----------------------------------------------------------------------- */

static void win_unmap_locked(void)
{
    if (S.win_mapped) {
        esp_partition_munmap(S.win_handle);
        S.win_mapped = false;
        S.win_ptr = NULL;
    }
}

/* len bytes at off of part (the running partition, or the spare one), through the 256 KB window
 * (call with s_src held, from a task with an internal-RAM stack). A window of another partition is
 * unmapped first. */
static esp_err_t win_read_locked(const esp_partition_t *part, uint32_t off, uint8_t *out, size_t len)
{
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (S.part != part) {
        win_unmap_locked();
        S.part = part;
    }
    if ((uint64_t)off + len > S.part->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (len > 0u) {
        const uint32_t start = off - off % WINDOW;
        if (!S.win_mapped || S.win_off != start) {
            win_unmap_locked();
            const uint32_t rest = (uint32_t)S.part->size - start;
            const uint32_t n = rest < WINDOW ? rest : WINDOW;
            const void *ptr = NULL;
            esp_partition_mmap_handle_t handle = 0;
            const esp_err_t err =
                esp_partition_mmap(S.part, start, n, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "map at %" PRIu32 ": %s", start, esp_err_to_name(err));
                return err;
            }
            S.win_handle = handle;
            S.win_ptr = ptr;
            S.win_off = start;
            S.win_len = n;
            S.win_mapped = true;
        }
        const uint32_t in = off - S.win_off;
        const size_t take_n = len < S.win_len - in ? len : S.win_len - in;
        memcpy(out, S.win_ptr + in, take_n);
        off += (uint32_t)take_n;
        out += take_n;
        len -= take_n;
    }
    S.win_used_ms = alarm_service_now();
    return ESP_OK;
}

static bool part_read(void *ctx, uint32_t offset, void *out, size_t len)
{
    return esp_partition_read((const esp_partition_t *)ctx, offset, out, len) == ESP_OK;
}

/* The SHA-256 of the image's size bytes, read through the windows in 4 KB pieces. */
static bool hash_running(uint32_t size, uint8_t out[32])
{
    uint8_t *buf = alarm_mem_slow_malloc(HASH_PIECE);
    psa_hash_operation_t op = psa_hash_operation_init();
    bool ok = buf != NULL && psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t off = 0; ok && off < size; off += HASH_PIECE) {
        const uint32_t n = size - off < HASH_PIECE ? size - off : HASH_PIECE;
        if (!take()) {
            ok = false;
            break;
        }
        ok = win_read_locked(esp_ota_get_running_partition(), off, buf, n) == ESP_OK;
        give();
        ok = ok && psa_hash_update(&op, buf, n) == PSA_SUCCESS;
    }
    size_t len = 0;
    if (ok) {
        ok = psa_hash_finish(&op, out, 32u, &len) == PSA_SUCCESS && len == 32u;
    } else {
        psa_hash_abort(&op);
    }
    free(buf);
    return ok;
}

/* The update task, once the image is confirmed: its size, digest and facts. */
static void image_work(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    panel_ota_image_t img;
    memset(&img, 0, sizeof(img));
    uint8_t head[PROTO_FW_HEADER_LEN];
    proto_fw_header_t h;
    bool ok = part != NULL && esp_partition_read(part, 0, head, sizeof(head)) == ESP_OK &&
              proto_fw_header_read(head, sizeof(head), &h) && h.image_magic && h.app_magic;
    if (ok) {
        img.size = proto_fw_image_size(part_read, (void *)part, (uint32_t)part->size,
                                       (uint16_t)CONFIG_IDF_FIRMWARE_CHIP_ID);
        ok = img.size != 0u;
    }
    if (ok) {
        ok = hash_running(img.size, img.sha256);
    }
    if (ok) {
        snprintf(img.version, sizeof(img.version), "%s", h.version);
        snprintf(img.project, sizeof(img.project), "%s", h.project);
        img.chip_id = h.chip_id;
        img.min_rev_full = h.min_rev_full;
        img.max_rev_full = h.max_rev_full;
        img.has_key = panel_ota_trust_own_key(img.key_sha256);
    }
    if (!take()) {
        return;
    }
    s_img = img;
    S.img_state = ok ? IMG_READY : IMG_FAILED;
    give();
    if (ok) {
        ESP_LOGI(TAG, "running image %s: %" PRIu32 " bytes, %02x%02x%02x%02x..., revisions %u-%u",
                 img.version, img.size, img.sha256[0], img.sha256[1], img.sha256[2],
                 img.sha256[3], (unsigned)img.min_rev_full, (unsigned)img.max_rev_full);
    } else {
        ESP_LOGW(TAG, "the running image can't be worked out for the keypads' relay");
    }
}

esp_err_t panel_ota_running_image(panel_ota_image_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!take()) {
        return ESP_ERR_NO_MEM;
    }
    const img_state_t state = S.img_state;
    if (state == IMG_READY) {
        *out = s_img;
    } else if (state == IMG_NONE) {
        S.img_state = IMG_ASKED;
    }
    give();
    if (state == IMG_NONE) {
        panel_ota_notify_source();
    }
    return state == IMG_READY ? ESP_OK : state == IMG_FAILED ? ESP_FAIL : ESP_ERR_NOT_FINISHED;
}

esp_err_t panel_ota_running_read(uint32_t offset, void *out, size_t len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    if (S.img_state != IMG_READY) {
        err = ESP_ERR_NOT_FINISHED;
    } else if ((uint64_t)offset + len > s_img.size) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        err = win_read_locked(esp_ota_get_running_partition(), offset, out, len);
    }
    give();
    return err;
}

esp_err_t panel_ota_source_part_read(const esp_partition_t *part, uint32_t offset, void *out,
                                     size_t len)
{
    if (out == NULL || part == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take()) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = win_read_locked(part, offset, out, len);
    give();
    return err;
}

/* ---- the copy ------------------------------------------------------------------------------ */

static void copy_free_locked(void)
{
    free(S.copy_buf);
    S.copy_buf = NULL;
    if (s_copy.state != PANEL_OTA_COPY_NONE) {
        const uint32_t seq = s_copy.seq + 1u;
        memset(&s_copy, 0, sizeof(s_copy));
        s_copy.seq = seq;
    }
}

esp_err_t panel_ota_fetch_for(const proto_fwsrv_facts_t *facts, const char *version)
{
    proto_version_t v;
    if (facts == NULL || version == NULL || facts->device == NULL || facts->device[0] == '\0' ||
        facts->chip_rev == NULL || facts->chip_rev[0] == '\0' || facts->project == NULL ||
        facts->project[0] == '\0' || facts->version == NULL || facts->version[0] == '\0' ||
        !proto_version_parse(version, &v)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take()) {
        return ESP_ERR_NO_MEM;
    }
    /* Asked under s_src: a run accepted before this answer frees the copy only after this
     * request is in (panel_ota_source_own_run takes s_src too), so it never outlives the start. */
    /* not started, or its own run: its version is changing - but its own release downloaded and
     * checked, waiting for the restart, is the very version its keypads need a copy of */
    if (panel_ota_run_busy() && !panel_ota_own_waiting()) {
        give();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_creq, 0, sizeof(s_creq));
    s_creq.pending = true;
    snprintf(s_creq.device, sizeof(s_creq.device), "%s", facts->device);
    snprintf(s_creq.board_rev, sizeof(s_creq.board_rev), "%s",
             facts->board_rev != NULL ? facts->board_rev : "");
    snprintf(s_creq.chip_rev, sizeof(s_creq.chip_rev), "%s", facts->chip_rev);
    snprintf(s_creq.project, sizeof(s_creq.project), "%s", facts->project);
    snprintf(s_creq.version, sizeof(s_creq.version), "%s", facts->version);
    snprintf(s_creq.want, sizeof(s_creq.want), "%s", version);
    copy_free_locked(); /* one copy at a time: the new one replaces it */
    s_copy.state = PANEL_OTA_COPY_FETCHING;
    snprintf(s_copy.device, sizeof(s_copy.device), "%s", s_creq.device);
    snprintf(s_copy.chip_rev, sizeof(s_copy.chip_rev), "%s", s_creq.chip_rev);
    snprintf(s_copy.project, sizeof(s_copy.project), "%s", s_creq.project);
    snprintf(s_copy.version, sizeof(s_copy.version), "%s", s_creq.want);
    s_copy.seq++;
    s_copy_stop = true; /* the copy before, if its download is still under way, is not wanted */
    give();
    panel_ota_notify_source();
    return ESP_OK;
}

/* The update task: the check with the keypad's facts and want, then the download into PSRAM. */
static void copy_fetch(const copy_req_t *req)
{
    const proto_fwsrv_facts_t facts = {
        .device = req->device,
        .board_rev = req->board_rev,
        .chip_rev = req->chip_rev,
        .project = req->project,
        .version = req->version,
    };
    proto_fwsrv_answer_t *a = calloc(1, sizeof(*a));
    uint8_t why = PROTO_FW_WHY_NO_MEMORY;
    uint8_t *buf = NULL;
    if (a != NULL) {
        why = panel_ota_check_other(&facts, req->want, a);
    }
    if (why == PROTO_FWSRV_WHY_OK) {
        if (a->image.size == 0u) {
            why = PROTO_FW_WHY_BAD_SIZE;
        } else if (a->image.size > PANEL_OTA_COPY_MAX) {
            why = PROTO_FW_WHY_TOO_BIG;
        } else if (!a->image.has_sha256) {
            why = PROTO_FW_WHY_BAD_SHA; /* a copy is relayed by its digest (id8) */
        } else if ((buf = alarm_mem_slow_malloc(a->image.size)) == NULL) {
            why = PROTO_FW_WHY_NO_MEMORY;
        } else {
            size_t got = 0;
            uint8_t code = PROTO_FW_WHY_OK;
            if (panel_ota_download_stoppable(a->image.url, a->image.size, a->image.sha256, buf,
                                             a->image.size, &got, &s_copy_stop, &code) != ESP_OK) {
                why = s_copy_stop ? PROTO_FW_WHY_CANCELLED
                                  : code != PROTO_FW_WHY_OK ? code : PROTO_FW_WHY_BROKE_OFF;
            }
        }
    }
    if (!take()) {
        free(buf);
        free(a);
        return;
    }
    const bool wanted = !s_copy_stop && !s_creq.pending && s_copy.state == PANEL_OTA_COPY_FETCHING;
    if (why == PROTO_FWSRV_WHY_OK && wanted) {
        S.copy_buf = buf;
        buf = NULL;
        s_copy.state = PANEL_OTA_COPY_READY;
        s_copy.why = PROTO_FWSRV_WHY_OK;
        s_copy.size = a->image.size;
        memcpy(s_copy.sha256, a->image.sha256, sizeof(s_copy.sha256));
        memcpy(s_copy.key_sha256, a->key_sha256, sizeof(s_copy.key_sha256));
        s_copy.has_key = a->has_key;
        S.copy_used_ms = alarm_service_now();
        s_copy.seq++;
    } else if (wanted) {
        s_copy.state = PANEL_OTA_COPY_FAILED;
        s_copy.why = why;
        s_copy.seq++;
    }
    give();
    if (why == PROTO_FWSRV_WHY_OK && wanted) {
        ESP_LOGI(TAG, "a copy of %s for %s (%s): %" PRIu32 " bytes", req->want, req->device,
                 req->chip_rev, a->image.size);
    } else {
        ESP_LOGW(TAG, "no copy of %s for %s (%s): why %u", req->want, req->device, req->chip_rev,
                 (unsigned)why);
    }
    free(buf);
    free(a);
}

void panel_ota_copy_get(panel_ota_copy_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (__atomic_load_n(&s_src, __ATOMIC_ACQUIRE) == NULL || !take()) {
        return; /* never asked */
    }
    *out = s_copy;
    give();
}

esp_err_t panel_ota_copy_read(const uint8_t *id8, uint32_t offset, void *out, size_t len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (__atomic_load_n(&s_src, __ATOMIC_ACQUIRE) == NULL || !take()) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_OK;
    if (s_copy.state != PANEL_OTA_COPY_READY || S.copy_buf == NULL ||
        (id8 != NULL && memcmp(id8, s_copy.sha256, 8u) != 0)) {
        err = ESP_ERR_NOT_FOUND;
    } else if ((uint64_t)offset + len > s_copy.size) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(out, S.copy_buf + offset, len);
        S.copy_used_ms = alarm_service_now();
    }
    give();
    return err;
}

/* Frees the copy, forgets one asked for and ends a download under way (call with s_src held).
 * true when there was anything. */
static bool copy_drop_locked(void)
{
    const bool had = s_creq.pending || s_copy.state != PANEL_OTA_COPY_NONE;
    s_copy_stop = true; /* a download under way ends, and is not kept */
    s_creq.pending = false;
    copy_free_locked();
    return had;
}

void panel_ota_copy_free(void)
{
    if (__atomic_load_n(&s_src, __ATOMIC_ACQUIRE) == NULL || !take()) {
        return; /* never asked: nothing to free, nothing downloading */
    }
    (void)copy_drop_locked();
    give();
}

void panel_ota_source_own_run(void)
{
    /* Freed here, on the task that starts the run, rather than at the update task's next tick:
     * that task goes straight on to the run's own download, which needs the memory, and a copy of
     * the version that runs now must not be relayed from then on. Every reader takes s_src. */
    if (__atomic_load_n(&s_src, __ATOMIC_ACQUIRE) == NULL || !take()) {
        return; /* nothing was ever asked */
    }
    const bool had = copy_drop_locked();
    give();
    if (had) {
        ESP_LOGI(TAG, "this panel's own update runs: no copy for a keypad");
    }
}

void panel_ota_source_tick(uint64_t now, bool asked)
{
    (void)asked;
    if (__atomic_load_n(&s_src, __ATOMIC_ACQUIRE) == NULL) {
        return; /* nothing was ever asked */
    }
    if (!take()) {
        return;
    }
    const bool image = S.img_state == IMG_ASKED;
    copy_req_t req;
    memset(&req, 0, sizeof(req));
    const bool fetch = s_creq.pending;
    if (fetch) {
        req = s_creq;
        s_creq.pending = false;
        s_copy_stop = false; /* this request's download runs until something stops it */
    }
    if (s_copy.state == PANEL_OTA_COPY_READY && now - S.copy_used_ms >= COPY_IDLE_MS) {
        copy_free_locked();
        ESP_LOGI(TAG, "the copy for a keypad was unused for 30 min: freed");
    }
    if (S.win_mapped && now - S.win_used_ms >= WINDOW_IDLE_MS) {
        win_unmap_locked();
    }
    give();
    if (image && panel_ota_confirmed()) { /* once after confirmation */
        image_work();
    }
    if (fetch) {
        copy_fetch(&req);
    }
}
