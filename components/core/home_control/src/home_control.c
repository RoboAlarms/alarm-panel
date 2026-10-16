/*
 * home_control.c - the Devices registry (docs/features/24): the sources' device lists merged into
 * one table, the security classes (HCTL-005), one pending action per device with a bounded
 * lifetime and its result (HCTL-006, HCTL-012). Portable C11, no heap; the platform holds its
 * device lock around every call, and calls hc_tick periodically so a lost reply doesn't leave
 * a device busy forever.
 */
#include <string.h>

#include "home_control/home_control.h"

#include "home_control/dsrc_ha.h"
#include "home_control/dsrc_zw.h"

/* What a refresh carries over from the old row of a device that is still there: the pending
 * action and the last failure with what it showed then (failed_seen), unless its source now
 * reports something newer (BL-175), or its pending action rode on a connection that has since
 * dropped (HCTL-012), in which case the row is invalidated instead (see hc_refresh). */
typedef struct {
    uint32_t pending;
    uint32_t pending_gen;
    uint64_t pending_deadline_ms;
    uint32_t failed_seen;
    bool failed;
    bool unsure;
    uint8_t action;
    char error[HC_ERROR_LEN];
} hc_carry_t;

static void copy_str(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    if (len == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1u < len && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static const hc_device_t *find_ref_const(const hc_t *hc, const char *ref)
{
    if (ref == NULL || ref[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < hc->n_devices; i++) {
        if (strcmp(hc->devices[i].info.ref, ref) == 0) {
            return &hc->devices[i];
        }
    }
    return NULL;
}

static hc_device_t *find_ref(hc_t *hc, const char *ref)
{
    return (hc_device_t *)find_ref_const(hc, ref);
}

/* FNV-1a over a word's four bytes, unsigned arithmetic only. */
static uint32_t mix(uint32_t h, uint32_t v)
{
    for (unsigned b = 0; b < 4u; b++) {
        h = (h ^ ((v >> (8u * b)) & 0xFFu)) * 16777619u;
    }
    return h;
}

/* What a device shows, for BL-175: its state word, its level and setpoint when known, and a
 * scene's run time (Home Assistant's; a link drop's time is never in it, dsrc_ha.h). Not the
 * temperature, what a thermostat is doing, its modes or what it can do: they move by
 * themselves or say what it can do, not what it did. */
static uint32_t seen(const dev_info_t *i)
{
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < DEV_STATE_LEN && i->state[k] != '\0'; k++) {
        h = (h ^ (uint32_t)(unsigned char)i->state[k]) * 16777619u;
    }
    const uint32_t known = (uint32_t)i->values.flags & (DEV_VAL_LEVEL | DEV_VAL_SETPOINT);
    h = mix(mix(h, 0u), known);
    if ((known & DEV_VAL_LEVEL) != 0u) {
        h = mix(h, i->values.level);
    }
    if ((known & DEV_VAL_SETPOINT) != 0u) {
        h = mix(h, (uint32_t)i->values.setpoint);
    }
    if (i->kind == (uint8_t)DEV_SCENE) {
        h = mix(h, i->values.since);
    }
    return h;
}

/* ---- the source a device came from ---------------------------------------------------------- */

static size_t find_source(const hc_t *hc, uint8_t kind)
{
    for (size_t i = 0; i < hc->n_sources; i++) {
        if (hc->sources[i].ops != NULL && hc->sources[i].ops->kind == kind) {
            return i;
        }
    }
    return HC_MAX_SOURCES;
}

/* ---- the registry --------------------------------------------------------------------------- */

static void sink_changed(void *ctx, uint8_t source);
static void sink_result(void *ctx, uint32_t request_id, bool ok, const char *error,
                        bool unknown);

void hc_init(hc_t *hc, const hc_port_t *port)
{
    memset(hc, 0, sizeof(*hc));
    if (port != NULL) {
        hc->port = *port;
    }
    hc->sink.ctx = hc;
    hc->sink.changed = sink_changed;
    hc->sink.result = sink_result;
}

bool hc_register(hc_t *hc, const device_source_ops_t *ops, void *ctx)
{
    if (ops == NULL || ops->list == NULL || hc->n_sources >= HC_MAX_SOURCES) {
        return false;
    }
    if (find_source(hc, ops->kind) != HC_MAX_SOURCES) {
        return false; /* one source per kind: the second one would double every device */
    }
    hc->sources[hc->n_sources].ops = ops;
    hc->sources[hc->n_sources].ctx = ctx;
    hc->n_sources++;
    return true;
}

/* ---- what the sources report ---------------------------------------------------------------- */

static void notify_changed(hc_t *hc)
{
    if (hc->port.changed != NULL) {
        hc->port.changed(hc->port.ctx);
    }
}

static void sink_changed(void *ctx, uint8_t source)
{
    (void)source; /* the whole table is re-read: the platform calls hc_refresh on its lock */
    hc_t *hc = ctx;
    if (hc != NULL) {
        notify_changed(hc);
    }
}

static void sink_result(void *ctx, uint32_t request_id, bool ok, const char *error,
                        bool unknown)
{
    hc_t *hc = ctx;
    if (hc == NULL || request_id == 0) {
        return;
    }
    hc_device_t *d = NULL;
    for (size_t i = 0; i < hc->n_devices; i++) {
        if (hc->devices[i].pending == request_id) {
            d = &hc->devices[i];
            break;
        }
    }
    if (d == NULL) {
        return; /* a late/duplicate answer, or one to a device that has gone: nothing to show */
    }
    d->pending = 0;
    d->pending_gen = 0;
    d->pending_deadline_ms = 0;
    /* HCTL-012: a refusal says it wasn't done; a source that sent it on and couldn't confirm it
     * (Home Assistant's "timeout") says it may have happened - the source says which */
    d->unsure = !ok && unknown;
    if (ok) {
        d->failed = false;
        d->action = (uint8_t)ACT_NONE;
        d->error[0] = '\0';
    } else {
        d->failed = true;
        d->failed_seen = seen(&d->info); /* BL-175: a newer report clears it */
        copy_str(d->error, sizeof(d->error),
                 (error != NULL && error[0] != '\0') ? error : "failed");
    }
    if (hc->port.result != NULL) {
        hc->port.result(hc->port.ctx, request_id, d->info.ref, ok, d->error);
    }
    notify_changed(hc);
}

const dsrc_sink_t *hc_sink(hc_t *hc)
{
    return &hc->sink; /* filled by hc_init; lives as long as the registry does */
}

/* ---- the merged table ------------------------------------------------------------------------ */

void hc_refresh(hc_t *hc)
{
    /* Every source's connection generation, first (HCTL-012): a source that just dropped
     * bumps its generation, which the carry-over below uses to catch a pending action that
     * rode on the connection that is now gone, instead of nursing it forever. */
    for (size_t s = 0; s < hc->n_sources; s++) {
        const device_source_ops_t *ops = hc->sources[s].ops;
        if (ops == NULL) {
            continue;
        }
        const bool online = (ops->online != NULL) ? ops->online(hc->sources[s].ctx) : true;
        if (hc->sources[s].was_online && !online) {
            hc->sources[s].generation++;
        }
        hc->sources[s].was_online = online;
    }

    /* Every source's list (into hc->scratch: an hc_t lives off the stack), then the old
     * rows' pending action and last failure carried over for devices that are still there.
     * The carry table is ~3 KB of the caller's stack. */
    dev_info_t *batch = hc->scratch;
    hc_carry_t carry[HC_MAX_DEVICES];
    uint32_t invalidated[HC_MAX_DEVICES] = {0}; /* the pending id just failed as unknown, 0 = none */
    size_t n = 0;

    for (size_t s = 0; s < hc->n_sources && n < HC_MAX_DEVICES; s++) {
        const device_source_ops_t *ops = hc->sources[s].ops;
        if (ops == NULL || ops->list == NULL) {
            continue;
        }
        const size_t base = n;
        const size_t room = HC_MAX_DEVICES - base;
        size_t got = ops->list(hc->sources[s].ctx, &batch[base], room);
        if (got > room) {
            got = room;
        }
        for (size_t i = 0; i < got; i++) {
            dev_info_t entry = batch[base + i];
            if (entry.ref[0] == '\0' || entry.kind == DEV_NONE) {
                continue; /* nothing the screen could show or act on */
            }
            entry.source = ops->kind; /* the registry stamps it, not the source */
            batch[n] = entry;
            n++;
        }
    }

    for (size_t i = 0; i < n; i++) {
        memset(&carry[i], 0, sizeof(carry[i]));
        invalidated[i] = 0;
        const hc_device_t *old = find_ref_const(hc, batch[i].ref);
        if (old == NULL || old->info.source != batch[i].source) {
            continue;
        }
        if (old->pending != 0) {
            const size_t s = find_source(hc, batch[i].source);
            const bool same_gen =
                s != HC_MAX_SOURCES && old->pending_gen == hc->sources[s].generation;
            if (same_gen) {
                carry[i].pending = old->pending;
                carry[i].pending_gen = old->pending_gen;
                carry[i].pending_deadline_ms = old->pending_deadline_ms;
            } else {
                /* HCTL-012: the connection this command rode on is gone - an unknown outcome,
                 * not "busy" forever. Nothing here replays it: a person retries on purpose,
                 * which matters most for unlock and open. */
                invalidated[i] = old->pending;
            }
        }
        carry[i].action = old->action; /* under way, failed or invalidated: it stays named */
        if (invalidated[i] != 0) {
            /* what it showed before its connection dropped (the new row lists the drop) */
            carry[i].failed_seen = seen(&old->info);
        } else if (old->failed && old->pending == 0 && batch[i].online &&
                   seen(&batch[i]) != old->failed_seen) {
            /* BL-175, HCTL-006: its source reports something newer than what it showed when
             * its action failed: the state speaks now and the failure's words go (port.result
             * had it already) */
            carry[i].action = (uint8_t)ACT_NONE; /* failed false, error "" (the memset) */
        } else {
            carry[i].failed = old->failed;
            carry[i].unsure = old->unsure;
            carry[i].failed_seen = old->failed_seen;
            copy_str(carry[i].error, sizeof(carry[i].error), old->error);
        }
    }

    /* HCTL-012: a device the refresh dropped while its action was under way (gone from its
     * source's list, or moved to another source) fails that action as "offline" - a caller
     * waiting on the request id always hears back. Its old row is still intact here (the new
     * list is in hc->scratch), so the port gets its ref before the rows are rewritten. */
    for (size_t j = 0; j < hc->n_devices; j++) {
        hc_device_t *old = &hc->devices[j];
        if (old->pending == 0) {
            continue;
        }
        bool kept = false;
        for (size_t i = 0; i < n && !kept; i++) {
            kept = batch[i].source == old->info.source && strcmp(batch[i].ref, old->info.ref) == 0;
        }
        if (kept) {
            continue; /* carried over or invalidated above */
        }
        const uint32_t id = old->pending;
        old->pending = 0;
        old->pending_gen = 0;
        old->pending_deadline_ms = 0;
        /* HCTL-012: dropped in flight, outcome unknown - only the port's result hears it: the
         * row itself is rewritten or cleared below */
        old->failed = true;
        copy_str(old->error, sizeof(old->error), "offline");
        if (hc->port.result != NULL) {
            hc->port.result(hc->port.ctx, id, old->info.ref, false, old->error);
        }
    }

    for (size_t i = 0; i < n; i++) {
        hc_device_t *d = &hc->devices[i];
        memset(d, 0, sizeof(*d));
        d->info = batch[i];
        d->action = carry[i].action;
        d->failed_seen = carry[i].failed_seen;
        if (invalidated[i] != 0) {
            d->failed = true;
            d->unsure = true; /* HCTL-012: its connection dropped with it in flight */
            copy_str(d->error, sizeof(d->error), "offline");
        } else {
            d->pending = carry[i].pending;
            d->pending_gen = carry[i].pending_gen;
            d->pending_deadline_ms = carry[i].pending_deadline_ms;
            d->failed = carry[i].failed;
            d->unsure = carry[i].unsure;
            copy_str(d->error, sizeof(d->error), carry[i].error);
        }
    }
    for (size_t i = n; i < hc->n_devices; i++) {
        memset(&hc->devices[i], 0, sizeof(hc->devices[i]));
    }
    hc->n_devices = n;

    /* Tell the port about every command that was just invalidated, now the rows are settled -
     * the same channel a normal result uses (HCTL-006). */
    for (size_t i = 0; i < n; i++) {
        if (invalidated[i] != 0 && hc->port.result != NULL) {
            hc->port.result(hc->port.ctx, invalidated[i], hc->devices[i].info.ref, false,
                            hc->devices[i].error);
        }
    }
}

size_t hc_list(const hc_t *hc, const hc_device_t **out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < hc->n_devices && n < max; i++) {
        out[n++] = &hc->devices[i];
    }
    return n;
}

const hc_device_t *hc_get(const hc_t *hc, const char *ref)
{
    return find_ref_const(hc, ref);
}

bool hc_outcome_unknown(const hc_device_t *d)
{
    /* HCTL-012 (BL-171): by where the failure came from (the flag hc_tick and hc_refresh set,
     * or the source's own unknown), never by its word - Z-Wave's own "offline" is a refusal, the
     * registry's is not */
    return d != NULL && d->failed && d->pending == 0 && d->unsure;
}

/* ---- what an action means -------------------------------------------------------------------- */

bool hc_needs_code(dev_kind_t kind, dev_action_t action, int32_t value)
{
    /* HCTL-005: unlocking, opening any cover (whatever its class: a garage door, blinds, an
     * awning), opening a water valve. Lock, Close and Stop never ask. Judged by effect, not the
     * action's name (R15): a cover's ACT_SET_LEVEL reaches the same "open to some degree" state
     * ACT_OPEN does - the HA adapter sends it as set_position - so any target other than fully
     * closed (0) asks for the same code ACT_OPEN would, whatever the cover's reported position
     * or the direction of travel (a stale report can hide that a lower target opens it; BL-179
     * question 3, kept 2026-10-07); only a target of exactly 0 has ACT_CLOSE's effect and asks
     * for none. A target outside 0..100 is action validation's to judge: no exemption here. */
    if (action == ACT_UNLOCK) {
        return true;
    }
    if (action == ACT_OPEN) {
        return kind == DEV_COVER || kind == DEV_VALVE;
    }
    if (action == ACT_SET_LEVEL && kind == DEV_COVER) {
        return value != 0;
    }
    return false;
}

bool hc_action_valid(dev_kind_t kind, dev_action_t action)
{
    switch (kind) {
    case DEV_LIGHT:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE ||
               action == ACT_SET_LEVEL;
    case DEV_SWITCH:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_FAN:
        return action == ACT_ON || action == ACT_OFF || action == ACT_SET_LEVEL;
    case DEV_CLIMATE:
        return action == ACT_SET_TEMP || action == ACT_SET_MODE;
    case DEV_COVER:
        return action == ACT_OPEN || action == ACT_CLOSE || action == ACT_STOP ||
               action == ACT_SET_LEVEL;
    case DEV_LOCK:
        return action == ACT_LOCK || action == ACT_UNLOCK;
    case DEV_VALVE:
        return action == ACT_OPEN || action == ACT_CLOSE;
    case DEV_SCENE:
    case DEV_SCRIPT:
        return action == ACT_RUN;
    case DEV_NONE:
    case DEV_KIND_COUNT:
    default:
        return false;
    }
}

const char *hc_mode_word(uint8_t bit)
{
    /* One bit, one word: the screen's four mode keys. Home Assistant's own "heat_cool" is never
     * asked for by name - "auto" is, and dsrc_ha picks the word the thermostat has. */
    switch (bit) {
    case DEV_MODE_OFF: return "off";
    case DEV_MODE_HEAT: return "heat";
    case DEV_MODE_COOL: return "cool";
    case DEV_MODE_AUTO: return "auto";
    default: return NULL;
    }
}

bool hc_auto_less_secure(dev_kind_t kind, dev_action_t action, int32_t value)
{
    /* AUT-015 / D25: what the installer option decides - everything that asks a person for a
     * code, and a cover's stop, which can leave a garage door part-open. */
    return hc_needs_code(kind, action, value) || (kind == DEV_COVER && action == ACT_STOP);
}

bool hc_auto_allowed(dev_kind_t kind, dev_action_t action, int32_t value, const char *mode,
                     bool less_secure)
{
    /* HCTL-011 / AUT-002 (D23, ADR-050): what an automation may do with no person at the panel
     * (docs/features/41 DESIGN section 3). */
    if (!hc_action_valid(kind, action)) {
        return false;
    }
    /* a mode word goes with ACT_SET_MODE only: ha_link sends it with any action */
    if (action != ACT_SET_MODE && mode != NULL && mode[0] != '\0') {
        return false;
    }
    if (!less_secure && hc_auto_less_secure(kind, action, value)) {
        return false; /* D25: the installer option is off */
    }
    switch (kind) {
    case DEV_LIGHT:
        /* ACT_ON's value 0 is brightness 0, not "on" (dsrc_ha): only -1 */
        return (action == ACT_ON && value == -1) || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_FAN:
        return (action == ACT_ON && value == -1) || action == ACT_OFF;
    case DEV_SWITCH:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_LOCK:
        return action == ACT_LOCK || action == ACT_UNLOCK;
    case DEV_COVER:
        /* SET_LEVEL never: 0 is saved as ACT_CLOSE, a position is for later */
        return action == ACT_CLOSE || action == ACT_OPEN || action == ACT_STOP;
    case DEV_VALVE:
        return action == ACT_CLOSE || action == ACT_OPEN;
    case DEV_CLIMATE:
        return action == ACT_SET_MODE && mode != NULL && strcmp(mode, "off") == 0;
    case DEV_SCENE:
        return action == ACT_RUN;
    case DEV_SCRIPT: /* a script can do anything, unseen: never */
    case DEV_NONE:
    case DEV_KIND_COUNT:
    default:
        return false;
    }
}

/* Whether the device's own source can carry the pair out (what its act() would accept). */
static bool source_can(uint8_t source, dev_kind_t kind, dev_action_t action, int32_t value)
{
    switch (source) {
    case DSRC_HA:
        return dsrc_ha_action_word(kind, action) != NULL;
    case DSRC_ZWAVE:
        return dsrc_zw_level_for(kind, action, value) >= 0;
    default:
        return false; /* Zigbee (after M10) and none: nothing this firmware can send */
    }
}

size_t hc_auto_offer(dev_kind_t kind, uint8_t source, bool less_secure,
                     hc_auto_offer_t out[HC_AUTO_OFFER_MAX])
{
    /* AUT-002 layer 1, AUT-015: the pickers' rows, in the order they are shown. Every value is
     * -1 (none): a light's or a fan's Turn on with no level (dsrc_ha sends none, dsrc_zw full on). */
    static const struct {
        dev_action_t action;
        const char *mode;
    } k_rows[] = {
        {ACT_ON, ""},    {ACT_OFF, ""},   {ACT_TOGGLE, ""}, {ACT_LOCK, ""},
        {ACT_UNLOCK, ""}, {ACT_CLOSE, ""}, {ACT_OPEN, ""},   {ACT_STOP, ""},
        {ACT_SET_MODE, "off"}, {ACT_RUN, ""},
    };
    size_t n = 0;
    if (out == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0] && n < HC_AUTO_OFFER_MAX; i++) {
        if (!hc_auto_allowed(kind, k_rows[i].action, -1, k_rows[i].mode, less_secure) ||
            !source_can(source, kind, k_rows[i].action, -1)) {
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        out[n].action = (uint8_t)k_rows[i].action;
        out[n].value = -1;
        size_t m = 0;
        while (k_rows[i].mode[m] != '\0' && m + 1u < sizeof(out[n].mode)) {
            out[n].mode[m] = k_rows[i].mode[m];
            m++;
        }
        n++;
    }
    return n;
}

/* hc_act and hc_act_auto: the same checks in the same order; auto judges the action against
 * the device's live kind with the allowlist instead of hc_action_valid alone. */
static hc_result_t act(hc_t *hc, const char *ref, dev_action_t action, int32_t value,
                       const char *mode, bool automation, bool less_secure,
                       uint32_t *request_id)
{
    if (request_id != NULL) {
        *request_id = 0;
    }
    hc_device_t *d = find_ref(hc, ref);
    if (d == NULL) {
        return HC_ERR_UNKNOWN;
    }
    const size_t s = find_source(hc, d->info.source);
    if (s == HC_MAX_SOURCES || hc->sources[s].ops->act == NULL) {
        return HC_ERR_OFFLINE; /* the source went away with the device still listed */
    }
    const device_source_ops_t *ops = hc->sources[s].ops;
    void *sctx = hc->sources[s].ctx;
    if (!d->info.online || (ops->online != NULL && !ops->online(sctx))) {
        return HC_ERR_OFFLINE; /* HCTL-006: offline on the screen, its controls rest */
    }
    if (d->pending != 0) {
        return HC_ERR_BUSY;
    }
    const dev_kind_t kind = (dev_kind_t)d->info.kind;
    if (!hc_action_valid(kind, action)) {
        return HC_ERR_ARG;
    }
    if (automation && !hc_auto_allowed(kind, action, value, mode, less_secure)) {
        return HC_ERR_NOT_ALLOWED; /* HCTL-011: the live kind, before anything is sent */
    }

    /* The row says something now (a failed action's "Didn't" or "Couldn't confirm"): the clearing
     * below is a change the port hears of if the source refuses the send at once. */
    const bool said = d->failed || d->unsure || d->error[0] != '\0' ||
                      d->action != (uint8_t)ACT_NONE;
    uint32_t id = hc->next_request + 1u;
    if (id == 0) {
        id = 1u; /* 0 means "nothing under way" */
    }
    hc->next_request = id;
    d->pending = id;
    d->pending_gen = hc->sources[s].generation; /* HCTL-012: this command's connection identity */
    d->pending_deadline_ms = 0;                 /* hc_tick starts the clock: no clock in here */
    d->failed = false;
    d->unsure = false;
    d->action = (uint8_t)action;
    d->error[0] = '\0';
    if (!ops->act(sctx, d->info.ref, action, value, mode, id)) {
        d->pending = 0;
        d->pending_gen = 0;
        d->action = (uint8_t)ACT_NONE; /* nothing went out: nothing to say on the tile */
        if (said) {
            /* the failure it showed is gone: a reader that copies the list only when it changes
             * (device_service_seq, the panel link's device builds) must not keep showing it */
            notify_changed(hc);
        }
        return HC_ERR_OFFLINE;
    }
    if (request_id != NULL) {
        *request_id = id;
    }
    return HC_OK;
}

hc_result_t hc_act(hc_t *hc, const char *ref, dev_action_t action, int32_t value,
                   const char *mode)
{
    return act(hc, ref, action, value, mode, false, false, NULL);
}

hc_result_t hc_act_auto(hc_t *hc, const char *ref, dev_action_t action, int32_t value,
                        const char *mode, bool less_secure, uint32_t *request_id)
{
    return act(hc, ref, action, value, mode, true, less_secure, request_id);
}

/* ---- a pending action's lifetime (HCTL-012) --------------------------------------------------- */

void hc_tick(hc_t *hc, uint64_t now)
{
    for (size_t i = 0; i < hc->n_devices; i++) {
        hc_device_t *d = &hc->devices[i];
        if (d->pending == 0) {
            continue;
        }
        if (d->pending_deadline_ms == 0) {
            /* the first tick since hc_act started the clock: hc_act has no time of its own. A
             * device whose action takes longer (a garage door's whole move) says so. */
            d->pending_deadline_ms =
                now + (d->info.act_ms > HC_ACT_TIMEOUT_MS ? d->info.act_ms : HC_ACT_TIMEOUT_MS);
            continue;
        }
        if (now < d->pending_deadline_ms) {
            continue;
        }
        const uint32_t id = d->pending;
        d->pending = 0;
        d->pending_gen = 0;
        d->pending_deadline_ms = 0;
        d->failed = true;
        d->unsure = true; /* HCTL-012: no answer - it may have happened */
        d->failed_seen = seen(&d->info); /* BL-175: a newer report clears it */
        copy_str(d->error, sizeof(d->error), "timeout");
        if (hc->port.result != NULL) {
            hc->port.result(hc->port.ctx, id, d->info.ref, false, d->error);
        }
    }
}
