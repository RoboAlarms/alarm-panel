/*
 * zbz_ace.c - Zigbee keypads and key fobs arming and disarming the panel (zbz_ace.h;
 * docs/features/20 alarm-devices.md §3-§4; ZB-024, BL-148). The panel is the IAS ACE server
 * (cluster 0x0501): the client commands are decoded here, the server's frames encoded, and an
 * assigned keypad's Arm or panic button is turned into an alarm_cmd_t for the alarm task, whose
 * answer comes back by ticket.
 *
 * Nothing here compares a code: a keypad's code is copied once into the post and every other
 * copy (the decoded frame on the stack) is wiped. Every read is bounds-checked against the
 * frame's own length; nothing is written past ZSRC_ZB_ZCL_PAYLOAD_MAX.
 */
#include "zigbee_zones/zbz_ace.h"

#include <string.h>

#include "zigbee_zones/zbz_models.h"

#define NOTE_NONE      0xFFu
#define ENDPOINT_MAX   240u   /* application endpoints 1..240 (0 is ZDO) */
#define PANIC_KNOWN    0x0Fu  /* 1 << alarm_panic_t of the four kinds there are */
#define KIND_KEYPAD    1u
#define KIND_FOB       2u
#define KEYPAD_FIELDS  (ZBA_KEYPAD_ENTRY - 1u) /* after the length byte */
#define FOB_FIELDS     (ZBA_FOB_ENTRY - 1u)

/* ---- small helpers ----------------------------------------------------------------------------- */

static void erase(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n != 0u) {
        *v++ = 0u;
        n--;
    }
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (size_t i = 8; i > 0; i--) {
        v = (v << 8) | (uint64_t)(unsigned)p[i - 1u];
    }
    return v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    for (size_t i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}

static bool delay_status(uint8_t status)
{
    return status == ZBA_PS_EXIT_DELAY || status == ZBA_PS_ENTRY_DELAY;
}

/* ---- the frames -------------------------------------------------------------------------------- */

/* A server-to-client ACE frame that asks for no Default Response (alarm-devices.md §3). */
static void server_frame(uint8_t command, uint16_t tsn, zsrc_zb_zcl_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cluster = ZSRC_ZB_ZCL_IAS_ACE;
    out->command = command;
    out->server_to_client = true;
    out->want_response = false;
    out->manufacturer = 0;
    out->tsn = tsn;
}

/* A client frame of the standard ACE commands. */
static zbz_zcl_result_t client_frame(const zsrc_zb_zcl_t *f)
{
    if (f->cluster != ZSRC_ZB_ZCL_IAS_ACE || f->server_to_client || f->manufacturer != 0u ||
        f->command > ZBA_CMD_GET_ZONE_STATUS) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return ZBZ_ZCL_ERR_LONG;
    }
    return ZBZ_ZCL_OK;
}

zbz_zcl_result_t zba_decode_arm(const zsrc_zb_zcl_t *f, zba_arm_t *out)
{
    if (f == NULL || out == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    memset(out, 0, sizeof(*out));
    zbz_zcl_result_t r = client_frame(f);
    if (r != ZBZ_ZCL_OK) {
        return r;
    }
    if (f->command != ZBA_CMD_ARM) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len < 2u) {
        return ZBZ_ZCL_ERR_SHORT; /* the mode and the code's length byte are needed */
    }
    out->mode = f->payload[0];
    size_t n = f->payload[1];
    if (n == 0xFFu) {
        n = 0; /* an invalid string: no characters follow */
    }
    if (2u + n > (size_t)f->len) {
        out->code_bad = true; /* the length runs past the frame: never truncated */
        return ZBZ_ZCL_OK;
    }
    if (n > ALARM_CODE_MAX_LEN) {
        out->code_bad = true; /* longer than any code the panel has */
    } else {
        for (size_t i = 0; i < n; i++) {
            uint8_t c = f->payload[2u + i];
            if (c < (uint8_t)'0' || c > (uint8_t)'9') {
                out->code_bad = true; /* codes are digits; nothing else is passed on */
                break;
            }
        }
        if (!out->code_bad && n > 0u) {
            memcpy(out->code, &f->payload[2], n);
        }
    }
    if (2u + n < (size_t)f->len) {
        out->zone_id = f->payload[2u + n];
    }
    return ZBZ_ZCL_OK;
}

void zba_arm_response(uint8_t note, uint8_t tsn, zsrc_zb_zcl_t *out)
{
    if (out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_ARM, tsn, out);
    out->payload[0] = note;
    out->len = 1;
}

static void put_status(const zba_status_t *s, zsrc_zb_zcl_t *out)
{
    out->payload[0] = s->status;
    out->payload[1] = s->seconds;
    out->payload[2] = s->audible;
    out->payload[3] = s->alarm;
    out->len = 4;
}

void zba_panel_status_changed(const zba_status_t *s, zsrc_zb_zcl_t *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_PANEL_STATUS_CHANGED, ZSRC_ZB_TSN_NEW, out);
    put_status(s, out);
}

void zba_panel_status_response(const zba_status_t *s, uint8_t tsn, zsrc_zb_zcl_t *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_PANEL_STATUS, tsn, out);
    put_status(s, out);
}

zbz_zcl_result_t zba_decode_panel_status(const zsrc_zb_zcl_t *f, zba_status_t *out)
{
    if (f == NULL || out == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    memset(out, 0, sizeof(*out));
    if (f->cluster != ZSRC_ZB_ZCL_IAS_ACE || !f->server_to_client || f->manufacturer != 0u ||
        (f->command != ZBA_RSP_PANEL_STATUS_CHANGED && f->command != ZBA_RSP_PANEL_STATUS)) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return ZBZ_ZCL_ERR_LONG;
    }
    if (f->len < 4u) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    uint8_t st = f->payload[0];
    uint8_t al = f->payload[3];
    if ((st > ZBA_PS_ARMING_AWAY && st != ZBA_PS_ARMED_80) || al > ZBA_ALARM_EMERGENCY_PANIC) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    out->status = st;
    out->seconds = f->payload[1];
    out->audible = f->payload[2];
    out->alarm = al;
    return ZBZ_ZCL_OK;
}

zbz_zcl_result_t zba_decode_request(const zsrc_zb_zcl_t *f, zba_request_t *out)
{
    if (f == NULL || out == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    memset(out, 0, sizeof(*out));
    zbz_zcl_result_t r = client_frame(f);
    if (r != ZBZ_ZCL_OK) {
        return r;
    }
    if (f->command == ZBA_CMD_ARM) {
        return ZBZ_ZCL_ERR_TYPE; /* zba_decode_arm's */
    }
    const uint8_t *p = f->payload;
    size_t len = f->len;
    switch (f->command) {
    case ZBA_CMD_BYPASS: {
        /* number of zones | zone ids | arm/disarm code (a ZCL string): the code is only
         * stepped over - bypassing stays the panel's own keypads', so it is never needed. */
        if (len < 1u) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        size_t count = p[0];
        if (1u + count + 1u > len) {
            return ZBZ_ZCL_ERR_SHORT; /* the zones, or the code's length byte, missing */
        }
        size_t code = p[1u + count];
        if (code == 0xFFu) {
            code = 0;
        }
        if (1u + count + 1u + code > len) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        out->command = f->command;
        out->count = (uint8_t)count;
        if (count > 0u) {
            memcpy(out->zones, &p[1], count);
        }
        return ZBZ_ZCL_OK;
    }
    case ZBA_CMD_GET_ZONE_INFO:
        if (len < 1u) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        out->zone_id = p[0];
        break;
    case ZBA_CMD_GET_ZONE_STATUS:
        if (len < 5u) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        out->zone_id = p[0];
        out->max_zones = p[1];
        out->mask_flag = p[2] != 0u;
        out->status_mask = rd16(&p[3]);
        break;
    default:
        break; /* Emergency, Fire, Panic and the other questions carry no fields */
    }
    out->command = f->command;
    return ZBZ_ZCL_OK;
}

void zba_zone_id_map_response(const uint16_t *sections, uint8_t tsn, zsrc_zb_zcl_t *out)
{
    if (out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_ZONE_ID_MAP, tsn, out);
    for (size_t i = 0; i < ZBA_ZONE_MAP_SECTIONS; i++) {
        wr16(&out->payload[2u * i], sections != NULL ? sections[i] : 0u);
    }
    out->len = (uint8_t)(2u * ZBA_ZONE_MAP_SECTIONS);
}

/* A ZCL character string at payload[at]: its length byte, then at most ZBA_LABEL_MAX characters
 * (and no more than the payload holds). The new length. */
static uint8_t put_label(zsrc_zb_zcl_t *out, size_t at, const char *label)
{
    size_t n = label != NULL ? strlen(label) : 0u;
    if (n > ZBA_LABEL_MAX) {
        n = ZBA_LABEL_MAX;
    }
    if (at + 1u + n > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        n = ZSRC_ZB_ZCL_PAYLOAD_MAX - at - 1u;
    }
    out->payload[at] = (uint8_t)n;
    if (n > 0u) {
        memcpy(&out->payload[at + 1u], label, n);
    }
    return (uint8_t)(at + 1u + n);
}

void zba_zone_info_response(const zba_zone_info_t *z, uint8_t tsn, zsrc_zb_zcl_t *out)
{
    if (z == NULL || out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_ZONE_INFO, tsn, out);
    out->payload[0] = z->zone_id;
    wr16(&out->payload[1], z->zone_type);
    wr64(&out->payload[3], z->ieee);
    out->len = put_label(out, 11u, z->label);
}

void zba_zone_status_changed(uint8_t zone_id, uint16_t zone_status, uint8_t audible,
                             const char *label, zsrc_zb_zcl_t *out)
{
    if (out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_ZONE_STATUS_CHANGED, ZSRC_ZB_TSN_NEW, out);
    out->payload[0] = zone_id;
    wr16(&out->payload[1], zone_status);
    out->payload[3] = audible;
    out->len = put_label(out, 4u, label);
}

void zba_set_bypassed(const uint8_t *zones, uint8_t count, uint16_t tsn, zsrc_zb_zcl_t *out)
{
    if (out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_SET_BYPASSED, tsn, out);
    size_t n = zones != NULL ? count : 0u;
    if (n > ZSRC_ZB_ZCL_PAYLOAD_MAX - 1u) {
        n = ZSRC_ZB_ZCL_PAYLOAD_MAX - 1u;
    }
    out->payload[0] = (uint8_t)n;
    if (n > 0u) {
        memcpy(&out->payload[1], zones, n);
    }
    out->len = (uint8_t)(1u + n);
}

void zba_bypass_response(const uint8_t *results, uint8_t count, uint8_t tsn, zsrc_zb_zcl_t *out)
{
    zba_set_bypassed(results, count, tsn, out); /* the same shape: a count, then a byte each */
    if (out != NULL) {
        out->command = ZBA_RSP_BYPASS;
    }
}

void zba_zone_status_response(bool complete, const zba_zone_status_t *z, uint8_t count,
                              uint8_t tsn, zsrc_zb_zcl_t *out)
{
    if (out == NULL) {
        return;
    }
    server_frame(ZBA_RSP_ZONE_STATUS, tsn, out);
    size_t n = z != NULL ? count : 0u;
    if (n > ZBA_ZONE_STATUS_MAX) {
        n = ZBA_ZONE_STATUS_MAX;
    }
    out->payload[0] = complete ? 1u : 0u;
    out->payload[1] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out->payload[2u + 3u * i] = z[i].zone_id;
        wr16(&out->payload[3u + 3u * i], z[i].status);
    }
    out->len = (uint8_t)(2u + 3u * n);
}

/* ---- the panel's side, as data ----------------------------------------------------------------- */

/* A silent alarm never shows on a keypad: whoever stands at it may be the reason for it. */
static bool silent_cause(uint8_t cause)
{
    return cause == (uint8_t)ALARM_CAUSE_SILENT_BURGLARY || cause == (uint8_t)ALARM_CAUSE_24H_SILENT ||
           cause == (uint8_t)ALARM_CAUSE_PANIC_SILENT;
}

static uint8_t alarm_status_of(uint8_t cause)
{
    switch (cause) {
    case ALARM_CAUSE_BURGLARY:
    case ALARM_CAUSE_EXIT_ERROR:
    case ALARM_CAUSE_TAMPER:
    case ALARM_CAUSE_24H_AUDIBLE:
        return ZBA_ALARM_BURGLAR;
    case ALARM_CAUSE_FIRE:
        return ZBA_ALARM_FIRE;
    case ALARM_CAUSE_PANIC_POLICE:
        return ZBA_ALARM_POLICE_PANIC;
    case ALARM_CAUSE_PANIC_FIRE:
        return ZBA_ALARM_FIRE_PANIC;
    case ALARM_CAUSE_PANIC_MEDICAL:
        return ZBA_ALARM_EMERGENCY_PANIC;
    default:
        return ZBA_ALARM_EMERGENCY; /* CO, the auxiliary 24-hour zones, anything else */
    }
}

static uint8_t seconds_left(const zba_view_t *v, uint64_t now)
{
    uint64_t total = (uint64_t)v->delay_s * 1000u;
    uint64_t gone = now > v->at ? now - v->at : 0u;
    if (gone >= total) {
        return 0;
    }
    uint64_t s = (total - gone + 999u) / 1000u; /* 30 s just begun reads 30 */
    return (uint8_t)(s > 255u ? 255u : s);
}

bool zba_fob_may_be(alarm_authority_t authority)
{
    return authority == ALARM_AUTH_MASTER || authority == ALARM_AUTH_PARTITION_MASTER ||
           authority == ALARM_AUTH_STANDARD || authority == ALARM_AUTH_ARM_ONLY ||
           authority == ALARM_AUTH_GUEST;
}

void zba_status_for(const zba_view_t *v, uint64_t now, uint16_t model_flags, zba_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (v == NULL) {
        out->status = ZBA_PS_NOT_READY; /* nothing known yet: never "disarmed and ready" */
        return;
    }
    if (v->in_alarm && !silent_cause(v->cause)) {
        out->status = ZBA_PS_IN_ALARM;
        out->alarm = alarm_status_of(v->cause);
        out->audible = ZBA_AUDIBLE_DEFAULT;
    } else {
        switch (v->state) {
        case ALARM_STATE_DISARMED:
            out->status = v->ready ? ZBA_PS_DISARMED : ZBA_PS_NOT_READY;
            break;
        case ALARM_STATE_EXIT_DELAY:
            out->status = ZBA_PS_EXIT_DELAY;
            out->seconds = seconds_left(v, now);
            out->audible = ZBA_AUDIBLE_DEFAULT;
            break;
        case ALARM_STATE_ENTRY_DELAY:
            out->status = ZBA_PS_ENTRY_DELAY;
            out->seconds = seconds_left(v, now);
            out->audible = ZBA_AUDIBLE_DEFAULT;
            break;
        case ALARM_STATE_ARMED:
            out->status = v->level == (uint8_t)ALARM_LEVEL_STAY    ? ZBA_PS_ARMED_STAY
                          : v->level == (uint8_t)ALARM_LEVEL_NIGHT ? ZBA_PS_ARMED_NIGHT
                                                                   : ZBA_PS_ARMED_AWAY;
            break;
        default:
            out->status = ZBA_PS_NOT_READY;
            break;
        }
    }
    if ((model_flags & ZBZ_MODEL_ACE_AUDIBLE) != 0u) {
        out->audible = ZBA_AUDIBLE_DEFAULT; /* Frient's keypad beeps only with it */
    }
    if ((model_flags & ZBZ_MODEL_ACE_ARMED_80) != 0u) {
        /* Centralite 3400-D (provisional, as zigbee-herdsman-converters send it): one "armed"
         * for every armed state, 0 for disarmed, not ready and the exit delay. */
        switch (out->status) {
        case ZBA_PS_ARMED_STAY:
        case ZBA_PS_ARMED_NIGHT:
        case ZBA_PS_ARMED_AWAY:
        case ZBA_PS_ENTRY_DELAY:
        case ZBA_PS_IN_ALARM:
            out->status = ZBA_PS_ARMED_80;
            break;
        default:
            out->status = ZBA_PS_DISARMED;
            break;
        }
        out->seconds = 0;
    }
}

uint8_t zba_note_for(uint8_t mode, alarm_result_t result, const zba_view_t *before)
{
    if (mode > ZBA_ARM_ALL) {
        return ZBA_NOTE_NOT_READY;
    }
    switch (result) {
    case ALARM_OK:
        switch (mode) {
        case ZBA_ARM_DISARM:
            if (before != NULL && before->state == (uint8_t)ALARM_STATE_DISARMED &&
                !before->in_alarm) {
                return ZBA_NOTE_ALREADY_DISARMED;
            }
            return ZBA_NOTE_DISARMED;
        case ZBA_ARM_DAY:
            return ZBA_NOTE_DAY_ARMED;
        case ZBA_ARM_NIGHT:
            return ZBA_NOTE_NIGHT_ARMED;
        default:
            return ZBA_NOTE_ALL_ARMED;
        }
    case ALARM_ERR_INVALID_CODE:
    case ALARM_ERR_CODE_REQUIRED:
    case ALARM_ERR_LOCKED_OUT:
    case ALARM_ERR_NOT_AUTHORIZED:
    case ALARM_ERR_CODE_IN_USE:
        return ZBA_NOTE_INVALID_CODE;
    default:
        return ZBA_NOTE_NOT_READY; /* not ready, held, a bad state, unavailable */
    }
}

/* ---- the policy: lookups ----------------------------------------------------------------------- */

static zba_keypad_t *keypad_at(zba_t *a, uint64_t ieee, uint8_t endpoint)
{
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        if (a->keypad[i].used && a->keypad[i].ieee == ieee && a->keypad[i].endpoint == endpoint) {
            return &a->keypad[i];
        }
    }
    return NULL;
}

static zba_fob_t *fob_at(zba_t *a, uint64_t ieee, uint8_t endpoint)
{
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        if (a->fob[i].used && a->fob[i].ieee == ieee && a->fob[i].endpoint == endpoint) {
            return &a->fob[i];
        }
    }
    return NULL;
}

static zba_keypad_t *keypad_id(zba_t *a, uint16_t id)
{
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        if (a->keypad[i].used && a->keypad[i].id == id) {
            return &a->keypad[i];
        }
    }
    return NULL;
}

static zba_fob_t *fob_id(zba_t *a, uint16_t id)
{
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        if (a->fob[i].used && a->fob[i].id == id) {
            return &a->fob[i];
        }
    }
    return NULL;
}

/* The posts and the retransmission memory of a device go (cleared, replaced, re-assigned). */
static void forget_device(zba_t *a, uint64_t ieee)
{
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        if (a->pending[i].used && a->pending[i].ieee == ieee) {
            erase(&a->pending[i], sizeof(a->pending[i]));
        }
    }
    for (size_t i = 0; i < ZBA_RECENT; i++) {
        if (a->recent[i].used && a->recent[i].ieee == ieee) {
            erase(&a->recent[i], sizeof(a->recent[i]));
        }
    }
}

static void expire(zba_t *a, uint64_t now)
{
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        zba_pending_t *p = &a->pending[i];
        if (p->used && now >= p->at && now - p->at >= ZBA_ANSWER_MS) {
            erase(p, sizeof(*p));
        }
    }
    for (size_t i = 0; i < ZBA_RECENT; i++) {
        zba_recent_t *r = &a->recent[i];
        if (r->used && now >= r->at && now - r->at >= ZBA_RETRY_WINDOW_MS) {
            erase(r, sizeof(*r));
        }
    }
}

/* ---- the policy: sending ----------------------------------------------------------------------- */

static uint32_t next_tag(zba_t *a)
{
    a->seq++;
    return ZBA_TAG | (uint32_t)a->seq;
}

static bool send(zba_t *a, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *f)
{
    if (a->port.send == NULL) {
        return false;
    }
    return a->port.send(a->port.ctx, ieee, endpoint, f, next_tag(a));
}

static bool refuse(zba_t *a, uint64_t ieee, uint8_t endpoint, uint8_t tsn, uint8_t command,
                   uint8_t status)
{
    if (a->port.default_rsp == NULL) {
        return false; /* the owner answers through the stack (zba_port_t) */
    }
    return a->port.default_rsp(a->port.ctx, ieee, endpoint, tsn, command, status, next_tag(a));
}

static void answer_arm(zba_t *a, uint64_t ieee, uint8_t endpoint, uint8_t tsn, uint8_t note)
{
    zsrc_zb_zcl_t f;
    zba_arm_response(note, tsn, &f);
    (void)send(a, ieee, endpoint, &f);
}

/* The same command again: its device, sequence number and command - an Arm also its mode (a fob
 * that reuses a sequence number for another button is a new command). */
static bool same_command(uint64_t ieee, uint8_t tsn, uint8_t command, uint8_t mode,
                         uint64_t ieee2, uint8_t tsn2, uint8_t command2, uint8_t mode2)
{
    return ieee == ieee2 && tsn == tsn2 && command == command2 &&
           (command != ZBA_CMD_ARM || mode == mode2);
}

static void remember(zba_t *a, uint64_t now, uint64_t ieee, uint8_t endpoint, uint8_t tsn,
                     uint8_t command, uint8_t mode, uint8_t note, bool answered)
{
    zba_recent_t *slot = NULL;
    for (size_t i = 0; i < ZBA_RECENT && slot == NULL; i++) {
        zba_recent_t *r = &a->recent[i];
        if (r->used && same_command(ieee, tsn, command, mode, r->ieee, r->tsn, r->command, r->mode)) {
            slot = r;
        }
    }
    if (slot == NULL) {
        slot = &a->recent[a->next_recent];
        a->next_recent = (uint8_t)((a->next_recent + 1u) % ZBA_RECENT);
    }
    *slot = (zba_recent_t){.used = true, .ieee = ieee, .tsn = tsn, .at = now, .note = note,
                           .endpoint = endpoint, .command = command, .answered = answered,
                           .mode = command == ZBA_CMD_ARM ? mode : 0u};
}

static const zba_view_t *view_of(const zba_t *a, uint8_t partition)
{
    return &a->view[partition - 1u];
}

static bool greet(zba_t *a, uint64_t now, zba_keypad_t *k)
{
    zba_status_t s;
    zsrc_zb_zcl_t f;
    zba_status_for(view_of(a, k->partition), now, k->model_flags, &s);
    zba_panel_status_changed(&s, &f);
    if (!send(a, k->ieee, k->endpoint, &f)) {
        return false;
    }
    k->greeted = true;
    k->shown = s;
    k->shown_valid = true;
    return true;
}

/* ---- the policy: assignments ------------------------------------------------------------------- */

void zba_init(zba_t *a, const zba_port_t *port)
{
    if (a == NULL) {
        return;
    }
    const zba_port_t saved = port != NULL ? *port : (zba_port_t){0};
    erase(a, sizeof(*a));
    a->port = saved;
    for (size_t p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        a->view[p].state = (uint8_t)ALARM_STATE_DISARMED; /* not ready until the alarm task says */
    }
}

uint32_t zba_devices_held(const zba_t *a)
{
    uint32_t held = 0;
    if (a == NULL) {
        return 0;
    }
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        uint8_t d = a->keypad[i].device;
        if (a->keypad[i].used && d >= ZBA_DEVICE_FIRST && d <= ZBA_DEVICE_LAST) {
            held |= 1u << (unsigned)(d - 1u);
        }
    }
    return held;
}

uint8_t zba_free_device(const zba_t *a, uint32_t taken)
{
    uint32_t held = zba_devices_held(a) | taken;
    for (unsigned d = ZBA_DEVICE_FIRST; d <= ZBA_DEVICE_LAST; d++) {
        if ((held & (1u << (d - 1u))) == 0u) {
            return (uint8_t)d;
        }
    }
    return 0;
}

static bool usable(uint16_t id, uint64_t ieee, uint8_t endpoint, uint8_t partition)
{
    return id != 0u && ieee != 0u && endpoint >= 1u && endpoint <= ENDPOINT_MAX &&
           partition >= 1u && partition <= ALARM_MAX_PARTITIONS;
}

static bool keypad_ok(const zba_keypad_t *k)
{
    return usable(k->id, k->ieee, k->endpoint, k->partition) && k->device >= ZBA_DEVICE_FIRST &&
           k->device <= ZBA_DEVICE_LAST && (k->panic_mask & (uint8_t)~PANIC_KNOWN) == 0u;
}

static bool fob_ok(const zba_fob_t *f)
{
    return usable(f->id, f->ieee, f->endpoint, f->partition) && f->user >= 1u &&
           f->user <= ALARM_MAX_USERS;
}

zba_result_t zba_assign_keypad(zba_t *a, const zba_keypad_t *k)
{
    if (a == NULL || k == NULL || !keypad_ok(k)) {
        return ZBA_ERR_ARG;
    }
    zba_keypad_t *slot = keypad_id(a, k->id);
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        const zba_keypad_t *o = &a->keypad[i];
        if (o->used && o != slot && (o->device == k->device || o->ieee == k->ieee)) {
            return ZBA_ERR_IN_USE;
        }
    }
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        const zba_fob_t *o = &a->fob[i];
        if (o->used && o->id != k->id && o->ieee == k->ieee) {
            return ZBA_ERR_IN_USE;
        }
    }
    for (size_t i = 0; i < ZBA_KEYPADS_MAX && slot == NULL; i++) {
        if (!a->keypad[i].used) {
            slot = &a->keypad[i];
        }
    }
    if (slot == NULL) {
        return ZBA_ERR_FULL;
    }
    zba_fob_t *was_fob = fob_id(a, k->id);
    if (was_fob != NULL) {
        forget_device(a, was_fob->ieee);
        erase(was_fob, sizeof(*was_fob));
    }
    if (slot->used) {
        forget_device(a, slot->ieee);
    }
    *slot = *k;
    slot->used = true;
    slot->greeted = false;
    slot->shown_valid = false;
    memset(&slot->shown, 0, sizeof(slot->shown));
    a->dirty = true;
    return ZBA_OK;
}

zba_result_t zba_assign_fob(zba_t *a, const zba_fob_t *f)
{
    if (a == NULL || f == NULL || !fob_ok(f)) {
        return ZBA_ERR_ARG;
    }
    zba_fob_t *slot = fob_id(a, f->id);
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        const zba_fob_t *o = &a->fob[i];
        if (o->used && o != slot && (o->user == f->user || o->ieee == f->ieee)) {
            return ZBA_ERR_IN_USE; /* one fob for each user, one assignment for each device */
        }
    }
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        const zba_keypad_t *o = &a->keypad[i];
        if (o->used && o->id != f->id && o->ieee == f->ieee) {
            return ZBA_ERR_IN_USE;
        }
    }
    for (size_t i = 0; i < ZBA_FOBS_MAX && slot == NULL; i++) {
        if (!a->fob[i].used) {
            slot = &a->fob[i];
        }
    }
    if (slot == NULL) {
        return ZBA_ERR_FULL;
    }
    zba_keypad_t *was_keypad = keypad_id(a, f->id);
    if (was_keypad != NULL) {
        forget_device(a, was_keypad->ieee);
        erase(was_keypad, sizeof(*was_keypad)); /* its device number is free */
    }
    if (slot->used) {
        forget_device(a, slot->ieee);
    }
    *slot = *f;
    slot->used = true;
    a->dirty = true;
    return ZBA_OK;
}

zba_result_t zba_clear(zba_t *a, uint16_t id)
{
    if (a == NULL || id == 0u) {
        return ZBA_ERR_UNKNOWN;
    }
    zba_keypad_t *k = keypad_id(a, id);
    if (k != NULL) {
        forget_device(a, k->ieee);
        erase(k, sizeof(*k));
        a->dirty = true;
        return ZBA_OK;
    }
    zba_fob_t *f = fob_id(a, id);
    if (f != NULL) {
        forget_device(a, f->ieee);
        erase(f, sizeof(*f));
        a->dirty = true;
        return ZBA_OK;
    }
    return ZBA_ERR_UNKNOWN;
}

/* ---- the policy: commands ---------------------------------------------------------------------- */

static zba_pending_t *pending_free(zba_t *a)
{
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        if (!a->pending[i].used) {
            return &a->pending[i];
        }
    }
    return NULL;
}

/* No free slot for a panic: the oldest waiting Arm gives its slot up (a panic always gets
 * through). Its device's retransmission is remembered as unanswered - neither answered nor run
 * again - and the alarm task's answer to it finds no ticket. NULL: only panics wait. */
static zba_pending_t *pending_for_panic(zba_t *a, uint64_t now)
{
    zba_pending_t *oldest = NULL;
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        zba_pending_t *p = &a->pending[i];
        if (p->used && p->command == ZBA_CMD_ARM && (oldest == NULL || p->at < oldest->at)) {
            oldest = p;
        }
    }
    if (oldest != NULL) {
        remember(a, now, oldest->ieee, oldest->endpoint, oldest->tsn, ZBA_CMD_ARM, oldest->mode,
                 NOTE_NONE, false);
        erase(oldest, sizeof(*oldest));
    }
    return oldest;
}

/* A retransmission: still waiting for the alarm task (nothing sent: the answer comes), or
 * answered within the window (the first answer again). */
static bool retransmission(zba_t *a, uint64_t ieee, uint8_t endpoint, uint8_t tsn,
                           uint8_t command, uint8_t mode)
{
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        const zba_pending_t *p = &a->pending[i];
        if (p->used && same_command(ieee, tsn, command, mode, p->ieee, p->tsn, p->command, p->mode)) {
            return true;
        }
    }
    for (size_t i = 0; i < ZBA_RECENT; i++) {
        const zba_recent_t *r = &a->recent[i];
        if (!r->used || !same_command(ieee, tsn, command, mode, r->ieee, r->tsn, r->command, r->mode)) {
            continue;
        }
        if (r->answered) {
            if (command == ZBA_CMD_ARM) {
                answer_arm(a, ieee, endpoint, tsn, r->note);
            } else {
                (void)refuse(a, ieee, endpoint, tsn, command, r->note);
            }
        }
        return true;
    }
    return false;
}

static uint32_t next_ticket(zba_t *a)
{
    a->ticket_seq++;
    if (a->ticket_seq == 0u) {
        a->ticket_seq = 1u; /* 0 is never a ticket */
    }
    return a->ticket_seq;
}

static zba_action_t do_arm(zba_t *a, uint64_t now, zba_keypad_t *k, zba_fob_t *fob,
                           uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *in,
                           zba_post_t *post)
{
    const uint8_t tsn = (uint8_t)in->tsn;
    zba_arm_t req;
    if (zba_decode_arm(in, &req) != ZBZ_ZCL_OK) {
        erase(&req, sizeof(req));
        (void)refuse(a, ieee, endpoint, tsn, in->command, ZBA_ZCL_MALFORMED);
        return ZBA_INVALID;
    }
    if (retransmission(a, ieee, endpoint, tsn, ZBA_CMD_ARM, req.mode)) {
        erase(&req, sizeof(req));
        return ZBA_DUPLICATE;
    }
    const uint8_t partition = k != NULL ? k->partition : fob->partition;
    const uint8_t mode = req.mode; /* not secret: kept past the wipe, the retry's identity */
    uint8_t note = NOTE_NONE;
    if (req.code_bad) {
        note = ZBA_NOTE_INVALID_CODE;
    } else if (req.mode > ZBA_ARM_ALL) {
        note = ZBA_NOTE_NOT_READY;
    } else if (k != NULL && req.code[0] == '\0') {
        note = ZBA_NOTE_INVALID_CODE; /* a keypad always gives a code; only a fob gives none */
    } else if (fob != NULL && req.mode == ZBA_ARM_DISARM && !fob->may_disarm) {
        note = ZBA_NOTE_INVALID_CODE; /* off by default (ZB-024) */
    }
    if (note != NOTE_NONE) {
        erase(&req, sizeof(req));
        answer_arm(a, ieee, endpoint, tsn, note);
        remember(a, now, ieee, endpoint, tsn, ZBA_CMD_ARM, mode, note, true);
        return ZBA_NOTHING;
    }
    zba_pending_t *p = pending_free(a);
    if (p == NULL) {
        erase(&req, sizeof(req));
        return ZBA_BUSY; /* no answer: it retries */
    }
    post->cmd.partition = partition;
    if (req.mode == ZBA_ARM_DISARM) {
        post->cmd.type = ALARM_CMD_DISARM;
    } else {
        post->cmd.type = ALARM_CMD_ARM;
        post->cmd.level = req.mode == ZBA_ARM_DAY     ? ALARM_LEVEL_STAY
                          : req.mode == ZBA_ARM_NIGHT ? ALARM_LEVEL_NIGHT
                                                      : ALARM_LEVEL_AWAY;
    }
    if (k != NULL) {
        memcpy(post->cmd.code, req.code, sizeof(post->cmd.code)); /* the one copy: the owner's */
        post->source = (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = k->device};
    } else {
        post->source = (alarm_source_t){.kind = ZBA_FOB_SOURCE_KIND, .id = ZBA_FOB_SOURCE_ID};
        post->user = fob->user; /* acts as its user; a code a fob sent is never passed on */
    }
    erase(&req, sizeof(req));
    post->ticket = next_ticket(a);
    *p = (zba_pending_t){.used = true, .ticket = post->ticket, .ieee = ieee,
                         .endpoint = endpoint, .tsn = tsn, .mode = mode,
                         .before = *view_of(a, partition), .at = now, .command = ZBA_CMD_ARM};
    return ZBA_POST;
}

static zba_action_t do_panic(zba_t *a, uint64_t now, zba_keypad_t *k, zba_fob_t *fob,
                             uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *in,
                             zba_post_t *post)
{
    const uint8_t tsn = (uint8_t)in->tsn;
    alarm_panic_t kind;
    bool allowed;
    if (k != NULL) {
        kind = in->command == ZBA_CMD_EMERGENCY ? ALARM_PANIC_MEDICAL
               : in->command == ZBA_CMD_FIRE    ? ALARM_PANIC_FIRE
                                                : ALARM_PANIC_POLICE;
        allowed = (k->panic_mask & (1u << (unsigned)kind)) != 0u;
    } else {
        kind = ALARM_PANIC_POLICE; /* a fob's one panic button, whichever command it sends */
        allowed = fob->panic;      /* off by default (ZB-024) */
    }
    if (retransmission(a, ieee, endpoint, tsn, in->command, 0u)) {
        return ZBA_DUPLICATE;
    }
    if (!allowed) {
        (void)refuse(a, ieee, endpoint, tsn, in->command, ZBZ_ZCL_NOT_AUTHORIZED);
        return ZBA_NOTHING;
    }
    zba_pending_t *p = pending_free(a);
    if (p == NULL) {
        p = pending_for_panic(a, now); /* a panic always gets through */
    }
    if (p == NULL) {
        return ZBA_BUSY;
    }
    const uint8_t partition = k != NULL ? k->partition : fob->partition;
    post->cmd.type = ALARM_CMD_PANIC;
    post->cmd.partition = partition;
    post->cmd.panic = kind;
    if (k != NULL) {
        post->source = (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = k->device};
    } else {
        post->source = (alarm_source_t){.kind = ZBA_FOB_SOURCE_KIND, .id = ZBA_FOB_SOURCE_ID};
        post->user = fob->user;
    }
    post->ticket = next_ticket(a);
    *p = (zba_pending_t){.used = true, .ticket = post->ticket, .ieee = ieee,
                         .endpoint = endpoint, .tsn = tsn, .before = *view_of(a, partition),
                         .at = now, .command = in->command, .want_response = in->want_response};
    return ZBA_POST;
}

/* The questions: answered at once, never ignored (alarm-devices.md §3). */
static zba_action_t do_question(zba_t *a, uint64_t now, uint16_t model_flags, uint8_t partition,
                                uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *in)
{
    const uint8_t tsn = (uint8_t)in->tsn;
    zba_request_t req;
    if (zba_decode_request(in, &req) != ZBZ_ZCL_OK) {
        (void)refuse(a, ieee, endpoint, tsn, in->command, ZBA_ZCL_MALFORMED);
        return ZBA_INVALID;
    }
    zsrc_zb_zcl_t f;
    switch (req.command) {
    case ZBA_CMD_GET_PANEL_STATUS: {
        zba_status_t s;
        zba_status_for(view_of(a, partition), now, model_flags, &s);
        zba_panel_status_response(&s, tsn, &f);
        break;
    }
    case ZBA_CMD_GET_ZONE_ID_MAP:
        zba_zone_id_map_response(NULL, tsn, &f);
        break;
    case ZBA_CMD_GET_ZONE_INFO: {
        const zba_zone_info_t none = {.zone_id = req.zone_id, .zone_type = ZBA_ZONE_TYPE_NONE,
                                      .ieee = ZBA_IEEE_NONE, .label = NULL};
        zba_zone_info_response(&none, tsn, &f);
        break;
    }
    case ZBA_CMD_GET_BYPASSED:
        zba_set_bypassed(NULL, 0u, tsn, &f);
        break;
    case ZBA_CMD_GET_ZONE_STATUS:
        zba_zone_status_response(true, NULL, 0u, tsn, &f);
        break;
    default: { /* BYPASS: "not allowed" for every zone asked */
        uint8_t results[ZSRC_ZB_ZCL_PAYLOAD_MAX];
        memset(results, ZBA_BYPASS_NOT_ALLOWED, sizeof(results));
        zba_bypass_response(results, req.count, tsn, &f);
        break;
    }
    }
    (void)send(a, ieee, endpoint, &f);
    return ZBA_NOTHING;
}

zba_action_t zba_receive(zba_t *a, uint64_t now, uint64_t ieee, uint8_t endpoint,
                         const zsrc_zb_zcl_t *in, zba_post_t *post)
{
    if (post != NULL) {
        memset(post, 0, sizeof(*post));
    }
    if (a == NULL || in == NULL || post == NULL || in->cluster != ZSRC_ZB_ZCL_IAS_ACE ||
        in->server_to_client || in->tsn > 0xFFu) {
        return ZBA_INVALID; /* not a keypad's command: not answered */
    }
    const uint8_t tsn = (uint8_t)in->tsn;
    zba_keypad_t *k = keypad_at(a, ieee, endpoint);
    zba_fob_t *fob = k == NULL ? fob_at(a, ieee, endpoint) : NULL;
    if (k == NULL && fob == NULL) {
        (void)refuse(a, ieee, endpoint, tsn, in->command, ZBZ_ZCL_NOT_AUTHORIZED);
        return ZBA_NOT_ASSIGNED; /* nothing else: no state, no answer of any kind */
    }
    if (in->manufacturer != 0u || in->command > ZBA_CMD_GET_ZONE_STATUS) {
        (void)refuse(a, ieee, endpoint, tsn, in->command, ZBZ_ZCL_UNSUP_COMMAND);
        return ZBA_INVALID;
    }
    if (in->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        (void)refuse(a, ieee, endpoint, tsn, in->command, ZBA_ZCL_MALFORMED);
        return ZBA_INVALID;
    }
    expire(a, now);
    if (k != NULL && (k->model_flags & ZBZ_MODEL_ACE_INIT) != 0u && !k->greeted) {
        (void)greet(a, now, k); /* it takes Arm Responses only after one */
    }
    switch (in->command) {
    case ZBA_CMD_ARM:
        return do_arm(a, now, k, fob, ieee, endpoint, in, post);
    case ZBA_CMD_EMERGENCY:
    case ZBA_CMD_FIRE:
    case ZBA_CMD_PANIC:
        return do_panic(a, now, k, fob, ieee, endpoint, in, post);
    default:
        return do_question(a, now, k != NULL ? k->model_flags : fob->model_flags,
                           k != NULL ? k->partition : fob->partition, ieee, endpoint, in);
    }
}

bool zba_result(zba_t *a, uint64_t now, uint32_t ticket, alarm_result_t result,
                const alarm_cmd_result_t *detail)
{
    (void)detail; /* the answer is the result alone; who it was is the engine's log */
    if (a == NULL || ticket == 0u) {
        return false;
    }
    zba_pending_t *p = NULL;
    for (size_t i = 0; i < ZBA_PENDING_MAX && p == NULL; i++) {
        if (a->pending[i].used && a->pending[i].ticket == ticket) {
            p = &a->pending[i];
        }
    }
    if (p == NULL) {
        return false;
    }
    if (now >= p->at && now - p->at >= ZBA_ANSWER_MS) {
        erase(p, sizeof(*p)); /* too late: the device has shown its own error */
        return false;
    }
    if (p->command == ZBA_CMD_ARM) {
        uint8_t note = zba_note_for(p->mode, result, &p->before);
        zba_keypad_t *k = keypad_at(a, p->ieee, p->endpoint);
        if (k != NULL && (k->model_flags & ZBZ_MODEL_ACE_INIT) != 0u && !k->greeted) {
            (void)greet(a, now, k);
        }
        answer_arm(a, p->ieee, p->endpoint, p->tsn, note);
        remember(a, now, p->ieee, p->endpoint, p->tsn, ZBA_CMD_ARM, p->mode, note, true);
    } else {
        uint8_t status = result == ALARM_OK                   ? ZBZ_ZCL_SUCCESS
                         : result == ALARM_ERR_NOT_AUTHORIZED ? ZBZ_ZCL_NOT_AUTHORIZED
                                                              : ZBZ_ZCL_FAILURE;
        bool answer = status != ZBZ_ZCL_SUCCESS || p->want_response; /* errors always */
        if (answer) {
            (void)refuse(a, p->ieee, p->endpoint, p->tsn, p->command, status);
        }
        remember(a, now, p->ieee, p->endpoint, p->tsn, p->command, 0u, status, answer);
    }
    erase(p, sizeof(*p));
    return true;
}

void zba_panel(zba_t *a, uint64_t now, uint8_t partition, const zba_view_t *view)
{
    if (a == NULL || view == NULL || partition < 1u || partition > ALARM_MAX_PARTITIONS) {
        return;
    }
    a->view[partition - 1u] = *view;
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        zba_keypad_t *k = &a->keypad[i];
        if (!k->used || k->partition != partition || !k->show_state) {
            continue;
        }
        zba_status_t s;
        zba_status_for(view, now, k->model_flags, &s);
        if (k->shown_valid && !delay_status(s.status) && s.status == k->shown.status &&
            s.alarm == k->shown.alarm && s.audible == k->shown.audible) {
            continue; /* nothing it shows has changed: a sleepy keypad isn't woken for it */
        }
        zsrc_zb_zcl_t f;
        zba_panel_status_changed(&s, &f);
        if (send(a, k->ieee, k->endpoint, &f)) {
            k->shown = s;
            k->shown_valid = true;
            k->greeted = true;
        }
    }
}

bool zba_greet(zba_t *a, uint64_t now, uint16_t id)
{
    if (a == NULL) {
        return false;
    }
    zba_keypad_t *k = keypad_id(a, id);
    return k != NULL && greet(a, now, k);
}

void zba_poll(zba_t *a, uint64_t now)
{
    if (a != NULL) {
        expire(a, now);
    }
}

uint64_t zba_next_deadline(const zba_t *a)
{
    uint64_t next = UINT64_MAX;
    if (a == NULL) {
        return next;
    }
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        if (a->pending[i].used && a->pending[i].at + ZBA_ANSWER_MS < next) {
            next = a->pending[i].at + ZBA_ANSWER_MS;
        }
    }
    for (size_t i = 0; i < ZBA_RECENT; i++) {
        if (a->recent[i].used && a->recent[i].at + ZBA_RETRY_WINDOW_MS < next) {
            next = a->recent[i].at + ZBA_RETRY_WINDOW_MS;
        }
    }
    return next;
}

/* ---- the store --------------------------------------------------------------------------------- */

size_t zba_encode(const zba_t *a, uint8_t *out, size_t cap)
{
    if (a == NULL || out == NULL) {
        return 0;
    }
    size_t nk = 0;
    size_t nf = 0;
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        nk += a->keypad[i].used ? 1u : 0u;
    }
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        nf += a->fob[i].used ? 1u : 0u;
    }
    size_t need = 3u + nk * ZBA_KEYPAD_ENTRY + nf * ZBA_FOB_ENTRY;
    if (cap < need) {
        return 0;
    }
    size_t at = 0;
    out[at++] = ZBA_COMPAT;
    out[at++] = (uint8_t)nk;
    out[at++] = (uint8_t)nf;
    for (size_t i = 0; i < ZBA_KEYPADS_MAX; i++) {
        const zba_keypad_t *k = &a->keypad[i];
        if (!k->used) {
            continue;
        }
        out[at++] = (uint8_t)KEYPAD_FIELDS;
        out[at++] = KIND_KEYPAD;
        wr16(&out[at], k->id);
        at += 2u;
        wr64(&out[at], k->ieee);
        at += 8u;
        out[at++] = k->endpoint;
        out[at++] = k->partition;
        out[at++] = k->panic_mask;
        out[at++] = k->show_state ? 0x01u : 0x00u;
        out[at++] = k->device;
    }
    for (size_t i = 0; i < ZBA_FOBS_MAX; i++) {
        const zba_fob_t *f = &a->fob[i];
        if (!f->used) {
            continue;
        }
        out[at++] = (uint8_t)FOB_FIELDS;
        out[at++] = KIND_FOB;
        wr16(&out[at], f->id);
        at += 2u;
        wr64(&out[at], f->ieee);
        at += 8u;
        out[at++] = f->endpoint;
        out[at++] = f->user;
        out[at++] = f->partition;
        out[at++] = (uint8_t)((f->may_disarm ? 0x01u : 0x00u) | (f->panic ? 0x02u : 0x00u));
    }
    return at;
}

/* Whether `id` or `ieee` is one already read. */
static bool seen(const zba_keypad_t *kp, size_t nk, const zba_fob_t *fb, size_t nf, uint16_t id,
                 uint64_t ieee)
{
    for (size_t i = 0; i < nk; i++) {
        if (kp[i].id == id || kp[i].ieee == ieee) {
            return true;
        }
    }
    for (size_t i = 0; i < nf; i++) {
        if (fb[i].id == id || fb[i].ieee == ieee) {
            return true;
        }
    }
    return false;
}

static bool keypad_entry(const uint8_t *p, zba_keypad_t *kp, size_t *nk, const zba_fob_t *fb,
                         size_t nf)
{
    zba_keypad_t k = {0};
    k.used = true;
    k.id = rd16(&p[1]);
    k.ieee = rd64(&p[3]);
    k.endpoint = p[11];
    k.partition = p[12];
    k.panic_mask = p[13];
    k.show_state = ((unsigned)p[14] & 0x01u) != 0u;
    k.device = p[15];
    if (*nk >= ZBA_KEYPADS_MAX || !keypad_ok(&k) || seen(kp, *nk, fb, nf, k.id, k.ieee)) {
        return false;
    }
    for (size_t i = 0; i < *nk; i++) {
        if (kp[i].device == k.device) {
            return false; /* another keypad's number: two keypads never share a lockout */
        }
    }
    kp[(*nk)++] = k;
    return true;
}

static bool fob_entry(const uint8_t *p, const zba_keypad_t *kp, size_t nk, zba_fob_t *fb,
                      size_t *nf)
{
    zba_fob_t f = {0};
    f.used = true;
    f.id = rd16(&p[1]);
    f.ieee = rd64(&p[3]);
    f.endpoint = p[11];
    f.user = p[12];
    f.partition = p[13];
    f.may_disarm = ((unsigned)p[14] & 0x01u) != 0u;
    f.panic = ((unsigned)p[14] & 0x02u) != 0u;
    if (*nf >= ZBA_FOBS_MAX || !fob_ok(&f) || seen(kp, nk, fb, *nf, f.id, f.ieee)) {
        return false;
    }
    for (size_t i = 0; i < *nf; i++) {
        if (fb[i].user == f.user) {
            return false; /* one fob for each user */
        }
    }
    fb[(*nf)++] = f;
    return true;
}

bool zba_decode(zba_t *a, const uint8_t *buf, size_t len)
{
    if (a == NULL) {
        return false;
    }
    zba_keypad_t kp[ZBA_KEYPADS_MAX];
    zba_fob_t fb[ZBA_FOBS_MAX];
    memset(kp, 0, sizeof(kp));
    memset(fb, 0, sizeof(fb));
    size_t nk = 0;
    size_t nf = 0;
    bool ok = buf != NULL && len >= 3u && buf[0] != 0u && buf[0] <= ZBA_COMPAT &&
              buf[1] <= ZBA_KEYPADS_MAX && buf[2] <= ZBA_FOBS_MAX;
    size_t at = 3u;
    size_t entries = ok ? (size_t)buf[1] + (size_t)buf[2] : 0u;
    for (size_t e = 0; ok && e < entries; e++) {
        if (at >= len) {
            ok = false;
            break;
        }
        size_t n = buf[at++];
        if (n < 1u || at + n > len) {
            ok = false;
            break;
        }
        const uint8_t *p = &buf[at];
        if (p[0] == KIND_KEYPAD) {
            if (n < KEYPAD_FIELDS) {
                ok = false;
                break;
            }
            (void)keypad_entry(p, kp, &nk, fb, nf); /* unusable: dropped, the rest load */
        } else if (p[0] == KIND_FOB) {
            if (n < FOB_FIELDS) {
                ok = false;
                break;
            }
            (void)fob_entry(p, kp, nk, fb, &nf);
        }
        /* another kind: a later format's, skipped */
        at += n;
    }
    if (ok && at != len) {
        ok = false; /* bytes past the entries counted */
    }
    erase(a->keypad, sizeof(a->keypad));
    erase(a->fob, sizeof(a->fob));
    erase(a->pending, sizeof(a->pending));
    erase(a->recent, sizeof(a->recent));
    a->next_recent = 0;
    a->dirty = false;
    if (ok) {
        memcpy(a->keypad, kp, sizeof(kp));
        memcpy(a->fob, fb, sizeof(fb));
    }
    return ok;
}
