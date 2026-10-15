/* ZW-027: authenticated keypad commands through the real engine, not a second PIN authority. */
#include "sim.h"
#include "zwave_zones/zw_keypad.h"
static sim_t S;
static zwk_t K;
static zwk_binding_t B;
static zwk_reply_t R;
static unsigned CALLS;
static bool BUSY;
static uint64_t NOW;
static uint8_t WIRE[40];
static zw_s2_rx_t RX;

static bool command(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *src, zwk_reply_t *reply)
{
    (void)ctx;
    if (BUSY) { return false; }
    CALLS++;
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, src->kind);
    TEST_ASSERT_EQUAL_UINT16(B.device, src->id);
    TEST_ASSERT_EQUAL_UINT8(0, cmd->arm_flags);
    reply->result = alarm_engine_command(&S.eng, cmd, src, S.now, &reply->detail);
    return true;
}
static const zwk_port_t PORT = {.command = command};

void setUp(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_start(&S, &cfg, NULL);
    B = (zwk_binding_t){.home_id = 0xA1B2C3D4u, .generation = 3, .node = 8, .device = 2,
                       .partition = 1, .security = ZW_SEC_S2_AUTH};
    TEST_ASSERT_TRUE(zwk_init(&K, &B, &PORT));
    CALLS = 0; BUSY = false; NOW = 0u;
}
void tearDown(void) {}

static void frame(uint8_t sequence, uint8_t event, const char *code)
{
    const zw_cc_entry_notification_t n = {sequence,
        code != NULL ? ZW_ENTRY_DATA_ASCII : ZW_ENTRY_DATA_NONE, event,
        (const uint8_t *)code, code != NULL ? strlen(code) : 0u};
    const size_t len = zw_cc_entry_notification_build(WIRE, sizeof(WIRE), &n);
    TEST_ASSERT_TRUE(len != 0u);
    RX = (zw_s2_rx_t){.node = B.node, .security = B.security, .command = WIRE, .len = len};
}
static zwk_result_t receive(void) { return zwk_receive(&K, B.home_id, B.generation, &RX, NOW, &R); }
static void press(uint8_t seq, uint8_t event, const char *pin, alarm_result_t expected)
{
    frame(seq, event, pin);
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, receive());
    TEST_ASSERT_EQUAL_INT(expected, R.result);
}

static void test_arm_disarm_and_readiness_use_engine(void)
{
    sim_fault(&S, Z_WINDOW);
    press(1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER, ALARM_ERR_NOT_READY);
    TEST_ASSERT_EQUAL_UINT16(Z_WINDOW, R.detail.not_ready_zone);
    sim_restore(&S, Z_WINDOW);
    press(2, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER, ALARM_OK);
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    press(3, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER, ALARM_OK);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    press(4, ZW_ENTRY_EVENT_ARM_HOME, CODE_USER, ALARM_OK);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_STAY);
    press(5, ZW_ENTRY_EVENT_DISARM_ALL, CODE_ARMONLY, ALARM_ERR_NOT_AUTHORIZED);
    press(6, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER, ALARM_OK);
}

static void test_trust_and_current_identity_required(void)
{
    frame(1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    const zw_sec_class_t denied[] = {ZW_SEC_NONE, ZW_SEC_S0, ZW_SEC_S2_UNAUTH, ZW_SEC_TEMP, ZW_SEC_S2_ACCESS};
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        RX.security = denied[i];
        TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, receive());
    }
    RX.security = B.security;
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, zwk_receive(&K, B.home_id + 1u, B.generation, &RX, NOW, &R));
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, zwk_receive(&K, B.home_id, B.generation + 1u, &RX, NOW, &R));
    RX.node++;
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, receive());
    RX.node = B.node; RX.multi_channel = true; RX.source_endpoint = 1;
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, receive());
    RX.source_endpoint = 0; RX.destination_endpoint = 1;
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, receive());
    TEST_ASSERT_EQUAL_UINT(0, CALLS);
    TEST_ASSERT_FALSE(zwk_init(&K, NULL, &PORT));
    RX.destination_endpoint = 0;
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, receive());
}

static void test_duplicate_refusals_do_not_multiply_lockout(void)
{
    press(1, ZW_ENTRY_EVENT_DISARM_ALL, "0000", ALARM_ERR_INVALID_CODE);
    for (unsigned i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(ZWK_DUPLICATE, receive());
        TEST_ASSERT_EQUAL_INT(ALARM_ERR_INVALID_CODE, R.result);
    }
    TEST_ASSERT_EQUAL_UINT(1, CALLS);
    for (uint8_t seq = 2; seq <= 5; seq++) {
        press(seq, ZW_ENTRY_EVENT_DISARM_ALL, "0000", ALARM_ERR_INVALID_CODE);
    }
    press(6, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER, ALARM_ERR_LOCKED_OUT);
    EXPECT_EVENTS(&S, ALARM_EV_LOCKOUT, 1);
}

static void test_busy_is_retryable_and_interleaved_duplicates_are_not(void)
{
    frame(254, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    BUSY = true;
    TEST_ASSERT_EQUAL_INT(ZWK_BUSY, receive());
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_UNAVAILABLE, R.result);
    BUSY = false;
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, receive());
    press(255, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER, ALARM_OK);
    frame(254, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZWK_DUPLICATE, receive());
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    press(0, ZW_ENTRY_EVENT_ARM_HOME, CODE_USER, ALARM_OK);
    TEST_ASSERT_EQUAL_UINT(3, CALLS);
}

static void test_disarm_all_is_scoped_to_assignment(void)
{
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_arm(&S, 2, CODE_MASTER, ALARM_LEVEL_AWAY, 0));
    press(1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER, ALARM_OK);
    press(2, ZW_ENTRY_EVENT_DISARM_ALL, CODE_MASTER, ALARM_OK);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    EXPECT_STATE(&S, 2, ALARM_STATE_EXIT_DELAY);
    B.partition = 2;
    TEST_ASSERT_TRUE(zwk_init(&K, &B, &PORT));
    press(3, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER, ALARM_ERR_NOT_AUTHORIZED);
}

static void test_no_action_from_partial_codes_or_unconfigured_panics(void)
{
    const char *bad[] = {NULL, "", "123", "12345678901", "12x4", "12 4"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        frame(1, ZW_ENTRY_EVENT_DISARM_ALL, bad[i]);
        TEST_ASSERT_EQUAL_INT(ZWK_INVALID, receive());
    }
    frame(2, ZW_ENTRY_EVENT_CACHED_KEYS, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZWK_IGNORED, receive());
    frame(3, ZW_ENTRY_EVENT_ENTER, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZWK_IGNORED, receive());
    frame(4, ZW_ENTRY_EVENT_POLICE, NULL);
    TEST_ASSERT_EQUAL_INT(ZWK_IGNORED, receive());
    TEST_ASSERT_EQUAL_UINT(0, CALLS);
    B.panic_mask = (uint8_t)(1u << ALARM_PANIC_POLICE);
    TEST_ASSERT_TRUE(zwk_init(&K, &B, &PORT));
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, receive());
    TEST_ASSERT_EQUAL_INT(ALARM_OK, R.result);
    TEST_ASSERT_EQUAL_INT(ZWK_DUPLICATE, receive());
    EXPECT_EVENTS(&S, ALARM_EV_ALARM, 1);
}

static void test_invalid_assignments_stay_disabled(void)
{
    B.security = ZW_SEC_S2_UNAUTH;
    TEST_ASSERT_FALSE(zwk_init(&K, &B, &PORT));
    TEST_ASSERT_FALSE(K.enabled);
    B.security = ZW_SEC_S2_ACCESS; B.partition = 0;
    TEST_ASSERT_FALSE(zwk_init(&K, &B, &PORT));
    B.partition = 1; B.device = 0;
    TEST_ASSERT_FALSE(zwk_init(&K, &B, &PORT));
}

static void test_endpoint_access_class_and_reassignment(void)
{
    B.endpoint = 2; B.security = ZW_SEC_S2_ACCESS;
    TEST_ASSERT_TRUE(zwk_init(&K, &B, &PORT));
    frame(10, ZW_ENTRY_EVENT_ARM_HOME, CODE_USER);
    RX.multi_channel = true; RX.source_endpoint = 2;
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, receive());
    TEST_ASSERT_EQUAL_INT(ALARM_OK, R.result);
    TEST_ASSERT_TRUE(zwk_init(&K, &K.binding, &K.port));
    frame(11, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER);
    RX.multi_channel = true; RX.source_endpoint = 2;
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, receive());
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

static void test_duress_and_no_credentials_in_state(void)
{
    press(1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER, ALARM_OK);
    press(2, ZW_ENTRY_EVENT_DISARM_ALL, CODE_DURESS, ALARM_OK);
    EXPECT_EVENTS(&S, ALARM_EV_DURESS, 1);
    const unsigned char *memory = (const unsigned char *)&K;
    for (size_t i = 0; i + 4u <= sizeof(K); i++) {
        TEST_ASSERT_TRUE(memcmp(memory + i, CODE_USER, 4u) != 0);
        TEST_ASSERT_TRUE(memcmp(memory + i, CODE_DURESS, 4u) != 0);
    }
}

static void test_malformed_or_binary_pin_never_reaches_engine(void)
{
    frame(1, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER);
    RX.len--; /* missing padding still fails the declared wire length */
    TEST_ASSERT_EQUAL_INT(ZWK_INVALID, receive());
    RX.len++;
    WIRE[3] = ZW_ENTRY_DATA_MD5;
    TEST_ASSERT_EQUAL_INT(ZWK_INVALID, receive());
    WIRE[3] = ZW_ENTRY_DATA_RAW;
    TEST_ASSERT_EQUAL_INT(ZWK_INVALID, receive());
    WIRE[3] = ZW_ENTRY_DATA_ASCII; WIRE[7] = 0;
    TEST_ASSERT_EQUAL_INT(ZWK_INVALID, receive());
    WIRE[7] = 0xFF;
    TEST_ASSERT_EQUAL_INT(ZWK_INVALID, receive());
    TEST_ASSERT_EQUAL_UINT(0, CALLS);
}

static void test_retry_expiry_above_32bit_uptime(void)
{
    NOW = UINT32_MAX - 500u;
    press(1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER, ALARM_OK);
    press(2, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER, ALARM_OK);
    NOW += ZWK_RETRY_WINDOW_MS - 1u;
    frame(1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    TEST_ASSERT_EQUAL_INT(ZWK_DUPLICATE, receive());
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    NOW++; /* retries never extend the original deadline */
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, receive());
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT(3, CALLS);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arm_disarm_and_readiness_use_engine);
    RUN_TEST(test_trust_and_current_identity_required);
    RUN_TEST(test_duplicate_refusals_do_not_multiply_lockout);
    RUN_TEST(test_busy_is_retryable_and_interleaved_duplicates_are_not);
    RUN_TEST(test_disarm_all_is_scoped_to_assignment);
    RUN_TEST(test_no_action_from_partial_codes_or_unconfigured_panics);
    RUN_TEST(test_invalid_assignments_stay_disabled);
    RUN_TEST(test_endpoint_access_class_and_reassignment);
    RUN_TEST(test_duress_and_no_credentials_in_state);
    RUN_TEST(test_malformed_or_binary_pin_never_reaches_engine);
    RUN_TEST(test_retry_expiry_above_32bit_uptime);
    return UNITY_END();
}
