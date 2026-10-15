/*
 * net_harness.c - a panel for the network tests. See net_harness.h.
 */
#include "net_harness.h"

#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_cc.h"

harness_t H;

/* ---- The owner ---------------------------------------------------------------------------- */

static bool o_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&H.wire, bytes, len);
}

static void o_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    H.ups++;
    if (H.extra.link_up != NULL) {
        H.extra.link_up(H.extra.ctx, facts);
    }
}

static void o_link_down(void *ctx, const zw_link_down_t *why)
{
    (void)ctx;
    (void)why;
    if (H.extra.link_down != NULL) {
        H.extra.link_down(H.extra.ctx);
    }
}

static void o_restarted(void *ctx, uint8_t reason)
{
    (void)ctx;
    (void)reason;
    if (H.extra.link_down != NULL) {
        H.extra.link_down(H.extra.ctx);
    }
}

static void o_received(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    if (H.rx_n < H_RX && rx->len <= sizeof H.rx[0].cmd) {
        h_rx_t *r = &H.rx[H.rx_n++];
        r->node = rx->node;
        r->ep = rx->multi_channel ? rx->source_endpoint : 0u;
        r->security = rx->security;
        r->at = H.wire.now;
        r->len = rx->len;
        memcpy(r->cmd, rx->command, rx->len);
    }
    if (H.answer_supervision && rx->supervised) {
        /* command-classes.md §4.14: at once, with the Get's session and encapsulation */
        const zw_cc_supervision_report_t sr = {false, false, rx->supervision_session,
                                               ZW_SUPERVISION_STATUS_SUCCESS, 0u};
        uint8_t plain[8], mc[16];
        const uint8_t *out = plain;
        size_t n = zw_cc_supervision_report_build(plain, sizeof plain, &sr);
        if (rx->multi_channel) {
            n = zw_cc_wrap_multi_channel(mc, sizeof mc, rx->destination_endpoint,
                                         rx->source_endpoint, false, plain, n);
            out = mc;
        }
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, rx->node, rx->security,
                                                    out, n, H_SUPERVISION_TAG));
    }
    if (H.extra.received != NULL) {
        H.extra.received(H.extra.ctx, rx);
    }
}

static void o_frame(void *ctx, const zw_frame_t *frame)
{
    (void)ctx;
    if (H.frames_n < sizeof H.frames / sizeof H.frames[0]) {
        H.frames[H.frames_n].type = frame->type;
        H.frames[H.frames_n].func = frame->func;
        H.frames[H.frames_n].len = frame->payload_len;
        memcpy(H.frames[H.frames_n].payload, frame->payload,
               frame->payload_len < sizeof H.frames[0].payload ? frame->payload_len
                                                               : sizeof H.frames[0].payload);
        H.frames_n++;
    }
}

static void o_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    if (H.extra.send_done != NULL && H.extra.send_done(H.extra.ctx, tag, r)) {
        return; /* the other half's own */
    }
    if (H.done_n < 32u) {
        H.done_tag[H.done_n] = tag;
        H.done_r[H.done_n] = r;
        H.done_n++;
    }
}

static void o_incl(void *ctx, const zw_incl_event_t *ev)
{
    (void)ctx;
    if (H.incl_n < 64u) {
        H.incl[H.incl_n] = *ev;
        H.incl[H.incl_n].nif = NULL;
        H.incl_n++;
    }
    zw_incl_t *in = zw_net_incl(&H.net);
    if (H.on_incl != NULL) {
        H.on_incl(ev);
    }
    if (ev->kind == ZW_INCL_EV_GRANT && H.cancel_at_grant) {
        zw_incl_cancel(in, H.wire.now);
    } else if (ev->kind == ZW_INCL_EV_GRANT) {
        TEST_ASSERT_TRUE(zw_incl_grant(in, H.wire.now, H.grant != 0u ? H.grant : ev->requested));
    } else if (ev->kind == ZW_INCL_EV_PIN && !H.hold_pin) {
        TEST_ASSERT_TRUE(zw_incl_pin(in, H.wire.now, H.pin));
    }
}

static bool o_smart_find(void *ctx, const uint8_t nwi[4], bool long_range,
                         zw_incl_smartstart_t *out)
{
    (void)ctx;
    const int i = zw_prov_find_nwi(&H.prov, nwi, long_range);
    if (i < 0) {
        return false;
    }
    memcpy(out->dsk, H.prov.e[i].dsk, sizeof out->dsk);
    out->grant = H.prov.e[i].grant;
    out->long_range = H.prov.e[i].long_range;
    return true;
}

static void o_event(void *ctx, const zw_net_event_t *ev)
{
    (void)ctx;
    TEST_ASSERT_TRUE_MESSAGE(H.ev_n < H_EVENTS, "too many network events");
    H.ev_at[H.ev_n] = H.wire.now;
    H.ev[H.ev_n++] = *ev;
    if (ev->kind == ZW_NET_EV_SMARTSTART) {
        if (ev->smart == ZW_NET_SMART_JOINED) {
            (void)zw_prov_joined(&H.prov, ev->dsk, ev->node, ev->generation);
        } else {
            (void)zw_prov_failed(&H.prov, ev->dsk);
        }
        h_smart();
    } else if (ev->kind == ZW_NET_EV_NODE_REMOVED || ev->kind == ZW_NET_EV_NODE_GONE) {
        zw_prov_node_gone(&H.prov, ev->node, ev->generation);
        h_smart();
    }
    if (H.extra.event != NULL) {
        H.extra.event(H.extra.ctx, ev);
    }
}

static void o_store_nodes(void *ctx, const zw_node_cache_t *nodes, uint16_t generation)
{
    (void)ctx;
    H.blob_len = zw_node_cache_encode(nodes, H.blob, sizeof H.blob);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, H.blob_len, "the node cache did not fit the owner's blob");
    H.cfg.generation = generation;
    H.stores++;
}

static void o_store_keys(void *ctx, uint32_t home_id, const zw_s2_keys_t *keys)
{
    (void)ctx;
    H.cfg.keys = *keys;
    H.cfg.keys_bound = true;
    H.cfg.keys_home_id = home_id;
    H.key_stores++;
}

static bool o_quirk(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out)
{
    (void)ctx;
    (void)rec;
    if (H.has_quirk) {
        *out = H.quirk;
    }
    return H.has_quirk;
}

/* ---- The wire's view of the panel ----------------------------------------------------------- */

static void n_control(void *ctx, uint64_t now, uint8_t b)
{
    zw_net_on_control(ctx, now, b);
}

static void n_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    zw_net_on_frame(ctx, now, f);
}

static void n_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    zw_net_on_text(ctx, now, b, n);
}

static void n_poll(void *ctx, uint64_t now)
{
    zw_net_poll(ctx, now);
    if (H.extra.poll != NULL) {
        H.extra.poll(H.extra.ctx, now);
    }
}

static uint64_t n_deadline(void *ctx)
{
    const uint64_t t = zw_net_next_deadline(ctx);
    const uint64_t x = H.extra.deadline != NULL ? H.extra.deadline(H.extra.ctx) : UINT64_MAX;
    return x < t ? x : t;
}

/* ---- API ---------------------------------------------------------------------------------- */

void h_run(uint64_t t)
{
    fake_wire_run_until(&H.wire, t);
    TEST_ASSERT_FALSE_MESSAGE(H.wire.stuck, "the wire did not settle");
}

void h_run_for(uint64_t ms)
{
    h_run(H.wire.now + ms);
}

void h_boot(void)
{
    static const zw_net_port_t port = {
        .write = o_write,
        .link_up = o_link_up,
        .link_down = o_link_down,
        .restarted = o_restarted,
        .received = o_received,
        .send_done = o_send_done,
        .incl_event = o_incl,
        .event = o_event,
        .store_nodes = o_store_nodes,
        .store_keys = o_store_keys,
        .quirk = o_quirk,
        .frame = o_frame,
        .smartstart_find = o_smart_find,
    };
    const fake_host_t fh = {&H.net, n_control, n_frame, n_text, n_poll, n_deadline};
    const uint64_t t = H.wire.now;
    zw_net_init(&H.net, &port, &H.cfg, &H.crypto);
    if (H.blob_len > 0u) {
        TEST_ASSERT_TRUE(zw_net_load_nodes(&H.net, H.blob, H.blob_len));
    }
    if (H.extra.boot != NULL) {
        H.extra.boot(H.extra.ctx);
    }
    fake_wire_init(&H.wire, &H.zac, &fh, t);
    zw_net_start(&H.net, t);
    h_run(t + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
}

void h_init(void)
{
    memset(&H, 0, sizeof H);
    H.cfg.interview = true;
    H.allowed = true;
    zw_host_crypto_init(&H.hc, 0x686F757365u);
    H.crypto = zw_host_crypto_port(&H.hc);
    fake_zac93_init(&H.zac);
    h_boot();
}

void h_smart(void)
{
    if (H.smart) {
        zw_net_smartstart(&H.net, H.wire.now, zw_prov_waiting(&H.prov));
    }
}

void h_clear_events(void)
{
    H.ev_n = 0u;
}

const zw_net_event_t *h_find(zw_net_event_kind_t kind, uint16_t node)
{
    for (size_t i = H.ev_n; i > 0u; i--) {
        const zw_net_event_t *e = &H.ev[i - 1u];
        if (e->kind == kind && (node == 0u || e->node == node)) {
            return e;
        }
    }
    return NULL;
}

size_t h_count(zw_net_event_kind_t kind, uint16_t node)
{
    size_t n = 0u;
    for (size_t i = 0; i < H.ev_n; i++) {
        if (H.ev[i].kind == kind && (node == 0u || H.ev[i].node == node)) {
            n++;
        }
    }
    return n;
}

size_t h_rx_count(uint16_t node, uint8_t cc, uint8_t cmd)
{
    size_t n = 0u;
    for (size_t i = 0; i < H.rx_n; i++) {
        if (H.rx[i].node == node && H.rx[i].len >= 2u && H.rx[i].cmd[0] == cc &&
            H.rx[i].cmd[1] == cmd) {
            n++;
        }
    }
    return n;
}

uint16_t h_include(fake_zdev_t *d)
{
    if (d->zac == NULL) {
        TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, d));
    }
    H.pin = H.wrong_pin ? (uint16_t)(d->pin ^ 0x0101u) : d->pin;
    const size_t before = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED,
                          zw_net_add_device(&H.net, H.wire.now, H.allowed, 2u, 0u));
    h_run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&H.zac, H.wire.now, d));
    for (unsigned i = 0; i < 400u; i++) {
        h_run_for(100u);
        for (size_t k = before; k < H.incl_n; k++) {
            if (H.incl[k].kind == ZW_INCL_EV_DONE) {
                return H.incl[k].result == ZW_INCL_ADDED ? H.incl[k].node : 0u;
            }
        }
    }
    TEST_FAIL_MESSAGE("the inclusion did not end");
    return 0u;
}

bool h_until_ready(uint16_t node, uint64_t ms)
{
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end) {
        if (h_find(ZW_NET_EV_NODE_READY, node) != NULL) {
            return true;
        }
        h_run_for(100u);
    }
    return h_find(ZW_NET_EV_NODE_READY, node) != NULL;
}

const zw_node_record_t *h_rec(uint16_t node)
{
    return zw_node_find_const(zw_net_nodes(&H.net), node);
}

void h_device(fake_zdev_t *d, uint64_t seed, bool sleeps)
{
    fake_zdev_init(d, seed);
    d->m.answers = true;
    d->m.sleeps = sleeps;
}
