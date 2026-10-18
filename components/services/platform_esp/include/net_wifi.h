/*
 * net_wifi.h - Wi-Fi station (through the ESP32-C6 co-processor on the P4), SNTP time and
 * time zone, with the matching alarm troubles:
 *   ALARM_TRBL_COPROCESSOR   the Wi-Fi co-processor doesn't answer
 *   ALARM_TRBL_NETWORK       the network is down for NET_WIFI_TROUBLE_DELAY_S while something
 *                            depends on it (net_wifi_set_required: MQTT, LAN keypads)
 *   ALARM_TRBL_CLOCK_NOT_SET from boot until the clock is set: the first time sync, or a
 *                            battery-backed clock module (docs/features/35, panel_clock) that kept
 *                            the time or was set by hand
 *
 * Credentials and time zone live in this device's NVS ("net" namespace), not on the C6.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_WIFI_TROUBLE_DELAY_S 300

typedef enum {
    NET_WIFI_STARTING = 0,   /* waiting for the co-processor */
    NET_WIFI_NO_COPROCESSOR, /* co-processor failed to start */
    NET_WIFI_UNCONFIGURED,   /* no network saved */
    NET_WIFI_CONNECTING,
    NET_WIFI_CONNECTED,
    NET_WIFI_DISCONNECTED,
} net_wifi_state_t;

/* Why the last connection attempt failed. */
typedef enum {
    NET_WIFI_FAIL_NONE = 0,
    NET_WIFI_FAIL_PASSWORD,  /* authentication or handshake failed: most likely the password */
    NET_WIFI_FAIL_NOT_FOUND, /* no access point with that name answered */
    NET_WIFI_FAIL_OTHER,
} net_wifi_fail_t;

typedef struct {
    net_wifi_state_t state;
    char ssid[33];
    char ip[16];
    char mac[18];       /* this panel's Wi-Fi MAC, "" until the radio is up */
    int8_t rssi;
    net_wifi_fail_t fail;
    bool time_synced;
    char coproc_fw[24]; /* Wi-Fi co-processor firmware version, "" = unknown */
    /* The address in use right now, "" while there is none. */
    char mask[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
    bool static_ip;     /* set from net_ip_config_t, not by the router */
} net_wifi_status_t;

/* How the panel gets its address (docs/features/09 NET-011). Dotted IPv4 strings; "" = not set.
 * With use_static false the router's DHCP decides, and only dns1/dns2 are used, and only
 * when they are set (handy for a local resolver). */
typedef struct {
    bool use_static;
    char ip[16];
    char mask[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
} net_ip_config_t;

/* A network found by a scan. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure; /* needs a password */
} net_wifi_ap_t;

#define NET_WIFI_SCAN_MAX 20
#define NET_WIFI_DEFAULT_NTP "pool.ntp.org"

/* Starts the network task (returns immediately). Needs esp_netif_init() and the default
 * event loop. */
esp_err_t net_wifi_start(void);

void net_wifi_get_status(net_wifi_status_t *out);
/* Version reported by the local radio bridge, not an over-the-air peer. */
void net_wifi_coprocessor_version(const char *version);

/* A feature that needs the network (MQTT, the phone notifications, the Home Assistant link)
 * turns this on; only while at least one of them needs it does a lost network become the
 * "network" trouble. Each says which it is, so one switching itself off cannot silence the
 * trouble another still depends on. */
typedef enum {
    NET_WIFI_REQ_MQTT = 1u << 0,
    NET_WIFI_REQ_NOTIFY = 1u << 1,
    NET_WIFI_REQ_HA = 1u << 2,
    NET_WIFI_REQ_LINK = 1u << 3, /* the panel link's keypads (docs/features/11) */
} net_wifi_requester_t;
void net_wifi_set_required(net_wifi_requester_t who, bool required);

/* Wi-Fi power save off (WIFI_PS_NONE) while any of these holds it: the access point then hands
 * frames over at once instead of at the next beacon (100-300 ms). One hold per reason, so one
 * letting go never ends another's; only the network task changes the radio's mode, within a
 * second, and restores the one it found when the last hold goes. */
typedef enum {
    NET_WIFI_LATENCY_REMOTE = 1u << 0, /* remote viewing (also net_wifi_note_interaction) */
    NET_WIFI_LATENCY_LINK = 1u << 1,   /* the panel link: a master with keypads, and a keypad
                                        * (docs/features/11 LINK-024) */
} net_wifi_latency_owner_t;
void net_wifi_hold_low_latency(net_wifi_latency_owner_t owner, bool on);
/* Short-lived low-latency hint for authenticated screen viewing: NET_WIFI_LATENCY_REMOTE's hold
 * for five seconds from the last call. */
void net_wifi_note_interaction(void);

/* The time server runs (the default) or not: a keypad of the panel link takes its time from its
 * master and runs none of its own while paired (docs/features/11 "Roles"). Safe from any task;
 * applied by the network task. */
void net_wifi_set_time_server(bool on);

/* Saves credentials and (re)connects. Empty ssid forgets the network. */
esp_err_t net_wifi_set_credentials(const char *ssid, const char *password);

/* The stored address settings, and saving them (applied at once; ESP_ERR_INVALID_ARG when a
 * fixed address is incomplete or the router isn't on the panel's network). */
void net_wifi_get_ip_config(net_ip_config_t *out);
esp_err_t net_wifi_set_ip_config(const net_ip_config_t *in);
/* Browser changes are provisional for two minutes. A matching confirmation keeps
 * them; timeout or reboot restores the previous network, including credentials. */
esp_err_t net_wifi_trial_credentials(const char *ssid, const char *password, const char *token);
esp_err_t net_wifi_trial_ip(const net_ip_config_t *in, const char *token);
bool net_wifi_trial_confirm(const char *token);
bool net_wifi_trial_pending(void);

/* POSIX TZ string, e.g. "EST5EDT,M3.2.0,M11.1.0". Saved and applied immediately. */
esp_err_t net_wifi_set_timezone(const char *tz);
void net_wifi_get_timezone(char *out, size_t len);

/* Time-server syncs since start-up (each one set the system clock). The clock module's task
 * polls it to write the module after each (docs/features/35). */
uint32_t net_wifi_time_syncs(void);

/* The clock was set by something other than the time server (the clock module, a person):
 * clears ALARM_TRBL_CLOCK_NOT_SET. A clock already set when net_wifi_start() runs never raises
 * it. status.time_synced still means the time server. */
void net_wifi_clock_set(void);

/* Time server (host name or address). Empty restores NET_WIFI_DEFAULT_NTP. */
esp_err_t net_wifi_set_ntp_server(const char *server);
void net_wifi_get_ntp_server(char *out, size_t len);

/* Looks for networks (asynchronous; a connection attempt in progress is paused). Poll
 * net_wifi_scan_results() until *done: strongest first, one entry per name, hidden networks
 * left out. */
esp_err_t net_wifi_scan_start(void);
size_t net_wifi_scan_results(net_wifi_ap_t *out, size_t max, bool *done);
/* LINK-010: briefly stop station connection attempts so the keypad can find its
 * master's radio channel. RAM only; credentials and subsequent retries are kept. */
void net_wifi_radio_window(void);

#ifdef __cplusplus
}
#endif
