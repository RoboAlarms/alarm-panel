/* The save check, auto_rule_check (docs/features/41 AUT-002 layer 2, AUT-015 the installer option,
 * AUT-016 the confirm, AUT-011 the maker; AUT-001 limits). */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "automation/automation.h"
#include "home_control/home_control.h"

void setUp(void) {}
void tearDown(void) {}

static auto_zone_t s_zones[ALARM_MAX_ZONES];

static auto_env_t env_of(bool arm_supported)
{
    memset(s_zones, 0, sizeof s_zones);
    s_zones[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    s_zones[1] = (auto_zone_t){ALARM_ZT_INTERIOR_FOLLOWER, 1, ALARM_DEVCLASS_MOTION};
    s_zones[2] = (auto_zone_t){ALARM_ZT_FIRE, 1, ALARM_DEVCLASS_SMOKE};
    s_zones[ALARM_MAX_ZONES - 1] = (auto_zone_t){ALARM_ZT_PERIMETER, 2, ALARM_DEVCLASS_WINDOW};
    auto_env_t e = {0x03u, arm_supported, s_zones, true, false};
    return e;
}

static void device(auto_action_t *a, uint8_t kind, uint8_t action, const char *ref)
{
    memset(a, 0, sizeof *a);
    a->kind = AUTO_ACT_DEVICE;
    a->source = DSRC_HA;
    a->dev_kind = kind;
    a->dev_action = action;
    a->value = -1;
    strcpy(a->ref, ref);
}

/* The porch light: zone 1 opens -> turn on light.porch, saved by user 1. Valid. */
static auto_rule_t porch(void)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.uid = 1;
    r.slot = 1;
    r.n = 1;
    r.made_by = 1;
    r.when.kind = AUTO_TRIG_ZONE_OPENED;
    r.when.zone = 1;
    device(&r.then[0], DEV_LIGHT, ACT_ON, "light.porch");
    return r;
}

/* 22:30 -> Arm Night on partition 1, 5 minutes, made by user 2. Valid when arming is supported. */
static auto_rule_t arm_night(void)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.uid = 4;
    r.n = 1;
    r.made_by = 2;
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 1350;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].value = -1;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5;
    return r;
}

typedef struct {
    const char *what;
    void (*edit)(auto_rule_t *r);
    bool arm;            /* start from arm_night() instead of porch() */
    bool arm_supported;
    auto_check_t want;
} check_case_t;

/* ---- the edits, one per case ---- */
static void no_edit(auto_rule_t *r) { (void)r; }
static void trig_none(auto_rule_t *r) { r->when.kind = AUTO_TRIG_NONE; }
static void trig_unknown(auto_rule_t *r) { r->when.kind = AUTO_TRIG_COUNT; }
static void n_zero(auto_rule_t *r) { r->n = 0; }
static void n_three(auto_rule_t *r) { r->n = 3; }
static void act_none(auto_rule_t *r) { r->then[0].kind = AUTO_ACT_NONE; }
static void act_output(auto_rule_t *r) { r->then[0].kind = 3; }
static void zone_zero(auto_rule_t *r) { r->when.zone = 0; }
static void ref_empty(auto_rule_t *r) { r->then[0].ref[0] = '\0'; }
static void ref_unterminated(auto_rule_t *r) { memset(r->then[0].ref, 'x', DEV_REF_LEN); }
static void ref_longest(auto_rule_t *r)
{
    memset(r->then[0].ref, 'x', DEV_REF_LEN - 1u);
    r->then[0].ref[DEV_REF_LEN - 1u] = '\0';
}
static void source_none(auto_rule_t *r) { r->then[0].source = DSRC_NONE; }
static void source_last(auto_rule_t *r) { r->then[0].source = AUTO_SOURCE_LAST; }
static void source_past_last(auto_rule_t *r) { r->then[0].source = (uint8_t)(AUTO_SOURCE_LAST + 1u); }
static void device_with_arm_level(auto_rule_t *r) { r->then[0].arm_level = ALARM_LEVEL_NIGHT; }
static void device_with_warning(auto_rule_t *r) { r->then[0].warn_min = 5; }
static void arm_with_dev_kind(auto_rule_t *r) { r->then[0].dev_kind = 200; }
static void arm_with_source(auto_rule_t *r) { r->then[0].source = DSRC_HA; }
static void arm_with_ref(auto_rule_t *r) { strcpy(r->then[0].ref, "lock.front"); }
static void armed_levels_ok(auto_rule_t *r)
{
    r->when.kind = AUTO_TRIG_ARMED;
    r->when.zone = 0;
    r->when.levels = (uint8_t)(ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY) | ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) |
                               ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT));
}
static void armed_levels_none_bit(auto_rule_t *r)
{
    armed_levels_ok(r);
    r->when.levels |= ALARM_LEVEL_BIT(ALARM_LEVEL_NONE);
}
static void armed_levels_high_bit(auto_rule_t *r)
{
    armed_levels_ok(r);
    r->when.levels = 0x10u;
}
static void arm_level_none(auto_rule_t *r) { r->then[0].arm_level = ALARM_LEVEL_NONE; }
static void arm_level_away(auto_rule_t *r) { r->then[0].arm_level = ALARM_LEVEL_AWAY; }
static void arm_level_stay(auto_rule_t *r) { r->then[0].arm_level = ALARM_LEVEL_STAY; }
static void arm_level_four(auto_rule_t *r) { r->then[0].arm_level = 4; }
static void warn_zero(auto_rule_t *r) { r->then[0].warn_min = 0; }
static void warn_one(auto_rule_t *r) { r->then[0].warn_min = 1; }
static void warn_fifteen(auto_rule_t *r) { r->then[0].warn_min = 15; }
static void warn_sixteen(auto_rule_t *r) { r->then[0].warn_min = 16; }
static void arm_bad_level_bad_partition(auto_rule_t *r)
{
    r->then[0].arm_level = 9;
    r->then[0].partition = 0;
}
static void script(auto_rule_t *r) { device(&r->then[0], DEV_SCRIPT, ACT_RUN, "script.x"); }
static void unlock(auto_rule_t *r) { device(&r->then[0], DEV_LOCK, ACT_UNLOCK, "lock.front"); }
static void unlock_confirmed(auto_rule_t *r)
{
    unlock(r);
    r->then[0].confirmed = 1;
}
static void off_after_on(auto_rule_t *r) { r->then[0].off_after_min = 10; }
static void off_after_day(auto_rule_t *r) { r->then[0].off_after_min = 1440; }
static void off_after_too_long(auto_rule_t *r) { r->then[0].off_after_min = 1441; }
static void off_after_on_off(auto_rule_t *r)
{
    r->then[0].dev_action = ACT_OFF;
    r->then[0].off_after_min = 10;
}
static void off_after_arm(auto_rule_t *r) { r->then[0].off_after_min = 10; }
static void zone_too_high(auto_rule_t *r) { r->when.zone = ALARM_MAX_ZONES + 1u; }
static void zone_last(auto_rule_t *r) { r->when.zone = ALARM_MAX_ZONES; }
static void zone_unused(auto_rule_t *r) { r->when.zone = 4; }
static void zone_fire(auto_rule_t *r) { r->when.zone = 3; }
static void zone_motion_opens(auto_rule_t *r) { r->when.zone = 2; }
static void zone_motion_closes(auto_rule_t *r)
{
    r->when.kind = AUTO_TRIG_ZONE_CLOSED;
    r->when.zone = 2;
}
static void zone_door_closes(auto_rule_t *r) { r->when.kind = AUTO_TRIG_ZONE_CLOSED; }
static void part_enabled(auto_rule_t *r) { r->when.partitions = 0x03u; }
static void part_disabled(auto_rule_t *r) { r->when.partitions = 0x04u; }
static void part_high_bit(auto_rule_t *r) { r->when.partitions = 0x80u; }
static void arm_part_two(auto_rule_t *r) { r->then[0].partition = 2; }
static void arm_part_zero(auto_rule_t *r) { r->then[0].partition = 0; }
static void arm_part_three(auto_rule_t *r) { r->then[0].partition = 3; }
static void arm_part_too_high(auto_rule_t *r) { r->then[0].partition = ALARM_MAX_PARTITIONS + 1u; }
static void time_last_minute(auto_rule_t *r) { r->when.minute = 1439; }
static void time_day_minute(auto_rule_t *r) { r->when.minute = 1440; }
static void days_all(auto_rule_t *r) { r->when.days = AUTO_DAYS_ALL; }
static void days_bit7(auto_rule_t *r) { r->when.days = 0x80u; }
static void days_bit7_on_zone(auto_rule_t *r) { r->when.days = 0x81u; }
static void win_wrap(auto_rule_t *r)
{
    r->when.win_from = 18u * 60u + 1u;
    r->when.win_to = 6u * 60u + 1u;
}
static void win_full_day(auto_rule_t *r)
{
    r->when.win_from = 1;
    r->when.win_to = 1440;
}
static void win_same(auto_rule_t *r) { r->when.win_from = r->when.win_to = 600; }
static void win_from_only(auto_rule_t *r) { r->when.win_from = 600; }
static void win_to_only(auto_rule_t *r) { r->when.win_to = 600; }
static void win_from_high(auto_rule_t *r)
{
    r->when.win_from = 1441;
    r->when.win_to = 600;
}
static void win_to_high(auto_rule_t *r)
{
    r->when.win_from = 600;
    r->when.win_to = 1441;
}
static void win_on_time(auto_rule_t *r) { win_wrap(r); }
static void arm_after_zone(auto_rule_t *r)
{
    r->when.kind = AUTO_TRIG_ZONE_OPENED;
    r->when.zone = 1;
    r->when.minute = 0;
}
static void arm_after_disarm(auto_rule_t *r)
{
    r->when.kind = AUTO_TRIG_DISARMED;
    r->when.minute = 0;
}
static void arm_with_light(auto_rule_t *r)
{
    r->n = 2;
    device(&r->then[1], DEV_LIGHT, ACT_OFF, "light.porch");
}
static void arm_twice(auto_rule_t *r)
{
    r->n = 2;
    r->then[1] = r->then[0];
}
static void maker_none(auto_rule_t *r) { r->made_by = 0; }
static void maker_last(auto_rule_t *r) { r->made_by = ALARM_MAX_USERS; }
static void maker_too_high(auto_rule_t *r) { r->made_by = ALARM_MAX_USERS + 1u; }
static void two_devices(auto_rule_t *r)
{
    r->n = 2;
    device(&r->then[1], DEV_LIGHT, ACT_ON, "light.hall");
}
static void same_device_twice(auto_rule_t *r)
{
    r->n = 2;
    device(&r->then[1], DEV_LIGHT, ACT_OFF, "light.porch");
}
static void same_ref_other_source(auto_rule_t *r)
{
    r->n = 2;
    device(&r->then[1], DEV_LIGHT, ACT_OFF, "light.porch");
    r->then[1].source = DSRC_ZIGBEE;
}
static void second_incomplete(auto_rule_t *r)
{
    r->n = 2;
    device(&r->then[1], DEV_LIGHT, ACT_OFF, "");
}
static void second_script(auto_rule_t *r)
{
    r->n = 2;
    device(&r->then[1], DEV_SCRIPT, ACT_RUN, "script.x");
}
/* the order: an incomplete rule is INCOMPLETE even when it is also forbidden */
static void incomplete_and_forbidden(auto_rule_t *r)
{
    script(r);
    r->when.zone = 0;
}
static void forbidden_and_bad_zone(auto_rule_t *r)
{
    script(r);
    r->when.zone = 3;
}
static void unlock_no_maker(auto_rule_t *r)
{
    unlock_confirmed(r);
    r->made_by = 0;
}
static void light_no_maker(auto_rule_t *r) { r->made_by = 0; }

/* AUT-001, AUT-002: every reason, with a passing and a failing case each, and the order. */
static const check_case_t CASES[] = {
    {"the porch light", no_edit, false, false, AUTO_OK},
    {"the arm rule, phase C", no_edit, true, true, AUTO_OK},
    /* INCOMPLETE */
    {"no trigger", trig_none, false, false, AUTO_ERR_INCOMPLETE},
    {"a trigger kind from later", trig_unknown, false, false, AUTO_ERR_INCOMPLETE},
    {"no action", n_zero, false, false, AUTO_ERR_INCOMPLETE},
    {"three actions", n_three, false, false, AUTO_ERR_INCOMPLETE},
    {"an action of kind none", act_none, false, false, AUTO_ERR_INCOMPLETE},
    {"an output action (reserved)", act_output, false, false, AUTO_ERR_INCOMPLETE},
    {"a zone trigger without a zone", zone_zero, false, false, AUTO_ERR_INCOMPLETE},
    {"a device without a ref", ref_empty, false, false, AUTO_ERR_INCOMPLETE},
    {"a ref without its NUL", ref_unterminated, false, false, AUTO_ERR_INCOMPLETE},
    {"the longest ref", ref_longest, false, false, AUTO_OK},
    {"a device without a source", source_none, false, false, AUTO_ERR_INCOMPLETE},
    {"a device of the newest source", source_last, false, false, AUTO_OK},
    {"a device of a source past the last", source_past_last, false, false, AUTO_ERR_INCOMPLETE},
    {"a device action with an arm level", device_with_arm_level, false, false, AUTO_ERR_INCOMPLETE},
    {"a device action with a warning", device_with_warning, false, false, AUTO_ERR_INCOMPLETE},
    {"an arm with a device kind", arm_with_dev_kind, true, true, AUTO_ERR_INCOMPLETE},
    {"an arm with a source", arm_with_source, true, true, AUTO_ERR_INCOMPLETE},
    {"an arm with a ref", arm_with_ref, true, true, AUTO_ERR_INCOMPLETE},
    {"armed Away, Stay or Night", armed_levels_ok, false, false, AUTO_OK},
    {"armed level None", armed_levels_none_bit, false, false, AUTO_ERR_INCOMPLETE},
    {"armed level from later", armed_levels_high_bit, false, false, AUTO_ERR_INCOMPLETE},
    {"arm level none", arm_level_none, true, true, AUTO_ERR_INCOMPLETE},
    {"arm Away", arm_level_away, true, true, AUTO_OK},
    {"arm Stay", arm_level_stay, true, true, AUTO_OK},
    {"arm level 4", arm_level_four, true, true, AUTO_ERR_INCOMPLETE},
    {"arm warning 0", warn_zero, true, true, AUTO_ERR_INCOMPLETE},
    {"arm warning 1", warn_one, true, true, AUTO_OK},
    {"arm warning 15", warn_fifteen, true, true, AUTO_OK},
    {"arm warning 16", warn_sixteen, true, true, AUTO_ERR_INCOMPLETE},
    {"a second action incomplete", second_incomplete, false, false, AUTO_ERR_INCOMPLETE},
    {"incomplete before forbidden", incomplete_and_forbidden, false, false, AUTO_ERR_INCOMPLETE},
    /* UNSUPPORTED: phase A, before the other Arm rules */
    {"arm in phase A", no_edit, true, false, AUTO_ERR_UNSUPPORTED},
    {"a broken arm in phase A", arm_bad_level_bad_partition, true, false, AUTO_ERR_UNSUPPORTED},
    {"a broken arm in phase C", arm_bad_level_bad_partition, true, true, AUTO_ERR_INCOMPLETE},
    /* NOT_ALLOWED */
    {"a script", script, false, false, AUTO_ERR_NOT_ALLOWED},
    {"a second action runs a script", second_script, false, false, AUTO_ERR_NOT_ALLOWED},
    {"an unlock after a door, confirmed", unlock_confirmed, false, false, AUTO_OK},
    {"forbidden before the zone", forbidden_and_bad_zone, false, false, AUTO_ERR_NOT_ALLOWED},
    {"on for 10 minutes", off_after_on, false, false, AUTO_OK},
    {"on for a day", off_after_day, false, false, AUTO_OK},
    {"on for a day and a minute", off_after_too_long, false, false, AUTO_ERR_NOT_ALLOWED},
    {"off after on an Off", off_after_on_off, false, false, AUTO_ERR_NOT_ALLOWED},
    {"off after on an Arm", off_after_arm, true, true, AUTO_ERR_NOT_ALLOWED},
    /* ZONE */
    {"zone past the last", zone_too_high, false, false, AUTO_ERR_ZONE},
    {"the last zone", zone_last, false, false, AUTO_OK},
    {"an unused zone", zone_unused, false, false, AUTO_ERR_ZONE},
    {"a fire zone", zone_fire, false, false, AUTO_ERR_ZONE},
    {"motion opens", zone_motion_opens, false, false, AUTO_OK},
    {"motion closes", zone_motion_closes, false, false, AUTO_ERR_ZONE},
    {"a door closes", zone_door_closes, false, false, AUTO_OK},
    /* PARTITION */
    {"enabled partitions", part_enabled, false, false, AUTO_OK},
    {"partition 3, not enabled", part_disabled, false, false, AUTO_ERR_PARTITION},
    {"partition 8", part_high_bit, false, false, AUTO_ERR_PARTITION},
    {"arm partition 2", arm_part_two, true, true, AUTO_OK},
    {"arm partition 0", arm_part_zero, true, true, AUTO_ERR_PARTITION},
    {"arm partition 3, not enabled", arm_part_three, true, true, AUTO_ERR_PARTITION},
    {"arm partition past the last", arm_part_too_high, true, true, AUTO_ERR_PARTITION},
    /* TIME */
    {"23:59", time_last_minute, true, true, AUTO_OK},
    {"24:00", time_day_minute, true, true, AUTO_ERR_TIME},
    {"every weekday bit", days_all, true, true, AUTO_OK},
    {"a day bit 7", days_bit7, true, true, AUTO_ERR_TIME},
    {"a day bit 7 on a zone trigger", days_bit7_on_zone, false, false, AUTO_ERR_TIME},
    /* WINDOW */
    {"18:00 to 06:00, past midnight", win_wrap, false, false, AUTO_OK},
    {"00:00 to 23:59", win_full_day, false, false, AUTO_OK},
    {"from == to", win_same, false, false, AUTO_ERR_WINDOW},
    {"a from without a to", win_from_only, false, false, AUTO_ERR_WINDOW},
    {"a to without a from", win_to_only, false, false, AUTO_ERR_WINDOW},
    {"from past the day", win_from_high, false, false, AUTO_ERR_WINDOW},
    {"to past the day", win_to_high, false, false, AUTO_ERR_WINDOW},
    {"a window on a time trigger", win_on_time, true, true, AUTO_ERR_WINDOW},
    /* any trigger may arm (D23); ARM_ALONE; MAKER */
    {"arm after a zone", arm_after_zone, true, true, AUTO_OK},
    {"arm after a disarm", arm_after_disarm, true, true, AUTO_OK},
    {"arm and a light", arm_with_light, true, true, AUTO_ERR_ARM_ALONE},
    {"arm twice", arm_twice, true, true, AUTO_ERR_ARM_ALONE},
    {"arm without a maker", maker_none, true, true, AUTO_ERR_MAKER},
    {"arm by the last user", maker_last, true, true, AUTO_OK},
    {"arm by a user past the last", maker_too_high, true, true, AUTO_ERR_MAKER},
    {"an unlock without a maker", unlock_no_maker, false, false, AUTO_ERR_MAKER},
    {"a light without a maker", light_no_maker, false, false, AUTO_OK},
    /* NEEDS_CONFIRM (the full table is test_needs_confirm_table) */
    {"an unlock after a door", unlock, false, false, AUTO_ERR_NEEDS_CONFIRM},
    /* SAME_DEVICE */
    {"two devices", two_devices, false, false, AUTO_OK},
    {"one device twice", same_device_twice, false, false, AUTO_ERR_SAME_DEVICE},
    {"one ref in two sources", same_ref_other_source, false, false, AUTO_OK},
};

/* AUT-001, AUT-002, AUT-007, AUT-011, AUT-016: the table above (the installer option on). */
static void test_every_reason_passes_and_fails(void)
{
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const check_case_t *c = &CASES[i];
        auto_rule_t r = c->arm ? arm_night() : porch();
        c->edit(&r);
        auto_env_t env = env_of(c->arm_supported);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->want, auto_rule_check(&r, &env), c->what);
    }
}

/* AUT-002: no rule or no panel is incomplete; no zones known refuses a zone trigger only. */
static void test_null_and_no_zones(void)
{
    auto_rule_t r = porch();
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(NULL, &env));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, NULL));
    env.zones = NULL;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_ZONE, auto_rule_check(&r, &env));
    armed_levels_ok(&r);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
}

/* What an automation may do (DESIGN.md section 3's table), written out here on its own. */
static bool table_allows(unsigned kind, unsigned action)
{
    switch (kind) {
    case DEV_LIGHT:
    case DEV_SWITCH:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_FAN:
        return action == ACT_ON || action == ACT_OFF;
    case DEV_LOCK:
        return action == ACT_LOCK || action == ACT_UNLOCK;
    case DEV_COVER:
        return action == ACT_CLOSE || action == ACT_OPEN || action == ACT_STOP;
    case DEV_VALVE:
        return action == ACT_CLOSE || action == ACT_OPEN;
    case DEV_CLIMATE:
        return action == ACT_SET_MODE; /* with "off" */
    case DEV_SCENE:
        return action == ACT_RUN;
    default:
        return false;
    }
}

/* The less secure ones among them (D25). */
static bool table_less_secure(unsigned kind, unsigned action)
{
    return action == ACT_UNLOCK || (kind == DEV_COVER && (action == ACT_OPEN || action == ACT_STOP)) ||
           (kind == DEV_VALVE && action == ACT_OPEN);
}

/* AUT-002 (layer 2), AUT-015: every kind and action pair through the validator, confirmed, with
 * value -1 and mode "off" for a set-mode: allowed exactly where the design's table says; with the
 * installer option off the less secure ones are LESS_SECURE_OFF. */
static void test_every_pair_through_the_validator(void)
{
    unsigned allowed = 0;
    unsigned less_secure = 0;
    for (unsigned k = 0; k < DEV_KIND_COUNT; k++) {
        for (unsigned a = 0; a < ACT_COUNT; a++) {
            auto_rule_t r = porch();
            device(&r.then[0], (uint8_t)k, (uint8_t)a, "x.device");
            r.then[0].confirmed = 1;
            if (a == ACT_SET_MODE) {
                strcpy(r.then[0].mode, "off");
            }
            const bool want = table_allows(k, a);
            const bool ls = want && table_less_secure(k, a);
            TEST_ASSERT_EQUAL_MESSAGE(want, hc_auto_allowed((dev_kind_t)k, (dev_action_t)a, -1,
                                                            r.then[0].mode, true),
                                      "hc_auto_allowed and the table");
            if (want) { /* a pair never allowed (a cover's position) may also need a code */
                TEST_ASSERT_EQUAL_MESSAGE(ls, auto_action_less_secure(&r.then[0]),
                                          "auto_action_less_secure and the table");
            }
            char what[48];
            snprintf(what, sizeof what, "kind %u action %u", k, a);
            auto_env_t env = env_of(false);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want ? AUTO_OK : AUTO_ERR_NOT_ALLOWED,
                                          auto_rule_check(&r, &env), what);
            env.less_secure = false;
            TEST_ASSERT_EQUAL_INT_MESSAGE(!want ? AUTO_ERR_NOT_ALLOWED
                                                : ls ? AUTO_ERR_LESS_SECURE_OFF : AUTO_OK,
                                          auto_rule_check(&r, &env), what);
            allowed += want ? 1u : 0u;
            less_secure += ls ? 1u : 0u;
        }
    }
    TEST_ASSERT_EQUAL_UINT(17u, allowed);
    TEST_ASSERT_EQUAL_UINT(4u, less_secure);
}

/* 10:00 -> Disarm partition 1, made by user 2. Valid in phase C with the option on. */
static auto_rule_t disarm_at_ten(void)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.uid = 5;
    r.n = 1;
    r.made_by = 2;
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 600;
    r.then[0].kind = AUTO_ACT_DISARM;
    r.then[0].value = -1;
    r.then[0].partition = 1;
    return r;
}

/* AUT-011, AUT-015 (D23, D25): a disarm - alone or with one device action, one partition that is
 * enabled, a maker - only with the option on and only in phase C. */
static void test_disarm(void)
{
    auto_env_t env = env_of(true);
    auto_rule_t r = disarm_at_ten();
    TEST_ASSERT_TRUE(auto_action_less_secure(&r.then[0]));
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    /* the option off comes before phase A's refusal */
    env.less_secure = false;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &env));
    env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNSUPPORTED, auto_rule_check(&r, &env));
    env.less_secure = false;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &env));
    env = env_of(true);
    /* with one device action, either order */
    r.n = 2;
    device(&r.then[1], DEV_LOCK, ACT_UNLOCK, "lock.front");
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.then[1] = r.then[0];
    device(&r.then[0], DEV_LIGHT, ACT_ON, "light.hall");
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    /* with an arm: the arm must be alone */
    r = disarm_at_ten();
    r.n = 2;
    r.then[1] = arm_night().then[0];
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_ARM_ALONE, auto_rule_check(&r, &env));
    /* two disarms */
    r = disarm_at_ten();
    r.n = 2;
    r.then[1] = r.then[0];
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_SAME_DEVICE, auto_rule_check(&r, &env));
    r.then[1].partition = 2;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_SAME_DEVICE, auto_rule_check(&r, &env));
    /* the partition */
    r = disarm_at_ten();
    r.then[0].partition = 2;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r.then[0].partition = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_PARTITION, auto_rule_check(&r, &env));
    r.then[0].partition = 3;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_PARTITION, auto_rule_check(&r, &env));
    r.then[0].partition = ALARM_MAX_PARTITIONS + 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_PARTITION, auto_rule_check(&r, &env));
    /* the maker */
    r = disarm_at_ten();
    r.made_by = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_MAKER, auto_rule_check(&r, &env));
    r.made_by = ALARM_MAX_USERS + 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_MAKER, auto_rule_check(&r, &env));
    r.made_by = ALARM_MAX_USERS;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    /* stray fields of another kind */
    r = disarm_at_ten();
    r.then[0].arm_level = ALARM_LEVEL_AWAY;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    r = disarm_at_ten();
    r.then[0].warn_min = 5;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    r = disarm_at_ten();
    strcpy(r.then[0].ref, "lock.front");
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_INCOMPLETE, auto_rule_check(&r, &env));
    r = disarm_at_ten();
    r.then[0].off_after_min = 10;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
}

/* AUT-015 (D25): with the installer option off, a disarm, unlock, open or stop is refused at
 * save; a Lock, a Close and a light are not; a script stays NOT_ALLOWED (checked first). */
/* BL-187, USR-001: the installer never saves an Arm or a Disarm (an edit would make the installer
 * the person it acts as); device actions stay theirs to save */
static void test_installer_saves_no_alarm_action(void)
{
    auto_env_t env = env_of(true);
    env.by_installer = true;
    auto_rule_t r = disarm_at_ten();
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    r.off = true; /* turned off, it would still act as the installer once turned back on */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    r = arm_night();
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    /* a disarm with a device action, the device first */
    r = disarm_at_ten();
    r.n = 2;
    r.then[1] = r.then[0];
    device(&r.then[0], DEV_LIGHT, ACT_ON, "light.hall");
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    /* the same rules by the system master */
    env.by_installer = false;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    r = arm_night();
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    /* a device action alone is the installer's to save */
    env.by_installer = true;
    r = porch();
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
}

static void test_less_secure_off(void)
{
    auto_env_t env = env_of(false);
    env.less_secure = false;
    static const struct {
        uint8_t kind, action;
        auto_check_t want;
    } T[] = {
        {DEV_LOCK, ACT_UNLOCK, AUTO_ERR_LESS_SECURE_OFF},
        {DEV_COVER, ACT_OPEN, AUTO_ERR_LESS_SECURE_OFF},
        {DEV_COVER, ACT_STOP, AUTO_ERR_LESS_SECURE_OFF},
        {DEV_VALVE, ACT_OPEN, AUTO_ERR_LESS_SECURE_OFF},
        {DEV_LOCK, ACT_LOCK, AUTO_OK},
        {DEV_COVER, ACT_CLOSE, AUTO_OK},
        {DEV_VALVE, ACT_CLOSE, AUTO_OK},
        {DEV_LIGHT, ACT_TOGGLE, AUTO_OK},
        {DEV_SCRIPT, ACT_RUN, AUTO_ERR_NOT_ALLOWED},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        auto_rule_t r = porch();
        r.when.kind = AUTO_TRIG_TIME;
        r.when.zone = 0;
        r.when.minute = 600;
        device(&r.then[0], T[i].kind, T[i].action, "x.device");
        r.then[0].confirmed = 1;
        TEST_ASSERT_EQUAL_INT(T[i].want, auto_rule_check(&r, &env));
    }
    /* the option off comes before the confirm and the maker */
    auto_rule_t r = porch();
    unlock(&r);
    r.made_by = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &env));
    /* a second action that unlocks */
    r = porch();
    r.n = 2;
    device(&r.then[1], DEV_LOCK, ACT_UNLOCK, "lock.front");
    r.then[1].confirmed = 1;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &env));
    env.less_secure = true;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
}

/* AUT-016 (D24): every trigger x action - a less secure action (unlock, a cover opened or
 * stopped, a valve opened, a disarm) after a door, window, motion or alarm trigger needs its
 * confirm; a time of day, armed or disarmed trigger needs none; a Lock or a light never does. */
static void test_needs_confirm_table(void)
{
    static const uint8_t TRIGS[] = {AUTO_TRIG_ZONE_OPENED, AUTO_TRIG_ZONE_CLOSED, AUTO_TRIG_ALARM,
                                    AUTO_TRIG_FIRE, AUTO_TRIG_TIME, AUTO_TRIG_ARMED,
                                    AUTO_TRIG_DISARMED};
    static const struct {
        uint8_t act_kind, kind, action;
        bool less_secure;
    } ACTS[] = {
        {AUTO_ACT_DEVICE, DEV_LOCK, ACT_UNLOCK, true},
        {AUTO_ACT_DEVICE, DEV_COVER, ACT_OPEN, true},
        {AUTO_ACT_DEVICE, DEV_COVER, ACT_STOP, true},
        {AUTO_ACT_DEVICE, DEV_VALVE, ACT_OPEN, true},
        {AUTO_ACT_DISARM, 0, 0, true},
        {AUTO_ACT_DEVICE, DEV_LOCK, ACT_LOCK, false},
        {AUTO_ACT_DEVICE, DEV_COVER, ACT_CLOSE, false},
        {AUTO_ACT_DEVICE, DEV_LIGHT, ACT_ON, false},
    };
    auto_env_t env = env_of(true);
    for (size_t t = 0; t < sizeof TRIGS; t++) {
        for (size_t a = 0; a < sizeof ACTS / sizeof ACTS[0]; a++) {
            auto_rule_t r = porch();
            r.when.kind = TRIGS[t];
            r.when.zone = (TRIGS[t] == AUTO_TRIG_ZONE_OPENED || TRIGS[t] == AUTO_TRIG_ZONE_CLOSED)
                              ? 1u
                              : 0u;
            r.when.minute = TRIGS[t] == AUTO_TRIG_TIME ? 600u : 0u;
            if (ACTS[a].act_kind == AUTO_ACT_DISARM) {
                r.then[0] = disarm_at_ten().then[0];
            } else {
                device(&r.then[0], ACTS[a].kind, ACTS[a].action, "x.device");
            }
            const bool door = TRIGS[t] == AUTO_TRIG_ZONE_OPENED ||
                              TRIGS[t] == AUTO_TRIG_ZONE_CLOSED || TRIGS[t] == AUTO_TRIG_ALARM ||
                              TRIGS[t] == AUTO_TRIG_FIRE;
            /* decided 2026-10-05: an unlock, an open or a stop after an armed trigger needs
             * the confirm too; a disarm after one is refused (it would undo the arming) */
            const bool armed = TRIGS[t] == AUTO_TRIG_ARMED;
            const bool is_disarm = ACTS[a].act_kind == AUTO_ACT_DISARM;
            const bool confirm = ACTS[a].less_secure && (door || (armed && !is_disarm));
            /* D22: a disarm after an alarm or a fire is never allowed, confirmed or not */
            const bool silences = is_disarm && (TRIGS[t] == AUTO_TRIG_ALARM ||
                                                TRIGS[t] == AUTO_TRIG_FIRE);
            const auto_check_t after = silences                ? AUTO_ERR_NOT_ALLOWED
                                       : armed && is_disarm ? AUTO_ERR_UNDOES_ARMING
                                                            : AUTO_OK;
            char what[48];
            snprintf(what, sizeof what, "trigger %u action %u", (unsigned)TRIGS[t], (unsigned)a);
            /* the exported rule says the same as the validator (AUT-016) */
            TEST_ASSERT_EQUAL_MESSAGE(confirm,
                                      auto_action_needs_trigger_confirm(TRIGS[t], &r.then[0]),
                                      what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(confirm && !silences ? AUTO_ERR_NEEDS_CONFIRM : after,
                                          auto_rule_check(&r, &env), what);
            r.then[0].confirmed = 1;
            TEST_ASSERT_EQUAL_INT_MESSAGE(after, auto_rule_check(&r, &env), what);
        }
    }
    /* the second action's own confirm: the first one's doesn't cover it */
    auto_rule_t r = porch();
    r.n = 2;
    device(&r.then[0], DEV_COVER, ACT_OPEN, "cover.garage");
    r.then[0].confirmed = 1;
    device(&r.then[1], DEV_LOCK, ACT_UNLOCK, "lock.front");
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NEEDS_CONFIRM, auto_rule_check(&r, &env));
    r.then[1].confirmed = 1;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
}

/* AUT-002: a value or a mode word outside the table is refused. */
static void test_values_and_modes(void)
{
    auto_env_t env = env_of(false);
    auto_rule_t r = porch();
    r.then[0].value = 0; /* brightness 0, not "on" */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    r.then[0].value = 50;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    r = porch();
    strcpy(r.then[0].mode, "off"); /* a mode word with Turn on */
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    device(&r.then[0], DEV_CLIMATE, ACT_SET_MODE, "climate.hallway");
    strcpy(r.then[0].mode, "heat");
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    r.then[0].mode[0] = '\0';
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    strcpy(r.then[0].mode, "off");
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    device(&r.then[0], DEV_COVER, ACT_SET_LEVEL, "cover.garage");
    r.then[0].value = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
}

/* AUT-002 (decisions D8, D9): a scene, or a switch's Turn on or Toggle, only when the person
 * confirmed it; a switch's Turn off and a light's Turn on or Toggle need no confirmation. */
static void test_confirmed_scene_and_switch_on(void)
{
    auto_env_t env = env_of(false);
    static const struct {
        uint8_t kind, action;
        bool needs;
    } T[] = {
        {DEV_SCENE, ACT_RUN, true},
        {DEV_SWITCH, ACT_ON, true},
        {DEV_SWITCH, ACT_TOGGLE, true},
        {DEV_SWITCH, ACT_OFF, false},
        {DEV_LIGHT, ACT_ON, false},
        {DEV_LIGHT, ACT_TOGGLE, false},
        {DEV_LOCK, ACT_LOCK, false},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        auto_rule_t r = porch();
        device(&r.then[0], T[i].kind, T[i].action, "x.device");
        TEST_ASSERT_EQUAL(T[i].needs, auto_action_needs_confirm(T[i].kind, T[i].action));
        TEST_ASSERT_EQUAL_INT(T[i].needs ? AUTO_ERR_NOT_ALLOWED : AUTO_OK,
                              auto_rule_check(&r, &env));
        r.then[0].confirmed = 1;
        TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    }
}

/* AUT-002 (D8, D9) and AUT-016 (D24): the two exported confirm rules, each true and false case,
 * and auto_rule_check agreeing with both over every device pair an automation may send. */
static void test_confirm_rules_agree_with_the_validator(void)
{
    /* D8, D9: exactly a scene's Run and a switch's Turn on and Toggle */
    for (int k = DEV_NONE; k <= DEV_KIND_COUNT; k++) {
        for (int a = ACT_NONE; a <= ACT_COUNT; a++) {
            const bool want = (k == DEV_SCENE && a == ACT_RUN) ||
                              (k == DEV_SWITCH && (a == ACT_ON || a == ACT_TOGGLE));
            TEST_ASSERT_EQUAL(want, auto_action_needs_confirm((uint8_t)k, (uint8_t)a));
        }
    }
    /* D24: no trigger, a trigger past the last and no action never need one */
    auto_action_t unlock;
    device(&unlock, DEV_LOCK, ACT_UNLOCK, "lock.front");
    TEST_ASSERT_TRUE(auto_action_needs_trigger_confirm(AUTO_TRIG_ZONE_OPENED, &unlock));
    TEST_ASSERT_FALSE(auto_action_needs_trigger_confirm(AUTO_TRIG_NONE, &unlock));
    TEST_ASSERT_FALSE(auto_action_needs_trigger_confirm(AUTO_TRIG_COUNT, &unlock));
    TEST_ASSERT_FALSE(auto_action_needs_trigger_confirm(AUTO_TRIG_ZONE_OPENED, NULL));

    /* every allowed pair, after a door and after a time of day, unconfirmed then confirmed */
    auto_env_t env = env_of(false);
    static const char *const modes[] = {"", "off"};
    for (int k = DEV_NONE; k < DEV_KIND_COUNT; k++) {
        for (int a = ACT_NONE; a < ACT_COUNT; a++) {
            for (size_t m = 0; m < 2; m++) {
                if (!hc_auto_allowed((dev_kind_t)k, (dev_action_t)a, -1, modes[m], true)) {
                    continue;
                }
                for (int door = 0; door <= 1; door++) {
                    auto_rule_t r = porch();
                    if (!door) {
                        r.when.kind = AUTO_TRIG_TIME;
                        r.when.zone = 0;
                        r.when.minute = 600;
                    }
                    device(&r.then[0], (uint8_t)k, (uint8_t)a, "x.device");
                    strcpy(r.then[0].mode, modes[m]);
                    const bool pick = auto_action_needs_confirm((uint8_t)k, (uint8_t)a);
                    const bool trig = auto_action_needs_trigger_confirm(r.when.kind, &r.then[0]);
                    char what[48];
                    snprintf(what, sizeof what, "kind %d action %d door %d", k, a, door);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(pick   ? AUTO_ERR_NOT_ALLOWED
                                                  : trig ? AUTO_ERR_NEEDS_CONFIRM
                                                         : AUTO_OK,
                                                  auto_rule_check(&r, &env), what);
                    r.then[0].confirmed = 1;
                    TEST_ASSERT_EQUAL_INT_MESSAGE(AUTO_OK, auto_rule_check(&r, &env), what);
                }
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_reason_passes_and_fails);
    RUN_TEST(test_null_and_no_zones);
    RUN_TEST(test_every_pair_through_the_validator);
    RUN_TEST(test_disarm);
    RUN_TEST(test_installer_saves_no_alarm_action);
    RUN_TEST(test_less_secure_off);
    RUN_TEST(test_needs_confirm_table);
    RUN_TEST(test_values_and_modes);
    RUN_TEST(test_confirmed_scene_and_switch_on);
    RUN_TEST(test_confirm_rules_agree_with_the_validator);
    return UNITY_END();
}
