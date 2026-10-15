/*
 * zigbee_service.h - the panel's sensor radio (docs/features/20): the ESP32-H2 module in the
 * CrowPanel's wireless socket, and the Zigbee sensors paired to it.
 *
 * Plugging the module in is meant to be the whole installation (ZB-001). The service owns the
 * socket's UART, brings the radio up - reading which firmware it runs and, when it is not ours,
 * writing ours over the same wire (components/core/zigbee_host) - and then feeds the portable
 * zsrc_zigbee source, which is what the zone hub and the zone editor actually see.
 *
 * Nothing here is on the alarm task: the service has its own, and reaches the hub through
 * zone_service's lock like every other source.
 */
#ifndef ZIGBEE_SERVICE_H
#define ZIGBEE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

#include "alarm/alarm_snapshot.h"
#include "esp_err.h"
#include "ui_backend/ui_backend.h"
#include "zigbee_host/zb_bringup.h"
#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t uart_tx_gpio; /* -> the module's U1RXD (BOARD_RADIO_UART_TX_GPIO) */
    uint8_t uart_rx_gpio; /* <- the module's U1TXD (BOARD_RADIO_UART_RX_GPIO) */
} zigbee_service_config_t;

typedef struct {
    bool enabled;       /* the service is running */
    bool socket_zwave;  /* not started: the socket's UART is used by the Z-Wave radio */
    zb_boot_state_t boot;
    const char *words;  /* one line for Settings and the console (zb_boot_words) */
    uint8_t percent;    /* while the radio is being written */
    char version[ZB_BOOT_VERSION_LEN]; /* what the radio reports, "" = nothing yet */
    bool ready;         /* the Zigbee stack is running on top of it */
    bool jammed;        /* the channel is being drowned out (ZB-006) */
    bool paused;        /* the radio restarted the panel on its last starts: off until retry */
    uint8_t radio_fails; /* starts in a row the radio ended (the restart guard, radio_port.c) */
    bool energy_known;  /* energy_dbm holds a scan of the network's channel */
    int8_t energy_dbm;  /* the latest energy scan (ZB-006) */
    uint8_t devices;    /* paired devices the panel knows */
    uint8_t channel;
    uint16_t pan_id;
} zigbee_service_status_t;

/* Starts the radio service: the socket's UART, the probe and, once the radio answers, the Zigbee
 * stack. Without CONFIG_ALARM_ZSRC_ZIGBEE it returns ESP_ERR_NOT_SUPPORTED without touching the
 * UART: a firmware without Zigbee says nothing about whether a module is plugged in. */
esp_err_t zigbee_service_start(const zigbee_service_config_t *cfg);

/* The portable source, for zone_service to register with the hub. NULL until start(). */
zsrc_zb_t *zigbee_service_source(void);

void zigbee_service_status(zigbee_service_status_t *out);

/* The start-up says whether the Z-Wave radio holds the socket (ADR-047: the rule at run time).
 * Zigbee then does not start; status() answers socket_zwave and says so in words. Call it
 * before or instead of start(); safe from any task. */
void zigbee_service_set_socket_taken(bool by_zwave);

/* Try the radio again now (the console's "zigbee retry", or after a module is plugged in); also
 * lifts the pause after the radio restarted the panel. */
void zigbee_service_retry(void);

/* The console's `zigbee` command (with CONFIG_ALARM_SERVICE_CONSOLE). */
esp_err_t zigbee_service_console_register(void);

/* ---- BL-143 (docs/features/20 devices.md §2-§3) ---- */

/* What the radio side reports - how a device joined, its descriptors and Basic attributes, link
 * quality, check-ins, leaves, cluster commands the source doesn't decode, the end of each tagged
 * request - goes to `fn` (zbz_panel_on_report's shape), on the service's task with the zone lock
 * held. Until one is set the reports are dropped. Call it with the zone lock held, or before
 * start(). Without CONFIG_ALARM_ZSRC_ZIGBEE it does nothing. */
typedef void (*zigbee_service_report_fn)(void *ctx, uint64_t now, const zsrc_zb_report_t *r);
void zigbee_service_set_report_sink(zigbee_service_report_fn fn, void *ctx);

/* Answer `ieee`'s Poll Control check-ins with fast polling for the next `seconds` (0 stops): a
 * sleepy device stays awake for the setup or a page's request. False without a running stack. */
bool zigbee_service_wake_window(uint64_t ieee, uint16_t seconds);

/* Restart the radio (the device page's "Restart the radio"): the stack stops, every waiting
 * request ends NO_RADIO, and the service brings it up again, the stored network kept. False
 * without a running stack. */
bool zigbee_service_restart_radio(void);

/* ---- ZB-M6 (BL-143): the service owns zigbee_zones' zbz_panel (devices.md) ---- */

/* The touch UI's zb_* calls (ui_backend.h, ZB-027), each read under the zone lock. ui_status is
 * false when this panel has no Zigbee (the pages hide); with the socket given to Z-Wave it is true
 * with radio UI_ZB_RADIO_SOCKET_ZWAVE. */
bool zigbee_service_ui_status(ui_zb_status_t *out);
size_t zigbee_service_ui_devices(ui_zb_device_t *out, size_t max);
bool zigbee_service_ui_device(uint16_t id, ui_zb_device_page_t *out);
void zigbee_service_ui_progress(ui_zb_progress_t *out);
size_t zigbee_service_ui_codes(ui_zb_code_t *out, size_t max);
/* A page's request, from the touchscreen: installer mode with every partition disarmed decides
 * `allowed` (zbz_panel_request), RESET's installer_code is checked with the engine first as the
 * local keypad (counted toward its lockout, ZB-017) and never kept; waits up to 2 s for the
 * zigbee task (BUSY when it didn't answer). The caller wipes its copy afterwards. */
ui_zb_req_result_t zigbee_service_ui_request(const ui_zb_request_t *req);
/* The Panel Manager's request (panel_web's web_zigbee.c, devices.md §14): the caller decided
 * `allowed` itself (a signed-in installer, the panel not armed: panel_web's access table) and
 * re-asked the installer's password for RESET (recent_password); the request's installer_code is
 * never read and is wiped. Waits as zigbee_service_ui_request; UNSUPPORTED without Zigbee. */
ui_zb_req_result_t zigbee_service_web_request(const ui_zb_request_t *req, bool allowed);
/* The registry id of the device feeding `zone`, 0 none. */
uint16_t zigbee_service_device_of_zone(uint16_t zone);
/* Counts up whenever something the pages show changed. */
uint32_t zigbee_service_changes(void);

/* The engine's news for the sirens and keypads (ZB-023, ZB-024), handed on by the start-up's
 * own listener (alarm_service's listeners are full): from the alarm task, never blocking. */
void zigbee_service_engine_output(alarm_output_t output, uint8_t partition,
                                  alarm_pattern_t pattern);
void zigbee_service_engine_snapshot(const alarm_snapshot_t *snap);

/* MQTT (devices.md §15, ZB-026): ~/zigbee/state's values (proto_zb.h), false without Zigbee. */
typedef struct {
    uint8_t radio;      /* ui_zb_radio_t (proto_zb_radio_t's numbers) */
    uint8_t channel;    /* 0: no network */
    uint16_t devices;
    uint16_t not_heard;
    bool jammed;
} zigbee_service_mqtt_state_t;
bool zigbee_service_mqtt_state(zigbee_service_mqtt_state_t *out);
/* Each zone a Zigbee device feeds, with that device's battery and signal: at most max, by zone. */
typedef struct {
    uint16_t zone;
    uint8_t battery_pct; /* 0..100, UI_ZB_BATTERY_UNKNOWN */
    uint8_t signal;      /* ui_zb_signal_t (proto_signal_t's numbers) */
} zigbee_service_zone_diag_t;
size_t zigbee_service_zone_diags(zigbee_service_zone_diag_t *out, size_t max);

/* The developer link (docs/features/30, devices.md §12): GET /dev/v1/zigbee writes
 * {"api":1,"radio":{...},"devices":[...],"progress":{...},"codes":[...]} (its length; 0 when it
 * didn't fit cap). POST /dev/v1/zigbee takes {"op":...} as the console does (every partition
 * disarmed; forget and channel need "confirm": true; no network reset - no code travels the
 * network) and writes {"result":"<word>"}; false when the body is not one JSON object, names a
 * field twice, has no known "op", or cap is below ZIGBEE_SERVICE_ANSWER_JSON_MAX. The words only
 * grow (tools/panelctl.py reads them). */
#define ZIGBEE_SERVICE_STATE_JSON_MAX  49152u
#define ZIGBEE_SERVICE_ANSWER_JSON_MAX 32u /* {"result":"bad_address"} */
#define ZIGBEE_SERVICE_BODY_JSON_MAX   384u
size_t zigbee_service_state_json(char *out, size_t cap);
bool zigbee_service_request_json(const char *body, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_SERVICE_H */
