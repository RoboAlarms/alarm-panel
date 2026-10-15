/*
 * cp_ota.c - updates the Wi-Fi co-processor (the ESP32-C6 running ESP-Hosted) from the image
 * in the "cp_fw" partition (docs/features/12, OTA-007).
 *
 * The partition holds a plain ESP-IDF application image for the C6 (written over USB with
 * parttool, and later by the panel's own release download). It is streamed to the C6 over the
 * ESP-Hosted OTA calls; the C6 checks it and boots it after the panel restarts. The C6 keeps
 * its previous image until the new one is complete, so a failed transfer changes nothing.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "alarm_service.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "nvs.h"
#include "panel_ota_internal.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_HOST
#include "esp_hosted.h"
#endif

static const char *TAG = "ota_cp";

#define NVS_NS     "ota"
#define CHUNK      1024u
#define SIG_MAGIC  0xE7u /* first byte of a Secure Boot v2 signature block */

typedef struct {
    bool valid;
    uint32_t size;
    char version[24];
} cp_image_t;

static const esp_partition_t *s_part;
static cp_image_t s_img;
static bool s_requested;
static alarm_source_t s_src;
static uint8_t s_user;
#if CONFIG_PANEL_OTA_CP_AUTO
static bool s_auto_armed; /* automatic update decided, waiting for an idle system */
#endif

/* Size of an ESP-IDF image: header, segments, checksum padded to 16 bytes, optional SHA-256
 * and signature block. false when the partition doesn't start with a C6 application. */
static bool read_image(const esp_partition_t *p, cp_image_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_image_header_t ih;
    if (esp_partition_read(p, 0, &ih, sizeof(ih)) != ESP_OK || ih.magic != ESP_IMAGE_HEADER_MAGIC ||
        ih.chip_id != ESP_CHIP_ID_ESP32C6 || ih.segment_count == 0u || ih.segment_count > 16u) {
        return false;
    }
    uint32_t off = sizeof(ih);
    for (unsigned i = 0; i < ih.segment_count; i++) {
        esp_image_segment_header_t sh;
        if (esp_partition_read(p, off, &sh, sizeof(sh)) != ESP_OK || sh.data_len > p->size) {
            return false;
        }
        if (i == 0u) {
            esp_app_desc_t d;
            if (esp_partition_read(p, off + sizeof(sh), &d, sizeof(d)) != ESP_OK ||
                d.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                return false;
            }
            panel_ota_version_copy(out->version, sizeof(out->version), d.version,
                                   sizeof(d.version));
        }
        off += (uint32_t)sizeof(sh) + sh.data_len;
        if (off > p->size) {
            return false;
        }
    }
    off = (off + 1u + 15u) & ~15u; /* checksum byte, padded */
    if (ih.hash_appended == 1u) {
        off += 32u;
    }
    const uint32_t sig = (off + 4095u) & ~4095u;
    uint8_t magic = 0;
    if (sig + 4096u <= p->size && esp_partition_read(p, sig, &magic, 1) == ESP_OK &&
        magic == SIG_MAGIC) {
        off = sig + 4096u;
    }
    if (off > p->size) {
        return false;
    }
    out->size = off;
    out->valid = true;
    return true;
}

/* "v2.12.3-abc" -> 2, 12, 3. */
static bool parse3(const char *s, unsigned v[3])
{
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    return sscanf(s, "%u.%u.%u", &v[0], &v[1], &v[2]) == 3;
}

static bool same_version(const char *a, const char *b)
{
    unsigned va[3];
    unsigned vb[3];
    if (parse3(a, va) && parse3(b, vb)) {
        return va[0] == vb[0] && va[1] == vb[1] && va[2] == vb[2];
    }
    return strcmp(a, b) == 0;
}

void panel_ota_cp_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "cp_fw");
    if (s_part != NULL && read_image(s_part, &s_img)) {
        ESP_LOGI(TAG, "co-processor image %s (%" PRIu32 " bytes) in cp_fw", s_img.version,
                 s_img.size);
        panel_ota_set_cp_image(s_img.version);
    }
}

static bool cp_version(char *out, size_t cap)
{
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    snprintf(out, cap, "%s", ws.coproc_fw);
    return out[0] != '\0';
}

/* Streams the image; on success the update task restarts the panel once the system is idle. */
static void run_update(void)
{
#if CONFIG_ESP_HOSTED_HOST && CONFIG_ESP_HOSTED_HOST_FEAT_OTA_READY
    char running[24] = "";
    cp_version(running, sizeof(running));
    ESP_LOGW(TAG, "updating the co-processor %s -> %s", running[0] ? running : "?",
             s_img.version);
    panel_ota_set_progress(PANEL_OTA_RECEIVING, 0, NULL);
    static uint8_t buf[CHUNK];
    int shown = 0;
    esp_err_t err = esp_hosted_cp_ota_begin();
    for (uint32_t off = 0; err == ESP_OK && off < s_img.size; off += CHUNK) {
        const uint32_t n = s_img.size - off < CHUNK ? s_img.size - off : CHUNK;
        err = esp_partition_read(s_part, off, buf, n);
        if (err == ESP_OK) {
            err = esp_hosted_cp_ota_write(buf, n);
        }
        const int percent = (int)((uint64_t)(off + n) * 100u / s_img.size);
        if (percent >= shown + 5) {
            shown = percent;
            panel_ota_set_progress(PANEL_OTA_RECEIVING, percent, NULL);
        }
    }
    if (err == ESP_OK) {
        err = esp_hosted_cp_ota_end();
    }
    unsigned v[3] = {0, 0, 0};
    const bool can_activate = parse3(running, v) && (v[0] > 2u || (v[0] == 2u && v[1] >= 6u));
    if (err == ESP_OK && can_activate) {
        err = esp_hosted_cp_ota_activate(); /* co-processor v2.6 and later */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co-processor update failed: %s", esp_err_to_name(err));
        alarm_service_firmware_event(ALARM_FW_CP_FAILED, &s_src, s_user);
        panel_ota_set_progress(PANEL_OTA_FAILED, -1, "The Wi-Fi module update failed");
        panel_ota_release();
        return;
    }
    /* The C6 keeps its old image until the panel restarts: that waits for an idle system
     * (OTA-004) and the machinery stays claimed until then. */
    ESP_LOGW(TAG, "co-processor image sent: the panel restarts once everything is quiet");
    panel_ota_cp_sent(&s_src, s_user);
#else
    panel_ota_set_progress(PANEL_OTA_FAILED, -1, "This panel has no updatable Wi-Fi module");
    panel_ota_release();
#endif
}

esp_err_t panel_ota_cp_update(const alarm_source_t *src, uint8_t user, bool force, char *why,
                              size_t why_len)
{
    char running[24] = "";
    const char *text = NULL;
    if (s_part == NULL || !read_image(s_part, &s_img)) {
        text = "No Wi-Fi module image on the panel";
    } else if (!cp_version(running, sizeof(running))) {
        text = "The Wi-Fi module isn't answering";
    } else if (!force && same_version(running, s_img.version)) {
        text = "The Wi-Fi module already runs this version";
    } else if (!panel_ota_claim()) {
        text = "Another update is running";
    }
    if (text != NULL) {
        if (why != NULL && why_len > 0u) {
            snprintf(why, why_len, "%s", text);
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_src = src != NULL ? *src : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    s_user = user;
    panel_ota_cp_started(s_img.version, running);
    s_requested = true;
    panel_ota_task_notify_cp();
    return ESP_OK;
}

void panel_ota_cp_run(void)
{
    if (!s_requested) {
        return;
    }
    const char *why = "";
    if (!panel_ota_system_idle(&why)) {
        panel_ota_set_progress(PANEL_OTA_WAITING, -1, NULL);
        return; /* panel_ota_cp_tick() tries again */
    }
    s_requested = false;
    run_update();
}

/* OTA-007: an image in cp_fw that differs from what the C6 runs is installed once, when the
 * system is idle. A failed attempt isn't repeated automatically ("c6 update" retries). */
void panel_ota_cp_tick(void)
{
    if (s_requested) {
        panel_ota_cp_run();
        return;
    }
#if CONFIG_PANEL_OTA_CP_AUTO
    if (s_auto_armed || !s_img.valid) {
        return;
    }
    char running[24];
    if (!cp_version(running, sizeof(running))) {
        return; /* co-processor not up yet */
    }
    s_auto_armed = true; /* decided once per boot */
    if (same_version(running, s_img.version)) {
        return;
    }
    char tried[24] = "";
    size_t len = sizeof(tried);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    const bool again = nvs_get_str(h, "cp_try", tried, &len) == ESP_OK &&
                       strcmp(tried, s_img.version) == 0;
    if (!again) {
        nvs_set_str(h, "cp_try", s_img.version);
        nvs_commit(h);
    }
    nvs_close(h);
    if (again) {
        ESP_LOGW(TAG, "co-processor %s, image %s: an earlier attempt failed, not retrying",
                 running, s_img.version);
        return;
    }
    char why[64];
    if (panel_ota_cp_update(NULL, 0, false, why, sizeof(why)) != ESP_OK) {
        ESP_LOGW(TAG, "automatic co-processor update: %s", why);
    }
#endif
}
