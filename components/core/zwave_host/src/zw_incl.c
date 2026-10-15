/*
 * zw_incl.c - adding a device: network.md §4 and the including side of security.md §5. See
 * zw_incl.h.
 */
#include "zwave_host/zw_incl.h"

#include <string.h>

#include "zwave_proto/zw_api.h"

/* The inclusion's own requests: 'IC', what it is, the inclusion's generation. */
#define TAG_BASE   0x49430000u
#define TAG_MASK   0xFFFF0000u
#define K_START    1u /* AddNode(any): its first status report */
#define K_FINISH   2u /* AddNode(stop) after ProtocolDone: Done */
#define K_STOP     3u /* AddNode(stop) without a callback */
#define K_ROUTE    4u /* AssignSUCReturnRoute */
#define K_REPLACE  5u /* ReplaceFailedNode: its first report */

/* serial-api.md §6.18: ReplaceFailedNode's response (00 started, else why not) and reports. */
#define REPLACE_NOT_IN_FAILED_LIST 0x08u
#define REPLACE_BUSY               0x10u
#define REPLACE_NODE_OK            0x00u
#define REPLACE_READY              0x03u
#define REPLACE_DONE               0x04u

#define ADD_FLAGS (ZW_ADD_NODE_FLAG_HIGH_POWER | ZW_ADD_NODE_FLAG_NETWORK_WIDE)

/* ---- Helpers ------------------------------------------------------------------------------- */

static zw_nid_width_t nid(const zw_incl_t *in)
{
    return in->opt.nid_width == ZW_NID_16 ? ZW_NID_16 : ZW_NID_8;
}

static void wait_for(zw_incl_t *in, zw_incl_state_t st, uint64_t until)
{
    in->state = st;
    in->deadline = until;
}

static uint64_t after(const zw_incl_t *in, uint32_t ms)
{
    return in->now + ms;
}

static void emit(zw_incl_t *in, const zw_incl_event_t *ev)
{
    if (in->port.event != NULL) {
        in->port.event(in->port.ctx, ev);
    }
}

static zw_sec_class_t class_of(uint8_t key)
{
    switch (key) {
    case ZW_S2_KEY_UNAUTH: return ZW_SEC_S2_UNAUTH;
    case ZW_S2_KEY_AUTH: return ZW_SEC_S2_AUTH;
    case ZW_S2_KEY_ACCESS: return ZW_SEC_S2_ACCESS;
    default: return ZW_SEC_NONE;
    }
}

static zw_incl_security_t security_of(uint8_t keys)
{
    if ((keys & ZW_S2_KEY_ACCESS) != 0u) {
        return ZW_INCL_SEC_S2_ACCESS;
    }
    if ((keys & ZW_S2_KEY_AUTH) != 0u) {
        return ZW_INCL_SEC_S2_AUTH;
    }
    return (keys & ZW_S2_KEY_UNAUTH) != 0u ? ZW_INCL_SEC_S2_UNAUTH : ZW_INCL_SEC_NONE;
}

static bool is_kex_state(zw_incl_state_t st)
{
    return st >= ZW_INCL_KEX_REPORT && st <= ZW_INCL_TRANSFER_END;
}

/* Whether the node information lists a CC (as supported, before the "controlled" mark). */
static bool nif_has(const zw_incl_t *in, uint8_t want)
{
    zw_cc_iter_t it;
    uint16_t cc = 0u;
    bool controlled = false;
    zw_cc_iter_init(&it, in->nif, in->nif_len);
    while (zw_cc_iter_next(&it, &cc, &controlled)) {
        if (!controlled && cc == want) {
            return true;
        }
    }
    return false;
}

static void wipe_secrets(zw_incl_t *in)
{
    zw_wipe(in->priv, sizeof in->priv);
    zw_wipe(in->pub, sizeof in->pub);
}

/* ---- The end -------------------------------------------------------------------------------- */

/* The inclusion ends: the temporary key deleted, a failed exchange leaves no class (§5 "On any
 * failure"), then the owner hears the result. */
static void end(zw_incl_t *in, zw_incl_result_t result, zw_incl_security_t sec,
                zw_incl_s2_failure_t failure, uint8_t detail)
{
    const uint16_t node = in->node;
    if (in->s2->temp_node != 0u && in->s2->temp_node == node) {
        zw_s2_clear_temp_key(in->s2);
    }
    in->temp_installed = false;
    if (result == ZW_INCL_ADDED && sec != ZW_INCL_SEC_S2_UNAUTH && sec != ZW_INCL_SEC_S2_AUTH &&
        sec != ZW_INCL_SEC_S2_ACCESS) {
        (void)zw_s2_set_granted(in->s2, node, 0u);
    }
    wipe_secrets(in);
    zw_incl_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_INCL_EV_DONE;
    ev.node = node;
    ev.result = result;
    ev.security = sec;
    ev.failure = failure;
    ev.failure_detail = detail;
    ev.route_failed = in->route_failed;
    ev.nif = in->nif_len > 0u ? in->nif : NULL;
    ev.nif_len = in->nif_len;
    if (sec == ZW_INCL_SEC_S2_UNAUTH || sec == ZW_INCL_SEC_S2_AUTH ||
        sec == ZW_INCL_SEC_S2_ACCESS) {
        ev.granted = in->delivered;
        memcpy(ev.dsk_bytes, in->dev_pub, sizeof ev.dsk_bytes);
        (void)zw_dsk_to_text(ev.dsk_bytes, ev.dsk, sizeof ev.dsk);
    }
    ev.smartstart = in->smart;
    ev.replaced = in->replace;
    in->replace = false;
    in->checking = false;
    if (in->smart) {
        memcpy(ev.dsk_bytes, in->ss.dsk, sizeof ev.dsk_bytes); /* which entry, whatever happened */
        (void)zw_dsk_to_text(ev.dsk_bytes, ev.dsk, sizeof ev.dsk);
    }
    in->smart = false;
    zw_wipe(in->dev_pub, sizeof in->dev_pub);
    in->state = ZW_INCL_IDLE;
    in->deadline = UINT64_MAX;
    in->gen = (uint16_t)((unsigned)in->gen + 1u); /* its requests still in flight are ignored */
    emit(in, &ev);
}

/* ---- Module commands ------------------------------------------------------------------------ */

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *r);

static bool submit_add_node(zw_incl_t *in, uint8_t type, unsigned kind)
{
    zw_api_add_node_req_t a = {(uint8_t)((unsigned)type | ADD_FLAGS), 0u, false, {0}, {0}};
    if (type == ZW_ADD_NODE_TYPE_SMARTSTART_DSK) {
        a.has_smartstart_dsk = true;
        zw_dsk_home_ids(in->ss.dsk, a.nwi_home_id, a.auth_home_id);
        if (in->ss.long_range) {
            a.mode = (uint8_t)(a.mode | ZW_ADD_NODE_FLAG_LONG_RANGE);
        }
    }
    uint8_t buf[16];
    const size_t n = zw_api_add_node_req_build(buf, sizeof buf, &a);
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK;
    req.payload = buf;
    req.payload_len = n;
    req.prio = ZW_PRIO_NODE;
    req.done = request_done;
    req.ctx = in;
    req.tag = TAG_BASE | (uint32_t)kind << 12 | (in->gen & 0xFFFu);
    if (kind != K_STOP) { /* start and the stop after ProtocolDone: a status report ends them */
        size_t at = 0u;
        if (n == 0u || !zw_api_callback_id_offset(req.func, buf, n, &at)) {
            return false;
        }
        req.expect = ZW_EXPECT_CALLBACK;
        req.callback_id_at = (uint8_t)at;
        req.callback_timeout_ms = kind == K_START ? ZW_INCL_READY_MS : ZW_INCL_DONE_MS;
    }
    return n > 0u && in->port.submit(in->port.ctx, &req) == ZW_SUBMIT_OK;
}

/* Ends a window or a protocol inclusion that goes nowhere: AddNode(stop), then the result. */
static void stop_and_end(zw_incl_t *in, zw_incl_result_t result)
{
    (void)submit_add_node(in, ZW_ADD_NODE_TYPE_STOP, K_STOP);
    end(in, result, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
}

/* ---- Security 2 commands -------------------------------------------------------------------- */

static bool s2_send(zw_incl_t *in, zw_sec_class_t cls, const uint8_t *cmd, size_t len)
{
    return len > 0u && in->port.s2_send(in->port.ctx, in->node, cls, cmd, len) == ZW_S2_OK;
}

/* The key exchange fails: a KEX Fail of `type` (0: none), then the device stays without
 * security. */
static void kex_fail(zw_incl_t *in, uint8_t type, zw_incl_security_t sec,
                     zw_incl_s2_failure_t failure, uint8_t detail)
{
    if (type != 0u) {
        uint8_t f[3];
        (void)s2_send(in, ZW_SEC_NONE, f, zw_s2_kex_fail_build(f, sizeof f, type));
    }
    end(in, ZW_INCL_ADDED, sec, failure, detail);
}

static void refuse(zw_incl_t *in, uint8_t type, bool downgrade)
{
    kex_fail(in, type, downgrade ? ZW_INCL_SEC_DOWNGRADED : ZW_INCL_SEC_FAILED,
             downgrade ? ZW_INCL_S2_TAMPERED : ZW_INCL_S2_REFUSED, type);
}

/* §5 step 1: KEX Get, or no key exchange at all. Only a node information that lists its
 * command classes without Security 2 says "no S2"; an empty or missing one says nothing (every
 * Z-Wave Plus device lists at least 5E), so KEX Get goes anyway: a device with S2 answers it, one
 * without never does, and TA1 then ends it as "not confirmed" (ZW_INCL_S2_NO_INFO) - never as an
 * ordinary device without security (security.md §5, "An empty node information"). */
static void begin_security(zw_incl_t *in)
{
    /* SmartStart: S2 is forced - the code said it has it (network.md §4.2) */
    in->probing = in->nif_len == 0u && !in->smart;
    if (!in->probing && !in->smart && !nif_has(in, ZW_S2_CC)) {
        /* no S2 (S0 is phase 3: a device that lists only 98 is added without security too) */
        end(in, ZW_INCL_ADDED, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
        return;
    }
    uint8_t f[2];
    if (!s2_send(in, ZW_SEC_NONE, f, zw_s2_kex_get_build(f, sizeof f))) {
        end(in, ZW_INCL_ADDED, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    wait_for(in, ZW_INCL_KEX_REPORT, after(in, ZW_INCL_TA_MS));
}

static void joined(zw_incl_t *in);

/* network.md §4 steps 4-6: Done named the node - AddNode(stop) once more without a callback,
 * the refusals, the node known to the security manager, the SUC return route. */
static void added(zw_incl_t *in, uint16_t node)
{
    (void)submit_add_node(in, ZW_ADD_NODE_TYPE_STOP, K_STOP);
    if (node != 0u) {
        in->node = node;
    }
    if (in->node == 0u || in->node == 0xFFu || in->node == 0xFFFu) {
        end(in, ZW_INCL_BAD_NODE_ID, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
        return;
    }
    if (in->port.known != NULL && in->port.known(in->port.ctx, in->node)) {
        end(in, ZW_INCL_ALREADY_ADDED, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
        return;
    }
    joined(in);
}

/* §10: the replacement took the failed device's node ID - its old keys and counters go, the
 * new device starts with none (its node information isn't known: KEX Get asks). */
static void replaced(zw_incl_t *in)
{
    zw_s2_remove_node(in->s2, in->node);
    in->nif_len = 0u;
    joined(in);
}

/* A report of ReplaceFailedNode - its first (the request's end) or a later module frame. */
static void replace_status(zw_incl_t *in, uint8_t status)
{
    switch (status) {
    case REPLACE_READY:
        if (in->state == ZW_INCL_STARTING) { /* the window, as for "Add a device" */
            wait_for(in, ZW_INCL_LISTENING, after(in, ZW_INCL_WINDOW_MS));
            const zw_incl_event_t ev = {.kind = ZW_INCL_EV_LISTENING, .deadline = in->deadline,
                                        .node = in->node, .replaced = true};
            emit(in, &ev);
        }
        break;
    case REPLACE_DONE: replaced(in); break;
    case REPLACE_NODE_OK: /* the old device answered the radio */
        end(in, ZW_INCL_STILL_ANSWERS, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
        break;
    default: stop_and_end(in, ZW_INCL_FAILED); break;
    }
}

/* The node is in the network: known to the security manager, remembered by the owner, its SUC
 * return route, then its key exchange. */
static void joined(zw_incl_t *in)
{
    /* known from now on, with no class: its frames are taken, its key exchange can start */
    if (!zw_s2_add_node(in->s2, in->node, 0u, in->nif, in->nif_len)) {
        end(in, ZW_INCL_ADDED, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    {
        /* §4 step 5: the owner remembers it now (node cache, generation) */
        zw_incl_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = ZW_INCL_EV_JOINED;
        ev.node = in->node;
        ev.nif = in->nif_len > 0u ? in->nif : NULL;
        ev.nif_len = in->nif_len;
        ev.replaced = in->replace;
        emit(in, &ev);
    }
    if (in->node >= 256u) { /* Long Range does not route */
        begin_security(in);
        return;
    }
    const zw_api_assign_suc_return_route_req_t rr = {in->node, 0u};
    uint8_t buf[6];
    const size_t n = zw_api_assign_suc_return_route_req_build(buf, sizeof buf, nid(in), &rr);
    zw_request_t req;
    size_t at = 0u;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE;
    req.payload = buf;
    req.payload_len = n;
    req.expect = ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK;
    req.callback_timeout_ms = ZW_INCL_ROUTE_MS;
    req.prio = ZW_PRIO_NODE;
    req.done = request_done;
    req.ctx = in;
    req.tag = TAG_BASE | (uint32_t)K_ROUTE << 12 | (in->gen & 0xFFFu);
    wait_for(in, ZW_INCL_ROUTE, UINT64_MAX); /* the request's own timeouts end it */
    if (n == 0u || !zw_api_callback_id_offset(req.func, buf, n, &at)) {
        in->route_failed = true;
        begin_security(in);
        return;
    }
    req.callback_id_at = (uint8_t)at;
    if (in->port.submit(in->port.ctx, &req) != ZW_SUBMIT_OK) {
        in->route_failed = true;
        begin_security(in);
    }
}

/* An AddNode status report, as the first answer of a request or on its own (network.md §4). */
static void on_status(zw_incl_t *in, const zw_api_add_node_cb_t *cb)
{
    switch (cb->status) {
    case ZW_ADD_NODE_STATUS_NODE_FOUND:
        if (in->state == ZW_INCL_LISTENING) {
            wait_for(in, ZW_INCL_FOUND, after(in, ZW_INCL_FOUND_MS));
            const zw_incl_event_t ev = {.kind = ZW_INCL_EV_FOUND};
            emit(in, &ev);
        }
        break;
    case ZW_ADD_NODE_STATUS_ADDING_END_NODE:
        if (in->state == ZW_INCL_LISTENING || in->state == ZW_INCL_FOUND) {
            in->node = cb->node.node_id;
            in->nif_len = 0u;
            /* An empty list is NULL with length 0 (zw_api): nothing to copy, and memcpy must not
             * see that NULL; a length with no list is no node information. */
            if (cb->has_node_info && cb->node.cc_list_len > 0u && cb->node.cc_list != NULL &&
                cb->node.cc_list_len <= sizeof in->nif) {
                memcpy(in->nif, cb->node.cc_list, cb->node.cc_list_len);
                in->nif_len = cb->node.cc_list_len;
            }
            const uint32_t t = ZW_INCL_PROTOCOL_BASE_MS +
                               ZW_INCL_PROTOCOL_LISTENING_MS * (unsigned)in->opt.listening_nodes +
                               ZW_INCL_PROTOCOL_FLIRS_MS * (unsigned)in->opt.flirs_nodes;
            wait_for(in, ZW_INCL_ADDING, after(in, t));
            zw_incl_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = ZW_INCL_EV_ADDING;
            ev.node = in->node;
            ev.nif = in->nif;
            ev.nif_len = in->nif_len;
            emit(in, &ev);
        }
        break;
    case ZW_ADD_NODE_STATUS_ADDING_CONTROLLER:
        if (in->state == ZW_INCL_LISTENING || in->state == ZW_INCL_FOUND ||
            in->state == ZW_INCL_STARTING) {
            in->node = cb->node.node_id; /* the owner marks it "another controller: remove it" */
            stop_and_end(in, ZW_INCL_CONTROLLER);
        }
        break;
    case ZW_ADD_NODE_STATUS_PROTOCOL_DONE:
        if (in->state == ZW_INCL_ADDING) {
            if (!submit_add_node(in, ZW_ADD_NODE_TYPE_STOP, K_FINISH)) {
                stop_and_end(in, ZW_INCL_TIMEOUT);
                return;
            }
            wait_for(in, ZW_INCL_FINISHING, UINT64_MAX); /* its callback timeout ends it */
        }
        break;
    case ZW_ADD_NODE_STATUS_FAILED:
        if (in->state >= ZW_INCL_STARTING && in->state <= ZW_INCL_FINISHING) {
            stop_and_end(in, ZW_INCL_FAILED);
        }
        break;
    default:
        break;
    }
}

static void radio_restarted(zw_incl_t *in);

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_incl_t *in = ctx;
    if ((tag & TAG_MASK) != TAG_BASE || (tag & 0xFFFu) != (in->gen & 0xFFFu)) {
        return; /* an earlier inclusion's */
    }
    const unsigned kind = (unsigned)(tag >> 12) & 0xFu;
    if (r->status == ZW_TX_LINK_DOWN && in->state != ZW_INCL_IDLE) {
        end(in, in->state >= ZW_INCL_ROUTE ? ZW_INCL_ADDED : ZW_INCL_LINK_DOWN,
            in->state >= ZW_INCL_ROUTE ? ZW_INCL_SEC_FAILED : ZW_INCL_SEC_NONE,
            in->state >= ZW_INCL_ROUTE ? ZW_INCL_S2_ERROR : ZW_INCL_S2_OK, 0u);
        return;
    }
    if (r->status == ZW_TX_MODULE_RESTARTED &&
        (kind == K_START || kind == K_FINISH || kind == K_REPLACE)) {
        radio_restarted(in); /* its report will never come (serial-api.md §2) */
        return;
    }
    zw_api_add_node_cb_t cb;
    const bool report = r->status == ZW_TX_OK && r->callback != NULL &&
                        zw_api_add_node_cb_parse(r->callback, r->callback_len, nid(in), &cb);
    switch (kind) {
    case K_START:
        if (in->state != ZW_INCL_STARTING) {
            break;
        }
        if (!report) {
            stop_and_end(in, ZW_INCL_NOT_LISTENING); /* no Ready in 10 s */
            break;
        }
        in->add_cb = r->callback_id;
        if (cb.status == ZW_ADD_NODE_STATUS_FAILED) {
            stop_and_end(in, ZW_INCL_FAILED);
            break;
        }
        if (in->smart) {
            /* no window and nobody to tell: the device comes by itself, or FOUND's time ends it */
            wait_for(in, ZW_INCL_FOUND, after(in, ZW_INCL_FOUND_MS));
            if (cb.status != ZW_ADD_NODE_STATUS_READY &&
                cb.status != ZW_ADD_NODE_STATUS_NODE_FOUND) {
                on_status(in, &cb);
            }
            break;
        }
        {
            /* listening - at Ready, or at once when the module skipped it (then the report is
             * the next step's) */
            wait_for(in, ZW_INCL_LISTENING, after(in, ZW_INCL_WINDOW_MS));
            const zw_incl_event_t ev = {.kind = ZW_INCL_EV_LISTENING, .deadline = in->deadline};
            emit(in, &ev);
        }
        if (cb.status != ZW_ADD_NODE_STATUS_READY && in->state == ZW_INCL_LISTENING) {
            on_status(in, &cb);
        }
        break;
    case K_FINISH:
        if (in->state != ZW_INCL_FINISHING) {
            break;
        }
        if (report && cb.status == ZW_ADD_NODE_STATUS_DONE) {
            added(in, cb.node.node_id);
        } else if (report && cb.status == ZW_ADD_NODE_STATUS_FAILED) {
            stop_and_end(in, ZW_INCL_FAILED);
        } else {
            stop_and_end(in, ZW_INCL_TIMEOUT);
        }
        break;
    case K_REPLACE:
        if (!in->replace || in->checking || in->state != ZW_INCL_STARTING) {
            break;
        }
        if (r->status == ZW_TX_NOT_ACCEPTED) {
            /* its response: why the module would not start (serial-api.md 6.18) */
            const unsigned why =
                r->response != NULL && r->response_len >= 1u ? (unsigned)r->response[0] : 0u;
            end(in,
                (why & REPLACE_NOT_IN_FAILED_LIST) != 0u ? ZW_INCL_NOT_FAILED
                : (why & REPLACE_BUSY) != 0u             ? ZW_INCL_BUSY
                                                         : ZW_INCL_REFUSED,
                ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
            break;
        }
        {
            zw_api_assign_suc_return_route_cb_t rc; /* the same `callback ID | status` */
            if (!(r->status == ZW_TX_OK && r->callback != NULL &&
                  zw_api_assign_suc_return_route_cb_parse(r->callback, r->callback_len, &rc))) {
                end(in, ZW_INCL_NOT_LISTENING, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
                break;
            }
            in->add_cb = r->callback_id; /* its later reports carry it (module frames) */
            replace_status(in, rc.transmit_status);
        }
        break;
    case K_ROUTE:
        if (in->state == ZW_INCL_ROUTE) {
            zw_api_assign_suc_return_route_cb_t rc;
            in->route_failed = !(r->status == ZW_TX_OK && r->callback != NULL &&
                                 zw_api_assign_suc_return_route_cb_parse(r->callback,
                                                                         r->callback_len, &rc) &&
                                 rc.transmit_status == (uint8_t)ZW_TX_STATUS_OK);
            begin_security(in); /* a route that failed does not stop the device joining */
        }
        break;
    default:
        break;
    }
}

/* ---- The key exchange (security.md §5) ------------------------------------------------------ */

/* The keys the panel can grant: its S2 keys (S0 is phase 3). */
static uint8_t panel_keys(const zw_incl_t *in)
{
    uint8_t k = 0u;
    static const uint8_t BITS[3] = {ZW_S2_KEY_UNAUTH, ZW_S2_KEY_AUTH, ZW_S2_KEY_ACCESS};
    for (size_t i = 0; i < 3u; i++) {
        if (zw_s2_network_key(in->s2, in->node, BITS[i]) != NULL) {
            k = (uint8_t)(k | BITS[i]);
        }
    }
    return k;
}

/* Step 1's checks, then step 2: the installer decides. */
static void on_kex_report(zw_incl_t *in, const zw_s2_rx_t *rx)
{
    zw_s2_kex_t k;
    if (rx->security != ZW_SEC_NONE || !zw_s2_kex_report_parse(rx->command, rx->len, &k)) {
        return;
    }
    in->report = k;
    uint8_t usable = (uint8_t)(k.keys & panel_keys(in));
    if (in->smart) {
        usable = (uint8_t)(usable & in->ss.grant); /* the entry's keys, never more */
    }
    uint8_t fail = 0u;
    if ((k.flags & ZW_S2_KEX_ECHO) != 0u) {
        fail = ZW_S2_KEX_FAIL_CANCEL;
    } else if (k.schemes != ZW_S2_KEX_SCHEME_1) {
        fail = ZW_S2_KEX_FAIL_SCHEME;
    } else if (k.curves != ZW_S2_KEX_CURVE_25519) {
        fail = ZW_S2_KEX_FAIL_CURVES;
    } else if ((k.flags & ZW_S2_KEX_CSA) != 0u) {
        fail = ZW_S2_KEX_FAIL_CANCEL; /* client-side authentication is not supported */
    } else if (usable == 0u) {
        fail = ZW_S2_KEX_FAIL_KEY;
    }
    if (fail != 0u) {
        /* a device that lists Security 2 but asks only for S0: a possible downgrade attack */
        const bool downgrade = fail == ZW_S2_KEX_FAIL_KEY &&
                               ((unsigned)k.keys & ZW_S2_KEYS_S2) == 0u &&
                               ((unsigned)k.keys & ZW_S2_KEY_S0) != 0u;
        if (in->smart && fail == ZW_S2_KEX_FAIL_KEY && !downgrade) {
            kex_fail(in, fail, ZW_INCL_SEC_FAILED, ZW_INCL_S2_WRONG_DEVICE, fail);
            return;
        }
        refuse(in, fail, downgrade);
        return;
    }
    wait_for(in, ZW_INCL_GRANT, after(in, ZW_INCL_TAI_MS));
    if (in->smart) {
        (void)zw_incl_grant(in, in->now, usable); /* §4.2: the entry decides, not the installer */
        return;
    }
    zw_incl_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_INCL_EV_GRANT;
    ev.node = in->node;
    ev.requested = usable;
    ev.deadline = in->deadline;
    emit(in, &ev);
}

/* Step 6: the temporary key, from the shared secret; then step 8's wait for the echo. */
static void derive(zw_incl_t *in)
{
    const zw_crypto_port_t *c = &in->s2->crypto;
    uint8_t shared[32], prk[16], key_ccm[16], pstring[32];
    const bool ok = zw_s2_shared_secret(c, in->priv, in->dev_pub, shared) &&
                    zw_s2_temp_key_extract(c, shared, in->pub, in->dev_pub, prk) &&
                    zw_s2_temp_key_expand(c, prk, key_ccm, pstring) &&
                    zw_s2_set_temp_key(in->s2, in->node, key_ccm, pstring);
    zw_wipe(shared, sizeof shared);
    zw_wipe(prk, sizeof prk);
    zw_wipe(key_ccm, sizeof key_ccm);
    zw_wipe(pstring, sizeof pstring);
    zw_wipe(in->priv, sizeof in->priv);
    if (!ok) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    in->temp_installed = true;
    in->wrong_pin_count = 0u;
    wait_for(in, ZW_INCL_ECHO, in->tai2_until);
}

/* Steps 3-5: the device's Public Key Report, the panel's at once, the PIN when authenticated. */
static void on_pubkey(zw_incl_t *in, const zw_s2_rx_t *rx)
{
    zw_s2_pubkey_t pk;
    if (rx->security != ZW_SEC_NONE || !zw_s2_pubkey_parse(rx->command, rx->len, &pk)) {
        return;
    }
    if (pk.including) {
        refuse(in, ZW_S2_KEX_FAIL_CANCEL, false); /* its bit must be 0 */
        return;
    }
    memcpy(in->dev_pub, pk.key, sizeof in->dev_pub);
    uint8_t f[3u + ZW_S2_PUBKEY_LEN];
    const zw_s2_pubkey_t ours = {true, in->pub};
    if (!zw_s2_keypair(&in->s2->crypto, in->priv, in->pub) ||
        !s2_send(in, ZW_SEC_NONE, f, zw_s2_pubkey_build(f, sizeof f, &ours))) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    in->tai2_until = after(in, ZW_INCL_TAI_MS);
    const bool pin = (in->granted & (ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)) != 0u;
    if (in->smart) {
        /* §4.2: the device must be the one the code describes - its key is the DSK (bytes 0-1
         * are zero when a PIN is asked for: the DSK's first group is the PIN) */
        if (memcmp(&in->dev_pub[2], &in->ss.dsk[2], 14u) != 0 ||
            (!pin && memcmp(in->dev_pub, in->ss.dsk, 2u) != 0)) {
            kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_WRONG_DEVICE, 0u);
            return;
        }
        in->dev_pub[0] = in->ss.dsk[0];
        in->dev_pub[1] = in->ss.dsk[1];
        derive(in);
        return;
    }
    if (!pin) {
        derive(in);
        return;
    }
    wait_for(in, ZW_INCL_PIN, in->tai2_until);
    zw_incl_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_INCL_EV_PIN;
    ev.node = in->node;
    ev.deadline = in->deadline;
    (void)zw_dsk_to_text(in->dev_pub, ev.dsk, sizeof ev.dsk);
    emit(in, &ev);
}

/* Steps 8-9: the echo must be the KEX Set itself with echo 1; then the KEX Report echo. */
static void on_echo(zw_incl_t *in, const zw_s2_rx_t *rx)
{
    zw_s2_kex_t k;
    if (rx->security != ZW_SEC_TEMP || !zw_s2_kex_set_parse(rx->command, rx->len, &k)) {
        return;
    }
    if (k.flags != ZW_S2_KEX_ECHO || k.schemes != ZW_S2_KEX_SCHEME_1 ||
        k.curves != ZW_S2_KEX_CURVE_25519 || k.keys != in->granted) {
        /* not what the panel granted: the KEX Set was changed on its way */
        refuse(in, ZW_S2_KEX_FAIL_AUTH, true);
        return;
    }
    zw_s2_kex_t echo = in->report;
    echo.flags = (uint8_t)(echo.flags | ZW_S2_KEX_ECHO);
    uint8_t f[6];
    if (!s2_send(in, ZW_SEC_TEMP, f, zw_s2_kex_report_build(f, sizeof f, &echo))) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    wait_for(in, ZW_INCL_NETKEY_GET, after(in, ZW_INCL_TA_MS));
}

/* Step 10.1-2: a granted key asked for under the temporary key: its Network Key Report. */
static void on_netkey_get(zw_incl_t *in, const zw_s2_rx_t *rx)
{
    uint8_t key = 0u;
    if (!zw_s2_netkey_get_parse(rx->command, rx->len, &key)) {
        return;
    }
    if (rx->security != ZW_SEC_TEMP) {
        refuse(in, ZW_S2_KEX_FAIL_AUTH, false);
        return;
    }
    const uint8_t *netkey = zw_s2_network_key(in->s2, in->node, key);
    if (((unsigned)key & ((unsigned)key - 1u)) != 0u || (key & in->granted) == 0u ||
        netkey == NULL) {
        refuse(in, ZW_S2_KEX_FAIL_KEY_GET, false);
        return;
    }
    const zw_s2_netkey_report_t nk = {key, netkey};
    uint8_t f[3u + ZW_S2_NETKEY_LEN];
    const bool ok = s2_send(in, ZW_SEC_TEMP, f, zw_s2_netkey_report_build(f, sizeof f, &nk));
    zw_wipe(f, sizeof f);
    if (!ok) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    in->exchanging = key;
    zw_s2_set_exchange_class(in->s2, class_of(key));
    wait_for(in, ZW_INCL_VERIFY, after(in, ZW_INCL_TA_MS));
}

/* Step 10.3-4: the Verify under the key just sent; its SPAN forgotten; Transfer End. */
static void on_verify(zw_incl_t *in, const zw_s2_rx_t *rx)
{
    if (!zw_s2_netkey_verify_parse(rx->command, rx->len)) {
        return;
    }
    if (rx->security != class_of(in->exchanging)) {
        refuse(in, ZW_S2_KEX_FAIL_KEY_VERIFY, false); /* another key */
        return;
    }
    zw_s2_forget_span(in->s2, in->node);
    zw_s2_set_exchange_class(in->s2, ZW_SEC_NONE);
    in->delivered = (uint8_t)(in->delivered | in->exchanging);
    in->exchanging = 0u;
    uint8_t f[3];
    if (!s2_send(in, ZW_SEC_TEMP, f, zw_s2_transfer_end_build(f, sizeof f, ZW_S2_TE_VERIFIED))) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return;
    }
    wait_for(in, in->delivered == in->granted ? ZW_INCL_TRANSFER_END : ZW_INCL_NETKEY_GET,
             after(in, ZW_INCL_TA_MS));
}

/* Step 11: the device's Transfer End, "key request complete": done. */
static void on_transfer_end(zw_incl_t *in, const zw_s2_rx_t *rx)
{
    uint8_t flags = 0u;
    if (rx->security != ZW_SEC_TEMP || !zw_s2_transfer_end_parse(rx->command, rx->len, &flags)) {
        return;
    }
    if ((flags & ZW_S2_TE_COMPLETE) == 0u || in->delivered == 0u) {
        refuse(in, ZW_S2_KEX_FAIL_CANCEL, false);
        return;
    }
    (void)zw_s2_set_granted(in->s2, in->node, in->delivered);
    end(in, ZW_INCL_ADDED, security_of(in->delivered), ZW_INCL_S2_OK, 0u);
}

/* ---- API ------------------------------------------------------------------------------------- */

void zw_incl_init(zw_incl_t *in, const zw_incl_port_t *port, zw_s2_t *s2)
{
    memset(in, 0, sizeof *in);
    in->port = *port;
    in->s2 = s2;
    in->state = ZW_INCL_IDLE;
    in->deadline = UINT64_MAX;
}

static zw_incl_result_t begin(zw_incl_t *in, const zw_incl_options_t *opt, uint8_t type);

zw_incl_result_t zw_incl_start(zw_incl_t *in, uint64_t now, bool allowed,
                               const zw_incl_options_t *opt)
{
    in->now = now;
    if (!allowed) {
        return ZW_INCL_NOT_ALLOWED; /* ZW-005: installer mode, every partition disarmed */
    }
    if (in->state != ZW_INCL_IDLE) {
        return ZW_INCL_BUSY;
    }
    in->smart = false;
    return begin(in, opt, ZW_ADD_NODE_TYPE_ANY);
}

zw_incl_result_t zw_incl_smartstart(zw_incl_t *in, uint64_t now, const zw_incl_options_t *opt,
                                    const zw_incl_smartstart_t *ss)
{
    in->now = now;
    if (ss == NULL || ss->grant == 0u) {
        return ZW_INCL_NOT_ALLOWED;
    }
    if (in->state != ZW_INCL_IDLE) {
        return ZW_INCL_BUSY;
    }
    in->smart = true;
    in->ss = *ss;
    const zw_incl_result_t r = begin(in, opt, ZW_ADD_NODE_TYPE_SMARTSTART_DSK);
    if (r != ZW_INCL_STARTED) {
        in->smart = false;
    }
    return r;
}

static zw_incl_result_t begin(zw_incl_t *in, const zw_incl_options_t *opt, uint8_t type)
{
    in->gen = (uint16_t)((unsigned)in->gen + 1u);
    memset(&in->opt, 0, sizeof in->opt);
    if (opt != NULL) {
        in->opt = *opt;
    }
    in->node = 0u;
    in->nif_len = 0u;
    in->route_failed = false;
    in->probing = false;
    in->granted = in->delivered = in->exchanging = 0u;
    in->add_cb = 0u;
    in->temp_installed = false;
    wait_for(in, ZW_INCL_STARTING, UINT64_MAX); /* Ready's 10 s are the request's */
    if (!submit_add_node(in, type, K_START)) {
        in->state = ZW_INCL_IDLE;
        return ZW_INCL_NOT_LISTENING;
    }
    return ZW_INCL_STARTED;
}

static bool replace_response_ok(void *ctx, const uint8_t *payload, size_t len)
{
    (void)ctx;
    return len >= 1u && payload[0] == 0x00u; /* started */
}

zw_incl_result_t zw_incl_replace(zw_incl_t *in, uint64_t now, bool allowed,
                                 const zw_incl_options_t *opt, uint16_t node)
{
    in->now = now;
    if (!allowed) {
        return ZW_INCL_NOT_ALLOWED;
    }
    if (in->state != ZW_INCL_IDLE) {
        return ZW_INCL_BUSY;
    }
    if (node == 0u || node >= 256u) {
        return ZW_INCL_BAD_NODE_ID; /* Long Range devices are not replaced (phase 3) */
    }
    in->gen = (uint16_t)((unsigned)in->gen + 1u);
    memset(&in->opt, 0, sizeof in->opt);
    if (opt != NULL) {
        in->opt = *opt;
    }
    in->smart = false;
    in->replace = true;
    in->node = node;
    in->nif_len = 0u;
    in->route_failed = false;
    in->probing = false;
    in->granted = in->delivered = in->exchanging = 0u;
    in->add_cb = 0u;
    in->temp_installed = false;
    /* §10: the old device is checked first (the caller's zw_remove_check_failed) - the module
     * takes ReplaceFailedNode only for a node it lists as failed. The check's timeouts end it. */
    in->checking = true;
    wait_for(in, ZW_INCL_STARTING, UINT64_MAX);
    return ZW_INCL_STARTED;
}

/* ReplaceFailedNode for the checked node: false when it could not go. */
static bool send_replace(zw_incl_t *in)
{
    const zw_api_assign_suc_return_route_req_t a = {in->node, 0u}; /* `node | callback ID` */
    uint8_t buf[6];
    const size_t n = zw_api_assign_suc_return_route_req_build(buf, sizeof buf, nid(in), &a);
    size_t at = 0u;
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_REPLACE_FAILED_NODE;
    req.payload = buf;
    req.payload_len = n;
    req.expect = ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK;
    req.response_ok = replace_response_ok;
    /* its first report ends the request ("ready", or why not): the rest are the module's own
     * frames, so nothing else waits behind it for the whole window */
    req.callback_timeout_ms = ZW_INCL_READY_MS;
    req.prio = ZW_PRIO_NODE;
    req.done = request_done;
    req.ctx = in;
    req.tag = TAG_BASE | (uint32_t)K_REPLACE << 12 | (in->gen & 0xFFFu);
    if (n == 0u || !zw_api_callback_id_offset(req.func, buf, n, &at)) {
        return false;
    }
    req.callback_id_at = (uint8_t)at;
    return in->port.submit(in->port.ctx, &req) == ZW_SUBMIT_OK;
}

void zw_incl_replace_checked(zw_incl_t *in, uint64_t now, zw_incl_result_t verdict)
{
    in->now = now;
    if (!in->replace || !in->checking || in->state != ZW_INCL_STARTING) {
        return; /* stopped meanwhile, or no replacement */
    }
    in->checking = false;
    if (verdict != ZW_INCL_STARTED) {
        end(in, verdict, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u); /* nothing was sent: no stop */
        return;
    }
    if (!send_replace(in)) {
        end(in, ZW_INCL_NOT_LISTENING, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
    }
}

void zw_incl_keep_listening(zw_incl_t *in, uint64_t now)
{
    in->now = now;
    if (in->state == ZW_INCL_LISTENING && !in->replace) {
        in->deadline = after(in, ZW_INCL_WINDOW_MS);
        const zw_incl_event_t ev = {.kind = ZW_INCL_EV_LISTENING, .deadline = in->deadline};
        emit(in, &ev);
    }
}

void zw_incl_stop(zw_incl_t *in, uint64_t now)
{
    in->now = now;
    if (in->state == ZW_INCL_STARTING && in->checking) {
        end(in, ZW_INCL_STOPPED, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u); /* nothing sent to stop */
    } else if (in->state == ZW_INCL_STARTING || in->state == ZW_INCL_LISTENING) {
        stop_and_end(in, ZW_INCL_STOPPED);
    }
}

bool zw_incl_grant(zw_incl_t *in, uint64_t now, uint8_t keys)
{
    in->now = now;
    const uint8_t usable = (uint8_t)(in->report.keys & panel_keys(in));
    if (in->state != ZW_INCL_GRANT || keys == 0u || (keys & (uint8_t)~usable) != 0u) {
        return false; /* nothing is granted that was not requested */
    }
    in->granted = keys;
    const zw_s2_kex_t set = {0u, ZW_S2_KEX_SCHEME_1, ZW_S2_KEX_CURVE_25519, keys};
    uint8_t f[6];
    if (!s2_send(in, ZW_SEC_NONE, f, zw_s2_kex_set_build(f, sizeof f, &set))) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
        return true;
    }
    wait_for(in, ZW_INCL_PUBKEY, after(in, ZW_INCL_TA_MS));
    return true;
}

bool zw_incl_pin(zw_incl_t *in, uint64_t now, uint16_t pin)
{
    in->now = now;
    if (in->state != ZW_INCL_PIN) {
        return false;
    }
    in->dev_pub[0] = (uint8_t)(pin >> 8); /* §4: big-endian into bytes 0-1 */
    in->dev_pub[1] = (uint8_t)pin;
    derive(in);
    return true;
}

void zw_incl_cancel(zw_incl_t *in, uint64_t now)
{
    in->now = now;
    if (is_kex_state(in->state)) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_CANCELLED, 0u);
    }
}

void zw_incl_module_frame(zw_incl_t *in, uint64_t now, const zw_frame_t *frame)
{
    in->now = now;
    if (frame != NULL && frame->type == ZW_TYPE_REQUEST &&
        frame->func == (uint8_t)ZW_FUNC_REPLACE_FAILED_NODE) {
        zw_api_assign_suc_return_route_cb_t rc;
        if (in->replace && in->add_cb != 0u &&
            (in->state == ZW_INCL_STARTING || in->state == ZW_INCL_LISTENING) &&
            zw_api_assign_suc_return_route_cb_parse(frame->payload, frame->payload_len, &rc) &&
            rc.callback_id == in->add_cb) {
            replace_status(in, rc.transmit_status);
        }
        return;
    }
    zw_api_add_node_cb_t cb;
    if (frame == NULL || frame->type != ZW_TYPE_REQUEST ||
        frame->func != (uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK || in->add_cb == 0u ||
        in->state < ZW_INCL_LISTENING || in->state > ZW_INCL_ADDING ||
        !zw_api_add_node_cb_parse(frame->payload, frame->payload_len, nid(in), &cb) ||
        cb.callback_id != in->add_cb) {
        return;
    }
    on_status(in, &cb);
}

void zw_incl_s2_command(zw_incl_t *in, uint64_t now, const zw_s2_rx_t *rx)
{
    in->now = now;
    if (!is_kex_state(in->state) || rx->node != in->node || rx->len < 2u ||
        rx->command[0] != ZW_S2_CC || rx->multi_channel || rx->supervised || rx->multi_command) {
        return; /* key-exchange commands come on their own */
    }
    const uint8_t cmd = rx->command[1];
    if (cmd == ZW_S2_KEX_FAIL) {
        uint8_t type = 0u;
        if (zw_s2_kex_fail_parse(rx->command, rx->len, &type)) {
            /* refusing the panel's KEX Report echo: its KEX Report was changed on its way */
            const bool tampered = in->state == ZW_INCL_NETKEY_GET && in->delivered == 0u &&
                                  type == ZW_S2_KEX_FAIL_AUTH;
            end(in, ZW_INCL_ADDED, tampered ? ZW_INCL_SEC_DOWNGRADED : ZW_INCL_SEC_FAILED,
                tampered ? ZW_INCL_S2_TAMPERED : ZW_INCL_S2_DEVICE_FAIL, type);
        }
        return;
    }
    switch (in->state) {
    case ZW_INCL_KEX_REPORT:
        if (cmd == ZW_S2_KEX_REPORT) {
            on_kex_report(in, rx);
        }
        break;
    case ZW_INCL_PUBKEY:
        if (cmd == ZW_S2_PUBLIC_KEY_REPORT) {
            on_pubkey(in, rx);
        }
        break;
    case ZW_INCL_ECHO:
        if (cmd == ZW_S2_KEX_SET) {
            on_echo(in, rx);
        }
        break;
    case ZW_INCL_NETKEY_GET:
        if (cmd == ZW_S2_NETWORK_KEY_GET) {
            on_netkey_get(in, rx);
        } else if (cmd == ZW_S2_TRANSFER_END) {
            on_transfer_end(in, rx); /* it took fewer keys than granted */
        }
        break;
    case ZW_INCL_VERIFY:
        if (cmd == ZW_S2_NETWORK_KEY_VERIFY) {
            on_verify(in, rx);
        }
        break;
    case ZW_INCL_TRANSFER_END:
        if (cmd == ZW_S2_TRANSFER_END) {
            on_transfer_end(in, rx);
        } else if (cmd == ZW_S2_NETWORK_KEY_GET) {
            refuse(in, ZW_S2_KEX_FAIL_KEY_GET, false); /* every granted key was sent */
        }
        break;
    default:
        break;
    }
}

void zw_incl_s2_event(zw_incl_t *in, uint64_t now, uint16_t node, zw_s2_event_t ev)
{
    in->now = now;
    /* §5 step 8: a frame that fails to decrypt a second time with a nonce already exchanged -
     * the PIN was wrong */
    if (in->state == ZW_INCL_ECHO && in->temp_installed && node == in->node &&
        ev == ZW_S2_EV_DECRYPT_FAILED_SPAN && ++in->wrong_pin_count >= 2u) {
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_WRONG_PIN, 0u);
    }
}

void zw_incl_link_down(zw_incl_t *in, uint64_t now)
{
    in->now = now;
    if (in->state == ZW_INCL_IDLE) {
        return;
    }
    if (in->state >= ZW_INCL_ROUTE) {
        end(in, ZW_INCL_ADDED, ZW_INCL_SEC_FAILED, ZW_INCL_S2_ERROR, 0u);
    } else {
        end(in, ZW_INCL_LINK_DOWN, ZW_INCL_SEC_NONE, ZW_INCL_S2_OK, 0u);
    }
}

/* network.md §4 "The radio restarts": the module's window is gone. Before Done: the end - the
 * stop queued behind the restart undoes a start still waiting there. Between ProtocolDone and
 * Done too, as Z-Wave JS's performInclusion ends when finishInclusion gets no Done. */
static void radio_restarted(zw_incl_t *in)
{
    switch (in->state) {
    case ZW_INCL_STARTING:
        if (!in->checking) { /* a replacement's check has sent nothing yet: it goes on */
            stop_and_end(in, ZW_INCL_RADIO_RESTARTED);
        }
        break;
    case ZW_INCL_LISTENING:
    case ZW_INCL_FOUND:
    case ZW_INCL_ADDING:
    case ZW_INCL_FINISHING: stop_and_end(in, ZW_INCL_RADIO_RESTARTED); break;
    default: break; /* idle, or joined: the route and the key exchange carry on */
    }
}

void zw_incl_radio_restarted(zw_incl_t *in, uint64_t now)
{
    in->now = now;
    radio_restarted(in);
}

void zw_incl_poll(zw_incl_t *in, uint64_t now)
{
    in->now = now;
    if (now < in->deadline) {
        return;
    }
    in->deadline = UINT64_MAX;
    switch (in->state) {
    case ZW_INCL_LISTENING:
        stop_and_end(in, ZW_INCL_NO_DEVICE);
        break;
    case ZW_INCL_FOUND:
    case ZW_INCL_ADDING:
        stop_and_end(in, ZW_INCL_TIMEOUT);
        break;
    case ZW_INCL_KEX_REPORT:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED,
                 in->probing ? ZW_INCL_S2_NO_INFO : ZW_INCL_S2_TIMEOUT, 1u);
        break;
    case ZW_INCL_GRANT:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, ZW_INCL_TAI1);
        break;
    case ZW_INCL_PUBKEY:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, 2u);
        break;
    case ZW_INCL_PIN:
    case ZW_INCL_ECHO:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, ZW_INCL_TAI2);
        break;
    case ZW_INCL_NETKEY_GET:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, 3u);
        break;
    case ZW_INCL_VERIFY:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, 4u);
        break;
    case ZW_INCL_TRANSFER_END:
        kex_fail(in, ZW_S2_KEX_FAIL_CANCEL, ZW_INCL_SEC_FAILED, ZW_INCL_S2_TIMEOUT, 5u);
        break;
    default:
        break;
    }
}

uint64_t zw_incl_next_deadline(const zw_incl_t *in)
{
    return in->deadline;
}

zw_incl_state_t zw_incl_state(const zw_incl_t *in)
{
    return in->state;
}

uint16_t zw_incl_node(const zw_incl_t *in)
{
    return in->node;
}
