/*
 * test_zw_interview.c - keeping track of devices (docs/features/36 network.md §2, §5-§8, §10,
 * §12): zw_net with interviews on, against the fake ZAC93 and simulated devices that answer from
 * their models, on a fake clock.
 *
 * Requirements: ZW-010 (the interview, resumable across restarts and sleep, the lifeline, push or
 * pull, no inputs from pull-mode or lifeline-less devices), ZW-011 (wake-up interval, queue, No
 * More Information, a device's own check-in), ZW-012 (supervision wired, by the interval that
 * took), ZW-016 (removing), ZW-017 (keys, node cache, the home-ID cases), ZW-018 (region, network
 * reset), ZW-004 (restarting the radio), ZW-002 (the only controller: a controller feeds no
 * zone, another controller's node updates change nothing), ZW-006 and ZW-015 (a failed key
 * exchange stays insecure across a restart, and its node information is asked again), ZW-007 and
 * ZW-009 (end point traffic of an S2 node encrypted - the bridge's Gets too - replies not held
 * back), ZW-005 (an inclusion not disturbed by other nodes' interviews), ZW-013 (an end point's
 * own Notification interviewed there).
 */
#include <stdio.h>
#include <string.h>

#include "net_harness.h"
#include "unity.h"
#include "zone_hub/zsrc_zwave.h"
#include "zwave_host/zw_fwupd.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_zones/zwave_zones.h"

static fake_zdev_t dev;

void setUp(void)
{
    h_init();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, H.zac.overflow, "the fake module ran out of room");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, dev.heard_asleep, "a sleeping device heard the radio");
}

/* ---- Helpers --------------------------------------------------------------------------------- */

static const zw_node_cc_t *cc_of(const zw_node_record_t *rec, uint8_t cc)
{
    for (size_t i = 0; i < rec->cc_count; i++) {
        if (rec->ccs[i].cc == cc) {
            return &rec->ccs[i];
        }
    }
    return NULL;
}

/* Why an interview did not end, for the failure message. */
static void expect_ready(uint16_t node, uint64_t ms)
{
    if (h_until_ready(node, ms)) {
        return;
    }
    char msg[300];
    const zw_iv_t *iv = zw_net_iv(&H.net);
    const zw_iv_node_t *s = NULL;
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node == node) {
            s = &iv->slots[i];
        }
    }
    snprintf(msg, sizeof msg, "node %u not ready: step %u state %u phase %u item %u ep %u tries %u"
             " attempts %u parked %u; questions %u timeouts %u",
             node, s != NULL ? s->step : 0u, s != NULL ? s->state : 0u, s != NULL ? s->phase : 0u,
             s != NULL ? s->item : 0u, s != NULL ? s->ep : 0u, s != NULL ? s->tries : 0u,
             s != NULL ? s->attempts : 0u, s != NULL ? s->parked : 0u,
             (unsigned)iv->stats.questions, (unsigned)iv->stats.timeouts);
    TEST_FAIL_MESSAGE(msg);
}

static void run_until_step(uint16_t node, uint8_t step, uint64_t ms)
{
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end && zw_iv_step(zw_net_iv(&H.net), node) < step) {
        h_run_for(10u);
    }
    TEST_ASSERT_TRUE_MESSAGE(zw_iv_step(zw_net_iv(&H.net), node) >= step, "the step never came");
}

/* A plain (no Security 2) device whose node information and versions list `ccs`. */
static void plain_device(fake_zdev_t *d, uint64_t seed, bool listening, const uint8_t *ccs,
                         size_t n)
{
    h_device(d, seed, !listening);
    memcpy(d->nif, ccs, n);
    d->nif_len = n;
    d->m.versions_n = 0u;
    for (size_t i = 0; i < n; i++) {
        const uint8_t cc = ccs[i];
        const uint8_t v = cc == 0x71u ? 8u : cc == 0x31u ? 5u : cc == 0x60u ? 4u : 2u;
        d->m.versions[d->m.versions_n].cc = cc;
        d->m.versions[d->m.versions_n].version = v;
        d->m.versions_n++;
    }
    d->m.secure_n = 0u;
    d->m.listening = listening;
    d->m.auto_wake = !listening;
    if (listening) {
        d->m.zwp_role = 0x05u;
    }
}

/* A device already in the module's network (node 5), found by the check at the next start. */
static uint16_t found_at_start(fake_zdev_t *d)
{
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, d));
    fake_zac93_join(&H.zac, d, 5u);
    h_boot();
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_ADDED, 5u));
    return 5u;
}

/* When zw_health last heard from a node (network.md §8's clock). */
static uint64_t last_heard(uint16_t node)
{
    const zw_health_t *h = zw_net_health(&H.net);
    for (size_t i = 0; i < h->node_count; i++) {
        if (h->nodes[i].node_id == node) {
            return h->nodes[i].last_heard_ms;
        }
    }
    TEST_FAIL_MESSAGE("not supervised");
    return 0u;
}

/* The last Wake Up Interval Set a device got (after its S2 and any Supervision), or -1. */
static int got_interval_set(const fake_zdev_t *d)
{
    for (size_t i = d->got_n; i > 0u; i--) {
        if (d->got_len[i - 1u] >= 2u && d->got[i - 1u][0] == 0x84u && d->got[i - 1u][1] == 0x04u) {
            return (int)(i - 1u);
        }
    }
    return -1;
}

/* When the owner heard an event (it must have). */
static uint64_t heard_at(const zw_net_event_t *e)
{
    TEST_ASSERT_NOT_NULL(e);
    return H.ev_at[e - H.ev];
}

/* ---- The interview (ZW-010, ZW-011) ---------------------------------------------------------- */

/* ZW-010, network.md §5-§7: a secure sleeping door sensor, interviewed right after it joined -
 * every step's result in its record, the lifeline to the panel, push, the check-in interval set
 * with the panel as destination, stored; No More Information once it is done (ZW-011). */
static void test_door_sensor_interview(void)
{
    h_device(&dev, 0x5E41u, true);
    const uint16_t node = h_include(&dev);
    TEST_ASSERT_EQUAL_UINT16(2u, node);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, rec->interview_step);
    TEST_ASSERT_EQUAL_UINT16(1u, rec->generation);
    TEST_ASSERT_TRUE(rec->has_protocol);
    TEST_ASSERT_FALSE(rec->is_listening);
    TEST_ASSERT_EQUAL_HEX8(0x07u, rec->security_classes);
    TEST_ASSERT_TRUE(rec->has_dsk);
    TEST_ASSERT_TRUE(rec->has_identity);
    TEST_ASSERT_EQUAL_HEX16(0x027Au, rec->manufacturer_id);
    TEST_ASSERT_NOT_NULL(cc_of(rec, 0x71));
    TEST_ASSERT_TRUE(cc_of(rec, 0x71)->secure);
    TEST_ASSERT_EQUAL_UINT8(8u, cc_of(rec, 0x71)->version);
    TEST_ASSERT_TRUE(cc_of(rec, 0x5E)->supported);
    TEST_ASSERT_TRUE(rec->has_wake_up);
    TEST_ASSERT_EQUAL_UINT32(600u, rec->wake_up_min_seconds);
    TEST_ASSERT_EQUAL_UINT32(3600u, rec->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->wake_up_notify_node_id);
    TEST_ASSERT_EQUAL_UINT32(3600u, dev.m.wake_interval); /* §7: the default check-in, 1 h */
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.wake_node);         /* ...to the panel */
    TEST_ASSERT_TRUE(rec->has_zwave_plus);
    TEST_ASSERT_EQUAL_UINT8(0x06u, rec->zwave_plus_role_type);
    TEST_ASSERT_TRUE(rec->has_lifeline);
    TEST_ASSERT_TRUE(rec->lifeline_set);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->lifeline_group);
    TEST_ASSERT_EQUAL_UINT(1u, dev.m.assoc_n[0]);
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.assoc[0][0]);
    TEST_ASSERT_EQUAL_UINT8(2u, rec->endpoints[0].notification_count);
    TEST_ASSERT_TRUE(rec->endpoints[0].has_push_pull);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PUSH, rec->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, dev.m.notif_status[0]); /* Notification Set type FF */
    TEST_ASSERT_EQUAL_HEX8(0xFFu, dev.m.notif_status[1]);
    /* AGI says its lifeline sends Notification Reports: push without a probe - one Set a type */
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x71, 0x06));
    /* command-classes.md §4.14: every Set supervised - the interval, the lifeline, two types */
    TEST_ASSERT_EQUAL_UINT32(4u, dev.supervised);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x80, 0x02)); /* Battery Get */
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x00, 0x00)); /* no ping: it sleeps */
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO]); /* NIF known */
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, node)); /* awake all along */
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, h_find(ZW_NET_EV_NODE_READY, node)->inputs);
    /* stored as it is */
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    const zw_node_record_t *b = zw_node_find_const(&back, node);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, b->interview_step);
    TEST_ASSERT_EQUAL_UINT32(3600u, b->wake_up_interval_seconds);
    /* §7: No More Information once, after the last question - never during the interview */
    TEST_ASSERT_EQUAL_UINT32(0u, dev.nmi_heard == 1u ? 0u : dev.nmi_heard);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT32(1u, dev.nmi_heard);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now));
    /* §7: its node information (its button, no Wake Up Notification) says it is awake too */
    dev.m.wake_node = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    fake_zac93_node_info(&H.zac, H.wire.now, &dev);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT32(2u, dev.nmi_heard);
}

/* ZW-010, network.md §5: the panel restarts in the middle of the interview; it continues from the
 * step its record says - the steps done are not asked again - once the device wakes (a sleeping
 * device is presumed asleep after a restart: "Wake the device to finish"). */
static void test_interview_resumes_after_panel_restart(void)
{
    h_device(&dev, 0x5E42u, true);
    dev.m.wake_node = 1u; /* its wake-ups reach the panel from the start */
    const uint16_t node = h_include(&dev);
    run_until_step(node, ZW_IV_STEP_ENDPOINTS, 20000u);
    const uint8_t done = h_rec(node)->interview_step;
    TEST_ASSERT_TRUE(done >= ZW_IV_STEP_WAKE_UP);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x72, 0x04));
    h_clear_events();
    h_boot(); /* the panel restarts: RAM gone, the stored cache back */
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT8(done, h_rec(node)->interview_step);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    fake_zdev_wake(&dev, H.wire.now); /* its button */
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x72, 0x04)); /* identity: not again */
    TEST_ASSERT_EQUAL_UINT16(1u, h_rec(node)->generation);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
}

/* ZW-010, ZW-011, network.md §5 "Sleeping devices during the interview": the device falls asleep
 * with questions open - no try is spent on it, "Wake the device to finish"; its next wake-up
 * (its node information, its button) carries the interview on; No More Information only once it
 * is done. */
static void test_sleeping_interview_across_wakeups(void)
{
    h_device(&dev, 0x5E43u, true);
    const uint16_t node = h_include(&dev);
    TEST_ASSERT_EQUAL_HEX8(0x07u, h_rec(node)->security_classes);
    dev.m.awake_ms = 1500u; /* joined: now a sensor that dozes off quickly... */
    dev.m.awake_extend_ms = 300u;
    dev.awake_until = H.wire.now + 300u;
    h_run_for(3000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, node));
    TEST_ASSERT_EQUAL_UINT32(0u, dev.nmi_heard);
    unsigned wakes = 0u;
    while (h_find(ZW_NET_EV_NODE_READY, node) == NULL && wakes < 20u) {
        fake_zdev_wake(&dev, H.wire.now);
        fake_zac93_node_info(&H.zac, H.wire.now, &dev); /* ...woken by its button */
        wakes++;
        h_run_for(5000u);
        if (h_find(ZW_NET_EV_NODE_READY, node) == NULL) {
            /* never during the interview */
            TEST_ASSERT_EQUAL_UINT32(0u, zw_net_stats(&H.net)->no_more_info);
        }
    }
    expect_ready(node, 1000u);
    TEST_ASSERT_TRUE(wakes >= 1u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_UINT32(0u, zw_iv_stats(zw_net_iv(&H.net))->timeouts); /* no try spent */
    h_run_for(1000u);
    if (zw_net_stats(&H.net)->no_more_info != 1u) {
        const zw_health_node_t *hn = &H.net.health.nodes[0];
        char msg[200];
        snprintf(msg, sizeof msg, "nmi %u: node %u awake %d pending %d due %llu now %llu busy %d "
                 "interval %u queue %u; rec has %d interval %u dev %u",
                 (unsigned)zw_net_stats(&H.net)->no_more_info, hn->node_id, hn->awake,
                 hn->no_more_info_pending, (unsigned long long)hn->no_more_info_due_ms,
                 (unsigned long long)H.wire.now, hn->interviewing, (unsigned)hn->interval_s,
                 (unsigned)zw_health_queue_len(&H.net.health, node), h_rec(node)->has_wake_up,
                 (unsigned)h_rec(node)->wake_up_interval_seconds, (unsigned)dev.m.wake_interval);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* ---- The lifeline (§6) and push or pull (§5) ------------------------------------------------- */

/* §6: a device that is not Z-Wave Plus but names its lifeline in AGI group 1 (profile 0001). */
static void test_lifeline_from_agi(void)
{
    static const uint8_t CCS[] = {0x86, 0x72, 0x85, 0x59, 0x71};
    plain_device(&dev, 0x61u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->lifeline_group);
    TEST_ASSERT_TRUE(rec->lifeline_set);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x59, 0x03)); /* AGI Info Get */
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.assoc[0][0]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, rec->security_classes); /* found: no Security 2, no probe */
}

/* §6: neither Z-Wave Plus nor AGI: the quirks table's group; with none, no lifeline - "Does not
 * report to the panel", no inputs (ZW-010). */
static void test_lifeline_from_quirk_or_none(void)
{
    static const uint8_t CCS[] = {0x86, 0x72, 0x85, 0x71, 0x70};
    plain_device(&dev, 0x62u, true, CCS, sizeof CCS);
    dev.m.groups = 3u;
    H.has_quirk = true;
    H.quirk.lifeline_group = 3u;
    H.quirk.config_count = 1u; /* §5 step 15: the quirk's parameter */
    H.quirk.config[0].parameter = 5u;
    H.quirk.config[0].size = 1u;
    H.quirk.config[0].value = 1;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT8(3u, h_rec(node)->lifeline_group);
    TEST_ASSERT_TRUE(h_rec(node)->lifeline_set);
    TEST_ASSERT_TRUE(h_rec(node)->has_quirk);
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.assoc[2][0]); /* group 3 -> the panel */
    TEST_ASSERT_EQUAL_UINT(1u, dev.m.config_n);
    TEST_ASSERT_EQUAL_UINT8(5u, dev.m.config[0].parameter);
    TEST_ASSERT_EQUAL_INT32(1, dev.m.config[0].value);
    /* the same device without a quirk: nothing to set */
    H.has_quirk = false;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_clear_events();
    expect_ready(node, 30000u);
    TEST_ASSERT_TRUE(h_rec(node)->has_lifeline);
    TEST_ASSERT_FALSE(h_rec(node)->lifeline_set);
    TEST_ASSERT_EQUAL_UINT8(0u, h_rec(node)->lifeline_group);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NO_LIFELINE, zw_iv_inputs(h_rec(node), 0u));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NO_LIFELINE, h_find(ZW_NET_EV_NODE_READY, node)->inputs);
}

/* §6: end points none of which associates by itself: Multi Channel Association group 1 -> node
 * 1 end point 0, so the end points' reports come Multi Channel encapsulated; read back. */
static void test_lifeline_multi_channel(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x8E, 0x59, 0x60, 0x71};
    plain_device(&dev, 0x63u, true, CCS, sizeof CCS);
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        dev.m.eps[e].generic = 0x07u;
        dev.m.eps[e].specific = 0x01u;
        dev.m.eps[e].ccs[0] = 0x71u;
        dev.m.eps[e].cc_n = 1u;
        dev.m.eps[e].notif_types[0] = e == 0u ? 0x06u : 0x05u; /* a contact, a leak */
        dev.m.eps[e].notif_n = 1u;
    }
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_TRUE(rec->endpoints[1].used);
    TEST_ASSERT_TRUE(rec->endpoints[2].used);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[2].notification_count);
    TEST_ASSERT_EQUAL_UINT8(0x05u, rec->endpoints[2].notifications[0].notification_type);
    TEST_ASSERT_TRUE(rec->lifeline_set);
    static const uint8_t MC_DEST[3] = {0x00, 0x01, 0x00};
    TEST_ASSERT_EQUAL_UINT(3u, dev.m.mca_dest_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MC_DEST, dev.m.mca_dest, 3);
    TEST_ASSERT_EQUAL_UINT(0u, dev.m.assoc_n[0]); /* not the plain Association */
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 2u));
    /* an end point's report arrives from its end point */
    const uint8_t leak[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x02, 0x00};
    zw_s2_rx_t fake;
    memset(&fake, 0, sizeof fake);
    uint8_t mc[20];
    const size_t n = zw_cc_wrap_multi_channel(mc, sizeof mc, 2u, 0u, false, leak, sizeof leak);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_NONE, mc, n));
    h_run_for(100u);
    TEST_ASSERT_EQUAL_UINT8(2u, H.rx[H.rx_n - 1u].ep);
}

/* §6: the read-back does not show the panel: set again; a device that never shows it stays
 * interviewing after 5 attempts (§5). */
static void test_lifeline_read_back(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x64u, true, CCS, sizeof CCS);
    dev.m.read_back_empty = true;
    const uint16_t node = found_at_start(&dev);
    h_run_for(120000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    const zw_net_event_t *e = h_find(ZW_NET_EV_STALLED, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_LIFELINE, e->step);
    TEST_ASSERT_EQUAL_UINT(15u, fake_zdev_asked(&dev, 0x85, 0x01)); /* 5 attempts x 3 tries */
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_LIFELINE - 1u, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NOT_READY, zw_iv_inputs(h_rec(node), 0u));
}

/* §5 push or pull, without AGI: one type enabled and read - FF push (then every type enabled),
 * 00 pull (no inputs), no answer push. */
static void test_push_pull_probe(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x65u, true, CCS, sizeof CCS);
    uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PUSH, h_rec(node)->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x59, 0x05));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, dev.m.notif_status[1]); /* the other type enabled too */
    /* pull */
    dev.m.pull = true;
    memset(dev.m.notif_status, 0, sizeof dev.m.notif_status);
    const unsigned sets = fake_zdev_asked(&dev, 0x71, 0x06);
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_clear_events();
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PULL, h_rec(node)->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x71, 0x06)); /* the probe's alone */
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_PULL, zw_iv_inputs(h_rec(node), 0u));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_PULL, h_find(ZW_NET_EV_NODE_READY, node)->inputs);
    /* nothing answers the read: push - silence is the probe's answer, not a failed attempt (the
     * two Gets after it answered on their third try) */
    dev.m.pull = false;
    dev.m.mute_cc = 0x71u;
    dev.m.mute_cmd = ZW_CMD_NOTIFICATION_GET;
    dev.m.mute_n = 3u;
    const unsigned supported = fake_zdev_asked(&dev, 0x71, ZW_CMD_NOTIFICATION_SUPPORTED_GET);
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_clear_events();
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PUSH, h_rec(node)->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_UINT(supported + 1u,
                           fake_zdev_asked(&dev, 0x71, ZW_CMD_NOTIFICATION_SUPPORTED_GET));
}

/* ZW-010, ZW-024 (BL-085), network.md §5 step 12: a smoke sensor that reports through Alarm
 * Sensor - its types (smoke, CO, heat) asked once, after its Binary Sensor, and kept in the node
 * cache; its inputs are offered. */
static void test_alarm_sensor_types(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x30, 0x9C};
    plain_device(&dev, 0x7Cu, true, CCS, sizeof CCS);
    dev.m.binary_types[0] = 0x0Cu; /* motion */
    dev.m.binary_n = 1u;
    static const uint8_t TYPES[] = {ZW_ALARM_SENSOR_TYPE_SMOKE, ZW_ALARM_SENSOR_TYPE_CO,
                                    ZW_ALARM_SENSOR_TYPE_HEAT};
    memcpy(dev.m.alarm_types, TYPES, sizeof TYPES);
    dev.m.alarm_n = sizeof TYPES;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_endpoint_t *e = &h_rec(node)->endpoints[0];
    TEST_ASSERT_TRUE(e->has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(1u, e->alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_HEX8(0x16u, e->alarm_sensor_mask[0]);
    TEST_ASSERT_TRUE(e->has_binary_sensor_mask);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x30, ZW_CMD_BINARY_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
}

/* ZW-010, BL-085: a device whose Alarm Sensor Supported Report never parses (it claims more mask
 * bytes than it carries) is asked once, taken as silent - no types known - and its interview ends
 * ready with its other inputs, never stalled. */
static void test_alarm_sensor_bad_report_does_not_stall(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x9C};
    plain_device(&dev, 0x7Cu, true, CCS, sizeof CCS);
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    dev.m.alarm_bad_report = true;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 60000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    /* BL-130: asked, no types - field 12 empty, so it is not asked again at every start */
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, h_rec(node)->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(h_rec(node)));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
}

/* ZW-010, BL-085: a device that never answers Alarm Sensor's Supported Get is not stalled - asked
 * its 3 tries, the silence is an answer (no types known), and its interview ends ready. A device
 * without Alarm Sensor is never asked. */
static void test_alarm_sensor_silent_does_not_stall(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x9C};
    plain_device(&dev, 0x7Du, true, CCS, sizeof CCS);
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    dev.m.mute_cc = 0x9Cu;
    dev.m.mute_cmd = ZW_CMD_ALARM_SENSOR_SUPPORTED_GET;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 60000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_UINT(ZW_IV_TRIES, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    /* BL-130: asked, no types - field 12 empty */
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, h_rec(node)->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
}

/* The stored cache as a firmware from before the interview asked Alarm Sensor types (BL-085)
 * wrote it: the node's record without field 12. */
static void as_older_record(uint16_t node)
{
    static zw_node_cache_t c;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&c, H.blob, H.blob_len));
    zw_node_record_t *r = zw_node_find(&c, node);
    TEST_ASSERT_NOT_NULL(r);
    for (unsigned e = 0; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        zw_node_endpoint_t *x = &r->endpoints[e];
        x->has_alarm_sensor_mask = false;
        x->alarm_sensor_mask_len = 0u;
        memset(x->alarm_sensor_mask, 0, sizeof x->alarm_sensor_mask);
    }
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(r));
    H.blob_len = zw_node_cache_encode(&c, H.blob, sizeof H.blob);
    TEST_ASSERT_NOT_EQUAL(0u, H.blob_len);
    TEST_ASSERT_TRUE(zw_node_cache_decode(&c, H.blob, H.blob_len)); /* reads back so */
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(zw_node_find_const(&c, node)));
}

/* ZW-010, ZW-024 (BL-130), network.md §5 step 12: a device whose interview finished before the
 * interview asked Alarm Sensor types (no field 12) is asked them - only them - at the next start:
 * no other step again, field 0E stays done, READY so the zone source reads the types; stored, and
 * not asked at the start after. */
static void test_alarm_types_asked_after_older_interview(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x30, 0x9C};
    plain_device(&dev, 0x7Eu, true, CCS, sizeof CCS);
    dev.m.binary_types[0] = 0x0Cu; /* motion */
    dev.m.binary_n = 1u;
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(h_rec(node)));
    as_older_record(node);
    const unsigned identity = fake_zdev_asked(&dev, 0x72, 0x04);
    const unsigned binary = fake_zdev_asked(&dev, 0x30, ZW_CMD_BINARY_SENSOR_SUPPORTED_GET);
    const unsigned notif = fake_zdev_asked(&dev, 0x71, ZW_CMD_NOTIFICATION_SUPPORTED_GET);
    const unsigned battery = fake_zdev_asked(&dev, 0x80, 0x02);
    h_clear_events();
    h_boot(); /* the new firmware starts with the older cache */
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    const zw_node_endpoint_t *e = &h_rec(node)->endpoints[0];
    TEST_ASSERT_TRUE(e->has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(1u, e->alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_HEX8(0x02u, e->alarm_sensor_mask[0]);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_UINT(identity, fake_zdev_asked(&dev, 0x72, 0x04)); /* nothing else again */
    TEST_ASSERT_EQUAL_UINT(binary, fake_zdev_asked(&dev, 0x30, ZW_CMD_BINARY_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_EQUAL_UINT(notif, fake_zdev_asked(&dev, 0x71, ZW_CMD_NOTIFICATION_SUPPORTED_GET));
    TEST_ASSERT_EQUAL_UINT(battery, fake_zdev_asked(&dev, 0x80, 0x02));
    const zw_net_event_t *ready = h_find(ZW_NET_EV_NODE_READY, node);
    TEST_ASSERT_NOT_NULL(ready);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, ready->inputs);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, ready->step);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
    /* stored */
    static zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    const zw_node_record_t *b = zw_node_find_const(&back, node);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(b->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_HEX8(0x02u, b->endpoints[0].alarm_sensor_mask[0]);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, b->interview_step);
    /* the start after: nothing to ask */
    h_clear_events();
    h_boot();
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
}

/* ZW-010, BL-130: asked after an older interview, a device that never answers gets its tries, and
 * "no types" is stored (field 12 empty): never stalled, inputs offered, and not asked again at the
 * next start - no question at every start. */
static void test_alarm_types_silent_after_older_interview(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x9C};
    plain_device(&dev, 0x7Fu, true, CCS, sizeof CCS);
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    dev.m.mute_cc = 0x9Cu;
    dev.m.mute_cmd = ZW_CMD_ALARM_SENSOR_SUPPORTED_GET;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_UINT(ZW_IV_TRIES, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    as_older_record(node);
    h_clear_events();
    h_boot();
    h_run_for(60000u);
    TEST_ASSERT_EQUAL_UINT(2u * ZW_IV_TRIES,
                           fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, h_rec(node)->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    h_boot();
    h_run_for(60000u);
    TEST_ASSERT_EQUAL_UINT(2u * ZW_IV_TRIES,
                           fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
}

/* ZW-010, ZW-011, BL-130: a sleeping device asked its Alarm Sensor types after an older interview
 * - the question waits for its wake-up (never sent while it sleeps); its inputs stay offered
 * meanwhile (field 0E done, no "setting up"), and once awake it is asked and READY follows. */
static void test_alarm_types_asked_at_sleepers_wake_up(void)
{
    h_device(&dev, 0x7E10u, true);
    dev.m.secure_ccs[dev.m.secure_n++] = 0x9Cu;
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_CO;
    dev.m.alarm_n = 1u;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(0x04u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);
    h_run_for(2000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now));
    dev.m.auto_wake = false;
    as_older_record(node);
    h_clear_events();
    h_boot();
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    fake_zdev_wake(&dev, H.wire.now); /* its check-in */
    expect_ready(node, 20000u);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_HEX8(0x04u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
}

/* ZW-010, BL-130: a listening device out of reach when the catch-up asks its Alarm Sensor types
 * (powered off: no try is acknowledged) is not marked "asked, none" - nothing written, no READY,
 * never stalled, its inputs still offered - and the next network check (the next start) asks
 * again and stores its types. */
static void test_alarm_types_unreached_asked_again(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x9C};
    plain_device(&dev, 0x7E20u, true, CCS, sizeof CCS);
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    as_older_record(node);
    const unsigned asked = fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET);
    dev.silent = true; /* powered off */
    h_clear_events();
    h_boot();
    h_run_for(60000u);
    TEST_ASSERT_TRUE(zw_iv_stats(zw_net_iv(&H.net))->questions >= ZW_IV_TRIES); /* it was tried */
    TEST_ASSERT_EQUAL_UINT(asked, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_FALSE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(h_rec(node)));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    static zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(zw_node_find_const(&back, node)));
    /* the next start, the device back on */
    dev.silent = false;
    h_clear_events();
    h_boot();
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(asked + 1u,
                           fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_HEX8(0x02u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(h_rec(node)));
}

/* ZW-010, BL-130: in an interview, an Alarm Sensor question no try of which reaches the device
 * (never acknowledged) is not "asked, none" either: the interview still finishes ready, never
 * stalled, field 12 stays missing, and the next start asks it (the catch-up). */
static void test_alarm_sensor_unreached_in_interview(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x9C};
    plain_device(&dev, 0x7E21u, true, CCS, sizeof CCS);
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    dev.deaf_cc = 0x9Cu;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 60000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_FALSE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(h_rec(node)));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    dev.deaf_cc = 0u;
    h_clear_events();
    h_boot();
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_EQUAL_HEX8(0x02u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);
}

/* ZW-011, network.md §7, BL-130: while a sleeper's Alarm Sensor catch-up waits for its wake-up
 * (hours, perhaps days), its check-in can still be changed on its page - both only wait for the
 * wake-up - and at the wake-up both go: the types are stored, the new interval taken, and No More
 * Information follows. An interview of its own still refuses it. */
static void test_check_in_set_while_alarm_types_wait(void)
{
    h_device(&dev, 0x7E22u, true);
    dev.m.secure_ccs[dev.m.secure_n++] = 0x9Cu;
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_CO;
    dev.m.alarm_n = 1u;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false;
    as_older_record(node);
    h_clear_events();
    h_boot();
    h_run_for(5000u);
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), node));
    TEST_ASSERT_FALSE(zw_iv_setting_up(zw_net_iv(&H.net), node));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    const uint32_t nmi = dev.nmi_heard;
    fake_zdev_wake(&dev, H.wire.now);
    expect_ready(node, 20000u);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_HEX8(0x04u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->took);
    TEST_ASSERT_EQUAL_UINT32(43200u, dev.m.wake_interval);
    TEST_ASSERT_EQUAL_UINT32(43200u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, dev.nmi_heard);
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
    /* its own interview again ("Try its setup again"): refused meanwhile */
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    TEST_ASSERT_TRUE(zw_iv_setting_up(zw_net_iv(&H.net), node));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
}

/* ZW-010, BL-130: which records ask Alarm Sensor types after a finished interview - an end point
 * (the device itself or a used one) that lists Alarm Sensor without field 12; not an unfinished
 * interview (it asks them in its step 12), not a controller, not once field 12 is there (even
 * empty), not an unused end point's list. */
static void test_alarm_types_missing_rule(void)
{
    static zw_node_record_t r;
    memset(&r, 0, sizeof r);
    r.node_id = 9u;
    r.interview_step = ZW_IV_STEP_DONE;
    r.has_protocol = true;
    r.basic_class = 0x04u; /* an end node */
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(&r));
    r.endpoints[1].ccs[0] = 0x9Cu;
    r.endpoints[1].cc_count = 1u;
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(&r)); /* end point 1 not in use */
    r.endpoints[1].used = true;
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(&r));
    r.interview_step = ZW_IV_STEP_BINARY;
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(&r));
    r.interview_step = ZW_IV_STEP_DONE;
    r.basic_class = 0x01u; /* a controller */
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(&r));
    r.basic_class = 0x04u;
    r.endpoints[1].has_alarm_sensor_mask = true; /* asked: none */
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(&r));
    zw_iv_add_cc(&r, 0x9Cu, true, false); /* the device itself too */
    TEST_ASSERT_TRUE(zw_iv_alarm_types_missing(&r));
}

/* §5: a node without Association is pull - and has no lifeline: no inputs either way. */
static void test_no_association_is_pull(void)
{
    static const uint8_t CCS[] = {0x86, 0x72, 0x71};
    plain_device(&dev, 0x66u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PULL, h_rec(node)->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NO_LIFELINE, zw_iv_inputs(h_rec(node), 0u));
}

/* ZW-010, §5, serial-api.md 6.12: a question waits 1 s plus its round trip after its send ended,
 * is tried 3 times; the whole interview 5 times; a device that never answers stays
 * "interviewing" and the owner hears where it stopped. */
static void test_question_timing_and_stall(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x67u, true, CCS, sizeof CCS);
    dev.m.mute_cc = 0x72u; /* it never says who it is */
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    dev.watch_cc = 0x72u;
    dev.watch_cmd = 0x04u;
    const uint16_t node = found_at_start(&dev);
    h_run_for(120000u);
    const zw_net_event_t *e = h_find(ZW_NET_EV_STALLED, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_IDENTITY, e->step);
    TEST_ASSERT_EQUAL_UINT(15u, fake_zdev_asked(&dev, 0x72, 0x04));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_SECURE_CCS, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NOT_READY, zw_iv_inputs(h_rec(node), 0u));
    /* the same question again 1 s + the round trip (its SendData's 10 ms, plus the frames on the
     * wire) after its callback: never sooner, not much later */
    TEST_ASSERT_TRUE(dev.watch_n >= 3u);
    for (size_t i = 1; i < 3u; i++) {
        const uint64_t gap = dev.watch_at[i] - dev.watch_at[i - 1u];
        TEST_ASSERT_TRUE_MESSAGE(gap >= 1000u + 2u * H.zac.radio_ms, "asked again too soon");
        TEST_ASSERT_TRUE_MESSAGE(gap <= 1000u + 5u * H.zac.radio_ms, "asked again too late");
    }
}

/* ZW-029, BL-152 (Z-Wave JS's refreshInfo): a finished device read again from step 1 keeps its
 * record's field 0E done the whole time - its inputs stay offered, so its zones and keypad keep
 * working - and READY ends it as any interview. One that stops leaves field 0E done and says so
 * (zw_iv_reread_stopped, RAM only) until the next reading. One not finished still starts over
 * with field 0E cleared. */
static void test_reread_keeps_the_device_working(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x68u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    zw_iv_t *iv = &H.net.iv;
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    const unsigned identity = fake_zdev_asked(&dev, 0x72, 0x04);

    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, node));
    TEST_ASSERT_TRUE(zw_iv_setting_up(iv, node));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_PROTOCOL, zw_iv_step(iv, node));
    for (unsigned k = 0; k < 3000u && zw_iv_running(iv, node); k++) {
        h_run_for(10u);
        TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
        TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    }
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_TRUE(fake_zdev_asked(&dev, 0x72, 0x04) > identity); /* read again from step 1 */
    TEST_ASSERT_FALSE(zw_iv_refreshing(iv, node));
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, node));

    /* it stops answering who it is: the reading stops, the device works on its old facts */
    dev.m.mute_cc = 0x72u;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_run_for(120000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_FALSE(zw_iv_running(iv, node));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 0u));
    TEST_ASSERT_TRUE(zw_iv_reread_stopped(iv, node));
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, 0u));

    /* read again: the stopped one is forgotten at once, and it ends READY */
    dev.m.mute_n = 0u;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, node));
    expect_ready(node, 30000u);
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, node));

    /* a panel restart forgets a stopped re-read (RAM only): field 0E done, nothing running */
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_run_for(120000u);
    TEST_ASSERT_TRUE(zw_iv_reread_stopped(iv, node));
    dev.m.mute_n = 0u;
    h_boot();
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(&H.net.iv, node));
    TEST_ASSERT_FALSE(zw_iv_running(&H.net.iv, node));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
}

/* The device's CCs (node information and versions), changed in place: the same keys and node. */
static void set_ccs(fake_zdev_t *d, const uint8_t *ccs, size_t n)
{
    memcpy(d->nif, ccs, n);
    d->nif_len = n;
    d->m.versions_n = 0u;
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = ccs[i] == 0x71u ? 8u : ccs[i] == 0x60u ? 4u : 2u;
        d->m.versions[d->m.versions_n].cc = ccs[i];
        d->m.versions[d->m.versions_n].version = v;
        d->m.versions_n++;
    }
}

static const zw_iv_node_t *slot_c(uint16_t node)
{
    const zw_iv_t *iv = zw_net_iv(&H.net);
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node == node) {
            return &iv->slots[i];
        }
    }
    return NULL;
}

/* ZW-010, ZW-013, ZW-029 (BL-152, Z-Wave JS's refreshInfo): a re-read writes nothing of the node's
 * record and stores nothing until READY, which replaces the old facts with the new reading: a CC
 * the device no longer reports is gone, an end point it no longer has is gone, end point 0's
 * notification types are the new ones; its generation and security classes are kept, and it is
 * stored once. */
static void test_reread_replaces_the_old_facts(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71, 0x60, 0x80, 0x30};
    plain_device(&dev, 0x69u, true, CCS, sizeof CCS);
    dev.m.notif_types[0] = 0x06u;
    dev.m.notif_types[1] = 0x05u;
    dev.m.notif_n = 2u;
    dev.m.binary_types[0] = 0x0Au;
    dev.m.binary_n = 1u;
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        dev.m.eps[e].generic = 0x07u;
        dev.m.eps[e].specific = 0x01u;
        dev.m.eps[e].ccs[0] = 0x71u;
        dev.m.eps[e].cc_n = 1u;
        dev.m.eps[e].notif_types[0] = 0x06u;
        dev.m.eps[e].notif_n = 1u;
    }
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t old = *h_rec(node);
    TEST_ASSERT_NOT_NULL(cc_of(&old, 0x30u));
    TEST_ASSERT_NOT_NULL(cc_of(&old, 0x80u));
    TEST_ASSERT_TRUE(old.endpoints[2].used);
    TEST_ASSERT_EQUAL_UINT8(2u, old.endpoints[0].notification_count);

    /* new firmware: no Binary Sensor, no Battery, one end point, one notification type */
    static const uint8_t NEW[] = {0x5E, 0x86, 0x72, 0x85, 0x71, 0x60};
    set_ccs(&dev, NEW, sizeof NEW);
    dev.m.binary_n = 0u;
    dev.m.ep_n = 1u;
    dev.m.notif_types[0] = 0x07u;
    dev.m.notif_n = 1u;
    const unsigned stores = H.stores;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    TEST_ASSERT_NOT_NULL(slot_c(node));
    TEST_ASSERT_NOT_EQUAL(0u, slot_c(node)->scratch);
    for (unsigned k = 0; k < 3000u && zw_iv_running(&H.net.iv, node); k++) {
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&old, h_rec(node), sizeof old,
                                         "its record changed before READY");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(stores, H.stores, "stored before READY");
        h_run_for(10u);
    }
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_EQUAL_UINT(stores + 1u, H.stores);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_NULL(cc_of(rec, 0x30u));
    TEST_ASSERT_NULL(cc_of(rec, 0x80u));
    TEST_ASSERT_TRUE(rec->endpoints[1].used);
    TEST_ASSERT_FALSE(rec->endpoints[2].used);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[0].notification_count);
    TEST_ASSERT_EQUAL_UINT8(0x07u, rec->endpoints[0].notifications[0].notification_type);
    TEST_ASSERT_FALSE(rec->endpoints[0].has_binary_sensor_mask);
    TEST_ASSERT_EQUAL_UINT16(old.generation, rec->generation);
    TEST_ASSERT_EQUAL_UINT8(old.security_classes, rec->security_classes);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, rec->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 1u));
}

/* ZW-010, ZW-011, ZW-029 (BL-152): re-reads share ZW_IV_REREADS scratch records. With all in use
 * a listening device's re-read is refused (busy); a sleeper's waits for its wake-up holding none,
 * stays waiting at a wake-up while none is free - No More Information goes, its identity is not
 * asked - and reads at the next one. A second request for a re-read under way starts it again
 * from step 1, one waiting for its wake-up is left alone (Z-Wave JS's refreshInfo). A re-read
 * that stops is remembered without dropping a real entry of the list for a free place in it. */
static void test_reread_pool_and_sleepers(void)
{
    TEST_ASSERT_EQUAL_UINT(2u, ZW_IV_REREADS);
    static fake_zdev_t a, b, c, sl;
    fake_zdev_t *listening[3] = {&a, &b, &c};
    uint16_t n[3];
    for (size_t i = 0; i < 3u; i++) {
        h_device(listening[i], 0x7E40u + i, false);
        fake_zdev_listening(listening[i]);
        n[i] = h_include(listening[i]);
        expect_ready(n[i], 30000u);
    }
    h_device(&sl, 0x7E48u, true);
    const uint16_t sleeper = h_include(&sl);
    expect_ready(sleeper, 30000u);
    sl.m.auto_wake = false;
    h_run_for(3000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&sl, H.wire.now));

    zw_iv_t *iv = &H.net.iv;
    static const uint16_t HOLE[ZW_IV_STOPPED_MAX] = {201u, 202u, 0u, 204u, 205u, 206u, 207u, 208u};
    memcpy(iv->stopped, HOLE, sizeof HOLE);
    for (size_t i = 0; i < 2u; i++) {
        listening[i]->m.mute_cc = 0x72u; /* who it is: these readings stop */
        listening[i]->m.mute_n = FAKE_MUTE_ALWAYS;
        TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, n[i]));
    }
    TEST_ASSERT_FALSE(zw_net_interview(&H.net, H.wire.now, n[2])); /* busy */
    TEST_ASSERT_FALSE(zw_iv_running(iv, n[2]));
    h_run_for(3000u);
    TEST_ASSERT_TRUE(zw_iv_step(iv, n[0]) > ZW_IV_STEP_PROTOCOL);
    const uint8_t k0 = slot_c(n[0])->scratch;
    uint32_t started = iv->stats.started;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, n[0])); /* under way: from step 1 */
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_PROTOCOL, zw_iv_step(iv, n[0]));
    TEST_ASSERT_EQUAL_UINT32(started + 1u, iv->stats.started);
    TEST_ASSERT_EQUAL_UINT8(k0, slot_c(n[0])->scratch);

    const unsigned identity = fake_zdev_asked(&sl, 0x72, 0x04);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sleeper));
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sleeper));
    TEST_ASSERT_EQUAL_UINT8(0u, slot_c(sleeper)->scratch);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, sleeper));
    started = iv->stats.started;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sleeper)); /* waiting: left alone */
    TEST_ASSERT_EQUAL_UINT32(started, iv->stats.started);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT8(0u, slot_c(sleeper)->scratch);
    const uint32_t nmi = sl.nmi_heard;
    fake_zdev_wake(&sl, H.wire.now); /* awake, but both scratch records are in use */
    h_run_for(ZW_HEALTH_NO_MORE_INFO_MS + 1000u);
    TEST_ASSERT_TRUE_MESSAGE(sl.nmi_heard > nmi, "No More Information held by a waiting re-read");
    h_run_for(2000u);
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sleeper));
    TEST_ASSERT_EQUAL_UINT8(0u, slot_c(sleeper)->scratch);
    TEST_ASSERT_EQUAL_UINT(identity, fake_zdev_asked(&sl, 0x72, 0x04));

    for (unsigned k = 0; k < 2000u && (zw_iv_running(iv, n[0]) || zw_iv_running(iv, n[1])); k++) {
        h_run_for(100u);
    }
    TEST_ASSERT_FALSE(zw_iv_running(iv, n[0]));
    TEST_ASSERT_FALSE(zw_iv_running(iv, n[1]));
    TEST_ASSERT_TRUE(zw_iv_reread_stopped(iv, n[0]));
    TEST_ASSERT_TRUE(zw_iv_reread_stopped(iv, n[1]));
    /* the free place took the first one; only the second, in a full list, forgot the oldest */
    static const uint16_t KEPT[] = {201u, 202u, 204u, 205u, 206u, 207u};
    for (size_t i = 0; i < sizeof KEPT / sizeof KEPT[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(zw_iv_reread_stopped(iv, KEPT[i]), "a real entry was dropped");
    }
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, 208u));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(n[0])->interview_step);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(n[0]), 0u));

    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sleeper));
    h_run_for(20000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&sl, H.wire.now));
    fake_zdev_wake(&sl, H.wire.now); /* its next wake-up: a scratch record is free */
    expect_ready(sleeper, 30000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&sl, 0x72, 0x04) > identity);
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, n[2])); /* free again */
}

/* BL-152: a node removed (zw_iv_stop) is forgotten from the stopped re-reads. */
static void test_reread_stopped_list(void)
{
    zw_iv_t *iv = &H.net.iv;
    iv->stopped[ZW_IV_STOPPED_MAX - 1u] = 102u;
    TEST_ASSERT_TRUE(zw_iv_reread_stopped(iv, 102u));
    zw_iv_stop(iv, 102u);
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, 102u));
}

/* ---- Re-reads, second round (BL-152, Z-Wave JS's refreshInfo) --------------------------------- */

/* Frames the module sent to `node` from index *from on (moved past them): SendData and
 * RequestNodeInfo. */
static unsigned frames_to(uint16_t node, size_t *from)
{
    unsigned n = 0u;
    for (; *from < H.zac.heard; (*from)++) {
        const fake_heard_t *h = fake_zac93_heard(&H.zac, *from);
        TEST_ASSERT_NOT_NULL_MESSAGE(h, "the fake module's log wrapped: count more often");
        if ((h->func == ZW_FUNC_SEND_DATA || h->func == ZW_FUNC_REQUEST_NODE_INFO) &&
            h->payload_len >= 1u && h->payload[0] == node) {
            n++;
        }
    }
    return n;
}

/* Whether zw_health counts the node awake (network.md §7). */
static bool health_awake(uint16_t node)
{
    const zw_health_t *h = zw_net_health(&H.net);
    for (size_t i = 0; i < h->node_count; i++) {
        if (h->nodes[i].node_id == node) {
            return h->nodes[i].awake;
        }
    }
    return false;
}

/* No re-read holds a scratch record. */
static bool no_scratch_in_use(void)
{
    const zw_iv_t *iv = zw_net_iv(&H.net);
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node != 0u && iv->slots[i].scratch != 0u) {
            return false;
        }
    }
    return true;
}

/* A listening S2 device, READY. */
static uint16_t listening_ready(fake_zdev_t *d, uint64_t seed)
{
    h_device(d, seed, false);
    fake_zdev_listening(d);
    const uint16_t node = h_include(d);
    expect_ready(node, 30000u);
    return node;
}

/* A sleeping S2 device, READY and asleep (it wakes only when the test says). */
static uint16_t sleeper_ready(fake_zdev_t *d, uint64_t seed)
{
    h_device(d, seed, true);
    const uint16_t node = h_include(d);
    expect_ready(node, 30000u);
    d->m.auto_wake = false;
    h_run_for(35000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(d, H.wire.now));
    return node;
}

/* ZW-011, ZW-029 (BL-152, Z-Wave JS's waitForWakeup ends only at markAsAwake): a sleeper's re-read
 * that waits for its wake-up takes no scratch record and asks nothing when the device sends a
 * report, or an S2 Nonce Get - only its Wake Up Notification starts it. Both scratch records stay
 * free meanwhile. A new request forgets that its last re-read stopped. */
static void test_reread_waits_for_real_wake_up(void)
{
    static fake_zdev_t a, sl;
    const uint16_t la = listening_ready(&a, 0x7E50u);
    const uint16_t sleeper = sleeper_ready(&sl, 0x7E51u);
    zw_iv_t *iv = &H.net.iv;
    iv->stopped[0] = sleeper;
    const unsigned identity = fake_zdev_asked(&sl, 0x72, 0x04);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sleeper));
    TEST_ASSERT_FALSE(zw_iv_reread_stopped(iv, sleeper)); /* forgotten when it starts */
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sleeper));
    TEST_ASSERT_FALSE(zw_iv_asking(iv, sleeper));
    size_t from = H.zac.heard;

    static const uint8_t NOTIF[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x17, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&sl, H.wire.now, ZW_SEC_AUTO, NOTIF,
                                                     sizeof NOTIF));
    h_run_for(1000u);
    fake_zdev_restart(&sl); /* its SPAN forgotten: its next report starts with a Nonce Get */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&sl, H.wire.now, ZW_SEC_AUTO, NOTIF,
                                                     sizeof NOTIF));
    h_run_for(3000u);
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sleeper));
    TEST_ASSERT_EQUAL_UINT8(0u, slot_c(sleeper)->scratch);
    TEST_ASSERT_FALSE(zw_iv_asking(iv, sleeper));
    TEST_ASSERT_TRUE(no_scratch_in_use());
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), sleeper));
    TEST_ASSERT_EQUAL_UINT(identity, fake_zdev_asked(&sl, 0x72, 0x04));
    TEST_ASSERT_EQUAL_UINT(1u, frames_to(sleeper, &from)); /* its Nonce Report only */

    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, la)); /* a scratch record is free */
    TEST_ASSERT_NOT_EQUAL(0u, slot_c(la)->scratch);
    expect_ready(la, 30000u);
    h_clear_events();
    fake_zdev_wake(&sl, H.wire.now); /* its wake-up: the reading starts */
    expect_ready(sleeper, 30000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&sl, 0x72, 0x04) > identity);
}

/* ZW-010, ZW-029 (BL-152): sleepers' re-reads whose question waits for the device's wake-up (each
 * fell asleep mid-reading) hold no scratch record against a listening device's re-read: one gives
 * its record back, its held question leaves the device's queue, and the listening device is read
 * at once. At their next wake-up both sleepers' readings end READY, the one that gave its record
 * back from step 1 again. */
static void test_reread_parked_scratch_reclaimed(void)
{
    static fake_zdev_t a, s1, s2d;
    const uint16_t la = listening_ready(&a, 0x7E58u);
    fake_zdev_t *sl[2] = {&s1, &s2d};
    uint16_t sn[2];
    unsigned identity[2];
    for (size_t i = 0; i < 2u; i++) {
        sn[i] = sleeper_ready(sl[i], 0x7E59u + i);
    }
    zw_iv_t *iv = &H.net.iv;
    for (size_t i = 0; i < 2u; i++) {
        identity[i] = fake_zdev_asked(sl[i], 0x72, 0x04);
        sl[i]->m.awake_ms = 300u; /* it falls asleep mid-reading */
        sl[i]->m.awake_extend_ms = 0u;
        TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sn[i]));
        fake_zdev_wake(sl[i], H.wire.now);
    }
    h_run_for(5000u);
    for (size_t i = 0; i < 2u; i++) {
        TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sn[i]));
        TEST_ASSERT_NOT_EQUAL(0u, slot_c(sn[i])->scratch);
        TEST_ASSERT_TRUE_MESSAGE(slot_c(sn[i])->parked, "its question does not wait");
    }
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, la)); /* not busy */
    TEST_ASSERT_NOT_EQUAL(0u, slot_c(la)->scratch);
    size_t gave = 2u;
    for (size_t i = 0; i < 2u; i++) {
        if (slot_c(sn[i])->scratch == 0u) {
            gave = i;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(gave < 2u, "no sleeper gave its scratch record back");
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sn[gave]));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_PROTOCOL, zw_iv_step(iv, sn[gave]));
    TEST_ASSERT_FALSE(zw_iv_asking(iv, sn[gave]));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), sn[gave]));
    expect_ready(la, 30000u);

    for (size_t i = 0; i < 2u; i++) {
        sl[i]->m.awake_ms = 30000u;
        sl[i]->m.awake_extend_ms = 2000u;
        h_clear_events();
        fake_zdev_wake(sl[i], H.wire.now);
        expect_ready(sn[i], 60000u);
        TEST_ASSERT_TRUE(fake_zdev_asked(sl[i], 0x72, 0x04) > identity[i]);
    }
    TEST_ASSERT_TRUE(no_scratch_in_use());
}

/* ZW-029 (BL-152): a re-read under way starts again from step 1 at a second request, as Z-Wave
 * JS's refreshInfo restarts a running interview; it keeps its scratch record and ends READY. */
static void test_reread_second_request_restarts(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7E5Cu, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    zw_iv_t *iv = &H.net.iv;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    run_until_step(node, ZW_IV_STEP_VERSIONS, 30000u);
    const uint8_t k = slot_c(node)->scratch;
    const uint32_t started = iv->stats.started;
    const unsigned identity = fake_zdev_asked(&dev, 0x72, 0x04);
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_PROTOCOL, zw_iv_step(iv, node));
    TEST_ASSERT_EQUAL_UINT32(started + 1u, iv->stats.started);
    TEST_ASSERT_EQUAL_UINT8(k, slot_c(node)->scratch);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    expect_ready(node, 30000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&dev, 0x72, 0x04) > identity);
    TEST_ASSERT_TRUE(no_scratch_in_use());
}

/* ZW-009, ZW-029 (BL-152, Z-Wave JS refreshInfo + updateNodeInfo): a re-read's node information
 * reaches the security layer at READY, replacing the old reading's - a CC the S2 device now lists
 * only in its NIF goes at its highest class, one it no longer lists goes in the clear - and its
 * classes stay. Nothing changes before READY. */
static void test_reread_nif_reaches_security(void)
{
    h_device(&dev, 0x7E5Du, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    const zw_s2_t *s2 = zw_net_s2(&H.net);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x5Eu)); /* NIF only */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x25u));
    size_t w = 0u;
    for (size_t i = 0; i < dev.nif_len; i++) {
        if (dev.nif[i] != 0x5Eu) {
            dev.nif[w++] = dev.nif[i];
        }
    }
    dev.nif[w++] = 0x25u;
    dev.nif_len = w;
    dev.m.versions[dev.m.versions_n++] = (fake_cc_version_t){0x25u, 1};
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    run_until_step(node, ZW_IV_STEP_IDENTITY, 30000u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x25u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x5Eu));
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x25u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x5Eu));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u)); /* secure list */
}

/* ZW-010, ZW-009 (Security2CC.ts interview: only the interview's first non-empty Commands
 * Supported Report is the secure list; the report stores nothing outside it): an unasked report at
 * the node's highest class after READY - empty, then one naming another CC - leaves the secure
 * list as the interview set it. */
static void test_unasked_secure_report_changes_nothing(void)
{
    h_device(&dev, 0x7E70u, false);
    fake_zdev_listening(&dev);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    const zw_s2_t *s2 = zw_net_s2(&H.net);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u)); /* secure list */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x25u));
    static const uint8_t EMPTY[] = {0x9F, 0x0E};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_S2_ACCESS, EMPTY,
                                                     sizeof EMPTY));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u));
    static const uint8_t OTHER[] = {0x9F, 0x0E, 0x25};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_S2_ACCESS, OTHER,
                                                     sizeof OTHER));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x25u));
}

/* ZW-009, ZW-029 (BL-152, Z-Wave JS refreshInfo forgets the end point CC data): a re-read whose
 * step 4 is never answered leaves the record without its secure CCs, and from READY on the
 * security layer sends as the record says - the same class as after the next start. */
static void test_reread_unanswered_secure_list_reaches_security(void)
{
    h_device(&dev, 0x7E71u, false);
    fake_zdev_listening(&dev);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    const zw_s2_t *s2 = zw_net_s2(&H.net);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u)); /* secure list */
    dev.m.mute_cc = 0x9Fu;
    dev.m.mute_cmd = 0x0Du;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_UINT(asked + 3u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    const zw_node_cc_t *c = cc_of(h_rec(node), 0x71u);
    TEST_ASSERT_TRUE_MESSAGE(c == NULL || !c->secure, "the record kept its secure CC");
    const zw_sec_class_t after_ready = zw_s2_class_for(s2, node, 0x71u);
    TEST_ASSERT_EQUAL_INT(c != NULL && c->supported ? ZW_SEC_S2_ACCESS : ZW_SEC_NONE, after_ready);
    h_boot();
    TEST_ASSERT_EQUAL_INT(after_ready, zw_s2_class_for(zw_net_s2(&H.net), node, 0x71u));
}

/* ZW-029 (BL-152; network.md: a parked record is given back "when another device needs one"): two
 * sleepers whose re-reads fell asleep mid-reading hold both scratch records; a third sleeper's
 * re-read takes one back at its wake-up and is read then, not after every wake-up. The parked ones
 * read from step 1 at their next wake-up. */
static void test_reread_waking_sleeper_reclaims_parked(void)
{
    static fake_zdev_t s1, s2d, s3;
    fake_zdev_t *sl[3] = {&s1, &s2d, &s3};
    uint16_t sn[3];
    unsigned identity[3];
    for (size_t i = 0; i < 3u; i++) {
        sn[i] = sleeper_ready(sl[i], 0x7E72u + i);
        identity[i] = fake_zdev_asked(sl[i], 0x72, 0x04);
    }
    zw_iv_t *iv = &H.net.iv;
    for (size_t i = 0; i < 2u; i++) {
        sl[i]->m.awake_ms = 300u; /* it falls asleep mid-reading */
        sl[i]->m.awake_extend_ms = 0u;
        TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sn[i]));
        fake_zdev_wake(sl[i], H.wire.now);
    }
    h_run_for(5000u);
    for (size_t i = 0; i < 2u; i++) {
        TEST_ASSERT_NOT_EQUAL(0u, slot_c(sn[i])->scratch);
        TEST_ASSERT_TRUE_MESSAGE(slot_c(sn[i])->parked, "its question does not wait");
    }
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sn[2])); /* waits for its wake-up */
    TEST_ASSERT_EQUAL_UINT8(0u, slot_c(sn[2])->scratch);
    h_clear_events();
    fake_zdev_wake(sl[2], H.wire.now);
    h_run_for(100u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, slot_c(sn[2])->scratch, "it waits for another wake-up");
    size_t gave = 2u;
    for (size_t i = 0; i < 2u; i++) {
        if (slot_c(sn[i])->scratch == 0u) {
            gave = i;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(gave < 2u, "no parked sleeper gave its scratch record back");
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_PROTOCOL, zw_iv_step(iv, sn[gave]));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), sn[gave]));
    expect_ready(sn[2], 60000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(sl[2], 0x72, 0x04) > identity[2]);

    for (size_t i = 0; i < 2u; i++) {
        sl[i]->m.awake_ms = 30000u;
        sl[i]->m.awake_extend_ms = 2000u;
        h_clear_events();
        fake_zdev_wake(sl[i], H.wire.now);
        expect_ready(sn[i], 60000u);
        TEST_ASSERT_TRUE(fake_zdev_asked(sl[i], 0x72, 0x04) > identity[i]);
    }
    TEST_ASSERT_TRUE(no_scratch_in_use());
}

/* ZW-011, ZW-029 (BL-152): Z-Wave JS waits for the wake-up only of a sleeper that supports Wake
 * Up; one that does not is read at once (its question then waits in its queue). Its re-read's
 * step 1 does not make the panel think it awake, though it joined last. */
static void test_reread_sleeper_without_wake_up(void)
{
    static fake_zdev_t sl;
    const uint16_t sleeper = sleeper_ready(&sl, 0x7E5Eu);
    TEST_ASSERT_EQUAL_UINT16(sleeper, H.net.joined);
    zw_node_record_t *rec = zw_node_find(&H.net.nodes, sleeper);
    for (size_t i = 0; i < rec->cc_count; i++) {
        if (rec->ccs[i].cc == ZW_CC_WAKE_UP) {
            rec->ccs[i].supported = false;
            rec->ccs[i].secure = false;
        }
    }
    TEST_ASSERT_FALSE(zw_iv_has_cc(rec, ZW_CC_WAKE_UP));
    size_t from = H.zac.heard;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sleeper));
    TEST_ASSERT_NOT_EQUAL(0u, slot_c(sleeper)->scratch); /* started now */
    h_run_for(3000u);
    TEST_ASSERT_TRUE(zw_iv_refreshing(&H.net.iv, sleeper));
    TEST_ASSERT_FALSE(health_awake(sleeper));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, frames_to(sleeper, &from), "sent to a sleeping device");
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, sleeper));
    fake_zdev_wake(&sl, H.wire.now);
    expect_ready(sleeper, 30000u);
}

/* ZW-011, ZW-012, ZW-029 (BL-152): a Wake Up Interval Report heard during a sleeper's re-read,
 * after its step 7, is what READY keeps - in its record and in the supervision - not the reading's
 * older value. */
static void test_reread_keeps_wake_interval_report(void)
{
    static fake_zdev_t sl;
    const uint16_t sleeper = sleeper_ready(&sl, 0x7E5Fu);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sleeper));
    fake_zdev_wake(&sl, H.wire.now);
    run_until_step(sleeper, ZW_IV_STEP_ENDPOINTS, 30000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, sleeper));
    const uint32_t chosen = h_rec(sleeper)->wake_up_interval_seconds;
    const uint32_t heard = chosen + 3600u;
    const uint8_t REPORT[] = {0x84, 0x06, (uint8_t)(heard >> 16), (uint8_t)(heard >> 8),
                              (uint8_t)heard, 0x01};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&sl, H.wire.now, ZW_SEC_AUTO, REPORT,
                                                     sizeof REPORT));
    expect_ready(sleeper, 30000u);
    TEST_ASSERT_EQUAL_UINT32(heard, h_rec(sleeper)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(heard, zw_health_interval(zw_net_health(&H.net), sleeper));
}

/* ZW-011, ZW-029 (BL-152): a sleeper's re-read whose attempt fails gives its scratch record back
 * and waits for the next wake-up from step 1 - No More Information goes meanwhile, and two
 * listening devices' re-reads can both start. At its next wake-up it is read again from step 1
 * and ends READY. */
static void test_reread_sleeper_attempt_fails(void)
{
    static fake_zdev_t a, b, sl;
    const uint16_t la = listening_ready(&a, 0x7E60u);
    const uint16_t lb = listening_ready(&b, 0x7E61u);
    const uint16_t sleeper = sleeper_ready(&sl, 0x7E62u);
    zw_iv_t *iv = &H.net.iv;
    sl.m.awake_ms = 300000u;
    sl.m.mute_cc = 0x72u;
    sl.m.mute_n = (uint8_t)ZW_IV_TRIES;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, sleeper));
    fake_zdev_wake(&sl, H.wire.now);
    run_until_step(sleeper, ZW_IV_STEP_IDENTITY, 30000u);
    const uint32_t nmi = sl.nmi_heard;
    for (unsigned k = 0; k < 600u && slot_c(sleeper)->scratch != 0u; k++) {
        h_run_for(100u);
    }
    TEST_ASSERT_TRUE(zw_iv_refreshing(iv, sleeper));
    TEST_ASSERT_EQUAL_UINT8(0u, slot_c(sleeper)->scratch);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_PROTOCOL, zw_iv_step(iv, sleeper));
    TEST_ASSERT_FALSE(zw_iv_asking(iv, sleeper));
    h_run_for(ZW_HEALTH_NO_MORE_INFO_MS + 1000u);
    TEST_ASSERT_TRUE_MESSAGE(sl.nmi_heard > nmi, "No More Information held");
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, la));
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, lb));
    h_clear_events();
    expect_ready(la, 30000u);
    expect_ready(lb, 30000u);
    const unsigned identity = fake_zdev_asked(&sl, 0x72, 0x04);
    h_clear_events();
    fake_zdev_wake(&sl, H.wire.now);
    expect_ready(sleeper, 30000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&sl, 0x72, 0x04) > identity);
}

/* ZW-029, BL-130 (BL-152): a re-read whose Alarm Sensor Supported Get never reaches the device
 * keeps the old types (the zone keeps using them); one the device heard and left unanswered
 * stores "none" - the old types are gone. */
static void test_reread_keeps_alarm_types(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x9C};
    plain_device(&dev, 0x7E63u, true, CCS, sizeof CCS);
    dev.m.alarm_types[0] = ZW_ALARM_SENSOR_TYPE_SMOKE;
    dev.m.alarm_n = 1u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_HEX8(0x02u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);

    dev.deaf_cc = 0x9Cu; /* undelivered */
    const unsigned asked = fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_UINT(asked, fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET));
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(1u, h_rec(node)->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_HEX8(0x02u, h_rec(node)->endpoints[0].alarm_sensor_mask[0]);
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(h_rec(node)));

    dev.deaf_cc = 0u;
    dev.m.mute_cc = 0x9Cu; /* delivered, never answered */
    dev.m.mute_cmd = ZW_CMD_ALARM_SENSOR_SUPPORTED_GET;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    expect_ready(node, 60000u);
    TEST_ASSERT_TRUE(fake_zdev_asked(&dev, 0x9C, ZW_CMD_ALARM_SENSOR_SUPPORTED_GET) > asked);
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, h_rec(node)->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_FALSE(zw_iv_alarm_types_missing(h_rec(node)));
}

/* ZW-002, network.md §10: a controller in the network feeds no zone: its interview stops at its
 * protocol info ("another controller: remove it"). */
static void test_controller_not_usable(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72};
    plain_device(&dev, 0x68u, true, CCS, sizeof CCS);
    dev.basic = 0x02u; /* static controller */
    dev.controller = true;
    const uint16_t node = found_at_start(&dev);
    h_run_for(5000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NOT_USABLE, node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NOT_USABLE, zw_iv_inputs(h_rec(node), 0u));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x72, 0x04));
}

/* ---- The node cache at every start (network.md §2; ZW-017) ----------------------------------- */

/* ZW-017: the first start binds fresh keys (the panel's RNG) to the module's home ID and has them
 * stored, once; a restart finds them. */
static void test_first_start_binds_keys(void)
{
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->first_start);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, e->radio);
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
    TEST_ASSERT_TRUE(H.cfg.keys_bound);
    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE01u, H.cfg.keys_home_id);
    static const uint8_t ZERO[16] = {0};
    TEST_ASSERT_FALSE(memcmp(H.cfg.keys.s2_access, ZERO, 16) == 0);
    TEST_ASSERT_FALSE(memcmp(H.cfg.keys.s2_access, H.cfg.keys.s2_auth, 16) == 0);
    h_clear_events();
    h_boot();
    e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_FALSE(e->first_start);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, e->radio);
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
}

/* ---- ZW-017, ZW01: a first start whose keys or security manager fail --------------------------- */

/* The panel's crypto port with failures on demand: the six keys' draw (sizeof zw_s2_keys_t), the
 * security manager's DRBG seed (32 bytes), and the network key expansion (the first AES call with
 * a key other than the DRBG's initial zero key after a seed was drawn). */
static struct {
    zw_crypto_port_t real;
    unsigned key_fails, seed_fails, expand_fails;
    unsigned key_draws;
    bool seeded;
} FK;

static bool fk_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (len == sizeof(zw_s2_keys_t)) {
        FK.key_draws++;
        if (FK.key_fails > 0u) {
            FK.key_fails--;
            memset(buf, 0x5A, len / 2u); /* a provider may fail half-way */
            return false;
        }
    }
    if (len == 32u && FK.seed_fails > 0u) {
        FK.seed_fails--;
        return false;
    }
    if (len == 32u) {
        FK.seeded = true;
    }
    return FK.real.random(FK.real.ctx, buf, len);
}

static bool fk_aes(void *ctx, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    (void)ctx;
    static const uint8_t ZERO[16] = {0};
    if (FK.expand_fails > 0u && FK.seeded && memcmp(key, ZERO, 16) != 0) {
        FK.expand_fails--;
        FK.seeded = false;
        return false;
    }
    return FK.real.aes128_encrypt(FK.real.ctx, key, in, out);
}

static bool fk_x25519(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    (void)ctx;
    return FK.real.x25519(FK.real.ctx, out, scalar, point);
}

/* A panel with nothing stored starts on the module h_init left: a first start. */
static void fresh_install(unsigned key_fails, unsigned seed_fails, unsigned expand_fails)
{
    memset(&FK, 0, sizeof FK);
    FK.real = zw_host_crypto_port(&H.hc);
    FK.key_fails = key_fails;
    FK.seed_fails = seed_fails;
    FK.expand_fails = expand_fails;
    H.crypto.ctx = NULL;
    H.crypto.aes128_encrypt = fk_aes;
    H.crypto.x25519 = fk_x25519;
    H.crypto.random = fk_random;
    memset(&H.cfg.keys, 0, sizeof H.cfg.keys);
    H.cfg.keys_bound = false;
    H.cfg.keys_home_id = 0u;
    H.key_stores = 0u;
    h_clear_events();
    h_boot();
}

static bool all_zero(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* The security manager's six active keys, the panel's set and the owner's stored set are one set
 * of real keys (booleans only: no key is printed). */
static bool keys_agree(void)
{
    const zw_s2_keys_t *k = &H.cfg.keys;
    const zw_s2_t *s2 = &H.net.s2;
    const uint8_t *active[6] = {zw_s2_network_key(s2, 2u, ZW_S2_KEY_UNAUTH),
                                zw_s2_network_key(s2, 2u, ZW_S2_KEY_AUTH),
                                zw_s2_network_key(s2, 2u, ZW_S2_KEY_ACCESS),
                                zw_s2_network_key(s2, 2u, ZW_S2_KEY_S0),
                                zw_s2_network_key(s2, 256u, ZW_S2_KEY_AUTH),
                                zw_s2_network_key(s2, 256u, ZW_S2_KEY_ACCESS)};
    const uint8_t *stored[6] = {k->s2_unauth, k->s2_auth, k->s2_access, k->s0, k->lr_auth,
                                k->lr_access};
    bool ok = s2->started && H.cfg.keys_bound && H.net.keys_bound && H.net.keys_ready &&
              memcmp(&H.net.keys, k, sizeof *k) == 0 && !all_zero((const uint8_t *)k, sizeof *k);
    for (size_t i = 0; i < 6u; i++) {
        ok = ok && active[i] != NULL && memcmp(active[i], stored[i], 16) == 0 &&
             !all_zero(stored[i], 16);
    }
    return ok;
}

/* The start stopped: nothing secure runs, nothing bound or stored, the network not usable. */
static void expect_start_stopped(void)
{
    TEST_ASSERT_FALSE(H.net.s2.started);
    TEST_ASSERT_EQUAL_UINT(0u, H.key_stores);
    TEST_ASSERT_FALSE(H.cfg.keys_bound);
    TEST_ASSERT_FALSE(H.net.keys_bound);
    TEST_ASSERT_FALSE(H.net.checked);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_UNKNOWN, zw_net_radio(&H.net));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NETWORK, 0u));
    TEST_ASSERT_TRUE(zw_net_stats(&H.net)->s2_start_failures > 0u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BUSY, zw_net_add_device(&H.net, H.wire.now, true, 2u, 0u));
}

/* The retry after ZW_NET_KEY_RETRY_MS starts the network on one set; a restart keeps it. */
static void expect_recovered_and_kept(void)
{
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->first_start);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, e->radio);
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
    TEST_ASSERT_TRUE(keys_agree());
    zw_s2_keys_t before = H.cfg.keys;
    h_clear_events();
    h_boot(); /* the panel restarts with what it stored */
    e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->first_start);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, e->radio);
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
    TEST_ASSERT_TRUE(memcmp(&before, &H.cfg.keys, sizeof before) == 0);
    TEST_ASSERT_TRUE(keys_agree());
    memset(&before, 0, sizeof before);
}

/* ZW-017, ZW01: control - a first start with a working port binds one set: active = stored. */
static void test_first_start_keys_agree(void)
{
    fresh_install(0u, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT(1u, FK.key_draws);
    expect_recovered_and_kept();
}

/* ZW-017, ZW01: the link up's draw of the six keys fails, the check's draw works: the security
 * manager starts from exactly the set that is bound and stored (it once ran on the unset one). */
static void test_first_start_key_draw_fails_once(void)
{
    fresh_install(1u, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT(2u, FK.key_draws);
    expect_recovered_and_kept();
}

/* ZW-017, ZW01: both draws fail: the start stops - no unset keys active, bound or stored - and is
 * tried again ZW_NET_KEY_RETRY_MS later with keys drawn afresh. */
static void test_first_start_key_draws_fail(void)
{
    fresh_install(2u, 0u, 0u);
    expect_start_stopped();
    TEST_ASSERT_FALSE(H.net.keys_ready);
    TEST_ASSERT_TRUE(all_zero((const uint8_t *)&H.net.keys, sizeof H.net.keys));
    h_run_for(ZW_NET_KEY_RETRY_MS - 1000u);
    expect_start_stopped(); /* not before its time */
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(3u, FK.key_draws);
    expect_recovered_and_kept();
}

/* ZW-017, ZW01: the keys are drawn but the security manager's DRBG seed fails at the link up and at
 * the check: nothing is bound or stored until it starts; the retry starts it and binds that set. */
static void test_first_start_seed_fails(void)
{
    fresh_install(0u, 2u, 0u);
    expect_start_stopped();
    h_run_for(ZW_NET_KEY_RETRY_MS + 2000u);
    expect_recovered_and_kept();
}

/* ZW-017, ZW01: the network keys' expansion fails at the link up and at the check: the same. */
static void test_first_start_key_expansion_fails(void)
{
    fresh_install(0u, 0u, 2u);
    expect_start_stopped();
    h_run_for(ZW_NET_KEY_RETRY_MS + 2000u);
    expect_recovered_and_kept();
}

/* The security manager holds no network: not started, no home or node ID, none of the six keys. */
static bool s2_holds_nothing(void)
{
    const zw_s2_t *s2 = &H.net.s2;
    const uint8_t bits[4] = {ZW_S2_KEY_UNAUTH, ZW_S2_KEY_AUTH, ZW_S2_KEY_ACCESS, ZW_S2_KEY_S0};
    bool none = !s2->started && s2->home_id == 0u && s2->own_node == 0u;
    for (size_t i = 0; i < 4u; i++) {
        none = none && zw_s2_network_key(s2, 2u, bits[i]) == NULL &&
               zw_s2_network_key(s2, 256u, bits[i]) == NULL;
    }
    return none;
}

/* ZW-018, ZW-017, ZW01 (repair 1): the network reset draws a new set while the security manager
 * still runs the old one; the HardReset is never answered, so the module comes back on the same
 * home and node IDs (the link up keeps the manager as it is). Its check is a first start: the set
 * it binds and stores is the one the manager runs on - never the old one active behind a new one
 * stored. */
static void test_failed_reset_binds_active_keys(void)
{
    fresh_install(0u, 0u, 0u);
    TEST_ASSERT_TRUE(keys_agree());
    const zw_s2_keys_t old = H.cfg.keys;
    const uint32_t home = H.cfg.keys_home_id;
    H.zac.fault.mute_func = ZW_FUNC_HARD_RESET;
    H.zac.fault.mute_count = 1u;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_reset_network(&H.net, H.wire.now, true, NULL));
    h_run_for(20000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_RESET, 0u));
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->first_start);
    TEST_ASSERT_EQUAL_HEX32(home, H.zac.id.home_id); /* the module was never reset */
    TEST_ASSERT_EQUAL_HEX32(home, H.cfg.keys_home_id);
    TEST_ASSERT_EQUAL_UINT(2u, H.key_stores);
    TEST_ASSERT_FALSE(memcmp(&old, &H.cfg.keys, sizeof old) == 0); /* a new set... */
    TEST_ASSERT_TRUE(keys_agree());                                /* ...and the active one */
}

/* ZW-017, ZW01 (repair 2): a working network is reset; the new set cannot be drawn - at the reset,
 * at the new home ID's link up, at its check. The stopped start leaves the security manager on
 * nothing: not the old network's keys and home ID. The retry starts it on the set it binds. */
static void test_stopped_first_start_stops_s2(void)
{
    fresh_install(0u, 0u, 0u);
    const uint32_t old_home = H.cfg.keys_home_id;
    FK.key_fails = 3u;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_reset_network(&H.net, H.wire.now, true, NULL));
    h_clear_events();
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, FK.key_fails);
    TEST_ASSERT_NOT_EQUAL(old_home, H.zac.id.home_id);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NETWORK, 0u));
    TEST_ASSERT_FALSE(H.net.keys_ready);
    TEST_ASSERT_FALSE(H.net.keys_bound);
    TEST_ASSERT_TRUE(all_zero((const uint8_t *)&H.net.keys, sizeof H.net.keys));
    TEST_ASSERT_TRUE(s2_holds_nothing());
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
    h_run_for(ZW_NET_KEY_RETRY_MS + 2000u);
    TEST_ASSERT_EQUAL_UINT(2u, H.key_stores);
    TEST_ASSERT_EQUAL_HEX32(H.zac.id.home_id, H.cfg.keys_home_id);
    TEST_ASSERT_EQUAL_HEX32(H.zac.id.home_id, H.net.s2.home_id);
    TEST_ASSERT_TRUE(keys_agree());
}

/* ZW-017, §2 "normal": a node the module lists that the cache lacks is added and interviewed; one
 * the cache has that the module no longer lists is forgotten - its zones show CHECK "sensor
 * removed", the event names its generation; a device that gets its node ID later has another
 * generation (zones.md §1), even when the owner's stored counter fell behind its records. All
 * stored. */
static void test_reconcile_normal(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x71u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT16(1u, h_rec(node)->generation);
    zw_node_list_set(H.zac.id.node_list, node, false); /* the module no longer has it */
    dev.status = FAKE_ZDEV_OUT;
    H.cfg.generation = 0u; /* the counter was not stored with the records */
    h_clear_events();
    h_boot();
    const zw_net_event_t *g = h_find(ZW_NET_EV_NODE_GONE, node);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_INT(ZW_NET_GONE_MISSING, g->gone);
    TEST_ASSERT_EQUAL_UINT16(1u, g->generation);
    TEST_ASSERT_NULL(h_rec(node));
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    TEST_ASSERT_EQUAL_UINT(0u, zw_node_count(&back));
    static fake_zdev_t other;
    plain_device(&other, 0x72u, true, CCS, sizeof CCS);
    h_clear_events();
    TEST_ASSERT_EQUAL_UINT16(node, found_at_start(&other));
    TEST_ASSERT_EQUAL_UINT16(2u, h_rec(node)->generation);
}

/* ZW-014 (ZW32; zones.md §1): the inclusion counter never wraps. At 65534 a device found at
 * start gets 65535 and the counter is used up; then a node the cache lacks gets no record and no
 * NODE_ADDED - never generation 1, which a removed device's zones may still hold - also when the
 * stored counter was lost (raised again from the records), and the device it has keeps its own. */
static void test_generation_counter_never_wraps(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    H.cfg.generation = 65534u;
    plain_device(&dev, 0x73u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    TEST_ASSERT_EQUAL_UINT16(65535u, h_rec(node)->generation);
    TEST_ASSERT_EQUAL_UINT16(65535u, H.cfg.generation);
    TEST_ASSERT_FALSE(zw_net_generations_left(&H.net));
    static fake_zdev_t other;
    plain_device(&other, 0x74u, true, CCS, sizeof CCS);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &other));
    fake_zac93_join(&H.zac, &other, 6u);
    for (unsigned lost = 0u; lost < 2u; lost++) {
        if (lost == 1u) {
            H.cfg.generation = 0u; /* the counter was not stored with the records */
        }
        h_clear_events();
        h_boot();
        TEST_ASSERT_NULL(h_rec(6u));
        TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_ADDED, 6u));
        TEST_ASSERT_EQUAL_UINT16(65535u, h_rec(node)->generation);
        TEST_ASSERT_FALSE(zw_net_generations_left(&H.net));
    }
}

/* ZW-017, §2: the module was replaced (or reset): no nodes, another home ID - "add its N devices
 * again"; the cache is kept until the installer adds a device, then fresh keys - never the old
 * network's - are bound to the new home ID and the old cache goes. */
static void test_reconcile_replaced(void)
{
    h_device(&dev, 0x5E44u, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    const zw_s2_keys_t old = H.cfg.keys;
    fake_zac93_init(&H.zac);
    H.zac.id.home_id = 0xC0FFEE02u;
    h_clear_events();
    h_boot();
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_REPLACED, e->radio);
    TEST_ASSERT_EQUAL_UINT16(1u, e->count);
    TEST_ASSERT_NOT_NULL(h_rec(node)); /* kept: a mistaken swap can be undone */
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
    static fake_zdev_t next;
    h_device(&next, 0x5E48u, false);
    const uint16_t n2 = h_include(&next);
    TEST_ASSERT_NOT_EQUAL(0u, n2);
    TEST_ASSERT_EQUAL_UINT(2u, H.key_stores);
    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE02u, H.cfg.keys_home_id);
    TEST_ASSERT_FALSE(memcmp(&old, &H.cfg.keys, sizeof old) == 0);
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(zw_net_nodes(&H.net)));
    TEST_ASSERT_EQUAL_UINT16(2u, h_rec(n2)->generation); /* the counter goes on */
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, zw_net_radio(&H.net));
    expect_ready(n2, 30000u); /* and it is interviewed on the new network */
}

/* ZW-017, §2: a module of another system - nodes under another home ID, or no keys stored at all:
 * none of its nodes is used and adding is refused until it is reset; nothing is stored. */
static void test_reconcile_foreign(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    fake_zac93_init(&H.zac);
    H.zac.id.home_id = 0x0BADF00Du;
    plain_device(&dev, 0x73u, true, CCS, sizeof CCS);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &dev));
    fake_zac93_join(&H.zac, &dev, 7u);
    h_clear_events();
    h_boot();
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_FOREIGN, h_find(ZW_NET_EV_NETWORK, 0u)->radio);
    TEST_ASSERT_EQUAL_UINT(0u, zw_node_count(zw_net_nodes(&H.net)));
    TEST_ASSERT_FALSE(zw_s2_has_node(zw_net_s2(&H.net), 7u));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_FOREIGN_RADIO, zw_net_add_device(&H.net, H.wire.now, true, 0, 0));
    static const uint8_t BASIC_SET[] = {0x20, 0x01, 0xFF};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_NONE, BASIC_SET,
                                                     sizeof BASIC_SET));
    h_run_for(100u);
    TEST_ASSERT_EQUAL_UINT(0u, h_rx_count(7u, 0x20, 0x01));
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
    H.cfg.keys_bound = false; /* keys lost: a module with nodes is still not ours */
    h_clear_events();
    h_boot();
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_FOREIGN, h_find(ZW_NET_EV_NETWORK, 0u)->radio);
    TEST_ASSERT_EQUAL_UINT(1u, H.key_stores);
}

/* ZW-017, ZW-010 with §5 step 4: the stored cache is lost; the module still lists a secure device - it is
 * interviewed again (a new generation) and its classes probed: it answers at Access Control, its
 * highest; its reports count at that class again. */
static void test_cache_lost_classes_probed(void)
{
    h_device(&dev, 0x5E46u, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    H.blob_len = 0u;
    h_clear_events();
    h_boot();
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_ADDED, node));
    TEST_ASSERT_EQUAL_UINT16(2u, h_rec(node)->generation);
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    fake_zdev_wake(&dev, H.wire.now); /* asleep, as far as the panel knows: its button */
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, h_rx_count(node, 0x71, 0x05) >= 1u ? 1u : 0u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, H.rx[H.rx_n - 1u].security);
}

static size_t csg_classes(zw_sec_class_t *out, size_t cap);

/* The device's own decryption failures: a frame the panel sent it at a class it does not hold. */
static uint32_t dev_decrypt_failures(void)
{
    const zw_s2_node_stats_t *st = zw_s2_node_stats(&dev.s2, dev.s2.peers[0].node);
    TEST_ASSERT_NOT_NULL(st);
    return st->decrypt_failures;
}

/* ZW-009, ZW-010, ZW-017 (Z-Wave JS Security2CC.ts decryptSinglecast learns an unknown node's class by
 * decrypting it; its interview then asks only that class): the cache lost, the secure device
 * reports before the interview's probe - on the SPAN the panel forgot. The panel answers with a
 * Nonce Report and decrypts the frame sent again with the key that fits (Access Control):
 * delivered. A report is no wake-up (Z-Wave JS Node.ts): its button wakes it, and step 4 asks only
 * the learnt class - recorded, nothing probed at a class it lacks, the classes not learnt still
 * undecided in the security manager. */
static void test_cache_lost_s2_decrypted_before_probe(void)
{
    h_device(&dev, 0x5E4Au, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    H.blob_len = 0u;
    h_clear_events();
    h_boot();
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    const size_t before = h_rx_count(node, 0x71, 0x05);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_TRUE(zw_s2_node_stats(zw_net_s2(&H.net), node)->nonce_reports >= 1u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, H.rx[H.rx_n - 1u].security);
    const uint32_t failures = dev_decrypt_failures();
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    dev.got_n = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    TEST_ASSERT_EQUAL_UINT32(failures, dev_decrypt_failures());
    zw_sec_class_t asked_at[4];
    TEST_ASSERT_EQUAL_UINT(asked + 1u, fake_zdev_asked(&dev, 0x9F, 0x0D)); /* exactly one */
    TEST_ASSERT_EQUAL_UINT(1u, csg_classes(asked_at, 4u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, asked_at[0]);
    TEST_ASSERT_TRUE(zw_s2_classes_unknown(zw_net_s2(&H.net), node));
}

/* ZW-009, ZW-010 (Z-Wave JS Security2CC.ts interview: "The highest security class is known to be
 * S2, only query that one"): a cache-lost device granted only S2 Unauthenticated whose report was
 * learnt by decrypting gets exactly one Commands Supported Get, at Unauthenticated - none at
 * Access Control or Authenticated (which it could not decrypt) - and is recorded Unauthenticated. */
static void test_cache_lost_learnt_class_asked_alone(void)
{
    H.grant = ZW_S2_KEY_UNAUTH;
    h_device(&dev, 0x5E4Cu, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    h_run_for(1000u);
    H.blob_len = 0u;
    h_clear_events();
    h_boot();
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    const size_t before = h_rx_count(node, 0x71, 0x05);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, H.rx[H.rx_n - 1u].security);
    const uint32_t failures = dev_decrypt_failures();
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    dev.got_n = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_UINT32(failures, dev_decrypt_failures()); /* no probe at another class */
    zw_sec_class_t asked_at[4];
    TEST_ASSERT_EQUAL_UINT(asked + 1u, fake_zdev_asked(&dev, 0x9F, 0x0D)); /* exactly one */
    TEST_ASSERT_EQUAL_UINT(1u, csg_classes(asked_at, 4u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, asked_at[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, zw_s2_highest(zw_net_s2(&H.net), node));
}

/* ZW-009, ZW-010 (Z-Wave JS Security2CC.ts interview: each class queried at an explicit class,
 * Unauthenticated, Authenticated, Access Control, while decryption keeps trying every class not
 * refused): a cache-lost device granted only Authenticated, with nothing learnt, is probed; a report
 * it sends while the probe asks another class is delivered at Authenticated; the probe records
 * Authenticated only. */
static void test_cache_lost_probe_keeps_real_class(void)
{
    H.grant = ZW_S2_KEY_AUTH;
    h_device(&dev, 0x5E4Du, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_AUTH, h_rec(node)->security_classes);
    h_run_for(1000u);
    H.blob_len = 0u;
    h_clear_events();
    h_boot();
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    fake_zdev_wake(&dev, H.wire.now);
    /* its Wake Up Notification came encrypted and taught its class: forgotten here, so that
     * nothing is learnt before step 4 and the whole probe runs (as for a device whose frames
     * before it came in the clear) */
    zw_s2_t *ps2 = (zw_s2_t *)zw_net_s2(&H.net);
    for (unsigned i = 0; i < 2000u && zw_s2_granted(ps2, node) == 0u; i++) {
        h_run_for(1u);
    }
    TEST_ASSERT_TRUE(zw_iv_step(&H.net.iv, node) < ZW_IV_STEP_SECURE_CCS);
    TEST_ASSERT_TRUE(zw_s2_set_granted(ps2, node, 0u));
    TEST_ASSERT_TRUE(zw_s2_set_classes_unknown(ps2, node, true));
    for (unsigned i = 0; i < 6000u && zw_iv_step(&H.net.iv, node) != ZW_IV_STEP_SECURE_CCS; i++) {
        h_run_for(5u);
    }
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_SECURE_CCS, zw_iv_step(&H.net.iv, node));
    h_run_for(300u); /* its first question out, at Unauthenticated, which it lacks */
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_SECURE_CCS, zw_iv_step(&H.net.iv, node));
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    const size_t before = h_rx_count(node, 0x71, 0x05);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_S2_AUTH, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, H.rx[H.rx_n - 1u].security);
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_AUTH, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_highest(zw_net_s2(&H.net), node));
    TEST_ASSERT_FALSE(zw_s2_classes_unknown(zw_net_s2(&H.net), node)); /* all decided */
}

/* ZW-009 (Z-Wave JS Node.ts updateNodeInfo: a NIF's CCs are added as supported only while
 * interviewStage < NodeInfo, never removed; after that a NIF only marks the node awake): after the
 * interview, a NIF from an S2 device listing fewer CCs (unauthenticated, perhaps forged) removes
 * nothing, and one listing a new CC adds nothing either - that CC still goes in the clear. Before
 * a first interview's node-information step (field 0E set back by hand here, as an interview
 * resumed from an older record has it) a NIF's CCs are added. A re-read keeps field 0E done: its
 * NIF reaches the security layer at its READY (test_reread_nif_reaches_security). */
static uint16_t held_sleeper(uint64_t seed, bool keyless);
static unsigned send_datas_since(size_t from);

static void test_nif_never_removes_encrypted_ccs(void)
{
    const uint16_t node = held_sleeper(0x5E4Bu, false); /* asleep, a Battery Get held */
    const zw_s2_t *s2 = zw_net_s2(&H.net);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x5Eu)); /* NIF only */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x25u));
    static const uint8_t FEWER[] = {0x9F};
    memcpy(dev.nif, FEWER, sizeof FEWER);
    dev.nif_len = sizeof FEWER;
    const size_t heard = H.zac.heard;
    dev.awake_until = H.wire.now + 10000u; /* its button: awake, and its NIF sent */
    fake_zac93_node_info(&H.zac, H.wire.now, &dev);
    h_run_for(1000u);
    /* ZW-011, network.md §7 (Node.ts updateNodeInfo marks it awake): the held command goes */
    TEST_ASSERT_TRUE(send_datas_since(heard) >= 1u);
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n); /* the Battery Get went */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x5Eu));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x71u));
    static const uint8_t MORE[] = {0x9F, 0x25};
    memcpy(dev.nif, MORE, sizeof MORE);
    dev.nif_len = sizeof MORE;
    fake_zac93_node_info(&H.zac, H.wire.now, &dev);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(s2, node, 0x25u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x5Eu));
    /* before the interview's node-information step: added */
    zw_node_find(&H.net.nodes, node)->interview_step = (uint8_t)ZW_IV_STEP_PING;
    fake_zac93_node_info(&H.zac, H.wire.now, &dev);
    h_run_for(10u);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(s2, node, 0x25u));
}

/* ---- Removing (network.md §10; ZW-016) ------------------------------------------------------- */

static const zw_remove_event_t *remove_end(void)
{
    for (size_t i = H.ev_n; i > 0u; i--) {
        if (H.ev[i - 1u].kind == ZW_NET_EV_REMOVE &&
            H.ev[i - 1u].remove.kind == ZW_REMOVE_EV_DONE) {
            return &H.ev[i - 1u].remove;
        }
    }
    return NULL;
}

/* ZW-016: "Remove a device" for one of this network: Ready, the window, NodeFound,
 * RemovingEndNode with its node ID, Done, the stop - forgotten from the cache and the security
 * manager, stored; its zones keep their bindings and show CHECK "sensor removed" (the event names
 * its generation). */
static void test_exclusion_own_device(void)
{
    h_device(&dev, 0x5E47u, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_clear_events();
    const uint32_t stops = H.zac.by_func[ZW_FUNC_REMOVE_NODE_FROM_NETWORK];
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    h_run_for(100u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REMOVE, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_EV_LISTENING, h_find(ZW_NET_EV_REMOVE, 0u)->remove.kind);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&H.zac, H.wire.now, &dev));
    h_run_for(1000u);
    const zw_remove_event_t *r = remove_end();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_DONE, r->result);
    TEST_ASSERT_EQUAL_UINT16(node, r->node);
    const zw_net_event_t *gone = h_find(ZW_NET_EV_NODE_REMOVED, node);
    TEST_ASSERT_NOT_NULL(gone);
    TEST_ASSERT_EQUAL_UINT16(1u, gone->generation);
    TEST_ASSERT_NULL(h_rec(node));
    TEST_ASSERT_FALSE(zw_s2_has_node(zw_net_s2(&H.net), node));
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    TEST_ASSERT_EQUAL_UINT(0u, zw_node_count(&back));
    TEST_ASSERT_EQUAL_UINT32(stops + 2u, H.zac.by_func[ZW_FUNC_REMOVE_NODE_FROM_NETWORK]);
}

/* ZW-016: a device of another network is removed too (node ID 0: "it belonged to another system:
 * it can be added now") and nothing of this network is forgotten; the window ends after 60 s;
 * not allowed, and busy during another operation, are refused. */
static void test_exclusion_foreign_and_refusals(void)
{
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_NOT_ALLOWED, zw_net_remove_device(&H.net, H.wire.now, false));
    h_device(&dev, 0x99u, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &dev));
    dev.status = FAKE_ZDEV_JOINED; /* in some other network */
    dev.home_id = 0x12345678u;
    dev.node = 9u;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_BUSY, zw_net_add_device(&H.net, H.wire.now, true, 0, 0));
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_BUSY, zw_net_remove_device(&H.net, H.wire.now, true));
    h_run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press_remove(&H.zac, H.wire.now, &dev));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_DONE, remove_end()->result);
    TEST_ASSERT_EQUAL_UINT16(0u, remove_end()->node);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_REMOVED, 0u));
    /* the window, alone: 60 s from Ready, then the stop */
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    h_run_for(50u);
    TEST_ASSERT_EQUAL_UINT(1u, H.ev_n);
    const uint64_t ready_at = H.ev_at[0];
    const uint64_t until = H.ev[0].remove.deadline;
    TEST_ASSERT_EQUAL_UINT64(60000u, until - ready_at);
    h_run(until - 1u);
    TEST_ASSERT_NULL(remove_end());
    h_run(until + 10u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_NO_DEVICE, remove_end()->result);
}

/* ZW-016, §10 "Remove a failed device": one that still answers a ping is refused ("It still
 * answers: use Remove a device"); a dead one is pinged 3 times 2 s apart, then IsFailedNode, then
 * RemoveFailedNode - removed and forgotten. */
static void test_remove_failed(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x74u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_UNKNOWN_NODE,
                          zw_net_remove_failed(&H.net, H.wire.now, true, 77u));
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_failed(&H.net, H.wire.now, true, node));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STILL_ANSWERS, remove_end()->result);
    TEST_ASSERT_NOT_NULL(h_rec(node));
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REMOVE_FAILED_NODE]);
    dev.status = FAKE_ZDEV_OUT; /* broken */
    /* the pings fail, but the module does not list it as failed: not removed */
    H.zac.failed_off = true;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_failed(&H.net, H.wire.now, true, node));
    h_run_for(8000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_NOT_FAILED, remove_end()->result);
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.by_func[ZW_FUNC_IS_FAILED_NODE]);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REMOVE_FAILED_NODE]);
    TEST_ASSERT_NOT_NULL(h_rec(node));
    H.zac.failed_off = false;
    dev.watch_cc = 0x00u; /* its pings */
    dev.watch_n = 0u;
    h_clear_events();
    const uint64_t t0 = H.wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_failed(&H.net, H.wire.now, true, node));
    h_run_for(3000u);
    TEST_ASSERT_NULL(remove_end()); /* still pinging: 2 s apart */
    h_run_for(5000u);
    const zw_remove_event_t *r = remove_end();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_DONE, r->result);
    TEST_ASSERT_EQUAL_UINT32(2u, H.zac.by_func[ZW_FUNC_IS_FAILED_NODE]);
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.by_func[ZW_FUNC_REMOVE_FAILED_NODE]);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_REMOVED, node));
    TEST_ASSERT_NULL(h_rec(node));
    TEST_ASSERT_FALSE(zw_node_list_test(H.zac.id.node_list, node));
    TEST_ASSERT_TRUE(H.wire.now - t0 >= 2u * ZW_REMOVE_PING_GAP_MS);
}

/* ZW-016, §10 "Device reset locally": its 5A 01 marks it gone at once (its zones CHECK), then it
 * is pinged until the module lists it failed, and removed as failed. */
static void test_device_reset_locally(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x5A, 0x71};
    plain_device(&dev, 0x75u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    h_clear_events();
    fake_zdev_reset_locally(&dev, H.wire.now);
    h_run_for(200u);
    const zw_net_event_t *g = h_find(ZW_NET_EV_NODE_GONE, node);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_INT(ZW_NET_GONE_RESET_LOCALLY, g->gone);
    TEST_ASSERT_EQUAL_UINT16(1u, g->generation);
    h_run_for(20000u);
    const zw_remove_event_t *r = remove_end();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_BY_RESET_LOCALLY, r->how);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_DONE, r->result);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_REMOVED, node));
    TEST_ASSERT_NULL(h_rec(node));
    TEST_ASSERT_FALSE(zw_node_list_test(H.zac.id.node_list, node));
}

/* ZW-016, §10: a device reset locally that the module never lists as failed - pinged in 3 rounds,
 * then left as it is (its zones show CHECK already; "Remove a failed device" can end it later). */
static void test_device_reset_locally_never_failed(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x5A, 0x71};
    plain_device(&dev, 0x69u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    H.zac.failed_off = true;
    h_clear_events();
    fake_zdev_reset_locally(&dev, H.wire.now);
    h_run_for(60000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_GONE, node));
    const zw_remove_event_t *r = remove_end();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_BY_RESET_LOCALLY, r->how);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_NOT_FAILED, r->result);
    TEST_ASSERT_EQUAL_UINT32(3u, H.zac.by_func[ZW_FUNC_IS_FAILED_NODE]);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.by_func[ZW_FUNC_REMOVE_FAILED_NODE]);
    TEST_ASSERT_NOT_NULL(h_rec(node));
}

/* ---- Region and reset (network.md §12; ZW-018) ----------------------------------------------- */

static unsigned setup_heard(uint8_t sub)
{
    unsigned n = 0u;
    for (size_t i = 0; i < H.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&H.zac, i);
        if (h != NULL && h->func == ZW_FUNC_SERIAL_API_SETUP && h->payload_len > 0u &&
            h->payload[0] == sub) {
            n++;
        }
    }
    return n;
}

/* ZW-018, §12: the region - installer only, one the module offers; SetRFRegion, the soft reset,
 * the start-up again; the devices stay (the page warns). */
static void test_region_change(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x76u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED, zw_net_set_region(&H.net, H.wire.now, false, 0x09u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_UNSUPPORTED, zw_net_set_region(&H.net, H.wire.now, true, 0x03u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_UNCHANGED, zw_net_set_region(&H.net, H.wire.now, true, 0x01u));
    h_clear_events();
    const unsigned ups = H.ups;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_region(&H.net, H.wire.now, true, 0x09u));
    TEST_ASSERT_EQUAL_UINT8(0x09u, h_find(ZW_NET_EV_REGION, 0u)->region);
    /* the start-up runs: nothing else meanwhile */
    TEST_ASSERT_EQUAL_INT(ZW_NET_LINK_DOWN, zw_net_set_region(&H.net, H.wire.now, true, 0x01u));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(ups + 1u, H.ups);
    TEST_ASSERT_EQUAL_UINT(1u, setup_heard(ZW_SETUP_SET_RF_REGION));
    TEST_ASSERT_EQUAL_UINT8(0x09u, zw_host_facts(zw_net_host(&H.net))->region);
    TEST_ASSERT_EQUAL_UINT8(0x09u, H.zac.id.region);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, zw_net_radio(&H.net));
    TEST_ASSERT_NOT_NULL(h_rec(node));
}

/* ZW-018, §12 "Reset the Z-Wave network": HardReset - a new home ID, no nodes; the cache cleared
 * and stored; fresh keys bound to the new home ID (never the old ones); installer only (the
 * "confirmed twice" is the caller's). */
static void test_network_reset(void)
{
    h_device(&dev, 0x5E49u, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    const zw_s2_keys_t old = H.cfg.keys;
    const uint32_t old_home = H.cfg.keys_home_id;
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED,
                          zw_net_reset_network(&H.net, H.wire.now, false, NULL));
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_reset_network(&H.net, H.wire.now, true, NULL));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.by_func[ZW_FUNC_HARD_RESET]);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_RESET, 0u));
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_GONE, node));
    TEST_ASSERT_EQUAL_UINT(0u, zw_node_count(zw_net_nodes(&H.net)));
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    TEST_ASSERT_EQUAL_UINT(0u, zw_node_count(&back));
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->first_start);
    TEST_ASSERT_EQUAL_UINT(2u, H.key_stores);
    TEST_ASSERT_NOT_EQUAL(old_home, H.cfg.keys_home_id);
    TEST_ASSERT_EQUAL_HEX32(H.zac.id.home_id, H.cfg.keys_home_id);
    TEST_ASSERT_FALSE(memcmp(&old, &H.cfg.keys, sizeof old) == 0);
    /* a device can be added to the new network */
    static fake_zdev_t next;
    h_device(&next, 0x5E4Au, false);
    TEST_ASSERT_NOT_EQUAL(0u, h_include(&next));
}

/* screens.md §8 "Restart the radio" (ZW-004's start-up on request): allowed only; refused while a
 * network operation runs; the start-up again with its soft reset, the check at the link up, the
 * device and its keys kept; while it starts, nothing else is taken. */
static void test_radio_restart(void)
{
    h_device(&dev, 0x5E4Bu, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED, zw_net_restart(&H.net, H.wire.now, false));
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    TEST_ASSERT_EQUAL_INT(ZW_NET_BUSY, zw_net_restart(&H.net, H.wire.now, true));
    zw_remove_stop(zw_net_rm(&H.net), H.wire.now);
    h_run_for(12000u);
    const zw_s2_keys_t keys = H.cfg.keys;
    const uint32_t resets = H.zac.by_func[ZW_FUNC_SOFT_RESET];
    const unsigned ups = H.ups;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&H.net, H.wire.now, true));
    TEST_ASSERT_NOT_EQUAL(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_LINK_DOWN, zw_net_add_device(&H.net, H.wire.now, true, 0u, 0u));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
    TEST_ASSERT_EQUAL_UINT(ups + 1u, H.ups);
    TEST_ASSERT_EQUAL_UINT32(resets + 1u, H.zac.by_func[ZW_FUNC_SOFT_RESET]);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NETWORK, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, zw_net_radio(&H.net));
    TEST_ASSERT_NOT_NULL(h_rec(node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_GONE, node));
    TEST_ASSERT_TRUE(memcmp(&keys, &H.cfg.keys, sizeof keys) == 0);
}

/* ZW-004, §12 "Restart the radio" (screens.md §8): the start-up again - its soft reset (08) and
 * every step after it, no SetRFRegion - and the check finds the network as it was: nobody
 * forgotten, added or interviewed again, the keys and the SPANs kept, so a device's report is
 * taken at once at its class. Refused when not allowed, and while a device is being added
 * (BUSY). */
static void test_restart_radio(void)
{
    h_device(&dev, 0x5E56u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    const zw_node_record_t before = *h_rec(node);
    const zw_s2_keys_t keys = H.cfg.keys;
    const unsigned key_stores = H.key_stores;
    const unsigned identity = fake_zdev_asked(&dev, 0x72, 0x04);
    const zw_s2_span_state_t span = zw_s2_span_state(zw_net_s2(&H.net), node);
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED, zw_net_restart(&H.net, H.wire.now, false));
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&H.net, H.wire.now, true, 1u, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_BUSY, zw_net_restart(&H.net, H.wire.now, true));
    zw_incl_stop(zw_net_incl(&H.net), H.wire.now);
    h_run_for(1000u);
    h_clear_events();
    const uint32_t soft = H.zac.by_func[ZW_FUNC_SOFT_RESET];
    const uint32_t caps = H.zac.by_func[ZW_FUNC_GET_SERIAL_API_CAPABILITIES];
    const unsigned regions = setup_heard(ZW_SETUP_SET_RF_REGION);
    const unsigned ups = H.ups;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&H.net, H.wire.now, true));
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_RESTART, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_LINK_STARTING, zw_host_link(zw_net_host(&H.net)));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT32(soft + 1u, H.zac.by_func[ZW_FUNC_SOFT_RESET]);
    TEST_ASSERT_EQUAL_UINT32(caps + 1u, H.zac.by_func[ZW_FUNC_GET_SERIAL_API_CAPABILITIES]);
    TEST_ASSERT_EQUAL_UINT(regions, setup_heard(ZW_SETUP_SET_RF_REGION));
    TEST_ASSERT_EQUAL_UINT(ups + 1u, H.ups);
    TEST_ASSERT_EQUAL_UINT(1u, h_count(ZW_NET_EV_RESTART, 0u));
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, e->radio);
    TEST_ASSERT_FALSE(e->first_start);
    /* the network as it was */
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_GONE, 0u));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_ADDED, 0u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(zw_net_nodes(&H.net)));
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_UINT16(before.generation, rec->generation);
    TEST_ASSERT_EQUAL_HEX8(before.security_classes, rec->security_classes);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, rec->interview_step);
    TEST_ASSERT_EQUAL_UINT32(before.wake_up_interval_seconds, rec->wake_up_interval_seconds);
    TEST_ASSERT_TRUE(rec->lifeline_set);
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(identity, fake_zdev_asked(&dev, 0x72, 0x04)); /* not interviewed */
    TEST_ASSERT_EQUAL_UINT(key_stores, H.key_stores);
    TEST_ASSERT_EQUAL_MEMORY(&keys, &H.cfg.keys, sizeof keys);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    TEST_ASSERT_EQUAL_INT(span, zw_s2_span_state(zw_net_s2(&H.net), node));
    /* its report after the restart, taken at its class */
    const size_t reports = h_rx_count(node, 0x71, 0x05);
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(reports + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, H.rx[H.rx_n - 1u].security);
}

/* ZW-004, §12 with serial-api.md §7: "Restart the radio" while the radio is not answering is "try
 * now" - the start-up begins at once, not at the recovery's next attempt (30 s after the link went
 * down); the radio back, the link is up in a moment and the network is as it was. */
static void test_restart_radio_not_answering(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x5E5Au, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    H.zac.fault.dead = true; /* unplugged: the restart's start-up finds nobody */
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&H.net, H.wire.now, true));
    for (unsigned t = 0; t < 1200u && zw_host_link(zw_net_host(&H.net)) != ZW_LINK_DOWN; t++) {
        h_run_for(100u);
    }
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(zw_net_host(&H.net)));
    H.zac.fault.dead = false; /* plugged in again */
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(zw_net_host(&H.net))); /* its next try: later */
    const uint32_t attempts = zw_host_stats(zw_net_host(&H.net))->attempts;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&H.net, H.wire.now, true));
    TEST_ASSERT_EQUAL_UINT32(attempts + 1u, zw_host_stats(zw_net_host(&H.net))->attempts);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_STARTING, zw_host_link(zw_net_host(&H.net))); /* at once */
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
    const zw_net_event_t *e = h_find(ZW_NET_EV_NETWORK, 0u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ZW_NET_RADIO_OURS, e->radio);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_GONE, 0u));
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, h_rec(node)->interview_step);
}

/* SendData frames to `node` the module heard from index *from on (moved past them): all of them
 * (len 0) or those carrying exactly `data`. Called often enough that the log never wraps. */
static unsigned send_data_to(uint16_t node, size_t *from, const uint8_t *data, size_t len)
{
    unsigned n = 0u;
    for (; *from < H.zac.heard; (*from)++) {
        const fake_heard_t *h = fake_zac93_heard(&H.zac, *from);
        TEST_ASSERT_NOT_NULL_MESSAGE(h, "the fake module's log wrapped: count more often");
        if (h->func != ZW_FUNC_SEND_DATA || h->payload_len < 2u || h->payload[0] != node) {
            continue;
        }
        if (len == 0u || ((size_t)h->payload[1] == len && h->payload_len >= 2u + len &&
                          memcmp(&h->payload[2], data, len) == 0)) {
            n++;
        }
    }
    return n;
}

/* ---- Sleeping devices and supervision, wired (network.md §7-8; ZW-011, ZW-012) --------------- */

/* ZW-012: a listening device silent for 15 min is pinged (No Operation, 3 attempts); two failed
 * pings 15 min apart make it UNREACHABLE; its next frame REACHABLE again. */
static void test_supervision_listening(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x77u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    h_run_for(10000u); /* the restart's verification ping, answered */
    /* §8: every ACK counts for a listening device - commands it takes keep it from pings */
    uint32_t pings = zw_net_stats(&H.net)->pings;
    static const uint8_t BASIC_GET[] = {0x20, 0x02};
    for (unsigned k = 0; k < 4u; k++) {
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO,
                                                    BASIC_GET, sizeof BASIC_GET, 0x50u + k));
        h_run_for(10u * 60000u);
    }
    TEST_ASSERT_EQUAL_UINT32(pings, zw_net_stats(&H.net)->pings);
    h_clear_events();
    dev.status = FAKE_ZDEV_OUT; /* unplugged */
    const uint64_t t0 = H.wire.now;
    pings = zw_net_stats(&H.net)->pings;
    size_t from = H.zac.heard;
    unsigned noops = 0u;
    static const uint8_t NOOP[] = {0x00};
    while (H.wire.now < t0 + 15u * 60000u + 1000u) {
        h_run_for(60000u);
        noops += send_data_to(node, &from, NOOP, sizeof NOOP);
    }
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node)); /* one failed ping is not enough */
    TEST_ASSERT_EQUAL_UINT32(pings + 3u, zw_net_stats(&H.net)->pings); /* its 3 attempts */
    /* ZW-037: ...and 3 frames on the air, not 3 transmissions of each attempt (9) */
    TEST_ASSERT_EQUAL_UINT(3u, noops);
    h_run(t0 + 31u * 60000u);
    const zw_net_event_t *u = h_find(ZW_NET_EV_UNREACHABLE, node);
    TEST_ASSERT_NOT_NULL(u);
    TEST_ASSERT_EQUAL_UINT16(1u, u->generation);
    dev.status = FAKE_ZDEV_JOINED; /* plugged in again: it speaks */
    static const uint8_t BASIC_SET[] = {0x20, 0x01, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_NONE, BASIC_SET,
                                                     sizeof BASIC_SET));
    h_run_for(100u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REACHABLE, node));
}

/* ZW-037, serial-api.md 6.12: a question of the interview goes at most 3 times in all to a
 * listening device that does not answer - the interview's own tries (ZW_IV_TRIES), each one
 * transmission, not 3 tries of 3. A device ready, then unplugged and interviewed again: every
 * attempt fails at its first question to the device, the No Operation of step 2, so the stalled
 * interview put exactly ZW_IV_TRIES x ZW_IV_ATTEMPTS frames on the air (45 before). */
static void test_interview_question_transmissions(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7F01u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    h_run_for(10000u); /* the restart's verification ping, answered */
    dev.status = FAKE_ZDEV_OUT; /* unplugged */
    h_clear_events();
    size_t from = H.zac.heard;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    unsigned frames = 0u;
    const uint64_t end = H.wire.now + 60000u;
    while (H.wire.now < end && h_find(ZW_NET_EV_STALLED, node) == NULL) {
        h_run_for(10u);
        frames += send_data_to(node, &from, NULL, 0u);
    }
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_UINT(ZW_IV_TRIES * ZW_IV_ATTEMPTS, frames);
}

/* ZW-037, serial-api.md 6.12: who asks again decides how often a frame goes to a listening device
 * that does not ACK. A command (here a No Operation sent by a page's Ping, with its owner's tag)
 * goes 3 times; a firmware update's frame once, as its fragments and Gets are tried again by
 * zw_fwupd itself (ZW_FWUPD_TRIES). */
static void test_transmissions_per_owner(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71, 0x7A};
    plain_device(&dev, 0x7F02u, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    h_run_for(10000u);
    dev.status = FAKE_ZDEV_OUT; /* unplugged, not yet unreachable */
    static const uint8_t NOOP[] = {0x00};
    size_t from = H.zac.heard;
    H.done_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, NOOP,
                                                sizeof NOOP, 0x5Bu));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(3u, send_data_to(node, &from, NULL, 0u));
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[0]);

    static const uint8_t MD_GET[] = {0x7A, 0x01};
    H.done_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, MD_GET,
                                                sizeof MD_GET, ZW_FWUPD_TAG_BASE | 1u));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1u, send_data_to(node, &from, NULL, 0u));
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[0]);
}

/* ZW-012, ZW-011: a sleeping device (interval T = 1 h) unreachable once T + 10 min pass without a
 * frame; a command for it waits in its queue; its wake-up - late, so its interval is read again -
 * makes it reachable, sends the command, then No More Information. */
static void test_supervision_sleeping_and_queue(void)
{
    h_device(&dev, 0x5E4Bu, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false; /* it stops checking in */
    h_clear_events();
    const uint64_t t0 = H.wire.now;
    h_run(t0 + 3600000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    h_run(t0 + 3600000u + 600000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 0x42u));
    h_run_for(60000u);
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n); /* it waits for the device */
    const unsigned rereads = fake_zdev_asked(&dev, 0x84, 0x05);
    const uint32_t nmi = dev.nmi_heard;
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REACHABLE, node));
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    TEST_ASSERT_EQUAL_UINT32(0x42u, H.done_tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[0]);
    TEST_ASSERT_TRUE(h_rx_count(node, 0x80, 0x03) >= 1u);
    TEST_ASSERT_EQUAL_UINT(rereads + 1u, fake_zdev_asked(&dev, 0x84, 0x05)); /* late: re-read */
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, dev.nmi_heard);
}

/* ZW-011, §7: a newer command of a kind replaces the older in a sleeping device's queue - also
 * one already on its way: the older's end is still told to the owner (every command it sent is
 * answered once), and the newer waits for the next wake-up. */
static void test_queue_replaces_command_out(void)
{
    h_device(&dev, 0x5E4Fu, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u); /* sent back to sleep */
    static const uint8_t SET_1[] = {0x70, 0x04, 0x05, 0x01, 0x01}; /* parameter 5 = 1 */
    static const uint8_t SET_2[] = {0x70, 0x04, 0x05, 0x01, 0x02}; /* parameter 5 = 2 */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, SET_1,
                                                sizeof SET_1, 0xA1u));
    fake_zdev_wake(&dev, H.wire.now);
    dev.status = FAKE_ZDEV_OUT; /* ...and its battery dies right after its Wake Up Notification */
    bool out = false;
    for (unsigned t = 0; t < 2000u && !out; t++) {
        h_run_for(1u);
        for (size_t i = 0; i < ZW_NET_QUEUE_OUT; i++) {
            out = out || (H.net.qout[i].used && H.net.qout[i].tag == 0xA1u);
        }
    }
    TEST_ASSERT_TRUE(out);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, SET_2,
                                                sizeof SET_2, 0xA2u));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    TEST_ASSERT_EQUAL_UINT32(0xA1u, H.done_tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_queue_len(zw_net_health(&H.net), node)); /* the newer */
}

/* How many times the owner heard a tag's end, and the last result. */
static unsigned done_count(uint32_t tag, zw_s2_result_t *last)
{
    unsigned n = 0u;
    for (size_t i = 0; i < H.done_n; i++) {
        if (H.done_tag[i] == tag) {
            n++;
            if (last != NULL) {
                *last = H.done_r[i];
            }
        }
    }
    return n;
}

/* Whether the device received this (unwrapped) command. */
static bool dev_got(const fake_zdev_t *d, const uint8_t *cmd, size_t len)
{
    for (size_t i = 0; i < d->got_n; i++) {
        if (d->got_len[i] == len && memcmp(d->got[i], cmd, len) == 0) {
            return true;
        }
    }
    return false;
}

/* ZW-011, §7, ZW13: in a sleeping device's queue a Multi Channel command is keyed by its
 * destination end point and inner command: commands for different end points - the same inner
 * command or another parameter - never replace each other; the same end point and command does,
 * and the replaced one's owner hears its end once (CANCELLED: never sent). At the wake-up the
 * others all go, each with its own tag. */
static void test_queue_keeps_end_points_apart(void)
{
    h_device(&dev, 0x5E52u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u); /* asleep */
    dev.m.auto_wake = false;
    static const uint8_t EP1_P5_1[] = {0x60, 0x0D, 0x00, 0x01, 0x70, 0x04, 0x05, 0x01, 0x01};
    static const uint8_t EP2_P5_1[] = {0x60, 0x0D, 0x00, 0x02, 0x70, 0x04, 0x05, 0x01, 0x01};
    static const uint8_t EP2_P6_1[] = {0x60, 0x0D, 0x00, 0x02, 0x70, 0x04, 0x06, 0x01, 0x01};
    static const uint8_t EP1_P5_2[] = {0x60, 0x0D, 0x00, 0x01, 0x70, 0x04, 0x05, 0x01, 0x02};
    static const uint8_t PLAIN_P5_1[] = {0x70, 0x04, 0x05, 0x01, 0x01};
    H.done_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, EP1_P5_1,
                                                sizeof EP1_P5_1, 0xB1u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, EP2_P5_1,
                                                sizeof EP2_P5_1, 0xB2u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, EP2_P6_1,
                                                sizeof EP2_P6_1, 0xB3u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, PLAIN_P5_1,
                                                sizeof PLAIN_P5_1, 0xB5u));
    TEST_ASSERT_EQUAL_UINT(4u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n);
    /* the same end point and command: replaced, its owner told at once - once */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, EP1_P5_2,
                                                sizeof EP1_P5_2, 0xB4u));
    TEST_ASSERT_EQUAL_UINT(4u, zw_health_queue_len(zw_net_health(&H.net), node));
    zw_s2_result_t r = ZW_S2_OK;
    TEST_ASSERT_EQUAL_UINT(1u, done_count(0xB1u, &r));
    TEST_ASSERT_EQUAL_INT(ZW_S2_CANCELLED, r);
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    dev.got_n = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(1u, done_count(0xB1u, NULL)); /* still once */
    const uint32_t sent[] = {0xB2u, 0xB3u, 0xB4u, 0xB5u};
    for (size_t i = 0; i < 4u; i++) {
        TEST_ASSERT_EQUAL_UINT(1u, done_count(sent[i], &r));
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, r);
    }
    TEST_ASSERT_EQUAL_UINT(5u, H.done_n);
    static const uint8_t P5_2[] = {0x70, 0x04, 0x05, 0x01, 0x02};
    static const uint8_t P6_1[] = {0x70, 0x04, 0x06, 0x01, 0x01};
    TEST_ASSERT_TRUE(dev_got(&dev, P5_2, sizeof P5_2)); /* end point 1's newer */
    TEST_ASSERT_TRUE(dev_got(&dev, P6_1, sizeof P6_1));
    TEST_ASSERT_TRUE(dev_got(&dev, PLAIN_P5_1, sizeof PLAIN_P5_1)); /* end point 2's, the root's */
}

/* ZW-011, §7, ZW13 (repair 3, 4): an encapsulation the queue does not look into - Multi Instance
 * v1, CRC-16, Supervision Get - says nothing of what it carries, so it replaces nothing (two to one
 * device, the same outer bytes, both stay); a Multi Channel command to end point 127 with an inner
 * FF FF FF is a command like any other, not refused as if it were the ping's tag. */
static void test_queue_keeps_undecoded_encapsulations(void)
{
    h_device(&dev, 0x5E53u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u); /* asleep */
    dev.m.auto_wake = false;
    static const uint8_t MI_A[] = {0x60, 0x06, 0x01, 0x25, 0x01, 0xFF};
    static const uint8_t MI_B[] = {0x60, 0x06, 0x01, 0x70, 0x04, 0x05, 0x01, 0x01};
    static const uint8_t CRC_A[] = {0x56, 0x01, 0x70, 0x04, 0x05, 0x01, 0x01, 0x12, 0x34};
    static const uint8_t CRC_B[] = {0x56, 0x01, 0x70, 0x04, 0x06, 0x01, 0x01, 0x56, 0x78};
    static const uint8_t SV_A[] = {0x6C, 0x01, 0x01, 0x03, 0x25, 0x01, 0xFF};
    static const uint8_t SV_B[] = {0x6C, 0x01, 0x01, 0x03, 0x25, 0x01, 0x00};
    static const uint8_t EP127[] = {0x60, 0x0D, 0x00, 0x7F, 0xFF, 0xFF, 0xFF};
    const uint8_t *cmds[] = {MI_A, MI_B, CRC_A, CRC_B, SV_A, SV_B};
    const size_t lens[] = {sizeof MI_A, sizeof MI_B, sizeof CRC_A, sizeof CRC_B, sizeof SV_A,
                           sizeof SV_B};
    H.done_n = 0u;
    for (size_t i = 0; i < 6u; i++) {
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, cmds[i],
                                                    lens[i], 0xD1u + (uint32_t)i));
    }
    TEST_ASSERT_EQUAL_UINT(6u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n); /* nothing replaced */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, EP127,
                                                sizeof EP127, 0xD7u));
    TEST_ASSERT_EQUAL_UINT(7u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n);
}

/* An owner that sends again, from inside zw_net_send's CANCELLED, what was just replaced. */
static struct {
    uint16_t node;
    unsigned depth, max_depth, resends;
    zw_s2_result_t again[4];
} RS;

static const uint8_t RS_EP1_A[] = {0x60, 0x0D, 0x00, 0x01, 0x70, 0x04, 0x05, 0x01, 0x01};
static const uint8_t RS_EP1_B[] = {0x60, 0x0D, 0x00, 0x01, 0x70, 0x04, 0x05, 0x01, 0x02};
static const uint8_t RS_EP2[] = {0x60, 0x0D, 0x00, 0x02, 0x70, 0x04, 0x05, 0x01, 0x03};

static bool rs_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    if (r != ZW_S2_CANCELLED || RS.resends >= 4u) {
        return false;
    }
    RS.depth++;
    RS.max_depth = RS.depth > RS.max_depth ? RS.depth : RS.max_depth;
    if (RS.depth < 4u) { /* a bound for the test: the panel must not need it */
        const uint8_t *cmd = tag == 0xE2u ? RS_EP1_B : RS_EP1_A;
        RS.again[RS.resends++] =
            zw_net_send(&H.net, H.wire.now, RS.node, ZW_SEC_AUTO, cmd, sizeof RS_EP1_A, tag);
        if (RS.resends == 1u) { /* and one for another end point, which replaces nothing */
            RS.again[RS.resends++] =
                zw_net_send(&H.net, H.wire.now, RS.node, ZW_SEC_AUTO, RS_EP2, sizeof RS_EP2, 0xE3u);
        }
    }
    RS.depth--;
    return false; /* heard as any end */
}

/* ZW-011, §7, ZW13 (repair 6): an owner told CANCELLED sends its command again from inside that
 * callback. Re-sending what would replace the newer one is refused at once (BUSY, its owner told
 * by the return) - no second replacement, no recursion past the one callback - and a re-send that
 * replaces nothing is queued. Every tag ends once: the queued ones at the wake-up. */
static void test_queue_cancelled_owner_resends(void)
{
    h_device(&dev, 0x5E54u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u); /* asleep */
    dev.m.auto_wake = false;
    memset(&RS, 0, sizeof RS);
    RS.node = node;
    H.extra.send_done = rs_send_done;
    H.done_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, RS_EP1_A,
                                                sizeof RS_EP1_A, 0xE1u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, RS_EP1_B,
                                                sizeof RS_EP1_B, 0xE2u));
    TEST_ASSERT_EQUAL_UINT(1u, RS.max_depth);
    TEST_ASSERT_EQUAL_UINT(2u, RS.resends);
    TEST_ASSERT_EQUAL_INT(ZW_S2_BUSY, RS.again[0]); /* 0xE1 again: refused, told at once */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, RS.again[1]);   /* end point 2: queued */
    zw_s2_result_t r = ZW_S2_OK;
    TEST_ASSERT_EQUAL_UINT(1u, done_count(0xE1u, &r));
    TEST_ASSERT_EQUAL_INT(ZW_S2_CANCELLED, r);
    TEST_ASSERT_EQUAL_UINT(0u, done_count(0xE2u, NULL));
    TEST_ASSERT_EQUAL_UINT(2u, zw_health_queue_len(zw_net_health(&H.net), node));
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(5000u);
    H.extra.send_done = NULL;
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(1u, done_count(0xE1u, NULL));
    TEST_ASSERT_EQUAL_UINT(1u, done_count(0xE2u, &r));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, r);
    TEST_ASSERT_EQUAL_UINT(1u, done_count(0xE3u, &r));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, r);
    TEST_ASSERT_EQUAL_UINT(3u, H.done_n);
}

/* ZW-011, §7: a device at interval 0 ("wake only by hand") is left alone - no Interval Set, not
 * supervised; one already at the check-in but waking towards nobody gets the panel as its
 * destination, at its own interval. */
static void test_wake_up_interval_rules(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71, 0x80, 0x84};
    plain_device(&dev, 0x78u, false, CCS, sizeof CCS);
    dev.m.wake_interval = 0u;
    uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x84, 0x04));
    TEST_ASSERT_EQUAL_UINT32(0u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_health_interval(zw_net_health(&H.net), node));
    static fake_zdev_t other;
    plain_device(&other, 0x79u, false, CCS, sizeof CCS);
    other.m.wake_interval = 3600u;
    other.m.wake_node = 0u;
    node = h_include(&other);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&other, 0x84, 0x04));
    TEST_ASSERT_EQUAL_UINT32(3600u, other.m.wake_interval);
    TEST_ASSERT_EQUAL_UINT8(1u, other.m.wake_node);
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_interval(zw_net_health(&H.net), node));
}

/* ---- A device's own check-in (network.md §7 "per device on its page"; ZW-011, ZW-012) -------- */

#define MIN_MS  60000u
#define HOUR_MS 3600000u

/* ZW-011, ZW-012: a door sensor's check-in lengthened from 1 h to 12 h while it sleeps. Nothing
 * goes until it wakes, and meanwhile the old interval counts: UNREACHABLE at T + 10 min. Its
 * wake-up (late, so its interval is read again too) takes the Set - at its highest class, inside
 * Supervision as the interview's was - then No More Information; the Report "done" makes it count:
 * the record, the stored cache, zw_health. Not unreachable 1 h + 10 min later; at 12 h + 10 min, to
 * the millisecond. */
static void test_check_in_lengthened_at_next_wake_up(void)
{
    h_device(&dev, 0x5E50u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u); /* back to sleep */
    dev.m.auto_wake = false; /* it wakes when the test says */
    const uint64_t heard = last_heard(node);
    const unsigned sets = fake_zdev_asked(&dev, 0x84, 0x04);
    const unsigned stores = H.stores;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    h_run_for(10u * MIN_MS);
    TEST_ASSERT_EQUAL_UINT(sets, fake_zdev_asked(&dev, 0x84, 0x04)); /* asleep: it waits */
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_queue_len(zw_net_health(&H.net), node));
    h_run(heard + HOUR_MS + 10u * MIN_MS - 1u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    h_run(heard + HOUR_MS + 10u * MIN_MS);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node)); /* the old interval counts */
    TEST_ASSERT_EQUAL_UINT32(3600u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_CHECK_IN, node));
    /* it wakes */
    const uint32_t nmi = dev.nmi_heard;
    const uint32_t supervised = dev.supervised;
    const unsigned rereads = fake_zdev_asked(&dev, 0x84, 0x05);
    dev.got_n = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
    TEST_ASSERT_EQUAL_UINT32(supervised + 1u, dev.supervised); /* the Set alone */
    const int k = got_interval_set(&dev);
    TEST_ASSERT_TRUE(k >= 0);
    static const uint8_t SET_12H[] = {0x84, 0x04, 0x00, 0xA8, 0xC0, 0x01}; /* 43200 s, node 1 */
    TEST_ASSERT_EQUAL_UINT(sizeof SET_12H, dev.got_len[k]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(SET_12H, dev.got[k], sizeof SET_12H);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, dev.got_sec[k]);
    TEST_ASSERT_EQUAL_UINT(rereads + 1u, fake_zdev_asked(&dev, 0x84, 0x05)); /* late: read again */
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, dev.nmi_heard);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REACHABLE, node));
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->took);
    TEST_ASSERT_EQUAL_UINT32(43200u, e->check_in_s);
    TEST_ASSERT_EQUAL_UINT16(1u, e->generation);
    TEST_ASSERT_EQUAL_UINT(1u, h_count(ZW_NET_EV_CHECK_IN, node));
    TEST_ASSERT_EQUAL_UINT32(43200u, dev.m.wake_interval);
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.wake_node);
    TEST_ASSERT_EQUAL_UINT32(43200u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT8(1u, h_rec(node)->wake_up_notify_node_id);
    TEST_ASSERT_EQUAL_UINT32(43200u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), node));
    TEST_ASSERT_TRUE(H.stores > stores);
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    TEST_ASSERT_EQUAL_UINT32(43200u, zw_node_find_const(&back, node)->wake_up_interval_seconds);
    /* judged by 12 h from now on */
    h_clear_events();
    const uint64_t t = last_heard(node);
    h_run(t + HOUR_MS + 10u * MIN_MS + 1000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    h_run(t + 12u * HOUR_MS + 10u * MIN_MS - 1u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    h_run(t + 12u * HOUR_MS + 10u * MIN_MS);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
}

/* ZW-011, §7: a newer choice replaces an older one still waiting - one Set goes at the wake-up, the
 * last; it is clamped to the device's maximum and rounded to its step (zw_health_choose_interval). */
static void test_check_in_last_choice_clamped_and_rounded(void)
{
    h_device(&dev, 0x5E51u, true);
    dev.m.wake_max = 36000u; /* its capabilities: 10 min to 10 h, in minutes */
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false;
    const unsigned sets = fake_zdev_asked(&dev, 0x84, 0x04);
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_queue_len(zw_net_health(&H.net), node));
    h_run_for(5u * MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04)); /* one Set: the last */
    TEST_ASSERT_EQUAL_UINT32(36000u, dev.m.wake_interval); /* clamped to its maximum */
    TEST_ASSERT_EQUAL_UINT(1u, h_count(ZW_NET_EV_CHECK_IN, node));
    TEST_ASSERT_TRUE(h_find(ZW_NET_EV_CHECK_IN, node)->took);
    TEST_ASSERT_EQUAL_UINT32(36000u, h_find(ZW_NET_EV_CHECK_IN, node)->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(36000u, h_rec(node)->wake_up_interval_seconds);
    /* rounded to its step: 4 h and 20 s is 4 h */
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14420u));
    h_run_for(MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 2u, fake_zdev_asked(&dev, 0x84, 0x04));
    TEST_ASSERT_EQUAL_UINT32(14400u, dev.m.wake_interval);
    TEST_ASSERT_EQUAL_UINT32(14400u, h_find(ZW_NET_EV_CHECK_IN, node)->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(14400u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(14400u, zw_health_interval(zw_net_health(&H.net), node));
}

/* ZW-011, §7: the refusals - not allowed; the link not up (a restart's start-up running); a node
 * the cache lacks; seconds outside 10 min..24 h (the limits themselves taken); a device still
 * interviewing; a listening device, a sleeping one without Wake Up, one woken only by hand
 * (interval 0). Never BUSY: taken while a device is being added or removed, and it holds up
 * neither. */
static void test_check_in_refusals(void)
{
    h_device(&dev, 0x5E52u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false;
    const uint64_t now = H.wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED, zw_net_set_check_in(&H.net, now, false, node, 14400u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_set_check_in(&H.net, now, true, 77u, 14400u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_set_check_in(&H.net, now, true, node, 599u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_set_check_in(&H.net, now, true, node, 86401u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_check_in(&H.net, now, true, node, 600u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_check_in(&H.net, now, true, node, 86400u));
    /* not a network operation */
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&H.net, H.wire.now, true, 1u, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    h_run_for(100u);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_LISTENING, zw_incl_state(zw_net_incl(&H.net))); /* it goes on */
    zw_incl_stop(zw_net_incl(&H.net), H.wire.now);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    zw_remove_stop(zw_net_rm(&H.net), H.wire.now);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_queue_len(zw_net_health(&H.net), node)); /* the last */
    /* the link not up */
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&H.net, H.wire.now, true));
    TEST_ASSERT_EQUAL_INT(ZW_NET_LINK_DOWN,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    /* not devices whose check-in can be set */
    static const uint8_t MAINS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    static const uint8_t NO_WAKE_UP[] = {0x5E, 0x86, 0x72, 0x85, 0x71, 0x80};
    static const uint8_t SLEEPER[] = {0x5E, 0x86, 0x72, 0x85, 0x71, 0x80, 0x84};
    static fake_zdev_t mains, no_wake_up, by_hand;
    plain_device(&mains, 0x5E57u, true, MAINS, sizeof MAINS);
    plain_device(&no_wake_up, 0x5E58u, false, NO_WAKE_UP, sizeof NO_WAKE_UP);
    plain_device(&by_hand, 0x5E59u, false, SLEEPER, sizeof SLEEPER);
    by_hand.m.wake_interval = 0u;
    const uint16_t listens = h_include(&mains);
    expect_ready(listens, 30000u);
    const uint16_t never = h_include(&no_wake_up);
    expect_ready(never, 30000u);
    const uint16_t manual = h_include(&by_hand);
    expect_ready(manual, 30000u);
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID,
                          zw_net_set_check_in(&H.net, H.wire.now, true, listens, 14400u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID,
                          zw_net_set_check_in(&H.net, H.wire.now, true, never, 14400u));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID,
                          zw_net_set_check_in(&H.net, H.wire.now, true, manual, 14400u));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&by_hand, 0x84, 0x04));
    /* still interviewing: interviewed again from step 1 */
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
}

/* ZW-011, §7: what does not take changes nothing. A Set the device did not hear (no ACK) waits for
 * its next wake-up - it counts as asleep again, nothing is said yet - then goes and takes. One it
 * answers "fail", and one it does not answer within 1 s + the round trip (No More Information held
 * until then), end there: the record, the stored cache and zw_health keep the interval it had,
 * the owner hears it did not take, and the Set is not sent again. */
static void test_check_in_failures_change_nothing(void)
{
    h_device(&dev, 0x5E54u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false;
    unsigned sets = fake_zdev_asked(&dev, 0x84, 0x04);
    unsigned stores = H.stores;
    h_clear_events();
    /* not heard */
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    h_run_for(MIN_MS);
    dev.lose_in = 1u; /* the panel's next frame never reaches it */
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets, fake_zdev_asked(&dev, 0x84, 0x04));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_CHECK_IN, node));
    TEST_ASSERT_EQUAL_UINT32(3600u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
    TEST_ASSERT_EQUAL_UINT(1u, zw_health_queue_len(zw_net_health(&H.net), node));
    h_run_for(MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
    TEST_ASSERT_TRUE(h_find(ZW_NET_EV_CHECK_IN, node)->took);
    TEST_ASSERT_EQUAL_UINT32(14400u, h_rec(node)->wake_up_interval_seconds);
    /* "fail" */
    dev.m.supervision_fail = true;
    sets = fake_zdev_asked(&dev, 0x84, 0x04);
    stores = H.stores;
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    h_run_for(MIN_MS);
    h_clear_events();
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->took);
    TEST_ASSERT_EQUAL_UINT32(43200u, e->check_in_s);
    TEST_ASSERT_EQUAL_UINT32(14400u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(14400u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
    h_run_for(MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04)); /* not again */
    TEST_ASSERT_EQUAL_UINT(1u, h_count(ZW_NET_EV_CHECK_IN, node));
    /* no answer */
    dev.m.supervision_fail = false;
    dev.m.mute_cc = 0x84u;
    dev.m.mute_cmd = 0x04u;
    dev.m.mute_n = 1u;
    dev.watch_cc = 0x84u; /* when No More Information comes */
    dev.watch_cmd = 0x08u;
    dev.watch_n = 0u;
    sets = fake_zdev_asked(&dev, 0x84, 0x04);
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 28800u));
    h_run_for(MIN_MS);
    h_clear_events();
    const uint64_t woke = H.wire.now;
    fake_zdev_wake(&dev, woke);
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
    e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->took);
    TEST_ASSERT_EQUAL_UINT32(28800u, e->check_in_s);
    const uint64_t ended = heard_at(e);
    TEST_ASSERT_TRUE(ended >= woke + 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, dev.watch_n);
    TEST_ASSERT_TRUE(dev.watch_at[0] >= ended); /* No More Information after it */
    TEST_ASSERT_EQUAL_UINT32(14400u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(14400u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
}

/* ZW-011, command-classes.md §4.14 with zones.md §8: a model whose quirks row says its firmware
 * answers supervised commands "fail" - the interview's Sets and its check-in's go without
 * Supervision (zw_iv_supervised), the same rule for both; delivered is its word, and it takes. */
static void test_check_in_follows_the_quirk(void)
{
    H.has_quirk = true;
    H.quirk.no_supervision = true;
    h_device(&dev, 0x5E55u, true);
    dev.m.supervision_fail = true; /* what its firmware would say */
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u); /* its Sets were taken: nothing failed */
    TEST_ASSERT_EQUAL_UINT32(0u, dev.supervised);
    TEST_ASSERT_EQUAL_UINT32(3600u, dev.m.wake_interval);
    h_run_for(2000u);
    dev.m.auto_wake = false;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    h_run_for(MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(0u, dev.supervised);
    TEST_ASSERT_EQUAL_UINT32(14400u, dev.m.wake_interval);
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->took);
    TEST_ASSERT_EQUAL_UINT32(14400u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(14400u, zw_health_interval(zw_net_health(&H.net), node));
}

/* §6: a Z-Wave Plus device without Association has no lifeline - no Set tried, ready at once. */
static void test_zwave_plus_without_association(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x71};
    plain_device(&dev, 0x7Au, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x85, 0x01));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NO_LIFELINE, zw_iv_inputs(h_rec(node), 0u));
}

/* command-classes.md §3: a CC its Version report calls version 0 is not supported - kept with
 * neither flag, and never asked. */
static void test_version_zero_is_unsupported(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x30};
    plain_device(&dev, 0x7Bu, true, CCS, sizeof CCS);
    dev.m.versions_n--; /* Binary Sensor (the last): version 0 */
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_cc_t *bs = cc_of(h_rec(node), 0x30);
    TEST_ASSERT_NOT_NULL(bs);
    TEST_ASSERT_FALSE(bs->supported || bs->secure);
    TEST_ASSERT_FALSE(zw_iv_has_cc(h_rec(node), 0x30));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x30, 0x01));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x30, 0x02));
}

/* network.md §2 limits: a device with more end points than the cache keeps (3) - only those are
 * asked about. */
static void test_endpoints_beyond_limit(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x8E, 0x60};
    plain_device(&dev, 0x7Cu, true, CCS, sizeof CCS);
    dev.m.ep_n = 5u; /* it says five; the fake knows three */
    for (size_t e = 0; e < FAKE_ZDEV_EPS; e++) {
        dev.m.eps[e].generic = 0x20u;
        dev.m.eps[e].ccs[0] = 0x30u;
        dev.m.eps[e].cc_n = 1u;
    }
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(3u, fake_zdev_asked(&dev, 0x60, 0x09));
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[3].used);
}

/* The interview asks at most two listening devices at once: three found together are all
 * interviewed, never more than two with a question out - also while their first answers are late
 * (each waits its 1 s). */
static void test_interviews_two_at_a_time(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    static fake_zdev_t more[3];
    for (size_t i = 0; i < 3u; i++) {
        plain_device(&more[i], 0x80u + i, true, CCS, sizeof CCS);
        more[i].m.mute_cc = 0x72u;
        more[i].m.mute_cmd = 0x04u;
        more[i].m.mute_n = 1u;
        TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &more[i]));
        fake_zac93_join(&H.zac, &more[i], (uint16_t)(5u + i));
    }
    h_boot();
    size_t most = 0u;
    for (unsigned t = 0; t < 6000u && h_count(ZW_NET_EV_NODE_READY, 0u) < 3u; t++) {
        size_t out = 0u;
        for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
            const uint8_t st = H.net.iv.slots[i].state; /* 2 sending, 3 waiting (zw_iv.c) */
            out += H.net.iv.slots[i].node != 0u && (st == 2u || st == 3u) ? 1u : 0u;
        }
        most = out > most ? out : most;
        h_run_for(5u);
    }
    TEST_ASSERT_EQUAL_UINT(3u, h_count(ZW_NET_EV_NODE_READY, 0u));
    TEST_ASSERT_EQUAL_UINT(2u, most);
}

/* ZW-010, §5 "Sleeping devices during the interview": a sleeping device's questions do not wait
 * for a turn (it is awake only briefly), nor does the question for its protocol info when it has
 * just joined: one added while two listening devices keep both turns busy - each question of
 * theirs waits its 1 s for an answer that never comes - is interviewed before they give up. */
static void test_sleeper_does_not_wait_its_turn(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    static fake_zdev_t busy[2];
    for (size_t i = 0; i < 2u; i++) {
        plain_device(&busy[i], 0x90u + i, true, CCS, sizeof CCS);
        busy[i].m.mute_cc = 0x72u;
        busy[i].m.mute_cmd = 0x04u;
        busy[i].m.mute_n = FAKE_MUTE_ALWAYS;
        TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &busy[i]));
        fake_zac93_join(&H.zac, &busy[i], (uint16_t)(5u + i));
    }
    h_boot();
    static const uint8_t SLEEPER[] = {0x5E, 0x86, 0x72, 0x85, 0x59, 0x71, 0x80, 0x84};
    plain_device(&dev, 0x92u, false, SLEEPER, sizeof SLEEPER);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 8000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, 0u)); /* the two still at it */
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), 5u));
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), 6u));
}

/* ZW-010, §5 "Sleeping devices during the interview": a sleeping device's failed attempt (its 3
 * tries unanswered) is not followed at once by the next - all five would be spent in one wake-up
 * on a device that answers nothing; the next waits for its next wake-up. */
static void test_sleeper_attempt_waits_for_wake_up(void)
{
    h_device(&dev, 0x5E4Cu, true);
    dev.m.mute_cc = 0x72u; /* it never says who it is */
    dev.m.mute_cmd = 0x04u;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    const uint16_t node = h_include(&dev);
    h_run_for(20000u); /* awake all along: 30 s after joining */
    TEST_ASSERT_EQUAL_UINT(3u, fake_zdev_asked(&dev, 0x72, 0x04));
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), node));
    fake_zdev_wake(&dev, H.wire.now); /* its button */
    fake_zac93_node_info(&H.zac, H.wire.now, &dev);
    h_run_for(10000u);
    TEST_ASSERT_EQUAL_UINT(6u, fake_zdev_asked(&dev, 0x72, 0x04)); /* the next attempt, alone */
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
}

/* ZW-010, network.md §2 with §5: the module restarts while a question waits for its answer - the
 * interview pauses and asks again once the module is back, without spending a try. */
static void test_module_restart_pauses_interview(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7Eu, true, CCS, sizeof CCS);
    dev.m.mute_cc = 0x72u;
    dev.m.mute_cmd = 0x04u;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    const uint16_t node = found_at_start(&dev);
    /* the next time it is asked, the module restarts before the answer could come */
    const unsigned asked = fake_zdev_asked(&dev, 0x72, 0x04);
    for (unsigned t = 0; t < 5000u && fake_zdev_asked(&dev, 0x72, 0x04) == asked; t++) {
        h_run_for(1u);
    }
    TEST_ASSERT_EQUAL_UINT(asked + 1u, fake_zdev_asked(&dev, 0x72, 0x04));
    dev.m.mute_n = 0u; /* it answers from now on */
    const uint32_t timeouts = zw_iv_stats(zw_net_iv(&H.net))->timeouts;
    const unsigned ups = H.ups;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&H.zac, H.wire.now + 10u, 0x03u));
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(ups + 1u, H.ups);
    TEST_ASSERT_EQUAL_UINT32(timeouts, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
    TEST_ASSERT_EQUAL_UINT(asked + 2u, fake_zdev_asked(&dev, 0x72, 0x04));
}

/* ZW-010, command-classes.md §4.14: a supervised Set the device reports failed is a failed try,
 * never taken as done - it is not read back; a device that always fails it stays interviewing at
 * its lifeline. */
static void test_supervision_fail_is_a_failed_try(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x6C, 0x71};
    plain_device(&dev, 0x7Du, true, CCS, sizeof CCS);
    dev.m.supervision_fail = true;
    const uint16_t node = found_at_start(&dev);
    h_run_for(120000u);
    const zw_net_event_t *e = h_find(ZW_NET_EV_STALLED, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_LIFELINE, e->step);
    TEST_ASSERT_EQUAL_UINT(15u, fake_zdev_asked(&dev, 0x85, 0x01)); /* 5 attempts x 3 tries */
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x85, 0x02));  /* never read back */
    TEST_ASSERT_EQUAL_UINT32(15u, dev.supervised);
}

/* A supervised Set's answer "working" (status 01, a duration), the node's one Report. The fake
 * device has no knob for it, so its Set is muted and the Report is handed to the host with the
 * session of the panel's Get (read from the interview's slot or the check-in's record). */
void zw_net_received(zw_net_t *net, const zw_s2_rx_t *rx);
static void send_working_report(uint16_t node, uint8_t session)
{
    const zw_cc_supervision_report_t r = {false, false, session, ZW_SUPERVISION_STATUS_WORKING, 5u};
    uint8_t sr[8];
    const size_t k = zw_cc_supervision_report_build(sr, sizeof sr, &r);
    zw_s2_rx_t rx;
    memset(&rx, 0, sizeof rx);
    rx.node = node;
    rx.security = ZW_SEC_S2_ACCESS;
    rx.command = sr;
    rx.len = k;
    zw_net_received(&H.net, &rx);
}

static uint8_t interview_session(uint16_t node)
{
    const zw_iv_t *iv = zw_net_iv(&H.net);
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node == node) {
            return iv->slots[i].spv_session;
        }
    }
    return 0u;
}

/* Its Set is out and answer awaited (the deadline is set once the send has ended). */
static bool interview_waiting(uint16_t node)
{
    const zw_iv_t *iv = zw_net_iv(&H.net);
    for (size_t i = 0; i < ZW_IV_SLOTS; i++) {
        if (iv->slots[i].node == node) {
            return iv->slots[i].deadline != 0u && iv->slots[i].spv_session != 0u;
        }
    }
    return false;
}

static uint8_t check_in_session(uint16_t node)
{
    for (size_t i = 0; i < ZW_NET_CHECK_INS; i++) {
        if (H.net.checkins[i].node == node) {
            return H.net.checkins[i].session;
        }
    }
    return 0u;
}

/* ZW-011, ZW-010, Supervision CC (SDS13782): the panel never asks for status updates, so a Report
 * "working" is the node's one answer - the interview's Wake Up Interval Set counts as taken, with
 * no wait for a final Report, no retry and no stall. */
static void test_supervision_working_completes_interview_step(void)
{
    h_device(&dev, 0x5E51u, true);
    dev.m.mute_cc = 0x84u;
    dev.m.mute_cmd = 0x04u;
    dev.m.mute_n = 1u;
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &dev));
    H.pin = dev.pin;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED,
                          zw_net_add_device(&H.net, H.wire.now, H.allowed, 2u, 0u));
    h_run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&H.zac, H.wire.now, &dev));
    uint16_t node = 0u;
    for (unsigned k = 0; k < 400u && node == 0u; k++) {
        h_run_for(100u);
        for (size_t i = 0; i < H.incl_n; i++) {
            if (H.incl[i].kind == ZW_INCL_EV_DONE) {
                node = H.incl[i].node;
            }
        }
    }
    TEST_ASSERT_TRUE(node != 0u);
    bool answered = false;
    for (unsigned k = 0; k < 600u && !h_until_ready(node, 0u); k++) {
        h_run_for(50u);
        if (!answered && fake_zdev_asked(&dev, 0x84, 0x04) > 0u && interview_waiting(node)) {
            send_working_report(node, interview_session(node)); /* the muted Set's answer */
            answered = true;
            h_run_for(1800u); /* past the Report's deadline: the old code asked the Set again */
            TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x84, 0x04));
        }
    }
    expect_ready(node, 30000u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
}

/* ZW-011, ZW-012: the same for a check-in change from the page: took = true on "working". */
static void test_supervision_working_takes_check_in(void)
{
    h_device(&dev, 0x5E52u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    const unsigned sets = fake_zdev_asked(&dev, 0x84, 0x04);
    dev.m.mute_cc = 0x84u;
    dev.m.mute_cmd = 0x04u;
    dev.m.mute_n = 1u;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    h_run_for(100u);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(300u);
    send_working_report(node, check_in_session(node));
    h_run_for(5000u);
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->took);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
}

/* ZW-010: "fail" still fails the check-in. */
static void test_supervision_fail_still_fails_check_in(void)
{
    h_device(&dev, 0x5E53u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.supervision_fail = true;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    h_run_for(100u);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(5000u);
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->took);
}

/* network.md §5: an answer from another end point than the one asked is not the answer - it is
 * waited for again; nothing of the other end point is kept for this one. */
static void test_answer_from_another_end_point(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x8E, 0x59, 0x60, 0x71};
    plain_device(&dev, 0x6Au, true, CCS, sizeof CCS);
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        dev.m.eps[e].generic = 0x07u;
        dev.m.eps[e].specific = 0x01u;
        dev.m.eps[e].ccs[0] = 0x71u;
        dev.m.eps[e].cc_n = 1u;
        dev.m.eps[e].notif_types[0] = e == 0u ? 0x06u : 0x05u;
        dev.m.eps[e].notif_n = 1u;
    }
    dev.m.wrong_ep_cc = 0x71u; /* end point 1's first answer comes from end point 2 */
    dev.m.wrong_ep_cmd = ZW_CMD_NOTIFICATION_SUPPORTED_GET;
    dev.m.wrong_ep_n = 1u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[1].notification_count);
    TEST_ASSERT_EQUAL_UINT8(0x06u, rec->endpoints[1].notifications[0].notification_type);
    TEST_ASSERT_EQUAL_UINT8(0x05u, rec->endpoints[2].notifications[0].notification_type);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
}

/* ZW-010, ZW-017, §5 step 4 with §8: the probe for a device whose classes were lost - one frame the
 * device cannot decrypt at a class it lacks (and the one resend after its Nonce Report) says "not
 * this class": the next class at once, no try spent again on it. */
static void test_probe_skips_classes_it_lacks(void)
{
    h_device(&dev, 0x5E4Eu, false);
    fake_zdev_listening(&dev);
    H.grant = ZW_S2_KEY_UNAUTH;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    H.blob_len = 0u; /* the stored cache is lost */
    h_clear_events();
    h_boot();
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, zw_s2_highest(zw_net_s2(&H.net), node));
    /* in Z-Wave JS's order (Security2CC.ts interview): Unauthenticated answers (granted), then
     * Authenticated and Access Control are each sent once and resent once (refused) */
    TEST_ASSERT_EQUAL_UINT32(2u, zw_s2_node_stats(zw_net_s2(&H.net), node)->resends);
}

static void set_version(fake_zdev_t *d, uint8_t cc, uint8_t v);

/* The classes of the Security 2 Commands Supported Gets the device decrypted (dev.got), in order,
 * since dev.got_n was last cleared; their number. */
static size_t csg_classes(zw_sec_class_t *out, size_t cap)
{
    size_t n = 0u;
    for (size_t i = 0; i < dev.got_n; i++) {
        if (dev.got_len[i] >= 2u && dev.got[i][0] == 0x9Fu && dev.got[i][1] == 0x0Du &&
            n < cap) {
            out[n++] = dev.got_sec[i];
        }
    }
    return n;
}

static void hold_interviews(void *ctx)
{
    (void)ctx;
    zw_iv_hold(&H.net.iv, true);
}

/* A cache-lost device (the stored cache gone, the device still in the module) - listening, so
 * that nothing it sends before step 4 teaches a class unless the test sends it. The interviews
 * are held from the start (zw_iv_hold) until the test lets them go. */
static uint16_t cache_lost_listening(uint64_t seed, uint8_t grant)
{
    h_device(&dev, seed, false);
    fake_zdev_listening(&dev);
    H.grant = grant;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(grant, h_rec(node)->security_classes);
    h_run_for(1000u);
    H.blob_len = 0u;
    h_clear_events();
    H.extra.boot = hold_interviews;
    h_boot();
    H.extra.boot = NULL;
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    TEST_ASSERT_TRUE(zw_iv_step(&H.net.iv, node) < ZW_IV_STEP_SECURE_CCS);
    dev.got_n = 0u;
    return node;
}

static void let_go(void)
{
    zw_iv_hold(&H.net.iv, false);
}

static const uint8_t OPEN_REPORT[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};

/* A report from the device at `cls`: delivered at that class (true), or not delivered. */
static bool report_delivered_at(uint16_t node, zw_sec_class_t cls)
{
    const size_t before = h_rx_count(node, 0x71, 0x05);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, cls, OPEN_REPORT,
                                                     sizeof OPEN_REPORT));
    h_run_for(1000u);
    return h_rx_count(node, 0x71, 0x05) == before + 1u && H.rx[H.rx_n - 1u].security == cls;
}

/* ZW-010, ZW-009, ZW-017, network.md §5 step 4 (Z-Wave JS Security2CC.ts interview: Unauthenticated,
 * Authenticated, Access Control, each queried once at that class; every class that answers is
 * granted - an empty list at a lower class too): a cache-lost device granted all three, nothing
 * learnt, gets exactly three Commands Supported Gets, in that order, and is recorded 0x07 - every
 * class, not only the highest - all decided. */
static void test_probe_records_every_class_in_order(void)
{
    const uint16_t node = cache_lost_listening(0x5E50u, ZW_S2_KEYS_S2);
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    let_go();
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(asked + 3u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    zw_sec_class_t order[4];
    TEST_ASSERT_EQUAL_UINT(3u, csg_classes(order, 4u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, order[0]);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, order[1]);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, order[2]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEYS_S2, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    TEST_ASSERT_FALSE(zw_s2_classes_unknown(zw_net_s2(&H.net), node));
}

/* ZW-010, ZW-009 (Security2CC.ts interview: only the first non-empty Commands Supported Report is
 * the secure list, hasReceivedSecureCommands): a cache-lost device granted Access Control and
 * Unauthenticated that lists CCs at Unauthenticated too - Authenticated is refused (it could not
 * decode it), the record says 0x05, and the secure list is the Unauthenticated answer's: a CC only
 * its later Access Control answer names is not taken. */
static void test_probe_secure_list_from_first_answer(void)
{
    const uint16_t node = cache_lost_listening(0x5E51u, ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH);
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    const uint32_t failures = dev_decrypt_failures();
    let_go();
    for (unsigned i = 0; i < 20000u && fake_zdev_asked(&dev, 0x9F, 0x0D) == asked; i++) {
        h_run_for(1u);
    }
    TEST_ASSERT_EQUAL_UINT(asked + 1u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    /* answered at Unauthenticated; its later answers also name Binary Switch */
    TEST_ASSERT_TRUE(dev.m.secure_n < FAKE_ZDEV_CCS);
    dev.m.secure_ccs[dev.m.secure_n++] = 0x25u;
    expect_ready(node, 30000u);
    TEST_ASSERT_TRUE(dev_decrypt_failures() > failures); /* the Authenticated question */
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    TEST_ASSERT_NULL(cc_of(h_rec(node), 0x25u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(zw_net_s2(&H.net), node, 0x25u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_class_for(zw_net_s2(&H.net), node, 0x71u));
}

/* ZW-010, ZW-009, ZW-017 (Z-Wave JS: decryptSinglecast's setSecurityClass(secClass, true) is
 * stored at once - securityClasses is a cache-backed map): a cache-lost device granted
 * Unauthenticated and Access Control whose first frame decrypts at Unauthenticated gets one
 * Commands Supported Get, at Unauthenticated, and is recorded 0x01; its later Access Control
 * report is delivered at Access Control and the record becomes 0x05, stored. After a restart that
 * keeps the stored cache its Access Control reports still count, and one at Unauthenticated is
 * below its class. */
static void test_class_learnt_after_single_probe_stored(void)
{
    const uint16_t node = cache_lost_listening(0x5E52u, ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH);
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    TEST_ASSERT_TRUE(report_delivered_at(node, ZW_SEC_S2_UNAUTH));
    let_go();
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(asked + 1u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    zw_sec_class_t order[4];
    TEST_ASSERT_EQUAL_UINT(1u, csg_classes(order, 4u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, order[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    h_run_for(2000u);
    const unsigned stores = H.stores;
    TEST_ASSERT_TRUE(report_delivered_at(node, ZW_SEC_S2_ACCESS));
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    h_run_for(2000u);
    TEST_ASSERT_TRUE(H.stores > stores);
    /* a restart with the stored cache */
    h_clear_events();
    h_boot();
    h_run_for(2000u);
    TEST_ASSERT_FALSE(zw_iv_running(&H.net.iv, node));
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    TEST_ASSERT_FALSE(zw_s2_classes_unknown(zw_net_s2(&H.net), node));
    TEST_ASSERT_TRUE(report_delivered_at(node, ZW_SEC_S2_ACCESS));
    const uint32_t wrong = zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level;
    TEST_ASSERT_FALSE(report_delivered_at(node, ZW_SEC_S2_UNAUTH));
    TEST_ASSERT_EQUAL_UINT32(wrong + 1u, zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level);
}

/* ZW-010, ZW-009 (Z-Wave JS Node.ts: a node information frame without Security 2 sets false only
 * the classes still undecided): a cache-lost device granted Access Control whose Access Control
 * frame was learnt before step 4, then whose node information lists no Security 2 (forged, or
 * not), keeps its class: recorded 0x04, its encrypted report delivered at Access Control, its
 * plaintext one below its class. */
static void test_nif_without_s2_keeps_learnt_class(void)
{
    H.grant = ZW_S2_KEY_ACCESS;
    h_device(&dev, 0x5E53u, false);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(1000u);
    H.blob_len = 0u;
    h_clear_events();
    h_boot();
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, h_rec(node)->security_classes);
    static const uint8_t NO_S2[] = {0x5E, 0x55, 0x6C};
    memcpy(dev.nif, NO_S2, sizeof NO_S2);
    dev.nif_len = sizeof NO_S2;
    fake_zdev_wake(&dev, H.wire.now); /* its Wake Up Notification, at Access Control: learnt */
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    TEST_ASSERT_TRUE(report_delivered_at(node, ZW_SEC_S2_ACCESS));
    const uint32_t wrong = zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level;
    TEST_ASSERT_FALSE(report_delivered_at(node, ZW_SEC_NONE));
    TEST_ASSERT_EQUAL_UINT32(wrong + 1u, zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level);
}

/* ZW-010, ZW-009 (Z-Wave JS: a transmit failure is Controller_CallbackNOK, a transmission error
 * that aborts the interview attempt with no setSecurityClass; only silence or "cannot decode"
 * refuses a class): a cache-lost listening device granted Unauthenticated and Access Control -
 * Unauthenticated answers, Authenticated is refused (not decoded), and the Access Control query is
 * never acknowledged. Access Control is not refused: the attempt failed, the next one asks the
 * granted class alone, and a later Access Control report is delivered at that class (and stored). */
static void test_probe_transmit_failure_records_nothing(void)
{
    const uint16_t node = cache_lost_listening(0x5E54u, ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH);
    let_go();
    const zw_iv_node_t *s = NULL;
    for (unsigned i = 0; i < 20000u; i++) {
        s = slot_c(node);
        if (s != NULL && s->step == ZW_IV_STEP_SECURE_CCS && s->phase == 3u) {
            break;
        }
        h_run_for(1u);
    }
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT8(3u, s->phase); /* the Access Control question, about to go */
    TEST_ASSERT_EQUAL_UINT8(0u, s->attempts);
    dev.lose_in = 1000u; /* nothing reaches it: no acknowledgement */
    for (unsigned i = 0; i < 20000u && slot_c(node) != NULL && slot_c(node)->attempts == 0u; i++) {
        h_run_for(1u);
    }
    dev.lose_in = 0u;
    TEST_ASSERT_NOT_NULL(slot_c(node));
    TEST_ASSERT_EQUAL_UINT8(1u, slot_c(node)->attempts);
    TEST_ASSERT_EQUAL_HEX8(0u, zw_s2_refused(zw_net_s2(&H.net), node) & ZW_S2_KEY_ACCESS);
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    TEST_ASSERT_TRUE(report_delivered_at(node, ZW_SEC_S2_ACCESS));
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
}

/* ZW-010: runs until `node` is READY while noting, for each watched try, its send-done time and
 * the end of its answer's wait. The first deadline the slot takes after the device heard try k is
 * that wait's end: ZW_IV_REPORT_MS plus the round trip after the send ended (zw_iv_sent), so the
 * send ended at (end + sent_at - ZW_IV_REPORT_MS) / 2. Nothing else is asked of the node between
 * two tries of one query. */
typedef struct {
    uint64_t done;     /* the send ended (the module's callback) */
    uint64_t wait_end; /* its answer's wait ended */
} try_times_t;

static void ready_noting_sends(uint16_t node, uint64_t ms, try_times_t *t, size_t n)
{
    memset(t, 0, n * sizeof *t);
    size_t seen = 0u;
    uint64_t d0 = 0u;
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end && h_find(ZW_NET_EV_NODE_READY, node) == NULL) {
        h_run_for(10u);
        const zw_iv_node_t *s = slot_c(node);
        const size_t k = dev.watch_n;
        if (s == NULL || k == 0u || k > n) {
            continue;
        }
        if (k != seen) {
            seen = k;
            d0 = s->deadline; /* try k heard: the deadline it was sent with */
        } else if (t[k - 1u].wait_end == 0u && s->deadline != d0) {
            t[k - 1u].wait_end = s->deadline;
            t[k - 1u].done = (s->deadline + s->sent_at - ZW_IV_REPORT_MS) / 2u;
        }
    }
    expect_ready(node, 1u);
}

/* ZW-010, network.md §5 step 4: try k+1 goes no sooner than ZW_IV_REPORT_MS + 500 ms after try k's
 * send ended, and 500 ms after its answer's wait ("Retrying in 500ms"). */
static void assert_tries_spaced(const try_times_t *t)
{
    for (size_t k = 0; k < 2u; k++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, t[k].wait_end, "no answer wait seen");
        TEST_ASSERT_TRUE(dev.watch_at[k + 1u] >= t[k].done + ZW_IV_REPORT_MS + 500u);
        TEST_ASSERT_TRUE_MESSAGE(dev.watch_at[k + 1u] >= t[k].wait_end + 500u,
                                 "the next try did not wait 500 ms");
    }
}

/* ZW-010, network.md §5 step 4 (Security2CC.ts interview, one class possible: "Try up to 3 times
 * on the root device", one query each - a response timeout is not resent - 500 ms apart, then
 * "Let's hope for the best"): a cache-lost device whose learnt class never answers its Commands
 * Supported Get gets exactly 3, at least 500 ms apart, and is recorded at its learnt class. */
static void test_single_class_query_three_times(void)
{
    const uint16_t node = cache_lost_listening(0x5E55u, ZW_S2_KEY_UNAUTH);
    TEST_ASSERT_TRUE(report_delivered_at(node, ZW_SEC_S2_UNAUTH));
    dev.m.mute_cc = 0x9Fu;
    dev.m.mute_cmd = 0x0Du;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    dev.watch_cc = 0x9Fu;
    dev.watch_cmd = 0x0Du;
    dev.watch_n = 0u;
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    let_go();
    try_times_t t[3];
    ready_noting_sends(node, 60000u, t, 3u);
    TEST_ASSERT_EQUAL_UINT(asked + 3u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    TEST_ASSERT_EQUAL_UINT(3u, dev.watch_n);
    assert_tries_spaced(t);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEY_UNAUTH, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, zw_s2_highest(zw_net_s2(&H.net), node));
}

/* ZW-010, network.md §5 steps 4 and 8 (Security2CC.ts interview): a known S2 node that never
 * answers Commands Supported Get - read again ("Set it up again") - gets 3 queries at its root and
 * then keeps its classes ("Let's hope for the best"), and one at each end point, which then goes
 * on to the next ("Assuming the endpoint supports all its mandatory CCs securely"): READY, its
 * classes and end points unchanged, never a stopped setup. */
static void test_known_node_silent_query_kept(void)
{
    h_device(&dev, 0x5E56u, false);
    fake_zdev_listening(&dev);
    static const uint8_t NIF[] = {0x5E, 0x55, 0x9F, 0x6C, 0x60};
    memcpy(dev.nif, NIF, sizeof NIF);
    dev.nif_len = sizeof NIF;
    set_version(&dev, 0x60u, 4u);
    dev.m.notif_n = 0u;
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &dev.m.eps[e];
        ep->generic = 0x20u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x30u;
        ep->secure_n = 1u;
        ep->binary_types[0] = 0x06u;
        ep->binary_n = 1u;
    }
    const uint16_t node = h_include(&dev);
    expect_ready(node, 60000u);
    const uint8_t classes = h_rec(node)->security_classes;
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_KEYS_S2, classes);
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[2].used);
    dev.m.mute_cc = 0x9Fu;
    dev.m.mute_cmd = 0x0Du;
    dev.m.mute_n = FAKE_MUTE_ALWAYS;
    dev.watch_cc = 0x9Fu;
    dev.watch_cmd = 0x0Du;
    dev.watch_n = 0u;
    const unsigned asked = fake_zdev_asked(&dev, 0x9F, 0x0D);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    try_times_t t[3];
    ready_noting_sends(node, 60000u, t, 3u);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_EQUAL_UINT(asked + 3u + 2u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    assert_tries_spaced(t);
    TEST_ASSERT_EQUAL_HEX8(classes, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[1].used);
    TEST_ASSERT_TRUE(h_rec(node)->endpoints[2].used);
}

/* §2: at every link up the node list is read again (GetSerialApiInitData, 3 tries) - after a
 * module restart too, whose own start-up does not read it: a device another controller added
 * meanwhile is found, even when the first two answers are garbled; three garbled answers leave
 * the list the start-up read. */
static void test_node_list_read_again(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7Fu, true, CCS, sizeof CCS);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &dev));
    fake_zac93_join(&H.zac, &dev, 6u);
    H.zac.fault.garble_func = ZW_FUNC_GET_SERIAL_API_INIT_DATA;
    H.zac.fault.garble_count = 3u;
    uint32_t reads = H.zac.by_func[ZW_FUNC_GET_SERIAL_API_INIT_DATA];
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&H.zac, H.wire.now + 10u, 0x03u));
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(reads + 3u, H.zac.by_func[ZW_FUNC_GET_SERIAL_API_INIT_DATA]);
    TEST_ASSERT_TRUE(H.net.checked);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_ADDED, 6u)); /* the module's word never came */
    H.zac.fault.garble_count = 2u;
    reads = H.zac.by_func[ZW_FUNC_GET_SERIAL_API_INIT_DATA];
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&H.zac, H.wire.now + 10u, 0x03u));
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(reads + 3u, H.zac.by_func[ZW_FUNC_GET_SERIAL_API_INIT_DATA]);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_NODE_ADDED, 6u));
    expect_ready(6u, 30000u);
}

/* ---- Scenarios from Z-Wave JS's test suite (hardening) ------------------------------------- */

/* ZW-006, ZW-015: a device whose S2 key exchange failed - the wrong PIN typed - stays in the
 * network without security, and a panel restart with the stored cache does not make it secure.
 * Mirrors Z-Wave JS's failedS2BootstrappingInterview.test.ts ("restoring a node with
 * failedS2Bootstrapping=true from cache"). One difference is the panel's own design: a device
 * whose exchange failed is kept and interviewed without security (network.md §4.1, "Added
 * without encryption: remove it and add it again"), not left uninterviewed. Its record says 00 -
 * known to hold no class - never FF "not yet known", before and after the restart; after it the
 * security manager has it with no class and the interview does not start again; nothing the panel
 * ever sent it was encrypted (so no probe for a class either); its report in the clear counts as
 * unencrypted, and an S2 frame from it is dropped. */
static void test_failed_s2_bootstrap_stays_insecure_after_restart(void)
{
    h_device(&dev, 0x5E4Bu, true);
    H.wrong_pin = true;
    const uint16_t node = h_include(&dev);
    TEST_ASSERT_EQUAL_UINT16(2u, node);
    const zw_incl_event_t *done = NULL;
    for (size_t i = 0; i < H.incl_n; i++) {
        done = H.incl[i].kind == ZW_INCL_EV_DONE ? &H.incl[i] : done;
    }
    TEST_ASSERT_NOT_NULL(done);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, done->result);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_S2_WRONG_PIN, done->failure);
    TEST_ASSERT_EQUAL_HEX8(0x00u, done->granted);
    TEST_ASSERT_EQUAL_HEX8(0x00u, h_rec(node)->security_classes);
    expect_ready(node, 30000u); /* interviewed in the clear, on its node information alone */

    h_clear_events();
    h_boot(); /* the panel restarts: RAM gone, the stored cache back */
    h_run_for(2000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_HEX8(0x00u, rec->security_classes);
    TEST_ASSERT_FALSE(rec->has_dsk);
    TEST_ASSERT_EQUAL_UINT8(ZW_IV_STEP_DONE, rec->interview_step);
    TEST_ASSERT_FALSE(zw_iv_running(zw_net_iv(&H.net), node));
    zw_s2_t *s2 = zw_net_s2(&H.net);
    TEST_ASSERT_TRUE(zw_s2_has_node(s2, node));
    TEST_ASSERT_EQUAL_HEX8(0x00u, zw_s2_granted(s2, node));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_highest(s2, node));
    const zw_s2_node_stats_t *at_dev = zw_s2_node_stats(&dev.s2, FAKE_ZDEV_PANEL);
    TEST_ASSERT_EQUAL_UINT32(0u, at_dev->decrypted);
    TEST_ASSERT_EQUAL_UINT32(0u, at_dev->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x9F, 0x0D));
    for (size_t i = 0; i < dev.got_n; i++) {
        TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, dev.got_sec[i]);
    }

    const size_t before = h_rx_count(node, 0x71, 0x05);
    const uint8_t door_open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    fake_zdev_wake(&dev, H.wire.now);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_NONE, door_open,
                                                     sizeof door_open));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, H.rx[H.rx_n - 1u].security);

    TEST_ASSERT_TRUE(zw_s2_set_key(&dev.s2, ZW_S2_KEY_ACCESS, H.cfg.keys.s2_access));
    TEST_ASSERT_TRUE(zw_s2_set_granted(&dev.s2, FAKE_ZDEV_PANEL, ZW_S2_KEY_ACCESS));
    const uint8_t door_closed[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x17, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_S2_ACCESS,
                                                     door_closed, sizeof door_closed));
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_HEX8(0x00u, zw_s2_granted(s2, node));
}

/* The model's Version CC Report for `cc`, set. */
static void set_version(fake_zdev_t *d, uint8_t cc, uint8_t v)
{
    for (size_t i = 0; i < d->m.versions_n; i++) {
        if (d->m.versions[i].cc == cc) {
            d->m.versions[i].version = v;
            return;
        }
    }
    d->m.versions[d->m.versions_n].cc = cc;
    d->m.versions[d->m.versions_n].version = v;
    d->m.versions_n++;
}

/* ZW-010, command-classes.md §3: "version 0" means "not supported" - except for a CC the node is
 * demonstrably using. Mirrors Z-Wave JS's ignoreCCVersion0ForKnownSupportedCCs.test.ts (their
 * issue #6305: a node answering 0 for Security 2 while the controller decrypts its frames). A
 * secure door sensor answers 0 for Security 2 and for Supervision; before the interview asks
 * versions it has sent a report inside a Supervision Get. Supervision stays supported (version
 * 1): the interview's Sets after it still go supervised. Security 2 is never asked (the key
 * exchange proved it) and stays supported: the sensor keeps its classes, its reports count at
 * Access Control. */
static void test_version_zero_ignored_for_ccs_in_use(void)
{
    h_device(&dev, 0x5E4Cu, true);
    set_version(&dev, 0x6Cu, 0u);
    set_version(&dev, 0x9Fu, 0u);
    dev.m.mute_cc = 0x86u; /* step 6's first question goes unanswered once: time for the report */
    dev.m.mute_cmd = 0x11u;
    dev.m.mute_n = 1u;
    const uint16_t node = h_include(&dev);
    run_until_step(node, ZW_IV_STEP_VERSIONS, 20000u);
    const uint8_t door_open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    uint8_t supervised[16];
    const size_t n = zw_cc_wrap_supervision(supervised, sizeof supervised, 9u, false, door_open,
                                            sizeof door_open);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, supervised, n));
    const uint32_t supervised_before = dev.supervised;
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    const zw_node_cc_t *spv = cc_of(rec, 0x6C);
    TEST_ASSERT_NOT_NULL(spv);
    TEST_ASSERT_TRUE(spv->supported);
    TEST_ASSERT_EQUAL_UINT8(1u, spv->version);
    TEST_ASSERT_TRUE(zw_iv_supervised(zw_net_iv(&H.net), rec));
    TEST_ASSERT_EQUAL_UINT32(supervised_before + 4u, dev.supervised); /* interval, lifeline, 2 types */
    const zw_node_cc_t *s2cc = cc_of(rec, 0x9F);
    TEST_ASSERT_NOT_NULL(s2cc);
    TEST_ASSERT_TRUE(s2cc->supported);
    TEST_ASSERT_EQUAL_HEX8(0x07u, rec->security_classes);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(zw_net_s2(&H.net), node));
    const size_t before = h_rx_count(node, 0x71, 0x05);
    fake_zdev_wake(&dev, H.wire.now);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, door_open,
                                                     sizeof door_open));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, H.rx[H.rx_n - 1u].security);
}

/* ZW-010: ...and without that evidence the rule stands: a device listing Supervision that answers
 * 0 for it and never used it is taken at its word - its Sets go without Supervision. */
static void test_version_zero_supervision_unseen_is_unsupported(void)
{
    static const uint8_t CCS[] = {0x5E, 0x6C, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7Bu, true, CCS, sizeof CCS);
    set_version(&dev, 0x6Cu, 0u);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_cc_t *spv = cc_of(h_rec(node), 0x6C);
    TEST_ASSERT_NOT_NULL(spv);
    TEST_ASSERT_FALSE(spv->supported || spv->secure);
    TEST_ASSERT_FALSE(zw_iv_supervised(zw_net_iv(&H.net), h_rec(node)));
    TEST_ASSERT_EQUAL_UINT32(0u, dev.supervised);
    TEST_ASSERT_TRUE(fake_zdev_asked(&dev, 0x85, 0x01) > 0u); /* its lifeline was set, plainly */
}

/* ZW-009, ZW-010, security.md §8: traffic to an end point of an S2 node stays encrypted even when
 * the end point's own CC list omits Security 2 - S2 belongs to the node, and its end points are
 * reached through it. Mirrors Z-Wave JS's secureNodeSecureEndpoint.test.ts ("Communicate with
 * endpoints of secure nodes securely, even if the endpoint does not list S2 as supported"). A
 * mains two-channel leak detector with S2 Unauthenticated: Multi Channel is in its node
 * information (non-secure at the root, as some devices list it), each end point's Capability
 * Report names only Z-Wave Plus Info, and each end point's own Commands Supported names Binary
 * Sensor - which the root does not have at all. Every command to an end point goes encrypted at
 * Unauthenticated (the device ignores its end points' secure CCs in the clear) and the interview
 * ends ready; an end point's report in the clear is dropped at the wrong level, the same report
 * encrypted counts. */
static void test_endpoint_traffic_of_s2_node_encrypted(void)
{
    h_device(&dev, 0x5E4Du, false);
    fake_zdev_listening(&dev);
    static const uint8_t NIF[] = {0x5E, 0x55, 0x9F, 0x6C, 0x60};
    memcpy(dev.nif, NIF, sizeof NIF);
    dev.nif_len = sizeof NIF;
    static const uint8_t ROOT_SECURE[] = {0x86, 0x72, 0x5A, 0x85, 0x8E, 0x59};
    memcpy(dev.m.secure_ccs, ROOT_SECURE, sizeof ROOT_SECURE);
    dev.m.secure_n = sizeof ROOT_SECURE;
    set_version(&dev, 0x60u, 4u);
    dev.m.notif_n = 0u;
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &dev.m.eps[e];
        ep->generic = 0x20u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x30u;
        ep->secure_n = 1u;
        ep->binary_types[0] = 0x06u; /* water */
        ep->binary_n = 1u;
    }
    H.grant = ZW_S2_KEY_UNAUTH;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 60000u);
    TEST_ASSERT_TRUE(dev.mc_secure > 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, dev.mc_clear);
    TEST_ASSERT_TRUE(fake_zdev_asked(&dev, 0x30, 0x02) >= 2u); /* each end point's Get */
    TEST_ASSERT_TRUE(zw_iv_endpoint_has_cc(h_rec(node), 1u, 0x30));
    TEST_ASSERT_TRUE(zw_iv_endpoint_has_cc(h_rec(node), 2u, 0x30));

    const uint8_t leak[] = {0x30, 0x03, 0xFF, 0x06};
    uint8_t mc[16];
    const size_t n = zw_cc_wrap_multi_channel(mc, sizeof mc, 1u, 0u, false, leak, sizeof leak);
    const size_t before = h_rx_count(node, 0x30, 0x03);
    const uint32_t wrong = zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_NONE, mc, n));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before, h_rx_count(node, 0x30, 0x03));
    TEST_ASSERT_EQUAL_UINT32(wrong + 1u, zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_S2_UNAUTH, mc, n));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x30, 0x03));
    TEST_ASSERT_EQUAL_UINT8(1u, H.rx[H.rx_n - 1u].ep);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, H.rx[H.rx_n - 1u].security);
}

/* Whether the device got a Supervision Report since it had `before`, the last one of this session,
 * "success", at `cls`. */
static bool device_got_supervision_report(uint32_t before, uint8_t session, zw_sec_class_t cls)
{
    return dev.spv_reports > before && dev.spv_session == session && dev.spv_sec == cls &&
           dev.spv_status == ZW_SUPERVISION_STATUS_SUCCESS;
}

/* ZW-007, ZW-011, the scheduler's priority order (docs/features/36 README "Design": nonce and
 * Supervision replies first): a reply the panel owes a device is not held back by anything else.
 * Mirrors Z-Wave JS's s0EncapsulationTwoNodes.test.ts ("Security S0 Nonce Get is answered while
 * waiting for a reply from another node") in S2 terms - its Nonce Report half is
 * test_zw_inclusion.c's test_nonce_reports_overtake; this is the Supervision Report. A sleeping S2
 * door sensor, asleep since its interview's No More Information, sends a report inside a
 * Supervision Get - and listens now for the panel's Supervision Report. The owner's report (as
 * zwave_zones sends it, zw_net_send) is a reply, not a command for its wake-up queue: it reaches
 * the sensor at once. And with the Serial API busy for another device - a slow RequestNodeInfo for
 * node A in flight, a second one queued - it goes as soon as the line is free, ahead of the queued
 * command. One the sensor no longer hears ends FAILED instead of waiting for its next wake-up. */
static void test_supervision_report_not_held_back(void)
{
    h_device(&dev, 0x5E4Fu, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now)); /* asleep after No More Information */
    H.answer_supervision = true;
    const uint8_t door_open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    uint8_t supervised[16];
    size_t n = zw_cc_wrap_supervision(supervised, sizeof supervised, 5u, false, door_open,
                                      sizeof door_open);
    uint32_t reports = dev.spv_reports;
    const uint64_t sent = H.wire.now;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, supervised, n));
    h_run_for(200u);
    TEST_ASSERT_TRUE_MESSAGE(device_got_supervision_report(reports, 5u, ZW_SEC_S2_ACCESS),
                             "the Supervision Report waited for the sensor's next wake-up");
    TEST_ASSERT_TRUE(dev.spv_at < sent + 100u);

    h_run_for(3000u); /* asleep again */
    fake_rule_t slow;
    memset(&slow, 0, sizeof slow);
    slow.func = ZW_FUNC_REQUEST_NODE_INFO;
    slow.respond = true;
    slow.response.len = 1u;
    slow.response.bytes[0] = 0x01u;
    slow.response.after_ms = 600u;
    TEST_ASSERT_TRUE(fake_zac93_on(&H.zac, &slow));
    static const uint8_t NODE_A[1] = {9u};
    for (uint32_t i = 0; i < 2u; i++) {
        zw_request_t r;
        memset(&r, 0, sizeof r);
        r.func = ZW_FUNC_REQUEST_NODE_INFO;
        r.payload = NODE_A;
        r.payload_len = sizeof NODE_A;
        r.expect = ZW_EXPECT_RESPONSE;
        r.prio = ZW_PRIO_NODE;
        r.tag = 0x41u + i;
        TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, zw_host_submit(zw_net_host(&H.net), &r));
    }
    h_run_for(50u); /* the first in flight, the second queued */
    const size_t from = H.zac.heard;
    n = zw_cc_wrap_supervision(supervised, sizeof supervised, 6u, false, door_open,
                               sizeof door_open);
    reports = dev.spv_reports;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, supervised, n));
    h_run_for(2000u);
    TEST_ASSERT_TRUE(device_got_supervision_report(reports, 6u, ZW_SEC_S2_ACCESS));
    size_t report = SIZE_MAX, queued = SIZE_MAX;
    for (size_t i = from; i < H.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&H.zac, i);
        if (h == NULL) {
            continue;
        }
        if (h->func == ZW_FUNC_SEND_DATA && h->payload_len > 0u && h->payload[0] == node &&
            report == SIZE_MAX) {
            report = i;
        }
        if (h->func == ZW_FUNC_REQUEST_NODE_INFO && queued == SIZE_MAX) {
            queued = i;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(report != SIZE_MAX && queued != SIZE_MAX, "not both heard");
    TEST_ASSERT_TRUE_MESSAGE(report < queued, "the Supervision Report waited behind a command");

    /* and a reply the sensor no longer hears (its radio asleep at once) is not kept for its next
     * wake-up, where it would mean nothing: it ends FAILED, and the owner hears so */
    h_run_for(3000u);
    const size_t done0 = H.done_n;
    n = zw_cc_wrap_supervision(supervised, sizeof supervised, 7u, false, door_open,
                               sizeof door_open);
    reports = dev.spv_reports;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, supervised, n));
    dev.awake_until = H.wire.now;
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_UINT32(reports, dev.spv_reports);
    bool failed = false;
    for (size_t i = done0; i < H.done_n; i++) {
        failed = failed || (H.done_tag[i] == H_SUPERVISION_TAG && H.done_r[i] == ZW_S2_FAILED);
    }
    TEST_ASSERT_TRUE_MESSAGE(failed, "the owner did not hear its reply fail");
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(reports, dev.spv_reports); /* nothing stale at its wake-up */
}

/* ZW-002: the panel is its network's only controller, so an ApplicationUpdate saying another
 * controller added a node (40) or removed one (20) changes nothing: no node added, none
 * forgotten, the cache not written, the link untouched, the named node's next report taken as
 * before; the frames reach the owner (to log them) and are counted. Mirrors Z-Wave JS's
 * proxyExclusionRemovesNode.test.ts - which removes the node, as Z-Wave JS lives among other
 * controllers; serial-api.md §6.13 decides the opposite for the panel. */
static void test_other_controller_updates_ignored(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7Eu, true, CCS, sizeof CCS);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    h_run_for(1000u);
    const unsigned stores = H.stores;
    const unsigned ups = H.ups;
    const size_t frames = H.frames_n;
    h_clear_events();
    static const uint8_t REMOVED[] = {0x20, 0x05};                         /* node 5 */
    static const uint8_t ADDED[] = {0x40, 0x09, 0x05, 0x04, 0x07, 0x01, 0x5E, 0x71}; /* node 9 */
    TEST_ASSERT_TRUE(fake_zac93_send_at(&H.zac, H.wire.now + 1u, 0x00, 0x49, REMOVED,
                                        sizeof REMOVED));
    TEST_ASSERT_TRUE(fake_zac93_send_at(&H.zac, H.wire.now + 2u, 0x00, 0x49, ADDED, sizeof ADDED));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
    TEST_ASSERT_EQUAL_UINT(ups, H.ups);
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
    TEST_ASSERT_EQUAL_UINT(0u, H.ev_n);
    TEST_ASSERT_NOT_NULL(h_rec(node));
    TEST_ASSERT_NULL(h_rec(9u));
    TEST_ASSERT_TRUE(zw_s2_has_node(zw_net_s2(&H.net), node));
    TEST_ASSERT_FALSE(zw_s2_has_node(zw_net_s2(&H.net), 9u));
    TEST_ASSERT_EQUAL_UINT32(2u, zw_net_stats(&H.net)->other_controller);
    TEST_ASSERT_EQUAL_UINT(frames + 2u, H.frames_n);
    TEST_ASSERT_EQUAL_HEX8(0x49u, H.frames[frames].func);
    TEST_ASSERT_EQUAL_HEX8(0x20u, H.frames[frames].payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x40u, H.frames[frames + 1u].payload[0]);

    const size_t before = h_rx_count(node, 0x71, 0x05);
    const uint8_t alarm[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x02, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_NONE, alarm,
                                                     sizeof alarm));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before + 1u, h_rx_count(node, 0x71, 0x05));
}

/* ---- More scenarios from Z-Wave JS's test suite (hardening, second round) ------------------ */

/* When the owner heard the i-th command from a node with this CC and command (0: the first), or
 * UINT64_MAX. */
static uint64_t rx_at(uint16_t node, uint8_t cc, uint8_t cmd, size_t i)
{
    for (size_t k = 0; k < H.rx_n; k++) {
        if (H.rx[k].node == node && H.rx[k].len >= 2u && H.rx[k].cmd[0] == cc &&
            H.rx[k].cmd[1] == cmd && i-- == 0u) {
            return H.rx[k].at;
        }
    }
    return UINT64_MAX;
}

/* The owner's last word on an inclusion's end, or NULL. */
static const zw_incl_event_t *incl_done(void)
{
    const zw_incl_event_t *done = NULL;
    for (size_t i = 0; i < H.incl_n; i++) {
        done = H.incl[i].kind == ZW_INCL_EV_DONE ? &H.incl[i] : done;
    }
    return done;
}

/* ZW-010, network.md §6: the lifeline's read-back when the device's Association Report comes in
 * two parts (reports to follow), the panel named only in the second - as a device with more
 * destinations than one Report holds sends it. Mirrors Z-Wave JS's
 * multiStageResponseNoTimeout.test.ts ("GET requests don't time out early if the response is split
 * using the 'more to follow' flag"), assemblePartialCCs.test.ts (the final part completes the
 * answer) and partialCCSessions.test.ts ("A response with a missing final report is re-requested
 * after a timeout"). The parts come 700 ms apart, the second after the first wait would have ended:
 * the read-back waits for every part - the first restarts its wait, no try is spent - the Set goes
 * once, the Get once, the lifeline is set. Interviewed again with the second part lost once: that
 * wait ends (one timeout), the Get - not the Set - is asked again, and both parts come. */
static void test_split_association_report(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7E01u, true, CCS, sizeof CCS);
    dev.m.split_nodes[0] = 7u;
    dev.m.split_nodes[1] = 8u;
    dev.m.split_n = 2u;
    dev.m.split_ms = 700u;
    dev.watch_cc = 0x85u; /* when the Get went */
    dev.watch_cmd = 0x02u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_TRUE(rec->lifeline_set);
    TEST_ASSERT_EQUAL_UINT8(1u, rec->lifeline_group);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x85, 0x01));
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x85, 0x02));
    TEST_ASSERT_EQUAL_UINT32(0u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 0u));
    /* the second part came after the first wait (1 s + the round trip) would have ended */
    TEST_ASSERT_EQUAL_UINT(1u, dev.watch_n);
    const uint64_t second = rx_at(node, 0x85, 0x03, 1u);
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, second);
    TEST_ASSERT_TRUE(second > dev.watch_at[0] + 1000u + 5u * H.zac.radio_ms);

    /* the second part lost once: re-requested after its wait, not set again */
    dev.m.split_lose_n = 1u;
    const uint32_t timeouts = zw_iv_stats(zw_net_iv(&H.net))->timeouts;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_clear_events();
    expect_ready(node, 30000u);
    TEST_ASSERT_TRUE(h_rec(node)->lifeline_set);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x85, 0x01)); /* the re-interview's one Set */
    TEST_ASSERT_EQUAL_UINT(3u, fake_zdev_asked(&dev, 0x85, 0x02)); /* its Get twice */
    TEST_ASSERT_EQUAL_UINT32(timeouts + 1u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
}

/* ZW-010, network.md §6: the same through Multi Channel Association - a device with end points,
 * the panel as node 1 end point 0 in the second part of the group's Report. */
static void test_split_multi_channel_association_report(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x8E, 0x59, 0x60, 0x71};
    plain_device(&dev, 0x7E02u, true, CCS, sizeof CCS);
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        dev.m.eps[e].generic = 0x07u;
        dev.m.eps[e].specific = 0x01u;
        dev.m.eps[e].ccs[0] = 0x71u;
        dev.m.eps[e].cc_n = 1u;
        dev.m.eps[e].notif_types[0] = e == 0u ? 0x06u : 0x05u;
        dev.m.eps[e].notif_n = 1u;
    }
    dev.m.split_nodes[0] = 9u;
    dev.m.split_n = 1u;
    dev.m.split_ms = 700u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_TRUE(h_rec(node)->lifeline_set);
    static const uint8_t MC_DEST[3] = {0x00, 0x01, 0x00};
    TEST_ASSERT_EQUAL_UINT(3u, dev.m.mca_dest_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MC_DEST, dev.m.mca_dest, 3);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x8E, 0x01));
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&dev, 0x8E, 0x02));
    TEST_ASSERT_EQUAL_UINT(2u, h_rx_count(node, 0x8E, 0x03)); /* both parts */
    TEST_ASSERT_EQUAL_UINT32(0u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 1u));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(node), 2u));
}

/* ZW-006, ZW-010, network.md §4.1 with screens.md §4 ("Keep it this way"): after a failed key
 * exchange the panel asks the device for its node information again before interviewing it. By
 * the protocol's fallback a device whose key exchange failed works without security and then lists
 * its command classes in the clear - where, joining, it listed only Z-Wave Plus Info, Transport
 * Service, Security 2 and Supervision (its other CCs were to be secure). Z-Wave JS's
 * failedS2BootstrappingInterview.test.ts does not interview such a device at all; the panel keeps
 * it (network.md §4.1), so it must learn what the device has. A door sensor whose PIN was typed
 * wrong: RequestNodeInfo once, its whole list in the record, a lifeline, push, its inputs offered -
 * a working unencrypted sensor, nothing sent to it encrypted, its report taken as unencrypted. The
 * same after its own KEX Fail (it cancelled) and after a timed-out exchange. One that still lists no
 * Association afterwards stays "Does not report". The installer's own "add it without encryption"
 * declines it too: the device falls back just the same, so it is asked again (ZW-029). */
static void test_failed_s2_bootstrap_node_info_asked_again(void)
{
    h_device(&dev, 0x7E03u, true);
    dev.nif_fallback = true;
    H.wrong_pin = true;
    uint32_t nifs = H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO];
    const uint16_t node = h_include(&dev);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_S2_WRONG_PIN, incl_done()->failure);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT32(nifs + 1u, H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO]);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_EQUAL_HEX8(0x00u, rec->security_classes);
    TEST_ASSERT_TRUE(zw_iv_has_cc(rec, 0x85));
    TEST_ASSERT_TRUE(zw_iv_has_cc(rec, 0x71));
    TEST_ASSERT_FALSE(cc_of(rec, 0x71)->secure);
    TEST_ASSERT_TRUE(rec->lifeline_set);
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.assoc[0][0]);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PUSH, rec->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, h_find(ZW_NET_EV_NODE_READY, node)->inputs);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_s2_node_stats(&dev.s2, FAKE_ZDEV_PANEL)->decrypted);
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    const size_t reports = h_rx_count(node, 0x71, 0x05);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(reports + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, H.rx[H.rx_n - 1u].security);
    H.wrong_pin = false;

    /* its own KEX Fail (it cancelled), and a key exchange that timed out */
    static fake_zdev_t cancels, silent;
    h_device(&cancels, 0x7E04u, true);
    cancels.nif_fallback = true;
    cancels.fail_at = FAKE_KEX_PUBKEY;
    cancels.fail_type = ZW_S2_KEX_FAIL_CANCEL;
    h_device(&silent, 0x7E05u, true);
    silent.nif_fallback = true;
    silent.silent_at = FAKE_KEX_PUBKEY;
    fake_zdev_t *const failing[2] = {&cancels, &silent};
    const zw_incl_s2_failure_t why[2] = {ZW_INCL_S2_DEVICE_FAIL, ZW_INCL_S2_TIMEOUT};
    for (size_t i = 0; i < 2u; i++) {
        nifs = H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO];
        const uint16_t n = h_include(failing[i]);
        TEST_ASSERT_NOT_EQUAL(0u, n);
        TEST_ASSERT_EQUAL_INT(why[i], incl_done()->failure);
        failing[i]->awake_until = H.wire.now + 30000u; /* its button: awake to be interviewed */
        expect_ready(n, 30000u);
        TEST_ASSERT_EQUAL_UINT32(nifs + 1u, H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO]);
        TEST_ASSERT_TRUE(h_rec(n)->lifeline_set);
        TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(n), 0u));
    }

    /* a device whose node information has no Association after it either: "Does not report" */
    static fake_zdev_t stays;
    h_device(&stays, 0x7E06u, true);
    H.wrong_pin = true;
    nifs = H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO];
    const uint16_t mute = h_include(&stays);
    H.wrong_pin = false;
    expect_ready(mute, 30000u);
    TEST_ASSERT_EQUAL_UINT32(nifs + 1u, H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO]);
    TEST_ASSERT_FALSE(zw_iv_has_cc(h_rec(mute), 0x85));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_NO_LIFELINE, zw_iv_inputs(h_rec(mute), 0u));

    /* the installer's own "add it without encryption" (ZW-029, docs/bugs/B015): a KEX Fail to the
     * device all the same - it falls back and lists everything in the clear, so it is asked again
     * and offers its inputs (it used to keep its joining list and offer none) */
    static fake_zdev_t chosen;
    h_device(&chosen, 0x7E07u, true);
    chosen.nif_fallback = true;
    H.cancel_at_grant = true;
    nifs = H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO];
    const uint16_t plain = h_include(&chosen);
    H.cancel_at_grant = false;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_S2_CANCELLED, incl_done()->failure);
    chosen.awake_until = H.wire.now + 30000u;
    expect_ready(plain, 30000u);
    TEST_ASSERT_EQUAL_UINT32(nifs + 1u, H.zac.by_func[ZW_FUNC_REQUEST_NODE_INFO]);
    TEST_ASSERT_TRUE(h_rec(plain)->lifeline_set);
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(h_rec(plain), 0u));
}

/* ZW-010, ZW-013, network.md §5 steps 8 and 11: a Notification CC only an end point lists is
 * interviewed at that end point. Mirrors Z-Wave JS's queryVersionOfEndpointOnlyCCs.test.ts ("The
 * version of endpoint-only CCs gets queried during the interview"): the root answers Version for
 * its end points' CCs (the Multi Channel specification), so step 8 asks it for Notification's
 * version, kept in the record without the "supported" flags - the device itself still has no
 * Notification. A mains two-channel sensor whose root lists none: each end point's types, events,
 * push or pull and state are read at that end point, both offer their inputs, the root offers
 * nothing of Notification, and the version is stored with the cache. Before, the notification step
 * read the version from the root's own list (none) and skipped every end point. */
static void test_endpoint_only_notification(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x8E, 0x59, 0x60};
    plain_device(&dev, 0x7E08u, true, CCS, sizeof CCS);
    set_version(&dev, 0x71u, 8u); /* the root's answer for its end points */
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        dev.m.eps[e].generic = 0x07u;
        dev.m.eps[e].specific = 0x01u;
        dev.m.eps[e].ccs[0] = 0x5Eu;
        dev.m.eps[e].ccs[1] = 0x71u;
        dev.m.eps[e].cc_n = 2u;
        dev.m.eps[e].notif_types[0] = e == 0u ? 0x06u : 0x05u;
        dev.m.eps[e].notif_n = 1u;
    }
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_FALSE(zw_iv_has_cc(rec, 0x71));
    const zw_node_cc_t *n = cc_of(rec, 0x71);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_UINT8(8u, n->version);
    TEST_ASSERT_FALSE(n->supported || n->secure);
    for (uint8_t e = 1u; e <= 2u; e++) {
        TEST_ASSERT_EQUAL_UINT8(1u, rec->endpoints[e].notification_count);
        TEST_ASSERT_TRUE(rec->endpoints[e].notifications[0].event_mask_len > 0u); /* v3+ events */
        TEST_ASSERT_TRUE(rec->endpoints[e].has_push_pull);
        TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PUSH, rec->endpoints[e].push_pull_mode);
        TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, e));
    }
    TEST_ASSERT_EQUAL_UINT8(0x06u, rec->endpoints[1].notifications[0].notification_type);
    TEST_ASSERT_EQUAL_UINT8(0x05u, rec->endpoints[2].notifications[0].notification_type);
    TEST_ASSERT_EQUAL_UINT8(0u, rec->endpoints[0].notification_count);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x71, 0x07)); /* Supported Get, each */
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x71, 0x01)); /* Event Supported Get */
    zw_node_cache_t back;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&back, H.blob, H.blob_len));
    const zw_node_cc_t *stored = cc_of(zw_node_find_const(&back, node), 0x71);
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_UINT8(8u, stored->version);
    TEST_ASSERT_FALSE(stored->supported || stored->secure);
}

/* ZW-012, network.md §8: commands for a device that has become unreachable end promptly - the
 * owner hears each one failed, none waits for ever - and once the device is heard from again a
 * fresh command goes normally. Mirrors Z-Wave JS's nodeDeadReject.test.ts ("all pending commands
 * get rejected"; "commands sent via the commandClasses API afterwards are still attempted") and
 * controllerJammed.test.ts's third case (a node the controller cannot reach must not keep the
 * others from being served). A mains S2 sensor unplugged, unreachable after its two failed pings:
 * three commands sent to it back to back each end FAILED within a second, in order; a command to
 * another device meanwhile is delivered; nothing stays queued. Plugged in again it speaks, is
 * reachable, and a new command to it is delivered. */
static void test_commands_to_unreachable_node_end_promptly(void)
{
    h_device(&dev, 0x7E09u, false);
    fake_zdev_listening(&dev);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    static const uint8_t PLAIN[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    static fake_zdev_t other;
    plain_device(&other, 0x7E0Au, true, PLAIN, sizeof PLAIN);
    const uint16_t node2 = h_include(&other);
    expect_ready(node2, 30000u);
    h_run_for(1000u);
    const fake_zdev_status_t was = dev.status;
    dev.status = FAKE_ZDEV_OUT; /* unplugged */
    h_clear_events();
    const uint64_t t0 = H.wire.now;
    h_run(t0 + 32u * MIN_MS);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node2));

    static const uint8_t BASIC_GET[] = {0x20, 0x02};
    H.done_n = 0u;
    const uint64_t sent = H.wire.now;
    for (uint32_t k = 0; k < 3u; k++) {
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO,
                                                    BASIC_GET, sizeof BASIC_GET, 0x61u + k));
    }
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node2, ZW_SEC_AUTO, BASIC_GET,
                                                sizeof BASIC_GET, 0x71u));
    h_run(sent + 1000u);
    TEST_ASSERT_EQUAL_UINT(4u, H.done_n);
    uint32_t next = 0x61u;
    for (size_t i = 0; i < H.done_n; i++) {
        if (H.done_tag[i] == 0x71u) {
            TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[i]); /* the other device is served */
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32(next, H.done_tag[i]); /* in order */
        TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[i]);
        next++;
    }
    TEST_ASSERT_EQUAL_UINT32(0x64u, next);
    TEST_ASSERT_EQUAL_UINT(0u, zw_health_queue_len(zw_net_health(&H.net), node));

    /* plugged in again: it speaks, it is reachable, a new command goes */
    dev.status = was;
    const uint8_t open[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&dev, H.wire.now, ZW_SEC_AUTO, open,
                                                     sizeof open));
    h_run_for(1000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REACHABLE, node));
    H.done_n = 0u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, BASIC_GET,
                                                sizeof BASIC_GET, 0x65u));
    h_run_for(2000u);
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    TEST_ASSERT_EQUAL_UINT32(0x65u, H.done_tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[0]);
}

/* ZW-010, ZW-011, network.md §7 "Waking": a sleeping device's node information on its own - no Wake
 * Up Notification - releases its interview waiting for it. Mirrors Z-Wave JS's
 * compat/reInterviewWakeUpNIF.test.ts ("Re-interviews that wait for a wake-up start when receiving
 * a NIF"). A secure door sensor, ready and asleep, interviewed again: the first question for the
 * device waits ("Wake the device to finish"); its button sends just its node information; the
 * interview runs to its end in that wake-up, then No More Information. */
static void test_node_info_releases_reinterview(void)
{
    h_device(&dev, 0x7E0Bu, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now));
    dev.m.auto_wake = false;
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_run_for(3000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), node));
    const uint32_t wakeups = dev.wakeups;
    const uint32_t nmi = dev.nmi_heard;
    dev.m.wake_node = 0u; /* its button: awake, and no Wake Up Notification */
    fake_zdev_wake(&dev, H.wire.now);
    fake_zac93_node_info(&H.zac, H.wire.now, &dev);
    expect_ready(node, 20000u);
    TEST_ASSERT_EQUAL_UINT32(wakeups, dev.wakeups);
    TEST_ASSERT_EQUAL_UINT(1u, h_count(ZW_NET_EV_WAKE_NEEDED, node));
    TEST_ASSERT_EQUAL_UINT32(nmi, dev.nmi_heard); /* never during the interview */
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, dev.nmi_heard);
}

/* ZW-011, network.md §7: a re-interview started just as a sleeping device's Wake Up Notification
 * arrives completes in that wake-up rather than waiting for the next. Mirrors Z-Wave JS's
 * reInterviewAssumeAwake.test.ts ("Assume a node to be awake at the start of a re-interview"):
 * started while the device sleeps, its Wake Up Notification 5 ms later carries it through; and its
 * Wake Up Notification first, the re-interview 100 ms later - before No More Information would have
 * sent it back to sleep - holds No More Information back until the interview is done. One wake-up
 * each. */
static void test_reinterview_at_wake_up(void)
{
    h_device(&dev, 0x7E0Cu, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now));
    dev.m.auto_wake = false;
    h_clear_events();
    uint32_t wakeups = dev.wakeups;
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    h_run_for(5u);
    fake_zdev_wake(&dev, H.wire.now);
    expect_ready(node, 20000u);
    TEST_ASSERT_EQUAL_UINT32(wakeups + 1u, dev.wakeups);
    h_run_for(3000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now)); /* sent back to sleep once done */

    h_clear_events();
    wakeups = dev.wakeups;
    dev.watch_cc = 0x84u; /* when No More Information comes */
    dev.watch_cmd = 0x08u;
    dev.watch_n = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(100u);
    TEST_ASSERT_EQUAL_UINT(0u, dev.watch_n);
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, node));
    expect_ready(node, 20000u);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT32(wakeups + 1u, dev.wakeups);
    TEST_ASSERT_EQUAL_UINT(1u, dev.watch_n);
    TEST_ASSERT_TRUE(dev.watch_at[0] >= heard_at(h_find(ZW_NET_EV_NODE_READY, node)));
}

/* ZW-033: a device held awake (zw_net_hold_awake: a firmware update it drives) gets no No More
 * Information when it wakes and its queue is empty; let go, it gets one and sleeps. */
static void test_hold_awake(void)
{
    h_device(&dev, 0x7E0Du, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now));
    dev.m.auto_wake = false;
    dev.watch_cc = 0x84u;
    dev.watch_cmd = 0x08u;
    dev.watch_n = 0u;
    zw_net_hold_awake(&H.net, H.wire.now, node);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, dev.watch_n);
    zw_net_hold_awake(&H.net, H.wire.now, 0u);
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, dev.watch_n);
    TEST_ASSERT_EQUAL_UINT16(0u, H.net.hold_awake);
}

/* What the mains sensor had heard when the inclusion ended, and whether the PIN was asked for. */
static fake_zdev_t mains_dev;
static uint32_t mains_heard_at_done;
static bool pin_asked;

static void note_incl(const zw_incl_event_t *ev)
{
    if (ev->kind == ZW_INCL_EV_PIN) {
        pin_asked = true;
    } else if (ev->kind == ZW_INCL_EV_DONE) {
        mains_heard_at_done = mains_dev.heard_n;
    }
}

/* ZW-005, network.md §4 ("The panel asks nothing of other nodes while it runs"): a device being
 * added - its key exchange and PIN window included - is not disturbed by another node's interview,
 * which carries on once the new device has joined. Mirrors Z-Wave JS's
 * pauseInterviewDuringInclusion.test.ts ("an ongoing interview stays paused until the S2
 * bootstrapping of another node is completed"). A mains sensor interviewed again is in the middle
 * of its versions when "Add a device" opens; a door sensor joins with S2 Access Control, its PIN
 * typed 5 s after it was asked for: from the window's opening to the end of the key exchange the
 * mains sensor hears nothing; then its interview ends, and the door sensor's does too. */
static void test_inclusion_not_disturbed_by_interview(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&mains_dev, 0x7E0Du, true, CCS, sizeof CCS);
    const uint16_t mains = found_at_start(&mains_dev);
    expect_ready(mains, 30000u);
    h_run_for(10000u); /* the restart's verification ping, answered */
    TEST_ASSERT_TRUE(zw_net_interview(&H.net, H.wire.now, mains));
    run_until_step(mains, ZW_IV_STEP_VERSIONS, 5000u);
    h_device(&dev, 0x7E0Eu, true);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &dev));
    H.hold_pin = true;
    H.on_incl = note_incl;
    pin_asked = false;
    mains_heard_at_done = 0u;
    const size_t incl_from = H.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&H.net, H.wire.now, true, 2u, 0u));
    h_run_for(100u); /* a question already out has ended */
    const uint32_t heard = mains_dev.heard_n;
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), mains));
    TEST_ASSERT_TRUE(fake_zac93_press(&H.zac, H.wire.now, &dev));
    for (unsigned i = 0; i < 200u && !pin_asked; i++) {
        h_run_for(50u);
    }
    TEST_ASSERT_TRUE(pin_asked);
    h_run_for(5000u); /* the installer reads the label */
    TEST_ASSERT_EQUAL_UINT32(heard, mains_dev.heard_n);
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), mains));
    TEST_ASSERT_TRUE(zw_incl_pin(zw_net_incl(&H.net), H.wire.now, dev.pin));
    const zw_incl_event_t *done = NULL;
    for (unsigned i = 0; i < 400u && done == NULL; i++) {
        h_run_for(50u);
        for (size_t k = incl_from; k < H.incl_n; k++) {
            done = H.incl[k].kind == ZW_INCL_EV_DONE ? &H.incl[k] : done;
        }
    }
    TEST_ASSERT_NOT_NULL(done);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, done->result);
    TEST_ASSERT_EQUAL_INT(ZW_INCL_SEC_S2_ACCESS, done->security);
    TEST_ASSERT_EQUAL_UINT32(heard, mains_heard_at_done); /* nothing through the key exchange */
    const uint16_t node = done->node;
    H.on_incl = NULL;
    H.hold_pin = false;
    expect_ready(mains, 30000u);
    TEST_ASSERT_TRUE(mains_dev.heard_n > heard); /* its interview went on */
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_HEX8(0x07u, h_rec(node)->security_classes);
}

/* ZW-010, network.md §5 "Push or pull": a device with Association but no Association Group
 * Information, Notification version 2 (it cannot say which events it has), whose Notification Get
 * is answered, is push and offers its inputs. Mirrors Z-Wave JS's notificationPushNoAGI.test.ts
 * ("Push nodes without AGI support are detected as push, not pull", their issue #6143) as a
 * regression test for the same rule here: without AGI one type is enabled and read - FF is push.
 * No AGI and no Event Supported Get are asked; every type is enabled after the probe. */
static void test_push_without_agi(void)
{
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x71};
    plain_device(&dev, 0x7E0Fu, true, CCS, sizeof CCS);
    set_version(&dev, 0x71u, 2u);
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 30000u);
    const zw_node_record_t *rec = h_rec(node);
    TEST_ASSERT_TRUE(rec->endpoints[0].has_push_pull);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_MODE_PUSH, rec->endpoints[0].push_pull_mode);
    TEST_ASSERT_EQUAL_UINT8(2u, rec->endpoints[0].notification_count);
    TEST_ASSERT_EQUAL_UINT8(0u, rec->endpoints[0].notifications[0].event_mask_len); /* unsaid */
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, zw_iv_inputs(rec, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_IV_INPUTS_OFFERED, h_find(ZW_NET_EV_NODE_READY, node)->inputs);
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x59, 0x03));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x59, 0x05));
    TEST_ASSERT_EQUAL_UINT(0u, fake_zdev_asked(&dev, 0x71, 0x01));
    TEST_ASSERT_TRUE(fake_zdev_asked(&dev, 0x71, 0x04) >= 1u);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, dev.m.notif_status[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, dev.m.notif_status[1]);
}

/* ZW-011, command-classes.md §4.14 and §6 ("Supervision Get (session s) - must match session s"): a
 * Supervision Report with a stale or wrong session ID is never taken as the device's word. Mirrors
 * Z-Wave JS's cc/SupervisionCC.test.ts (a Report answering another session is "unexpected"). A
 * quirk's Configuration Set answered twice with the session of the Get before it - a stale Report -
 * waits out 1 s plus the round trip each time and is taken on its third try, with its own session.
 * A device's own check-in whose Set is answered with the next session: not confirmed - it ends not
 * taken when its wait is over; the record, the stored cache and supervision keep the old interval.
 */
static void test_stale_supervision_session_not_confirmation(void)
{
    H.has_quirk = true;
    H.quirk.config_count = 1u;
    H.quirk.config[0].parameter = 7u;
    H.quirk.config[0].size = 1u;
    H.quirk.config[0].value = 3;
    h_device(&dev, 0x7E10u, true);
    dev.m.spv_wrong_cc = 0x70u;
    dev.m.spv_wrong_n = 2u;
    dev.m.spv_stale = true;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(3u, fake_zdev_asked(&dev, 0x70, 0x04));
    TEST_ASSERT_EQUAL_UINT32(2u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
    TEST_ASSERT_TRUE(h_rec(node)->has_quirk);

    h_run_for(2000u);
    dev.m.auto_wake = false;
    dev.m.spv_wrong_cc = 0x84u;
    dev.m.spv_wrong_n = 1u;
    dev.m.spv_stale = false;
    const unsigned stores = H.stores;
    const unsigned sets = fake_zdev_asked(&dev, 0x84, 0x04);
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    h_run_for(MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->took);
    TEST_ASSERT_EQUAL_UINT32(3600u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
}

/* ---- The bridge to the zones (zwave_zones), handed zw_net's news by the harness ------------- */

static struct {
    zwz_t bridge;
    zsrc_zw_t src;
} Z;

static void z_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t v, uint64_t t)
{
    (void)ctx;
    (void)a;
    (void)state;
    (void)v;
    (void)t;
}

static void z_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t t)
{
    (void)ctx;
    (void)a;
    (void)ev;
    (void)t;
}

static void z_reachable(void *ctx, const zsrc_address_t *a, bool ok, uint64_t t)
{
    (void)ctx;
    (void)a;
    (void)ok;
    (void)t;
}

static void z_link(void *ctx, uint8_t kind, bool up, uint64_t t)
{
    (void)ctx;
    (void)kind;
    (void)up;
    (void)t;
}

static void x_boot(void *ctx)
{
    (void)ctx;
    static const zsrc_sink_t sink = {NULL, z_input, z_event, z_reachable, z_link, NULL};
    zsrc_zw_init(&Z.src, &sink);
    zwz_init(&Z.bridge, &H.net, &Z.src, NULL);
}

static void x_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    zwz_link_up(&Z.bridge, facts, H.wire.now);
}

static void x_link_down(void *ctx)
{
    (void)ctx;
    zwz_link_down(&Z.bridge, H.wire.now);
}

static void x_received(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    zwz_received(&Z.bridge, rx, H.wire.now);
}

static bool x_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    return zwz_send_done(&Z.bridge, tag, r);
}

static void x_event(void *ctx, const zw_net_event_t *ev)
{
    (void)ctx;
    zwz_event(&Z.bridge, ev, H.wire.now);
}

static void x_poll(void *ctx, uint64_t now)
{
    (void)ctx;
    zwz_poll(&Z.bridge, now);
}

static uint64_t x_deadline(void *ctx)
{
    (void)ctx;
    return zwz_next_deadline(&Z.bridge);
}

/* security.md §8, zones.md §3 "Fresh state" (the bridge, zwave_zones' next_get): the bridge asks an
 * S2 node's end points for their state at the node's highest class - zw_s2_class_for_endpoint, the
 * rule the interview's own questions follow - never in the clear, even for a CC only the end point
 * lists. The device of test_endpoint_traffic_of_s2_node_encrypted (a mains two-channel leak detector,
 * S2 Unauthenticated, Binary Sensor only in its end points' own Commands Supported): both end
 * points' inputs watched, each end point's Binary Sensor Get goes encrypted and is answered from
 * that end point; before, the bridge took Binary Sensor's class at the root (none) and sent them in
 * the clear, where the device ignores them. */
static void test_bridge_asks_end_points_at_their_class(void)
{
    const h_extra_t extra = {NULL,       x_boot,       x_link_up, x_link_down, x_received,
                             x_send_done, x_event,     x_poll,    x_deadline};
    H.extra = extra;
    h_boot(); /* the panel with its bridge */
    TEST_ASSERT_TRUE(Z.src.link_up);
    h_device(&dev, 0x7E11u, false);
    fake_zdev_listening(&dev);
    static const uint8_t NIF[] = {0x5E, 0x55, 0x9F, 0x6C, 0x60};
    memcpy(dev.nif, NIF, sizeof NIF);
    dev.nif_len = sizeof NIF;
    static const uint8_t ROOT_SECURE[] = {0x86, 0x72, 0x5A, 0x85, 0x8E, 0x59};
    memcpy(dev.m.secure_ccs, ROOT_SECURE, sizeof ROOT_SECURE);
    dev.m.secure_n = sizeof ROOT_SECURE;
    set_version(&dev, 0x60u, 4u);
    set_version(&dev, 0x30u, 2u); /* the root's answer for its end points */
    dev.m.notif_n = 0u;
    dev.m.ep_n = 2u;
    for (size_t e = 0; e < 2u; e++) {
        fake_ep_t *ep = &dev.m.eps[e];
        ep->generic = 0x20u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = 0x30u;
        ep->secure_n = 1u;
        ep->binary_types[0] = 0x06u; /* water */
        ep->binary_n = 1u;
    }
    H.grant = ZW_S2_KEY_UNAUTH;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 60000u);
    h_run_for(2000u);
    const zsrc_zw_device_t *d = zsrc_zw_find(&Z.src, node);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(d->ready);
    zsrc_candidate_t c[16];
    const size_t n = zsrc_zw_ops()->candidates(&Z.src, NULL, 0u, c, 16u);
    const uint32_t clear = dev.mc_clear;
    const unsigned gets = fake_zdev_asked(&dev, 0x30, 0x02);
    const size_t answers = h_rx_count(node, 0x30, 0x03);
    size_t watched = 0u;
    for (size_t i = 0; i < n; i++) {
        uint32_t home = 0u;
        uint16_t gen = 0u;
        uint16_t nd = 0u;
        uint8_t ep = 0u;
        uint8_t slot = 0u;
        if (zsrc_zw_address_parse(&c[i].addr, &home, &gen, &nd, &ep, &slot) && nd == node &&
            ep >= 1u) {
            TEST_ASSERT_TRUE(zsrc_zw_ops()->watch(&Z.src, &c[i].addr, NULL, H.wire.now));
            watched++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(2u, watched);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(gets + 2u, fake_zdev_asked(&dev, 0x30, 0x02));
    TEST_ASSERT_EQUAL_UINT32(clear, dev.mc_clear); /* never in the clear */
    TEST_ASSERT_EQUAL_UINT(answers + 2u, h_rx_count(node, 0x30, 0x03));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, H.rx[H.rx_n - 1u].security);
    TEST_ASSERT_TRUE(Z.bridge.stats.gets >= 2u);
    memset(&H.extra, 0, sizeof H.extra);
}

/* ZW-010, command-classes.md §4.25: Application Status answering one of the interview's Sets - a
 * quirk's Configuration Set - is that Set failing, promptly, never silence or "done". Mirrors Z-Wave
 * JS's applicationRejectedRequest.test.ts ("Application Rejected Request should be converted to
 * SupervisionResult with Fail status") with its Busy sibling. A secure door sensor (Supervision)
 * answers the Set Busy "try again in 2 s": no Supervision Report, and none awaited - the next try 2 s
 * later, no timeout counted - then carries it out: two Sets, ready, the parameter set once. */
static void test_application_busy_answers_quirk_set(void)
{
    H.has_quirk = true;
    H.quirk.config_count = 1u;
    H.quirk.config[0].parameter = 9u;
    H.quirk.config[0].size = 1u;
    H.quirk.config[0].value = 1;
    h_device(&dev, 0x7E12u, true);
    dev.m.app_status_cc = 0x70u;
    dev.m.app_status_cmd = ZW_CMD_APPLICATION_BUSY;
    dev.m.app_busy_status = ZW_APPLICATION_BUSY_WAIT;
    dev.m.app_busy_wait_s = 2u;
    dev.m.app_status_n = 1u;
    dev.watch_cc = 0x70u;
    dev.watch_cmd = 0x04u;
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x70, 0x04));
    TEST_ASSERT_EQUAL_UINT(2u, dev.watch_n);
    const uint64_t busy_at = rx_at(node, 0x22, 0x01, 0u);
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, busy_at);
    TEST_ASSERT_TRUE_MESSAGE(dev.watch_at[1] >= busy_at + 2000u,
                             "tried again before the wait time it named");
    TEST_ASSERT_TRUE_MESSAGE(dev.watch_at[1] < busy_at + 2200u,
                             "not tried again after the wait time it named");
    TEST_ASSERT_EQUAL_UINT32(0u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
    TEST_ASSERT_EQUAL_UINT(1u, dev.m.config_n);
    TEST_ASSERT_EQUAL_UINT8(9u, dev.m.config[0].parameter);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
}

/* ZW-010, command-classes.md §4.25: a Set without Supervision to a device that lists Application
 * Status is done only once 1 s plus the round trip passed without a word from it - so its Rejected
 * Request is not taken for success. A mains sensor without Supervision rejects a quirk's
 * Configuration Set once: a failed try, the Set goes again at once and is taken; no timeout counted.
 * One that does not list Application Status still has its Set done when it was delivered. */
static void test_application_rejected_request_without_supervision(void)
{
    H.has_quirk = true;
    H.quirk.config_count = 1u;
    H.quirk.config[0].parameter = 4u;
    H.quirk.config[0].size = 1u;
    H.quirk.config[0].value = 0;
    static const uint8_t CCS[] = {0x5E, 0x86, 0x72, 0x85, 0x22, 0x70, 0x71};
    plain_device(&dev, 0x7E13u, true, CCS, sizeof CCS);
    dev.m.app_status_cc = 0x70u;
    dev.m.app_status_cmd = ZW_CMD_APPLICATION_REJECTED_REQUEST;
    dev.m.app_status_n = 1u;
    dev.watch_cc = 0x70u;
    dev.watch_cmd = 0x04u;
    const uint16_t node = found_at_start(&dev);
    expect_ready(node, 60000u);
    TEST_ASSERT_EQUAL_UINT(2u, fake_zdev_asked(&dev, 0x70, 0x04));
    TEST_ASSERT_EQUAL_UINT(2u, dev.watch_n);
    const uint64_t rejected_at = rx_at(node, 0x22, 0x02, 0u);
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, rejected_at);
    TEST_ASSERT_TRUE_MESSAGE(dev.watch_at[1] < rejected_at + 100u, "not tried again at once");
    TEST_ASSERT_EQUAL_UINT32(0u, dev.supervised);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_iv_stats(zw_net_iv(&H.net))->timeouts);
    TEST_ASSERT_EQUAL_UINT(1u, dev.m.config_n);
    /* without Application Status in its list, done when delivered: the Set once, as before */
    static fake_zdev_t mute;
    static const uint8_t PLAIN[] = {0x5E, 0x86, 0x72, 0x85, 0x70, 0x71};
    plain_device(&mute, 0x7E14u, true, PLAIN, sizeof PLAIN);
    mute.m.app_status_cc = 0x70u;
    mute.m.app_status_cmd = ZW_CMD_APPLICATION_REJECTED_REQUEST;
    mute.m.app_status_n = 1u;
    const uint16_t other = h_include(&mute);
    expect_ready(other, 30000u);
    TEST_ASSERT_EQUAL_UINT(1u, fake_zdev_asked(&mute, 0x70, 0x04));
}

/* ZW-011, command-classes.md §4.25 with network.md §7: Application Status answering a device's own
 * check-in. Rejected Request ends it at once, not taken - well before its 1 s plus the round trip -
 * the record, the stored cache and supervision keep the old interval, and it is not sent again at
 * the next wake-up. Busy (try again later): the Set goes again 1 s later in the same wake-up, No
 * More Information held until it is done, and is taken. */
static void test_application_status_answers_check_in(void)
{
    h_device(&dev, 0x7E15u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false;
    dev.m.app_status_cc = 0x84u;
    dev.m.app_status_cmd = ZW_CMD_APPLICATION_REJECTED_REQUEST;
    dev.m.app_status_n = 1u;
    unsigned sets = fake_zdev_asked(&dev, 0x84, 0x04);
    const unsigned stores = H.stores;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 43200u));
    h_run_for(MIN_MS);
    dev.watch_cc = 0x84u;
    dev.watch_cmd = 0x04u;
    dev.watch_n = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04));
    const zw_net_event_t *e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->took);
    TEST_ASSERT_EQUAL_UINT32(43200u, e->check_in_s);
    TEST_ASSERT_EQUAL_UINT(1u, dev.watch_n);
    const uint64_t rejected_at = rx_at(node, 0x22, 0x02, 0u);
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, rejected_at);
    TEST_ASSERT_EQUAL_UINT64(rejected_at, heard_at(e)); /* ended by it, at once */
    TEST_ASSERT_TRUE_MESSAGE(heard_at(e) < dev.watch_at[0] + 1000u, "waited for the Report");
    TEST_ASSERT_EQUAL_UINT32(3600u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(3600u, zw_health_interval(zw_net_health(&H.net), node));
    TEST_ASSERT_EQUAL_UINT32(3600u, dev.m.wake_interval); /* it did not carry it out */
    TEST_ASSERT_EQUAL_UINT(stores, H.stores);
    h_run_for(MIN_MS);
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(sets + 1u, fake_zdev_asked(&dev, 0x84, 0x04)); /* not again */

    dev.m.app_status_cmd = ZW_CMD_APPLICATION_BUSY;
    dev.m.app_busy_status = ZW_APPLICATION_BUSY_TRY_LATER;
    dev.m.app_status_n = 1u;
    sets = fake_zdev_asked(&dev, 0x84, 0x04);
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED,
                          zw_net_set_check_in(&H.net, H.wire.now, true, node, 14400u));
    h_run_for(MIN_MS);
    dev.watch_n = 0u;
    const uint32_t nmi = dev.nmi_heard;
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(sets + 2u, fake_zdev_asked(&dev, 0x84, 0x04));
    TEST_ASSERT_EQUAL_UINT(2u, dev.watch_n);
    /* again 1 s after the panel heard Busy (the device's own S2 sends one frame at a time: its
     * Busy waited behind its Wake Up Notification) */
    const uint64_t busy_at = rx_at(node, 0x22, 0x01, 0u);
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, busy_at);
    TEST_ASSERT_TRUE(dev.watch_at[1] >= busy_at + ZW_IV_BUSY_MS);
    TEST_ASSERT_TRUE(dev.watch_at[1] < busy_at + ZW_IV_BUSY_MS + 200u);
    e = h_find(ZW_NET_EV_CHECK_IN, node);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->took);
    TEST_ASSERT_EQUAL_UINT(1u, h_count(ZW_NET_EV_CHECK_IN, node));
    TEST_ASSERT_EQUAL_UINT32(14400u, h_rec(node)->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(14400u, dev.m.wake_interval);
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, dev.nmi_heard);
}

/* ---- ZW-042: a sleeper's broadcast Wake Up Notification (serial-api.md §6.11) ---------------- */

static const uint8_t BC_WUN[2] = {0x84u, 0x07u};

/* What the module heard from the host: SendData requests (func 0x13), counted from `from`. */
static unsigned send_datas_since(size_t from)
{
    unsigned n = 0u;
    for (size_t i = from; i < H.zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&H.zac, i);
        if (h != NULL && h->type == ZW_TYPE_REQUEST && h->func == 0x13u) {
            n++;
        }
    }
    return n;
}

static uint64_t last_heard_of(uint16_t node)
{
    for (size_t i = 0; i < H.net.health.node_count; i++) {
        if (H.net.health.nodes[i].node_id == node) {
            return H.net.health.nodes[i].last_heard_ms;
        }
    }
    TEST_FAIL_MESSAGE("no health record");
    return 0u;
}

/* The device wakes with no wake-up destination: awake, nothing singlecast to the panel; then its
 * notification to everyone, as the module reports it (`status`: 0x04 broadcast, 0x08 multicast). */
static void wakes_and_announces(uint8_t status)
{
    const uint8_t keep = dev.m.wake_node;
    dev.m.wake_node = 0u;
    fake_zdev_wake(&dev, H.wire.now);
    dev.m.wake_node = keep;
    fake_zac93_from_node_as(&H.zac, H.wire.now, dev.node, status, BC_WUN, sizeof BC_WUN);
}

/* A door sensor, ready and asleep, with a Battery Get held for its next wake-up; `keyless`: added
 * without encryption ("Add it without encryption"), else with the S2 classes it asks for. */
static uint16_t held_sleeper(uint64_t seed, bool keyless)
{
    h_device(&dev, seed, true);
    dev.nif_fallback = keyless; /* without S2 it lists everything in the clear */
    H.cancel_at_grant = keyless;
    const uint16_t node = h_include(&dev);
    H.cancel_at_grant = false;
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_INT(keyless ? ZW_SEC_NONE : ZW_SEC_S2_ACCESS,
                          zw_s2_highest(zw_net_s2(&H.net), node));
    h_run_for(2000u);
    static const uint8_t BATTERY_GET[] = {0x80, 0x02};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, BATTERY_GET,
                                                sizeof BATTERY_GET, 0x42u));
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n); /* it waits for the device */
    TEST_ASSERT_FALSE(fake_zdev_awake(&dev, H.wire.now));
    return node;
}

static uint32_t wrong_level_of(uint16_t node)
{
    return zw_s2_node_stats(zw_net_s2(&H.net), node)->wrong_level;
}

/* ZW-042 (a), as Z-Wave JS: a sleeper added without encryption whose interview stopped before its
 * wake-up step (step 7: it has no wake-up destination yet) announces its wake-up by broadcast -
 * taken as a singlecast one: the interview goes on, with no button pressed, and the owner hears
 * it. */
static void test_broadcast_wake_up_resumes_interview(void)
{
    h_device(&dev, 0x5E91u, true);
    dev.m.mute_cc = 0x72u; /* its first attempt at Manufacturer Specific (step 5) unanswered */
    dev.m.mute_cmd = 0x04u;
    dev.m.mute_n = 3u;
    dev.m.auto_wake = false;
    dev.nif_fallback = true;
    H.cancel_at_grant = true;
    const uint16_t node = h_include(&dev);
    H.cancel_at_grant = false;
    h_run_for(20000u);
    TEST_ASSERT_EQUAL_UINT(3u, fake_zdev_asked(&dev, 0x72, 0x04));
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_WAKE_NEEDED, node));
    TEST_ASSERT_TRUE(zw_iv_running(zw_net_iv(&H.net), node));
    TEST_ASSERT_EQUAL_UINT8(0u, dev.m.wake_node); /* no destination: it broadcasts */
    h_run_for(60000u);
    TEST_ASSERT_EQUAL_UINT(3u, fake_zdev_asked(&dev, 0x72, 0x04)); /* nothing while it sleeps */
    const uint32_t dropped = zw_net_stats(&H.net)->not_singlecast;
    wakes_and_announces(0x04u);
    h_run_for(500u);
    TEST_ASSERT_EQUAL_UINT(4u, fake_zdev_asked(&dev, 0x72, 0x04)); /* its next step went */
    TEST_ASSERT_EQUAL_UINT32(dropped, zw_net_stats(&H.net)->not_singlecast);
    TEST_ASSERT_EQUAL_UINT(1u, h_rx_count(node, 0x84, 0x07)); /* the owner's, as singlecast */
    expect_ready(node, 30000u);
    TEST_ASSERT_EQUAL_UINT8(1u, dev.m.wake_node); /* step 7 made the panel its destination */
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_STALLED, node));
}

/* ZW-042 (b): a keyless sleeper's broadcast Wake Up Notification sends what was held for it, then
 * No More Information - as a singlecast one. An S2 sleeper's, in the clear, is discarded as a
 * wrong level: nothing held goes (ZW-009, Z-Wave JS isSecurityLevelTooLow). */
static void test_broadcast_wake_up_sends_held_commands(void)
{
    const uint16_t node = held_sleeper(0x5E92u, true);
    const uint32_t nmi = dev.nmi_heard;
    const unsigned gets = fake_zdev_asked(&dev, 0x80, 0x02); /* the interview's own */
    const uint32_t dropped = zw_net_stats(&H.net)->not_singlecast;
    wakes_and_announces(0x04u);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(1u, H.done_n);
    TEST_ASSERT_EQUAL_UINT32(0x42u, H.done_tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT(gets + 1u, fake_zdev_asked(&dev, 0x80, 0x02));
    TEST_ASSERT_EQUAL_UINT32(nmi + 1u, dev.nmi_heard);
    TEST_ASSERT_EQUAL_UINT32(dropped, zw_net_stats(&H.net)->not_singlecast);
    TEST_ASSERT_EQUAL_UINT(1u, h_rx_count(node, 0x84, 0x07));
}

/* ZW-042 (b), ZW-009 (Z-Wave JS Driver.ts isSecurityLevelTooLow): an S2 sleeper's Wake Up
 * Notification, broadcast in the clear, is discarded as a wrong level - nothing held goes, no No
 * More Information, the owner hears nothing. */
static void test_s2_broadcast_wake_up_discarded(void)
{
    const uint16_t node = held_sleeper(0x5E97u, false);
    const uint32_t nmi = dev.nmi_heard;
    const uint32_t wrong = wrong_level_of(node);
    const size_t heard = H.zac.heard;
    wakes_and_announces(0x04u);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(wrong + 1u, wrong_level_of(node));
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n);
    TEST_ASSERT_EQUAL_UINT(0u, send_datas_since(heard));
    TEST_ASSERT_EQUAL_UINT32(nmi, dev.nmi_heard);
    TEST_ASSERT_EQUAL_UINT(0u, h_rx_count(node, 0x84, 0x07));
}

/* ZW-042 (c): the same notification by broadcast from a node the panel does not know: dropped and
 * counted, nothing sent - not even to the sleeper with something held. */
static void test_broadcast_wake_up_unknown_node_dropped(void)
{
    (void)held_sleeper(0x5E93u, true);
    const uint32_t dropped = zw_net_stats(&H.net)->not_singlecast;
    const size_t heard = H.zac.heard;
    const size_t rx = H.rx_n;
    fake_zac93_from_node_as(&H.zac, H.wire.now, 9u, 0x04u, BC_WUN, sizeof BC_WUN);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(dropped + 1u, zw_net_stats(&H.net)->not_singlecast);
    TEST_ASSERT_EQUAL_UINT(0u, send_datas_since(heard));
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n);
    TEST_ASSERT_EQUAL_UINT(rx, H.rx_n);
}

/* ZW-042 (d), as Z-Wave JS (Driver.ts shouldDiscardCC, isSecurityLevelTooLow): a keyless node's
 * broadcast report - a Notification Report, a Basic Set - is processed like a singlecast one; a
 * broadcast Get (Wake Up Interval Get) is dropped and counted; an S2 node's plaintext broadcast
 * is discarded as a wrong level. ZW-011 (network.md §7): a report is no wake-up - the held command
 * stays - and a Get from the sleeper is one - it goes. */
static void test_broadcast_other_commands_dropped(void)
{
    const uint16_t node = held_sleeper(0x5E94u, true);
    static const uint8_t NOTIF[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    static const uint8_t BASIC_SET[] = {0x20, 0x01, 0xFF};
    static const uint8_t WAKE_UP_GET[] = {0x84, 0x05}; /* Wake Up Interval Get */
    const uint32_t dropped = zw_net_stats(&H.net)->not_singlecast;
    const size_t heard = H.zac.heard;
    const unsigned notifs = h_rx_count(node, 0x71, 0x05);
    const unsigned sets = h_rx_count(node, 0x20, 0x01);
    const unsigned gets = h_rx_count(node, 0x84, 0x05);
    fake_zac93_from_node_as(&H.zac, H.wire.now, node, 0x04u, NOTIF, sizeof NOTIF);
    fake_zac93_from_node_as(&H.zac, H.wire.now + 10u, node, 0x08u, BASIC_SET, sizeof BASIC_SET);
    fake_zac93_from_node_as(&H.zac, H.wire.now + 20u, node, 0x04u, WAKE_UP_GET,
                            sizeof WAKE_UP_GET);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(dropped + 1u, zw_net_stats(&H.net)->not_singlecast);
    TEST_ASSERT_EQUAL_UINT(notifs + 1u, h_rx_count(node, 0x71, 0x05));
    TEST_ASSERT_EQUAL_UINT(sets + 1u, h_rx_count(node, 0x20, 0x01));
    TEST_ASSERT_EQUAL_UINT(gets, h_rx_count(node, 0x84, 0x05));
    /* §7 (Z-Wave JS Node.ts: only a Wake Up Notification, a NIF or a Get from it marks a sleeper
     * awake): a report from a sleeper with something held is no wake-up - the held Battery Get
     * stays; a Get from it (singlecast) is one - it goes */
    TEST_ASSERT_EQUAL_UINT(0u, send_datas_since(heard));
    static const uint8_t SWITCH_GET[] = {0x25, 0x02}; /* Binary Switch Get */
    fake_zac93_from_node_as(&H.zac, H.wire.now, node, 0x00u, SWITCH_GET, sizeof SWITCH_GET);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT(1u, send_datas_since(heard));
    /* an S2 sleeper's plaintext broadcast report: a wrong level, nothing reaches anyone */
    h_clear_events();
    const uint16_t s2 = held_sleeper(0x5E98u, false);
    const uint32_t wrong = wrong_level_of(s2);
    const size_t rx = H.rx_n;
    fake_zac93_from_node_as(&H.zac, H.wire.now, s2, 0x04u, NOTIF, sizeof NOTIF);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(wrong + 1u, wrong_level_of(s2));
    TEST_ASSERT_EQUAL_UINT(rx, H.rx_n);
}

/* ZW-042 (e), ZW-009: an S2 sleeper's plaintext broadcast Wake Up Notification is discarded, so
 * it is no check-in: a sleeper reported unreachable stays so, its last-heard time does not move,
 * its interval is not read again, the zones hear nothing. Its singlecast one is the check-in. */
static void test_broadcast_wake_up_is_no_check_in(void)
{
    h_device(&dev, 0x5E95u, true);
    const uint16_t node = h_include(&dev);
    expect_ready(node, 30000u);
    h_run_for(2000u);
    dev.m.auto_wake = false; /* it stops checking in */
    const uint64_t t0 = H.wire.now;
    h_run(t0 + 3600000u + 600000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_UNREACHABLE, node));
    TEST_ASSERT_FALSE(zw_health_is_reachable(&H.net.health, node));
    h_clear_events();
    const uint64_t heard_at = last_heard_of(node);
    const size_t rx = H.rx_n;
    const unsigned rereads = fake_zdev_asked(&dev, 0x84, 0x05);
    const uint32_t nmi = dev.nmi_heard;
    wakes_and_announces(0x04u);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(nmi, dev.nmi_heard); /* discarded: no wake-up handling */
    TEST_ASSERT_EQUAL_UINT64(heard_at, last_heard_of(node));
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_REACHABLE, node));
    TEST_ASSERT_FALSE(zw_health_is_reachable(&H.net.health, node));
    TEST_ASSERT_EQUAL_UINT(rereads, fake_zdev_asked(&dev, 0x84, 0x05));
    TEST_ASSERT_EQUAL_UINT(rx, H.rx_n);
    /* its singlecast notification is the check-in, as before */
    fake_zdev_wake(&dev, H.wire.now);
    h_run_for(3000u);
    TEST_ASSERT_NOT_NULL(h_find(ZW_NET_EV_REACHABLE, node));
    TEST_ASSERT_TRUE(last_heard_of(node) > heard_at);
}

/* ZW-042 (f): a multicast Wake Up Notification from an S2 sleeper in the clear is a wrong level:
 * counted there, nothing held goes. */
static void test_multicast_wake_up_dropped(void)
{
    const uint16_t node = held_sleeper(0x5E96u, false);
    const uint32_t wrong = wrong_level_of(node);
    const size_t heard = H.zac.heard;
    wakes_and_announces(0x08u);
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(wrong + 1u, wrong_level_of(node));
    TEST_ASSERT_EQUAL_UINT(0u, send_datas_since(heard));
    TEST_ASSERT_EQUAL_UINT(0u, H.done_n);
    TEST_ASSERT_EQUAL_UINT(0u, h_rx_count(node, 0x84, 0x07));
}

/* ZW-009, ZW-042: what counts as hearing from a device, as Z-Wave JS: a frame the security layer
 * discards - S2 from a device added without it - is no check-in (Z-Wave JS discards before
 * handleRequest, where markAsAlive is); a frame it accepts is, and so is the device's ACK of the
 * panel's own send (Driver.ts: a successful transmit report updates lastSeen). */
static void test_heard_only_accepted_frames(void)
{
    h_device(&dev, 0x5E99u, false);
    fake_zdev_listening(&dev);
    dev.nif_fallback = true;
    H.cancel_at_grant = true;
    const uint16_t node = h_include(&dev);
    H.cancel_at_grant = false;
    expect_ready(node, 30000u);
    h_run_for(5000u);
    static const uint8_t ENCAP[] = {0x9F, 0x03, 0x01, 0x00, 0x20, 0x01, 0xFF,
                                    1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t t = last_heard_of(node);
    h_run_for(1000u);
    fake_zac93_from_node_as(&H.zac, H.wire.now, node, 0x00u, ENCAP, sizeof ENCAP);
    h_run_for(100u);
    TEST_ASSERT_EQUAL_UINT64(t, last_heard_of(node));
    static const uint8_t BASIC_SET[] = {0x20, 0x01, 0xFF};
    fake_zac93_from_node_as(&H.zac, H.wire.now, node, 0x00u, BASIC_SET, sizeof BASIC_SET);
    h_run_for(100u);
    TEST_ASSERT_TRUE(last_heard_of(node) > t);
    t = last_heard_of(node);
    h_run_for(1000u);
    static const uint8_t NOP[] = {0x00};
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_NONE, NOP,
                                                sizeof NOP, 0x43u));
    h_run_for(500u);
    TEST_ASSERT_TRUE(last_heard_of(node) > t);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_door_sensor_interview);
    RUN_TEST(test_interview_resumes_after_panel_restart);
    RUN_TEST(test_sleeping_interview_across_wakeups);
    RUN_TEST(test_lifeline_from_agi);
    RUN_TEST(test_lifeline_from_quirk_or_none);
    RUN_TEST(test_lifeline_multi_channel);
    RUN_TEST(test_lifeline_read_back);
    RUN_TEST(test_push_pull_probe);
    RUN_TEST(test_no_association_is_pull);
    RUN_TEST(test_alarm_sensor_types);
    RUN_TEST(test_alarm_sensor_silent_does_not_stall);
    RUN_TEST(test_alarm_sensor_bad_report_does_not_stall);
    RUN_TEST(test_alarm_types_asked_after_older_interview);
    RUN_TEST(test_alarm_types_silent_after_older_interview);
    RUN_TEST(test_alarm_types_asked_at_sleepers_wake_up);
    RUN_TEST(test_alarm_types_unreached_asked_again);
    RUN_TEST(test_alarm_sensor_unreached_in_interview);
    RUN_TEST(test_check_in_set_while_alarm_types_wait);
    RUN_TEST(test_alarm_types_missing_rule);
    RUN_TEST(test_question_timing_and_stall);
    RUN_TEST(test_reread_keeps_the_device_working);
    RUN_TEST(test_reread_stopped_list);
    RUN_TEST(test_reread_replaces_the_old_facts);
    RUN_TEST(test_reread_pool_and_sleepers);
    RUN_TEST(test_reread_waits_for_real_wake_up);
    RUN_TEST(test_reread_parked_scratch_reclaimed);
    RUN_TEST(test_reread_second_request_restarts);
    RUN_TEST(test_reread_nif_reaches_security);
    RUN_TEST(test_unasked_secure_report_changes_nothing);
    RUN_TEST(test_reread_unanswered_secure_list_reaches_security);
    RUN_TEST(test_reread_waking_sleeper_reclaims_parked);
    RUN_TEST(test_reread_sleeper_without_wake_up);
    RUN_TEST(test_reread_keeps_wake_interval_report);
    RUN_TEST(test_reread_sleeper_attempt_fails);
    RUN_TEST(test_reread_keeps_alarm_types);
    RUN_TEST(test_controller_not_usable);
    RUN_TEST(test_first_start_binds_keys);
    RUN_TEST(test_first_start_keys_agree);
    RUN_TEST(test_first_start_key_draw_fails_once);
    RUN_TEST(test_first_start_key_draws_fail);
    RUN_TEST(test_first_start_seed_fails);
    RUN_TEST(test_first_start_key_expansion_fails);
    RUN_TEST(test_failed_reset_binds_active_keys);
    RUN_TEST(test_stopped_first_start_stops_s2);
    RUN_TEST(test_reconcile_normal);
    RUN_TEST(test_generation_counter_never_wraps);
    RUN_TEST(test_reconcile_replaced);
    RUN_TEST(test_reconcile_foreign);
    RUN_TEST(test_cache_lost_classes_probed);
    RUN_TEST(test_cache_lost_s2_decrypted_before_probe);
    RUN_TEST(test_nif_never_removes_encrypted_ccs);
    RUN_TEST(test_cache_lost_learnt_class_asked_alone);
    RUN_TEST(test_cache_lost_probe_keeps_real_class);
    RUN_TEST(test_probe_skips_classes_it_lacks);
    RUN_TEST(test_probe_records_every_class_in_order);
    RUN_TEST(test_probe_secure_list_from_first_answer);
    RUN_TEST(test_class_learnt_after_single_probe_stored);
    RUN_TEST(test_nif_without_s2_keeps_learnt_class);
    RUN_TEST(test_probe_transmit_failure_records_nothing);
    RUN_TEST(test_single_class_query_three_times);
    RUN_TEST(test_known_node_silent_query_kept);
    RUN_TEST(test_node_list_read_again);
    RUN_TEST(test_exclusion_own_device);
    RUN_TEST(test_exclusion_foreign_and_refusals);
    RUN_TEST(test_remove_failed);
    RUN_TEST(test_device_reset_locally);
    RUN_TEST(test_device_reset_locally_never_failed);
    RUN_TEST(test_region_change);
    RUN_TEST(test_network_reset);
    RUN_TEST(test_radio_restart);
    RUN_TEST(test_restart_radio);
    RUN_TEST(test_restart_radio_not_answering);
    RUN_TEST(test_supervision_listening);
    RUN_TEST(test_interview_question_transmissions);
    RUN_TEST(test_transmissions_per_owner);
    RUN_TEST(test_supervision_sleeping_and_queue);
    RUN_TEST(test_queue_replaces_command_out);
    RUN_TEST(test_queue_keeps_end_points_apart);
    RUN_TEST(test_queue_keeps_undecoded_encapsulations);
    RUN_TEST(test_queue_cancelled_owner_resends);
    RUN_TEST(test_wake_up_interval_rules);
    RUN_TEST(test_check_in_lengthened_at_next_wake_up);
    RUN_TEST(test_check_in_last_choice_clamped_and_rounded);
    RUN_TEST(test_check_in_refusals);
    RUN_TEST(test_check_in_failures_change_nothing);
    RUN_TEST(test_check_in_follows_the_quirk);
    RUN_TEST(test_zwave_plus_without_association);
    RUN_TEST(test_version_zero_is_unsupported);
    RUN_TEST(test_endpoints_beyond_limit);
    RUN_TEST(test_interviews_two_at_a_time);
    RUN_TEST(test_sleeper_does_not_wait_its_turn);
    RUN_TEST(test_sleeper_attempt_waits_for_wake_up);
    RUN_TEST(test_module_restart_pauses_interview);
    RUN_TEST(test_supervision_fail_is_a_failed_try);
    RUN_TEST(test_supervision_working_completes_interview_step);
    RUN_TEST(test_supervision_working_takes_check_in);
    RUN_TEST(test_supervision_fail_still_fails_check_in);
    RUN_TEST(test_answer_from_another_end_point);
    RUN_TEST(test_failed_s2_bootstrap_stays_insecure_after_restart);
    RUN_TEST(test_version_zero_ignored_for_ccs_in_use);
    RUN_TEST(test_version_zero_supervision_unseen_is_unsupported);
    RUN_TEST(test_endpoint_traffic_of_s2_node_encrypted);
    RUN_TEST(test_supervision_report_not_held_back);
    RUN_TEST(test_other_controller_updates_ignored);
    RUN_TEST(test_split_association_report);
    RUN_TEST(test_split_multi_channel_association_report);
    RUN_TEST(test_failed_s2_bootstrap_node_info_asked_again);
    RUN_TEST(test_endpoint_only_notification);
    RUN_TEST(test_commands_to_unreachable_node_end_promptly);
    RUN_TEST(test_node_info_releases_reinterview);
    RUN_TEST(test_reinterview_at_wake_up);
    RUN_TEST(test_hold_awake);
    RUN_TEST(test_inclusion_not_disturbed_by_interview);
    RUN_TEST(test_push_without_agi);
    RUN_TEST(test_stale_supervision_session_not_confirmation);
    RUN_TEST(test_bridge_asks_end_points_at_their_class);
    RUN_TEST(test_application_busy_answers_quirk_set);
    RUN_TEST(test_application_rejected_request_without_supervision);
    RUN_TEST(test_application_status_answers_check_in);
    RUN_TEST(test_broadcast_wake_up_resumes_interview);
    RUN_TEST(test_broadcast_wake_up_sends_held_commands);
    RUN_TEST(test_s2_broadcast_wake_up_discarded);
    RUN_TEST(test_heard_only_accepted_frames);
    RUN_TEST(test_broadcast_wake_up_unknown_node_dropped);
    RUN_TEST(test_broadcast_other_commands_dropped);
    RUN_TEST(test_broadcast_wake_up_is_no_check_in);
    RUN_TEST(test_multicast_wake_up_dropped);
    return UNITY_END();
}
