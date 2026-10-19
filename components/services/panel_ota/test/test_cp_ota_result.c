/* CP-005: the production result builder must replace a successful P4 install
 * with a failed radio install, without substituting the P4 version or authority. */
#include "cp_ota_result.h"
#include "unity.h"

static panel_ota_status_t st;
void setUp(void)
{
    memset(&st, 0, sizeof st);
    st.phase = PANEL_OTA_RECEIVING;
    st.stage = PANEL_OTA_STAGE_WIFI_MODULE;
    st.kind = PANEL_OTA_KIND_WIFI_MODULE;
    st.percent = 50;
    snprintf(st.running, sizeof st.running, "0.7.4-dev.14");
    snprintf(st.target, sizeof st.target, "3.0.10-ra4");
    snprintf(st.previous, sizeof st.previous, "3.0.10-ra3");
    snprintf(st.cp_running, sizeof st.cp_running, "3.0.10-ra3");
    snprintf(st.waiting_for, sizeof st.waiting_for, "The system is armed");
    st.last.result = PANEL_OTA_LAST_UPDATED;
    snprintf(st.last.version, sizeof st.last.version, "0.7.4-dev.14");
    st.last.failed = 0xFF;
}
void tearDown(void)
{
}

static void test_transfer_failure_replaces_p4_success_and_classifies_install(void)
{
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = 2};
    cp_ota_failure_result(&st, "The Wi-Fi module update failed", &src, 3, 1700000000);
    TEST_ASSERT_EQUAL(PANEL_OTA_FAILED, st.phase);
    TEST_ASSERT_TRUE(st.failed_install);
    TEST_ASSERT_EQUAL_INT(-1, st.percent);
    TEST_ASSERT_EQUAL_STRING("", st.waiting_for);
    TEST_ASSERT_EQUAL(PANEL_OTA_LAST_FAILED, st.last.result);
    TEST_ASSERT_EQUAL_STRING("3.0.10-ra4", st.last.version);
    TEST_ASSERT_EQUAL_STRING("3.0.10-ra3", st.last.from);
    TEST_ASSERT_EQUAL_STRING("The Wi-Fi module update failed", st.last.reason);
    TEST_ASSERT_EQUAL_STRING(st.error, st.last.reason);
    TEST_ASSERT_EQUAL_UINT32(1700000000, st.last.at);
    TEST_ASSERT_EQUAL(src.kind, st.last.by_src.kind);
    TEST_ASSERT_EQUAL_UINT16(2, st.last.by_src.id);
    TEST_ASSERT_EQUAL_UINT8(3, st.last.by_user);
    TEST_ASSERT_EQUAL_UINT32(0, st.last.failed);
    TEST_ASSERT_EQUAL_STRING("0.7.4-dev.14", st.running);
    TEST_ASSERT_EQUAL_STRING("3.0.10-ra3", st.cp_running);
}
static void test_verification_failure_records_automatic_request(void)
{
    cp_ota_failure_result(&st, "The Wi-Fi module image failed verification", NULL, 0, 0);
    TEST_ASSERT_TRUE(st.failed_install);
    TEST_ASSERT_EQUAL(PANEL_OTA_LAST_FAILED, st.last.result);
    TEST_ASSERT_EQUAL_STRING("The Wi-Fi module image failed verification", st.last.reason);
    TEST_ASSERT_EQUAL(ALARM_SRC_SYSTEM, st.last.by_src.kind);
    TEST_ASSERT_EQUAL_UINT16(0, st.last.by_src.id);
    TEST_ASSERT_EQUAL_UINT8(0, st.last.by_user);
    TEST_ASSERT_EQUAL_UINT32(0, st.last.at);
}
static void test_reason_is_bounded_and_identical_in_current_and_last_result(void)
{
    char reason[128];
    memset(reason, 'x', sizeof reason);
    reason[sizeof reason - 1] = '\0';
    cp_ota_failure_result(&st, reason, NULL, 0, 0);
    TEST_ASSERT_EQUAL_UINT(sizeof st.error - 1, strlen(st.error));
    TEST_ASSERT_EQUAL_STRING(st.error, st.last.reason);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_transfer_failure_replaces_p4_success_and_classifies_install);
    RUN_TEST(test_verification_failure_records_automatic_request);
    RUN_TEST(test_reason_is_bounded_and_identical_in_current_and_last_result);
    return UNITY_END();
}
