/**
 * @file alarm_ui_gen.c
 */

#include "alarm_ui_gen.h"

/**********************
 *  STATIC VARIABLES
 **********************/

static uint32_t alarm_ui_target = ALARM_UI_TARGET_ALL;

/**********************
 *  GLOBAL VARIABLES
 **********************/

/*--------------------
 *  Permanent screens
 *-------------------*/

lv_obj_t * screen_main = NULL;
lv_obj_t * screen_saver = NULL;
lv_obj_t * screen_wizard = NULL;

/*----------------
 * Fonts
 *----------------*/

lv_font_t * font_caption;
extern lv_font_t font_caption_data;
lv_font_t * font_body;
extern lv_font_t font_body_data;
lv_font_t * font_label;
extern lv_font_t font_label_data;
lv_font_t * font_title;
extern lv_font_t font_title_data;
lv_font_t * font_headline;
extern lv_font_t font_headline_data;
lv_font_t * font_keypad;
extern const lv_font_t font_keypad_data;
lv_font_t * font_digits;
extern const lv_font_t font_digits_data;
lv_font_t * icons_20;
extern const lv_font_t icons_20_data;
lv_font_t * icons_28;
extern const lv_font_t icons_28_data;
lv_font_t * icons_40;
extern const lv_font_t icons_40_data;
lv_font_t * icons_96;
extern const lv_font_t icons_96_data;

const uint32_t alarm_ui_letters_fingerprint = 0x7145e655u;

/*----------------
 * Global styles
 *----------------*/

lv_style_t style_screen;
lv_style_t style_screen_alarm;
lv_style_t style_saver;
lv_style_t style_card;
lv_style_t style_pressable;
lv_style_t style_knob_off;
lv_style_t style_text_2;
lv_style_t style_text_3;
lv_style_t style_text_error;
lv_style_t style_text_warn;
lv_style_t style_nav_item;
lv_style_t style_nav_item_pressed;
lv_style_t style_nav_active;
lv_style_t style_nav_on;
lv_style_t style_device_arrange;
lv_style_t style_device_picked;
lv_style_t style_device_still;
lv_style_t style_segment_active;
lv_style_t style_button;
lv_style_t style_button_pressed;
lv_style_t style_button_primary;
lv_style_t style_button_danger;
lv_style_t style_button_tone_pressed;
lv_style_t style_disabled;
lv_style_t style_bar_round;
lv_style_t style_bar_pill;
lv_style_t style_bar_pill_active;
lv_style_t style_backdrop;
lv_style_t style_sheet_card;
lv_style_t style_roller;
lv_style_t style_roller_selected;
lv_style_t style_step;
lv_style_t style_step_on;

/*----------------
 * Subjects
 *----------------*/

lv_subject_t subject_view;
lv_subject_t subject_panel;
lv_subject_t subject_alarm_active;
lv_subject_t subject_tone_color;
lv_subject_t subject_pad_color;
lv_subject_t subject_hero_icon;
lv_subject_t subject_headline;
lv_subject_t subject_detail;
lv_subject_t subject_hero_note;
lv_subject_t subject_countdown;
lv_subject_t subject_countdown_pct;
lv_subject_t subject_disarm_title;
lv_subject_t subject_disarm_note;
lv_subject_t subject_memory_note;
lv_subject_t subject_walk_count;
lv_subject_t subject_walk_left;
lv_subject_t subject_walk_from;
lv_subject_t subject_walk_from_on;
lv_subject_t subject_walk_warn;
lv_subject_t subject_walk_banner;
lv_subject_t subject_date;
lv_subject_t subject_time;
lv_subject_t subject_ampm;
lv_subject_t subject_wifi_icon;
lv_subject_t subject_battery_icon;
lv_subject_t subject_battery_text;
lv_subject_t subject_battery_critical;
lv_subject_t subject_battery_warn;
lv_subject_t subject_wifi_ok;
lv_subject_t subject_trouble_count;
lv_subject_t subject_trouble_text;
lv_subject_t subject_open_count;
lv_subject_t subject_devices_avail;
lv_subject_t subject_devices_note;
lv_subject_t subject_chime;
lv_subject_t subject_default_codes;
lv_subject_t subject_chime_icon;
lv_subject_t subject_pad_title;
lv_subject_t subject_pad_hint;
lv_subject_t subject_pad_glyph;
lv_subject_t subject_pad_glyph_on;
lv_subject_t subject_pad_note;
lv_subject_t subject_pad_note_on;
lv_subject_t subject_pad_close_on;
lv_subject_t subject_modal_pad;
lv_subject_t subject_pad_dots_on;
lv_subject_t subject_pad_digits;
lv_subject_t subject_pad_error;
lv_subject_t subject_pad_options;
lv_subject_t subject_pad_silence;
lv_subject_t subject_opt_instant;
lv_subject_t subject_opt_silent;
lv_subject_t subject_opt_silent_avail;
lv_subject_t subject_locked;
lv_subject_t subject_opt_bypass;
lv_subject_t subject_opt_bypass_avail;
lv_subject_t subject_toast;
lv_subject_t subject_toast_visible;
lv_subject_t subject_zones_note;
lv_subject_t subject_zone_filter;
lv_subject_t subject_events_note;
lv_subject_t subject_event_filter;
lv_subject_t subject_troubles_note;
lv_subject_t subject_troubles_empty;
lv_subject_t subject_zones_empty;
lv_subject_t subject_troubles_pager;
lv_subject_t subject_troubles_page;
lv_subject_t subject_panic_report;
lv_subject_t subject_bar_banner;
lv_subject_t subject_update_banner;
lv_subject_t subject_update_banner_glyph;
lv_subject_t subject_remote_banner;
lv_subject_t subject_arm_banner;
lv_subject_t subject_arm_strip;
lv_subject_t subject_arm_strip_glyph;
lv_subject_t subject_arm_strip_color;
lv_subject_t subject_arm_strip_title;
lv_subject_t subject_arm_strip_detail;
lv_subject_t subject_arm_strip_note;
lv_subject_t subject_arm_strip_note_on;
lv_subject_t subject_poff_hour;
lv_subject_t subject_poff_night_title;
lv_subject_t subject_poff_night;
lv_subject_t subject_link_role;
lv_subject_t subject_link_home;
lv_subject_t subject_panic_hint;
lv_subject_t subject_panic_link;
lv_subject_t subject_settings_section;
lv_subject_t subject_automations_avail;
lv_subject_t subject_update_waiting;
lv_subject_t subject_settings_pad;
lv_subject_t subject_page_title;
lv_subject_t subject_page_note;
lv_subject_t subject_page_back;
lv_subject_t subject_busy;
lv_subject_t subject_sheet;
lv_subject_t subject_sheet_tone;
lv_subject_t subject_sheet_title;
lv_subject_t subject_sheet_note;
lv_subject_t subject_sheet_ok;
lv_subject_t subject_sheet_cancel;
lv_subject_t subject_sheet_eye;
lv_subject_t subject_sheet_shown;
lv_subject_t subject_wiz_step;
lv_subject_t subject_wiz_steps;
lv_subject_t subject_wiz_icon;
lv_subject_t subject_wiz_title;
lv_subject_t subject_wiz_note;
lv_subject_t subject_wiz_next;
lv_subject_t subject_wiz_next_on;
lv_subject_t subject_wiz_back;
lv_subject_t subject_wiz_exit;
lv_subject_t subject_wiz_pad;

/*----------------
 * Appearance (themes.xml)
 *----------------*/

/* A value unlike themes.xml's (the comment) is nudged in the bits RGB565 drops, so each
 * value stands for one colour (tools/lvpro_gen.py): the screen shows the same. */
const lv_color_t alarm_ui_palettes[ALARM_UI_THEME_COUNT][ALARM_UI_COLOR_COUNT] = {
    [ALARM_UI_THEME_DARK] = {
        [ALARM_UI_COLOR_BG] = LV_COLOR_MAKE(0x0B, 0x0F, 0x14),
        [ALARM_UI_COLOR_RAIL] = LV_COLOR_MAKE(0x0E, 0x14, 0x1B),
        [ALARM_UI_COLOR_SURFACE] = LV_COLOR_MAKE(0x14, 0x1B, 0x24),
        [ALARM_UI_COLOR_RAISED] = LV_COLOR_MAKE(0x1C, 0x25, 0x31),
        [ALARM_UI_COLOR_PRESSED] = LV_COLOR_MAKE(0x2A, 0x35, 0x44),
        [ALARM_UI_COLOR_LINE] = LV_COLOR_MAKE(0x23, 0x2D, 0x3A),
        [ALARM_UI_COLOR_TEXT] = LV_COLOR_MAKE(0xF1, 0xF5, 0xF9),
        [ALARM_UI_COLOR_TEXT_2] = LV_COLOR_MAKE(0x94, 0xA3, 0xB8),
        [ALARM_UI_COLOR_TEXT_3] = LV_COLOR_MAKE(0x80, 0x8D, 0xA0),
        [ALARM_UI_COLOR_GREEN] = LV_COLOR_MAKE(0x34, 0xD3, 0x99),
        [ALARM_UI_COLOR_AMBER] = LV_COLOR_MAKE(0xFB, 0xBF, 0x24),
        [ALARM_UI_COLOR_BLUE] = LV_COLOR_MAKE(0x60, 0xA5, 0xFA),
        [ALARM_UI_COLOR_VIOLET] = LV_COLOR_MAKE(0xA7, 0x8B, 0xFA),
        [ALARM_UI_COLOR_ROSE] = LV_COLOR_MAKE(0xFB, 0x71, 0x85),
        [ALARM_UI_COLOR_ORANGE] = LV_COLOR_MAKE(0xFB, 0x92, 0x3C),
        [ALARM_UI_COLOR_RED] = LV_COLOR_MAKE(0xF8, 0x71, 0x71),
        [ALARM_UI_COLOR_ALARM_BG] = LV_COLOR_MAKE(0x2A, 0x0B, 0x0E),
        [ALARM_UI_COLOR_ON_TONE] = LV_COLOR_MAKE(0x0B, 0x0F, 0x15), /* 0x0B0F14 */
        [ALARM_UI_COLOR_ALARM_CARD] = LV_COLOR_MAKE(0x3A, 0x12, 0x16),
        [ALARM_UI_COLOR_TOAST_LINE] = LV_COLOR_MAKE(0x3A, 0x46, 0x58),
        [ALARM_UI_COLOR_KNOB] = LV_COLOR_MAKE(0xF1, 0xF5, 0xF8), /* 0xF1F5F9 */
        [ALARM_UI_COLOR_KEY_SPECIAL] = LV_COLOR_MAKE(0x23, 0x2D, 0x3B), /* 0x232D3A */
    },
    [ALARM_UI_THEME_LIGHT] = {
        [ALARM_UI_COLOR_BG] = LV_COLOR_MAKE(0xED, 0xF1, 0xF5),
        [ALARM_UI_COLOR_RAIL] = LV_COLOR_MAKE(0xF8, 0xFA, 0xFC),
        [ALARM_UI_COLOR_SURFACE] = LV_COLOR_MAKE(0xFF, 0xFF, 0xFE), /* 0xFFFFFF */
        [ALARM_UI_COLOR_RAISED] = LV_COLOR_MAKE(0xE1, 0xE7, 0xEE),
        [ALARM_UI_COLOR_PRESSED] = LV_COLOR_MAKE(0xCB, 0xD4, 0xDF),
        [ALARM_UI_COLOR_LINE] = LV_COLOR_MAKE(0xD3, 0xDA, 0xE3),
        [ALARM_UI_COLOR_TEXT] = LV_COLOR_MAKE(0x0F, 0x17, 0x2A),
        [ALARM_UI_COLOR_TEXT_2] = LV_COLOR_MAKE(0x47, 0x55, 0x69),
        [ALARM_UI_COLOR_TEXT_3] = LV_COLOR_MAKE(0x56, 0x65, 0x7A),
        [ALARM_UI_COLOR_GREEN] = LV_COLOR_MAKE(0x04, 0x76, 0x56),
        [ALARM_UI_COLOR_AMBER] = LV_COLOR_MAKE(0x9C, 0x56, 0x00),
        [ALARM_UI_COLOR_BLUE] = LV_COLOR_MAKE(0x1D, 0x4E, 0xD8),
        [ALARM_UI_COLOR_VIOLET] = LV_COLOR_MAKE(0x6D, 0x28, 0xD9),
        [ALARM_UI_COLOR_ROSE] = LV_COLOR_MAKE(0xC7, 0x1E, 0x4E),
        [ALARM_UI_COLOR_ORANGE] = LV_COLOR_MAKE(0xB8, 0x3E, 0x0B),
        [ALARM_UI_COLOR_RED] = LV_COLOR_MAKE(0xB9, 0x1C, 0x1C),
        [ALARM_UI_COLOR_ALARM_BG] = LV_COLOR_MAKE(0xFD, 0xE2, 0xE2),
        [ALARM_UI_COLOR_ON_TONE] = LV_COLOR_MAKE(0xFE, 0xFF, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_ALARM_CARD] = LV_COLOR_MAKE(0xFF, 0xE4, 0xE6),
        [ALARM_UI_COLOR_TOAST_LINE] = LV_COLOR_MAKE(0xB8, 0xC3, 0xD0),
        [ALARM_UI_COLOR_KNOB] = LV_COLOR_MAKE(0xFF, 0xFE, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_KEY_SPECIAL] = LV_COLOR_MAKE(0xD3, 0xDA, 0xE2), /* 0xD3DAE3 */
    },
    [ALARM_UI_THEME_CONTRAST] = {
        [ALARM_UI_COLOR_BG] = LV_COLOR_MAKE(0x00, 0x00, 0x01), /* 0x000000 */
        [ALARM_UI_COLOR_RAIL] = LV_COLOR_MAKE(0x01, 0x00, 0x00), /* 0x000000 */
        [ALARM_UI_COLOR_SURFACE] = LV_COLOR_MAKE(0x0A, 0x0C, 0x10),
        [ALARM_UI_COLOR_RAISED] = LV_COLOR_MAKE(0x1A, 0x1F, 0x27),
        [ALARM_UI_COLOR_PRESSED] = LV_COLOR_MAKE(0x39, 0x41, 0x50),
        [ALARM_UI_COLOR_LINE] = LV_COLOR_MAKE(0xA7, 0xB1, 0xBE),
        [ALARM_UI_COLOR_TEXT] = LV_COLOR_MAKE(0xFF, 0xFF, 0xFD), /* 0xFFFFFF */
        [ALARM_UI_COLOR_TEXT_2] = LV_COLOR_MAKE(0xE8, 0xEC, 0xF1),
        [ALARM_UI_COLOR_TEXT_3] = LV_COLOR_MAKE(0xC9, 0xD0, 0xD9),
        [ALARM_UI_COLOR_GREEN] = LV_COLOR_MAKE(0x4A, 0xDE, 0x80),
        [ALARM_UI_COLOR_AMBER] = LV_COLOR_MAKE(0xFC, 0xD3, 0x4D),
        [ALARM_UI_COLOR_BLUE] = LV_COLOR_MAKE(0x93, 0xC5, 0xFD),
        [ALARM_UI_COLOR_VIOLET] = LV_COLOR_MAKE(0xC4, 0xB5, 0xFD),
        [ALARM_UI_COLOR_ROSE] = LV_COLOR_MAKE(0xFD, 0xA4, 0xAF),
        [ALARM_UI_COLOR_ORANGE] = LV_COLOR_MAKE(0xFD, 0xBA, 0x74),
        [ALARM_UI_COLOR_RED] = LV_COLOR_MAKE(0xFF, 0x8A, 0x8A),
        [ALARM_UI_COLOR_ALARM_BG] = LV_COLOR_MAKE(0x3D, 0x00, 0x00),
        [ALARM_UI_COLOR_ON_TONE] = LV_COLOR_MAKE(0x00, 0x01, 0x00), /* 0x000000 */
        [ALARM_UI_COLOR_ALARM_CARD] = LV_COLOR_MAKE(0x4A, 0x0A, 0x0A),
        [ALARM_UI_COLOR_TOAST_LINE] = LV_COLOR_MAKE(0xE8, 0xEC, 0xF0), /* 0xE8ECF1 */
        [ALARM_UI_COLOR_KNOB] = LV_COLOR_MAKE(0xFF, 0xFE, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_KEY_SPECIAL] = LV_COLOR_MAKE(0x2A, 0x31, 0x3C),
    },
    [ALARM_UI_THEME_CONTRAST_LIGHT] = {
        [ALARM_UI_COLOR_BG] = LV_COLOR_MAKE(0xFD, 0xFF, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_RAIL] = LV_COLOR_MAKE(0xFF, 0xFD, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_SURFACE] = LV_COLOR_MAKE(0xFF, 0xFF, 0xFE), /* 0xFFFFFF */
        [ALARM_UI_COLOR_RAISED] = LV_COLOR_MAKE(0xEE, 0xF1, 0xF4),
        [ALARM_UI_COLOR_PRESSED] = LV_COLOR_MAKE(0xD5, 0xDB, 0xE2),
        [ALARM_UI_COLOR_LINE] = LV_COLOR_MAKE(0x4B, 0x55, 0x63),
        [ALARM_UI_COLOR_TEXT] = LV_COLOR_MAKE(0x00, 0x00, 0x02), /* 0x000000 */
        [ALARM_UI_COLOR_TEXT_2] = LV_COLOR_MAKE(0x1F, 0x29, 0x37),
        [ALARM_UI_COLOR_TEXT_3] = LV_COLOR_MAKE(0x37, 0x41, 0x51),
        [ALARM_UI_COLOR_GREEN] = LV_COLOR_MAKE(0x06, 0x5C, 0x44),
        [ALARM_UI_COLOR_AMBER] = LV_COLOR_MAKE(0x74, 0x46, 0x00),
        [ALARM_UI_COLOR_BLUE] = LV_COLOR_MAKE(0x1E, 0x40, 0xAF),
        [ALARM_UI_COLOR_VIOLET] = LV_COLOR_MAKE(0x5B, 0x21, 0xB6),
        [ALARM_UI_COLOR_ROSE] = LV_COLOR_MAKE(0x9F, 0x12, 0x39),
        [ALARM_UI_COLOR_ORANGE] = LV_COLOR_MAKE(0x90, 0x31, 0x11),
        [ALARM_UI_COLOR_RED] = LV_COLOR_MAKE(0x99, 0x1B, 0x1B),
        [ALARM_UI_COLOR_ALARM_BG] = LV_COLOR_MAKE(0xFF, 0xE1, 0xE1),
        [ALARM_UI_COLOR_ON_TONE] = LV_COLOR_MAKE(0xFE, 0xFF, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_ALARM_CARD] = LV_COLOR_MAKE(0xFF, 0xE1, 0xE0), /* 0xFFE1E1 */
        [ALARM_UI_COLOR_TOAST_LINE] = LV_COLOR_MAKE(0x1F, 0x29, 0x36), /* 0x1F2937 */
        [ALARM_UI_COLOR_KNOB] = LV_COLOR_MAKE(0xFF, 0xFE, 0xFF), /* 0xFFFFFF */
        [ALARM_UI_COLOR_KEY_SPECIAL] = LV_COLOR_MAKE(0xE0, 0xE5, 0xEA),
    },
};

const lv_color_t * alarm_ui_palette = alarm_ui_palettes[0];

const uint8_t alarm_ui_theme_saver[ALARM_UI_THEME_COUNT] = {ALARM_UI_THEME_DARK, ALARM_UI_THEME_DARK, ALARM_UI_THEME_CONTRAST, ALARM_UI_THEME_CONTRAST};
const bool alarm_ui_theme_high_contrast[ALARM_UI_THEME_COUNT] = {false, false, true, true};

lv_style_t * alarm_ui_styles[ALARM_UI_STYLE_COUNT];

const alarm_ui_style_override_t alarm_ui_style_overrides[] = {
    {ALARM_UI_STYLE_CARD, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_CARD, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_CARD, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_BUTTON, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_BUTTON, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_BUTTON, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_BAR_ROUND, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_BAR_ROUND, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_BAR_ROUND, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_BAR_PILL, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_BAR_PILL, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_BAR_PILL, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_SHEET_CARD, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_SHEET_CARD, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_SHEET_CARD, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_LIST_ROW_LIST_ROW, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_LIST_ROW_LIST_ROW, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_LIST_ROW_LIST_ROW, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ROW_SETTING, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ROW_SETTING, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ROW_SETTING, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_SWITCH, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_SWITCH, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_SWITCH, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_ROW_SETTING_ICON, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_ROW_SETTING_ICON, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_ROW_SETTING_ICON, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_STEPPER_ROW_STEPPER, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_STEPPER_ROW_STEPPER, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_STEPPER_ROW_STEPPER, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_ROW_SLIDER, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_ROW_SLIDER, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_ROW_SLIDER, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_SLIDER, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 1, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_SLIDER, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 0, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_SLIDER, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_EVENT_ROW_EVENT_ROW, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_EVENT_ROW_EVENT_ROW, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_EVENT_ROW_EVENT_ROW, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_TROUBLE_ROW_TROUBLE_ROW, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_TROUBLE_ROW_TROUBLE_ROW, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_TROUBLE_ROW_TROUBLE_ROW, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ZONE_CARD_ZONE_CARD, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ZONE_CARD_ZONE_CARD, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ZONE_CARD_ZONE_CARD, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_BIG_VALUE_BIG_VALUE, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_BIG_VALUE_BIG_VALUE, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_BIG_VALUE_BIG_VALUE, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_CHIP_CHIP, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_CHIP_CHIP, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_CHIP_CHIP, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ARM_TILE_ARM_TILE, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ARM_TILE_ARM_TILE, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ARM_TILE_ARM_TILE, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_HERO_HERO, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_HERO_HERO, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_HERO_HERO, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_HERO_HERO_ALARM, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_RED, 0},
    {ALARM_UI_STYLE_CODE_PAD_PAD, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_CODE_PAD_PAD, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_CODE_PAD_PAD, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_PAD_KEY_PAD_KEY, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_PAD_KEY_PAD_KEY, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_PAD_KEY_PAD_KEY, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROUND, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROUND, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROUND, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_LINE, 0},
    {ALARM_UI_STYLE_NAV_ACTIVE, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_NAV_ACTIVE, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_NAV_ACTIVE, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_TEXT, 0},
    {ALARM_UI_STYLE_SEGMENT_ACTIVE, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_SEGMENT_ACTIVE, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_SEGMENT_ACTIVE, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_TEXT, 0},
    {ALARM_UI_STYLE_CAL_DAY_CAL_DAY_CHOSEN, LV_STYLE_OUTLINE_WIDTH, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 2, 0},
    {ALARM_UI_STYLE_CAL_DAY_CAL_DAY_CHOSEN, LV_STYLE_OUTLINE_PAD, 0x0C, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, -2, 0},
    {ALARM_UI_STYLE_CAL_DAY_CAL_DAY_CHOSEN, LV_STYLE_OUTLINE_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_TEXT, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_SWITCH_KNOB_ON, LV_STYLE_BG_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_ON_TONE, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_KNOB_ON, LV_STYLE_BG_COLOR, 0x0C, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_ON_TONE, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_SWITCH_KNOB, LV_STYLE_BORDER_WIDTH, 0x0A, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 1, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_SWITCH_KNOB, LV_STYLE_BORDER_COLOR, 0x0A, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_TEXT_3, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_KNOB, LV_STYLE_BORDER_WIDTH, 0x0A, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 1, 0},
    {ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_KNOB, LV_STYLE_BORDER_COLOR, 0x0A, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_TEXT_3, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_SLIDER_KNOB, LV_STYLE_BORDER_WIDTH, 0x0A, ALARM_UI_VALUE_INT, ALARM_UI_VALUE_NONE, 1, 0},
    {ALARM_UI_STYLE_ROW_SLIDER_SLIDER_KNOB, LV_STYLE_BORDER_COLOR, 0x0A, ALARM_UI_VALUE_COLOR, ALARM_UI_VALUE_NONE, ALARM_UI_COLOR_TEXT_3, 0},
};

const uint32_t alarm_ui_style_override_count = 108;

lv_subject_t * const alarm_ui_color_subjects[] = {
    &subject_tone_color,
    &subject_pad_color,
    &subject_arm_strip_color,
};

const uint32_t alarm_ui_color_subject_count = 3;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void alarm_ui_init_gen(const char * asset_path)
{
    LV_UNUSED(asset_path);

    /* When running from the editor the theme set from the XML should overwrite this */
#if !defined(LV_EDITOR_PREVIEW)
#if LV_USE_THEME_SIMPLE
    lv_display_t * disp = lv_display_get_default();
    lv_theme_t * th = lv_theme_simple_init(disp);
    lv_display_set_theme(disp, th);
#else
    LV_LOG_WARN("Simple theme is selected in project.xml but LV_USE_THEME_SIMPLE is disabled");
#endif
#endif /*LV_EDITOR_PREVIEW*/

    /*----------------
     * Fonts
     *----------------*/

    /* get font 'font_caption' from a C array */
    font_caption = (lv_font_t *)&font_caption_data;
    /* get font 'font_body' from a C array */
    font_body = (lv_font_t *)&font_body_data;
    /* get font 'font_label' from a C array */
    font_label = (lv_font_t *)&font_label_data;
    /* get font 'font_title' from a C array */
    font_title = (lv_font_t *)&font_title_data;
    /* get font 'font_headline' from a C array */
    font_headline = (lv_font_t *)&font_headline_data;
    /* get font 'font_keypad' from a C array */
    font_keypad = (lv_font_t *)&font_keypad_data;
    /* get font 'font_digits' from a C array */
    font_digits = (lv_font_t *)&font_digits_data;
    /* get font 'icons_20' from a C array */
    icons_20 = (lv_font_t *)&icons_20_data;
    /* get font 'icons_28' from a C array */
    icons_28 = (lv_font_t *)&icons_28_data;
    /* get font 'icons_40' from a C array */
    icons_40 = (lv_font_t *)&icons_40_data;
    /* get font 'icons_96' from a C array */
    icons_96 = (lv_font_t *)&icons_96_data;

    /*----------------
     * Global styles
     *----------------*/

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_screen,
            &style_screen_alarm,
            &style_saver,
            &style_card,
            &style_pressable,
            &style_knob_off,
            &style_text_2,
            &style_text_3,
            &style_text_error,
            &style_text_warn,
            &style_nav_item,
            &style_nav_item_pressed,
            &style_nav_active,
            &style_nav_on,
            &style_device_arrange,
            &style_device_picked,
            &style_device_still,
            &style_segment_active,
            &style_button,
            &style_button_pressed,
            &style_button_primary,
            &style_button_danger,
            &style_button_tone_pressed,
            &style_disabled,
            &style_bar_round,
            &style_bar_pill,
            &style_bar_pill_active,
            &style_backdrop,
            &style_sheet_card,
            &style_roller,
            &style_roller_selected,
            &style_step,
            &style_step_on,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_6(&style_screen, COLOR_BG, (255 * 100 / 100));
        lv_style_set_text_color(&style_screen, COLOR_TEXT);
        lv_style_set_text_font(&style_screen, font_body);
        lv_style_set_pad_all(&style_screen, 0);
        lv_style_set_bg_color(&style_screen_alarm, COLOR_ALARM_BG);
        lv_style_set_bg_grad_color(&style_screen_alarm, COLOR_BG);
        lv_style_set_bg_grad_dir(&style_screen_alarm, LV_GRAD_DIR_VER);
        alarm_ui_run_6(&style_saver, lv_color_hex(0x000000), (255 * 100 / 100));
        lv_style_set_text_font(&style_saver, font_body);
        lv_style_set_pad_all(&style_saver, 0);
        alarm_ui_run_6(&style_card, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_card, RADIUS_LG);
        lv_style_set_bg_color(&style_pressable, COLOR_RAISED);
        lv_style_set_bg_opa(&style_knob_off, (255 * 0 / 100));
        lv_style_set_pad_all(&style_knob_off, 0);
        lv_style_set_text_color(&style_text_2, COLOR_TEXT_2);
        lv_style_set_text_color(&style_text_3, COLOR_TEXT_3);
        lv_style_set_text_color(&style_text_error, COLOR_RED);
        lv_style_set_text_color(&style_text_warn, COLOR_AMBER);
        alarm_ui_run_19(&style_nav_item, 88, 68, RADIUS_MD);
        alarm_ui_run_17(&style_nav_item, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_nav_item, 4);
        lv_style_set_text_color(&style_nav_item, COLOR_TEXT_2);
        alarm_ui_run_6(&style_nav_item_pressed, COLOR_PRESSED, (255 * 100 / 100));
        alarm_ui_run_6(&style_nav_active, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_nav_active, COLOR_TEXT);
        lv_style_set_text_color(&style_nav_on, COLOR_BLUE);
        alarm_ui_run_64(&style_device_arrange, 3, COLOR_BLUE);
        lv_style_set_border_opa(&style_device_arrange, (255 * 0 / 100));
        alarm_ui_run_64(&style_device_picked, 3, COLOR_BLUE);
        lv_style_set_border_opa(&style_device_picked, (255 * 100 / 100));
        lv_style_set_bg_color(&style_device_still, COLOR_SURFACE);
        alarm_ui_run_6(&style_segment_active, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_segment_active, COLOR_TEXT);
        alarm_ui_run_63(&style_button, 52, RADIUS_FULL);
        alarm_ui_run_6(&style_button, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_15(&style_button, SPACE_XL, SPACE_XL);
        lv_style_set_pad_column(&style_button, SPACE_SM);
        alarm_ui_run_45(&style_button, font_label, COLOR_TEXT);
        lv_style_set_bg_color(&style_button_pressed, COLOR_PRESSED);
        lv_style_set_bg_color(&style_button_primary, COLOR_BLUE);
        lv_style_set_text_color(&style_button_primary, COLOR_ON_TONE);
        lv_style_set_bg_color(&style_button_danger, COLOR_RED);
        lv_style_set_text_color(&style_button_danger, COLOR_ON_TONE);
        lv_style_set_bg_opa(&style_button_tone_pressed, (255 * 80 / 100));
        lv_style_set_opa(&style_disabled, (255 * 40 / 100));
        lv_style_set_radius(&style_bar_round, RADIUS_FULL);
        alarm_ui_run_6(&style_bar_round, COLOR_SURFACE, (255 * 100 / 100));
        lv_style_set_text_color(&style_bar_round, COLOR_TEXT_2);
        alarm_ui_run_63(&style_bar_pill, 40, RADIUS_FULL);
        alarm_ui_run_6(&style_bar_pill, COLOR_AMBER, (255 * 16 / 100));
        alarm_ui_run_15(&style_bar_pill, SPACE_MD, SPACE_MD);
        lv_style_set_pad_column(&style_bar_pill, 6);
        lv_style_set_text_color(&style_bar_pill, COLOR_AMBER);
        lv_style_set_bg_opa(&style_bar_pill_active, (255 * 30 / 100));
        alarm_ui_run_6(&style_backdrop, lv_color_hex(0x000000), (255 * 65 / 100));
        alarm_ui_run_6(&style_sheet_card, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_sheet_card, RADIUS_XL);
        lv_style_set_pad_all(&style_sheet_card, SPACE_XL);
        lv_style_set_layout(&style_sheet_card, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&style_sheet_card, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_sheet_card, SPACE_MD);
        alarm_ui_run_63(&style_roller, ROLLER_H, RADIUS_MD);
        alarm_ui_run_6(&style_roller, COLOR_BG, (255 * 100 / 100));
        lv_style_set_border_width(&style_roller, 0);
        lv_style_set_pad_all(&style_roller, 0);
        alarm_ui_run_45(&style_roller, font_label, COLOR_TEXT_2);
        lv_style_set_text_align(&style_roller, LV_TEXT_ALIGN_CENTER);
        lv_style_set_anim_duration(&style_roller, 0);
        lv_style_set_radius(&style_roller_selected, RADIUS_SM);
        alarm_ui_run_6(&style_roller_selected, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_45(&style_roller_selected, font_title, COLOR_TEXT);
        alarm_ui_run_19(&style_step, 36, 6, RADIUS_FULL);
        alarm_ui_run_6(&style_step, COLOR_LINE, (255 * 100 / 100));
        lv_style_set_bg_color(&style_step_on, COLOR_BLUE);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_SCREEN,
            ALARM_UI_STYLE_SCREEN_ALARM,
            ALARM_UI_STYLE_SAVER,
            ALARM_UI_STYLE_CARD,
            ALARM_UI_STYLE_PRESSABLE,
            ALARM_UI_STYLE_KNOB_OFF,
            ALARM_UI_STYLE_TEXT_2,
            ALARM_UI_STYLE_TEXT_3,
            ALARM_UI_STYLE_TEXT_ERROR,
            ALARM_UI_STYLE_TEXT_WARN,
            ALARM_UI_STYLE_NAV_ITEM,
            ALARM_UI_STYLE_NAV_ITEM_PRESSED,
            ALARM_UI_STYLE_NAV_ACTIVE,
            ALARM_UI_STYLE_NAV_ON,
            ALARM_UI_STYLE_DEVICE_ARRANGE,
            ALARM_UI_STYLE_DEVICE_PICKED,
            ALARM_UI_STYLE_DEVICE_STILL,
            ALARM_UI_STYLE_SEGMENT_ACTIVE,
            ALARM_UI_STYLE_BUTTON,
            ALARM_UI_STYLE_BUTTON_PRESSED,
            ALARM_UI_STYLE_BUTTON_PRIMARY,
            ALARM_UI_STYLE_BUTTON_DANGER,
            ALARM_UI_STYLE_BUTTON_TONE_PRESSED,
            ALARM_UI_STYLE_DISABLED,
            ALARM_UI_STYLE_BAR_ROUND,
            ALARM_UI_STYLE_BAR_PILL,
            ALARM_UI_STYLE_BAR_PILL_ACTIVE,
            ALARM_UI_STYLE_BACKDROP,
            ALARM_UI_STYLE_SHEET_CARD,
            ALARM_UI_STYLE_ROLLER,
            ALARM_UI_STYLE_ROLLER_SELECTED,
            ALARM_UI_STYLE_STEP,
            ALARM_UI_STYLE_STEP_ON,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }

    /*----------------
     * Subjects
     *----------------*/
    lv_subject_init_int(&subject_view, 0);
    lv_subject_set_min_value_int(&subject_view, 0);
    lv_subject_set_max_value_int(&subject_view, 6);
    lv_subject_init_int(&subject_panel, 0);
    lv_subject_init_int(&subject_alarm_active, 0);
    lv_subject_init_color(&subject_tone_color, lv_color_hex(0x34D399));
    lv_subject_init_color(&subject_pad_color, lv_color_hex(0x34D399));
    static char subject_hero_icon_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_hero_icon_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_hero_icon,
                           subject_hero_icon_buf,
                           subject_hero_icon_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x87\xBF"
                          );
    static char subject_headline_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_headline_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_headline,
                           subject_headline_buf,
                           subject_headline_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Disarmed")
                          );
    static char subject_detail_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_detail_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_detail,
                           subject_detail_buf,
                           subject_detail_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Ready to arm")
                          );
    static char subject_hero_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_hero_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_hero_note,
                           subject_hero_note_buf,
                           subject_hero_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("All zones closed")
                          );
    lv_subject_init_int(&subject_countdown, 0);
    lv_subject_init_int(&subject_countdown_pct, 1000);
    lv_subject_set_min_value_int(&subject_countdown_pct, 0);
    lv_subject_set_max_value_int(&subject_countdown_pct, 1000);
    static char subject_disarm_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_disarm_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_disarm_title,
                           subject_disarm_title_buf,
                           subject_disarm_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Disarm")
                          );
    static char subject_disarm_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_disarm_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_disarm_note,
                           subject_disarm_note_buf,
                           subject_disarm_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Enter your code to disarm")
                          );
    static char subject_memory_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_memory_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_memory_note,
                           subject_memory_note_buf,
                           subject_memory_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_walk_count_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_walk_count_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_walk_count,
                           subject_walk_count_buf,
                           subject_walk_count_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_walk_left_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_walk_left_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_walk_left,
                           subject_walk_left_buf,
                           subject_walk_left_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_walk_from_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_walk_from_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_walk_from,
                           subject_walk_from_buf,
                           subject_walk_from_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_walk_from_on, 0);
    lv_subject_init_int(&subject_walk_warn, 0);
    static char subject_walk_banner_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_walk_banner_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_walk_banner,
                           subject_walk_banner_buf,
                           subject_walk_banner_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_date_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_date_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_date,
                           subject_date_buf,
                           subject_date_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Saturday, September 19")
                          );
    static char subject_time_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_time_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_time,
                           subject_time_buf,
                           subject_time_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "10:42"
                          );
    static char subject_ampm_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_ampm_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_ampm,
                           subject_ampm_buf,
                           subject_ampm_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("PM")
                          );
    static char subject_wifi_icon_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_wifi_icon_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_wifi_icon,
                           subject_wifi_icon_buf,
                           subject_wifi_icon_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x86\xAE"
                          );
    static char subject_battery_icon_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_battery_icon_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_battery_icon,
                           subject_battery_icon_buf,
                           subject_battery_icon_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x81\x93"
                          );
    static char subject_battery_text_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_battery_text_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_battery_text,
                           subject_battery_text_buf,
                           subject_battery_text_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_battery_critical, 1);
    lv_subject_init_int(&subject_battery_warn, 0);
    lv_subject_init_int(&subject_wifi_ok, 1);
    lv_subject_init_int(&subject_trouble_count, 0);
    static char subject_trouble_text_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_trouble_text_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_trouble_text,
                           subject_trouble_text_buf,
                           subject_trouble_text_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_open_count, 0);
    lv_subject_init_int(&subject_devices_avail, 0);
    static char subject_devices_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_devices_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_devices_note,
                           subject_devices_note_buf,
                           subject_devices_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_chime, 0);
    lv_subject_init_int(&subject_default_codes, 3);
    static char subject_chime_icon_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_chime_icon_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_chime_icon,
                           subject_chime_icon_buf,
                           subject_chime_icon_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x81\x9A"
                          );
    static char subject_pad_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_pad_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_pad_title,
                           subject_pad_title_buf,
                           subject_pad_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Arm Away")
                          );
    static char subject_pad_hint_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_pad_hint_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_pad_hint,
                           subject_pad_hint_buf,
                           subject_pad_hint_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Enter your code")
                          );
    static char subject_pad_glyph_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_pad_glyph_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_pad_glyph,
                           subject_pad_glyph_buf,
                           subject_pad_glyph_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_pad_glyph_on, 0);
    static char subject_pad_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_pad_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_pad_note,
                           subject_pad_note_buf,
                           subject_pad_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_pad_note_on, 0);
    lv_subject_init_int(&subject_pad_close_on, 1);
    lv_subject_init_int(&subject_modal_pad, 0);
    lv_subject_init_int(&subject_pad_dots_on, 0);
    lv_subject_init_int(&subject_pad_digits, 0);
    lv_subject_set_min_value_int(&subject_pad_digits, 0);
    lv_subject_set_max_value_int(&subject_pad_digits, 10);
    lv_subject_init_int(&subject_pad_error, 0);
    lv_subject_init_int(&subject_pad_options, 0);
    lv_subject_init_int(&subject_pad_silence, 0);
    lv_subject_init_int(&subject_opt_instant, 0);
    lv_subject_init_int(&subject_opt_silent, 0);
    lv_subject_init_int(&subject_opt_silent_avail, 1);
    lv_subject_init_int(&subject_locked, 0);
    lv_subject_init_int(&subject_opt_bypass, 0);
    lv_subject_init_int(&subject_opt_bypass_avail, 0);
    static char subject_toast_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_toast_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_toast,
                           subject_toast_buf,
                           subject_toast_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_toast_visible, 0);
    static char subject_zones_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_zones_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_zones_note,
                           subject_zones_note_buf,
                           subject_zones_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_zone_filter, 0);
    lv_subject_set_min_value_int(&subject_zone_filter, 0);
    lv_subject_set_max_value_int(&subject_zone_filter, 3);
    static char subject_events_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_events_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_events_note,
                           subject_events_note_buf,
                           subject_events_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_event_filter, 0);
    static char subject_troubles_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_troubles_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_troubles_note,
                           subject_troubles_note_buf,
                           subject_troubles_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_troubles_empty, 1);
    lv_subject_init_int(&subject_zones_empty, 0);
    lv_subject_set_min_value_int(&subject_zones_empty, 0);
    lv_subject_set_max_value_int(&subject_zones_empty, 1);
    lv_subject_init_int(&subject_troubles_pager, 0);
    static char subject_troubles_page_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_troubles_page_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_troubles_page,
                           subject_troubles_page_buf,
                           subject_troubles_page_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_panic_report_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_panic_report_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_panic_report,
                           subject_panic_report_buf,
                           subject_panic_report_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_bar_banner, 0);
    static char subject_update_banner_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_update_banner_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_update_banner,
                           subject_update_banner_buf,
                           subject_update_banner_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_update_banner_glyph_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_update_banner_glyph_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_update_banner_glyph,
                           subject_update_banner_glyph_buf,
                           subject_update_banner_glyph_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x82\xB2"
                          );
    static char subject_remote_banner_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_remote_banner_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_remote_banner,
                           subject_remote_banner_buf,
                           subject_remote_banner_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_arm_banner_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_arm_banner_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_arm_banner,
                           subject_arm_banner_buf,
                           subject_arm_banner_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_arm_strip, 0);
    static char subject_arm_strip_glyph_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_arm_strip_glyph_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_arm_strip_glyph,
                           subject_arm_strip_glyph_buf,
                           subject_arm_strip_glyph_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x84\x9E"
                          );
    lv_subject_init_color(&subject_arm_strip_color, lv_color_hex(0xA78BFA));
    static char subject_arm_strip_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_arm_strip_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_arm_strip_title,
                           subject_arm_strip_title_buf,
                           subject_arm_strip_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_arm_strip_detail_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_arm_strip_detail_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_arm_strip_detail,
                           subject_arm_strip_detail_buf,
                           subject_arm_strip_detail_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_arm_strip_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_arm_strip_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_arm_strip_note,
                           subject_arm_strip_note_buf,
                           subject_arm_strip_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_arm_strip_note_on, 0);
    static char subject_poff_hour_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_poff_hour_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_poff_hour,
                           subject_poff_hour_buf,
                           subject_poff_hour_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_poff_night_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_poff_night_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_poff_night_title,
                           subject_poff_night_title_buf,
                           subject_poff_night_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_poff_night_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_poff_night_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_poff_night,
                           subject_poff_night_buf,
                           subject_poff_night_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_link_role, 0);
    lv_subject_init_int(&subject_link_home, 0);
    static char subject_panic_hint_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_panic_hint_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_panic_hint,
                           subject_panic_hint_buf,
                           subject_panic_hint_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_panic_link, 0);
    lv_subject_init_int(&subject_settings_section, 0);
    lv_subject_init_int(&subject_automations_avail, 0);
    lv_subject_init_int(&subject_update_waiting, 0);
    lv_subject_init_int(&subject_settings_pad, 0);
    static char subject_page_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_page_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_page_title,
                           subject_page_title_buf,
                           subject_page_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Display and sound")
                          );
    static char subject_page_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_page_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_page_note,
                           subject_page_note_buf,
                           subject_page_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    lv_subject_init_int(&subject_page_back, 0);
    lv_subject_init_int(&subject_busy, 0);
    lv_subject_init_int(&subject_sheet, 0);
    lv_subject_init_int(&subject_sheet_tone, 0);
    static char subject_sheet_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_sheet_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_sheet_title,
                           subject_sheet_title_buf,
                           subject_sheet_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_sheet_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_sheet_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_sheet_note,
                           subject_sheet_note_buf,
                           subject_sheet_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_sheet_ok_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_sheet_ok_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_sheet_ok,
                           subject_sheet_ok_buf,
                           subject_sheet_ok_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Done")
                          );
    static char subject_sheet_cancel_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_sheet_cancel_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_sheet_cancel,
                           subject_sheet_cancel_buf,
                           subject_sheet_cancel_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Cancel")
                          );
    lv_subject_init_int(&subject_sheet_eye, 0);
    lv_subject_init_int(&subject_sheet_shown, 0);
    lv_subject_init_int(&subject_wiz_step, 0);
    lv_subject_set_min_value_int(&subject_wiz_step, 0);
    lv_subject_set_max_value_int(&subject_wiz_step, 6);
    lv_subject_init_int(&subject_wiz_steps, 5);
    lv_subject_set_min_value_int(&subject_wiz_steps, 4);
    lv_subject_set_max_value_int(&subject_wiz_steps, 6);
    static char subject_wiz_icon_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_wiz_icon_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_wiz_icon,
                           subject_wiz_icon_buf,
                           subject_wiz_icon_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           "\xEE\x90\x92"
                          );
    static char subject_wiz_title_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_wiz_title_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_wiz_title,
                           subject_wiz_title_buf,
                           subject_wiz_title_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Welcome")
                          );
    static char subject_wiz_note_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_wiz_note_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_wiz_note,
                           subject_wiz_note_buf,
                           subject_wiz_note_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           ""
                          );
    static char subject_wiz_next_buf[UI_SUBJECT_STRING_LENGTH];
    static char subject_wiz_next_prev_buf[UI_SUBJECT_STRING_LENGTH];
    lv_subject_init_string(&subject_wiz_next,
                           subject_wiz_next_buf,
                           subject_wiz_next_prev_buf,
                           UI_SUBJECT_STRING_LENGTH,
                           alarm_ui_tr("Start")
                          );
    lv_subject_init_int(&subject_wiz_next_on, 1);
    lv_subject_init_int(&subject_wiz_back, 0);
    lv_subject_init_int(&subject_wiz_exit, 0);
    lv_subject_init_int(&subject_wiz_pad, 0);

#if defined(LV_USE_XML) && LV_USE_XML
    /* Register fonts */
    lv_xml_register_font(NULL, "font_caption", font_caption);
    lv_xml_register_font(NULL, "font_body", font_body);
    lv_xml_register_font(NULL, "font_label", font_label);
    lv_xml_register_font(NULL, "font_title", font_title);
    lv_xml_register_font(NULL, "font_headline", font_headline);
    lv_xml_register_font(NULL, "font_keypad", font_keypad);
    lv_xml_register_font(NULL, "font_digits", font_digits);
    lv_xml_register_font(NULL, "icons_20", icons_20);
    lv_xml_register_font(NULL, "icons_28", icons_28);
    lv_xml_register_font(NULL, "icons_40", icons_40);
    lv_xml_register_font(NULL, "icons_96", icons_96);

    /* Register subjects */
    lv_xml_register_subject(NULL, "subject_view", &subject_view);
    lv_xml_register_subject(NULL, "subject_panel", &subject_panel);
    lv_xml_register_subject(NULL, "subject_alarm_active", &subject_alarm_active);
    lv_xml_register_subject(NULL, "subject_tone_color", &subject_tone_color);
    lv_xml_register_subject(NULL, "subject_pad_color", &subject_pad_color);
    lv_xml_register_subject(NULL, "subject_hero_icon", &subject_hero_icon);
    lv_xml_register_subject(NULL, "subject_headline", &subject_headline);
    lv_xml_register_subject(NULL, "subject_detail", &subject_detail);
    lv_xml_register_subject(NULL, "subject_hero_note", &subject_hero_note);
    lv_xml_register_subject(NULL, "subject_countdown", &subject_countdown);
    lv_xml_register_subject(NULL, "subject_countdown_pct", &subject_countdown_pct);
    lv_xml_register_subject(NULL, "subject_disarm_title", &subject_disarm_title);
    lv_xml_register_subject(NULL, "subject_disarm_note", &subject_disarm_note);
    lv_xml_register_subject(NULL, "subject_memory_note", &subject_memory_note);
    lv_xml_register_subject(NULL, "subject_walk_count", &subject_walk_count);
    lv_xml_register_subject(NULL, "subject_walk_left", &subject_walk_left);
    lv_xml_register_subject(NULL, "subject_walk_from", &subject_walk_from);
    lv_xml_register_subject(NULL, "subject_walk_from_on", &subject_walk_from_on);
    lv_xml_register_subject(NULL, "subject_walk_warn", &subject_walk_warn);
    lv_xml_register_subject(NULL, "subject_walk_banner", &subject_walk_banner);
    lv_xml_register_subject(NULL, "subject_date", &subject_date);
    lv_xml_register_subject(NULL, "subject_time", &subject_time);
    lv_xml_register_subject(NULL, "subject_ampm", &subject_ampm);
    lv_xml_register_subject(NULL, "subject_wifi_icon", &subject_wifi_icon);
    lv_xml_register_subject(NULL, "subject_battery_icon", &subject_battery_icon);
    lv_xml_register_subject(NULL, "subject_battery_text", &subject_battery_text);
    lv_xml_register_subject(NULL, "subject_battery_critical", &subject_battery_critical);
    lv_xml_register_subject(NULL, "subject_battery_warn", &subject_battery_warn);
    lv_xml_register_subject(NULL, "subject_wifi_ok", &subject_wifi_ok);
    lv_xml_register_subject(NULL, "subject_trouble_count", &subject_trouble_count);
    lv_xml_register_subject(NULL, "subject_trouble_text", &subject_trouble_text);
    lv_xml_register_subject(NULL, "subject_open_count", &subject_open_count);
    lv_xml_register_subject(NULL, "subject_devices_avail", &subject_devices_avail);
    lv_xml_register_subject(NULL, "subject_devices_note", &subject_devices_note);
    lv_xml_register_subject(NULL, "subject_chime", &subject_chime);
    lv_xml_register_subject(NULL, "subject_default_codes", &subject_default_codes);
    lv_xml_register_subject(NULL, "subject_chime_icon", &subject_chime_icon);
    lv_xml_register_subject(NULL, "subject_pad_title", &subject_pad_title);
    lv_xml_register_subject(NULL, "subject_pad_hint", &subject_pad_hint);
    lv_xml_register_subject(NULL, "subject_pad_glyph", &subject_pad_glyph);
    lv_xml_register_subject(NULL, "subject_pad_glyph_on", &subject_pad_glyph_on);
    lv_xml_register_subject(NULL, "subject_pad_note", &subject_pad_note);
    lv_xml_register_subject(NULL, "subject_pad_note_on", &subject_pad_note_on);
    lv_xml_register_subject(NULL, "subject_pad_close_on", &subject_pad_close_on);
    lv_xml_register_subject(NULL, "subject_modal_pad", &subject_modal_pad);
    lv_xml_register_subject(NULL, "subject_pad_dots_on", &subject_pad_dots_on);
    lv_xml_register_subject(NULL, "subject_pad_digits", &subject_pad_digits);
    lv_xml_register_subject(NULL, "subject_pad_error", &subject_pad_error);
    lv_xml_register_subject(NULL, "subject_pad_options", &subject_pad_options);
    lv_xml_register_subject(NULL, "subject_pad_silence", &subject_pad_silence);
    lv_xml_register_subject(NULL, "subject_opt_instant", &subject_opt_instant);
    lv_xml_register_subject(NULL, "subject_opt_silent", &subject_opt_silent);
    lv_xml_register_subject(NULL, "subject_opt_silent_avail", &subject_opt_silent_avail);
    lv_xml_register_subject(NULL, "subject_locked", &subject_locked);
    lv_xml_register_subject(NULL, "subject_opt_bypass", &subject_opt_bypass);
    lv_xml_register_subject(NULL, "subject_opt_bypass_avail", &subject_opt_bypass_avail);
    lv_xml_register_subject(NULL, "subject_toast", &subject_toast);
    lv_xml_register_subject(NULL, "subject_toast_visible", &subject_toast_visible);
    lv_xml_register_subject(NULL, "subject_zones_note", &subject_zones_note);
    lv_xml_register_subject(NULL, "subject_zone_filter", &subject_zone_filter);
    lv_xml_register_subject(NULL, "subject_events_note", &subject_events_note);
    lv_xml_register_subject(NULL, "subject_event_filter", &subject_event_filter);
    lv_xml_register_subject(NULL, "subject_troubles_note", &subject_troubles_note);
    lv_xml_register_subject(NULL, "subject_troubles_empty", &subject_troubles_empty);
    lv_xml_register_subject(NULL, "subject_zones_empty", &subject_zones_empty);
    lv_xml_register_subject(NULL, "subject_troubles_pager", &subject_troubles_pager);
    lv_xml_register_subject(NULL, "subject_troubles_page", &subject_troubles_page);
    lv_xml_register_subject(NULL, "subject_panic_report", &subject_panic_report);
    lv_xml_register_subject(NULL, "subject_bar_banner", &subject_bar_banner);
    lv_xml_register_subject(NULL, "subject_update_banner", &subject_update_banner);
    lv_xml_register_subject(NULL, "subject_update_banner_glyph", &subject_update_banner_glyph);
    lv_xml_register_subject(NULL, "subject_remote_banner", &subject_remote_banner);
    lv_xml_register_subject(NULL, "subject_arm_banner", &subject_arm_banner);
    lv_xml_register_subject(NULL, "subject_arm_strip", &subject_arm_strip);
    lv_xml_register_subject(NULL, "subject_arm_strip_glyph", &subject_arm_strip_glyph);
    lv_xml_register_subject(NULL, "subject_arm_strip_color", &subject_arm_strip_color);
    lv_xml_register_subject(NULL, "subject_arm_strip_title", &subject_arm_strip_title);
    lv_xml_register_subject(NULL, "subject_arm_strip_detail", &subject_arm_strip_detail);
    lv_xml_register_subject(NULL, "subject_arm_strip_note", &subject_arm_strip_note);
    lv_xml_register_subject(NULL, "subject_arm_strip_note_on", &subject_arm_strip_note_on);
    lv_xml_register_subject(NULL, "subject_poff_hour", &subject_poff_hour);
    lv_xml_register_subject(NULL, "subject_poff_night_title", &subject_poff_night_title);
    lv_xml_register_subject(NULL, "subject_poff_night", &subject_poff_night);
    lv_xml_register_subject(NULL, "subject_link_role", &subject_link_role);
    lv_xml_register_subject(NULL, "subject_link_home", &subject_link_home);
    lv_xml_register_subject(NULL, "subject_panic_hint", &subject_panic_hint);
    lv_xml_register_subject(NULL, "subject_panic_link", &subject_panic_link);
    lv_xml_register_subject(NULL, "subject_settings_section", &subject_settings_section);
    lv_xml_register_subject(NULL, "subject_automations_avail", &subject_automations_avail);
    lv_xml_register_subject(NULL, "subject_update_waiting", &subject_update_waiting);
    lv_xml_register_subject(NULL, "subject_settings_pad", &subject_settings_pad);
    lv_xml_register_subject(NULL, "subject_page_title", &subject_page_title);
    lv_xml_register_subject(NULL, "subject_page_note", &subject_page_note);
    lv_xml_register_subject(NULL, "subject_page_back", &subject_page_back);
    lv_xml_register_subject(NULL, "subject_busy", &subject_busy);
    lv_xml_register_subject(NULL, "subject_sheet", &subject_sheet);
    lv_xml_register_subject(NULL, "subject_sheet_tone", &subject_sheet_tone);
    lv_xml_register_subject(NULL, "subject_sheet_title", &subject_sheet_title);
    lv_xml_register_subject(NULL, "subject_sheet_note", &subject_sheet_note);
    lv_xml_register_subject(NULL, "subject_sheet_ok", &subject_sheet_ok);
    lv_xml_register_subject(NULL, "subject_sheet_cancel", &subject_sheet_cancel);
    lv_xml_register_subject(NULL, "subject_sheet_eye", &subject_sheet_eye);
    lv_xml_register_subject(NULL, "subject_sheet_shown", &subject_sheet_shown);
    lv_xml_register_subject(NULL, "subject_wiz_step", &subject_wiz_step);
    lv_xml_register_subject(NULL, "subject_wiz_steps", &subject_wiz_steps);
    lv_xml_register_subject(NULL, "subject_wiz_icon", &subject_wiz_icon);
    lv_xml_register_subject(NULL, "subject_wiz_title", &subject_wiz_title);
    lv_xml_register_subject(NULL, "subject_wiz_note", &subject_wiz_note);
    lv_xml_register_subject(NULL, "subject_wiz_next", &subject_wiz_next);
    lv_xml_register_subject(NULL, "subject_wiz_next_on", &subject_wiz_next_on);
    lv_xml_register_subject(NULL, "subject_wiz_back", &subject_wiz_back);
    lv_xml_register_subject(NULL, "subject_wiz_exit", &subject_wiz_exit);
    lv_xml_register_subject(NULL, "subject_wiz_pad", &subject_wiz_pad);
#endif

#if defined(LV_USE_XML) && LV_USE_XML == 0
    /*--------------------
     *  Permanent screens
     *-------------------*/
    /* To allow screens to reference each other, create them all before calling the sceen create functions */
    screen_main = lv_obj_create(NULL);
    screen_saver = lv_obj_create(NULL);
    screen_wizard = lv_obj_create(NULL);

    screen_main_create();
    screen_saver_create();
    screen_wizard_create();
#endif
}

void alarm_ui_set_target(uint32_t target)
{
    alarm_ui_target = target;
}

uint32_t alarm_ui_get_target(void)
{
    return alarm_ui_target;
}

bool alarm_ui_check_target(uint32_t target)
{
    return (alarm_ui_target & target) ? true : false;
}
