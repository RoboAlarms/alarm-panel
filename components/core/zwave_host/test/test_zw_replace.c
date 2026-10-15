/*
 * test_zw_replace.c - replacing a failed device (docs/features/36 network.md §10; ZW-024, ZW-029):
 * zw_net against the fake ZAC93 - the old device checked first as for removing a failed one (3
 * pings 2 s apart, then IsFailedNode: the module takes ReplaceFailedNode only for a node it lists
 * as failed, and lists one only after a SendData to it got no answer); the new device takes the
 * failed one's node ID and generation (its zones follow it), its key exchange with its own PIN, a
 * fresh record and interview; a device that still answers is not replaced; the radio's refusals
 * each keep their own result; the check or the window stopped, or the window run out; nothing
 * else runs meanwhile.
 */
#include <string.h>

#include "net_harness.h"
#include "unity.h"
#include "zwave_host/zw_remove.h"

void setUp(void)
{
    h_init();
}
void tearDown(void) {}

static const zw_incl_event_t *done_after(size_t from, uint64_t ms)
{
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end) {
        for (size_t i = from; i < H.incl_n; i++) {
            if (H.incl[i].kind == ZW_INCL_EV_DONE) {
                return &H.incl[i];
            }
        }
        h_run_for(200u);
    }
    return NULL;
}

static bool listening_after(size_t from)
{
    for (size_t i = from; i < H.incl_n; i++) {
        if (H.incl[i].kind == ZW_INCL_EV_LISTENING && H.incl[i].replaced) {
            return true;
        }
    }
    return false;
}

/* The window opens once the check is over (3 pings 2 s apart, IsFailedNode): within 15 s. */
static bool listening_within(size_t from, uint64_t ms)
{
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end && !listening_after(from)) {
        h_run_for(200u);
    }
    return listening_after(from);
}

/* The door's device breaks: it hears nothing. The radio doesn't list it as failed yet - only
 * the check's pings, unanswered, make it do so. */
static uint16_t broken_door(fake_zdev_t *old)
{
    h_device(old, 0xB0B0u, false);
    fake_zdev_listening(old);
    const uint16_t node = h_include(old);
    TEST_ASSERT_TRUE(h_until_ready(node, 120000u));
    old->silent = true;
    TEST_ASSERT_FALSE(zw_node_list_test(H.zac.failed, node));
    return node;
}

/* ReplaceFailedNode's response scripted: `why` (00 started, else its refusal bits), then the
 * report `status` (none when refused). */
static void script_replace(uint8_t why, bool report, uint8_t status)
{
    fake_rule_t rule;
    memset(&rule, 0, sizeof rule);
    rule.func = (uint8_t)ZW_FUNC_REPLACE_FAILED_NODE;
    rule.respond = true;
    rule.response.len = 1u;
    rule.response.bytes[0] = why;
    if (report) {
        rule.callbacks = 1u;
        rule.callback[0].len = 1u;
        rule.callback[0].bytes[0] = status;
        rule.callback[0].after_ms = 40u;
    }
    TEST_ASSERT_TRUE(fake_zac93_on(&H.zac, &rule));
}

/* ZW-024: a new device in the failed one's place - same node ID and generation, its own keys (its
 * PIN typed), its record read again; nothing else runs meanwhile, the check included */
static void test_replacement_takes_its_place(void)
{
    static fake_zdev_t old, fresh;
    const uint16_t node = broken_door(&old);
    const uint16_t generation = h_rec(node)->generation;
    h_device(&fresh, 0xF4E5u, false);
    fake_zdev_listening(&fresh);
    fresh.m.product_id = 0x7777u; /* another model */
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &fresh));
    H.pin = fresh.pin;
    h_clear_events();
    const size_t from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_NOT_ALLOWED, zw_net_replace_failed(&H.net, H.wire.now, false, node));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BAD_NODE_ID, zw_net_replace_failed(&H.net, H.wire.now, true, 99u));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BUSY, zw_net_add_device(&H.net, H.wire.now, true, 2u, 0u));
    h_run_for(3000u); /* the check runs: the network is still held */
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BUSY, zw_net_add_device(&H.net, H.wire.now, true, 2u, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_BUSY, zw_net_remove_device(&H.net, H.wire.now, true));
    TEST_ASSERT_TRUE_MESSAGE(listening_within(from, 15000u), "the window did not open");
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.replace_asks);
    TEST_ASSERT_TRUE(fake_zac93_press_replace(&H.zac, H.wire.now, &fresh));
    const zw_incl_event_t *e = done_after(from, 120000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->replaced);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, e->result);
    TEST_ASSERT_EQUAL_UINT16(node, e->node);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_SEC_S2_ACCESS, e->security);
    TEST_ASSERT_EQUAL_HEX8(0x07, fresh.have); /* the new device holds the keys */
    TEST_ASSERT_TRUE(h_until_ready(node, 120000u));
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_EQUAL_UINT16(generation, rec->generation); /* its zones follow it */
    TEST_ASSERT_EQUAL_HEX16(0x7777u, rec->product_id);      /* read again: the new model */
    TEST_ASSERT_EQUAL_HEX8(0x07, rec->security_classes);
    TEST_ASSERT_TRUE(zw_health_is_reachable(zw_net_health(&H.net), node));
}

/* ZW-024, ZW-029 (a): the most common case - a battery sensor whose battery died. The panel sent it
 * nothing since its last check-in, so the radio's failed list is empty: the check's pings get no
 * answer, the radio lists it, IsFailedNode says so, and only then ReplaceFailedNode - which the
 * radio takes; the replacement completes. */
static void test_dead_sleeper_is_checked_then_replaced(void)
{
    static fake_zdev_t old, fresh;
    h_device(&old, 0x5EE9u, true);
    const uint16_t node = h_include(&old);
    TEST_ASSERT_TRUE(h_until_ready(node, 120000u));
    h_run_for(20000u); /* asleep */
    old.silent = true;  /* its battery died */
    old.awake_until = 0u;
    TEST_ASSERT_FALSE(zw_node_list_test(H.zac.failed, node)); /* no reason to list it yet */
    h_device(&fresh, 0xF4E6u, false);
    fake_zdev_listening(&fresh);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &fresh));
    H.pin = fresh.pin;
    const uint32_t sends = H.zac.by_func[ZW_FUNC_SEND_DATA];
    const uint32_t checks = H.zac.by_func[ZW_FUNC_IS_FAILED_NODE];
    const size_t from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    h_run_for(200u);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.replace_asks); /* not before the check */
    TEST_ASSERT_TRUE_MESSAGE(listening_within(from, 15000u), "the window did not open");
    TEST_ASSERT_TRUE(H.zac.by_func[ZW_FUNC_SEND_DATA] - sends >= ZW_REMOVE_PINGS); /* its pings */
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.by_func[ZW_FUNC_IS_FAILED_NODE] - checks);
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.replace_asks);
    TEST_ASSERT_TRUE(fake_zac93_press_replace(&H.zac, H.wire.now, &fresh));
    const zw_incl_event_t *e = done_after(from, 120000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->replaced);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, e->result);
    TEST_ASSERT_EQUAL_UINT16(node, e->node);
}

/* ZW-024, ZW-029 (b): a device that answers a ping is not replaced - STILL_ANSWERS, and
 * ReplaceFailedNode is never sent, even when the radio still listed it from an old NoAck */
static void test_answering_device_kept(void)
{
    static fake_zdev_t old;
    const uint16_t node = broken_door(&old);
    old.silent = false; /* it was only behind a wall */
    zw_node_list_set(H.zac.failed, node, true);
    const size_t from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    const zw_incl_event_t *e = done_after(from, 20000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->replaced);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STILL_ANSWERS, e->result);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.replace_asks);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REPLACE_FAILED_NODE]);
    TEST_ASSERT_EQUAL_UINT16(node, h_rec(node)->node_id); /* nothing changed */
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_IDLE, zw_remove_state(zw_net_rm(&H.net)));
    static fake_zdev_t other;
    h_device(&other, 0x0DD6u, false);
    TEST_ASSERT_NOT_EQUAL(0u, h_include(&other)); /* the network is free again */
}

/* ZW-029 (c): the radio's own answers - not listed as failed (IsFailedNode no: NOT_FAILED, no
 * ReplaceFailedNode); listed, but ReplaceFailedNode refused with bit 08 (NOT_FAILED), busy (10:
 * BUSY) or another bit (REFUSED); started, but its report says the node answered (STILL_ANSWERS) */
static void test_radio_refusals(void)
{
    static fake_zdev_t old;
    const uint16_t node = broken_door(&old);
    H.zac.failed_off = true; /* the radio never lists it */
    size_t from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    const zw_incl_event_t *e = done_after(from, 20000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_NOT_FAILED, e->result);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REPLACE_FAILED_NODE]);
    H.zac.failed_off = false;

    static const struct {
        uint8_t why;
        bool report;
        uint8_t status;
        zw_incl_result_t want;
    } CASES[] = {
        {0x08u, false, 0u, ZW_INCL_NOT_FAILED}, /* listed (IsFailedNode yes), yet refused: 08 */
        {0x10u, false, 0u, ZW_INCL_BUSY},
        {0x02u, false, 0u, ZW_INCL_REFUSED}, /* not the primary */
        {0x00u, true, 0x00u, ZW_INCL_STILL_ANSWERS}, /* the radio heard it after all */
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        script_replace(CASES[i].why, CASES[i].report, CASES[i].status);
        const uint32_t asks = H.zac.by_func[ZW_FUNC_REPLACE_FAILED_NODE];
        const uint32_t checks = H.zac.by_func[ZW_FUNC_IS_FAILED_NODE];
        from = H.incl_n;
        TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED,
                              zw_net_replace_failed(&H.net, H.wire.now, true, node));
        e = done_after(from, 20000u);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_INT_MESSAGE(CASES[i].want, e->result, "the radio's answer");
        TEST_ASSERT_EQUAL_UINT32(1u, H.zac.by_func[ZW_FUNC_IS_FAILED_NODE] - checks);
        TEST_ASSERT_EQUAL_UINT32(1u, H.zac.by_func[ZW_FUNC_REPLACE_FAILED_NODE] - asks);
        TEST_ASSERT_TRUE(zw_node_list_test(H.zac.failed, node)); /* the module listed it */
    }
    TEST_ASSERT_EQUAL_UINT16(node, h_rec(node)->node_id); /* nothing changed */
}

/* ZW-024: stopped while the old device is checked - STOPPED at once, nothing sent to the radio
 * after, the check over with it (the network free) */
static void test_stopped_during_the_check(void)
{
    static fake_zdev_t old;
    const uint16_t node = broken_door(&old);
    const size_t from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    h_run_for(2500u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_PINGING, zw_remove_state(zw_net_rm(&H.net)));
    zw_incl_stop(zw_net_incl(&H.net), H.wire.now);
    const zw_incl_event_t *e = done_after(from, 1u); /* at once */
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STOPPED, e->result);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_IDLE, zw_remove_state(zw_net_rm(&H.net)));
    const uint32_t checks = H.zac.by_func[ZW_FUNC_IS_FAILED_NODE];
    const size_t n = H.incl_n;
    h_run_for(10000u);
    TEST_ASSERT_EQUAL_UINT32(checks, H.zac.by_func[ZW_FUNC_IS_FAILED_NODE]);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REPLACE_FAILED_NODE]);
    TEST_ASSERT_EQUAL_size_t(n, H.incl_n); /* nothing more */
    static fake_zdev_t other;
    h_device(&other, 0x0DD7u, false);
    TEST_ASSERT_NOT_EQUAL(0u, h_include(&other));
}

/* ZW-024: the window stopped by the installer, or run out: nothing replaced, the network free */
static void test_window_stopped_or_run_out(void)
{
    static fake_zdev_t old;
    const uint16_t node = broken_door(&old);
    const uint16_t generation = h_rec(node)->generation;
    size_t from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    TEST_ASSERT_TRUE(listening_within(from, 15000u));
    zw_incl_stop(zw_net_incl(&H.net), H.wire.now);
    const zw_incl_event_t *e = done_after(from, 5000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STOPPED, e->result);
    h_run_for(500u); /* its AddNode(stop) on the wire */
    TEST_ASSERT_FALSE(H.zac.replace_ready);
    from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_replace_failed(&H.net, H.wire.now, true, node));
    e = done_after(from, 90000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_NO_DEVICE, e->result);
    TEST_ASSERT_EQUAL_UINT16(generation, h_rec(node)->generation);
    static fake_zdev_t other;
    h_device(&other, 0x0DD5u, false);
    TEST_ASSERT_NOT_EQUAL(0u, h_include(&other)); /* free again */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_replacement_takes_its_place);
    RUN_TEST(test_dead_sleeper_is_checked_then_replaced);
    RUN_TEST(test_answering_device_kept);
    RUN_TEST(test_radio_refusals);
    RUN_TEST(test_stopped_during_the_check);
    RUN_TEST(test_window_stopped_or_run_out);
    return UNITY_END();
}
