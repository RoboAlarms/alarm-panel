/* A keypad's own wired inputs as the master's zones (docs/features/11 LINK-055; ZSRC_KEYPAD):
 * reports reach only watched terminals, a lost frame's opening is not lost, a keypad the link
 * stops hearing makes its zones unreachable, learning finds the terminal being tested. */
#include "unity.h"
#include "zone_hub/zsrc_keypad.h"

#include <string.h>

void setUp(void);
void tearDown(void);

typedef struct {
    zsrc_address_t a;
    int32_t value; /* -1: a reachability call */
    bool up;
} call_t;

static call_t calls[64];
static size_t ncalls;
static zsrc_keypad_t K;

static void on_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                     uint64_t now)
{
    (void)ctx;
    (void)state;
    (void)now;
    if (ncalls < 64u) {
        calls[ncalls++] = (call_t){.a = *a, .value = value, .up = true};
    }
}
static void on_reach(void *ctx, const zsrc_address_t *a, bool up, uint64_t now)
{
    (void)ctx;
    (void)now;
    if (ncalls < 64u) {
        calls[ncalls++] = (call_t){.a = *a, .value = -1, .up = up};
    }
}

void setUp(void)
{
    const zsrc_sink_t sink = {.input = on_input, .reachable = on_reach};
    zsrc_keypad_init(&K, &sink);
    ncalls = 0;
}
void tearDown(void) {}

static void report(uint8_t device, uint8_t gpio, bool active, uint16_t changes, uint64_t now)
{
    const zsrc_keypad_term_t t[2] = {
        {.gpio = 4, .flags = ZSRC_KEYPAD_KNOWN, .changes = 0},
        {.gpio = gpio, .flags = (uint8_t)(ZSRC_KEYPAD_KNOWN | (active ? ZSRC_KEYPAD_ACTIVE : 0u)),
         .changes = changes},
    };
    zsrc_keypad_report(&K, device, t, 2, now);
}

static void watch(uint8_t device, uint8_t gpio)
{
    const zsrc_address_t a = zsrc_keypad_address(device, gpio);
    zsrc_mapping_t m;
    memset(&m, 0, sizeof(m));
    TEST_ASSERT_TRUE(zsrc_keypad_ops()->watch(&K, &a, &m, 0));
}

/* LINK-055: an address is the keypad and its pin; only watched terminals are reported. */
static void test_addresses_and_only_watched_terminals_report(void)
{
    const zsrc_address_t a = zsrc_keypad_address(2, 5);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_KEYPAD, a.kind);
    TEST_ASSERT_EQUAL_UINT64(0x0205u, a.id);
    const zone_source_ops_t *ops = zsrc_keypad_ops();
    TEST_ASSERT_TRUE(ops->valid(&K, &a)); /* not heard yet: bindings load at start */
    const zsrc_address_t bad = zsrc_keypad_address(9, 5);
    TEST_ASSERT_FALSE(ops->valid(&K, &bad));
    report(2, 5, false, 0, 10);
    TEST_ASSERT_EQUAL_UINT(0, ncalls); /* nothing watched */
    const zsrc_address_t gone = zsrc_keypad_address(2, 6);
    TEST_ASSERT_FALSE(ops->valid(&K, &gone)); /* heard: only its terminals */
    watch(2, 5);
    TEST_ASSERT_EQUAL_UINT(1, ncalls); /* its level at once */
    TEST_ASSERT_EQUAL_INT32(0, calls[0].value);
    report(2, 5, true, 1, 20);
    TEST_ASSERT_EQUAL_UINT(2, ncalls);
    TEST_ASSERT_EQUAL_INT32(1, calls[1].value);
    TEST_ASSERT_TRUE(zsrc_address_equal(&calls[1].a, &a));
    report(2, 5, true, 1, 30); /* the periodic repeat: nothing new */
    TEST_ASSERT_EQUAL_UINT(2, ncalls);
    char words[48];
    ops->describe(&K, &a, words, sizeof(words));
    TEST_ASSERT_EQUAL_STRING("IO5 on keypad 2", words);
}

/* LINK-055: a door opened and closed while its frames were lost still faults the zone. */
static void test_a_lost_opening_is_not_lost(void)
{
    report(1, 7, false, 4, 0);
    watch(1, 7);
    ncalls = 0;
    report(1, 7, false, 6, 10); /* two changes, the same level: an opening came and went */
    TEST_ASSERT_EQUAL_UINT(2, ncalls);
    TEST_ASSERT_EQUAL_INT32(1, calls[0].value);
    TEST_ASSERT_EQUAL_INT32(0, calls[1].value);
    ncalls = 0;
    report(1, 7, true, 9, 20); /* three changes, now open: closed-open seen in between */
    TEST_ASSERT_EQUAL_UINT(2, ncalls);
    TEST_ASSERT_EQUAL_INT32(0, calls[0].value);
    TEST_ASSERT_EQUAL_INT32(1, calls[1].value);
    ncalls = 0;
    report(1, 7, false, 10, 30); /* one change: just the level */
    TEST_ASSERT_EQUAL_UINT(1, ncalls);
    TEST_ASSERT_EQUAL_INT32(0, calls[0].value);
    ncalls = 0;
    report(1, 7, false, 0xFFFFu, 40); /* wrapping counts: 65535 moves, odd, same level */
    report(1, 7, true, 0x0000u, 50);  /* one more, wrapping to 0 */
    TEST_ASSERT_TRUE(ncalls >= 1u);
    TEST_ASSERT_EQUAL_INT32(1, calls[ncalls - 1u].value);
}

/* ZONE-008: the link stops hearing the keypad: its watched inputs are unreachable (CHECK after
 * their grace), and reachable again with its next report. */
static void test_a_keypad_unheard_makes_its_zones_unreachable(void)
{
    report(3, 15, false, 0, 0);
    watch(3, 15);
    ncalls = 0;
    zsrc_keypad_online(&K, 3, false, 10);
    TEST_ASSERT_EQUAL_UINT(1, ncalls);
    TEST_ASSERT_EQUAL_INT32(-1, calls[0].value);
    TEST_ASSERT_FALSE(calls[0].up);
    report(3, 15, false, 0, 20);
    TEST_ASSERT_EQUAL_INT32(-1, calls[ncalls - 1u].value);
    TEST_ASSERT_TRUE(calls[ncalls - 1u].up);
    ncalls = 0;
    zsrc_keypad_forget(&K, 3, 30);
    TEST_ASSERT_EQUAL_UINT(1, ncalls);
    TEST_ASSERT_FALSE(calls[0].up);
    TEST_ASSERT_FALSE(K.dev[2].known);
    TEST_ASSERT_TRUE((K.dev[2].watched & (UINT64_C(1) << 15)) != 0u); /* still bound */
}

/* A keypad that stops reporting (its 5 s repeats) is unheard after 20 s, without the link's
 * minutes-long supervision. */
static void test_a_silent_keypad_is_unheard_after_twenty_seconds(void)
{
    report(4, 16, false, 0, 1000);
    watch(4, 16);
    const zone_source_ops_t *ops = zsrc_keypad_ops();
    TEST_ASSERT_EQUAL_UINT64(21000u, ops->next_tick(&K, 1000));
    report(4, 16, false, 0, 6000); /* the repeat */
    ncalls = 0;
    ops->tick(&K, 25999);
    TEST_ASSERT_EQUAL_UINT(0, ncalls);
    ops->tick(&K, 26000);
    TEST_ASSERT_EQUAL_UINT(1, ncalls);
    TEST_ASSERT_FALSE(calls[0].up);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ops->next_tick(&K, 26000));
}

/* Learning: every keypad's terminals are offered, the one that moved marked. */
static void test_learning_marks_the_terminal_being_tested(void)
{
    report(1, 5, false, 0, 0);
    report(2, 6, false, 0, 0);
    const zone_source_ops_t *ops = zsrc_keypad_ops();
    TEST_ASSERT_TRUE(ops->learn(&K, true, 0));
    report(2, 6, true, 1, 10);
    zsrc_candidate_t c[8];
    const size_t n = ops->candidates(&K, "", 0, c, 8);
    TEST_ASSERT_EQUAL_UINT(4, n);
    size_t marked = 0;
    for (size_t i = 0; i < n; i++) {
        if (c[i].changed) {
            marked++;
            TEST_ASSERT_EQUAL_STRING("IO6", c[i].name);
            TEST_ASSERT_EQUAL_STRING("Keypad 2", c[i].detail);
            TEST_ASSERT_EQUAL_STRING("open", c[i].state);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, marked);
    TEST_ASSERT_TRUE(ops->learn(&K, false, 20));
    TEST_ASSERT_EQUAL_UINT(4, ops->candidates(&K, "", 0, c, 8));
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_FALSE(c[i].changed);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_addresses_and_only_watched_terminals_report);
    RUN_TEST(test_a_lost_opening_is_not_lost);
    RUN_TEST(test_a_keypad_unheard_makes_its_zones_unreachable);
    RUN_TEST(test_a_silent_keypad_is_unheard_after_twenty_seconds);
    RUN_TEST(test_learning_marks_the_terminal_being_tested);
    return UNITY_END();
}
