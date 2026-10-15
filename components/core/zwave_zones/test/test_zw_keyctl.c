/* ZW-031 (docs/features/36): the installer's keypad control assignments - who may arm and disarm,
 * bound to the very device and class it was assigned at, stored and read back, and commands that
 * go through the real alarm engine's authority. The adapter underneath is test_zw_keypad's. */
#include <string.h>

#include "sim.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_zones/zw_keyctl.h"

static sim_t S;
static zw_net_t NET;
static zwkc_t K;
static unsigned CALLS;
static uint16_t LAST_SOURCE;

static bool command(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *src, zwk_reply_t *r)
{
    (void)ctx;
    CALLS++;
    LAST_SOURCE = src->id;
    r->result = alarm_engine_command(&S.eng, cmd, src, S.now, &r->detail);
    return true;
}
static const zwk_port_t PORT = {.command = command};

/* Node 8: a keypad with secure Entry Control at Access Control; node 9: a door sensor. */
static void node(size_t i, uint16_t id, bool keypad)
{
    zw_node_record_t *n = &NET.nodes.nodes[i];
    memset(n, 0, sizeof(*n));
    n->node_id = id;
    n->generation = 3;
    n->has_protocol = true;
    n->is_flirs = keypad;
    n->security_classes = ZW_S2_KEY_ACCESS | ZW_S2_KEY_AUTH;
    n->interview_step = ZW_IV_STEP_DONE;
    n->cc_count = 1;
    n->ccs[0] = (zw_node_cc_t){keypad ? (uint8_t)ZW_CC_ENTRY_CONTROL : 0x71u, 1, false, true};
}

void setUp(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_start(&S, &cfg, NULL);
    memset(&NET, 0, sizeof(NET));
    NET.checked = true;
    NET.radio = ZW_NET_RADIO_OURS;
    NET.keys_home_id = 0xA1B2C3D4u;
    NET.nodes.node_count = 3;
    node(0, 8, true);
    node(1, 9, false);
    node(2, 10, true);
    zwkc_init(&K, &PORT);
    CALLS = 0;
}
void tearDown(void) {}

static uint8_t WIRE[40];
static zw_s2_rx_t notification(uint16_t from, zw_sec_class_t cls, uint8_t seq, uint8_t event,
                               const char *code)
{
    const zw_cc_entry_notification_t n = {seq, code != NULL ? ZW_ENTRY_DATA_ASCII
                                                            : ZW_ENTRY_DATA_NONE,
                                          event, (const uint8_t *)code,
                                          code != NULL ? strlen(code) : 0u};
    const size_t len = zw_cc_entry_notification_build(WIRE, sizeof(WIRE), &n);
    TEST_ASSERT_NOT_EQUAL(0u, len);
    return (zw_s2_rx_t){.node = from, .security = cls, .command = WIRE, .len = len};
}

/* Claimed and run, as the service does: the engine's result. */
static alarm_result_t press(uint16_t from, uint8_t seq, uint8_t event, const char *code)
{
    const zw_s2_rx_t rx = notification(from, ZW_SEC_S2_ACCESS, seq, event, code);
    size_t slot = ZWKC_SLOTS;
    TEST_ASSERT_TRUE(zwkc_claims(&K, &NET, &rx, &slot));
    zwk_reply_t reply;
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, zwkc_run(&K, slot, NET.keys_home_id, 3, &rx, 0u, &reply));
    return reply.result;
}

static void test_fit_says_why(void)
{
    TEST_ASSERT_EQUAL_INT(ZWKC_FIT_OK, zwkc_fit(&NET, 8));
    TEST_ASSERT_EQUAL_INT(ZWKC_FIT_NONE, zwkc_fit(&NET, 9));  /* a sensor */
    TEST_ASSERT_EQUAL_INT(ZWKC_FIT_NONE, zwkc_fit(&NET, 77)); /* not in the network */
    NET.nodes.nodes[0].interview_step = ZW_IV_STEP_IDENTITY;
    TEST_ASSERT_EQUAL_INT(ZWKC_FIT_NOT_READY, zwkc_fit(&NET, 8));
    NET.nodes.nodes[0].interview_step = ZW_IV_STEP_DONE;
    NET.nodes.nodes[0].security_classes = ZW_S2_KEY_UNAUTH;
    TEST_ASSERT_EQUAL_INT(ZWKC_FIT_NOT_SECURE, zwkc_fit(&NET, 8));
    NET.nodes.nodes[0].security_classes = ZW_S2_KEY_AUTH;
    NET.nodes.nodes[0].ccs[0].secure = false; /* Entry Control only in the clear */
    TEST_ASSERT_EQUAL_INT(ZWKC_FIT_NOT_SECURE, zwkc_fit(&NET, 8));
    TEST_ASSERT_FALSE(zwkc_assign(&K, 0, &NET, 8, 1, 0));
}

/* An assigned keypad arms and disarms with the users' own codes, on its partition only; it is
 * its own source (its lockout is its own). */
static void test_assigned_keypad_uses_the_engine(void)
{
    TEST_ASSERT_TRUE(zwkc_assign(&K, 2, &NET, 8, 1, 0));
    TEST_ASSERT_EQUAL_UINT(2u, zwkc_slot_of(&K, 8));
    TEST_ASSERT_EQUAL_UINT8(ZW_SEC_S2_ACCESS, K.slot[2].binding.security); /* its highest */
    TEST_ASSERT_EQUAL_INT(ALARM_OK, press(8, 1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    TEST_ASSERT_EQUAL_UINT16(ZWKC_DEVICE(2), LAST_SOURCE);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, press(8, 2, ZW_ENTRY_EVENT_DISARM_ALL, CODE_USER));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_INVALID_CODE, press(8, 3, ZW_ENTRY_EVENT_ARM_AWAY, "0000"));
    EXPECT_STATE(&S, 2, ALARM_STATE_DISARMED); /* partition 2 never touched */
    /* a second keypad: its own slot and source; one keypad in one slot only */
    TEST_ASSERT_FALSE(zwkc_assign(&K, 3, &NET, 8, 2, 0));
    TEST_ASSERT_TRUE(zwkc_assign(&K, zwkc_slot_for(&K, 10), &NET, 10, 2, 0));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, press(10, 1, ZW_ENTRY_EVENT_ARM_HOME, CODE_MASTER));
    EXPECT_LEVEL(&S, 2, ALARM_LEVEL_STAY);
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

/* Five wrong codes lock that keypad out, not the other one (per source, as the engine does). */
static void test_lockout_is_per_keypad(void)
{
    TEST_ASSERT_TRUE(zwkc_assign(&K, 0, &NET, 8, 1, 0));
    TEST_ASSERT_TRUE(zwkc_assign(&K, 1, &NET, 10, 1, 0));
    for (uint8_t seq = 1; seq <= 5; seq++) {
        TEST_ASSERT_EQUAL_INT(ALARM_ERR_INVALID_CODE,
                              press(8, seq, ZW_ENTRY_EVENT_DISARM_ALL, "0000"));
    }
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_LOCKED_OUT, press(8, 6, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, press(10, 1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER));
}

/* Only an assigned keypad's Entry Control Notification, at its class, from its current device:
 * a node ID given to another device (a new generation), another network, another class, another
 * command, a slot turned off - none is claimed, and nothing reaches the engine. */
static void test_claims_only_the_assigned_device(void)
{
    TEST_ASSERT_TRUE(zwkc_assign(&K, 0, &NET, 8, 1, 0));
    zw_s2_rx_t rx = notification(8, ZW_SEC_S2_ACCESS, 1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    TEST_ASSERT_TRUE(zwkc_claims(&K, &NET, &rx, NULL));
    rx.security = ZW_SEC_S2_AUTH;
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL));
    rx.security = ZW_SEC_NONE;
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL));
    rx = notification(10, ZW_SEC_S2_ACCESS, 1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL)); /* not assigned */
    const uint8_t battery[] = {0x80, 0x03, 50};
    rx = (zw_s2_rx_t){.node = 8, .security = ZW_SEC_S2_ACCESS, .command = battery, .len = 3};
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL));
    rx = notification(8, ZW_SEC_S2_ACCESS, 1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER);
    NET.nodes.nodes[0].generation = 4; /* removed, its node ID given to a new device */
    TEST_ASSERT_FALSE(zwkc_live(&K, 0, &NET));
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL));
    NET.nodes.nodes[0].generation = 3;
    NET.keys_home_id++; /* a new network */
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL));
    NET.keys_home_id--;
    NET.nodes.nodes[0].security_classes = ZW_S2_KEY_AUTH; /* paired again at another class */
    TEST_ASSERT_FALSE(zwkc_claims(&K, &NET, &rx, NULL));
    NET.nodes.nodes[0].security_classes = ZW_S2_KEY_ACCESS | ZW_S2_KEY_AUTH;
    TEST_ASSERT_TRUE(zwkc_claims(&K, &NET, &rx, NULL));
    /* turned off between the claim and the run: nothing runs */
    TEST_ASSERT_TRUE(zwkc_assign(&K, 0, &NET, 0, 0, 0));
    zwk_reply_t reply;
    TEST_ASSERT_EQUAL_INT(ZWK_UNTRUSTED, zwkc_run(&K, 0, NET.keys_home_id, 3, &rx, 0u, &reply));
    TEST_ASSERT_EQUAL_UINT(0u, CALLS);
}

/* Emergency buttons count only when allowed; the mask takes nothing else. */
static void test_emergency_buttons_need_the_installer(void)
{
    TEST_ASSERT_FALSE(zwkc_assign(&K, 0, &NET, 8, 1, (uint8_t)(1u << ALARM_PANIC_SILENT)));
    TEST_ASSERT_FALSE(zwkc_assign(&K, 0, &NET, 8, 1, 0x80u)); /* no such button */
    TEST_ASSERT_TRUE(zwkc_assign(&K, 0, &NET, 8, 1, 0));
    zw_s2_rx_t rx = notification(8, ZW_SEC_S2_ACCESS, 1, ZW_ENTRY_EVENT_POLICE, NULL);
    zwk_reply_t reply;
    TEST_ASSERT_EQUAL_INT(ZWK_IGNORED, zwkc_run(&K, 0, NET.keys_home_id, 3, &rx, 0u, &reply));
    TEST_ASSERT_EQUAL_UINT(0u, CALLS);
    TEST_ASSERT_TRUE(zwkc_assign(&K, 0, &NET, 8, 1, (uint8_t)(1u << ALARM_PANIC_POLICE)));
    rx = notification(8, ZW_SEC_S2_ACCESS, 2, ZW_ENTRY_EVENT_POLICE, NULL);
    TEST_ASSERT_EQUAL_INT(ZWK_HANDLED, zwkc_run(&K, 0, NET.keys_home_id, 3, &rx, 0u, &reply));
    TEST_ASSERT_EQUAL_UINT(1u, CALLS);
    rx = notification(8, ZW_SEC_S2_ACCESS, 3, ZW_ENTRY_EVENT_FIRE, NULL);
    TEST_ASSERT_EQUAL_INT(ZWK_IGNORED, zwkc_run(&K, 0, NET.keys_home_id, 3, &rx, 0u, &reply));
}

/* The stored assignments, frozen: this format's bytes are what an older or newer firmware reads. */
static const uint8_t FIXTURE_V1[ZWKC_BLOB_SIZE] = {
    'K', 'C', 1, 8,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                          /* slot 1: off */
    0xD4, 0xC3, 0xB2, 0xA1, 8, 0, 3, 0, 0, 1, 3, 0x01,         /* slot 2: node 8, gen 3, p1,
                                                                * Access Control, police */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x00, /* CRC: filled in below from the bytes above, then compared with the encoder */
};

static void test_stored_and_read_back(void)
{
    TEST_ASSERT_EQUAL_INT(0, ALARM_PANIC_POLICE); /* the fixture's mask 0x01 is police */
    TEST_ASSERT_TRUE(zwkc_assign(&K, 1, &NET, 8, 1, (uint8_t)(1u << ALARM_PANIC_POLICE)));
    uint8_t blob[ZWKC_BLOB_SIZE];
    TEST_ASSERT_EQUAL_UINT(ZWKC_BLOB_SIZE, zwkc_encode(&K, blob, sizeof blob));
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V1, blob, ZWKC_BLOB_SIZE - 2u);
    zwkc_t back;
    zwkc_init(&back, &PORT);
    TEST_ASSERT_TRUE(zwkc_decode(&back, blob, sizeof blob));
    TEST_ASSERT_EQUAL_UINT(1u, zwkc_slot_of(&back, 8));
    TEST_ASSERT_TRUE(zwkc_live(&back, 1, &NET));
    K = back; /* after a restart: it still works */
    TEST_ASSERT_EQUAL_INT(ALARM_OK, press(8, 1, ZW_ENTRY_EVENT_ARM_AWAY, CODE_USER));
    /* damage: every slot off, never a partial table */
    blob[20] ^= 0x01u;
    TEST_ASSERT_FALSE(zwkc_decode(&back, blob, sizeof blob));
    TEST_ASSERT_EQUAL_UINT(ZWKC_SLOTS, zwkc_slot_of(&back, 8));
    blob[20] ^= 0x01u;
    TEST_ASSERT_FALSE(zwkc_decode(&back, blob, sizeof blob - 1u));
    /* a value no assignment can have (security Unauthenticated), CRC right: refused */
    blob[4 + 12 + 10] = ZW_SEC_S2_UNAUTH;
    const uint16_t crc = zw_crc16(ZW_CRC16_INIT, blob, ZWKC_BLOB_SIZE - 2u);
    blob[ZWKC_BLOB_SIZE - 2u] = (uint8_t)crc;
    blob[ZWKC_BLOB_SIZE - 1u] = (uint8_t)(crc >> 8);
    TEST_ASSERT_FALSE(zwkc_decode(&back, blob, sizeof blob));
    /* no engine port: nothing can be granted */
    zwkc_init(&back, NULL);
    TEST_ASSERT_EQUAL_UINT(ZWKC_BLOB_SIZE, zwkc_encode(&K, blob, sizeof blob));
    TEST_ASSERT_FALSE(zwkc_decode(&back, blob, sizeof blob));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fit_says_why);
    RUN_TEST(test_assigned_keypad_uses_the_engine);
    RUN_TEST(test_lockout_is_per_keypad);
    RUN_TEST(test_claims_only_the_assigned_device);
    RUN_TEST(test_emergency_buttons_need_the_installer);
    RUN_TEST(test_stored_and_read_back);
    return UNITY_END();
}
