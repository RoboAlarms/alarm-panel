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
    case OW_FN_AUTOMATION: return "automation";
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

/* At most `cap` - 1 bytes of `src` (stopping at a NUL), never ending inside a UTF-8 sequence
 * (one a cut split is dropped). */
static void name_copy(char *dst, size_t cap, const char *src, size_t len)
{
    size_t n = 0u;
    while (n < len && n + 1u < cap && src[n] != '\0') {
        n++;
    }
    size_t start = n; /* the last sequence's lead byte, if it isn't ASCII */
    while (start > 0u && ((unsigned char)src[start - 1u] & 0xC0u) == 0x80u) {
        start--;
    }
    if (start > 0u && ((unsigned char)src[start - 1u] & 0xC0u) == 0xC0u) {
        const unsigned char lead = (unsigned char)src[start - 1u];
        const size_t need = (lead & 0xE0u) == 0xC0u ? 2u : (lead & 0xF0u) == 0xE0u ? 3u : 4u;
        if (n - (start - 1u) < need) {
            n = start - 1u;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* What ow_set keeps of a configuration: an AUTOMATION channel only its pin, relay board and name. */
static ow_config_t tidy(const ow_config_t *c)
{
    ow_config_t t = *c;
    if (t.function == OW_FN_AUTOMATION) {
        const ow_config_t a = {.function = OW_FN_AUTOMATION, .gpio = c->gpio,
                               .active_low = c->active_low, .sense = OW_SENSE_NONE};
        t = a;
        name_copy(t.name, sizeof(t.name), c->name, sizeof(c->name));
    } else {
        memset(t.name, 0, sizeof(t.name));
    }
    return t;
}

ow_result_t ow_set(ow_t *o, uint64_t now, size_t slot, const ow_config_t *given)
{
    if (slot >= OW_CHANNELS || given->function >= OW_FN_COUNT) {
        return OW_ERR_ARG;
    }
    if (given->function == OW_FN_NONE) {
        return ow_clear(o, slot);
    }
    const ow_config_t tidied = tidy(given);
    const ow_config_t *c = &tidied;
    if (c->function != OW_FN_SMOKE_RESET && c->function != OW_FN_AUTOMATION &&
        c->partitions == 0u) {
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
    if (ch->c.function == OW_FN_AUTOMATION) {
        if (!ch->pin_ok) {
            return OW_ERR_PIN;
        }
        ch->test_until = now + OW_TEST_MS; /* a light, not a siren: never refused (OUT-023) */
        ow_poll(o, now);
        return OW_OK;
    }
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

ow_result_t ow_auto_set(ow_t *o, uint64_t now, size_t slot, bool on)
{
    if (slot >= OW_CHANNELS || o->ch[slot].c.function != OW_FN_AUTOMATION) {
        return OW_ERR_ARG;
    }
    ow_channel_t *ch = &o->ch[slot];
    if (!ch->pin_ok) {
        return OW_ERR_PIN;
    }
    ch->auto_on = on;
    ow_poll(o, now);
    return OW_OK;
}

uint8_t ow_auto_mask(const ow_t *o)
{
    uint8_t mask = 0u;
    for (size_t i = 0; i < OW_CHANNELS && i < 8u; i++) {
        if (o->ch[i].c.function == OW_FN_AUTOMATION && o->ch[i].pin_ok) {
            mask = (uint8_t)(mask | (1u << i));
        }
    }
    return mask;
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
    case OW_FN_AUTOMATION: return ch->auto_on; /* never the engine's (AUT-002) */
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

static size_t put_crc(uint8_t *out, size_t n)
{
    const uint16_t crc = siren_crc16(out, n);
    out[n++] = (uint8_t)(crc >> 8);
    out[n++] = (uint8_t)(crc & 0xFFu);
    return n;
}

static bool crc_ok(const uint8_t *buf, size_t len)
{
    const uint16_t crc = (uint16_t)(((unsigned)buf[len - 2u] << 8) | buf[len - 1u]);
    return siren_crc16(buf, len - 2u) == crc;
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
        if (c->function == OW_FN_NONE || c->function == OW_FN_AUTOMATION) {
            continue; /* an output for automations is outputs/auto's */
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
    return put_crc(out, n);
}

bool ow_decode(const uint8_t *buf, size_t len, ow_config_t out[OW_CHANNELS])
{
    if (len < 6u || buf[0] != 'O' || buf[1] != 'W' || buf[2] == 0u || buf[2] > OW_FORMAT ||
        !crc_ok(buf, len)) {
        return false;
    }
    ow_config_t got[OW_CHANNELS];
    bool seen[OW_CHANNELS] = {false};
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
        if (e[0] >= OW_CHANNELS || e[1] == OW_FN_NONE || seen[e[0]]) {
            return false;
        }
        seen[e[0]] = true;
        if (e[1] >= OW_FN_COUNT || e[1] == OW_FN_AUTOMATION) {
            continue; /* not kept here (a newer firmware's function): that channel stays free */
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

size_t ow_auto_encode(const ow_t *o, uint8_t *out, size_t cap)
{
    if (cap < OW_AUTO_BLOB_MAX) {
        return 0u;
    }
    size_t n = 0u;
    out[n++] = 'O';
    out[n++] = 'A';
    out[n++] = OW_AUTO_FORMAT;
    const size_t count_at = n++;
    uint8_t count = 0u;
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        const ow_config_t *c = &o->ch[i].c;
        if (c->function != OW_FN_AUTOMATION) {
            continue;
        }
        size_t name_len = 0u;
        while (name_len + 1u < sizeof(c->name) && c->name[name_len] != '\0') {
            name_len++;
        }
        out[n++] = (uint8_t)(OW_AUTO_ENTRY + 1u + name_len);
        out[n++] = (uint8_t)i;
        out[n++] = c->gpio;
        out[n++] = c->active_low ? 0x01u : 0u;
        out[n++] = (uint8_t)name_len;
        memcpy(&out[n], c->name, name_len);
        n += name_len;
        count++;
    }
    out[count_at] = count;
    return put_crc(out, n);
}

bool ow_auto_decode(const uint8_t *buf, size_t len, ow_config_t out[OW_CHANNELS])
{
    if (len < 6u || buf[0] != 'O' || buf[1] != 'A' || buf[2] == 0u || buf[2] > OW_AUTO_FORMAT ||
        !crc_ok(buf, len)) {
        return false;
    }
    ow_config_t got[OW_CHANNELS];
    bool seen[OW_CHANNELS] = {false};
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
        uint8_t e[OW_AUTO_ENTRY + 1u];
        memset(e, 0, sizeof(e));
        memcpy(e, &buf[p], elen < sizeof(e) ? elen : sizeof(e));
        const size_t name_len = e[OW_AUTO_ENTRY];
        if (name_len != 0u && OW_AUTO_ENTRY + 1u + name_len > elen) {
            return false;
        }
        const char *name = (const char *)&buf[p + OW_AUTO_ENTRY + 1u];
        p += elen;
        if (e[0] >= OW_CHANNELS || seen[e[0]]) {
            return false;
        }
        seen[e[0]] = true;
        ow_config_t *c = &got[e[0]];
        c->function = OW_FN_AUTOMATION;
        c->gpio = e[1];
        c->active_low = (e[2] & 0x01u) != 0u;
        c->sense = OW_SENSE_NONE;
        if (name_len != 0u) {
            name_copy(c->name, sizeof(c->name), name, name_len);
        }
    }
    if (p != end) {
        return false;
    }
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        if (seen[i] && out[i].function == OW_FN_NONE) { /* `wired`'s channel wins its slot */
            out[i] = got[i];
        }
    }
    return true;
}
