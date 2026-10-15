/*
 * zwave_zones.c - Z-Wave devices as zones: zw_net's news to zsrc_zwave, commands decoded, the
 * panel's answers, devices asked for their state (see zwave_zones.h).
 */
#include "zwave_zones/zwave_zones.h"

#include <string.h>

bool zwz_is_tag(uint32_t tag)
{
    return (tag & ZWZ_TAG_MASK) == ZWZ_TAG_BASE;
}

/* ---- a node record as the zone source sees it ------------------------------------------------ */

/* A Z-Wave bit mask (bit n counted from bit 0 of byte 0) as a word. */
static uint32_t mask32(const uint8_t *m, size_t len)
{
    uint32_t v = 0u;
    for (size_t i = 0; i < len && i < 4u; i++) {
        v |= (uint32_t)m[i] << (8u * (unsigned)i);
    }
    return v;
}

static uint8_t security_of(uint8_t classes)
{
    if (classes == ZW_NODE_SECURITY_UNKNOWN) {
        return ZSRC_ZW_SEC_UNKNOWN;
    }
    if (((unsigned)classes & ZW_S2_KEY_ACCESS) != 0u) {
        return ZSRC_ZW_SEC_S2_ACCESS;
    }
    if (((unsigned)classes & ZW_S2_KEY_AUTH) != 0u) {
        return ZSRC_ZW_SEC_S2_AUTH;
    }
    if (((unsigned)classes & ZW_S2_KEY_UNAUTH) != 0u) {
        return ZSRC_ZW_SEC_S2_UNAUTH;
    }
    return ((unsigned)classes & ZW_S2_KEY_S0) != 0u ? ZSRC_ZW_SEC_S0 : ZSRC_ZW_SEC_NONE;
}

void zwz_describe(const zw_node_record_t *rec, zsrc_zw_desc_t *d)
{
    memset(d, 0, sizeof(*d));
    d->node = rec->node_id;
    d->generation = rec->generation;
    d->has_identity = rec->has_identity;
    d->manufacturer = rec->manufacturer_id;
    d->product_type = rec->product_type;
    d->product_id = rec->product_id;
    d->security = security_of(rec->security_classes);
    d->listening = rec->is_listening || rec->is_flirs;
    d->check_in_s = rec->has_wake_up ? rec->wake_up_interval_seconds : 0u;
    if (rec->has_zwave_plus) {
        d->installer_icon = rec->installer_icon;
    }
    for (uint8_t e = 0u; e < ZSRC_ZW_EP_MAX && e < ZW_NODE_ENDPOINTS_MAX; e++) {
        const zw_node_endpoint_t *x = &rec->endpoints[e];
        zsrc_zw_endpoint_desc_t *o = &d->ep[e];
        if (e > 0u && !x->used) {
            o->inputs = ZSRC_ZW_INPUTS_NOT_READY;
            continue;
        }
        if (e > 0u) {
            d->endpoint_count = e;
        }
        o->inputs = (uint8_t)zw_iv_inputs(rec, e);
        for (size_t n = 0; n < x->notification_count && n < ZSRC_ZW_TYPES_MAX; n++) {
            o->notif[n].type = x->notifications[n].notification_type;
            o->notif[n].events =
                mask32(x->notifications[n].event_mask, x->notifications[n].event_mask_len);
            o->notif_count = (uint8_t)(n + 1u);
        }
        o->binary = zw_iv_endpoint_has_cc(rec, e, ZW_CC_BINARY_SENSOR);
        o->binary_types = x->has_binary_sensor_mask
                              ? mask32(x->binary_sensor_mask, x->binary_sensor_mask_len)
                              : 0u;
        /* Alarm Sensor's types (BL-085): the source's field holds types 0..15 - smoke, CO and
         * heat are 1, 2 and 4 */
        o->alarm_types = x->has_alarm_sensor_mask
                             ? (uint16_t)(mask32(x->alarm_sensor_mask, x->alarm_sensor_mask_len) &
                                          0xFFFFu)
                             : 0u;
        /* Multilevel Sensor before version 5 cannot say which types it has: offered */
        o->air_temperature = zw_iv_endpoint_has_cc(rec, e, ZW_CC_MULTILEVEL_SENSOR) &&
                             (!x->has_multilevel_mask ||
                              zw_cc_multilevel_sensor_type_supported(
                                  x->multilevel_mask, x->multilevel_mask_len,
                                  ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE));
        o->basic = zw_iv_endpoint_has_cc(rec, e, ZW_CC_BASIC);
    }
}

static const zw_node_record_t *record(const zwz_t *b, uint16_t node)
{
    return zw_node_find_const(zw_net_nodes(b->net), node);
}

/* The zone source hears what the node cache says of a device now. */
static void feed(zwz_t *b, uint16_t node)
{
    const zw_node_record_t *rec = record(b, node);
    if (rec == NULL || rec->generation == 0u) {
        return;
    }
    zsrc_zw_desc_t d;
    zwz_describe(rec, &d);
    (void)zsrc_zw_device(b->src, &d, b->now);
}

/* ---- the outbox: one Get out per device, answers apart and at once --------------------------- */

/* A Get of this device is out (held in zw_net's wake-up queue for a sleeper until it wakes, or
 * one that got no ACK and waits there for the next wake-up): its next Get waits. Answers never
 * do (network.md section 7, "the bridge's outbox"). */
static bool node_busy(const zwz_t *b, uint16_t node)
{
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        if (b->out[i].used && b->out[i].refresh && b->out[i].node == node && b->out[i].out) {
            return true;
        }
    }
    return false;
}

static bool node_waiting(const zwz_t *b, uint16_t node)
{
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        if (b->out[i].used && b->out[i].node == node) {
            return true;
        }
    }
    return false;
}

/* Gets take at most ZWZ_GET_SLOTS of the outbox (one per device: node_waiting), so answers
 * always find ZWZ_REPLY_SLOTS free; an answer may take any free slot. */
static bool enqueue(zwz_t *b, uint16_t node, zw_sec_class_t cls, const uint8_t *cmd, size_t len,
                    bool refresh)
{
    zwz_out_t *o = NULL;
    size_t gets = 0u;
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        if (b->out[i].used) {
            gets += b->out[i].refresh ? 1u : 0u;
        } else if (o == NULL) {
            o = &b->out[i];
        }
    }
    if (refresh && gets >= ZWZ_GET_SLOTS) {
        o = NULL;
    }
    if (len == 0u || len > ZWZ_CMD_MAX || (o == NULL && !refresh)) {
        b->stats.dropped++; /* a Get without room is not dropped: it is asked at a later poll */
        return false;
    }
    if (o == NULL) {
        return false;
    }
    memset(o, 0, sizeof(*o));
    o->used = true;
    o->refresh = refresh;
    o->node = node;
    o->cls = (uint8_t)cls;
    o->len = (uint8_t)len;
    b->seq++;
    o->order = b->seq;
    o->tag = ZWZ_TAG_BASE | (b->seq & 0x00FFFFFFu);
    memcpy(o->cmd, cmd, len);
    return true;
}

/* A reply goes back the way its question came: the same class, the end points swapped
 * (command-classes.md §1). It goes at the next poll, whatever Get of its device is out; one that
 * finds no room is counted (stats.replies_lost), never lost silently. */
static void reply(zwz_t *b, const zw_s2_rx_t *rx, const uint8_t *cmd, size_t len)
{
    uint8_t mc[ZWZ_CMD_MAX];
    if (rx->multi_channel) {
        len = zw_cc_wrap_multi_channel(mc, sizeof(mc), rx->destination_endpoint,
                                       rx->source_endpoint, false, cmd, len);
        cmd = mc;
    }
    if (!enqueue(b, rx->node, rx->security, cmd, len, false)) {
        b->stats.replies_lost++;
    }
}

/* The oldest answer not yet sent, of any device (NULL: none). */
static zwz_out_t *next_reply(zwz_t *b)
{
    zwz_out_t *best = NULL;
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        zwz_out_t *o = &b->out[i];
        if (o->used && !o->out && !o->refresh && (best == NULL || o->order < best->order)) {
            best = o;
        }
    }
    return best;
}

/* A device's oldest Get not yet sent. */
static zwz_out_t *next_for(zwz_t *b, uint16_t node)
{
    zwz_out_t *best = NULL;
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        zwz_out_t *o = &b->out[i];
        if (!o->used || o->out || !o->refresh || o->node != node) {
            continue;
        }
        if (best == NULL || o->order < best->order) {
            best = o;
        }
    }
    return best;
}

static void drop_node(zwz_t *b, uint16_t node)
{
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        if (b->out[i].used && b->out[i].node == node) {
            memset(&b->out[i], 0, sizeof(b->out[i])); /* zw_net forgot its queue with it */
        }
    }
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (b->refresh[i].node == node) {
            memset(&b->refresh[i], 0, sizeof(b->refresh[i]));
        }
        if (b->unknown[i].node == node) {
            memset(&b->unknown[i], 0, sizeof(b->unknown[i])); /* a reused node ID starts afresh */
        }
    }
}

/* ---- unknown commands (command-classes.md §2: logged once per node and CC) -------------------- */

static zwz_unknown_t *unknown_of(zwz_t *b, uint16_t node)
{
    zwz_unknown_t *free_e = NULL;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (b->unknown[i].node == node) {
            return &b->unknown[i];
        }
        if (free_e == NULL && b->unknown[i].node == 0u) {
            free_e = &b->unknown[i];
        }
    }
    if (free_e != NULL && node != 0u) {
        free_e->node = node;
    }
    return free_e;
}

/* command-classes.md §2's CC-identifiers table: every CC the panel has first-class awareness of,
 * either because it decodes the device's reports (Notification, Binary Sensor, ...) or because it
 * is the panel's own side of an exchange it starts (Version Get, Manufacturer Specific Get,
 * Association Get, a quirk's Configuration Set, a Supervision Get it sent...). A reply to one of
 * the panel's own questions reaches here exactly like an unsolicited report - `zw_iv`/`zw_net`
 * match it to its question elsewhere - so it must not be mistaken for "anything else". */
static const uint8_t KNOWN_CCS[] = {
    0x00u, 0x20u, 0x30u, 0x31u, 0x56u, 0x59u, 0x5Au, 0x5Bu, 0x5Eu, 0x60u, 0x6Cu, 0x70u,
    0x71u, 0x72u, 0x80u, 0x82u, 0x84u, 0x85u, 0x86u, 0x87u, 0x8Au, 0x8Eu, 0x8Fu, 0x98u,
    0x9Cu, 0x9Fu,
};

static bool known_cc(uint8_t cc)
{
    for (size_t i = 0; i < sizeof(KNOWN_CCS) / sizeof(KNOWN_CCS[0]); i++) {
        if (KNOWN_CCS[i] == cc) {
            return true;
        }
    }
    return false;
}

/* A CC from a node that is neither one of ours (above) nor decoded into a report: counted the
 * first time, not again - a chatty, unsupported device must not flood whatever logs this later. */
static void note_unknown_cc(zwz_t *b, uint16_t node, uint8_t cc)
{
    if (known_cc(cc)) {
        return;
    }
    zwz_unknown_t *e = unknown_of(b, node);
    if (e == NULL) {
        return; /* the table is full: not counted, but nothing crashes either */
    }
    for (size_t i = 0; i < e->count && i < ZWZ_UNKNOWN_CC_MAX; i++) {
        if (e->cc[i] == cc) {
            return; /* already counted for this node */
        }
    }
    if (e->count < ZWZ_UNKNOWN_CC_MAX) {
        e->cc[e->count++] = cc;
    }
    b->stats.unknown++;
}

/* ---- proof of life (network.md §8) ----------------------------------------------------------- */

/* A device's entry (node 0: a free one), or NULL. */
static zwz_heard_t *heard_of(zwz_t *b, uint16_t node)
{
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (b->heard[i].node == node) {
            return &b->heard[i];
        }
    }
    return NULL;
}

static bool in_health(const zw_health_t *h, uint16_t node)
{
    for (size_t i = 0; i < h->node_count && i < ZW_HEALTH_NODES_MAX; i++) {
        if (h->nodes[i].node_id == node) {
            return true;
        }
    }
    return false;
}

/* A received command: zw_health heard it too, and it went to the zone source already. */
static void note_heard(zwz_t *b, uint16_t node, uint64_t at)
{
    zwz_heard_t *e = node != 0u ? heard_of(b, node) : NULL;
    if (e != NULL && at > e->at) {
        e->at = at;
    }
}

/* zw_health heard from a device since the zone source last did: an ACK of a listening device (a
 * ping's too - how a quiet one checks in), or a command delivered to a sleeping one. The zone
 * source takes it as a frame: the device is alive. After a network check zw_health's clocks are
 * set anew (no proof of anything): they are taken as they are. */
static void pass_heard(zwz_t *b, uint64_t now)
{
    const zw_health_t *h = zw_net_health(b->net);
    for (size_t k = 0; k < ZSRC_ZW_DEV_MAX; k++) {
        if (b->heard[k].node != 0u && !in_health(h, b->heard[k].node)) {
            memset(&b->heard[k], 0, sizeof(b->heard[k])); /* removed, gone, another network's */
        }
    }
    for (size_t i = 0; i < h->node_count && i < ZW_HEALTH_NODES_MAX; i++) {
        const uint16_t node = h->nodes[i].node_id;
        const uint64_t at = h->nodes[i].last_heard_ms;
        zwz_heard_t *e = node != 0u ? heard_of(b, node) : NULL;
        if (e == NULL && node != 0u) {
            e = heard_of(b, 0u); /* room for each: zw_health has at most as many devices */
            if (e != NULL) {
                e->node = node;
                e->at = 0u;
            }
        }
        if (e == NULL || at <= e->at) {
            continue;
        }
        e->at = at;
        if (!b->rebase) {
            zsrc_zw_report_t r;
            memset(&r, 0, sizeof(r));
            r.kind = ZSRC_ZW_RPT_FRAME;
            zsrc_zw_report(b->src, node, &r, now);
            b->stats.heard++;
        }
    }
    b->rebase = false;
}

/* Whether pass_heard() has something to pass on. */
static bool heard_waiting(const zwz_t *b)
{
    const zw_health_t *h = zw_net_health(b->net);
    for (size_t i = 0; i < h->node_count && i < ZW_HEALTH_NODES_MAX; i++) {
        const uint16_t node = h->nodes[i].node_id;
        bool seen = false;
        for (size_t k = 0; k < ZSRC_ZW_DEV_MAX && !seen; k++) {
            seen = b->heard[k].node == node && h->nodes[i].last_heard_ms <= b->heard[k].at;
        }
        if (node != 0u && !seen) {
            return true;
        }
    }
    return false;
}

/* ---- asking a device for its state (zones.md §3 "Fresh state") ------------------------------- */

static void want_refresh(zwz_t *b, uint16_t node, uint8_t endpoints)
{
    zwz_refresh_t *f = NULL;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (b->refresh[i].node == node) {
            f = &b->refresh[i];
            break;
        }
        if (f == NULL && b->refresh[i].node == 0u) {
            f = &b->refresh[i];
        }
    }
    if (f == NULL) {
        return;
    }
    if (f->node != node) {
        memset(f, 0, sizeof(*f));
        f->node = node;
    }
    f->endpoints = (uint8_t)((unsigned)f->endpoints | endpoints);
    f->next = 0u; /* from the start: asking twice is harmless */
}

static uint8_t version_of(const zw_node_record_t *rec, uint8_t cc)
{
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == cc) {
            return rec->ccs[i].version;
        }
    }
    return 0u;
}

/* The index-th Get of a device's refresh (its end points' notification types, Binary Sensor
 * types, Alarm Sensor types, air temperature, then its battery), with its CC; 0 past the last. */
static size_t nth_get(const zw_node_record_t *rec, uint8_t endpoints, unsigned index, uint8_t *out,
                      size_t cap, uint8_t *endpoint, uint8_t *cc)
{
    const uint8_t v_notif = version_of(rec, ZW_CC_NOTIFICATION);
    const uint8_t v_binary = version_of(rec, ZW_CC_BINARY_SENSOR);
    const uint8_t v_ml = version_of(rec, ZW_CC_MULTILEVEL_SENSOR);
    unsigned k = 0u;
    for (uint8_t e = 0u; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        const zw_node_endpoint_t *x = &rec->endpoints[e];
        if (((unsigned)endpoints & (1u << e)) == 0u || (e > 0u && !x->used)) {
            continue;
        }
        *endpoint = e;
        for (size_t t = 0; v_notif >= 2u && t < x->notification_count; t++) {
            if (k++ == index) {
                const zw_cc_notification_get_t g = {0u, x->notifications[t].notification_type,
                                                    v_notif >= 3u, 0u};
                *cc = ZW_CC_NOTIFICATION;
                return zw_cc_notification_get_build(out, cap, &g);
            }
        }
        if (zw_iv_endpoint_has_cc(rec, e, ZW_CC_BINARY_SENSOR)) {
            const uint32_t types = x->has_binary_sensor_mask && v_binary >= 2u
                                       ? mask32(x->binary_sensor_mask, x->binary_sensor_mask_len)
                                       : 0u;
            for (uint8_t t = 1u; t < 32u; t++) {
                if ((types & (1u << t)) != 0u && k++ == index) {
                    const zw_cc_binary_sensor_get_t g = {true, t};
                    *cc = ZW_CC_BINARY_SENSOR;
                    return zw_cc_binary_sensor_get_build(out, cap, &g);
                }
            }
            if (types == 0u && k++ == index) {
                const zw_cc_binary_sensor_get_t g = {false, 0u};
                *cc = ZW_CC_BINARY_SENSOR;
                return zw_cc_binary_sensor_get_build(out, cap, &g);
            }
        }
        if (zw_iv_endpoint_has_cc(rec, e, ZW_CC_ALARM_SENSOR) && x->has_alarm_sensor_mask) {
            /* BL-130: a Get per type its Supported Report listed, those the source holds (0..15,
             * as zwz_describe). Unlike Binary Sensor v1 there is no untyped Get to fall back on:
             * every Alarm Sensor Get names a type, and an end point with no types known offers
             * no Alarm Sensor input - nothing to ask. */
            const uint32_t types = mask32(x->alarm_sensor_mask, x->alarm_sensor_mask_len) & 0xFFFFu;
            for (uint8_t t = 0u; t < 16u; t++) {
                if ((types & (1u << t)) != 0u && k++ == index) {
                    const zw_cc_alarm_sensor_get_t g = {t};
                    *cc = ZW_CC_ALARM_SENSOR;
                    return zw_cc_alarm_sensor_get_build(out, cap, &g);
                }
            }
        }
        if (zw_iv_endpoint_has_cc(rec, e, ZW_CC_MULTILEVEL_SENSOR) &&
            (!x->has_multilevel_mask ||
             zw_cc_multilevel_sensor_type_supported(x->multilevel_mask, x->multilevel_mask_len,
                                                    ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE)) &&
            k++ == index) {
            const bool typed = v_ml >= 5u && x->has_multilevel_mask;
            const zw_cc_multilevel_sensor_get_t g = {typed,
                                                     ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE,
                                                     ZW_MULTILEVEL_SENSOR_SCALE_CELSIUS};
            *cc = ZW_CC_MULTILEVEL_SENSOR;
            return zw_cc_multilevel_sensor_get_build(out, cap, &g);
        }
    }
    if (((unsigned)endpoints & 1u) != 0u && zw_iv_has_cc(rec, ZW_CC_BATTERY) && k == index) {
        *endpoint = 0u;
        *cc = ZW_CC_BATTERY;
        return zw_cc_battery_get_build(out, cap);
    }
    return 0u;
}

/* A device with nothing waiting gets its refresh's next Get. */
static void next_get(zwz_t *b, zwz_refresh_t *f)
{
    const zw_node_record_t *rec = record(b, f->node);
    uint8_t get[16];
    uint8_t ep = 0u;
    uint8_t cc = 0u;
    const size_t n = rec != NULL ? nth_get(rec, f->endpoints, f->next, get, sizeof(get), &ep, &cc)
                                 : 0u;
    if (n == 0u) {
        memset(f, 0, sizeof(*f)); /* all asked */
        return;
    }
    uint8_t mc[24];
    const uint8_t *cmd = get;
    size_t len = n;
    if (ep != 0u) {
        len = zw_cc_wrap_multi_channel(mc, sizeof(mc), 0u, ep, false, get, n);
        cmd = mc;
    }
    /* security.md §8: an end point of an S2 node is reached inside the node's security, whatever
     * the CC - the class of the Multi Channel layer; the device itself by its CC */
    const zw_sec_class_t cls = zw_s2_class_for_endpoint(zw_net_s2(b->net), f->node, ep, cc);
    if (enqueue(b, f->node, cls, cmd, len, true)) {
        f->next++; /* only once it is queued: one without room is asked at a later poll */
        b->stats.gets++;
    } else {
        b->get_retry_at = b->now + ZWZ_RETRY_MS; /* the Gets wait; the answers do not */
    }
}

/* ---- the daily battery re-read (network.md section 7) ---------------------------------------- */

static zwz_battery_t *battery_slot(zwz_t *b, uint16_t node, uint64_t now)
{
    zwz_battery_t *free_slot = NULL;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (b->battery[i].node == node) {
            return &b->battery[i];
        }
        if (free_slot == NULL && b->battery[i].node == 0u) {
            free_slot = &b->battery[i];
        }
    }
    if (free_slot != NULL) {
        free_slot->node = node;
        free_slot->at = now; /* first seen: its day starts now */
    }
    return free_slot;
}

/* The devices that report a battery level at all: the Battery CC at the root. */
static bool has_battery(const zwz_t *b, const zsrc_zw_device_t *d)
{
    const zw_node_record_t *rec = d->used && !d->gone ? record(b, d->node) : NULL;
    return rec != NULL && rec->generation != 0u && zw_iv_has_cc(rec, ZW_CC_BATTERY);
}

/* A Battery Get for each device whose level is a day old. A sleeper's waits in zw_net's queue
 * for its wake-up; one that listens gets it at once. At most one a day per device, and none
 * while a command for it waits in the outbox. */
static void battery_reread(zwz_t *b, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        const zsrc_zw_device_t *d = &b->src->dev[i];
        if (!has_battery(b, d)) {
            continue;
        }
        zwz_battery_t *e = battery_slot(b, d->node, now);
        if (e == NULL || now < e->at || now - e->at < ZWZ_BATTERY_REREAD_MS ||
            node_waiting(b, d->node)) {
            continue;
        }
        uint8_t get[8];
        const size_t n = zw_cc_battery_get_build(get, sizeof(get));
        const zw_sec_class_t cls = zw_s2_class_for_endpoint(zw_net_s2(b->net), d->node, 0u,
                                                            ZW_CC_BATTERY);
        if (enqueue(b, d->node, cls, get, n, true)) {
            b->stats.gets++;
            e->at = now;
        } else {
            b->get_retry_at = now + ZWZ_RETRY_MS;
        }
    }
}

/* When the next re-read falls due (UINT64_MAX: none). A device with a command waiting in the
 * outbox has none now (battery_reread skips it): a sleeper that has not woken for a day - one that
 * wakes only by hand (interval 0), or stopped checking in - would otherwise be due at every poll,
 * a busy loop; its next is counted again once its command leaves. */
static uint64_t battery_due(const zwz_t *b)
{
    uint64_t due = UINT64_MAX;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        const zsrc_zw_device_t *d = &b->src->dev[i];
        if (!has_battery(b, d) || node_waiting(b, d->node)) {
            continue;
        }
        uint64_t at = b->now; /* not yet seen: its day starts at the next poll */
        for (size_t k = 0; k < ZSRC_ZW_DEV_MAX; k++) {
            if (b->battery[k].node == d->node) {
                at = b->battery[k].at;
                break;
            }
        }
        const uint64_t t = at + ZWZ_BATTERY_REREAD_MS;
        if (t < due) {
            due = t;
        }
    }
    return due;
}

/* ---- life cycle ------------------------------------------------------------------------------ */

void zwz_init(zwz_t *b, zw_net_t *net, zsrc_zw_t *src, const zw_cc_panel_identity_t *identity)
{
    memset(b, 0, sizeof(*b));
    b->net = net;
    b->src = src;
    b->answers = identity != NULL;
    if (identity != NULL) {
        b->identity = *identity;
    }
}

void zwz_link_up(zwz_t *b, const zw_module_facts_t *facts, uint64_t now)
{
    b->now = now;
    if (facts != NULL) {
        b->identity.protocol_major = facts->protocol_major; /* what Version Get is answered */
        b->identity.protocol_minor = facts->protocol_minor;
    }
}

void zwz_link_down(zwz_t *b, uint64_t now)
{
    b->now = now;
    zsrc_zw_link(b->src, false, now); /* until the network is checked again (ZSRC-015) */
    /* The source's view of the air follows zw_net's: a real link down resets the detector first
     * (no event), a module restart keeps it - the link that comes back is still jammed */
    zsrc_zw_jammed(b->src, zw_net_jammed(b->net), now);
}

void zwz_event(zwz_t *b, const zw_net_event_t *ev, uint64_t now)
{
    b->now = now;
    switch (ev->kind) {
    case ZW_NET_EV_NETWORK: {
        /* network.md §2: checked. Ours - its devices; replaced - none of the old ones (their
         * zones CHECK, zones.md §1); another system's - no network for the zones at all. */
        zsrc_zw_link(b->src, false, now);
        if (ev->radio == ZW_NET_RADIO_FOREIGN) {
            return;
        }
        zsrc_zw_network(b->src, ev->home_id);
        b->rebase = true; /* zw_health restarts its clocks next */
        const zw_node_cache_t *nodes = zw_net_nodes(b->net);
        for (size_t i = 0; ev->radio == ZW_NET_RADIO_OURS && i < zw_node_count(nodes); i++) {
            feed(b, zw_node_at_const(nodes, i)->node_id);
        }
        zsrc_zw_link(b->src, true, now);
        return;
    }
    case ZW_NET_EV_JAM:
        /* network.md §9: jammed - the sensor_radio trouble at once, but the source keeps its link:
         * what gets through is still told and answered (Supervision, Time), and nothing it knew
         * is forgotten; once the air clears its watched devices are asked afresh (ZW-024) */
        zsrc_zw_jammed(b->src, ev->jammed, now);
        return;
    case ZW_NET_EV_NODE_ADDED:
    case ZW_NET_EV_NODE_READY:
    case ZW_NET_EV_NOT_USABLE: feed(b, ev->node); return;
    case ZW_NET_EV_NODE_REMOVED:
    case ZW_NET_EV_NODE_GONE:
        drop_node(b, ev->node);
        zsrc_zw_device_gone(b->src, ev->node, ev->generation, now);
        return;
    case ZW_NET_EV_UNREACHABLE: zsrc_zw_reachable(b->src, ev->node, false, now); return;
    case ZW_NET_EV_REACHABLE: zsrc_zw_reachable(b->src, ev->node, true, now); return;
    case ZW_NET_EV_CHECK_IN:
        /* network.md §7-8: the device confirmed a check-in of its own, or its interval read
         * again after a late wake-up changed - its zones' grace and supervision follow now, not
         * at the next network check or interview (0: no longer supervised, ZW-015); its inputs
         * are not asked again */
        if (ev->took) {
            (void)zsrc_zw_check_in(b->src, ev->node, ev->generation, ev->check_in_s);
        }
        return;
    default: return;
    }
}

/* ---- what devices say ------------------------------------------------------------------------ */

/* A Multilevel Sensor value in hundredths of its unit. */
static int32_t hundredths(int32_t raw, uint8_t precision)
{
    int64_t v = raw;
    for (unsigned p = precision; p < 2u; p++) {
        v *= 10;
    }
    for (unsigned p = 2u; p < precision && p < 12u; p++) {
        v /= 10;
    }
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

/* The command decoded for the zone source; false: not one it understands (a plain frame). */
static bool decode(zwz_t *b, const zw_s2_rx_t *rx, zsrc_zw_report_t *r)
{
    const uint8_t *c = rx->command;
    const size_t n = rx->len;
    uint8_t cmd = 0u;
    if (n > 1u) {
        cmd = c[1];
    }
    switch (c[0]) {
    case ZW_CC_NOTIFICATION: {
        zw_cc_notification_report_t p;
        if (cmd != ZW_CMD_NOTIFICATION_REPORT || !zw_cc_notification_report_parse(c, n, &p)) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_NOTIFICATION;
        r->notif_v1 = p.is_v1;
        r->notif_v1_type = p.v1_alarm_type; /* ZW-043: a quirk's alarm map reads them */
        r->notif_v1_level = p.v1_alarm_level;
        r->notif_status = p.status;
        r->notif_type = p.notification_type;
        r->notif_event = p.event;
        const size_t k = p.event_params_len < sizeof(r->notif_params) ? p.event_params_len
                                                                      : sizeof(r->notif_params);
        if (k > 0u && p.event_params != NULL) {
            memcpy(r->notif_params, p.event_params, k);
        }
        r->notif_param_len = (uint8_t)k;
        return true;
    }
    case ZW_CC_BINARY_SENSOR: {
        zw_cc_binary_sensor_report_t p;
        if (cmd != ZW_CMD_BINARY_SENSOR_REPORT || !zw_cc_binary_sensor_report_parse(c, n, &p)) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_BINARY;
        r->binary_has_type = p.has_type;
        r->binary_type = p.sensor_type;
        r->binary_on = p.detected;
        return true;
    }
    case ZW_CC_BATTERY: {
        zw_cc_battery_report_t p;
        if (cmd != ZW_CMD_BATTERY_REPORT || !zw_cc_battery_report_parse(c, n, &p)) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_BATTERY;
        r->battery_level = p.level;
        if (p.has_v2) {
            r->battery_replace = p.replace_or_recharge;
            r->battery_disconnected = p.disconnected;
        }
        return true;
    }
    case ZW_CC_MULTILEVEL_SENSOR: {
        zw_cc_multilevel_sensor_report_t p;
        if (cmd != ZW_CMD_MULTILEVEL_SENSOR_REPORT ||
            !zw_cc_multilevel_sensor_report_parse(c, n, &p)) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_MULTILEVEL;
        r->ml_type = p.sensor_type;
        r->ml_scale = p.scale;
        r->ml_value_x100 = hundredths(p.raw_value, p.precision);
        return true;
    }
    case ZW_CC_ALARM_SENSOR: {
        zw_cc_alarm_sensor_report_t p;
        if (cmd != ZW_CMD_ALARM_SENSOR_REPORT || !zw_cc_alarm_sensor_report_parse(c, n, &p)) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_ALARM_SENSOR;
        r->alarm_type = p.sensor_type;
        r->alarm_state = p.state;
        return true;
    }
    case ZW_CC_BASIC: {
        zw_cc_basic_set_t s;
        zw_cc_basic_report_t p;
        if (cmd == ZW_CMD_BASIC_SET && zw_cc_basic_set_parse(c, n, &s)) {
            r->basic_value = s.value;
        } else if (cmd == ZW_CMD_BASIC_REPORT && zw_cc_basic_report_parse(c, n, &p)) {
            r->basic_value = p.current_value;
        } else {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_BASIC;
        return true;
    }
    case ZW_CC_WAKE_UP:
        if (cmd != ZW_CMD_WAKE_UP_NOTIFICATION) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_WAKE_UP;
        return true;
    case ZW_CC_CENTRAL_SCENE: {
        zw_cc_central_scene_notification_t s;
        if (cmd != ZW_CMD_CENTRAL_SCENE_NOTIFICATION ||
            !zw_cc_central_scene_notification_parse(c, n, &s)) {
            return false;
        }
        r->kind = ZSRC_ZW_RPT_CENTRAL_SCENE;
        r->scene_seq = s.sequence_number;
        r->scene_attr = s.key_attribute;
        r->scene_number = s.scene_number;
        return true;
    }
    case ZW_CC_HAIL:
        if (!zw_cc_hail_parse(c, n)) {
            return false;
        }
        want_refresh(b, rx->node, 0x0Fu); /* "something changed, ask me" (§4.23) */
        return true;
    default: return false;
    }
}

void zwz_received(zwz_t *b, const zw_s2_rx_t *rx, uint64_t now)
{
    b->now = now;
    if (rx == NULL || rx->command == NULL || rx->len == 0u) {
        return;
    }
    zsrc_zw_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = ZSRC_ZW_RPT_FRAME;
    if (rx->multi_channel) {
        r.endpoint = rx->source_endpoint;
    }
    bool understood = decode(b, rx, &r);
    /* ZW-009 (network.md §8): every command that reaches here passed Security 2's level check,
     * as in Z-Wave JS - a check-in and its report both */
    zsrc_zw_report(b->src, rx->node, &r, now);
    note_heard(b, rx->node, now); /* zw_health heard this one too */
    if (r.kind == ZSRC_ZW_RPT_BATTERY) {
        zwz_battery_t *e = battery_slot(b, rx->node, now);
        if (e != NULL) {
            e->at = now; /* a day from this level */
        }
    }
    b->stats.reports++;
    if (!understood && b->claim != NULL && b->claim(b->claim_ctx, rx)) {
        understood = true; /* the owner's (an assigned keypad's command): it runs it */
    }
    if (!understood && b->answers && rx->command[0] == (uint8_t)ZW_CC_TIME) {
        /* a Time or Date Get: the panel's local time as it is now, or "clock not set" */
        zwz_local_time_t t;
        memset(&t, 0, sizeof(t));
        b->identity.clock_trusted = b->clock != NULL && b->clock(b->clock_ctx, &t);
        b->identity.time_hour = t.hour;
        b->identity.time_minute = t.minute;
        b->identity.time_second = t.second;
        b->identity.date_year = t.year;
        b->identity.date_month = t.month;
        b->identity.date_day = t.day;
    }
    if (!understood && b->answers) {
        uint8_t out[ZWZ_CMD_MAX - 4u]; /* room for the Multi Channel wrapper */
        size_t n = 0u;
        const zw_cc_answer_result_t a =
            zw_cc_panel_answer(rx->command, rx->len, &b->identity, out, sizeof(out), &n);
        if (a == ZW_CC_ANSWER_REPLY) {
            reply(b, rx, out, n);
            b->stats.answers++;
        }
        understood = a != ZW_CC_ANSWER_NOT_HANDLED;
    }
    if (!understood) {
        note_unknown_cc(b, rx->node, rx->command[0]); /* §2: logged once per node and CC */
    }
    if (rx->supervised) {
        /* command-classes.md §4.14: at once, with the Get's encapsulation */
        const zw_cc_supervision_report_t s = {
            false, false, rx->supervision_session,
            understood ? ZW_SUPERVISION_STATUS_SUCCESS : ZW_SUPERVISION_STATUS_NO_SUPPORT, 0u};
        uint8_t sr[8];
        const size_t k = zw_cc_supervision_report_build(sr, sizeof(sr), &s);
        reply(b, rx, sr, k);
        b->stats.supervision++;
    }
}

void zwz_set_clock(zwz_t *b, bool (*clock)(void *ctx, zwz_local_time_t *out), void *ctx)
{
    b->clock = clock;
    b->clock_ctx = ctx;
}

void zwz_set_claim(zwz_t *b, bool (*claim)(void *ctx, const zw_s2_rx_t *rx), void *ctx)
{
    b->claim = claim;
    b->claim_ctx = ctx;
}

bool zwz_send_done(zwz_t *b, uint32_t tag, zw_s2_result_t result)
{
    (void)result; /* a failed Get is not asked again: the device's next report says it */
    if (!zwz_is_tag(tag)) {
        return false;
    }
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        if (b->out[i].used && b->out[i].out && b->out[i].tag == tag) {
            memset(&b->out[i], 0, sizeof(b->out[i]));
        }
    }
    return true;
}

bool zwz_quirk(const zwz_t *b, const zw_node_record_t *rec, zw_iv_quirk_t *out)
{
    if (!rec->has_identity) {
        return false;
    }
    const zsrc_zw_quirk_t *q = zsrc_zw_quirk_lookup(b->src, rec->manufacturer_id,
                                                    rec->product_type, rec->product_id);
    if (q == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->lifeline_group = q->lifeline_group;
    /* the node cache keeps no firmware version: an unknown one counts as below the row's */
    out->no_supervision = zsrc_zw_quirk_no_supervision(q, false, 0u);
    for (size_t i = 0; i < q->config_count && i < ZSRC_ZW_QUIRK_CONFIG_MAX &&
                       i < ZW_IV_QUIRK_CONFIG_MAX;
         i++) {
        out->config[i].parameter = q->config[i].number;
        out->config[i].size = q->config[i].size;
        out->config[i].value = q->config[i].value;
        out->config_count = (uint8_t)(i + 1u);
    }
    return true;
}

/* ---- polling --------------------------------------------------------------------------------- */

/* One command of the outbox to zw_net; false: the security manager is busy, the rest waits. */
static bool send_one(zwz_t *b, zwz_out_t *o, uint64_t now)
{
    const zw_s2_result_t r = zw_net_send(b->net, now, o->node, (zw_sec_class_t)o->cls, o->cmd,
                                         o->len, o->tag);
    if (r == ZW_S2_OK) {
        o->out = true; /* a Get for a sleeping device: held until it wakes */
    } else if (r == ZW_S2_BUSY) {
        b->retry_at = now + ZWZ_RETRY_MS;
        b->stats.busy++;
        return false;
    } else {
        if (!o->refresh) {
            b->stats.replies_lost++;
        }
        memset(o, 0, sizeof(*o));
        b->stats.dropped++;
    }
    return true;
}

void zwz_poll(zwz_t *b, uint64_t now)
{
    b->now = now;
    uint16_t node = 0u;
    uint8_t eps = 0u;
    while (zsrc_zw_take_refresh(b->src, &node, &eps)) {
        want_refresh(b, node, eps);
    }
    if (!b->src->link_up) {
        return;
    }
    pass_heard(b, now);
    if (b->retry_at != 0u && now < b->retry_at) {
        return;
    }
    b->retry_at = 0u;
    if (b->get_retry_at == 0u || now >= b->get_retry_at) {
        b->get_retry_at = 0u;
        for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
            zwz_refresh_t *f = &b->refresh[i];
            if (f->node != 0u && !node_waiting(b, f->node)) {
                next_get(b, f);
            }
        }
        battery_reread(b, now);
    }
    /* the answers first, oldest first, whatever Get of their device is out: zw_net_send puts
     * a reply to a sleeper first in its queue and sends it at once (network.md section 7) */
    for (zwz_out_t *o = next_reply(b); o != NULL; o = next_reply(b)) {
        if (!send_one(b, o, now)) {
            return;
        }
    }
    for (size_t i = 0; i < ZWZ_OUTBOX; i++) {
        const zwz_out_t *head = &b->out[i];
        if (!head->used || head->out || !head->refresh || node_busy(b, head->node)) {
            continue;
        }
        if (!send_one(b, next_for(b, head->node), now)) {
            return;
        }
    }
}

uint64_t zwz_next_deadline(const zwz_t *b)
{
    if (!b->src->link_up) {
        return UINT64_MAX;
    }
    if (heard_waiting(b)) {
        return b->now;
    }
    /* a Get that found the outbox full waits for get_retry_at; the answers do not */
    const bool gets_wait = b->get_retry_at != 0u && b->now < b->get_retry_at;
    bool work = false;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX && !work; i++) {
        const zwz_refresh_t *f = &b->refresh[i];
        work = (!gets_wait && f->node != 0u && !node_waiting(b, f->node)) ||
               (b->src->dev[i].used && b->src->dev[i].refresh != 0u);
    }
    for (size_t i = 0; i < ZWZ_OUTBOX && !work; i++) {
        work = b->out[i].used && !b->out[i].out &&
               (!b->out[i].refresh || !node_busy(b, b->out[i].node));
    }
    if (!work) {
        uint64_t due = battery_due(b);
        bool asking = false; /* a refresh's next Get waits for room */
        for (size_t i = 0; i < ZSRC_ZW_DEV_MAX && gets_wait && !asking; i++) {
            asking = b->refresh[i].node != 0u && !node_waiting(b, b->refresh[i].node);
        }
        if (gets_wait && (asking || due < b->get_retry_at)) {
            due = b->get_retry_at;
        }
        return due < b->now ? b->now : due;
    }
    return b->retry_at != 0u ? b->retry_at : b->now;
}
