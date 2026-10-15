/*
 * panel_system.c - version information and the factory reset.
 */
#include "panel_system.h"

#include <stdio.h>
#include <string.h>

#include "alarm_service.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "panel_log.h"
#include "sdkconfig.h"

static const char *TAG = "system";

void panel_system_about(panel_about_t *out)
{
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(out->firmware, sizeof(out->firmware), "%s", app->version);
    snprintf(out->built, sizeof(out->built), "%s %s", app->date, app->time);
    snprintf(out->idf, sizeof(out->idf), "%s", app->idf_ver);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
#if CONFIG_IDF_TARGET_ESP32P4
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    const char *image = "v1.x image";
#else
    const char *image = "v3.x image";
#endif
    snprintf(out->chip, sizeof(out->chip), "ESP32-P4 v%d.%d (%s)", chip.revision / 100,
             chip.revision % 100, image);
#else
    snprintf(out->chip, sizeof(out->chip), "%s v%d.%d", CONFIG_IDF_TARGET, chip.revision / 100,
             chip.revision % 100);
#endif
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->heap_internal_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u);
    out->heap_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u);
}

void panel_system_factory_reset(const alarm_source_t *src, uint8_t user, bool erase_log)
{
    ESP_LOGW(TAG, "factory reset%s", erase_log ? ", event log too" : "");
    alarm_service_factory_reset(src, user, erase_log); /* logged, then no more NVS writes */
    panel_log_flush(2000);
    if (erase_log) {
        panel_log_clear();
    }
    const esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erasing NVS failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
