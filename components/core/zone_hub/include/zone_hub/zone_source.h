/*
 * zone_source.h - the zone source interface (docs/features/22): what feeds a zone, kept apart from
 * the alarm core. A source (the simulator, wired terminals; later Zigbee, Home Assistant, RF,
 * keypad inputs) implements zone_source_ops_t and reports its sensors by address through a
 * zsrc_sink_t. Only the zone hub knows which zone a sensor feeds, and only the hub talks to the
 * engine (through its port).
 *
 * Portable C11: no heap, no RTOS, no globals. zsrc_kind_t, zsrc_role_t and zsrc_map_mode_t are
 * stored in the binding blob (zone_hub.h): append new values, never renumber.
 */
#ifndef ZONE_SOURCE_H
#define ZONE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZSRC_NONE = 0,
    ZSRC_SIM = 1,    /* simulator: the service console and tests */
    ZSRC_WIRED = 2,  /* dry contacts on the panel's terminals */
    ZSRC_ZIGBEE = 3, /* Zigbee sensors through an ESP32-H2 (docs/features/20, M10) */
    ZSRC_HA = 4,     /* Home Assistant entities (docs/features/23, M12/M13) */
    ZSRC_RF = 5,     /* 315/345/433 MHz sensors (docs/features/25, M14) */
    ZSRC_KEYPAD = 6, /* a remote keypad's own inputs (docs/features/11, M7) */
    ZSRC_ZWAVE = 7,  /* Z-Wave sensors through a ZAC93 (docs/features/36, zones.md §1) */
} zsrc_kind_t;

/* What a source can promise. The zone editor turns these into words (ZSRC-009). */
#define ZSRC_CAP_LEARN       0x0001u /* finds sensors by listening (pairing, open-and-close) */
#define ZSRC_CAP_SEARCH      0x0002u /* searches a catalog by name */
#define ZSRC_CAP_SUPERVISED  0x0004u /* notices a sensor that stops reporting */
#define ZSRC_CAP_AUTH        0x0008u /* inputs can't be forged: a wire, or authenticated radio */
#define ZSRC_CAP_TAMPER      0x0010u /* reports tamper */
#define ZSRC_CAP_BATTERY     0x0020u /* reports low battery */
#define ZSRC_CAP_OFFLINE_OK  0x0040u /* works without Wi-Fi and without Home Assistant */
#define ZSRC_CAP_LIFE_SAFETY 0x0080u /* trusted for fire, CO and 24-hour panic zones (ZSRC-008) */
#define ZSRC_CAP_TEST        0x0100u /* a test tool, not a sensor (the simulator) */

#ifndef ZSRC_REF_LEN
#define ZSRC_REF_LEN 96 /* text address incl. NUL: a Home Assistant entity_id */
#endif
#ifndef ZSRC_STATES_LEN
#define ZSRC_STATES_LEN 48 /* "open,unlocked" incl. NUL */
#endif
#if ZSRC_REF_LEN < 1 || ZSRC_REF_LEN > 256 || ZSRC_STATES_LEN < 1 || ZSRC_STATES_LEN > 256
#error "ZSRC_REF_LEN and ZSRC_STATES_LEN must be 1..256 (the blob stores lengths in a byte)"
#endif

/* Where a sensor is, in the source's own terms. */
typedef struct {
    uint8_t kind;           /* zsrc_kind_t */
    uint8_t input;          /* input, loop or endpoint on the device (0 = its only one) */
    uint64_t id;            /* simulator input, GPIO, Zigbee IEEE address, RF serial, keypad */
    char ref[ZSRC_REF_LEN]; /* text address (Home Assistant entity_id); "" for the others */
} zsrc_address_t;

/* How a source's states or values become zone events (ZSRC-006). Every field's zero value is
 * its default. */
typedef enum {
    ZSRC_MAP_BINARY = 0, /* active: value != 0, or the state "on", "open", "true" or "1" */
    ZSRC_MAP_ABOVE,      /* active from value >= threshold until value < threshold - hysteresis */
    ZSRC_MAP_BELOW,      /* active from value <= threshold until value > threshold + hysteresis */
    ZSRC_MAP_STATES,     /* active while the state is one of 'states' ("open,unlocked") */
    ZSRC_MAP_COUNT
} zsrc_map_mode_t;

typedef struct {
    uint8_t mode;            /* zsrc_map_mode_t */
    bool invert;             /* swaps active and inactive (a normally open contact) */
    int32_t threshold_x100;  /* ABOVE / BELOW, in hundredths (5 °C = 500) */
    int32_t hysteresis_x100; /* >= 0 */
    char states[ZSRC_STATES_LEN];
    uint16_t grace_s;        /* unreachable this long -> CHECK; 0 = the source's default */
    uint16_t response_ms;    /* a new level must hold this long (wired, ZONE-004); 0 = default */
    uint16_t hold_s;         /* a sensor that only ever says "open" (RF, docs/features/25): its fault
                              * ends this long after its last code; 0 = the source decides */
} zsrc_mapping_t;

/* What an input means for its zone. One zone has one sensor (ZSRC-003), optionally with
 * separate tamper and battery inputs (a Home Assistant entity each). */
typedef enum {
    ZSRC_ROLE_MAIN = 0, /* active = FAULT */
    ZSRC_ROLE_TAMPER,   /* active = TAMPER */
    ZSRC_ROLE_BATTERY,  /* active = LOW_BATTERY */
    ZSRC_ROLE_COUNT
} zsrc_role_t;

/* A sensor offered in the learn and search screens. */
typedef struct {
    zsrc_address_t addr;
    char name[48];        /* "GPIO 4", a device's name */
    char detail[48];      /* where it is: "J7 header", an area */
    char state[24];       /* live state in the source's words ("closed", "21.5"), "" = unknown */
    uint8_t device_class; /* alarm_device_class_t guess; GENERIC when unknown */
    bool changed;         /* changed while learning: most likely the one being tested */
    bool best;            /* the source is sure this is the one being tested (RF-011) */
    bool unusable;        /* the source advises against it for the input being picked; detail
                           * says why (RF-021: a sensor's closed code as its tamper code) */
    uint16_t zone;        /* the zone it already feeds, 0 = free (filled in by the hub) */
} zsrc_candidate_t;

/* How a source reports. The platform's sink adds locking and passes the calls to the hub
 * (zone_hub_input/_event/_reachable/_link); sources never see zones or the engine. 'now' is the
 * platform's monotonic time in ms, the same clock the hub ticks with. */
typedef struct {
    void *ctx;
    /* A state or value to map (wired levels, Home Assistant states). state NULL: use value. */
    void (*input)(void *ctx, const zsrc_address_t *a, const char *state, int32_t value_x100,
                  uint64_t now);
    /* An event the source already understands (the simulator, Zigbee IAS bits, RF packets). */
    void (*event)(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now);
    /* The source can (no longer) reach this sensor. */
    void (*reachable)(void *ctx, const zsrc_address_t *a, bool reachable, uint64_t now);
    /* The source's whole link (radio, bridge) is up or down (ZONE-008). */
    void (*link)(void *ctx, uint8_t kind, bool up, uint64_t now);
    /* Optional: the link is down and its trouble is due at once (zone_hub_link_now): a jammed
     * Zigbee channel has already been high for ZB-006's 30 s. NULL: the source uses link(). */
    void (*link_now)(void *ctx, uint8_t kind, uint64_t now);
} zsrc_sink_t;

/* The interface a source implements (C's version of an interface: function pointers plus a
 * context). Only kind, caps and name are required. Calls come from the hub with the platform's
 * lock held; a source may report through its sink from inside watch and tick. */
typedef struct zone_source_ops {
    uint8_t kind;       /* zsrc_kind_t */
    uint32_t caps;      /* ZSRC_CAP_* */
    const char *name;   /* for logs and the console; the UI has its own words per kind */
    uint16_t grace_s;   /* how long a sensor may be unreachable before CHECK; 0 = hub default */
    /* Zones can be added with it right now (ZSRC-005). NULL = always. */
    bool (*usable)(void *ctx);
    /* The source has this sensor (checked before a zone is bound to it). NULL = any address. */
    bool (*valid)(void *ctx, const zsrc_address_t *a);
    /* Start needing a sensor (a zone was bound, or at start), or take a new mapping for it.
     * false: this source has no such sensor. Should report the current state if it knows it. */
    bool (*watch)(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now);
    void (*unwatch)(void *ctx, const zsrc_address_t *a);
    /* Listen for sensors (open and close a door, pair a device). Optional. */
    bool (*learn)(void *ctx, bool on, uint64_t now);
    /* Sensors to offer: learned, found, or every terminal. query / filter for SEARCH sources. */
    size_t (*candidates)(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                         size_t max);
    /* The address in words ("GPIO 4 on the J7 header"). */
    void (*describe)(void *ctx, const zsrc_address_t *a, char *out, size_t len);
    /* Polling and timers; called from zone_hub_tick. next_tick: when it wants the next call,
     * UINT64_MAX for never. */
    void (*tick)(void *ctx, uint64_t now);
    uint64_t (*next_tick)(void *ctx, uint64_t now);
    /* Optional, for a source whose sensors differ (a Z-Wave device with or without Security 2,
     * docs/features/36 zones.md §6): what this sensor can promise. NULL: `caps`. */
    uint32_t (*caps_of)(void *ctx, const zsrc_address_t *a);
    /* Optional: this sensor's grace when its binding names none (a Z-Wave sleeper's check-in +
     * 10 min, network.md §8); 0: `grace_s`; ZSRC_GRACE_NEVER: never CHECK for its silence. */
    uint32_t (*grace_of)(void *ctx, const zsrc_address_t *a);
    /* Optional: how long the source's whole link may be down before it is one trouble of its
     * own (ZSRC-015); 0: `grace_s`. For a source whose sensors' grace is long (a sleeper's
     * check-in) while its radio's failure must be said soon. */
    uint16_t link_grace_s;
    /* Optional: the hub's grouping of the sensors it watches, said right after watch()
     * succeeds: every input of one zone gets the same token, an input of another zone another
     * (0: none). Not stored. A source whose sensors' reports depend on each other keeps it
     * without knowing what it means: another code of a 433 MHz transmitter restores an input
     * unless an input of the same group claims that code (docs/features/25, zsrc_rf.h). */
    void (*group)(void *ctx, const zsrc_address_t *a, uint32_t group);
    /* Optional: how many more sensors the source can watch now (a table of its own). Once
     * started, the hub refuses a binding that needs more (ZONE_HUB_ERR_FULL); an input it
     * already holds that its source had no room for (loaded at start) stays bound, in CHECK
     * after its grace, and is watched again when one of that source's inputs is released or
     * its binding is saved again. NULL: no limit of its own. */
    size_t (*room)(void *ctx);
} zone_source_ops_t;

/* grace_of(): the sensor is never CHECK for being silent (it can't check in). */
#define ZSRC_GRACE_NEVER 0xFFFFFFFFu

/* Two addresses name the same sensor. */
bool zsrc_address_equal(const zsrc_address_t *a, const zsrc_address_t *b);
/* A simple address (kind, input, id; no text). */
zsrc_address_t zsrc_address(uint8_t kind, uint8_t input, uint64_t id);

/* Mapping defaults for a sensor kind (ZSRC-006): contacts, motion, water and smoke are binary;
 * a temperature sensor is "below 5 °C" with 1 °C hysteresis (freeze warning). */
void zsrc_mapping_defaults(zsrc_mapping_t *m, alarm_device_class_t cls);

/* The zone type a sensor kind suggests (docs/features/22 "Adding a zone"): doors and windows
 * perimeter, motion interior follower, smoke and heat fire, CO, water and temperature 24-hour
 * auxiliary, panic buttons 24-hour audible. */
alarm_zone_type_t zsrc_suggest_zone_type(alarm_device_class_t cls);

/* Fire, CO and 24-hour panic zones. */
bool zsrc_zone_type_life_safety(alarm_zone_type_t type);
/* The source may feed this zone type without an installer override (ZSRC-008). */
bool zsrc_trusted_for(uint32_t caps, alarm_zone_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_SOURCE_H */
