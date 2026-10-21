/*
 * zwave_service.h - Z-Wave sensors through a Zooz ZAC93 on a UART (docs/features/36, Z8): the
 * panel's own Z-Wave network, its devices as zones.
 *
 * Two tasks. zw_link (small internal stack, above the alarm task) owns the UART: it collects the
 * module's frames, ACKs or NAKs each at once, hands frames, ACK/NAK/CAN bytes and stray text to
 * the zwave task, and writes what the zwave task sends. zwave (8 KB internal stack: it writes
 * NVS; below the zone task) runs the network (components/core/zwave_zones' zwz_panel: zw_net, the
 * bridge to the Z-Wave zone source, the pages' view) with the zone service's lock held, the
 * requests of the touch UI and the console one at a time, the NVS namespace "zwave" and the
 * logged events. The network's state (about 90 KB) sits in PSRAM.
 *
 * Built only with CONFIG_ALARM_ZSRC_ZWAVE: without it the sources compile to nothing and callers
 * guard their calls (docs/features/33). The radio stays off - no pin touched - until a place is
 * chosen (`zwave place`), the zone source is registered either way so that zones bound to Z-Wave
 * devices keep their bindings. Development builds also keep a frame trace of the link for the
 * developer link (docs/features/30).
 */
#ifndef ZWAVE_SERVICE_H
#define ZWAVE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "esp_err.h"
#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the radio is wired this start, from the board's description as the stored place makes
 * it (board_zwave_place_on, board_zwave_reset_on); place NULL: off. */
typedef struct {
    const char *place; /* "socket", "j7"; NULL: no radio */
    int16_t tx, rx;    /* the panel's TX (to the module's RX) and RX */
    int16_t reset;     /* the module's reset line, -1: not wired */
    const char *where; /* in words: "wireless-module socket" */
} zwave_service_config_t;

/* What the console may choose (the app knows the board and the zones; the service only asks).
 * Given before zwave_service_start, and even when the radio stays off, so that `zwave place off`
 * can always undo a choice. */
typedef struct {
    /* One line per place the board offers, for `zwave place`: "socket: wireless-module socket
     * (TX GPIO 53, RX 54)". NULL or "": none. */
    const char *places;
    /* May the radio move to the place called id? false with the reason in why. */
    bool (*place_ok)(const char *id, char *why, size_t len);
    /* May its reset line go on gpio (at the place stored now)? false with the reason in why. */
    bool (*reset_ok)(int16_t gpio, char *why, size_t len);
    /* May the radio go on pins of the person's own (`zwave place custom`, ZW-046; reset -1: no
     * reset line)? NULL: refused. */
    bool (*custom_ok)(int16_t tx, int16_t rx, int16_t reset, char *why, size_t len);
    /* Lines `zwave status` and `zwave place` print this start, NULL when none: why a stored
     * custom place was not used ("custom place not used: ..."), and a warning about the stored
     * pins. */
    const char *custom_why;
    const char *warning;
} zwave_service_rules_t;

typedef enum {
    ZWAVE_CUSTOM_NONE = 0, /* the stored place is not "custom" */
    ZWAVE_CUSTOM_STORED,   /* it is, and its pins read back */
    ZWAVE_CUSTOM_DAMAGED,  /* it is, but a pin is missing or out of range */
} zwave_custom_t;

void zwave_service_rules(const zwave_service_rules_t *rules);
/* The place chosen with `zwave place` (NVS zwave/place), copied into out: false when off. Read
 * before the zone sources start: a place takes pins and terminals. */
bool zwave_service_stored_place(char *out, size_t len);
/* The reset line chosen with `zwave reset-pin` (NVS zwave/rstpin), -1 when none. */
int16_t zwave_service_stored_reset_pin(void);
/* The pins stored with `zwave place custom` (NVS zwave/c_tx, c_rx, c_reset; ZW-046), set when
 * ZWAVE_CUSTOM_STORED; *reset -1 for no reset line. */
zwave_custom_t zwave_service_stored_custom(int16_t *tx, int16_t *rx, int16_t *reset);

/* After zone_service_start(): registers the zone source and, with a place, opens the UART and
 * starts the tasks and the network. */
esp_err_t zwave_service_start(const zwave_service_config_t *cfg);

/* The `zwave` console command (docs/features/36 README "Console"). */
esp_err_t zwave_service_console_register(void);

/* The touch UI's zw_* calls (ui_backend_t, docs/features/36 screens.md §12), for ui_port_esp.
 * Safe from any task; status reads take the zone service's lock briefly, a request waits for the
 * zwave task (normally a few milliseconds). ui_status false: the service does not run. */
bool zwave_service_ui_status(ui_zw_status_t *out);
size_t zwave_service_ui_devices(ui_zw_device_t *out, size_t max);
bool zwave_service_ui_device(uint16_t node, uint16_t generation, ui_zw_device_page_t *out);
/* Checks installer mode and the partitions itself (ZW-005, ZW-016, ZW-018). */
ui_zw_req_result_t zwave_service_ui_request(const ui_zw_request_t *req);
void zwave_service_ui_progress(ui_zw_progress_t *out);
/* SmartStart's list (network.md §4.2): read, a code registered, an entry changed. Registering and
 * changing check installer mode themselves. */
size_t zwave_service_ui_prov(ui_zw_prov_t *out, size_t max);
ui_zw_prov_result_t zwave_service_ui_prov_register(const char *code, const char *name);
ui_zw_prov_result_t zwave_service_ui_prov_change(const uint8_t dsk[16], uint8_t op, uint8_t arg);
/* The same for the Panel Manager, which decides `allowed` itself (a signed-in installer, the panel
 * not armed: panel_web's access table), not from the panel's own installer mode. */
ui_zw_prov_result_t zwave_service_prov_register(const char *code, const char *name, bool allowed);
ui_zw_prov_result_t zwave_service_prov_change(const uint8_t dsk[16], uint8_t op, uint8_t arg,
                                              bool allowed);
/* A network operation for the Panel Manager (screens.md §11): `allowed` its own decision (a
 * signed-in installer, the panel not armed), and every partition disarmed besides. */
ui_zw_req_result_t zwave_service_request(const ui_zw_request_t *req, bool allowed);
/* network.md §14: the radio's own firmware from the installer's .gbl (the Panel Manager's upload):
 * checked, copied, sent (UI_ZW_OP_RADIO_UPDATE; arming held meanwhile). `data` is the caller's
 * only during the call. The radio's check and the service's file: zwave_service_request with
 * UI_ZW_REQ_FW_CHECK / _FW_INSTALL and node 0. */
ui_zw_req_result_t zwave_service_radio_file(const uint8_t *data, size_t len, bool allowed,
                                            uint8_t user);

/* The radio's diagnostics for the Panel Manager's Radio page (screens.md §11): what the touchscreen
 * leaves out. False: the service does not run. */
#define ZWAVE_DIAG_NODES 64u
typedef struct {
    bool up;                        /* the link is up */
    uint32_t home_id;
    uint16_t node_id, suc_node_id;
    uint8_t chip_type, chip_version, library_type, api_version;
    char library[24];
    uint16_t manufacturer, product_type, product_id;
    bool region_known;
    uint8_t region;
    bool noise_known;
    uint8_t channels;
    int8_t noise[4];                /* dBm per channel (ZW_RSSI_* values as zw_jam keeps them) */
    int8_t jam_threshold;
    bool jammed;
    bool power_known, lr_power_known;
    int16_t power_dbm10, power_cal_dbm10, lr_power_dbm10;
    bool power_cap_set, lr_cap_set;
    int16_t power_cap_dbm10, lr_cap_dbm10;
    uint32_t link_ups, link_downs, restarts, recoveries, bootloader;
    uint32_t foreign, malformed, not_singlecast;
    uint8_t node_count;
    struct {
        uint16_t node;
        uint32_t wrong_level;       /* frames at the wrong security level (security.md §8) */
        uint32_t decrypt_failures;
        uint32_t duplicates;
    } nodes[ZWAVE_DIAG_NODES];
} zwave_diag_t;
bool zwave_service_diag(zwave_diag_t *out);

/* Home Assistant through the MQTT client (docs/features/36 ZW-036, net_mqtt_zw_port_t): the
 * outputs the panel switches as alarm_proto's proto_zw_device_t, how many; and a command for one
 * (level 0..100, a garage 0 = close), run by the zwave task like the pages' requests. */
size_t zwave_service_ha_devices(void *out, size_t max);
bool zwave_service_ha_command(const char *key, int level, uint64_t received_ms);
/* For each of those devices (the n proto_zw_device_t zwave_service_ha_devices gave), how long a
 * command to it may take to end, ms: a garage door's whole move (zwo_garage_budget_ms, its travel
 * time included), 0 for anything else (home_control's own HC_ACT_TIMEOUT_MS). RAM only. */
void zwave_service_action_ms(const void *devices, size_t n, uint32_t *ms);

/* The panel's own devices (device_service's dsrc_zw, docs/features/41 DESIGN section 4 "Z-Wave
 * (phase B)"): a command POSTED to the zwave task, never waited for. `done` is called exactly once
 * from the zwave task, with no lock of this service held: at once when the output layer refused
 * it (error "failed": unknown, too old (ZWO_STALE_MS from issued_ms), a level it can't take, a
 * garage whose position isn't known; "busy"; "offline"; "send_failed") or found the garage there
 * already (ok); otherwise at the command's END (PL02): ok once the device confirmed it (a garage:
 * its sensor saw the door get there), "failed" for a door that didn't get there, "timeout" for
 * anything that may have happened (not acknowledged, not confirmed, a relay that won't let go) -
 * zwo_end_word's words. A garage's end can come after home_control's own HC_ACT_TIMEOUT_MS: its
 * device carries its own longer wait (zwave_service_action_ms). A garage moves through
 * zwz_panel_garage_move (level 0 closes, 100 opens; anything else is refused), whose output layer
 * presses only when its sensor says the door is the other way: never blind. Who may open was
 * decided above (hc_act / hc_act_auto). false: not posted (the Z-Wave service isn't running, the
 * queue is full; a radio that is down answers "offline" through `done`) - `done` is never called.
 * Never blocks. */
typedef void (*zwave_service_output_done_fn)(void *ctx, uint32_t request_id, bool ok,
                                             const char *error);
bool zwave_service_output_post(const char *key, int level, uint64_t issued_ms,
                               uint32_t request_id, zwave_service_output_done_fn done, void *ctx);
/* The radio's link is up (the network runs). Safe from any task. */
bool zwave_service_link_up(void);

/* The engine's bell for the Z-Wave sirens (docs/features/16 OUT-016, docs/features/36 sirens.md),
 * handed on by the start-up's own listener (panel_app.c on_output) as Zigbee's is: from the alarm
 * task, never blocking, never taking a lock - the latest per partition is kept and posted to the
 * zwave task (replayed whole if its mailbox was full). Only ALARM_OUT_BELL is taken. Built
 * without CONFIG_ALARM_ZSRC_ZWAVE too (it does nothing), so the caller needs no guard. */
void zwave_service_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);

/* The developer link (docs/features/30 "Z-Wave"): JSON for an enrolled workstation, through
 * dev_remote's adapter (the firmware's dev_adapter.c). Each writes one JSON document into out,
 * at most cap bytes with its NUL, and is safe from any task (the HTTPS worker): the state is read
 * under the zone service's lock, a request waits for the zwave task (2 s at most). The words are
 * the contract in docs/features/30; they change only by adding. */
/* The worst case: the radio ~0.3 KB, 64 devices of 0.26 KB each (their check-ins included) with
 * a 39-byte model escaped six-fold (0.23 KB), the operation ~0.5 KB: about 32.4 KB. */
#define ZWAVE_SERVICE_STATE_JSON_MAX    36864u
#define ZWAVE_SERVICE_TRACE_JSON_MAX    12288u /* a trace answer stops at about 12 KB ... */
#define ZWAVE_SERVICE_TRACE_ENTRIES_MAX 64u    /* ... or at 64 entries */
#define ZWAVE_SERVICE_ANSWER_JSON_MAX   32u    /* {"result":"unsupported"} */

/* GET /dev/v1/zwave: {"api":1,"radio":{...},"devices":[...],"progress":{...}} - the radio block
 * also before the service started or with no place chosen ("state":"not_connected"). The
 * document's length; 0 when it did not fit or memory for the device list was short. */
size_t zwave_service_state_json(char *out, size_t cap);
/* GET /dev/v1/zwave/trace (development builds only, CONFIG_ALARM_BUILD_DEV): the frame trace from
 * sequence number `since` (numbers start at 1; 0 asks for everything),
 * {"api":1,"next":N,"lost":L,"frames":[{"seq","t","dir","hex"}]}. An older `since` starts at the
 * oldest entry kept, "lost" counting what was skipped (and entries the link could not record); one
 * past the newest (the panel restarted since) starts at the oldest too. "next" is the number to
 * ask for next. 0 when cap is too small for one frame (it needs about 0.7 KB). */
size_t zwave_service_trace_json(uint64_t since, char *out, size_t cap);
/* POST /dev/v1/zwave: a request {"op":...} through the slot the console uses, allowed as on the
 * console (every partition disarmed): true with {"result":"ok|not_allowed|busy|no_radio|invalid|
 * unsupported"} in out; false when the body is not one JSON object, names a field twice, has no
 * known "op", or cap is below ZWAVE_SERVICE_ANSWER_JSON_MAX (the link answers 400). */
bool zwave_service_request_json(const char *body, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZWAVE_SERVICE_H */
