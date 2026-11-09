/* docs/features/43: POST network-confirm through the server link keeps a Wi-Fi or address change
 * that alarm-server started, and is safe for the server to repeat until it hears 200. */
#include <string.h>

#include "unity.h"
#include "web_net_confirm.h"

#define S(n) ((int64_t)(n) * 1000000)
#define OP "0123456789abcdef0123456789abcdef"

static web_net_confirm_memo_t memo;
static bool trial_yes; /* what net_wifi_trial_confirm() says next */
static unsigned asked;
static char asked_op[40];

static bool fake_confirm(const char *op, void *ctx)
{
    (void)ctx;
    asked++;
    strncpy(asked_op, op, sizeof(asked_op) - 1);
    const bool yes = trial_yes;
    trial_yes = false; /* as on the panel: a confirmed trial is over */
    return yes;
}

void setUp(void)
{
    memset(&memo, 0, sizeof(memo));
    trial_yes = false;
    asked = 0;
    memset(asked_op, 0, sizeof(asked_op));
}
void tearDown(void) {}

/* Confirmed: 200 {"ok":true}, with the operation handed on as it came. */
static void test_confirm_ok(void)
{
    trial_yes = true;
    const web_net_confirm_t r = web_net_confirm(&memo, "POST", OP, S(10), fake_confirm, NULL);
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_OK, r);
    TEST_ASSERT_EQUAL_STRING("200 OK", web_net_confirm_status(r));
    TEST_ASSERT_NULL(web_net_confirm_error(r));
    TEST_ASSERT_EQUAL_UINT(1, asked);
    TEST_ASSERT_EQUAL_STRING(OP, asked_op);
}

/* No trial, another token, too early or not connected yet: 409 not_confirmed. */
static void test_not_confirmed(void)
{
    const web_net_confirm_t r = web_net_confirm(&memo, "POST", OP, S(5), fake_confirm, NULL);
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_NOT_CONFIRMED, r);
    TEST_ASSERT_EQUAL_STRING("409 Conflict", web_net_confirm_status(r));
    TEST_ASSERT_EQUAL_STRING("not_confirmed", web_net_confirm_error(r));
    TEST_ASSERT_EQUAL_UINT(1, asked);
}

/* The server's retries: the same token again after a 200 (lost on the way) is 200 again, and a
 * retry before the panel is ready is 409 until it is. */
static void test_repeats_are_safe(void)
{
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_NOT_CONFIRMED,
                      web_net_confirm(&memo, "POST", OP, S(5), fake_confirm, NULL));
    trial_yes = true;
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_OK, web_net_confirm(&memo, "POST", OP, S(10), fake_confirm, NULL));
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_OK, web_net_confirm(&memo, "POST", OP, S(15), fake_confirm, NULL));
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_OK, web_net_confirm(&memo, "POST", OP, S(115), fake_confirm, NULL));
    /* Another token is not the one kept. */
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_NOT_CONFIRMED,
                      web_net_confirm(&memo, "POST", "ffffffffffffffffffffffffffffffff", S(16),
                                      fake_confirm, NULL));
    /* Long after the trial, the memory has gone. */
    TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_NOT_CONFIRMED,
                      web_net_confirm(&memo, "POST", OP, S(10) + WEB_NET_CONFIRM_MEMO_US, fake_confirm,
                                      NULL));
}

/* A missing or malformed operation: 400 bad_operation, and the trial is never asked. */
static void test_bad_operation(void)
{
    static const char *const BAD[] = {
        NULL,
        "",
        "0123456789abcdef0123456789abcde",   /* 31 */
        "0123456789abcdef0123456789abcdef0", /* 33 */
        "0123456789ABCDEF0123456789abcdef",  /* the panel's tokens are lowercase */
        "0123456789abcdef0123456789abcdeg",
        "0123456789abcdef 123456789abcdef",
    };
    trial_yes = true;
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        const web_net_confirm_t r = web_net_confirm(&memo, "POST", BAD[i], S(10), fake_confirm, NULL);
        TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_BAD_OPERATION, r);
        TEST_ASSERT_EQUAL_STRING("400 Bad Request", web_net_confirm_status(r));
        TEST_ASSERT_EQUAL_STRING("bad_operation", web_net_confirm_error(r));
    }
    TEST_ASSERT_EQUAL_UINT(0, asked);
    TEST_ASSERT_FALSE(web_net_confirm_operation_ok(NULL));
    TEST_ASSERT_TRUE(web_net_confirm_operation_ok(OP));
}

/* Anything but POST: 405, before the operation is even looked at. */
static void test_wrong_method(void)
{
    static const char *const METHODS[] = {"GET", "PUT", "DELETE", "post", "", NULL};
    trial_yes = true;
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        const web_net_confirm_t r = web_net_confirm(&memo, METHODS[i], OP, S(10), fake_confirm, NULL);
        TEST_ASSERT_EQUAL(WEB_NET_CONFIRM_BAD_METHOD, r);
        TEST_ASSERT_EQUAL_STRING("405 Method Not Allowed", web_net_confirm_status(r));
        TEST_ASSERT_FALSE(web_net_confirm_method_ok(METHODS[i]));
    }
    TEST_ASSERT_EQUAL_UINT(0, asked);
    TEST_ASSERT_TRUE(web_net_confirm_method_ok("POST"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_confirm_ok);
    RUN_TEST(test_not_confirmed);
    RUN_TEST(test_repeats_are_safe);
    RUN_TEST(test_bad_operation);
    RUN_TEST(test_wrong_method);
    return UNITY_END();
}
