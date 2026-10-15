/*
 * test_zw_routes.c - rebuilding routes (docs/features/36 network.md §11; ZW-024): zw_net against
 * the fake ZAC93 - every device in turn (listening ones first, a sleeper at its wake-up, kept
 * awake while it is repaired), a neighbour update tried again up to 5 times, a device that
 * doesn't answer skipped, one device on its own, the installer stopping it, the radio going away,
 * and nothing else running meanwhile.
 */
#include <string.h>

#include "net_harness.h"
#include "unity.h"

void setUp(void)
{
    h_init();
}
void tearDown(void) {}

static const zw_net_event_t *routes_end(uint64_t ms)
{
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end) {
        for (size_t i = 0; i < H.ev_n; i++) {
            if (H.ev[i].kind == ZW_NET_EV_ROUTES && H.ev[i].routes.kind == ZW_ROUTES_EV_END) {
                return &H.ev[i];
            }
        }
        h_run_for(500u);
    }
    return NULL;
}

static uint16_t add(fake_zdev_t *d, uint64_t seed, bool sleeps)
{
    h_device(d, seed, sleeps);
    if (!sleeps) {
        fake_zdev_listening(d);
    }
    const uint16_t node = h_include(d);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_TRUE(h_until_ready(node, 120000u));
    return node;
}

/* ZW-024: every device - the listening ones in order, the sleeper at its wake-up (no No More
 * Information until it is done); nothing else runs meanwhile; the job's progress in numbers */
static void test_every_device(void)
{
    static fake_zdev_t lamp, plug, door;
    const uint16_t a = add(&lamp, 0x1111u, false);
    const uint16_t b = add(&plug, 0x2222u, false);
    const uint16_t c = add(&door, 0x3333u, true);
    h_run_for(5000u);
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED, zw_net_rebuild_routes(&H.net, H.wire.now, false, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_BUSY, zw_net_rebuild_routes(&H.net, H.wire.now, true, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BUSY, zw_net_add_device(&H.net, H.wire.now, true, 3u, 0u));
    h_run_for(10000u);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)H.zac.routed_n);
    TEST_ASSERT_EQUAL_UINT16(a, H.zac.routed[0]);
    TEST_ASSERT_EQUAL_UINT16(b, H.zac.routed[1]);
    TEST_ASSERT_TRUE(H.zac.suc_route_asks >= 2u);
    TEST_ASSERT_TRUE(H.zac.delete_route_asks >= 2u);
    TEST_ASSERT_TRUE(zw_routes_running(zw_net_routes(&H.net))); /* the door waits */
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_ASLEEP, zw_routes_node_state(zw_net_routes(&H.net), c));
    uint16_t done, total, failed, skipped, waiting;
    zw_routes_counts(zw_net_routes(&H.net), &done, &total, &failed, &skipped, &waiting);
    TEST_ASSERT_EQUAL_UINT16(2u, done);
    TEST_ASSERT_EQUAL_UINT16(3u, total);
    TEST_ASSERT_EQUAL_UINT16(1u, waiting);

    const uint32_t nmi = door.nmi_heard;
    fake_zdev_wake(&door, H.wire.now);
    const zw_net_event_t *e = routes_end(60000u);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the job did not end when the sleeper woke");
    TEST_ASSERT_FALSE(e->routes.stopped);
    TEST_ASSERT_EQUAL_UINT16(3u, e->routes.done);
    TEST_ASSERT_EQUAL_UINT16(0u, e->routes.failed);
    TEST_ASSERT_EQUAL_UINT16(c, H.zac.routed[2]);
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_DONE, zw_routes_node_state(zw_net_routes(&H.net), c));
    h_run_for(2000u);
    TEST_ASSERT_TRUE(door.nmi_heard > nmi); /* released: it may sleep again */
    TEST_ASSERT_FALSE(zw_routes_running(zw_net_routes(&H.net)));
    static fake_zdev_t more;
    h_device(&more, 0x4444u, false);
    TEST_ASSERT_NOT_EQUAL(0u, h_include(&more)); /* free again */
}

/* ZW-024: a neighbour update that fails is tried again, up to 5 times; one device on its own */
static void test_retries_and_one_device(void)
{
    static fake_zdev_t lamp, plug;
    const uint16_t a = add(&lamp, 0x5151u, false);
    (void)add(&plug, 0x5252u, false);
    h_clear_events();
    H.zac.neighbor_fails = 2u;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, a));
    const zw_net_event_t *e = routes_end(30000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT16(1u, e->routes.total); /* only it */
    TEST_ASSERT_EQUAL_UINT16(0u, e->routes.failed);
    TEST_ASSERT_EQUAL_UINT32(3u, H.zac.neighbor_asks);

    h_clear_events();
    H.zac.neighbor_asks = 0u;
    H.zac.neighbor_fails = 9u;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, a));
    e = routes_end(30000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT16(1u, e->routes.failed);
    TEST_ASSERT_EQUAL_UINT32(ZW_ROUTES_TRIES, H.zac.neighbor_asks);
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_rebuild_routes(&H.net, H.wire.now, true, 77u));
}

/* No Operation frames to `node` the module heard from index `from` on (the log must not wrap). */
static unsigned noops_to(uint16_t node, size_t from)
{
    unsigned n = 0u;
    for (size_t i = from; i < H.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&H.zac, i);
        TEST_ASSERT_NOT_NULL_MESSAGE(h, "the fake module's log wrapped");
        if (h->func == ZW_FUNC_SEND_DATA && h->payload_len >= 3u && h->payload[0] == node &&
            h->payload[1] == 1u && h->payload[2] == 0x00u) {
            n++;
        }
    }
    return n;
}

/* ZW-024: a device the panel counts dead is asked first and skipped when silent. ZW-037: that
 * check is the panel's own ping - one No Operation on the air, not 3 transmissions. */
static void test_dead_device_skipped(void)
{
    static fake_zdev_t lamp, gone;
    const uint16_t a = add(&lamp, 0x6161u, false);
    const uint16_t g = add(&gone, 0x6262u, false);
    gone.silent = true; /* unplugged */
    for (unsigned i = 0; i < 60u && zw_health_is_reachable(zw_net_health(&H.net), g); i++) {
        h_run_for(60000u); /* §8: pinged after 15 min of silence, lost after 2 failed pings */
    }
    TEST_ASSERT_FALSE(zw_health_is_reachable(zw_net_health(&H.net), g));
    h_clear_events();
    H.zac.routed_n = 0u;
    const size_t from = H.zac.heard;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, 0u));
    const zw_net_event_t *e = routes_end(60000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT(1u, noops_to(g, from));
    TEST_ASSERT_EQUAL_UINT16(1u, e->routes.skipped);
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_SKIPPED, zw_routes_node_state(zw_net_routes(&H.net), g));
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_DONE, zw_routes_node_state(zw_net_routes(&H.net), a));
    for (size_t i = 0; i < H.zac.routed_n; i++) {
        TEST_ASSERT_NOT_EQUAL(g, H.zac.routed[i]); /* never asked to update its neighbours */
    }
}

/* ZW-024: a sleeper counted unreachable is skipped at the start; the job ends once the listening
 * devices are done, without waiting for a wake-up that never comes */
static void test_dead_sleeper_skipped(void)
{
    static fake_zdev_t lamp, door;
    const uint16_t a = add(&lamp, 0x8181u, false);
    const uint16_t c = add(&door, 0x8282u, true);
    door.silent = true; /* removed from the wall: no more wake-ups */
    door.next_wake = 0u;
    for (unsigned i = 0; i < 3000u && zw_health_is_reachable(zw_net_health(&H.net), c); i++) {
        h_run_for(60000u); /* §8: a sleeper is lost after missing its wake-ups */
    }
    TEST_ASSERT_FALSE(zw_health_is_reachable(zw_net_health(&H.net), c));
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, 0u));
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_SKIPPED, zw_routes_node_state(zw_net_routes(&H.net), c));
    const zw_net_event_t *e = routes_end(60000u);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the job waited for a dead sleeper");
    TEST_ASSERT_FALSE(e->routes.stopped);
    TEST_ASSERT_EQUAL_UINT16(1u, e->routes.skipped);
    TEST_ASSERT_EQUAL_UINT16(0u, e->routes.waiting);
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_DONE, zw_routes_node_state(zw_net_routes(&H.net), a));
    TEST_ASSERT_FALSE(zw_routes_running(zw_net_routes(&H.net)));
}

/* ZW-024: the installer stops it while it waits for a sleeper; the radio going away ends it */
static void test_stopped_and_radio_lost(void)
{
    static fake_zdev_t lamp, door;
    (void)add(&lamp, 0x7171u, false);
    const uint16_t c = add(&door, 0x7272u, true);
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, 0u));
    h_run_for(10000u);
    TEST_ASSERT_TRUE(zw_routes_running(zw_net_routes(&H.net)));
    zw_net_stop_routes(&H.net, H.wire.now);
    const zw_net_event_t *e = routes_end(2000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->routes.stopped);
    TEST_ASSERT_EQUAL_UINT8(ZW_ROUTES_SKIPPED, zw_routes_node_state(zw_net_routes(&H.net), c));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_rebuild_routes(&H.net, H.wire.now, true, 0u));
    h_clear_events();
    H.zac.fault.dead = true;
    e = routes_end(120000u);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the job outlived its radio");
    TEST_ASSERT_TRUE(e->routes.stopped);
    TEST_ASSERT_FALSE(zw_routes_running(zw_net_routes(&H.net)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_device);
    RUN_TEST(test_retries_and_one_device);
    RUN_TEST(test_dead_device_skipped);
    RUN_TEST(test_dead_sleeper_skipped);
    RUN_TEST(test_stopped_and_radio_lost);
    return UNITY_END();
}
