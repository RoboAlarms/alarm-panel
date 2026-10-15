/*
 * ui_status.c - the status screen (see ui_status.h).
 *
 * Built from plain LVGL objects and the layout's fonts (read-only data, no RAM), so it costs a
 * few kilobytes where the touch UI needs a few hundred. The words change at most once a second
 * (one timer), and only when they differ: nothing on it flickers (docs/features/07 UI-020). The ring
 * follows the finger only while it is down; it is the panel's view of the touch, so a ring
 * that sits beside the finger means the touch needs calibrating.
 */
#include "ui_status.h"

#include <stdio.h>
#include <string.h>

#include "alarm/alarm_snapshot.h"
#include "ui_internal.h"

/* The status screen is a low-RAM board's fallback for the whole UI: it has no appearance
 * setting and keeps Dark's colours (docs/features/07 UI-010) - its fixed background and words
 * below, and the tones, which follow the current palette, taken back to Dark's. */
static lv_color_t dark(lv_color_t c)
{
    return alarm_ui_theme_color(c, alarm_ui_palettes[ALARM_UI_THEME_DARK]);
}

/* The layout's fonts (components/ui/alarm_ui/layouts/<WxH>/alarm_ui/fonts), used without the layout's styles and subjects. */
extern lv_font_t font_caption_data;
extern lv_font_t font_body_data;
extern lv_font_t font_title_data;
extern lv_font_t font_headline_data;

#define STATUS_TICK_MS 1000u
#define RING_SIZE      56

typedef struct {
    ui_backend_t be;
    uint8_t partition;
    uint8_t brightness;
    bool lit;
    uint32_t seq;
    bool have_seq;
    alarm_snapshot_t *snap; /* LVGL heap: only a panel on this screen pays for it */
    lv_obj_t *screen;
    lv_obj_t *time;
    lv_obj_t *headline;
    lv_obj_t *detail;
    lv_obj_t *troubles;
    lv_obj_t *ring;
} status_t;

static status_t *s_status;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_color(lv_obj_t *label, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(label, c, LV_PART_MAIN);
    }
}

/* The Home screen's headline and detail for a partition, without the zone names (this screen
 * has no configuration to read them from). The headline lands in a 48-byte buffer and the detail
 * in a 64-byte one. */
static void words(const alarm_snapshot_t *snap, const alarm_partition_status_t *ps, char *headline,
                  size_t hl, char *detail, size_t dl, lv_color_t *tone)
{
    const alarm_cause_t cause = ui_top_cause(ps->memory);
    switch (ps->sounder) {
    case ALARM_SOUND_FIRE:
        /* TR: the status screen's headline while the fire alarm sounds. At most 47 bytes */
        i18n_utf8_copy(headline, hl, TR("Fire alarm"));
        /* TR: the status screen, under the fire headline. At most 63 bytes */
        i18n_utf8_copy(detail, dl, TR("Get everyone out"));
        *tone = ui_tone_red();
        return;
    case ALARM_SOUND_CO:
        /* TR: the status screen's headline while the carbon monoxide alarm sounds. At most 47
         * bytes */
        i18n_utf8_copy(headline, hl, TR("Carbon monoxide"));
        /* TR: the status screen, under the carbon monoxide headline. At most 63 bytes */
        i18n_utf8_copy(detail, dl, TR("Get everyone into fresh air"));
        *tone = ui_tone_red();
        return;
    case ALARM_SOUND_ALARM:
    case ALARM_SOUND_AUX:
        i18n_utf8_copy(headline, hl, ui_cause_title(cause));
        /* TR: the status screen, under the alarm headline; {0} is how many zones. At most 60
         * bytes */
        I18N_FORMAT(detail, dl, TRN("{0} zone in alarm", "{0} zones in alarm", ps->alarms),
                    (unsigned)ps->alarms);
        *tone = ui_tone_red();
        return;
    default:
        break;
    }
    switch (ps->state) {
    case ALARM_STATE_ENTRY_DELAY:
        /* TR: the status screen's headline during entry time. At most 47 bytes */
        i18n_utf8_copy(headline, hl, TR("Disarm now"));
        /* TR: the status screen, under "Disarm now". At most 63 bytes */
        i18n_utf8_copy(detail, dl, TR("The alarm sounds when time runs out"));
        *tone = ui_tone_orange();
        return;
    case ALARM_STATE_EXIT_DELAY:
        /* TR: the status screen's headline during exit time; Away, Stay and Night are the
         * arming modes. At most 47 bytes */
        i18n_utf8_copy(headline, hl,
                       ps->level == ALARM_LEVEL_STAY    ? TR("Arming Stay")
                       : ps->level == ALARM_LEVEL_NIGHT ? TR("Arming Night")
                                                        : TR("Arming Away"));
        /* TR: the status screen, under the arming headline. At most 63 bytes */
        i18n_utf8_copy(detail, dl,
                       ps->level == ALARM_LEVEL_AWAY ? TR("Leave now through the exit door")
                                                     : TR("Stay inside, arming shortly"));
        *tone = ui_tone_for_level(ps->level);
        return;
    case ALARM_STATE_ARMED:
        if (ps->alarms > 0u) {
            i18n_utf8_copy(headline, hl, ui_cause_title(cause));
            /* TR: the status screen, under the alarm headline once the siren stopped. At most 63
             * bytes */
            i18n_utf8_copy(detail, dl, TR("The siren timed out"));
            *tone = ui_tone_red();
            return;
        }
        /* TR: the status screen's headline once armed; Away, Stay and Night are the arming
         * modes. At most 47 bytes */
        i18n_utf8_copy(headline, hl,
                       ps->level == ALARM_LEVEL_STAY    ? TR("Armed Stay")
                       : ps->level == ALARM_LEVEL_NIGHT ? TR("Armed Night")
                                                        : TR("Armed Away"));
        /* TR: the status screen, under the armed headline. At most 63 bytes */
        i18n_utf8_copy(detail, dl,
                       ps->level == ALARM_LEVEL_AWAY   ? TR("The whole house is protected")
                       : ps->level == ALARM_LEVEL_STAY ? TR("Doors and windows are protected")
                                                       : TR("Doors, windows and night zones"));
        *tone = ui_tone_for_level(ps->level);
        return;
    default:
        break;
    }
    if (ps->walk_test) {
        /* TR: the status screen's headline during a walk test. At most 47 bytes */
        i18n_utf8_copy(headline, hl, TR("Walk test"));
        /* TR: the status screen, under "Walk test". At most 63 bytes */
        i18n_utf8_copy(detail, dl, TR("Open and close each zone"));
        *tone = ui_tone_blue();
    } else if ((ps->memory & (uint16_t)~ALARM_MEM_CANCELED) != 0u || ps->alarms > 0u) {
        /* TR: the status screen's headline after an alarm. At most 47 bytes */
        i18n_utf8_copy(headline, hl,
                       (ps->memory & ALARM_MEM_CANCELED) ? TR("Alarm canceled")
                                                         : TR("Alarm occurred"));
        /* TR: the status screen, under "Alarm canceled". At most 63 bytes */
        i18n_utf8_copy(detail, dl,
                       (ps->memory & ALARM_MEM_CANCELED) ? TR("No alert was sent")
                                                         : ui_cause_title(cause));
        *tone = ui_tone_amber();
    } else if (snap->program) {
        /* TR: the status screen's headline while the installer changes settings. At most 47
         * bytes */
        i18n_utf8_copy(headline, hl, TR("Installer mode"));
        /* TR: the status screen, under "Installer mode". At most 63 bytes */
        i18n_utf8_copy(detail, dl, TR("Settings are being changed"));
        *tone = ui_tone_blue();
    } else if (ps->ready) {
        /* TR: the status screen's headline when the panel can be armed. At most 47 bytes */
        i18n_utf8_copy(headline, hl, TR("Ready to arm"));
        /* TR: the status screen, under "Ready to arm". At most 63 bytes */
        i18n_utf8_copy(detail, dl, TR("All zones are closed"));
        *tone = ui_tone_green();
    } else {
        /* TR: the status screen's headline when a zone is open. At most 47 bytes */
        i18n_utf8_copy(headline, hl, TR("Not ready"));
        /* TR: the status screen, under "Not ready"; {0} is how many zones. At most 60 bytes */
        I18N_FORMAT(detail, dl, TRN("{0} zone is open", "{0} zones are open", ps->faults),
                    (unsigned)ps->faults);
        *tone = ui_tone_amber();
    }
}

static void refresh(status_t *st)
{
    const bool changed = st->be.snapshot_seq == NULL || !st->have_seq ||
                         st->be.snapshot_seq(st->be.ctx) != st->seq;
    if (changed) {
        if (st->be.snapshot_seq != NULL) {
            st->seq = st->be.snapshot_seq(st->be.ctx);
            st->have_seq = true;
        }
        st->be.get_snapshot(st->be.ctx, st->snap);
        const alarm_partition_status_t *ps = &st->snap->part[st->partition - 1u];
        char headline[48];
        char detail[64];
        lv_color_t tone = ui_tone_gray();
        words(st->snap, ps, headline, sizeof(headline), detail, sizeof(detail), &tone);
        set_text(st->headline, headline);
        set_color(st->headline, dark(tone));
        set_text(st->detail, detail);
        char troubles[32] = "";
        if (ps->troubles > 0u) {
            /* TR: the status screen, under the detail; {0} is how many troubles. At most 21
             * bytes */
            I18N_FORMAT(troubles, sizeof(troubles), TRN("{0} trouble", "{0} troubles", ps->troubles),
                        (unsigned)ps->troubles);
        }
        set_text(st->troubles, troubles);
    }
    const int64_t epoch = st->be.epoch_ms != NULL ? st->be.epoch_ms(st->be.ctx) : 0;
    char clock[16] = "";
    if (epoch > 0) {
        ui_clock_text(epoch, clock, sizeof(clock), true);
    }
    set_text(st->time, clock);
}

static void tick(lv_timer_t *t)
{
    status_t *st = lv_timer_get_user_data(t);
    if (!st->lit) {
        st->lit = true; /* the first frame is in the panel by now: no noise on power-up */
        if (st->be.backlight != NULL) {
            st->be.backlight(st->be.ctx, st->brightness);
        }
    }
    refresh(st);
}

static void on_touch(lv_event_t *e)
{
    status_t *st = lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        /* A child's position is measured from inside the screen's padding. */
        const int32_t x = p.x - RING_SIZE / 2 - lv_obj_get_style_pad_left(st->screen, LV_PART_MAIN);
        const int32_t y = p.y - RING_SIZE / 2 - lv_obj_get_style_pad_top(st->screen, LV_PART_MAIN);
        lv_obj_set_pos(st->ring, x, y);
        lv_obj_remove_flag(st->ring, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(st->ring, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    if (l != NULL) {
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    }
    return l;
}

bool ui_status_start(const ui_backend_t *backend, const ui_status_options_t *opt)
{
    if (s_status != NULL || backend == NULL || backend->get_snapshot == NULL) {
        return s_status != NULL;
    }
    status_t *st = lv_malloc_zeroed(sizeof(*st));
    alarm_snapshot_t *snap = lv_malloc_zeroed(sizeof(*snap));
    lv_obj_t *scr = (st != NULL && snap != NULL) ? lv_obj_create(NULL) : NULL;
    if (scr == NULL) {
        lv_free(snap);
        lv_free(st);
        return false;
    }
    st->be = *backend;
    st->snap = snap;
    st->screen = scr;
    st->partition = (opt != NULL && opt->partition != 0u && opt->partition <= ALARM_MAX_PARTITIONS)
                        ? opt->partition
                        : 1u;
    st->brightness = opt != NULL ? opt->brightness : 60u;

    const lv_color_t text = lv_color_hex(0xE2E8F0);
    const lv_color_t muted = dark(ui_tone_gray());
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1120), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 16, LV_PART_MAIN);

    lv_obj_t *board = label(scr, &font_caption_data, muted);
    st->time = label(scr, &font_title_data, text);
    st->headline = label(scr, &font_headline_data, muted);
    st->detail = label(scr, &font_body_data, text);
    st->troubles = label(scr, &font_body_data, ui_tone_amber());
    lv_obj_t *note = label(scr, &font_caption_data, muted);
    st->ring = lv_obj_create(scr);
    if (board == NULL || st->time == NULL || st->headline == NULL || st->detail == NULL ||
        st->troubles == NULL || note == NULL || st->ring == NULL) {
        lv_obj_delete(scr);
        lv_free(snap);
        lv_free(st);
        return false;
    }
    lv_label_set_text(board, opt != NULL && opt->board != NULL ? opt->board : "");
    lv_obj_align(board, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_align(st->time, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_name(st->headline, "status_headline");
    lv_obj_set_name(st->ring, "status_ring");
    /* TR: the status screen's headline until the first state arrives. At most 47 bytes */
    lv_label_set_text(st->headline, TR("Starting"));
    lv_obj_align(st->headline, LV_ALIGN_CENTER, 0, -34);
    lv_obj_align(st->detail, LV_ALIGN_CENTER, 0, 4);
    lv_obj_align(st->troubles, LV_ALIGN_CENTER, 0, 34);
    lv_label_set_text(note, opt != NULL && opt->note != NULL
                                ? opt->note
                                : TR("Status only: this panel has no room for its touch screen "
                                     "yet. A ring shows where it feels a touch."));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, lv_pct(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_remove_style_all(st->ring);
    lv_obj_add_flag(st->ring, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(st->ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(st->ring, RING_SIZE, RING_SIZE);
    lv_obj_set_style_radius(st->ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(st->ring, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(st->ring, text, LV_PART_MAIN);
    lv_obj_set_style_border_opa(st->ring, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_PRESSED, st);
    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_PRESSING, st);
    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_RELEASED, st);
    lv_obj_add_event_cb(scr, on_touch, LV_EVENT_PRESS_LOST, st);

    s_status = st;
    refresh(st);
    lv_screen_load(scr);
    lv_timer_create(tick, STATUS_TICK_MS, st);
    return true;
}
