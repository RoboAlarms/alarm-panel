/*
 * zw_remove.h - removing devices (docs/features/36 network.md §10; requirement ZW-016):
 *
 * - "Remove a device" (exclusion) for any device, also one from another network:
 *   RemoveNodeFromNetwork(any) and its status reports - Ready within 10 s, a 60 s window ("Keep
 *   listening" restarts it), NodeFound, RemovingEndNode with the node ID within 60 s, Done within
 *   60 s, then RemoveNodeFromNetwork(stop). Node ID 0: "Removed a device that belonged to another
 *   system: it can be added now". Status 07 (and the reserved 05, "stopped") fail it.
 * - "Remove a failed device" (phase 2 in the documents, needed here for Device Reset Locally):
 *   No Operation up to 3 times 2 s apart - any answer refuses it ("It still answers: use Remove a
 *   device"); then IsFailedNode, and only a node the module lists as failed goes to
 *   RemoveFailedNode (result 01: removed). The module's own list is the only thing removed from:
 *   the panel forgets the node only after that 01 (no ghost left in the module), and a refusal
 *   (REFUSED), a missing end (TIMEOUT) or an answer (STILL_ANSWERS) each say so.
 * - Device Reset Locally (a device's 5A 01: it left by itself): the same, pinging round after
 *   round (at most ZW_REMOVE_DRL_ROUNDS) until the module lists it as failed.
 * - "Replace a failed device"'s check (zw_remove_check_failed): the same pings and IsFailedNode,
 *   without RemoveFailedNode - the module accepts ReplaceFailedNode only for a node it lists as
 *   failed, and lists one only after a SendData to it got no answer (a sleeper the panel sent
 *   nothing to is not listed). Its end goes to the caller's own callback, not the port's event.
 *
 * What a removal means for the node cache, the security manager and the zones is the owner's
 * (zw_net). ZW-016's "installer mode, every partition disarmed" is the caller's check, passed in
 * as `allowed`, as for adding (zw_incl.h). One removal at a time.
 *
 * Portable C11, caller-owned state, no heap. Callbacks may call the zw_remove functions.
 */
#ifndef ZW_REMOVE_H
#define ZW_REMOVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_transport.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* network.md §10 timing */
#define ZW_REMOVE_READY_MS    10000u /* RemoveNode(any): status Ready */
#define ZW_REMOVE_WINDOW_MS   60000u /* the window, restarted by "Keep listening" */
#define ZW_REMOVE_FOUND_MS    60000u /* NodeFound: RemovingEndNode */
#define ZW_REMOVE_DONE_MS     60000u /* RemovingEndNode: Done */
#define ZW_REMOVE_PINGS       3u     /* remove failed: pings... */
#define ZW_REMOVE_PING_GAP_MS 2000u  /* ...this far apart */
#define ZW_REMOVE_DRL_ROUNDS  3u     /* Device Reset Locally: rounds of pings until listed failed */
#define ZW_REMOVE_FAILED_MS   30000u /* RemoveFailedNode's callback (not documented; decided) */

typedef enum {
    ZW_REMOVE_IDLE = 0,
    ZW_REMOVE_STARTING,   /* RemoveNode(any) sent: Ready awaited */
    ZW_REMOVE_LISTENING,  /* the window: "Put the device in pairing mode" */
    ZW_REMOVE_FOUND,      /* NodeFound: RemovingEndNode awaited */
    ZW_REMOVE_REMOVING,   /* RemovingEndNode: Done awaited */
    ZW_REMOVE_PINGING,    /* remove failed: its pings */
    ZW_REMOVE_CHECKING,   /* IsFailedNode */
    ZW_REMOVE_DELETING    /* RemoveFailedNode */
} zw_remove_state_t;

typedef enum {
    ZW_REMOVE_DONE = 0,       /* it left the network: `node` (0: it belonged to another system) */
    ZW_REMOVE_NOT_ALLOWED,    /* refused: not in installer mode with every partition disarmed */
    ZW_REMOVE_BUSY,           /* refused: a network operation is running */
    ZW_REMOVE_NOT_LISTENING,  /* "The Z-Wave radio did not start listening" */
    ZW_REMOVE_NO_DEVICE,      /* the window ended, nothing found */
    ZW_REMOVE_STOPPED,        /* the installer stopped it */
    ZW_REMOVE_FAILED,         /* status Failed (or "stopped"), or the module did not answer */
    ZW_REMOVE_TIMEOUT,        /* RemovingEndNode, Done or RemoveFailedNode's end not in time */
    ZW_REMOVE_LINK_DOWN,      /* the module went away */
    ZW_REMOVE_STILL_ANSWERS,  /* "It still answers: use Remove a device" */
    ZW_REMOVE_NOT_FAILED,     /* the module does not list it as failed */
    ZW_REMOVE_UNKNOWN_NODE,   /* (zw_net) not a device of this network */
    ZW_REMOVE_STARTED,        /* started: events follow */
    ZW_REMOVE_REFUSED,        /* RemoveFailedNode refused, or it answered "not removed" (02) */
    ZW_REMOVE_LISTED,         /* the check only (zw_remove_check_failed): no ping answered and the
                               * module lists it as failed - it can be replaced */
    ZW_REMOVE_RADIO_RESTARTED /* the module restarted (by itself, or serial-api.md §7's soft reset)
                               * during "Remove a device": its window closed with it
                               * (zw_remove_radio_restarted) */
} zw_remove_result_t;

typedef enum {
    ZW_REMOVE_EV_LISTENING = 1, /* the window is open until `deadline` */
    ZW_REMOVE_EV_FOUND,         /* a device answered: the countdown stops */
    ZW_REMOVE_EV_DONE           /* the end: `result`, `node` */
} zw_remove_event_kind_t;

typedef enum {
    ZW_REMOVE_BY_EXCLUSION = 0, /* "Remove a device" */
    ZW_REMOVE_BY_FAILED,        /* "Remove a failed device" */
    ZW_REMOVE_BY_RESET_LOCALLY, /* the device said it left (5A 01) */
    ZW_REMOVE_BY_REPLACE        /* "Replace a failed device"'s check: nothing removed */
} zw_remove_kind_t;

typedef struct {
    zw_remove_event_kind_t kind;
    zw_remove_kind_t how;
    uint16_t node;
    uint64_t deadline;
    zw_remove_result_t result; /* DONE */
} zw_remove_event_t;

typedef struct {
    void *ctx;
    /* A module command (RemoveNode, IsFailedNode, RemoveFailedNode) for the host. */
    zw_submit_t (*submit)(void *ctx, const zw_request_t *req);
    /* A No Operation to the node; its end through zw_remove_ping_done() with `tag`. False: it
     * could not go. */
    bool (*ping)(void *ctx, uint16_t node, uint32_t tag);
    void (*event)(void *ctx, const zw_remove_event_t *ev);
} zw_remove_port_t;

/* The end of a replacement's check (zw_remove_check_failed): LISTED, STILL_ANSWERS, NOT_FAILED,
 * FAILED (the module did not answer IsFailedNode) or LINK_DOWN. */
typedef void (*zw_remove_checked_t)(void *ctx, uint16_t node, zw_remove_result_t result);

typedef struct {
    zw_remove_port_t port;
    zw_remove_state_t state;
    zw_remove_kind_t how;
    zw_nid_width_t nid;
    uint64_t now;
    uint64_t deadline;        /* the current wait's end; UINT64_MAX: none */
    uint16_t gen;             /* this removal: requests of an older one are ignored */
    uint8_t cb;               /* RemoveNode(any)'s callback ID: its status reports carry it */
    uint16_t node;
    uint8_t pings;            /* pings failed in this round */
    uint8_t rounds;           /* rounds of pings */
    bool ping_out;            /* a ping's end awaited */
    zw_remove_checked_t checked; /* BY_REPLACE: its end goes here (once) */
    void *checked_ctx;
} zw_remove_t;

void zw_remove_init(zw_remove_t *rm, const zw_remove_port_t *port);

/* "Remove a device": ZW_REMOVE_STARTED or the refusal (nothing sent). */
zw_remove_result_t zw_remove_start(zw_remove_t *rm, uint64_t now, bool allowed,
                                   zw_nid_width_t nid);
/* "Keep listening": the 60 s again (while listening). */
void zw_remove_keep_listening(zw_remove_t *rm, uint64_t now);
/* The installer stops listening (before a device is found). */
void zw_remove_stop(zw_remove_t *rm, uint64_t now);

/* "Remove a failed device" / Device Reset Locally for a node of the network. */
zw_remove_result_t zw_remove_failed(zw_remove_t *rm, uint64_t now, bool allowed, uint16_t node,
                                    zw_nid_width_t nid);
zw_remove_result_t zw_remove_reset_locally(zw_remove_t *rm, uint64_t now, uint16_t node,
                                           zw_nid_width_t nid);
/* "Replace a failed device"'s check (network.md §10): pings and IsFailedNode as for
 * zw_remove_failed - any answer ends it STILL_ANSWERS, a node the module doesn't list NOT_FAILED -
 * and a node it lists ends it LISTED, nothing removed. The end goes to `done` only (no event).
 * ZW_REMOVE_STARTED, or BUSY / UNKNOWN_NODE (nothing sent). */
zw_remove_result_t zw_remove_check_failed(zw_remove_t *rm, uint64_t now, uint16_t node,
                                          zw_nid_width_t nid, zw_remove_checked_t done, void *ctx);
/* Ends a running check at once, without calling its `done` (the replacement ended first: stopped,
 * the radio gone). Nothing else is touched. */
void zw_remove_cancel_check(zw_remove_t *rm, uint64_t now);

/* Inputs: RemoveNode status reports the transport did not claim, a ping's end, the link. */
void zw_remove_module_frame(zw_remove_t *rm, uint64_t now, const zw_frame_t *frame);
void zw_remove_ping_done(zw_remove_t *rm, uint64_t now, uint32_t tag, bool delivered);
void zw_remove_link_down(zw_remove_t *rm, uint64_t now);
/* The module restarted - by itself or by serial-api.md §7's soft reset (network.md §10): "Remove
 * a device" before Done (starting, listening, found, removing) ends at once,
 * ZW_REMOVE_RADIO_RESTARTED, a RemoveNode(stop) queued for after the restart; whether a device in
 * `removing` left is the module's node list's to say, read again at the link up that follows
 * (serial-api.md §2). A failed device's
 * removal and a replacement's check carry on (their commands have their own ends). */
void zw_remove_radio_restarted(zw_remove_t *rm, uint64_t now);

void zw_remove_poll(zw_remove_t *rm, uint64_t now);
uint64_t zw_remove_next_deadline(const zw_remove_t *rm);
zw_remove_state_t zw_remove_state(const zw_remove_t *rm);
/* Whether a tag is a removal's ping. */
bool zw_remove_is_tag(uint32_t tag);

#ifdef __cplusplus
}
#endif

#endif /* ZW_REMOVE_H */
