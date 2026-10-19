/*
 * ui_dev_model.h - the Smart Home screen's model (docs/features/24, BL-045): its layout - which
 * devices are on Favorites, the order of the tiles and each tile's size - kept as one panel setting,
 * the same for everyone (HCTL-003, HCTL-008), a source's state words read into ui_dev_state_t, and
 * the one conversion of a device's parts into the ui_dev_t both screens show - the master's from
 * its registry, a link keypad's from the panel link (docs/features/11, BL-216).
 * Header only and portable: the touch UI (ui_devices.c) and the ESP port use it, the host tests
 * prove it.
 *
 * The layout blob is a stored format (components/core/AGENTS.md): a blob one firmware writes must
 * read on later ones; test/ui_dev_layout_fixture_v1.h is version 1's frozen copy.
 *   byte 0      compat, UI_DEVL_COMPAT (1). A reader refuses a blob with a compat it doesn't know
 *               and the screen falls back to the defaults. Raised only when an older reader would
 *               misread the blob.
 *   byte 1      n, the entries that follow: 0..UI_DEVL_MAX
 *   n entries   flags: bit 0 on Favorites; bits 1-2 the tile's size (0 the kind's, 1 one cell,
 *                      2 two cells, 3 reads as the kind's); bits 3-7 reserved: written 0, ignored
 *                      when read
 *               len:   the ref's length, 1..DEV_REF_LEN - 1
 *               ref:   the device's registry key (an entity_id), len bytes, no NUL
 *   Bytes after the n-th entry are ignored: a later version may append a section there.
 * The entries' order is the tiles' order on every page: Favorites shows the ones flagged, an area
 * page its own devices. A device the layout doesn't name follows them in the registry's order on
 * its area's page, and is on Favorites only while nothing is stored (the defaults: every device on
 * Favorites in the registry's order, each at its kind's size).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "home_control/home_control.h"
#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_DEVL_COMPAT     1u
#define UI_DEVL_MAX        64u /* devices the layout names (the registry's HC_MAX_DEVICES) */
#define UI_DEVL_FAV        0x01u
#define UI_DEVL_SIZE_SHIFT 1u
#define UI_DEVL_SIZE_MASK  0x06u
#define UI_DEVL_REF_MAX    (DEV_REF_LEN - 1u)
/* The longest blob version 1 writes: 6210 bytes. */
#define UI_DEVL_BLOB_MAX (2u + UI_DEVL_MAX * (2u + UI_DEVL_REF_MAX))

typedef enum {
    UI_DEVL_SIZE_KIND = 0, /* the kind's: two cells where it carries a control */
    UI_DEVL_SIZE_SMALL = 1,
    UI_DEVL_SIZE_WIDE = 2,
} ui_devl_size_t;

typedef struct {
    uint8_t flags; /* UI_DEVL_FAV, the size */
    char ref[DEV_REF_LEN];
} ui_devl_entry_t;

typedef struct {
    bool stored; /* false: nothing (readable) is stored, the defaults apply */
    uint8_t n;
    ui_devl_entry_t e[UI_DEVL_MAX];
} ui_devl_t;

static inline void ui_devl_clear(ui_devl_t *l)
{
    l->stored = false;
    l->n = 0;
}

/* The entry naming ref, -1 none. */
static inline int ui_devl_find(const ui_devl_t *l, const char *ref)
{
    for (int i = 0; i < (int)l->n; i++) {
        if (strcmp(l->e[i].ref, ref) == 0) {
            return i;
        }
    }
    return -1;
}

/* Writes the blob: its length, 0 when out (cap bytes) is too small or an entry is unusable. */
static inline size_t ui_devl_encode(const ui_devl_t *l, uint8_t *out, size_t cap)
{
    if (l->n > UI_DEVL_MAX || cap < 2u) {
        return 0;
    }
    size_t at = 0;
    out[at++] = (uint8_t)UI_DEVL_COMPAT;
    out[at++] = l->n;
    for (size_t i = 0; i < l->n; i++) {
        size_t len = 0;
        while (len < sizeof(l->e[i].ref) && l->e[i].ref[len] != '\0') {
            len++;
        }
        if (len == 0u || len > UI_DEVL_REF_MAX || at + 2u + len > cap) {
            return 0;
        }
        out[at++] = (uint8_t)(l->e[i].flags & (UI_DEVL_FAV | UI_DEVL_SIZE_MASK));
        out[at++] = (uint8_t)len;
        memcpy(&out[at], l->e[i].ref, len);
        at += len;
    }
    return at;
}

/* Reads a blob into l. False - l cleared, the defaults apply - when it is short, cut off, from a
 * newer incompatible version or otherwise not a layout; a ref named twice keeps its first place. */
static inline bool ui_devl_decode(ui_devl_t *l, const uint8_t *b, size_t len)
{
    ui_devl_clear(l);
    if (b == NULL || len < 2u || b[0] != UI_DEVL_COMPAT || b[1] > UI_DEVL_MAX) {
        return false;
    }
    const size_t n = b[1];
    size_t at = 2;
    for (size_t i = 0; i < n; i++) {
        if (at + 2u > len) {
            ui_devl_clear(l);
            return false;
        }
        const uint8_t flags = b[at];
        const size_t rlen = b[at + 1u];
        at += 2u;
        if (rlen == 0u || rlen > UI_DEVL_REF_MAX || at + rlen > len ||
            memchr(&b[at], 0, rlen) != NULL) {
            ui_devl_clear(l);
            return false;
        }
        ui_devl_entry_t *e = &l->e[l->n];
        memcpy(e->ref, &b[at], rlen);
        e->ref[rlen] = '\0';
        at += rlen;
        if (ui_devl_find(l, e->ref) >= 0) {
            continue; /* named twice: the first place stands */
        }
        e->flags = (uint8_t)(flags & (UI_DEVL_FAV | UI_DEVL_SIZE_MASK));
        l->n++;
    }
    l->stored = true;
    return true;
}

/* Two cells by its kind: what carries a control on its tile (a dimmable light's brightness, a
 * thermostat's set point, a cover's keys where it has a position, a fan's speeds where it has
 * them: a fan without speeds is one cell, as a switch is). */
static inline bool ui_devl_kind_wide(const ui_dev_t *d)
{
    switch (d->kind) {
    case DEV_LIGHT:
    case DEV_COVER: return (d->caps & UI_DEV_CAP_LEVEL) != 0u;
    case DEV_CLIMATE: return true;
    case DEV_FAN: return (d->caps & UI_DEV_CAP_NO_SPEED) == 0u;
    default: return false;
    }
}

/* The size its tile has: the layout's, else its kind's. */
static inline bool ui_devl_wide(const ui_devl_t *l, const ui_dev_t *d)
{
    const int i = ui_devl_find(l, d->ref);
    const unsigned size = i < 0 ? 0u : ((unsigned)l->e[i].flags & UI_DEVL_SIZE_MASK) >> UI_DEVL_SIZE_SHIFT;
    if (size == UI_DEVL_SIZE_SMALL) {
        return false;
    }
    if (size == UI_DEVL_SIZE_WIDE) {
        return true;
    }
    return ui_devl_kind_wide(d);
}

static inline bool ui_devl_favorite(const ui_devl_t *l, const ui_dev_t *d)
{
    if (!l->stored) {
        return true;
    }
    const int i = ui_devl_find(l, d->ref);
    return i >= 0 && (l->e[i].flags & UI_DEVL_FAV) != 0u;
}

/* The tiles of one page, in order: indexes into devs (n of them, the registry's order). area NULL
 * is Favorites; otherwise the devices whose area is that ("" for those without one). At most max;
 * the count. */
static inline size_t ui_devl_page(const ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *area,
                                  uint8_t *out, size_t max)
{
    size_t k = 0;
    if (area == NULL && !l->stored) {
        for (size_t i = 0; i < n && k < max && i <= UINT8_MAX; i++) {
            out[k++] = (uint8_t)i;
        }
        return k;
    }
    for (size_t e = 0; e < l->n && k < max; e++) {
        if (area == NULL && (l->e[e].flags & UI_DEVL_FAV) == 0u) {
            continue;
        }
        for (size_t i = 0; i < n && i <= UINT8_MAX; i++) {
            if (strcmp(devs[i].ref, l->e[e].ref) == 0) {
                if (area == NULL || strcmp(devs[i].area, area) == 0) {
                    out[k++] = (uint8_t)i;
                }
                break;
            }
        }
    }
    if (area == NULL) {
        return k;
    }
    for (size_t i = 0; i < n && k < max && i <= UINT8_MAX; i++) {
        if (strcmp(devs[i].area, area) == 0 && ui_devl_find(l, devs[i].ref) < 0) {
            out[k++] = (uint8_t)i;
        }
    }
    return k;
}

/* ---- Arrange (HCTL-008): the edits the screen makes to a layout, bounded at UI_DEVL_MAX entries.
 * Each takes the registry (devs, n) so that editing the defaults first writes them out. ---- */

/* Names ref in the layout, appended with flags; true when it is named (already, or now), false
 * when the layout is full or ref can't be stored. */
static inline bool ui_devl_name(ui_devl_t *l, const char *ref, uint8_t flags)
{
    if (ui_devl_find(l, ref) >= 0) {
        return true;
    }
    const size_t len = strlen(ref);
    if (len == 0u || len > UI_DEVL_REF_MAX || l->n >= UI_DEVL_MAX) {
        return false;
    }
    ui_devl_entry_t *e = &l->e[l->n++];
    memset(e, 0, sizeof(*e));
    memcpy(e->ref, ref, len);
    e->flags = (uint8_t)(flags & (UI_DEVL_FAV | UI_DEVL_SIZE_MASK));
    l->stored = true;
    return true;
}

/* Forgets the entries of devices no longer in the registry (devs, n), the others keeping their
 * order. An empty registry (the source not heard from yet) forgets nothing. */
static inline void ui_devl_prune(ui_devl_t *l, const ui_dev_t *devs, size_t n)
{
    if (n == 0u) {
        return;
    }
    size_t k = 0;
    for (size_t e = 0; e < l->n; e++) {
        bool known = false;
        for (size_t i = 0; i < n && !known; i++) {
            known = strcmp(devs[i].ref, l->e[e].ref) == 0;
        }
        if (known) {
            l->e[k++] = l->e[e];
        }
    }
    l->n = (uint8_t)k;
}

/* The defaults written out as a stored layout: every device on Favorites, in the registry's order.
 * A stored layout that is full makes room first, forgetting devices that have left. */
static inline void ui_devl_materialize(ui_devl_t *l, const ui_dev_t *devs, size_t n)
{
    if (l->stored) {
        if (l->n >= UI_DEVL_MAX) {
            ui_devl_prune(l, devs, n);
        }
        return;
    }
    l->n = 0;
    for (size_t i = 0; i < n && i < UI_DEVL_MAX; i++) {
        (void)ui_devl_name(l, devs[i].ref, UI_DEVL_FAV);
    }
    l->stored = true;
}

/* Small or wide (or UI_DEVL_SIZE_KIND, the kind's) for ref's tile. */
static inline bool ui_devl_set_size(ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *ref,
                                    ui_devl_size_t size)
{
    ui_devl_materialize(l, devs, n);
    if (!ui_devl_name(l, ref, 0u)) {
        return false;
    }
    ui_devl_entry_t *e = &l->e[ui_devl_find(l, ref)];
    e->flags = (uint8_t)((e->flags & UI_DEVL_FAV) |
                         (((unsigned)size << UI_DEVL_SIZE_SHIFT) & UI_DEVL_SIZE_MASK));
    return true;
}

/* Takes ref off Favorites; it keeps its place in its area's order. */
static inline bool ui_devl_remove(ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *ref)
{
    ui_devl_materialize(l, devs, n);
    const int i = ui_devl_find(l, ref);
    if (i < 0) {
        return true; /* never on Favorites */
    }
    l->e[i].flags = (uint8_t)(l->e[i].flags & ~UI_DEVL_FAV);
    return true;
}

/* Puts ref on Favorites at the end of the page, at its kind's size. It moves to the end of the
 * whole list, so its area's page shows it last too. */
static inline bool ui_devl_add(ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *ref)
{
    ui_devl_materialize(l, devs, n);
    const int i = ui_devl_find(l, ref);
    if (i >= 0) {
        const ui_devl_entry_t keep = l->e[i];
        for (size_t k = (size_t)i; k + 1u < l->n; k++) {
            l->e[k] = l->e[k + 1u];
        }
        l->e[l->n - 1u] = keep;
        l->e[l->n - 1u].flags = UI_DEVL_FAV;
        return true;
    }
    return ui_devl_name(l, ref, UI_DEVL_FAV);
}

/* Ready to reorder the page area (NULL: Favorites): the defaults written out, and on an area's page
 * every device it shows named, so that its order is the layout's. Writes the page (as
 * ui_devl_page) and ref's place on it to *at; false when a device can't be named or ref isn't on
 * the page. */
static inline bool ui_devl_reorder_page(ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *area,
                                        const char *ref, uint8_t *page, size_t *k, size_t *at)
{
    ui_devl_materialize(l, devs, n);
    if (area != NULL) {
        for (size_t i = 0; i < n && i < UI_DEVL_MAX; i++) { /* an area's order names its devices */
            if (strcmp(devs[i].area, area) == 0 && !ui_devl_name(l, devs[i].ref, 0u)) {
                return false;
            }
        }
    }
    *k = ui_devl_page(l, devs, n, area, page, UI_DEVL_MAX);
    size_t p = 0;
    while (p < *k && strcmp(devs[page[p]].ref, ref) != 0) {
        p++;
    }
    *at = p;
    return p < *k;
}

/* Swaps the entries of the page's places p and p + 1 (and the page with them): the other entries,
 * on other pages, keep their places. */
static inline bool ui_devl_swap_places(ui_devl_t *l, const ui_dev_t *devs, uint8_t *page, size_t p)
{
    const int a = ui_devl_find(l, devs[page[p]].ref);
    const int b = ui_devl_find(l, devs[page[p + 1u]].ref);
    if (a < 0 || b < 0) {
        return false;
    }
    const ui_devl_entry_t swap = l->e[a];
    l->e[a] = l->e[b];
    l->e[b] = swap;
    const uint8_t d = page[p];
    page[p] = page[p + 1u];
    page[p + 1u] = d;
    return true;
}

/* Moves ref one place earlier (delta -1) or later (+1) on the page area (NULL: Favorites); false at
 * either end of the page, or when ref isn't on it. */
static inline bool ui_devl_move(ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *area,
                                const char *ref, int delta)
{
    uint8_t page[UI_DEVL_MAX];
    size_t k = 0;
    size_t p = 0;
    if (!ui_devl_reorder_page(l, devs, n, area, ref, page, &k, &p) || (delta < 0 && p == 0u) ||
        (delta > 0 && p + 1u >= k) || delta == 0) {
        return false;
    }
    return ui_devl_swap_places(l, devs, page, delta < 0 ? p - 1u : p);
}

/* Moves ref to place `to` (0-based) on the page area (NULL: Favorites), the tiles between shifting
 * one place towards where it was: a tile dragged onto another takes that one's place. False when ref
 * isn't on the page, `to` is past its end, or ref is there already (nothing changed). */
static inline bool ui_devl_move_to(ui_devl_t *l, const ui_dev_t *devs, size_t n, const char *area,
                                   const char *ref, size_t to)
{
    uint8_t page[UI_DEVL_MAX];
    size_t k = 0;
    size_t p = 0;
    if (!ui_devl_reorder_page(l, devs, n, area, ref, page, &k, &p) || to >= k || to == p) {
        return false;
    }
    while (p != to) {
        const size_t s = to > p ? p : p - 1u;
        if (!ui_devl_swap_places(l, devs, page, s)) {
            return false;
        }
        p = to > p ? p + 1u : p - 1u;
    }
    return true;
}

/* A source's state words ("on", "locked", "closing", "heat_cool": Home Assistant's, docs/features/24
 * "Actions per domain") as the screen's ui_dev_state_t for a device of this kind; anything else -
 * "unavailable", a scene's time - is UI_DEV_ST_UNKNOWN. */
static inline uint8_t ui_dev_state_parse(uint8_t kind, const char *words)
{
    static const struct {
        const char *w;
        uint8_t st;
    } LOCK[] = {{"locked", UI_DEV_ST_LOCKED},     {"unlocked", UI_DEV_ST_UNLOCKED},
                {"open", UI_DEV_ST_UNLOCKED},     {"jammed", UI_DEV_ST_JAMMED},
                {"locking", UI_DEV_ST_LOCKING},   {"unlocking", UI_DEV_ST_UNLOCKING},
                {"opening", UI_DEV_ST_UNLOCKING}},
      COVER[] = {{"open", UI_DEV_ST_OPEN},
                 {"closed", UI_DEV_ST_CLOSED},
                 {"opening", UI_DEV_ST_OPENING},
                 {"closing", UI_DEV_ST_CLOSING}},
      CLIMATE[] = {{"off", UI_DEV_ST_OFF},       {"heat", UI_DEV_ST_HEAT},
                   {"cool", UI_DEV_ST_COOL},     {"heat_cool", UI_DEV_ST_AUTO},
                   {"auto", UI_DEV_ST_AUTO},     {"dry", UI_DEV_ST_ON},
                   {"fan_only", UI_DEV_ST_ON}},
      SWITCHED[] = {{"on", UI_DEV_ST_ON}, {"off", UI_DEV_ST_OFF}};
    if (words == NULL) {
        return UI_DEV_ST_UNKNOWN;
    }
#define UI_DEV_STATE_FIND(table)                                                                   \
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {                                \
        if (strcmp(words, table[i].w) == 0) {                                                      \
            return table[i].st;                                                                    \
        }                                                                                          \
    }                                                                                              \
    return UI_DEV_ST_UNKNOWN
    switch (kind) {
    case DEV_LOCK: UI_DEV_STATE_FIND(LOCK);
    case DEV_COVER:
    case DEV_VALVE: UI_DEV_STATE_FIND(COVER);
    case DEV_CLIMATE: UI_DEV_STATE_FIND(CLIMATE);
    case DEV_LIGHT:
    case DEV_SWITCH:
    case DEV_FAN:
    case DEV_SCRIPT: UI_DEV_STATE_FIND(SWITCHED);
    default: return UI_DEV_ST_UNKNOWN;
    }
#undef UI_DEV_STATE_FIND
}

/* What a source knows beside its state words (home_control's dev_values_t, HCTL-002) as the
 * screen's fields, so the sheets show what is known (HCTL-004): level -1, modes 0, setpoint and
 * temperature UI_DEV_TEMP_UNKNOWN when not; UI_DEV_CAP_LEVEL added when the level can be set (a
 * brightness slider, a position); UI_DEV_CAP_NO_STOP / NO_SPEED when the source says a cover
 * can't stop or a fan has no speeds (u->kind is set first); since_ms from since. Home
 * Assistant's "heat_cool" is the screen's Auto, as ui_dev_state_parse reads that word; what a
 * thermostat is doing as hvac_action (BL-171): DEV_HVAC_UNKNOWN for any other kind or a number
 * past DEV_HVAC_COUNT. */
static inline void ui_dev_values_fill(ui_dev_t *u, const dev_values_t *v)
{
    u->level = (v->flags & DEV_VAL_LEVEL) != 0u && v->level <= 100u ? (int8_t)v->level : (int8_t)-1;
    if ((v->flags & DEV_VAL_LEVEL_SET) != 0u) {
        u->caps = (uint8_t)(u->caps | UI_DEV_CAP_LEVEL);
    }
    uint8_t modes = 0u;
    modes = (uint8_t)(modes | ((v->modes & DEV_MODE_OFF) != 0u ? UI_DEV_MODE_OFF : 0u));
    modes = (uint8_t)(modes | ((v->modes & DEV_MODE_HEAT) != 0u ? UI_DEV_MODE_HEAT : 0u));
    modes = (uint8_t)(modes | ((v->modes & DEV_MODE_COOL) != 0u ? UI_DEV_MODE_COOL : 0u));
    modes = (uint8_t)(modes | ((v->modes & (DEV_MODE_AUTO | DEV_MODE_HEAT_COOL)) != 0u
                                   ? UI_DEV_MODE_AUTO
                                   : 0u));
    u->modes = modes;
    u->setpoint = (v->flags & DEV_VAL_SETPOINT) != 0u ? v->setpoint : UI_DEV_TEMP_UNKNOWN;
    u->temperature = (v->flags & DEV_VAL_TEMP) != 0u ? v->temp : UI_DEV_TEMP_UNKNOWN;
    if (u->kind == DEV_COVER && !dev_can(v, DEV_CAP_STOP)) {
        u->caps = (uint8_t)(u->caps | UI_DEV_CAP_NO_STOP);
    }
    if (u->kind == DEV_FAN && !dev_can(v, DEV_CAP_SPEED)) {
        u->caps = (uint8_t)(u->caps | UI_DEV_CAP_NO_SPEED);
    }
    u->since_ms = v->since != 0u ? (int64_t)v->since * 1000 : 0;
    u->hvac_action = u->kind == DEV_CLIMATE && v->hvac_action < DEV_HVAC_COUNT
                         ? v->hvac_action
                         : (uint8_t)DEV_HVAC_UNKNOWN;
}

/* ---- one converter for both screens (docs/features/11 "Smart Home and Home Assistant on keypads",
 * BL-216): a device's parts, wherever they come from - the registry's row on the master
 * (ui_dev_parts_hc), the panel link's catalog and device records on a keypad - become the one
 * ui_dev_t the screen reads. ---- */

/* One device's parts. The strings and values are read during ui_dev_from_parts only; NULL reads
 * as "" (values: nothing known). */
typedef struct {
    uint8_t kind;   /* dev_kind_t */
    uint8_t source; /* dsrc_kind_t */
    bool online;    /* false: offline, readable, its controls rest (HCTL-006) */
    bool pending;   /* an action is under way */
    bool failed;    /* the last action failed */
    bool unsure;    /* hc_outcome_unknown: whether the failed action happened isn't known */
    uint8_t action; /* dev_action_t under way, or the last one when it failed */
    const char *ref;
    const char *name;
    const char *area;  /* "" none */
    const char *state; /* the source's words ("on", "locked", "heat_cool") */
    const dev_values_t *values;
} ui_dev_parts_t;

/* src into dst (cap bytes, cap > 0) cut to fit and NUL-terminated, as snprintf("%s") writes it. */
static inline void ui_dev_copy_text(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (src != NULL) {
        while (n + 1u < cap && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}

/* The device as the screen shows it. Its status is the first that applies: OFFLINE, PENDING,
 * FAILED, IDLE (HCTL-006); `action` only while pending or failed - the action whoever started it,
 * this screen, another keypad, the console, an automation - so a tile says "Didn't unlock", not
 * "Didn't respond"; `unsure` only when failed (HCTL-012: "couldn't confirm", BL-171); the values
 * through ui_dev_values_fill (kind first: the fill reads it). Every byte of *u is written, padding
 * included (zeroed). */
static inline void ui_dev_from_parts(ui_dev_t *u, const ui_dev_parts_t *p)
{
    dev_values_t none;
    memset(&none, 0, sizeof(none));
    memset(u, 0, sizeof(*u));
    u->kind = p->kind;
    u->source = p->source;
    u->state = ui_dev_state_parse(p->kind, p->state);
    u->status = (uint8_t)(!p->online   ? UI_DEV_OFFLINE
                          : p->pending ? UI_DEV_PENDING
                          : p->failed  ? UI_DEV_FAILED
                                       : UI_DEV_IDLE);
    u->action = u->status == UI_DEV_PENDING || u->status == UI_DEV_FAILED ? p->action
                                                                           : (uint8_t)ACT_NONE;
    u->unsure = (uint8_t)(u->status == UI_DEV_FAILED && p->unsure ? 1u : 0u);
    ui_dev_values_fill(u, p->values != NULL ? p->values : &none);
    ui_dev_copy_text(u->ref, sizeof(u->ref), p->ref);
    ui_dev_copy_text(u->name, sizeof(u->name), p->name);
    ui_dev_copy_text(u->area, sizeof(u->area), p->area);
}

/* The master's parts: one row of home_control's registry (device_service_list's copy). The
 * strings and values point into *d. */
static inline ui_dev_parts_t ui_dev_parts_hc(const hc_device_t *d)
{
    ui_dev_parts_t p;
    memset(&p, 0, sizeof(p));
    p.kind = d->info.kind;
    p.source = d->info.source;
    p.online = d->info.online;
    p.pending = d->pending != 0u;
    p.failed = d->failed;
    p.unsure = hc_outcome_unknown(d);
    p.action = d->action;
    p.ref = d->info.ref;
    p.name = d->info.name;
    p.area = d->info.area;
    p.state = d->info.state;
    p.values = &d->info.values;
    return p;
}

/* A thermostat's mode key as the screen sends it - dev_act's ACT_SET_MODE value, a ui_dev_state_t
 * (UI_DEV_ST_OFF, _HEAT, _COOL, _AUTO) - as the DEV_MODE_* bit the judge takes
 * (device_service_person_act, a link keypad's DEV_ACT); 0 for any other value, which the judge
 * refuses. */
static inline uint8_t ui_dev_mode_bit(int32_t value)
{
    switch (value) {
    case UI_DEV_ST_OFF: return (uint8_t)DEV_MODE_OFF;
    case UI_DEV_ST_HEAT: return (uint8_t)DEV_MODE_HEAT;
    case UI_DEV_ST_COOL: return (uint8_t)DEV_MODE_COOL;
    case UI_DEV_ST_AUTO: return (uint8_t)DEV_MODE_AUTO;
    default: return 0u;
    }
}

#ifdef __cplusplus
}
#endif
