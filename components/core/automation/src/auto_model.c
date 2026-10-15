/* The automation model: which zones may start one, and the validator (docs/features/41 AUT-002,
 * AUT-004). */
#include "automation/automation.h"

#include <string.h>

#include "home_control/home_control.h"

bool auto_zone_usable(uint8_t zone_type, uint8_t dev_class, bool for_close)
{
    /* Types listed one by one: a type added to the engine later starts nothing until it is
     * added here on purpose. 24-hour aux sounds at the keypad and is never silent (water,
     * temperature); silent types and buttons never start an automation (AUT-003, AUT-004). */
    switch (zone_type) {
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
    case ALARM_ZT_PERIMETER:
    case ALARM_ZT_INTERIOR_FOLLOWER:
    case ALARM_ZT_DAY_NIGHT:
    case ALARM_ZT_24H_AUX:
    case ALARM_ZT_INTERIOR_DELAY:
    case ALARM_ZT_MONITOR:
    case ALARM_ZT_NO_RESPONSE:
        break;
    default:
        return false;
    }
    switch (dev_class) {
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
    case ALARM_DEVCLASS_GARAGE_DOOR:
        return true;
    case ALARM_DEVCLASS_GENERIC:
    case ALARM_DEVCLASS_MOTION:
    case ALARM_DEVCLASS_GLASS_BREAK:
    case ALARM_DEVCLASS_WATER:
    case ALARM_DEVCLASS_TEMPERATURE:
    case ALARM_DEVCLASS_VIBRATION:
        return !for_close;
    default: /* smoke, heat, CO, gas, panic button, keyfob, keyswitch and anything newer */
        return false;
    }
}

/* The length of a string held in a buffer of cap bytes; cap when it has no NUL (unterminated). */
static size_t bounded_len(const char *s, size_t cap)
{
    size_t i = 0;
    while (i < cap && s[i] != '\0') {
        i++;
    }
    return i;
}

static bool arm_level_ok(uint8_t level)
{
    return level == ALARM_LEVEL_AWAY || level == ALARM_LEVEL_STAY || level == ALARM_LEVEL_NIGHT;
}

static bool alarm_action(uint8_t kind)
{
    return kind == AUTO_ACT_ARM || kind == AUTO_ACT_DISARM;
}

/* INCOMPLETE for the parts every action needs (the Arm fields come after UNSUPPORTED). */
static bool action_incomplete(const auto_action_t *a)
{
    /* No stray fields of the other kind: a record this firmware saves must load the same, not as
     * NEWER (a device kind past the last on an Arm action would). */
    if (a->bypass_open != 0u && (a->kind != AUTO_ACT_ARM || a->bypass_open != 1u)) {
        return true; /* D29: "Bypass open zones" is an Arm action's, and a yes or no */
    }
    if (a->kind == AUTO_ACT_DEVICE) {
        size_t ref = bounded_len(a->ref, sizeof a->ref);
        return ref == 0u || ref == sizeof a->ref || a->source == DSRC_NONE ||
               a->source > AUTO_SOURCE_LAST ||
               bounded_len(a->mode, sizeof a->mode) == sizeof a->mode ||
               bounded_len(a->name, sizeof a->name) == sizeof a->name ||
               a->partition != 0u || a->arm_level != 0u || a->warn_min != 0u;
    }
    if (a->kind == AUTO_ACT_ARM) {
        return a->source != DSRC_NONE || a->dev_kind != DEV_NONE || a->dev_action != ACT_NONE ||
               a->confirmed != 0u || a->ref[0] != '\0' || a->mode[0] != '\0';
    }
    if (a->kind == AUTO_ACT_DISARM) {
        /* the partition and, after a door trigger, the confirm (D24): nothing else */
        return a->source != DSRC_NONE || a->dev_kind != DEV_NONE || a->dev_action != ACT_NONE ||
               a->ref[0] != '\0' || a->mode[0] != '\0' || a->arm_level != 0u ||
               a->warn_min != 0u;
    }
    return true; /* NONE, 3 (an output, reserved) and anything newer */
}

bool auto_action_less_secure(const auto_action_t *a)
{
    if (a == NULL) {
        return false;
    }
    if (a->kind == AUTO_ACT_DISARM) {
        return true;
    }
    return a->kind == AUTO_ACT_DEVICE &&
           hc_auto_less_secure((dev_kind_t)a->dev_kind, (dev_action_t)a->dev_action, a->value);
}

/* NOT_ALLOWED: what no automation may do, whatever the installer option says (AUT-002). */
static bool action_not_allowed(const auto_action_t *a)
{
    if (a->off_after_min != 0u &&
        (a->kind != AUTO_ACT_DEVICE || a->dev_action != ACT_ON ||
         a->off_after_min > AUTO_MINUTES_PER_DAY)) {
        return true;
    }
    if (a->kind != AUTO_ACT_DEVICE) {
        return false;
    }
    if (!hc_auto_allowed((dev_kind_t)a->dev_kind, (dev_action_t)a->dev_action, a->value,
                         a->mode, true)) {
        return true;
    }
    /* D8, D9: a scene, or a switch's Turn on or Toggle, only once the person confirmed it */
    return auto_action_needs_confirm(a->dev_kind, a->dev_action) && a->confirmed == 0u;
}

bool auto_action_needs_confirm(uint8_t dev_kind, uint8_t dev_action)
{
    /* D8, D9 (AUT-002): what the panel can't see inside - a scene may unlock or open, a switch
     * may front a gate opener - so the person attests once when picking it. */
    return (dev_kind == DEV_SCENE && dev_action == ACT_RUN) ||
           (dev_kind == DEV_SWITCH && (dev_action == ACT_ON || dev_action == ACT_TOGGLE));
}

bool auto_action_needs_trigger_confirm(uint8_t trig_kind, const auto_action_t *a)
{
    /* D24 (AUT-016): a door, window, motion or alarm trigger - someone outside may cause it -
     * starting an unlock, an open, a stop or a disarm. */
    const bool outside = trig_kind == AUTO_TRIG_ZONE_OPENED || trig_kind == AUTO_TRIG_ZONE_CLOSED ||
                         trig_kind == AUTO_TRIG_ALARM || trig_kind == AUTO_TRIG_FIRE;
    if (outside) {
        return auto_action_less_secure(a);
    }
    /* Decided 2026-10-05: an unlock, an open or a stop after an armed trigger too (a disarm
     * after one is refused, or of another partition needs none: it follows a code). */
    return trig_kind == AUTO_TRIG_ARMED && a != NULL && a->kind == AUTO_ACT_DEVICE &&
           auto_action_less_secure(a);
}

static bool same_action_sentence(const auto_action_t *a, const auto_action_t *b)
{
    return a->kind == b->kind && a->source == b->source && a->dev_kind == b->dev_kind &&
           a->dev_action == b->dev_action && a->partition == b->partition &&
           a->arm_level == b->arm_level && a->warn_min == b->warn_min &&
           a->bypass_open == b->bypass_open && a->value == b->value &&
           a->off_after_min == b->off_after_min &&
           strncmp(a->mode, b->mode, sizeof a->mode) == 0 &&
           strncmp(a->ref, b->ref, sizeof a->ref) == 0 &&
           strncmp(a->name, b->name, sizeof a->name) == 0;
}

bool auto_rule_sentence_changed(const auto_rule_t *was, const auto_rule_t *now)
{
    if (was == NULL || now == NULL) {
        return true;
    }
    const auto_trigger_t *a = &was->when;
    const auto_trigger_t *b = &now->when;
    if (a->kind != b->kind || a->partitions != b->partitions || a->levels != b->levels ||
        a->days != b->days || a->zone != b->zone || a->minute != b->minute ||
        a->win_from != b->win_from || a->win_to != b->win_to || was->n != now->n) {
        return true;
    }
    for (unsigned i = 0; i < was->n && i < AUTO_ACTIONS; i++) {
        if (!same_action_sentence(&was->then[i], &now->then[i])) {
            return true;
        }
    }
    return false;
}

auto_check_t auto_rule_check(const auto_rule_t *r, const auto_env_t *env)
{
    if (r == NULL || env == NULL) {
        return AUTO_ERR_INCOMPLETE;
    }
    const auto_trigger_t *w = &r->when;
    bool zone_trigger = w->kind == AUTO_TRIG_ZONE_OPENED || w->kind == AUTO_TRIG_ZONE_CLOSED;
    const uint8_t arm_levels = (uint8_t)(ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY) |
                                         ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) |
                                         ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT));

    /* INCOMPLETE */
    if (w->kind == AUTO_TRIG_NONE || w->kind >= AUTO_TRIG_COUNT || r->n == 0u ||
        r->n > AUTO_ACTIONS) {
        return AUTO_ERR_INCOMPLETE;
    }
    if (zone_trigger && w->zone == 0u) {
        return AUTO_ERR_INCOMPLETE;
    }
    if (w->kind == AUTO_TRIG_ARMED && (w->levels & (uint8_t)~arm_levels) != 0u) {
        return AUTO_ERR_INCOMPLETE;
    }
    bool any_arm = false;
    bool any_alarm = false;
    bool any_less_secure = false;
    unsigned alarm_actions = 0u;
    for (unsigned i = 0; i < r->n; i++) {
        const auto_action_t *a = &r->then[i];
        if (action_incomplete(a)) {
            return AUTO_ERR_INCOMPLETE;
        }
        any_arm = any_arm || a->kind == AUTO_ACT_ARM;
        if (alarm_action(a->kind)) {
            any_alarm = true;
            alarm_actions++;
        }
        any_less_secure = any_less_secure || auto_action_less_secure(a) || a->bypass_open != 0u;
    }

    /* NOT_ALLOWED (layer 2): never, whatever the installer option */
    for (unsigned i = 0; i < r->n; i++) {
        if (action_not_allowed(&r->then[i])) {
            return AUTO_ERR_NOT_ALLOWED;
        }
    }

    /* NOT_ALLOWED (BL-187, USR-001): the installer never arms or disarms, and a save makes the
     * saver the person an alarm action acts as (D23): an installer's save of one would arm as
     * the installer, or quietly stop a master's automation from arming */
    if (any_alarm && env->by_installer) {
        return AUTO_ERR_NOT_ALLOWED;
    }

    /* LESS_SECURE_OFF (D25, AUT-015): a disarm, unlock, open or stop while the option is off */
    if (any_less_secure && !env->less_secure) {
        return AUTO_ERR_LESS_SECURE_OFF;
    }

    /* UNSUPPORTED: arming and disarming in phase A, before the other alarm action rules */
    if (any_alarm && !env->arm_supported) {
        return AUTO_ERR_UNSUPPORTED;
    }
    for (unsigned i = 0; i < r->n; i++) {
        const auto_action_t *a = &r->then[i];
        if (a->kind == AUTO_ACT_ARM &&
            (!arm_level_ok(a->arm_level) || a->warn_min < 1u || a->warn_min > 15u)) {
            return AUTO_ERR_INCOMPLETE;
        }
    }

    /* ARM_ALONE: arming is the automation's only action; SAME_DEVICE: one alarm action at most */
    if (any_arm && r->n != 1u) {
        return AUTO_ERR_ARM_ALONE;
    }
    if (alarm_actions > 1u) {
        return AUTO_ERR_SAME_DEVICE;
    }

    /* ZONE */
    if (zone_trigger) {
        if (w->zone > ALARM_MAX_ZONES || env->zones == NULL) {
            return AUTO_ERR_ZONE;
        }
        const auto_zone_t *z = &env->zones[w->zone - 1u];
        if (z->type == 0u ||
            !auto_zone_usable(z->type, z->dev_class, w->kind == AUTO_TRIG_ZONE_CLOSED)) {
            return AUTO_ERR_ZONE;
        }
    }

    /* PARTITION */
    if ((w->partitions & (uint8_t)~env->partitions_enabled) != 0u) {
        return AUTO_ERR_PARTITION;
    }
    for (unsigned i = 0; i < r->n; i++) {
        const auto_action_t *a = &r->then[i];
        if (!alarm_action(a->kind)) {
            continue;
        }
        if (a->partition == 0u || a->partition > ALARM_MAX_PARTITIONS ||
            (env->partitions_enabled & (1u << (a->partition - 1u))) == 0u) {
            return AUTO_ERR_PARTITION;
        }
    }

    /* NOT_ALLOWED (D22, the no-silence limit): a Disarm after an alarm or a fire would silence
     * it; whatever the option, and whichever partition (both editors never offer it) */
    if (w->kind == AUTO_TRIG_ALARM || w->kind == AUTO_TRIG_FIRE) {
        for (unsigned i = 0; i < r->n; i++) {
            if (r->then[i].kind == AUTO_ACT_DISARM) {
                return AUTO_ERR_NOT_ALLOWED;
            }
        }
    }

    /* UNDOES_ARMING (AUT-016, DESIGN.md "Disarming" 4): a Disarm after an armed trigger of its
     * own partition, or of any partition (mask 0), could only undo the arming */
    if (w->kind == AUTO_TRIG_ARMED) {
        for (unsigned i = 0; i < r->n; i++) {
            const auto_action_t *a = &r->then[i];
            if (a->kind == AUTO_ACT_DISARM &&
                (w->partitions == 0u || (w->partitions & (1u << (a->partition - 1u))) != 0u)) {
                return AUTO_ERR_UNDOES_ARMING;
            }
        }
    }

    /* TIME */
    if ((w->kind == AUTO_TRIG_TIME && w->minute >= AUTO_MINUTES_PER_DAY) ||
        (w->days & (uint8_t)~AUTO_DAYS_ALL) != 0u) {
        return AUTO_ERR_TIME;
    }

    /* WINDOW: minute + 1 each, wrapping past midnight; from == to would be every minute or none */
    if (w->win_from != 0u || w->win_to != 0u) {
        if (w->kind == AUTO_TRIG_TIME || w->win_from == 0u || w->win_to == 0u ||
            w->win_from > AUTO_MINUTES_PER_DAY || w->win_to > AUTO_MINUTES_PER_DAY ||
            w->win_from == w->win_to) {
            return AUTO_ERR_WINDOW;
        }
    }

    /* MAKER (AUT-011, AUT-015): an alarm or less secure action acts as the person who saved it */
    if ((any_alarm || any_less_secure) && (r->made_by == 0u || r->made_by > ALARM_MAX_USERS)) {
        return AUTO_ERR_MAKER;
    }

    /* NEEDS_CONFIRM (D24, AUT-016): a less secure action after a trigger someone outside may
     * cause, only with that action's confirm */
    for (unsigned i = 0; i < r->n; i++) {
        if (auto_action_needs_trigger_confirm(w->kind, &r->then[i]) &&
            r->then[i].confirmed == 0u) {
            return AUTO_ERR_NEEDS_CONFIRM;
        }
    }

    /* SAME_DEVICE (AUT-007) */
    if (r->n == 2u && r->then[0].kind == AUTO_ACT_DEVICE && r->then[1].kind == AUTO_ACT_DEVICE &&
        r->then[0].source == r->then[1].source &&
        strncmp(r->then[0].ref, r->then[1].ref, DEV_REF_LEN) == 0) {
        return AUTO_ERR_SAME_DEVICE;
    }
    return AUTO_OK;
}
