/*
 * zwave_zones.h - Z-Wave devices as zones: the glue between the panel's Z-Wave network (zw_net,
 * components/core/zwave_host) and its zone source (zsrc_zwave, components/core/zone_hub), so the
 * PC simulator and the firmware's zwave_service (docs/features/36 Z8) share one instead of two.
 *
 * The owner of the zw_net_t passes on what its port hears:
 *
 *   link_up    -> zwz_link_up        the module's facts (the panel's own protocol version)
 *   link_down,
 *   restarted  -> zwz_link_down      the zone source's link goes down (ZSRC-015)
 *   event      -> zwz_event          the network checked (NETWORK), devices added, interviewed,
 *                                    removed or gone, reachable or not, a device's own check-in
 *                                    confirmed (CHECK_IN: its grace follows) (network.md §2-§10)
 *   received   -> zwz_received       a command from a device, at its highest class
 *   send_done  -> zwz_send_done      true for the bridge's own sends (tags ZWZ_TAG_BASE)
 *   quirk      -> zwz_quirk          the quirks table's lifeline, parameters and "no
 *                                    Supervision" (zones.md §8)
 *
 * and calls zwz_poll() after zw_net_poll(), at zwz_next_deadline() too.
 *
 * What it does: builds each device's facts from the node cache (zsrc_zw_desc_t: the interview's
 * inputs verdict per endpoint, notification types and events, sensor types, security, check-in)
 * and tells the zone source when a device is added, interviewed, removed or gone, and its new
 * check-in alone when it confirmed one (zsrc_zw_check_in: its grace); decodes the
 * commands a device sends (Notification, Binary Sensor, Battery, Multilevel Sensor, Alarm Sensor,
 * Basic, Wake Up Notification, Hail) into zsrc_zw_report_t; answers a Supervision Get at once, with
 * the request's class and end point (command-classes.md §4.14: FF understood, 00 not), and the
 * questions command-classes.md §5 says the panel answers (zw_cc_panel_answer); and asks devices for
 * their state when the zone source wants it (zsrc_zw_take_refresh, zones.md §3 "Fresh state"):
 * Notification Get per type, Binary Sensor Get per type, Multilevel Sensor Get for air
 * temperature, Battery Get - at once for a listening device, at its next wake-up for a sleeping
 * one (zw_net's wake-up queue). Its Gets to one device go one at a time (a command to an end
 * point is queued under the kind of its Multi Channel wrapper); its answers are never held in
 * the bridge behind a Get (zw_net_send puts a reply first in its queue).
 *
 * Threads: in the firmware the zwave task owns the zw_net_t and the zone task the hub; call these
 * with the zone service's lock held, as the zone source's state is the hub's too.
 *
 * Portable C11, caller-owned state, no heap.
 */
#ifndef ZWAVE_ZONES_H
#define ZWAVE_ZONES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zone_hub/zsrc_zwave.h"
#include "zwave_host/zw_net.h"
#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bridge's own send tags (top byte FE; zw_net's own are FF): the owner routes send_done for
 * them here. */
#define ZWZ_TAG_BASE  0xFE000000u
#define ZWZ_TAG_MASK  0xFF000000u

/* The outbox (network.md section 7, "the bridge's outbox"): a device has at most one Get in it
 * at a time - zw_net's wake-up queue keys a command by its first bytes, so a sleeper's several
 * Gets (every Notification Get, every Multi Channel one) would replace each other there - and a
 * sleeper's holds its slot until the device wakes. So every device has room for its Get, and the
 * answers the panel owes (Supervision Reports, Time and the other command-classes.md section 5
 * answers) have ZWZ_REPLY_SLOTS of their own that no Get can take; an answer goes at the next
 * poll, never behind a Get held for its device. */
#define ZWZ_REPLY_SLOTS 16u
#define ZWZ_GET_SLOTS ZSRC_ZW_DEV_MAX
#define ZWZ_OUTBOX    (ZWZ_GET_SLOTS + ZWZ_REPLY_SLOTS)
#define ZWZ_CMD_MAX   48u    /* the longest command the bridge sends, encapsulation included */
#define ZWZ_RETRY_MS  100u   /* the security manager was busy: again this much later */
#define ZWZ_UNKNOWN_CC_MAX 4u /* distinct "unknown" CCs remembered per device (below) */

/* A command for a device. */
typedef struct {
    bool used;
    bool out;                /* sent: its end awaited (a Get: one per device at a time) */
    bool refresh;            /* one of a refresh's Gets (not an answer) */
    uint16_t node;
    uint8_t cls;             /* zw_sec_class_t */
    uint8_t len;
    uint32_t tag;
    uint32_t order;          /* first come, first served (answers apart, before Gets) */
    uint8_t cmd[ZWZ_CMD_MAX];
} zwz_out_t;

/* A device whose state is to be asked for: its end points (bit e) and the next Get. */
typedef struct {
    uint16_t node;           /* 0: free */
    uint8_t endpoints;
    uint8_t next;
} zwz_refresh_t;

/* When a device's battery level was last heard (or last asked for): the daily re-read (network.md
 * section 7). Runtime only. */
typedef struct {
    uint16_t node;           /* 0: free */
    uint64_t at;
} zwz_battery_t;
#define ZWZ_BATTERY_REREAD_MS (24u * 3600u * 1000u)

/* zw_health's last proof of life of a device that the zone source has heard of. */
typedef struct {
    uint16_t node;           /* 0: free */
    uint64_t at;
} zwz_heard_t;

/* CCs from a node already counted as "unknown" (command-classes.md §2: logged once per node and
 * CC, so a chatty device cannot flood the log; beyond ZWZ_UNKNOWN_CC_MAX distinct ones, further
 * CCs from that node are counted every time - an unlikely case for any real sensor). */
typedef struct {
    uint16_t node;           /* 0: free */
    uint8_t cc[ZWZ_UNKNOWN_CC_MAX];
    uint8_t count;
} zwz_unknown_t;

typedef struct {
    uint32_t reports;        /* commands decoded for the zone source */
    uint32_t supervision;    /* Supervision Reports queued */
    uint32_t answers;        /* command-classes.md §5 answers queued */
    uint32_t gets;           /* state questions sent (or queued for a sleeping device) */
    uint32_t dropped;        /* commands that could not go (an answer without room, a send
                              * refused); a Get without room is not one: it is asked later */
    uint32_t busy;           /* sends put off: the security manager was busy (ZWZ_RETRY_MS) */
    uint32_t heard;          /* proofs of life without a command passed on (network.md §8) */
    uint32_t unknown;        /* distinct (node, CC) pairs neither decoded nor answered (§2) */
    uint32_t replies_lost;   /* answers owed (Supervision Reports, §5 answers) that found no room
                              * in the outbox: counted, never silent (also in `dropped`) */
} zwz_stats_t;

/* The panel's local time for a device's Time or Date Get (command-classes.md §5, ZW-024). */
typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} zwz_local_time_t;

typedef struct {
    zw_net_t *net;
    zsrc_zw_t *src;
    zw_cc_panel_identity_t identity; /* what the panel says it is (command-classes.md §5) */
    bool answers;            /* answer §5's questions (an identity was given) */
    uint64_t now;
    uint64_t retry_at;       /* a device's command waits for the security manager (0: none) */
    uint64_t get_retry_at;   /* a Get found the outbox full: Gets wait until then (0: none);
                              * answers do not */
    uint32_t seq;            /* stamps tags and the outbox's order */
    zwz_out_t out[ZWZ_OUTBOX];
    zwz_refresh_t refresh[ZSRC_ZW_DEV_MAX];
    zwz_battery_t battery[ZSRC_ZW_DEV_MAX];
    zwz_heard_t heard[ZSRC_ZW_DEV_MAX];
    zwz_unknown_t unknown[ZSRC_ZW_DEV_MAX];
    bool rebase;             /* zw_health's clocks were set anew (a network check): as they are */
    zwz_stats_t stats;
    /* A command it does not decode, offered to the owner before the panel's own answers
     * (zwz_set_claim): true, the owner took it (copied it to run later; its Supervision is
     * answered SUCCESS). NULL: none. */
    bool (*claim)(void *ctx, const zw_s2_rx_t *rx);
    void *claim_ctx;
    /* The local time a Time or Date Get is answered with (zwz_set_clock): false, or NULL, when the
     * panel's clock isn't set - the answer then says "clock not set". */
    bool (*clock)(void *ctx, zwz_local_time_t *out);
    void *clock_ctx;
} zwz_t;

/* `identity` (copied; NULL: §5's questions go unanswered, Supervision Gets are still answered)
 * says what the panel answers Version, Manufacturer Specific and Z-Wave Plus Info Gets with; its
 * protocol version comes from the module at each link up. */
void zwz_init(zwz_t *b, zw_net_t *net, zsrc_zw_t *src, const zw_cc_panel_identity_t *identity);
/* The owner's claim on commands the bridge doesn't decode (an assigned keypad's Entry Control,
 * zw_keyctl.h). It runs under the owner's lock: copy, never wait. */
void zwz_set_claim(zwz_t *b, bool (*claim)(void *ctx, const zw_s2_rx_t *rx), void *ctx);
/* The owner's local time, read when a device asks for it (a keypad or a lock setting its own
 * clock); NULL: always "clock not set". */
void zwz_set_clock(zwz_t *b, bool (*clock)(void *ctx, zwz_local_time_t *out), void *ctx);

void zwz_link_up(zwz_t *b, const zw_module_facts_t *facts, uint64_t now);
void zwz_link_down(zwz_t *b, uint64_t now);
void zwz_event(zwz_t *b, const zw_net_event_t *ev, uint64_t now);
void zwz_received(zwz_t *b, const zw_s2_rx_t *rx, uint64_t now);
/* True when the tag is the bridge's (the owner hears nothing of it). */
bool zwz_send_done(zwz_t *b, uint32_t tag, zw_s2_result_t result);
bool zwz_quirk(const zwz_t *b, const zw_node_record_t *rec, zw_iv_quirk_t *out);

void zwz_poll(zwz_t *b, uint64_t now);
uint64_t zwz_next_deadline(const zwz_t *b);

bool zwz_is_tag(uint32_t tag);
/* A node record's facts as the zone source gets them. */
void zwz_describe(const zw_node_record_t *rec, zsrc_zw_desc_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZWAVE_ZONES_H */
