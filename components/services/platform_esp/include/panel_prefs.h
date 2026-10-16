/*
 * panel_prefs.h - this keypad's display and sound preferences and the setup state, in NVS
 * (namespaces "ui" and "sys"). Anyone at the keypad may change these; they are not part of
 * the alarm configuration. What is stored is also kept in RAM for any task (panel_prefs_seq,
 * panel_prefs_current), and a change from elsewhere reaches the screen through a mailbox
 * (panel_prefs_remote_*; docs/features/07 UI-046, docs/features/11 "Settings over the link").
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm_proto/proto_prefs.h"
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

/* ---- what is stored, for any task (docs/features/11 "Settings over the link", BL-216) ----
 * Both are kept by panel_prefs_save, so every writer is caught (the screen, the Panel Manager, a
 * language pack's install, the developer link), and read without NVS. */
/* Moves with every save that succeeded (one that changed nothing too). */
uint32_t panel_prefs_seq(void);
/* What is stored: the last save, else the start-up's first panel_prefs_load. False, with the
 * defaults, before either. */
bool panel_prefs_current(panel_prefs_t *out);
/* All 19 settings fields of p as they are reported (proto_prefs: the master's settings to Home
 * Assistant, a keypad's PREFS): out->have = PROTO_PREFS_KNOWN. The language and region aren't
 * settings fields (docs/features/11 "Settings fields"). */
void panel_prefs_to_values(const panel_prefs_t *p, proto_prefs_values_t *out);

/* ---- a change from elsewhere (docs/features/07 UI-046; docs/features/11 "The keypad's path to
 * its screen") ----
 * One patch at a time from whoever changes a setting from elsewhere (the panel link's task, Home
 * Assistant's) to the screen's task, which judges it and applies it through its own copy and its
 * normal save (ui_settings.c, through ui_port_esp's prefs_remote_*), never behind it. The
 * bookkeeping is ui_backend's ui_prefs_box_* (ui_prefs.h, host-tested). */

/* The screen's answer: why[id - 1] a PROTO_PREFS_WHY_* for each field refused, 0 for the rest (a
 * field not refused is applied and stored: panel_prefs_current shows it). Runs on the screen's
 * task, inside its tick and under its lock: it only copies the answer where the submitter reads
 * it (an atomic box), and never blocks, takes board_ui_lock or writes NVS. */
typedef void (*panel_prefs_done_fn)(void *ctx, uint32_t ticket, const uint8_t why[PROTO_PREFS_F_LAST]);

/* Hands a patch to the screen: patch->have the fields to change, each value already checked
 * (proto_prefs). Returns its ticket, or 0 while another is under way (answer BUSY at once) or
 * without a patch or a done. done is called at most once for the ticket, on the screen's task; a
 * patch the screen doesn't take within 3 s (UI_PREFS_TAKE_MS) is answered BUSY there and never
 * applied. A submitter whose screen may never look (it is stuck, or there is none) calls
 * panel_prefs_remote_cancel once 3 s have passed. */
uint32_t panel_prefs_remote_submit(const proto_prefs_values_t *patch, panel_prefs_done_fn done,
                                   void *ctx);
/* The submitter gives up on ticket: true when the screen hadn't taken it - withdrawn, done will
 * never be called for it, answer BUSY yourself; false when done has been or will be called (the
 * screen took it: within 2 s, UI_PREFS_LOOK_MS). */
bool panel_prefs_remote_cancel(uint32_t ticket);
/* The screen's task only (ui_port_esp's prefs_remote_take): the patch waiting, now the screen's,
 * answered with panel_prefs_remote_done. False when none waits; a patch that waited 3 s or more is
 * answered BUSY here (its done runs now) and false. */
bool panel_prefs_remote_take(proto_prefs_values_t *out, uint32_t *ticket);
/* The screen's answer for the ticket it took (ui_port_esp's prefs_remote_done): its done runs
 * here. Any other ticket is dropped. */
void panel_prefs_remote_done(uint32_t ticket, const uint8_t why[PROTO_PREFS_F_LAST]);

/* A language was ever stored (the NVS key exists): false on a new panel and after a factory
 * reset, when a language pack store's default language is adopted (docs/features/37). */
bool panel_prefs_lang_stored(void);

/* The first-boot setup wizard ran to the end (a factory reset clears it). */
bool panel_setup_done(void);
esp_err_t panel_setup_set_done(void);

#ifdef __cplusplus
}
#endif
