/*
 * zbz_wd.c - Zigbee sirens and strobes as the panel's bell (zbz_wd.h; docs/features/20
 * alarm-devices.md §2 and §4; ZB-023, BL-147).
 *
 * The frames are pure. The policy keeps, for each siren, the warning it was last asked for
 * (`mode`, `strobe_only`, `info`) and makes it match zbw_wanted() - the engine's bell and strobe
 * outputs of its partitions - at every call that carries the time (step()). A warning or Stop is
 * confirmed by the radio's end of the send (zbw_send_done); a squawk and a test sound are sent
 * once and never counted against the siren (its supervision tells). The decisions are recorded
 * in alarm-devices.md §2 "Decided in ZB-M10".
 */
#include "zigbee_zones/zbz_wd.h"

#include <string.h>

#include "zigbee_zones/zbz_models.h"

#define PART_MASK ((uint8_t)((1u << ALARM_MAX_PARTITIONS) - 1u))
#define ENDPOINT_MAX 240u
#define ENTRY_LEN (ZBW_ENTRY - 1u) /* what follows an entry's length byte */
/* What a tag's send was (bits 23-16 of the tag; bits 15-0 the sequence). */
#define KIND_COMMAND 0u /* a warning or Stop: confirmed, retried */
#define KIND_SQUAWK 1u
#define KIND_TEST 2u    /* "Test it" and the engine's bell test pulse */
#define TROUBLE_BITS (ZSRC_ZB_IAS_TROUBLE | ZSRC_ZB_IAS_AC_MAINS | ZSRC_ZB_IAS_BATT_DEFECT)

/* ---- the frames ------------------------------------------------------------------------------ */

static bool reversed(uint8_t layout)
{
    return layout == ZBW_LAYOUT_REVERSED;
}

uint8_t zbw_warning_info(const zbw_warning_t *w, uint8_t layout)
{
    const unsigned mode = (unsigned)w->mode & 0x0Fu;
    const unsigned level = (unsigned)w->level & 0x03u;
    if (reversed(layout)) {
        return (uint8_t)(mode | (w->strobe ? 0x10u : 0u) | ((unsigned)w->reserved & 0x20u) |
                         (level << 6));
    }
    return (uint8_t)((mode << 4) | (w->strobe ? 0x04u : 0u) | ((unsigned)w->reserved & 0x08u) |
                     level);
}

void zbw_warning_info_decode(uint8_t info, uint8_t layout, zbw_warning_t *out)
{
    const unsigned b = info;
    memset(out, 0, sizeof(*out));
    if (reversed(layout)) {
        out->mode = (uint8_t)(b & 0x0Fu);
        out->strobe = (b & 0x10u) != 0u;
        out->reserved = (uint8_t)(b & 0x20u);
        out->level = (uint8_t)(b >> 6);
    } else {
        out->mode = (uint8_t)(b >> 4);
        out->strobe = (b & 0x04u) != 0u;
        out->reserved = (uint8_t)(b & 0x08u);
        out->level = (uint8_t)(b & 0x03u);
    }
}

static void wd_command(uint8_t command, zsrc_zb_zcl_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cluster = ZSRC_ZB_ZCL_IAS_WD;
    out->command = command;
    out->server_to_client = false;
    out->want_response = true;
    out->manufacturer = 0u;
    out->tsn = ZSRC_ZB_TSN_NEW;
}

void zbw_start_warning(const zbw_warning_t *w, uint8_t layout, zsrc_zb_zcl_t *out)
{
    wd_command(ZBW_CMD_START_WARNING, out);
    out->payload[0] = zbw_warning_info(w, layout);
    out->payload[1] = (uint8_t)(w->duration_s & 0xFFu);
    out->payload[2] = (uint8_t)((unsigned)w->duration_s >> 8);
    out->payload[3] = (uint8_t)(w->duty > 100u ? 100u : w->duty);
    out->payload[4] = (uint8_t)(w->strobe_level & 0x03u);
    out->len = 5u;
}

/* The IAS WD command `command` from the panel, or ERR_TYPE. */
static zbz_zcl_result_t check_wd(const zsrc_zb_zcl_t *f, uint8_t command)
{
    if (f->cluster != ZSRC_ZB_ZCL_IAS_WD || f->command != command || f->server_to_client ||
        f->manufacturer != 0u) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return ZBZ_ZCL_ERR_LONG;
    }
    return ZBZ_ZCL_OK;
}

zbz_zcl_result_t zbw_decode_start_warning(const zsrc_zb_zcl_t *f, uint8_t layout,
                                          zbw_warning_t *out)
{
    if (f == NULL || out == NULL) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    zbz_zcl_result_t r = check_wd(f, ZBW_CMD_START_WARNING);
    if (r != ZBZ_ZCL_OK) {
        return r;
    }
    /* Warning Info and duration always; the strobe's duty cycle and level came later in the
     * cluster's history, and an older sender leaves them out (read as 0). */
    if (f->len < 3u) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    zbw_warning_t w;
    zbw_warning_info_decode(f->payload[0], layout, &w);
    w.duration_s = (uint16_t)((unsigned)f->payload[1] | ((unsigned)f->payload[2] << 8));
    if (f->len >= 4u) {
        if (f->payload[3] > 100u) {
            return ZBZ_ZCL_ERR_VALUE;
        }
        w.duty = f->payload[3];
    }
    if (f->len >= 5u) {
        if (f->payload[4] > ZBW_LEVEL_VERY_HIGH) {
            return ZBZ_ZCL_ERR_VALUE;
        }
        w.strobe_level = f->payload[4];
    }
    *out = w;
    return ZBZ_ZCL_OK;
}

uint8_t zbw_squawk_byte(const zbw_squawk_t *s, uint8_t layout)
{
    const unsigned mode = (unsigned)s->mode & 0x0Fu;
    const unsigned level = (unsigned)s->level & 0x03u;
    if (reversed(layout)) {
        return (uint8_t)(mode | (s->strobe ? 0x10u : 0u) | ((unsigned)s->reserved & 0x20u) |
                         (level << 6));
    }
    return (uint8_t)((mode << 4) | (s->strobe ? 0x08u : 0u) | ((unsigned)s->reserved & 0x04u) |
                     level);
}

void zbw_squawk_decode(uint8_t byte, uint8_t layout, zbw_squawk_t *out)
{
    const unsigned b = byte;
    memset(out, 0, sizeof(*out));
    if (reversed(layout)) {
        out->mode = (uint8_t)(b & 0x0Fu);
        out->strobe = (b & 0x10u) != 0u;
        out->reserved = (uint8_t)(b & 0x20u);
        out->level = (uint8_t)(b >> 6);
    } else {
        out->mode = (uint8_t)(b >> 4);
        out->strobe = (b & 0x08u) != 0u;
        out->reserved = (uint8_t)(b & 0x04u);
        out->level = (uint8_t)(b & 0x03u);
    }
}

void zbw_squawk(const zbw_squawk_t *s, uint8_t layout, zsrc_zb_zcl_t *out)
{
    wd_command(ZBW_CMD_SQUAWK, out);
    out->payload[0] = zbw_squawk_byte(s, layout);
    out->len = 1u;
}

uint8_t zbw_mode_for(alarm_pattern_t pattern, uint16_t model_flags)
{
    if (pattern == ALARM_PAT_OFF) {
        return ZBW_MODE_STOP;
    }
    if ((model_flags & ZBZ_MODEL_WD_ONE_TONE) != 0u) {
        return ZBW_MODE_EMERGENCY; /* its one sound, fire included */
    }
    switch (pattern) {
    case ALARM_PAT_TEMPORAL3:
        return ZBW_MODE_FIRE;
    case ALARM_PAT_TEMPORAL4:
        return ZBW_MODE_EMERGENCY; /* no CO sound of its own */
    default:
        return ZBW_MODE_BURGLAR; /* steady, the test pulse, and a bell that is simply on */
    }
}

/* ---- the policy ------------------------------------------------------------------------------ */

static uint8_t layout_of(const zbw_assign_t *a)
{
    return (uint8_t)((a->model_flags & ZBZ_MODEL_WD_REVERSED) != 0u ? ZBW_LAYOUT_REVERSED
                                                                    : ZBW_LAYOUT_SPEC);
}

/* How urgent a bell is, among the partitions a siren serves: fire, CO, then the rest. */
static unsigned rank(alarm_pattern_t p)
{
    switch (p) {
    case ALARM_PAT_OFF:
    case ALARM_PAT_TEST:
        return 0u;
    case ALARM_PAT_TEMPORAL3:
        return 3u;
    case ALARM_PAT_TEMPORAL4:
        return 2u;
    default:
        return 1u;
    }
}

static bool sounds_for(const zbw_assign_t *a, alarm_pattern_t p)
{
    switch (p) {
    case ALARM_PAT_OFF:
    case ALARM_PAT_TEST:
        return false;
    case ALARM_PAT_TEMPORAL3:
    case ALARM_PAT_TEMPORAL4:
        return a->fire;
    default:
        return a->burglary;
    }
}

/* The warning's length asked for (zbw_warn_seconds): its MaxDuration as read, at most its model's
 * cap; not read, the cap or the ZCL default; at most ZBW_WARN_MAX_S. */
static uint16_t warn_seconds(const zbw_assign_t *a)
{
    unsigned s;
    if (a->max_read) {
        /* Under ZBW_MAX_DURATION_ZERO_S (a read 0, or a 1..19 s the siren can't mean) is a 0. */
        s = a->max_read_s >= ZBW_MAX_DURATION_ZERO_S ? a->max_read_s : ZBW_MAX_DURATION_ZERO_S;
        if (a->max_duration_s != 0u && a->max_duration_s < s) {
            s = a->max_duration_s;
        }
    } else {
        s = a->max_duration_s != 0u ? a->max_duration_s : ZBW_MAX_DURATION_DEF;
    }
    return (uint16_t)(s < ZBW_WARN_MAX_S ? s : ZBW_WARN_MAX_S);
}

uint16_t zbw_warn_seconds(const zbw_assign_t *a)
{
    return a != NULL ? warn_seconds(a) : 0u;
}

bool zbw_max_duration_of(const zsrc_zb_zcl_t *f, uint16_t *out)
{
    if (f == NULL || out == NULL || f->cluster != ZSRC_ZB_ZCL_IAS_WD ||
        f->command != ZBZ_ZCL_READ_ATTR_RSP || !f->server_to_client || f->manufacturer != 0u ||
        f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return false;
    }
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zbz_zcl_iter(&it, f);
    while (zbz_zcl_next_attr(&it, &a)) {
        if (a.attr != ZBW_ATTR_MAX_DURATION) {
            continue;
        }
        if (a.status != ZBZ_ZCL_SUCCESS || a.type != ZBZ_ZCL_TYPE_U16 || a.len != 2u ||
            a.value == NULL) {
            return false;
        }
        const uint16_t v = (uint16_t)((unsigned)a.value[0] | ((unsigned)a.value[1] << 8));
        if (v == ZBW_MAX_DURATION_BAD) {
            return false;
        }
        *out = v;
        return true;
    }
    return false; /* not in it, or the records stop short of it */
}

/* Asked again this long before the warning runs out in the siren: room for every try of the
 * refresh (ZBW_REFRESH_LEAD_MS, at least ZBW_REFRESH_S), at most half the warning. */
static uint64_t lead_ms(const zbw_assign_t *a)
{
    const uint64_t half = (uint64_t)warn_seconds(a) * 500u;
    const uint64_t least = (uint64_t)ZBW_REFRESH_S * 1000u;
    const uint64_t budget = (uint64_t)ZBW_REFRESH_LEAD_MS;
    const uint64_t lead = budget > least ? budget : least;
    return lead < half ? lead : half;
}

static uint64_t silent_limit_ms(const zbw_assign_t *a)
{
    uint64_t s;
    if (a->supervision_min != 0u) {
        s = (uint64_t)a->supervision_min * 60u;
    } else {
        s = a->sleepy ? ZBW_SILENT_SLEEPY_S : ZBW_SILENT_ROUTER_S;
    }
    return s * 1000u;
}

uint64_t zbw_silent_limit_ms(const zbw_assign_t *a)
{
    return silent_limit_ms(a);
}

bool zbw_wanted(const zbw_t *w, const zbw_siren_t *s, zbw_warning_t *out)
{
    memset(out, 0, sizeof(*out));
    if (w == NULL || s == NULL || !s->a.used) {
        return false;
    }
    alarm_pattern_t best = ALARM_PAT_OFF;
    bool strobe_out = false;
    /* its own partitions and the ones it also sounds for (OUT-015): their alarms and strobe */
    const uint8_t serves = (uint8_t)(s->a.partitions | s->a.also);
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((serves & (1u << p)) == 0u) {
            continue;
        }
        const alarm_pattern_t pat = (alarm_pattern_t)w->bell[p];
        if (sounds_for(&s->a, pat) && rank(pat) > rank(best)) {
            best = pat;
        }
        strobe_out = strobe_out || w->strobe[p];
    }
    const bool sound = best != ALARM_PAT_OFF;
    const bool strobe = s->a.strobe && (sound || strobe_out);
    if (!sound && !strobe) {
        return false;
    }
    out->mode = (uint8_t)(sound ? zbw_mode_for(best, s->a.model_flags) : ZBW_MODE_STOP);
    out->strobe = strobe;
    out->level = (uint8_t)(s->a.volume & 0x03u);
    out->duration_s = warn_seconds(&s->a);
    out->duty = (uint8_t)(strobe ? ZBW_STROBE_DUTY : 0u);
    out->strobe_level = (uint8_t)(strobe ? ZBW_LEVEL_HIGH : ZBW_LEVEL_LOW);
    return true;
}

static bool active(const zbw_siren_t *s)
{
    return s->mode != ZBW_MODE_STOP || s->strobe_only;
}

/* Idle: quiet, and nothing of its own command left to confirm - a squawk or a test may go. */
static bool idle(const zbw_siren_t *s)
{
    return !active(s) && !s->pending;
}

/* Whether the siren's warning differs from the one it should give. */
static bool differs(const zbw_t *w, const zbw_siren_t *s)
{
    zbw_warning_t want;
    const bool on = zbw_wanted(w, s, &want);
    if (!on) {
        return active(s);
    }
    return !active(s) || zbw_warning_info(&want, layout_of(&s->a)) != s->info;
}

static uint32_t next_tag(zbw_t *w, unsigned kind)
{
    w->seq++;
    return ZBW_TAG | ((uint32_t)kind << 16) | (uint32_t)w->seq;
}

/* `cmd` to the siren; its tag, 0 when it wasn't queued. */
static uint32_t transmit(zbw_t *w, const zbw_siren_t *s, const zsrc_zb_zcl_t *cmd, unsigned kind)
{
    if (w->port.send == NULL) {
        return 0u;
    }
    const uint32_t tag = next_tag(w, kind);
    return w->port.send(w->port.ctx, s->a.ieee, s->a.endpoint, cmd, tag) ? tag : 0u;
}

static void stop_frame(const zbw_assign_t *a, zsrc_zb_zcl_t *out)
{
    zbw_warning_t stop;
    memset(&stop, 0, sizeof(stop));
    stop.mode = ZBW_MODE_STOP;
    stop.level = (uint8_t)(a->volume & 0x03u);
    zbw_start_warning(&stop, layout_of(a), out);
}

static void heard(zbw_siren_t *s, uint64_t now)
{
    if (now > s->heard_at || s->fresh) {
        s->heard_at = now;
    }
    s->fresh = false;
    s->silent = false;
}

/* A warning or Stop that failed, or whose end never came. */
static void failed(const zbw_t *w, zbw_siren_t *s, uint64_t now)
{
    s->tag = 0u;
    s->pending = true;
    if (!w->radio_up) {
        s->retry_at = now + ZBW_RETRY_SLOW_MS; /* not counted: the radio's own trouble says it */
        return;
    }
    if (s->tries < ZBW_SEND_TRIES) {
        s->tries++;
    }
    if (s->tries >= ZBW_SEND_TRIES) {
        s->unconfirmed = true;
        s->retry_at = now + ZBW_RETRY_SLOW_MS;
    } else {
        s->retry_at = now + ZBW_RETRY_MS;
    }
}

/* The siren's current warning (or Stop) sent, to be confirmed. */
static void send_current(zbw_t *w, zbw_siren_t *s, uint64_t now)
{
    zsrc_zb_zcl_t cmd;
    zbw_warning_t want;
    if (active(s) && zbw_wanted(w, s, &want)) {
        zbw_start_warning(&want, layout_of(&s->a), &cmd);
        s->until = now + (uint64_t)want.duration_s * 1000u;
    } else {
        stop_frame(&s->a, &cmd);
        s->until = 0u;
    }
    cmd.want_response = (s->a.model_flags & ZBZ_MODEL_NO_DEFAULT_RSP) == 0u;
    s->pending = true;
    s->tag = transmit(w, s, &cmd, KIND_COMMAND);
    if (s->tag == 0u) {
        failed(w, s, now);
    } else {
        s->retry_at = now + ZBW_CONFIRM_MS;
    }
}

/* Make the siren's warning the one it should give. */
static void sync(zbw_t *w, zbw_siren_t *s, uint64_t now)
{
    if (!differs(w, s)) {
        return;
    }
    zbw_warning_t want;
    if (zbw_wanted(w, s, &want)) {
        s->mode = want.mode;
        s->strobe_only = want.mode == ZBW_MODE_STOP;
        s->info = zbw_warning_info(&want, layout_of(&s->a));
    } else {
        s->mode = ZBW_MODE_STOP;
        s->strobe_only = false;
        s->info = 0u;
    }
    s->tries = 0u;
    send_current(w, s, now);
}

static void update_trouble(zbw_t *w)
{
    bool any = false;
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        zbw_siren_t *s = &w->siren[i];
        if (!s->a.used) {
            continue;
        }
        s->trouble = s->unconfirmed || s->silent || s->status_bad;
        any = any || s->trouble;
    }
    if (any != w->trouble) {
        w->trouble = any;
        if (w->port.bell_trouble != NULL) {
            w->port.bell_trouble(w->port.ctx, any);
        }
    }
}

/* Everything that is due at `now`, for every siren. */
static void step(zbw_t *w, uint64_t now)
{
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        zbw_siren_t *s = &w->siren[i];
        if (!s->a.used) {
            continue;
        }
        if (s->fresh) {
            s->heard_at = now;
            s->fresh = false;
        }
        s->silent = w->radio_up && now >= s->heard_at + silent_limit_ms(&s->a);
        sync(w, s, now);
        if (s->pending && now >= s->retry_at) {
            if (s->tag != 0u) {
                failed(w, s, now); /* its end never came */
            } else {
                send_current(w, s, now);
            }
        } else if (!s->pending && active(s) && now + lead_ms(&s->a) >= s->until) {
            s->tries = 0u;
            send_current(w, s, now); /* asked again before it runs out */
        }
    }
    update_trouble(w);
}

void zbw_init(zbw_t *w, const zbw_port_t *port)
{
    memset(w, 0, sizeof(*w));
    if (port != NULL) {
        w->port = *port;
    }
}

static zbw_siren_t *find(zbw_t *w, uint16_t id)
{
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        if (w->siren[i].a.used && w->siren[i].a.id == id) {
            return &w->siren[i];
        }
    }
    return NULL;
}

const zbw_siren_t *zbw_find(const zbw_t *w, uint16_t id)
{
    if (w == NULL) {
        return NULL;
    }
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        if (w->siren[i].a.used && w->siren[i].a.id == id) {
            return &w->siren[i];
        }
    }
    return NULL;
}

static bool assign_ok(const zbw_assign_t *a)
{
    return a->id != 0u && a->ieee != 0u && a->endpoint != 0u && a->endpoint <= ENDPOINT_MAX &&
           a->partitions != 0u && (a->partitions & (uint8_t)~PART_MASK) == 0u &&
           (a->also & (uint8_t)~PART_MASK) == 0u &&
           a->volume <= ZBW_LEVEL_VERY_HIGH &&
           (a->supervision_min == 0u || a->supervision_min >= ZBW_SUPERVISION_MIN_MIN);
}

/* Another siren than `self` at the same address and endpoint. */
static bool taken(const zbw_t *w, const zbw_siren_t *self, uint64_t ieee, uint8_t endpoint)
{
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        const zbw_siren_t *s = &w->siren[i];
        if (s != self && s->a.used && s->a.ieee == ieee && s->a.endpoint == endpoint) {
            return true;
        }
    }
    return false;
}

/* A siren that is no longer ours at its address: its last word is Stop when it gives a warning or
 * its last command (a warning or a Stop) isn't confirmed yet - not followed up. */
static void quiet(zbw_t *w, const zbw_siren_t *s)
{
    if (active(s) || s->pending) {
        zsrc_zb_zcl_t cmd;
        stop_frame(&s->a, &cmd);
        (void)transmit(w, s, &cmd, KIND_COMMAND);
    }
}

zbw_result_t zbw_assign(zbw_t *w, const zbw_assign_t *a)
{
    if (w == NULL || a == NULL || !assign_ok(a)) {
        return ZBW_ERR_ARG;
    }
    zbw_siren_t *s = find(w, a->id);
    if (taken(w, s, a->ieee, a->endpoint)) {
        return ZBW_ERR_ARG;
    }
    if (s != NULL && (s->a.ieee != a->ieee || s->a.endpoint != a->endpoint)) {
        /* Moved to another address: the old one is told to be quiet, and the new one starts as
         * a new siren (its warning, if one is wanted, goes at the next poll). */
        quiet(w, s);
        memset(s, 0, sizeof(*s));
        s->mode = ZBW_MODE_STOP;
        s->fresh = true;
        s->a = *a;
        s->a.used = true;
        s->a.device = 0u;
        s->a.also = (uint8_t)(a->also & (uint8_t)~a->partitions); /* never one of its own */
        w->dirty = true;
        update_trouble(w);
        return ZBW_OK;
    }
    if (s == NULL) {
        for (unsigned i = 0; i < ZBW_SIRENS_MAX && s == NULL; i++) {
            if (!w->siren[i].a.used) {
                s = &w->siren[i];
            }
        }
        if (s == NULL) {
            return ZBW_ERR_FULL;
        }
        memset(s, 0, sizeof(*s));
        s->mode = ZBW_MODE_STOP;
        s->fresh = true;
    }
    s->a = *a;
    s->a.used = true;
    s->a.device = 0u;
    s->a.also = (uint8_t)(a->also & (uint8_t)~a->partitions); /* never one of its own */
    w->dirty = true;
    return ZBW_OK;
}

zbw_result_t zbw_clear(zbw_t *w, uint16_t id)
{
    zbw_siren_t *s = w != NULL ? find(w, id) : NULL;
    if (s == NULL) {
        return ZBW_ERR_UNKNOWN;
    }
    quiet(w, s);
    memset(s, 0, sizeof(*s));
    w->dirty = true;
    update_trouble(w);
    return ZBW_OK;
}

void zbw_output(zbw_t *w, uint64_t now, uint8_t partition, alarm_output_t out,
                alarm_pattern_t pattern)
{
    if (w == NULL || partition < 1u || partition > ALARM_MAX_PARTITIONS) {
        return;
    }
    const unsigned p = partition - 1u;
    if (out == ALARM_OUT_BELL && pattern == ALARM_PAT_TEST) {
        /* The engine's bell test pulse (walk test): a short sound from each idle siren of it,
         * once; the bell's state is unchanged (no OFF follows a pulse). */
        step(w, now);
        for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
            zbw_siren_t *s = &w->siren[i];
            if (s->a.used && (s->a.partitions & (1u << p)) != 0u &&
                (s->a.burglary || s->a.fire) && idle(s)) {
                zbw_warning_t t;
                memset(&t, 0, sizeof(t));
                t.mode = zbw_mode_for(ALARM_PAT_TEST, s->a.model_flags);
                t.level = (uint8_t)(s->a.volume & 0x03u);
                t.duration_s = ZBW_TEST_S;
                zsrc_zb_zcl_t cmd;
                zbw_start_warning(&t, layout_of(&s->a), &cmd);
                cmd.want_response = (s->a.model_flags & ZBZ_MODEL_NO_DEFAULT_RSP) == 0u;
                const uint32_t tag = transmit(w, s, &cmd, KIND_TEST);
                if (tag != 0u) {
                    s->tag = tag;
                }
            }
        }
        return;
    }
    if (out == ALARM_OUT_BELL) {
        w->bell[p] = (uint8_t)pattern;
    } else if (out == ALARM_OUT_STROBE) {
        w->strobe[p] = pattern != ALARM_PAT_OFF;
    } else {
        return; /* smoke power reset: not a siren's */
    }
    step(w, now);
}

void zbw_set_armed(zbw_t *w, uint8_t partition, bool armed)
{
    if (w != NULL && partition >= 1u && partition <= ALARM_MAX_PARTITIONS) {
        w->armed[partition - 1u] = armed;
    }
}

void zbw_armed(zbw_t *w, uint64_t now, uint8_t partition, bool armed)
{
    if (w == NULL || partition < 1u || partition > ALARM_MAX_PARTITIONS) {
        return;
    }
    const unsigned p = partition - 1u;
    w->armed[p] = armed;
    step(w, now);
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        zbw_siren_t *s = &w->siren[i];
        if (!s->a.used || !s->a.squawk || (s->a.partitions & (1u << p)) == 0u || !idle(s)) {
            continue;
        }
        zbw_squawk_t sq;
        memset(&sq, 0, sizeof(sq));
        sq.mode = (uint8_t)(armed ? ZBW_SQUAWK_ARMED : ZBW_SQUAWK_DISARMED);
        sq.strobe = s->a.strobe;
        sq.level = (uint8_t)(s->a.volume & 0x03u);
        zsrc_zb_zcl_t cmd;
        zbw_squawk(&sq, layout_of(&s->a), &cmd);
        cmd.want_response = false; /* a chirp is never retried: a late one would mislead */
        const uint32_t tag = transmit(w, s, &cmd, KIND_SQUAWK);
        if (tag != 0u) {
            s->tag = tag;
        }
    }
}

zbw_result_t zbw_test(zbw_t *w, uint64_t now, uint16_t id)
{
    zbw_siren_t *s = w != NULL ? find(w, id) : NULL;
    if (s == NULL) {
        return ZBW_ERR_UNKNOWN;
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->a.partitions & (1u << p)) != 0u && w->armed[p]) {
            return ZBW_ERR_ARMED;
        }
    }
    step(w, now);
    if (!idle(s)) {
        return ZBW_ERR_BUSY;
    }
    zbw_warning_t t;
    memset(&t, 0, sizeof(t));
    t.mode = zbw_mode_for(ALARM_PAT_TEST, s->a.model_flags);
    t.strobe = s->a.strobe;
    t.level = (uint8_t)(s->a.volume & 0x03u);
    t.duration_s = ZBW_TEST_S;
    t.duty = (uint8_t)(s->a.strobe ? ZBW_STROBE_DUTY : 0u);
    t.strobe_level = (uint8_t)(s->a.strobe ? ZBW_LEVEL_HIGH : ZBW_LEVEL_LOW);
    zsrc_zb_zcl_t cmd;
    zbw_start_warning(&t, layout_of(&s->a), &cmd);
    cmd.want_response = (s->a.model_flags & ZBZ_MODEL_NO_DEFAULT_RSP) == 0u;
    const uint32_t tag = transmit(w, s, &cmd, KIND_TEST);
    if (tag == 0u) {
        return ZBW_ERR_SEND;
    }
    s->tag = tag;
    return ZBW_OK;
}

bool zbw_send_done(zbw_t *w, uint64_t now, uint32_t tag, zsrc_zb_done_t result)
{
    if (w == NULL || (tag & ZBW_TAG_MASK) != ZBW_TAG) {
        return false;
    }
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        zbw_siren_t *s = &w->siren[i];
        if (!s->a.used || s->tag != tag) {
            continue;
        }
        const unsigned kind = (unsigned)(tag >> 16) & 0xFFu;
        if (kind == KIND_COMMAND && s->pending) {
            if (result == ZSRC_ZB_DONE_OK) {
                s->tag = 0u;
                s->pending = false;
                s->tries = 0u;
                s->unconfirmed = false;
                heard(s, now);
            } else {
                failed(w, s, now);
            }
        } else {
            s->tag = 0u; /* a squawk or a test: its outcome counts only as a frame heard */
            if (result == ZSRC_ZB_DONE_OK) {
                heard(s, now);
            }
        }
        break;
    }
    step(w, now);
    return true; /* ours, even when a newer send replaced it */
}

void zbw_heard(zbw_t *w, uint64_t now, uint64_t ieee)
{
    if (w == NULL) {
        return;
    }
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        if (w->siren[i].a.used && w->siren[i].a.ieee == ieee) {
            heard(&w->siren[i], now);
        }
    }
    step(w, now);
}

void zbw_zone_status(zbw_t *w, uint64_t now, uint64_t ieee, uint16_t status)
{
    if (w == NULL) {
        return;
    }
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        zbw_siren_t *s = &w->siren[i];
        if (s->a.used && s->a.ieee == ieee) {
            s->status_bad = (status & TROUBLE_BITS) != 0u;
            heard(s, now);
        }
    }
    step(w, now);
}

void zbw_radio(zbw_t *w, uint64_t now, bool up)
{
    if (w == NULL) {
        return;
    }
    const bool came_up = up && !w->radio_up;
    w->radio_up = up;
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        zbw_siren_t *s = &w->siren[i];
        if (!s->a.used) {
            continue;
        }
        s->silent = false;
        if (came_up) {
            heard(s, now); /* silence counts from now */
            if (s->pending) {
                s->tag = 0u; /* whatever was under way went with the radio */
                s->tries = 0u;
                s->retry_at = now;
            }
        }
    }
    step(w, now);
}

void zbw_poll(zbw_t *w, uint64_t now)
{
    if (w != NULL) {
        step(w, now);
    }
}

static uint64_t earlier(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

uint64_t zbw_next_deadline(const zbw_t *w)
{
    uint64_t d = UINT64_MAX;
    if (w == NULL) {
        return d;
    }
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        const zbw_siren_t *s = &w->siren[i];
        if (!s->a.used) {
            continue;
        }
        if (s->fresh || differs(w, s)) {
            return 0u;
        }
        if (s->pending) {
            d = earlier(d, s->retry_at);
        } else if (active(s)) {
            const uint64_t lead = lead_ms(&s->a);
            d = earlier(d, s->until > lead ? s->until - lead : 0u);
        }
        if (w->radio_up && !s->silent) {
            d = earlier(d, s->heard_at + silent_limit_ms(&s->a));
        }
    }
    return d;
}

/* ---- the store ------------------------------------------------------------------------------- */

size_t zbw_encode(const zbw_t *w, uint8_t *out, size_t cap)
{
    if (w == NULL || out == NULL) {
        return 0;
    }
    size_t count = 0;
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        count += w->siren[i].a.used ? 1u : 0u;
    }
    if (cap < 2u + count * ZBW_ENTRY) {
        return 0;
    }
    size_t at = 0;
    out[at++] = ZBW_COMPAT;
    out[at++] = (uint8_t)count;
    for (unsigned i = 0; i < ZBW_SIRENS_MAX; i++) {
        const zbw_assign_t *a = &w->siren[i].a;
        if (!a->used) {
            continue;
        }
        out[at++] = (uint8_t)ENTRY_LEN;
        out[at++] = (uint8_t)(a->id & 0xFFu);
        out[at++] = (uint8_t)((unsigned)a->id >> 8);
        for (unsigned b = 0; b < 8u; b++) {
            out[at++] = (uint8_t)((a->ieee >> (8u * b)) & 0xFFu);
        }
        out[at++] = a->endpoint;
        out[at++] = a->partitions;
        out[at++] = (uint8_t)((a->burglary ? 0x01u : 0u) | (a->fire ? 0x02u : 0u) |
                              (a->strobe ? 0x04u : 0u) | (a->squawk ? 0x08u : 0u));
        out[at++] = a->volume;
        out[at++] = 0u; /* device: a siren takes none */
        out[at++] = (uint8_t)(a->supervision_min & 0xFFu);
        out[at++] = (uint8_t)((unsigned)a->supervision_min >> 8);
        out[at++] = a->also;
    }
    return at;
}

/* One entry's fields (`n` of them present; the rest read as 0) into `a`. */
static void read_entry(const uint8_t *p, size_t n, zbw_assign_t *a)
{
    uint8_t e[ENTRY_LEN];
    memset(e, 0, sizeof(e));
    memcpy(e, p, n < ENTRY_LEN ? n : ENTRY_LEN);
    memset(a, 0, sizeof(*a));
    a->used = true;
    a->id = (uint16_t)((unsigned)e[0] | ((unsigned)e[1] << 8));
    for (unsigned b = 0; b < 8u; b++) {
        a->ieee |= (uint64_t)e[2u + b] << (8u * b);
    }
    a->endpoint = e[10];
    a->partitions = e[11];
    a->burglary = (e[12] & 0x01u) != 0u;
    a->fire = (e[12] & 0x02u) != 0u;
    a->strobe = (e[12] & 0x04u) != 0u;
    a->squawk = (e[12] & 0x08u) != 0u;
    a->volume = e[13];
    /* e[14], the device, is ignored: a siren takes none */
    a->supervision_min = (uint16_t)((unsigned)e[15] | ((unsigned)e[16] << 8));
    a->also = (uint8_t)(e[17] & (uint8_t)~a->partitions); /* never one of its own */
}

bool zbw_decode(zbw_t *w, const uint8_t *buf, size_t len)
{
    if (w == NULL) {
        return false;
    }
    /* All or nothing: the live sirens change only when the whole blob reads. */
    if (buf == NULL || len < 2u || buf[0] == 0u || buf[0] > ZBW_COMPAT ||
        buf[1] > ZBW_SIRENS_MAX) {
        return false;
    }
    zbw_siren_t got[ZBW_SIRENS_MAX];
    memset(got, 0, sizeof(got));
    const size_t count = buf[1];
    size_t at = 2u;
    for (size_t i = 0; i < count; i++) {
        if (at >= len) {
            return false;
        }
        const size_t entry = buf[at++];
        if (at + entry > len) {
            return false;
        }
        /* A later format may append fields inside an entry: its length covers them. */
        read_entry(&buf[at], entry, &got[i].a);
        if (!assign_ok(&got[i].a)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (got[j].a.id == got[i].a.id ||
                (got[j].a.ieee == got[i].a.ieee && got[j].a.endpoint == got[i].a.endpoint)) {
                return false; /* the same siren twice */
            }
        }
        got[i].mode = ZBW_MODE_STOP;
        got[i].fresh = true;
        at += entry;
    }
    if (at != len) {
        return false;
    }
    memcpy(w->siren, got, sizeof(got));
    w->dirty = false;
    update_trouble(w);
    return true;
}
