/*
 * web_signin_gate.c - password tries per source address (web_signin_gate.h, F8). Portable.
 */
#include "web_signin_gate.h"

#include <string.h>

/* The source's slot; a new one takes a free slot or the one heard from longest ago. */
static signin_source_t *source_of(signin_gate_t *g, const uint8_t addr[16], int64_t now_us)
{
    signin_source_t *oldest = NULL;
    for (unsigned i = 0; i < SIGNIN_SOURCES; i++) {
        signin_source_t *s = &g->src[i];
        if (s->used && memcmp(s->addr, addr, sizeof(s->addr)) == 0)
            return s;
        if (oldest == NULL || (oldest->used && (!s->used || s->seen_us < oldest->seen_us)))
            oldest = s;
    }
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->addr, addr, sizeof(oldest->addr));
    oldest->used = true;
    oldest->seen_us = now_us;
    return oldest;
}

static int64_t hold_for(unsigned fails, unsigned free_tries, int64_t first, int64_t max)
{
    const unsigned over = fails - free_tries;
    const int64_t hold = first << (over > 8u ? 8u : over);
    return hold > max ? max : hold;
}

signin_verdict_t signin_gate_try(signin_gate_t *g, const uint8_t addr[16], int64_t now_us,
                                 bool spaced)
{
    signin_source_t *s = source_of(g, addr, now_us);
    s->seen_us = now_us;
    if (s->fails && now_us - s->last_fail_us > SIGNIN_FORGET_US)
        s->fails = 0;
    if (g->shared_fails && now_us - g->shared_last_fail_us > SIGNIN_FORGET_US)
        g->shared_fails = 0;
    if (now_us < s->hold_until_us || now_us < g->shared_hold_until_us)
        return SIGNIN_HELD;
    if (spaced) {
        if (now_us < s->next_try_us)
            return SIGNIN_TOO_SOON;
        s->next_try_us = now_us + SIGNIN_SPACING_US;
    }
    return SIGNIN_OK;
}

void signin_gate_failed(signin_gate_t *g, const uint8_t addr[16], int64_t now_us)
{
    signin_source_t *s = source_of(g, addr, now_us);
    s->last_fail_us = now_us;
    if (++s->fails >= SIGNIN_FREE_TRIES)
        s->hold_until_us = now_us + hold_for(s->fails, SIGNIN_FREE_TRIES, SIGNIN_HOLD_US,
                                             SIGNIN_HOLD_MAX);
    g->shared_last_fail_us = now_us;
    if (++g->shared_fails >= SIGNIN_SHARED_FREE)
        g->shared_hold_until_us = now_us + hold_for(g->shared_fails, SIGNIN_SHARED_FREE,
                                                    SIGNIN_SHARED_HOLD_US, SIGNIN_SHARED_MAX);
}
