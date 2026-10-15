/*
 * zw_remove.c - removing devices: exclusion, a failed device, Device Reset Locally (network.md
 * §10, serial-api.md 6.17-6.18). See zw_remove.h.
 */
#include "zwave_host/zw_remove.h"

#include <string.h>

/* A removal's requests and pings: 0xFF2 | what | the removal's generation. */
#define TAG_BASE 0xFF200000u
#define TAG_MASK 0xFFF00000u
#define K_START  1u /* RemoveNode(any): its first status report */
#define K_STOP   2u /* RemoveNode(stop), callback 0 */
#define K_PING   3u
#define K_CHECK  4u /* IsFailedNode */
#define K_DELETE 5u /* RemoveFailedNode */

#define REMOVE_FLAGS (ZW_ADD_NODE_FLAG_HIGH_POWER | ZW_ADD_NODE_FLAG_NETWORK_WIDE)

static uint32_t tag_of(const zw_remove_t *rm, unsigned kind)
{
    return TAG_BASE | (uint32_t)kind << 16 | (uint32_t)rm->gen;
}

static bool tag_current(const zw_remove_t *rm, uint32_t tag, unsigned kind)
{
    return tag == tag_of(rm, kind);
}

static void wait_for(zw_remove_t *rm, zw_remove_state_t st, uint64_t until)
{
    rm->state = st;
    rm->deadline = until;
}

static void emit(zw_remove_t *rm, zw_remove_event_kind_t kind, zw_remove_result_t result)
{
    if (rm->port.event != NULL) {
        const zw_remove_event_t ev = {kind, rm->how, rm->node, rm->deadline, result};
        rm->port.event(rm->port.ctx, &ev);
    }
}

static void end(zw_remove_t *rm, zw_remove_result_t result)
{
    rm->deadline = UINT64_MAX;
    rm->state = ZW_REMOVE_IDLE;
    rm->gen = (uint16_t)((unsigned)rm->gen + 1u); /* its requests still out are ignored */
    rm->ping_out = false;
    if (rm->how == ZW_REMOVE_BY_REPLACE) {
        /* a replacement's check: its end is the caller's alone (once), nothing was removed */
        const zw_remove_checked_t done = rm->checked;
        void *ctx = rm->checked_ctx;
        rm->checked = NULL;
        rm->checked_ctx = NULL;
        if (done != NULL) {
            done(ctx, rm->node, result);
        }
        return;
    }
    const zw_remove_event_t ev = {ZW_REMOVE_EV_DONE, rm->how, rm->node, UINT64_MAX, result};
    if (rm->port.event != NULL) {
        rm->port.event(rm->port.ctx, &ev);
    }
}

/* ---- Module commands ------------------------------------------------------------------------- */

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *r);

static bool submit(zw_remove_t *rm, zw_request_t *req, unsigned kind)
{
    req->prio = ZW_PRIO_NODE;
    req->done = request_done;
    req->ctx = rm;
    req->tag = tag_of(rm, kind);
    return rm->port.submit != NULL && rm->port.submit(rm->port.ctx, req) == ZW_SUBMIT_OK;
}

static bool submit_remove_node(zw_remove_t *rm, uint8_t type, unsigned kind)
{
    const zw_api_remove_node_req_t q = {(uint8_t)((unsigned)type | REMOVE_FLAGS), 0u};
    uint8_t buf[4];
    const size_t n = zw_api_remove_node_req_build(buf, sizeof buf, &q);
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_REMOVE_NODE_FROM_NETWORK;
    req.payload = buf;
    req.payload_len = n;
    if (kind == K_START) { /* its first status report ends it; the rest are module frames */
        size_t at = 0u;
        if (n == 0u || !zw_api_callback_id_offset(req.func, buf, n, &at)) {
            return false;
        }
        req.expect = ZW_EXPECT_CALLBACK;
        req.callback_id_at = (uint8_t)at;
        req.callback_timeout_ms = ZW_REMOVE_READY_MS;
    }
    return n > 0u && submit(rm, &req, kind);
}

static void stop_and_end(zw_remove_t *rm, zw_remove_result_t result)
{
    (void)submit_remove_node(rm, ZW_REMOVE_NODE_TYPE_STOP, K_STOP);
    end(rm, result);
}

/* ---- Exclusion (§10 "Remove") ---------------------------------------------------------------- */

static void on_status(zw_remove_t *rm, const zw_api_remove_node_cb_t *cb)
{
    switch (cb->status) {
    case ZW_REMOVE_NODE_STATUS_NODE_FOUND:
        if (rm->state == ZW_REMOVE_LISTENING) {
            wait_for(rm, ZW_REMOVE_FOUND, rm->now + ZW_REMOVE_FOUND_MS);
            emit(rm, ZW_REMOVE_EV_FOUND, ZW_REMOVE_STARTED);
        }
        break;
    case ZW_REMOVE_NODE_STATUS_REMOVING_END_NODE:
    case ZW_REMOVE_NODE_STATUS_REMOVING_CONTROLLER:
        if (rm->state == ZW_REMOVE_LISTENING || rm->state == ZW_REMOVE_FOUND) {
            rm->node = cb->node.node_id; /* 0: a device of another network (it is reset anyway) */
            wait_for(rm, ZW_REMOVE_REMOVING, rm->now + ZW_REMOVE_DONE_MS);
        }
        break;
    case ZW_REMOVE_NODE_STATUS_DONE:
        if (rm->state == ZW_REMOVE_LISTENING || rm->state == ZW_REMOVE_FOUND ||
            rm->state == ZW_REMOVE_REMOVING) {
            if (rm->state != ZW_REMOVE_REMOVING) {
                rm->node = cb->node.node_id; /* a module that skipped RemovingEndNode */
            }
            stop_and_end(rm, ZW_REMOVE_DONE);
        }
        break;
    case ZW_REMOVE_NODE_STATUS_RESERVED_STOPPED: /* serial-api.md 6.17: treated as "stopped" */
    case ZW_REMOVE_NODE_STATUS_FAILED:
        if (rm->state >= ZW_REMOVE_STARTING && rm->state <= ZW_REMOVE_REMOVING) {
            stop_and_end(rm, ZW_REMOVE_FAILED);
        }
        break;
    default:
        break;
    }
}

/* ---- A failed device (§10 "Remove a failed device", "Device reset locally") ------------------ */

static void ping_now(zw_remove_t *rm)
{
    wait_for(rm, ZW_REMOVE_PINGING, UINT64_MAX);
    rm->ping_out = true;
    if (rm->port.ping == NULL || !rm->port.ping(rm->port.ctx, rm->node, tag_of(rm, K_PING))) {
        rm->ping_out = false; /* it could not go: a ping that got no answer */
        rm->pings++;
        rm->deadline = rm->now + ZW_REMOVE_PING_GAP_MS;
    }
}

/* RemoveFailedNode's response: 00 started, any bit set is a refusal (serial-api.md 6.18). */
static bool delete_started(void *ctx, const uint8_t *p, size_t len)
{
    (void)ctx;
    return len > 0u && p[0] == 0x00u;
}

static bool node_request(zw_remove_t *rm, uint8_t func, unsigned kind)
{
    uint8_t buf[4];
    size_t n = zw_api_node_id_build(buf, sizeof buf, rm->nid, rm->node);
    if (n == 0u) {
        return false;
    }
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = func;
    req.expect = ZW_EXPECT_RESPONSE;
    if (kind == K_DELETE) {
        buf[n++] = 0u; /* its callback ID, the transport's */
        req.expect = ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK;
        req.callback_id_at = (uint8_t)(n - 1u);
        req.callback_timeout_ms = ZW_REMOVE_FAILED_MS;
        req.response_ok = delete_started;
    }
    req.payload = buf;
    req.payload_len = n;
    return submit(rm, &req, kind);
}

/* The pings are over, none answered: does the module list it as failed? */
static void check(zw_remove_t *rm)
{
    wait_for(rm, ZW_REMOVE_CHECKING, UINT64_MAX);
    if (!node_request(rm, (uint8_t)ZW_FUNC_IS_FAILED_NODE, K_CHECK)) {
        end(rm, ZW_REMOVE_FAILED);
    }
}

static void begin_failed(zw_remove_t *rm, uint16_t node, zw_remove_kind_t how, zw_nid_width_t nid)
{
    rm->gen = (uint16_t)((unsigned)rm->gen + 1u);
    rm->how = how;
    rm->node = node;
    rm->nid = nid == ZW_NID_16 ? ZW_NID_16 : ZW_NID_8;
    rm->pings = 0u;
    rm->rounds = 0u;
    ping_now(rm);
}

/* ---- Answers --------------------------------------------------------------------------------- */

static void start_done(zw_remove_t *rm, const zw_tx_result_t *r)
{
    zw_api_remove_node_cb_t cb;
    if (rm->state != ZW_REMOVE_STARTING) {
        return;
    }
    if (r->status == ZW_TX_LINK_DOWN) {
        end(rm, ZW_REMOVE_LINK_DOWN);
        return;
    }
    if (r->status == ZW_TX_MODULE_RESTARTED) { /* its Ready will never come (serial-api.md §2) */
        stop_and_end(rm, ZW_REMOVE_RADIO_RESTARTED);
        return;
    }
    if (!(r->status == ZW_TX_OK && r->callback != NULL &&
          zw_api_remove_node_cb_parse(r->callback, r->callback_len, rm->nid, &cb))) {
        stop_and_end(rm, ZW_REMOVE_NOT_LISTENING); /* no Ready in 10 s */
        return;
    }
    rm->cb = r->callback_id;
    if (cb.status == ZW_REMOVE_NODE_STATUS_FAILED) {
        stop_and_end(rm, ZW_REMOVE_FAILED);
        return;
    }
    wait_for(rm, ZW_REMOVE_LISTENING, rm->now + ZW_REMOVE_WINDOW_MS);
    emit(rm, ZW_REMOVE_EV_LISTENING, ZW_REMOVE_STARTED);
    if (cb.status != ZW_REMOVE_NODE_STATUS_READY) {
        on_status(rm, &cb); /* the module skipped Ready: this is the next step's report */
    }
}

static void check_done(zw_remove_t *rm, const zw_tx_result_t *r)
{
    if (rm->state != ZW_REMOVE_CHECKING) {
        return;
    }
    if (r->status == ZW_TX_OK && r->response != NULL && r->response_len > 0u &&
        r->response[0] != 0u) {
        if (rm->how == ZW_REMOVE_BY_REPLACE) {
            end(rm, ZW_REMOVE_LISTED); /* §10: the module will take ReplaceFailedNode now */
            return;
        }
        wait_for(rm, ZW_REMOVE_DELETING, UINT64_MAX); /* listed as failed: RemoveFailedNode */
        if (!node_request(rm, (uint8_t)ZW_FUNC_REMOVE_FAILED_NODE, K_DELETE)) {
            end(rm, ZW_REMOVE_FAILED);
        }
        return;
    }
    if (r->status == ZW_TX_LINK_DOWN) {
        end(rm, ZW_REMOVE_LINK_DOWN);
        return;
    }
    if (r->status != ZW_TX_OK) {
        end(rm, ZW_REMOVE_FAILED); /* no answer from the module: nothing is known, nothing done */
        return;
    }
    if (rm->how == ZW_REMOVE_BY_RESET_LOCALLY && ++rm->rounds < ZW_REMOVE_DRL_ROUNDS) {
        rm->pings = 0u; /* "ping it until the module lists it as failed" */
        ping_now(rm);
        return;
    }
    end(rm, ZW_REMOVE_NOT_FAILED);
}

static void delete_done(zw_remove_t *rm, const zw_tx_result_t *r)
{
    if (rm->state != ZW_REMOVE_DELETING) {
        return;
    }
    if (r->status == ZW_TX_LINK_DOWN) {
        end(rm, ZW_REMOVE_LINK_DOWN);
        return;
    }
    /* serial-api.md 6.18: response 00 started, any bit set a refusal; callback 01 removed, 00
     * the node answered, 02 not removed. Each outcome keeps its own words: only 01 removes it. */
    if (r->status == ZW_TX_NOT_ACCEPTED ||
        (r->response != NULL && r->response_len > 0u && r->response[0] != 0u)) {
        end(rm, ZW_REMOVE_REFUSED);
        return;
    }
    if (r->status == ZW_TX_CALLBACK_TIMEOUT || r->status == ZW_TX_RESPONSE_TIMEOUT) {
        end(rm, ZW_REMOVE_TIMEOUT);
        return;
    }
    if (r->status != ZW_TX_OK || r->callback == NULL || r->callback_len < 2u) {
        end(rm, ZW_REMOVE_FAILED);
        return;
    }
    end(rm, r->callback[1] == 0x01u   ? ZW_REMOVE_DONE
            : r->callback[1] == 0x00u ? ZW_REMOVE_STILL_ANSWERS
            : r->callback[1] == 0x02u ? ZW_REMOVE_REFUSED
                                      : ZW_REMOVE_FAILED);
}

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_remove_t *rm = ctx;
    if (tag_current(rm, tag, K_START)) {
        start_done(rm, r);
    } else if (tag_current(rm, tag, K_CHECK)) {
        check_done(rm, r);
    } else if (tag_current(rm, tag, K_DELETE)) {
        delete_done(rm, r);
    }
}

/* ---- API ------------------------------------------------------------------------------------- */

void zw_remove_init(zw_remove_t *rm, const zw_remove_port_t *port)
{
    memset(rm, 0, sizeof *rm);
    rm->port = *port;
    rm->deadline = UINT64_MAX;
    rm->nid = ZW_NID_8;
}

zw_remove_result_t zw_remove_start(zw_remove_t *rm, uint64_t now, bool allowed,
                                   zw_nid_width_t nid)
{
    rm->now = now;
    if (!allowed) {
        return ZW_REMOVE_NOT_ALLOWED; /* ZW-016 with ZW-005's rule: installer mode, disarmed */
    }
    if (rm->state != ZW_REMOVE_IDLE) {
        return ZW_REMOVE_BUSY;
    }
    rm->gen = (uint16_t)((unsigned)rm->gen + 1u);
    rm->how = ZW_REMOVE_BY_EXCLUSION;
    rm->node = 0u;
    rm->cb = 0u;
    rm->nid = nid == ZW_NID_16 ? ZW_NID_16 : ZW_NID_8;
    wait_for(rm, ZW_REMOVE_STARTING, UINT64_MAX); /* Ready's 10 s are the request's */
    if (!submit_remove_node(rm, ZW_REMOVE_NODE_TYPE_ANY, K_START)) {
        rm->state = ZW_REMOVE_IDLE;
        return ZW_REMOVE_NOT_LISTENING;
    }
    return ZW_REMOVE_STARTED;
}

void zw_remove_keep_listening(zw_remove_t *rm, uint64_t now)
{
    rm->now = now;
    if (rm->state == ZW_REMOVE_LISTENING) {
        rm->deadline = now + ZW_REMOVE_WINDOW_MS;
        emit(rm, ZW_REMOVE_EV_LISTENING, ZW_REMOVE_STARTED);
    }
}

void zw_remove_stop(zw_remove_t *rm, uint64_t now)
{
    rm->now = now;
    if (rm->state == ZW_REMOVE_STARTING || rm->state == ZW_REMOVE_LISTENING) {
        stop_and_end(rm, ZW_REMOVE_STOPPED);
    }
}

zw_remove_result_t zw_remove_failed(zw_remove_t *rm, uint64_t now, bool allowed, uint16_t node,
                                    zw_nid_width_t nid)
{
    rm->now = now;
    if (!allowed) {
        return ZW_REMOVE_NOT_ALLOWED;
    }
    if (rm->state != ZW_REMOVE_IDLE) {
        return ZW_REMOVE_BUSY;
    }
    if (node == 0u) {
        return ZW_REMOVE_UNKNOWN_NODE;
    }
    begin_failed(rm, node, ZW_REMOVE_BY_FAILED, nid);
    return ZW_REMOVE_STARTED;
}

zw_remove_result_t zw_remove_reset_locally(zw_remove_t *rm, uint64_t now, uint16_t node,
                                           zw_nid_width_t nid)
{
    rm->now = now;
    if (rm->state != ZW_REMOVE_IDLE) {
        return ZW_REMOVE_BUSY;
    }
    if (node == 0u) {
        return ZW_REMOVE_UNKNOWN_NODE;
    }
    begin_failed(rm, node, ZW_REMOVE_BY_RESET_LOCALLY, nid);
    return ZW_REMOVE_STARTED;
}

zw_remove_result_t zw_remove_check_failed(zw_remove_t *rm, uint64_t now, uint16_t node,
                                          zw_nid_width_t nid, zw_remove_checked_t done, void *ctx)
{
    rm->now = now;
    if (rm->state != ZW_REMOVE_IDLE) {
        return ZW_REMOVE_BUSY;
    }
    if (node == 0u) {
        return ZW_REMOVE_UNKNOWN_NODE;
    }
    rm->checked = done;
    rm->checked_ctx = ctx;
    begin_failed(rm, node, ZW_REMOVE_BY_REPLACE, nid);
    return ZW_REMOVE_STARTED;
}

void zw_remove_cancel_check(zw_remove_t *rm, uint64_t now)
{
    rm->now = now;
    if (rm->state != ZW_REMOVE_IDLE && rm->how == ZW_REMOVE_BY_REPLACE) {
        rm->checked = NULL; /* nobody hears it: the replacement already ended */
        rm->checked_ctx = NULL;
        end(rm, ZW_REMOVE_STOPPED); /* its ping or IsFailedNode still out is ignored */
    }
}

void zw_remove_module_frame(zw_remove_t *rm, uint64_t now, const zw_frame_t *frame)
{
    rm->now = now;
    zw_api_remove_node_cb_t cb;
    if (frame == NULL || frame->type != ZW_TYPE_REQUEST ||
        frame->func != (uint8_t)ZW_FUNC_REMOVE_NODE_FROM_NETWORK || rm->cb == 0u ||
        rm->state < ZW_REMOVE_LISTENING || rm->state > ZW_REMOVE_REMOVING ||
        !zw_api_remove_node_cb_parse(frame->payload, frame->payload_len, rm->nid, &cb) ||
        cb.callback_id != rm->cb) {
        return;
    }
    on_status(rm, &cb);
}

void zw_remove_ping_done(zw_remove_t *rm, uint64_t now, uint32_t tag, bool delivered)
{
    rm->now = now;
    if (!tag_current(rm, tag, K_PING) || rm->state != ZW_REMOVE_PINGING || !rm->ping_out) {
        return;
    }
    rm->ping_out = false;
    if (delivered) {
        end(rm, ZW_REMOVE_STILL_ANSWERS); /* §10: any answer refuses it */
        return;
    }
    rm->pings++;
    if (rm->pings < ZW_REMOVE_PINGS) {
        rm->deadline = now + ZW_REMOVE_PING_GAP_MS;
        return;
    }
    check(rm);
}

void zw_remove_link_down(zw_remove_t *rm, uint64_t now)
{
    rm->now = now;
    if (rm->state != ZW_REMOVE_IDLE) {
        end(rm, ZW_REMOVE_LINK_DOWN);
    }
}

void zw_remove_radio_restarted(zw_remove_t *rm, uint64_t now)
{
    rm->now = now;
    if (rm->state >= ZW_REMOVE_STARTING && rm->state <= ZW_REMOVE_REMOVING) {
        /* the stop waits behind the restart: a start still queued there is undone by it */
        stop_and_end(rm, ZW_REMOVE_RADIO_RESTARTED);
    }
}

void zw_remove_poll(zw_remove_t *rm, uint64_t now)
{
    rm->now = now;
    if (now < rm->deadline) {
        return;
    }
    rm->deadline = UINT64_MAX;
    switch (rm->state) {
    case ZW_REMOVE_LISTENING:
        stop_and_end(rm, ZW_REMOVE_NO_DEVICE);
        break;
    case ZW_REMOVE_FOUND:
    case ZW_REMOVE_REMOVING:
        stop_and_end(rm, ZW_REMOVE_TIMEOUT);
        break;
    case ZW_REMOVE_PINGING:
        if (rm->pings >= ZW_REMOVE_PINGS) {
            check(rm);
        } else if (!rm->ping_out) {
            ping_now(rm);
        }
        break;
    default:
        break;
    }
}

uint64_t zw_remove_next_deadline(const zw_remove_t *rm)
{
    return rm->state == ZW_REMOVE_IDLE ? UINT64_MAX : rm->deadline;
}

zw_remove_state_t zw_remove_state(const zw_remove_t *rm)
{
    return rm->state;
}

bool zw_remove_is_tag(uint32_t tag)
{
    return (tag & TAG_MASK) == TAG_BASE;
}
