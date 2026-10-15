/*
 * notify_words.h - what the panel says about an event, in words (docs/features/26).
 *
 * This is the single copy of the event sentences. The Activity list on the panel
 * (components/ui/alarm_ui/src/ui_text.c) and a push notification on a phone (components/services/net_notify)
 * both read them from here, so what the panel shows and what your phone says can never drift
 * apart.
 *
 * Every function that returns words takes the language's catalog first (docs/features/37);
 * NULL is English. Household names (zones, users) are never translated.
 *
 * Portable C11: no heap, no globals, no LVGL. Colours and glyphs are reported as the ids below,
 * and the UI maps them back to its own lv_color_t and icon font code points. Callers own every
 * buffer.
 */
#ifndef NOTIFY_WORDS_H
#define NOTIFY_WORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_engine.h" /* alarm_zone_status_t */
#include "alarm/alarm_types.h"
#include "i18n/i18n.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which kind of event this is. The event log's filters and a notification's categories use the
 * same buckets, so the panel and a phone always agree about what counts as a trouble.
 * ui_event_kind_t mirrors this list one for one (a _Static_assert in ui_text.c keeps it so). */
typedef enum {
    NOTIFY_CAT_ALARM = 0,
    NOTIFY_CAT_TROUBLE,
    NOTIFY_CAT_ARMING,
    NOTIFY_CAT_BYPASS,
    NOTIFY_CAT_SYSTEM,
    NOTIFY_CAT_COUNT,
} notify_cat_t;

/* The colour an event is shown in. Mirrors ui_tone_*() one for one. */
typedef enum {
    NOTIFY_ACCENT_GRAY = 0,
    NOTIFY_ACCENT_GREEN,
    NOTIFY_ACCENT_AMBER,
    NOTIFY_ACCENT_BLUE,
    NOTIFY_ACCENT_VIOLET,
    NOTIFY_ACCENT_ROSE,
    NOTIFY_ACCENT_ORANGE,
    NOTIFY_ACCENT_RED,
    NOTIFY_ACCENT_COUNT,
} notify_accent_t;

/* The icon an event is shown with. The UI maps these to UI_ICON_* code points with one flat
 * table; a phone notification ignores them. Append only: nothing stores these. */
typedef enum {
    NOTIFY_ICON_INFO = 0,
    NOTIFY_ICON_ACTIVITY,
    NOTIFY_ICON_BATTERY_WARNING,
    NOTIFY_ICON_BELL,
    NOTIFY_ICON_BELL_OFF,
    NOTIFY_ICON_CHECK,
    NOTIFY_ICON_CIRCLE_CHECK,
    NOTIFY_ICON_CIRCLE_SLASH,
    NOTIFY_ICON_CLOCK,
    NOTIFY_ICON_CLOUD_FOG,
    NOTIFY_ICON_CLOUD_OFF,
    NOTIFY_ICON_CPU,
    NOTIFY_ICON_DOOR_OPEN,
    NOTIFY_ICON_FLAME,
    NOTIFY_ICON_FOOTPRINTS,
    NOTIFY_ICON_HARD_DRIVE,
    NOTIFY_ICON_HEART_PULSE,
    NOTIFY_ICON_HOUSE,
    NOTIFY_ICON_KEY,
    NOTIFY_ICON_LOCK,
    NOTIFY_ICON_LOCK_OPEN,
    NOTIFY_ICON_RADIO_TOWER,
    NOTIFY_ICON_REFRESH,
    NOTIFY_ICON_ROTATE_CCW,
    NOTIFY_ICON_SETTINGS,
    NOTIFY_ICON_SHIELD,
    NOTIFY_ICON_SHIELD_ALERT,
    NOTIFY_ICON_SHIELD_X,
    NOTIFY_ICON_SIREN,
    NOTIFY_ICON_TRIANGLE_ALERT,
    NOTIFY_ICON_USER,
    NOTIFY_ICON_WIFI_OFF,
    NOTIFY_ICON_WRENCH,
    NOTIFY_ICON_ZAP,
    NOTIFY_ICON_COUNT,
} notify_icon_t;

/* The same sizes ui_line_t uses, so the words survive the trip to the screen unchanged. */
#define NOTIFY_TITLE_LEN 64
#define NOTIFY_BODY_LEN  96

typedef struct {
    char title[NOTIFY_TITLE_LEN];
    char body[NOTIFY_BODY_LEN];
    uint8_t cat;    /* notify_cat_t */
    uint8_t accent; /* notify_accent_t */
    uint8_t icon;   /* notify_icon_t */
    bool silent;    /* the event carried ALARM_EVF_SILENT (duress, silent panic) */
} notify_words_t;

/* One event in words. false: nothing to say about it.
 *
 * show_silent false hides duress and a silent panic, which is what a keypad in a hallway needs
 * (LOG-008) - a notification leaving the building wants them, so it asks with show_silent true
 * and honours the owner's own choice instead. */
bool notify_words(const i18n_catalog_t *cat, const alarm_config_t *cfg, const alarm_event_t *ev,
                  bool show_silent, notify_words_t *out);

/* A system or zone trouble in words, on its own (the Troubles list, and a trouble notification).
 * Both set accent and icon; neither ever fails. */
void notify_sys_trouble_words(const i18n_catalog_t *cat, alarm_sys_trouble_t t, bool restored,
                              notify_words_t *out);
void notify_zone_trouble_words(const i18n_catalog_t *cat, const alarm_config_t *cfg, uint16_t zone,
                               alarm_zone_trouble_t t, bool restored, notify_words_t *out);

/* "Burglary alarm", "Fire alarm", "Medical emergency"... */
const char *notify_cause_title(const i18n_catalog_t *cat, alarm_cause_t cause);

/* "Front Door", or "Zone 12" when unnamed. */
const char *notify_zone_name(const i18n_catalog_t *cat, const alarm_config_t *cfg, uint16_t zone,
                             char *buf, size_t len);
/* "Master", or "User 05"; "" for user 0. */
const char *notify_user_name(const i18n_catalog_t *cat, const alarm_config_t *cfg, uint8_t user,
                             char *buf, size_t len);

/* What a command came back with, in words the owner can act on. */
const char *notify_result_text(const i18n_catalog_t *cat, alarm_result_t r);
/* "Installer", "System master", "Arm only"... */
const char *notify_authority_name(const i18n_catalog_t *cat, alarm_authority_t a);

/* A zone card's words, as on the Zones view: "Closed", "Open", "Motion", "Bypassed · Open",
 * "Low battery", "Alarm · Smoke detected"... into detail, and the accent to show them in. The
 * touchscreen and the Panel Manager read the same words (docs/features/31). */
notify_accent_t notify_zone_state(const i18n_catalog_t *cat, const alarm_config_t *cfg,
                                  uint16_t zone, const alarm_zone_status_t *zs, char *detail,
                                  size_t len);

/* Which bucket an event type belongs to. An unknown type falls to NOTIFY_CAT_SYSTEM, which a
 * notification leaves switched off by default: a future event type cannot start reaching phones
 * on its own. */
notify_cat_t notify_category(alarm_event_type_t type);

/* The colour an arming level is shown in. */
notify_accent_t notify_accent_for_level(alarm_arm_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_WORDS_H */
