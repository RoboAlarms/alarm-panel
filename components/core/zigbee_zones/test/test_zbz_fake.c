/*
 * test_zbz_fake.c - the fake radio itself (fake/zbz_fake_radio.h; docs/features/20 testing.md §2,
 * §7, §8): what it sends are real ZCL and ZDO frames (decoded here with zbz_zcl.h and the Zigbee
 * specification's layouts), a virtual device answers through the same decoders, joins and leaves
 * are reported as the radio side reports them, every fault knob does what it says, a sleepy
 * device answers only inside its wake window, and what a device says reaches the zone source.
 *
 * Golden bytes: the IAS Zone CIE write and Enroll Response are testing.md §3's rows; the ZDO
 * requests follow the Zigbee specification (2.4.3.1: Node_Desc_req is the transaction sequence
 * number and NWKAddrOfInterest; Device_annce is nwk | ieee | capability). Addresses are the
 * signature rows' (fictional, locally administered).
 *
 * Requirements: ZB-002 (the join window), ZB-003 (IAS Zone frames, enrollment), ZB-005 and ZB-014
 * (supervision, sleepy devices), ZB-006 (the radio unplugged), ZB-011 (link quality), ZB-012 (the
 * setup's questions, faults and replays), ZB-013 (leave and forget), ZB-015 and ZB-021 (install
 * codes, how a device joined), ZB-016 and ZB-017 (channel, network reset), ZB-022 (IAS test mode),
 * ZB-023 and ZB-024 (siren and keypad commands).
 */
#include <string.h>

#include "unity.h"
#include "zbz_fake_radio.h"
#include "zigbee_zones/zbz_zcl.h"
#include "zone_hub/zsrc_zigbee.h"

#define REPS_MAX 512u

#define CONTACT "sonoff_snzb_04pr2" /* a sleepy IAS contact with Poll Control */
#define MAGNET  "aqara_magnet_agl02" /* IAS without Poll Control */
#define MOTION  "aqara_motion_agl04" /* Occupancy, not IAS */
#define TEMP    "sonoff_snzb_02d"
#define NO_IDENT "third_reality_3rds17bz" /* IAS without Identify */
#define SIREN   "frient_sirzb_111"   /* a router; IAS WD on endpoint 43 */
#define KEYPAD  "frient_kepzb_110"   /* IAS ACE client on endpoint 44 */

static zbf_radio_t R;
static zsrc_zb_t Z;
static zsrc_zb_radio_t H;
static uint64_t NOW;
static zsrc_zb_report_t REPS[REPS_MAX];
static uint64_t REP_AT[REPS_MAX];
static size_t NREPS;

static void on_report(void *ctx, const zsrc_zb_report_t *r, uint64_t now)
{
    (void)ctx;
    if (NREPS < REPS_MAX) {
        REPS[NREPS] = *r;
        REP_AT[NREPS] = now;
        NREPS++;
    }
    (void)zsrc_zb_on_report(&Z, r, now); /* as zbz_panel_on_report does first */
}

static const zbf_sink_t SINK = {.report = on_report};

void setUp(void)
{
    NOW = 1000000u;
    NREPS = 0u;
    zbf_init(&R, &Z, &SINK, NOW);
    H = zbf_hooks(&R);
    zsrc_zb_init(&Z, &H, NULL);
    zbf_up(&R);
}

void tearDown(void) {}

static void run(uint64_t ms)
{
    NOW += ms;
    zbf_advance(&R, NOW);
}

static void clear(void)
{
    NREPS = 0u;
}

static size_t count_of(uint8_t kind)
{
    size_t n = 0;
    for (size_t i = 0; i < NREPS; i++) {
        n += REPS[i].kind == kind ? 1u : 0u;
    }
    return n;
}

static const zsrc_zb_report_t *last_of(uint8_t kind)
{
    for (size_t i = NREPS; i > 0u; i--) {
        if (REPS[i - 1u].kind == kind) {
            return &REPS[i - 1u];
        }
    }
    return NULL;
}

static size_t index_of_done(uint32_t tag)
{
    for (size_t i = 0; i < NREPS; i++) {
        if (REPS[i].kind == ZSRC_ZB_REP_DONE && REPS[i].tag == tag) {
            return i;
        }
    }
    return REPS_MAX;
}

static const zsrc_zb_report_t *done_of(uint32_t tag)
{
    size_t i = index_of_done(tag);
    return i < REPS_MAX ? &REPS[i] : NULL;
}

static size_t dones_of(uint32_t tag)
{
    size_t n = 0;
    for (size_t i = 0; i < NREPS; i++) {
        n += (REPS[i].kind == ZSRC_ZB_REP_DONE && REPS[i].tag == tag) ? 1u : 0u;
    }
    return n;
}

static zbf_dev_t *joined(const char *name)
{
    zbf_dev_t *d = zbf_add(&R, name);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(H.permit_join(H.ctx, (uint16_t)ZSRC_ZB_LEARN_S));
    TEST_ASSERT_TRUE(zbf_join(&R, d));
    run(100u);
    return d;
}

/* The newest frame from a device. */
static const zbf_frame_t *last_rx(uint64_t ieee)
{
    for (size_t i = zbf_log_count(&R); i > 0u; i--) {
        const zbf_frame_t *f = zbf_log_at(&R, i - 1u);
        if (f->rx && f->ieee == ieee) {
            return f;
        }
    }
    return NULL;
}

/* Basic's manufacturer and model, from the first endpoint that has Basic. */
static bool read_basic(zbf_dev_t *d, uint32_t tag)
{
    static const uint16_t attrs[] = {0x0004u, 0x0005u};
    for (uint8_t i = 0; i < d->ep_count; i++) {
        for (uint8_t k = 0; k < d->ep[i].sd.in_count; k++) {
            if (d->ep[i].sd.in[k] == ZSRC_ZB_ZCL_BASIC) {
                return H.read_attributes(H.ctx, d->ieee, d->ep[i].sd.endpoint, ZSRC_ZB_ZCL_BASIC,
                                         attrs, 2u, tag);
            }
        }
    }
    return false;
}

static zsrc_zb_desc_t desc_of(const zbf_dev_t *d, uint8_t ep, uint16_t clusters, uint16_t zone)
{
    zsrc_zb_desc_t x;
    memset(&x, 0, sizeof(x));
    x.ieee = d->ieee;
    x.nwk = d->nwk;
    x.endpoint = ep;
    x.clusters = clusters;
    x.zone_type = zone;
    x.router = !d->sleepy;
    return x;
}

/* ---- real bytes ---------------------------------------------------------------------------------- */

/* ZB-003, ZB-012 (testing.md §3): the panel's frames are the bytes the specification lays out,
 * and the device's answer decodes with the firmware's decoders. */
static void test_frames_are_real_zcl_and_zdo_bytes(void)
{
    zbf_dev_t *d = joined(CONTACT);

    /* Device_annce: nwk | ieee | capability (Zigbee 2.4.3.1.11). */
    bool announced = false;
    for (size_t i = 0; i < zbf_log_count(&R); i++) {
        const zbf_frame_t *f = zbf_log_at(&R, i);
        if (f->rx && f->kind == ZBF_FRAME_ZDO && f->cluster == ZBF_ZDO_ANNOUNCE) {
            TEST_ASSERT_EQUAL_UINT8(12u, f->len);
            TEST_ASSERT_EQUAL_HEX16(d->nwk, (uint16_t)((unsigned)f->bytes[1] | ((unsigned)f->bytes[2] << 8)));
            uint64_t ieee = 0;
            for (unsigned k = 0; k < 8u; k++) {
                ieee |= (uint64_t)f->bytes[3u + k] << (8u * k);
            }
            TEST_ASSERT_EQUAL_HEX64(d->ieee, ieee);
            TEST_ASSERT_EQUAL_HEX8(d->node.mac_caps, f->bytes[11]);
            announced = true;
        }
    }
    TEST_ASSERT_TRUE(announced);

    /* Read Attributes: a global frame, client to server, Default Response asked for. */
    zbf_set_tsn(&R, 0x42u, 0x17u);
    TEST_ASSERT_TRUE(read_basic(d, 7u));
    const zbf_frame_t *tx = zbf_last_tx(&R, d->ieee);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8(ZBF_FRAME_ZCL, tx->kind);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_BASIC, tx->cluster);
    const uint8_t read_golden[] = {0x00, 0x42, 0x00, 0x04, 0x00, 0x05, 0x00};
    TEST_ASSERT_EQUAL_UINT8(sizeof(read_golden), tx->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(read_golden, tx->bytes, sizeof(read_golden));
    run(1000u);
    const zbf_frame_t *rx = last_rx(d->ieee);
    TEST_ASSERT_NOT_NULL(rx);
    zsrc_zb_zcl_t z;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_global(rx->bytes, rx->len, rx->cluster, &z));
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_READ_ATTR_RSP, z.command);
    TEST_ASSERT_TRUE(z.server_to_client);
    TEST_ASSERT_EQUAL_UINT16(0x42u, z.tsn);
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zbz_zcl_iter(&it, &z);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0004u, a.attr);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_TYPE_STRING, a.type);
    TEST_ASSERT_EQUAL_UINT8(strlen(d->manufacturer), a.len);
    TEST_ASSERT_EQUAL_MEMORY(d->manufacturer, a.value, a.len);
    TEST_ASSERT_TRUE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_HEX16(0x0005u, a.attr);
    TEST_ASSERT_FALSE(zbz_zcl_next_attr(&it, &a));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, it.err);
    /* ... and the panel side made of it a REP_BASIC for the request, then its end. */
    const zsrc_zb_report_t *b = last_of(ZSRC_ZB_REP_BASIC);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(7u, b->tag);
    TEST_ASSERT_EQUAL_STRING(d->manufacturer, b->u.basic.manufacturer);
    TEST_ASSERT_EQUAL_STRING(d->model, b->u.basic.model);
    const zsrc_zb_report_t *done = done_of(7u);
    TEST_ASSERT_NOT_NULL(done);
    TEST_ASSERT_TRUE(index_of_done(7u) > (size_t)(b - REPS)); /* after what it answered */
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_BASIC, done->u.done.step);

    /* testing.md §3: Write IAS CIE Address 00 57 02 | 10 00 F0 + 8 address bytes, low first.
     * (It doesn't ask to enroll by itself here: the source would answer with its own zone id.) */
    d->auto_enroll = false;
    zbf_set_tsn(&R, 0x57u, 0x17u);
    TEST_ASSERT_TRUE(H.ias_cie_write(H.ctx, d->ieee, 1u));
    tx = zbf_last_tx(&R, d->ieee);
    const uint8_t cie_golden[] = {0x00, 0x57, 0x02, 0x10, 0x00, 0xF0, 0xF0, 0x00,
                                  0x00, 0x00, 0x00, 0xCD, 0xAB, 0x02};
    TEST_ASSERT_EQUAL_UINT8(sizeof(cie_golden), tx->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cie_golden, tx->bytes, sizeof(cie_golden));
    /* testing.md §3: Enroll Response 01 57 00 | 00 05 (success, zone id 5). */
    zbf_set_tsn(&R, 0x57u, 0x17u);
    TEST_ASSERT_TRUE(H.ias_enroll_response(H.ctx, d->ieee, 1u, 5u));
    tx = zbf_last_tx(&R, d->ieee);
    const uint8_t enroll_golden[] = {0x01, 0x57, 0x00, 0x00, 0x05};
    TEST_ASSERT_EQUAL_UINT8(sizeof(enroll_golden), tx->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(enroll_golden, tx->bytes, sizeof(enroll_golden));
    /* Node_Desc_req: tsn | NWKAddrOfInterest. */
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 8u));
    tx = zbf_last_tx(&R, d->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZBF_FRAME_ZDO, tx->kind);
    TEST_ASSERT_EQUAL_HEX16(ZBF_ZDO_NODE_DESC, tx->cluster);
    const uint8_t nd_golden[] = {0x17, (uint8_t)(d->nwk & 0xFFu), (uint8_t)(d->nwk >> 8)};
    TEST_ASSERT_EQUAL_UINT8(sizeof(nd_golden), tx->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(nd_golden, tx->bytes, sizeof(nd_golden));
    run(1000u);
    /* The device took both IAS frames as the specification says. */
    const zbf_ep_t *e = zbf_ep(d, 1u);
    TEST_ASSERT_EQUAL_HEX64(ZBF_PANEL_IEEE, e->cie);
    TEST_ASSERT_TRUE(e->enrolled);
    TEST_ASSERT_EQUAL_UINT8(5u, e->zone_id);
    const zsrc_zb_report_t *nd = last_of(ZSRC_ZB_REP_NODE_DESC);
    TEST_ASSERT_NOT_NULL(nd);
    TEST_ASSERT_EQUAL_UINT32(8u, nd->tag);
    TEST_ASSERT_EQUAL_UINT8(d->node.logical_type, nd->u.node.logical_type);
    TEST_ASSERT_EQUAL_HEX8(d->node.mac_caps, nd->u.node.mac_caps);
    TEST_ASSERT_EQUAL_HEX16(d->node.manufacturer_code, nd->u.node.manufacturer_code);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(8u)->u.done.result);
}

/* ZB-012: Configure Reporting is decoded by the device; an attribute it can't report is
 * UNSUPPORTED (not a failure), one it can is OK. */
static void test_configure_reporting_is_decoded_by_the_device(void)
{
    zbf_dev_t *d = joined(CONTACT);
    const zsrc_zb_reporting_t battery = {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x0021u,
                                         .type = ZBZ_ZCL_TYPE_U8, .min_s = 3600u,
                                         .max_s = 43200u, .change = 4u};
    const zsrc_zb_reporting_t bogus = {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x0099u,
                                       .type = ZBZ_ZCL_TYPE_U8, .min_s = 1u, .max_s = 60u};
    TEST_ASSERT_TRUE(H.configure_reporting(H.ctx, d->ieee, 1u, &battery, 21u));
    TEST_ASSERT_TRUE(H.configure_reporting(H.ctx, d->ieee, 1u, &bogus, 22u));
    TEST_ASSERT_FALSE(H.configure_reporting(H.ctx, d->ieee, 1u, NULL, 23u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(21u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_REPORTING, done_of(21u)->u.done.step);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(22u)->u.done.result);
}

/* ZB-012, ZB-014: bind and identify answer by what the endpoint has. */
static void test_bind_and_identify_follow_the_clusters(void)
{
    zbf_dev_t *d = joined(NO_IDENT);
    TEST_ASSERT_TRUE(H.bind(H.ctx, d->ieee, 1u, ZSRC_ZB_ZCL_POWER_CFG, 31u));
    TEST_ASSERT_TRUE(H.bind(H.ctx, d->ieee, 1u, ZSRC_ZB_ZCL_ON_OFF, 32u));
    TEST_ASSERT_TRUE(H.identify(H.ctx, d->ieee, 1u, 10u, 33u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(31u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_BIND, done_of(31u)->u.done.step);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(32u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(33u)->u.done.result);
    /* Bind_req's layout: src ieee | src ep | cluster | mode 3 | dst ieee | dst ep. */
    const zbf_frame_t *tx = NULL;
    for (size_t i = 0; i < zbf_log_count(&R); i++) {
        const zbf_frame_t *f = zbf_log_at(&R, i);
        if (!f->rx && f->kind == ZBF_FRAME_ZDO && f->cluster == ZBF_ZDO_BIND) {
            tx = f;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8(22u, tx->len);
    TEST_ASSERT_EQUAL_HEX8(0x01u, tx->bytes[9]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, tx->bytes[10]); /* 0x0001, low first */
    TEST_ASSERT_EQUAL_HEX8(0x00u, tx->bytes[11]);
    TEST_ASSERT_EQUAL_HEX8(0x03u, tx->bytes[12]);
    TEST_ASSERT_EQUAL_HEX8(0xF0u, tx->bytes[13]); /* the panel's address, low first */
    TEST_ASSERT_EQUAL_HEX8(ZBF_PANEL_EP, tx->bytes[21]);
    /* A device that has Identify identifies. */
    zbf_dev_t *c = joined(CONTACT);
    TEST_ASSERT_TRUE(H.identify(H.ctx, c->ieee, 1u, 10u, 34u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(34u)->u.done.result);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IDENTIFY, zbf_ep(c, 1u)->last_cmd.cluster);
}

/* ---- joins and leaves ---------------------------------------------------------------------------- */

/* ZB-002, ZB-015, ZB-021: a device joins only during the window; with its install code when the
 * trust center holds it, else the standard key; refused when only codes may join. */
static void test_joins_record_how_the_device_joined(void)
{
    static const uint8_t code[18] = {0x83, 0xFE, 0xD3, 0x40, 0x7A, 0x93, 0x97, 0x23, 0xA5,
                                     0xC6, 0x39, 0xB2, 0x69, 0x16, 0xD5, 0x05, 0xC3, 0xB5};
    zbf_dev_t *d = zbf_add(&R, CONTACT);
    TEST_ASSERT_FALSE(zbf_join(&R, d)); /* the window is closed */
    TEST_ASSERT_EQUAL_size_t(0u, count_of(ZSRC_ZB_REP_JOINED));
    TEST_ASSERT_TRUE(H.permit_join(H.ctx, 180u));
    TEST_ASSERT_TRUE(zbf_join(&R, d));
    TEST_ASSERT_FALSE(zbf_join(&R, d)); /* already in */
    run(100u);
    const zsrc_zb_report_t *j = last_of(ZSRC_ZB_REP_JOINED);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_HEX64(d->ieee, j->ieee);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, j->u.joined.join);
    TEST_ASSERT_FALSE(j->u.joined.rejoin);
    const zsrc_zb_report_t *an = last_of(ZSRC_ZB_REP_ANNOUNCE);
    TEST_ASSERT_NOT_NULL(an);
    TEST_ASSERT_TRUE(an > j); /* the announce follows the join */
    TEST_ASSERT_EQUAL_HEX8(d->node.mac_caps, an->u.announce.mac_caps);

    /* With an install code the trust center holds. */
    zbf_dev_t *m = zbf_add(&R, MAGNET);
    zbf_set_code(m, code, sizeof(code));
    TEST_ASSERT_FALSE(H.install_code(H.ctx, m->ieee, code, 7u)); /* not a code length */
    TEST_ASSERT_TRUE(H.install_code(H.ctx, m->ieee, code, sizeof(code)));
    TEST_ASSERT_TRUE(H.codes_only(H.ctx, true));
    TEST_ASSERT_TRUE(zbf_join(&R, m));
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, last_of(ZSRC_ZB_REP_JOINED)->u.joined.join);

    /* Only codes, and none for this one: refused. */
    zbf_dev_t *o = zbf_add(&R, MOTION);
    TEST_ASSERT_FALSE(zbf_join(&R, o));
    run(100u);
    const zsrc_zb_report_t *ref = last_of(ZSRC_ZB_REP_JOIN_REFUSED);
    TEST_ASSERT_NOT_NULL(ref);
    TEST_ASSERT_EQUAL_HEX64(o->ieee, ref->ieee);
    TEST_ASSERT_EQUAL_UINT8(1u, ref->u.refused.why);
    TEST_ASSERT_FALSE(o->joined);
    /* A code taken back: the next join is the standard key again. */
    TEST_ASSERT_TRUE(H.codes_only(H.ctx, false));
    TEST_ASSERT_TRUE(H.install_code(H.ctx, m->ieee, NULL, 0u));
    zbf_leave(&R, m, false);
    TEST_ASSERT_TRUE(zbf_join(&R, m));
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, last_of(ZSRC_ZB_REP_JOINED)->u.joined.join);
    /* The window closes. */
    run((uint64_t)ZSRC_ZB_LEARN_S * 1000u);
    TEST_ASSERT_FALSE(zbf_join(&R, o));
}

/* ZB-013: it leaves by itself, or when asked (REP_DONE, then REP_LEFT); a rejoin keeps its key
 * and brings a new short address; forgotten, its frames are no longer the network's. */
static void test_leave_rejoin_and_forget(void)
{
    zbf_dev_t *d = joined(SIREN);
    clear();
    zbf_leave(&R, d, true);
    run(100u);
    const zsrc_zb_report_t *l = last_of(ZSRC_ZB_REP_LEFT);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_TRUE(l->u.left.rejoin);
    TEST_ASSERT_FALSE(l->u.left.asked);

    clear();
    TEST_ASSERT_TRUE(zbf_rejoin(&R, d, 0x2B01u));
    run(100u);
    const zsrc_zb_report_t *j = last_of(ZSRC_ZB_REP_JOINED);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_TRUE(j->u.joined.rejoin);
    TEST_ASSERT_EQUAL_HEX16(0x2B01u, last_of(ZSRC_ZB_REP_ANNOUNCE)->nwk);

    clear();
    TEST_ASSERT_TRUE(H.leave(H.ctx, d->ieee, d->nwk, 41u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(41u)->u.done.result);
    l = last_of(ZSRC_ZB_REP_LEFT);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_TRUE(l->u.left.asked);
    TEST_ASSERT_EQUAL_UINT32(41u, l->tag);
    TEST_ASSERT_TRUE(index_of_done(41u) < (size_t)(l - REPS));
    TEST_ASSERT_FALSE(d->joined);
    /* Gone: nothing answers any more. */
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 42u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(42u)->u.done.result);

    /* Forget: synchronous, and its frames stop. */
    zbf_dev_t *c = joined(CONTACT);
    TEST_ASSERT_TRUE(H.forget(H.ctx, c->ieee));
    TEST_ASSERT_FALSE(c->joined);
    TEST_ASSERT_FALSE(zbf_trip(&R, c, true));
}

/* ---- the source ----------------------------------------------------------------------------------- */

/* ZB-003, ZB-004, ZB-011: what a device says reaches the zone source as the radio side says it. */
static void test_reports_reach_the_source(void)
{
    zbf_dev_t *d = joined(CONTACT);
    const zsrc_zb_desc_t x = desc_of(d, 1u, ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_POWER_CFG,
                                     ZSRC_ZB_ZONE_TYPE_CONTACT);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &x, NOW));
    zbf_set_link(d, 77u, -70);
    TEST_ASSERT_TRUE(zbf_trip(&R, d, true));
    run(100u);
    const zsrc_zb_device_t *zd = zsrc_zb_device(&Z, d->ieee, 1u);
    TEST_ASSERT_NOT_NULL(zd);
    TEST_ASSERT_TRUE(zd->status_known);
    TEST_ASSERT_TRUE((zd->status & ZSRC_ZB_IAS_ALARM1) != 0u);
    TEST_ASSERT_EQUAL_UINT8(77u, zd->lqi);
    TEST_ASSERT_EQUAL_INT8(-70, zd->rssi);
    const zsrc_zb_report_t *link = last_of(ZSRC_ZB_REP_LINK);
    TEST_ASSERT_NOT_NULL(link);
    TEST_ASSERT_EQUAL_UINT8(77u, link->u.link.lqi);
    TEST_ASSERT_TRUE(zbf_trip(&R, d, false));
    TEST_ASSERT_TRUE(zbf_set_battery(&R, d, 40u));
    run(100u);
    TEST_ASSERT_TRUE((zd->status & ZSRC_ZB_IAS_ALARM1) == 0u);
    TEST_ASSERT_EQUAL_UINT8(40u, zd->battery_pct);
    /* The Zone Status Change Notification as it went: 09 tsn 00 | status ext zone-id delay. */
    TEST_ASSERT_TRUE(zbf_ias_status(&R, d, 1u, ZSRC_ZB_IAS_TAMPER));
    const zbf_frame_t *f = last_rx(d->ieee);
    zsrc_zb_zcl_t z;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(f->bytes, f->len, f->cluster, &z));
    zbz_ias_status_t st;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&z, &st));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_IAS_TAMPER, st.status);
    TEST_ASSERT_FALSE(zbf_ias_status(&R, d, 9u, 0u)); /* no such endpoint */

    /* Occupancy and temperature, not IAS. */
    zbf_dev_t *m = joined(MOTION);
    const zsrc_zb_desc_t mx = desc_of(m, 1u, ZSRC_ZB_CL_OCCUPANCY, ZSRC_ZB_ZONE_TYPE_NONE);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &mx, NOW));
    TEST_ASSERT_TRUE(zbf_trip(&R, m, true));
    zbf_dev_t *t = joined(TEMP);
    const zsrc_zb_desc_t tx = desc_of(t, 1u, ZSRC_ZB_CL_TEMP, ZSRC_ZB_ZONE_TYPE_NONE);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &tx, NOW));
    TEST_ASSERT_TRUE(zbf_set_temperature(&R, t, -150));
    TEST_ASSERT_FALSE(zbf_set_temperature(&R, m, 2000)); /* it has no thermometer */
    run(100u);
    TEST_ASSERT_TRUE(zsrc_zb_device(&Z, m->ieee, 1u)->on);
    TEST_ASSERT_EQUAL_INT32(-150, zsrc_zb_device(&Z, t->ieee, 1u)->value_x100);
}

/* ZB-024, ZB-023: a keypad's command reaches the policy raw (REP_ZCL); the panel's answer to it
 * asks no Default Response and ends when delivered; a siren's warning is confirmed. */
static void test_keypad_and_siren_commands(void)
{
    zbf_dev_t *k = joined(KEYPAD);
    zsrc_zb_zcl_t arm;
    memset(&arm, 0, sizeof(arm));
    arm.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    arm.command = 0x00u; /* Arm: mode, code, zone id */
    arm.want_response = true;
    arm.tsn = ZSRC_ZB_TSN_NEW;
    const uint8_t payload[] = {0x03, 0x00, 0x00};
    memcpy(arm.payload, payload, sizeof(payload));
    arm.len = sizeof(payload);
    clear();
    TEST_ASSERT_TRUE(zbf_send(&R, k, 44u, &arm));
    TEST_ASSERT_FALSE(zbf_send(&R, k, 1u, NULL));
    TEST_ASSERT_FALSE(zbf_send(&R, k, 9u, &arm)); /* no such endpoint */
    run(100u);
    const zsrc_zb_report_t *z = last_of(ZSRC_ZB_REP_ZCL);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQUAL_UINT8(44u, z->endpoint);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ACE, z->u.zcl.cluster);
    TEST_ASSERT_EQUAL_HEX8(0x00u, z->u.zcl.command);
    TEST_ASSERT_FALSE(z->u.zcl.server_to_client);
    TEST_ASSERT_EQUAL_UINT8(sizeof(payload), z->u.zcl.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, z->u.zcl.payload, sizeof(payload));
    /* The Arm Response, with the request's sequence number, no Default Response asked. */
    zsrc_zb_zcl_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    rsp.command = 0x00u;
    rsp.server_to_client = true;
    rsp.want_response = false;
    rsp.tsn = z->u.zcl.tsn;
    rsp.payload[0] = 0x03u;
    rsp.len = 1u;
    TEST_ASSERT_TRUE(H.zcl_command(H.ctx, k->ieee, 44u, &rsp, 51u));
    const zbf_frame_t *tx = zbf_last_tx(&R, k->ieee);
    TEST_ASSERT_EQUAL_HEX8(0x19u, tx->bytes[0]); /* cluster | server to client | no default rsp */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)z->u.zcl.tsn, tx->bytes[1]);
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(51u)->u.done.result);
    TEST_ASSERT_EQUAL_HEX8(0x03u, zbf_ep(k, 44u)->last_cmd.payload[0]);

    zbf_dev_t *s = joined(SIREN);
    zsrc_zb_zcl_t warn;
    memset(&warn, 0, sizeof(warn));
    warn.cluster = ZSRC_ZB_ZCL_IAS_WD;
    warn.command = 0x00u; /* Start Warning (testing.md §4's spec-layout frame) */
    warn.want_response = true;
    warn.tsn = ZSRC_ZB_TSN_NEW;
    const uint8_t w[] = {0x16, 0x3C, 0x00, 0x32, 0x02};
    memcpy(warn.payload, w, sizeof(w));
    warn.len = sizeof(w);
    clear();
    TEST_ASSERT_TRUE(H.zcl_command(H.ctx, s->ieee, 43u, &warn, 52u));
    run(100u);
    TEST_ASSERT_EQUAL_UINT32(1u, zbf_ep(s, 43u)->warnings);
    const zsrc_zb_report_t *conf = last_of(ZSRC_ZB_REP_ZCL);
    TEST_ASSERT_NOT_NULL(conf);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_DEFAULT_RSP, conf->u.zcl.command);
    TEST_ASSERT_EQUAL_HEX8(0x00u, conf->u.zcl.payload[0]); /* the command it confirms */
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_SUCCESS, conf->u.zcl.payload[1]);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(52u)->u.done.result);
    /* The same warning to an endpoint without IAS WD: the cluster isn't there. */
    TEST_ASSERT_TRUE(H.zcl_command(H.ctx, k->ieee, 44u, &warn, 53u));
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(53u)->u.done.result);
}

/* ---- fault knobs ---------------------------------------------------------------------------------- */

/* ZB-012 (testing.md §7 lossy): the next N requests are lost, each ends TIMEOUT when its time
 * is up; the one after goes through. Faults are per step. */
static void test_fault_drop_is_per_step(void)
{
    zbf_dev_t *d = joined(SIREN);
    const zbf_fault_t drop2 = {.drop = 2u};
    zbf_fault(d, ZSRC_ZB_STEP_NODE_DESC, &drop2);
    const uint64_t sent = NOW;
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 1u));
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, d->ieee, d->nwk, 2u)); /* another step: untouched */
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(1u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_NODE_DESC, done_of(1u)->u.done.step);
    TEST_ASSERT_EQUAL_UINT64(sent + ZBF_TIMEOUT_MS, REP_AT[index_of_done(1u)]);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(2u)->u.done.result);
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 3u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(3u)->u.done.result);
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 4u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(4u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(0u, d->fault[ZSRC_ZB_STEP_NODE_DESC].drop);
    /* Device-wide (STEP_NONE): every step without its own. */
    const zbf_fault_t drop1 = {.drop = 1u};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, &drop1);
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, d->ieee, d->nwk, 5u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(5u)->u.done.result);
    zbf_fault(d, 99u, &drop1); /* not a step: ignored */
    zbf_fault(d, ZSRC_ZB_STEP_NONE, NULL);
}

/* ZB-012: a late answer; one later than the request's time comes after its TIMEOUT - a ZCL one
 * then reaches the panel as unsolicited (tag 0), a ZDO one is dropped by the stack. */
static void test_fault_delay(void)
{
    zbf_dev_t *d = joined(SIREN);
    const zbf_fault_t late = {.delay_ms = 3000u};
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &late);
    const uint64_t sent = NOW;
    TEST_ASSERT_TRUE(read_basic(d, 1u));
    run(2000u);
    TEST_ASSERT_NULL(done_of(1u));
    run(2000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(1u)->u.done.result);
    TEST_ASSERT_TRUE(REP_AT[index_of_done(1u)] >= sent + 3000u);

    const zbf_fault_t too_late = {.delay_ms = ZBF_TIMEOUT_MS + 2000u};
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &too_late);
    zbf_fault(d, ZSRC_ZB_STEP_NODE_DESC, &too_late);
    clear();
    TEST_ASSERT_TRUE(read_basic(d, 2u));
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 3u));
    run(ZBF_TIMEOUT_MS + 5000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(2u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(3u)->u.done.result);
    const zsrc_zb_report_t *b = last_of(ZSRC_ZB_REP_BASIC);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(0u, b->tag);
    TEST_ASSERT_TRUE(index_of_done(2u) < (size_t)(b - REPS));
    TEST_ASSERT_EQUAL_size_t(0u, count_of(ZSRC_ZB_REP_NODE_DESC));
}

/* ZB-012 (testing.md §7 replays): an answer sent twice with one sequence number ends its request
 * once; the second copy is unsolicited. */
static void test_fault_duplicate(void)
{
    zbf_dev_t *d = joined(SIREN);
    const zbf_fault_t twice = {.duplicate = true};
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &twice);
    clear();
    zbf_log_clear(&R);
    TEST_ASSERT_TRUE(read_basic(d, 1u));
    run(1000u);
    TEST_ASSERT_EQUAL_size_t(1u, dones_of(1u));
    TEST_ASSERT_EQUAL_size_t(2u, count_of(ZSRC_ZB_REP_BASIC));
    const zsrc_zb_report_t *b = last_of(ZSRC_ZB_REP_BASIC);
    TEST_ASSERT_EQUAL_UINT32(0u, b->tag);
    /* Two frames from it, byte for byte the same. */
    const zbf_frame_t *f[2] = {NULL, NULL};
    size_t n = 0;
    for (size_t i = 0; i < zbf_log_count(&R) && n < 2u; i++) {
        if (zbf_log_at(&R, i)->rx) {
            f[n++] = zbf_log_at(&R, i);
        }
    }
    TEST_ASSERT_EQUAL_size_t(2u, n);
    TEST_ASSERT_EQUAL_UINT8(f[0]->len, f[1]->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f[0]->bytes, f[1]->bytes, f[0]->len);
}

/* ZB-012: an answer with another sequence number matches nothing - its data still arrives
 * (unsolicited), the request times out; a ZDO one is dropped by the stack. */
static void test_fault_other_tsn(void)
{
    zbf_dev_t *d = joined(SIREN);
    const zbf_fault_t other = {.other_tsn = true};
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &other);
    zbf_fault(d, ZSRC_ZB_STEP_NODE_DESC, &other);
    clear();
    TEST_ASSERT_TRUE(read_basic(d, 1u));
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 2u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(1u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(2u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT32(0u, last_of(ZSRC_ZB_REP_BASIC)->tag);
    TEST_ASSERT_EQUAL_size_t(0u, count_of(ZSRC_ZB_REP_NODE_DESC));
}

/* ZB-012: a failure status: REFUSED with the status, or UNSUPPORTED (not a failure, devices.md
 * §5) - for ZCL and ZDO alike; and a cluster the endpoint lacks. */
static void test_fault_status(void)
{
    zbf_dev_t *d = joined(SIREN);
    zbf_fault_t f = {.status = ZBZ_ZCL_FAILURE};
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &f);
    TEST_ASSERT_TRUE(read_basic(d, 1u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_REFUSED, done_of(1u)->u.done.result);
    TEST_ASSERT_EQUAL_HEX8(ZBZ_ZCL_FAILURE, done_of(1u)->u.done.zcl_status);
    f.status = ZBZ_ZCL_UNSUP_ATTRIBUTE;
    zbf_fault(d, ZSRC_ZB_STEP_BASIC, &f);
    TEST_ASSERT_TRUE(read_basic(d, 2u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(2u)->u.done.result);
    TEST_ASSERT_EQUAL_HEX8(0u, done_of(2u)->u.done.zcl_status);
    f.status = ZBF_ZDP_NOT_SUPPORTED;
    zbf_fault(d, ZSRC_ZB_STEP_POWER_DESC, &f);
    f.status = ZBF_ZDP_TABLE_FULL;
    zbf_fault(d, ZSRC_ZB_STEP_SIMPLE_DESC, &f);
    TEST_ASSERT_TRUE(H.power_desc_req(H.ctx, d->ieee, d->nwk, 3u));
    TEST_ASSERT_TRUE(H.simple_desc_req(H.ctx, d->ieee, d->nwk, 43u, 4u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(3u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_REFUSED, done_of(4u)->u.done.result);
    TEST_ASSERT_EQUAL_HEX8(ZBF_ZDP_TABLE_FULL, done_of(4u)->u.done.zcl_status);
    TEST_ASSERT_EQUAL_size_t(0u, count_of(ZSRC_ZB_REP_POWER_DESC));
    TEST_ASSERT_EQUAL_size_t(0u, count_of(ZSRC_ZB_REP_SIMPLE_DESC));
    /* Without a fault: an endpoint it doesn't have is NOT_ACTIVE (unsupported); a cluster it
     * doesn't have answers UNSUP_CLUSTER. */
    zbf_fault(d, ZSRC_ZB_STEP_SIMPLE_DESC, NULL);
    TEST_ASSERT_TRUE(H.simple_desc_req(H.ctx, d->ieee, d->nwk, 7u, 5u));
    static const uint16_t interval = 0x0000u;
    TEST_ASSERT_TRUE(H.read_attributes(H.ctx, d->ieee, 43u, ZSRC_ZB_ZCL_POLL_CTRL, &interval, 1u,
                                       6u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(5u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_UNSUPPORTED, done_of(6u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_POLL_CTRL, done_of(6u)->u.done.step);
}

/* ZB-012: a device that never answers, until the knob is cleared. */
static void test_fault_timeout(void)
{
    zbf_dev_t *d = joined(SIREN);
    const zbf_fault_t mute = {.timeout = true};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, &mute);
    for (uint32_t t = 1u; t <= 3u; t++) {
        TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, d->ieee, d->nwk, t));
        run(ZBF_TIMEOUT_MS + 100u);
        TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(t)->u.done.result);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, d->requests);
    zbf_fault(d, ZSRC_ZB_STEP_NONE, NULL);
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, d->ieee, d->nwk, 4u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(4u)->u.done.result);
    const zsrc_zb_report_t *a = last_of(ZSRC_ZB_REP_ACTIVE_EP);
    TEST_ASSERT_EQUAL_UINT8(3u, a->u.active.count);
    TEST_ASSERT_EQUAL_UINT8(43u, a->u.active.ep[1]);
}

/* ---- sleepy devices ------------------------------------------------------------------------------- */

/* ZB-005, ZB-014 (testing.md §8): a sleepy device answers only inside its wake window - opened by
 * its own frame or check-in; a request waits at its parent for ZBF_HOLD_MS, then is lost. */
static void test_sleepy_device_answers_in_its_wake_window(void)
{
    zbf_dev_t *d = joined(CONTACT);
    TEST_ASSERT_TRUE(d->sleepy);
    TEST_ASSERT_TRUE(zbf_awake(&R, d)); /* just joined: it listens */
    run(ZBF_WAKE_MS + 1000u);
    TEST_ASSERT_FALSE(zbf_awake(&R, d));

    /* Asleep the whole hold: lost, TIMEOUT. */
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 1u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(1u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT32(0u, d->requests);

    /* It checks in inside the hold: the request reaches it, it answers. */
    clear();
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 2u));
    run(3000u);
    TEST_ASSERT_NULL(done_of(2u));
    TEST_ASSERT_TRUE(zbf_check_in(&R, d));
    TEST_ASSERT_TRUE(zbf_awake(&R, d));
    run(1000u);
    TEST_ASSERT_EQUAL_size_t(1u, count_of(ZSRC_ZB_REP_CHECK_IN));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(2u)->u.done.result);
    /* Answering keeps it listening: the next question in its window goes straight through. */
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, d->ieee, d->nwk, 3u));
    run(500u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(3u)->u.done.result);

    /* Its own report opens the window too (a device without Poll Control announces itself). */
    zbf_dev_t *m = joined(MAGNET);
    run(ZBF_WAKE_MS + 1000u);
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, m->ieee, m->nwk, 4u));
    TEST_ASSERT_TRUE(zbf_trip(&R, m, true));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(4u)->u.done.result);
    clear();
    TEST_ASSERT_TRUE(zbf_check_in(&R, m));
    run(100u);
    TEST_ASSERT_EQUAL_size_t(1u, count_of(ZSRC_ZB_REP_ANNOUNCE));

    /* Periodic check-ins, and a router that answers whenever. */
    zbf_set_check_in(&R, d, 60u);
    clear();
    run(60000u + 100u); /* due at 60 s, on the air ZBF_LATENCY_MS */
    TEST_ASSERT_EQUAL_size_t(1u, count_of(ZSRC_ZB_REP_CHECK_IN));
    run(60000u);
    TEST_ASSERT_EQUAL_size_t(2u, count_of(ZSRC_ZB_REP_CHECK_IN));
    zbf_set_check_in(&R, d, 0u);
    zbf_dev_t *s = joined(SIREN);
    run(3600000u);
    TEST_ASSERT_TRUE(zbf_awake(&R, s));
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, s->ieee, s->nwk, 5u));
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(5u)->u.done.result);
    /* Made sleepy by the knob, a router waits too. */
    zbf_set_sleepy(s, true, 2000u);
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, s->ieee, s->nwk, 6u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_TIMEOUT, done_of(6u)->u.done.result);
}

/* ZB-005 (testing.md §8): the Poll Control check-in interval read reaches the source. */
static void test_check_in_interval_reaches_the_source(void)
{
    zbf_dev_t *d = joined(CONTACT);
    const zsrc_zb_desc_t x = desc_of(d, 1u, ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_POLL_CTRL,
                                     ZSRC_ZB_ZONE_TYPE_CONTACT);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &x, NOW));
    zbf_set_check_in(&R, d, 5400u);
    static const uint16_t interval = 0x0000u;
    TEST_ASSERT_TRUE(H.read_attributes(H.ctx, d->ieee, 1u, ZSRC_ZB_ZCL_POLL_CTRL, &interval, 1u,
                                       1u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(1u)->u.done.result);
    TEST_ASSERT_EQUAL_UINT32(5400u, zsrc_zb_device(&Z, d->ieee, 1u)->check_in_s);
}

/* ---- replays, test mode, the radio ---------------------------------------------------------------- */

/* ZB-012 (testing.md §7): a device's last frame again, the same bytes and sequence number - a
 * duplicate Enroll Request after the CIE write. */
static void test_repeat_sends_the_same_frame_again(void)
{
    zbf_init(&R, NULL, &SINK, NOW); /* no source: nobody answers the Enroll Request */
    zbf_up(&R);
    zbf_dev_t *d = joined(CONTACT);
    TEST_ASSERT_TRUE(H.ias_cie_write(H.ctx, d->ieee, 1u));
    run(100u);
    const zbf_frame_t *first = last_rx(d->ieee);
    TEST_ASSERT_NOT_NULL(first);
    zsrc_zb_zcl_t z;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(first->bytes, first->len, first->cluster, &z));
    zbz_ias_enroll_req_t req;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_enroll_request(&z, &req));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, req.zone_type);
    TEST_ASSERT_EQUAL_HEX16(d->node.manufacturer_code, req.manufacturer);
    const zbf_frame_t copy = *first;
    const uint32_t before = R.log_total;
    TEST_ASSERT_TRUE(zbf_repeat(&R, d));
    TEST_ASSERT_EQUAL_UINT32(before + 1u, R.log_total);
    const zbf_frame_t *again = last_rx(d->ieee);
    TEST_ASSERT_EQUAL_UINT8(copy.len, again->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(copy.bytes, again->bytes, copy.len);
    zbf_dev_t *fresh = zbf_add(&R, MOTION);
    TEST_ASSERT_FALSE(zbf_repeat(&R, fresh)); /* never spoke */
}

/* ZB-022: Initiate Test Mode puts the TEST bit in its status until the time is up; Normal
 * Operation Mode takes it out at once. */
static void test_ias_test_mode(void)
{
    zbf_dev_t *d = joined(SIREN); /* a router, IAS Zone on 43 */
    TEST_ASSERT_TRUE(H.ias_test_mode(H.ctx, d->ieee, 43u, 60u, 61u));
    const zbf_frame_t *tx = zbf_last_tx(&R, d->ieee);
    TEST_ASSERT_EQUAL_HEX8(0x02u, tx->bytes[2]); /* Initiate Test Mode */
    TEST_ASSERT_EQUAL_HEX8(60u, tx->bytes[3]);
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(61u)->u.done.result);
    TEST_ASSERT_TRUE(zbf_trip(&R, d, true));
    zsrc_zb_zcl_t z;
    zbz_ias_status_t st;
    const zbf_frame_t *f = last_rx(d->ieee);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(f->bytes, f->len, f->cluster, &z));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&z, &st));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_IAS_ALARM1 | ZSRC_ZB_IAS_TEST, st.status);
    run(61000u);
    TEST_ASSERT_TRUE(zbf_trip(&R, d, true));
    f = last_rx(d->ieee);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(f->bytes, f->len, f->cluster, &z));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_ias_decode_status_change(&z, &st));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_IAS_ALARM1, st.status);
    TEST_ASSERT_TRUE(H.ias_test_mode(H.ctx, d->ieee, 43u, 60u, 62u));
    TEST_ASSERT_TRUE(H.ias_test_mode(H.ctx, d->ieee, 43u, 0u, 63u));
    tx = zbf_last_tx(&R, d->ieee);
    TEST_ASSERT_EQUAL_HEX8(0x01u, tx->bytes[2]); /* Initiate Normal Operation Mode */
    run(100u);
    TEST_ASSERT_EQUAL_UINT64(0u, zbf_ep(d, 43u)->test_until);
}

/* ZB-012 (testing.md §7): the panel's sequence numbers wrap from 0xFF to 0x00 and still match. */
static void test_sequence_numbers_wrap(void)
{
    zbf_dev_t *d = joined(SIREN);
    zbf_set_tsn(&R, 0xFFu, 0xFFu);
    TEST_ASSERT_TRUE(read_basic(d, 1u));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, zbf_last_tx(&R, d->ieee)->bytes[1]);
    TEST_ASSERT_TRUE(read_basic(d, 2u));
    TEST_ASSERT_EQUAL_HEX8(0x00u, zbf_last_tx(&R, d->ieee)->bytes[1]);
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 3u));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, zbf_last_tx(&R, d->ieee)->bytes[0]);
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 4u));
    TEST_ASSERT_EQUAL_HEX8(0x00u, zbf_last_tx(&R, d->ieee)->bytes[0]);
    run(1000u);
    for (uint32_t t = 1u; t <= 4u; t++) {
        TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(t)->u.done.result);
    }
}

/* ZB-006, ZSRC-015: the radio unplugged - requests in flight end NO_RADIO, every hook fails, the
 * source's link goes down; back, the network is reported and the link comes up. */
static void test_unplugged_radio(void)
{
    zbf_dev_t *d = joined(SIREN);
    TEST_ASSERT_TRUE(Z.link_up);
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 1u));
    zbf_unplug(&R, true);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_NO_RADIO, done_of(1u)->u.done.result);
    TEST_ASSERT_FALSE(Z.link_up);
    TEST_ASSERT_FALSE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 2u));
    TEST_ASSERT_FALSE(H.permit_join(H.ctx, 180u));
    TEST_ASSERT_FALSE(H.energy_scan(H.ctx));
    TEST_ASSERT_FALSE(H.forget(H.ctx, d->ieee));
    TEST_ASSERT_FALSE(H.set_channel(H.ctx, 20u, 3u));
    run(ZBF_TIMEOUT_MS + 100u);
    TEST_ASSERT_EQUAL_size_t(1u, dones_of(1u));
    TEST_ASSERT_NULL(done_of(2u));
    clear();
    zbf_unplug(&R, false);
    TEST_ASSERT_TRUE(Z.link_up);
    TEST_ASSERT_EQUAL_size_t(1u, count_of(ZSRC_ZB_REP_NETWORK));
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, d->ieee, d->nwk, 4u));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(4u)->u.done.result);
}

/* ZB-016, ZB-017, ZB-006: the channel, the survey, the energy scan and a network reset. */
static void test_network_operations(void)
{
    zbf_dev_t *d = joined(SIREN);
    run((uint64_t)ZSRC_ZB_LEARN_S * 1000u); /* the window closes */
    clear();
    TEST_ASSERT_FALSE(H.set_channel(H.ctx, 12u, 1u)); /* not one of the panel's */
    TEST_ASSERT_TRUE(H.set_channel(H.ctx, 20u, 2u));
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(2u)->u.done.result);
    const zsrc_zb_report_t *n = last_of(ZSRC_ZB_REP_NETWORK);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_TRUE(index_of_done(2u) < (size_t)(n - REPS));
    TEST_ASSERT_EQUAL_UINT8(20u, n->u.network.channel);
    TEST_ASSERT_EQUAL_UINT8(1u, n->u.network.update_id);
    TEST_ASSERT_EQUAL_HEX64(ZBF_PANEL_IEEE, n->u.network.panel_ieee);
    TEST_ASSERT_EQUAL_UINT8(20u, Z.channel);

    TEST_ASSERT_TRUE(H.channel_survey(H.ctx, 3u));
    run(100u);
    const zsrc_zb_report_t *s = last_of(ZSRC_ZB_REP_SURVEY);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_HEX8(0x0Fu, s->u.survey.known);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(3u)->u.done.result);
    TEST_ASSERT_TRUE(H.permit_join(H.ctx, 60u));
    TEST_ASSERT_FALSE(H.channel_survey(H.ctx, 4u)); /* not while devices are joining */
    TEST_ASSERT_TRUE(H.permit_join(H.ctx, 0u));

    zbf_set_energy(&R, -20);
    TEST_ASSERT_TRUE(H.energy_scan(H.ctx));
    run(100u);
    TEST_ASSERT_EQUAL_INT8(-20, Z.energy_dbm);

    const uint16_t pan = R.pan_id;
    TEST_ASSERT_TRUE(H.reset_network(H.ctx, 5u));
    TEST_ASSERT_FALSE(d->joined); /* every device went with the old network */
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_OK, done_of(5u)->u.done.result);
    TEST_ASSERT_TRUE(last_of(ZSRC_ZB_REP_NETWORK)->u.network.pan_id != pan);
}

/* ZB-012: what can't be asked is refused at once, not queued: bad attribute counts, and more
 * requests at once than the radio keeps. */
static void test_requests_beyond_the_radio_are_refused(void)
{
    zbf_dev_t *d = joined(SIREN);
    static const uint16_t nine[9] = {0};
    TEST_ASSERT_FALSE(H.read_attributes(H.ctx, d->ieee, 43u, ZSRC_ZB_ZCL_BASIC, nine, 0u, 1u));
    TEST_ASSERT_FALSE(H.read_attributes(H.ctx, d->ieee, 43u, ZSRC_ZB_ZCL_BASIC, nine, 9u, 1u));
    TEST_ASSERT_FALSE(H.read_attributes(H.ctx, d->ieee, 43u, ZSRC_ZB_ZCL_BASIC, NULL, 1u, 1u));
    TEST_ASSERT_FALSE(H.zcl_command(H.ctx, d->ieee, 43u, NULL, 1u));
    size_t taken = 0;
    while (taken < 2u * ZBF_RQ_MAX && H.active_ep_req(H.ctx, d->ieee, d->nwk, 100u)) {
        taken++;
    }
    TEST_ASSERT_EQUAL_size_t(ZBF_RQ_MAX, taken);
    run(1000u);
    TEST_ASSERT_EQUAL_size_t(ZBF_RQ_MAX, dones_of(100u));
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, d->ieee, d->nwk, 101u)); /* room again */
}

/* ZB-003, ZB-012: a malformed frame either way. The device answers a cut Enroll Response with
 * MALFORMED_COMMAND (REFUSED); the panel side drops a Zone Status Change Notification of 5 bytes
 * (a cut delay) without telling the source anything; and when nothing is left, nothing is due. */
static void test_malformed_frames_are_refused(void)
{
    zbf_dev_t *d = joined(SIREN);
    zsrc_zb_zcl_t cut;
    zbz_ias_enroll_response(ZBZ_IAS_ENROLL_OK, 5u, &cut);
    cut.len = 1u;
    TEST_ASSERT_TRUE(H.zcl_command(H.ctx, d->ieee, 43u, &cut, 71u));
    run(100u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_DONE_REFUSED, done_of(71u)->u.done.result);
    TEST_ASSERT_EQUAL_HEX8(0x80u, done_of(71u)->u.done.zcl_status);
    TEST_ASSERT_FALSE(zbf_ep(d, 43u)->enrolled);

    const zsrc_zb_desc_t x = desc_of(d, 43u, ZSRC_ZB_CL_IAS_ZONE, ZSRC_ZB_ZONE_TYPE_WARNING);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &x, NOW));
    zsrc_zb_zcl_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    bad.command = ZBZ_IAS_STATUS_CHANGE;
    bad.server_to_client = true;
    bad.tsn = ZSRC_ZB_TSN_NEW;
    bad.payload[0] = (uint8_t)ZSRC_ZB_IAS_ALARM1;
    bad.len = 5u;
    TEST_ASSERT_TRUE(zbf_send(&R, d, 43u, &bad));
    run(100u);
    TEST_ASSERT_FALSE(zsrc_zb_device(&Z, d->ieee, 43u)->status_known);
    run(ZBF_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zbf_next_due(&R));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frames_are_real_zcl_and_zdo_bytes);
    RUN_TEST(test_configure_reporting_is_decoded_by_the_device);
    RUN_TEST(test_bind_and_identify_follow_the_clusters);
    RUN_TEST(test_joins_record_how_the_device_joined);
    RUN_TEST(test_leave_rejoin_and_forget);
    RUN_TEST(test_reports_reach_the_source);
    RUN_TEST(test_keypad_and_siren_commands);
    RUN_TEST(test_fault_drop_is_per_step);
    RUN_TEST(test_fault_delay);
    RUN_TEST(test_fault_duplicate);
    RUN_TEST(test_fault_other_tsn);
    RUN_TEST(test_fault_status);
    RUN_TEST(test_fault_timeout);
    RUN_TEST(test_sleepy_device_answers_in_its_wake_window);
    RUN_TEST(test_check_in_interval_reaches_the_source);
    RUN_TEST(test_repeat_sends_the_same_frame_again);
    RUN_TEST(test_ias_test_mode);
    RUN_TEST(test_sequence_numbers_wrap);
    RUN_TEST(test_unplugged_radio);
    RUN_TEST(test_network_operations);
    RUN_TEST(test_requests_beyond_the_radio_are_refused);
    RUN_TEST(test_malformed_frames_are_refused);
    return UNITY_END();
}
