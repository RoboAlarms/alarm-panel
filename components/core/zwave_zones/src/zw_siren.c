/*
 * zw_siren.c - Z-Wave sirens as the panel's bell (zw_siren.h).
 *
 * Nothing is sent from the radio's own news (zws_send_done, zws_received, zws_heard, zws_forget):
 * they run inside the network's callbacks, so they only change what is due and the owner's next
 * poll sends it.
 */
#include "zwave_zones/zw_siren.h"

#include <string.h>

#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_cc_switch.h"

static siren_scope_t scope_of(const zws_config_t *c)
{
    const siren_scope_t s = {.partitions = c->partitions, .also = c->also, .burglary = c->burglary,
                             .fire = c->fire};
    return s;
}

static uint8_t own_bit(uint8_t partition)
{
    if (partition == 0u || partition > ALARM_MAX_PARTITIONS) {
        return 0u;
    }
    return (uint8_t)(1u << (partition - 1u));
}

bool zws_in_trouble(const zws_siren_t *s)
{
    return s->c.used && (s->unconfirmed || s->silent);
}

static void tell_trouble(zws_t *z)
{
    bool any = false;
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        any = any || zws_in_trouble(&z->s[i]);
    }
    if (any != z->trouble) {
        z->trouble = any;
        if (z->port.bell_trouble != NULL) {
            z->port.bell_trouble(z->port.ctx, any);
        }
    }
}

static uint32_t next_tag(zws_t *z, size_t slot)
{
    z->seq = (uint16_t)((unsigned)z->seq + 1u);
    return ZWS_TAG | ((uint32_t)slot << 16) | (uint32_t)z->seq;
}

/* The tone (or switch value) for a sound. */
static uint8_t value_for(const zws_config_t *c, siren_sound_t sound)
{
    if (sound == SIREN_QUIET) {
        return 0u;
    }
    if (c->kind == (uint8_t)ZWS_KIND_BINARY) {
        return ZW_SWITCH_ON;
    }
    const uint8_t burglary = c->tone_burglary != 0u ? c->tone_burglary : ZWS_TONE_DEFAULT;
    switch (sound) {
    case SIREN_FIRE: return c->tone_fire != 0u ? c->tone_fire : burglary;
    case SIREN_CO: return c->tone_co != 0u ? c->tone_co : burglary;
    default: return burglary; /* burglary, the bell test */
    }
}

size_t zws_command(const zws_config_t *c, uint8_t value, uint8_t *out, size_t cap)
{
    if (cap < 4u) {
        return 0u;
    }
    if (c->kind == (uint8_t)ZWS_KIND_BINARY) {
        out[0] = (uint8_t)ZW_CC_BINARY_SWITCH;
        out[1] = ZW_CMD_SWITCH_SET;
        out[2] = value != 0u ? ZW_SWITCH_ON : ZW_SWITCH_OFF;
        return 3u;
    }
    out[0] = (uint8_t)ZW_CC_SOUND_SWITCH;
    out[1] = ZW_CMD_SOUND_TONE_PLAY_SET;
    out[2] = value;
    if (value != ZW_SOUND_TONE_STOP && c->volume != 0u) {
        out[3] = c->volume; /* v2's volume; a v1 siren ignores it */
        return 4u;
    }
    return 3u;
}

static bool send_value(zws_t *z, size_t slot, uint8_t value, uint32_t tag)
{
    const zws_siren_t *s = &z->s[slot];
    uint8_t cmd[4];
    const size_t n = zws_command(&s->c, value, cmd, sizeof cmd);
    return n > 0u && z->port.send != NULL &&
           z->port.send(z->port.ctx, s->c.node, s->c.endpoint, cmd, n, tag);
}

static bool send_get(zws_t *z, size_t slot, uint32_t tag)
{
    const zws_siren_t *s = &z->s[slot];
    const uint8_t cmd[2] = {s->c.kind == (uint8_t)ZWS_KIND_BINARY ? (uint8_t)ZW_CC_BINARY_SWITCH
                                                                   : (uint8_t)ZW_CC_SOUND_SWITCH,
                            s->c.kind == (uint8_t)ZWS_KIND_BINARY ? ZW_CMD_SWITCH_GET
                                                                   : ZW_CMD_SOUND_TONE_PLAY_GET};
    return z->port.send != NULL &&
           z->port.send(z->port.ctx, s->c.node, s->c.endpoint, cmd, sizeof cmd, tag);
}

/* A try of `value` failed (not delivered, not queued, or never answered). */
static void failed_try(zws_siren_t *s, uint64_t now)
{
    s->tag = 0u;
    if (s->tries < UINT8_MAX) {
        s->tries++;
    }
    if (s->tries >= ZWS_SEND_TRIES) {
        s->unconfirmed = true;
        s->retry_at = now + ZWS_RETRY_SLOW_MS;
        if (s->value == 0u && s->tries >= ZWS_STOP_TRIES) {
            /* a stop nobody takes is not sent for ever: it goes again when the siren is heard
             * (the trouble stays until one is delivered) */
            s->pending = false;
        }
    } else {
        s->retry_at = now + ZWS_RETRY_MS;
    }
}

void zws_init(zws_t *z, const zws_port_t *port)
{
    memset(z, 0, sizeof(*z));
    z->port = *port;
    siren_view_init(&z->view);
}

void zws_defaults(zws_config_t *c, uint16_t node, uint16_t generation, uint8_t endpoint,
                  uint8_t kind)
{
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->node = node;
    c->generation = generation;
    c->endpoint = endpoint;
    c->kind = kind;
    c->partitions = 0x01u;
    c->burglary = true;
    c->fire = true;
    c->tone_burglary = ZWS_TONE_DEFAULT;
}

uint64_t zws_silent_limit_ms(const zws_config_t *c)
{
    const uint32_t min = c->supervision_min != 0u ? c->supervision_min : ZWS_SUPERVISION_DEF_MIN;
    return (uint64_t)min * 60000u;
}

int zws_find(const zws_t *z, uint16_t node, uint8_t endpoint)
{
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        if (z->s[i].c.used && z->s[i].c.node == node && z->s[i].c.endpoint == endpoint) {
            return (int)i;
        }
    }
    return -1;
}

int zws_slot_for(const zws_t *z, uint16_t node, uint8_t endpoint)
{
    const int own = zws_find(z, node, endpoint);
    if (own >= 0) {
        return own;
    }
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        if (!z->s[i].c.used) {
            return (int)i;
        }
    }
    return -1;
}

static bool valid(const zws_config_t *c)
{
    return c->used && c->node != 0u && c->endpoint < 128u &&
           (c->kind == (uint8_t)ZWS_KIND_SOUND || c->kind == (uint8_t)ZWS_KIND_BINARY) &&
           c->partitions != 0u && (c->burglary || c->fire) && c->volume <= ZWS_VOLUME_MAX &&
           (c->supervision_min == 0u || c->supervision_min >= ZWS_SUPERVISION_MIN_MIN);
}

zws_result_t zws_set(zws_t *z, uint64_t now, size_t slot, const zws_config_t *c)
{
    if (slot >= ZWS_SIRENS || c == NULL || !valid(c)) {
        return ZWS_ERR_ARG;
    }
    const int other = zws_find(z, c->node, c->endpoint);
    if (other >= 0 && (size_t)other != slot) {
        return ZWS_ERR_ARG; /* one assignment a siren */
    }
    zws_siren_t *s = &z->s[slot];
    const bool moved = s->c.used && (s->c.node != c->node || s->c.endpoint != c->endpoint ||
                                     s->c.generation != c->generation || s->c.kind != c->kind);
    if (moved && s->value != 0u) {
        /* the old one goes quiet first (best effort, once) */
        (void)send_value(z, slot, 0u, next_tag(z, slot));
    }
    if (!s->c.used || moved) {
        memset(s, 0, sizeof(*s));
        s->fresh = true;
    }
    s->c = *c;
    z->dirty = true;
    zws_poll(z, now);
    return ZWS_OK;
}

zws_result_t zws_clear(zws_t *z, uint64_t now, size_t slot)
{
    (void)now;
    if (slot >= ZWS_SIRENS) {
        return ZWS_ERR_ARG;
    }
    zws_siren_t *s = &z->s[slot];
    if (!s->c.used) {
        return ZWS_ERR_UNKNOWN;
    }
    if (s->value != 0u || s->pending) {
        (void)send_value(z, slot, 0u, next_tag(z, slot));
    }
    memset(s, 0, sizeof(*s));
    z->dirty = true;
    tell_trouble(z);
    return ZWS_OK;
}

bool zws_forget(zws_t *z, uint16_t node)
{
    bool any = false;
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        if (z->s[i].c.used && z->s[i].c.node == node) {
            memset(&z->s[i], 0, sizeof(z->s[i]));
            any = true;
        }
    }
    if (any) {
        z->dirty = true;
        tell_trouble(z);
    }
    return any;
}

void zws_load(zws_t *z, const zws_config_t cfg[ZWS_SIRENS])
{
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        memset(&z->s[i], 0, sizeof(z->s[i]));
        if (cfg[i].used) {
            z->s[i].c = cfg[i];
            z->s[i].fresh = true;
        }
    }
    tell_trouble(z);
}

void zws_output(zws_t *z, uint64_t now, uint8_t partition, alarm_output_t out,
                alarm_pattern_t pattern)
{
    if (out != ALARM_OUT_BELL) {
        return; /* a Sound Switch has no strobe: its sound is the bell's */
    }
    if (siren_view_output(&z->view, now, partition, out, pattern)) {
        zws_poll(z, now);
    }
}

void zws_armed(zws_t *z, uint64_t now, uint8_t partition, bool armed)
{
    if (!siren_view_armed(&z->view, partition, armed)) {
        return;
    }
    const uint8_t bit = own_bit(partition);
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        zws_siren_t *s = &z->s[i];
        const uint8_t tone = armed ? s->c.tone_arm : s->c.tone_disarm;
        if (!s->c.used || !s->c.squawk || s->c.kind != (uint8_t)ZWS_KIND_SOUND ||
            (s->c.partitions & bit) == 0u || tone == 0u || s->value != 0u || s->pending ||
            s->tag != 0u || s->test_until != 0u || !z->radio_up) {
            continue;
        }
        /* once: a chirp is never confirmed or retried (a late one would only confuse) */
        s->chirp_tag = next_tag(z, i);
        if (!send_value(z, i, tone, s->chirp_tag)) {
            s->chirp_tag = 0u;
        }
    }
    zws_poll(z, now);
}

void zws_set_armed(zws_t *z, uint8_t partition, bool armed)
{
    (void)siren_view_armed(&z->view, partition, armed);
}

void zws_radio(zws_t *z, uint64_t now, bool up)
{
    if (up == z->radio_up) {
        return;
    }
    z->radio_up = up;
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        zws_siren_t *s = &z->s[i];
        if (!s->c.used) {
            continue;
        }
        s->silent = false; /* down: the radio's own trouble; up: counted from now */
        s->heard_at = now;
        if (up && s->pending && s->tag == 0u) {
            s->retry_at = now; /* what waited goes now */
        }
    }
    tell_trouble(z);
}

zws_result_t zws_test(zws_t *z, uint64_t now, size_t slot)
{
    if (slot >= ZWS_SIRENS) {
        return ZWS_ERR_ARG;
    }
    zws_siren_t *s = &z->s[slot];
    if (!s->c.used) {
        return ZWS_ERR_UNKNOWN;
    }
    const siren_scope_t sc = scope_of(&s->c);
    if (siren_armed(&sc, &z->view)) {
        return ZWS_ERR_ARMED;
    }
    if (s->value != 0u || s->sound != SIREN_QUIET || s->test_until != 0u ||
        siren_pick(&sc, &z->view, now) != SIREN_QUIET) {
        return ZWS_ERR_BUSY;
    }
    if (!z->radio_up) {
        return ZWS_ERR_OFFLINE;
    }
    s->test_until = now + ZWS_TEST_MS;
    zws_poll(z, now);
    return ZWS_OK;
}

bool zws_send_done(zws_t *z, uint64_t now, uint32_t tag, bool delivered)
{
    if ((tag & ZWS_TAG_MASK) != ZWS_TAG) {
        return false;
    }
    const size_t slot = (tag >> 16) & 0xFFu;
    if (slot >= ZWS_SIRENS || !z->s[slot].c.used) {
        return true; /* a siren's since cleared: nothing waits for it */
    }
    zws_siren_t *s = &z->s[slot];
    if (tag == s->tag) {
        s->tag = 0u;
        if (delivered) {
            s->pending = false;
            s->tries = 0u;
            s->unconfirmed = false;
            s->sets++;
            s->check_at = now + ZWS_CHECK_MS;
        } else {
            failed_try(s, now);
        }
        tell_trouble(z);
    } else if (tag == s->get_tag) {
        s->get_tag = 0u; /* its Report says the rest; silence is the supervision's */
    } else if (tag == s->chirp_tag) {
        s->chirp_tag = 0u;
    }
    return true;
}

void zws_heard(zws_t *z, uint64_t now, uint16_t node)
{
    bool changed = false;
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        zws_siren_t *s = &z->s[i];
        if (!s->c.used || s->c.node != node) {
            continue;
        }
        s->heard_at = now;
        s->fresh = false;
        if (s->silent) {
            s->silent = false;
            changed = true;
        }
        if (s->unconfirmed && s->tag == 0u) {
            /* it is there again: what waited (a stop given up too) goes at once */
            if (!s->pending) {
                s->pending = true;
                s->tries = 0u;
            }
            s->retry_at = now;
        }
    }
    if (changed) {
        tell_trouble(z);
    }
}

bool zws_received(zws_t *z, uint64_t now, uint16_t node, uint8_t endpoint, const uint8_t *cmd,
                  size_t len)
{
    if (cmd == NULL || len < 2u) {
        return false;
    }
    const int i = zws_find(z, node, endpoint);
    if (i < 0) {
        return false;
    }
    zws_siren_t *s = &z->s[i];
    const bool sound = s->c.kind == (uint8_t)ZWS_KIND_SOUND;
    const uint8_t cc = sound ? (uint8_t)ZW_CC_SOUND_SWITCH : (uint8_t)ZW_CC_BINARY_SWITCH;
    const uint8_t report = sound ? ZW_CMD_SOUND_TONE_PLAY_REPORT : ZW_CMD_SWITCH_REPORT;
    if (cmd[0] != cc) {
        return false;
    }
    if (cmd[1] != report) {
        return true; /* the siren's own class: nobody else's to take */
    }
    if (len < 3u) {
        return true;
    }
    s->reports++;
    s->get_tag = 0u;
    const bool quiet = cmd[2] == 0x00u; /* tone 00 / switch off (FE: unknown, not quiet) */
    if (quiet && s->value != 0u && !s->pending && s->tag == 0u) {
        /* it stopped on its own (its timer) while the bell still sounds: the sound again */
        s->pending = true;
        s->tries = 0u;
        s->retry_at = now;
    }
    return true;
}

void zws_poll(zws_t *z, uint64_t now)
{
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        zws_siren_t *s = &z->s[i];
        if (!s->c.used) {
            continue;
        }
        if (s->fresh) {
            s->fresh = false;
            s->heard_at = now;
        }
        if (s->tag != 0u && now >= s->sent_at + ZWS_CONFIRM_MS) {
            failed_try(s, now); /* the radio never told its end: a failed try */
        }
        const siren_scope_t sc = scope_of(&s->c);
        const siren_sound_t want = siren_pick(&sc, &z->view, now);
        if (want != SIREN_QUIET) {
            s->test_until = 0u; /* a test gives way to the alarm */
        } else if (s->test_until != 0u && now >= s->test_until) {
            s->test_until = 0u;
        }
        s->sound = want;
        const uint8_t value =
            value_for(&s->c, want != SIREN_QUIET ? want
                                                 : (s->test_until != 0u ? SIREN_BURGLARY
                                                                        : SIREN_QUIET));
        if (value != s->value) {
            s->value = value;
            s->pending = true;
            s->tries = 0u;
            s->tag = 0u; /* an older Set's end no longer counts */
            s->retry_at = now;
        }
        if (s->pending && s->tag == 0u && now >= s->retry_at) {
            const uint32_t tag = next_tag(z, i);
            if (send_value(z, i, s->value, tag)) {
                s->tag = tag;
                s->sent_at = now;
            } else {
                failed_try(s, now);
            }
        }
        if (!s->pending && s->value != 0u && now >= s->check_at) {
            /* OUT-017: still sounding? */
            s->check_at = now + ZWS_CHECK_MS;
            const uint32_t tag = next_tag(z, i);
            s->get_tag = send_get(z, i, tag) ? tag : 0u;
        }
        s->silent = z->radio_up && now - s->heard_at >= zws_silent_limit_ms(&s->c);
    }
    tell_trouble(z);
}

static void earliest(uint64_t *best, uint64_t at)
{
    if (at < *best) {
        *best = at;
    }
}

uint64_t zws_next_deadline(const zws_t *z, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        const zws_siren_t *s = &z->s[i];
        if (!s->c.used) {
            continue;
        }
        if (s->fresh) {
            return now;
        }
        if (s->tag != 0u) {
            earliest(&next, s->sent_at + ZWS_CONFIRM_MS);
        } else if (s->pending) {
            earliest(&next, s->retry_at);
        }
        if (!s->pending && s->value != 0u) {
            earliest(&next, s->check_at);
        }
        if (s->test_until != 0u) {
            earliest(&next, s->test_until);
        }
        if (z->radio_up && !s->silent) {
            earliest(&next, s->heard_at + zws_silent_limit_ms(&s->c));
        }
        const siren_scope_t sc = scope_of(&s->c);
        const uint64_t v = siren_view_deadline(&sc, &z->view, now);
        if (v != 0u) {
            earliest(&next, v);
        }
    }
    return next;
}

/* ---- the store ---------------------------------------------------------------------------------- */

size_t zws_encode(const zws_t *z, uint8_t *out, size_t cap)
{
    if (cap < ZWS_BLOB_MAX) {
        return 0u;
    }
    size_t n = 0u;
    out[n++] = 'Z';
    out[n++] = 'S';
    out[n++] = ZWS_FORMAT;
    const size_t count_at = n++;
    uint8_t count = 0u;
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        const zws_config_t *c = &z->s[i].c;
        if (!c->used) {
            continue;
        }
        out[n++] = ZWS_ENTRY;
        out[n++] = (uint8_t)i;
        out[n++] = (uint8_t)(c->node >> 8);
        out[n++] = (uint8_t)(c->node & 0xFFu);
        out[n++] = (uint8_t)(c->generation >> 8);
        out[n++] = (uint8_t)(c->generation & 0xFFu);
        out[n++] = c->endpoint;
        out[n++] = c->kind;
        out[n++] = c->partitions;
        out[n++] = c->also;
        out[n++] = (uint8_t)((c->burglary ? 0x01u : 0u) | (c->fire ? 0x02u : 0u) |
                             (c->squawk ? 0x04u : 0u));
        out[n++] = c->tone_burglary;
        out[n++] = c->tone_fire;
        out[n++] = c->tone_co;
        out[n++] = c->tone_arm;
        out[n++] = c->tone_disarm;
        out[n++] = c->volume;
        out[n++] = (uint8_t)(c->supervision_min >> 8);
        out[n++] = (uint8_t)(c->supervision_min & 0xFFu);
        count++;
    }
    out[count_at] = count;
    const uint16_t crc = siren_crc16(out, n);
    out[n++] = (uint8_t)(crc >> 8);
    out[n++] = (uint8_t)(crc & 0xFFu);
    return n;
}

bool zws_decode(const uint8_t *buf, size_t len, zws_config_t out[ZWS_SIRENS])
{
    if (buf == NULL || len < 6u || buf[0] != 'Z' || buf[1] != 'S' || buf[2] == 0u ||
        buf[2] > ZWS_FORMAT) {
        return false;
    }
    const uint16_t crc = (uint16_t)(((unsigned)buf[len - 2u] << 8) | buf[len - 1u]);
    if (siren_crc16(buf, len - 2u) != crc) {
        return false;
    }
    zws_config_t got[ZWS_SIRENS];
    memset(got, 0, sizeof(got));
    const size_t end = len - 2u;
    size_t p = 4u;
    for (unsigned k = 0; k < buf[3]; k++) {
        if (p >= end) {
            return false;
        }
        const size_t elen = buf[p++];
        if (elen < 3u || p + elen > end) {
            return false;
        }
        uint8_t e[ZWS_ENTRY];
        memset(e, 0, sizeof(e));
        memcpy(e, &buf[p], elen < ZWS_ENTRY ? elen : ZWS_ENTRY);
        p += elen;
        if (e[0] >= ZWS_SIRENS || got[e[0]].used) {
            return false;
        }
        zws_config_t *c = &got[e[0]];
        c->used = true;
        c->node = (uint16_t)(((unsigned)e[1] << 8) | e[2]);
        c->generation = (uint16_t)(((unsigned)e[3] << 8) | e[4]);
        c->endpoint = e[5];
        c->kind = e[6];
        c->partitions = e[7];
        c->also = e[8];
        c->burglary = (e[9] & 0x01u) != 0u;
        c->fire = (e[9] & 0x02u) != 0u;
        c->squawk = (e[9] & 0x04u) != 0u;
        c->tone_burglary = e[10];
        c->tone_fire = e[11];
        c->tone_co = e[12];
        c->tone_arm = e[13];
        c->tone_disarm = e[14];
        c->volume = e[15];
        c->supervision_min = (uint16_t)(((unsigned)e[16] << 8) | e[17]);
        if (c->node == 0u) {
            return false;
        }
    }
    if (p != end) {
        return false;
    }
    memcpy(out, got, sizeof(got));
    return true;
}
