/*
 * zbz_panel.h - the panel's Zigbee, all of it but the RTOS and the stack (docs/features/20
 * devices.md; ZB-010..ZB-027, BL-143): the device registry and the install codes, each device's
 * setup, the operations the pages and the console ask for, what the Zigbee pages show
 * (ui_backend_t's zb_* calls), the events the log keeps and what the panel stores. The zone
 * source (zone_hub's zsrc_zigbee) stays the zones' side; this is the devices' side, the way
 * zwave_zones' zwz_panel sits beside zsrc_zwave.
 *
 * The firmware's zigbee_service owns one in its zigbee task. With the zone service's lock held it
 * feeds it the radio's reports (zbz_panel_on_report, drained from the queue the stack's callbacks
 * fill - never from a callback) and the radio's state (zbz_panel_radio), polls it at
 * zbz_panel_next_deadline(), and runs the pages' and the console's requests; after unlocking it
 * writes what the take calls hand out and logs what zbz_panel_take_log() returns, through the
 * alarm task (alarm_engine_platform_event_ex). The pages' readers (status, devices, a device,
 * progress, codes) only read, under the same lock, from any task. The host tests drive it against
 * the fake radio (testing.md §2); the preview and the simulator use the same fake.
 *
 * Requests (zbz_panel_request) follow ui_zb_request_t. `allowed` is zwz_panel_request's: one flag
 * with one meaning, the caller's "installer mode with every partition disarmed" (ZB-002, ZB-013,
 * ZB-016, ZB-017). Every request needs it (NOT_ALLOWED without) except PING, IDENTIFY, SURVEY and
 * DISMISS, which don't read it, as Z-Wave's ping and identify don't: they change nothing, and
 * installer mode for them is the caller's alone - every caller is installer-only (the pages,
 * /api/v1/zigbee, the console) and passes nothing on outside installer mode. One operation at a
 * time (BUSY otherwise); settings (rename, codes, siren, keypad, fob) answer at once.
 *
 * `allowed` is a snapshot, and the alarm task arms on its own (ZB21): the owner runs a request
 * that needs it through zbz_panel_request_gated, which holds a claimed binding gate
 * (zone_hub/zone_bind_gate.h) from the partitions' check to the end of the radio hook - armed
 * meanwhile: NOT_ALLOWED, the radio never called. A replacement's rebinds (ZB-013, RF32) wait for
 * the same gate when the owner sets port.replace_waits (zbz_panel_replace_gated).
 *
 * The network reset (ZB-017) is done when asked; asking twice is the caller's, and so is the
 * installer code typed again (ui_zb_request_t.installer_code): the caller checks it with the
 * engine on its own task before the request goes on - never in the zigbee task or under the zone
 * lock, since it waits for the alarm task (alarm_service_authenticate_as, a wrong code counting
 * toward that source's lockout) - maps the answer with zbz_reset_code_result, refuses WRONG_CODE
 * or LOCKED_OUT, and wipes the code. The code never reaches this. The Panel Manager's reset is
 * checked by its own recent-password rule instead (devices.md §14: the browser takes no codes).
 *
 * Device numbers (zbz_ace.h): a keypad's is given here, when the KEYPAD request assigns it (the
 * request's keypad.device is ignored), from the numbers neither another Zigbee keypad nor the
 * port's devices_taken holds; none free: FULL, and the page shows UI_ZB_FIT_NO_DEVICE_SLOT.
 * Nothing here writes the engine's device table.
 *
 * The NVS budget (devices.md §4): the owner tells the free entries (zbz_panel_nvs_free); while
 * they are under ZBZ_NVS_ADD_MIN a new device or install code is refused FULL - ADD, ADD_CODE,
 * REPLACE and CODE_ADD, and a device that joins anyway is asked to leave (UI_ZB_RES_FULL) - and
 * the status says so (storage_full). Changing what is there still writes.
 *
 * What is stored (namespace ZBZ_NVS_NAMESPACE; formats in their headers):
 *   meta, d00..d63  the registry (zbz_registry.h)
 *   codes           install codes waiting (zbz_codes.h)
 *   cfg             compat (1) | flags (bit 0 a channel was chosen) | channel | 0: 4 bytes
 *   wd              siren assignments (zbz_wd.h)
 *   ace             keypad and fob assignments (zbz_ace.h)
 * The network itself (keys, PAN, the stack's device table) stays in the stack's own namespace.
 *
 * Sirens and keypads (alarm-devices.md §2-§5, wired in the ZB-M10/ZB-M11 wiring unit): the panel
 * owns a zbw_t and a zba_t whose ports are its own - their frames go out through the radio's
 * zcl_command with their own tags ('W', 'A'; never ZSRC_ZB_TAG_SOURCE) - and keeps the owner's
 * ports given to zbz_panel_init for what only the owner can do: zbw_port_t.bell_trouble (post
 * ALARM_TRBL_BELL to the alarm task) and zba_port_t.default_rsp (a Default Response, with the
 * request's own sequence number, through the stack); their `send` hooks are not used. A keypad's
 * or fob's command reaches the engine only through zbz_panel_port_t.ace_post and comes back with
 * zbz_panel_ace_result. An Arm carries a code in the report's payload: the owner wipes its copy of
 * the report after zbz_panel_on_report. zbz_panel_t holds pointers to itself in those ports: it
 * must not move after zbz_panel_init.
 *
 * Built in ZB-M3/ZB-M4 (src/zbz_panel.c, test/test_zbz_panel.c), the sirens and keypads wired in
 * ZB-M10/ZB-M11. Portable C11, caller-owned state (about 20 KB, the registry most of it: PSRAM in
 * the firmware), no heap.
 */
#ifndef ZBZ_PANEL_H
#define ZBZ_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_config.h"
#include "alarm/alarm_snapshot.h"
#include "alarm/alarm_types.h"
#include "ui_backend/ui_backend.h"
#include "zigbee_zones/zbz_ace.h"
#include "zigbee_zones/zbz_codes.h"
#include "zigbee_zones/zbz_models.h"
#include "zigbee_zones/zbz_registry.h"
#include "zigbee_zones/zbz_wd.h"
#include "zone_hub/zone_bind_gate.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel's own requests: top byte 'Z' | a sequence number (the sirens' are 'W', the keypads'
 * 'A'). */
#define ZBZ_TAG      0x5A000000u
#define ZBZ_TAG_MASK 0xFF000000u

#define ZBZ_LOG_MAX        8u   /* events waiting to be logged */
#define ZBZ_SETUP_TRIES    3u   /* a setup step's attempts before it stops (a listening device) */
#define ZBZ_STEP_MS        10000u /* a step's answer from a listening device */
#define ZBZ_WAKE_WAIT_MS   (10u * 60000u) /* a sleepy device's next frame: then the step again */
#define ZBZ_SETUP_MAX_MS   (30u * 60000u) /* a setup still waiting this long stops (STOPPED) */
/* B021: a device that has just joined may run its own key exchange (BDB v3.0.1 10.2.5: it asks
 * the trust center for its node descriptor, three tries of 5 s, then for its own link key, and
 * leaves if that fails). The setup's first request waits this long, or until the trust center saw
 * the exchange end, so as not to take the device's polls from it. */
#define ZBZ_KEY_WAIT_MS    16000u
#define ZBZ_LEAVE_MS       15000u /* "Asked it to leave": no answer by then is NO_ANSWER */
#define ZBZ_PING_MS        10000u
#define ZBZ_IDENTIFY_S     10u
#define ZBZ_CHANNEL_MS     30000u /* a channel move: the panel's radio on the new channel */
#define ZBZ_RESET_MS       60000u
#define ZBZ_CFG_KEY        "cfg"
#define ZBZ_CFG_BLOB       4u
#define ZBZ_CFG_COMPAT     1u
/* ZB-M3 (additive): a stranger turned away is logged once an hour per address, for this many
 * addresses at once; and the IAS zone id the setup's Enroll Response gives an endpoint the zone
 * source keeps no slot for (a siren's, a keypad's): ZBZ_ZONE_ID_OWN + its registry slot. */
#define ZBZ_REFUSED_MAX    4u
#define ZBZ_REFUSED_LOG_MS 3600000u
#define ZBZ_ZONE_ID_OWN    0x80u
/* The ZB-M10 wiring (additive): "Test it" waits this long for the end of its Start Warning (the
 * radio's REP_DONE), then shows NO_ANSWER. */
#define ZBZ_SIREN_TEST_MS  ZBW_CONFIRM_MS

/* The NVS budget (devices.md §4; decided 2026-10-02 without a review). NVS keeps
 * 32-byte entries, 126 to a 4 KiB page; a blob costs an index entry, then for each chunk of at
 * most 4000 bytes a header entry and its bytes in whole entries. The 128 KiB partition is 32
 * pages, one kept empty by NVS itself: 31 x 126 entries. */
#define ZBZ_NVS_ENTRY_BYTES   32u
#define ZBZ_NVS_CHUNK_BYTES   4000u
#define ZBZ_NVS_PAGE_ENTRIES  126u
#define ZBZ_NVS_USABLE        (31u * ZBZ_NVS_PAGE_ENTRIES)
#define ZBZ_NVS_ENTRIES(bytes)                                                                  \
    (1u + ((bytes) + ZBZ_NVS_CHUNK_BYTES - 1u) / ZBZ_NVS_CHUNK_BYTES +                          \
     ((bytes) + ZBZ_NVS_ENTRY_BYTES - 1u) / ZBZ_NVS_ENTRY_BYTES)
/* zbpanel at its largest: the namespace, meta, 64 records, codes, cfg, wd, ace (686). */
#define ZBZ_NVS_WORST                                                                           \
    (1u + ZBZ_NVS_ENTRIES(ZBZ_META_LEN) + ZBZ_DEV_MAX * ZBZ_NVS_ENTRIES(ZBZ_RECORD_MAX) +       \
     ZBZ_NVS_ENTRIES(ZBZ_CODES_BLOB_MAX) + ZBZ_NVS_ENTRIES(ZBZ_CFG_BLOB) +                      \
     ZBZ_NVS_ENTRIES(ZBW_BLOB_MAX) + ZBZ_NVS_ENTRIES(ZBA_BLOB_MAX))
/* What Zigbee never takes: the alarm configuration rewritten whole at its largest (NVS writes the
 * new value before it erases the old one) and one more registry record. */
#define ZBZ_NVS_RESERVE                                                                         \
    (ZBZ_NVS_ENTRIES(ALARM_CONFIG_BLOB_MAX) + ZBZ_NVS_ENTRIES(ZBZ_RECORD_MAX))
/* Free entries under this: no new device, no new install code (FULL). */
#define ZBZ_NVS_ADD_MIN       (ZBZ_NVS_RESERVE + ZBZ_NVS_ENTRIES(ZBZ_RECORD_MAX))
#define ZBZ_NVS_NOT_TOLD      UINT32_MAX /* zbz_panel_init's: no limit until the owner tells */

/* The signal buckets (devices.md §7): from the last frame's LQI, as the radio reports it. */
#define ZBZ_LQI_GOOD 150u /* and above: good */
#define ZBZ_LQI_FAIR 80u  /* and above: fair; below: weak */

/* What zbz_panel_dirty() reports. */
#define ZBZ_DIRTY_REGISTRY 0x01u /* zbz_reg_take hands the keys out one by one */
#define ZBZ_DIRTY_CODES    0x02u
#define ZBZ_DIRTY_CFG      0x04u
#define ZBZ_DIRTY_WD       0x08u
#define ZBZ_DIRTY_ACE      0x10u

/* The installer's settings (NVS cfg). */
typedef struct {
    bool channel_set;      /* a channel was chosen (ZB-016); else the build's mask decides */
    uint8_t channel;
} zbz_cfg_t;

/* The radio as the service sees it (its bring-up, the restart guard, the stack's network). */
typedef struct {
    uint8_t state;         /* ui_zb_radio_t */
    uint8_t paused_why;    /* ui_zb_pause_t */
    uint8_t restarts;      /* the guard's count */
    char version[32];      /* the radio's firmware version; "" not known */
    bool jammed;           /* zsrc_zb_jammed */
    bool noise_known;
    int8_t noise_dbm;
    uint16_t learn_s;      /* the join window's seconds left */
} zbz_radio_info_t;

/* A device's live state (not stored), by registry slot. */
typedef struct {
    uint8_t lqi;
    int8_t rssi;
    uint64_t heard_at;     /* 0: not since the panel started */
    uint8_t tries;         /* the current setup step's attempts */
    uint8_t ep_next;       /* the setup's next endpoint to describe or bind */
    uint32_t tag;          /* the request out for it, 0 none */
    uint64_t step_deadline;
    uint64_t setup_since;
    bool asleep;           /* the setup waits for it to wake up */
    /* ---- ZB-M3 (additive) ---- */
    uint8_t ep_list[ZSRC_ZB_EP_MAX]; /* its active endpoints (ACTIVE_EP's answer) */
    uint8_t ep_list_n;
    bool ep_list_known;    /* ACTIVE_EP answered since the setup started */
    /* ---- the review of wave 6 (additive) ---- */
    bool src_full;         /* the zone source had no room for one of its sensor endpoints (its
                            * add row ends FULL, not ADDED) */
    /* ---- B021 (additive) ---- */
    uint64_t key_wait;     /* the setup sends nothing before this (ZBZ_KEY_WAIT_MS); 0 none */
} zbz_live_t;

/* An event for the log (devices.md §11): the owner records it through the engine
 * (alarm_engine_platform_event_ex) once the ALARM_EV_ZIGBEE_* types exist (ZB-M5). */
typedef struct {
    uint8_t type;          /* zbz_event_t until then */
    uint16_t zone;         /* the first zone it fed, 0 none */
    uint16_t device;       /* the registry id */
    uint8_t arg;
    uint8_t user;          /* who asked; 0 the panel */
    uint8_t via;           /* where they asked it: UI_ZB_VIA_PANEL or UI_ZB_VIA_WEB */
} zbz_log_t;

/* The events the panel logs (devices.md §11); ZB-M5 appends ALARM_EV_ZIGBEE_* for them. */
typedef enum {
    ZBZ_EV_ADDED = 1,      /* arg: zsrc_zb_join_t */
    ZBZ_EV_REMOVED,        /* asked to leave and left */
    ZBZ_EV_FORGOTTEN,      /* removed without its answer */
    ZBZ_EV_LEFT,           /* it left by itself */
    ZBZ_EV_REPLACED,       /* arg: unused; device: the new one */
    ZBZ_EV_JOIN_REFUSED,   /* a stranger was turned away */
    ZBZ_EV_CHANNEL,        /* arg: the new channel */
    ZBZ_EV_NETWORK_RESET,
    ZBZ_EV_RADIO_RESTARTED, /* arg: 0 asked, 1 by itself, 2 the guard paused it */
} zbz_event_t;

typedef struct {
    void *ctx;
    /* A zone's binding (the hub's), for the zones a device feeds; false: none. */
    bool (*binding)(void *ctx, uint16_t zone, zone_hub_binding_t *out);
    /* Replace (ZB-013): the zone's binding moved to the new device's endpoint (the hub's
     * zone_hub_bind, refused while armed). */
    bool (*rebind)(void *ctx, uint16_t zone, const zone_hub_binding_t *b);
    /* The wall clock, Unix seconds; 0 when it isn't set. */
    uint32_t (*wall_clock)(void *ctx);
    /* Optional: something a page or the Panel Manager shows changed. */
    void (*changed)(void *ctx);
    /* Optional: the engine device numbers other parts of the panel hold, bit n-1 for device n -
     * the panel-link keypads' (docs/features/11 LINK-001) and those the installer turned on in
     * the engine's device table (alarm_config_t.devices[n-1].enabled). Called in the zigbee task
     * with the zone lock held: answered from a copy the owner keeps (it never waits for the alarm
     * task). NULL: none. A keypad is never given one of them (zbz_ace.h). */
    uint32_t (*devices_taken)(void *ctx);
    /* ---- the ZB-M11 wiring (additive) ---- */
    /* A keypad's or fob's command for the alarm task (zba_post_t, its code with it): copy it into
     * the owner's queue and return - never run the engine here (the zigbee task, zone lock held).
     * The panel wipes `post` after the call; the owner wipes its copy once posted. The alarm
     * task's answer comes back through zbz_panel_ace_result with post->ticket. False (or NULL):
     * not queued - the command is forgotten and the device's retry runs again. */
    bool (*ace_post)(void *ctx, const zba_post_t *post);
    /* ---- review 2, ZB21/RF32 (additive) ---- */
    /* True: a replacement found (ZB-013) moves no zone by itself - it waits, the window open,
     * for zbz_panel_replace_gated, which claims the binding gate around every rebind (the owner's
     * alarm task can arm meanwhile). False (the preview, the simulator): the rebinds go at once,
     * the hub's own refusal the only check. */
    bool replace_waits;
} zbz_panel_port_t;

typedef struct {
    zbz_panel_port_t port;
    zsrc_zb_t *src;                 /* the zone source (the owner registers it with the hub) */
    zsrc_zb_radio_t radio;          /* the same radio the source has */
    zbz_registry_t reg;
    zbz_live_t live[ZBZ_DEV_MAX];
    zbz_codes_t codes;
    zbz_cfg_t cfg;
    zbw_t wd;
    zba_t ace;
    zbz_radio_info_t info;
    bool network_known;
    uint8_t channel;
    uint16_t pan_id;
    uint64_t ext_pan_id, panel_ieee;
    uint8_t survey_known;
    int8_t survey_dbm[ZSRC_ZB_CHANNELS];
    uint64_t now;
    uint8_t dirty;
    uint16_t seq;
    /* the operation the pages follow */
    ui_zb_progress_t progress;
    uint64_t deadline;              /* its countdown's end; 0 none */
    uint8_t op_user;
    uint8_t op_via;                 /* UI_ZB_VIA_*: where op_user asked it */
    uint32_t op_tag;
    uint64_t op_ieee;               /* ADD_CODE: the one device listened for */
    uint16_t replacing;             /* REPLACE: the device whose zones the next one takes */
    /* the log */
    zbz_log_t log[ZBZ_LOG_MAX];
    size_t log_n;
    uint32_t log_lost;
    /* the NVS budget */
    uint32_t nvs_free;              /* free entries as last told; ZBZ_NVS_NOT_TOLD */
    /* ---- ZB-M3 (additive) ---- */
    bool describe_owed;             /* the radio came up: the registry is described to the source
                                     * once its link is up */
    bool restart_down;              /* RESTART/RETRY: the radio has gone down since it was asked */
    uint64_t up_at;                 /* when the radio last came up; 0 not since the start */
    bool op_wait_frame;             /* PING of a sleepy device: its next frame answers */
    uint8_t op_channel;             /* CHANNEL: the channel asked for */
    struct {
        uint64_t ieee;
        uint64_t at;
    } refused[ZBZ_REFUSED_MAX];     /* strangers turned away, as last logged */
    /* ---- the ZB-M10/ZB-M11 wiring (additive) ---- */
    zbw_port_t wd_owner;            /* the owner's siren port as given: its bell_trouble */
    zba_port_t ace_owner;           /* the owner's keypad port as given: its default_rsp */
    bool part_known[ALARM_MAX_PARTITIONS]; /* a view of the partition came since the start */
    zba_view_t part_view[ALARM_MAX_PARTITIONS]; /* ... the last one */
    struct {
        uint16_t id;                /* the siren's registry id, 0 none */
        uint8_t result;             /* ui_zb_result_t of its last "Test it" (RAM only) */
    } siren_test[ZBW_SIRENS_MAX];
    uint16_t test_id;               /* SIREN_TEST: the siren the operation tests */
    /* ---- the review of wave 6 (additive) ---- */
    uint64_t wd_ping_at[ZBW_SIRENS_MAX]; /* when each siren (wd.siren[k]) was last read to keep
                                          * it heard (ZB-023); 0 never */
    /* ---- review 2, ZB21/RF32 (additive) ---- */
    uint16_t replace_with;          /* REPLACE (port.replace_waits): the device found to take the
                                     * zones, waiting for the gate; 0 none */
} zbz_panel_t;

/* ---- life cycle -------------------------------------------------------------------------------- */

/* Ready, not started: the source (already initialised with the same radio), the radio, the
 * owner's siren and keypad ports (NULL: none; only wd->bell_trouble and ace->default_rsp are
 * used, see above). */
void zbz_panel_init(zbz_panel_t *p, const zbz_panel_port_t *port, zsrc_zb_t *src,
                    const zsrc_zb_radio_t *radio, const zbw_port_t *wd, const zba_port_t *ace);
/* What is stored (before the radio is up): the registry, the codes, cfg, wd, ace. Each part
 * stands alone: a damaged one is empty, the others load. The registry's verdict. */
zbz_reg_load_t zbz_panel_load(zbz_panel_t *p, const zbz_store_port_t *store);
/* The radio's state, as the service's bring-up and the guard see it (each change, and each
 * second while a window is open). Coming up: every waiting install code is given again, every
 * registry device is described to the source again (zsrc_zb_device_seen from its record), and
 * setups left STOPPED stay stopped; the sirens' silence counts only while it works (zbw_radio). */
void zbz_panel_radio(zbz_panel_t *p, uint64_t now, const zbz_radio_info_t *info);
/* A report from the radio side (zsrc_zigbee.h): joins and leaves, descriptors, Basic, link
 * quality, check-ins, ACE frames (to zba), Default Responses (to zbw), the end of requests. The
 * sensor traffic still goes straight to the source's zsrc_zb_* calls. */
void zbz_panel_on_report(zbz_panel_t *p, uint64_t now, const zsrc_zb_report_t *r);
/* Everything due at now: setup steps and their timeouts, the operation's countdown, the sirens
 * and the keypads. */
void zbz_panel_poll(zbz_panel_t *p, uint64_t now);
/* When to poll again: 0 at once, UINT64_MAX not before news. */
uint64_t zbz_panel_next_deadline(const zbz_panel_t *p);

/* ---- requests ----------------------------------------------------------------------------------- */

/* A page's (or the console's, or the Panel Manager's) request: started, done, or why not. */
ui_zb_req_result_t zbz_panel_request(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                     bool allowed);
/* The request result for the engine's answer to RESET's installer code (ZB-017), `authority`
 * being the code's user's: OK only for ALARM_OK from the installer (ALARM_AUTH_INSTALLER - a
 * master's or anyone else's valid code is WRONG_CODE), LOCKED_OUT for ALARM_ERR_LOCKED_OUT,
 * WRONG_CODE for everything else (an empty code included). Pure. */
ui_zb_req_result_t zbz_reset_code_result(alarm_result_t r, alarm_authority_t authority);
/* Whether a request of this kind (ui_zb_req_kind_t) needs `allowed`: all but PING, IDENTIFY,
 * SURVEY and DISMISS. Pure. */
bool zbz_panel_needs_allowed(uint8_t kind);

/* ---- the binding gate (ZB21, RF32) --------------------------------------------------------------- */

/* The owner's side of zone_hub's binding gate (zone_bind_gate.h) and of the zone lock, for the
 * two calls below. reserve waits for the alarm task: it is called holding no lock. claim, end and
 * the lock's calls never wait for the alarm task; pause lets a tick pass, nothing held. One
 * reservation at a time across the panel (the owner's reserve answers BUSY, after a bounded wait,
 * while another change holds it), and end ends only the caller's own. */
typedef struct {
    void *ctx;
    zone_bind_reserve_t (*reserve)(void *ctx);
    zone_bind_claim_t (*claim)(void *ctx);
    void (*end)(void *ctx);
    void (*lock)(void *ctx);   /* the zone lock (the panel's) */
    void (*unlock)(void *ctx);
    void (*pause)(void *ctx);
    /* Optional: after the request's answer, still under the lock and the claim (when there is
     * one) - the owner's own part of it, e.g. RESTART's stack stop. */
    void (*after)(void *ctx, const ui_zb_request_t *req, ui_zb_req_result_t r);
} zbz_gate_port_t;

/* A gate busy with another change is asked again this many times, a pause between, before the
 * answer is BUSY; a claim the alarm task's step keeps answering RETRY gives up after
 * ZBZ_GATE_PAUSES pauses (the claim's own ZONE_BIND_GATE_MS ends it first). */
#define ZBZ_GATE_TRIES  3u
#define ZBZ_GATE_PAUSES 2000u

/* zbz_panel_request with `allowed` kept true through the change (ZB21, ZB-002): called holding no
 * lock. A request that needs `allowed` (zbz_panel_needs_allowed) and has it reserves the gate,
 * takes the lock, claims, runs zbz_panel_request and `after`, ends the claim and lets the lock
 * go; arming waits from the claim to the end - the request's own work and its radio hook's post,
 * never a reservation. Armed since the snapshot (at the reservation or before the claim):
 * zbz_panel_request without `allowed` - NOT_ALLOWED, the radio never called. A gate still busy
 * after ZBZ_GATE_TRIES: BUSY, nothing done. Any other request runs under the lock alone. */
ui_zb_req_result_t zbz_panel_request_gated(zbz_panel_t *p, uint64_t now, const ui_zb_request_t *req,
                                           bool allowed, const zbz_gate_port_t *gate);

/* A replacement waits for the gate (port.replace_waits): call under the zone lock. */
bool zbz_panel_replace_waiting(const zbz_panel_t *p);

/* RF32: a waiting replacement, under a claimed gate - called holding no lock, after each pass that
 * may have found one (reports, polls). The device is checked again under the lock and the claim;
 * then every zone moves, or none (a zone the hub refuses puts back the ones already moved, under
 * the same claim: never refused by the gate), and the old device goes: REPLACED, or FAILED. Armed
 * meanwhile, or the gate still busy: FAILED, nothing moved, the old device keeps its zones and
 * the new one stays added. The claim ends before the lock goes (the owner's unlock may store).
 * Nothing waiting: nothing happens, no reservation. */
void zbz_panel_replace_gated(zbz_panel_t *p, uint64_t now, const zbz_gate_port_t *gate);

/* ---- the device numbers and the NVS budget ------------------------------------------------------ */

/* The engine device numbers the Zigbee keypads hold (bit n-1: device n), for the panel's other
 * users of the shared numbers (the panel-link pairing, the installer's device table). Reads only. */
uint32_t zbz_panel_devices_held(const zbz_panel_t *p);
/* NVS's free entries, as the owner reads them (nvs_get_stats, entries NVS can still give out) at
 * start, before the radio is up, and after each write it makes for anyone. */
void zbz_panel_nvs_free(zbz_panel_t *p, uint32_t entries);

/* ---- the engine's news, for sirens and keypads (posted from the alarm task) --------------------- */

/* The engine's bell and strobe outputs (on_output): to the sirens (zbw_output). */
void zbz_panel_output(zbz_panel_t *p, uint64_t now, uint8_t partition, alarm_output_t out,
                      alarm_pattern_t pattern);
/* A partition's view (the alarm task posts it at start-up and whenever the partition's state
 * changes). Armed is any state but disarmed (exit delay included): the first view since the start
 * sets the sirens' armed state silently (zbw_set_armed), a later change of it squawks (zbw_armed);
 * the keypads hear of it (zba_panel) only when something in it but `at` changed. */
void zbz_panel_partition(zbz_panel_t *p, uint64_t now, uint8_t partition, const zba_view_t *view);
/* The alarm task's answer to a keypad's post (zba_result). */
bool zbz_panel_ace_result(zbz_panel_t *p, uint64_t now, uint32_t ticket, alarm_result_t result,
                          const alarm_cmd_result_t *detail);
/* A walk test started or ended (ZB-022): to the source (zsrc_zb_walk_test). On alone puts no
 * sensor into IAS test mode: zbz_panel_walk_scope names the zones the walk test covers. */
void zbz_panel_walk_test(zbz_panel_t *p, uint64_t now, bool on);
/* The zones the running walk test covers (ZB-022, docs/features/40 WALK-007), bit z-1 for zone z:
 * each in use, of a partition whose walk test runs, of a type that test counts
 * (alarm_zone_type_walk_counts with its walk_flags) - never fire or CO, panic and 24-hour zones
 * only with "Include panic and 24-hour zones". From the engine's configuration and snapshot (the
 * alarm task's); all clear when no walk test runs. */
void zbz_walk_covered(const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                      uint8_t covered[ALARM_ZONE_BITMAP_BYTES]);
/* Each Zigbee input the source watches goes in or out of the walk test's scope
 * (zsrc_zb_walk_test_input): in only when every zone bound to it, in any role, is covered. Call it
 * after zbz_panel_walk_test(on) and again whenever the covered zones or the bindings change; NULL
 * covered: none. */
void zbz_panel_walk_scope(zbz_panel_t *p, uint64_t now,
                          const uint8_t covered[ALARM_ZONE_BITMAP_BYTES]);

/* ---- what the pages show (they only read) ------------------------------------------------------ */

void zbz_panel_status(const zbz_panel_t *p, uint64_t now, ui_zb_status_t *out);
size_t zbz_panel_devices(const zbz_panel_t *p, uint64_t now, ui_zb_device_t *out, size_t max);
/* False: no such device (removed since). */
bool zbz_panel_device(const zbz_panel_t *p, uint64_t now, uint16_t id, ui_zb_device_page_t *out);
void zbz_panel_progress(const zbz_panel_t *p, uint64_t now, ui_zb_progress_t *out);
size_t zbz_panel_codes(const zbz_panel_t *p, ui_zb_code_t *out, size_t max);
/* The device a zone's sensor is (its registry id), 0 none: the zone page's "Zigbee device" row. */
uint16_t zbz_panel_device_of_zone(const zbz_panel_t *p, uint16_t zone);
/* The signal bucket for an LQI (ui_zb_signal_t). Pure. */
uint8_t zbz_signal_of(uint8_t lqi);
/* ZB-M3 (additive): what the panel uses a device as (zbz_role_t), from what its record holds -
 * the model table's role first, then its endpoints: an IAS ACE client is a keypad (a key fob
 * when its zone type or its model says so), an IAS WD server whose endpoint is a warning device
 * a siren, an input the zones can read (IAS Zone, Occupancy, Temperature, On/Off but not an
 * actuator's) a sensor; else a router repeats and anything else is OTHER. Pure. */
uint8_t zbz_role_of(const zbz_device_t *d);

/* ---- what is to be stored and logged ------------------------------------------------------------ */

uint8_t zbz_panel_dirty(const zbz_panel_t *p);
/* The registry's next key (zbz_reg_take). */
zbz_take_t zbz_panel_take_registry(zbz_panel_t *p, char key[ZBZ_KEY_LEN], uint8_t *buf,
                                   size_t cap, size_t *len);
/* The other values, encoded, their dirty bit cleared; 0 when nothing is due or buf is too small
 * (the bit then stays). A write that failed is asked for again with zbz_panel_redo. */
size_t zbz_panel_take_codes(zbz_panel_t *p, uint8_t *buf, size_t cap); /* wipe buf after use */
size_t zbz_panel_take_cfg(zbz_panel_t *p, uint8_t *buf, size_t cap);
size_t zbz_panel_take_wd(zbz_panel_t *p, uint8_t *buf, size_t cap);
size_t zbz_panel_take_ace(zbz_panel_t *p, uint8_t *buf, size_t cap);
void zbz_panel_redo(zbz_panel_t *p, uint8_t dirty);
/* The next event to log, oldest first; false when none. */
bool zbz_panel_take_log(zbz_panel_t *p, zbz_log_t *out);

size_t zbz_cfg_encode(const zbz_cfg_t *c, uint8_t *out, size_t cap);
bool zbz_cfg_decode(zbz_cfg_t *c, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_PANEL_H */
