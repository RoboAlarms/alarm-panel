/*
 * ui_prefs.h - a change of this panel's Screen and sound settings from elsewhere (docs/features/07
 * UI-046; docs/features/11 "Settings fields", "Applying a change from elsewhere"): Home Assistant
 * through the master, or the master's console over the panel link. Header only and portable: the
 * screen merges a patch into its own copy with it (ui_settings.c, prefs_remote_tick), the
 * platform's mailbox (platform_esp's panel_prefs_remote_*) and the PC's (host_panel.c) keep their
 * bookkeeping with it, and the host tests prove both.
 *
 * A patch (ui_prefs_patch_t, ui_backend.h) names its fields by the settings fields' frozen ids,
 * UI_PREFS_F_*: alarm_proto's PROTO_PREFS_F_* numbers, which the UI can't see (asserted equal in
 * test_ui_prefs.c and in the panel's port, ui_port_esp.c). Its values are in the wire's units: a
 * delay in ms, a switch 0 or 1. A refusal says why with PROTO_PREFS_WHY_*'s numbers
 * (UI_PREFS_WHY_*). The partition, the language and the region are never settings fields
 * (LINK-052): a merge leaves them, and run_setup, as they are.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The settings fields' ids (docs/features/11 "Settings fields"): never renumbered, only appended,
 * and always alarm_proto's PROTO_PREFS_F_* (asserted). */
enum {
    UI_PREFS_F_BRIGHTNESS = 1,        /* brightness, 10..100 */
    UI_PREFS_F_SAVER_BRIGHTNESS = 2,  /* saver_brightness, 0..50 */
    UI_PREFS_F_SAVER_AFTER = 3,       /* saver_after_ms: 0 (never), or 10 s..24 h */
    UI_PREFS_F_OFF_AFTER = 4,         /* off_after_ms, as 3 */
    UI_PREFS_F_NIGHT_ON = 5,          /* night_on, 0/1 */
    UI_PREFS_F_NIGHT_START = 6,       /* night_start, minutes after midnight 0..1439 */
    UI_PREFS_F_NIGHT_END = 7,         /* night_end, 0..1439 */
    UI_PREFS_F_NIGHT_BRIGHTNESS = 8,  /* night_brightness, 10..100 */
    UI_PREFS_F_NIGHT_OFF_AFTER = 9,   /* night_off_after_ms, as 3 */
    UI_PREFS_F_CLOCK_MODE = 10,       /* clock_mode, 0..2 */
    UI_PREFS_F_APPEARANCE = 11,       /* appearance, 0..2 (UI_APPEARANCE_*) */
    UI_PREFS_F_HIGH_CONTRAST = 12,    /* high_contrast, 0/1 */
    UI_PREFS_F_VOLUME = 13,           /* volume (Beeps), 0..100 */
    UI_PREFS_F_CHIME_VOLUME = 14,     /* chime_volume, 0..100 */
    UI_PREFS_F_VOICE_VOLUME = 15,     /* voice_volume, 0..100 */
    UI_PREFS_F_KEY_CLICK = 16,        /* key_click, 0/1 */
    UI_PREFS_F_VOICE_ENABLED = 17,    /* voice_enabled, 0/1: on only while the voice is ready */
    UI_PREFS_F_VOICE_STATUS = 18,     /* voice_status, 0/1 */
    UI_PREFS_F_VOICE_CHIME = 19,      /* voice_chime, 0/1 */
};

#define UI_PREFS_BIT(id) ((uint32_t)1u << ((unsigned)(id) - 1u))
#define UI_PREFS_KNOWN   0x0007FFFFu /* ids 1..UI_PREFS_F_LAST */
/* How the screen looks (ids 1, 2, 5-8, 11, 12: the brightnesses, the night hours, the appearance
 * and high contrast): the wire's PROTO_PREFS_LOOK. The screen holds more: UI_PREFS_SHOWN. */
#define UI_PREFS_LOOK    0x00000CF3u
/* What a change shows on the screen at once (ids 1-12): the look; when a screen nobody touches
 * dims and goes dark (ids 3, 4 and 9: a new delay moves its backlight from the screen saver to
 * dark and back at once, for no touch wakes it in between); and the clock's format (10: its words
 * change). The screen takes a change of any of them from elsewhere at most once per
 * UI_PREFS_LOOK_MS (UI-046; UI-020, "Display safety": the backlight never flickers, live words
 * change at most once a second): a patch that would change one sooner waits, whole. Every field
 * ui_light.h reads is in it (proven in test_ui_prefs.c). The UI's own: no wire carries it. */
#define UI_PREFS_SHOWN   0x00000FFFu

/* Why a field was refused: PROTO_PREFS_WHY_*'s numbers (asserted), 0 = not refused. */
typedef enum {
    UI_PREFS_WHY_NONE = 0,
    UI_PREFS_WHY_NOT_ALLOWED = 1,     /* not a field this panel takes */
    UI_PREFS_WHY_RANGE = 2,           /* outside the field's range */
    UI_PREFS_WHY_IN_USE = 3,          /* the setup runs, a page that shows these settings is open,
                                       * or a change made on the screen is not saved yet */
    UI_PREFS_WHY_HELD = 4,            /* an alarm, entry time or exit time is on the screen */
    UI_PREFS_WHY_VOICE_NOT_READY = 5, /* Announcements turned on while the voice isn't ready */
    UI_PREFS_WHY_BAD = 6,             /* the wire's: a TLV of the wrong length, an id named twice */
    UI_PREFS_WHY_BUSY = 7,            /* another change is under way, or the screen didn't take it */
} ui_prefs_why_t;

#define UI_PREFS_TAKE_MS 3000u /* a patch the screen hasn't taken this soon is answered BUSY */
#define UI_PREFS_LOOK_MS 2000u /* the shortest time between two changes from elsewhere the screen
                                * shows (UI_PREFS_SHOWN) */

/* The value a field may take: the panel's own ranges (panel_prefs.c, the Panel Manager's
 * web_config.c), the same as alarm_proto's proto_prefs_check (proven in test_ui_prefs.c). False
 * for an id this firmware doesn't know. */
static inline bool ui_prefs_valid(unsigned id, uint32_t value)
{
    switch (id) {
    case UI_PREFS_F_BRIGHTNESS:
    case UI_PREFS_F_NIGHT_BRIGHTNESS: return value >= 10u && value <= 100u;
    case UI_PREFS_F_SAVER_BRIGHTNESS: return value <= 50u;
    case UI_PREFS_F_SAVER_AFTER:
    case UI_PREFS_F_OFF_AFTER:
    case UI_PREFS_F_NIGHT_OFF_AFTER: return value == 0u || (value >= 10000u && value <= 86400000u);
    case UI_PREFS_F_NIGHT_START:
    case UI_PREFS_F_NIGHT_END: return value <= 1439u;
    case UI_PREFS_F_CLOCK_MODE:
    case UI_PREFS_F_APPEARANCE: return value <= 2u;
    case UI_PREFS_F_VOLUME:
    case UI_PREFS_F_CHIME_VOLUME:
    case UI_PREFS_F_VOICE_VOLUME: return value <= 100u;
    case UI_PREFS_F_NIGHT_ON:
    case UI_PREFS_F_HIGH_CONTRAST:
    case UI_PREFS_F_KEY_CLICK:
    case UI_PREFS_F_VOICE_ENABLED:
    case UI_PREFS_F_VOICE_STATUS:
    case UI_PREFS_F_VOICE_CHIME: return value <= 1u;
    default: return false;
    }
}

/* A field of o as the wire has it (a switch 0 or 1); false for an id this firmware doesn't know. */
static inline bool ui_prefs_get(const ui_options_t *o, unsigned id, uint32_t *out)
{
    uint32_t v;
    switch (id) {
    case UI_PREFS_F_BRIGHTNESS: v = o->brightness; break;
    case UI_PREFS_F_SAVER_BRIGHTNESS: v = o->saver_brightness; break;
    case UI_PREFS_F_SAVER_AFTER: v = o->saver_after_ms; break;
    case UI_PREFS_F_OFF_AFTER: v = o->off_after_ms; break;
    case UI_PREFS_F_NIGHT_ON: v = o->night_on ? 1u : 0u; break;
    case UI_PREFS_F_NIGHT_START: v = o->night_start; break;
    case UI_PREFS_F_NIGHT_END: v = o->night_end; break;
    case UI_PREFS_F_NIGHT_BRIGHTNESS: v = o->night_brightness; break;
    case UI_PREFS_F_NIGHT_OFF_AFTER: v = o->night_off_after_ms; break;
    case UI_PREFS_F_CLOCK_MODE: v = o->clock_mode; break;
    case UI_PREFS_F_APPEARANCE: v = o->appearance; break;
    case UI_PREFS_F_HIGH_CONTRAST: v = o->high_contrast ? 1u : 0u; break;
    case UI_PREFS_F_VOLUME: v = o->volume; break;
    case UI_PREFS_F_CHIME_VOLUME: v = o->chime_volume; break;
    case UI_PREFS_F_VOICE_VOLUME: v = o->voice_volume; break;
    case UI_PREFS_F_KEY_CLICK: v = o->key_click ? 1u : 0u; break;
    case UI_PREFS_F_VOICE_ENABLED: v = o->voice_enabled ? 1u : 0u; break;
    case UI_PREFS_F_VOICE_STATUS: v = o->voice_status ? 1u : 0u; break;
    case UI_PREFS_F_VOICE_CHIME: v = o->voice_chime ? 1u : 0u; break;
    default: return false;
    }
    *out = v;
    return true;
}

/* Writes a value ui_prefs_valid() took into o. ui_prefs_merge's own step. */
static inline void ui_prefs_store(ui_options_t *o, unsigned id, uint32_t v)
{
    switch (id) {
    case UI_PREFS_F_BRIGHTNESS: o->brightness = (uint8_t)v; break;
    case UI_PREFS_F_SAVER_BRIGHTNESS: o->saver_brightness = (uint8_t)v; break;
    case UI_PREFS_F_SAVER_AFTER: o->saver_after_ms = v; break;
    case UI_PREFS_F_OFF_AFTER: o->off_after_ms = v; break;
    case UI_PREFS_F_NIGHT_ON: o->night_on = v != 0u; break;
    case UI_PREFS_F_NIGHT_START: o->night_start = (uint16_t)v; break;
    case UI_PREFS_F_NIGHT_END: o->night_end = (uint16_t)v; break;
    case UI_PREFS_F_NIGHT_BRIGHTNESS: o->night_brightness = (uint8_t)v; break;
    case UI_PREFS_F_NIGHT_OFF_AFTER: o->night_off_after_ms = v; break;
    case UI_PREFS_F_CLOCK_MODE: o->clock_mode = (uint8_t)v; break;
    case UI_PREFS_F_APPEARANCE: o->appearance = (uint8_t)v; break;
    case UI_PREFS_F_HIGH_CONTRAST: o->high_contrast = v != 0u; break;
    case UI_PREFS_F_VOLUME: o->volume = (uint8_t)v; break;
    case UI_PREFS_F_CHIME_VOLUME: o->chime_volume = (uint8_t)v; break;
    case UI_PREFS_F_VOICE_VOLUME: o->voice_volume = (uint8_t)v; break;
    case UI_PREFS_F_KEY_CLICK: o->key_click = v != 0u; break;
    case UI_PREFS_F_VOICE_ENABLED: o->voice_enabled = v != 0u; break;
    case UI_PREFS_F_VOICE_STATUS: o->voice_status = v != 0u; break;
    case UI_PREFS_F_VOICE_CHIME: o->voice_chime = v != 0u; break;
    default: break;
    }
}

/* Puts a field's value in a patch (not checked: the merge checks it). False, the patch as it was,
 * for an id this firmware doesn't know. */
static inline bool ui_prefs_put(ui_prefs_patch_t *p, unsigned id, uint32_t value)
{
    if (id < 1u || id > UI_PREFS_F_LAST) {
        return false;
    }
    p->have |= UI_PREFS_BIT(id);
    p->value[id - 1u] = value;
    return true;
}

/* Refuses every field of the patch this firmware knows with one reason: why[id - 1] = reason. */
static inline void ui_prefs_refuse_all(const ui_prefs_patch_t *p, uint8_t reason,
                                       uint8_t why[UI_PREFS_F_LAST])
{
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        if ((p->have & UI_PREFS_BIT(id)) != 0u) {
            why[id - 1u] = reason;
        }
    }
}

/* Merges a patch into dst (the screen's copy of its options, or a copy of it): each field p->have
 * names that this firmware knows and whose value is in range is written, and one out of range is
 * left as it was with why[id - 1] = UI_PREFS_WHY_RANGE; why is left alone for every other field.
 * The partition, the language, the region and run_setup are never written, nor a field the patch
 * doesn't name. Returns the fields whose value changed (bit id - 1): 0 when nothing did. */
static inline uint32_t ui_prefs_merge(ui_options_t *dst, const ui_prefs_patch_t *p,
                                      uint8_t why[UI_PREFS_F_LAST])
{
    uint32_t changed = 0u;
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        if ((p->have & UI_PREFS_BIT(id)) == 0u) {
            continue;
        }
        const uint32_t v = p->value[id - 1u];
        if (!ui_prefs_valid(id, v)) {
            why[id - 1u] = (uint8_t)UI_PREFS_WHY_RANGE;
            continue;
        }
        uint32_t was = 0u;
        if (ui_prefs_get(dst, id, &was) && was != v) {
            ui_prefs_store(dst, id, v);
            changed |= UI_PREFS_BIT(id);
        }
    }
    return changed;
}

/* ---- the mailbox (docs/features/11 "The keypad's path to its screen") -------------------------
 * One patch at a time between whoever changes a setting from elsewhere (the panel link's task,
 * Home Assistant's) and the screen's task. This is only its bookkeeping: the platform keeps the
 * patch and the answer's callback beside it, under its own lock (panel_prefs.c; host_panel.c
 * runs on one thread). Times are ms on a clock that only moves forward, compared by difference,
 * so its wrap is harmless. A ticket names a patch from its submit to its answer; never 0.
 *
 *   submit -> WAITING -> take (within UI_PREFS_TAKE_MS) -> TAKEN -> done -> FREE
 *                     -> take (later): EXPIRED -> FREE, the platform answers BUSY
 *                     -> cancel -> FREE, the submitter answers BUSY itself
 */
typedef enum {
    UI_PREFS_BOX_FREE = 0,
    UI_PREFS_BOX_WAITING, /* submitted, not taken yet */
    UI_PREFS_BOX_TAKEN,   /* the screen has it: its answer comes through done */
} ui_prefs_box_state_t;

typedef struct {
    uint8_t state;     /* ui_prefs_box_state_t */
    uint32_t ticket;   /* the patch's, while WAITING or TAKEN */
    uint32_t since_ms; /* when it was submitted */
    uint32_t last;     /* the last ticket given out */
} ui_prefs_box_t;

typedef enum {
    UI_PREFS_TAKE_NONE = 0, /* nothing waiting */
    UI_PREFS_TAKE_OK,       /* *ticket's patch is the screen's now */
    UI_PREFS_TAKE_EXPIRED,  /* *ticket's patch waited UI_PREFS_TAKE_MS or longer: withdrawn,
                             * never applied; the platform answers it BUSY */
} ui_prefs_take_t;

/* A patch in: its ticket, or 0 while another is in the box (WAITING or TAKEN: one change at a
 * time, the new one is BUSY at once). */
static inline uint32_t ui_prefs_box_submit(ui_prefs_box_t *b, uint32_t now_ms)
{
    if (b->state != (uint8_t)UI_PREFS_BOX_FREE) {
        return 0u;
    }
    b->last = b->last == UINT32_MAX ? 1u : b->last + 1u;
    b->ticket = b->last;
    b->since_ms = now_ms;
    b->state = (uint8_t)UI_PREFS_BOX_WAITING;
    return b->ticket;
}

/* The screen's task looks in the box (*ticket is set unless NONE). */
static inline ui_prefs_take_t ui_prefs_box_take(ui_prefs_box_t *b, uint32_t now_ms, uint32_t *ticket)
{
    if (b->state != (uint8_t)UI_PREFS_BOX_WAITING) {
        return UI_PREFS_TAKE_NONE;
    }
    *ticket = b->ticket;
    if ((uint32_t)(now_ms - b->since_ms) >= UI_PREFS_TAKE_MS) {
        b->state = (uint8_t)UI_PREFS_BOX_FREE;
        return UI_PREFS_TAKE_EXPIRED;
    }
    b->state = (uint8_t)UI_PREFS_BOX_TAKEN;
    return UI_PREFS_TAKE_OK;
}

/* The screen answered ticket: true when it was the patch taken (the box is free again, pass the
 * answer on); false for any other (a repeated or a stale answer: drop it). */
static inline bool ui_prefs_box_done(ui_prefs_box_t *b, uint32_t ticket)
{
    if (b->state != (uint8_t)UI_PREFS_BOX_TAKEN || ticket == 0u || b->ticket != ticket) {
        return false;
    }
    b->state = (uint8_t)UI_PREFS_BOX_FREE;
    return true;
}

/* The submitter gives up on ticket (it waited UI_PREFS_TAKE_MS): true when it was still waiting -
 * withdrawn, no answer will come, the submitter answers BUSY itself; false when the screen has it
 * (its answer follows, within UI_PREFS_LOOK_MS) or it has already been answered. */
static inline bool ui_prefs_box_cancel(ui_prefs_box_t *b, uint32_t ticket)
{
    if (b->state != (uint8_t)UI_PREFS_BOX_WAITING || ticket == 0u || b->ticket != ticket) {
        return false;
    }
    b->state = (uint8_t)UI_PREFS_BOX_FREE;
    return true;
}

#ifdef __cplusplus
}
#endif
