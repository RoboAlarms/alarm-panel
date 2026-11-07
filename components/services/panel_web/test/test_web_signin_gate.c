/* F8: wrong passwords are counted per source address, with a lower shared ceiling, so someone on
 * the home network can't keep the owner out of the Panel Manager. */
#include <string.h>

#include "unity.h"
#include "web_signin_gate.h"

#define S(n) ((int64_t)(n) * 1000000)

static void addr_of(uint8_t out[16], uint8_t last)
{
    memset(out, 0, 16);
    out[10] = 0xff;
    out[11] = 0xff;
    out[12] = 192;
    out[14] = 2;
    out[15] = last;
}

void setUp(void) {}
void tearDown(void) {}

/* F8: one source guessing wrong is held, the owner at another address signs in at once. */
static void test_a_guesser_is_held_alone(void)
{
    signin_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t guesser[16], owner[16];
    addr_of(guesser, 66);
    addr_of(owner, 10);
    int64_t t = S(1);
    for (unsigned i = 0; i < SIGNIN_FREE_TRIES; i++, t += S(3)) {
        TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, guesser, t, true));
        signin_gate_failed(&g, guesser, t);
    }
    TEST_ASSERT_EQUAL(SIGNIN_HELD, signin_gate_try(&g, guesser, t, true));
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, owner, t, true));
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, guesser, t + SIGNIN_HOLD_US, true));
}

/* F8: the spacing between tries is per source too: a script trying every two seconds doesn't
 * make the owner wait. */
static void test_spacing_is_per_source(void)
{
    signin_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t script[16], owner[16];
    addr_of(script, 66);
    addr_of(owner, 10);
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, script, S(10), true));
    TEST_ASSERT_EQUAL(SIGNIN_TOO_SOON, signin_gate_try(&g, script, S(11), true));
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, owner, S(11), true));
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, script, S(11), false)); /* unspaced kinds */
}

/* F8: guessing from many addresses meets the shared count, whose wait stays short. */
static void test_many_sources_share_a_lower_ceiling(void)
{
    signin_gate_t g;
    memset(&g, 0, sizeof(g));
    int64_t t = S(1);
    for (unsigned i = 0; i < SIGNIN_SHARED_FREE; i++) {
        uint8_t a[16];
        addr_of(a, (uint8_t)(100 + i));
        TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, a, t, true));
        signin_gate_failed(&g, a, t);
    }
    uint8_t owner[16];
    addr_of(owner, 10);
    TEST_ASSERT_EQUAL(SIGNIN_HELD, signin_gate_try(&g, owner, t, true));
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, owner, t + SIGNIN_SHARED_HOLD_US, true));
    for (unsigned i = 0; i < 40; i++) /* however long it goes on, the shared wait stays short */
        signin_gate_failed(&g, owner, t);
    TEST_ASSERT_TRUE(g.shared_hold_until_us - t <= SIGNIN_SHARED_MAX);
}

/* An hour without a wrong guess forgets the count. */
static void test_an_hour_forgets(void)
{
    signin_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t a[16];
    addr_of(a, 7);
    for (unsigned i = 0; i < SIGNIN_FREE_TRIES - 1u; i++)
        signin_gate_failed(&g, a, S(1));
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, a, S(1) + SIGNIN_FORGET_US + 1, true));
    signin_gate_failed(&g, a, S(1) + SIGNIN_FORGET_US + 1);
    TEST_ASSERT_EQUAL(SIGNIN_OK, signin_gate_try(&g, a, S(1) + SIGNIN_FORGET_US + S(4), true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_guesser_is_held_alone);
    RUN_TEST(test_spacing_is_per_source);
    RUN_TEST(test_many_sources_share_a_lower_ceiling);
    RUN_TEST(test_an_hour_forgets);
    return UNITY_END();
}
