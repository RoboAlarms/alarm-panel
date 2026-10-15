/*
 * test_zigbee.c - the Zigbee sensor source (components/core/zone_hub/src/zsrc_zigbee.c, docs/features/20):
 * pairing windows, IAS Zone enrollment (including the retries sleepy devices need), the status
 * word mapped onto zone events, the non-IAS clusters, battery, supervision, jamming, the quirks
 * table and the picker - driven through the real hub, and for the alarms through the real
 * engine.
 *
 * No radio is involved: zsrc_zigbee talks to one through zsrc_zb_radio_t, and this file gives it
 * a fake whose answers the test chooses (a sleepy device refuses everything until it is awake).
 * That is the whole point of keeping the mapping portable - it can be proved on a PC, before any
 * hardware exists.
 *
 * Requirements: ZB-002..ZB-008 (docs/features/20), ZSRC-003, ZSRC-005..008, ZSRC-010, ZSRC-015;
 * since BL-143 (ZB-M2): ZB-005 (supervision per device), ZB-011 (link quality), ZB-012 (the
 * setup's hand-off and the radio's reports), ZB-013 (forget, leave and rejoin), ZB-021 (trust
 * whichever way it joined), ZB-022 (walk-test mode and the TEST bit).
 */
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zigbee.h"

/* ---- harness ------------------------------------------------------------------------------- */

#define IEEE_A 0x00158D0001A2B3C4ull /* the device most tests use */
#define IEEE_B 0x00124B00229988FFull
#define IEEE_C 0x847127FFFE001122ull

typedef struct {
    uint16_t zone;
    alarm_zone_event_t ev;
} rec_t;

static zone_hub_t H;
static zsrc_zb_t Z;
static sim_t E;
static bool TO_ENGINE;
static uint64_t NOW;
static rec_t R[512];
static unsigned NR;
static unsigned TROUBLE_ON;
static unsigned TROUBLE_OFF;
/* What the source itself said, before the hub has its say: ALARM_ZEV_TEST is the source's
 * contract (ZB-022); what the hub makes of it is the hub's. */
static unsigned SRC_TESTS;

/* What the fake radio was asked to do, and whether it is willing. */
static struct {
    unsigned permit_join;
    uint16_t permit_seconds;
    unsigned cie_writes;
    unsigned enroll_responses;
    uint8_t last_zone_id;
    unsigned reads;
    uint16_t last_read_cluster;
    uint16_t last_read_attr;
    uint16_t read_cluster[8]; /* the first reads, in order (ZB31) */
    uint16_t read_attr[8];
    unsigned removes;
    unsigned scans;
    bool awake; /* a sleepy device refuses everything until a frame proves it is awake */
    unsigned test_asks;     /* ias_test_mode calls (ZB-022) */
    uint8_t last_test_len;  /* 0: back to normal operation */
    uint32_t last_test_tag;
    uint64_t last_test_ieee;
} RADIO;

static void port_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    TEST_ASSERT_TRUE(NR < sizeof(R) / sizeof(R[0]));
    R[NR].zone = zone;
    R[NR].ev = ev;
    NR++;
    if (TO_ENGINE) {
        alarm_engine_zone_event(&E.eng, zone, ev, E.now);
    }
}

static void port_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZIGBEE, kind);
    if (on) {
        TROUBLE_ON++;
    } else {
        TROUBLE_OFF++;
    }
}

static unsigned count(uint16_t zone, alarm_zone_event_t ev)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NR; i++) {
        n += R[i].zone == zone && R[i].ev == ev;
    }
    return n;
}

static void clear_records(void)
{
    SRC_TESTS = 0;
    NR = 0;
    TROUBLE_ON = 0;
    TROUBLE_OFF = 0;
}

#define EXPECT_POSTED(z_, e_, n) TEST_ASSERT_EQUAL_UINT_MESSAGE((n), count((z_), (e_)), #e_)
#define EXPECT_LAST(z_, e_)                             \
    do {                                                \
        TEST_ASSERT_TRUE(NR > 0u);                      \
        TEST_ASSERT_EQUAL_UINT16((z_), R[NR - 1].zone); \
        TEST_ASSERT_EQUAL_INT((e_), R[NR - 1].ev);      \
    } while (0)

/* ---- the fake radio -------------------------------------------------------------------------- */

static bool radio_permit_join(void *ctx, uint16_t seconds)
{
    (void)ctx;
    RADIO.permit_join++;
    RADIO.permit_seconds = seconds;
    return true;
}

static bool radio_cie(void *ctx, uint64_t ieee, uint8_t endpoint)
{
    (void)ctx;
    (void)ieee;
    (void)endpoint;
    RADIO.cie_writes++;
    return RADIO.awake;
}

static bool radio_enroll(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t zone_id)
{
    (void)ctx;
    (void)ieee;
    (void)endpoint;
    RADIO.enroll_responses++;
    RADIO.last_zone_id = zone_id;
    return RADIO.awake;
}

static bool radio_read(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cluster, uint16_t attr)
{
    (void)ctx;
    (void)ieee;
    (void)ep;
    RADIO.reads++;
    RADIO.last_read_cluster = cluster;
    RADIO.last_read_attr = attr;
    if (RADIO.reads <= 8u) {
        RADIO.read_cluster[RADIO.reads - 1u] = cluster;
        RADIO.read_attr[RADIO.reads - 1u] = attr;
    }
    return true;
}

static bool radio_remove(void *ctx, uint64_t ieee)
{
    (void)ctx;
    (void)ieee;
    RADIO.removes++;
    return true;
}

static bool radio_scan(void *ctx)
{
    (void)ctx;
    RADIO.scans++;
    return true;
}

static bool radio_test_mode(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t len, uint32_t tag)
{
    (void)ctx;
    (void)endpoint;
    RADIO.test_asks++;
    RADIO.last_test_len = len;
    RADIO.last_test_tag = tag;
    RADIO.last_test_ieee = ieee;
    return RADIO.awake;
}

/* ---- the sink: straight into the hub, as zone_service wires it ------------------------------ */

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                       uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H, a, state, value, now);
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    if (ev == ALARM_ZEV_TEST) {
        SRC_TESTS++;
    }
    zone_hub_event(&H, a, ev, now);
}

static void sink_reachable(void *ctx, const zsrc_address_t *a, bool ok, uint64_t now)
{
    (void)ctx;
    zone_hub_reachable(&H, a, ok, now);
}

static void sink_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    zone_hub_link(&H, kind, up, now);
}

static void sink_link_now(void *ctx, uint8_t kind, uint64_t now)
{
    (void)ctx;
    zone_hub_link_now(&H, kind, now);
}

static void advance(uint64_t ms)
{
    NOW += ms;
    if (TO_ENGINE) {
        sim_advance(&E, (alarm_ms_t)ms);
    }
    zone_hub_tick(&H, NOW);
}

void setUp(void)
{
    memset(&RADIO, 0, sizeof(RADIO));
    RADIO.awake = true;
    TO_ENGINE = false;
    NOW = 1000000u;

    const zone_hub_port_t port = {
        .ctx = NULL, .zone_event = port_event, .changed = NULL, .source_trouble = port_trouble};
    zone_hub_init(&H, &port);
    const zsrc_zb_radio_t radio = {.ctx = NULL,
                                   .permit_join = radio_permit_join,
                                   .ias_cie_write = radio_cie,
                                   .ias_enroll_response = radio_enroll,
                                   .read_attribute = radio_read,
                                   .remove_device = radio_remove,
                                   .energy_scan = radio_scan,
                                   .ias_test_mode = radio_test_mode};
    const zsrc_sink_t sink = {.ctx = NULL,
                              .input = sink_input,
                              .event = sink_event,
                              .reachable = sink_reachable,
                              .link = sink_link,
                              .link_now = sink_link_now};
    zsrc_zb_init(&Z, &radio, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_zb_ops(), &Z, NOW));
    zone_hub_start(&H, NOW);
    zsrc_zb_link(&Z, true, NOW); /* the radio is there and the network is formed */
    clear_records();
}

void tearDown(void) {}

/* A descriptor for a device that speaks standard IAS. */
static zsrc_zb_desc_t ias_desc(uint64_t ieee, uint8_t ep, uint16_t zone_type, const char *manuf,
                               const char *model)
{
    zsrc_zb_desc_t d;
    memset(&d, 0, sizeof(d));
    d.ieee = ieee;
    d.nwk = 0x1234u;
    d.endpoint = ep;
    d.clusters = (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_POWER_CFG);
    d.zone_type = zone_type;
    snprintf(d.manufacturer, sizeof(d.manufacturer), "%s", manuf);
    snprintf(d.model, sizeof(d.model), "%s", model);
    return d;
}

static zone_hub_binding_t zb_binding(uint64_t ieee, uint8_t ep)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_zb_address(ieee, ep);
    return b;
}

/* Pair a standard IAS contact and bind it to a zone; returns its address. */
static zsrc_address_t pair_and_bind(uint16_t zone, uint64_t ieee, uint16_t zone_type)
{
    const zsrc_zb_desc_t d = ias_desc(ieee, 1u, zone_type, "Frient A/S", "MOSZB-1xx");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_ias_enroll_request(&Z, ieee, 1u, zone_type, NOW);
    zone_hub_binding_t b = zb_binding(ieee, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
    clear_records();
    return b.in[ZSRC_ROLE_MAIN].addr;
}

/* ---- ZB-003 / ZB-004: what a device says it is ---------------------------------------------- */

/* ZB-003: the IAS Zone Type picks the sensor kind, which in turn suggests the zone type the
 * installer is offered (docs/features/22 "Adding a zone"). */
static void test_zone_type_becomes_a_sensor_kind(void)
{
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_CONTACT));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_MOTION, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_MOTION));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_SMOKE, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_FIRE));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_CO, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_CO));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_WATER, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_WATER));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GLASS_BREAK,
                          zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_GLASS_BREAK));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_PANIC_BUTTON,
                          zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_PERSONAL));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_KEYFOB, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_KEYFOB));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_VIBRATION,
                          zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_VIBRATION));
    /* a siren, a keypad and the CIE itself are not sensors */
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_WARNING));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_CIE));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_zb_class_of_zone_type(ZSRC_ZB_ZONE_TYPE_NONE));

    /* ZB-004: a device with no IAS cluster is read off its ordinary clusters instead. */
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_MOTION, zsrc_zb_class_of_clusters(ZSRC_ZB_CL_OCCUPANCY));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, zsrc_zb_class_of_clusters(ZSRC_ZB_CL_ON_OFF));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_TEMPERATURE, zsrc_zb_class_of_clusters(ZSRC_ZB_CL_TEMP));
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, zsrc_zb_class_of_clusters(ZSRC_ZB_CL_POWER_CFG));
    /* occupancy wins over a thermometer on the same endpoint: it is the security function */
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_MOTION,
                          zsrc_zb_class_of_clusters((uint16_t)(ZSRC_ZB_CL_OCCUPANCY | ZSRC_ZB_CL_TEMP)));

    /* And the suggestion the zone editor makes from it (docs/features/22). */
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_PERIMETER, zsrc_suggest_zone_type(ALARM_DEVCLASS_WINDOW));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_FIRE, zsrc_suggest_zone_type(ALARM_DEVCLASS_SMOKE));
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_CO, zsrc_suggest_zone_type(ALARM_DEVCLASS_CO));
}

/* ZB-007: the quirks table is data, matched on manufacturer and model as prefixes so one row
 * covers a family; an ordinary device matches nothing and is treated as standard. */
static void test_quirks_table(void)
{
    const zsrc_zb_quirk_t *q = zsrc_zb_quirk_for("_TZE200_abcdefgh", "TS0601");
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_TRUE((q->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) != 0u);

    q = zsrc_zb_quirk_for("_TZ3000_zzzz", "TS0203");
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_TRUE((q->quirks & ZSRC_ZB_QUIRK_NO_ENROLL) != 0u);
    TEST_ASSERT_TRUE((q->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u);

    /* A model row is more specific than the manufacturer row below it, so it has to come first
     * in the table for either to be reachable. */
    q = zsrc_zb_quirk_for("LUMI", "lumi.sensor_magnet.aq2");
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, q->device_class);
    q = zsrc_zb_quirk_for("LUMI", "lumi.something.new");
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_GENERIC, q->device_class);

    TEST_ASSERT_NULL(zsrc_zb_quirk_for("Frient A/S", "MOSZB-140"));
    TEST_ASSERT_NULL(zsrc_zb_quirk_for(NULL, NULL));

    /* Every row explains itself: the note is what the picker and the zone page show. */
    size_t n = 0;
    const zsrc_zb_quirk_t *table = zsrc_zb_quirk_table(&n);
    TEST_ASSERT_TRUE(n > 0u);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(table[i].note);
        TEST_ASSERT_TRUE(strlen(table[i].note) > 0u);
        TEST_ASSERT_TRUE(strlen(table[i].note) < 48u); /* it has to fit zsrc_candidate_t.detail */
    }
}

/* ZB-007: what the table says reaches the device - its kind, and how we treat it. */
static void test_quirks_reach_the_device(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "LUMI", "lumi.sensor_magnet");
    d.clusters = ZSRC_ZB_CL_ON_OFF;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_A, 1u);
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, dev->device_class);
    TEST_ASSERT_TRUE((dev->quirks & ZSRC_ZB_QUIRK_NO_ENROLL) != 0u);
    TEST_ASSERT_NOT_NULL(dev->note);

    /* An unknown but standard device keeps what its own zone type said. */
    const zsrc_zb_desc_t s = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_FIRE, "Frient A/S", "SMSZB-120");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &s, NOW));
    dev = zsrc_zb_device(&Z, IEEE_B, 1u);
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_SMOKE, dev->device_class);
    TEST_ASSERT_EQUAL_UINT32(0u, dev->quirks);
    TEST_ASSERT_NULL(dev->note);
}

/* ---- ZB-002: pairing happens only in a window the installer opened --------------------------- */

static void test_pairing_only_in_a_learn_window(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.permit_join);

    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZIGBEE, true, NOW));
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.permit_join);
    TEST_ASSERT_EQUAL_UINT16(ZSRC_ZB_LEARN_S, RADIO.permit_seconds);
    TEST_ASSERT_TRUE(Z.learning);

    /* The hub asks for a tick before the window is out, and the window shuts by itself. */
    const uint64_t next = zone_hub_next_deadline(&H, NOW);
    TEST_ASSERT_TRUE(next <= NOW + (uint64_t)ZSRC_ZB_LEARN_S * 1000u);
    advance((uint64_t)ZSRC_ZB_LEARN_S * 1000u - 1u);
    TEST_ASSERT_TRUE(Z.learning);
    advance(1u);
    TEST_ASSERT_FALSE(Z.learning);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.permit_join);
    TEST_ASSERT_EQUAL_UINT16(0u, RADIO.permit_seconds); /* closed again */

    /* Closing it by hand works too, and with no radio there is no window at all. */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZIGBEE, true, NOW));
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZIGBEE, false, NOW));
    TEST_ASSERT_FALSE(Z.learning);
    zsrc_zb_link(&Z, false, NOW);
    TEST_ASSERT_FALSE(zone_hub_learn(&H, ZSRC_ZIGBEE, true, NOW));
}

/* ZB-002: a device that joins while the window is open is the one being held to the door frame,
 * so the picker offers it first and marks it. */
static void test_new_joins_come_first_in_the_picker(void)
{
    const zsrc_zb_desc_t old = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_MOTION, "Frient A/S", "MOSZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &old, NOW));

    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZIGBEE, true, NOW));
    const zsrc_zb_desc_t fresh = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &fresh, NOW));

    zsrc_candidate_t c[8];
    const size_t n = zone_hub_candidates(&H, ZSRC_ZIGBEE, "", 0, c, 8);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_UINT64(IEEE_A, c[0].addr.id);
    TEST_ASSERT_TRUE(c[0].changed);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, c[0].device_class);
    TEST_ASSERT_EQUAL_STRING("WISZB", c[0].name);
    TEST_ASSERT_EQUAL_UINT64(IEEE_B, c[1].addr.id);
    TEST_ASSERT_FALSE(c[1].changed);

    /* Opening the window again forgets last time's newcomers. */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZIGBEE, false, NOW));
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_ZIGBEE, true, NOW));
    TEST_ASSERT_EQUAL_UINT(2u, zone_hub_candidates(&H, ZSRC_ZIGBEE, "", 0, c, 8));
    TEST_ASSERT_FALSE(c[0].changed);
    TEST_ASSERT_FALSE(c[1].changed);
}

/* ---- ZB-003: IAS Zone enrollment -------------------------------------------------------------- */

/* The straightforward case: the device joins, we write the CIE address, it asks to enroll and we
 * answer it with a zone id. */
static void test_enrollment_of_a_mains_powered_device(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB-120");
    d.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.cie_writes);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_CIE, dev->enroll);

    zsrc_zb_ias_enroll_request(&Z, IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.enroll_responses);
    TEST_ASSERT_EQUAL_UINT8(1u, RADIO.last_zone_id);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, dev->enroll);

    /* The next device gets the next zone id, and keeps it. */
    const zsrc_zb_desc_t d2 = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_MOTION, "Frient A/S", "MOSZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d2, NOW));
    zsrc_zb_ias_enroll_request(&Z, IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_MOTION, NOW);
    TEST_ASSERT_EQUAL_UINT8(2u, RADIO.last_zone_id);
    zsrc_zb_ias_enroll_request(&Z, IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_MOTION, NOW);
    TEST_ASSERT_EQUAL_UINT8(2u, RADIO.last_zone_id);
}

/* ZB-003, the known pitfall: a sleepy device (an SOS button, a contact) answers the enroll
 * request and goes straight back to sleep, so the response never lands. It has to be retried in
 * the device's next wake window - which is any frame it sends - and no faster. */
static void test_a_sleepy_device_is_enrolled_in_its_wake_windows(void)
{
    RADIO.awake = false;
    const zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_PERSONAL, "HEIMAN", "SOS-EM");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_ias_enroll_request(&Z, IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_PERSONAL, NOW);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_A, 1u);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.enroll_responses); /* tried, and missed */
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_RESPOND, dev->enroll);

    /* A burst of frames inside one gap must not burn the retries. */
    for (int i = 0; i < 5; i++) {
        NOW += 100u;
        zsrc_zb_frame_seen(&Z, IEEE_A, 120u, NOW);
    }
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.enroll_responses);

    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_frame_seen(&Z, IEEE_A, 120u, NOW);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.enroll_responses);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_RESPOND, dev->enroll);

    /* Awake at last. */
    RADIO.awake = true;
    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_frame_seen(&Z, IEEE_A, 120u, NOW);
    TEST_ASSERT_EQUAL_UINT(3u, RADIO.enroll_responses);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, dev->enroll);
    TEST_ASSERT_EQUAL_UINT8(120u, dev->lqi);

    /* ... and it is not pestered again. */
    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_frame_seen(&Z, IEEE_A, 120u, NOW);
    TEST_ASSERT_EQUAL_UINT(3u, RADIO.enroll_responses);
}

/* ZB-003: we do not retry for ever. A device that never answers stops being asked - but if it
 * reports its status anyway, which many do, it is a working sensor and is treated as one
 * (ZB-007 ZSRC_ZB_QUIRK_NO_ENROLL is the same story, declared in advance). */
static void test_enrollment_gives_up_but_the_sensor_still_works(void)
{
    RADIO.awake = false;
    const zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Nobody", "NoName");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_ias_enroll_request(&Z, IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_A, 1u);
    for (unsigned i = 1; i < ZSRC_ZB_ENROLL_TRIES + 2u; i++) {
        NOW += ZSRC_ZB_ENROLL_GAP_MS;
        zsrc_zb_frame_seen(&Z, IEEE_A, 0u, NOW);
    }
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_FAILED, dev->enroll);
    TEST_ASSERT_EQUAL_UINT(ZSRC_ZB_ENROLL_TRIES, RADIO.enroll_responses);

    /* It reports anyway: that is proof enough that it works. */
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, dev->enroll);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* ZB-007: a device declared as never enrolling is not chased at all - it just reports. */
static void test_a_device_that_never_enrolls(void)
{
    const zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "_TZ3000_26fmupbb", "TS0203");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_A, 1u);
    TEST_ASSERT_TRUE((dev->quirks & ZSRC_ZB_QUIRK_NO_ENROLL) != 0u);

    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.enroll_responses);
}

/* ---- ZB-003: the status word onto zone events ------------------------------------------------ */

static void test_ias_status_bits_become_zone_events(void)
{
    const zsrc_address_t a = pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    (void)a;

    /* The first report tells the zone where it stands, even when nothing is wrong. */
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER_RESTORE, 0); /* nothing to restore */

    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 2);

    /* Tamper, low battery and trouble, each with its restore. */
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_TAMPER, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER, 1);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, (uint16_t)(ZSRC_ZB_IAS_TAMPER | ZSRC_ZB_IAS_BATTERY), NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY, 1);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER, 1); /* unchanged: not said twice */
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_TROUBLE, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER_RESTORE, 1);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY_RESTORE, 1);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TROUBLE, 1);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TROUBLE_RESTORE, 1);

    /* The bits that only describe the device (supervision and restore reporting, mains, test)
     * are not conditions of the zone. */
    NOW += 1000u;
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1u,
                       (uint16_t)(ZSRC_ZB_IAS_SUPERVISION | ZSRC_ZB_IAS_RESTORE |
                                  ZSRC_ZB_IAS_AC_MAINS | ZSRC_ZB_IAS_TEST),
                       NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TROUBLE, 0);

    /* A defective battery is a low battery. */
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_BATT_DEFECT, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY, 1);
}

/* ZB-003: either alarm bit faults the zone, because on a panel a missed break-in is worse than a
 * fault the owner can see; a device known to use only Alarm2 is read that way (ZB-007). */
static void test_alarm_bits_and_the_alarm2_quirk(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_MOTION);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM2, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);

    /* Now the same device with the quirk set by hand, as the table would. */
    zsrc_zb_device_t *dev = (zsrc_zb_device_t *)zsrc_zb_device(&Z, IEEE_A, 1u);
    dev->quirks = ZSRC_ZB_QUIRK_ALARM2;
    NOW += 1000u;
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1); /* Alarm1 is noise on this model */
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM2, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* ZSRC-006 / ZB-007: the binding's 'invert' turns the sensor round, and so does a quirk; both
 * together cancel out. */
static void test_invert_from_the_binding_and_from_a_quirk(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    b.in[ZSRC_ROLE_MAIN].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    clear_records();

    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);

    zsrc_zb_device_t *dev = (zsrc_zb_device_t *)zsrc_zb_device(&Z, IEEE_A, 1u);
    dev->quirks = ZSRC_ZB_QUIRK_INVERT;
    NOW += 1000u;
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1); /* two inversions is none */
}

/* ZB-007: a model whose tamper bit is noise has it ignored, and only it. */
static void test_the_no_tamper_quirk(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_device_t *dev = (zsrc_zb_device_t *)zsrc_zb_device(&Z, IEEE_A, 1u);
    dev->quirks = ZSRC_ZB_QUIRK_NO_TAMPER;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, (uint16_t)(ZSRC_ZB_IAS_TAMPER | ZSRC_ZB_IAS_ALARM1), NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* ZB-003, end to end: a paired Zigbee contact on an armed partition is a burglary alarm, and the
 * engine never learns that a radio was involved (ZSRC-001). */
static void test_a_zigbee_contact_raises_a_real_alarm(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
    TEST_ASSERT_TRUE(sim_status(&E, 1).ready);

    sim_arm_away_and_leave(&E, CODE_USER);
    EXPECT_STATE(&E, 1, ALARM_STATE_ARMED);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_TRUE(sim_status(&E, 1).triggered);
    advance(SEC(31)); /* the abort window has to run before it is reported */
    TEST_ASSERT_TRUE(sim_alarm_reported(&E, Z_WINDOW, ALARM_CAUSE_BURGLARY));

    /* Tamper on a Zigbee sensor is a tamper on its zone. */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&E, 1, CODE_USER));
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_TAMPER, NOW);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_TAMPER);
}

/* ---- ZB-028 (B020): a burst's re-read ------------------------------------------------------ */

/* The zone's last open or closed event: what it shows now. */
static alarm_zone_event_t last_state(uint16_t zone)
{
    alarm_zone_event_t ev = ALARM_ZEV_CHECKIN;
    for (unsigned i = 0; i < NR; i++) {
        if (R[i].zone == zone && (R[i].ev == ALARM_ZEV_FAULT || R[i].ev == ALARM_ZEV_RESTORE)) {
            ev = R[i].ev;
        }
    }
    return ev;
}

/* A contact like the SNZB-04PR2 on the bench (B020): its magnet bounced, it sent open, closed,
 * open 60 ms apart and left out its last change - the door is shut. The panel followed every
 * notification; once the sensor has been quiet for ZSRC_ZB_REREAD_MS it reads zoneStatus, and the
 * sensor's own answer (closed) wins. One read, and none after it. */
static void test_a_burst_is_read_again_and_the_answer_wins(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(60000u);
    const uint16_t sent[3] = {ZSRC_ZB_IAS_ALARM1, 0u, ZSRC_ZB_IAS_ALARM1};
    for (unsigned i = 0; i < 3u; i++) {
        advance(60u);
        zsrc_zb_ias_status(&Z, IEEE_A, 1u, sent[i], NOW);
    }
    TEST_ASSERT_EQUAL(ALARM_ZEV_FAULT, last_state(Z_FRONT)); /* what it said last */
    advance(ZSRC_ZB_REREAD_MS - 100u);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.reads); /* not while it may still be bouncing */
    advance(100u);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, RADIO.last_read_cluster);
    TEST_ASSERT_EQUAL_HEX16(0x0002u, RADIO.last_read_attr);
    advance(300u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW); /* its answer: closed */
    TEST_ASSERT_EQUAL(ALARM_ZEV_RESTORE, last_state(Z_FRONT));
    advance(ZSRC_ZB_REREAD_WAIT_MS * 3u);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads); /* the answer brought no burst of its own */
}

/* ZB-028: a door opened once, or opened and shut a while later, is no burst: no read. */
static void test_a_plain_change_is_not_read_again(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(1000u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    advance(ZSRC_ZB_BURST_MS + 1000u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(ZSRC_ZB_REREAD_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.reads);
    /* The same status again (a periodic report) is no change either. */
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(100u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(ZSRC_ZB_REREAD_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.reads);
}

/* ZB-028: a sleepy sensor that went back to sleep before its parent could hand it the read: no
 * answer within ZSRC_ZB_REREAD_WAIT_MS, so it is asked again at its next frame (its check-in),
 * ZSRC_ZB_REREAD_TRIES times in all, then left alone. A device not bound to a zone is never read. */
static void test_an_unanswered_reread_is_asked_at_the_next_frame(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(60000u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    advance(80u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    advance(ZSRC_ZB_REREAD_MS);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    advance(ZSRC_ZB_REREAD_WAIT_MS * 6u);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads); /* asleep: the tick doesn't ask it again */
    zsrc_zb_frame_seen(&Z, IEEE_A, 0u, NOW); /* its check-in */
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.reads);
    advance(ZSRC_ZB_REREAD_WAIT_MS * 6u);
    zsrc_zb_frame_seen(&Z, IEEE_A, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.reads); /* two tries, then it is left alone */

    /* Unbound, a burst owes nothing. */
    const zsrc_zb_desc_t d = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "MOSZB-1xx");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_ias_status(&Z, IEEE_B, 1u, 0u, NOW);
    advance(1000u);
    zsrc_zb_ias_status(&Z, IEEE_B, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    advance(80u);
    zsrc_zb_ias_status(&Z, IEEE_B, 1u, 0u, NOW);
    advance(ZSRC_ZB_REREAD_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.reads);
}

/* ZB-028 / B020, end to end: on an armed partition an opening that lasted 80 ms - a bounce, then
 * closed - is a burglary alarm all the same: the engine is told every change, in order, and a
 * re-read that says closed restores the zone without undoing the alarm. */
static void test_a_momentary_opening_while_armed_counts(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    sim_arm_away_and_leave(&E, CODE_USER);
    EXPECT_STATE(&E, 1, ALARM_STATE_ARMED);
    advance(1000u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    advance(80u);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    TEST_ASSERT_TRUE(sim_status(&E, 1).triggered);
    advance(ZSRC_ZB_REREAD_MS);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW); /* the re-read: closed */
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_FAULT);
    TEST_ASSERT_TRUE(sim_status(&E, 1).triggered);
    advance(SEC(31));
    TEST_ASSERT_TRUE(sim_alarm_reported(&E, Z_WINDOW, ALARM_CAUSE_BURGLARY));
}

/* ZB-003 / ZSRC-008: a Zigbee smoke detector may feed a fire zone, and does. */
static void test_a_zigbee_smoke_detector_on_a_fire_zone(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_SMOKE, IEEE_A, ZSRC_ZB_ZONE_TYPE_FIRE);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_TRUE(sim_status(&E, 1).triggered);
    TEST_ASSERT_TRUE(sim_alarm_reported(&E, Z_SMOKE, ALARM_CAUSE_FIRE));
}

/* ---- ZB-004: the clusters that are not IAS --------------------------------------------------- */

/* A contact that reports as a switch (Aqara, IKEA) and a motion sensor that reports occupancy. */
static void test_on_off_and_occupancy_sensors(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "LUMI", "lumi.sensor_magnet");
    d.clusters = ZSRC_ZB_CL_ON_OFF;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    clear_records();

    zsrc_zb_attr_bool(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_ON_OFF, true, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    NOW += 1000u;
    zsrc_zb_attr_bool(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_ON_OFF, false, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1);

    /* Occupancy, on a second device, with the binding inverted for a sensor wired the other way
     * round: the hub's own mapping does that, off the words this source sends. */
    zsrc_zb_desc_t m = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "IKEA of Sweden",
                                "TRADFRI motion sensor");
    m.clusters = ZSRC_ZB_CL_OCCUPANCY;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &m, NOW));
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_B, 1u);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_MOTION, dev->device_class);
    zone_hub_binding_t mb = zb_binding(IEEE_B, 1u);
    mb.in[ZSRC_ROLE_MAIN].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_HALL_PIR, &mb, NOW));
    clear_records();
    zsrc_zb_attr_bool(&Z, IEEE_B, 1u, ZSRC_ZB_ZCL_OCCUPANCY, true, NOW);
    EXPECT_POSTED(Z_HALL_PIR, ALARM_ZEV_RESTORE, 1);
    NOW += 1000u;
    zsrc_zb_attr_bool(&Z, IEEE_B, 1u, ZSRC_ZB_ZCL_OCCUPANCY, false, NOW);
    EXPECT_POSTED(Z_HALL_PIR, ALARM_ZEV_FAULT, 1);
}

/* ZB-004: a thermometer is a freeze zone. The mapping decides which of a device's functions the
 * zone is looking at, so a sensor that has both an alarm and a temperature is unambiguous. */
static void test_a_thermometer_is_a_freeze_zone(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_WATER, "Frient A/S", "FLSZB-110");
    d.clusters = (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_TEMP);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));

    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_TEMPERATURE);
    TEST_ASSERT_EQUAL_INT(ZSRC_MAP_BELOW, b.in[ZSRC_ROLE_MAIN].map.mode);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FREEZER, &b, NOW));
    clear_records();

    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 1850, NOW); /* 18.50 C */
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_RESTORE, 1); /* the first reading settles the zone */
    NOW += 1000u;
    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 400, NOW); /* 4.00 C: freezing */
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_FAULT, 1);
    NOW += 1000u;
    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 550, NOW); /* inside the hysteresis */
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_RESTORE, 1); /* still freezing: nothing new is said */
    NOW += 1000u;
    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 700, NOW);
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_RESTORE, 2);

    /* The same device's water alarm is not this zone's business. */
    NOW += 1000u;
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_FAULT, 0);
}

/* ZB-004: the battery, whether the device sends a percentage, an IAS bit, or both. They can
 * never contradict each other on the zone page, because there is one answer. */
static void test_battery_from_a_percentage_and_from_the_status_word(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);

    zsrc_zb_battery(&Z, IEEE_A, 80u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY, 0);
    NOW += 1000u;
    zsrc_zb_battery(&Z, IEEE_A, ZSRC_ZB_BATTERY_LOW_PCT, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY, 1);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_BATTERY_LOW_PCT, zsrc_zb_device(&Z, IEEE_A, 1u)->battery_pct);
    NOW += 1000u;
    zsrc_zb_battery(&Z, IEEE_A, 19u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY, 1); /* still low: not said again */
    NOW += 1000u;
    zsrc_zb_battery(&Z, IEEE_A, 60u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY_RESTORE, 1);

    /* The IAS bit on its own says the same thing, and a fresh percentage does not undo it while
     * the device is still complaining. */
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_BATTERY, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY, 2);
    NOW += 1000u;
    zsrc_zb_battery(&Z, IEEE_A, 90u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY_RESTORE, 1);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_LOW_BATTERY_RESTORE, 2);
}

/* ---- ZB-005: supervision ---------------------------------------------------------------------- */

/* A sleepy sensor that stops checking in is CHECK - after its own window and then the hub's
 * grace, not before. A mains-powered router is given much less rope. */
static void test_a_silent_sensor_becomes_check(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    TEST_ASSERT_TRUE(sim_status(&E, 1).ready);

    advance((uint64_t)ZSRC_ZB_SUPERVISION_S * 1000u - 1000u);
    TEST_ASSERT_FALSE(zsrc_zb_device(&Z, IEEE_A, 1u)->unreachable);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);

    advance(1000u);
    TEST_ASSERT_TRUE(zsrc_zb_device(&Z, IEEE_A, 1u)->unreachable);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 0); /* the hub's grace has not run yet */
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);

    advance((uint64_t)ZSRC_ZB_GRACE_S * 1000u);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 1);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    TEST_ASSERT_FALSE(sim_status(&E, 1).ready); /* CORE-TRBL-013: it blocks arming */

    /* One frame and it is back. */
    NOW += 1000u;
    zsrc_zb_frame_seen(&Z, IEEE_A, 90u, NOW);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    TEST_ASSERT_TRUE(sim_status(&E, 1).ready);
}

static void test_a_router_is_supervised_sooner(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    d.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    clear_records();

    advance((uint64_t)ZSRC_ZB_ROUTER_SUPERVISION_S * 1000u - 1000u);
    TEST_ASSERT_FALSE(zsrc_zb_device(&Z, IEEE_A, 1u)->unreachable);
    advance(1000u);
    TEST_ASSERT_TRUE(zsrc_zb_device(&Z, IEEE_A, 1u)->unreachable);
}

/* ZB-005 / ZSRC-007: a device that leaves the network is its zone's CHECK, and the binding is
 * kept so the installer can see what is missing and re-pair it (ZSRC-010). */
static void test_a_device_that_leaves_is_check_and_the_binding_stays(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);

    zsrc_zb_device_gone(&Z, IEEE_A, NOW);
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, IEEE_A, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_ON); /* one device is not the radio's trouble */
    advance((uint64_t)ZSRC_ZB_GRACE_S * 1000u);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);

    zone_hub_binding_t out;
    TEST_ASSERT_TRUE(zone_hub_get(&H, Z_WINDOW, &out));
    TEST_ASSERT_EQUAL_UINT64(IEEE_A, out.in[ZSRC_ROLE_MAIN].addr.id);

    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, Z_WINDOW, &info));
    TEST_ASSERT_TRUE(info.check);
    TEST_ASSERT_TRUE(info.source_present);
    TEST_ASSERT_EQUAL_STRING("0x00158D0001A2B3C4 (not paired)", info.address);
}

/* ZSRC-006: an unchanged report keeps the engine's own supervision timer alive, but at most
 * once a minute however chatty the device is. */
static void test_check_ins_are_rate_limited(void)
{
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_frame_seen(&Z, IEEE_A, 0u, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_CHECKIN, 1);
    for (int i = 0; i < 20; i++) {
        NOW += 1000u;
        zsrc_zb_frame_seen(&Z, IEEE_A, 0u, NOW);
    }
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_CHECKIN, 1);
    NOW += ZSRC_ZB_CHECKIN_EVERY_MS;
    zsrc_zb_frame_seen(&Z, IEEE_A, 0u, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_CHECKIN, 2);
}

/* ---- ZB-005 / ZSRC-015: the radio itself ----------------------------------------------------- */

/* The radio not answering is one trouble for the source, never a CHECK per zone; and with it
 * back, nothing it said before counts until each sensor has spoken for itself. */
static void test_the_radio_going_away_is_one_trouble(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    pair_and_bind(Z_FRONT, IEEE_B, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    zsrc_zb_ias_status(&Z, IEEE_B, 1u, 0u, NOW);
    clear_records();

    zsrc_zb_link(&Z, false, NOW);
    advance((uint64_t)ZSRC_ZB_GRACE_S * 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON); /* one message, not one per zone */
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_UNREACHABLE, 0);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);

    /* Back, but on probation: both sensors have their grace to report again. */
    zsrc_zb_link(&Z, true, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_OFF);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_OFF); /* the other one still owes us a word */
    advance((uint64_t)ZSRC_ZB_GRACE_S * 1000u);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_UNREACHABLE, 1);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    EXPECT_ZONE_FLAG(&E, Z_FRONT, ALARM_ZS_SUPERVISION);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF); /* every sensor has now reported or shown CHECK */
}

/* ZB-005: with the link back, a mains-powered router is asked outright rather than waited for. */
static void test_routers_are_polled_when_the_radio_comes_back(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    d.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    RADIO.reads = 0;

    zsrc_zb_link(&Z, false, NOW);
    zsrc_zb_link(&Z, true, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    /* ZB31: a contact zone asks for what it watches, the IAS Zone Status. */
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, RADIO.read_cluster[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0002u, RADIO.read_attr[0]);
}

#define IEEE_SLEEPY 0x02ABCD0000000201ull /* fictional, locally administered (AGENTS.md) */

/* A mains-powered router with the clusters given, paired and bound to `zone`. */
static void pair_router(uint64_t ieee, uint16_t clusters, uint16_t zone, zsrc_map_mode_t mode)
{
    zsrc_zb_desc_t d = ias_desc(ieee, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "Generic", "router");
    d.clusters = clusters;
    d.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zone_hub_binding_t b = zb_binding(ieee, 1u);
    b.in[ZSRC_ROLE_MAIN].map.mode = (uint8_t)mode;
    if (mode == ZSRC_MAP_BELOW) {
        b.in[ZSRC_ROLE_MAIN].map.threshold_x100 = 500; /* below 5.00 C */
    }
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
    clear_records();
}

/* ZB31 (ZB-004, ZB-005, ZSRC-015): with the radio back, a freeze zone on an endpoint that also
 * has an alarm or a switch cluster asks for the temperature it watches, so its first answer
 * settles the zone. An endpoint with IAS Zone is asked for its status too: its tamper, trouble
 * and battery bits are this zone's business whatever the mapping. */
static void numeric_router_reconnect(uint16_t clusters)
{
    pair_router(IEEE_A, clusters, Z_FREEZER, ZSRC_MAP_BELOW);
    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 100, NOW); /* 1.00 C: freezing */
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_FAULT, 1);
    RADIO.reads = 0;

    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    const bool ias = (clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u;
    TEST_ASSERT_EQUAL_UINT(ias ? 2u : 1u, RADIO.reads);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_TEMP_MEAS, RADIO.read_cluster[0]); /* the watched input first */
    TEST_ASSERT_EQUAL_HEX16(0x0000u, RADIO.read_attr[0]);
    if (ias) {
        TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, RADIO.read_cluster[1]);
        TEST_ASSERT_EQUAL_HEX16(0x0002u, RADIO.read_attr[1]);
    }

    /* The answers: an IAS status says nothing about the temperature; the temperature does. */
    clear_records();
    if (ias) {
        zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, ++NOW);
        EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_FAULT, 0);
        EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_RESTORE, 0);
    }
    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 450, ++NOW); /* still below */
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_RESTORE, 0);
    zsrc_zb_attr_value(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, 1800, ++NOW);
    EXPECT_POSTED(Z_FREEZER, ALARM_ZEV_RESTORE, 1);
}

static void test_reconnect_reads_temperature_on_an_ias_and_temperature_router(void)
{
    numeric_router_reconnect((uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_TEMP));
}

static void test_reconnect_reads_temperature_on_an_occupancy_and_temperature_router(void)
{
    numeric_router_reconnect((uint16_t)(ZSRC_ZB_CL_OCCUPANCY | ZSRC_ZB_CL_TEMP));
}

static void test_reconnect_reads_temperature_on_an_on_off_and_temperature_router(void)
{
    numeric_router_reconnect((uint16_t)(ZSRC_ZB_CL_ON_OFF | ZSRC_ZB_CL_TEMP));
}

/* ZB31 (ZB-004, ZB-005): the binary zones on the same mixed endpoints, and the ones that are not
 * asked at all. */
static void test_reconnect_reads_the_binary_input_a_zone_watches(void)
{
    /* IAS and temperature, bound as a contact: the status, once. */
    pair_router(IEEE_A, (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_TEMP), Z_WATER,
                ZSRC_MAP_BINARY);
    RADIO.reads = 0;
    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, RADIO.read_cluster[0]);
    zone_hub_unbind(&H, Z_WATER, NOW);

    /* A switch-only contact, and occupancy before on/off on a motion sensor with both. */
    pair_router(IEEE_B, (uint16_t)(ZSRC_ZB_CL_ON_OFF | ZSRC_ZB_CL_TEMP), Z_FRONT, ZSRC_MAP_BINARY);
    RADIO.reads = 0;
    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_ON_OFF, RADIO.read_cluster[0]);
    zone_hub_unbind(&H, Z_FRONT, NOW);

    pair_router(IEEE_C, (uint16_t)(ZSRC_ZB_CL_ON_OFF | ZSRC_ZB_CL_OCCUPANCY), Z_HALL_PIR,
                ZSRC_MAP_BINARY);
    RADIO.reads = 0;
    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_OCCUPANCY, RADIO.read_cluster[0]);
    zone_hub_unbind(&H, Z_HALL_PIR, NOW);

    /* A sleepy freeze sensor is waited for, not asked. */
    zsrc_zb_desc_t s = ias_desc(IEEE_SLEEPY, 1u, ZSRC_ZB_ZONE_TYPE_WATER, "Generic", "sleepy");
    s.clusters = (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_TEMP);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &s, NOW));
    zone_hub_binding_t b = zb_binding(IEEE_SLEEPY, 1u);
    b.in[ZSRC_ROLE_MAIN].map.mode = ZSRC_MAP_BELOW;
    b.in[ZSRC_ROLE_MAIN].map.threshold_x100 = 500;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FREEZER, &b, NOW));
    RADIO.reads = 0;
    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.reads);
}

/* ZB32 (ZB-004: On/Off and Occupancy are for contacts and motion that don't use IAS): on an
 * endpoint with IAS Zone, a switch or occupancy report never moves the zone the alarm feeds. */
static void boolean_never_overrides_ias(uint64_t ieee, uint16_t cluster, uint16_t bit,
                                        uint16_t zone, bool invert)
{
    zsrc_zb_desc_t d = ias_desc(ieee, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Generic", "mixed");
    d.clusters = (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | bit);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zone_hub_binding_t b = zb_binding(ieee, 1u);
    b.in[ZSRC_ROLE_MAIN].map.invert = invert;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
    clear_records();
    const alarm_zone_event_t open = invert ? ALARM_ZEV_RESTORE : ALARM_ZEV_FAULT;
    const alarm_zone_event_t shut = invert ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE;

    zsrc_zb_ias_status(&Z, ieee, 1u, ZSRC_ZB_IAS_ALARM1, ++NOW); /* the IAS alarm: open */
    EXPECT_POSTED(zone, open, 1);
    zsrc_zb_attr_bool(&Z, ieee, 1u, cluster, false, ++NOW);
    zsrc_zb_attr_bool(&Z, ieee, 1u, cluster, true, ++NOW);
    zsrc_zb_attr_bool(&Z, ieee, 1u, cluster, false, ++NOW);
    EXPECT_POSTED(zone, open, 1);
    EXPECT_POSTED(zone, shut, 0); /* the IAS alarm is still on: nothing restored it */
    zsrc_zb_ias_status(&Z, ieee, 1u, ZSRC_ZB_IAS_ALARM1, ++NOW); /* unchanged: nothing new */
    EXPECT_POSTED(zone, open, 1);
    zsrc_zb_ias_status(&Z, ieee, 1u, 0u, ++NOW); /* only the IAS status closes it */
    EXPECT_POSTED(zone, shut, 1);
    zsrc_zb_attr_bool(&Z, ieee, 1u, cluster, true, ++NOW);
    EXPECT_POSTED(zone, open, 1);
    /* Its tamper still comes through. */
    zsrc_zb_ias_status(&Z, ieee, 1u, ZSRC_ZB_IAS_TAMPER, ++NOW);
    EXPECT_POSTED(zone, ALARM_ZEV_TAMPER, 1);
}

static void test_an_on_off_report_never_overrides_ias(void)
{
    boolean_never_overrides_ias(IEEE_A, ZSRC_ZB_ZCL_ON_OFF, ZSRC_ZB_CL_ON_OFF, Z_FRONT, false);
    boolean_never_overrides_ias(IEEE_B, ZSRC_ZB_ZCL_ON_OFF, ZSRC_ZB_CL_ON_OFF, Z_WINDOW, true);
}

static void test_an_occupancy_report_never_overrides_ias(void)
{
    boolean_never_overrides_ias(IEEE_A, ZSRC_ZB_ZCL_OCCUPANCY, ZSRC_ZB_CL_OCCUPANCY, Z_HALL_PIR,
                                false);
    boolean_never_overrides_ias(IEEE_B, ZSRC_ZB_ZCL_OCCUPANCY, ZSRC_ZB_CL_OCCUPANCY, Z_FAMILY,
                                true);
}

/* ZB32: reports, the replay to a new binding and the reconnect poll pick the same input. A mixed
 * IAS and On/Off endpoint whose switch alone has spoken replays nothing for its contact (the
 * switch is not what the zone watches) and is asked for its IAS status; once that is known the
 * replay is the status. */
static void test_replay_and_recovery_pick_the_input_reports_use(void)
{
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Generic", "mixed");
    d.clusters = (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_ON_OFF);
    d.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_attr_bool(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_ON_OFF, true, NOW);

    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, ++NOW));
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 0);

    RADIO.reads = 0;
    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.reads);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ZONE, RADIO.read_cluster[0]);

    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, ++NOW);
    zsrc_zb_attr_bool(&Z, IEEE_A, 1u, ZSRC_ZB_ZCL_ON_OFF, false, ++NOW);
    zone_hub_unbind(&H, Z_FRONT, ++NOW);
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, ++NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1); /* the IAS alarm, not the switch's "off" */
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_RESTORE, 0);

    /* Without IAS the switch is the input, for the replay as for its reports. */
    zsrc_zb_desc_t s = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "Generic", "switch");
    s.clusters = ZSRC_ZB_CL_ON_OFF;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &s, NOW));
    zsrc_zb_attr_bool(&Z, IEEE_B, 1u, ZSRC_ZB_ZCL_ON_OFF, true, ++NOW);
    zone_hub_binding_t sb = zb_binding(IEEE_B, 1u);
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &sb, ++NOW));
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* ZB-006: the channel drowned out for half a minute is the sensor radio's trouble - because a
 * jammed channel is exactly "no sensor can be heard" - and it clears when the noise stops. */
static void test_jamming_is_the_radios_trouble(void)
{
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    clear_records();

    zsrc_zb_energy(&Z, -80, NOW); /* a quiet channel */
    TEST_ASSERT_FALSE(zsrc_zb_jammed(&Z));

    zsrc_zb_energy(&Z, ZSRC_ZB_JAM_DBM, NOW);
    TEST_ASSERT_FALSE(zsrc_zb_jammed(&Z)); /* a microwave oven is not an attack */
    for (unsigned i = 0; i < 5; i++) {
        NOW += ZSRC_ZB_SCAN_MS;
        zsrc_zb_energy(&Z, -20, NOW);
    }
    NOW += ZSRC_ZB_SCAN_MS - 1u;
    zsrc_zb_energy(&Z, -20, NOW);
    TEST_ASSERT_FALSE(zsrc_zb_jammed(&Z));
    NOW += 1u;
    zsrc_zb_energy(&Z, -20, NOW);
    TEST_ASSERT_TRUE(zsrc_zb_jammed(&Z));
    TEST_ASSERT_FALSE(zone_hub_source_ops(&H, ZSRC_ZIGBEE)->usable(&Z)); /* no pairing now */

    /* ZB-006 (BL-104, decided 2026-09-28): the radio's trouble at once - the channel has been
     * drowned for half a minute already, so the hour of grace for a passing outage is skipped. */
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON);
    advance((uint64_t)ZSRC_ZB_GRACE_S * 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON); /* raised once */
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 0); /* not the zone's fault */

    NOW += 1000u;
    zsrc_zb_energy(&Z, -85, NOW);
    TEST_ASSERT_FALSE(zsrc_zb_jammed(&Z));
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF);

    /* The hub is asked for a tick while the jam timer is running. */
    zsrc_zb_energy(&Z, -10, NOW);
    TEST_ASSERT_TRUE(zone_hub_next_deadline(&H, NOW) <= NOW + ZSRC_ZB_JAM_HOLD_MS);
}

/* ---- the picker, binding and trust ------------------------------------------------------------ */

/* ZSRC-005 / ZB-007: what the installer sees, and what the panel refuses to build a zone on. */
static void test_the_picker_and_what_may_be_bound(void)
{
    /* a standard contact, a plug (nothing a zone could read), and a Tuya radio we cannot speak */
    const zsrc_zb_desc_t c = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB-120");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &c, NOW));
    zsrc_zb_desc_t plug = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "Frient A/S", "SPLZB-131");
    plug.clusters = ZSRC_ZB_CL_POWER_CFG;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &plug, NOW));
    zsrc_zb_desc_t tuya = ias_desc(IEEE_C, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "_TZE200_pay2byax", "TS0601");
    tuya.clusters = ZSRC_ZB_CL_TUYA;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &tuya, NOW));

    zsrc_candidate_t list[8];
    const size_t n = zone_hub_candidates(&H, ZSRC_ZIGBEE, "", 0, list, 8);
    TEST_ASSERT_EQUAL_UINT(2u, n); /* the plug is not offered */
    bool saw_contact = false;
    bool saw_tuya = false;
    for (size_t i = 0; i < n; i++) {
        if (list[i].addr.id == IEEE_A) {
            saw_contact = true;
            TEST_ASSERT_EQUAL_STRING("Frient A/S", list[i].detail);
        }
        if (list[i].addr.id == IEEE_C) {
            /* listed, so the installer can see that it joined - with the reason on the row */
            saw_tuya = true;
            TEST_ASSERT_TRUE(strstr(list[i].detail, "Home Assistant") != NULL);
        }
    }
    TEST_ASSERT_TRUE(saw_contact);
    TEST_ASSERT_TRUE(saw_tuya);

    /* Only the one we can actually read may become a zone. */
    zone_hub_binding_t ok = zb_binding(IEEE_A, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &ok, NOW));
    zone_hub_binding_t no = zb_binding(IEEE_C, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, Z_WINDOW, &no, NOW));
    zone_hub_binding_t stranger = zb_binding(0x1122334455667788ull, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, Z_WINDOW, &stranger, NOW));
    zone_hub_binding_t noep = zb_binding(IEEE_A, 0u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, Z_WINDOW, &noep, NOW));

    /* ZSRC-003: and that device now belongs to zone 1 alone. */
    TEST_ASSERT_EQUAL_UINT16(Z_FRONT, zone_hub_zone_of(&H, &ok.in[ZSRC_ROLE_MAIN].addr));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_IN_USE, zone_hub_bind(&H, Z_WINDOW, &ok, NOW));
}

/* ZSRC-009: the state the picker and the zone page show, in the sensor's own words. */
static void test_the_words_a_device_is_shown_with(void)
{
    const struct {
        uint16_t zone_type;
        uint16_t status;
        const char *words;
    } cases[] = {
        {ZSRC_ZB_ZONE_TYPE_CONTACT, ZSRC_ZB_IAS_ALARM1, "open"},
        {ZSRC_ZB_ZONE_TYPE_CONTACT, 0u, "closed"},
        {ZSRC_ZB_ZONE_TYPE_MOTION, ZSRC_ZB_IAS_ALARM1, "motion"},
        {ZSRC_ZB_ZONE_TYPE_MOTION, 0u, "clear"},
        {ZSRC_ZB_ZONE_TYPE_WATER, ZSRC_ZB_IAS_ALARM1, "wet"},
        {ZSRC_ZB_ZONE_TYPE_WATER, 0u, "dry"},
        {ZSRC_ZB_ZONE_TYPE_FIRE, ZSRC_ZB_IAS_ALARM1, "alarm"},
        {ZSRC_ZB_ZONE_TYPE_PERSONAL, ZSRC_ZB_IAS_ALARM1, "pressed"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint64_t ieee = IEEE_A + i;
        const zsrc_zb_desc_t d = ias_desc(ieee, 1u, cases[i].zone_type, "Frient A/S", "X");
        TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
        zsrc_zb_ias_status(&Z, ieee, 1u, cases[i].status, NOW);
    }
    zsrc_candidate_t list[16];
    const size_t n = zone_hub_candidates(&H, ZSRC_ZIGBEE, "", 0, list, 16);
    TEST_ASSERT_EQUAL_UINT(sizeof(cases) / sizeof(cases[0]), n);
    for (size_t i = 0; i < n; i++) {
        const size_t c = (size_t)(list[i].addr.id - IEEE_A);
        TEST_ASSERT_TRUE(c < sizeof(cases) / sizeof(cases[0]));
        TEST_ASSERT_EQUAL_STRING(cases[c].words, list[i].state);
    }

    /* A thermometer shows its reading instead. */
    zsrc_zb_desc_t t = ias_desc(IEEE_C, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "Frient A/S", "Thermo");
    t.clusters = ZSRC_ZB_CL_TEMP;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &t, NOW));
    zsrc_zb_attr_value(&Z, IEEE_C, 1u, ZSRC_ZB_ZCL_TEMP_MEAS, -150, NOW);
    const size_t n2 = zone_hub_candidates(&H, ZSRC_ZIGBEE, "", 0, list, 16);
    for (size_t i = 0; i < n2; i++) {
        if (list[i].addr.id == IEEE_C) {
            TEST_ASSERT_EQUAL_STRING("-1.50", list[i].state);
        }
    }
}

/* ZSRC-009: how the zone page names the sensor, including a device on a second endpoint. */
static void test_how_a_sensor_is_described(void)
{
    const zsrc_zb_desc_t a = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB-120");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &a, NOW));
    zsrc_zb_desc_t b = ias_desc(IEEE_A, 2u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB-120");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &b, NOW));

    zone_hub_binding_t bind1 = zb_binding(IEEE_A, 1u);
    zone_hub_binding_t bind2 = zb_binding(IEEE_A, 2u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &bind1, NOW));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &bind2, NOW));

    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, Z_FRONT, &info));
    TEST_ASSERT_EQUAL_STRING("WISZB-120", info.address);
    TEST_ASSERT_TRUE(zone_hub_info(&H, Z_WINDOW, &info));
    TEST_ASSERT_EQUAL_STRING("WISZB-120, endpoint 2", info.address);
    TEST_ASSERT_TRUE((info.caps & ZSRC_CAP_OFFLINE_OK) != 0u);

    char buf[24];
    TEST_ASSERT_EQUAL_STRING("0x00158D0001A2B3C4", zsrc_zb_ieee_str(IEEE_A, buf, sizeof(buf)));
}

/* ZSRC-008: Zigbee is the radio source that may carry fire, CO and panic zones without an
 * installer override, and it says so through its capability bits. */
static void test_trust_and_life_safety(void)
{
    const zone_source_ops_t *ops = zone_hub_source_ops(&H, ZSRC_ZIGBEE);
    TEST_ASSERT_NOT_NULL(ops);
    const uint32_t caps = ops->caps;
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_LIFE_SAFETY) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_OFFLINE_OK) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_SUPERVISED) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_AUTH) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_TAMPER) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_BATTERY) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_LEARN) != 0u);
    TEST_ASSERT_TRUE((caps & ZSRC_CAP_TEST) == 0u); /* it is not a test tool */

    TEST_ASSERT_TRUE(zsrc_trusted_for(caps, ALARM_ZT_FIRE));
    TEST_ASSERT_TRUE(zsrc_trusted_for(caps, ALARM_ZT_CO));
    TEST_ASSERT_TRUE(zsrc_trusted_for(caps, ALARM_ZT_24H_AUDIBLE));
    TEST_ASSERT_TRUE(zsrc_trusted_for(caps, ALARM_ZT_PERIMETER));

    /* ZSRC-005: and it can only be used while the radio is up. */
    TEST_ASSERT_TRUE(ops->usable(&Z));
    zsrc_zb_link(&Z, false, NOW);
    TEST_ASSERT_FALSE(ops->usable(&Z));
}

/* ZSRC-004 / ZB-008: a Zigbee binding is an IEEE address and an endpoint, and it survives being
 * written out and read back - which is how it survives a restart. */
static void test_bindings_survive_a_restart(void)
{
    const zsrc_zb_desc_t d = ias_desc(IEEE_A, 3u, ZSRC_ZB_ZONE_TYPE_MOTION, "Frient A/S", "MOSZB-140");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zone_hub_binding_t b = zb_binding(IEEE_A, 3u);
    b.in[ZSRC_ROLE_MAIN].map.grace_s = 900u;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_HALL_PIR, &b, NOW));

    uint8_t blob[ZONE_HUB_BLOB_MAX];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, blob, sizeof(blob), &len));
    TEST_ASSERT_TRUE(len > 0u);

    /* A fresh panel, reading what the old one stored. */
    zone_hub_t H2;
    const zone_hub_port_t port = {.ctx = NULL, .zone_event = NULL, .changed = NULL,
                                  .source_trouble = NULL};
    zone_hub_init(&H2, &port);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H2, blob, len));
    zone_hub_binding_t out;
    TEST_ASSERT_TRUE(zone_hub_get(&H2, Z_HALL_PIR, &out));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZIGBEE, out.in[ZSRC_ROLE_MAIN].addr.kind);
    TEST_ASSERT_EQUAL_UINT64(IEEE_A, out.in[ZSRC_ROLE_MAIN].addr.id);
    TEST_ASSERT_EQUAL_UINT8(3u, out.in[ZSRC_ROLE_MAIN].addr.input);
    TEST_ASSERT_EQUAL_UINT16(900u, out.in[ZSRC_ROLE_MAIN].map.grace_s);

    /* ZSRC-010: with the source compiled out the binding is still kept, and the zone is CHECK. */
    zone_hub_start(&H2, NOW);
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H2, Z_HALL_PIR, &info));
    TEST_ASSERT_FALSE(info.source_present);
}

/* ZSRC-010: a binding whose device has not turned up again yet is accepted and waits for it,
 * instead of being thrown away at start-up. */
static void test_a_binding_waits_for_its_device_to_reappear(void)
{
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    uint8_t blob[ZONE_HUB_BLOB_MAX];
    size_t len = 0;
    {
        /* build the blob on a hub that has the device, so the bind is accepted */
        const zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
        TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, blob, sizeof(blob), &len));
    }

    /* A restart: the radio is up but discovery has not found the device yet. */
    const zsrc_zb_radio_t radio = Z.radio; /* zsrc_zb_init clears what it is handed a pointer to */
    const zsrc_sink_t sink = Z.sink;
    zsrc_zb_init(&Z, &radio, &sink);
    const zone_hub_port_t port = {
        .ctx = NULL, .zone_event = port_event, .changed = NULL, .source_trouble = port_trouble};
    zone_hub_init(&H, &port);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, blob, len));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_zb_ops(), &Z, NOW));
    zone_hub_start(&H, NOW);
    zsrc_zb_link(&Z, true, NOW);
    clear_records();

    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, Z_WINDOW, &info));
    TEST_ASSERT_TRUE(info.source_present);
    TEST_ASSERT_FALSE(info.known);

    /* When it does turn up, and reports, the zone settles without anyone touching it. */
    const zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);
}

/* ZSRC-006: changing what a sensor means re-reads the last report under the new mapping at
 * once, and exactly once. */
static void test_changing_the_mapping_settles_the_zone_once(void)
{
    pair_and_bind(Z_WINDOW, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);

    clear_records();
    zone_hub_binding_t b = zb_binding(IEEE_A, 1u);
    b.in[ZSRC_ROLE_MAIN].map.invert = true;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_RESTORE, 1);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 0);
    EXPECT_LAST(Z_WINDOW, ALARM_ZEV_RESTORE);

    /* The hub re-reads its own stored state under the new mapping, radio or no radio - that is
     * ZSRC-006 and it is right: the installer changed what the contact means, not what it said.
     * ZSRC-015 is about something else, and about this source: a sensor the radio has not heard
     * from since the link broke must not be answered for. A zone bound while the radio is away
     * therefore hears nothing at all until the device itself speaks. */
    const zsrc_zb_desc_t second = ias_desc(IEEE_B, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &second, NOW));
    zsrc_zb_ias_status(&Z, IEEE_B, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    zsrc_zb_link(&Z, false, NOW);
    clear_records();
    zone_hub_binding_t b2 = zb_binding(IEEE_B, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b2, NOW));
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 0);

    /* With the radio back and the device heard from, the zone settles. */
    zsrc_zb_link(&Z, true, NOW);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_B, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* The table has an end: a panel with more devices than it can hold says so rather than
 * forgetting one quietly, and unbinding frees the slot it used. */
static void test_the_tables_have_limits(void)
{
    for (unsigned i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        const zsrc_zb_desc_t d = ias_desc(IEEE_A + i, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
        TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    }
    const zsrc_zb_desc_t one_too_many =
        ias_desc(IEEE_A + ZSRC_ZB_DEV_MAX, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
    TEST_ASSERT_FALSE(zsrc_zb_device_seen(&Z, &one_too_many, NOW));

    const zsrc_zb_device_t *list[ZSRC_ZB_DEV_MAX + 4];
    TEST_ASSERT_EQUAL_UINT(ZSRC_ZB_DEV_MAX, zsrc_zb_devices(&Z, list, ZSRC_ZB_DEV_MAX + 4u));

    zsrc_zb_device_gone(&Z, IEEE_A, NOW);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &one_too_many, NOW));

    /* An endpoint seen twice updates what we know instead of taking a second slot. */
    const size_t before = zsrc_zb_devices(&Z, list, ZSRC_ZB_DEV_MAX + 4u);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &one_too_many, NOW));
    TEST_ASSERT_EQUAL_UINT(before, zsrc_zb_devices(&Z, list, ZSRC_ZB_DEV_MAX + 4u));
}

/* Nothing is asked of a radio that is not there: a source built without one must not crash the
 * panel, it must simply be unusable. */
static void test_a_source_with_no_radio_behind_it(void)
{
    zsrc_zb_t bare;
    const zsrc_sink_t sink = {.ctx = NULL,
                              .input = sink_input,
                              .event = sink_event,
                              .reachable = sink_reachable,
                              .link = sink_link,
                              .link_now = sink_link_now};
    zsrc_zb_init(&bare, NULL, &sink);
    TEST_ASSERT_FALSE(zsrc_zb_ops()->usable(&bare));
    TEST_ASSERT_FALSE(zsrc_zb_ops()->learn(&bare, true, NOW));
    zsrc_zb_link(&bare, true, NOW);
    TEST_ASSERT_FALSE(zsrc_zb_ops()->learn(&bare, true, NOW)); /* no permit_join to call */
    const zsrc_zb_desc_t d = ias_desc(IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&bare, &d, NOW));
    zsrc_zb_ias_enroll_request(&bare, IEEE_A, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    zsrc_zb_ias_status(&bare, IEEE_A, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    zsrc_zb_ops()->tick(&bare, NOW);
    TEST_ASSERT_TRUE(zsrc_zb_ops()->next_tick(&bare, NOW) > NOW);
}

/* ZB-003/005: faults cleared while the radio was away must clear on its first report. */
static void test_first_report_after_reconnect_restores_tamper_and_trouble(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1, ZSRC_ZB_IAS_TAMPER | ZSRC_ZB_IAS_TROUBLE, NOW);
    zsrc_zb_link(&Z, false, ++NOW);
    zsrc_zb_link(&Z, true, ++NOW);
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_A, 1, 0, ++NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TAMPER_RESTORE, 1);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_TROUBLE_RESTORE, 1);
}

/* ZB-005: a queued report from before disconnect cannot restore a zone. */
static void test_reports_are_ignored_while_the_radio_is_down(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_link(&Z, false, ++NOW);
    clear_records();
    const uint64_t heard = zsrc_zb_device(&Z, IEEE_A, 1)->heard_at;
    zsrc_zb_ias_status(&Z, IEEE_A, 1, ZSRC_ZB_IAS_ALARM1, ++NOW);
    zsrc_zb_attr_bool(&Z, IEEE_A, 1, ZSRC_ZB_ZCL_ON_OFF, true, NOW);
    zsrc_zb_attr_value(&Z, IEEE_A, 1, ZSRC_ZB_ZCL_TEMP_MEAS, 1200, NOW);
    zsrc_zb_battery(&Z, IEEE_A, 8, NOW);
    zsrc_zb_frame_seen(&Z, IEEE_A, 120, NOW);
    TEST_ASSERT_EQUAL_UINT64(heard, zsrc_zb_device(&Z, IEEE_A, 1)->heard_at);
    TEST_ASSERT_EQUAL_UINT(0, NR);
}

/* ZB-003: repeated removal and pairing cannot exhaust the IAS ID allocator. */
static void test_ias_zone_ids_remain_unique_after_many_rejoins(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    for (unsigned i = 0; i < 300; i++) {
        zsrc_zb_ias_enroll_request(&Z, IEEE_B, 1, ZSRC_ZB_ZONE_TYPE_CONTACT, ++NOW);
        TEST_ASSERT_NOT_EQUAL(zsrc_zb_device(&Z, IEEE_A, 1)->zone_id,
                              zsrc_zb_device(&Z, IEEE_B, 1)->zone_id);
        zsrc_zb_device_gone(&Z, IEEE_B, NOW);
    }
    zsrc_zb_ias_enroll_request(&Z, IEEE_B, 1, ZSRC_ZB_ZONE_TYPE_CONTACT, ++NOW);
    zsrc_zb_ias_enroll_request(&Z, IEEE_C, 1, ZSRC_ZB_ZONE_TYPE_CONTACT, ++NOW);
    TEST_ASSERT_NOT_EQUAL(zsrc_zb_device(&Z, IEEE_B, 1)->zone_id,
                          zsrc_zb_device(&Z, IEEE_C, 1)->zone_id);
}

/* ZB-004: invalid temperature and unrelated clusters must never trip a freeze zone. */
static void test_invalid_cluster_reports_are_ignored(void)
{
    zsrc_zb_attr_bool(&Z, IEEE_A, 1, ZSRC_ZB_ZCL_POWER_CFG, true, NOW);
    zsrc_zb_attr_value(&Z, IEEE_A, 1, ZSRC_ZB_ZCL_TEMP_MEAS, -32768, NOW);
    zsrc_zb_attr_value(&Z, IEEE_A, 1, ZSRC_ZB_ZCL_POWER_CFG, 1200, NOW);
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, IEEE_A, 1));
    zsrc_zb_desc_t d = ias_desc(IEEE_A, 241, ZSRC_ZB_ZONE_TYPE_CONTACT, "F", "X");
    TEST_ASSERT_FALSE(zsrc_zb_device_seen(&Z, &d, NOW));
}

/* ZB-004: switching an IAS+temperature endpoint to numeric mapping replays temperature. */
static void test_numeric_rebind_replays_temperature_even_with_ias_status(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_WATER);
    zsrc_zb_ias_status(&Z, IEEE_A, 1, 0, NOW);
    zsrc_zb_attr_value(&Z, IEEE_A, 1, ZSRC_ZB_ZCL_TEMP_MEAS, 100, NOW);
    zone_hub_binding_t b = zb_binding(IEEE_A, 1);
    b.in[ZSRC_ROLE_MAIN].map.mode = ZSRC_MAP_BELOW;
    b.in[ZSRC_ROLE_MAIN].map.threshold_x100 = 500;
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, ++NOW));
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* ZB-004: the new zone must inherit a known low battery, even if another zone heard it. */
static void test_rebinding_replays_low_battery(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1, 0, NOW);
    zsrc_zb_battery(&Z, IEEE_A, 8, NOW);
    zone_hub_unbind(&H, Z_FRONT, ++NOW);
    zone_hub_binding_t b = zb_binding(IEEE_A, 1);
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_LOW_BATTERY, 1);
}

/* ZB-005/006: recovery from interference cannot replay a pre-jam contact state. */
static void test_jam_recovery_invalidates_cached_status(void)
{
    pair_and_bind(Z_FRONT, IEEE_A, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_A, 1, 0, NOW);
    zsrc_zb_energy(&Z, -20, NOW);
    for (unsigned i = 0; i < 6; i++) {
        NOW += ZSRC_ZB_SCAN_MS;
        zsrc_zb_energy(&Z, -20, NOW);
    }
    TEST_ASSERT_TRUE(zsrc_zb_jammed(&Z));
    TEST_ASSERT_FALSE(zsrc_zb_ops()->learn(&Z, true, NOW));
    zsrc_zb_energy(&Z, -85, ++NOW);
    TEST_ASSERT_FALSE(zsrc_zb_device(&Z, IEEE_A, 1)->status_known);
    TEST_ASSERT_EQUAL_UINT64(0, zsrc_zb_device(&Z, IEEE_A, 1)->heard_at);
}

/* ZB-006: a single high reading or a gap in scan results cannot establish sustained noise. */
static void test_jamming_requires_fresh_samples_and_scans_continue(void)
{
    zsrc_zb_energy(&Z, -20, NOW);
    advance(ZSRC_ZB_JAM_HOLD_MS);
    TEST_ASSERT_FALSE(zsrc_zb_jammed(&Z));
    TEST_ASSERT_TRUE(RADIO.scans > 0);
    zsrc_zb_energy(&Z, -20, NOW);
    TEST_ASSERT_FALSE(zsrc_zb_jammed(&Z));
    for (unsigned i = 0; i < 6; i++) {
        NOW += ZSRC_ZB_SCAN_MS;
        zsrc_zb_energy(&Z, -20, NOW);
    }
    TEST_ASSERT_TRUE(zsrc_zb_jammed(&Z));
    const unsigned scans = RADIO.scans;
    advance(ZSRC_ZB_SCAN_MS);
    TEST_ASSERT_TRUE(RADIO.scans > scans);
}

/* ==== BL-143 / ZB-M2: the source's new calls ===================================================== */

#define IEEE_F 0x02ABCD0000000101ull /* fictional, locally administered (AGENTS.md) */
#define IEEE_G 0x02ABCD0000000102ull

static zsrc_zb_report_t report(uint8_t kind, uint64_t ieee)
{
    zsrc_zb_report_t r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.ieee = ieee;
    r.nwk = 0x1A01u;
    return r;
}

static void done(uint32_t tag, zsrc_zb_done_t result, bool want_ours)
{
    zsrc_zb_report_t r = report(ZSRC_ZB_REP_DONE, 0u);
    r.tag = tag;
    r.u.done.result = (uint8_t)result;
    TEST_ASSERT_EQUAL(want_ours, zsrc_zb_on_report(&Z, &r, NOW));
}

/* Pair, enroll and bind a device that listens all the time (a mains contact or siren). */
static zsrc_address_t pair_router_and_bind(uint16_t zone, uint64_t ieee)
{
    zsrc_zb_desc_t d = ias_desc(ieee, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB-120");
    d.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    zsrc_zb_ias_enroll_request(&Z, ieee, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    zone_hub_binding_t b = zb_binding(ieee, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
    clear_records();
    return b.in[ZSRC_ROLE_MAIN].addr;
}

/* A walk test runs and its scope covers this device's endpoint 1 (zbz_panel_walk_scope's work). */
static void walk_on(uint64_t ieee)
{
    zsrc_zb_walk_test(&Z, true, NOW);
    const zsrc_address_t a = zsrc_zb_address(ieee, 1u);
    zsrc_zb_walk_test_input(&Z, &a, true, NOW);
}

/* Review finding 7: the setup's steps are in the order devices.md §5 runs them, POWER_DESC after
 * BASIC (it is asked only when Basic has no power source), DONE last (ZB-012). */
static void test_the_setup_steps_are_in_order(void)
{
    TEST_ASSERT_EQUAL_INT(0, ZSRC_ZB_STEP_NONE);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_NODE_DESC + 1, ZSRC_ZB_STEP_ACTIVE_EP);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_ACTIVE_EP + 1, ZSRC_ZB_STEP_SIMPLE_DESC);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_SIMPLE_DESC + 1, ZSRC_ZB_STEP_BASIC);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_BASIC + 1, ZSRC_ZB_STEP_POWER_DESC);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_POWER_DESC + 1, ZSRC_ZB_STEP_IAS_ZONE);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_IAS_ZONE + 1, ZSRC_ZB_STEP_CIE_WRITE);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_STEP_POLL_CTRL + 1, ZSRC_ZB_STEP_DONE);
    TEST_ASSERT_EQUAL_INT(12, ZSRC_ZB_STEP_DONE);
}

/* ZB-013: Forget drops the device without its answer. Its zone loses its sensor at once and goes
 * CHECK after a second (grace_of), not after the hour a passing silence gets; nothing it said
 * replays, and its traffic - its key may outlive the forget, devices.md §6 - does not bring it
 * back. Only a new join (zbz_panel describing it again) does, with nothing of its old state. */
static void test_forget_drops_the_device_and_nothing_replays(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    const zsrc_address_t a = pair_and_bind(Z_WINDOW, IEEE_F, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);
    const zone_source_ops_t *ops = zone_hub_source_ops(&H, ZSRC_ZIGBEE);
    TEST_ASSERT_EQUAL_UINT32(0u, ops->grace_of(&Z, &a)); /* known: the source's hour */

    clear_records();
    zsrc_zb_device_forget(&Z, IEEE_F, NOW);
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, IEEE_F, 1u));
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_GONE_GRACE_S, ops->grace_of(&Z, &a));
    advance(2000u);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 1);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
    zone_hub_binding_t out;
    TEST_ASSERT_TRUE(zone_hub_get(&H, Z_WINDOW, &out)); /* the binding stays (ZSRC-010) */

    /* Its traffic: no slot, no event, no candidate. */
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    zsrc_zb_attr_bool(&Z, IEEE_F, 1u, ZSRC_ZB_ZCL_ON_OFF, true, NOW);
    zsrc_zb_frame_seen(&Z, IEEE_F, 200u, NOW);
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, IEEE_F, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, NR);
    zsrc_candidate_t c[4];
    TEST_ASSERT_EQUAL_UINT(0u, ops->candidates(&Z, "", 0u, c, 4u));

    /* Forgetting an address nobody has, or 0, is harmless. */
    zsrc_zb_device_forget(&Z, IEEE_G, NOW);
    zsrc_zb_device_forget(&Z, 0u, NOW);

    /* Added again: described afresh, it knows nothing from before. */
    const zsrc_zb_desc_t d = ias_desc(IEEE_F, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    TEST_ASSERT_EQUAL_UINT32(0u, ops->grace_of(&Z, &a));
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_F, 1u);
    TEST_ASSERT_FALSE(dev->status_known);
    clear_records();
    zone_hub_binding_t b = zb_binding(IEEE_F, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 0);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_RESTORE, 1);
    EXPECT_NO_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);
}

/* ZB-013: the forgotten list has an end (every registry device since the review of wave 6); past
 * it the newest are remembered, the oldest keep the normal grace - never a wrong answer, only a
 * slower CHECK. */
static void test_the_forgotten_list_keeps_the_newest(void)
{
    const zone_source_ops_t *ops = zone_hub_source_ops(&H, ZSRC_ZIGBEE);
    for (uint64_t i = 1u; i <= ZSRC_ZB_FORGOTTEN_MAX + 2u; i++) {
        zsrc_zb_device_forget(&Z, 0x02ABCD0000000200ull + i, NOW);
    }
    zsrc_address_t first = zsrc_zb_address(0x02ABCD0000000201ull, 1u);
    zsrc_address_t last = zsrc_zb_address(0x02ABCD0000000200ull + ZSRC_ZB_FORGOTTEN_MAX + 2u, 1u);
    TEST_ASSERT_EQUAL_UINT32(0u, ops->grace_of(&Z, &first));
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_GONE_GRACE_S, ops->grace_of(&Z, &last));
}

/* ZB-013 (the review of wave 6): a network reset forgets every device at once. Each of twelve
 * bound zones shows CHECK within seconds - before, only the newest eight did and the other four
 * kept the hour's grace with no sensor in the network. */
static void test_twelve_forgotten_devices_all_check_at_once(void)
{
    const zone_source_ops_t *ops = zone_hub_source_ops(&H, ZSRC_ZIGBEE);
    for (uint16_t i = 0; i < 12u; i++) {
        (void)pair_and_bind((uint16_t)(1u + i), 0x02ABCD0000000400ull + i, ZSRC_ZB_ZONE_TYPE_CONTACT);
        zsrc_zb_ias_status(&Z, 0x02ABCD0000000400ull + i, 1u, 0u, NOW);
    }
    clear_records();
    for (uint16_t i = 0; i < 12u; i++) {
        zsrc_zb_device_forget(&Z, 0x02ABCD0000000400ull + i, NOW);
    }
    advance(2000u);
    for (uint16_t i = 0; i < 12u; i++) {
        const zsrc_address_t a = zsrc_zb_address(0x02ABCD0000000400ull + i, 1u);
        TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_GONE_GRACE_S, ops->grace_of(&Z, &a));
        EXPECT_POSTED((uint16_t)(1u + i), ALARM_ZEV_UNREACHABLE, 1);
    }
}

/* ZB-012 (devices.md §5): while a device's setup runs, the source leaves its CIE write and
 * enrollment to the setup - even when the setup starts before the device is described - but
 * still takes its reports and gives it a zone id. A setup that enrolled it says so; one that
 * stopped hands the device back to the source's own patient enrollment. */
static void test_the_setup_hand_off(void)
{
    zsrc_zb_device_setup(&Z, IEEE_F, true, NOW);
    const zsrc_zb_desc_t d = ias_desc(IEEE_F, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_F, 1u);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.cie_writes);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_NONE, dev->enroll);
    TEST_ASSERT_TRUE(dev->zone_id != 0u); /* what the setup's Enroll Response carries */

    /* Its Enroll Request is the setup's to answer. */
    zsrc_zb_ias_enroll_request(&Z, IEEE_F, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_frame_seen(&Z, IEEE_F, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.enroll_responses);

    /* Its reports still count. */
    zone_hub_binding_t b = zb_binding(IEEE_F, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);

    /* The setup enrolled it: the source never chases it. */
    zsrc_zb_ias_enrolled(&Z, IEEE_F, 1u, NOW);
    zsrc_zb_device_setup(&Z, IEEE_F, false, NOW);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, dev->enroll);
    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_frame_seen(&Z, IEEE_F, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.cie_writes);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.enroll_responses);

    /* A second device whose setup stopped: the source writes its CIE address itself. */
    zsrc_zb_device_setup(&Z, IEEE_G, true, NOW);
    const zsrc_zb_desc_t g = ias_desc(IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_MOTION, "Frient A/S", "MOSZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &g, NOW));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.cie_writes);
    zsrc_zb_device_setup(&Z, IEEE_G, false, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.cie_writes);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_CIE, zsrc_zb_device(&Z, IEEE_G, 1u)->enroll);

    /* ... and one that asked to enroll during its stopped setup is answered after it. */
    const zsrc_zb_desc_t h = ias_desc(IEEE_C, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    zsrc_zb_device_setup(&Z, IEEE_C, true, NOW);
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &h, NOW));
    zsrc_zb_ias_enroll_request(&Z, IEEE_C, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.enroll_responses);
    zsrc_zb_device_setup(&Z, IEEE_C, false, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.enroll_responses);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, zsrc_zb_device(&Z, IEEE_C, 1u)->enroll);

    /* An unknown address, or 0, is harmless; so is enrolled for an endpoint nobody has. */
    zsrc_zb_device_setup(&Z, 0u, true, NOW);
    zsrc_zb_ias_enrolled(&Z, 0x02ABCD00000009FFull, 1u, NOW);
}

/* ZB-012: more setups at once than the source tracks - the extra device is enrolled by the
 * source itself (a CIE write the setup repeats), never left unenrolled. */
static void test_more_setups_than_the_source_tracks(void)
{
    for (uint64_t i = 1u; i <= ZSRC_ZB_SETUP_MAX; i++) {
        zsrc_zb_device_setup(&Z, 0x02ABCD0000000300ull + i, true, NOW);
    }
    zsrc_zb_device_setup(&Z, IEEE_F, true, NOW); /* one too many */
    const zsrc_zb_desc_t d = ias_desc(IEEE_F, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &d, NOW));
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.cie_writes);
}

/* ZB-022 (docs/features/40 WALK-007): the walk test puts each bound, enrolled IAS sensor into
 * test mode - a listening one at once - and its TEST-bit trips are tests, not alarms; it is asked
 * again while the test lasts, and back to normal at the end, when the zone is told where it
 * stands. The request's end comes back as the source's own REP_DONE. */
static void test_walk_test_mode_on_a_listening_device(void)
{
    pair_router_and_bind(Z_FRONT, IEEE_F);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_F, 1u);
    /* Not bound, so not asked. */
    zsrc_zb_desc_t other = ias_desc(IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB");
    other.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &other, NOW));
    zsrc_zb_ias_enroll_request(&Z, IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    clear_records();

    walk_on(IEEE_F);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT64(IEEE_F, RADIO.last_test_ieee);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_TEST_MODE_LEN, RADIO.last_test_len);
    TEST_ASSERT_TRUE((RADIO.last_test_tag & ZSRC_ZB_TAG_SOURCE) != 0u);
    TEST_ASSERT_TRUE(dev->test_mode);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ASKED_ON, dev->test);
    done(RADIO.last_test_tag, ZSRC_ZB_DONE_OK, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ON, dev->test);
    /* Someone else's request is not the source's. */
    done(0x00000042u, ZSRC_ZB_DONE_OK, false);

    /* A trip with the TEST bit: a test, never a fault; its restore is a restore. */
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS); /* entering test mode is not a trip */
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, SRC_TESTS);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1);

    /* A rebinding never replays a test. */
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    clear_records();
    zone_hub_binding_t b = zb_binding(IEEE_F, 1u);
    b.in[ZSRC_ROLE_MAIN].map.grace_s = 7u; /* a changed mapping makes the hub ask again */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_FRONT, &b, NOW));
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);

    /* Asked again while the walk test lasts, no sooner. */
    const unsigned asks = RADIO.test_asks;
    advance(ZSRC_ZB_TEST_REFRESH_MS - 1000u);
    TEST_ASSERT_EQUAL_UINT(asks, RADIO.test_asks);
    advance(1000u);
    TEST_ASSERT_EQUAL_UINT(asks + 1u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_TEST_MODE_LEN, RADIO.last_test_len);

    /* Over: back to normal operation, and the zone told where it stands - still open. */
    clear_records();
    zsrc_zb_walk_test(&Z, false, NOW);
    TEST_ASSERT_EQUAL_UINT(asks + 2u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT8(0u, RADIO.last_test_len);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ASKED_OFF, dev->test);
    TEST_ASSERT_FALSE(dev->test_mode);
    done(RADIO.last_test_tag, ZSRC_ZB_DONE_OK, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_NONE, dev->test);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS);
    advance(ZSRC_ZB_TEST_REFRESH_MS);
    TEST_ASSERT_EQUAL_UINT(asks + 2u, RADIO.test_asks); /* nothing more is owed */

    /* A stale DONE (a slot asked again since) changes nothing. */
    done(ZSRC_ZB_TAG_SOURCE | 0x0001FFFFu, ZSRC_ZB_DONE_TIMEOUT, true);
    done(ZSRC_ZB_TAG_SOURCE | 0x7FFF0001u, ZSRC_ZB_DONE_TIMEOUT, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_NONE, dev->test);
}

/* ZB-022: a sleepy sensor is asked at its next frame, again after a missed answer (no faster than
 * the enrollment gap), never again once it says it has no test mode; and back to normal at its
 * next frame after the walk test. A device that only enrolls during the test is asked too. */
static void test_walk_test_mode_on_a_sleepy_device(void)
{
    pair_and_bind(Z_FRONT, IEEE_F, ZSRC_ZB_ZONE_TYPE_CONTACT);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_F, 1u);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, dev->enroll);

    walk_on(IEEE_F);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.test_asks); /* asleep: its next frame */
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_OWED_ON, dev->test);
    NOW += 1000u;
    zsrc_zb_frame_seen(&Z, IEEE_F, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.test_asks);
    done(RADIO.last_test_tag, ZSRC_ZB_DONE_TIMEOUT, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_OWED_ON, dev->test);
    NOW += 1000u;
    zsrc_zb_frame_seen(&Z, IEEE_F, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.test_asks); /* inside the gap */
    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_report_t ci = report(ZSRC_ZB_REP_CHECK_IN, IEEE_F);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &ci, NOW)); /* a check-in is a wake window too */
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.test_asks);
    done(RADIO.last_test_tag, ZSRC_ZB_DONE_UNSUPPORTED, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_REFUSED, dev->test);
    TEST_ASSERT_FALSE(dev->test_mode);
    NOW += ZSRC_ZB_TEST_REFRESH_MS;
    zsrc_zb_frame_seen(&Z, IEEE_F, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.test_asks); /* it has no test mode: its trips stay faults */
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);

    /* Enrolled only during the walk test: asked at its next frame. */
    const zsrc_zb_desc_t g = ias_desc(IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "X");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &g, NOW));
    zone_hub_binding_t b = zb_binding(IEEE_G, 1u);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    const zsrc_address_t ga = zsrc_zb_address(IEEE_G, 1u);
    zsrc_zb_walk_test_input(&Z, &ga, true, NOW); /* its zone is in the walk test's scope */
    NOW += 1000u;
    zsrc_zb_frame_seen(&Z, IEEE_G, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.test_asks); /* not enrolled yet */
    zsrc_zb_ias_enroll_request(&Z, IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, NOW);
    TEST_ASSERT_EQUAL_UINT(3u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT64(IEEE_G, RADIO.last_test_ieee);

    /* Over, unconfirmed: asked back at its next frame, until it confirms. */
    zsrc_zb_walk_test(&Z, false, NOW);
    const zsrc_zb_device_t *gd = zsrc_zb_device(&Z, IEEE_G, 1u);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_OWED_OFF, gd->test);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_NONE, dev->test); /* the refused one owes nothing */
    NOW += 1000u;
    zsrc_zb_frame_seen(&Z, IEEE_G, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(4u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT8(0u, RADIO.last_test_len);
    done(RADIO.last_test_tag, ZSRC_ZB_DONE_SEND_FAILED, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_OWED_OFF, gd->test);
    TEST_ASSERT_TRUE(gd->test_mode);
    NOW += ZSRC_ZB_ENROLL_GAP_MS;
    zsrc_zb_frame_seen(&Z, IEEE_G, 0u, NOW);
    TEST_ASSERT_EQUAL_UINT(5u, RADIO.test_asks);
    done(RADIO.last_test_tag, ZSRC_ZB_DONE_OK, true);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_NONE, gd->test);
}

/* ZB-022: a radio without the hook can't: the walk test asks nothing and faults stay faults,
 * TEST bit or not (fail-safe: only a device the source put into test mode trips as a test). */
static void test_walk_test_without_the_hook(void)
{
    zsrc_zb_radio_t radio = Z.radio;
    radio.ias_test_mode = NULL;
    Z.radio = radio;
    pair_router_and_bind(Z_FRONT, IEEE_F);
    walk_on(IEEE_F);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_REFUSED, zsrc_zb_device(&Z, IEEE_F, 1u)->test);
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS); /* not asked into test mode: a fault */
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    zsrc_zb_walk_test(&Z, false, NOW);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_NONE, zsrc_zb_device(&Z, IEEE_F, 1u)->test);
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
}

/* ZB-022, fail-safe: outside a walk test the TEST bit never turns a trip into a test. A device
 * that sets it by itself trips as a fault. A trip told as a test during the walk test is told as
 * the fault it still is once the walk test is over, even before the device's TEST bit clears (it
 * has not heard its Normal Operation request yet); after that, only changes are told. */
static void test_test_bit_outside_the_walk_test_is_a_fault(void)
{
    pair_router_and_bind(Z_FRONT, IEEE_F);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST, NOW);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_RESTORE, 1); /* the bit clearing alone tells nothing */

    /* In the walk test, its zone in the scope, asked into test mode: a test. */
    walk_on(IEEE_F);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_F, 1u);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ASKED_ON, dev->test);
    clear_records();
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, SRC_TESTS);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);
    TEST_ASSERT_TRUE(dev->test_masked);

    /* Over, the Normal Operation request unanswered: the zone is told the fault it still is at
     * once (ZB-022), not at the device's next report; the same status again tells nothing. */
    zsrc_zb_walk_test(&Z, false, NOW);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ASKED_OFF, dev->test);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_FALSE(dev->test_masked);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_FALSE(dev->test_masked);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_EQUAL_UINT(1u, SRC_TESTS);
}

/* ZB-022 through the hub and the engine, fail-safe: a Zigbee smoke detector the source asked into
 * test mode trips as a test (no alarm) only while its input is in the walk test's scope. After
 * the walk test, while the sleepy detector has not yet heard its Normal Operation request and
 * still sets its TEST bit, the same trip is a fire alarm. (A fire zone is never in the scope:
 * zbz_walk_covered, test_zbz_replay.c; here the scope is named by hand to reach the window.) */
static void test_a_test_mode_trip_alarms_only_outside_the_walk_test(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_SMOKE, IEEE_F, ZSRC_ZB_ZONE_TYPE_FIRE);
    const zsrc_zb_device_t *dev = zsrc_zb_device(&Z, IEEE_F, 1u);

    walk_on(IEEE_F);
    NOW += 1000u;
    zsrc_zb_frame_seen(&Z, IEEE_F, 0u, NOW); /* asleep until now: asked into test mode */
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ASKED_ON, dev->test);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST, NOW);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, SRC_TESTS);
    EXPECT_POSTED(Z_SMOKE, ALARM_ZEV_FAULT, 0);
    TEST_ASSERT_FALSE((sim_zstatus(&E, Z_SMOKE).flags & ALARM_ZS_ALARM) != 0u);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST, NOW);

    /* Over. The detector sleeps through its Normal Operation request and wakes with a fire. */
    zsrc_zb_walk_test(&Z, false, NOW);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_OWED_OFF, dev->test);
    NOW += 1000u;
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, SRC_TESTS);
    EXPECT_POSTED(Z_SMOKE, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_TRUE((sim_zstatus(&E, Z_SMOKE).flags & ALARM_ZS_ALARM) != 0u);
}

/* ZB-022 (docs/features/40 WALK-007; the review of wave 6): the walk test's scope is per input. A
 * walk test alone asks no sensor into test mode; only an input named in its scope is asked, and
 * another one's TEST-bit trip - another partition's sensor, a fire detector - is a fault, so the
 * engine decides (an armed partition alarms, a fire zone sounds). Before the fix the walk test
 * asked every bound sensor and turned every TEST-bit trip into a test. */
static void test_the_walk_test_asks_only_the_inputs_in_its_scope(void)
{
    pair_router_and_bind(Z_FRONT, IEEE_F);
    pair_router_and_bind(Z_WINDOW, IEEE_G);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    zsrc_zb_ias_status(&Z, IEEE_G, 1u, 0u, NOW);
    clear_records();

    zsrc_zb_walk_test(&Z, true, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.test_asks); /* no scope named yet: nobody */
    advance(ZSRC_ZB_TEST_REFRESH_MS);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.test_asks);

    const zsrc_address_t fa = zsrc_zb_address(IEEE_F, 1u);
    const zsrc_address_t ga = zsrc_zb_address(IEEE_G, 1u);
    zsrc_zb_walk_test_input(&Z, &fa, true, NOW);
    zsrc_zb_walk_test_input(&Z, &ga, false, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT64(IEEE_F, RADIO.last_test_ieee);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_NONE, zsrc_zb_device(&Z, IEEE_G, 1u)->test);

    /* The one out of the scope sets the TEST bit by itself and trips: a fault. */
    zsrc_zb_ias_status(&Z, IEEE_G, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_EQUAL_UINT(0u, SRC_TESTS);
    /* The one in it: a test. */
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_TEST | ZSRC_ZB_IAS_ALARM1, NOW);
    TEST_ASSERT_EQUAL_UINT(1u, SRC_TESTS);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 0);

    /* Out of the scope while still open (its zone moved partition, say): asked back to normal,
     * and its zone told the fault at once. */
    zsrc_zb_walk_test_input(&Z, &fa, false, NOW);
    EXPECT_POSTED(Z_FRONT, ALARM_ZEV_FAULT, 1);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT8(0u, RADIO.last_test_len);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_TEST_ASKED_OFF, zsrc_zb_device(&Z, IEEE_F, 1u)->test);

    /* A scope for an input nobody watches, or not Zigbee, is ignored. */
    const zsrc_address_t none = zsrc_zb_address(IEEE_C, 1u);
    zsrc_zb_walk_test_input(&Z, &none, true, NOW);
    zsrc_zb_walk_test_input(&Z, NULL, true, NOW);
    TEST_ASSERT_EQUAL_UINT(2u, RADIO.test_asks);

    /* Unbound while in the scope: never asked again, asked back at its next chance. */
    zsrc_zb_walk_test_input(&Z, &ga, true, NOW);
    TEST_ASSERT_EQUAL_UINT(3u, RADIO.test_asks);
    TEST_ASSERT_EQUAL_UINT64(IEEE_G, RADIO.last_test_ieee);
    zone_hub_unbind(&H, Z_WINDOW, NOW);
    advance(ZSRC_ZB_TEST_REFRESH_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT64(IEEE_G, RADIO.last_test_ieee);
    TEST_ASSERT_EQUAL_UINT8(0u, RADIO.last_test_len); /* back to normal, not asked in again */
    TEST_ASSERT_FALSE(zsrc_zb_device(&Z, IEEE_G, 1u)->test_mode);

    /* Over: the scope goes with it - a new walk test asks nobody until it names one. */
    zsrc_zb_walk_test(&Z, false, NOW);
    const unsigned asks = RADIO.test_asks;
    zsrc_zb_walk_test(&Z, true, NOW + 10000u);
    TEST_ASSERT_EQUAL_UINT(asks, RADIO.test_asks);
    TEST_ASSERT_FALSE(Z.watch[0].walk || Z.watch[1].walk);
}

/* ZB-005 (devices.md §7, testing.md §8): the supervision window by what the device is - 2 h for a
 * sleepy one, 30 min for a listening one, stretched to two check-ins and ten minutes when a
 * sleepy device's Poll Control interval is longer, never shrunk, never past a day. */
static void test_supervision_windows_per_device(void)
{
    const zsrc_zb_desc_t s = ias_desc(IEEE_F, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "X");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &s, NOW));
    zsrc_zb_desc_t r = ias_desc(IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "Y");
    r.router = true;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &r, NOW));
    const zsrc_zb_device_t *sd = zsrc_zb_device(&Z, IEEE_F, 1u);
    const zsrc_zb_device_t *rd = zsrc_zb_device(&Z, IEEE_G, 1u);
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_SUPERVISION_S, zsrc_zb_supervision_s(&Z, sd));
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_ROUTER_SUPERVISION_S, zsrc_zb_supervision_s(&Z, rd));

    zsrc_zb_check_in_interval(&Z, IEEE_F, 60u, NOW); /* frequent: the 2 h stand */
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_SUPERVISION_S, zsrc_zb_supervision_s(&Z, sd));
    zsrc_zb_check_in_interval(&Z, IEEE_F, 3600u, NOW); /* hourly: two and some slack */
    TEST_ASSERT_EQUAL_UINT32(2u * 3600u + ZSRC_ZB_CHECKIN_SLACK_S, zsrc_zb_supervision_s(&Z, sd));
    zsrc_zb_check_in_interval(&Z, IEEE_F, 0xFFFFFFFFu, NOW); /* nonsense: capped */
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_SUPERVISION_MAX_S, zsrc_zb_supervision_s(&Z, sd));
    zsrc_zb_check_in_interval(&Z, IEEE_G, 3600u, NOW); /* a listening device answers sooner */
    TEST_ASSERT_EQUAL_UINT32(ZSRC_ZB_ROUTER_SUPERVISION_S, zsrc_zb_supervision_s(&Z, rd));

    /* The node descriptor decides what it is (a router listens; a sleepy end device doesn't). */
    zsrc_zb_report_t nd = report(ZSRC_ZB_REP_NODE_DESC, IEEE_G);
    nd.u.node.logical_type = ZSRC_ZB_LOGICAL_END_DEVICE;
    nd.u.node.mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &nd, NOW));
    TEST_ASSERT_FALSE(rd->router);
    TEST_ASSERT_EQUAL_UINT32(2u * 3600u + ZSRC_ZB_CHECKIN_SLACK_S, zsrc_zb_supervision_s(&Z, rd));
    nd.u.node.mac_caps = (uint8_t)(ZSRC_ZB_MAC_ALLOC_ADDR | ZSRC_ZB_MAC_RX_ON_IDLE |
                                   ZSRC_ZB_MAC_MAINS);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &nd, NOW)); /* an end device that listens */
    TEST_ASSERT_TRUE(rd->router);
}

/* ZB-005, testing.md §8 with real numbers: a sleepy contact that checks in hourly never goes
 * unreachable, while one with a known 6-hourly check-in is given 12 h 10 min, not 2 h. */
static void test_check_ins_keep_a_sleepy_device(void)
{
    pair_and_bind(Z_WINDOW, IEEE_F, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_check_in_interval(&Z, IEEE_F, 3600u, NOW);
    for (int h = 0; h < 10; h++) {
        advance(3600u * 1000u);
        zsrc_zb_report_t ci = report(ZSRC_ZB_REP_CHECK_IN, IEEE_F);
        (void)zsrc_zb_on_report(&Z, &ci, NOW);
        TEST_ASSERT_FALSE(zsrc_zb_device(&Z, IEEE_F, 1u)->unreachable);
    }
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_UNREACHABLE, 0);

    const zsrc_zb_desc_t g = ias_desc(IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "Y");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &g, NOW));
    zsrc_zb_check_in_interval(&Z, IEEE_G, 6u * 3600u, NOW);
    advance((uint64_t)ZSRC_ZB_SUPERVISION_S * 1000u + 1000u);
    TEST_ASSERT_FALSE(zsrc_zb_device(&Z, IEEE_G, 1u)->unreachable);
    advance((12u * 3600u + ZSRC_ZB_CHECKIN_SLACK_S - ZSRC_ZB_SUPERVISION_S) * 1000u);
    TEST_ASSERT_TRUE(zsrc_zb_device(&Z, IEEE_G, 1u)->unreachable);
}

/* ZB-011, ZB-012: each report the radio side adds, as the source takes it. */
static void test_the_reports_the_source_takes(void)
{
    const zsrc_address_t a = pair_and_bind(Z_FRONT, IEEE_F, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    const zsrc_zb_device_t *d = zsrc_zb_device(&Z, IEEE_F, 1u);
    const zone_source_ops_t *ops = zone_hub_source_ops(&H, ZSRC_ZIGBEE);

    /* LINK: LQI and RSSI; 0 means not measured and keeps what was. */
    zsrc_zb_report_t r = report(ZSRC_ZB_REP_LINK, IEEE_F);
    r.u.link.lqi = 170u;
    r.u.link.rssi = -61;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT8(170u, d->lqi);
    TEST_ASSERT_EQUAL_INT8(-61, d->rssi);
    r.u.link.lqi = 0u;
    r.u.link.rssi = 0;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT8(170u, d->lqi);
    TEST_ASSERT_EQUAL_INT8(-61, d->rssi);

    /* A frame's link quality is the device speaking: an unreachable one is back. */
    advance((uint64_t)ZSRC_ZB_SUPERVISION_S * 1000u);
    TEST_ASSERT_TRUE(d->unreachable);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_FALSE(d->unreachable);

    /* ANNOUNCE: the new short address; what it said is kept. */
    r = report(ZSRC_ZB_REP_ANNOUNCE, IEEE_F);
    r.nwk = 0x2B02u;
    r.u.announce.mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_HEX16(0x2B02u, d->nwk);
    TEST_ASSERT_TRUE(d->status_known);
    TEST_ASSERT_TRUE(d->node_known);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_LOGICAL_END_DEVICE, d->logical_type);
    TEST_ASSERT_FALSE(d->router);

    /* BASIC: names (the quirks are looked up again) and the power source. */
    r = report(ZSRC_ZB_REP_BASIC, IEEE_F);
    r.u.basic.have = (uint16_t)(ZSRC_ZB_BASIC_MANUFACTURER | ZSRC_ZB_BASIC_MODEL |
                                ZSRC_ZB_BASIC_POWER);
    snprintf(r.u.basic.manufacturer, sizeof(r.u.basic.manufacturer), "%s", "_TZ3000_n2egfsli");
    snprintf(r.u.basic.model, sizeof(r.u.basic.model), "%s", "TS0203");
    r.u.basic.power_source = 3u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_STRING("TS0203", d->model);
    TEST_ASSERT_TRUE((d->quirks & ZSRC_ZB_QUIRK_NO_ENROLL) != 0u);
    TEST_ASSERT_EQUAL_UINT8(3u, d->power_source);
    r.u.basic.have = ZSRC_ZB_BASIC_SW_BUILD; /* nothing the source keeps: names stay */
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_STRING("TS0203", d->model);

    /* POWER_DESC fills a power source Basic didn't give. */
    zsrc_zb_desc_t g = ias_desc(IEEE_G, 1u, ZSRC_ZB_ZONE_TYPE_NONE, "", "");
    g.clusters = 0u; /* nothing known yet: its simple descriptor will say */
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &g, NOW));
    const zsrc_zb_device_t *gd = zsrc_zb_device(&Z, IEEE_G, 1u);
    r = report(ZSRC_ZB_REP_POWER_DESC, IEEE_G);
    r.u.power.current_mode = 0u;
    r.u.power.current_source = 0x01u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT8(1u, gd->power_source);
    TEST_ASSERT_TRUE(gd->router); /* receiver on when idle, no node descriptor yet */

    /* ZB-021: a mains device with no battery can't report one; with no IAS Zone, no tamper. The
     * trust doesn't depend on how it joined. */
    zsrc_zb_desc_t plain;
    memset(&plain, 0, sizeof(plain));
    plain.ieee = IEEE_C;
    plain.endpoint = 1u;
    plain.clusters = ZSRC_ZB_CL_ON_OFF;
    plain.zone_type = ZSRC_ZB_ZONE_TYPE_NONE;
    plain.join = ZSRC_ZB_JOIN_STANDARD_KEY;
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &plain, NOW));
    r = report(ZSRC_ZB_REP_BASIC, IEEE_C);
    r.u.basic.have = ZSRC_ZB_BASIC_POWER;
    r.u.basic.power_source = 1u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    const zsrc_address_t pa = zsrc_zb_address(IEEE_C, 1u);
    const uint32_t pc = ops->caps_of(&Z, &pa);
    TEST_ASSERT_TRUE((pc & ZSRC_CAP_LIFE_SAFETY) != 0u);
    TEST_ASSERT_TRUE((pc & ZSRC_CAP_AUTH) != 0u);
    TEST_ASSERT_TRUE((pc & ZSRC_CAP_SUPERVISED) != 0u);
    TEST_ASSERT_TRUE((pc & ZSRC_CAP_TAMPER) == 0u);
    TEST_ASSERT_TRUE((pc & ZSRC_CAP_BATTERY) == 0u);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, zsrc_zb_device(&Z, IEEE_C, 1u)->join);
    r.u.basic.power_source = 0x81u; /* mains with a backup battery: it can */
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_TRUE((ops->caps_of(&Z, &pa) & ZSRC_CAP_BATTERY) != 0u);
    TEST_ASSERT_EQUAL_HEX32(ops->caps, ops->caps_of(&Z, &a) | 0u); /* an IAS contact: all */
    const zsrc_address_t nobody = zsrc_zb_address(0x02ABCD00000009FFull, 1u);
    TEST_ASSERT_EQUAL_HEX32(ops->caps, ops->caps_of(&Z, &nobody));

    /* SIMPLE_DESC: the endpoint's clusters; IAS Zone found starts its enrollment. An endpoint
     * zbz_panel never described (Green Power's 242) is no slot. */
    r = report(ZSRC_ZB_REP_SIMPLE_DESC, IEEE_G);
    r.u.simple.endpoint = 1u;
    r.u.simple.profile = 0x0104u;
    r.u.simple.device_id = 0x0402u;
    r.u.simple.in_count = 4u;
    r.u.simple.in[0] = ZSRC_ZB_ZCL_BASIC;
    r.u.simple.in[1] = ZSRC_ZB_ZCL_POWER_CFG;
    r.u.simple.in[2] = ZSRC_ZB_ZCL_IAS_ZONE;
    r.u.simple.in[3] = 0xFC57u; /* a vendor cluster: no bit */
    const unsigned cie = RADIO.cie_writes;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_BASIC | ZSRC_ZB_CL_POWER_CFG | ZSRC_ZB_CL_IAS_ZONE,
                            gd->clusters);
    TEST_ASSERT_EQUAL_UINT(cie + 1u, RADIO.cie_writes);
    r.u.simple.endpoint = 242u;
    r.u.simple.profile = 0xA1E0u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, IEEE_G, 242u));
    r.u.simple.endpoint = 1u;
    r.u.simple.in_count = 200u; /* past the list: read as far as it goes */
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));

    /* JOINED with the key it used; a rejoin keeps what it said. */
    r = report(ZSRC_ZB_REP_JOINED, IEEE_F);
    r.u.joined.join = ZSRC_ZB_JOIN_INSTALL_CODE;
    r.u.joined.rejoin = true;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, d->join);
    TEST_ASSERT_TRUE(d->status_known);

    /* ACTIVE_EP, ZCL: it answered - alive; NETWORK: the channel and the panel's address. */
    r = report(ZSRC_ZB_REP_ZCL, IEEE_F);
    NOW += 1000u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT64(NOW, d->heard_at);
    r = report(ZSRC_ZB_REP_ACTIVE_EP, IEEE_F);
    NOW += 1000u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT64(NOW, d->heard_at);
    r = report(ZSRC_ZB_REP_NETWORK, 0u);
    r.u.network.channel = 15u;
    r.u.network.panel_ieee = 0x02ABCD00000000FFull;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT8(15u, Z.channel);
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD00000000FFull, Z.panel_ieee);

    /* Not the source's: refused joins, surveys, nothing, NULL. */
    clear_records();
    r = report(ZSRC_ZB_REP_JOIN_REFUSED, 0x02ABCD0000000777ull);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    r = report(ZSRC_ZB_REP_SURVEY, 0u);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    r = report(ZSRC_ZB_REP_NONE, IEEE_F);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    r = report(0xEEu, IEEE_F); /* a kind from a newer radio side */
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, NULL, NOW));
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, 0x02ABCD0000000777ull, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, NR);

    /* With the radio down, device reports count for nothing (ZSRC-015). */
    zsrc_zb_link(&Z, false, NOW);
    r = report(ZSRC_ZB_REP_LINK, IEEE_F);
    r.u.link.lqi = 40u;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_EQUAL_UINT8(170u, d->lqi);
}

/* ZB-013, ZSRC-015: a device that leaves to rejoin loses its zones until it speaks again, and
 * nothing it said before replays - neither on its return nor to a new binding; one that leaves
 * for good is gone, and one reset and joined afresh is enrolled again from nothing. */
static void test_leave_then_rejoin_replays_nothing(void)
{
    sim_start(&E, NULL, NULL);
    TO_ENGINE = true;
    pair_and_bind(Z_WINDOW, IEEE_F, ZSRC_ZB_ZONE_TYPE_CONTACT);
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    zsrc_zb_battery(&Z, IEEE_F, 50u, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 1);
    const zsrc_zb_device_t *d = zsrc_zb_device(&Z, IEEE_F, 1u);

    zsrc_zb_report_t r = report(ZSRC_ZB_REP_LEFT, IEEE_F);
    r.u.left.rejoin = true;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_NOT_NULL(zsrc_zb_device(&Z, IEEE_F, 1u)); /* it will be back */
    TEST_ASSERT_TRUE(d->unreachable);
    TEST_ASSERT_FALSE(d->status_known);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_BATTERY_UNKNOWN, d->battery_pct);

    /* Back, with a new short address: no stale fault to a new binding. */
    clear_records();
    r = report(ZSRC_ZB_REP_JOINED, IEEE_F);
    r.nwk = 0x3C03u;
    r.u.joined.rejoin = true;
    r.u.joined.join = ZSRC_ZB_JOIN_STANDARD_KEY;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_FALSE(d->unreachable);
    TEST_ASSERT_EQUAL_HEX16(0x3C03u, d->nwk);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_DONE, d->enroll); /* a rejoin keeps its enrollment */
    zone_hub_binding_t b = zb_binding(IEEE_F, 1u);
    b.in[ZSRC_ROLE_MAIN].map.grace_s = 9u;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 0);
    /* What it says now is told as its first word. */
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, 0u, NOW);
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_RESTORE, 1);

    /* Reset and joined afresh: enrolled again from nothing (it forgot the panel's address). */
    zsrc_zb_ias_status(&Z, IEEE_F, 1u, ZSRC_ZB_IAS_ALARM1, NOW);
    const unsigned cie = RADIO.cie_writes;
    r.u.joined.rejoin = false;
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_FALSE(d->status_known);
    TEST_ASSERT_EQUAL_INT(ZSRC_ZB_ENROLL_CIE, d->enroll);
    TEST_ASSERT_EQUAL_UINT(cie + 1u, RADIO.cie_writes);

    /* Left for good: gone, its zone CHECK after the grace (the binding stays). */
    clear_records();
    r = report(ZSRC_ZB_REP_LEFT, IEEE_F);
    TEST_ASSERT_FALSE(zsrc_zb_on_report(&Z, &r, NOW));
    TEST_ASSERT_NULL(zsrc_zb_device(&Z, IEEE_F, 1u));
    advance((uint64_t)ZSRC_ZB_GRACE_S * 1000u);
    EXPECT_ZONE_FLAG(&E, Z_WINDOW, ALARM_ZS_SUPERVISION);

    /* ... and when it is added again, it is a new device with nothing to replay. */
    const zsrc_zb_desc_t again = ias_desc(IEEE_F, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "X");
    TEST_ASSERT_TRUE(zsrc_zb_device_seen(&Z, &again, NOW));
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, Z_WINDOW, &b, NOW));
    EXPECT_POSTED(Z_WINDOW, ALARM_ZEV_FAULT, 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_jamming_requires_fresh_samples_and_scans_continue);
    RUN_TEST(test_first_report_after_reconnect_restores_tamper_and_trouble);
    RUN_TEST(test_reports_are_ignored_while_the_radio_is_down);
    RUN_TEST(test_ias_zone_ids_remain_unique_after_many_rejoins);
    RUN_TEST(test_invalid_cluster_reports_are_ignored);
    RUN_TEST(test_numeric_rebind_replays_temperature_even_with_ias_status);
    RUN_TEST(test_rebinding_replays_low_battery);
    RUN_TEST(test_jam_recovery_invalidates_cached_status);
    RUN_TEST(test_zone_type_becomes_a_sensor_kind);
    RUN_TEST(test_quirks_table);
    RUN_TEST(test_quirks_reach_the_device);
    RUN_TEST(test_pairing_only_in_a_learn_window);
    RUN_TEST(test_new_joins_come_first_in_the_picker);
    RUN_TEST(test_enrollment_of_a_mains_powered_device);
    RUN_TEST(test_a_sleepy_device_is_enrolled_in_its_wake_windows);
    RUN_TEST(test_enrollment_gives_up_but_the_sensor_still_works);
    RUN_TEST(test_a_device_that_never_enrolls);
    RUN_TEST(test_ias_status_bits_become_zone_events);
    RUN_TEST(test_a_burst_is_read_again_and_the_answer_wins);
    RUN_TEST(test_a_plain_change_is_not_read_again);
    RUN_TEST(test_an_unanswered_reread_is_asked_at_the_next_frame);
    RUN_TEST(test_a_momentary_opening_while_armed_counts);
    RUN_TEST(test_alarm_bits_and_the_alarm2_quirk);
    RUN_TEST(test_invert_from_the_binding_and_from_a_quirk);
    RUN_TEST(test_the_no_tamper_quirk);
    RUN_TEST(test_a_zigbee_contact_raises_a_real_alarm);
    RUN_TEST(test_a_zigbee_smoke_detector_on_a_fire_zone);
    RUN_TEST(test_on_off_and_occupancy_sensors);
    RUN_TEST(test_a_thermometer_is_a_freeze_zone);
    RUN_TEST(test_battery_from_a_percentage_and_from_the_status_word);
    RUN_TEST(test_a_silent_sensor_becomes_check);
    RUN_TEST(test_a_router_is_supervised_sooner);
    RUN_TEST(test_a_device_that_leaves_is_check_and_the_binding_stays);
    RUN_TEST(test_check_ins_are_rate_limited);
    RUN_TEST(test_the_radio_going_away_is_one_trouble);
    RUN_TEST(test_routers_are_polled_when_the_radio_comes_back);
    RUN_TEST(test_reconnect_reads_temperature_on_an_ias_and_temperature_router);
    RUN_TEST(test_reconnect_reads_temperature_on_an_occupancy_and_temperature_router);
    RUN_TEST(test_reconnect_reads_temperature_on_an_on_off_and_temperature_router);
    RUN_TEST(test_reconnect_reads_the_binary_input_a_zone_watches);
    RUN_TEST(test_an_on_off_report_never_overrides_ias);
    RUN_TEST(test_an_occupancy_report_never_overrides_ias);
    RUN_TEST(test_replay_and_recovery_pick_the_input_reports_use);
    RUN_TEST(test_jamming_is_the_radios_trouble);
    RUN_TEST(test_the_picker_and_what_may_be_bound);
    RUN_TEST(test_the_words_a_device_is_shown_with);
    RUN_TEST(test_how_a_sensor_is_described);
    RUN_TEST(test_trust_and_life_safety);
    RUN_TEST(test_bindings_survive_a_restart);
    RUN_TEST(test_a_binding_waits_for_its_device_to_reappear);
    RUN_TEST(test_changing_the_mapping_settles_the_zone_once);
    RUN_TEST(test_the_tables_have_limits);
    RUN_TEST(test_a_source_with_no_radio_behind_it);
    RUN_TEST(test_the_setup_steps_are_in_order);
    RUN_TEST(test_forget_drops_the_device_and_nothing_replays);
    RUN_TEST(test_the_forgotten_list_keeps_the_newest);
    RUN_TEST(test_the_setup_hand_off);
    RUN_TEST(test_more_setups_than_the_source_tracks);
    RUN_TEST(test_walk_test_mode_on_a_listening_device);
    RUN_TEST(test_walk_test_mode_on_a_sleepy_device);
    RUN_TEST(test_walk_test_without_the_hook);
    RUN_TEST(test_test_bit_outside_the_walk_test_is_a_fault);
    RUN_TEST(test_a_test_mode_trip_alarms_only_outside_the_walk_test);
    RUN_TEST(test_twelve_forgotten_devices_all_check_at_once);
    RUN_TEST(test_the_walk_test_asks_only_the_inputs_in_its_scope);
    RUN_TEST(test_supervision_windows_per_device);
    RUN_TEST(test_check_ins_keep_a_sleepy_device);
    RUN_TEST(test_the_reports_the_source_takes);
    RUN_TEST(test_leave_then_rejoin_replays_nothing);
    return UNITY_END();
}
