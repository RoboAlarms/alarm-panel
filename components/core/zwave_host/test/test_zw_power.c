/*
 * test_zw_power.c - the radio's transmit power (docs/features/36 network.md §12 "Power", phase 3;
 * serial-api.md §6.7; ZW-025): read from the module (the 16-bit form when it has it, the Long
 * Range maximum too), changed only as a network operation, never above the cap the owner keeps
 * (the power the radio came with), its calibration sent back unchanged, in effect after the
 * start-up's soft reset - and read again then.
 */
#include <string.h>

#include "net_harness.h"
#include "unity.h"

void setUp(void)
{
    h_init();
}
void tearDown(void) {}

static void offer(uint8_t sub)
{
    H.zac.id.setup_supported[sub / 8u] = (uint8_t)(H.zac.id.setup_supported[sub / 8u] | 1u << (sub % 8u));
}

/* The module lists the power sub-commands (16-bit ones when `sixteen`); the panel starts again. */
static void power_module(bool sixteen)
{
    offer(sixteen ? ZW_SETUP_GET_POWERLEVEL_16BIT : ZW_SETUP_GET_POWERLEVEL);
    offer(sixteen ? ZW_SETUP_SET_POWERLEVEL_16BIT : ZW_SETUP_SET_POWERLEVEL);
    offer(ZW_SETUP_GET_LONG_RANGE_MAXIMUM_TX_POWER);
    offer(ZW_SETUP_SET_LONG_RANGE_MAXIMUM_TX_POWER);
    h_boot();
    h_run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));
}

static const zw_net_event_t *power_event(zw_net_power_ev_t what)
{
    for (size_t i = H.ev_n; i-- > 0u;) {
        if (H.ev[i].kind == ZW_NET_EV_POWER && H.ev[i].power_ev == what) {
            return &H.ev[i];
        }
    }
    return NULL;
}

/* ZW-025: a read gives the module's power, calibration and Long Range maximum; nothing is read
 * at start-up unasked */
static void test_read(void)
{
    H.zac.power_dbm10 = H.zac.next_power_dbm10 = 120;
    H.zac.power_cal_dbm10 = -15;
    power_module(true);
    TEST_ASSERT_FALSE(zw_net_power(&H.net)->known);
    h_clear_events();
    TEST_ASSERT_TRUE(zw_net_power_read(&H.net, H.wire.now));
    TEST_ASSERT_FALSE(zw_net_power_read(&H.net, H.wire.now)); /* one read at a time */
    h_run_for(1000u);
    const zw_net_event_t *ev = power_event(ZW_NET_POWER_READ);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_TRUE(ev->power.known && ev->power.sixteen_bit && ev->power.lr_known);
    TEST_ASSERT_EQUAL_INT16(120, ev->power.dbm10);
    TEST_ASSERT_EQUAL_INT16(-15, ev->power.cal_dbm10);
    TEST_ASSERT_EQUAL_INT16(140, ev->power.lr_dbm10);
    /* a module without the power sub-commands: nothing to read */
    h_init();
    TEST_ASSERT_FALSE(zw_net_power_read(&H.net, H.wire.now));
}

/* ZW-025: never above the cap, nothing before a read, only values the module takes; a network
 * operation like the others */
static void test_refusals(void)
{
    power_module(true);
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_set_power(&H.net, H.wire.now, true, false, -20, 0));
    TEST_ASSERT_TRUE(zw_net_power_read(&H.net, H.wire.now));
    h_run_for(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_NET_NOT_ALLOWED, zw_net_set_power(&H.net, H.wire.now, false, false, -20, 0));
    TEST_ASSERT_EQUAL_INT(ZW_NET_TOO_HIGH, zw_net_set_power(&H.net, H.wire.now, true, false, 10, 0));
    TEST_ASSERT_EQUAL_INT(ZW_NET_UNCHANGED, zw_net_set_power(&H.net, H.wire.now, true, false, 0, 0));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_set_power(&H.net, H.wire.now, true, false, -110, 0));
    TEST_ASSERT_EQUAL_INT(ZW_NET_INVALID, zw_net_set_power(&H.net, H.wire.now, true, true, 120, 140));
    TEST_ASSERT_EQUAL_INT(ZW_NET_TOO_HIGH, zw_net_set_power(&H.net, H.wire.now, true, true, 200, 140));
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.power_sets);
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_power(&H.net, H.wire.now, true, false, -20, 0));
    TEST_ASSERT_EQUAL_INT(ZW_NET_BUSY, zw_net_set_power(&H.net, H.wire.now, true, false, -30, 0));
    TEST_ASSERT_EQUAL_INT(ZW_NET_BUSY, zw_net_restart(&H.net, H.wire.now, true));
}

/* ZW-025: a lower power, its calibration unchanged, in effect after the soft reset of the
 * start-up that follows, and read again then; the Long Range maximum the same way */
static void test_lower_it(void)
{
    power_module(true);
    TEST_ASSERT_TRUE(zw_net_power_read(&H.net, H.wire.now));
    h_run_for(1000u);
    const uint32_t restarts = H.zac.restarts;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_power(&H.net, H.wire.now, true, false, -30, 0));
    h_run_for(8000u);
    TEST_ASSERT_NOT_NULL(power_event(ZW_NET_POWER_SET_TOOK));
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.power_sets);
    TEST_ASSERT_GREATER_THAN_UINT32(restarts, H.zac.restarts); /* the soft reset applied it */
    const zw_net_event_t *ev = power_event(ZW_NET_POWER_READ);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_INT16(-30, ev->power.dbm10);
    TEST_ASSERT_EQUAL_INT16(33, ev->power.cal_dbm10); /* sent back as read */
    TEST_ASSERT_EQUAL_INT16(33, H.zac.power_cal_dbm10);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&H.net)));

    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_power(&H.net, H.wire.now, true, true, 100, 140));
    h_run_for(8000u);
    ev = power_event(ZW_NET_POWER_READ);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_INT16(100, ev->power.lr_dbm10);
    TEST_ASSERT_EQUAL_INT16(-30, ev->power.dbm10);
}

/* ZW-025: the 8-bit forms when the module has only those; a refused Set changes nothing and
 * restarts nothing */
static void test_eight_bit_and_refused(void)
{
    power_module(false);
    TEST_ASSERT_TRUE(zw_net_power_read(&H.net, H.wire.now));
    h_run_for(1000u);
    TEST_ASSERT_FALSE(zw_net_power(&H.net)->sixteen_bit);
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_power(&H.net, H.wire.now, true, false, -10, 0));
    h_run_for(8000u);
    TEST_ASSERT_EQUAL_INT16(-10, zw_net_power(&H.net)->dbm10);
    const uint32_t restarts = H.zac.restarts;
    H.zac.fault.refuse_setup = ZW_SETUP_SET_POWERLEVEL;
    h_clear_events();
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_set_power(&H.net, H.wire.now, true, false, -20, 0));
    h_run_for(8000u);
    TEST_ASSERT_NOT_NULL(power_event(ZW_NET_POWER_SET_REFUSED));
    TEST_ASSERT_NULL(power_event(ZW_NET_POWER_SET_TOOK));
    TEST_ASSERT_EQUAL_UINT32(restarts, H.zac.restarts);
    TEST_ASSERT_EQUAL_INT16(-10, zw_net_power(&H.net)->dbm10);
    TEST_ASSERT_EQUAL_INT(ZW_NET_STARTED, zw_net_restart(&H.net, H.wire.now, true)); /* free again */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_read);
    RUN_TEST(test_refusals);
    RUN_TEST(test_lower_it);
    RUN_TEST(test_eight_bit_and_refused);
    return UNITY_END();
}
