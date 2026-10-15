/*
 * zw_net_power.c - the radio's transmit power (docs/features/36 network.md §12 "Power", phase 3;
 * serial-api.md §6.7): read it, and change it within the cap the owner keeps - the power the
 * radio had when the panel first read it, which Zooz sets to the legal value for its region.
 * The module applies a new power at its next start, so a Set it takes is followed by the
 * start-up (its step 12 is the soft reset) and a read once the link is up again.
 */
#include <string.h>

#include "zw_net_internal.h"

/* Z-Wave JS's limits: classic -10..+20 dBm (a signed byte without the 16-bit form); the Long
 * Range maximum 10, 14 or 20 dBm. */
#define PW_MIN_DBM10 (-100)
#define PW_MAX_DBM10 200
#define PW_READ_CLASSIC 0x01u
#define PW_READ_LR      0x02u
#define PW_SET          3u

static bool supported(const zw_net_t *net, uint8_t sub)
{
    const zw_module_facts_t *f = zw_host_facts(&net->host);
    const zw_api_setup_supported_cmds_t cmds = {
        .legacy_mask = f->setup_legacy,
        .extended_mask = f->setup_extended_len > 0u ? f->setup_extended : NULL,
        .extended_mask_len = f->setup_extended_len,
    };
    return zw_api_setup_supports(&cmds, sub);
}

static void emit(zw_net_t *net, zw_net_power_ev_t what)
{
    zw_net_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = ZW_NET_EV_POWER;
    ev.power_ev = what;
    ev.power = net->power;
    zw_net_emit(net, &ev);
}

/* The SerialAPISetup answer in a result; false when there is none to use. */
static bool setup_answer(const zw_tx_result_t *r, zw_api_api_setup_rsp_t *rsp)
{
    uint8_t refused = 0u;
    return r->status == ZW_TX_OK && r->response != NULL &&
           zw_api_api_setup_rsp_parse(r->response, r->response_len, rsp) &&
           !zw_api_setup_unsupported(rsp, &refused);
}

static void read_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    const uint8_t bit = (uint8_t)(tag & 0x3u);
    if ((net->power_out & bit) == 0u) {
        return; /* a read from before the link went (zw_net_power_lost) */
    }
    net->power_out = (uint8_t)(net->power_out & (uint8_t)~bit);
    zw_api_api_setup_rsp_t rsp;
    if (setup_answer(r, &rsp)) {
        if (bit == PW_READ_CLASSIC) {
            zw_api_setup_power_t p;
            if (zw_api_setup_power_parse(&rsp, &p)) {
                net->power.dbm10 = p.dbm10;
                net->power.cal_dbm10 = p.cal_dbm10;
                net->power.known = true;
            }
        } else if (zw_api_setup_lr_power_parse(&rsp, &net->power.lr_dbm10)) {
            net->power.lr_known = true;
        }
    }
    if (net->power_out == 0u) {
        emit(net, ZW_NET_POWER_READ);
    }
}

static bool submit(zw_net_t *net, uint32_t low, const uint8_t *payload, size_t len,
                   void (*done)(void *, uint32_t, const zw_tx_result_t *))
{
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_SERIAL_API_SETUP;
    req.payload = payload;
    req.payload_len = len;
    req.expect = ZW_EXPECT_RESPONSE;
    req.prio = ZW_PRIO_NODE;
    req.done = done;
    req.ctx = net;
    req.tag = zw_net_tag(NT_POWER, low);
    return zw_host_submit(&net->host, &req) == ZW_SUBMIT_OK;
}

bool zw_net_power_read(zw_net_t *net, uint64_t now)
{
    net->now = now;
    if (zw_host_link(&net->host) != ZW_LINK_UP || !net->checked || net->power_out != 0u) {
        return false;
    }
    const bool sixteen = supported(net, ZW_SETUP_GET_POWERLEVEL_16BIT);
    const uint8_t sub = sixteen ? ZW_SETUP_GET_POWERLEVEL_16BIT : ZW_SETUP_GET_POWERLEVEL;
    if (!supported(net, sub)) {
        return false;
    }
    static const uint8_t get16[] = {ZW_SETUP_GET_POWERLEVEL_16BIT};
    static const uint8_t get8[] = {ZW_SETUP_GET_POWERLEVEL};
    static const uint8_t get_lr[] = {ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER};
    net->power_reread = false;
    net->power.sixteen_bit = sixteen;
    if (!submit(net, PW_READ_CLASSIC, sixteen ? get16 : get8, 1u, read_done)) {
        return false;
    }
    net->power_out = PW_READ_CLASSIC;
    if (supported(net, ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER) &&
        submit(net, PW_READ_LR, get_lr, 1u, read_done)) {
        net->power_out = (uint8_t)(net->power_out | PW_READ_LR);
    }
    return true;
}

const zw_net_power_t *zw_net_power(const zw_net_t *net)
{
    return &net->power;
}

static void set_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_net_t *net = ctx;
    (void)tag;
    if (net->op != OP_POWER) {
        return;
    }
    net->op = OP_NONE;
    zw_api_api_setup_rsp_t rsp;
    const bool took = setup_answer(r, &rsp) && rsp.data_len >= 1u && rsp.data[0] != 0u;
    emit(net, took ? ZW_NET_POWER_SET_TOOK : ZW_NET_POWER_SET_REFUSED);
    if (took) {
        zw_net_restart_link(net); /* step 12's soft reset applies it */
        net->power_reread = true;
    }
}

zw_net_result_t zw_net_set_power(zw_net_t *net, uint64_t now, bool allowed, bool long_range,
                                 int16_t dbm10, int16_t cap_dbm10)
{
    net->now = now;
    const int refused = zw_net_op_refusal(net, allowed);
    if (refused != 0) {
        return zw_net_op_result(refused);
    }
    zw_net_power_t *pw = &net->power;
    uint8_t buf[8];
    size_t n;
    if (long_range) {
        if (!pw->lr_known || !supported(net, ZW_SETUP_SET_LONG_RANGE_MAXIMUM_TX_POWER) ||
            (dbm10 != 100 && dbm10 != 140 && dbm10 != 200)) {
            return ZW_NET_INVALID;
        }
        if (dbm10 == pw->lr_dbm10) {
            return ZW_NET_UNCHANGED;
        }
        n = zw_api_setup_set_lr_power_req_build(buf, sizeof buf, dbm10);
    } else {
        const uint8_t sub = pw->sixteen_bit ? ZW_SETUP_SET_POWERLEVEL_16BIT : ZW_SETUP_SET_POWERLEVEL;
        if (!pw->known || !supported(net, sub) || dbm10 < PW_MIN_DBM10 || dbm10 > PW_MAX_DBM10) {
            return ZW_NET_INVALID;
        }
        if (dbm10 == pw->dbm10) {
            return ZW_NET_UNCHANGED;
        }
        const zw_api_setup_power_t p = {.dbm10 = dbm10, .cal_dbm10 = pw->cal_dbm10};
        n = zw_api_setup_set_power_req_build(buf, sizeof buf, pw->sixteen_bit, &p);
    }
    if (dbm10 > cap_dbm10) {
        return ZW_NET_TOO_HIGH;
    }
    if (n == 0u) {
        return ZW_NET_INVALID;
    }
    net->op = OP_POWER;
    if (!submit(net, PW_SET, buf, n, set_done)) {
        net->op = OP_NONE;
        return ZW_NET_LINK_DOWN;
    }
    return ZW_NET_STARTED;
}

void zw_net_power_poll(zw_net_t *net)
{
    if (net->power_reread && net->op == OP_NONE) {
        (void)zw_net_power_read(net, net->now);
    }
}

void zw_net_power_lost(zw_net_t *net)
{
    net->power_out = 0u;
    net->power.known = false;
    net->power.lr_known = false;
}
