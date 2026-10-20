/*
 * hc_home.h - the house's Smart Home settings (docs/features/24, BL-220; the mockup's
 * proposals/BL-220-device-rooms.js, approved 2026-10-19): the rooms the panel keeps, the room a
 * person put each device in, the devices that ask for a master code before a change, and whether
 * arranging the screen asks for one. One blob on the alarm panel, the same for every screen: its
 * touchscreen edits it, the device judge (device_service) reads the protection from it, and the
 * panel link sends a keypad each device's room (HCTL-003, HCTL-008, HCTL-013).
 * Header only and portable: the touch UI, the services and the host tests use it.
 *
 * Rooms belong to the panel. A device from a source with areas (Home Assistant's) follows its
 * source: it is in the room linked to its area (a room made from that area, else one of that name)
 * until a person puts it somewhere on the panel ("placed"); from then on the panel's choice wins,
 * and following its source again forgets the choice. hc_home_adopt makes the room an area needs and
 * drops a room made from an area once nothing is in it, unless a person renamed it.
 *
 * The blob is a stored format (components/core/AGENTS.md): one firmware's must read on later ones;
 * test/hc_home_fixture_v1.h is version 1's frozen copy.
 *   byte 0   compat, HC_HOME_COMPAT (1). A reader refuses a blob with a compat it doesn't know (the
 *            defaults apply: no rooms, nothing asks for a code). Raised only when an older reader
 *            would misread the blob.
 *   byte 1   flags: bit 0 arranging asks for a master code; bits 1-7 reserved: written 0, ignored
 *   byte 2   r, the rooms that follow, in the tabs' order: 0..HC_HOME_ROOMS
 *   r rooms  id:    1..255, unique, kept while the room is (0 means no room everywhere)
 *            flags: bit 0 a person renamed it; bits 1-7 reserved: written 0, ignored
 *            nlen:  the name's length, 1..HC_HOME_NAME_MAX; the name, no NUL
 *            slen:  the source area it was made from, 0..HC_HOME_NAME_MAX (0: a person made it);
 *                   the area, no NUL
 *   byte     d, the devices that follow: 0..HC_HOME_DEVICES
 *   d items  flags: bit 0 placed by a person (room says where), bit 1 asks for a master code;
 *                   bits 2-7 reserved: written 0, ignored
 *            room:  a room's id, 0 none (read only while placed)
 *            len:   the ref's length, 1..DEV_REF_LEN - 1; the ref (the registry's key), no NUL
 *   Bytes after the last device are ignored: a later version may append a section there.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "home_control/device_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HC_HOME_COMPAT   1u
#define HC_HOME_ROOMS    24u /* the tabs scroll past what fits */
#define HC_HOME_DEVICES  64u /* the registry's HC_MAX_DEVICES */
#define HC_HOME_NAME_MAX (DEV_AREA_LEN - 1u) /* 47 bytes, as a device's area */
#define HC_HOME_REF_MAX  (DEV_REF_LEN - 1u)

#define HC_HOME_F_ARRANGE_CODE 0x01u /* home flags: arranging asks for a master code */
#define HC_HOME_R_RENAMED      0x01u /* room flags: a person renamed it */
#define HC_HOME_D_PLACED       0x01u /* device flags: a person put it in `room` */
#define HC_HOME_D_PROTECTED    0x02u /* ... it asks for a master code before a change */

/* The longest blob version 1 writes: 8628 bytes. */
#define HC_HOME_BLOB_MAX                                                                           \
    (4u + HC_HOME_ROOMS * (4u + 2u * HC_HOME_NAME_MAX) + HC_HOME_DEVICES * (3u + HC_HOME_REF_MAX))

typedef struct {
    uint8_t id;
    uint8_t flags;
    char name[HC_HOME_NAME_MAX + 1u];
    char src[HC_HOME_NAME_MAX + 1u]; /* the source area it was made from, "" a person made it */
} hc_home_room_t;

typedef struct {
    uint8_t flags;
    uint8_t room;
    char ref[DEV_REF_LEN];
} hc_home_dev_t;

typedef struct {
    uint8_t flags;
    uint8_t nrooms;
    uint8_t ndevs;
    hc_home_room_t rooms[HC_HOME_ROOMS];
    hc_home_dev_t devs[HC_HOME_DEVICES];
} hc_home_t;

static inline void hc_home_clear(hc_home_t *h)
{
    h->flags = 0u;
    h->nrooms = 0u;
    h->ndevs = 0u;
}

/* ---- finding ---- */

/* The room with id, -1 none (id 0 is never a room). */
static inline int hc_home_room_index(const hc_home_t *h, uint8_t id)
{
    for (int i = 0; id != 0u && i < (int)h->nrooms; i++) {
        if (h->rooms[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* Two names are the same room's when they match ignoring ASCII case. */
static inline bool hc_home_same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a;
        char y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
}

static inline int hc_home_room_named(const hc_home_t *h, const char *name)
{
    for (int i = 0; name != NULL && name[0] != '\0' && i < (int)h->nrooms; i++) {
        if (hc_home_same_name(h->rooms[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* The room a source area belongs to: the one made from it, else one of its name; -1 none. */
static inline int hc_home_room_for_area(const hc_home_t *h, const char *area)
{
    if (area == NULL || area[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < (int)h->nrooms; i++) {
        if (strcmp(h->rooms[i].src, area) == 0) {
            return i;
        }
    }
    return hc_home_room_named(h, area);
}

/* The device entry naming ref, -1 none. */
static inline int hc_home_dev_index(const hc_home_t *h, const char *ref)
{
    for (int i = 0; ref != NULL && i < (int)h->ndevs; i++) {
        if (strcmp(h->devs[i].ref, ref) == 0) {
            return i;
        }
    }
    return -1;
}

static inline bool hc_home_placed(const hc_home_t *h, const char *ref)
{
    const int d = hc_home_dev_index(h, ref);
    return d >= 0 && (h->devs[d].flags & HC_HOME_D_PLACED) != 0u;
}

static inline bool hc_home_protected(const hc_home_t *h, const char *ref)
{
    const int d = hc_home_dev_index(h, ref);
    return d >= 0 && (h->devs[d].flags & HC_HOME_D_PROTECTED) != 0u;
}

static inline bool hc_home_arrange_code(const hc_home_t *h)
{
    return (h->flags & HC_HOME_F_ARRANGE_CODE) != 0u;
}

/* The room a device is in: its index in rooms, -1 none. area is its source's ("" none). */
static inline int hc_home_room_of(const hc_home_t *h, const char *ref, const char *area)
{
    const int d = hc_home_dev_index(h, ref);
    if (d >= 0 && (h->devs[d].flags & HC_HOME_D_PLACED) != 0u) {
        return hc_home_room_index(h, h->devs[d].room);
    }
    return hc_home_room_for_area(h, area);
}

/* The name its room goes by, for a screen that knows no rooms (a link keypad, HCTL-013): the
 * room's name; a source area no room was made for yet, as it is; "" none. */
static inline const char *hc_home_room_name(const hc_home_t *h, const char *ref, const char *area)
{
    const int r = hc_home_room_of(h, ref, area);
    if (r >= 0) {
        return h->rooms[r].name;
    }
    if (hc_home_placed(h, ref) || area == NULL) {
        return "";
    }
    return area;
}

/* The order of that name's tab, for sorting: a room's place, rooms first; a source area without
 * a room after them; no room last. */
static inline unsigned hc_home_room_rank(const hc_home_t *h, const char *ref, const char *area)
{
    const int r = hc_home_room_of(h, ref, area);
    if (r >= 0) {
        return (unsigned)r;
    }
    return hc_home_room_name(h, ref, area)[0] != '\0' ? HC_HOME_ROOMS : HC_HOME_ROOMS + 1u;
}

/* strnlen is POSIX, not C11: the length of s, at most max. */
static inline size_t hc_home_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* ---- the blob ---- */

static inline size_t hc_home_put_str(uint8_t *out, size_t at, size_t cap, const char *s, size_t max,
                                     bool empty_ok, bool *ok)
{
    const size_t len = hc_home_len(s, max + 1u);
    if (len > max || (len == 0u && !empty_ok) || at + 1u + len > cap) {
        *ok = false;
        return at;
    }
    out[at++] = (uint8_t)len;
    memcpy(&out[at], s, len);
    return at + len;
}

/* Writes the blob: its length, 0 when cap is too small or something can't be stored. */
static inline size_t hc_home_encode(const hc_home_t *h, uint8_t *out, size_t cap)
{
    if (h->nrooms > HC_HOME_ROOMS || h->ndevs > HC_HOME_DEVICES || cap < 4u) {
        return 0u;
    }
    bool ok = true;
    size_t at = 0;
    out[at++] = (uint8_t)HC_HOME_COMPAT;
    out[at++] = (uint8_t)(h->flags & HC_HOME_F_ARRANGE_CODE);
    out[at++] = h->nrooms;
    for (size_t i = 0; i < h->nrooms && ok; i++) {
        const hc_home_room_t *r = &h->rooms[i];
        if (r->id == 0u || at + 2u > cap) {
            return 0u;
        }
        out[at++] = r->id;
        out[at++] = (uint8_t)(r->flags & HC_HOME_R_RENAMED);
        at = hc_home_put_str(out, at, cap, r->name, HC_HOME_NAME_MAX, false, &ok);
        at = hc_home_put_str(out, at, cap, r->src, HC_HOME_NAME_MAX, true, &ok);
    }
    if (!ok || at + 1u > cap) {
        return 0u;
    }
    out[at++] = h->ndevs;
    for (size_t i = 0; i < h->ndevs && ok; i++) {
        const hc_home_dev_t *d = &h->devs[i];
        if (at + 2u > cap) {
            return 0u;
        }
        out[at++] = (uint8_t)(d->flags & (HC_HOME_D_PLACED | HC_HOME_D_PROTECTED));
        out[at++] = d->room;
        at = hc_home_put_str(out, at, cap, d->ref, HC_HOME_REF_MAX, false, &ok);
    }
    return ok ? at : 0u;
}

static inline bool hc_home_get_str(const uint8_t *b, size_t len, size_t *at, char *out, size_t max,
                                   bool empty_ok)
{
    if (*at + 1u > len) {
        return false;
    }
    const size_t n = b[*at];
    *at += 1u;
    if (n > max || (n == 0u && !empty_ok) || *at + n > len || memchr(&b[*at], 0, n) != NULL) {
        return false;
    }
    memcpy(out, &b[*at], n);
    out[n] = '\0';
    *at += n;
    return true;
}

/* Reads a blob into h. False - h cleared, the defaults apply - when it is short, cut off, from a
 * newer incompatible version or otherwise not one; a room id or a name used twice, and a ref named
 * twice, keep their first. */
static inline bool hc_home_decode(hc_home_t *h, const uint8_t *b, size_t len)
{
    hc_home_clear(h);
    if (b == NULL || len < 4u || b[0] != HC_HOME_COMPAT || b[2] > HC_HOME_ROOMS) {
        return false;
    }
    const uint8_t flags = b[1];
    const size_t nr = b[2];
    size_t at = 3;
    for (size_t i = 0; i < nr; i++) {
        hc_home_room_t r;
        if (at + 2u > len) {
            hc_home_clear(h);
            return false;
        }
        r.id = b[at];
        r.flags = (uint8_t)(b[at + 1u] & HC_HOME_R_RENAMED);
        at += 2u;
        if (r.id == 0u || !hc_home_get_str(b, len, &at, r.name, HC_HOME_NAME_MAX, false) ||
            !hc_home_get_str(b, len, &at, r.src, HC_HOME_NAME_MAX, true)) {
            hc_home_clear(h);
            return false;
        }
        if (hc_home_room_index(h, r.id) >= 0 || hc_home_room_named(h, r.name) >= 0) {
            continue; /* used twice: the first stands */
        }
        h->rooms[h->nrooms++] = r;
    }
    if (at + 1u > len || b[at] > HC_HOME_DEVICES) {
        hc_home_clear(h);
        return false;
    }
    const size_t nd = b[at++];
    for (size_t i = 0; i < nd; i++) {
        hc_home_dev_t d;
        if (at + 2u > len) {
            hc_home_clear(h);
            return false;
        }
        d.flags = (uint8_t)(b[at] & (HC_HOME_D_PLACED | HC_HOME_D_PROTECTED));
        d.room = b[at + 1u];
        at += 2u;
        if (!hc_home_get_str(b, len, &at, d.ref, HC_HOME_REF_MAX, false)) {
            hc_home_clear(h);
            return false;
        }
        if (hc_home_dev_index(h, d.ref) >= 0) {
            continue;
        }
        h->devs[h->ndevs++] = d;
    }
    h->flags = (uint8_t)(flags & HC_HOME_F_ARRANGE_CODE);
    return true;
}

/* ---- edits (the touchscreen's; each returns whether it changed something to store) ---- */

/* A trimmed copy of a name a person typed: no leading or trailing spaces, at most
 * HC_HOME_NAME_MAX bytes cut on a whole UTF-8 character; false when nothing is left. */
static inline bool hc_home_clean_name(const char *in, char out[HC_HOME_NAME_MAX + 1u])
{
    while (*in == ' ') {
        in++;
    }
    size_t n = hc_home_len(in, 4096u);
    while (n > 0u && in[n - 1u] == ' ') {
        n--;
    }
    if (n > HC_HOME_NAME_MAX) {
        n = HC_HOME_NAME_MAX;
        while (n > 0u && ((unsigned char)in[n] & 0xC0u) == 0x80u) {
            n--; /* not inside a character */
        }
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (unsigned char)in[i] < 0x20u ? ' ' : in[i]; /* no line breaks in a tab */
    }
    out[n] = '\0';
    return n > 0u;
}

static inline uint8_t hc_home_new_id(const hc_home_t *h)
{
    uint8_t top = 0u;
    for (size_t i = 0; i < h->nrooms; i++) {
        if (h->rooms[i].id > top) {
            top = h->rooms[i].id;
        }
    }
    if (top < 255u) {
        return (uint8_t)(top + 1u);
    }
    for (unsigned id = 1u; id <= 255u; id++) {
        if (hc_home_room_index(h, (uint8_t)id) < 0) {
            return (uint8_t)id;
        }
    }
    return 0u;
}

/* Why a name can't be a room's (hc_home_add, hc_home_rename). */
typedef enum {
    HC_HOME_NAME_OK = 0,
    HC_HOME_NAME_EMPTY,
    HC_HOME_NAME_TAKEN, /* another room has it, ignoring case */
    HC_HOME_NAME_FULL,  /* HC_HOME_ROOMS rooms already */
} hc_home_name_t;

static inline hc_home_name_t hc_home_name_check(const hc_home_t *h, const char *name, uint8_t self)
{
    char clean[HC_HOME_NAME_MAX + 1u];
    if (!hc_home_clean_name(name, clean)) {
        return HC_HOME_NAME_EMPTY;
    }
    const int other = hc_home_room_named(h, clean);
    if (other >= 0 && h->rooms[other].id != self) {
        return HC_HOME_NAME_TAKEN;
    }
    if (self == 0u && h->nrooms >= HC_HOME_ROOMS) {
        return HC_HOME_NAME_FULL;
    }
    return HC_HOME_NAME_OK;
}

static inline uint8_t hc_home_add_room(hc_home_t *h, const char *name, const char *src)
{
    char clean[HC_HOME_NAME_MAX + 1u];
    if (hc_home_name_check(h, name, 0u) != HC_HOME_NAME_OK || !hc_home_clean_name(name, clean)) {
        return 0u;
    }
    const uint8_t id = hc_home_new_id(h);
    if (id == 0u) {
        return 0u;
    }
    hc_home_room_t *r = &h->rooms[h->nrooms++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    memcpy(r->name, clean, strlen(clean) + 1u);
    if (src != NULL) {
        const size_t n = hc_home_len(src, HC_HOME_NAME_MAX);
        memcpy(r->src, src, n);
        r->src[n] = '\0';
    }
    return id;
}

/* A new room a person made, last of the tabs: its id, 0 when the name can't be one. */
static inline uint8_t hc_home_add(hc_home_t *h, const char *name)
{
    return hc_home_add_room(h, name, "");
}

static inline bool hc_home_rename(hc_home_t *h, uint8_t id, const char *name)
{
    const int r = hc_home_room_index(h, id);
    char clean[HC_HOME_NAME_MAX + 1u];
    if (r < 0 || hc_home_name_check(h, name, id) != HC_HOME_NAME_OK ||
        !hc_home_clean_name(name, clean)) {
        return false;
    }
    if (strcmp(h->rooms[r].name, clean) == 0) {
        return false;
    }
    memcpy(h->rooms[r].name, clean, strlen(clean) + 1u);
    h->rooms[r].flags |= HC_HOME_R_RENAMED;
    return true;
}

/* Moves a room's tab one place earlier (-1) or later (+1). */
static inline bool hc_home_move(hc_home_t *h, uint8_t id, int delta)
{
    const int r = hc_home_room_index(h, id);
    const int to = r + (delta < 0 ? -1 : 1);
    if (r < 0 || to < 0 || to >= (int)h->nrooms) {
        return false;
    }
    const hc_home_room_t t = h->rooms[r];
    h->rooms[r] = h->rooms[to];
    h->rooms[to] = t;
    return true;
}

/* The entry for ref, made when missing (flags 0); NULL when the list is full or ref can't be
 * one. */
static inline hc_home_dev_t *hc_home_dev_entry(hc_home_t *h, const char *ref)
{
    const int d = hc_home_dev_index(h, ref);
    if (d >= 0) {
        return &h->devs[d];
    }
    const size_t len = ref == NULL ? 0u : hc_home_len(ref, DEV_REF_LEN);
    if (len == 0u || len > HC_HOME_REF_MAX || h->ndevs >= HC_HOME_DEVICES) {
        return NULL;
    }
    hc_home_dev_t *e = &h->devs[h->ndevs++];
    memset(e, 0, sizeof(*e));
    memcpy(e->ref, ref, len);
    return e;
}

/* An entry with nothing to keep goes. */
static inline void hc_home_dev_tidy(hc_home_t *h, const char *ref)
{
    const int d = hc_home_dev_index(h, ref);
    if (d >= 0 && h->devs[d].flags == 0u) {
        for (size_t i = (size_t)d; i + 1u < h->ndevs; i++) {
            h->devs[i] = h->devs[i + 1u];
        }
        h->ndevs--;
    }
}

/* A person puts a device in a room (id 0: no room): from now on the panel's choice wins. */
static inline bool hc_home_place(hc_home_t *h, const char *ref, uint8_t id)
{
    if (id != 0u && hc_home_room_index(h, id) < 0) {
        return false;
    }
    hc_home_dev_t *e = hc_home_dev_entry(h, ref);
    if (e == NULL) {
        return false;
    }
    if ((e->flags & HC_HOME_D_PLACED) != 0u && e->room == id) {
        return false;
    }
    e->flags |= HC_HOME_D_PLACED;
    e->room = id;
    return true;
}

/* The device follows its source's area again. */
static inline bool hc_home_follow(hc_home_t *h, const char *ref)
{
    const int d = hc_home_dev_index(h, ref);
    if (d < 0 || (h->devs[d].flags & HC_HOME_D_PLACED) == 0u) {
        return false;
    }
    h->devs[d].flags &= (uint8_t)~HC_HOME_D_PLACED;
    h->devs[d].room = 0u;
    hc_home_dev_tidy(h, ref);
    return true;
}

static inline bool hc_home_protect(hc_home_t *h, const char *ref, bool on)
{
    if (hc_home_protected(h, ref) == on) {
        return false;
    }
    hc_home_dev_t *e = hc_home_dev_entry(h, ref);
    if (e == NULL) {
        return false;
    }
    if (on) {
        e->flags |= HC_HOME_D_PROTECTED;
    } else {
        e->flags &= (uint8_t)~HC_HOME_D_PROTECTED;
        hc_home_dev_tidy(h, ref);
    }
    return true;
}

static inline bool hc_home_set_arrange_code(hc_home_t *h, bool on)
{
    if (hc_home_arrange_code(h) == on) {
        return false;
    }
    if (on) {
        h->flags = (uint8_t)(h->flags | HC_HOME_F_ARRANGE_CODE);
    } else {
        h->flags = (uint8_t)(h->flags & (uint8_t)~HC_HOME_F_ARRANGE_CODE);
    }
    return true;
}

/* What the registry says of one device, for the edits that look at every device. */
typedef struct {
    const char *ref;
    const char *area; /* its source's, "" none */
} hc_home_seen_t;

/* Removes a room: the devices in it - put there, or following its area - stay on the panel in no
 * room, on purpose (the source doesn't put them back). Nothing else changes. */
static inline bool hc_home_remove(hc_home_t *h, uint8_t id, const hc_home_seen_t *seen, size_t n)
{
    const int r = hc_home_room_index(h, id);
    if (r < 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (hc_home_room_of(h, seen[i].ref, seen[i].area) == r) {
            (void)hc_home_place(h, seen[i].ref, 0u);
        }
    }
    for (size_t i = 0; i < h->ndevs; i++) {
        if (h->devs[i].room == id) {
            h->devs[i].room = 0u; /* placed there, not in the registry now */
        }
    }
    for (size_t i = (size_t)r; i + 1u < h->nrooms; i++) {
        h->rooms[i] = h->rooms[i + 1u];
    }
    h->nrooms--;
    return true;
}

/* Keeps the rooms in step with the sources (the touchscreen calls it with each new device list):
 * a device following its source whose area has no room gets one, made from the area and last of
 * the tabs; with drop, a room made from an area that nobody renamed goes once no device is in it
 * (the screen asks for that only when Smart Home is opened, so a source that lists its devices a
 * few at a time can't shuffle the tabs while they are looked at). An empty registry (the source not
 * heard from yet) changes nothing. */
static inline bool hc_home_adopt(hc_home_t *h, const hc_home_seen_t *seen, size_t n, bool drop)
{
    if (n == 0u) {
        return false;
    }
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        const char *area = seen[i].area;
        if (area == NULL || area[0] == '\0' || hc_home_placed(h, seen[i].ref) ||
            hc_home_room_for_area(h, area) >= 0 || h->nrooms >= HC_HOME_ROOMS) {
            continue;
        }
        changed = hc_home_add_room(h, area, area) != 0u || changed;
    }
    for (size_t r = drop ? h->nrooms : 0u; r-- > 0u;) {
        const hc_home_room_t *room = &h->rooms[r];
        if (room->src[0] == '\0' || (room->flags & HC_HOME_R_RENAMED) != 0u) {
            continue;
        }
        bool used = false;
        for (size_t i = 0; i < n && !used; i++) {
            used = hc_home_room_of(h, seen[i].ref, seen[i].area) == (int)r;
        }
        for (size_t i = 0; i < h->ndevs && !used; i++) {
            used = (h->devs[i].flags & HC_HOME_D_PLACED) != 0u && h->devs[i].room == room->id;
        }
        if (!used) {
            for (size_t k = r; k + 1u < h->nrooms; k++) {
                h->rooms[k] = h->rooms[k + 1u];
            }
            h->nrooms--;
            changed = true;
        }
    }
    return changed;
}

/* Forgets the entries of devices no longer in the registry; an empty registry forgets nothing. */
static inline bool hc_home_prune(hc_home_t *h, const hc_home_seen_t *seen, size_t n)
{
    if (n == 0u) {
        return false;
    }
    size_t k = 0;
    for (size_t d = 0; d < h->ndevs; d++) {
        bool known = false;
        for (size_t i = 0; i < n && !known; i++) {
            known = strcmp(seen[i].ref, h->devs[d].ref) == 0;
        }
        if (known) {
            h->devs[k++] = h->devs[d];
        }
    }
    const bool changed = k != h->ndevs;
    h->ndevs = (uint8_t)k;
    return changed;
}

#ifdef __cplusplus
}
#endif
