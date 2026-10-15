/*
 * zw_net.h - the Z-Wave network the panel runs (docs/features/36): the host (the link to the
 * module, zw_host.h), the S2 security manager (zw_s2.h), adding devices (zw_incl.h), the node
 * cache (zw_nodes.h), the interview (zw_iv.h), sleeping devices and supervision (zw_health.h)
 * and removing devices (zw_remove.h), wired together. The owner - the zwave task (Z8), the tests
 * and the PC simulator - feeds it the link's news and the clock and hears back through one port.
 *
 * Routing: the module's AddNode and RemoveNode status reports go to the inclusion and the
 * removal; ApplicationCommandHandler frames (04, A8) that are singlecast and from this network
 * (serial-api.md §6.11) go to the security manager, whose key-exchange commands go to the
 * inclusion and everything else to the interview, supervision and the owner; ApplicationUpdate's
 * node information (84, 81) to the interview and supervision; other module frames to the owner.
 *
 * The network (network.md §2). At every link up the module's node list is read again
 * (GetSerialApiInitData: a module that restarted by itself only gets steps 13-19 of the start-up)
 * and compared with the node cache and the keys' home ID (zw_node_reconcile): a first start binds
 * fresh keys (the crypto port's random: the panel's RNG, ZW-017) to the module's home ID and hands
 * them to the owner (store_keys); a normal start forgets the nodes the module no longer lists
 * (their zones show CHECK "sensor removed") and interviews the ones the cache lacks (a new
 * generation, classes probed); a replaced or reset module is reported with the number of devices
 * to add again - its cache kept until the installer adds a device (then fresh keys, never the old
 * network's); a module from another system is reported and none of its nodes is used (adding is
 * refused; reset it).
 *
 * The node cache is stored through the owner (store_nodes, with the inclusion counter): when a
 * device joins (network.md §4 step 5), when its key exchange ends, when an interview step
 * completes, when devices are removed or forgotten, never because of a report. The owner loads it
 * with zw_net_load_nodes() before zw_net_start().
 *
 * Sleeping devices (network.md §7) through zw_health: commands to a sleeping device - the
 * interview's, the owner's - wait in its wake-up queue until it wakes (a Wake Up Notification, its
 * node information, or a Get from it other than a Nonce Get; a report is no wake-up), No More
 * Information follows 250 ms after the last one (never while its interview asks it something), a late wake-up
 * has its interval read again. The interval set is the installer's check-in (config, or per device
 * through the port) within the device's limits (zw_health_choose_interval). A device's own
 * check-in chosen later on its page (zw_net_set_check_in) waits in its queue for its next wake-up
 * and counts - its record, the stored cache, supervision - only once the device confirmed it.
 * Supervision (§8): pings of listening devices, the unreachable clocks, REACHABLE/UNREACHABLE for
 * the owner.
 *
 * SmartStart (network.md §4.2, zw_net_smartstart): while the owner wants it (a device on its
 * provisioning list waits, every partition disarmed) and no operation runs, the module listens
 * for SmartStart requests; a request whose NWI home ID the owner's list knows (port
 * smartstart_find) is included at once - with Security 2 on the entry's terms - as its own
 * operation; one that ends without its keys is removed again as failed (the device resets itself
 * and asks again). SMARTSTART says how each ended. Any other operation turns the listen mode off
 * first; it comes back when nothing runs.
 *
 * Network operations (network.md §1): one at a time - add, remove, remove failed, region, reset,
 * restart, a SmartStart join, a power change; a second is refused (BUSY). A device's Device Reset Locally (5A 01) marks it gone at
 * once and removes it as failed as soon as no other operation runs. ZW-005/016/018's "installer
 * mode with every partition disarmed" is the caller's check, passed in as `allowed`, and so is the
 * network reset's "confirmed twice": the owner asks twice before it calls zw_net_reset_network().
 *
 * Portable C11, caller-owned state (about 84 KB: PSRAM in the firmware - the node cache 42 KB, the
 * security manager 14 KB, zw_health 13 KB), no heap.
 */
#ifndef ZW_NET_H
#define ZW_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_health.h"
#include "zwave_host/zw_host.h"
#include "zwave_host/zw_incl.h"
#include "zwave_host/zw_iv.h"
#include "zwave_host/zw_jam.h"
#include "zwave_host/zw_nodes.h"
#include "zwave_host/zw_remove.h"
#include "zwave_host/zw_routes.h"
#include "zwave_host/zw_s2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The tag the inclusion's own Security 2 sends carry. Every tag whose top byte is FF is zw_net's
 * own (the inclusion's, the interview's, removals', supervision's): not the owner's to use. */
#define ZW_NET_INCL_TAG 0xFFFFFFFEu
#define ZW_NET_OWN_TAGS 0xFF000000u

/* network.md §7: the installer's check-in when the config names none; a device's own
 * (zw_net_set_check_in) from 10 min to 24 h (its page offers 1, 4 and 12 hours). */
#define ZW_NET_CHECK_IN_S     3600u
#define ZW_NET_CHECK_IN_MIN_S 600u
#define ZW_NET_CHECK_IN_MAX_S 86400u

/* Queued commands to sleeping devices at once, the node-to-frame map for "every ACK". */
#define ZW_NET_QUEUE_OUT 4u
#define ZW_NET_FRAMES    24u
#define ZW_NET_SIGNALS   ZW_NODES_MAX /* nodes whose last signal readings are kept */
#define ZW_NET_PINGS     8u
#define ZW_NET_GONE      8u  /* devices that reset themselves, waiting for their removal */
#define ZW_NET_GONE_WAIT_MS 500u /* ...which starts this long after their notification (Z-Wave JS
                                  * DeviceResetLocallyCC.ts: its Supervision Report goes first) */
#define ZW_NET_CHECK_INS ZW_NODES_MAX /* devices' own check-ins on their way: one a device */

/* §2, ZW-017: a first start whose network keys could not be drawn, or whose security manager
 * could not start on them, stops (nothing bound, stored or usable) and is tried again this long
 * later: keys drawn then if their draw failed, the unbound set kept if the manager's start failed
 * (ZW01). */
#define ZW_NET_KEY_RETRY_MS 30000u

/* §8: a ping is a No Operation with the normal 3 attempts. */
#define ZW_NET_PING_ATTEMPTS 3u

/* The radio's standing after a link up (network.md §2 "At every start"). */
typedef enum {
    ZW_NET_RADIO_UNKNOWN = 0, /* not checked yet */
    ZW_NET_RADIO_OURS,        /* home ID and keys agree (or a new network: fresh keys bound) */
    ZW_NET_RADIO_REPLACED,    /* "The Z-Wave radio was replaced or reset: add its N devices
                               * again" */
    ZW_NET_RADIO_FOREIGN      /* "This Z-Wave radio belongs to another system: reset it to use
                               * it here" */
} zw_net_radio_t;

typedef enum {
    ZW_NET_EV_NETWORK = 1,      /* the radio's standing: `radio`, `count` (REPLACED: the devices
                                 * to add again), `first_start` (a new network) */
    ZW_NET_EV_NODE_ADDED,       /* a node has a record: joined (§4 step 5) or found in the
                                 * module (§2): `node`, `generation` */
    ZW_NET_EV_NODE_READY,       /* its interview is done: `inputs` at end point 0 (zw_iv_inputs()
                                 * for the others) */
    ZW_NET_EV_WAKE_NEEDED,      /* §5: "Wake the device to finish": questions wait for it */
    ZW_NET_EV_STALLED,          /* §5: 5 attempts failed at `step`: it stays interviewing */
    ZW_NET_EV_NOT_USABLE,       /* another controller: "Remove it", it feeds no zone */
    ZW_NET_EV_NODE_REMOVED,     /* §10: it left, forgotten: its zones CHECK "sensor removed" */
    ZW_NET_EV_NODE_GONE,        /* §10 reset locally / §2 missing from the module: its zones CHECK
                                 * "sensor removed" (`gone`) */
    ZW_NET_EV_REMOVE,           /* a removal's progress or end (`remove`) */
    ZW_NET_EV_UNREACHABLE,      /* §8: the zone source tells the hub it cannot reach it */
    ZW_NET_EV_REACHABLE,
    ZW_NET_EV_RESET,            /* §12: the network was reset (logged); the start-up follows */
    ZW_NET_EV_REGION,           /* §12: the region `region` set (store it); the start-up follows */
    ZW_NET_EV_RESTART,          /* §12: "Restart the radio" began (log it); the start-up follows,
                                 * then NETWORK */
    ZW_NET_EV_CHECK_IN,         /* §7: a device's own check-in (zw_net_set_check_in) ended:
                                 * `check_in_s`, `took`; also (`took`) its interval read again
                                 * after a late wake-up changed - 0: it no longer checks in */
    ZW_NET_EV_SMARTSTART,       /* §4.2: a SmartStart join ended - `smart`, `dsk` (the entry's),
                                 * `node` and `generation` when it joined */
    ZW_NET_EV_JAM,              /* §9: the radio is jammed now, or no longer: `jammed` */
    ZW_NET_EV_ROUTES,           /* §11: rebuilding routes - a device began or ended, or the job
                                 * ended (`routes`) */
    ZW_NET_EV_POWER,            /* §12 power (phase 3): read (`power`), or a change ended
                                 * (`power_set`: SET_TOOK, SET_REFUSED; the start-up follows one
                                 * that took, then a read) */
    ZW_NET_EV_OTW               /* §14: the radio's own firmware update ended (`otw`); the
                                 * start-up follows, then NETWORK */
} zw_net_event_kind_t;

/* §12 power (phase 3), tenths of a dBm: what the module said at the last read. */
typedef struct {
    bool known;              /* read since this link up (a change forgets it until read again) */
    bool sixteen_bit;        /* the module has the 16-bit forms (13, 12) */
    int16_t dbm10;           /* classic Z-Wave transmit power */
    int16_t cal_dbm10;       /* its measured output at 0 dBm, sent back unchanged */
    bool lr_known;           /* the Long Range maximum was read (the module has 05) */
    int16_t lr_dbm10;
} zw_net_power_t;

typedef enum {
    ZW_NET_POWER_READ = 1,   /* `power` is the module's now */
    ZW_NET_POWER_SET_TOOK,   /* the module took the new power: its soft reset and a read follow */
    ZW_NET_POWER_SET_REFUSED /* it refused, or did not answer: nothing changed */
} zw_net_power_ev_t;

typedef enum {
    ZW_NET_SMART_JOINED = 1,    /* in the network with its keys: its interview follows */
    ZW_NET_SMART_FAILED,        /* joined without its keys: removed again as failed (a failure) */
    ZW_NET_SMART_NOT_JOINED     /* the protocol did not finish (a failure; it asks again) */
} zw_net_smart_t;

typedef enum {
    ZW_NET_GONE_MISSING = 1,    /* the module no longer lists it */
    ZW_NET_GONE_RESET_LOCALLY   /* it said so (Device Reset Locally): removed as failed next */
} zw_net_gone_t;

typedef struct {
    zw_net_event_kind_t kind;
    uint16_t node;
    uint16_t generation;       /* the node's (zones.md §1): its zones' addresses */
    uint32_t home_id;
    zw_net_radio_t radio;      /* NETWORK */
    bool first_start;          /* NETWORK: a new network (fresh keys) */
    uint16_t count;            /* NETWORK (REPLACED): the devices to add again */
    uint8_t step;              /* WAKE_NEEDED, STALLED */
    zw_iv_inputs_t inputs;     /* NODE_READY */
    zw_net_gone_t gone;        /* NODE_GONE */
    zw_remove_event_t remove;  /* REMOVE */
    uint8_t region;            /* REGION */
    uint32_t check_in_s;       /* CHECK_IN: the interval asked of the device, s, as it was set
                                * (clamped and rounded to its limits) - its interval now when
                                * `took`; or its interval read again after a late wake-up
                                * changed (0: no longer supervised, ZW-015) */
    bool took;                 /* CHECK_IN: the device confirmed it, or its interval read again
                                * changed: its record, the stored cache
                                * and supervision (§8) use it from now on. False: it answered
                                * "fail", nothing, or the Set could not go - nothing changed, it
                                * keeps the interval it had; not sent again */
    zw_net_smart_t smart;      /* SMARTSTART */
    uint8_t dsk[16];           /* SMARTSTART: the provisioning entry's DSK */
    bool jammed;               /* JAM */
    zw_routes_event_t routes;  /* ROUTES */
    zw_net_power_ev_t power_ev; /* POWER */
    zw_net_power_t power;      /* POWER */
    zw_otw_progress_t otw;     /* OTW */
} zw_net_event_t;

typedef struct {
    void *ctx;
    /* zw_host_port_t's: bytes to the UART; the reset line (NULL: none wired). */
    bool (*write)(void *ctx, const uint8_t *bytes, size_t len);
    void (*reset_line)(void *ctx, bool asserted);
    /* The link: up, down, the module restarted by itself. */
    void (*link_up)(void *ctx, const zw_module_facts_t *facts);
    void (*link_down)(void *ctx, const zw_link_down_t *why);
    void (*restarted)(void *ctx, uint8_t reason);
    /* A command from a node that passed the security rules. */
    void (*received)(void *ctx, const zw_s2_rx_t *rx);
    /* zw_net_send()'s outcome (for a sleeping device: after its wake-up). */
    void (*send_done)(void *ctx, uint32_t tag, zw_s2_result_t result);
    /* The inclusion's news (zw_incl_event_t). */
    void (*incl_event)(void *ctx, const zw_incl_event_t *ev);
    /* Optional: a node ID the owner counts as known besides the node cache; module frames
     * nobody else took; what the security manager dropped. */
    bool (*known)(void *ctx, uint16_t node);
    void (*frame)(void *ctx, const zw_frame_t *frame);
    void (*s2_event)(void *ctx, uint16_t node, zw_s2_event_t ev);
    /* Optional: the network's news (zw_net_event_t). */
    void (*event)(void *ctx, const zw_net_event_t *ev);
    /* Optional: store the node cache and the inclusion counter (NVS zwave/nodes, Z8): encode it
     * with zw_node_cache_encode(). */
    void (*store_nodes)(void *ctx, const zw_node_cache_t *nodes, uint16_t generation);
    /* Optional: store the network keys and the home ID they belong to (NVS zwave/keys). */
    void (*store_keys)(void *ctx, uint32_t home_id, const zw_s2_keys_t *keys);
    /* Optional: the quirks table (zones.md §8, Z6): the interview's lifeline group and
     * Configuration parameters for a model. */
    bool (*quirk)(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out);
    /* Optional: a device's own check-in (s; network.md §7 "per device on its page"); 0: the
     * config's. The owner keeps it (what it last passed to zw_net_set_check_in); the interview
     * sets it at the device's next interview. */
    uint32_t (*check_in)(void *ctx, const zw_node_record_t *rec);
    /* Optional (SmartStart, §4.2): the owner's active, waiting provisioning entry whose DSK gives
     * this NWI home ID with this protocol - its DSK and the keys to grant. False: not ours. */
    bool (*smartstart_find)(void *ctx, const uint8_t nwi[4], bool long_range,
                            zw_incl_smartstart_t *out);
} zw_net_port_t;

typedef struct {
    zw_host_config_t host;
    zw_s2_keys_t keys;       /* the stored keys when keys_bound */
    bool keys_bound;         /* the keys belong to keys_home_id (NVS zwave/keys); false: none
                              * stored - fresh ones are made at the first start */
    uint32_t keys_home_id;
    uint16_t generation;     /* the inclusion counter as stored (0: none yet) */
    uint32_t check_in_s;     /* the installer's check-in, s (0: ZW_NET_CHECK_IN_S) */
    bool interview;          /* interview devices (§5) and supervise them (§7, §8): the firmware;
                              * false leaves them as the inclusion left them (lower-layer tests) */
    bool jam;                /* §9: watch for jamming (GetBackgroundRSSI every 5 s, transmit
                              * failures); jam_dbm its threshold, 0: ZW_JAM_THRESHOLD_DBM */
    int8_t jam_dbm;
} zw_net_config_t;

typedef struct {
    uint32_t not_singlecast;   /* broadcast and multicast commands not taken: an unknown node's,
                                * Security 2 ones, Get-type ones (ZW-042) */
    uint32_t foreign;          /* from another network (receive status bits 6-7), dropped */
    uint32_t malformed;        /* ApplicationCommandHandler frames that did not parse */
    uint32_t s2_start_failures; /* the security manager could not start at a link up, or a first
                                 * start's keys could not be drawn (ZW01) */
    uint32_t checks;           /* node lists read and reconciled */
    uint32_t stores;           /* node cache writes asked of the owner */
    uint32_t queued;           /* commands held for a sleeping device */
    uint32_t pings;            /* supervision pings sent (attempts counted) */
    uint32_t no_more_info;     /* Wake Up No More Information sent */
    uint32_t other_controller; /* ApplicationUpdate 40/20: a node added or removed by another
                                * controller - ignored (the panel is the network's only
                                * controller, serial-api.md §6.13), handed to the owner's
                                * `frame` for its log */
    uint32_t smartstart_asks;  /* SmartStart requests heard (ApplicationUpdate 85/87) */
    uint32_t smartstart_ignored; /* ...not on the list, or not now (it asks again) */
    uint32_t jam_resets;       /* soft resets of a module jammed for 5 transmissions
                                * (serial-api.md §6.12) */
    uint32_t unsupported_cc;   /* Meter or Multilevel Sensor commands from a device (end point)
                                * without that CC, discarded (ZW-009, Driver.ts shouldDiscardCC) */
} zw_net_stats_t;

/* A command of the wake-up queue out (network.md §7). */
typedef struct {
    bool used;
    bool nif;             /* a RequestNodeInfo (its answer: ApplicationUpdate 84/81) */
    bool answered;        /* RequestNodeInfo: the module sent it */
    bool retry;           /* the security manager was busy: send again at the next poll */
    bool superseded;      /* a newer command of its kind was queued meanwhile: its end is its own */
    uint16_t node;
    uint32_t kind;        /* zw_health's key */
    uint32_t tag;         /* its origin's tag (the interview's, the owner's) */
    uint8_t cls;
    uint8_t len;
    uint8_t cmd[ZW_HEALTH_QUEUE_ENTRY_BYTES];
} zw_net_qout_t;

/* A device's own check-in on its way (network.md §7, zw_net_set_check_in): its Wake Up Interval
 * Set in the device's wake-up queue, then the device's word on it. */
typedef struct {
    uint16_t node;        /* 0: free */
    bool went;            /* its Set has left at least once: a Supervision Report can be its */
    bool out;             /* its Set is out: its end awaited */
    bool waiting;         /* its Set was delivered: its Supervision Report - without Supervision,
                           * to a device that lists Application Status, that one's word - awaited
                           * until `deadline` (No More Information held meanwhile) */
    bool retry;           /* the device answered Application Busy: the Set goes again at
                           * `retry_at` (No More Information held meanwhile) */
    bool supervised;      /* the Set is wrapped in a Supervision Get of `session` */
    uint8_t session;      /* the device's Supervision sessions, 1..63 from a random start */
    uint8_t busy;         /* Application Busy answers so far (ZW_IV_TRIES end it) */
    uint32_t seconds;     /* the interval it sets */
    uint32_t tag;         /* its queue entry's (one of zw_net's own) */
    uint64_t sent_at;     /* when it left last: the round trip counts from here */
    uint64_t deadline;
    uint64_t retry_at;
} zw_net_checkin_t;

/* ZW-040: what the radio last measured of one node, in memory only. `rx_*`: the RSSI byte of the
 * last singlecast frame from this network the node sent; `tx_*`: the TX report of the last
 * delivered SendData to it (the node's ACK, the repeaters used). Times are the net's clock, ms.
 * RSSI is signed dBm; a reserved value (not available, saturated, no signal) is never kept. */
typedef struct {
    bool has_rx;
    int8_t rx_rssi;
    uint64_t rx_at_ms;
    bool has_tx;
    int8_t tx_rssi;
    uint8_t tx_repeaters;          /* 0..4 hops */
    uint8_t tx_repeater_ids[4];
    int8_t tx_repeater_rssi[4];    /* ZW_RSSI_NOT_AVAILABLE where the report had none */
    uint64_t tx_at_ms;
} zw_net_signal_t;

typedef struct {
    zw_net_port_t port;
    zw_net_config_t cfg;
    zw_s2_keys_t keys;
    bool keys_ready;         /* `keys` are usable: bound, or fresh */
    bool keys_bound;
    uint32_t keys_home_id;
    uint64_t key_retry_at;   /* ZW01: a stopped first start's check again then (0: none) */
    uint16_t generation;
    zw_host_t host;
    zw_s2_t s2;
    zw_incl_t incl;
    zw_iv_t iv;
    zw_remove_t rm;
    zw_health_t health;
    zw_node_cache_t nodes;
    uint64_t now;
    /* network.md §2 */
    zw_net_radio_t radio;
    bool checked;            /* this link up's node list read and reconciled */
    uint8_t check_tries;
    uint16_t check_gen;
    /* network.md §1 */
    uint8_t op;              /* the network operation running (zw_net.c) */
    uint16_t op_gen;
    uint16_t gone[ZW_NET_GONE]; /* devices reset locally, their removal waiting */
    size_t gone_n;
    uint64_t gone_after;     /* ...and starting no sooner than this (ZW_NET_GONE_WAIT_MS) */
    bool gone_held;          /* ZW-014: ...and not while set (the owner's node cache unstored) */
    uint16_t joined;         /* the node that joined last (it is awake) */
    /* network.md §7, §8 */
    zw_net_qout_t qout[ZW_NET_QUEUE_OUT];
    struct {
        uint32_t tag;        /* its SendData's tag (the security manager's) */
        uint16_t node;
    } frames[ZW_NET_FRAMES];
    struct {
        uint16_t node;       /* 0: free */
        uint8_t attempts;
        bool out;
    } pings[ZW_NET_PINGS];
    uint16_t nif_node;       /* a listening device's RequestNodeInfo out */
    uint32_t nif_tag;
    uint16_t stamp;          /* stamps zw_net's own tags */
    uint16_t reply_stamp;    /* stamps replies queued for sleeping devices (their own kinds) */
    bool telling_replaced;   /* ZW13: inside the CANCELLED of a command replaced in a queue */
    zw_net_checkin_t checkins[ZW_NET_CHECK_INS];
    uint16_t checkin_stamp;  /* stamps the check-ins' tags */
    bool store_due;
    uint16_t hold_awake;     /* zw_net_hold_awake: a device kept awake after its queue empties */
    zw_routes_t routes;      /* §11 */
    bool routes_held;        /* hold_awake is the route repair's (a sleeper being repaired) */
    zw_jam_t jam;            /* §9 */
    uint64_t jam_next;       /* the next background noise reading */
    bool jam_out;            /* ...its GetBackgroundRSSI out */
    bool jam_reset;          /* serial-api.md §6.12: this jam episode's soft reset is behind (a
                              * delivery or the air clear again ends the episode) */
    bool ss_want;            /* §4.2: the owner wants SmartStart (zw_net_smartstart) */
    bool ss_on;              /* the module's SmartStart listen mode is on, as far as known */
    zw_net_power_t power;    /* §12 power (phase 3), zw_net_power.c */
    uint8_t power_out;       /* its reads out (a bit a read: 1 classic, 2 Long Range) */
    bool power_reread;       /* read again once the link is up and checked (after a change) */
    struct {
        uint16_t node;       /* 0: free */
        zw_net_signal_t sig;
    } signals[ZW_NET_SIGNALS]; /* ZW-040 */
    zw_net_stats_t stats;
} zw_net_t;

void zw_net_init(zw_net_t *net, const zw_net_port_t *port, const zw_net_config_t *cfg,
                 const zw_crypto_port_t *crypto);
/* The stored node cache (before zw_net_start). False when the blob is damaged: the cache is then
 * empty (zw_node_cache_decode). */
bool zw_net_load_nodes(zw_net_t *net, const uint8_t *blob, size_t len);
/* The link's start-up (zw_host_start). */
void zw_net_start(zw_net_t *net, uint64_t now);

void zw_net_on_control(zw_net_t *net, uint64_t now, uint8_t byte);
void zw_net_on_frame(zw_net_t *net, uint64_t now, const zw_frame_t *frame);
void zw_net_on_text(zw_net_t *net, uint64_t now, const uint8_t *bytes, size_t len);
void zw_net_poll(zw_net_t *net, uint64_t now);
uint64_t zw_net_next_deadline(const zw_net_t *net);

/* A node for the security manager alone: its granted classes (ZW_S2_KEY_*) and node
 * information's CC list (the owner's own registration; the node cache's nodes are registered at
 * every link up). */
bool zw_net_add_node(zw_net_t *net, uint16_t node, uint8_t granted, const uint8_t *nif,
                     size_t nif_len);
/* A command to a node (zw_s2_send); a sleeping device's waits in its wake-up queue (§7) - but a
 * reply the panel owes it (a Supervision Report, zw_s2_command_is_reply) does not wait for its
 * next wake-up: the device that sent the Get listens now, so the reply goes first in its queue at
 * once (No More Information behind it, nothing replacing it), and in the reply class, ahead of
 * the node commands queued; one the device no longer hears ends FAILED, never kept for its next
 * wake-up. In the queue a newer command of the same kind (CC, command, first parameter; for Multi
 * Channel encapsulation the destination end point and the inner command's) replaces an older one:
 * the older's owner hears ZW_S2_CANCELLED at once if it never went (ZW13), its real end if it was
 * out; a command sent from inside that CANCELLED which would replace another in turn is refused
 * (ZW_S2_BUSY), so one replacement never sets off another. An encapsulation it does not look into
 * replaces nothing. `tag` must not be one of zw_net's own (ZW_NET_OWN_TAGS). */
zw_s2_result_t zw_net_send(zw_net_t *net, uint64_t now, uint16_t node, zw_sec_class_t cls,
                           const uint8_t *cmd, size_t len, uint32_t tag);

/* "Add a device" (zw_incl_start with the node-ID width in effect): `allowed` is ZW-005's check,
 * the node counts lengthen ProtocolDone's timeout. The installer's answers go to
 * zw_incl_grant / zw_incl_pin / zw_incl_cancel / zw_incl_stop on zw_net_incl(). */
zw_incl_result_t zw_net_add_device(zw_net_t *net, uint64_t now, bool allowed,
                                   uint16_t listening_nodes, uint16_t flirs_nodes);

/* ZW-014 (zones.md §1): whether a new device can still get a generation no device of this
 * network had - the 16-bit counter is below 65535. It never wraps: once used up, a node the cache
 * lacks gets no record (no generation, so it feeds no zone) and the owner refuses new devices. */
bool zw_net_generations_left(const zw_net_t *net);

/* §10 "Replace a failed device": a node of the cache takes a new device. It checks the old one
 * first, as for removing a failed device (zw_remove_check_failed: 3 pings 2 s apart - any answer
 * refuses it - then IsFailedNode), holding the network operation through the check so nothing
 * else starts; only a node the module then lists as failed goes to ReplaceFailedNode
 * (zw_incl_replace / zw_incl_replace_checked). The check's or the radio's refusal ends it
 * STILL_ANSWERS, NOT_FAILED, BUSY or REFUSED (BUSY at once while a removal runs). Replaced, its
 * generation stays, so its zones follow the new device; its record starts again (classes,
 * identity, security) and it is interviewed. The installer's answers go to zw_incl_* on
 * zw_net_incl(), as for "Add a device". */
zw_incl_result_t zw_net_replace_failed(zw_net_t *net, uint64_t now, bool allowed, uint16_t node);

/* "Remove a device" (exclusion, own or foreign devices) and "Remove a failed device" (a node of
 * the cache); "Keep listening" and "Stop" go to zw_remove_* on zw_net_rm(). */
zw_remove_result_t zw_net_remove_device(zw_net_t *net, uint64_t now, bool allowed);
zw_remove_result_t zw_net_remove_failed(zw_net_t *net, uint64_t now, bool allowed,
                                        uint16_t node);

typedef enum {
    ZW_NET_STARTED = 0,  /* zw_net_set_check_in: queued for the device's next wake-up */
    ZW_NET_NOT_ALLOWED,  /* not in installer mode with every partition disarmed */
    ZW_NET_BUSY,         /* another network operation runs (zw_net_set_check_in: the device's
                          * wake-up queue is full) */
    ZW_NET_LINK_DOWN,
    ZW_NET_UNSUPPORTED,  /* a region the module does not offer */
    ZW_NET_UNCHANGED,    /* the module is in that region already */
    ZW_NET_INVALID,      /* zw_net_set_check_in: not a device whose check-in can be set, or
                          * seconds outside ZW_NET_CHECK_IN_MIN_S..ZW_NET_CHECK_IN_MAX_S;
                          * zw_net_set_power: a value the module can't take */
    ZW_NET_TOO_HIGH      /* zw_net_set_power: above the cap (the power the radio came with) */
} zw_net_result_t;

/* §12: the region (one of the module's, zw_module_facts_t.regions): SetRFRegion, soft reset and
 * the start-up again. The warning "Devices added in another region will stop working" is the
 * page's (zw_node_count(zw_net_nodes())). */
zw_net_result_t zw_net_set_region(zw_net_t *net, uint64_t now, bool allowed, uint8_t region);
/* §12: reset the network - HardReset: every device loses the panel; the cache is cleared; new
 * keys (these, or fresh ones from the crypto port when NULL) are bound to the new home ID at the
 * start-up that follows. `allowed`: installer, disarmed, confirmed twice (the caller's). */
zw_net_result_t zw_net_reset_network(zw_net_t *net, uint64_t now, bool allowed,
                                     const zw_s2_keys_t *keys);
/* §12 "Restart the radio" (screens.md §8, radio details): the start-up again from step 0 - its
 * step 12 is the soft reset - as for a region change, without SetRFRegion. The devices, their
 * keys, the SPANs and the node cache stay; the check at the link up (§2) finds them, NETWORK says
 * how it went. With the link down (the radio not answering) it is "try now": the start-up begins
 * at once instead of at the recovery's next attempt (serial-api.md §7). Refused when not allowed
 * (the caller's installer check) and while another network operation runs (BUSY). */
zw_net_result_t zw_net_restart(zw_net_t *net, uint64_t now, bool allowed);

/* §7 a device's own check-in (its page: 1, 4 or 12 hours), applied at its next wake-up ("Changes
 * when it next wakes up"): Wake Up Interval Set (`seconds` clamped and rounded to the device's
 * limits, zw_health_choose_interval; the panel as destination) queued in its wake-up queue at §8's
 * class for Wake Up - its highest - and wrapped in Supervision exactly when the interview's Sets
 * are (zw_iv_supervised); a newer call replaces one not yet confirmed. When the device confirms it
 * (the interview's rule: delivered without Supervision, else a Supervision Report other than
 * "fail" - "working" waits 1 s more) its record takes the interval, the cache is stored and
 * zw_health judges it by it; until then, and when it does not take, the old interval counts. A
 * Set the device does not hear (no ACK) waits for its next wake-up (§7); "fail", no Report in 1 s
 * plus the round trip, or a Set that cannot go end it - not sent again. Either way CHECK_IN says
 * how it ended. The wanted value is the owner's to keep (port.check_in).
 * Not a network operation: never refused because one runs, and it blocks none. Refused:
 * NOT_ALLOWED; LINK_DOWN (the link not up, or its node list not checked yet); INVALID - a node the
 * cache lacks, a listening device, one without Wake Up, one still interviewing (or whose
 * interview stalled), one gone, one at interval 0 ("wake only by hand": §7 leaves it alone), or
 * `seconds` outside 600..86400; BUSY only when the device's wake-up queue is full (zw_health's 8
 * a device, 128 in all). */
zw_net_result_t zw_net_set_check_in(zw_net_t *net, uint64_t now, bool allowed, uint16_t node,
                                    uint32_t seconds);

/* §4.2 SmartStart: `listen` - the owner's provisioning list has a device waiting to join and every
 * partition is disarmed (the caller's checks; call again whenever either changes). The listen
 * mode follows at once when nothing else runs, else when the operation ends. */
void zw_net_smartstart(zw_net_t *net, uint64_t now, bool listen);
/* Whether the module is in SmartStart listen mode (as far as the panel knows). */
bool zw_net_smartstart_listening(const zw_net_t *net);

/* §11 "Rebuild routes": one device (`node`), or every device (0) - a network operation (BUSY
 * while another runs); sleepers are repaired as they wake, so the job can wait for them:
 * zw_net_stop_routes() ends it. ROUTES events follow it. */
zw_net_result_t zw_net_rebuild_routes(zw_net_t *net, uint64_t now, bool allowed, uint16_t node);
void zw_net_stop_routes(zw_net_t *net, uint64_t now);
const zw_routes_t *zw_net_routes(const zw_net_t *net);

/* §12 power (phase 3). Reading: GetPowerlevel (the 16-bit form when the module has it) and, when
 * it has it, the Long Range maximum; POWER (READ) follows. Not a network operation: refused only
 * with the link not up (or not checked yet) and while a read is out.
 * Changing it: the classic power (`long_range` false) or the Long Range maximum, in tenths of a
 * dBm, never above `cap_dbm10` - the power the radio had when the panel first read it, the
 * maker's legal setting (the owner keeps it): TOO_HIGH. INVALID: outside -10..+20 dBm (classic;
 * a signed byte without the 16-bit form) or not 10, 14 or 20 dBm (Long Range), or the module
 * lacks the command, or nothing read yet. A network operation (BUSY, NOT_ALLOWED, LINK_DOWN as
 * the others); UNCHANGED when it is so already. The calibration goes back as read. The module
 * applies it at its next start: a Set it takes is followed by the start-up (its soft reset,
 * step 12) and a read once the link is up; POWER says how the Set ended. */
bool zw_net_power_read(zw_net_t *net, uint64_t now);
const zw_net_power_t *zw_net_power(const zw_net_t *net);
zw_net_result_t zw_net_set_power(zw_net_t *net, uint64_t now, bool allowed, bool long_range,
                                 int16_t dbm10, int16_t cap_dbm10);

/* §14 the radio's own firmware (zw_host_otw_begin): a network operation - BUSY while another
 * runs; NOT_ALLOWED as the others (the caller's installer, disarmed and arming-held checks);
 * LINK_DOWN when the radio is neither up nor waiting in its bootloader; INVALID for a file that
 * isn't a whole .gbl. `image` is the caller's until OTW. The devices, their keys and the node
 * cache stay: the check at the link up that follows finds them (NETWORK). */
zw_net_result_t zw_net_update_radio(zw_net_t *net, uint64_t now, bool allowed,
                                    const uint8_t *image, size_t len);
const zw_otw_progress_t *zw_net_otw(const zw_net_t *net);

/* §9: jammed now (cfg.jam); the last background noise reading (zw_jam_t's rssi, channels). */
bool zw_net_jammed(const zw_net_t *net);
const zw_jam_t *zw_net_jam(const zw_net_t *net);
/* ZW-040: the last signal readings of a node (false: none yet). Not stored; gone with the node. */
bool zw_net_node_signal(const zw_net_t *net, uint16_t node, zw_net_signal_t *out);
/* `zwave jam <dBm>`: the threshold the quietest channel must reach (-100..-20 dBm). */
bool zw_net_set_jam_threshold(zw_net_t *net, int8_t dbm);

/* Interview a node again from step 1 (the device page's "interview again"). */
bool zw_net_interview(zw_net_t *net, uint64_t now, uint16_t node);
/* The same without waiting for a sleeper's wake-up (zw_iv_restart_now): after a software update,
 * as Z-Wave JS. */
bool zw_net_interview_now(zw_net_t *net, uint64_t now, uint16_t node);
/* Continue a node's interview from the step it stopped at (network.md §5: an interview that
 * stalled after its attempts, or was never finished): the device page's "Try its setup again".
 * True when it runs (also when it was running already); false when the node is unknown, set up
 * already, or no interview slot is free. */
bool zw_net_resume_interview(zw_net_t *net, uint64_t now, uint16_t node);
/* Keep a sleeping device awake once it wakes (its Wake Up No More Information held back, as during
 * its interview) while the device itself drives an exchange the panel only answers - a firmware
 * update's fragments (ZW-033). One device at a time: another node, or 0, lets the one before
 * sleep again (its No More Information follows). */
void zw_net_hold_awake(zw_net_t *net, uint64_t now, uint16_t node);

zw_host_t *zw_net_host(zw_net_t *net);
zw_s2_t *zw_net_s2(zw_net_t *net);
zw_incl_t *zw_net_incl(zw_net_t *net);
zw_remove_t *zw_net_rm(zw_net_t *net);
zw_iv_t *zw_net_iv(zw_net_t *net);
zw_health_t *zw_net_health(zw_net_t *net);
const zw_node_cache_t *zw_net_nodes(const zw_net_t *net);
zw_net_radio_t zw_net_radio(const zw_net_t *net);
const zw_net_stats_t *zw_net_stats(const zw_net_t *net);

#ifdef __cplusplus
}
#endif

#endif /* ZW_NET_H */
