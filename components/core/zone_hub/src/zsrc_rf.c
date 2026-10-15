/*
 * zsrc_rf.c - one-way RF sensors as zones (see zsrc_rf.h).
 */
#include "zone_hub/zsrc_rf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- protocols and addresses ---------------------------------------------------------------- */

typedef struct {
    const char *name;
    uint8_t device_bits;
    uint8_t codes;       /* distinct data values */
    bool battery_bit;    /* every frame carries the battery state */
} proto_t;

/* By ZSRC_RF_PROTO_*; the widths are rf_decode's (the rf suite compares them). */
static const proto_t PROTOS[ZSRC_RF_PROTO_COUNT] = {
    [ZSRC_RF_PROTO_EV1527] = {"EV1527", 20u, 16u, false},
    [ZSRC_RF_PROTO_HT6P20B] = {"HT6P20B", 22u, 4u, false},
    [ZSRC_RF_PROTO_HT12E] = {"HT12E", 8u, 16u, false},
    [ZSRC_RF_PROTO_KEELOQ] = {"KeeLoq", 28u, 16u, true},
    [ZSRC_RF_PROTO_ARCTECH] = {"arctech", 30u, 2u, false},
};

static const proto_t *proto_of(uint8_t proto)
{
    return proto < ZSRC_RF_PROTO_COUNT && PROTOS[proto].name != NULL ? &PROTOS[proto] : NULL;
}

uint8_t zsrc_rf_device_bits(uint8_t proto)
{
    const proto_t *p = proto_of(proto);
    return p != NULL ? p->device_bits : 0u;
}

uint8_t zsrc_rf_codes(uint8_t proto)
{
    const proto_t *p = proto_of(proto);
    return p != NULL ? p->codes : 0u;
}

const char *zsrc_rf_proto_name(uint8_t proto)
{
    const proto_t *p = proto_of(proto);
    return p != NULL ? p->name : "RF";
}

static uint32_t device_mask(const proto_t *p)
{
    return p->device_bits >= 32u ? 0xFFFFFFFFu : (1u << p->device_bits) - 1u;
}

zsrc_address_t zsrc_rf_address(uint8_t proto, uint32_t device, uint8_t data)
{
    const proto_t *p = proto_of(proto);
    const uint32_t dev = p != NULL ? device & device_mask(p) : device & ZSRC_RF_DEVICE_MAX;
    return zsrc_address(ZSRC_RF, (uint8_t)((data & 0xFu) + 1u), ((uint64_t)proto << 32) | dev);
}

bool zsrc_rf_address_parts(const zsrc_address_t *a, uint8_t *proto, uint32_t *device,
                           uint8_t *data)
{
    if (a->kind != ZSRC_RF || a->ref[0] != '\0' || (a->id >> 40) != 0u) {
        return false;
    }
    const uint8_t pn = (uint8_t)(a->id >> 32);
    const proto_t *p = proto_of(pn);
    const uint32_t dev = (uint32_t)(a->id & 0xFFFFFFFFu);
    if (p == NULL || a->input < 1u || a->input > p->codes || dev > device_mask(p)) {
        return false;
    }
    if (proto != NULL) {
        *proto = pn;
    }
    if (device != NULL) {
        *device = dev;
    }
    if (data != NULL) {
        *data = (uint8_t)(a->input - 1u);
    }
    return true;
}

zsrc_rf_packet_t zsrc_rf_ev1527(uint32_t code)
{
    zsrc_rf_packet_t p;
    memset(&p, 0, sizeof(p));
    p.proto = ZSRC_RF_PROTO_EV1527;
    p.bits = 24u;
    p.unit_us = 350u;
    p.code = code & 0xFFFFFFu;
    p.device = (code >> 4) & ZSRC_RF_DEVICE_MAX;
    p.data = (uint8_t)(code & 0xFu);
    return p;
}

void zsrc_rf_code_words(uint8_t proto, uint32_t device, uint8_t data, char *out, size_t len)
{
    const unsigned bits = zsrc_rf_device_bits(proto);
    const int width = bits != 0u ? (int)((bits + 3u) / 4u) : 5;
    snprintf(out, len, "%s %0*lX code %X", zsrc_rf_proto_name(proto), width, (unsigned long)device,
             (unsigned)(data & 0xFu));
}

/* ---- the table ------------------------------------------------------------------------------- */

static zsrc_rf_watch_t *find_watch(zsrc_rf_t *r, uint8_t proto, uint32_t device, uint8_t data)
{
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        zsrc_rf_watch_t *w = &r->watch[i];
        if (w->used && w->proto == proto && w->device == device && w->data == data) {
            return w;
        }
    }
    return NULL;
}

/* An input of w's group (its zone, as the hub says) claims this code of w's transmitter: the
 * code means something of its own to the zone, and restores nothing of w (RF01). Inputs no
 * group was said for are one group: every binding then claims it. */
static bool claimed(const zsrc_rf_t *r, const zsrc_rf_watch_t *w, uint8_t data)
{
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        const zsrc_rf_watch_t *o = &r->watch[i];
        if (o->used && o->proto == w->proto && o->device == w->device && o->data == data &&
            o->group == w->group) {
            return true;
        }
    }
    return false;
}

static void tell(zsrc_rf_t *r, zsrc_rf_watch_t *w, bool active, uint64_t now)
{
    w->active = active;
    if (r->sink.input != NULL) {
        const zsrc_address_t a = zsrc_rf_address(w->proto, w->device, w->data);
        r->sink.input(r->sink.ctx, &a, NULL, active ? 1 : 0, now);
    }
}

static void tell_event(zsrc_rf_t *r, const zsrc_rf_watch_t *w, alarm_zone_event_t ev, uint64_t now)
{
    if (r->sink.event != NULL) {
        const zsrc_address_t a = zsrc_rf_address(w->proto, w->device, w->data);
        r->sink.event(r->sink.ctx, &a, ev, now);
    }
}

static void update_link(zsrc_rf_t *r, uint64_t now)
{
    const bool up = r->receiver && !r->jammed;
    if (up != r->told_up) {
        r->told_up = up;
        if (r->sink.link != NULL) {
            r->sink.link(r->sink.ctx, ZSRC_RF, up, now);
        }
        if (up) {
            /* After a break the hub wants every sensor to speak again before the source's
             * trouble clears (ZSRC-015). One-way sensors can't be asked, so the source speaks
             * for them: what it last knew is what it knows now. */
            for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
                if (r->watch[i].used) {
                    tell(r, &r->watch[i], r->watch[i].active, now);
                }
            }
        }
    }
}

void zsrc_rf_init(zsrc_rf_t *r, const zsrc_sink_t *sink)
{
    memset(r, 0, sizeof(*r));
    r->sink = *sink;
}

void zsrc_rf_receiver(zsrc_rf_t *r, bool present, uint64_t now)
{
    r->receiver = present;
    if (!present) {
        r->jammed = false;
        memset(r->jam_ms, 0, sizeof(r->jam_ms));
        r->jam_at = 0;
        r->jam_carrier = false;
    }
    update_link(r, now);
}

bool zsrc_rf_jammed(const zsrc_rf_t *r)
{
    return r->jammed;
}

void zsrc_rf_noise(zsrc_rf_t *r, int8_t rssi_dbm, uint64_t now)
{
    zsrc_rf_carrier(r, rssi_dbm >= ZSRC_RF_JAM_DBM, now);
}

/* ---- jamming (RF-005) -------------------------------------------------------------------------- */

#define JAM_SLOTS (ZSRC_RF_JAM_WINDOW_MS / ZSRC_RF_JAM_SLOT_MS)

/* The clock moved on to 'now': slots that fell out of the minute are emptied. */
static void jam_roll(zsrc_rf_t *r, uint64_t now)
{
    const uint64_t slot = now / ZSRC_RF_JAM_SLOT_MS;
    if (slot <= r->jam_slot) {
        return;
    }
    const uint64_t gone = slot - r->jam_slot;
    for (uint64_t i = 1; i <= gone && i <= JAM_SLOTS; i++) {
        r->jam_ms[(r->jam_slot + i) % JAM_SLOTS] = 0;
    }
    r->jam_slot = slot;
}

static uint32_t jam_total(const zsrc_rf_t *r)
{
    uint32_t t = 0;
    for (size_t i = 0; i < JAM_SLOTS; i++) {
        t += r->jam_ms[i];
    }
    return t;
}

static void jam_judge(zsrc_rf_t *r, uint64_t now)
{
    const uint32_t t = jam_total(r);
    if (!r->jammed && t >= ZSRC_RF_JAM_HOLD_MS) {
        r->jammed = true;
        update_link(r, now);
    } else if (r->jammed && t < ZSRC_RF_JAM_CLEAR_MS) {
        r->jammed = false;
        update_link(r, now);
    }
}

/* One sample: the time since the last one counts as what the last one heard (samples come every
 * 200 ms from a CC1101, every second from a module; a longer silence counts one slot at most). */
void zsrc_rf_carrier(zsrc_rf_t *r, bool carrier, uint64_t now)
{
    /* A sensor's frames arriving: the carrier is that sensor (-15 dBm a metre from the panel,
     * two seconds an event for a cheap contact), not a jammer. A CC1101's line toggles on noise,
     * so the platform cannot wait for a quiet line to sample between frames; the frames say it.
     * A frame stamped after this sample's clock was read is as recent as can be. */
    if (carrier && r->talk_at != 0u && (now <= r->talk_at || now - r->talk_at < ZSRC_RF_TALK_MS)) {
        carrier = false;
    }
    jam_roll(r, now);
    if (r->jam_at != 0u && now > r->jam_at && r->jam_carrier) {
        uint64_t dt = now - r->jam_at;
        if (dt > ZSRC_RF_JAM_SLOT_MS) {
            dt = ZSRC_RF_JAM_SLOT_MS;
        }
        /* split across the slot boundary it may straddle */
        const uint64_t into = now % ZSRC_RF_JAM_SLOT_MS;
        const uint64_t here = dt < into ? dt : into;
        r->jam_ms[r->jam_slot % JAM_SLOTS] += (uint32_t)here;
        if (dt > here && r->jam_slot > 0u) {
            r->jam_ms[(r->jam_slot - 1u) % JAM_SLOTS] += (uint32_t)(dt - here);
        }
    }
    r->jam_at = now != 0u ? now : 1u;
    r->jam_carrier = carrier;
    jam_judge(r, now);
}

/* ---- learning ------------------------------------------------------------------------------- */

/* A confirmed transmission from a sensor: the learn table notes it, learning or not, so that
 * the sensors already transmitting before learning began can be told apart (RF-010). */
static void remember(zsrc_rf_t *r, uint8_t proto, uint32_t device, uint8_t data, uint64_t now)
{
    zsrc_rf_cand_t *slot = NULL;
    zsrc_rf_cand_t *oldest = NULL;
    for (size_t i = 0; i < ZSRC_RF_CAND_MAX; i++) {
        zsrc_rf_cand_t *c = &r->cand[i];
        if (!c->used) {
            if (slot == NULL) {
                slot = c;
            }
            continue;
        }
        if (c->proto == proto && c->device == device) {
            slot = c;
            break;
        }
        /* Room is made from sensors not heard while learning first, then the oldest. */
        if (oldest == NULL || (c->count == 0u && oldest->count != 0u) ||
            ((c->count == 0u) == (oldest->count == 0u) && c->heard_at < oldest->heard_at)) {
            oldest = c;
        }
    }
    if (slot == NULL) {
        slot = oldest;
    }
    if (!slot->used || slot->proto != proto || slot->device != device) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->proto = proto;
        slot->device = device;
    }
    slot->last_code = data;
    slot->heard_at = now != 0u ? now : 1u;
    if (r->learning) {
        if (slot->count == 0u) {
            slot->first_code = data;
            slot->learn_at = slot->heard_at;
        }
        slot->count++;
        const uint16_t bit = (uint16_t)(1u << (data & 0xFu));
        if ((slot->codes & bit) == 0u && slot->norder < ZSRC_RF_ORDER_MAX) {
            slot->order[slot->norder] = (uint8_t)(data & 0xFu);
            slot->order_n[slot->norder] = 0;
            slot->norder++;
        }
        slot->codes = (uint16_t)(slot->codes | bit);
        for (size_t k = 0; k < slot->norder; k++) {
            if (slot->order[k] == (data & 0xFu)) {
                if (slot->order_n[k] < UINT8_MAX) {
                    slot->order_n[k]++;
                }
                slot->order_s[k] = (uint32_t)(slot->heard_at / 1000u);
            }
        }
    }
}

/* ---- frames ---------------------------------------------------------------------------------- */

static bool same_frame(const zsrc_rf_tx_t *t, const zsrc_rf_packet_t *p)
{
    return t->proto == p->proto && t->device == p->device && t->data == p->data &&
           t->extra == p->extra;
}

/* True when t's transmitter has sent another frame since t's last one, and that frame was real
 * (confirmed): the transmitter moved on, so t is over. A door opened and shut and opened again
 * inside ZSRC_RF_REPEAT_MS sends open, closed, open: the second open is new, not the first one's
 * repeat, and the zone must end open (RF-023, B020). An unconfirmed frame - a copy with a bit
 * flipped - ends nothing (RF-009). */
static bool moved_on(const zsrc_rf_t *r, const zsrc_rf_tx_t *t)
{
    for (size_t i = 0; i < ZSRC_RF_RECENT; i++) {
        const zsrc_rf_tx_t *u = &r->tx[i];
        if (u != t && u->used && u->acted && u->proto == t->proto && u->device == t->device &&
            (u->data != t->data || u->extra != t->extra) &&
            (int32_t)(u->last_n - t->last_n) > 0) { /* later, across the counter's wrap */
            return true;
        }
    }
    return false;
}

/* The transmission this frame repeats, or NULL: the same frame within ZSRC_RF_REPEAT_MS of its
 * last one, its transmitter having sent nothing else real since. Several are followed at once, so
 * a corrupted frame or another transmitter between two repeats does not start the transmission
 * over (RF-009). */
static zsrc_rf_tx_t *repeat_of(zsrc_rf_t *r, const zsrc_rf_packet_t *p, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_RF_RECENT; i++) {
        zsrc_rf_tx_t *t = &r->tx[i];
        if (t->used && same_frame(t, p) && (now <= t->last_at || now - t->last_at < ZSRC_RF_REPEAT_MS) &&
            !moved_on(r, t)) {
            return t;
        }
    }
    return NULL;
}

static zsrc_rf_tx_t *start_tx(zsrc_rf_t *r, const zsrc_rf_packet_t *p, uint64_t now)
{
    zsrc_rf_tx_t *slot = &r->tx[0];
    for (size_t i = 0; i < ZSRC_RF_RECENT; i++) {
        zsrc_rf_tx_t *t = &r->tx[i];
        if (!t->used) {
            slot = t;
            break;
        }
        if (t->last_at < slot->last_at) {
            slot = t; /* the one whose last frame is oldest */
        }
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->proto = p->proto;
    slot->device = p->device;
    slot->data = p->data;
    slot->extra = p->extra;
    slot->last_at = now;
    slot->last_n = r->frames;
    slot->frames = 1;
    return slot;
}

/* RF-016: a rolling code a bound input of this transmitter has had before is a replay. */
static bool replayed(zsrc_rf_t *r, const zsrc_rf_packet_t *p)
{
    if ((p->flags & ZSRC_RF_PKT_ROLLING) == 0u) {
        return false;
    }
    bool seen = false;
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        zsrc_rf_watch_t *w = &r->watch[i];
        if (!w->used || w->proto != p->proto || w->device != p->device) {
            continue;
        }
        for (size_t k = 0; k < w->nhops && k < ZSRC_RF_HOPS; k++) {
            seen = seen || w->hops[k] == p->extra;
        }
    }
    if (seen) {
        return true;
    }
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        zsrc_rf_watch_t *w = &r->watch[i];
        if (w->used && w->proto == p->proto && w->device == p->device) {
            memmove(&w->hops[1], &w->hops[0], (ZSRC_RF_HOPS - 1u) * sizeof(w->hops[0]));
            w->hops[0] = p->extra;
            if (w->nhops < ZSRC_RF_HOPS) {
                w->nhops++;
            }
        }
    }
    return false;
}

/* What a confirmed transmission of its transmitter means to one bound input (RF-014..016). */
static void act_one(zsrc_rf_t *r, zsrc_rf_watch_t *w, const zsrc_rf_packet_t *p, uint64_t now)
{
    const proto_t *pr = proto_of(p->proto);
    const bool low = (p->flags & ZSRC_RF_PKT_BATTERY_LOW) != 0u;
    if (w->data == p->data) {
        w->heard_at = now;
        if (!w->active) {
            tell(r, w, true, now);
        }
    } else if (w->active && w->hold_s == 0u && !claimed(r, w, p->data)) {
        /* The device said something else that means nothing on its own to this zone (no
         * binding of the zone claims it): its closed code, or another zone's code. A code the
         * zone has bound - tamper, low battery, the open code itself - restores nothing but its
         * own hold. */
        tell(r, w, false, now);
        w->closed_by = p->data; /* RF-021: never a tamper or battery code for this zone */
        w->closed_by_known = true;
    }
    if (pr != NULL && pr->battery_bit && low != w->battery_low) {
        w->battery_low = low;
        tell_event(r, w, low ? ALARM_ZEV_LOW_BATTERY : ALARM_ZEV_LOW_BATTERY_RESTORE, now);
    }
    /* RF-015: the sensor is alive, whatever it said (the hub counts it for the zone's own
     * sensor only). */
    if (w->checkin_at == 0u || now < w->checkin_at || now - w->checkin_at >= ZSRC_RF_CHECKIN_MS) {
        w->checkin_at = now != 0u ? now : 1u;
        tell_event(r, w, ALARM_ZEV_CHECKIN, now);
    }
}

/* A confirmed transmission: what it means to the zones bound to its transmitter. */
static void act(zsrc_rf_t *r, const zsrc_rf_packet_t *p, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        zsrc_rf_watch_t *w = &r->watch[i];
        if (w->used && w->proto == p->proto && w->device == p->device) {
            act_one(r, w, p, now);
        }
    }
}

/* A frame of a transmission confirmed before w was made, still arriving: w gets it now, as it
 * would have at the confirmation - the transmission is not counted or learned again (RF02). Its
 * rolling code becomes one w has had, so a recording of it is a replay (RF-016). */
static void act_late(zsrc_rf_t *r, zsrc_rf_watch_t *w, const zsrc_rf_tx_t *t,
                     const zsrc_rf_packet_t *p, uint64_t now)
{
    if ((p->flags & ZSRC_RF_PKT_ROLLING) != 0u) {
        bool seen = false;
        for (size_t k = 0; k < w->nhops && k < ZSRC_RF_HOPS; k++) {
            seen = seen || w->hops[k] == p->extra;
        }
        if (!seen) {
            memmove(&w->hops[1], &w->hops[0], (ZSRC_RF_HOPS - 1u) * sizeof(w->hops[0]));
            w->hops[0] = p->extra;
            if (w->nhops < ZSRC_RF_HOPS) {
                w->nhops++;
            }
        }
    }
    w->since_n = t->acted_n - 1u; /* it has had this one now */
    act_one(r, w, p, now);
}

/* The frame's transmission is real now: counted, learned and acted on - or, a rolling code
 * heard before, refused. */
static void confirm(zsrc_rf_t *r, zsrc_rf_tx_t *t, const zsrc_rf_packet_t *p, uint64_t now)
{
    if (replayed(r, p)) {
        t->replay = true;
        r->replays++;
        return;
    }
    t->acted = true;
    t->acted_n = r->frames;
    r->confirmed++;
    remember(r, p->proto, p->device, p->data, now);
    act(r, p, now);
}

bool zsrc_rf_packet(zsrc_rf_t *r, const zsrc_rf_packet_t *p, uint64_t now)
{
    const proto_t *pr = proto_of(p->proto);
    if (pr == NULL || p->device > device_mask(pr) || p->data >= pr->codes) {
        r->refused++;
        return false;
    }
    r->frames++;
    if (now > r->now) {
        r->now = now;
    }
    /* A transmitter repeats its frame for as long as the button is held: one transmission. */
    zsrc_rf_tx_t *t = repeat_of(r, p, now);
    if (t != NULL) {
        t->frames++;
        if (now > t->last_at) {
            t->last_at = now;
        }
        t->last_n = r->frames;
        if (t->frames == ZSRC_RF_BURST_FRAMES) {
            r->bursts++; /* RF-006: far longer than a finger on a button */
        }
        if (t->frames < ZSRC_RF_BURST_FRAMES) {
            r->talk_at = now != 0u ? now : 1u; /* RF-005: a sensor talking; a stuck one is not */
        }
        if (t->replay) {
            return false;
        }
        zsrc_rf_watch_t *w = find_watch(r, p->proto, p->device, p->data);
        if (w != NULL && t->acted && t->frames < ZSRC_RF_BURST_FRAMES &&
            (int32_t)(w->since_n - t->acted_n) >= 0) {
            /* bound since it was confirmed (RF02); a burst far beyond a finger (a recording sent
             * over and over) gives an input bound during it nothing */
            act_late(r, w, t, p, now);
        }
        if (w != NULL && t->acted) {
            w->heard_at = now; /* the hold counts from the last frame */
            w->frames++;
        }
        if (t->acted || t->frames != ZSRC_RF_CONFIRM_FRAMES) {
            return false;
        }
        /* The frame again: a real transmission, not a corrupted copy or noise (RF-009, RF-014). */
        confirm(r, t, p, now);
        if (w != NULL && t->acted) {
            w->frames += t->frames;
        }
        return t->acted;
    }
    t = start_tx(r, p, now);
    r->transmissions++;
    r->talk_at = now != 0u ? now : 1u;
    bool known = false;
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX && !known; i++) {
        known = r->watch[i].used && r->watch[i].proto == p->proto && r->watch[i].device == p->device;
    }
    if (!known) {
        r->unknown++;
    }
    if ((p->flags & ZSRC_RF_PKT_CHECKED) != 0u) {
        confirm(r, t, p, now); /* a checksum passed: this frame alone is real */
        zsrc_rf_watch_t *w = find_watch(r, p->proto, p->device, p->data);
        if (w != NULL && t->acted) {
            w->frames++;
        }
        return t->acted;
    }
    return false;
}

/* ---- the source ------------------------------------------------------------------------------ */

static bool rf_usable(void *ctx)
{
    const zsrc_rf_t *r = ctx;
    return r->receiver;
}

static bool rf_valid(void *ctx, const zsrc_address_t *a)
{
    (void)ctx;
    return zsrc_rf_address_parts(a, NULL, NULL, NULL);
}

static bool rf_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    zsrc_rf_t *r = ctx;
    uint8_t proto;
    uint32_t device;
    uint8_t data;
    if (!zsrc_rf_address_parts(a, &proto, &device, &data)) {
        return false;
    }
    zsrc_rf_watch_t *w = find_watch(r, proto, device, data);
    if (w != NULL) {
        w->hold_s = m->hold_s; /* a new hold; the state stays */
        return true;
    }
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        if (!r->watch[i].used) {
            w = &r->watch[i];
            break;
        }
    }
    if (w == NULL) {
        return false;
    }
    memset(w, 0, sizeof(*w));
    w->used = true;
    w->proto = proto;
    w->device = device;
    w->data = data;
    w->hold_s = m->hold_s;
    w->since_n = r->frames;
    /* Another input of the same transmitter already knows its rolling codes (RF-016). */
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        const zsrc_rf_watch_t *o = &r->watch[i];
        if (o != w && o->used && o->proto == proto && o->device == device && o->nhops > w->nhops) {
            memcpy(w->hops, o->hops, sizeof(w->hops));
            w->nhops = o->nhops;
        }
    }
    tell(r, w, false, now); /* nothing heard yet: the zone starts quiet, and known */
    return true;
}

static void rf_unwatch(void *ctx, const zsrc_address_t *a)
{
    zsrc_rf_t *r = ctx;
    uint8_t proto;
    uint32_t device;
    uint8_t data;
    if (!zsrc_rf_address_parts(a, &proto, &device, &data)) {
        return;
    }
    zsrc_rf_watch_t *w = find_watch(r, proto, device, data);
    if (w != NULL) {
        memset(w, 0, sizeof(*w));
    }
}

/* The hub's group for a bound input (RF01): its zone's inputs share one. */
static void rf_group(void *ctx, const zsrc_address_t *a, uint32_t group)
{
    zsrc_rf_t *r = ctx;
    uint8_t proto;
    uint32_t device;
    uint8_t data;
    if (!zsrc_rf_address_parts(a, &proto, &device, &data)) {
        return;
    }
    zsrc_rf_watch_t *w = find_watch(r, proto, device, data);
    if (w != NULL) {
        w->group = group;
    }
}

/* Inputs it can still watch (RF31). */
static size_t rf_room(void *ctx)
{
    const zsrc_rf_t *r = ctx;
    size_t n = 0;
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        n += r->watch[i].used ? 0u : 1u;
    }
    return n;
}

static bool rf_learn(void *ctx, bool on, uint64_t now)
{
    zsrc_rf_t *r = ctx;
    r->learning = on;
    if (on) {
        /* What arrives from here on is the sensor under test; what came before is nearby. */
        r->learn_since = now;
        for (size_t i = 0; i < ZSRC_RF_CAND_MAX; i++) {
            zsrc_rf_cand_t *c = &r->cand[i];
            c->count = 0;
            c->codes = 0;
            c->first_code = 0;
            c->learn_at = 0;
            c->norder = 0;
        }
    }
    return true;
}

/* The time now as far as the source knows it: the hub's tick, or a later frame. Frames alone
 * left the newest sensor "just now" for good (B012). */
static uint64_t latest(const zsrc_rf_t *r)
{
    return r->clock > r->now ? r->clock : r->now;
}

static void ago_words(uint64_t now, uint64_t at, char *out, size_t len)
{
    const uint64_t s = at <= now ? (now - at) / 1000u : 0u;
    if (s < 3u) {
        snprintf(out, len, "just now");
    } else if (s < 120u) {
        snprintf(out, len, "%lu s ago", (unsigned long)s);
    } else {
        snprintf(out, len, "%lu min ago", (unsigned long)(s / 60u));
    }
}

/* "Code 5", "Codes 5 and E", "Codes 5, E and A", "Codes 1, 2, 5 and more". */
static void codes_words(uint16_t codes, char *out, size_t len)
{
    char list[4];
    unsigned n = 0;
    unsigned total = 0;
    for (unsigned d = 0; d < ZSRC_RF_CODES; d++) {
        if ((codes & (1u << d)) != 0u) {
            total++;
            if (n < 4u) {
                list[n++] = "0123456789ABCDEF"[d];
            }
        }
    }
    if (total == 1u) {
        snprintf(out, len, "Code %c", list[0]);
    } else if (total == 2u) {
        snprintf(out, len, "Codes %c and %c", list[0], list[1]);
    } else if (total == 3u) {
        snprintf(out, len, "Codes %c, %c and %c", list[0], list[1], list[2]);
    } else if (total == 4u) {
        snprintf(out, len, "Codes %c, %c, %c and %c", list[0], list[1], list[2], list[3]);
    } else if (total > 4u) {
        snprintf(out, len, "Codes %c, %c, %c and more", list[0], list[1], list[2]);
    } else {
        out[0] = '\0';
    }
}

/* The sensor that clearly answered the installer (RF-011): heard ZSRC_RF_SURE_MIN times or
 * more while learning, more often than any other; a tie is nobody's. */
static const zsrc_rf_cand_t *best_of(const zsrc_rf_t *r)
{
    const zsrc_rf_cand_t *best = NULL;
    bool tie = false;
    for (size_t i = 0; i < ZSRC_RF_CAND_MAX; i++) {
        const zsrc_rf_cand_t *c = &r->cand[i];
        if (!c->used || c->count < ZSRC_RF_SURE_MIN) {
            continue;
        }
        if (best == NULL || c->count > best->count) {
            best = c;
            tie = false;
        } else if (c->count == best->count) {
            tie = true;
        }
    }
    return tie ? NULL : best;
}

static bool listed(const zsrc_rf_t *r, const zsrc_rf_cand_t *c)
{
    if (!r->learning || !c->used) {
        return false;
    }
    if (c->count > 0u) {
        return true; /* heard while learning */
    }
    /* nearby: heard not long before learning began */
    return c->heard_at + (uint64_t)ZSRC_RF_NEARBY_S * 1000u > r->learn_since;
}

/* Sensors heard while learning first, newest first; then the nearby ones, newest first. */
static bool before(const zsrc_rf_cand_t *a, const zsrc_rf_cand_t *b)
{
    if ((a->count > 0u) != (b->count > 0u)) {
        return a->count > 0u;
    }
    return a->heard_at > b->heard_at;
}

/* "Sensor A2B3C" (the address as wide as its protocol's); a KeeLoq transmitter is a key fob. */
static void cand_name(const zsrc_rf_cand_t *c, char *out, size_t len)
{
    const unsigned bits = zsrc_rf_device_bits(c->proto);
    const int width = bits != 0u ? (int)((bits + 3u) / 4u) : 5;
    snprintf(out, len, "%s %0*lX", c->proto == ZSRC_RF_PROTO_KEELOQ ? "Key fob" : "Sensor", width,
             (unsigned long)c->device);
}

/* ---- one sensor's codes (RF-021) ------------------------------------------------------------ */

#define CODES_QUERY "codes "

void zsrc_rf_codes_query(const zsrc_address_t *main, char *out, size_t len)
{
    uint8_t proto;
    uint32_t device;
    if (len == 0u) {
        return;
    }
    if (main == NULL || !zsrc_rf_address_parts(main, &proto, &device, NULL)) {
        out[0] = '\0';
        return;
    }
    snprintf(out, len, CODES_QUERY "%u %lX", (unsigned)proto, (unsigned long)device);
}

/* The query's sensor, or false when the query is not one (the picker of a zone's own sensor). */
static bool codes_query_parts(const char *query, uint8_t *proto, uint32_t *device)
{
    if (query == NULL || strncmp(query, CODES_QUERY, sizeof(CODES_QUERY) - 1u) != 0) {
        return false;
    }
    char *end = NULL;
    const unsigned long p = strtoul(query + sizeof(CODES_QUERY) - 1u, &end, 10);
    if (end == NULL || *end != ' ' || proto_of((uint8_t)(p & 0xFFu)) == NULL || p > 0xFFu) {
        return false;
    }
    const char *dev = end + 1;
    const unsigned long d = strtoul(dev, &end, 16);
    if (end == dev || *end != '\0' || d > device_mask(proto_of((uint8_t)p))) {
        return false;
    }
    *proto = (uint8_t)p;
    *device = (uint32_t)d;
    return true;
}

static const char *const ORDINAL[ZSRC_RF_ORDER_MAX] = {"first", "second", "third", "fourth"};

/* The code that ended one of this transmitter's bound inputs: its closed code (RF-021). */
static bool closes(const zsrc_rf_t *r, uint8_t proto, uint32_t device, uint8_t code)
{
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        const zsrc_rf_watch_t *w = &r->watch[i];
        if (w->used && w->proto == proto && w->device == device && w->closed_by_known &&
            w->closed_by == code) {
            return true;
        }
    }
    return false;
}

static size_t codes_of(const zsrc_rf_t *r, uint8_t proto, uint32_t device, zsrc_candidate_t *out,
                       size_t max)
{
    const zsrc_rf_cand_t *c = NULL;
    for (size_t i = 0; i < ZSRC_RF_CAND_MAX && c == NULL; i++) {
        if (r->cand[i].used && r->cand[i].proto == proto && r->cand[i].device == device) {
            c = &r->cand[i];
        }
    }
    const uint64_t now = latest(r);
    size_t n = 0;
    for (size_t k = 0; c != NULL && r->learning && k < c->norder && n < max; k++) {
        const uint8_t code = c->order[k];
        zsrc_candidate_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->addr = zsrc_rf_address(proto, device, code);
        snprintf(o->name, sizeof(o->name), "Code %X", (unsigned)code);
        char times[16];
        if (c->order_n[k] <= 1u) {
            snprintf(times, sizeof(times), "once");
        } else if (c->order_n[k] == 2u) {
            snprintf(times, sizeof(times), "twice");
        } else {
            snprintf(times, sizeof(times), "%u times", (unsigned)c->order_n[k]);
        }
        o->unusable = closes(r, proto, device, code);
        if (o->unusable) {
            snprintf(o->detail, sizeof(o->detail), "Closes the zone, %s", times);
        } else {
            snprintf(o->detail, sizeof(o->detail), "Sent %s, %s", ORDINAL[k], times);
        }
        ago_words(now, (uint64_t)c->order_s[k] * 1000u, o->state, sizeof(o->state));
        o->device_class = ALARM_DEVCLASS_GENERIC;
        o->changed = true;
    }
    return n;
}

static size_t rf_candidates(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                            size_t max)
{
    (void)filter;
    zsrc_rf_t *r = ctx;
    uint8_t proto;
    uint32_t device;
    if (codes_query_parts(query, &proto, &device)) {
        return codes_of(r, proto, device, out, max);
    }
    const zsrc_rf_cand_t *best = best_of(r);
    bool taken[ZSRC_RF_CAND_MAX] = {false};
    size_t n = 0;
    while (n < max) {
        size_t pick = ZSRC_RF_CAND_MAX;
        for (size_t i = 0; i < ZSRC_RF_CAND_MAX; i++) {
            if (!taken[i] && listed(r, &r->cand[i]) &&
                (pick == ZSRC_RF_CAND_MAX || before(&r->cand[i], &r->cand[pick]))) {
                pick = i;
            }
        }
        if (pick == ZSRC_RF_CAND_MAX) {
            break;
        }
        taken[pick] = true;
        const zsrc_rf_cand_t *c = &r->cand[pick];
        const bool heard = c->count > 0u;
        zsrc_candidate_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        /* Its first code while learning is what the installer did first: opened the door. */
        o->addr = zsrc_rf_address(c->proto, c->device, heard ? c->first_code : c->last_code);
        cand_name(c, o->name, sizeof(o->name));
        if (heard) {
            char codes[24]; /* "Codes A, B, C and more" at most: the sentence fits detail */
            codes_words(c->codes, codes, sizeof(codes));
            /* Plain ASCII words: the panel's font has no multiplication sign. */
            if (c->count == 1u) {
                snprintf(o->detail, sizeof(o->detail), "%s, heard once", codes);
            } else {
                snprintf(o->detail, sizeof(o->detail), "%s, heard %lu times", codes,
                         (unsigned long)c->count);
            }
        } else {
            /* the picker puts these under "Already transmitting before you started" */
            codes_words((uint16_t)(1u << (c->last_code & 0xFu)), o->detail, sizeof(o->detail));
        }
        ago_words(latest(r) > c->heard_at ? latest(r) : c->heard_at, c->heard_at, o->state,
                  sizeof(o->state));
        o->device_class = ALARM_DEVCLASS_GENERIC;
        o->changed = heard;
        o->best = c == best;
    }
    return n;
}

static void rf_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    (void)ctx;
    uint8_t proto;
    uint32_t device;
    uint8_t data;
    if (!zsrc_rf_address_parts(a, &proto, &device, &data)) {
        snprintf(out, len, "433 MHz sensor");
        return;
    }
    /* Short: it sits after "433 MHz · " on one row of the zone page; EV1527, by far the most
     * common, goes without its name. */
    if (proto == ZSRC_RF_PROTO_EV1527) {
        snprintf(out, len, "%05lX code %X", (unsigned long)device, (unsigned)data);
    } else {
        zsrc_rf_code_words(proto, device, data, out, len);
    }
}

static void rf_tick(void *ctx, uint64_t now)
{
    zsrc_rf_t *r = ctx;
    if (now > r->clock) {
        r->clock = now; /* the picker's "12 s ago" (latest()) */
    }
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        zsrc_rf_watch_t *w = &r->watch[i];
        if (!w->used || !w->active) {
            continue;
        }
        const uint64_t hold = (w->hold_s != 0u ? w->hold_s : ZSRC_RF_AUTO_HOLD_S) * 1000u;
        /* A frame stamped after this tick's clock was read is fresh, not four billion years
         * old: never subtract a later time from an earlier one. */
        if (now >= w->heard_at && now - w->heard_at >= hold) {
            tell(r, w, false, now);
        }
    }
    if (r->jam_at != 0u) {
        jam_roll(r, now); /* the minute moves on even between samples */
        jam_judge(r, now);
    }
}

static uint64_t rf_next(void *ctx, uint64_t now)
{
    const zsrc_rf_t *r = ctx;
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < ZSRC_RF_WATCH_MAX; i++) {
        const zsrc_rf_watch_t *w = &r->watch[i];
        if (!w->used || !w->active) {
            continue;
        }
        const uint64_t at = w->heard_at + (w->hold_s != 0u ? w->hold_s : ZSRC_RF_AUTO_HOLD_S) * 1000u;
        if (at < next) {
            next = at;
        }
    }
    if (r->jammed) {
        const uint64_t at = (r->jam_slot + 1u) * ZSRC_RF_JAM_SLOT_MS; /* a slot drops out */
        if (at < next) {
            next = at;
        }
    }
    if (r->learning && now + 1000u < next) {
        next = now + 1000u; /* a picker is open: its "12 s ago" moves every second */
    }
    return next <= now ? now : next;
}

static const zone_source_ops_t RF_OPS = {
    .kind = ZSRC_RF,
    /* No AUTH (a fixed code is replayable) and no SUPERVISED (no heartbeat): not trusted for
     * fire, CO or panic zones unless the installer says so (RF-006, ZSRC-008). */
    .caps = ZSRC_CAP_LEARN | ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_TAMPER | ZSRC_CAP_BATTERY,
    .name = "433 MHz sensor",
    .grace_s = ZSRC_RF_GRACE_S,
    .usable = rf_usable,
    .valid = rf_valid,
    .watch = rf_watch,
    .unwatch = rf_unwatch,
    .learn = rf_learn,
    .candidates = rf_candidates,
    .describe = rf_describe,
    .tick = rf_tick,
    .next_tick = rf_next,
    .group = rf_group,
    .room = rf_room,
};

const zone_source_ops_t *zsrc_rf_ops(void)
{
    return &RF_OPS;
}
