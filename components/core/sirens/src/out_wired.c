/*
 * out_wired.c - the panel's wired outputs (out_wired.h).
 */
#include "sirens/out_wired.h"

#include <string.h>

#define OW_READ_MS 100u /* supervision inputs are read this often */

static bool line_of(const ow_config_t *c, bool on)
{
    return c->active_low ? !on : on;
}

static siren_scope_t scope_of(const ow_config_t *c)
{
    const siren_scope_t s = {.partitions = c->partitions, .also = c->also, .burglary = c->burglary,
                             .fire = c->fire};
    return s;
}

static bool used(const ow_channel_t *ch)
{
    return ch->c.function != OW_FN_NONE;
}

void ow_init(ow_t *o, const ow_port_t *port)
{
    memset(o, 0, sizeof(*o));
    o->port = *port;
    siren_view_init(&o->view);
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        o->ch[i].c.sense = OW_SENSE_NONE;
    }
}

const char *ow_function_name(uint8_t function)
{
    switch (function) {
    case OW_FN_SIREN: return "siren";
    case OW_FN_STROBE: return "strobe";
    case OW_FN_SMOKE_RESET: return "smoke reset";
    case OW_FN_ARMED: return "armed";
    default: return "";
    }
}

int ow_find_pin(const ow_t *o, uint8_t gpio)
{
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        const ow_channel_t *ch = &o->ch[i];
        if (used(ch) && (ch->c.gpio == gpio || ch->c.sense == gpio)) {
            return (int)i;
        }
    }
    return -1;
}

static void tell_trouble(ow_t *o)
{
    bool any = false;
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        any = any || (used(&o->ch[i]) && o->ch[i].trouble);
    }
    if (any != o->trouble) {
        o->trouble = any;
        if (o->port.bell_trouble != NULL) {
            o->port.bell_trouble(o->port.ctx, any);
        }
    }
}

/* Give a channel's pins back, its output inactive first. */
static void let_go(ow_t *o, ow_channel_t *ch)
{
    if (ch->pin_ok) {
        if (o->port.write != NULL) {
            o->port.write(o->port.ctx, ch->c.gpio, line_of(&ch->c, false));
        }
        if (o->port.release != NULL) {
            o->port.release(o->port.ctx, ch->c.gpio);
        }
    }
    if (ch->sense_ok && o->port.release != NULL) {
        o->port.release(o->port.ctx, ch->c.sense);
    }
    ch->pin_ok = false;
    ch->sense_ok = false;
}

ow_result_t ow_set(ow_t *o, uint64_t now, size_t slot, const ow_config_t *c)
{
    if (slot >= OW_CHANNELS || c->function >= OW_FN_COUNT) {
        return OW_ERR_ARG;
    }
    if (c->function == OW_FN_NONE) {
        return ow_clear(o, slot);
    }
    if (c->function != OW_FN_SMOKE_RESET && c->partitions == 0u) {
        return OW_ERR_ARG;
    }
    if (c->function == OW_FN_SIREN && !c->burglary && !c->fire) {
        return OW_ERR_ARG;
    }
    if (c->sense != OW_SENSE_NONE && c->sense == c->gpio) {
        return OW_ERR_ARG;
    }
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        if (i == slot || !used(&o->ch[i])) {
            continue;
        }
        const ow_config_t *x = &o->ch[i].c;
        if (x->gpio == c->gpio || x->sense == c->gpio ||
            (c->sense != OW_SENSE_NONE && (x->gpio == c->sense || x->sense == c->sense))) {
            return OW_ERR_IN_USE;
        }
    }
    ow_channel_t *ch = &o->ch[slot];
    if (used(ch)) {
        let_go(o, ch);
    }
    memset(ch, 0, sizeof(*ch));
    ch->c = *c;
    o->dirty = true;
    if (o->port.setup == NULL ||
        !o->port.setup(o->port.ctx, c->gpio, true, line_of(c, false))) {
        ch->c.function = OW_FN_NONE;
        ch->c.sense = OW_SENSE_NONE;
        tell_trouble(o);
        return OW_ERR_PIN;
    }
    ch->pin_ok = true;
    ch->driven = false;
    if (c->sense != OW_SENSE_NONE) {
        ch->sense_ok = o->port.setup(o->port.ctx, c->sense, false, false);
    }
    ow_poll(o, now);
    return OW_OK;
}

ow_result_t ow_clear(ow_t *o, size_t slot)
{
    if (slot >= OW_CHANNELS) {
        return OW_ERR_ARG;
    }
    ow_channel_t *ch = &o->ch[slot];
    if (used(ch)) {
        let_go(o, ch);
        o->dirty = true;
    }
    memset(ch, 0, sizeof(*ch));
    ch->c.sense = OW_SENSE_NONE;
    tell_trouble(o);
    return OW_OK;
}

static void start_chirps(ow_t *o, uint64_t now, uint8_t partition, uint8_t chirps)
{
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        ow_channel_t *ch = &o->ch[i];
        if (ch->c.function != OW_FN_SIREN || !ch->c.squawk ||
            (ch->c.partitions & (1u << (partition - 1u))) == 0u || ch->sound != SIREN_QUIET ||
            ch->test_until != 0u) {
            continue;
        }
        ch->chirps = chirps;
        ch->chirp_at = now;
    }
}

void ow_output(ow_t *o, uint64_t now, uint8_t partition, alarm_output_t out,
               alarm_pattern_t pattern)
{
    if (siren_view_output(&o->view, now, partition, out, pattern)) {
        ow_poll(o, now);
    }
}

void ow_armed(ow_t *o, uint64_t now, uint8_t partition, bool armed)
{
    if (!siren_view_armed(&o->view, partition, armed)) {
        return;
    }
    /* Not while a partition's alarm sounds: on a disarm that silences it the silence is the
     * feedback (the sound goes quiet in this poll, so the chirps would follow at once). */
    bool sounding = false;
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        sounding = sounding || (o->ch[i].c.function == OW_FN_SIREN && o->ch[i].sound != SIREN_QUIET &&
                                o->ch[i].sound != SIREN_TEST &&
                                (o->ch[i].c.partitions & (1u << (partition - 1u))) != 0u);
    }
    if (!sounding) {
        start_chirps(o, now, partition, armed ? SIREN_CHIRPS_ARM : SIREN_CHIRPS_DISARM);
    }
    ow_poll(o, now);
}

void ow_set_armed(ow_t *o, uint64_t now, uint8_t partition, bool armed)
{
    if (siren_view_armed(&o->view, partition, armed)) {
        ow_poll(o, now);
    }
}

ow_result_t ow_test(ow_t *o, uint64_t now, size_t slot)
{
    if (slot >= OW_CHANNELS) {
        return OW_ERR_ARG;
    }
    ow_channel_t *ch = &o->ch[slot];
    if (ch->c.function != OW_FN_SIREN && ch->c.function != OW_FN_STROBE) {
        return OW_ERR_ARG;
    }
    const siren_scope_t s = scope_of(&ch->c);
    if (siren_armed(&s, &o->view)) {
        return OW_ERR_ARMED;
    }
    if (ch->on && ch->test_until == 0u && ch->chirps == 0u) {
        return OW_ERR_BUSY;
    }
    ch->chirps = 0u;
    ch->test_until = now + OW_TEST_MS;
    ow_poll(o, now);
    return OW_OK;
}

/* What the channel's line should be now, and its next edge (0 none). */
static bool want(ow_t *o, ow_channel_t *ch, uint64_t now, uint64_t *next)
{
    const siren_scope_t s = scope_of(&ch->c);
    *next = 0u;
    if (ch->test_until != 0u) {
        if (now < ch->test_until) {
            *next = ch->test_until;
            return true;
        }
        ch->test_until = 0u;
    }
    switch (ch->c.function) {
    case OW_FN_SIREN: {
        const siren_sound_t sound = siren_pick(&s, &o->view, now);
        if (sound != ch->sound) {
            ch->sound = sound;
            ch->sound_at = now;
            if (sound != SIREN_QUIET) {
                ch->chirps = 0u;
            }
        }
        uint32_t step = UINT32_MAX;
        bool on = false;
        if (sound != SIREN_QUIET) {
            on = siren_level(sound, (uint32_t)(now - ch->sound_at), &step);
            const uint64_t end = siren_view_deadline(&s, &o->view, now);
            if (end != 0u) {
                *next = end;
            }
        } else if (ch->chirps != 0u) {
            on = siren_chirp_level(ch->chirps, (uint32_t)(now - ch->chirp_at), &step);
            if (step == UINT32_MAX) {
                ch->chirps = 0u;
            }
        }
        if (step != UINT32_MAX && (*next == 0u || now + step < *next)) {
            *next = now + step;
        }
        return on;
    }
    case OW_FN_STROBE: return siren_strobe(&s, &o->view);
    case OW_FN_SMOKE_RESET: return o->view.smoke_reset;
    case OW_FN_ARMED: return siren_armed(&s, &o->view);
    default: return false;
    }
}

static void supervise(ow_t *o, ow_channel_t *ch, uint64_t now)
{
    if (!ch->sense_ok || o->port.read == NULL) {
        ch->trouble = false;
        return;
    }
    if (o->port.read(o->port.ctx, ch->c.sense) != 0u) {
        if (!ch->sense_open) {
            ch->sense_open = true;
            ch->sense_since = now;
        }
        if (now - ch->sense_since >= OW_SENSE_MS) {
            ch->trouble = true;
        }
    } else {
        ch->sense_open = false;
        ch->trouble = false;
    }
}

void ow_poll(ow_t *o, uint64_t now)
{
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        ow_channel_t *ch = &o->ch[i];
        if (!used(ch)) {
            continue;
        }
        if (!ch->pin_ok) {
            ch->trouble = true; /* a siren whose pin can't be driven may not sound */
            continue;
        }
        ch->on = want(o, ch, now, &ch->next);
        if (ch->on != ch->driven) {
            ch->driven = ch->on;
            if (o->port.write != NULL) {
                o->port.write(o->port.ctx, ch->c.gpio, line_of(&ch->c, ch->on));
            }
        }
        supervise(o, ch, now);
    }
    tell_trouble(o);
}

uint64_t ow_next_deadline(const ow_t *o, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        const ow_channel_t *ch = &o->ch[i];
        if (!used(ch)) {
            continue;
        }
        if (ch->next != 0u && ch->next < next) {
            next = ch->next;
        }
        if (ch->sense_ok) {
            uint64_t at = now + OW_READ_MS;
            if (ch->sense_open && !ch->trouble && ch->sense_since + OW_SENSE_MS < at) {
                at = ch->sense_since + OW_SENSE_MS;
            }
            if (at < next) {
                next = at;
            }
        }
    }
    return next;
}

size_t ow_encode(const ow_t *o, uint8_t *out, size_t cap)
{
    if (cap < OW_BLOB_MAX) {
        return 0u;
    }
    size_t n = 0u;
    out[n++] = 'O';
    out[n++] = 'W';
    out[n++] = OW_FORMAT;
    const size_t count_at = n++;
    uint8_t count = 0u;
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        const ow_config_t *c = &o->ch[i].c;
        if (c->function == OW_FN_NONE) {
            continue;
        }
        out[n++] = OW_ENTRY;
        out[n++] = (uint8_t)i;
        out[n++] = c->function;
        out[n++] = c->gpio;
        out[n++] = c->partitions;
        out[n++] = (uint8_t)((c->burglary ? 0x01u : 0u) | (c->fire ? 0x02u : 0u) |
                             (c->squawk ? 0x04u : 0u) | (c->active_low ? 0x08u : 0u));
        out[n++] = (c->sense == OW_SENSE_NONE) ? 0u : (uint8_t)(c->sense + 1u);
        out[n++] = c->also;
        out[n++] = 0u;
        count++;
    }
    out[count_at] = count;
    const uint16_t crc = siren_crc16(out, n);
    out[n++] = (uint8_t)(crc >> 8);
    out[n++] = (uint8_t)(crc & 0xFFu);
    return n;
}

bool ow_decode(const uint8_t *buf, size_t len, ow_config_t out[OW_CHANNELS])
{
    if (len < 6u || buf[0] != 'O' || buf[1] != 'W' || buf[2] == 0u || buf[2] > OW_FORMAT) {
        return false;
    }
    const uint16_t crc = (uint16_t)(((unsigned)buf[len - 2u] << 8) | buf[len - 1u]);
    if (siren_crc16(buf, len - 2u) != crc) {
        return false;
    }
    ow_config_t got[OW_CHANNELS];
    memset(got, 0, sizeof(got));
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        got[i].sense = OW_SENSE_NONE;
    }
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
        uint8_t e[OW_ENTRY];
        memset(e, 0, sizeof(e));
        memcpy(e, &buf[p], elen < OW_ENTRY ? elen : OW_ENTRY);
        p += elen;
        if (e[0] >= OW_CHANNELS || e[1] == OW_FN_NONE || e[1] >= OW_FN_COUNT ||
            got[e[0]].function != OW_FN_NONE) {
            return false;
        }
        ow_config_t *c = &got[e[0]];
        c->function = e[1];
        c->gpio = e[2];
        c->partitions = e[3];
        c->burglary = (e[4] & 0x01u) != 0u;
        c->fire = (e[4] & 0x02u) != 0u;
        c->squawk = (e[4] & 0x04u) != 0u;
        c->active_low = (e[4] & 0x08u) != 0u;
        c->sense = (e[5] == 0u) ? OW_SENSE_NONE : (uint8_t)(e[5] - 1u);
        c->also = e[6];
    }
    if (p != end) {
        return false;
    }
    memcpy(out, got, sizeof(got));
    return true;
}
