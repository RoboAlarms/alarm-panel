/*
 * net_notify.h - pushing notifications to a phone (docs/features/26, /27, /28).
 *
 * One task owns both services. It listens to the engine, asks components/core/notify what to say and
 * whether to say it, and posts the result over HTTPS. Everything that can be decided without a
 * socket lives in components/core/notify and is host-tested; this component is the transport.
 *
 * The event log is the queue, as it is for MQTT (ADR-018): each service keeps its own delivered
 * position in NVS, so an alarm during a Wi-Fi outage still reaches the phone afterwards and
 * survives a restart, and one service failing never holds up the other.
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
    NET_NOTIFY_NTFY = 0,
    NET_NOTIFY_PUSHOVER,
    NET_NOTIFY_PROVIDERS,
} net_notify_provider_t;

typedef enum {
    NET_NOTIFY_OFF = 0,
    NET_NOTIFY_NO_NETWORK,
    NET_NOTIFY_READY, /* on, nothing sent yet */
    NET_NOTIFY_SENDING,
    NET_NOTIFY_OK,
    NET_NOTIFY_FAILED,
    NET_NOTIFY_PAUSED, /* too many today (NOTE-031) */
} net_notify_state_t;

typedef enum {
    NET_NOTIFY_FAIL_NONE = 0,
    NET_NOTIFY_FAIL_UNREACHABLE,
    NET_NOTIFY_FAIL_AUTH,
    NET_NOTIFY_FAIL_TLS,
    NET_NOTIFY_FAIL_QUOTA,
    NET_NOTIFY_FAIL_OTHER,
} net_notify_fail_t;

typedef enum {
    NET_NOTIFY_TLS_PUBLIC = 0, /* the certificate bundle: ntfy.sh, api.pushover.net */
    NET_NOTIFY_TLS_OWN_CA,     /* a self-hosted server's own authority, set in the Panel Manager */
} net_notify_tls_t;

typedef struct {
    net_notify_state_t state;
    net_notify_fail_t fail;
    bool waiting;     /* something is queued for when it can be sent */
    uint32_t sent;    /* delivered since the panel started */
    uint32_t today;   /* toward the daily limit */
    uint32_t limit;   /* the daily limit */
    bool ever_sent;
    uint32_t ago_s;   /* seconds since the last one got through */
    char detail[80];  /* what the service said when it refused */
    int32_t quota_left; /* Pushover: messages left this month, -1 = not told (PUSH-011) */
} net_notify_status_t;

/* ntfy. 'token' is never returned by the getter; 'token_set' says one is stored. */
typedef struct {
    bool enabled;
    bool self_hosted;
    char server[96];
    char topic[64];
    char token[64];
    bool token_set;
    bool ca_loaded;
    uint8_t cats;
    bool silent_too;
} net_notify_ntfy_t;

/* Pushover. Both credentials are the owner's own, and neither is returned by the getter. */
typedef struct {
    bool enabled;
    char user_key[48];
    bool user_key_set;
    char app_token[48];
    bool app_token_set;
    uint8_t cats;
    bool silent_too;
    bool emergency;
    char sound[16];
    char device[26];
} net_notify_pushover_t;

/* Listener feeding the sender. Register before alarm_service_start(). */
const alarm_listener_t *net_notify_listener(void);

/* Loads the settings and starts the task. Call after alarm_service_start(), panel_log_start()
 * and net_wifi_start(). */
esp_err_t net_notify_start(void);

void net_notify_ntfy_get(net_notify_ntfy_t *out);
/* Validates, stores and applies. change_token replaces the stored token (empty clears it).
 * ESP_ERR_INVALID_ARG with a reason in 'why' (may be NULL). */
esp_err_t net_notify_ntfy_set(const net_notify_ntfy_t *in, bool change_token, char *why,
                              size_t why_len);
/* A fresh unguessable topic. The old one stops working: every phone must subscribe again. */
void net_notify_new_topic(char *out, size_t len);

void net_notify_pushover_get(net_notify_pushover_t *out);
esp_err_t net_notify_pushover_set(const net_notify_pushover_t *in, bool change_user,
                                  bool change_token, char *why, size_t why_len);

void net_notify_get_status(uint8_t provider, net_notify_status_t *out);

/* Starts a test notification: one message, past the rate limits, to prove the whole path -
 * the only check that actually reaches the phone (NOTE-023). It returns as soon as the task
 * has been asked, because posting it can take ten seconds and the caller is the UI thread.
 * ESP_ERR_INVALID_ARG with a reason in 'why' when there is no point starting one. */
esp_err_t net_notify_test_start(uint8_t provider, char *why, size_t why_len);

/* The answer to the test that was started - or to the key check a credential change sets
 * off (PUSH-006) - once there is one. false: still trying. */
bool net_notify_test_result(bool *ok, char *why, size_t why_len);

/* PEM certificate authority for a self-hosted ntfy; NULL or "" removes it. */
esp_err_t net_notify_set_ca(const char *pem);

esp_err_t net_notify_console_register(void);

#ifdef __cplusplus
}
#endif
