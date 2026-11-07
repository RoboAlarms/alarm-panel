/*
 * tls_gate.c - who may start a TLS handshake: the buckets (tls_gate.h, SEC-012). Portable.
 */
#include "tls_gate.h"

#include <string.h>

static uint16_t refill(uint16_t tokens, int64_t *at_us, int64_t now_us, int64_t every_us,
                       uint16_t burst)
{
    if (tokens >= burst) {
        *at_us = now_us;
        return burst;
    }
    if (now_us <= *at_us) {
        return tokens;
    }
    const int64_t gained = (now_us - *at_us) / every_us;
    if (gained >= (int64_t)(burst - tokens)) {
        *at_us = now_us;
        return burst;
    }
    *at_us += gained * every_us;
    return (uint16_t)(tokens + (uint16_t)gained);
}

static bool trusted(const tls_gate_peer_t *p, int64_t now_us)
{
    return p->used && p->trusted_until_us > now_us;
}

/* The address's slot; a new one takes a free slot or the one heard from longest ago, an
 * untrusted one before any trusted one (F20: a flood of addresses can't push a phone out). */
static tls_gate_peer_t *peer_of(tls_gate_t *g, const uint8_t addr[16], int64_t now_us, bool add)
{
    tls_gate_peer_t *oldest = NULL;
    for (size_t i = 0; i < TLS_GATE_PEERS; i++) {
        tls_gate_peer_t *p = &g->peer[i];
        if (p->used && memcmp(p->addr, addr, sizeof(p->addr)) == 0) {
            return p;
        }
        if (oldest == NULL || (oldest->used && !p->used)) {
            oldest = p;
        } else if (p->used && oldest->used) {
            const bool pt = trusted(p, now_us), ot = trusted(oldest, now_us);
            if ((ot && !pt) || (ot == pt && p->seen_us < oldest->seen_us)) {
                oldest = p;
            }
        }
    }
    if (!add || oldest == NULL) {
        return NULL;
    }
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->addr, addr, sizeof(oldest->addr));
    oldest->used = true;
    oldest->tokens = TLS_GATE_PEER_BURST;
    oldest->refill_at_us = now_us;
    oldest->seen_us = now_us;
    return oldest;
}

tls_gate_verdict_t tls_gate_admit(tls_gate_t *g, const uint8_t addr[16], int64_t now_us,
                                  size_t internal_free, size_t internal_block)
{
    if (!g->started) {
        g->started = true;
        g->all_tokens = TLS_GATE_ALL_BURST;
        g->all_refill_at_us = now_us;
    }
    tls_gate_verdict_t v = TLS_GATE_ADMIT;
    tls_gate_peer_t *p = peer_of(g, addr, now_us, true);
    p->seen_us = now_us;
    p->tokens = refill(p->tokens, &p->refill_at_us, now_us, TLS_GATE_PEER_REFILL_US,
                       TLS_GATE_PEER_BURST);
    g->all_tokens = refill(g->all_tokens, &g->all_refill_at_us, now_us, TLS_GATE_ALL_REFILL_US,
                           TLS_GATE_ALL_BURST);
    if (internal_free < TLS_GATE_MIN_FREE || internal_block < TLS_GATE_MIN_BLOCK) {
        v = TLS_GATE_LOW_MEMORY;
    } else if (p->tokens == 0u) {
        v = TLS_GATE_PEER_LIMIT;
    } else if (trusted(p, now_us)) {
        p->tokens--; /* F20: the shared bucket is for addresses that haven't proved who they are */
    } else if (g->all_tokens == 0u) {
        v = TLS_GATE_ALL_LIMIT;
    } else {
        p->tokens--;
        g->all_tokens--;
    }
    g->counts[v]++;
    return v;
}

void tls_gate_failed(tls_gate_t *g, const uint8_t addr[16], int64_t now_us)
{
    tls_gate_peer_t *p = peer_of(g, addr, now_us, false);
    if (p == NULL) {
        return;
    }
    const uint16_t extra = TLS_GATE_FAIL_COST - 1u;
    p->tokens = p->tokens > extra ? (uint16_t)(p->tokens - extra) : 0u;
}

void tls_gate_trust(tls_gate_t *g, const uint8_t addr[16], int64_t now_us)
{
    tls_gate_peer_t *p = peer_of(g, addr, now_us, false);
    if (p != NULL) {
        p->trusted_until_us = now_us + TLS_GATE_TRUST_US;
    }
}

const char *tls_gate_verdict_name(tls_gate_verdict_t v)
{
    switch (v) {
    case TLS_GATE_ADMIT: return "admitted";
    case TLS_GATE_PEER_LIMIT: return "address over its limit";
    case TLS_GATE_ALL_LIMIT: return "all addresses over the limit";
    case TLS_GATE_LOW_MEMORY: return "internal RAM short";
    default: return "?";
    }
}
