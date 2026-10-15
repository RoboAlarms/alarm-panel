/*
 * test_zw_house.c - the virtual house (docs/features/36 README, milestone Z5 "Done when"): six
 * simulated devices - sleeping and listening, S2 Authenticated, Unauthenticated and Access
 * Control, and insecure, one with two end points, one that never sends idle - added, interviewed,
 * reporting, through two panel restarts, a module restart, a pulled module and a removal, on a
 * fake clock of hours. The owner sees each device REACHABLE until it stops answering, and
 * UNREACHABLE when network.md §8 says, to the second; nothing is lost across the restarts.
 *
 * Requirements: ZW-010, ZW-011, ZW-012, ZW-016, ZW-017 end to end (and ZW-002: every device a
 * plain end node, the panel the only controller).
 */
#include <stdio.h>
#include <string.h>

#include "net_harness.h"
#include "unity.h"
#include "zwave_proto/zw_cc.h"

#define MIN_MS  60000u
#define HOUR_MS 3600000u

enum { DOOR = 0, MOTION, MULTI, LEAK, SMOKE, TILT, DEVICES };

static fake_zdev_t dev[DEVICES];
static uint16_t node_of[DEVICES];

/* What each record looked like: nothing of it may be lost. */
static zw_node_record_t before[DEVICES];

void setUp(void)
{
    h_init();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, H.zac.overflow, "the fake module ran out of room");
    for (size_t i = 0; i < DEVICES; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, dev[i].heard_asleep, "a sleeper heard the radio");
    }
}

/* ---- The devices ----------------------------------------------------------------------------- */

static void add_version(fake_zdev_t *d, uint8_t cc, uint8_t v)
{
    d->m.versions[d->m.versions_n].cc = cc;
    d->m.versions[d->m.versions_n].version = v;
    d->m.versions_n++;
}

static void add_secure(fake_zdev_t *d, uint8_t cc)
{
    d->m.secure_ccs[d->m.secure_n++] = cc;
}

/* A plain device (no Security 2): its node information, each CC at version 2 (Notification 8). */
static void plain(fake_zdev_t *d, uint64_t seed, bool listening, const uint8_t *ccs, size_t n)
{
    h_device(d, seed, !listening);
    memcpy(d->nif, ccs, n);
    d->nif_len = n;
    d->m.versions_n = 0u;
    for (size_t i = 0; i < n; i++) {
        add_version(d, ccs[i], ccs[i] == 0x71u ? 8u : 2u);
    }
    d->m.secure_n = 0u;
    d->m.listening = listening;
    d->m.auto_wake = !listening;
    d->m.zwp_role = listening ? 0x05u : 0x06u;
}

static void build_house(void)
{
    /* the front door: a sleeping contact, S2 Authenticated (its PIN typed) */
    h_device(&dev[DOOR], 0xD0u, true);
    /* the hall: a sleeping motion sensor, S2 Unauthenticated; it never sends idle */
    h_device(&dev[MOTION], 0xD1u, true);
    dev[MOTION].m.notif_types[0] = 0x07u; /* home security: motion */
    dev[MOTION].m.notif_n = 1u;
    dev[MOTION].m.product_id = 0xE002u;
    /* the kitchen: a mains multi-sensor, S2 Access Control, two end points - a contact, a leak */
    h_device(&dev[MULTI], 0xD2u, false);
    fake_zdev_listening(&dev[MULTI]);
    add_version(&dev[MULTI], 0x60u, 4u);
    add_secure(&dev[MULTI], 0x60u);
    dev[MULTI].m.notif_n = 0u;
    dev[MULTI].m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &dev[MULTI].m.eps[e];
        ep->generic = 0x07u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x71u;
        ep->secure_n = 1u;
        ep->notif_types[0] = e == 0u ? 0x06u : 0x05u;
        ep->notif_n = 1u;
    }
    /* the basement: an insecure mains leak sensor (Binary Sensor, water) */
    static const uint8_t LEAK_CCS[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x30};
    plain(&dev[LEAK], 0xD3u, true, LEAK_CCS, sizeof LEAK_CCS);
    dev[LEAK].m.binary_types[0] = 0x06u;
    dev[LEAK].m.binary_n = 1u;
    /* the landing: an insecure sleeping smoke detector */
    static const uint8_t SMOKE_CCS[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x80, 0x84};
    plain(&dev[SMOKE], 0xD4u, false, SMOKE_CCS, sizeof SMOKE_CCS);
    dev[SMOKE].m.notif_types[0] = 0x01u;
    dev[SMOKE].m.notif_n = 1u;
    /* the garage door: a sleeping tilt sensor, S2 Access Control (Binary Sensor, tilt) */
    h_device(&dev[TILT], 0xD5u, true);
    add_version(&dev[TILT], 0x30u, 2u);
    add_secure(&dev[TILT], 0x30u);
    dev[TILT].m.binary_types[0] = 0x0Bu;
    dev[TILT].m.binary_n = 1u;
    dev[TILT].m.notif_n = 0u;
}

static const uint8_t GRANT[DEVICES] = {ZW_S2_KEY_AUTH, ZW_S2_KEY_UNAUTH, ZW_S2_KEY_ACCESS, 0u, 0u,
                                       ZW_S2_KEY_ACCESS};

static const zw_sec_class_t CLASS[DEVICES] = {ZW_SEC_S2_AUTH, ZW_SEC_S2_UNAUTH, ZW_SEC_S2_ACCESS,
                                              ZW_SEC_NONE, ZW_SEC_NONE, ZW_SEC_S2_ACCESS};

/* ---- Helpers --------------------------------------------------------------------------------- */

static void expect_ready(size_t i)
{
    char msg[80];
    snprintf(msg, sizeof msg, "device %u (node %u) not ready", (unsigned)i, node_of[i]);
    TEST_ASSERT_TRUE_MESSAGE(h_until_ready(node_of[i], 60000u), msg);
}

static const zw_health_node_t *health_of(uint16_t node)
{
    const zw_health_t *h = zw_net_health(&H.net);
    for (size_t i = 0; i < h->node_count; i++) {
        if (h->nodes[i].node_id == node) {
            return &h->nodes[i];
        }
    }
    return NULL;
}

/* Every device reports once (the motion sensor three times, never idle); the owner gets each
 * from its node, end point and class. */
static void everyone_reports(void)
{
    const size_t rx0 = H.rx_n;
    const uint8_t door[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    const uint8_t motion[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x08, 0x00};
    const uint8_t contact[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    const uint8_t leak_ep[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x02, 0x00};
    const uint8_t water[] = {0x30, 0x03, 0xFF, 0x06};
    const uint8_t smoke[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x01, 0x02, 0x00};
    const uint8_t tilt[] = {0x30, 0x03, 0xFF, 0x0B};
    uint8_t mc[32];
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[DOOR], H.wire.now, ZW_SEC_AUTO, door,
                                                     sizeof door));
    h_run_for(2000u);
    for (unsigned k = 0; k < 3u; k++) { /* motion, motion, motion - and never "idle" */
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[MOTION], H.wire.now, ZW_SEC_AUTO,
                                                         motion, sizeof motion));
        h_run_for(2000u);
    }
    size_t n = zw_cc_wrap_multi_channel(mc, sizeof mc, 1u, 0u, false, contact, sizeof contact);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[MULTI], H.wire.now, ZW_SEC_S2_ACCESS, mc,
                                                     n));
    h_run_for(2000u);
    n = zw_cc_wrap_multi_channel(mc, sizeof mc, 2u, 0u, false, leak_ep, sizeof leak_ep);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[MULTI], H.wire.now, ZW_SEC_S2_ACCESS, mc,
                                                     n));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[LEAK], H.wire.now, ZW_SEC_NONE, water,
                                                     sizeof water));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[SMOKE], H.wire.now, ZW_SEC_NONE, smoke,
                                                     sizeof smoke));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev[TILT], H.wire.now, ZW_SEC_AUTO, tilt,
                                                     sizeof tilt));
    h_run_for(2000u);
    /* what arrived, in order */
    static const struct {
        size_t dev;
        uint8_t ep, cc, event;
    } WANT[] = {{DOOR, 0, 0x71, 0x16},  {MOTION, 0, 0x71, 0x08}, {MOTION, 0, 0x71, 0x08},
                {MOTION, 0, 0x71, 0x08}, {MULTI, 1, 0x71, 0x16}, {MULTI, 2, 0x71, 0x02},
                {LEAK, 0, 0x30, 0x06},   {SMOKE, 0, 0x71, 0x02}, {TILT, 0, 0x30, 0x0B}};
    size_t got = 0u;
    for (size_t i = rx0; i < H.rx_n && got < sizeof WANT / sizeof WANT[0]; i++) {
        const h_rx_t *r = &H.rx[i];
        const size_t d = WANT[got].dev;
        if (r->node != node_of[d] || r->cmd[0] != WANT[got].cc) {
            continue; /* the interview's and supervision's answers go by too */
        }
        const uint8_t event = r->cmd[0] == 0x71 ? r->cmd[7] : r->cmd[3];
        TEST_ASSERT_EQUAL_UINT8(WANT[got].ep, r->ep);
        TEST_ASSERT_EQUAL_UINT8(WANT[got].event, event);
        TEST_ASSERT_EQUAL_INT(CLASS[d], r->security);
        got++;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(sizeof WANT / sizeof WANT[0], got, "a report went missing");
}

static void remember(void)
{
    for (size_t i = 0; i < DEVICES; i++) {
        const zw_node_record_t *rec = h_rec(node_of[i]);
        TEST_ASSERT_NOT_NULL(rec);
        before[i] = *rec;
    }
}

/* Nothing of any record was lost, and nobody is interviewed again. */
static void nothing_lost(size_t skip)
{
    for (size_t i = 0; i < DEVICES; i++) {
        if (i == skip) {
            continue;
        }
        const zw_node_record_t *rec = h_rec(node_of[i]);
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL_UINT16(before[i].generation, rec->generation);
        TEST_ASSERT_EQUAL_HEX8(before[i].security_classes, rec->security_classes);
        TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, rec->interview_step);
        TEST_ASSERT_EQUAL(before[i].lifeline_set, rec->lifeline_set);
        TEST_ASSERT_EQUAL_UINT32(before[i].wake_up_interval_seconds, rec->wake_up_interval_seconds);
        TEST_ASSERT_EQUAL_UINT8(before[i].cc_count, rec->cc_count);
        TEST_ASSERT_EQUAL_UINT8(before[i].endpoints[2].notification_count,
                                rec->endpoints[2].notification_count);
        TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 0u));
        TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node_of[i]));
    }
}

/* ---- The house ------------------------------------------------------------------------------- */

/* ZW-010, ZW-011, ZW-012, ZW-016, ZW-017: see the file's comment. */
static void test_virtual_house(void)
{
    build_house();

    /* 1. Six devices added, one at a time; each interviewed while it is awake from joining. */
    for (size_t i = 0; i < DEVICES; i++) {
        H.grant = GRANT[i];
        node_of[i] = h_include(&dev[i]);
        TEST_ASSERT_EQUAL_UINT16(2u + i, node_of[i]);
        expect_ready(i);
        const zw_node_record_t *rec = h_rec(node_of[i]);
        TEST_ASSERT_EQUAL_UINT16(1u + i, rec->generation);
        TEST_ASSERT_EQUAL_HEX8(GRANT[i], rec->security_classes);
        TEST_ASSERT_EQUAL_INT(CLASS[i], zw_s2_highest(zw_net_s2(&H.net), node_of[i]));
        TEST_ASSERT_TRUE(rec->lifeline_set);
        TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 0u));
        if (!dev[i].m.listening) { /* §7: the check-in, the panel as destination */
            TEST_ASSERT_EQUAL_UINT32(3600u, rec->wake_up_interval_seconds);
            TEST_ASSERT_EQUAL_UINT32(3600u, dev[i].m.wake_interval);
            TEST_ASSERT_EQUAL_UINT8(1u, dev[i].m.wake_node);
        }
    }
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node_of[MULTI]), 2u));
    TEST_ASSERT_EQUAL_UINT8(0x05u, h_rec(node_of[MULTI])->endpoints[2].notifications[0]
                                        .notification_type);
    static const uint8_t MC_DEST[3] = {0x00, 0x01, 0x00}; /* §6: node 1, end point 0 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MC_DEST, dev[MULTI].m.mca_dest, 3);

    /* 2. They report - nothing stored for a report (network.md §2: writes on changes only). */
    const unsigned stores = H.stores;
    everyone_reports();
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);

    /* 3. Two hours of ordinary life: the sleepers check in hourly and are sent back to sleep, the
     * mains devices are pinged when quiet; nobody is unreachable. */
    h_clear_events();
    uint32_t wakeups[DEVICES], nmis[DEVICES];
    for (size_t i = 0; i < DEVICES; i++) {
        wakeups[i] = dev[i].wakeups;
        nmis[i] = dev[i].nmi_heard;
    }
    h_run_for(2u * HOUR_MS);
    TEST_ASSERT_EQUAL_UINT(0u, h_count(ZW_NET_EV_UNREACHABLE, 0u));
    for (size_t i = 0; i < DEVICES; i++) {
        if (!dev[i].m.listening) {
            TEST_ASSERT_EQUAL_UINT32(wakeups[i] + 2u, dev[i].wakeups);
            TEST_ASSERT_EQUAL_UINT32(nmis[i] + 2u, dev[i].nmi_heard); /* 250 ms after each */
        }
    }
    TEST_ASSERT_TRUE(zw_net_stats(&H.net)->pings >= 4u); /* the two mains devices, quiet */

    /* 4. The panel restarts: every device back from the stored cache, none interviewed again,
     * none unreachable for what the panel did not hear before it started (§8), their reports
     * still taken (the SPANs resync). */
    remember();
    unsigned asked_identity[DEVICES];
    for (size_t i = 0; i < DEVICES; i++) {
        asked_identity[i] = fake_zdev_asked(&dev[i], 0x72, 0x04);
    }
    h_clear_events();
    h_boot();
    h_run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(0u, h_count(ZW_NET_EV_UNREACHABLE, 0u));
    nothing_lost(DEVICES);
    everyone_reports();
    for (size_t i = 0; i < DEVICES; i++) {
        TEST_ASSERT_EQUAL_UINT(asked_identity[i], fake_zdev_asked(&dev[i], 0x72, 0x04));
    }
    /* a sleeper's next check-in comes before its T + 10 min: nobody unreachable */
    h_clear_events();
    h_run_for(HOUR_MS + 11u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(0u, h_count(ZW_NET_EV_UNREACHABLE, 0u));

    /* 5. The module restarts by itself: the same network - the SPANs stay, nothing lost. */
    const unsigned ups = H.ups;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&H.zac, H.wire.now + 10u, 0x03u));
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(ups + 1u, H.ups);
    nothing_lost(DEVICES);
    everyone_reports();

    /* 6. The module is pulled out for twenty minutes: the link goes down (the panel notices at its
     * next command - a quiet mains device's ping) and comes back when it is plugged in again;
     * nothing lost, and reachability starts afresh (§8: nothing heard before counts). */
    H.zac.fault.dead = true;
    h_run_for(20u * MIN_MS);
    TEST_ASSERT_NOT_EQUAL(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
    H.zac.fault.dead = false;
    h_run_for(6u * MIN_MS); /* the next start-up attempt */
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
    nothing_lost(DEVICES);
    h_clear_events();
    h_run_for(HOUR_MS + 11u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(0u, h_count(ZW_NET_EV_UNREACHABLE, 0u));
    everyone_reports();

    /* 7. The smoke detector is removed ("Remove a device", its button): forgotten, its zones
     * CHECK "sensor removed"; the others untouched. */
    h_clear_events();
    fake_zdev_wake(&dev[SMOKE], H.wire.now); /* awake for its button press */
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    h_run_for(200u);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&H.zac, H.wire.now, &dev[SMOKE]));
    h_run_for(2000u);
    const zw_net_event_t *removed = h_find(ZW_NET_EV_NODE_REMOVED, node_of[SMOKE]);
    TEST_ASSERT_NOT_NULL(removed);
    TEST_ASSERT_EQUAL_UINT16(before[SMOKE].generation, removed->generation);
    TEST_ASSERT_NULL(h_rec(node_of[SMOKE]));
    TEST_ASSERT_NULL(health_of(node_of[SMOKE]));
    nothing_lost(SMOKE);

    /* 8. Two devices stop answering - the owner hears it when §8 says. The leak sensor (mains)
     * loses power: pinged after 15 min of silence, again 15 min later, then UNREACHABLE. The
     * garage door's battery dies: UNREACHABLE T + 10 min after the last frame it sent. */
    h_run_for(5u * MIN_MS);
    h_clear_events();
    const zw_health_node_t *tilt = health_of(node_of[TILT]);
    const zw_health_node_t *leak = health_of(node_of[LEAK]);
    TEST_ASSERT_NOT_NULL(tilt);
    TEST_ASSERT_NOT_NULL(leak);
    dev[TILT].m.auto_wake = false;
    dev[LEAK].status = FAKE_ZDEV_OUT;
    const uint64_t tilt_due = tilt->last_heard_ms + 3600000u + 600000u;
    const uint64_t leak_quiet = leak->last_heard_ms;
    h_run_for(2u * HOUR_MS);
    const zw_net_event_t *u_tilt = NULL, *u_leak = NULL;
    uint64_t at_tilt = 0u, at_leak = 0u;
    for (size_t i = 0; i < H.ev_n; i++) {
        if (H.ev[i].kind == ZW_NET_EV_UNREACHABLE && H.ev[i].node == node_of[TILT]) {
            u_tilt = &H.ev[i];
            at_tilt = H.ev_at[i];
        }
        if (H.ev[i].kind == ZW_NET_EV_UNREACHABLE && H.ev[i].node == node_of[LEAK]) {
            u_leak = &H.ev[i];
            at_leak = H.ev_at[i];
        }
    }
    TEST_ASSERT_NOT_NULL(u_tilt);
    TEST_ASSERT_EQUAL_UINT64(tilt_due, at_tilt); /* to the millisecond */
    TEST_ASSERT_EQUAL_UINT16(before[TILT].generation, u_tilt->generation);
    TEST_ASSERT_NOT_NULL(u_leak);
    /* the first ping at 15 min of silence (3 attempts), the second 15 min after it failed */
    TEST_ASSERT_TRUE(at_leak >= leak_quiet + 30u * MIN_MS);
    TEST_ASSERT_TRUE(at_leak <= leak_quiet + 30u * MIN_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT(2u, h_count(ZW_NET_EV_UNREACHABLE, 0u)); /* no one else */

    /* 9. The garage door gets a new battery and wakes: REACHABLE again. */
    dev[TILT].m.auto_wake = true;
    fake_zdev_wake(&dev[TILT], H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REACHABLE, node_of[TILT]));

    /* 10. The panel restarts once more: what it stored is what it had. */
    zw_node_cache_t stored;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&stored, H.blob, H.blob_len));
    TEST_ASSERT_EQUAL_UINT(DEVICES - 1u, zw_node_count(&stored));
    h_boot();
    h_run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(DEVICES - 1u, zw_node_count(zw_net_nodes(&H.net)));
    nothing_lost(SMOKE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_virtual_house);
    return UNITY_END();
}
