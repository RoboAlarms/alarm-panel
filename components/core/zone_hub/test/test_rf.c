/*
 * test_rf.c - the 433 MHz sensor source (components/core/zone_hub/src/zsrc_rf.c, docs/features/25):
 * learning codes by triggering, what a code does to its zone, closed codes and holds, tamper and
 * low-battery codes, repeats, replay bursts, jamming and a receiver that goes missing - driven
 * through the real hub.
 *
 * No radio: zsrc_rf is handed decoded frames, exactly what components/drivers/rf_rx hands it on the
 * panel, so every rule here is proved on a PC before a CC1101 is wired.
 *
 * Requirements: RF-003..RF-006, RF-009..RF-012, RF-014..RF-016 (docs/features/25), ZSRC-003, ZSRC-004, ZSRC-005, ZSRC-006, ZSRC-015
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_rf.h"
#include "rf_decode/rf_decode.h" /* only to prove the stored numbers are the decoder's */

/* ---- harness ------------------------------------------------------------------------------- */

#define DEV_A 0xA2B3Cu /* the contact under test */
#define DEV_B 0x0BEEFu /* a neighbour's remote */
#define OPEN  0x5u
#define CLOSE 0xEu
#define TAMP  0xAu
#define BATT  0x3u
#define CODE(dev, data) (((dev) << 4) | (data))

typedef struct {
    uint16_t zone;
    alarm_zone_event_t ev;
} rec_t;

static zone_hub_t H;
static zsrc_rf_t SRC;
static uint64_t NOW;
static rec_t REC[256];
static unsigned NREC;
static unsigned TROUBLE_ON;
static unsigned TROUBLE_OFF;

static void port_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    TEST_ASSERT_TRUE(NREC < sizeof(REC) / sizeof(REC[0]));
    REC[NREC].zone = zone;
    REC[NREC].ev = ev;
    NREC++;
}

static void port_changed(void *ctx)
{
    (void)ctx;
}

static void port_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT8(ZSRC_RF, kind);
    if (on) {
        TROUBLE_ON++;
    } else {
        TROUBLE_OFF++;
    }
}

/* The source reports into the hub, the way zone_service wires it on the panel. */
static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t v,
                       uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H, a, state, v, now);
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
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

static unsigned count(uint16_t zone, alarm_zone_event_t ev)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NREC; i++) {
        n += REC[i].zone == zone && REC[i].ev == ev;
    }
    return n;
}

static void clear_records(void)
{
    NREC = 0;
    TROUBLE_ON = 0;
    TROUBLE_OFF = 0;
}

#define EXPECT_POSTED(z_, e_, n) TEST_ASSERT_EQUAL_UINT_MESSAGE((n), count((z_), (e_)), #e_)

static void advance(uint64_t ms)
{
    const uint64_t until = NOW + ms;
    while (NOW < until) {
        const uint64_t step = until - NOW < 1000u ? until - NOW : 1000u;
        NOW += step;
        zone_hub_tick(&H, NOW);
    }
}

/* One transmission: a transmitter repeats its frame while the button is held. */
static void send_frames(uint32_t code, unsigned frames)
{
    const zsrc_rf_packet_t p = zsrc_rf_ev1527(code);
    for (unsigned i = 0; i < frames; i++) {
        zsrc_rf_packet(&SRC, &p, NOW);
        NOW += 40u; /* a frame is 26 pulse pairs: about 40 ms at 350 us */
        zone_hub_tick(&H, NOW);
    }
}

static void send(uint32_t code)
{
    send_frames(code, 4u);
    advance(1000u); /* a finger comes off the button; the next press is a new transmission */
}

static void bind_role(uint16_t zone, uint8_t role, uint32_t device, uint8_t data, uint16_t hold_s)
{
    zone_hub_binding_t b;
    if (!zone_hub_get(&H, zone, &b)) {
        memset(&b, 0, sizeof(b));
    }
    b.in[role].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, device, data);
    zsrc_mapping_defaults(&b.in[role].map, ALARM_DEVCLASS_DOOR);
    b.in[role].map.hold_s = hold_s;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
}

static void bind_main(uint16_t zone, uint32_t device, uint8_t data, uint16_t hold_s)
{
    bind_role(zone, ZSRC_ROLE_MAIN, device, data, hold_s);
    clear_records();
}

void setUp(void)
{
    NOW = 1000u;
    clear_records();
    const zone_hub_port_t port = {.ctx = NULL,
                                  .zone_event = port_event,
                                  .changed = port_changed,
                                  .source_trouble = port_trouble};
    zone_hub_init(&H, &port);
    const zsrc_sink_t sink = {.ctx = NULL,
                              .input = sink_input,
                              .event = sink_event,
                              .reachable = sink_reachable,
                              .link = sink_link};
    zsrc_rf_init(&SRC, &sink);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_rf_ops(), &SRC, NOW));
    zone_hub_start(&H, NOW);
    zsrc_rf_receiver(&SRC, true, NOW);
}

void tearDown(void) {}

/* ---- the source in the picker (ZSRC-005) ----------------------------------------------------- */

static void test_usable_only_with_a_receiver(void)
{
    zone_hub_source_info_t info[ZONE_HUB_MAX_SOURCES];
    TEST_ASSERT_EQUAL_UINT(1u, zone_hub_sources(&H, info, ZONE_HUB_MAX_SOURCES));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_RF, info[0].kind);
    TEST_ASSERT_TRUE(info[0].usable);
    TEST_ASSERT_EQUAL_UINT32(0u, info[0].caps & ZSRC_CAP_SUPERVISED); /* RF-004 */
    TEST_ASSERT_EQUAL_UINT32(0u, info[0].caps & ZSRC_CAP_AUTH);       /* RF-006 */
    TEST_ASSERT_FALSE(zsrc_trusted_for(info[0].caps, ALARM_ZT_FIRE)); /* ZSRC-008 */
    zsrc_rf_receiver(&SRC, false, NOW);
    zone_hub_sources(&H, info, ZONE_HUB_MAX_SOURCES);
    TEST_ASSERT_FALSE(info[0].usable);
}

/* ---- learning (RF-003, RF-009..011) ------------------------------------------------------- */

static size_t candidates(zsrc_candidate_t *c)
{
    return zone_hub_candidates(&H, ZSRC_RF, "", 0u, c, ZSRC_RF_CAND_MAX);
}

static void send_one(uint32_t code)
{
    const zsrc_rf_packet_t p = zsrc_rf_ev1527(code);
    zsrc_rf_packet(&SRC, &p, NOW);
    NOW += 40u;
    zone_hub_tick(&H, NOW);
}

/* RF-003, RF-010: one row per sensor with the codes it sent; the code it sent first while
 * learning is the one bound (the installer opened the door first). */
static void test_learning_lists_sensors_not_codes(void)
{
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    send(CODE(DEV_A, OPEN)); /* before learning */
    TEST_ASSERT_EQUAL_UINT(0u, candidates(c)); /* nothing is offered until learning begins */

    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, NOW));
    TEST_ASSERT_EQUAL_UINT(1u, candidates(c)); /* heard just before: nearby, not the one tested */
    TEST_ASSERT_EQUAL_STRING("Sensor A2B3C", c[0].name);
    TEST_ASSERT_EQUAL_STRING("Code 5", c[0].detail); /* the code it sent last */
    TEST_ASSERT_FALSE(c[0].changed);
    TEST_ASSERT_FALSE(c[0].best);

    send(CODE(DEV_A, OPEN)); /* four frames */
    TEST_ASSERT_EQUAL_UINT(1u, candidates(c));
    TEST_ASSERT_EQUAL_STRING("Sensor A2B3C", c[0].name);
    TEST_ASSERT_EQUAL_STRING("Code 5, heard once", c[0].detail);
    TEST_ASSERT_TRUE(c[0].changed);
    TEST_ASSERT_FALSE(c[0].best); /* once is not sure yet */
    TEST_ASSERT_EQUAL_UINT16(0u, c[0].zone);
    TEST_ASSERT_EQUAL_UINT32(2u, SRC.transmissions);
    TEST_ASSERT_EQUAL_UINT32(2u, SRC.confirmed);
    TEST_ASSERT_EQUAL_UINT32(8u, SRC.frames);

    send(CODE(DEV_A, CLOSE)); /* the door shut again: the same sensor, another code */
    TEST_ASSERT_EQUAL_UINT(1u, candidates(c));
    TEST_ASSERT_EQUAL_STRING("Codes 5 and E, heard 2 times", c[0].detail);
    TEST_ASSERT_TRUE(c[0].best);
    zsrc_address_t want = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, OPEN);
    TEST_ASSERT_TRUE(zsrc_address_equal(&want, &c[0].addr)); /* its first code: the opening */

    zone_hub_learn(&H, ZSRC_RF, false, NOW);
    send(CODE(DEV_B, 1u));
    TEST_ASSERT_EQUAL_UINT(0u, candidates(c)); /* the window is shut */
}

/* RF-009: a transmission counts once its frame arrives twice. A lone frame - noise, or a copy
 * with a bit flipped, which EV1527 cannot tell from a real code - is never a sensor, and a
 * corrupted frame between two repeats does not split the real transmission in two. */
static void test_a_lone_frame_is_not_a_sensor(void)
{
    zone_hub_learn(&H, ZSRC_RF, true, NOW);
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    send_one(CODE(DEV_B, 1u));
    advance(1000u);
    TEST_ASSERT_EQUAL_UINT(0u, candidates(c));
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.transmissions);
    TEST_ASSERT_EQUAL_UINT32(0u, SRC.confirmed);

    const zsrc_rf_packet_t a = zsrc_rf_ev1527(CODE(DEV_A, OPEN));
    const zsrc_rf_packet_t bent = zsrc_rf_ev1527(CODE(DEV_A ^ 0x00400u, OPEN)); /* one bit flipped */
    TEST_ASSERT_FALSE(zsrc_rf_packet(&SRC, &a, NOW)); /* the first frame: not sure yet */
    NOW += 40u;
    TEST_ASSERT_FALSE(zsrc_rf_packet(&SRC, &bent, NOW)); /* a corrupted repeat */
    NOW += 40u;
    TEST_ASSERT_TRUE(zsrc_rf_packet(&SRC, &a, NOW)); /* the frame again: confirmed, once */
    NOW += 40u;
    TEST_ASSERT_FALSE(zsrc_rf_packet(&SRC, &a, NOW));
    advance(1000u);

    TEST_ASSERT_EQUAL_UINT(1u, candidates(c)); /* the real sensor only */
    TEST_ASSERT_EQUAL_STRING("Sensor A2B3C", c[0].name);
    TEST_ASSERT_EQUAL_STRING("Code 5, heard once", c[0].detail); /* not split in two */
    TEST_ASSERT_EQUAL_UINT32(3u, SRC.transmissions); /* the lone frame, A, the bent copy */
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
}

/* RF-011: the sensor that answered the installer more often than any other is best; a tie is
 * nobody's, and the list stays for the installer to choose. */
static void test_the_best_sensor_is_the_one_that_answered(void)
{
    zone_hub_learn(&H, ZSRC_RF, true, NOW);
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    send(CODE(DEV_A, OPEN));
    send(CODE(DEV_B, 1u)); /* a neighbour's remote, pressed at the same time */
    send(CODE(DEV_A, CLOSE));
    send(CODE(DEV_B, 1u));
    TEST_ASSERT_EQUAL_UINT(2u, candidates(c));
    TEST_ASSERT_FALSE(c[0].best);
    TEST_ASSERT_FALSE(c[1].best);
    TEST_ASSERT_EQUAL_STRING("Sensor 0BEEF", c[0].name); /* newest first */

    send(CODE(DEV_A, OPEN)); /* the installer opens the door once more */
    TEST_ASSERT_EQUAL_UINT(2u, candidates(c));
    TEST_ASSERT_EQUAL_STRING("Sensor A2B3C", c[0].name);
    TEST_ASSERT_TRUE(c[0].best);
    TEST_ASSERT_FALSE(c[1].best);
    TEST_ASSERT_EQUAL_STRING("Codes 5 and E, heard 3 times", c[0].detail);
}

static void test_the_learn_table_makes_room_for_the_newest(void)
{
    zone_hub_learn(&H, ZSRC_RF, true, NOW);
    for (unsigned i = 0; i < ZSRC_RF_CAND_MAX + 2u; i++) {
        send(CODE(0x1000u + i, 1u));
    }
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    TEST_ASSERT_EQUAL_UINT(ZSRC_RF_CAND_MAX, candidates(c));
    TEST_ASSERT_EQUAL_STRING("Sensor 01011", c[0].name); /* the last one sent */
    /* the two heard longest ago are gone */
    for (size_t i = 0; i < ZSRC_RF_CAND_MAX; i++) {
        TEST_ASSERT_TRUE(strcmp(c[i].name, "Sensor 01000") != 0);
        TEST_ASSERT_TRUE(strcmp(c[i].name, "Sensor 01001") != 0);
    }
}

/* ---- codes and zones (ZSRC-006) --------------------------------------------------------------- */

static void test_a_bound_zone_starts_quiet_and_known(void)
{
    bind_role(1u, ZSRC_ROLE_MAIN, DEV_A, OPEN, 0u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u); /* nothing heard yet, and that is a state */
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 1u, &info));
    TEST_ASSERT_TRUE(info.known);
    TEST_ASSERT_FALSE(info.active);
}

static void test_the_open_code_opens_and_another_code_closes(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send(CODE(DEV_A, OPEN));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    send(CODE(DEV_A, OPEN)); /* the same code again: still open, nothing new to say */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    send(CODE(DEV_A, CLOSE));
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

static void test_a_lonely_code_restores_after_the_automatic_hold(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    advance(ZSRC_RF_AUTO_HOLD_S * 1000u - 2000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    advance(3000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

static void test_an_explicit_hold_only_ever_times_out(void)
{
    bind_main(1u, DEV_A, OPEN, 10u); /* a PIR: motion ends 10 s after its last frame */
    send_frames(CODE(DEV_A, OPEN), 4u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    send_frames(CODE(DEV_A, BATT), 4u); /* its low-battery chirp must not end the motion */
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    advance(8000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    advance(3000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

/* A frame stamped a moment after the tick's clock was read (the receiver's task and the zone
 * task read the clock separately) must not look four billion years old and end the fault. */
static void test_a_frame_from_the_near_future_keeps_its_zone_open(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    const zsrc_rf_packet_t p = zsrc_rf_ev1527(CODE(DEV_A, OPEN));
    zsrc_rf_packet(&SRC, &p, NOW + 80u);
    zsrc_rf_packet(&SRC, &p, NOW + 120u); /* confirmed (RF-014) */
    zone_hub_tick(&H, NOW);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
}

static void test_repeats_extend_the_hold(void)
{
    bind_main(1u, DEV_A, OPEN, 10u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    advance(8000u);
    send_frames(CODE(DEV_A, OPEN), 4u); /* still moving */
    advance(8000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    advance(3000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_EQUAL_UINT32(2u, SRC.transmissions);
}

static void test_tamper_and_battery_codes_are_their_own_inputs(void)
{
    bind_role(1u, ZSRC_ROLE_MAIN, DEV_A, OPEN, 0u);
    bind_role(1u, ZSRC_ROLE_TAMPER, DEV_A, TAMP, 120u); /* a tamper is over two minutes later */
    bind_role(1u, ZSRC_ROLE_BATTERY, DEV_A, BATT, 0u);  /* left automatic, on purpose */
    clear_records();
    send(CODE(DEV_A, TAMP));
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    send(CODE(DEV_A, OPEN)); /* a bound code restores nothing but its own hold */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    send(CODE(DEV_A, BATT));
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u); /* the door is still open */
    /* The one code nothing claims: the door closes, and so does every other automatic hold
     * (the battery's - which is why a battery code is best bound with a long hold of its
     * own); the tamper, with its explicit hold, keeps counting. */
    send(CODE(DEV_A, CLOSE));
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    advance(ZSRC_RF_AUTO_HOLD_S * 1000u + 1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER_RESTORE, 0u);
    advance(120000u);
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER_RESTORE, 1u);
}

static void test_a_stranger_is_counted_and_ignored(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send(CODE(DEV_B, OPEN)); /* the same data bits from another address */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.unknown);
}

static void test_a_burst_far_beyond_a_finger_is_noted(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), ZSRC_RF_BURST_FRAMES + 20u); /* RF-006: a replay, or a stuck button */
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.bursts);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.transmissions);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u); /* it still opened the zone once, as it should */
}

/* ---- one code, one zone (ZSRC-003) ------------------------------------------------------------ */

static void test_one_code_feeds_one_zone(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, OPEN);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_IN_USE, zone_hub_bind(&H, 2u, &b, NOW));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, CLOSE);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 2u, &b, NOW)); /* another code: fine */
}

static void test_addresses_round_trip_and_bad_ones_are_refused(void)
{
    const zsrc_address_t a = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, OPEN);
    uint8_t proto;
    uint32_t device;
    uint8_t data;
    TEST_ASSERT_TRUE(zsrc_rf_address_parts(&a, &proto, &device, &data));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_RF_PROTO_EV1527, proto);
    TEST_ASSERT_EQUAL_HEX32(DEV_A, device);
    TEST_ASSERT_EQUAL_UINT8(OPEN, data);
    zsrc_address_t bad = a;
    bad.input = 0u;
    TEST_ASSERT_FALSE(zsrc_rf_address_parts(&bad, NULL, NULL, NULL));
    bad = a;
    bad.id = ((uint64_t)9u << 32) | DEV_A; /* a protocol this firmware has never heard of */
    TEST_ASSERT_FALSE(zsrc_rf_address_parts(&bad, NULL, NULL, NULL));
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = bad;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_REFUSED, zone_hub_bind(&H, 1u, &b, NOW));
    char words[64];
    zsrc_rf_code_words(ZSRC_RF_PROTO_EV1527, DEV_A, OPEN, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("EV1527 A2B3C code 5", words);
}

/* ---- the binding blob (ZSRC-004) --------------------------------------------------------------- */

static void test_the_hold_survives_the_blob(void)
{
    bind_main(1u, DEV_A, OPEN, 12u);
    uint8_t blob[ZONE_HUB_BLOB_MAX];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_encode(&H, blob, sizeof(blob), &len));
    setUp(); /* a restart */
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_decode(&H, blob, len));
    zone_hub_start(&H, NOW); /* the panel loads the blob, then starts watching (zone_service) */
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 1u, &b));
    TEST_ASSERT_EQUAL_UINT16(12u, b.in[ZSRC_ROLE_MAIN].map.hold_s);
    const zsrc_address_t want = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, OPEN);
    TEST_ASSERT_TRUE(zsrc_address_equal(&want, &b.in[ZSRC_ROLE_MAIN].addr));
    clear_records();
    send_frames(CODE(DEV_A, OPEN), 4u); /* the source watches it again after the restart */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    advance(13000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u); /* with its 12 s hold */
}

/* ---- the link: jamming and a missing receiver (RF-005, ZSRC-015) --------------------------- */

static void jam(int8_t dbm, uint64_t ms)
{
    const uint64_t until = NOW + ms;
    while (NOW < until) {
        zsrc_rf_noise(&SRC, dbm, NOW);
        advance(500u);
    }
}

static void test_jamming_is_the_sources_trouble(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    jam(-40, ZSRC_RF_JAM_HOLD_MS - 1000u);
    TEST_ASSERT_FALSE(zsrc_rf_jammed(&SRC));
    jam(-40, 2000u);
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_ON); /* the hub's grace first */
    jam(-40, ZSRC_RF_GRACE_S * 1000u + 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON);
    zone_hub_info_t info;
    TEST_ASSERT_TRUE(zone_hub_info(&H, 1u, &info));
    TEST_ASSERT_TRUE(info.link_down);
    jam(-95, 2000u); /* the carrier is gone: the last minute still holds it */
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));
    jam(-95, ZSRC_RF_JAM_WINDOW_MS);
    TEST_ASSERT_FALSE(zsrc_rf_jammed(&SRC));
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF);
}

static void test_a_short_carrier_is_not_jamming(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    jam(-40, 10000u);
    jam(-95, 1000u);
    jam(-40, 10000u);
    TEST_ASSERT_FALSE(zsrc_rf_jammed(&SRC));
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_ON);
}

/* A receiver module without RSSI reports the carrier itself (its data line held high). */
static void test_a_carrier_without_rssi_is_jamming_too(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    for (uint64_t t = 0; t < ZSRC_RF_JAM_HOLD_MS + 2000u; t += 500u) {
        zsrc_rf_carrier(&SRC, true, NOW);
        advance(500u);
    }
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));
    for (uint64_t t = 0; t < ZSRC_RF_JAM_WINDOW_MS; t += 1000u) {
        zsrc_rf_carrier(&SRC, false, NOW);
        advance(1000u);
    }
    TEST_ASSERT_FALSE(zsrc_rf_jammed(&SRC));
}

static void test_a_missing_receiver_is_the_sources_trouble(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    zsrc_rf_receiver(&SRC, false, NOW);
    advance(ZSRC_RF_GRACE_S * 1000u + 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_ON);
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 1u, &b)); /* the binding is kept (ZSRC-010) */
    zsrc_rf_receiver(&SRC, true, NOW);
    advance(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, TROUBLE_OFF);
    send(CODE(DEV_A, OPEN));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
}

static void test_unbinding_forgets_the_code(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    zone_hub_unbind(&H, 1u, NOW);
    clear_records();
    send(CODE(DEV_A, OPEN));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.unknown);
}

/* ---- acting on confirmed frames (RF-014) ------------------------------------------------------ */

static void send_pkt(const zsrc_rf_packet_t *p, unsigned frames)
{
    for (unsigned i = 0; i < frames; i++) {
        zsrc_rf_packet(&SRC, p, NOW);
        NOW += 40u;
        zone_hub_tick(&H, NOW);
    }
}

/* One frame is not a transmission: it may be a bit flipped by noise or a collision. The zone
 * moves on the second (a PT2272 decoder waits for two code words as well). */
static void test_a_lone_frame_moves_no_zone(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_one(CODE(DEV_A, OPEN));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    send_one(CODE(DEV_A, OPEN));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    advance(1000u);
    /* the close code, once: a corrupted frame could say anything - nothing restores */
    send_one(CODE(DEV_A, CLOSE));
    advance(1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    send_frames(CODE(DEV_A, CLOSE), 2u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

/* A frame from another sensor between the repeats does not stop the confirmation. */
static void test_two_sensors_interleaved_both_count(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    bind_main(2u, DEV_B, 1u, 0u);
    for (unsigned i = 0; i < 3u; i++) {
        send_one(CODE(DEV_A, OPEN));
        send_one(CODE(DEV_B, 1u));
    }
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 1u);
}

/* A frame whose protocol carries a checksum it passed is a transmission on its own. */
static void test_a_checked_frame_acts_at_once(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    zsrc_rf_packet_t p = zsrc_rf_ev1527(CODE(DEV_A, OPEN));
    p.flags = ZSRC_RF_PKT_CHECKED;
    TEST_ASSERT_TRUE(zsrc_rf_packet(&SRC, &p, NOW));
    zone_hub_tick(&H, NOW);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
}

/* The door is opened and shut within a second: both transmissions count, in order. */
static void test_a_door_opened_and_shut_at_once(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    send_frames(CODE(DEV_A, CLOSE), 4u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    unsigned last = 0;
    for (unsigned i = 0; i < NREC; i++) {
        if (REC[i].zone == 1u && (REC[i].ev == ALARM_ZEV_FAULT || REC[i].ev == ALARM_ZEV_RESTORE)) {
            last = i;
        }
    }
    TEST_ASSERT_EQUAL(ALARM_ZEV_RESTORE, REC[last].ev);
}

/* The zone's last open or closed event: what it shows now. */
static alarm_zone_event_t last_state(uint16_t zone)
{
    alarm_zone_event_t ev = ALARM_ZEV_CHECKIN;
    for (unsigned i = 0; i < NREC; i++) {
        if (REC[i].zone == zone && (REC[i].ev == ALARM_ZEV_FAULT || REC[i].ev == ALARM_ZEV_RESTORE)) {
            ev = REC[i].ev;
        }
    }
    return ev;
}

/* RF-023 (B020): a door opened, shut and opened again inside ZSRC_RF_REPEAT_MS sends open,
 * closed, open: the second open is new, not a repeat of the first, so the zone ends open. */
static void test_open_shut_open_at_once_ends_open(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    send_frames(CODE(DEV_A, CLOSE), 4u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 2u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    TEST_ASSERT_EQUAL(ALARM_ZEV_FAULT, last_state(1u));
    TEST_ASSERT_EQUAL_UINT32(3u, SRC.transmissions);
}

/* RF-023: a magnet bouncing past the reed switch - each code sent only twice, 80 ms apart -
 * leaves the zone where the sensor's last code says, whichever that is; every opening counts. */
static void test_a_bounce_ends_on_the_last_code(void)
{
    static const uint8_t ends[2] = {OPEN, CLOSE};
    for (unsigned e = 0; e < 2u; e++) {
        bind_main(1u, DEV_A, OPEN, 0u);
        for (unsigned k = 0; k < 3u; k++) {
            send_frames(CODE(DEV_A, OPEN), 2u);
            send_frames(CODE(DEV_A, CLOSE), 2u);
        }
        send_frames(CODE(DEV_A, ends[e]), 4u);
        EXPECT_POSTED(1u, ALARM_ZEV_FAULT, ends[e] == OPEN ? 4u : 3u);
        TEST_ASSERT_EQUAL(ends[e] == OPEN ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE, last_state(1u));
        advance(ZSRC_RF_AUTO_HOLD_S * 1000u + 1000u); /* an open zone restores after its hold */
        zone_hub_unbind(&H, 1u, NOW);
    }
}

/* RF-009 with RF-023: a closed code heard once - a copy with a bit flipped, never confirmed -
 * between the open code's repeats ends nothing: the open stays one transmission. */
static void test_a_lone_other_code_does_not_split_a_transmission(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 3u);
    send_frames(CODE(DEV_A, CLOSE), 1u);
    send_frames(CODE(DEV_A, OPEN), 3u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
}

/* A transmitter stuck on (a jammed button, a failing sensor): one long transmission, counted as
 * a burst; its zone stays active while it lasts and restores after its hold once it stops. */
static void test_a_stuck_transmitter(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 3000u); /* two minutes */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.bursts);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.transmissions);
    advance(ZSRC_RF_AUTO_HOLD_S * 1000u + 1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

/* ---- supervision (RF-015) ------------------------------------------------------------------- */

/* Every confirmed transmission of a bound sensor, whatever code, is a check-in for its zone, at
 * most one a minute: a sensor that reports now and then can be supervised. */
static void test_every_transmission_is_a_check_in(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    send(CODE(DEV_A, 0x9u)); /* an "alive" code nothing is bound to */
    EXPECT_POSTED(1u, ALARM_ZEV_CHECKIN, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    send(CODE(DEV_A, 0x9u));
    EXPECT_POSTED(1u, ALARM_ZEV_CHECKIN, 1u); /* not twice a minute */
    advance(ZSRC_RF_CHECKIN_MS);
    send(CODE(DEV_A, 0x9u));
    EXPECT_POSTED(1u, ALARM_ZEV_CHECKIN, 2u);
    send_one(CODE(DEV_B, 0x9u)); /* a stranger checks nobody in */
    advance(ZSRC_RF_CHECKIN_MS);
    send_one(CODE(DEV_A, 0x9u)); /* nor does one lone frame */
    advance(1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_CHECKIN, 2u);
}

/* ---- rolling codes (RF-016) ------------------------------------------------------------------- */

static zsrc_rf_packet_t keeloq(uint32_t serial, uint8_t button, uint32_t hop, bool low)
{
    zsrc_rf_packet_t p;
    memset(&p, 0, sizeof(p));
    p.proto = ZSRC_RF_PROTO_KEELOQ;
    p.bits = 66u;
    p.unit_us = 400u;
    p.device = serial;
    p.data = button;
    p.code = serial;
    p.extra = hop;
    p.flags = (uint8_t)(ZSRC_RF_PKT_ROLLING | (low ? ZSRC_RF_PKT_BATTERY_LOW : 0u));
    return p;
}

static void bind_fob(uint16_t zone, uint32_t serial, uint8_t button, uint16_t hold_s)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_KEELOQ, serial, button);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_GENERIC);
    b.in[ZSRC_ROLE_MAIN].map.hold_s = hold_s;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
    clear_records();
}

/* A key fob's code changes with every press: the same one again is somebody replaying a
 * recording, and is refused - the repeats within one press are the press. */
static void test_a_replayed_rolling_code_is_refused(void)
{
    const uint32_t fob = 0x0ABCDEFu;
    bind_fob(1u, fob, 0x2u, 5u);
    zsrc_rf_packet_t press = keeloq(fob, 0x2u, 0x11111111u, false);
    send_pkt(&press, 6u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    advance(6000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    send_pkt(&press, 6u); /* the recording, played back */
    advance(1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.replays);
    press.extra = 0x22222222u; /* the next real press */
    send_pkt(&press, 6u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 2u);
}

/* The fob's battery bit reaches its zone as low battery, said once, and its restore. */
static void test_the_battery_bit_reaches_the_zone(void)
{
    const uint32_t fob = 0x0123456u;
    bind_fob(1u, fob, 0x1u, 0u);
    zsrc_rf_packet_t p = keeloq(fob, 0x1u, 1u, true);
    send_pkt(&p, 3u);
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    advance(2000u);
    p = keeloq(fob, 0x1u, 2u, true);
    send_pkt(&p, 3u);
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    advance(2000u);
    p = keeloq(fob, 0x1u, 3u, false); /* a new battery */
    send_pkt(&p, 3u);
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 1u);
}

/* ---- every protocol (RF-012) ----------------------------------------------------------------- */

static void test_every_protocol_binds_by_its_own_widths(void)
{
    static const struct {
        uint8_t proto;
        uint32_t device_max;
        uint8_t codes;
    } P[] = {{ZSRC_RF_PROTO_EV1527, 0xFFFFFu, 16u},   {ZSRC_RF_PROTO_HT6P20B, 0x3FFFFFu, 4u},
             {ZSRC_RF_PROTO_HT12E, 0xFFu, 16u},       {ZSRC_RF_PROTO_KEELOQ, 0xFFFFFFFu, 16u},
             {ZSRC_RF_PROTO_ARCTECH, 0x3FFFFFFFu, 2u}};
    for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); i++) {
        const zsrc_address_t a = zsrc_rf_address(P[i].proto, P[i].device_max, (uint8_t)(P[i].codes - 1u));
        uint8_t proto, data;
        uint32_t device;
        TEST_ASSERT_TRUE(zsrc_rf_address_parts(&a, &proto, &device, &data));
        TEST_ASSERT_EQUAL_HEX32(P[i].device_max, device);
        TEST_ASSERT_EQUAL_UINT8(P[i].codes - 1u, data);
        zsrc_address_t bad = a;
        bad.input = (uint8_t)(P[i].codes + 1u); /* a code the protocol cannot send */
        TEST_ASSERT_FALSE(zsrc_rf_address_parts(&bad, NULL, NULL, NULL));
        bad = a;
        bad.id = ((uint64_t)P[i].proto << 32) | ((uint64_t)P[i].device_max + 1u);
        TEST_ASSERT_FALSE(zsrc_rf_address_parts(&bad, NULL, NULL, NULL));
        zone_hub_binding_t b;
        memset(&b, 0, sizeof(b));
        b.in[ZSRC_ROLE_MAIN].addr = a;
        zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_DOOR);
        const uint16_t zone = (uint16_t)(10u + i);
        TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, NOW));
        clear_records();
        zsrc_rf_packet_t p;
        memset(&p, 0, sizeof(p));
        p.proto = P[i].proto;
        p.device = P[i].device_max;
        p.data = (uint8_t)(P[i].codes - 1u);
        p.extra = 0x1000u + (uint32_t)i;
        p.flags = P[i].proto == ZSRC_RF_PROTO_KEELOQ ? ZSRC_RF_PKT_ROLLING : 0u;
        send_pkt(&p, 3u);
        EXPECT_POSTED(zone, ALARM_ZEV_FAULT, 1u);
        advance(1000u);
    }
    char words[48];
    zsrc_rf_code_words(ZSRC_RF_PROTO_HT6P20B, 0x2A3B4Cu, 1u, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("HT6P20B 2A3B4C code 1", words);
    zsrc_rf_code_words(ZSRC_RF_PROTO_KEELOQ, 0xABCDEFu, 2u, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("KeeLoq 0ABCDEF code 2", words);
}

/* A frame of a protocol this firmware does not know, or outside its widths, is refused. */
static void test_unknown_frames_are_refused(void)
{
    zsrc_rf_packet_t p = zsrc_rf_ev1527(CODE(DEV_A, OPEN));
    p.proto = 9u;
    TEST_ASSERT_FALSE(zsrc_rf_packet(&SRC, &p, NOW));
    p = zsrc_rf_ev1527(CODE(DEV_A, OPEN));
    p.device = 0x100000u;
    TEST_ASSERT_FALSE(zsrc_rf_packet(&SRC, &p, NOW));
    TEST_ASSERT_EQUAL_UINT32(2u, SRC.refused);
    TEST_ASSERT_EQUAL_UINT32(0u, SRC.frames);
}

/* The numbers are rf_decode's: the one stored in a binding is the one the decoder hands back. */
static void test_the_numbers_are_the_decoders(void)
{
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_COUNT, ZSRC_RF_PROTO_COUNT);
    for (uint8_t p = 1u; p < RF_PROTO_COUNT; p++) {
        const rf_proto_info_t *info = rf_proto_info(p);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_UINT8(info->device_bits, zsrc_rf_device_bits(p));
        TEST_ASSERT_EQUAL_UINT8(1u << info->data_bits, zsrc_rf_codes(p));
        TEST_ASSERT_EQUAL_STRING(info->name, zsrc_rf_proto_name(p));
    }
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_EV1527, ZSRC_RF_PROTO_EV1527);
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_HT6P20B, ZSRC_RF_PROTO_HT6P20B);
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_HT12E, ZSRC_RF_PROTO_HT12E);
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_KEELOQ, ZSRC_RF_PROTO_KEELOQ);
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_ARCTECH, ZSRC_RF_PROTO_ARCTECH);
    TEST_ASSERT_EQUAL_UINT(RF_PKT_CHECKED, ZSRC_RF_PKT_CHECKED);
    TEST_ASSERT_EQUAL_UINT(RF_PKT_BATTERY_LOW, ZSRC_RF_PKT_BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(RF_PKT_ROLLING, ZSRC_RF_PKT_ROLLING);
}

/* ---- review findings RF01, RF02, RF31 ------------------------------------------------------- */

/* RF-014, RF01 (zsrc_rf.h): another code restores an automatic input unless a binding of the
 * SAME zone claims it. Two zones on two codes of one transmitter are allowed
 * (test_one_code_feeds_one_zone): zone 2's code must not keep zone 1 open until its hold. */
static void test_another_zones_code_restores_an_automatic_input(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    bind_role(2u, ZSRC_ROLE_MAIN, DEV_A, CLOSE, 0u);
    clear_records();
    send(CODE(DEV_A, OPEN));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    send(CODE(DEV_A, CLOSE)); /* zone 2's code: zone 2 opens, zone 1 closes at once */
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    send(CODE(DEV_A, OPEN)); /* and back */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 2u);
    EXPECT_POSTED(2u, ALARM_ZEV_RESTORE, 1u);

    /* Rapid open, closed, open: each code ends the other zone's input; zone 1 ends open. */
    send_frames(CODE(DEV_A, CLOSE), 4u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    send_frames(CODE(DEV_A, CLOSE), 4u);
    send_frames(CODE(DEV_A, OPEN), 4u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 4u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 3u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 3u);
    EXPECT_POSTED(2u, ALARM_ZEV_RESTORE, 3u);

    /* A zone with an explicit hold still ignores every other code, its neighbours' too. */
    bind_role(3u, ZSRC_ROLE_MAIN, DEV_A, BATT, 60u);
    clear_records();
    send(CODE(DEV_A, BATT));
    EXPECT_POSTED(3u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u); /* zone 3's code is not zone 1's */
    send(CODE(DEV_A, OPEN));
    EXPECT_POSTED(3u, ALARM_ZEV_RESTORE, 0u);
    advance(60000u);
    EXPECT_POSTED(3u, ALARM_ZEV_RESTORE, 1u);
}

/* RF-021, RF01: the same zone's tamper and low-battery codes still never close its main input,
 * while another zone's code on the same transmitter does. */
static void test_a_zones_own_codes_hold_while_anothers_restore(void)
{
    bind_role(1u, ZSRC_ROLE_MAIN, DEV_A, OPEN, 0u);
    bind_role(1u, ZSRC_ROLE_TAMPER, DEV_A, TAMP, 120u);
    bind_role(1u, ZSRC_ROLE_BATTERY, DEV_A, BATT, 600u);
    bind_role(2u, ZSRC_ROLE_MAIN, DEV_A, CLOSE, 0u);
    clear_records();
    send(CODE(DEV_A, OPEN));
    send(CODE(DEV_A, TAMP));
    send(CODE(DEV_A, BATT));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u); /* its own codes: still open */
    send(CODE(DEV_A, CLOSE));                 /* zone 2's: closes it */
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER_RESTORE, 0u); /* explicit holds keep counting */
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY_RESTORE, 0u);
}

/* RF-009, RF-014, RF02: a code bound while its confirmed transmission is still arriving gets
 * that transmission at its next frame - once, without counting or learning it again. */
/* RF02: a transmission gone on past ZSRC_RF_BURST_FRAMES is no finger on a button (a recording
 * sent over and over, say): an input bound during it gets nothing from it. */
static void test_a_code_bound_during_an_endless_burst_gets_nothing(void)
{
    send_frames(CODE(DEV_A, OPEN), ZSRC_RF_BURST_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 5u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
}

static void test_a_code_bound_mid_burst_gets_the_rest_of_it(void)
{
    send_frames(CODE(DEV_A, OPEN), 4u); /* confirmed; nobody bound yet (learning, say) */
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
    bind_main(1u, DEV_A, OPEN, 0u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u); /* it starts quiet: nothing heard since */
    send_frames(CODE(DEV_A, OPEN), 1u);     /* the same burst goes on */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    send_frames(CODE(DEV_A, OPEN), 10u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed); /* one transmission, counted once */
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.transmissions);
    /* unbound and bound again while it still goes on: the new input gets it too */
    zone_hub_unbind(&H, 1u, NOW);
    bind_main(1u, DEV_A, OPEN, 0u);
    send_frames(CODE(DEV_A, OPEN), 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
    /* the hold counts from the burst's last frame, as for an input bound before it */
    advance(ZSRC_RF_AUTO_HOLD_S * 1000u - 1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    advance(1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);

    /* Bound after one frame only: nothing until the frame comes again (RF-009). */
    send_frames(CODE(DEV_B, OPEN), 1u);
    bind_main(2u, DEV_B, OPEN, 0u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 0u);
    send_frames(CODE(DEV_B, OPEN), 1u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(2u, SRC.confirmed);

    /* A burst that is over is over: bound after it, its code waits for a new transmission. */
    advance(5000u);
    send_frames(CODE(DEV_A, CLOSE), 4u);
    advance(ZSRC_RF_REPEAT_MS + 100u);
    bind_main(3u, DEV_A, CLOSE, 0u);
    send_frames(CODE(DEV_A, CLOSE), 1u); /* a new transmission's first frame */
    EXPECT_POSTED(3u, ALARM_ZEV_FAULT, 0u);
    send_frames(CODE(DEV_A, CLOSE), 1u);
    EXPECT_POSTED(3u, ALARM_ZEV_FAULT, 1u);

    /* Another transmitter's burst moves nothing of a code bound in the middle of it. */
    send_frames(CODE(DEV_B, CLOSE), 4u);
    bind_main(4u, DEV_A, TAMP, 0u);
    send_frames(CODE(DEV_B, CLOSE), 4u);
    EXPECT_POSTED(4u, ALARM_ZEV_FAULT, 0u);
}

/* ---- a zone's tamper and low-battery codes (RF-021) ----------------------------------------- */

/* The bench, 2026-09-25: a contact sending code 6 open and 9 closed got its tamper input bound
 * to 9 - the first code the sensor sent while the tamper was learned, the picker of a zone's own
 * sensor binding "the code it sent first". The tamper then showed whenever the door closed.
 * Now that picker lists the zone's sensor's codes one each, in the order heard, with how often
 * and how long ago, for the installer to choose; the zone's own code is marked by the hub (its
 * zone), its closed code by the source (unusable). */
static void test_a_zones_other_inputs_list_its_sensors_codes(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 1u, &b));
    char q[48];
    zsrc_rf_codes_query(&b.in[ZSRC_ROLE_MAIN].addr, q, sizeof(q));
    TEST_ASSERT_EQUAL_STRING("codes 1 A2B3C", q);

    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    send(CODE(DEV_A, TAMP)); /* before the page opened: not listed */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, NOW));
    TEST_ASSERT_EQUAL_UINT(0u, zone_hub_candidates(&H, ZSRC_RF, q, 0u, c, ZSRC_RF_CAND_MAX));
    send(CODE(DEV_A, OPEN));  /* the installer opens the door ... */
    send(CODE(DEV_A, CLOSE)); /* ... and closes it: E ends the zone, so it is the closed code */
    send(CODE(DEV_A, TAMP));  /* the cover comes off */
    send(CODE(DEV_B, 1u));    /* a neighbour's remote: another sensor, never listed here */
    send(CODE(DEV_A, TAMP));  /* again (the cover goes back on): listed once, heard twice */
    TEST_ASSERT_EQUAL_UINT(3u, zone_hub_candidates(&H, ZSRC_RF, q, 0u, c, ZSRC_RF_CAND_MAX));
    TEST_ASSERT_EQUAL_STRING("Code 5", c[0].name);
    TEST_ASSERT_EQUAL_STRING("Sent first, once", c[0].detail);
    TEST_ASSERT_EQUAL_UINT16(1u, c[0].zone); /* the zone's own code: the picker greys it */
    TEST_ASSERT_FALSE(c[0].unusable);
    TEST_ASSERT_EQUAL_STRING("Code E", c[1].name);
    TEST_ASSERT_EQUAL_STRING("Closes the zone, once", c[1].detail);
    TEST_ASSERT_TRUE(c[1].unusable);
    TEST_ASSERT_EQUAL_STRING("Code A", c[2].name);
    TEST_ASSERT_EQUAL_STRING("Sent third, twice", c[2].detail);
    TEST_ASSERT_FALSE(c[2].unusable);
    TEST_ASSERT_EQUAL_UINT16(0u, c[2].zone);
    const zsrc_address_t tamp = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, TAMP);
    TEST_ASSERT_TRUE(zsrc_address_equal(&tamp, &c[2].addr));
    TEST_ASSERT_EQUAL_STRING("just now", c[2].state);

    /* the tamper input takes the code the installer chose; the door still closes on E */
    bind_role(1u, ZSRC_ROLE_TAMPER, DEV_A, TAMP, 0u);
    zone_hub_learn(&H, ZSRC_RF, false, NOW);
    TEST_ASSERT_EQUAL_UINT(0u, zone_hub_candidates(&H, ZSRC_RF, q, 0u, c, ZSRC_RF_CAND_MAX));
    advance(ZSRC_RF_AUTO_HOLD_S * 1000u + 1000u); /* the tamper above is over */
    clear_records();
    send(CODE(DEV_A, OPEN));
    send(CODE(DEV_A, CLOSE));
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER, 0u);
    send(CODE(DEV_A, TAMP));
    EXPECT_POSTED(1u, ALARM_ZEV_TAMPER, 1u);
}

/* The owner's EV1527 contact: 6 open, 9 closed - and 9 again when its cover comes off. A tamper
 * input on 9 would show every time the door shuts, so 9 is offered as its closed code only,
 * and nothing is left to pick: the sensor has no tamper code of its own. */
static void test_a_cover_that_sends_the_closed_code_offers_nothing(void)
{
    bind_main(1u, DEV_A, 0x6u, 0u);
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&H, 1u, &b));
    char q[48];
    zsrc_rf_codes_query(&b.in[ZSRC_ROLE_MAIN].addr, q, sizeof(q));
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, NOW));
    send(CODE(DEV_A, 0x6u)); /* open */
    send(CODE(DEV_A, 0x9u)); /* closed */
    send(CODE(DEV_A, 0x9u)); /* the cover comes off */
    TEST_ASSERT_EQUAL_UINT(2u, zone_hub_candidates(&H, ZSRC_RF, q, 0u, c, ZSRC_RF_CAND_MAX));
    TEST_ASSERT_EQUAL_UINT16(1u, c[0].zone);
    TEST_ASSERT_EQUAL_STRING("Code 9", c[1].name);
    TEST_ASSERT_EQUAL_STRING("Closes the zone, twice", c[1].detail);
    TEST_ASSERT_TRUE(c[1].unusable);
}

/* The picker's "12 s ago" moves on with the clock, not only when the next frame arrives (the
 * newest sensor stayed "just now" for good on the panel); while learning, the source asks the
 * hub for a tick every second. */
static void test_the_picker_says_how_long_ago(void)
{
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, NOW));
    send(CODE(DEV_A, OPEN));
    TEST_ASSERT_EQUAL_UINT(1u, candidates(c));
    TEST_ASSERT_EQUAL_STRING("just now", c[0].state);
    TEST_ASSERT_TRUE(zone_hub_next_deadline(&H, NOW) <= NOW + 1000u);
    advance(15000u);
    TEST_ASSERT_EQUAL_UINT(1u, candidates(c));
    TEST_ASSERT_NOT_NULL(strstr(c[0].state, " s ago"));
    TEST_ASSERT_EQUAL_STRING("16 s ago", c[0].state);
    advance(120000u);
    candidates(c);
    TEST_ASSERT_EQUAL_STRING("2 min ago", c[0].state);
}

/* Not an RF address, or a query that is not one: nothing special. */
static void test_the_codes_query_is_only_for_rf(void)
{
    char q[48] = "x";
    const zsrc_address_t wired = zsrc_address(ZSRC_WIRED, 0u, 4u);
    zsrc_rf_codes_query(&wired, q, sizeof(q));
    TEST_ASSERT_EQUAL_STRING("", q);
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, NOW));
    send(CODE(DEV_A, OPEN));
    send(CODE(DEV_A, OPEN));
    static const char *const odd[] = {"codes", "codes 1", "codes 1 ", "codes 1 XYZ", "codes 99 A2B3C",
                                      "codes 1 A2B3C0", "codes 1 A2B3C "};
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, zone_hub_candidates(&H, ZSRC_RF, odd[i], 0u, c, ZSRC_RF_CAND_MAX),
                                       odd[i]);
        TEST_ASSERT_EQUAL_STRING("Sensor A2B3C", c[0].name); /* the sensors, as always */
    }
}

/* ---- jamming chopped up (RF-005) ------------------------------------------------------------ */

/* A jammer that pauses now and then (panels that reset their timer on any quiet sample were
 * beaten by 50 s on / 0.2 s off): 30 s of carrier in the last minute is jamming however it is
 * cut up. */
static void test_a_pulsed_jammer_is_still_jamming(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    for (unsigned cycle = 0; cycle < 3u && !zsrc_rf_jammed(&SRC); cycle++) {
        for (unsigned t = 0; t < 50000u; t += 200u) { /* 50 s on, sampled every 200 ms */
            zsrc_rf_noise(&SRC, -40, NOW);
            advance(200u);
        }
        zsrc_rf_noise(&SRC, -95, NOW); /* 0.2 s off */
        advance(200u);
    }
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));

    /* chopped finer still: 1 s on, 1 s off - half of every minute */
    const zsrc_sink_t sink = SRC.sink;
    zsrc_rf_init(&SRC, &sink);
    zsrc_rf_receiver(&SRC, true, NOW);
    for (unsigned t = 0; t < 70000u; t += 1000u) {
        zsrc_rf_noise(&SRC, (t / 1000u) % 2u == 0u ? -40 : -95, NOW);
        advance(1000u);
    }
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));
}

/* Busy but honest: sensors and a neighbour's remote now and then, 10 s of carrier a minute. */
static void test_a_busy_band_is_not_jamming(void)
{
    for (unsigned t = 0; t < 300000u; t += 1000u) {
        zsrc_rf_noise(&SRC, (t / 1000u) % 6u == 0u ? -40 : -95, NOW);
        advance(1000u);
    }
    TEST_ASSERT_FALSE(zsrc_rf_jammed(&SRC));
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_ON);
}

/* The bench, 2026-09-25: a contact a metre from the CC1101 reads -15 to -27 dBm and talks for
 * about two seconds an event (36 frames), and an installer testing it keeps it on the air most
 * of the minute. The CC1101's line toggles on noise, so the RSSI is sampled once a second
 * whatever arrives; samples inside the sensor's frames were counted as a jammer, and "Sensor
 * radio problem" came up. Only the moment before its first frame is decoded still counts.
 * RF-005 */
static void talk_and_sample(uint32_t code, uint64_t period_ms, uint64_t talk_ms, uint64_t total_ms)
{
    const zsrc_rf_packet_t p = zsrc_rf_ev1527(code);
    for (uint64_t t = 0; t < total_ms; t += 50u) {
        const uint64_t phase = t % period_ms;
        const bool on_air = phase < talk_ms;
        if (t % 1000u == 0u) {
            zsrc_rf_noise(&SRC, on_air ? -20 : -90, NOW);
        }
        if (on_air && phase >= 100u) { /* a frame is handed out once the next one begins */
            zsrc_rf_packet(&SRC, &p, NOW);
        }
        advance(50u);
    }
}

static void test_a_sensor_talking_is_not_jamming(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    talk_and_sample(CODE(DEV_A, OPEN), 2500u, 2000u, 180000u); /* on the air 80 % of 3 minutes */
    TEST_ASSERT_FALSE(zsrc_rf_jammed(&SRC));
    TEST_ASSERT_EQUAL_UINT(0u, TROUBLE_ON);
    TEST_ASSERT_TRUE(SRC.confirmed > 60u);
}

/* A transmitter that never lets go (a stuck button, a neighbour's dying sensor) blocks the band
 * like any carrier once it has gone on past a burst: its frames excuse nothing from then on. */
static void test_a_stuck_transmitter_is_jamming(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    talk_and_sample(CODE(DEV_B, 1u), 120000u, 120000u, 45000u);
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.bursts);
}

/* A jammer is not excused by a sensor that gets a word in now and then. */
static void test_a_jammer_with_a_sensor_in_between_is_jamming(void)
{
    bind_main(1u, DEV_A, OPEN, 0u);
    for (unsigned i = 0; i < 4u; i++) {
        jam(-40, 10000u);
        send_frames(CODE(DEV_A, OPEN), 4u);
    }
    TEST_ASSERT_TRUE(zsrc_rf_jammed(&SRC));
}

/* RF-016, RF02: a key fob's press bound mid-press reaches the new input, and its rolling code
 * is then known to it: played back later, refused; a replayed burst bound mid-way moves
 * nothing. */
static void test_a_fob_bound_mid_press_keeps_the_replay_rule(void)
{
    const uint32_t fob = 0x0ABCDEFu;
    const zsrc_rf_packet_t press = keeloq(fob, 0x2u, 0x31313131u, false);
    send_pkt(&press, 3u); /* confirmed, nobody bound */
    bind_fob(1u, fob, 0x2u, 5u);
    send_pkt(&press, 2u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
    advance(6000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    send_pkt(&press, 4u); /* the recording */
    advance(1000u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.replays);
    /* a replay going on while the input is bound again: still nothing */
    send_pkt(&press, 2u);
    TEST_ASSERT_EQUAL_UINT32(2u, SRC.replays);
    zone_hub_unbind(&H, 1u, NOW);
    bind_fob(1u, fob, 0x2u, 5u);
    send_pkt(&press, 3u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, SRC.confirmed);
}

/* ZSRC-005, RF31: the source watches ZSRC_RF_WATCH_MAX inputs. A new binding past that is refused
 * as "no room" once the panel runs; a stored one past it (loaded at start) is kept, shows CHECK,
 * and is watched as soon as a slot frees - by an unbind, or by saving it again unchanged. */
static void bind_dev(uint16_t zone, uint32_t device)
{
    bind_role(zone, ZSRC_ROLE_MAIN, device, OPEN, 0u);
}

static void test_a_binding_past_the_watch_table_waits_for_room(void)
{
    /* the stored bindings, loaded before the hub starts: one more than the source can watch */
    zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed,
                            .source_trouble = port_trouble};
    zone_hub_init(&H, &port);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_rf_ops(), &SRC, NOW));
    const uint16_t extra = (uint16_t)(ZSRC_RF_WATCH_MAX + 1u);
    const uint16_t last = (uint16_t)(extra + 1u);
    for (uint16_t z = 1u; z <= last; z++) {
        bind_dev(z, 0x10000u + z);
    }
    zone_hub_start(&H, NOW);
    clear_records();
    send(CODE(0x10000u + extra, OPEN));
    send(CODE(0x10000u + last, OPEN));
    EXPECT_POSTED(extra, ALARM_ZEV_FAULT, 0u); /* not watched */
    EXPECT_POSTED(last, ALARM_ZEV_FAULT, 0u);
    advance(ZSRC_RF_GRACE_S * 1000u + 1000u);
    EXPECT_POSTED(extra, ALARM_ZEV_UNREACHABLE, 1u); /* CHECK, as before */
    EXPECT_POSTED(last, ALARM_ZEV_UNREACHABLE, 1u);

    /* a new one while it is full: refused up front, and the table is unchanged */
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, 0x0F00Du, OPEN);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, 40u, &b, NOW));
    TEST_ASSERT_FALSE(zone_hub_get(&H, 40u, &b));
    /* the same zone's sensor swapped for another: the slot it frees is its own */
    bind_dev(5u, 0x0F00Eu);

    /* a slot frees: the first waiting input is watched at once and its CHECK clears */
    clear_records();
    zone_hub_unbind(&H, 1u, NOW);
    EXPECT_POSTED(extra, ALARM_ZEV_REACHABLE, 1u);
    EXPECT_POSTED(last, ALARM_ZEV_REACHABLE, 0u); /* no room for the second yet */
    send(CODE(0x10000u + extra, OPEN));
    EXPECT_POSTED(extra, ALARM_ZEV_FAULT, 1u);
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, 0x0F00Du, OPEN);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, 1u, &b, NOW)); /* full again */

    /* a slot freed behind the hub's back (here: zone 3's, by hand): saving the waiting binding
     * unchanged watches it */
    const zsrc_address_t gone = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, 0x10003u, OPEN);
    zsrc_rf_ops()->unwatch(&SRC, &gone);
    TEST_ASSERT_TRUE(zone_hub_get(&H, last, &b));
    clear_records();
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, last, &b, NOW));
    EXPECT_POSTED(last, ALARM_ZEV_REACHABLE, 1u);
    send(CODE(0x10000u + last, OPEN));
    EXPECT_POSTED(last, ALARM_ZEV_FAULT, 1u);
}

/* RF31: a zone whose kept input still waits for a slot, saved again with a new input: the kept
 * one is retried first, so both need room - else the new one would be accepted and not watched.
 * Saved without the new one, the waiting one takes the slot. */
static void test_a_waiting_input_counts_when_its_zone_gains_one(void)
{
    zone_hub_port_t port = {.ctx = NULL, .zone_event = port_event, .changed = port_changed,
                            .source_trouble = port_trouble};
    zone_hub_init(&H, &port);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_rf_ops(), &SRC, NOW));
    const uint16_t last = (uint16_t)(ZSRC_RF_WATCH_MAX + 1u);
    for (uint16_t z = 1u; z <= last; z++) {
        bind_dev(z, 0x10000u + z);
    }
    zone_hub_start(&H, NOW);
    const zsrc_address_t gone = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, 0x10001u, OPEN);
    zsrc_rf_ops()->unwatch(&SRC, &gone); /* one slot, freed behind the hub's back */
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(zone_hub_get(&H, last, &b));
    b.in[ZSRC_ROLE_TAMPER].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, 0x10000u + last, TAMP);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_TAMPER].map, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, last, &b, NOW));
    memset(&b.in[ZSRC_ROLE_TAMPER], 0, sizeof(b.in[ZSRC_ROLE_TAMPER]));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, last, &b, NOW));
    clear_records();
    send(CODE(0x10000u + last, OPEN));
    EXPECT_POSTED(last, ALARM_ZEV_FAULT, 1u);
}

/* RF31: a binding of several inputs needs a slot for each new one. */
static void test_a_binding_needs_room_for_every_new_input(void)
{
    for (uint16_t z = 1u; z < ZSRC_RF_WATCH_MAX; z++) {
        bind_dev(z, 0x10000u + z);
    }
    /* one slot left: a contact with a tamper code needs two */
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, OPEN);
    b.in[ZSRC_ROLE_TAMPER].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, TAMP);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_DOOR);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_TAMPER].map, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, 40u, &b, NOW));
    memset(&b.in[ZSRC_ROLE_TAMPER], 0, sizeof(b.in[ZSRC_ROLE_TAMPER]));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 40u, &b, NOW));
    /* full: the zone's tamper added later is refused, its main kept; a new main code fits in
     * the slot the old one leaves */
    b.in[ZSRC_ROLE_TAMPER].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, TAMP);
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_TAMPER].map, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, 40u, &b, NOW));
    b.in[ZSRC_ROLE_MAIN].addr = zsrc_rf_address(ZSRC_RF_PROTO_EV1527, DEV_A, CLOSE);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_ERR_FULL, zone_hub_bind(&H, 40u, &b, NOW)); /* two new, one freed */
    memset(&b.in[ZSRC_ROLE_TAMPER], 0, sizeof(b.in[ZSRC_ROLE_TAMPER]));
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, 40u, &b, NOW));
    clear_records();
    send(CODE(DEV_A, CLOSE));
    EXPECT_POSTED(40u, ALARM_ZEV_FAULT, 1u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_usable_only_with_a_receiver);
    RUN_TEST(test_learning_lists_sensors_not_codes);
    RUN_TEST(test_a_lone_frame_is_not_a_sensor);
    RUN_TEST(test_the_best_sensor_is_the_one_that_answered);
    RUN_TEST(test_the_learn_table_makes_room_for_the_newest);
    RUN_TEST(test_a_bound_zone_starts_quiet_and_known);
    RUN_TEST(test_the_open_code_opens_and_another_code_closes);
    RUN_TEST(test_a_lonely_code_restores_after_the_automatic_hold);
    RUN_TEST(test_an_explicit_hold_only_ever_times_out);
    RUN_TEST(test_a_frame_from_the_near_future_keeps_its_zone_open);
    RUN_TEST(test_repeats_extend_the_hold);
    RUN_TEST(test_tamper_and_battery_codes_are_their_own_inputs);
    RUN_TEST(test_a_stranger_is_counted_and_ignored);
    RUN_TEST(test_a_burst_far_beyond_a_finger_is_noted);
    RUN_TEST(test_one_code_feeds_one_zone);
    RUN_TEST(test_addresses_round_trip_and_bad_ones_are_refused);
    RUN_TEST(test_the_hold_survives_the_blob);
    RUN_TEST(test_jamming_is_the_sources_trouble);
    RUN_TEST(test_a_short_carrier_is_not_jamming);
    RUN_TEST(test_a_carrier_without_rssi_is_jamming_too);
    RUN_TEST(test_a_missing_receiver_is_the_sources_trouble);
    RUN_TEST(test_unbinding_forgets_the_code);
    RUN_TEST(test_a_lone_frame_moves_no_zone);
    RUN_TEST(test_two_sensors_interleaved_both_count);
    RUN_TEST(test_a_checked_frame_acts_at_once);
    RUN_TEST(test_a_door_opened_and_shut_at_once);
    RUN_TEST(test_open_shut_open_at_once_ends_open);
    RUN_TEST(test_a_bounce_ends_on_the_last_code);
    RUN_TEST(test_a_lone_other_code_does_not_split_a_transmission);
    RUN_TEST(test_a_stuck_transmitter);
    RUN_TEST(test_every_transmission_is_a_check_in);
    RUN_TEST(test_a_replayed_rolling_code_is_refused);
    RUN_TEST(test_the_battery_bit_reaches_the_zone);
    RUN_TEST(test_every_protocol_binds_by_its_own_widths);
    RUN_TEST(test_unknown_frames_are_refused);
    RUN_TEST(test_the_numbers_are_the_decoders);
    RUN_TEST(test_a_zones_other_inputs_list_its_sensors_codes);
    RUN_TEST(test_a_cover_that_sends_the_closed_code_offers_nothing);
    RUN_TEST(test_the_picker_says_how_long_ago);
    RUN_TEST(test_the_codes_query_is_only_for_rf);
    RUN_TEST(test_a_pulsed_jammer_is_still_jamming);
    RUN_TEST(test_a_busy_band_is_not_jamming);
    RUN_TEST(test_a_sensor_talking_is_not_jamming);
    RUN_TEST(test_a_stuck_transmitter_is_jamming);
    RUN_TEST(test_a_jammer_with_a_sensor_in_between_is_jamming);
    RUN_TEST(test_another_zones_code_restores_an_automatic_input);
    RUN_TEST(test_a_zones_own_codes_hold_while_anothers_restore);
    RUN_TEST(test_a_code_bound_mid_burst_gets_the_rest_of_it);
    RUN_TEST(test_a_code_bound_during_an_endless_burst_gets_nothing);
    RUN_TEST(test_a_fob_bound_mid_press_keeps_the_replay_rule);
    RUN_TEST(test_a_binding_past_the_watch_table_waits_for_room);
    RUN_TEST(test_a_binding_needs_room_for_every_new_input);
    RUN_TEST(test_a_waiting_input_counts_when_its_zone_gains_one);
    return UNITY_END();
}
