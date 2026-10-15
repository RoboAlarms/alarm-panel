/*
 * zw_transport.c - the Serial API transport and request scheduler (docs/features/36
 * serial-api.md §1 "Sending", §2, §6.8, §6.12, §7; see zw_transport.h).
 */
#include "zwave_host/zw_transport.h"

#include <string.h>

#include "zwave_proto/zw_api.h" /* zw_func_t: the functions with rules of their own (§6.8, §6.12) */

/* §1 "Sending". */
#define ZW_ACK_TIMEOUT_MS       1600u  /* after the frame is written */
#define ZW_RETRY_WAIT_2ND_MS     100u  /* NAK or ACK timeout: before the 2nd transmission */
#define ZW_RETRY_WAIT_3RD_MS    1100u  /* ... and before the 3rd */
#define ZW_CAN_WAIT_MS           100u  /* CAN: before any retransmission */
#define ZW_TRANSMISSIONS           3u  /* in all; then the module is not answering */
#define ZW_RESPONSE_TIMEOUT_MS 10000u
/* §6.12 SendData. */
#define ZW_NOT_QUEUED_WAIT_MS    500u
#define ZW_NOT_QUEUED_MAX          3u  /* "not queued" answers, then the module is not answering */
#define ZW_SEND_DATA_ABORT_MS  20000u  /* after "queued": write SendDataAbort */
#define ZW_SEND_DATA_FAIL_MS   30000u  /* after "queued": give up */
/* §5 step 12, §6.8 SoftReset. */
#define ZW_SOFT_RESET_WAIT_MS   5000u

/* ---- Telling the owner -------------------------------------------------------------------- */

static void notify(zw_transport_t *t, zw_transport_event_t ev, const zw_frame_t *frame)
{
    t->events++;
    if (t->port.event != NULL) {
        t->depth++;
        t->port.event(t->port.ctx, ev, frame);
        t->depth--;
    }
}

static void unclaimed(zw_transport_t *t, const zw_frame_t *f)
{
    t->stats.unclaimed++;
    if (t->port.frame != NULL) {
        t->depth++;
        t->port.frame(t->port.ctx, f);
        t->depth--;
    }
}

/* Ends the command in flight and tells its owner. The active slot keeps its buffers until the
 * next command starts (never inside done(), which may only queue it), so the result's views
 * stay valid through the call. */
static void complete(zw_transport_t *t, zw_tx_outcome_t status, const uint8_t *cb, size_t cb_len)
{
    zw_tx_active_t *a = &t->active;
    const zw_tx_result_t r = {
        .status = status,
        .func = a->cmd.func,
        .callback_id = a->callback_id,
        .transmissions = a->transmissions,
        .response = a->has_response ? a->response : NULL,
        .response_len = a->has_response ? a->response_len : 0u,
        .callback = cb,
        .callback_len = cb_len,
    };
    a->phase = ZW_TX_PHASE_IDLE;
    a->abort_at = 0u;
    if (status == ZW_TX_OK) {
        t->stats.completed++;
    } else {
        t->stats.failed++;
    }
    if (a->cmd.done != NULL) {
        t->depth++;
        a->cmd.done(a->cmd.ctx, a->cmd.tag, &r);
        t->depth--;
    }
}

/* ---- The command in flight ---------------------------------------------------------------- */

/* A transmission without an ACK (NAK, CAN, 1600 ms, or a write that failed): wait and write it
 * again - 100 ms before the 2nd, 1100 ms before the 3rd, 100 ms after a CAN, when the module's
 * own frame is on its way and is handled first - or give up after the 3rd. */
static void transmission_failed(zw_transport_t *t, uint64_t now, bool can)
{
    zw_tx_active_t *a = &t->active;
    a->attempts++;
    if (a->attempts >= ZW_TRANSMISSIONS) {
        complete(t, ZW_TX_NO_ACK, NULL, 0u);
        notify(t, ZW_TRANSPORT_NOT_ANSWERING, NULL);
        return;
    }
    a->phase = ZW_TX_PHASE_SEND;
    a->due = now + (can ? ZW_CAN_WAIT_MS
                        : a->attempts == 1u ? ZW_RETRY_WAIT_2ND_MS : ZW_RETRY_WAIT_3RD_MS);
}

/* Writes the command's frame; the ACK timeout runs from here. */
static void transmit(zw_transport_t *t, uint64_t now)
{
    zw_tx_active_t *a = &t->active;
    uint8_t frame[ZW_FRAME_MAX];
    const size_t n = zw_frame_build(frame, sizeof frame, ZW_TYPE_REQUEST, a->cmd.func,
                                    a->cmd.payload, a->cmd.payload_len);
    a->transmissions++;
    t->stats.transmissions++;
    a->phase = ZW_TX_PHASE_WAIT_ACK;
    a->due = now + ZW_ACK_TIMEOUT_MS;
    bool written = false;
    if (n > 0u) {
        t->depth++;
        written = t->port.write(t->port.ctx, frame, n);
        t->depth--;
    }
    if (!written) {
        t->stats.write_failures++;
        transmission_failed(t, now, false);
    }
}

/* Waiting for the callback: SendData's fixed 20 s abort and 30 s limit (§6.12), or the
 * request's own timeout. */
static void wait_callback(zw_transport_t *t, uint64_t now)
{
    zw_tx_active_t *a = &t->active;
    a->phase = ZW_TX_PHASE_WAIT_CALLBACK;
    if (a->cmd.func == ZW_FUNC_SEND_DATA) {
        a->abort_at = now + ZW_SEND_DATA_ABORT_MS;
        a->due = now + ZW_SEND_DATA_FAIL_MS;
    } else {
        a->due = now + a->cmd.callback_timeout_ms;
    }
}

static uint32_t response_timeout(const zw_tx_active_t *a)
{
    return a->cmd.response_timeout_ms != 0u ? a->cmd.response_timeout_ms : ZW_RESPONSE_TIMEOUT_MS;
}

/* §7: a SendData ACKed whose "queued" response never came. The module took the frame, so its
 * callback may still come (Z-Wave JS's SerialAPICommandMachine keeps waiting for it after a
 * SendData response timeout): wait for it on SendData's own clocks, counted from the ACK -
 * SendDataAbort at 20 s, give up at 30 s, at least 10 s after the abort - and never send the
 * frame again. Only a successful callback ends it OK (callback()); a missing one means the module
 * is not answering (expire()). */
static void send_data_response_lost(zw_transport_t *t, uint64_t now)
{
    zw_tx_active_t *a = &t->active;
    const uint64_t acked_at = a->due - response_timeout(a); /* acked() set due from it */
    const uint64_t abort_at = acked_at + ZW_SEND_DATA_ABORT_MS;
    const uint64_t fail_at = acked_at + ZW_SEND_DATA_FAIL_MS;
    a->response_lost = true;
    a->phase = ZW_TX_PHASE_WAIT_CALLBACK;
    a->abort_at = abort_at > now ? abort_at : now; /* written by the next poll */
    const uint64_t after_abort = a->abort_at + (ZW_SEND_DATA_FAIL_MS - ZW_SEND_DATA_ABORT_MS);
    a->due = fail_at > after_abort ? fail_at : after_abort;
}

/* The frame's ACK (heard, damaged or implied by the answer): on to what the command waits for
 * next. */
static void acked(zw_transport_t *t, uint64_t now)
{
    zw_tx_active_t *a = &t->active;
    a->attempts = 0u;
    if (a->cmd.func == ZW_FUNC_SOFT_RESET) {
        a->phase = ZW_TX_PHASE_WAIT_RESTART;
        a->due = now + ZW_SOFT_RESET_WAIT_MS;
    } else if ((a->cmd.expect & ZW_EXPECT_RESPONSE) != 0u) {
        a->phase = ZW_TX_PHASE_WAIT_RESPONSE;
        a->due = now + response_timeout(a);
    } else if ((a->cmd.expect & ZW_EXPECT_CALLBACK) != 0u) {
        wait_callback(t, now);
    } else {
        complete(t, ZW_TX_OK, NULL, 0u);
    }
}

/* TYPE 01 with the command's FUNC, while it waits for one. */
static void response(zw_transport_t *t, uint64_t now, const zw_frame_t *f)
{
    zw_tx_active_t *a = &t->active;
    const size_t len = f->payload_len <= ZW_PAYLOAD_MAX ? f->payload_len : ZW_PAYLOAD_MAX;
    a->has_response = true;
    a->response_len = len;
    if (len > 0u) {
        memcpy(a->response, f->payload, len);
    }
    if (a->cmd.func == ZW_FUNC_SEND_DATA) {
        if (len == 0u) {
            complete(t, ZW_TX_NOT_ACCEPTED, NULL, 0u);
            return;
        }
        if (a->response[0] == 0x00u) { /* "not queued": the module is busy */
            t->stats.not_queued++;
            a->not_queued++;
            if (a->not_queued >= ZW_NOT_QUEUED_MAX) {
                complete(t, ZW_TX_NOT_QUEUED, NULL, 0u);
                notify(t, ZW_TRANSPORT_NOT_ANSWERING, NULL);
                return;
            }
            a->phase = ZW_TX_PHASE_SEND; /* the same frame, callback ID and all */
            a->due = now + ZW_NOT_QUEUED_WAIT_MS;
            a->attempts = 0u;
            return;
        }
    } else if (a->cmd.response_ok != NULL) {
        t->depth++;
        const bool ok = a->cmd.response_ok(a->cmd.ctx, a->response, a->response_len);
        t->depth--;
        if (!ok) {
            complete(t, ZW_TX_NOT_ACCEPTED, NULL, 0u);
            return;
        }
    }
    if ((a->cmd.expect & ZW_EXPECT_CALLBACK) != 0u) {
        wait_callback(t, now);
    } else {
        complete(t, ZW_TX_OK, NULL, 0u);
    }
}

/* TYPE 00 with the command's FUNC and callback ID, while it waits for one. */
static void callback(zw_transport_t *t, uint64_t now, const zw_frame_t *f)
{
    zw_tx_active_t *a = &t->active;
    zw_callback_kind_t kind = ZW_CALLBACK_FINAL;
    if (a->cmd.callback_kind != NULL) {
        t->depth++;
        kind = a->cmd.callback_kind(a->cmd.ctx, f->payload, f->payload_len);
        t->depth--;
    }
    if (kind == ZW_CALLBACK_INTERMEDIATE) {
        t->stats.intermediate++;
        if (a->cmd.func != ZW_FUNC_SEND_DATA) { /* the diagram's self-transition: wait anew */
            a->due = now + a->cmd.callback_timeout_ms;
        }
        return;
    }
    if (a->response_lost) { /* §7: its "queued" response lost - only a delivered frame is OK */
        const bool delivered = f->payload_len >= 2u && f->payload[1] == (uint8_t)ZW_TX_STATUS_OK;
        complete(t, delivered ? ZW_TX_OK : ZW_TX_RESPONSE_TIMEOUT, f->payload, f->payload_len);
        return;
    }
    complete(t, ZW_TX_OK, f->payload, f->payload_len);
}

/* A SerialAPIStarted (§2, §6.8). */
static void module_started(zw_transport_t *t, const zw_frame_t *f)
{
    zw_tx_active_t *a = &t->active;
    /* After our SoftReset - acknowledged, or its ACK lost or damaged on the way - it is the
     * answer: the module restarted as asked. */
    if (a->phase != ZW_TX_PHASE_IDLE && a->cmd.func == ZW_FUNC_SOFT_RESET) {
        complete(t, ZW_TX_OK, f->payload, f->payload_len);
        return;
    }
    t->stats.restarts++;
    t->abort_ack_until = 0u; /* nothing sent to the old module is answered any more */
    if (a->phase != ZW_TX_PHASE_IDLE) {
        complete(t, ZW_TX_MODULE_RESTARTED, NULL, 0u);
    }
    notify(t, ZW_TRANSPORT_MODULE_RESTARTED, f);
}

/* Everything due at `now` that needs no writing: the timeouts. Retransmissions and
 * SendDataAbort wait for zw_transport_poll(), the only writer. */
static void expire(zw_transport_t *t, uint64_t now)
{
    zw_tx_active_t *a = &t->active;
    if (t->abort_ack_until != 0u && now >= t->abort_ack_until) {
        t->abort_ack_until = 0u;
    }
    if (a->phase == ZW_TX_PHASE_IDLE || a->phase == ZW_TX_PHASE_SEND || now < a->due) {
        return;
    }
    switch (a->phase) {
    case ZW_TX_PHASE_WAIT_ACK:
        t->stats.ack_timeouts++;
        transmission_failed(t, now, false);
        break;
    case ZW_TX_PHASE_WAIT_RESPONSE: /* §7: ACKed, but no response */
        if (a->cmd.func == ZW_FUNC_SEND_DATA && a->callback_id != 0u) {
            send_data_response_lost(t, now); /* its callback may still come */
            break;
        }
        complete(t, ZW_TX_RESPONSE_TIMEOUT, NULL, 0u);
        notify(t, ZW_TRANSPORT_NOT_ANSWERING, NULL);
        break;
    case ZW_TX_PHASE_WAIT_CALLBACK: {
        const bool send_data = a->cmd.func == ZW_FUNC_SEND_DATA;
        /* §7: neither the response nor the callback - the module is not answering */
        complete(t, a->response_lost ? ZW_TX_RESPONSE_TIMEOUT : ZW_TX_CALLBACK_TIMEOUT, NULL, 0u);
        if (send_data) { /* §7: the SendData callback missing after 30 s */
            notify(t, ZW_TRANSPORT_NOT_ANSWERING, NULL);
        }
        break;
    }
    case ZW_TX_PHASE_WAIT_RESTART:
        complete(t, ZW_TX_NO_RESTART, NULL, 0u);
        break;
    default:
        break;
    }
}

/* §6.12: 20 s after "queued" without the callback, ask the module to give up. SendDataAbort has
 * no response and no callback of its own; the SendData callback it provokes still ends the
 * command. Its ACK (or NAK or CAN - it is not retried) is taken as the abort's, and no new
 * command goes out until that answer has come or 1600 ms have passed, so no later command can
 * mistake it for its own. */
static void write_abort(zw_transport_t *t, uint64_t now)
{
    uint8_t frame[5];
    const size_t n = zw_frame_build(frame, sizeof frame, ZW_TYPE_REQUEST, ZW_FUNC_SEND_DATA_ABORT,
                                    NULL, 0u);
    t->active.abort_at = 0u;
    t->stats.aborts++;
    t->depth++;
    const bool written = n > 0u && t->port.write(t->port.ctx, frame, n);
    t->depth--;
    if (written) {
        t->abort_ack_until = now + ZW_ACK_TIMEOUT_MS;
    } else {
        t->stats.write_failures++;
    }
}

/* A class's place in the gate's order: MODULE, REPLY, NODE, INTERVIEW, BACKGROUND (what it
 * needs to work, not how urgent it is). */
static unsigned gate_rank(unsigned prio)
{
    return prio == (unsigned)ZW_PRIO_MODULE  ? 0u
           : prio == (unsigned)ZW_PRIO_REPLY ? 1u
                                             : prio;
}

static bool admitted(const zw_transport_t *t, unsigned prio)
{
    return gate_rank(prio) <= gate_rank((unsigned)t->gate);
}

/* The queued request to send next: the most urgent the gate lets out, first come within its
 * class; with `failing`, the next one zw_transport_fail_queued() picked instead. */
static zw_tx_slot_t *next_slot(zw_transport_t *t, bool failing)
{
    zw_tx_slot_t *best = NULL;
    for (size_t i = 0; i < ZW_TRANSPORT_QUEUE_LEN; i++) {
        zw_tx_slot_t *s = &t->queue[i];
        if (!s->used || s->failing != failing || (!failing && !admitted(t, s->cmd.prio))) {
            continue;
        }
        if (best == NULL || s->cmd.prio < best->cmd.prio ||
            (s->cmd.prio == best->cmd.prio && s->seq < best->seq)) {
            best = s;
        }
    }
    return best;
}

/* The next queued request becomes the command in flight. */
static void start_next(zw_transport_t *t, uint64_t now)
{
    zw_tx_slot_t *best = next_slot(t, false);
    if (best == NULL) {
        return;
    }
    zw_tx_active_t *a = &t->active;
    a->cmd = best->cmd;
    best->used = false;
    a->transmissions = 0u;
    a->attempts = 0u;
    a->not_queued = 0u;
    a->damaged_ack_used = false;
    a->response_lost = false;
    a->has_response = false;
    a->response_len = 0u;
    a->abort_at = 0u;
    a->callback_id = 0u;
    if ((a->cmd.expect & ZW_EXPECT_CALLBACK) != 0u && a->cmd.func != ZW_FUNC_SOFT_RESET) {
        /* 1..255, wrapping, never 0 (0 asks the module for no callback). Every transmission of
         * this command, "not queued" resends too, carries the same ID. */
        a->callback_id = t->next_callback_id;
        t->next_callback_id =
            (uint8_t)(t->next_callback_id == 255u ? 1u : (unsigned)t->next_callback_id + 1u);
        a->cmd.payload[a->cmd.callback_id_at] = a->callback_id;
    }
    t->stats.commands++;
    transmit(t, now);
}

/* ---- API ---------------------------------------------------------------------------------- */

void zw_transport_init(zw_transport_t *t, const zw_transport_port_t *port)
{
    memset(t, 0, sizeof *t);
    t->port = *port;
    t->next_callback_id = 1u;
    t->gate = ZW_PRIO_BACKGROUND;
}

zw_submit_t zw_transport_submit(zw_transport_t *t, const zw_request_t *req)
{
    if (req == NULL) {
        t->stats.refused++;
        return ZW_SUBMIT_INVALID;
    }
    const bool with_callback =
        (req->expect & ZW_EXPECT_CALLBACK) != 0u && req->func != ZW_FUNC_SOFT_RESET;
    const bool valid =
        req->payload_len <= ZW_PAYLOAD_MAX && (req->payload != NULL || req->payload_len == 0u) &&
        ((unsigned)req->expect & ~(ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK)) == 0u &&
        (unsigned)req->prio < (unsigned)ZW_PRIO_COUNT &&
        (!with_callback ||
         ((size_t)req->callback_id_at < req->payload_len &&
          (req->callback_timeout_ms > 0u || req->func == ZW_FUNC_SEND_DATA)));
    if (!valid) {
        t->stats.refused++;
        return ZW_SUBMIT_INVALID;
    }
    zw_tx_slot_t *slot = NULL;
    for (size_t i = 0; i < ZW_TRANSPORT_QUEUE_LEN && slot == NULL; i++) {
        if (!t->queue[i].used) {
            slot = &t->queue[i];
        }
    }
    if (slot == NULL) {
        t->stats.refused++;
        return ZW_SUBMIT_FULL;
    }
    zw_tx_cmd_t *c = &slot->cmd;
    c->func = req->func;
    c->expect = req->expect;
    c->callback_id_at = req->callback_id_at;
    c->prio = (uint8_t)req->prio;
    c->response_timeout_ms = req->response_timeout_ms;
    c->callback_timeout_ms = req->callback_timeout_ms;
    c->response_ok = req->response_ok;
    c->callback_kind = req->callback_kind;
    c->done = req->done;
    c->ctx = req->ctx;
    c->tag = req->tag;
    c->payload_len = req->payload_len;
    if (req->payload_len > 0u) {
        memcpy(c->payload, req->payload, req->payload_len);
    }
    slot->seq = t->next_seq++;
    slot->used = true;
    t->stats.submitted++;
    return ZW_SUBMIT_OK;
}

size_t zw_transport_cancel(zw_transport_t *t, uint32_t tag)
{
    size_t n = 0u;
    for (size_t i = 0; i < ZW_TRANSPORT_QUEUE_LEN; i++) {
        if (t->queue[i].used && !t->queue[i].failing && t->queue[i].cmd.tag == tag) {
            t->queue[i].used = false;
            n++;
        }
    }
    t->stats.cancelled += (uint32_t)n;
    return n;
}

void zw_transport_halt(zw_transport_t *t, zw_tx_outcome_t status)
{
    if (t->depth > 0u) {
        t->stats.reentries++;
        return;
    }
    t->abort_ack_until = 0u;
    if (t->active.phase != ZW_TX_PHASE_IDLE) {
        complete(t, status, NULL, 0u);
    }
}

void zw_transport_set_gate(zw_transport_t *t, zw_prio_t max_prio)
{
    t->gate = (unsigned)max_prio < (unsigned)ZW_PRIO_COUNT ? max_prio : ZW_PRIO_BACKGROUND;
}

size_t zw_transport_fail_queued(zw_transport_t *t, unsigned classes, zw_tx_outcome_t status)
{
    /* Pick them first: a done() below may submit, and what it submits must not be ended. */
    for (size_t i = 0; i < ZW_TRANSPORT_QUEUE_LEN; i++) {
        zw_tx_slot_t *s = &t->queue[i];
        if (s->used && (classes & ZW_PRIO_BIT(s->cmd.prio)) != 0u) {
            s->failing = true;
        }
    }
    size_t n = 0u;
    zw_tx_slot_t *s;
    while ((s = next_slot(t, true)) != NULL) {
        /* The slot is free before done() runs, so done() can use it again. */
        void (*done)(void *, uint32_t, const zw_tx_result_t *) = s->cmd.done;
        void *ctx = s->cmd.ctx;
        const uint32_t tag = s->cmd.tag;
        const zw_tx_result_t r = {.status = status, .func = s->cmd.func};
        s->used = false;
        s->failing = false;
        n++;
        t->stats.dropped++;
        if (done != NULL) {
            t->depth++;
            done(ctx, tag, &r);
            t->depth--;
        }
    }
    return n;
}

void zw_transport_on_control(zw_transport_t *t, uint64_t now_ms, uint8_t byte)
{
    if (t->depth > 0u) {
        t->stats.reentries++;
        return;
    }
    if (byte != ZW_ACK && byte != ZW_NAK && byte != ZW_CAN) {
        return;
    }
    expire(t, now_ms);
    if (t->active.phase == ZW_TX_PHASE_WAIT_ACK) {
        if (byte == ZW_ACK) {
            t->stats.acks++;
            acked(t, now_ms);
        } else if (byte == ZW_NAK) {
            t->stats.naks++;
            transmission_failed(t, now_ms, false);
        } else {
            t->stats.cans++;
            transmission_failed(t, now_ms, true);
        }
    } else if (t->abort_ack_until != 0u) {
        t->abort_ack_until = 0u; /* SendDataAbort's answer */
    } else {
        t->stats.stray++;
    }
}

void zw_transport_on_frame(zw_transport_t *t, uint64_t now_ms, const zw_frame_t *frame)
{
    if (t->depth > 0u) {
        t->stats.reentries++;
        return;
    }
    if (frame == NULL) {
        return;
    }
    expire(t, now_ms);
    const zw_tx_active_t *a = &t->active;
    const bool ours = a->phase != ZW_TX_PHASE_IDLE && frame->func == a->cmd.func;
    const bool awaiting_ack = a->phase == ZW_TX_PHASE_WAIT_ACK;
    /* The response, or - when the ACK was lost - one that comes while the ACK is awaited. */
    const bool is_response =
        ours && frame->type == ZW_TYPE_RESPONSE &&
        (a->phase == ZW_TX_PHASE_WAIT_RESPONSE ||
         (awaiting_ack && (a->cmd.expect & ZW_EXPECT_RESPONSE) != 0u));
    /* The callback: its FUNC and ID; while the ACK is awaited only for a command whose sole
     * answer is the callback. */
    const bool is_callback =
        ours && frame->type == ZW_TYPE_REQUEST && a->callback_id != 0u &&
        frame->payload_len > 0u && frame->payload[0] == a->callback_id &&
        (a->phase == ZW_TX_PHASE_WAIT_CALLBACK ||
         (awaiting_ack && a->cmd.expect == ZW_EXPECT_CALLBACK));
    if (frame->type == ZW_TYPE_REQUEST && frame->func == ZW_FUNC_SERIAL_API_STARTED) {
        module_started(t, frame);
    } else if (is_response || is_callback) {
        if (awaiting_ack) { /* the module took the frame: never send it again */
            t->stats.lost_acks++;
            acked(t, now_ms);
        }
        if (is_response) {
            response(t, now_ms, frame);
        } else {
            callback(t, now_ms, frame);
        }
    } else {
        unclaimed(t, frame);
    }
}

void zw_transport_on_text(zw_transport_t *t, uint64_t now_ms, const uint8_t *bytes, size_t len)
{
    if (t->depth > 0u) {
        t->stats.reentries++;
        return;
    }
    if (bytes == NULL || len == 0u) {
        return;
    }
    expire(t, now_ms);
    zw_tx_active_t *a = &t->active;
    /* §6.8: some firmware damages a SoftReset's ACK in its high nibble. Only the bug's own shape
     * counts, once: a lone byte, low nibble 6, while that SoftReset's ACK is awaited - the module
     * sends nothing else until its SerialAPIStarted - never a byte inside text (the bootloader's
     * menu holds 'v', '6', 'F'...). */
    if (a->phase == ZW_TX_PHASE_WAIT_ACK && a->cmd.func == ZW_FUNC_SOFT_RESET &&
        !a->damaged_ack_used && len == 1u && ((unsigned)bytes[0] & 0x0Fu) == 0x06u) {
        a->damaged_ack_used = true;
        t->stats.acks++;
        acked(t, now_ms);
    }
    if (t->port.text != NULL) {
        t->depth++;
        t->port.text(t->port.ctx, bytes, len);
        t->depth--;
    }
}

void zw_transport_poll(zw_transport_t *t, uint64_t now_ms)
{
    if (t->depth > 0u) {
        t->stats.reentries++;
        return;
    }
    const uint32_t events = t->events;
    expire(t, now_ms);
    zw_tx_active_t *a = &t->active;
    if (a->phase == ZW_TX_PHASE_WAIT_CALLBACK && a->abort_at != 0u && now_ms >= a->abort_at) {
        write_abort(t, now_ms);
    }
    if (a->phase == ZW_TX_PHASE_SEND && now_ms >= a->due) {
        transmit(t, now_ms);
    }
    /* After an event (the module not answering) the owner decides before anything else goes:
     * a nonce reply queued behind the failed command must not reach a dead module first. */
    if (a->phase == ZW_TX_PHASE_IDLE && t->abort_ack_until == 0u && t->events == events) {
        start_next(t, now_ms);
    }
}

uint64_t zw_transport_next_deadline(const zw_transport_t *t)
{
    const zw_tx_active_t *a = &t->active;
    switch (a->phase) {
    case ZW_TX_PHASE_IDLE: {
        bool ready = false; /* a queued request the gate lets out */
        for (size_t i = 0; i < ZW_TRANSPORT_QUEUE_LEN && !ready; i++) {
            const zw_tx_slot_t *s = &t->queue[i];
            ready = s->used && !s->failing && admitted(t, s->cmd.prio);
        }
        return ready ? t->abort_ack_until /* 0: at once */ : UINT64_MAX;
    }
    case ZW_TX_PHASE_WAIT_CALLBACK:
        return a->abort_at != 0u && a->abort_at < a->due ? a->abort_at : a->due;
    default:
        return a->due;
    }
}

bool zw_transport_busy(const zw_transport_t *t)
{
    return t->active.phase != ZW_TX_PHASE_IDLE;
}

size_t zw_transport_queued(const zw_transport_t *t)
{
    size_t n = 0u;
    for (size_t i = 0; i < ZW_TRANSPORT_QUEUE_LEN; i++) {
        if (t->queue[i].used) {
            n++;
        }
    }
    return n;
}
