/*
 * dsrc_ha.h - Home Assistant as a device source (docs/features/24 M13): the catalog entries the
 * integration marks control:true (protocol.md) become devices; actions go back as `call`
 * messages, results return by request id. ha_link feeds it on the panel, the tests and the
 * PC preview feed it directly. Portable C11; calls come on the platform's device lock.
 */
#ifndef DSRC_HA_H
#define DSRC_HA_H

#include "home_control/device_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSRC_HA_MAX 64 /* controllable entities kept (the options list should stay sane) */

/* The platform sends {"t":"call"} for this request (ha_link; false: not connected).
 * action / value / mode are already the protocol's words. */
typedef bool (*dsrc_ha_send_fn)(void *ctx, const char *entity, const char *action,
                                bool has_value, int32_t value, const char *mode,
                                uint32_t request_id);

typedef struct {
    dsrc_sink_t sink; /* home_control's (hc_sink) */
    bool link_up;
    dev_info_t devices[DSRC_HA_MAX];
    size_t n;
    /* The lights and covers that have reported a level (DEV_VAL_LEVEL_SET), by FNV-1a of their
     * entity id: kept across catalogs, so a light that is off when the link comes back (Home
     * Assistant then sends no brightness) keeps its slider. Oldest replaced when full; a hash
     * collision at worst offers a slider Home Assistant ignores. */
    uint32_t dimmable[DSRC_HA_MAX];
    uint8_t n_dimmable;
    uint8_t next_dimmable;
    dsrc_ha_send_fn send;
    void *send_ctx;
    uint32_t down_at;     /* the panel's clock (Unix s) when the link went down; 0 up, or not known */
    uint64_t down_was_on; /* bit i: devices[i] was online when the link went down */
} dsrc_ha_t;

_Static_assert(DSRC_HA_MAX <= 64, "down_was_on has a bit per device");

void dsrc_ha_init(dsrc_ha_t *d, const dsrc_sink_t *sink);
const device_source_ops_t *dsrc_ha_ops(void); /* register with the dsrc_ha_t as ctx */
void dsrc_ha_set_sender(dsrc_ha_t *d, dsrc_ha_send_fn fn, void *ctx);

/* ---- fed by ha_link (or a test) ------------------------------------------------------------ */

/* The paired connection came up or went down: devices go offline with it. now_s is the panel's
 * clock (Unix seconds, 0 when not set): while the link is down, a device that was online when it
 * dropped lists now_s as its since ("Offline since", HCTL-006); one already unavailable keeps
 * Home Assistant's time. The time is applied when listed, never stored in a device's values. */
void dsrc_ha_link_at(dsrc_ha_t *d, bool up, uint32_t now_s);
/* dsrc_ha_link_at(d, up, 0): no time known. */
void dsrc_ha_link(dsrc_ha_t *d, bool up);
/* A page-0 catalog starts fresh (the same begin ha_link already does for zones); which lights
 * and covers can be set (DEV_VAL_LEVEL_SET) is remembered across it until dsrc_ha_init. */
void dsrc_ha_catalog_begin(dsrc_ha_t *d);
/* One catalog entity marked control:true. Unknown domains are ignored. values (NULL: none
 * known) as dsrc_ha_state keeps them. */
void dsrc_ha_catalog_add(dsrc_ha_t *d, const char *entity_id, const char *name,
                         const char *area, const char *domain, const char *state,
                         const dev_values_t *values);
/* A state push (the same feed as the zones'). values (NULL: none known) replace what the last
 * push said: only those that fit the kind are kept (a level for a light, a fan, a cover or a
 * valve; a setpoint, a temperature, modes and hvac_action for a thermostat, the last only while
 * available), each within its range; since for every kind; caps only the kind's own DEV_CAP_*
 * (an unavailable device whose push says none keeps its previous ones). DEV_VAL_LEVEL_SET: when
 * caps are known, DEV_CAP_DIM (a light) or DEV_CAP_POSITION (a cover) decides; when not, it
 * stays once a light or a cover has reported a level. */
void dsrc_ha_state(dsrc_ha_t *d, const char *entity_id, const char *state, bool available,
                   const dev_values_t *values);
/* The client's call_result for request_id (protocol.md), to home_control's sink. error "" when
 * ok. HCTL-012: "timeout" - the integration started the service call and it didn't finish in
 * time, so it may have happened - is an unknown outcome; "not_allowed", "unavailable" and
 * "failed" (and any other word) say it wasn't done. */
void dsrc_ha_result(dsrc_ha_t *d, uint32_t request_id, bool ok, const char *error);

/* Home Assistant's domain to a device kind; DEV_NONE when it isn't a device. */
dev_kind_t dsrc_ha_kind(const char *domain);

/* The protocol's action word for a kind and action; NULL when the pair makes no sense.
 * ACT_SET_LEVEL is "turn_on" with a brightness for lights, "set_percentage" for fans and
 * "set_position" for covers; ACT_SET_MODE is "set_hvac_mode" (presets come later). A mode of
 * "auto" goes out as "heat_cool" to a thermostat that has only that (DEV_MODE_HEAT_COOL). */
const char *dsrc_ha_action_word(dev_kind_t kind, dev_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* DSRC_HA_H */
