/*
 * zsrc_keypad.c - a keypad's own wired inputs as the master's zones (zsrc_keypad.h, LINK-055).
 * Portable; plink_service (or a test) feeds it with each keypad's IO_STATE.
 */
#include <stdio.h>
#include <string.h>

#include "zone_hub/zsrc_keypad.h"

#define BIT64(n) (UINT64_C(1) << (n))

zsrc_address_t zsrc_keypad_address(uint8_t device, uint8_t gpio)
{
    return zsrc_address(ZSRC_KEYPAD, 0, ((uint64_t)device << 8) | gpio);
}

static bool split(const zsrc_address_t *a, uint8_t *device, uint8_t *gpio)
{
    if (a == NULL || a->kind != ZSRC_KEYPAD || a->input != 0u || a->id > 0xFFFFu) {
        return false;
    }
    *device = (uint8_t)(a->id >> 8);
    *gpio = (uint8_t)(a->id & 0xFFu);
    return *device >= 1u && *device <= ZSRC_KEYPAD_DEVICES && *gpio < 64u;
}

static zsrc_keypad_dev_t *dev_of(zsrc_keypad_t *k, uint8_t device)
{
    return device >= 1u && device <= ZSRC_KEYPAD_DEVICES ? &k->dev[device - 1u] : NULL;
}

static int term_of(const zsrc_keypad_dev_t *d, uint8_t gpio)
{
    for (size_t i = 0; i < d->n; i++) {
        if (d->term[i].gpio == gpio) {
            return (int)i;
        }
    }
    return -1;
}

static void tell(zsrc_keypad_t *k, uint8_t device, uint8_t gpio, bool active, uint64_t now)
{
    if (k->sink.input != NULL) {
        const zsrc_address_t a = zsrc_keypad_address(device, gpio);
        k->sink.input(k->sink.ctx, &a, NULL, active ? 1 : 0, now);
    }
}

static void reach(zsrc_keypad_t *k, uint8_t device, uint8_t gpio, bool up, uint64_t now)
{
    if (k->sink.reachable != NULL) {
        const zsrc_address_t a = zsrc_keypad_address(device, gpio);
        k->sink.reachable(k->sink.ctx, &a, up, now);
    }
}

void zsrc_keypad_init(zsrc_keypad_t *k, const zsrc_sink_t *sink)
{
    memset(k, 0, sizeof(*k));
    k->sink = *sink;
}

void zsrc_keypad_report(zsrc_keypad_t *k, uint8_t device, const zsrc_keypad_term_t *terms,
                        size_t n, uint64_t now)
{
    zsrc_keypad_dev_t *d = dev_of(k, device);
    if (d == NULL || (terms == NULL && n > 0u)) {
        return;
    }
    if (n > ZSRC_KEYPAD_TERMS) {
        n = ZSRC_KEYPAD_TERMS;
    }
    const bool first = !d->known;
    zsrc_keypad_dev_t old = *d;
    d->n = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        const zsrc_keypad_term_t *t = &terms[i];
        d->term[i] = *t;
        const int o = first ? -1 : term_of(&old, t->gpio);
        const bool known = (t->flags & ZSRC_KEYPAD_KNOWN) != 0u;
        const bool active = (t->flags & ZSRC_KEYPAD_ACTIVE) != 0u;
        const bool was_known = o >= 0 && (old.term[o].flags & ZSRC_KEYPAD_KNOWN) != 0u;
        const bool was_active = o >= 0 && (old.term[o].flags & ZSRC_KEYPAD_ACTIVE) != 0u;
        const uint16_t moved = o >= 0 ? (uint16_t)(t->changes - old.term[o].changes) : 0u;
        /* Learning: a terminal whose level moved since learning began is the one being tested. */
        if (k->learning) {
            const uint8_t base = o >= 0 ? old.learn_flags[o] : t->flags;
            d->learn_flags[i] = base;
            d->changed[i] = (o >= 0 && old.changed[o]) || moved != 0u ||
                            ((base ^ t->flags) & ZSRC_KEYPAD_ACTIVE) != 0u;
        } else {
            d->learn_flags[i] = t->flags;
            d->changed[i] = false;
        }
        if (t->gpio >= 64u || (d->watched & BIT64(t->gpio)) == 0u || !known) {
            continue;
        }
        if (!was_known) {
            tell(k, device, t->gpio, active, now);
            continue;
        }
        /* More changes than the level shows: a lost frame hid an opening (or a closing) that
         * already went back. The missed one first, so the zone sees its fault. */
        if (moved >= 2u && (moved % 2u == 0u) == (active == was_active)) {
            tell(k, device, t->gpio, !active, now);
            tell(k, device, t->gpio, active, now);
        } else if (moved != 0u || active != was_active) {
            tell(k, device, t->gpio, active, now);
        }
    }
    d->known = true;
    d->heard = now;
    if (!d->online) {
        zsrc_keypad_online(k, device, true, now);
    }
}

void zsrc_keypad_online(zsrc_keypad_t *k, uint8_t device, bool online, uint64_t now)
{
    zsrc_keypad_dev_t *d = dev_of(k, device);
    if (d == NULL || d->online == online) {
        return;
    }
    d->online = online;
    for (uint8_t g = 0; g < 64u; g++) {
        if ((d->watched & BIT64(g)) != 0u) {
            reach(k, device, g, online, now);
        }
    }
}

void zsrc_keypad_forget(zsrc_keypad_t *k, uint8_t device, uint64_t now)
{
    zsrc_keypad_dev_t *d = dev_of(k, device);
    if (d == NULL) {
        return;
    }
    zsrc_keypad_online(k, device, false, now);
    const uint64_t watched = d->watched;
    memset(d, 0, sizeof(*d));
    d->watched = watched; /* the zones stay bound; they CHECK until a keypad reports again */
}

/* ---- the source interface ------------------------------------------------------------------ */

static bool kp_valid(void *ctx, const zsrc_address_t *a)
{
    zsrc_keypad_t *k = ctx;
    uint8_t device, gpio;
    if (!split(a, &device, &gpio)) {
        return false;
    }
    const zsrc_keypad_dev_t *d = dev_of(k, device);
    return !d->known || term_of(d, gpio) >= 0; /* not heard yet (just started): any terminal */
}

static bool kp_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    (void)m; /* the response time is the keypad's own debouncing (ZSRC_WIRED_RESPONSE_MS) */
    zsrc_keypad_t *k = ctx;
    uint8_t device, gpio;
    if (!split(a, &device, &gpio)) {
        return false;
    }
    zsrc_keypad_dev_t *d = dev_of(k, device);
    d->watched |= BIT64(gpio);
    const int i = term_of(d, gpio);
    if (d->online && i >= 0 && (d->term[i].flags & ZSRC_KEYPAD_KNOWN) != 0u) {
        tell(k, device, gpio, (d->term[i].flags & ZSRC_KEYPAD_ACTIVE) != 0u, now);
    }
    return true;
}

static void kp_unwatch(void *ctx, const zsrc_address_t *a)
{
    zsrc_keypad_t *k = ctx;
    uint8_t device, gpio;
    if (split(a, &device, &gpio)) {
        dev_of(k, device)->watched &= ~BIT64(gpio);
    }
}

static bool kp_learn(void *ctx, bool on, uint64_t now)
{
    (void)now;
    zsrc_keypad_t *k = ctx;
    k->learning = on;
    for (size_t d = 0; d < ZSRC_KEYPAD_DEVICES; d++) {
        for (size_t i = 0; i < k->dev[d].n; i++) {
            k->dev[d].learn_flags[i] = k->dev[d].term[i].flags;
            k->dev[d].changed[i] = false;
        }
    }
    return true;
}

static size_t kp_candidates(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                            size_t max)
{
    (void)query;
    (void)filter;
    const zsrc_keypad_t *k = ctx;
    size_t n = 0;
    for (uint8_t d = 0; d < ZSRC_KEYPAD_DEVICES; d++) {
        const zsrc_keypad_dev_t *dv = &k->dev[d];
        for (size_t i = 0; i < dv->n && n < max; i++) {
            const zsrc_keypad_term_t *t = &dv->term[i];
            zsrc_candidate_t *c = &out[n++];
            memset(c, 0, sizeof(*c));
            c->addr = zsrc_keypad_address((uint8_t)(d + 1u), t->gpio);
            snprintf(c->name, sizeof(c->name), "IO%u", (unsigned)t->gpio);
            snprintf(c->detail, sizeof(c->detail), "Keypad %u", (unsigned)(d + 1u));
            if (dv->online && (t->flags & ZSRC_KEYPAD_KNOWN) != 0u) {
                snprintf(c->state, sizeof(c->state), "%s",
                         (t->flags & ZSRC_KEYPAD_ACTIVE) != 0u ? "open" : "closed");
            }
            c->device_class = ALARM_DEVCLASS_GENERIC;
            c->changed = dv->changed[i];
        }
    }
    return n;
}

/* A keypad that stopped reporting is unheard after ZSRC_KEYPAD_STALE_MS (its zones' grace runs). */
static void kp_tick(void *ctx, uint64_t now)
{
    zsrc_keypad_t *k = ctx;
    for (uint8_t d = 0; d < ZSRC_KEYPAD_DEVICES; d++) {
        if (k->dev[d].online && now - k->dev[d].heard >= ZSRC_KEYPAD_STALE_MS) {
            zsrc_keypad_online(k, (uint8_t)(d + 1u), false, now);
        }
    }
}

static uint64_t kp_next_tick(void *ctx, uint64_t now)
{
    const zsrc_keypad_t *k = ctx;
    uint64_t next = UINT64_MAX;
    for (size_t d = 0; d < ZSRC_KEYPAD_DEVICES; d++) {
        if (k->dev[d].online) {
            const uint64_t due = k->dev[d].heard + ZSRC_KEYPAD_STALE_MS;
            next = due < next ? due : next;
        }
    }
    return next < now ? now : next;
}

static void kp_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    (void)ctx;
    uint8_t device, gpio;
    if (split(a, &device, &gpio)) {
        snprintf(out, len, "IO%u on keypad %u", (unsigned)gpio, (unsigned)device);
    } else {
        snprintf(out, len, "a keypad input");
    }
}

const zone_source_ops_t *zsrc_keypad_ops(void)
{
    static const zone_source_ops_t ops = {
        .kind = ZSRC_KEYPAD,
        /* A wire at the keypad, its level sealed over the link: authenticated and supervised;
         * not offline (it needs the network), not trusted for life safety without the
         * installer's override (ZSRC-008). */
        .caps = ZSRC_CAP_LEARN | ZSRC_CAP_SUPERVISED | ZSRC_CAP_AUTH,
        .name = "keypad",
        .grace_s = ZSRC_KEYPAD_GRACE_S,
        .valid = kp_valid,
        .watch = kp_watch,
        .unwatch = kp_unwatch,
        .learn = kp_learn,
        .candidates = kp_candidates,
        .describe = kp_describe,
        .tick = kp_tick,
        .next_tick = kp_next_tick,
    };
    return &ops;
}
