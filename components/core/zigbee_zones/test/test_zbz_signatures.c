/*
 * test_zbz_signatures.c - the converter test (docs/features/20 testing.md §2): every device
 * signature (test/signatures, one JSON file per device, generated into zb_signatures.h by tools/zb_signatures.py)
 * becomes a virtual device on the fake radio, is interviewed as the setup will interview it
 * (devices.md §5: node descriptor, active endpoints, each simple descriptor, Basic, IAS Zone),
 * and the registry record built from those answers must be what the row expects:
 *   - router or sleepy (zbz_dev_router, zbz_dev_sleepy);
 *   - the endpoint chosen first (zbz_dev_set_endpoint's order);
 *   - its role, by zbz_panel's own rule (zbz_role_of, devices.md §1 and §5);
 *   - for each input, zbz_dev_desc: the zsrc_zb_desc_t the row expects (clusters, zone type,
 *     router, identity, how it joined), and handed to the zone source (zsrc_zb_device_seen), the
 *     device class, the supervision window and the enrollment path the row names - which then runs
 *     to the end against the virtual device.
 *
 * zone_hub's test_zb_signatures.c feeds the same rows to the source directly; this one proves the
 * record and its converter in between. Addresses are the rows' own (fictional).
 *
 * Requirements: ZB-003, ZB-004 (what a device is, its enrollment), ZB-005 (supervision per
 * device), ZB-007 (a brand decided by its clusters), ZB-012 (the setup's questions and answers),
 * ZB-021 (how it joined, kept in the record).
 */
#include <string.h>

#include "unity.h"
#include "zbz_fake_radio.h"
#include "zigbee_zones/zbz_panel.h"
#include "zigbee_zones/zbz_registry.h"
#include "zone_hub/zsrc_zigbee.h"

#include "signatures/zb_signatures.h"

/* zb_signatures.h can't include zbz_registry.h: its role numbers are zbz_role_t's by value. */
_Static_assert(ZBSIG_ROLE_UNKNOWN == ZBZ_ROLE_UNKNOWN, "role numbers agree");
_Static_assert(ZBSIG_ROLE_SENSOR == ZBZ_ROLE_SENSOR, "role numbers agree");
_Static_assert(ZBSIG_ROLE_SIREN == ZBZ_ROLE_SIREN, "role numbers agree");
_Static_assert(ZBSIG_ROLE_KEYPAD == ZBZ_ROLE_KEYPAD, "role numbers agree");
_Static_assert(ZBSIG_ROLE_KEYFOB == ZBZ_ROLE_KEYFOB, "role numbers agree");
_Static_assert(ZBSIG_ROLE_REPEATER == ZBZ_ROLE_REPEATER, "role numbers agree");
_Static_assert(ZBSIG_ROLE_OTHER == ZBZ_ROLE_OTHER, "role numbers agree");

#define TAGS 32u

static zbf_radio_t R;
static zsrc_zb_t Z;
static zsrc_zb_radio_t H;
static zbz_device_t D;
static uint64_t NOW;
static uint8_t ACTIVE[ZSRC_ZB_EP_MAX];
static uint8_t ACTIVE_N;
static bool DONE[TAGS];
static uint8_t RESULT[TAGS];
static uint32_t NEXT_TAG;

void setUp(void) {}
void tearDown(void) {}

/* What zbz_panel does with each report, as far as the record goes: the source sees it first. */
static void on_report(void *ctx, const zsrc_zb_report_t *r, uint64_t now)
{
    (void)ctx;
    (void)zsrc_zb_on_report(&Z, r, now);
    if (r->ieee != D.ieee && r->kind != ZSRC_ZB_REP_DONE) {
        return;
    }
    switch (r->kind) {
    case ZSRC_ZB_REP_JOINED:
        D.join = r->u.joined.join;
        D.nwk = r->nwk;
        break;
    case ZSRC_ZB_REP_NODE_DESC:
        D.node_type = (uint8_t)(r->u.node.logical_type == ZSRC_ZB_LOGICAL_END_DEVICE
                                    ? ZBZ_NODE_END_DEVICE
                                    : ZBZ_NODE_ROUTER);
        D.mac_caps = r->u.node.mac_caps;
        D.manufacturer_code = r->u.node.manufacturer_code;
        break;
    case ZSRC_ZB_REP_ACTIVE_EP:
        ACTIVE_N = r->u.active.count;
        memcpy(ACTIVE, r->u.active.ep, sizeof(ACTIVE));
        break;
    case ZSRC_ZB_REP_SIMPLE_DESC:
        TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&D, &r->u.simple));
        break;
    case ZSRC_ZB_REP_BASIC:
        if ((r->u.basic.have & ZSRC_ZB_BASIC_MANUFACTURER) != 0u) {
            memcpy(D.manufacturer, r->u.basic.manufacturer, sizeof(D.manufacturer));
        }
        if ((r->u.basic.have & ZSRC_ZB_BASIC_MODEL) != 0u) {
            memcpy(D.model, r->u.basic.model, sizeof(D.model));
        }
        if ((r->u.basic.have & ZSRC_ZB_BASIC_POWER) != 0u) {
            D.power_source = r->u.basic.power_source;
        }
        break;
    case ZSRC_ZB_REP_DONE:
        if (r->tag < TAGS) {
            DONE[r->tag] = true;
            RESULT[r->tag] = r->u.done.result;
        }
        break;
    default: break;
    }
}

/* The IAS Zone Type comes from the IAS Zone read, not the descriptor. */
static void on_attr(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint16_t attr,
                    uint64_t value, uint64_t now)
{
    (void)ctx;
    (void)now;
    if (ieee != D.ieee || cluster != ZSRC_ZB_ZCL_IAS_ZONE || attr != ZBZ_IAS_ATTR_ZONE_TYPE) {
        return;
    }
    for (uint8_t i = 0; i < D.ep_count; i++) {
        if (D.ep[i].endpoint == endpoint) {
            D.ep[i].zone_type = (uint16_t)value;
        }
    }
}

static void run(uint64_t ms)
{
    NOW += ms;
    zbf_advance(&R, NOW);
}

static uint32_t tag(void)
{
    uint32_t t = NEXT_TAG++;
    TEST_ASSERT_TRUE(t < TAGS);
    return t;
}

/* Each question is answered within a second, and the request ends OK (ZB-012). */
static void answered(uint32_t t, const char *file)
{
    run(1000u);
    TEST_ASSERT_TRUE_MESSAGE(DONE[t], file);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZSRC_ZB_DONE_OK, RESULT[t], file);
}

static zbf_dev_t *interview(const zbsig_t *s)
{
    static const zbf_sink_t sink = {.report = on_report, .attr = on_attr};
    memset(&D, 0, sizeof(D));
    memset(DONE, 0, sizeof(DONE));
    memset(RESULT, 0xFF, sizeof(RESULT));
    ACTIVE_N = 0u;
    NEXT_TAG = 1u;
    NOW = 5000000u;
    D.used = true;
    D.id = 1u;
    D.ieee = s->ieee;
    D.nwk = s->nwk;

    zbf_init(&R, &Z, &sink, NOW);
    H = zbf_hooks(&R);
    zsrc_zb_init(&Z, &H, NULL);
    zbf_up(&R);
    zbf_dev_t *dev = zbf_add(&R, s->file);
    TEST_ASSERT_NOT_NULL_MESSAGE(dev, s->file);
    TEST_ASSERT_TRUE(H.permit_join(H.ctx, (uint16_t)ZSRC_ZB_LEARN_S));
    TEST_ASSERT_TRUE_MESSAGE(zbf_join(&R, dev), s->file);
    run(100u);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZSRC_ZB_JOIN_STANDARD_KEY, D.join, s->file);

    uint32_t t = tag();
    TEST_ASSERT_TRUE(H.node_desc_req(H.ctx, s->ieee, D.nwk, t));
    answered(t, s->file);
    t = tag();
    TEST_ASSERT_TRUE(H.active_ep_req(H.ctx, s->ieee, D.nwk, t));
    answered(t, s->file);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->ep_count, ACTIVE_N, s->file);
    for (uint8_t i = 0; i < ACTIVE_N; i++) {
        t = tag();
        TEST_ASSERT_TRUE(H.simple_desc_req(H.ctx, s->ieee, D.nwk, ACTIVE[i], t));
        answered(t, s->file);
    }
    /* Basic from the first endpoint that has it (devices.md §5). Manufacturer and model are
     * asked one at a time: both at their longest (32 characters each) don't fit one answer of
     * ZSRC_ZB_ZCL_PAYLOAD_MAX bytes (IKEA's 27-character model with its manufacturer is 51), and
     * a device answers only the records that fit. */
    static const uint16_t basic[3][4] = {
        {0x0004u, 0x0007u},
        {0x0005u},
        {0x0000u, 0x0001u, 0x0003u, 0x4000u},
    };
    static const uint8_t basic_n[3] = {2u, 1u, 4u};
    for (uint8_t i = 0; i < D.ep_count; i++) {
        if ((D.ep[i].in_clusters & ZSRC_ZB_CL_BASIC) != 0u) {
            for (size_t k = 0; k < 3u; k++) {
                t = tag();
                TEST_ASSERT_TRUE(H.read_attributes(H.ctx, s->ieee, D.ep[i].endpoint,
                                                   ZSRC_ZB_ZCL_BASIC, basic[k], basic_n[k], t));
                answered(t, s->file);
            }
            break;
        }
    }
    static const uint16_t ias[] = {ZBZ_IAS_ATTR_ZONE_STATE, ZBZ_IAS_ATTR_ZONE_TYPE,
                                   ZBZ_IAS_ATTR_CIE_ADDRESS};
    for (uint8_t i = 0; i < D.ep_count; i++) {
        if ((D.ep[i].in_clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u) {
            t = tag();
            TEST_ASSERT_TRUE(H.read_attributes(H.ctx, s->ieee, D.ep[i].endpoint,
                                               ZSRC_ZB_ZCL_IAS_ZONE, ias, 3u, t));
            answered(t, s->file);
        }
    }
    return dev;
}

static bool same_text(const char *a, const char *b)
{
    return strcmp(a != NULL ? a : "", b != NULL ? b : "") == 0;
}

static bool row_has(const uint16_t *list, uint8_t n, uint16_t cluster)
{
    for (uint8_t i = 0; i < n; i++) {
        if (list[i] == cluster) {
            return true;
        }
    }
    return false;
}

/* The endpoint the row says is the device's main one, from its raw cluster ids. */
static uint8_t row_primary(const zbsig_t *s)
{
    if (s->input_count > 0u) {
        return s->input[0].endpoint;
    }
    for (uint8_t i = 0; i < s->ep_count; i++) {
        const zbsig_ep_t *e = &s->ep[i];
        if (e->profile == 0x0104u && e->endpoint != 242u &&
            (row_has(e->in, e->in_count, ZSRC_ZB_ZCL_IAS_WD) ||
             row_has(e->out, e->out_count, ZSRC_ZB_ZCL_IAS_ACE))) {
            return e->endpoint;
        }
    }
    for (uint8_t i = 0; i < s->ep_count; i++) {
        if (s->ep[i].profile == 0x0104u && s->ep[i].endpoint != 242u) {
            return s->ep[i].endpoint;
        }
    }
    return s->ep[0].endpoint;
}

static const zbsig_ep_t *row_ep(const zbsig_t *s, uint8_t endpoint)
{
    for (uint8_t i = 0; i < s->ep_count; i++) {
        if (s->ep[i].endpoint == endpoint) {
            return &s->ep[i];
        }
    }
    return NULL;
}

static size_t cie_writes_to(uint64_t ieee)
{
    size_t n = 0;
    for (size_t i = 0; i < zbf_log_count(&R); i++) {
        const zbf_frame_t *f = zbf_log_at(&R, i);
        if (!f->rx && f->ieee == ieee && f->kind == ZBF_FRAME_ZCL &&
            f->cluster == ZSRC_ZB_ZCL_IAS_ZONE && f->len >= 3u && f->bytes[2] == ZBZ_ZCL_WRITE_ATTR) {
            n++;
        }
    }
    return n;
}

/* ZB-003, ZB-004, ZB-005, ZB-007, ZB-012, ZB-021: every row, interviewed through the fake radio,
 * gives the record and the descriptors the row expects. */
static void test_each_signature_converts_to_what_its_row_expects(void)
{
    TEST_ASSERT_TRUE(ZB_SIGNATURE_COUNT >= 15u);
    TEST_ASSERT_EQUAL_size_t(ZB_SIGNATURE_COUNT, zbf_signature_count());
    for (size_t i = 0; i < ZB_SIGNATURE_COUNT; i++) {
        const zbsig_t *s = &ZB_SIGNATURES[i];
        zbf_dev_t *dev = interview(s);

        /* The record. */
        TEST_ASSERT_EQUAL_MESSAGE(s->router, zbz_dev_router(&D), s->file);
        TEST_ASSERT_EQUAL_MESSAGE(s->sleepy, zbz_dev_sleepy(&D), s->file);
        TEST_ASSERT_TRUE_MESSAGE(same_text(D.manufacturer, s->manufacturer), s->file);
        TEST_ASSERT_TRUE_MESSAGE(same_text(D.model, s->model), s->file);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->power_source, D.power_source, s->file);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(s->node.manufacturer_code, D.manufacturer_code, s->file);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(s->ep_count > ZBZ_EP_MAX ? ZBZ_EP_MAX : s->ep_count),
                                        D.ep_count, s->file);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row_primary(s), D.ep[0].endpoint, s->file);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->role, zbz_role_of(&D), s->file);
        for (uint8_t e = 0; e < D.ep_count; e++) {
            const zbsig_ep_t *se = row_ep(s, D.ep[e].endpoint);
            TEST_ASSERT_NOT_NULL_MESSAGE(se, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(se->profile, D.ep[e].profile, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(se->device_id, D.ep[e].device_id, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(se->zone_type, D.ep[e].zone_type, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(zbz_cluster_bits(se->out, se->out_count),
                                            D.ep[e].out_clusters, s->file);
        }
        /* An alarm device's main endpoint, as the source would be told of it. */
        zsrc_zb_desc_t desc;
        TEST_ASSERT_TRUE_MESSAGE(zbz_dev_desc(&D, D.ep[0].endpoint, &desc), s->file);
        TEST_ASSERT_EQUAL_MESSAGE(s->router, desc.router, s->file);
        TEST_ASSERT_FALSE_MESSAGE(zbz_dev_desc(&D, 99u, &desc), s->file); /* no such endpoint */

        /* Each input: the descriptor the row expects, and what the source makes of it. */
        for (uint8_t k = 0; k < s->input_count; k++) {
            const zbsig_input_t *in = &s->input[k];
            TEST_ASSERT_TRUE_MESSAGE(zbz_dev_desc(&D, in->endpoint, &desc), s->file);
            TEST_ASSERT_EQUAL_HEX64_MESSAGE(s->ieee, desc.ieee, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(s->nwk, desc.nwk, s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(in->endpoint, desc.endpoint, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(in->clusters, desc.clusters, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(in->zone_type, desc.zone_type, s->file);
            TEST_ASSERT_EQUAL_MESSAGE(s->router, desc.router, s->file);
            TEST_ASSERT_TRUE_MESSAGE(same_text(desc.manufacturer, s->manufacturer), s->file);
            TEST_ASSERT_TRUE_MESSAGE(same_text(desc.model, s->model), s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZSRC_ZB_JOIN_STANDARD_KEY, desc.join, s->file);

            TEST_ASSERT_TRUE_MESSAGE(zsrc_zb_device_seen(&Z, &desc, NOW), s->file);
            const zsrc_zb_device_t *zd = zsrc_zb_device(&Z, s->ieee, in->endpoint);
            TEST_ASSERT_NOT_NULL_MESSAGE(zd, s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(in->device_class, zd->device_class, s->file);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(s->supervision_s, zsrc_zb_supervision_s(&Z, zd),
                                             s->file);
            if (s->enroll == ZBSIG_ENROLL_NONE || s->enroll == ZBSIG_ENROLL_UNSUPPORTED) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(ZSRC_ZB_ENROLL_NONE, zd->enroll, s->file);
            } else {
                TEST_ASSERT_TRUE_MESSAGE(zd->enroll == ZSRC_ZB_ENROLL_CIE ||
                                             zd->enroll == ZSRC_ZB_ENROLL_RESPOND ||
                                             zd->enroll == ZSRC_ZB_ENROLL_DONE,
                                         s->file);
            }
        }
        if (s->input_count == 0u) {
            continue;
        }
        /* The enrollment path runs to its end against the device: it speaks, the source writes
         * its CIE address, it asks to enroll, the source answers - and the device holds it. */
        dev->auto_enroll = s->enroll == ZBSIG_ENROLL_STANDARD;
        TEST_ASSERT_TRUE_MESSAGE(zbf_check_in(&R, dev), s->file);
        run(20000u);
        const zsrc_zb_device_t *zd = zsrc_zb_device(&Z, s->ieee, s->input[0].endpoint);
        const zbf_ep_t *fe = zbf_ep(dev, s->input[0].endpoint);
        TEST_ASSERT_NOT_NULL_MESSAGE(fe, s->file);
        if (s->enroll == ZBSIG_ENROLL_STANDARD) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(ZSRC_ZB_ENROLL_DONE, zd->enroll, s->file);
            TEST_ASSERT_TRUE_MESSAGE(fe->enrolled, s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(zd->zone_id, fe->zone_id, s->file);
            TEST_ASSERT_EQUAL_HEX64_MESSAGE(ZBF_PANEL_IEEE, fe->cie, s->file);
        } else {
            TEST_ASSERT_FALSE_MESSAGE(fe->enrolled, s->file);
            TEST_ASSERT_EQUAL_size_t_MESSAGE(0u, cie_writes_to(s->ieee), s->file);
        }
    }
}

/* ZB-007: a device the table doesn't have, and a name that is not a row, are refused. */
static void test_rows_are_found_by_file_or_device_name_only(void)
{
    zbf_init(&R, NULL, NULL, 0u);
    for (size_t i = 0; i < ZB_SIGNATURE_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(ZB_SIGNATURES[i].file, zbf_signature_file(i));
        TEST_ASSERT_EQUAL_STRING(ZB_SIGNATURES[i].device, zbf_signature_device(i));
    }
    TEST_ASSERT_NULL(zbf_signature_file(ZB_SIGNATURE_COUNT));
    TEST_ASSERT_NULL(zbf_signature_device(ZB_SIGNATURE_COUNT));
    TEST_ASSERT_NOT_NULL(zbf_add(&R, "frient_kepzb_110"));
    TEST_ASSERT_NULL(zbf_add(&R, "frient_kepzb_110.json")); /* the same address twice */
    TEST_ASSERT_NOT_NULL(zbf_add(&R, "Heiman HS1RC (RC-EF-3.0)"));
    TEST_ASSERT_NOT_NULL(zbf_add(&R, "tuya_ts0216.json"));
    TEST_ASSERT_NULL(zbf_add(&R, "frient_kepzb"));          /* a prefix is not a name */
    TEST_ASSERT_NULL(zbf_add(&R, "no_such_device"));
    TEST_ASSERT_NULL(zbf_add(&R, ""));
    TEST_ASSERT_NULL(zbf_add(&R, NULL));
    TEST_ASSERT_NULL(zbf_add_row(&R, ZB_SIGNATURE_COUNT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_each_signature_converts_to_what_its_row_expects);
    RUN_TEST(test_rows_are_found_by_file_or_device_name_only);
    return UNITY_END();
}
