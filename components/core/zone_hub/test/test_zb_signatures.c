/*
 * test_zb_signatures.c - every Zigbee device signature (docs/features/20 testing.md §2,
 * the JSON rows in components/core/zigbee_zones/test/signatures, generated into zb_signatures.h by
 * tools/zb_signatures.py) through the zone source as the radio side will feed it: the setup's
 * hand-off, the node descriptor, each endpoint's simple descriptor and Basic, as reports. What the
 * source makes of each input must be what the row says: its clusters, its device class, router or
 * sleepy, its supervision window, its enrollment path, the quirks row it matches, and its trust.
 *
 * The converter from a registry record to the source's descriptor (zbz_dev_desc) is zigbee_zones'
 * and gets its own test against the same table (ZB-M3).
 *
 * Requirements: ZB-003, ZB-004 (what a device is), ZB-005 (supervision per device), ZB-007 (the
 * quirks table), ZB-012 (the setup's reports), ZB-021 (trust whichever way it joined).
 */
#include <string.h>

#include "unity.h"
#include "zone_hub/zsrc_zigbee.h"

#include "../../zigbee_zones/test/signatures/zb_signatures.h"

static unsigned CIE_WRITES;

static bool radio_cie(void *ctx, uint64_t ieee, uint8_t endpoint)
{
    (void)ctx;
    (void)ieee;
    (void)endpoint;
    CIE_WRITES++;
    return true;
}

static zsrc_zb_t Z;
static uint64_t NOW;

void setUp(void)
{
    CIE_WRITES = 0u;
    NOW = 1000000u;
}

void tearDown(void) {}

static void fresh_source(void)
{
    const zsrc_zb_radio_t radio = {.ias_cie_write = radio_cie};
    CIE_WRITES = 0u;
    zsrc_zb_init(&Z, &radio, NULL);
    zsrc_zb_link(&Z, true, NOW);
}

static bool same_text(const char *a, const char *b)
{
    const char *x = a != NULL ? a : "";
    const char *y = b != NULL ? b : "";
    return strcmp(x, y) == 0;
}

static zsrc_zb_report_t report(uint8_t kind, const zbsig_t *s)
{
    zsrc_zb_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.ieee = s->ieee;
    r.nwk = s->nwk;
    return r;
}

/* The radio side's reports for one device, during its setup (devices.md §5 order). */
static void feed(const zbsig_t *s)
{
    zsrc_zb_device_setup(&Z, s->ieee, true, NOW);
    for (uint8_t i = 0; i < s->input_count; i++) {
        zsrc_zb_desc_t d;
        memset(&d, 0, sizeof(d));
        d.ieee = s->ieee;
        d.nwk = s->nwk;
        d.endpoint = s->input[i].endpoint;
        d.zone_type = s->input[i].zone_type;
        TEST_ASSERT_TRUE_MESSAGE(zsrc_zb_device_seen(&Z, &d, NOW), s->file);
    }
    zsrc_zb_report_t r = report(ZSRC_ZB_REP_NODE_DESC, s);
    r.u.node = s->node;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    for (uint8_t e = 0; e < s->ep_count; e++) {
        r = report(ZSRC_ZB_REP_SIMPLE_DESC, s);
        r.endpoint = s->ep[e].endpoint;
        r.u.simple.endpoint = s->ep[e].endpoint;
        r.u.simple.profile = s->ep[e].profile;
        r.u.simple.device_id = s->ep[e].device_id;
        r.u.simple.in_count = s->ep[e].in_count;
        r.u.simple.out_count = s->ep[e].out_count;
        memcpy(r.u.simple.in, s->ep[e].in, sizeof(r.u.simple.in));
        memcpy(r.u.simple.out, s->ep[e].out, sizeof(r.u.simple.out));
        TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    }
    r = report(ZSRC_ZB_REP_BASIC, s);
    r.u.basic.have = (uint16_t)(ZSRC_ZB_BASIC_MANUFACTURER | ZSRC_ZB_BASIC_MODEL |
                                ZSRC_ZB_BASIC_POWER);
    strncpy(r.u.basic.manufacturer, s->manufacturer, sizeof(r.u.basic.manufacturer) - 1u);
    strncpy(r.u.basic.model, s->model, sizeof(r.u.basic.model) - 1u);
    r.u.basic.power_source = s->power_source;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    zsrc_zb_device_setup(&Z, s->ieee, false, NOW); /* stopped unenrolled: the source's own path */
}

/* ZB-007 (testing.md §5): every seeded signature resolves to the quirks row it expects - or to
 * none - by its manufacturer and model as Basic reports them. */
static void test_each_signature_matches_its_quirk_row(void)
{
    TEST_ASSERT_TRUE(ZB_SIGNATURE_COUNT >= 15u);
    for (size_t i = 0; i < ZB_SIGNATURE_COUNT; i++) {
        const zbsig_t *s = &ZB_SIGNATURES[i];
        const zsrc_zb_quirk_t *q = zsrc_zb_quirk_for(s->manufacturer, s->model);
        if (!s->quirk) {
            TEST_ASSERT_NULL_MESSAGE(q, s->file);
            continue;
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(q, s->file);
        TEST_ASSERT_TRUE_MESSAGE(same_text(q->manufacturer, s->quirk_manufacturer), s->file);
        TEST_ASSERT_TRUE_MESSAGE(same_text(q->model, s->quirk_model), s->file);
        if (s->enroll == ZBSIG_ENROLL_UNSUPPORTED) {
            TEST_ASSERT_TRUE_MESSAGE((q->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) != 0u, s->file);
        }
        if (s->enroll == ZBSIG_ENROLL_NO_ENROLL) {
            TEST_ASSERT_TRUE_MESSAGE((q->quirks & ZSRC_ZB_QUIRK_NO_ENROLL) != 0u, s->file);
        }
    }
}

/* ZB-003, ZB-004, ZB-005, ZB-012, ZB-021: fed as the radio side will feed it, each input of each
 * sensor ends up as its row says. */
static void test_each_sensor_signature_through_the_source(void)
{
    for (size_t i = 0; i < ZB_SIGNATURE_COUNT; i++) {
        const zbsig_t *s = &ZB_SIGNATURES[i];
        if (s->input_count == 0u) {
            TEST_ASSERT_TRUE_MESSAGE(s->role != ZBSIG_ROLE_SENSOR, s->file);
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ZBSIG_ROLE_SENSOR, s->role, s->file);
        fresh_source();
        feed(s);
        const zone_source_ops_t *ops = zsrc_zb_ops();
        for (uint8_t k = 0; k < s->input_count; k++) {
            const zbsig_input_t *in = &s->input[k];
            const zsrc_zb_device_t *d = zsrc_zb_device(&Z, s->ieee, in->endpoint);
            TEST_ASSERT_NOT_NULL_MESSAGE(d, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(in->clusters, d->clusters, s->file);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(in->zone_type, d->zone_type, s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(in->device_class, d->device_class, s->file);
            TEST_ASSERT_EQUAL_MESSAGE(s->router, d->router, s->file);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(s->supervision_s, zsrc_zb_supervision_s(&Z, d),
                                             s->file);
            TEST_ASSERT_TRUE_MESSAGE(same_text(d->manufacturer, s->manufacturer), s->file);
            TEST_ASSERT_TRUE_MESSAGE(same_text(d->model, s->model), s->file);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->power_source, d->power_source, s->file);
            switch (s->enroll) {
            case ZBSIG_ENROLL_STANDARD:
            case ZBSIG_ENROLL_NO_ENROLL:
                TEST_ASSERT_EQUAL_INT_MESSAGE(ZSRC_ZB_ENROLL_CIE, d->enroll, s->file);
                TEST_ASSERT_TRUE_MESSAGE(d->zone_id != 0u, s->file);
                break;
            default:
                TEST_ASSERT_EQUAL_INT_MESSAGE(ZSRC_ZB_ENROLL_NONE, d->enroll, s->file);
                break;
            }
            /* Every Zigbee sensor may feed a fire, CO or panic zone (decided
             * 2026-10-02): how it joined is shown, not used to refuse it. */
            const zsrc_address_t a = zsrc_zb_address(s->ieee, in->endpoint);
            TEST_ASSERT_TRUE_MESSAGE((ops->caps_of(&Z, &a) & ZSRC_CAP_LIFE_SAFETY) != 0u, s->file);
            TEST_ASSERT_TRUE_MESSAGE(ops->valid(&Z, &a), s->file);
        }
        /* Endpoints that are not inputs (a vendor profile's, Green Power's) never become slots. */
        for (uint8_t e = 0; e < s->ep_count; e++) {
            bool is_input = false;
            for (uint8_t k = 0; k < s->input_count; k++) {
                is_input = is_input || s->input[k].endpoint == s->ep[e].endpoint;
            }
            if (!is_input) {
                TEST_ASSERT_NULL_MESSAGE(zsrc_zb_device(&Z, s->ieee, s->ep[e].endpoint), s->file);
            }
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(s->enroll == ZBSIG_ENROLL_NONE ? 0u : s->input_count,
                                       CIE_WRITES, s->file);
    }
}

/* AGENTS.md "Conventions": the table holds fictional addresses only, each once. */
static void test_the_signatures_are_fictional_and_distinct(void)
{
    for (size_t i = 0; i < ZB_SIGNATURE_COUNT; i++) {
        const zbsig_t *s = &ZB_SIGNATURES[i];
        TEST_ASSERT_TRUE_MESSAGE(((s->ieee >> 56) & 0x02u) != 0u, s->file);
        TEST_ASSERT_EQUAL_MESSAGE(!s->router, s->sleepy, s->file);
        for (size_t j = i + 1u; j < ZB_SIGNATURE_COUNT; j++) {
            TEST_ASSERT_TRUE_MESSAGE(s->ieee != ZB_SIGNATURES[j].ieee, s->file);
            TEST_ASSERT_TRUE_MESSAGE(s->nwk != ZB_SIGNATURES[j].nwk, s->file);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_each_signature_matches_its_quirk_row);
    RUN_TEST(test_each_sensor_signature_through_the_source);
    RUN_TEST(test_the_signatures_are_fictional_and_distinct);
    return UNITY_END();
}
