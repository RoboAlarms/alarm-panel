/*
 * proto_zb.h - the Zigbee network's diagnostics over MQTT and in Home Assistant (docs/features/10
 * MQTT-021, MQTT-022; docs/features/20 devices.md §15, ZB-026). Topics, keys, values and ids are
 * an external contract: never rename, only add.
 *
 *   state  ~/zigbee/state  retained
 *          {"radio":"working","channel":15,"devices":5,"not_heard":0,"jammed":false}
 *
 * Home Assistant: one sensor "Zigbee" (object "zigbee", entity_category diagnostic) whose state is
 * `radio` and whose attributes are the whole payload. No entity per device: a zone fed by a Zigbee
 * device carries the device's battery and signal in its own zone state (proto_zone_state_diag),
 * the attributes of the zone's existing binary_sensor; the device's page stays the panel's. The
 * network's join, leave and reset events go out on ~/event like every other event.
 *
 * Portable C11, no heap.
 */
#ifndef PROTO_ZB_H
#define PROTO_ZB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto.h"
#include "alarm_proto/proto_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_ZB_STATE_SUFFIX "/zigbee/state" /* for proto_topic() */
#define PROTO_ZB_OBJECT       "zigbee"        /* proto_ha_topic(id, "sensor", PROTO_ZB_OBJECT, ...) */

/* The radio's state: the same numbers as ui_backend's ui_zb_radio_t (the owner passes it
 * straight through). Its words are the payload's `radio`. Append only. */
typedef enum {
    PROTO_ZB_RADIO_NOT_CONNECTED = 0, /* "not_connected": nothing answered in the socket */
    PROTO_ZB_RADIO_STARTING,          /* "starting" */
    PROTO_ZB_RADIO_WORKING,           /* "working" */
    PROTO_ZB_RADIO_NOT_ANSWERING,     /* "not_answering": it went silent, the panel retries */
    PROTO_ZB_RADIO_PAUSED,            /* "paused": the restart guard stopped it */
    PROTO_ZB_RADIO_SOCKET_ZWAVE,      /* "socket_zwave": the socket is given to the Z-Wave radio */
    PROTO_ZB_RADIO_UNUSABLE,          /* "unusable": its firmware is not one the panel can use */
    PROTO_ZB_RADIO_COUNT
} proto_zb_radio_t;

typedef struct {
    uint8_t radio;      /* proto_zb_radio_t */
    uint8_t channel;    /* 11..26; 0 (or anything else): no network yet, null */
    uint16_t devices;   /* devices in the registry (not endpoints) */
    uint16_t not_heard; /* of them: not heard within their supervision time */
    bool jammed;        /* ZB-006: the channel is drowned out */
} proto_zb_state_t;

/* "working", ...; "unknown" past the table. */
const char *proto_zb_radio_name(uint8_t radio);

/* Its state payload (retained on ~/zigbee/state). */
void proto_zb_state(pj_writer_t *w, const proto_zb_state_t *s);

/* Its discovery document (homeassistant/sensor/<node>/zigbee/config). */
void proto_ha_zb(pj_writer_t *w, const proto_ident_t *id);

/* A sensor's link quality, bucketed: the same numbers as ui_backend's ui_zb_signal_t. Its words
 * are the zone state's `signal`. Append only. */
typedef enum {
    PROTO_SIGNAL_UNKNOWN = 0, /* left out */
    PROTO_SIGNAL_GOOD,        /* "good" */
    PROTO_SIGNAL_FAIR,        /* "fair" */
    PROTO_SIGNAL_WEAK,        /* "weak" */
    PROTO_SIGNAL_COUNT
} proto_signal_t;

#define PROTO_BATTERY_UNKNOWN 0xFFu

/* What the device feeding a zone says about itself. */
typedef struct {
    uint8_t battery; /* percent 0..100; PROTO_BATTERY_UNKNOWN (or over 100): left out */
    uint8_t signal;  /* proto_signal_t; PROTO_SIGNAL_UNKNOWN (or past the table): left out */
} proto_zone_diag_t;

/* "good", "fair", "weak"; NULL for unknown or past the table. */
const char *proto_signal_name(uint8_t signal);

/* The zone state (proto_zone_state) with the device's "battery" (0..100) and "signal" after the
 * keys it always has, each only when known. diag NULL: exactly proto_zone_state. */
void proto_zone_state_diag(pj_writer_t *w, const alarm_config_t *cfg,
                           const alarm_snapshot_t *snap, uint16_t zone,
                           const proto_zone_diag_t *diag);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_ZB_H */
