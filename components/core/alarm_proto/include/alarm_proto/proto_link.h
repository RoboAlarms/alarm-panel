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
#include "alarm_proto/proto_prefs.h"
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
    PROTO_LINK_IN_SETTINGS_SET, /* hand the whole frame to proto_link_parse_settings_set
                                 * (HAI-017) */
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

/* ---- panel settings and keypads (protocol.md "Panel settings and keypads"; docs/features/11
 * "Smart Home and Home Assistant on keypads"; HAI-016..019). The master's own Screen and sound
 * settings and its link keypads', relayed by the master: a keypad runs no link of its own. The
 * keys and units are proto_prefs.h's. ---------------------------------------------------------- */

/* settings_result's error for the whole request (external contract; "" when none). */
#define PROTO_LINK_SET_E_NOT_FOUND   "not_found"   /* no panel with that id: the master or a keypad */
#define PROTO_LINK_SET_E_OFFLINE     "offline"     /* the keypad isn't heard now */
#define PROTO_LINK_SET_E_UNSUPPORTED "unsupported" /* the keypad takes no settings (no PREFS) */
#define PROTO_LINK_SET_E_BUSY        "busy"        /* no room or no turn: nothing was applied */
#define PROTO_LINK_SET_E_TIMEOUT     "timeout"     /* sent, never answered: not known */
#define PROTO_LINK_SET_E_BAD         "bad"         /* the request itself (proto_prefs_json_parse) */

/* A panel's voice: the panel link's PREFS numbers (docs/features/11 "Frozen values"). JSON
 * "ready", "loading", "none"; null for one not known (and any other number). */
#define PROTO_LINK_VOICE_UNKNOWN 0u
#define PROTO_LINK_VOICE_READY   1u
#define PROTO_LINK_VOICE_LOADING 2u
#define PROTO_LINK_VOICE_NONE    3u

#define PROTO_LINK_KEYPADS_MAX      8u    /* link keypads: device numbers 1..8 (docs/features/11
                                           * LINK-001) */
#define PROTO_LINK_NAME_LEN         48u   /* a keypad's name: 47 bytes + NUL (PLINK_NAME_MAX) */
#define PROTO_LINK_FW_LEN           32u   /* a keypad's firmware: 31 bytes + NUL (PLINK_FW_MAX) */
#define PROTO_LINK_KEYPADS_JSON_MAX 8192u /* proto_link_keypads, 8 keypads at their longest
                                           * (host-tested) */

/* One link keypad as the master knows it. */
typedef struct {
    uint8_t device;                 /* 1..8 */
    char id[PROTO_PANEL_ID_LEN];    /* its own panel id, 6 lowercase hex; "" until it reports one
                                     * (null) */
    char name[PROTO_LINK_NAME_LEN]; /* the master's name for it */
    uint8_t home;                   /* its home partition 1..8; 0 = not known (null) */
    bool online;                    /* a live session heard in the last 30 s */
    char fw[PROTO_LINK_FW_LEN];     /* its firmware version, "" = not known */
    int8_t rssi;                    /* dBm; 0 or more = not known (null) */
    bool reported;                  /* its settings report came (PREFS): settings, can and voice
                                     * below; until then settings null, can [] and voice null */
    proto_prefs_values_t settings;  /* what it reported (settings.have: the fields it sent) */
    proto_prefs_mask_t can;         /* the fields it accepts */
    uint8_t voice;                  /* PROTO_LINK_VOICE_* */
} proto_link_keypad_t;

/* {"t":"settings","values":{...},"can":[...],"voice":...}: the master's own, whole every time.
 * values: values->have's fields (proto_prefs_json_values); can: its keys in id order. */
void proto_link_settings(pj_writer_t *w, const proto_prefs_values_t *values,
                         proto_prefs_mask_t can, uint8_t voice);

/* {"t":"keypads","keypads":[{"device","id","name","home","online","fw","rssi","settings","can",
 * "voice"}, ...]}: the whole list, every time, in the order given. Fits
 * PROTO_LINK_KEYPADS_JSON_MAX with n <= PROTO_LINK_KEYPADS_MAX. */
void proto_link_keypads(pj_writer_t *w, const proto_link_keypad_t *k, size_t n);

/* Before a list is published or searched: an id that isn't 6 lowercase hex characters, the
 * master's own (own_id) or one a lower device number already reports is cleared (published as
 * null, never a settings_set's target; docs/features/11 "Settings over the link"). Returns how
 * many were cleared, for a log line. */
size_t proto_link_keypads_tidy(proto_link_keypad_t *k, size_t n, const char *own_id);

/* A settings_set: {"t":"settings_set","id","target","values":{...}}. */
typedef struct {
    char id[PROTO_LINK_CALL_ID_LEN]; /* echoed in the answer */
    char target[PROTO_PANEL_ID_LEN]; /* a panel id; "" when longer than any (matches none) */
    bool bad;                        /* the whole request is refused "bad", nothing applied:
                                      * "values" missing, named twice, or refused whole by
                                      * proto_prefs_json_parse (then p is empty) */
    proto_prefs_parsed_t p;          /* the fields taken (p.values.have), refused alone (RANGE),
                                      * and the unknown keys (refused "not_allowed") */
} proto_link_settings_set_t;

/* false (close the connection, as for any message missing a required key): damaged JSON, no
 * "id" or one longer than PROTO_LINK_CALL_ID_LEN - 1 bytes, no "target" string. Otherwise true,
 * with out->bad for a request answered "bad". The values are read against every known key; the
 * target's own keys are applied with proto_link_settings_set_accept. */
bool proto_link_parse_settings_set(const char *json, size_t len, proto_link_settings_set_t *out);

/* The target takes only the keys of accept (its "can"): any other field taken is dropped and
 * refused NOT_ALLOWED, as is any other field refused for its range (the target would refuse it
 * first). */
void proto_link_settings_set_accept(proto_link_settings_set_t *s, proto_prefs_mask_t accept);

/* {"t":"settings_result","id","ok","error","refused":{key: word}}. error NULL or "" means none:
 * then refused names req's own refusals (proto_prefs_why_word; its unknown keys "not_allowed")
 * and, for the fields it sent (req->p.values.have), the target's answer why[id - 1] (NULL: none),
 * and ok is true only when nothing is refused. With an error, ok is false and refused is {}. */
void proto_link_settings_result(pj_writer_t *w, const proto_link_settings_set_t *req,
                                const char *error, const uint8_t *why);

/* ---- settings_set's queue on the panel (HAI-019) ------------------------------------------------
 * Every settings_set taken is answered within 7 s: at once when it can't go (bad, not_found,
 * offline, unsupported, busy), else when its target answers, or busy (never sent) or timeout
 * (sent, no answer) once PROTO_LINK_SET_ANSWER_MS have passed. At most PROTO_LINK_SET_QUEUE wait
 * or are in flight, one in flight per target, each target sent at most one a second after a burst
 * of PROTO_LINK_SET_BURST, in the order they came. The caller owns the clock, the sending and the
 * answers; the queue keeps the order, the turns and the deadlines. */
#define PROTO_LINK_SET_TARGETS   9u    /* 0 = the master itself, 1..8 = a link keypad's device */
#define PROTO_LINK_SET_QUEUE     8u    /* requests waiting or in flight, all targets */
#define PROTO_LINK_SET_BURST     3u    /* per target */
#define PROTO_LINK_SET_EVERY_MS  1000u /* per target, after the burst */
#define PROTO_LINK_SET_ANSWER_MS 6500u /* from its arrival: with the link task's 500 ms pass,
                                        * answered within 7 s (protocol.md), inside the client's
                                        * 15 s; past a keypad's own 6 s give-up (LINK-051) */

typedef enum {
    PROTO_LINK_SET_FREE = 0,
    PROTO_LINK_SET_QUEUED, /* waiting for its target's turn */
    PROTO_LINK_SET_FLIGHT, /* handed to its target: its answer comes by ticket */
} proto_link_set_state_t;

typedef struct {
    uint8_t state;        /* proto_link_set_state_t */
    uint8_t target;       /* 0..8 */
    bool withdrawn;       /* the caller's own (ha_link: the master's mailbox was given up on) */
    uint32_t ticket;      /* the request's own: never 0, never reused while it could answer */
    uint32_t handle;      /* the caller's own (ha_link: the master's mailbox ticket) */
    uint64_t received_ms; /* when it came */
    uint64_t sent_ms;     /* when it went to its target (FLIGHT) */
    proto_link_settings_set_t req; /* the id, the fields to send (req.p.values), refusals so far */
} proto_link_set_slot_t;

typedef struct {
    proto_link_set_slot_t slot[PROTO_LINK_SET_QUEUE];
    uint64_t tat[PROTO_LINK_SET_TARGETS]; /* each target's turn: one is due when tat <= now +
                                           * (BURST - 1) * EVERY_MS (a token bucket, as GCRA) */
    uint32_t next_ticket;
} proto_link_setq_t;

void proto_link_setq_init(proto_link_setq_t *q);
/* Drops every request (a new connection: their answers have nowhere to go). The turns and the
 * tickets go on, so a reconnect doesn't buy a new burst and a late answer matches nothing. */
void proto_link_setq_clear(proto_link_setq_t *q);
/* Queues a request for target (0..8): NULL when the queue is full (answer busy at once) or the
 * target is out of range. */
proto_link_set_slot_t *proto_link_setq_add(proto_link_setq_t *q, uint64_t now, uint8_t target,
                                           const proto_link_settings_set_t *req);
/* The oldest request whose target has none in flight and a turn due: now in flight (its turn
 * taken, sent_ms = now). The caller sends it, or answers and frees it when it can't go. NULL
 * when none may go now. */
proto_link_set_slot_t *proto_link_setq_next(proto_link_setq_t *q, uint64_t now);
/* The request in flight with that ticket; NULL for a late or unknown answer (drop it). */
proto_link_set_slot_t *proto_link_setq_find(proto_link_setq_t *q, uint32_t ticket);
/* The oldest request PROTO_LINK_SET_ANSWER_MS old or more: answer it busy when QUEUED (it never
 * went), timeout when in FLIGHT, and free it. NULL when none. */
proto_link_set_slot_t *proto_link_setq_expired(proto_link_setq_t *q, uint64_t now);
void proto_link_setq_free(proto_link_setq_t *q, proto_link_set_slot_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_LINK_H */
