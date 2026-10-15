/*
 * ui_light.h - how lit this keypad's screen is (docs/features/07 UI-009, UI-043): in use, the
 * screen saver (dimmed) or dark, from the time since the last touch and the night hours. Header
 * only and portable: the touch UI follows it, the host tests prove it.
 *
 * The screen saver and "screen off" both count from the last touch and the first of the two wins:
 * a screen-off time shorter than the saver's goes straight to dark. Night hours swap in their own
 * brightness and their own screen-off time. The appearance's Automatic follows the same night hours (ui_light_theme).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_LIGHT_AWAKE = 0, /* in use: the awake brightness */
    UI_LIGHT_SAVER,     /* the screen saver, dimmed */
    UI_LIGHT_OFF,       /* dark, the backlight off: a touch wakes it */
} ui_light_stage_t;

/* Night hours now? minute: minutes after local midnight, negative while the clock is not set
 * (never night then: the panel can't tell). The start is in, the end is out; a start after the end
 * wraps past midnight; a start equal to the end is never. */
static inline bool ui_light_night(const ui_options_t *opt, int minute)
{
    if (!opt->night_on || minute < 0 || opt->night_start == opt->night_end) {
        return false;
    }
    const int m = minute % 1440;
    const int start = opt->night_start;
    const int end = opt->night_end;
    return start < end ? (m >= start && m < end) : (m >= start || m < end);
}

/* What a screen nobody touched for idle_ms shows, when nothing else keeps it awake. */
static inline ui_light_stage_t ui_light_stage(const ui_options_t *opt, uint32_t idle_ms, bool night)
{
    const uint32_t off = night ? opt->night_off_after_ms : opt->off_after_ms;
    if (off != 0u && idle_ms >= off) {
        return UI_LIGHT_OFF;
    }
    if (opt->saver_after_ms != 0u && idle_ms >= opt->saver_after_ms) {
        return UI_LIGHT_SAVER;
    }
    return UI_LIGHT_AWAKE;
}

/* The backlight for a stage, in percent. At night the screen is never brighter than the night
 * brightness - except while an alarm sounds (alarm): then it is lit as by day, to be read. */
static inline uint8_t ui_light_percent(const ui_options_t *opt, ui_light_stage_t stage, bool night,
                                       bool alarm)
{
    switch (stage) {
    case UI_LIGHT_OFF: return 0u;
    case UI_LIGHT_SAVER:
        return night && opt->night_brightness < opt->saver_brightness ? opt->night_brightness
                                                                      : opt->saver_brightness;
    case UI_LIGHT_AWAKE:
    default: return night && !alarm ? opt->night_brightness : opt->brightness;
    }
}

/* The palette the screen is in (docs/features/07 UI-010, BL-132): the same four, in the same order,
 * as the touch UI's ui_theme_t (ui_app.h), which the core can't see. */
typedef enum {
    UI_LIGHT_THEME_DARK = 0,
    UI_LIGHT_THEME_LIGHT,
    UI_LIGHT_THEME_CONTRAST,       /* high contrast: white on black */
    UI_LIGHT_THEME_CONTRAST_LIGHT, /* high contrast, light: black on white */
} ui_light_theme_t;

/* The palette the options ask for at a minute (ui_light_night's minute: negative while the clock
 * is not set). Automatic is dark during the night hours and light otherwise - with the night hours
 * off or the time unknown, light. An appearance this firmware doesn't know is Dark. High contrast
 * picks the pair's high-contrast palette. The result changes only at the night's two ends, so a
 * caller that applies it when it changes switches once at each (UI-020). */
static inline ui_light_theme_t ui_light_theme(const ui_options_t *opt, int minute)
{
    bool dark;
    switch (opt->appearance) {
    case UI_APPEARANCE_LIGHT: dark = false; break;
    case UI_APPEARANCE_AUTO: dark = ui_light_night(opt, minute); break;
    case UI_APPEARANCE_DARK:
    default: dark = true; break;
    }
    if (opt->high_contrast) {
        return dark ? UI_LIGHT_THEME_CONTRAST : UI_LIGHT_THEME_CONTRAST_LIGHT;
    }
    return dark ? UI_LIGHT_THEME_DARK : UI_LIGHT_THEME_LIGHT;
}

#ifdef __cplusplus
}
#endif
