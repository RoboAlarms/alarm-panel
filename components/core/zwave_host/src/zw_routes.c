/*
 * zw_routes.c - rebuilding routes (see zw_routes.h).
 */
#include "zwave_host/zw_routes.h"

#include <string.h>

#define TAG_BASE 0xFF400000u
#define TAG_MASK 0xFFF00000u

enum { S_NONE = 0, S_PING, S_NEIGHBOR, S_SUC, S_DELETE };

static uint32_t tag_of(const zw_routes_t *r, unsigned step)
{
    return TAG_BASE | (uint32_t)step << 16 | (uint32_t)r->gen;
}

static void emit(zw_routes_t *r, zw_routes_event_kind_t kind, uint16_t node, uint8_t state,
                 bool stopped)
{
    if (r->port.event == NULL) {
        return;
    }
    zw_routes_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.node = node;
    ev.state = state;
    ev.stopped = stopped;
    zw_routes_counts(r, &ev.done, &ev.total, &ev.failed, &ev.skipped, &ev.waiting);
    r->port.event(r->port.ctx, &ev);
}

static void end_job(zw_routes_t *r, bool stopped)
{
    for (size_t i = 0; i < r->n; i++) {
        zw_routes_node_t *n = &r->nodes[i];
        if (n->state == ZW_ROUTES_PENDING || n->state == ZW_ROUTES_ASLEEP ||
            n->state == ZW_ROUTES_RUNNING) {
            n->state = ZW_ROUTES_SKIPPED;
        }
    }
    r->running = false;
    r->cur = SIZE_MAX;
    r->step = S_NONE;
    r->out = false;
    r->gen = (uint16_t)((unsigned)r->gen + 1u); /* whatever is still out is ignored */
    emit(r, ZW_ROUTES_EV_END, 0u, 0u, stopped);
}

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *res);

static zw_callback_kind_t neighbor_kind(void *ctx, const uint8_t *p, size_t len)
{
    (void)ctx;
    return len >= 2u && p[1] == ZW_NEIGHBOR_UPDATE_STARTED ? ZW_CALLBACK_INTERMEDIATE
                                                           : ZW_CALLBACK_FINAL;
}

/* A `node | callback` module command (48, 51, 47) for the device running. */
static bool submit(zw_routes_t *r, uint8_t func, unsigned step)
{
    const zw_routes_node_t *n = &r->nodes[r->cur];
    const zw_api_assign_suc_return_route_req_t a = {n->node, 0u};
    uint8_t buf[6];
    const size_t len = zw_api_assign_suc_return_route_req_build(buf, sizeof buf, r->nid, &a);
    size_t at = 0u;
    if (len == 0u || !zw_api_callback_id_offset(func, buf, len, &at)) {
        return false;
    }
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = func;
    req.payload = buf;
    req.payload_len = len;
    req.prio = ZW_PRIO_NODE;
    req.callback_id_at = (uint8_t)at;
    req.done = request_done;
    req.ctx = r;
    req.tag = tag_of(r, step);
    if (func == (uint8_t)ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE) {
        req.expect = ZW_EXPECT_CALLBACK; /* no response: "started", then done or failed */
        req.callback_kind = neighbor_kind;
        req.callback_timeout_ms = ZW_ROUTES_BASE_MS +
                                  ZW_ROUTES_PER_LISTENING_MS * (uint32_t)r->listening_nodes +
                                  ZW_ROUTES_PER_FLIRS_MS * (uint32_t)r->flirs_nodes;
    } else {
        req.expect = ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK;
        req.callback_timeout_ms = ZW_ROUTES_ROUTE_MS;
    }
    r->step = (uint8_t)step;
    r->out = r->port.submit(r->port.ctx, &req) == ZW_SUBMIT_OK;
    return r->out;
}

static void pick_next(zw_routes_t *r);

/* The device running ended. */
static void finish(zw_routes_t *r, uint8_t state)
{
    zw_routes_node_t *n = &r->nodes[r->cur];
    n->state = state;
    r->cur = SIZE_MAX;
    r->step = S_NONE;
    r->out = false;
    emit(r, ZW_ROUTES_EV_PROGRESS, n->node, state, false);
    pick_next(r);
}

static void neighbor(zw_routes_t *r)
{
    if (r->tries >= ZW_ROUTES_TRIES) {
        finish(r, ZW_ROUTES_FAILED);
        return;
    }
    r->tries++;
    if (!submit(r, (uint8_t)ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE, S_NEIGHBOR)) {
        finish(r, ZW_ROUTES_FAILED);
    }
}

static void suc_route(zw_routes_t *r)
{
    if (!submit(r, (uint8_t)ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE, S_SUC)) {
        r->nodes[r->cur].route_failed = true;
        r->tries = 0u;
        if (!submit(r, (uint8_t)ZW_FUNC_DELETE_RETURN_ROUTE, S_DELETE)) {
            finish(r, ZW_ROUTES_DONE);
        }
    }
}

static void delete_routes(zw_routes_t *r)
{
    if (r->tries >= ZW_ROUTES_TRIES) {
        finish(r, ZW_ROUTES_DONE); /* its neighbours are renewed; old routes stay, noted below */
        return;
    }
    r->tries++;
    if (!submit(r, (uint8_t)ZW_FUNC_DELETE_RETURN_ROUTE, S_DELETE)) {
        finish(r, ZW_ROUTES_DONE);
    }
}

static void begin(zw_routes_t *r, size_t i)
{
    r->cur = i;
    r->tries = 0u;
    zw_routes_node_t *n = &r->nodes[i];
    n->state = ZW_ROUTES_RUNNING;
    emit(r, ZW_ROUTES_EV_PROGRESS, n->node, n->state, false);
    if (n->dead) {
        /* a device known dead: asked first, skipped when it doesn't answer */
        r->step = S_PING;
        r->out = r->port.ping != NULL && r->port.ping(r->port.ctx, n->node, tag_of(r, S_PING));
        if (!r->out) {
            finish(r, ZW_ROUTES_SKIPPED);
        }
        return;
    }
    neighbor(r);
}

/* The next listening device; with none left, the job waits for its sleepers or ends. */
static void pick_next(zw_routes_t *r)
{
    if (!r->running || r->cur != SIZE_MAX) {
        return;
    }
    size_t asleep = 0u;
    for (size_t i = 0; i < r->n; i++) {
        if (r->nodes[i].state == ZW_ROUTES_PENDING) {
            begin(r, i);
            return;
        }
        asleep += r->nodes[i].state == ZW_ROUTES_ASLEEP ? 1u : 0u;
    }
    if (asleep == 0u) {
        end_job(r, false);
    }
}

static void request_done(void *ctx, uint32_t tag, const zw_tx_result_t *res)
{
    zw_routes_t *r = ctx;
    if ((tag & TAG_MASK) != TAG_BASE || (tag & 0xFFFFu) != r->gen || !r->running ||
        r->cur == SIZE_MAX) {
        return; /* an earlier job's, or ended */
    }
    r->out = false;
    zw_api_assign_suc_return_route_cb_t cb;
    const bool heard = res->status == ZW_TX_OK && res->callback != NULL &&
                       zw_api_assign_suc_return_route_cb_parse(res->callback, res->callback_len,
                                                               &cb);
    switch (r->step) {
    case S_NEIGHBOR:
        if (heard && cb.transmit_status == ZW_NEIGHBOR_UPDATE_DONE) {
            suc_route(r);
        } else {
            neighbor(r); /* failed, or no word in time: again, up to its tries */
        }
        break;
    case S_SUC:
        r->nodes[r->cur].route_failed = !(heard && cb.transmit_status == (uint8_t)ZW_TX_STATUS_OK);
        r->tries = 0u;
        delete_routes(r);
        break;
    case S_DELETE:
        if (heard && cb.transmit_status == (uint8_t)ZW_TX_STATUS_OK) {
            finish(r, ZW_ROUTES_DONE);
        } else {
            delete_routes(r);
        }
        break;
    default:
        break;
    }
}

void zw_routes_init(zw_routes_t *r, const zw_routes_port_t *port)
{
    memset(r, 0, sizeof *r);
    r->port = *port;
    r->cur = SIZE_MAX;
}

bool zw_routes_prepare(zw_routes_t *r)
{
    if (r->running) {
        return false;
    }
    r->n = 0u;
    return true;
}

bool zw_routes_add(zw_routes_t *r, const zw_routes_target_t *t)
{
    if (r->running || t == NULL || r->n >= ZW_NODES_MAX) {
        return false;
    }
    zw_routes_node_t *d = &r->nodes[r->n++];
    memset(d, 0, sizeof *d);
    d->node = t->node;
    d->sleeping = t->sleeping;
    d->dead = t->dead;
    /* a sleeper counted unreachable is skipped up front: waiting for a wake-up that never comes
     * would hold the network until the installer taps Stop */
    d->state = t->long_range || t->node >= 256u || (t->sleeping && t->dead) ? ZW_ROUTES_SKIPPED
               : t->sleeping                                                 ? ZW_ROUTES_ASLEEP
                                                : ZW_ROUTES_PENDING;
    return true;
}

bool zw_routes_start(zw_routes_t *r, uint64_t now, const zw_routes_target_t *targets, size_t n,
                     uint16_t listening_nodes, uint16_t flirs_nodes, zw_nid_width_t nid)
{
    if (targets == NULL || !zw_routes_prepare(r)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        (void)zw_routes_add(r, &targets[i]);
    }
    return zw_routes_go(r, now, listening_nodes, flirs_nodes, nid);
}

bool zw_routes_go(zw_routes_t *r, uint64_t now, uint16_t listening_nodes, uint16_t flirs_nodes,
                  zw_nid_width_t nid)
{
    r->now = now;
    if (r->running || r->n == 0u) {
        return false;
    }
    r->gen = (uint16_t)((unsigned)r->gen + 1u);
    r->listening_nodes = listening_nodes;
    r->flirs_nodes = flirs_nodes;
    r->nid = nid;
    r->running = true;
    r->cur = SIZE_MAX;
    r->step = S_NONE;
    emit(r, ZW_ROUTES_EV_PROGRESS, 0u, 0u, false);
    pick_next(r);
    return true;
}

void zw_routes_stop(zw_routes_t *r, uint64_t now)
{
    r->now = now;
    if (r->running) {
        end_job(r, true); /* the step out finishes in the module; its end is ignored */
    }
}

bool zw_routes_woke(zw_routes_t *r, uint64_t now, uint16_t node)
{
    r->now = now;
    if (!r->running || r->cur != SIZE_MAX) {
        return false; /* busy with another: it waits for its next wake-up */
    }
    for (size_t i = 0; i < r->n; i++) {
        if (r->nodes[i].node == node && r->nodes[i].state == ZW_ROUTES_ASLEEP) {
            begin(r, i);
            return r->cur == i;
        }
    }
    return false;
}

uint16_t zw_routes_holding(const zw_routes_t *r)
{
    return (uint16_t)(r->running && r->cur != SIZE_MAX && r->nodes[r->cur].sleeping
                          ? (unsigned)r->nodes[r->cur].node
                          : 0u);
}

void zw_routes_ping_done(zw_routes_t *r, uint64_t now, uint32_t tag, bool delivered)
{
    r->now = now;
    if (tag != tag_of(r, S_PING) || !r->running || r->cur == SIZE_MAX || r->step != S_PING) {
        return;
    }
    r->out = false;
    if (delivered) {
        neighbor(r);
    } else {
        finish(r, ZW_ROUTES_SKIPPED); /* dead: nothing to route to */
    }
}

bool zw_routes_is_tag(uint32_t tag)
{
    return (tag & TAG_MASK) == TAG_BASE;
}

void zw_routes_link_down(zw_routes_t *r, uint64_t now)
{
    r->now = now;
    if (r->running) {
        end_job(r, true); /* the radio went away: the rest waits for another run */
    }
}

void zw_routes_poll(zw_routes_t *r, uint64_t now)
{
    r->now = now;
    pick_next(r);
}

bool zw_routes_running(const zw_routes_t *r)
{
    return r->running;
}

void zw_routes_counts(const zw_routes_t *r, uint16_t *done, uint16_t *total, uint16_t *failed,
                      uint16_t *skipped, uint16_t *waiting)
{
    uint16_t d = 0u, f = 0u, s = 0u, w = 0u;
    for (size_t i = 0; i < r->n; i++) {
        switch (r->nodes[i].state) {
        case ZW_ROUTES_DONE: d++; break;
        case ZW_ROUTES_FAILED: f++; break;
        case ZW_ROUTES_SKIPPED: s++; break;
        case ZW_ROUTES_ASLEEP: w++; break;
        default: break;
        }
    }
    *done = (uint16_t)(d + f + s);
    *total = (uint16_t)r->n;
    *failed = f;
    *skipped = s;
    *waiting = w;
}

uint8_t zw_routes_node_state(const zw_routes_t *r, uint16_t node)
{
    for (size_t i = 0; i < r->n; i++) {
        if (r->nodes[i].node == node) {
            return r->nodes[i].state;
        }
    }
    return ZW_ROUTES_PENDING;
}
