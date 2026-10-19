/*
 * plink_fw.c - the master's keypad-update planner (plink_fw.h; docs/features/11 "Updates through
 * the link").
 */
#include "panel_link/plink_fw.h"

#include <string.h>

#include "alarm/alarm_types.h"
#include "alarm_proto/ota_profile.h"
#include "alarm_proto/proto_fw.h"

typedef struct {
    const plink_fw_world_t *w;
    uint64_t now;
    plink_fw_step_t *out;
    bool need_copy[PLINK_KEYPADS_MAX]; /* waits for a copy, or relays from the one held */
} plan_t;

/* ---- small helpers --------------------------------------------------------------------------- */

static bool same(const char *a, const char *b, size_t cap)
{
    return strncmp(a, b, cap) == 0;
}

#define SAME(a, b) same((a), (b), sizeof(a))

/* dst (cap bytes) holds src, cut and terminated, zeros after it. */
static void put(char *dst, size_t cap, const char *src)
{
    memset(dst, 0, cap);
    size_t n = 0u;
    while (n + 1u < cap && src[n] != '\0') {
        n++;
    }
    memcpy(dst, src, n);
}

#define PUT(dst, src) put((dst), sizeof(dst), (src))

/* Version a is older than b (OTA-014's rule: b would be an upgrade of a). */
static bool older(const char *a, const char *b)
{
    return proto_version_is_upgrade(a, b, false);
}

/* The FETCH could not be done, and the relay may bring V (LINK-031): not reached, broke off, too
 * slow, or the server offers another version, nothing newer or no release of V. */
static bool relayable(uint8_t why)
{
    return (why >= PLINK_FW_WHY_NO_NETWORK && why <= 9u) || why == PLINK_FW_WHY_BROKE_OFF ||
           why == PLINK_FW_WHY_TOO_SLOW ||
           (why >= PLINK_FW_WHY_OTHER_VERSION && why <= PLINK_FW_WHY_UNKNOWN_VERSION);
}

/* A refusal that waits for the master's next version: the server's (10-12, 16-19, and an unknown
 * one of that decade) and the image's or the order's (30-49), but BUDGET, which ends by itself. */
static bool final(uint8_t why)
{
    const plink_fw_why_class_t c = plink_fw_why_class(why);
    if (c == PLINK_FWC_SERVER) {
        return why < PLINK_FW_WHY_OTHER_VERSION || why > PLINK_FW_WHY_UNKNOWN_VERSION;
    }
    return c == PLINK_FWC_REFUSED && why != PLINK_FW_WHY_BUDGET;
}

/* An order's echo that says it wasn't taken. */
static bool refusal(uint8_t why)
{
    return plink_fw_why_class(why) == PLINK_FWC_REFUSED || why == PLINK_FW_WHY_BUSY;
}

static plink_fw_facts_t facts_of(const char *version, const plink_fw_info_t *info)
{
    plink_fw_facts_t f;
    PUT(f.version, version);
    PUT(f.device, info->device);
    PUT(f.chip_rev, info->chip_rev);
    PUT(f.project, info->project);
    return f;
}

static bool facts_eq(const plink_fw_facts_t *a, const plink_fw_facts_t *b)
{
    return SAME(a->version, b->version) && SAME(a->device, b->device) &&
           SAME(a->chip_rev, b->chip_rev) && SAME(a->project, b->project);
}

/* ---- images ---------------------------------------------------------------------------------- */

bool plink_fw_fits(const plink_fw_image_t *im, const char *version, const plink_fw_info_t *info)
{
    if (im == NULL || version == NULL || info == NULL || im->src == PLINK_FWSRC_NONE ||
        im->size == 0u || version[0] == '\0') {
        return false;
    }
    return same(im->version, version, sizeof im->version) && SAME(im->device, info->device) &&
           info->chip_rev_full >= im->chip_min && info->chip_rev_full <= im->chip_max &&
           ota_profile_accepts(info->project, im->project) &&
           (info->slot == 0u || im->size <= info->slot);
}

/* The image of the target the master can relay to keypad info: its own, else the copy. */
static const plink_fw_image_t *image_for(const plink_fw_world_t *w, const plink_fw_info_t *info)
{
    if (plink_fw_fits(&w->own, w->target, info)) {
        return &w->own;
    }
    if (w->copy.state == PLINK_FWCOPY_READY && plink_fw_fits(&w->copy.image, w->target, info)) {
        return &w->copy.image;
    }
    return NULL;
}

/* The image of the target whose SHA-256 starts with id8 (NULL: none held). */
static const plink_fw_image_t *image_by_id8(const plink_fw_world_t *w, const uint8_t *id8)
{
    const plink_fw_image_t *im[2] = {&w->own, &w->copy.image};
    for (size_t i = 0; i < 2u; i++) {
        if (im[i]->src != PLINK_FWSRC_NONE && im[i]->size > 0u &&
            SAME(im[i]->version, w->target) && memcmp(im[i]->sha256, id8, PLINK_ID8_LEN) == 0 &&
            (i == 0u || w->copy.state == PLINK_FWCOPY_READY)) {
            return im[i];
        }
    }
    return NULL;
}

/* ---- orders ---------------------------------------------------------------------------------- */

static plink_fw_order_t *order(plan_t *p, size_t i, uint8_t op, const char *version)
{
    plink_fw_order_t *o = &p->out->send[i];
    memset(o, 0, sizeof *o);
    o->op = op;
    o->kind = PLINK_FW_KIND_APP;
    PUT(o->version, version);
    return o;
}

/* FETCH (im NULL) or RELAY of im, naming the master's server, channel and key (LINK-036). */
static void send_image(plan_t *p, size_t i, uint8_t op, const plink_fw_image_t *im)
{
    const plink_fw_world_t *w = p->w;
    plink_fw_order_t *o = order(p, i, op, w->target);
    memcpy(o->key, w->key, sizeof o->key);
    PUT(o->server, w->server);
    PUT(o->channel, w->channel);
    if (im != NULL) {
        o->size = im->size;
        memcpy(o->sha256, im->sha256, sizeof o->sha256);
        o->page = PLINK_FWP_PAGE;
        o->count = PLINK_FWP_COUNT;
        o->gap_ms = PLINK_FWP_GAP_MS;
    }
    p->out->mem[i].op = op;
}

/* The download failed with why: tried again in an hour, or at the master's next version. */
static void fail(plan_t *p, size_t i, uint8_t why)
{
    plink_fw_mem_t *m = &p->out->mem[i];
    m->op = 0u;
    m->why = why;
    if (final(why)) {
        m->wait_next = true;
    } else {
        m->retry_at = p->now + PLINK_FWP_RETRY_MS;
    }
}

/* The relay for keypad i, whose FETCH could not be done (why) or who can't fetch: the image that
 * fits it, else a copy for its facts - wanted, waited for, or its failure taken as the keypad's. */
static void relay(plan_t *p, size_t i, uint8_t why)
{
    const plink_fw_world_t *w = p->w;
    const plink_fw_info_t *info = &w->kp[i].info;
    if ((info->can & PLINK_FWI_RELAY) == 0u) {
        fail(p, i, why); /* its own server again, in an hour */
        return;
    }
    const plink_fw_image_t *im = image_for(w, info);
    if (im != NULL) {
        send_image(p, i, PLINK_FW_OP_RELAY, im);
        p->need_copy[i] = im == &w->copy.image; /* (kept while it relays) */
        return;
    }
    const plink_fw_facts_t f = facts_of(w->target, info);
    if (w->copy.state == PLINK_FWCOPY_FAILED && facts_eq(&w->copy.facts, &f)) {
        fail(p, i, w->copy.why);
        return;
    }
    if (w->copy.state == PLINK_FWCOPY_READY && facts_eq(&w->copy.facts, &f)) {
        /* The copy for its facts doesn't fit it (fetching it again would bring the same). */
        const bool big = info->slot != 0u && w->copy.image.size > info->slot;
        fail(p, i, big ? (uint8_t)PLINK_FW_WHY_TOO_BIG : (uint8_t)PLINK_FW_WHY_CHIP);
        return;
    }
    p->need_copy[i] = true;
}

/* ---- one keypad ------------------------------------------------------------------------------ */

/* It went back on v: its own word (FW_STATE's last section), or back behind v in a later session
 * than its INSTALL with no image of v READY. */
static bool went_back(plan_t *p, size_t i, bool run_v)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    const plink_fw_state_t *st = &kp->state;
    plink_fw_mem_t *m = &p->out->mem[i];
    const char *v = p->w->target;
    if (!m->again && st->last == PLINK_FW_LAST_ROLLED_BACK &&
        same(st->last_version, v, sizeof st->last_version)) {
        PUT(m->went_back, v);
    }
    if (m->install_sid != 0u && m->install_sid != kp->sessions &&
        !(run_v && (st->state == PLINK_FWS_READY || st->state == PLINK_FWS_INSTALLING))) {
        PUT(m->went_back, v);
        m->install_sid = 0u;
        m->again = false;
    }
    return SAME(m->went_back, v);
}

static void decide(plan_t *p, size_t i)
{
    const plink_fw_world_t *w = p->w;
    const plink_fw_kp_t *kp = &w->kp[i];
    plink_fw_mem_t *m = &p->out->mem[i];
    const char *v = w->target;
    if (!kp->online) {
        return;
    }
    if (kp->order != 0u && !kp->order_acked) {
        if (kp->order_op == PLINK_FW_OP_INSTALL) {
            p->out->installing |= (uint8_t)(1u << i);
        }
        return; /* one order at a time: the core resends it until it is echoed */
    }
    if (w->running[0] != '\0' && (m->told_sid != kp->sessions || !SAME(m->told, w->running))) {
        (void)order(p, i, PLINK_FW_OP_INFO, w->running);
        m->told_sid = kp->sessions;
        PUT(m->told, w->running);
        return;
    }
    /* Its facts and its state of this session, from after the newest order. */
    if (!kp->info_now || !kp->state_now || (kp->order != 0u && kp->state.order != kp->order)) {
        return;
    }
    if (v[0] == '\0' || !older(kp->info.version, v)) {
        return; /* on the target or newer: left alone */
    }
    const plink_fw_state_t *st = &kp->state;
    const bool run_v = same(st->target, v, sizeof st->target);
    if (went_back(p, i, run_v) || m->wait_next) {
        return;
    }
    if (run_v) {
        switch (st->state) {
        case PLINK_FWS_READY:
            if ((w->own_restart || SAME(w->running, v)) && m->install_sid != kp->sessions) {
                (void)order(p, i, PLINK_FW_OP_INSTALL, v);
                m->install_sid = kp->sessions;
                p->out->installing |= (uint8_t)(1u << i);
            }
            return;
        case PLINK_FWS_RECEIVING: {
            /* A relay under way: kept served; sent again when the core no longer serves it. */
            const plink_fw_image_t *im = image_by_id8(w, st->id8);
            p->need_copy[i] =
                im == &w->copy.image || (im == NULL && image_for(w, &kp->info) == NULL);
            if (im != NULL && (!kp->relay || memcmp(kp->relay_id8, st->id8, PLINK_ID8_LEN) != 0)) {
                send_image(p, i, PLINK_FW_OP_RELAY, im);
            }
            return;
        }
        case PLINK_FWS_FETCHING:
        case PLINK_FWS_KEY_WAIT:
        case PLINK_FWS_VERIFYING:
        case PLINK_FWS_INSTALLING: return;
        default: break;
        }
    }
    /* No run for V under way: the last one failed, or was refused, or there is none. */
    const bool refused = kp->order_acked && refusal(kp->order_why) &&
                         (kp->order_op == PLINK_FW_OP_FETCH || kp->order_op == PLINK_FW_OP_RELAY);
    const bool failed = run_v && (st->state == PLINK_FWS_FAILED || st->state == PLINK_FWS_STOPPED);
    if (m->op != 0u && (refused || failed)) {
        const uint8_t why = refused ? kp->order_why : st->why;
        if (m->op == PLINK_FW_OP_FETCH && relayable(why)) {
            relay(p, i, why);
        } else {
            fail(p, i, why);
        }
        return;
    }
    m->op = 0u; /* (a run it lost - it restarted - starts again) */
    if (m->retry_at != 0u && p->now < m->retry_at) {
        return;
    }
    m->retry_at = 0u;
    /* A copy loaded on the bench that fits it: relayed at once (its server has no such build). */
    if (w->copy.bench && (kp->info.can & PLINK_FWI_RELAY) != 0u &&
        image_for(w, &kp->info) == &w->copy.image) {
        send_image(p, i, PLINK_FW_OP_RELAY, &w->copy.image);
        p->need_copy[i] = true;
        return;
    }
    if ((kp->info.can & PLINK_FWI_FETCH) != 0u) {
        send_image(p, i, PLINK_FW_OP_FETCH, NULL);
    } else {
        relay(p, i, PLINK_FW_WHY_NO_NETWORK);
    }
}

/* Keypad i's row for the screens, from its newest facts. */
static void row(plan_t *p, size_t i)
{
    const plink_fw_world_t *w = p->w;
    const plink_fw_kp_t *kp = &w->kp[i];
    const plink_fw_mem_t *m = &p->out->mem[i];
    plink_fw_row_t *r = &p->out->row[i];
    const char *v = w->target;
    if (!kp->fw_cap || !kp->have_info) {
        return;
    }
    if (v[0] == '\0' || !older(kp->info.version, v)) {
        const char *ref = v[0] != '\0' ? v : w->running;
        r->state = ref[0] != '\0' && older(ref, kp->info.version) ? (uint8_t)PLINK_FWR_NEWER
                                                                   : (uint8_t)PLINK_FWR_UP_TO_DATE;
        return;
    }
    const plink_fw_state_t *st = &kp->state;
    const bool run_v = kp->have_state && same(st->target, v, sizeof st->target);
    const bool waits = SAME(m->went_back, v) || m->wait_next || m->retry_at != 0u;
    uint8_t s = PLINK_FWR_NOT_UPDATED;
    if (run_v && !waits) {
        switch (st->state) {
        case PLINK_FWS_INSTALLING: s = PLINK_FWR_RESTARTING; break;
        case PLINK_FWS_READY: s = PLINK_FWR_READY; break;
        case PLINK_FWS_RECEIVING: s = PLINK_FWR_RECEIVING; break;
        case PLINK_FWS_FETCHING:
        case PLINK_FWS_KEY_WAIT: s = PLINK_FWR_DOWNLOADING; break;
        case PLINK_FWS_VERIFYING:
            s = m->op == PLINK_FW_OP_RELAY ? PLINK_FWR_RECEIVING : PLINK_FWR_DOWNLOADING;
            break;
        default: break;
        }
    }
    if (s == PLINK_FWR_NOT_UPDATED && !waits && m->op != 0u) {
        s = m->op == PLINK_FW_OP_RELAY ? PLINK_FWR_RECEIVING : PLINK_FWR_DOWNLOADING;
    }
    r->state = s;
    if ((s == PLINK_FWR_DOWNLOADING || s == PLINK_FWR_RECEIVING) && run_v && st->size > 0u) {
        const uint64_t pc = (uint64_t)st->bytes * 100u / st->size;
        r->percent = (uint8_t)(pc > 100u ? 100u : pc);
    }
    if (s == PLINK_FWR_NOT_UPDATED) {
        r->why = m->why;
    }
}

/* ---- the step -------------------------------------------------------------------------------- */

void plink_fw_plan(const plink_fw_world_t *w, uint64_t now_ms, plink_fw_step_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (w == NULL) {
        return;
    }
    plan_t p;
    memset(&p, 0, sizeof p);
    p.w = w;
    p.now = now_ms;
    p.out = out;
    memcpy(out->mem, w->mem, sizeof out->mem);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &w->kp[i];
        plink_fw_mem_t *m = &out->mem[i];
        if (kp->device == 0u) {
            continue;
        }
        if (!SAME(m->target, w->target)) {
            /* A new target: what was waited for, refused or ordered was about the last one. */
            PUT(m->target, w->target);
            m->op = 0u;
            m->why = 0u;
            m->wait_next = false;
            m->retry_at = 0u;
            m->install_sid = 0u;
            m->again = false;
        }
        if (kp->fw) {
            decide(&p, i);
        }
        row(&p, i);
    }
    /* One copy at a time: the one held while a keypad of its facts still needs it, else the
     * first keypad's that needs one. */
    size_t pick = PLINK_KEYPADS_MAX;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (!p.need_copy[i]) {
            continue;
        }
        const plink_fw_facts_t f = facts_of(w->target, &w->kp[i].info);
        if (w->copy.state != PLINK_FWCOPY_NONE && facts_eq(&w->copy.facts, &f)) {
            pick = i;
            break;
        }
        if (pick == PLINK_KEYPADS_MAX) {
            pick = i;
        }
    }
    if (pick < PLINK_KEYPADS_MAX) {
        out->copy = facts_of(w->target, &w->kp[pick].info);
    }
}

void plink_fw_again(plink_fw_mem_t *m)
{
    if (m == NULL) {
        return;
    }
    m->op = 0u;
    m->why = 0u;
    m->wait_next = false;
    m->retry_at = 0u;
    m->went_back[0] = '\0';
    m->install_sid = 0u;
    m->again = true;
}

/* ---- names and the log ----------------------------------------------------------------------- */

const char *plink_fw_row_name(uint8_t state)
{
    static const char *const NAMES[] = {"none",     "up_to_date", "downloading", "receiving",
                                        "ready",    "restarting", "not_updated", "newer"};
    return state < sizeof NAMES / sizeof NAMES[0] ? NAMES[state] : "unknown";
}

static void log_add(plink_fw_log_ev_t *out, size_t max, size_t *n, uint8_t device, uint8_t stage,
                    uint8_t why)
{
    if (*n < max) {
        out[*n].device = device;
        out[*n].stage = stage;
        out[*n].why = why;
        (*n)++;
    }
}

size_t plink_fw_log_step(plink_fw_log_t *log, const plink_fw_world_t *w, const plink_fw_step_t *s,
                         plink_fw_log_ev_t *out, size_t max)
{
    size_t n = 0u;
    if (log == NULL || w == NULL || s == NULL || (out == NULL && max != 0u)) {
        return 0u;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &w->kp[i];
        plink_fw_log_kp_t *k = &log->kp[i];
        if (kp->device == 0u) {
            memset(k, 0, sizeof *k); /* no longer paired */
            continue;
        }
        const uint8_t d = kp->device;
        const plink_fw_row_t *r = &s->row[i];
        const char *v = w->target;
        /* Its INSTALL echoed (it restarts once its gate passes, with the master or alone), or
         * its row Restarting: the master may restart before its keypad says INSTALLING. */
        const bool took = kp->order_op == PLINK_FW_OP_INSTALL && kp->order_acked &&
                          kp->order_why == 0u && kp->have_state &&
                          kp->state.order == kp->order &&
                          same(kp->state.target, v, sizeof kp->state.target);
        if ((took || r->state == PLINK_FWR_RESTARTING) && v[0] != '\0' &&
            !SAME(k->installing, v)) {
            PUT(k->installing, v);
            log_add(out, max, &n, d, ALARM_FW_KEYPAD_INSTALLING, 0u);
        }
        if (kp->have_state) {
            const plink_fw_state_t *st = &kp->state;
            const bool moved = st->last != k->last || !SAME(k->last_version, st->last_version);
            if (k->seen && moved && st->last_version[0] != '\0') {
                if (st->last == PLINK_FW_LAST_UPDATED) {
                    log_add(out, max, &n, d, ALARM_FW_KEYPAD_UPDATED, 0u);
                } else if (st->last == PLINK_FW_LAST_ROLLED_BACK) {
                    log_add(out, max, &n, d, ALARM_FW_KEYPAD_ROLLED_BACK, 0u);
                }
            }
            k->seen = true;
            k->last = st->last;
            PUT(k->last_version, st->last_version);
        }
        if (r->state == PLINK_FWR_NOT_UPDATED && r->why != 0u && v[0] != '\0' &&
            (!SAME(k->failed, v) || k->failed_why != r->why)) {
            PUT(k->failed, v);
            k->failed_why = r->why;
            log_add(out, max, &n, d, ALARM_FW_KEYPAD_FAILED, r->why);
        }
    }
    return n;
}
