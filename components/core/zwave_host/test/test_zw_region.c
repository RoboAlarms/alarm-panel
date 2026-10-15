/*
 * test_zw_region.c - what a region change does to the devices (docs/features/36 network.md §12;
 * ZW-045): United States with United States Long Range and Europe with Europe Long Range are
 * families that keep the classic band, every other region is its own family; leaving a Long Range
 * region for its classic one drops the Long Range devices; any other change drops every device.
 */
#include <stddef.h>
#include <stdio.h>

#include "unity.h"
#include "zwave_host/zw_region.h"

void setUp(void) {}
void tearDown(void) {}

/* Every region of serial-api.md §6.7 a module may offer, with its family written out by hand
 * (not through zw_region_family) and whether it has Long Range. */
static const struct {
    uint8_t code;
    uint8_t family;
    bool lr;
} REGIONS[] = {
    {0x00u, 'E', false}, {0x01u, 'U', false}, {0x02u, 'A', false}, {0x03u, 'H', false},
    {0x05u, 'I', false}, {0x06u, 'L', false}, {0x07u, 'R', false}, {0x08u, 'C', false},
    {0x09u, 'U', true},  {0x0Au, 'U', true},  {0x0Bu, 'E', true},  {0x20u, 'J', false},
    {0x21u, 'K', false}, {0xFFu, 'E', false},
};
#define N_REGIONS (sizeof REGIONS / sizeof REGIONS[0])

/* ZW-045: the families and Long Range, region by region. */
static void test_families(void)
{
    TEST_ASSERT_EQUAL_UINT8(ZW_REGION_US, zw_region_family(0x09u));
    TEST_ASSERT_EQUAL_UINT8(ZW_REGION_US, zw_region_family(0x0Au));
    TEST_ASSERT_EQUAL_UINT8(ZW_REGION_EU, zw_region_family(0x0Bu));
    TEST_ASSERT_EQUAL_UINT8(ZW_REGION_EU, zw_region_family(0xFFu)); /* the default is Europe */
    TEST_ASSERT_EQUAL_UINT8(0x20u, zw_region_family(0x20u));
    TEST_ASSERT_EQUAL_UINT8(0x02u, zw_region_family(0x02u));
    for (size_t i = 0; i < N_REGIONS; i++) {
        TEST_ASSERT_EQUAL(REGIONS[i].lr, zw_region_long_range(REGIONS[i].code));
    }
}

/* ZW-045: every pair of regions, with no devices, with classic devices only, with classic and
 * Long Range devices. */
static void test_every_pair(void)
{
    for (size_t i = 0; i < N_REGIONS; i++) {
        for (size_t j = 0; j < N_REGIONS; j++) {
            const uint8_t from = REGIONS[i].code, to = REGIONS[j].code;
            const bool same_family = REGIONS[i].family == REGIONS[j].family;
            const bool drops_lr = same_family && REGIONS[i].lr && !REGIONS[j].lr;
            char msg[48];
            (void)snprintf(msg, sizeof msg, "from %02X to %02X", (unsigned)from, (unsigned)to);
            uint16_t stop = 99u;

            /* no devices: nothing stops, but the kind is still said */
            zw_region_change_t r = zw_region_change(true, from, to, 0u, 0u, &stop);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, stop, msg);
            TEST_ASSERT_EQUAL_MESSAGE(same_family ? ZW_REGION_SAME_BAND : ZW_REGION_BAND_CHANGE,
                                      r, msg);

            /* 5 classic devices: they keep working within a family, else every one stops */
            r = zw_region_change(true, from, to, 5u, 0u, &stop);
            TEST_ASSERT_EQUAL_MESSAGE(same_family ? ZW_REGION_SAME_BAND : ZW_REGION_BAND_CHANGE,
                                      r, msg);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(same_family ? 0u : 5u, stop, msg);

            /* 5 classic and 2 Long Range: leaving Long Range for its classic region drops the 2 */
            r = zw_region_change(true, from, to, 5u, 2u, &stop);
            if (!same_family) {
                TEST_ASSERT_EQUAL_MESSAGE(ZW_REGION_BAND_CHANGE, r, msg);
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(7u, stop, msg);
            } else if (drops_lr) {
                TEST_ASSERT_EQUAL_MESSAGE(ZW_REGION_DROPS_LR, r, msg);
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(2u, stop, msg);
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(ZW_REGION_SAME_BAND, r, msg);
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, stop, msg);
            }
        }
    }
}

/* ZW-045: the usual changes, by name. */
static void test_usual_changes(void)
{
    uint16_t stop = 0u;
    /* United States -> with Long Range: the classic devices keep working */
    TEST_ASSERT_EQUAL(ZW_REGION_SAME_BAND, zw_region_change(true, 0x01u, 0x09u, 12u, 0u, &stop));
    TEST_ASSERT_EQUAL_UINT16(0u, stop);
    /* ... and back, with 3 Long Range devices: those 3 stop */
    TEST_ASSERT_EQUAL(ZW_REGION_DROPS_LR, zw_region_change(true, 0x09u, 0x01u, 12u, 3u, &stop));
    TEST_ASSERT_EQUAL_UINT16(3u, stop);
    /* Europe with Long Range -> Europe likewise */
    TEST_ASSERT_EQUAL(ZW_REGION_DROPS_LR, zw_region_change(true, 0x0Bu, 0x00u, 1u, 1u, &stop));
    TEST_ASSERT_EQUAL_UINT16(1u, stop);
    /* United States with Long Range -> Europe with Long Range: another band, all 15 stop */
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE,
                      zw_region_change(true, 0x09u, 0x0Bu, 12u, 3u, &stop));
    TEST_ASSERT_EQUAL_UINT16(15u, stop);
    /* Europe -> Australia and New Zealand */
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE, zw_region_change(true, 0x00u, 0x02u, 4u, 0u, &stop));
    TEST_ASSERT_EQUAL_UINT16(4u, stop);
    /* the stop count is optional */
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE, zw_region_change(true, 0x20u, 0x21u, 4u, 0u, NULL));
}

/* ZW-045: a region the module never said, or "unknown" (FE): nothing is assumed to keep
 * working, so every device is counted as stopping. */
static void test_unknown_region(void)
{
    uint16_t stop = 0u;
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE, zw_region_change(false, 0x01u, 0x01u, 6u, 1u, &stop));
    TEST_ASSERT_EQUAL_UINT16(7u, stop);
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE, zw_region_change(false, 0x00u, 0x09u, 6u, 0u, &stop));
    TEST_ASSERT_EQUAL_UINT16(6u, stop);
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE, zw_region_change(true, 0xFEu, 0x01u, 2u, 0u, &stop));
    TEST_ASSERT_EQUAL_UINT16(2u, stop);
    TEST_ASSERT_EQUAL(ZW_REGION_BAND_CHANGE, zw_region_change(true, 0x01u, 0xFEu, 2u, 0u, &stop));
    TEST_ASSERT_EQUAL_UINT16(2u, stop);
    /* the same region is no change at all, whatever it is */
    TEST_ASSERT_EQUAL(ZW_REGION_SAME_BAND, zw_region_change(true, 0xFEu, 0xFEu, 2u, 0u, &stop));
    TEST_ASSERT_EQUAL_UINT16(0u, stop);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_families);
    RUN_TEST(test_every_pair);
    RUN_TEST(test_usual_changes);
    RUN_TEST(test_unknown_region);
    return UNITY_END();
}
