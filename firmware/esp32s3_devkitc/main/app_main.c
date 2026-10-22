/*
 * app_main.c - the ESP32-S3-DevKitC-1 (N16R8) as a headless alarm panel (docs/features/42).
 *
 * The start-up every board shares is components/app/panel_app (docs/features/33 step 7). What is
 * this board's alone is here: the Panel Manager and developer access without a screen to show
 * or press (no remote keypad), the Panel Manager's sign-ins, and the BOOT button for setup
 * recovery.
 */
#include "board.h"
#include "dev_remote.h"
#include "esp_log.h"
#include "panel_app.h"
#include "panel_ota.h"
#include "panel_web.h"
#include "alarm_service.h"
#include <stdio.h>
#include "sdkconfig.h"
#include "web_login.h"

static const char *TAG = "main";

/* Panel Manager sign-ins follow the users they belong to (deleted, code changed). */
static void web_listeners(void)
{
    if (web_login_init() != ESP_OK) {
        ESP_LOGE(TAG, "stored browser sign-ins could not be read: new ones can be set");
    }
    ESP_ERROR_CHECK(alarm_service_add_listener(web_login_listener()));
}

static void metrics(char *out, size_t cap)
{
    snprintf(out, cap, "{\"headless\":true}");
}

/* Developer access and the Panel Manager, with no screen: no frames, presses or video. */
static void web_start(void)
{
    const dev_remote_adapter_t a = {.board = board_info()->id,
                                    .metrics = metrics,
                                    .browser_grant = panel_web_issue_grant};
    if (dev_remote_start(&a) != ESP_OK) {
        ESP_LOGE(TAG, "developer access unavailable");
    }
    if (panel_web_start(&a) != ESP_OK) {
        ESP_LOGE(TAG, "Panel Manager unavailable");
        return;
    }
    /* BOOT held five seconds and let go: setup recovery (docs/features/31 headless-setup.md). */
    panel_web_setup_button(board_boot_button_down);
}

void app_main(void)
{
    static const panel_app_config_t cfg = {
        /* It protects through its zone sources; there is no screen or touch to wait for (R18). */
        .health_mask = PANEL_OTA_OK_ENGINE | PANEL_OTA_OK_LOG | PANEL_OTA_OK_SENSORS,
        /* As a keypad of the panel link (docs/features/11 LINK-055): no screen, so what it gives
         * its master is its wired terminals; the link is what must keep working. */
        .keypad_health_mask = PANEL_OTA_OK_LINK,
        .before_engine = web_listeners,
        .started = web_start,
    };
    panel_app_run(&cfg);
}
