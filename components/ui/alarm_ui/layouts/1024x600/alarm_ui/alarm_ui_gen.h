/**
 * @file alarm_ui_gen.h
 */

#ifndef ALARM_UI_GEN_H
#define ALARM_UI_GEN_H

#ifndef UI_SUBJECT_STRING_LENGTH
#define UI_SUBJECT_STRING_LENGTH 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
    #include "lvgl_private.h"
#else
    #include "lvgl/lvgl.h"
    #include "lvgl/lvgl_private.h"
#endif

#if defined(LV_USE_XML) && LV_USE_XML
    #include "lv_xml/lv_xml.h"
#endif

/* Prototypes for target functions, needed by responsive const definitions */

void alarm_ui_set_target(uint32_t target);
uint32_t alarm_ui_get_target(void);
bool alarm_ui_check_target(uint32_t target);

/*********************
 *      DEFINES
 *********************/

#define ALARM_UI_TARGET_UNDEFINED  (0 << 1)
#define ALARM_UI_TARGET_CROWPANEL_7IN    (1 << 1)
#define ALARM_UI_TARGET_ALL        0x0FFFFFFF

/* By default compile for all targets, allowing to switch to any targets at runtime */
#ifndef ALARM_UI_COMPILE_TARGET
#define ALARM_UI_COMPILE_TARGET ALARM_UI_TARGET_ALL
#endif

#define ALARM_UI_CHECK_COMPILE_TARGET(target) (ALARM_UI_COMPILE_TARGET & (target) ? 1 : 0)

/*----------------
 * Appearance (themes.xml): COLOR_* read the current palette
 *----------------*/

#define ALARM_UI_THEMES 1 /* the tables below are here (tools/lvpro_gen.py, not the editor) */

typedef enum {
    ALARM_UI_COLOR_BG = 0,
    ALARM_UI_COLOR_RAIL,
    ALARM_UI_COLOR_SURFACE,
    ALARM_UI_COLOR_RAISED,
    ALARM_UI_COLOR_PRESSED,
    ALARM_UI_COLOR_LINE,
    ALARM_UI_COLOR_TEXT,
    ALARM_UI_COLOR_TEXT_2,
    ALARM_UI_COLOR_TEXT_3,
    ALARM_UI_COLOR_GREEN,
    ALARM_UI_COLOR_AMBER,
    ALARM_UI_COLOR_BLUE,
    ALARM_UI_COLOR_VIOLET,
    ALARM_UI_COLOR_ROSE,
    ALARM_UI_COLOR_ORANGE,
    ALARM_UI_COLOR_RED,
    ALARM_UI_COLOR_ALARM_BG,
    ALARM_UI_COLOR_ON_TONE,
    ALARM_UI_COLOR_ALARM_CARD,
    ALARM_UI_COLOR_TOAST_LINE,
    ALARM_UI_COLOR_KNOB,
    ALARM_UI_COLOR_KEY_SPECIAL,
    ALARM_UI_COLOR_COUNT
} alarm_ui_color_t;

typedef enum {
    ALARM_UI_THEME_DARK = 0, /* Dark */
    ALARM_UI_THEME_LIGHT, /* Light */
    ALARM_UI_THEME_CONTRAST, /* High contrast */
    ALARM_UI_THEME_CONTRAST_LIGHT, /* High contrast, light */
    ALARM_UI_THEME_COUNT
} alarm_ui_theme_t;

typedef enum {
    ALARM_UI_STYLE_SCREEN = 0,
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
    ALARM_UI_STYLE_BADGE_BADGE,
    ALARM_UI_STYLE_CHIP_CHIP,
    ALARM_UI_STYLE_CHIP_CHIP_CHECKED,
    ALARM_UI_STYLE_ICON_CHIP_ICON_CHIP,
    ALARM_UI_STYLE_SEGMENT_SEGMENT,
    ALARM_UI_STYLE_SEGMENT_SEGMENT_PRESSED,
    ALARM_UI_STYLE_DEVICE_KEY_DEVICE_KEY,
    ALARM_UI_STYLE_DEVICE_KEY_DEVICE_KEY_PRESSED,
    ALARM_UI_STYLE_DEVICE_LEVEL_DEVICE_LEVEL,
    ALARM_UI_STYLE_DEVICE_LEVEL_DEVICE_LEVEL_FILL,
    ALARM_UI_STYLE_DEVICE_LEVEL_DEVICE_LEVEL_EDGE,
    ALARM_UI_STYLE_DEVICE_SETPOINT_DEVICE_STEP_BIG,
    ALARM_UI_STYLE_DEVICE_SETPOINT_DEVICE_STEP_BIG_PRESSED,
    ALARM_UI_STYLE_DEVICE_STEPPER_DEVICE_STEP,
    ALARM_UI_STYLE_DEVICE_STEPPER_DEVICE_STEP_PRESSED,
    ALARM_UI_STYLE_DEVICE_TILE_DEVICE_TILE,
    ALARM_UI_STYLE_DEVICE_TILE_DEVICE_TILE_PRESSED,
    ALARM_UI_STYLE_ACTION_PAIR_ACTION_PAIR,
    ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL,
    ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_PRESSED,
    ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_AWAY,
    ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_STAY,
    ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_NIGHT,
    ALARM_UI_STYLE_BIG_VALUE_BIG_VALUE,
    ALARM_UI_STYLE_HOLD_BUTTON_HOLD_BUTTON,
    ALARM_UI_STYLE_HOLD_BUTTON_HOLD_FILL,
    ALARM_UI_STYLE_HOLD_BUTTON_HOLD_FILL_INDICATOR,
    ALARM_UI_STYLE_LIST_ROW_LIST_ROW,
    ALARM_UI_STYLE_LIST_ROW_LIST_ROW_PRESSED,
    ALARM_UI_STYLE_LIST_ROW_LIST_ROW_LEVEL,
    ALARM_UI_STYLE_LIST_ROW_LIST_ROW_LEVEL_INDICATOR,
    ALARM_UI_STYLE_ROW_BUTTON_ROW_BUTTON,
    ALARM_UI_STYLE_ROW_BUTTON_ROW_BUTTON_PRESSED,
    ALARM_UI_STYLE_ROW_SLIDER_ROW_SLIDER,
    ALARM_UI_STYLE_ROW_SLIDER_SLIDER,
    ALARM_UI_STYLE_ROW_SLIDER_SLIDER_INDICATOR,
    ALARM_UI_STYLE_ROW_SLIDER_SLIDER_KNOB,
    ALARM_UI_STYLE_ROW_STEPPER_ROW_STEPPER,
    ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON,
    ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON_PRESSED,
    ALARM_UI_STYLE_ROW_SWITCH_ROW_SETTING,
    ALARM_UI_STYLE_ROW_SWITCH_SWITCH,
    ALARM_UI_STYLE_ROW_SWITCH_SWITCH_INDICATOR,
    ALARM_UI_STYLE_ROW_SWITCH_SWITCH_ON,
    ALARM_UI_STYLE_ROW_SWITCH_SWITCH_KNOB,
    ALARM_UI_STYLE_ROW_SWITCH_SWITCH_KNOB_ON,
    ALARM_UI_STYLE_ROW_SWITCH_ICON_ROW_SETTING_ICON,
    ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON,
    ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_INDICATOR,
    ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_ON,
    ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_KNOB,
    ALARM_UI_STYLE_ROW_SWITCH_ICON_SWITCH_ICON_KNOB_ON,
    ALARM_UI_STYLE_ROW_VALUE_ROW_VALUE,
    ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE,
    ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE_PRESSED,
    ALARM_UI_STYLE_ARM_STRIP_ARM_STRIP,
    ALARM_UI_STYLE_ARM_TILE_ARM_TILE,
    ALARM_UI_STYLE_ARM_TILE_ARM_TILE_PRESSED,
    ALARM_UI_STYLE_HERO_HERO,
    ALARM_UI_STYLE_HERO_RING_TRACK,
    ALARM_UI_STYLE_HERO_RING_INDICATOR,
    ALARM_UI_STYLE_HERO_DISK,
    ALARM_UI_STYLE_HERO_HERO_ALARM,
    ALARM_UI_STYLE_EVENT_ROW_EVENT_ROW,
    ALARM_UI_STYLE_TROUBLE_ROW_TROUBLE_ROW,
    ALARM_UI_STYLE_ZONE_CARD_ZONE_CARD,
    ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS,
    ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS_PRESSED,
    ALARM_UI_STYLE_CODE_PAD_PAD,
    ALARM_UI_STYLE_CODE_PAD_PAD_ICON,
    ALARM_UI_STYLE_CODE_PAD_PAD_HINT_UNDER_DOTS,
    ALARM_UI_STYLE_CODE_PAD_PAD_WORDS_ALONE,
    ALARM_UI_STYLE_CODE_PAD_PAD_DOT,
    ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE,
    ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE_PRESSED,
    ALARM_UI_STYLE_CODE_PAD_PAD_KEYS,
    ALARM_UI_STYLE_CODE_PAD_PAD_ENTER,
    ALARM_UI_STYLE_CODE_PAD_PAD_SILENCE,
    ALARM_UI_STYLE_CODE_PAD_PAD_SILENCE_PRESSED,
    ALARM_UI_STYLE_PAD_KEY_PAD_KEY,
    ALARM_UI_STYLE_PAD_KEY_PAD_KEY_PRESSED,
    ALARM_UI_STYLE_PAD_KEY_PAD_KEY_SHORT,
    ALARM_UI_STYLE_PAD_KEY_PAD_KEY_DISABLED,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_RING,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_RING_INDICATOR,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_BUTTON,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_BUTTON_PRESSED,
    ALARM_UI_STYLE_CAL_DAY_CAL_DAY,
    ALARM_UI_STYLE_CAL_DAY_CAL_DAY_PRESSED,
    ALARM_UI_STYLE_CAL_DAY_CAL_DAY_TODAY,
    ALARM_UI_STYLE_CAL_DAY_CAL_DAY_CHOSEN,
    ALARM_UI_STYLE_DAY_TOGGLE_DAY_TOGGLE,
    ALARM_UI_STYLE_DAY_TOGGLE_DAY_TOGGLE_PRESSED,
    ALARM_UI_STYLE_DAY_TOGGLE_DAY_TOGGLE_ON,
    ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROUND,
    ALARM_UI_STYLE_MONTH_CALENDAR_CAL_WEEK,
    ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROW,
    ALARM_UI_STYLE_SETTINGS_ITEM_SETTINGS_ITEM,
    ALARM_UI_STYLE_SETTINGS_ITEM_SETTINGS_ITEM_PRESSED,
    ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH,
    ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH_INDICATOR,
    ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH_ON,
    ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH_KNOB,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD_FOCUSED,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_CURSOR,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_PLACEHOLDER,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD_EYE,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_EYE,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_EYE_PRESSED,
    ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD,
    ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY,
    ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY_PRESSED,
    ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY_SPECIAL,
    ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY_ENTER,
    ALARM_UI_STYLE_COUNT
} alarm_ui_style_id_t;

typedef enum {
    ALARM_UI_VALUE_NONE = 0, /* the style has none: the property is removed */
    ALARM_UI_VALUE_INT,      /* a number */
    ALARM_UI_VALUE_COLOR,    /* a palette index: ALARM_UI_COLOR_* */
    ALARM_UI_VALUE_HEX,      /* a fixed colour, 0xRRGGBB */
} alarm_ui_value_kind_t;

/* A property a style takes in some themes only (themes.xml's <styles themes=...>) */
typedef struct {
    uint16_t style;        /* ALARM_UI_STYLE_* */
    lv_style_prop_t prop;
    uint8_t themes;        /* 1 << ALARM_UI_THEME_* of each theme it is in */
    uint8_t kind;          /* alarm_ui_value_kind_t */
    uint8_t base_kind;     /* the style's own value, in the other themes */
    int32_t value;
    int32_t base;
} alarm_ui_style_override_t;

extern const lv_color_t alarm_ui_palettes[ALARM_UI_THEME_COUNT][ALARM_UI_COLOR_COUNT];
extern const lv_color_t * alarm_ui_palette; /* the current theme's */
extern const uint8_t alarm_ui_theme_saver[ALARM_UI_THEME_COUNT]; /* the theme the screen saver keeps */
extern const bool alarm_ui_theme_high_contrast[ALARM_UI_THEME_COUNT];
extern lv_style_t * alarm_ui_styles[ALARM_UI_STYLE_COUNT]; /* each style, once it is initialised */
extern const alarm_ui_style_override_t alarm_ui_style_overrides[];
extern const uint32_t alarm_ui_style_override_count;
extern lv_subject_t * const alarm_ui_color_subjects[];
extern const uint32_t alarm_ui_color_subject_count;

/* In the hand-written alarm_ui.c: a style joins the registry and takes the theme's changes */
void alarm_ui_style_track(lv_style_t * style, uint32_t id);

/**
 * Tightest spacing
 */
#define SPACE_XS 4
/**
 * Small spacing
 */
#define SPACE_SM 8
/**
 * Default gap between related items
 */
#define SPACE_MD 12
/**
 * Gap between items and card padding on small cards
 */
#define SPACE_LG 16
/**
 * Card padding and gap between cards
 */
#define SPACE_XL 24
/**
 * Section spacing
 */
#define SPACE_XXL 32
/**
 * Small rounding (chips inside cards)
 */
#define RADIUS_SM 12
/**
 * Buttons and keys
 */
#define RADIUS_MD 18
/**
 * Cards and tiles
 */
#define RADIUS_LG 24
/**
 * Large panels
 */
#define RADIUS_XL 32
/**
 * Pills and circles
 */
#define RADIUS_FULL 32767
/**
 * Navigation rail width
 */
#define RAIL_W 96
/**
 * Status bar height
 */
#define BAR_H 64
/**
 * Screen width minus the rail
 */
#define CONTENT_W 928
/**
 * Screen height minus the status bar
 */
#define CONTENT_H 536
/**
 * Width of one of the two home panes
 */
#define PANE_W 428
/**
 * A choice sheet is as tall as its choices, up to this, then it scrolls
 */
#define PICK_MAX_H 520
/**
 * Smart Home: one cell of the tile grid, four across the view (BL-045)
 */
#define DEVICE_CELL_W 208
/**
 * Smart Home: a row of tiles; three rows fill the view
 */
#define DEVICE_CELL_H 132
/**
 * Smart Home: a tile two cells wide (two cells and the gap between)
 */
#define DEVICE_WIDE_W 432
/**
 * Smart Home: the gap between tiles
 */
#define DEVICE_GAP 16
/**
 * Smart Home, arranging: the length of a dash in a tile's dashed edge
 */
#define DEVICE_DASH_W 8
/**
 * Smart Home, arranging: the gap between the dashes
 */
#define DEVICE_DASH_GAP 6
/**
 * Smart Home, arranging: the width of a tile's dashed edge
 */
#define DEVICE_EDGE_W 2
/**
 * Smart Home, arranging: the grip's inset from a tile's right edge
 */
#define DEVICE_GRIP_X 6
/**
 * Smart Home, arranging: the grip's drop from a tile's top
 */
#define DEVICE_GRIP_Y 14
/**
 * Smart Home: a tile's name, one line of font_label (its line height; BL-184): a one-cell tile's, and a wide one's with a slider, keys or a stepper
 */
#define DEVICE_NAME_H 26
/**
 * Smart Home: a wide tile's name without a control under it, two lines of font_label; LVGL ends the last line with ... (BL-184)
 */
#define DEVICE_NAME_TWO_H 52
/**
 * Smart Home: a tile's state in words, one line of font_caption (its line height; BL-184)
 */
#define DEVICE_STATE_H 19
/**
 * Toast: offset from the screen's centre (the content's centre, right of the rail)
 */
#define TOAST_X 48
/**
 * Toast: offset from the bottom
 */
#define TOAST_Y -28
/**
 * Toast while the keyboard is open: offset from the top, clear of the keys
 */
#define TOAST_KB_Y 76
/**
 * Calendar on Set the date and time
 */
#define CAL_W 392
/**
 * A day in that calendar
 */
#define CAL_CELL_H 44
/**
 * Calendar on the setup's clock step
 */
#define CAL_W_SETUP 320
/**
 * A day in that calendar
 */
#define CAL_CELL_H_SETUP 40
/**
 * An Hour or Minute roller on the sheet
 */
#define ROLLER_W 88
/**
 * The AM/PM roller on the sheet
 */
#define ROLLER_AMPM_W 80
/**
 * An Hour or Minute roller on the setup's step
 */
#define ROLLER_W_SETUP 76
/**
 * The AM/PM roller on the setup's step
 */
#define ROLLER_AMPM_W_SETUP 72
/**
 * A roller: five rows on show
 */
#define ROLLER_H 240
/**
 * One row of a roller (ui_net.c sets the line space to it)
 */
#define ROLLER_ROW_H 48
/**
 * Screen background
 */
#define COLOR_BG (alarm_ui_palette[ALARM_UI_COLOR_BG])
/**
 * Navigation rail
 */
#define COLOR_RAIL (alarm_ui_palette[ALARM_UI_COLOR_RAIL])
/**
 * Cards and tiles
 */
#define COLOR_SURFACE (alarm_ui_palette[ALARM_UI_COLOR_SURFACE])
/**
 * Keys, buttons, selected items
 */
#define COLOR_RAISED (alarm_ui_palette[ALARM_UI_COLOR_RAISED])
/**
 * Pressed keys and buttons
 */
#define COLOR_PRESSED (alarm_ui_palette[ALARM_UI_COLOR_PRESSED])
/**
 * Card borders and separators
 */
#define COLOR_LINE (alarm_ui_palette[ALARM_UI_COLOR_LINE])
/**
 * Primary text
 */
#define COLOR_TEXT (alarm_ui_palette[ALARM_UI_COLOR_TEXT])
/**
 * Secondary text
 */
#define COLOR_TEXT_2 (alarm_ui_palette[ALARM_UI_COLOR_TEXT_2])
/**
 * Tertiary text, hints
 */
#define COLOR_TEXT_3 (alarm_ui_palette[ALARM_UI_COLOR_TEXT_3])
/**
 * Disarmed and ready
 */
#define COLOR_GREEN (alarm_ui_palette[ALARM_UI_COLOR_GREEN])
/**
 * Not ready, troubles, alarm memory
 */
#define COLOR_AMBER (alarm_ui_palette[ALARM_UI_COLOR_AMBER])
/**
 * Armed Stay
 */
#define COLOR_BLUE (alarm_ui_palette[ALARM_UI_COLOR_BLUE])
/**
 * Armed Night
 */
#define COLOR_VIOLET (alarm_ui_palette[ALARM_UI_COLOR_VIOLET])
/**
 * Armed Away
 */
#define COLOR_ROSE (alarm_ui_palette[ALARM_UI_COLOR_ROSE])
/**
 * Entry delay: disarm now
 */
#define COLOR_ORANGE (alarm_ui_palette[ALARM_UI_COLOR_ORANGE])
/**
 * Alarms
 */
#define COLOR_RED (alarm_ui_palette[ALARM_UI_COLOR_RED])
/**
 * Screen background during an alarm
 */
#define COLOR_ALARM_BG (alarm_ui_palette[ALARM_UI_COLOR_ALARM_BG])
/**
 * Text drawn on a tone colour
 */
#define COLOR_ON_TONE (alarm_ui_palette[ALARM_UI_COLOR_ON_TONE])
/**
 * The state card during an alarm
 */
#define COLOR_ALARM_CARD (alarm_ui_palette[ALARM_UI_COLOR_ALARM_CARD])
/**
 * A toast's edge
 */
#define COLOR_TOAST_LINE (alarm_ui_palette[ALARM_UI_COLOR_TOAST_LINE])
/**
 * A switch's and a slider's knob
 */
#define COLOR_KNOB (alarm_ui_palette[ALARM_UI_COLOR_KNOB])
/**
 * The keyboard's special keys
 */
#define COLOR_KEY_SPECIAL (alarm_ui_palette[ALARM_UI_COLOR_KEY_SPECIAL])
/**
 * Tinted backgrounds (tone at 16 %)
 */
#define OPA_TINT (255 * 16 / 100)
/**
 * Smart Home: an offline device's chip (its words stay at full opacity, BL-181)
 */
#define OPA_DEVICE_OFFLINE_CHIP (255 * 70 / 100)

/**********************
 * GLOBAL VARIABLES
 **********************/

/*-------------------
 * Permanent screens
 *------------------*/

extern lv_obj_t * screen_main;
extern lv_obj_t * screen_saver;
extern lv_obj_t * screen_wizard;

/*----------------
 * Global styles
 *----------------*/

extern lv_style_t style_screen;
extern lv_style_t style_screen_alarm;
extern lv_style_t style_saver;
extern lv_style_t style_card;
extern lv_style_t style_pressable;
extern lv_style_t style_knob_off;
extern lv_style_t style_text_2;
extern lv_style_t style_text_3;
extern lv_style_t style_text_error;
extern lv_style_t style_text_warn;
extern lv_style_t style_nav_item;
extern lv_style_t style_nav_item_pressed;
extern lv_style_t style_nav_active;
extern lv_style_t style_nav_on;
extern lv_style_t style_device_arrange;
extern lv_style_t style_device_picked;
extern lv_style_t style_device_still;
extern lv_style_t style_segment_active;
extern lv_style_t style_button;
extern lv_style_t style_button_pressed;
extern lv_style_t style_button_primary;
extern lv_style_t style_button_danger;
extern lv_style_t style_button_tone_pressed;
extern lv_style_t style_disabled;
extern lv_style_t style_bar_round;
extern lv_style_t style_bar_pill;
extern lv_style_t style_bar_pill_active;
extern lv_style_t style_backdrop;
extern lv_style_t style_sheet_card;
extern lv_style_t style_roller;
extern lv_style_t style_roller_selected;
extern lv_style_t style_step;
extern lv_style_t style_step_on;

/*----------------
 * Fonts
 *----------------*/

/* Targets: any */
extern lv_font_t * font_caption;
extern lv_font_t * font_body;
extern lv_font_t * font_label;
extern lv_font_t * font_title;
extern lv_font_t * font_headline;
extern lv_font_t * font_keypad;
extern lv_font_t * font_digits;
extern lv_font_t * icons_20;
extern lv_font_t * icons_28;
extern lv_font_t * icons_40;
extern lv_font_t * icons_96;

/* Fingerprint of the letters pack's fonts: a pack whose `match` equals it was made
 * from these fonts (tools/lvpro_gen.py, docs/features/39 OPT-010) */
extern const uint32_t alarm_ui_letters_fingerprint;

/*----------------
 * Subjects
 *----------------*/

extern lv_subject_t subject_view;
extern lv_subject_t subject_panel;
extern lv_subject_t subject_alarm_active;
extern lv_subject_t subject_tone_color;
extern lv_subject_t subject_pad_color;
extern lv_subject_t subject_hero_icon;
extern lv_subject_t subject_headline;
extern lv_subject_t subject_detail;
extern lv_subject_t subject_hero_note;
extern lv_subject_t subject_countdown;
extern lv_subject_t subject_countdown_pct;
extern lv_subject_t subject_disarm_title;
extern lv_subject_t subject_disarm_note;
extern lv_subject_t subject_memory_note;
extern lv_subject_t subject_walk_count;
extern lv_subject_t subject_walk_left;
extern lv_subject_t subject_walk_from;
extern lv_subject_t subject_walk_from_on;
extern lv_subject_t subject_walk_warn;
extern lv_subject_t subject_walk_banner;
extern lv_subject_t subject_date;
extern lv_subject_t subject_time;
extern lv_subject_t subject_ampm;
extern lv_subject_t subject_wifi_icon;
extern lv_subject_t subject_battery_icon;
extern lv_subject_t subject_battery_text;
extern lv_subject_t subject_battery_critical;
extern lv_subject_t subject_battery_warn;
extern lv_subject_t subject_wifi_ok;
extern lv_subject_t subject_trouble_count;
extern lv_subject_t subject_trouble_text;
extern lv_subject_t subject_open_count;
extern lv_subject_t subject_devices_avail;
extern lv_subject_t subject_devices_note;
extern lv_subject_t subject_chime;
extern lv_subject_t subject_default_codes;
extern lv_subject_t subject_chime_icon;
extern lv_subject_t subject_pad_title;
extern lv_subject_t subject_pad_hint;
extern lv_subject_t subject_pad_glyph;
extern lv_subject_t subject_pad_glyph_on;
extern lv_subject_t subject_pad_note;
extern lv_subject_t subject_pad_note_on;
extern lv_subject_t subject_pad_close_on;
extern lv_subject_t subject_modal_pad;
extern lv_subject_t subject_pad_dots_on;
extern lv_subject_t subject_pad_digits;
extern lv_subject_t subject_pad_error;
extern lv_subject_t subject_pad_options;
extern lv_subject_t subject_pad_silence;
extern lv_subject_t subject_opt_instant;
extern lv_subject_t subject_opt_silent;
extern lv_subject_t subject_opt_silent_avail;
extern lv_subject_t subject_locked;
extern lv_subject_t subject_settings_locked;
extern lv_subject_t subject_opt_bypass;
extern lv_subject_t subject_opt_bypass_avail;
extern lv_subject_t subject_toast;
extern lv_subject_t subject_toast_visible;
extern lv_subject_t subject_zones_note;
extern lv_subject_t subject_zone_filter;
extern lv_subject_t subject_events_note;
extern lv_subject_t subject_event_filter;
extern lv_subject_t subject_troubles_note;
extern lv_subject_t subject_troubles_empty;
extern lv_subject_t subject_zones_empty;
extern lv_subject_t subject_troubles_pager;
extern lv_subject_t subject_troubles_page;
extern lv_subject_t subject_panic_report;
extern lv_subject_t subject_bar_banner;
extern lv_subject_t subject_update_banner;
extern lv_subject_t subject_update_banner_glyph;
extern lv_subject_t subject_remote_banner;
extern lv_subject_t subject_arm_banner;
extern lv_subject_t subject_arm_strip;
extern lv_subject_t subject_arm_strip_glyph;
extern lv_subject_t subject_arm_strip_color;
extern lv_subject_t subject_arm_strip_title;
extern lv_subject_t subject_arm_strip_detail;
extern lv_subject_t subject_arm_strip_note;
extern lv_subject_t subject_arm_strip_note_on;
extern lv_subject_t subject_poff_hour;
extern lv_subject_t subject_poff_night_title;
extern lv_subject_t subject_poff_night;
extern lv_subject_t subject_link_role;
extern lv_subject_t subject_link_home;
extern lv_subject_t subject_panic_hint;
extern lv_subject_t subject_panic_link;
extern lv_subject_t subject_settings_section;
extern lv_subject_t subject_automations_avail;
extern lv_subject_t subject_update_waiting;
extern lv_subject_t subject_settings_pad;
extern lv_subject_t subject_page_title;
extern lv_subject_t subject_page_note;
extern lv_subject_t subject_page_back;
extern lv_subject_t subject_busy;
extern lv_subject_t subject_sheet;
extern lv_subject_t subject_sheet_tone;
extern lv_subject_t subject_sheet_title;
extern lv_subject_t subject_sheet_note;
extern lv_subject_t subject_sheet_ok;
extern lv_subject_t subject_sheet_cancel;
extern lv_subject_t subject_sheet_eye;
extern lv_subject_t subject_sheet_shown;
extern lv_subject_t subject_wiz_step;
extern lv_subject_t subject_wiz_steps;
extern lv_subject_t subject_wiz_icon;
extern lv_subject_t subject_wiz_title;
extern lv_subject_t subject_wiz_note;
extern lv_subject_t subject_wiz_next;
extern lv_subject_t subject_wiz_next_on;
extern lv_subject_t subject_wiz_back;
extern lv_subject_t subject_wiz_exit;
extern lv_subject_t subject_wiz_pad;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/*----------------
 * Event Callbacks
 *----------------*/

void ui_on_zone_bypass(lv_event_t * e);
void ui_on_pad_close(lv_event_t * e);
void ui_on_silence(lv_event_t * e);
void ui_on_pad_key(lv_event_t * e);
void ui_on_sheet_cancel(lv_event_t * e);
void ui_on_sheet_done(lv_event_t * e);
void ui_on_sheet_eye(lv_event_t * e);
void ui_on_program_exit(lv_event_t * e);
void ui_on_change_code(lv_event_t * e);
void ui_on_end_walk_test(lv_event_t * e);
void ui_on_remote_banner(lv_event_t * e);
void ui_on_chime(lv_event_t * e);
void ui_on_settings(lv_event_t * e);
void ui_on_arm(lv_event_t * e);
void ui_on_disarm(lv_event_t * e);
void ui_on_clear_memory(lv_event_t * e);
void ui_on_ack_troubles(lv_event_t * e);
void ui_on_troubles_page(lv_event_t * e);
void ui_on_panic(lv_event_t * e);
void ui_on_settings_section(lv_event_t * e);
void ui_on_page_back(lv_event_t * e);
void ui_on_wake(lv_event_t * e);
void ui_on_wizard_exit(lv_event_t * e);
void ui_on_wizard_back(lv_event_t * e);
void ui_on_wizard_next(lv_event_t * e);

/**
 * Initialize the component library
 */

void alarm_ui_init_gen(const char * asset_path);

/**
 * Text for people in the XML goes through this lookup (the portable UI defines it)
 */

const char * alarm_ui_tr(const char * english);
const char * alarm_ui_tr_c(const char * ctx, const char * english);

/**********************
 *   POST INCLUDES
 **********************/

/*Include all the widgets, components and screens of this library*/
#include "alarm_ui_runs_gen.h"
#include "components/base/box/box_gen.h"
#include "components/common/badge/badge_gen.h"
#include "components/common/chip/chip_gen.h"
#include "components/common/icon_chip/icon_chip_gen.h"
#include "components/common/segment/segment_gen.h"
#include "components/common/view_header/view_header_gen.h"
#include "components/devices/device_key/device_key_gen.h"
#include "components/devices/device_keys/device_keys_gen.h"
#include "components/devices/device_level/device_level_gen.h"
#include "components/devices/device_setpoint/device_setpoint_gen.h"
#include "components/devices/device_stepper/device_stepper_gen.h"
#include "components/devices/device_tile/device_tile_gen.h"
#include "components/forms/action_pair/action_pair_gen.h"
#include "components/forms/arm_levels/arm_levels_gen.h"
#include "components/forms/big_value/big_value_gen.h"
#include "components/forms/hold_button/hold_button_gen.h"
#include "components/forms/list_row/list_row_gen.h"
#include "components/forms/result_line/result_line_gen.h"
#include "components/forms/row_button/row_button_gen.h"
#include "components/forms/row_slider/row_slider_gen.h"
#include "components/forms/row_stepper/row_stepper_gen.h"
#include "components/forms/row_switch/row_switch_gen.h"
#include "components/forms/row_switch_icon/row_switch_icon_gen.h"
#include "components/forms/row_value/row_value_gen.h"
#include "components/forms/section_title/section_title_gen.h"
#include "components/home/action_tile/action_tile_gen.h"
#include "components/home/arm_strip/arm_strip_gen.h"
#include "components/home/arm_tile/arm_tile_gen.h"
#include "components/home/hero/hero_gen.h"
#include "components/lists/event_row/event_row_gen.h"
#include "components/lists/trouble_row/trouble_row_gen.h"
#include "components/lists/zone_card/zone_card_gen.h"
#include "components/nav/nav_item/nav_item_gen.h"
#include "components/nav/nav_toggle/nav_toggle_gen.h"
#include "components/pad/code_pad/code_pad_gen.h"
#include "components/pad/pad_key/pad_key_gen.h"
#include "components/panic/panic_button/panic_button_gen.h"
#include "components/pickers/cal_day/cal_day_gen.h"
#include "components/pickers/clock_picker/clock_picker_gen.h"
#include "components/pickers/day_toggle/day_toggle_gen.h"
#include "components/pickers/month_calendar/month_calendar_gen.h"
#include "components/pickers/time_rollers/time_rollers_gen.h"
#include "components/settings/settings_item/settings_item_gen.h"
#include "components/sheets/clock_sheet/clock_sheet_gen.h"
#include "components/sheets/confirm_dialog/confirm_dialog_gen.h"
#include "components/sheets/device_sheet/device_sheet_gen.h"
#include "components/sheets/form_sheet/form_sheet_gen.h"
#include "components/sheets/modal_pad/modal_pad_gen.h"
#include "components/sheets/night_sheet/night_sheet_gen.h"
#include "components/sheets/pick_sheet/pick_sheet_gen.h"
#include "components/sheets/text_sheet/text_sheet_gen.h"
#include "components/sheets/time_sheet/time_sheet_gen.h"
#include "components/sheets/window_sheet/window_sheet_gen.h"
#include "screens/screen_main_gen.h"
#include "screens/screen_saver_gen.h"
#include "screens/screen_wizard_gen.h"

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ALARM_UI_GEN_H*/
