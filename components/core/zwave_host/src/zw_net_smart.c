/*
 * zw_net_smart.c - SmartStart (network.md §4.2): the module's SmartStart listen mode kept on while
 * the owner wants it and nothing else runs, a device's request to join matched to the owner's
 * provisioning list, its inclusion, and a join that failed its key exchange removed again. See
 * zw_net.h.
 */
#include <string.h>

#include "zw_net_internal.h"
#include "zwave_proto/zw_api.h"

#define ADD_FLAGS (ZW_ADD_NODE_FLAG_HIGH_POWER | ZW_ADD_NODE_FLAG_NETWORK_WIDE)

/* AddNodeToNetwork of `type` without a callback: the listen mode on, or off. */
static bool add_node_no_callback(zw_net_t *net, uint8_t type)
{
    const zw_api_add_node_req_t a = {(uint8_t)((unsigned)type | ADD_FLAGS), 0u, false, {0}, {0}};
    uint8_t buf[12];
    const size_t n = zw_api_add_node_req_build(buf, sizeof buf, &a);
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK;
    req.payload = buf;
    req.payload_len = n;
    req.prio = ZW_PRIO_NODE;
    req.tag = zw_net_tag(NT_SMART, (uint32_t)(unsigned)type);
    return n > 0u && zw_host_submit(&net->host, &req) == ZW_SUBMIT_OK;
}

static bool may_run(const zw_net_t *net)
{
    return zw_host_link(&net->host) == ZW_LINK_UP && net->checked &&
           net->radio == ZW_NET_RADIO_OURS && net->op == OP_NONE;
}

void zw_net_smart_pause(zw_net_t *net)
{
    if (net->ss_on) {
        (void)add_node_no_callback(net, ZW_ADD_NODE_TYPE_STOP);
        net->ss_on = false;
    }
}

void zw_net_smart_lost(zw_net_t *net)
{
    net->ss_on = false; /* a module that restarts forgets the mode */
}

void zw_net_smart_poll(zw_net_t *net)
{
    if (!may_run(net)) {
        return;
    }
    if (net->ss_want && !net->ss_on) {
        net->ss_on = add_node_no_callback(net, ZW_ADD_NODE_TYPE_SMARTSTART_LISTEN);
    } else if (!net->ss_want && net->ss_on) {
        zw_net_smart_pause(net);
    }
}

void zw_net_smart_request(zw_net_t *net, const zw_api_app_update_t *u)
{
    zw_api_smartstart_join_t j;
    if (!zw_api_smartstart_join_parse(u->data, u->data_len, zw_net_nid(net), &j)) {
        net->stats.malformed++;
        return;
    }
    net->stats.smartstart_asks++;
    zw_incl_smartstart_t ss;
    const bool long_range = u->type == ZW_APP_UPDATE_SMARTSTART_LONG_RANGE;
    if (!net->ss_want || !may_run(net) || net->port.smartstart_find == NULL ||
        !net->port.smartstart_find(net->port.ctx, j.nwi_home_id, long_range, &ss)) {
        net->stats.smartstart_ignored++; /* not ours, or not now: it asks again */
        return;
    }
    const zw_incl_options_t o = {0u, 0u, zw_net_nid(net)};
    if (zw_incl_smartstart(&net->incl, net->now, &o, &ss) != ZW_INCL_STARTED) {
        net->stats.smartstart_ignored++;
        return;
    }
    net->op = OP_SMART;
    net->ss_on = false; /* AddNode 08 ends the listen mode; it comes back when this ends */
    zw_net_quiet(net);
}

void zw_net_smart_done(zw_net_t *net, const zw_incl_event_t *ev, bool *interview)
{
    zw_net_event_t e;
    memset(&e, 0, sizeof e);
    e.kind = ZW_NET_EV_SMARTSTART;
    e.node = ev->node;
    memcpy(e.dsk, ev->dsk_bytes, sizeof e.dsk);
    const bool secure = ev->security == ZW_INCL_SEC_S2_UNAUTH ||
                        ev->security == ZW_INCL_SEC_S2_AUTH ||
                        ev->security == ZW_INCL_SEC_S2_ACCESS;
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, ev->node);
    if (ev->result == ZW_INCL_ADDED && secure) {
        e.smart = ZW_NET_SMART_JOINED;
        e.generation = (uint16_t)(rec != NULL ? (unsigned)rec->generation : 0u);
    } else if (ev->result == ZW_INCL_ADDED && rec != NULL) {
        /* §4.2: a SmartStart device without its keys is removed again - it goes back to asking
         * (it resets itself); nothing is interviewed */
        e.smart = ZW_NET_SMART_FAILED;
        e.generation = rec->generation;
        *interview = false;
        zw_net_gone_quietly(net, ev->node);
    } else {
        e.smart = ZW_NET_SMART_NOT_JOINED;
    }
    zw_net_emit(net, &e);
}

void zw_net_smartstart(zw_net_t *net, uint64_t now, bool listen)
{
    net->now = now;
    net->ss_want = listen;
    zw_net_smart_poll(net);
}

bool zw_net_smartstart_listening(const zw_net_t *net)
{
    return net->ss_on;
}
