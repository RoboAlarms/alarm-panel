/*
 * zw_routes.h - rebuilding routes (docs/features/36 network.md §11, ZW-024): Z-Wave routes around
 * obstacles through mains-powered devices, and routes go stale when devices move. For each
 * device, one at a time:
 *   1. RequestNodeNeighborUpdate (up to ZW_ROUTES_TRIES tries; "started" then done or failed);
 *   2. AssignSUCReturnRoute (how it reaches the panel; a failure is noted, not fatal);
 *   3. DeleteReturnRoute (stale priority routes cleared; up to ZW_ROUTES_TRIES tries).
 * A device known dead is pinged first and skipped when it doesn't answer. Long Range devices do
 * not route and are skipped. Listening devices go first, in order; a sleeping one waits for its
 * wake-up (the owner calls zw_routes_woke() and keeps it awake while zw_routes_holding() names
 * it) - the job ends when every device is done, or when the installer stops it.
 *
 * Portable C11, caller-owned state, no heap.
 */
#ifndef ZW_ROUTES_H
#define ZW_ROUTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_nodes.h"
#include "zwave_host/zw_transport.h"
#include "zwave_proto/zw_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_ROUTES_TRIES      5u
#define ZW_ROUTES_BASE_MS    76000u /* a neighbour update: 76 s */
#define ZW_ROUTES_PER_LISTENING_MS 217u /* + this per listening node */
#define ZW_ROUTES_PER_FLIRS_MS 3500u /* + this per FLiRS node */
#define ZW_ROUTES_ROUTE_MS   30000u /* AssignSUCReturnRoute's / DeleteReturnRoute's callback */

typedef enum {
    ZW_ROUTES_PENDING = 0, /* not yet */
    ZW_ROUTES_ASLEEP,      /* waits for its wake-up */
    ZW_ROUTES_RUNNING,
    ZW_ROUTES_DONE,        /* its neighbours found again and its routes renewed */
    ZW_ROUTES_FAILED,      /* its neighbour update failed every try */
    ZW_ROUTES_SKIPPED      /* dead (it didn't answer), Long Range, or the job was stopped */
} zw_routes_node_state_t;

/* A device to repair: from the node cache, with what the owner knows of it. */
typedef struct {
    uint16_t node;
    bool sleeping;   /* repaired at its next wake-up */
    bool dead;       /* the owner counts it unreachable: pinged first */
    bool long_range; /* doesn't route: skipped */
} zw_routes_target_t;

typedef struct {
    uint16_t node;
    uint8_t state;   /* zw_routes_node_state_t */
    bool sleeping;
    bool dead;
    bool route_failed; /* its SUC return route could not be assigned */
} zw_routes_node_t;

typedef enum {
    ZW_ROUTES_EV_PROGRESS = 1, /* a device started or ended: `done` of `total` */
    ZW_ROUTES_EV_END           /* the job ended (every device, or stopped): `done`, `failed`,
                                * `skipped`, `stopped` */
} zw_routes_event_kind_t;

typedef struct {
    zw_routes_event_kind_t kind;
    uint16_t node;          /* PROGRESS: the device concerned */
    uint8_t state;          /* PROGRESS: its state now */
    uint16_t done, total;   /* devices ended (done, failed or skipped) of all */
    uint16_t failed, skipped, waiting;
    bool stopped;           /* END: the installer stopped it (the rest skipped) */
} zw_routes_event_t;

typedef struct {
    void *ctx;
    /* A module command (48, 51, 47) for the host. */
    zw_submit_t (*submit)(void *ctx, const zw_request_t *req);
    /* A No Operation to the node; its end through zw_routes_ping_done() with `tag`. */
    bool (*ping)(void *ctx, uint16_t node, uint32_t tag);
    void (*event)(void *ctx, const zw_routes_event_t *ev);
} zw_routes_port_t;

typedef struct {
    zw_routes_port_t port;
    bool running;
    zw_nid_width_t nid;
    uint16_t listening_nodes, flirs_nodes; /* a neighbour update's timeout grows with them */
    uint64_t now;
    uint16_t gen;          /* this job: an older one's ends are ignored */
    zw_routes_node_t nodes[ZW_NODES_MAX];
    size_t n;
    size_t cur;            /* the device running; SIZE_MAX: none */
    uint8_t step;          /* zw_routes.c */
    uint8_t tries;
    bool out;              /* a command or ping of the device running is out */
} zw_routes_t;

void zw_routes_init(zw_routes_t *r, const zw_routes_port_t *port);
/* Starts a job over `targets` (one device, or every device of the network). False: one runs
 * already, or no target. */
bool zw_routes_start(zw_routes_t *r, uint64_t now, const zw_routes_target_t *targets, size_t n,
                     uint16_t listening_nodes, uint16_t flirs_nodes, zw_nid_width_t nid);
/* The same, the targets given one by one (no array of the caller's): prepare, add each, go. */
bool zw_routes_prepare(zw_routes_t *r);
bool zw_routes_add(zw_routes_t *r, const zw_routes_target_t *t);
bool zw_routes_go(zw_routes_t *r, uint64_t now, uint16_t listening_nodes, uint16_t flirs_nodes,
                  zw_nid_width_t nid);
/* The installer stops it: the device running finishes its step, the rest are skipped. */
void zw_routes_stop(zw_routes_t *r, uint64_t now);
/* A sleeping device woke (its Wake Up Notification): true when the job takes it now - the owner
 * keeps it awake until zw_routes_holding() no longer names it. */
bool zw_routes_woke(zw_routes_t *r, uint64_t now, uint16_t node);
/* The device the job keeps awake now (a sleeper being repaired); 0: none. */
uint16_t zw_routes_holding(const zw_routes_t *r);
void zw_routes_ping_done(zw_routes_t *r, uint64_t now, uint32_t tag, bool delivered);
bool zw_routes_is_tag(uint32_t tag);
void zw_routes_link_down(zw_routes_t *r, uint64_t now);
void zw_routes_poll(zw_routes_t *r, uint64_t now);
bool zw_routes_running(const zw_routes_t *r);
/* done, failed, skipped of total, and those waiting for their wake-up. */
void zw_routes_counts(const zw_routes_t *r, uint16_t *done, uint16_t *total, uint16_t *failed,
                      uint16_t *skipped, uint16_t *waiting);
/* A device's state in the job (ZW_ROUTES_PENDING when it isn't in it). */
uint8_t zw_routes_node_state(const zw_routes_t *r, uint16_t node);

#ifdef __cplusplus
}
#endif

#endif /* ZW_ROUTES_H */
