/*
 * test_rf_devices.c - the 433 MHz sensors people buy, as mock devices (rf_mock.h), played as
 * pulses through a model of the air and the receiver into the real decoder (rf_decode) and the
 * real source (zsrc_rf), driven through the real hub (docs/features/25 "Mock devices").
 *
 * test_rf_decode.c proves the decoder on frames its own encoders make; test_rf.c proves the
 * source on packets handed to it. Here each product brings its own timings (a contact's 2.3 : 1
 * pulses, another's 3.5 : 1, a driveway sensor at T 188 us, a smoke alarm's stop pulses), its own
 * codes and habits (a PIR asleep after a trigger, a smoke alarm repeating while it sounds, a cover
 * that re-sends the door's code, a sagging cell), and the neighbours on the band (weather
 * stations, other alarm systems' sensors) must never decode.
 *
 * Requirements: RF-002, RF-003, RF-009..RF-016, RF-021, RF-023, RF-024 (docs/features/25)
 */
#include <stdio.h>
#include <string.h>

#include "rf_mock.h"
#include "unity.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_rf.h"

/* ---- harness ------------------------------------------------------------------------------- */

typedef struct {
    uint16_t zone;
    alarm_zone_event_t ev;
} rec_t;

static zone_hub_t H;
static zsrc_rf_t SRC;
static rfm_air_t AIR;
static uint64_t NOW_US;
static rec_t REC[512];
static unsigned NREC;
static rf_packet_t DEC[2048]; /* everything the decoder handed out */
static unsigned NDEC;
static rfm_device_t *DEVS[8]; /* devices with transmissions of their own (repeats, heartbeats) */
static unsigned NDEVS;

static uint64_t now_ms(void)
{
    return NOW_US / 1000u;
}

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
    (void)kind;
    (void)on;
}

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t v, uint64_t now)
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

/* What the rf task does with a decoded packet (rf_service.c deliver()). */
static void deliver(void *ctx, const rf_packet_t *p, uint64_t now_us)
{
    (void)ctx;
    if (NDEC < sizeof(DEC) / sizeof(DEC[0])) {
        DEC[NDEC++] = *p;
    }
    const zsrc_rf_packet_t zp = {.proto = p->proto, .bits = p->bits, .flags = p->flags,
                                 .data = p->data, .unit_us = p->unit_us, .device = p->device,
                                 .code = p->code, .extra = p->extra};
    (void)zsrc_rf_packet(&SRC, &zp, now_us / 1000u);
    zone_hub_tick(&H, now_us / 1000u);
}

static unsigned count(uint16_t zone, alarm_zone_event_t ev)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NREC; i++) {
        n += REC[i].zone == zone && REC[i].ev == ev;
    }
    return n;
}

#define EXPECT_POSTED(z_, e_, n) TEST_ASSERT_EQUAL_UINT_MESSAGE((n), count((z_), (e_)), #e_)

static void clear_records(void)
{
    NREC = 0;
    NDEC = 0;
}

/* The world goes on: the devices' own transmissions, the air, the hub's clock. */
static void run(uint64_t ms)
{
    const uint64_t until = NOW_US + ms * 1000u;
    while (NOW_US < until) {
        const uint64_t step = until - NOW_US < 100000u ? until - NOW_US : 100000u;
        NOW_US += step;
        for (unsigned i = 0; i < NDEVS; i++) {
            rfm_tick(&AIR, DEVS[i], NOW_US);
        }
        rfm_air_play(&AIR, NOW_US);
        zone_hub_tick(&H, now_ms());
    }
}

/* Something happens to a device; three seconds for its burst to be heard. */
static void act(rfm_device_t *d, rfm_event_t ev)
{
    (void)rfm_event(&AIR, d, ev, NOW_US);
    run(3000u);
}

static void world(const rfm_channel_t *ch)
{
    rfm_air_init(&AIR, ch, deliver, NULL, NOW_US);
    NDEVS = 0;
}

void setUp(void)
{
    NOW_US = 1000000u;
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
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&H, zsrc_rf_ops(), &SRC, now_ms()));
    zone_hub_start(&H, now_ms());
    zsrc_rf_receiver(&SRC, true, now_ms());
    world(NULL);
}

void tearDown(void) {}

/* An address of the device's width, different per profile, never a common factory one. */
static uint32_t address_for(const rfm_profile_t *p, unsigned salt)
{
    unsigned bits = p->addr_bits;
    if (p->shape == RFM_ENCODER) {
        bits = zsrc_rf_device_bits(p->proto);
    }
    const uint32_t mask = bits >= 32u ? 0xFFFFFFFFu : (1u << bits) - 1u;
    uint32_t a = (0x5A3C1u + salt * 0x1F3u + (uint32_t)(p - RFM_PROFILES) * 0x2E7u) & mask;
    if (a == 0u || a == mask) {
        a = 0x12345u & mask;
    }
    return a;
}

static void device(rfm_device_t *d, const char *id, unsigned salt)
{
    const rfm_profile_t *p = rfm_profile(id);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, id);
    rfm_device_init(d, p, address_for(p, salt));
}

/* The first event a device has a code for, in rfm_event_t order. */
static rfm_event_t first_event(const rfm_device_t *d)
{
    for (unsigned e = 0; e < (unsigned)RFM_HEARTBEAT; e++) {
        if (d->p->code[e] != RFM_NONE && d->p->code[e] != RFM_DOOR_STATE) {
            return (rfm_event_t)e;
        }
    }
    return RFM_HEARTBEAT;
}

static bool same(const rf_packet_t *a, const rf_packet_t *b)
{
    return a->proto == b->proto && a->device == b->device && a->data == b->data;
}

static unsigned decoded(const rf_packet_t *want, unsigned *others)
{
    unsigned n = 0;
    *others = 0;
    for (unsigned i = 0; i < NDEC; i++) {
        if (same(&DEC[i], want)) {
            n++;
        } else {
            (*others)++;
        }
    }
    return n;
}

static void bind_role(uint16_t zone, uint8_t role, const rfm_device_t *d, uint8_t data, uint8_t devclass,
                      uint16_t hold_s)
{
    zone_hub_binding_t b;
    if (!zone_hub_get(&H, zone, &b)) {
        memset(&b, 0, sizeof(b));
    }
    b.in[role].addr = zsrc_rf_address(d->p->proto, d->address, data);
    zsrc_mapping_defaults(&b.in[role].map, devclass);
    b.in[role].map.hold_s = hold_s;
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&H, zone, &b, now_ms()));
}

static size_t candidates(zsrc_candidate_t *c)
{
    return zone_hub_candidates(&H, ZSRC_RF, "", 0u, c, ZSRC_RF_CAND_MAX);
}

/* ---- every device decodes as itself (RF-002, RF-012, RF-013) ----------------------------- */

static void play_first_event(rfm_device_t *d)
{
    (void)rfm_event(&AIR, d, first_event(d), NOW_US);
    run(4000u);
}

/* Each product's first event, on a clean band: its frames decode as its protocol, its address
 * and its data, at least twice (a transmission, RF-009), and nothing else is made of them. */
static void test_every_device_decodes_as_itself(void)
{
    char msg[96];
    for (size_t i = 0; i < RFM_PROFILE_COUNT; i++) {
        const rfm_profile_t *p = &RFM_PROFILES[i];
        if (p->proto == RF_PROTO_NONE) {
            continue;
        }
        world(NULL);
        clear_records();
        rfm_device_t d;
        rfm_device_init(&d, p, address_for(p, 0u));
        const rfm_event_t ev = first_event(&d);
        const rf_packet_t want = rfm_expected(&d, rfm_code_for(&d, ev));
        play_first_event(&d);
        unsigned others = 0;
        const unsigned n = decoded(&want, &others);
        snprintf(msg, sizeof(msg), "%s: frames decoded", p->id);
        TEST_ASSERT_TRUE_MESSAGE(n >= ZSRC_RF_CONFIRM_FRAMES, msg);
        snprintf(msg, sizeof(msg), "%s: nothing else made of it", p->id);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, others, msg);
    }
}

/* The same through receivers that move the edges (a strong and a weak signal), jitter them,
 * lose the start of a burst while the AGC settles, or hiss on an empty band: the event still
 * arrives as a transmission, and nothing else is ever confirmed (RF-013).
 *
 * The one limit: a superhet's AGC (RXB6: ~0.17 s, docs/features/25 "Robustness" row 3) can eat
 * all but one frame of a short burst - a frame lost to it, plus the first whole frame after it
 * when the protocol's sync is a long low (HT6P20B, HT12E: the silence before is a gap, not the
 * pilot). Such a burst must be short (under the AGC and three frames), and the profiles it
 * happens to are printed; a CC1101 has no such AGC. */
static void test_every_device_survives_the_receiver(void)
{
    static const rfm_channel_t CHANNELS[] = {
        {.shift_us = 80},   {.shift_us = -80},   {.jitter_us = 40u},
        {.agc_us = 170000u}, {.static_per_s = 2000u},
    };
    char msg[120];
    for (size_t c = 0; c < sizeof(CHANNELS) / sizeof(CHANNELS[0]); c++) {
        for (size_t i = 0; i < RFM_PROFILE_COUNT; i++) {
            const rfm_profile_t *p = &RFM_PROFILES[i];
            if (p->proto == RF_PROTO_NONE) {
                continue;
            }
            world(&CHANNELS[c]);
            clear_records();
            rfm_device_t d;
            rfm_device_init(&d, p, address_for(p, 1u));
            const rf_packet_t want = rfm_expected(&d, rfm_code_for(&d, first_event(&d)));
            const uint64_t start = NOW_US;
            const uint64_t end = rfm_event(&AIR, &d, first_event(&d), NOW_US);
            run(4000u);
            unsigned others = 0;
            const unsigned n = decoded(&want, &others);
            const unsigned need = (want.flags & RF_PKT_CHECKED) != 0u ? 1u : ZSRC_RF_CONFIRM_FRAMES;
            if (n < need && CHANNELS[c].agc_us != 0u) {
                const uint64_t period = (end - start) / p->frames;
                snprintf(msg, sizeof(msg), "%s: a %lu ms burst lost to the AGC", p->id,
                         (unsigned long)((end - start) / 1000u));
                TEST_ASSERT_TRUE_MESSAGE(end - start < CHANNELS[c].agc_us + 3u * period, msg);
                TEST_MESSAGE(msg);
            } else {
                snprintf(msg, sizeof(msg), "%s through channel %u: frames decoded", p->id, (unsigned)c);
                TEST_ASSERT_TRUE_MESSAGE(n >= need, msg);
            }
            /* a lone corrupted frame may come out; it never repeats into a transmission */
            for (unsigned k = 0; k < NDEC; k++) {
                if (same(&DEC[k], &want)) {
                    continue;
                }
                unsigned again = 0;
                for (unsigned j = 0; j < NDEC; j++) {
                    again += same(&DEC[j], &DEC[k]);
                }
                snprintf(msg, sizeof(msg), "%s through channel %u: another code %06lX twice", p->id,
                         (unsigned)c, (unsigned long)DEC[k].code);
                TEST_ASSERT_TRUE_MESSAGE(again < ZSRC_RF_CONFIRM_FRAMES, msg);
            }
        }
    }
}

/* The neighbours - weather stations, another alarm system's sensors - for ten minutes, alone and
 * all at once on a hissing band: nothing of theirs decodes (RF-012, RF-013). */
static void test_neighbours_never_decode(void)
{
    static rfm_device_t n[8];
    unsigned k = 0;
    for (size_t i = 0; i < RFM_PROFILE_COUNT && k < 8u; i++) {
        if (RFM_PROFILES[i].proto == RF_PROTO_NONE) {
            rfm_device_init(&n[k], &RFM_PROFILES[i], address_for(&RFM_PROFILES[i], 2u));
            k++;
        }
    }
    TEST_ASSERT_TRUE(k >= 4u);
    const rfm_channel_t hiss = {.static_per_s = 2000u, .jitter_us = 20u};
    for (unsigned pass = 0; pass < 2u; pass++) {
        world(pass == 0u ? NULL : &hiss);
        clear_records();
        for (unsigned i = 0; i < k; i++) {
            DEVS[NDEVS++] = &n[i];
        }
        for (unsigned round = 0; round < 10u; round++) {
            for (unsigned i = 0; i < k; i++) {
                if (n[i].p->code[RFM_OPEN] != RFM_NONE) {
                    (void)rfm_event(&AIR, &n[i], round % 2u ? RFM_CLOSE : RFM_OPEN, NOW_US + i * 700000u);
                }
            }
            run(60000u);
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, NDEC, pass == 0u ? "clean band" : "hissing band");
        if (pass == 1u) {
            TEST_ASSERT_GREATER_THAN_UINT32(1000000u, AIR.dec.pulses); /* ten minutes of hiss */
        }
        TEST_ASSERT_EQUAL_UINT32(0u, SRC.confirmed);
    }
}

/* ---- learning (RF-003, RF-010, RF-011) ---------------------------------------------------- */

/* The installer does what the picker asks - opens and closes the door, walks past the PIR, presses
 * the test button or the remote's buttons - and the picker names that device (RF-011), whatever
 * product it is. */
static void test_every_sensor_is_found_while_learning(void)
{
    char msg[96];
    for (size_t i = 0; i < RFM_PROFILE_COUNT; i++) {
        const rfm_profile_t *p = &RFM_PROFILES[i];
        if (p->proto == RF_PROTO_NONE) {
            continue;
        }
        setUp();
        rfm_device_t d;
        rfm_device_init(&d, p, address_for(p, 3u));
        TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, now_ms()));
        const rfm_event_t ev = first_event(&d);
        const rfm_event_t again = ev == RFM_OPEN && p->code[RFM_CLOSE] != RFM_NONE ? RFM_CLOSE : ev;
        act(&d, ev);
        run(p->lockout_ms + 1000u); /* a PIR asleep would not answer */
        act(&d, again);
        zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
        snprintf(msg, sizeof(msg), "%s: one sensor listed", p->id);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, candidates(c), msg);
        snprintf(msg, sizeof(msg), "%s: named as the one", p->id);
        TEST_ASSERT_TRUE_MESSAGE(c[0].best, msg);
        uint8_t proto = 0;
        uint32_t dev = 0;
        uint8_t data = 0;
        TEST_ASSERT_TRUE(zsrc_rf_address_parts(&c[0].addr, &proto, &dev, &data));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(p->proto, proto, p->id);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(d.address, dev, p->id);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(rfm_code_for(&d, ev), data, p->id); /* its first code */
    }
}

/* Learning in a real street: a neighbour's weather station, another's remote and another alarm
 * system's contact keep transmitting; the installer opens the door twice; the picker names the
 * door (RF-010, RF-011) and sets the remote it heard before learning apart. */
static void test_the_door_is_found_among_the_neighbours(void)
{
    rfm_device_t door, remote, wx, foreign;
    device(&door, "ev1527_contact_ae", 4u);
    device(&remote, "ev1527_remote4", 5u);
    device(&wx, "nexus_th", 6u);
    device(&foreign, "ppm41_other_contact", 7u);
    DEVS[NDEVS++] = &wx;
    act(&remote, RFM_BUTTON_2); /* before learning */
    TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, now_ms()));
    act(&foreign, RFM_OPEN);
    act(&door, RFM_OPEN);
    run(20000u);
    act(&foreign, RFM_CLOSE);
    act(&door, RFM_CLOSE);
    zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
    TEST_ASSERT_EQUAL_UINT(2u, candidates(c)); /* the door, then the remote heard before */
    TEST_ASSERT_TRUE(c[0].best);
    TEST_ASSERT_FALSE(c[1].best);
    uint8_t proto = 0;
    uint32_t dev = 0;
    uint8_t data = 0;
    TEST_ASSERT_TRUE(zsrc_rf_address_parts(&c[0].addr, &proto, &dev, &data));
    TEST_ASSERT_EQUAL_HEX32(door.address, dev);
    TEST_ASSERT_EQUAL_UINT8(0xA, data);
    TEST_ASSERT_EQUAL_STRING("Codes A and E, heard 2 times", c[0].detail);
}

/* ---- what each kind of device does to its zone (RF-014, RF-015, RF-021, RF-023) ---------- */

/* Every contact opens its zone; one with a closed code closes it, one without restores after the
 * automatic hold. */
static void test_every_contact_opens_and_closes_its_zone(void)
{
    char msg[96];
    for (size_t i = 0; i < RFM_PROFILE_COUNT; i++) {
        const rfm_profile_t *p = &RFM_PROFILES[i];
        if (p->proto == RF_PROTO_NONE || p->code[RFM_OPEN] == RFM_NONE) {
            continue;
        }
        setUp();
        rfm_device_t d;
        rfm_device_init(&d, p, address_for(p, 8u));
        bind_role(1u, ZSRC_ROLE_MAIN, &d, p->code[RFM_OPEN], ALARM_DEVCLASS_DOOR, 0u);
        clear_records();
        act(&d, RFM_OPEN);
        snprintf(msg, sizeof(msg), "%s: opened", p->id);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_FAULT), msg);
        if (p->code[RFM_CLOSE] != RFM_NONE) {
            act(&d, RFM_CLOSE);
            snprintf(msg, sizeof(msg), "%s: closed by its closed code", p->id);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_RESTORE), msg);
        } else {
            run(ZSRC_RF_AUTO_HOLD_S * 1000u - 5000u);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, count(1u, ALARM_ZEV_RESTORE), p->id);
            run(5000u);
            snprintf(msg, sizeof(msg), "%s: restored after the hold", p->id);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_RESTORE), msg);
        }
    }
}

/* The bench's EV1527 contact (B012): its cover re-sends the door's code. With the door shut
 * nothing happens; with it open the zone stays open (no second fault); the door then closes it. */
static void test_a_cover_that_resends_the_door_changes_nothing(void)
{
    rfm_device_t d;
    device(&d, "ev1527_contact_69", 9u);
    bind_role(1u, ZSRC_ROLE_MAIN, &d, 0x6, ALARM_DEVCLASS_DOOR, 0u);
    clear_records();
    act(&d, RFM_TAMPER); /* door shut: 9 */
    act(&d, RFM_TAMPER_BACK);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 0u);
    act(&d, RFM_OPEN);
    act(&d, RFM_TAMPER); /* door open: 6 again */
    act(&d, RFM_TAMPER_BACK);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    act(&d, RFM_CLOSE);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

/* Contacts with tamper and low-battery codes of their own (the E/7, A/E and 7/C
 * contacts): bound as the zone's tamper and battery inputs (RF-021), each reaches its input; a low
 * cell sends its battery code after the door's. */
static void test_tamper_and_battery_codes_reach_their_inputs(void)
{
    static const char *const IDS[] = {"ev1527_contact_e7", "ev1527_contact_ae", "ev1527_contact_7c"};
    for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) {
        setUp();
        rfm_device_t d;
        device(&d, IDS[i], 10u);
        bind_role(1u, ZSRC_ROLE_MAIN, &d, d.p->code[RFM_OPEN], ALARM_DEVCLASS_DOOR, 0u);
        bind_role(1u, ZSRC_ROLE_TAMPER, &d, d.p->code[RFM_TAMPER], ALARM_DEVCLASS_DOOR, 0u);
        bind_role(1u, ZSRC_ROLE_BATTERY, &d, d.p->code[RFM_BATTERY], ALARM_DEVCLASS_DOOR,
                  ZSRC_RF_BATTERY_HOLD_S);
        clear_records();
        act(&d, RFM_TAMPER);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_TAMPER), IDS[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, count(1u, ALARM_ZEV_FAULT), IDS[i]);
        d.low = true;
        act(&d, RFM_OPEN);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_FAULT), IDS[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_LOW_BATTERY), IDS[i]);
        act(&d, RFM_CLOSE);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_RESTORE), IDS[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, count(1u, ALARM_ZEV_LOW_BATTERY_RESTORE), IDS[i]);
    }
}

/* A PIR asleep after a trigger sends nothing: the zone restores after its hold even while
 * someone moves, and faults again at the next trigger it sends. The Sonoff PIR3-RF needs a
 * quiet minute first; the other PIR's sleep is short. */
static void test_a_pir_asleep_sends_nothing(void)
{
    rfm_device_t d;
    device(&d, "sonoff_pir3_rf", 11u);
    bind_role(1u, ZSRC_ROLE_MAIN, &d, 0xA, ALARM_DEVCLASS_MOTION, 0u);
    clear_records();
    act(&d, RFM_MOTION); /* t = 0 */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    run(17000u);
    act(&d, RFM_MOTION); /* t = 20 s: asleep, and the minute starts again */
    run(27000u);
    act(&d, RFM_MOTION); /* t = 50 s: still asleep */
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u); /* the hold ran out while it slept */
    TEST_ASSERT_EQUAL_UINT32(1u, d.sent);
    run(67000u);
    act(&d, RFM_MOTION); /* t = 2 min: a quiet minute behind it */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 2u);
    TEST_ASSERT_EQUAL_UINT32(2u, d.sent);

    setUp();
    device(&d, "ev1527_pir_t330", 12u);
    bind_role(1u, ZSRC_ROLE_MAIN, &d, 0xA, ALARM_DEVCLASS_MOTION, 0u);
    clear_records();
    act(&d, RFM_MOTION);
    act(&d, RFM_MOTION); /* 3 s later: asleep */
    run(5000u);
    act(&d, RFM_MOTION); /* awake again: sent, and its hold starts again */
    TEST_ASSERT_EQUAL_UINT32(2u, d.sent);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    run(ZSRC_RF_AUTO_HOLD_S * 1000u - 5000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 0u);
    run(5000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
}

/* Smoke and water sensors send their code again while the alarm lasts: the zone stays in alarm
 * as long as it does (each repeat a new transmission inside the hold), and restores after the
 * hold once it stops. */
static void test_smoke_and_water_keep_their_zone_while_they_last(void)
{
    static const struct {
        const char *id;
        uint8_t devclass;
    } S[] = {{"ev1527_smoke_6", ALARM_DEVCLASS_SMOKE}, {"smoke_wakeup_3", ALARM_DEVCLASS_SMOKE},
             {"ev1527_water_5", ALARM_DEVCLASS_WATER}, {"ev1527_water_8", ALARM_DEVCLASS_WATER}};
    for (size_t i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        setUp();
        rfm_device_t d;
        device(&d, S[i].id, 13u);
        DEVS[NDEVS++] = &d;
        bind_role(1u, ZSRC_ROLE_MAIN, &d, d.p->code[RFM_ALARM], S[i].devclass, 0u);
        clear_records();
        (void)rfm_alarm(&AIR, &d, true, NOW_US);
        run(90000u);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_FAULT), S[i].id);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, count(1u, ALARM_ZEV_RESTORE), S[i].id);
        TEST_ASSERT_TRUE_MESSAGE(d.sent >= 9u, S[i].id);
        (void)rfm_alarm(&AIR, &d, false, NOW_US);
        run(ZSRC_RF_AUTO_HOLD_S * 1000u + 2000u);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(1u, ALARM_ZEV_RESTORE), S[i].id);
    }
}

/* A remote's buttons are codes of one device: learning lists it once with the codes pressed;
 * a panic button bound to a zone sets it off; the other buttons don't. */
static void test_remote_buttons_are_one_devices_codes(void)
{
    static const char *const IDS[] = {"ev1527_remote4", "sonoff_rm433", "ev1527_alarm_remote"};
    for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) {
        setUp();
        rfm_device_t d;
        device(&d, IDS[i], 14u);
        TEST_ASSERT_TRUE(zone_hub_learn(&H, ZSRC_RF, true, now_ms()));
        act(&d, RFM_BUTTON_1);
        act(&d, RFM_BUTTON_2);
        zsrc_candidate_t c[ZSRC_RF_CAND_MAX];
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, candidates(c), IDS[i]);
        const uint8_t b1 = d.p->code[RFM_BUTTON_1];
        const uint8_t b2 = d.p->code[RFM_BUTTON_2];
        char want[48];
        snprintf(want, sizeof(want), "Codes %X and %X, heard 2 times", b1 < b2 ? b1 : b2,
                 b1 < b2 ? b2 : b1); /* the picker lists codes in order */
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, c[0].detail, IDS[i]);
        zone_hub_learn(&H, ZSRC_RF, false, now_ms());

        bind_role(2u, ZSRC_ROLE_MAIN, &d, d.p->code[RFM_BUTTON_4], ALARM_DEVCLASS_PANIC_BUTTON, 0u);
        clear_records();
        act(&d, RFM_BUTTON_1);
        act(&d, RFM_BUTTON_3);
        EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 0u);
        act(&d, RFM_BUTTON_4);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, count(2u, ALARM_ZEV_FAULT), IDS[i]);
    }
}

/* A KeeLoq fob: each press a new hopping code; a recording of a press played back is refused
 * (RF-016), and a low cell's VLOW reaches the zone as low battery. */
static void test_a_rolling_code_fob_refuses_a_replay(void)
{
    rfm_device_t d;
    device(&d, "hcs301_fob", 15u);
    bind_role(1u, ZSRC_ROLE_MAIN, &d, 0x1, ALARM_DEVCLASS_PANIC_BUTTON, 10u);
    clear_records();
    rfm_device_t thief = d; /* what a recording of the next press holds */
    act(&d, RFM_BUTTON_1);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    run(15000u);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    act(&thief, RFM_BUTTON_1); /* played back */
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    TEST_ASSERT_TRUE(SRC.replays >= 1u);
    d.low = true;
    act(&d, RFM_BUTTON_1);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 2u);
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY, 1u);
}

/* RF-024: a 12 V EV1527 sensor's oscillator slows as its cell sags (about 31 % at 5 V): at T
 * 500 us that is 655 us, inside the decoder's range (RF_EV1527_UNIT_MAX_US), and its sync space
 * (31 T, 20.3 ms) must still reach the decoder - the receiver must not end the reception inside
 * it - or the panel stops hearing the sensor just as its low-battery code matters. */
static void test_a_sagging_cell_is_still_heard(void)
{
    rfm_device_t d;
    device(&d, "ev1527_12v_slow", 16u);
    bind_role(1u, ZSRC_ROLE_MAIN, &d, 0x6, ALARM_DEVCLASS_DOOR, 0u);
    bind_role(1u, ZSRC_ROLE_BATTERY, &d, 0x3, ALARM_DEVCLASS_DOOR, ZSRC_RF_BATTERY_HOLD_S);
    clear_records();
    d.low = true;
    act(&d, RFM_OPEN);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(1u, ALARM_ZEV_LOW_BATTERY, 1u);
    act(&d, RFM_CLOSE);
    EXPECT_POSTED(1u, ALARM_ZEV_RESTORE, 1u);
    /* the slowest T the decoder takes: its sync space fits in a reception */
    TEST_ASSERT_TRUE(31u * RF_EV1527_UNIT_MAX_US < RF_RECEPTION_IDLE_US);
}

/* A door and a PIR transmitting at once (OOK has no capture effect: their carriers add up): the
 * frames that overlap are lost, those alone on the air decode, and both zones fault. */
static void test_two_sensors_overlapping_both_count(void)
{
    rfm_device_t door, pir;
    device(&door, "ev1527_contact_69", 17u);
    device(&pir, "ev1527_pir_t330", 18u);
    bind_role(1u, ZSRC_ROLE_MAIN, &door, 0x6, ALARM_DEVCLASS_DOOR, 0u);
    bind_role(2u, ZSRC_ROLE_MAIN, &pir, 0xA, ALARM_DEVCLASS_MOTION, 0u);
    clear_records();
    const uint64_t door_end = rfm_event(&AIR, &door, RFM_OPEN, NOW_US);
    (void)rfm_event(&AIR, &pir, RFM_MOTION, door_end - 400000u); /* over the door's last 0.4 s */
    run(4000u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 1u);
}

/* The limit, in writing: a short burst wholly inside a longer one (a PIR's 1 s inside the
 * door contact's 2 s) has no frame alone on the air and is lost; the longer one still counts, and
 * nothing else is made of the mixture. The PIR's next trigger, or its supervision, is what
 * remains (RF-015). */
static void test_a_burst_inside_a_longer_one_is_lost(void)
{
    rfm_device_t door, pir;
    device(&door, "ev1527_contact_69", 19u);
    device(&pir, "ev1527_pir_t330", 20u);
    bind_role(1u, ZSRC_ROLE_MAIN, &door, 0x6, ALARM_DEVCLASS_DOOR, 0u);
    bind_role(2u, ZSRC_ROLE_MAIN, &pir, 0xA, ALARM_DEVCLASS_MOTION, 0u);
    clear_records();
    (void)rfm_event(&AIR, &door, RFM_OPEN, NOW_US);
    (void)rfm_event(&AIR, &pir, RFM_MOTION, NOW_US + 300000u);
    run(4000u);
    EXPECT_POSTED(1u, ALARM_ZEV_FAULT, 1u);
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 0u);
    const rf_packet_t opened = rfm_expected(&door, 0x6);
    unsigned others = 0;
    TEST_ASSERT_TRUE(decoded(&opened, &others) >= ZSRC_RF_CONFIRM_FRAMES);
    TEST_ASSERT_EQUAL_UINT(0u, others); /* nothing made of the mixture */
    run(6000u);
    act(&pir, RFM_MOTION); /* awake again: heard */
    EXPECT_POSTED(2u, ALARM_ZEV_FAULT, 1u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_device_decodes_as_itself);
    RUN_TEST(test_every_device_survives_the_receiver);
    RUN_TEST(test_neighbours_never_decode);
    RUN_TEST(test_every_sensor_is_found_while_learning);
    RUN_TEST(test_the_door_is_found_among_the_neighbours);
    RUN_TEST(test_every_contact_opens_and_closes_its_zone);
    RUN_TEST(test_a_cover_that_resends_the_door_changes_nothing);
    RUN_TEST(test_tamper_and_battery_codes_reach_their_inputs);
    RUN_TEST(test_a_pir_asleep_sends_nothing);
    RUN_TEST(test_smoke_and_water_keep_their_zone_while_they_last);
    RUN_TEST(test_remote_buttons_are_one_devices_codes);
    RUN_TEST(test_a_rolling_code_fob_refuses_a_replay);
    RUN_TEST(test_a_sagging_cell_is_still_heard);
    RUN_TEST(test_two_sensors_overlapping_both_count);
    RUN_TEST(test_a_burst_inside_a_longer_one_is_lost);
    return UNITY_END();
}
