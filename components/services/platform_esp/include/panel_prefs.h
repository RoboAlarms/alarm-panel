/*
 * panel_prefs.h - this keypad's display and sound preferences and the setup state, in NVS
 * (namespaces "ui" and "sys"). Anyone at the keypad may change these; they are not part of
 * the alarm configuration.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t brightness;       /* backlight when awake, 10..100 (default 80) */
    uint8_t saver_brightness; /* screen saver backlight, 0..50 (default 25) */
    uint32_t saver_after_ms;  /* idle time before the screen saver, 0 = never (default 60 s) */
    /* The screen going dark and the night hours (docs/features/07 UI-043; ui_options_t). */
    uint32_t off_after_ms;       /* idle time before the screen goes dark, 0 = never (default) */
    bool night_on;               /* night hours (default off) */
    uint16_t night_start;        /* minutes after midnight, 0..1439 (default 22:00) */
    uint16_t night_end;          /* minutes after midnight, 0..1439 (default 07:00) */
    uint8_t night_brightness;    /* backlight when awake at night, 10..100 (default 20) */
    uint32_t night_off_after_ms; /* off_after_ms at night, 0 = never (default 60 s) */
    uint8_t clock_mode;       /* I18N_CLOCK_*: 0 the region's (default), 1 12 h, 2 24 h */
    uint8_t volume;           /* Beeps volume: keypad sounds except alarms, the chime and the
                               * voice, 0..100 (default 70; docs/features/08 AUD-032) */
    uint8_t chime_volume;     /* the door chime, 0..100 (default: volume, as before AUD-032) */
    uint8_t voice_volume;     /* announcements and Preview, 0..100 (default: volume) */
    bool key_click;           /* touch feedback (default true) */
    char lang[16];            /* the language, BCP 47 (default "en-US"; docs/features/37) */
    char region[4];           /* the region for formats (ISO 3166), "" = the language's */
    /* How the screen looks (docs/features/07 UI-010, BL-132; ui_options_t): keys "appearance"
     * and "contrast", absent before them, so an older store reads Dark without high contrast. */
    uint8_t appearance;       /* UI_APPEARANCE_*: 0 Dark (default), 1 Light, 2 Automatic */
    bool high_contrast;       /* default false */
    /* Announcements (docs/features/08 voice-first-build.md D06): keys "voice", "voice_st",
     * "voice_ch", absent before them. The main switch is on by default (decided 2026-10-12; an
     * absent key reads as on); the two kinds keep their choice while it is off. The walk test has
     * no switch: it always speaks (WALK-016); a "voice_wk" an older firmware stored is no longer
     * read. Each panel's own: a link keypad's too (docs/features/11 LINK-028). */
    bool voice_enabled;       /* default true; an install never changes it */
    bool voice_status;        /* arming, armed, disarmed, entry, not ready (default true) */
    bool voice_chime;         /* the zone's spoken name after the chime (default true) */
} panel_prefs_t;

void panel_prefs_defaults(panel_prefs_t *out);

/* Stored values, defaults for anything missing or out of range. */
void panel_prefs_load(panel_prefs_t *out);
esp_err_t panel_prefs_save(const panel_prefs_t *prefs);

/* A language was ever stored (the NVS key exists): false on a new panel and after a factory
 * reset, when a language pack store's default language is adopted (docs/features/37). */
bool panel_prefs_lang_stored(void);

/* The first-boot setup wizard ran to the end (a factory reset clears it). */
bool panel_setup_done(void);
esp_err_t panel_setup_set_done(void);

#ifdef __cplusplus
}
#endif
