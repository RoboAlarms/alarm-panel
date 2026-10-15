/*
 * ui_port_esp.h - the touch UI's backend on ESP-IDF (ui_backend_t filled from the panel's
 * services: alarm_service, panel_log, net_wifi, net_check, panel_prefs, sounder, panel_system,
 * zone_service, panel_storage and, when enabled, net_mqtt, net_notify, ha_link, panel_ota,
 * zwave_service).
 * The browser Panel Manager wraps the same backend.
 */
#pragma once

#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fills everything except backlight (board specific; set it before ui_app_start). */
void ui_port_esp_backend(ui_backend_t *out);

/* This keypad's stored preferences, and whether the first-boot setup still has to run. */
void ui_port_esp_options(ui_options_t *out);

#ifdef __cplusplus
}
#endif
