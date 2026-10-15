/*
 * proto.h - the panel's MQTT messages: topics, JSON payloads, commands and Home Assistant
 * discovery (docs/features/10-MQTT-and-Home-Assistant).
 *
 * Portable C11 like alarm_core: no heap, no globals, no RTOS. Every builder writes one JSON
 * document into a pj_writer_t; check pj_finish() for overflow. Topic names, JSON keys and the
 * string values (alarm_strings.h) are an external contract: never rename, only add.
 */
#ifndef ALARM_PROTO_H
#define ALARM_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_snapshot.h"
#include "alarm_proto/proto_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_PREFIX_DEFAULT "alarmpanel"
#define PROTO_PREFIX_LEN     32  /* bytes incl. NUL */
#define PROTO_PANEL_ID_LEN   17  /* bytes incl. NUL */
#define PROTO_TOPIC_LEN      112 /* longest topic incl. NUL */
#define PROTO_HA_DISCOVERY   "homeassistant"
#define PROTO_CMD_ID_LEN     41  /* request id echoed in results, bytes incl. NUL */
#define PROTO_PAYLOAD_MAX    4096 /* every payload built here fits (host-tested) */

/* Who this panel is: part of every topic and discovery document. */
typedef struct {
    char prefix[PROTO_PREFIX_LEN];     /* topic prefix, default PROTO_PREFIX_DEFAULT */
    char panel_id[PROTO_PANEL_ID_LEN]; /* default: last 6 hex digits of the MAC */
    char fw[32];                       /* firmware version */
    char model[48];                    /* hardware model */
} proto_ident_t;

/* ---- identity and topics ----------------------------------------------------------------- */

/* 1-31 characters: letters, digits, '-', '_', and '/' between words (no MQTT wildcards). */
bool proto_prefix_valid(const char *prefix);
/* 1-16 characters: a-z, 0-9, '-', '_'. */
bool proto_panel_id_valid(const char *panel_id);
/* Last three bytes of the MAC as six lowercase hex digits. */
void proto_panel_id_from_mac(const uint8_t mac[6], char out[PROTO_PANEL_ID_LEN]);

/* "<prefix>/<panel_id><suffix>", e.g. suffix "/availability". false when it doesn't fit. */
bool proto_topic(const proto_ident_t *id, const char *suffix, char *out, size_t cap);
/* ".../partition/<n>/<leaf>" with leaf "status", "state" or "set". */
bool proto_topic_partition(const proto_ident_t *id, uint8_t partition, const char *leaf,
                           char *out, size_t cap);
/* ".../zone/<z>/state" */
bool proto_topic_zone(const proto_ident_t *id, uint16_t zone, char *out, size_t cap);

/* ---- state and events (published) -------------------------------------------------------- */

/* Home Assistant alarm panel state: disarmed, arming, armed_home, armed_away, armed_night,
 * pending or triggered. Silent alarms and duress never show (status.triggered ignores them). */
const char *proto_ha_state(const alarm_partition_status_t *ps);

/* Wall-clock ms as "2026-09-18T20:15:03Z". false (and "") when unknown (<= 0). */
bool proto_iso8601(int64_t epoch_ms, char out[24]);

/* Time of publication, for the countdown and time stamp in the partition status. */
typedef struct {
    alarm_ms_t now;   /* monotonic, same clock as the snapshot */
    int64_t epoch_ms; /* wall clock, 0 = unknown */
} proto_when_t;

/* Partition status JSON (retained on .../partition/<n>/status). when = NULL leaves out
 * delay_remaining_s, walk_test_remaining_s and ts, which change without anything
 * happening (change detection). */
void proto_partition_status(pj_writer_t *w, const alarm_config_t *cfg,
                            const alarm_snapshot_t *snap, uint8_t partition,
                            const proto_when_t *when);

/* Zone state JSON (retained on .../zone/<z>/state). */
void proto_zone_state(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                      uint16_t zone);

/* Troubles JSON (retained on .../troubles): active ones, and restored ones awaiting code + OFF. */
#define PROTO_TROUBLE_ZONES_MAX 16 /* zones listed; "zones_more" counts the rest */
void proto_troubles(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap);

/* Event JSON (.../event). replayed: sent after a reconnect, possibly a second time. */
void proto_event(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev,
                 bool replayed);
/* The same fields into an already-open object (the Home Assistant link's envelope). */
void proto_event_fields(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev,
                        bool replayed);

typedef struct {
    uint64_t uptime_s;
    const char *fw;
    const char *ip;        /* NULL or "" when unknown */
    int rssi;              /* dBm, 0 = unknown */
    uint32_t free_heap_kb;
    uint32_t boot;         /* boot counter */
    uint32_t unsent;       /* events waiting for the broker */
} proto_heartbeat_t;

/* Heartbeat JSON (.../heartbeat). */
void proto_heartbeat(pj_writer_t *w, const proto_heartbeat_t *hb);

/* ---- commands (received) ----------------------------------------------------------------- */

typedef struct {
    char id[PROTO_CMD_ID_LEN]; /* echoed in the result, "" = none */
    char action[16];           /* as received (for the result) */
    alarm_cmd_t cmd;
} proto_cmd_t;

/* Command JSON from .../cmd, e.g.
 *   {"id":"8f2c","action":"arm","level":"away","partition":1,"code":"246810","flags":["silent_exit"]}
 * Actions: arm (level away / stay / home / night, flags no_entry_delay / silent_exit /
 * bypass_faulted), disarm, bypass and unbypass (zone), chime, exit_restart, panic (panic
 * police / silent / fire / medical). partition defaults to 1; code is 1-10 digits or null.
 * Returns ALARM_OK, ALARM_ERR_BAD_ARG (not JSON, unknown action, missing or invalid field) or
 * ALARM_ERR_DISABLED (panic while allow_panic is off). The id is filled in whenever it could
 * be read, so a rejected command still gets its result. Unknown keys are ignored. */
alarm_result_t proto_cmd_parse(const char *json, size_t len, bool allow_panic, proto_cmd_t *out);

/* Result JSON (.../cmd/result): {"id":…,"action":…,"result":"not_ready","user":…,"zone":…}.
 * result names are alarm_result_name(); res may be NULL. */
void proto_cmd_result(pj_writer_t *w, const proto_cmd_t *cmd, alarm_result_t result,
                      const alarm_cmd_result_t *res);
/* The same fields into an already-open object (the Home Assistant link's envelope). */
void proto_cmd_result_fields(pj_writer_t *w, const proto_cmd_t *cmd, alarm_result_t result,
                             const alarm_cmd_result_t *res);

/* Home Assistant command from .../partition/<n>/set: {"action":"ARM_AWAY","code":"1234"} (the
 * discovery's command_template) or a bare "ARM_AWAY". ARM_HOME is Stay; TRIGGER is a police
 * panic, only with allow_trigger (else ALARM_ERR_DISABLED). Other actions: ALARM_ERR_BAD_ARG. */
alarm_result_t proto_ha_cmd_parse(const char *payload, size_t len, uint8_t partition,
                                  bool allow_trigger, alarm_cmd_t *out);

/* ---- Home Assistant discovery ------------------------------------------------------------ */

typedef struct {
    bool commands;         /* false = read-only: Home Assistant can't arm */
    bool arm_without_code; /* options.remote_arm_without_code */
    bool allow_trigger;    /* panic ("trigger") from Home Assistant */
} proto_ha_opts_t;

/* "homeassistant/<component>/<prefix>_<panel_id>/<object>/config" (characters other than
 * letters, digits, '-' and '_' become '_'). Objects: "p<n>", "z<n>", "trouble", "event". */
bool proto_ha_topic(const proto_ident_t *id, const char *component, const char *object,
                    char *out, size_t cap);

/* alarm_control_panel for one partition. */
void proto_ha_panel(pj_writer_t *w, const proto_ident_t *id, const alarm_config_t *cfg,
                    uint8_t partition, const proto_ha_opts_t *opts);
/* binary_sensor for one zone (on = open / faulted). */
void proto_ha_zone(pj_writer_t *w, const proto_ident_t *id, const alarm_config_t *cfg,
                   uint16_t zone);
/* binary_sensor "Trouble" (problem, diagnostic). */
void proto_ha_trouble(pj_writer_t *w, const proto_ident_t *id);
/* event entity: every event published on .../event. */
void proto_ha_event(pj_writer_t *w, const proto_ident_t *id);

/* Home Assistant binary_sensor device_class for a zone's device class, NULL = none. */
const char *proto_ha_device_class(alarm_device_class_t cls);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PROTO_H */
