/*
 * device_source.h - the device endpoint interface (docs/features/24 HCTL-001): where the panel's
 * Devices screen gets lights, locks, thermostats, covers... A source (Home Assistant through
 * ha_link first, the panel's Z-Wave outputs, native Zigbee after M10) implements device_source_ops_t and reports through
 * a dsrc_sink_t; the home_control registry merges them and applies the security classes
 * (HCTL-005). Only home_control talks to the UI.
 *
 * Portable C11 like zone_source.h: no heap, no RTOS, no globals. dev_kind_t and dev_action_t
 * follow docs/features/24's tables; values are stored in events and requests: append, never
 * renumber.
 */
#ifndef DEVICE_SOURCE_H
#define DEVICE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSRC_NONE = 0,
    DSRC_HA = 1,     /* Home Assistant entities the options allow (docs/features/23, HAI-009) */
    DSRC_ZIGBEE = 2, /* native Zigbee devices (after M10) */
    DSRC_ZWAVE = 3,  /* the panel's own Z-Wave outputs (dsrc_zw.h, docs/features/41 phase B) */
} dsrc_kind_t;

typedef enum {
    DEV_NONE = 0,
    DEV_LIGHT = 1,
    DEV_SWITCH,  /* switch, input_boolean, plugs */
    DEV_LOCK,
    DEV_CLIMATE,
    DEV_COVER,   /* covers and garage doors */
    DEV_FAN,
    DEV_VALVE,
    DEV_SCENE,
    DEV_SCRIPT,
    DEV_KIND_COUNT
} dev_kind_t;

typedef enum {
    ACT_NONE = 0,
    ACT_ON = 1,      /* turn_on; value >= 0: brightness / speed percent */
    ACT_OFF,
    ACT_TOGGLE,
    ACT_LOCK,
    ACT_UNLOCK,      /* asks for a user code first (HCTL-005) */
    ACT_OPEN,        /* covers and valves: asks for a user code first */
    ACT_CLOSE,
    ACT_STOP,
    ACT_SET_LEVEL,   /* value: brightness / fan / position percent */
    ACT_SET_TEMP,    /* value: the setpoint in hundredths (21.5 °C = 2150) */
    ACT_SET_MODE,    /* mode: an hvac or preset mode word */
    ACT_RUN,         /* scenes and scripts */
    ACT_COUNT
} dev_action_t;

#define DEV_REF_LEN   96 /* the source's address for a device (an entity_id) incl. NUL */
#define DEV_NAME_LEN  48
#define DEV_AREA_LEN  48
#define DEV_STATE_LEN 24
#define DEV_MODE_LEN  24

/* What a device reports beside its state words (HCTL-002 "attributes"). Zero is "nothing known",
 * so a dev_info_t a source clears with memset says nothing it doesn't know. */
#define DEV_VAL_LEVEL     0x01u /* level is known */
#define DEV_VAL_SETPOINT  0x02u /* setpoint is known */
#define DEV_VAL_TEMP      0x04u /* temp is known */
#define DEV_VAL_LEVEL_SET 0x08u /* the level can be set: a light that has reported a brightness,
                                 * a cover that has reported a position (kept while it is off) */
/* A thermostat's modes it has (dev_values_t.modes), in its source's terms. */
#define DEV_MODE_OFF       0x01u
#define DEV_MODE_HEAT      0x02u
#define DEV_MODE_COOL      0x04u
#define DEV_MODE_AUTO      0x08u
#define DEV_MODE_HEAT_COOL 0x10u /* Home Assistant's "heat_cool": the screen's Auto */
#define DEV_TEMP_MAX       100000 /* |setpoint|, |temp| in hundredths beyond this: not known */
/* What a device can do beyond its kind's basic actions (dev_values_t.caps, HCTL-002). 0 = not
 * known: the screen offers what its kind has, as before (an older integration says nothing). */
#define DEV_CAP_STOP     0x01u /* a cover or a valve stops (ACT_STOP) */
#define DEV_CAP_POSITION 0x02u /* a cover or a valve goes to a position (ACT_SET_LEVEL) */
#define DEV_CAP_SPEED    0x04u /* a fan's speed can be set (ACT_SET_LEVEL); without it, on/off only */
#define DEV_CAP_DIM      0x08u /* a light's brightness can be set */
#define DEV_CAP_KNOWN    0x80u /* the source said what it can: a bit not set can't be done */

/* What a thermostat is doing (dev_values_t.hvac_action), Home Assistant's hvac_action: shown on
 * its tile and sheet (BL-171). Append, never renumber (the same numbers as proto_link.h's
 * PROTO_LINK_HVAC_*). */
typedef enum {
    DEV_HVAC_UNKNOWN = 0,
    DEV_HVAC_OFF,
    DEV_HVAC_HEATING,
    DEV_HVAC_COOLING,
    DEV_HVAC_DRYING,
    DEV_HVAC_IDLE,
    DEV_HVAC_FAN,
    DEV_HVAC_PREHEATING,
    DEV_HVAC_DEFROSTING,
    DEV_HVAC_COUNT
} dev_hvac_t;

typedef struct {
    uint8_t flags;       /* DEV_VAL_* */
    uint8_t level;       /* 0..100 when DEV_VAL_LEVEL: brightness, fan speed, position percent */
    uint8_t modes;       /* DEV_MODE_*, 0 = not known */
    uint8_t hvac_action; /* dev_hvac_t; DEV_HVAC_UNKNOWN = not known */
    int32_t setpoint;    /* when DEV_VAL_SETPOINT: a thermostat's target, hundredths of a degree */
    int32_t temp;        /* when DEV_VAL_TEMP: what it measures now, hundredths of a degree */
    uint32_t since;      /* Unix seconds: since when it has been unavailable, or when a scene
                          * last ran; 0 = not known (HCTL-006) */
    uint8_t caps;        /* DEV_CAP_*, 0 = not known */
} dev_values_t;

/* One device, as its source reports it. */
typedef struct {
    uint8_t source;             /* dsrc_kind_t */
    uint8_t kind;               /* dev_kind_t */
    char ref[DEV_REF_LEN];      /* unique within the source */
    char name[DEV_NAME_LEN];
    char area[DEV_AREA_LEN];
    char state[DEV_STATE_LEN];  /* the source's words ("on", "locked", "21.5") */
    bool online;                /* false: offline, still readable, controls rest (HCTL-006) */
    dev_values_t values;        /* level, setpoint, temperature, modes: what is known */
    uint32_t act_ms;            /* how long an action may take before HCTL-012's backstop calls it
                                 * "timeout": 0 (or less) = HC_ACT_TIMEOUT_MS. A Z-Wave garage's
                                 * whole move, its travel time included, takes longer. RAM only. */
} dev_info_t;

/* Whether the device can do it: known and set, or not known (then its kind decides, as before). */
static inline bool dev_can(const dev_values_t *v, uint8_t cap)
{
    return (v->caps & DEV_CAP_KNOWN) == 0u || (v->caps & cap) != 0u;
}

/* How a source reports back. Calls come on the platform's device lock. */
typedef struct {
    void *ctx;
    /* A device appeared, changed or went away (gone = true). The registry re-reads it. */
    void (*changed)(void *ctx, uint8_t source);
    /* The answer to act() with the same request id (HCTL-006). error "" = done. unknown (only
     * with ok false, HCTL-012): the source sent it on but couldn't confirm it, so it may have
     * happened - Home Assistant's "timeout" (its service call didn't finish in time); false for
     * a refusal made before anything was sent, which says it wasn't done. The source decides,
     * from its own words: the registry never reads the word. */
    void (*result)(void *ctx, uint32_t request_id, bool ok, const char *error, bool unknown);
} dsrc_sink_t;

typedef struct device_source_ops {
    uint8_t kind; /* dsrc_kind_t */
    const char *name;
    bool (*online)(void *ctx);
    /* Every device it has right now. */
    size_t (*list)(void *ctx, dev_info_t *out, size_t max);
    /* Run an action; the result arrives through the sink (or false right away when the
     * device is unknown or the source is offline). value / mode as dev_action_t says. */
    bool (*act)(void *ctx, const char *ref, dev_action_t action, int32_t value,
                const char *mode, uint32_t request_id);
} device_source_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SOURCE_H */
