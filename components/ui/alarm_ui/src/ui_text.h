/*
 * ui_text.h - words, icons and tones for what the alarm engine reports (English, v1).
 * Portable: LVGL + alarm_core only. The touch UI's words live in the ui_*.c modules; the event
 * sentences ("Burglary alarm", "Armed Away") have their single copy in components/core/notify
 * (notify_words.c), which ui_text.c wraps; ui_text.c itself keeps the tones, the icon table and
 * the zone card. Translation goes through one catalog (docs/features/37).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_engine.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lucide glyphs used from C (the XML uses the same code points). */
#define UI_ICON_ACTIVITY        "\xEE\x80\xB8" /* U+E038 */
#define UI_ICON_BELL            "\xEE\x81\x99" /* U+E059 */
#define UI_ICON_BELL_OFF        "\xEE\x81\x9A" /* U+E05A */
#define UI_ICON_CHECK           "\xEE\x81\xAC" /* U+E06C */
#define UI_ICON_CIRCLE          "\xEE\x81\xB6" /* U+E076 */
#define UI_ICON_CIRCLE_ALERT    "\xEE\x81\xB7" /* U+E077 */
#define UI_ICON_CLOCK           "\xEE\x82\x87" /* U+E087 */
#define UI_ICON_CLOUD_OFF       "\xEE\x82\x8D" /* U+E08D */
#define UI_ICON_CPU             "\xEE\x82\xA9" /* U+E0A9 */
#define UI_ICON_DROPLETS        "\xEE\x82\xB5" /* U+E0B5 */
#define UI_ICON_FLAME           "\xEE\x83\x92" /* U+E0D2 */
#define UI_ICON_HARD_DRIVE      "\xEE\x83\xAD" /* U+E0ED */
#define UI_ICON_HOUSE           "\xEE\x83\xB5" /* U+E0F5 */
#define UI_ICON_IMAGE           "\xEE\x83\xB6" /* U+E0F6 */
#define UI_ICON_INFO            "\xEE\x83\xB9" /* U+E0F9 */
#define UI_ICON_KEY             "\xEE\x83\xBD" /* U+E0FD */
#define UI_ICON_LOCK            "\xEE\x84\x8B" /* U+E10B */
#define UI_ICON_LOCK_OPEN       "\xEE\x84\x8C" /* U+E10C */
#define UI_ICON_LOG_OUT         "\xEE\x84\x8E" /* U+E10E */
#define UI_ICON_MOON            "\xEE\x84\x9E" /* U+E11E */
#define UI_ICON_SETTINGS        "\xEE\x85\x94" /* U+E154 */
#define UI_ICON_SHIELD          "\xEE\x85\x98" /* U+E158 */
#define UI_ICON_THERMOMETER     "\xEE\x86\x86" /* U+E186 */
#define UI_ICON_TRIANGLE_ALERT  "\xEE\x86\x93" /* U+E193 */
#define UI_ICON_USER            "\xEE\x86\x9F" /* U+E19F */
#define UI_ICON_WIFI            "\xEE\x86\xAE" /* U+E1AE */
#define UI_ICON_WIFI_OFF        "\xEE\x86\xAF" /* U+E1AF */
#define UI_ICON_ZAP             "\xEE\x86\xB4" /* U+E1B4 */
#define UI_ICON_SHIELD_ALERT    "\xEE\x87\xBE" /* U+E1FE */
#define UI_ICON_SHIELD_CHECK    "\xEE\x87\xBF" /* U+E1FF */
#define UI_ICON_SHIELD_X        "\xEE\x88\x80" /* U+E200 */
#define UI_ICON_CLOUD_FOG       "\xEE\x88\x94" /* U+E214 */
#define UI_ICON_BELL_RING       "\xEE\x88\xA4" /* U+E224 */
#define UI_ICON_CIRCLE_CHECK    "\xEE\x88\xA6" /* U+E226 */
#define UI_ICON_WAVES           "\xEE\x8A\x83" /* U+E283 */
#define UI_ICON_SIREN           "\xEE\x8B\xAF" /* U+E2EF */
#define UI_ICON_CIRCLE_DOT      "\xEE\x8D\x85" /* U+E345 */
#define UI_ICON_HEART_PULSE     "\xEE\x8D\xAE" /* U+E36E */
#define UI_ICON_BATTERY_WARNING "\xEE\x8E\xAC" /* U+E3AC */
#define UI_ICON_FOOTPRINTS      "\xEE\x8E\xB9" /* U+E3B9 */
#define UI_ICON_DOOR_CLOSED     "\xEE\x8F\x95" /* U+E3D5 */
#define UI_ICON_DOOR_OPEN       "\xEE\x8F\x96" /* U+E3D6 */
#define UI_ICON_WAREHOUSE       "\xEE\x8F\xA6" /* U+E3E6 */
#define UI_ICON_CIRCLE_SLASH    "\xEE\x90\x82" /* U+E402 */
#define UI_ICON_RADIO_TOWER     "\xEE\x90\x84" /* U+E404 */
#define UI_ICON_APP_WINDOW      "\xEE\x90\xA6" /* U+E426 */
#define UI_ICON_KEY_ROUND       "\xEE\x92\xA3" /* U+E4A3 */
#define UI_ICON_EYE_OFF         "\xEE\x82\xBB" /* U+E0BB */
#define UI_ICON_ALARM_SMOKE     "\xEE\x95\xBB" /* U+E57B */
#define UI_ICON_WIFI_HIGH       "\xEE\x97\xB7" /* U+E5F7 */
#define UI_ICON_WIFI_LOW        "\xEE\x97\xB8" /* U+E5F8 */
#define UI_ICON_WIFI_ZERO       "\xEE\x97\xB9" /* U+E5F9 */
#define UI_ICON_CIRCLE_HELP     "\xEE\x82\x82" /* U+E082 */
#define UI_ICON_GLOBE           "\xEE\x83\xA8" /* U+E0E8 */
#define UI_ICON_HASH            "\xEE\x83\xAF" /* U+E0EF */
#define UI_ICON_MONITOR_OFF     "\xEE\x87\x9C" /* U+E1DC */
#define UI_ICON_LIFE_BUOY       "\xEE\x84\x81" /* U+E101 */
#define UI_ICON_LOG_IN          "\xEE\x84\x8D" /* U+E10D */
#define UI_ICON_PLUS            "\xEE\x84\xBD" /* U+E13D */
#define UI_ICON_ROTATE_CCW      "\xEE\x85\x88" /* U+E148 */
#define UI_ICON_SERVER          "\xEE\x85\x93" /* U+E153 */
#define UI_ICON_SUN             "\xEE\x85\xB8" /* U+E178 */
#define UI_ICON_SUN_MOON        "\xEE\x8A\xB2" /* U+E2B2 */
#define UI_ICON_CONTRAST        "\xEE\x82\x9D" /* U+E09D */
#define UI_ICON_TAG             "\xEE\x85\xBF" /* U+E17F */
#define UI_ICON_TRASH           "\xEE\x86\x8E" /* U+E18E */
#define UI_ICON_USER_PLUS       "\xEE\x86\xA2" /* U+E1A2 */
#define UI_ICON_USERS           "\xEE\x86\xA4" /* U+E1A4 */
#define UI_ICON_VOLUME          "\xEE\x86\xAB" /* U+E1AB */
#define UI_ICON_WRENCH          "\xEE\x86\xB1" /* U+E1B1 */
#define UI_ICON_PENCIL          "\xEE\x87\xB9" /* U+E1F9 */
#define UI_ICON_LAYERS          "\xEE\x94\xA9" /* U+E529 */
#define UI_ICON_FLASK_CONICAL   "\xEE\x83\x95" /* U+E0D5 */
#define UI_ICON_BADGE_CHECK     "\xEE\x89\x81" /* U+E241 */
#define UI_ICON_SLIDERS         "\xEE\x8A\x9A" /* U+E29A */
#define UI_ICON_PARTY           "\xEE\x8D\x83" /* U+E343 */
#define UI_ICON_SPARKLES        "\xEE\x90\x92" /* U+E412 */
#define UI_ICON_MEMORY          "\xEE\x91\x85" /* U+E445 */
#define UI_ICON_SCROLL_TEXT     "\xEE\x91\x9F" /* U+E45F */
#define UI_ICON_CHEVRON_RIGHT   "\xEE\x81\xAF" /* U+E06F */
#define UI_ICON_HOUSE_WIFI      "\xEE\x98\xBC" /* U+E63C */
#define UI_ICON_LOCK_KEYHOLE    "\xEE\x94\xB1" /* U+E531 */
#define UI_ICON_SHIELD_OFF      "\xEE\x85\x9A" /* U+E15A */
#define UI_ICON_REFRESH         "\xEE\x85\x85" /* U+E145 */
#define UI_ICON_CIRCLE_X        "\xEE\x82\x84" /* U+E084 */
#define UI_ICON_CALENDAR_CLOCK  "\xEE\x8C\x84" /* U+E304 */
#define UI_ICON_PLUG            "\xEE\x8D\xBF" /* U+E37F */
#define UI_ICON_PUZZLE          "\xEE\x8A\x9C" /* U+E29C */
#define UI_ICON_LANGUAGES       "\xEE\x83\xBE" /* U+E0FE */
#define UI_ICON_DOWNLOAD        "\xEE\x82\xB2" /* U+E0B2 */
#define UI_ICON_MAP_PIN         "\xEE\x84\x91" /* U+E111 */
#define UI_ICON_QR_CODE         "\xEE\x87\x9F" /* U+E1DF */
#define UI_ICON_SEND            "\xEE\x85\x92" /* U+E152 */
#define UI_ICON_UNLINK          "\xEE\x86\x9C" /* U+E19C */
#define UI_ICON_MONITOR         "\xEE\x84\x9D" /* U+E11D */
#define UI_ICON_TIMER           "\xEE\x87\xA0" /* U+E1E0 */
#define UI_ICON_CODE_XML        "\xEE\x88\x86" /* U+E206 */
#define UI_ICON_ARROW_LEFT_RIGHT "\xEE\x89\x8A" /* U+E24A */
#define UI_ICON_CLOCK_ALERT     "\xEE\x98\xAA" /* U+E62A */
/* Settings > Automations (BL-168): "Try it now", and the devices an action names */
#define UI_ICON_PLAY            "\xEE\x84\xBC" /* U+E13C */
#define UI_ICON_LIGHTBULB       "\xEE\x87\x82" /* U+E1C2 */
#define UI_ICON_DROPLET         "\xEE\x82\xB4" /* U+E0B4 */
#define UI_ICON_POWER           "\xEE\x85\x80" /* U+E140 */
/* The voice (BL-202, docs/features/08 voice-first-build.md D03, D06): the zone's Spoken name and
 * its editor, Screen and sound > Announcements */
#define UI_ICON_MIC             "\xEE\x84\x98" /* U+E118 */
#define UI_ICON_MEGAPHONE       "\xEE\x88\xB5" /* U+E235 */
#define UI_ICON_AUDIO_LINES     "\xEE\x95\x9A" /* U+E55A */
#define UI_ICON_CALENDAR_X      "\xEE\x8A\xBE" /* U+E2BE */
#define UI_ICON_TEXT_CURSOR     "\xEE\x89\xA5" /* U+E265 */
#define UI_ICON_COMPASS         "\xEE\x82\x9B" /* U+E09B */
#define UI_ICON_PACKAGE         "\xEE\x84\xA9" /* U+E129 */
#define UI_ICON_SEARCH          "\xEE\x85\x91" /* U+E151 */
#define UI_ICON_ARROW_UP        "\xEE\x81\x8A" /* U+E04A */
#define UI_ICON_ARROW_DOWN      "\xEE\x81\x82" /* U+E042 */
#define UI_ICON_CHEVRON_LEFT    "\xEE\x81\xAE" /* U+E06E */

/* Tone colours: the XML's colour constants, in the current theme (ui_theme.c); gray is
 * color_text_2. */
lv_color_t ui_tone_green(void);
lv_color_t ui_tone_amber(void);
lv_color_t ui_tone_blue(void);
lv_color_t ui_tone_violet(void);
lv_color_t ui_tone_rose(void);
lv_color_t ui_tone_orange(void);
lv_color_t ui_tone_red(void);
lv_color_t ui_tone_gray(void);
lv_color_t ui_tone_for_level(alarm_arm_level_t level);

typedef struct {
    const char *glyph;
    lv_color_t tone;
    char title[64];
    char detail[96];
    bool link; /* the Troubles list: its row opens a page (the Zigbee radio's, BL-144) */
    bool link_zw; /* with link: it is the Z-Wave radio's page, not the Zigbee radio's (BL-152) */
} ui_line_t;

/* "Front Door", or "Zone 12" when unnamed. */
const char *ui_zone_name(const alarm_config_t *cfg, uint16_t zone, char *buf, size_t len);
/* "Master", or "User 05"; "" for user 0. */
const char *ui_user_name(const alarm_config_t *cfg, uint8_t user, char *buf, size_t len);

/* Zone card: class icon, state words ("Closed", "Open", "Bypassed", "Low battery"...), tone. */
void ui_zone_state(const alarm_config_t *cfg, uint16_t zone, const alarm_zone_status_t *zs,
                   ui_line_t *out);

/* Activity row for an event. Silent events (duress, silent panic) are only described when
 * show_silent is set (the event log behind a master or installer code, LOG-008); otherwise
 * this returns false for them. */
bool ui_event_line(const alarm_config_t *cfg, const alarm_event_t *ev, bool show_silent,
                   ui_line_t *out);

/* Which kind of event, for the event log filters. */
typedef enum {
    UI_EVKIND_ALARM = 0,
    UI_EVKIND_TROUBLE,
    UI_EVKIND_ARMING,
    UI_EVKIND_BYPASS,
    UI_EVKIND_SYSTEM,
} ui_event_kind_t;
ui_event_kind_t ui_event_kind(alarm_event_type_t type);

/* "Standard", "Arm only"... and one line on what that user can do. */
const char *ui_authority_name(alarm_authority_t a);
const char *ui_authority_detail(alarm_authority_t a);
lv_color_t ui_authority_tone(alarm_authority_t a);

/* Zone types in the order the installer picks them, with their numbers. */
typedef struct {
    alarm_zone_type_t type;
    const char *name;   /* "Perimeter" */
    const char *detail; /* "Doors and windows: alarm right away" */
} ui_zone_type_t;
extern const ui_zone_type_t UI_ZONE_TYPES[];
extern const size_t UI_ZONE_TYPE_COUNT;
const char *ui_zone_type_name(alarm_zone_type_t type);

/* Sensor kinds (device classes) with their icons. */
const char *ui_device_class_name(alarm_device_class_t cls);
const char *ui_device_class_glyph(alarm_device_class_t cls);
#define UI_DEVICE_CLASS_COUNT 16

/* Trouble rows: the system trouble / zone trouble in words and its icon. */
void ui_sys_trouble_line(alarm_sys_trouble_t t, bool restored, ui_line_t *out);
void ui_zone_trouble_line(const alarm_config_t *cfg, uint16_t zone, alarm_zone_trouble_t t,
                          bool restored, ui_line_t *out);

/* "Burglary alarm", "Fire alarm"... from an alarm cause. */
const char *ui_cause_title(alarm_cause_t cause);
/* Most important cause in a memory mask (fire > CO > panic > burglary...), NONE if empty. */
alarm_cause_t ui_top_cause(uint16_t memory);

/* Human text for a command result ("Wrong code, try again"). */
const char *ui_result_text(alarm_result_t r);

#ifdef __cplusplus
}
#endif
