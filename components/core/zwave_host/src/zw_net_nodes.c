/*
 * zw_net_nodes.c - the network's nodes: the node cache and its persistence, the check at every
 * link up (network.md §2), the interview (§5-6), sleeping devices, a device's own check-in and
 * supervision through zw_health (§7-8), removals and Device Reset Locally (§10). See zw_net.h.
 */
#include <string.h>

#include "zw_net_internal.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"

#define RETRY_MS 100u /* the security manager or the radio was busy: again this much later */

uint32_t zw_net_tag(unsigned what, uint32_t low)
{
    return NT_BASE | (uint32_t)what << 16 | (low & 0xFFFFu);
}

bool zw_net_is_own(uint32_t tag, unsigned what)
{
    return (tag & NT_MASK) == NT_BASE && ((tag >> 16) & 0xFu) == what;
}

zw_nid_width_t zw_net_nid(const zw_net_t *net)
{
    return net->host.facts.node_id_bits == 16u ? ZW_NID_16 : ZW_NID_8;
}

static bool link_ready(const zw_net_t *net)
{
    return zw_host_link(&net->host) == ZW_LINK_UP && net->checked;
}

void zw_net_emit(zw_net_t *net, zw_net_event_t *ev)
{
    ev->home_id = net->host.facts.home_id;
    if (ev->kind == ZW_NET_EV_NODE_GONE || ev->kind == ZW_NET_EV_NODE_REMOVED ||
        ev->kind == ZW_NET_EV_NODE_ADDED) {
        zw_net_signal_forget(net, ev->node); /* a number may be given to another device */
    }
    if (net->port.event != NULL) {
        net->port.event(net->port.ctx, ev);
    }
}

static void emit_node(zw_net_t *net, zw_net_event_kind_t kind, uint16_t node)
{
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.node = node;
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, node);
    ev.generation = rec != NULL ? (unsigned)rec->generation : 0u;
    zw_net_emit(net, &ev);
}

/* network.md §2: written when a device joins, is removed or forgotten, and when an interview
 * step completes - never because of a report. */
void zw_net_flush(zw_net_t *net)
{
    if (!net->store_due) {
        return;
    }
    net->store_due = false;
    net->stats.stores++;
    if (net->port.store_nodes != NULL) {
        net->port.store_nodes(net->port.ctx, &net->nodes, net->generation);
    }
}

/* ---- Keys (network.md §2, ZW-017) ------------------------------------------------------------ */

bool zw_net_fresh_keys(zw_net_t *net)
{
    const zw_crypto_port_t *c = &net->s2.crypto;
    zw_s2_keys_t k;
    if (c->random == NULL || !c->random(c->ctx, (uint8_t *)&k, sizeof k)) {
        zw_wipe(&k, sizeof k); /* whatever a failed draw left there (ZW01) */
        return false;
    }
    net->keys = k;
    zw_wipe(&k, sizeof k);
    net->keys_ready = true;
    net->keys_bound = false;
    return true;
}

/* The keys now belong to this network: the owner stores them with its home ID. */
void zw_net_bind_keys(zw_net_t *net, uint32_t home_id)
{
    net->keys_bound = true;
    net->keys_home_id = home_id;
    if (net->port.store_keys != NULL) {
        net->port.store_keys(net->port.ctx, home_id, &net->keys);
    }
}

/* ---- Registration: the security manager and zw_health ---------------------------------------- */

/* The record's CC lists as the security layer takes them: its node information's CCs (`nif`) and
 * its secure ones (`sec`), each ZW_NODE_CC_MAX long. */
static void record_ccs(const zw_node_record_t *rec, uint8_t *nif, size_t *nn, uint8_t *sec,
                       size_t *ns)
{
    *nn = 0u;
    *ns = 0u;
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].supported) {
            nif[(*nn)++] = rec->ccs[i].cc;
        }
        if (rec->ccs[i].secure) {
            sec[(*ns)++] = rec->ccs[i].cc;
        }
    }
}

static void s2_register(zw_net_t *net, const zw_node_record_t *rec)
{
    uint8_t nif[ZW_NODE_CC_MAX];
    uint8_t sec[ZW_NODE_CC_MAX];
    size_t nn = 0u;
    size_t ns = 0u;
    record_ccs(rec, nif, &nn, sec, &ns);
    if (rec->security_classes == ZW_NODE_SECURITY_UNKNOWN &&
        zw_s2_has_node(&net->s2, rec->node_id)) {
        return; /* registered by the owner itself (or being probed): left as it is */
    }
    const uint8_t granted = rec->security_classes == ZW_NODE_SECURITY_UNKNOWN
                                ? 0u
                                : (uint8_t)(rec->security_classes & ZW_S2_KEYS_S2);
    if (!zw_s2_add_node(&net->s2, rec->node_id, granted, nif, nn)) {
        return;
    }
    if (rec->security_classes == ZW_NODE_SECURITY_UNKNOWN) {
        /* its cache lost: its S2 is decrypted with whichever key fits, as Z-Wave JS does, until
         * the interview's probe decides (security.md §8) */
        (void)zw_s2_set_classes_unknown(&net->s2, rec->node_id, true);
    } else if (granted != 0u && rec->interview_step >= ZW_IV_STEP_SECURE_CCS) {
        (void)zw_s2_set_secure_ccs(&net->s2, rec->node_id, sec, ns);
    }
}

/* A device whose protocol info is known: supervised, its wake-ups handled (§7-8). */
static void health_register(zw_net_t *net, const zw_node_record_t *rec)
{
    if (!net->cfg.interview || !rec->has_protocol ||
        zw_iv_inputs(rec, 0u) == ZW_IV_INPUTS_NOT_USABLE) {
        return;
    }
    const bool listening = rec->is_listening || rec->is_flirs;
    const uint32_t interval = rec->has_wake_up ? rec->wake_up_interval_seconds : 0u;
    if (!zw_health_add(&net->health, rec->node_id, listening, interval)) {
        (void)zw_health_set_interval(&net->health, rec->node_id, interval);
    }
    zw_health_set_busy(&net->health, rec->node_id, zw_iv_asking(&net->iv, rec->node_id),
                       rec->node_id == net->hold_awake);
}

bool zw_net_sleeping(const zw_net_t *net, uint16_t node)
{
    bool listening = true;
    return zw_health_is_listening(&net->health, node, &listening) && !listening;
}

static void start_interview(zw_net_t *net, uint16_t node)
{
    if (net->cfg.interview && zw_iv_start(&net->iv, net->now, node)) {
        zw_health_set_busy(&net->health, node, true, node == net->hold_awake);
    }
}

/* ---- Forgetting (§2 missing, §10 removed) ---------------------------------------------------- */

static void checkin_drop(zw_net_t *net, uint16_t node);

static void drop_gone(zw_net_t *net, uint16_t node)
{
    for (size_t i = 0; i < net->gone_n; i++) {
        if (net->gone[i] == node) {
            memmove(&net->gone[i], &net->gone[i + 1u],
                    (net->gone_n - i - 1u) * sizeof net->gone[0]);
            net->gone_n--;
            return;
        }
    }
}

/* The node leaves the cache, the security manager, the interview and supervision; its zones
 * keep their bindings and show CHECK "sensor removed" (the owner, from the event). */
static void forget(zw_net_t *net, uint16_t node, zw_net_event_kind_t kind, zw_net_gone_t gone)
{
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.node = node;
    ev.gone = gone;
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, node);
    ev.generation = rec != NULL ? (unsigned)rec->generation : 0u;
    zw_iv_stop(&net->iv, node);
    (void)zw_health_remove(&net->health, node);
    zw_s2_remove_node(&net->s2, node);
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        if (net->qout[i].used && net->qout[i].node == node) {
            memset(&net->qout[i], 0, sizeof net->qout[i]);
        }
    }
    checkin_drop(net, node);
    if (zw_node_remove(&net->nodes, node)) {
        net->store_due = true;
    }
    drop_gone(net, node);
    zw_net_emit(net, &ev);
}

void zw_net_forget_all(zw_net_t *net)
{
    while (zw_node_count(&net->nodes) > 0u) {
        const zw_node_record_t *rec = zw_node_at_const(&net->nodes, 0u);
        forget(net, rec->node_id, ZW_NET_EV_NODE_GONE, ZW_NET_GONE_MISSING);
    }
    net->gone_n = 0u;
    net->store_due = true;
}

/* ---- The check at every link up (network.md §2 "At every start") ----------------------------- */

bool zw_net_generations_left(const zw_net_t *net)
{
    return net->generation < UINT16_MAX;
}

/* ZW-014: the next generation; the counter never wraps (0 is "never set", and a wrapped 1 could
 * be a removed device's whose zones still hold it - zones.md §1). Only while one is left. */
static uint16_t next_generation(zw_net_t *net)
{
    net->generation = (uint16_t)((unsigned)net->generation + 1u);
    return net->generation;
}

/* A node the cache lacks: a record, a new generation, classes unknown (the interview probes).
 * None once the generations are used up (ZW-014): a node without a record feeds nothing. */
static zw_node_record_t *new_record(zw_net_t *net, uint16_t node)
{
    if (!zw_net_generations_left(net)) {
        return NULL;
    }
    zw_node_record_t *rec = zw_node_add(&net->nodes, node);
    if (rec != NULL) {
        rec->generation = next_generation(net);
        rec->security_classes = ZW_NODE_SECURITY_UNKNOWN;
        net->store_due = true;
    }
    return rec;
}

/* §2, ZW-017: a first start's keys - drawn now if there are none (the link up could not, or a
 * stopped start's draw failed), and the security manager always started on them: the link up
 * keeps a manager already on this home and node ID, which may run another set (a network reset
 * whose HardReset failed drew a new one meanwhile), and a first start has no SPANs worth keeping
 * (its cache is forgotten). False when either failed (ZW01). */
static bool first_start_keys(zw_net_t *net, const zw_module_facts_t *f)
{
    if (!net->keys_ready && !zw_net_fresh_keys(net)) {
        zw_wipe(&net->keys, sizeof net->keys); /* not keys: never used as ones */
        return false;
    }
    return zw_s2_start(&net->s2, f->home_id, f->node_id, &net->keys);
}

/* ZW01: the first start stopped - nothing bound, stored or usable (no NETWORK: the radio is not
 * checked yet), the security manager on no network (not the old one's keys and home ID); the
 * check runs again ZW_NET_KEY_RETRY_MS from now - keys drawn then if the draw failed, the unbound
 * set kept for it if the manager's start failed. */
static void first_start_stopped(zw_net_t *net)
{
    zw_s2_stop(&net->s2);
    net->stats.s2_start_failures++;
    net->checked = false;
    net->radio = ZW_NET_RADIO_UNKNOWN;
    net->key_retry_at = net->now + ZW_NET_KEY_RETRY_MS;
}

static void reconcile(zw_net_t *net, const uint8_t list[ZW_NODE_LIST_BYTES])
{
    const zw_module_facts_t *f = zw_host_facts(&net->host);
    net->checked = true;
    net->stats.checks++;
    uint16_t ids[232];
    size_t n = zw_node_list_iterate(list, ids, 232u);
    n = n < 232u ? n : 232u;
    size_t k = 0u;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] != f->node_id) { /* the panel itself is not one of its devices */
            ids[k++] = ids[i];
        }
    }
    zw_reconcile_result_t r;
    zw_node_reconcile(f->home_id, ids, k, net->keys_bound, net->keys_home_id, &net->nodes, &r);
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_NETWORK;
    switch (r.reconcile_case) {
    case ZW_RECONCILE_FIRST_START:
        /* first start, or keys lost: this network's own keys from now on - only real ones, with
         * the security manager running on exactly them, are bound (ZW01) */
        if (!first_start_keys(net, f)) {
            first_start_stopped(net);
            return;
        }
        if (zw_node_count(&net->nodes) > 0u) {
            zw_net_forget_all(net); /* a cache without keys belongs to no network we can use */
        }
        zw_net_bind_keys(net, f->home_id);
        net->radio = ZW_NET_RADIO_OURS;
        ev.first_start = true;
        break;
    case ZW_RECONCILE_NORMAL:
        net->radio = ZW_NET_RADIO_OURS;
        break;
    case ZW_RECONCILE_REPLACED_OR_RESET:
        net->radio = ZW_NET_RADIO_REPLACED;
        ev.count = (uint16_t)zw_node_count(&net->nodes);
        break;
    default:
        net->radio = ZW_NET_RADIO_FOREIGN;
        break;
    }
    ev.radio = net->radio;
    zw_net_emit(net, &ev);
    if (net->radio != ZW_NET_RADIO_OURS) {
        return;
    }
    for (size_t i = 0; i < r.to_forget_count; i++) {
        forget(net, r.to_forget[i], ZW_NET_EV_NODE_GONE, ZW_NET_GONE_MISSING);
    }
    for (size_t i = 0; i < r.to_interview_count; i++) {
        if (new_record(net, r.to_interview[i]) != NULL) {
            emit_node(net, ZW_NET_EV_NODE_ADDED, r.to_interview[i]);
        }
    }
    for (size_t i = 0; i < zw_node_count(&net->nodes); i++) {
        const zw_node_record_t *rec = zw_node_at_const(&net->nodes, i);
        s2_register(net, rec);
        health_register(net, rec);
    }
    zw_health_restart(&net->health, net->now); /* §7-8: nothing heard before counts */
    zw_iv_link_up(&net->iv, net->now, f->node_id, zw_net_nid(net));
    for (size_t i = 0; i < zw_node_count(&net->nodes); i++) {
        start_interview(net, zw_node_at_const(&net->nodes, i)->node_id);
    }
}

static void check_done(void *ctx, uint32_t tag, const zw_tx_result_t *r);

static void submit_check(zw_net_t *net)
{
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_GET_SERIAL_API_INIT_DATA;
    req.expect = ZW_EXPECT_RESPONSE;
    req.prio = ZW_PRIO_NODE;
    req.done = check_done;
    req.ctx = net;
    req.tag = zw_net_tag(NT_CHECK, net->check_gen);
    if (zw_host_submit(&net->host, &req) != ZW_SUBMIT_OK) {
        net->check_tries = 3u; /* the facts' list will do */
        reconcile(net, net->host.facts.node_list);
    }
}

static void check_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    if (tag != zw_net_tag(NT_CHECK, net->check_gen) || net->checked ||
        r->status == ZW_TX_LINK_DOWN) {
        return;
    }
    zw_api_init_data_rsp_t init;
    const bool ok = r->status == ZW_TX_OK && r->response != NULL &&
                    zw_api_init_data_rsp_parse(r->response, r->response_len, &init);
    if (!ok && ++net->check_tries < 3u) {
        submit_check(net);
        return;
    }
    uint8_t list[ZW_NODE_LIST_BYTES];
    memcpy(list, ok ? init.node_list : net->host.facts.node_list, sizeof list);
    reconcile(net, list);
    zw_net_run_gone(net);
    zw_net_flush(net);
}

void zw_net_check(zw_net_t *net)
{
    net->checked = false;
    net->key_retry_at = 0u; /* this check is the retry a stopped first start waited for */
    net->check_tries = 0u;
    net->check_gen = (uint16_t)((unsigned)net->check_gen + 1u);
    submit_check(net);
}

void zw_net_link_lost(zw_net_t *net)
{
    net->checked = false;
    net->check_gen = (uint16_t)((unsigned)net->check_gen + 1u);
    zw_iv_link_down(&net->iv, net->now);
    memset(net->pings, 0, sizeof net->pings); /* a ping lost with the link says nothing */
    net->nif_node = 0u;
}

/* ---- The wake-up queue (network.md §7) ------------------------------------------------------- */

static void route_queue_end(zw_net_t *net, const zw_net_qout_t *e, zw_s2_result_t result,
                            bool delivered);
static void nif_done(void *ctx, uint32_t tag, const zw_tx_result_t *r);
static void checkin_went(zw_net_t *net, uint16_t node, uint32_t tag);
static void checkin_sent(zw_net_t *net, uint16_t node, uint32_t tag, zw_s2_result_t result,
                         bool requeued);

/* Sends a taken command. */
static void queue_go(zw_net_t *net, size_t idx)
{
    zw_net_qout_t *e = &net->qout[idx];
    e->retry = false;
    if (e->nif) {
        if (net->nif_node != 0u) {
            e->retry = true; /* one RequestNodeInfo at a time: ApplicationUpdate 81 names none */
            return;
        }
        uint8_t buf[4];
        const zw_api_request_node_info_req_t q = {e->node};
        const size_t n = zw_api_request_node_info_req_build(buf, sizeof buf, zw_net_nid(net), &q);
        zw_request_t req;
        memset(&req, 0, sizeof req);
        req.func = (uint8_t)ZW_FUNC_REQUEST_NODE_INFO;
        req.payload = buf;
        req.payload_len = n;
        req.expect = ZW_EXPECT_RESPONSE;
        req.prio = ZW_PRIO_INTERVIEW;
        req.done = nif_done;
        req.ctx = net;
        req.tag = zw_net_tag(NT_QUEUE, (uint32_t)idx);
        if (n == 0u || zw_host_submit(&net->host, &req) != ZW_SUBMIT_OK) {
            e->retry = true;
            return;
        }
        net->nif_node = e->node;
        net->nif_tag = e->tag;
        return;
    }
    const zw_s2_result_t r = zw_s2_send(&net->s2, net->now, e->node, (zw_sec_class_t)e->cls,
                                        e->cmd, e->len, zw_net_tag(NT_QUEUE, (uint32_t)idx));
    if (r == ZW_S2_BUSY) {
        e->retry = true;
    } else if (r != ZW_S2_OK) {
        /* it can never go (too long, no key): done with that, delivered at the radio's level */
        const zw_net_qout_t copy = *e;
        memset(e, 0, sizeof *e);
        route_queue_end(net, &copy, r, true);
    }
}

/* The node's next queued command, when it is awake and none is out: its origin's tag, or 0. */
static uint32_t pump(zw_net_t *net, uint16_t node)
{
    if (!link_ready(net)) {
        return 0u;
    }
    size_t free_idx = ZW_NET_QUEUE_OUT;
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        if (net->qout[i].used && net->qout[i].node == node) {
            return 0u; /* one at a time (§7) */
        }
        if (!net->qout[i].used && free_idx == ZW_NET_QUEUE_OUT) {
            free_idx = i;
        }
    }
    uint8_t data[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    size_t len = 0u;
    uint32_t kind = 0u;
    bool interview = false;
    if (free_idx == ZW_NET_QUEUE_OUT ||
        !zw_health_queue_take(&net->health, node, &kind, data, sizeof data, &len, &interview) ||
        len < 5u) {
        return 0u;
    }
    zw_net_qout_t *e = &net->qout[free_idx];
    memset(e, 0, sizeof *e);
    e->used = true;
    e->node = node;
    e->kind = kind;
    e->cls = data[0];
    e->nif = data[0] == QCLS_NIF;
    e->tag = (uint32_t)data[1] << 24 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 8 | (uint32_t)data[4];
    e->len = (uint8_t)(len - 5u);
    memcpy(e->cmd, &data[5], e->len);
    const uint32_t tag = e->tag;
    if (kind == KIND_IV) {
        zw_iv_went(&net->iv, net->now, tag);
    } else if (kind == KIND_CHECK_IN) {
        checkin_went(net, node, tag);
    }
    queue_go(net, free_idx);
    return tag;
}

/* A slot of the queue freed: nodes waiting for one go on. */
static void pump_all(zw_net_t *net)
{
    for (size_t i = 0; i < zw_node_count(&net->nodes); i++) {
        const uint16_t node = zw_node_at_const(&net->nodes, i)->node_id;
        if (zw_health_queue_len(&net->health, node) > 0u) {
            (void)pump(net, node);
        }
    }
}

/* A key of the owner's commands (zw_net_send): not the interview's, a check-in's or a reply's. */
static bool owner_kind(uint32_t kind)
{
    return kind != KIND_IV && kind != KIND_REREAD && kind != KIND_CHECK_IN &&
           (kind & 0xFFFF0000u) != KIND_REPLY;
}

zw_s2_result_t zw_net_queue_send(zw_net_t *net, uint16_t node, zw_sec_class_t cls,
                                 const uint8_t *cmd, size_t len, uint32_t tag, uint32_t kind,
                                 bool interview, bool *held)
{
    uint8_t data[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    *held = true;
    if (len + 5u > sizeof data) {
        return ZW_S2_TOO_LONG;
    }
    data[0] = (uint8_t)cls;
    data[1] = (uint8_t)(tag >> 24);
    data[2] = (uint8_t)(tag >> 16);
    data[3] = (uint8_t)(tag >> 8);
    data[4] = (uint8_t)tag;
    if (len > 0u) {
        memcpy(&data[5], cmd, len);
    }
    bool out = false; /* this kind's command is out (taken from the queue, its send not ended) */
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        const zw_net_qout_t *e = &net->qout[i];
        out = out || (e->used && e->node == node && e->kind == kind && !e->superseded);
    }
    /* ZW13: one still waiting is replaced before it ever went - its owner hears its end now (Z-Wave
     * JS settles a superseded command's caller too); zw_net's own kinds have no owner to tell */
    uint8_t old[ZW_HEALTH_QUEUE_ENTRY_BYTES];
    size_t old_len = 0u;
    const bool replaces = owner_kind(kind) && !out &&
                          zw_health_queue_peek(&net->health, node, kind, old, sizeof old, &old_len) &&
                          old_len >= 5u;
    const uint32_t old_tag = replaces ? (uint32_t)old[1] << 24 | (uint32_t)old[2] << 16 |
                                            (uint32_t)old[3] << 8 | (uint32_t)old[4]
                                      : 0u;
    if (replaces && net->telling_replaced) {
        return ZW_S2_BUSY; /* sent from a replaced one's CANCELLED: never a replacement in turn */
    }
    /* §7: a newer command of the same kind replaces an older one - also one already out (the
     * interview's next question can come before its last one's send ended): that one's end no
     * longer speaks for the queue */
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        zw_net_qout_t *e = &net->qout[i];
        if (e->used && e->node == node && e->kind == kind && !e->superseded) {
            e->superseded = true;
            (void)zw_health_queue_cancel(&net->health, node, kind);
        }
    }
    if (!zw_health_queue_put(&net->health, node, kind, interview, data, len + 5u)) {
        return ZW_S2_BUSY;
    }
    net->stats.queued++;
    bool behind = false; /* the device is awake: its previous command is still out */
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        behind = behind || (net->qout[i].used && net->qout[i].node == node);
    }
    *held = pump(net, node) != tag && !behind;
    if (replaces && old_tag != tag && net->port.send_done != NULL) {
        net->telling_replaced = true; /* what it sends from there replaces nothing */
        net->port.send_done(net->port.ctx, old_tag, ZW_S2_CANCELLED); /* never sent */
        net->telling_replaced = false;
    }
    return ZW_S2_OK;
}

static void health_actions(zw_net_t *net, const zw_health_action_t *a, size_t n);

static void route_queue_end(zw_net_t *net, const zw_net_qout_t *e, zw_s2_result_t result,
                            bool delivered)
{
    if (e->superseded) {
        /* replaced while it was out: the queue holds its successor; only the owner hears */
        if (e->kind != KIND_IV && e->kind != KIND_REREAD && e->kind != KIND_CHECK_IN &&
            net->port.send_done != NULL) {
            net->port.send_done(net->port.ctx, e->tag, result);
        }
        return;
    }
    zw_health_action_t acts[2];
    const size_t n = zw_health_sent(&net->health, e->node, e->kind, delivered, net->now, acts, 2u);
    bool requeued = !delivered && zw_net_sleeping(net, e->node);
    if (requeued && (e->kind & 0xFFFF0000u) == KIND_REPLY) {
        /* a reply the device did not hear is not sent at its next wake-up: it would mean nothing
         * by then - it ends here, and the owner hears so */
        (void)zw_health_queue_cancel(&net->health, e->node, e->kind);
        requeued = false;
    }
    if (e->kind == KIND_IV) {
        if (requeued) {
            zw_iv_asleep(&net->iv, net->now, e->node); /* it waits for the next wake-up */
        } else if (!e->nif) {
            (void)zw_iv_sent(&net->iv, net->now, e->tag, result);
        }
    } else if (e->kind == KIND_CHECK_IN) {
        checkin_sent(net, e->node, e->tag, result, requeued);
    } else if (e->kind != KIND_REREAD && !requeued && net->port.send_done != NULL) {
        net->port.send_done(net->port.ctx, e->tag, result); /* the owner's command */
    }
    health_actions(net, acts, n);
}

static void queue_done(zw_net_t *net, size_t idx, zw_s2_result_t result)
{
    if (idx >= ZW_NET_QUEUE_OUT || !net->qout[idx].used) {
        return;
    }
    const zw_net_qout_t e = net->qout[idx];
    memset(&net->qout[idx], 0, sizeof net->qout[idx]);
    /* not delivered = the radio got no ACK: a sleeping device is asleep again */
    route_queue_end(net, &e, result, result != ZW_S2_FAILED);
    (void)pump(net, e.node);
    pump_all(net);
}

/* A queued RequestNodeInfo's module response (01: sent, its answer is an ApplicationUpdate). */
static void queue_nif_response(zw_net_t *net, size_t idx, const zw_tx_result_t *r)
{
    zw_net_qout_t *e = &net->qout[idx];
    if (idx >= ZW_NET_QUEUE_OUT || !e->used || !e->nif) {
        return;
    }
    if (r->status == ZW_TX_OK && r->response != NULL && r->response_len > 0u &&
        r->response[0] != 0u) {
        e->answered = true;
        (void)zw_iv_sent(&net->iv, net->now, e->tag, ZW_S2_OK);
        return;
    }
    net->nif_node = 0u;
    queue_done(net, idx, ZW_S2_FAILED);
}

/* ---- Supervision's pings, No More Information, a late wake-up (network.md §7-8) -------------- */

static void ping_finish(zw_net_t *net, size_t idx, bool delivered)
{
    const uint16_t node = net->pings[idx].node;
    memset(&net->pings[idx], 0, sizeof net->pings[idx]);
    zw_health_action_t acts[2];
    const size_t n =
        zw_health_sent(&net->health, node, ZW_HEALTH_PING_TAG, delivered, net->now, acts, 2u);
    health_actions(net, acts, n);
}

static void ping_send(zw_net_t *net, size_t idx)
{
    static const uint8_t NOP[1] = {0x00u};
    net->pings[idx].out = false;
    /* stamped with the link's generation: a ping lost with the link is never taken for a new one */
    const zw_s2_result_t r =
        zw_s2_send(&net->s2, net->now, net->pings[idx].node, ZW_SEC_NONE, NOP, sizeof NOP,
                   zw_net_tag(NT_PING, (uint32_t)(net->check_gen & 0xFFu) << 8 | (uint32_t)idx));
    if (r == ZW_S2_OK) {
        net->pings[idx].out = true;
        net->pings[idx].attempts++;
        net->stats.pings++;
    } else if (r != ZW_S2_BUSY) {
        ping_finish(net, idx, false);
    }
}

static void ping_start(zw_net_t *net, uint16_t node)
{
    size_t idx = ZW_NET_PINGS;
    for (size_t i = 0; i < ZW_NET_PINGS; i++) {
        if (net->pings[i].node == node) {
            return;
        }
        if (net->pings[i].node == 0u && idx == ZW_NET_PINGS) {
            idx = i;
        }
    }
    if (idx == ZW_NET_PINGS) {
        return; /* all out: zw_health asks again at its next deadline */
    }
    net->pings[idx].node = node;
    net->pings[idx].attempts = 0u;
    ping_send(net, idx);
}

static void ping_done(zw_net_t *net, uint32_t tag, zw_s2_result_t result)
{
    const size_t idx = tag & 0xFFu;
    if (idx >= ZW_NET_PINGS || net->pings[idx].node == 0u || !net->pings[idx].out ||
        ((tag >> 8) & 0xFFu) != (net->check_gen & 0xFFu)) {
        return;
    }
    net->pings[idx].out = false;
    const bool delivered = result == ZW_S2_OK;
    if (!delivered && net->pings[idx].attempts < ZW_NET_PING_ATTEMPTS) {
        ping_send(net, idx); /* §8: each ping gets the normal 3 attempts - here, as zw_s2
                              * sends a No Operation once (zw_net.c s_attempts) */
        return;
    }
    ping_finish(net, idx, delivered);
}

static void no_more_info(zw_net_t *net, uint16_t node)
{
    static const uint8_t NMI[2] = {ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_NO_MORE_INFORMATION};
    if (zw_s2_send(&net->s2, net->now, node, zw_s2_class_for(&net->s2, node, ZW_CC_WAKE_UP), NMI,
                   sizeof NMI, zw_net_tag(NT_NMI, node)) == ZW_S2_OK) {
        net->stats.no_more_info++;
    }
}

static void reread_interval(zw_net_t *net, uint16_t node)
{
    uint8_t cmd[2];
    bool held = false;
    const size_t n = zw_cc_wake_up_interval_get_build(cmd, sizeof cmd);
    (void)zw_net_queue_send(net, node, zw_s2_class_for(&net->s2, node, ZW_CC_WAKE_UP), cmd, n, 0u,
                            KIND_REREAD, false, &held);
}

static void health_actions(zw_net_t *net, const zw_health_action_t *a, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (a[i].kind) {
        case ZW_HEALTH_ACTION_PING:
            ping_start(net, a[i].node_id);
            break;
        case ZW_HEALTH_ACTION_UNREACHABLE:
            emit_node(net, ZW_NET_EV_UNREACHABLE, a[i].node_id);
            break;
        case ZW_HEALTH_ACTION_REACHABLE:
            emit_node(net, ZW_NET_EV_REACHABLE, a[i].node_id);
            break;
        case ZW_HEALTH_ACTION_SEND_QUEUE:
            (void)pump(net, a[i].node_id);
            break;
        case ZW_HEALTH_ACTION_NO_MORE_INFO:
            no_more_info(net, a[i].node_id);
            break;
        case ZW_HEALTH_ACTION_REREAD_INTERVAL:
            reread_interval(net, a[i].node_id);
            break;
        default:
            break;
        }
    }
}

/* Health counts the device awake now (a sleeper during its wake-up, or a listening device). */
static bool node_counts_awake(const zw_net_t *net, uint16_t node)
{
    bool listening = false;
    if (zw_health_is_listening(&net->health, node, &listening) && listening) {
        return true;
    }
    for (size_t i = 0; i < net->health.node_count && i < ZW_HEALTH_NODES_MAX; i++) {
        if (net->health.nodes[i].node_id == node) {
            return net->health.nodes[i].awake;
        }
    }
    return false;
}

bool zw_net_counts_awake(const zw_net_t *net, uint16_t node)
{
    return node_counts_awake(net, node);
}

static zw_net_checkin_t *checkin_of(zw_net_t *net, uint16_t node);

/* What holds the device's No More Information back: its interview asking (not a re-read waiting
 * for its wake-up with no scratch record, BL-152), or its check-in Set whose word is awaited. */
static bool asking_or_checkin(zw_net_t *net, uint16_t node)
{
    const zw_net_checkin_t *c = checkin_of(net, node);
    return zw_iv_asking(&net->iv, node) || (c != NULL && (c->waiting || c->retry));
}

/* A node was heard from: proof of life (§8); a wake-up (§7) when `wake_up` - as Z-Wave JS marks a
 * sleeper awake (markAsAwake): its Wake Up Notification, its node information frame, or a Get
 * from it other than a Nonce Get (Node.ts). Any other frame, a report included, is no wake-up,
 * even with something waiting in its queue. */
static void heard_node(zw_net_t *net, uint16_t node, bool wake_up)
{
    if (!net->cfg.interview) {
        return;
    }
    zw_health_action_t acts[4];
    size_t n;
    if (wake_up) {
        n = zw_health_wakeup(&net->health, node, net->now, acts, 4u);
    } else {
        n = zw_health_heard(&net->health, node, net->now, acts, 4u);
    }
    health_actions(net, acts, n);
    if (wake_up || node_counts_awake(net, node)) {
        /* BL-152: an interview waiting for the wake-up goes on only then, as Z-Wave JS's
         * waitForWakeup ends only at markAsAwake: a report or an S2 Nonce Get from a sleeper
         * starts nothing. A re-read that could not take a scratch record keeps nothing held:
         * No More Information may follow */
        zw_iv_awake(&net->iv, net->now, node);
        if (zw_iv_running(&net->iv, node)) {
            zw_health_set_busy(&net->health, node, asking_or_checkin(net, node),
                               node == net->hold_awake);
        }
    }
    if (wake_up) {
        zw_net_routes_woke(net, node); /* §11: a sleeper waiting for its routes */
    }
}

/* ZW-009, §8: a frame accepted and kept by Security 2 or the inclusion - a Nonce Get or Nonce
 * Report, a key-exchange command, a Commands Supported Get - is heard from the node, as an accepted
 * frame is. A dropped frame never counts: the panel's last-heard time follows Z-Wave JS's
 * markAsAlive (after the discard checks), not its lastSeen (every frame that parses) - security.md
 * §8, BL-157. */
void zw_net_heard(zw_net_t *net, uint16_t node)
{
    heard_node(net, node, false);
}

void zw_net_woke(zw_net_t *net, uint16_t node)
{
    heard_node(net, node, true);
}

/* ---- The frames' ACKs ("every ACK", §8) ------------------------------------------------------ */

void zw_net_frame_sent(zw_net_t *net, uint32_t s2tag, uint16_t node)
{
    for (size_t i = 0; i < ZW_NET_FRAMES; i++) {
        if (net->frames[i].node == 0u) {
            net->frames[i].tag = s2tag;
            net->frames[i].node = node;
            return;
        }
    }
}

void zw_net_frame_done(zw_net_t *net, uint32_t s2tag, bool delivered)
{
    for (size_t i = 0; i < ZW_NET_FRAMES; i++) {
        if (net->frames[i].node != 0u && net->frames[i].tag == s2tag) {
            const uint16_t node = net->frames[i].node;
            net->frames[i].node = 0u;
            if (delivered && net->cfg.interview) {
                zw_health_action_t acts[2];
                const size_t n = zw_health_acked(&net->health, node, net->now, acts, 2u);
                health_actions(net, acts, n);
            }
            return;
        }
    }
}

/* ---- A device's own check-in (network.md §7 "per device on its page") ------------------------ */

static zw_net_checkin_t *checkin_of(zw_net_t *net, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_NET_CHECK_INS; i++) {
        if (net->checkins[i].node == node) {
            return &net->checkins[i];
        }
    }
    return NULL;
}

/* Forgotten with its device (removed, gone): nobody waits for it any more. */
static void checkin_drop(zw_net_t *net, uint16_t node)
{
    zw_net_checkin_t *c = checkin_of(net, node);
    if (c != NULL) {
        memset(c, 0, sizeof *c);
    }
}

/* How it ended. Taken: its record, the stored cache and supervision follow (§8: the device wakes
 * at its new interval now). Either way the owner hears it. */
static void checkin_end(zw_net_t *net, zw_net_checkin_t *c, bool took)
{
    const zw_net_checkin_t done = *c;
    memset(c, 0, sizeof *c);
    if (!done.out) {
        /* a Set it heard but whose ACK was lost is back in its queue: it goes no more */
        (void)zw_health_queue_cancel(&net->health, done.node, KIND_CHECK_IN);
    }
    if (done.waiting || done.retry) {
        /* No More Information may follow now */
        zw_health_set_busy(&net->health, done.node, zw_iv_asking(&net->iv, done.node),
                           done.node == net->hold_awake);
    }
    zw_node_record_t *rec = zw_node_find(&net->nodes, done.node);
    if (took && rec != NULL) {
        const uint8_t own = (uint8_t)net->iv.own_node;
        if (rec->wake_up_interval_seconds != done.seconds || rec->wake_up_notify_node_id != own) {
            rec->wake_up_interval_seconds = done.seconds;
            rec->wake_up_notify_node_id = own;
            net->store_due = true;
        }
        (void)zw_health_set_interval(&net->health, done.node, done.seconds);
        zw_net_flush(net);
    }
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_CHECK_IN;
    ev.node = done.node;
    ev.generation = rec != NULL ? (unsigned)rec->generation : 0u;
    ev.check_in_s = done.seconds;
    ev.took = took;
    zw_net_emit(net, &ev);
}

/* Its Set left the queue (the device is awake). */
static void checkin_went(zw_net_t *net, uint16_t node, uint32_t tag)
{
    zw_net_checkin_t *c = checkin_of(net, node);
    if (c != NULL && c->tag == tag) {
        c->went = true;
        c->out = true;
        c->sent_at = net->now;
    }
}

/* Its Set's end: not heard - it waits for the next wake-up (§7); delivered - with Supervision its
 * Report is awaited 1 s + the round trip (serial-api.md 6.12); without, that is the device's word,
 * unless it lists Application Status (command-classes.md §4.25): then its word is awaited as long,
 * none meaning taken; No More Information held while awaited; anything else ends it. */
static void checkin_sent(zw_net_t *net, uint16_t node, uint32_t tag, zw_s2_result_t result,
                         bool requeued)
{
    zw_net_checkin_t *c = checkin_of(net, node);
    if (c == NULL || c->tag != tag) {
        /* it ended already (its Report came first), or a newer one took its place */
        if (c == NULL && requeued) {
            /* its ACK was lost after the device answered: the queue must not send it again */
            (void)zw_health_queue_cancel(&net->health, node, KIND_CHECK_IN);
        }
        return;
    }
    c->out = false;
    if (requeued) {
        return;
    }
    if (result != ZW_S2_OK) {
        checkin_end(net, c, false);
        return;
    }
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, node);
    if (!c->supervised && (rec == NULL || !zw_iv_has_cc(rec, ZW_CC_APPLICATION_STATUS))) {
        checkin_end(net, c, true);
        return;
    }
    c->waiting = true;
    c->deadline = net->now + ZW_IV_REPORT_MS + (net->now - c->sent_at);
    zw_health_set_busy(&net->health, node, true, node == net->hold_awake);
}

/* A Supervision Report of its session: FF done, 01 working and 00 "no support" done too, 02 failed
 * (command-classes.md §4.14, the interview's rule). */
static void checkin_report(zw_net_t *net, const zw_s2_rx_t *rx)
{
    zw_net_checkin_t *c = checkin_of(net, rx->node);
    zw_cc_supervision_report_t sr;
    if (c == NULL || !c->supervised || !c->went ||
        !zw_cc_supervision_report_parse(rx->command, rx->len, &sr) || sr.session_id != c->session) {
        return;
    }
    /* Supervision CC (SDS13782): the Get never asked for status updates, so this is the node's
     * one Report; WORKING (with a duration) means accepted and under way, as for the interview. */
    checkin_end(net, c, sr.status != ZW_SUPERVISION_STATUS_FAIL);
}

/* command-classes.md §4.25: Application Status from the device itself answering its Set (out, or
 * its word awaited): Rejected Request ends it, not taken - never taken for silence or success;
 * Busy sends it again after the device's wait time (1 s when it names none, at most
 * ZW_IV_BUSY_MAX_S), No More Information held meanwhile, the ZW_IV_TRIES-th Busy ending it. */
static void checkin_status(zw_net_t *net, const zw_s2_rx_t *rx)
{
    zw_net_checkin_t *c = checkin_of(net, rx->node);
    if (c == NULL || !c->went || !(c->out || c->waiting) || rx->len < 2u ||
        rx->command[0] != (uint8_t)ZW_CC_APPLICATION_STATUS ||
        (rx->multi_channel && rx->source_endpoint != 0u)) {
        return;
    }
    zw_cc_application_rejected_request_t rejected;
    zw_cc_application_busy_t busy;
    if (zw_cc_application_rejected_request_parse(rx->command, rx->len, &rejected)) {
        checkin_end(net, c, false);
        return;
    }
    if (!zw_cc_application_busy_parse(rx->command, rx->len, &busy)) {
        return;
    }
    c->busy = (uint8_t)((unsigned)c->busy + 1u);
    if (c->busy >= ZW_IV_TRIES) {
        checkin_end(net, c, false);
        return;
    }
    uint64_t wait = ZW_IV_BUSY_MS;
    if (busy.status == ZW_APPLICATION_BUSY_WAIT && busy.wait_s != 0u) {
        const unsigned w = busy.wait_s < ZW_IV_BUSY_MAX_S ? (unsigned)busy.wait_s : ZW_IV_BUSY_MAX_S;
        wait = (uint64_t)w * 1000u;
    }
    c->tag = 0u; /* the Set out, still to end, speaks for it no more */
    c->out = false;
    c->waiting = false;
    c->retry = true;
    c->retry_at = net->now + wait;
    zw_health_set_busy(&net->health, c->node, true, c->node == net->hold_awake);
}

static zw_s2_result_t checkin_queue(zw_net_t *net, zw_net_checkin_t *c);

/* No Report in time: not taken (without Supervision, no word against it: taken); a Set the
 * device was busy for goes again. */
static void checkin_poll(zw_net_t *net)
{
    for (size_t i = 0; i < ZW_NET_CHECK_INS; i++) {
        zw_net_checkin_t *c = &net->checkins[i];
        if (c->node != 0u && c->waiting && net->now >= c->deadline) {
            checkin_end(net, c, !c->supervised);
        } else if (c->node != 0u && c->retry && net->now >= c->retry_at) {
            c->retry = false;
            zw_health_set_busy(&net->health, c->node, zw_iv_asking(&net->iv, c->node),
                               c->node == net->hold_awake);
            if (checkin_queue(net, c) != ZW_S2_OK) {
                checkin_end(net, c, false);
            }
        }
    }
}

static uint64_t checkin_deadline(const zw_net_t *net)
{
    uint64_t t = UINT64_MAX;
    for (size_t i = 0; i < ZW_NET_CHECK_INS; i++) {
        const zw_net_checkin_t *c = &net->checkins[i];
        if (c->node != 0u && c->waiting && c->deadline < t) {
            t = c->deadline;
        }
        if (c->node != 0u && c->retry && c->retry_at < t) {
            t = c->retry_at;
        }
    }
    return t;
}

/* command-classes.md §4.14: a device's Supervision sessions count 1..63 from a random start. */
static uint8_t checkin_session_start(const zw_net_t *net)
{
    const zw_crypto_port_t *cp = &net->s2.crypto;
    uint8_t r = 0u;
    if (cp->random == NULL || !cp->random(cp->ctx, &r, 1u)) {
        r = 0u;
    }
    return (uint8_t)((unsigned)r % 63u);
}

/* Its Wake Up Interval Set (`seconds`, the panel as destination) into the device's queue, at §8's
 * class for Wake Up, wrapped in a Supervision Get of a new session when it is supervised; §7: it
 * replaces the device's older one there - also one already out. */
static zw_s2_result_t checkin_queue(zw_net_t *net, zw_net_checkin_t *c)
{
    const zw_cc_wake_up_interval_set_t set = {c->seconds, (uint8_t)net->iv.own_node};
    uint8_t plain[8];
    uint8_t wrapped[16];
    const uint8_t *cmd = plain;
    size_t n = zw_cc_wake_up_interval_set_build(plain, sizeof plain, &set);
    if (c->supervised) {
        c->session = (uint8_t)((unsigned)c->session % 63u + 1u);
        n = zw_cc_wrap_supervision(wrapped, sizeof wrapped, c->session, false, plain, n);
        cmd = wrapped;
    }
    net->checkin_stamp = (uint16_t)((unsigned)net->checkin_stamp + 1u);
    c->tag = zw_net_tag(NT_CHECK_IN, net->checkin_stamp);
    c->went = false;
    c->out = false;
    c->waiting = false;
    c->deadline = 0u;
    bool held = false;
    return n > 0u ? zw_net_queue_send(net, c->node, zw_s2_class_for(&net->s2, c->node, ZW_CC_WAKE_UP),
                                      cmd, n, c->tag, KIND_CHECK_IN, false, &held)
                  : ZW_S2_INVALID;
}

zw_net_result_t zw_net_set_check_in(zw_net_t *net, uint64_t now, bool allowed, uint16_t node,
                                    uint32_t seconds)
{
    net->now = now;
    if (!allowed) {
        return ZW_NET_NOT_ALLOWED;
    }
    if (!link_ready(net)) {
        return ZW_NET_LINK_DOWN;
    }
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, node);
    bool listening = true;
    if (rec == NULL || !zw_iv_sleeping(rec) || !rec->has_wake_up ||
        rec->interview_step < ZW_IV_STEP_DONE || zw_iv_setting_up(&net->iv, node) ||
        rec->wake_up_interval_seconds == ZW_HEALTH_INTERVAL_NEVER ||
        !zw_health_is_listening(&net->health, node, &listening) || listening ||
        seconds < ZW_NET_CHECK_IN_MIN_S || seconds > ZW_NET_CHECK_IN_MAX_S) {
        return ZW_NET_INVALID; /* not a sleeping device of ours, ready, that checks in */
    }
    zw_net_checkin_t *c = checkin_of(net, node);
    if (c == NULL) {
        for (size_t i = 0; i < ZW_NET_CHECK_INS && c == NULL; i++) {
            c = net->checkins[i].node == 0u ? &net->checkins[i] : NULL;
        }
        if (c == NULL) {
            return ZW_NET_BUSY; /* never: one a device the cache holds */
        }
        memset(c, 0, sizeof *c);
        c->node = node;
        c->session = checkin_session_start(net);
    } else if (c->waiting || c->retry) {
        /* the older one's word no longer matters */
        zw_health_set_busy(&net->health, node, zw_iv_asking(&net->iv, node),
                           node == net->hold_awake);
    }
    /* network.md §7: within its limits, the panel as destination; supervised as the interview's
     * Sets are, at §8's class for Wake Up (its highest) */
    c->seconds = zw_health_choose_interval(seconds, rec->wake_up_min_seconds,
                                           rec->wake_up_max_seconds, rec->wake_up_step_seconds);
    c->supervised = zw_iv_supervised(&net->iv, rec);
    c->retry = false;
    c->busy = 0u;
    if (checkin_queue(net, c) != ZW_S2_OK) {
        memset(c, 0, sizeof *c);
        return ZW_NET_BUSY; /* its queue is full */
    }
    zw_net_flush(net);
    return ZW_NET_STARTED;
}

/* ---- What nodes say -------------------------------------------------------------------------- */

static bool is(const zw_s2_rx_t *rx, uint8_t cc, uint8_t cmd)
{
    return rx->len >= 2u && rx->command[0] == cc && rx->command[1] == cmd;
}

/* network.md §10: it left by itself - gone now, removed as failed when nothing else runs and
 * ZW_NET_GONE_WAIT_MS have passed (Z-Wave JS: a device still finishing its notification would
 * answer the first ping, and its removal would be refused). */
static void reset_locally(zw_net_t *net, uint16_t node)
{
    for (size_t i = 0; i < net->gone_n; i++) {
        if (net->gone[i] == node) {
            return;
        }
    }
    if (net->gone_n >= ZW_NET_GONE) {
        return;
    }
    net->gone[net->gone_n++] = node;
    net->gone_after = net->now + ZW_NET_GONE_WAIT_MS;
    zw_iv_stop(&net->iv, node);
    (void)zw_health_remove(&net->health, node); /* not supervised: it is gone */
    checkin_drop(net, node);
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_NODE_GONE;
    ev.node = node;
    ev.gone = ZW_NET_GONE_RESET_LOCALLY;
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, node);
    ev.generation = rec != NULL ? (unsigned)rec->generation : 0u;
    zw_net_emit(net, &ev);
    zw_net_run_gone(net);
}

void zw_net_gone_quietly(zw_net_t *net, uint16_t node)
{
    for (size_t i = 0; i < net->gone_n; i++) {
        if (net->gone[i] == node) {
            return;
        }
    }
    if (net->gone_n >= ZW_NET_GONE) {
        return;
    }
    net->gone[net->gone_n++] = node;
    zw_iv_stop(&net->iv, node);
    (void)zw_health_remove(&net->health, node);
    checkin_drop(net, node);
}

void zw_net_run_gone(zw_net_t *net)
{
    if (net->op != OP_NONE || net->gone_n == 0u || !link_ready(net) ||
        net->now < net->gone_after || net->gone_held) {
        return; /* gone_held (ZW-014): it stays in the module, and so in the cache, as gone */
    }
    zw_net_smart_pause(net);
    if (zw_remove_reset_locally(&net->rm, net->now, net->gone[0], zw_net_nid(net)) ==
        ZW_REMOVE_STARTED) {
        net->op = OP_REMOVE;
    } else {
        drop_gone(net, net->gone[0]);
    }
}

/* ZW-009, Z-Wave JS Driver.ts shouldDiscardCC (`node.getEndpoint(i) ?? node`): whether the end
 * point supports `cc` - or the device itself for an end point the device does not have or has not
 * described yet, as there. Where the record cannot hold an end point's list (an end point at
 * ZW_NODE_ENDPOINTS_MAX or past it, or one whose ZW_NODE_ENDPOINT_CC_MAX list is full without the
 * CC) the device's list decides too, where Z-Wave JS uses the end point's full list; a device CC
 * past ZW_NODE_CC_MAX is not recorded, so its reports are discarded; controlled CCs are not
 * recorded (all in security.md "Where the panel still differs": the stored record's capacity). */
static bool endpoint_or_node_has_cc(const zw_node_record_t *rec, uint8_t endpoint, uint8_t cc)
{
    if (endpoint == 0u) {
        return zw_iv_endpoint_has_cc(rec, 0u, cc);
    }
    if (endpoint < ZW_NODE_ENDPOINTS_MAX && rec->endpoints[endpoint].has_class) {
        const zw_node_endpoint_t *e = &rec->endpoints[endpoint];
        if (zw_iv_endpoint_has_cc(rec, endpoint, cc)) {
            return true;
        }
        if (e->cc_count < ZW_NODE_ENDPOINT_CC_MAX) {
            return false; /* its whole list is known, without the CC */
        }
    }
    return zw_iv_endpoint_has_cc(rec, 0u, cc);
}

void zw_net_received(zw_net_t *net, const zw_s2_rx_t *rx)
{
    const bool known = zw_node_find_const(&net->nodes, rx->node) != NULL;
    const zw_node_record_t *crec = zw_node_find_const(&net->nodes, rx->node);
    if (crec != NULL && rx->len >= 1u && !rx->multi_command &&
        (rx->command[0] == (uint8_t)ZW_CC_MULTILEVEL_SENSOR ||
         rx->command[0] == 0x32u) &&
        !endpoint_or_node_has_cc(crec, rx->multi_channel ? rx->source_endpoint : 0u,
                                 rx->command[0])) {
        /* ZW-009, as Z-Wave JS Driver.ts shouldDiscardCC (zwave-js issue 5510): a Meter or
         * Multilevel Sensor command from a device (or end point) that does not support that CC is
         * discarded - no zone, no proof of life. Like shouldDiscardCC, it does not look inside a
         * Multi Command (a multi-encapsulating CC) */
        net->stats.unsupported_cc++;
        return;
    }
    /* §7 (Z-Wave JS Node.ts): a Wake Up Notification, or a Get from a sleeper other than
     * Security's Nonce Get (Security 2's never reaches here), marks it awake */
    const bool get_wakes = zw_net_sleeping(net, rx->node) && zw_s2_command_is_get(rx->command,
                                                                                   rx->len) &&
                           !is(rx, ZW_CC_SECURITY_0, 0x40u);
    const bool wake_up = is(rx, ZW_CC_WAKE_UP, ZW_CMD_WAKE_UP_NOTIFICATION) || get_wakes;
    heard_node(net, rx->node, wake_up); /* ZW-009, §8: every accepted frame (markAsAlive) */
    zw_iv_received(&net->iv, net->now, rx);
    checkin_report(net, rx);
    checkin_status(net, rx);
    if (known && is(rx, ZW_CC_DEVICE_RESET_LOCALLY, ZW_CMD_DEVICE_RESET_LOCALLY_NOTIFICATION)) {
        reset_locally(net, rx->node);
    }
    zw_node_record_t *rec = zw_node_find(&net->nodes, rx->node);
    zw_cc_wake_up_interval_report_t w;
    if (rec != NULL && rec->has_wake_up && rx->source_endpoint == 0u &&
        zw_cc_wake_up_interval_report_parse(rx->command, rx->len, &w)) {
        /* §7: a late wake-up's interval, read again (or the interview's): kept when it changed */
        const bool changed = rec->wake_up_interval_seconds != w.interval_seconds;
        if (changed) {
            rec->wake_up_interval_seconds = w.interval_seconds;
            net->store_due = true;
        }
        rec->wake_up_notify_node_id = w.node_id;
        /* BL-152: a re-read under way keeps it too: READY does not put back an older one */
        zw_iv_wake_interval_heard(&net->iv, rx->node, w.interval_seconds, w.node_id);
        (void)zw_health_set_interval(&net->health, rx->node, w.interval_seconds);
        if (changed && !zw_iv_running(&net->iv, rx->node)) {
            /* §8: its zones' grace and supervision follow now, not at the next restart (ZW-012,
             * ZW-015: 0 = not supervised). The interview's own read reaches them by NODE_READY */
            zw_net_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = ZW_NET_EV_CHECK_IN;
            ev.node = rx->node;
            ev.generation = rec->generation;
            ev.check_in_s = w.interval_seconds;
            ev.took = true;
            zw_net_emit(net, &ev);
        }
    }
    if (net->port.received != NULL) {
        net->port.received(net->port.ctx, rx);
    }
}

bool zw_net_app_update(zw_net_t *net, const zw_frame_t *frame)
{
    zw_api_app_update_t u;
    if (!zw_api_app_update_parse(frame->payload, frame->payload_len, &u)) {
        return false;
    }
    if (u.type == ZW_APP_UPDATE_SMARTSTART_CLASSIC ||
        u.type == ZW_APP_UPDATE_SMARTSTART_LONG_RANGE) {
        zw_net_smart_request(net, &u); /* §4.2 */
        return true;
    }
    if (u.type == ZW_APP_UPDATE_NODE_ADDED_BY_OTHER ||
        u.type == ZW_APP_UPDATE_NODE_REMOVED_BY_OTHER) {
        /* serial-api.md §6.13: the panel is the only controller - nothing changes for it; the
         * owner gets the frame to log */
        net->stats.other_controller++;
        return false;
    }
    if (u.type == ZW_APP_UPDATE_NODE_INFO_RECEIVED) {
        zw_api_node_update_t nu;
        if (!zw_api_node_update_parse(u.data, u.data_len, zw_net_nid(net), &nu)) {
            return false;
        }
        if (net->nif_node == nu.node_id) {
            net->nif_node = 0u;
        }
        for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
            zw_net_qout_t *e = &net->qout[i];
            if (e->used && e->nif && e->node == nu.node_id) {
                const zw_net_qout_t copy = *e;
                memset(e, 0, sizeof *e);
                route_queue_end(net, &copy, ZW_S2_OK, true);
            }
        }
        /* ZW-009: what it supports decides which sends go encrypted (Z-Wave JS isCCSecure asks
         * supportsCC). As Z-Wave JS Node.ts updateNodeInfo, a NIF's CCs count only before the
         * interview's node-information step is done (interviewStage < NodeInfo), and are added,
         * never removed; after that a NIF changes no CC - it only marks the node awake */
        const zw_node_record_t *nrec = zw_node_find_const(&net->nodes, nu.node_id);
        if (nrec != NULL && nrec->interview_step < (uint8_t)ZW_IV_STEP_NODE_INFO) {
            (void)zw_s2_add_nif_ccs(&net->s2, nu.node_id, nu.cc_list, nu.cc_list_len);
        }
        zw_iv_node_info(&net->iv, net->now, nu.node_id, nu.cc_list, nu.cc_list_len);
        heard_node(net, nu.node_id, true); /* §7: its node information: it listens now */
        return false; /* the owner sees it too */
    }
    if (u.type == ZW_APP_UPDATE_NODE_INFO_REQUEST_FAILED) {
        const uint16_t node = net->nif_node;
        net->nif_node = 0u;
        for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
            if (net->qout[i].used && net->qout[i].nif && net->qout[i].answered) {
                queue_done(net, i, ZW_S2_FAILED); /* asleep: back in its queue */
                return true;
            }
        }
        if (node != 0u) {
            zw_iv_node_info_failed(&net->iv, net->now, node);
        }
        return true;
    }
    return false;
}

/* ---- zw_net's own sends' ends ---------------------------------------------------------------- */

bool zw_net_own_send_done(zw_net_t *net, uint32_t tag, zw_s2_result_t result)
{
    if (zw_iv_is_tag(tag)) {
        (void)zw_iv_sent(&net->iv, net->now, tag, result);
        return true;
    }
    if (zw_remove_is_tag(tag)) {
        zw_remove_ping_done(&net->rm, net->now, tag, result == ZW_S2_OK);
        return true;
    }
    if (zw_routes_is_tag(tag)) {
        zw_routes_ping_done(&net->routes, net->now, tag, result == ZW_S2_OK);
        return true;
    }
    if (zw_net_is_own(tag, NT_QUEUE)) {
        queue_done(net, tag & 0xFFFFu, result);
        return true;
    }
    if (zw_net_is_own(tag, NT_PING)) {
        ping_done(net, tag, result);
        return true;
    }
    /* No More Information and the rest: nobody waits for their end */
    return (tag & ZW_NET_OWN_TAGS) == ZW_NET_OWN_TAGS;
}

/* ---- The interview's port -------------------------------------------------------------------- */

static zw_s2_result_t iv_send(void *ctx, uint16_t node, zw_sec_class_t cls, const uint8_t *cmd,
                              size_t len, uint32_t tag, bool *held)
{
    zw_net_t *net = ctx;
    if (zw_net_sleeping(net, node)) {
        return zw_net_queue_send(net, node, cls, cmd, len, tag, KIND_IV, true, held);
    }
    *held = false;
    return zw_s2_send(&net->s2, net->now, node, cls, cmd, len, tag);
}

static void nif_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    if (zw_net_is_own(tag, NT_QUEUE)) {
        queue_nif_response(net, tag & 0xFFFFu, r);
        return;
    }
    if (tag != zw_net_tag(NT_NIF, net->stamp) || net->nif_node == 0u) {
        return;
    }
    const bool sent = r->status == ZW_TX_OK && r->response != NULL && r->response_len > 0u &&
                      r->response[0] != 0u;
    if (!sent) {
        net->nif_node = 0u;
    }
    (void)zw_iv_sent(&net->iv, net->now, net->nif_tag, sent ? ZW_S2_OK : ZW_S2_FAILED);
}

static bool iv_node_info(void *ctx, uint16_t node, uint32_t tag, bool *held)
{
    zw_net_t *net = ctx;
    if (zw_net_sleeping(net, node)) {
        /* §5 step 3: a sleeping device at its next wake-up */
        uint8_t none[1];
        return zw_net_queue_send(net, node, (zw_sec_class_t)QCLS_NIF, none, 0u, tag, KIND_IV,
                                 true, held) == ZW_S2_OK;
    }
    *held = false;
    if (net->nif_node != 0u) {
        return false; /* one at a time */
    }
    uint8_t buf[4];
    const zw_api_request_node_info_req_t q = {node};
    const size_t n = zw_api_request_node_info_req_build(buf, sizeof buf, zw_net_nid(net), &q);
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_REQUEST_NODE_INFO;
    req.payload = buf;
    req.payload_len = n;
    req.expect = ZW_EXPECT_RESPONSE;
    req.prio = ZW_PRIO_INTERVIEW;
    req.done = nif_done;
    req.ctx = net;
    net->stamp = (uint16_t)((unsigned)net->stamp + 1u);
    req.tag = zw_net_tag(NT_NIF, net->stamp);
    if (n == 0u || zw_host_submit(&net->host, &req) != ZW_SUBMIT_OK) {
        return false;
    }
    net->nif_node = node;
    net->nif_tag = tag;
    return true;
}

static zw_submit_t iv_submit(void *ctx, const zw_request_t *req)
{
    zw_net_t *net = ctx;
    return zw_host_submit(&net->host, req);
}

static void iv_changed(void *ctx)
{
    zw_net_t *net = ctx;
    net->store_due = true;
}

static void iv_event(void *ctx, const zw_iv_event_t *e)
{
    zw_net_t *net = ctx;
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.node = e->node;
    ev.step = e->step;
    const zw_node_record_t *rec = zw_node_find_const(&net->nodes, e->node);
    ev.generation = rec != NULL ? (unsigned)rec->generation : 0u;
    switch (e->kind) {
    case ZW_IV_EV_PROTOCOL:
        if (rec != NULL) {
            health_register(net, rec);
            if (e->node == net->joined && net->cfg.interview &&
                !zw_iv_refreshing(&net->iv, e->node)) {
                /* not a re-read's step 1: "just joined" is its inclusion's word only */
                zw_health_action_t acts[4]; /* it just joined: it is awake */
                const size_t n = zw_health_wakeup(&net->health, e->node, net->now, acts, 4u);
                health_actions(net, acts, n);
            }
        }
        return;
    case ZW_IV_EV_READY:
        ev.kind = ZW_NET_EV_NODE_READY;
        ev.inputs = rec != NULL ? zw_iv_inputs(rec, 0u) : ZW_IV_INPUTS_NOT_READY;
        if (rec != NULL) {
            health_register(net, rec); /* its interval is known now */
            if (zw_s2_has_node(&net->s2, e->node)) {
                /* BL-152: the reading's node information and secure CCs reach the security layer,
                 * replacing the old reading's (Z-Wave JS refreshInfo forgets the CCs, the new
                 * interview adds them again); its classes stay. The secure list is the committed
                 * record's, taken as s2_register takes it at a start - a re-read whose step 4 went
                 * unanswered leaves none, at once, not only after the next start. A first
                 * interview's are the same lists (step 3 and take_secure_list) */
                uint8_t nif[ZW_NODE_CC_MAX];
                uint8_t sec[ZW_NODE_CC_MAX];
                size_t nn = 0u;
                size_t ns = 0u;
                record_ccs(rec, nif, &nn, sec, &ns);
                (void)zw_s2_set_nif_ccs(&net->s2, e->node, nif, nn);
                if (rec->security_classes != ZW_NODE_SECURITY_UNKNOWN &&
                    (rec->security_classes & ZW_S2_KEYS_S2) != 0u) {
                    /* an empty list where s2_register sets none: the same class as after a start */
                    const bool listed = rec->interview_step >= ZW_IV_STEP_SECURE_CCS;
                    (void)zw_s2_set_secure_ccs(&net->s2, e->node, sec, listed ? ns : 0u);
                }
            }
        }
        break;
    case ZW_IV_EV_WAKE_NEEDED: {
        ev.kind = ZW_NET_EV_WAKE_NEEDED;
        /* BL-152: a re-read that gave its scratch record back for the next wake-up keeps the
         * device awake no longer: No More Information may follow */
        zw_health_set_busy(&net->health, e->node, asking_or_checkin(net, e->node),
                           e->node == net->hold_awake);
        break;
    }
    case ZW_IV_EV_STALLED:
        ev.kind = ZW_NET_EV_STALLED;
        break;
    case ZW_IV_EV_NOT_USABLE:
        ev.kind = ZW_NET_EV_NOT_USABLE;
        break;
    case ZW_IV_EV_RESTARTED: {
        /* BL-152: its question held for the device's wake-up is void; one that waits for the
         * wake-up again keeps nothing held */
        (void)zw_health_queue_cancel(&net->health, e->node, KIND_IV);
        zw_health_set_busy(&net->health, e->node, asking_or_checkin(net, e->node),
                           e->node == net->hold_awake);
        return;
    }
    case ZW_IV_EV_ENDED:
    default: {
        /* a check-in Set whose word is still awaited keeps No More Information held (it may run
         * beside the Alarm Sensor catch-up, BL-130) */
        const zw_net_checkin_t *c = checkin_of(net, e->node);
        zw_health_set_busy(&net->health, e->node, c != NULL && (c->waiting || c->retry),
                           e->node == net->hold_awake);
        return;
    }
    }
    zw_net_emit(net, &ev);
}

static bool iv_quirk(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out)
{
    zw_net_t *net = ctx;
    return net->port.quirk != NULL && net->port.quirk(net->port.ctx, rec, out);
}

/* §7: the installer's check-in (per device, else the config's) within the device's limits. */
static uint32_t iv_wake_interval(void *ctx, const zw_node_record_t *rec)
{
    zw_net_t *net = ctx;
    uint32_t want = net->port.check_in != NULL ? net->port.check_in(net->port.ctx, rec) : 0u;
    if (want == 0u) {
        want = net->cfg.check_in_s != 0u ? net->cfg.check_in_s : ZW_NET_CHECK_IN_S;
    }
    return zw_health_choose_interval(want, rec->wake_up_min_seconds, rec->wake_up_max_seconds,
                                     rec->wake_up_step_seconds);
}

/* ---- Removals' port -------------------------------------------------------------------------- */

static zw_submit_t rm_submit(void *ctx, const zw_request_t *req)
{
    zw_net_t *net = ctx;
    return zw_host_submit(&net->host, req);
}

static bool rm_ping(void *ctx, uint16_t node, uint32_t tag)
{
    zw_net_t *net = ctx;
    static const uint8_t NOP[1] = {0x00u};
    return zw_s2_send(&net->s2, net->now, node, ZW_SEC_NONE, NOP, sizeof NOP, tag) == ZW_S2_OK;
}

static void rm_event(void *ctx, const zw_remove_event_t *e)
{
    zw_net_t *net = ctx;
    if (e->kind == ZW_REMOVE_EV_DONE) {
        net->op = OP_NONE;
        if (e->result == ZW_REMOVE_DONE && e->node != 0u &&
            zw_node_find_const(&net->nodes, e->node) != NULL) {
            forget(net, e->node, ZW_NET_EV_NODE_REMOVED, ZW_NET_GONE_MISSING);
        }
        drop_gone(net, e->node); /* a reset-locally device is not tried again and again */
    }
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_REMOVE;
    ev.node = e->node;
    ev.remove = *e;
    zw_net_emit(net, &ev);
    if (e->kind == ZW_REMOVE_EV_DONE) {
        zw_net_run_gone(net);
    }
}

/* ---- The inclusion's news (network.md §4) ---------------------------------------------------- */

void zw_net_incl_event(zw_net_t *net, const zw_incl_event_t *ev)
{
    if (ev->kind == ZW_INCL_EV_JOINED && ev->replaced) {
        /* §10: a new device in a failed one's place - its record starts again, its generation
         * stays (its zones follow it); nothing of the old device is waited for any more */
        zw_node_record_t *old = zw_node_find(&net->nodes, ev->node);
        if (old != NULL) {
            const uint16_t generation = old->generation;
            zw_iv_stop(&net->iv, ev->node);
            (void)zw_health_remove(&net->health, ev->node);
            checkin_drop(net, ev->node);
            drop_gone(net, ev->node);
            memset(old, 0, sizeof *old);
            old->node_id = ev->node;
            old->generation = generation;
            old->security_classes = ZW_NODE_SECURITY_UNKNOWN;
        }
    }
    if (ev->kind == ZW_INCL_EV_JOINED) {
        /* §4 step 5: remembered now - a new generation, interviewing, classes to come */
        zw_node_record_t *rec = zw_node_find(&net->nodes, ev->node);
        if (rec == NULL) {
            rec = new_record(net, ev->node);
        }
        if (rec != NULL) {
            uint8_t list[ZW_NODE_CC_MAX];
            zw_cc_iter_t it;
            uint16_t cc = 0u;
            bool controlled = false;
            size_t n = 0u;
            zw_cc_iter_init(&it, ev->nif, ev->nif_len);
            while (zw_cc_iter_next(&it, &cc, &controlled) && n < sizeof list) {
                if (!controlled && cc <= 0xFFu) {
                    list[n++] = (uint8_t)cc;
                }
            }
            for (size_t i = 0; i < n; i++) {
                zw_iv_add_cc(rec, list[i], true, false);
            }
            net->joined = ev->node;
            net->store_due = true;
            emit_node(net, ZW_NET_EV_NODE_ADDED, ev->node);
        }
    } else if (ev->kind == ZW_INCL_EV_DONE) {
        if (net->op == OP_ADD || net->op == OP_SMART) {
            net->op = OP_NONE;
        }
        zw_iv_hold(&net->iv, false); /* the other nodes' interviews go on */
        zw_node_record_t *rec = zw_node_find(&net->nodes, ev->node);
        bool interview = true;
        if (ev->smartstart) {
            zw_net_smart_done(net, ev, &interview);
        }
        if (ev->result == ZW_INCL_ADDED && rec != NULL && interview) {
            /* §4 step 7's outcome: the classes it holds, none when it failed (§4.1) */
            rec->security_classes = (uint8_t)(ev->granted & ZW_S2_KEYS_S2);
            rec->has_dsk = rec->security_classes != 0u;
            if (rec->has_dsk) {
                memcpy(rec->dsk, ev->dsk_bytes, sizeof rec->dsk);
            }
            if (ev->security == ZW_INCL_SEC_FAILED || ev->security == ZW_INCL_SEC_DOWNGRADED) {
                /* §4.1: a device whose key exchange did not finish - failed, timed out, or
                 * declined by the installer ("add it without encryption" is a KEX Fail to the
                 * device too) - falls back to working without security, and then lists its CCs in
                 * the clear: the node information it gave while joining (secure CCs left out) no
                 * longer says what it has, so it is asked again (step 3). (Before ZW-029 a
                 * declined one kept its joining list and could offer no input.) */
                rec->cc_count = 0u;
            }
            net->store_due = true;
            start_interview(net, ev->node);
        }
    }
    if (net->port.incl_event != NULL) {
        net->port.incl_event(net->port.ctx, ev);
    }
    if (ev->kind == ZW_INCL_EV_DONE) {
        zw_net_run_gone(net);
    }
}

/* ---- Set-up, poll ---------------------------------------------------------------------------- */

void zw_net_nodes_init(zw_net_t *net)
{
    const zw_iv_port_t ip = {net,         iv_send,  iv_node_info, iv_submit,
                             iv_changed,  iv_event, iv_quirk,     iv_wake_interval};
    zw_iv_init(&net->iv, &ip, &net->nodes, &net->s2);
    const zw_remove_port_t rp = {net, rm_submit, rm_ping, rm_event};
    zw_remove_init(&net->rm, &rp);
    zw_net_routes_init(net);
    zw_health_init(&net->health);
    zw_node_cache_init(&net->nodes);
}

void zw_net_nodes_poll(zw_net_t *net)
{
    if (!link_ready(net)) {
        return;
    }
    checkin_poll(net); /* first: No More Information it held may go in this poll */
    if (net->cfg.interview) {
        zw_health_action_t acts[8];
        for (unsigned round = 0; round < 8u; round++) {
            const size_t n = zw_health_poll(&net->health, net->now, acts, 8u);
            health_actions(net, acts, n);
            if (n < 8u) {
                break;
            }
        }
    }
    for (size_t i = 0; i < ZW_NET_PINGS; i++) {
        if (net->pings[i].node != 0u && !net->pings[i].out) {
            ping_send(net, i);
        }
    }
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        if (net->qout[i].used && net->qout[i].retry) {
            queue_go(net, i);
        }
    }
    zw_net_run_gone(net);
}

uint64_t zw_net_nodes_deadline(const zw_net_t *net)
{
    if (!link_ready(net)) {
        return UINT64_MAX;
    }
    uint64_t t = net->cfg.interview ? zw_health_next_deadline(&net->health) : UINT64_MAX;
    const uint64_t c = checkin_deadline(net);
    t = c < t ? c : t;
    for (size_t i = 0; i < ZW_NET_PINGS; i++) {
        if (net->pings[i].node != 0u && !net->pings[i].out) {
            t = net->now + RETRY_MS < t ? net->now + RETRY_MS : t;
        }
    }
    for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
        if (net->qout[i].used && net->qout[i].retry) {
            t = net->now + RETRY_MS < t ? net->now + RETRY_MS : t;
        }
    }
    if (net->gone_n > 0u && net->op == OP_NONE && !net->gone_held && net->now < net->gone_after &&
        net->gone_after < t) {
        t = net->gone_after; /* a reset-locally device's removal starts then */
    }
    return t;
}
