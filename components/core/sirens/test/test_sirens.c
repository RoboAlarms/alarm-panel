/*
 * test_sirens.c - the sirens' shared policy, the wired outputs and the 433 MHz sirens
 * (docs/features/16 OUT-002..OUT-014).
 */
#include <string.h>

#include "sirens/out_wired.h"
#include "sirens/rf_siren.h"
#include "sirens/siren.h"
#include "sirens_fixture_v1.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- a fake port for the wired outputs ------------------------------------------------------- */

#define PINS 64
static struct {
    bool out[PINS];      /* configured as an output */
    bool in[PINS];       /* configured as an input */
    bool level[PINS];    /* what it drives */
    uint8_t reads[PINS]; /* what an input reads */
    int writes;
    bool refuse[PINS];
    int trouble;         /* -1 never told */
    uint64_t edges[64];  /* the times pin 4 changed */
    size_t n_edges;
    uint64_t now;
} F;

static bool f_setup(void *ctx, uint8_t gpio, bool output, bool level)
{
    (void)ctx;
    if (F.refuse[gpio]) {
        return false;
    }
    F.out[gpio] = output;
    F.in[gpio] = !output;
    if (output) {
        F.level[gpio] = level;
    }
    return true;
}
static void f_release(void *ctx, uint8_t gpio)
{
    (void)ctx;
    F.out[gpio] = false;
    F.in[gpio] = false;
}
static void f_write(void *ctx, uint8_t gpio, bool level)
{
    (void)ctx;
    if (gpio == 4u && F.level[gpio] != level && F.n_edges < 64u) {
        F.edges[F.n_edges++] = F.now;
    }
    F.level[gpio] = level;
    F.writes++;
}
static uint8_t f_read(void *ctx, uint8_t gpio)
{
    (void)ctx;
    return F.reads[gpio];
}
static void f_trouble(void *ctx, bool on)
{
    (void)ctx;
    F.trouble = on ? 1 : 0;
}

static const ow_port_t PORT = {.setup = f_setup, .release = f_release, .write = f_write,
                               .read = f_read, .bell_trouble = f_trouble};

static ow_t O;

static void wired_start(void)
{
    memset(&F, 0, sizeof(F));
    F.trouble = -1;
    ow_init(&O, &PORT);
}

static ow_config_t siren_cfg(uint8_t gpio)
{
    const ow_config_t c = {.function = OW_FN_SIREN, .gpio = gpio, .partitions = 0x01u,
                           .burglary = true, .fire = true, .squawk = false, .active_low = false,
                           .sense = OW_SENSE_NONE};
    return c;
}

/* Run the outputs from `from` to `to` in their own deadlines (as the panel's timer would). */
static void run(uint64_t from, uint64_t to)
{
    uint64_t t = from;
    while (t <= to) {
        F.now = t;
        ow_poll(&O, t);
        uint64_t next = ow_next_deadline(&O, t);
        if (next <= t) {
            next = t + 1u;
        }
        t = next;
    }
}

/* ---- the policy ------------------------------------------------------------------------------ */

/* OUT-003: several partitions share one siren; fire wins, then CO, then burglary. */
static void test_fire_wins_across_partitions(void)
{
    siren_view_t v;
    siren_view_init(&v);
    const siren_scope_t both = {.partitions = 0x03u, .burglary = true, .fire = true};
    (void)siren_view_output(&v, 10u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_INT(SIREN_BURGLARY, siren_pick(&both, &v, 10u));
    (void)siren_view_output(&v, 20u, 2u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL4);
    TEST_ASSERT_EQUAL_INT(SIREN_CO, siren_pick(&both, &v, 20u));
    (void)siren_view_output(&v, 30u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_INT(SIREN_FIRE, siren_pick(&both, &v, 30u));
    (void)siren_view_output(&v, 40u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    TEST_ASSERT_EQUAL_INT(SIREN_CO, siren_pick(&both, &v, 40u));
    /* a siren of partition 1 only hears none of partition 2's */
    const siren_scope_t one = {.partitions = 0x01u, .burglary = true, .fire = true};
    TEST_ASSERT_EQUAL_INT(SIREN_QUIET, siren_pick(&one, &v, 40u));
}

/* OUT-003: a burglary-only siren stays quiet for fire, a fire-only one for burglary. */
static void test_scope_by_alarm_kind(void)
{
    siren_view_t v;
    siren_view_init(&v);
    const siren_scope_t burg = {.partitions = 0x01u, .burglary = true, .fire = false};
    const siren_scope_t fire = {.partitions = 0x01u, .burglary = false, .fire = true};
    (void)siren_view_output(&v, 1u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_INT(SIREN_QUIET, siren_pick(&burg, &v, 1u));
    TEST_ASSERT_EQUAL_INT(SIREN_FIRE, siren_pick(&fire, &v, 1u));
    (void)siren_view_output(&v, 2u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_INT(SIREN_BURGLARY, siren_pick(&burg, &v, 2u));
    TEST_ASSERT_EQUAL_INT(SIREN_QUIET, siren_pick(&fire, &v, 2u));
}

/* OUT-008: the bell test is a one-shot pulse of SIREN_TEST_MS, never over a sounding alarm. */
static void test_bell_test_is_a_pulse(void)
{
    siren_view_t v;
    siren_view_init(&v);
    const siren_scope_t s = {.partitions = 0x01u, .burglary = true, .fire = false};
    (void)siren_view_output(&v, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEST);
    TEST_ASSERT_EQUAL_INT(SIREN_TEST, siren_pick(&s, &v, 1000u));
    TEST_ASSERT_EQUAL_UINT64(1000u + SIREN_TEST_MS, siren_view_deadline(&s, &v, 1500u));
    TEST_ASSERT_EQUAL_INT(SIREN_QUIET, siren_pick(&s, &v, 1000u + SIREN_TEST_MS));
    TEST_ASSERT_EQUAL_UINT64(0u, siren_view_deadline(&s, &v, 1000u + SIREN_TEST_MS));
    (void)siren_view_output(&v, 5000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    (void)siren_view_output(&v, 5001u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEST);
    TEST_ASSERT_EQUAL_INT(SIREN_BURGLARY, siren_pick(&s, &v, 5002u));
}

/* OUT-002: temporal-3 (NFPA 72) and temporal-4 (NFPA 720) to the millisecond. */
static void test_temporal_patterns(void)
{
    uint32_t next = 0u;
    /* temporal-3: on 0-500, off 500-1000, on 1000-1500, off -2000, on 2000-2500, off 2500-4000 */
    TEST_ASSERT_TRUE(siren_level(SIREN_FIRE, 0u, &next));
    TEST_ASSERT_EQUAL_UINT32(500u, next);
    TEST_ASSERT_FALSE(siren_level(SIREN_FIRE, 500u, &next));
    TEST_ASSERT_TRUE(siren_level(SIREN_FIRE, 2499u, &next));
    TEST_ASSERT_EQUAL_UINT32(1u, next);
    TEST_ASSERT_FALSE(siren_level(SIREN_FIRE, 2500u, &next));
    TEST_ASSERT_EQUAL_UINT32(1500u, next);
    TEST_ASSERT_TRUE(siren_level(SIREN_FIRE, 4000u, &next)); /* the next cycle */
    /* temporal-4: four 100 ms pulses, then 5 s quiet: a 5.7 s cycle */
    TEST_ASSERT_TRUE(siren_level(SIREN_CO, 0u, &next));
    TEST_ASSERT_EQUAL_UINT32(100u, next);
    TEST_ASSERT_TRUE(siren_level(SIREN_CO, 650u, &next));
    TEST_ASSERT_FALSE(siren_level(SIREN_CO, 700u, &next));
    TEST_ASSERT_EQUAL_UINT32(5000u, next);
    TEST_ASSERT_TRUE(siren_level(SIREN_CO, 5700u, &next));
    /* steady and quiet never change */
    TEST_ASSERT_TRUE(siren_level(SIREN_BURGLARY, 123456u, &next));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, next);
    TEST_ASSERT_FALSE(siren_level(SIREN_QUIET, 0u, &next));
}

/* OUT-007: chirps - one on arming, two on disarming. */
static void test_chirp_counts(void)
{
    uint32_t next = 0u;
    TEST_ASSERT_EQUAL_UINT32(SIREN_CHIRP_ON_MS, siren_chirp_ms(1u));
    TEST_ASSERT_EQUAL_UINT32(2u * SIREN_CHIRP_ON_MS + SIREN_CHIRP_OFF_MS, siren_chirp_ms(2u));
    TEST_ASSERT_TRUE(siren_chirp_level(2u, 0u, &next));
    TEST_ASSERT_FALSE(siren_chirp_level(2u, SIREN_CHIRP_ON_MS, &next));
    TEST_ASSERT_TRUE(siren_chirp_level(2u, SIREN_CHIRP_ON_MS + SIREN_CHIRP_OFF_MS, &next));
    TEST_ASSERT_FALSE(siren_chirp_level(2u, siren_chirp_ms(2u), &next));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, next);
}

/* ---- wired outputs --------------------------------------------------------------------------- */

/* OUT-006: a channel's pin is an output at its inactive level the moment it is configured. */
static void test_safe_at_boot(void)
{
    wired_start();
    ow_config_t c = siren_cfg(4u);
    c.active_low = true;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    TEST_ASSERT_TRUE(F.out[4]);
    TEST_ASSERT_TRUE(F.level[4]); /* active low: high is off */
    TEST_ASSERT_EQUAL_INT(0, (int)F.n_edges);
    ow_output(&O, 10u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_FALSE(F.level[4]);
    ow_output(&O, 20u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    TEST_ASSERT_TRUE(F.level[4]);
    /* cleared: inactive first, then released */
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_clear(&O, 0u));
    TEST_ASSERT_TRUE(F.level[4]);
    TEST_ASSERT_FALSE(F.out[4]);
}

/* OUT-002: the wired siren plays temporal-3 for fire, with edges where the standard puts them. */
static void test_wired_fire_cadence(void)
{
    wired_start();
    const ow_config_t c = siren_cfg(4u);
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    F.now = 1000u;
    ow_output(&O, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    run(1000u, 9000u);
    static const uint64_t want[] = {1000u, 1500u, 2000u, 2500u, 3000u, 3500u,
                                    5000u, 5500u, 6000u, 6500u, 7000u, 7500u, 9000u};
    TEST_ASSERT_EQUAL_INT((int)(sizeof(want) / sizeof(want[0])), (int)F.n_edges);
    for (size_t i = 0; i < F.n_edges; i++) {
        TEST_ASSERT_EQUAL_UINT64(want[i], F.edges[i]);
    }
    /* silenced: off at once */
    F.now = 9100u;
    ow_output(&O, 9100u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    TEST_ASSERT_FALSE(F.level[4]);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ow_next_deadline(&O, 9100u));
}

/* OUT-007: squawks on a wired siren: one chirp on arming, two on disarming; none without
 * `squawk`, none while it sounds. */
static void test_wired_squawk(void)
{
    wired_start();
    ow_config_t c = siren_cfg(4u);
    c.squawk = true;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    F.now = 100u;
    ow_armed(&O, 100u, 1u, true);
    run(100u, 3000u);
    TEST_ASSERT_EQUAL_INT(2, (int)F.n_edges); /* on, off */
    TEST_ASSERT_EQUAL_UINT64(100u + SIREN_CHIRP_ON_MS, F.edges[1]);
    F.n_edges = 0u;
    F.now = 5000u;
    ow_armed(&O, 5000u, 1u, false);
    run(5000u, 8000u);
    TEST_ASSERT_EQUAL_INT(4, (int)F.n_edges);
    /* while sounding, a disarm is not a chirp: the alarm's end is the feedback */
    F.n_edges = 0u;
    F.now = 10000u;
    ow_armed(&O, 10000u, 1u, true);
    run(10000u, 11000u);
    ow_output(&O, 12000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    F.n_edges = 0u;
    F.now = 13000u;
    ow_armed(&O, 13000u, 1u, false);
    ow_output(&O, 13000u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    run(13000u, 16000u);
    TEST_ASSERT_EQUAL_INT(1, (int)F.n_edges); /* only the alarm's end */
    TEST_ASSERT_FALSE(F.level[4]);
    /* without squawk: nothing */
    wired_start();
    c.squawk = false;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    ow_armed(&O, 100u, 1u, true);
    run(100u, 3000u);
    TEST_ASSERT_EQUAL_INT(0, (int)F.n_edges);
}

/* OUT-009: strobe, smoke reset and armed channels follow their outputs. */
static void test_other_functions(void)
{
    wired_start();
    const ow_config_t strobe = {.function = OW_FN_STROBE, .gpio = 5u, .partitions = 0x01u,
                                .sense = OW_SENSE_NONE};
    const ow_config_t smoke = {.function = OW_FN_SMOKE_RESET, .gpio = 6u, .sense = OW_SENSE_NONE};
    const ow_config_t armed = {.function = OW_FN_ARMED, .gpio = 7u, .partitions = 0x02u,
                               .sense = OW_SENSE_NONE};
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &strobe));
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 1u, &smoke));
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 2u, &armed));
    ow_output(&O, 1u, 1u, ALARM_OUT_STROBE, ALARM_PAT_ON);
    TEST_ASSERT_TRUE(F.level[5]);
    ow_output(&O, 2u, 0u, ALARM_OUT_SMOKE_RESET, ALARM_PAT_ON);
    TEST_ASSERT_TRUE(F.level[6]);
    ow_output(&O, 3u, 0u, ALARM_OUT_SMOKE_RESET, ALARM_PAT_OFF);
    TEST_ASSERT_FALSE(F.level[6]);
    ow_armed(&O, 4u, 1u, true);
    TEST_ASSERT_FALSE(F.level[7]); /* partition 1 isn't its */
    ow_set_armed(&O, 5u, 2u, true);
    TEST_ASSERT_TRUE(F.level[7]);
}

/* OUT-009: the configuration's rules - no shared pins, a siren must sound for something. */
static void test_wired_configuration_rules(void)
{
    wired_start();
    ow_config_t c = siren_cfg(4u);
    c.burglary = false;
    c.fire = false;
    TEST_ASSERT_EQUAL_INT(OW_ERR_ARG, ow_set(&O, 0u, 0u, &c));
    c = siren_cfg(4u);
    c.partitions = 0u;
    TEST_ASSERT_EQUAL_INT(OW_ERR_ARG, ow_set(&O, 0u, 0u, &c));
    c = siren_cfg(4u);
    c.sense = 4u;
    TEST_ASSERT_EQUAL_INT(OW_ERR_ARG, ow_set(&O, 0u, 0u, &c));
    c = siren_cfg(4u);
    c.sense = 5u;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    ow_config_t d = siren_cfg(5u);
    TEST_ASSERT_EQUAL_INT(OW_ERR_IN_USE, ow_set(&O, 0u, 1u, &d));
    TEST_ASSERT_EQUAL_INT(0, ow_find_pin(&O, 5u));
    F.refuse[9] = true;
    d = siren_cfg(9u);
    TEST_ASSERT_EQUAL_INT(OW_ERR_PIN, ow_set(&O, 0u, 1u, &d));
    TEST_ASSERT_EQUAL_INT(-1, ow_find_pin(&O, 9u));
    /* moving a channel to another pin gives the old one back */
    d = siren_cfg(8u);
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &d));
    TEST_ASSERT_FALSE(F.out[4]);
    TEST_ASSERT_FALSE(F.in[5]);
    TEST_ASSERT_TRUE(F.out[8]);
}

/* OUT-004: a supervision input open for OW_SENSE_MS is the bell trouble; closed clears it. */
static void test_bell_supervision(void)
{
    wired_start();
    ow_config_t c = siren_cfg(4u);
    c.sense = 5u;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    TEST_ASSERT_TRUE(F.in[5]);
    run(0u, 1000u);
    TEST_ASSERT_EQUAL_INT(-1, F.trouble);
    F.reads[5] = 1u; /* the loop opens */
    run(1000u, 1000u + OW_SENSE_MS - 200u);
    TEST_ASSERT_EQUAL_INT(-1, F.trouble);
    run(1000u + OW_SENSE_MS - 200u, 1000u + OW_SENSE_MS + 200u);
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    F.reads[5] = 0u;
    run(20000u, 20200u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    /* a short open is not a trouble */
    F.reads[5] = 1u;
    run(30000u, 31000u);
    F.reads[5] = 0u;
    run(31000u, 50000u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
}

/* OUT-009: "Test it" sounds the siren for OW_TEST_MS, refused while armed. */
static void test_wired_test(void)
{
    wired_start();
    const ow_config_t c = siren_cfg(4u);
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    F.now = 100u;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_test(&O, 100u, 0u));
    TEST_ASSERT_TRUE(F.level[4]);
    run(100u, 100u + OW_TEST_MS + 10u);
    TEST_ASSERT_FALSE(F.level[4]);
    TEST_ASSERT_EQUAL_UINT64(100u + OW_TEST_MS, F.edges[1]);
    ow_set_armed(&O, 5000u, 1u, true);
    TEST_ASSERT_EQUAL_INT(OW_ERR_ARMED, ow_test(&O, 5000u, 0u));
}

/* OUT-009: the stored configuration reads back, a frozen blob still reads, damage changes
 * nothing. */
static void test_wired_store(void)
{
    wired_start();
    ow_config_t c = siren_cfg(4u);
    c.squawk = true;
    c.active_low = true;
    c.sense = 5u;
    c.partitions = 0x03u;
    c.also = 0x04u;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 1u, &c));
    const ow_config_t a = {.function = OW_FN_ARMED, .gpio = 2u, .partitions = 0x01u,
                           .sense = OW_SENSE_NONE};
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 3u, &a));
    uint8_t buf[OW_BLOB_MAX];
    const size_t n = ow_encode(&O, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(OW_FIXTURE_V1), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(OW_FIXTURE_V1, buf, n);
    ow_config_t got[OW_CHANNELS];
    TEST_ASSERT_TRUE(ow_decode(OW_FIXTURE_V1, sizeof(OW_FIXTURE_V1), got));
    TEST_ASSERT_EQUAL_UINT8(OW_FN_NONE, got[0].function);
    TEST_ASSERT_EQUAL_UINT8(OW_FN_SIREN, got[1].function);
    TEST_ASSERT_EQUAL_UINT8(4u, got[1].gpio);
    TEST_ASSERT_EQUAL_UINT8(0x03u, got[1].partitions);
    TEST_ASSERT_TRUE(got[1].burglary && got[1].fire && got[1].squawk && got[1].active_low);
    TEST_ASSERT_EQUAL_UINT8(5u, got[1].sense);
    TEST_ASSERT_EQUAL_UINT8(0x04u, got[1].also);
    TEST_ASSERT_EQUAL_UINT8(OW_FN_ARMED, got[3].function);
    TEST_ASSERT_EQUAL_UINT8(OW_SENSE_NONE, got[3].sense);
    uint8_t bad[sizeof(OW_FIXTURE_V1)];
    memcpy(bad, OW_FIXTURE_V1, sizeof(bad));
    bad[8] ^= 0x01u;
    got[1].gpio = 99u;
    TEST_ASSERT_FALSE(ow_decode(bad, sizeof(bad), got));
    TEST_ASSERT_EQUAL_UINT8(99u, got[1].gpio);
    TEST_ASSERT_FALSE(ow_decode(OW_FIXTURE_V1, sizeof(OW_FIXTURE_V1) - 1u, got));
}

/* ---- 433 MHz sirens ---------------------------------------------------------------------------- */

static struct {
    rf_packet_t sent[64];
    uint64_t at[64];
    uint8_t frames[64];
    size_t n;
    bool refuse;
    int trouble;
    uint64_t now;
} R;

static bool r_send(void *ctx, const rf_packet_t *p, uint16_t unit_us, uint8_t frames)
{
    (void)ctx;
    (void)unit_us;
    if (R.refuse) {
        return false;
    }
    if (R.n < 64u) {
        R.sent[R.n] = *p;
        R.at[R.n] = R.now;
        R.frames[R.n] = frames;
        R.n++;
    }
    return true;
}
static void r_trouble(void *ctx, bool on)
{
    (void)ctx;
    R.trouble = on ? 1 : 0;
}

static rfs_t S;
static const rfs_port_t RPORT = {.send = r_send, .bell_trouble = r_trouble};

static void rf_start(void)
{
    memset(&R, 0, sizeof(R));
    R.trouble = -1;
    rfs_init(&S, &RPORT);
    rfs_transmitter(&S, 0u, true);
}

static void rf_run(uint64_t from, uint64_t to)
{
    uint64_t t = from;
    while (t <= to) {
        R.now = t;
        rfs_poll(&S, t);
        uint64_t next = rfs_next_deadline(&S, t);
        if (next <= t) {
            next = t + 1u;
        }
        if (next == UINT64_MAX) {
            break;
        }
        t = next;
    }
}

/* OUT-010: while its sound is wanted the alarm code goes out at once and every RFS_REPEAT_MS;
 * the stop code RFS_STOP_SENDS times after. */
static void test_rf_alarm_repeats_and_stops(void)
{
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0x12345u);
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 0u, &c));
    TEST_ASSERT_EQUAL_size_t(0u, R.n);
    R.now = 1000u;
    rfs_output(&S, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_size_t(1u, R.n);
    TEST_ASSERT_EQUAL_UINT8(RF_PROTO_EV1527, R.sent[0].proto);
    TEST_ASSERT_EQUAL_UINT32(0x12345u, R.sent[0].device);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_ALARM, R.sent[0].data);
    TEST_ASSERT_EQUAL_UINT8(RFS_FRAMES, R.frames[0]);
    rf_run(1000u, 1000u + 2u * RFS_REPEAT_MS + 10u);
    TEST_ASSERT_EQUAL_size_t(3u, R.n);
    TEST_ASSERT_EQUAL_UINT64(1000u + RFS_REPEAT_MS, R.at[1]);
    R.now = 50000u;
    rfs_output(&S, 50000u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    rf_run(50000u, 60000u);
    TEST_ASSERT_EQUAL_size_t(3u + RFS_STOP_SENDS, R.n);
    for (size_t i = 3u; i < R.n; i++) {
        TEST_ASSERT_EQUAL_UINT8(RFS_CODE_STOP, R.sent[i].data);
    }
    TEST_ASSERT_EQUAL_UINT64(50000u + RFS_STOP_GAP_MS, R.at[4]);
    /* fire sounds the one sound too (a fire siren), and a burglary-only one stays quiet */
    R.n = 0u;
    c.fire = false;
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 70000u, 0u, &c));
    rfs_output(&S, 70000u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_size_t(0u, R.n);
}

/* OUT-007, OUT-010: a 433 MHz siren with squawk gets the arm and disarm codes. */
static void test_rf_squawk(void)
{
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0x23456u);
    c.squawk = true;
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 0u, &c));
    rfs_set_armed(&S, 1u, false); /* start-up: nothing */
    TEST_ASSERT_EQUAL_size_t(0u, R.n);
    rfs_armed(&S, 100u, 1u, true);
    rfs_armed(&S, 200u, 1u, true); /* no change, no squawk */
    rfs_armed(&S, 300u, 1u, false);
    TEST_ASSERT_EQUAL_size_t(2u, R.n);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_ARM, R.sent[0].data);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_DISARM, R.sent[1].data);
}

/* OUT-011: pairing sends the arm code RFS_PAIR_SENDS times, longer; refused while armed. */
static void test_rf_pairing(void)
{
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0x34567u);
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 2u, &c));
    R.now = 10u;
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_pair(&S, 10u, 2u));
    rf_run(10u, 10000u);
    TEST_ASSERT_EQUAL_size_t(RFS_PAIR_SENDS, R.n);
    TEST_ASSERT_EQUAL_UINT8(RFS_PAIR_FRAMES, R.frames[0]);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_ARM, R.sent[2].data);
    rfs_set_armed(&S, 1u, true);
    TEST_ASSERT_EQUAL_INT(RFS_ERR_ARMED, rfs_pair(&S, 20000u, 2u));
    TEST_ASSERT_EQUAL_INT(RFS_ERR_UNKNOWN, rfs_pair(&S, 20000u, 1u));
}

/* OUT-012: no transmitter while a siren is assigned is the bell trouble; a test says so. */
static void test_rf_trouble_without_transmitter(void)
{
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0x45678u);
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 0u, &c));
    TEST_ASSERT_EQUAL_INT(-1, R.trouble);
    rfs_transmitter(&S, 10u, false);
    TEST_ASSERT_EQUAL_INT(1, R.trouble);
    TEST_ASSERT_EQUAL_INT(RFS_ERR_SEND, rfs_test(&S, 20u, 0u));
    rfs_transmitter(&S, 30u, true);
    TEST_ASSERT_EQUAL_INT(0, R.trouble);
    R.refuse = true;
    rfs_output(&S, 40u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_INT(1, R.trouble);
    R.refuse = false;
    rf_run(40u, 40u + RFS_STOP_GAP_MS + 1u);
    TEST_ASSERT_EQUAL_INT(0, R.trouble);
    TEST_ASSERT_EQUAL_size_t(1u, R.n); /* tried again soon, and it went */
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_clear(&S, 50000u, 0u)); /* stop goes out */
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_STOP, R.sent[R.n - 1u].data);
}

/* OUT-010: "Test it": the alarm code, then stop RFS_TEST_MS later. */
static void test_rf_test(void)
{
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0x56789u);
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 0u, &c));
    R.now = 100u;
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_test(&S, 100u, 0u));
    rf_run(100u, 100u + RFS_TEST_MS + 3u * RFS_STOP_GAP_MS);
    TEST_ASSERT_EQUAL_size_t(1u + RFS_STOP_SENDS, R.n);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_ALARM, R.sent[0].data);
    TEST_ASSERT_EQUAL_UINT64(100u + RFS_TEST_MS, R.at[1]);
}

/* OUT-014: a new siren's address is 20 bits, never a factory one, never one in use. */
static void test_rf_addresses(void)
{
    const uint32_t avoid[] = {0x00ABCu, 0x48A71u};
    TEST_ASSERT_EQUAL_HEX32(0x00ABDu, rfs_pick_address(0x00ABDu, avoid, 2u));
    const uint32_t a = rfs_pick_address(0x00ABCu, avoid, 2u);
    TEST_ASSERT_NOT_EQUAL(0u, a);
    TEST_ASSERT_NOT_EQUAL(0x00ABCu, a);
    TEST_ASSERT_TRUE(a <= 0xFFFFFu);
    TEST_ASSERT_FALSE(rf_address_is_common(RF_PROTO_EV1527, rfs_pick_address(0xFFFFFu, NULL, 0u)));
    TEST_ASSERT_NOT_EQUAL(0u, rfs_pick_address(0u, NULL, 0u));
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0x11111u);
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 0u, &c));
    TEST_ASSERT_EQUAL_INT(RFS_ERR_IN_USE, rfs_set(&S, 0u, 1u, &c));
    c.address = 0xFFFFFu;
    TEST_ASSERT_EQUAL_INT(RFS_ERR_ARG, rfs_set(&S, 0u, 1u, &c));
    rfs_defaults(&c, 0x22222u);
    c.code_alarm = 0x10u;
    TEST_ASSERT_EQUAL_INT(RFS_ERR_ARG, rfs_set(&S, 0u, 1u, &c));
    TEST_ASSERT_EQUAL_INT(1, rfs_free_slot(&S));
}

/* OUT-013: the 433 MHz sirens' store round-trips and its frozen blob still reads. */
static void test_rf_store(void)
{
    rf_start();
    rfs_config_t c;
    rfs_defaults(&c, 0xA1B2Cu);
    c.squawk = true;
    c.partitions = 0x05u;
    c.also = 0x02u;
    c.fire = false;
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 2u, &c));
    uint8_t buf[RFS_BLOB_MAX];
    const size_t n = rfs_encode(&S, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(RFS_FIXTURE_V1), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(RFS_FIXTURE_V1, buf, n);
    rfs_config_t got[RFS_SIRENS];
    TEST_ASSERT_TRUE(rfs_decode(RFS_FIXTURE_V1, sizeof(RFS_FIXTURE_V1), got));
    TEST_ASSERT_FALSE(got[0].used);
    TEST_ASSERT_TRUE(got[2].used);
    TEST_ASSERT_EQUAL_HEX32(0xA1B2Cu, got[2].address);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_ALARM, got[2].code_alarm);
    TEST_ASSERT_EQUAL_UINT8(0x05u, got[2].partitions);
    TEST_ASSERT_TRUE(got[2].burglary && got[2].squawk && !got[2].fire);
    TEST_ASSERT_EQUAL_UINT8(0x02u, got[2].also);
    uint8_t bad[sizeof(RFS_FIXTURE_V1)];
    memcpy(bad, RFS_FIXTURE_V1, sizeof(bad));
    bad[7] ^= 0x10u;
    TEST_ASSERT_FALSE(rfs_decode(bad, sizeof(bad), got));
}

/* OUT-015: a siren of the house (partition 1) also sounds for the shop's (partition 2) alarms,
 * and its strobe - but the shop's arming doesn't chirp it, its bell test doesn't sound it, and
 * the shop being armed doesn't stop its own test. */
static void test_also_sounds_for_another_partition(void)
{
    siren_view_t v;
    siren_view_init(&v);
    const siren_scope_t house = {.partitions = 0x01u, .also = 0x02u, .burglary = true, .fire = true};
    (void)siren_view_output(&v, 10u, 2u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    (void)siren_view_output(&v, 10u, 2u, ALARM_OUT_STROBE, ALARM_PAT_ON);
    TEST_ASSERT_EQUAL_INT(SIREN_BURGLARY, siren_pick(&house, &v, 10u));
    TEST_ASSERT_TRUE(siren_strobe(&house, &v));
    (void)siren_view_output(&v, 20u, 2u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    (void)siren_view_output(&v, 20u, 2u, ALARM_OUT_BELL, ALARM_PAT_TEST);
    TEST_ASSERT_EQUAL_INT(SIREN_QUIET, siren_pick(&house, &v, 21u));
    (void)siren_view_armed(&v, 2u, true);
    TEST_ASSERT_FALSE(siren_armed(&house, &v));
    /* a wired siren of the house: the shop arming is no chirp, the shop's alarm sounds it */
    wired_start();
    ow_config_t c = siren_cfg(4u);
    c.also = 0x02u;
    c.squawk = true;
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_set(&O, 0u, 0u, &c));
    F.now = 100u;
    ow_armed(&O, 100u, 2u, true);
    run(100u, 2000u);
    TEST_ASSERT_EQUAL_INT(0, (int)F.n_edges);
    TEST_ASSERT_EQUAL_INT(OW_OK, ow_test(&O, 3000u, 0u)); /* the shop armed: still testable */
    run(3000u, 6000u);
    F.n_edges = 0u;
    F.now = 7000u;
    ow_output(&O, 7000u, 2u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_TRUE(F.level[4]);
    /* and a 433 MHz siren the same */
    rf_start();
    rfs_config_t r;
    rfs_defaults(&r, 0x13579u);
    r.also = 0x02u;
    TEST_ASSERT_EQUAL_INT(RFS_OK, rfs_set(&S, 0u, 0u, &r));
    rfs_output(&S, 100u, 2u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_size_t(1u, R.n);
    TEST_ASSERT_EQUAL_UINT8(RFS_CODE_ALARM, R.sent[0].data);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fire_wins_across_partitions);
    RUN_TEST(test_scope_by_alarm_kind);
    RUN_TEST(test_bell_test_is_a_pulse);
    RUN_TEST(test_also_sounds_for_another_partition);
    RUN_TEST(test_temporal_patterns);
    RUN_TEST(test_chirp_counts);
    RUN_TEST(test_safe_at_boot);
    RUN_TEST(test_wired_fire_cadence);
    RUN_TEST(test_wired_squawk);
    RUN_TEST(test_other_functions);
    RUN_TEST(test_wired_configuration_rules);
    RUN_TEST(test_bell_supervision);
    RUN_TEST(test_wired_test);
    RUN_TEST(test_wired_store);
    RUN_TEST(test_rf_alarm_repeats_and_stops);
    RUN_TEST(test_rf_squawk);
    RUN_TEST(test_rf_pairing);
    RUN_TEST(test_rf_trouble_without_transmitter);
    RUN_TEST(test_rf_test);
    RUN_TEST(test_rf_addresses);
    RUN_TEST(test_rf_store);
    return UNITY_END();
}
