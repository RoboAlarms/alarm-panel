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
#define ALARM_UI_TARGET_E32R40T_4IN    (1 << 1)
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
    ALARM_UI_STYLE_SEGMENT_ACTIVE,
    ALARM_UI_STYLE_BUTTON,
    ALARM_UI_STYLE_BUTTON_PRESSED,
    ALARM_UI_STYLE_BUTTON_PRIMARY,
    ALARM_UI_STYLE_BUTTON_DANGER,
    ALARM_UI_STYLE_BUTTON_TONE_PRESSED,
    ALARM_UI_STYLE_DISABLED,
    ALARM_UI_STYLE_BACKDROP,
    ALARM_UI_STYLE_SHEET_CARD,
    ALARM_UI_STYLE_STEP,
    ALARM_UI_STYLE_STEP_ON,
    ALARM_UI_STYLE_BADGE_BADGE,
    ALARM_UI_STYLE_CHIP_CHIP,
    ALARM_UI_STYLE_CHIP_CHIP_CHECKED,
    ALARM_UI_STYLE_ICON_CHIP_ICON_CHIP,
    ALARM_UI_STYLE_SEGMENT_SEGMENT,
    ALARM_UI_STYLE_SEGMENT_SEGMENT_PRESSED,
    ALARM_UI_STYLE_HOLD_BUTTON_HOLD_BUTTON,
    ALARM_UI_STYLE_HOLD_BUTTON_HOLD_FILL,
    ALARM_UI_STYLE_HOLD_BUTTON_HOLD_FILL_INDICATOR,
    ALARM_UI_STYLE_LIST_ROW_LIST_ROW,
    ALARM_UI_STYLE_LIST_ROW_LIST_ROW_PRESSED,
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
    ALARM_UI_STYLE_ROW_VALUE_ROW_VALUE,
    ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE,
    ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE_PRESSED,
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
    ALARM_UI_STYLE_CODE_PAD_PAD_DOT,
    ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE,
    ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE_PRESSED,
    ALARM_UI_STYLE_CODE_PAD_PAD_KEYS,
    ALARM_UI_STYLE_CODE_PAD_PAD_ENTER,
    ALARM_UI_STYLE_CODE_PAD_PAD_SILENCE,
    ALARM_UI_STYLE_CODE_PAD_PAD_SILENCE_PRESSED,
    ALARM_UI_STYLE_PAD_KEY_PAD_KEY,
    ALARM_UI_STYLE_PAD_KEY_PAD_KEY_PRESSED,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_RING,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_RING_INDICATOR,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_BUTTON,
    ALARM_UI_STYLE_PANIC_BUTTON_PANIC_BUTTON_PRESSED,
    ALARM_UI_STYLE_SETTINGS_ITEM_SETTINGS_ITEM,
    ALARM_UI_STYLE_SETTINGS_ITEM_SETTINGS_ITEM_PRESSED,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD_FOCUSED,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_CURSOR,
    ALARM_UI_STYLE_TEXT_SHEET_TEXT_PLACEHOLDER,
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
#define SPACE_XS 2
/**
 * Small spacing
 */
#define SPACE_SM 4
/**
 * Default gap between related items
 */
#define SPACE_MD 8
/**
 * Gap between items and card padding on small cards
 */
#define SPACE_LG 10
/**
 * Card padding and gap between cards
 */
#define SPACE_XL 12
/**
 * Section spacing
 */
#define SPACE_XXL 16
/**
 * Small rounding (chips inside cards)
 */
#define RADIUS_SM 8
/**
 * Buttons and keys
 */
#define RADIUS_MD 12
/**
 * Cards and tiles
 */
#define RADIUS_LG 14
/**
 * Large panels
 */
#define RADIUS_XL 18
/**
 * Pills and circles
 */
#define RADIUS_FULL 32767
/**
 * Navigation rail width
 */
#define RAIL_W 72
/**
 * Status bar height
 */
#define BAR_H 36
/**
 * Screen width minus the rail
 */
#define CONTENT_W 408
/**
 * Screen height minus the status bar
 */
#define CONTENT_H 284
/**
 * Width of Home's state card
 */
#define PANE_W 164
/**
 * Code pad key width
 */
#define KEY_W 80
/**
 * Code pad key height
 */
#define KEY_H 58
/**
 * Three keys and two gaps
 */
#define KEYS_W 252
/**
 * Toast: offset from the screen's centre (the content's centre, right of the rail)
 */
#define TOAST_X 36
/**
 * Toast: offset from the bottom
 */
#define TOAST_Y -10
/**
 * Toast while the keyboard is open: offset from the top. The keyboard fills the screen, so the toast lies over the sheet's title row, clear of every key (it takes no touch)
 */
#define TOAST_KB_Y 2
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
extern lv_style_t style_segment_active;
extern lv_style_t style_button;
extern lv_style_t style_button_pressed;
extern lv_style_t style_button_primary;
extern lv_style_t style_button_danger;
extern lv_style_t style_button_tone_pressed;
extern lv_style_t style_disabled;
extern lv_style_t style_backdrop;
extern lv_style_t style_sheet_card;
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

/*----------------
 * Subjects
 *----------------*/

extern lv_subject_t subject_program_text;
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
extern lv_subject_t subject_opt_bypass;
extern lv_subject_t subject_opt_bypass_avail;
extern lv_subject_t subject_toast;
extern lv_subject_t subject_toast_visible;
extern lv_subject_t subject_zones_note;
extern lv_subject_t subject_zone_filter;
extern lv_subject_t subject_events_note;
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
extern lv_subject_t subject_settings_section;
extern lv_subject_t subject_update_waiting;
extern lv_subject_t subject_settings_pad;
extern lv_subject_t subject_page_title;
extern lv_subject_t subject_page_note;
extern lv_subject_t subject_page_back;
extern lv_subject_t subject_busy;
extern lv_subject_t subject_sheet;
extern lv_subject_t subject_sheet_title;
extern lv_subject_t subject_sheet_note;
extern lv_subject_t subject_sheet_ok;
extern lv_subject_t subject_sheet_cancel;
extern lv_subject_t subject_sheet_eye;
extern lv_subject_t subject_sheet_shown;
extern lv_subject_t subject_wiz_step;
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
#include "components/forms/hold_button/hold_button_gen.h"
#include "components/forms/list_row/list_row_gen.h"
#include "components/forms/row_button/row_button_gen.h"
#include "components/forms/row_slider/row_slider_gen.h"
#include "components/forms/row_stepper/row_stepper_gen.h"
#include "components/forms/row_switch/row_switch_gen.h"
#include "components/forms/row_value/row_value_gen.h"
#include "components/forms/section_title/section_title_gen.h"
#include "components/home/action_tile/action_tile_gen.h"
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
#include "components/settings/settings_item/settings_item_gen.h"
#include "components/sheets/confirm_dialog/confirm_dialog_gen.h"
#include "components/sheets/modal_pad/modal_pad_gen.h"
#include "components/sheets/pick_sheet/pick_sheet_gen.h"
#include "components/sheets/text_sheet/text_sheet_gen.h"
#include "screens/screen_main_gen.h"
#include "screens/screen_saver_gen.h"
#include "screens/screen_wizard_gen.h"

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ALARM_UI_GEN_H*/
