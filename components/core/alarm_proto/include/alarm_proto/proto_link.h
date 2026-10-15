/*
 * proto_link.h - the local Home Assistant link, version 1: framing, hello, pairing
 * (docs/features/23, protocol.md next to that README is the specification).
 *
 * Portable C11 like the rest of alarm_proto: no heap, no globals, no RTOS. The message keys,
 * the framing and the pairing derivation are an external contract shared with the
 * aioroboalarms Python client (same golden vectors in both test suites): never rename or
 * reuse, only add.
 */
#ifndef PROTO_LINK_H
#define PROTO_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto.h"
#include "alarm_proto/proto_zw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_LINK_API       1
#define PROTO_LINK_PORT      6054
#define PROTO_LINK_HDR_LEN   4    /* big-endian payload length */
#define PROTO_LINK_FRAME_MAX 65536u /* payload bytes per frame; 0 or more closes the link */
#define PROTO_LINK_SNAPSHOT_MAX 49152u /* a full 128-zone snapshot fits (host-tested) */
#define PROTO_LINK_NONCE_LEN 32
#define PROTO_LINK_FP_LEN    32 /* SHA-256 of a certificate (DER) */
#define PROTO_LINK_CODE_LEN  7  /* six digits + NUL */
#define PROTO_LINK_CLIENT_LEN 48 /* the client's name and version, e.g. "aioroboalarms 0.1.0" */

/* pair_result values (external contract). */
#define PROTO_LINK_R_PAIRED      "paired"
#define PROTO_LINK_R_REJECTED    "rejected"
#define PROTO_LINK_R_TIMEOUT     "timeout"
#define PROTO_LINK_R_MISMATCH    "mismatch"
#define PROTO_LINK_R_NOT_PAIRING "not_pairing"
#define PROTO_LINK_R_BUSY        "busy"
#define PROTO_LINK_R_UNSUPPORTED "unsupported"

/* ---- primitives --------------------------------------------------------------------------- */

/* SHA-256 (FIPS 180-4), host-tested against the standard vectors. */
void proto_sha256(const void *data, size_t len, uint8_t out[32]);

/* Lowercase hex; out holds 2 * len + 1 bytes. */
void proto_hex(const uint8_t *in, size_t len, char *out);
/* Exactly len bytes of hex (either case): false on odd input, other characters, wrong length. */
bool proto_unhex(const char *hex, uint8_t *out, size_t len);

/* ---- framing ------------------------------------------------------------------------------ */

void proto_link_frame_header(uint32_t payload_len, uint8_t out[PROTO_LINK_HDR_LEN]);
/* false when the length is 0 or beyond PROTO_LINK_FRAME_MAX: close the connection. */
bool proto_link_frame_length(const uint8_t hdr[PROTO_LINK_HDR_LEN], uint32_t *payload_len);

/* ---- pairing (HAI-003) -------------------------------------------------------------------- */

/* The 6-digit code both screens show, zero-padded (protocol.md "Code derivation"). */
void proto_link_pair_code(const uint8_t nonce_ha[PROTO_LINK_NONCE_LEN],
                          const uint8_t nonce_panel[PROTO_LINK_NONCE_LEN],
                          const uint8_t fp_ha[PROTO_LINK_FP_LEN],
                          const uint8_t fp_panel[PROTO_LINK_FP_LEN],
                          char out[PROTO_LINK_CODE_LEN]);

/* ---- messages the panel sends ------------------------------------------------------------- */

/* {"t":"hello","api":1,"id":...,"name":...,"model":...,"fw":...,"paired":...}
 * name = the panel's friendly name ("" allowed). */
void proto_link_hello(pj_writer_t *w, const proto_ident_t *id, const char *name, bool paired);
void proto_link_pair_nonce(pj_writer_t *w, const uint8_t nonce[PROTO_LINK_NONCE_LEN]);
void proto_link_pair_result(pj_writer_t *w, const char *result);
void proto_link_pong(pj_writer_t *w);

/* State for the paired Home Assistant (protocol.md "State, events and commands"): the MQTT
 * payload shapes of docs/features/10 inside the link's envelope. The buffer behind w must hold
 * PROTO_LINK_SNAPSHOT_MAX for a snapshot; the others stay small. */
void proto_link_snapshot(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                         const proto_when_t *when);
/* Only what changed: the partitions whose bit is set (bit 0 = partition 1), the zones whose
 * bit is set (bit = zone - 1, ALARM_ZONE_BITMAP_BYTES long; NULL = none) and, when
 * with_troubles, the troubles object. */
void proto_link_delta(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                      const proto_when_t *when, uint8_t partitions, const uint8_t *zones,
                      bool with_troubles);
void proto_link_event(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev,
                      bool replayed);
void proto_link_result(pj_writer_t *w, const proto_cmd_t *cmd, alarm_result_t result,
                       const alarm_cmd_result_t *res);

/* ---- two-way: Home Assistant entities as zones (M13, protocol.md) ------------------------- */

/* The full list of entity_ids the panel's zones are bound to (empty clears the watch). */
void proto_link_watch(pj_writer_t *w, const char *const *ids, size_t n);

#define ZSRC_REF_LEN_LINK 96 /* matches zone_hub's ZSRC_REF_LEN (a Home Assistant entity_id) */

/* What a controllable entity reports beside its state word (protocol.md "the Devices screen":
 * "level", "setpoint", "temp", "modes", "hvac_action", "supports" and "since" on a catalog
 * entity and a state message). Each is optional: absent, null, a fraction, another type or out
 * of range reads as not known. */
#define PROTO_LINK_LEVEL_UNKNOWN (-1)       /* level */
#define PROTO_LINK_TEMP_UNKNOWN  INT32_MIN  /* setpoint, temp */
#define PROTO_LINK_TEMP_LIMIT    100000     /* |hundredths of a degree| beyond this: not known */
#define PROTO_LINK_MODES_MAX     16         /* a "modes" array longer than this: not known */
/* A thermostat's hvac_modes the panel can show, as bits; Home Assistant's other words ("dry",
 * "fan_only", anything later) are ignored. 0 = not known. */
#define PROTO_LINK_MODE_OFF       0x01u /* "off" */
#define PROTO_LINK_MODE_HEAT      0x02u /* "heat" */
#define PROTO_LINK_MODE_COOL      0x04u /* "cool" */
#define PROTO_LINK_MODE_AUTO      0x08u /* "auto" */
#define PROTO_LINK_MODE_HEAT_COOL 0x10u /* "heat_cool" (the panel shows it as its Auto) */
/* "supports": what the entity can do, as bits, with PROTO_LINK_CAP_KNOWN when the array was
 * there (then a bit not set: it can't). 0 = not known (absent, another type, more than
 * PROTO_LINK_SUPPORTS_MAX elements). Unknown words are ignored. */
#define PROTO_LINK_CAP_STOP       0x01u /* "stop" (cover, valve) */
#define PROTO_LINK_CAP_POSITION   0x02u /* "set_position" (cover, valve) */
#define PROTO_LINK_CAP_PERCENTAGE 0x04u /* "set_percentage" (fan) */
#define PROTO_LINK_CAP_BRIGHTNESS 0x08u /* "brightness" (light) */
#define PROTO_LINK_CAP_KNOWN      0x80u
#define PROTO_LINK_SUPPORTS_MAX   16
/* "hvac_action": what a thermostat is doing; 0 = not known. Append, never renumber (the same
 * numbers as device_source.h's dev_hvac_t). */
#define PROTO_LINK_HVAC_OFF        1u
#define PROTO_LINK_HVAC_HEATING    2u
#define PROTO_LINK_HVAC_COOLING    3u
#define PROTO_LINK_HVAC_DRYING     4u
#define PROTO_LINK_HVAC_IDLE       5u
#define PROTO_LINK_HVAC_FAN        6u
#define PROTO_LINK_HVAC_PREHEATING 7u
#define PROTO_LINK_HVAC_DEFROSTING 8u

typedef struct {
    int8_t level;        /* 0..100: a light's brightness, a fan's percentage, a cover's or a
                          * valve's position; PROTO_LINK_LEVEL_UNKNOWN */
    uint8_t modes;       /* PROTO_LINK_MODE_*, 0 = not known */
    uint8_t caps;        /* "supports": PROTO_LINK_CAP_*, 0 = not known */
    uint8_t hvac_action; /* PROTO_LINK_HVAC_*, 0 = not known */
    int32_t setpoint;    /* a thermostat's target, hundredths of a degree in Home Assistant's
                          * unit; PROTO_LINK_TEMP_UNKNOWN */
    int32_t temp;        /* what it measures now, hundredths; PROTO_LINK_TEMP_UNKNOWN */
    uint32_t since;      /* "since", Unix seconds (UTC): unavailable since, or a scene's last
                          * run; 0 = not known */
} proto_link_values_t;

/* Everything not known. */
void proto_link_values_none(proto_link_values_t *v);

/* One entity of a catalog page. */
typedef struct {
    char id[ZSRC_REF_LEN_LINK]; /* the entity_id */
    char name[48];
    char area[48];
    char domain[16];
    char class_[24]; /* the device class in Home Assistant's words, "" = none */
    char state[24];  /* current state, "" = unknown */
    bool zones;      /* may feed a zone (absent in the frame = true) */
    bool control;    /* may be acted on from the Devices screen (absent = false) */
    proto_link_values_t values; /* level, setpoint, temp, modes (each not known when absent) */
} proto_link_entity_t;

/* Walk a catalog frame: init, then next() per entity until it returns false. */
typedef struct {
    pj_reader_t r;
    bool in_entities;
    int64_t page;
    bool more;
} proto_link_catalog_t;

/* false: not a catalog message. page and more are valid afterwards (protocol.md: they come
 * before the entities). */
bool proto_link_catalog_init(proto_link_catalog_t *c, const char *json, size_t len);
/* false at the end of the page (or on damage: the connection's caller closes on pj errors). */
bool proto_link_catalog_next(proto_link_catalog_t *c, proto_link_entity_t *out);

/* A state message. false: not usable (no id). An overlong state comes back "" (unknown).
 * values (NULL: not wanted) gets level, setpoint, temp and modes, each not known when the
 * message doesn't carry it: a state replaces what the last one said. */
bool proto_link_parse_state(const char *json, size_t len, char id[ZSRC_REF_LEN_LINK],
                            char state[24], bool *available, proto_link_values_t *values);

/* ---- the Devices screen (M13 second half, protocol.md) ------------------------------------ */

#define PROTO_LINK_CALL_ID_LEN 12 /* echoed request id incl. NUL */

/* A device action for Home Assistant to run. action: one of the domain table's words.
 * has_value: send 'value' (brightness percent, position, percentage, or temperature in
 * hundredths). mode: NULL or the hvac / preset mode string. */
void proto_link_call(pj_writer_t *w, const char *id, const char *entity, const char *action,
                     bool has_value, int32_t value, const char *mode);

/* Its answer. false: not usable (no id). error is "" when ok. */
bool proto_link_parse_call_result(const char *json, size_t len,
                                  char id[PROTO_LINK_CALL_ID_LEN], bool *ok, char error[24]);

/* ---- messages the panel receives ---------------------------------------------------------- */

typedef enum {
    PROTO_LINK_IN_UNKNOWN = 0, /* valid JSON, a type this version doesn't know: ignore it */
    PROTO_LINK_IN_HELLO,
    PROTO_LINK_IN_PING,
    PROTO_LINK_IN_PAIR_START,
    PROTO_LINK_IN_PAIR_REVEAL,
    PROTO_LINK_IN_COMMAND, /* hand the whole frame to proto_cmd_parse (it skips "t") */
    PROTO_LINK_IN_CATALOG, /* hand the whole frame to proto_link_catalog_init/_next */
    PROTO_LINK_IN_STATE,   /* hand the whole frame to proto_link_parse_state */
    PROTO_LINK_IN_CALL_RESULT, /* hand the whole frame to proto_link_parse_call_result */
    PROTO_LINK_IN_ZWAVE_SET,   /* hand the whole frame to proto_link_parse_zwave_set (ZW-036) */
} proto_link_in_type_t;

typedef struct {
    proto_link_in_type_t type;
    int64_t api;                        /* hello; 0 when missing */
    char client[PROTO_LINK_CLIENT_LEN]; /* hello; "" when missing, truncation refused */
    uint8_t commit[PROTO_LINK_NONCE_LEN]; /* pair_start */
    uint8_t nonce[PROTO_LINK_NONCE_LEN];  /* pair_reveal */
} proto_link_in_t;

/* Parse one received frame. false = not a message at all (bad JSON, no object, no "t", or a
 * known type missing a required field): close the connection. Unknown keys are skipped. */
bool proto_link_parse(const char *json, size_t len, proto_link_in_t *out);

/* ---- the Z-Wave devices the panel switches (docs/features/36 ZW-036; protocol.md "Z-Wave
 * devices"). The same devices and rules as MQTT (proto_zw.h): a garage door closes from Home
 * Assistant, never opens. ------------------------------------------------------------------------ */

/* {"t":"zwave_devices","devices":[{"key","kind":"switch"|"light"|"garage","name","level"
 * (0..100 or null),"online","state" (a garage's)}]} - the whole list, every time. */
void proto_link_zwave_devices(pj_writer_t *w, const proto_zw_device_t *d, size_t n);
/* {"t":"zwave_set","id","key","level"}: false when a field is missing or out of range. */
bool proto_link_parse_zwave_set(const char *json, size_t len, char id[PROTO_LINK_CALL_ID_LEN],
                                char key[PROTO_ZW_KEY_LEN], int *level);
/* {"t":"zwave_result","id","ok","error"} - error "" when ok; "not_allowed" (a garage's open),
 * "not_taken" (unknown, busy, too old, the radio away). */
void proto_link_zwave_result(pj_writer_t *w, const char *id, bool ok, const char *error);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_LINK_H */
