/* ZW-028: real engine -> device-owned feedback; no panel audio/voice renderer. */
#include "sim.h"
#include "zwave_zones/zw_feedback.h"
#include "zwave_proto/zw_cc.h"
#include "feedback_fixture_v1.h"
static sim_t S;
static zw_net_t NET;
static zwf_t F;
static alarm_snapshot_t SNAP;
static size_t SEEN;
static unsigned COUNT;
static bool HOLD;
static zw_s2_result_t RESULT;
static struct { uint16_t node; uint8_t id, property, value; uint32_t tag; } SENT[64];

static zw_s2_result_t send_command(void *ctx, uint16_t node, zw_sec_class_t cls,
                                  const uint8_t *cmd, size_t len, uint32_t tag)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, cls);
    TEST_ASSERT_EQUAL_UINT(7, len);
    const uint8_t header[] = {0x87, 0x01, 0x00, 0x01};
    TEST_ASSERT_EQUAL_MEMORY(header, cmd, sizeof(header));
    TEST_ASSERT_LESS_THAN(64, COUNT);
    SENT[COUNT].node = node; SENT[COUNT].id = cmd[4]; SENT[COUNT].property = cmd[5];
    SENT[COUNT].value = cmd[6]; SENT[COUNT].tag = tag;
    COUNT++;
    if (!HOLD && RESULT == ZW_S2_OK) { TEST_ASSERT_TRUE(zwf_done(&F, tag, RESULT, S.now)); }
    return RESULT;
}

static void pump(void)
{
    alarm_engine_snapshot(&S.eng, &SNAP);
    zwf_view_t view;
    zwf_view(&SNAP, &view);
    zwf_snapshot(&F, &view);
    while (SEEN < S.n_events) { zwf_event(&F, &S.events[SEEN++]); }
    zwf_poll(&F, &NET, S.now, send_command, NULL);
}

void setUp(void)
{
    alarm_config_t cfg;
    sim_config_basic(&cfg);
    cfg.partitions[1].enabled = true;
    sim_start(&S, &cfg, NULL);
    memset(&NET, 0, sizeof(NET));
    NET.checked = true; NET.radio = ZW_NET_RADIO_OURS; NET.keys_home_id = 0xA1B2C3D4u;
    NET.nodes.node_count = 2;
    for (size_t i = 0; i < 2; i++) {
        NET.nodes.nodes[i] = (zw_node_record_t){.node_id = (uint16_t)(8u + i), .generation = 3,
            .has_protocol = true, .is_flirs = true, .has_identity = true,
            .manufacturer_id = 0x0346, .product_type = 0x0101, .product_id = 0x0301,
            .security_classes = ZW_S2_KEY_ACCESS, .interview_step = ZW_IV_STEP_DONE,
            .cc_count = 1, .ccs = {{ZW_CC_INDICATOR, 3, false, true}}};
    }
    zwf_init(&F);
    TEST_ASSERT_TRUE(zwf_assign(&F, 0, &NET, 8, 1));
    SEEN = 0; COUNT = 0; HOLD = false; RESULT = ZW_S2_OK;
    pump();
    TEST_ASSERT_EQUAL_UINT8(2, SENT[0].id);
}
void tearDown(void) {}

static void test_engine_modes_delays_and_no_repeat(void)
{
    pump(); TEST_ASSERT_EQUAL_UINT(1, COUNT);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    pump();
    TEST_ASSERT_EQUAL_UINT8(18, SENT[1].id);
    TEST_ASSERT_EQUAL_UINT8(7, SENT[1].property);
    TEST_ASSERT_EQUAL_UINT8(60, SENT[1].value);
    sim_fault(&S, Z_FRONT); sim_restore(&S, Z_FRONT);
    sim_advance(&S, SEC(30)); pump();
    TEST_ASSERT_EQUAL_UINT(2, COUNT); /* no countdown restart at each snapshot */
    sim_advance(&S, SEC(30)); pump();
    TEST_ASSERT_EQUAL_UINT8(11, SENT[2].id);
    sim_fault(&S, Z_FRONT); pump();
    TEST_ASSERT_EQUAL_UINT8(17, SENT[3].id);
    TEST_ASSERT_EQUAL_UINT8(30, SENT[3].value);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); pump();
    TEST_ASSERT_EQUAL_UINT8(2, SENT[4].id);
}

static void test_not_ready_event_plays_bypass_once(void)
{
    sim_fault(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_NOT_READY, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    pump();
    TEST_ASSERT_EQUAL_UINT8(16, SENT[1].id);
    TEST_ASSERT_EQUAL_UINT8(1, SENT[1].value);
    for (size_t i = 0; i < S.n_events; i++) { zwf_event(&F, &S.events[i]); }
    pump(); TEST_ASSERT_EQUAL_UINT(2, COUNT);
    /* A late refusal may not undo successful arming. */
    sim_restore(&S, Z_WINDOW);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    pump(); TEST_ASSERT_EQUAL_UINT8(18, SENT[2].id);
}

static void test_silent_panic_duress_and_alarm_silence(void)
{
    sim_fault(&S, Z_HOLDUP); pump();
    TEST_ASSERT_EQUAL_UINT(1, COUNT);
    sim_restore(&S, Z_HOLDUP);
    sim_fault(&S, Z_SMOKE); pump();
    TEST_ASSERT_EQUAL_UINT8(14, SENT[1].id);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); pump();
    TEST_ASSERT_EQUAL_UINT8(2, SENT[2].id);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_disarm(&S, 1, CODE_DURESS)); pump();
    TEST_ASSERT_EQUAL_UINT(3, COUNT);
}

static void test_partition_scope_and_multiple_keypads(void)
{
    TEST_ASSERT_TRUE(zwf_assign(&F, 1, &NET, 9, 2));
    pump(); TEST_ASSERT_EQUAL_UINT16(9, SENT[1].node);
    sim_fault(&S, Z_SMOKE); pump();
    TEST_ASSERT_EQUAL_UINT(3, COUNT);
    TEST_ASSERT_EQUAL_UINT16(8, SENT[2].node);
    TEST_ASSERT_TRUE(zwf_assign(&F, 1, &NET, 9, 1));
    pump(); TEST_ASSERT_EQUAL_UINT8(14, SENT[3].id);
}

static void test_coalesces_inflight_arm_then_disarm(void)
{
    HOLD = true;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0)); pump();
    const uint32_t tag = SENT[1].tag;
    TEST_ASSERT_FALSE(zwf_assign(&F, 0, &NET, 0, 0));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_disarm(&S, 1, CODE_USER)); pump();
    TEST_ASSERT_EQUAL_UINT(2, COUNT);
    TEST_ASSERT_TRUE(zwf_done(&F, tag, ZW_S2_OK, S.now));
    HOLD = false; pump();
    TEST_ASSERT_EQUAL_UINT8(2, SENT[2].id);
    TEST_ASSERT_TRUE(zwf_done(&F, tag, ZW_S2_OK, S.now));
    pump(); TEST_ASSERT_EQUAL_UINT(3, COUNT);
}

static void test_retry_uses_current_state_and_expires_prompts(void)
{
    RESULT = ZW_S2_BUSY;
    sim_fault(&S, Z_WINDOW);
    (void)sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0); pump();
    TEST_ASSERT_EQUAL_UINT8(16, SENT[1].id);
    pump(); TEST_ASSERT_EQUAL_UINT(2, COUNT);
    sim_advance(&S, SEC(6)); RESULT = ZW_S2_OK; pump();
    TEST_ASSERT_EQUAL_UINT(2, COUNT); /* do not replay an old refusal after reconnect */
}

static void test_assignment_identity_and_security_guards(void)
{
    zw_node_record_t *n = &NET.nodes.nodes[0];
    n->generation++; pump(); TEST_ASSERT_EQUAL_UINT(1, COUNT);
    n->generation--; NET.keys_home_id++; pump(); TEST_ASSERT_EQUAL_UINT(1, COUNT);
    NET.keys_home_id--; n->security_classes = ZW_S2_KEY_UNAUTH; pump();
    TEST_ASSERT_FALSE(zwf_assign(&F, 0, &NET, 8, 1));
    n->security_classes = ZW_S2_KEY_ACCESS; n->product_id = 0xFFFF;
    TEST_ASSERT_FALSE(zwf_assign(&F, 0, &NET, 8, 1));
    n->product_id = 0x0301; n->is_flirs = false;
    TEST_ASSERT_FALSE(zwf_assign(&F, 0, &NET, 8, 1));
    n->is_flirs = true; n->ccs[0].secure = false;
    TEST_ASSERT_FALSE(zwf_assign(&F, 0, &NET, 8, 1));
    n->ccs[0].secure = true; pump();
    TEST_ASSERT_EQUAL_UINT(2, COUNT); /* one resync, then quiet */
    pump(); TEST_ASSERT_EQUAL_UINT(2, COUNT);
    TEST_ASSERT_TRUE(zwf_assign(&F, 0, &NET, 0, 0));
    sim_fault(&S, Z_SMOKE); pump(); TEST_ASSERT_EQUAL_UINT(2, COUNT);
}

/* ZW-028: the device page's "Sounds and lights" says why a device can't take feedback, exactly
 * where zwf_assign() refuses it, and an assignment goes to the device's own slot, else the first
 * free one. */
static void test_fit_and_slot_choice(void)
{
    zw_node_record_t *n = &NET.nodes.nodes[1]; /* node 9: unassigned */
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_OK, zwf_fit(&NET, 9));
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_NONE, zwf_fit(&NET, 77)); /* not in the network */
    n->interview_step = ZW_IV_STEP_NODE_INFO;
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_NOT_READY, zwf_fit(&NET, 9));
    TEST_ASSERT_FALSE(zwf_assign(&F, 1, &NET, 9, 1));
    n->interview_step = ZW_IV_STEP_DONE;
    n->security_classes = 0u;
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_NOT_SECURE, zwf_fit(&NET, 9));
    n->security_classes = ZW_S2_KEY_UNAUTH;
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_NOT_SECURE, zwf_fit(&NET, 9));
    TEST_ASSERT_FALSE(zwf_assign(&F, 1, &NET, 9, 1));
    n->security_classes = ZW_S2_KEY_AUTH;
    n->ccs[0].version = 2u;
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_UNSUPPORTED, zwf_fit(&NET, 9));
    TEST_ASSERT_FALSE(zwf_assign(&F, 1, &NET, 9, 1));
    n->ccs[0].version = 3u;
    n->product_id = 0x0201u; /* no profile for it: nothing to offer */
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_NONE, zwf_fit(&NET, 9));
    n->product_id = 0x0401u;
    TEST_ASSERT_EQUAL_INT(ZWF_FIT_OK, zwf_fit(&NET, 9));
    TEST_ASSERT_EQUAL_UINT(0u, zwf_slot_of(&F, 8));       /* setUp assigned node 8 to slot 0 */
    TEST_ASSERT_EQUAL_UINT(0u, zwf_slot_for(&F, 8));      /* its own slot again */
    TEST_ASSERT_EQUAL_UINT(ZWF_TARGETS, zwf_slot_of(&F, 9));
    TEST_ASSERT_EQUAL_UINT(1u, zwf_slot_for(&F, 9));      /* the first free one */
    TEST_ASSERT_TRUE(zwf_assign(&F, zwf_slot_for(&F, 9), &NET, 9, 2));
    TEST_ASSERT_EQUAL_UINT(1u, zwf_slot_of(&F, 9));
    for (size_t i = 2; i < ZWF_TARGETS; i++) { /* the rest taken by other (stale) bindings */
        F.target[i].binding = (zwf_binding_t){NET.keys_home_id, (uint16_t)(20u + i), 1u, 1u};
    }
    TEST_ASSERT_EQUAL_UINT(ZWF_TARGETS, zwf_slot_for(&F, 50)); /* full */
    TEST_ASSERT_EQUAL_UINT(1u, zwf_slot_for(&F, 9));           /* but its own is still its own */
}

static void test_persistence_crc_and_truncation(void)
{
    uint8_t blob[ZWF_BLOB_SIZE];
    zwf_t restored;
    TEST_ASSERT_EQUAL_UINT(ZWF_BLOB_SIZE, zwf_encode(&F, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_MEMORY(FEEDBACK_FIXTURE_V1, blob, sizeof(blob));
    for (size_t i = 0; i < sizeof(blob); i++) {
        TEST_ASSERT_FALSE(zwf_decode(&restored, blob, i));
        TEST_ASSERT_EQUAL_UINT(0, restored.target[0].binding.partition);
    }
    TEST_ASSERT_TRUE(zwf_decode(&restored, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT32(NET.keys_home_id, restored.target[0].binding.home);
    TEST_ASSERT_EQUAL_UINT16(8, restored.target[0].binding.node);
    TEST_ASSERT_FALSE(restored.target[0].synced);
    blob[10] ^= 1u;
    TEST_ASSERT_FALSE(zwf_decode(&restored, blob, sizeof(blob)));
}

static void test_stay_auto_stay_and_silent_exit(void)
{
    (void)sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT); pump();
    TEST_ASSERT_EQUAL_UINT(1, COUNT);
    sim_advance(&S, SEC(121)); pump(); /* auto-stay when nobody left */
    TEST_ASSERT_EQUAL_UINT8(10, SENT[1].id);
}

static void test_failed_send_does_not_delay_new_alarm(void)
{
    RESULT = ZW_S2_BUSY;
    (void)sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0); pump();
    TEST_ASSERT_EQUAL_UINT(2, COUNT);
    sim_fault(&S, Z_SMOKE);
    RESULT = ZW_S2_OK; pump(); /* no clock advance: urgent state supersedes backoff */
    TEST_ASSERT_EQUAL_UINT8(14, SENT[2].id);
}

static void test_old_refusal_after_successful_arm_disarm_is_dropped(void)
{
    sim_fault(&S, Z_WINDOW);
    (void)sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0);
    sim_restore(&S, Z_WINDOW);
    (void)sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0);
    (void)sim_disarm(&S, 1, CODE_USER);
    pump(); TEST_ASSERT_EQUAL_UINT(1, COUNT);
}

static void test_wrong_code_co_burglary_medical_feedback(void)
{
    (void)sim_arm(&S, 1, "0000", ALARM_LEVEL_AWAY, 0); pump();
    TEST_ASSERT_EQUAL_UINT8(9, SENT[1].id);
    sim_fault(&S, Z_CO); pump(); TEST_ASSERT_EQUAL_UINT8(15, SENT[2].id);
    sim_restore(&S, Z_CO); (void)sim_disarm(&S, 1, CODE_USER); pump();
    sim_fault(&S, Z_PANIC); pump(); TEST_ASSERT_EQUAL_UINT8(13, SENT[4].id);
    sim_restore(&S, Z_PANIC); (void)sim_disarm(&S, 1, CODE_USER); pump();
    sim_fault(&S, Z_WATER); pump(); TEST_ASSERT_EQUAL_UINT8(19, SENT[6].id);
}

static void test_countdown_uses_remaining_time_and_renews_long_delay(void)
{
    zwf_view_t view = {.partitions = 1, .taken_ms = S.now, .part = {{18, S.now + SEC(300)}}};
    zwf_snapshot(&F, &view);
    zwf_poll(&F, &NET, S.now, send_command, NULL);
    TEST_ASSERT_EQUAL_UINT8(255, SENT[1].value);
    S.now += SEC(240);
    zwf_poll(&F, &NET, S.now, send_command, NULL);
    TEST_ASSERT_EQUAL_UINT8(60, SENT[2].value);
    S.now += SEC(60);
    zwf_poll(&F, &NET, S.now, send_command, NULL);
    TEST_ASSERT_EQUAL_UINT(3, COUNT); /* do not replay an expired delay */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_engine_modes_delays_and_no_repeat);
    RUN_TEST(test_not_ready_event_plays_bypass_once);
    RUN_TEST(test_silent_panic_duress_and_alarm_silence);
    RUN_TEST(test_partition_scope_and_multiple_keypads);
    RUN_TEST(test_coalesces_inflight_arm_then_disarm);
    RUN_TEST(test_retry_uses_current_state_and_expires_prompts);
    RUN_TEST(test_assignment_identity_and_security_guards);
    RUN_TEST(test_fit_and_slot_choice);
    RUN_TEST(test_persistence_crc_and_truncation);
    RUN_TEST(test_stay_auto_stay_and_silent_exit);
    RUN_TEST(test_failed_send_does_not_delay_new_alarm);
    RUN_TEST(test_old_refusal_after_successful_arm_disarm_is_dropped);
    RUN_TEST(test_wrong_code_co_burglary_medical_feedback);
    RUN_TEST(test_countdown_uses_remaining_time_and_renews_long_delay);
    return UNITY_END();
}
