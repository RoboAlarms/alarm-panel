/*
 * ui_app.h - the alarm panel's touch UI (an LVGL Pro layout components/ui/alarm_ui/layouts/<WxH>/alarm_ui + its logic).
 *
 * Portable: LVGL 9.5 and core components only. The panel plugs in through ui_backend_t
 * (components/core/ui_backend), so the same code runs on the panel (its backend is
 * components/services/ui_port_esp), on a remote keypad and in the PC preview and simulator
 * (host/ui_preview). Every function here runs on the LVGL thread.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "i18n/i18n.h"
#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ui_options_defaults(), kept for callers of the old name. */
void ui_app_default_options(ui_options_t *opt);
/* Platform calls under the UI lock after remotely saving keypad preferences. */
void ui_app_apply_options(const ui_options_t *opt);

/* Builds the screens on the default display and starts the UI (the setup wizard first when
 * opt->run_setup). Call with the LVGL lock held (or from the LVGL thread). opt may be NULL
 * for the defaults. */
void ui_app_start(const ui_backend_t *backend, const ui_options_t *opt);


/* The panel's appearance (docs/features/07 UI-010, BL-132; ui_theme.c): the palettes of the
 * layout's themes.xml. ui_theme_apply() switches every screen at once, in one step (UI-020),
 * keeping the view, the page and an open sheet; the screen saver keeps its theme's dark
 * colours. It stores nothing: the caller keeps the choice. Call with the LVGL lock held; it may
 * be called before ui_app_start() (the stored theme at boot), and the screens are made in it. */
typedef enum {
    UI_THEME_DARK = 0,
    UI_THEME_LIGHT,
    UI_THEME_CONTRAST,       /* high contrast: white on black, everything outlined */
    UI_THEME_CONTRAST_LIGHT, /* high contrast, light: black on white */
} ui_theme_t;
void ui_theme_apply(ui_theme_t t);
ui_theme_t ui_theme_current(void);

/* How many texts were longer than the screen's text holds (127 bytes) and were cut, on a whole
 * character, since ui_app_start(); *last gets the start of the last one. A translation over its
 * budget (I18N-023); the PC preview reports them. */
unsigned ui_app_text_cuts(const char **last);

/* Runs the setup again on a panel that is already set up (the installer menu): every step can
 * be left with "Leave setup", which goes back to Home and changes nothing else. */
void ui_app_run_setup(void);
#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
bool ui_dev_control_begin(void);
void ui_dev_control_end(void);
#endif

/* ---- the remote keypad (docs/features/31) -------------------------------------------------------
 * Someone uses this screen from the network: the Panel Manager's remote keypad or an enrolled
 * workstation. While a session is active a red banner sits in the status bar and the screen
 * saver stays off, so whoever is at the panel sees it happen. Tapping the banner (at the panel:
 * a press from the remote itself is ignored) pauses the session and asks whether to allow it
 * or turn remote access off (ui_backend_t.remote_pause / remote_set). The pause belongs to the
 * panel, not the session: a remote that leaves and comes back is still paused until the person
 * at the panel answers, so reconnecting can't undo it. */
typedef struct {
    bool active;
    bool paused;    /* the remote keypad is paused (with or without a session) */
    char who[48];   /* "Alex", "Installer", "Development workstation": a name, ALARM_NAME_LEN */
    char where[48]; /* the address it comes from, "" when unknown */
} ui_remote_session_t;
void ui_app_remote_session(const ui_remote_session_t *session);
/* The input device remote presses arrive on (never the panel's own touch screen). */
void ui_app_remote_indev(lv_indev_t *indev);
/* Typing on the remote end's keyboard: text into the text sheet on screen, digits into the code
 * pad. false when neither is showing, so nothing happened. */
bool ui_app_remote_text(const char *utf8);
typedef enum {
    UI_REMOTE_KEY_BACKSPACE = 0,
    UI_REMOTE_KEY_ENTER,  /* the sheet's Done, the pad's check key */
    UI_REMOTE_KEY_ESCAPE, /* the sheet's Cancel, the pad's close */
} ui_remote_key_t;
bool ui_app_remote_key(ui_remote_key_t key);
#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
/* Development-only page walk for memory tests; the caller holds the UI lock. Never submits
 * a code or saves edits. */
bool ui_memory_diagnostic_step(unsigned step);
#endif

#ifdef __cplusplus
}
#endif
