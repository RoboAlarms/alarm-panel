/*
 * zsrc_wired.c - wired zone inputs (see zsrc_wired.h).
 */
#include "zone_hub/zsrc_wired.h"
#include "zone_hub/zone_eol.h"

#include <stdio.h>
#include <string.h>

static int find(const zsrc_wired_t *w, const zsrc_address_t *a)
{
    if (a->kind != ZSRC_WIRED || a->input > ZEOL_IN_LAST || a->ref[0] != '\0' || a->id > 0xFFu) {
        return -1;
    }
    for (size_t i = 0; i < w->n; i++) {
        if (w->terms[i].gpio == a->id) {
            /* an end-of-line loop only on a terminal that measures, with a port that can */
            if (a->input != ZEOL_IN_NONE &&
                (!w->terms[i].eol || w->gpio.read_mv == NULL || w->gpio.setup_analog == NULL)) {
                return -1;
            }
            return (int)i;
        }
    }
    return -1;
}

/* An end-of-line loop's sample: its reading (zeol_reading_t), or the last one if none came. */
static uint8_t read_eol(zsrc_wired_t *w, size_t i)
{
    zsrc_wired_term_t *t = &w->st[i];
    uint32_t mv = 0u;
    uint32_t vref = 0u;
    if (!w->gpio.read_mv(w->gpio.ctx, w->terms[i].gpio, &mv, &vref)) {
        return t->db.primed ? t->db.stable : (uint8_t)ZEOL_NORMAL;
    }
    t->mv = mv;
    return (uint8_t)zeol_classify(t->mode, mv, vref);
}

/* An end-of-line loop's reading as the zone's events: its fault as an input (0/1), its tamper
 * and trouble as events, each only when it changes (all of them the first time). */
static void report_eol(zsrc_wired_t *w, size_t i, uint64_t now)
{
    zsrc_wired_term_t *t = &w->st[i];
    const zsrc_address_t a = zsrc_wired_eol_address(w->terms[i].gpio, t->mode);
    const zeol_meaning_t m = zeol_mean(t->mode, (zeol_reading_t)t->db.stable);
    if (w->sink.input != NULL) {
        w->sink.input(w->sink.ctx, &a, NULL, m.fault ? 1 : 0, now);
    }
    if (w->sink.event != NULL && (!t->told || m.tamper != t->tamper)) {
        w->sink.event(w->sink.ctx, &a, m.tamper ? ALARM_ZEV_TAMPER : ALARM_ZEV_TAMPER_RESTORE, now);
    }
    if (w->sink.event != NULL && (!t->told || m.trouble != t->trouble)) {
        w->sink.event(w->sink.ctx, &a, m.trouble ? ALARM_ZEV_TROUBLE : ALARM_ZEV_TROUBLE_RESTORE,
                      now);
    }
    t->tamper = m.tamper;
    t->trouble = m.trouble;
    t->told = true;
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
    if (w->st[i].mode != ZEOL_IN_NONE) {
        report_eol(w, i, now);
        return;
    }
    const zsrc_address_t a = zsrc_wired_address(w->terms[i].gpio);
    if (w->sink.input != NULL) {
        w->sink.input(w->sink.ctx, &a, NULL, w->st[i].db.stable, now);
    }
}

static bool wired_valid(void *ctx, const zsrc_address_t *a)
{
    const zsrc_wired_t *w = ctx;
    const int i = find(w, a);
    return i >= 0 && !w->st[i].reserved;
}

static bool wired_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    zsrc_wired_t *w = ctx;
    const int i = find(w, a);
    if (i < 0) {
        return false;
    }
    zsrc_wired_term_t *t = &w->st[i];
    if (t->reserved) {
        return false;
    }
    const uint16_t hold = m->response_ms != 0u ? m->response_ms : ZSRC_WIRED_RESPONSE_MS;
    if (t->watched && t->mode == a->input) {
        t->db.hold_ms = hold; /* a new response time; the level stays */
        return true;
    }
    if (t->watched) {
        return false; /* another wiring of a terminal already watched */
    }
    if (a->input != ZEOL_IN_NONE) {
        /* an end-of-line loop (ZONE-005): analog, no pull-up; set up for learning: given back */
        if (t->setup) {
            release(w, (size_t)i);
        }
        if (!w->gpio.setup_analog(w->gpio.ctx, w->terms[i].gpio, true)) {
            return false;
        }
        t->analog = true;
        t->mode = a->input;
        t->watched = true;
        t->told = false;
        zone_debounce_init(&t->db, hold);
        (void)zone_debounce_sample_level(&t->db, read_eol(w, (size_t)i), now);
        report(w, (size_t)i, now);
        return true;
    }
    if (!setup(w, (size_t)i, true)) {
        return false;
    }
    t->mode = ZEOL_IN_NONE;
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
    if (i < 0 || w->st[i].mode != a->input) {
        return;
    }
    zsrc_wired_term_t *t = &w->st[i];
    t->watched = false;
    if (t->analog) {
        (void)w->gpio.setup_analog(w->gpio.ctx, w->terms[i].gpio, false);
        t->analog = false;
        t->mode = ZEOL_IN_NONE;
        memset(&t->db, 0, sizeof(t->db));
        return;
    }
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
        if (t->reserved || t->analog) {
            continue; /* an output's, or a measured loop's: left as they are */
        }
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
        if (t->reserved) {
            continue;
        }
        zsrc_candidate_t *c = &out[n++];
        memset(c, 0, sizeof(*c));
        c->addr = zsrc_wired_address(w->terms[i].gpio);
        snprintf(c->name, sizeof(c->name), "GPIO %u", (unsigned)w->terms[i].gpio);
        snprintf(c->detail, sizeof(c->detail), "%s", w->terms[i].where != NULL ? w->terms[i].where : "");
        c->device_class = ALARM_DEVCLASS_GENERIC;
        c->eol = w->terms[i].eol && w->gpio.read_mv != NULL;
        if (t->analog && t->db.primed) {
            snprintf(c->state, sizeof(c->state), "%s", zeol_reading_name((zeol_reading_t)t->db.stable));
        } else if (t->setup && t->db.primed) {
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
    const char *loop = a->input == ZEOL_IN_NC   ? ", end-of-line, normally closed"
                       : a->input == ZEOL_IN_NO ? ", end-of-line, normally open"
                       : a->input == ZEOL_IN_DEOL ? ", double end-of-line"
                                                  : "";
    if (i >= 0 && w->terms[i].where != NULL && w->terms[i].where[0] != '\0') {
        snprintf(out, len, "GPIO %u on the %s%s", (unsigned)a->id, w->terms[i].where, loop);
    } else {
        snprintf(out, len, "GPIO %u%s", (unsigned)a->id, loop);
    }
}

static void wired_tick(void *ctx, uint64_t now)
{
    zsrc_wired_t *w = ctx;
    for (size_t i = 0; i < w->n; i++) {
        zsrc_wired_term_t *t = &w->st[i];
        if (t->analog) {
            if (zone_debounce_sample_level(&t->db, read_eol(w, i), now)) {
                report(w, i, now);
            }
            continue;
        }
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
        if (w->st[i].setup || w->st[i].analog) {
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

zsrc_address_t zsrc_wired_eol_address(uint8_t gpio, uint8_t input)
{
    return zsrc_address(ZSRC_WIRED, input, gpio);
}

bool zsrc_wired_eol_capable(const zsrc_wired_t *w, uint8_t gpio)
{
    for (size_t i = 0; i < w->n; i++) {
        if (w->terms[i].gpio == gpio) {
            return w->terms[i].eol && w->gpio.read_mv != NULL && w->gpio.setup_analog != NULL;
        }
    }
    return false;
}

static int find_gpio(const zsrc_wired_t *w, uint8_t gpio)
{
    for (size_t i = 0; i < w->n; i++) {
        if (w->terms[i].gpio == gpio) {
            return (int)i;
        }
    }
    return -1;
}

bool zsrc_wired_reserve(zsrc_wired_t *w, uint8_t gpio, bool reserved)
{
    const int i = find_gpio(w, gpio);
    if (i < 0 || (reserved && w->st[i].watched)) {
        return false;
    }
    if (reserved && w->st[i].setup) {
        release(w, (size_t)i); /* set up while learning: given back before the output takes it */
    }
    w->st[i].reserved = reserved;
    return true;
}

bool zsrc_wired_in_use(const zsrc_wired_t *w, uint8_t gpio)
{
    const int i = find_gpio(w, gpio);
    return i >= 0 && w->st[i].watched;
}
