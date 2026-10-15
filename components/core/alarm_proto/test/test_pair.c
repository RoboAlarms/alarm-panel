/*
 * test_pair.c - the panel's pairing state machine (alarm_proto pair.c): the happy path with
 * the golden vector, the commitment check, one attempt per window, timeouts and rejection.
 * Requirements: HAI-003 (docs/features/23, protocol.md "Pairing")
 */
#include <string.h>

#include "alarm_proto/proto_pair.h"
#include "unity.h"

static proto_pair_t P;
static uint8_t NONCE_HA[32];
static uint8_t NONCE_PANEL[32];
static uint8_t FP_HA[32];
static uint8_t FP_PANEL[32];
static uint8_t COMMIT[32];

void setUp(void)
{
    for (uint8_t i = 0; i < 32u; i++) {
        NONCE_HA[i] = i;                     /* vector 1 of protocol.md */
        NONCE_PANEL[i] = (uint8_t)(0x20 + i);
        FP_HA[i] = (uint8_t)(0x40 + i);
        FP_PANEL[i] = (uint8_t)(0x60 + i);
    }
    proto_sha256(NONCE_HA, sizeof(NONCE_HA), COMMIT);
    memset(&P, 0, sizeof(P));
}

void tearDown(void) {}

/* HAI-003: commit, nonce, reveal, matching code, Allow, the peer's certificate pinned. */
static void test_happy_path(void)
{
    const char *reply = NULL;
    proto_pair_open(&P, 1000, FP_PANEL);
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_WINDOW, P.state);

    TEST_ASSERT_TRUE(proto_pair_on_start(&P, 2000, COMMIT, FP_HA, NONCE_PANEL, &reply));
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_WAIT_REVEAL, P.state);

    TEST_ASSERT_TRUE(proto_pair_on_reveal(&P, 3000, NONCE_HA, &reply));
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_SHOW_CODE, P.state);
    TEST_ASSERT_EQUAL_STRING("588460", P.code); /* golden vector 1 */

    TEST_ASSERT_EQUAL_STRING("paired", proto_pair_decide(&P, true));
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_DONE, P.state);
    TEST_ASSERT_EQUAL_MEMORY(FP_HA, P.peer_fp, 32); /* what ha_link pins */
    TEST_ASSERT_EQUAL_STRING("", P.code);           /* secrets wiped */
}

/* HAI-003: a reveal that doesn't match the commitment ends the window. */
static void test_commit_must_verify(void)
{
    const char *reply = NULL;
    proto_pair_open(&P, 0, FP_PANEL);
    TEST_ASSERT_TRUE(proto_pair_on_start(&P, 0, COMMIT, FP_HA, NONCE_PANEL, &reply));

    uint8_t other[32];
    memcpy(other, NONCE_HA, 32);
    other[0] ^= 1u;
    TEST_ASSERT_FALSE(proto_pair_on_reveal(&P, 0, other, &reply));
    TEST_ASSERT_EQUAL_STRING("mismatch", reply);
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_FAILED, P.state);

    /* the window is spent: a fresh start is refused until the user opens it again */
    TEST_ASSERT_FALSE(proto_pair_on_start(&P, 1, COMMIT, FP_HA, NONCE_PANEL, &reply));
    TEST_ASSERT_EQUAL_STRING("mismatch", reply);
}

/* Without an open window there is nothing to talk to. */
static void test_closed_window(void)
{
    const char *reply = NULL;
    TEST_ASSERT_FALSE(proto_pair_on_start(&P, 0, COMMIT, FP_HA, NONCE_PANEL, &reply));
    TEST_ASSERT_EQUAL_STRING("not_pairing", reply);
    TEST_ASSERT_FALSE(proto_pair_on_reveal(&P, 0, NONCE_HA, &reply));
    TEST_ASSERT_EQUAL_STRING("not_pairing", reply);
    TEST_ASSERT_EQUAL_STRING("not_pairing", proto_pair_decide(&P, true));
}

/* One attempt at a time: a second client is told busy, the first goes on. */
static void test_busy(void)
{
    const char *reply = NULL;
    proto_pair_open(&P, 0, FP_PANEL);
    TEST_ASSERT_TRUE(proto_pair_on_start(&P, 0, COMMIT, FP_HA, NONCE_PANEL, &reply));
    TEST_ASSERT_FALSE(proto_pair_on_start(&P, 1, COMMIT, FP_HA, NONCE_PANEL, &reply));
    TEST_ASSERT_EQUAL_STRING("busy", reply);
    TEST_ASSERT_TRUE(proto_pair_on_reveal(&P, 2, NONCE_HA, &reply)); /* undisturbed */
}

/* HAI-003: the 2-minute window closes by itself, even mid-attempt. */
static void test_timeout(void)
{
    const char *reply = NULL;
    proto_pair_open(&P, 0, FP_PANEL);
    TEST_ASSERT_TRUE(proto_pair_on_start(&P, 0, COMMIT, FP_HA, NONCE_PANEL, &reply));

    proto_pair_tick(&P, PROTO_PAIR_WINDOW_MS); /* 2 minutes later */
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_FAILED, P.state);
    TEST_ASSERT_FALSE(proto_pair_on_reveal(&P, PROTO_PAIR_WINDOW_MS + 1u, NONCE_HA, &reply));
    TEST_ASSERT_EQUAL_STRING("timeout", reply);

    /* a fresh window works again */
    proto_pair_open(&P, PROTO_PAIR_WINDOW_MS + 2u, FP_PANEL);
    TEST_ASSERT_TRUE(
        proto_pair_on_start(&P, PROTO_PAIR_WINDOW_MS + 2u, COMMIT, FP_HA, NONCE_PANEL, &reply));
}

/* The user says no: rejected, and the attempt is over. */
static void test_reject(void)
{
    const char *reply = NULL;
    proto_pair_open(&P, 0, FP_PANEL);
    TEST_ASSERT_TRUE(proto_pair_on_start(&P, 0, COMMIT, FP_HA, NONCE_PANEL, &reply));
    TEST_ASSERT_TRUE(proto_pair_on_reveal(&P, 0, NONCE_HA, &reply));
    TEST_ASSERT_EQUAL_STRING("rejected", proto_pair_decide(&P, false));
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_FAILED, P.state);
    TEST_ASSERT_EQUAL_STRING("", P.code);
}

/* Closing the window by hand (the user, or the link dropping) leaves it idle. */
static void test_close(void)
{
    const char *reply = NULL;
    proto_pair_open(&P, 0, FP_PANEL);
    TEST_ASSERT_TRUE(proto_pair_on_start(&P, 0, COMMIT, FP_HA, NONCE_PANEL, &reply));
    proto_pair_close(&P);
    TEST_ASSERT_EQUAL_INT(PROTO_PAIR_IDLE, P.state);
    TEST_ASSERT_FALSE(proto_pair_on_reveal(&P, 1, NONCE_HA, &reply));
    TEST_ASSERT_EQUAL_STRING("not_pairing", reply);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_happy_path);
    RUN_TEST(test_commit_must_verify);
    RUN_TEST(test_closed_window);
    RUN_TEST(test_busy);
    RUN_TEST(test_timeout);
    RUN_TEST(test_reject);
    RUN_TEST(test_close);
    return UNITY_END();
}
