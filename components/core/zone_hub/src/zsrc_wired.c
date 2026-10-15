/*
 * zsrc_wired.c - wired zone inputs (see zsrc_wired.h).
 */
#include "zone_hub/zsrc_wired.h"

#include <stdio.h>
#include <string.h>

static int find(const zsrc_wired_t *w, const zsrc_address_t *a)
{
    if (a->kind != ZSRC_WIRED || a->input != 0u || a->ref[0] != '\0' || a->id > 0xFFu) {
        return -1;
    }
    for (size_t i = 0; i < w->n; i++) {
        if (w->terms[i].gpio == a->id) {
            return (int)i;
        }
    }
    return -1;
}

static bool setup(zsrc_wired_t *w, size_t i, bool on)
{
    zsrc_wired_term_t *t = &w->st[i];
    if (t->setup == on) {
        return true;
    }
    if (w->gpio.setup != NULL && !w->gpio.setup(w->gpio.ctx, w->terms[i].gpio, on)) {
        return false;
    }
    t->setup = on;
    return true;
}

/* Gives the pin back and forgets its level. */
static void release(zsrc_wired_t *w, size_t i)
{
    setup(w, i, false);
    memset(&w->st[i].db, 0, sizeof(w->st[i].db));
    w->st[i].changed = false;
}

static uint8_t read_pin(const zsrc_wired_t *w, size_t i)
{
    const uint8_t l = w->gpio.read != NULL ? w->gpio.read(w->gpio.ctx, w->terms[i].gpio) : 0u;
    return l != 0u ? 1u : 0u;
}

static void report(zsrc_wired_t *w, size_t i, uint64_t now)
{
    const zsrc_address_t a = zsrc_wired_address(w->terms[i].gpio);
    if (w->sink.input != NULL) {
        w->sink.input(w->sink.ctx, &a, NULL, w->st[i].db.stable, now);
    }
}

static bool wired_valid(void *ctx, const zsrc_address_t *a)
{
    return find(ctx, a) >= 0;
}

static bool wired_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    zsrc_wired_t *w = ctx;
    const int i = find(w, a);
    if (i < 0) {
        return false;
    }
    zsrc_wired_term_t *t = &w->st[i];
    const uint16_t hold = m->response_ms != 0u ? m->response_ms : ZSRC_WIRED_RESPONSE_MS;
    if (t->watched) {
        t->db.hold_ms = hold; /* a new response time; the level stays */
        return true;
    }
    if (!setup(w, (size_t)i, true)) {
        return false;
    }
    t->watched = true;
    if (t->db.primed) {
        t->db.hold_ms = hold; /* picked while learning: its level is known already */
    } else {
        zone_debounce_init(&t->db, hold);
        zone_debounce_sample(&t->db, read_pin(w, (size_t)i), now); /* the state at start */
    }
    report(w, (size_t)i, now);
    return true;
}

static void wired_unwatch(void *ctx, const zsrc_address_t *a)
{
    zsrc_wired_t *w = ctx;
    const int i = find(w, a);
    if (i < 0) {
        return;
    }
    w->st[i].watched = false;
    if (!w->learning) {
        release(w, (size_t)i);
    }
}

static bool wired_learn(void *ctx, bool on, uint64_t now)
{
    zsrc_wired_t *w = ctx;
    w->learning = on;
    for (size_t i = 0; i < w->n; i++) {
        zsrc_wired_term_t *t = &w->st[i];
        if (on) {
            if (!t->watched && setup(w, i, true)) {
                zone_debounce_init(&t->db, ZSRC_WIRED_RESPONSE_MS);
                zone_debounce_sample(&t->db, read_pin(w, i), now);
            }
            t->learn_level = t->db.stable;
            t->changed = false;
        } else if (!t->watched) {
            release(w, i);
        }
    }
    return true;
}

static size_t wired_candidates(void *ctx, const char *query, uint32_t filter,
                               zsrc_candidate_t *out, size_t max)
{
    (void)query;
    (void)filter;
    const zsrc_wired_t *w = ctx;
    size_t n = 0;
    for (size_t i = 0; i < w->n && n < max; i++) {
        const zsrc_wired_term_t *t = &w->st[i];
        zsrc_candidate_t *c = &out[n++];
        memset(c, 0, sizeof(*c));
        c->addr = zsrc_wired_address(w->terms[i].gpio);
        snprintf(c->name, sizeof(c->name), "GPIO %u", (unsigned)w->terms[i].gpio);
        snprintf(c->detail, sizeof(c->detail), "%s", w->terms[i].where != NULL ? w->terms[i].where : "");
        c->device_class = ALARM_DEVCLASS_GENERIC;
        if (t->setup && t->db.primed) {
            snprintf(c->state, sizeof(c->state), "%s", t->db.stable != 0u ? "open" : "closed");
        }
        c->changed = t->changed;
    }
    return n;
}

static void wired_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    const zsrc_wired_t *w = ctx;
    const int i = find(w, a);
    if (i >= 0 && w->terms[i].where != NULL && w->terms[i].where[0] != '\0') {
        snprintf(out, len, "GPIO %u on the %s", (unsigned)a->id, w->terms[i].where);
    } else {
        snprintf(out, len, "GPIO %u", (unsigned)a->id);
    }
}

static void wired_tick(void *ctx, uint64_t now)
{
    zsrc_wired_t *w = ctx;
    for (size_t i = 0; i < w->n; i++) {
        zsrc_wired_term_t *t = &w->st[i];
        if (!t->setup) {
            continue;
        }
        if (zone_debounce_sample(&t->db, read_pin(w, i), now) && t->watched) {
            report(w, i, now);
        }
        if (w->learning && t->db.stable != t->learn_level) {
            t->changed = true; /* opened or closed while the installer watched */
        }
    }
}

static uint64_t wired_next(void *ctx, uint64_t now)
{
    const zsrc_wired_t *w = ctx;
    for (size_t i = 0; i < w->n; i++) {
        if (w->st[i].setup) {
            return now + ZSRC_WIRED_POLL_MS;
        }
    }
    return UINT64_MAX;
}

static const zone_source_ops_t WIRED_OPS = {
    .kind = ZSRC_WIRED,
    .caps = ZSRC_CAP_LEARN | ZSRC_CAP_AUTH | ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_LIFE_SAFETY,
    .name = "Wired input",
    .valid = wired_valid,
    .watch = wired_watch,
    .unwatch = wired_unwatch,
    .learn = wired_learn,
    .candidates = wired_candidates,
    .describe = wired_describe,
    .tick = wired_tick,
    .next_tick = wired_next,
};

void zsrc_wired_init(zsrc_wired_t *w, const zsrc_wired_terminal_t *terms, size_t n,
                     const zsrc_gpio_port_t *gpio, const zsrc_sink_t *sink)
{
    memset(w, 0, sizeof(*w));
    w->terms = terms;
    w->n = n < ZSRC_WIRED_MAX ? n : ZSRC_WIRED_MAX;
    if (gpio != NULL) {
        w->gpio = *gpio;
    }
    if (sink != NULL) {
        w->sink = *sink;
    }
}

const zone_source_ops_t *zsrc_wired_ops(void)
{
    return &WIRED_OPS;
}

zsrc_address_t zsrc_wired_address(uint8_t gpio)
{
    return zsrc_address(ZSRC_WIRED, 0u, gpio);
}
