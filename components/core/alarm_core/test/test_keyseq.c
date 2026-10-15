/*
 * test_keyseq.c - keypad key sequences with 4-10 digit codes.
 * Requirements: CORE-KEY-*
 *
 * Key letters: O=OFF W=AWAY S=STAY N=NIGHT M=MAX I=INSTANT T=TEST Y=BYPASS
 * U=CODE H=CHIME, plus digits, '*', '#' and panic keys A/B/C.
 */
#include "sim.h"

static alarm_keyseq_t KS;
static alarm_keyseq_result_t R[16];
static int NR;
static alarm_ms_t T;
static sim_t S;

void setUp(void)
{
    alarm_config_t cfg;
    alarm_config_defaults(&cfg);
    alarm_keyseq_init(&KS, &cfg.opt, 1);
    NR = 0;
    T = SEC(100);
    memset(R, 0, sizeof(R));
}

void tearDown(void) {}

static void feed(const char *keys)
{
    for (const char *k = keys; *k != '\0'; k++) {
        T += 100;
        alarm_keyseq_result_t out;
        if (alarm_keyseq_key(&KS, *k, T, &out) && NR < 16) {
            R[NR++] = out;
        }
    }
}

static void expect_arm(int i, alarm_arm_level_t level, uint8_t flags, const char *code)
{
    TEST_ASSERT_EQUAL_INT(ALARM_KS_COMMAND, R[i].kind);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, R[i].cmd.type);
    TEST_ASSERT_EQUAL_STRING(alarm_level_name(level), alarm_level_name(R[i].cmd.level));
    TEST_ASSERT_EQUAL_UINT8(flags, R[i].cmd.arm_flags);
    TEST_ASSERT_EQUAL_STRING(code, R[i].cmd.code);
    TEST_ASSERT_EQUAL_UINT8(1, R[i].cmd.partition);
}

static void expect_cmd(int i, alarm_cmd_type_t type, const char *code)
{
    TEST_ASSERT_EQUAL_INT(ALARM_KS_COMMAND, R[i].kind);
    TEST_ASSERT_EQUAL_INT(type, R[i].cmd.type);
    TEST_ASSERT_EQUAL_STRING(code, R[i].cmd.code);
}

/* CORE-KEY-001: code + command key. */
static void test_code_plus_command_keys(void)
{
    feed("1111W1111O1111S1111N1111M1111I1111T1111H");
    TEST_ASSERT_EQUAL_INT(8, NR);
    expect_arm(0, ALARM_LEVEL_AWAY, 0, "1111");
    expect_cmd(1, ALARM_CMD_DISARM, "1111");
    expect_arm(2, ALARM_LEVEL_STAY, 0, "1111");
    expect_arm(3, ALARM_LEVEL_NIGHT, 0, "1111");
    expect_arm(4, ALARM_LEVEL_AWAY, ALARM_ARM_NO_ENTRY_DELAY, "1111");  /* MAXIMUM */
    expect_arm(5, ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY, "1111");  /* INSTANT */
    expect_cmd(6, ALARM_CMD_WALK_TEST, "1111");
    expect_cmd(7, ALARM_CMD_CHIME, "1111");
}

/* CORE-KEY-002: codes of any length from 4 to 10 digits. */
static void test_variable_length_codes(void)
{
    feed("123456W12345678W1234567890W69420O");
    TEST_ASSERT_EQUAL_INT(4, NR);
    expect_arm(0, ALARM_LEVEL_AWAY, 0, "123456");
    expect_arm(1, ALARM_LEVEL_AWAY, 0, "12345678");
    expect_arm(2, ALARM_LEVEL_AWAY, 0, "1234567890");
    expect_cmd(3, ALARM_CMD_DISARM, "69420");
}

/* CORE-KEY-003: fewer than 4 or more than 10 digits is an error. */
static void test_code_length_limits(void)
{
    feed("123W");
    TEST_ASSERT_EQUAL_INT(1, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_KS_ERROR, R[0].kind);
    feed("12345678901");
    TEST_ASSERT_EQUAL_INT(2, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_KS_ERROR, R[1].kind);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_keyseq_deadline(&KS));
}

/* CORE-KEY-004: OFF alone silences (fire, CO, aux, trouble beeps) without a code. */
static void test_off_alone_silences(void)
{
    feed("O");
    TEST_ASSERT_EQUAL_INT(1, NR);
    expect_cmd(0, ALARM_CMD_SILENCE, "");
}

/* CORE-KEY-005: AWAY / STAY / NIGHT alone = quick arm, CHIME alone = chime without a code
 * (both only if the engine's option allows it, CORE-ARM-006 and CORE-CHIME-005); other keys
 * need a code. */
static void test_command_keys_alone(void)
{
    feed("WSNHT");
    TEST_ASSERT_EQUAL_INT(5, NR);
    expect_arm(0, ALARM_LEVEL_AWAY, 0, "");
    expect_arm(1, ALARM_LEVEL_STAY, 0, "");
    expect_arm(2, ALARM_LEVEL_NIGHT, 0, "");
    expect_cmd(3, ALARM_CMD_CHIME, "");
    TEST_ASSERT_EQUAL_INT(ALARM_KS_ERROR, R[4].kind);
}

/* CORE-KEY-006: code + BYPASS + zone numbers, # to finish. */
static void test_bypass_zones(void)
{
    feed("1111Y003004#");
    TEST_ASSERT_EQUAL_INT(2, NR);
    expect_cmd(0, ALARM_CMD_BYPASS, "1111");
    TEST_ASSERT_EQUAL_UINT16(3, R[0].cmd.zone);
    TEST_ASSERT_EQUAL_UINT16(4, R[1].cmd.zone);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_keyseq_deadline(&KS));
}

/* CORE-KEY-007: code + BYPASS + # = quick bypass. */
static void test_quick_bypass(void)
{
    feed("1111Y#");
    TEST_ASSERT_EQUAL_INT(1, NR);
    expect_cmd(0, ALARM_CMD_QUICK_BYPASS, "1111");
}

/* CORE-KEY-008: code + CODE + user + new code + #, and code + CODE + user + # 0. */
static void test_user_programming(void)
{
    feed("123456U0824681357#");
    TEST_ASSERT_EQUAL_INT(1, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_KS_USER_SET, R[0].kind);
    TEST_ASSERT_EQUAL_UINT8(8, R[0].user_no);
    TEST_ASSERT_EQUAL_STRING("24681357", R[0].new_code);
    TEST_ASSERT_EQUAL_STRING("123456", R[0].cmd.code);

    feed("123456U08#0");
    TEST_ASSERT_EQUAL_INT(2, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_KS_USER_DELETE, R[1].kind);
    TEST_ASSERT_EQUAL_UINT8(8, R[1].user_no);

    feed("123456U08123#"); /* new code too short */
    TEST_ASSERT_EQUAL_INT(3, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_KS_ERROR, R[2].kind);
}

/* CORE-KEY-009: # 2 / # 3 quick arm, # 9 quick exit. */
static void test_hash_quick_functions(void)
{
    feed("#2#3#9#5");
    TEST_ASSERT_EQUAL_INT(4, NR);
    expect_arm(0, ALARM_LEVEL_AWAY, 0, "");
    expect_arm(1, ALARM_LEVEL_STAY, 0, "");
    expect_cmd(2, ALARM_CMD_EXIT_RESTART, "");
    TEST_ASSERT_EQUAL_INT(ALARM_KS_ERROR, R[3].kind);
}

/* CORE-KEY-010 */
static void test_star_and_goto(void)
{
    feed("*");
    TEST_ASSERT_EQUAL_INT(ALARM_KS_STAR, R[0].kind);
    feed("1111*2");
    TEST_ASSERT_EQUAL_INT(ALARM_KS_GOTO, R[1].kind);
    TEST_ASSERT_EQUAL_UINT8(2, R[1].partition);
}

/* CORE-KEY-011: panic keys follow the configured mapping and discard a half-typed code. */
static void test_panic_keys(void)
{
    feed("ABC");
    TEST_ASSERT_EQUAL_INT(3, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, R[0].cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_POLICE, R[0].cmd.panic);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_MEDICAL, R[1].cmd.panic);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_FIRE, R[2].cmd.panic);
    feed("12A1111W");
    TEST_ASSERT_EQUAL_INT(5, NR);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, R[3].cmd.type);
    TEST_ASSERT_EQUAL_STRING("", R[3].cmd.code);
    expect_arm(4, ALARM_LEVEL_AWAY, 0, "1111");
}

/* CORE-KEY-012: partial sequences are dropped after the key timeout. */
static void test_abandoned_sequence(void)
{
    feed("12");
    T += ALARM_KEYSEQ_KEY_TIMEOUT_MS;
    alarm_keyseq_tick(&KS, T);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_keyseq_deadline(&KS));
    feed("1111W");
    TEST_ASSERT_EQUAL_INT(1, NR);
    expect_arm(0, ALARM_LEVEL_AWAY, 0, "1111");
}

/* CORE-KEY-013: digit count for masked dots (iPhone-style), never the digits. */
static void test_digit_count_for_dots(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, alarm_keyseq_digits(&KS));
    feed("12345");
    TEST_ASSERT_EQUAL_UINT8(5, alarm_keyseq_digits(&KS));
    feed("U0899");
    TEST_ASSERT_EQUAL_UINT8(2, alarm_keyseq_digits(&KS));
    alarm_keyseq_clear(&KS);
    TEST_ASSERT_EQUAL_UINT8(0, alarm_keyseq_digits(&KS));
}

/* CORE-KEY-014: parser output drives the engine directly (default master code). */
static void test_end_to_end_with_engine(void)
{
    sim_start(&S, NULL, NULL);
    feed("123456W");
    TEST_ASSERT_EQUAL_INT(1, NR);
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &R[0].cmd));
    EXPECT_STATE(&S, 1, ALARM_STATE_EXIT_DELAY);
    EXPECT_LEVEL(&S, 1, ALARM_LEVEL_AWAY);
    feed("1111O");
    EXPECT_RESULT(ALARM_OK, sim_cmd(&S, &R[1].cmd));
    EXPECT_STATE(&S, 1, ALARM_STATE_DISARMED);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_code_plus_command_keys);
    RUN_TEST(test_variable_length_codes);
    RUN_TEST(test_code_length_limits);
    RUN_TEST(test_off_alone_silences);
    RUN_TEST(test_command_keys_alone);
    RUN_TEST(test_bypass_zones);
    RUN_TEST(test_quick_bypass);
    RUN_TEST(test_user_programming);
    RUN_TEST(test_hash_quick_functions);
    RUN_TEST(test_star_and_goto);
    RUN_TEST(test_panic_keys);
    RUN_TEST(test_abandoned_sequence);
    RUN_TEST(test_digit_count_for_dots);
    RUN_TEST(test_end_to_end_with_engine);
    return UNITY_END();
}
