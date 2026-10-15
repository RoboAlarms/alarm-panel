/*
 * test_zw_api.c - the Serial API's commands (components/core/zwave_proto/src/zw_api_*.c,
 * docs/features/36 serial-api.md §2-§6), proved without a ZAC93: every phase-1 message (plus DA
 * and the SerialAPISetup sub-commands §5 uses) round-trips at both node-ID widths, the doc's own
 * worked examples come out byte-exact, and every parser refuses malformed input. Last, whole
 * frames captured from real controllers (Z-Wave JS's tests) through the receive parser and the
 * codecs.
 *
 * Requirements: ZW-001, ZW-003, ZW-004, ZW-023
 */
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_frame.h"

void setUp(void) {}
void tearDown(void) {}

#define BUF_MAX 400u

/* ================================================================================================
 * §3 helpers
 * ================================================================================================ */

static void test_node_id_codec(void)
{
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_node_id_build(buf, sizeof(buf), ZW_NID_8, 0x42u));
    TEST_ASSERT_EQUAL_HEX8(0x42u, buf[0]);
    TEST_ASSERT_EQUAL_UINT(0u, zw_api_node_id_build(buf, sizeof(buf), ZW_NID_8, 0x100u)); /* ZW-023 */
    TEST_ASSERT_EQUAL_UINT(2u, zw_api_node_id_build(buf, sizeof(buf), ZW_NID_16, 0x1234u));
    TEST_ASSERT_EQUAL_HEX8(0x12u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x34u, buf[1]);
    TEST_ASSERT_EQUAL_UINT(0u, zw_api_node_id_build(buf, 1u, ZW_NID_16, 0x1234u)); /* cap too small */

    uint16_t nid = 0u;
    TEST_ASSERT_TRUE(zw_api_node_id_parse(buf, 2u, ZW_NID_16, &nid));
    TEST_ASSERT_EQUAL_UINT16(0x1234u, nid);
    TEST_ASSERT_FALSE(zw_api_node_id_parse(buf, 1u, ZW_NID_16, &nid));  /* one byte short */
    /* a building block, not a whole message: it only enforces the minimum, leaving anything past
     * the first 'w' bytes for the caller (a `_req` wrapper adds its own exact-length check). */
    TEST_ASSERT_TRUE(zw_api_node_id_parse(buf, 3u, ZW_NID_16, &nid));
    TEST_ASSERT_EQUAL_UINT16(0x1234u, nid);
    /* likewise reading only the first byte at 8-bit width, ignoring the second as "extra" */
    TEST_ASSERT_TRUE(zw_api_node_id_parse(buf, 2u, ZW_NID_8, &nid));
    TEST_ASSERT_EQUAL_UINT16(0x0012u, nid);
}

static void test_node_list_helpers(void)
{
    uint8_t list[ZW_NODE_LIST_BYTES];
    memset(list, 0, sizeof(list));
    TEST_ASSERT_FALSE(zw_node_list_test(list, 1u));
    zw_node_list_set(list, 1u, true);
    zw_node_list_set(list, 232u, true);
    zw_node_list_set(list, 9u, true);
    TEST_ASSERT_TRUE(zw_node_list_test(list, 1u));
    TEST_ASSERT_TRUE(zw_node_list_test(list, 232u));
    TEST_ASSERT_TRUE(zw_node_list_test(list, 9u));
    TEST_ASSERT_FALSE(zw_node_list_test(list, 2u));
    zw_node_list_set(list, 1u, false);
    TEST_ASSERT_FALSE(zw_node_list_test(list, 1u));

    uint16_t out[8];
    TEST_ASSERT_EQUAL_UINT(2u, zw_node_list_iterate(list, out, 8u));
    TEST_ASSERT_EQUAL_UINT16(9u, out[0]);
    TEST_ASSERT_EQUAL_UINT16(232u, out[1]);
    TEST_ASSERT_EQUAL_UINT(2u, zw_node_list_iterate(list, NULL, 0u)); /* count only */

    TEST_ASSERT_FALSE(zw_node_list_test(list, 0u));   /* out of range: node IDs are 1-based */
    TEST_ASSERT_FALSE(zw_node_list_test(list, 233u));
    zw_node_list_set(list, 233u, true);               /* silently ignored, never crashes */
    TEST_ASSERT_FALSE(zw_node_list_test(list, 233u));
}

static void test_lr_segment_helpers(void)
{
    uint8_t seg[ZW_LR_SEGMENT_BYTES];
    memset(seg, 0, sizeof(seg));
    TEST_ASSERT_FALSE(zw_lr_segment_test(seg, 0u, 256u));
    zw_lr_segment_set(seg, 0u, 256u, true);
    zw_lr_segment_set(seg, 0u, 383u, true);
    TEST_ASSERT_TRUE(zw_lr_segment_test(seg, 0u, 256u));
    TEST_ASSERT_TRUE(zw_lr_segment_test(seg, 0u, 383u));
    TEST_ASSERT_FALSE(zw_lr_segment_test(seg, 0u, 257u));
    TEST_ASSERT_FALSE(zw_lr_segment_test(seg, 0u, 255u)); /* below the segment */
    TEST_ASSERT_FALSE(zw_lr_segment_test(seg, 0u, 384u)); /* above the segment */

    uint16_t out[4];
    TEST_ASSERT_EQUAL_UINT(2u, zw_lr_segment_iterate(seg, 0u, out, 4u));
    TEST_ASSERT_EQUAL_UINT16(256u, out[0]);
    TEST_ASSERT_EQUAL_UINT16(383u, out[1]);

    uint8_t seg1[ZW_LR_SEGMENT_BYTES];
    memset(seg1, 0, sizeof(seg1));
    zw_lr_segment_set(seg1, 1u, 400u, true);
    TEST_ASSERT_TRUE(zw_lr_segment_test(seg1, 1u, 400u));
    TEST_ASSERT_FALSE(zw_lr_segment_test(seg1, 0u, 400u)); /* wrong segment index */
}

static void test_func_mask(void)
{
    uint8_t mask[ZW_FUNC_MASK_BYTES];
    memset(mask, 0, sizeof(mask));
    TEST_ASSERT_FALSE(zw_func_mask_test(mask, (uint8_t)ZW_FUNC_SEND_DATA));
    mask[(0x13u - 1u) / 8u] = (uint8_t)(1u << ((0x13u - 1u) % 8u)); /* function 0x13 = SendData */
    TEST_ASSERT_TRUE(zw_func_mask_test(mask, (uint8_t)ZW_FUNC_SEND_DATA));
    TEST_ASSERT_FALSE(zw_func_mask_test(mask, (uint8_t)ZW_FUNC_GET_CONTROLLER_ID));
    TEST_ASSERT_FALSE(zw_func_mask_test(mask, 0u)); /* function 0 does not exist */
}

static void test_cc_iterator(void)
{
    const uint8_t list[] = {0x5Eu, 0x9Fu, 0x6Cu, 0x56u, 0x8Fu, 0xEFu, 0x98u, 0xF1u, 0x00u};
    const uint16_t expect_id[] = {0x5Eu, 0x9Fu, 0x6Cu, 0x56u, 0x8Fu, 0x98u, 0xF100u};
    const bool expect_ctl[] = {false, false, false, false, false, true, true};

    zw_cc_iter_t it;
    zw_cc_iter_init(&it, list, sizeof(list));
    for (size_t i = 0; i < 7u; i++) {
        uint16_t id;
        bool controlled;
        TEST_ASSERT_TRUE(zw_cc_iter_next(&it, &id, &controlled));
        TEST_ASSERT_EQUAL_HEX16(expect_id[i], id);
        TEST_ASSERT_EQUAL(expect_ctl[i], controlled);
    }
    uint16_t id;
    bool controlled;
    TEST_ASSERT_FALSE(zw_cc_iter_next(&it, &id, &controlled));
    TEST_ASSERT_TRUE(zw_cc_list_is_well_formed(list, sizeof(list)));
    TEST_ASSERT_TRUE(zw_cc_list_is_well_formed(NULL, 0u)); /* empty is well-formed */

    const uint8_t truncated[] = {0x5Eu, 0xF1u}; /* a two-byte identifier cut short */
    TEST_ASSERT_FALSE(zw_cc_list_is_well_formed(truncated, sizeof(truncated)));
    zw_cc_iter_init(&it, truncated, sizeof(truncated));
    TEST_ASSERT_TRUE(zw_cc_iter_next(&it, &id, &controlled));
    TEST_ASSERT_EQUAL_HEX16(0x5Eu, id);
    TEST_ASSERT_FALSE(zw_cc_iter_next(&it, &id, &controlled));
}

static void test_tx_report(void)
{
    zw_api_tx_report_t in;
    memset(&in, 0, sizeof(in));
    in.transmit_ticks = 0x0102u;
    in.repeater_count = 1u;
    in.ack_rssi = (uint8_t)0xC8;
    for (size_t i = 0; i < 4u; i++) {
        in.ack_rssi_per_repeater[i] = (uint8_t)(0x10u + i);
    }
    in.ack_channel = 1u;
    in.tx_channel = 2u;
    in.route_scheme_state = 3u;
    for (size_t i = 0; i < 4u; i++) {
        in.repeater_node_ids[i] = (uint8_t)(0x20u + i);
    }
    in.beam_and_route_speed = 0x41u;
    in.routing_attempts = 1u;

    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(17u, zw_api_tx_report_build(buf, sizeof(buf), &in));
    zw_api_tx_report_t out;
    TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, 17u, &out));
    TEST_ASSERT_EQUAL_UINT16(0x0102u, out.transmit_ticks);
    TEST_ASSERT_FALSE(out.has_last_working_node);
    TEST_ASSERT_FALSE(out.has_first_failing_node);
    TEST_ASSERT_FALSE(out.has_long_range);
    TEST_ASSERT_FALSE(zw_api_tx_report_parse(buf, 16u, &out)); /* one byte short */

    in.has_last_working_node = true;
    in.last_working_node = 5u;
    TEST_ASSERT_EQUAL_UINT(18u, zw_api_tx_report_build(buf, sizeof(buf), &in));
    TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, 18u, &out));
    TEST_ASSERT_TRUE(out.has_last_working_node);
    TEST_ASSERT_EQUAL_UINT8(5u, out.last_working_node);

    in.has_first_failing_node = true;
    in.first_failing_node = 6u;
    TEST_ASSERT_EQUAL_UINT(19u, zw_api_tx_report_build(buf, sizeof(buf), &in));
    TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, 19u, &out));
    TEST_ASSERT_TRUE(out.has_first_failing_node);
    TEST_ASSERT_EQUAL_UINT8(6u, out.first_failing_node);
    /* below 24, byte 19 is not yet a Long Range field: it is pure trailing, accepted and ignored */
    buf[19] = 0xAAu;
    TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, 20u, &out));
    TEST_ASSERT_TRUE(out.has_first_failing_node);
    TEST_ASSERT_FALSE(out.has_long_range);
    TEST_ASSERT_EQUAL_UINT8(6u, out.first_failing_node);

    in.has_long_range = true;
    in.lr_tx_power = -5;
    in.lr_noise_floor = 7u;
    in.lr_dest_ack_tx_power = -6;
    in.lr_dest_ack_rssi = ZW_RSSI_NOT_AVAILABLE;
    in.lr_dest_noise_floor = 8u;
    TEST_ASSERT_EQUAL_UINT(24u, zw_api_tx_report_build(buf, sizeof(buf), &in));
    TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, 24u, &out));
    TEST_ASSERT_TRUE(out.has_long_range);
    TEST_ASSERT_EQUAL_INT(-5, out.lr_tx_power);
    TEST_ASSERT_EQUAL_INT(ZW_RSSI_NOT_AVAILABLE, out.lr_dest_ack_rssi);
    /* past 24 bytes, further data is always pure trailing, whatever it is */
    buf[24] = 0xAAu;
    TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, 25u, &out));
    TEST_ASSERT_TRUE(out.has_long_range);
    TEST_ASSERT_EQUAL_INT(-5, out.lr_tx_power);

    /* first_failing_node without last_working_node, or long_range without both, is inconsistent */
    zw_api_tx_report_t bad = in;
    bad.has_last_working_node = false;
    TEST_ASSERT_EQUAL_UINT(0u, zw_api_tx_report_build(buf, sizeof(buf), &bad));
    bad = in;
    bad.has_first_failing_node = false;
    TEST_ASSERT_EQUAL_UINT(0u, zw_api_tx_report_build(buf, sizeof(buf), &bad));
}

static void test_node_update_shape(void)
{
    const uint8_t cc[] = {0x5Eu, 0x9Fu, 0x6Cu};
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_node_update_t in = {
            .node_id = w == ZW_NID_8 ? 5u : 300u,
            .basic_class = 0x04u, .generic_class = 0x20u, .specific_class = 0x01u,
            .cc_list = cc, .cc_list_len = sizeof(cc),
        };
        uint8_t buf[BUF_MAX];
        const size_t n = zw_api_node_update_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w + 4u + sizeof(cc), n);
        TEST_ASSERT_EQUAL_UINT8(3u + sizeof(cc), buf[(size_t)w]); /* L = 3 + cc_list_len */

        zw_api_node_update_t out;
        TEST_ASSERT_TRUE(zw_api_node_update_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT16(in.node_id, out.node_id);
        TEST_ASSERT_EQUAL_UINT8(in.basic_class, out.basic_class);
        TEST_ASSERT_EQUAL_UINT8(in.generic_class, out.generic_class);
        TEST_ASSERT_EQUAL_UINT8(in.specific_class, out.specific_class);
        TEST_ASSERT_EQUAL_UINT(sizeof(cc), out.cc_list_len);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(cc, out.cc_list, sizeof(cc));

        TEST_ASSERT_FALSE(zw_api_node_update_parse(buf, n - 1u, w, &out)); /* one byte short */
        buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_node_update_parse(buf, n + 1u, w, &out));
        TEST_ASSERT_EQUAL_UINT16(in.node_id, out.node_id);
        TEST_ASSERT_EQUAL_UINT(sizeof(cc), out.cc_list_len);
        uint8_t bad_l[BUF_MAX];
        memcpy(bad_l, buf, n);
        bad_l[(size_t)w] = 2u; /* L < 3 */
        TEST_ASSERT_FALSE(zw_api_node_update_parse(bad_l, n, w, &out));
    }
    /* an empty CC list is fine */
    zw_api_node_update_t empty = {.node_id = 1u, .basic_class = 1u, .generic_class = 2u, .specific_class = 3u};
    uint8_t buf[16];
    const size_t n = zw_api_node_update_build(buf, sizeof(buf), ZW_NID_8, &empty);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_api_node_update_t out;
    TEST_ASSERT_TRUE(zw_api_node_update_parse(buf, n, ZW_NID_8, &out));
    TEST_ASSERT_EQUAL_UINT(0u, out.cc_list_len);
    TEST_ASSERT_NULL(out.cc_list);
}

static void test_smartstart_join_shape(void)
{
    const uint8_t cc[] = {0x20u, 0x30u};
    const uint8_t nwi[4] = {0x11u, 0x22u, 0x33u, 0x44u};
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_smartstart_join_t in = {
            .node_id = 0u, .receive_status = 0x01u,
            .basic_class = 0x04u, .generic_class = 0x21u, .specific_class = 0x01u,
            .cc_list = cc, .cc_list_len = sizeof(cc),
        };
        memcpy(in.nwi_home_id, nwi, 4u);
        uint8_t buf[BUF_MAX];
        const size_t n = zw_api_smartstart_join_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w + 9u + sizeof(cc), n);

        zw_api_smartstart_join_t out;
        TEST_ASSERT_TRUE(zw_api_smartstart_join_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(nwi, out.nwi_home_id, 4u);
        TEST_ASSERT_EQUAL_UINT8(in.receive_status, out.receive_status);
        TEST_ASSERT_EQUAL_UINT(sizeof(cc), out.cc_list_len);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(cc, out.cc_list, sizeof(cc));

        TEST_ASSERT_FALSE(zw_api_smartstart_join_parse(buf, n - 1u, w, &out));
        buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_smartstart_join_parse(buf, n + 1u, w, &out));
        TEST_ASSERT_EQUAL_UINT(sizeof(cc), out.cc_list_len);
    }
}

/* ================================================================================================
 * `0B` SerialAPISetup
 * ================================================================================================ */

static void test_api_setup_envelope(void)
{
    const uint8_t data[] = {0xFFu};
    zw_api_api_setup_req_t req = {.sub = ZW_SETUP_SET_TX_STATUS_REPORT, .data = data, .data_len = 1u};
    uint8_t buf[8];
    const size_t n = zw_api_api_setup_req_build(buf, sizeof(buf), &req);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_TX_STATUS_REPORT, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, buf[1]);

    zw_api_api_setup_req_t out;
    TEST_ASSERT_TRUE(zw_api_api_setup_req_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_TX_STATUS_REPORT, out.sub);
    TEST_ASSERT_EQUAL_UINT(1u, out.data_len);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, out.data[0]);
    TEST_ASSERT_FALSE(zw_api_api_setup_req_parse(buf, 0u, &out)); /* needs at least the sub byte */

    /* a sub-command with no data at all (e.g. GetSupportedRegions) */
    zw_api_api_setup_req_t bare = {.sub = ZW_SETUP_GET_SUPPORTED_REGIONS, .data = NULL, .data_len = 0u};
    const size_t n2 = zw_api_api_setup_req_build(buf, sizeof(buf), &bare);
    TEST_ASSERT_EQUAL_UINT(1u, n2);
    TEST_ASSERT_TRUE(zw_api_api_setup_req_parse(buf, n2, &out));
    TEST_ASSERT_EQUAL_UINT(0u, out.data_len);
    TEST_ASSERT_NULL(out.data);

    /* "unsupported" response: sub 00, data[0] names the refused sub-command */
    const uint8_t refused[] = {ZW_SETUP_SET_RF_REGION};
    zw_api_api_setup_rsp_t rsp = {.sub = ZW_SETUP_UNSUPPORTED, .data = refused, .data_len = 1u};
    const size_t n3 = zw_api_api_setup_rsp_build(buf, sizeof(buf), &rsp);
    zw_api_api_setup_rsp_t rout;
    TEST_ASSERT_TRUE(zw_api_api_setup_rsp_parse(buf, n3, &rout));
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_UNSUPPORTED, rout.sub);
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_RF_REGION, rout.data[0]);
}

static void test_setup_unsupported_helper(void)
{
    const uint8_t refused[] = {ZW_SETUP_SET_RF_REGION};
    zw_api_api_setup_rsp_t rsp = {.sub = ZW_SETUP_UNSUPPORTED, .data = refused, .data_len = 1u};
    uint8_t sub = 0u;
    TEST_ASSERT_TRUE(zw_api_setup_unsupported(&rsp, &sub));
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_RF_REGION, sub);

    zw_api_api_setup_rsp_t ok = {.sub = ZW_SETUP_SET_RF_REGION, .data = NULL, .data_len = 0u};
    TEST_ASSERT_FALSE(zw_api_setup_unsupported(&ok, &sub)); /* not an "unsupported" response */

    zw_api_api_setup_rsp_t empty = {.sub = ZW_SETUP_UNSUPPORTED, .data = NULL, .data_len = 0u};
    TEST_ASSERT_FALSE(zw_api_setup_unsupported(&empty, &sub)); /* no refused sub-command given */
}

static void test_setup_supported_cmds(void)
{
    /* legacy mask: SetTxStatusReport (02) supported, GetPowerlevel (08) not */
    const uint8_t legacy = ZW_SETUP_SET_TX_STATUS_REPORT;
    /* extended mask: bit n of byte b = sub-command 8b+n+1; GetRegionInfo is 0x16 = 22,
     * so b=(22-1)/8=2, n=(22-1)%8=5 */
    uint8_t ext[3] = {0};
    ext[2] = (uint8_t)(1u << 5);
    uint8_t data[4];
    data[0] = legacy;
    data[1] = ext[0];
    data[2] = ext[1];
    data[3] = ext[2];
    zw_api_api_setup_rsp_t rsp = {.sub = ZW_SETUP_GET_SUPPORTED_COMMANDS, .data = data, .data_len = 4u};

    zw_api_setup_supported_cmds_t cmds;
    TEST_ASSERT_TRUE(zw_api_setup_supported_cmds_parse(&rsp, &cmds));
    TEST_ASSERT_EQUAL_HEX8(legacy, cmds.legacy_mask);
    TEST_ASSERT_EQUAL_UINT(3u, cmds.extended_mask_len);

    TEST_ASSERT_TRUE(zw_api_setup_supports(&cmds, ZW_SETUP_SET_TX_STATUS_REPORT));  /* legacy bit */
    TEST_ASSERT_FALSE(zw_api_setup_supports(&cmds, ZW_SETUP_GET_POWERLEVEL));       /* legacy, unset */
    TEST_ASSERT_TRUE(zw_api_setup_supports(&cmds, ZW_SETUP_GET_REGION_INFO));       /* extended bit */
    TEST_ASSERT_FALSE(zw_api_setup_supports(&cmds, ZW_SETUP_GET_SUPPORTED_REGIONS)); /* extended, unset */
    TEST_ASSERT_FALSE(zw_api_setup_supports(&cmds, 0u));

    /* legacy mask only, no extended mask at all */
    zw_api_api_setup_rsp_t bare_rsp = {.sub = ZW_SETUP_GET_SUPPORTED_COMMANDS, .data = data, .data_len = 1u};
    zw_api_setup_supported_cmds_t bare;
    TEST_ASSERT_TRUE(zw_api_setup_supported_cmds_parse(&bare_rsp, &bare));
    TEST_ASSERT_NULL(bare.extended_mask);
    TEST_ASSERT_TRUE(zw_api_setup_supports(&bare, ZW_SETUP_SET_TX_STATUS_REPORT));
    TEST_ASSERT_FALSE(zw_api_setup_supports(&bare, ZW_SETUP_GET_REGION_INFO)); /* no extended mask */

    zw_api_api_setup_rsp_t wrong_sub = {.sub = ZW_SETUP_GET_RF_REGION, .data = data, .data_len = 4u};
    TEST_ASSERT_FALSE(zw_api_setup_supported_cmds_parse(&wrong_sub, &cmds));
}

static void test_setup_regions(void)
{
    const uint8_t data[] = {3u, 0x00u, 0x01u, 0x09u}; /* count=3: Europe, USA, USA Long Range */
    zw_api_api_setup_rsp_t rsp = {.sub = ZW_SETUP_GET_SUPPORTED_REGIONS, .data = data, .data_len = sizeof(data)};
    zw_api_setup_regions_t out;
    TEST_ASSERT_TRUE(zw_api_setup_regions_parse(&rsp, &out));
    TEST_ASSERT_EQUAL_UINT(3u, out.count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&data[1], out.regions, 3u);

    /* the count must fit inside what was given */
    zw_api_api_setup_rsp_t short_rsp = {.sub = ZW_SETUP_GET_SUPPORTED_REGIONS, .data = data, .data_len = 3u};
    TEST_ASSERT_FALSE(zw_api_setup_regions_parse(&short_rsp, &out));

    /* trailing bytes beyond the count are accepted and ignored */
    const uint8_t trailing[] = {1u, 0x00u, 0xAAu};
    zw_api_api_setup_rsp_t trail_rsp = {.sub = ZW_SETUP_GET_SUPPORTED_REGIONS, .data = trailing,
                                         .data_len = sizeof(trailing)};
    TEST_ASSERT_TRUE(zw_api_setup_regions_parse(&trail_rsp, &out));
    TEST_ASSERT_EQUAL_UINT(1u, out.count);

    zw_api_api_setup_rsp_t wrong_sub = {.sub = ZW_SETUP_GET_REGION_INFO, .data = data, .data_len = sizeof(data)};
    TEST_ASSERT_FALSE(zw_api_setup_regions_parse(&wrong_sub, &out));
}

static void test_setup_region_info(void)
{
    const uint8_t with_superset[] = {0x09u, 0x03u, 0x01u}; /* USA LR, classic+LR, superset USA */
    zw_api_api_setup_rsp_t rsp = {.sub = ZW_SETUP_GET_REGION_INFO, .data = with_superset,
                                   .data_len = sizeof(with_superset)};
    zw_api_setup_region_info_t out;
    TEST_ASSERT_TRUE(zw_api_setup_region_info_parse(&rsp, &out));
    TEST_ASSERT_EQUAL_HEX8(0x09u, out.region);
    TEST_ASSERT_TRUE(out.supports_classic);
    TEST_ASSERT_TRUE(out.supports_long_range);
    TEST_ASSERT_TRUE(out.has_classic_superset);
    TEST_ASSERT_EQUAL_HEX8(0x01u, out.classic_superset_region);

    const uint8_t no_superset[] = {0x00u, 0x01u}; /* Europe, classic only */
    zw_api_api_setup_rsp_t rsp2 = {.sub = ZW_SETUP_GET_REGION_INFO, .data = no_superset,
                                    .data_len = sizeof(no_superset)};
    TEST_ASSERT_TRUE(zw_api_setup_region_info_parse(&rsp2, &out));
    TEST_ASSERT_FALSE(out.has_classic_superset);
    TEST_ASSERT_FALSE(out.supports_long_range);

    zw_api_api_setup_rsp_t short_rsp = {.sub = ZW_SETUP_GET_REGION_INFO, .data = no_superset, .data_len = 1u};
    TEST_ASSERT_FALSE(zw_api_setup_region_info_parse(&short_rsp, &out));
}

/* ZW-025 (network.md §12 power): tenths of a dBm - signed bytes in 08/04, signed big-endian words
 * in 13/12 and the Long Range maximum 05/03; the calibration goes back unchanged */
static void test_setup_power(void)
{
    zw_api_setup_power_t pw;
    const uint8_t eight[] = {0xF6u, 0x05u}; /* -1.0 dBm, calibration 0.5 dBm */
    const zw_api_api_setup_rsp_t r8 = {.sub = ZW_SETUP_GET_POWERLEVEL, .data = eight, .data_len = 2u};
    TEST_ASSERT_TRUE(zw_api_setup_power_parse(&r8, &pw));
    TEST_ASSERT_EQUAL_INT16(-10, pw.dbm10);
    TEST_ASSERT_EQUAL_INT16(5, pw.cal_dbm10);
    const uint8_t sixteen[] = {0x00u, 0x8Cu, 0xFFu, 0xF6u}; /* 14.0 dBm, calibration -1.0 dBm */
    const zw_api_api_setup_rsp_t r16 = {.sub = ZW_SETUP_GET_POWERLEVEL_16BIT, .data = sixteen,
                                        .data_len = 4u};
    TEST_ASSERT_TRUE(zw_api_setup_power_parse(&r16, &pw));
    TEST_ASSERT_EQUAL_INT16(140, pw.dbm10);
    TEST_ASSERT_EQUAL_INT16(-10, pw.cal_dbm10);
    const zw_api_api_setup_rsp_t short16 = {.sub = ZW_SETUP_GET_POWERLEVEL_16BIT, .data = sixteen,
                                            .data_len = 3u};
    TEST_ASSERT_FALSE(zw_api_setup_power_parse(&short16, &pw));
    const zw_api_api_setup_rsp_t other = {.sub = ZW_SETUP_GET_RF_REGION, .data = sixteen, .data_len = 4u};
    TEST_ASSERT_FALSE(zw_api_setup_power_parse(&other, &pw));

    int16_t lr = 0;
    const uint8_t lr_word[] = {0x00u, 0xC8u}; /* 20.0 dBm */
    const zw_api_api_setup_rsp_t rlr = {.sub = ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER,
                                        .data = lr_word, .data_len = 2u};
    TEST_ASSERT_TRUE(zw_api_setup_lr_power_parse(&rlr, &lr));
    TEST_ASSERT_EQUAL_INT16(200, lr);
    TEST_ASSERT_FALSE(zw_api_setup_lr_power_parse(&r16, &lr));

    uint8_t out[8];
    const zw_api_setup_power_t want = {.dbm10 = 100, .cal_dbm10 = -10};
    static const uint8_t set16[] = {0x12u, 0x00u, 0x64u, 0xFFu, 0xF6u};
    TEST_ASSERT_EQUAL_size_t(5u, zw_api_setup_set_power_req_build(out, sizeof out, true, &want));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(set16, out, 5u);
    static const uint8_t set8[] = {0x04u, 0x64u, 0xF6u};
    TEST_ASSERT_EQUAL_size_t(3u, zw_api_setup_set_power_req_build(out, sizeof out, false, &want));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(set8, out, 3u);
    const zw_api_setup_power_t too_big = {.dbm10 = 140, .cal_dbm10 = 0}; /* 14 dBm needs 16 bits */
    TEST_ASSERT_EQUAL_size_t(0u, zw_api_setup_set_power_req_build(out, sizeof out, false, &too_big));
    static const uint8_t setlr[] = {0x03u, 0x00u, 0x8Cu};
    TEST_ASSERT_EQUAL_size_t(3u, zw_api_setup_set_lr_power_req_build(out, sizeof out, 140));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(setlr, out, 3u);
}

static void test_setup_rsp_u8_and_builders(void)
{
    const uint8_t ok[] = {0x01u};
    zw_api_api_setup_rsp_t rsp = {.sub = ZW_SETUP_SET_RF_REGION, .data = ok, .data_len = 1u};
    uint8_t value = 0u;
    TEST_ASSERT_TRUE(zw_api_setup_rsp_u8(&rsp, ZW_SETUP_SET_RF_REGION, &value));
    TEST_ASSERT_EQUAL_HEX8(0x01u, value);
    TEST_ASSERT_FALSE(zw_api_setup_rsp_u8(&rsp, ZW_SETUP_SET_NODE_ID_TYPE, &value)); /* wrong sub */

    uint8_t buf[8];
    size_t n = zw_api_setup_set_tx_status_report_req_build(buf, sizeof(buf), true);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_TX_STATUS_REPORT, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, buf[1]);

    n = zw_api_setup_get_region_info_req_build(buf, sizeof(buf), 0x09u);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_GET_REGION_INFO, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x09u, buf[1]);

    n = zw_api_setup_set_rf_region_req_build(buf, sizeof(buf), 0x01u);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_RF_REGION, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[1]);

    n = zw_api_setup_set_node_id_type_req_build(buf, sizeof(buf), ZW_NID_16);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(ZW_SETUP_SET_NODE_ID_TYPE, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[1]);
    n = zw_api_setup_set_node_id_type_req_build(buf, sizeof(buf), ZW_NID_8);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[1]);
}

/* ================================================================================================
 * What the transport needs: zw_api_func_info() against serial-api.md §6's table, row by row, and
 * zw_api_callback_id_offset() for every callback-bearing request.
 * ================================================================================================ */

typedef struct {
    uint8_t func;
    bool has_response;
    zw_api_callback_kind_t callback;
    bool required;
    uint8_t phase;
} func_info_expect_t;

static void test_func_info_table(void)
{
    /* serial-api.md §6, transcribed independently of src/zw_api_transport.c's own table. */
    static const func_info_expect_t rows[] = {
        {0x02u, true,  ZW_CB_NONE,           true,  1u},
        {0x03u, false, ZW_CB_NONE,           true,  1u},
        {0x04u, false, ZW_CB_NONE,           false, 1u},
        {0x05u, true,  ZW_CB_NONE,           true,  1u},
        {0x06u, true,  ZW_CB_NONE,           false, 1u},
        {0x07u, true,  ZW_CB_NONE,           true,  1u},
        {0x08u, false, ZW_CB_NONE,           true,  1u},
        {0x09u, true,  ZW_CB_NONE,           true,  1u},
        {0x0Au, false, ZW_CB_NONE,           false, 1u},
        {0x0Bu, true,  ZW_CB_NONE,           true,  1u},
        {0x10u, true,  ZW_CB_NONE,           false, 3u},
        {0x13u, true,  ZW_CB_ONE_FINAL,      true,  1u},
        {0x15u, true,  ZW_CB_NONE,           true,  1u},
        {0x16u, false, ZW_CB_NONE,           true,  1u},
        {0x20u, true,  ZW_CB_NONE,           true,  1u},
        {0x27u, false, ZW_CB_NONE,           false, 3u},
        {0x2Eu, true,  ZW_CB_NONE,           false, 3u},
        {0x3Bu, true,  ZW_CB_NONE,           false, 2u},
        {0x3Du, true,  ZW_CB_NONE,           false, 3u},
        {0x41u, true,  ZW_CB_NONE,           true,  1u},
        {0x42u, false, ZW_CB_ONE_FINAL,      true,  1u},
        {0x46u, true,  ZW_CB_ONE_FINAL,      false, 2u},
        {0x47u, true,  ZW_CB_ONE_FINAL,      false, 2u},
        {0x48u, false, ZW_CB_ONE_FINAL,      false, 2u},
        {0x49u, false, ZW_CB_NONE,           false, 1u},
        {0x4Au, false, ZW_CB_STATUS_REPORTS, true,  1u},
        {0x4Bu, false, ZW_CB_STATUS_REPORTS, true,  1u},
        {0x51u, true,  ZW_CB_ONE_FINAL,      true,  1u},
        {0x54u, true,  ZW_CB_ONE_FINAL,      true,  1u},
        {0x55u, true,  ZW_CB_ONE_FINAL,      false, 2u},
        {0x56u, true,  ZW_CB_NONE,           true,  1u},
        {0x60u, true,  ZW_CB_NONE,           true,  1u},
        {0x61u, true,  ZW_CB_ONE_FINAL,      false, 2u},
        {0x62u, true,  ZW_CB_NONE,           false, 2u},
        {0x63u, true,  ZW_CB_ONE_FINAL,      false, 2u},
        {0x80u, true,  ZW_CB_NONE,           false, 2u},
        {0xA8u, false, ZW_CB_NONE,           false, 1u},
        {0xD9u, true,  ZW_CB_NONE,           false, 3u},
        {0xDAu, true,  ZW_CB_NONE,           false, 3u},
        {0xDBu, true,  ZW_CB_NONE,           false, 3u},
        {0xDCu, true,  ZW_CB_NONE,           false, 3u},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        zw_api_func_info_t out;
        TEST_ASSERT_TRUE(zw_api_func_info(rows[i].func, &out));
        TEST_ASSERT_EQUAL(rows[i].has_response, out.has_response);
        TEST_ASSERT_EQUAL(rows[i].callback, out.callback);
        TEST_ASSERT_EQUAL(rows[i].required, out.required);
        TEST_ASSERT_EQUAL_UINT8(rows[i].phase, out.phase);
    }
    zw_api_func_info_t missing;
    TEST_ASSERT_FALSE(zw_api_func_info(0x00u, &missing));   /* not a function at all */
    TEST_ASSERT_FALSE(zw_api_func_info(0xFFu, &missing));   /* not in §6's table */
}

static void test_callback_id_offset(void)
{
    /* the six phase-1 functions this milestone has codecs for: build a real request with a known
     * callback ID and confirm the offset actually points at it. */
    uint8_t buf[16];
    size_t off;

    zw_api_send_data_req_t sd = {.node_id = 3u, .command = NULL, .command_len = 0u,
                                  .transmit_options = ZW_TX_OPT_DEFAULT, .callback_id = 0x77u};
    size_t n = zw_api_send_data_req_build(buf, sizeof(buf), ZW_NID_8, &sd);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_SEND_DATA, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x77u, buf[off]);

    zw_api_hard_reset_req_t hr = {.callback_id = 0x22u};
    n = zw_api_hard_reset_req_build(buf, sizeof(buf), &hr);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_HARD_RESET, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x22u, buf[off]);

    zw_api_add_node_req_t an = {.mode = ZW_ADD_NODE_TYPE_ANY, .callback_id = 0x33u, .has_smartstart_dsk = false};
    n = zw_api_add_node_req_build(buf, sizeof(buf), &an);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x33u, buf[off]);
    /* the same offset for the SmartStart-DSK variant, a longer payload */
    zw_api_add_node_req_t an_dsk = {.mode = ZW_ADD_NODE_TYPE_SMARTSTART_DSK, .callback_id = 0x44u,
                                     .has_smartstart_dsk = true};
    n = zw_api_add_node_req_build(buf, sizeof(buf), &an_dsk);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x44u, buf[off]);
    TEST_ASSERT_EQUAL_UINT(1u, off);

    zw_api_remove_node_req_t rn = {.mode = ZW_REMOVE_NODE_TYPE_ANY, .callback_id = 0x55u};
    n = zw_api_remove_node_req_build(buf, sizeof(buf), &rn);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_REMOVE_NODE_FROM_NETWORK, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x55u, buf[off]);

    zw_api_assign_suc_return_route_req_t asr = {.node_id = 4u, .callback_id = 0x66u};
    n = zw_api_assign_suc_return_route_req_build(buf, sizeof(buf), ZW_NID_16, &asr);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x66u, buf[off]);

    zw_api_set_suc_node_id_req_t ssn = {.node_id = 1u, .enable_suc = true,
                                         .transmit_options = ZW_TX_OPT_DEFAULT, .is_sis = true,
                                         .callback_id = 0x88u};
    n = zw_api_set_suc_node_id_req_build(buf, sizeof(buf), ZW_NID_8, &ssn);
    TEST_ASSERT_TRUE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_SET_SUC_NODE_ID, buf, n, &off));
    TEST_ASSERT_EQUAL_HEX8(0x88u, buf[off]);

    /* the phase-2 functions (no codec yet): every one of them ends with its callback ID too */
    static const uint8_t phase2_funcs[] = {0x46u, 0x47u, 0x48u, 0x55u, 0x61u, 0x63u};
    for (size_t i = 0; i < sizeof(phase2_funcs) / sizeof(phase2_funcs[0]); i++) {
        uint8_t synth[6] = {1u, 2u, 3u, 4u, 5u, 0x99u};
        for (size_t len = 1u; len <= sizeof(synth); len++) {
            TEST_ASSERT_TRUE(zw_api_callback_id_offset(phase2_funcs[i], synth, len, &off));
            TEST_ASSERT_EQUAL_UINT(len - 1u, off);
        }
    }

    /* a function with no callback ID at all in its request */
    TEST_ASSERT_FALSE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_GET_CONTROLLER_ID, buf, 4u, &off));
    /* too short for AddNode/RemoveNode's fixed byte-1 offset */
    const uint8_t one_byte[] = {ZW_ADD_NODE_TYPE_ANY};
    TEST_ASSERT_FALSE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK, one_byte, 1u, &off));
    /* zero length is never enough for a "last byte" function */
    TEST_ASSERT_FALSE(zw_api_callback_id_offset((uint8_t)ZW_FUNC_SEND_DATA, buf, 0u, &off));
}

/* ================================================================================================
 * The start-up commands
 * ================================================================================================ */

static void test_init_data_rsp(void)
{
    uint8_t node_list[ZW_NODE_LIST_BYTES];
    memset(node_list, 0, sizeof(node_list));
    zw_node_list_set(node_list, 1u, true);
    zw_api_init_data_rsp_t in = {
        .serial_api_version = 10u, .is_controller = true, .has_timers = false,
        .is_secondary = false, .is_sis = true, .node_list = node_list,
        .chip_type = 0x08u, .chip_version = 0x00u,
    };
    uint8_t buf[64];
    const size_t n = zw_api_init_data_rsp_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(34u, n);
    TEST_ASSERT_EQUAL_HEX8(0x08u, buf[1]); /* controller (bit0 clear) + SIS (bit3) */
    TEST_ASSERT_EQUAL_HEX8(0x08u, buf[32]);

    zw_api_init_data_rsp_t out;
    TEST_ASSERT_TRUE(zw_api_init_data_rsp_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(10u, out.serial_api_version);
    TEST_ASSERT_TRUE(out.is_controller);
    TEST_ASSERT_FALSE(out.has_timers);
    TEST_ASSERT_TRUE(out.is_sis);
    TEST_ASSERT_TRUE(zw_node_list_test(out.node_list, 1u));
    TEST_ASSERT_EQUAL_UINT8(0x08u, out.chip_type);

    TEST_ASSERT_FALSE(zw_api_init_data_rsp_parse(buf, n - 1u, &out)); /* one byte short */
    buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_init_data_rsp_parse(buf, n + 1u, &out));
    TEST_ASSERT_EQUAL_UINT8(10u, out.serial_api_version);
    uint8_t bad[64];
    memcpy(bad, buf, n);
    bad[2] = 30u; /* the node-list-length byte must be 29 */
    TEST_ASSERT_FALSE(zw_api_init_data_rsp_parse(bad, n, &out));
}

static void test_app_node_info_req(void)
{
    const uint8_t cc[] = {0x5Eu, 0x9Fu, 0x6Cu, 0x56u, 0x8Fu};
    zw_api_app_node_info_req_t in = {
        .always_listening = true, .generic_class = 0x01u, .specific_class = 0x00u,
        .cc_list = cc, .cc_list_len = sizeof(cc),
    };
    uint8_t buf[64];
    const size_t n = zw_api_app_node_info_req_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(4u + sizeof(cc), n);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[0]);

    zw_api_app_node_info_req_t out;
    TEST_ASSERT_TRUE(zw_api_app_node_info_req_parse(buf, n, &out));
    TEST_ASSERT_TRUE(out.always_listening);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(cc, out.cc_list, sizeof(cc));

    TEST_ASSERT_FALSE(zw_api_app_node_info_req_parse(buf, n - 1u, &out));
    TEST_ASSERT_FALSE(zw_api_app_node_info_req_parse(buf, n + 1u, &out));

    uint8_t big_cc[36];
    memset(big_cc, 0x20u, sizeof(big_cc));
    zw_api_app_node_info_req_t too_big = {.cc_list = big_cc, .cc_list_len = 36u};
    TEST_ASSERT_EQUAL_UINT(0u, zw_api_app_node_info_req_build(buf, sizeof(buf), &too_big)); /* > 35 */
}

static void test_controller_caps_rsp(void)
{
    zw_api_controller_caps_rsp_t in = {
        .is_secondary = false, .on_other_network = false, .sis_present = true,
        .was_real_primary = true, .is_suc = true, .no_nodes_included = false,
    };
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_controller_caps_rsp_build(buf, sizeof(buf), &in));
    TEST_ASSERT_EQUAL_HEX8(0x1Cu, buf[0]); /* SIS(04) | was primary(08) | is SUC(10) */
    zw_api_controller_caps_rsp_t out;
    TEST_ASSERT_TRUE(zw_api_controller_caps_rsp_parse(buf, 1u, &out));
    TEST_ASSERT_TRUE(out.sis_present);
    TEST_ASSERT_TRUE(out.was_real_primary);
    TEST_ASSERT_TRUE(out.is_suc);
    TEST_ASSERT_FALSE(out.is_secondary);
    TEST_ASSERT_FALSE(zw_api_controller_caps_rsp_parse(buf, 0u, &out));
    buf[1] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_controller_caps_rsp_parse(buf, 2u, &out));
    TEST_ASSERT_TRUE(out.sis_present);
}

/* The panel sends A0 0F: 1600 ms ACK timeout, 150 ms byte timeout (serial-api.md §6.6). */
static void test_api_timeouts(void)
{
    zw_api_api_timeouts_req_t in = {.ack_timeout_x10ms = 0xA0u, .byte_timeout_x10ms = 0x0Fu};
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT(2u, zw_api_api_timeouts_req_build(buf, sizeof(buf), &in));
    TEST_ASSERT_EQUAL_HEX8(0xA0u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0Fu, buf[1]);

    zw_api_api_timeouts_req_t out;
    TEST_ASSERT_TRUE(zw_api_api_timeouts_req_parse(buf, 2u, &out));
    TEST_ASSERT_EQUAL_UINT8(0xA0u, out.ack_timeout_x10ms);
    TEST_ASSERT_EQUAL_UINT8(0x0Fu, out.byte_timeout_x10ms);
    TEST_ASSERT_FALSE(zw_api_api_timeouts_req_parse(buf, 1u, &out));
    TEST_ASSERT_FALSE(zw_api_api_timeouts_req_parse(buf, 3u, &out));

    /* the response shares the same shape (the module's old values) */
    zw_api_api_timeouts_rsp_t rout;
    TEST_ASSERT_EQUAL_UINT(2u, zw_api_api_timeouts_rsp_build(buf, sizeof(buf), &in));
    TEST_ASSERT_TRUE(zw_api_api_timeouts_rsp_parse(buf, 2u, &rout));
    TEST_ASSERT_EQUAL_UINT8(0xA0u, rout.ack_timeout_x10ms);
    TEST_ASSERT_FALSE(zw_api_api_timeouts_rsp_parse(buf, 1u, &rout)); /* one byte short */
    buf[2] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_api_timeouts_rsp_parse(buf, 3u, &rout));
    TEST_ASSERT_EQUAL_UINT8(0xA0u, rout.ack_timeout_x10ms);
}

static void test_api_caps_rsp(void)
{
    uint8_t mask[ZW_FUNC_MASK_BYTES];
    memset(mask, 0, sizeof(mask));
    mask[0] = 0xFFu;
    zw_api_api_caps_rsp_t in = {
        .app_version_major = 2u, .app_version_minor = 20u,
        .manufacturer_id = 0x027Au, .product_type = 0x0004u, .product_id = 0x0611u,
        .function_mask = mask,
    };
    uint8_t buf[64];
    const size_t n = zw_api_api_caps_rsp_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(40u, n);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x7Au, buf[3]);

    zw_api_api_caps_rsp_t out;
    TEST_ASSERT_TRUE(zw_api_api_caps_rsp_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT16(0x027Au, out.manufacturer_id);
    TEST_ASSERT_EQUAL_UINT16(0x0004u, out.product_type);
    TEST_ASSERT_EQUAL_UINT16(0x0611u, out.product_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mask, out.function_mask, ZW_FUNC_MASK_BYTES);
    TEST_ASSERT_FALSE(zw_api_api_caps_rsp_parse(buf, n - 1u, &out));
    buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_api_caps_rsp_parse(buf, n + 1u, &out));
    TEST_ASSERT_EQUAL_UINT16(0x027Au, out.manufacturer_id);
}

static void test_protocol_version_rsp(void)
{
    const uint8_t hash[16] = {0};
    zw_api_protocol_version_rsp_t in = {
        .protocol_type = 0u, .major = 7u, .minor = 22u, .patch = 1u,
        .app_framework_build = 0x1234u, .git_hash = hash,
    };
    uint8_t buf[32];
    const size_t n = zw_api_protocol_version_rsp_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(22u, n);
    zw_api_protocol_version_rsp_t out;
    TEST_ASSERT_TRUE(zw_api_protocol_version_rsp_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(7u, out.major);
    TEST_ASSERT_EQUAL_UINT8(22u, out.minor);
    TEST_ASSERT_EQUAL_UINT8(1u, out.patch);
    TEST_ASSERT_EQUAL_UINT16(0x1234u, out.app_framework_build);
    TEST_ASSERT_FALSE(zw_api_protocol_version_rsp_parse(buf, n - 1u, &out));
    buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_protocol_version_rsp_parse(buf, n + 1u, &out));
    TEST_ASSERT_EQUAL_UINT8(7u, out.major);
}

static void test_started(void)
{
    const uint8_t cc[] = {0x5Eu, 0x9Fu};
    zw_api_started_t in = {
        .wake_up_reason = 0x05u, .watchdog_on = false, .always_listening = true,
        .generic_class = 0x01u, .specific_class = 0x00u, .cc_list = cc, .cc_list_len = sizeof(cc),
        .has_supported_protocols = false,
    };
    uint8_t buf[32];
    size_t n = zw_api_started_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(6u + sizeof(cc), n);
    zw_api_started_t out;
    TEST_ASSERT_TRUE(zw_api_started_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(0x05u, out.wake_up_reason);
    TEST_ASSERT_FALSE(out.has_supported_protocols);
    TEST_ASSERT_FALSE(zw_api_started_parse(buf, n - 1u, &out)); /* one byte short */

    /* Trailing tolerance: the byte right after the CC list, if any, is always
     * supported_protocols - the module sent it, so it is read, not just ignored - but anything
     * beyond that one byte is a newer firmware's own business. */
    buf[n] = 0x01u; /* Long Range available */
    TEST_ASSERT_TRUE(zw_api_started_parse(buf, n + 1u, &out));
    TEST_ASSERT_TRUE(out.has_supported_protocols);
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.supported_protocols);
    buf[n + 1u] = 0xAAu; /* further trailing byte: accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_started_parse(buf, n + 2u, &out));
    TEST_ASSERT_TRUE(out.has_supported_protocols);
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.supported_protocols); /* unchanged by the further byte */

    in.has_supported_protocols = true;
    in.supported_protocols = 0x01u; /* Long Range available */
    n = zw_api_started_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(6u + sizeof(cc) + 1u, n);
    TEST_ASSERT_TRUE(zw_api_started_parse(buf, n, &out));
    TEST_ASSERT_TRUE(out.has_supported_protocols);
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.supported_protocols);
    buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_started_parse(buf, n + 1u, &out));
    TEST_ASSERT_TRUE(out.has_supported_protocols);
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.supported_protocols);
}

static void test_controller_version_rsp(void)
{
    /* serial-api.md §6.4's own example string. */
    static const char lib[] = "Z-Wave 7.22";
    zw_api_controller_version_rsp_t in = {.library = lib, .library_len = strlen(lib), .library_type = 0x01u};
    uint8_t buf[32];
    const size_t n = zw_api_controller_version_rsp_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(strlen(lib) + 2u, n);
    TEST_ASSERT_EQUAL_HEX8(0u, buf[strlen(lib)]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[strlen(lib) + 1u]);

    zw_api_controller_version_rsp_t out;
    TEST_ASSERT_TRUE(zw_api_controller_version_rsp_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT(strlen(lib), out.library_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(lib, out.library, strlen(lib));
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.library_type);

    TEST_ASSERT_FALSE(zw_api_controller_version_rsp_parse(buf, n - 1u, &out)); /* no room for type */
    buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_controller_version_rsp_parse(buf, n + 1u, &out));
    TEST_ASSERT_EQUAL_UINT(strlen(lib), out.library_len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.library_type);
    TEST_ASSERT_FALSE(zw_api_controller_version_rsp_parse(buf, 1u, &out)); /* no NUL at all */

    uint8_t no_nul[4] = {'a', 'b', 'c', 'd'};
    TEST_ASSERT_FALSE(zw_api_controller_version_rsp_parse(no_nul, sizeof(no_nul), &out));

    /* The first NUL always ends the string; the byte right after it is the library type, and
     * anything past that (here "d", 0x01) is trailing data from a newer firmware, ignored. */
    uint8_t early_nul[5] = {'a', 0u, 'c', 'd', 0x01u};
    TEST_ASSERT_TRUE(zw_api_controller_version_rsp_parse(early_nul, sizeof(early_nul), &out));
    TEST_ASSERT_EQUAL_UINT(1u, out.library_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("a", out.library, 1u);
    TEST_ASSERT_EQUAL_UINT8('c', out.library_type);
}

static void test_controller_id_rsp(void)
{
    const uint8_t home[4] = {0xC0u, 0xFFu, 0xEEu, 0x01u};
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_controller_id_rsp_t in;
        memcpy(in.home_id, home, 4u);
        in.own_node_id = w == ZW_NID_8 ? 1u : 300u;
        uint8_t buf[8];
        const size_t n = zw_api_controller_id_rsp_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT(4u + (size_t)w, n);
        zw_api_controller_id_rsp_t out;
        TEST_ASSERT_TRUE(zw_api_controller_id_rsp_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(home, out.home_id, 4u);
        TEST_ASSERT_EQUAL_UINT16(in.own_node_id, out.own_node_id);
        TEST_ASSERT_FALSE(zw_api_controller_id_rsp_parse(buf, n - 1u, w, &out));
        buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_controller_id_rsp_parse(buf, n + 1u, w, &out));
        TEST_ASSERT_EQUAL_UINT16(in.own_node_id, out.own_node_id);
    }
}

static void test_get_suc_node_id_rsp(void)
{
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_get_suc_node_id_rsp_t in = {.node_id = w == ZW_NID_8 ? 0u : 300u};
        uint8_t buf[4];
        const size_t n = zw_api_get_suc_node_id_rsp_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w, n);
        zw_api_get_suc_node_id_rsp_t out;
        TEST_ASSERT_TRUE(zw_api_get_suc_node_id_rsp_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT16(in.node_id, out.node_id);
        buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_get_suc_node_id_rsp_parse(buf, n + 1u, w, &out));
        TEST_ASSERT_EQUAL_UINT16(in.node_id, out.node_id);
    }
}

static void test_set_suc_node_id(void)
{
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_set_suc_node_id_req_t in = {
            .node_id = 1u, .enable_suc = true, .transmit_options = ZW_TX_OPT_DEFAULT,
            .is_sis = true, .callback_id = 7u,
        };
        uint8_t buf[8];
        const size_t n = zw_api_set_suc_node_id_req_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w + 4u, n);
        zw_api_set_suc_node_id_req_t out;
        TEST_ASSERT_TRUE(zw_api_set_suc_node_id_req_parse(buf, n, w, &out));
        TEST_ASSERT_TRUE(out.enable_suc);
        TEST_ASSERT_EQUAL_HEX8(ZW_TX_OPT_DEFAULT, out.transmit_options);
        TEST_ASSERT_TRUE(out.is_sis);
        TEST_ASSERT_EQUAL_UINT8(7u, out.callback_id);
        TEST_ASSERT_FALSE(zw_api_set_suc_node_id_req_parse(buf, n - 1u, w, &out));
        TEST_ASSERT_FALSE(zw_api_set_suc_node_id_req_parse(buf, n + 1u, w, &out));
    }
    uint8_t buf[4];
    zw_api_set_suc_node_id_rsp_t rin = {.started = true};
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_set_suc_node_id_rsp_build(buf, sizeof(buf), &rin));
    zw_api_set_suc_node_id_rsp_t rout;
    TEST_ASSERT_TRUE(zw_api_set_suc_node_id_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_TRUE(rout.started);
    buf[0] = 0x02u; /* §6.9: anything but 00 means started */
    TEST_ASSERT_TRUE(zw_api_set_suc_node_id_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_TRUE(rout.started);
    buf[0] = 0x00u;
    TEST_ASSERT_TRUE(zw_api_set_suc_node_id_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_FALSE(rout.started);

    zw_api_set_suc_node_id_cb_t cin = {.callback_id = 3u, .status = ZW_SUC_STATUS_SUCCEEDED};
    TEST_ASSERT_EQUAL_UINT(2u, zw_api_set_suc_node_id_cb_build(buf, sizeof(buf), &cin));
    zw_api_set_suc_node_id_cb_t cout;
    TEST_ASSERT_TRUE(zw_api_set_suc_node_id_cb_parse(buf, 2u, &cout));
    TEST_ASSERT_EQUAL_UINT8(3u, cout.callback_id);
    TEST_ASSERT_EQUAL_HEX8(ZW_SUC_STATUS_SUCCEEDED, cout.status);
    TEST_ASSERT_FALSE(zw_api_set_suc_node_id_cb_parse(buf, 1u, &cout));
}

static void test_assign_suc_return_route(void)
{
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_assign_suc_return_route_req_t in = {.node_id = 4u, .callback_id = 9u};
        uint8_t buf[8];
        const size_t n = zw_api_assign_suc_return_route_req_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w + 1u, n);
        zw_api_assign_suc_return_route_req_t out;
        TEST_ASSERT_TRUE(zw_api_assign_suc_return_route_req_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT16(4u, out.node_id);
        TEST_ASSERT_EQUAL_UINT8(9u, out.callback_id);
        TEST_ASSERT_FALSE(zw_api_assign_suc_return_route_req_parse(buf, n - 1u, w, &out));
        TEST_ASSERT_FALSE(zw_api_assign_suc_return_route_req_parse(buf, n + 1u, w, &out));
    }
    uint8_t buf[4];
    zw_api_assign_suc_return_route_rsp_t rin = {.started = true};
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_assign_suc_return_route_rsp_build(buf, sizeof(buf), &rin));
    zw_api_assign_suc_return_route_rsp_t rout;
    TEST_ASSERT_TRUE(zw_api_assign_suc_return_route_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_TRUE(rout.started);

    zw_api_assign_suc_return_route_cb_t cin = {.callback_id = 1u, .transmit_status = ZW_TX_STATUS_OK};
    TEST_ASSERT_EQUAL_UINT(2u, zw_api_assign_suc_return_route_cb_build(buf, sizeof(buf), &cin));
    zw_api_assign_suc_return_route_cb_t cout;
    TEST_ASSERT_TRUE(zw_api_assign_suc_return_route_cb_parse(buf, 2u, &cout));
    TEST_ASSERT_EQUAL_HEX8(ZW_TX_STATUS_OK, cout.transmit_status);
}

static void test_node_protocol_info(void)
{
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_node_protocol_info_req_t in = {.node_id = 3u};
        uint8_t buf[4];
        const size_t n = zw_api_node_protocol_info_req_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w, n);
        zw_api_node_protocol_info_req_t out;
        TEST_ASSERT_TRUE(zw_api_node_protocol_info_req_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT16(3u, out.node_id);
    }
    /* 5-byte response: no specific device class */
    zw_api_node_protocol_info_rsp_t in = {
        .always_listening = true, .routing = true, .protocol_version = 3u,
        .flirs = ZW_FLIRS_NONE, .supports_beaming = true, .is_controller_type = false,
        .has_specific_device_class = false, .supports_s0 = true,
        .speed_100k_classic = true, .basic_device_class = 0x03u, .generic_device_class = 0x20u,
    };
    uint8_t buf[8];
    size_t n = zw_api_node_protocol_info_rsp_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    zw_api_node_protocol_info_rsp_t out;
    TEST_ASSERT_TRUE(zw_api_node_protocol_info_rsp_parse(buf, n, &out));
    TEST_ASSERT_TRUE(out.always_listening);
    TEST_ASSERT_FALSE(out.has_specific_device_class);
    TEST_ASSERT_EQUAL_UINT8(3u, out.protocol_version);
    TEST_ASSERT_FALSE(zw_api_node_protocol_info_rsp_parse(buf, 4u, &out)); /* one byte short */
    /* the flag, not the length, decides: with has_specific clear, byte 5 (if any) is trailing */
    buf[5] = 0xAAu;
    TEST_ASSERT_TRUE(zw_api_node_protocol_info_rsp_parse(buf, 6u, &out));
    TEST_ASSERT_FALSE(out.has_specific_device_class);
    TEST_ASSERT_EQUAL_UINT8(0u, out.specific_device_class);

    /* 6-byte response: has a specific device class */
    in.has_specific_device_class = true;
    in.specific_device_class = 0x01u;
    in.flirs = ZW_FLIRS_1000MS;
    n = zw_api_node_protocol_info_rsp_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(6u, n);
    TEST_ASSERT_TRUE(zw_api_node_protocol_info_rsp_parse(buf, n, &out));
    TEST_ASSERT_TRUE(out.has_specific_device_class);
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.specific_device_class);
    TEST_ASSERT_EQUAL_UINT8(ZW_FLIRS_1000MS, out.flirs);
    TEST_ASSERT_FALSE(zw_api_node_protocol_info_rsp_parse(buf, 5u, &out)); /* flag says 6, not 5 */
    buf[6] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_node_protocol_info_rsp_parse(buf, 7u, &out));
    TEST_ASSERT_EQUAL_UINT8(0x01u, out.specific_device_class);
}

static void test_hard_reset(void)
{
    zw_api_hard_reset_req_t in = {.callback_id = 42u};
    uint8_t buf[4];
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_hard_reset_req_build(buf, sizeof(buf), &in));
    zw_api_hard_reset_req_t out;
    TEST_ASSERT_TRUE(zw_api_hard_reset_req_parse(buf, 1u, &out));
    TEST_ASSERT_EQUAL_UINT8(42u, out.callback_id);
    TEST_ASSERT_FALSE(zw_api_hard_reset_req_parse(buf, 2u, &out));

    zw_api_hard_reset_cb_t cin = {.callback_id = 42u};
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_hard_reset_cb_build(buf, sizeof(buf), &cin));
    zw_api_hard_reset_cb_t cout;
    TEST_ASSERT_TRUE(zw_api_hard_reset_cb_parse(buf, 1u, &cout));
    TEST_ASSERT_EQUAL_UINT8(42u, cout.callback_id);
    TEST_ASSERT_FALSE(zw_api_hard_reset_cb_parse(buf, 0u, &cout)); /* one byte short */
    buf[1] = 0xAAu; /* the module's callback: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_hard_reset_cb_parse(buf, 2u, &cout));
    TEST_ASSERT_EQUAL_UINT8(42u, cout.callback_id);
}

static void test_request_node_info(void)
{
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_request_node_info_req_t in = {.node_id = 8u};
        uint8_t buf[4];
        const size_t n = zw_api_request_node_info_req_build(buf, sizeof(buf), w, &in);
        zw_api_request_node_info_req_t out;
        TEST_ASSERT_TRUE(zw_api_request_node_info_req_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT16(8u, out.node_id);
    }
    uint8_t buf[4];
    zw_api_request_node_info_rsp_t rin = {.sent = true};
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_request_node_info_rsp_build(buf, sizeof(buf), &rin));
    zw_api_request_node_info_rsp_t rout;
    TEST_ASSERT_TRUE(zw_api_request_node_info_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_TRUE(rout.sent);
    buf[1] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_request_node_info_rsp_parse(buf, 2u, &rout));
    TEST_ASSERT_TRUE(rout.sent);
}

static void test_lr_nodes(void)
{
    zw_api_lr_nodes_req_t in = {.segment = 2u};
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_lr_nodes_req_build(buf, sizeof(buf), &in));
    zw_api_lr_nodes_req_t out;
    TEST_ASSERT_TRUE(zw_api_lr_nodes_req_parse(buf, 1u, &out));
    TEST_ASSERT_EQUAL_UINT8(2u, out.segment);

    uint8_t list[ZW_LR_SEGMENT_BYTES];
    memset(list, 0, sizeof(list));
    zw_lr_segment_set(list, 2u, 256u + 128u * 2u, true);
    zw_api_lr_nodes_rsp_t rin = {.more_segments = true, .segment = 2u, .node_list = list};
    const size_t n = zw_api_lr_nodes_rsp_build(buf, sizeof(buf), &rin);
    TEST_ASSERT_EQUAL_UINT(3u + ZW_LR_SEGMENT_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)ZW_LR_SEGMENT_BYTES, buf[2]);

    zw_api_lr_nodes_rsp_t rout;
    TEST_ASSERT_TRUE(zw_api_lr_nodes_rsp_parse(buf, n, &rout));
    TEST_ASSERT_TRUE(rout.more_segments);
    TEST_ASSERT_EQUAL_UINT8(2u, rout.segment);
    TEST_ASSERT_TRUE(zw_lr_segment_test(rout.node_list, 2u, 256u + 128u * 2u));
    TEST_ASSERT_FALSE(zw_api_lr_nodes_rsp_parse(buf, n - 1u, &rout));
    buf[n] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_lr_nodes_rsp_parse(buf, n + 1u, &rout));
    TEST_ASSERT_EQUAL_UINT8(2u, rout.segment);

    uint8_t bad_len[32];
    memcpy(bad_len, buf, n);
    bad_len[2] = 15u; /* the length field must read 16 */
    TEST_ASSERT_FALSE(zw_api_lr_nodes_rsp_parse(bad_len, n, &rout));
}

/* ================================================================================================
 * Network traffic and membership
 * ================================================================================================ */

static void test_app_cmd_04(void)
{
    const uint8_t cmd[] = {0x20u, 0x03u}; /* Basic Report, value 3 */
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_app_cmd_t in = {
            .is_bridge = false, .receive_status = 0x00u, .source_node_id = 5u,
            .command = cmd, .command_len = sizeof(cmd), .has_rssi = false,
        };
        uint8_t buf[32];
        size_t n = zw_api_app_cmd_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT(2u + (size_t)w + sizeof(cmd), n);
        zw_api_app_cmd_t out;
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n, w, ZW_FUNC_APPLICATION_COMMAND_HANDLER, &out));
        TEST_ASSERT_FALSE(out.is_bridge);
        TEST_ASSERT_EQUAL_UINT16(5u, out.source_node_id);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(cmd, out.command, sizeof(cmd));
        TEST_ASSERT_FALSE(out.has_rssi);
        TEST_ASSERT_FALSE(zw_api_app_cmd_parse(buf, n - 1u, w, ZW_FUNC_APPLICATION_COMMAND_HANDLER, &out));

        in.has_rssi = true;
        in.rssi = -70;
        n = zw_api_app_cmd_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n, w, ZW_FUNC_APPLICATION_COMMAND_HANDLER, &out));
        TEST_ASSERT_TRUE(out.has_rssi);
        TEST_ASSERT_EQUAL_INT(-70, out.rssi);
        buf[n] = 0xAAu; /* the module's frame: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n + 1u, w, ZW_FUNC_APPLICATION_COMMAND_HANDLER, &out));
        TEST_ASSERT_TRUE(out.has_rssi);
        TEST_ASSERT_EQUAL_INT(-70, out.rssi); /* unchanged by the further byte */
        /* wrong FUNC for this shape: the bridge parser needs two node IDs, so a short 04 frame
         * used as A8 must fail rather than silently misreading. */
        TEST_ASSERT_FALSE(zw_api_app_cmd_parse(buf, n, w, ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &out));
    }
}

static void test_app_cmd_a8(void)
{
    const uint8_t cmd[] = {0x25u, 0x02u}; /* Basic Set-ish */
    const uint8_t mask[] = {0x01u};
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_app_cmd_t in = {
            .is_bridge = true, .receive_status = 0x00u, .destination_node_id = 1u,
            .source_node_id = 6u, .command = cmd, .command_len = sizeof(cmd),
            .multicast_mask = mask, .multicast_mask_len = sizeof(mask),
            .has_rssi = false,
        };
        uint8_t buf[32];
        size_t n = zw_api_app_cmd_build(buf, sizeof(buf), w, &in);
        zw_api_app_cmd_t out;
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n, w, ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &out));
        TEST_ASSERT_TRUE(out.is_bridge);
        TEST_ASSERT_EQUAL_UINT16(1u, out.destination_node_id);
        TEST_ASSERT_EQUAL_UINT16(6u, out.source_node_id);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(cmd, out.command, sizeof(cmd));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(mask, out.multicast_mask, sizeof(mask));
        TEST_ASSERT_FALSE(out.has_rssi);
        /* one byte short of the mandatory part: not just a missing optional RSSI byte */
        TEST_ASSERT_FALSE(zw_api_app_cmd_parse(buf, n - 1u, w, ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &out));

        in.has_rssi = true;
        in.rssi = ZW_RSSI_SATURATED;
        n = zw_api_app_cmd_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n, w, ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &out));
        TEST_ASSERT_TRUE(out.has_rssi);
        TEST_ASSERT_EQUAL_INT(ZW_RSSI_SATURATED, out.rssi);
        buf[n] = 0xAAu; /* the module's frame: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n + 1u, w, ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &out));
        TEST_ASSERT_TRUE(out.has_rssi);
        TEST_ASSERT_EQUAL_INT(ZW_RSSI_SATURATED, out.rssi); /* unchanged by the further byte */
    }
    /* an unrecognised FUNC is refused outright */
    uint8_t buf[4] = {0};
    zw_api_app_cmd_t out;
    TEST_ASSERT_FALSE(zw_api_app_cmd_parse(buf, sizeof(buf), ZW_NID_8, ZW_FUNC_SEND_DATA, &out));
}

static void test_app_update(void)
{
    /* the generic envelope */
    const uint8_t data[] = {0xAAu, 0xBBu};
    zw_api_app_update_t in = {.type = ZW_APP_UPDATE_NODE_INFO_REQUEST_FAILED, .data = data, .data_len = 2u};
    uint8_t buf[32];
    const size_t n = zw_api_app_update_build(buf, sizeof(buf), &in);
    TEST_ASSERT_EQUAL_UINT(3u, n);
    zw_api_app_update_t out;
    TEST_ASSERT_TRUE(zw_api_app_update_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_HEX8(ZW_APP_UPDATE_NODE_INFO_REQUEST_FAILED, out.type);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out.data, 2u);
    TEST_ASSERT_FALSE(zw_api_app_update_parse(buf, 0u, &out)); /* needs at least the type byte */

    /* a type with no data at all (82/81/80) */
    zw_api_app_update_t bare = {.type = ZW_APP_UPDATE_NODE_INFO_REQUEST_DONE, .data = NULL, .data_len = 0u};
    const size_t n2 = zw_api_app_update_build(buf, sizeof(buf), &bare);
    TEST_ASSERT_EQUAL_UINT(1u, n2);
    TEST_ASSERT_TRUE(zw_api_app_update_parse(buf, n2, &out));
    TEST_ASSERT_EQUAL_UINT(0u, out.data_len);

    /* type 84 (node info received): the node_update shape, decoded from 'data' */
    const uint8_t cc[] = {0x20u};
    zw_api_node_update_t node = {.node_id = 9u, .basic_class = 4u, .generic_class = 0x20u,
                                  .specific_class = 1u, .cc_list = cc, .cc_list_len = 1u};
    uint8_t node_buf[16];
    const size_t node_n = zw_api_node_update_build(node_buf, sizeof(node_buf), ZW_NID_8, &node);
    zw_api_app_update_t nin = {.type = ZW_APP_UPDATE_NODE_INFO_RECEIVED, .data = node_buf, .data_len = node_n};
    const size_t n3 = zw_api_app_update_build(buf, sizeof(buf), &nin);
    TEST_ASSERT_TRUE(zw_api_app_update_parse(buf, n3, &out));
    TEST_ASSERT_EQUAL_HEX8(ZW_APP_UPDATE_NODE_INFO_RECEIVED, out.type);
    zw_api_node_update_t node_out;
    TEST_ASSERT_TRUE(zw_api_node_update_parse(out.data, out.data_len, ZW_NID_8, &node_out));
    TEST_ASSERT_EQUAL_UINT16(9u, node_out.node_id);

    /* type 85 (SmartStart classic join): the smartstart_join shape */
    zw_api_smartstart_join_t ss = {.node_id = 0u, .receive_status = 1u, .basic_class = 4u,
                                    .generic_class = 0x20u, .specific_class = 1u,
                                    .cc_list = cc, .cc_list_len = 1u};
    memset(ss.nwi_home_id, 0xAAu, 4u);
    uint8_t ss_buf[24];
    const size_t ss_n = zw_api_smartstart_join_build(ss_buf, sizeof(ss_buf), ZW_NID_8, &ss);
    zw_api_app_update_t sin = {.type = ZW_APP_UPDATE_SMARTSTART_CLASSIC, .data = ss_buf, .data_len = ss_n};
    const size_t n4 = zw_api_app_update_build(buf, sizeof(buf), &sin);
    TEST_ASSERT_TRUE(zw_api_app_update_parse(buf, n4, &out));
    zw_api_smartstart_join_t ss_out;
    TEST_ASSERT_TRUE(zw_api_smartstart_join_parse(out.data, out.data_len, ZW_NID_8, &ss_out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ss.nwi_home_id, ss_out.nwi_home_id, 4u);
}

/* the panel always sends high power + network-wide: start C1, stop C5 (§6.16); SmartStart listen
 * 49 (network-wide only). */
static void test_add_node(void)
{
    zw_api_add_node_req_t start = {.mode = ZW_ADD_NODE_FLAG_HIGH_POWER | ZW_ADD_NODE_FLAG_NETWORK_WIDE |
                                            ZW_ADD_NODE_TYPE_ANY,
                                    .callback_id = 1u, .has_smartstart_dsk = false};
    TEST_ASSERT_EQUAL_HEX8(0xC1u, start.mode);
    zw_api_add_node_req_t stop = {.mode = ZW_ADD_NODE_FLAG_HIGH_POWER | ZW_ADD_NODE_FLAG_NETWORK_WIDE |
                                           ZW_ADD_NODE_TYPE_STOP,
                                   .callback_id = 0u};
    TEST_ASSERT_EQUAL_HEX8(0xC5u, stop.mode);
    zw_api_add_node_req_t listen = {.mode = ZW_ADD_NODE_FLAG_NETWORK_WIDE | ZW_ADD_NODE_TYPE_SMARTSTART_LISTEN,
                                     .callback_id = 0u};
    TEST_ASSERT_EQUAL_HEX8(0x49u, listen.mode);

    uint8_t buf[16];
    size_t n = zw_api_add_node_req_build(buf, sizeof(buf), &start);
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_HEX8(0xC1u, buf[0]);
    zw_api_add_node_req_t out;
    TEST_ASSERT_TRUE(zw_api_add_node_req_parse(buf, n, &out));
    TEST_ASSERT_EQUAL_HEX8(0xC1u, out.mode);
    TEST_ASSERT_FALSE(out.has_smartstart_dsk);
    TEST_ASSERT_FALSE(zw_api_add_node_req_parse(buf, n + 1u, &out));

    /* the SmartStart DSK variant */
    zw_api_add_node_req_t dsk = {.mode = ZW_ADD_NODE_TYPE_SMARTSTART_DSK, .callback_id = 2u,
                                  .has_smartstart_dsk = true};
    memset(dsk.nwi_home_id, 0x11u, 4u);
    memset(dsk.auth_home_id, 0x22u, 4u);
    n = zw_api_add_node_req_build(buf, sizeof(buf), &dsk);
    TEST_ASSERT_EQUAL_UINT(10u, n);
    TEST_ASSERT_TRUE(zw_api_add_node_req_parse(buf, n, &out));
    TEST_ASSERT_TRUE(out.has_smartstart_dsk);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(dsk.nwi_home_id, out.nwi_home_id, 4u);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(dsk.auth_home_id, out.auth_home_id, 4u);
    TEST_ASSERT_FALSE(zw_api_add_node_req_parse(buf, n - 1u, &out));
    TEST_ASSERT_FALSE(zw_api_add_node_req_parse(buf, n + 1u, &out));
    /* mode/flag mismatch is refused at build time */
    zw_api_add_node_req_t mismatch = {.mode = ZW_ADD_NODE_TYPE_ANY, .has_smartstart_dsk = true};
    TEST_ASSERT_EQUAL_UINT(0u, zw_api_add_node_req_build(buf, sizeof(buf), &mismatch));

    /* Status reports: `callback ID | status | source node ID (nid) | L | node info`, the same
     * layout for every status (Silicon Labs' actual Serial API, not serial-api.md's now-corrected
     * "context -" rows). A real "Ready" arrives as `cb 01 00 00`. */
    const uint8_t ready_wire[] = {0x05u, ZW_ADD_NODE_STATUS_READY, 0x00u, 0x00u};
    zw_api_add_node_cb_t ready_out;
    TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(ready_wire, sizeof(ready_wire), ZW_NID_8, &ready_out));
    TEST_ASSERT_EQUAL_UINT8(0x05u, ready_out.callback_id);
    TEST_ASSERT_EQUAL_HEX8(ZW_ADD_NODE_STATUS_READY, ready_out.status);
    TEST_ASSERT_FALSE(ready_out.has_node_info);
    TEST_ASSERT_EQUAL_UINT16(0u, ready_out.node.node_id);

    /* build() always writes the full layout, even for a status with no real node info. */
    zw_api_add_node_cb_t ready_in;
    memset(&ready_in, 0, sizeof(ready_in));
    ready_in.callback_id = 0x05u;
    ready_in.status = ZW_ADD_NODE_STATUS_READY;
    n = zw_api_add_node_cb_build(buf, sizeof(buf), ZW_NID_8, &ready_in);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ready_wire, buf, sizeof(ready_wire));
    TEST_ASSERT_EQUAL_UINT(sizeof(ready_wire), n);

    /* "ends right after the status byte": some firmware, and simple mocks - even for a status
     * that would otherwise need node info. */
    const uint8_t short_ready[] = {0x05u, ZW_ADD_NODE_STATUS_READY};
    TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(short_ready, sizeof(short_ready), ZW_NID_8, &ready_out));
    TEST_ASSERT_FALSE(ready_out.has_node_info);
    const uint8_t short_adding[] = {0x05u, ZW_ADD_NODE_STATUS_ADDING_END_NODE};
    TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(short_adding, sizeof(short_adding), ZW_NID_8, &ready_out));
    TEST_ASSERT_FALSE(ready_out.has_node_info);

    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;

        /* AddingEndNode/AddingController must have real node info once anything follows status */
        const uint8_t ccl[] = {0x20u};
        zw_api_add_node_cb_t adding;
        memset(&adding, 0, sizeof(adding));
        adding.callback_id = 5u;
        adding.status = ZW_ADD_NODE_STATUS_ADDING_END_NODE;
        adding.has_node_info = true;
        adding.node.node_id = 11u;
        adding.node.basic_class = 4u;
        adding.node.generic_class = 0x20u;
        adding.node.specific_class = 1u;
        adding.node.cc_list = ccl;
        adding.node.cc_list_len = 1u;
        const size_t an = zw_api_add_node_cb_build(buf, sizeof(buf), w, &adding);
        zw_api_add_node_cb_t aout;
        TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(buf, an, w, &aout));
        TEST_ASSERT_TRUE(aout.has_node_info);
        TEST_ASSERT_EQUAL_UINT16(11u, aout.node.node_id);
        TEST_ASSERT_EQUAL_UINT(1u, aout.node.cc_list_len);
        TEST_ASSERT_FALSE(zw_api_add_node_cb_parse(buf, an - 1u, w, &aout)); /* one byte short */
        buf[an] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(buf, an + 1u, w, &aout));
        TEST_ASSERT_EQUAL_UINT16(11u, aout.node.node_id);

        /* AddingEndNode with node ID + L=0 (explicit "no info") is refused once anything follows
         * the status byte: a real report always has node info there. */
        zw_api_add_node_cb_t no_info = adding;
        no_info.has_node_info = false;
        no_info.node.cc_list = NULL;
        no_info.node.cc_list_len = 0u;
        const size_t ni_n = zw_api_add_node_cb_build(buf, sizeof(buf), w, &no_info);
        zw_api_add_node_cb_t ni_out;
        TEST_ASSERT_FALSE(zw_api_add_node_cb_parse(buf, ni_n, w, &ni_out));

        /* Done carries the node ID; node info is not required */
        zw_api_add_node_cb_t done;
        memset(&done, 0, sizeof(done));
        done.callback_id = 5u;
        done.status = ZW_ADD_NODE_STATUS_DONE;
        done.node.node_id = 11u;
        const size_t dn = zw_api_add_node_cb_build(buf, sizeof(buf), w, &done);
        zw_api_add_node_cb_t dout;
        TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(buf, dn, w, &dout));
        TEST_ASSERT_EQUAL_UINT16(11u, dout.node.node_id);
        TEST_ASSERT_FALSE(dout.has_node_info);
        TEST_ASSERT_FALSE(zw_api_add_node_cb_parse(buf, dn - 1u, w, &dout));
        buf[dn] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(buf, dn + 1u, w, &dout));
        TEST_ASSERT_EQUAL_UINT16(11u, dout.node.node_id);
    }

    /* a malformed internal length field (L=1, too short to be real node info) is refused */
    const uint8_t bad_l[] = {5u, ZW_ADD_NODE_STATUS_READY, 0u, 1u};
    zw_api_add_node_cb_t bad_out;
    TEST_ASSERT_FALSE(zw_api_add_node_cb_parse(bad_l, sizeof(bad_l), ZW_NID_8, &bad_out));

    /* an unrecognised status with nothing else to interpret still parses */
    const uint8_t unknown[] = {5u, 0x99u};
    zw_api_add_node_cb_t uout;
    TEST_ASSERT_TRUE(zw_api_add_node_cb_parse(unknown, sizeof(unknown), ZW_NID_8, &uout));
    TEST_ASSERT_FALSE(uout.has_node_info);
}

static void test_remove_node(void)
{
    zw_api_remove_node_req_t start = {.mode = 0xC1u, .callback_id = 1u};
    uint8_t buf[16];
    TEST_ASSERT_EQUAL_UINT(2u, zw_api_remove_node_req_build(buf, sizeof(buf), &start));
    zw_api_remove_node_req_t out;
    TEST_ASSERT_TRUE(zw_api_remove_node_req_parse(buf, 2u, &out));
    TEST_ASSERT_EQUAL_HEX8(0xC1u, out.mode);
    TEST_ASSERT_FALSE(zw_api_remove_node_req_parse(buf, 1u, &out));
    TEST_ASSERT_FALSE(zw_api_remove_node_req_parse(buf, 3u, &out));

    /* the same layout as AddNode's callback (zw_api_remove_node_cb_t is that same type) */
    const uint8_t ready_wire[] = {0x02u, ZW_REMOVE_NODE_STATUS_READY, 0x00u, 0x00u};
    zw_api_remove_node_cb_t ready_out;
    TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(ready_wire, sizeof(ready_wire), ZW_NID_8, &ready_out));
    TEST_ASSERT_FALSE(ready_out.has_node_info);

    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;

        zw_api_remove_node_cb_t simple;
        memset(&simple, 0, sizeof(simple));
        simple.callback_id = 2u;
        simple.status = ZW_REMOVE_NODE_STATUS_DONE;
        simple.node.node_id = 9u;
        const size_t sn = zw_api_remove_node_cb_build(buf, sizeof(buf), w, &simple);
        zw_api_remove_node_cb_t sout;
        TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(buf, sn, w, &sout));
        TEST_ASSERT_EQUAL_UINT16(9u, sout.node.node_id);
        TEST_ASSERT_FALSE(zw_api_remove_node_cb_parse(buf, sn - 1u, w, &sout));
        buf[sn] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(buf, sn + 1u, w, &sout));

        /* unlike AddNode, RemoveNode's 03/04 never require node info: node ID + L=0 is fine. */
        zw_api_remove_node_cb_t removing_bare;
        memset(&removing_bare, 0, sizeof(removing_bare));
        removing_bare.callback_id = 2u;
        removing_bare.status = ZW_REMOVE_NODE_STATUS_REMOVING_END_NODE;
        removing_bare.node.node_id = 7u;
        const size_t rbn = zw_api_remove_node_cb_build(buf, sizeof(buf), w, &removing_bare);
        zw_api_remove_node_cb_t rbout;
        TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(buf, rbn, w, &rbout));
        TEST_ASSERT_EQUAL_UINT16(7u, rbout.node.node_id);
        TEST_ASSERT_FALSE(rbout.has_node_info);

        /* RemoveNode's 03/04 also accept real node info, same as AddNode's */
        const uint8_t ccl[] = {0x25u};
        zw_api_remove_node_cb_t removing_full;
        memset(&removing_full, 0, sizeof(removing_full));
        removing_full.callback_id = 2u;
        removing_full.status = ZW_REMOVE_NODE_STATUS_REMOVING_CONTROLLER;
        removing_full.has_node_info = true;
        removing_full.node.node_id = 7u;
        removing_full.node.basic_class = 2u;
        removing_full.node.generic_class = 0x02u;
        removing_full.node.specific_class = 0x01u;
        removing_full.node.cc_list = ccl;
        removing_full.node.cc_list_len = 1u;
        const size_t rn = zw_api_remove_node_cb_build(buf, sizeof(buf), w, &removing_full);
        zw_api_remove_node_cb_t rout;
        TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(buf, rn, w, &rout));
        TEST_ASSERT_TRUE(rout.has_node_info);
        TEST_ASSERT_EQUAL_UINT16(7u, rout.node.node_id);
        TEST_ASSERT_FALSE(zw_api_remove_node_cb_parse(buf, rn - 1u, w, &rout));
        buf[rn] = 0xAAu; /* module-sent data: a trailing byte is accepted and ignored */
        TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(buf, rn + 1u, w, &rout));
    }
    const uint8_t unknown[] = {2u, 0x99u};
    zw_api_remove_node_cb_t uout;
    TEST_ASSERT_TRUE(zw_api_remove_node_cb_parse(unknown, sizeof(unknown), ZW_NID_8, &uout));
    TEST_ASSERT_FALSE(uout.has_node_info);
}

static void test_send_data(void)
{
    const uint8_t cmd[] = {0x25u, 0x01u, 0xFFu}; /* Basic Set 0xFF */
    for (int wi = 0; wi < 2; wi++) {
        const zw_nid_width_t w = wi == 0 ? ZW_NID_8 : ZW_NID_16;
        zw_api_send_data_req_t in = {.node_id = 2u, .command = cmd, .command_len = sizeof(cmd),
                                      .transmit_options = ZW_TX_OPT_DEFAULT, .callback_id = 3u};
        uint8_t buf[32];
        const size_t n = zw_api_send_data_req_build(buf, sizeof(buf), w, &in);
        TEST_ASSERT_EQUAL_UINT((size_t)w + 1u + sizeof(cmd) + 2u, n);
        zw_api_send_data_req_t out;
        TEST_ASSERT_TRUE(zw_api_send_data_req_parse(buf, n, w, &out));
        TEST_ASSERT_EQUAL_UINT16(2u, out.node_id);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(cmd, out.command, sizeof(cmd));
        TEST_ASSERT_EQUAL_HEX8(ZW_TX_OPT_DEFAULT, out.transmit_options);
        TEST_ASSERT_EQUAL_UINT8(3u, out.callback_id);
        TEST_ASSERT_FALSE(zw_api_send_data_req_parse(buf, n - 1u, w, &out));
        TEST_ASSERT_FALSE(zw_api_send_data_req_parse(buf, n + 1u, w, &out));
    }
    uint8_t buf[4];
    zw_api_send_data_rsp_t rin = {.queued = true};
    TEST_ASSERT_EQUAL_UINT(1u, zw_api_send_data_rsp_build(buf, sizeof(buf), &rin));
    zw_api_send_data_rsp_t rout;
    TEST_ASSERT_TRUE(zw_api_send_data_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_TRUE(rout.queued);
    buf[0] = 0xFFu; /* 00 = not queued, anything else = queued */
    TEST_ASSERT_TRUE(zw_api_send_data_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_TRUE(rout.queued);
    buf[0] = 0x00u;
    TEST_ASSERT_TRUE(zw_api_send_data_rsp_parse(buf, 1u, &rout));
    TEST_ASSERT_FALSE(rout.queued);

    /* callback without a TX report (SetTxStatusReport off) */
    zw_api_send_data_cb_t cin;
    memset(&cin, 0, sizeof(cin));
    cin.callback_id = 3u;
    cin.transmit_status = ZW_TX_STATUS_NO_ACK;
    uint8_t cbuf[64];
    memset(cbuf, 0, sizeof(cbuf));
    size_t cn = zw_api_send_data_cb_build(cbuf, sizeof(cbuf), &cin);
    TEST_ASSERT_EQUAL_UINT(2u, cn);
    zw_api_send_data_cb_t cout;
    TEST_ASSERT_TRUE(zw_api_send_data_cb_parse(cbuf, cn, &cout));
    TEST_ASSERT_FALSE(cout.has_tx_report);
    TEST_ASSERT_EQUAL_HEX8(ZW_TX_STATUS_NO_ACK, cout.transmit_status);
    TEST_ASSERT_FALSE(zw_api_send_data_cb_parse(cbuf, 1u, &cout));

    /* callback with a TX report */
    cin.has_tx_report = true;
    cin.transmit_status = ZW_TX_STATUS_OK;
    cin.tx_report.transmit_ticks = 5u;
    cin.tx_report.ack_channel = 1u;
    cn = zw_api_send_data_cb_build(cbuf, sizeof(cbuf), &cin);
    TEST_ASSERT_EQUAL_UINT(2u + 17u, cn);
    TEST_ASSERT_TRUE(zw_api_send_data_cb_parse(cbuf, cn, &cout));
    TEST_ASSERT_TRUE(cout.has_tx_report);
    TEST_ASSERT_EQUAL_UINT16(5u, cout.tx_report.transmit_ticks);
    /* never fail a SendData callback over its TX report: one byte short of a report (16 left
     * after callback_id/transmit_status, below the minimum 17) just means there is no report. */
    TEST_ASSERT_TRUE(zw_api_send_data_cb_parse(cbuf, cn - 1u, &cout));
    TEST_ASSERT_FALSE(cout.has_tx_report);
    /* three trailing bytes past a full report are accepted and ignored */
    TEST_ASSERT_TRUE(zw_api_send_data_cb_parse(cbuf, cn + 3u, &cout));
    TEST_ASSERT_TRUE(cout.has_tx_report);
    TEST_ASSERT_EQUAL_UINT16(5u, cout.tx_report.transmit_ticks);
}

/* ================================================================================================
 * Known-answer frames from real controllers, as Z-Wave JS's tests carry them (commit fac3872):
 * whole Serial API frames, SOF to checksum, through the receive parser and then the codecs.
 * ================================================================================================ */

typedef struct {
    uint8_t type, func;
    size_t len;
    uint8_t payload[ZW_PAYLOAD_MAX];
} kat_frame_t;

/* A capture fed in one read (one timestamp) through `rx`: the frames it reports, copied out. */
static size_t kat_feed(zw_rx_t *rx, const uint8_t *bytes, size_t n, kat_frame_t *out, size_t max)
{
    size_t got = 0u;
    for (size_t i = 0; i < n; i++) {
        if (zw_rx_feed(rx, bytes[i], 0u) == ZW_RX_FRAME) {
            zw_frame_t f;
            TEST_ASSERT_TRUE(zw_rx_frame(rx, &f));
            TEST_ASSERT_TRUE(got < max);
            out[got].type = f.type;
            out[got].func = f.func;
            out[got].len = f.payload_len;
            if (f.payload_len > 0u) {
                memcpy(out[got].payload, f.payload, f.payload_len);
            }
            got++;
        }
    }
    return got;
}

/* ZW-001, ZW-023: a bridge controller's BridgeApplicationCommandHandler with an empty multicast
 * mask and no RSSI byte - the frame ends right after the mask's length byte, the everyday shape of
 * a singlecast report on bridge firmware. Mirrors Z-Wave JS's
 * BridgeApplicationCommandRequest.test.ts ("can be parsed without RSSI", their issue #4335),
 * 011200a80001020a320221340000000000000069: node 2 to node 1, its 10 command bytes intact, no
 * mask, no RSSI; one byte shorter (the mask's length missing) it is refused. */
static void test_known_bridge_frame_without_mask_or_rssi(void)
{
    static const uint8_t RAW[] = {0x01, 0x12, 0x00, 0xA8, 0x00, 0x01, 0x02, 0x0A, 0x32, 0x02,
                                  0x21, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x69};
    static const uint8_t CMD[] = {0x32, 0x02, 0x21, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    zw_rx_t rx;
    zw_rx_init(&rx);
    kat_frame_t f[2];
    TEST_ASSERT_EQUAL_UINT(1u, kat_feed(&rx, RAW, sizeof(RAW), f, 2u)); /* its checksum is good */
    TEST_ASSERT_EQUAL_UINT32(0u, rx.discarded);
    TEST_ASSERT_EQUAL_HEX8(ZW_TYPE_REQUEST, f[0].type);
    TEST_ASSERT_EQUAL_HEX8(ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, f[0].func);
    zw_api_app_cmd_t c;
    TEST_ASSERT_TRUE(zw_api_app_cmd_parse(f[0].payload, f[0].len, ZW_NID_8,
                                          ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &c));
    TEST_ASSERT_TRUE(c.is_bridge);
    TEST_ASSERT_EQUAL_HEX8(0x00u, c.receive_status); /* singlecast, from this network */
    TEST_ASSERT_EQUAL_UINT16(1u, c.destination_node_id);
    TEST_ASSERT_EQUAL_UINT16(2u, c.source_node_id);
    TEST_ASSERT_EQUAL_UINT(sizeof(CMD), c.command_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(CMD, c.command, sizeof(CMD));
    TEST_ASSERT_EQUAL_UINT(0u, c.multicast_mask_len);
    TEST_ASSERT_NULL(c.multicast_mask);
    TEST_ASSERT_FALSE(c.has_rssi);
    TEST_ASSERT_FALSE(zw_api_app_cmd_parse(f[0].payload, f[0].len - 1u, ZW_NID_8,
                                           ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &c));
}

/* ZW-003: two complete frames in one read - a SendData callback and a node's Manufacturer
 * Specific Report, one right after the other. Mirrors Z-Wave JS's receiveMessages.test.ts
 * ("multiple messages in the receive buffer"),
 * 010700130f000002e6010e000400020872050086000200828e: both are reported, each with a good
 * checksum, nothing discarded between them; the callback is ID 0F, delivered, and its two bytes of
 * TX report are too few to be one (serial-api.md §6.12: the callback still counts); the report is
 * node 2's, manufacturer 0086, product type 0002, product 0082. */
static void test_known_two_frames_in_one_read(void)
{
    static const uint8_t RAW[] = {0x01, 0x07, 0x00, 0x13, 0x0F, 0x00, 0x00, 0x02, 0xE6,
                                  0x01, 0x0E, 0x00, 0x04, 0x00, 0x02, 0x08, 0x72, 0x05,
                                  0x00, 0x86, 0x00, 0x02, 0x00, 0x82, 0x8E};
    zw_rx_t rx;
    zw_rx_init(&rx);
    kat_frame_t f[3];
    TEST_ASSERT_EQUAL_UINT(2u, kat_feed(&rx, RAW, sizeof(RAW), f, 3u));
    TEST_ASSERT_EQUAL_UINT32(2u, rx.frames);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.discarded);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.bad_checksum);
    TEST_ASSERT_FALSE(zw_rx_busy(&rx));

    TEST_ASSERT_EQUAL_HEX8(ZW_TYPE_REQUEST, f[0].type);
    TEST_ASSERT_EQUAL_HEX8(ZW_FUNC_SEND_DATA, f[0].func);
    zw_api_send_data_cb_t cb;
    TEST_ASSERT_TRUE(zw_api_send_data_cb_parse(f[0].payload, f[0].len, &cb));
    TEST_ASSERT_EQUAL_HEX8(0x0Fu, cb.callback_id);
    TEST_ASSERT_EQUAL_HEX8(ZW_TX_STATUS_OK, cb.transmit_status);
    TEST_ASSERT_FALSE(cb.has_tx_report);

    TEST_ASSERT_EQUAL_HEX8(ZW_TYPE_REQUEST, f[1].type);
    TEST_ASSERT_EQUAL_HEX8(ZW_FUNC_APPLICATION_COMMAND_HANDLER, f[1].func);
    zw_api_app_cmd_t c;
    TEST_ASSERT_TRUE(zw_api_app_cmd_parse(f[1].payload, f[1].len, ZW_NID_8,
                                          ZW_FUNC_APPLICATION_COMMAND_HANDLER, &c));
    TEST_ASSERT_EQUAL_HEX8(0x00u, c.receive_status);
    TEST_ASSERT_EQUAL_UINT16(2u, c.source_node_id);
    TEST_ASSERT_FALSE(c.has_rssi);
    zw_cc_manufacturer_specific_report_t ms;
    TEST_ASSERT_TRUE(zw_cc_manufacturer_specific_report_parse(c.command, c.command_len, &ms));
    TEST_ASSERT_EQUAL_HEX16(0x0086u, ms.manufacturer_id);
    TEST_ASSERT_EQUAL_HEX16(0x0002u, ms.product_type);
    TEST_ASSERT_EQUAL_HEX16(0x0082u, ms.product_id);
}

/* ZW-023: a frame whose checksum is right but whose ApplicationCommandHandler announces 4 command
 * bytes and carries 2 - a Binary Sensor Report from node 33 missing its value. Mirrors Z-Wave JS's
 * invalidPayloadLog.test.ts, 010800040021043003e5: the frame is delivered (zw_link ACKs it,
 * serial-api.md §1), then its payload is refused - the command length runs past the frame - and
 * the two command bytes alone are no Binary Sensor Report either; nothing is read from it. The
 * parser takes the node's complete report right after. */
static void test_known_truncated_report_dropped(void)
{
    static const uint8_t RAW[] = {0x01, 0x08, 0x00, 0x04, 0x00, 0x21, 0x04, 0x30, 0x03, 0xE5};
    static const uint8_t FULL[] = {0x00, 0x21, 0x03, 0x30, 0x03, 0xFF};
    zw_rx_t rx;
    zw_rx_init(&rx);
    kat_frame_t f[2];
    TEST_ASSERT_EQUAL_UINT(1u, kat_feed(&rx, RAW, sizeof(RAW), f, 2u));
    TEST_ASSERT_EQUAL_HEX8(ZW_FUNC_APPLICATION_COMMAND_HANDLER, f[0].func);
    TEST_ASSERT_EQUAL_UINT(5u, f[0].len);
    zw_api_app_cmd_t c;
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_FALSE(zw_api_app_cmd_parse(f[0].payload, f[0].len, ZW_NID_8,
                                           ZW_FUNC_APPLICATION_COMMAND_HANDLER, &c));
    zw_cc_binary_sensor_report_t bs;
    TEST_ASSERT_FALSE(zw_cc_binary_sensor_report_parse(&f[0].payload[3], 2u, &bs));

    uint8_t next[16];
    const size_t n = zw_frame_build(next, sizeof(next), ZW_TYPE_REQUEST,
                                    ZW_FUNC_APPLICATION_COMMAND_HANDLER, FULL, sizeof(FULL));
    TEST_ASSERT_EQUAL_UINT(1u, kat_feed(&rx, next, n, f, 2u));
    TEST_ASSERT_TRUE(zw_api_app_cmd_parse(f[0].payload, f[0].len, ZW_NID_8,
                                          ZW_FUNC_APPLICATION_COMMAND_HANDLER, &c));
    TEST_ASSERT_EQUAL_UINT16(0x21u, c.source_node_id);
    TEST_ASSERT_TRUE(zw_cc_binary_sensor_report_parse(c.command, c.command_len, &bs));
    TEST_ASSERT_TRUE(bs.detected);
    TEST_ASSERT_EQUAL_UINT32(2u, rx.frames);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.bad_checksum);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_node_id_codec);
    RUN_TEST(test_node_list_helpers);
    RUN_TEST(test_lr_segment_helpers);
    RUN_TEST(test_func_mask);
    RUN_TEST(test_cc_iterator);
    RUN_TEST(test_tx_report);
    RUN_TEST(test_node_update_shape);
    RUN_TEST(test_smartstart_join_shape);
    RUN_TEST(test_api_setup_envelope);
    RUN_TEST(test_setup_unsupported_helper);
    RUN_TEST(test_setup_supported_cmds);
    RUN_TEST(test_setup_regions);
    RUN_TEST(test_setup_region_info);
    RUN_TEST(test_setup_power);
    RUN_TEST(test_setup_rsp_u8_and_builders);
    RUN_TEST(test_func_info_table);
    RUN_TEST(test_callback_id_offset);
    RUN_TEST(test_init_data_rsp);
    RUN_TEST(test_app_node_info_req);
    RUN_TEST(test_controller_caps_rsp);
    RUN_TEST(test_api_timeouts);
    RUN_TEST(test_api_caps_rsp);
    RUN_TEST(test_protocol_version_rsp);
    RUN_TEST(test_started);
    RUN_TEST(test_controller_version_rsp);
    RUN_TEST(test_controller_id_rsp);
    RUN_TEST(test_get_suc_node_id_rsp);
    RUN_TEST(test_set_suc_node_id);
    RUN_TEST(test_assign_suc_return_route);
    RUN_TEST(test_node_protocol_info);
    RUN_TEST(test_hard_reset);
    RUN_TEST(test_request_node_info);
    RUN_TEST(test_lr_nodes);
    RUN_TEST(test_app_cmd_04);
    RUN_TEST(test_app_cmd_a8);
    RUN_TEST(test_app_update);
    RUN_TEST(test_add_node);
    RUN_TEST(test_remove_node);
    RUN_TEST(test_send_data);
    RUN_TEST(test_known_bridge_frame_without_mask_or_rssi);
    RUN_TEST(test_known_two_frames_in_one_read);
    RUN_TEST(test_known_truncated_report_dropped);
    return UNITY_END();
}
