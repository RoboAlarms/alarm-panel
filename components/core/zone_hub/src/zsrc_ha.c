/*
 * zsrc_ha.c - Home Assistant entities as zones (zsrc_ha.h): the shared catalog for the
 * picker, watched entities mapped onto zones, availability as reachability. Portable;
 * ha_link (or a test) feeds it.
 */
#include <stdio.h>
#include <string.h>

#include "i18n/i18n.h"
#include "zone_hub/zsrc_ha.h"

/* Case-insensitive "needle somewhere in haystack", accented letters too ("SALÓN" finds
 * "Salón", docs/features/37 I18N-009); an empty needle matches. */
static bool ci_contains(const char *haystack, const char *needle)
{
    return i18n_utf8_contains_fold(haystack, needle);
}

zsrc_address_t zsrc_ha_address(const char *entity_id)
{
    zsrc_address_t a;
    memset(&a, 0, sizeof(a));
    a.kind = ZSRC_HA;
    snprintf(a.ref, sizeof(a.ref), "%s", entity_id != NULL ? entity_id : "");
    return a;
}

alarm_device_class_t zsrc_ha_device_class(const char *ha_class)
{
    static const struct {
        const char *ha;
        alarm_device_class_t cls;
    } map[] = {
        {"door", ALARM_DEVCLASS_DOOR},
        {"window", ALARM_DEVCLASS_WINDOW},
        {"garage_door", ALARM_DEVCLASS_GARAGE_DOOR},
        {"motion", ALARM_DEVCLASS_MOTION},
        {"occupancy", ALARM_DEVCLASS_MOTION},
        {"presence", ALARM_DEVCLASS_MOTION},
        {"smoke", ALARM_DEVCLASS_SMOKE},
        {"carbon_monoxide", ALARM_DEVCLASS_CO},
        {"co", ALARM_DEVCLASS_CO},
        {"gas", ALARM_DEVCLASS_GAS},
        {"heat", ALARM_DEVCLASS_HEAT},
        {"moisture", ALARM_DEVCLASS_WATER},
        {"vibration", ALARM_DEVCLASS_VIBRATION},
        {"temperature", ALARM_DEVCLASS_TEMPERATURE}, /* a thermometer: the freeze warning */
    };
    if (ha_class != NULL) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (strcmp(ha_class, map[i].ha) == 0) {
                return map[i].cls;
            }
        }
    }
    return ALARM_DEVCLASS_GENERIC;
}

void zsrc_ha_init(zsrc_ha_t *h, const zsrc_sink_t *sink)
{
    memset(h, 0, sizeof(*h));
    h->sink = *sink;
}

void zsrc_ha_set_watch_changed(zsrc_ha_t *h, void (*fn)(void *ctx), void *ctx)
{
    h->watch_changed = fn;
    h->watch_ctx = ctx;
}

static void notify_watch_changed(zsrc_ha_t *h)
{
    if (h->watch_changed != NULL) {
        h->watch_changed(h->watch_ctx);
    }
}

static zsrc_ha_watch_t *find_watch(zsrc_ha_t *h, const char *ref)
{
    for (size_t i = 0; i < ZSRC_HA_WATCH_MAX; i++) {
        if (h->watch[i].used && strcmp(h->watch[i].ref, ref) == 0) {
            return &h->watch[i];
        }
    }
    return NULL;
}

static zsrc_ha_entity_t *find_entity(zsrc_ha_t *h, const char *id)
{
    for (size_t i = 0; i < h->n_catalog; i++) {
        if (strcmp(h->catalog[i].id, id) == 0) {
            return &h->catalog[i];
        }
    }
    return NULL;
}

/* ---- fed by ha_link ------------------------------------------------------------------------ */

void zsrc_ha_link(zsrc_ha_t *h, bool up, uint64_t now)
{
    if (h->link_up == up) {
        return;
    }
    h->link_up = up;
    if (up) {
        for (size_t i = 0; i < ZSRC_HA_WATCH_MAX; i++) {
            h->watch[i].known = false; /* fresh states follow the new watch list */
        }
    }
    if (h->sink.link != NULL) {
        h->sink.link(h->sink.ctx, ZSRC_HA, up, now);
    }
}

void zsrc_ha_catalog_begin(zsrc_ha_t *h)
{
    h->n_catalog = 0;
}

void zsrc_ha_catalog_add(zsrc_ha_t *h, const zsrc_ha_entity_t *e)
{
    if (e->id[0] == '\0') {
        return;
    }
    zsrc_ha_entity_t *slot = find_entity(h, e->id);
    if (slot == NULL) {
        if (h->n_catalog >= ZSRC_HA_CATALOG_MAX) {
            return; /* the picker shows the first entries; narrow with the search box */
        }
        slot = &h->catalog[h->n_catalog++];
    }
    *slot = *e;
}

void zsrc_ha_state(zsrc_ha_t *h, const char *entity_id, const char *state, bool available,
                   uint64_t now)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return;
    }
    zsrc_ha_entity_t *ent = find_entity(h, entity_id);
    if (ent != NULL) {
        snprintf(ent->state, sizeof(ent->state), "%s", available && state != NULL ? state : "");
    }
    zsrc_ha_watch_t *wt = find_watch(h, entity_id);
    if (wt == NULL) {
        return;
    }
    const zsrc_address_t addr = zsrc_ha_address(entity_id);
    if (!available) {
        if ((wt->avail || !wt->known) && h->sink.reachable != NULL) {
            h->sink.reachable(h->sink.ctx, &addr, false, now);
        }
        wt->avail = false;
        wt->known = true;
        return;
    }
    if ((!wt->avail || !wt->known) && h->sink.reachable != NULL) {
        h->sink.reachable(h->sink.ctx, &addr, true, now);
    }
    wt->avail = true;
    wt->known = true;
    snprintf(wt->state, sizeof(wt->state), "%s", state != NULL ? state : "");
    if (h->sink.input != NULL) {
        h->sink.input(h->sink.ctx, &addr, wt->state, 0, now);
    }
}

size_t zsrc_ha_watch_list(const zsrc_ha_t *h, const char *out[], size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < ZSRC_HA_WATCH_MAX && n < max; i++) {
        if (h->watch[i].used) {
            out[n++] = h->watch[i].ref;
        }
    }
    return n;
}

/* ---- the source interface ------------------------------------------------------------------ */

static bool ha_usable(void *ctx)
{
    return ((zsrc_ha_t *)ctx)->link_up; /* ZSRC-005: only while paired and connected */
}

static bool ha_valid(void *ctx, const zsrc_address_t *a)
{
    (void)ctx;
    return a->kind == ZSRC_HA && a->ref[0] != '\0';
}

static bool ha_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    (void)m;
    zsrc_ha_t *h = ctx;
    zsrc_ha_watch_t *wt = find_watch(h, a->ref);
    if (wt != NULL) {
        /* a mapping change: report what we know so the zone settles at once. Only while the
         * link is up: with Home Assistant away, what it last said is old news and must not
         * count as the sensor answering (ZSRC-015). */
        if (h->link_up && wt->known && wt->avail && h->sink.input != NULL) {
            h->sink.input(h->sink.ctx, a, wt->state, 0, now);
        }
        return true;
    }
    for (size_t i = 0; i < ZSRC_HA_WATCH_MAX; i++) {
        if (!h->watch[i].used) {
            wt = &h->watch[i];
            break;
        }
    }
    if (wt == NULL) {
        return false; /* every slot bound: ZSRC_HA_WATCH_MAX zones on this source */
    }
    memset(wt, 0, sizeof(*wt));
    wt->used = true;
    wt->avail = true;
    snprintf(wt->ref, sizeof(wt->ref), "%s", a->ref);
    const zsrc_ha_entity_t *ent = find_entity(h, a->ref);
    if (h->link_up && ent != NULL && ent->state[0] != '\0' && h->sink.input != NULL) {
        wt->known = true;
        snprintf(wt->state, sizeof(wt->state), "%s", ent->state);
        h->sink.input(h->sink.ctx, a, wt->state, 0, now);
    }
    notify_watch_changed(h);
    return true;
}

static void ha_unwatch(void *ctx, const zsrc_address_t *a)
{
    zsrc_ha_t *h = ctx;
    zsrc_ha_watch_t *wt = find_watch(h, a->ref);
    if (wt != NULL) {
        memset(wt, 0, sizeof(*wt));
        notify_watch_changed(h);
    }
}

static size_t ha_candidates(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                            size_t max)
{
    (void)filter;
    zsrc_ha_t *h = ctx;
    size_t n = 0;
    for (size_t i = 0; i < h->n_catalog && n < max; i++) {
        const zsrc_ha_entity_t *e = &h->catalog[i];
        if (!ci_contains(e->name, query) && !ci_contains(e->id, query) &&
            !ci_contains(e->area, query)) {
            continue;
        }
        zsrc_candidate_t *c = &out[n++];
        memset(c, 0, sizeof(*c));
        c->addr = zsrc_ha_address(e->id);
        /* an entity_id is longer than the name field: show as much of it as fits */
        snprintf(c->name, sizeof(c->name), "%.*s", (int)(sizeof(c->name) - 1u),
                 e->name[0] != '\0' ? e->name : e->id);
        snprintf(c->detail, sizeof(c->detail), "%s", e->area);
        snprintf(c->state, sizeof(c->state), "%s", e->state);
        c->device_class = e->device_class;
    }
    return n;
}

static void ha_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    /* the zone page already says "Home Assistant ·" in front of this (like "Wired · GPIO 4"),
     * so this is just which entity: its name while the catalog has it, its id otherwise */
    zsrc_ha_t *h = ctx;
    const zsrc_ha_entity_t *ent = find_entity(h, a->ref);
    snprintf(out, len, "%s", (ent != NULL && ent->name[0] != '\0') ? ent->name : a->ref);
}

const zone_source_ops_t *zsrc_ha_ops(void)
{
    static const zone_source_ops_t ops = {
        .kind = ZSRC_HA,
        /* the trust table of docs/features/22: searched by name, supervised through the link and
         * HA's own "unavailable", authenticated by the paired mutual TLS, tamper and battery
         * as linked entities; needs Wi-Fi and Home Assistant, and not trusted for life
         * safety by default (ZSRC-008) */
        .caps = ZSRC_CAP_SEARCH | ZSRC_CAP_SUPERVISED | ZSRC_CAP_AUTH | ZSRC_CAP_TAMPER |
                ZSRC_CAP_BATTERY,
        .name = "home_assistant",
        .grace_s = ZSRC_HA_GRACE_S,
        .usable = ha_usable,
        .valid = ha_valid,
        .watch = ha_watch,
        .unwatch = ha_unwatch,
        .candidates = ha_candidates,
        .describe = ha_describe,
    };
    return &ops;
}
