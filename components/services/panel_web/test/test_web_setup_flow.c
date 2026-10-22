/* BL-048 screen-free setup: authorization, interrupted writes, physical input.
 * The commit fake models an atomic adapter; it does not prove NVS or HTTP wiring. */
#include "unity.h"
#include "web_setup_flow.h"
#include <string.h>

static web_setup_flow_t flow;
static web_setup_record_t saved;
static web_setup_actor_t actor;
static bool fail_write;
static unsigned writes;
static web_setup_write_t last_op;

static bool commit(void *ctx, web_setup_write_t op, const web_setup_record_t *next)
{
    (void)ctx;
    writes++;
    if (fail_write)
        return false;
    last_op = op;
    saved = *next;
    return true;
}
void setUp(void)
{
    memset(&saved, 0, sizeof saved);
    memset(&actor, 0, sizeof actor);
    actor.provisioned = actor.safe_to_change = true;
    actor.session = 1;
    writes = 0;
    fail_write = false;
    TEST_ASSERT_TRUE(web_setup_flow_init(&flow, &saved, commit, NULL));
}
void tearDown(void)
{
}

static void restart(void)
{
    TEST_ASSERT_TRUE(web_setup_flow_init(&flow, &saved, commit, NULL));
}
static web_setup_result_t write_op(web_setup_write_t op, uint64_t now)
{
    return web_setup_write(&flow, &actor, op, flow.record.revision, now);
}
static void accounts(void)
{
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 0));
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_ACCOUNTS, 1));
    actor.authenticated = actor.may_network = true;
}
static void network(void)
{
    accounts();
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_KEY_SAVED, 2));
}
static void ready(void)
{
    network();
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_begin(&flow, &actor, 3));
    actor.on_trial_network = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_confirm(&flow, &actor, 4));
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_FINISH, 5));
}

static void test_claim_requires_provisioning_safety_and_secret(void)
{
    actor.provisioned = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAVAILABLE, web_setup_claim(&flow, &actor, true, 0));
    actor.provisioned = true;
    actor.safe_to_change = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_LOCKED, web_setup_claim(&flow, &actor, true, 0));
    actor.safe_to_change = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, web_setup_claim(&flow, &actor, false, 0));
    actor.session = 0;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, web_setup_claim(&flow, &actor, true, 0));
    TEST_ASSERT_EQUAL_UINT(0, writes);
}
static void test_duplicate_claim_does_not_extend_or_allow_takeover(void)
{
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 10));
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 100));
    TEST_ASSERT_EQUAL_UINT64(300010, flow.claim_until);
    actor.session = 2;
    TEST_ASSERT_EQUAL(WEB_SETUP_BUSY, web_setup_claim(&flow, &actor, true, 101));
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_ACCOUNTS, 102));
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 300010));
}
static void test_expired_claim_cannot_create_accounts(void)
{
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 0));
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_ACCOUNTS, 300000));
}
static void test_failed_account_write_can_retry_without_consuming_claim(void)
{
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 0));
    fail_write = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_STORAGE, write_op(WEB_SETUP_ACCOUNTS, 1));
    TEST_ASSERT_EQUAL(WEB_SETUP_UNCLAIMED, flow.record.stage);
    TEST_ASSERT_EQUAL_UINT32(0, saved.revision);
    fail_write = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_ACCOUNTS, 2));
    restart();
    TEST_ASSERT_EQUAL(WEB_SETUP_KEY_PENDING, flow.record.stage);
    TEST_ASSERT_TRUE(flow.record.recovery_key_pending);
    TEST_ASSERT_EQUAL(WEB_SETUP_WRONG_STAGE, web_setup_claim(&flow, &actor, true, 3));
}
static void test_restart_revokes_claim_and_physical_window(void)
{
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_claim(&flow, &actor, true, 0));
    web_setup_recovery_open(&flow, 0);
    restart();
    TEST_ASSERT_FALSE(web_setup_recovery_active(&flow, 1));
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_ACCOUNTS, 1));
}
static void test_account_creation_cannot_skip_key_acknowledgement(void)
{
    accounts();
    TEST_ASSERT_EQUAL(WEB_SETUP_WRONG_STAGE, web_setup_trial_begin(&flow, &actor, 3));
    actor.authenticated = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_KEY_SAVED, 3));
    actor.authenticated = true;
    actor.may_network = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_KEY_SAVED, 3));
}
static void test_stale_revision_does_not_write(void)
{
    accounts();
    TEST_ASSERT_EQUAL(WEB_SETUP_STALE, web_setup_write(&flow, &actor, WEB_SETUP_KEY_SAVED, 0, 2));
    TEST_ASSERT_EQUAL_UINT(1, writes);
}
static void test_trial_requires_new_interface_and_owner(void)
{
    network();
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_begin(&flow, &actor, 10));
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, web_setup_trial_confirm(&flow, &actor, 20));
    actor.on_trial_network = true;
    actor.session = 2;
    TEST_ASSERT_EQUAL(WEB_SETUP_STALE, web_setup_trial_confirm(&flow, &actor, 20));
    TEST_ASSERT_EQUAL(WEB_SETUP_BUSY, web_setup_trial_begin(&flow, &actor, 20));
    TEST_ASSERT_EQUAL(WEB_SETUP_STALE, write_op(WEB_SETUP_FINISH, 20));
}
/* A wrong Wi-Fi password: the owner gives the trial up and tries again at once; nobody else can. */
static void test_trial_cancel_is_the_owners_and_frees_a_new_trial(void)
{
    network();
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_begin(&flow, &actor, 10));
    actor.session = 2;
    TEST_ASSERT_EQUAL(WEB_SETUP_STALE, web_setup_trial_cancel(&flow, &actor));
    actor.session = 1;
    actor.authenticated = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, web_setup_trial_cancel(&flow, &actor));
    actor.authenticated = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_cancel(&flow, &actor));
    TEST_ASSERT_EQUAL(WEB_SETUP_STALE, web_setup_trial_cancel(&flow, &actor));
    TEST_ASSERT_FALSE(web_setup_tick(&flow, 200000)); /* nothing left to roll back */
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_begin(&flow, &actor, 30));
}
static void test_trial_commit_failure_still_rolls_back_on_timeout(void)
{
    network();
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_begin(&flow, &actor, 10));
    actor.on_trial_network = true;
    fail_write = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_STORAGE, web_setup_trial_confirm(&flow, &actor, 20));
    TEST_ASSERT_FALSE(saved.network_confirmed);
    TEST_ASSERT_FALSE(web_setup_tick(&flow, 120009));
    TEST_ASSERT_EQUAL(WEB_SETUP_STALE, web_setup_trial_confirm(&flow, &actor, 120010));
    TEST_ASSERT_TRUE(web_setup_tick(&flow, 120010));
    TEST_ASSERT_FALSE(web_setup_tick(&flow, 120011));
}
static void test_confirmed_network_survives_restart_before_finish(void)
{
    network();
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_begin(&flow, &actor, 10));
    actor.on_trial_network = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, web_setup_trial_confirm(&flow, &actor, 20));
    TEST_ASSERT_EQUAL(WEB_SETUP_NETWORK_CONFIRMED, last_op);
    restart();
    actor.session = 2; /* sign in again after reconnect */
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_FINISH, 30));
    TEST_ASSERT_EQUAL(WEB_SETUP_READY, saved.stage);
}
static void test_armed_state_blocks_each_mutation(void)
{
    ready();
    web_setup_recovery_open(&flow, 10);
    actor.recovery_key_verified = true;
    actor.safe_to_change = false;
    unsigned before = writes;
    TEST_ASSERT_EQUAL(WEB_SETUP_LOCKED, write_op(WEB_SETUP_RESET_PASSWORD, 20));
    TEST_ASSERT_EQUAL(WEB_SETUP_LOCKED, web_setup_trial_begin(&flow, &actor, 20));
    TEST_ASSERT_EQUAL(WEB_SETUP_LOCKED, web_setup_trial_confirm(&flow, &actor, 20));
    TEST_ASSERT_EQUAL_UINT(before, writes);
}
static void test_password_reset_needs_physical_window_and_private_key(void)
{
    ready();
    actor.recovery_key_verified = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_RESET_PASSWORD, 10));
    web_setup_recovery_open(&flow, 10);
    actor.recovery_key_verified = false;
    TEST_ASSERT_EQUAL(WEB_SETUP_UNAUTHORIZED, write_op(WEB_SETUP_RESET_PASSWORD, 20));
    actor.recovery_key_verified = true;
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_RESET_PASSWORD, 20));
    TEST_ASSERT_FALSE(web_setup_recovery_active(&flow, 21));
    restart();
    TEST_ASSERT_TRUE(saved.recovery_key_pending);
    TEST_ASSERT_EQUAL(WEB_SETUP_READY, saved.stage);
    TEST_ASSERT_EQUAL(WEB_SETUP_WRONG_STAGE, web_setup_trial_begin(&flow, &actor, 22));
    TEST_ASSERT_EQUAL(WEB_SETUP_OK, write_op(WEB_SETUP_KEY_SAVED, 23));
    TEST_ASSERT_EQUAL(WEB_SETUP_READY, saved.stage);
    TEST_ASSERT_FALSE(saved.recovery_key_pending);
}
static void test_recovery_window_is_bounded_and_not_extended(void)
{
    web_setup_recovery_open(&flow, 10);
    web_setup_recovery_open(&flow, 100);
    TEST_ASSERT_TRUE(web_setup_recovery_active(&flow, 300009));
    TEST_ASSERT_FALSE(web_setup_recovery_active(&flow, 300010));
}
static void test_display_panels_never_auto_enable_listener(void)
{
    TEST_ASSERT_FALSE(web_setup_listener_allowed(&flow, false, true, 0));
    TEST_ASSERT_FALSE(web_setup_listener_allowed(&flow, true, false, 0));
    TEST_ASSERT_TRUE(web_setup_listener_allowed(&flow, true, true, 0));
    ready();
    TEST_ASSERT_FALSE(web_setup_listener_allowed(&flow, true, true, 10));
    web_setup_recovery_open(&flow, 10);
    TEST_ASSERT_FALSE(web_setup_listener_allowed(&flow, false, true, 20));
    TEST_ASSERT_TRUE(web_setup_listener_allowed(&flow, true, true, 20));
    TEST_ASSERT_FALSE(web_setup_listener_allowed(&flow, true, true, 300010));
}

static unsigned samples(web_setup_button_t *b, bool down, uint64_t *t, unsigned count)
{
    unsigned events = 0;
    for (unsigned i = 0; i < count; i++, *t += 10)
        if (web_setup_button_sample(b, down, *t))
            events++;
    return events;
}
static void test_boot_held_at_start_is_ignored_until_released(void)
{
    web_setup_button_t b = {0};
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, true, &t, 1000));
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, true, &t, 510));
    TEST_ASSERT_EQUAL_UINT(1, samples(&b, false, &t, 100));
}
static void test_short_press_and_bounce_do_nothing(void)
{
    web_setup_button_t b = {0};
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
    for (unsigned i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL_UINT(0, samples(&b, true, &t, 2));
        TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 2));
    }
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, true, &t, 490));
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
}
static void test_long_hold_only_fires_once_after_release(void)
{
    web_setup_button_t b = {0};
    uint64_t t = 0;
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, true, &t, 1000));
    TEST_ASSERT_EQUAL_UINT(1, samples(&b, false, &t, 100));
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, true, &t, 510));
    TEST_ASSERT_EQUAL_UINT(1, samples(&b, false, &t, 10));
}
static void test_polling_gap_cancels_unobserved_hold(void)
{
    web_setup_button_t b = {0};
    uint64_t t = 0;
    (void)samples(&b, false, &t, 10);
    (void)samples(&b, true, &t, 510);
    t += 1000;
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
    (void)samples(&b, true, &t, 510);
    t = 0; /* clock restart also invalidates the gesture */
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
}
static void test_five_second_threshold(void)
{
    web_setup_button_t b = {0};
    uint64_t t = 0;
    (void)samples(&b, false, &t, 10);
    (void)samples(&b, true, &t, 499);
    TEST_ASSERT_EQUAL_UINT(0, samples(&b, false, &t, 10));
    (void)samples(&b, true, &t, 500);
    TEST_ASSERT_EQUAL_UINT(1, samples(&b, false, &t, 10));
}
static void test_invalid_persisted_state_is_rejected(void)
{
    saved.stage = WEB_SETUP_READY;
    TEST_ASSERT_FALSE(web_setup_flow_init(&flow, &saved, commit, NULL));
    saved.stage = WEB_SETUP_UNCLAIMED;
    saved.recovery_key_pending = true;
    TEST_ASSERT_FALSE(web_setup_flow_init(&flow, &saved, commit, NULL));
    saved.recovery_key_pending = false;
    saved.revision = UINT32_MAX;
    TEST_ASSERT_FALSE(web_setup_flow_init(&flow, &saved, commit, NULL));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_claim_requires_provisioning_safety_and_secret);
    RUN_TEST(test_duplicate_claim_does_not_extend_or_allow_takeover);
    RUN_TEST(test_expired_claim_cannot_create_accounts);
    RUN_TEST(test_failed_account_write_can_retry_without_consuming_claim);
    RUN_TEST(test_restart_revokes_claim_and_physical_window);
    RUN_TEST(test_account_creation_cannot_skip_key_acknowledgement);
    RUN_TEST(test_stale_revision_does_not_write);
    RUN_TEST(test_trial_requires_new_interface_and_owner);
    RUN_TEST(test_trial_cancel_is_the_owners_and_frees_a_new_trial);
    RUN_TEST(test_trial_commit_failure_still_rolls_back_on_timeout);
    RUN_TEST(test_confirmed_network_survives_restart_before_finish);
    RUN_TEST(test_armed_state_blocks_each_mutation);
    RUN_TEST(test_password_reset_needs_physical_window_and_private_key);
    RUN_TEST(test_recovery_window_is_bounded_and_not_extended);
    RUN_TEST(test_display_panels_never_auto_enable_listener);
    RUN_TEST(test_boot_held_at_start_is_ignored_until_released);
    RUN_TEST(test_short_press_and_bounce_do_nothing);
    RUN_TEST(test_long_hold_only_fires_once_after_release);
    RUN_TEST(test_polling_gap_cancels_unobserved_hold);
    RUN_TEST(test_five_second_threshold);
    RUN_TEST(test_invalid_persisted_state_is_rejected);
    return UNITY_END();
}
