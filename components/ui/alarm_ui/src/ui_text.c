/*
 * ui_text.c - icons and tones for what the alarm engine reports, and the words that are only
 * ever on this screen. The event sentences themselves live in components/core/notify (docs/features/26),
 * so the Activity list and a push notification to a phone can never drift apart.
 */
#include "ui_text.h"

#include <stdio.h>
#include <string.h>

#include "notify/notify_words.h"
#include "ui_internal.h"

/* ---- portable words -> this screen -------------------------------------------------------
 * The event sentences live in components/core/notify so that the Activity list and a push
 * notification to a phone read the same words (docs/features/26). Only the colour and the glyph are
 * ours, so they come back as ids and these two tables map them. */

static lv_color_t tone_of(notify_accent_t a)
{
    switch (a) {
    case NOTIFY_ACCENT_GREEN: return ui_tone_green();
    case NOTIFY_ACCENT_AMBER: return ui_tone_amber();
    case NOTIFY_ACCENT_BLUE: return ui_tone_blue();
    case NOTIFY_ACCENT_VIOLET: return ui_tone_violet();
    case NOTIFY_ACCENT_ROSE: return ui_tone_rose();
    case NOTIFY_ACCENT_ORANGE: return ui_tone_orange();
    case NOTIFY_ACCENT_RED: return ui_tone_red();
    case NOTIFY_ACCENT_GRAY:
    default: return ui_tone_gray();
    }
}

static const char *glyph_of(notify_icon_t i)
{
    static const char *const table[NOTIFY_ICON_COUNT] = {
        [NOTIFY_ICON_INFO] = UI_ICON_INFO,
        [NOTIFY_ICON_ACTIVITY] = UI_ICON_ACTIVITY,
        [NOTIFY_ICON_BATTERY_WARNING] = UI_ICON_BATTERY_WARNING,
        [NOTIFY_ICON_BELL] = UI_ICON_BELL,
        [NOTIFY_ICON_BELL_OFF] = UI_ICON_BELL_OFF,
        [NOTIFY_ICON_CHECK] = UI_ICON_CHECK,
        [NOTIFY_ICON_CIRCLE_CHECK] = UI_ICON_CIRCLE_CHECK,
        [NOTIFY_ICON_CIRCLE_SLASH] = UI_ICON_CIRCLE_SLASH,
        [NOTIFY_ICON_CLOCK] = UI_ICON_CLOCK,
        [NOTIFY_ICON_CLOUD_FOG] = UI_ICON_CLOUD_FOG,
        [NOTIFY_ICON_CLOUD_OFF] = UI_ICON_CLOUD_OFF,
        [NOTIFY_ICON_CPU] = UI_ICON_CPU,
        [NOTIFY_ICON_DOOR_OPEN] = UI_ICON_DOOR_OPEN,
        [NOTIFY_ICON_FLAME] = UI_ICON_FLAME,
        [NOTIFY_ICON_FOOTPRINTS] = UI_ICON_FOOTPRINTS,
        [NOTIFY_ICON_HARD_DRIVE] = UI_ICON_HARD_DRIVE,
        [NOTIFY_ICON_HEART_PULSE] = UI_ICON_HEART_PULSE,
        [NOTIFY_ICON_HOUSE] = UI_ICON_HOUSE,
        [NOTIFY_ICON_KEY] = UI_ICON_KEY,
        [NOTIFY_ICON_LOCK] = UI_ICON_LOCK,
        [NOTIFY_ICON_LOCK_OPEN] = UI_ICON_LOCK_OPEN,
        [NOTIFY_ICON_RADIO_TOWER] = UI_ICON_RADIO_TOWER,
        [NOTIFY_ICON_REFRESH] = UI_ICON_REFRESH,
        [NOTIFY_ICON_ROTATE_CCW] = UI_ICON_ROTATE_CCW,
        [NOTIFY_ICON_SETTINGS] = UI_ICON_SETTINGS,
        [NOTIFY_ICON_SHIELD] = UI_ICON_SHIELD,
        [NOTIFY_ICON_SHIELD_ALERT] = UI_ICON_SHIELD_ALERT,
        [NOTIFY_ICON_SHIELD_X] = UI_ICON_SHIELD_X,
        [NOTIFY_ICON_SIREN] = UI_ICON_SIREN,
        [NOTIFY_ICON_TRIANGLE_ALERT] = UI_ICON_TRIANGLE_ALERT,
        [NOTIFY_ICON_USER] = UI_ICON_USER,
        [NOTIFY_ICON_WIFI_OFF] = UI_ICON_WIFI_OFF,
        [NOTIFY_ICON_WRENCH] = UI_ICON_WRENCH,
        [NOTIFY_ICON_ZAP] = UI_ICON_ZAP,
    };
    return ((unsigned)i < NOTIFY_ICON_COUNT && table[i] != NULL) ? table[i] : UI_ICON_INFO;
}

static void fill_line(const notify_words_t *w, ui_line_t *out)
{
    out->glyph = glyph_of((notify_icon_t)w->icon);
    out->tone = tone_of((notify_accent_t)w->accent);
    i18n_utf8_copy(out->title, sizeof(out->title), w->title);
    i18n_utf8_copy(out->detail, sizeof(out->detail), w->body);
}

lv_color_t ui_tone_green(void)
{
    return COLOR_GREEN;
}

lv_color_t ui_tone_amber(void)
{
    return COLOR_AMBER;
}

lv_color_t ui_tone_blue(void)
{
    return COLOR_BLUE;
}

lv_color_t ui_tone_violet(void)
{
    return COLOR_VIOLET;
}

lv_color_t ui_tone_rose(void)
{
    return COLOR_ROSE;
}

lv_color_t ui_tone_orange(void)
{
    return COLOR_ORANGE;
}

lv_color_t ui_tone_red(void)
{
    return COLOR_RED;
}

lv_color_t ui_tone_gray(void)
{
    return COLOR_TEXT_2;
}

lv_color_t ui_tone_for_level(alarm_arm_level_t level)
{
    return tone_of(notify_accent_for_level(level));
}

const char *ui_zone_name(const alarm_config_t *cfg, uint16_t zone, char *buf, size_t len)
{
    return notify_zone_name(S.catalog, cfg, zone, buf, len);
}

const char *ui_user_name(const alarm_config_t *cfg, uint8_t user, char *buf, size_t len)
{
    return notify_user_name(S.catalog, cfg, user, buf, len);
}

static alarm_device_class_t zone_class(const alarm_config_t *cfg, uint16_t zone)
{
    if (cfg == NULL || zone < 1u || zone > ALARM_MAX_ZONES) {
        return ALARM_DEVCLASS_GENERIC;
    }
    return (alarm_device_class_t)cfg->zones[zone - 1u].device_class;
}

static const char *class_glyph(alarm_device_class_t cls, bool active)
{
    switch (cls) {
    case ALARM_DEVCLASS_DOOR: return active ? UI_ICON_DOOR_OPEN : UI_ICON_DOOR_CLOSED;
    case ALARM_DEVCLASS_WINDOW: return UI_ICON_APP_WINDOW;
    case ALARM_DEVCLASS_GARAGE_DOOR: return UI_ICON_WAREHOUSE;
    case ALARM_DEVCLASS_MOTION: return UI_ICON_FOOTPRINTS;
    case ALARM_DEVCLASS_GLASS_BREAK: return UI_ICON_WAVES;
    case ALARM_DEVCLASS_SMOKE: return UI_ICON_ALARM_SMOKE;
    case ALARM_DEVCLASS_HEAT: return UI_ICON_FLAME;
    case ALARM_DEVCLASS_CO:
    case ALARM_DEVCLASS_GAS: return UI_ICON_CLOUD_FOG;
    case ALARM_DEVCLASS_WATER: return UI_ICON_DROPLETS;
    case ALARM_DEVCLASS_TEMPERATURE: return UI_ICON_THERMOMETER;
    case ALARM_DEVCLASS_VIBRATION: return UI_ICON_ACTIVITY;
    case ALARM_DEVCLASS_PANIC_BUTTON: return UI_ICON_SIREN;
    case ALARM_DEVCLASS_KEYFOB: return UI_ICON_KEY_ROUND;
    case ALARM_DEVCLASS_KEYSWITCH: return UI_ICON_KEY;
    default: return UI_ICON_CIRCLE_DOT;
    }
}

void ui_zone_state(const alarm_config_t *cfg, uint16_t zone, const alarm_zone_status_t *zs,
                   ui_line_t *out)
{
    ui_zone_name(cfg, zone, out->title, sizeof(out->title));
    out->glyph = class_glyph(zone_class(cfg, zone), (zs->flags & ALARM_ZS_FAULT) != 0u);
    out->tone = tone_of(notify_zone_state(S.catalog, cfg, zone, zs, out->detail, sizeof(out->detail)));
}

const char *ui_cause_title(alarm_cause_t cause)
{
    return notify_cause_title(S.catalog, cause);
}

alarm_cause_t ui_top_cause(uint16_t memory)
{
    static const alarm_cause_t order[] = {
        ALARM_CAUSE_FIRE,          ALARM_CAUSE_PANIC_FIRE,   ALARM_CAUSE_CO,
        ALARM_CAUSE_PANIC_POLICE,  ALARM_CAUSE_PANIC_MEDICAL, ALARM_CAUSE_BURGLARY,
        ALARM_CAUSE_EXIT_ERROR,    ALARM_CAUSE_TAMPER,       ALARM_CAUSE_24H_AUDIBLE,
        ALARM_CAUSE_24H_AUX,       ALARM_CAUSE_SILENT_BURGLARY, ALARM_CAUSE_24H_SILENT,
        ALARM_CAUSE_PANIC_SILENT,
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (memory & (1u << (unsigned)order[i])) {
            return order[i];
        }
    }
    return ALARM_CAUSE_NONE;
}

bool ui_event_line(const alarm_config_t *cfg, const alarm_event_t *ev, bool show_silent,
                   ui_line_t *out)
{
    notify_words_t w;
    if (!notify_words(S.catalog, cfg, ev, show_silent, &w)) {
        return false;
    }
    fill_line(&w, out);
    return true;
}

/* The event log's filters and a notification's categories are the same buckets, so the two can
 * never disagree about what counts as a trouble. ui_settings.c compares this against a bound
 * that comes from the XML filter, so the numbering is load-bearing. */
_Static_assert((int)UI_EVKIND_ALARM == (int)NOTIFY_CAT_ALARM &&
                   (int)UI_EVKIND_TROUBLE == (int)NOTIFY_CAT_TROUBLE &&
                   (int)UI_EVKIND_ARMING == (int)NOTIFY_CAT_ARMING &&
                   (int)UI_EVKIND_BYPASS == (int)NOTIFY_CAT_BYPASS &&
                   (int)UI_EVKIND_SYSTEM == (int)NOTIFY_CAT_SYSTEM,
               "ui_event_kind_t and notify_cat_t must be numbered alike");

ui_event_kind_t ui_event_kind(alarm_event_type_t type)
{
    return (ui_event_kind_t)notify_category(type);
}

const char *ui_authority_name(alarm_authority_t a)
{
    return notify_authority_name(S.catalog, a);
}

const char *ui_authority_detail(alarm_authority_t a)
{
    /* TR: One line on what a user level can do (the user's page, the level picker). */
    switch (a) {
    case ALARM_AUTH_INSTALLER:
        return TR("Programs the panel, manages users; disarms only what it armed");
    case ALARM_AUTH_MASTER: return TR("Everything, including managing users");
    case ALARM_AUTH_PARTITION_MASTER: return TR("Everything, and manages users of its area");
    case ALARM_AUTH_STANDARD: return TR("Arms, disarms, bypasses, chime");
    case ALARM_AUTH_ARM_ONLY: return TR("Arms only, can't disarm");
    case ALARM_AUTH_GUEST: return TR("Disarms only what this code armed");
    case ALARM_AUTH_DURESS: return TR("Disarms and silently calls for help");
    default: return "";
    }
}

lv_color_t ui_authority_tone(alarm_authority_t a)
{
    switch (a) {
    case ALARM_AUTH_INSTALLER: return ui_tone_amber();
    case ALARM_AUTH_MASTER:
    case ALARM_AUTH_PARTITION_MASTER: return ui_tone_violet();
    case ALARM_AUTH_DURESS: return ui_tone_red();
    case ALARM_AUTH_GUEST:
    case ALARM_AUTH_ARM_ONLY: return ui_tone_gray();
    default: return ui_tone_blue();
    }
}

/* The table holds the English source: a reader translates name with TR_C("zone type", ...) and
 * detail with TR(...) (ui_zone_type_name() does the name). */
const ui_zone_type_t UI_ZONE_TYPES[] = {
    {ALARM_ZT_ENTRY_EXIT_1, TR_NOOP_C("zone type", "Entry/exit 1"), TR_NOOP("The door you come in by: starts entry time 1")},
    {ALARM_ZT_ENTRY_EXIT_2, TR_NOOP_C("zone type", "Entry/exit 2"), TR_NOOP("A second door, like the garage: entry time 2")},
    {ALARM_ZT_PERIMETER, TR_NOOP_C("zone type", "Perimeter"), TR_NOOP("Doors and windows: alarm right away")},
    {ALARM_ZT_INTERIOR_FOLLOWER, TR_NOOP_C("zone type", "Interior follower"), TR_NOOP("Motion: waits while entry time runs")},
    {ALARM_ZT_INTERIOR_DELAY, TR_NOOP_C("zone type", "Interior with delay"), TR_NOOP("Motion that starts entry time")},
    {ALARM_ZT_DAY_NIGHT, TR_NOOP_C("zone type", "Trouble by day, alarm by night"), TR_NOOP("Doors that shouldn't open")},
    {ALARM_ZT_SILENT_BURGLARY, TR_NOOP_C("zone type", "Silent burglary"), TR_NOOP("Reports an alarm without the siren")},
    {ALARM_ZT_24H_SILENT, TR_NOOP_C("zone type", "24-hour silent"), TR_NOOP("Hold-up button: always armed, silent")},
    {ALARM_ZT_24H_AUDIBLE, TR_NOOP_C("zone type", "24-hour audible"), TR_NOOP("Panic button: always armed, siren")},
    {ALARM_ZT_24H_AUX, TR_NOOP_C("zone type", "24-hour auxiliary"), TR_NOOP("Water, freezer, medical: keypad sounds")},
    {ALARM_ZT_FIRE, TR_NOOP_C("zone type", "Fire"), TR_NOOP("Smoke and heat detectors")},
    {ALARM_ZT_FIRE_VERIFIED, TR_NOOP_C("zone type", "Fire with verification"), TR_NOOP("Resets once, alarms on a second trip")},
    {ALARM_ZT_CO, TR_NOOP_C("zone type", "Carbon monoxide"), TR_NOOP("CO detectors")},
    {ALARM_ZT_MONITOR, TR_NOOP_C("zone type", "Monitor"), TR_NOOP("Logs and reports, never alarms")},
    {ALARM_ZT_ARM_STAY, TR_NOOP_C("zone type", "Arm Stay button"), TR_NOOP("A keyfob or button that arms Stay")},
    {ALARM_ZT_ARM_AWAY, TR_NOOP_C("zone type", "Arm Away button"), TR_NOOP("A keyfob or button that arms Away")},
    {ALARM_ZT_DISARM, TR_NOOP_C("zone type", "Disarm button"), TR_NOOP("A keyfob or button that disarms")},
    {ALARM_ZT_KEYSWITCH, TR_NOOP_C("zone type", "Keyswitch"), TR_NOOP("Arms Away and disarms")},
    {ALARM_ZT_MOMENTARY_EXIT, TR_NOOP_C("zone type", "Momentary exit"), TR_NOOP("Restarts exit time when pressed")},
    {ALARM_ZT_NO_RESPONSE, TR_NOOP_C("zone type", "No response"), TR_NOOP("Chime only (a doorbell)")},
};

const size_t UI_ZONE_TYPE_COUNT = sizeof(UI_ZONE_TYPES) / sizeof(UI_ZONE_TYPES[0]);

const char *ui_zone_type_name(alarm_zone_type_t type)
{
    for (size_t i = 0; i < UI_ZONE_TYPE_COUNT; i++) {
        if (UI_ZONE_TYPES[i].type == type) {
            return TR_C("zone type", UI_ZONE_TYPES[i].name);
        }
    }
    return type == ALARM_ZT_NOT_USED ? TR_C("zone type", "Not used") : TR("Unknown");
}

const char *ui_device_class_name(alarm_device_class_t cls)
{
    static const char *const names[UI_DEVICE_CLASS_COUNT] = {
        [ALARM_DEVCLASS_GENERIC] = TR_NOOP_C("sensor kind", "Other sensor"),
        [ALARM_DEVCLASS_DOOR] = TR_NOOP_C("sensor kind", "Door"),
        [ALARM_DEVCLASS_WINDOW] = TR_NOOP_C("sensor kind", "Window"),
        [ALARM_DEVCLASS_GARAGE_DOOR] = TR_NOOP_C("sensor kind", "Garage door"),
        [ALARM_DEVCLASS_MOTION] = TR_NOOP_C("sensor kind", "Motion"),
        [ALARM_DEVCLASS_GLASS_BREAK] = TR_NOOP_C("sensor kind", "Glass break"),
        [ALARM_DEVCLASS_SMOKE] = TR_NOOP_C("sensor kind", "Smoke"),
        [ALARM_DEVCLASS_HEAT] = TR_NOOP_C("sensor kind", "Heat"),
        [ALARM_DEVCLASS_CO] = TR_NOOP_C("sensor kind", "Carbon monoxide"),
        [ALARM_DEVCLASS_GAS] = TR_NOOP_C("sensor kind", "Gas"),
        [ALARM_DEVCLASS_WATER] = TR_NOOP_C("sensor kind", "Water leak"),
        [ALARM_DEVCLASS_TEMPERATURE] = TR_NOOP_C("sensor kind", "Temperature"),
        [ALARM_DEVCLASS_VIBRATION] = TR_NOOP_C("sensor kind", "Vibration"),
        [ALARM_DEVCLASS_PANIC_BUTTON] = TR_NOOP_C("sensor kind", "Panic button"),
        [ALARM_DEVCLASS_KEYFOB] = TR_NOOP_C("sensor kind", "Keyfob"),
        [ALARM_DEVCLASS_KEYSWITCH] = TR_NOOP_C("sensor kind", "Keyswitch"),
    };
    return ((unsigned)cls < UI_DEVICE_CLASS_COUNT && names[cls] != NULL)
               ? TR_C("sensor kind", names[cls])
               : TR_C("sensor kind", "Other sensor");
}

const char *ui_device_class_glyph(alarm_device_class_t cls)
{
    return class_glyph(cls, false);
}

void ui_sys_trouble_line(alarm_sys_trouble_t t, bool restored, ui_line_t *out)
{
    notify_words_t w;
    notify_sys_trouble_words(S.catalog, t, restored, &w);
    fill_line(&w, out);
}

void ui_zone_trouble_line(const alarm_config_t *cfg, uint16_t zone, alarm_zone_trouble_t t,
                          bool restored, ui_line_t *out)
{
    notify_words_t w;
    notify_zone_trouble_words(S.catalog, cfg, zone, t, restored, &w);
    fill_line(&w, out);
}

const char *ui_result_text(alarm_result_t r)
{
    return notify_result_text(S.catalog, r);
}
