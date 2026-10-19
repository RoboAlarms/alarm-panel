/*
 * ha_link.h - the local Home Assistant link on the panel (docs/features/23, M12): mDNS
 * advertisement, the TLS server on port 6054, pairing confirmed on the panel's screen, and
 * (as M12 grows) state, events and commands for the paired Home Assistant.
 *
 * The protocol lives in alarm_proto (proto_link.h, proto_pair.h, host-tested); this component
 * wires transport (mbedTLS server over lwIP), randomness (esp_fill_random), storage (NVS
 * "halink": the panel's key and certificate, the pinned client) and the screen around it.
 *
 * Pairing is an exception to NET-007 (no listening services), documented in docs/features/15:
 * mutual TLS with pinned certificates after pairing, a 6-digit comparison code during it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto_link.h"
#include "alarm_service.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_LINK_OFF = 0,      /* the integration is switched off on the panel */
    HA_LINK_NO_NETWORK,   /* enabled, waiting for Wi-Fi */
    HA_LINK_READY,        /* advertising, nothing paired yet */
    HA_LINK_PAIRING,      /* the 2-minute window is open (window_s counts down) */
    HA_LINK_PAIRING_CODE, /* both screens show the code: ha_link_pair_decide() */
    HA_LINK_PAIRED_IDLE,  /* paired, Home Assistant not connected right now */
    HA_LINK_CONNECTED,    /* the paired Home Assistant is connected */
} ha_link_state_t;

typedef struct {
    ha_link_state_t state;
    bool enabled;
    bool paired;
    char client[48];   /* the paired (or pairing) client, e.g. "aioroboalarms 0.1.0" */
    char code[7];      /* the 6-digit code while HA_LINK_PAIRING_CODE, else "" */
    uint32_t window_s; /* seconds left of the pairing window, 0 when closed */
    char panel_id[17];
    uint16_t port;
    /* An unpair the panel could not write down: it holds for this session, but a restart
     * would bring the revoked client back. The panel keeps trying until it clears. */
    bool unpair_unsaved;
} ha_link_status_t;

/* Register with alarm_service_add_listener() before alarm_service_start(): state changes
 * and events reach the paired Home Assistant through it (snapshot, delta, event). */
const alarm_listener_t *ha_link_listener(void);

/* The hardware model the panel reports to Home Assistant (the app passes the board's; up to
 * 47 characters; a literal or the board description: it is kept, not copied). Call before
 * ha_link_start(); "RoboAlarms panel" when never set. */
void ha_link_set_model(const char *model);

/* The Z-Wave devices the panel switches (docs/features/36 ZW-036; protocol.md "Z-Wave devices"):
 * the owner lists them (alarm_proto's proto_zw_device_t) and takes their commands, as for MQTT.
 * Optional; call before ha_link_start(). */
typedef struct {
    void *ctx;
    size_t (*list)(void *ctx, void *out, size_t max);
    bool (*command)(void *ctx, const char *key, int level, uint64_t received_ms);
} ha_link_zw_port_t;
void ha_link_set_zwave(const ha_link_zw_port_t *port);

/* ---- panel settings and keypads (docs/features/23 HAI-016..019; protocol.md "Panel settings and
 * keypads"; docs/features/11 "Smart Home and Home Assistant on keypads") -------------------------
 * The master's own Screen and sound settings need nothing from the owner: ha_link reports them
 * (panel_prefs) and hands a settings_set for them to the screen (panel_prefs_remote_submit). The
 * link keypads come through a port (the panel link's service: ha_link must not depend on it). */

/* What a keypad target's set() did with the fields, or how its request ended. */
typedef enum {
    HA_LINK_SET_SENT = 0,    /* set(): on its way; the answer comes by ticket (below) */
    HA_LINK_SET_NOT_FOUND,   /* "not_found": no such keypad (any more) */
    HA_LINK_SET_OFFLINE,     /* "offline": no live session, or not heard for 30 s */
    HA_LINK_SET_UNSUPPORTED, /* "unsupported": no settings over its link (no PREFS) */
    HA_LINK_SET_BUSY,        /* "busy": one already in flight, or no room: nothing applied */
    HA_LINK_SET_TIMEOUT,     /* "timeout": sent, never answered (the link gave up) */
} ha_link_set_t;

typedef struct {
    void *ctx;
    /* The link keypads (at most max, PROTO_LINK_KEYPADS_MAX), each with its own panel id once it
     * reported one, its report and the fields it accepts. Called on the link task about once a
     * second and on each settings_set: a short copy under the owner's lock, never a wait. */
    size_t (*keypads)(void *ctx, proto_link_keypad_t *out, size_t max);
    /* Send keypad `device` (1..8) these fields, each already checked against its range and its
     * accepts (only values->have's). Never blocks: HA_LINK_SET_SENT when taken (then exactly one
     * answer follows: ha_link_settings_done or ha_link_settings_failed with this ticket, from any
     * task), else the refusal now (NOT_FOUND, OFFLINE, UNSUPPORTED, BUSY). */
    ha_link_set_t (*set)(void *ctx, uint8_t device, uint32_t ticket,
                         const proto_prefs_values_t *values);
} ha_link_settings_port_t;

/* Optional (a master without the panel link has no keypads: the list is empty and a keypad
 * target is not_found). Once, at any time (a later call is ignored); the link task takes the port
 * on its next pass. */
void ha_link_set_settings(const ha_link_settings_port_t *port);

/* A keypad's answer to the set() with this ticket: why[id - 1] a PROTO_PREFS_WHY_* for each field
 * refused, 0 for one applied; n entries (a shorter list leaves the rest 0). Any task, never
 * blocks: the answer is queued for the link task (a full queue drops it, and the request is
 * answered "timeout"). An answer for a ticket no longer waiting is dropped. */
void ha_link_settings_done(uint32_t ticket, const uint8_t *why, size_t n);
/* The same request ended without an answer from the keypad (OFFLINE, UNSUPPORTED, BUSY,
 * NOT_FOUND, TIMEOUT): the client gets that error for the whole request. */
void ha_link_settings_failed(uint32_t ticket, ha_link_set_t err);

/* Loads NVS and starts the link task. Call after net_wifi_start(); returns at once. */
esp_err_t ha_link_start(void);

void ha_link_get_status(ha_link_status_t *out);

/* Switch the integration on or off (stored; mDNS and the server follow). The first enable
 * generates the panel's key and certificate (a moment of CPU). The new state takes effect
 * straight away; a non-ESP_OK return means only that it didn't reach flash, so a restart
 * would undo it. */
esp_err_t ha_link_set_enabled(bool on);

/* Open the pairing window (the Settings page sits behind the master or installer code,
 * HAI-003). ESP_ERR_INVALID_STATE when the link is off. */
esp_err_t ha_link_pair_begin(void);

/* The user compared the codes on both screens (only in HA_LINK_PAIRING_CODE). */
void ha_link_pair_decide(bool allow);

/* Close the window without deciding (Cancel, or leaving the page). */
void ha_link_pair_cancel(void);

/* Forget the paired Home Assistant (it gets a repair issue and must pair again).
 *
 * The session is revoked before this returns, whatever happens: the fingerprint leaves RAM,
 * the connection is dropped and mDNS stops saying "paired". The return value says whether the
 * revocation was also written down. ESP_OK means it will survive a restart; anything else
 * means it will not (yet) - the panel keeps trying, and ha_link_get_status() reports
 * unpair_unsaved until it lands. Never tell a user the client is gone for good on anything
 * but ESP_OK. */
esp_err_t ha_link_unpair(void);

/* Console command "ha" (status, on, off, pair, allow, reject, unpair). */
esp_err_t ha_link_console_register(void);

#ifdef __cplusplus
}
#endif
