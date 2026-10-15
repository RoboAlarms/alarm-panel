/*
 * zone_hub.h - binds sensors to zones (docs/features/22). The hub owns the binding table, maps each
 * source's states to zone events, runs the grace timers that turn an unreachable sensor into
 * CHECK, and hands zone events to the platform through its port. The alarm core never learns
 * where a zone's sensor is (ZSRC-001).
 *
 *   static zone_hub_t hub;                        // caller owns storage
 *   zone_hub_init(&hub, &port);
 *   zone_hub_decode(&hub, blob, len);             // stored bindings (optional)
 *   zone_hub_register(&hub, &wired_ops, &wired, now);
 *   zone_hub_start(&hub, now);                    // sources start watching their sensors
 *   ... sources report through their sink: zone_hub_input / _event / _reachable / _link
 *   ... call zone_hub_tick(&hub, now) at zone_hub_next_deadline(&hub)
 *
 * Portable C11: no heap, no RTOS, no globals; not thread-safe (the platform serializes calls).
 * A source may call the input functions from inside its watch() and tick().
 *
 * The binding blob (zone_hub_encode/_decode) is stored by the platform (NVS "zones"/"bind"):
 *   header  "ZBND" | format u8 | compat u8 | header_len u16 | payload_len u32 | crc32 u32
 *   payload items: id u16 | len u8 | value[len]; id = (group << 8) | field
 *   group 0x01, field 0x00 selects an input: zone u16 | role u8; the fields that follow describe
 *   it. Fields whose value is zero are left out (zero is every field's default).
 * Items this firmware doesn't know are kept and written back, so a firmware rollback never
 * loses a newer firmware's bindings (ZSRC-004). Item ids are an external contract: never reuse
 * or renumber one.
 */
#ifndef ZONE_HUB_H
#define ZONE_HUB_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZONE_HUB_MAX_SOURCES
#define ZONE_HUB_MAX_SOURCES 8
#endif
#ifndef ZONE_HUB_MAX_INPUTS
#define ZONE_HUB_MAX_INPUTS (ALARM_MAX_ZONES + 16) /* main inputs + a few tamper/battery links */
#endif
#ifndef ZONE_HUB_KEEP_BYTES
#define ZONE_HUB_KEEP_BYTES 1024 /* blob items from a newer firmware, kept for writing back */
#endif

#define ZONE_HUB_GRACE_DEFAULT_S  60u    /* when neither the binding nor its source sets one */
#define ZONE_HUB_CHECKIN_EVERY_MS 60000u /* an unchanged report counts as a check-in this often */

#define ZONE_HUB_BLOB_FORMAT 1u /* what this firmware writes */
#define ZONE_HUB_BLOB_COMPAT 1u /* newest format this firmware reads */
/* Upper bound of an encoded binding table with the compiled capacities. */
#define ZONE_HUB_BLOB_MAX \
    (16u + ZONE_HUB_MAX_INPUTS * (64u + ZSRC_REF_LEN + ZSRC_STATES_LEN) + ZONE_HUB_KEEP_BYTES)

typedef enum {
    ZONE_HUB_OK = 0,
    ZONE_HUB_ERR_ARG,       /* bad zone number, address or mapping; damaged blob */
    ZONE_HUB_ERR_NO_SOURCE, /* no source of that kind is registered */
    ZONE_HUB_ERR_IN_USE,    /* the sensor already feeds another zone (ZSRC-003) */
    ZONE_HUB_ERR_FULL,      /* no room for more inputs (or sources) */
    ZONE_HUB_ERR_REFUSED,   /* the source has no such sensor */
    ZONE_HUB_ERR_BUSY,      /* platforms: not now (the system is armed); the hub never says it */
} zone_hub_result_t;

/* One sensor input and how it maps. */
typedef struct {
    zsrc_address_t addr; /* kind ZSRC_NONE = not used */
    zsrc_mapping_t map;
} zsrc_input_t;

/* Everything that feeds one zone: its sensor, and optional separate tamper / battery inputs. */
typedef struct {
    zsrc_input_t in[ZSRC_ROLE_COUNT]; /* indexed by zsrc_role_t; in[ZSRC_ROLE_MAIN] required */
} zone_hub_binding_t;

typedef struct {
    void *ctx;
    /* A zone event for the engine: the platform queues it for the alarm task. */
    void (*zone_event)(void *ctx, uint16_t zone, alarm_zone_event_t ev);
    /* The bindings changed (bind, unbind): store zone_hub_encode(). Optional. */
    void (*changed)(void *ctx);
    /* A source's whole link has been down longer than its grace while zones depend on it
     * (ZSRC-015): one supervision trouble for the source, never a CHECK per zone (ZONE-008).
     * The platform raises it as that source's device or system trouble. It clears only once
     * every sensor of the source has reported again or its zone shows CHECK. Optional. */
    void (*source_trouble)(void *ctx, uint8_t kind, bool on);
} zone_hub_port_t;

/* ---- storage (fields are private; use the API) ------------------------------------------- */
#define ZONE_HUB_COND_FAULT   0x01u
#define ZONE_HUB_COND_TROUBLE 0x02u
#define ZONE_HUB_COND_TAMPER  0x04u
#define ZONE_HUB_COND_LOWBAT  0x08u

typedef struct {
    uint16_t zone;          /* 0 = free slot */
    uint8_t role;           /* zsrc_role_t */
    uint8_t cond;           /* ZONE_HUB_COND_* the engine was told */
    bool known;             /* a state arrived (ZONE_HUB_COND_FAULT means something) */
    bool active;            /* mapped state (thresholds need it for their hysteresis) */
    bool raw;               /* the same state in the sensor's own sense, before 'invert':
                             * kept apart from active so a new mapping is never applied twice
                             * when the source reports afresh under it (ZSRC-006) */
    bool unreachable;       /* the source can't reach the sensor */
    bool check;             /* UNREACHABLE went to the engine */
    bool watched;           /* its source watches it */
    bool has_value;
    int32_t value_x100;     /* last value */
    uint64_t unreachable_at;
    uint64_t heard_at;      /* last report, 0 = none yet */
    uint64_t checkin_at;    /* last event sent for it */
    zsrc_input_t in;
} zone_hub_input_t;

typedef struct {
    const zone_source_ops_t *ops;
    void *ctx;
    bool link_down;
    bool trouble;     /* the link has been down long enough: the port was told (ZSRC-015) */
    bool urgent;      /* down with its trouble due at once (a jammed channel, ZB-006) */
    uint64_t link_at; /* when the link last changed */
} zone_hub_source_t;

typedef struct {
    zone_hub_port_t port;
    zone_hub_source_t sources[ZONE_HUB_MAX_SOURCES];
    uint8_t n_sources;
    bool started;
    zone_hub_input_t inputs[ZONE_HUB_MAX_INPUTS];
    uint8_t lost[ALARM_ZONE_BITMAP_BYTES]; /* zones whose stored binding couldn't be read */
    uint16_t keep_len;
    uint16_t keep_lost; /* bytes of unknown items the last decode had no room to keep */
    uint8_t keep[ZONE_HUB_KEEP_BYTES]; /* records: zone u16 | role u8 | raw item */
} zone_hub_t;

/* ---- life cycle ------------------------------------------------------------------------------ */
void zone_hub_init(zone_hub_t *h, const zone_hub_port_t *port);

/* Registers a source (one per kind). After zone_hub_start its sensors are watched at once.
 * A source that can't be used yet (usable() says no: Home Assistant before it connects) counts
 * as link down from here, so a panel that restarts while its bridge is away doesn't look ready
 * (ZSRC-015). ZONE_HUB_ERR_FULL when there is no room, ZONE_HUB_ERR_ARG for a kind already
 * registered. */
zone_hub_result_t zone_hub_register(zone_hub_t *h, const zone_source_ops_t *ops, void *ctx,
                                    uint64_t now);

/* Sources start watching their sensors. Inputs whose source isn't registered (compiled out, or
 * from a newer firmware) count as unreachable: CHECK after the grace (ZSRC-010). */
void zone_hub_start(zone_hub_t *h, uint64_t now);

void zone_hub_tick(zone_hub_t *h, uint64_t now);
/* Earliest time the hub (or a source) needs a tick, UINT64_MAX when idle. */
uint64_t zone_hub_next_deadline(const zone_hub_t *h, uint64_t now);

/* ---- bindings (installer programming) ------------------------------------------------------ */

/* Replaces zone's binding (ZSRC-003: one binding per zone, one zone per sensor). The mapping
 * and state of an unchanged address carry over; conditions a replaced sensor had set are
 * cleared in the engine first. */
zone_hub_result_t zone_hub_bind(zone_hub_t *h, uint16_t zone, const zone_hub_binding_t *b,
                                uint64_t now);
/* The zone no longer has a sensor; its conditions are cleared in the engine. */
void zone_hub_unbind(zone_hub_t *h, uint16_t zone, uint64_t now);
bool zone_hub_get(const zone_hub_t *h, uint16_t zone, zone_hub_binding_t *out);
/* Do a and b name the same sensors, role by role (docs/features/08 voice-first-build.md D07)?
 * Addresses only: a new mapping, response time or grace is the same sensor. NULL, or a binding
 * with no main input, means no sensor. A zone whose answer is false has a new sensor: its
 * spoken-name approval is invalidated before the binding is committed. */
bool zone_hub_same_sensors(const zone_hub_binding_t *a, const zone_hub_binding_t *b);
/* The zone an address feeds, 0 = none. */
uint16_t zone_hub_zone_of(const zone_hub_t *h, const zsrc_address_t *a);

/* The engine forgot these zones' input conditions (new or retyped zones after a configuration
 * change, alarm_zone_config_resets_state): tell it again what the hub knows. zones: bit
 * (zone - 1), ALARM_ZONE_BITMAP_BYTES long. */
void zone_hub_resync(zone_hub_t *h, const uint8_t *zones, uint64_t now);

/* The stored bindings couldn't be read (damaged, or written by a firmware whose format this one
 * doesn't know): these zones have no sensor at all, so they show CHECK until the installer gives
 * them one (ZSRC-010) instead of looking ready while nothing can ever fault them. Cleared per
 * zone by zone_hub_bind / zone_hub_unbind. zones: bit (zone - 1). */
void zone_hub_lost(zone_hub_t *h, const uint8_t *zones, uint64_t now);

/* ---- what sources report (through their zsrc_sink_t) --------------------------------------- */
/* Each returns false when no zone is bound to the address. */
bool zone_hub_input(zone_hub_t *h, const zsrc_address_t *a, const char *state,
                    int32_t value_x100, uint64_t now);
bool zone_hub_event(zone_hub_t *h, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now);
bool zone_hub_reachable(zone_hub_t *h, const zsrc_address_t *a, bool reachable, uint64_t now);
/* While a source's link is down its sensors don't turn CHECK: the link is a device trouble
 * (ZONE-008), raised once for the source through port.source_trouble after its grace, while
 * zones depend on it (ZSRC-015). Nothing the source said before the break can be trusted
 * afterwards, so with the link back every one of its sensors has the grace to report again or
 * its zone shows CHECK; the source's trouble stays until then. */
void zone_hub_link(zone_hub_t *h, uint8_t kind, bool up, uint64_t now);
/* The same as a link going down, with the trouble due at once: the source has already waited
 * as long as its rule says (a Zigbee channel jammed for ZB-006's 30 s), so the grace for a
 * passing outage doesn't apply. It lasts until the link comes back up. */
void zone_hub_link_now(zone_hub_t *h, uint8_t kind, uint64_t now);

/* ---- queries for the zone editor and the console ------------------------------------------ */
typedef struct {
    uint8_t kind;
    uint32_t caps;
    const char *name;
    bool usable;    /* zones can be added with it now (ZSRC-005) */
    bool link_down;
    bool trouble;   /* its link has been down long enough to be a trouble (ZSRC-015) */
} zone_hub_source_info_t;

size_t zone_hub_sources(const zone_hub_t *h, zone_hub_source_info_t *out, size_t max);
const zone_source_ops_t *zone_hub_source_ops(const zone_hub_t *h, uint8_t kind);

typedef struct {
    zone_hub_binding_t binding;
    bool lost;           /* no binding because the stored one couldn't be read (zone_hub_lost) */
    bool source_present; /* the main input's source is registered */
    uint32_t caps;       /* of that sensor: its source's caps_of(), else the source's caps */
    bool link_down;      /* its link is down (a device trouble, not CHECK) */
    bool unreachable;    /* the source can't reach the sensor */
    bool check;          /* ... for longer than the grace: the zone shows CHECK */
    bool known;          /* a state arrived */
    bool active;         /* mapped state of the main input */
    bool has_value;
    int32_t value_x100;
    uint64_t heard_at;   /* last report, 0 = none */
    char address[64];    /* the source's words for the address; "" without the source */
} zone_hub_info_t;

/* false when the zone has no sensor at all; true with an empty binding and out->lost when its
 * stored binding couldn't be read. */
bool zone_hub_info(const zone_hub_t *h, uint16_t zone, zone_hub_info_t *out);

/* Learn mode and candidates of one source (fills zsrc_candidate_t.zone). */
bool zone_hub_learn(zone_hub_t *h, uint8_t kind, bool on, uint64_t now);
size_t zone_hub_candidates(zone_hub_t *h, uint8_t kind, const char *query, uint32_t filter,
                           zsrc_candidate_t *out, size_t max);

/* ---- the binding blob ------------------------------------------------------------------------ */
/* Encodes every binding plus kept items. ZONE_HUB_ERR_ARG when buf is too small. */
zone_hub_result_t zone_hub_encode(const zone_hub_t *h, uint8_t *buf, size_t cap, size_t *len_out);
/* Replaces the bindings (before zone_hub_start). ZONE_HUB_ERR_ARG for a damaged or incompatible
 * blob (the bindings are then empty). Unknown items that don't fit ZONE_HUB_KEEP_BYTES are
 * counted in h->keep_lost (the known bindings still load): don't store over such a blob until
 * the bindings change. */
zone_hub_result_t zone_hub_decode(zone_hub_t *h, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_HUB_H */
