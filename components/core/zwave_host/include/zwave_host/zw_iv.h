/*
 * zw_iv.h - the interview (docs/features/36 network.md §5, the lifeline §6, push or pull;
 * requirement ZW-010): what the panel asks a device after it joined, step by step, each step
 * recorded in the node cache (field 0E) the moment it completes, so that a restart of the panel
 * or of the module, or the device falling asleep, continues where it stopped. A finished device
 * read again (zw_iv_restart, BL-152) is the exception: its steps fill a scratch record, and only
 * READY changes - and stores - its own.
 *
 * Every question waits for its answer 1 s plus the round trip after its send ended (serial-api.md
 * 6.12; the round trip is measured from the question leaving to its send's end), is tried 3
 * times, and the whole interview 5 times (a failed attempt continues from the step that failed;
 * a sleeping device's next attempt waits for its next wake-up); a device that never answers stays
 * "interviewing" and the owner hears where it stopped. The owner delivers a sleeping device's
 * questions when it is awake (its wake-up queue, network.md §7: zw_net with zw_health): a
 * question waiting there costs no try, and the owner hears "Wake the device to finish" once per
 * sleep (zw_iv_asleep); zw_iv_went says when it finally left.
 *
 * The lifeline (§6): group 1 of a Z-Wave Plus device; else Association Group Information group 1
 * with the Lifeline profile; else the quirks table's group; else none - the device then offers no
 * zone input ("Does not report to the panel"). Multi Channel Association with endpoint 0 when the
 * device has endpoints none of which associates by itself and the group is within its Multi
 * Channel Association groups, else plain Association; then read back - a Report that comes in
 * parts (reports to follow) is awaited part by part, each part restarting the wait (no try spent),
 * and the first part naming the panel settles it. Push or pull (§5, per endpoint with
 * Notification): no Association CC is pull; the lifeline's AGI command list naming Notification
 * Report is push; else the first type is enabled and read: FF push, 00/FE pull, no answer push.
 * Pull devices offer no zone input either (zw_iv_inputs()). A sensor CC only end points list
 * (Notification, Binary Sensor, Multilevel Sensor) has its version asked of the root at step 8 -
 * the root answers for its end points - and kept in the record without the "supported" flags.
 *
 * Security for the questions: §8's class per command, decided by the CC inside any Multi Channel
 * or Supervision layer (zw_s2_class_for); Security 2 Commands Supported Get at the node's highest
 * class. A node whose classes are not known (found in the module at start, network.md §2, or the
 * panel restarted during its key exchange) is asked only a class learnt by decrypting, or else
 * probed there class by class - network.md §5 step 4 has the order and what each answer, silence
 * or transmit failure records. SET
 * commands to a node that lists Supervision go wrapped in a Supervision Get (command-classes.md
 * §4.14) unless its quirks row says no_supervision (zw_iv_supervised), and its Report is the
 * result: FF done, 00 "not supported" done, 02 a failed try. A Set without Supervision is done when
 * it was delivered - to a node that lists Application Status, once 1 s plus the round trip passed
 * without one. Application Status answering a Set (command-classes.md §4.25): Rejected Request is
 * a failed try at once; Busy a failed try whose next one waits the node's wait time (1 s when it
 * names none, at most ZW_IV_BUSY_MAX_S). Neither is ever taken for silence or for "done".
 *
 * While the owner holds the interview (a device being added, network.md §4: "the panel asks
 * nothing of other nodes while it runs"), no question goes; one already out still gets its answer
 * or its timeout, one held for a sleeping device's wake-up is taken back (the owner removes it from
 * the device's queue), and the questions go on when it lets go.
 *
 * Decisions the documents leave open: Version Get is asked and its report not kept (the node
 * cache has no field for a device's firmware version); a CC a node reports at version 0 stays in
 * the cache with neither flag (not supported); the quirks step (15) and the lifeline's quirk group
 * come from the owner's quirks table (zones.md §8, Z6) through the port, and so does the wake-up
 * interval step 7 sets (network.md §7's choice); a device at interval 0 is left alone, and one
 * whose wake-ups go elsewhere gets the panel as its destination at its own interval.
 *
 * It sends through its port (node questions to the owner, who encrypts or queues them; module
 * questions to the host) and hears through zw_iv_received() (every command from a node, after the
 * security manager), zw_iv_node_info() (ApplicationUpdate 84/81), zw_iv_sent() and its module
 * requests' own ends. At most ZW_IV_ACTIVE questions of listening devices are out at once; a
 * sleeping device's go whenever it is awake, and so does the question for a device's protocol
 * info (the module answers it; a device that just joined may be a sleeper).
 *
 * Portable C11, caller-owned state (zw_net holds it), no heap. Callbacks may call the zw_iv
 * functions, except that `changed` and `event` must not start or stop interviews.
 */
#ifndef ZW_IV_H
#define ZW_IV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_nodes.h"
#include "zwave_host/zw_s2.h"
#include "zwave_host/zw_transport.h"
#include "zwave_proto/zw_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* network.md §5 timing and counts */
#define ZW_IV_REPORT_MS  1000u /* an answer is awaited this long after its send ended, plus
                                * the round trip */
#define ZW_IV_TRIES      3u    /* each question */
#define ZW_IV_ATTEMPTS   5u    /* the whole interview */
#define ZW_IV_ACTIVE     2u    /* listening devices' questions out at once */
#define ZW_IV_RETRY_MS   100u  /* the radio's queue was full: this much later */
#define ZW_IV_BUSY_MS    1000u /* Application Busy naming no wait time: the next try this later */
#define ZW_IV_BUSY_MAX_S 30u   /* ...and at most this long, whatever wait time it names */
#define ZW_IV_SLOTS      ZW_NODES_MAX
#define ZW_IV_CMD_MAX    16u   /* the longest question, before its encapsulation */
#define ZW_IV_STOPPED_MAX 8u  /* re-reads that stopped, remembered (RAM only, BL-152) */
#define ZW_IV_REREADS    2u    /* re-reads that read at once, each into a scratch record (BL-152);
                                * a listening device's beyond them is refused, a sleeper's waits */

/* The steps of network.md §5's table, in order; the node cache's field 0E holds the last one
 * done (ZW_IV_STEP_DONE: ready). */
typedef enum {
    ZW_IV_STEP_PROTOCOL = 1, /* GetNodeProtocolInfo */
    ZW_IV_STEP_PING,         /* No Operation, listening devices */
    ZW_IV_STEP_NODE_INFO,    /* from the inclusion, else RequestNodeInfo */
    ZW_IV_STEP_SECURE_CCS,   /* Security 2 Commands Supported (the probe: classes unknown) */
    ZW_IV_STEP_IDENTITY,     /* Manufacturer Specific */
    ZW_IV_STEP_VERSIONS,     /* Version, Version CC per used CC */
    ZW_IV_STEP_WAKE_UP,      /* capabilities, interval, Interval Set */
    ZW_IV_STEP_ENDPOINTS,    /* Multi Channel end points and capabilities */
    ZW_IV_STEP_ZWAVE_PLUS,   /* Z-Wave Plus Info */
    ZW_IV_STEP_LIFELINE,     /* §6 */
    ZW_IV_STEP_NOTIFICATION, /* types, events, push or pull, state, Notification Set */
    ZW_IV_STEP_BINARY,       /* Binary Sensor */
    ZW_IV_STEP_MULTILEVEL,   /* Multilevel Sensor: air temperature */
    ZW_IV_STEP_BATTERY,
    ZW_IV_STEP_QUIRKS,       /* Configuration Set per the quirks table */
    ZW_IV_STEP_DONE          /* ready */
} zw_iv_step_t;

/* What a device offers the zone source (zones.md §2), or why nothing. */
typedef enum {
    ZW_IV_INPUTS_OFFERED = 0,
    ZW_IV_INPUTS_NOT_READY,   /* still interviewing */
    ZW_IV_INPUTS_NO_LIFELINE, /* §6: "Does not report to the panel" */
    ZW_IV_INPUTS_PULL,        /* §5: pull mode - the panel would have to poll it */
    ZW_IV_INPUTS_NOT_USABLE   /* another controller (network.md §10) */
} zw_iv_inputs_t;

typedef enum {
    ZW_IV_EV_PROTOCOL = 1, /* step 1 done: listening, FLiRS or sleeping is known */
    ZW_IV_EV_READY,        /* step 16 done: the zone source builds its inputs (zw_iv_inputs) */
    ZW_IV_EV_WAKE_NEEDED,  /* a question waits for the device: "Wake the device to finish" */
    ZW_IV_EV_STALLED,      /* 5 attempts failed at `step`: it stays interviewing */
    ZW_IV_EV_NOT_USABLE,   /* a controller: listed with "Remove it", it feeds no zone */
    ZW_IV_EV_ENDED,        /* the interview ended (READY, STALLED, NOT_USABLE or stopped) */
    ZW_IV_EV_RESTARTED     /* BL-152: a re-read whose question was held for the device's
                            * wake-up starts again from step 1 (a second request, or its scratch
                            * record taken back): that question, in the device's queue, is void -
                            * the owner takes it out */
} zw_iv_event_kind_t;

typedef struct {
    zw_iv_event_kind_t kind;
    uint16_t node;
    uint8_t step; /* the step it is at (READY: ZW_IV_STEP_DONE) */
} zw_iv_event_t;

/* What the interview needs from the quirks table (zones.md §8), for one model. */
#define ZW_IV_QUIRK_CONFIG_MAX 4u
typedef struct {
    uint8_t lifeline_group; /* §6: its reports' group when the device names none; 0: none */
    uint8_t config_count;
    struct {
        uint8_t parameter;
        uint8_t size;  /* 1, 2 or 4 */
        int32_t value;
    } config[ZW_IV_QUIRK_CONFIG_MAX]; /* step 15: Configuration Set, supervised */
    bool no_supervision;    /* its firmware answers a supervised command "fail": its Sets go
                             * without Supervision (zones.md §8 `no_supervision`) */
} zw_iv_quirk_t;

/* The wake-up interval "leave it as it is". */
#define ZW_IV_WAKE_KEEP 0xFFFFFFFFu

typedef struct {
    void *ctx;
    /* A question to a node at a class; its end comes back through zw_iv_sent() with `tag`. The
     * owner may hold it until the device is awake: *held true (zw_iv_went when it leaves,
     * zw_iv_asleep when it went unheard and waits again). */
    zw_s2_result_t (*send)(void *ctx, uint16_t node, zw_sec_class_t cls, const uint8_t *cmd,
                           size_t len, uint32_t tag, bool *held);
    /* RequestNodeInfo for a node: the module's response through zw_iv_sent() with `tag` (OK: it
     * went), its answer through zw_iv_node_info() / zw_iv_node_info_failed(); held as above.
     * False: not now. */
    bool (*node_info)(void *ctx, uint16_t node, uint32_t tag, bool *held);
    /* A module command (GetNodeProtocolInfo) for the host. */
    zw_submit_t (*submit)(void *ctx, const zw_request_t *req);
    /* The node cache changed in a way to keep (a step completed): store it. */
    void (*changed)(void *ctx);
    void (*event)(void *ctx, const zw_iv_event_t *ev);
    /* Optional: the quirks table's row for this model (identity known); false: none. */
    bool (*quirk)(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out);
    /* Optional: the wake-up interval to set, seconds, within the device's limits (network.md §7);
     * ZW_IV_WAKE_KEEP (or NULL) keeps the device's own. */
    uint32_t (*wake_interval)(void *ctx, const zw_node_record_t *rec);
} zw_iv_port_t;

/* One device being interviewed (RAM only; what it learned is in its node cache record). */
typedef struct {
    uint16_t node;          /* 0: a free slot */
    uint8_t state;          /* zw_iv.c */
    uint8_t step;           /* the step running: the record's field 0E + 1 */
    uint8_t ep, item, phase; /* the step's cursor */
    uint8_t tries;          /* of the question out */
    uint8_t attempts;       /* failed attempts of the whole interview */
    uint8_t seq;            /* stamps its questions: an older one's end is ignored */
    uint8_t session;        /* its Supervision session IDs, 1..63 */
    uint8_t ep_count;       /* step 8: Multi Channel end points it reports (at most 3 kept) */
    uint8_t mca_groups;     /* step 10: Multi Channel Association's groups; 0 not asked */
    uint8_t agi;            /* step 11: 0 not asked, 1 its lifeline sends Notification
                             * Reports, 2 not */
    uint8_t wait;           /* what the question waits for (zw_iv.c) */
    bool optional;          /* no answer after the tries is itself an answer (probes) */
    uint8_t max_tries;      /* the question's tries (0: ZW_IV_TRIES) */
    bool nok_aborts;        /* its last try not delivered (ZW_S2_FAILED) fails the attempt, even
                             * when optional (Security 2's queries: Z-Wave JS's
                             * Controller_CallbackNOK is a transmission error) */
    bool tx_failed;         /* the last try's send ended ZW_S2_FAILED */
    bool secure_taken;      /* step 4 took a non-empty secure CC list this attempt (Z-Wave JS
                             * hasReceivedSecureCommands) */
    bool parked;            /* the owner holds it until the device wakes */
    bool told_asleep;       /* WAKE_NEEDED said for this sleep */
    bool spv_seen;          /* it sent the panel a Supervision Get, or answered one: it uses
                             * Supervision, whatever its Version CC Report says (step 6) */
    bool alarm_only;        /* BL-130: a finished interview that lacks step 12's Alarm Sensor
                             * types asks only them; field 0E is left as it is */
    bool delivered;         /* a try of the question out reached the device (its delivery
                             * confirmed, or an answer): only then is an optional question's
                             * silence an answer (BL-130) */
    bool alarm_stored;      /* this run stored an end point's Alarm Sensor types, or "none" */
    bool refresh;           /* BL-152: a finished device read again from step 1 (Z-Wave JS's
                             * refreshInfo): its answers go into a scratch record, and its own
                             * record - its inputs, zones and keypad - keeps the old facts until
                             * READY replaces them; RAM only, a panel restart drops it */
    uint8_t scratch;        /* BL-152: the re-read's record, 1-based in zw_iv_t.reread; 0 none
                             * (a first interview writes the node's own record; a sleeper's
                             * re-read takes one only at its wake-up) */
    uint8_t cls;           /* zw_sec_class_t of the question */
    uint8_t cmd_ep;         /* its endpoint */
    uint8_t cmd_len;
    uint8_t spv_session;    /* its Supervision Get's session; 0: not supervised */
    uint8_t cmd[ZW_IV_CMD_MAX]; /* the question, before its encapsulation (for matching) */
    uint64_t sent_at;
    uint64_t deadline;      /* the wait's end (READY: not before) */
} zw_iv_node_t;

typedef struct {
    uint32_t started;       /* interviews begun (or continued) */
    uint32_t ready;
    uint32_t stalled;
    uint32_t questions;     /* questions sent, resends included */
    uint32_t timeouts;      /* answers that did not come */
    uint32_t parked;        /* questions held for a sleeping device's wake-up */
} zw_iv_stats_t;

typedef struct {
    zw_iv_port_t port;
    zw_node_cache_t *nodes; /* the owner's node cache */
    zw_s2_t *s2;
    uint16_t own_node;      /* the panel's node ID: the lifeline's and wake-up's destination */
    zw_nid_width_t nid;
    bool link_up;
    bool hold;              /* the owner holds every question (zw_iv_hold) */
    uint64_t now;
    zw_iv_node_t slots[ZW_IV_SLOTS];
    zw_iv_stats_t stats;
    uint16_t stopped[ZW_IV_STOPPED_MAX]; /* nodes whose re-read stopped (BL-152; RAM only, the
                                          * oldest forgotten first): they work on their old facts */
    zw_node_record_t reread[ZW_IV_REREADS]; /* BL-152: the re-reads' scratch records; one is
                                             * free when no slot names it */
} zw_iv_t;

/* ---- Functions ---------------------------------------------------------------------------- */

/* `nodes` and `s2` are the owner's (zw_net's): the records it fills, the classes it probes. */
void zw_iv_init(zw_iv_t *iv, const zw_iv_port_t *port, zw_node_cache_t *nodes, zw_s2_t *s2);

/* The link: up (the panel's node ID and the node-ID width in effect), down (every interview
 * pauses; what was out is asked again after the next link up). */
void zw_iv_link_up(zw_iv_t *iv, uint64_t now, uint16_t own_node, zw_nid_width_t nid);
void zw_iv_link_down(zw_iv_t *iv, uint64_t now);
/* No question goes while held (a device being added, network.md §4); what is out already ends as
 * it would, a question held for a sleeping device's wake-up is taken back (the caller removes it
 * from the device's queue first), the rest waits until the hold is let go. */
void zw_iv_hold(zw_iv_t *iv, bool hold);

/* Starts, or continues from its record's field 0E, the interview of a node in the cache. A
 * finished one whose record lacks Alarm Sensor types (zw_iv_alarm_types_missing) asks only those
 * (step 12's Alarm Sensor question, BL-130): field 0E stays done, its inputs stay offered, and
 * READY follows - unless no try reached the device: then it ends with nothing written and no
 * READY (ENDED only), and the next start asks again. False when it is not in the cache or has
 * nothing left to ask. */
bool zw_iv_start(zw_iv_t *iv, uint64_t now, uint16_t node);
/* From step 1 again (the owner's "interview again", a software update). One not finished has its
 * record's field 0E cleared. A finished one is re-read (BL-152, Z-Wave JS's refreshInfo): its
 * answers go into a scratch record (ZW_IV_REREADS of them), its own record is neither changed nor
 * stored - its inputs, zones and keypad keep its old facts - and READY replaces the old facts with
 * the new reading at once, keeping what the reading does not ask (its security classes, DSK,
 * generation, raw items this build cannot hold) and storing it once. A sleeper that supports Wake
 * Up has its re-read wait for its wake-up (zw_iv_awake) and take a scratch record then (none
 * free: one a parked sleeper holds, as below; else at the next wake-up); any other device starts
 * now. With none free, a sleeper's re-read whose question waits for the device's wake-up gives its
 * record back (ZW_IV_EV_RESTARTED: it starts again at that wake-up). One that stops leaves the old
 * facts (zw_iv_reread_stopped). False: not in the cache, or a re-read that would start now while
 * every scratch record is in use (busy). As Z-Wave JS's refreshInfo: a re-read waiting for its
 * wake-up is left alone (true), and one under way starts again from step 1, keeping its scratch
 * record - with ZW_IV_EV_RESTARTED only when its question was held for the device's wake-up (the
 * owner takes it out of the queue). */
bool zw_iv_restart(zw_iv_t *iv, uint64_t now, uint16_t node);
/* zw_iv_restart without waiting for a sleeper's wake-up (Z-Wave JS's refreshInfo({ waitForWakeup:
 * false }), what it does after a software update): the re-read takes a scratch record now (or
 * is busy) and its questions wait in the device's wake-up queue while it sleeps. */
bool zw_iv_restart_now(zw_iv_t *iv, uint64_t now, uint16_t node);
/* Ends it without a word (the node was removed). */
void zw_iv_stop(zw_iv_t *iv, uint16_t node);
bool zw_iv_running(const zw_iv_t *iv, uint16_t node);
/* Running, and not a re-read that waits for its device's wake-up with no scratch record: what
 * keeps the device awake (No More Information held, BL-152). */
bool zw_iv_asking(const zw_iv_t *iv, uint16_t node);
/* A Wake Up Interval Report from the node (zw_net keeps it in the node's record): a re-read under
 * way keeps it in its scratch record too, so READY does not put back an older interval (BL-152);
 * its own Set, later, still wins. */
void zw_iv_wake_interval_heard(zw_iv_t *iv, uint16_t node, uint32_t interval_s, uint8_t notify);
/* Running, and not only the Alarm Sensor catch-up of a finished interview (BL-130): what may not
 * overlap the interview itself (a check-in Set of its own; its step 9 sets one) waits for it. */
bool zw_iv_setting_up(const zw_iv_t *iv, uint16_t node);
/* The step it is at; 0 when not running. */
uint8_t zw_iv_step(const zw_iv_t *iv, uint16_t node);
/* A finished device is being read again (zw_iv_restart, BL-152). */
bool zw_iv_refreshing(const zw_iv_t *iv, uint16_t node);
/* Its last re-read stopped (its attempts ran out) and none has run since: it works on its old
 * facts. RAM only; a new reading, its removal or READY forgets it. */
bool zw_iv_reread_stopped(const zw_iv_t *iv, uint16_t node);

/* Every command from a node, after the security manager: perhaps the answer awaited. */
void zw_iv_received(zw_iv_t *iv, uint64_t now, const zw_s2_rx_t *rx);
/* ApplicationUpdate 84: a node's information (the answer to RequestNodeInfo). */
void zw_iv_node_info(zw_iv_t *iv, uint64_t now, uint16_t node, const uint8_t *ccs, size_t len);
/* ApplicationUpdate 81: the node did not answer its RequestNodeInfo (a failed try). */
void zw_iv_node_info_failed(zw_iv_t *iv, uint64_t now, uint16_t node);
/* The end of a question (port.send's or port.node_info's tag): ZW_S2_OK delivered (for
 * RequestNodeInfo: the module sent it). False: not the interview's tag, or not the one out. */
bool zw_iv_sent(zw_iv_t *iv, uint64_t now, uint32_t tag, zw_s2_result_t result);
/* The owner holds the node's question until it wakes (it did not hear it, or is asleep). */
void zw_iv_asleep(zw_iv_t *iv, uint64_t now, uint16_t node);
/* The node's held question left now. */
void zw_iv_went(zw_iv_t *iv, uint64_t now, uint32_t tag);
/* The node woke (zw_net: its Wake Up Notification, NIF or Get, or a frame while it counts as
 * awake) while its next attempt waits for a wake-up: go on. */
void zw_iv_awake(zw_iv_t *iv, uint64_t now, uint16_t node);
/* Whether a tag is the interview's. */
bool zw_iv_is_tag(uint32_t tag);

void zw_iv_poll(zw_iv_t *iv, uint64_t now);
uint64_t zw_iv_next_deadline(const zw_iv_t *iv);
const zw_iv_stats_t *zw_iv_stats(const zw_iv_t *iv);

/* What a node's record offers the zone source at an endpoint (0: the device itself). */
zw_iv_inputs_t zw_iv_inputs(const zw_node_record_t *rec, uint8_t endpoint);
/* A finished interview (field 0E done) of a device, not a controller, with an end point that lists
 * Alarm Sensor but has no field 12: written before the interview asked Alarm Sensor types
 * (BL-085), or no try of the question reached the device. Once asked, field 12 is there - empty
 * when the device heard it and gave no usable answer. */
bool zw_iv_alarm_types_missing(const zw_node_record_t *rec);

/* Whether a record lists a CC for the device itself (in its NIF or its secure list, not reported
 * at version 0), or for one of its Multi Channel end points (0: the device itself). */
bool zw_iv_has_cc(const zw_node_record_t *rec, uint8_t cc);
bool zw_iv_endpoint_has_cc(const zw_node_record_t *rec, uint8_t endpoint, uint8_t cc);
/* A sleeping device (neither listening nor FLiRS; protocol info known). */
bool zw_iv_sleeping(const zw_node_record_t *rec);
/* Whether a Set to the node goes wrapped in Supervision (command-classes.md §4.14): it lists
 * Supervision and its quirks row (the port's) does not say no_supervision. The interview's Sets
 * and zw_net's own (a device's check-in) follow this one rule. */
bool zw_iv_supervised(const zw_iv_t *iv, const zw_node_record_t *rec);
/* Adds a CC to a record's list (supported: in its NIF; secure: in its Security 2 Commands
 * Supported Report), or those flags to the entry already there. */
void zw_iv_add_cc(zw_node_record_t *rec, uint8_t cc, bool supported, bool secure);

#ifdef __cplusplus
}
#endif

#endif /* ZW_IV_H */
