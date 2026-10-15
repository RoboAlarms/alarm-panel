#pragma once
/*
 * panel_app.h - the panel's start-up, shared by every board (docs/features/33 step 7).
 *
 * A firmware's app_main() fills a panel_app_config_t and calls panel_app_run(). Everything a
 * panel does at start-up happens here in one order for every board: storage and networking
 * stacks, the board's power and audio, the event log and the alarm engine (the panel protects
 * from there on), the zone sources from board_info(), firmware updates, the display and the
 * touch UI, the health heartbeats, the network, the integrations and the console. What only one
 * board has comes in through the hooks below; everything else comes from the board contract
 * (board_api.h) and the feature options (components/services/alarm_features).
 */
#include <stdint.h>

#include "esp_err.h"
#include "power_monitor.h"
#include "sdkconfig.h"
#if CONFIG_ALARM_FEATURE_UI
#include "ui_backend/ui_backend.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The hardware model the integrations name (NULL: board_name()). Home Assistant shows
     * them, so a board keeps the strings it has always sent. */
    const char *mqtt_model; /* MQTT discovery */
    const char *ha_model;   /* the Home Assistant link */

    /* The parts that must keep reporting while a new firmware is on probation
     * (PANEL_OTA_OK_*, docs/features/13 OTA-005). */
    uint32_t health_mask;

    /* Optional hooks, NULL when the board has nothing to add. */
    void (*log_chip)(void);         /* the silicon line (default: chip and revision) */
    void (*before_engine)(void);    /* extra alarm listeners, registered just before the
                                     * engine starts (the Panel Manager's sign-ins) */
#if CONFIG_ALARM_FEATURE_UI
    void (*extend_backend)(ui_backend_t *backend); /* wraps the UI backend (the Panel Manager) */
#endif
    int (*power_report)(void);      /* the console's `power` report, returning the command's
                                     * result (default: the generic one) */
    esp_err_t (*console)(void);     /* the board's own console commands */
    void (*started)(void);          /* after everything else (remote development) */
} panel_app_config_t;

/* Runs the start-up. Returns once the panel is running (the tasks carry on), or never when
 * start-up cannot finish (the recovery window restarts the panel, R07). */
void panel_app_run(const panel_app_config_t *cfg);

/* The power status the alarm task keeps (for a board's console commands). */
esp_err_t panel_app_power_status(power_sample_t *out);

#ifdef __cplusplus
}
#endif
