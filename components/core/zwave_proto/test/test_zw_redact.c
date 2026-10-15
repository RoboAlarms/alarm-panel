/* ZW-030 (docs/features/36): no PIN or code survives into what the panel records about its link -
 * clear Entry Control, User Code and the like, directly or inside clear encapsulations, in frames
 * received and sent, with 8- and 16-bit node IDs; damaged frames blanked; everything else left
 * exactly as it was. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_frame.h"
#include "zwave_proto/zw_redact.h"

void setUp(void) {}
void tearDown(void) {}

static const char PIN[] = "4729";

/* Whether the digits of PIN appear anywhere in the bytes. */
static bool has_pin(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4u <= n; i++) {
        if (memcmp(&p[i], PIN, 4u) == 0) {
            return true;
        }
    }
    return false;
}

static size_t entry_command(uint8_t *out, size_t cap)
{
    const zw_cc_entry_notification_t n = {9u, ZW_ENTRY_DATA_ASCII, ZW_ENTRY_EVENT_DISARM_ALL,
                                          (const uint8_t *)PIN, 4u};
    return zw_cc_entry_notification_build(out, cap, &n);
}

/* An ApplicationCommandHandler frame carrying `cmd` from node 5. */
static size_t app_frame(uint8_t *out, size_t cap, zw_nid_width_t w, const uint8_t *cmd, size_t n)
{
    zw_api_app_cmd_t a;
    memset(&a, 0, sizeof a);
    a.source_node_id = 5u;
    a.command = cmd;
    a.command_len = n;
    a.has_rssi = true;
    a.rssi = -60;
    uint8_t payload[ZW_PAYLOAD_MAX];
    const size_t pl = zw_api_app_cmd_build(payload, sizeof payload, w, &a);
    TEST_ASSERT_NOT_EQUAL(0u, pl);
    return zw_frame_build(out, cap, ZW_TYPE_REQUEST, (uint8_t)ZW_FUNC_APPLICATION_COMMAND_HANDLER,
                          payload, pl);
}

static void expect_valid(const uint8_t *f, size_t n)
{
    TEST_ASSERT_EQUAL_UINT((size_t)f[1] + 2u, n);
    TEST_ASSERT_EQUAL_HEX8(zw_frame_checksum(&f[1], f[1]), f[n - 1u]);
}

static void test_clear_entry_control_blanked_both_widths(void)
{
    uint8_t cmd[40];
    const size_t cn = entry_command(cmd, sizeof cmd);
    const zw_nid_width_t widths[2] = {ZW_NID_8, ZW_NID_16};
    for (size_t i = 0; i < 2u; i++) {
        uint8_t f[80];
        const size_t n = app_frame(f, sizeof f, widths[i], cmd, cn);
        TEST_ASSERT_TRUE(has_pin(f, n));
        TEST_ASSERT_TRUE(zw_redact_frame(f, n));
        TEST_ASSERT_FALSE(has_pin(f, n));
        expect_valid(f, n); /* still a frame a trace reader accepts */
        zw_api_app_cmd_t a;
        TEST_ASSERT_TRUE(zw_api_app_cmd_parse(&f[4], (size_t)f[1] - 3u, widths[i],
                                              ZW_FUNC_APPLICATION_COMMAND_HANDLER, &a));
        TEST_ASSERT_EQUAL_UINT(cn, a.command_len);
        TEST_ASSERT_EQUAL_HEX8(0x6F, a.command[0]); /* which command went by stays visible */
        TEST_ASSERT_EQUAL_HEX8(0x01, a.command[1]);
        TEST_ASSERT_EQUAL_UINT16(5u, a.source_node_id);
        TEST_ASSERT_FALSE(zw_redact_frame(f, n)); /* twice changes nothing */
    }
}

static void test_inside_clear_encapsulations(void)
{
    uint8_t inner[40];
    const size_t in = entry_command(inner, sizeof inner);
    uint8_t cmd[80];
    /* Supervision Get */
    cmd[0] = 0x6C; cmd[1] = 0x01; cmd[2] = 0x05; cmd[3] = (uint8_t)in;
    memcpy(&cmd[4], inner, in);
    TEST_ASSERT_TRUE(zw_redact_command(cmd, in + 4u));
    TEST_ASSERT_FALSE(has_pin(cmd, in + 4u));
    TEST_ASSERT_EQUAL_HEX8(0x05, cmd[2]);
    /* Multi Channel, from end point 1 */
    cmd[0] = 0x60; cmd[1] = 0x0D; cmd[2] = 0x01; cmd[3] = 0x00;
    memcpy(&cmd[4], inner, in);
    TEST_ASSERT_TRUE(zw_redact_command(cmd, in + 4u));
    TEST_ASSERT_FALSE(has_pin(cmd, in + 4u));
    /* CRC-16 around Multi Channel around it */
    cmd[0] = 0x56; cmd[1] = 0x01; cmd[2] = 0x60; cmd[3] = 0x0D; cmd[4] = 1; cmd[5] = 0;
    memcpy(&cmd[6], inner, in);
    cmd[6u + in] = 0xAB; cmd[7u + in] = 0xCD;
    TEST_ASSERT_TRUE(zw_redact_command(cmd, in + 8u));
    TEST_ASSERT_FALSE(has_pin(cmd, in + 8u));
    TEST_ASSERT_EQUAL_HEX8(0xAB, cmd[6u + in]); /* its CRC left */
    /* Multi Command: a battery report stays, the PIN goes */
    const uint8_t battery[] = {0x80, 0x03, 0x5A};
    size_t k = 0;
    cmd[k++] = 0x8F; cmd[k++] = 0x01; cmd[k++] = 2;
    cmd[k++] = sizeof battery; memcpy(&cmd[k], battery, sizeof battery); k += sizeof battery;
    cmd[k++] = (uint8_t)in; memcpy(&cmd[k], inner, in); k += in;
    TEST_ASSERT_TRUE(zw_redact_command(cmd, k));
    TEST_ASSERT_FALSE(has_pin(cmd, k));
    TEST_ASSERT_EQUAL_MEMORY(battery, &cmd[4], sizeof battery);
}

static void test_other_credential_classes_and_sent_frames(void)
{
    /* User Code Set sent to a lock (SendData), node 7 */
    uint8_t code[] = {0x63, 0x01, 0x03, 0x01, '4', '7', '2', '9'};
    zw_api_send_data_req_t s;
    memset(&s, 0, sizeof s);
    s.node_id = 7u;
    s.command = code;
    s.command_len = sizeof code;
    s.transmit_options = 0x25u;
    s.callback_id = 3u;
    uint8_t payload[64];
    const size_t pl = zw_api_send_data_req_build(payload, sizeof payload, ZW_NID_8, &s);
    TEST_ASSERT_NOT_EQUAL(0u, pl);
    uint8_t f[80];
    const size_t n = zw_frame_build(f, sizeof f, ZW_TYPE_REQUEST, (uint8_t)ZW_FUNC_SEND_DATA,
                                    payload, pl);
    TEST_ASSERT_TRUE(zw_redact_frame(f, n));
    TEST_ASSERT_FALSE(has_pin(f, n));
    expect_valid(f, n);
    /* Transport Service segment carrying anything */
    uint8_t seg[] = {0x55, 0xC0, 0x16, 0x00, 0x6F, 0x01, '4', '7', '2', '9'};
    TEST_ASSERT_TRUE(zw_redact_command(seg, sizeof seg));
    TEST_ASSERT_FALSE(has_pin(seg, sizeof seg));
    TEST_ASSERT_TRUE(zw_redact_sensitive_cc(0x4C));
    TEST_ASSERT_TRUE(zw_redact_sensitive_cc(0x83));
    TEST_ASSERT_TRUE(zw_redact_sensitive_cc(0x98));
}

/* Ciphertext and ordinary reports are exactly as they were: the trace keeps its value. */
static void test_everything_else_untouched(void)
{
    const uint8_t s2[] = {0x9F, 0x03, 0x11, 0x00, '4', '7', '2', '9', 0x12, 0x34, 0x56, 0x78};
    const uint8_t door[] = {0x71, 0x05, 0, 0, 0, 0xFF, 0x06, 0x16, 0x00};
    const uint8_t *cmds[] = {s2, door};
    const size_t lens[] = {sizeof s2, sizeof door};
    for (size_t i = 0; i < 2u; i++) {
        uint8_t f[80], before[80];
        const size_t n = app_frame(f, sizeof f, ZW_NID_8, cmds[i], lens[i]);
        memcpy(before, f, n);
        TEST_ASSERT_FALSE(zw_redact_frame(f, n));
        TEST_ASSERT_EQUAL_MEMORY(before, f, n);
    }
    uint8_t ack = 0x06;
    TEST_ASSERT_FALSE(zw_redact_frame(&ack, 1u));
    TEST_ASSERT_FALSE(zw_redact_frame(NULL, 10u));
    TEST_ASSERT_FALSE(zw_redact_command(NULL, 10u));
}

/* A damaged or cut-off frame: where its command is can't be trusted - blanked after FUNC. */
static void test_damaged_and_cut_frames_blanked(void)
{
    uint8_t cmd[40];
    const size_t cn = entry_command(cmd, sizeof cmd);
    uint8_t f[80];
    size_t n = app_frame(f, sizeof f, ZW_NID_8, cmd, cn);
    f[n - 1u] ^= 0x55u; /* bad checksum */
    TEST_ASSERT_TRUE(zw_redact_frame(f, n));
    TEST_ASSERT_FALSE(has_pin(f, n));
    TEST_ASSERT_EQUAL_HEX8(0x01, f[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_FUNC_APPLICATION_COMMAND_HANDLER, f[3]);
    n = app_frame(f, sizeof f, ZW_NID_8, cmd, cn);
    const size_t cut = n - 6u; /* the byte timeout cut it */
    TEST_ASSERT_TRUE(zw_redact_frame(f, cut));
    TEST_ASSERT_FALSE(has_pin(f, cut));
}

/* ZW-023: random frames never crash it, and no PIN placed anywhere in a whole frame's clear
 * Entry Control command survives. */
static void test_random_frames(void)
{
    srand(1234u);
    uint8_t cmd[40];
    const size_t cn = entry_command(cmd, sizeof cmd);
    for (unsigned r = 0; r < 20000u; r++) {
        uint8_t f[96];
        const size_t n = 1u + (size_t)rand() % sizeof f;
        for (size_t i = 0; i < n; i++) {
            f[i] = (uint8_t)rand();
        }
        if ((r & 3u) == 0u) {
            f[0] = 0x01;
        }
        (void)zw_redact_frame(f, n);
        /* nested encapsulation noise around a real Entry Control */
        uint8_t c[64];
        size_t k = 0;
        const unsigned depth = (unsigned)rand() % 6u;
        for (unsigned d = 0; d < depth && k + 4u < 20u; d++) {
            c[k++] = 0x6C; c[k++] = 0x01; c[k++] = 0x00; c[k++] = 0x00;
        }
        memcpy(&c[k], cmd, cn);
        k += cn;
        (void)zw_redact_command(c, k);
        TEST_ASSERT_FALSE(has_pin(c, k));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clear_entry_control_blanked_both_widths);
    RUN_TEST(test_inside_clear_encapsulations);
    RUN_TEST(test_other_credential_classes_and_sent_frames);
    RUN_TEST(test_everything_else_untouched);
    RUN_TEST(test_damaged_and_cut_frames_blanked);
    RUN_TEST(test_random_frames);
    return UNITY_END();
}
