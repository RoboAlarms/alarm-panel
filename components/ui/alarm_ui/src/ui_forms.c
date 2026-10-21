/*
 * ui_forms.c - building blocks of the settings and setup pages: rows (links, switches,
 * numbers, levels, actions, read-only values, hold-to-confirm), and the sheets that open over
 * the screen (text entry with the on-screen keyboard, a list of choices, a confirmation, a time and
 * its days, a window of time).
 * Rows are the LVGL Pro components list_row, row_switch, row_stepper, row_slider, row_button,
 * row_value, hold_button and section_title, created from C with a small binding each.
 */
#include <stdio.h>
#include <string.h>

#include "i18n/i18n_date.h"
#include "ui_internal.h"

#define MAX_ROWS     96u
#define HOLD_MS      2000u
#define GLYPH_SHIFT  "\xEE\x87\xA4" /* U+E1E4 arrow-big-up */
#define GLYPH_DEL    "\xEE\x82\xAE" /* U+E0AE delete */
#define GLYPH_ENTER  "\xEE\x82\xA1" /* U+E0A1 corner-down-left */
#define GLYPH_CHECK  UI_ICON_CHECK
#define GLYPH_CHEVRON "\xEE\x81\xAF" /* U+E06F chevron-right */

typedef enum { ROW_LINK, ROW_TOGGLE, ROW_NUMBER, ROW_LEVEL, ROW_ACTION, ROW_HOLD } row_kind_t;

typedef struct {
    row_kind_t kind;
    void *ctx;
    ui_click_fn click;
    ui_toggle_fn toggle;
    ui_number_fn number;
    int32_t value, min, max, step;
    ui_unit_t unit;
    const char *zero_text;
    lv_obj_t *obj;
    lv_obj_t *label;
    lv_obj_t *fill;
    bool beep_on_release;
    bool holding;
    bool fired;
    uint32_t hold_start;
} row_t;

static row_t *s_rows; /* MAX_ROWS of them, from ui_slow_alloc() */
static size_t s_nrows;

static int32_t s_sheet_kind;
static ui_text_fn s_text_fn;
static ui_confirm_fn s_cancel_fn; /* ui_confirm_choice: what its cancel button does */
static ui_pick_fn s_pick_fn;
static ui_confirm_fn s_confirm_fn;
static const ui_choice_t *s_choices;
static ui_sheet_ok_fn s_ok_fn;       /* a sheet of its own (ui_sheet_open): what its OK does */
static ui_confirm_fn s_closed_fn;    /* ... and what runs once it is gone */
static ui_time_sheet_fn s_time_fn;     /* the time sheet: what runs as it changes */
static ui_window_sheet_fn s_window_fn; /* the window sheet: the same */
static ui_confirm_fn s_any_fn;         /* ... and what its "Any time" does */
static ui_confirm_fn s_owner_fn;       /* ui_sheet_owner: ends the owner once its sheet is gone */
static uint8_t s_day_bit[7];           /* day_N's day (0 Monday .. 6 Sunday), in the week's order */
static lv_font_t s_key_font;  /* font_label + Lucide for the special keys ... */
static lv_font_t s_key_icons; /* ... then font_label's own chain: the letters pack's letters */

/* ---- rows ------------------------------------------------------------------------------ */

static row_t *row_new(row_kind_t kind, void *ctx)
{
    if (s_nrows >= MAX_ROWS) {
        LV_LOG_WARN("too many settings rows");
        return NULL;
    }
    if (s_rows == NULL && (s_rows = ui_slow_alloc(MAX_ROWS * sizeof(*s_rows))) == NULL) {
        return NULL;
    }
    row_t *r = &s_rows[s_nrows++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->ctx = ctx;
    return r;
}

void ui_rows_reset(void)
{
    ui_net_stop(); /* its list lives in the page being dropped */
    ui_integrations_stop();
    ui_notify_stop();
    ui_update_stop();
    ui_storage_stop();
    ui_sources_stop();
#if ALARM_UI_ZWAVE
    ui_zwave_stop();
#endif
#if ALARM_UI_ZIGBEE
    ui_zigbee_stop();
#endif
    ui_sirens_stop();
    ui_link_stop(); /* leaving Add a keypad closes pairing (BL-048) */
    /* Rows of pages that aren't on screen could point at reused bindings: drop them all. */
    if (S.page_body != NULL) {
        lv_obj_clean(S.page_body);
    }
    lv_obj_t *wiz_body = ui_find(S.wizard, "wiz_body");
    if (wiz_body != NULL) {
        lv_obj_clean(wiz_body);
    }
    s_nrows = 0;
}

size_t ui_rows_mark(void)
{
    return s_nrows;
}

void ui_rows_release(size_t mark)
{
    if (mark <= s_nrows) {
        s_nrows = mark;
    }
}

static void show_if(lv_obj_t *obj, bool show)
{
    if (obj != NULL) {
        lv_obj_set_flag(obj, LV_OBJ_FLAG_HIDDEN, !show);
    }
}

static void on_row_click(lv_event_t *e)
{
    row_t *r = (row_t *)lv_event_get_user_data(e);
    if (r != NULL && r->click != NULL) {
        r->click(r->ctx);
    }
}

lv_obj_t *ui_row_link(lv_obj_t *parent, const char *glyph, lv_color_t tone, const char *title,
                      const char *detail, const char *value, const char *trail, ui_click_fn fn,
                      void *ctx)
{
    lv_obj_t *row = list_row_create(parent, glyph != NULL ? glyph : "", tone, title,
                                    detail != NULL ? detail : "", value != NULL ? value : "",
                                    trail != NULL ? trail : "");
    show_if(ui_find(row, "chip"), glyph != NULL && glyph[0] != '\0');
    show_if(ui_find(row, "detail_label"), detail != NULL && detail[0] != '\0');
    show_if(ui_find(row, "value_label"), value != NULL && value[0] != '\0');
    show_if(ui_find(row, "trail_label"), trail != NULL && trail[0] != '\0');
    show_if(ui_find(row, "level"), false); /* a status row turns its bar on (ui_storage.c) */
    if (fn == NULL) {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        return row;
    }
    row_t *r = row_new(ROW_LINK, ctx);
    if (r != NULL) {
        r->click = fn;
        r->obj = row;
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, r);
    }
    return row;
}

static void on_toggle(lv_event_t *e)
{
    row_t *r = (row_t *)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target_obj(e);
    if (r != NULL && r->toggle != NULL) {
        r->toggle(lv_obj_has_state(sw, LV_STATE_CHECKED), r->ctx);
    }
}

static lv_obj_t *toggle_finish(lv_obj_t *row, const char *detail, bool on, ui_toggle_fn fn,
                               void *ctx)
{
    show_if(ui_find(row, "detail_label"), detail != NULL && detail[0] != '\0');
    lv_obj_t *sw = ui_find(row, "toggle");
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    row_t *r = row_new(ROW_TOGGLE, ctx);
    if (r != NULL) {
        r->toggle = fn;
        r->obj = row;
        lv_obj_add_event_cb(sw, on_toggle, LV_EVENT_VALUE_CHANGED, r);
    }
    return row;
}

lv_obj_t *ui_row_toggle(lv_obj_t *parent, const char *title, const char *detail, bool on,
                        ui_toggle_fn fn, void *ctx)
{
    return toggle_finish(row_switch_create(parent, title, detail != NULL ? detail : ""), detail, on,
                         fn, ctx);
}

lv_obj_t *ui_row_toggle_icon(lv_obj_t *parent, const char *glyph, lv_color_t tone,
                             const char *title, const char *detail, bool on, ui_toggle_fn fn,
                             void *ctx)
{
    lv_obj_t *row = row_switch_icon_create(parent, glyph, tone, title, detail != NULL ? detail : "");
    return toggle_finish(row, detail, on, fn, ctx);
}

static void number_text(const row_t *r, char *buf, size_t len)
{
    if (r->value == 0 && r->zero_text != NULL) {
        i18n_utf8_copy(buf, len, r->zero_text);
        return;
    }
    switch (r->unit) {
    case UNIT_SECONDS:
        /* TR: a number of seconds in a number row; {0} is the number. At most 20 bytes */
        I18N_FORMAT(buf, len, TRN("{0} s", "{0} s", (uint32_t)r->value), (int)r->value);
        break;
    case UNIT_MINUTES:
        /* TR: a number of minutes in a number row; {0} is the number. At most 20 bytes */
        I18N_FORMAT(buf, len, TRN("{0} min", "{0} min", (uint32_t)(r->value / 60)),
                    (int)(r->value / 60));
        break;
    case UNIT_DIGITS:
        /* TR: a count in a number row, for example the length of a code; {0} is the number. At
         * most 20 bytes */
        I18N_FORMAT(buf, len, TRN("{0} digit", "{0} digits", (uint32_t)r->value), (int)r->value);
        break;
    case UNIT_TIMES:
        /* TR: a count in a number row, for example how many times a siren may sound; {0} is the
         * number. At most 20 bytes */
        I18N_FORMAT(buf, len, TRN("{0} time", "{0} times", (uint32_t)r->value), (int)r->value);
        break;
    case UNIT_MONTH: {
        i18n_civil_t c = {.year = 2026, .month = 1, .day = 1};
        c.month = (uint8_t)((r->value >= 1 && r->value <= 12) ? r->value : 1);
        /* TR: the month's name alone in a number row (Set the date and time), in CLDR pattern
         * letters (MMMM month name); a language may need the form used on its own. At most 30
         * bytes of result */
        i18n_date(buf, len, S.catalog, TR_C("date pattern", "MMMM"), &c);
        break;
    }
    case UNIT_HOUR: {
        i18n_civil_t c = {.year = 2026, .month = 1, .day = 1};
        c.hour = (uint8_t)(r->value >= 0 && r->value <= 23 ? r->value : 0);
        /* TR: an hour in a number row on a 24-hour clock, in CLDR pattern letters (HH hour
         * 00-23). At most 15 bytes of result */
        const char *const pattern24 = TR_C("date pattern", "HH");
        /* TR: an hour in a number row on a 12-hour clock, in CLDR pattern letters (h hour 1-12, a
         * AM/PM). The order may change. At most 15 bytes of result */
        const char *const pattern12 = TR_C("date pattern", "h a");
        i18n_date(buf, len, S.catalog, ui_clock_24h() ? pattern24 : pattern12, &c);
        break;
    }
    case UNIT_TWO_DIGITS: snprintf(buf, len, "%02d", (int)r->value); break;
    case UNIT_TIME_OF_DAY: ui_minute_text((int)r->value, buf, len); break;
    default: snprintf(buf, len, "%d", (int)r->value); break;
    }
}

static void on_step(lv_event_t *e)
{
    row_t *r = (row_t *)lv_event_get_user_data(e);
    if (r == NULL) {
        return;
    }
    const bool plus = lv_event_get_current_target_obj(e) == ui_find(r->obj, "plus");
    int32_t v = r->value + (plus ? r->step : -r->step);
    if (r->unit == UNIT_TIME_OF_DAY) {
        v = (v % 1440 + 1440) % 1440; /* 23:45 and a quarter on is 00:00 */
    }
    v = v < r->min ? r->min : v > r->max ? r->max : v;
    if (v == r->value) {
        return;
    }
    r->value = v;
    char text[32];
    number_text(r, text, sizeof(text));
    lv_label_set_text(r->label, text);
    if (r->number != NULL) {
        r->number(v, r->ctx);
    }
}

lv_obj_t *ui_row_number(lv_obj_t *parent, const char *title, const char *detail, int32_t value,
                        int32_t min, int32_t max, int32_t step, ui_unit_t unit,
                        const char *zero_text, ui_number_fn fn, void *ctx)
{
    lv_obj_t *row = row_stepper_create(parent, title, detail != NULL ? detail : "", "");
    show_if(ui_find(row, "detail_label"), detail != NULL && detail[0] != '\0');
    row_t *r = row_new(ROW_NUMBER, ctx);
    if (r == NULL) {
        return row;
    }
    r->obj = row;
    r->label = ui_find(row, "value_label");
    r->value = value;
    r->min = min;
    r->max = max;
    r->step = step;
    r->unit = unit;
    r->zero_text = zero_text;
    r->number = fn;
    char text[32];
    number_text(r, text, sizeof(text));
    lv_label_set_text(r->label, text);
    lv_obj_t *minus = ui_find(row, "minus");
    lv_obj_t *plus = ui_find(row, "plus");
    lv_obj_add_event_cb(minus, on_step, LV_EVENT_SHORT_CLICKED, r);
    lv_obj_add_event_cb(minus, on_step, LV_EVENT_LONG_PRESSED_REPEAT, r);
    lv_obj_add_event_cb(plus, on_step, LV_EVENT_SHORT_CLICKED, r);
    lv_obj_add_event_cb(plus, on_step, LV_EVENT_LONG_PRESSED_REPEAT, r);
    return row;
}

static void on_level(lv_event_t *e)
{
    row_t *r = (row_t *)lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (r == NULL) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (r->beep_on_release) {
            ui_beep(UI_BEEP_TEST);
        }
        return;
    }
    r->value = lv_slider_get_value(slider);
    char text[16];
    /* TR: a level in percent; {0} is the number. At most 12 bytes */
    I18N_FORMAT(text, sizeof(text), TR_C("percent", "{0}%"), (int)r->value);
    lv_label_set_text(r->label, text);
    if (r->number != NULL) {
        r->number(r->value, r->ctx);
    }
}

lv_obj_t *ui_row_level(lv_obj_t *parent, const char *glyph, const char *title, int32_t value,
                       int32_t min, int32_t max, ui_number_fn fn, void *ctx, bool beep_on_release)
{
    char text[16];
    /* TR: a level in percent; {0} is the number. At most 12 bytes */
    I18N_FORMAT(text, sizeof(text), TR_C("percent", "{0}%"), (int)value);
    lv_obj_t *row = row_slider_create(parent, glyph, title, text);
    lv_obj_t *slider = ui_find(row, "slider");
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    row_t *r = row_new(ROW_LEVEL, ctx);
    if (r != NULL) {
        r->obj = row;
        r->label = ui_find(row, "value_label");
        r->value = value;
        r->number = fn;
        r->beep_on_release = beep_on_release;
        lv_obj_add_event_cb(slider, on_level, LV_EVENT_VALUE_CHANGED, r);
        lv_obj_add_event_cb(slider, on_level, LV_EVENT_RELEASED, r);
    }
    return row;
}

lv_obj_t *ui_row_action(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t tone,
                        ui_click_fn fn, void *ctx)
{
    lv_obj_t *row = row_button_create(parent, glyph, text, tone);
    row_t *r = row_new(ROW_ACTION, ctx);
    if (r != NULL) {
        r->click = fn;
        r->obj = row;
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, r);
    }
    return row;
}

lv_obj_t *ui_row_value(lv_obj_t *parent, const char *title, const char *value)
{
    return row_value_create(parent, title, value);
}

static void on_hold(lv_event_t *e)
{
    row_t *r = (row_t *)lv_event_get_user_data(e);
    if (r == NULL) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        r->holding = true;
        r->fired = false;
        r->hold_start = lv_tick_get();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_INDEV_RESET) { /* a remote pointer that dropped mid-press */
        if (r->holding && !r->fired) {
            ui_toast(TR("Keep holding for 2 seconds"));
        }
        r->holding = false;
        lv_bar_set_value(r->fill, 0, LV_ANIM_OFF);
    }
}

lv_obj_t *ui_row_hold(lv_obj_t *parent, const char *text, ui_click_fn fn, void *ctx)
{
    lv_obj_t *row = hold_button_create(parent, text, ui_tone_red());
    row_t *r = row_new(ROW_HOLD, ctx);
    if (r != NULL) {
        r->click = fn;
        r->obj = row;
        r->fill = ui_find(row, "fill");
        lv_obj_add_event_cb(row, on_hold, LV_EVENT_PRESSED, r);
        lv_obj_add_event_cb(row, on_hold, LV_EVENT_RELEASED, r);
        lv_obj_add_event_cb(row, on_hold, LV_EVENT_PRESS_LOST, r);
        lv_obj_add_event_cb(row, on_hold, LV_EVENT_INDEV_RESET, r);
    }
    return row;
}

void ui_forms_tick(void)
{
    for (size_t i = 0; i < s_nrows; i++) {
        row_t *r = &s_rows[i];
        if (r->kind != ROW_HOLD || !r->holding || r->fired) {
            continue;
        }
        const uint32_t held = lv_tick_elaps(r->hold_start);
        lv_bar_set_value(r->fill, (int32_t)(held >= HOLD_MS ? 100u : held * 100u / HOLD_MS),
                         LV_ANIM_OFF);
        if (held >= HOLD_MS) {
            r->fired = true;
            r->holding = false;
            if (r->click != NULL) {
                r->click(r->ctx);
            }
            return; /* the callback may have rebuilt the page */
        }
    }
}

lv_obj_t *ui_section(lv_obj_t *parent, const char *text)
{
    return section_title_create(parent, text);
}

lv_obj_t *ui_paragraph(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_pad_left(l, 4, 0);
    lv_obj_set_style_pad_right(l, 4, 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_row_disable(lv_obj_t *row)
{
    if (row == NULL || lv_obj_has_state(row, LV_STATE_DISABLED)) {
        return;
    }
    /* style_disabled dims the row and all it holds (opa is drawn through to the children) */
    lv_obj_add_style(row, &style_disabled, LV_STATE_DISABLED);
    lv_obj_add_state(row, LV_STATE_DISABLED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE); /* a tap goes to the list: it still scrolls */
    lv_obj_t *toggle = ui_find(row, "toggle");
    if (toggle != NULL) {
        lv_obj_add_state(toggle, LV_STATE_DISABLED);
        lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t *ui_row_sub(lv_obj_t *row, const char *text, lv_color_t tone)
{
    lv_obj_t *detail = ui_find(row, "detail_label");
    if (detail == NULL) {
        return NULL;
    }
    const bool show = text != NULL && text[0] != '\0';
    lv_obj_t *sub = ui_find(row, "sub_label");
    if (sub == NULL) {
        if (!show) {
            return NULL;
        }
        sub = lv_label_create(lv_obj_get_parent(detail)); /* the words' column, under the detail */
        lv_obj_set_name_static(sub, "sub_label");
        lv_obj_set_width(sub, lv_pct(100));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(sub, lv_obj_get_style_text_font(detail, LV_PART_MAIN), 0);
        lv_obj_move_to_index(sub, lv_obj_get_index(detail) + 1);
        lv_label_set_text(sub, "");
    }
    if (show) {
        ui_set_label(row, "sub_label", text);
        lv_obj_set_style_text_color(sub, tone, 0);
    }
    show_if(sub, show);
    return sub;
}

void ui_row_set_detail(lv_obj_t *row, const char *text, bool alert, lv_color_t tone)
{
    lv_obj_t *d = ui_find(row, "detail_label");
    if (d == NULL) {
        return;
    }
    ui_set_label(row, "detail_label", text);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(d, alert ? tone : ui_tone_gray(), 0); /* gray is the row's own */
}

lv_obj_t *ui_big_value(lv_obj_t *parent, const char *label, const char *value)
{
    return big_value_create(parent, label, value);
}

void ui_action_pair(lv_obj_t *parent, const char *left_glyph, const char *left_text,
                    lv_color_t left_tone, ui_click_fn left_fn, const char *right_glyph,
                    const char *right_text, lv_color_t right_tone, ui_click_fn right_fn, void *ctx)
{
    lv_obj_t *pair = action_pair_create(parent);
    ui_row_action(ui_find(pair, "left"), left_glyph, left_text, left_tone, left_fn, ctx);
    ui_row_action(ui_find(pair, "right"), right_glyph, right_text, right_tone, right_fn, ctx);
}

/* ---- keyboard ---------------------------------------------------------------------------- */

/* Key widths are relative units within a row (4 bits, 1..15); rows are 40 units wide,
 * the bottom row 20. */
#define HIDE LV_BUTTONMATRIX_CTRL_HIDDEN
#define SPEC LV_BUTTONMATRIX_CTRL_CHECKED

/* keyboard layouts per language: G3 */
/* The accent layer (I18N-019): two pages of the accented letters of the Latin alphabets
 * the fonts carry, reached from the letters' KEY_ACCENT1 and back with abc; shift gives the
 * capitals of the page shown. The sharp s is its own capital (U+1E9E is not in the fonts). */
#define KEY_ACCENT1 NOTR("\xC3\xA0\xC3\xA9") /* the first page, from the letters too */
#define KEY_ACCENT2 NOTR("\xC4\x8D\xC5\x82") /* the second page */
static const char *const MAP_ACCENT1_LOWER[] = {
    "\xC3\xA0", "\xC3\xA1", "\xC3\xA2", "\xC3\xA4", "\xC3\xA3",
    "\xC3\xA5", "\xC3\xA6", "\xC3\xA7", "\xC3\xA8", "\xC3\xA9",
    "\n",
    "\xC3\xAA", "\xC3\xAB", "\xC3\xAC", "\xC3\xAD", "\xC3\xAE",
    "\xC3\xAF", "\xC3\xB1", "\xC3\xB2", "\xC3\xB3", "\xC3\xB4",
    "\n",
    GLYPH_SHIFT,
    "\xC3\xB5", "\xC3\xB6", "\xC3\xB8", "\xC3\xB9", "\xC3\xBA",
    "\xC3\xBC", "\xC3\x9F",
    GLYPH_DEL, "\n",
    NOTR("abc"), KEY_ACCENT2, NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const char *const MAP_ACCENT1_UPPER[] = {
    "\xC3\x80", "\xC3\x81", "\xC3\x82", "\xC3\x84", "\xC3\x83",
    "\xC3\x85", "\xC3\x86", "\xC3\x87", "\xC3\x88", "\xC3\x89",
    "\n",
    "\xC3\x8A", "\xC3\x8B", "\xC3\x8C", "\xC3\x8D", "\xC3\x8E",
    "\xC3\x8F", "\xC3\x91", "\xC3\x92", "\xC3\x93", "\xC3\x94",
    "\n",
    GLYPH_SHIFT,
    "\xC3\x95", "\xC3\x96", "\xC3\x98", "\xC3\x99", "\xC3\x9A",
    "\xC3\x9C", "\xC3\x9F",
    GLYPH_DEL, "\n",
    NOTR("abc"), KEY_ACCENT2, NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const char *const MAP_ACCENT2_LOWER[] = {
    "\xC4\x8D", "\xC4\x87", "\xC4\x8F", "\xC4\x9B", "\xC4\x99",
    "\xC4\x85", "\xC5\x82", "\xC4\xBE", "\xC5\x84", "\xC5\x88",
    "\n",
    "\xC5\x91", "\xC5\x99", "\xC5\x9B", "\xC5\xA1", "\xC5\xA5",
    "\xC5\xAF", "\xC5\xB1", "\xC5\xBA", "\xC5\xBC", "\xC5\xBE",
    "\n",
    GLYPH_SHIFT,
    "\xC3\xBB", "\xC5\x93", "\xC3\xBD", "\xC3\xBF", "\xC4\x9F",
    "\xC5\x9F", "\xC4\xB1",
    GLYPH_DEL, "\n",
    NOTR("abc"), KEY_ACCENT1, NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const char *const MAP_ACCENT2_UPPER[] = {
    "\xC4\x8C", "\xC4\x86", "\xC4\x8E", "\xC4\x9A", "\xC4\x98",
    "\xC4\x84", "\xC5\x81", "\xC4\xBD", "\xC5\x83", "\xC5\x87",
    "\n",
    "\xC5\x90", "\xC5\x98", "\xC5\x9A", "\xC5\xA0", "\xC5\xA4",
    "\xC5\xAE", "\xC5\xB0", "\xC5\xB9", "\xC5\xBB", "\xC5\xBD",
    "\n",
    GLYPH_SHIFT,
    "\xC3\x9B", "\xC5\x92", "\xC3\x9D", "\xC5\xB8", "\xC4\x9E",
    "\xC5\x9E", "\xC4\xB0",
    GLYPH_DEL, "\n",
    NOTR("abc"), KEY_ACCENT1, NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const lv_buttonmatrix_ctrl_t CTRL_ACCENT[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    6 | SPEC, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC,
    3 | SPEC, 2 | SPEC, 8, 2, 2, 3 | SPEC}; /* widths are 4 bits: at most 15 units */
static const lv_buttonmatrix_ctrl_t CTRL_ACCENT_UPPER[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    6, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC, /* shift lit: capitals are on */
    3 | SPEC, 2 | SPEC, 8, 2, 2, 3 | SPEC}; /* widths are 4 bits: at most 15 units */

static const char *const MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    GLYPH_SHIFT, "z", "x", "c", "v", "b", "n", "m", GLYPH_DEL, "\n",
    NOTR("123"), KEY_ACCENT1, NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const char *const MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    GLYPH_SHIFT, "Z", "X", "C", "V", "B", "N", "M", GLYPH_DEL, "\n",
    NOTR("123"), KEY_ACCENT1, NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const lv_buttonmatrix_ctrl_t CTRL_TEXT[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    2 | HIDE, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2 | HIDE,
    6 | SPEC, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC,
    3 | SPEC, 2 | SPEC, 8, 2, 2, 3 | SPEC}; /* widths are 4 bits: at most 15 units */
static const lv_buttonmatrix_ctrl_t CTRL_TEXT_UPPER[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    2 | HIDE, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2 | HIDE,
    6, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC, /* shift lit: capitals are on */
    3 | SPEC, 2 | SPEC, 8, 2, 2, 3 | SPEC}; /* widths are 4 bits: at most 15 units */
/* The letters without the àé key (asset-packs.md "Decisions / The letters", OPT-014): while the
 * body font cannot draw U+00E0 - no letters pack, and a layout whose fonts lack the letters - the
 * accent pages would be keys of boxes, so the key is left out and the space bar takes its place
 * (the proposal approved 2026-10-10, docs/features/07 mockup screens/settings.js). */
static const char *const MAP_LOWER_PLAIN[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    GLYPH_SHIFT, "z", "x", "c", "v", "b", "n", "m", GLYPH_DEL, "\n",
    NOTR("123"), NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const char *const MAP_UPPER_PLAIN[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    GLYPH_SHIFT, "Z", "X", "C", "V", "B", "N", "M", GLYPH_DEL, "\n",
    NOTR("123"), NOTR("space"), "-", ".", GLYPH_ENTER, ""};
static const lv_buttonmatrix_ctrl_t CTRL_TEXT_PLAIN[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    2 | HIDE, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2 | HIDE,
    6 | SPEC, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC,
    3 | SPEC, 10, 2, 2, 3 | SPEC}; /* the space bar has the àé key's 2 units */
static const lv_buttonmatrix_ctrl_t CTRL_TEXT_UPPER_PLAIN[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    2 | HIDE, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2 | HIDE,
    6, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC, /* shift lit: capitals are on */
    3 | SPEC, 10, 2, 2, 3 | SPEC};
static const char *const MAP_NUMBERS[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    NOTR("#+="), ".", ",", "?", "!", "'", "_", "*", GLYPH_DEL, "\n",
    NOTR("abc"), "-", NOTR("space"), ".", GLYPH_ENTER, ""};
static const lv_buttonmatrix_ctrl_t CTRL_NUMBERS[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    6 | SPEC, 4, 4, 4, 4, 4, 4, 4, 6 | SPEC,
    3 | SPEC, 2, 10, 2, 3 | SPEC}; /* widths are 4 bits: at most 15 units */
static const char *const MAP_SYMBOLS[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "`", "'", "\"", "@", "\n",
    NOTR("123"), ".", ",", "?", "!", ";", ":", "/", GLYPH_DEL, "\n",
    NOTR("abc"), "-", NOTR("space"), ".", GLYPH_ENTER, ""};

/* The accent layer lives in LV_KEYBOARD_MODE_USER_2, its map swapped for the page and case. */
#define MODE_ACCENT LV_KEYBOARD_MODE_USER_2
static uint8_t s_accent_page; /* 0 or 1 */
static bool s_accent_upper;

static void kb_mode(lv_obj_t *kb, lv_keyboard_mode_t mode)
{
    lv_keyboard_set_mode(kb, mode);
}

static void kb_accents(lv_obj_t *kb, uint8_t page, bool upper)
{
    static const char *const *const maps[2][2] = {{MAP_ACCENT1_LOWER, MAP_ACCENT1_UPPER},
                                                   {MAP_ACCENT2_LOWER, MAP_ACCENT2_UPPER}};
    s_accent_page = page;
    s_accent_upper = upper;
    lv_keyboard_set_map(kb, MODE_ACCENT, maps[page][upper ? 1 : 0],
                        upper ? CTRL_ACCENT_UPPER : CTRL_ACCENT);
    lv_keyboard_set_mode(kb, MODE_ACCENT);
}

/* The letters' pages with or without the àé key: checked when a keyboard opens, never while one
 * is in use. lv_font_get_glyph_dsc follows the body font's fallback chain (the letters pack). */
static void kb_letters(lv_obj_t *kb)
{
    lv_font_glyph_dsc_t g;
    const bool accents = font_body != NULL && lv_font_get_glyph_dsc(font_body, &g, 0xE0u, 0u);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, accents ? MAP_LOWER : MAP_LOWER_PLAIN,
                        accents ? CTRL_TEXT : CTRL_TEXT_PLAIN);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, accents ? MAP_UPPER : MAP_UPPER_PLAIN,
                        accents ? CTRL_TEXT_UPPER : CTRL_TEXT_UPPER_PLAIN);
}

static void on_key(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target_obj(e);
    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (txt == NULL || ta == NULL) {
        return;
    }
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
    if (strcmp(txt, GLYPH_SHIFT) == 0 && mode == MODE_ACCENT) {
        kb_accents(kb, s_accent_page, !s_accent_upper);
    } else if (strcmp(txt, GLYPH_SHIFT) == 0) {
        kb_mode(kb, mode == LV_KEYBOARD_MODE_TEXT_UPPER ? LV_KEYBOARD_MODE_TEXT_LOWER
                                                        : LV_KEYBOARD_MODE_TEXT_UPPER);
    } else if (strcmp(txt, KEY_ACCENT1) == 0) {
        /* From capitals, the capitals of the accents: "É" as easily as "é". */
        kb_accents(kb, 0u,
                   mode == LV_KEYBOARD_MODE_TEXT_UPPER || (mode == MODE_ACCENT && s_accent_upper));
    } else if (strcmp(txt, KEY_ACCENT2) == 0) {
        kb_accents(kb, 1u, mode == MODE_ACCENT && s_accent_upper);
    } else if (strcmp(txt, "123") == 0) {
        kb_mode(kb, LV_KEYBOARD_MODE_SPECIAL);
    } else if (strcmp(txt, "#+=") == 0) {
        kb_mode(kb, LV_KEYBOARD_MODE_USER_1);
    } else if (strcmp(txt, "abc") == 0) {
        kb_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    } else if (strcmp(txt, GLYPH_DEL) == 0) {
        lv_textarea_delete_char(ta);
    } else if (strcmp(txt, GLYPH_ENTER) == 0) {
        ui_sheet_done();
    } else if (strcmp(txt, "space") == 0) {
        lv_textarea_add_char(ta, ' ');
    } else {
        lv_textarea_add_text(ta, txt);
        if (mode == LV_KEYBOARD_MODE_TEXT_UPPER) {
            kb_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER); /* one capital, like a phone */
        } else if (mode == MODE_ACCENT) {
            kb_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER); /* one accented letter, back to the letters */
        }
    }
}

static void setup_keyboard(lv_obj_t *kb)
{
    if (kb == NULL) {
        return;
    }
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, MAP_LOWER, CTRL_TEXT);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, MAP_UPPER, CTRL_TEXT_UPPER);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, MAP_NUMBERS, CTRL_NUMBERS);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, MAP_SYMBOLS, CTRL_NUMBERS);
    lv_keyboard_set_map(kb, MODE_ACCENT, MAP_ACCENT1_LOWER, CTRL_ACCENT);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(kb, &s_key_font, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, on_key, LV_EVENT_VALUE_CHANGED, NULL);
}

/* What the text sheet may hold, in bytes: a name is stored in bytes, and a letter outside ASCII
 * takes two or three of them, while LVGL's own limit counts characters (I18N-010). */
static uint32_t s_max_bytes;
static char s_insert_fit[128];

static void on_insert(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    const char *txt = lv_event_get_param(e);
    if (txt == NULL || s_max_bytes == 0u) {
        return;
    }
    const size_t have = strlen(lv_textarea_get_text(ta));
    const size_t room = have < s_max_bytes ? s_max_bytes - have : 0u;
    if (strlen(txt) <= room) {
        return;
    }
    /* Only whole characters, as many as fit (a paste from the remote keypad may be long). */
    const size_t cap = room < sizeof(s_insert_fit) - 1u ? room : sizeof(s_insert_fit) - 1u;
    const size_t n = i18n_utf8_fit(txt, cap);
    memcpy(s_insert_fit, txt, n);
    s_insert_fit[n] = '\0';
    lv_textarea_set_insert_replace(ta, s_insert_fit);
}

static void setup_text(lv_obj_t *ta)
{
    lv_textarea_set_password_bullet(ta, "\xE2\x80\xA2"); /* U+2022 */
    ui_forms_steady_cursor(ta);
    lv_obj_add_event_cb(ta, on_insert, LV_EVENT_INSERT, NULL);
}

void ui_forms_init(void)
{
    s_key_font = *font_label;
    s_key_icons = *icons_20;
    s_key_font.fallback = &s_key_icons;
    ui_forms_fonts_changed();
    setup_keyboard(ui_find(S.main, "sheet_kb"));
    setup_keyboard(ui_find(S.wizard, "sheet_kb"));
    setup_text(ui_find(S.main, "sheet_text"));
    setup_text(ui_find(S.wizard, "sheet_text"));
}

void ui_forms_fonts_changed(void)
{
    /* The accent pages' letters come from the letters pack (or a language's font) behind the icons */
    s_key_icons.fallback = font_label != NULL ? font_label->fallback : NULL;
}

/* The text cursor never blinks (UI-020). LVGL's theme gives the cursor a 500 ms on/off
 * animation in the focused state, and a style on a more specific state beats the sheet's own
 * cursor style whatever its origin: the zero has to be set on the same selector. */
void ui_forms_steady_cursor(lv_obj_t *ta)
{
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
}

/* ---- sheets ------------------------------------------------------------------------------ */

static lv_obj_t *on_screen(const char *name)
{
    return ui_find(lv_screen_active(), name);
}

/* The sheet on screen is closed or replaced: its owner (ui_sheet_owner) ends, once. */
static void sheet_owner_gone(void)
{
    const ui_confirm_fn fn = s_owner_fn;
    s_owner_fn = NULL;
    if (fn != NULL) {
        fn();
    }
}

/* A sheet's own button hands over to its callback: the owner is held while it runs, and ends
 * after it unless the callback put up a sheet the same owner took again (its next sheet). */
static ui_confirm_fn sheet_owner_hold(void)
{
    const ui_confirm_fn fn = s_owner_fn;
    s_owner_fn = NULL;
    return fn;
}

static void sheet_owner_release(ui_confirm_fn held)
{
    if (held != NULL && s_owner_fn != held) {
        held();
    }
}

void ui_sheet_owner(ui_confirm_fn end)
{
    s_owner_fn = end;
    if (end != NULL && s_sheet_kind == SHEET_NONE) {
        sheet_owner_gone(); /* its sheet didn't open */
    }
}

static void sheet_cancel_words(void)
{
    /* TR: the button that closes a sheet without doing anything. At most 127 bytes */
    ui_set_str(&subject_sheet_cancel, TR("Cancel"));
}

void ui_sheet_close(void)
{
    if (s_sheet_kind == SHEET_TEXT) {
        lv_obj_t *ta = on_screen("sheet_text");
        if (ta != NULL) {
            lv_textarea_set_text(ta, ""); /* don't keep passwords around */
        }
    }
    s_sheet_kind = SHEET_NONE;
    s_text_fn = NULL;
    s_pick_fn = NULL;
    s_confirm_fn = NULL;
    s_cancel_fn = NULL;
    s_choices = NULL;
    s_ok_fn = NULL;
    s_time_fn = NULL;
    s_window_fn = NULL;
    s_any_fn = NULL;
    const ui_confirm_fn closed = s_closed_fn;
    s_closed_fn = NULL;
    sheet_cancel_words();
    ui_set_int(&subject_sheet, SHEET_NONE);
    sheet_owner_gone();
    if (closed != NULL) {
        closed();
    }
}

void ui_sheet_open(int32_t kind, ui_sheet_ok_fn ok, ui_confirm_fn closed)
{
    ui_sheet_close(); /* whatever was open (a half-typed password too) */
    s_ok_fn = ok;
    s_closed_fn = closed;
    s_sheet_kind = kind;
    ui_set_int(&subject_sheet, kind);
}

void ui_forms_language_changed(void)
{
    if (s_sheet_kind == SHEET_NONE) {
        sheet_cancel_words();
    }
}

void ui_sheet_cancel(void)
{
    const ui_confirm_fn fn = s_sheet_kind == SHEET_CONFIRM || s_sheet_kind == SHEET_PICK ||
                                     s_sheet_kind == SHEET_TEXT
                                 ? s_cancel_fn
                                 : NULL;
    const ui_confirm_fn owner = sheet_owner_hold();
    ui_sheet_close();
    if (fn != NULL) {
        fn();
    }
    sheet_owner_release(owner);
}

void ui_text_sheet_kind(const char *title, const char *note, const char *initial,
                        ui_text_kind_t kind, uint32_t max_len, ui_text_fn done)
{
    lv_obj_t *ta = on_screen("sheet_text");
    lv_obj_t *kb = on_screen("sheet_kb");
    if (ta == NULL || kb == NULL) {
        return;
    }
    sheet_owner_gone(); /* it replaces the sheet on screen */
    ui_set_str(&subject_sheet_title, title);
    ui_set_str(&subject_sheet_note, note != NULL ? note : "");
    /* TR: the button that accepts what was typed, on the text entry sheet. At most 127 bytes */
    ui_set_str(&subject_sheet_ok, TR("Done"));
    s_max_bytes = 0u; /* the initial text is the caller's, already within its limit */
    lv_textarea_set_max_length(ta, max_len); /* characters: a bound on top of the bytes */
    lv_textarea_set_password_mode(ta, kind == UI_TEXT_PASSWORD);
    /* A password's field ends in an eye, and opens hidden every time: the choice isn't kept, as
     * the panel hangs where others walk past (BL-131). */
    ui_set_int(&subject_sheet_eye, kind == UI_TEXT_PASSWORD ? 1 : 0);
    ui_set_int(&subject_sheet_shown, 0);
    lv_textarea_set_text(ta, initial != NULL ? initial : "");
    s_max_bytes = max_len; /* from here on, what is typed is counted in bytes (I18N-010) */
    lv_obj_add_state(ta, LV_STATE_FOCUSED); /* show the cursor */
    lv_keyboard_mode_t mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    if (kind == UI_TEXT_NUMBER) {
        mode = LV_KEYBOARD_MODE_SPECIAL;
    } else if (kind == UI_TEXT_WORDS && (initial == NULL || initial[0] == '\0')) {
        mode = LV_KEYBOARD_MODE_TEXT_UPPER; /* a name starts with a capital */
    }
    kb_letters(kb);
    lv_keyboard_set_mode(kb, mode);
    s_text_fn = done;
    s_cancel_fn = NULL; /* ui_text_sheet_on_cancel sets one */
    s_sheet_kind = SHEET_TEXT;
    ui_set_int(&subject_sheet, SHEET_TEXT);
}

void ui_text_sheet_on_cancel(ui_confirm_fn fn)
{
    if (s_sheet_kind == SHEET_TEXT) {
        s_cancel_fn = fn;
    }
}

void ui_text_sheet_ok(const char *label)
{
    ui_set_str(&subject_sheet_ok, label);
}

void ui_text_sheet(const char *title, const char *note, const char *initial, bool password,
                   uint32_t max_len, ui_text_fn done)
{
    ui_text_sheet_kind(title, note, initial, password ? UI_TEXT_PASSWORD : UI_TEXT_WORDS, max_len,
                       done);
}

void ui_sheet_eye(void)
{
    lv_obj_t *ta = s_sheet_kind == SHEET_TEXT ? on_screen("sheet_text") : NULL;
    if (ta == NULL || lv_subject_get_int(&subject_sheet_eye) == 0) {
        return;
    }
    const bool show = lv_subject_get_int(&subject_sheet_shown) == 0;
    lv_textarea_set_password_mode(ta, !show); /* the cursor stays where it was */
    ui_set_int(&subject_sheet_shown, show ? 1 : 0);
}

static void on_pick(lv_event_t *e)
{
    const int32_t index = (int32_t)(intptr_t)lv_event_get_user_data(e);
    const ui_pick_fn fn = s_pick_fn;
    const ui_confirm_fn owner = sheet_owner_hold();
    ui_sheet_close();
    if (fn != NULL) {
        fn(index);
    }
    sheet_owner_release(owner);
}

/* The sheet is as tall as its choices up to the card's own maximum height (the layout's
 * pick_max_h), then the list scrolls: the list takes what the card has left under its heading. */
static void pick_fit(lv_obj_t *list)
{
    lv_obj_t *card = lv_obj_get_parent(list);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_update_layout(card);
    const int32_t wanted = lv_obj_get_height(list);
    /* the card's border is inside its maximum height too (LVGL's content box is within it) */
    const int32_t used = lv_obj_get_style_pad_top(card, 0) + lv_obj_get_style_pad_bottom(card, 0) +
                         2 * lv_obj_get_style_border_width(card, 0) +
                         lv_obj_get_height(lv_obj_get_child(card, 0)) +
                         lv_obj_get_style_pad_row(card, 0);
    const int32_t room = lv_obj_get_style_max_height(card, 0) - used;
    if (wanted > room) {
        lv_obj_set_height(list, room);
        lv_obj_update_layout(card);
    }
}

const char UI_CHOICE_HEADING[] = "";

void ui_pick_sheet(const char *title, const char *note, const ui_choice_t *choices, size_t n,
                   int32_t selected, ui_pick_fn fn)
{
    lv_obj_t *list = on_screen("pick_list");
    if (list == NULL) {
        return;
    }
    sheet_owner_gone(); /* it replaces the sheet on screen */
    lv_obj_clean(list);
    lv_obj_t *current = NULL;
    for (size_t i = 0; i < n; i++) {
        const ui_choice_t *c = &choices[i];
        if (c->glyph == UI_CHOICE_HEADING) {
            section_title_create(list, c->title != NULL ? c->title : ""); /* one child per entry */
            continue;
        }
        lv_obj_t *row = list_row_create(list, c->glyph != NULL ? c->glyph : "", c->tone, c->title,
                                        c->detail != NULL ? c->detail : "", "",
                                        (int32_t)i == selected ? GLYPH_CHECK : "");
        show_if(ui_find(row, "chip"), c->glyph != NULL && c->glyph[0] != '\0');
        show_if(ui_find(row, "detail_label"), c->detail != NULL && c->detail[0] != '\0');
        show_if(ui_find(row, "value_label"), false);
        show_if(ui_find(row, "level"), false);
        lv_obj_t *trail = ui_find(row, "trail_label");
        lv_obj_set_style_text_color(trail, ui_tone_blue(), 0);
        show_if(trail, (int32_t)i == selected);
        lv_obj_add_event_cb(row, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if ((int32_t)i == selected) {
            current = row;
        }
    }
    ui_set_str(&subject_sheet_title, title);
    ui_set_str(&subject_sheet_note, note != NULL ? note : "");
    s_choices = choices;
    s_pick_fn = fn;
    s_cancel_fn = NULL; /* ui_pick_sheet_on_cancel sets one */
    s_sheet_kind = SHEET_PICK;
    ui_set_int(&subject_sheet, SHEET_PICK);
    pick_fit(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    if (current != NULL) {
        lv_obj_scroll_to_view(current, LV_ANIM_OFF);
    }
}

void ui_pick_sheet_dim(int32_t index)
{
    lv_obj_t *list = s_sheet_kind == SHEET_PICK ? on_screen("pick_list") : NULL;
    if (list != NULL && index >= 0) {
        ui_row_disable(lv_obj_get_child(list, index));
    }
}

void ui_pick_sheet_on_cancel(ui_confirm_fn fn)
{
    if (s_sheet_kind == SHEET_PICK) {
        s_cancel_fn = fn;
    }
}

_Static_assert(UI_SHEET_NOTE_LENGTH >= UI_SUBJECT_STRING_LENGTH,
               "the sheets' note holds at least what any string subject does");

void ui_sheet_note_init(void)
{
    /* Re-made before the screens bind it: its observers come with them. */
    static char note[UI_SHEET_NOTE_LENGTH] UI_SLOW_BSS;
    static char prev[UI_SHEET_NOTE_LENGTH] UI_SLOW_BSS;
    lv_subject_init_string(&subject_sheet_note, note, prev, sizeof(note), "");
}

void ui_confirm(const char *title, const char *text, const char *ok, ui_confirm_fn fn)
{
    /* TR: the button that closes a question without doing anything. At most 127 bytes */
    ui_confirm_choice(title, text, ok, fn, TR("Cancel"), NULL);
}

void ui_confirm_primary(const char *title, const char *text, const char *ok, ui_confirm_fn fn)
{
    ui_confirm(title, text, ok, fn);
    /* The blue OK, where the layout has one; the red one otherwise */
    ui_set_int(&subject_sheet_tone, ui_find(S.main, "dialog_ok_primary") != NULL ? 1 : 0);
}

void ui_confirm_choice(const char *title, const char *text, const char *ok, ui_confirm_fn fn,
                       const char *cancel, ui_confirm_fn cancel_fn)
{
    sheet_owner_gone(); /* it replaces the sheet on screen (the remote banner's question) */
    if (s_sheet_kind == SHEET_TEXT) {
        ui_sheet_close(); /* a half-typed password doesn't wait behind the question */
    }
    ui_set_str(&subject_sheet_title, title);
    ui_set_str(&subject_sheet_note, text);
    ui_set_str(&subject_sheet_ok, ok);
    ui_set_str(&subject_sheet_cancel, cancel);
    s_confirm_fn = fn;
    s_cancel_fn = cancel_fn;
    ui_set_int(&subject_sheet_tone, 0); /* the red OK, unless ui_confirm_primary says */
    s_sheet_kind = SHEET_CONFIRM;
    ui_set_int(&subject_sheet, SHEET_CONFIRM);
}

/* ---- typing from the remote keypad -------------------------------------------------------- */

bool ui_forms_remote_text(const char *utf8)
{
    lv_obj_t *ta = s_sheet_kind == SHEET_TEXT ? on_screen("sheet_text") : NULL;
    lv_obj_t *kb = on_screen("sheet_kb");
    if (ta == NULL || utf8 == NULL || utf8[0] == '\0') {
        return false;
    }
    lv_textarea_add_text(ta, utf8); /* the sheet's length limit and accepted characters apply */
    if (kb != NULL && lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER) {
        kb_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER); /* one capital, as with the on-screen keys */
    }
    return true;
}

bool ui_forms_remote_key(ui_remote_key_t key)
{
    lv_obj_t *ta = s_sheet_kind == SHEET_TEXT ? on_screen("sheet_text") : NULL;
    if (ta == NULL) {
        return false;
    }
    switch (key) {
    case UI_REMOTE_KEY_BACKSPACE: lv_textarea_delete_char(ta); break;
    case UI_REMOTE_KEY_ENTER: ui_sheet_done(); break;
    case UI_REMOTE_KEY_ESCAPE: ui_sheet_cancel(); break;
    default: return false;
    }
    return true;
}

void ui_sheet_done(void)
{
    if (s_sheet_kind == SHEET_TEXT) {
        lv_obj_t *ta = on_screen("sheet_text");
        static char text[192]; /* the longest field: an update source address */
        snprintf(text, sizeof(text), "%s", ta != NULL ? lv_textarea_get_text(ta) : "");
        const ui_text_fn fn = s_text_fn;
        const ui_confirm_fn owner = sheet_owner_hold();
        ui_sheet_close();
        if (fn != NULL) {
            fn(text);
        }
        memset(text, 0, sizeof(text));
        sheet_owner_release(owner);
    } else if (s_sheet_kind == SHEET_CONFIRM) {
        const ui_confirm_fn fn = s_confirm_fn;
        const ui_confirm_fn owner = sheet_owner_hold();
        ui_sheet_close();
        if (fn != NULL) {
            fn();
        }
        sheet_owner_release(owner);
    } else if (s_sheet_kind >= SHEET_CLOCK) {
        const int32_t kind = s_sheet_kind;
        if (s_ok_fn == NULL || s_ok_fn()) {
            if (s_sheet_kind == kind) { /* ok may have closed it, or opened another */
                ui_sheet_close();
            }
        }
    }
}

/* ---- rollers ------------------------------------------------------------------------------ */

void ui_event_once(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t code, void *ctx)
{
    if (obj == NULL) {
        return;
    }
    while (lv_obj_remove_event_cb(obj, cb) > 0u) {
    }
    lv_obj_add_event_cb(obj, cb, code, ctx);
}

void ui_roller_fill(lv_obj_t *roller, const char *options, int32_t selected, bool wrap)
{
    if (roller == NULL) {
        return;
    }
    /* Rows as tall as the layout says, whatever the font (a language pack brings its own). */
    const lv_font_t *font = lv_obj_get_style_text_font(roller, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, ROLLER_ROW_H - lv_font_get_line_height(font), 0);
    lv_roller_set_options(roller, options, wrap ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
    /* Set, never animated to (UI-020): the style's anim_duration is 0 too, so a flick snaps. */
    lv_roller_set_selected(roller, selected > 0 ? (uint32_t)selected : 0u, LV_ANIM_OFF);
}

/* "a\nb\n..." of count numbers from first, stepping by step, two digits or as they come. */
static void number_options(char *out, size_t len, int32_t first, int32_t count, int32_t step, bool two_digits)
{
    size_t used = 0;
    out[0] = '\0';
    for (int32_t i = 0; i < count && used + 4u < len; i++) {
        const int32_t v = first + i * step;
        const char *const sep = i > 0 ? "\n" : "";
        const int n = two_digits ? snprintf(out + used, len - used, "%s%02d", sep, (int)v)
                                 : snprintf(out + used, len - used, "%s%d", sep, (int)v);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
}

void ui_time_rollers_setup(lv_obj_t *rollers, int32_t minute_of_day, int32_t step, lv_event_cb_t changed,
                           void *ctx)
{
    if (rollers == NULL) {
        return;
    }
    if (step < 1) {
        step = 1;
    }
    const int32_t m = (minute_of_day % 1440 + 1440) % 1440;
    const bool h24 = ui_clock_24h();
    char options[200];
    if (h24) {
        number_options(options, sizeof(options), 0, 24, 1, false); /* 0-23 */
        ui_roller_fill(ui_find(rollers, "roll_hour"), options, m / 60, true);
    } else {
        /* 12, 1, 2 ... 11: the row is the hour modulo 12 */
        snprintf(options, sizeof(options), "12\n");
        number_options(options + 3, sizeof(options) - 3u, 1, 11, 1, false);
        ui_roller_fill(ui_find(rollers, "roll_hour"), options, (m / 60) % 12, true);
        /* The day period's names, as the clock writes them ("AM", "PM") */
        i18n_civil_t c;
        memset(&c, 0, sizeof(c));
        c.year = 2026;
        c.month = 1;
        c.day = 1;
        char am[24];
        char pm[24];
        i18n_date(am, sizeof(am), S.catalog, NOTR("a"), &c);
        c.hour = 12;
        i18n_date(pm, sizeof(pm), S.catalog, NOTR("a"), &c);
        snprintf(options, sizeof(options), "%s\n%s", am, pm);
        ui_roller_fill(ui_find(rollers, "roll_ampm"), options, m >= 720 ? 1 : 0, false);
    }
    show_if(ui_find(rollers, "ampm_col"), !h24);
    number_options(options, sizeof(options), 0, (60 + step - 1) / step, step, true);
    ui_roller_fill(ui_find(rollers, "roll_minute"), options, (m % 60) / step, true);
    static const char *const NAMES[] = {"roll_hour", "roll_minute", "roll_ampm"};
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        lv_obj_t *r = ui_find(rollers, NAMES[i]);
        if (r == NULL) {
            continue;
        }
        lv_obj_set_user_data(r, (void *)(intptr_t)step);
        if (changed != NULL) {
            ui_event_once(r, changed, LV_EVENT_VALUE_CHANGED, ctx);
        }
    }
}

int32_t ui_time_rollers_get(lv_obj_t *rollers)
{
    lv_obj_t *hour = ui_find(rollers, "roll_hour");
    lv_obj_t *minute = ui_find(rollers, "roll_minute");
    lv_obj_t *ampm = ui_find(rollers, "roll_ampm");
    if (hour == NULL || minute == NULL) {
        return 0;
    }
    const int32_t step = (int32_t)(intptr_t)lv_obj_get_user_data(minute);
    int32_t h = (int32_t)lv_roller_get_selected(hour);
    if (!ui_clock_24h()) {
        h = h % 12 + (ampm != NULL && lv_roller_get_selected(ampm) == 1u ? 12 : 0);
    }
    return h * 60 + (int32_t)lv_roller_get_selected(minute) * (step > 0 ? step : 1);
}

/* ---- the time sheet and the window sheet (BL-168) ----------------------------------------- */

static const char *const DAY_TOGGLES[7] = {"day_1", "day_2", "day_3", "day_4", "day_5", "day_6", "day_7"};

/* A sheet's Done, pressable or dimmed */
static void ok_enabled_set(lv_obj_t *ok, bool enabled)
{
    if (ok != NULL) {
        lv_obj_set_state(ok, LV_STATE_DISABLED, !enabled);
    }
}

static void time_sheet_changed(void)
{
    if (s_sheet_kind == SHEET_TIME && s_time_fn != NULL) {
        s_time_fn(ui_time_sheet_minute(), ui_time_sheet_days());
    }
}

static void on_time_sheet_input(lv_event_t *e)
{
    (void)e;
    time_sheet_changed();
}

void ui_time_sheet_open(const char *title, const char *note, int32_t minute, uint8_t days_mon, int32_t step,
                        ui_time_sheet_fn changed, ui_sheet_ok_fn ok)
{
    lv_obj_t *sheet = on_screen("time_sheet");
    if (sheet == NULL) {
        return;
    }
    ui_time_rollers_setup(ui_find(sheet, "time_at"), minute, step, on_time_sheet_input, NULL);
    /* The week as the region starts it (I18N-016), each day by its short name */
    const int32_t first = (int32_t)ui_region()->first_day;
    i18n_civil_t c;
    memset(&c, 0, sizeof(c));
    c.year = 2026;
    c.month = 1;
    c.day = 1;
    for (int32_t col = 0; col < 7; col++) {
        const int32_t weekday = (first + col) % 7; /* 0 Sunday */
        s_day_bit[col] = (uint8_t)((weekday + 6) % 7);
        lv_obj_t *toggle = ui_find(sheet, DAY_TOGGLES[col]);
        if (toggle == NULL) {
            continue;
        }
        c.weekday = (uint8_t)weekday;
        char name[24];
        i18n_date(name, sizeof(name), S.catalog, NOTR("EEE"), &c);
        ui_set_label(toggle, "day_name", name);
        lv_obj_set_state(toggle, LV_STATE_CHECKED, ((unsigned)days_mon >> s_day_bit[col]) & 1u);
        ui_event_once(toggle, on_time_sheet_input, LV_EVENT_VALUE_CHANGED, NULL);
    }
    ui_set_str(&subject_sheet_title, title);
    ui_set_str(&subject_sheet_note, note != NULL ? note : "");
    ui_time_sheet_result("", false, true);
    ui_sheet_open(SHEET_TIME, ok, NULL);
    s_time_fn = changed; /* after ui_sheet_open, which forgets the last sheet's */
}

void ui_time_sheet_result(const char *text, bool amber, bool ok_enabled)
{
    lv_obj_t *sheet = on_screen("time_sheet");
    lv_obj_t *result = ui_find(sheet, "time_result");
    if (result == NULL) {
        return;
    }
    ui_set_label(sheet, "time_result", text != NULL ? text : "");
    if (amber) {
        lv_obj_set_style_text_color(result, ui_tone_amber(), 0);
    } else {
        lv_obj_remove_local_style_prop(result, LV_STYLE_TEXT_COLOR, 0); /* the card's own */
    }
    ok_enabled_set(ui_find(sheet, "time_ok"), ok_enabled);
}

int32_t ui_time_sheet_minute(void)
{
    return ui_time_rollers_get(ui_find(on_screen("time_sheet"), "time_at"));
}

uint8_t ui_time_sheet_days(void)
{
    lv_obj_t *sheet = on_screen("time_sheet");
    uint8_t days = 0u;
    for (int32_t col = 0; col < 7 && sheet != NULL; col++) {
        lv_obj_t *toggle = ui_find(sheet, DAY_TOGGLES[col]);
        if (toggle != NULL && lv_obj_has_state(toggle, LV_STATE_CHECKED)) {
            days = (uint8_t)(days | (1u << s_day_bit[col]));
        }
    }
    return days;
}

static void on_window_rollers(lv_event_t *e)
{
    (void)e;
    if (s_sheet_kind == SHEET_WINDOW && s_window_fn != NULL) {
        s_window_fn(ui_window_sheet_from(), ui_window_sheet_to());
    }
}

static void on_window_any(lv_event_t *e)
{
    (void)e;
    const ui_confirm_fn fn = s_sheet_kind == SHEET_WINDOW ? s_any_fn : NULL;
    ui_sheet_close();
    if (fn != NULL) {
        fn();
    }
}

void ui_window_sheet_open(const char *title, int32_t from, int32_t to, int32_t step, ui_window_sheet_fn changed,
                          ui_sheet_ok_fn ok, ui_confirm_fn any_time)
{
    lv_obj_t *sheet = on_screen("window_sheet");
    if (sheet == NULL) {
        return;
    }
    ui_time_rollers_setup(ui_find(sheet, "window_from"), from, step, on_window_rollers, NULL);
    ui_time_rollers_setup(ui_find(sheet, "window_to"), to, step, on_window_rollers, NULL);
    lv_obj_t *any = ui_find(sheet, "window_any");
    ui_event_once(any, on_window_any, LV_EVENT_CLICKED, NULL);
    show_if(any, any_time != NULL);
    ui_set_str(&subject_sheet_title, title);
    ui_set_str(&subject_sheet_note, "");
    ui_window_sheet_result("", true);
    ui_sheet_open(SHEET_WINDOW, ok, NULL);
    s_window_fn = changed; /* after ui_sheet_open, which forgets the last sheet's */
    s_any_fn = any_time;
}

void ui_window_sheet_result(const char *text, bool ok_enabled)
{
    lv_obj_t *sheet = on_screen("window_sheet");
    ui_set_label(sheet, "window_result", text != NULL ? text : "");
    ok_enabled_set(ui_find(sheet, "window_ok"), ok_enabled);
}

int32_t ui_window_sheet_from(void)
{
    return ui_time_rollers_get(ui_find(on_screen("window_sheet"), "window_from"));
}

int32_t ui_window_sheet_to(void)
{
    return ui_time_rollers_get(ui_find(on_screen("window_sheet"), "window_to"));
}

/* ---- a sheet of rows (form_sheet.xml) --------------------------------------------------------- */

lv_obj_t *ui_form_sheet_open(const char *title, const char *note, ui_sheet_ok_fn ok)
{
    lv_obj_t *sheet = on_screen("form_sheet");
    lv_obj_t *body = ui_find(sheet, "form_body");
    if (body == NULL) {
        return NULL;
    }
    lv_obj_clean(body);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    ui_sheet_open(SHEET_FORM, ok, NULL);
    ui_set_str(&subject_sheet_title, title);
    ui_set_str(&subject_sheet_note, note != NULL ? note : "");
    show_if(ui_find(sheet, "form_note"), note != NULL && note[0] != '\0');
    return body;
}

void ui_sheet_body_fit(lv_obj_t *body)
{
    if (body != NULL) {
        /* pick_fit lays the body out at its contents' height first, and LVGL scrolls a body that
         * then has nothing left to scroll back to its top: a body the person had scrolled would
         * snap up whenever its words change. Keep where it was, clamped to the new range; all in
         * this call, so no frame is drawn in between (BL-184). */
        const int32_t y = lv_obj_get_scroll_y(body);
        pick_fit(body);
        lv_obj_scroll_to_y(body, y, LV_ANIM_OFF);
    }
}

/* As tall as its rows up to the card's maximum height, then the body scrolls (pick_fit's way). */
void ui_form_sheet_fit(void)
{
    lv_obj_t *body = s_sheet_kind == SHEET_FORM ? ui_find(on_screen("form_sheet"), "form_body") : NULL;
    if (body != NULL) {
        pick_fit(body);
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    }
}
