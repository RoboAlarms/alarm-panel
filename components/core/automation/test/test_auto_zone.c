/* Which zones may start an automation (docs/features/41 AUT-004). */
#include "unity.h"

#include "automation/automation.h"

void setUp(void) {}
void tearDown(void) {}

/* AUT-004: doors, windows and garage doors open and close; motion and water sensors only open. */
static void test_doors_open_and_close_sensors_only_open(void)
{
    TEST_ASSERT_TRUE(auto_zone_usable(ALARM_ZT_ENTRY_EXIT_1, ALARM_DEVCLASS_DOOR, false));
    TEST_ASSERT_TRUE(auto_zone_usable(ALARM_ZT_ENTRY_EXIT_1, ALARM_DEVCLASS_DOOR, true));
    TEST_ASSERT_TRUE(auto_zone_usable(ALARM_ZT_PERIMETER, ALARM_DEVCLASS_WINDOW, true));
    TEST_ASSERT_TRUE(auto_zone_usable(ALARM_ZT_PERIMETER, ALARM_DEVCLASS_GARAGE_DOOR, true));
    TEST_ASSERT_TRUE(auto_zone_usable(ALARM_ZT_INTERIOR_FOLLOWER, ALARM_DEVCLASS_MOTION, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_INTERIOR_FOLLOWER, ALARM_DEVCLASS_MOTION, true));
    TEST_ASSERT_TRUE(auto_zone_usable(ALARM_ZT_24H_AUX, ALARM_DEVCLASS_WATER, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_24H_AUX, ALARM_DEVCLASS_WATER, true));
}

/* AUT-003, AUT-004: silent zones, fire, CO, buttons and keyswitches never start one. */
static void test_silent_fire_and_buttons_never(void)
{
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_24H_SILENT, ALARM_DEVCLASS_DOOR, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_SILENT_BURGLARY, ALARM_DEVCLASS_DOOR, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_24H_AUDIBLE, ALARM_DEVCLASS_PANIC_BUTTON, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_FIRE, ALARM_DEVCLASS_SMOKE, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_CO, ALARM_DEVCLASS_CO, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_ARM_AWAY, ALARM_DEVCLASS_KEYFOB, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_KEYSWITCH, ALARM_DEVCLASS_KEYSWITCH, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_NOT_USED, ALARM_DEVCLASS_DOOR, false));
    /* a usable type with a class that never starts one */
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_PERIMETER, ALARM_DEVCLASS_GAS, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_PERIMETER, ALARM_DEVCLASS_HEAT, false));
}

/* AUT-004: a type or class this firmware doesn't list starts nothing. */
static void test_unknown_type_or_class_never(void)
{
    TEST_ASSERT_FALSE(auto_zone_usable(200, ALARM_DEVCLASS_DOOR, false));
    TEST_ASSERT_FALSE(auto_zone_usable(ALARM_ZT_PERIMETER, 200, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_doors_open_and_close_sensors_only_open);
    RUN_TEST(test_silent_fire_and_buttons_never);
    RUN_TEST(test_unknown_type_or_class_never);
    return UNITY_END();
}
