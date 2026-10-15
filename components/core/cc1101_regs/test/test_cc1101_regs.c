/*
 * test_cc1101_regs.c - the CC1101's receive table and arithmetic (components/core/cc1101_regs,
 * docs/features/25 RF-017): the numbers the driver writes and the judgements it makes on what the
 * chip answers, proved without a chip.
 *
 * Requirements: RF-017
 */
#include <string.h>

#include "cc1101_regs/cc1101_regs.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* The FREQ word against TI's 26 MHz formula; 433.92 MHz is SmartRF Studio's 0x10B071. */
static void test_the_frequency_word(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x10B071u, cc1101_freq_word(433920u));
    TEST_ASSERT_EQUAL_HEX32(0x0C1D8Au, cc1101_freq_word(315000u));
    TEST_ASSERT_EQUAL_HEX32(0x0D44ECu, cc1101_freq_word(345000u));
    TEST_ASSERT_EQUAL_HEX32(0x0B89D9u, cc1101_freq_word(300000u));
    TEST_ASSERT_EQUAL_HEX32(0x11D89Eu, cc1101_freq_word(464000u));
}

static void test_the_bands(void)
{
    TEST_ASSERT_TRUE(cc1101_band_ok(433920u));
    TEST_ASSERT_TRUE(cc1101_band_ok(315000u));
    TEST_ASSERT_TRUE(cc1101_band_ok(345000u));
    TEST_ASSERT_TRUE(cc1101_band_ok(348000u));
    TEST_ASSERT_TRUE(cc1101_band_ok(387000u));
    TEST_ASSERT_FALSE(cc1101_band_ok(350000u)); /* the gap between the chip's bands */
    TEST_ASSERT_FALSE(cc1101_band_ok(299999u));
    TEST_ASSERT_FALSE(cc1101_band_ok(868300u)); /* a band this table is not for */
}

static const cc1101_reg_t *find(const cc1101_reg_t *t, size_t n, uint8_t reg)
{
    for (size_t i = 0; i < n; i++) {
        if (t[i].reg == reg) {
            return &t[i];
        }
    }
    return NULL;
}

/* The receive table: asynchronous serial data on GDO0, OOK without a sync word, the frequency
 * and the band's TEST0; every register once, all of them configuration registers. */
static void test_the_receive_table(void)
{
    cc1101_reg_t t[CC1101_TABLE_MAX];
    const size_t n = cc1101_ook_table(433920u, t, CC1101_TABLE_MAX);
    TEST_ASSERT_TRUE(n > 30u && n <= CC1101_TABLE_MAX);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(t[i].reg <= CC1101_LAST_CONFIG);
        for (size_t j = i + 1u; j < n; j++) {
            TEST_ASSERT_NOT_EQUAL(t[i].reg, t[j].reg);
        }
    }
    TEST_ASSERT_EQUAL_HEX8(0x0Du, find(t, n, CC1101_IOCFG0)->value); /* async serial out */
    TEST_ASSERT_EQUAL_HEX8(0x32u, find(t, n, 0x08u)->value);           /* PKTCTRL0 async, infinite */
    TEST_ASSERT_EQUAL_HEX8(0x30u, find(t, n, 0x12u)->value);           /* MDMCFG2: OOK, no sync */
    TEST_ASSERT_EQUAL_HEX8(0x07u, find(t, n, 0x16u)->value);           /* RX_TIME_RSSI off */
    TEST_ASSERT_EQUAL_HEX8(0x10u, find(t, n, CC1101_FREQ2)->value);
    TEST_ASSERT_EQUAL_HEX8(0xB0u, find(t, n, CC1101_FREQ1)->value);
    TEST_ASSERT_EQUAL_HEX8(0x71u, find(t, n, CC1101_FREQ0)->value);
    TEST_ASSERT_EQUAL_HEX8(0x09u, find(t, n, CC1101_TEST0)->value);
    const size_t m = cc1101_ook_table(315000u, t, CC1101_TABLE_MAX);
    TEST_ASSERT_EQUAL_UINT(n, m);
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, find(t, m, CC1101_TEST0)->value); /* below 322.88 MHz */
    TEST_ASSERT_EQUAL_UINT(0u, cc1101_ook_table(350000u, t, CC1101_TABLE_MAX));
    TEST_ASSERT_EQUAL_UINT(0u, cc1101_ook_table(433920u, t, 10u));
}

/* RF-017: the values at their datasheet addresses (SWRS061 table 43). FSTEST is 0x29, and the
 * production-test registers next to it - PTEST 0x2A, AGCTEST 0x2B - are left alone (TI: "for
 * test only"; the table once wrote FSTEST's value into PTEST, docs/bugs/B011). */
static void test_the_table_uses_the_datasheet_addresses(void)
{
    cc1101_reg_t t[CC1101_TABLE_MAX];
    const size_t n = cc1101_ook_table(433920u, t, CC1101_TABLE_MAX);
    TEST_ASSERT_NULL(find(t, n, 0x2Au)); /* PTEST */
    TEST_ASSERT_NULL(find(t, n, 0x2Bu)); /* AGCTEST */
    TEST_ASSERT_NOT_NULL(find(t, n, 0x29u));
    TEST_ASSERT_EQUAL_HEX8(0x59u, find(t, n, 0x29u)->value); /* FSTEST */
    TEST_ASSERT_EQUAL_HEX8(0x81u, find(t, n, 0x2Cu)->value); /* TEST2, bandwidths <= 325 kHz */
    TEST_ASSERT_EQUAL_HEX8(0x35u, find(t, n, 0x2Du)->value); /* TEST1, the same */
    TEST_ASSERT_EQUAL_HEX8(0x47u, find(t, n, 0x03u)->value); /* FIFOTHR: ADC retention, the same */
    TEST_ASSERT_EQUAL_HEX8(0x57u, find(t, n, 0x10u)->value); /* MDMCFG4: 325 kHz */
    TEST_ASSERT_EQUAL_HEX8(0xB6u, find(t, n, 0x21u)->value); /* FREND1: RX over 101 kHz */
    TEST_ASSERT_EQUAL_HEX8(0x18u, find(t, n, 0x18u)->value); /* MCSM0: calibrate IDLE -> RX */
}

/* Read-back: a register the chip did not take (a loose MOSI wire, a clone) is found. */
static void test_verification_finds_what_the_chip_did_not_take(void)
{
    cc1101_reg_t t[CC1101_TABLE_MAX];
    const size_t n = cc1101_ook_table(433920u, t, CC1101_TABLE_MAX);
    uint8_t read[CC1101_TABLE_MAX];
    for (size_t i = 0; i < n; i++) {
        read[i] = t[i].value;
    }
    TEST_ASSERT_EQUAL_UINT(n, cc1101_verify(t, n, read));
    read[5] ^= 0x01u;
    TEST_ASSERT_EQUAL_UINT(5u, cc1101_verify(t, n, read));
    memset(read, 0xFF, sizeof(read)); /* MISO stuck high */
    TEST_ASSERT_EQUAL_UINT(0u, cc1101_verify(t, n, read));
}

static void test_rssi_in_dbm(void)
{
    TEST_ASSERT_EQUAL_INT(-74, cc1101_rssi_dbm(0x00u));
    TEST_ASSERT_EQUAL_INT(-11, cc1101_rssi_dbm(0x7Fu)); /* the strongest */
    TEST_ASSERT_EQUAL_INT(-138, cc1101_rssi_dbm(0x80u)); /* the weakest */
    TEST_ASSERT_EQUAL_INT(-99, cc1101_rssi_dbm(0xCEu));  /* a quiet band: -50 half-dB */
    TEST_ASSERT_EQUAL_INT(-54, cc1101_rssi_dbm(40u));    /* a carrier: jamming territory */
}

/* RSSI register replies, one per read; a read at 'fail_at' fails on the bus. */
typedef struct {
    const uint8_t *replies;
    size_t n;
    size_t next;
    size_t fail_at;
} rssi_bus_t;

static bool rssi_bus_read(void *ctx, uint8_t *value)
{
    rssi_bus_t *bus = ctx;
    if (bus->next == bus->fail_at || bus->next >= bus->n) {
        return false;
    }
    *value = bus->replies[bus->next++];
    return true;
}

/* RF-017, RF14: a value is used only when two consecutive reads agree (errata SWRZ020). */
static void test_rssi_is_used_only_when_two_reads_agree(void)
{
    uint8_t raw = 0xAAu;
    uint8_t low = 0x55u;
    /* the first pair agrees: two reads, that value */
    const uint8_t first[] = {0x00u, 0x00u};
    rssi_bus_t bus = {first, sizeof(first), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_STABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0x00u, raw);
    TEST_ASSERT_EQUAL_UINT(2u, bus.next);

    /* a torn read, then two that agree: the agreeing value, not the torn one */
    const uint8_t later[] = {0x40u, 0x00u, 0xCEu, 0xCEu};
    bus = (rssi_bus_t){later, sizeof(later), 0u, SIZE_MAX};
    raw = 0xAAu;
    TEST_ASSERT_EQUAL(CC1101_RSSI_STABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0xCEu, raw);

    /* consecutive means consecutive: the 2nd and 3rd reads agree */
    const uint8_t sliding[] = {0x10u, 0x20u, 0x20u};
    bus = (rssi_bus_t){sliding, sizeof(sliding), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_STABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0x20u, raw);
    TEST_ASSERT_EQUAL_HEX8(0x55u, low); /* a stable read has a value, no lower bound */
}

/* RF14: every read within the budget differs from the one before - no value, and 'raw' is left
 * alone (the review's probe got -42 dBm, the first read of the last pair, as if valid). */
static void test_rssi_that_never_settles_is_unstable(void)
{
    const uint8_t torn[] = {0x40u, 0x00u, 0x41u, 0x01u, 0x42u, 0x02u, 0x40u, 0x00u, 0x00u};
    rssi_bus_t bus = {torn, sizeof(torn), 0u, SIZE_MAX};
    uint8_t raw = 0xAAu;
    uint8_t low = 0x55u;
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0xAAu, raw);
    TEST_ASSERT_EQUAL_UINT(CC1101_RSSI_READS, bus.next); /* the budget, no read more */
    /* a budget of one read can never agree */
    bus = (rssi_bus_t){torn, sizeof(torn), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, 1u, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0x40u, low); /* its one read is the lowest */
    /* no read at all: no lower bound either */
    low = 0x55u;
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, 0u, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0x55u, low);
}

/* RF-005, RF14 (review 2): a reading that never settles still bounds the signal from below: the
 * lowest of its reads, compared in dBm - the register is two's complement, so 0xF0 (-82 dBm) is
 * weaker than 0x10 (-66 dBm) though it is the bigger byte. The service feeds the jamming count
 * that bound when it is at or above the jamming level, so a carrier whose RSSI keeps moving
 * between reads can't make the receiver look quiet. */
static void test_rssi_that_never_settles_gives_its_lowest_read(void)
{
    uint8_t raw = 0xAAu;
    uint8_t low = 0x55u;
    /* below the jamming level somewhere: the lowest read, in dBm (a byte compare picks 0x10) */
    const uint8_t mixed[] = {0x10u, 0xF0u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u};
    rssi_bus_t bus = {mixed, sizeof(mixed), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0xF0u, low);
    TEST_ASSERT_EQUAL_INT(-82, cc1101_rssi_dbm(low));
    TEST_ASSERT_EQUAL_HEX8(0xAAu, raw); /* still no value */
    /* the weakest byte the register has, 0x80 (-138 dBm), against 0x7F (-11 dBm) */
    const uint8_t ends[] = {0x7Fu, 0x80u, 0x7Fu, 0x80u, 0x7Fu, 0x80u, 0x7Fu, 0x80u};
    bus = (rssi_bus_t){ends, sizeof(ends), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0x80u, low);
    /* a pulsed jammer: every read differs, none below -55 dBm - the bound is a carrier */
    const uint8_t jammer[] = {0x28u, 0x40u, 0x2Au, 0x50u, 0x2Cu, 0x60u, 0x2Eu, 0x70u};
    bus = (rssi_bus_t){jammer, sizeof(jammer), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0x28u, low);
    TEST_ASSERT_EQUAL_INT(-54, cc1101_rssi_dbm(low)); /* >= ZSRC_RF_JAM_DBM (-55) */
    /* NULL: the caller doesn't want it */
    bus = (rssi_bus_t){jammer, sizeof(jammer), 0u, SIZE_MAX};
    TEST_ASSERT_EQUAL(CC1101_RSSI_UNSTABLE, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, NULL));
}

/* RF14: a read that fails on the bus is a bus error (the chip gone), not an unstable sample. */
static void test_rssi_bus_failure_is_not_unstable(void)
{
    const uint8_t replies[] = {0x40u, 0x00u, 0x00u};
    rssi_bus_t bus = {replies, sizeof(replies), 0u, 0u};
    uint8_t raw = 0xAAu;
    uint8_t low = 0x55u;
    TEST_ASSERT_EQUAL(CC1101_RSSI_BUS_ERROR, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    bus = (rssi_bus_t){replies, sizeof(replies), 0u, 1u}; /* the second read fails */
    TEST_ASSERT_EQUAL(CC1101_RSSI_BUS_ERROR, cc1101_rssi_settle(rssi_bus_read, &bus, CC1101_RSSI_READS, &raw, &low));
    TEST_ASSERT_EQUAL_HEX8(0xAAu, raw);
    TEST_ASSERT_EQUAL_HEX8(0x55u, low); /* a bus error has no lower bound */
}

/* RF12 (the storm guard, RF-005): the edge rate over a window from a free-running counter of the
 * symbols the receiver handed over (dropped ones included), over the window's real length. */
static void test_edge_rate_over_the_window(void)
{
    /* 25,600 symbols in a second: 51,200 edges - over the guard, ring full or not */
    TEST_ASSERT_EQUAL_UINT32(51200u, cc1101_edges_per_s(25600u, 0u, 1000000u));
    /* the same symbols over two seconds: half the rate */
    TEST_ASSERT_EQUAL_UINT32(25600u, cc1101_edges_per_s(25600u, 0u, 2000000u));
    /* the counter wrapped within the window */
    TEST_ASSERT_EQUAL_UINT32(200u, cc1101_edges_per_s(50u, UINT32_MAX - 49u, 1000000u));
    /* nothing arrived */
    TEST_ASSERT_EQUAL_UINT32(0u, cc1101_edges_per_s(7u, 7u, 1000000u));
    /* a window shorter than a second counts as one: a short burst is not a big rate */
    TEST_ASSERT_EQUAL_UINT32(2000u, cc1101_edges_per_s(1000u, 0u, 1000u));
    TEST_ASSERT_EQUAL_UINT32(2000u, cc1101_edges_per_s(1000u, 0u, 0u));
    /* the whole counter in a second does not overflow */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, cc1101_edges_per_s(UINT32_MAX, 0u, 1000000u));
}

/* A genuine chip, a clone, and the ways an empty or broken bus answers. */
static void test_what_is_on_the_bus(void)
{
    TEST_ASSERT_EQUAL(CC1101_GENUINE, cc1101_identify(0x00u, 0x14u, 0x14u));
    TEST_ASSERT_EQUAL(CC1101_GENUINE, cc1101_identify(0x00u, 0x04u, 0x04u));
    TEST_ASSERT_EQUAL(CC1101_CLONE, cc1101_identify(0x00u, 0x17u, 0x17u));
    TEST_ASSERT_EQUAL(CC1101_ABSENT, cc1101_identify(0xFFu, 0xFFu, 0xFFu)); /* MISO pulled high */
    TEST_ASSERT_EQUAL(CC1101_ABSENT, cc1101_identify(0x00u, 0x00u, 0x00u)); /* MISO held low */
    TEST_ASSERT_EQUAL(CC1101_ABSENT, cc1101_identify(0x00u, 0x14u, 0x3Au)); /* a floating bus */
    TEST_ASSERT_EQUAL(CC1101_ABSENT, cc1101_identify(0x80u, 0x14u, 0x14u));
}

/* A brown-out puts the chip back at its reset values: GDO0 becomes a 135 kHz clock. */
static void test_a_reset_chip_is_noticed(void)
{
    TEST_ASSERT_FALSE(cc1101_lost_config(CC1101_IOCFG0_ASYNC));
    TEST_ASSERT_TRUE(cc1101_lost_config(CC1101_IOCFG0_RESET));
    TEST_ASSERT_TRUE(cc1101_lost_config(0xFFu)); /* the bus answered nothing */
}

static void test_state_names(void)
{
    TEST_ASSERT_EQUAL_STRING("rx", cc1101_state_name(CC1101_MARC_RX));
    TEST_ASSERT_EQUAL_STRING("idle", cc1101_state_name(CC1101_MARC_IDLE));
    TEST_ASSERT_EQUAL_STRING("rx overflow", cc1101_state_name(0x11u));
    TEST_ASSERT_EQUAL_STRING("no answer", cc1101_state_name(0xFFu));
    TEST_ASSERT_EQUAL_STRING("other", cc1101_state_name(0x1Fu));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_frequency_word);
    RUN_TEST(test_the_bands);
    RUN_TEST(test_the_receive_table);
    RUN_TEST(test_the_table_uses_the_datasheet_addresses);
    RUN_TEST(test_verification_finds_what_the_chip_did_not_take);
    RUN_TEST(test_rssi_in_dbm);
    RUN_TEST(test_rssi_is_used_only_when_two_reads_agree);
    RUN_TEST(test_rssi_that_never_settles_is_unstable);
    RUN_TEST(test_rssi_that_never_settles_gives_its_lowest_read);
    RUN_TEST(test_rssi_bus_failure_is_not_unstable);
    RUN_TEST(test_edge_rate_over_the_window);
    RUN_TEST(test_what_is_on_the_bus);
    RUN_TEST(test_a_reset_chip_is_noticed);
    RUN_TEST(test_state_names);
    return UNITY_END();
}
