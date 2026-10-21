/*
 * rf_siren.c - 433 MHz sirens (rf_siren.h).
 */
#include "sirens/rf_siren.h"

#include <string.h>

static siren_scope_t scope_of(const rfs_config_t *c)
{
    const siren_scope_t s = {.partitions = c->partitions, .also = c->also, .burglary = c->burglary,
                             .fire = c->fire};
    return s;
}

static uint16_t unit_of(const rfs_config_t *c)
{
    return (c->unit_us != 0u) ? c->unit_us : (uint16_t)RFS_UNIT_US;
}

static void tell_trouble(rfs_t *r)
{
    bool any = false;
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        const rfs_siren_t *s = &r->s[i];
        any = any || (s->c.used && (!r->transmitter || s->refused));
    }
    if (any != r->trouble) {
        r->trouble = any;
        if (r->port.bell_trouble != NULL) {
            r->port.bell_trouble(r->port.ctx, any);
        }
    }
}

/* One transmission of `code` for siren `s`. */
static bool transmit(rfs_t *r, uint64_t now, rfs_siren_t *s, uint8_t code, uint8_t frames)
{
    const rf_packet_t p = rf_packet_make(s->c.proto, s->c.address, code, 0u, 0u);
    const bool ok = r->transmitter && r->port.send != NULL &&
                    r->port.send(r->port.ctx, &p, unit_of(&s->c), frames);
    s->refused = !ok;
    if (ok) {
        s->sent++;
        s->last_sent = (now == 0u) ? 1u : now;
    }
    return ok;
}

void rfs_init(rfs_t *r, const rfs_port_t *port)
{
    memset(r, 0, sizeof(*r));
    r->port = *port;
    siren_view_init(&r->view);
}

void rfs_defaults(rfs_config_t *c, uint32_t address)
{
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->proto = RF_PROTO_EV1527;
    c->address = address & 0xFFFFFu;
    c->code_alarm = RFS_CODE_ALARM;
    c->code_stop = RFS_CODE_STOP;
    c->code_arm = RFS_CODE_ARM;
    c->code_disarm = RFS_CODE_DISARM;
    c->partitions = 0x01u;
    c->burglary = true;
    c->fire = true;
    c->squawk = false;
    c->unit_us = 0u;
}

uint32_t rfs_pick_address(uint32_t random, const uint32_t *avoid, size_t n_avoid)
{
    uint32_t a = random & 0xFFFFFu;
    for (unsigned tries = 0; tries < 64u; tries++) {
        bool bad = rf_address_is_common(RF_PROTO_EV1527, a) || a == 0u;
        for (size_t i = 0; i < n_avoid && !bad; i++) {
            bad = (avoid[i] & 0xFFFFFu) == a;
        }
        if (!bad) {
            return a;
        }
        /* the next candidate: a step of a large odd number walks every 20-bit value */
        a = (a + 0x9E3B5u) & 0xFFFFFu;
    }
    return 0u;
}

int rfs_free_slot(const rfs_t *r)
{
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        if (!r->s[i].c.used) {
            return (int)i;
        }
    }
    return -1;
}

static bool valid(const rfs_config_t *c)
{
    const rf_proto_info_t *info = rf_proto_info(c->proto);
    if (c->proto != RF_PROTO_EV1527 || info == NULL || c->partitions == 0u ||
        (!c->burglary && !c->fire) || c->address == 0u || c->address > 0xFFFFFu ||
        rf_address_is_common(c->proto, c->address)) {
        return false;
    }
    const uint8_t limit = (uint8_t)((1u << info->data_bits) - 1u);
    if (c->code_alarm > limit || c->code_stop > limit || c->code_arm > limit ||
        c->code_disarm > limit) {
        return false;
    }
    return c->unit_us == 0u || (c->unit_us >= RF_EV1527_UNIT_MIN_US && c->unit_us <= RF_EV1527_UNIT_MAX_US);
}

rfs_result_t rfs_set(rfs_t *r, uint64_t now, size_t slot, const rfs_config_t *c)
{
    if (slot >= RFS_SIRENS || !c->used || !valid(c)) {
        return RFS_ERR_ARG;
    }
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        if (i != slot && r->s[i].c.used && r->s[i].c.address == c->address) {
            return RFS_ERR_IN_USE;
        }
    }
    rfs_siren_t *s = &r->s[slot];
    const bool sounding = s->c.used && s->sound != SIREN_QUIET;
    const bool moved = s->c.used && s->c.address != c->address;
    if (sounding && moved) {
        /* its old address goes quiet first (best effort, once) */
        (void)transmit(r, now, s, s->c.code_stop, RFS_FRAMES);
    }
    if (!s->c.used || moved) {
        memset(s, 0, sizeof(*s));
    }
    s->c = *c;
    r->dirty = true;
    rfs_poll(r, now);
    return RFS_OK;
}

rfs_result_t rfs_clear(rfs_t *r, uint64_t now, size_t slot)
{
    if (slot >= RFS_SIRENS) {
        return RFS_ERR_ARG;
    }
    rfs_siren_t *s = &r->s[slot];
    if (!s->c.used) {
        return RFS_ERR_UNKNOWN;
    }
    if (s->sound != SIREN_QUIET || s->test_until != 0u) {
        (void)transmit(r, now, s, s->c.code_stop, RFS_FRAMES);
    }
    memset(s, 0, sizeof(*s));
    r->dirty = true;
    tell_trouble(r);
    return RFS_OK;
}

void rfs_output(rfs_t *r, uint64_t now, uint8_t partition, alarm_output_t out,
                alarm_pattern_t pattern)
{
    if (out == ALARM_OUT_STROBE || out == ALARM_OUT_SMOKE_RESET) {
        return; /* a one-way siren's strobe goes with its sound */
    }
    if (siren_view_output(&r->view, now, partition, out, pattern)) {
        rfs_poll(r, now);
    }
}

void rfs_armed(rfs_t *r, uint64_t now, uint8_t partition, bool armed)
{
    if (!siren_view_armed(&r->view, partition, armed) || partition == 0u || partition > 8u) {
        return;
    }
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        rfs_siren_t *s = &r->s[i];
        if (!s->c.used || !s->c.squawk || (s->c.partitions & (1u << (partition - 1u))) == 0u ||
            s->sound != SIREN_QUIET || s->stops_left != 0u || s->test_until != 0u) {
            continue;
        }
        (void)transmit(r, now, s, armed ? s->c.code_arm : s->c.code_disarm, RFS_FRAMES);
    }
    rfs_poll(r, now);
}

void rfs_set_armed(rfs_t *r, uint8_t partition, bool armed)
{
    (void)siren_view_armed(&r->view, partition, armed);
}

void rfs_transmitter(rfs_t *r, uint64_t now, bool present)
{
    r->transmitter = present;
    if (present) {
        for (size_t i = 0; i < RFS_SIRENS; i++) {
            r->s[i].refused = false;
        }
    }
    rfs_poll(r, now);
}

rfs_result_t rfs_test(rfs_t *r, uint64_t now, size_t slot)
{
    if (slot >= RFS_SIRENS) {
        return RFS_ERR_ARG;
    }
    rfs_siren_t *s = &r->s[slot];
    if (!s->c.used) {
        return RFS_ERR_UNKNOWN;
    }
    const siren_scope_t sc = scope_of(&s->c);
    if (siren_armed(&sc, &r->view)) {
        return RFS_ERR_ARMED;
    }
    if (s->sound != SIREN_QUIET) {
        return RFS_ERR_BUSY;
    }
    if (!transmit(r, now, s, s->c.code_alarm, RFS_FRAMES)) {
        tell_trouble(r);
        return RFS_ERR_SEND;
    }
    s->test_until = now + RFS_TEST_MS;
    tell_trouble(r);
    return RFS_OK;
}

rfs_result_t rfs_pair(rfs_t *r, uint64_t now, size_t slot)
{
    if (slot >= RFS_SIRENS) {
        return RFS_ERR_ARG;
    }
    rfs_siren_t *s = &r->s[slot];
    if (!s->c.used) {
        return RFS_ERR_UNKNOWN;
    }
    const siren_scope_t sc = scope_of(&s->c);
    if (siren_armed(&sc, &r->view)) {
        return RFS_ERR_ARMED;
    }
    if (!transmit(r, now, s, s->c.code_arm, RFS_PAIR_FRAMES)) {
        tell_trouble(r);
        return RFS_ERR_SEND;
    }
    s->pairs_left = RFS_PAIR_SENDS - 1u;
    s->pair_at = now + RFS_STOP_GAP_MS;
    tell_trouble(r);
    return RFS_OK;
}

void rfs_poll(rfs_t *r, uint64_t now)
{
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        rfs_siren_t *s = &r->s[i];
        if (!s->c.used) {
            continue;
        }
        const siren_scope_t sc = scope_of(&s->c);
        siren_sound_t want = siren_pick(&sc, &r->view, now);
        if (want != SIREN_QUIET && s->sound == SIREN_QUIET) {
            /* a sound begins: the alarm code now (a test or squawk under way gives way) */
            s->test_until = 0u;
            s->pairs_left = 0u;
            s->stops_left = 0u;
            s->sound = want;
            s->next_send = transmit(r, now, s, s->c.code_alarm, RFS_FRAMES)
                               ? now + RFS_REPEAT_MS
                               : now + RFS_STOP_GAP_MS; /* no transmitter: try again soon */
        } else if (want != SIREN_QUIET) {
            s->sound = want;
            if (now >= s->next_send) {
                s->next_send = transmit(r, now, s, s->c.code_alarm, RFS_FRAMES)
                                   ? now + RFS_REPEAT_MS
                                   : now + RFS_STOP_GAP_MS;
            }
        } else if (s->sound != SIREN_QUIET) {
            /* the sound ended: stop, several times (nobody can hear whether it took) */
            s->sound = SIREN_QUIET;
            s->next_send = 0u;
            s->stops_left = RFS_STOP_SENDS;
            s->stop_at = now;
        }
        if (s->test_until != 0u && now >= s->test_until) {
            s->test_until = 0u;
            s->stops_left = RFS_STOP_SENDS;
            s->stop_at = now;
        }
        if (s->stops_left != 0u && now >= s->stop_at) {
            (void)transmit(r, now, s, s->c.code_stop, RFS_FRAMES);
            s->stops_left--;
            s->stop_at = now + RFS_STOP_GAP_MS;
        }
        if (s->pairs_left != 0u && now >= s->pair_at) {
            (void)transmit(r, now, s, s->c.code_arm, RFS_PAIR_FRAMES);
            s->pairs_left--;
            s->pair_at = now + RFS_STOP_GAP_MS;
        }
    }
    tell_trouble(r);
}

static void earliest(uint64_t *best, uint64_t at)
{
    if (at != 0u && at < *best) {
        *best = at;
    }
}

uint64_t rfs_next_deadline(const rfs_t *r, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        const rfs_siren_t *s = &r->s[i];
        if (!s->c.used) {
            continue;
        }
        const siren_scope_t sc = scope_of(&s->c);
        if (s->sound != SIREN_QUIET) {
            earliest(&next, s->next_send);
        }
        earliest(&next, s->test_until);
        if (s->stops_left != 0u) {
            earliest(&next, s->stop_at == 0u ? 1u : s->stop_at);
        }
        if (s->pairs_left != 0u) {
            earliest(&next, s->pair_at);
        }
        earliest(&next, siren_view_deadline(&sc, &r->view, now));
    }

    return next;
}

size_t rfs_encode(const rfs_t *r, uint8_t *out, size_t cap)
{
    if (cap < RFS_BLOB_MAX) {
        return 0u;
    }
    size_t n = 0u;
    out[n++] = 'R';
    out[n++] = 'S';
    out[n++] = RFS_FORMAT;
    const size_t count_at = n++;
    uint8_t count = 0u;
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        const rfs_config_t *c = &r->s[i].c;
        if (!c->used) {
            continue;
        }
        out[n++] = RFS_ENTRY;
        out[n++] = (uint8_t)i;
        out[n++] = c->proto;
        out[n++] = (uint8_t)(c->address >> 24);
        out[n++] = (uint8_t)(c->address >> 16);
        out[n++] = (uint8_t)(c->address >> 8);
        out[n++] = (uint8_t)(c->address & 0xFFu);
        out[n++] = c->code_alarm;
        out[n++] = c->code_stop;
        out[n++] = c->code_arm;
        out[n++] = c->code_disarm;
        out[n++] = c->partitions;
        out[n++] = (uint8_t)((c->burglary ? 0x01u : 0u) | (c->fire ? 0x02u : 0u) |
                             (c->squawk ? 0x04u : 0u));
        out[n++] = (uint8_t)(c->unit_us >> 8);
        out[n++] = (uint8_t)(c->unit_us & 0xFFu);
        out[n++] = c->also;
        count++;
    }
    out[count_at] = count;
    const uint16_t crc = siren_crc16(out, n);
    out[n++] = (uint8_t)(crc >> 8);
    out[n++] = (uint8_t)(crc & 0xFFu);
    return n;
}

bool rfs_decode(const uint8_t *buf, size_t len, rfs_config_t out[RFS_SIRENS])
{
    if (len < 6u || buf[0] != 'R' || buf[1] != 'S' || buf[2] == 0u || buf[2] > RFS_FORMAT) {
        return false;
    }
    const uint16_t crc = (uint16_t)(((unsigned)buf[len - 2u] << 8) | buf[len - 1u]);
    if (siren_crc16(buf, len - 2u) != crc) {
        return false;
    }
    rfs_config_t got[RFS_SIRENS];
    memset(got, 0, sizeof(got));
    const size_t end = len - 2u;
    size_t p = 4u;
    for (unsigned k = 0; k < buf[3]; k++) {
        if (p >= end) {
            return false;
        }
        const size_t elen = buf[p++];
        if (elen < 2u || p + elen > end) {
            return false;
        }
        uint8_t e[RFS_ENTRY];
        memset(e, 0, sizeof(e));
        memcpy(e, &buf[p], elen < RFS_ENTRY ? elen : RFS_ENTRY);
        p += elen;
        if (e[0] >= RFS_SIRENS || got[e[0]].used) {
            return false;
        }
        rfs_config_t *c = &got[e[0]];
        c->used = true;
        c->proto = e[1];
        c->address = ((uint32_t)e[2] << 24) | ((uint32_t)e[3] << 16) | ((uint32_t)e[4] << 8) | e[5];
        c->code_alarm = e[6];
        c->code_stop = e[7];
        c->code_arm = e[8];
        c->code_disarm = e[9];
        c->partitions = e[10];
        c->burglary = (e[11] & 0x01u) != 0u;
        c->fire = (e[11] & 0x02u) != 0u;
        c->squawk = (e[11] & 0x04u) != 0u;
        c->unit_us = (uint16_t)(((unsigned)e[12] << 8) | e[13]);
        c->also = e[14];
    }
    if (p != end) {
        return false;
    }
    memcpy(out, got, sizeof(got));
    return true;
}
