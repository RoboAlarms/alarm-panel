/*
 * zw_net_routes.c - rebuilding routes as a network operation (network.md §11): the targets from
 * the node cache, a sleeper taken at its wake-up and kept awake while it is repaired, the job's
 * news for the owner. See zw_net.h.
 */
#include <string.h>

#include "zw_net_internal.h"

static zw_submit_t rt_submit(void *ctx, const zw_request_t *req)
{
    zw_net_t *net = ctx;
    return zw_host_submit(&net->host, req);
}

static bool rt_ping(void *ctx, uint16_t node, uint32_t tag)
{
    zw_net_t *net = ctx;
    static const uint8_t NOP[1] = {0x00u};
    return zw_s2_send(&net->s2, net->now, node, ZW_SEC_NONE, NOP, sizeof NOP, tag) == ZW_S2_OK;
}

static void rt_event(void *ctx, const zw_routes_event_t *e)
{
    zw_net_t *net = ctx;
    if (net->hold_awake != 0u && net->hold_awake != zw_routes_holding(&net->routes) &&
        net->routes_held) {
        net->routes_held = false;
        zw_net_hold_awake(net, net->now, 0u); /* its repair ended: it may sleep again */
    }
    if (e->kind == ZW_ROUTES_EV_END && net->op == OP_ROUTES) {
        net->op = OP_NONE;
    }
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_ROUTES;
    ev.node = e->node;
    ev.routes = *e;
    zw_net_emit(net, &ev);
}

void zw_net_routes_init(zw_net_t *net)
{
    const zw_routes_port_t port = {net, rt_submit, rt_ping, rt_event};
    zw_routes_init(&net->routes, &port);
}

void zw_net_routes_woke(zw_net_t *net, uint16_t node)
{
    if (net->op == OP_ROUTES && zw_routes_woke(&net->routes, net->now, node) &&
        zw_routes_holding(&net->routes) == node) {
        net->routes_held = true;
        zw_net_hold_awake(net, net->now, node); /* no No More Information until it is done */
    }
}

zw_net_result_t zw_net_rebuild_routes(zw_net_t *net, uint64_t now, bool allowed, uint16_t node)
{
    net->now = now;
    if (!allowed) {
        return ZW_NET_NOT_ALLOWED;
    }
    if (zw_host_link(&net->host) != ZW_LINK_UP || !net->checked) {
        return ZW_NET_LINK_DOWN;
    }
    if (net->op != OP_NONE) {
        return ZW_NET_BUSY;
    }
    if (!zw_routes_prepare(&net->routes)) {
        return ZW_NET_BUSY;
    }
    size_t n = 0u;
    uint16_t listening = 0u, flirs = 0u;
    const zw_node_cache_t *nodes = &net->nodes;
    for (size_t i = 0; i < zw_node_count(nodes); i++) {
        const zw_node_record_t *rec = zw_node_at_const(nodes, i);
        listening = (uint16_t)((unsigned)listening + (rec->is_listening ? 1u : 0u));
        flirs = (uint16_t)((unsigned)flirs + (rec->is_flirs ? 1u : 0u));
        if (node != 0u && rec->node_id != node) {
            continue;
        }
        const zw_routes_target_t t = {rec->node_id, zw_iv_sleeping(rec),
                                      !zw_health_is_reachable(&net->health, rec->node_id),
                                      rec->node_id >= 256u};
        n += zw_routes_add(&net->routes, &t) ? 1u : 0u;
    }
    if (n == 0u) {
        return ZW_NET_INVALID;
    }
    zw_net_smart_pause(net);
    if (!zw_routes_go(&net->routes, now, listening, flirs, zw_net_nid(net))) {
        return ZW_NET_BUSY;
    }
    if (zw_routes_running(&net->routes)) {
        net->op = OP_ROUTES;
    }
    return ZW_NET_STARTED;
}

void zw_net_stop_routes(zw_net_t *net, uint64_t now)
{
    net->now = now;
    if (net->op == OP_ROUTES) {
        zw_routes_stop(&net->routes, now); /* its END frees the operation */
    }
}

const zw_routes_t *zw_net_routes(const zw_net_t *net)
{
    return &net->routes;
}
