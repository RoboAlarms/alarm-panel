/*
 * dsrc_zw.h - the panel's own Z-Wave outputs as a device source (docs/features/41 DESIGN section 4
 * "Z-Wave (phase B)", docs/features/36 ZW-034/035/036): lights, dimmers, plugs, relays and garage
 * doors the panel itself switches become devices in home_control's registry, so automations (and
 * later the Smart Home screen) can use them.
 *
 * Refs are the outputs' keys, "zw_<home>_<node>_<gen>_<ep>" as proto_zw_key makes them (the same
 * ids MQTT and Home Assistant see). Kinds: a switch or relay is DEV_SWITCH, a dimmer DEV_LIGHT, a
 * garage door DEV_COVER.
 *
 * Commands are POSTED, never run here: the platform's sender queues the command for the Z-Wave
 * task and returns at once (zwave_service_ha_command blocks up to 2 s and must never run under the
 * device lock); the outcome comes back later through dsrc_zw_result, by request id.
 *
 * A garage door is a relay press read back by its tilt sensor (zw_outputs.h, ZW-035): CLOSE posts
 * 0 and OPEN posts 100, and the output layer presses only when the sensor says the door is the
 * other way - never blind. STOP and SET_LEVEL are refused on a cover (a press can't stop a door or
 * set a position), and TOGGLE on anything (a toggle of a garage, or of a state not known, is an
 * open nobody chose). The act interface carries no origin: who may open is decided above it
 * (hc_needs_code's user code for a person, hc_auto_allowed and the installer option for an
 * automation, docs/features/41 AUT-015).
 *
 * Registration order: the platform registers this source with home_control BEFORE Home
 * Assistant's. hc_refresh fills HC_MAX_DEVICES in registration order, so the panel's own outputs
 * (at most DSRC_ZW_MAX) are always listed and Home Assistant's long list takes what is left.
 *
 * Portable C11, caller-owned state, no heap; calls come on the platform's device lock. It needs no
 * Z-Wave header: the platform passes plain values (proto_zw_device_t's fields).
 */
#ifndef DSRC_ZW_H
#define DSRC_ZW_H

#include "home_control/device_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bound: zw_outputs.h's ZWO_OUTPUTS (16) + ZWO_GARAGES (2), copied so home_control needs no
 * Z-Wave component. A garage's relay is never an output of its own, so this is generous. */
#define DSRC_ZW_MAX     (16u + 2u)
/* proto_zw.h's PROTO_ZW_KEY_LEN: a longer key is refused, never cut (two cut keys could match). */
#define DSRC_ZW_KEY_LEN 40u

/* An output's kind: the same numbers as alarm_proto's proto_zw_kind_t, so the platform passes
 * proto_zw_device_t.kind through unchanged. */
typedef enum {
    DSRC_ZW_SWITCH = 1, /* a switch, plug or relay: 0 or 100 */
    DSRC_ZW_LIGHT = 2,  /* a dimmer: 0..100 */
    DSRC_ZW_GARAGE = 3, /* a garage door: close (0) or open (100) */
} dsrc_zw_kind_t;

/* The platform posts "set <key> to <level>" to the Z-Wave task and returns at once. level is
 * 0..100 (0 off / close, 100 on / open; a dimmer's 1..99 is its level; a garage gets 0 or 100 only,
 * and the platform moves it with zwz_panel_garage_move, whose output layer presses only when the
 * sensor says the door is the other way - not through zwz_panel_ha_command, which is Home
 * Assistant's path and refuses an open). true:
 * posted, and the platform answers exactly once later through dsrc_zw_result with the same
 * request_id. false: not posted (the radio is offline, the queue is full) - nothing is answered
 * and home_control fails it at once. It must never block and never call back into this source
 * from inside the call. */
typedef bool (*dsrc_zw_send_fn)(void *ctx, const char *key, int level, uint32_t request_id);

typedef struct {
    dsrc_sink_t sink; /* home_control's (hc_sink) */
    bool link_up;
    dev_info_t devices[DSRC_ZW_MAX];
    size_t n;
    /* dsrc_zw_begin..dsrc_zw_end build the next list here; list() keeps the old one meanwhile */
    bool building;
    dev_info_t staged[DSRC_ZW_MAX];
    size_t n_staged;
    dsrc_zw_send_fn send;
    void *send_ctx;
} dsrc_zw_t;

void dsrc_zw_init(dsrc_zw_t *d, const dsrc_sink_t *sink);
const device_source_ops_t *dsrc_zw_ops(void); /* register with the dsrc_zw_t as ctx */
void dsrc_zw_set_sender(dsrc_zw_t *d, dsrc_zw_send_fn fn, void *ctx);

/* ---- fed by the platform (or a test) ------------------------------------------------------- */

/* The Z-Wave network came up or went down: every device goes offline with it, and comes back
 * online with the platform's next list (begin..end) or state for it. */
void dsrc_zw_link(dsrc_zw_t *d, bool up);
/* A fresh list of outputs: begin, one add per output, end. The list changes (and sink.changed
 * fires) only at end, so a refresh in between still sees the old list. */
void dsrc_zw_begin(dsrc_zw_t *d);
/* One output. kind is dsrc_zw_kind_t (anything else is ignored); level -1 unknown or 0..100;
 * garage_word is its sensor's word ("open", "closed", "unknown"), used for a garage only. act_ms:
 * how long a command to it may take to end (dev_info_t.act_ms; a garage's whole move,
 * zwo_garage_budget_ms), 0 for home_control's own HC_ACT_TIMEOUT_MS. */
void dsrc_zw_add(dsrc_zw_t *d, const char *key, uint8_t kind, const char *name, int level,
                 bool online, const char *garage_word, uint32_t act_ms);
void dsrc_zw_end(dsrc_zw_t *d);
/* An output's state changed (a Report, the panel's own Set, a door's sensor). Unknown keys are
 * ignored. */
void dsrc_zw_state(dsrc_zw_t *d, const char *key, int level, bool online,
                   const char *garage_word);
/* The outcome of a posted command (HCTL-012): straight to home_control's sink. error "" or NULL
 * when ok; a short word otherwise ("failed", "timeout", "busy"...). "timeout" (sent, and not
 * confirmed) is an unknown outcome. Every other word is definite: a refusal made before anything
 * was sent, or "failed" at a garage move's end - pressed, and its sensor says the door didn't
 * get there. */
void dsrc_zw_result(dsrc_zw_t *d, uint32_t request_id, bool ok, const char *error);

/* The level a kind and action send, or -1 when this source refuses the pair. value is
 * the action's (ACT_ON: -1 or 0..100 for a light; ACT_SET_LEVEL: 0..100). A light's ON with -1
 * sends 100: Z-Wave outputs have no "previous level" form (zwo_set takes 0..100). */
int dsrc_zw_level_for(dev_kind_t kind, dev_action_t action, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* DSRC_ZW_H */
