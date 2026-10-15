/*
 * zw_net.c - the host, the security manager, adding and removing devices, the interview and
 * supervision, wired together: the link, the routing of frames, the network operations. The
 * nodes' side is zw_net_nodes.c. See zw_net.h.
 */
#include "zwave_host/zw_net.h"

#include <string.h>

#include "zw_net_internal.h"
#include "zwave_host/zw_fwupd.h"
#include "zwave_host/zw_s2_cc.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_cc_encap.h"

/* serial-api.md §6.11: receive status. */
#define RX_FRAME_TYPE   0x0Cu /* bits 3..2: 00 singlecast */
#define RX_BROADCAST    0x04u /* bits 3..2: 01 broadcast (10 multicast) */
#define RX_FOREIGN      0xC0u /* bit 6 foreign frame, bit 7 foreign home ID */

/* serial-api.md 6.14: HardReset's callback (not documented; decided). */
#define RESET_MS 10000u

/* ---- The host's port ------------------------------------------------------------------------ */

static bool h_write(void *ctx, const uint8_t *bytes, size_t len)
{
    zw_net_t *net = ctx;
    return net->port.write(net->port.ctx, bytes, len);
}

static void h_reset_line(void *ctx, bool asserted)
{
    zw_net_t *net = ctx;
    net->port.reset_line(net->port.ctx, asserted);
}

static void h_link_up(void *ctx, const zw_module_facts_t *facts)
{
    zw_net_t *net = ctx;
    /* no keys stored: fresh ones, bound to this network only if the check finds it new (§2) */
    const bool fresh = !net->keys_ready;
    if (fresh && !zw_net_fresh_keys(net)) {
        /* ZW01: no keys - nothing starts on the unset set; the check draws again, and stops the
         * start if that fails too */
        net->stats.s2_start_failures++;
        zw_wipe(&net->keys, sizeof net->keys);
    } else if ((fresh || !net->s2.started || net->s2.home_id != facts->home_id ||
                net->s2.own_node != facts->node_id) &&
               !zw_s2_start(&net->s2, facts->home_id, facts->node_id, &net->keys)) {
        /* the same network, node and keys: the SPANs stay (a module restart); else the manager
         * starts afresh on this network */
        net->stats.s2_start_failures++; /* the crypto port failed: nothing secure until the next */
    }
    /* every link up follows a module start - the §7 recovery's soft reset too, which tells no
     * restart: the SmartStart listen mode is gone, set again by the next poll (§4.2) */
    zw_net_smart_lost(net);
    zw_net_check(net); /* §2: the node list, read again, then reconciled */
    if (net->port.link_up != NULL) {
        net->port.link_up(net->port.ctx, facts);
    }
}

static void h_link_down(void *ctx, const zw_link_down_t *why)
{
    zw_net_t *net = ctx;
    zw_routes_link_down(&net->routes, net->now); /* its END frees the operation */
    zw_net_smart_lost(net);
    zw_jam_reset(&net->jam); /* nothing known of the air without the radio (no event: the link
                              * itself is down) */
    net->jam_out = false;
    net->jam_reset = false; /* a jam episode ends with the link */
    zw_incl_link_down(&net->incl, net->now);
    zw_remove_link_down(&net->rm, net->now);
    zw_net_link_lost(net);
    if (net->port.link_down != NULL) {
        net->port.link_down(net->port.ctx, why);
    }
}

/* §14: the radio's own update ended; the host's start-up follows (NETWORK after it). */
static void h_otw_done(void *ctx)
{
    zw_net_t *net = ctx;
    if (net->op == OP_OTW) {
        net->op = OP_NONE;
    }
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_OTW;
    ev.otw = *zw_host_otw(&net->host);
    zw_net_emit(net, &ev);
}

/* The module restarts, by itself (serial-api.md §2) or by serial-api.md §7's soft reset: the
 * windows of "Add a device" and "Remove a device" closed with it (network.md §4, §10). Both are
 * started only while the link is up, so either still open while it is not is such a window. */
static void radio_restarted(zw_net_t *net)
{
    zw_incl_radio_restarted(&net->incl, net->now);
    zw_remove_radio_restarted(&net->rm, net->now);
}

static void h_restarted(void *ctx, uint8_t reason)
{
    zw_net_t *net = ctx;
    radio_restarted(net);
    zw_net_smart_lost(net);
    zw_net_link_lost(net); /* the check again at the link up that follows */
    if (net->port.restarted != NULL) {
        net->port.restarted(net->port.ctx, reason);
    }
}

/* ---- ZW-040: the signal each node was last heard with ------------------------------------------ */

static bool rssi_real(int8_t v)
{
    return v != ZW_RSSI_NOT_AVAILABLE && v != ZW_RSSI_SATURATED && v != ZW_RSSI_NO_SIGNAL;
}

/* The slot of a node; a free one, else the one whose newest reading is oldest. */
static zw_net_signal_t *signal_slot(zw_net_t *net, uint16_t node)
{
    size_t pick = 0u;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; node != 0u && i < ZW_NET_SIGNALS; i++) {
        if (net->signals[i].node == node) {
            return &net->signals[i].sig;
        }
        const zw_net_signal_t *s = &net->signals[i].sig;
        const uint64_t at = net->signals[i].node == 0u ? 0u
                            : (s->has_tx && s->tx_at_ms > s->rx_at_ms ? s->tx_at_ms : s->rx_at_ms);
        if (at < oldest) {
            oldest = at;
            pick = i;
        }
    }
    if (node == 0u) {
        return NULL;
    }
    memset(&net->signals[pick], 0, sizeof net->signals[pick]);
    net->signals[pick].node = node;
    return &net->signals[pick].sig;
}

void zw_net_signal_forget(zw_net_t *net, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZW_NET_SIGNALS; i++) {
        if (net->signals[i].node == node) {
            memset(&net->signals[i], 0, sizeof net->signals[i]);
        }
    }
}

bool zw_net_node_signal(const zw_net_t *net, uint16_t node, zw_net_signal_t *out)
{
    for (size_t i = 0; node != 0u && i < ZW_NET_SIGNALS; i++) {
        if (net->signals[i].node == node &&
            (net->signals[i].sig.has_rx || net->signals[i].sig.has_tx)) {
            if (out != NULL) {
                *out = net->signals[i].sig;
            }
            return true;
        }
    }
    return false;
}

static void signal_rx(zw_net_t *net, uint16_t node, int8_t rssi)
{
    zw_net_signal_t *s = rssi_real(rssi) ? signal_slot(net, node) : NULL;
    if (s != NULL) {
        s->has_rx = true;
        s->rx_rssi = rssi;
        s->rx_at_ms = net->now;
    }
}

static void signal_tx(zw_net_t *net, uint16_t node, const zw_api_tx_report_t *r)
{
    const int8_t ack = (int8_t)r->ack_rssi;
    zw_net_signal_t *s = rssi_real(ack) ? signal_slot(net, node) : NULL;
    if (s == NULL) {
        return;
    }
    s->has_tx = true;
    s->tx_rssi = ack;
    s->tx_at_ms = net->now;
    s->tx_repeaters = r->repeater_count < 4u ? r->repeater_count : 4u;
    for (size_t i = 0; i < 4u; i++) {
        s->tx_repeater_ids[i] = r->repeater_node_ids[i];
        s->tx_repeater_rssi[i] = (int8_t)r->ack_rssi_per_repeater[i];
    }
}

/* ApplicationCommandHandler: singlecast commands from this network go to the security
 * manager. */
static void on_app_command(zw_net_t *net, const zw_frame_t *frame)
{
    zw_api_app_cmd_t c;
    if (!zw_api_app_cmd_parse(frame->payload, frame->payload_len, zw_net_nid(net),
                              (zw_func_t)frame->func, &c) ||
        c.command == NULL || c.command_len == 0u) {
        net->stats.malformed++;
        return;
    }
    if (((unsigned)c.receive_status & RX_FOREIGN) != 0u) {
        net->stats.foreign++;
        return;
    }
    if ((c.receive_status & RX_FRAME_TYPE) != 0u) {
        /* ZW-042, as Z-Wave JS: a known node's broadcast or multicast command passes the same
         * security check as a singlecast one (an S2 node's plaintext one is a wrong level, a Wake
         * Up Notification too), and a Get-type one is discarded (Driver.ts shouldDiscardCC). An
         * unknown node's stays dropped */
        if (zw_node_find_const(&net->nodes, c.source_node_id) == NULL ||
            !zw_s2_receive_group(&net->s2, net->now, c.source_node_id, c.command,
                                 c.command_len)) {
            net->stats.not_singlecast++;
        }
        return;
    }
    if (c.has_rssi) {
        signal_rx(net, c.source_node_id, c.rssi); /* ZW-040: this network's own frames only */
    }
    zw_s2_receive(&net->s2, net->now, c.source_node_id, c.command, c.command_len);
}

static void h_frame(void *ctx, const zw_frame_t *frame)
{
    zw_net_t *net = ctx;
    if (frame->type == ZW_TYPE_REQUEST) {
        switch (frame->func) {
        case ZW_FUNC_ADD_NODE_TO_NETWORK:
        case ZW_FUNC_REPLACE_FAILED_NODE: /* §10: a replacement's later reports */
            zw_incl_module_frame(&net->incl, net->now, frame);
            return;
        case ZW_FUNC_REMOVE_NODE_FROM_NETWORK:
            zw_remove_module_frame(&net->rm, net->now, frame);
            return;
        case ZW_FUNC_APPLICATION_COMMAND_HANDLER:
        case ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER:
            on_app_command(net, frame);
            return;
        case ZW_FUNC_APPLICATION_UPDATE:
            if (zw_net_app_update(net, frame)) {
                return;
            }
            break;
        default:
            break;
        }
    }
    if (net->port.frame != NULL) {
        net->port.frame(net->port.ctx, frame);
    }
}

/* ---- The security manager's port ------------------------------------------------------------ */

/* §9: the radio jammed, or no longer: the owner hears it (the zone source's link, the pages). */
static void jam_changed(zw_net_t *net)
{
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_JAM;
    ev.jammed = zw_jam_jammed(&net->jam);
    if (!ev.jammed) {
        net->jam_reset = false; /* §6.12 serial-api.md: the air clear again ends the episode */
    }
    zw_net_emit(net, &ev);
}

static void jam_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    (void)tag;
    net->jam_out = false;
    bool changed;
    if (r->status == ZW_TX_OK && r->response != NULL && r->response_len > 0u) {
        changed = zw_jam_reading(&net->jam, net->now, r->response, r->response_len);
    } else {
        zw_jam_no_reading(&net->jam, net->now); /* a missing answer proves nothing */
        changed = zw_jam_poll(&net->jam, net->now);
    }
    if (changed) {
        jam_changed(net);
    }
}

/* Every ZW_JAM_POLL_MS while the link is up: the background noise; the failures age. */
static void jam_poll(zw_net_t *net)
{
    if (!net->cfg.jam || zw_host_link(&net->host) != ZW_LINK_UP || !net->checked) {
        return;
    }
    if (zw_jam_poll(&net->jam, net->now)) {
        jam_changed(net);
    }
    if (net->jam_out || net->now < net->jam_next) {
        return;
    }
    net->jam_next = net->now + ZW_JAM_POLL_MS;
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_GET_BACKGROUND_RSSI;
    req.expect = ZW_EXPECT_RESPONSE;
    req.prio = ZW_PRIO_NODE;
    req.done = jam_done;
    req.ctx = net;
    req.tag = zw_net_tag(NT_JAM, 0u);
    net->jam_out = zw_host_submit(&net->host, &req) == ZW_SUBMIT_OK;
}

static void send_data_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    zw_api_send_data_cb_t cb;
    const bool parsed = r->status == ZW_TX_OK && r->callback != NULL &&
                        zw_api_send_data_cb_parse(r->callback, r->callback_len, &cb);
    const bool delivered = parsed && cb.transmit_status == (uint8_t)ZW_TX_STATUS_OK;
    const bool jammed = parsed && cb.transmit_status == (uint8_t)ZW_TX_STATUS_FAIL &&
                        cb.has_tx_report && cb.tx_report.transmit_ticks == 0u;
    if (net->cfg.jam && jammed && zw_jam_tx_failed(&net->jam, net->now)) {
        jam_changed(net); /* §9: the module could not get the air at all */
    }
    /* serial-api.md §6.12: what the callback said, for zw_s2's transmissions */
    zw_s2_tx_t tx = ZW_S2_TX_UNKNOWN;
    if (delivered) {
        tx = ZW_S2_TX_OK;
    } else if (jammed) {
        tx = ZW_S2_TX_JAMMED;
    } else if (parsed && cb.transmit_status == (uint8_t)ZW_TX_STATUS_NO_ACK) {
        tx = ZW_S2_TX_NO_ACK;
    } else if (parsed && cb.transmit_status == (uint8_t)ZW_TX_STATUS_FAIL) {
        tx = ZW_S2_TX_FAIL;
    } else if (parsed && cb.transmit_status == (uint8_t)ZW_TX_STATUS_NO_ROUTE) {
        tx = ZW_S2_TX_NO_ROUTE;
    }
    if (delivered) {
        net->jam_reset = false; /* §6.12: a delivery ends a jam episode */
    }
    if (delivered && cb.has_tx_report) {
        for (size_t i = 0; i < ZW_NET_FRAMES; i++) {
            if (net->frames[i].node != 0u && net->frames[i].tag == tag) {
                signal_tx(net, net->frames[i].node, &cb.tx_report); /* ZW-040 */
                break;
            }
        }
    }
    zw_net_frame_done(net, tag, delivered); /* §8: every ACK of a listening device counts */
    zw_s2_sent_tx(&net->s2, net->now, tag, tx);
}

/* serial-api.md §6.12: a frame the radio did not deliver goes again, ZW_S2_TX_ATTEMPTS
 * transmissions in all - to a device that listens, and in the key exchange (the device joining
 * listens for it). A sleeping device's NoAck means it is asleep: once, and the command goes back
 * into its wake-up queue (network.md §7). Owners that ask again themselves go once, so a frame
 * still goes at most 3 times in all (ZW-037): the panel's own checks' pings (supervision, removal,
 * route checks), the interview's questions (ZW_IV_TRIES) and a firmware update's frames
 * (ZW_FWUPD_TRIES). A Ping asked for on a device's page is a command like any other (3). */
static uint8_t s_attempts(void *ctx, uint16_t node, uint32_t tag)
{
    const zw_net_t *net = ctx;
    if (tag == ZW_NET_INCL_TAG) {
        return (uint8_t)ZW_S2_TX_ATTEMPTS;
    }
    if (zw_net_is_own(tag, NT_PING) || zw_remove_is_tag(tag) || zw_routes_is_tag(tag) ||
        zw_iv_is_tag(tag) || (tag & ZW_FWUPD_TAG_MASK) == ZW_FWUPD_TAG_BASE ||
        zw_net_sleeping(net, node)) {
        return 1u;
    }
    return (uint8_t)ZW_S2_TX_ATTEMPTS;
}

/* serial-api.md §6.12: the module could not send a frame at all (Fail with 0 transmit ticks).
 * Not a delivery attempt, whether jam watching is on or not: the same frame again after 1 s, 5
 * transmissions in all; then the module is soft-reset once (§7's recovery, zw_host_recover) and
 * the frame goes once more, behind the gate, when the module is back. Still jammed: it fails, and
 * so does every jammed frame after it, at once, until the episode ends - a frame delivered, the
 * air clear again (§9) or the link down: one reset an episode, as Z-Wave JS (Driver.ts
 * handleJammedController: retryJammed 1000 ms, sendDataJammed 5, then JammedAfterReset with one
 * attempt). The wait lives in the security manager's send slot - each frame has its own deadline
 * and keeps its bytes, so a jammed S2 frame goes again with the same sequence number and nonce -
 * and the episode here, where the host and the jam watch are. */
static zw_s2_jam_t s_jammed(void *ctx, uint16_t node, uint32_t tag, uint8_t jams)
{
    zw_net_t *net = ctx;
    (void)node;
    (void)tag;
    if (net->jam_reset) {
        return ZW_S2_JAM_FAIL; /* the episode's reset is behind it */
    }
    if (jams < ZW_S2_JAM_TRIES) {
        return ZW_S2_JAM_WAIT;
    }
    if (!zw_host_recover(&net->host, net->now)) {
        return ZW_S2_JAM_FAIL; /* not now (the link starting or recovering already) */
    }
    net->jam_reset = true;
    net->stats.jam_resets++;
    /* the module restarts, as zw_net_restart_link's: its listen mode goes, and a power level set
     * since takes effect (the facts unknown until read again) */
    zw_net_smart_lost(net);
    zw_net_power_lost(net);
    return ZW_S2_JAM_NOW;
}

/* A frame for a node: SendData; replies (Nonce Reports, Supervision Reports) in the reply class. */
static bool s_send(void *ctx, uint16_t node, const uint8_t *frame, size_t len, uint8_t tx_options,
                   uint32_t tag)
{
    zw_net_t *net = ctx;
    const zw_api_send_data_req_t sd = {node, frame, len, tx_options, 0u};
    uint8_t buf[ZW_PAYLOAD_MAX];
    size_t at = 0u;
    const size_t n = zw_api_send_data_req_build(buf, sizeof buf, zw_net_nid(net), &sd);
    if (n == 0u || !zw_api_callback_id_offset((uint8_t)ZW_FUNC_SEND_DATA, buf, n, &at)) {
        return false;
    }
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_SEND_DATA;
    req.payload = buf;
    req.payload_len = n;
    req.expect = ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK;
    req.callback_id_at = (uint8_t)at;
    req.prio = zw_s2_frame_is_reply(&net->s2, tag) ? ZW_PRIO_REPLY : ZW_PRIO_NODE;
    req.done = send_data_done;
    req.ctx = net;
    req.tag = tag;
    if (zw_host_submit(&net->host, &req) != ZW_SUBMIT_OK) {
        return false;
    }
    zw_net_frame_sent(net, tag, node);
    return true;
}

/* The key exchange's commands to the inclusion; the rest to the nodes' side and the owner. */
static void s_received(void *ctx, const zw_s2_rx_t *rx)
{
    zw_net_t *net = ctx;
    if (rx->len >= 2u && rx->command[0] == ZW_S2_CC && rx->command[1] >= ZW_S2_KEX_GET &&
        rx->command[1] <= ZW_S2_TRANSFER_END) {
        /* accepted: heard from the node (§8; Z-Wave JS's handleRequest marks it alive) */
        zw_net_heard(net, rx->node);
        zw_incl_s2_command(&net->incl, net->now, rx);
        return;
    }
    if (rx->len >= 2u && rx->command[0] == (uint8_t)ZW_CC_SECURITY_0 && rx->command[1] == 0x40u) {
        /* ZW-009: a Security 0 Nonce Get is the driver's, never the application's (Z-Wave JS
         * handles Nonce Gets in Driver.ts): heard from the node as Security 2's Nonce Get is (§8),
         * no wake-up ("Nonces can be sent while asleep", Node.ts), no report, nothing a zone sees.
         * The panel speaks no Security 0: it is not answered */
        zw_net_heard(net, rx->node);
        return;
    }
    zw_net_received(net, rx);
}

static void s_send_done(void *ctx, uint32_t tag, zw_s2_result_t result)
{
    zw_net_t *net = ctx;
    if (tag == ZW_NET_INCL_TAG || zw_net_own_send_done(net, tag, result)) {
        return;
    }
    if (net->port.send_done != NULL) {
        net->port.send_done(net->port.ctx, tag, result);
    }
}

static void s_event(void *ctx, uint16_t node, zw_s2_event_t ev)
{
    zw_net_t *net = ctx;
    zw_incl_s2_event(&net->incl, net->now, node, ev);
    if (ev == ZW_S2_EV_ANSWERED && zw_net_sleeping(net, node)) {
        /* a Commands Supported Get: a Get, so a sleeper is awake (Z-Wave JS Node.ts) */
        zw_net_woke(net, node);
    } else if (ev == ZW_S2_EV_NONCE || ev == ZW_S2_EV_ANSWERED) {
        /* a frame Security 2 accepted and kept for itself; a dropped one never counts (§8). A
         * Nonce Get is no wake-up ("Nonces can be sent while asleep", Node.ts). A node whose
         * classes are not known is no exception: its S2 is answered with a Nonce Report and
         * tried with every key it may hold (zw_s2_set_classes_unknown) */
        zw_net_heard(net, node);
    } else if (ev == ZW_S2_EV_CLASS_LEARNT) {
        /* ZW-009, ZW-017: a class learnt by decrypting once step 4 has recorded the node's classes
         * (Z-Wave JS's securityClasses is a cache-backed map: setSecurityClass is stored at
         * once). While the record says "not known" the running probe records instead. */
        zw_node_record_t *rec = zw_node_find(&net->nodes, node);
        const uint8_t learnt = (uint8_t)(zw_s2_granted(&net->s2, node) & ZW_S2_KEYS_S2);
        if (rec != NULL && rec->security_classes != ZW_NODE_SECURITY_UNKNOWN &&
            (rec->security_classes | learnt) != rec->security_classes) {
            rec->security_classes = (uint8_t)(rec->security_classes | learnt);
            net->store_due = true;
        }
    }
    if (net->port.s2_event != NULL) {
        net->port.s2_event(net->port.ctx, node, ev);
    }
}

/* ---- The inclusion's port ------------------------------------------------------------------- */

static zw_submit_t i_submit(void *ctx, const zw_request_t *req)
{
    zw_net_t *net = ctx;
    return zw_host_submit(&net->host, req);
}

static zw_s2_result_t i_s2_send(void *ctx, uint16_t node, zw_sec_class_t cls, const uint8_t *cmd,
                                size_t len)
{
    zw_net_t *net = ctx;
    return zw_s2_send(&net->s2, net->now, node, cls, cmd, len, ZW_NET_INCL_TAG);
}

static bool i_known(void *ctx, uint16_t node)
{
    zw_net_t *net = ctx;
    return zw_node_find_const(&net->nodes, node) != NULL ||
           (net->port.known != NULL && net->port.known(net->port.ctx, node));
}

static void i_event(void *ctx, const zw_incl_event_t *ev)
{
    zw_net_t *net = ctx;
    if (ev->kind == ZW_INCL_EV_DONE && ev->replaced) {
        /* a replacement that ended while its check ran (stopped, the radio gone): the check ends
         * with it, unheard */
        zw_remove_cancel_check(&net->rm, net->now);
    }
    zw_net_incl_event(net, ev);
}

/* ---- API ------------------------------------------------------------------------------------- */

void zw_net_init(zw_net_t *net, const zw_net_port_t *port, const zw_net_config_t *cfg,
                 const zw_crypto_port_t *crypto)
{
    memset(net, 0, sizeof *net);
    net->port = *port;
    if (cfg != NULL) {
        net->cfg = *cfg;
    }
    net->keys = net->cfg.keys;
    net->keys_bound = net->cfg.keys_bound;
    net->keys_ready = net->cfg.keys_bound;
    net->keys_home_id = net->cfg.keys_home_id;
    net->generation = net->cfg.generation;
    zw_jam_init(&net->jam, net->cfg.jam_dbm != 0 ? net->cfg.jam_dbm : (int8_t)ZW_JAM_THRESHOLD_DBM);
    const zw_host_port_t hp = {net,         h_write,
                               port->reset_line != NULL ? h_reset_line : NULL,
                               h_link_up,   h_link_down,
                               h_restarted, h_frame,
                               h_otw_done};
    zw_host_init(&net->host, &hp, &net->cfg.host);
    const zw_s2_port_t sp = {net, s_send, s_received, s_send_done, s_event};
    const zw_s2_config_t sc = {true, 0u};
    zw_s2_init(&net->s2, crypto, &sp, &sc);
    zw_s2_set_attempts(&net->s2, s_attempts);
    zw_s2_set_jammed(&net->s2, s_jammed);
    const zw_incl_port_t ip = {net, i_submit, i_s2_send, i_known, i_event};
    zw_incl_init(&net->incl, &ip, &net->s2);
    zw_net_nodes_init(net);
}

bool zw_net_load_nodes(zw_net_t *net, const uint8_t *blob, size_t len)
{
    const bool ok = zw_node_cache_decode(&net->nodes, blob, len);
    for (size_t i = 0; i < zw_node_count(&net->nodes); i++) {
        const uint16_t g = zw_node_at_const(&net->nodes, i)->generation;
        if (g > net->generation) {
            net->generation = g; /* a lost counter never goes back below a device's own */
        }
    }
    return ok;
}

void zw_net_start(zw_net_t *net, uint64_t now)
{
    net->now = now;
    zw_host_start(&net->host, now);
}

void zw_net_on_control(zw_net_t *net, uint64_t now, uint8_t byte)
{
    net->now = now;
    zw_host_on_control(&net->host, now, byte);
}

void zw_net_on_frame(zw_net_t *net, uint64_t now, const zw_frame_t *frame)
{
    net->now = now;
    zw_host_on_frame(&net->host, now, frame);
}

void zw_net_on_text(zw_net_t *net, uint64_t now, const uint8_t *bytes, size_t len)
{
    net->now = now;
    zw_host_on_text(&net->host, now, bytes, len);
}

/* ZW01: a stopped first start waits for its retry (a link up checks anyway). */
static bool key_retry_due(const zw_net_t *net)
{
    return net->key_retry_at != 0u && !net->checked && zw_host_link(&net->host) == ZW_LINK_UP;
}

void zw_net_poll(zw_net_t *net, uint64_t now)
{
    net->now = now;
    net->s2.cfg.max_frame = net->host.facts.max_payload; /* the module's own limit (§5 step 8) */
    zw_s2_poll(&net->s2, now);
    zw_incl_poll(&net->incl, now);
    zw_remove_poll(&net->rm, now);
    zw_iv_poll(&net->iv, now);
    zw_net_nodes_poll(net);
    zw_routes_poll(&net->routes, now);
    zw_net_smart_poll(net);
    zw_net_power_poll(net);
    jam_poll(net);
    zw_host_poll(&net->host, now);
    if (zw_host_link(&net->host) == ZW_LINK_RECOVERING ||
        zw_host_link(&net->host) == ZW_LINK_STARTING) {
        /* serial-api.md §7's recovery (the module stopped answering, a jammed radio's reset) or
         * a new start-up restarts the module without a restarted(): its windows are gone all the
         * same */
        radio_restarted(net);
    }
    if (key_retry_due(net) && now >= net->key_retry_at) {
        net->key_retry_at = 0u;
        zw_net_check(net); /* ZW01: the stopped first start again */
    }
    zw_net_flush(net);
}

uint64_t zw_net_next_deadline(const zw_net_t *net)
{
    uint64_t t = zw_host_next_deadline(&net->host);
    if (key_retry_due(net) && net->key_retry_at < t) {
        t = net->key_retry_at;
    }
    const uint64_t d[5] = {zw_s2_next_deadline(&net->s2), zw_incl_next_deadline(&net->incl),
                           zw_remove_next_deadline(&net->rm), zw_iv_next_deadline(&net->iv),
                           zw_net_nodes_deadline(net)};
    for (size_t i = 0; i < 5u; i++) {
        if (d[i] < t) {
            t = d[i];
        }
    }
    if (net->cfg.jam && zw_host_link(&net->host) == ZW_LINK_UP && net->checked && !net->jam_out &&
        net->jam_next < t) {
        t = net->jam_next;
    }
    if (net->cfg.jam && zw_jam_jammed(&net->jam) && net->now + 1000u < t) {
        t = net->now + 1000u; /* its end is judged as time passes */
    }
    return net->store_due && net->now < t ? net->now : t;
}

bool zw_net_jammed(const zw_net_t *net)
{
    return zw_jam_jammed(&net->jam);
}

const zw_jam_t *zw_net_jam(const zw_net_t *net)
{
    return &net->jam;
}

bool zw_net_set_jam_threshold(zw_net_t *net, int8_t dbm)
{
    if (dbm < -100 || dbm > -20) {
        return false;
    }
    net->cfg.jam_dbm = dbm;
    zw_jam_set_threshold(&net->jam, dbm);
    return true;
}

bool zw_net_add_node(zw_net_t *net, uint16_t node, uint8_t granted, const uint8_t *nif,
                     size_t nif_len)
{
    return zw_s2_add_node(&net->s2, node, granted, nif, nif_len);
}

/* Byte i of a command, 0 past its end. */
static uint32_t cmd_byte(const uint8_t *cmd, size_t len, size_t i)
{
    return i < len ? (uint32_t)cmd[i] : 0u;
}

/* An encapsulation queue_kind does not look into: its outer bytes say nothing of the command it
 * carries - Multi Instance v1, CRC-16, Supervision Get, Multi Command, Security 0 and 2, Transport
 * Service, and a Multi Channel one to several end points at once (bit-addressed) or too short. */
static bool opaque_encapsulation(const uint8_t *cmd, size_t len)
{
    const uint32_t c = cmd_byte(cmd, len, 1u);
    switch (cmd[0]) {
    case (uint8_t)ZW_CC_MULTI_CHANNEL:
        return c == 0x06u || /* Multi Instance Command Encapsulation (v1) */
               (c == ZW_CMD_MULTI_CHANNEL_COMMAND_ENCAPSULATION &&
                (len < 4u || (cmd[3] & 0x80u) != 0u));
    case (uint8_t)ZW_CC_CRC16_ENCAPSULATION:
        return c == ZW_CMD_CRC16_ENCAPSULATE;
    case (uint8_t)ZW_CC_SUPERVISION:
        return c == ZW_CMD_SUPERVISION_GET;
    case (uint8_t)ZW_CC_MULTI_COMMAND:
        return c == ZW_CMD_MULTI_COMMAND_ENCAPSULATE;
    case (uint8_t)ZW_CC_SECURITY_0:
        return c == 0x81u || c == 0xC1u; /* Message Encapsulation (Nonce Get) */
    case (uint8_t)ZW_CC_SECURITY_2:
        return c == ZW_S2_MESSAGE_ENCAPSULATION;
    case (uint8_t)ZW_CC_TRANSPORT_SERVICE:
        return true;
    default:
        return false;
    }
}

/* §7: what an owner's command replaces in a sleeping device's queue - an older one of the same
 * CC, command and first parameter to the same destination. A Multi Channel encapsulation is keyed
 * by its destination end point and inner command (ZW13: Z-Wave JS keeps different Multi Channel
 * destinations apart, MultiChannelCC.ts); an encapsulation it does not look into replaces nothing
 * (a stamp of its own), and no kind is ever zw_health's ping tag. */
static uint32_t queue_kind(zw_net_t *net, const uint8_t *cmd, size_t len)
{
    uint32_t kind = KIND_OWNER | cmd_byte(cmd, len, 0u) << 16 | cmd_byte(cmd, len, 1u) << 8 |
                    cmd_byte(cmd, len, 2u);
    if (!opaque_encapsulation(cmd, len) && cmd[0] == (uint8_t)ZW_CC_MULTI_CHANNEL &&
        cmd_byte(cmd, len, 1u) == ZW_CMD_MULTI_CHANNEL_COMMAND_ENCAPSULATION) {
        kind = KIND_OWNER_EP | (uint32_t)(cmd[3] & 0x7Fu) << 24 | cmd_byte(cmd, len, 4u) << 16 |
               cmd_byte(cmd, len, 5u) << 8 | cmd_byte(cmd, len, 6u);
    }
    /* end point 127's FF FF FF would be ZW_HEALTH_PING_TAG, which the queue refuses: a stamp */
    if (opaque_encapsulation(cmd, len) || kind == ZW_HEALTH_PING_TAG) {
        net->reply_stamp = (uint16_t)((unsigned)net->reply_stamp + 1u); /* any unique stamp */
        kind = KIND_OWNER_EPS | (uint32_t)net->reply_stamp;
    }
    return kind;
}

zw_s2_result_t zw_net_send(zw_net_t *net, uint64_t now, uint16_t node, zw_sec_class_t cls,
                           const uint8_t *cmd, size_t len, uint32_t tag)
{
    net->now = now;
    if ((tag & ZW_NET_OWN_TAGS) == ZW_NET_OWN_TAGS || cmd == NULL || len == 0u) {
        return ZW_S2_INVALID;
    }
    if (zw_net_sleeping(net, node)) {
        /* §7: its wake-up queue; a newer command of the same kind replaces an older one */
        const zw_sec_class_t c = cls == ZW_SEC_AUTO ? zw_s2_class_for(&net->s2, node, cmd[0]) : cls;
        if (zw_s2_command_is_reply(cmd, len)) {
            /* a reply (a Supervision Report): the device that asked listens now. Asleep (a report
             * it supervised is no wake-up): it goes at once and the device stays asleep - Z-Wave
             * JS's Immediate queue, "Do not update the node status when we only responded to a
             * nonce/supervision request" (Driver.ts) - so no No More Information follows it and
             * its queue waits for its wake-up: a frame it never ACKs takes the shared SPAN's next
             * nonce, and its next report fails to decrypt (B026). Awake: first in its queue,
             * under a kind of its own (nothing replaces it); No More Information waits behind it */
            if (!zw_net_counts_awake(net, node)) {
                return zw_s2_send(&net->s2, now, node, c, cmd, len, tag);
            }
            net->reply_stamp = (uint16_t)((unsigned)net->reply_stamp + 1u);
            bool held = false;
            const zw_s2_result_t r =
                zw_net_queue_send(net, node, c, cmd, len, tag,
                                  KIND_REPLY | (uint32_t)net->reply_stamp, true, &held);
            if (r == ZW_S2_OK && held) {
                zw_net_woke(net, node);
            }
            return r;
        }
        bool held = false;
        return zw_net_queue_send(net, node, c, cmd, len, tag, queue_kind(net, cmd, len), false,
                                 &held);
    }
    return zw_s2_send(&net->s2, now, node, cls, cmd, len, tag);
}

/* Refusals every network operation shares (network.md §1). */
int zw_net_op_refusal(const zw_net_t *net, bool allowed)
{
    if (!allowed) {
        return 1;
    }
    if (zw_host_link(&net->host) != ZW_LINK_UP) {
        return 2;
    }
    if (net->op != OP_NONE || !net->checked) {
        return 3;
    }
    return 0;
}

zw_incl_result_t zw_net_add_device(zw_net_t *net, uint64_t now, bool allowed,
                                   uint16_t listening_nodes, uint16_t flirs_nodes)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused == 2) {
        return ZW_INCL_LINK_DOWN;
    }
    if (refused == 3) {
        return ZW_INCL_BUSY;
    }
    if (refused == 0 && net->radio == ZW_NET_RADIO_FOREIGN) {
        return ZW_INCL_FOREIGN_RADIO; /* §2: reset it to use it here */
    }
    if (refused == 0 && net->radio == ZW_NET_RADIO_REPLACED) {
        /* §2: the installer adds devices to the new radio - the old cache goes, and the old
         * network's keys are never reused on the new one (ZW-017) */
        const zw_module_facts_t *f = zw_host_facts(&net->host);
        if (!zw_net_fresh_keys(net) || !zw_s2_start(&net->s2, f->home_id, f->node_id, &net->keys)) {
            net->stats.s2_start_failures++;
            return ZW_INCL_NOT_LISTENING;
        }
        zw_net_forget_all(net);
        zw_net_bind_keys(net, f->home_id);
        net->radio = ZW_NET_RADIO_OURS;
        zw_iv_link_up(&net->iv, now, f->node_id, zw_net_nid(net)); /* its devices get interviewed */
        zw_net_flush(net);
    }
    const zw_incl_options_t o = {listening_nodes, flirs_nodes, zw_net_nid(net)};
    if (allowed) {
        net->ss_on = false; /* AddNode(any) ends SmartStart's listen mode */
    }
    const zw_incl_result_t r = zw_incl_start(&net->incl, now, allowed, &o); /* ZW-005's refusal */
    if (r == ZW_INCL_STARTED) {
        net->op = OP_ADD;
        zw_net_quiet(net);
    }
    return r;
}

/* §10: the old device's check is over - replace it, or why not. */
static void replace_checked(void *ctx, uint16_t node, zw_remove_result_t result)
{
    zw_net_t *net = ctx;
    (void)node;
    zw_incl_result_t verdict;
    switch (result) {
    case ZW_REMOVE_LISTED: verdict = ZW_INCL_STARTED; break; /* ReplaceFailedNode now */
    case ZW_REMOVE_STILL_ANSWERS: verdict = ZW_INCL_STILL_ANSWERS; break;
    case ZW_REMOVE_NOT_FAILED: verdict = ZW_INCL_NOT_FAILED; break;
    case ZW_REMOVE_LINK_DOWN: verdict = ZW_INCL_LINK_DOWN; break;
    case ZW_REMOVE_STOPPED: verdict = ZW_INCL_STOPPED; break;
    default: verdict = ZW_INCL_NOT_LISTENING; break; /* IsFailedNode got no answer */
    }
    zw_incl_replace_checked(&net->incl, net->now, verdict);
}

zw_incl_result_t zw_net_replace_failed(zw_net_t *net, uint64_t now, bool allowed, uint16_t node)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused == 1) {
        return ZW_INCL_NOT_ALLOWED;
    }
    if (refused == 2) {
        return ZW_INCL_LINK_DOWN;
    }
    if (refused == 3) {
        return ZW_INCL_BUSY;
    }
    if (net->radio != ZW_NET_RADIO_OURS || zw_node_find_const(&net->nodes, node) == NULL) {
        return ZW_INCL_BAD_NODE_ID;
    }
    uint16_t listening = 0u, flirs = 0u;
    for (size_t i = 0; i < zw_node_count(&net->nodes); i++) {
        const zw_node_record_t *rec = zw_node_at_const(&net->nodes, i);
        listening = (uint16_t)((unsigned)listening + (rec->is_listening ? 1u : 0u));
        flirs = (uint16_t)((unsigned)flirs + (rec->is_flirs ? 1u : 0u));
    }
    if (zw_remove_state(&net->rm) != ZW_REMOVE_IDLE) {
        return ZW_INCL_BUSY; /* its check needs the removals' pings */
    }
    zw_net_smart_pause(net);
    const zw_incl_options_t o = {listening, flirs, zw_net_nid(net)};
    const zw_incl_result_t r = zw_incl_replace(&net->incl, now, allowed, &o, node);
    if (r != ZW_INCL_STARTED) {
        return r;
    }
    net->op = OP_ADD; /* held through the check: nothing else starts meanwhile */
    zw_net_quiet(net);
    /* §10: checked first, as for removing a failed device - 3 pings 2 s apart (any answer
     * refuses it), then IsFailedNode; only a node the module lists goes to ReplaceFailedNode */
    if (zw_remove_check_failed(&net->rm, now, node, zw_net_nid(net), replace_checked, net) !=
        ZW_REMOVE_STARTED) {
        zw_incl_replace_checked(&net->incl, now, ZW_INCL_NOT_LISTENING); /* not reached: idle */
    }
    return r;
}

/* §4: the panel asks nothing of other nodes while a device is added - key exchange and PIN
 * window included; their interviews go on at its end. A sleeping device's question waiting in its
 * queue is taken back too, or its wake-up would send it meanwhile. */
void zw_net_quiet(zw_net_t *net)
{
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        const zw_iv_node_t *s = &net->iv.slots[i];
        if (s->node != 0u && s->parked) {
            (void)zw_health_queue_cancel(&net->health, s->node, KIND_IV);
        }
    }
    zw_iv_hold(&net->iv, true);
}

zw_remove_result_t zw_net_remove_device(zw_net_t *net, uint64_t now, bool allowed)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused == 1) {
        return ZW_REMOVE_NOT_ALLOWED;
    }
    if (refused == 2) {
        return ZW_REMOVE_LINK_DOWN;
    }
    if (refused == 3) {
        return ZW_REMOVE_BUSY;
    }
    zw_net_smart_pause(net);
    const zw_remove_result_t r = zw_remove_start(&net->rm, now, allowed, zw_net_nid(net));
    if (r == ZW_REMOVE_STARTED) {
        net->op = OP_REMOVE;
    }
    return r;
}

zw_remove_result_t zw_net_remove_failed(zw_net_t *net, uint64_t now, bool allowed, uint16_t node)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused == 1) {
        return ZW_REMOVE_NOT_ALLOWED;
    }
    if (zw_node_find_const(&net->nodes, node) == NULL) {
        return ZW_REMOVE_UNKNOWN_NODE;
    }
    if (refused == 2) {
        return ZW_REMOVE_LINK_DOWN;
    }
    if (refused == 3) {
        return ZW_REMOVE_BUSY;
    }
    zw_net_smart_pause(net);
    const zw_remove_result_t r = zw_remove_failed(&net->rm, now, allowed, node, zw_net_nid(net));
    if (r == ZW_REMOVE_STARTED) {
        net->op = OP_REMOVE;
    }
    return r;
}

zw_net_result_t zw_net_op_result(int refused)
{
    return refused == 1   ? ZW_NET_NOT_ALLOWED
           : refused == 2 ? ZW_NET_LINK_DOWN
                          : ZW_NET_BUSY;
}

/* The start-up again, from step 0 (a region change, a reset, a restart): the interview and
 * supervision pause until the link is up and checked. */
void zw_net_restart_link(zw_net_t *net)
{
    zw_net_smart_lost(net); /* the module restarts: its listen mode goes */
    zw_net_power_lost(net);
    zw_net_link_lost(net);
    zw_host_start(&net->host, net->now);
}

zw_net_result_t zw_net_set_region(zw_net_t *net, uint64_t now, bool allowed, uint8_t region)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused != 0) {
        return zw_net_op_result(refused);
    }
    const zw_module_facts_t *f = zw_host_facts(&net->host);
    if (memchr(f->regions, region, f->region_count) == NULL) {
        return ZW_NET_UNSUPPORTED; /* §12: only what the module offers */
    }
    if (f->region_known && f->region == region) {
        return ZW_NET_UNCHANGED;
    }
    net->cfg.host.set_region = true;
    net->cfg.host.region = region;
    zw_host_set_config(&net->host, &net->cfg.host); /* step 10 SetRFRegion, 12 the soft reset */
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_REGION;
    ev.region = region;
    zw_net_emit(net, &ev);
    zw_net_restart_link(net);
    return ZW_NET_STARTED;
}

static void reset_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    (void)r; /* whatever it answered, the module is started afresh (serial-api.md 6.14) */
    if (tag != zw_net_tag(NT_RESET, net->op_gen) || net->op != OP_RESET) {
        return;
    }
    net->op = OP_NONE;
    net->radio = ZW_NET_RADIO_UNKNOWN;
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_RESET;
    zw_net_emit(net, &ev);
    zw_net_restart_link(net); /* the new home ID's check binds the new keys (a first start) */
}

zw_net_result_t zw_net_reset_network(zw_net_t *net, uint64_t now, bool allowed,
                                     const zw_s2_keys_t *keys)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused != 0) {
        return zw_net_op_result(refused);
    }
    /* the network's state goes first: nothing of the old one may be used on the new one */
    zw_net_forget_all(net);
    if (keys != NULL) {
        net->keys = *keys;
        net->keys_ready = true;
        net->keys_bound = false;
    } else if (!zw_net_fresh_keys(net)) {
        net->keys_ready = false; /* made at the next link up */
        net->keys_bound = false;
    }
    zw_net_flush(net);
    uint8_t buf[1] = {0u};
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_HARD_RESET;
    req.payload = buf;
    req.payload_len = sizeof buf;
    req.expect = ZW_EXPECT_CALLBACK;
    req.callback_id_at = 0u;
    req.callback_timeout_ms = RESET_MS;
    req.prio = ZW_PRIO_NODE;
    req.done = reset_done;
    req.ctx = net;
    net->op_gen = (uint16_t)((unsigned)net->op_gen + 1u);
    req.tag = zw_net_tag(NT_RESET, net->op_gen);
    zw_net_smart_pause(net);
    net->op = OP_RESET;
    if (zw_host_submit(&net->host, &req) != ZW_SUBMIT_OK) {
        const zw_tx_result_t none = {ZW_TX_LINK_DOWN, 0u, 0u, 0u, NULL, 0u, NULL, 0u};
        reset_done(net, req.tag, &none); /* the start-up again will find what the module has */
    }
    return ZW_NET_STARTED;
}

zw_net_result_t zw_net_update_radio(zw_net_t *net, uint64_t now, bool allowed,
                                    const uint8_t *image, size_t len)
{
    net->now = now;
    if (!allowed) {
        return ZW_NET_NOT_ALLOWED;
    }
    if (net->op != OP_NONE) {
        return ZW_NET_BUSY;
    }
    switch (zw_host_otw_begin(&net->host, now, image, len)) {
    case ZW_OTW_BEGUN:
        net->op = OP_OTW;
        return ZW_NET_STARTED;
    case ZW_OTW_BAD_FILE: return ZW_NET_INVALID;
    case ZW_OTW_NO_MODULE: return ZW_NET_LINK_DOWN;
    default: return ZW_NET_BUSY;
    }
}

const zw_otw_progress_t *zw_net_otw(const zw_net_t *net)
{
    return zw_host_otw(&net->host);
}

zw_net_result_t zw_net_restart(zw_net_t *net, uint64_t now, bool allowed)
{
    net->now = now;
    if (!allowed) {
        return ZW_NET_NOT_ALLOWED;
    }
    if (net->op != OP_NONE) {
        return ZW_NET_BUSY;
    }
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_RESTART;
    zw_net_emit(net, &ev);
    /* the link down or starting too: at once, not at the recovery's next attempt; step 12
     * soft-resets it, and the check at the link up keeps what is ours */
    zw_net_restart_link(net);
    return ZW_NET_STARTED;
}

/* A sleeping device awake now (§7: its wake-up being served, No More Information not yet sent). */
static bool node_awake(const zw_net_t *net, uint16_t node)
{
    for (size_t i = 0; i < net->health.node_count && i < ZW_HEALTH_NODES_MAX; i++) {
        if (net->health.nodes[i].node_id == node) {
            return net->health.nodes[i].awake;
        }
    }
    return false;
}

static bool interview(zw_net_t *net, uint64_t now, uint16_t node, bool may_wait)
{
    net->now = now;
    const bool ok = net->cfg.interview && (may_wait ? zw_iv_restart(&net->iv, now, node)
                                                    : zw_iv_restart_now(&net->iv, now, node));
    if (ok) {
        zw_health_set_busy(&net->health, node, zw_iv_asking(&net->iv, node),
                           node == net->hold_awake);
        if (node_awake(net, node)) {
            /* BL-152: a sleeper's re-read waits for its wake-up - it is awake now (Z-Wave JS
             * assumes a node awake at the start of a re-interview the same way) */
            zw_iv_awake(&net->iv, now, node);
            zw_health_set_busy(&net->health, node, zw_iv_asking(&net->iv, node),
                               node == net->hold_awake);
        }
    }
    zw_net_flush(net);
    return ok;
}

bool zw_net_interview(zw_net_t *net, uint64_t now, uint16_t node)
{
    return interview(net, now, node, true);
}

bool zw_net_interview_now(zw_net_t *net, uint64_t now, uint16_t node)
{
    return interview(net, now, node, false);
}

void zw_net_hold_awake(zw_net_t *net, uint64_t now, uint16_t node)
{
    net->now = now;
    const uint16_t before = net->hold_awake;
    net->hold_awake = node;
    if (before != 0u && before != node) {
        zw_health_set_busy(&net->health, before, zw_iv_asking(&net->iv, before), false);
    }
    if (node != 0u) {
        zw_health_set_busy(&net->health, node, zw_iv_asking(&net->iv, node), true);
    }
    zw_net_flush(net);
}

bool zw_net_resume_interview(zw_net_t *net, uint64_t now, uint16_t node)
{
    net->now = now;
    const bool ok = net->cfg.interview && zw_iv_start(&net->iv, now, node);
    if (ok) {
        zw_health_set_busy(&net->health, node, true, node == net->hold_awake);
    }
    zw_net_flush(net);
    return ok;
}

zw_host_t *zw_net_host(zw_net_t *net)
{
    return &net->host;
}

zw_s2_t *zw_net_s2(zw_net_t *net)
{
    return &net->s2;
}

zw_incl_t *zw_net_incl(zw_net_t *net)
{
    return &net->incl;
}

zw_remove_t *zw_net_rm(zw_net_t *net)
{
    return &net->rm;
}

zw_iv_t *zw_net_iv(zw_net_t *net)
{
    return &net->iv;
}

zw_health_t *zw_net_health(zw_net_t *net)
{
    return &net->health;
}

const zw_node_cache_t *zw_net_nodes(const zw_net_t *net)
{
    return &net->nodes;
}

zw_net_radio_t zw_net_radio(const zw_net_t *net)
{
    return net->radio;
}

const zw_net_stats_t *zw_net_stats(const zw_net_t *net)
{
    return &net->stats;
}
