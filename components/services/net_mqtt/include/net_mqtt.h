/*
 * net_mqtt.h - the panel's MQTT client (docs/features/10): TLS to a broker, availability (LWT),
 * retained partition / zone / trouble state, reportable events from the event log with
 * store-and-forward, commands, Home Assistant discovery, the comm_fail trouble and a heartbeat.
 *
 * Messages come from components/core/alarm_proto; the connection is espressif/mqtt. Settings and the
 * delivery position live in NVS ("mqtt"). One task owns the client; the API is thread-safe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_service.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_MQTT_TLS_OFF = 0, /* plain MQTT: home network only (MQTT-002) */
    NET_MQTT_TLS_PUBLIC,  /* TLS, verified with the ESP-IDF certificate bundle */
    NET_MQTT_TLS_OWN_CA,  /* TLS, verified with a CA loaded with net_mqtt_set_ca() */
} net_mqtt_tls_t;

typedef struct {
    bool enabled;
    char host[64];
    uint16_t port;     /* 0 = 1883, or 8883 with TLS */
    char user[64];
    char password[64]; /* net_mqtt_get_settings() leaves it empty */
    bool password_set; /* net_mqtt_get_settings(): a password is stored */
    uint8_t tls;       /* net_mqtt_tls_t */
    bool discovery;    /* Home Assistant discovery (default on) */
    bool commands;     /* accept commands (default on; off = read-only, MQTT-014) */
    bool allow_panic;  /* panic / HA "trigger" commands (default off; console only) */
    char prefix[32];   /* topic prefix, default "alarmpanel" */
    char panel_id[17]; /* "" = last 6 hex digits of the MAC */
} net_mqtt_settings_t;

typedef enum {
    NET_MQTT_OFF = 0,
    NET_MQTT_NO_NETWORK,
    NET_MQTT_CONNECTING,
    NET_MQTT_CONNECTED,
    NET_MQTT_FAILED, /* see fail; the client keeps trying */
} net_mqtt_state_t;

typedef enum {
    NET_MQTT_FAIL_NONE = 0,
    NET_MQTT_FAIL_UNREACHABLE,
    NET_MQTT_FAIL_AUTH,
    NET_MQTT_FAIL_TLS,
    NET_MQTT_FAIL_OTHER,
} net_mqtt_fail_t;

typedef struct {
    net_mqtt_state_t state;
    net_mqtt_fail_t fail;
    bool waiting;       /* reportable events not yet delivered */
    uint32_t sent;      /* events delivered since start */
    char broker[80];    /* "host:port" */
    char panel_id[17];  /* in use */
    bool ca_loaded;
} net_mqtt_status_t;

/* Listener feeding the client (state changes, reports). Register before alarm_service_start(). */
const alarm_listener_t *net_mqtt_listener(void);

/* The hardware model named in Home Assistant discovery (the app passes the board's; up to
 * 47 characters; a literal or the board description: it is kept, not copied). Call before
 * net_mqtt_start(); "RoboAlarms panel" when never set. */
void net_mqtt_set_model(const char *model);

/* Loads the settings and starts the client task. Call after alarm_service_start(),
 * panel_log_start() and net_wifi_start(). */
esp_err_t net_mqtt_start(void);

void net_mqtt_get_settings(net_mqtt_settings_t *out);

/* Validates, stores and applies (the connection restarts). The password is replaced only with
 * change_password. Returns ESP_ERR_INVALID_ARG with a reason in why (may be NULL). */
esp_err_t net_mqtt_set_settings(const net_mqtt_settings_t *in, bool change_password, char *why,
                                size_t why_len);

void net_mqtt_get_status(net_mqtt_status_t *out);

/* PEM certificate authority for NET_MQTT_TLS_OWN_CA; NULL or "" removes it. */
esp_err_t net_mqtt_set_ca(const char *pem);

/* HAI-010 (docs/features/23): while the local Home Assistant link is paired, discovery is
 * suppressed and retracted so the panel doesn't appear twice. MQTT publishing for other
 * systems stays as configured. Safe from any task. */
void net_mqtt_suppress_discovery(bool suppressed);

/* Registers the "mqtt" service console command (after the console started). */
esp_err_t net_mqtt_console_register(void);

/* ---- firmware update entity (docs/features/13 OTA-010) ------------------------------------------ */

typedef struct {
    char installed[32];
    char latest[32];       /* newest release known, "" = unknown (shown as installed) */
    char release_url[192]; /* "" = none */
    bool in_progress;
    int percent;           /* 0..100 while an image is written, -1 = none */
} net_mqtt_fw_t;

typedef struct {
    void *ctx;
    void (*get)(void *ctx, net_mqtt_fw_t *out);
    esp_err_t (*install)(void *ctx); /* "install" pressed in Home Assistant */
} net_mqtt_fw_port_t;

/* Adds the Home Assistant update entity. Call before net_mqtt_start(). */
void net_mqtt_set_firmware(const net_mqtt_fw_port_t *port);

/* The firmware state changed (any task). */
void net_mqtt_firmware_changed(void);

/* The Z-Wave devices the panel switches, for Home Assistant (docs/features/10 MQTT-018..020,
 * docs/features/36 ZW-036). The owner (zwave_service, through panel_app) lists them and takes
 * their commands; this client announces, publishes and retracts them. Optional. */
typedef struct {
    void *ctx;
    /* Every output as Home Assistant sees it (proto_zw_device_t), how many. */
    size_t (*list)(void *ctx, void *out, size_t max);
    /* A command for `key`: a level 0..100 (a garage: 0 = close), and when it arrived
     * (alarm_service_now()). false: not taken (unknown, busy, too old, the radio away). */
    bool (*command)(void *ctx, const char *key, int level, uint64_t received_ms);
} net_mqtt_zw_port_t;

/* Call before net_mqtt_start(). */
void net_mqtt_set_zwave(const net_mqtt_zw_port_t *port);

/* The Zigbee network for Home Assistant (docs/features/10 MQTT-021, MQTT-022; docs/features/20
 * devices.md §15, ZB-026). The owner (zigbee_service, through panel_app) answers; this client
 * publishes ~/zigbee/state (retained) when it changes - at most once a second, and again with
 * every full republish - announces its diagnostic sensor (retracted with the rest) and adds each
 * Zigbee-fed zone's battery and signal to that zone's state (proto_zone_state_diag). Optional;
 * every call comes from the MQTT task. */
typedef struct {
    uint8_t radio;      /* ui_zb_radio_t, the same numbers as proto_zb_radio_t */
    uint8_t channel;    /* 11..26; 0: no network */
    uint16_t devices;
    uint16_t not_heard;
    bool jammed;
} net_mqtt_zb_state_t;

typedef struct {
    uint16_t zone;
    uint8_t battery_pct; /* 0..100; over 100 (0xFF): not known, left out */
    uint8_t signal;      /* ui_zb_signal_t, the same numbers as proto_signal_t; 0 left out */
} net_mqtt_zb_diag_t;

typedef struct {
    void *ctx;
    /* The network now; false: no Zigbee on this panel (nothing published or announced). */
    bool (*state)(void *ctx, net_mqtt_zb_state_t *out);
    /* Each zone a Zigbee device feeds, by zone, at most max. */
    size_t (*zone_diags)(void *ctx, net_mqtt_zb_diag_t *out, size_t max);
    /* Optional: counts up whenever what the devices say may have changed (their battery and
     * signal are then read again). NULL: read with the zone states only. */
    uint32_t (*changes)(void *ctx);
} net_mqtt_zb_port_t;

/* Call before net_mqtt_start(). */
void net_mqtt_set_zigbee(const net_mqtt_zb_port_t *port);

#ifdef __cplusplus
}
#endif
