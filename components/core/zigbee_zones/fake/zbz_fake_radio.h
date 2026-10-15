/*
 * zbz_fake_radio.h - a pretend Zigbee radio and the virtual devices behind it (docs/features/20
 * testing.md §2, §7, §8; ZB-007, ZB-012, ZB-022). Host builds only: the host tests and, later, the
 * PC preview and simulator link it (CMake target zigbee_zones_fake); the firmware never does.
 *
 * The radio is a zsrc_zb_radio_t (zbf_hooks) whose every hook ENCODES the frame the real radio
 * would send - ZCL with zbz_zcl.h, ZDO by the Zigbee specification's field layouts (2.4.3) - into
 * a log of frames. A virtual device, built from one row of the device signatures
 * (test/signatures, tools/zb_signatures.py), DECODES those bytes, answers with bytes of its own,
 * and the radio's panel side decodes the answers with the same decoders the firmware's
 * radio_port uses and hands them on as the real one does: zsrc_zb_report_t to a report sink
 * (zbz_panel_on_report in the firmware; zsrc_zb_on_report when the caller gives none) and the
 * source's inbound calls (zsrc_zb_ias_status, zsrc_zb_battery, ...) to the zsrc_zb_t.
 *
 * Time is simulated: nothing happens inside a hook (the firmware queues too, and a hook is called
 * with the zone lock held). Every frame spends ZBF_LATENCY_MS on the air; zbf_advance() moves the
 * clock and delivers what is due, in order, each at its own time. A request no one answers ends
 * ZBF_TIMEOUT_MS after it was sent (REP_DONE TIMEOUT); one to a sleepy device that is asleep is
 * held by its parent for ZBF_HOLD_MS (macTransactionPersistenceTime) and delivered only if the
 * device wakes meanwhile - by its own frame (a report, a check-in, an answer, a join), which
 * keeps it listening for its wake window.
 *
 * Fault knobs per device and per setup step (zbf_fault): drop the next N requests, answer late,
 * answer twice with one sequence number, answer with another sequence number, answer a failure
 * status, never answer; and zbf_repeat() sends a device's last frame again unchanged.
 *
 * What is not real bytes: the stack's own signals (a device joined, was refused, left by itself)
 * are logged as ZBF_FRAME_NWK entries with the event's fields, since the firmware never sees
 * them as frames either. Install-code joins check the code's bytes, not its key derivation.
 *
 * Portable C11, no heap, caller-owned state (zbf_radio_t is large: keep it static).
 */
#ifndef ZBZ_FAKE_RADIO_H
#define ZBZ_FAKE_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zigbee_zones/zbz_zcl.h"
#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBF_DEV_MAX     16u    /* virtual devices on one radio */
#define ZBF_QUEUE_MAX   64u    /* frames on the air at once */
#define ZBF_RQ_MAX      32u    /* the panel's requests waiting for their answer */
#define ZBF_LOG_MAX     96u    /* frames the log keeps (the oldest go first) */
#define ZBF_CODES_MAX   8u     /* install codes the trust center holds */
#define ZBF_FRAME_MAX   80u    /* the longest frame: a Simple_Desc_rsp with 16 + 16 clusters */

#define ZBF_LATENCY_MS  10u    /* a frame's time on the air */
#define ZBF_TIMEOUT_MS  10000u /* a request without an answer by then ends TIMEOUT */
#define ZBF_HOLD_MS     7680u  /* a sleeping device's frames wait at its parent this long */
#define ZBF_WAKE_MS     5000u  /* a sleepy device listens this long after its own frame */
#define ZBF_PANEL_EP    1u     /* the panel's endpoint (binds, the CIE) */
#define ZBF_PANEL_IEEE  0x02ABCD00000000F0ull /* the fake coordinator (fictional) */

/* ZDO cluster ids (Zigbee 2.4.3; a response is its request | 0x8000). */
#define ZBF_ZDO_NODE_DESC   0x0002u
#define ZBF_ZDO_POWER_DESC  0x0003u
#define ZBF_ZDO_SIMPLE_DESC 0x0004u
#define ZBF_ZDO_ACTIVE_EP   0x0005u
#define ZBF_ZDO_ANNOUNCE    0x0013u
#define ZBF_ZDO_BIND        0x0021u
#define ZBF_ZDO_LEAVE       0x0034u
#define ZBF_ZDO_PERMIT_JOIN 0x0036u
#define ZBF_ZDO_NWK_UPDATE  0x0038u
#define ZBF_ZDO_RSP         0x8000u

/* ZDP status codes (Zigbee 2.4.5) a fault may answer. */
#define ZBF_ZDP_SUCCESS       0x00u
#define ZBF_ZDP_INV_REQUEST   0x80u
#define ZBF_ZDP_NOT_FOUND     0x81u
#define ZBF_ZDP_INVALID_EP    0x82u
#define ZBF_ZDP_NOT_ACTIVE    0x83u
#define ZBF_ZDP_NOT_SUPPORTED 0x84u
#define ZBF_ZDP_TIMEOUT       0x85u
#define ZBF_ZDP_NO_DESCRIPTOR 0x89u
#define ZBF_ZDP_TABLE_FULL    0x8Cu
/* ZCL statuses zbz_zcl.h has no name for. */
#define ZBF_ZCL_UNREPORTABLE  0x8Cu
#define ZBF_ZCL_UNSUP_CLUSTER 0xC3u

/* The stack's signals, logged as ZBF_FRAME_NWK (cluster = the event). */
#define ZBF_NWK_JOINED  1u /* bytes: join method, rejoin, parent u16 */
#define ZBF_NWK_REFUSED 2u /* bytes: why */
#define ZBF_NWK_LEFT    3u /* bytes: rejoin */
#define ZBF_NWK_ENERGY  4u /* the panel's own radio: an energy scan's result (dbm) */
#define ZBF_NWK_CHANNEL 5u /* ... on its new channel (channel, update id) */
#define ZBF_NWK_RESET   6u /* ... a new network formed (PAN id) */
#define ZBF_NWK_SURVEY  7u /* ... the four channels' energy */

typedef enum {
    ZBF_FRAME_ZCL = 0, /* a ZCL frame: header | payload (zbz_zcl.h) */
    ZBF_FRAME_ZDO,     /* a ZDO frame: transaction sequence number | fields */
    ZBF_FRAME_NWK,     /* one of the stack's signals (ZBF_NWK_*) */
} zbf_frame_kind_t;

/* One frame on the air, as the log keeps it. */
typedef struct {
    uint64_t at;       /* when it was sent */
    bool rx;           /* from a device to the panel; false: the panel's */
    uint8_t kind;      /* zbf_frame_kind_t */
    uint64_t ieee;     /* the device (0: a broadcast, or the panel's own radio) */
    uint16_t nwk;
    uint8_t endpoint;  /* the device's endpoint (0: ZDO, NWK) */
    uint16_t cluster;  /* ZCL cluster, ZDO cluster (ZBF_ZDO_*), or ZBF_NWK_* */
    uint32_t tag;      /* the request it carries or answers (0: none) */
    uint8_t len;
    uint8_t bytes[ZBF_FRAME_MAX];
} zbf_frame_t;

/* What a device does with requests (zbf_fault). All zero: it behaves. */
typedef struct {
    uint8_t drop;      /* the next N requests never reach it (each ends TIMEOUT) */
    uint32_t delay_ms; /* its answers leave this much later */
    bool duplicate;    /* each answer is sent twice, with the same sequence number */
    bool other_tsn;    /* each answer carries another sequence number: it matches no request */
    uint8_t status;    /* answer this status (ZCL, or ZDP for a ZDO request); 0 success */
    bool timeout;      /* it never answers, until the knob is cleared */
} zbf_fault_t;

/* One endpoint of a virtual device: its descriptor and what it holds. */
typedef struct {
    zsrc_zb_simple_desc_t sd;
    uint16_t zone_type;  /* IAS Zone Type; ZSRC_ZB_ZONE_TYPE_NONE without IAS Zone */
    bool enrolled;       /* IAS Zone State */
    uint8_t zone_id;
    uint64_t cie;        /* the CIE address written to it, 0 none */
    uint16_t status;     /* IAS Zone Status, without the TEST bit (added while in test mode) */
    uint64_t test_until; /* in IAS test mode until then (0: normal operation) */
    bool on;             /* On/Off, Occupancy */
    int16_t temp_x100;
    uint32_t warnings;   /* IAS WD Start Warning commands received */
    uint32_t commands;   /* cluster commands received on this endpoint */
    zsrc_zb_zcl_t last_cmd; /* the last cluster command it received, decoded */
} zbf_ep_t;

typedef struct {
    bool used;
    uint16_t sig;            /* the signature row it was built from */
    uint64_t ieee;
    uint16_t nwk;
    zsrc_zb_node_desc_t node;
    uint8_t power_source;    /* Basic 0x0007 */
    char manufacturer[32];
    char model[32];
    uint8_t ep_count;
    zbf_ep_t ep[ZSRC_ZB_EP_MAX];

    bool joined;
    uint8_t join;            /* zsrc_zb_join_t of its last join */
    bool sleepy;             /* answers only inside its wake window */
    bool auto_enroll;        /* sends a Zone Enroll Request once its CIE address is written */
    uint32_t wake_ms;        /* its wake window (ZBF_WAKE_MS) */
    uint64_t awake_until;
    uint32_t check_in_s;     /* Poll Control check-ins this often (0: none of its own) */
    uint64_t next_check_in;
    uint8_t battery_pct;     /* 0..100; ZSRC_ZB_BATTERY_UNKNOWN never said */
    uint8_t lqi;             /* of its frames at the panel (0: not measured) */
    int8_t rssi;
    uint8_t tsn;             /* its own frames' sequence numbers */
    uint8_t code_len;        /* its install code, as on its label (0: none) */
    uint8_t code[18];
    zbf_fault_t fault[ZSRC_ZB_STEP_DONE + 1]; /* [step]; [ZSRC_ZB_STEP_NONE] any request */
    zbf_frame_t last;        /* its last frame (zbf_repeat) */
    bool has_last;
    uint32_t requests;       /* requests that reached it */
    /* ---- the replays (ZB-M3, test_zbz_replay.c; additive) ---- */
    uint8_t listed_only_ep;  /* an endpoint its Active_EP_rsp lists after its own and that has no
                              * simple descriptor (Simple_Desc_rsp NOT_ACTIVE: Aqara's H1
                              * switches do this); 0 none */
    /* ---- ZB03 (additive): its IAS WD MaxDuration (0x0000) answer ---- */
    bool wd_max_set;         /* wd_max_s is what it answers; false: the ZCL default, 240 */
    uint16_t wd_max_s;       /* any u16, 0xFFFF (ZCL's invalid value) included */
    uint8_t wd_max_type;     /* the type it answers with; 0: uint16 (ZBZ_ZCL_TYPE_U16) */
} zbf_dev_t;

/* Where the radio's panel side delivers. */
typedef struct {
    void *ctx;
    /* Every report. NULL: straight to zsrc_zb_on_report of the source. */
    void (*report)(void *ctx, const zsrc_zb_report_t *r, uint64_t now);
    /* Every attribute value read or reported outside Basic (zone type, zone state, CIE address,
     * check-in interval...), after the source's inbound call. Strings are not passed. May be
     * NULL. */
    void (*attr)(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint16_t attr,
                 uint64_t value, uint64_t now);
} zbf_sink_t;

/* Internal: a frame on the air, and a request waiting for its end. */
typedef struct {
    bool used;
    bool to_dev;     /* to a device; false: to the panel */
    bool held;       /* waiting at a sleeping device's parent */
    uint64_t at;     /* due */
    uint64_t sent;
    uint32_t seq;    /* order among frames due together */
    uint8_t step;    /* zsrc_zb_step_t of the request (to a device) */
    zbf_frame_t f;
} zbf_air_t;

typedef struct {
    bool used;
    uint64_t ieee;
    uint8_t kind;    /* zbf_frame_kind_t */
    uint16_t cluster;
    uint8_t tsn;
    uint8_t want;    /* the ZCL answer it waits for (read, write, configure, default response) */
    bool any_answer; /* a Default Response ends it too */
    bool on_delivery; /* no answer asked for: it ends when the device has it (the APS ack) */
    uint32_t tag;
    uint8_t step;
    uint64_t deadline;
} zbf_rq_t;

typedef struct {
    zsrc_zb_t *src;
    zbf_sink_t sink;
    uint64_t now;
    bool unplugged;
    bool up;           /* zbf_up: the network is formed */

    uint64_t panel_ieee;
    uint16_t pan_id;
    uint64_t ext_pan_id;
    uint8_t channel;
    uint8_t update_id;
    uint64_t join_until; /* the join window */
    bool codes_only;
    struct {
        bool used;
        uint64_t ieee;
        uint8_t len;
        uint8_t code[18];
    } codes[ZBF_CODES_MAX];
    int8_t energy_dbm;   /* what an energy scan of the network's channel finds */
    int8_t survey_dbm[ZSRC_ZB_CHANNELS];

    uint8_t tsn;         /* the panel's ZCL sequence numbers */
    uint8_t zdo_tsn;
    uint32_t seq;
    uint32_t lost;       /* frames lost because the air was full */

    zbf_dev_t dev[ZBF_DEV_MAX];
    zbf_air_t air[ZBF_QUEUE_MAX];
    zbf_rq_t rq[ZBF_RQ_MAX];
    zbf_frame_t log[ZBF_LOG_MAX];
    size_t log_head;     /* the oldest kept */
    size_t log_len;
    uint32_t log_total;  /* every frame ever logged */
} zbf_radio_t;

/* ---- the radio --------------------------------------------------------------------------------- */

/* A radio at `now`, its network not formed yet. `src` gets the inbound calls (NULL: none); `sink`
 * the reports (NULL or sink->report NULL: zsrc_zb_on_report of src). */
void zbf_init(zbf_radio_t *r, zsrc_zb_t *src, const zbf_sink_t *sink, uint64_t now);
/* The hooks for zsrc_zb_init (ctx = r). */
zsrc_zb_radio_t zbf_hooks(zbf_radio_t *r);
/* The network is formed: REP_NETWORK, and the source's link comes up. */
void zbf_up(zbf_radio_t *r);
/* The radio module unplugged (true) or back (false): every hook fails while it is out, requests
 * in flight end NO_RADIO, the air empties and the source's link goes down; back, as zbf_up. */
void zbf_unplug(zbf_radio_t *r, bool unplugged);
/* Moves the clock to `now`, delivering everything due on the way, each at its own time. */
void zbf_advance(zbf_radio_t *r, uint64_t now);
/* The earliest time something is due (UINT64_MAX: nothing). */
uint64_t zbf_next_due(const zbf_radio_t *r);
/* The panel's next ZCL / ZDO sequence numbers (to test the wrap from 0xFF to 0x00). */
void zbf_set_tsn(zbf_radio_t *r, uint8_t zcl, uint8_t zdo);
void zbf_set_energy(zbf_radio_t *r, int8_t dbm);

/* The frames, oldest first; i < zbf_log_count. */
size_t zbf_log_count(const zbf_radio_t *r);
const zbf_frame_t *zbf_log_at(const zbf_radio_t *r, size_t i);
void zbf_log_clear(zbf_radio_t *r);
/* The newest frame the panel sent to `ieee` (0: any), NULL none. */
const zbf_frame_t *zbf_last_tx(const zbf_radio_t *r, uint64_t ieee);

/* ---- the signatures ---------------------------------------------------------------------------- */

size_t zbf_signature_count(void);
/* A row's file name ("frient_kepzb_110.json") and device name ("Frient KEPZB-110"). */
const char *zbf_signature_file(size_t i);
const char *zbf_signature_device(size_t i);

/* ---- the virtual devices ----------------------------------------------------------------------- */

/* A device built from the signature row named `name`: its file name with or without ".json", or
 * its device name. Not joined yet. NULL: no such row, no room, or that address already there. */
zbf_dev_t *zbf_add(zbf_radio_t *r, const char *name);
zbf_dev_t *zbf_add_row(zbf_radio_t *r, size_t row);
zbf_dev_t *zbf_find(zbf_radio_t *r, uint64_t ieee);
zbf_ep_t *zbf_ep(zbf_dev_t *d, uint8_t endpoint);

/* It joins through the trust center: with its install code when the trust center holds the same
 * one (ZSRC_ZB_JOIN_INSTALL_CODE), else the well-known key - refused (REP_JOIN_REFUSED) when only
 * devices with codes may join. false: refused, the join window closed, the radio out, or already
 * joined. REP_JOINED, then its Device_annce. */
bool zbf_join(zbf_radio_t *r, zbf_dev_t *d);
/* It rejoins with a new short address (0: keeps its own). */
bool zbf_rejoin(zbf_radio_t *r, zbf_dev_t *d, uint16_t nwk);
/* It leaves by itself (REP_LEFT, not asked), saying whether it will rejoin. */
void zbf_leave(zbf_radio_t *r, zbf_dev_t *d, bool rejoin);
/* Its install code, as on its label (8, 10, 14 or 18 bytes; 0 none). */
void zbf_set_code(zbf_dev_t *d, const uint8_t *code, uint8_t len);

/* Its own frames (each opens its wake window); false: not joined, or it has no such cluster. */
bool zbf_check_in(zbf_radio_t *r, zbf_dev_t *d); /* Poll Control check-in, else Device_annce */
bool zbf_trip(zbf_radio_t *r, zbf_dev_t *d, bool on); /* IAS Alarm1, else Occupancy, On/Off */
bool zbf_ias_status(zbf_radio_t *r, zbf_dev_t *d, uint8_t endpoint, uint16_t status);
bool zbf_set_battery(zbf_radio_t *r, zbf_dev_t *d, uint8_t percent);
bool zbf_set_temperature(zbf_radio_t *r, zbf_dev_t *d, int16_t value_x100);
/* A cluster command it sends (a keypad's IAS ACE Arm): `cmd` client to server. */
bool zbf_send(zbf_radio_t *r, zbf_dev_t *d, uint8_t endpoint, const zsrc_zb_zcl_t *cmd);
/* Its last frame again, the same bytes and sequence number (a retry the panel already heard). */
bool zbf_repeat(zbf_radio_t *r, zbf_dev_t *d);

/* Knobs. */
void zbf_set_link(zbf_dev_t *d, uint8_t lqi, int8_t rssi);
void zbf_set_sleepy(zbf_dev_t *d, bool sleepy, uint32_t wake_ms);
void zbf_set_check_in(zbf_radio_t *r, zbf_dev_t *d, uint32_t seconds);
/* The fault for requests of `step` (zsrc_zb_step_t); ZSRC_ZB_STEP_NONE: every request whose own
 * step has none. NULL clears it. */
void zbf_fault(zbf_dev_t *d, uint8_t step, const zbf_fault_t *f);
/* It is listening now (a router, or a sleepy device inside its wake window). */
bool zbf_awake(const zbf_radio_t *r, const zbf_dev_t *d);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_FAKE_RADIO_H */
