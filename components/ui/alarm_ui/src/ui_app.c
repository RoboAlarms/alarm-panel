/*
 * ui_app.c - logic of the alarm panel's touch UI: start-up, the periodic tick, status
 * snapshots, Home, the code pad, the zone / activity / trouble lists, the emergency buttons and
 * the screen saver. Settings, installer mode and the first-boot setup live in their own modules
 * (see ui_internal.h).
 *
 * Reads status snapshots through the backend, derives the view model (the subjects of the
 * LVGL Pro project components/ui/alarm_ui/layouts/1024x600/alarm_ui), and turns taps into engine commands. Everything runs on the
 * LVGL thread; the backend's commands block until the engine has answered.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "i18n/i18n_date.h"
#include "ui_backend/ui_light.h"
#include "ui_backend/ui_why_words.h"
#include "ui_internal.h"

LV_IMAGE_DECLARE(ui_img_roboalarms_mark);

#define TICK_MS          50u
#define TOAST_MS         2600u
#define HOME_AFTER_MS    90000u  /* idle time before an open view returns to Home */
#define SETTINGS_IDLE_MS 300000u /* ... and Settings (installer mode ends with it) */
#define PANIC_HOLD_MS    2000u
#define NET_POLL_MS      1000u
#define LIGHT_PREVIEW_MS 3000u /* a brightness slider's level stays in view this long (UI-043) */
#define ZONES_EMPTY_SETTLE_MS 1000u /* an empty Zones filter's check: settled, once a second */

enum { FILTER_ALL = 0, FILTER_OPEN, FILTER_BYPASSED, FILTER_TROUBLE };

ui_state_t S UI_SLOW_BSS;

/* LVGL's heap: PSRAM on the 7" panel (board_lvgl_memory.c), the system heap on a board
 * without PSRAM, where only a running touch UI asks for these tables. */
void *ui_slow_alloc(size_t size)
{
    return lv_malloc_zeroed(size);
}

unsigned ui_app_text_cuts(const char **last)
{
    if (last != NULL) {
        *last = S.last_cut;
    }
    return S.text_cuts;
}


/* Every English text the layout's XML handed to alarm_ui_tr while the screens were built (the
 * generated code's string literals, so the pointers stay good): a new language finds the labels
 * showing one of them by their old translation (I18N-014). */
#define UI_LITERALS_MAX 320u
typedef struct {
    const char *ctx; /* its translation context (a tr-context comment in the XML), or NULL */
    const char *english;
} ui_literal_t;
static ui_literal_t s_literals[UI_LITERALS_MAX] UI_SLOW_BSS; /* this thread's only: PSRAM (B001) */
static size_t s_n_literals;

/* The lookup the generated layout code calls for the text in the XML (tools/lvpro_gen.py routes
 * every literal string of text for people through it; tools/i18n.py lists the same texts). */
static bool same_ctx(const char *a, const char *b)
{
    return a == b || (a != NULL && b != NULL && strcmp(a, b) == 0);
}

const char *alarm_ui_tr_c(const char *ctx, const char *english)
{
    /* A component made again (a list's rows) hands the same literal: found by its address,
     * without comparing text; the same words from another place in the XML by their text. */
    size_t i = 0;
    while (i < s_n_literals && (s_literals[i].english != english || s_literals[i].ctx != ctx)) {
        i++;
    }
    if (i == s_n_literals) {
        i = 0;
        while (i < s_n_literals && (strcmp(s_literals[i].english, english) != 0 ||
                                    !same_ctx(s_literals[i].ctx, ctx))) {
            i++;
        }
        if (i == s_n_literals && s_n_literals < UI_LITERALS_MAX) {
            s_literals[s_n_literals++] = (ui_literal_t){ctx, english};
        }
    }
    return i18n_text(S.catalog, ctx, english);
}

const char *ui_answer(const char *ctx, const char *why)
{
    static char line[128] UI_SLOW_BSS;
    ui_why_line(S.catalog, ctx, why, line, sizeof(line));
    return line;
}

const char *alarm_ui_tr(const char *english)
{
    return alarm_ui_tr_c(NULL, english);
}

/* ---- small helpers ------------------------------------------------------------------ */

void ui_set_int(lv_subject_t *s, int32_t v)
{
    if (lv_subject_get_int(s) != v) {
        lv_subject_set_int(s, v);
    }
}

void ui_set_str(lv_subject_t *s, const char *v)
{
    /* lv_subject_copy_string cuts with lv_strlcpy, which can stop inside a UTF-8 character
     * (I18N-010): cut here, on a whole one, so it never has to. */
    char text[UI_SHEET_NOTE_LENGTH]; /* the longest string subject */
    const size_t size = s == &subject_sheet_note ? sizeof(text) : UI_SUBJECT_STRING_LENGTH;
    if (i18n_utf8_copy(text, size, v) < strlen(v != NULL ? v : "")) {
        /* A word lost: a translation longer than its budget (I18N-023) */
        S.text_cuts++;
        i18n_utf8_copy(S.last_cut, sizeof(S.last_cut), v);
    }
    if (strcmp(lv_subject_get_string(s), text) != 0) {
        lv_subject_copy_string(s, text);
    }
}

void ui_set_color(lv_subject_t *s, lv_color_t c)
{
    if (!lv_color_eq(lv_subject_get_color(s), c)) {
        lv_subject_set_color(s, c);
    }
}

lv_obj_t *ui_find(lv_obj_t *root, const char *name)
{
    return root != NULL ? lv_obj_find_by_name(root, name) : NULL;
}

void ui_set_label(lv_obj_t *parent, const char *name, const char *text)
{
    lv_obj_t *l = ui_find(parent, name);
    if (l != NULL && strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

void ui_chip_tone(lv_obj_t *chip, const char *glyph, lv_color_t tone)
{
    if (chip == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(chip, tone, 0);
    lv_obj_t *icon = ui_find(chip, "icon");
    if (icon != NULL) {
        lv_obj_set_style_text_color(icon, tone, 0);
        if (glyph != NULL && strcmp(lv_label_get_text(icon), glyph) != 0) {
            lv_label_set_text(icon, glyph);
        }
    }
}

void ui_beep(ui_beep_t b)
{
    if (S.be.beep != NULL) {
        S.be.beep(S.be.ctx, b);
    }
}

static void backlight(uint8_t percent)
{
    if (S.be.backlight != NULL) {
        S.be.backlight(S.be.ctx, percent);
    }
}

static const alarm_partition_status_t *part(void)
{
    return &S.snap.part[S.opt.partition - 1u];
}

static bool zone_in_partition(uint16_t z)
{
    const alarm_zone_config_t *zc = &S.cfg->zones[z - 1u];
    return zc->type != ALARM_ZT_NOT_USED && zc->partition == S.opt.partition;
}

/* false when the C library can't convert the time (ui_when_text asks; the others don't). */
static bool local_tm(int64_t epoch_ms, struct tm *out)
{
    const time_t t = (time_t)(epoch_ms / 1000);
#if defined(_WIN32)
    return localtime_s(out, &t) == 0;
#else
    return localtime_r(&t, out) != NULL;
#endif
}

static void civil_from_tm(i18n_civil_t *c, const struct tm *tm)
{
    c->year = (int16_t)(tm->tm_year + 1900);
    c->month = (uint8_t)(tm->tm_mon + 1);
    c->day = (uint8_t)tm->tm_mday;
    c->hour = (uint8_t)tm->tm_hour;
    c->minute = (uint8_t)tm->tm_min;
    c->second = (uint8_t)tm->tm_sec;
    c->weekday = (uint8_t)tm->tm_wday;
}

const i18n_region_t *ui_region(void)
{
    return i18n_region_for(S.opt.region, S.opt.lang);
}

bool ui_clock_24h(void)
{
    return i18n_clock_24h(S.opt.clock_mode, ui_region());
}

void ui_size_text(char *out, size_t len, uint64_t bytes)
{
    /* As storage_size() counts (powers of 1024, one decimal), in the region's numbers, and as a card's
     * label says it: GB, not GiB (BL-131). */
    uint64_t divisor = 1u;
    unsigned unit = 0;
    while (unit < 4u && bytes / divisor >= 1024u) {
        divisor *= 1024u;
        unit++;
    }
    const uint64_t tenths = (bytes / divisor) * 10u + (bytes % divisor) * 10u / divisor;
    char num[32];
    i18n_number(num, sizeof(num), ui_region(), (int64_t)(unit == 0u ? bytes : tenths),
                unit == 0u ? 0u : 1u);
    /* TR: an amount of data; {0} is the number. The units count in 1024s, as a memory card's label does. At most 31 bytes */
    const char *const units[] = {TR_C("data size", "{0} B"), TR_C("data size", "{0} KB"),
                                 TR_C("data size", "{0} MB"), TR_C("data size", "{0} GB"),
                                 TR_C("data size", "{0} TB")};
    I18N_FORMAT(out, len, units[unit], num);
}

static void clock_tm(const struct tm *tm, char *buf, size_t len, bool with_ampm)
{
    i18n_civil_t c;
    civil_from_tm(&c, tm);
    const char *pattern;
    if (ui_clock_24h()) {
        /* TR: a clock time on a 24-hour clock, in CLDR pattern letters (HH hour 00-23, mm minute);
         * shows on Home, in lists and on the screen saver. The order and the separator may
         * change. At most 15 bytes of result */
        pattern = TR_C("date pattern", "HH:mm");
    } else if (with_ampm) {
        /* TR: a clock time on a 12-hour clock with its AM/PM marker, in CLDR pattern letters (h
         * hour 1-12, mm minute, a AM/PM); shows in lists and rows. The order may change. At most
         * 15 bytes of result */
        pattern = TR_C("date pattern", "h:mm a");
    } else {
        /* TR: a clock time on a 12-hour clock without the marker, in CLDR pattern letters (h
         * hour 1-12, mm minute); Home's big clock, its AM/PM label is separate. At most 15
         * bytes of result */
        pattern = TR_C("date pattern", "h:mm");
    }
    i18n_date(buf, len, S.catalog, pattern, &c);
}

void ui_clock_text(int64_t epoch_ms, char *buf, size_t len, bool with_ampm)
{
    struct tm tm;
    local_tm(epoch_ms, &tm);
    clock_tm(&tm, buf, len, with_ampm);
}

void ui_minute_text(int minute, char *buf, size_t len)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    const int m = (minute % 1440 + 1440) % 1440;
    tm.tm_year = 126; /* any day: only the hour and minute show */
    tm.tm_mday = 1;
    tm.tm_hour = m / 60;
    tm.tm_min = m % 60;
    clock_tm(&tm, buf, len, true);
}

bool ui_clock_known(void)
{
    return S.be.epoch_ms(S.be.ctx) > 0;
}

void ui_date_time_text(int64_t epoch_ms, char *buf, size_t len)
{
    struct tm tm;
    local_tm(epoch_ms, &tm);
    i18n_civil_t c;
    civil_from_tm(&c, &tm);
    /* TR: a short date and time on a 12-hour clock, in CLDR pattern letters (MMM month name
     * short, d day, h hour 1-12, mm minute, a AM/PM); shows in the Activity list and rows. The
     * order may change. At most 31 bytes of result */
    const char *const pattern12 = TR_C("date pattern", "MMM d, h:mm a");
    /* TR: a short date and time on a 24-hour clock, in CLDR pattern letters (MMM month name
     * short, d day, HH hour 00-23, mm minute); shows in the Activity list and rows. The order
     * may change. At most 31 bytes of result */
    const char *const pattern24 = TR_C("date pattern", "MMM d, HH:mm");
    i18n_date_in(buf, len, S.catalog, ui_region(), ui_clock_24h() ? pattern24 : pattern12, &c);
}

void ui_time_or_day_text(int64_t epoch_ms, char *buf, size_t len)
{
    struct tm then;
    struct tm now;
    local_tm(epoch_ms, &then);
    local_tm(S.be.epoch_ms(S.be.ctx), &now);
    if (then.tm_year == now.tm_year && then.tm_yday == now.tm_yday) {
        clock_tm(&then, buf, len, true);
        return;
    }
    i18n_civil_t c;
    civil_from_tm(&c, &then);
    /* TR: a short date without the time ("Sep 19"), in CLDR pattern letters (MMM month name
     * short, d day); Software update says when it last checked. The order may change. At most 31
     * bytes of result */
    i18n_date_in(buf, len, S.catalog, ui_region(), TR_C("date pattern", "MMM d"), &c);
}

ui_when_t ui_when_text(int64_t epoch_ms, char *buf, size_t len)
{
    if (len > 0u) {
        buf[0] = '\0';
    }
    const int64_t now_ms = S.be.epoch_ms(S.be.ctx);
    /* no clock, no time, or more than a minute ahead of the clock: no date (BL-179) */
    if (now_ms <= 0 || epoch_ms <= 0 || epoch_ms - now_ms > 60000) {
        return UI_WHEN_UNKNOWN;
    }
    if (epoch_ms > now_ms) { /* up to a minute ahead: two clocks a little apart, now */
        epoch_ms = now_ms;
    }
    struct tm then;
    struct tm now;
    if (!local_tm(epoch_ms, &then) || !local_tm(now_ms, &now)) {
        return UI_WHEN_UNKNOWN;
    }
    if (then.tm_year == now.tm_year && then.tm_yday == now.tm_yday) {
        clock_tm(&then, buf, len, true);
        return UI_WHEN_TODAY;
    }
    i18n_civil_t c;
    civil_from_tm(&c, &then);
    if (then.tm_year == now.tm_year) {
        /* TR: a short date without the time ("Sep 18"), in CLDR pattern letters (MMM month name
         * short, d day); a device's tile says when it last ran or went offline, earlier this year.
         * The order may change. At most 31 bytes of result */
        i18n_date_in(buf, len, S.catalog, ui_region(), TR_C("date pattern", "MMM d"), &c);
        return UI_WHEN_DAY;
    }
    /* TR: a short date with its year ("Sep 18, 2025"), in CLDR pattern letters (MMM month name
     * short, d day, y year); a device's tile says when it last ran or went offline, an earlier
     * year's. The order may change. At most 31 bytes of result */
    i18n_date_in(buf, len, S.catalog, ui_region(), TR_C("date pattern", "MMM d, y"), &c);
    return UI_WHEN_YEAR;
}

void ui_date_time_long(int64_t epoch_ms, char *buf, size_t len)
{
    struct tm tm;
    local_tm(epoch_ms, &tm);
    i18n_civil_t c;
    civil_from_tm(&c, &tm);
    /* TR: the date and time in full on a 12-hour clock, in CLDR pattern letters (EEEE weekday,
     * MMMM month, d day, h hour 1-12, mm minute, a AM/PM); the title of the clock's row in Time
     * and language. The order may change. At most 63 bytes of result */
    const char *const pattern12 = TR_C("date pattern", "EEEE, MMMM d · h:mm a");
    /* TR: the date and time in full on a 24-hour clock, in CLDR pattern letters (EEEE weekday,
     * MMMM month, d day, HH hour 00-23, mm minute); the title of the clock's row in Time and
     * language. The order may change. At most 63 bytes of result */
    const char *const pattern24 = TR_C("date pattern", "EEEE, MMMM d · HH:mm");
    i18n_date_in(buf, len, S.catalog, ui_region(), ui_clock_24h() ? pattern24 : pattern12, &c);
}

/* Its caller's buffer: the Activity list's time column, 48 bytes. */
void ui_entry_time(const elog_entry_t *e, char *buf, size_t len)
{
    const uint32_t boot = S.be.boot_id != NULL ? S.be.boot_id(S.be.ctx) : e->boot;
    if (e->ev.epoch_ms <= 0) {
        /* TR: when an event happened, in the Activity list. At most 31 bytes */
        i18n_utf8_copy(buf, len, (e->boot == boot) ? TR("Time unknown") : TR("Before a restart"));
        return;
    }
    const int64_t now_ms = S.be.epoch_ms(S.be.ctx);
    if (now_ms > 0) {
        struct tm then;
        struct tm now;
        local_tm(e->ev.epoch_ms, &then);
        local_tm(now_ms, &now);
        const bool today = then.tm_year == now.tm_year && then.tm_yday == now.tm_yday;
        /* the day before today, a new year's day's included */
        const bool yesterday =
            (then.tm_year == now.tm_year && then.tm_yday + 1 == now.tm_yday) ||
            (then.tm_year + 1 == now.tm_year && now.tm_yday == 0 && then.tm_mon == 11 &&
             then.tm_mday == 31);
        if (today) {
            clock_tm(&then, buf, len, true);
            return;
        }
        if (yesterday) {
            char t[24];
            clock_tm(&then, t, sizeof(t), true);
            /* TR: when an event happened, in the Activity list: yesterday, {0} is its time
             * ("9:01 PM"). At most 47 bytes */
            I18N_FORMAT(buf, len, TR("Yesterday, {0}"), t);
            return;
        }
    }
    ui_date_time_text(e->ev.epoch_ms, buf, len);
}

/* "Front Door", "Front Door and Back Door", "Front Door, Back Door +2" */
static void zone_list_text(uint16_t flag, char *buf, size_t len)
{
    unsigned n = 0;
    buf[0] = '\0';
    char name[ALARM_NAME_LEN + 8];
    uint16_t first = 0;
    uint16_t second = 0;
    for (uint16_t z = 1; z <= S.snap.zones_used; z++) {
        if (zone_in_partition(z) && (S.snap.zone[z - 1u].flags & flag)) {
            if (n == 0u) {
                first = z;
            } else if (n == 1u) {
                second = z;
            }
            n++;
        }
    }
    if (n == 0u) {
        return;
    }
    char a[ALARM_NAME_LEN + 8];
    ui_zone_name(S.cfg, first, a, sizeof(a));
    if (n == 1u) {
        i18n_utf8_copy(buf, len, a);
    } else if (n == 2u) {
        /* TR: two zone names, on Home under the state headline */
        I18N_FORMAT(buf, len, TR("{0} and {1}"), a,
                    ui_zone_name(S.cfg, second, name, sizeof(name)));
    } else {
        /* TR: two zone names and how many more ({2}), on Home under the state headline */
        I18N_FORMAT(buf, len, TRN("{0}, {1} +{2}", "{0}, {1} +{2}", n - 2u), a,
                    ui_zone_name(S.cfg, second, name, sizeof(name)), n - 2u);
    }
}

static unsigned count_zones(uint16_t flag)
{
    unsigned n = 0;
    for (uint16_t z = 1; z <= S.snap.zones_used; z++) {
        if (zone_in_partition(z) && (S.snap.zone[z - 1u].flags & flag)) {
            n++;
        }
    }
    return n;
}

/* ---- toast ---------------------------------------------------------------------------- */

/* The text sheet's card sits at the bottom of the screen, so a toast moves to the top, clear
 * of the keyboard. Everything else keeps the toast where it belongs, at the bottom. Where
 * exactly is the layout's own (its toast_x, toast_y and toast_kb_y constants). */
static void toast_align(void)
{
    lv_obj_t *t = ui_find(S.main, "toast");
    if (t == NULL) {
        return;
    }
    const bool keyboard = lv_subject_get_int(&subject_sheet) == SHEET_TEXT;
    lv_obj_align(t, keyboard ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, TOAST_X,
                 keyboard ? TOAST_KB_Y : TOAST_Y);
}

/* A panel over the screen fills the middle: words about what happened before it opened would
 * land on its title or its keys. They have been read by now. */
static void toast_hide(void)
{
    S.toast_on = false;
    ui_set_int(&subject_toast_visible, 0);
}

void ui_toast(const char *text)
{
    toast_align();
    ui_set_str(&subject_toast, text);
    ui_set_int(&subject_toast_visible, 1);
    S.toast_on = true;
    S.toast_since = lv_tick_get();
}

/* ---- animations ----------------------------------------------------------------------- */

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)(v / 3), 0);
}

static void set_pulse(bool on)
{
    lv_obj_t *disk = ui_find(S.main, "disk");
    if (disk == NULL || on == S.pulse_on) {
        return;
    }
    S.pulse_on = on;
    lv_anim_delete(disk, pulse_exec);
    if (on) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, disk);
        lv_anim_set_exec_cb(&a, pulse_exec);
        lv_anim_set_values(&a, 60, 200);
        lv_anim_set_duration(&a, 650);
        lv_anim_set_reverse_duration(&a, 650);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    } else {
        lv_obj_remove_local_style_prop(disk, LV_STYLE_SHADOW_OPA, 0);
        lv_obj_remove_local_style_prop(disk, LV_STYLE_BG_OPA, 0);
    }
}

static void shake_exec(void *obj, int32_t v)
{
    /* damped wobble over 360 ms */
    static const int8_t wave[] = {0, -14, 12, -10, 8, -6, 4, -2, 0};
    const int32_t i = v / 40;
    lv_obj_set_style_translate_x((lv_obj_t *)obj, wave[i < 9 ? i : 8], 0);
}

/* A wrong code shakes the line that says so (the dots it replaced are gone by then). */
static void shake_dots(void)
{
    lv_obj_t *dots = ui_find(S.pads[S.pad_host], "pad_hint");
    if (dots == NULL) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dots);
    lv_anim_set_exec_cb(&a, shake_exec);
    lv_anim_set_values(&a, 0, 359);
    lv_anim_set_duration(&a, 360);
    lv_anim_start(&a);
}

/* ---- screens: main, saver, setup ------------------------------------------------------ */

void ui_show_main(void)
{
    if (lv_screen_active() != S.main) {
        lv_screen_load(S.main);
    }
}

static void pad_forget_typing(void);
static void bar_banner(bool locked);

/* ---- the hero's colour ------------------------------------------------------------------ */

/* The state card's colour changes at most twice a second (UI-020): a sensor flapping on a
 * loose wire would otherwise flip Ready and Not ready, green and amber, as fast as it bounces,
 * and the wall would strobe. An alarm or an entry delay is never held back; a change that was
 * held is applied once the dwell has passed. */
#define HERO_TONE_DWELL_MS 500u
static lv_color_t s_hero_tone;
static bool s_hero_tone_valid;
static uint32_t s_hero_tone_at;
static lv_color_t s_hero_tone_wanted;
static bool s_hero_tone_pending;

static void hero_tone(lv_color_t tone, bool urgent)
{
    if (s_hero_tone_valid && lv_color_eq(tone, s_hero_tone)) {
        s_hero_tone_pending = false;
        return;
    }
    if (s_hero_tone_valid && !urgent && lv_tick_elaps(s_hero_tone_at) < HERO_TONE_DWELL_MS) {
        s_hero_tone_wanted = tone;
        s_hero_tone_pending = true;
        return;
    }
    ui_set_color(&subject_tone_color, tone);
    s_hero_tone = tone;
    s_hero_tone_valid = true;
    s_hero_tone_at = lv_tick_get();
    s_hero_tone_pending = false;
}

static void hero_tone_tick(void)
{
    if (s_hero_tone_pending && lv_tick_elaps(s_hero_tone_at) >= HERO_TONE_DWELL_MS) {
        hero_tone(s_hero_tone_wanted, true);
    }
}

static uint8_t s_priv_user; /* whose code shows Activity's private events (LOG-008), 0 = nobody */
static void activity_forget(void);

#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
/* DEV-010: remote control never borrows a local authenticated session, and never leaves one
 * of its own behind. Every screen may be viewed remotely except Activity's private events
 * (LOG-008): not while a code shows them, and a remote session's are forgotten when it ends. */
bool ui_dev_control_begin(void)
{
    if (S.program || S.auth_user || S.wizard_on || S.digits[0] || s_priv_user != 0u) return false;
    return true;
}
void ui_dev_control_end(void)
{
    activity_forget();
    if (S.auth_user || S.program || S.digits[0]) {
        ui_pad_cancel();
        /* A pad on Home stays open (an entry delay may be counting), but the remote's
         * half-typed code must not wait there for the next person to press the tick. */
        pad_forget_typing();
        ui_set_int(&subject_view, VIEW_HOME);
    }
}
#endif

static bool alarm_mode(ui_mode_t m);

/* The backlight this screen should have now (UI-009, UI-043; ui_light.h), sent only when it
 * changes: one step at a time, never back and forth (UI-020). Nothing before the first frame. */
static void light_apply(void)
{
    if (!S.lit) {
        return;
    }
    /* A walk test is lit as an alarm is: at the day brightness, night hours or not (WALK-022);
     * so is a keypad keeping a fire or CO tone through a lost link (its Silence must be seen). */
    int pct = ui_light_percent(&S.opt, (ui_light_stage_t)S.light, S.night,
                               alarm_mode(S.mode) || ui_walk_running() || ui_link_local_alarm());
    if (S.light_preview >= 0) {
        if (!S.saver_on && lv_tick_elaps(S.light_preview_at) < LIGHT_PREVIEW_MS) {
            pct = S.light_preview;
        } else {
            S.light_preview = -1;
        }
    }
    if (pct != S.light_sent) {
        S.light_sent = pct;
        backlight((uint8_t)pct);
    }
}

void ui_light_preview(uint8_t percent)
{
    S.light_preview = percent;
    S.light_preview_at = lv_tick_get();
    light_apply();
}

static void saver_exit(void)
{
    if (!S.saver_on) {
        return;
    }
    S.saver_on = false;
    S.light = UI_LIGHT_AWAKE;
    lv_screen_load(S.wizard_on && !S.wizard_paused ? S.wizard : S.main);
    light_apply();
    lv_display_trigger_activity(S.disp);
}

/* The screen saver, dimmed, or the same screen dark (UI-043): it takes the touch that wakes it,
 * so that touch presses nothing. From the saver it goes on to dark in place. */
static void saver_enter(ui_light_stage_t stage)
{
    if (!S.saver_on) {
        S.saver_on = true;
        activity_forget(); /* the private events never wait behind the saver (LOG-008) */
        ui_saver_prepare(); /* the picture, decoded the first time */
        lv_screen_load(S.saver);
    }
    S.light = (uint8_t)stage;
    light_apply();
}

/* The appearance the options ask for at this minute (UI-010, BL-132; ui_light.h ui_light_theme),
 * applied only when the result changes: Automatic switches once at each end of the night hours,
 * in one step (UI-020). Called when the options change and when the clock's minute does. */
static int s_theme_wanted = -1; /* the last result, -1 before the first */

/* by_clock: the night hours began or ended, or the time was lost. That change waits while an
 * alarm sounds or entry or exit time runs, like a language change: the screen doesn't change
 * its colours under someone's hands; the next minute after it applies. */
static void theme_follow_as(bool by_clock)
{
    ui_theme_t want;
    switch (ui_light_theme(&S.opt, S.local_minute)) {
    case UI_LIGHT_THEME_LIGHT: want = UI_THEME_LIGHT; break;
    case UI_LIGHT_THEME_CONTRAST: want = UI_THEME_CONTRAST; break;
    case UI_LIGHT_THEME_CONTRAST_LIGHT: want = UI_THEME_CONTRAST_LIGHT; break;
    case UI_LIGHT_THEME_DARK:
    default: want = UI_THEME_DARK; break;
    }
    if ((int)want == s_theme_wanted) {
        return;
    }
    if (by_clock && S.main != NULL && ui_app_held()) {
        return; /* tried again at the next minute */
    }
    s_theme_wanted = (int)want;
    if (want != ui_theme_current()) {
        ui_theme_apply(want);
    }
}

static void theme_follow(void)
{
    theme_follow_as(false);
}

void ui_options_changed(void)
{
    theme_follow();
    light_apply();
    if (S.be.set_volume != NULL) {
        S.be.set_volume(S.be.ctx, S.opt.volume);
    }
    if (S.be.set_chime_volume != NULL) {
        S.be.set_chime_volume(S.be.ctx, S.opt.chime_volume);
    }
    if (S.be.set_voice_volume != NULL) {
        S.be.set_voice_volume(S.be.ctx, S.opt.voice_volume);
    }
    if (S.be.save_options != NULL) {
        S.be.save_options(S.be.ctx, &S.opt);
    }
    S.last_minute = -1; /* the clock format may have changed */
}

void ui_app_apply_options(const ui_options_t *opt)
{
    const bool new_language = strncmp(S.opt.lang, opt->lang, sizeof(S.opt.lang)) != 0;
    const uint8_t partition = S.opt.partition;
    S.opt = *opt;
    if (S.opt.partition < 1u || S.opt.partition > ALARM_MAX_PARTITIONS) {
        S.opt.partition = partition; /* range-checked: the one in use stays */
    }
    S.opt.lang[sizeof(S.opt.lang) - 1u] = '\0';
    S.opt.region[sizeof(S.opt.region) - 1u] = '\0';
    ui_options_changed();
    if (new_language) {
        ui_language_apply();
    }
}

/* ---- the right-hand panel and the code pad -------------------------------------------- */

static bool alarm_mode(ui_mode_t m)
{
    return m == MODE_ALARM || m == MODE_FIRE || m == MODE_CO || m == MODE_AUX;
}

/* Exit time, armed, entry time or an alarm: the code is the only thing to enter on Home. */
/* Where the screen rests (BL-220 R8, D8): Smart Home where this screen's start screen says so,
 * while it is in the rail and the partition is disarmed; Security otherwise - and an entry time or
 * an alarm takes Security by itself (apply_mode). */
#define START_SETTLE_MS  3000u   /* a start: nobody touched it for this long */
#define START_GIVE_UP_MS 120000u /* ... nor this long since the start: no devices came */
static bool s_start_pending = true;

static int32_t rest_view(void)
{
#if ALARM_UI_DEVICES
    if (S.opt.start_view == UI_START_SMART_HOME && lv_subject_get_int(&subject_devices_avail) == 1 &&
        (S.mode == MODE_READY || S.mode == MODE_NOT_READY) && !S.wizard_on) {
        return VIEW_DEVICES;
    }
#endif
    return VIEW_HOME;
}

static bool disarm_mode(ui_mode_t m)
{
    return m == MODE_EXIT || m == MODE_AWAY || m == MODE_STAY || m == MODE_NIGHT ||
           m == MODE_ENTRY || alarm_mode(m);
}

/* In those modes the Disarm pad is Home's right-hand side itself, not a card to tap first:
 * it has no X (there is nothing behind it to go back to), and it stays until the reason for
 * it goes. Only what was typed on it is forgotten when nobody finishes (UI-032). */
static bool pad_resident(void)
{
    return S.pad == PAD_DISARM && S.pad_host == HOST_HOME && disarm_mode(S.mode);
}

static void update_panel(void)
{
    int32_t panel;
    /* TR: Home, the title of the card on the right that stands for the code pad */
    const char *title = TR("Disarm");
    /* TR: Home, the line under the title of the card on the right. At most 127 bytes */
    const char *note = TR("Enter your code to disarm");
    char link_title[48];
    char link_note[128];
    if (ui_link_silence_words(link_title, sizeof(link_title), link_note, sizeof(link_note))) {
        /* a keypad keeping a fire or CO tone through a lost link: the way to stop it here
         * (decided 2026-10-15, "until OFF is pressed on the keypad"); steady, like the rest */
        ui_set_str(&subject_disarm_title, link_title);
        ui_set_str(&subject_disarm_note, link_note);
        ui_set_int(&subject_panel, PANEL_SILENCE);
        return;
    }
    if (ui_link_unpaired(link_title, sizeof(link_title), link_note, sizeof(link_note))) {
        /* a keypad the alarm panel removed: the way back (BL-048, LINK-014) */
        ui_set_str(&subject_disarm_title, link_title);
        ui_set_str(&subject_disarm_note, link_note);
        ui_set_int(&subject_panel, PANEL_LINK);
        return;
    }
    if (S.pad != PAD_NONE && S.pad_host == HOST_HOME) {
        panel = PANEL_PAD;
    } else {
        switch (S.mode) {
        case MODE_READY:
        case MODE_NOT_READY: panel = PANEL_ARM; break;
        case MODE_MEMORY: panel = PANEL_MEMORY; break;
        case MODE_WALK_TEST: panel = PANEL_WALK; break;
        default: panel = PANEL_DISARM; break;
        }
    }
    /* The card only stands in for the Disarm pad for the moment between a code that worked
     * and the new state (pad_result refreshes at once); installer mode is its real use. */
    switch (S.mode) {
    case MODE_MEMORY:
        /* TR: Home, the card on the right after an alarm. At most 127 bytes */
        title = TR("Clear alarm");
        /* TR: Home, the card on the right after an alarm. At most 127 bytes */
        note = TR("Enter your code to clear it");
        break;
    case MODE_PROGRAM:
        /* TR: Home, the card on the right while the installer changes settings. At most 127
         * bytes */
        title = TR("Installer mode");
        /* TR: Home, the card on the right: this keypad is in installer mode, or another one
         * is. At most 127 bytes */
        note = S.program
                   ? TR("Tap to finish and leave installer mode")
                   : TR("The installer is changing settings. Arming is off until they finish");
        break;
    default: break;
    }
    ui_set_str(&subject_disarm_title, title);
    ui_set_str(&subject_disarm_note, note);
    ui_set_str(&subject_memory_note, note);
    ui_set_int(&subject_panel, panel);
}

/* The pad's title: the level alone. "No entry delay" is said by the chip the owner ticked (the
 * Overview words it the same way); the title no longer repeats it. */
static const char *arm_title(alarm_arm_level_t level, bool no_entry_delay)
{
    (void)no_entry_delay;
    /* TR: the code pad's title when arming: the arming tiles' names */
    return level == ALARM_LEVEL_STAY    ? TR("Arm Stay")
           : level == ALARM_LEVEL_NIGHT ? TR("Arm Night")
                                        : TR("Arm Away");
}

/* The glyph on the tile that arms at this level (screen_main.xml panel_arm). */
static const char *arm_glyph(alarm_arm_level_t level)
{
    switch (level) {
    case ALARM_LEVEL_STAY: return UI_ICON_HOUSE;
    case ALARM_LEVEL_NIGHT: return UI_ICON_MOON;
    default: return UI_ICON_LOG_OUT;
    }
}

static bool pad_instant(void)
{
    return lv_subject_get_int(&subject_opt_instant) != 0;
}

/* What chime does, in one line: the panel over the screen and Settings say the same thing. */
const char *ui_chime_note(void)
{
    /* TR: what chime does: over the screen when chime is switched, and in Settings. At most 127
     * bytes */
    return TR("A beep at the panel each time a door or window opens, while the system is disarmed");
}

/* The shortest new code allowed. */
static unsigned code_min(void)
{
    return S.cfg->opt.code_min_len > ALARM_CODE_MIN_LEN ? S.cfg->opt.code_min_len
                                                        : ALARM_CODE_MIN_LEN;
}

const char *ui_code_digits(void)
{
    static char text[24];
    /* TR: how long a code may be, {0} to {1} digits (each 1 or 2 bytes); under "Change the code"
     * in Settings. At most 19 bytes */
    I18N_FORMAT(text, sizeof(text),
                TRN("{0} to {1} digit", "{0} to {1} digits", ALARM_CODE_MAX_LEN), code_min(),
                (unsigned)ALARM_CODE_MAX_LEN);
    return text;
}

static const char *new_code_hint(void)
{
    static char hint[48];
    /* TR: the code pad's hint for a new code of {0} to {1} digits (each 1 or 2 bytes). At most
     * 43 bytes */
    I18N_FORMAT(hint, sizeof(hint),
                TRN("Enter a new code, {0} to {1} digit", "Enter a new code, {0} to {1} digits",
                    ALARM_CODE_MAX_LEN),
                code_min(), (unsigned)ALARM_CODE_MAX_LEN);
    return hint;
}

static void pad_texts(void)
{
    char title[64];
    char name[ALARM_NAME_LEN + 8];
    /* TR: the code pad, the line above its keys. At most 95 bytes */
    const char *hint = TR("Enter your code");
    /* The pad carries the icon of the tile or card it stands for, left of the title. Over the
     * screen it adds a line of plain words: what it is about is never read off the screen
     * behind it (UI-025). The titles are cut at 63 bytes, the notes at 127. */
    const char *glyph = NULL;
    const char *note = NULL;
    switch (S.pad) {
    case PAD_ARM:
        i18n_utf8_copy(title, sizeof(title), arm_title(S.pad_level, pad_instant()));
        glyph = arm_glyph(S.pad_level);
        if (S.cfg->opt.quick_arm) {
            /* TR: the code pad when arming needs no code; "check" is the key with a tick */
            hint = TR("Enter your code or press check");
        }
        break;
    case PAD_DISARM:
        /* TR: the code pad's title during exit time, or once armed or in alarm */
        i18n_utf8_copy(title, sizeof(title),
                       S.mode == MODE_EXIT ? TR("Cancel arming") : TR("Disarm"));
        glyph = UI_ICON_LOCK_OPEN;
        break;
    case PAD_CHIME:
        /* TR: the code pad's title when chime is switched with a code */
        i18n_utf8_copy(title, sizeof(title),
                       part()->chime ? TR("Turn chime off") : TR("Turn chime on"));
        glyph = part()->chime ? UI_ICON_BELL_OFF : UI_ICON_BELL;
        note = ui_chime_note();
        break;
    case PAD_BYPASS:
        /* TR: the code pad's title; {0} is the zone's name */
        I18N_FORMAT(title, sizeof(title), S.pad_unbypass ? TR("Unbypass {0}") : TR("Bypass {0}"),
                    ui_zone_name(S.cfg, S.pad_zone, name, sizeof(name)));
        glyph = S.pad_unbypass ? UI_ICON_SHIELD_CHECK : UI_ICON_CIRCLE_SLASH;
        /* TR: the code pad over the Zones list, what bypassing means. At most 127 bytes */
        note = S.pad_unbypass ? TR("The zone is watched again the next time you arm")
                              : TR("A bypassed zone is left out the next time you arm. Bypasses "
                                   "clear when you disarm");
        break;
    case PAD_ACK:
        /* TR: the code pad's title over the Troubles list */
        i18n_utf8_copy(title, sizeof(title), TR("Acknowledge troubles"));
        glyph = UI_ICON_TRIANGLE_ALERT;
        /* TR: the code pad over the Troubles list, what acknowledging does. At most 127 bytes */
        note = TR("Stops the trouble beeping. The troubles stay listed until they are fixed");
        break;
    case PAD_CLEAR_MEMORY:
        i18n_utf8_copy(title, sizeof(title), TR("Clear alarm memory"));
        glyph = UI_ICON_BELL_RING;
        break;
    case PAD_EXTEND_WALK_TEST:
        /* TR: the code pad's title on Home during a walk test */
        i18n_utf8_copy(title, sizeof(title), TR("Extend walk test"));
        glyph = UI_ICON_CLOCK;
        /* TR: the code pad over Home, what extending a walk test does. At most 127 bytes */
        note = TR("The test runs its full time again");
        break;
    case PAD_CODE_CURRENT:
        i18n_utf8_copy(title, sizeof(title), TR("Change your code"));
        hint = TR("Enter your current code");
        glyph = UI_ICON_KEY_ROUND;
        break;
    case PAD_CODE_NEW:
        i18n_utf8_copy(title, sizeof(title), TR("Change your code"));
        hint = new_code_hint();
        glyph = UI_ICON_KEY_ROUND;
        break;
    case PAD_CODE_CONFIRM:
        i18n_utf8_copy(title, sizeof(title), TR("Change your code"));
        hint = TR("Enter the new code again");
        glyph = UI_ICON_KEY_ROUND;
        break;
    case PAD_ASK:
        i18n_utf8_copy(title, sizeof(title), S.pad_ask_title);
        hint = S.pad_ask_hint;
        glyph = S.pad_ask_glyph;
        note = S.pad_ask_note[0] != '\0' ? S.pad_ask_note : NULL;
        break;
    case PAD_ASK_NEW:
        i18n_utf8_copy(title, sizeof(title), S.pad_ask_title);
        hint = new_code_hint();
        glyph = S.pad_ask_glyph;
        break;
    case PAD_ASK_AGAIN:
        i18n_utf8_copy(title, sizeof(title), S.pad_ask_title);
        hint = TR("Enter the new code again");
        glyph = S.pad_ask_glyph;
        break;
    default: title[0] = '\0'; break;
    }
    ui_set_str(&subject_pad_title, title);
    ui_set_str(&subject_pad_hint, hint);
    ui_set_int(&subject_pad_error, 0);
    ui_set_str(&subject_pad_glyph, glyph != NULL ? glyph : "");
    ui_set_int(&subject_pad_glyph_on, glyph != NULL ? 1 : 0);
    ui_set_str(&subject_pad_note, note != NULL ? note : "");
    ui_set_int(&subject_pad_note_on, note != NULL ? 1 : 0);
    ui_set_int(&subject_pad_close_on, pad_resident() ? 0 : 1);
}

static void pad_clear_digits(void)
{
    memset(S.digits, 0, sizeof(S.digits));
    ui_set_int(&subject_pad_digits, 0);
}

void ui_pad_error(const char *text)
{
    ui_set_str(&subject_pad_hint, text);
    ui_set_int(&subject_pad_error, 1);
    shake_dots();
}

static lv_color_t pad_color(pad_t kind)
{
    switch (kind) {
    case PAD_ARM: return ui_tone_for_level(S.pad_level);
    case PAD_DISARM:
    case PAD_ACK:
    case PAD_CLEAR_MEMORY: return ui_tone_green();
    case PAD_ASK: return S.pad_ask_tone;
    default: return ui_tone_blue();
    }
}

static void pad_show(pad_host_t host)
{
    S.pad_host = host;
    ui_set_int(&subject_settings_pad, host == HOST_SETTINGS ? 1 : 0);
    ui_set_int(&subject_wiz_pad, host == HOST_WIZARD ? 1 : 0);
    ui_set_int(&subject_modal_pad, host == HOST_MODAL ? 1 : 0);
    if (host == HOST_MODAL && S.toast_on) {
        toast_hide();
    }
}

static void pad_open_in(pad_host_t host, pad_t kind)
{
    if (S.pad == PAD_NONE || S.pad_host != HOST_HOME) {
        S.pad_return_view = lv_subject_get_int(&subject_view);
    }
    if (S.pad_host != HOST_HOME) {
        S.pad_fn = NULL; /* an alarm replaced a settings or setup pad */
    }
    pad_show(host);
    S.pad = kind;
    ui_set_color(&subject_pad_color, pad_color(kind));
    pad_clear_digits();
    ui_set_int(&subject_pad_options, kind == PAD_ARM ? 1 : 0);
    ui_set_int(&subject_pad_silence,
               (kind == PAD_DISARM && (S.mode == MODE_FIRE || S.mode == MODE_CO || S.mode == MODE_AUX))
                   ? 1
                   : 0);
    if (kind == PAD_ARM) {
        ui_set_int(&subject_opt_instant, 0);
        ui_set_int(&subject_opt_silent, 0);
        ui_set_int(&subject_opt_silent_avail, S.pad_level == ALARM_LEVEL_AWAY ? 1 : 0);
        ui_set_int(&subject_opt_bypass, 0);
        ui_set_int(&subject_opt_bypass_avail, part()->ready ? 0 : 1);
    }
    pad_texts();
    if (host == HOST_HOME) {
        ui_set_int(&subject_view, VIEW_HOME);
    }
    update_panel();
}

/* Arming and disarming own the Home screen; everything else asks over the screen (UI-025). */
static void pad_open(pad_t kind)
{
    pad_open_in(HOST_HOME, kind);
}

/* Put the resident Disarm pad on Home without taking anyone off the view they are reading
 * (armed from a keyfob while the Zones list is open). Entry time and alarms bring Home
 * forward themselves (mode_entered). */
static void disarm_pad_keep(void)
{
    int32_t view = lv_subject_get_int(&subject_view);
    pad_open_in(HOST_HOME, PAD_DISARM);
    S.pad_return_view = VIEW_HOME;
    if (view == VIEW_SETTINGS) {
        view = VIEW_HOME; /* locked while armed */
    }
    ui_set_int(&subject_view, view);
}

static void pad_close(void)
{
    const pad_host_t host = S.pad_host;
    S.pad = PAD_NONE;
    S.pad_fn = NULL;
    pad_clear_digits();
    memset(S.code_current, 0, sizeof(S.code_current));
    memset(S.code_new, 0, sizeof(S.code_new));
    pad_show(HOST_HOME);
    if (host != HOST_HOME && disarm_mode(S.mode)) {
        disarm_pad_keep(); /* what the pad over the screen covered comes back */
    }
    update_panel();
    if (host != HOST_HOME) {
        if (host == HOST_MODAL && lv_subject_get_int(&subject_view) == VIEW_SETTINGS) {
            ui_page_rebuild(); /* the row behind the panel shows what the system does now */
        }
        return;
    }
    /* Back to where the pad was opened from (bypass from Zones, acknowledge from Troubles). */
    if ((S.pad_return_view == VIEW_ZONES || S.pad_return_view == VIEW_TROUBLES) &&
        S.mode != MODE_ENTRY && !alarm_mode(S.mode)) {
        ui_set_int(&subject_view, S.pad_return_view);
    }
    S.pad_return_view = VIEW_HOME;
}

void ui_pad_cancel(void)
{
    if (S.pad != PAD_NONE && S.pad_host != HOST_HOME) {
        pad_close();
    }
}

bool ui_app_busy(void)
{
    return (S.pad != PAD_NONE && !(pad_resident() && S.digits[0] == '\0')) || S.panic_holding ||
           lv_subject_get_int(&subject_view) == VIEW_SETTINGS || (S.wizard_on && !S.wizard_paused) ||
           lv_subject_get_int(&subject_sheet) != SHEET_NONE || ui_call_waiting();
}

bool ui_pad_asks(ui_code_fn fn)
{
    return S.pad == PAD_ASK && S.pad_fn == fn;
}

void ui_app_wake(void)
{
    saver_exit();
    lv_display_trigger_activity(S.disp);
}

void ui_app_walk_show(void)
{
    saver_exit();
    lv_display_trigger_activity(S.disp);
    ui_sheet_close();
    if (S.pad != PAD_NONE && !pad_resident()) {
        pad_close(); /* the person chose to see the test: what they had open goes */
    }
    ui_set_int(&subject_view, VIEW_HOME);
}

static void pad_close_by_user(void)
{
    if (pad_resident()) {
        return; /* it has no X; nothing to go back to */
    }
    const bool settings = S.pad_host == HOST_SETTINGS;
    pad_close();
    if (settings) {
        ui_page_rebuild(); /* the page behind the pad (usually "locked") */
    }
}

/* Nobody touched the screen while a code pad was open. Half a code is a credential - the next
 * person past the panel must not be able to finish somebody else's entry - so what was typed
 * is dropped in every case, on every host: Home, Settings, the first-boot setup and the panel
 * over the screen (UI-032).
 * Entry time and a sounding alarm are the exception, and only about the pad itself: it is how
 * the house is disarmed and the screen must go on showing the alarm, so the pad stays exactly
 * where it is and only the digits and the stale error go. */
/* Whatever was typed so far, gone; the pad itself stays. */
static void pad_forget_typing(void)
{
    if (S.pad != PAD_NONE &&
        (S.digits[0] != '\0' || lv_subject_get_int(&subject_pad_error) != 0)) {
        pad_clear_digits();
        memset(S.code_current, 0, sizeof(S.code_current));
        memset(S.code_new, 0, sizeof(S.code_new));
        pad_texts(); /* fresh hint, no error left over */
    }
}

static void pad_idle_expired(void)
{
    if (S.mode == MODE_ENTRY || alarm_mode(S.mode) || pad_resident()) {
        pad_forget_typing();
        return;
    }
    const pad_host_t host = S.pad_host;
    pad_close(); /* clears the digits and both half-typed new codes */
    if (host == HOST_SETTINGS) {
        ui_page_rebuild(); /* the page behind the pad (usually "locked") */
    }
}

static void ask(pad_host_t host, pad_t kind, const char *glyph, const char *title,
                const char *hint, lv_color_t tone, ui_code_fn fn)
{
    S.pad_ask_glyph = glyph;
    i18n_utf8_copy(S.pad_ask_title, sizeof(S.pad_ask_title), title);
    i18n_utf8_copy(S.pad_ask_hint, sizeof(S.pad_ask_hint),
                   hint != NULL ? hint : TR("Enter your code"));
    S.pad_ask_note[0] = '\0';
    S.pad_ask_tone = tone;
    S.pad_fn = fn;
    S.pad = kind;
    pad_show(host);
    ui_set_color(&subject_pad_color, pad_color(kind));
    ui_set_int(&subject_pad_options, 0);
    ui_set_int(&subject_pad_silence, 0);
    pad_clear_digits();
    pad_texts();
    update_panel();
}

void ui_pad_ask(pad_host_t host, const char *glyph, const char *title, const char *hint,
                lv_color_t tone, ui_code_fn fn)
{
    ask(host, PAD_ASK, glyph, title, hint, tone, fn);
}

void ui_pad_ask_new(pad_host_t host, const char *glyph, const char *title, ui_code_fn fn)
{
    ask(host, PAD_ASK_NEW, glyph, title, NULL, ui_tone_blue(), fn);
}

void ui_pad_ask_note(const char *note)
{
    i18n_utf8_copy(S.pad_ask_note, sizeof(S.pad_ask_note), note != NULL ? note : "");
    if (S.pad == PAD_ASK) {
        pad_texts();
    }
}

static void pad_result(alarm_result_t r, const alarm_cmd_result_t *res)
{
    char text[96];
    char name[ALARM_NAME_LEN + 8];
    switch (r) {
    case ALARM_OK:
        /* No toast: the view the pad closes onto shows the outcome itself - the state card
         * arms or disarms, the zone card says Bypassed, the troubles clear, the rail's bell
         * changes. A toast would only repeat what is already on the screen. */
        pad_close();
        ui_refresh(true);
        return;
    case ALARM_ERR_NOT_READY:
        if (res != NULL && res->not_ready_zone != 0u) {
            const uint16_t z = res->not_ready_zone;
            const bool open = z <= S.snap.zones_used && (S.snap.zone[z - 1u].flags & ALARM_ZS_FAULT);
            /* One line: "Bypass open" appears beside it as the way on (UI-004). The same
             * words as Home's line for one zone, so the plural entry with n = 1. */
            /* TR: the code pad's hint when arming is refused; {0} is the zone's name */
            I18N_FORMAT(text, sizeof(text),
                        open ? TRN("{0} is open", "{0} are open", 1u) : TR("{0} isn't ready"),
                        ui_zone_name(S.cfg, z, name, sizeof(name)));
        } else {
            /* TR: the code pad's hint when arming is refused. At most 95 bytes */
            i18n_utf8_copy(text, sizeof(text), TR("Not ready: a zone is open"));
        }
        ui_set_int(&subject_opt_bypass_avail, 1);
        ui_set_str(&subject_pad_hint, text);
        ui_set_int(&subject_pad_error, 1);
        return; /* keep the digits: tick "Bypass open" and press check again */
    case ALARM_ERR_BAD_STATE:
        pad_clear_digits();
        if (S.pad == PAD_ARM && part()->walk_test) {
            /* TR: the code pad's error when arming during a walk test (WALK-005) */
            ui_pad_error(TR("A walk test is running"));
        } else if (S.pad == PAD_EXTEND_WALK_TEST) {
            /* TR: the code pad's error: the walk test ended while the pad was open */
            ui_pad_error(TR("The walk test has already ended"));
        } else {
            /* TR: the code pad's error when arming while installer mode is on */
            ui_pad_error(S.snap.program && S.pad == PAD_ARM ? TR("Not while the installer is working")
                                                            : ui_result_text(r));
        }
        return;
    default:
        pad_clear_digits();
        ui_pad_error(ui_result_text(r));
        return;
    }
}

/* The pad asks on behalf of a callback (settings, installer mode, setup). */
static void pad_submit_ask(size_t len)
{
    if (S.pad == PAD_ASK) {
        char code[ALARM_CODE_MAX_LEN + 1];
        memcpy(code, S.digits, sizeof(code));
        pad_clear_digits();
        const ui_code_fn fn = S.pad_fn;
        const bool done = fn == NULL || fn(code);
        memset(code, 0, sizeof(code));
        if (done && S.pad == PAD_ASK && S.pad_fn == fn) {
            pad_close(); /* unless the callback moved on to another request */
        }
        return;
    }
    if (S.pad == PAD_ASK_NEW) {
        if (len < S.cfg->opt.code_min_len) {
            char text[64];
            /* TR: the code pad's error: a new code is too short */
            I18N_FORMAT(text, sizeof(text),
                        TRN("Use at least {0} digit", "Use at least {0} digits",
                            S.cfg->opt.code_min_len),
                        (unsigned)S.cfg->opt.code_min_len);
            ui_beep(UI_BEEP_ERROR);
            pad_clear_digits();
            ui_pad_error(text);
            return;
        }
        memcpy(S.code_new, S.digits, sizeof(S.code_new));
        S.pad = PAD_ASK_AGAIN;
        pad_clear_digits();
        pad_texts();
        return;
    }
    /* PAD_ASK_AGAIN */
    if (strcmp(S.digits, S.code_new) != 0) {
        S.pad = PAD_ASK_NEW;
        pad_clear_digits();
        ui_beep(UI_BEEP_ERROR);
        pad_texts();
        ui_pad_error(TR("Those didn't match. Try again"));
        return;
    }
    char code[ALARM_CODE_MAX_LEN + 1];
    memcpy(code, S.code_new, sizeof(code));
    pad_clear_digits();
    const ui_code_fn fn = S.pad_fn;
    const bool done = fn == NULL || fn(code);
    memset(code, 0, sizeof(code));
    if (done && S.pad == PAD_ASK_AGAIN && S.pad_fn == fn) {
        pad_close();
    } else if (!done) {
        S.pad = PAD_ASK_NEW; /* try another code (the callback reported why) */
        memset(S.code_new, 0, sizeof(S.code_new));
    }
}

/* The pad a call's answer is for: the same one still open (LINK-054: the screen went on while
 * the call waited, and a timeout or an alarm may have closed it). */
static uintptr_t pad_tag(void)
{
    return (uintptr_t)S.pad | ((uintptr_t)S.pad_host << 8);
}

static bool pad_still(const ui_call_t *c)
{
    return c->tag == pad_tag() && S.pad != PAD_NONE;
}

static void code_changed(const ui_call_t *c)
{
    if (!pad_still(c)) {
        ui_refresh(true);
        return;
    }
    if (c->r == ALARM_OK) {
        char name[ALARM_NAME_LEN + 8];
        char text[96];
        /* TR: a toast after a code was changed; {0} is the user's name */
        I18N_FORMAT(text, sizeof(text), TR("Code changed for {0}"),
                    ui_user_name(S.cfg, c->user, name, sizeof(name)));
        pad_close();
        ui_toast(text);
        ui_refresh(true);
    } else {
        S.pad = (c->r == ALARM_ERR_INVALID_CODE || c->r == ALARM_ERR_LOCKED_OUT) ? PAD_CODE_CURRENT
                                                                                  : PAD_CODE_NEW;
        pad_clear_digits();
        pad_texts();
        ui_pad_error(c->r == ALARM_ERR_INVALID_CODE ? TR("Your current code is wrong")
                                                    : ui_result_text(c->r));
    }
}

static void pad_answered(const ui_call_t *c)
{
    if (!pad_still(c)) {
        ui_refresh(true);
        return;
    }
    pad_result(c->r, &c->res);
}

static void pad_submit(void)
{
    const size_t len = strlen(S.digits);
    const bool quick = S.pad == PAD_ARM && len == 0u && S.cfg->opt.quick_arm;
    if (len < ALARM_CODE_MIN_LEN && !quick) {
        char text[64];
        ui_beep(UI_BEEP_ERROR);
        if (len == 0u) {
            /* TR: the code pad's error when OK is pressed with nothing typed. At most 63 bytes */
            i18n_utf8_copy(text, sizeof(text), TR("Enter your code first"));
        } else {
            /* TR: the code pad's error: the code typed is shorter than any code can be; {0} is
             * 1 or 2 bytes. At most 61 bytes */
            I18N_FORMAT(text, sizeof(text),
                        TRN("Codes have at least {0} digit", "Codes have at least {0} digits",
                            ALARM_CODE_MIN_LEN),
                        (unsigned)ALARM_CODE_MIN_LEN);
        }
        ui_pad_error(text);
        return;
    }
    if (S.pad == PAD_ASK || S.pad == PAD_ASK_NEW || S.pad == PAD_ASK_AGAIN) {
        pad_submit_ask(len);
        return;
    }

    if (S.pad == PAD_CODE_CURRENT) {
        memcpy(S.code_current, S.digits, sizeof(S.code_current));
        S.pad = PAD_CODE_NEW;
        pad_clear_digits();
        pad_texts();
        return;
    }
    if (S.pad == PAD_CODE_NEW) {
        if (len < S.cfg->opt.code_min_len) {
            char text[64];
            /* TR: the code pad's error: a new code is too short */
            I18N_FORMAT(text, sizeof(text),
                        TRN("Use at least {0} digit", "Use at least {0} digits",
                            S.cfg->opt.code_min_len),
                        (unsigned)S.cfg->opt.code_min_len);
            ui_beep(UI_BEEP_ERROR);
            pad_clear_digits();
            ui_pad_error(text);
            return;
        }
        memcpy(S.code_new, S.digits, sizeof(S.code_new));
        S.pad = PAD_CODE_CONFIRM;
        pad_clear_digits();
        pad_texts();
        return;
    }
    if (S.pad == PAD_CODE_CONFIRM) {
        if (strcmp(S.digits, S.code_new) != 0) {
            S.pad = PAD_CODE_NEW;
            pad_clear_digits();
            ui_beep(UI_BEEP_ERROR);
            pad_texts();
            ui_pad_error(TR("Those didn't match. Try again"));
            return;
        }
        if (!ui_call_change_code(S.code_current, S.code_new, code_changed, pad_tag())) {
            pad_clear_digits();
            ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
        }
        return;
    }

    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.partition = S.opt.partition;
    memcpy(cmd.code, S.digits, sizeof(cmd.code));
    switch (S.pad) {
    case PAD_ARM:
        cmd.type = ALARM_CMD_ARM;
        cmd.level = S.pad_level;
        cmd.arm_flags = (uint8_t)((pad_instant() ? ALARM_ARM_NO_ENTRY_DELAY : 0u) |
                                  ((lv_subject_get_int(&subject_opt_silent) &&
                                    lv_subject_get_int(&subject_opt_silent_avail))
                                       ? ALARM_ARM_SILENT_EXIT
                                       : 0u) |
                                  (lv_subject_get_int(&subject_opt_bypass) ? ALARM_ARM_BYPASS_FAULTED : 0u));
        break;
    case PAD_DISARM:
    case PAD_ACK:
    case PAD_CLEAR_MEMORY: cmd.type = ALARM_CMD_DISARM; break;
    case PAD_CHIME:
        cmd.type = ALARM_CMD_CHIME;
        S.chime_was_on = part()->chime;
        break;
    case PAD_BYPASS:
        cmd.type = S.pad_unbypass ? ALARM_CMD_UNBYPASS : ALARM_CMD_BYPASS;
        cmd.zone = S.pad_zone;
        break;
    case PAD_EXTEND_WALK_TEST: /* never a toggle (WALK-002) */
        cmd.type = ALARM_CMD_WALK_TEST;
        cmd.walk_op = (uint8_t)ALARM_WALK_EXTEND;
        break;
    default: return;
    }
    const bool queued = ui_call_command(&cmd, pad_answered, pad_tag());
    memset(cmd.code, 0, sizeof(cmd.code));
    if (!queued) {
        pad_result(ALARM_ERR_UNAVAILABLE, NULL);
    }
}

static void pad_key(const char *key)
{
    if (S.pad == PAD_NONE || key == NULL) {
        return;
    }
    size_t len = strlen(S.digits);
    if (strcmp(key, "ok") == 0) {
        pad_submit();
        return;
    }
    if (strcmp(key, "del") == 0) {
        if (len > 0u) {
            S.digits[--len] = '\0';
        }
    } else if (key[0] >= '0' && key[0] <= '9' && key[1] == '\0') {
        if (len < ALARM_CODE_MAX_LEN) {
            S.digits[len++] = key[0];
            S.digits[len] = '\0';
        }
    }
    ui_set_int(&subject_pad_digits, (int32_t)len);
    if (lv_subject_get_int(&subject_pad_error)) {
        pad_texts(); /* typing again clears the error */
    }
}

/* ---- modes ---------------------------------------------------------------------------- */

static ui_mode_t compute_mode(const alarm_partition_status_t *ps)
{
    switch (ps->sounder) {
    case ALARM_SOUND_FIRE: return MODE_FIRE;
    case ALARM_SOUND_CO: return MODE_CO;
    case ALARM_SOUND_ALARM: return MODE_ALARM;
    case ALARM_SOUND_AUX: return MODE_AUX;
    default: break;
    }
    switch (ps->state) {
    case ALARM_STATE_ENTRY_DELAY: return MODE_ENTRY;
    case ALARM_STATE_EXIT_DELAY: return MODE_EXIT;
    case ALARM_STATE_ARMED:
        if (ps->alarms > 0u) {
            return MODE_ALARM; /* the siren timed out; the alarm stays on the display */
        }
        return ps->level == ALARM_LEVEL_STAY    ? MODE_STAY
               : ps->level == ALARM_LEVEL_NIGHT ? MODE_NIGHT
                                                : MODE_AWAY;
    default: break;
    }
    if (ps->walk_test) {
        return MODE_WALK_TEST;
    }
    if ((ps->memory & (uint16_t)~ALARM_MEM_CANCELED) != 0u || ps->alarms > 0u) {
        return MODE_MEMORY;
    }
    if (S.snap.program) {
        return MODE_PROGRAM;
    }
    return ps->ready ? MODE_READY : MODE_NOT_READY;
}

/* Newest event of a type for our partition, from the event ring. */
static bool find_event(alarm_event_type_t type, alarm_event_t *out)
{
    enum { RECENT = 24 };
    static alarm_event_t *evs;
    if (evs == NULL && (evs = ui_slow_alloc(RECENT * sizeof(*evs))) == NULL) {
        return false;
    }
    const size_t n = S.be.recent_events(S.be.ctx, evs, RECENT);
    for (size_t i = 0; i < n; i++) {
        if (evs[i].type == type && (evs[i].partition == S.opt.partition || evs[i].partition == 0u)) {
            *out = evs[i];
            return true;
        }
    }
    return false;
}

static void mode_entered(ui_mode_t m, ui_mode_t prev, bool first)
{
    alarm_event_t ev;
    if (m == MODE_EXIT || m == MODE_ENTRY) {
        S.count_total_ms = 0;
    }
    if (m == MODE_ENTRY) {
        S.entry_zone = find_event(ALARM_EV_ENTRY_DELAY, &ev) ? ev.zone : 0u;
    }
    if (m == MODE_AWAY || m == MODE_STAY || m == MODE_NIGHT) {
        if (find_event(ALARM_EV_ARMED, &ev)) {
            S.armed_user = ev.user;
            S.armed_epoch_ms = ev.epoch_ms;
            S.armed_by_auto = ev.source.kind == (uint8_t)ALARM_SRC_AUTOMATION;
            S.armed_bypassed = S.armed_by_auto ? ALARM_ARMED_BYPASSED(ev.arg2) : 0u;
        } else {
            /* the snapshot's arm_source (C2c) still says an automation did it */
            S.armed_user = part()->armed_by;
            S.armed_epoch_ms = 0;
            S.armed_by_auto = part()->arm_source.kind == (uint8_t)ALARM_SRC_AUTOMATION;
            S.armed_bypassed = 0u;
        }
    }
    if (m == MODE_ENTRY || alarm_mode(m)) {
        saver_exit();
        ui_sheet_close();
        if (S.wizard_on) {
            ui_wizard_pause(); /* the alarm needs the main screen */
        }
        ui_set_int(&subject_view, VIEW_HOME);
        if (S.pad != PAD_DISARM || S.pad_host != HOST_HOME) {
            pad_open(PAD_DISARM);
        } else {
            pad_texts(); /* entry time turned into an alarm: fresh title, no stale error */
        }
    } else if (disarm_mode(m)) {
        /* Exit time and armed: the pad is there without a tap on a Disarm card first. */
        if (S.pad != PAD_DISARM || S.pad_host != HOST_HOME) {
            disarm_pad_keep();
        } else {
            pad_texts(); /* "Cancel arming" became "Disarm": fresh title, no stale error */
        }
    } else if (S.pad_host == HOST_HOME &&
               (S.pad == PAD_DISARM || S.pad == PAD_CLEAR_MEMORY || S.pad == PAD_EXTEND_WALK_TEST)) {
        pad_close(); /* the reason for the pad went away (disarmed elsewhere, alarm over...) */
    }
    if ((m == MODE_READY || m == MODE_NOT_READY) && S.wizard_on && S.wizard_paused) {
        ui_wizard_resume();
    }
    if (!first && m != prev) {
        /* Any change of state wakes the screen, and the screen saver waits its full time again
         * from here: armed after a long exit time, the new state stays in view before it dims. */
        saver_exit();
        lv_display_trigger_activity(S.disp);
    }
}

/* A keypad that can't show the alarm panel's state - connecting, no system communication, not
 * paired (BL-048, LINK-008): its own words on the state card, steady, and nothing of the last state
 * it had, which may be stale: no alarm look, no pad to disarm with, the tiles dimmed (the XML,
 * subject_link_home). The real state comes back with the link (apply_mode's mode_entered). */
static void apply_link_home(const ui_link_home_t *lh)
{
    /* calm: the screen saver may come, as on a quiet Home - unless a fire or CO tone is kept
     * here (ui_link_local_alarm: lit and awake as an alarm, ui_app_wake when it starts) */
    S.mode = MODE_READY;
    S.mode_valid = true;
    if (S.pad != PAD_NONE && S.pad_host == HOST_HOME) {
        pad_close();
    }
    hero_tone(lh->tone, false);
    ui_set_str(&subject_hero_icon, lh->glyph);
    ui_set_str(&subject_headline, lh->headline);
    ui_set_str(&subject_detail, lh->detail);
    ui_set_str(&subject_hero_note, lh->note);
    ui_set_int(&subject_alarm_active, 0);
    set_pulse(false);
    update_panel();
}

static void apply_mode(void)
{
    ui_link_home_t lh;
    if (ui_link_home_words(&lh)) {
        apply_link_home(&lh);
        return;
    }
    const alarm_partition_status_t *ps = part();
    const ui_mode_t prev = S.mode;
    const ui_mode_t m = compute_mode(ps);
    const bool first = !S.mode_valid;
    S.mode = m;
    S.mode_valid = true;
    if (first || m != prev) {
        mode_entered(m, prev, first);
    }

    const char *icon = UI_ICON_SHIELD_CHECK;
    lv_color_t tone = ui_tone_green();
    /* Home's state card: the headline, the line under it (detail) and the note at its foot. */
    char headline[48];
    char detail[128];
    char note[128];
    char zones[96];
    char name[ALARM_NAME_LEN + 8];
    detail[0] = '\0';
    note[0] = '\0';
    const bool ned = (ps->arm_flags & ALARM_ARM_NO_ENTRY_DELAY) != 0u;

    switch (m) {
    case MODE_READY:
        /* TR: Home, the state card's headline. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline), TR("Disarmed"));
        /* TR: Home, the line under the state card's headline. At most 127 bytes */
        i18n_utf8_copy(detail, sizeof(detail), TR("Ready to arm"));
        /* TR: Home, facts at the foot of the state card, " · " between them; "Chime on" says
         * chime is switched on */
        snprintf(note, sizeof(note), "%s%s%s", TR("All zones closed"), ps->chime ? " · " : "",
                 ps->chime ? TR("Chime on") : "");
        break;
    case MODE_NOT_READY: {
        const unsigned n = count_zones(ALARM_ZS_NOT_READY);
        /* A tamper or a sensor fault holds arming back too: a shut door with a trouble is not
         * "open", and "Close it" would send someone to a door that is already closed. */
        bool all_open = true;
        for (uint16_t z = 1; z <= S.snap.zones_used; z++) {
            const uint16_t f = S.snap.zone[z - 1u].flags;
            if (zone_in_partition(z) && (f & ALARM_ZS_NOT_READY) && !(f & ALARM_ZS_FAULT)) {
                all_open = false;
            }
        }
        icon = UI_ICON_SHIELD_ALERT;
        tone = ui_tone_amber();
        /* TR: Home, the state card's headline. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline), TR("Not ready"));
        zone_list_text(ALARM_ZS_NOT_READY, zones, sizeof(zones));
        if (n <= 2u && zones[0] != '\0') {
            /* TR: Home, under the state card's headline; {0} is one zone's name or two ("Front
             * Door and Back Door"), up to 95 bytes. At most 32 bytes */
            I18N_FORMAT(detail, sizeof(detail),
                        all_open ? TRN("{0} is open", "{0} are open", n)
                                 : TRN("{0} is not ready", "{0} are not ready", n),
                        zones);
        } else {
            /* TR: Home, under the state card's headline; {0} is how many zones, up to 3 bytes. At
             * most 124 bytes */
            I18N_FORMAT(detail, sizeof(detail),
                        all_open ? TRN("{0} zone is open", "{0} zones are open", n)
                                 : TRN("{0} zone is not ready", "{0} zones are not ready", n),
                        n);
        }
        /* TR: Home, the foot of the state card: what to do about the zones named above. At most
         * 127 bytes */
        i18n_utf8_copy(note, sizeof(note),
                       all_open ? TRN("Close it, or bypass when arming",
                                      "Close them, or bypass when arming", n)
                                : TRN("Check it, or bypass when arming",
                                      "Check them, or bypass when arming", n));
        break;
    }
    case MODE_PROGRAM:
        icon = UI_ICON_SHIELD;
        tone = ui_tone_blue();
        /* TR: Home, the state card's headline. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline), TR("Installer mode"));
        /* TR: Home, the line under the state card's headline. At most 127 bytes */
        i18n_utf8_copy(detail, sizeof(detail), TR("Settings are being changed"));
        /* TR: Home, the foot of the state card. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note), TR("Fire, CO and panic still work · Arming is off"));
        break;
    case MODE_EXIT:
        icon = UI_ICON_LOG_OUT;
        tone = ui_tone_for_level(ps->level);
        /* TR: Home, the state card's headline during exit time; Away, Stay and Night are the
         * arming tiles' names. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline),
                       ps->level == ALARM_LEVEL_STAY    ? TR("Arming Stay")
                       : ps->level == ALARM_LEVEL_NIGHT ? TR("Arming Night")
                                                        : TR("Arming Away"));
        /* TR: Home, the line under the state card's headline during exit time. At most 127
         * bytes */
        i18n_utf8_copy(detail, sizeof(detail),
                       ps->level == ALARM_LEVEL_AWAY ? TR("Leave now through the exit door")
                                                     : TR("Stay inside, arming shortly"));
        /* Only Away beeps through exit time; Stay and Night are quiet. */
        /* TR: Home, facts at the foot of the state card during exit time, " · " between them */
        snprintf(note, sizeof(note), "%s%s%s",
                 ps->level != ALARM_LEVEL_AWAY                   ? TR("Quiet exit time")
                 : (ps->arm_flags & ALARM_ARM_SILENT_EXIT) != 0u ? TR("Silent exit · No beeps")
                                                                  : TR("Exit time"),
                 ned ? " · " : "", ned ? TR("No entry delay") : "");
        break;
    case MODE_AWAY:
    case MODE_STAY:
    case MODE_NIGHT: {
        icon = m == MODE_AWAY ? UI_ICON_LOCK : m == MODE_STAY ? UI_ICON_HOUSE : UI_ICON_MOON;
        tone = ui_tone_for_level(ps->level);
        /* TR: Home, the state card's headline once armed; Away, Stay and Night are the arming
         * tiles' names. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline),
                       ps->level == ALARM_LEVEL_STAY    ? TR("Armed Stay")
                       : ps->level == ALARM_LEVEL_NIGHT ? TR("Armed Night")
                                                        : TR("Armed Away"));
        /* TR: Home, the line under the state card's headline once armed. At most 127 bytes */
        i18n_utf8_copy(detail, sizeof(detail),
                       m == MODE_AWAY   ? TR("The whole house is protected")
                       : m == MODE_STAY ? TR("Doors and windows are protected")
                                        : TR("Doors, windows and night zones"));
        char when[24] = "";
        if (S.armed_epoch_ms > 0) {
            char t[16];
            ui_clock_text(S.armed_epoch_ms, t, sizeof(t), true);
            /* TR: Home, the foot of the state card: when it was armed; {0} is a time of day
             * ("10:41 PM", 8 bytes). At most 15 bytes */
            I18N_FORMAT(when, sizeof(when), TR("Since {0}"), t);
        }
        char by[ALARM_NAME_LEN + 40] = "";
        char bypassed[40] = "";
        if (S.armed_by_auto) {
            /* D30, AUT-009: an automation armed it, never read as the person who saved it.
             * TR: Who did it: one of the panel's automations, not a person. At most 48 bytes */
            i18n_utf8_copy(by, sizeof(by), TR("By automation"));
            if (S.armed_bypassed > 0u) {
                /* TR: Home, the foot of the state card after "By automation": how many open
                 * zones it bypassed to arm ("1 zone bypassed"). At most 32 bytes */
                I18N_FORMAT(bypassed, sizeof(bypassed),
                            TRN("{0} zone bypassed", "{0} zones bypassed", S.armed_bypassed),
                            (unsigned)S.armed_bypassed);
            }
        } else {
            ui_user_name(S.cfg, S.armed_user, name, sizeof(name));
            if (name[0] != '\0') {
                /* TR: Home, the foot of the state card: who armed it; {0} is the user's name */
                I18N_FORMAT(by, sizeof(by), TR("By {0}"), name);
            }
        }
        /* Home, facts at the foot of the state card once armed, " · " between them
         * ("Since 10:30 PM · By automation · 1 zone bypassed").
         * TR: Home, the foot of the state card once armed: the arming has no entry delay. At
         * most 40 bytes */
        const char *facts[4] = {when, by, bypassed, ned ? TR("No entry delay") : ""};
        size_t used = 0u;
        note[0] = '\0';
        for (size_t f = 0; f < 4u; f++) {
            if (facts[f][0] == '\0') {
                continue;
            }
            if (used > 0u) {
                i18n_utf8_copy(note + used, sizeof(note) - used, " · ");
                used = strlen(note);
            }
            i18n_utf8_copy(note + used, sizeof(note) - used, facts[f]);
            used = strlen(note);
        }
        break;
    }
    case MODE_ENTRY:
        icon = UI_ICON_DOOR_OPEN;
        tone = ui_tone_orange();
        /* TR: Home, the state card's headline during entry time. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline), TR("Disarm now"));
        if (S.entry_zone != 0u) {
            /* TR: Home, under the state card's headline during entry time; {0} is the zone's
             * name, up to 23 bytes. At most 104 bytes */
            I18N_FORMAT(detail, sizeof(detail), TR("Entry through {0}"),
                        ui_zone_name(S.cfg, S.entry_zone, name, sizeof(name)));
        } else {
            i18n_utf8_copy(detail, sizeof(detail), TR("Enter your code"));
        }
        /* TR: Home, the foot of the state card during entry time. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note),
                       ps->exit_error ? TR("A zone was left open after exit time")
                                      : TR("The alarm sounds when time runs out"));
        break;
    case MODE_ALARM:
    case MODE_FIRE:
    case MODE_CO:
    case MODE_AUX: {
        const alarm_cause_t cause = ui_top_cause(ps->memory);
        icon = m == MODE_FIRE ? UI_ICON_FLAME
               : m == MODE_CO ? UI_ICON_CLOUD_FOG
               : m == MODE_AUX ? UI_ICON_HEART_PULSE
               : (cause == ALARM_CAUSE_FIRE || cause == ALARM_CAUSE_PANIC_FIRE) ? UI_ICON_FLAME
               : (cause == ALARM_CAUSE_CO) ? UI_ICON_CLOUD_FOG
                                           : UI_ICON_SIREN;
        tone = ui_tone_red();
        /* TR: Home, the state card's headline while the alarm sounds. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline),
                       m == MODE_FIRE ? TR("Fire alarm")
                       : m == MODE_CO ? TR("Carbon monoxide")
                                      : ui_cause_title(cause));
        zone_list_text(ALARM_ZS_ALARM, zones, sizeof(zones));
        /* TR: Home, under the state card's headline: an alarm no zone set off. At most 127
         * bytes */
        i18n_utf8_copy(detail, sizeof(detail), zones[0] ? zones : TR("Keypad or remote panic"));
        /* TR: Home, the foot of the state card while the alarm sounds. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note),
                       m == MODE_FIRE ? TR("Get everyone out. Silence stops the siren")
                       : m == MODE_CO ? TR("Get everyone into fresh air")
                       : ps->sounder == ALARM_SOUND_NONE ? TR("The siren timed out")
                                                         : TR("Enter your code to disarm"));
        break;
    }
    case MODE_MEMORY:
        icon = UI_ICON_BELL_RING;
        tone = ui_tone_amber();
        /* TR: Home, the state card's headline after an alarm. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline), (ps->memory & ALARM_MEM_CANCELED)
                                                       ? TR("Alarm canceled")
                                                       : TR("Alarm occurred"));
        zone_list_text(ALARM_ZS_ALARM, zones, sizeof(zones));
        if (zones[0] != '\0') {
            /* TR: Home, under the state card's headline after an alarm: {0} what it was
             * ("Burglary"), {1} the zones ("Front Door and Back Door") */
            I18N_FORMAT(detail, sizeof(detail), TR("{0}: {1}"),
                        ui_cause_title(ui_top_cause(ps->memory)), zones);
        } else {
            i18n_utf8_copy(detail, sizeof(detail), ui_cause_title(ui_top_cause(ps->memory)));
        }
        /* TR: Home, the foot of the state card after an alarm. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note),
                       (ps->memory & ALARM_MEM_CANCELED)
                           ? TR("No alert was sent. Clear it with your code")
                           : TR("Review it, then clear it with your code"));
        break;
    case MODE_WALK_TEST:
        icon = UI_ICON_FOOTPRINTS;
        tone = ui_tone_blue();
        /* TR: Home, the state card's headline. At most 47 bytes */
        i18n_utf8_copy(headline, sizeof(headline), TR("Walk test"));
        /* TR: Home, the line under the state card's headline. At most 127 bytes */
        i18n_utf8_copy(detail, sizeof(detail), TR("Open and close each zone"));
        /* TR: Home, the foot of the state card. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note), TR("The panel beeps for every zone it sees"));
        break;
    }

    hero_tone(tone, m == MODE_ENTRY || alarm_mode(m));
    ui_set_str(&subject_hero_icon, icon);
    ui_set_str(&subject_headline, headline);
    ui_set_str(&subject_detail, detail);
    ui_set_str(&subject_hero_note, note);
    const bool sounding = ps->sounder == ALARM_SOUND_ALARM || ps->sounder == ALARM_SOUND_FIRE ||
                          ps->sounder == ALARM_SOUND_CO || ps->sounder == ALARM_SOUND_AUX;
    ui_set_int(&subject_alarm_active, sounding ? 1 : 0);
    set_pulse(sounding);
    if (S.pad == PAD_DISARM) {
        ui_set_int(&subject_pad_silence,
                   (S.mode == MODE_FIRE || S.mode == MODE_CO || S.mode == MODE_AUX) ? 1 : 0);
    }
    if (S.pad == PAD_ARM && !ps->ready) {
        ui_set_int(&subject_opt_bypass_avail, 1);
    }
    update_panel();
}

/* ---- countdown, clock, network -------------------------------------------------------- */

static void update_countdown(void)
{
    const alarm_partition_status_t *ps = part();
    const bool delay = ps->state == ALARM_STATE_EXIT_DELAY || ps->state == ALARM_STATE_ENTRY_DELAY;
    if (!S.have_snap || !delay || ui_link_holds_home()) {
        S.count_total_ms = 0;
        ui_set_int(&subject_countdown, 0);
        ui_set_int(&subject_countdown_pct, 1000);
        return;
    }
    const alarm_ms_t now = S.be.now_ms(S.be.ctx);
    const alarm_ms_t elapsed = now > S.snap.taken_ms ? now - S.snap.taken_ms : 0u;
    const alarm_ms_t left =
        ps->delay_remaining_ms > elapsed ? ps->delay_remaining_ms - elapsed : 0u;
    if (left > S.count_total_ms) {
        S.count_total_ms = (uint32_t)left; /* start, or exit time restarted */
    }
    const int32_t secs = (int32_t)((left + 999u) / 1000u);
    const int32_t pct = S.count_total_ms ? (int32_t)(left * 1000u / S.count_total_ms) : 0;
    ui_set_int(&subject_countdown, secs);
    ui_set_int(&subject_countdown_pct, pct);
}

/* KPD-008: while this keypad is locked out the pad's message counts the time left, in whole
 * seconds (so the words change once a second, UI-020). The time left comes from the snapshot's
 * end and the UI's clock, as the delay countdown does; at 0 the pad's normal prompt returns
 * without a second snapshot. */
/* The number keys look disabled while the keypad is locked out (still, nothing blinks). */
static void pad_keys_dim(bool dim)
{
    static bool s_dim;
    if (dim == s_dim) {
        return;
    }
    s_dim = dim;
    for (size_t p = 0; p < sizeof(S.pads) / sizeof(S.pads[0]); p++) {
        lv_obj_t *keys = S.pads[p] != NULL ? lv_obj_find_by_name(S.pads[p], "pad_keys") : NULL;
        for (uint32_t i = 0; keys != NULL && i < lv_obj_get_child_count(keys); i++) {
            lv_obj_t *key = lv_obj_get_child(keys, (int32_t)i);
            lv_obj_t *label = lv_obj_get_child(key, 0);
            const char *text = label != NULL ? lv_label_get_text(label) : NULL;
            if (text != NULL && text[0] >= '0' && text[0] <= '9' && text[1] == '\0') {
                lv_obj_set_state(key, LV_STATE_DISABLED, dim);
            }
        }
    }
}

static void update_lockout(void)
{
    alarm_ms_t left = 0u;
    if (S.have_snap && S.pad != PAD_NONE && S.snap.local_lockout_until != 0u) {
        const alarm_ms_t now = S.be.now_ms(S.be.ctx);
        const alarm_ms_t elapsed = now > S.snap.taken_ms ? now - S.snap.taken_ms : 0u;
        const alarm_ms_t total = S.snap.local_lockout_until > S.snap.taken_ms
                                     ? S.snap.local_lockout_until - S.snap.taken_ms
                                     : 0u;
        left = total > elapsed ? total - elapsed : 0u;
    }
    const int32_t secs = (int32_t)((left + 999u) / 1000u);
    pad_keys_dim(secs != 0);
    if (secs == 0) {
        if (S.lock_shown_secs != 0) {
            S.lock_shown_secs = 0;
            if (S.pad != PAD_NONE) {
                pad_texts();
            }
        }
        return;
    }
    if (secs == S.lock_shown_secs && lv_subject_get_int(&subject_pad_error) != 0) {
        return;
    }
    char clock[16];
    char text[96];
    snprintf(clock, sizeof(clock), "%d:%02d", (int)(secs / 60), (int)(secs % 60));
    /* TR: the code pad's message while the keypad is locked out after wrong codes; {0} is the
     * time left as minutes and seconds, such as 4:59. At most 95 bytes */
    I18N_FORMAT(text, sizeof(text), TR("Keypad locked. Try again in {0}"), clock);
    S.lock_shown_secs = secs;
    ui_set_str(&subject_pad_hint, text);
    ui_set_int(&subject_pad_error, 1);
}

static void update_clock(bool force)
{
    const int64_t ms = S.be.epoch_ms(S.be.ctx);
    if (ms <= 0) {
        ui_set_str(&subject_time, "--:--");
        ui_set_str(&subject_ampm, "");
        /* TR: Home's status bar, where the date goes when the time is not known yet. At most
         * 127 bytes */
        ui_set_str(&subject_date, TR("Clock not set"));
        S.last_minute = -1;
        if (S.local_minute >= 0) {
            S.local_minute = -1;
            theme_follow_as(true); /* the time lost: Automatic can't tell night from day */
        }
        return;
    }
    struct tm tm;
    local_tm(ms, &tm);
    const int minute = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    if (!force && minute == S.last_minute) {
        return;
    }
    S.last_minute = minute;
    S.local_minute = tm.tm_hour * 60 + tm.tm_min;
    theme_follow_as(true); /* a new minute: the night hours may begin or end (Automatic) */
    char buf[40];
    clock_tm(&tm, buf, sizeof(buf), false);
    ui_set_str(&subject_time, buf);
    i18n_civil_t civil;
    civil_from_tm(&civil, &tm);
    char ampm[16] = "";
    if (!ui_clock_24h()) {
        i18n_date(ampm, sizeof(ampm), S.catalog, "a", &civil); /* the AM/PM word alone */
    }
    ui_set_str(&subject_ampm, ampm);
    /* TR: Home's status bar date, in CLDR pattern letters (EEEE weekday, MMMM month, d day). The
     * order may change. The result must fit 39 bytes */
    i18n_date_in(buf, sizeof(buf), S.catalog, ui_region(), TR_C("date pattern", "EEEE, MMMM d"),
                 &civil);
    ui_set_str(&subject_date, buf);
}

static void update_power(void)
{
    ui_power_info_t power = {.state = UI_BATTERY_UNKNOWN, .percent = -1};
    if (S.be.power_info) {
        S.be.power_info(S.be.ctx, &power);
    }
    const bool present = power.state == UI_BATTERY_PRESENT;
    const bool valid = present && power.percent >= 0 && power.percent <= 100;
    const char *icon = "\xEE\x81\x93"; /* Lucide battery, U+E053 */
    char text[12] = "";
    if (valid) {
        /* TR: the battery charge in the status bar; {0} is 0 to 100. At most 8 bytes */
        I18N_FORMAT(text, sizeof(text), TR_C("percent", "{0}%"), (unsigned)power.percent);
        icon = power.percent >= 75 ? "\xEE\x81\x95" :
               power.percent >= 35 ? "\xEE\x81\x97" : "\xEE\x81\x96";
    }
    if (valid && power.charging) {
        icon = "\xEE\x81\x94"; /* Lucide battery-charging */
    }
    ui_set_str(&subject_battery_icon, icon);
    ui_set_str(&subject_battery_text, text);
    ui_set_int(&subject_battery_critical, !valid);
    ui_set_int(&subject_battery_warn, valid && power.low);
}

static void update_net(void)
{
    int rssi = 0;
    const ui_net_state_t st =
        S.be.net_state ? S.be.net_state(S.be.ctx, &rssi) : UI_NET_UNCONFIGURED;
    const char *icon = UI_ICON_WIFI_OFF;
    int ok = 1;
    switch (st) {
    case UI_NET_CONNECTED:
        icon = rssi >= -60 ? UI_ICON_WIFI : rssi >= -70 ? UI_ICON_WIFI_HIGH
               : rssi >= -80 ? UI_ICON_WIFI_LOW : UI_ICON_WIFI_ZERO;
        break;
    case UI_NET_STARTING:
    case UI_NET_CONNECTING: icon = UI_ICON_WIFI_ZERO; break;
    case UI_NET_UNCONFIGURED: icon = UI_ICON_WIFI_OFF; break;
    case UI_NET_NO_RADIO:
    case UI_NET_DISCONNECTED:
        icon = UI_ICON_WIFI_OFF;
        ok = 0;
        break;
    }
    ui_set_str(&subject_wifi_icon, icon);
    ui_set_int(&subject_wifi_ok, ok);
}

/* ---- lists ---------------------------------------------------------------------------- */

/* An empty filter says so (BL-177): the green check and the filter's words are the layout's
 * (zones_none), shown instead of the list. A filter tap or the view opening shows the right one
 * at once. From a zone's change the swap is settled both ways (UI-020, UI-023): the check comes
 * only once the list has stayed empty for a second, and neither the check nor the list replaces
 * the other within a second of the last swap, so a chattering contact never pulses the big
 * check in and out. An open zone that ends the check shows within that second. */
static void zones_empty_tick(void)
{
    const bool shown = lv_subject_get_int(&subject_zones_empty) == 1;
    if (shown == S.zones_empty_want ||
        lv_tick_elaps(S.zones_empty_swapped) < ZONES_EMPTY_SETTLE_MS ||
        (S.zones_empty_want && lv_tick_elaps(S.zones_empty_since) < ZONES_EMPTY_SETTLE_MS)) {
        return;
    }
    ui_set_int(&subject_zones_empty, S.zones_empty_want ? 1 : 0);
    S.zones_empty_swapped = lv_tick_get();
}

static void zones_empty_set(bool want, int32_t filter)
{
    const bool tap = filter != S.zones_empty_filter; /* a filter tap, or the view opening */
    S.zones_empty_filter = filter;
    if (want != S.zones_empty_want) {
        S.zones_empty_want = want;
        S.zones_empty_since = lv_tick_get();
    }
    if (!tap) {
        zones_empty_tick();
    } else if ((lv_subject_get_int(&subject_zones_empty) == 1) != want) {
        ui_set_int(&subject_zones_empty, want ? 1 : 0);
        S.zones_empty_swapped = lv_tick_get();
    }
}

/* A zone card's words; on a keypad without the alarm panel every zone is Unknown, never the
 * last state it had as if it were now (BL-048, LINK-008). */
static void zone_line(uint16_t z, const alarm_zone_status_t *zs, ui_line_t *l)
{
    ui_zone_state(S.cfg, z, zs, l);
    if (ui_link_holds_home()) {
        l->tone = ui_tone_gray();
        /* TR: a zone card on a keypad that has lost the alarm panel: its state isn't known */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR_C("zone state", "Unknown"));
    }
}

static void zones_update(void)
{
    const bool disarmed = part()->state == ALARM_STATE_DISARMED;
    const int32_t filter = lv_subject_get_int(&subject_zone_filter);
    const uint16_t trouble_flags = ALARM_ZS_TROUBLE | ALARM_ZS_TAMPER | ALARM_ZS_LOW_BATTERY |
                                   ALARM_ZS_SUPERVISION;
    unsigned shown = 0u;
    for (uint16_t i = 0; i < S.zone_cards_n; i++) {
        const uint16_t z = S.zone_card_zone[i];
        lv_obj_t *card = S.zone_cards[i];
        const alarm_zone_status_t *zs = &S.snap.zone[z - 1u];
        ui_line_t l;
        zone_line(z, zs, &l);
        ui_chip_tone(ui_find(card, "chip"), l.glyph, l.tone);
        ui_set_label(card, "title_label", l.title);
        ui_set_label(card, "detail_label", l.detail);
        lv_obj_t *bypass = ui_find(card, "bypass");
        const bool can_bypass =
            disarmed && alarm_zone_bypassable(&S.cfg->zones[z - 1u]) && !ui_link_holds_home();
        lv_obj_set_flag(bypass, LV_OBJ_FLAG_HIDDEN, !can_bypass);
        /* TR: the button on a zone card in the Zones list */
        ui_set_label(bypass, "bypass_label", (zs->flags & ALARM_ZS_BYPASSED)
                                                 ? TR_C("zone card button", "Unbypass")
                                                 : TR_C("zone card button", "Bypass"));
        bool show = true;
        switch (filter) {
        case FILTER_OPEN: show = (zs->flags & ALARM_ZS_FAULT) != 0u; break;
        case FILTER_BYPASSED: show = (zs->flags & ALARM_ZS_BYPASSED) != 0u; break;
        case FILTER_TROUBLE: show = (zs->flags & trouble_flags) != 0u; break;
        default: break;
        }
        lv_obj_set_flag(card, LV_OBJ_FLAG_HIDDEN, !show);
        shown += show ? 1u : 0u;
    }
    zones_empty_set(filter != FILTER_ALL && shown == 0u, filter);
    /* The line above the Zones list: three counts, each its own plural, " · " between them. */
    const unsigned n_open = count_zones(ALARM_ZS_FAULT);
    const unsigned n_bypassed = count_zones(ALARM_ZS_BYPASSED);
    char zones[32];
    char opened[32];
    char skipped[32];
    /* TR: the Zones list, the first of the counts above it; {0} is up to 3 bytes. At most 28
     * bytes */
    I18N_FORMAT(zones, sizeof(zones), TRN("{0} zone", "{0} zones", S.zone_cards_n),
                (unsigned)S.zone_cards_n);
    /* TR: the Zones list, the counts above it: how many zones are open; {0} is up to 3 bytes.
     * At most 28 bytes */
    I18N_FORMAT(opened, sizeof(opened), TRN_C("zones", "{0} open", "{0} open", n_open), n_open);
    /* TR: the Zones list, the counts above it: how many zones are bypassed; {0} is up to 3
     * bytes. At most 28 bytes */
    I18N_FORMAT(skipped, sizeof(skipped),
                TRN_C("zones", "{0} bypassed", "{0} bypassed", n_bypassed), n_bypassed);
    char note[128]; /* a string subject holds 127 bytes */
    snprintf(note, sizeof(note), "%s · %s · %s", zones, opened, skipped);
    ui_set_str(&subject_zones_note, note);
}

static void zones_build(void)
{
    lv_obj_clean(S.zone_list);
    S.zone_cards_n = 0;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (!zone_in_partition(z)) {
            continue;
        }
        ui_line_t l;
        zone_line(z, &S.snap.zone[z - 1u], &l);
        lv_obj_t *card = zone_card_create(S.zone_list, l.glyph, l.title, l.detail);
        lv_obj_set_user_data(card, (void *)(uintptr_t)z);
        S.zone_cards[S.zone_cards_n] = card;
        S.zone_card_zone[S.zone_cards_n] = z;
        S.zone_cards_n++;
    }
    S.zones_cfg_seq = S.snap.config_seq;
    S.zones_built = true;
    zones_update();
}

/* ---- Activity: the whole event log (BL-131 finding 7) --------------------------------------- */

/* Newest first from the flash log (across restarts), or from the RAM ring when the platform has
 * no log; the filters (LOG-007), "Show older events" paging, and how much the log keeps.
 * Silent events (duress, silent panic) never show without a master or installer code (LOG-008):
 * the quiet "Show private events" row at the list's end asks for one, and what it opens is
 * forgotten on leaving Activity, at the screen saver, and when an arming starts. While armed or
 * in alarm the row is dimmed (UI-022). After the code, LOG-010's latched loss heads the list. */
#define ACTIVITY_PAGE 30u

static uint32_t s_act_before; /* the next older record to read; 0 = the log's start reached */
static uint32_t s_act_count;  /* events on the list */
static unsigned s_act_pages;  /* pages read: a rebuild reads as many again */
static int32_t s_act_filter;  /* subject_event_filter while reading: 0 = all, else kind + 1 */
static int s_act_day = -1;    /* the local day the times were written on */
static lv_obj_t *s_act_tail;  /* older events, how much the log keeps, the private step */

/* LOG-008: may the code shown here see this silent event? The system master and the installer
 * see every one; a partition master only those of their own partitions. Read from the
 * configuration each time, so a code demoted or deleted since shows nothing more. */
static bool private_authority(uint8_t user)
{
    if (user == 0u || user > ALARM_MAX_USERS) {
        return false;
    }
    const alarm_authority_t a = S.cfg->users[user - 1u].authority;
    return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_INSTALLER || a == ALARM_AUTH_PARTITION_MASTER;
}

static bool private_shown(void)
{
    return !S.locked && private_authority(s_priv_user);
}

static bool private_may_see(const alarm_event_t *ev)
{
    if (!private_shown()) {
        return false;
    }
    const alarm_user_t *u = &S.cfg->users[s_priv_user - 1u];
    if (u->authority != ALARM_AUTH_PARTITION_MASTER) {
        return true;
    }
    return ev->partition != 0u && ev->partition <= ALARM_MAX_PARTITIONS &&
           ((u->partitions >> (ev->partition - 1u)) & 1u) != 0u;
}

static bool activity_keep(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    if (e->kind != ELOG_KIND_EVENT ||
        (s_act_filter > 0 && (int32_t)ui_event_kind(e->ev.type) != s_act_filter - 1)) {
        return false;
    }
    return (e->ev.flags & ALARM_EVF_SILENT) == 0u || private_may_see(&e->ev);
}

static void activity_build(bool keep_scroll);

static void activity_more_click(lv_event_t *e)
{
    (void)e;
    s_act_pages++;
    activity_build(true);
}

static void activity_clear_loss_click(lv_event_t *e)
{
    (void)e;
    /* Only the installer code (docs/features/14); checked again here, not only when drawn. */
    if (private_shown() && S.cfg->users[s_priv_user - 1u].authority == ALARM_AUTH_INSTALLER &&
        S.be.log_clear_loss != NULL) {
        S.be.log_clear_loss(S.be.ctx);
    }
    activity_build(true); /* the screen shows the outcome: the warning goes, no toast */
}

static bool private_unlock(const char *code);

static void private_checked(const ui_call_t *c)
{
    if (!ui_pad_asks(private_unlock)) {
        return; /* the pad closed while the code was checked */
    }
    if (c->r != ALARM_OK) { /* counted towards the keypad lockout as any wrong code is */
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(c->r == ALARM_ERR_INVALID_CODE ? TR("Wrong code, try again")
                                                    : ui_result_text(c->r));
        return;
    }
    if (!private_authority(c->user)) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(TR("This code can't show private events"));
        return;
    }
    if (S.locked) {
        ui_pad_cancel(); /* armed while the code was typed: the pad closes, nothing opens */
        return;
    }
    s_priv_user = c->user;
    ui_pad_cancel();
    activity_build(true);
}

/* The pad stays open; the answer closes it (private_checked). */
static bool private_unlock(const char *code)
{
    if (!ui_call_authenticate(code, private_checked, 0u)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

static void activity_private_click(lv_event_t *e)
{
    (void)e;
    if (S.locked) {
        return;
    }
    if (s_priv_user != 0u) {
        s_priv_user = 0;
        activity_build(true);
        return;
    }
    /* TR: Activity, the title of the code pad that shows the private (silent) events. At most
     * 31 bytes */
    const char *title = TR("Show private events");
    ui_pad_ask(HOST_MODAL, UI_ICON_KEY_ROUND, title, TR("Enter a master or installer code"),
               ui_tone_blue(), private_unlock);
}

/* The private step is forgotten: on leaving Activity, at the screen saver, when an arming starts. */
static void activity_forget(void)
{
    if (S.pad == PAD_ASK && S.pad_fn == private_unlock) {
        ui_pad_cancel();
    }
    if (s_priv_user == 0u) {
        return;
    }
    s_priv_user = 0;
    if (S.have_snap && lv_subject_get_int(&subject_view) == VIEW_ACTIVITY) {
        activity_build(true);
    }
}

/* LOG-010: what the log could not keep, steady at the top until the installer clears it. */
static void activity_loss_rows(void)
{
    uint32_t lost = 0;
    uint32_t lost_reports = 0;
    if (S.be.log_loss == NULL) {
        return;
    }
    S.be.log_loss(S.be.ctx, &lost, &lost_reports);
    if (lost == 0u && lost_reports == 0u) {
        return;
    }
    char title[64];
    char detail[64];
    detail[0] = '\0';
    title[0] = '\0';
    if (lost != 0u) {
        /* TR: Activity, a warning row at the top: how many events the log could not save.
         * At most 63 bytes */
        I18N_FORMAT(title, sizeof(title), TRN("{0} event couldn't be saved",
                                              "{0} events couldn't be saved", lost),
                    (unsigned)lost);
    }
    if (lost_reports != 0u) {
        /* TR: Activity, a warning row at the top: how many alerts could not be kept to send
         * again later. At most 63 bytes */
        I18N_FORMAT(lost != 0u ? detail : title, sizeof(title),
                    TRN("{0} alert couldn't be kept", "{0} alerts couldn't be kept",
                        lost_reports),
                    (unsigned)lost_reports);
    }
    ui_row_link(S.event_list, UI_ICON_TRIANGLE_ALERT, ui_tone_amber(), title, detail, NULL, NULL,
                NULL, NULL);
    if (S.be.log_clear_loss == NULL) {
        return;
    }
    if (S.cfg->users[s_priv_user - 1u].authority == ALARM_AUTH_INSTALLER) {
        /* TR: Activity, the button that clears the warning above */
        lv_obj_t *b = row_button_create(S.event_list, UI_ICON_CHECK, TR("Clear the warning"),
                                        ui_tone_blue());
        lv_obj_add_event_cb(b, activity_clear_loss_click, LV_EVENT_CLICKED, NULL);
    } else {
        /* TR: Activity, under the warning: who can clear it. At most 127 bytes */
        ui_paragraph(S.event_list, TR("Only the installer code clears this warning."),
                     ui_tone_gray());
    }
}

static void activity_add(const elog_entry_t *e)
{
    ui_line_t l;
    if (!ui_event_line(S.cfg, &e->ev, private_may_see(&e->ev), &l)) {
        return;
    }
    char when[48];
    ui_entry_time(e, when, sizeof(when));
    lv_obj_t *row = event_row_create(S.event_list, l.glyph, l.title, l.detail, when);
    ui_chip_tone(ui_find(row, "chip"), l.glyph, l.tone);
    s_act_count++;
}

/* One page of older events; false when the platform has no log to page through. */
static bool activity_page(elog_entry_t *entries)
{
    if (S.be.log_read == NULL) {
        return false;
    }
    uint32_t next = 0;
    size_t n = 0;
    /* A sparse filter may need several scans; stop once a page is full or the log ends. */
    for (unsigned tries = 0; tries < 8u && n < ACTIVITY_PAGE && s_act_before != 0u; tries++) {
        n += S.be.log_read(S.be.ctx, s_act_before, activity_keep, NULL, entries + n,
                           ACTIVITY_PAGE - n, 512, &next);
        s_act_before = next;
    }
    for (size_t i = 0; i < n; i++) {
        activity_add(&entries[i]);
    }
    return true;
}

/* The platform without a flash log: the RAM ring, newest first, one page. */
static void activity_ring(elog_entry_t *entries)
{
    static alarm_event_t *evs;
    if (S.be.recent_events == NULL ||
        (evs == NULL && (evs = ui_slow_alloc(UI_MAX_EVENT_ROWS * sizeof(*evs))) == NULL)) {
        return;
    }
    const size_t n = S.be.recent_events(S.be.ctx, evs, UI_MAX_EVENT_ROWS);
    const uint32_t boot = S.be.boot_id != NULL ? S.be.boot_id(S.be.ctx) : 0u;
    for (size_t i = 0; i < n; i++) {
        elog_entry_t *e = &entries[0];
        memset(e, 0, sizeof(*e));
        e->kind = ELOG_KIND_EVENT;
        e->boot = boot;
        e->uptime_s = (uint32_t)(evs[i].mono_ms / 1000u);
        e->ev = evs[i];
        if (activity_keep(NULL, e)) {
            activity_add(e);
        }
    }
}

static int activity_today(void)
{
    const int64_t now = S.be.epoch_ms(S.be.ctx);
    if (now <= 0) {
        return -1;
    }
    struct tm tm;
    local_tm(now, &tm);
    return tm.tm_year * 400 + tm.tm_yday;
}

static void activity_tail(void)
{
    s_act_tail = lv_obj_create(S.event_list);
    lv_obj_remove_style_all(s_act_tail);
    lv_obj_set_size(s_act_tail, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_act_tail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_act_tail, SPACE_SM, 0);
    lv_obj_remove_flag(s_act_tail, LV_OBJ_FLAG_SCROLLABLE);
    if (s_act_before != 0u && S.be.log_read != NULL) {
        /* TR: Activity, the button at the end of the list that loads earlier events */
        lv_obj_t *b = row_button_create(s_act_tail, UI_ICON_HASH, TR("Show older events"),
                                        ui_tone_blue());
        lv_obj_add_event_cb(b, activity_more_click, LV_EVENT_CLICKED, NULL);
    } else if (s_act_count == 0u && s_act_filter > 0) {
        /* TR: Activity, a filter chose a kind of event the log holds none of */
        ui_paragraph(s_act_tail, TR("Nothing of this kind in the log."), ui_tone_gray());
    }
    ui_about_t about;
    memset(&about, 0, sizeof(about));
    if (S.be.about != NULL) {
        S.be.about(S.be.ctx, &about);
    }
    if (about.log_capacity > 0u) {
        /* How much the log keeps, never how much it holds: a count that included the silent
         * events would hint at them (LOG-008). */
        char kept[64];
        /* TR: Activity, at the end of the list: how many events the log keeps at least. At most
         * 63 bytes */
        I18N_FORMAT(kept, sizeof(kept), TR("Keeps the last {0} or more"),
                    (unsigned)about.log_capacity);
        ui_paragraph(s_act_tail, kept, ui_tone_gray());
    }
    /* Always there, whether or not there are silent events, so it hints at nothing. */
    lv_obj_t *row;
    if (S.locked) {
        /* TR: Activity, the row at the end of the list that asks for a code to show the private
         * (silent) events. At most 63 bytes */
        row = ui_row_link(s_act_tail, UI_ICON_KEY_ROUND, ui_tone_gray(), TR("Show private events"),
                          /* TR: Activity, under Show private events while armed or in alarm:
                           * the row works again once every partition is disarmed. At most 63
                           * bytes */
                          TR("Once the system is disarmed"), NULL, NULL, NULL, NULL);
        ui_row_disable(row);
        return;
    }
    if (private_shown()) {
        /* TR: Activity, the row at the end of the list that hides the private events again.
         * At most 63 bytes */
        row = ui_row_link(s_act_tail, UI_ICON_EYE_OFF, ui_tone_gray(), TR("Hide private events"),
                          NULL, NULL, NULL, NULL, NULL);
    } else {
        row = ui_row_link(s_act_tail, UI_ICON_KEY_ROUND, ui_tone_gray(), TR("Show private events"),
                          NULL, NULL, NULL, NULL, NULL);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, activity_private_click, LV_EVENT_CLICKED, NULL);
}

/* Draws the list again from the newest event, as many pages as were read. keep_scroll: it stays
 * where the person scrolled it (a new event, a page more, the private step). */
static void activity_build(bool keep_scroll)
{
    static elog_entry_t *entries;
    if (entries == NULL &&
        (entries = ui_slow_alloc(ACTIVITY_PAGE * sizeof(*entries))) == NULL) {
        return;
    }
    const int32_t y = keep_scroll ? lv_obj_get_scroll_y(S.event_list) : 0;
    lv_obj_clean(S.event_list);
    s_act_tail = NULL;
    s_act_count = 0;
    s_act_before = UINT32_MAX;
    s_act_filter = lv_subject_get_int(&subject_event_filter);
    s_act_day = activity_today();
    if (s_act_pages == 0u) {
        s_act_pages = 1;
    }
    const bool shown = private_shown();
    if (shown) {
        activity_loss_rows();
    }
    if (S.be.log_read != NULL) {
        for (unsigned p = 0; p < s_act_pages && s_act_before != 0u; p++) {
            activity_page(entries);
        }
    } else {
        activity_ring(entries);
        s_act_before = 0;
    }
    activity_tail();
    if (shown) {
        /* TR: Activity's note under its title once a master or installer code shows the private
         * (silent) events too. At most 127 bytes */
        ui_set_str(&subject_events_note, TR("Newest first · private events shown"));
    } else if (s_act_count == 0u && s_act_filter == 0) {
        /* TR: Activity's note under its title when the log holds no event yet. At most 127
         * bytes */
        ui_set_str(&subject_events_note, TR("Nothing has happened yet"));
    } else {
        /* TR: Activity's note under its title: the list is the whole event log. At most 127
         * bytes */
        ui_set_str(&subject_events_note, TR("Everything the panel did, newest first"));
    }
    lv_obj_update_layout(S.event_list);
    lv_obj_scroll_to_y(S.event_list, y, LV_ANIM_OFF);
    S.events_shown_seq = S.snap.last_event_seq;
}

static void event_filter_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    if (S.have_snap && lv_subject_get_int(&subject_view) == VIEW_ACTIVITY) {
        s_act_pages = 1;
        activity_build(false);
    }
}

/* The next line of a walk, or NULL when this one is before the page or past its end. Counting
 * carries on either way, so the caller always learns the true total. */
static ui_line_t *trouble_slot(size_t *seen, size_t skip, ui_line_t *out, size_t max,
                               size_t *filled)
{
    ui_line_t *l = NULL;
    if (*seen >= skip && *filled < max) {
        l = &out[(*filled)++];
        l->link = false;
        l->link_zw = false;
    }
    (*seen)++;
    return l;
}

/* The sensor radio trouble is one for every wireless source, so its rows are told apart by the
 * radios' own status: Zigbee's (BL-144 page 13) and Z-Wave's (BL-152, ZW-019) each open their own
 * page. With both radios in trouble each gets a row, titled for its radio; with one, Zigbee's
 * keeps the shared title. Counts the rows when `out` is NULL. */
static void sensor_radio_rows(size_t *seen, size_t skip, ui_line_t *out, size_t max,
                              size_t *filled)
{
    ui_line_t base;
    ui_line_t zw;
    bool zb = false;
    bool has_zw = false;
    memset(&base, 0, sizeof(base));
    memset(&zw, 0, sizeof(zw));
    ui_sys_trouble_line(ALARM_TRBL_SENSOR_RADIO, false, &base);
#if ALARM_UI_ZIGBEE
    zb = ui_zigbee_trouble_line(ALARM_TRBL_SENSOR_RADIO, &base);
    base.link = zb;
#endif
#if ALARM_UI_ZWAVE
    has_zw = ui_zwave_trouble_line(ALARM_TRBL_SENSOR_RADIO, &zw);
#endif
    if (zb && has_zw) {
        /* TR: the title of the sensor radio trouble's row when both radios have the problem */
        i18n_utf8_copy(base.title, sizeof(base.title), TR("Zigbee radio problem"));
    }
    if (has_zw && !zb) { /* Z-Wave's row alone */
        ui_line_t *l = trouble_slot(seen, skip, out, max, filled);
        if (l != NULL) {
            *l = base;
            i18n_utf8_copy(l->title, sizeof(l->title), zw.title);
            i18n_utf8_copy(l->detail, sizeof(l->detail), zw.detail);
            l->link = true;
            l->link_zw = true;
        }
        return;
    }
    ui_line_t *l = trouble_slot(seen, skip, out, max, filled);
    if (l != NULL) {
        *l = base;
    }
    if (has_zw) { /* both: Z-Wave's second */
        l = trouble_slot(seen, skip, out, max, filled);
        if (l != NULL) {
            *l = base;
            i18n_utf8_copy(l->title, sizeof(l->title), zw.title);
            i18n_utf8_copy(l->detail, sizeof(l->detail), zw.detail);
            l->link = true;
            l->link_zw = true;
        }
    }
}

/* The rows of one line of the shared walk: one row, or the sensor radio's one or two. */
static void trouble_rows(const alarm_trouble_item_t *it, size_t *seen, size_t skip,
                         ui_line_t *out, size_t max, size_t *filled)
{
    if (it->family == ALARM_TRBL_FAMILY_SYSTEM && !it->restored &&
        it->kind == ALARM_TRBL_SENSOR_RADIO) {
        sensor_radio_rows(seen, skip, out, max, filled);
        return;
    }
    ui_line_t *l = trouble_slot(seen, skip, out, max, filled);
    if (l == NULL) {
        return;
    }
    if (it->family == ALARM_TRBL_FAMILY_SYSTEM) {
        ui_sys_trouble_line((alarm_sys_trouble_t)it->kind, it->restored, l);
    } else if (it->family == ALARM_TRBL_FAMILY_DEVICE) {
        l->glyph = UI_ICON_RADIO_TOWER;
        l->tone = ui_tone_amber();
        /* TR: a row's title in the Troubles list; {0} is the keypad's number */
        I18N_FORMAT(l->title, sizeof(l->title), TR("Keypad {0} not responding"), it->number);
        /* TR: the line under a row's title in the Troubles list. At most 95 bytes */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Check its power and signal"));
    } else {
        ui_zone_trouble_line(S.cfg, it->number, (alarm_zone_trouble_t)it->kind, it->restored, l);
    }
}

/* Walks every trouble the snapshot holds, as the Panel Manager does (alarm_snapshot_troubles,
 * BL-140): the system's, then the keypads', then the zones' by number, in an order that doesn't
 * move about. Active faults and restored memories are walked separately so the active ones always
 * come first and old history can never push a fault off the screen (UI-030). Returns how many
 * rows of that kind there are - all of them, not just the ones that fitted - and fills rows
 * [skip, skip + max) of them. out may be NULL with max 0: then it only counts. */
static size_t troubles_walk(bool restored, size_t skip, ui_line_t *out, size_t max,
                            size_t *filled)
{
    alarm_trouble_item_t items[16];
    const size_t step = sizeof(items) / sizeof(items[0]);
    size_t seen = 0;
    size_t total = 1;
    for (size_t first = 0; first < total; first += step) {
        total = alarm_snapshot_troubles_from(&S.snap, S.cfg, 0xFFu, first, items, step);
        for (size_t i = 0; i < step && first + i < total; i++) {
            if (items[i].restored == restored) {
                trouble_rows(&items[i], &seen, skip, out, max, filled);
            }
        }
    }
    return seen;
}

/* How many troubles there are altogether, active and restored. */
static void troubles_total(size_t *active, size_t *restored)
{
    size_t none = 0;
    *active = troubles_walk(false, 0, NULL, 0, &none);
    *restored = troubles_walk(true, 0, NULL, 0, &none);
}

static size_t troubles_pages(size_t total)
{
    return total == 0u ? 1u : (total + UI_MAX_TROUBLE_ROWS - 1u) / UI_MAX_TROUBLE_ROWS;
}

static void troubles_update(void)
{
    static ui_line_t *lines; /* ~8 KB: off the LVGL task stack */
    if (lines == NULL && (lines = ui_slow_alloc(UI_MAX_TROUBLE_ROWS * sizeof(*lines))) == NULL) {
        return;
    }
    size_t active = 0;
    size_t restored = 0;
    troubles_total(&active, &restored);
    const size_t total = active + restored;
    const size_t pages = troubles_pages(total);
    if ((size_t)S.trouble_page >= pages) {
        S.trouble_page = (uint8_t)(pages - 1u); /* troubles cleared under the page we were on */
    }
    const size_t first = (size_t)S.trouble_page * UI_MAX_TROUBLE_ROWS;
    size_t n = 0;
    troubles_walk(false, first, lines, UI_MAX_TROUBLE_ROWS, &n);
    troubles_walk(true, first > active ? first - active : 0u, lines, UI_MAX_TROUBLE_ROWS, &n);

    for (size_t i = 0; i < n; i++) {
        lv_obj_t *row = S.trouble_rows[i];
        if (row == NULL) {
            row = trouble_row_create(S.trouble_list, lines[i].glyph, lines[i].title,
                                     lines[i].detail);
            S.trouble_rows[i] = row;
        } else {
            ui_set_label(row, "title_label", lines[i].title);
            ui_set_label(row, "detail_label", lines[i].detail);
        }
        ui_chip_tone(ui_find(row, "chip"), lines[i].glyph, lines[i].tone);
#if ALARM_UI_ZIGBEE
        ui_zigbee_trouble_link(row, lines[i].link && !lines[i].link_zw);
#endif
#if ALARM_UI_ZWAVE
        ui_zwave_trouble_link(row, lines[i].link && lines[i].link_zw);
#endif
        lv_obj_set_flag(row, LV_OBJ_FLAG_HIDDEN, false);
    }
    for (size_t i = n; i < UI_MAX_TROUBLE_ROWS && S.trouble_rows[i] != NULL; i++) {
        lv_obj_set_flag(S.trouble_rows[i], LV_OBJ_FLAG_HIDDEN, true);
    }
    ui_set_int(&subject_troubles_empty, total == 0u ? 1 : 0);
    lv_obj_set_flag(S.trouble_list, LV_OBJ_FLAG_HIDDEN, n == 0u);

    /* The pager only appears when there is something it can reach. */
    ui_set_int(&subject_troubles_pager, pages > 1u ? 1 : 0);
    char page[48];
    /* TR: the Troubles list's pager: rows {0} to {1} of {2} troubles (each up to 4 bytes). At
     * most 35 bytes */
    I18N_FORMAT(page, sizeof(page), TRN("{0}-{1} of {2}", "{0}-{1} of {2}", total),
                (unsigned)(first + 1u), (unsigned)(first + n), (unsigned)total);
    ui_set_str(&subject_troubles_page, page);

    /* The count is of everything, whether it is on this page or not. */
    char note[128]; /* a string subject holds 127 bytes */
    char now[40];
    /* TR: the Troubles list, the line above it: how many troubles are there now; {0} is up to
     * 4 bytes. At most 35 bytes */
    I18N_FORMAT(now, sizeof(now), TRN_C("troubles", "{0} active", "{0} active", active),
                (unsigned)active);
    if (total == 0u) {
        /* TR: the Troubles list, the line above it when there is none. At most 127 bytes */
        i18n_utf8_copy(note, sizeof(note), TR("Everything is working"));
    } else if (restored == 0u) {
        i18n_utf8_copy(note, sizeof(note), now);
    } else {
        char was[80];
        /* TR: the Troubles list, the line above it, after "{0} active · ": troubles that went
         * away but were not acknowledged yet; {0} is up to 4 bytes. At most 75 bytes */
        I18N_FORMAT(was, sizeof(was),
                    TRN_C("troubles", "{0} restored, waiting for acknowledgement",
                          "{0} restored, waiting for acknowledgement", restored),
                    (unsigned)restored);
        snprintf(note, sizeof(note), "%s · %s", now, was);
    }
    ui_set_str(&subject_troubles_note, note);
}

/* The pager's two keys. Both wrap round, so neither is ever a dead control. */
static void troubles_page_turn(const char *which)
{
    size_t active = 0;
    size_t restored = 0;
    troubles_total(&active, &restored);
    const size_t pages = troubles_pages(active + restored);
    const size_t page = S.trouble_page < pages ? S.trouble_page : pages - 1u;
    const bool back = which != NULL && strcmp(which, "back") == 0;
    S.trouble_page = (uint8_t)(back ? (page == 0u ? pages - 1u : page - 1u)
                                    : (page + 1u >= pages ? 0u : page + 1u));
    troubles_update();
    lv_obj_scroll_to_y(S.trouble_list, 0, LV_ANIM_OFF); /* a new page starts at its top */
}

static void update_lists(void)
{
    switch (lv_subject_get_int(&subject_view)) {
    case VIEW_ZONES:
        if (!S.zones_built || S.zones_cfg_seq != S.snap.config_seq) {
            zones_build();
        } else {
            zones_update();
        }
        break;
    case VIEW_TROUBLES: troubles_update(); break;
    default: break;
    }
}

/* ---- locked while armed (UI-022) ------------------------------------------------------ */

/* From exit time on, and during an alarm, the panel is locked down: Settings, chime, codes and
 * acknowledging troubles wait until every partition is disarmed and quiet. Disarming, the lists
 * and the panic buttons stay. */
static bool any_armed(void)
{
    return alarm_snapshot_any_armed(&S.snap);
}

/* The same rule the Panel Manager applies to every change (alarm_snapshot_locked). */
static bool compute_locked(void)
{
    return alarm_snapshot_locked(&S.snap);
}

/* Locked, but Settings opens all the same: a keypad that can't show the alarm panel's state, whose
 * last status said armed or in alarm, with a page it needs without a code (LINK-025's maintenance,
 * an update it needs). The rest of the lock stands; Settings closes once this ends while locked. */
static bool s_lock_waived;

/* A locked control says why: one whole sentence for "armed" and one for "in alarm". */
static void locked_toast(const char *when_disarmed, const char *when_silenced)
{
    ui_toast(any_armed() ? when_disarmed : when_silenced);
}

/* The panel was just armed (here, remotely, by a keyfob) or an alarm started: close whatever
 * the lock covers. Settings closing forgets the code typed there. */
static void lock_down(void)
{
    activity_forget(); /* an arming hides the private events again (LOG-008) */
    if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS) {
        ui_set_int(&subject_view, VIEW_HOME);
    }
    ui_sheet_close();
    if (S.pad != PAD_NONE && (S.pad_host != HOST_HOME || S.pad != PAD_DISARM)) {
        pad_close(); /* arming, chime, bypass, troubles, codes; disarming stays */
    }
    if (S.wizard_on && !S.wizard_paused) {
        ui_wizard_pause(); /* resumes once disarmed */
    }
}

/* ---- snapshot ------------------------------------------------------------------------- */

void ui_partition_view(uint8_t partition)
{
    if (partition < 1u || partition > ALARM_MAX_PARTITIONS || partition == S.opt.partition) return;
    ui_pad_cancel();
    ui_sheet_close();
    activity_forget();
    S.opt.partition = partition; /* transient: never save it as the default */
    ui_refresh(true);
    if (lv_subject_get_int(&subject_view) == VIEW_ACTIVITY) activity_build(false);
}

void ui_refresh(bool force)
{
    const uint32_t seq = S.be.snapshot_seq(S.be.ctx);
    if (!force && S.have_snap && seq == S.snap_seq) {
        return;
    }
    S.be.get_snapshot(S.be.ctx, &S.snap);
    S.snap_seq = S.snap.seq;
    S.have_snap = true;
    if (S.snap.config_seq != S.cfg_seq) {
        if (S.be.get_config(S.be.ctx, S.cfg)) {
            S.cfg_seq = S.snap.config_seq;
        }
    }
    const alarm_partition_status_t *ps = part();
    ui_set_int(&subject_open_count, (int32_t)count_zones(ALARM_ZS_FAULT));
    /* The status bar's pill and the screen saver count everything the Troubles list holds:
     * active faults, and restored ones not acknowledged yet (UI-030). The pill is the only way
     * to the list, so nothing in it may go uncounted. */
    size_t active = 0;
    size_t restored = 0;
    troubles_total(&active, &restored);
    const unsigned troubles = (unsigned)(active + restored);
    ui_set_int(&subject_trouble_count, (int32_t)troubles);
    char trouble_text[24];
    /* TR: the screen saver, how many troubles there are; {0} is up to 4 bytes. At most 19
     * bytes */
    I18N_FORMAT(trouble_text, sizeof(trouble_text), TRN("{0} trouble", "{0} troubles", troubles),
                troubles);
    ui_set_str(&subject_trouble_text, trouble_text);
    ui_set_int(&subject_chime, ps->chime ? 1 : 0);
    ui_set_str(&subject_chime_icon, ps->chime ? UI_ICON_BELL : UI_ICON_BELL_OFF);
    ui_set_int(&subject_default_codes, S.snap.default_codes);
    const bool locked = compute_locked();
    const bool waived = locked && ui_link_settings_waived();
    ui_set_int(&subject_locked, locked ? 1 : 0); /* chime, Acknowledge: still locked */
    ui_set_int(&subject_settings_locked, locked && !waived ? 1 : 0); /* the gear as it works */
    bar_banner(locked);
    if (!S.snap.program && S.program) {
        S.program = false; /* timed out or left elsewhere */
        ui_settings_refresh();
    }
    apply_mode();
    if (locked && (!S.locked || (s_lock_waived && !waived))) {
        lock_down();
    }
    s_lock_waived = waived;
    if (locked != S.locked) {
        S.events_shown_seq = UINT32_MAX; /* Activity's private row dims or wakes (UI-022) */
    }
    S.locked = locked;
    update_countdown();
    ui_walk_home();
    update_lists();
    ui_settings_refresh();
}

/* ---- the remote keypad (docs/features/31) -------------------------------------------------- */

/* The status bar's left side: a remote session first (it has to be seen), then installer mode,
 * then the walk test (docs/features/40), then an automation's arming warning (docs/features/41
 * AUT-012; never while an alarm sounds: the alarm has the screen), then an update that runs
 * (BL-188; the same), then the factory-code reminder, else the date. */
/* The installer banner's words: where installer mode is held when that isn't this screen
 * (BL-223, LINK-060). */
static void program_words(void)
{
    char text[96];
    const uint8_t holder = ui_link_remote_holder();
    if (ui_link_keypad() && !ui_link_remote_installer() && holder == UI_LINK_HOLDER_PANEL) {
        /* TR: a keypad's status bar while its alarm panel's own screen or Panel Manager holds
         * installer mode; {0} is the alarm panel's name. At most 95 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Installer on {0} · Arming off"), ui_link_master_name());
    } else if (ui_link_keypad() && !ui_link_remote_installer() && holder != UI_LINK_HOLDER_NONE) {
        /* TR: a keypad's status bar while another keypad holds installer mode; {0} is its
         * number. At most 95 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Installer at Keypad {0} · Arming off"),
                    (unsigned)holder);
    } else if (ui_link_master() && !S.program && holder >= 1u && holder <= UI_LINK_KEYPADS_MAX) {
        /* TR: the alarm panel's status bar while a keypad holds its installer mode; {0} is the
         * keypad's number. At most 95 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Installer at Keypad {0} · Arming off · Tap to end"),
                    (unsigned)holder);
    } else {
        i18n_utf8_copy(text, sizeof(text), TR("Installer · Arming off · Tap to leave"));
    }
    if (strcmp(lv_subject_get_string(&subject_program_text), text) != 0) {
        ui_set_str(&subject_program_text, text);
    }
}

static void bar_banner(bool locked)
{
    program_words();
    const bool walk = ui_walk_banner();
    const bool quiet = !alarm_mode(compute_mode(part())); /* an alarm has the screen */
    const bool arming = ui_auto_warn_banner() && quiet;
    const uint8_t update = quiet ? ui_update_banner() : 0u; /* BL-188: 1 blue, 2 amber */
    ui_set_int(&subject_bar_banner,
               S.remote.active  ? 3
               : S.snap.program ? 2
               : walk           ? 4
               : arming         ? 5
               : update != 0u   ? (int32_t)(5u + update)
               : (S.snap.default_codes != 0u && !locked && !ui_link_holds_home())
                   ? (ui_link_keypad() ? 8 : 1) /* a keypad's: BL-048 */
                   : 0);
}

void ui_bar_banner_update(void)
{
    bar_banner(S.locked);
}

/* The left side (the date or a banner) takes the room the right side leaves, and its words end
 * with "..." where that runs out: the trouble pill, the icons, the clock, Chime and the gear
 * stay on the screen whatever the language, the count or the time (I18N-022). The right side
 * is everything after the bar's filler. Runs after each layout of the bar; a new width lays it
 * out once more, which finds nothing to change. */
static void bar_fit(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_current_target_obj(e);
    lv_obj_t *left = NULL;
    bool right = false;
    int32_t used = 0;
    int32_t shown = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *c = lv_obj_get_child(bar, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        shown++;
        if (lv_obj_get_style_flex_grow(c, LV_PART_MAIN) > 0) {
            right = true;
        } else if (right) {
            used += lv_obj_get_width(c);
        } else {
            left = c;
        }
    }
    if (left == NULL || !right) {
        return;
    }
    lv_obj_t *text = lv_obj_check_type(left, &lv_label_class) ? left : lv_obj_get_child(left, -1);
    if (text == NULL || !lv_obj_check_type(text, &lv_label_class)) {
        return;
    }
    /* what the banner draws around its words: padding, the icon and the gap beside it */
    const int32_t around = lv_obj_get_width(left) - lv_obj_get_width(text);
    const int32_t room = lv_obj_get_content_width(bar) - used - around -
                         (shown - 1) * lv_obj_get_style_pad_column(bar, LV_PART_MAIN);
    const int32_t fit = LV_MAX(room, 0);
    if (lv_obj_get_style_max_width(text, LV_PART_MAIN) != fit) {
        lv_obj_set_style_max_width(text, fit, 0);
    }
}

/* The arming options take two rows at most in English; where a language's words put each chip on
 * a row of its own, the number keys get shorter (pad_key's user_2 state) so the title and "Enter
 * your code" keep their place above them (I18N-022). Runs after each layout of a code pad; the
 * keys' new height moves no chip, so the next layout finds nothing to change. */
static void pad_fit(lv_event_t *e)
{
    lv_obj_t *pad = lv_event_get_current_target_obj(e);
    lv_obj_t *opts = lv_obj_find_by_name(pad, "pad_options");
    lv_obj_t *keys = lv_obj_find_by_name(pad, "pad_keys");
    if (opts == NULL || keys == NULL) {
        return;
    }
    uint32_t rows = 0;
    if (!lv_obj_has_flag(opts, LV_OBJ_FLAG_HIDDEN)) {
        int32_t row_y = INT32_MIN;
        for (uint32_t i = 0; i < lv_obj_get_child_count(opts); i++) {
            lv_obj_t *chip = lv_obj_get_child(opts, (int32_t)i);
            if (!lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_y(chip) != row_y) {
                row_y = lv_obj_get_y(chip);
                rows++;
            }
        }
    }
    const bool short_keys = rows > 2u;
    for (uint32_t i = 0; i < lv_obj_get_child_count(keys); i++) {
        lv_obj_t *key = lv_obj_get_child(keys, (int32_t)i);
        if (lv_obj_has_state(key, LV_STATE_USER_2) != short_keys) {
            lv_obj_set_state(key, LV_STATE_USER_2, short_keys);
        }
    }
}

static void remote_banner_text(void)
{
    char text[64];
    if (S.remote_paused) {
        /* TR: the status bar while a remote keypad session is paused. At most 63 bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Remote keypad paused · Tap for choices"));
    } else if (S.remote.who[0] != '\0') {
        char who[25]; /* the name, cut to 24 bytes */
        i18n_utf8_copy(who, sizeof(who), S.remote.who);
        /* TR: the status bar during a remote keypad session; {0} is who, a name cut to 24
         * bytes. At most 39 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Remote: {0} · Tap to pause"), who);
    } else {
        /* TR: the status bar during a remote keypad session whose user is unknown. At most 63
         * bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Remote: someone · Tap to pause"));
    }
    ui_set_str(&subject_remote_banner, text);
}

static void remote_pause(bool paused)
{
    S.remote_paused = paused;
    if (S.be.remote_pause != NULL) {
        S.be.remote_pause(S.be.ctx, paused);
    }
    remote_banner_text();
}

static void remote_allowed(void)
{
    remote_pause(false);
}

/* Turned off: the session stays paused until the server closing ends it a moment later, so the
 * remote never gets the screen back in between. Failed: still paused, the banner asks again. */
static void remote_turned_off(void)
{
    const bool off = S.be.remote_set != NULL && S.be.remote_set(S.be.ctx, false, false);
    /* TR: a toast; the last "Installer" is the Settings section of that name */
    ui_toast(off ? TR("Remote access is off. The installer turns it back on in Installer")
                 : TR("Remote access could not be turned off. Try again"));
}

static void remote_question(void)
{
    char note[128];
    char who[21];   /* the name, cut to 20 bytes */
    char where[31]; /* the address, cut to 30 bytes */
    i18n_utf8_copy(who, sizeof(who), S.remote.who);
    i18n_utf8_copy(where, sizeof(where), S.remote.where);
    if (where[0] != '\0') {
        /* TR: the question over the screen about a remote keypad session; {0} is who, cut to
         * 20 bytes, {1} the address it comes from, cut to 30. At most 77 bytes */
        I18N_FORMAT(note, sizeof(note),
                    TR("{0}, from {1}. Not expected? Turn it off: only the installer can turn it "
                       "on again."),
                    who, where);
    } else {
        /* TR: the question over the screen about a remote keypad session; {0} is who, cut to
         * 20 bytes. At most 107 bytes */
        I18N_FORMAT(note, sizeof(note),
                    TR("{0}. Not expected? Turn it off: only the installer can turn it on again."),
                    who);
    }
    /* TR: the question over the screen about a remote keypad session: its title and its two
     * buttons, the second leaves the session running */
    ui_confirm_choice(TR("Someone is using this keypad remotely"), note,
                      TR("Turn off remote access"), remote_turned_off,
                      TR_C("remote access", "Allow"), remote_allowed);
}

/* Only the person at the panel pauses the remote, allows it or turns it off: a press arriving
 * from the network on the banner does nothing. Pausing takes back whatever the remote had open
 * (its code pad, a half-typed code, a sheet) before asking. */
static void remote_banner_pressed(void)
{
    if (!S.remote.active || (S.remote_indev != NULL && lv_indev_active() == S.remote_indev)) {
        return;
    }
    if (!S.remote_paused) {
        remote_pause(true);
        ui_sheet_close();
        if (S.pad != PAD_NONE && !pad_resident()) {
            ui_pad_cancel();
        }
        pad_forget_typing();
    }
    remote_question();
}

/* The pause comes from the platform with the session: it outlives a session, so a remote that
 * reconnects can't undo it, and an open question stays open for whoever paused it to answer. */
void ui_app_remote_session(const ui_remote_session_t *session)
{
    const bool was = S.remote.active;
    if (session == NULL || !session->active) {
        memset(&S.remote, 0, sizeof(S.remote));
    } else {
        S.remote = *session;
        S.remote.who[sizeof(S.remote.who) - 1u] = '\0';
        S.remote.where[sizeof(S.remote.where) - 1u] = '\0';
        if (!was) {
            saver_exit(); /* show the screen the remote is using */
            lv_display_trigger_activity(S.disp);
        }
    }
    S.remote_paused = session != NULL && session->paused;
    remote_banner_text();
    bar_banner(S.locked);
}

void ui_app_remote_indev(lv_indev_t *indev)
{
    S.remote_indev = indev;
}

bool ui_app_remote_text(const char *utf8)
{
    if (!S.remote.active || S.remote_paused || utf8 == NULL) {
        return false;
    }
    lv_display_trigger_activity(S.disp);
    ui_partition_input(NULL);
    if (ui_forms_remote_text(utf8)) {
        return true;
    }
    bool used = false;
    for (const char *c = utf8; S.pad != PAD_NONE && *c != '\0'; c++) {
        if (*c >= '0' && *c <= '9') {
            const char digit[2] = {*c, '\0'};
            pad_key(digit);
            used = true;
        }
    }
    return used;
}

bool ui_app_remote_key(ui_remote_key_t key)
{
    if (!S.remote.active || S.remote_paused) {
        return false;
    }
    lv_display_trigger_activity(S.disp);
    ui_partition_input(NULL);
    if (ui_forms_remote_key(key)) {
        return true;
    }
    if (S.pad == PAD_NONE) {
        return false;
    }
    switch (key) {
    case UI_REMOTE_KEY_BACKSPACE: pad_key("del"); break;
    case UI_REMOTE_KEY_ENTER: pad_key("ok"); break;
    case UI_REMOTE_KEY_ESCAPE: pad_close_by_user(); break;
    default: return false;
    }
    return true;
}

/* ---- actions -------------------------------------------------------------------------- */

/* What the panel can honestly say about telling anyone outside the house (UI-029). A command
 * the engine accepted means the alarm is on here; a broker that took a message means a message
 * was taken. Neither means anyone is coming, so the Emergency screen separates: the alarm is
 * set off locally, something is (or isn't) set up to carry it, that thing is (or isn't)
 * reachable right now - and the panel never calls emergency services. No integration is named
 * on a general screen (UI-018). */
typedef enum { UI_REPORT_NONE = 0, UI_REPORT_OFFLINE, UI_REPORT_ONLINE } ui_report_t;

static ui_report_t report_state(void)
{
    bool set_up = false;
    bool online = false;
    if (S.be.halink_status != NULL) {
        ui_halink_status_t h;
        memset(&h, 0, sizeof(h));
        S.be.halink_status(S.be.ctx, &h);
        if (h.enabled && h.paired) {
            set_up = true;
            online = online || h.state == UI_HALINK_CONNECTED;
        }
    }
    if (S.be.mqtt_status != NULL) {
        ui_mqtt_status_t m;
        memset(&m, 0, sizeof(m));
        S.be.mqtt_status(S.be.ctx, &m);
        if (m.state != UI_MQTT_OFF) {
            set_up = true;
            online = online || m.state == UI_MQTT_CONNECTED;
        }
    }
    return !set_up ? UI_REPORT_NONE : (online ? UI_REPORT_ONLINE : UI_REPORT_OFFLINE);
}

/* The line under the Emergency buttons. Polled once a second at most (UI-020). */
static void update_panic_report(void)
{
    /* TR: the Emergency screen, the line under its buttons. At most 127 bytes */
    static const char *const WORDS[] = {
        TR_NOOP("Nothing is connected to pass this on · The panel can't call emergency services"),
        TR_NOOP("A connected service is set up but offline now · The panel can't call emergency "
                "services"),
        TR_NOOP("A connected service is online and is sent the alarm · The panel can't call "
                "emergency services"),
    };
    ui_set_str(&subject_panic_report, TR(WORDS[report_state()]));
}

static void panic_reset(void)
{
    if (S.panic_arc != NULL) {
        lv_arc_set_value(S.panic_arc, 0);
    }
    S.panic_arc = NULL;
    S.panic_holding = false;
    S.panic_sent = false;
}

static void on_panic(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        const char *which = (const char *)lv_event_get_user_data(e);
        panic_reset();
        S.panic = (which != NULL && strcmp(which, "fire") == 0)      ? ALARM_PANIC_FIRE
                  : (which != NULL && strcmp(which, "medical") == 0) ? ALARM_PANIC_MEDICAL
                                                                     : ALARM_PANIC_POLICE;
        S.panic_arc = ui_find(lv_event_get_current_target_obj(e), "progress");
        S.panic_holding = true;
        S.panic_start = lv_tick_get();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_INDEV_RESET) {
        /* INDEV_RESET is how a remote pointer that dropped mid-press (lv_indev_reset) lets go:
         * without it the hold would complete on its own and set the alarm off. */
        if (S.panic_holding && !S.panic_sent) {
            /* TR: a toast when an Emergency button was let go too soon */
            ui_toast(TR("Keep holding for 2 seconds to set the alarm off"));
        }
        panic_reset();
    }
}

static void panic_answered(const ui_call_t *c);

static void panic_tick(void)
{
    if (!S.panic_holding || S.panic_sent) {
        return;
    }
    const uint32_t held = lv_tick_elaps(S.panic_start);
    if (S.panic_arc != NULL) {
        lv_arc_set_value(S.panic_arc, (int32_t)(held >= PANIC_HOLD_MS ? 100u : held * 100u / PANIC_HOLD_MS));
    }
    if (held < PANIC_HOLD_MS) {
        return;
    }
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_PANIC;
    cmd.partition = S.opt.partition;
    cmd.panic = S.panic;
    /* never dropped: with every slot taken, the next tick sends it */
    S.panic_sent = ui_call_command(&cmd, panic_answered, 0u);
}

static void panic_answered(const ui_call_t *c)
{
    const alarm_result_t r = c->r;
    if (r != ALARM_OK) {
        ui_toast(ui_result_text(r));
    } else if (c->cmd.panic == ALARM_PANIC_SILENT) {
        /* The one panic the screen deliberately doesn't show. It says the alarm is on here -
         * never that it was delivered, and never that help is coming - and stays short enough
         * to be unremarkable to anyone else in the room (UI-029). */
        /* TR: a toast after the silent panic; keep it short and unremarkable */
        ui_toast(report_state() == UI_REPORT_ONLINE ? TR("Silent alarm on")
                                                    : TR("Silent alarm on · nothing sent out"));
    }
    /* audible panics need no words: the screen becomes the alarm */
    ui_refresh(true);
    update_panic_report();
}

static void on_zone_bypass(lv_event_t *e)
{
    lv_obj_t *card = lv_obj_get_parent(lv_event_get_current_target_obj(e));
    const uint16_t z = (uint16_t)(uintptr_t)lv_obj_get_user_data(card);
    if (z < 1u || z > ALARM_MAX_ZONES) {
        return;
    }
    S.pad_zone = z;
    S.pad_unbypass = (S.snap.zone[z - 1u].flags & ALARM_ZS_BYPASSED) != 0u;
    pad_open_in(HOST_MODAL, PAD_BYPASS); /* the zone list stays behind it */
}

/* Chime with no code (CORE-CHIME-005, the installer's choice): one touch. Nothing opens; the
 * rail's bell changes under the finger and that is enough. */
static void chime_answered(const ui_call_t *c)
{
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(ui_result_text(c->r));
        return;
    }
    ui_refresh(true);
}

static void chime_toggle_now(void)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.partition = S.opt.partition;
    cmd.type = ALARM_CMD_CHIME;
    if (!ui_call_command(&cmd, chime_answered, 0u)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
}

/* The one way chime is switched: the rail's Chime and the row in Settings both come here.
 * It never leaves the view you are on (UI-025) and waits while armed (UI-022). */
void ui_chime_toggle(void)
{
    if (ui_link_holds_home()) {
        /* TR: a toast when the dimmed chime is tapped on a keypad without the alarm panel */
        ui_toast(TR("Chime can be changed once the alarm panel answers"));
    } else if (S.locked) {
        /* TR: a toast when chime is tapped while armed, or while the alarm sounds */
        locked_toast(TR("Chime can be changed once the system is disarmed"),
                     TR("Chime can be changed once the alarm is silenced"));
    } else if (S.cfg->opt.chime_needs_code) {
        pad_open_in(HOST_MODAL, PAD_CHIME);
    } else {
        chime_toggle_now();
    }
}

static void silence_answered(const ui_call_t *c);

static void on_silence(void)
{
    /* A keypad keeping a fire or CO tone through a lost link: the command can't reach the alarm
     * panel, and the link stops the tone here all the same (decided 2026-10-15). */
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_SILENCE;
    cmd.partition = S.opt.partition;
    if (!ui_call_command(&cmd, silence_answered, ui_link_local_alarm() ? 1u : 0u)) {
        ui_toast(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
}

static void silence_answered(const ui_call_t *c)
{
    const bool here = c->tag != 0u;
    const alarm_result_t r = c->r;
    if (here) {
        /* TR: a toast after Silence on a keypad without system communication: its own alarm
         * sound stops; the alarm panel goes on as it was */
        ui_toast(TR("Silenced on this keypad"));
    } else {
        /* TR: a toast after Silence during a fire, CO or medical alarm */
        ui_toast(r == ALARM_OK ? TR("Siren silenced. Enter your code to reset")
                               : ui_result_text(r));
    }
    ui_refresh(true);
}

/* The alarm panel's screen ends the installer mode a keypad holds (BL-223, LINK-060): leaving
 * installer mode never needs a code, here as at the keypad. */
static void end_keypad_installer(void)
{
    if (S.be.program_exit != NULL) {
        S.be.program_exit(S.be.ctx);
    }
    ui_refresh(true);
}

static void on_disarm_tile(void)
{
    if (S.mode == MODE_PROGRAM) {
        const uint8_t holder = ui_link_remote_holder();
        if (!S.program && ui_link_master() && holder >= 1u && holder <= UI_LINK_KEYPADS_MAX) {
            char title[64];
            /* TR: the alarm panel's question when its installer banner is tapped while a keypad
             * holds installer mode; {0} is the keypad's number. At most 63 bytes */
            I18N_FORMAT(title, sizeof(title), TR("End installer mode at Keypad {0}?"),
                        (unsigned)holder);
            /* TR: under it. At most 95 bytes */
            ui_confirm(title, TR("The installer there loses what they haven't saved."),
                       /* TR: its button. At most 23 bytes */
                       TR("End it"), end_keypad_installer);
            return;
        }
        if (S.program) {
            ui_installer_leave();
        } else {
            /* TR: a toast when the installer mode card is tapped at another keypad */
            ui_toast(TR("The installer is working at another keypad"));
        }
        return;
    }
    pad_open(PAD_DISARM);
}

/* Home during a walk test (WALK-013): Extend asks for a code on Home's pad (WALK-001), End asks
 * a question (WALK-025); the list is Settings > Walk test, which shows it without a code. */
static void on_walk_card(const char *ud)
{
    if (ud != NULL && strcmp(ud, "zones") == 0) {
        ui_settings_open();
        ui_settings_section(NOTR("walk"));
    } else if (ud != NULL && strcmp(ud, "update") == 0) {
        ui_settings_open(); /* the status bar's update banner: About, whose row asks for the code */
        ui_settings_section(NOTR("about"));
    } else if (ud != NULL && strcmp(ud, "banner") == 0) {
        ui_walk_banner_pressed(); /* the status bar's walk test banner (WALK-023, WALK-010) */
    } else if (ud != NULL && strcmp(ud, "pair") == 0) {
        ui_link_pair_again(); /* a keypad that isn't paired: Home's Pair again (BL-048) */
    } else if (ud != NULL && strcmp(ud, "extend") == 0) {
        pad_open(PAD_EXTEND_WALK_TEST);
    } else {
        ui_walk_end_ask();
    }
}

static void on_action(alarm_ui_action_t action, lv_event_t *e)
{
    const char *ud = (const char *)lv_event_get_user_data(e);
    switch (action) {
    case ALARM_UI_ACT_ARM:
        if (ui_link_holds_home()) {
            break; /* dimmed and deaf: a keypad without the alarm panel arms nothing (LINK-008) */
        }
        S.pad_level = (ud != NULL && strcmp(ud, "stay") == 0)    ? ALARM_LEVEL_STAY
                      : (ud != NULL && strcmp(ud, "night") == 0) ? ALARM_LEVEL_NIGHT
                                                                 : ALARM_LEVEL_AWAY;
        pad_open(PAD_ARM);
        break;
    case ALARM_UI_ACT_DISARM: on_disarm_tile(); break;
    case ALARM_UI_ACT_PAD_KEY: pad_key(ud); break;
    case ALARM_UI_ACT_PAD_CLOSE: pad_close_by_user(); break;
    case ALARM_UI_ACT_SILENCE: on_silence(); break;
    case ALARM_UI_ACT_CLEAR_MEMORY: pad_open(PAD_CLEAR_MEMORY); break;
    case ALARM_UI_ACT_END_WALK_TEST: on_walk_card(ud); break;
    case ALARM_UI_ACT_ZONE_BYPASS: on_zone_bypass(e); break;
    /* Locked while armed or in alarm (UI-022): these say why instead. Acknowledging is code +
     * OFF, which would disarm. */
    case ALARM_UI_ACT_ACK_TROUBLES:
        if (S.locked) {
            /* TR: a toast when troubles are acknowledged while armed, or while the alarm
             * sounds */
            locked_toast(TR("Troubles can be acknowledged once the system is disarmed"),
                         TR("Troubles can be acknowledged once the alarm is silenced"));
        } else {
            pad_open_in(HOST_MODAL, PAD_ACK); /* the trouble list stays behind it */
        }
        break;
    case ALARM_UI_ACT_TROUBLES_PAGE: troubles_page_turn(ud); break;
    case ALARM_UI_ACT_PANIC: on_panic(e); break;
    case ALARM_UI_ACT_CHIME: ui_chime_toggle(); break;
    case ALARM_UI_ACT_CHANGE_CODE:
        if (S.locked) {
            /* TR: a toast when a code is changed while armed, or while the alarm sounds */
            locked_toast(TR("Codes can be changed once the system is disarmed"),
                         TR("Codes can be changed once the alarm is silenced"));
        } else {
            pad_open_in(HOST_MODAL, PAD_CODE_CURRENT); /* asked from the status bar, from any view */
        }
        break;
    case ALARM_UI_ACT_SETTINGS:
        if (S.locked && !ui_link_settings_waived()) {
            /* TR: a toast when Settings is tapped while armed, or while the alarm sounds */
            locked_toast(TR("Settings open once the system is disarmed"),
                         TR("Settings open once the alarm is silenced"));
        } else {
            ui_settings_open();
        }
        break;
    case ALARM_UI_ACT_WAKE: saver_exit(); break;
    case ALARM_UI_ACT_SETTINGS_SECTION: ui_settings_section(ud); break;
    case ALARM_UI_ACT_PAGE_BACK: ui_page_back(); break;
    case ALARM_UI_ACT_SHEET_DONE: ui_sheet_done(); break;
    case ALARM_UI_ACT_SHEET_CANCEL: ui_sheet_cancel(); break;
    case ALARM_UI_ACT_SHEET_EYE: ui_sheet_eye(); break;
    case ALARM_UI_ACT_REMOTE_BANNER: remote_banner_pressed(); break;
    case ALARM_UI_ACT_PROGRAM_EXIT: on_disarm_tile(); break;
    case ALARM_UI_ACT_WIZARD_NEXT: ui_wizard_next(); break;
    case ALARM_UI_ACT_WIZARD_BACK: ui_wizard_back(); break;
    case ALARM_UI_ACT_WIZARD_EXIT: ui_wizard_exit(); break;
    }
}

#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
bool ui_memory_diagnostic_step(unsigned step)
{
    if (!S.have_snap || S.locked || S.program || S.auth_user != 0u) {
        return false;
    }
    static const char *sections[] = {"display", "installer", "users", "wifi", "time",
                                      "integrations", NULL, "about"};
    if (step == 0u || step == 9u) {
        saver_exit();
        ui_set_int(&subject_view, VIEW_HOME);
    } else if (step <= 8u && sections[step - 1u] == NULL) {
        ui_set_int(&subject_view, VIEW_ACTIVITY); /* the whole event log (BL-131 finding 7) */
    } else if (step <= 8u) {
        ui_settings_open();
        ui_settings_section(sections[step - 1u]);
    } else if (step == 10u) {
        saver_enter(UI_LIGHT_SAVER);
    } else if (step == 11u) {
        saver_exit();
    } else {
        return false;
    }
    return true;
}
#endif

static int32_t s_last_view;

static void view_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    const int32_t view = lv_subject_get_int(&subject_view);
    if (s_last_view == VIEW_SETTINGS && view != VIEW_SETTINGS) {
        ui_settings_leave();
    }
    if (s_last_view == VIEW_ACTIVITY && view != VIEW_ACTIVITY) {
        activity_forget(); /* LOG-008: leaving Activity forgets the code */
    }
    const bool to_activity = view == VIEW_ACTIVITY && s_last_view != VIEW_ACTIVITY;
    s_last_view = view;
    if (!S.have_snap) {
        return;
    }
    if (view != VIEW_PANIC) {
        panic_reset();
    } else {
        update_panic_report(); /* right the moment it opens, then once a second */
    }
    if (view == VIEW_TROUBLES) {
        S.trouble_page = 0; /* coming back to the list starts at the active faults */
    }
    if (to_activity) {
        s_act_pages = 1; /* coming back starts at the newest events */
        activity_build(false);
    }
    S.zones_empty_filter = -1; /* opening the view shows the empty filter's check at once */
    update_lists();
#if ALARM_UI_DEVICES
    if (view == VIEW_DEVICES) {
        ui_devices_shown();
    }
#endif
}

/* The dots take the hint's line while digits are typed and no error is showing. */
static void dots_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    ui_set_int(&subject_pad_dots_on, lv_subject_get_int(&subject_pad_digits) > 0 &&
                                             lv_subject_get_int(&subject_pad_error) == 0
                                         ? 1
                                         : 0);
}

/* The "No entry delay" chip: the pad title stays the level ("Arm Away"). */
static void instant_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    if (S.pad == PAD_ARM) {
        char title[64];
        i18n_utf8_copy(title, sizeof(title), arm_title(S.pad_level, pad_instant()));
        ui_set_str(&subject_pad_title, title); /* a "not ready" hint stays */
    }
}

/* One rule for a toast and whatever opens over the screen, worked out whenever either side
 * changes rather than only when a toast appears (UI-031):
 *   - the text sheet's card is at the bottom, so the toast moves to the top and every key,
 *     Space and Done included, stays visible and reachable;
 *   - a choice list, a confirmation or the code pad over the screen fills the middle: words
 *     about what happened before it opened would land on its title, and they have been read
 *     by now, so the toast goes (the same rule pad_show() has always used).
 * Nothing is left hovering: the toast is never clickable (box.xml) and it is hidden, not just
 * moved out of sight, so it can take no touch meant for a key. */
static void sheet_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    if (!S.toast_on) {
        return;
    }
    const int32_t sheet = lv_subject_get_int(&subject_sheet);
    if (sheet == SHEET_PICK || sheet == SHEET_CONFIRM) {
        toast_hide();
    } else {
        toast_align();
    }
}

static void filter_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    if (S.have_snap && S.zones_built) {
        zones_update();
    }
}

static void indev_pressed(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_param(e);
    /* Click for controls only: screens are clickable too, and so is every scrolling
     * viewport (so a drag can start on a plain row or a gap), but pressing that background
     * isn't a key press. */
    if (S.opt.key_click && obj != NULL && !S.saver_on && lv_obj_get_parent(obj) != NULL &&
        lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        !lv_obj_has_flag(obj, UI_FLAG_SCROLL_SURFACE)) {
        ui_beep(UI_BEEP_KEY);
    }
}

/* ---- tick ----------------------------------------------------------------------------- */

/* ---- the language, changed while running (docs/features/37 I18N-014) ----------------------- */

/* The XML's words in `from` become the same words in `to`. A row's or a card's title and detail
 * are left alone: they hold household names and C's words, and are built again after this. */
static void relabel(lv_obj_t *o, const i18n_catalog_t *from, const i18n_catalog_t *to)
{
    if (o == NULL) {
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *name = lv_obj_get_name(o);
        const bool words = name == NULL ||
                           (strcmp(name, "title_label") != 0 && strcmp(name, "detail_label") != 0);
        const char *text = lv_label_get_text(o);
        for (size_t i = 0; words && text != NULL && i < s_n_literals; i++) {
            const ui_literal_t *lit = &s_literals[i];
            if (strcmp(text, i18n_text(from, lit->ctx, lit->english)) == 0) {
                const char *now = i18n_text(to, lit->ctx, lit->english);
                if (strcmp(now, text) != 0) {
                    lv_label_set_text(o, now);
                }
                break;
            }
        }
    }
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        relabel(lv_obj_get_child(o, (int32_t)i), from, to);
    }
}

/* An alarm, an entry time or an exit time keeps its words: nobody reading the screen then should
 * see it change language under them (and the rest of how it looks: the theme by the clock, a
 * setting from elsewhere, UI-046). */
bool ui_app_held(void)
{
    return S.mode_valid && (alarm_mode(S.mode) || S.mode == MODE_ENTRY || S.mode == MODE_EXIT);
}

/* A language's own letters (docs/features/37 I18N-049): a pack may carry a font for each of the five
 * text fonts. It is loaded once (LVGL copies what it needs out of the pack's bytes), kept for good -
 * LVGL may still reference one after the language changed, and languages change rarely - and hung
 * behind the built-in font as its `fallback` while the language is on; the built-in font draws
 * what it has, the language's font what it lacks. English and a language without fonts leave it
 * out. Runs before the relabelling, so text is laid out with the fallback.
 *
 * The letters pack (docs/features/39 asset-packs.md "The panel", OPT-010): the image's text fonts
 * keep ASCII, the symbols and the shipped catalogs' letters; the rest of U+00A0-U+017F comes from
 * the letters pack's font for the role (caption, body, label, title; the headline has none). The
 * chain of a text font is: the built-in font, then the language's font when the language brings
 * one, then the letters font - the language's fonts are this module's own objects, so their own
 * `fallback` is set here too. A letters font is loaded once per copy the backend hands out (a new
 * copy has new bytes: one installed while the panel runs) and kept for good, as the language's.
 * Without the pack a letter outside the image's set draws as the placeholder box (I18N-011). A
 * pack whose fingerprint differs from the image's is still used: a letter a pixel off beats a box. */
#define LANG_FONT_CACHE 16u
#define LETTERS_ROLES 4u /* caption, body, label, title: the first roles below */
#define LETTERS_FONT_CACHE 16u /* the panel loads at most four copies of a kind between restarts */

typedef struct {
    char locale[16];
    uint8_t role; /* index into the roles below */
    lv_font_t *font;
} lang_font_t;

typedef struct {
    const void *data; /* the backend's bytes it was made from */
    uint8_t role;
    lv_font_t *font;
} letters_font_t;

static lang_font_t s_lang_fonts[LANG_FONT_CACHE] UI_SLOW_BSS;
static size_t s_lang_font_n;
static letters_font_t s_letters_fonts[LETTERS_FONT_CACHE] UI_SLOW_BSS;
static size_t s_letters_font_n;

static const char *const s_font_roles[] = {"caption", "body", "label", "title", "headline"};

/* The letters pack's font for a role, NULL without one (no pack, no part, or none loads). */
static lv_font_t *letters_font(size_t r)
{
#if LV_USE_FS_MEMFS
    const void *data = NULL;
    size_t size = 0;
    if (r >= LETTERS_ROLES || S.be.asset_font == NULL ||
        !S.be.asset_font(S.be.ctx, s_font_roles[r], &data, &size) || data == NULL || size == 0u ||
        size > UINT32_MAX) {
        return NULL;
    }
    for (size_t i = 0; i < s_letters_font_n; i++) {
        if (s_letters_fonts[i].role == r && s_letters_fonts[i].data == data) {
            return s_letters_fonts[i].font;
        }
    }
    if (s_letters_font_n >= LETTERS_FONT_CACHE) {
        return NULL;
    }
    lv_font_t *f = lv_binfont_create_from_buffer((void *)(uintptr_t)data, (uint32_t)size);
    if (f == NULL) {
        return NULL;
    }
    letters_font_t *e = &s_letters_fonts[s_letters_font_n++];
    e->data = data;
    e->role = (uint8_t)r;
    e->font = f;
    return f;
#else
    (void)r;
    return NULL;
#endif
}

static void lang_fonts_apply(const char *locale, bool on)
{
    lv_font_t *const text_fonts[] = {font_caption, font_body, font_label, font_title, font_headline};
    for (size_t r = 0; r < sizeof(s_font_roles) / sizeof(s_font_roles[0]); r++) {
        if (text_fonts[r] == NULL) {
            continue;
        }
        lv_font_t *lang = NULL;
#if LV_USE_FS_MEMFS
        if (on && S.be.lang_font != NULL && locale != NULL) {
            for (size_t i = 0; i < s_lang_font_n && lang == NULL; i++) {
                if (s_lang_fonts[i].role == r && strcmp(s_lang_fonts[i].locale, locale) == 0) {
                    lang = s_lang_fonts[i].font;
                }
            }
            const void *data = NULL;
            size_t size = 0;
            if (lang == NULL && strlen(locale) < sizeof(s_lang_fonts[0].locale) &&
                s_lang_font_n < LANG_FONT_CACHE &&
                S.be.lang_font(S.be.ctx, locale, s_font_roles[r], &data, &size) && data != NULL &&
                size > 0u && size <= UINT32_MAX) {
                lv_font_t *f = lv_binfont_create_from_buffer((void *)(uintptr_t)data, (uint32_t)size);
                if (f != NULL) {
                    lang_font_t *e = &s_lang_fonts[s_lang_font_n++];
                    snprintf(e->locale, sizeof(e->locale), "%s", locale);
                    e->role = (uint8_t)r;
                    e->font = f;
                    lang = f;
                }
            }
        }
#else
        (void)on;
        (void)locale;
#endif
        lv_font_t *letters = letters_font(r);
        if (lang != NULL) {
            lang->fallback = letters;
            text_fonts[r]->fallback = lang;
        } else {
            text_fonts[r]->fallback = letters;
        }
    }
    ui_forms_fonts_changed(); /* the keyboard's keys follow font_label's chain */
}

void ui_language_apply(void)
{
    const i18n_catalog_t *to = ui_lang_catalog(S.opt.lang);
    if (to == S.catalog) {
        S.lang_pending = false;
        return;
    }
    if (ui_app_held()) {
        S.lang_pending = true;
        return;
    }
    S.lang_pending = false;
    /* The spoken-name editor's sheets hold words made in the old language: it ends first (its
     * draft dropped, its Preview stopped, its lease released), before anything is relabelled. */
    ui_voice_close();
    const i18n_catalog_t *from = S.catalog;
    lang_fonts_apply(S.opt.lang, to != NULL);
    relabel(S.main, from, to);
    relabel(S.saver, from, to);
    relabel(S.wizard, from, to);
    relabel(lv_layer_top(), from, to);
    S.catalog = to;
    /* What C wrote, written again: the lists, the clock, the status and the open page. */
    S.zones_built = false;
    S.events_shown_seq = UINT32_MAX;
    S.last_minute = -1;
    ui_forms_language_changed();
    ui_refresh(true);
    update_clock(true);
    update_net();
    update_power();
    ui_settings_language_changed();
    ui_wizard_language_changed();
#if ALARM_UI_DEVICES
    ui_devices_language_changed();
#endif
#if ALARM_UI_AUTOMATIONS
    ui_automations_language_changed();
    ui_auto_warn_language_changed();
#endif
}

/* A language was installed, removed, loaded or replaced (docs/features/37 I18N-042): look the
 * stored language up again, and draw the Language page again if it is open. */
static void language_generation_tick(void)
{
    if (S.be.lang_generation == NULL) {
        return;
    }
    const uint32_t gen = S.be.lang_generation(S.be.ctx);
    if (gen == S.lang_gen) {
        return;
    }
    ui_language_apply();
    if (S.depth > 0u && S.pages[S.depth - 1u].id == PAGE_TIME &&
        lv_subject_get_int(&subject_view) == VIEW_SETTINGS) {
        if (lv_subject_get_int(&subject_sheet) != SHEET_NONE) {
            return; /* a list is open on it: once it closes */
        }
        ui_page_rebuild();
    }
    S.lang_gen = gen;
}

/* A letters pack was installed or replaced while the panel runs (docs/features/39 asset-packs.md
 * "The panel", OPT-010): the text fonts take the new copy, then every text is laid out again and
 * what C wrote is written again, as a language change does - without closing what is open. The
 * keyboard's àé key follows when a keyboard next opens (ui_forms.c), never under a finger. */
static void asset_generation_tick(void)
{
    if (S.be.asset_generation == NULL) {
        return;
    }
    const uint32_t gen = S.be.asset_generation(S.be.ctx);
    if (gen == S.asset_gen) {
        return;
    }
    S.asset_gen = gen;
    lang_fonts_apply(S.catalog != NULL ? S.catalog->locale : S.opt.lang, S.catalog != NULL);
    lv_obj_t *const roots[] = {S.main, S.saver, S.wizard, lv_layer_top()};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i] != NULL) {
            lv_obj_refresh_style(roots[i], LV_PART_ANY, LV_STYLE_PROP_ANY);
        }
    }
    S.zones_built = false;
    S.events_shown_seq = UINT32_MAX;
    ui_refresh(true);
    if (lv_subject_get_int(&subject_sheet) == SHEET_NONE) {
        ui_settings_language_changed(); /* the open page, as drawn in C */
        ui_wizard_language_changed();
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (!S.lit) {
        S.lit = true; /* the first frame has been drawn by now: no garbage on power-up */
        light_apply();
    }
    ui_refresh(false);
    if (S.lang_pending && !ui_app_held()) {
        ui_language_apply(); /* the alarm or the delay that held it ended */
    }
    update_countdown();
    update_lockout();
    ui_walk_home();
    update_clock(false);
    if (lv_tick_elaps(S.net_polled) >= NET_POLL_MS) {
        S.net_polled = lv_tick_get();
        language_generation_tick();
        asset_generation_tick();
        update_net();
        update_power();
        if (lv_subject_get_int(&subject_view) == VIEW_ACTIVITY &&
            (S.events_shown_seq != S.snap.last_event_seq || s_act_day != activity_today())) {
            activity_build(true); /* the log writer caught up, or today became yesterday */
        }
        if (lv_subject_get_int(&subject_view) == VIEW_PANIC) {
            update_panic_report(); /* what can be passed on, at most once a second (UI-020) */
        }
        if (lv_subject_get_int(&subject_view) == VIEW_SETTINGS) {
            ui_update_tag(); /* About speaks in the list while an update waits */
        }
    }
    if (lv_subject_get_int(&subject_view) == VIEW_ZONES && S.zones_built) {
        zones_empty_tick(); /* the empty filter's check, once the list settled (BL-177) */
    }
    if (S.toast_on && lv_tick_elaps(S.toast_since) >= TOAST_MS) {
        S.toast_on = false;
        ui_set_int(&subject_toast_visible, 0);
    }
    panic_tick();
    hero_tone_tick();
    ui_forms_tick();
    ui_settings_tick();
    ui_walk_tick();
    ui_net_tick();
    ui_integrations_tick();
    ui_notify_tick();
    ui_update_banner_tick();
    ui_update_tick();
    ui_sources_tick();
#if ALARM_UI_ZWAVE
    ui_zwave_tick();
#endif
#if ALARM_UI_ZIGBEE
    ui_zigbee_tick();
#endif
    ui_sirens_tick();
#if ALARM_UI_DEVICES
    ui_devices_tick();
#endif
#if ALARM_UI_AUTOMATIONS
    ui_automations_tick();
    ui_auto_warn_tick();
#endif
    ui_wizard_tick();
    ui_link_tick(); /* the panel link (BL-048): once a second */

    const uint32_t idle = lv_display_get_inactive_time(S.disp);
    const int32_t view = lv_subject_get_int(&subject_view);
    const bool setup = S.wizard_on && !S.wizard_paused;
    const bool calm = (S.mode == MODE_READY || S.mode == MODE_NOT_READY || S.mode == MODE_AWAY ||
                       S.mode == MODE_STAY || S.mode == MODE_NIGHT || S.mode == MODE_MEMORY ||
                       S.mode == MODE_PROGRAM) &&
                      !ui_app_busy() && /* the same test a walk test started elsewhere waits on */
                      !S.remote.active && /* someone at the panel sees a remote session */
                      !ui_walk_running() && /* awake for the whole test, any partition (WALK-022) */
                      !ui_link_local_alarm(); /* a keypad's kept fire or CO tone: as an alarm */
    /* Dimmed, then dark (UI-009, UI-043): only while nothing else needs the screen. Night hours
     * that begin or end, or new times, move it on from where it is; only a touch or the panel
     * itself (a change of state, an alarm) wakes it. */
    S.night = ui_light_night(&S.opt, S.local_minute);
    const ui_light_stage_t want = ui_light_stage(&S.opt, idle, S.night);
    if (calm && want != UI_LIGHT_AWAKE) {
        if (!S.saver_on) {
            ui_set_int(&subject_view, rest_view()); /* what a touch wakes it to */
        }
        saver_enter(want);
    } else if (S.saver_on && !calm) {
        saver_exit();
    }
    light_apply(); /* night hours beginning or ending while in use, a slider's preview ending */
    /* A code entry nobody finished, wherever it sits (UI-032). */
    if (!S.saver_on && S.pad != PAD_NONE && idle >= HOME_AFTER_MS) {
        pad_idle_expired();
    }
    const uint32_t home_after = view == VIEW_SETTINGS ? SETTINGS_IDLE_MS : HOME_AFTER_MS;
    const int32_t rest = rest_view();
    if (!S.saver_on && !setup && idle >= home_after && S.pad_host == HOST_HOME &&
        (S.pad == PAD_NONE || pad_resident() || view == VIEW_SETTINGS) && view != rest) {
        ui_sheet_close();
        ui_set_int(&subject_view, rest);
    }
    /* BL-220 R8: after a start, Smart Home as soon as its devices are in, while nobody touches the
     * screen; given up after two minutes (no device came) or at the first touch */
    if (s_start_pending) {
        const uint32_t up = lv_tick_get();
        const bool touched = idle + 1000u < up; /* activity since the start */
        if (touched || up >= START_GIVE_UP_MS) {
            s_start_pending = false; /* the 90 s rest takes it from here */
        } else if (rest == VIEW_DEVICES && view == VIEW_HOME && !S.saver_on && !setup &&
                   S.pad == PAD_NONE && idle >= START_SETTLE_MS) {
            ui_set_int(&subject_view, VIEW_DEVICES);
            s_start_pending = false;
        }
    }
}

/* ---- start ---------------------------------------------------------------------------- */

/* The appearance changed (ui_theme.c): the colours kept here take the new palette's. */
void ui_app_theme_changed(void)
{
    const lv_color_t *pal = alarm_ui_palette;
    S.pad_ask_tone = alarm_ui_theme_color(S.pad_ask_tone, pal);
    s_hero_tone = alarm_ui_theme_color(s_hero_tone, pal);
    s_hero_tone_wanted = alarm_ui_theme_color(s_hero_tone_wanted, pal);
}

void ui_app_default_options(ui_options_t *opt)
{
    ui_options_defaults(opt);
}

void ui_app_run_setup(void)
{
    ui_wizard_start(true); /* already set up: the setup can be left again */
}

void ui_app_first_setup(void)
{
    ui_wizard_start(false); /* as a first start: no Leave setup, the role first (BL-048) */
}

void ui_app_start(const ui_backend_t *backend, const ui_options_t *opt)
{
    memset(&S, 0, sizeof(S));
    S.be = *backend;
    ui_call_reset();
    S.local_minute = -1;
    S.light_sent = -1;
    S.light_preview = -1;
    S.zones_empty_filter = -1; /* no filter applied yet: the first one shows at once */
    S.lang_gen = S.be.lang_generation != NULL ? S.be.lang_generation(S.be.ctx) : 0u;
    S.asset_gen = S.be.asset_generation != NULL ? S.be.asset_generation(S.be.ctx) : 0u;
    if (opt != NULL) {
        S.opt = *opt;
    } else {
        ui_app_default_options(&S.opt);
    }
    S.opt.lang[sizeof(S.opt.lang) - 1u] = '\0';
    S.opt.region[sizeof(S.opt.region) - 1u] = '\0';
    /* The stored language: English has no catalog, and one this firmware lacks is English too. */
    S.catalog = ui_lang_catalog(S.opt.lang);
    if (S.opt.partition < 1u || S.opt.partition > ALARM_MAX_PARTITIONS) {
        S.opt.partition = 1;
    }
    S.last_minute = -1;
    S.cfg = lv_malloc_zeroed(sizeof(alarm_config_t));
    LV_ASSERT_MALLOC(S.cfg);

    S.disp = lv_display_get_default();
    alarm_ui_init("");
    ui_sheet_note_init(); /* before the screens bind it */
    /* The stored appearance, at the minute it is now (Automatic), before the screens are made in
     * it (UI-010): with no screens yet, ui_theme_apply only sets the palette. */
    const int64_t now_ms = S.be.epoch_ms(S.be.ctx);
    if (now_ms > 0) {
        struct tm tm;
        local_tm(now_ms, &tm);
        S.local_minute = tm.tm_hour * 60 + tm.tm_min;
    }
    s_theme_wanted = -1;
    theme_follow();
    lang_fonts_apply(S.opt.lang, S.catalog != NULL);
    S.main = screen_main_create();
    lv_image_set_src(ui_find(S.main, "rail_brand"), &ui_img_roboalarms_mark);
    S.saver = screen_saver_create();
    ui_theme_watch_saver(); /* after the saver's bindings: it runs after them */
    S.wizard = screen_wizard_create();
    S.zone_list = ui_find(S.main, "zone_list");
    S.event_list = ui_find(S.main, "event_list");
    {
        /* Activity's filters: as wide as the view at most, then they slide (I18N-022). */
        lv_obj_t *filters = ui_find(S.main, "event_filters");
        lv_obj_set_style_max_width(filters, lv_pct(100), 0);
        lv_obj_set_scroll_dir(filters, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(filters, LV_SCROLLBAR_MODE_OFF);
    }
    S.trouble_list = ui_find(S.main, "trouble_list");
    S.page_body = ui_find(S.main, "page_body");
    S.pads[HOST_HOME] = ui_find(S.main, "pad");
    S.pads[HOST_SETTINGS] = ui_find(S.main, "settings_pad");
    S.pads[HOST_WIZARD] = ui_find(S.wizard, "wiz_pad");
    S.pads[HOST_MODAL] = ui_find(S.main, "modal_pad_card");
    lv_obj_t *bar = ui_find(S.main, "bar");
    if (bar != NULL) {
        lv_obj_add_event_cb(bar, bar_fit, LV_EVENT_LAYOUT_CHANGED, NULL);
    }
    for (size_t i = 0; i < sizeof(S.pads) / sizeof(S.pads[0]); i++) {
        if (S.pads[i] != NULL) {
            lv_obj_add_event_cb(S.pads[i], pad_fit, LV_EVENT_LAYOUT_CHANGED, NULL);
        }
    }
#if !ALARM_UI_INTEGRATIONS && !ALARM_UI_NOTIFY
    /* Nothing to integrate with in this firmware: no Integrations section (docs/features/33). */
    lv_obj_add_flag(ui_find(S.main, "set_integrations"), LV_OBJ_FLAG_HIDDEN);
#endif
    alarm_ui_set_action_cb(on_action);
    lv_subject_add_observer(&subject_view, view_changed, NULL);
    lv_subject_add_observer(&subject_zone_filter, filter_changed, NULL);
    lv_subject_add_observer(&subject_event_filter, event_filter_changed, NULL);
    lv_subject_add_observer(&subject_sheet, sheet_changed, NULL);
    lv_subject_add_observer(&subject_opt_instant, instant_changed, NULL);
    lv_subject_add_observer(&subject_pad_digits, dots_changed, NULL);
    lv_subject_add_observer(&subject_pad_error, dots_changed, NULL);
    for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
        lv_indev_add_event_cb(i, indev_pressed, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(i, ui_partition_input, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(i, ui_partition_input, LV_EVENT_PRESSING, NULL);
        lv_indev_add_event_cb(i, ui_partition_input, LV_EVENT_RELEASED, NULL);
    }
    ui_forms_init();
    ui_settings_init();
    ui_wizard_init();
#if ALARM_UI_DEVICES
    ui_devices_init();
#endif
#if ALARM_UI_AUTOMATIONS
    ui_automations_init();
    ui_auto_warn_init();
#endif

    ui_link_init(); /* the role and its partition, before Home and the setup (BL-048) */
    lv_screen_load(S.main);
    ui_refresh(true);
    update_clock(true);
    update_net();
    update_power();
    S.net_polled = lv_tick_get();
    lv_timer_create(tick, TICK_MS, NULL);
    if (S.opt.run_setup) {
        ui_wizard_start(false); /* first boot: the setup ends at "You're all set" */
    }
}
