/*
 * plink_test_net.c - see plink_test_net.h.
 */
#include "plink_test_net.h"

#include <string.h>

#include "unity.h"

static uint64_t rnd(plink_net_t *net)
{
    uint64_t z = (net->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static bool chance(plink_net_t *net, uint32_t per_mille)
{
    return per_mille != 0u && (uint32_t)(rnd(net) % 1000u) < per_mille;
}

void plink_net_init(plink_net_t *net, uint64_t seed)
{
    memset(net, 0, sizeof(*net));
    net->rng = seed;
    net->now_ms = 1000u;
    net->delay_ms = 2u;
    net->cut_from = -1;
    net->cut_to = -1;
    net->lose_next_from = -1;
}

int plink_net_node(plink_net_t *net, const uint8_t ip[4], void *ctx, plink_net_input_fn input,
                   plink_net_tick_fn tick, plink_net_next_fn next)
{
    TEST_ASSERT_TRUE_MESSAGE(net->nodes < (int)PLINK_NET_NODES, "too many nodes");
    plink_net_node_t *n = &net->node[net->nodes];
    n->addr = plink_addr_udp(ip, (uint16_t)PLINK_UDP_PORT);
    n->ctx = ctx;
    n->input = input;
    n->tick = tick;
    n->next = next;
    n->down = false;
    return net->nodes++;
}

const plink_addr_t *plink_net_addr(const plink_net_t *net, int node)
{
    return &net->node[node].addr;
}

static int node_at(const plink_net_t *net, const plink_addr_t *a)
{
    for (int i = 0; i < net->nodes; i++) {
        if (plink_addr_equal(&net->node[i].addr, a)) {
            return i;
        }
    }
    return -1;
}

static bool is_broadcast(const plink_addr_t *a)
{
    return a->tp == PLINK_TP_UDP && a->len >= 4u && a->a[0] == 255u && a->a[1] == 255u &&
           a->a[2] == 255u && a->a[3] == 255u;
}

static void note(plink_net_t *net, int from, int to, const uint8_t *frame, size_t len, bool lost)
{
    if (net->log_count < PLINK_NET_LOG) {
        plink_net_seen_t *s = &net->log[net->log_count++];
        s->from = from;
        s->to = to;
        s->type = len > 3u ? frame[3] : 0u;
        s->len = len;
        s->lost = lost;
        s->at_ms = net->now_ms;
    }
}

static void enqueue(plink_net_t *net, int from, const plink_addr_t *to, const uint8_t *frame,
                    size_t len, uint64_t at)
{
    TEST_ASSERT_TRUE_MESSAGE(net->q_count < PLINK_NET_QUEUE, "the network queue is full");
    TEST_ASSERT_TRUE_MESSAGE(len <= PLINK_FRAME_MAX, "a datagram over 1200 bytes");
    plink_net_dgram_t *d = &net->q[net->q_count++];
    d->from = from;
    d->from_addr = net->node[from].addr;
    d->to = *to;
    d->at_ms = at;
    d->len = len;
    memcpy(d->b, frame, len);
}

static bool lose(plink_net_t *net, int from, int to, const uint8_t *frame, size_t len)
{
    if (net->cut_from == from || (to >= 0 && net->cut_to == to)) {
        return true;
    }
    if (net->lose_next > 0u && (net->lose_next_from < 0 || net->lose_next_from == from)) {
        net->lose_next--;
        return true;
    }
    if (net->lose_type != 0u && len > 3u && frame[3] == net->lose_type) {
        if (net->lose_type_count > 0u) {
            if (--net->lose_type_count == 0u) {
                net->lose_type = 0u;
            }
        }
        return true;
    }
    return chance(net, net->loss_pm);
}

bool plink_net_send(plink_net_t *net, int from, const plink_addr_t *to, const uint8_t *frame,
                    size_t len)
{
    TEST_ASSERT_TRUE(from >= 0 && from < net->nodes);
    TEST_ASSERT_TRUE_MESSAGE(len <= PLINK_FRAME_MAX, "a datagram over 1200 bytes");
    if (net->node[from].down) {
        return true;
    }
    plink_net_dgram_t *r = &net->recent[net->recent_next++ % PLINK_NET_RECENT];
    r->from = from;
    r->from_addr = net->node[from].addr;
    r->to = *to;
    r->len = len;
    memcpy(r->b, frame, len);
    const int to_node = is_broadcast(to) ? -2 : node_at(net, to);
    const bool lost = lose(net, from, to_node, frame, len);
    note(net, from, to_node, frame, len, lost);
    if (lost) {
        return true;
    }
    uint8_t buf[PLINK_FRAME_MAX];
    memcpy(buf, frame, len);
    if (chance(net, net->corrupt_pm) && len > 0u) {
        buf[rnd(net) % len] ^= (uint8_t)(1u + rnd(net) % 255u);
    }
    const uint64_t jitter = net->jitter_ms != 0u ? rnd(net) % (net->jitter_ms + 1u) : 0u;
    enqueue(net, from, to, buf, len, net->now_ms + net->delay_ms + jitter);
    if (chance(net, net->dup_pm)) {
        const uint64_t j2 = net->jitter_ms != 0u ? rnd(net) % (net->jitter_ms + 1u) : 0u;
        enqueue(net, from, to, buf, len, net->now_ms + net->delay_ms + j2 + 1u);
    }
    return true;
}

void plink_net_inject(plink_net_t *net, int from, const plink_addr_t *to, const uint8_t *frame,
                      size_t len)
{
    enqueue(net, from, to, frame, len, net->now_ms + 1u);
}

static void deliver(plink_net_t *net, const plink_net_dgram_t *d)
{
    for (int i = 0; i < net->nodes; i++) {
        plink_net_node_t *n = &net->node[i];
        if (i == d->from || n->down) {
            continue;
        }
        if (is_broadcast(&d->to) || plink_addr_equal(&n->addr, &d->to)) {
            n->input(n->ctx, net->now_ms, &d->from_addr, d->b, d->len);
        }
    }
}

/* Delivers every datagram due by now, earliest first. */
static void deliver_due(plink_net_t *net)
{
    while (!net->hold) {
        size_t best = net->q_count;
        for (size_t i = 0; i < net->q_count; i++) {
            if (net->q[i].at_ms <= net->now_ms &&
                (best == net->q_count || net->q[i].at_ms < net->q[best].at_ms)) {
                best = i;
            }
        }
        if (best == net->q_count) {
            return;
        }
        plink_net_dgram_t d = net->q[best];
        net->q[best] = net->q[net->q_count - 1u];
        net->q_count--;
        deliver(net, &d);
    }
}

static uint64_t next_event(const plink_net_t *net, uint64_t limit)
{
    uint64_t t = net->now_ms + 50u;
    for (int i = 0; i < net->nodes; i++) {
        if (!net->node[i].down && net->node[i].next != NULL) {
            const uint64_t n = net->node[i].next(net->node[i].ctx);
            if (n < t) {
                t = n;
            }
        }
    }
    if (!net->hold) {
        for (size_t i = 0; i < net->q_count; i++) {
            if (net->q[i].at_ms < t) {
                t = net->q[i].at_ms;
            }
        }
    }
    if (t <= net->now_ms) {
        t = net->now_ms + 1u;
    }
    return t < limit ? t : limit;
}

static void step(plink_net_t *net)
{
    deliver_due(net);
    for (int i = 0; i < net->nodes; i++) {
        plink_net_node_t *n = &net->node[i];
        if (!n->down && n->tick != NULL) {
            n->tick(n->ctx, net->now_ms);
        }
    }
    deliver_due(net);
}

void plink_net_run(plink_net_t *net, uint64_t ms)
{
    const uint64_t end = net->now_ms + ms;
    step(net);
    while (net->now_ms < end) {
        net->now_ms = next_event(net, end);
        step(net);
    }
}

bool plink_net_run_until(plink_net_t *net, uint64_t ms, bool (*cond)(void *ctx), void *ctx)
{
    const uint64_t end = net->now_ms + ms;
    step(net);
    if (cond(ctx)) {
        return true;
    }
    while (net->now_ms < end) {
        net->now_ms = next_event(net, end);
        step(net);
        if (cond(ctx)) {
            return true;
        }
    }
    return false;
}

void plink_net_release(plink_net_t *net)
{
    net->hold = false;
    deliver_due(net);
}

void plink_net_flush(plink_net_t *net)
{
    net->q_count = 0;
}

bool plink_net_last(const plink_net_t *net, int from, uint8_t type, uint8_t *out, size_t cap,
                    size_t *len)
{
    for (size_t k = 1; k <= PLINK_NET_RECENT && k <= net->recent_next; k++) {
        const plink_net_dgram_t *r = &net->recent[(net->recent_next - k) % PLINK_NET_RECENT];
        if (r->from == from && r->len > 3u && r->b[3] == type && r->len <= cap) {
            memcpy(out, r->b, r->len);
            *len = r->len;
            return true;
        }
    }
    return false;
}

size_t plink_net_count(const plink_net_t *net, int from, uint8_t type)
{
    size_t n = 0;
    for (size_t i = 0; i < net->log_count; i++) {
        if (net->log[i].from == from && net->log[i].type == type) {
            n++;
        }
    }
    return n;
}
