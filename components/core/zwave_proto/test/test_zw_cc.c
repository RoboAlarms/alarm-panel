/*
 * test_zw_cc.c - the command classes the panel speaks (components/core/zwave_proto/src/zw_cc_*.c,
 * docs/features/36 command-classes.md): every codec round-trips, the doc's own worked examples
 * come out byte-exact, §1's encapsulation layers wrap/unwrap correctly (including a CRC mismatch
 * refusing the whole command), §6's answer matching and §5's panel answers follow their tables.
 *
 * Requirements: command-classes.md §4.1-§4.25 (build/parse), §1 (encapsulation), §5 (panel
 * answers), §6 (answer matching); ZW-023 (malformed input - the exhaustive form is in
 * test_zw_fuzz.c, this file also checks a few malformed/trailing cases by hand where the shape is
 * easiest to see spelled out).
 */
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_crc16.h"

void setUp(void) {}
void tearDown(void) {}

#define BUF_MAX 400u

/* ================================================================================================
 * §4.1 No Operation, §4.2 Basic
 * ================================================================================================ */

static void test_nop(void)
{
    uint8_t buf[BUF_MAX];
    const size_t n = zw_cc_nop_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(1u, n);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[0]);
    TEST_ASSERT_TRUE(zw_cc_nop_parse(buf, n));
    TEST_ASSERT_TRUE(zw_cc_nop_parse(buf, 2u)); /* a trailing byte is ignored */
    TEST_ASSERT_FALSE(zw_cc_nop_parse(buf, 0u));
    buf[0] = 0x01u;
    TEST_ASSERT_FALSE(zw_cc_nop_parse(buf, 1u)); /* wrong CC */
}

static void test_basic(void)
{
    uint8_t buf[BUF_MAX];

    const zw_cc_basic_set_t set_in = {.value = 0xFFu};
    size_t n = zw_cc_basic_set_build(buf, sizeof(buf), &set_in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    TEST_ASSERT_EQUAL_HEX8(0x20u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[1]);
    zw_cc_basic_set_t set_out;
    TEST_ASSERT_TRUE(zw_cc_basic_set_parse(buf, n, &set_out));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, set_out.value);
    TEST_ASSERT_TRUE(zw_cc_basic_set_parse(buf, n + 1u, &set_out)); /* trailing byte ignored */

    n = zw_cc_basic_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_basic_get_parse(buf, n));

    const zw_cc_basic_report_t rep_v1 = {.current_value = 0x63u, .has_target = false};
    n = zw_cc_basic_report_build(buf, sizeof(buf), &rep_v1);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_basic_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_basic_report_parse(buf, n, &rep_out));
    TEST_ASSERT_FALSE(rep_out.has_target);

    const zw_cc_basic_report_t rep_v2 = {.current_value = 0x00u, .has_target = true, .target_value = 0x63u,
                                          .duration = 0x05u};
    n = zw_cc_basic_report_build(buf, sizeof(buf), &rep_v2);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    TEST_ASSERT_TRUE(zw_cc_basic_report_parse(buf, n, &rep_out));
    TEST_ASSERT_TRUE(rep_out.has_target);
    TEST_ASSERT_EQUAL_HEX8(0x63u, rep_out.target_value);
    TEST_ASSERT_EQUAL_HEX8(0x05u, rep_out.duration);
    TEST_ASSERT_TRUE(zw_cc_basic_report_parse(buf, n + 1u, &rep_out)); /* trailing byte ignored */
    TEST_ASSERT_FALSE(zw_cc_basic_report_parse(buf, 2u, &rep_out));    /* too short: no value byte */
}

/* ================================================================================================
 * §4.3 Binary Sensor
 * ================================================================================================ */

static void test_binary_sensor(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_binary_sensor_supported_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_supported_get_parse(buf, n));

    const uint8_t mask[2] = {0x00u, 0x04u}; /* bit 10 set: byte 1 (bits 8-15), bit 2 of it */
    const zw_cc_binary_sensor_supported_report_t sup_in = {.mask = mask, .mask_len = 2u};
    n = zw_cc_binary_sensor_supported_report_build(buf, sizeof(buf), &sup_in);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    zw_cc_binary_sensor_supported_report_t sup_out;
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_supported_report_parse(buf, n, &sup_out));
    TEST_ASSERT_EQUAL_UINT(2u, sup_out.mask_len);
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_type_supported(sup_out.mask, sup_out.mask_len, ZW_BINARY_SENSOR_TYPE_DOOR_WINDOW));
    TEST_ASSERT_FALSE(zw_cc_binary_sensor_type_supported(sup_out.mask, sup_out.mask_len, ZW_BINARY_SENSOR_TYPE_SMOKE));
    TEST_ASSERT_FALSE(zw_cc_binary_sensor_type_supported(sup_out.mask, sup_out.mask_len, 0u)); /* reserved bit */

    const zw_cc_binary_sensor_get_t get_v1 = {.has_type = false};
    n = zw_cc_binary_sensor_get_build(buf, sizeof(buf), &get_v1);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    zw_cc_binary_sensor_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_get_parse(buf, n, &get_out));
    TEST_ASSERT_FALSE(get_out.has_type);

    const zw_cc_binary_sensor_get_t get_v2 = {.has_type = true, .sensor_type = ZW_BINARY_SENSOR_TYPE_MOTION};
    n = zw_cc_binary_sensor_get_build(buf, sizeof(buf), &get_v2);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_get_parse(buf, n, &get_out));
    TEST_ASSERT_TRUE(get_out.has_type);
    TEST_ASSERT_EQUAL_HEX8(ZW_BINARY_SENSOR_TYPE_MOTION, get_out.sensor_type);

    const zw_cc_binary_sensor_report_t rep_in = {.detected = true, .has_type = true,
                                                  .sensor_type = ZW_BINARY_SENSOR_TYPE_DOOR_WINDOW};
    n = zw_cc_binary_sensor_report_build(buf, sizeof(buf), &rep_in);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    zw_cc_binary_sensor_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_report_parse(buf, n, &rep_out));
    TEST_ASSERT_TRUE(rep_out.detected);
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_report_parse(buf, n + 1u, &rep_out)); /* trailing ignored */
    buf[2] = 0x01u; /* only 00/FF are valid values */
    TEST_ASSERT_FALSE(zw_cc_binary_sensor_report_parse(buf, n, &rep_out));
}

/* ================================================================================================
 * §4.4 Multilevel Sensor
 * ================================================================================================ */

static void test_multilevel_sensor(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_multilevel_sensor_supported_sensor_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_supported_sensor_get_parse(buf, n));

    const uint8_t mask[1] = {0x01u}; /* bit 0 = type 1 (air temperature): one-based, unlike Binary Sensor */
    const zw_cc_multilevel_sensor_supported_sensor_report_t sup_in = {.mask = mask, .mask_len = 1u};
    n = zw_cc_multilevel_sensor_supported_sensor_report_build(buf, sizeof(buf), &sup_in);
    zw_cc_multilevel_sensor_supported_sensor_report_t sup_out;
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_supported_sensor_report_parse(buf, n, &sup_out));
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_type_supported(sup_out.mask, sup_out.mask_len,
                                                              ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE));
    TEST_ASSERT_FALSE(zw_cc_multilevel_sensor_type_supported(sup_out.mask, sup_out.mask_len,
                                                              ZW_MULTILEVEL_SENSOR_TYPE_HUMIDITY));

    const zw_cc_multilevel_sensor_supported_scale_get_t scale_get_in = {
        .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE};
    n = zw_cc_multilevel_sensor_supported_scale_get_build(buf, sizeof(buf), &scale_get_in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_multilevel_sensor_supported_scale_get_t scale_get_out;
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_supported_scale_get_parse(buf, n, &scale_get_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, scale_get_out.sensor_type);

    const zw_cc_multilevel_sensor_supported_scale_report_t scale_rep_in = {
        .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, .scale_mask = 0x03u};
    n = zw_cc_multilevel_sensor_supported_scale_report_build(buf, sizeof(buf), &scale_rep_in);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    zw_cc_multilevel_sensor_supported_scale_report_t scale_rep_out;
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_supported_scale_report_parse(buf, n, &scale_rep_out));
    TEST_ASSERT_EQUAL_HEX8(0x03u, scale_rep_out.scale_mask);

    const zw_cc_multilevel_sensor_get_t get_v1 = {.has_type = false};
    n = zw_cc_multilevel_sensor_get_build(buf, sizeof(buf), &get_v1);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    zw_cc_multilevel_sensor_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_get_parse(buf, n, &get_out));
    TEST_ASSERT_FALSE(get_out.has_type);

    const zw_cc_multilevel_sensor_get_t get_v5 = {.has_type = true,
                                                   .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE,
                                                   .scale = ZW_MULTILEVEL_SENSOR_SCALE_FAHRENHEIT};
    n = zw_cc_multilevel_sensor_get_build(buf, sizeof(buf), &get_v5);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_get_parse(buf, n, &get_out));
    TEST_ASSERT_TRUE(get_out.has_type);
    TEST_ASSERT_EQUAL_UINT8(ZW_MULTILEVEL_SENSOR_SCALE_FAHRENHEIT, get_out.scale);

    /* value = signed integer / 10^precision - the panel decodes as an integer, never float. Here:
     * -12.8 degC as precision 1, size 2: raw -128. */
    const zw_cc_multilevel_sensor_report_t rep_in = {
        .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, .precision = 1u,
        .scale = ZW_MULTILEVEL_SENSOR_SCALE_CELSIUS, .size = 2u, .raw_value = -128,
    };
    n = zw_cc_multilevel_sensor_report_build(buf, sizeof(buf), &rep_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_multilevel_sensor_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_INT32(-128, rep_out.raw_value);
    TEST_ASSERT_EQUAL_UINT8(1u, rep_out.precision);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(buf, n + 1u, &rep_out)); /* trailing ignored */
    TEST_ASSERT_FALSE(zw_cc_multilevel_sensor_report_parse(buf, n - 1u, &rep_out)); /* value truncated */

    /* 1-byte and 4-byte sizes, including negative values, without floating point anywhere. */
    const zw_cc_multilevel_sensor_report_t rep_1b = {.sensor_type = 1u, .precision = 0u, .scale = 0u,
                                                       .size = 1u, .raw_value = -1};
    n = zw_cc_multilevel_sensor_report_build(buf, sizeof(buf), &rep_1b);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, buf[4]);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_INT32(-1, rep_out.raw_value);

    const zw_cc_multilevel_sensor_report_t rep_4b = {.sensor_type = 1u, .precision = 0u, .scale = 0u,
                                                       .size = 4u, .raw_value = -2000000};
    n = zw_cc_multilevel_sensor_report_build(buf, sizeof(buf), &rep_4b);
    TEST_ASSERT_EQUAL_UINT(8u, n);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_INT32(-2000000, rep_out.raw_value);
}

/* ZW-013, ZW-023 (code review ZW23): a Multilevel Sensor Report may carry its value in 3 bytes,
 * signed, as in 1, 2 or 4 (Z-Wave JS's numeric parser takes every width 1..4): +23.5 and -12.5
 * degC, a Fahrenheit reading, the 24-bit limits, the same value in every width, trailing bytes;
 * a truncated 3-byte value is still refused, and Configuration's own widths (1, 2, 4) stay. */
static void test_multilevel_sensor_three_bytes(void)
{
    zw_cc_multilevel_sensor_report_t out;
    /* 31 05 | type 01 | precision 1, scale 0 (degC), size 3 | 00 00 EB = 235: 23.5 degC */
    const uint8_t pos[] = {0x31u, 0x05u, 0x01u, 0x23u, 0x00u, 0x00u, 0xEBu, 0xAAu};
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(pos, 7u, &out));
    TEST_ASSERT_EQUAL_INT32(235, out.raw_value);
    TEST_ASSERT_EQUAL_UINT8(1u, out.precision);
    TEST_ASSERT_EQUAL_UINT8(ZW_MULTILEVEL_SENSOR_SCALE_CELSIUS, out.scale);
    TEST_ASSERT_EQUAL_UINT8(3u, out.size);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(pos, sizeof pos, &out)); /* trailing */
    TEST_ASSERT_EQUAL_INT32(235, out.raw_value);
    TEST_ASSERT_FALSE(zw_cc_multilevel_sensor_report_parse(pos, 6u, &out)); /* truncated */
    TEST_ASSERT_FALSE(zw_cc_multilevel_sensor_report_parse(pos, 4u, &out));
    /* FF FF 83 = -125: -12.5 degC */
    const uint8_t neg[] = {0x31u, 0x05u, 0x01u, 0x23u, 0xFFu, 0xFFu, 0x83u};
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(neg, sizeof neg, &out));
    TEST_ASSERT_EQUAL_INT32(-125, out.raw_value);
    /* scale 1 (degF), precision 1: 00 02 E7 = 743, 74.3 degF */
    const uint8_t fahr[] = {0x31u, 0x05u, 0x01u, 0x2Bu, 0x00u, 0x02u, 0xE7u};
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(fahr, sizeof fahr, &out));
    TEST_ASSERT_EQUAL_INT32(743, out.raw_value);
    TEST_ASSERT_EQUAL_UINT8(ZW_MULTILEVEL_SENSOR_SCALE_FAHRENHEIT, out.scale);
    /* the 24-bit limits, sign-extended into int32_t */
    const uint8_t max[] = {0x31u, 0x05u, 0x01u, 0x03u, 0x7Fu, 0xFFu, 0xFFu};
    const uint8_t min[] = {0x31u, 0x05u, 0x01u, 0x03u, 0x80u, 0x00u, 0x00u};
    const uint8_t minus_one[] = {0x31u, 0x05u, 0x01u, 0x03u, 0xFFu, 0xFFu, 0xFFu};
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(max, sizeof max, &out));
    TEST_ASSERT_EQUAL_INT32(8388607, out.raw_value);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(min, sizeof min, &out));
    TEST_ASSERT_EQUAL_INT32(-8388608, out.raw_value);
    TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(minus_one, sizeof minus_one, &out));
    TEST_ASSERT_EQUAL_INT32(-1, out.raw_value);
    /* the same value in every width reads the same; the builder writes 3 bytes too */
    uint8_t buf[BUF_MAX];
    for (uint8_t size = 1u; size <= 4u; size++) {
        const zw_cc_multilevel_sensor_report_t in = {
            .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, .precision = 1u,
            .scale = ZW_MULTILEVEL_SENSOR_SCALE_CELSIUS, .size = size, .raw_value = -125};
        const size_t n = zw_cc_multilevel_sensor_report_build(buf, sizeof(buf), &in);
        TEST_ASSERT_EQUAL_UINT(4u + size, n);
        TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(buf, n, &out));
        TEST_ASSERT_EQUAL_INT32(-125, out.raw_value);
        TEST_ASSERT_EQUAL_UINT8(size, out.size);
        if (size == 3u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(neg, buf, sizeof neg);
        }
    }
    const zw_cc_multilevel_sensor_report_t too_big = {.sensor_type = 1u, .size = 3u,
                                                      .raw_value = 8388608};
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_multilevel_sensor_report_build(buf, sizeof(buf), &too_big));
    /* sizes 0, 5, 6 and 7 are still refused */
    uint8_t odd[] = {0x31u, 0x05u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t bad_sizes[] = {0u, 5u, 6u, 7u};
    for (size_t i = 0; i < sizeof bad_sizes; i++) {
        odd[3] = bad_sizes[i];
        TEST_ASSERT_FALSE(zw_cc_multilevel_sensor_report_parse(odd, sizeof odd, &out));
    }
    /* Configuration keeps its 1, 2 or 4 */
    const uint8_t cfg[] = {0x70u, 0x06u, 0x01u, 0x03u, 0x00u, 0x00u, 0x01u};
    zw_cc_configuration_report_t cfg_out;
    TEST_ASSERT_FALSE(zw_cc_configuration_report_parse(cfg, sizeof cfg, &cfg_out));
    const zw_cc_configuration_set_t cfg_set = {.parameter = 1u, .size = 3u, .value = 1};
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_configuration_set_build(buf, sizeof(buf), &cfg_set));
}

/* ================================================================================================
 * §4.6 Battery
 * ================================================================================================ */

static void test_battery(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_battery_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_battery_get_parse(buf, n));

    const zw_cc_battery_report_t rep_v1 = {.level = 75u, .has_v2 = false};
    n = zw_cc_battery_report_build(buf, sizeof(buf), &rep_v1);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_battery_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_battery_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_UINT8(75u, rep_out.level);
    TEST_ASSERT_FALSE(rep_out.has_v2);

    const zw_cc_battery_report_t rep_low = {.level = ZW_BATTERY_LOW, .has_v2 = false};
    n = zw_cc_battery_report_build(buf, sizeof(buf), &rep_low);
    TEST_ASSERT_TRUE(zw_cc_battery_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_BATTERY_LOW, rep_out.level);

    const zw_cc_battery_report_t rep_bad = {.level = 0x65u}; /* > 64 and not FF: invalid */
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_battery_report_build(buf, sizeof(buf), &rep_bad));
    buf[0] = (uint8_t)ZW_CC_BATTERY;
    buf[1] = ZW_CMD_BATTERY_REPORT;
    buf[2] = 0x65u;
    TEST_ASSERT_FALSE(zw_cc_battery_report_parse(buf, 3u, &rep_out));

    const zw_cc_battery_report_t rep_v2 = {
        .level = 50u, .has_v2 = true, .charging_state = 1u, .rechargeable = true, .is_backup = false,
        .overheating = true, .low_fluid = false, .replace_or_recharge = 1u, .low_temperature = true,
        .disconnected = false,
    };
    n = zw_cc_battery_report_build(buf, sizeof(buf), &rep_v2);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    TEST_ASSERT_TRUE(zw_cc_battery_report_parse(buf, n, &rep_out));
    TEST_ASSERT_TRUE(rep_out.has_v2);
    TEST_ASSERT_EQUAL_UINT8(1u, rep_out.charging_state);
    TEST_ASSERT_TRUE(rep_out.rechargeable);
    TEST_ASSERT_TRUE(rep_out.overheating);
    TEST_ASSERT_EQUAL_UINT8(1u, rep_out.replace_or_recharge);
    TEST_ASSERT_TRUE(rep_out.low_temperature);
    TEST_ASSERT_FALSE(rep_out.disconnected);
    TEST_ASSERT_TRUE(zw_cc_battery_report_parse(buf, n + 1u, &rep_out)); /* trailing ignored */
}

/* ================================================================================================
 * §4.5 Notification
 * ================================================================================================ */

static void test_notification(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_notification_supported_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_notification_supported_get_parse(buf, n));

    const uint8_t tmask[1] = {(uint8_t)(1u << ZW_NOTIFICATION_TYPE_HOME_SECURITY)};
    const zw_cc_notification_supported_report_t sup_in = {.sends_v1_alarms_too = true, .type_mask = tmask,
                                                            .type_mask_len = 1u};
    n = zw_cc_notification_supported_report_build(buf, sizeof(buf), &sup_in);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    zw_cc_notification_supported_report_t sup_out;
    TEST_ASSERT_TRUE(zw_cc_notification_supported_report_parse(buf, n, &sup_out));
    TEST_ASSERT_TRUE(sup_out.sends_v1_alarms_too);
    TEST_ASSERT_TRUE(zw_cc_notification_type_supported(sup_out.type_mask, sup_out.type_mask_len,
                                                         ZW_NOTIFICATION_TYPE_HOME_SECURITY));
    TEST_ASSERT_FALSE(zw_cc_notification_type_supported(sup_out.type_mask, sup_out.type_mask_len,
                                                          ZW_NOTIFICATION_TYPE_SMOKE));
    /* the length is declared in the flags byte, not just "whatever follows": a trailing byte past
     * the declared mask length is extra, not part of the mask. */
    buf[n] = 0xFFu;
    TEST_ASSERT_TRUE(zw_cc_notification_supported_report_parse(buf, n + 1u, &sup_out));
    TEST_ASSERT_EQUAL_UINT(1u, sup_out.type_mask_len);

    const zw_cc_notification_event_supported_get_t evget_in = {.notification_type = ZW_NOTIFICATION_TYPE_SMOKE};
    n = zw_cc_notification_event_supported_get_build(buf, sizeof(buf), &evget_in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_notification_event_supported_get_t evget_out;
    TEST_ASSERT_TRUE(zw_cc_notification_event_supported_get_parse(buf, n, &evget_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_NOTIFICATION_TYPE_SMOKE, evget_out.notification_type);

    const uint8_t emask[1] = {0x06u};
    const zw_cc_notification_event_supported_report_t evrep_in = {
        .notification_type = ZW_NOTIFICATION_TYPE_SMOKE, .event_mask = emask, .event_mask_len = 1u};
    n = zw_cc_notification_event_supported_report_build(buf, sizeof(buf), &evrep_in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_cc_notification_event_supported_report_t evrep_out;
    TEST_ASSERT_TRUE(zw_cc_notification_event_supported_report_parse(buf, n, &evrep_out));
    TEST_ASSERT_EQUAL_UINT(1u, evrep_out.event_mask_len);

    /* the panel always asks 00 . type . 00 */
    const zw_cc_notification_get_t get_in = {.v1_alarm_type = 0x00u,
                                              .notification_type = ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                              .has_event = true, .event = 0x00u};
    n = zw_cc_notification_get_build(buf, sizeof(buf), &get_in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_cc_notification_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_notification_get_parse(buf, n, &get_out));
    TEST_ASSERT_EQUAL_HEX8(0x00u, get_out.v1_alarm_type);
    TEST_ASSERT_TRUE(get_out.has_event);

    const zw_cc_notification_set_t set_in = {.notification_type = ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                              .status = ZW_NOTIFICATION_STATUS_ON};
    n = zw_cc_notification_set_build(buf, sizeof(buf), &set_in);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    zw_cc_notification_set_t set_out;
    TEST_ASSERT_TRUE(zw_cc_notification_set_parse(buf, n, &set_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_NOTIFICATION_STATUS_ON, set_out.status);

    /* Report: a v1 alarm is anything shorter than 7 payload bytes (2 header + 2 = 4 total). */
    const zw_cc_notification_report_t rep_v1 = {.is_v1 = true, .v1_alarm_type = 0x05u, .v1_alarm_level = 0x63u};
    n = zw_cc_notification_report_build(buf, sizeof(buf), &rep_v1);
    TEST_ASSERT_EQUAL_UINT(4u, n);
    zw_cc_notification_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_notification_report_parse(buf, n, &rep_out));
    TEST_ASSERT_TRUE(rep_out.is_v1);
    TEST_ASSERT_EQUAL_HEX8(0x05u, rep_out.v1_alarm_type);
    TEST_ASSERT_EQUAL_HEX8(0x63u, rep_out.v1_alarm_level);

    /* Full v2+ layout, with event parameters and a sequence number: offsets exactly as in the
     * doc's table (0 v1 type, 1 v1 level, 2 reserved, 3 status, 4 type, 5 event, 6 flags,
     * 7..7+k-1 params, 7+k sequence). */
    const uint8_t params[2] = {0x01u, 0x00u}; /* Access Control event 16, "regular" (not tilted) */
    const zw_cc_notification_report_t rep_v2 = {
        .is_v1 = false, .v1_alarm_type = 0u, .v1_alarm_level = 0u, .status = ZW_NOTIFICATION_STATUS_ON,
        .notification_type = ZW_NOTIFICATION_TYPE_ACCESS_CONTROL, .event = 0x16u, .event_params = params,
        .event_params_len = 2u, .has_sequence_number = true, .sequence_number = 0x2Au,
    };
    n = zw_cc_notification_report_build(buf, sizeof(buf), &rep_v2);
    TEST_ASSERT_EQUAL_UINT(12u, n); /* 2 header + 7 + 2 params + 1 sequence number */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)ZW_CC_NOTIFICATION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_CMD_NOTIFICATION_REPORT, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[4]); /* reserved */
    TEST_ASSERT_EQUAL_HEX8(0x16u, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0x82u, buf[8]); /* bit 7 (has sequence) | length 2 */
    TEST_ASSERT_EQUAL_HEX8(0x2Au, buf[11]);
    TEST_ASSERT_TRUE(zw_cc_notification_report_parse(buf, n, &rep_out));
    TEST_ASSERT_FALSE(rep_out.is_v1);
    TEST_ASSERT_EQUAL_HEX8(ZW_NOTIFICATION_TYPE_ACCESS_CONTROL, rep_out.notification_type);
    TEST_ASSERT_EQUAL_HEX8(0x16u, rep_out.event);
    TEST_ASSERT_EQUAL_UINT(2u, rep_out.event_params_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(params, rep_out.event_params, 2u);
    TEST_ASSERT_TRUE(rep_out.has_sequence_number);
    TEST_ASSERT_EQUAL_HEX8(0x2Au, rep_out.sequence_number);
    TEST_ASSERT_TRUE(zw_cc_notification_report_parse(buf, n + 1u, &rep_out)); /* trailing ignored */
    TEST_ASSERT_FALSE(zw_cc_notification_report_parse(buf, n - 1u, &rep_out)); /* sequence number missing */
}

/* ================================================================================================
 * §4.14 Supervision
 * ================================================================================================ */

static void test_supervision(void)
{
    uint8_t buf[BUF_MAX];
    const uint8_t inner[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET};

    const zw_cc_supervision_get_t get_in = {.status_updates_wanted = true, .session_id = 0x3Fu,
                                             .command = inner, .command_len = 2u};
    size_t n = zw_cc_supervision_get_build(buf, sizeof(buf), &get_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    TEST_ASSERT_EQUAL_HEX8(0xBFu, buf[2]); /* bit 7 set | session 0x3F */
    zw_cc_supervision_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_supervision_get_parse(buf, n, &get_out));
    TEST_ASSERT_TRUE(get_out.status_updates_wanted);
    TEST_ASSERT_EQUAL_HEX8(0x3Fu, get_out.session_id);
    TEST_ASSERT_EQUAL_UINT(2u, get_out.command_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, get_out.command, 2u);
    TEST_ASSERT_TRUE(zw_cc_supervision_get_parse(buf, n + 1u, &get_out)); /* trailing byte ignored */
    TEST_ASSERT_FALSE(zw_cc_supervision_get_parse(buf, n - 1u, &get_out)); /* declared length too long */

    const zw_cc_supervision_report_t rep_in = {.more_updates_follow = false, .wake_up_request = true,
                                                .session_id = 0x01u, .status = ZW_SUPERVISION_STATUS_SUCCESS,
                                                .duration = 0u};
    n = zw_cc_supervision_report_build(buf, sizeof(buf), &rep_in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_cc_supervision_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_supervision_report_parse(buf, n, &rep_out));
    TEST_ASSERT_FALSE(rep_out.more_updates_follow);
    TEST_ASSERT_TRUE(rep_out.wake_up_request);
    TEST_ASSERT_EQUAL_HEX8(ZW_SUPERVISION_STATUS_SUCCESS, rep_out.status);
    TEST_ASSERT_TRUE(zw_cc_supervision_report_parse(buf, n + 1u, &rep_out)); /* trailing ignored */
}

/* ================================================================================================
 * §4.15 Multi Channel
 * ================================================================================================ */

static void test_multi_channel(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_multi_channel_end_point_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_multi_channel_end_point_get_parse(buf, n));

    const zw_cc_multi_channel_end_point_report_t ep_in = {.is_dynamic = false, .identical_capabilities = true,
                                                            .individual_count = 4u, .has_aggregated_count = true,
                                                            .aggregated_count = 1u};
    n = zw_cc_multi_channel_end_point_report_build(buf, sizeof(buf), &ep_in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_cc_multi_channel_end_point_report_t ep_out;
    TEST_ASSERT_TRUE(zw_cc_multi_channel_end_point_report_parse(buf, n, &ep_out));
    TEST_ASSERT_TRUE(ep_out.identical_capabilities);
    TEST_ASSERT_EQUAL_UINT8(4u, ep_out.individual_count);
    TEST_ASSERT_TRUE(ep_out.has_aggregated_count);
    TEST_ASSERT_EQUAL_UINT8(1u, ep_out.aggregated_count);
    TEST_ASSERT_TRUE(zw_cc_multi_channel_end_point_report_parse(buf, n - 1u, &ep_out)); /* v1: no aggregated */
    TEST_ASSERT_FALSE(ep_out.has_aggregated_count);

    const zw_cc_multi_channel_capability_get_t cap_get_in = {.endpoint = 2u};
    n = zw_cc_multi_channel_capability_get_build(buf, sizeof(buf), &cap_get_in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_multi_channel_capability_get_t cap_get_out;
    TEST_ASSERT_TRUE(zw_cc_multi_channel_capability_get_parse(buf, n, &cap_get_out));
    TEST_ASSERT_EQUAL_UINT8(2u, cap_get_out.endpoint);

    const uint8_t cclist[3] = {(uint8_t)ZW_CC_BINARY_SENSOR, (uint8_t)ZW_CC_NOTIFICATION,
                               (uint8_t)ZW_CC_BATTERY};
    const zw_cc_multi_channel_capability_report_t cap_rep_in = {
        .is_dynamic = true, .endpoint = 2u, .generic_class = 0x21u, .specific_class = 0x01u,
        .cc_list = cclist, .cc_list_len = 3u,
    };
    n = zw_cc_multi_channel_capability_report_build(buf, sizeof(buf), &cap_rep_in);
    TEST_ASSERT_EQUAL_UINT(8u, n);
    zw_cc_multi_channel_capability_report_t cap_rep_out;
    TEST_ASSERT_TRUE(zw_cc_multi_channel_capability_report_parse(buf, n, &cap_rep_out));
    TEST_ASSERT_TRUE(cap_rep_out.is_dynamic);
    TEST_ASSERT_EQUAL_UINT8(2u, cap_rep_out.endpoint);
    TEST_ASSERT_EQUAL_UINT(3u, cap_rep_out.cc_list_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(cclist, cap_rep_out.cc_list, 3u);

    const uint8_t inner[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET};
    const zw_cc_multi_channel_command_encapsulation_t enc_in = {
        .source_endpoint = 0u, .destination_is_mask = false, .destination = 3u, .command = inner,
        .command_len = 2u,
    };
    n = zw_cc_multi_channel_command_encapsulation_build(buf, sizeof(buf), &enc_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_multi_channel_command_encapsulation_t enc_out;
    TEST_ASSERT_TRUE(zw_cc_multi_channel_command_encapsulation_parse(buf, n, &enc_out));
    TEST_ASSERT_EQUAL_UINT8(3u, enc_out.destination);
    TEST_ASSERT_FALSE(enc_out.destination_is_mask);
    TEST_ASSERT_EQUAL_UINT(2u, enc_out.command_len);
}

/* ================================================================================================
 * §4.7 Wake Up
 * ================================================================================================ */

static void test_wake_up(void)
{
    uint8_t buf[BUF_MAX];

    const zw_cc_wake_up_interval_set_t set_in = {.interval_seconds = 3600u, .node_id_to_notify = 1u};
    size_t n = zw_cc_wake_up_interval_set_build(buf, sizeof(buf), &set_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_wake_up_interval_set_t set_out;
    TEST_ASSERT_TRUE(zw_cc_wake_up_interval_set_parse(buf, n, &set_out));
    TEST_ASSERT_EQUAL_UINT32(3600u, set_out.interval_seconds);
    TEST_ASSERT_EQUAL_UINT8(1u, set_out.node_id_to_notify);
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_wake_up_interval_set_build(buf, sizeof(buf),
                                                                 &(zw_cc_wake_up_interval_set_t){
                                                                     .interval_seconds = 0x1000000u}));

    n = zw_cc_wake_up_interval_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_wake_up_interval_get_parse(buf, n));

    const zw_cc_wake_up_interval_report_t rep_in = {.interval_seconds = 7200u, .node_id = 1u};
    n = zw_cc_wake_up_interval_report_build(buf, sizeof(buf), &rep_in);
    zw_cc_wake_up_interval_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_wake_up_interval_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_UINT32(7200u, rep_out.interval_seconds);

    n = zw_cc_wake_up_notification_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_wake_up_notification_parse(buf, n));
    n = zw_cc_wake_up_no_more_information_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_wake_up_no_more_information_parse(buf, n));
    n = zw_cc_wake_up_interval_capabilities_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_wake_up_interval_capabilities_get_parse(buf, n));

    const zw_cc_wake_up_interval_capabilities_report_t cap_v2 = {
        .min_seconds = 30u, .max_seconds = 86400u, .default_seconds = 3600u, .step_seconds = 30u,
        .has_v3 = false,
    };
    n = zw_cc_wake_up_interval_capabilities_report_build(buf, sizeof(buf), &cap_v2);
    TEST_ASSERT_EQUAL_UINT(14u, n);
    zw_cc_wake_up_interval_capabilities_report_t cap_out;
    TEST_ASSERT_TRUE(zw_cc_wake_up_interval_capabilities_report_parse(buf, n, &cap_out));
    TEST_ASSERT_EQUAL_UINT32(86400u, cap_out.max_seconds);
    TEST_ASSERT_FALSE(cap_out.has_v3);

    const zw_cc_wake_up_interval_capabilities_report_t cap_v3 = {
        .min_seconds = 30u, .max_seconds = 86400u, .default_seconds = 3600u, .step_seconds = 30u,
        .has_v3 = true, .wake_up_on_demand = true,
    };
    n = zw_cc_wake_up_interval_capabilities_report_build(buf, sizeof(buf), &cap_v3);
    TEST_ASSERT_EQUAL_UINT(15u, n);
    TEST_ASSERT_TRUE(zw_cc_wake_up_interval_capabilities_report_parse(buf, n, &cap_out));
    TEST_ASSERT_TRUE(cap_out.has_v3);
    TEST_ASSERT_TRUE(cap_out.wake_up_on_demand);
}

/* ================================================================================================
 * §4.8 Association
 * ================================================================================================ */

static void test_association(void)
{
    uint8_t buf[BUF_MAX];
    const uint8_t nodes[3] = {2u, 3u, 4u};

    const zw_cc_association_set_t set_in = {.group = 1u, .node_ids = nodes, .node_id_count = 3u};
    size_t n = zw_cc_association_set_build(buf, sizeof(buf), &set_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_association_set_t set_out;
    TEST_ASSERT_TRUE(zw_cc_association_set_parse(buf, n, &set_out));
    TEST_ASSERT_EQUAL_UINT(3u, set_out.node_id_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(nodes, set_out.node_ids, 3u);

    const zw_cc_association_get_t get_in = {.group = ZW_ASSOCIATION_LIFELINE_GROUP};
    n = zw_cc_association_get_build(buf, sizeof(buf), &get_in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_association_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_association_get_parse(buf, n, &get_out));
    TEST_ASSERT_EQUAL_UINT8(1u, get_out.group);

    const zw_cc_association_report_t rep_in = {.group = 1u, .max_nodes = 5u, .reports_to_follow = 0u,
                                                .node_ids = nodes, .node_id_count = 3u};
    n = zw_cc_association_report_build(buf, sizeof(buf), &rep_in);
    TEST_ASSERT_EQUAL_UINT(8u, n);
    zw_cc_association_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_association_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_UINT8(5u, rep_out.max_nodes);
    TEST_ASSERT_EQUAL_UINT(3u, rep_out.node_id_count);

    const zw_cc_association_remove_t rem_all = {.group = 1u, .node_ids = NULL, .node_id_count = 0u};
    n = zw_cc_association_remove_build(buf, sizeof(buf), &rem_all);
    TEST_ASSERT_EQUAL_UINT(3u, n); /* no node IDs: remove all */
    zw_cc_association_remove_t rem_out;
    TEST_ASSERT_TRUE(zw_cc_association_remove_parse(buf, n, &rem_out));
    TEST_ASSERT_EQUAL_UINT(0u, rem_out.node_id_count);

    n = zw_cc_association_supported_groupings_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_association_supported_groupings_get_parse(buf, n));
    const zw_cc_association_supported_groupings_report_t sg_in = {.group_count = 1u};
    n = zw_cc_association_supported_groupings_report_build(buf, sizeof(buf), &sg_in);
    zw_cc_association_supported_groupings_report_t sg_out;
    TEST_ASSERT_TRUE(zw_cc_association_supported_groupings_report_parse(buf, n, &sg_out));
    TEST_ASSERT_EQUAL_UINT8(1u, sg_out.group_count);

    n = zw_cc_association_specific_group_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_association_specific_group_get_parse(buf, n));
    const zw_cc_association_specific_group_report_t spg_in = {.group = 0u};
    n = zw_cc_association_specific_group_report_build(buf, sizeof(buf), &spg_in);
    zw_cc_association_specific_group_report_t spg_out;
    TEST_ASSERT_TRUE(zw_cc_association_specific_group_report_parse(buf, n, &spg_out));
    TEST_ASSERT_EQUAL_UINT8(0u, spg_out.group);
}

/* ================================================================================================
 * §4.9 Multi Channel Association (same command IDs as Association, its own CC)
 * ================================================================================================ */

static void test_mc_association(void)
{
    uint8_t buf[BUF_MAX];
    /* node 5, node 9, marker, (3, endpoint 1), (4, mask covering endpoint 2) */
    const uint8_t dest[7] = {5u, 9u, 0x00u, 3u, 0x01u, 4u, 0x82u};

    const zw_cc_mc_association_set_t set_in = {.group = 1u, .destinations = dest, .destinations_len = 7u};
    size_t n = zw_cc_mc_association_set_build(buf, sizeof(buf), &set_in);
    TEST_ASSERT_EQUAL_UINT(10u, n);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_CMD_ASSOCIATION_SET, buf[1]);
    zw_cc_mc_association_set_t set_out;
    TEST_ASSERT_TRUE(zw_cc_mc_association_set_parse(buf, n, &set_out));
    TEST_ASSERT_EQUAL_UINT(7u, set_out.destinations_len);

    zw_cc_mc_dest_iter_t it;
    zw_cc_mc_dest_iter_init(&it, set_out.destinations, set_out.destinations_len);
    zw_cc_mc_destination_t d;
    TEST_ASSERT_TRUE(zw_cc_mc_dest_iter_next(&it, &d));
    TEST_ASSERT_FALSE(d.is_endpoint);
    TEST_ASSERT_EQUAL_UINT16(5u, d.node_id);
    TEST_ASSERT_TRUE(zw_cc_mc_dest_iter_next(&it, &d));
    TEST_ASSERT_FALSE(d.is_endpoint);
    TEST_ASSERT_EQUAL_UINT16(9u, d.node_id);
    TEST_ASSERT_TRUE(zw_cc_mc_dest_iter_next(&it, &d));
    TEST_ASSERT_TRUE(d.is_endpoint);
    TEST_ASSERT_FALSE(d.is_mask);
    TEST_ASSERT_EQUAL_UINT16(3u, d.node_id);
    TEST_ASSERT_EQUAL_UINT8(1u, d.endpoint);
    TEST_ASSERT_TRUE(zw_cc_mc_dest_iter_next(&it, &d));
    TEST_ASSERT_TRUE(d.is_mask);
    TEST_ASSERT_EQUAL_UINT16(4u, d.node_id);
    TEST_ASSERT_EQUAL_UINT8(0x02u, d.endpoint_mask);
    TEST_ASSERT_FALSE(zw_cc_mc_dest_iter_next(&it, &d)); /* end of list */

    /* an iterator on a stray trailing byte after the marker (an odd remainder) stops cleanly */
    const uint8_t odd[2] = {0x00u, 0x07u};
    zw_cc_mc_dest_iter_init(&it, odd, sizeof(odd));
    TEST_ASSERT_FALSE(zw_cc_mc_dest_iter_next(&it, &d));

    const zw_cc_mc_association_get_t get_in = {.group = 1u};
    n = zw_cc_mc_association_get_build(buf, sizeof(buf), &get_in);
    zw_cc_mc_association_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_mc_association_get_parse(buf, n, &get_out));
    TEST_ASSERT_EQUAL_UINT8(1u, get_out.group);

    const zw_cc_mc_association_report_t rep_in = {.group = 1u, .max_nodes = 5u, .reports_to_follow = 0u,
                                                   .destinations = dest, .destinations_len = 7u};
    n = zw_cc_mc_association_report_build(buf, sizeof(buf), &rep_in);
    zw_cc_mc_association_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_mc_association_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_UINT(7u, rep_out.destinations_len);

    const zw_cc_mc_association_remove_t rem_in = {.group = 1u, .destinations = dest, .destinations_len = 7u};
    n = zw_cc_mc_association_remove_build(buf, sizeof(buf), &rem_in);
    zw_cc_mc_association_remove_t rem_out;
    TEST_ASSERT_TRUE(zw_cc_mc_association_remove_parse(buf, n, &rem_out));
    TEST_ASSERT_EQUAL_UINT(7u, rem_out.destinations_len);

    n = zw_cc_mc_association_supported_groupings_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_mc_association_supported_groupings_get_parse(buf, n));
    const zw_cc_mc_association_supported_groupings_report_t sg_in = {.group_count = 1u};
    n = zw_cc_mc_association_supported_groupings_report_build(buf, sizeof(buf), &sg_in);
    zw_cc_mc_association_supported_groupings_report_t sg_out;
    TEST_ASSERT_TRUE(zw_cc_mc_association_supported_groupings_report_parse(buf, n, &sg_out));
    TEST_ASSERT_EQUAL_UINT8(1u, sg_out.group_count);
}

/* ================================================================================================
 * §4.10 Association Group Information
 * ================================================================================================ */

static void test_agi(void)
{
    uint8_t buf[BUF_MAX];

    const zw_cc_agi_name_get_t name_get_in = {.group = 1u};
    size_t n = zw_cc_agi_name_get_build(buf, sizeof(buf), &name_get_in);
    zw_cc_agi_name_get_t name_get_out;
    TEST_ASSERT_TRUE(zw_cc_agi_name_get_parse(buf, n, &name_get_out));
    TEST_ASSERT_EQUAL_UINT8(1u, name_get_out.group);

    static const char lifeline[] = "Lifeline";
    const zw_cc_agi_name_report_t name_rep_in = {.group = 1u, .name = lifeline,
                                                  .name_len = sizeof(lifeline) - 1u};
    n = zw_cc_agi_name_report_build(buf, sizeof(buf), &name_rep_in);
    TEST_ASSERT_EQUAL_UINT(12u, n);
    zw_cc_agi_name_report_t name_rep_out;
    TEST_ASSERT_TRUE(zw_cc_agi_name_report_parse(buf, n, &name_rep_out));
    TEST_ASSERT_EQUAL_UINT(8u, name_rep_out.name_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(lifeline, name_rep_out.name, 8u);

    const zw_cc_agi_info_get_t info_get_in = {.refresh_cache = false, .list_mode = true, .group = 0u};
    n = zw_cc_agi_info_get_build(buf, sizeof(buf), &info_get_in);
    zw_cc_agi_info_get_t info_get_out;
    TEST_ASSERT_TRUE(zw_cc_agi_info_get_parse(buf, n, &info_get_out));
    TEST_ASSERT_TRUE(info_get_out.list_mode);

    uint8_t groups[7];
    groups[0] = 1u;
    groups[1] = 0u;
    groups[2] = (uint8_t)(ZW_AGI_PROFILE_GENERAL_LIFELINE >> 8);
    groups[3] = (uint8_t)ZW_AGI_PROFILE_GENERAL_LIFELINE;
    groups[4] = 0u;
    groups[5] = 0u;
    groups[6] = 0u;
    const zw_cc_agi_info_report_t info_rep_in = {.list_mode = false, .dynamic = false, .count = 1u,
                                                  .groups = groups, .groups_len = 7u};
    n = zw_cc_agi_info_report_build(buf, sizeof(buf), &info_rep_in);
    TEST_ASSERT_EQUAL_UINT(10u, n);
    zw_cc_agi_info_report_t info_rep_out;
    TEST_ASSERT_TRUE(zw_cc_agi_info_report_parse(buf, n, &info_rep_out));
    TEST_ASSERT_EQUAL_UINT8(1u, info_rep_out.count);
    zw_cc_agi_group_info_t g0;
    TEST_ASSERT_TRUE(zw_cc_agi_group_info_get(info_rep_out.groups, info_rep_out.groups_len, 0u, &g0));
    TEST_ASSERT_EQUAL_UINT8(1u, g0.group);
    TEST_ASSERT_EQUAL_UINT16(ZW_AGI_PROFILE_GENERAL_LIFELINE, g0.profile);
    TEST_ASSERT_FALSE(zw_cc_agi_group_info_get(info_rep_out.groups, info_rep_out.groups_len, 1u, &g0));

    const zw_cc_agi_command_list_get_t cl_get_in = {.flags = 0u, .group = 1u};
    n = zw_cc_agi_command_list_get_build(buf, sizeof(buf), &cl_get_in);
    zw_cc_agi_command_list_get_t cl_get_out;
    TEST_ASSERT_TRUE(zw_cc_agi_command_list_get_parse(buf, n, &cl_get_out));
    TEST_ASSERT_EQUAL_UINT8(1u, cl_get_out.group);

    const uint8_t pairs[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_SET};
    const zw_cc_agi_command_list_report_t cl_rep_in = {.group = 1u, .cc_command_pairs = pairs,
                                                        .cc_command_pairs_len = 2u};
    n = zw_cc_agi_command_list_report_build(buf, sizeof(buf), &cl_rep_in);
    zw_cc_agi_command_list_report_t cl_rep_out;
    TEST_ASSERT_TRUE(zw_cc_agi_command_list_report_parse(buf, n, &cl_rep_out));
    TEST_ASSERT_EQUAL_UINT(2u, cl_rep_out.cc_command_pairs_len);
}

/* ================================================================================================
 * §4.11 Version
 * ================================================================================================ */

static void test_version(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_version_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_version_get_parse(buf, n));

    const zw_cc_version_report_t rep_v1 = {.library_type = ZW_VERSION_LIBRARY_STATIC_CONTROLLER,
                                            .protocol_major = 7u, .protocol_minor = 18u,
                                            .firmware0_major = 1u, .firmware0_minor = 0u, .has_v2 = false};
    n = zw_cc_version_report_build(buf, sizeof(buf), &rep_v1);
    TEST_ASSERT_EQUAL_UINT(7u, n);
    zw_cc_version_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_version_report_parse(buf, n, &rep_out));
    TEST_ASSERT_FALSE(rep_out.has_v2);
    TEST_ASSERT_EQUAL_UINT8(7u, rep_out.protocol_major);

    const uint8_t targets[2] = {2u, 1u};
    const zw_cc_version_report_t rep_v2 = {.library_type = 1u, .protocol_major = 7u, .protocol_minor = 18u,
                                            .firmware0_major = 1u, .firmware0_minor = 0u, .has_v2 = true,
                                            .hardware_version = 1u, .other_targets = targets,
                                            .other_target_count = 1u};
    n = zw_cc_version_report_build(buf, sizeof(buf), &rep_v2);
    TEST_ASSERT_EQUAL_UINT(11u, n);
    TEST_ASSERT_TRUE(zw_cc_version_report_parse(buf, n, &rep_out));
    TEST_ASSERT_TRUE(rep_out.has_v2);
    TEST_ASSERT_EQUAL_UINT8(1u, rep_out.hardware_version);
    TEST_ASSERT_EQUAL_UINT8(1u, rep_out.other_target_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(targets, rep_out.other_targets, 2u);
    TEST_ASSERT_TRUE(zw_cc_version_report_parse(buf, n + 1u, &rep_out)); /* trailing ignored */
    TEST_ASSERT_FALSE(zw_cc_version_report_parse(buf, n - 1u, &rep_out)); /* declared target missing */

    const zw_cc_version_command_class_get_t cc_get_in = {.cc = (uint8_t)ZW_CC_NOTIFICATION};
    n = zw_cc_version_command_class_get_build(buf, sizeof(buf), &cc_get_in);
    zw_cc_version_command_class_get_t cc_get_out;
    TEST_ASSERT_TRUE(zw_cc_version_command_class_get_parse(buf, n, &cc_get_out));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)ZW_CC_NOTIFICATION, cc_get_out.cc);

    const zw_cc_version_command_class_report_t cc_rep_in = {.cc = (uint8_t)ZW_CC_NOTIFICATION, .version = 8u};
    n = zw_cc_version_command_class_report_build(buf, sizeof(buf), &cc_rep_in);
    zw_cc_version_command_class_report_t cc_rep_out;
    TEST_ASSERT_TRUE(zw_cc_version_command_class_report_parse(buf, n, &cc_rep_out));
    TEST_ASSERT_EQUAL_UINT8(8u, cc_rep_out.version);

    n = zw_cc_version_capabilities_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_version_capabilities_get_parse(buf, n));
    const zw_cc_version_capabilities_report_t caps_in = {.supports_version_command = true,
                                                          .supports_command_class_command = true,
                                                          .supports_zwave_software_get = false};
    n = zw_cc_version_capabilities_report_build(buf, sizeof(buf), &caps_in);
    zw_cc_version_capabilities_report_t caps_out;
    TEST_ASSERT_TRUE(zw_cc_version_capabilities_report_parse(buf, n, &caps_out));
    TEST_ASSERT_TRUE(caps_out.supports_version_command);
    TEST_ASSERT_TRUE(caps_out.supports_command_class_command);
    TEST_ASSERT_FALSE(caps_out.supports_zwave_software_get);
}

/* ================================================================================================
 * §4.12 Manufacturer Specific, §4.13 Z-Wave Plus Info
 * ================================================================================================ */

static void test_manufacturer_specific_and_zwave_plus_info(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_manufacturer_specific_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_manufacturer_specific_get_parse(buf, n));
    const zw_cc_manufacturer_specific_report_t ms_in = {.manufacturer_id = 0xFFFFu, .product_type = 0xFFFFu,
                                                         .product_id = 0xFFFFu};
    n = zw_cc_manufacturer_specific_report_build(buf, sizeof(buf), &ms_in);
    TEST_ASSERT_EQUAL_UINT(8u, n);
    zw_cc_manufacturer_specific_report_t ms_out;
    TEST_ASSERT_TRUE(zw_cc_manufacturer_specific_report_parse(buf, n, &ms_out));
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, ms_out.manufacturer_id);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, ms_out.product_type);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, ms_out.product_id);

    n = zw_cc_zwave_plus_info_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_zwave_plus_info_get_parse(buf, n));
    const zw_cc_zwave_plus_info_report_t zp_in = {.version = 2u,
                                                   .role_type = ZW_ZWAVE_PLUS_ROLE_CENTRAL_STATIC_CONTROLLER,
                                                   .node_type = ZW_ZWAVE_PLUS_NODE_TYPE_NODE,
                                                   .installer_icon = 0x0500u, .user_icon = 0x0500u};
    n = zw_cc_zwave_plus_info_report_build(buf, sizeof(buf), &zp_in);
    TEST_ASSERT_EQUAL_UINT(9u, n);
    zw_cc_zwave_plus_info_report_t zp_out;
    TEST_ASSERT_TRUE(zw_cc_zwave_plus_info_report_parse(buf, n, &zp_out));
    TEST_ASSERT_EQUAL_UINT8(2u, zp_out.version);
    TEST_ASSERT_EQUAL_UINT16(0x0500u, zp_out.installer_icon);
    TEST_ASSERT_EQUAL_UINT16(0x0500u, zp_out.user_icon);
}

/* ================================================================================================
 * §4.18 Indicator - the doc's own "Identify" example, byte-exact
 * ================================================================================================ */

static void test_indicator(void)
{
    uint8_t buf[BUF_MAX];

    const zw_cc_indicator_set_t v1_in = {.has_entries = false, .v1_value = 0x63u};
    size_t n = zw_cc_indicator_set_build(buf, sizeof(buf), &v1_in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_cc_indicator_set_t v1_out;
    TEST_ASSERT_TRUE(zw_cc_indicator_set_parse(buf, n, &v1_out));
    TEST_ASSERT_FALSE(v1_out.has_entries);
    TEST_ASSERT_EQUAL_HEX8(0x63u, v1_out.v1_value);

    /* command-classes.md §4.18 "Identify": 87 01 00 03 50 03 08 50 04 03 50 05 06 */
    static const uint8_t identify[] = {0x87u, 0x01u, 0x00u, 0x03u, 0x50u, 0x03u, 0x08u,
                                        0x50u, 0x04u, 0x03u, 0x50u, 0x05u, 0x06u};
    const uint8_t entries[9] = {0x50u, 0x03u, 0x08u, 0x50u, 0x04u, 0x03u, 0x50u, 0x05u, 0x06u};
    const zw_cc_indicator_set_t id_in = {.has_entries = true, .indicator0_value = 0x00u, .entries = entries,
                                          .entry_count = 3u};
    n = zw_cc_indicator_set_build(buf, sizeof(buf), &id_in);
    TEST_ASSERT_EQUAL_UINT(sizeof(identify), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(identify, buf, sizeof(identify));

    zw_cc_indicator_set_t id_out;
    TEST_ASSERT_TRUE(zw_cc_indicator_set_parse(identify, sizeof(identify), &id_out));
    TEST_ASSERT_TRUE(id_out.has_entries);
    TEST_ASSERT_EQUAL_UINT8(3u, id_out.entry_count);
    zw_cc_indicator_entry_t e;
    TEST_ASSERT_TRUE(zw_cc_indicator_entry_get(id_out.entries, id_out.entry_count * 3u, 0u, &e));
    TEST_ASSERT_EQUAL_HEX8(ZW_INDICATOR_ID_NODE_IDENTIFY, e.indicator_id);
    TEST_ASSERT_EQUAL_HEX8(0x03u, e.property_id);
    TEST_ASSERT_EQUAL_HEX8(0x08u, e.value);
    TEST_ASSERT_TRUE(zw_cc_indicator_entry_get(id_out.entries, id_out.entry_count * 3u, 2u, &e));
    TEST_ASSERT_EQUAL_HEX8(0x05u, e.property_id);
    TEST_ASSERT_EQUAL_HEX8(0x06u, e.value);
    TEST_ASSERT_FALSE(zw_cc_indicator_entry_get(id_out.entries, id_out.entry_count * 3u, 3u, &e));
    TEST_ASSERT_TRUE(zw_cc_indicator_set_parse(identify, sizeof(identify) + 0u, &id_out));

    const zw_cc_indicator_supported_get_t sup_get_in = {.indicator_id = ZW_INDICATOR_ID_FIRST};
    n = zw_cc_indicator_supported_get_build(buf, sizeof(buf), &sup_get_in);
    zw_cc_indicator_supported_get_t sup_get_out;
    TEST_ASSERT_TRUE(zw_cc_indicator_supported_get_parse(buf, n, &sup_get_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_INDICATOR_ID_FIRST, sup_get_out.indicator_id);

    const uint8_t pmask[1] = {0x07u};
    const zw_cc_indicator_supported_report_t sup_rep_in = {.indicator_id = ZW_INDICATOR_ID_FIRST,
                                                            .next_indicator_id = ZW_INDICATOR_ID_NODE_IDENTIFY,
                                                            .property_mask = pmask, .property_mask_len = 1u};
    n = zw_cc_indicator_supported_report_build(buf, sizeof(buf), &sup_rep_in);
    zw_cc_indicator_supported_report_t sup_rep_out;
    TEST_ASSERT_TRUE(zw_cc_indicator_supported_report_parse(buf, n, &sup_rep_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_INDICATOR_ID_NODE_IDENTIFY, sup_rep_out.next_indicator_id);
    TEST_ASSERT_EQUAL_UINT(1u, sup_rep_out.property_mask_len);
}

/* ================================================================================================
 * §4.19 Device Reset Locally, §4.20 Configuration, §4.21 Central Scene, §4.22 Alarm Sensor,
 * §4.23 Hail, §4.24 Time
 * ================================================================================================ */

static void test_device_reset_locally(void)
{
    uint8_t buf[BUF_MAX];
    const size_t n = zw_cc_device_reset_locally_notification_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_device_reset_locally_notification_parse(buf, n));
}

static void test_configuration(void)
{
    uint8_t buf[BUF_MAX];

    const zw_cc_configuration_set_t set_in = {.parameter = 5u, .restore_default = false, .size = 2u,
                                               .value = -1000};
    size_t n = zw_cc_configuration_set_build(buf, sizeof(buf), &set_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_configuration_set_t set_out;
    TEST_ASSERT_TRUE(zw_cc_configuration_set_parse(buf, n, &set_out));
    TEST_ASSERT_EQUAL_UINT8(5u, set_out.parameter);
    TEST_ASSERT_EQUAL_INT32(-1000, set_out.value);
    TEST_ASSERT_FALSE(set_out.restore_default);

    const zw_cc_configuration_set_t restore_in = {.parameter = 5u, .restore_default = true, .size = 1u,
                                                   .value = 0};
    n = zw_cc_configuration_set_build(buf, sizeof(buf), &restore_in);
    TEST_ASSERT_TRUE(zw_cc_configuration_set_parse(buf, n, &set_out));
    TEST_ASSERT_TRUE(set_out.restore_default);

    const zw_cc_configuration_get_t get_in = {.parameter = 5u};
    n = zw_cc_configuration_get_build(buf, sizeof(buf), &get_in);
    zw_cc_configuration_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_configuration_get_parse(buf, n, &get_out));
    TEST_ASSERT_EQUAL_UINT8(5u, get_out.parameter);

    const zw_cc_configuration_report_t rep_in = {.parameter = 5u, .size = 4u, .value = -70000};
    n = zw_cc_configuration_report_build(buf, sizeof(buf), &rep_in);
    TEST_ASSERT_EQUAL_UINT(8u, n);
    zw_cc_configuration_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_configuration_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_INT32(-70000, rep_out.value);
}

static void test_central_scene(void)
{
    uint8_t buf[BUF_MAX];
    const zw_cc_central_scene_notification_t in = {.sequence_number = 0x10u,
                                                     .key_attribute = ZW_CENTRAL_SCENE_KEY_HELD,
                                                     .scene_number = 1u};
    const size_t n = zw_cc_central_scene_notification_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_cc_central_scene_notification_t out;
    TEST_ASSERT_TRUE(zw_cc_central_scene_notification_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_HEX8(ZW_CENTRAL_SCENE_KEY_HELD, out.key_attribute);
}

static void test_alarm_sensor(void)
{
    uint8_t buf[BUF_MAX];
    const zw_cc_alarm_sensor_get_t get_in = {.sensor_type = ZW_ALARM_SENSOR_TYPE_GENERAL};
    size_t n = zw_cc_alarm_sensor_get_build(buf, sizeof(buf), &get_in);
    zw_cc_alarm_sensor_get_t get_out;
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_get_parse(buf, n, &get_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_ALARM_SENSOR_TYPE_GENERAL, get_out.sensor_type);

    const zw_cc_alarm_sensor_report_t rep_in = {.source_node = 3u, .sensor_type = ZW_ALARM_SENSOR_TYPE_GENERAL,
                                                 .state = ZW_ALARM_SENSOR_STATE_ALARM, .seconds = 0xFFFFu};
    n = zw_cc_alarm_sensor_report_build(buf, sizeof(buf), &rep_in);
    TEST_ASSERT_EQUAL_UINT(7u, n);
    zw_cc_alarm_sensor_report_t rep_out;
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_report_parse(buf, n, &rep_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_ALARM_SENSOR_STATE_ALARM, rep_out.state);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, rep_out.seconds);
}

/* ZW-024 (Alarm Sensor), ZW-023 (malformed input): Supported Get `9C 03` and its Report `9C 04`
 * `count · mask`, bit t = type t (bit 0, general purpose, is a real type here); a report
 * shorter than its own count is refused, bytes after the mask are ignored; the answer matches
 * its question (§6). */
static void test_alarm_sensor_supported(void)
{
    uint8_t buf[BUF_MAX];
    size_t n = zw_cc_alarm_sensor_supported_get_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(0x9Cu, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03u, buf[1]);
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_supported_get_parse(buf, n));
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_alarm_sensor_supported_get_build(buf, 1u));
    uint8_t get[2];
    memcpy(get, buf, sizeof get);

    /* a smoke, CO and heat sensor: bits 1, 2, 4 */
    static const uint8_t MASK[] = {0x16u};
    const zw_cc_alarm_sensor_supported_report_t in = {MASK, sizeof MASK};
    n = zw_cc_alarm_sensor_supported_report_build(buf, sizeof(buf), &in);
    static const uint8_t WIRE[] = {0x9Cu, 0x04u, 0x01u, 0x16u};
    TEST_ASSERT_EQUAL_UINT(sizeof WIRE, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(WIRE, buf, n);
    zw_cc_alarm_sensor_supported_report_t out;
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_supported_report_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT(1u, out.mask_len);
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_SMOKE));
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_CO));
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_HEAT));
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_GENERAL));
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_WATER));
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, 9u)); /* past it */
    TEST_ASSERT_TRUE(zw_cc_answers(get, sizeof get, buf, n));
    TEST_ASSERT_FALSE(zw_cc_answers(get, sizeof get, (const uint8_t[]){0x9Cu, 0x02u, 0u, 1u, 0u, 0u, 0u}, 7u));

    /* general purpose only: bit 0 */
    static const uint8_t GENERAL[] = {0x9Cu, 0x04u, 0x01u, 0x01u};
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_supported_report_parse(GENERAL, sizeof GENERAL, &out));
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_GENERAL));

    /* no types at all: a count of 0 */
    static const uint8_t NONE[] = {0x9Cu, 0x04u, 0x00u};
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_supported_report_parse(NONE, sizeof NONE, &out));
    TEST_ASSERT_EQUAL_UINT(0u, out.mask_len);
    TEST_ASSERT_NULL(out.mask);
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, ZW_ALARM_SENSOR_TYPE_SMOKE));

    /* truncated: no count, or fewer mask bytes than it says */
    static const uint8_t NO_COUNT[] = {0x9Cu, 0x04u};
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_supported_report_parse(NO_COUNT, sizeof NO_COUNT, &out));
    static const uint8_t SHORT[] = {0x9Cu, 0x04u, 0x03u, 0x16u, 0x00u};
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_supported_report_parse(SHORT, sizeof SHORT, &out));

    /* oversized: more bytes than its count - the mask is what the count says */
    static const uint8_t LONG[] = {0x9Cu, 0x04u, 0x01u, 0x16u, 0xFFu, 0xFFu};
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_supported_report_parse(LONG, sizeof LONG, &out));
    TEST_ASSERT_EQUAL_UINT(1u, out.mask_len);
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, 9u));

    /* a wide mask (a later device's types), and the wrong command or CC */
    static const uint8_t WIDE[] = {0x9Cu, 0x04u, 0x05u, 0x16u, 0x00u, 0x00u, 0x00u, 0x80u};
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_supported_report_parse(WIDE, sizeof WIDE, &out));
    TEST_ASSERT_EQUAL_UINT(5u, out.mask_len);
    TEST_ASSERT_TRUE(zw_cc_alarm_sensor_type_supported(out.mask, out.mask_len, 39u));
    static const uint8_t WRONG[] = {0x9Cu, 0x02u, 0x01u, 0x16u};
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_supported_report_parse(WRONG, sizeof WRONG, &out));
    static const uint8_t OTHER_CC[] = {0x30u, 0x04u, 0x01u, 0x16u};
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_supported_report_parse(OTHER_CC, sizeof OTHER_CC, &out));
    TEST_ASSERT_FALSE(zw_cc_alarm_sensor_supported_report_parse(WIRE, sizeof WIRE, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_alarm_sensor_supported_report_build(buf, 3u, &in));
}

static void test_hail(void)
{
    uint8_t buf[BUF_MAX];
    const size_t n = zw_cc_hail_build(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_TRUE(zw_cc_hail_parse(buf, n));
}

static void test_time_and_date(void)
{
    uint8_t buf[BUF_MAX];

    size_t n = zw_cc_time_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_time_get_parse(buf, n));
    const zw_cc_time_report_t time_in = {.clock_not_set = false, .hour = 14u, .minute = 30u, .second = 5u};
    n = zw_cc_time_report_build(buf, sizeof(buf), &time_in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_cc_time_report_t time_out;
    TEST_ASSERT_TRUE(zw_cc_time_report_parse(buf, n, &time_out));
    TEST_ASSERT_FALSE(time_out.clock_not_set);
    TEST_ASSERT_EQUAL_UINT8(14u, time_out.hour);

    const zw_cc_time_report_t untrusted = {.clock_not_set = true, .hour = 0u, .minute = 0u, .second = 0u};
    n = zw_cc_time_report_build(buf, sizeof(buf), &untrusted);
    TEST_ASSERT_TRUE(zw_cc_time_report_parse(buf, n, &time_out));
    TEST_ASSERT_TRUE(time_out.clock_not_set);

    n = zw_cc_date_get_build(buf, sizeof(buf));
    TEST_ASSERT_TRUE(zw_cc_date_get_parse(buf, n));
    const zw_cc_date_report_t date_in = {.year = 2026u, .month = 9u, .day = 25u};
    n = zw_cc_date_report_build(buf, sizeof(buf), &date_in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_date_report_t date_out;
    TEST_ASSERT_TRUE(zw_cc_date_report_parse(buf, n, &date_out));
    TEST_ASSERT_EQUAL_UINT16(2026u, date_out.year);
    TEST_ASSERT_EQUAL_UINT8(25u, date_out.day);
}

/* §4.25 Application Status: what a node answers instead of carrying out a Set (Z-Wave JS's
 * applicationRejectedRequest.test.ts sends the Rejected Request): Busy with its wait time,
 * byte-exact both ways; one without its wait time (0); Rejected Request; short, wrong-command and
 * wrong-CC bytes refused, trailing bytes accepted. */
static void test_application_status(void)
{
    uint8_t buf[BUF_MAX];
    const zw_cc_application_busy_t busy_in = {.status = ZW_APPLICATION_BUSY_WAIT, .wait_s = 5u};
    size_t n = zw_cc_application_busy_build(buf, sizeof(buf), &busy_in);
    static const uint8_t BUSY[] = {0x22, 0x01, 0x01, 0x05};
    TEST_ASSERT_EQUAL_UINT(sizeof(BUSY), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(BUSY, buf, sizeof(BUSY));
    zw_cc_application_busy_t busy_out;
    TEST_ASSERT_TRUE(zw_cc_application_busy_parse(buf, n, &busy_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_APPLICATION_BUSY_WAIT, busy_out.status);
    TEST_ASSERT_EQUAL_UINT8(5u, busy_out.wait_s);
    static const uint8_t BUSY_SHORT[] = {0x22, 0x01, 0x00};
    TEST_ASSERT_TRUE(zw_cc_application_busy_parse(BUSY_SHORT, sizeof(BUSY_SHORT), &busy_out));
    TEST_ASSERT_EQUAL_HEX8(ZW_APPLICATION_BUSY_TRY_LATER, busy_out.status);
    TEST_ASSERT_EQUAL_UINT8(0u, busy_out.wait_s);
    TEST_ASSERT_FALSE(zw_cc_application_busy_parse(BUSY, 2u, &busy_out));
    TEST_ASSERT_EQUAL_UINT(0u, zw_cc_application_busy_build(buf, 3u, &busy_in));

    const zw_cc_application_rejected_request_t rej_in = {.status = 0u};
    n = zw_cc_application_rejected_request_build(buf, sizeof(buf), &rej_in);
    static const uint8_t REJECTED[] = {0x22, 0x02, 0x00};
    TEST_ASSERT_EQUAL_UINT(sizeof(REJECTED), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(REJECTED, buf, sizeof(REJECTED));
    zw_cc_application_rejected_request_t rej_out;
    TEST_ASSERT_TRUE(zw_cc_application_rejected_request_parse(buf, n, &rej_out));
    TEST_ASSERT_EQUAL_HEX8(0x00u, rej_out.status);
    static const uint8_t REJECTED_LONG[] = {0x22, 0x02, 0x00, 0x7F};
    TEST_ASSERT_TRUE(zw_cc_application_rejected_request_parse(REJECTED_LONG, sizeof(REJECTED_LONG),
                                                              &rej_out));
    TEST_ASSERT_FALSE(zw_cc_application_rejected_request_parse(REJECTED, 2u, &rej_out));
    TEST_ASSERT_FALSE(zw_cc_application_rejected_request_parse(BUSY, sizeof(BUSY), &rej_out));
    TEST_ASSERT_FALSE(zw_cc_application_busy_parse(REJECTED, sizeof(REJECTED), &busy_out));
    static const uint8_t NOT_IT[] = {0x23, 0x01, 0x00, 0x00};
    TEST_ASSERT_FALSE(zw_cc_application_busy_parse(NOT_IT, sizeof(NOT_IT), &busy_out));
}

/* ================================================================================================
 * §1 Encapsulation
 * ================================================================================================ */

static void test_encap_multi_channel(void)
{
    uint8_t buf[BUF_MAX];
    const uint8_t inner[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET};
    const size_t n = zw_cc_wrap_multi_channel(buf, sizeof(buf), 0u, 3u, false, inner, 2u);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_unwrap_t out;
    TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_MULTI_CHANNEL, out.kind);
    TEST_ASSERT_EQUAL_UINT8(3u, out.destination_endpoint);
    TEST_ASSERT_FALSE(out.destination_is_mask);
    TEST_ASSERT_EQUAL_UINT(2u, out.inner_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, out.inner, 2u);

    /* a command not addressed through Multi Channel is not wrapped at all */
    zw_cc_unwrap_t plain;
    TEST_ASSERT_TRUE(zw_cc_unwrap(inner, 2u, &plain));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_NONE, plain.kind);
    TEST_ASSERT_EQUAL_UINT(2u, plain.inner_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, plain.inner, 2u);

    /* End Point Get/Report and Capability Get/Report are Multi Channel commands but not the
     * encapsulation layer: zw_cc_unwrap must not mistake them for it. */
    uint8_t ep_get[2];
    TEST_ASSERT_EQUAL_UINT(2u, zw_cc_multi_channel_end_point_get_build(ep_get, sizeof(ep_get)));
    zw_cc_unwrap_t ep_out;
    TEST_ASSERT_TRUE(zw_cc_unwrap(ep_get, 2u, &ep_out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_NONE, ep_out.kind);
}

static void test_encap_supervision(void)
{
    uint8_t buf[BUF_MAX];
    const uint8_t inner[2] = {(uint8_t)ZW_CC_BATTERY, ZW_CMD_BATTERY_GET};
    const size_t n = zw_cc_wrap_supervision(buf, sizeof(buf), 0x2Au, true, inner, 2u);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    zw_cc_unwrap_t out;
    TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_SUPERVISION, out.kind);
    TEST_ASSERT_EQUAL_HEX8(0x2Au, out.supervision_session_id);
    TEST_ASSERT_TRUE(out.supervision_status_updates_wanted);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, out.inner, 2u);

    /* a Supervision Report is a terminal answer, not a wrapper: zw_cc_unwrap leaves it alone. */
    const zw_cc_supervision_report_t rep = {.status = ZW_SUPERVISION_STATUS_SUCCESS};
    uint8_t rbuf[8];
    const size_t rn = zw_cc_supervision_report_build(rbuf, sizeof(rbuf), &rep);
    zw_cc_unwrap_t rout;
    TEST_ASSERT_TRUE(zw_cc_unwrap(rbuf, rn, &rout));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_NONE, rout.kind);
}

static void test_encap_crc16(void)
{
    /* command-classes.md §4.16: `56 01 20 02` -> `4D26`, i.e. the full frame is 56 01 20 02 4D 26. */
    static const uint8_t expect[] = {0x56u, 0x01u, 0x20u, 0x02u, 0x4Du, 0x26u};
    const uint8_t inner[2] = {0x20u, 0x02u};
    uint8_t buf[BUF_MAX];
    const size_t n = zw_cc_wrap_crc16(buf, sizeof(buf), inner, 2u);
    TEST_ASSERT_EQUAL_UINT(sizeof(expect), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, buf, sizeof(expect));
    /* cross-check against zw_crc16 directly, the way the header documents chaining it */
    TEST_ASSERT_EQUAL_HEX16(0x4D26u, zw_crc16(ZW_CRC16_INIT, buf, 4u));

    zw_cc_unwrap_t out;
    TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_CRC16, out.kind);
    TEST_ASSERT_EQUAL_UINT(2u, out.inner_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, out.inner, 2u);

    buf[n - 1u] ^= 0xFFu; /* the trailing CRC's own byte: always different, never a coincidence */
    zw_cc_unwrap_t bad;
    TEST_ASSERT_FALSE(zw_cc_unwrap(buf, n, &bad));
}

static void test_encap_multi_command(void)
{
    uint8_t buf[BUF_MAX];
    const uint8_t c0[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET};
    const uint8_t c1[2] = {(uint8_t)ZW_CC_BATTERY, ZW_CMD_BATTERY_GET};
    const uint8_t *cmds[2] = {c0, c1};
    const size_t lens[2] = {2u, 2u};
    const size_t n = zw_cc_wrap_multi_command(buf, sizeof(buf), cmds, lens, 2u);
    TEST_ASSERT_EQUAL_UINT(9u, n); /* 2 header + count + 2 x (1 length + 2 bytes) */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)ZW_CC_MULTI_COMMAND, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_CMD_MULTI_COMMAND_ENCAPSULATE, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(2u, buf[2]);

    zw_cc_unwrap_t out;
    TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_MULTI_COMMAND, out.kind);

    zw_cc_multi_command_iter_t it;
    TEST_ASSERT_TRUE(zw_cc_multi_command_iter_init(&it, out.inner, out.inner_len));
    const uint8_t *cmd;
    size_t cmd_len;
    TEST_ASSERT_TRUE(zw_cc_multi_command_iter_next(&it, &cmd, &cmd_len));
    TEST_ASSERT_EQUAL_UINT(2u, cmd_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(c0, cmd, 2u);
    TEST_ASSERT_TRUE(zw_cc_multi_command_iter_next(&it, &cmd, &cmd_len));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(c1, cmd, 2u);
    TEST_ASSERT_FALSE(zw_cc_multi_command_iter_next(&it, &cmd, &cmd_len)); /* only two were sent */
}

static void test_encap_security_and_transport_service_recognised_only(void)
{
    const uint8_t s2[3] = {(uint8_t)ZW_CC_SECURITY_2, 0xAAu, 0xBBu};
    zw_cc_unwrap_t out;
    TEST_ASSERT_TRUE(zw_cc_unwrap(s2, sizeof(s2), &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_SECURITY_2, out.kind);
    TEST_ASSERT_EQUAL_UINT(2u, out.inner_len); /* handed onward untouched: decoding it is not this file's job */

    const uint8_t s0[3] = {(uint8_t)ZW_CC_SECURITY_0, 0xAAu, 0xBBu};
    TEST_ASSERT_TRUE(zw_cc_unwrap(s0, sizeof(s0), &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_SECURITY_0, out.kind);

    const uint8_t ts[3] = {(uint8_t)ZW_CC_TRANSPORT_SERVICE, 0x01u, 0x02u};
    TEST_ASSERT_TRUE(zw_cc_unwrap(ts, sizeof(ts), &out));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_TRANSPORT_SERVICE, out.kind); /* §1: "not supported - drop and count" */
}

static void test_encap_nesting_order(void)
{
    /* §1 "sending, the panel builds from the inside out": command -> Multi Channel -> Supervision.
     * Receiving unwraps the same layers from the outside in, in whichever order they appear. */
    uint8_t inner_buf[BUF_MAX];
    uint8_t outer_buf[BUF_MAX];
    const uint8_t cmd[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_SET};

    const size_t mc_len = zw_cc_wrap_multi_channel(inner_buf, sizeof(inner_buf), 0u, 2u, false, cmd, 2u);
    TEST_ASSERT_TRUE(mc_len > 0u);
    const size_t sup_len =
        zw_cc_wrap_supervision(outer_buf, sizeof(outer_buf), 5u, false, inner_buf, mc_len);
    TEST_ASSERT_TRUE(sup_len > 0u);

    zw_cc_unwrap_t layer1;
    TEST_ASSERT_TRUE(zw_cc_unwrap(outer_buf, sup_len, &layer1));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_SUPERVISION, layer1.kind);
    TEST_ASSERT_EQUAL_UINT8(5u, layer1.supervision_session_id);

    zw_cc_unwrap_t layer2;
    TEST_ASSERT_TRUE(zw_cc_unwrap(layer1.inner, layer1.inner_len, &layer2));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_MULTI_CHANNEL, layer2.kind);
    TEST_ASSERT_EQUAL_UINT8(2u, layer2.destination_endpoint);

    zw_cc_unwrap_t layer3;
    TEST_ASSERT_TRUE(zw_cc_unwrap(layer2.inner, layer2.inner_len, &layer3));
    TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_NONE, layer3.kind);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(cmd, layer3.inner, 2u);
}

/* ================================================================================================
 * §6 "which answer belongs to which question"
 * ================================================================================================ */

static void test_answers_table(void)
{
    uint8_t a[BUF_MAX];
    uint8_t b[BUF_MAX];
    size_t alen, blen;

    /* Basic Get -> Basic Report */
    alen = zw_cc_basic_get_build(a, sizeof(a));
    blen = zw_cc_basic_report_build(b, sizeof(b), &(zw_cc_basic_report_t){.current_value = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Binary Sensor Get(type t) -> Report[type t]; wrong type does not answer; a v1 report
     * (no type) is accepted regardless. */
    alen = zw_cc_binary_sensor_get_build(
        a, sizeof(a), &(zw_cc_binary_sensor_get_t){.has_type = true, .sensor_type = ZW_BINARY_SENSOR_TYPE_MOTION});
    blen = zw_cc_binary_sensor_report_build(
        b, sizeof(b),
        &(zw_cc_binary_sensor_report_t){.detected = true, .has_type = true,
                                         .sensor_type = ZW_BINARY_SENSOR_TYPE_MOTION});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_binary_sensor_report_build(
        b, sizeof(b),
        &(zw_cc_binary_sensor_report_t){.detected = true, .has_type = true,
                                         .sensor_type = ZW_BINARY_SENSOR_TYPE_SMOKE});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_binary_sensor_report_build(b, sizeof(b), &(zw_cc_binary_sensor_report_t){.detected = true});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen)); /* v1 report, no type: accepted */

    alen = zw_cc_binary_sensor_supported_get_build(a, sizeof(a));
    blen = zw_cc_binary_sensor_supported_report_build(b, sizeof(b),
                                                       &(zw_cc_binary_sensor_supported_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Multilevel Sensor Get(type t) -> Report[type t] */
    alen = zw_cc_multilevel_sensor_get_build(
        a, sizeof(a),
        &(zw_cc_multilevel_sensor_get_t){.has_type = true,
                                          .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE});
    blen = zw_cc_multilevel_sensor_report_build(
        b, sizeof(b),
        &(zw_cc_multilevel_sensor_report_t){.sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE,
                                             .size = 1u, .raw_value = 20});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_multilevel_sensor_report_build(
        b, sizeof(b),
        &(zw_cc_multilevel_sensor_report_t){.sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_HUMIDITY, .size = 1u,
                                             .raw_value = 50});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));

    alen = zw_cc_multilevel_sensor_supported_scale_get_build(
        a, sizeof(a),
        &(zw_cc_multilevel_sensor_supported_scale_get_t){.sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE});
    blen = zw_cc_multilevel_sensor_supported_scale_report_build(
        b, sizeof(b),
        &(zw_cc_multilevel_sensor_supported_scale_report_t){
            .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, .scale_mask = 0x03u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Notification Get(type t) -> Report[type t, or a v1-only report] */
    alen = zw_cc_notification_get_build(
        a, sizeof(a),
        &(zw_cc_notification_get_t){.notification_type = ZW_NOTIFICATION_TYPE_HOME_SECURITY});
    blen = zw_cc_notification_report_build(
        b, sizeof(b),
        &(zw_cc_notification_report_t){.notification_type = ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                        .status = ZW_NOTIFICATION_STATUS_ON});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_notification_report_build(
        b, sizeof(b), &(zw_cc_notification_report_t){.notification_type = ZW_NOTIFICATION_TYPE_SMOKE});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_notification_report_build(b, sizeof(b), &(zw_cc_notification_report_t){.is_v1 = true});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen)); /* v1-only report always counts as an answer */

    alen = zw_cc_notification_event_supported_get_build(
        a, sizeof(a), &(zw_cc_notification_event_supported_get_t){.notification_type = ZW_NOTIFICATION_TYPE_SMOKE});
    blen = zw_cc_notification_event_supported_report_build(
        b, sizeof(b),
        &(zw_cc_notification_event_supported_report_t){.notification_type = ZW_NOTIFICATION_TYPE_SMOKE});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Battery Get -> Battery Report */
    alen = zw_cc_battery_get_build(a, sizeof(a));
    blen = zw_cc_battery_report_build(b, sizeof(b), &(zw_cc_battery_report_t){.level = 50u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Wake Up Interval Get/Capabilities Get */
    alen = zw_cc_wake_up_interval_get_build(a, sizeof(a));
    blen = zw_cc_wake_up_interval_report_build(b, sizeof(b), &(zw_cc_wake_up_interval_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_wake_up_interval_capabilities_get_build(a, sizeof(a));
    blen = zw_cc_wake_up_interval_capabilities_report_build(
        b, sizeof(b), &(zw_cc_wake_up_interval_capabilities_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Association Get(g)/Supported Groupings Get */
    alen = zw_cc_association_get_build(a, sizeof(a), &(zw_cc_association_get_t){.group = 1u});
    blen = zw_cc_association_report_build(b, sizeof(b), &(zw_cc_association_report_t){.group = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_association_report_build(b, sizeof(b), &(zw_cc_association_report_t){.group = 2u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_association_supported_groupings_get_build(a, sizeof(a));
    blen = zw_cc_association_supported_groupings_report_build(
        b, sizeof(b), &(zw_cc_association_supported_groupings_report_t){.group_count = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Multi Channel Association Get(g) */
    alen = zw_cc_mc_association_get_build(a, sizeof(a), &(zw_cc_mc_association_get_t){.group = 1u});
    blen = zw_cc_mc_association_report_build(b, sizeof(b), &(zw_cc_mc_association_report_t){.group = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_mc_association_report_build(b, sizeof(b), &(zw_cc_mc_association_report_t){.group = 2u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));

    /* AGI Name/Info/Command List Get(g) */
    alen = zw_cc_agi_name_get_build(a, sizeof(a), &(zw_cc_agi_name_get_t){.group = 1u});
    blen = zw_cc_agi_name_report_build(b, sizeof(b), &(zw_cc_agi_name_report_t){.group = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_agi_name_report_build(b, sizeof(b), &(zw_cc_agi_name_report_t){.group = 2u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));

    uint8_t g1[7] = {1u, 0u, 0u, 1u, 0u, 0u, 0u};
    alen = zw_cc_agi_info_get_build(a, sizeof(a), &(zw_cc_agi_info_get_t){.list_mode = false, .group = 1u});
    blen = zw_cc_agi_info_report_build(
        b, sizeof(b), &(zw_cc_agi_info_report_t){.count = 1u, .groups = g1, .groups_len = 7u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_agi_info_get_build(a, sizeof(a), &(zw_cc_agi_info_get_t){.list_mode = true, .group = 0u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen)); /* list mode: any list-mode report answers it */

    alen = zw_cc_agi_command_list_get_build(a, sizeof(a), &(zw_cc_agi_command_list_get_t){.group = 1u});
    blen = zw_cc_agi_command_list_report_build(b, sizeof(b), &(zw_cc_agi_command_list_report_t){.group = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Version Get / Command Class Get(cc) / Capabilities Get */
    alen = zw_cc_version_get_build(a, sizeof(a));
    blen = zw_cc_version_report_build(
        b, sizeof(b), &(zw_cc_version_report_t){.protocol_major = 7u, .protocol_minor = 18u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_version_command_class_get_build(a, sizeof(a),
                                                  &(zw_cc_version_command_class_get_t){.cc = (uint8_t)ZW_CC_BASIC});
    blen = zw_cc_version_command_class_report_build(
        b, sizeof(b), &(zw_cc_version_command_class_report_t){.cc = (uint8_t)ZW_CC_BASIC, .version = 2u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_version_command_class_report_build(
        b, sizeof(b), &(zw_cc_version_command_class_report_t){.cc = (uint8_t)ZW_CC_BATTERY, .version = 1u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_version_capabilities_get_build(a, sizeof(a));
    blen = zw_cc_version_capabilities_report_build(b, sizeof(b), &(zw_cc_version_capabilities_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Manufacturer Specific Get / Z-Wave Plus Info Get */
    alen = zw_cc_manufacturer_specific_get_build(a, sizeof(a));
    blen = zw_cc_manufacturer_specific_report_build(b, sizeof(b), &(zw_cc_manufacturer_specific_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_zwave_plus_info_get_build(a, sizeof(a));
    blen = zw_cc_zwave_plus_info_report_build(b, sizeof(b), &(zw_cc_zwave_plus_info_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Multi Channel End Point Get / Capability Get(e) */
    alen = zw_cc_multi_channel_end_point_get_build(a, sizeof(a));
    blen = zw_cc_multi_channel_end_point_report_build(b, sizeof(b),
                                                       &(zw_cc_multi_channel_end_point_report_t){0});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_multi_channel_capability_get_build(a, sizeof(a), &(zw_cc_multi_channel_capability_get_t){.endpoint = 2u});
    blen = zw_cc_multi_channel_capability_report_build(
        b, sizeof(b), &(zw_cc_multi_channel_capability_report_t){.endpoint = 2u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_multi_channel_capability_report_build(
        b, sizeof(b), &(zw_cc_multi_channel_capability_report_t){.endpoint = 3u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));

    /* Configuration Get(p) */
    alen = zw_cc_configuration_get_build(a, sizeof(a), &(zw_cc_configuration_get_t){.parameter = 5u});
    blen = zw_cc_configuration_report_build(b, sizeof(b),
                                             &(zw_cc_configuration_report_t){.parameter = 5u, .size = 1u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_configuration_report_build(b, sizeof(b),
                                             &(zw_cc_configuration_report_t){.parameter = 6u, .size = 1u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));

    /* Indicator Supported Get(i) */
    alen = zw_cc_indicator_supported_get_build(a, sizeof(a),
                                                &(zw_cc_indicator_supported_get_t){.indicator_id = 0x50u});
    blen = zw_cc_indicator_supported_report_build(
        b, sizeof(b), &(zw_cc_indicator_supported_report_t){.indicator_id = 0x50u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));

    /* Supervision Get(session s) */
    const uint8_t cmd[2] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET};
    alen = zw_cc_supervision_get_build(
        a, sizeof(a), &(zw_cc_supervision_get_t){.session_id = 9u, .command = cmd, .command_len = 2u});
    blen = zw_cc_supervision_report_build(b, sizeof(b), &(zw_cc_supervision_report_t){.session_id = 9u});
    TEST_ASSERT_TRUE(zw_cc_answers(a, alen, b, blen));
    blen = zw_cc_supervision_report_build(b, sizeof(b), &(zw_cc_supervision_report_t){.session_id = 10u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));

    /* A Set has no answer; No Operation and Security are explicitly out of scope here. */
    alen = zw_cc_basic_set_build(a, sizeof(a), &(zw_cc_basic_set_t){.value = 1u});
    blen = zw_cc_basic_report_build(b, sizeof(b), &(zw_cc_basic_report_t){.current_value = 1u});
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, b, blen));
    alen = zw_cc_nop_build(a, sizeof(a));
    TEST_ASSERT_FALSE(zw_cc_answers(a, alen, a, alen));
    const uint8_t s2get[2] = {(uint8_t)ZW_CC_SECURITY_2, 0x01u};
    TEST_ASSERT_FALSE(zw_cc_answers(s2get, 2u, s2get, 2u));

    /* two lengths too short to even carry a CC/command pair: refused, not a crash */
    TEST_ASSERT_FALSE(zw_cc_answers(a, 1u, b, 2u));
    TEST_ASSERT_FALSE(zw_cc_answers(a, 2u, b, 1u));
}

/* ================================================================================================
 * §5 "what the panel answers"
 * ================================================================================================ */

static const zw_cc_panel_identity_t TEST_IDENTITY = {
    .protocol_major = 7u, .protocol_minor = 18u, .firmware_major = 1u, .firmware_minor = 3u,
    .manufacturer_id = 0xFFFFu, .product_type = 0xFFFFu, .product_id = 0xFFFFu,
    .zwave_plus_role_type = ZW_ZWAVE_PLUS_ROLE_CENTRAL_STATIC_CONTROLLER,
    .zwave_plus_node_type = ZW_ZWAVE_PLUS_NODE_TYPE_NODE, .installer_icon = 0x0500u, .user_icon = 0x0500u,
    .clock_trusted = true, .time_hour = 14u, .time_minute = 5u, .time_second = 0u, .date_year = 2026u,
    .date_month = 9u, .date_day = 25u,
};

static void test_panel_answers(void)
{
    uint8_t cmd[BUF_MAX];
    uint8_t out[BUF_MAX];
    size_t out_len;

    /* Version Get: library 01, this panel's protocol/firmware versions, hardware 01, no targets */
    size_t n = zw_cc_version_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_version_report_t vrep;
    TEST_ASSERT_TRUE(zw_cc_version_report_parse(out, out_len, &vrep));
    TEST_ASSERT_EQUAL_HEX8(ZW_VERSION_LIBRARY_STATIC_CONTROLLER, vrep.library_type);
    TEST_ASSERT_EQUAL_UINT8(7u, vrep.protocol_major);
    TEST_ASSERT_EQUAL_UINT8(1u, vrep.firmware0_major);
    TEST_ASSERT_TRUE(vrep.has_v2);
    TEST_ASSERT_EQUAL_UINT8(0u, vrep.other_target_count);

    /* Version Command Class Get: the answered table, and 0 for anything not in it */
    n = zw_cc_version_command_class_get_build(cmd, sizeof(cmd),
                                               &(zw_cc_version_command_class_get_t){.cc = (uint8_t)ZW_CC_ZWAVE_PLUS_INFO});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_version_command_class_report_t ccrep;
    TEST_ASSERT_TRUE(zw_cc_version_command_class_report_parse(out, out_len, &ccrep));
    TEST_ASSERT_EQUAL_UINT8(2u, ccrep.version);
    n = zw_cc_version_command_class_get_build(cmd, sizeof(cmd),
                                               &(zw_cc_version_command_class_get_t){.cc = (uint8_t)ZW_CC_BASIC});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    TEST_ASSERT_TRUE(zw_cc_version_command_class_report_parse(out, out_len, &ccrep));
    TEST_ASSERT_EQUAL_UINT8(0u, ccrep.version); /* Basic is not one of §5's answered CCs */

    /* Association Specific Group Get -> group 0 */
    n = zw_cc_association_specific_group_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_association_specific_group_report_t spg;
    TEST_ASSERT_TRUE(zw_cc_association_specific_group_report_parse(out, out_len, &spg));
    TEST_ASSERT_EQUAL_UINT8(0u, spg.group);

    /* AGI Command List Get(1) -> group 1, an empty list; group 2 is not handled */
    n = zw_cc_agi_command_list_get_build(cmd, sizeof(cmd), &(zw_cc_agi_command_list_get_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_agi_command_list_report_t clr;
    TEST_ASSERT_TRUE(zw_cc_agi_command_list_report_parse(out, out_len, &clr));
    TEST_ASSERT_EQUAL_UINT8(1u, clr.group);
    TEST_ASSERT_EQUAL_UINT(0u, clr.cc_command_pairs_len);
    n = zw_cc_agi_command_list_get_build(cmd, sizeof(cmd), &(zw_cc_agi_command_list_get_t){.group = 2u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NOT_HANDLED,
                      zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));

    /* Manufacturer Specific Get: whatever the caller's identity carries (FFFF/FFFF/FFFF today) */
    n = zw_cc_manufacturer_specific_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_manufacturer_specific_report_t msr;
    TEST_ASSERT_TRUE(zw_cc_manufacturer_specific_report_parse(out, out_len, &msr));
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, msr.manufacturer_id);

    /* Z-Wave Plus Info Get: version 2, central static controller, node, the identity's icons */
    n = zw_cc_zwave_plus_info_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_zwave_plus_info_report_t zpr;
    TEST_ASSERT_TRUE(zw_cc_zwave_plus_info_report_parse(out, out_len, &zpr));
    TEST_ASSERT_EQUAL_UINT8(2u, zpr.version);
    TEST_ASSERT_EQUAL_HEX8(ZW_ZWAVE_PLUS_ROLE_CENTRAL_STATIC_CONTROLLER, zpr.role_type);
    TEST_ASSERT_EQUAL_UINT16(0x0500u, zpr.installer_icon);

    /* Association Supported Groupings Get -> 1; Get(group 1) -> reports to nobody; group 2: not handled */
    n = zw_cc_association_supported_groupings_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_association_supported_groupings_report_t sgr;
    TEST_ASSERT_TRUE(zw_cc_association_supported_groupings_report_parse(out, out_len, &sgr));
    TEST_ASSERT_EQUAL_UINT8(1u, sgr.group_count);

    n = zw_cc_association_get_build(cmd, sizeof(cmd), &(zw_cc_association_get_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_association_report_t ar;
    TEST_ASSERT_TRUE(zw_cc_association_report_parse(out, out_len, &ar));
    TEST_ASSERT_EQUAL_UINT8(1u, ar.max_nodes);
    TEST_ASSERT_EQUAL_UINT(0u, ar.node_id_count);

    n = zw_cc_association_get_build(cmd, sizeof(cmd), &(zw_cc_association_get_t){.group = 2u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NOT_HANDLED,
                      zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));

    /* Association Set/Remove: accepted and ignored - no command-level reply */
    n = zw_cc_association_set_build(cmd, sizeof(cmd), &(zw_cc_association_set_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NO_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    n = zw_cc_association_remove_build(cmd, sizeof(cmd), &(zw_cc_association_remove_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NO_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));

    /* Multi Channel Association Get/Supported Groupings Get: "as Association" */
    n = zw_cc_mc_association_supported_groupings_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    n = zw_cc_mc_association_get_build(cmd, sizeof(cmd), &(zw_cc_mc_association_get_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));

    /* AGI Name Get(1) -> "Lifeline"; group 2 is not handled. Info Get answers with group 1. */
    n = zw_cc_agi_name_get_build(cmd, sizeof(cmd), &(zw_cc_agi_name_get_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_agi_name_report_t nr;
    TEST_ASSERT_TRUE(zw_cc_agi_name_report_parse(out, out_len, &nr));
    TEST_ASSERT_EQUAL_UINT(8u, nr.name_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("Lifeline", nr.name, 8u);
    n = zw_cc_agi_name_get_build(cmd, sizeof(cmd), &(zw_cc_agi_name_get_t){.group = 2u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NOT_HANDLED,
                      zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));

    n = zw_cc_agi_info_get_build(cmd, sizeof(cmd), &(zw_cc_agi_info_get_t){.group = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_agi_info_report_t ir;
    TEST_ASSERT_TRUE(zw_cc_agi_info_report_parse(out, out_len, &ir));
    zw_cc_agi_group_info_t entry;
    TEST_ASSERT_TRUE(zw_cc_agi_group_info_get(ir.groups, ir.groups_len, 0u, &entry));
    TEST_ASSERT_EQUAL_UINT8(1u, entry.group);
    TEST_ASSERT_EQUAL_UINT16(ZW_AGI_PROFILE_GENERAL_LIFELINE, entry.profile);

    /* Time Get / Date Get (phase 2): the panel's own clock */
    n = zw_cc_time_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_time_report_t tr;
    TEST_ASSERT_TRUE(zw_cc_time_report_parse(out, out_len, &tr));
    TEST_ASSERT_FALSE(tr.clock_not_set);
    TEST_ASSERT_EQUAL_UINT8(14u, tr.hour);
    n = zw_cc_date_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    zw_cc_date_report_t dr;
    TEST_ASSERT_TRUE(zw_cc_date_report_parse(out, out_len, &dr));
    TEST_ASSERT_EQUAL_UINT16(2026u, dr.year);

    /* an untrusted clock: bit 7 (clock not set) reflects it */
    const zw_cc_panel_identity_t untrusted = TEST_IDENTITY;
    zw_cc_panel_identity_t untrusted2 = untrusted;
    untrusted2.clock_trusted = false;
    n = zw_cc_time_get_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_REPLY, zw_cc_panel_answer(cmd, n, &untrusted2, out, sizeof(out), &out_len));
    TEST_ASSERT_TRUE(zw_cc_time_report_parse(out, out_len, &tr));
    TEST_ASSERT_TRUE(tr.clock_not_set);

    /* anything else: not handled (a wrapping Supervision would say status 00) */
    n = zw_cc_hail_build(cmd, sizeof(cmd));
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NOT_HANDLED,
                      zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));
    n = zw_cc_configuration_get_build(cmd, sizeof(cmd), &(zw_cc_configuration_get_t){.parameter = 1u});
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NOT_HANDLED,
                      zw_cc_panel_answer(cmd, n, &TEST_IDENTITY, out, sizeof(out), &out_len));

    /* too short to be any command at all */
    TEST_ASSERT_EQUAL(ZW_CC_ANSWER_NOT_HANDLED,
                      zw_cc_panel_answer(cmd, 1u, &TEST_IDENTITY, out, sizeof(out), &out_len));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nop);
    RUN_TEST(test_basic);
    RUN_TEST(test_binary_sensor);
    RUN_TEST(test_multilevel_sensor);
    RUN_TEST(test_multilevel_sensor_three_bytes);
    RUN_TEST(test_battery);
    RUN_TEST(test_notification);
    RUN_TEST(test_supervision);
    RUN_TEST(test_multi_channel);
    RUN_TEST(test_wake_up);
    RUN_TEST(test_association);
    RUN_TEST(test_mc_association);
    RUN_TEST(test_agi);
    RUN_TEST(test_version);
    RUN_TEST(test_manufacturer_specific_and_zwave_plus_info);
    RUN_TEST(test_indicator);
    RUN_TEST(test_device_reset_locally);
    RUN_TEST(test_configuration);
    RUN_TEST(test_central_scene);
    RUN_TEST(test_alarm_sensor);
    RUN_TEST(test_alarm_sensor_supported);
    RUN_TEST(test_hail);
    RUN_TEST(test_time_and_date);
    RUN_TEST(test_application_status);
    RUN_TEST(test_encap_multi_channel);
    RUN_TEST(test_encap_supervision);
    RUN_TEST(test_encap_crc16);
    RUN_TEST(test_encap_multi_command);
    RUN_TEST(test_encap_security_and_transport_service_recognised_only);
    RUN_TEST(test_encap_nesting_order);
    RUN_TEST(test_answers_table);
    RUN_TEST(test_panel_answers);
    return UNITY_END();
}
