/*
 * zw_iv.c - the interview's engine: slots, questions and their answers, tries and attempts,
 * timing, questions held for a sleeping device (network.md §5, serial-api.md 6.12). The steps
 * themselves are zw_iv_steps.c. See zw_iv.h.
 */
#include "zwave_host/zw_iv.h"

#include <string.h>

#include "zw_iv_internal.h"
#include "zwave_proto/zw_cc.h"

/* A slot's states. */
enum {
    S_FREE = 0,
    S_READY,   /* the next question goes at `deadline` (0: at once), when its turn comes */
    S_SENDING, /* a question out: its end awaited (perhaps held until the device wakes) */
    S_WAITING, /* its send ended delivered: its answer awaited until `deadline` */
    S_PAUSED,  /* the link is down */
    S_WAKE     /* a sleeping device's failed attempt: the next waits for it to be heard from */
};

/* What a question waits for. */
enum {
    W_DELIVERY = 0, /* its send's end (a Set without Supervision, No Operation) */
    W_ANSWER,       /* a report (zw_iv_matches) */
    W_SUPERVISION,  /* the Supervision Report of its session */
    W_PROTOCOL,     /* GetNodeProtocolInfo's response */
    W_NODE_INFO,    /* ApplicationUpdate 84 */
    W_STATUS        /* a Set without Supervision, delivered to a node that lists Application
                     * Status: its word until `deadline` - none by then is "done" */
};

/* The interview's tags: 0xFF1 | slot | stamp. */
#define TAG_BASE 0xFF100000u
#define TAG_MASK 0xFFF00000u

static uint32_t make_tag(const zw_iv_t *iv, const zw_iv_node_t *s)
{
    const uint32_t idx = (uint32_t)(s - iv->slots);
    return TAG_BASE | (idx & 0xFFu) << 8 | (unsigned)s->seq;
}

/* The slot a tag names, when it is still the question out. */
static zw_iv_node_t *slot_of(zw_iv_t *iv, uint32_t tag)
{
    const size_t idx = (tag >> 8) & 0xFFu;
    if ((tag & TAG_MASK) != TAG_BASE || idx >= ZW_IV_SLOTS) {
        return NULL;
    }
    zw_iv_node_t *s = &iv->slots[idx];
    return s->node != 0u && s->seq == (uint8_t)tag ? s : NULL;
}

static zw_iv_node_t *find_slot(zw_iv_t *iv, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node == node) {
            return &iv->slots[i];
        }
    }
    return NULL;
}

static const zw_iv_node_t *find_slot_c(const zw_iv_t *iv, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node == node) {
            return &iv->slots[i];
        }
    }
    return NULL;
}

static void emit(zw_iv_t *iv, zw_iv_event_kind_t kind, uint16_t node, uint8_t step)
{
    if (iv->port.event != NULL) {
        const zw_iv_event_t ev = {kind, node, step};
        iv->port.event(iv->port.ctx, &ev);
    }
}

static void changed(zw_iv_t *iv)
{
    if (iv->port.changed != NULL) {
        iv->port.changed(iv->port.ctx);
    }
}

static void forget_stopped(zw_iv_t *iv, uint16_t node)
{
    for (size_t i = 0; i < ZW_IV_STOPPED_MAX; i++) {
        if (iv->stopped[i] == node) {
            iv->stopped[i] = 0u;
        }
    }
}

/* BL-152: its re-read stopped - it goes first; the entries before the first free place move
 * down into it, and only a full list forgets its oldest. */
static void remember_stopped(zw_iv_t *iv, uint16_t node)
{
    forget_stopped(iv, node);
    size_t hole = ZW_IV_STOPPED_MAX - 1u;
    for (size_t i = 0; i < ZW_IV_STOPPED_MAX; i++) {
        if (iv->stopped[i] == 0u) {
            hole = i;
            break;
        }
    }
    memmove(&iv->stopped[1], &iv->stopped[0], hole * sizeof iv->stopped[0]);
    iv->stopped[0] = node;
}

/* ---- Re-reads' scratch records (BL-152) ------------------------------------------------------ */

static bool scratch_free(const zw_iv_t *iv, size_t k)
{
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node != 0u && iv->slots[i].scratch == k + 1u) {
            return false;
        }
    }
    return true;
}

static bool any_scratch_free(const zw_iv_t *iv)
{
    for (size_t k = 0; k < ZW_IV_REREADS; k++) {
        if (scratch_free(iv, k)) {
            return true;
        }
    }
    return false;
}

/* A scratch record for a re-read, started from what the reading does not ask again - who the node
 * is, its generation, its security classes and DSK (granted at its inclusion, kept as Z-Wave JS
 * keeps them), the raw items this build cannot hold (kept and written back as the stored format
 * promises: fields it does not know, and end points, CC lists, notification types and firmware
 * targets past its limits, written by a later firmware; this build cannot read them, so a re-read
 * cannot refresh them) - and its protocol info until step 1 reads it again. Everything else (its
 * CCs, end points, notification types, lifeline...) is read fresh, as Z-Wave JS's refreshInfo
 * does. */
static void fill_scratch(zw_node_record_t *r, const zw_node_record_t *live)
{
    memset(r, 0, sizeof *r);
    r->node_id = live->node_id;
    r->generation = live->generation;
    r->has_protocol = live->has_protocol;
    r->is_listening = live->is_listening;
    r->is_flirs = live->is_flirs;
    r->is_routing = live->is_routing;
    r->supports_long_range = live->supports_long_range;
    r->basic_class = live->basic_class;
    r->generic_class = live->generic_class;
    r->specific_class = live->specific_class;
    r->security_classes = live->security_classes;
    r->has_dsk = live->has_dsk;
    memcpy(r->dsk, live->dsk, sizeof r->dsk);
    memcpy(r->unknown, live->unknown, sizeof r->unknown);
    r->unknown_len = live->unknown_len;
    r->interview_step = (uint8_t)ZW_IV_STEP_DONE;
}

/* A free scratch record for the slot's re-read (fill_scratch). False: none free. */
static bool take_scratch(zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *live)
{
    for (size_t k = 0; k < ZW_IV_REREADS; k++) {
        if (scratch_free(iv, k)) {
            fill_scratch(&iv->reread[k], live);
            s->scratch = (uint8_t)(k + 1u);
            return true;
        }
    }
    return false;
}

/* READY of a re-read: the new reading replaces the old facts in one step. Kept from the node's own
 * record: what the reading never changes (its ID, generation, security classes, DSK), the raw
 * items this build cannot hold (fields it does not know, and end points, CC lists, notification
 * types and firmware targets past its limits, written by a later firmware: kept and written back
 * as the stored format promises - this build cannot read them again, so a re-read cannot refresh
 * them), and an end point's Alarm Sensor types the reading could not ask (no try reached it: the
 * old answer is still the best one, and the zone keeps using it). */
static void commit_scratch(zw_node_record_t *live, const zw_node_record_t *scratch)
{
    zw_node_record_t *r = live;
    const zw_node_record_t old = *live;
    *r = *scratch;
    r->node_id = old.node_id;
    r->generation = old.generation;
    r->security_classes = old.security_classes != ZW_NODE_SECURITY_UNKNOWN
                              ? (unsigned)old.security_classes
                              : (unsigned)scratch->security_classes;
    r->has_dsk = old.has_dsk;
    memcpy(r->dsk, old.dsk, sizeof r->dsk);
    memcpy(r->unknown, old.unknown, sizeof r->unknown);
    r->unknown_len = old.unknown_len;
    r->interview_step = (uint8_t)ZW_IV_STEP_DONE;
    for (uint8_t e = 0u; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        zw_node_endpoint_t *x = &r->endpoints[e];
        const zw_node_endpoint_t *o = &old.endpoints[e];
        if ((e == 0u || x->used) && !x->has_alarm_sensor_mask && o->has_alarm_sensor_mask &&
            zw_iv_endpoint_has_cc(r, e, ZW_CC_ALARM_SENSOR)) {
            x->has_alarm_sensor_mask = true;
            memcpy(x->alarm_sensor_mask, o->alarm_sensor_mask, sizeof x->alarm_sensor_mask);
            x->alarm_sensor_mask_len = o->alarm_sensor_mask_len;
        }
    }
}

/* The slot freed first (the callbacks see it ended), then the word. */
static void end(zw_iv_t *iv, zw_iv_node_t *s, zw_iv_event_kind_t kind)
{
    const uint16_t node = s->node;
    const uint8_t step = s->step;
    if (s->refresh && kind == ZW_IV_EV_STALLED) {
        remember_stopped(iv, node);
    }
    memset(s, 0, sizeof *s);
    emit(iv, kind, node, step);
    emit(iv, ZW_IV_EV_ENDED, node, step);
}

static void to_ready(zw_iv_node_t *s, uint64_t at)
{
    s->state = S_READY;
    s->deadline = at;
    s->parked = false;
}

/* The step starts again from its beginning (a failed attempt). */
static void reset_cursor(zw_iv_node_t *s)
{
    s->ep = 0u;
    s->item = 0u;
    s->phase = 0u;
    s->tries = 0u;
    s->ep_count = 0u;
    s->mca_groups = 0u;
    s->agi = 0u;
    s->secure_taken = false;
}

/* ---- Tries and attempts ---------------------------------------------------------------------- */

static void attempt_failed(zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *rec)
{
    s->attempts++;
    s->seq++;
    if (s->attempts >= ZW_IV_ATTEMPTS) {
        iv->stats.stalled++;
        end(iv, s, ZW_IV_EV_STALLED);
        return;
    }
    reset_cursor(s);
    if (rec != NULL && zw_iv_sleeping(rec)) {
        /* its next attempt at its next wake-up: all of them in one wake-up would be spent on a
         * device that answers nothing. A re-read gives back its scratch record meanwhile (a
         * device that sleeps on must not keep others' re-reads waiting) and reads from step 1
         * again then (BL-152) */
        if (s->scratch != 0u) {
            s->scratch = 0u;
            s->step = (uint8_t)ZW_IV_STEP_PROTOCOL;
        }
        s->state = S_WAKE;
        s->parked = false;
        if (!s->told_asleep) {
            s->told_asleep = true;
            emit(iv, ZW_IV_EV_WAKE_NEEDED, s->node, s->step);
        }
        return;
    }
    to_ready(s, 0u);
}

static void silence(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec)
{
    s->tries = 0u;
    s->seq++;
    const uint32_t wait = zw_iv_on_silence(iv, s, rec);
    to_ready(s, wait != 0u ? iv->now + wait : 0u);
}

/* A try failed: the same question again (`at`: not before; 0 at once), or the attempt is over
 * after the last (the 3rd, or the question's own count). An optional question's silence is an
 * answer - except a Security 2 query whose last try was not delivered (network.md §5 step 4: Z-Wave
 * JS's Controller_CallbackNOK aborts the interview attempt and records nothing). */
static void fail_try_at(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, uint64_t at)
{
    if (rec == NULL) {
        memset(s, 0, sizeof *s); /* forgotten meanwhile */
        return;
    }
    s->tries++;
    s->seq++;
    const unsigned max = s->max_tries != 0u ? (unsigned)s->max_tries : ZW_IV_TRIES;
    if (s->tries < max) {
        to_ready(s, at);
    } else if (s->optional && !(s->nok_aborts && s->tx_failed)) {
        silence(iv, s, rec);
    } else {
        attempt_failed(iv, s, rec);
    }
}

static void fail_try(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec)
{
    fail_try_at(iv, s, rec, 0u);
}

/* The question is answered (rx: the command; NULL: a Set done). */
static void answered(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, const zw_s2_rx_t *rx)
{
    s->seq++; /* its send's end, still to come, is ignored */
    s->delivered = true;
    const iv_verdict_t v = zw_iv_on_answer(iv, s, rec, rx);
    if (v == IV_MORE) {
        /* a part of an answer in parts: the next part is awaited as long again (the wait
         * restarts; the send's end, if still to come, no longer matters) */
        s->state = S_WAITING;
        s->parked = false;
        s->deadline = iv->now + ZW_IV_REPORT_MS;
        return;
    }
    if (v == IV_RETRY) {
        s->tries++;
        if (s->tries < (s->max_tries != 0u ? (unsigned)s->max_tries : ZW_IV_TRIES)) {
            to_ready(s, 0u);
        } else {
            attempt_failed(iv, s, rec);
        }
        return;
    }
    if (v == IV_NEXT) {
        s->tries = 0u;
    }
    to_ready(s, 0u);
}

/* ---- Asking ---------------------------------------------------------------------------------- */

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *r);

static bool ask_protocol(zw_iv_t *iv, zw_iv_node_t *s)
{
    const zw_api_node_protocol_info_req_t q = {s->node};
    uint8_t buf[4];
    const size_t n = zw_api_node_protocol_info_req_build(buf, sizeof buf, iv->nid, &q);
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_GET_NODE_PROTOCOL_INFO;
    req.payload = buf;
    req.payload_len = n;
    req.expect = ZW_EXPECT_RESPONSE;
    req.prio = ZW_PRIO_INTERVIEW;
    req.done = request_done;
    req.ctx = iv;
    req.tag = make_tag(iv, s);
    return n > 0u && iv->port.submit != NULL && iv->port.submit(iv->port.ctx, &req) == ZW_SUBMIT_OK;
}

/* A question to the node: Multi Channel for an end point, Supervision for a Set to a node that
 * has it (command-classes.md §1: built from the inside out). */
static void ask_node(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, const iv_ask_t *a)
{
    uint8_t mc[ZW_IV_CMD_MAX + 4u];
    uint8_t wire[ZW_IV_CMD_MAX + 8u];
    const uint8_t *out = a->cmd;
    size_t n = a->len;
    if (s->tries == 0u) {
        s->delivered = false; /* a new question: none of its tries has reached the device yet */
    }
    memcpy(s->cmd, a->cmd, a->len);
    s->cmd_len = a->len;
    s->cmd_ep = a->ep;
    s->cls = a->cls;
    s->optional = a->optional;
    s->max_tries = a->tries;
    s->nok_aborts = a->nok_aborts;
    s->tx_failed = false;
    s->spv_session = 0u;
    s->wait = a->supervise ? W_SUPERVISION : a->answer ? W_ANSWER : W_DELIVERY;
    if (a->ep != 0u) {
        n = zw_cc_wrap_multi_channel(mc, sizeof mc, 0u, a->ep, false, out, n);
        out = mc;
    }
    if (a->supervise && n > 0u) {
        s->session = (uint8_t)((unsigned)s->session % 63u + 1u);
        s->spv_session = s->session;
        n = zw_cc_wrap_supervision(wire, sizeof wire, s->session, false, out, n);
        out = wire;
    }
    s->seq++;
    bool held = false;
    const zw_s2_result_t r =
        n > 0u ? iv->port.send(iv->port.ctx, s->node, (zw_sec_class_t)a->cls, out, n,
                               make_tag(iv, s), &held)
               : ZW_S2_INVALID;
    if (r == ZW_S2_OK) {
        s->state = S_SENDING;
        s->sent_at = iv->now;
        s->parked = false;
        iv->stats.questions++;
        if (held) {
            zw_iv_asleep(iv, iv->now, s->node);
        }
    } else if (r == ZW_S2_BUSY) {
        to_ready(s, iv->now + ZW_IV_RETRY_MS);
    } else {
        fail_try(iv, s, rec); /* it cannot go (too long, no key): counted, so it cannot loop */
    }
}

static zw_node_record_t *record(zw_iv_t *iv, uint16_t node)
{
    return zw_node_find(iv->nodes, node);
}

/* The record a slot's answers go into: its re-read's scratch record (BL-152), else the node's own;
 * NULL once the node is gone from the cache. */
static zw_node_record_t *rec_of(zw_iv_t *iv, const zw_iv_node_t *s)
{
    zw_node_record_t *live = record(iv, s->node);
    if (live == NULL || s->scratch == 0u || s->scratch > ZW_IV_REREADS) {
        return live;
    }
    return &iv->reread[s->scratch - 1u];
}

/* The slot's next question: steps that are done or do not apply are recorded on the way. */
static void ask(zw_iv_t *iv, zw_iv_node_t *s)
{
    for (unsigned guard = 0; guard < 4u * ZW_IV_STEP_DONE; guard++) {
        zw_node_record_t *rec = rec_of(iv, s);
        if (rec == NULL) {
            memset(s, 0, sizeof *s); /* forgotten meanwhile */
            return;
        }
        iv_ask_t a;
        memset(&a, 0, sizeof a);
        zw_iv_plan(iv, s, rec, &a);
        switch (a.kind) {
        case IV_STEP_DONE: {
            const uint8_t done = s->step;
            if (s->alarm_only && !s->alarm_stored) {
                /* BL-130: no try reached the device (powered off, out of reach): nothing learned,
                 * nothing written, no READY - field 12 stays missing, and the next network check
                 * (at link up) asks again */
                const uint16_t node = s->node;
                memset(s, 0, sizeof *s);
                emit(iv, ZW_IV_EV_ENDED, node, (uint8_t)ZW_IV_STEP_DONE);
                return;
            }
            if (s->alarm_only) {
                /* BL-130: only step 12's Alarm Sensor question - field 0E was done already and
                 * stays so; the types (or "none") are stored, and the zone source reads them */
                changed(iv);
                iv->stats.ready++;
                s->step = (uint8_t)ZW_IV_STEP_DONE; /* READY says it, as for any interview */
                end(iv, s, ZW_IV_EV_READY);
                return;
            }
            if (s->scratch == 0u) {
                rec->interview_step = done;
                changed(iv);
            }
            /* BL-152: a re-read stores nothing before READY - its node's record is untouched */
            if (done >= ZW_IV_STEP_DONE) {
                if (s->scratch != 0u) {
                    zw_node_record_t *live = record(iv, s->node);
                    commit_scratch(live, rec);
                    changed(iv);
                }
                iv->stats.ready++;
                end(iv, s, ZW_IV_EV_READY);
                return;
            }
            s->step = (uint8_t)((unsigned)done + 1u);
            reset_cursor(s);
            if (done == ZW_IV_STEP_PROTOCOL) {
                emit(iv, ZW_IV_EV_PROTOCOL, s->node, s->step);
            }
            continue;
        }
        case IV_NOT_USABLE:
            end(iv, s, ZW_IV_EV_NOT_USABLE);
            return;
        case IV_ASK_PROTOCOL:
            s->wait = W_PROTOCOL;
            s->optional = false;
            s->seq++;
            if (ask_protocol(iv, s)) {
                s->state = S_SENDING;
                s->sent_at = iv->now;
                iv->stats.questions++;
            } else {
                to_ready(s, iv->now + ZW_IV_RETRY_MS);
            }
            return;
        case IV_ASK_NODE_INFO: {
            bool held = false;
            s->wait = W_NODE_INFO;
            s->optional = false;
            s->seq++;
            if (iv->port.node_info != NULL &&
                iv->port.node_info(iv->port.ctx, s->node, make_tag(iv, s), &held)) {
                s->state = S_SENDING;
                s->sent_at = iv->now;
                s->parked = false;
                iv->stats.questions++;
                if (held) {
                    zw_iv_asleep(iv, iv->now, s->node);
                }
            } else {
                to_ready(s, iv->now + ZW_IV_RETRY_MS);
            }
            return;
        }
        case IV_ASK_NODE:
        default:
            ask_node(iv, s, rec, &a);
            return;
        }
    }
    to_ready(s, iv->now + ZW_IV_RETRY_MS); /* the steps did not settle: never spin */
}

/* Questions out now that count against ZW_IV_ACTIVE: a listening device's. */
static size_t active(const zw_iv_t *iv)
{
    size_t n = 0u;
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        const zw_iv_node_t *s = &iv->slots[i];
        if (s->node != 0u && (s->state == S_SENDING || s->state == S_WAITING) && !s->parked) {
            const zw_node_record_t *rec = zw_node_find_const(iv->nodes, s->node);
            if (rec == NULL || !zw_iv_sleeping(rec)) {
                n++;
            }
        }
    }
    return n;
}

/* A sleeping device's question goes whenever it can (it is awake only briefly), and so does the
 * question for a device's protocol info (the module answers it; a device that just joined may be
 * a sleeper); a listening device's waits for its turn. */
static bool may_ask(const zw_iv_t *iv, const zw_iv_node_t *s, size_t busy)
{
    if (s->state != S_READY || !iv->link_up || iv->hold) {
        return false;
    }
    const zw_node_record_t *rec = zw_node_find_const(iv->nodes, s->node);
    return busy < ZW_IV_ACTIVE || rec == NULL || !rec->has_protocol || zw_iv_sleeping(rec);
}

/* ---- Answers --------------------------------------------------------------------------------- */

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_iv_t *iv = ctx;
    zw_iv_node_t *s = slot_of(iv, tag);
    if (s == NULL || s->state != S_SENDING || s->wait != W_PROTOCOL) {
        return;
    }
    zw_node_record_t *rec = rec_of(iv, s);
    if (rec == NULL) {
        memset(s, 0, sizeof *s);
        return;
    }
    if (r->status == ZW_TX_LINK_DOWN) {
        return; /* paused already; asked again at the next link up */
    }
    if (r->status == ZW_TX_OK && r->response != NULL &&
        zw_iv_on_protocol(s, rec, r->response, r->response_len)) {
        s->tries = 0u;
        s->seq++;
        to_ready(s, 0u);
        return;
    }
    fail_try(iv, s, rec);
}

bool zw_iv_sent(zw_iv_t *iv, uint64_t now, uint32_t tag, zw_s2_result_t result)
{
    iv->now = now;
    zw_iv_node_t *s = slot_of(iv, tag);
    if (s == NULL || s->state != S_SENDING) {
        return false;
    }
    zw_node_record_t *rec = rec_of(iv, s);
    if (rec == NULL) {
        memset(s, 0, sizeof *s);
        return true;
    }
    s->tx_failed = result == ZW_S2_FAILED; /* not delivered: a transmission error */
    if (result == ZW_S2_OK) {
        s->parked = false;
        s->delivered = true;
        if (s->wait == W_DELIVERY && s->cmd_len >= 2u &&
            zw_iv_has_cc(rec, ZW_CC_APPLICATION_STATUS)) {
            /* command-classes.md §4.25: a node that can refuse a Set says so after hearing it -
             * its word is awaited as an answer would be; none is "done" */
            s->wait = W_STATUS;
            s->state = S_WAITING;
            s->deadline = now + ZW_IV_REPORT_MS + (now - s->sent_at);
        } else if (s->wait == W_DELIVERY) {
            answered(iv, s, rec, NULL);
        } else {
            /* serial-api.md 6.12: 1 s + the round trip after the callback */
            s->state = S_WAITING;
            s->deadline = now + ZW_IV_REPORT_MS + (now - s->sent_at);
        }
    } else if (result == ZW_S2_CANCELLED) {
        memset(s, 0, sizeof *s); /* the node is gone from the security manager */
    } else if (result == ZW_S2_NOT_DECODED && s->optional) {
        s->delivered = true; /* it heard, and could not read it */
        silence(iv, s, rec); /* the probe: this class is not the node's */
    } else {
        fail_try(iv, s, rec);
    }
    return true;
}

/* command-classes.md §4.25: Application Status from the node, from the end point asked, while a
 * Set of the slot is out (sent and not parked, or delivered and its word awaited). Rejected
 * Request: a failed try at once. Busy: a failed try whose next one waits the node's wait time (1 s
 * when it names none; ZW_IV_BUSY_MAX_S at most). True when it was that. */
static bool application_status(zw_iv_t *iv, zw_iv_node_t *s, const zw_s2_rx_t *rx)
{
    if (rx->len < 2u || rx->command[0] != (uint8_t)ZW_CC_APPLICATION_STATUS) {
        return false;
    }
    const bool set_out =
        ((s->state == S_SENDING && !s->parked) &&
         (s->wait == W_DELIVERY || s->wait == W_SUPERVISION)) ||
        (s->state == S_WAITING && (s->wait == W_SUPERVISION || s->wait == W_STATUS));
    const uint8_t ep = rx->multi_channel ? (unsigned)rx->source_endpoint : 0u;
    if (!set_out || s->cmd_len < 2u || ep != s->cmd_ep) {
        return false;
    }
    zw_node_record_t *rec = rec_of(iv, s);
    zw_cc_application_busy_t busy;
    zw_cc_application_rejected_request_t rejected;
    if (zw_cc_application_rejected_request_parse(rx->command, rx->len, &rejected)) {
        fail_try(iv, s, rec);
        return true;
    }
    if (!zw_cc_application_busy_parse(rx->command, rx->len, &busy)) {
        return false;
    }
    uint64_t wait = ZW_IV_BUSY_MS;
    if (busy.status == ZW_APPLICATION_BUSY_WAIT && busy.wait_s != 0u) {
        const unsigned w = busy.wait_s < ZW_IV_BUSY_MAX_S ? (unsigned)busy.wait_s : ZW_IV_BUSY_MAX_S;
        wait = (uint64_t)w * 1000u;
    }
    fail_try_at(iv, s, rec, iv->now + wait);
    return true;
}

void zw_iv_received(zw_iv_t *iv, uint64_t now, const zw_s2_rx_t *rx)
{
    iv->now = now;
    zw_iv_node_t *s = find_slot(iv, rx->node);
    if (s != NULL && rx->supervised) {
        s->spv_seen = true; /* it speaks Supervision (step 6 believes that over a "version 0") */
    }
    if (s != NULL && application_status(iv, s, rx)) {
        return;
    }
    if (s == NULL || (s->state != S_SENDING && s->state != S_WAITING) ||
        (s->wait != W_ANSWER && s->wait != W_SUPERVISION) || !zw_iv_matches(s, rx)) {
        return;
    }
    zw_node_record_t *rec = rec_of(iv, s);
    if (rec == NULL) {
        memset(s, 0, sizeof *s);
        return;
    }
    if (s->wait == W_SUPERVISION) {
        zw_cc_supervision_report_t sr;
        if (!zw_cc_supervision_report_parse(rx->command, rx->len, &sr)) {
            return;
        }
        s->spv_seen = true; /* it answered a Supervision Get */
        /* Supervision CC (SDS13782): the panel's Gets never ask for status updates, so the node
         * sends exactly one Report; WORKING (with a duration) is then its answer - the Set is
         * accepted and under way - and no final Report follows. Z-Wave JS counts it as success. FAIL fails; NO_SUPPORT is done too. */
        if (sr.status == ZW_SUPERVISION_STATUS_FAIL) {
            fail_try(iv, s, rec);
            return;
        }
        answered(iv, s, rec, NULL); /* FF done, 01 working, 00 not supported: done too */
        return;
    }
    answered(iv, s, rec, rx);
}

void zw_iv_node_info(zw_iv_t *iv, uint64_t now, uint16_t node, const uint8_t *ccs, size_t len)
{
    iv->now = now;
    zw_iv_node_t *s = find_slot(iv, node);
    if (s == NULL || (s->state != S_SENDING && s->state != S_WAITING) || s->wait != W_NODE_INFO) {
        return;
    }
    zw_node_record_t *rec = rec_of(iv, s);
    if (rec == NULL) {
        memset(s, 0, sizeof *s);
        return;
    }
    zw_iv_on_node_info(s, rec, ccs, len);
    s->tries = 0u;
    s->seq++;
    to_ready(s, 0u);
}

void zw_iv_node_info_failed(zw_iv_t *iv, uint64_t now, uint16_t node)
{
    iv->now = now;
    zw_iv_node_t *s = find_slot(iv, node);
    if (s == NULL || (s->state != S_SENDING && s->state != S_WAITING) || s->wait != W_NODE_INFO) {
        return;
    }
    fail_try(iv, s, rec_of(iv, s));
}

void zw_iv_asleep(zw_iv_t *iv, uint64_t now, uint16_t node)
{
    iv->now = now;
    zw_iv_node_t *s = find_slot(iv, node);
    /* a question held again - also RequestNodeInfo whose answer said the device did not hear */
    if (s == NULL || s->parked ||
        !(s->state == S_SENDING || (s->state == S_WAITING && s->wait == W_NODE_INFO))) {
        return;
    }
    s->state = S_SENDING;
    s->parked = true;
    iv->stats.parked++;
    if (!s->told_asleep) {
        s->told_asleep = true;
        emit(iv, ZW_IV_EV_WAKE_NEEDED, node, s->step);
    }
}

void zw_iv_went(zw_iv_t *iv, uint64_t now, uint32_t tag)
{
    iv->now = now;
    zw_iv_node_t *s = slot_of(iv, tag);
    if (s == NULL || s->state != S_SENDING) {
        return;
    }
    s->parked = false;
    s->told_asleep = false;
    s->sent_at = now; /* the round trip counts from here */
}

static bool reclaim_parked(zw_iv_t *iv, uint16_t except);

void zw_iv_awake(zw_iv_t *iv, uint64_t now, uint16_t node)
{
    iv->now = now;
    zw_iv_node_t *s = find_slot(iv, node);
    if (s == NULL || s->state != S_WAKE) {
        return;
    }
    if (s->refresh && s->scratch == 0u) {
        /* BL-152: a sleeper's re-read starts at its wake-up, as Z-Wave JS's; with every scratch
         * record in use it waits for the next one */
        const zw_node_record_t *live = record(iv, node);
        if (live == NULL) {
            memset(s, 0, sizeof *s);
            return;
        }
        /* none free: a parked sleeper's is taken back (network.md: given back "when another
         * device needs one"), never this node's own - it holds none */
        if (!take_scratch(iv, s, live) &&
            !(reclaim_parked(iv, node) && take_scratch(iv, s, live))) {
            return;
        }
    }
    s->told_asleep = false;
    to_ready(s, 0u);
}

bool zw_iv_is_tag(uint32_t tag)
{
    return (tag & TAG_MASK) == TAG_BASE;
}

/* ---- Slots ----------------------------------------------------------------------------------- */

void zw_iv_init(zw_iv_t *iv, const zw_iv_port_t *port, zw_node_cache_t *nodes, zw_s2_t *s2)
{
    memset(iv, 0, sizeof *iv);
    iv->port = *port;
    iv->nodes = nodes;
    iv->s2 = s2;
    iv->nid = ZW_NID_8;
    iv->own_node = 1u;
}

void zw_iv_link_up(zw_iv_t *iv, uint64_t now, uint16_t own_node, zw_nid_width_t nid)
{
    iv->now = now;
    iv->link_up = true;
    iv->own_node = own_node != 0u ? (unsigned)own_node : 1u;
    iv->nid = nid == ZW_NID_16 ? ZW_NID_16 : ZW_NID_8;
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node != 0u && iv->slots[i].state == S_PAUSED) {
            to_ready(&iv->slots[i], 0u);
        }
    }
}

void zw_iv_hold(zw_iv_t *iv, bool hold)
{
    iv->hold = hold;
    for (size_t i = 0; hold && i < ZW_IV_SLOTS; i++) {
        zw_iv_node_t *s = &iv->slots[i];
        if (s->node != 0u && s->state == S_SENDING && s->parked) {
            /* held for its device's wake-up, never sent (the owner takes it back from the
             * device's queue): asked again once the hold ends, no try spent */
            s->seq++;
            to_ready(s, 0u);
        }
    }
}

void zw_iv_link_down(zw_iv_t *iv, uint64_t now)
{
    iv->now = now;
    iv->link_up = false;
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        zw_iv_node_t *s = &iv->slots[i];
        if (s->node != 0u && s->state != S_WAKE) {
            s->state = S_PAUSED;
            s->parked = false;
            s->seq++; /* what was out ends unheard */
        }
    }
}

static zw_iv_node_t *begin(zw_iv_t *iv, uint16_t node, uint8_t step, bool alarm_only,
                           bool refresh);

bool zw_iv_start(zw_iv_t *iv, uint64_t now, uint16_t node)
{
    iv->now = now;
    const zw_node_record_t *rec = zw_node_find_const(iv->nodes, node);
    if (rec == NULL) {
        return false;
    }
    /* BL-130: a finished interview from before step 12 asked Alarm Sensor types asks them now */
    const bool alarm_only = rec->interview_step >= ZW_IV_STEP_DONE && zw_iv_alarm_types_missing(rec);
    if (rec->interview_step >= ZW_IV_STEP_DONE && !alarm_only) {
        return false;
    }
    zw_iv_node_t *s = find_slot(iv, node);
    if (s != NULL) {
        if (s->state == S_PAUSED && iv->link_up) {
            to_ready(s, 0u);
        }
        return true;
    }
    return begin(iv, node,
                 alarm_only ? (uint8_t)ZW_IV_STEP_BINARY
                            : (uint8_t)((unsigned)rec->interview_step + 1u),
                 alarm_only, false) != NULL;
}

/* A free slot runs it from `step`; NULL: none free. */
static zw_iv_node_t *begin(zw_iv_t *iv, uint16_t node, uint8_t step, bool alarm_only,
                           bool refresh)
{
    zw_iv_node_t *s = NULL;
    for (size_t i = 0; i < ZW_IV_SLOTS && s == NULL; i++) {
        if (iv->slots[i].node == 0u) {
            s = &iv->slots[i];
        }
    }
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->node = node;
    s->step = step;
    s->alarm_only = alarm_only;
    s->refresh = refresh;
    /* command-classes.md §4.14: its session IDs count 1..63 from a random start */
    uint8_t r = 0u;
    if (iv->s2 != NULL && iv->s2->crypto.random != NULL &&
        !iv->s2->crypto.random(iv->s2->crypto.ctx, &r, 1u)) {
        r = 0u;
    }
    s->session = (uint8_t)((unsigned)r % 63u);
    s->seq = r;
    if (iv->link_up) {
        to_ready(s, 0u);
    } else {
        s->state = S_PAUSED;
    }
    iv->stats.started++;
    return s;
}

/* BL-152: a scratch record taken back from a sleeper's re-read whose question waits for the
 * device's wake-up (parked), for a device that can be read now: that reading starts again from
 * step 1 at its next wake-up, as a re-read that waits for one does. False: none parked. */
static bool reclaim_parked(zw_iv_t *iv, uint16_t except)
{
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        zw_iv_node_t *s = &iv->slots[i];
        if (s->node == 0u || s->node == except || !s->refresh || s->scratch == 0u ||
            s->state != S_SENDING || !s->parked) {
            continue;
        }
        const zw_node_record_t *live = record(iv, s->node);
        if (live == NULL || !zw_iv_sleeping(live)) {
            continue;
        }
        s->scratch = 0u;
        s->step = (uint8_t)ZW_IV_STEP_PROTOCOL;
        reset_cursor(s);
        s->state = S_WAKE;
        s->parked = false;
        s->told_asleep = true; /* WAKE_NEEDED was said when it parked */
        s->seq++;              /* its held question is void: the owner takes it out of the queue */
        emit(iv, ZW_IV_EV_RESTARTED, s->node, s->step);
        return true;
    }
    return false;
}

static bool restart(zw_iv_t *iv, uint64_t now, uint16_t node, bool may_wait)
{
    zw_node_record_t *rec = record(iv, node);
    if (rec == NULL) {
        return false;
    }
    const zw_iv_node_t *cur = find_slot_c(iv, node);
    if (cur != NULL && cur->refresh && cur->scratch == 0u) {
        return true; /* BL-152: waiting for its wake-up - left alone, as Z-Wave JS's */
    }
    if (cur != NULL && cur->refresh) {
        /* BL-152: a re-read under way starts again from step 1, as Z-Wave JS's refreshInfo
         * cancels a running interview (Controller_InterviewRestarted) and begins a fresh one; it
         * keeps its scratch record, filled again from the node's own record */
        const uint8_t k = cur->scratch;
        const bool held = cur->state == S_SENDING && cur->parked;
        zw_iv_stop(iv, node);
        iv->now = now;
        zw_iv_node_t *s = begin(iv, node, (uint8_t)ZW_IV_STEP_PROTOCOL, false, true);
        if (s == NULL) {
            return false; /* never: its own slot was freed just now */
        }
        fill_scratch(&iv->reread[k - 1u], rec);
        s->scratch = k;
        if (held) {
            emit(iv, ZW_IV_EV_RESTARTED, node, s->step); /* its held question is void */
        }
        return true;
    }
    if (rec->interview_step >= ZW_IV_STEP_DONE && !zw_iv_is_controller(rec)) {
        /* BL-152: re-read into a scratch record - the node's own record, and so its inputs and
         * what is stored, keep the old facts until READY replaces them. Z-Wave JS waits for the
         * wake-up of a sleeper that supports Wake Up (Node.ts refreshInfo); any other starts now,
         * and so does every device when the caller says not to wait (waitForWakeup: false) */
        const bool wait = may_wait && zw_iv_sleeping(rec) && zw_iv_has_cc(rec, ZW_CC_WAKE_UP);
        if (!wait && !any_scratch_free(iv) && !reclaim_parked(iv, node)) {
            return false; /* busy: every scratch record is in use */
        }
        zw_iv_stop(iv, node);
        iv->now = now;
        zw_iv_node_t *s = begin(iv, node, (uint8_t)ZW_IV_STEP_PROTOCOL, false, true);
        if (s == NULL) {
            return false;
        }
        if (wait) {
            /* Z-Wave JS waits for a sleeper's wake-up before it forgets anything: nothing is
             * asked, and no scratch record held, until it wakes (zw_iv_awake) */
            s->state = S_WAKE;
            s->told_asleep = true;
            emit(iv, ZW_IV_EV_WAKE_NEEDED, node, s->step);
        } else {
            (void)take_scratch(iv, s, rec);
        }
        return true;
    }
    zw_iv_stop(iv, node);
    rec->interview_step = 0u;
    changed(iv);
    return zw_iv_start(iv, now, node);
}

bool zw_iv_restart(zw_iv_t *iv, uint64_t now, uint16_t node)
{
    return restart(iv, now, node, true);
}

bool zw_iv_restart_now(zw_iv_t *iv, uint64_t now, uint16_t node)
{
    return restart(iv, now, node, false);
}

void zw_iv_stop(zw_iv_t *iv, uint16_t node)
{
    forget_stopped(iv, node);
    zw_iv_node_t *s = find_slot(iv, node);
    if (s != NULL) {
        memset(s, 0, sizeof *s);
    }
}

bool zw_iv_running(const zw_iv_t *iv, uint16_t node)
{
    return find_slot_c(iv, node) != NULL;
}

bool zw_iv_asking(const zw_iv_t *iv, uint16_t node)
{
    const zw_iv_node_t *s = find_slot_c(iv, node);
    return s != NULL && !(s->refresh && s->scratch == 0u && s->state == S_WAKE);
}

void zw_iv_wake_interval_heard(zw_iv_t *iv, uint16_t node, uint32_t interval_s, uint8_t notify)
{
    const zw_iv_node_t *s = find_slot_c(iv, node);
    if (s == NULL || !s->refresh || s->scratch == 0u || s->scratch > ZW_IV_REREADS) {
        return;
    }
    zw_node_record_t *r = &iv->reread[s->scratch - 1u];
    if (r->has_wake_up) {
        r->wake_up_interval_seconds = interval_s;
        r->wake_up_notify_node_id = notify;
    }
}

bool zw_iv_setting_up(const zw_iv_t *iv, uint16_t node)
{
    const zw_iv_node_t *s = find_slot_c(iv, node);
    return s != NULL && !s->alarm_only;
}

bool zw_iv_refreshing(const zw_iv_t *iv, uint16_t node)
{
    const zw_iv_node_t *s = find_slot_c(iv, node);
    return s != NULL && s->refresh;
}

bool zw_iv_reread_stopped(const zw_iv_t *iv, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_IV_STOPPED_MAX; i++) {
        if (iv->stopped[i] == node) {
            return true;
        }
    }
    return false;
}

uint8_t zw_iv_step(const zw_iv_t *iv, uint16_t node)
{
    const zw_iv_node_t *s = find_slot_c(iv, node);
    return s != NULL ? (unsigned)s->step : 0u;
}

void zw_iv_poll(zw_iv_t *iv, uint64_t now)
{
    iv->now = now;
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        zw_iv_node_t *s = &iv->slots[i];
        if (s->node == 0u || s->state != S_WAITING || now < s->deadline) {
            continue;
        }
        zw_node_record_t *rec = rec_of(iv, s);
        if (s->wait == W_STATUS && rec != NULL) {
            answered(iv, s, rec, NULL); /* nothing said against the Set: done */
            continue;
        }
        iv->stats.timeouts++;
        fail_try(iv, s, rec);
    }
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        zw_iv_node_t *s = &iv->slots[i];
        if (s->node != 0u && may_ask(iv, s, active(iv)) && now >= s->deadline) {
            ask(iv, s);
        }
    }
}

uint64_t zw_iv_next_deadline(const zw_iv_t *iv)
{
    uint64_t t = UINT64_MAX;
    const size_t busy = active(iv);
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        const zw_iv_node_t *s = &iv->slots[i];
        if (s->node == 0u) {
            continue;
        }
        if ((s->state == S_WAITING || may_ask(iv, s, busy)) && s->deadline < t) {
            t = s->deadline;
        }
    }
    return t;
}

const zw_iv_stats_t *zw_iv_stats(const zw_iv_t *iv)
{
    return &iv->stats;
}

/* ---- What a record says ---------------------------------------------------------------------- */

bool zw_iv_has_cc(const zw_node_record_t *rec, uint8_t cc)
{
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == cc) {
            return rec->ccs[i].supported || rec->ccs[i].secure;
        }
    }
    return false;
}

bool zw_iv_endpoint_has_cc(const zw_node_record_t *rec, uint8_t endpoint, uint8_t cc)
{
    if (endpoint == 0u) {
        return zw_iv_has_cc(rec, cc);
    }
    if (endpoint >= ZW_NODE_ENDPOINTS_MAX) {
        return false;
    }
    const zw_node_endpoint_t *e = &rec->endpoints[endpoint];
    for (size_t i = 0; i < e->cc_count && i < ZW_NODE_ENDPOINT_CC_MAX; i++) {
        if (e->ccs[i] == cc) {
            return true;
        }
    }
    return false;
}

bool zw_iv_sleeping(const zw_node_record_t *rec)
{
    return rec->has_protocol && !rec->is_listening && !rec->is_flirs;
}

bool zw_iv_is_controller(const zw_node_record_t *rec)
{
    return rec->has_protocol && (rec->basic_class == 0x01u || rec->basic_class == 0x02u);
}

zw_iv_inputs_t zw_iv_inputs(const zw_node_record_t *rec, uint8_t endpoint)
{
    if (zw_iv_is_controller(rec)) {
        return ZW_IV_INPUTS_NOT_USABLE;
    }
    if (rec->interview_step < ZW_IV_STEP_DONE) {
        return ZW_IV_INPUTS_NOT_READY;
    }
    if (!rec->has_lifeline || !rec->lifeline_set) {
        return ZW_IV_INPUTS_NO_LIFELINE;
    }
    if (endpoint < ZW_NODE_ENDPOINTS_MAX && rec->endpoints[endpoint].has_push_pull &&
        rec->endpoints[endpoint].push_pull_mode == ZW_NODE_MODE_PULL) {
        return ZW_IV_INPUTS_PULL;
    }
    return ZW_IV_INPUTS_OFFERED;
}

bool zw_iv_alarm_types_missing(const zw_node_record_t *rec)
{
    if (rec->interview_step < ZW_IV_STEP_DONE || zw_iv_is_controller(rec)) {
        return false;
    }
    for (uint8_t e = 0u; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        const zw_node_endpoint_t *x = &rec->endpoints[e];
        if ((e == 0u || x->used) && !x->has_alarm_sensor_mask &&
            zw_iv_endpoint_has_cc(rec, e, ZW_CC_ALARM_SENSOR)) {
            return true;
        }
    }
    return false;
}

void zw_iv_add_cc(zw_node_record_t *rec, uint8_t cc, bool supported, bool secure)
{
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == cc) {
            rec->ccs[i].supported = rec->ccs[i].supported || supported;
            rec->ccs[i].secure = rec->ccs[i].secure || secure;
            return;
        }
    }
    if (rec->cc_count < ZW_NODE_CC_MAX) {
        zw_node_cc_t *e = &rec->ccs[rec->cc_count++];
        memset(e, 0, sizeof *e);
        e->cc = cc;
        e->supported = supported;
        e->secure = secure;
    }
}

bool zw_iv_matches(const zw_iv_node_t *s, const zw_s2_rx_t *rx)
{
    if (rx->len < 2u) {
        return false;
    }
    if (s->wait == W_SUPERVISION) {
        zw_cc_supervision_report_t sr;
        return zw_cc_supervision_report_parse(rx->command, rx->len, &sr) &&
               sr.session_id == s->spv_session;
    }
    const uint8_t ep = rx->multi_channel ? (unsigned)rx->source_endpoint : 0u;
    if (ep != s->cmd_ep || s->cmd_len < 2u) {
        return false;
    }
    if (s->cmd[0] == ZW_S2_CC && s->cmd[1] == ZW_S2_COMMANDS_SUPPORTED_GET) {
        /* the report at the class asked (the probe tries one class at a time) */
        return rx->command[0] == ZW_S2_CC && rx->command[1] == ZW_S2_COMMANDS_SUPPORTED_REPORT &&
               (uint8_t)rx->security == s->cls;
    }
    return zw_cc_answers(s->cmd, s->cmd_len, rx->command, rx->len);
}
