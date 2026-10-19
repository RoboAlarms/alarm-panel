/*
 * plink_test_net.h - a pretend network for the panel link's tests (docs/features/11 "Test and
 * acceptance"): nodes with UDP-style addresses exchange datagrams on a fake clock, with
 * deterministic faults - loss, duplicates, reordering (a random extra delay), corruption, one
 * direction cut, everything held. A node is a context with three callbacks (the managers' input,
 * tick and next), so the master and keypads run exactly as on a panel, with no sockets.
 *
 * Usage:
 *   plink_net_init(&net, seed);
 *   int m = plink_net_node(&net, ip_m, ctx_m, m_input, m_tick, m_next);
 *   ... the managers' port.send calls plink_net_send(&net, from_node, to, frame, len) ...
 *   plink_net_run(&net, 30000);   // 30 s of fake time
 */
#ifndef PLINK_TEST_NET_H
#define PLINK_TEST_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_frame.h"

#define PLINK_NET_NODES  12u
#define PLINK_NET_QUEUE  512u
#define PLINK_NET_LOG    1024u
#define PLINK_NET_RECENT 64u

typedef void (*plink_net_input_fn)(void *ctx, uint64_t now_ms, const plink_addr_t *from,
                                   const uint8_t *frame, size_t len);
typedef void (*plink_net_tick_fn)(void *ctx, uint64_t now_ms);
typedef uint64_t (*plink_net_next_fn)(void *ctx);

typedef struct {
    plink_addr_t addr;
    void *ctx;
    plink_net_input_fn input;
    plink_net_tick_fn tick;
    plink_net_next_fn next;
    bool down;          /* switched off: sends nothing, hears nothing */
} plink_net_node_t;

typedef struct {
    int from;           /* node index */
    plink_addr_t from_addr;
    plink_addr_t to;
    uint64_t at_ms;     /* delivery time */
    uint64_t order;     /* datagrams due at the same ms go in the order they were sent */
    size_t len;
    uint8_t b[PLINK_FRAME_MAX];
} plink_net_dgram_t;

/* What went over the network (delivered or not), for assertions: type and sizes. */
typedef struct {
    int from;
    int to;             /* -1: no node has that address (broadcast goes to all others: -2) */
    uint8_t type;
    size_t len;
    bool lost;
    bool corrupted;     /* delivered with a byte flipped */
    uint64_t at_ms;
} plink_net_seen_t;

typedef struct {
    uint64_t now_ms;
    uint64_t rng;
    plink_net_node_t node[PLINK_NET_NODES];
    int nodes;
    plink_net_dgram_t q[PLINK_NET_QUEUE];
    size_t q_count;
    uint64_t q_order;   /* the next datagram's order */
    plink_net_seen_t log[PLINK_NET_LOG];
    size_t log_count;
    plink_net_dgram_t recent[PLINK_NET_RECENT]; /* the newest datagrams sent, for replays */
    size_t recent_next;
    /* Faults, applied to every datagram as it is sent (per mille: 0..1000). */
    uint32_t delay_ms;       /* every datagram's base delay (default 2 ms) */
    uint32_t jitter_ms;      /* a random extra 0..jitter (reorders when > delay gaps) */
    uint32_t loss_pm;
    uint32_t dup_pm;
    uint32_t corrupt_pm;     /* one byte flipped */
    bool hold;               /* nothing is delivered until released */
    int cut_from;            /* datagrams from this node are lost (-1 none) */
    int cut_to;              /* datagrams to this node are lost (-1 none) */
    uint32_t lose_next;      /* the next n datagrams (from anyone) are lost */
    int lose_next_from;      /* ... only those from this node (-1 any) */
    uint8_t lose_type;       /* datagrams of this message type are lost (0 none) */
    uint32_t lose_type_count;/* ... this many times (0 = always while lose_type is set) */
    int lose_type_from;      /* ... only those from this node (-1 any) */
    uint8_t subnet_bcast[4]; /* a subnet's broadcast, e.g. x.y.z.127 on a /25 (0: none) */
    /* The longest wait between two steps when nothing is due (0: 50 ms). A test may lengthen it
     * for hours with nothing to do, when every node's next() names all its deadlines. */
    uint32_t step_ms;
} plink_net_t;

void plink_net_init(plink_net_t *net, uint64_t seed);
/* Adds a node at ip:PLINK_UDP_PORT; returns its index. */
int plink_net_node(plink_net_t *net, const uint8_t ip[4], void *ctx, plink_net_input_fn input,
                   plink_net_tick_fn tick, plink_net_next_fn next);
const plink_addr_t *plink_net_addr(const plink_net_t *net, int node);
/* A node's port.send: queues the datagram (faults applied). A destination of 255.255.255.255,
 * or subnet_bcast when set, reaches every other node. Always true (UDP doesn't know). */
bool plink_net_send(plink_net_t *net, int from, const plink_addr_t *to, const uint8_t *frame,
                    size_t len);
/* Runs ms of fake time: delivers due datagrams in time order and ticks every node at its next
 * deadline (at least every 50 ms, or step_ms). */
void plink_net_run(plink_net_t *net, uint64_t ms);
/* Runs until cond(ctx) is true or ms passed; true when it became true. */
bool plink_net_run_until(plink_net_t *net, uint64_t ms, bool (*cond)(void *ctx), void *ctx);
/* Releases held datagrams (hold = false) and delivers what is due. */
void plink_net_release(plink_net_t *net);
/* Drops everything queued. */
void plink_net_flush(plink_net_t *net);
/* A copy of a queued or delivered datagram to replay later: the newest one of that type sent
 * by node from (false: none seen). Kept in a small ring per test. */
bool plink_net_last(const plink_net_t *net, int from, uint8_t type, uint8_t *out, size_t cap,
                    size_t *len);
/* Injects a datagram as if node from sent it (no faults). */
void plink_net_inject(plink_net_t *net, int from, const plink_addr_t *to, const uint8_t *frame,
                      size_t len);
/* How many datagrams of a type node from sent (delivered or not). */
size_t plink_net_count(const plink_net_t *net, int from, uint8_t type);

#endif /* PLINK_TEST_NET_H */
