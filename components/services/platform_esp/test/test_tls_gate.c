/* SEC-012 (docs/bugs/B033): TLS handshakes are rationed before mbedTLS allocates, so a flood from
 * the home network can't take the RAM the panel's Wi-Fi needs. */
#include <string.h>

#include "tls_gate.h"
#include "unity.h"

#define RAM_OK 100000u, 30000u

static void addr_of(uint8_t out[16], uint8_t last)
{
    memset(out, 0, 16);
    out[10] = 0xff;
    out[11] = 0xff;
    out[12] = 192;
    out[13] = 0;
    out[14] = 2;
    out[15] = last;
}

void setUp(void) {}
void tearDown(void) {}

/* SEC-012: a browser's page load (a burst of connections) and a phone reconnecting every second
 * are never turned away. */
static void test_ordinary_use_is_never_refused(void)
{
    tls_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t browser[16], phone[16];
    addr_of(browser, 10);
    addr_of(phone, 11);
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL(TLS_GATE_ADMIT, tls_gate_admit(&g, browser, 1000, RAM_OK));
    }
    for (int64_t t = 0; t < 600; t++) { /* ten minutes of a phone reconnecting every second */
        TEST_ASSERT_EQUAL(TLS_GATE_ADMIT, tls_gate_admit(&g, phone, 2000000 + t * 1000000, RAM_OK));
    }
}

/* SEC-012: one address flooding gets its burst, then one handshake per refill; others still get in. */
static void test_a_flooding_address_is_held_to_its_rate(void)
{
    tls_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t flood[16], phone[16];
    addr_of(flood, 66);
    addr_of(phone, 11);
    unsigned admitted = 0;
    for (int64_t t = 0; t < 10000000; t += 10000) { /* 100 a second for 10 s */
        admitted += tls_gate_admit(&g, flood, t, RAM_OK) == TLS_GATE_ADMIT;
    }
    TEST_ASSERT_LESS_OR_EQUAL(TLS_GATE_PEER_BURST + 10000000 / TLS_GATE_PEER_REFILL_US + 1, admitted);
    TEST_ASSERT_GREATER_THAN(0, g.counts[TLS_GATE_PEER_LIMIT]);
    TEST_ASSERT_EQUAL(TLS_GATE_ADMIT, tls_gate_admit(&g, phone, 10000000, RAM_OK));
}

/* SEC-012: failed handshakes (a scanner, a stalled connection) use the bucket up faster. */
static void test_failed_handshakes_cost_more(void)
{
    tls_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t scan[16];
    addr_of(scan, 99);
    unsigned admitted = 0;
    for (unsigned i = 0; i < TLS_GATE_PEER_BURST; i++) {
        if (tls_gate_admit(&g, scan, 0, RAM_OK) == TLS_GATE_ADMIT) {
            admitted++;
            tls_gate_failed(&g, scan, 0);
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL((TLS_GATE_PEER_BURST + TLS_GATE_FAIL_COST - 1) / TLS_GATE_FAIL_COST, admitted);
    TEST_ASSERT_EQUAL(TLS_GATE_PEER_LIMIT, tls_gate_admit(&g, scan, 0, RAM_OK));
}

/* SEC-012: many addresses together are held to the shared rate. */
static void test_many_addresses_share_one_limit(void)
{
    tls_gate_t g;
    memset(&g, 0, sizeof(g));
    unsigned admitted = 0;
    for (int64_t t = 0; t < 1000000; t += 5000) { /* 200 a second from 200 addresses, 1 s */
        uint8_t a[16];
        addr_of(a, (uint8_t)(t / 5000));
        admitted += tls_gate_admit(&g, a, t, RAM_OK) == TLS_GATE_ADMIT;
    }
    TEST_ASSERT_LESS_OR_EQUAL(TLS_GATE_ALL_BURST + 1000000 / TLS_GATE_ALL_REFILL_US + 1, admitted);
    TEST_ASSERT_GREATER_THAN(0, g.counts[TLS_GATE_ALL_LIMIT]);
}

/* SEC-012: with internal RAM short, nobody starts a handshake; the tokens aren't spent. */
static void test_short_internal_ram_refuses_everyone(void)
{
    tls_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t phone[16];
    addr_of(phone, 11);
    TEST_ASSERT_EQUAL(TLS_GATE_LOW_MEMORY, tls_gate_admit(&g, phone, 0, TLS_GATE_MIN_FREE - 1u, 30000u));
    TEST_ASSERT_EQUAL(TLS_GATE_LOW_MEMORY, tls_gate_admit(&g, phone, 0, 100000u, TLS_GATE_MIN_BLOCK - 1u));
    for (unsigned i = 0; i < TLS_GATE_PEER_BURST; i++) {
        TEST_ASSERT_EQUAL(TLS_GATE_ADMIT, tls_gate_admit(&g, phone, 0, RAM_OK));
    }
}

/* F20: an address that proved who it is still gets in while other addresses use up the shared
 * bucket, and a flood of new addresses can't push it out of its slot. */
static void test_a_trusted_address_skips_the_shared_bucket(void)
{
    tls_gate_t g;
    memset(&g, 0, sizeof(g));
    uint8_t phone[16];
    addr_of(phone, 11);
    TEST_ASSERT_EQUAL(TLS_GATE_ADMIT, tls_gate_admit(&g, phone, 0, RAM_OK));
    tls_gate_trust(&g, phone, 0);
    for (int64_t t = 0; t < 1000000; t += 5000) { /* 200 addresses at 200 a second */
        uint8_t a[16];
        addr_of(a, (uint8_t)(100 + t / 5000));
        (void)tls_gate_admit(&g, a, t, RAM_OK);
    }
    TEST_ASSERT_EQUAL(0, g.all_tokens);
    TEST_ASSERT_EQUAL(TLS_GATE_ADMIT, tls_gate_admit(&g, phone, 995000, RAM_OK)); /* with the last of them */
    /* an address that didn't prove itself waits for the shared bucket */
    uint8_t other[16];
    addr_of(other, 12);
    TEST_ASSERT_EQUAL(TLS_GATE_ALL_LIMIT, tls_gate_admit(&g, other, 995000, RAM_OK));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ordinary_use_is_never_refused);
    RUN_TEST(test_a_flooding_address_is_held_to_its_rate);
    RUN_TEST(test_failed_handshakes_cost_more);
    RUN_TEST(test_many_addresses_share_one_limit);
    RUN_TEST(test_short_internal_ram_refuses_everyone);
    RUN_TEST(test_a_trusted_address_skips_the_shared_bucket);
    return UNITY_END();
}
