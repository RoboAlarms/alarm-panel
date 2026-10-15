/**
 * @file alarm_ui.h
 *
 * Custom (not generated) part of the LVGL Pro project. The screens call the event callbacks
 * named in the XML (ui_on_*); they all forward to one action handler that the application
 * installs with alarm_ui_set_action_cb(). Without a handler (LVGL Pro Editor preview) the
 * buttons do nothing.
 */

#ifndef ALARM_UI_H
#define ALARM_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "alarm_ui_gen.h"

typedef enum {
    ALARM_UI_ACT_ARM = 0,      /* user_data: "away" | "stay" | "night" */
    ALARM_UI_ACT_DISARM,
    ALARM_UI_ACT_PAD_KEY,      /* user_data: "0".."9" | "del" | "ok" */
    ALARM_UI_ACT_PAD_CLOSE,
    ALARM_UI_ACT_SILENCE,
    ALARM_UI_ACT_CLEAR_MEMORY,
    ALARM_UI_ACT_END_WALK_TEST, /* Home during a walk test; user_data: "end" | "extend" | "zones" */
    ALARM_UI_ACT_ZONE_BYPASS,  /* the zone card is the target's parent */
    ALARM_UI_ACT_ACK_TROUBLES,
    ALARM_UI_ACT_TROUBLES_PAGE, /* user_data: "back" | "more" */
    ALARM_UI_ACT_PANIC,        /* every event; user_data: "police" | "fire" | "medical" */
    ALARM_UI_ACT_CHIME,
    ALARM_UI_ACT_CHANGE_CODE,
    ALARM_UI_ACT_SETTINGS,
    ALARM_UI_ACT_WAKE,
    ALARM_UI_ACT_SETTINGS_SECTION, /* user_data: "display" | "users" | "log" | "installer" | "about" */
    ALARM_UI_ACT_PAGE_BACK,
    ALARM_UI_ACT_SHEET_DONE,       /* text entry Done, confirmation OK */
    ALARM_UI_ACT_SHEET_CANCEL,
    ALARM_UI_ACT_SHEET_EYE,        /* the eye at the end of a password field */
    ALARM_UI_ACT_PROGRAM_EXIT,     /* the installer-mode banner */
    ALARM_UI_ACT_WIZARD_NEXT,
    ALARM_UI_ACT_WIZARD_BACK,
    ALARM_UI_ACT_WIZARD_EXIT,      /* "Leave setup": the panel is already set up */
    ALARM_UI_ACT_REMOTE_BANNER,    /* the red remote-keypad banner */
} alarm_ui_action_t;

typedef void (*alarm_ui_action_cb_t)(alarm_ui_action_t action, lv_event_t * e);

/**
 * Initialize the component library
 */
void alarm_ui_init(const char * asset_path);

/** Route the screens' events to the application. */
void alarm_ui_set_action_cb(alarm_ui_action_cb_t cb);

/**
 * Appearance (docs/features/07 UI-010, BL-132): make `theme` (ALARM_UI_THEME_*) current - the
 * palette COLOR_* read, every style the layout made, its theme changes (themes.xml) and the
 * colour subjects - at once. Objects' own colours follow with alarm_ui_theme_recolor_tree().
 */
void alarm_ui_theme_set(uint32_t theme);

uint32_t alarm_ui_theme_get(void);

/** The colour of the same name in `to` when `c` is a palette colour, else `c` */
lv_color_t alarm_ui_theme_color(lv_color_t c, const lv_color_t * to);

/** Give the colours `obj` and its children hold of their own (local styles) `to`'s */
void alarm_ui_theme_recolor_tree(lv_obj_t * obj, const lv_color_t * to);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ALARM_UI_H*/
