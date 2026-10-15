/*
 * net_check.h - "Check the connection" (docs/features/09 NET-012): one pass over everything the
 * panel needs from the network, in the order it needs it, so a failure points at the step
 * that broke rather than at "no internet".
 *
 *   Wi-Fi     joined the access point
 *   Address   the panel has an address (from the router, or the fixed one)
 *   Router    the router answers a ping
 *   Lookup    a name can be turned into an address
 *   Internet  something out on the internet accepts a connection
 *   Time      the time server answers
 *   Service   the connected service (the MQTT broker), when one is set up
 *
 * The run happens on its own short-lived task; poll net_check_get() for how far it has come.
 * Nothing here changes any setting, and nothing here is needed to protect the house.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_CHECK_WIFI = 0,
    NET_CHECK_ADDRESS,
    NET_CHECK_ROUTER,
    NET_CHECK_DNS,
    NET_CHECK_INTERNET,
    NET_CHECK_TIME,
    NET_CHECK_SERVICE,
    NET_CHECK_COUNT,
} net_check_step_t;

typedef enum {
    NET_CHECK_WAITING = 0, /* its turn hasn't come */
    NET_CHECK_RUNNING,
    NET_CHECK_PASS,
    NET_CHECK_FAIL,
    NET_CHECK_WARN,    /* worked, but worth knowing */
    NET_CHECK_SKIPPED, /* nothing to check, or an earlier step failed */
} net_check_state_t;

typedef struct {
    uint8_t state;                      /* net_check_state_t */
    uint16_t ms;                        /* how long it took, 0 = not measured */
    const char *words;                  /* what it found, or where it went wrong: a pattern with
                                           {0} and {1} (ui_check_words.h); NULL: nothing yet */
    char value[2][48];                  /* what goes in {0} and {1}: ui_check_line() puts the
                                           line together, in English or the screen's words */
} net_check_result_t;

typedef struct {
    bool running;
    bool done; /* a run has finished and the results below are its */
    net_check_result_t steps[NET_CHECK_COUNT];
} net_check_report_t;

/* Where the panel's connected service lives, so the check can try it as well. The MQTT client
 * lives in another component, so the application hands this in (app_main). */
typedef void (*net_check_service_fn)(void *ctx, bool *enabled, char *host, size_t host_len,
                                     uint16_t *port);

/* Creates the lock. Call once at start-up, before anything may ask for a check. */
esp_err_t net_check_init(void);
void net_check_set_service(net_check_service_fn fn, void *ctx);

/* Starts a run. ESP_ERR_INVALID_STATE while one is already going (or before net_check_init). */
esp_err_t net_check_start(void);

/* How the current or last run is doing. */
void net_check_get(net_check_report_t *out);

#ifdef __cplusplus
}
#endif
