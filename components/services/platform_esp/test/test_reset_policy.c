/* F2: the service console can't factory-reset an armed panel, nor without the installer code, nor
 * erase the event log; with the engine down it is the way out. */
#include "reset_policy.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_armed_is_refused(void)
{
    TEST_ASSERT_NOT_NULL(reset_policy_refusal(true, true, true, false));
}

static void test_disarmed_with_the_installer_code_is_allowed(void)
{
    TEST_ASSERT_NULL(reset_policy_refusal(true, false, true, false));
}

static void test_without_the_code_is_refused(void)
{
    TEST_ASSERT_NOT_NULL(reset_policy_refusal(true, false, false, false));
}

static void test_the_log_is_always_refused(void)
{
    TEST_ASSERT_NOT_NULL(reset_policy_refusal(true, false, true, true));
    TEST_ASSERT_NOT_NULL(reset_policy_refusal(false, false, false, true));
}

static void test_the_way_out_when_the_engine_is_down(void)
{
    TEST_ASSERT_NULL(reset_policy_refusal(false, false, false, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_armed_is_refused);
    RUN_TEST(test_disarmed_with_the_installer_code_is_allowed);
    RUN_TEST(test_without_the_code_is_refused);
    RUN_TEST(test_the_log_is_always_refused);
    RUN_TEST(test_the_way_out_when_the_engine_is_down);
    return UNITY_END();
}
