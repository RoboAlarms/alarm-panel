/*
 * test_zw_fuzz.c - the receive parser (components/core/zwave_proto/src/zw_frame.c), every
 * zw_api_*_parse() codec (src/zw_api_*.c) and every zw_cc_*_parse()/zw_cc_unwrap() codec
 * (src/zw_cc_*.c) against random input, run under ASan/UBSan in CI: whatever a corrupted UART
 * line or a hostile module/sensor sends, it must never read or write outside its own buffer. The
 * frame parser must also never report a frame whose checksum does not actually verify - the only
 * fact a Serial API frame carries about itself.
 *
 * Requirements: ZW-023
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_frame.h"
#include "zwave_proto/zw_cc_fwu.h"
#include "zwave_proto/zw_cc_switch.h"
#include "zwave_proto/zw_fwfile.h"
#include "zwave_proto/zw_redact.h"

void setUp(void) {}
void tearDown(void) {}

/* splitmix64, fixed seed: the same stream on every platform, so a failure can be replayed. */
static uint64_t SEED = 0x9E3779B97F4A7C15ULL;

static uint64_t next_u64(void)
{
    uint64_t z = (SEED += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint8_t rnd_byte(void)
{
    return (uint8_t)(next_u64() & 0xFFu);
}

/* Inclusive range [lo, hi]. */
static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(next_u64() % (uint64_t)(hi - lo + 1u));
}

/* Checks that hold for any frame the parser ever hands back, whatever produced it: the checksum
 * it carries genuinely verifies, and its length is the one the format allows. */
static void assert_frame_is_genuine(const zw_frame_t *f)
{
    TEST_ASSERT_EQUAL_UINT(f->payload_len + 5u, f->raw_len);
    TEST_ASSERT_TRUE(f->raw_len <= ZW_FRAME_MAX);
    TEST_ASSERT_EQUAL_UINT(f->payload_len + 3u, f->raw[1]);
    const uint8_t chk = zw_frame_checksum(&f->raw[1], f->raw[1]);
    TEST_ASSERT_EQUAL_HEX8(f->raw[f->raw_len - 1u], chk);
}

/* (a) At least a million random bytes with random gaps: never crashes (ASan/UBSan in CI), and
 * every ZW_RX_FRAME it ever reports is genuine; the 'frames' counter and the events actually seen
 * agree, and so do 'bad_checksum + bad_format' against ZW_RX_BAD_FRAME. */
static void test_random_bytes_never_lie_or_go_out_of_bounds(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    uint64_t now = 0u;
    uint32_t seen_frames = 0u;
    uint32_t seen_bad = 0u;

    for (uint32_t i = 0; i < 1000000u; i++) {
        now += rnd_range(0u, 300u);
        const zw_rx_event_t ev = zw_rx_feed(&rx, rnd_byte(), now);
        if (ev == ZW_RX_FRAME) {
            zw_frame_t f;
            TEST_ASSERT_TRUE(zw_rx_frame(&rx, &f));
            assert_frame_is_genuine(&f);
            seen_frames++;
        } else if (ev == ZW_RX_BAD_FRAME) {
            seen_bad++;
        }
    }
    /* Also exercise the read-timeout entry point with whatever is left partially collected. */
    (void)zw_rx_expire(&rx, now + ZW_BYTE_TIMEOUT_MS + 1u);

    TEST_ASSERT_EQUAL_UINT32(seen_frames, rx.frames);
    TEST_ASSERT_EQUAL_UINT32(seen_bad, rx.bad_checksum + rx.bad_format);
}

/* (b) 10,000 valid frames, each preceded by a run of noise bytes that can never be mistaken for
 * SOF/ACK/NAK/CAN: every one is recovered exactly, in the order sent, with no bad frame ever
 * reported. */
static void test_valid_frames_survive_noise_in_order(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    uint64_t now = 0u;

    for (uint32_t iter = 0u; iter < 10000u; iter++) {
        const uint32_t noise_n = rnd_range(0u, 6u);
        for (uint32_t k = 0u; k < noise_n; k++) {
            uint8_t nb = rnd_byte();
            while (nb == ZW_SOF || nb == ZW_ACK || nb == ZW_NAK || nb == ZW_CAN) {
                nb = rnd_byte();
            }
            now += 1u;
            TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, nb, now));
        }

        const uint8_t type = rnd_byte();
        const uint8_t func = rnd_byte();
        const size_t plen = (size_t)rnd_range(0u, ZW_PAYLOAD_MAX);
        uint8_t payload[ZW_PAYLOAD_MAX];
        for (size_t i = 0; i < plen; i++) {
            payload[i] = rnd_byte();
        }
        uint8_t frame[ZW_FRAME_MAX];
        const size_t flen = zw_frame_build(frame, sizeof(frame), type, func, payload, plen);
        TEST_ASSERT_TRUE(flen > 0u);

        zw_rx_event_t last = ZW_RX_NONE;
        for (size_t i = 0; i < flen; i++) {
            now += 1u;
            last = zw_rx_feed(&rx, frame[i], now);
        }
        TEST_ASSERT_EQUAL(ZW_RX_FRAME, last);

        zw_frame_t view;
        TEST_ASSERT_TRUE(zw_rx_frame(&rx, &view));
        assert_frame_is_genuine(&view);
        TEST_ASSERT_EQUAL_UINT8(type, view.type);
        TEST_ASSERT_EQUAL_UINT8(func, view.func);
        TEST_ASSERT_EQUAL_UINT(plen, view.payload_len);
        if (plen > 0u) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, view.payload, plen);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(10000u, rx.frames);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.bad_checksum);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.bad_format);
}

/* (c) A valid frame with exactly one byte corrupted, fed on its own, followed by a 200 ms idle
 * gap and then a genuinely different valid frame.
 *
 * Why the first half can never produce a false positive, for any corrupted byte position: the
 * parser only reports ZW_RX_FRAME once it has collected as many bytes as its own LEN byte calls
 * for, and only when the checksum over them verifies. Reusing the ORIGINAL alignment (byte 0 of
 * this feed is the frame's own SOF) can only complete once exactly 'flen' bytes have been fed,
 * because a single flipped byte value (guaranteed different from the original at 'idx') changes
 * either the stored CHK or the checksum computed from LEN/TYPE/FUNC/payload, but never both the
 * same way - so that completion is always ZW_RX_BAD_FRAME, never FRAME. A parser that instead
 * resynchronises on a later byte (mistaking a data byte for a new SOF after losing the original
 * one) can only gather a *shorter* frame than 'flen', since only 'flen' bytes are fed in total
 * and some were already consumed before the new SOF - so it can never reach raw_len == flen
 * either. Consequently no ZW_RX_FRAME with raw_len == flen can occur here, corrupted byte or not;
 * this is checked directly rather than left as a probability. */
static void test_one_corrupted_byte_is_never_reported_as_the_original(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    uint64_t now = 0u;

    for (uint32_t iter = 0u; iter < 5000u; iter++) {
        const uint8_t type = rnd_byte();
        const uint8_t func = rnd_byte();
        const size_t plen = (size_t)rnd_range(0u, ZW_PAYLOAD_MAX);
        uint8_t payload[ZW_PAYLOAD_MAX];
        for (size_t i = 0; i < plen; i++) {
            payload[i] = rnd_byte();
        }
        uint8_t frame[ZW_FRAME_MAX];
        const size_t flen = zw_frame_build(frame, sizeof(frame), type, func, payload, plen);
        TEST_ASSERT_TRUE(flen > 0u);

        const size_t idx = (size_t)rnd_range(0u, (uint32_t)(flen - 1u));
        uint8_t corrupt;
        do {
            corrupt = rnd_byte();
        } while (corrupt == frame[idx]);
        frame[idx] = corrupt;

        for (size_t i = 0; i < flen; i++) {
            now += 1u;
            const zw_rx_event_t ev = zw_rx_feed(&rx, frame[i], now);
            if (ev == ZW_RX_FRAME) {
                zw_frame_t view;
                TEST_ASSERT_TRUE(zw_rx_frame(&rx, &view));
                assert_frame_is_genuine(&view); /* whatever it is, it must be self-consistent */
                TEST_ASSERT_NOT_EQUAL(flen, view.raw_len); /* never the original's own length */
            }
        }

        /* Idle it out, corrupted tail included, then prove the parser still works. */
        now += 200u;
        (void)zw_rx_expire(&rx, now);
        TEST_ASSERT_FALSE(zw_rx_busy(&rx));

        const uint8_t other_type = (uint8_t)(type ^ 0xFFu);
        uint8_t good[ZW_FRAME_MAX];
        const size_t glen = zw_frame_build(good, sizeof(good), other_type, func, payload, plen);
        TEST_ASSERT_TRUE(glen > 0u);
        zw_rx_event_t last = ZW_RX_NONE;
        for (size_t i = 0; i < glen; i++) {
            now += 1u;
            last = zw_rx_feed(&rx, good[i], now);
        }
        TEST_ASSERT_EQUAL(ZW_RX_FRAME, last);
        zw_frame_t view;
        TEST_ASSERT_TRUE(zw_rx_frame(&rx, &view));
        TEST_ASSERT_EQUAL_UINT8(other_type, view.type);
        TEST_ASSERT_EQUAL_UINT(plen, view.payload_len);
    }
}

/* ================================================================================================
 * Every zw_api_*_parse() codec (docs/features/36 serial-api.md §2-§6), ZW-023.
 * ================================================================================================ */

/* One wrapper per parser, uniform signature: discards the parsed value, keeps only whether the
 * call crashed (caught by ASan/UBSan) or refused the input. A message that needs a FUNC to pick
 * its shape (app_cmd) gets one wrapper per shape instead of a parameter. */
typedef bool (*api_parse_fn)(const uint8_t *p, size_t len, zw_nid_width_t w);

static bool pw_node_id(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    uint16_t v;
    return zw_api_node_id_parse(p, len, w, &v);
}
static bool pw_tx_report(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_tx_report_t out;
    return zw_api_tx_report_parse(p, len, &out);
}
static bool pw_node_update(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_node_update_t out;
    return zw_api_node_update_parse(p, len, w, &out);
}
static bool pw_smartstart_join(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_smartstart_join_t out;
    return zw_api_smartstart_join_parse(p, len, w, &out);
}
static bool pw_api_setup(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_api_setup_req_t out;
    return zw_api_api_setup_req_parse(p, len, &out);
}
static bool pw_init_data_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_init_data_rsp_t out;
    return zw_api_init_data_rsp_parse(p, len, &out);
}
static bool pw_app_node_info_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_app_node_info_req_t out;
    return zw_api_app_node_info_req_parse(p, len, &out);
}
static bool pw_controller_caps_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_controller_caps_rsp_t out;
    return zw_api_controller_caps_rsp_parse(p, len, &out);
}
static bool pw_api_timeouts(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_api_timeouts_req_t out;
    return zw_api_api_timeouts_req_parse(p, len, &out);
}
static bool pw_api_caps_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_api_caps_rsp_t out;
    return zw_api_api_caps_rsp_parse(p, len, &out);
}
static bool pw_protocol_version_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_protocol_version_rsp_t out;
    return zw_api_protocol_version_rsp_parse(p, len, &out);
}
static bool pw_started(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_started_t out;
    return zw_api_started_parse(p, len, &out);
}
static bool pw_controller_version_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_controller_version_rsp_t out;
    return zw_api_controller_version_rsp_parse(p, len, &out);
}
static bool pw_controller_id_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_controller_id_rsp_t out;
    return zw_api_controller_id_rsp_parse(p, len, w, &out);
}
static bool pw_get_suc_node_id_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_get_suc_node_id_rsp_t out;
    return zw_api_get_suc_node_id_rsp_parse(p, len, w, &out);
}
static bool pw_set_suc_node_id_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_set_suc_node_id_req_t out;
    return zw_api_set_suc_node_id_req_parse(p, len, w, &out);
}
static bool pw_set_suc_node_id_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_set_suc_node_id_rsp_t out;
    return zw_api_set_suc_node_id_rsp_parse(p, len, &out);
}
static bool pw_set_suc_node_id_cb(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_set_suc_node_id_cb_t out;
    return zw_api_set_suc_node_id_cb_parse(p, len, &out);
}
static bool pw_assign_suc_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_assign_suc_return_route_req_t out;
    return zw_api_assign_suc_return_route_req_parse(p, len, w, &out);
}
static bool pw_assign_suc_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_assign_suc_return_route_rsp_t out;
    return zw_api_assign_suc_return_route_rsp_parse(p, len, &out);
}
static bool pw_assign_suc_cb(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_assign_suc_return_route_cb_t out;
    return zw_api_assign_suc_return_route_cb_parse(p, len, &out);
}
static bool pw_node_protocol_info_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_node_protocol_info_req_t out;
    return zw_api_node_protocol_info_req_parse(p, len, w, &out);
}
static bool pw_node_protocol_info_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_node_protocol_info_rsp_t out;
    return zw_api_node_protocol_info_rsp_parse(p, len, &out);
}
static bool pw_hard_reset(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_hard_reset_req_t out;
    return zw_api_hard_reset_req_parse(p, len, &out);
}
static bool pw_request_node_info_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_request_node_info_req_t out;
    return zw_api_request_node_info_req_parse(p, len, w, &out);
}
static bool pw_request_node_info_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_request_node_info_rsp_t out;
    return zw_api_request_node_info_rsp_parse(p, len, &out);
}
static bool pw_lr_nodes_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_lr_nodes_req_t out;
    return zw_api_lr_nodes_req_parse(p, len, &out);
}
static bool pw_lr_nodes_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_lr_nodes_rsp_t out;
    return zw_api_lr_nodes_rsp_parse(p, len, &out);
}
static bool pw_app_cmd_04(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_app_cmd_t out;
    return zw_api_app_cmd_parse(p, len, w, ZW_FUNC_APPLICATION_COMMAND_HANDLER, &out);
}
static bool pw_app_cmd_a8(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_app_cmd_t out;
    return zw_api_app_cmd_parse(p, len, w, ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER, &out);
}
static bool pw_app_update(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_app_update_t out;
    return zw_api_app_update_parse(p, len, &out);
}
static bool pw_add_node_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_add_node_req_t out;
    return zw_api_add_node_req_parse(p, len, &out);
}
static bool pw_add_node_cb(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_add_node_cb_t out;
    return zw_api_add_node_cb_parse(p, len, w, &out);
}
static bool pw_remove_node_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_remove_node_req_t out;
    return zw_api_remove_node_req_parse(p, len, &out);
}
static bool pw_remove_node_cb(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_remove_node_cb_t out;
    return zw_api_remove_node_cb_parse(p, len, w, &out);
}
static bool pw_send_data_req(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    zw_api_send_data_req_t out;
    return zw_api_send_data_req_parse(p, len, w, &out);
}
static bool pw_send_data_rsp(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_send_data_rsp_t out;
    return zw_api_send_data_rsp_parse(p, len, &out);
}
static bool pw_send_data_cb(const uint8_t *p, size_t len, zw_nid_width_t w)
{
    (void)w;
    zw_api_send_data_cb_t out;
    return zw_api_send_data_cb_parse(p, len, &out);
}

static const api_parse_fn API_PARSERS[] = {
    pw_node_id, pw_tx_report, pw_node_update, pw_smartstart_join, pw_api_setup,
    pw_init_data_rsp, pw_app_node_info_req, pw_controller_caps_rsp, pw_api_timeouts,
    pw_api_caps_rsp, pw_protocol_version_rsp, pw_started, pw_controller_version_rsp,
    pw_controller_id_rsp, pw_get_suc_node_id_rsp, pw_set_suc_node_id_req, pw_set_suc_node_id_rsp,
    pw_set_suc_node_id_cb, pw_assign_suc_req, pw_assign_suc_rsp, pw_assign_suc_cb,
    pw_node_protocol_info_req, pw_node_protocol_info_rsp, pw_hard_reset,
    pw_request_node_info_req, pw_request_node_info_rsp, pw_lr_nodes_req, pw_lr_nodes_rsp,
    pw_app_cmd_04, pw_app_cmd_a8, pw_app_update, pw_add_node_req, pw_add_node_cb,
    pw_remove_node_req, pw_remove_node_cb, pw_send_data_req, pw_send_data_rsp, pw_send_data_cb,
};
#define N_API_PARSERS (sizeof(API_PARSERS) / sizeof(API_PARSERS[0]))

/* Every zw_api parser, over random payloads 0..300 bytes long, at both node-ID widths: whatever
 * it decides, it must never read or write outside its own buffer (ASan/UBSan in CI). */
static void test_every_api_parser_survives_random_bytes(void)
{
    uint8_t payload[300];
    for (uint32_t iter = 0; iter < 4000u; iter++) {
        const size_t len = (size_t)rnd_range(0u, 300u);
        for (size_t i = 0; i < len; i++) {
            payload[i] = rnd_byte();
        }
        for (size_t f = 0; f < N_API_PARSERS; f++) {
            (void)API_PARSERS[f](payload, len, ZW_NID_8);
            (void)API_PARSERS[f](payload, len, ZW_NID_16);
        }
    }
}

/* Random but valid messages, built then parsed back: a representative cross-section of the
 * trickiest shapes (variable-length CC lists, optional trailing fields, both node-ID widths, both
 * ApplicationCommandHandler shapes, the SmartStart-DSK variant of AddNode) come back unchanged. */
static void test_random_valid_api_messages_round_trip(void)
{
    uint8_t buf[400];
    for (uint32_t iter = 0; iter < 3000u; iter++) {
        const zw_nid_width_t w = (rnd_byte() & 1u) ? ZW_NID_16 : ZW_NID_8;
        const uint16_t node_max = w == ZW_NID_8 ? 255u : 60000u;
        switch (rnd_range(0u, 8u)) {
        case 0u: { /* the node_update shape */
            uint8_t cc[20];
            const size_t cc_len = (size_t)rnd_range(0u, 20u);
            for (size_t i = 0; i < cc_len; i++) {
                cc[i] = rnd_byte();
            }
            zw_api_node_update_t in = {
                .node_id = (uint16_t)rnd_range(1u, node_max), .basic_class = rnd_byte(),
                .generic_class = rnd_byte(), .specific_class = rnd_byte(),
                .cc_list = cc_len > 0u ? cc : NULL, .cc_list_len = cc_len,
            };
            const size_t n = zw_api_node_update_build(buf, sizeof(buf), w, &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_node_update_t out;
            TEST_ASSERT_TRUE(zw_api_node_update_parse(buf, n, w, &out));
            TEST_ASSERT_EQUAL_UINT16(in.node_id, out.node_id);
            TEST_ASSERT_EQUAL_UINT(cc_len, out.cc_list_len);
            if (cc_len > 0u) {
                TEST_ASSERT_EQUAL_UINT8_ARRAY(cc, out.cc_list, cc_len);
            }
            break;
        }
        case 1u: { /* the TX report, every optional-field combination */
            zw_api_tx_report_t in;
            memset(&in, 0, sizeof(in));
            in.transmit_ticks = (uint16_t)rnd_range(0u, 65535u);
            in.ack_rssi = rnd_byte();
            in.tx_channel = rnd_byte();
            const uint32_t variant = rnd_range(0u, 3u);
            if (variant >= 1u) {
                in.has_last_working_node = true;
                in.last_working_node = rnd_byte();
            }
            if (variant >= 2u) {
                in.has_first_failing_node = true;
                in.first_failing_node = rnd_byte();
            }
            if (variant >= 3u) {
                in.has_long_range = true;
                in.lr_dest_ack_rssi = (int8_t)rnd_byte();
            }
            const size_t n = zw_api_tx_report_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_tx_report_t out;
            TEST_ASSERT_TRUE(zw_api_tx_report_parse(buf, n, &out));
            TEST_ASSERT_EQUAL_UINT16(in.transmit_ticks, out.transmit_ticks);
            TEST_ASSERT_EQUAL(in.has_long_range, out.has_long_range);
            break;
        }
        case 2u: { /* SerialAPIStarted, with or without its optional trailing byte */
            uint8_t cc[30];
            const size_t cc_len = (size_t)rnd_range(0u, 30u);
            for (size_t i = 0; i < cc_len; i++) {
                cc[i] = rnd_byte();
            }
            zw_api_started_t in = {
                .wake_up_reason = rnd_byte(), .watchdog_on = (rnd_byte() & 1u) != 0u,
                .always_listening = (rnd_byte() & 1u) != 0u, .generic_class = rnd_byte(),
                .specific_class = rnd_byte(), .cc_list = cc_len > 0u ? cc : NULL, .cc_list_len = cc_len,
                .has_supported_protocols = (rnd_byte() & 1u) != 0u, .supported_protocols = rnd_byte(),
            };
            const size_t n = zw_api_started_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_started_t out;
            TEST_ASSERT_TRUE(zw_api_started_parse(buf, n, &out));
            TEST_ASSERT_EQUAL_UINT(cc_len, out.cc_list_len);
            TEST_ASSERT_EQUAL(in.has_supported_protocols, out.has_supported_protocols);
            break;
        }
        case 3u: { /* the library-string view of GetControllerVersion */
            char lib[32];
            const size_t lib_len = (size_t)rnd_range(0u, 30u);
            for (size_t i = 0; i < lib_len; i++) {
                lib[i] = (char)('A' + (rnd_byte() % 26u));
            }
            zw_api_controller_version_rsp_t in = {
                .library = lib_len > 0u ? lib : NULL, .library_len = lib_len, .library_type = rnd_byte(),
            };
            const size_t n = zw_api_controller_version_rsp_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_controller_version_rsp_t out;
            TEST_ASSERT_TRUE(zw_api_controller_version_rsp_parse(buf, n, &out));
            TEST_ASSERT_EQUAL_UINT(lib_len, out.library_len);
            if (lib_len > 0u) {
                TEST_ASSERT_EQUAL_UINT8_ARRAY(lib, out.library, lib_len);
            }
            TEST_ASSERT_EQUAL_UINT8(in.library_type, out.library_type);
            break;
        }
        case 4u: { /* SendData's request, a random command payload */
            uint8_t cmd[100];
            const size_t cmd_len = (size_t)rnd_range(0u, 100u);
            for (size_t i = 0; i < cmd_len; i++) {
                cmd[i] = rnd_byte();
            }
            zw_api_send_data_req_t in = {
                .node_id = (uint16_t)rnd_range(1u, node_max), .command = cmd_len > 0u ? cmd : NULL,
                .command_len = cmd_len, .transmit_options = rnd_byte(), .callback_id = rnd_byte(),
            };
            const size_t n = zw_api_send_data_req_build(buf, sizeof(buf), w, &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_send_data_req_t out;
            TEST_ASSERT_TRUE(zw_api_send_data_req_parse(buf, n, w, &out));
            TEST_ASSERT_EQUAL_UINT16(in.node_id, out.node_id);
            TEST_ASSERT_EQUAL_UINT(cmd_len, out.command_len);
            if (cmd_len > 0u) {
                TEST_ASSERT_EQUAL_UINT8_ARRAY(cmd, out.command, cmd_len);
            }
            break;
        }
        case 5u: { /* ApplicationCommandHandler, with or without its optional RSSI byte */
            uint8_t cmd[50];
            const size_t cmd_len = (size_t)rnd_range(0u, 50u);
            for (size_t i = 0; i < cmd_len; i++) {
                cmd[i] = rnd_byte();
            }
            const bool has_rssi = (rnd_byte() & 1u) != 0u;
            zw_api_app_cmd_t in = {
                .is_bridge = false, .receive_status = rnd_byte(),
                .source_node_id = (uint16_t)rnd_range(1u, node_max), .command = cmd_len > 0u ? cmd : NULL,
                .command_len = cmd_len, .has_rssi = has_rssi, .rssi = (int8_t)rnd_byte(),
            };
            const size_t n = zw_api_app_cmd_build(buf, sizeof(buf), w, &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_app_cmd_t out;
            TEST_ASSERT_TRUE(zw_api_app_cmd_parse(buf, n, w, ZW_FUNC_APPLICATION_COMMAND_HANDLER, &out));
            TEST_ASSERT_EQUAL_UINT16(in.source_node_id, out.source_node_id);
            TEST_ASSERT_EQUAL(has_rssi, out.has_rssi);
            break;
        }
        case 6u: { /* GetLongRangeNodes' response */
            uint8_t list[ZW_LR_SEGMENT_BYTES];
            for (size_t i = 0; i < ZW_LR_SEGMENT_BYTES; i++) {
                list[i] = rnd_byte();
            }
            zw_api_lr_nodes_rsp_t in = {.more_segments = (rnd_byte() & 1u) != 0u, .segment = rnd_byte(),
                                         .node_list = list};
            const size_t n = zw_api_lr_nodes_rsp_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_lr_nodes_rsp_t out;
            TEST_ASSERT_TRUE(zw_api_lr_nodes_rsp_parse(buf, n, &out));
            TEST_ASSERT_EQUAL_UINT8(in.segment, out.segment);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(list, out.node_list, ZW_LR_SEGMENT_BYTES);
            break;
        }
        case 7u: { /* AddNodeToNetwork's request, classic and SmartStart-DSK */
            const bool dsk = (rnd_byte() & 1u) != 0u;
            zw_api_add_node_req_t in;
            memset(&in, 0, sizeof(in));
            in.callback_id = rnd_byte();
            in.has_smartstart_dsk = dsk;
            in.mode = dsk ? ZW_ADD_NODE_TYPE_SMARTSTART_DSK : ZW_ADD_NODE_TYPE_ANY;
            if (dsk) {
                for (size_t i = 0; i < 4u; i++) {
                    in.nwi_home_id[i] = rnd_byte();
                    in.auth_home_id[i] = rnd_byte();
                }
            }
            const size_t n = zw_api_add_node_req_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_add_node_req_t out;
            TEST_ASSERT_TRUE(zw_api_add_node_req_parse(buf, n, &out));
            TEST_ASSERT_EQUAL_HEX8(in.mode, out.mode);
            if (dsk) {
                TEST_ASSERT_EQUAL_UINT8_ARRAY(in.nwi_home_id, out.nwi_home_id, 4u);
                TEST_ASSERT_EQUAL_UINT8_ARRAY(in.auth_home_id, out.auth_home_id, 4u);
            }
            break;
        }
        default: { /* GetControllerId's response */
            uint8_t home[4];
            for (size_t i = 0; i < 4u; i++) {
                home[i] = rnd_byte();
            }
            zw_api_controller_id_rsp_t in;
            memcpy(in.home_id, home, 4u);
            in.own_node_id = (uint16_t)rnd_range(0u, node_max);
            const size_t n = zw_api_controller_id_rsp_build(buf, sizeof(buf), w, &in);
            TEST_ASSERT_TRUE(n > 0u);
            zw_api_controller_id_rsp_t out;
            TEST_ASSERT_TRUE(zw_api_controller_id_rsp_parse(buf, n, w, &out));
            TEST_ASSERT_EQUAL_UINT8_ARRAY(home, out.home_id, 4u);
            TEST_ASSERT_EQUAL_UINT16(in.own_node_id, out.own_node_id);
            break;
        }
        }
    }
}

/* ================================================================================================
 * Every zw_cc_*_parse() and zw_cc_unwrap() codec (command-classes.md), ZW-023.
 * ================================================================================================ */

/* Same shape as the API section's pw_* wrappers, but every CC parser takes just (p, len): the CC
 * and command bytes are read from the payload itself, not passed separately. CC_PW0 is for a
 * parser with no output struct (a plain bool), CC_PWS for one that fills an output struct. */
#define CC_PW0(name, fn) \
    static bool name(const uint8_t *p, size_t len) { return fn(p, len); }
#define CC_PWS(name, fn, ty) \
    static bool name(const uint8_t *p, size_t len) { ty out; return fn(p, len, &out); }

/* §4.1-§4.4, §4.6 (zw_cc_basic.h) */
CC_PW0(pwc_nop, zw_cc_nop_parse)
CC_PWS(pwc_basic_set, zw_cc_basic_set_parse, zw_cc_basic_set_t)
CC_PW0(pwc_basic_get, zw_cc_basic_get_parse)
CC_PWS(pwc_basic_report, zw_cc_basic_report_parse, zw_cc_basic_report_t)
CC_PW0(pwc_bsensor_supported_get, zw_cc_binary_sensor_supported_get_parse)
CC_PWS(pwc_bsensor_supported_report, zw_cc_binary_sensor_supported_report_parse,
       zw_cc_binary_sensor_supported_report_t)
CC_PWS(pwc_bsensor_get, zw_cc_binary_sensor_get_parse, zw_cc_binary_sensor_get_t)
CC_PWS(pwc_bsensor_report, zw_cc_binary_sensor_report_parse, zw_cc_binary_sensor_report_t)
CC_PW0(pwc_msensor_supported_sensor_get, zw_cc_multilevel_sensor_supported_sensor_get_parse)
CC_PWS(pwc_msensor_supported_sensor_report, zw_cc_multilevel_sensor_supported_sensor_report_parse,
       zw_cc_multilevel_sensor_supported_sensor_report_t)
CC_PWS(pwc_msensor_supported_scale_get, zw_cc_multilevel_sensor_supported_scale_get_parse,
       zw_cc_multilevel_sensor_supported_scale_get_t)
CC_PWS(pwc_msensor_supported_scale_report, zw_cc_multilevel_sensor_supported_scale_report_parse,
       zw_cc_multilevel_sensor_supported_scale_report_t)
CC_PWS(pwc_msensor_get, zw_cc_multilevel_sensor_get_parse, zw_cc_multilevel_sensor_get_t)
CC_PWS(pwc_msensor_report, zw_cc_multilevel_sensor_report_parse, zw_cc_multilevel_sensor_report_t)
CC_PW0(pwc_battery_get, zw_cc_battery_get_parse)
CC_PWS(pwc_battery_report, zw_cc_battery_report_parse, zw_cc_battery_report_t)

/* §4.5 Notification (zw_cc_notification.h) */
CC_PW0(pwc_notif_supported_get, zw_cc_notification_supported_get_parse)
CC_PWS(pwc_notif_supported_report, zw_cc_notification_supported_report_parse,
       zw_cc_notification_supported_report_t)
CC_PWS(pwc_notif_event_supported_get, zw_cc_notification_event_supported_get_parse,
       zw_cc_notification_event_supported_get_t)
CC_PWS(pwc_notif_event_supported_report, zw_cc_notification_event_supported_report_parse,
       zw_cc_notification_event_supported_report_t)
CC_PWS(pwc_notif_get, zw_cc_notification_get_parse, zw_cc_notification_get_t)
CC_PWS(pwc_notif_set, zw_cc_notification_set_parse, zw_cc_notification_set_t)
CC_PWS(pwc_notif_report, zw_cc_notification_report_parse, zw_cc_notification_report_t)

/* §4.14 Supervision (zw_cc_supervision.h) */
CC_PWS(pwc_supervision_get, zw_cc_supervision_get_parse, zw_cc_supervision_get_t)
CC_PWS(pwc_supervision_report, zw_cc_supervision_report_parse, zw_cc_supervision_report_t)

/* §4.15 Multi Channel (zw_cc_multichannel.h) */
CC_PW0(pwc_mc_end_point_get, zw_cc_multi_channel_end_point_get_parse)
CC_PWS(pwc_mc_end_point_report, zw_cc_multi_channel_end_point_report_parse,
       zw_cc_multi_channel_end_point_report_t)
CC_PWS(pwc_mc_capability_get, zw_cc_multi_channel_capability_get_parse,
       zw_cc_multi_channel_capability_get_t)
CC_PWS(pwc_mc_capability_report, zw_cc_multi_channel_capability_report_parse,
       zw_cc_multi_channel_capability_report_t)
CC_PWS(pwc_mc_command_encapsulation, zw_cc_multi_channel_command_encapsulation_parse,
       zw_cc_multi_channel_command_encapsulation_t)

/* §4.7-§4.13 (zw_cc_network.h) */
CC_PWS(pwc_wake_up_interval_set, zw_cc_wake_up_interval_set_parse, zw_cc_wake_up_interval_set_t)
CC_PW0(pwc_wake_up_interval_get, zw_cc_wake_up_interval_get_parse)
CC_PWS(pwc_wake_up_interval_report, zw_cc_wake_up_interval_report_parse, zw_cc_wake_up_interval_report_t)
CC_PW0(pwc_wake_up_notification, zw_cc_wake_up_notification_parse)
CC_PW0(pwc_wake_up_no_more_information, zw_cc_wake_up_no_more_information_parse)
CC_PW0(pwc_wake_up_interval_capabilities_get, zw_cc_wake_up_interval_capabilities_get_parse)
CC_PWS(pwc_wake_up_interval_capabilities_report, zw_cc_wake_up_interval_capabilities_report_parse,
       zw_cc_wake_up_interval_capabilities_report_t)
CC_PWS(pwc_association_set, zw_cc_association_set_parse, zw_cc_association_set_t)
CC_PWS(pwc_association_get, zw_cc_association_get_parse, zw_cc_association_get_t)
CC_PWS(pwc_association_report, zw_cc_association_report_parse, zw_cc_association_report_t)
CC_PWS(pwc_association_remove, zw_cc_association_remove_parse, zw_cc_association_remove_t)
CC_PW0(pwc_association_supported_groupings_get, zw_cc_association_supported_groupings_get_parse)
CC_PWS(pwc_association_supported_groupings_report, zw_cc_association_supported_groupings_report_parse,
       zw_cc_association_supported_groupings_report_t)
CC_PW0(pwc_association_specific_group_get, zw_cc_association_specific_group_get_parse)
CC_PWS(pwc_association_specific_group_report, zw_cc_association_specific_group_report_parse,
       zw_cc_association_specific_group_report_t)
CC_PWS(pwc_mc_association_set, zw_cc_mc_association_set_parse, zw_cc_mc_association_set_t)
CC_PWS(pwc_mc_association_get, zw_cc_mc_association_get_parse, zw_cc_mc_association_get_t)
CC_PWS(pwc_mc_association_report, zw_cc_mc_association_report_parse, zw_cc_mc_association_report_t)
CC_PWS(pwc_mc_association_remove, zw_cc_mc_association_remove_parse, zw_cc_mc_association_remove_t)
CC_PW0(pwc_mc_association_supported_groupings_get, zw_cc_mc_association_supported_groupings_get_parse)
CC_PWS(pwc_mc_association_supported_groupings_report,
       zw_cc_mc_association_supported_groupings_report_parse,
       zw_cc_mc_association_supported_groupings_report_t)
CC_PWS(pwc_agi_name_get, zw_cc_agi_name_get_parse, zw_cc_agi_name_get_t)
CC_PWS(pwc_agi_name_report, zw_cc_agi_name_report_parse, zw_cc_agi_name_report_t)
CC_PWS(pwc_agi_info_get, zw_cc_agi_info_get_parse, zw_cc_agi_info_get_t)
CC_PWS(pwc_agi_info_report, zw_cc_agi_info_report_parse, zw_cc_agi_info_report_t)
CC_PWS(pwc_agi_command_list_get, zw_cc_agi_command_list_get_parse, zw_cc_agi_command_list_get_t)
CC_PWS(pwc_agi_command_list_report, zw_cc_agi_command_list_report_parse, zw_cc_agi_command_list_report_t)
CC_PW0(pwc_version_get, zw_cc_version_get_parse)
CC_PWS(pwc_version_report, zw_cc_version_report_parse, zw_cc_version_report_t)
CC_PWS(pwc_version_cc_get, zw_cc_version_command_class_get_parse, zw_cc_version_command_class_get_t)
CC_PWS(pwc_version_cc_report, zw_cc_version_command_class_report_parse,
       zw_cc_version_command_class_report_t)
CC_PW0(pwc_version_capabilities_get, zw_cc_version_capabilities_get_parse)
CC_PWS(pwc_version_capabilities_report, zw_cc_version_capabilities_report_parse,
       zw_cc_version_capabilities_report_t)
CC_PW0(pwc_manufacturer_specific_get, zw_cc_manufacturer_specific_get_parse)
CC_PWS(pwc_manufacturer_specific_report, zw_cc_manufacturer_specific_report_parse,
       zw_cc_manufacturer_specific_report_t)
CC_PW0(pwc_zwave_plus_info_get, zw_cc_zwave_plus_info_get_parse)
CC_PWS(pwc_zwave_plus_info_report, zw_cc_zwave_plus_info_report_parse, zw_cc_zwave_plus_info_report_t)

/* §4.18-§4.24 (zw_cc_misc.h) */
CC_PWS(pwc_indicator_set, zw_cc_indicator_set_parse, zw_cc_indicator_set_t)
CC_PWS(pwc_indicator_supported_get, zw_cc_indicator_supported_get_parse, zw_cc_indicator_supported_get_t)
CC_PWS(pwc_indicator_supported_report, zw_cc_indicator_supported_report_parse,
       zw_cc_indicator_supported_report_t)
CC_PW0(pwc_device_reset_locally, zw_cc_device_reset_locally_notification_parse)
CC_PWS(pwc_configuration_set, zw_cc_configuration_set_parse, zw_cc_configuration_set_t)
CC_PWS(pwc_configuration_get, zw_cc_configuration_get_parse, zw_cc_configuration_get_t)
CC_PWS(pwc_configuration_report, zw_cc_configuration_report_parse, zw_cc_configuration_report_t)
CC_PWS(pwc_central_scene, zw_cc_central_scene_notification_parse, zw_cc_central_scene_notification_t)
CC_PWS(pwc_alarm_sensor_get, zw_cc_alarm_sensor_get_parse, zw_cc_alarm_sensor_get_t)
CC_PWS(pwc_alarm_sensor_report, zw_cc_alarm_sensor_report_parse, zw_cc_alarm_sensor_report_t)
CC_PW0(pwc_alarm_sensor_supported_get, zw_cc_alarm_sensor_supported_get_parse)
CC_PWS(pwc_alarm_sensor_supported_report, zw_cc_alarm_sensor_supported_report_parse,
       zw_cc_alarm_sensor_supported_report_t)
CC_PW0(pwc_hail, zw_cc_hail_parse)
CC_PW0(pwc_time_get, zw_cc_time_get_parse)
CC_PWS(pwc_time_report, zw_cc_time_report_parse, zw_cc_time_report_t)
CC_PW0(pwc_date_get, zw_cc_date_get_parse)
CC_PWS(pwc_date_report, zw_cc_date_report_parse, zw_cc_date_report_t)
CC_PWS(pwc_application_busy, zw_cc_application_busy_parse, zw_cc_application_busy_t)
CC_PWS(pwc_application_rejected_request, zw_cc_application_rejected_request_parse,
       zw_cc_application_rejected_request_t)

/* §1 encapsulation (zw_cc_encap.h) */
CC_PWS(pwc_unwrap, zw_cc_unwrap, zw_cc_unwrap_t)

static bool pwc_multi_command(const uint8_t *p, size_t len)
{
    zw_cc_multi_command_iter_t it;
    if (!zw_cc_multi_command_iter_init(&it, p, len)) {
        return false;
    }
    const uint8_t *cmd;
    size_t cmd_len;
    while (zw_cc_multi_command_iter_next(&it, &cmd, &cmd_len)) {
        /* draining only: bounded by it.remaining (a uint8_t), so this always terminates. */
    }
    return true;
}

/* Bonus, beyond one parser per command: the two functions that themselves call into several of
 * the parsers above on attacker-controlled bytes (§5's answer builder, §6's answer matcher). */
static bool pwc_answers(const uint8_t *p, size_t len)
{
    const size_t half = len / 2u;
    return zw_cc_answers(p, half, p + half, len - half);
}

static bool pwc_panel_answer(const uint8_t *p, size_t len)
{
    static const zw_cc_panel_identity_t identity = {
        .protocol_major = 7u, .protocol_minor = 18u, .firmware_major = 1u, .firmware_minor = 0u,
        .manufacturer_id = 0xFFFFu, .product_type = 0xFFFFu, .product_id = 0xFFFFu,
        .zwave_plus_role_type = ZW_ZWAVE_PLUS_ROLE_CENTRAL_STATIC_CONTROLLER,
        .zwave_plus_node_type = ZW_ZWAVE_PLUS_NODE_TYPE_NODE, .installer_icon = 0x0500u,
        .user_icon = 0x0500u, .clock_trusted = true, .time_hour = 12u, .time_minute = 0u,
        .time_second = 0u, .date_year = 2026u, .date_month = 1u, .date_day = 1u,
    };
    uint8_t out[64];
    size_t out_len = 0u;
    (void)zw_cc_panel_answer(p, len, &identity, out, sizeof(out), &out_len);
    return true;
}

typedef bool (*cc_parse_fn)(const uint8_t *p, size_t len);
CC_PWS(pwc_entry_notification, zw_cc_entry_notification_parse, zw_cc_entry_notification_t)
CC_PWS(pwc_entry_keys, zw_cc_entry_keys_parse, zw_cc_entry_keys_t)
CC_PWS(pwc_entry_events, zw_cc_entry_events_parse, zw_cc_entry_events_t)
CC_PWS(pwc_entry_set, zw_cc_entry_configuration_set_parse, zw_cc_entry_configuration_t)
CC_PWS(pwc_entry_report, zw_cc_entry_configuration_report_parse, zw_cc_entry_configuration_t)
CC_PW0(pwc_entry_key_get, zw_cc_entry_key_supported_get_parse)
CC_PW0(pwc_entry_event_get, zw_cc_entry_event_supported_get_parse)
CC_PW0(pwc_entry_config_get, zw_cc_entry_configuration_get_parse)

static const cc_parse_fn CC_PARSERS[] = {
    pwc_entry_notification, pwc_entry_keys, pwc_entry_events, pwc_entry_set, pwc_entry_report,
    pwc_entry_key_get, pwc_entry_event_get, pwc_entry_config_get,
    pwc_nop, pwc_basic_set, pwc_basic_get, pwc_basic_report, pwc_bsensor_supported_get,
    pwc_bsensor_supported_report, pwc_bsensor_get, pwc_bsensor_report, pwc_msensor_supported_sensor_get,
    pwc_msensor_supported_sensor_report, pwc_msensor_supported_scale_get,
    pwc_msensor_supported_scale_report, pwc_msensor_get, pwc_msensor_report, pwc_battery_get,
    pwc_battery_report, pwc_notif_supported_get, pwc_notif_supported_report,
    pwc_notif_event_supported_get, pwc_notif_event_supported_report, pwc_notif_get, pwc_notif_set,
    pwc_notif_report, pwc_supervision_get, pwc_supervision_report, pwc_mc_end_point_get,
    pwc_mc_end_point_report, pwc_mc_capability_get, pwc_mc_capability_report,
    pwc_mc_command_encapsulation, pwc_wake_up_interval_set, pwc_wake_up_interval_get,
    pwc_wake_up_interval_report, pwc_wake_up_notification, pwc_wake_up_no_more_information,
    pwc_wake_up_interval_capabilities_get, pwc_wake_up_interval_capabilities_report,
    pwc_association_set, pwc_association_get, pwc_association_report, pwc_association_remove,
    pwc_association_supported_groupings_get, pwc_association_supported_groupings_report,
    pwc_association_specific_group_get, pwc_association_specific_group_report, pwc_mc_association_set,
    pwc_mc_association_get, pwc_mc_association_report, pwc_mc_association_remove,
    pwc_mc_association_supported_groupings_get, pwc_mc_association_supported_groupings_report,
    pwc_agi_name_get, pwc_agi_name_report, pwc_agi_info_get, pwc_agi_info_report,
    pwc_agi_command_list_get, pwc_agi_command_list_report, pwc_version_get, pwc_version_report,
    pwc_version_cc_get, pwc_version_cc_report, pwc_version_capabilities_get,
    pwc_version_capabilities_report, pwc_manufacturer_specific_get, pwc_manufacturer_specific_report,
    pwc_zwave_plus_info_get, pwc_zwave_plus_info_report, pwc_indicator_set, pwc_indicator_supported_get,
    pwc_indicator_supported_report, pwc_device_reset_locally, pwc_configuration_set,
    pwc_configuration_get, pwc_configuration_report, pwc_central_scene, pwc_alarm_sensor_get,
    pwc_alarm_sensor_report, pwc_alarm_sensor_supported_get, pwc_alarm_sensor_supported_report,
    pwc_hail, pwc_time_get, pwc_time_report, pwc_date_get, pwc_date_report,
    pwc_application_busy, pwc_application_rejected_request,
    pwc_unwrap, pwc_multi_command, pwc_answers, pwc_panel_answer,
};
#define N_CC_PARSERS (sizeof(CC_PARSERS) / sizeof(CC_PARSERS[0]))

/* Every zw_cc parser, zw_cc_unwrap() and the two functions built on top of them, over random
 * payloads 0..300 bytes long: whatever they decide, they must never read or write outside their
 * own buffer (ASan/UBSan in CI). */
static void test_every_cc_parser_survives_random_bytes(void)
{
    uint8_t payload[300];
    for (uint32_t iter = 0; iter < 3000u; iter++) {
        const size_t len = (size_t)rnd_range(0u, 300u);
        for (size_t i = 0; i < len; i++) {
            payload[i] = rnd_byte();
        }
        for (size_t f = 0; f < N_CC_PARSERS; f++) {
            (void)CC_PARSERS[f](payload, len);
        }
    }
}

/* ---- ZW-023: headers forced, truncation sweeps, mutated round-trip output ---------------------
 * Every parser checks its own CC and command byte first, so plain random bytes reach a parser's
 * body about 0.05 times in 3,000 tries. The tests below find, for every parser in CC_PARSERS, the
 * (CC, command) pairs it accepts (probing all 65,536 header pairs against a few body fills), then
 * force exactly those two bytes in front of random, zeroed and 0xFF-run bodies. Each call gets a
 * heap copy exactly as long as the frame, so ASan (CI's Linux build) flags a one-byte over-read. */
#define N_PLAIN (N_CC_PARSERS - 2u) /* the last two (answers, panel_answer) take no single header */
#define MAX_PAIRS 16u
#define N_FILLS 12u
#define PROBE_LEN 64u

typedef struct { uint8_t cc, cmd, fill; } cc_pair_t;
static cc_pair_t g_pairs[N_CC_PARSERS][MAX_PAIRS];
static size_t g_npairs[N_CC_PARSERS];
static bool g_found;

static void fill_body(uint8_t *p, size_t len, uint8_t fill)
{
    uint32_t x = 0x1234567u + (uint32_t)fill * 7919u;
    for (size_t i = 2u; i < len; i++) {
        switch (fill) {
        case 0u: p[i] = 0x00u; break;
        case 1u: p[i] = 0x01u; break;
        case 2u: p[i] = 0x02u; break;
        case 3u: p[i] = 0x03u; break;
        case 4u: p[i] = (uint8_t)(i - 2u); break;
        case 5u: p[i] = (uint8_t)(0x80u | (i & 0x0Fu)); break;
        default:
            x = x * 1664525u + 1013904223u;
            p[i] = (uint8_t)(x >> 24);
            break;
        }
    }
}

/* Calls fn on an exact-size copy of p[0..len). */
static bool call_exact(cc_parse_fn fn, const uint8_t *p, size_t len)
{
    uint8_t *e = (uint8_t *)malloc(len > 0u ? len : 1u);
    TEST_ASSERT_NOT_NULL(e);
    if (len > 0u) {
        memcpy(e, p, len);
    }
    const bool r = fn(e, len);
    free(e);
    return r;
}

static void call_all_exact(const uint8_t *p, size_t len)
{
    uint8_t *e = (uint8_t *)malloc(len > 0u ? len : 1u);
    TEST_ASSERT_NOT_NULL(e);
    if (len > 0u) {
        memcpy(e, p, len);
    }
    for (size_t f = 0; f < N_CC_PARSERS; f++) {
        (void)CC_PARSERS[f](e, len);
    }
    free(e);
}

static void discover_pairs(void)
{
    if (g_found) {
        return;
    }
    uint8_t p[PROBE_LEN];
    for (size_t f = 0; f < N_PLAIN; f++) {
        for (uint8_t fill = 0; fill < N_FILLS && g_npairs[f] < MAX_PAIRS; fill++) {
            memset(p, 0, sizeof p);
            fill_body(p, sizeof p, fill);
            for (uint32_t h = 0u; h < 65536u && g_npairs[f] < MAX_PAIRS; h++) {
                p[0] = (uint8_t)(h >> 8);
                p[1] = (uint8_t)(h & 0xFFu);
                if (!CC_PARSERS[f](p, sizeof p)) {
                    continue;
                }
                bool known = false;
                for (size_t k = 0; k < g_npairs[f]; k++) {
                    known = known || (g_pairs[f][k].cc == p[0] && g_pairs[f][k].cmd == p[1]);
                }
                if (!known) {
                    g_pairs[f][g_npairs[f]].cc = p[0];
                    g_pairs[f][g_npairs[f]].cmd = p[1];
                    g_pairs[f][g_npairs[f]].fill = fill;
                    g_npairs[f]++;
                }
            }
        }
    }
    g_found = true;
}

/* A body for a forced-header call: random, zeroed, a 0xFF run, or all 0xFF. */
static void fuzz_body(uint8_t *p, size_t len, uint32_t iter)
{
    for (size_t i = 2u; i < len; i++) {
        p[i] = rnd_byte();
    }
    if (len > 2u) {
        switch (iter & 3u) {
        case 1u: memset(p + 2, 0, len - 2u); break;
        case 2u: {
            const size_t at = 2u + (size_t)rnd_range(0u, (uint32_t)(len - 3u));
            const size_t run = (size_t)rnd_range(1u, 6u);
            for (size_t i = at; i < len && i < at + run; i++) {
                p[i] = 0xFFu;
            }
            break;
        }
        case 3u: memset(p + 2, 0xFF, len - 2u); break;
        default: break;
        }
    }
}

/* Every parser must accept at least one command, or the forced-header tests would silently skip
 * it. */
static void test_every_cc_parser_has_a_header_to_force(void)
{
    discover_pairs();
    for (size_t f = 0; f < N_PLAIN; f++) {
        char msg[48];
        (void)snprintf(msg, sizeof msg, "CC_PARSERS[%u] accepts no probe frame", (unsigned)f);
        TEST_ASSERT_TRUE_MESSAGE(g_npairs[f] > 0u, msg);
    }
}

/* ZW-023: each parser's own (CC, command) pairs forced over random bodies of 2..120 bytes. */
static void test_every_cc_parser_survives_random_bytes_with_its_header(void)
{
    discover_pairs();
    uint8_t p[130];
    for (size_t f = 0; f < N_PLAIN; f++) {
        for (size_t k = 0; k < g_npairs[f]; k++) {
            for (uint32_t iter = 0; iter < 150u; iter++) {
                const size_t len = (iter % 5u == 0u) ? (size_t)rnd_range(2u, 8u) : (size_t)rnd_range(2u, 120u);
                p[0] = g_pairs[f][k].cc;
                p[1] = g_pairs[f][k].cmd;
                fuzz_body(p, len, iter);
                (void)call_exact(CC_PARSERS[f], p, len);
            }
        }
    }
    /* The two that take no single header: answers() splits the bytes in half and reads a command
     * from each, panel_answer() reads one. Force real headers where each one looks. */
    for (uint32_t iter = 0; iter < 6000u; iter++) {
        const size_t len = (size_t)rnd_range(4u, 120u);
        for (size_t i = 0; i < len; i++) {
            p[i] = rnd_byte();
        }
        const size_t half = len / 2u;
        const size_t a = (size_t)rnd_range(0u, (uint32_t)N_PLAIN - 1u);
        const size_t b = (size_t)rnd_range(0u, (uint32_t)N_PLAIN - 1u);
        if (g_npairs[a] > 0u) {
            const cc_pair_t *pa = &g_pairs[a][rnd_range(0u, (uint32_t)g_npairs[a] - 1u)];
            p[0] = pa->cc;
            p[1] = pa->cmd;
        }
        if (g_npairs[b] > 0u && half + 1u < len) {
            const cc_pair_t *pb = &g_pairs[b][rnd_range(0u, (uint32_t)g_npairs[b] - 1u)];
            p[half] = pb->cc;
            p[half + 1u] = pb->cmd;
        }
        (void)call_exact(pwc_answers, p, len);
        (void)call_exact(pwc_panel_answer, p, len);
    }
}

/* ZW-023: every prefix length of a frame each parser accepts (every length from 0 up to the
 * whole probe frame) on its own parser, and the shortest accepted prefix on all of them. */
static void test_every_cc_parser_survives_every_truncation(void)
{
    discover_pairs();
    uint8_t p[PROBE_LEN];
    for (size_t f = 0; f < N_PLAIN; f++) {
        for (size_t k = 0; k < g_npairs[f]; k++) {
            memset(p, 0, sizeof p);
            fill_body(p, sizeof p, g_pairs[f][k].fill);
            p[0] = g_pairs[f][k].cc;
            p[1] = g_pairs[f][k].cmd;
            bool shortest_done = false;
            for (size_t len = 0; len <= sizeof p; len++) {
                const bool ok = call_exact(CC_PARSERS[f], p, len);
                if (ok && !shortest_done) {
                    shortest_done = true;
                    call_all_exact(p, len);
                    if (len > 0u) {
                        call_all_exact(p, len - 1u);
                    }
                }
            }
            TEST_ASSERT_TRUE(shortest_done);
        }
    }
}

/* ZW-023: a valid frame cut at every length, every byte flipped, its length one more, and 0xFF
 * runs laid over it, each fed to every parser. */
static void mutate_frame(const uint8_t *frame, size_t n)
{
    uint8_t m[420];
    if (n == 0u || n + 2u > sizeof m) {
        return;
    }
    for (size_t len = 0; len <= n; len++) { /* every prefix */
        call_all_exact(frame, len);
    }
    for (size_t i = 0; i < n; i++) { /* a byte flipped: one bit, and the whole byte */
        memcpy(m, frame, n);
        m[i] ^= (uint8_t)(1u << rnd_range(0u, 7u));
        call_all_exact(m, n);
        m[i] = (uint8_t)(frame[i] ^ 0xFFu);
        call_all_exact(m, n);
    }
    memcpy(m, frame, n); /* one longer, with a 0x00 and with a 0xFF after the end */
    m[n] = 0x00u;
    call_all_exact(m, n + 1u);
    m[n] = 0xFFu;
    call_all_exact(m, n + 1u);
    for (size_t k = 0; k < 4u; k++) { /* 0xFF runs */
        memcpy(m, frame, n);
        const size_t at = (size_t)rnd_range(0u, (uint32_t)n - 1u);
        const size_t run = (size_t)rnd_range(1u, 8u);
        for (size_t i = at; i < n && i < at + run; i++) {
            m[i] = 0xFFu;
        }
        call_all_exact(m, n);
    }
}

/* ZW-023: the shortest frame each parser accepts, mutated, so every parser's own valid shape is
 * covered and not only the round-trip test's seven. */
static void test_mutated_valid_frames_survive(void)
{
    discover_pairs();
    uint8_t p[PROBE_LEN];
    for (size_t f = 0; f < N_PLAIN; f++) {
        for (size_t k = 0; k < g_npairs[f] && k < 2u; k++) {
            memset(p, 0, sizeof p);
            fill_body(p, sizeof p, g_pairs[f][k].fill);
            p[0] = g_pairs[f][k].cc;
            p[1] = g_pairs[f][k].cmd;
            size_t len = 2u;
            while (len < sizeof p && !call_exact(CC_PARSERS[f], p, len)) {
                len++;
            }
            mutate_frame(p, len);
        }
    }
}

/* Random but valid command classes and encapsulation layers, built then parsed/unwrapped back: a
 * cross-section of the trickiest shapes (every Multilevel Sensor integer size, Notification's v1
 * vs v2+ split, Indicator's v1 vs v2+ split, and every §1 wrapper including a deliberately broken
 * CRC-16). */
static void test_random_valid_cc_round_trips(void)
{
    uint8_t buf[400];
    for (uint32_t iter = 0; iter < 1500u; iter++) {
        switch (rnd_range(0u, 7u)) {
        case 0u: { /* Multilevel Sensor Report, every signed size */
            const uint8_t sizes[4] = {1u, 2u, 3u, 4u}; /* 3: code review ZW23 */
            const uint8_t size = sizes[rnd_range(0u, 3u)];
            int32_t value;
            switch (size) {
            case 1u: value = (int32_t)(int8_t)rnd_byte(); break;
            case 2u: value = (int32_t)(int16_t)(uint16_t)rnd_range(0u, 65535u); break;
            case 3u: value = (int32_t)rnd_range(0u, 16777215u) - 8388608; break;
            /* a full 32-bit range: rnd_range's (hi - lo + 1) would overflow to 0 and divide by
             * it, so take all 32 bits straight from the generator instead. */
            default: value = (int32_t)(uint32_t)next_u64(); break;
            }
            const zw_cc_multilevel_sensor_report_t in = {
                .sensor_type = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, .precision = (uint8_t)rnd_range(0u, 7u),
                .scale = (uint8_t)rnd_range(0u, 3u), .size = size, .raw_value = value,
            };
            const size_t n = zw_cc_multilevel_sensor_report_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
            zw_cc_multilevel_sensor_report_t out;
            TEST_ASSERT_TRUE(zw_cc_multilevel_sensor_report_parse(buf, n, &out));
            TEST_ASSERT_EQUAL_INT32(in.raw_value, out.raw_value);
            TEST_ASSERT_EQUAL_UINT8(in.precision, out.precision);
            break;
        }
        case 1u: { /* Notification Report: v1-only vs. full v2+ with params and a sequence number */
            const bool is_v1 = (rnd_byte() & 1u) != 0u;
            uint8_t params[10];
            const size_t plen = is_v1 ? 0u : (size_t)rnd_range(0u, 10u);
            for (size_t i = 0; i < plen; i++) {
                params[i] = rnd_byte();
            }
            const zw_cc_notification_report_t in = {
                .is_v1 = is_v1, .v1_alarm_type = rnd_byte(), .v1_alarm_level = rnd_byte(),
                .status = ZW_NOTIFICATION_STATUS_ON, .notification_type = ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                .event = rnd_byte(), .event_params = plen > 0u ? params : NULL, .event_params_len = plen,
                .has_sequence_number = !is_v1 && (rnd_byte() & 1u) != 0u, .sequence_number = rnd_byte(),
            };
            const size_t n = zw_cc_notification_report_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
            zw_cc_notification_report_t out;
            TEST_ASSERT_TRUE(zw_cc_notification_report_parse(buf, n, &out));
            TEST_ASSERT_EQUAL(is_v1, out.is_v1);
            if (!is_v1) {
                TEST_ASSERT_EQUAL_UINT(plen, out.event_params_len);
                TEST_ASSERT_EQUAL(in.has_sequence_number, out.has_sequence_number);
            }
            break;
        }
        case 2u: { /* Indicator Set: v1 (a single value) vs. v2+ (a list of entries) */
            const bool v2 = (rnd_byte() & 1u) != 0u;
            uint8_t entries[3 * 5];
            const uint8_t count = v2 ? (uint8_t)rnd_range(0u, 5u) : 0u;
            for (size_t i = 0; i < (size_t)count * 3u; i++) {
                entries[i] = rnd_byte();
            }
            const zw_cc_indicator_set_t in = {
                .has_entries = v2, .v1_value = rnd_byte(), .indicator0_value = 0u,
                .entries = count > 0u ? entries : NULL, .entry_count = count,
            };
            const size_t n = zw_cc_indicator_set_build(buf, sizeof(buf), &in);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
            zw_cc_indicator_set_t out;
            TEST_ASSERT_TRUE(zw_cc_indicator_set_parse(buf, n, &out));
            TEST_ASSERT_EQUAL(v2, out.has_entries);
            if (v2) {
                TEST_ASSERT_EQUAL_UINT8(count, out.entry_count);
                if (count > 0u) {
                    TEST_ASSERT_EQUAL_UINT8_ARRAY(entries, out.entries, (size_t)count * 3u);
                }
            }
            break;
        }
        case 3u: { /* §1 Multi Channel wrap/unwrap */
            uint8_t inner[20];
            const size_t inner_len = (size_t)rnd_range(0u, 20u);
            for (size_t i = 0; i < inner_len; i++) {
                inner[i] = rnd_byte();
            }
            const uint8_t src_ep = (uint8_t)rnd_range(0u, 127u);
            const uint8_t dst_ep = (uint8_t)rnd_range(0u, 127u);
            const size_t n = zw_cc_wrap_multi_channel(buf, sizeof(buf), src_ep, dst_ep, false, inner, inner_len);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
            zw_cc_unwrap_t out;
            TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
            TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_MULTI_CHANNEL, out.kind);
            TEST_ASSERT_EQUAL_UINT8(src_ep, out.source_endpoint);
            TEST_ASSERT_EQUAL_UINT8(dst_ep, out.destination_endpoint);
            TEST_ASSERT_EQUAL_UINT(inner_len, out.inner_len);
            if (inner_len > 0u) {
                TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, out.inner, inner_len);
            }
            break;
        }
        case 4u: { /* §1 Supervision wrap/unwrap */
            uint8_t inner[20];
            const size_t inner_len = (size_t)rnd_range(1u, 20u);
            for (size_t i = 0; i < inner_len; i++) {
                inner[i] = rnd_byte();
            }
            const uint8_t session = (uint8_t)rnd_range(0u, 63u);
            const size_t n = zw_cc_wrap_supervision(buf, sizeof(buf), session, true, inner, inner_len);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
            zw_cc_unwrap_t out;
            TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
            TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_SUPERVISION, out.kind);
            TEST_ASSERT_EQUAL_UINT8(session, out.supervision_session_id);
            TEST_ASSERT_TRUE(out.supervision_status_updates_wanted);
            TEST_ASSERT_EQUAL_UINT(inner_len, out.inner_len);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, out.inner, inner_len);
            break;
        }
        case 5u: { /* §1 CRC-16: a good frame unwraps; one flipped body byte is refused */
            uint8_t inner[20];
            const size_t inner_len = (size_t)rnd_range(1u, 20u);
            for (size_t i = 0; i < inner_len; i++) {
                inner[i] = rnd_byte();
            }
            const size_t n = zw_cc_wrap_crc16(buf, sizeof(buf), inner, inner_len);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
            zw_cc_unwrap_t out;
            TEST_ASSERT_TRUE(zw_cc_unwrap(buf, n, &out));
            TEST_ASSERT_EQUAL(ZW_CC_UNWRAP_CRC16, out.kind);
            TEST_ASSERT_EQUAL_UINT(inner_len, out.inner_len);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(inner, out.inner, inner_len);
            /* Flip the trailing CRC's own low byte via XOR 0xFF: since x ^ 0xFF != x for every x,
             * the reconstructed 16-bit CRC is provably different from the one still freshly
             * computed over the (unchanged) body - unlike flipping a body byte, this can never
             * coincidentally collide, so the refusal below is a certainty, not a probability. */
            buf[n - 1u] ^= 0xFFu;
            zw_cc_unwrap_t bad;
            TEST_ASSERT_FALSE(zw_cc_unwrap(buf, n, &bad));
            break;
        }
        case 6u: { /* §1 Multi Command: several inner commands, walked back with the iterator */
            uint8_t c0[5] = {(uint8_t)ZW_CC_BASIC, ZW_CMD_BASIC_GET};
            uint8_t c1[5] = {(uint8_t)ZW_CC_BATTERY, ZW_CMD_BATTERY_GET};
            const uint8_t *cmds[2] = {c0, c1};
            const size_t lens[2] = {2u, 2u};
            const size_t n = zw_cc_wrap_multi_command(buf, sizeof(buf), cmds, lens, 2u);
            TEST_ASSERT_TRUE(n > 0u);
            mutate_frame(buf, n);
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
            TEST_ASSERT_FALSE(zw_cc_multi_command_iter_next(&it, &cmd, &cmd_len));
            break;
        }
        default: { /* Multi Channel Association's destination list, walked with its own iterator */
            uint8_t dest[7] = {5u, 9u, 0x00u, 3u, 0x01u, 4u, 0x82u};
            zw_cc_mc_dest_iter_t it;
            zw_cc_mc_dest_iter_init(&it, dest, sizeof(dest));
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
            TEST_ASSERT_TRUE(d.is_endpoint);
            TEST_ASSERT_TRUE(d.is_mask);
            TEST_ASSERT_EQUAL_UINT16(4u, d.node_id);
            TEST_ASSERT_EQUAL_UINT8(0x02u, d.endpoint_mask);
            TEST_ASSERT_FALSE(zw_cc_mc_dest_iter_next(&it, &d));
            break;
        }
        }
    }
}

/* The codecs added for switches (ZW-034) and device software updates (ZW-033), and the trace
 * redaction (ZW-030): random commands with their own CC and command bytes in front (so the
 * parsers get past their headers), random Intel HEX-like text, random file names: never out of
 * bounds (ASan/UBSan in CI); a fragment's data never points outside the command; a converted
 * image never longer than its buffer. */
static void test_newer_parsers_survive_random_bytes(void)
{
    static const uint8_t CCS[] = {0x25, 0x26, 0x7A};
    uint8_t p[80];
    uint8_t out[128];
    for (uint32_t iter = 0; iter < 20000u; iter++) {
        const size_t len = (size_t)rnd_range(0u, sizeof p);
        for (size_t i = 0; i < len; i++) {
            p[i] = rnd_byte();
        }
        if (len > 1u) {
            p[0] = CCS[rnd_range(0u, 2u)];
            p[1] = (uint8_t)rnd_range(0u, 10u);
        }
        zw_cc_switch_set_t s;
        zw_cc_switch_report_t r;
        (void)zw_cc_switch_set_parse(p, len, p[0], &s);
        (void)zw_cc_switch_get_parse(p, len, p[0]);
        if (zw_cc_switch_report_parse(p, len, p[0], &r)) {
            (void)zw_cc_switch_percent(p[0], r.current);
        }
        zw_fwu_md_t md;
        zw_fwu_request_t q;
        zw_fwu_get_t g;
        zw_fwu_activation_t a;
        uint8_t st = 0u;
        bool has_wait = false;
        uint16_t wait = 0u;
        (void)zw_fwu_md_report_parse(p, len, &md);
        (void)zw_fwu_request_get_parse(p, len, &q);
        (void)zw_fwu_request_report_parse(p, len, &st);
        (void)zw_fwu_get_parse(p, len, &g);
        (void)zw_fwu_status_report_parse(p, len, &st, &has_wait, &wait);
        (void)zw_fwu_activation_report_parse(p, len, &a);
        uint16_t number = 0u;
        bool last = false;
        const uint8_t *data = NULL;
        size_t n = 0u;
        if (zw_fwu_report_parse(p, len, (iter & 1u) != 0u, &number, &last, &data, &n)) {
            TEST_ASSERT_TRUE(data >= p && data + n <= p + len);
        }
        uint8_t copy[80];
        memcpy(copy, p, len);
        (void)zw_redact_sensitive_cc(copy[0]);
        (void)zw_redact_command(copy, len);
    }
    /* Intel HEX-like text: ':' and hex digits, line ends, now and then a whole valid line */
    static const char ALPHA[] = "0123456789ABCDEF:\r\n";
    char text[200];
    for (uint32_t iter = 0; iter < 20000u; iter++) {
        const size_t len = (size_t)rnd_range(0u, sizeof text);
        for (size_t i = 0; i < len; i++) {
            text[i] = ALPHA[rnd_range(0u, (uint32_t)(sizeof ALPHA - 2u))];
        }
        if (len > 12u && (iter & 3u) == 0u) {
            memcpy(text, ":0400000001020304F2\n", 20u < len ? 20u : len);
        }
        const size_t cap = (size_t)rnd_range(0u, sizeof out);
        const size_t got = zw_fwfile_hex_to_bin((const uint8_t *)text, len, out, cap);
        TEST_ASSERT_TRUE(got <= cap);
        char name[12];
        for (size_t i = 0; i + 1u < sizeof name; i++) {
            name[i] = (char)rnd_range(0x2Eu, 0x7Au);
        }
        name[sizeof name - 1u] = '\0';
        (void)zw_fwfile_guess(name, (const uint8_t *)text, len);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_random_bytes_never_lie_or_go_out_of_bounds);
    RUN_TEST(test_valid_frames_survive_noise_in_order);
    RUN_TEST(test_one_corrupted_byte_is_never_reported_as_the_original);
    RUN_TEST(test_every_api_parser_survives_random_bytes);
    RUN_TEST(test_random_valid_api_messages_round_trip);
    RUN_TEST(test_every_cc_parser_survives_random_bytes);
    RUN_TEST(test_every_cc_parser_has_a_header_to_force);
    RUN_TEST(test_every_cc_parser_survives_random_bytes_with_its_header);
    RUN_TEST(test_every_cc_parser_survives_every_truncation);
    RUN_TEST(test_mutated_valid_frames_survive);
    RUN_TEST(test_random_valid_cc_round_trips);
    RUN_TEST(test_newer_parsers_survive_random_bytes);
    return UNITY_END();
}
