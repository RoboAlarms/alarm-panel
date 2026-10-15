/*
 * zwz_panel.h - the panel's Z-Wave, all of it but the RTOS (docs/features/36 Z8): the network
 * (zw_net) and the bridge to the zone source (zwave_zones.h), what the Z-Wave pages show and ask
 * for (ui_backend_t's zw_* calls, screens.md §12), the events the log keeps (network.md §15) and
 * what the panel stores (NVS `zwave`: keys, gen, nodes, cfg, chk; network.md §2).
 *
 * The firmware's zwave_service owns one in its zwave task: it feeds it the link's news and the
 * clock with the zone service's lock held (the zone source's state is the hub's too), polls it
 * at zwz_panel_next_deadline(), runs the pages' and the console's requests the same way, and
 * after unlocking writes what zwz_panel_dirty() says changed and logs what zwz_panel_take_log()
 * hands out. The pages' calls (status, devices, a device, progress) only read, under the same
 * lock, from any task. The host tests drive it against the fake ZAC93.
 *
 * Requests (zwz_panel_request) follow ui_zw_request_t: `allowed` is the caller's "installer mode
 * with every partition disarmed" (ZW-005, ZW-016, ZW-018; the console asks for disarmed only).
 * The network reset is done when asked: asking twice is the caller's (the pages ask twice, the
 * console wants `confirm`). Phase 2's replace, identify and routes answer UNSUPPORTED.
 *
 * A device's own check-in (UI_ZW_REQ_CHECK_IN with a node; network.md §7) goes to
 * zw_net_set_check_in - refused NOT_ALLOWED, NO_RADIO (the link not up or not checked yet),
 * INVALID (not a ready sleeping device of this network, seconds out of range) or BUSY (its wake-up
 * queue full) - and the value it sets is stored (`chk`): its page shows it as "changes when it
 * next wakes up" (check_in_next_s) until the device confirms it and its record has it, and its
 * later interviews set it. When the device does not take it ("fail", no answer, a Set that cannot
 * go) it keeps the interval it had and the stored value is dropped: the page shows the interval it
 * has and nothing to come, and nothing is logged. A value not confirmed before a restart of the
 * panel is set again at the first check that finds the network ours. The zone source's grace
 * follows a confirmed one at once (zwz_event).
 *
 * What is stored, and how (the formats, little-endian, a format byte first; a later format only
 * appends, so a longer value is read as far as this one knows it):
 *   keys  format 1 | bound | 00 00 | home ID u32 | the six keys (S2 unauthenticated,
 *         authenticated, access control, S0, Long Range authenticated, access control): 104 bytes
 *         - generated from the panel's random source at the first start (zw_net), never shown
 *   gen   the inclusion counter (u16): written before `nodes`, so a counter never goes back
 *   nodes the node cache blob (zw_nodes.h, `ZWND`)
 *   cfg   format 1 | flags (bit 0 region chosen, bit 1 S0 allowed) | region | jamming threshold
 *         (signed dBm; 0 - as older firmware writes it - the default) | new devices' check-in
 *         u32 (0: an hour): 8 bytes
 *   chk   format 1 | count | count x (node u16, generation u16, seconds u32): devices' own
 *         check-ins (network.md §7 "per device on its page")
 *   prov  the SmartStart provisioning list (zw_prov.h's blob, `PV`)
 *
 * SmartStart (network.md §4.2, ZW-024): the provisioning list lives here. While an entry waits and
 * the owner says every partition is disarmed (port.disarmed), the radio listens for SmartStart
 * requests; a registered device joins by itself with Security 2 on its entry's terms, and is
 * logged as added by the panel (user 0) - a join that failed is removed again and not logged, its
 * entry counting the failure. Removing a SmartStart device by hand forgets its code too (else it
 * would join again once reset); the removal's page may keep it (UI_ZW_PROV_KEEP). A device that
 * left by itself keeps its entry: it joins again when powered. The network reset empties the list.
 *
 * The NVS budget (ZW-039; docs/features/20 devices.md "The NVS budget"): the owner tells the free
 * entries (zwz_panel_nvs_free); while they are under zwz_panel_nvs_need() a new device is refused -
 * ADD and REPLACE answer UI_ZW_REQ_FULL, and SmartStart neither listens nor answers - so Z-Wave
 * never takes the room the alarm configuration needs. UI_ZW_REQ_FULL also answers ADD, REPLACE,
 * REMOVE and REMOVE_FAILED while the node cache's last write failed, and ADD once the inclusion
 * counter is used up (ZW-014, zwz_panel_identity_held).
 *
 * Portable C11, caller-owned state (about 90 KB: PSRAM in the firmware), no heap.
 */
#ifndef ZWZ_PANEL_H
#define ZWZ_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_types.h"
#include "alarm_proto/proto_zw.h"
#include "ui_backend/ui_backend.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zwave.h"
#include "zwave_host/zw_fwupd.h"
#include "zwave_host/zw_net.h"
#include "zwave_host/zw_prov.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_zones/zw_feedback.h"
#include "zwave_zones/zw_keyctl.h"
#include "zwave_zones/zw_outputs.h"
#include "zwave_zones/zwave_zones.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel's own sends (the pages' "Check it now" pings): top byte 5B, zw_net's own are FF, the
 * bridge's FE, the host's 5A. */
#define ZWZ_PANEL_TAG      0x5B000000u
#define ZWZ_PANEL_TAG_MASK 0xFF000000u

/* ZW-041: the events the owner takes in one go (its array); more wait for its next turn (at
 * most a second away). The queue holds a network check that forgets every cached device and as
 * many others besides; past that each is counted in log_lost, shown by `zwave status`. */
#define ZWZ_PANEL_LOG_MAX   8u
#define ZWZ_PANEL_LOG_QUEUE (ZW_NODES_MAX + ZWZ_PANEL_LOG_MAX) /* events waiting to be logged */
#define ZWZ_PANEL_CHECKINS ZW_NODES_MAX /* devices with a check-in of their own */

/* network.md §7: the check-ins the pages offer (1 h, 4 h, 12 h), bounded. */
#define ZWZ_CHECK_IN_MIN_S 600u
#define ZWZ_CHECK_IN_MAX_S 86400u

/* A device's software update (ZW-033, zwz_panel_fw_start). */
#define ZWZ_FW_BATTERY_MIN    30u            /* % a battery device needs (and not "low") */
#define ZWZ_FW_WAKE_WAIT_MS   (10u * 60000u) /* a sleeping device must wake within this */
#define ZWZ_FW_PAYLOAD        46u            /* a frame's command bytes when the radio doesn't say */
#define ZWZ_FW_SECURE_COST    30u            /* Security 2 around it, its nonce extension included */
#define ZWZ_FW_REINTERVIEW_MS 5000u          /* its re-read refused (busy): tried again after */

/* What zwz_panel_dirty() reports: what is to be written. */
#define ZWZ_DIRTY_NODES    0x01u /* gen, then nodes */
#define ZWZ_DIRTY_KEYS     0x02u /* keys */
#define ZWZ_DIRTY_CFG      0x04u /* cfg */
#define ZWZ_DIRTY_CHECKINS 0x08u /* chk */
#define ZWZ_DIRTY_GARAGE   0x10u /* garage (zw_outputs.h's blob) */
#define ZWZ_DIRTY_PROV     0x20u /* prov (zw_prov.h's blob) */

/* Devices whose own jamming reports are kept (network.md §9); the oldest makes room. */
#define ZWZ_PANEL_JAM_REPORTS 8u
/* The arming hold after a radio update that didn't bring the radio back (network.md §14). */
#define ZWZ_PANEL_OTW_HOLD_MS (15u * 60000u)
/* A device's jamming reports go to the event log once an hour at most. */
#define ZWZ_PANEL_JAM_LOG_MS  3600000u
/* ZW-041: the radio's own restarts too - the first at once, then one event an hour at most
 * that says in arg2 how many restarts it stands for. */
#define ZWZ_PANEL_RESTART_LOG_MS 3600000u

/* The stored values' sizes. */
#define ZWZ_KEYS_BLOB     104u
#define ZWZ_CFG_BLOB      13u
#define ZWZ_CFG_BLOB_MIN  8u  /* what firmware before the power cap wrote (read as no cap) */
#define ZWZ_CHECKINS_BLOB (2u + 8u * ZWZ_PANEL_CHECKINS)
/* The node cache: a node's record at its largest, and the whole blob (network.md §2). */
#define ZWZ_NODE_RECORD_MAX 800u
#define ZWZ_NODES_BLOB_MAX  (ZW_NODES_MAX * ZWZ_NODE_RECORD_MAX + 1024u)

/* The NVS budget (ZW-039; docs/features/20 devices.md "The NVS budget", the same arithmetic as
 * zbz_panel.h's): NVS keeps 32-byte entries; a blob costs an index entry, then for each chunk of
 * at most 4,000 bytes a header entry and its bytes in whole entries. */
#define ZWZ_NVS_ENTRY_BYTES 32u
#define ZWZ_NVS_CHUNK_BYTES 4000u
#define ZWZ_NVS_ENTRIES(bytes)                                                                  \
    (1u + ((bytes) + ZWZ_NVS_CHUNK_BYTES - 1u) / ZWZ_NVS_CHUNK_BYTES +                          \
     ((bytes) + ZWZ_NVS_ENTRY_BYTES - 1u) / ZWZ_NVS_ENTRY_BYTES)
/* What one more node adds to the node cache when it is rewritten: its record, a chunk header
 * and the rounding (27 entries). */
#define ZWZ_NVS_NODE_GROWTH ZWZ_NVS_ENTRIES(ZWZ_NODE_RECORD_MAX)
/* NVS "zwave" at its largest (1,753 entries, pinned by test_zwz_panel.c): the namespace, keys,
 * gen (a u16: one entry), nodes, cfg, chk, garage, prov, keypad_fb, keypad_ctl, place (a string
 * of at most 15 characters: two entries) and rstpin (a u8: one). */
#define ZWZ_NVS_WORST                                                                           \
    (1u + ZWZ_NVS_ENTRIES(ZWZ_KEYS_BLOB) + 1u + ZWZ_NVS_ENTRIES(ZWZ_NODES_BLOB_MAX) +            \
     ZWZ_NVS_ENTRIES(ZWZ_CFG_BLOB) + ZWZ_NVS_ENTRIES(ZWZ_CHECKINS_BLOB) +                       \
     ZWZ_NVS_ENTRIES(ZWO_GARAGE_BLOB) + ZWZ_NVS_ENTRIES(ZW_PROV_BLOB_MAX) +                     \
     ZWZ_NVS_ENTRIES(ZWF_BLOB_SIZE) + ZWZ_NVS_ENTRIES(ZWKC_BLOB_SIZE) + 2u + 1u)
/* What Z-Wave never takes: the alarm configuration rewritten whole at its largest (NVS writes the
 * new value before it erases the old one) and the node cache's growth by one more record. */
#define ZWZ_NVS_RESERVE (ZWZ_NVS_ENTRIES(ALARM_CONFIG_BLOB_MAX) + ZWZ_NVS_NODE_GROWTH)
/* Free entries under this: no Add, no Replace, no SmartStart join (UI_ZW_REQ_FULL). */
#define ZWZ_NVS_ADD_MIN     (ZWZ_NVS_RESERVE + ZWZ_NVS_NODE_GROWTH)
/* The node cache with n devices at its largest: its rewrite needs this much free beside the old
 * copy (zwz_panel_nvs_need). */
#define ZWZ_NVS_CACHE_ENTRIES(n)                                                                \
    ZWZ_NVS_ENTRIES((uint32_t)(n) * ZWZ_NODE_RECORD_MAX + 1024u)
#define ZWZ_NVS_NOT_TOLD    UINT32_MAX /* zwz_panel_init's: no limit until the owner tells */

/* The installer's settings (NVS zwave/cfg). */
typedef struct {
    bool region_set;     /* a region was chosen (network.md §12); else the module keeps its own */
    uint8_t region;
    uint32_t check_in_s; /* new devices' check-in (network.md §7); 0: ZW_NET_CHECK_IN_S */
    bool s0_allowed;     /* security.md §10: phase 3 (ALARM_ZWAVE_S0) */
    int8_t jam_dbm;      /* network.md §9: the jamming threshold; 0: ZW_JAM_THRESHOLD_DBM */
    /* network.md §12 power (phase 3): the radio's power when the panel first read it - the cap
     * of every change (Zooz sets the legal value) - in tenths of a dBm; a new radio forgets it */
    bool power_cap_set;
    int16_t power_cap_dbm10;
    bool lr_cap_set;
    int16_t lr_cap_dbm10;
} zwz_cfg_t;

/* The network keys and the home ID they belong to (NVS zwave/keys, ZW-017). */
typedef struct {
    bool bound;
    uint32_t home_id;
    zw_s2_keys_t keys;
} zwz_keys_t;

/* A device's own check-in (NVS zwave/chk). */
typedef struct {
    uint16_t node, generation;
    uint32_t seconds;
} zwz_checkin_t;

/* An event for the log (network.md §15): the owner records it through the engine
 * (alarm_engine_platform_event_ex), as the panel (user 0) or for the user who asked. */
typedef struct {
    uint8_t type; /* alarm_event_type_t: ALARM_EV_ZWAVE_* */
    uint16_t zone;
    uint8_t arg, arg2;
    uint8_t user;
} zwz_log_t;

typedef struct {
    void *ctx;
    /* Bytes to the module (the zw_link task writes them); false: they could not go. */
    bool (*write)(void *ctx, const uint8_t *bytes, size_t len);
    /* The module's reset line, asserted (low) or released; NULL: none wired. */
    void (*reset_line)(void *ctx, bool asserted);
    /* A zone's binding (the hub's), for the zones a device feeds; false: none. */
    bool (*binding)(void *ctx, uint16_t zone, zone_hub_binding_t *out);
    /* Optional completion for owner sends not consumed by the panel/zone bridge. */
    void (*send_done)(void *ctx, uint32_t tag, zw_s2_result_t result);
    /* Optional: a command the bridge doesn't decode, offered first (zwz_set_claim) - an assigned
     * keypad's Entry Control. Under the owner's lock: copy it, never wait; true: taken. */
    bool (*command)(void *ctx, const zw_s2_rx_t *rx);
    /* Optional (ZW-035): where a garage's tilt sensor says the door is - its zone's input, known,
     * reachable and not CHECK; else unknown. NULL: always unknown (no garage ever moves). */
    zwo_position_t (*zone_position)(void *ctx, uint16_t zone);
    /* Optional (SmartStart): every partition is disarmed now. NULL: never - the radio does not
     * listen for SmartStart. */
    bool (*disarmed)(void *ctx);
    /* Optional (ZW-024): the panel's local time, for a device's Time or Date Get; false when the
     * clock isn't set. NULL: always "clock not set". */
    bool (*local_time)(void *ctx, zwz_local_time_t *out);
    /* Optional (ZW-038, CORE-TRBL-008): a keypad whose control slot is assigned (it has a
     * partition, emergency buttons or not; its security class isn't checked) can't be reached
     * (false) or is heard again (true); `device` is its slot's ZWKC_DEVICE. Only changes are
     * said; a slot turned off or a keypad removed while it was unreachable says true. Under the
     * owner's lock: queue it, never wait on the engine. NULL: keypads aren't supervised. */
    void (*keypad_reachable)(void *ctx, uint8_t device, bool reachable);
} zwz_panel_port_t;

/* What the panel starts with: what is stored, and the build's choices. */
typedef struct {
    zwz_cfg_t cfg;
    zwz_keys_t keys;
    uint16_t generation;
    bool long_range;                 /* ALARM_ZWAVE_LR (phase 3) */
    bool jam;                        /* network.md §9: watch for jamming (the firmware: on) */
    zw_cc_panel_identity_t identity; /* what it answers about itself (command-classes.md §5) */
} zwz_panel_setup_t;

/* A posted command's caller, waiting for that command's end (PL02, zwz_panel_output_wait). One
 * command an output or a garage at a time: never more waits than this. */
#define ZWZ_OUT_WAITS (ZWO_OUTPUTS + ZWO_GARAGES)
typedef struct {
    uint32_t cmd;     /* the output layer's command id, 0: free */
    uint32_t request; /* the caller's */
    uint8_t result;   /* zwo_result_t, once ended */
    bool ended;
} zwz_out_wait_t;

typedef struct {
    zwz_panel_port_t port;
    zw_crypto_port_t crypto;
    zsrc_zw_t *src;
    zwz_panel_setup_t setup; /* kept current: what is (to be) stored */
    zwz_checkin_t checkins[ZWZ_PANEL_CHECKINS];
    size_t checkin_count;
    zw_net_t net;
    zwz_t bridge;
    uint64_t now;
    uint8_t dirty;
    bool started;
    bool reissue;                   /* the network was found ours: stored check-ins again (poll) */
    bool quiet;                     /* devices the installer's own operation forgets: not news */
    zw_down_reason_t down;          /* why the link went down last */
    uint64_t expect_restart_until;  /* a restart before this is the panel's own doing */
    uint16_t stamp;                 /* stamps the panel's own tags */
    /* the operation the pages follow */
    ui_zw_progress_t progress;
    uint64_t deadline;              /* its countdown's end; 0 none */
    uint8_t pending;                /* an add's result, when its interview ends */
    uint8_t op_user;                /* who started it */
    uint32_t ping_tag;              /* the pages' "Check it now" out */
    uint32_t identify_tag;          /* the pages' "Blink its light" out (ZW-024) */
    bool otw_hold;                  /* network.md §14: arming held for the radio's update... */
    uint64_t otw_hold_until;        /* ...until the radio is back, or this (UINT64_MAX: running) */
    /* network.md §9: devices' own "RF jamming detected" reports, the newest per device */
    struct {
        uint16_t node, generation;  /* node 0: free */
        uint64_t at;                /* the last report */
        uint64_t logged_at;         /* the last one logged (one an hour at most) */
    } jam_reports[ZWZ_PANEL_JAM_REPORTS];
    uint16_t routes_one;            /* §11: the device a "Rebuild its routes" is for; 0: all */
    const zwf_t *feedback;          /* the owner's keypad feedback (zwz_panel_set_feedback) */
    const zwkc_t *keyctl;           /* the owner's keypad control (zwz_panel_set_keyctl) */
    uint8_t keyctl_down;            /* ZW-038: slots said unreachable (port.keypad_reachable) */
    /* a device's software update (ZW-033) */
    zw_fwupd_t fw;
    bool fw_sleeper;                /* it sleeps: its update waited for its wake-up */
    bool fw_radio_lost;             /* the radio went away during it */
    uint16_t fw_reinterview;        /* updated: read again at fw_reinterview_at */
    uint16_t fw_reinterview_gen;
    uint64_t fw_reinterview_at;
    /* what the devices switch, and garage doors (ZW-034, ZW-035) */
    zwo_t outputs;
    uint8_t out_last[ZWO_OUTPUTS];    /* ui_zw_out_result_t of each output's last command */
    uint8_t garage_last[ZWO_GARAGES];
    uint8_t garage_user[ZWO_GARAGES]; /* who moved it last (the log) */
    uint16_t out_seq;
    bool out_refresh;                 /* the network was found ours: ask the outputs */
    zwz_out_wait_t out_wait[ZWZ_OUT_WAITS]; /* posted commands' callers (PL02) */
    /* SmartStart (network.md §4.2) */
    zw_prov_t prov;
    zw_prov_entry_t prov_kept;        /* the entry the last removal forgot (UI_ZW_PROV_KEEP) */
    bool prov_kept_valid;
    /* the log */
    zwz_log_t log[ZWZ_PANEL_LOG_QUEUE];
    size_t log_n;
    uint32_t log_lost;                /* events that found the queue full (`zwave status`) */
    /* ZW-041: the radio's own restarts, logged once an hour at most */
    bool restart_logged;              /* one was logged, at restart_logged_at */
    uint64_t restart_logged_at;
    uint8_t restarts_held;            /* restarts since, not logged yet (at most 255) */
    uint8_t restart_reason;           /* the wake-up reason of the last of them */
    /* the NVS budget (ZW-039) */
    uint32_t nvs_free;                /* free entries as last told; ZWZ_NVS_NOT_TOLD */
    /* ZW-014: a node ID's new occupant only once its old one's going is stored (zones.md §1) */
    uint32_t gone_seq;                /* devices that left the cache (removed, forgotten) */
    uint32_t gone_taken;              /* gone_seq when the node cache was last taken */
    uint32_t gone_stored;             /* gone_seq the stored node cache holds */
    bool nodes_failed;                /* the last write of the node cache failed */
} zwz_panel_t;

/* ---- life cycle -------------------------------------------------------------------------------- */

/* Ready, not started: the network from `setup`, the devices' own check-ins, the zone source (the
 * owner registers it with the hub) and the crypto port (copied). */
void zwz_panel_init(zwz_panel_t *p, const zwz_panel_port_t *port, const zwz_panel_setup_t *setup,
                    const zwz_checkin_t *checkins, size_t checkin_count, zsrc_zw_t *src,
                    const zw_crypto_port_t *crypto);
/* The stored node cache (before zwz_panel_start). False when it is damaged (then empty). */
bool zwz_panel_load_nodes(zwz_panel_t *p, const uint8_t *blob, size_t len);
/* The link's start-up. */
void zwz_panel_start(zwz_panel_t *p, uint64_t now);
/* The owner's keypad feedback assignments (zw_feedback.h), read for the device page's "Sounds and
 * lights" (its fit, partition and whether it runs); NULL: none shown. The owner keeps and stores
 * them: UI_ZW_REQ_FEEDBACK is its to answer (zwz_panel_request says UNSUPPORTED). */
void zwz_panel_set_feedback(zwz_panel_t *p, const zwf_t *feedback);
/* The owner's keypad control assignments (zw_keyctl.h), read for the device page's "Arm and
 * disarm from its buttons" and for supervising those keypads (port.keypad_reachable, read again
 * at every poll: the owner changes them under its lock); NULL: none shown. UI_ZW_REQ_CONTROL is
 * the owner's to answer. */
void zwz_panel_set_keyctl(zwz_panel_t *p, const zwkc_t *keyctl);

/* The zw_link task's news. */
void zwz_panel_on_control(zwz_panel_t *p, uint64_t now, uint8_t byte);
void zwz_panel_on_frame(zwz_panel_t *p, uint64_t now, const zw_frame_t *frame);
void zwz_panel_on_text(zwz_panel_t *p, uint64_t now, const uint8_t *bytes, size_t len);
/* Everything due at now: the network, then the bridge, then the countdowns. */
void zwz_panel_poll(zwz_panel_t *p, uint64_t now);
/* When to poll again: 0 at once, UINT64_MAX not before news. */
uint64_t zwz_panel_next_deadline(const zwz_panel_t *p);

/* ---- requests ----------------------------------------------------------------------------------- */

/* A page's (or the console's) request: started or answered, or why not (screens.md §12). */
ui_zw_req_result_t zwz_panel_request(zwz_panel_t *p, uint64_t now, const ui_zw_request_t *req,
                                     bool allowed);
/* Interview a device again from step 1 (`zwave interview`). False: not a device of this
 * network. */
bool zwz_panel_interview(zwz_panel_t *p, uint64_t now, uint16_t node);
/* The same, saying why not (`zwave interview <n>`): UI_ZW_REQ_NO_RADIO when the panel's Z-Wave is
 * not started, UI_ZW_REQ_INVALID for a device the panel does not know, UI_ZW_REQ_BUSY when a known
 * device's re-read was refused (every scratch record in use). */
ui_zw_req_result_t zwz_panel_interview_result(zwz_panel_t *p, uint64_t now, uint16_t node);
/* The module restarts because the panel said so (its reset line pulsed): no "restarted by itself"
 * for a restart within ms from now. */
void zwz_panel_expect_restart(zwz_panel_t *p, uint64_t now, uint32_t ms);

/* The NVS budget (ZW-039; docs/features/20 devices.md "The NVS budget"): NVS's free entries, as
 * the owner reads them (nvs_get_stats' available entries) at start, before the radio is up, after
 * each pass of its store, just before it passes on an Add or a Replace, and every few seconds (a
 * SmartStart join asks nobody first; the owner's other writes are covered by it). Until told there is no limit (the host tests, the preview).
 * While they are under zwz_panel_nvs_need() a new device is refused: Add and Replace answer
 * UI_ZW_REQ_FULL, the radio stops listening for SmartStart and a SmartStart request is not
 * answered. Changing what is there (a setup, a check-in, a removal) still writes. */
void zwz_panel_nvs_free(zwz_panel_t *p, uint32_t entries);
/* The free entries a new device needs now: ZWZ_NVS_ADD_MIN, or more once the node cache is large
 * enough that its rewrite with one more device at its largest is bigger than the reserve (the
 * rewrite happens inside one NVS call, never beside the configuration's own). */
uint32_t zwz_panel_nvs_need(const zwz_panel_t *p);
/* True while a new device is refused for want of storage. */
bool zwz_panel_storage_full(const zwz_panel_t *p);
/* ZW-014 (zones.md §1): true while a device's identity may not change - a device that left the
 * cache (removed, forgotten) is not in the stored node cache yet, or the cache's last write
 * failed. A restart then would read the old cache, and a node ID given meanwhile to a new device
 * would be taken for the old one's (its generation, so its zones). Add, Replace and SmartStart
 * wait: Add and Replace answer UI_ZW_REQ_FULL after a failed write, UI_ZW_REQ_BUSY while the
 * write is only due; SmartStart neither listens nor answers. Removing still works while the
 * write is only due; after a failed one Remove and Remove Failed answer UI_ZW_REQ_FULL too, and a
 * device reset locally stays in the cache as gone (not removed from the module) until a write
 * succeeds - else a restart could give its generation to its node's next device. */
bool zwz_panel_identity_held(const zwz_panel_t *p);

/* ---- a device's software update (ZW-033) -------------------------------------------------------- */

/* The device takes software updates: its setup is done, its identity and versions are known, and
 * it lists Firmware Update Meta Data. */
bool zwz_panel_fw_updatable(const zwz_panel_t *p, uint16_t node, uint16_t generation);
/* Sends the device `image` for its firmware `target` (0: its own) - an image the owner verified
 * (the update's SHA-256, its file read by zw_fwfile.h) and keeps unchanged until the update ends
 * (UI_ZW_OP_FIRMWARE ends). Refused: NOT_ALLOWED (`allowed` as for the other requests), INVALID
 * (not this device, not updatable, no image), BUSY (another operation), NO_RADIO, LOW_BATTERY
 * (a battery device reporting low, or under ZWZ_FW_BATTERY_MIN %). A sleeping device waits for its
 * wake-up (up to ZWZ_FW_WAKE_WAIT_MS: the page asks to wake it) and is kept awake until it ends;
 * STOP stops it. The device checks the image itself before it takes it; taken, its setup reads it
 * again after its restart (its versions, and whatever else changed). Logged:
 * ALARM_EV_ZWAVE_SOFTWARE, started and how it ended. */
ui_zw_req_result_t zwz_panel_fw_start(zwz_panel_t *p, uint64_t now, uint16_t node,
                                      uint16_t generation, uint8_t target, const uint8_t *image,
                                      size_t len, bool allowed, uint8_t user);

/* ---- what the devices switch (ZW-034, ZW-035) ------------------------------------------------------ */

/* Switch (node, generation)'s output at `endpoint` to `level` (0..100), asked at `issued_ms`: the
 * pages and Home Assistant alike. Started, or why not (zw_outputs.h); its end in the page's
 * output `last`. */
zwo_result_t zwz_panel_output_set(zwz_panel_t *p, uint64_t now, uint16_t node, uint16_t generation,
                                  uint8_t endpoint, int level, uint64_t issued_ms);
/* Move the garage door whose relay that output is: `open` or close. Logged with `user`
 * (ALARM_EV_ZWAVE_GARAGE). ZWO_ALREADY / ZWO_ERR_POSITION pressed nothing. */
zwo_result_t zwz_panel_garage_move(zwz_panel_t *p, uint64_t now, uint16_t node,
                                   uint16_t generation, uint8_t endpoint, bool open,
                                   uint64_t issued_ms, uint8_t user);
/* PL02 (HCTL-012, AUT-006): ZWO_OK from those two means started, not done. A caller that must
 * hear the end (device_service, the automations) waits for it: right after the start, under the
 * same lock, for the caller's `request` (non-zero). The slot it waits in (the owner keeps its own
 * callback there), or -1: nothing under way at that output, or someone waits for it already. */
int zwz_panel_output_wait(zwz_panel_t *p, uint16_t node, uint16_t generation, uint8_t endpoint,
                          uint32_t request);
/* One wait whose command has ended - confirmed (ZWO_OK), failed, timed out, or dropped with its
 * device or garage - taken: its slot, request and result. false: none. Each wait ends once, by
 * its own command; a late answer to an older command never ends it. The owner calls its caller
 * after letting go of its lock. */
bool zwz_panel_output_ended(zwz_panel_t *p, size_t *slot, uint32_t *request,
                            zwo_result_t *result);
/* Home Assistant's view of them (ZW-036, docs/features/10 MQTT-018..020): one entry an output -
 * its stable key, kind (a garage door's relay is the garage), name, level, whether it can be
 * reached. How many. */
size_t zwz_panel_ha_devices(const zwz_panel_t *p, uint64_t now, proto_zw_device_t *out,
                            size_t max);
/* A command from Home Assistant for `key` (level 0..100; a garage: 0 closes, opening is refused),
 * received at `received_ms`: true when it was taken (started, or a garage already closed). */
bool zwz_panel_ha_command(zwz_panel_t *p, uint64_t now, const char *key, int level,
                          uint64_t received_ms);

/* The stored garages (NVS zwave/garage), after zwz_panel_load_nodes: the outputs are read from
 * the cache first. False: damaged (none kept). */
bool zwz_panel_load_garage(zwz_panel_t *p, const uint8_t *blob, size_t len);
size_t zwz_panel_take_garage(zwz_panel_t *p, uint8_t *buf, size_t cap);

/* `zwave jam <dBm>` (network.md §9): the jamming threshold, -100..-20 dBm, stored (cfg). */
bool zwz_panel_set_jam(zwz_panel_t *p, uint64_t now, int8_t dbm);
/* network.md §12 power (phase 3): read it (zw_net_power_read; the first read becomes the stored
 * cap), and change it within that cap (zw_net_set_power; INVALID before a read). */
bool zwz_panel_read_power(zwz_panel_t *p, uint64_t now);
/* network.md §14 the radio's own firmware: a whole .gbl (the service's download, checked against
 * its SHA-256, or the installer's own file) sent as UI_ZW_OP_RADIO_UPDATE; logged as the radio's
 * software (ALARM_EV_ZWAVE_SOFTWARE, device 0). `allowed`: installer, every partition disarmed.
 * INVALID: not a whole .gbl; BUSY: an operation runs; NO_RADIO: neither up nor in its
 * bootloader. The image is the caller's until the operation ends. */
ui_zw_req_result_t zwz_panel_radio_update(zwz_panel_t *p, uint64_t now, const uint8_t *image,
                                          size_t len, bool allowed, uint8_t user);
/* CORE-ARM-013's hold for the owner to pass to the engine: from the update's start until the
 * radio is back and ours - or ZWZ_PANEL_OTW_HOLD_MS after the update ended, whichever is first,
 * so a radio that never comes back can't stop arming for good (its trouble says it instead). */
bool zwz_panel_arming_held(const zwz_panel_t *p, uint64_t now);
zw_net_result_t zwz_panel_set_power(zwz_panel_t *p, uint64_t now, bool allowed, bool long_range,
                                    int16_t dbm10);

/* ---- SmartStart (network.md §4.2, screens.md §9) --------------------------------------------------- */

/* The list, as the pages show it (at most max entries). */
size_t zwz_panel_prov(const zwz_panel_t *p, ui_zw_prov_t *out, size_t max);
/* A code registered: a SmartStart QR code's digits or a 40-digit key (spaces and dashes allowed
 * in a key); `name` may be NULL or empty. `allowed`: installer mode. A key of a device already in
 * the network is registered as joined. */
ui_zw_prov_result_t zwz_panel_prov_register(zwz_panel_t *p, uint64_t now, const char *code,
                                            const char *name, bool allowed);
/* An entry changed (ui_zw_prov_op_t; GRANT's keys in `arg`). */
ui_zw_prov_result_t zwz_panel_prov_change(zwz_panel_t *p, uint64_t now, const uint8_t dsk[16],
                                          uint8_t op, uint8_t arg, bool allowed);
/* The stored list (NVS zwave/prov). False: damaged (empty). */
bool zwz_panel_load_prov(zwz_panel_t *p, const uint8_t *blob, size_t len);
size_t zwz_panel_take_prov(zwz_panel_t *p, uint8_t *buf, size_t cap);

/* ---- what the pages show (they only read) ------------------------------------------------------ */

void zwz_panel_status(const zwz_panel_t *p, uint64_t now, ui_zw_status_t *out);
size_t zwz_panel_devices(const zwz_panel_t *p, uint64_t now, ui_zw_device_t *out, size_t max);
/* False: no such device (removed, or its node ID given to another since). */
bool zwz_panel_device(const zwz_panel_t *p, uint64_t now, uint16_t node, uint16_t generation,
                      ui_zw_device_page_t *out);
void zwz_panel_progress(const zwz_panel_t *p, uint64_t now, ui_zw_progress_t *out);
/* The generation of a device of this network, 0 when there is none (the console names devices
 * by their node ID alone). */
uint16_t zwz_panel_generation(const zwz_panel_t *p, uint16_t node);
/* The first zone a device feeds, 0 none. */
uint16_t zwz_panel_zone_of(const zwz_panel_t *p, uint16_t node, uint16_t generation);

/* ---- what is to be stored and logged ------------------------------------------------------------ */

uint8_t zwz_panel_dirty(const zwz_panel_t *p);
/* The value to write, encoded into buf, and its dirty bit cleared; 0 when nothing is due or buf is
 * too small (the bit then stays). A write that failed is asked for again with zwz_panel_redo(). */
size_t zwz_panel_take_nodes(zwz_panel_t *p, uint8_t *buf, size_t cap, uint16_t *generation);
size_t zwz_panel_take_keys(zwz_panel_t *p, uint8_t *buf, size_t cap); /* wipe buf after use */
size_t zwz_panel_take_cfg(zwz_panel_t *p, uint8_t *buf, size_t cap);
size_t zwz_panel_take_checkins(zwz_panel_t *p, uint8_t *buf, size_t cap);
void zwz_panel_redo(zwz_panel_t *p, uint8_t dirty);
/* What was taken and is now in storage (ZWZ_DIRTY_* bits; set and committed). The node cache's
 * bit ends zwz_panel_identity_held() for what that copy holds (ZW-014). */
void zwz_panel_stored(zwz_panel_t *p, uint8_t stored);
/* The next event to log, oldest first; false when none. */
bool zwz_panel_take_log(zwz_panel_t *p, zwz_log_t *out);

/* ---- the stored formats ------------------------------------------------------------------------ */

size_t zwz_cfg_encode(const zwz_cfg_t *c, uint8_t *out, size_t cap);
bool zwz_cfg_decode(zwz_cfg_t *c, const uint8_t *buf, size_t len);
size_t zwz_keys_encode(const zwz_keys_t *k, uint8_t *out, size_t cap);
bool zwz_keys_decode(zwz_keys_t *k, const uint8_t *buf, size_t len);
size_t zwz_checkins_encode(const zwz_checkin_t *c, size_t n, uint8_t *out, size_t cap);
/* The entries read into c (at most max); false when the value is damaged (*n is then 0). */
bool zwz_checkins_decode(zwz_checkin_t *c, size_t max, size_t *n, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZWZ_PANEL_H */
