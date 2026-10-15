/*
 * app_main.c - the CrowPanel Advanced P4 alarm panels (ESP32-P4 + C6): the 7" (rev V1.1) and the
 * boards built on its firmware (the 10.1", rev V1.2), each with its own board component.
 *
 * The start-up every board shares is components/app/panel_app (docs/features/33 step 7). What is
 * these boards' alone is here: the silicon family the image was built for, the STC battery
 * sensing in the `power` report, the Panel Manager's sign-ins and backend, the dark-build UI
 * memory test and remote development (dev_adapter.c, the P4's JPEG encoder). Which board it is
 * comes from board.h (its names in MQTT and Home Assistant) and board_info().
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "alarm_service.h"
#include "board_battery.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_app.h"
#include "panel_ota.h"
#include "panel_web.h"
#include "sdkconfig.h"
#if CONFIG_ALARM_FEATURE_UI
#include "ui_app.h"
#endif
#include "web_login.h"

static const char *TAG = "main";

/* The dark build's UI memory test walks the touch UI: a headless build
 * (CONFIG_ALARM_FEATURE_UI off) has none to walk (S09). */
#define P4_UI_MEMORY_TEST \
    (CONFIG_ALARM_CRYPTO_DIAGNOSTICS && CONFIG_BOARD_BACKLIGHT_FORCE_OFF && CONFIG_ALARM_FEATURE_UI)

static void log_silicon(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    const char *variant = "v1.x";
#else
    const char *variant = "v3.x";
#endif
    ESP_LOGI(TAG, "ESP32-P4 silicon v%d.%d, firmware variant %s", chip.revision / 100,
             chip.revision % 100, variant);
}

/* Panel Manager sign-ins follow the users they belong to (deleted, code changed). */
static void web_listeners(void)
{
    if (web_login_init() != ESP_OK) {
        ESP_LOGE(TAG, "stored browser sign-ins could not be read: new ones can be set");
    }
    ESP_ERROR_CHECK(alarm_service_add_listener(web_login_listener()));
}

static const char *power_word(power_value_t value)
{
    return value == POWER_YES ? "yes" : value == POWER_NO ? "no" : "unknown";
}

/* The console's `power`: what the STC battery sensing reports, raw frame included. */
static int power_report(void)
{
    power_sample_t status;
    if (panel_app_power_status(&status) != ESP_OK) {
        return 1;
    }
    printf("Power source: STC8 I2C 0x2f (cell presence and percent are estimates)\n"
           "  AC present: %s; battery present: %s; battery low: %s\n"
           "  battery mV: %d; percent: %d (-1 = unavailable)\n",
           power_word(status.ac_present), power_word(status.battery_present),
           power_word(status.battery_low), status.battery_mv, status.battery_percent);
    board_battery_diagnostic_t diag;
    board_battery_diagnostic(&diag);
    printf("  charging: %s; I2C: %s; decoded: %s; raw:",
           power_word(status.charging), esp_err_to_name(diag.error), diag.decoded ? "yes" : "no");
    for (size_t i = 0; i < sizeof(diag.raw); ++i) {
        printf(" %02x", diag.raw[i]);
    }
    printf("\n");
    return 0;
}

#if P4_UI_MEMORY_TEST
static int cmd_ui_memory_test(int argc, char **argv)
{
    const unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    if (count == 0u || count > 20u) {
        return 1;
    }
    for (unsigned lap = 0; lap < count; lap++) {
        for (unsigned step = 0; step < 12u; step++) {
            if (!board_ui_lock(1000)) {
                return 1;
            }
            const bool ok = ui_memory_diagnostic_step(step);
            board_ui_unlock();
            if (!ok) {
                printf("UI test refused: must be idle, disarmed and unauthenticated\n");
                return 1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            printf("UI lap=%u step=%u internal=%u PSRAM=%u\n", lap + 1u, step,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
    }
    return 0;
}

#endif

#if CONFIG_BOARD_DISPLAY_DIAG
static int cmd_dispdiag(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "dark") == 0) {
        const int ms = atoi(argv[2]);
        board_display_diag_dark(ms > 0 ? (uint32_t)ms : 1000u, argc >= 4 && strcmp(argv[3], "redraw") == 0);
        return 0;
    }
    board_display_diag_print();
    return 0;
}
#endif

#if P4_UI_MEMORY_TEST || CONFIG_BOARD_DISPLAY_DIAG
static esp_err_t board_console(void)
{
#if P4_UI_MEMORY_TEST
    const esp_console_cmd_t ui_test = {
        .command = "ui_memory_test", .help = "Dark-build page walk; no code entry or edits",
        .hint = "[1..20]", .func = cmd_ui_memory_test,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ui_test), TAG, "UI diagnostic console");
#endif
#if CONFIG_BOARD_DISPLAY_DIAG
    const esp_console_cmd_t diag = {
        .command = "dispdiag", .help = "Display self-check: frame buffer vs drawn, since start; "
                                       "dark: the backlight off for <ms>, then back",
        .hint = "[dark <ms> [redraw]]", .func = cmd_dispdiag,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&diag), TAG, "display diagnostic console");
#endif
    return ESP_OK;
}
#endif

#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
esp_err_t dev_adapter_start(void);

static void remote_development(void)
{
    if (dev_adapter_start() != ESP_OK) ESP_LOGE(TAG, "remote development unavailable");
}
#endif

void app_main(void)
{
    /* The backlight off before anything else: a panic or watchdog restart keeps the PWM's duty,
     * and a start-up that parks for recovery (its settings unreadable) never reaches
     * board_power_init or the display (docs/bugs/B023). Both find it ready. */
    if (board_backlight_init() != ESP_OK) {
        ESP_LOGW(TAG, "backlight not ready yet");
    }
    static const panel_app_config_t cfg = {
        /* The model strings Home Assistant shows for this board (board.h). */
        .mqtt_model = BOARD_MQTT_MODEL,
        .ha_model = BOARD_HA_MODEL,
        /* It protects through its zone sources and is operated on its touchscreen, so both are
         * in the probation mask (R18). A headless build has no screen or touch UI: nothing
         * reports PANEL_OTA_OK_UI or PANEL_OTA_OK_TOUCH there, so it doesn't wait for them (S09). */
#if CONFIG_ALARM_FEATURE_UI
        .health_mask = PANEL_OTA_OK_ENGINE | PANEL_OTA_OK_LOG | PANEL_OTA_OK_UI |
                       PANEL_OTA_OK_SENSORS | PANEL_OTA_OK_TOUCH,
#else
        .health_mask = PANEL_OTA_OK_ENGINE | PANEL_OTA_OK_LOG | PANEL_OTA_OK_SENSORS,
#endif
        .log_chip = log_silicon,
        .before_engine = web_listeners,
#if CONFIG_ALARM_FEATURE_UI
        .extend_backend = panel_web_ui_backend,
#endif
        .power_report = power_report,
#if P4_UI_MEMORY_TEST || CONFIG_BOARD_DISPLAY_DIAG
        .console = board_console,
#endif
#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
        .started = remote_development,
#endif
    };
    panel_app_run(&cfg);
}
