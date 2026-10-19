/*
 * link_settings.c - the local Home Assistant link's panel settings and keypads (proto_link.h;
 * docs/features/23 protocol.md "Panel settings and keypads", HAI-016..019): the settings, keypads
 * and settings_result writers, the settings_set parser, and the queue that paces settings_set per
 * target. The keys are proto_prefs.h's; the message keys are an external contract shared with the
 * aioroboalarms Python client (the golden strings in test_link.c).
 */
#include <string.h>

#include "alarm_proto/proto_link.h"

/* ---- messages the panel sends ------------------------------------------------------------- */

/* NULL (null) for a voice not known. */
static const char *voice_word(uint8_t voice)
{
    switch (voice) {
    case PROTO_LINK_VOICE_READY: return "ready";
    case PROTO_LINK_VOICE_LOADING: return "loading";
    case PROTO_LINK_VOICE_NONE: return "none";
    default: return NULL;
    }
}

void proto_link_settings(pj_writer_t *w, const proto_prefs_values_t *values,
                         proto_prefs_mask_t can, uint8_t voice)
{
    pj_obj(w);
    pj_kstr(w, "t", "settings");
    pj_key(w, "values");
    proto_prefs_json_values(w, values);
    pj_key(w, "can");
    proto_prefs_json_keys(w, can);
    pj_kstr(w, "voice", voice_word(voice));
    pj_obj_end(w);
}

void proto_link_keypads(pj_writer_t *w, const proto_link_keypad_t *k, size_t n)
{
    pj_obj(w);
    pj_kstr(w, "t", "keypads");
    pj_karr(w, "keypads");
    for (size_t i = 0; k != NULL && i < n; i++) {
        const proto_link_keypad_t *p = &k[i];
        pj_obj(w);
        pj_kuint(w, "device", p->device);
        pj_kstr(w, "id", p->id[0] != '\0' ? p->id : NULL);
        pj_kstr(w, "name", p->name);
        pj_knum(w, "home", p->home);
        pj_kbool(w, "online", p->online);
        pj_kstr(w, "fw", p->fw[0] != '\0' ? p->fw : NULL);
        if (p->rssi < 0) {
            pj_kint(w, "rssi", p->rssi);
        } else {
            pj_knull(w, "rssi");
        }
        /* Until it reports, nothing about its settings is known: no entities for it yet. */
        pj_key(w, "settings");
        if (p->reported) {
            proto_prefs_json_values(w, &p->settings);
        } else {
            pj_null(w);
        }
        pj_key(w, "can");
        proto_prefs_json_keys(w, p->reported ? p->can : 0u);
        pj_kstr(w, "voice", p->reported ? voice_word(p->voice) : NULL);
        pj_obj_end(w);
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

/* A keypad's panel id as the panel link keeps it: 6 lowercase hex characters. */
static bool panel_id_ok(const char *id)
{
    for (size_t i = 0; i < 6u; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false; /* the NUL of a shorter one too */
        }
    }
    return id[6] == '\0';
}

size_t proto_link_keypads_tidy(proto_link_keypad_t *k, size_t n, const char *own_id)
{
    size_t cleared = 0;
    for (size_t i = 0; k != NULL && i < n; i++) {
        if (k[i].id[0] == '\0') {
            continue;
        }
        bool drop = !panel_id_ok(k[i].id) || (own_id != NULL && strcmp(k[i].id, own_id) == 0);
        /* The lowest device number holding an id keeps it: it is never cleared for a twin, so
         * every later holder still finds it here. */
        for (size_t j = 0; !drop && j < n; j++) {
            const bool lower = k[j].device < k[i].device || (k[j].device == k[i].device && j < i);
            drop = j != i && lower && strcmp(k[j].id, k[i].id) == 0;
        }
        if (drop) {
            memset(k[i].id, 0, sizeof(k[i].id));
            cleared++;
        }
    }
    return cleared;
}

/* ---- settings_set ------------------------------------------------------------------------- */

bool proto_link_parse_settings_set(const char *json, size_t len, proto_link_settings_set_t *out)
{
    memset(out, 0, sizeof(*out));
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    bool has_target = false;
    bool has_values = false;
    char key[8]; /* "target", "values": a longer key matches nothing */
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            if (!pj_get_str(&r, out->id, sizeof(out->id))) {
                out->id[0] = '\0'; /* another type, or longer than any id echoed: no id */
            }
        } else if (strcmp(key, "target") == 0) {
            has_target = pj_peek(&r) == PJ_STRING;
            if (!pj_get_str(&r, out->target, sizeof(out->target))) {
                out->target[0] = '\0'; /* longer than any panel id: it matches none */
            }
        } else if (strcmp(key, "values") == 0) {
            if (has_values) {
                out->bad = true; /* named twice */
                pj_skip(&r);
            } else {
                has_values = true;
                if (!proto_prefs_json_parse(&r, PROTO_PREFS_KNOWN, &out->p)) {
                    out->bad = true; /* refused whole, or damaged (r.error, below) */
                }
            }
        } else {
            pj_skip(&r);
        }
    }
    if (r.error || !pj_finished(&r) || out->id[0] == '\0' || !has_target) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    if (!has_values) {
        out->bad = true;
    }
    if (out->bad) {
        memset(&out->p, 0, sizeof(out->p)); /* nothing of a bad request is applied */
    }
    return true;
}

void proto_link_settings_set_accept(proto_link_settings_set_t *s, proto_prefs_mask_t accept)
{
    proto_prefs_values_t kept;
    memset(&kept, 0, sizeof(kept));
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        const proto_prefs_mask_t bit = PROTO_PREFS_BIT(id);
        uint32_t x = 0u;
        if ((accept & bit) != 0u) {
            if (proto_prefs_get(&s->p.values, id, &x)) {
                (void)proto_prefs_put(&kept, id, x); /* checked when parsed */
            }
        } else if (((s->p.values.have | s->p.refused) & bit) != 0u) {
            s->p.refused |= bit;
            s->p.why[id - 1u] = (uint8_t)PROTO_PREFS_WHY_NOT_ALLOWED;
        }
    }
    s->p.values = kept;
}

/* Why a field is refused in the answer: the request's own refusal, else the target's for a field
 * the request sent it; 0 for none. */
static uint8_t refusal(const proto_link_settings_set_t *req, const uint8_t *why, unsigned id)
{
    if (req->p.why[id - 1u] != 0u) {
        return req->p.why[id - 1u];
    }
    if (why != NULL && (req->p.values.have & PROTO_PREFS_BIT(id)) != 0u) {
        return why[id - 1u];
    }
    return 0u;
}

void proto_link_settings_result(pj_writer_t *w, const proto_link_settings_set_t *req,
                                const char *error, const uint8_t *why)
{
    const bool failed = error != NULL && error[0] != '\0';
    bool refused = !failed && req->p.unknown_n > 0u; /* all of them, named or not */
    for (unsigned id = 1u; !failed && id <= PROTO_PREFS_F_LAST; id++) {
        refused = refused || refusal(req, why, id) != 0u;
    }
    pj_obj(w);
    pj_kstr(w, "t", "settings_result");
    pj_kstr(w, "id", req->id);
    pj_kbool(w, "ok", !failed && !refused);
    pj_kstr(w, "error", failed ? error : "");
    pj_kobj(w, "refused");
    if (!failed) {
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            const uint8_t r = refusal(req, why, id);
            if (r != 0u) {
                pj_kstr(w, proto_prefs_field(id)->key, proto_prefs_why_word(r));
            }
        }
        for (unsigned u = 0; u < req->p.unknown_n && u < PROTO_PREFS_UNKNOWN_MAX; u++) {
            pj_kstr(w, req->p.unknown[u], proto_prefs_why_word(PROTO_PREFS_WHY_NOT_ALLOWED));
        }
    }
    pj_obj_end(w);
    pj_obj_end(w);
}

/* ---- settings_set's queue (HAI-019) ------------------------------------------------------- */

void proto_link_setq_init(proto_link_setq_t *q)
{
    memset(q, 0, sizeof(*q));
    q->next_ticket = 1u;
}

void proto_link_setq_clear(proto_link_setq_t *q)
{
    memset(q->slot, 0, sizeof(q->slot));
}

static uint32_t new_ticket(proto_link_setq_t *q)
{
    if (q->next_ticket == 0u) {
        q->next_ticket = 1u; /* never 0: across the wrap, or before an init */
    }
    return q->next_ticket++;
}

/* a came before b (tickets count up; wrap-safe). */
static bool earlier(const proto_link_set_slot_t *a, const proto_link_set_slot_t *b)
{
    return (int32_t)(a->ticket - b->ticket) < 0;
}

static bool in_flight(const proto_link_setq_t *q, uint8_t target)
{
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        if (q->slot[i].state == PROTO_LINK_SET_FLIGHT && q->slot[i].target == target) {
            return true;
        }
    }
    return false;
}

static bool turn_due(const proto_link_setq_t *q, uint8_t target, uint64_t now)
{
    return q->tat[target] <=
           now + (uint64_t)(PROTO_LINK_SET_BURST - 1u) * (uint64_t)PROTO_LINK_SET_EVERY_MS;
}

proto_link_set_slot_t *proto_link_setq_add(proto_link_setq_t *q, uint64_t now, uint8_t target,
                                           const proto_link_settings_set_t *req)
{
    if (target >= PROTO_LINK_SET_TARGETS || req == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        proto_link_set_slot_t *s = &q->slot[i];
        if (s->state == PROTO_LINK_SET_FREE) {
            memset(s, 0, sizeof(*s));
            s->state = PROTO_LINK_SET_QUEUED;
            s->target = target;
            s->ticket = new_ticket(q);
            s->received_ms = now;
            s->req = *req;
            return s;
        }
    }
    return NULL;
}

proto_link_set_slot_t *proto_link_setq_next(proto_link_setq_t *q, uint64_t now)
{
    proto_link_set_slot_t *best = NULL;
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        proto_link_set_slot_t *s = &q->slot[i];
        if (s->state != PROTO_LINK_SET_QUEUED || in_flight(q, s->target) ||
            !turn_due(q, s->target, now)) {
            continue;
        }
        /* a target's own requests share its conditions, so its oldest goes first */
        if (best == NULL || earlier(s, best)) {
            best = s;
        }
    }
    if (best == NULL) {
        return NULL;
    }
    uint64_t *tat = &q->tat[best->target];
    *tat = (*tat > now ? *tat : now) + PROTO_LINK_SET_EVERY_MS;
    best->state = PROTO_LINK_SET_FLIGHT;
    best->sent_ms = now;
    return best;
}

proto_link_set_slot_t *proto_link_setq_find(proto_link_setq_t *q, uint32_t ticket)
{
    for (size_t i = 0; ticket != 0u && i < PROTO_LINK_SET_QUEUE; i++) {
        if (q->slot[i].state == PROTO_LINK_SET_FLIGHT && q->slot[i].ticket == ticket) {
            return &q->slot[i];
        }
    }
    return NULL;
}

proto_link_set_slot_t *proto_link_setq_expired(proto_link_setq_t *q, uint64_t now)
{
    proto_link_set_slot_t *oldest = NULL;
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        proto_link_set_slot_t *s = &q->slot[i];
        if (s->state != PROTO_LINK_SET_FREE && now >= s->received_ms + PROTO_LINK_SET_ANSWER_MS &&
            (oldest == NULL || earlier(s, oldest))) {
            oldest = s;
        }
    }
    return oldest;
}

void proto_link_setq_free(proto_link_setq_t *q, proto_link_set_slot_t *s)
{
    (void)q;
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}
