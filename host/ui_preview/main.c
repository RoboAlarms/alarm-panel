/*
 * main.c - PC preview of the touch UI.
 *
 * Runs the real alarm engine, the event log, the LVGL Pro UI and its logic on a headless
 * 1024x600 RGB565 display, drives them through a scripted evening (arming, entry, alarm, fire,
 * troubles...), then Settings, installer mode and the first-boot setup, with a virtual
 * touchscreen, and writes a PNG of every step.
 *
 *   ui_preview <output folder>
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "alarm/alarm_strings.h"
#include "annunciator/annunciator.h"
#include "alarm_ui.h"
#include "alarm_ui_gen.h"
#include "i18n/i18n.h"
#include "host_panel.h"
#include "zone_hub/zone_eol.h"
#include "home_control/hc_home.h"
#include "voice_walk.h"
#include "host_link_demo.h"
#include "host_zigbee_demo.h"
#include "shot_png.h"
#include "lvgl.h"
#include "src/misc/lv_text_private.h" /* lv_text_get_next_line: where LVGL breaks a sheet's title (BL-184) */
#include "ui_app.h"
#include "ui_backend/ui_dev_model.h"
#include "ui_status.h"
#if ALARM_SIM_ZWAVE
#include "zwave_proto/zw_cc.h"
#endif

/* The board profile's screen (cmake/alarm_board.cmake); the 7" panel's without one. */
#ifndef ALARM_SCREEN_W
#define ALARM_SCREEN_W 1024
#define ALARM_SCREEN_H 600
#endif
#define W ALARM_SCREEN_W
#define H ALARM_SCREEN_H

static uint16_t s_fb[W * H];
static uint32_t s_ms;
static int32_t s_px;
static int32_t s_py;
static bool s_pressed;
static const char *s_out = "ui_preview_out";
static int s_shots;
static int s_clicks;

/* Where the walk's time goes (printed at the end): the waits, the frames LVGL draws in them,
 * and each screenshot's drawing, checks and PNG. */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static double s_t_wait, s_t_frames, s_t_draw, s_t_checks, s_t_png, s_t_voice, s_t_frame0;
static unsigned s_frames;
static void on_frame(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_t_frame0 = now_s();
    } else {
        s_t_frames += now_s() - s_t_frame0;
        s_frames++;
    }
}

static uint32_t tick_cb(void)
{
    return s_ms;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_px;
    data->point.y = s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Advance the world: engine time and LVGL time together, in 10 ms steps. */
static void run(uint32_t ms)
{
    const double t0 = now_s();
    for (uint32_t t = 0; t < ms; t += 10u) {
        s_ms += 10u;
        host_panel_advance(10u);
        lv_timer_handler();
    }
    s_t_wait += now_s() - t0;
}

/* -Locale (docs/features/37 I18N-022, I18N-024): the walk in a test language. Texts are found
 * through the catalog (or the test language's own transform, which contexts don't change), and
 * the walk's checks only report: they are written in English and the English walk keeps them.
 * Every screenshot then reports each label whose text doesn't fit. */
static const i18n_catalog_t *s_cat;
static bool s_locale;
static unsigned s_soft_fails;
static unsigned s_clipped;
static unsigned s_glyph_misses; /* icons without a glyph in their font, once each (OPT-005) */
static unsigned s_shortened; /* labels meant to shorten that did, with "..." (I18N-022) */
static void scroll_into_view(lv_obj_t *o);

/* A real language (es-US) has no transform to undo: a text is found through its translation with
 * no context or any context the template uses ("user level", "zone state"...), read once from
 * messages.pot (ALARM_PREVIEW_POT). */
static char s_ctx[192][64];
static size_t s_ctx_count;

#ifdef ALARM_PREVIEW_LOCALE
static void load_contexts(const char *pot)
{
    FILE *f = fopen(pot, "r");
    if (f == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL && s_ctx_count < sizeof(s_ctx) / sizeof(s_ctx[0])) {
        char ctx[64];
        if (sscanf(line, "msgctxt \"%63[^\"]\"", ctx) != 1) {
            continue;
        }
        bool seen = false;
        for (size_t i = 0; i < s_ctx_count && !seen; i++) {
            seen = strcmp(s_ctx[i], ctx) == 0;
        }
        if (!seen) {
            snprintf(s_ctx[s_ctx_count++], sizeof(s_ctx[0]), "%s", ctx);
        }
    }
    fclose(f);
}
#endif

/* The ways the searched English text can show in the walk's language, worked out once per text. */
static char s_cand_for[600];
static char s_cand[48][600];
static size_t s_cand_count;

/* `english` (a pattern when args are given) translated under every context it has. */
static void add_translations(const char *english, const i18n_arg_t *args, size_t nargs)
{
    for (size_t i = 0; i <= s_ctx_count && s_cand_count < sizeof(s_cand) / sizeof(s_cand[0]); i++) {
        const char *tr = i18n_text(s_cat, i == 0u ? NULL : s_ctx[i - 1u], english);
        if (tr == english) {
            continue; /* not translated under this context */
        }
        if (nargs > 0u) {
            i18n_format(s_cand[s_cand_count], sizeof(s_cand[0]), tr, args, nargs);
        } else {
            snprintf(s_cand[s_cand_count], sizeof(s_cand[0]), "%s", tr);
        }
        s_cand_count++;
    }
}

/* A text put together from translated pieces joined by " · " ("Radio · Working": the
 * Zigbee page's "%s · %s" of TR_C("Zigbee radio", "Radio") and a radio state): each piece
 * under the first context that translates it. */
static const char *first_translation(const char *english)
{
    for (size_t i = 0; i <= s_ctx_count; i++) {
        const char *tr = i18n_text(s_cat, i == 0u ? NULL : s_ctx[i - 1u], english);
        if (tr != english) {
            return tr;
        }
    }
    return english;
}

static void add_dotted(const char *want)
{
    static const char sep[] = " \xC2\xB7 ";
    if (strstr(want, sep) == NULL || s_cand_count >= sizeof(s_cand) / sizeof(s_cand[0])) {
        return;
    }
    char out[600];
    size_t n = 0;
    for (const char *p = want; p != NULL && n < sizeof(out);) {
        const char *end = strstr(p, sep);
        char part[300];
        snprintf(part, sizeof(part), "%.*s", (int)(end != NULL ? (size_t)(end - p) : strlen(p)), p);
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s%s", n > 0u ? sep : "",
                              first_translation(part));
        p = end != NULL ? end + strlen(sep) : NULL;
    }
    snprintf(s_cand[s_cand_count++], sizeof(s_cand[0]), "%s", out);
}

/* The text itself; with its numbers as placeholders ("Zone 72" from "Zone {0}"); with one value
 * at either end ("Use Sensor A2B3C" from "Use {0}", "Porch · restored" from "{0} · restored"). */
static void find_candidates(const char *want)
{
    if (strcmp(want, s_cand_for) == 0) {
        return;
    }
    snprintf(s_cand_for, sizeof(s_cand_for), "%s", want);
    s_cand_count = 0;
    add_dotted(want);
    add_translations(want, NULL, 0);
    char pattern[600];
    char nums[3][16];
    i18n_arg_t args[3];
    size_t n = 0;
    size_t k = 0;
    for (const char *p = want; *p != '\0' && n + 4u < sizeof(pattern);) {
        size_t digits = 0;
        while (p[digits] >= '0' && p[digits] <= '9') {
            digits++;
        }
        if (digits > 0u && digits < sizeof(nums[0]) && k < 3u) {
            snprintf(nums[k], sizeof(nums[k]), "%.*s", (int)digits, p);
            args[k] = i18n_arg_str(nums[k]);
            n += (size_t)snprintf(pattern + n, sizeof(pattern) - n, "{%u}", (unsigned)k);
            k++;
            p += digits;
        } else {
            pattern[n++] = *p++;
        }
    }
    pattern[n] = '\0';
    if (k > 0u) {
        add_translations(pattern, args, k);
    }
    char value[600];
    for (const char *sp = strchr(want, ' '); sp != NULL; sp = strchr(sp + 1, ' ')) {
        const int head = (int)(sp - want);
        snprintf(pattern, sizeof(pattern), "%.*s {0}", head, want);
        args[0] = i18n_arg_str(sp + 1);
        add_translations(pattern, args, 1);
        snprintf(pattern, sizeof(pattern), "{0}%s", sp);
        snprintf(value, sizeof(value), "%.*s", head, want);
        args[0] = i18n_arg_str(value);
        add_translations(pattern, args, 1);
    }
}

/* This label shows `want` in the walk's language (a real one: the catalog, any context). */
static bool shows_translated(const char *shown, const char *want, bool prefix)
{
    find_candidates(want);
    for (size_t i = 0; i < s_cand_count; i++) {
        if (strcmp(shown, s_cand[i]) == 0 || (prefix && strncmp(shown, s_cand[i], strlen(s_cand[i])) == 0)) {
            return true;
        }
    }
    return false;
}

/* en-XA as tools/i18n.py writes it: accented look-alikes, about 40 % longer, in brackets. */
static size_t pseudo_en(const char *s, char *out, size_t cap)
{
    static const char *const from = "AaCcDdEeGgHhIiJjKkLlNnOoRrSsTtUuWwYyZz";
    static const char *const to[] = {
        "\xC3\x85", "\xC3\xA1", "\xC3\x87", "\xC3\xA7", "\xC3\x90", "\xC3\xB0", "\xC3\x89",
        "\xC3\xA9", "\xC4\x9C", "\xC4\x9D", "\xC4\xA4", "\xC4\xA5", "\xC3\x8D", "\xC3\xAD",
        "\xC4\xB4", "\xC4\xB5", "\xC4\xB6", "\xC4\xB7", "\xC4\xB9", "\xC4\xBA", "\xC3\x91",
        "\xC3\xB1", "\xC3\x93", "\xC3\xB3", "\xC5\x94", "\xC5\x95", "\xC5\xA0", "\xC5\xA1",
        "\xC5\xA2", "\xC5\xA3", "\xC3\x9A", "\xC3\xBA", "\xC5\xB4", "\xC5\xB5", "\xC3\x9D",
        "\xC3\xBD", "\xC5\xBD", "\xC5\xBE"};
    size_t n = 0;
    size_t chars = 0;
    char body[512];
    const char *p = s;
    while (*p != '\0' && n + 4 < sizeof(body)) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            body[n++] = p[0];
            body[n++] = p[1];
            p += 2;
            continue;
        }
        if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') {
            memcpy(body + n, p, 3);
            n += 3;
            p += 3;
            continue;
        }
        const char *k = strchr(from, *p);
        if (k != NULL && *p != '\0') {
            const char *r = to[k - from];
            memcpy(body + n, r, strlen(r));
            n += strlen(r);
            p++;
        } else {
            size_t l = 1;
            while ((p[l] & 0xC0) == 0x80) {
                l++;
            }
            memcpy(body + n, p, l);
            n += l;
            p += l;
        }
        chars++;
    }
    body[n] = '\0';
    if (chars == 0) {
        return (size_t)snprintf(out, cap, "%s", s);
    }
    size_t o = (size_t)snprintf(out, cap, "[%s", body);
    for (size_t d = 0; d < (chars * 2 + 4) / 5 && o + 3 < cap; d++) {
        o += (size_t)snprintf(out + o, cap - o, "\xC2\xB7");
    }
    if (o + 1 < cap) {
        out[o++] = ']';
        out[o] = '\0';
    }
    return o;
}

/* The English a test language's text came from, pieces put together included: en-XA's brackets
 * and padding dots dropped and its accents undone, ar-XB's direction marks dropped. */
static void unpseudo(const char *s, char *out, size_t cap)
{
    static const char *const from = "AaCcDdEeGgHhIiJjKkLlNnOoRrSsTtUuWwYyZz";
    static const char *const to[] = {
        "\xC3\x85", "\xC3\xA1", "\xC3\x87", "\xC3\xA7", "\xC3\x90", "\xC3\xB0", "\xC3\x89",
        "\xC3\xA9", "\xC4\x9C", "\xC4\x9D", "\xC4\xA4", "\xC4\xA5", "\xC3\x8D", "\xC3\xAD",
        "\xC4\xB4", "\xC4\xB5", "\xC4\xB6", "\xC4\xB7", "\xC4\xB9", "\xC4\xBA", "\xC3\x91",
        "\xC3\xB1", "\xC3\x93", "\xC3\xB3", "\xC5\x94", "\xC5\x95", "\xC5\xA0", "\xC5\xA1",
        "\xC5\xA2", "\xC5\xA3", "\xC3\x9A", "\xC3\xBA", "\xC5\xB4", "\xC5\xB5", "\xC3\x9D",
        "\xC3\xBD", "\xC5\xBD", "\xC5\xBE"};
    size_t n = 0;
    const char *p = s;
    while (*p != '\0' && n + 1 < cap) {
        if (*p == '[' || *p == ']') {
            p++;
            continue;
        }
        if (strncmp(p, "\xE2\x80\xAE", 3) == 0 || strncmp(p, "\xE2\x80\xAC", 3) == 0) {
            p += 3; /* RLO, PDF */
            continue;
        }
        if (strncmp(p, "\xC2\xB7", 2) == 0) { /* padding dots run up to a "]" */
            const char *q = p;
            while (strncmp(q, "\xC2\xB7", 2) == 0) {
                q += 2;
            }
            if (*q == ']') {
                p = q;
                continue;
            }
        }
        bool mapped = false;
        for (size_t i = 0; i < sizeof(to) / sizeof(to[0]); i++) {
            const size_t l = strlen(to[i]);
            if (strncmp(p, to[i], l) == 0) {
                out[n++] = from[i];
                p += l;
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
}

/* This label shows this English text, in the walk's language. */
static bool shows(const char *shown, const char *english, bool prefix)
{
    const size_t len = strlen(english) - (prefix ? 1u : 0u);
    if (prefix ? strncmp(shown, english, len) == 0 : strcmp(shown, english) == 0) {
        return true;
    }
    if (!s_locale) {
        return false;
    }
    char back[1200];
    unpseudo(shown, back, sizeof(back));
    if (prefix ? strncmp(back, english, len) == 0 : strcmp(back, english) == 0) {
        return true;
    }
    char want[600];
    snprintf(want, sizeof(want), "%.*s", (int)len, english);
    if (shows_translated(shown, want, prefix)) {
        return true;
    }
    char pseudo[1200];
    pseudo_en(want, pseudo, sizeof(pseudo));
    if (prefix) {
        pseudo[strlen(pseudo) > 0 ? strlen(pseudo) - 1u : 0u] = '\0'; /* without its "]" */
        char *dots = strstr(pseudo, "\xC2\xB7");
        if (dots != NULL) {
            *dots = '\0';
        }
        return strncmp(shown, pseudo, strlen(pseudo)) == 0;
    }
    return strcmp(shown, pseudo) == 0;
}

static bool visible(lv_obj_t *o)
{
    for (; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

/* First visible object with this name (several pads and sheets share names). */
static lv_obj_t *find_visible(lv_obj_t *root, const char *name)
{
    if (root == NULL) {
        return NULL; /* a locale walk's missing page: the caller soft-fails */
    }
    const char *n = lv_obj_get_name(root);
    if (n != NULL && strcmp(n, name) == 0 && visible(root)) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = find_visible(lv_obj_get_child(root, (int32_t)i), name);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

static lv_obj_t *by_name(const char *name)
{
    lv_obj_t *o = find_visible(lv_screen_active(), name);
    if (o == NULL && s_locale) {
        o = lv_obj_find_by_name(lv_screen_active(), name); /* hidden, or on another page */
    }
    if (o == NULL) {
        fprintf(stderr, "no visible object named '%s' on the active screen\n", name);
        exit(1);
    }
    return o;
}

/* First visible label showing exactly this text. */
static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if (root == NULL) {
        return NULL; /* a locale walk's missing page: the caller soft-fails */
    }
    /* "Forgot the code?*" matches any label that starts with "Forgot the code?". */
    const size_t len = strlen(text);
    const bool prefix = len > 0u && text[len - 1u] == '*';
    (void)len;
    if (lv_obj_check_type(root, &lv_label_class) && visible(root) &&
        shows(lv_label_get_text(root), text, prefix)) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = find_label(lv_obj_get_child(root, (int32_t)i), text);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

/* First visible label with this anywhere in it (for words that must not be on a screen at
 * all, wherever they sit in a sentence). */
static lv_obj_t *find_label_with(lv_obj_t *root, const char *text)
{
    if (root == NULL) {
        return NULL; /* a locale walk's missing page: the caller soft-fails */
    }
    if (lv_obj_check_type(root, &lv_label_class) && visible(root)) {
        char back[1200];
        const char *shown = lv_label_get_text(root);
        if (s_locale) {
            unpseudo(shown, back, sizeof(back));
            shown = back;
        }
        if (strstr(shown, text) != NULL) {
            return root;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = find_label_with(lv_obj_get_child(root, (int32_t)i), text);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

/* A visible label under root that shows at least half of this text, cut short: LVGL wrote
 * "..." where its room ran out ("Walk test from the Panel Manage..."). A '*' at the end matches
 * as find_label's. */
static lv_obj_t *find_label_shortened(lv_obj_t *root, const char *text)
{
    if (root == NULL) {
        return NULL; /* a locale walk's missing page: the caller soft-fails */
    }
    if (lv_obj_check_type(root, &lv_label_class) && visible(root)) {
        const char *shown = lv_label_get_text(root);
        const size_t len = strlen(shown);
        size_t want = strlen(text);
        if (want > 0u && text[want - 1u] == '*') {
            want--;
        }
        if (len > 3u && strcmp(shown + len - 3u, "...") == 0) {
            const size_t kept = len - 3u < want ? len - 3u : want;
            if (kept > 0u && kept * 2u >= want && strncmp(shown, text, kept) == 0) {
                return root;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = find_label_shortened(lv_obj_get_child(root, (int32_t)i), text);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

/* The status bar shows this text, whole or, where the trouble pill, the icons, the clock,
 * Chime and the gear leave too little room, shortened with "..." (ui_app.c bar_fit). */
static bool bar_shows(const char *text)
{
    lv_obj_t *bar = by_name("bar");
    return find_label(bar, text) != NULL || find_label_shortened(bar, text) != NULL;
}

/* Bring a line into view before a screenshot (long pages: the check's results). */
static void scroll_to_text(const char *viewport, const char *text)
{
    lv_obj_t *o = find_label(by_name(viewport), text);
    if (o == NULL && s_locale) {
        s_soft_fails++;
        return;
    }
    if (o == NULL) {
        fprintf(stderr, "no visible label '%s' to scroll to\n", text);
        exit(1);
    }
    scroll_into_view(o);
    run(50);
}

static void tap_xy(int32_t x, int32_t y)
{
    s_px = x;
    s_py = y;
    s_pressed = true;
    run(80);
    s_pressed = false;
    run(120);
}

/* Whether a finger on the object's centre would land on it: inside the screen and inside every
 * parent (a list clips what it has scrolled away). */
static bool centre_visible(lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t x = (a.x1 + a.x2) / 2;
    const int32_t y = (a.y1 + a.y2) / 2;
    for (lv_obj_t *p = lv_obj_get_parent(obj); p != NULL; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (x < pa.x1 || x > pa.x2 || y < pa.y1 || y > pa.y2) {
            return false;
        }
    }
    return x >= 0 && x < W && y >= 0 && y < H;
}

/* A finger presses only what is on the screen: a control scrolled out of its list (the 4"
 * screen's one-column lists hold less) is brought into view first, as a person would. */
/* lv_obj_scroll_to_view_recursive, but a lookup that only works in English (-Locale) is skipped. */
static void scroll_into_view(lv_obj_t *o)
{
    if (o == NULL && s_locale) {
        s_soft_fails++;
        return;
    }
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
}

static void tap(lv_obj_t *obj)
{
    if (obj == NULL && s_locale) {
        s_soft_fails++; /* -Locale: found only in English */
        return;
    }
    if (!centre_visible(obj)) {
        scroll_into_view(obj);
        run(50);
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    tap_xy((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}

static void tap_name(const char *name)
{
    /* A code pad in Settings takes the whole view on the 4" layout, section tabs included (its
     * keys need the height): to reach another section a person closes it with its X first, and
     * so does the walk. On the 7" the sections stay beside the pad. */
    if (find_visible(lv_screen_active(), name) == NULL && strncmp(name, "set_", 4) == 0 &&
        lv_subject_get_int(&subject_settings_pad) == 1) {
        tap(lv_obj_find_by_name(by_name("settings_pad"), "pad_close"));
        run(300);
    }
    tap(by_name(name));
}

static bool saver_showing(void)
{
    return find_visible(lv_screen_active(), "saver_bg") != NULL;
}

/* The screen saver is (or isn't) showing: the check fails the preview run, and so CI. */
static void expect_saver(bool on, const char *what)
{
    if (saver_showing() != on) {
        fprintf(stderr, "screen saver: %s: expected it %s\n", what, on ? "on" : "off");
        exit(1);
    }
}

/* A check of the walk-through: failing it fails the preview run, and so CI. */
static void expect(bool ok, const char *what)
{
    if (!ok && s_locale) {
        printf("  (in English only) %s\n", what); /* -Locale: the English walk checks it */
        s_soft_fails++;
        return;
    }
    if (!ok) {
        fprintf(stderr, "%s\n", what);
        exit(1);
    }
}

static void beep_cb(void *ctx, ui_beep_t b)
{
    (void)ctx;
    if (b == UI_BEEP_KEY) {
        s_clicks++;
    }
}

/* Drag a finger: put it down, move it in small steps (the panel reads a fresh position
 * every few milliseconds), lift it, and let the list coast to a stop. */
static void drag(int32_t x, int32_t y, int32_t dy)
{
    const int32_t steps = 10;
    s_px = x;
    s_py = y;
    s_pressed = true;
    run(60);
    for (int32_t i = 1; i <= steps; i++) {
        s_py = y + dy * i / steps;
        s_py = s_py < 0 ? 0 : (s_py >= H ? H - 1 : s_py); /* a finger can't leave the glass */
        run(30);
    }
    s_pressed = false;
    run(900); /* the throw animation finishes */
}

/* What a touch at this point would press. */
static lv_obj_t *hit(int32_t x, int32_t y)
{
    lv_point_t p = {x, y};
    return lv_indev_search_obj(lv_screen_active(), &p);
}

/* The first control a finger could reach in a list (a choice, a setting, a section to
 * open): clickable, shown, and not scrolled off the edge. */
static lv_obj_t *first_control(lv_obj_t *list)
{
    lv_area_t la;
    lv_obj_get_coords(list, &la);
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *c = lv_obj_get_child(list, (int32_t)i);
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        const lv_point_t mid = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && visible(c) &&
            lv_area_is_point_on(&la, &mid, 0)) {
            return c;
        }
    }
    return NULL;
}

/* A drag that starts on a control inside a list still scrolls the list - LVGL hands the
 * drag up to the parent. This is the only thing that worked before UI-024 was fixed, so
 * it has to keep working. The caller checks that the control itself didn't fire. */
static void expect_drag_from_control(const char *viewport, const char *what)
{
    lv_obj_t *list = by_name(viewport);
    lv_obj_t *control = first_control(list);
    /* The finger needs room above it to drag up: on a short screen the first row can sit so
     * close to the top edge that the drag leaves the glass at once (the 4" setup lists).
     * Then the next control down is the one to drag from. */
    for (uint32_t i = 0; control != NULL && i < lv_obj_get_child_count(list); i++) {
        lv_area_t ca;
        lv_obj_get_coords(control, &ca);
        if ((ca.y1 + ca.y2) / 2 >= 56) { /* 40 px of scroll after LVGL's 10 px drag threshold */
            break;
        }
        lv_obj_t *next = NULL;
        lv_area_t la;
        lv_obj_get_coords(list, &la);
        for (uint32_t j = (uint32_t)lv_obj_get_index(control) + 1u;
             j < lv_obj_get_child_count(list) && next == NULL; j++) {
            lv_obj_t *c = lv_obj_get_child(list, (int32_t)j);
            lv_area_t a;
            lv_obj_get_coords(c, &a);
            const lv_point_t mid = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && visible(c) &&
                lv_area_is_point_on(&la, &mid, 0)) {
                next = c;
            }
        }
        control = next;
    }
    if (control == NULL) {
        fprintf(stderr, "scrolling: %s: no control in %s to drag from\n", what, viewport);
        exit(1);
    }
    lv_area_t a;
    lv_obj_get_coords(control, &a);
    const int32_t x = (a.x1 + a.x2) / 2;
    const int32_t y = (a.y1 + a.y2) / 2;
    if (hit(x, y) != control) {
        fprintf(stderr, "scrolling: %s: %d,%d isn't on the control after all\n", what,
                (int)x, (int)y);
        exit(1);
    }
    /* 40 px, or as far as the list goes when it holds less than that below the edge (a short
     * list on the 4" screen). */
    const int32_t room = lv_obj_get_scroll_bottom(list);
    if (room <= 0) {
        fprintf(stderr, "scrolling: %s: %s has nothing below the edge, so the check proves "
                        "nothing\n", what, viewport);
        exit(1);
    }
    const int32_t need = room < 40 ? room : 40;
    const int32_t top = lv_obj_get_scroll_y(list);
    drag(x, y, -150);
    if (lv_obj_get_scroll_y(list) - top < need) {
        fprintf(stderr, "scrolling: %s: dragging from a control moved %s %d px (%d px below "
                        "the edge; the finger started at y %d)\n", what, viewport,
                (int)(lv_obj_get_scroll_y(list) - top), (int)room, (int)y);
        exit(1);
    }
}

/* The touch has to land on the list itself, not on a control: that is what keeps the
 * scrolling checks honest, since a button would scroll the list anyway. */
static void expect_on_list(lv_obj_t *list, int32_t x, int32_t y, const char *viewport,
                          const char *what)
{
    lv_obj_t *on = hit(x, y);
    if (on != list) {
        const char *n = on == NULL ? "nothing" : lv_obj_get_name(on);
        fprintf(stderr, "scrolling: %s: a touch at %d,%d presses %s, not %s\n", what,
                (int)x, (int)y, n == NULL ? "an unnamed object" : n, viewport);
        exit(1);
    }
}

/* A finger put down on plain content - a read-only row, an event, a setting's name - and
 * dragged has to scroll the list it is on, in both directions, without making the key
 * click (UI-024). The first check keeps the test honest: the touch must land on the list
 * itself, so it can't be a nearby button doing the work.
 * Failing any of them fails the preview run, and so CI. */
static void expect_drag_scrolls(const char *viewport, lv_obj_t *at, const char *what)
{
    lv_obj_t *list = by_name(viewport);
    lv_area_t a;
    lv_obj_get_coords(at, &a);
    const int32_t x = (a.x1 + a.x2) / 2;
    const int32_t y = (a.y1 + a.y2) / 2;
    expect_on_list(list, x, y, viewport, what);
    if (lv_obj_get_scroll_bottom(list) <= 0) {
        fprintf(stderr, "scrolling: %s: %s has nothing below the edge, so the check "
                        "proves nothing\n", what, viewport);
        exit(1);
    }
    const int32_t clicks = s_clicks;
    const int32_t top = lv_obj_get_scroll_y(list);
    drag(x, y, -150);
    const int32_t moved = lv_obj_get_scroll_y(list);
    if (moved - top < 40) {
        fprintf(stderr, "scrolling: %s: dragging up on %s moved it %d px\n", what,
                viewport, (int)(moved - top));
        exit(1);
    }
    expect_on_list(list, x, y, viewport, what); /* still plain content after scrolling */
    drag(x, y, 150);
    const int32_t back = lv_obj_get_scroll_y(list);
    if (moved - back < 40) {
        fprintf(stderr, "scrolling: %s: dragging back down on %s brought it back %d px\n",
                what, viewport, (int)(moved - back));
        exit(1);
    }
    if (s_clicks != clicks) {
        fprintf(stderr, "scrolling: %s: scrolling %s made %d key clicks\n", what, viewport,
                (int)(s_clicks - clicks));
        exit(1);
    }
}

/* Two things that are both on screen must not sit on top of each other (a toast across the
 * keyboard hides keys). Failing it fails the preview run, and so CI. */
static void expect_clear_of(const char *a_name, const char *b_name, const char *what)
{
    lv_area_t a;
    lv_area_t b;
    lv_obj_get_coords(by_name(a_name), &a);
    lv_obj_get_coords(by_name(b_name), &b);
    if (a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2) {
        fprintf(stderr, "layering: %s: %s (%d,%d..%d,%d) covers part of %s (%d,%d..%d,%d)\n",
                what, a_name, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, b_name, (int)b.x1,
                (int)b.y1, (int)b.x2, (int)b.y2);
        exit(1);
    }
}

/* Every key of the open keyboard has to be reachable: a touch anywhere on it must land on the
 * keyboard, not on something floating over it (visible or not). */
static void expect_keyboard_reachable(const char *what)
{
    lv_obj_t *kb = by_name("sheet_kb");
    lv_area_t a;
    lv_obj_get_coords(kb, &a);
    for (int32_t y = a.y1 + 16; y < a.y2 - 16; y += 32) {
        for (int32_t x = a.x1 + 16; x < a.x2 - 16; x += 32) {
            lv_obj_t *on = hit(x, y);
            if (on != kb) {
                const char *n = on == NULL ? "nothing" : lv_obj_get_name(on);
                fprintf(stderr, "layering: %s: a touch at %d,%d reaches %s, not the keyboard\n",
                        what, (int)x, (int)y, n == NULL ? "an unnamed object" : n);
                exit(1);
            }
        }
    }
}

/* Touch the screen saver away (a tap on the main screen could press something). */
static void wake(void)
{
    if (saver_showing()) {
        tap_xy(W / 2, H / 2);
    }
}

/* Tap the control that shows this text: in an open sheet first, then the settings page or
 * setup step, then anywhere (so "Zones" means the settings row, not the rail). */
static void tap_text(const char *text);
/* Settings > About > Installer (BL-189): the Installer line is a row at the end of About. */
static void open_installer(void)
{
    tap_name("set_about");
    tap_text("Installer");
}

static void tap_text(const char *text)
{
    static const char *const roots[] = {"pick_list", "text_card", "dialog_card", "form_body",
                                        "page_body", "wiz_body", "settings_page"};
    lv_obj_t *o = NULL;
    lv_obj_t *root = NULL;
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]) && o == NULL; i++) {
        root = find_visible(lv_screen_active(), roots[i]);
        if (root != NULL) {
            o = find_label(root, text);
        }
    }
    if (o == NULL) {
        root = lv_screen_active();
        o = find_label(root, text);
    }
    if (o == NULL && s_locale) {
        printf("  (in English only) no label '%s' to tap\n", text);
        s_soft_fails++;
        return;
    }
    if (o == NULL) {
        fprintf(stderr, "no visible label '%s' on the active screen\n", text);
        exit(1);
    }
    /* Up to the first clickable ancestor, but no further than the row inside the root. */
    while (lv_obj_get_parent(o) != root && lv_obj_get_parent(o) != NULL &&
           !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        o = lv_obj_get_parent(o);
    }
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        lv_obj_t *control = find_visible(o, "toggle"); /* a switch row */
        if (control == NULL) {
            fprintf(stderr, "nothing to tap for '%s'\n", text);
            exit(1);
        }
        o = control;
    }
    scroll_into_view(o);
    run(50);
    tap(o);
}

/* Inside something that can be tapped, below root: a row's title, not a heading. */
static bool in_link(lv_obj_t *o, lv_obj_t *root)
{
    for (lv_obj_t *p = lv_obj_get_parent(o); p != NULL && p != root; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
            return true;
        }
    }
    return false;
}

static lv_obj_t *find_link_in(lv_obj_t *root, lv_obj_t *o, const char *text)
{
    if (o == NULL) {
        return NULL; /* a locale walk's missing page: the caller soft-fails */
    }
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) &&
        shows(lv_label_get_text(o), text, false) && in_link(o, root)) {
        return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *f = find_link_in(root, lv_obj_get_child(o, (int32_t)i), text);
        if (f != NULL) {
            return f;
        }
    }
    return NULL;
}

/* Tap the row with this title on the open page: tap_text takes the first label with the words,
 * and "Language" heads Time and language's language rows as well as naming one of them. */
static void tap_link(const char *text)
{
    lv_obj_t *body = by_name("page_body");
    lv_obj_t *o = find_link_in(body, body, text);
    if (o == NULL && s_locale) {
        printf("  (in English only) no row '%s' to tap\n", text);
        s_soft_fails++;
        return;
    }
    if (o == NULL) {
        fprintf(stderr, "no row '%s' to tap on the page\n", text);
        exit(1);
    }
    while (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        o = lv_obj_get_parent(o);
    }
    scroll_into_view(o);
    run(50);
    tap(o);
}

/* Time and language's first row says what the clock says; it opens the setter only where a time
 * set by hand is kept (RTC-008). */
static lv_obj_t *clock_row(void)
{
    return lv_obj_get_child(by_name("page_body"), 0);
}

static bool clock_row_opens(void)
{
    lv_obj_t *row = clock_row();
    return row != NULL && lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE);
}

/* Type into the open text sheet (the keyboard itself is shown in the screenshots). */
static void sheet_text(const char *text)
{
    lv_obj_t *ta = by_name("sheet_text");
    /* UI-020: the cursor of an open (focused) text sheet never blinks. LVGL's theme animates
     * it in the focused state, which is why this is asked with the sheet open. */
    expect(lv_obj_get_style_anim_duration(ta, LV_PART_CURSOR) == 0,
           "the text cursor does not blink");
    lv_textarea_set_text(ta, text);
    run(100);
}

/* Keys of the code pad: 1..9, delete, 0, check. */
static void key(char k)
{
    lv_obj_t *keys = by_name("pad_keys");
    int idx = (k == '0') ? 10 : (k == 'd') ? 9 : (k == 'k') ? 11 : (k - '1');
    tap(lv_obj_get_child(keys, idx));
}

/* The setup is on step 'step' (the checks fail the preview run, and so CI). */
static void expect_step(int32_t step, const char *what)
{
    const int32_t now = lv_subject_get_int(&subject_wiz_step);
    if (now != step) {
        fprintf(stderr, "setup: %s: on step %d, expected %d\n", what, (int)now, (int)step);
        exit(1);
    }
}

/* With more than one language the setup opens on the language (docs/features/37 I18N-012): the
 * test-language walks step past it; the English walk has one language and no such step. */
static void past_the_language(void)
{
#ifdef ALARM_PREVIEW_LOCALE
    expect_step(0, "the language");
    expect(find_label(by_name("wiz_body"), "Region") != NULL, "setup: no language step first");
    tap_name("wiz_next");
    run(300);
#endif
}

/* The clock's calendar and rollers (BL-131 finding 3, docs/features/35): a tap on a day of the
 * calendar on screen, and a roller moved to a row as a finger leaves it (it reports the change). */
#if ALARM_LV_BUDGET_KB == 0
static void tap_day(const char *day)
{
    lv_obj_t *label = find_label(by_name("cal_days"), day);
    expect(label != NULL, "no such day in the calendar");
    if (label != NULL) {
        tap(lv_obj_get_parent(label));
        run(60);
    }
}

/* One of the rollers inside the time_rollers called parent (Night hours has two of them). */
static void roll_in(const char *parent, const char *name, uint32_t row)
{
    lv_obj_t *roller = lv_obj_find_by_name(by_name(parent), name);
    expect(roller != NULL, "no such roller");
    if (roller == NULL) {
        return;
    }
    expect(lv_obj_get_style_anim_duration(roller, LV_PART_MAIN) == 0, "a roller animates to its value");
    lv_roller_set_selected(roller, row, LV_ANIM_OFF);
    lv_obj_send_event(roller, LV_EVENT_VALUE_CHANGED, NULL);
    run(60);
}

static void roll_to(const char *name, uint32_t row)
{
    lv_obj_t *roller = by_name(name);
    /* UI-020: a roller never slides to a value; it snaps where it is left */
    expect(lv_obj_get_style_anim_duration(roller, LV_PART_MAIN) == 0, "a roller animates to its value");
    lv_roller_set_selected(roller, row, LV_ANIM_OFF);
    lv_obj_send_event(roller, LV_EVENT_VALUE_CHANGED, NULL);
    run(60);
}
#endif

static void type(const char *keys)
{
    for (const char *k = keys; *k != '\0'; k++) {
        key(*k);
    }
}

#if ALARM_LV_BUDGET_KB == 0
/* A settings row's own label, found by the row's title on the page: its value ("value_label") or
 * its sub-line ("detail_label"); "" when it shows none. */
static const char *row_label(const char *title, const char *part)
{
    lv_obj_t *body = by_name("page_body");
    lv_obj_t *row = find_label(body, title);
    if (row == NULL && s_locale) {
        printf("  (in English only) no row '%s' on the page\n", title);
        s_soft_fails++;
        return "";
    }
    if (row == NULL) {
        fprintf(stderr, "no row '%s' on the page\n", title);
        exit(1);
    }
    while (lv_obj_get_parent(row) != body) {
        row = lv_obj_get_parent(row);
    }
    lv_obj_t *l = find_visible(row, part);
    return l != NULL ? lv_label_get_text(l) : "";
}
#endif

#if ALARM_SIM_ZWAVE && ALARM_LV_BUDGET_KB == 0
static void shot(const char *name);

/* The Z-Wave world's own time, in steps longer than run()'s: joining and setting a device up take
 * simulated minutes. The UI keeps drawing between them. */
static void zw_wait(bool (*done)(void), uint32_t ms, const char *what)
{
    for (uint32_t t = 0; !done() && t < ms; t += 100u) {
        s_ms += 100u;
        host_panel_advance(100u);
        lv_timer_handler();
    }
    expect(done(), what);
}

/* A label's text colour is this tone (the Z-Wave page's amber warnings). */
static bool label_tone(const char *text, lv_color_t tone)
{
    lv_obj_t *l = find_label(by_name("page_body"), text);
    return l != NULL && lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), tone);
}

/* Z7b: waits for a word or phrase to appear anywhere on screen - the add/remove pages rebuild
 * themselves as the simulated network answers, on their own schedule, not run()'s. */
static const char *s_wait_text;
static bool text_visible(void)
{
    return find_label_with(lv_screen_active(), s_wait_text) != NULL;
}
static void wait_for_text(const char *text, uint32_t ms, const char *what)
{
    s_wait_text = text;
    zw_wait(text_visible, ms, what);
}


/* The conditions the walk waits for, as the page shows them. */
static bool check_in_taken(void)
{
    return find_label_with(lv_screen_active(), "when it next wakes up") == NULL;
}

static bool region_long_range(void)
{
    return strcmp(row_label("Region", "value_label"), "United States with Long Range") == 0;
}

static bool radio_state_working(void)
{
    return strcmp(row_label("State", "value_label"), "Working") == 0;
}

/* A sleeping sensor's default wake-up is 12 simulated hours (fake_zdev_init): zw_wait's 100 ms
 * steps would take forever to reach it, so this steps in whole minutes instead - still fine for a
 * coarse "it went quiet" wait, unlike zw_wait's protocol-timed ones. A touch every step: without
 * one, LVGL's own inactivity timer (unrelated to host_panel's simulated clock) would drop the
 * screen to its saver, and installer mode's own keepalive (ui_settings_tick, sent only within 5 s
 * of a real touch) would time out server-side and force Settings back to the installer menu -
 * both well before a wait of hours is over. The device page's "Security" value row is never
 * clickable, so tapping it does nothing else, on either its reachable or unreachable layout; it
 * is looked for on the page only, as the rail's first place is called Security too. */
static void hour_wait(bool (*done)(void), uint32_t hours, const char *what)
{
    const uint32_t STEP_MS = 20000u; /* under any plausible keepalive/saver timeout */
    const uint32_t steps = hours * 3600000u / STEP_MS;
    for (uint32_t i = 0; !done() && i < steps; i++) {
        s_ms += STEP_MS;
        host_panel_advance(STEP_MS);
        lv_timer_handler();
        lv_obj_t *page = lv_obj_find_by_name(lv_screen_active(), "page_body");
        lv_obj_t *safe = page != NULL ? find_label(page, "Security") : NULL;
        if (safe != NULL) {
            tap(safe);
        }
    }
    expect(done(), what);
}

/* docs/features/36 Z7a: Installer > Z-Wave - the installer menu's row (screens.md §2) and the
 * Z-Wave page (§3), with a secure door sensor that is zone 1's sensor and an unencrypted motion
 * sensor in no zone, added by the simulated world's own buttons (the add pages are Z7b). The
 * world runs only from here on (host_panel_zwave_later), so every shot before is a panel without
 * Z-Wave - the same shots as the default preview's. */
static void walk_zwave(void)
{
    printf("Z-Wave\n");
    host_panel_zwave_begin();
    zw_wait(host_panel_zwave_link_up, 10000u, "Z-Wave: the radio did not come up");
    host_panel_zwave_add_secure();
    zw_wait(host_panel_zwave_pin_pending, 30000u,
            "Z-Wave: the door sensor did not ask for its PIN");
    host_panel_zwave_pin();
    zw_wait(host_panel_zwave_door_ready, 300000u, "Z-Wave: the door sensor was not set up");
    host_panel_zwave_add_insecure();
    zw_wait(host_panel_zwave_motion_ready, 300000u, "Z-Wave: the motion sensor was not set up");
    expect(host_panel_zwave_bind_door(1), "Z-Wave: the door sensor did not become zone 1's sensor");
    run(1000);

    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    lv_obj_t *body = by_name("page_body");
    expect(find_label(body, "Z-Wave") != NULL, "ZW-019: the installer menu has no Z-Wave row");
    expect(find_label(body, "Sensors on the panel's Z-Wave radio") != NULL,
           "the Z-Wave row does not say what it is");
    expect(find_label(body, "2 devices") != NULL, "the Z-Wave row does not count its devices");
    shot("zwave_installer_menu");

    tap_text("Z-Wave");
    run(1500);
    body = by_name("page_body");
    expect(find_label(lv_screen_active(), "Sensors paired to the panel's Z-Wave radio") != NULL,
           "the Z-Wave page has no subtitle");
    expect(find_label(body, "Radio · Working") != NULL, "the Z-Wave page's radio isn't working");
    expect(find_label(body, "United States · 2 devices · all heard") != NULL,
           "the radio row does not give the region, the devices and whether all are heard");
    expect(find_label(body, "Add a device") != NULL && find_label(body, "Remove a device") != NULL,
           "the installer can't add or remove devices");
    expect(find_label(body, "Front Door") != NULL,
           "the door sensor is not named after its zone (screens.md §3)");
    expect(find_label(body, "Zooz ZSE41 · Secure · Battery 90 %") != NULL,
           "the door sensor's line lacks its model, security or battery");
    expect(find_label(body, "Zone 1") != NULL, "the door sensor's row does not name its zone");
    expect(find_label(body, "Zooz ZSE18 motion sensor") != NULL,
           "a device in no zone is not named after its model");
    expect(find_label(body, "Not in a zone") != NULL,
           "the motion sensor's row says it is in a zone");
    expect(label_tone("Not encrypted", COLOR_AMBER),
           "an unencrypted device is not marked amber (screens.md §3)");
    expect(find_label_with(lv_screen_active(), "C0FFEE") == NULL &&
               find_label_with(lv_screen_active(), "ode ") == NULL,
           "ZW-019: the Z-Wave page shows a home ID or a node ID");
    /* UI-020, UI-023: a second later the words are the same words, nothing redrawn in between. */
    lv_obj_t *line = find_label(body, "United States · 2 devices · all heard");
    run(2500);
    expect(find_label(body, "United States · 2 devices · all heard") == line,
           "the radio row was made again although nothing changed");
    shot("zwave_page");

    /* the page's end: its last row whole */
    lv_obj_t *reset = find_label(body, "Reset the Z-Wave network");
    expect(reset != NULL, "the installer can't reset the Z-Wave network");
    scroll_into_view(lv_obj_get_parent(lv_obj_get_parent(reset)));
    run(300);
    expect(find_label(body, "United States") != NULL, "the region is not in words");
    expect(find_label(body, "Every hour") != NULL, "the check-in for new devices is not in words");
    shot("zwave_page_more");

    /* The check-in for new devices: a choice list, answered through the backend (no shot). */
    tap_text("Check-in for new devices");
    run(300);
    expect(find_label(by_name("pick_list"), "Every 12 hours") != NULL,
           "the check-in choices are missing");
    tap_text("Every 4 hours");
    run(1200);
    expect(find_label(body, "Every 4 hours") != NULL,
           "the check-in for new devices did not change");

    /* Z7b, screens.md §4: adding a device, end to end - a secure one with its PIN, then a plain
     * one with no security at all. Both spares are still in radio range but unjoined (the door
     * and motion above were joined before this walk's own shots begin). */
    tap_text("Add a device");
    run(300);
    expect(find_label(lv_screen_active(), "Add a Z-Wave device") != NULL,
           "the add page has no title");
    expect(find_label(lv_screen_active(), "Keep listening") != NULL &&
               find_label(lv_screen_active(), "Stop") != NULL,
           "the listening page has no Keep listening or Stop button");
    /* "Listening for the device" (a checklist row) is always there; the subtitle "Listening ·
     * N s left" only once the radio really is (screens.md §4), a beat after "Starting". */
    wait_for_text("Listening ·", 10000u, "Z-Wave: the add page never started listening");
    shot("zwave_add_listening");

    host_panel_zwave_press_spare_secure();
    wait_for_text("Secure connection", 20000u, "Z-Wave: the spare sensor did not ask to be secured");
    expect(find_label(lv_screen_active(), "With its PIN") != NULL,
           "the secure connection question has no With its PIN toggle");
    /* ZW-029: the time to decide is a live "Time left" row, not words frozen when built */
    expect(find_label(lv_screen_active(), "Time left") != NULL &&
               find_label_with(lv_screen_active(), "to decide") == NULL,
           "ZW-029: the secure connection page has no live Time left row");
    shot("zwave_add_security");

    tap_text("Allow");
    run(300);
    wait_for_text("Enter its PIN", 10000u, "Z-Wave: the PIN page never came up");
    /* ZW-005: the way out of the PIN says what it does */
    expect(find_label(lv_screen_active(), "Add it without encryption") != NULL &&
               find_label(lv_screen_active(), "Time left") != NULL,
           "ZW-005: the PIN page's way out isn't 'Add it without encryption'");
    shot("zwave_add_pin");

    {
        char pin[6];
        snprintf(pin, sizeof(pin), "%05u", (unsigned)host_panel_zwave_spare_secure_pin());
        tap_text("Enter the PIN");
        run(300);
        sheet_text(pin);
        tap_name("sheet_done");
    }
    /* "Reading what it can do · Step n of m" (screens.md §4 07): its detail only once the
     * interview is actually counting steps, unlike the row's own fixed title. */
    wait_for_text("Step ", 60000u, "Z-Wave: the spare sensor's interview never started");
    shot("zwave_add_progress");
    wait_for_text("Use it for", 300000u, "Z-Wave: the spare sensor was not added");
    expect(find_label_with(lv_screen_active(), "Encrypted") != NULL,
           "a securely added device's results page does not say it is encrypted");
    shot("zwave_add_done");

    tap_text("Not now: keep it without a zone");
    run(300);

    tap_text("Add a device");
    run(300);
    host_panel_zwave_press_spare_insecure();
    wait_for_text("Use it for", 60000u, "Z-Wave: the insecure spare sensor was not added");
    expect(find_label_with(lv_screen_active(), "Not encrypted") != NULL,
           "a plainly added device's results page does not say it is unencrypted");
    shot("zwave_add_insecure_done");

    /* screens.md §5: the source picker's Z-Wave entry, and picking a sensor - the door (already
     * bound to zone 1) opened and closed while the picker is open, then the spare just added
     * becomes a new zone's sensor. */
    tap_name("page_back"); /* the add page (still showing the insecure result) -> the Z-Wave page */
    run(300);
    tap_name("page_back"); /* the Z-Wave page -> the installer menu */
    run(300);
    tap_text("Zones");
    run(300);
    tap_text("Add a zone");
    run(300);
    expect(find_label(lv_screen_active(), "Z-Wave sensor") != NULL,
           "the source picker has no Z-Wave sensor choice");
    shot("zwave_source_picker");

    tap_text("Z-Wave sensor");
    run(300);
    expect(find_label(lv_screen_active(), "Add a new Z-Wave device") != NULL,
           "the Z-Wave picker has no 'Add a new Z-Wave device' row");
    host_panel_zwave_door(true);
    run(1200);
    host_panel_zwave_door(false);
    run(1200);
    shot("zwave_pick_sensor");

    tap_text("Zooz ZSE43 tilt and shock sensor");
    run(500);
    expect(find_label_with(lv_screen_active(), "Z-Wave") != NULL,
           "the new zone's Source does not name Z-Wave");
    expect(find_label(lv_screen_active(), "Z-Wave device") != NULL,
           "the zone page has no Z-Wave device row");
    shot("zwave_zone_page");

    tap_text("Z-Wave device");
    run(300);
    expect(find_label(lv_screen_active(), "90 %") != NULL,
           "a battery device's page does not show its battery level");
    expect(strcmp(row_label("Signal", "value_label"), "Good") == 0,
           "ZW-040: the device page has no Signal row saying Good");
    shot("zwave_device_page");

    /* screens.md §6: a sleeping device's own check-in, chosen on its page - it takes it when it
     * next wakes up, and until then the row says so, words in place (UI-020, UI-023). The tilt
     * sensor joined after the check-in for new devices became 4 hours. */
    expect(strcmp(row_label("Checks in", "value_label"), "Every 4 hours") == 0,
           "ZW-011: the tilt sensor does not check in as new devices do");
    tap_text("Checks in");
    run(300);
    {
        lv_obj_t *list = by_name("pick_list");
        expect(find_label(list, "Every hour") != NULL && find_label(list, "Every 4 hours") != NULL &&
                   find_label(list, "Every 12 hours") != NULL,
               "a device's check-in choices are missing");
        expect(find_label(list, "Never") == NULL, "a device's check-in offers Never");
    }
    tap_text("Every 12 hours");
    run(300);
    expect(strcmp(row_label("Checks in", "detail_label"),
                  "Changes to every 12 hours when it next wakes up") == 0,
           "the device page does not say its check-in changes when it next wakes up");
    expect(strcmp(row_label("Checks in", "value_label"), "Every 4 hours") == 0,
           "the check-in changed before the device took it");
    lv_obj_t *check_in_row = find_label(by_name("page_body"), "Checks in");
    run(2500);
    expect(find_label(by_name("page_body"), "Checks in") == check_in_row,
           "the device page was made again although nothing changed");
    shot("zwave_device_check_in");
    /* It wakes by itself at its 4-hour check-in and takes the new one: the line goes, and the
     * value follows, in place. */
    hour_wait(check_in_taken, 5u, "ZW-011: the tilt sensor never took its new check-in");
    expect(strcmp(row_label("Checks in", "value_label"), "Every 12 hours") == 0,
           "ZW-011: the device page does not show the check-in the device took");
    expect(find_label(by_name("page_body"), "Checks in") == check_in_row,
           "the device page was made again for its check-in, not its words changed");

    /* screens.md §6/§7: a sensor that misses its check-ins, then removed - its zone shows CHECK.
     * Back to the installer menu (device page -> zone page -> zones list -> menu), staying in
     * installer mode: going Home would end it (ui_installer_leave, "arming is on again"). */
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_text("Z-Wave");
    run(300);
    tap_text("Front Door");
    run(300);
    /* BL-152 (ZW-029): a sleeper set up again waits calmly for its next wake-up - it works on as
     * before, so neither its page nor its list row warns "Wake it to finish setting it up" */
    scroll_to_text("page_body", "Set it up again");
    tap_text("Set it up again");
    run(300);
    expect(find_label_with(lv_screen_active(), "It's read again at its next wake-up. Its zones") !=
               NULL,
           "BL-152: a sleeper's confirmation doesn't say it is read at its next wake-up");
    tap_text("Set it up again");
    run(1500);
    lv_obj_t *waits = find_label_with(lv_screen_active(), "It's read again at its next wake-up.");
    expect(waits != NULL, "BL-152: a sleeper read again doesn't say it waits for its wake-up");
    if (waits != NULL) {
        scroll_into_view(waits);
        run(50);
    }
    expect(find_label_with(lv_screen_active(), "Wake it to finish setting it up") == NULL,
           "BL-152: a sleeper read again is told to finish its setup");
    shot("zwave_device_setup_again_sleeper");
    tap_name("page_back");
    run(300);
    expect(find_label_with(lv_screen_active(), "Read again at its next wake-up") != NULL,
           "BL-152: the device list doesn't say the sleeper is read at its next wake-up");
    tap_text("Front Door");
    run(300);
    host_panel_zwave_sleep_miss(true);
    /* One missed 12 h wake-up plus the grace (network.md §8). */
    hour_wait(host_panel_zwave_door_unreachable, 13u, "Z-Wave: the door sensor never went quiet");
    run(1500); /* the open device page notices and rebuilds on its own 1 s tick */
    expect(find_label_with(lv_screen_active(), "missed its check-ins") != NULL,
           "the unreachable device page does not say it missed its check-ins");
    expect(find_label(lv_screen_active(), "Remove it: it's reset, broken or gone") != NULL,
           "the unreachable device page has no reset-broken-or-gone removal");
    shot("zwave_device_unreachable");

    tap_text("Remove this device");
    run(300);
    expect(find_label(lv_screen_active(), "Remove Front Door") != NULL,
           "removing from a device page does not name the device");
    host_panel_zwave_press_door_remove();
    wait_for_text("Removed", 20000u, "Z-Wave: the door sensor was not removed");
    expect(find_label_with(lv_screen_active(), "shows CHECK") != NULL,
           "the removal result does not say the zone shows CHECK");
    shot("zwave_remove_result");

    tap_text("Done");
    run(300);

    /* screens.md §8: region and the radio's details. */
    tap_text("Region");
    run(300);
    expect(find_label(lv_screen_active(), "United States") != NULL, "the region list is missing");
    /* ZW-045: United States with Long Range keeps the classic band: no warning on the page */
    expect(find_label(by_name("page_body"), "Devices keep working") != NULL,
           "ZW-045: the same-band region does not say the devices keep working");
    expect(find_label_with(by_name("page_body"), "must be added again") == NULL,
           "ZW-045: the region page warns of a change that stops no device");
    shot("zwave_region");

    /* ZW-018: a new region restarts the radio, which comes back working in it. */
    tap_text("United States with Long Range");
    run(300);
    expect(find_label(lv_screen_active(),
                      "Change the region to United States with Long Range?") != NULL,
           "choosing a region does not ask once more");
    expect(find_label_with(lv_screen_active(), "devices keep working. The Z-Wave radio restarts") !=
               NULL,
           "ZW-045: the same-band confirmation does not say the devices keep working");
    shot("zwave_region_same_band");
    host_panel_zwave_offer_region(0x00u); /* Europe too, once the radio restarts */
    tap_text("Change it"); /* no code: nothing stops (ZW-045) */
    run(300);
    zw_wait(region_long_range, 20000u, "Z-Wave: the region did not change");
    wait_for_text("Radio · Working", 20000u,
                  "ZW-018: the radio did not come back after the region change");

    /* ZW-045: Europe is another band: the page and the confirmation count the devices that stop
     * and say their zones show CHECK; then the installer code, as for the network reset (a
     * master code is refused); closing the pad changes nothing. */
    tap_text("Region");
    run(300);
    expect(find_label_with(by_name("page_body"), "devices stop working") != NULL,
           "ZW-045: the region on another band does not count the devices that stop");
    expect(find_label_with(by_name("page_body"), "zones show CHECK") != NULL,
           "ZW-045: the region page does not say the zones show CHECK");
    shot("zwave_region_band");
    tap_text("Europe");
    run(300);
    expect(find_label_with(lv_screen_active(), "devices stop working. Each must be added again") !=
               NULL,
           "ZW-045: the band change's confirmation does not count the devices that stop");
    shot("zwave_region_drop_confirm");
    tap_text("Change it");
    run(300);
    expect(find_label(lv_screen_active(), "Confirm the region") != NULL,
           "ZW-045: a region change that stops devices did not ask for the installer code");
    shot("zwave_region_code");
    type("123456k"); /* the master code */
    run(300);
    expect(find_label(lv_screen_active(), "Wrong code, try again") != NULL,
           "ZW-045: a region change that stops devices accepted a master code");
    tap(lv_obj_find_by_name(by_name("settings_pad"), "pad_close"));
    run(300);
    tap_name("page_back"); /* the region page -> the Z-Wave page */
    run(300);
    expect(region_long_range(), "ZW-045: closing the code pad changed the region");

    scroll_to_text("page_body", "Reset the Z-Wave network");
    /* ZW-019: the longest region name must leave the help text on one line. The real
     * ZAC93 defaults to US with Long Range; the shorter US value hid this on the PC. */
    lv_obj_t *region_help = find_label(by_name("page_body"), "Match your radio");
    expect(region_help != NULL, "the region help is missing");
    lv_obj_update_layout(region_help);
    expect(lv_obj_get_height(region_help) <=
               lv_font_get_line_height(lv_obj_get_style_text_font(region_help, LV_PART_MAIN)),
           "the Region help wraps beside United States with Long Range");
    tap_text("Reset the Z-Wave network");
    run(300);
    expect(find_label(lv_screen_active(), "Reset the Z-Wave network?") != NULL,
           "the reset dialog never opened");
    shot("zwave_reset_confirm");
    /* ZW-013: only the installer code may confirm the second, plain question - a master code is
     * refused exactly like a wrong one. */
    tap_text("Reset the network");
    run(300);
    expect(find_label(lv_screen_active(), "Confirm the reset") != NULL,
           "the reset's second confirmation did not ask for the code again");
    type("123456k"); /* the master code */
    run(300);
    expect(find_label(lv_screen_active(), "Wrong code, try again") != NULL,
           "the Z-Wave network reset accepted a master code");
    type("69420k"); /* the installer code */
    run(300);
    expect(find_label_with(lv_screen_active(), "No devices") != NULL,
           "the Z-Wave network was not reset by the installer code");
    /* the reset restarts the radio too: it comes back working, with a network of its own */
    run(3000);
    wait_for_text("Radio · Working", 20000u, "ZW-018: the radio did not come back after the reset");

    tap_text("Radio · Working");
    run(300);
    expect(find_label(lv_screen_active(), "Radio software") != NULL,
           "the radio details page is missing its own row");
    expect(radio_state_working(), "ZW-018: the radio details say it isn't working after the reset");
    /* screens.md §8: "Restart the radio" asks once, then its State row follows it in place. */
    tap_text("Restart the radio");
    run(300);
    expect(find_label(lv_screen_active(), "Restart the Z-Wave radio?") != NULL,
           "restarting the radio does not ask once more");
    tap_text("Restart");
    run(300);
    expect(strcmp(row_label("State", "value_label"), "Starting") == 0,
           "the radio details do not say the radio is starting");
    zw_wait(radio_state_working, 20000u, "the radio details never said Working after the restart");
    expect(find_label(lv_screen_active(), "Background noise") != NULL,
           "ZW-024: the radio details have no Background noise row");
    shot("zwave_radio_details");

    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}
#endif

#if ALARM_SIM_ZWAVE && ALARM_LV_BUDGET_KB == 0
/* Simulated minutes in 1 s steps, the display kept active (no saver, installer mode kept alive by
 * its own keepalive, as someone watching the page would). */
static void zw_wait_long(bool (*done)(void), uint32_t ms, const char *what)
{
    for (uint32_t t = 0; !done() && t < ms; t += 1000u) {
        s_ms += 1000u;
        host_panel_advance(1000u);
        lv_display_trigger_activity(NULL);
        lv_timer_handler();
    }
    expect(done(), what);
}

static bool visible_text(const char *text)
{
    return find_label(lv_screen_active(), text) != NULL;
}

/* A line that a person can see now: a substring match, on screen and inside its scrolled lists
 * (find_label_with also finds a line scrolled out of sight). */
static bool visible_text_with(const char *text)
{
    lv_obj_t *o = find_label_with(lv_screen_active(), text);
    return o != NULL && centre_visible(o);
}

static bool stray_setup_stopped(void)
{
    return visible_text("It stopped answering before its setup finished. Keep it close to the "
                        "panel, check its power, then try again.");
}

static bool device_setup_stopped(void)
{
    return find_label_with(lv_screen_active(), "etup stopped") != NULL;
}

static bool stray_gone(void)
{
    const uint16_t n = host_panel_zwave_stray_node();
    return !host_panel_zwave_in_panel(n) && !host_panel_zwave_in_radio(n);
}

static bool keypad_heard_state(void)
{
    return host_panel_zwave_keypad_indicators(NULL) > 0u;
}

static bool clean_ended(void)
{
    return find_label(lv_screen_active(), "Checking that it no longer answers...") == NULL;
}

/* ZW-029, ZW-028 (docs/features/36 screens.md §4, §6, §7): what the real panel met - a device
 * that joined but never finished its setup - and a keypad's own sounds and lights. The stray
 * device joins with an empty node information and hears nothing after: its security is "not
 * confirmed", its setup stops (it didn't answer - never "went to sleep": it is woken by the
 * panel's call), "Stop and finish later" is not "Added", its setup is tried again; a normal
 * removal stopped and opened again listens afresh (it had shown the old "Nothing was removed");
 * then removed as reset, broken or gone - out of the radio's own list too. The keypad joins
 * securely with its PIN, on batteries (never "Mains powered"), and its sounds and lights are set
 * from its page; the outcomes of a removal as failed are said on its page. Appended after every
 * other shot of the -ZWave walk. */
static void walk_zwave_recovery(void)
{
    printf("Z-Wave: recovering a device that never finished joining; a keypad's feedback\n");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Z-Wave");
    run(500);

    /* the stray device: joined, then silent */
    tap_text("Add a device");
    run(300);
    wait_for_text("Listening ·", 10000u, "Z-Wave: the add page never started listening");
    host_panel_zwave_press_stray();
    zw_wait_long(stray_setup_stopped, 3600000u,
                 "ZW-029: a device that stopped answering never said its setup stopped");
    lv_obj_t *asleep = find_label_with(lv_screen_active(), "It went to sleep");
    expect(asleep == NULL || lv_obj_has_flag(asleep, LV_OBJ_FLAG_HIDDEN),
           "ZW-029: a device woken by the panel's call is said to have gone to sleep");
    expect(visible_text("Try again") && visible_text("Stop and finish later"),
           "ZW-029: a stopped setup offers no Try again or Stop and finish later");
    expect(strcmp(row_label("Secured", "detail_label"), "Not confirmed: its setup didn't finish") ==
               0,
           "ZW-029: an unanswered key exchange after an empty node information is not 'not "
           "confirmed'");
    expect(strcmp(row_label("Reading what it can do", "detail_label"),
                  "Stopped: it didn't answer") == 0,
           "ZW-019: a stopped setup's checklist row still says 'Step n of m'");
    shot("zwave_add_setup_stopped");

    tap_text("Stop and finish later");
    run(500);
    expect(visible_text("Setup not finished"),
           "ZW-029: Stop and finish later reports the device as added");
    expect(find_label_with(lv_screen_active(), "Not confirmed") != NULL,
           "ZW-029: the paused setup does not say its security isn't confirmed");
    shot("zwave_add_setup_paused");

    tap_text("Open it");
    run(500);
    expect(device_setup_stopped(), "ZW-029: the device page does not say its setup stopped");
    expect(strcmp(row_label("Security", "value_label"), "Not confirmed: its setup didn't finish") ==
               0,
           "ZW-029: the device page calls an unconfirmed pairing 'Not encrypted'");
    expect(strcmp(row_label("Battery", "value_label"), "Unknown") == 0,
           "a device whose power is not known is not 'Unknown'");
    expect(visible_text("Try its setup again"), "ZW-029: the device page has no Try its setup again");
    expect(visible_text("Remove it: it's reset, broken or gone") &&
               visible_text("Remove this device"),
           "ZW-029: the recovery removal is hidden until the device is unreachable");
    shot("zwave_device_setup_stopped");
    scroll_to_text("page_body", "The radio checks it no longer answers first");
    shot("zwave_device_remove_choices");

    scroll_to_text("page_body", "Try its setup again");
    tap_text("Try its setup again");
    run(500);
    expect(find_label_with(lv_screen_active(), "Trying its setup again: step") != NULL,
           "ZW-029: trying its setup again does not show it running");
    zw_wait_long(device_setup_stopped, 3600000u, "ZW-029: the retried setup never stopped again");

    /* A normal removal stopped, then opened again: a new window, never the old result. */
    tap_text("Remove this device");
    run(300);
    wait_for_text("Listening ·", 10000u, "Z-Wave: the remove page never started listening");
    tap_text("Stop");
    run(500);
    expect(device_setup_stopped(), "Stop did not go back to the device page");
    tap_text("Remove this device");
    run(300);
    expect(find_label_with(lv_screen_active(), "nothing was removed") == NULL &&
               find_label_with(lv_screen_active(), "Nothing was removed") == NULL,
           "ZW-029: reopening the removal showed the stopped one's result instead of listening");
    wait_for_text("Listening ·", 10000u, "ZW-029: reopening the removal did not listen again");
    /* the device can't answer: the window runs out */
    wait_for_text("No device answered in time", 70000u,
                  "the removal's window did not end");
    expect(visible_text("Remove it: it's reset, broken or gone"),
           "ZW-029: a removal that found nothing does not offer the other way");
    shot("zwave_remove_nothing");

    tap_text("Remove it: it's reset, broken or gone");
    run(300);
    expect(visible_text("Remove it: it's reset, broken or gone?"), "no confirmation");
    tap_text("Remove it");
    run(300);
    expect(visible_text_with("Checking that it no longer answers..."),
           "ZW-029: the device page does not show the check running");
    shot("zwave_device_checking");
    zw_wait(stray_gone, 60000u, "ZW-029: the stray device was not removed from the panel and radio");
    run(1500);
    expect(visible_text("Add a device"), "a removed device's page stayed open");

    /* The keypad: S2 with its PIN, on batteries, its sounds and lights from its page. */
    tap_text("Add a device");
    run(300);
    wait_for_text("Listening ·", 10000u, "Z-Wave: the add page never started listening");
    host_panel_zwave_press_keypad();
    wait_for_text("Allow", 20000u, "Z-Wave: the keypad did not ask to be secured");
    tap_text("Allow");
    run(300);
    wait_for_text("Enter the PIN", 10000u, "Z-Wave: the keypad's PIN page never came up");
    {
        char pin[6];
        snprintf(pin, sizeof(pin), "%05u", (unsigned)host_panel_zwave_keypad_pin());
        tap_text("Enter the PIN");
        run(300);
        sheet_text(pin);
        tap_name("sheet_done");
    }
    wait_for_text("Keypad added", 300000u, "Z-Wave: the keypad was not added (or not as a keypad)");
    expect(find_label_with(lv_screen_active(), "Encrypted") != NULL,
           "the keypad was not added securely");
    expect(find_label(lv_screen_active(), "Use it for") == NULL,
           "B015 UX-009: a keypad's result offers zones it can't feed");
    shot("zwave_keypad_added");
    tap_text("Open it");
    run(500);
    expect(visible_text("Keypad"), "B015 UX-009: the keypad is not called a keypad");
    expect(strcmp(row_label("Battery", "value_label"), "Mains powered") != 0,
           "ZW-029: a keypad woken by the panel's call is said to be mains powered");
    expect(strcmp(row_label("Checks in", "value_label"), "Not needed: it answers when called") == 0,
           "a keypad's check-in row does not say it answers when called");
    expect(strcmp(row_label("Sounds and lights", "value_label"), "Off") == 0,
           "ZW-028: the keypad's sounds and lights are not offered, off");
    expect(find_label_with(lv_screen_active(), "Its buttons don't arm or disarm the panel") != NULL,
           "ZW-028: the page implies the keypad's buttons control the panel");
    shot("zwave_keypad_page");
    tap_text("Sounds and lights");
    run(300);
    expect(find_label(by_name("pick_list"), "On") != NULL &&
               find_label(by_name("pick_list"), "Off") != NULL,
           "the sounds and lights choices are missing");
    shot("zwave_keypad_feedback_pick");
    tap_text("On");
    run(1200);
    expect(strcmp(row_label("Sounds and lights", "value_label"), "On") == 0,
           "ZW-028: the keypad's sounds and lights were not assigned");
    zw_wait(keypad_heard_state, 20000u, "ZW-028: the keypad never got the panel's state");
    run(1500);
    expect(strcmp(row_label("Sounds and lights", "detail_label"), "") == 0,
           "ZW-028: an assigned keypad that got the state still says it is waiting");
    shot("zwave_keypad_feedback_on");

    /* A removal as failed of a keypad that answers: kept, said so on its page. */
    scroll_to_text("page_body", "Remove it: it's reset, broken or gone");
    tap_text("Remove it: it's reset, broken or gone");
    run(300);
    tap_text("Remove it");
    run(300);
    zw_wait(clean_ended, 30000u, "the check never ended");
    run(1500);
    expect(visible_text_with("It answered, so it was kept"),
           "ZW-029: a device that answered is not said to be kept");
    expect(host_panel_zwave_in_panel(host_panel_zwave_keypad_node()) &&
               host_panel_zwave_in_radio(host_panel_zwave_keypad_node()),
           "ZW-029: a device that answers was removed as failed");
    shot("zwave_device_still_answers");

    /* Its power gone, and a radio that doesn't count it as failed: nothing removed, said so. */
    host_panel_zwave_keypad_silent(true);
    host_panel_zwave_radio_keeps_failed(true);
    scroll_to_text("page_body", "Remove it: it's reset, broken or gone");
    tap_text("Remove it: it's reset, broken or gone");
    run(300);
    tap_text("Remove it");
    run(300);
    zw_wait(clean_ended, 60000u, "the check never ended");
    run(1500);
    expect(visible_text_with("doesn't count it as failed yet"),
           "ZW-029: the radio's refusal is not said");
    expect(host_panel_zwave_in_panel(host_panel_zwave_keypad_node()),
           "ZW-029: a refused removal took it out of the panel's list (a ghost left in the radio)");
    shot("zwave_device_refused");
    host_panel_zwave_radio_keeps_failed(false);
    host_panel_zwave_keypad_silent(false);

    /* ZW-031: its buttons arm and disarm - assigned on its page after a question, refused while
     * installing, then through the engine with the users' own codes. */
    scroll_to_text("page_body", "Arm and disarm from its buttons");
    expect(strcmp(row_label("Arm and disarm from its buttons", "value_label"), "Off") == 0,
           "ZW-031: a keypad's buttons control the panel before the installer allowed it");
    tap_text("Arm and disarm from its buttons");
    run(300);
    tap_text("On");
    run(300);
    expect(visible_text("Let its buttons arm and disarm?"),
           "ZW-031: control was granted without a question");
    shot("zwave_keypad_control_confirm");
    tap_text("Allow");
    run(1200);
    expect(strcmp(row_label("Arm and disarm from its buttons", "value_label"), "On") == 0,
           "ZW-031: the keypad's control was not assigned");
    expect(find_label_with(lv_screen_active(), "Its buttons arm and disarm with a user's code") !=
               NULL,
           "ZW-031: the page does not say its buttons now arm and disarm");
    expect(visible_text("Emergency buttons") &&
               strcmp(row_label("Emergency buttons", "value_label"), "Off") == 0,
           "ZW-031: emergency buttons are on without the installer");
    scroll_to_text("page_body", "Emergency buttons");
    shot("zwave_keypad_control_on");
    host_panel_zwave_keypad_keys("123456", ZW_ENTRY_EVENT_ARM_AWAY);
    run(1500);
    expect(host_panel_zwave_keypad_last() != ALARM_OK,
           "ZW-031: the keypad armed the panel in installer mode");

    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
    host_panel_zwave_keypad_keys("0000", ZW_ENTRY_EVENT_ARM_AWAY);
    run(1500);
    expect(host_panel_zwave_keypad_last() == ALARM_ERR_INVALID_CODE,
           "ZW-031: a wrong code from the keypad was not refused by the engine");
    /* Zone 1 still shows CHECK from the removal above: the engine says not ready, and the
     * keypad's own "bypass" prompt answers (its feedback, ZW-028 - device sounds, not the
     * panel's) - the whole loop, keypad -> engine -> keypad. */
    const unsigned prompts = host_panel_zwave_keypad_indicators(NULL);
    host_panel_zwave_keypad_keys("123456", ZW_ENTRY_EVENT_ARM_AWAY);
    run(3000);
    expect(host_panel_zwave_keypad_last() == ALARM_ERR_NOT_READY,
           "ZW-031: the keypad's Away was not answered by the engine's readiness");
    uint8_t last_id = 0u;
    expect(host_panel_zwave_keypad_indicators(&last_id) > prompts && last_id == 16u,
           "ZW-028/031: the keypad never heard its not-ready prompt");
    shot("home_not_ready_from_zwave_keypad");
    /* Home (Stay) with a partition that is ready only for Stay is the engine's call as well:
     * Disarm with the master code is always an answer the keypad hears */
    host_panel_zwave_keypad_keys("123456", ZW_ENTRY_EVENT_DISARM_ALL);
    run(1500);
    expect(host_panel_zwave_keypad_last() == ALARM_OK,
           "ZW-031: the keypad's Disarm with the master code was refused");
    run(300);
}

/* ZW-033: a device's software - its version on its page, a check only when asked (the service
 * unreachable, then an update offered with its notes), the installer's confirmation, the
 * transfer's progress, its end, and its setup read again (its new version). The simulated update
 * service answers a second later; the keypad is woken by the panel's call, so nothing waits for a
 * wake-up (a sleeper's update is test_zwz_panel's). */
static void walk_zwave_firmware(void)
{
    printf("Z-Wave: a device's software update\n");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Z-Wave");
    run(500);
    tap_text("Keypad");
    run(500);
    /* BL-152 (ZW-029): a device that answers can be checked at any time, and a finished one set
     * up again - confirmed first, its zones kept, the page saying so while it runs. Here, after
     * the keypad's prompts above: its reading's questions fill the fake keypad's command log. */
    expect(visible_text("Check it now"), "BL-152: a reachable listening device has no Check it now");
    scroll_to_text("page_body", "Set it up again");
    expect(visible_text("Set it up again"), "BL-152: a finished device has no Set it up again");
    shot("zwave_device_health_actions");
    tap_text("Set it up again");
    run(300);
    expect(visible_text("Set it up again?"), "BL-152: setting it up again asks no confirmation");
    shot("zwave_device_setup_again_confirm");
    /* its first two answers lost: the re-read, done in well under a second since S2 answers at
     * once (9fa7893), waits two report times, so it still runs while the page and the list are
     * looked at. It still hears the panel (still reachable), and the third try goes through. */
    host_panel_zwave_keypad_lose_replies(2u);
    tap_text("Set it up again");
    run(300);
    lv_obj_t *reading = find_label_with(lv_screen_active(), "Reading it again.");
    expect(reading != NULL && strstr(lv_label_get_text(reading), "Its zones keep working") != NULL,
           "BL-152: the page doesn't say its setup is read again");
    if (reading != NULL) {
        scroll_into_view(reading);
        run(50);
    }
    expect(!visible_text("Try its setup again"),
           "BL-152: a re-read offers Try its setup again while it runs");
    shot("zwave_device_setup_again_running");
    /* left and opened again while it reads: the page still says so - the panel's word, not the
     * last visit's - and still offers no "Try its setup again" */
    tap_name("page_back");
    run(300);
    expect(find_label_with(lv_screen_active(), "Being read again") != NULL,
           "BL-152: the device list doesn't say the keypad is read again");
    tap_text("Keypad");
    run(500);
    expect(find_label_with(lv_screen_active(), "Reading it again.") != NULL,
           "BL-152: opened again, the page forgot its setup is read again");
    expect(find_label_with(lv_screen_active(), "Setting it up: step") == NULL &&
               find_label(lv_screen_active(), "Try its setup again") == NULL,
           "BL-152: opened again, a re-read shows a first setup");
    wait_for_text("Set it up again", 120000u, "BL-152: the device's setup was not read again");
    run(1500);

    /* BL-152 (ZW-029, ZW-037): a re-read that stopped - the keypad's power gone - is calm and
     * says it works as before; then it goes quiet, and its row and page say THAT instead */
    host_panel_zwave_keypad_silent(true);
    scroll_to_text("page_body", "Set it up again");
    tap_text("Set it up again");
    run(300);
    tap_text("Set it up again");
    run(300);
    s_wait_text = "Reading it again stopped";
    zw_wait_long(text_visible, 3600000u, "BL-152: a re-read of a silent device never stopped");
    expect(find_label_with(lv_screen_active(), "It works as before") != NULL,
           "BL-152: a stopped re-read doesn't say the device works as before");
    scroll_into_view(find_label_with(lv_screen_active(), "It works as before"));
    run(50);
    shot("zwave_device_setup_again_stopped");
    tap_name("page_back");
    run(300);
    expect(find_label_with(lv_screen_active(), "Read again stopped: works as before") != NULL,
           "BL-152: the device list doesn't say the re-read stopped");
    tap_text("Keypad");
    run(500);
    scroll_to_text("page_body", "Check it now");
    tap_text("Check it now");
    s_wait_text = "It stopped answering.";
    zw_wait_long(text_visible, 3600000u, "BL-152: a stopped re-read hides that it stopped answering");
    scroll_into_view(find_label_with(lv_screen_active(), "It stopped answering."));
    run(50);
    shot("zwave_device_setup_again_stopped_quiet");
    tap_name("page_back");
    run(1500);
    expect(find_label_with(lv_screen_active(), "Not heard") != NULL &&
               find_label_with(lv_screen_active(), "Read again stopped: works as before") == NULL,
           "BL-152: a stopped re-read of a silent device still reads calm in the list");
    tap_text("Keypad");
    run(500);
    host_panel_zwave_keypad_silent(false);
    scroll_to_text("page_body", "Try its setup again");
    tap_text("Try its setup again");
    run(300);
    wait_for_text("Set it up again", 120000u, "BL-152: the keypad's setup was not read again");
    run(1500);

    scroll_to_text("page_body", "Check for an update");
    expect(strcmp(row_label("Version", "value_label"), "1.10") == 0,
           "ZW-033: the device page does not show its software version");
    expect(visible_text("Checked only when you ask; nothing installs by itself."),
           "ZW-033: the page does not say updates are checked only when asked");
    shot("zwave_device_software");

    host_panel_zwave_fw_offline(true);
    tap_text("Check for an update");
    run(300);
    expect(visible_text("Asking the update service..."), "ZW-033: the check shows no progress");
    wait_for_text("couldn't reach the update service", 10000u,
                  "ZW-033: an unreachable update service is not said");
    scroll_to_text("page_body", "Check again");
    shot("zwave_fw_check_failed");
    host_panel_zwave_fw_offline(false);

    tap_text("Check again");
    wait_for_text("Install version 1.20", 10000u, "ZW-033: the offered update is not shown");
    expect(strcmp(row_label("Install version 1.20", "detail_label"),
                  "Longer battery life. Fixes a tamper report that could be missed.") == 0,
           "ZW-033: the update's notes are not shown on one line");
    scroll_to_text("page_body", "Longer battery life. Fixes a tamper report that could be missed.");
    shot("zwave_fw_available");

    tap_text("Install version 1.20");
    run(300);
    expect(visible_text("Install version 1.20?"), "ZW-033: installing asks no confirmation");
    shot("zwave_fw_confirm");
    tap_text("Install");
    wait_for_text("Sending its new software:", 60000u, "ZW-033: the transfer shows no progress");
    expect(visible_text("Stop the update"), "ZW-033: a running update can't be stopped");
    scroll_to_text("page_body", "Stop the update");
    shot("zwave_fw_sending");
    wait_for_text("It took its new software", 600000u, "ZW-033: the update's end is not said");
    scroll_to_text("page_body",
                   "It took its new software. It restarts, then the panel reads it again.");
    shot("zwave_fw_done");
    /* its restart, then its setup reads it again: the new version on its page */
    s_wait_text = "1.20";
    zw_wait_long(text_visible, 600000u, "ZW-033: the new version never shows after the update");
    expect(strcmp(row_label("Version", "value_label"), "1.20") == 0,
           "ZW-033: the version after the update is not 1.20");

    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}

/* Adds a simulated S2 device from the Z-Wave page: Add a device, its button, Allow, its PIN. */
static void zw_add_with_pin(void (*press)(void), uint16_t pin_value, const char *added,
                            const char *what)
{
    tap_text("Add a device");
    run(300);
    wait_for_text("Listening ·", 10000u, what);
    press();
    wait_for_text("Allow", 20000u, what);
    tap_text("Allow");
    run(300);
    wait_for_text("Enter the PIN", 10000u, what);
    char pin[6];
    snprintf(pin, sizeof(pin), "%05u", (unsigned)pin_value);
    tap_text("Enter the PIN");
    run(300);
    sheet_text(pin);
    tap_name("sheet_done");
    wait_for_text(added, 300000u, what);
}

static bool relay_released(void)
{
    return host_panel_zwave_relay_level(1u) == 0u;
}

/* ZW-034, ZW-035: a light switch and a relay kit added, switched from their pages (the switch
 * pressed by hand shows too), and relay 1 made a garage door opener with the tilt sensor's zone:
 * nothing pressed while the sensor hasn't said where the door is, then opened and watched. */
static void walk_zwave_switches(void)
{
    printf("Z-Wave: a light switch, a relay kit and a garage door\n");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Z-Wave");
    run(500);
    zw_add_with_pin(host_panel_zwave_press_light, host_panel_zwave_light_pin(), "Switch added",
                    "ZW-034: the light switch was not added as a switch");
    expect(find_label(lv_screen_active(), "Use it for") == NULL,
           "ZW-034: a switch's result offers zones it can't feed");
    shot("zwave_switch_added");
    tap_text("Open it");
    run(1500);
    expect(strcmp(row_label("Switch", "value_label"), "Off") == 0,
           "ZW-034: the switch's state was not read when it was ready");
    tap_text("Switch");
    run(3000);
    expect(strcmp(row_label("Switch", "value_label"), "On") == 0,
           "ZW-034: the switch did not confirm it is on");
    host_panel_zwave_light_toggle(); /* someone at the switch */
    run(2000);
    expect(strcmp(row_label("Switch", "value_label"), "Off") == 0,
           "ZW-034: a press at the switch did not show");
    scroll_to_text("page_body", "Switch");
    shot("zwave_switch_page");
    tap_name("page_back");
    run(500);

    /* a dimmer: its level picked, and confirmed by the device */
    zw_add_with_pin(host_panel_zwave_press_dimmer, host_panel_zwave_dimmer_pin(), "Dimmer added",
                    "ZW-034: the dimmer was not added as a dimmer");
    tap_text("Open it");
    run(1500);
    tap_text("Light");
    run(300);
    shot("zwave_dimmer_levels");
    tap_text("50 %");
    run(3000);
    expect(strcmp(row_label("Light", "value_label"), "50 %") == 0 &&
               host_panel_zwave_dimmer_level() == 50u,
           "ZW-034: the dimmer did not confirm 50 %");
    tap_name("page_back");
    run(500);

    zw_add_with_pin(host_panel_zwave_press_relay, host_panel_zwave_relay_pin(), "Relays added",
                    "ZW-035: the relay kit was not added");
    tap_text("Open it");
    run(1500);
    expect(visible_text("Relay 1") && visible_text("Relay 2") && visible_text("Relay 3"),
           "ZW-035: the relay kit's three relays are not on its page");
    scroll_to_text("page_body", "Relay 3");
    shot("zwave_relays_page");
    tap_text("Relay 2");
    run(3000);
    expect(strcmp(row_label("Relay 2", "value_label"), "On") == 0,
           "ZW-034: relay 2 did not confirm it is on");
    tap_text("Relay 2");
    run(3000);
    tap_text("Use relay 1 for a garage door");
    run(300);
    shot("zwave_garage_pick_zone");
    tap_text("Garage Door"); /* the simulator's garage door zone: a sensor that doesn't move */
    run(1000);
    expect(visible_text("Garage door"), "ZW-035: relay 1 did not become a garage door opener");
    expect(strcmp(row_label("Garage door", "value_label"), "Unknown") == 0 ||
               strcmp(row_label("Garage door", "value_label"), "Open") == 0 ||
               strcmp(row_label("Garage door", "value_label"), "Closed") == 0,
           "ZW-035: the garage door's state is not in words");
    const uint32_t sets = host_panel_zwave_relay_sets();
    scroll_to_text("page_body", "Garage door opener");
    shot("zwave_garage_page");
    if (strcmp(row_label("Garage door", "value_label"), "Unknown") == 0) {
        tap_text("Open it");
        run(1500);
        expect(host_panel_zwave_relay_sets() == sets,
               "ZW-035: the relay was pressed without knowing where the door is");
        expect(find_label_with(lv_screen_active(), "Nothing pressed") != NULL,
               "ZW-035: a press refused for want of a position is not explained");
        scroll_to_text("page_body", "Garage door opener");
        shot("zwave_garage_no_position");
    } else if (strcmp(row_label("Garage door", "value_label"), "Closed") == 0) {
        /* closed: pressed and released, then watched - this door's sensor never moves */
        tap_text("Open it");
        zw_wait(relay_released, 5000u, "ZW-035: the relay was not released after its press");
        expect(host_panel_zwave_relay_sets() == sets + 2u,
               "ZW-035: the press was not one on and one off");
        expect(strcmp(row_label("Garage door", "value_label"), "Closed") == 0 &&
                   strcmp(row_label("Garage door", "detail_label"),
                          "Pressed to open: waiting for its sensor") == 0,
               "ZW-035: the door is said to move before its sensor saw it (B015 UX-043)");
        scroll_to_text("page_body", "Garage door opener");
        shot("zwave_garage_opening");
        s_wait_text = "didn't see it get there";
        zw_wait_long(text_visible, 60000u, "ZW-035: a door that didn't move is not said");
        expect(strcmp(row_label("Garage door", "value_label"), "Closed") == 0,
               "ZW-035: the door's state is not the sensor's after all");
        scroll_to_text("page_body", "Garage door opener");
        shot("zwave_garage_did_not_move");
    }
    tap_text("Garage door opener");
    run(300);
    tap_text("Stop");
    run(800);
    expect(visible_text("Use relay 1 for a garage door"),
           "ZW-035: the relay did not go back to a plain relay");

    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}

static bool smart_joined_row(void)
{
    return strcmp(row_label(s_wait_text, "value_label"), "Joined") == 0;
}

/* ZW-024: SmartStart (screens.md §9) - a sensor registered by its 40-digit key joins by itself
 * once powered: the empty list, a short key refused, the key registered, the join, its page. */
static void walk_zwave_smartstart(void)
{
    printf("Z-Wave: SmartStart\n");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Z-Wave");
    run(500);
    scroll_to_text("page_body", "Waiting to join");
    expect(strcmp(row_label("Waiting to join", "value_label"), "None") == 0,
           "ZW-024: the Z-Wave page counts sensors waiting when none is registered");
    tap_text("Waiting to join");
    run(500);
    expect(visible_text("No sensor is registered."), "ZW-024: the empty list says nothing");
    shot("zwave_smartstart_empty");

    tap_text("Register a sensor");
    run(300);
    sheet_text("1234567890");
    tap_name("sheet_done");
    run(300);
    expect(strcmp(lv_subject_get_string(&subject_toast), "That isn't a 40-digit key: check it") == 0,
           "ZW-024: a short key was not refused in words");
    run(4000); /* its toast gone */

    char key[48], digits[48];
    host_panel_zwave_smart_key(key, sizeof(key));
    size_t n = 0;
    for (const char *c = key; *c != '\0' && n + 1u < sizeof(digits); c++) {
        if (*c != '-') {
            digits[n++] = *c;
        }
    }
    digits[n] = '\0';
    char name[24];
    snprintf(name, sizeof(name), "Sensor %.5s", &key[6]); /* its key's second group */
    tap_text("Register a sensor");
    run(300);
    sheet_text(digits);
    tap_name("sheet_done");
    run(800);
    expect(strcmp(row_label(name, "value_label"), "Waiting") == 0,
           "ZW-024: the registered sensor is not waiting to join");
    expect(strcmp(row_label(name, "detail_label"), "Power it up: it joins by itself") == 0,
           "ZW-024: a waiting sensor doesn't say to power it");
    shot("zwave_smartstart_waiting");
    tap_text(name);
    run(500);
    expect(find_label_with(lv_screen_active(), "Key xxxxx-") != NULL,
           "ZW-024: the entry page doesn't show its key without its PIN");
    expect(find_label_with(lv_screen_active(), key) == NULL,
           "ZW-024: the entry page shows the PIN");
    expect(visible_text("Security it may have"),
           "ZW-024: a waiting sensor's page doesn't say what security it may have");
    shot("zwave_smartstart_entry");
    tap_name("page_back");
    run(300);

    host_panel_zwave_smart_power();
    s_wait_text = name;
    zw_wait_long(smart_joined_row, 180000u, "ZW-024: the powered sensor did not join by itself");
    expect(host_panel_zwave_smart_joined(), "ZW-024: the simulated sensor is not in the network");
    expect(find_label_with(lv_screen_active(), "In the network: device") != NULL,
           "ZW-024: a joined sensor's line doesn't say which device it is");
    shot("zwave_smartstart_joined");
    tap_text(name); /* its line is its device */
    run(500);
    expect(find_label_with(lv_screen_active(), "Key xxxxx-") == NULL,
           "ZW-024: a joined sensor's line opened the registration page, not its device");
    shot("zwave_smartstart_joined_device");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Waiting to join");
    expect(strcmp(row_label("Waiting to join", "value_label"), "None") == 0,
           "ZW-024: a joined sensor still counts as waiting");

    /* removed by hand: its code goes with it - unless kept, and then the reset sensor, still
     * powered, joins again by itself (screens.md §7) */
    scroll_to_text("page_body", "Remove a device");
    tap_text("Remove a device");
    run(300);
    wait_for_text("Listening ·", 10000u, "ZW-024: the removal did not start");
    host_panel_zwave_press_smart_remove();
    wait_for_text("Removed:", 20000u, "ZW-024: the SmartStart sensor was not removed");
    expect(visible_text("Forget its QR code too"),
           "ZW-024: removing a registered sensor doesn't say its code goes too");
    shot("zwave_smartstart_removed");
    tap_text("Forget its QR code too"); /* off: kept */
    run(500);
    tap_text("Done");
    run(500);
    zw_wait_long(host_panel_zwave_smart_joined, 120000u,
                 "ZW-024: a kept code did not let the reset sensor join again");
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}

static bool routes_counting(void)
{
    return strstr(row_label("Rebuild routes", "value_label"), " of ") != NULL;
}

static bool routes_said(void)
{
    /* BL-152 ZW-024: the result stays on the page until it is cleared (no toast) */
    return find_label_with(lv_screen_active(), "Routes rebuilt for") != NULL;
}

static bool radio_interference(void)
{
    return find_label_with(lv_screen_active(), "Radio · Interference") != NULL;
}

static bool radio_working(void)
{
    return find_label_with(lv_screen_active(), "Radio · Working") != NULL;
}

static bool light_not_heard(void)
{
    return strstr(row_label("Switch", "detail_label"), "Not heard") != NULL;
}

/* ZW-024: looking after the network (screens.md §3, §6, network.md §9-§11) - rebuilding routes
 * (its progress and its end), interference said and cleared, a keypad's light
 * blinking on request, and a broken light switch replaced with its zone kept. */
static void walk_zwave_network_care(void)
{
    printf("Z-Wave: routes, interference, identify, replace\n");
    expect(host_panel_zwave_fake_overflow() == 0u, "the simulated radio ran out of room");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Z-Wave");
    run(500);

    scroll_to_text("page_body", "Rebuild routes");
    tap_text("Rebuild routes");
    run(300);
    tap_text("Rebuild");
    run(500);
    zw_wait_long(routes_counting, 30000u, "ZW-024: the routes' progress is not shown");
    scroll_to_text("page_body", "Rebuild routes");
    shot("zwave_routes_running");
    zw_wait_long(routes_said, 60000u, "ZW-024: the rebuilt routes weren't said");
    scroll_to_text("page_body", "Routes rebuilt for*");
    run(500);
    expect(visible_text_with("Routes rebuilt for"), "ZW-024: the routes' result is not on the page");
    expect(visible_text("Clear"), "ZW-024: the routes' result can't be cleared");
    shot("zwave_routes_done"); /* stopping it: the host tests (a simulated house is quick) */

    host_panel_zwave_noise(-45);
    zw_wait_long(radio_interference, 90000u, "ZW-024: interference is not shown");
    scroll_to_text("page_body", "Radio · Interference");
    shot("zwave_interference");
    host_panel_zwave_noise(-95);
    zw_wait_long(radio_working, 90000u, "ZW-024: the radio doesn't say working once quiet");

    host_panel_zwave_keypad_jam_report(); /* §9: its own view of the air, on its page */
    run(2000);
    scroll_to_text("page_body", "Keypad");
    tap_text("Keypad");
    run(800);
    scroll_to_text("page_body", "Interference");
    expect(visible_text("Interference"), "ZW-024: the keypad's jamming report isn't on its page");
    shot("zwave_device_jam_report");
    scroll_to_text("page_body", "Blink its light");
    tap_text("Blink its light");
    run(3000);
    expect(strcmp(lv_subject_get_string(&subject_toast), "Its light blinks 3 times") == 0,
           "ZW-024: the keypad was not told to blink");
    shot("zwave_identify");
    tap_name("page_back");
    run(500);

    host_panel_zwave_break_light();
    scroll_to_text("page_body", "Switch");
    zw_wait_long(light_not_heard, 45u * 60000u, "ZW-024: the broken switch is still heard");
    tap_text("Switch");
    run(800);
    scroll_to_text("page_body", "Replace it");
    tap_text("Replace it");
    run(300);
    tap_text("Replace it"); /* the confirmation */
    run(800);
    /* ZW-024 (network.md §10): first the check that the old one is gone (3 pings 2 s apart, then
     * the radio's failed list); the radio isn't listening yet, so the page asks for nothing */
    expect(visible_text("Checking the old device is gone"),
           "ZW-024: the replacement's check of the old device is not said");
    expect(find_label_with(lv_screen_active(), "Put the new device in pairing mode") == NULL &&
               find_label_with(lv_screen_active(), "Listening for the device") == NULL,
           "ZW-024: the page asks for the new device before the radio listens");
    shot("zwave_replace_checking");
    wait_for_text("Listening ·", 15000u, "ZW-024: the replacement window did not open");
    expect(visible_text("Replace a Z-Wave device"), "ZW-024: the page doesn't say it replaces");
    shot("zwave_replace_listening");
    host_panel_zwave_press_spare_light();
    wait_for_text("Allow", 20000u, "ZW-024: the replacement's security question");
    tap_text("Allow");
    run(300);
    wait_for_text("Enter the PIN", 10000u, "ZW-024: the replacement's PIN");
    char pin[6];
    snprintf(pin, sizeof(pin), "%05u", (unsigned)host_panel_zwave_spare_light_pin());
    tap_text("Enter the PIN");
    run(300);
    sheet_text(pin);
    tap_name("sheet_done");
    wait_for_text("It took the old device's place: its zones follow it.", 300000u,
                  "ZW-024: the replacement did not take the broken switch's place");
    shot("zwave_replaced");
    tap_text("Done");
    run(500);

    /* ZW-025 (network.md §14): the radio's own software, asked about as Z-Wave JS asks about a
     * controller, installed over the wire; arming waits meanwhile (CORE-ARM-013) */
    for (int i = 0; i < 4 && find_label(lv_screen_active(), "Radio · Working") == NULL; i++) {
        tap_name("page_back");
        run(300);
    }
    scroll_to_text("page_body", "Radio · Working");
    tap_text("Radio · Working");
    run(500);
    scroll_to_text("page_body", "Check for an update");
    tap_text("Check for an update");
    run(300);
    wait_for_text("Install version 2.30", 20000u, "ZW-025: the radio's update is not offered");
    shot("zwave_radio_update_available");
    tap_text("Install version 2.30");
    run(300);
    expect(visible_text("Update the radio to 2.30?"),
           "ZW-025: installing the radio's update does not ask first");
    tap_text("Update");
    run(500);
    wait_for_text("Updating the radio:", 20000u, "ZW-025: the radio's update shows no progress");
    expect(strcmp(row_label("State", "value_label"), "Updating") == 0,
           "ZW-025: the radio's state doesn't say it is updating");
    expect(find_label(lv_screen_active(), "Not while its software installs") != NULL,
           "ZW-025: Restart the radio stays tappable while the radio updates");
    shot("zwave_radio_updating");
    wait_for_text("Updated: it runs 2.30 now.", 600000u, "ZW-025: the radio's update did not finish");
    shot("zwave_radio_updated");
    /* the radio's file from an SD card (network.md §14): the simulator's card has none */
    scroll_to_text("page_body", "Install from an SD card");
    tap_text("Install from an SD card");
    run(300);
    expect(visible_text("Update the radio from the SD card?"),
           "ZW-025: installing from the SD card does not ask first");
    tap_text("Update");
    run(1500);
    expect(find_label(by_name("page_body"),
                      "No .gbl file on the SD card. Copy the radio's file from its maker to the "
                      "card's top folder, or to a folder named zwave.") != NULL,
           "ZW-025: a card without the radio's file doesn't say what to do");
    shot("zwave_radio_sd_no_file");
    /* nothing found for it (approved words, 2026-10-20): the SD card and the Panel Manager remain */
    host_panel_zwave_radio_unlisted(true);
    scroll_to_text("page_body", "Check for an update");
    tap_text("Check for an update");
    run(300);
    wait_for_text("No update was found for this radio. Its maker's own file can still be installed "
                  "from an SD card or the Panel Manager.",
                  20000u, "ZW-025: a check that finds nothing doesn't say what remains");
    shot("zwave_radio_none_found");
    host_panel_zwave_radio_unlisted(false);
    /* older than its maker advises: the page points to Check for an update */
    host_panel_zwave_radio_advised(true);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Radio · Update its software");
    tap_text("Radio · Update its software");
    run(500);
    scroll_to_text("page_body", "Its software is older than its maker advises. Tap Check for an "
                                "update below to install the newer one.");
    expect(visible_text("Its software is older than its maker advises. Tap Check for an update "
                        "below to install the newer one."),
           "ZW-025: an old radio's page doesn't point to Check for an update");
    shot("zwave_radio_advised");
    host_panel_zwave_radio_advised(false);
    for (int i = 0; i < 4 && find_label(lv_screen_active(), "Leave installer mode") == NULL; i++) {
        tap_name("page_back"); /* back to the installer menu, wherever "Done" left it */
        run(300);
    }
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}

/* A Troubles row that opens a radio's page: installer mode may have ended meanwhile (it times out
 * by itself), then the locked Installer page asks for the code first. */
static void zw_trouble_open(const char *row)
{
    tap_text(row);
    run(600);
    if (lv_subject_get_int(&subject_toast_visible) == 1 &&
        strcmp(lv_subject_get_string(&subject_toast),
               "Enter the installer code to see the radio") == 0) {
        tap_text("Enter the installer code");
        run(300);
        type("69420k");
    }
    run(1600);
}

/* BL-152, ZW-019: the sensor radio trouble names the Z-Wave radio when it has the problem, opens
 * its page after the installer code, and lists each radio when both are in trouble. */
static void walk_zwave_troubles(void)
{
    printf("Z-Wave: the Troubles row opens the radio's page\n");
    tap_name("nav_home");
    run(300);
    host_panel_zwave_status_force(1u); /* jammed: the radio still answers */
    host_panel_trouble(ALARM_TRBL_SENSOR_RADIO, true);
    run(1500);
    tap_name("bar_trouble");
    run(300);
    expect(visible_text("Z-Wave radio problem"),
           "ZW-019: the sensor radio trouble doesn't name Z-Wave when its radio is jammed");
    expect(visible_text_with("Tap to see the radio"),
           "ZW-019: the Z-Wave radio's trouble doesn't say it opens the radio's page");
    expect(!visible_text("Zigbee radio problem"),
           "ZW-019: a Zigbee row shows while only the Z-Wave radio is in trouble");
    shot("zwave_trouble_row");
    tap_text("Z-Wave radio problem");
    run(300);
    expect(!visible_text("Radio · Interference"),
           "ZW-019: the trouble opens the Z-Wave page without the installer code");
    expect(lv_subject_get_int(&subject_toast_visible) == 1 &&
               strcmp(lv_subject_get_string(&subject_toast),
                      "Enter the installer code to see the radio") == 0,
           "ZW-019: the Z-Wave trouble doesn't say the radio needs the installer code");
    tap_text("Enter the installer code");
    run(300);
    type("69420k");
    run(1600);
    expect(visible_text("Radio · Interference"),
           "ZW-019: the trouble doesn't open the Z-Wave page after the code");
    expect(lv_subject_get_int(&subject_page_back) == 1,
           "ZW-019: the Z-Wave page opened from the trouble has no way back");
    tap_name("page_back");
    run(300);
    expect(visible_text("Leave installer mode") || find_label(by_name("page_body"), "Leave installer mode") != NULL,
           "ZW-019: back from the trouble's Z-Wave page isn't the installer menu");

    /* installer mode now: the row opens the page at once; both radios in trouble list each */
    host_panel_zwave_status_force(2u); /* not answering */
    host_zigbee_demo_state(HOST_ZB_NOT_ANSWERING);
    tap_name("nav_home");
    run(1500);
    tap_name("bar_trouble");
    run(300);
    expect(visible_text("Z-Wave radio problem") && visible_text("Zigbee radio problem"),
           "ZW-019: with both radios in trouble the list doesn't show a row for each");
    shot("zwave_trouble_both");
    zw_trouble_open("Z-Wave radio problem");
    expect(find_label(by_name("page_body"), "Radio · Not answering") != NULL,
           "ZW-019: the Z-Wave row, in installer mode, doesn't open the Z-Wave page");
    tap_name("page_back");
    run(300);
    tap_name("nav_home");
    run(300);
    tap_name("bar_trouble");
    run(300);
    zw_trouble_open("Zigbee radio problem");
    expect(find_label(by_name("page_body"), "Radio · Not answering") != NULL &&
               find_label_with(by_name("page_body"), "Region") == NULL,
           "ZW-019: the Zigbee row of the pair doesn't open the Zigbee page");

    host_panel_zwave_status_force(0u);
    host_zigbee_demo_state(HOST_ZB_WORKING);
    host_panel_trouble(ALARM_TRBL_SENSOR_RADIO, false);
    run(1500);
    tap_name("nav_home");
    run(300);
}


static bool region_europe(void)
{
    return strcmp(row_label("Region", "value_label"), "Europe") == 0;
}

/* ZW-045: the installer code accepted on the region's pad sends the region picked, and the radio
 * comes back in it. The panel after walk_zigbee_real()'s restart has no Z-Wave yet: this starts it
 * with one device and a radio that offers Europe too (another band), so the change stops that
 * device. It runs after every other shot of the walk and takes none. */
static void walk_zwave_region_change(void)
{
    printf("Z-Wave: a region change that stops devices, with the installer code\n");
    host_panel_zwave_begin();
    zw_wait(host_panel_zwave_link_up, 10000u, "Z-Wave: the radio did not come up");
    host_panel_zwave_offer_region(0x00u); /* Europe, read at the panel's next start-up */
    host_panel_zwave_restart();
    zw_wait(host_panel_zwave_link_up, 10000u, "Z-Wave: the radio did not come up again");
    host_panel_zwave_add_insecure();
    zw_wait(host_panel_zwave_motion_ready, 300000u, "Z-Wave: the motion sensor was not set up");
    run(1000);
    wake();
    tap_name("nav_home");
    run(300);
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Z-Wave");
    run(1500);
    expect(!region_europe(), "ZW-045: the walk's radio is in Europe already");
    scroll_to_text("page_body", "Region");
    tap_text("Region");
    run(300);
    expect(find_label(by_name("page_body"), "1 device stops working") != NULL,
           "ZW-045: the region on another band does not count the devices that stop");
    tap_text("Europe");
    run(300);
    tap_text("Change it");
    run(300);
    expect(find_label(lv_screen_active(), "Confirm the region") != NULL,
           "ZW-045: a region change that stops devices did not ask for the installer code");
    type("69420k"); /* the installer code */
    run(300);
    expect(find_label(lv_screen_active(), "Confirm the region") == NULL,
           "ZW-045: the installer code did not close the region's pad");
    zw_wait(region_europe, 20000u, "ZW-045: the installer code did not change the region to Europe");
    wait_for_text("Radio · Working", 20000u,
                  "ZW-018: the radio did not come back after the region change");
    for (int i = 0; i < 4 && find_label(lv_screen_active(), "Leave installer mode") == NULL; i++) {
        tap_name("page_back");
        run(300);
    }
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}

/* ---- script mode: the panel driven by hand, for exploratory (UX) testing ---------------------
 * ALARM_PREVIEW_SCRIPT=<file> runs its lines instead of the walk and ends: a person's taps and the
 * simulated devices' buttons, with screenshots and what the screen says. Nothing here fails the
 * run: a missing label is said and the script goes on. Commands (one a line, # comments):
 *   tap <text>         the control showing exactly this text ("Forgot*" a prefix)
 *   tapname <name>     an object by name (nav_home, page_back, sheet_done, bar_settings...)
 *   pad <keys>         the code pad: digits, d (delete), k (enter)
 *   text <words>       into the open text sheet (then: tapname sheet_done)
 *   scroll <text>      bring the label into view
 *   wait <ms>          simulated time passes (the screen kept awake)
 *   shot <name>        a screenshot NN_<name>.png in the output folder
 *   dump | dumpall     the page's visible words (dumpall: the whole screen), one a line
 *   expect <text>      PASS or FAIL: some visible label contains the text
 *   pin keypad|spare|light|relay  the PIN on that simulated device's label
 *   zw <action>        begin, add-secure, add-insecure, press-spare-secure,
 *                      press-spare-insecure, press-keypad, press-stray, press-door-remove,
 *                      door-open, door-close, tamper-on, tamper-off, battery-low, battery-ok,
 *                      sleep-miss-on, sleep-miss-off, radio-out, radio-in, restart,
 *                      keypad-silent-on, keypad-silent-off, radio-keeps-failed-on,
 *                      radio-keeps-failed-off, wake-tilt, wake-door, zone1-door,
 *                      fw-offline-on, fw-offline-off (the simulated update service),
 *                      press-light, press-relay, press-dimmer (a light switch, a 3-relay
 *                      garage kit, a dimmer),
 *                      light-toggle, tilt-open, tilt-close (the garage door's tilt sensor),
 *                      smart-power (the SmartStart sensor gets its battery; `pin smart` prints
 *                      its whole 40-digit key), break-light, press-spare-light (a broken light
 *                      switch and its replacement),
 *                      keypad-away <code>, keypad-home <code>, keypad-disarm <code>,
 *                      keypad-police (the simulated keypad's buttons)
 *   echo <words>       printed as is */
static void dump_labels(lv_obj_t *o, int depth)
{
    if (o == NULL || !visible(o)) {
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class) && lv_label_get_text(o)[0] != '\0') {
        printf("  %*s%s\n", depth, "", lv_label_get_text(o));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        dump_labels(lv_obj_get_child(o, (int32_t)i), depth);
    }
}

static void script_wait(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100u) {
        s_ms += 100u;
        host_panel_advance(100u);
        lv_display_trigger_activity(NULL);
        lv_timer_handler();
    }
}

static void script_zw(const char *a)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } PLAIN[] = {
        {"begin", host_panel_zwave_begin},
        {"add-secure", host_panel_zwave_add_secure},
        {"add-insecure", host_panel_zwave_add_insecure},
        {"press-spare-secure", host_panel_zwave_press_spare_secure},
        {"press-spare-insecure", host_panel_zwave_press_spare_insecure},
        {"press-keypad", host_panel_zwave_press_keypad},
        {"press-stray", host_panel_zwave_press_stray},
        {"press-door-remove", host_panel_zwave_press_door_remove},
        {"restart", host_panel_zwave_restart},
        {"wake-tilt", host_panel_zwave_wake_spare_insecure},
        {"wake-door", host_panel_zwave_wake_door},
        {"press-light", host_panel_zwave_press_light},
        {"press-relay", host_panel_zwave_press_relay},
        {"press-dimmer", host_panel_zwave_press_dimmer},
        {"smart-power", host_panel_zwave_smart_power},
        {"break-light", host_panel_zwave_break_light},
        {"press-spare-light", host_panel_zwave_press_spare_light},
        {"light-toggle", host_panel_zwave_light_toggle},
    };
    for (size_t i = 0; i < sizeof(PLAIN) / sizeof(PLAIN[0]); i++) {
        if (strcmp(a, PLAIN[i].name) == 0) {
            PLAIN[i].fn();
            return;
        }
    }
    if (strcmp(a, "door-open") == 0 || strcmp(a, "door-close") == 0) {
        host_panel_zwave_door(strcmp(a, "door-open") == 0);
    } else if (strcmp(a, "tamper-on") == 0 || strcmp(a, "tamper-off") == 0) {
        host_panel_zwave_tamper(strcmp(a, "tamper-on") == 0);
    } else if (strcmp(a, "battery-low") == 0 || strcmp(a, "battery-ok") == 0) {
        host_panel_zwave_battery(strcmp(a, "battery-low") == 0);
    } else if (strcmp(a, "sleep-miss-on") == 0 || strcmp(a, "sleep-miss-off") == 0) {
        host_panel_zwave_sleep_miss(strcmp(a, "sleep-miss-on") == 0);
    } else if (strcmp(a, "radio-out") == 0 || strcmp(a, "radio-in") == 0) {
        host_panel_zwave_radio(strcmp(a, "radio-in") == 0);
    } else if (strcmp(a, "keypad-silent-on") == 0 || strcmp(a, "keypad-silent-off") == 0) {
        host_panel_zwave_keypad_silent(strcmp(a, "keypad-silent-on") == 0);
    } else if (strcmp(a, "tilt-open") == 0 || strcmp(a, "tilt-close") == 0) {
        host_panel_zwave_tilt(strcmp(a, "tilt-open") == 0); /* the garage door's tilt sensor */
    } else if (strcmp(a, "fw-offline-on") == 0 || strcmp(a, "fw-offline-off") == 0) {
        host_panel_zwave_fw_offline(strcmp(a, "fw-offline-on") == 0); /* the update service */
    } else if (strcmp(a, "radio-keeps-failed-on") == 0 || strcmp(a, "radio-keeps-failed-off") == 0) {
        host_panel_zwave_radio_keeps_failed(strcmp(a, "radio-keeps-failed-on") == 0);
    } else if (strncmp(a, "keypad-", 7) == 0) {
        /* keypad-away <code>, keypad-home <code>, keypad-disarm <code>, keypad-police */
        char what[16] = "";
        char code[16] = "";
        (void)sscanf(a + 7, "%15s %15s", what, code);
        const uint8_t ev = strcmp(what, "away") == 0     ? ZW_ENTRY_EVENT_ARM_AWAY
                           : strcmp(what, "home") == 0   ? ZW_ENTRY_EVENT_ARM_HOME
                           : strcmp(what, "disarm") == 0 ? ZW_ENTRY_EVENT_DISARM_ALL
                           : strcmp(what, "police") == 0 ? ZW_ENTRY_EVENT_POLICE
                                                          : 0u;
        if (ev == 0u) {
            printf("  ?? keypad-away|home|disarm <code> or keypad-police\n");
            return;
        }
        host_panel_zwave_keypad_keys(code[0] != '\0' ? code : NULL, ev);
        run(1500);
        printf("  the panel answered: %s\n", alarm_result_name(host_panel_zwave_keypad_last()));
    } else if (strcmp(a, "zone1-door") == 0) {
        printf("  zone 1 bound to the door: %s\n", host_panel_zwave_bind_door(1) ? "yes" : "no");
    } else {
        printf("  ?? unknown zw action '%s'\n", a);
    }
}

static int run_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open script %s\n", path);
        return 2;
    }
    char line[512];
    unsigned n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        n++;
        line[strcspn(line, "\r\n")] = '\0';
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') {
            cmd++;
        }
        if (*cmd == '\0' || *cmd == '#') {
            continue;
        }
        char *arg = strchr(cmd, ' ');
        if (arg != NULL) {
            *arg++ = '\0';
            while (*arg == ' ') {
                arg++;
            }
        } else {
            arg = cmd + strlen(cmd);
        }
        printf("[%u] %s %s\n", n, cmd, arg);
        if (strcmp(cmd, "tap") == 0) {
            if (find_label(lv_screen_active(), arg) == NULL) {
                printf("  MISSING: no visible label '%s'\n", arg);
            } else {
                tap_text(arg);
                run(300);
            }
        } else if (strcmp(cmd, "tapname") == 0) {
            lv_obj_t *o = find_visible(lv_screen_active(), arg);
            if (o == NULL) {
                printf("  MISSING: no visible object named '%s'\n", arg);
            } else {
                tap(o);
                run(300);
            }
        } else if (strcmp(cmd, "pad") == 0) {
            if (find_visible(lv_screen_active(), "pad_keys") == NULL) {
                printf("  MISSING: no code pad on screen\n");
            } else {
                type(arg);
                run(300);
            }
        } else if (strcmp(cmd, "text") == 0) {
            if (find_visible(lv_screen_active(), "sheet_text") == NULL) {
                printf("  MISSING: no text sheet open\n");
            } else {
                sheet_text(arg);
            }
        } else if (strcmp(cmd, "scroll") == 0) {
            lv_obj_t *o = find_label(lv_screen_active(), arg);
            if (o == NULL) {
                printf("  MISSING: no visible label '%s'\n", arg);
            } else {
                scroll_into_view(o);
                run(50);
            }
        } else if (strcmp(cmd, "wait") == 0) {
            script_wait((uint32_t)strtoul(arg, NULL, 10));
        } else if (strcmp(cmd, "shot") == 0) {
            shot(arg[0] != '\0' ? arg : "script");
        } else if (strcmp(cmd, "dump") == 0) {
            lv_obj_t *body = find_visible(lv_screen_active(), "settings_page");
            dump_labels(body != NULL ? body : lv_screen_active(), 0);
            lv_obj_t *over[] = {find_visible(lv_screen_active(), "dialog_card"),
                                find_visible(lv_screen_active(), "text_card"),
                                find_visible(lv_screen_active(), "pick_list")};
            for (size_t i = 0; i < 3; i++) {
                if (over[i] != NULL) {
                    printf("  -- on top:\n");
                    dump_labels(over[i], 2);
                }
            }
        } else if (strcmp(cmd, "dumpall") == 0) {
            dump_labels(lv_screen_active(), 0);
        } else if (strcmp(cmd, "expect") == 0) {
            printf("  %s: '%s'\n",
                   find_label_with(lv_screen_active(), arg) != NULL ? "PASS" : "FAIL", arg);
        } else if (strcmp(cmd, "pin") == 0 && strcmp(arg, "smart") == 0) {
            char key[48];
            host_panel_zwave_smart_key(key, sizeof(key));
            printf("  key on the SmartStart sensor's label: %s\n", key);
        } else if (strcmp(cmd, "pin") == 0) {
            printf("  PIN on the %s's label: %05u\n", arg,
                   (unsigned)(strcmp(arg, "keypad") == 0  ? host_panel_zwave_keypad_pin()
                              : strcmp(arg, "light") == 0 ? host_panel_zwave_light_pin()
                              : strcmp(arg, "relay") == 0 ? host_panel_zwave_relay_pin()
                              : strcmp(arg, "dimmer") == 0 ? host_panel_zwave_dimmer_pin()
                                                          : host_panel_zwave_spare_secure_pin()));
        } else if (strcmp(cmd, "zw") == 0) {
            script_zw(arg);
            run(100);
        } else if (strcmp(cmd, "echo") == 0) {
            printf("  %s\n", arg);
        } else {
            printf("  ?? unknown command '%s'\n", cmd);
        }
        fflush(stdout);
    }
    fclose(f);
    printf("script done: %d screenshots\n", s_shots);
    return 0;
}
#endif

/* ---- the sounder, from the engine to the speakers (docs/features/08) -------------------------- */

/* The interactive simulator plays these through the PC's sound card; here the same hook only
 * writes down what was asked for, so the wiring - engine status to keypad pattern - is proved
 * on a CI machine with no sound card at all. The patterns themselves are measured in the
 * `sounder` host suite; what is checked here is that the panel asks for the right one. */

static alarm_sound_t s_heard_pattern;
static int s_heard_one_shots;

static void heard_continuous(void *ctx, alarm_sound_t sound)
{
    (void)ctx;
    s_heard_pattern = sound;
}

static void heard_one_shot(void *ctx, alarm_sound_t sound)
{
    (void)ctx;
    (void)sound;
    s_heard_one_shots++;
}

static void expect_sounder(alarm_sound_t want, const char *when)
{
    if (s_heard_pattern != want) {
        fprintf(stderr, "sounder: %s: the panel asks for %s, not %s\n", when,
                alarm_sound_name(s_heard_pattern), alarm_sound_name(want));
        exit(1);
    }
}

static void remote_arm_away(void)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_AWAY;
    snprintf(cmd.code, sizeof(cmd.code), "123456");
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "sounder: the house would not arm");
}

/* Runs last, and takes no screenshots: it resets the world, so nothing may follow it. */
#if ALARM_LV_BUDGET_KB == 0
static void shot(const char *name);

/* Presses the open keyboard's key that shows this text, as a finger would (the keyboard's own
 * handler runs). */
static void press_key(const char *text)
{
    lv_obj_t *kb = by_name("sheet_kb");
    for (uint32_t i = 0; i < 64u; i++) {
        const char *k = lv_buttonmatrix_get_button_text(kb, i);
        if (k == NULL) {
            break;
        }
        if (strcmp(k, text) == 0) {
            lv_buttonmatrix_set_selected_button(kb, i);
            lv_obj_send_event(kb, LV_EVENT_VALUE_CHANGED, &i);
            run(50);
            return;
        }
    }
    fprintf(stderr, "no key \"%s\" on the keyboard\n", text);
    exit(1);
}

/* docs/features/37 G2 (I18N-009, -010, -011, -019): a name with an accent is typed on the
 * keyboard's accent layer, kept in bytes and cut on whole characters, and drawn; a character
 * the fonts don't carry draws as a box, not as nothing. */
static void walk_unicode_names(void)
{
    printf("Names in other scripts\n");
    /* With the letters pack (host_panel.c hands out the layout's parts, as a panel the Flasher set
     * up has them; OPT-010): the four text fonts behind which it hangs draw every letter. The
     * headline draws only the panel's own words and has no part in the pack (asset-packs.md
     * "Decisions / The letters"): it is checked for Greek only. */
    lv_font_t **const text_fonts[] = {&font_caption, &font_body, &font_label, &font_title,
                                      &font_headline};
    for (size_t f = 0; f < sizeof(text_fonts) / sizeof(text_fonts[0]); f++) {
        lv_font_glyph_dsc_t g;
        for (uint32_t cp = 0xA1u; *text_fonts[f] != font_headline && cp <= 0x17Fu; cp++) {
            if (cp == 0xADu || cp == 0x149u) {
                continue; /* the soft hyphen draws nothing; Inter lacks the deprecated U+0149 */
            }
            if (!lv_font_get_glyph_dsc(*text_fonts[f], &g, cp, 0)) {
                printf("  font %u has no U+%04X\n", (unsigned)f, (unsigned)cp);
            }
            expect(lv_font_get_glyph_dsc(*text_fonts[f], &g, cp, 0),
                   "a text font lacks a Latin-1 or Latin Extended-A letter");
        }
        expect(!lv_font_get_glyph_dsc(*text_fonts[f], &g, 0x3A9u, 0),
               "a text font has a Greek letter it wasn't given");
    }
    expect(LV_USE_FONT_PLACEHOLDER, "a missing character would draw as nothing");

    wake();
    tap_name("bar_settings");
    tap_name("set_users");
    type("123456k");
    run(300);
    tap_text("Add a user");
    type("3579k");
    type("3579k");
    run(300);
    tap_text("Name");
    run(200);
    lv_obj_t *ta = by_name("sheet_text");
    /* More than the name holds, in two-byte letters: whole ones only, up to its 47 bytes. */
    lv_textarea_set_text(ta, "");
    /* As a paste from the remote keypad arrives (ui_forms_remote_text): one insert. */
    lv_textarea_add_text(ta, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                             "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                             "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                             "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");
    const char *typed = lv_textarea_get_text(ta);
    expect(strlen(typed) == 46u, "a name wasn't kept to its bytes on a whole character");
    expect(i18n_utf8_valid(typed, strlen(typed)), "a name was cut inside a character");
    lv_textarea_set_text(ta, "");
    /* "Zoë" on the keys: a capital, a letter, then the accent layer for one letter. The sheet
     * opened on the new person's given name; an empty field starts on the capitals. */
    lv_keyboard_set_mode(by_name("sheet_kb"), LV_KEYBOARD_MODE_TEXT_UPPER);
    press_key("Z");
    press_key("o");
    press_key("\xC3\xA0\xC3\xA9");
    press_key("\xC3\xAB");
    expect(strcmp(lv_textarea_get_text(ta), "Zo\xC3\xAB") == 0, "the accent layer typed wrong");
    /* The layer's second page and its capitals are there too. */
    press_key("\xC3\xA0\xC3\xA9");
    press_key("\xC4\x8D\xC5\x82");
    press_key("\xEE\x87\xA4"); /* shift (GLYPH_SHIFT in ui_forms.c) */
    expect(lv_buttonmatrix_get_button_text(by_name("sheet_kb"), 0) != NULL &&
               strcmp(lv_buttonmatrix_get_button_text(by_name("sheet_kb"), 0), "\xC4\x8C") == 0,
           "the accent layer's capitals aren't there");
    shot("keyboard_accents");
    press_key("abc");
    tap_name("sheet_done");
    run(500);
    expect(find_label(by_name("page_body"), "Zo\xC3\xAB") != NULL, "the accented name wasn't kept");
    shot("unicode_names");
    tap_name("nav_home");
    run(300);
}
#endif

#if ALARM_LV_BUDGET_KB == 0
/* docs/features/37 G3: the language rows of Settings > Time and language (I18N-012, I18N-013;
 * BL-131) and a language picked there reaching every screen at once (I18N-014). Appended last:
 * earlier shots keep their numbers. */
static void walk_language(void)
{
    printf("Time and language\n");
    wake();
    tap_name("bar_settings");
    run(300);
    tap_name("set_time");
    run(300);
    expect(find_label(by_name("page_body"), "Region") == NULL,
           "Time and language: the region opened without a code");
    expect(find_label(by_name("page_body"), "Clock") != NULL,
           "Time and language: the 12/24-hour clock needs a code");
    expect(find_visible(lv_screen_active(), "page_note") != NULL,
           "Time and language without a code doesn't say what a code changes");
    shot("language_locked"); /* the clock is anyone's */
    tap_text("Enter a master or installer code");
    run(300);
    type("123456k"); /* the language and the region need a master or installer code */
    run(300);
    expect(find_label(by_name("page_body"), "Region") != NULL,
           "Time and language: a master's code didn't open the region");
    expect(find_visible(lv_screen_active(), "page_note") == NULL,
           "BL-131: Time and language has a note, or an empty line under its title");
    shot("language_region");

    tap_text("Region");
    run(300);
    shot("region_pick");
    tap_text("Germany");
    run(1200);
    expect(lv_subject_get_string(&subject_ampm)[0] == '\0',
           "I18N-016: a region with a 24-hour clock still shows AM/PM");
    expect(find_label_with(by_name("page_body"), "12.345,6") != NULL,
           "I18N-017: a German number isn't written 12.345,6");
#ifndef ALARM_PREVIEW_LOCALE
    expect(find_label_with(by_name("page_body"), " Sep, ") != NULL,
           "I18N-016: English in Germany doesn't put the day before the month");
#endif
    tap_text("Clock");
    run(300);
    shot("clock_pick");
    tap_text("12-hour");
    run(1200);
    expect(lv_subject_get_string(&subject_ampm)[0] != '\0',
           "I18N-016: the 12-hour clock chosen over the region's shows no AM/PM");
    expect(find_label(by_name("page_body"), "12-hour") != NULL,
           "BL-131: the Clock row doesn't say the clock is 12-hour");
    /* As the walk found it: the region's clock, the language's region. */
    tap_text("Clock");
    run(300);
    tap_text("Automatic");
    run(300);
    expect(find_label(by_name("page_body"), "24-hour") != NULL,
           "BL-131: the Clock row doesn't say what Automatic gives in Germany");
    tap_text("Region");
    run(300);
    tap_text("Automatic");
    run(1200);
    expect(lv_subject_get_string(&subject_ampm)[0] != '\0', "the language's region lost its clock");

    tap_link("Language");
    run(300);
    shot("language_pick");
    tap_text(i18n_language_name(NULL)); /* the first of the list, in its own name */
    run(300);
#ifdef ALARM_PREVIEW_LOCALE
    /* Picked on the page: every screen changes at once, this page and the rail included. */
    s_locale = false;
    const bool english = find_label(lv_screen_active(), "Time and language") != NULL &&
                         find_label(lv_screen_active(), "Security") != NULL;
    s_locale = true;
    expect(english, "I18N-014: English picked on the page didn't reach the page and the rail");
    tap_link("Language");
    run(300);
    tap_text(i18n_language_name(s_cat)); /* back to the test language, by its own name */
    run(300);
    expect(find_label(lv_screen_active(), i18n_text(s_cat, NULL, "Time and language")) != NULL,
           "I18N-014: the test language picked on the page didn't come back");
#endif
    tap_name("nav_home");
    run(300);
}

/* Language packs (docs/features/37 language-packs.md; I18N-042, I18N-043): the screen reads the
 * installed languages through the backend. A pretend panel: es-US ready but three phrases short
 * and a draft, fr-CA made for other firmware, de-DE refused. It runs after every other shot (and
 * the Z-Wave ones) so the earlier pictures and their numbers stay as they were; the screens are
 * English whatever the walk's language, because the stored languages are not installed. */
static void stored_language(const char *lang)
{
    ui_options_t o;
    ui_app_default_options(&o);
    snprintf(o.lang, sizeof(o.lang), "%s", lang);
    ui_app_apply_options(&o);
    run(300);
}

static void open_language_page(void)
{
    wake();
    tap_name("bar_settings");
    run(300);
    tap_name("set_time");
    run(300);
    if (find_label(by_name("page_body"), "Region") == NULL) {
        tap_text("Enter a master or installer code");
        run(300);
        type("123456k");
        run(300);
    }
}

static void walk_language_packs(void)
{
    printf("Language packs\n");
#ifdef ALARM_PREVIEW_LOCALE
    s_locale = false;
#endif
    host_panel_init(1789857660000LL); /* a quiet, disarmed house after the sounder's checks */
    run(500);
    host_panel_lang_world(true);
    stored_language("it-IT");
    run(1200);
    open_language_page();
    expect(find_label_with(by_name("page_body"), "Italian isn't installed: the screen shows English") != NULL,
           "I18N-042: a stored language that isn't installed isn't said to show English");
    shot("packs_not_installed");

    tap_link("Language");
    run(300);
    expect(find_label(lv_screen_active(), "Espa\xC3\xB1ol") != NULL &&
               find_label(lv_screen_active(), "Fran\xC3\xA7" "ais (Canada)") != NULL,
           "I18N-042: the installed languages aren't listed");
    expect(find_label(lv_screen_active(), "Deutsch") == NULL,
           "I18N-042: a refused language is listed");
    expect(find_label_with(lv_screen_active(), "3 phrases show in English") != NULL,
           "I18N-043: a language with phrases missing doesn't say so");
    expect(find_label_with(lv_screen_active(), "Made for version 0.6.0: some phrases may show in English") !=
               NULL,
           "I18N-043: a language made for other firmware doesn't say so");
    shot("packs_language_list");
    tap_text(i18n_language_name(NULL)); /* English again: the list closes */
    run(300);
    expect(find_label_with(by_name("page_body"), "isn't installed") == NULL,
           "I18N-042: English is said not to be installed");

    /* The panel loads a language after the screen asked for it: the generation changes and the
     * page open on it is drawn again. fr-CA isn't there at first, then it is. */
    tap_name("nav_home");
    run(300);
    host_panel_lang_world(false);
    stored_language("fr-CA");
    open_language_page();
    expect(find_label_with(by_name("page_body"), "French isn't installed") != NULL,
           "I18N-042: a language the panel doesn't have yet isn't said to be missing");
    host_panel_lang_world(true);
    run(1500);
    expect(find_label_with(by_name("page_body"), "isn't installed") == NULL,
           "I18N-042: the page didn't look again when the language generation changed");
    shot("packs_loaded_later");
    tap_link("Language");
    run(300);
    shot("packs_language_list_fr");
    tap_text(i18n_language_name(NULL));
    run(300);
    stored_language("en-US");
    host_panel_lang_world(false);
    tap_name("nav_home");
    run(300);

    /* A language's own font (I18N-049): the pretend pack el-GR brings a Greek body font, hung
     * behind the built-in body font while the language is on, and taken away again with it. The
     * built-in fonts have no Greek (walk_unicode_names checks that); this comes last so every
     * earlier shot keeps its number. */
    lv_font_glyph_dsc_t glyph;
    expect(!lv_font_get_glyph_dsc(font_body, &glyph, 0x3A9u, 0),
           "I18N-049: the body font has Greek before any language brought it");
    host_panel_zone_name(6, "\xCE\x9A\xCE\xBF\xCF\x85\xCE\xB6\xCE\xAF\xCE\xBD\xCE\xB1");
    host_panel_lang_world(true);
    host_panel_lang_greek(true);
    stored_language("el-GR");
    run(1200);
    expect(lv_font_get_glyph_dsc(font_body, &glyph, 0x3A9u, 0) &&
               lv_font_get_glyph_dsc(font_body, &glyph, 0x3B1u, 0),
           "I18N-049: the language's font isn't behind the body font");
    expect(lv_font_get_glyph_dsc(font_body, &glyph, 'A', 0) && glyph.adv_w > 0,
           "I18N-049: the built-in letters are gone behind the fallback");
    tap_name("nav_zones");
    run(500);
    shot("packs_greek_zone");
    tap_name("nav_home");
    run(300);
    stored_language("en-US");
    run(600);
    expect(!lv_font_get_glyph_dsc(font_body, &glyph, 0x3A9u, 0),
           "I18N-049: the language's font stayed behind the body font after English came back");
    host_panel_lang_greek(false);
    host_panel_lang_world(false);
#ifdef ALARM_PREVIEW_LOCALE
    s_locale = true;
#endif
}

/* The backlight the UI asked for is this (UI-009, UI-043). */
static void expect_light(int percent, const char *what)
{
    if (host_panel_backlight() != percent) {
        fprintf(stderr, "backlight: %s: %d %%, expected %d %%\n", what, host_panel_backlight(),
                percent);
        exit(1);
    }
}

/* UI-009, UI-043: dimmed to the screen saver, then dark; straight to dark when that comes first;
 * the night hours, dimmer and dark sooner; a touch wakes it without pressing anything behind, an
 * alarm lights it as by day, and without the time it is never night. After every other shot: it
 * changes the options and the time. */
static void walk_screen_light(const ui_options_t *start)
{
    printf("Screen off and night hours\n");
    host_panel_init(1789857660000LL + 12LL * 3600000LL); /* 10:41 in the morning */
    ui_options_t o = *start;
    o.off_after_ms = 300000u; /* dim after a minute (the default), dark after five */
    ui_app_apply_options(&o);
    wake();
    tap_name("nav_home");
    run(300);
    expect_light(80, "UI-043: in use by day");
    run(58000);
    expect_saver(false, "UI-009: before the saver's minute");
    run(3000);
    expect_saver(true, "UI-009: after the saver's minute");
    expect_light(25, "UI-009: the screen saver");
    run(240000);
    expect_saver(true, "UI-043: dark is the saver's screen, which takes the waking touch");
    expect_light(0, "UI-043: five minutes after the last touch");
    tap_xy(W / 2, H / 2);
    run(300);
    expect_saver(false, "UI-043: a touch wakes the dark screen");
    expect_light(80, "UI-043: awake again");
    expect(lv_subject_get_int(&subject_view) == 0, "UI-043: the waking touch pressed something");

    /* Dark sooner than the saver: straight to dark, no photo first. */
    o.off_after_ms = 30000u;
    ui_app_apply_options(&o);
    run(31000);
    expect_light(0, "UI-043: screen off before the saver");
    wake();
    run(300);

    /* Night hours from 10:00 to 12:00, so now: dimmer, and dark after the night's minute. */
    o.off_after_ms = 0u;
    o.night_on = true;
    o.night_start = 10u * 60u;
    o.night_end = 12u * 60u;
    ui_app_apply_options(&o);
    run(300);
    expect_light(20, "UI-043: in use at night");
    run(61000);
    expect_light(0, "UI-043: a minute after the last touch at night");
    wake();
    run(300);
    expect_light(20, "UI-043: woken at night");
    host_panel_zone(6, ALARM_ZEV_FAULT); /* smoke */
    run(300);
    expect_light(80, "UI-043: a fire alarm at night");
    host_panel_init(1789857660000LL + 12LL * 3600000LL + 120000LL); /* quiet again */
    run(300);
    expect_light(20, "UI-043: night again once the alarm is over");

    /* Screen and sound with the night hours on (the cut-off check reads every row). */
    tap_name("bar_settings");
    run(300);
    lv_obj_t *off_row = find_label(by_name("page_body"), "Screen off");
    expect(off_row != NULL, "UI-043: no Screen off row");
    expect(find_label(by_name("page_body"), "Chime") == NULL,
           "BL-131: Screen and sound has a Chime row (the status bar's switch is its one place)");
    expect(find_label(by_name("page_body"), "Night hours") != NULL &&
               find_label(by_name("page_body"), "10:00 AM to 12:00 PM*") != NULL,
           "BL-131: the Night hours row doesn't say its hours");
    /* The whole row in view, its second line too, not only its title */
    while (off_row != NULL && !lv_obj_has_flag(off_row, LV_OBJ_FLAG_CLICKABLE)) {
        off_row = lv_obj_get_parent(off_row);
    }
    if (off_row != NULL) {
        lv_obj_scroll_to_view_recursive(off_row, LV_ANIM_OFF);
        run(300);
        shot("settings_display_screen_off");
    }
    /* The screen saver's own page: when it starts and how bright it is */
    tap_text("Screen saver");
    run(300);
    expect(find_label(by_name("page_body"), "Screen saver brightness") != NULL,
           "BL-131: the Screen saver page has no brightness");
    shot("settings_display_saver");
    tap_name("page_back");
    run(300);
    /* The night hours' page, and the sheet that sets both ends with rollers */
    tap_text("Night hours");
    run(300);
    shot("settings_display_night");
    expect(find_label(by_name("page_body"), "10:00 AM to 12:00 PM") != NULL,
           "UI-043: the night's hours aren't shown as times of day");
    tap_text("Hours");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 5, "BL-131: the Hours row doesn't open the night hours");
    expect(find_label_with(by_name("night_sheet"), "2 hours") != NULL,
           "BL-131: the night hours sheet doesn't say how long the night lasts");
    shot("settings_night_hours");
    /* Cancel leaves the hours as they were */
    roll_in("night_start", "roll_hour", 9);
    tap_name("night_cancel");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 0, "BL-131: Cancel didn't close the night hours");
    expect(find_label(by_name("page_body"), "10:00 AM to 12:00 PM") != NULL,
           "BL-131: Cancel changed the night hours");
    /* 9:15 PM to 5:45 AM, set through the rollers */
    tap_text("Hours");
    run(300);
    roll_in("night_start", "roll_hour", 9);
    roll_in("night_start", "roll_minute", 1); /* :15 */
    roll_in("night_start", "roll_ampm", 1);
    roll_in("night_end", "roll_hour", 5);
    roll_in("night_end", "roll_minute", 3); /* :45 */
    roll_in("night_end", "roll_ampm", 0);
    expect(find_label_with(by_name("night_sheet"), "8 h 30 min \xC2\xB7 9:15 PM to 5:45 AM") != NULL,
           "BL-131: the rollers don't set the hours the sheet says");
    shot("settings_night_hours_set");
    tap_name("night_ok");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 0, "BL-131: Done didn't close the night hours");
    expect(find_label(by_name("page_body"), "9:15 PM to 5:45 AM") != NULL,
           "BL-131: the night hours weren't saved");
    /* A choice made on the page leaves it where it was (B015 UX-028). */
    const int32_t scrolled = lv_obj_get_scroll_y(by_name("page_body"));
    tap_text("Screen off at night");
    run(300);
    tap_text("After 5 minutes");
    run(300);
    expect(lv_obj_get_scroll_y(by_name("page_body")) == scrolled,
           "UI-043: picking the night's screen-off time scrolled the page");
    expect(find_label(by_name("page_body"), "After 5 minutes") != NULL,
           "UI-043: the night's screen-off time didn't change");
    tap_name("nav_home");
    run(300);

    /* No time, no night: the screen keeps the day's brightness. */
    host_panel_init(0);
    run(1500);
    expect_light(80, "UI-043: night hours without the time");

    /* The night hours begin and end by the clock alone, with nobody touching anything: a time
     * kept from the wrong moment once left the 433 MHz picker at "just now" (c88d544). Never
     * dimmed or dark here, so only the night's brightness changes: 10:43 to 10:45. */
    host_panel_init(1789857660000LL + 12LL * 3600000LL - 5000LL); /* 10:41:00 once started */
    o.saver_after_ms = 0u;
    o.off_after_ms = 0u;
    o.night_off_after_ms = 0u;
    o.night_start = 10u * 60u + 43u;
    o.night_end = 10u * 60u + 45u;
    ui_app_apply_options(&o);
    run(1000);
    expect_light(80, "UI-043: before the night hours");
    run(118000); /* 10:42:59 */
    expect_light(80, "UI-043: a second before the night hours");
    run(2000); /* 10:43:01 */
    expect_light(20, "UI-043: the night hours didn't begin by the clock alone");
    run(118000); /* 10:44:59 */
    expect_light(20, "UI-043: a second before the night hours end");
    run(2000); /* 10:45:01 */
    expect_light(80, "UI-043: the night hours didn't end by the clock alone");

    host_panel_init(1789857660000LL);
    ui_app_apply_options(start);
    run(300);
}

/* The engine's walk test command, as the Panel Manager sends it (signed in as the master). */
static alarm_result_t manager_walk(alarm_walk_op_t op)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_WALK_TEST;
    cmd.partition = 1;
    cmd.walk_op = (uint8_t)op;
    snprintf(cmd.code, sizeof(cmd.code), "123456");
    return host_panel_manager_command(&cmd);
}

/* docs/features/40: Settings > Walk test (a master code, WALK-001), Start, Home during the test
 * with zones tripped (WALK-013), the screen lit at night (WALK-022), the list (WALK-009), Extend,
 * End and the summary after it (WALK-010), a test started from the Panel Manager - over the screen
 * saver and while someone is busy at the panel (WALK-023) - with its end warning (WALK-004),
 * arming refused while a test runs (WALK-005) and the installer menu's entry. After every other
 * shot. */
static void walk_walk_test(const ui_options_t *start)
{
    printf("Walk test\n");
    host_panel_init(1789857660000LL); /* quiet and disarmed, 10:41 PM */
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);

    /* WALK-001: the section asks for a code that may start one, then offers the start. */
    tap_name("bar_settings");
    run(300);
    tap_name("set_walk");
    run(300);
    expect(lv_subject_get_int(&subject_settings_pad) == 1,
           "WALK-001: Settings > Walk test opened without a code");
    type("123456k");
    run(300);
    expect(find_label(by_name("page_body"), "Include panic and 24-hour zones") != NULL,
           "WALK-007: no switch for panic and 24-hour zones");
    expect(find_label(by_name("page_body"),
                      "The siren sounds for 1 s at the start \xC2\xB7 ends after 60 min") != NULL,
           "WALK-003: the start doesn't say the siren sounds, or when the test ends");
    shot("settings_walk_test");
    tap_text("Start walk test");
    run(500);
    expect(lv_subject_get_int(&subject_view) == 0, "WALK-013: Start didn't go back to Home");
    expect(lv_subject_get_int(&subject_panel) == 4, "WALK-013: Home doesn't show the walk test");
    expect(find_label(lv_screen_active(), "0 of *") != NULL, "WALK-013: Home has no count");
    expect(find_label(lv_screen_active(), "Ends in 60 min") != NULL,
           "WALK-013: Home doesn't say when the test ends");

    /* A door twice, a window, motion that stays, a cover off. */
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(300);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(300);
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(300);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(300);
    host_panel_zone(3, ALARM_ZEV_FAULT);
    run(300);
    host_panel_zone(3, ALARM_ZEV_RESTORE);
    host_panel_zone(5, ALARM_ZEV_FAULT);
    host_panel_zone(4, ALARM_ZEV_TAMPER);
    run(1500);
    expect(find_label(lv_screen_active(), "3 of *") != NULL,
           "WALK-013: Home's count didn't follow the zones tested");
    shot("home_walk_test");

    /* WALK-022: night hours now, and nobody touching: the screen stays awake at the day's level. */
    ui_options_t o = *start;
    o.night_on = true;
    o.night_start = 22u * 60u;
    o.night_end = 23u * 60u;
    ui_app_apply_options(&o);
    run(300);
    expect_light(80, "WALK-022: a walk test at night is dimmed");
    run(70000);
    expect_saver(false, "WALK-022: the screen saver came during a walk test");
    expect_light(80, "WALK-022: the screen went dark during a walk test");
    ui_app_apply_options(start);
    run(300);

    /* WALK-009: the list, without a code; its Extend asks for one (WALK-001). */
    tap_name("walk_zones");
    run(1500);
    expect(lv_subject_get_int(&subject_settings_pad) == 0, "WALK-009: the list asked for a code");
    lv_obj_t *body = by_name("page_body");
    tap(find_label(body, "Extend"));
    run(300);
    expect(lv_subject_get_int(&subject_settings_pad) == 1,
           "WALK-001: the list's Extend, opened without a code, asked for none");
    tap_name("pad_close");
    run(300);
    expect(find_label(body, "Not tested") != NULL, "WALK-013: the list has no untested zones first");
    expect(find_label(body, "Tested") != NULL, "WALK-013: the list has no tested zones");
    expect(find_label(body, "Zone 1 \xC2\xB7 Tested * \xC2\xB7 2 times") != NULL ||
               find_label(body, "Zone 1 \xC2\xB7 Tested*") != NULL,
           "WALK-009: the front door doesn't say it was tested");
    expect(find_label(body, "Zone 5 \xC2\xB7 Open now*") != NULL,
           "WALK-009: the motion sensor doesn't say it is open now");
    expect(find_label(body, "Zone 4 \xC2\xB7 Cover tested") != NULL,
           "WALK-008: the back door doesn't say its cover was tested");
    shot("walk_test_list");
    lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF);
    run(300);
    expect(find_label(body, "Stay on during the test") != NULL,
           "WALK-007: the list doesn't say fire and CO stay on");
    expect(find_label(body, "Zone 6 \xC2\xB7 Its test button marks it") != NULL,
           "WALK-007: the smoke detector doesn't say its test button marks it");
    shot("walk_test_list_end");

    /* A zone tripped while the list is open moves to Tested within a second (UI-023). */
    host_panel_zone(2, ALARM_ZEV_FAULT);
    run(300);
    host_panel_zone(2, ALARM_ZEV_RESTORE);
    run(1500);
    expect(find_label(by_name("page_body"), "Zone 2 \xC2\xB7 Tested*") != NULL,
           "WALK-013: the list didn't follow a zone tested while it was open");
    host_panel_zone(5, ALARM_ZEV_RESTORE); /* the house ready to arm again */
    host_panel_zone(4, ALARM_ZEV_TAMPER_RESTORE);
    tap_name("nav_home");
    run(300);

    /* WALK-004: Extend, with a code, starts the full time again. */
    expect(find_label(lv_screen_active(), "Ends in 59 min") != NULL,
           "WALK-013: the time left didn't count down");
    tap_name("walk_extend");
    run(300);
    expect(lv_subject_get_int(&subject_panel) == 2, "WALK-004: Extend asks for no code");
    type("123456k");
    run(1100); /* the time left changes at most once a second (UI-023): wait a whole one */
    expect(find_label(lv_screen_active(), "Ends in 60 min") != NULL,
           "WALK-004: Extend didn't start the time again");

    /* WALK-025: End asks a question, not a code. */
    tap_name("walk_end");
    run(300);
    expect(lv_subject_get_int(&subject_panel) == 4 &&
               find_label(lv_screen_active(), "End the walk test?") != NULL,
           "WALK-025: End didn't ask before ending the walk test");
    shot("walk_test_end_confirm");
    tap_text("Keep testing");
    run(300);
    expect(lv_subject_get_int(&subject_panel) == 4, "WALK-025: Keep testing ended the walk test");
    tap_name("walk_end");
    run(300);
    tap_text("End test");
    run(1500);
    expect(lv_subject_get_int(&subject_panel) == 0, "WALK-025: End didn't end the walk test");

    /* WALK-010: the summary - a steady banner on Home until it is seen (or the partition arms),
     * then Settings > Walk test, without a code, with the count, how it ended and the untested
     * zones first; a new start needs a code (WALK-001). */
    expect(lv_subject_get_int(&subject_bar_banner) == 4, "WALK-010: no summary after the end");
    expect(bar_shows("Walk test ended: *"),
           "WALK-010: the banner doesn't count the zones tested");
    shot("home_walk_test_ended");
    tap_name("walk_banner");
    run(500);
    expect(lv_subject_get_int(&subject_settings_pad) == 0, "WALK-010: the summary asked for a code");
    expect(find_label(lv_screen_active(), "Last walk test: *") != NULL,
           "WALK-010: Settings > Walk test has no summary");
    expect(find_label_with(lv_screen_active(), "Ended before its time ran out") != NULL,
           "WALK-010: the summary doesn't say how it ended");
    expect(find_label(by_name("page_body"), "Not tested") != NULL,
           "WALK-010: the summary doesn't name the untested zones");
    expect(find_label(by_name("page_body"), "Start walk test") == NULL,
           "WALK-001: a start beside the summary opened without a code");
    shot("walk_test_summary");
    tap_text("Enter a master or installer code");
    run(300);
    type("123456k");
    run(500);
    expect(find_label(by_name("page_body"), "Start walk test") != NULL,
           "WALK-002: no new start beside the summary once the code was given");
    tap_name("nav_home");
    run(300);
    expect(lv_subject_get_int(&subject_bar_banner) != 4,
           "WALK-010: the banner stayed after the summary was seen");

    /* WALK-023: a test started from the Panel Manager wakes the screen and shows itself, headed
     * by who started it and from where. */
    run(62000);
    expect_saver(true, "WALK-023: the screen saver before the test");
    expect(manager_walk(ALARM_WALK_START) == ALARM_OK,
           "WALK-023: the Panel Manager's start was refused");
    run(1500);
    expect_saver(false, "WALK-023: a test started elsewhere didn't wake the screen");
    expect(lv_subject_get_int(&subject_view) == 0 && lv_subject_get_int(&subject_panel) == 4,
           "WALK-023: a test started elsewhere doesn't show at the panel");
    expect(find_label(by_name("walk_from"), "Walk test started by *") != NULL &&
               find_label_with(by_name("walk_from"), "from the Panel Manager") != NULL,
           "WALK-023: Home doesn't say who started the test, and from where");
    shot("home_walk_test_from_panel_manager");

    /* WALK-004: five minutes before the limit the screen says so, steadily, beside Extend; away
     * from Home the status bar does, and leads back to it. */
    host_panel_advance(55u * 60000u);
    run(1500);
    expect(find_label(lv_screen_active(), "Walk test ends in 5 minutes") != NULL &&
               lv_subject_get_int(&subject_walk_warn) == 1,
           "WALK-004: Home doesn't say the test ends in 5 minutes");
    shot("home_walk_test_ends_soon");
    tap_name("nav_zones");
    run(300);
    expect(lv_subject_get_int(&subject_bar_banner) == 4 &&
               bar_shows("Walk test ends in 5 min \xC2\xB7 Show"),
           "WALK-004: away from Home, nothing says the test ends soon");
    shot("zones_walk_test_ends_soon");
    tap_name("walk_banner");
    run(300);
    expect(lv_subject_get_int(&subject_view) == 0 && lv_subject_get_int(&subject_panel) == 4,
           "WALK-004: the banner doesn't lead to Home's Extend");
    expect(manager_walk(ALARM_WALK_END) == ALARM_OK, "WALK-002: the Panel Manager's end was refused");
    run(500);

    /* WALK-005 and WALK-023: an arming pad open when a test starts elsewhere stays the person's:
     * a banner says a test started, arming says why it can't, and the test shows once they close
     * the pad. */
    tap_text("Away");
    run(300);
    expect(manager_walk(ALARM_WALK_START) == ALARM_OK,
           "WALK-023: the Panel Manager's start was refused");
    run(500);
    expect(lv_subject_get_int(&subject_panel) == 2,
           "WALK-023: a test started elsewhere took the screen from the person using it");
    expect(bar_shows("Walk test from the Panel Manager \xC2\xB7 Show"),
           "WALK-023: no banner for a test started elsewhere");
    type("123456k");
    run(300);
    expect(find_label(lv_screen_active(), "A walk test is running") != NULL,
           "WALK-005: arming during a walk test doesn't say why it can't");
    shot("walk_test_arm_refused");
    tap_name("pad_close");
    run(1500);
    expect(lv_subject_get_int(&subject_panel) == 4 &&
               find_label_with(by_name("walk_from"), "from the Panel Manager") != NULL,
           "WALK-023: the test didn't show once the pad was closed");
    expect(manager_walk(ALARM_WALK_END) == ALARM_OK, "WALK-002: the Panel Manager's end was refused");
    run(500);

    /* The installer menu's Walk test leaves installer mode and starts the test in one tap
     * (docs/features/40 "Touchscreen"; the engine keeps the two apart, CORE-PROG-002/003). */
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 0 &&
        find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        run(300);
    }
    type("69420k");
    run(500);
    lv_obj_t *walk_row = find_label(by_name("page_body"), "Leaves installer mode and starts one");
    expect(walk_row != NULL, "the installer menu has no walk test");
    scroll_into_view(walk_row);
    run(300);
    shot("installer_walk_test");
    tap_text("Leaves installer mode and starts one");
    run(1500);
    expect(lv_subject_get_int(&subject_view) == 0 && lv_subject_get_int(&subject_panel) == 4,
           "the installer menu's Walk test didn't start one");
    expect(lv_subject_get_int(&subject_walk_from_on) == 0,
           "WALK-023: a test started at this panel says it came from elsewhere");
    expect(manager_walk(ALARM_WALK_END) == ALARM_OK, "WALK-002: the Panel Manager's end was refused");
    run(500);

    host_panel_init(1789857660000LL);
    run(300);
}
#endif /* ALARM_LV_BUDGET_KB == 0 */

static void shot(const char *name);

/* Every shown child of a row lies inside the card around it (the arm pad's option chips). */
static void expect_inside(const char *row, const char *card, const char *what)
{
    lv_area_t c;
    lv_obj_get_coords(by_name(card), &c);
    lv_obj_t *r = by_name(row);
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++) {
        lv_obj_t *o = lv_obj_get_child(r, (int32_t)i);
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        expect(a.x1 >= c.x1 && a.x2 <= c.x2 && a.y1 >= c.y1 && a.y2 <= c.y2, what);
    }
}

/* The arm pad when the house isn't ready: No entry delay, Silent exit and Bypass open, all
 * inside the card, the keys too. Every layout; the last shot of the full UI's walk. */
static void walk_arm_options(void)
{
    printf("The arm pad's options when the house isn't ready\n");
    host_panel_init(1789857660000LL);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(1500);
    tap_text("Away");
    run(300);
    expect(lv_subject_get_int(&subject_opt_bypass_avail) == 1,
           "the not-ready arm pad doesn't offer Bypass open");
    expect_inside("pad_options", "pad", "an arming option lies outside the pad's card");
    expect_inside("pad_keys", "pad", "a key lies outside the pad's card");
    shot("pad_arm_not_ready_options");
    tap_name("pad_close");
    run(300);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    host_panel_init(1789857660000LL);
    run(300);
}

static void check_the_sounder(void)
{
    printf("The sounder, from the engine to the keypad\n");
    const host_panel_sound_t recorder = {
        .ctx = NULL,
        .continuous = heard_continuous,
        .one_shot = heard_one_shot,
    };
    host_panel_sound(&recorder);
    host_panel_init(1789857660000LL); /* a quiet, disarmed house again */
    run(500);
    expect_sounder(ALARM_SOUND_NONE, "disarmed and ready");

    remote_arm_away(); /* from a phone: no screen involved */
    run(500);
    expect_sounder(ALARM_SOUND_EXIT, "during the exit time");
    run(51000);
    expect_sounder(ALARM_SOUND_EXIT_FAST, "in the last 10 s of the exit time");
    run(11000);
    expect_sounder(ALARM_SOUND_NONE, "once armed");

    host_panel_zone(1, ALARM_ZEV_FAULT); /* the front door, on the way in */
    run(500);
    expect_sounder(ALARM_SOUND_ENTRY, "during the entry time");

    host_panel_zone(6, ALARM_ZEV_FAULT); /* smoke outranks everything else (AUD-001) */
    run(500);
    expect_sounder(ALARM_SOUND_FIRE, "with a fire alarm over an entry time");

    expect(s_heard_one_shots > 0,
           "sounder: the engine's one-shot sounds never reached the keypad");
    host_panel_sound(NULL);
}

/* The object on o's way up whose parent is a. */
static lv_obj_t *child_towards(lv_obj_t *a, lv_obj_t *o)
{
    while (o != NULL && lv_obj_get_parent(o) != a) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

static bool outside(const lv_area_t *in, const lv_area_t *box, bool across)
{
    return across ? (in->x1 < box->x1 - 1 || in->x2 > box->x2 + 1)
                  : (in->y1 < box->y1 - 1 || in->y2 > box->y2 + 1);
}

/* The box that cuts this label's text off, or NULL: text that fits its label can still run out
 * of the button or card around it. Rows of a list that scrolls run past its edge as whole rows
 * (the row carrying the text runs out too), and that is scrolling, not clipping. Only the
 * layout's scroll surfaces (UI_FLAG_SCROLL_SURFACE, user_1 in the XML) scroll up and down on
 * purpose; every LVGL object can scroll, but nobody would guess that a card or a column does. */
static lv_obj_t *spilled_from(lv_obj_t *label, const lv_area_t *text)
{
    for (lv_obj_t *a = lv_obj_get_parent(label); a != NULL; a = lv_obj_get_parent(a)) {
        if (lv_obj_has_flag(a, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
            continue;
        }
        lv_area_t box;
        lv_obj_get_coords(a, &box);
        const bool across = outside(text, &box, true);
        const bool down = outside(text, &box, false);
        if (!across && !down) {
            continue;
        }
        lv_area_t seen;
        if (!lv_area_intersect(&seen, text, &box)) {
            return NULL; /* scrolled out of sight */
        }
        lv_obj_t *row = child_towards(a, label);
        lv_area_t row_box;
        lv_obj_get_coords(row, &row_box);
        const bool scrolls = lv_obj_has_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        const lv_dir_t dir = lv_obj_get_scroll_dir(a);
        const bool surface = scrolls && lv_obj_has_flag(a, LV_OBJ_FLAG_USER_1);
        if (!across && surface && (dir & LV_DIR_VER) != 0 &&
            (row == label || outside(&row_box, &box, false))) {
            return NULL;
        }
        /* Smart Home's area tabs scroll across on purpose: a long note takes its room from them,
         * the open tab kept in view (UI-020, BL-171), so a tab past their edge is scrolled, not
         * cut. Their box is a scrollable row that LVGL lets scroll either way (BL-216). */
        const char *box_name = lv_obj_get_name(a);
        const bool tabs = box_name != NULL && strcmp(box_name, "device_areas") == 0;
        if (across && scrolls && (dir == LV_DIR_HOR || tabs) && row != label &&
            outside(&row_box, &box, true)) {
            return NULL;
        }
        return a;
    }
    return NULL;
}

/* A name to find a box by in the report: its own, or the nearest named one around it. */
static const char *name_of(lv_obj_t *o)
{
    for (; o != NULL; o = lv_obj_get_parent(o)) {
        const char *name = lv_obj_get_name(o);
        if (name != NULL && name[0] != '\0') {
            return name;
        }
    }
    return "(screen)";
}

/* Each cut-off text is reported once, at the first screenshot that shows it: the rail's labels
 * are on almost every one. */
static bool first_report(const char *text, const char *where)
{
    static uint32_t seen[1024];
    static unsigned count;
    uint32_t h = 2166136261u;
    for (const char *s = text; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    for (const char *s = where; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    for (unsigned i = 0; i < count; i++) {
        if (seen[i] == h) {
            return false;
        }
    }
    if (count < sizeof(seen) / sizeof(seen[0])) {
        seen[count++] = h;
    }
    return true;
}

/* OPT-005: each icon size carries only the icons drawn at that size (BL-122), so a code point in
 * the private use area (Lucide's) must have a real glyph in the font of the label or key that
 * shows it - LVGL would draw a box. Hidden objects count too: they show in other states. */
static const char *font_label_of(const lv_font_t *f)
{
    static char buf[32];
    if (f == icons_20) {
        return "icons_20";
    }
    if (f == icons_28) {
        return "icons_28";
    }
    if (f == icons_40) {
        return "icons_40";
    }
    if (f == icons_96) {
        return "icons_96";
    }
    snprintf(buf, sizeof(buf), "font of height %d", (int)f->line_height);
    return buf;
}

static void check_icon_text(const char *what, const char *text, const lv_font_t *font,
                            const char *shot_name)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (p[0] != 0xEEu && p[0] != 0xEFu) { /* U+E000..U+FFFF; the range ends at U+F8FF */
            continue;
        }
        if (p[1] == 0u || p[2] == 0u) {
            break;
        }
        const uint32_t cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
                            (uint32_t)(p[2] & 0x3Fu);
        if (cp < 0xE000u || cp > 0xF8FFu) {
            continue;
        }
        p += 2;
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(font, &dsc, cp, 0) && !dsc.is_placeholder) {
            continue;
        }
        char key[48];
        snprintf(key, sizeof(key), "%s/U+%04X", font_label_of(font), (unsigned)cp);
        if (first_report(key, "glyph")) {
            s_glyph_misses++;
            printf("  glyph %s: U+%04X has no glyph in %s (%s \"%.40s\")\n", shot_name,
                   (unsigned)cp, font_label_of(font), what, text);
        }
    }
}

static void report_glyphs(lv_obj_t *o, const char *shot_name)
{
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *text = lv_label_get_text(o);
        if (text != NULL) {
            check_icon_text(name_of(o), text, lv_obj_get_style_text_font(o, LV_PART_MAIN),
                            shot_name);
        }
    } else if (lv_obj_check_type(o, &lv_buttonmatrix_class)) {
        const char *const *map = lv_buttonmatrix_get_map(o);
        const lv_font_t *font = lv_obj_get_style_text_font(o, LV_PART_ITEMS);
        for (uint32_t i = 0; map != NULL && map[i][0] != '\0'; i++) {
            check_icon_text("key", map[i], font, shot_name);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        report_glyphs(lv_obj_get_child(o, (int32_t)i), shot_name);
    }
}

/* I18N-022: every visible label's text fits it - a word cut off is a word lost - and stays
 * inside the boxes around it. A label meant to shorten (LV_LABEL_LONG_MODE_DOTS) is counted
 * apart: it ends with an ellipsis on purpose. */
static void report_clipping(lv_obj_t *o, const char *shot_name)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *text = lv_label_get_text(o);
        const lv_label_long_mode_t mode = lv_label_get_long_mode(o);
        const int32_t w = lv_obj_get_content_width(o);
        const int32_t h = lv_obj_get_content_height(o);
        if (text != NULL && text[0] != '\0' && w > 0 && h > 0) {
            const lv_font_t *font = lv_obj_get_style_text_font(o, LV_PART_MAIN);
            /* Wrapping, clipping and dotted labels break lines at their width; only the
             * scrolling ones keep one line. */
            const bool one_line = mode == LV_LABEL_LONG_MODE_SCROLL ||
                                  mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
            /* As LVGL draws it: a label sized to its text, and not stretched by a layout,
             * never breaks its lines (get_label_flags in lv_label.c). */
            const bool fit = lv_obj_get_style_width(o, LV_PART_MAIN) == LV_SIZE_CONTENT &&
                             lv_obj_get_style_max_width(o, LV_PART_MAIN) == LV_COORD_MAX &&
                             lv_obj_get_style_flex_grow(o, LV_PART_MAIN) == 0;
            const int32_t letter = lv_obj_get_style_text_letter_space(o, LV_PART_MAIN);
            lv_point_t size;
            lv_text_get_size(&size, text, font, letter,
                             lv_obj_get_style_text_line_space(o, LV_PART_MAIN),
                             one_line ? LV_COORD_MAX : w,
                             fit ? LV_TEXT_FLAG_FIT : LV_TEXT_FLAG_NONE);
            /* A broken line is measured with the space it broke at, which may hang past the
             * edge unseen. */
            const int32_t slack =
                one_line || fit ? 1 : (int32_t)lv_font_get_glyph_width(font, ' ', 0) + letter + 1;
            const size_t len = strlen(text);
            if (mode == LV_LABEL_LONG_MODE_DOTS && len > 3 && strcmp(text + len - 3, "...") == 0) {
                /* LVGL wrote its dots into the text: shortened on purpose, and it fits */
                if (first_report(text, "...")) {
                    s_shortened++;
                    printf("  short %s: \"%.60s\"\n", shot_name, text);
                }
            } else if (size.x > w + slack || size.y > h + 1) {
                if (mode == LV_LABEL_LONG_MODE_DOTS) {
                    if (first_report(text, "...")) {
                        s_shortened++;
                        printf("  short %s: \"%.60s\" ends with ...\n", shot_name, text);
                    }
                } else if (first_report(text, "")) {
                    s_clipped++;
                    printf("  clip %s: \"%.60s\" needs %dx%d, has %dx%d\n", shot_name, text,
                           (int)size.x, (int)size.y, (int)w, (int)h);
                }
            } else {
                /* Where the text lands in the label: its alignment decides across, and a
                 * broken line's hanging space (the slack above) stays inside. */
                lv_area_t area;
                lv_obj_get_content_coords(o, &area);
                if (size.x > w) {
                    size.x = w;
                }
                const int32_t spare = w - size.x;
                const lv_text_align_t align = lv_obj_get_style_text_align(o, LV_PART_MAIN);
                if (align == LV_TEXT_ALIGN_CENTER) {
                    area.x1 += spare / 2;
                } else if (align == LV_TEXT_ALIGN_RIGHT) {
                    area.x1 += spare;
                }
                area.x2 = area.x1 + size.x - 1;
                area.y2 = area.y1 + size.y - 1;
                lv_obj_t *box = spilled_from(o, &area);
                if (box != NULL && first_report(text, name_of(box))) {
                    s_clipped++;
                    printf("  spill %s: \"%.60s\" runs out of %s\n", shot_name, text,
                           name_of(box));
                }
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        report_clipping(lv_obj_get_child(o, (int32_t)i), shot_name);
    }
}

/* I18N-022: the status bar's right side (the trouble pill, the icons, the clock, Chime and the
 * gear) stays inside the bar in every language: the date or a banner gives way (ui_app.c
 * bar_fit). Failing it fails the preview run, in a test language too. */
static void expect_bar_fits(const char *shot_name)
{
    lv_obj_t *bar = lv_obj_find_by_name(lv_screen_active(), "bar");
    if (bar == NULL || lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_area_t room;
    lv_obj_get_content_coords(bar, &room);
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *c = lv_obj_get_child(bar, (int32_t)i);
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && (a.x1 < room.x1 || a.x2 > room.x2)) {
            fprintf(stderr, "%s: the status bar's %s (x %d..%d) runs out of it (x %d..%d)\n",
                    shot_name, name_of(c), (int)a.x1, (int)a.x2, (int)room.x1, (int)room.x2);
            exit(1);
        }
    }
}

#if CONFIG_ALARM_HOME_CONTROL
/* Taps a Smart Home tile by its device's name (a value: the same in every language). */
static void tap_device(const char *name)
{
    lv_obj_t *l = find_label(by_name("device_grid"), name);
    expect(l != NULL, "Smart Home: a tile is missing");
    if (l != NULL) {
        tap(l);
    }
}

/* A long press on a Smart Home tile (what opens a device's sheet besides its arrow). */
static void hold_device(const char *name)
{
    lv_obj_t *l = find_label(by_name("device_grid"), name);
    expect(l != NULL, "Smart Home: a tile is missing");
    if (l == NULL) {
        return;
    }
    if (!centre_visible(l)) {
        scroll_into_view(l);
        run(50);
    }
    lv_area_t a;
    lv_obj_get_coords(l, &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(700);
    s_pressed = false;
    run(300);
}

/* The open device sheet (HCTL-004), or NULL. */
static lv_obj_t *device_sheet_open(void)
{
    return lv_subject_get_int(&subject_sheet) == 6 ? by_name("device_sheet") : NULL;
}

/* A tap a fraction of the way along the sheet's slider: the level set by a finger that lifts. */
static void tap_sheet_slider(int percent)
{
    lv_obj_t *sheet = device_sheet_open();
    lv_obj_t *slider = sheet != NULL ? lv_obj_find_by_name(sheet, "slider") : NULL;
    expect(slider != NULL, "HCTL-004: the sheet has no slider");
    if (slider == NULL) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(slider, &a);
    tap_xy(a.x1 + (a.x2 - a.x1) * percent / 100, (a.y1 + a.y2) / 2);
    run(1300);
}

static void tap_sheet_label(const char *text)
{
    lv_obj_t *l = device_sheet_open() != NULL ? find_label(device_sheet_open(), text) : NULL;
    expect(l != NULL, "HCTL-004: a control is missing from the sheet");
    if (l != NULL) {
        tap(l);
        run(1300);
    }
}

/* Whether the open sheet's key with this word is lit (the device's mode or state: a tinted key). */
static bool sheet_key_lit(const char *text)
{
    lv_obj_t *o = device_sheet_open() != NULL ? find_label(device_sheet_open(), text) : NULL;
    for (int up = 0; o != NULL && up < 3; up++, o = lv_obj_get_parent(o)) {
        if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) == LV_OPA_20) {
            return true;
        }
    }
    return false;
}

static void close_device_sheet(void)
{
    tap_name("device_close");
    run(300);
    expect(device_sheet_open() == NULL, "HCTL-004: the X didn't close the sheet");
}

static void tap_area(const char *name)
{
    lv_obj_t *l = find_label(by_name("device_areas"), name);
    expect(l != NULL, "Smart Home: an area's tab is missing");
    if (l != NULL) {
        tap(l);
    }
}

/* The grid's tile holding a device's name, or NULL. */
static lv_obj_t *device_tile_of(const char *name)
{
    lv_obj_t *grid = by_name("device_grid");
    lv_obj_t *o = find_label(grid, name);
    while (o != NULL && lv_obj_get_parent(o) != grid) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

/* BL-220 R7 (the owner, 2026-10-19): no "Smart Home" title and no note over the page - the
 * header is hidden, its note empty, and the tabs start the row with the pencil at its end. */
static bool no_title(void)
{
    lv_obj_t *head = lv_obj_find_by_name(lv_screen_active(), "device_header"); /* hidden: no by_name */
    lv_obj_t *tabs = by_name("device_areas");
    lv_obj_t *pencil = by_name("device_arrange");
    if (head == NULL || tabs == NULL || pencil == NULL) {
        return false;
    }
    lv_obj_update_layout(lv_screen_active());
    lv_area_t t;
    lv_area_t pen;
    lv_area_t row;
    lv_obj_get_coords(tabs, &t);
    lv_obj_get_coords(pencil, &pen);
    lv_obj_get_coords(lv_obj_get_parent(tabs), &row);
    return lv_obj_has_flag(head, LV_OBJ_FLAG_HIDDEN) &&
           lv_subject_get_string(&subject_devices_note)[0] == '\0' && t.x1 == row.x1 &&
           pen.x2 == row.x2 && t.x2 < pen.x1;
}

/* A tab wholly inside the tabs' box (scrolled into view, not cut at an edge). */
static bool tab_in_view(lv_obj_t *tabs, lv_obj_t *seg)
{
    if (seg == NULL) {
        return false;
    }
    lv_area_t a;
    lv_area_t box;
    lv_obj_get_coords(seg, &a);
    lv_obj_get_coords(tabs, &box);
    return a.x1 >= box.x1 && a.x2 <= box.x2;
}

/* BL-220 R7: with no title beside them the tabs take the row up to the pencil - a house of many
 * rooms scrolls them, the open one in view and the pencil where it was - and a room's long name
 * stays inside them; the tiles don't move. Every device as before afterwards. */
static void tabs_take_the_row(void)
{
    static const char ROOM[] = "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";
    lv_obj_t *tabs = by_name("device_areas");
    lv_obj_t *grid = by_name("device_grid");
    lv_obj_update_layout(lv_screen_active());
    const int32_t grid_y = lv_obj_get_y(grid);
    host_panel_devices_rooms(true);
    run(1200);
    lv_obj_update_layout(tabs);
    expect(no_title() && lv_obj_get_y(grid) == grid_y,
           "BL-220 R7: with many rooms the tabs don't start the row, the pencil moved, or the tiles did");
    expect(lv_obj_get_width(tabs) == lv_obj_get_style_max_width(tabs, LV_PART_MAIN) &&
               lv_obj_get_width(tabs) > 560,
           "BL-220 R7: many rooms didn't take the tabs to the row's width (wider than beside a title)");
    printf("  many rooms: tabs %d px\n", (int)lv_obj_get_width(tabs));
    host_panel_devices_area_rename("Living room", ROOM);
    run(1200);
    tap_area(ROOM);
    run(1200);
    lv_obj_update_layout(tabs);
    lv_obj_t *on = NULL;
    for (uint32_t c = 0; c < lv_obj_get_child_count(tabs) && on == NULL; c++) {
        lv_obj_t *seg = lv_obj_get_child(tabs, (int32_t)c);
        on = find_label_with(seg, "WWW") != NULL ? seg : NULL;
    }
    expect(tab_in_view(tabs, on) && no_title(),
           "BL-220 R7: a long room's open tab isn't wholly in view, or pushed the pencil");
    host_panel_devices_area_rename(ROOM, "Living room");
    host_panel_devices_rooms(false);
    run(1200);
    /* the rooms made from the areas that went stay until Smart Home is opened again (hc_home_adopt
     * drops them only then, so a source listing a few at a time can't shuffle the tabs) */
    expect(find_label_with(by_name("device_areas"), "WWW") != NULL,
           "BL-220 R3: an area's empty room went while Smart Home was looked at");
    tap_name("nav_home");
    run(300);
    tap_name("nav_devices");
    run(1200);
    expect(find_label_with(by_name("device_areas"), "WWW") == NULL,
           "BL-220 R3: an empty room made from an area didn't go when Smart Home opened again");
    tap_area("Living room");
    run(1200);
}

/* The tile's words of a device, in amber (a failure's colour, HCTL-006), or not. */
static bool tile_says(const char *name, const char *words, bool amber)
{
    lv_obj_t *tile = device_tile_of(name);
    lv_obj_t *l = tile != NULL ? find_label(tile, words) : NULL;
    return l != NULL && lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), COLOR_AMBER) == amber;
}

/* A thermostat's words on its tile and its open sheet alike (HCTL-004, BL-171). */
static bool climate_words(const char *name, const char *words)
{
    lv_obj_t *tile = device_tile_of(name);
    return tile != NULL && find_label(tile, words) != NULL && device_sheet_open() != NULL &&
           find_label(device_sheet_open(), words) != NULL;
}

/* A thermostat's words exactly, the same on its tile and in its open sheet ("" for none): the
 * tile's detail_label and the sheet's sheet_state (HCTL-004, BL-171). */
static bool climate_says(const char *name, const char *words)
{
    lv_obj_t *tile = device_tile_of(name);
    lv_obj_t *sheet = device_sheet_open();
    lv_obj_t *a = tile != NULL ? lv_obj_find_by_name(tile, "detail_label") : NULL;
    lv_obj_t *b = sheet != NULL ? lv_obj_find_by_name(sheet, "sheet_state") : NULL;
    return a != NULL && b != NULL && strcmp(lv_label_get_text(a), words) == 0 &&
           strcmp(lv_label_get_text(b), words) == 0;
}

/* A tap on one of a tile's own keys (a cover's Open/Stop/Close, a fan's speeds), found by its word. */
static void tap_tile_key(const char *device, const char *key)
{
    lv_obj_t *tile = device_tile_of(device);
    lv_obj_t *l = tile != NULL ? find_label(tile, key) : NULL;
    expect(l != NULL, "HCTL-004: a key is missing from the tile");
    if (l != NULL) {
        tap(l);
        run(300);
    }
}

/* ---- BL-181 (HCTL-006): an offline device stays readable ----------------------------------- */

/* What an object shows at, its parents' opacity drawn through (an object at 40 % inside one at
 * 100 % reads 101 of 255). */
static bool seen_at(lv_obj_t *o, lv_opa_t want)
{
    const int got = o != NULL ? (int)lv_obj_get_style_opa_recursive(o, LV_PART_MAIN) : -10;
    return got >= (int)want - 3 && got <= (int)want + 1;
}

/* A control at rest: disabled (no press, no pressed colour, no click) and seen at 40 %. */
static bool rests(lv_obj_t *o)
{
    return o != NULL && lv_obj_has_state(o, LV_STATE_DISABLED) && seen_at(o, LV_OPA_40);
}

static bool usable(lv_obj_t *o)
{
    return o != NULL && !lv_obj_has_state(o, LV_STATE_DISABLED) && seen_at(o, LV_OPA_COVER);
}

/* The key holding a word on a tile or the sheet (the label's parent), or NULL. */
static lv_obj_t *key_of(lv_obj_t *root, const char *word)
{
    lv_obj_t *l = find_label(root, word);
    return l != NULL ? lv_obj_get_parent(l) : NULL;
}

/* A finger put down on `at` for a moment, then lifted: whether `o` showed a pressed colour (its
 * background other than bg) meanwhile, or - a control - took the pressed state. The tile itself
 * is pressed (its long press opens the sheet), so only its colour counts. */
static bool pressed_shows(lv_obj_t *o, lv_obj_t *at, lv_color_t bg)
{
    if (o == NULL || at == NULL) {
        return true;
    }
    lv_area_t a;
    lv_obj_get_coords(at, &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(100);
    const bool tile = lv_obj_get_parent(o) == by_name("device_grid");
    const bool shows = (!tile && lv_obj_has_state(o, LV_STATE_PRESSED)) ||
                       !lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), bg);
    s_pressed = false;
    run(150);
    return shows;
}

/* An open light's sheet whose device answers: its switch on and usable, its slider usable at the
 * level `lit` in its own blue, its chip at full opacity (BL-181). */
static bool light_sheet_back(lv_obj_t *sheet, int32_t lit)
{
    lv_obj_t *sw = lv_obj_find_by_name(sheet, "sheet_switch");
    lv_obj_t *slider = lv_obj_find_by_name(sheet, "slider");
    return usable(sw) && lv_obj_has_state(sw, LV_STATE_CHECKED) && usable(slider) &&
           lv_slider_get_value(slider) == lit &&
           !lv_color_eq(lv_obj_get_style_bg_color(slider, LV_PART_INDICATOR), COLOR_TEXT_2) &&
           seen_at(lv_obj_find_by_name(sheet, "sheet_chip"), LV_OPA_COVER);
}

/* An open thermostat's sheet whose device answers: its reported mode's key lit and usable, its
 * set point's + usable (BL-181). */
static bool climate_sheet_back(lv_obj_t *sheet, const char *lit_word)
{
    return sheet_key_lit(lit_word) && usable(key_of(sheet, lit_word)) && usable(lv_obj_find_by_name(sheet, "plus"));
}

/* The tile and its sheet at full opacity, the name and the words with them; the chip at 70 %
 * (opa_device_offline_chip); a slider, a stepper, keys and the sheet's switch resting at 40 % in
 * their neutral grey, none lit. A tap sends nothing and shows no pressed colour; a long press still
 * opens the sheet, Close works, and the controls come back in one step (BL-181 decision; the
 * mockup's devices-offline-controls). A device lost while its own sheet is open (a light, a
 * thermostat) rests the same way under the open sheet and gets its controls back in one step.
 * On Outside; leaves the devices as it found them. */
static void offline_stays_readable(void)
{
    const int64_t down = host_panel_device("switch.pool_pump")->since_ms; /* 9:12 PM */
    const int64_t DAY = 24LL * 3600LL * 1000LL;
    static const char *const REFS[] = {"light.porch", "cover.patio_awning", "fan.porch", "climate.hallway"};
    int64_t was[4];
    for (size_t i = 0; i < 4u; i++) {
        was[i] = host_panel_device(REFS[i])->since_ms;
    }
    host_panel_device_offline("light.porch", down);
    host_panel_device_offline("cover.patio_awning", down - 366LL * DAY);
    host_panel_device_offline("fan.porch", down - DAY);
    run(1200);
    unsigned acts = host_panel_device_acts();
    const int32_t lit = host_panel_device("light.porch")->level;
    const int32_t speed = host_panel_device("fan.porch")->level;

    /* The tiles: readable, the chip at 70 %, the controls resting in grey */
    lv_obj_t *light = device_tile_of("Porch Light");
    lv_obj_t *awning = device_tile_of("Patio Awning");
    lv_obj_t *fan = device_tile_of("Porch Fan");
    lv_obj_t *words = find_label(light, "Offline since 9:12 PM");
    expect(seen_at(light, LV_OPA_COVER) && seen_at(find_label(light, "Porch Light"), LV_OPA_COVER) &&
               seen_at(words, LV_OPA_COVER) &&
               lv_color_eq(lv_obj_get_style_text_color(words, LV_PART_MAIN), COLOR_TEXT_2),
           "HCTL-006 (BL-181): an offline tile, its name or \"Offline since 9:12 PM\" isn't at full opacity in color_text_2");
    expect(seen_at(find_label(awning, "Offline since Sep 18, 2025"), LV_OPA_COVER) &&
               seen_at(find_label(fan, "Offline since Sep 18"), LV_OPA_COVER) &&
               seen_at(find_label(device_tile_of("Pool Pump"), "Offline since 9:12 PM"), LV_OPA_COVER),
           "HCTL-006 (BL-181): an offline tile's date or year isn't readable at full opacity");
    expect(seen_at(lv_obj_find_by_name(light, "chip"), OPA_DEVICE_OFFLINE_CHIP) &&
               seen_at(lv_obj_find_by_name(awning, "chip"), OPA_DEVICE_OFFLINE_CHIP),
           "HCTL-006 (BL-181): an offline tile's chip isn't at 70 % (opa_device_offline_chip)");
    lv_obj_t *level = lv_obj_find_by_name(light, "level");
    expect(rests(level) &&
               lv_color_eq(lv_obj_get_style_bg_color(level, LV_PART_INDICATOR), COLOR_TEXT_2),
           "HCTL-006 (BL-181): an offline light's slider isn't resting at 40 % in grey");
    bool keys_rest = true;
    static const char *const AWNING_KEYS[] = {"Open", "Stop", "Close"};
    static const char *const FAN_KEYS[] = {"Off", "Low", "Medium", "High"};
    for (size_t i = 0; i < 3u; i++) {
        keys_rest = keys_rest && rests(key_of(awning, AWNING_KEYS[i]));
    }
    for (size_t i = 0; i < 4u; i++) {
        lv_obj_t *k = key_of(fan, FAN_KEYS[i]);
        keys_rest = keys_rest && rests(k) && lv_obj_get_style_bg_opa(k, LV_PART_MAIN) != LV_OPA_20;
    }
    expect(keys_rest, "HCTL-006 (BL-181): an offline cover's or fan's keys aren't resting at 40 %, or a speed is lit");
    expect(find_visible(light, "more") == NULL && find_visible(awning, "more") == NULL,
           "HCTL-006: an offline tile shows its arrow");
    shot("devices_offline_controls");
    /* A tap on the tile, its slider or a key sends nothing; pressed, nothing shows it */
    tap_device("Porch Light");
    run(300);
    expect(host_panel_device_acts() == acts && host_panel_device("light.porch")->state == UI_DEV_ST_ON,
           "HCTL-006 (BL-181): a tap on an offline tile sent something");
    tap(level);
    run(300);
    expect(host_panel_device_acts() == acts && host_panel_device("light.porch")->level == lit,
           "HCTL-006 (BL-181): a tap on an offline tile's slider sent something");
    tap_tile_key("Patio Awning", "Close");
    expect(host_panel_device_acts() == acts && host_panel_device("cover.patio_awning")->state == UI_DEV_ST_OPEN,
           "HCTL-006 (BL-181): a tap on an offline cover's key sent something");
    tap_tile_key("Porch Fan", "High");
    expect(host_panel_device_acts() == acts && host_panel_device("fan.porch")->level == speed,
           "HCTL-006 (BL-181): a tap on an offline fan's key sent something");
    printf("  offline taps: %u act(s) asked, %u before; light %d %d, awning %d, fan %d\n",
           host_panel_device_acts(), acts, (int)host_panel_device("light.porch")->state,
           (int)host_panel_device("light.porch")->level, (int)host_panel_device("cover.patio_awning")->state,
           (int)host_panel_device("fan.porch")->level);
    expect(!pressed_shows(light, find_label(light, "Porch Light"), COLOR_SURFACE) &&
               !pressed_shows(level, level, lv_obj_get_style_bg_color(level, LV_PART_MAIN)) &&
               !pressed_shows(key_of(awning, "Open"), key_of(awning, "Open"),
                              lv_obj_get_style_bg_color(key_of(awning, "Open"), LV_PART_MAIN)),
           "HCTL-006 (BL-181): an offline tile or its control showed a pressed colour");
    expect(host_panel_device_acts() == acts, "HCTL-006 (BL-181): pressing an offline tile sent something");

    /* Its sheet, opened by a long press: readable, its switch and slider resting; Close works */
    hold_device("Porch Light");
    lv_obj_t *sheet = device_sheet_open();
    expect(sheet != NULL, "HCTL-006 (BL-181): a long press didn't open an offline light's sheet");
    if (sheet != NULL) {
        lv_obj_t *sw = lv_obj_find_by_name(sheet, "sheet_switch");
        lv_obj_t *slider = lv_obj_find_by_name(sheet, "slider");
        lv_obj_t *state = lv_obj_find_by_name(sheet, "sheet_state");
        expect(seen_at(lv_obj_find_by_name(sheet, "device_body"), LV_OPA_COVER) &&
                   seen_at(find_label(sheet, "Brightness"), LV_OPA_COVER) &&
                   seen_at(find_label(sheet, "Porch Light"), LV_OPA_COVER) && seen_at(state, LV_OPA_COVER) &&
                   state != NULL && strcmp(lv_label_get_text(state), "Offline since 9:12 PM") == 0,
               "HCTL-006 (BL-181): an offline sheet's body, title, words or \"Brightness\" aren't at full opacity");
        expect(seen_at(lv_obj_find_by_name(sheet, "sheet_chip"), OPA_DEVICE_OFFLINE_CHIP),
               "HCTL-006 (BL-181): an offline sheet's chip isn't at 70 %");
        expect(rests(sw) && !lv_obj_has_state(sw, LV_STATE_CHECKED) && rests(slider) &&
                   lv_color_eq(lv_obj_get_style_bg_color(slider, LV_PART_INDICATOR), COLOR_TEXT_2),
               "HCTL-006 (BL-181): an offline sheet's switch (shown on) or slider isn't resting at 40 % in grey");
        expect(usable(lv_obj_find_by_name(sheet, "device_close")),
               "HCTL-006 (BL-181): an offline sheet's Close isn't enabled at full opacity");
        shot("device_sheet_offline");
        tap(sw);
        run(300);
        tap_sheet_slider(30);
        expect(host_panel_device_acts() == acts && host_panel_device("light.porch")->level == lit &&
                   host_panel_device("light.porch")->state == UI_DEV_ST_ON,
               "HCTL-006 (BL-181): an offline sheet's switch or slider sent something");
        expect(!pressed_shows(sw, sw, lv_obj_get_style_bg_color(sw, LV_PART_MAIN)),
               "HCTL-006 (BL-181): an offline sheet's switch showed it was pressed");
        close_device_sheet(); /* Close works offline */
    }
    /* Open while it comes back: the controls return in one step, the sheet stays open */
    hold_device("Porch Light");
    host_panel_device_settle("light.porch");
    run(1200);
    sheet = device_sheet_open();
    expect(sheet != NULL, "HCTL-006 (BL-181): the sheet closed when its device came back");
    if (sheet != NULL) {
        expect(light_sheet_back(sheet, lit),
               "HCTL-006 (BL-181): a sheet whose device came back didn't get its switch (on), slider or chip back");
        shot("device_sheet_recovered");
        /* Loss while open (HCTL-006, BL-181): the device goes offline under its own open sheet. The
         * same sheet stays open and readable, its switch (shown off) and slider rest at 40 % in grey,
         * a tap sends nothing, Close works; when it answers again its controls come back in one step. */
        acts = host_panel_device_acts();
        host_panel_device_offline("light.porch", down);
        run(1200);
        lv_obj_t *sw = lv_obj_find_by_name(sheet, "sheet_switch");
        lv_obj_t *slider = lv_obj_find_by_name(sheet, "slider");
        lv_obj_t *state = lv_obj_find_by_name(sheet, "sheet_state");
        expect(device_sheet_open() == sheet && strcmp(lv_subject_get_string(&subject_sheet_title), "Porch Light") == 0,
               "HCTL-006 (BL-181): a sheet closed or changed when its device went offline under it");
        expect(rests(sw) && !lv_obj_has_state(sw, LV_STATE_CHECKED) && rests(slider) &&
                   lv_color_eq(lv_obj_get_style_bg_color(slider, LV_PART_INDICATOR), COLOR_TEXT_2) &&
                   seen_at(lv_obj_find_by_name(sheet, "sheet_chip"), OPA_DEVICE_OFFLINE_CHIP),
               "HCTL-006 (BL-181): offline under its open sheet, a light's switch (shown off) or slider isn't resting "
               "at 40 % in grey, or its chip isn't at 70 %");
        expect(seen_at(find_label(sheet, "Porch Light"), LV_OPA_COVER) && seen_at(find_label(sheet, "Brightness"), LV_OPA_COVER) &&
                   state != NULL && strcmp(lv_label_get_text(state), "Offline since 9:12 PM") == 0 &&
                   seen_at(state, LV_OPA_COVER) && usable(lv_obj_find_by_name(sheet, "device_close")),
               "HCTL-006 (BL-181): offline under its open sheet, its words aren't at full opacity or Close isn't usable");
        shot("device_sheet_lost_open");
        tap(sw);
        run(300);
        tap_sheet_slider(30);
        expect(host_panel_device_acts() == acts && host_panel_device("light.porch")->level == lit &&
                   host_panel_device("light.porch")->state == UI_DEV_ST_ON && device_sheet_open() == sheet,
               "HCTL-006 (BL-181): offline under its open sheet, a tap on its switch or slider sent something");
        host_panel_device_settle("light.porch");
        run(1200);
        expect(device_sheet_open() == sheet && light_sheet_back(sheet, lit),
               "HCTL-006 (BL-181): a light lost while its sheet was open didn't get its switch (on), slider or chip "
               "back in one step");
        close_device_sheet();
    }
    light = device_tile_of("Porch Light");
    expect(usable(lv_obj_find_by_name(light, "level")) &&
               seen_at(lv_obj_find_by_name(light, "chip"), LV_OPA_COVER) &&
               lv_color_eq(lv_obj_get_style_bg_color(lv_obj_find_by_name(light, "level"), LV_PART_INDICATOR),
                           COLOR_BLUE),
           "HCTL-006 (BL-181): a tile whose device came back didn't get its slider (blue) or chip back");
    host_panel_device_settle("cover.patio_awning");
    host_panel_device_settle("fan.porch");
    run(1200);
    expect(usable(key_of(device_tile_of("Patio Awning"), "Open")) && usable(key_of(device_tile_of("Porch Fan"), "Low")),
           "HCTL-006 (BL-181): the keys didn't come back with their devices");

    /* A thermostat: its stepper rests on the tile; its sheet shows no mode lit while offline, and
     * the reported one again when it answers (the mode is kept) */
    tap_area("Living room");
    run(1200);
    host_panel_device_offline("climate.hallway", down - 366LL * DAY);
    run(1200);
    lv_obj_t *hall = device_tile_of("Hallway");
    const int32_t set = host_panel_device("climate.hallway")->setpoint;
    const uint8_t mode = host_panel_device("climate.hallway")->state; /* as reported: kept offline */
    printf("  offline thermostat: mode %d, set point %d\n", (int)mode, (int)set);
    expect(rests(lv_obj_find_by_name(hall, "minus")) && rests(lv_obj_find_by_name(hall, "plus")) &&
               seen_at(find_label(hall, "Offline since Sep 18, 2025"), LV_OPA_COVER),
           "HCTL-006 (BL-181): an offline thermostat's stepper isn't resting, or its words aren't readable");
    acts = host_panel_device_acts();
    tap(lv_obj_find_by_name(hall, "plus"));
    run(300);
    expect(host_panel_device_acts() == acts && host_panel_device("climate.hallway")->setpoint == set,
           "HCTL-006 (BL-181): a tap on an offline thermostat's + sent something");
    hold_device("Hallway");
    sheet = device_sheet_open();
    expect(sheet != NULL, "HCTL-006 (BL-181): a long press didn't open an offline thermostat's sheet");
    if (sheet != NULL) {
        static const char *const MODES[] = {"Off", "Heat", "Cool", "Auto"};
        bool modes_rest = true;
        for (size_t i = 0; i < 4u; i++) {
            modes_rest = modes_rest && rests(key_of(sheet, MODES[i])) && !sheet_key_lit(MODES[i]);
        }
        lv_obj_t *state = lv_obj_find_by_name(sheet, "sheet_state");
        expect(modes_rest && seen_at(find_label(sheet, "Mode"), LV_OPA_COVER),
               "HCTL-006 (BL-181): an offline thermostat's mode keys aren't all resting and unlit, or \"Mode\" isn't clear");
        expect(state != NULL && strcmp(lv_label_get_text(state), "Offline since Sep 18, 2025") == 0 &&
                   seen_at(state, LV_OPA_COVER),
               "HCTL-006 (BL-181): the sheet doesn't show \"Offline since Sep 18, 2025\" whole at full opacity");
        expect(rests(lv_obj_find_by_name(sheet, "minus")) && rests(lv_obj_find_by_name(sheet, "plus")),
               "HCTL-006 (BL-181): an offline thermostat sheet's set point isn't resting");
        shot("device_sheet_offline_climate");
        tap_sheet_label(mode == UI_DEV_ST_AUTO ? "Heat" : "Auto");
        expect(host_panel_device_acts() == acts && host_panel_device("climate.hallway")->state == mode,
               "HCTL-006 (BL-181): an offline thermostat's mode key sent something");
        host_panel_device_settle("climate.hallway");
        run(1200);
        const char *lit_word = mode == UI_DEV_ST_HEAT   ? "Heat"
                               : mode == UI_DEV_ST_COOL ? "Cool"
                               : mode == UI_DEV_ST_AUTO ? "Auto"
                                                        : "Off";
        expect(climate_sheet_back(sheet, lit_word),
               "HCTL-006 (BL-181): back online, the thermostat's reported mode isn't lit again, or its controls stay resting");
        /* Loss while open (HCTL-006, BL-181): offline again under its own open sheet, the same sheet
         * stays; its mode keys and set point rest at 40 %, none lit; its words stay whole at full
         * opacity; a tap sends nothing; Close works; it answers again and all come back in one step */
        acts = host_panel_device_acts();
        host_panel_device_offline("climate.hallway", down - 366LL * DAY);
        run(1200);
        expect(device_sheet_open() == sheet && strcmp(lv_subject_get_string(&subject_sheet_title), "Hallway") == 0,
               "HCTL-006 (BL-181): a thermostat's sheet closed or changed when its device went offline under it");
        bool lost_rest = rests(lv_obj_find_by_name(sheet, "minus")) && rests(lv_obj_find_by_name(sheet, "plus"));
        for (size_t i = 0; i < 4u; i++) {
            lost_rest = lost_rest && rests(key_of(sheet, MODES[i])) && !sheet_key_lit(MODES[i]);
        }
        expect(lost_rest, "HCTL-006 (BL-181): offline under its open sheet, a thermostat's mode keys or set point "
                          "aren't all resting at 40 % and unlit");
        expect(strcmp(lv_label_get_text(state), "Offline since Sep 18, 2025") == 0 && seen_at(state, LV_OPA_COVER) &&
                   seen_at(find_label(sheet, "Hallway"), LV_OPA_COVER) && seen_at(find_label(sheet, "Mode"), LV_OPA_COVER) &&
                   seen_at(lv_obj_find_by_name(sheet, "sheet_chip"), OPA_DEVICE_OFFLINE_CHIP) &&
                   usable(lv_obj_find_by_name(sheet, "device_close")),
               "HCTL-006 (BL-181): offline under its open sheet, a thermostat's words aren't whole at full opacity, its "
               "chip at 70 % or Close usable");
        shot("device_sheet_lost_open_climate");
        tap_sheet_label(mode == UI_DEV_ST_AUTO ? "Heat" : "Auto");
        tap(lv_obj_find_by_name(sheet, "plus"));
        run(300);
        expect(host_panel_device_acts() == acts && host_panel_device("climate.hallway")->state == mode &&
                   host_panel_device("climate.hallway")->setpoint == set && device_sheet_open() == sheet,
               "HCTL-006 (BL-181): offline under its open sheet, a thermostat's mode key or + sent something");
        host_panel_device_settle("climate.hallway");
        run(1200);
        expect(device_sheet_open() == sheet && climate_sheet_back(sheet, lit_word),
               "HCTL-006 (BL-181): a thermostat lost while its sheet was open didn't get its mode and set point back "
               "in one step");
        close_device_sheet();
    }
    host_panel_device_settle("climate.hallway");
    for (size_t i = 0; i < 4u; i++) {
        host_panel_device_since(REFS[i], was[i]);
    }
    tap_area("Outside");
    run(1200);
}

/* Whether a's tile comes before b's: an earlier row, or the same row further left. */
static bool device_before(const char *a, const char *b)
{
    lv_obj_t *ta = device_tile_of(a);
    lv_obj_t *tb = device_tile_of(b);
    if (ta == NULL || tb == NULL) {
        return false;
    }
    lv_area_t x;
    lv_area_t y;
    lv_obj_get_coords(ta, &x);
    lv_obj_get_coords(tb, &y);
    return x.y1 < y.y1 || (x.y1 == y.y1 && x.x1 < y.x1);
}

static bool device_is_wide(const char *name)
{
    lv_obj_t *t = device_tile_of(name);
    return t != NULL && lv_obj_get_width(t) > 300;
}

/* Whether a control of the screen is on show (not hidden, nor under a hidden parent). */
static bool shown(const char *name)
{
    return find_visible(lv_screen_active(), name) != NULL;
}

/* A finger down on a tile's centre, held past the long press: the tile is lifted (HCTL-008). The
 * finger stays down. */
static void hold_down(lv_obj_t *tile)
{
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(700);
}

/* The finger, still down, moves to x, y in small steps (the panel reads a fresh position every
 * few milliseconds). */
static void finger_to(int32_t x, int32_t y)
{
    const int32_t x0 = s_px;
    const int32_t y0 = s_py;
    for (int32_t i = 1; i <= 10; i++) {
        s_px = x0 + (x - x0) * i / 10;
        s_py = y0 + (y - y0) * i / 10;
        run(30);
    }
}

static void finger_to_tile(const char *name)
{
    lv_obj_t *t = device_tile_of(name);
    expect(t != NULL, "Smart Home: a tile is missing");
    if (t != NULL) {
        lv_area_t a;
        lv_obj_get_coords(t, &a);
        finger_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    }
}

/* Where every tile of the grid is, to tell whether anything moved: the count. */
static uint32_t tile_places(lv_area_t *out, uint32_t max)
{
    lv_obj_t *grid = by_name("device_grid");
    const uint32_t n = lv_obj_get_child_count(grid);
    for (uint32_t i = 0; i < n && i < max; i++) {
        lv_obj_get_coords(lv_obj_get_child(grid, (int32_t)i), &out[i]);
    }
    return n < max ? n : max;
}

static bool header_says(const char *text)
{
    lv_obj_t *head = by_name("device_arrange_header");
    if (find_label(head, text) != NULL) {
        return true;
    }
    /* BL-220: Room shares the picked tile's row, so a name may end in LVGL's "..." there */
    lv_obj_t *title = head != NULL ? lv_obj_get_child(head, 0) : NULL;
    const char *t = title != NULL ? lv_label_get_text(title) : "";
    const size_t n = strlen(t);
    return n > 3u && strcmp(t + n - 3u, "...") == 0 && strncmp(t, text, n - 3u) == 0;
}

/* HCTL-008: Arrange's drag - a held tile is lifted (picked, as a tap picks it) and takes the place
 * of the tile the finger is well inside, the grid not scrolling under it; a still finger moves it
 * once at most, even where a wide tile can't land under it; release leaves it picked and stores
 * nothing until Done. The arrows put each tile back. Starts from Front Door, Porch Light, Back
 * Door, Garage Door, Hallway, ... on Favorites. */
static void walk_arrange_drag(void)
{
    lv_obj_t *grid = by_name("device_grid");
    lv_obj_t *back = device_tile_of("Back Door");
    expect(back != NULL && device_before("Front Door", "Porch Light") && device_before("Porch Light", "Back Door"),
           "HCTL-008: Favorites isn't in the order the drag starts from");
    if (back == NULL) {
        return;
    }
    const int32_t scrolled = lv_obj_get_scroll_y(grid);
    static uint8_t stored[UI_DEVL_BLOB_MAX];
    static uint8_t after[UI_DEVL_BLOB_MAX];
    const size_t stored_len = host_panel_dev_layout(stored, sizeof(stored));
    hold_down(back);
    expect(header_says("Back Door") && shown("device_arrange_next"),
           "HCTL-008: holding a tile didn't lift it: it isn't picked");
    finger_to_tile("Front Door");
    run(100);
    expect(device_before("Back Door", "Front Door") && device_before("Front Door", "Porch Light"),
           "HCTL-008: a tile dragged onto another didn't take its place");
    expect(lv_obj_get_scroll_y(grid) == scrolled, "HCTL-008: the grid scrolled under a drag");
    shot("devices_arrange_drag");
    s_pressed = false;
    run(300);
    expect(header_says("Back Door") && host_panel_dev_layout(after, sizeof(after)) == stored_len &&
               memcmp(stored, after, stored_len) == 0,
           "HCTL-008: after the drag the tile isn't picked, or the layout was stored before Done");
    tap_name("device_arrange_next");
    run(300);
    tap_name("device_arrange_next");
    run(300);
    expect(device_before("Front Door", "Porch Light") && device_before("Porch Light", "Back Door"),
           "HCTL-008: the arrows didn't put the dragged tile back");
    /* A wide tile onto a one-cell tile at a row's end: it can't fit there, so it goes to the next
     * row and Back Door stays under the finger. Taking Back Door's place again would put Hallway
     * back, and so on: a still finger must move it once and then nothing (no tile flashing back
     * and forth). */
    lv_obj_t *hall = device_tile_of("Hallway");
    expect(hall != NULL && device_before("Garage Door", "Hallway"), "HCTL-008: Hallway isn't after Garage Door");
    if (hall == NULL) {
        return;
    }
    hold_down(hall);
    finger_to_tile("Back Door");
    expect(device_before("Hallway", "Garage Door"), "HCTL-008: the wide tile didn't take Back Door's place");
    lv_area_t before[UI_DEVL_MAX];
    lv_area_t now[UI_DEVL_MAX];
    const uint32_t n = tile_places(before, UI_DEVL_MAX);
    for (int i = 0; i < 20; i++) {
        run(50);
        expect(tile_places(now, UI_DEVL_MAX) == n && memcmp(before, now, sizeof(before[0]) * n) == 0,
               "HCTL-008: under a still finger the tiles moved again");
    }
    s_pressed = false;
    run(300);
    tap_name("device_arrange_next");
    run(300);
    tap_name("device_arrange_next");
    run(300);
    expect(device_before("Back Door", "Garage Door") && device_before("Garage Door", "Hallway") &&
               device_before("Hallway", "Good Night"),
           "HCTL-008: the arrows didn't put Hallway back");
    /* A remote pointer (Panel Manager, workstation) that lets go mid-drag resets the indev
     * (dev_adapter.c release()): LVGL sends the tile only LV_EVENT_INDEV_RESET, no RELEASED (B004).
     * The drag must end there - the tile's scroll flags back - and the next hold must lift again. */
    lv_obj_t *porch = device_tile_of("Porch Light");
    expect(porch != NULL, "HCTL-008: Porch Light's tile is missing");
    if (porch == NULL) {
        return;
    }
    const bool chain = lv_obj_has_flag(porch, LV_OBJ_FLAG_SCROLL_CHAIN);
    const bool scrollable = lv_obj_has_flag(porch, LV_OBJ_FLAG_SCROLLABLE);
    hold_down(porch);
    finger_to_tile("Front Door");
    expect(device_before("Porch Light", "Front Door"), "HCTL-008: Porch Light didn't take Front Door's place");
    s_pressed = false;
    lv_indev_reset(lv_indev_get_next(NULL), NULL); /* the remote pointer let go */
    run(300);
    porch = device_tile_of("Porch Light");
    expect(porch != NULL && lv_obj_has_flag(porch, LV_OBJ_FLAG_SCROLL_CHAIN) == chain &&
               lv_obj_has_flag(porch, LV_OBJ_FLAG_SCROLLABLE) == scrollable,
           "HCTL-008: a drag ended by an indev reset left the tile's scrolling off");
    if (porch == NULL) {
        return;
    }
    hold_down(porch);
    finger_to_tile("Front Door");
    s_pressed = false;
    run(300);
    expect(device_before("Front Door", "Porch Light"),
           "HCTL-008: after an indev reset ended a drag, the next hold didn't lift and move the tile");
    /* a tap still picks (it lifts nothing) */
    tap_device("Porch Light");
    run(300);
    expect(header_says("Porch Light") && device_before("Front Door", "Porch Light"),
           "HCTL-008: a tap after a drag didn't pick the tile, or moved it");
}

/* Whether Favorites' tiles come in this order on the screen. */
static bool tiles_in_order(const char *const *names, size_t n)
{
    for (size_t i = 0; i + 1u < n; i++) {
        if (!device_before(names[i], names[i + 1u])) {
            return false;
        }
    }
    return true;
}

/* HCTL-008: on a page taller than the view, a swipe that starts without a hold still scrolls the
 * grid (and moves nothing), and a tile held at the grid's top edge scrolls it in steady steps - at
 * most one every 150 ms, none larger than a quarter of a row - until a place off the screen comes
 * into reach. Starts with Kitchen Lights wide, last; Front Door is made wide for it (four rows) and
 * small again after, and the arrows put Kitchen Lights back at the end. */
static void walk_arrange_scroll(void)
{
    static const char *const ORDER[] = {"Front Door", "Porch Light", "Back Door",        "Garage Door",
                                        "Hallway",    "Good Night",  "Living Room Lamp", "Kitchen Lights"};
    const size_t n = sizeof(ORDER) / sizeof(ORDER[0]);
    lv_obj_t *grid = by_name("device_grid");
    tap_device("Front Door");
    tap(find_label(by_name("device_arrange_size"), "Wide"));
    run(300);
    lv_obj_update_layout(grid);
    expect(device_is_wide("Front Door") && lv_obj_get_scroll_bottom(grid) > 0 && tiles_in_order(ORDER, n),
           "HCTL-008: four rows of Favorites don't scroll");
    lv_area_t g;
    lv_obj_get_coords(grid, &g);
    drag((g.x1 + g.x2) / 2, g.y2 - 80, -300);
    expect(lv_obj_get_scroll_y(grid) > 0 && tiles_in_order(ORDER, n) && header_says("Front Door"),
           "HCTL-008: a swipe without a hold didn't scroll the grid, or it moved or picked a tile");
    lv_obj_t *kitchen = device_tile_of("Kitchen Lights");
    expect(kitchen != NULL && centre_visible(kitchen), "HCTL-008: the swipe didn't bring the last row into view");
    if (kitchen == NULL) {
        return;
    }
    /* The arrows on a tile below the first screen: the tiles are made again, and the grid keeps its
     * scroll with the picked tile in view (it went back to the top, the tile out of sight) */
    tap_device("Kitchen Lights");
    run(300);
    expect(header_says("Kitchen Lights"), "HCTL-008: a tap didn't pick Kitchen Lights");
    tap_name("device_arrange_prev");
    run(300);
    kitchen = device_tile_of("Kitchen Lights");
    expect(lv_obj_get_scroll_y(grid) > 0 && kitchen != NULL && centre_visible(kitchen) &&
               device_before("Kitchen Lights", "Living Room Lamp"),
           "HCTL-008: an arrow on a tile below the first screen scrolled the grid back to the top, or lost the tile");
    tap_name("device_arrange_next");
    run(300);
    kitchen = device_tile_of("Kitchen Lights");
    expect(lv_obj_get_scroll_y(grid) > 0 && kitchen != NULL && centre_visible(kitchen) && tiles_in_order(ORDER, n),
           "HCTL-008: the arrow back scrolled the grid to the top, or lost the tile");
    if (kitchen == NULL) {
        return;
    }
    /* A tile held on the last row doesn't step the grid (on a panel an edge of half a tile kept
     * stepping under a finger held there): Kitchen Lights lifted and moved within itself. */
    const int32_t at = lv_obj_get_scroll_y(grid);
    hold_down(kitchen);
    finger_to(s_px - DEVICE_CELL_H / 3, s_py);
    run(1000);
    expect(lv_obj_get_scroll_y(grid) == at && tiles_in_order(ORDER, n),
           "HCTL-008: a tile held on the last row stepped the grid");
    s_pressed = false;
    run(300);
    kitchen = device_tile_of("Kitchen Lights");
    if (kitchen == NULL) {
        return;
    }
    hold_down(kitchen);
    finger_to(g.x1 + DEVICE_CELL_W / 2, g.y1 + DEVICE_CELL_H * 3 / 10);
    int32_t y = lv_obj_get_scroll_y(grid);
    uint32_t last = 0;
    uint32_t since = 1000;
    bool steady = true;
    for (int i = 0; i < 500; i++) {
        run(10);
        since += 10;
        const int32_t now = lv_obj_get_scroll_y(grid);
        if (now != y) {
            /* steps under 3 a second, at most half a row each (ui_devices.c DRAG_STEP_MS) */
            steady = steady && since >= 350 && LV_ABS(now - y) <= (DEVICE_CELL_H + DEVICE_GAP) / 2;
            last++;
            since = 0;
            y = now;
        }
    }
    expect(steady && last > 0u && y == 0, "HCTL-008: a tile held at the top edge didn't step the grid to the top");
    expect(device_before("Kitchen Lights", "Porch Light"), "HCTL-008: the held tile didn't come along to the top");
    s_pressed = false;
    run(300);
    for (int i = 0; i < 8; i++) {
        tap_name("device_arrange_next"); /* the last place: dimmed, it does nothing */
        run(100);
    }
    tap_device("Front Door");
    tap(find_label(by_name("device_arrange_size"), "Small"));
    run(300);
    expect(tiles_in_order(ORDER, n) && !device_is_wide("Front Door"),
           "HCTL-008: the arrows and Small didn't put Favorites back");
}

/* HCTL-008: Arrange (mockup-next n-devices-arrange, -tile, -add): every tile rests, a tap picks, the
 * arrows, Small and Wide, Remove and Add edit the page, Done keeps it. */
static void walk_arrange(void)
{
    printf("Smart Home: Arrange\n");
    tap_area("Favorites");
    run(300);
    const ui_dev_t front = *host_panel_device("lock.front_door");
    const ui_dev_t kitchen = *host_panel_device("light.kitchen");
    const ui_dev_t porch = *host_panel_device("light.porch");
    const ui_dev_t hall = *host_panel_device("climate.hallway");
    expect(device_before("Porch Light", "Front Door") && !device_is_wide("Kitchen Lights"),
           "HCTL-008: Favorites isn't in the stored order before arranging");
    tap_name("device_arrange");
    run(300);
    expect(find_label(by_name("device_arrange_header"), "Arrange Favorites") != NULL,
           "HCTL-008: the pencil didn't start arranging Favorites");
    shot("devices_arrange");
    /* nor does a tile's own control, a level deeper than the tile's children: a thermostat's plus,
     * a light's level */
    lv_obj_t *plus = find_visible(device_tile_of("Hallway"), "plus");
    lv_obj_t *level = find_visible(device_tile_of("Porch Light"), "level");
    expect(level != NULL, "HCTL-008: Porch Light's level isn't on its tile while arranging");
    if (plus != NULL) {
        tap(plus);
    }
    if (level != NULL) {
        tap(level);
    }
    /* a tap picks a tile and switches nothing, on a switch, a lock and a thermostat's tile alike */
    tap_device("Front Door");
    tap_device("Kitchen Lights");
    tap_device("Hallway");
    tap_device("Porch Light");
    run(300);
    expect(host_panel_device("lock.front_door")->state == front.state &&
               host_panel_device("light.kitchen")->state == kitchen.state &&
               host_panel_device("light.porch")->state == porch.state &&
               host_panel_device("light.porch")->level == porch.level &&
               host_panel_device("climate.hallway")->setpoint == hall.setpoint,
           "HCTL-008: a tap acted on a device while arranging");
    expect(find_label(by_name("device_arrange_header"), "Porch Light") != NULL &&
               !shown("device_arrange_add") &&
               shown("device_arrange_remove"),
           "HCTL-008: a picked tile's actions aren't in the header");
    shot("devices_arrange_tile");
    /* a later place: the arrows */
    tap_name("device_arrange_next");
    run(300);
    expect(device_before("Front Door", "Porch Light"), "HCTL-008: the arrow didn't move the tile later");
    tap_name("device_arrange_prev");
    run(300);
    expect(device_before("Porch Light", "Front Door"), "HCTL-008: the arrow didn't move the tile earlier");
    tap_name("device_arrange_next");
    run(300);
    walk_arrange_drag();
    /* sizes */
    tap_device("Kitchen Lights");
    tap(find_label(by_name("device_arrange_size"), "Wide"));
    run(300);
    expect(device_is_wide("Kitchen Lights"), "HCTL-008: Wide didn't widen the tile");
    walk_arrange_scroll();
    tap_device("Porch Light");
    tap(find_label(by_name("device_arrange_size"), "Small"));
    run(300);
    expect(!device_is_wide("Porch Light"), "HCTL-008: Small didn't narrow the tile");
    /* off Favorites, and back at the end */
    tap_device("Good Night");
    tap_name("device_arrange_remove");
    run(300);
    expect(device_tile_of("Good Night") == NULL, "HCTL-008: Remove left the tile on Favorites");
    tap_name("device_arrange_add");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2 && find_label(by_name("pick_list"), "Good Night") != NULL,
           "HCTL-008: Add doesn't list the device that was taken off Favorites");
    /* HCTL-008 (BL-177) */
    expect(find_label(lv_screen_active(), "Tap a device to add it at the end") != NULL,
           "HCTL-008 (BL-177): the Add sheet's note doesn't say a tap adds it at the end");
    shot("devices_arrange_add");
    tap(find_label(by_name("pick_list"), "Good Night"));
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2 && find_label(by_name("pick_list"), "Good Night") == NULL &&
               device_before("Kitchen Lights", "Good Night"),
           "HCTL-008: Add didn't put it at the end and keep the list open");
    tap_name("pick_close");
    run(300);
    tap_name("device_arrange_done");
    run(300);
    expect(shown("device_arrange") &&
               !shown("device_arrange_done"),
           "HCTL-008: Done didn't end arranging");
    shot("devices_arranged");
    /* kept: leave and come back */
    tap_name("nav_home");
    run(300);
    tap_name("nav_devices");
    run(300);
    expect(device_before("Front Door", "Porch Light") && device_before("Kitchen Lights", "Good Night") &&
               device_is_wide("Kitchen Lights") && !device_is_wide("Porch Light"),
           "HCTL-008: the arrangement wasn't kept");
    /* a room's page arranges too, without Remove; Add moves devices into the room and Rooms
     * changes the rooms (BL-220 D5, R2), and a picked tile has Room (R1) */
    tap_area("Kitchen");
    run(300);
    tap_name("device_arrange");
    run(300);
    expect(shown("device_arrange_add") && shown("device_arrange_rooms"),
           "BL-220: a room's page doesn't offer Add and Rooms");
    tap_device("Back Door");
    run(300);
    expect(!shown("device_arrange_remove") && shown("device_arrange_next") &&
               shown("device_arrange_room"),
           "HCTL-008, BL-220: a room's page offers Remove, or no arrows, or no Room");
    tap_name("device_arrange_next");
    run(300);
    expect(device_before("Kitchen Lights", "Back Door"), "HCTL-008: a room's page didn't move the tile");
    tap_name("device_arrange_done");
    run(300);
    tap_area("Favorites");
    run(300);
}

/* ---- BL-184 (HCTL-004): a long name stays in its tile; the sheet shows it whole ------------- */

/* The fixture's devices, and the 47-byte names (DEV_NAME_LEN 48) they get: wide letters, names
 * without a space, a multibyte Latin one within the fonts' range, and two that start the same. */
static const char *const LONG_REFS[] = {
    "light.porch",      "lock.front_door",   "lock.back_door", "cover.garage_door", "climate.hallway",
    "scene.good_night", "light.living_lamp", "light.kitchen",  "switch.pool_pump",  "cover.patio_awning",
    "fan.porch",        "light.patio",       "valve.garden",   "lock.side_gate",
};
#define LONG_W "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"
static const char *const LONG_NAMES[] = {
    "LivingRoomNorthWindowFloorLampOverTheArmchair01",
    "Upstairs Hallway Door Lock Beside The Linen Bin",
    "Upstairs Hallway Door Lock Beside The Guest Bed",
    "GarageDoorOpenerNorthBayBesideTheWorkbenchUnit2",
    LONG_W,
    "Good Night: Lights Off Doors Locked Shades Down",
    "L\xC3\xA1mpara de pie del sal\xC3\xB3n junto a las ventanas",
    LONG_W,
    "Garden Fountain Pump Beside The Back Patio Wall",
    "Patio Awning Over The Back Deck By The Hot Tubs",
    "Back Porch Ceiling Fan Beside The Kitchen Doors",
    "Patio String Lights Along The Fence And Pergola",
    "Garden Drip Irrigation Valve For The North Beds",
    "Side Gate Smart Lock Beside The Recycling Bin 2",
};
#define LONG_COUNT (sizeof(LONG_REFS) / sizeof(LONG_REFS[0]))

/* shown is name whole, or LVGL's shortening of it: valid UTF-8, the name's start cut between two
 * characters, then "..." (the stored name keeps every byte). */
static bool shows_name(const char *shown, const char *name)
{
    const size_t len = strlen(shown);
    if (strcmp(shown, name) == 0) {
        return true;
    }
    if (len <= 3u || strcmp(shown + len - 3u, "...") != 0 || len - 3u >= strlen(name)) {
        return false;
    }
    return i18n_utf8_valid(shown, len) && strncmp(shown, name, len - 3u) == 0 &&
           ((uint8_t)name[len - 3u] & 0xC0u) != 0x80u;
}

/* The device whose name this tile shows, whole or shortened (the first that fits), or NULL. */
static const ui_dev_t *tile_device(lv_obj_t *tile)
{
    lv_obj_t *title = lv_obj_find_by_name(tile, "title_label");
    for (size_t i = 0; title != NULL && i < LONG_COUNT; i++) {
        const ui_dev_t *d = host_panel_device(LONG_REFS[i]);
        if (d != NULL && shows_name(lv_label_get_text(title), d->name)) {
            return d;
        }
    }
    return NULL;
}

/* The n-th tile (from 0, in the grid's order) showing this device's name, whole or shortened: two
 * names that start the same read the same on their tiles. */
static lv_obj_t *long_tile(const char *ref, unsigned n)
{
    lv_obj_t *grid = by_name("device_grid");
    const ui_dev_t *d = host_panel_device(ref);
    for (uint32_t i = 0; d != NULL && i < lv_obj_get_child_count(grid); i++) {
        lv_obj_t *tile = lv_obj_get_child(grid, (int32_t)i);
        lv_obj_t *title = lv_obj_find_by_name(tile, "title_label");
        if (title != NULL && visible(tile) && shows_name(lv_label_get_text(title), d->name) && n-- == 0u) {
            return tile;
        }
    }
    return NULL;
}

/* A long press on a tile's name (hold_device by the tile, for a name its tile shortens). */
static void hold_tile(lv_obj_t *tile)
{
    lv_obj_t *l = tile != NULL ? lv_obj_find_by_name(tile, "title_label") : NULL;
    expect(l != NULL, "BL-184: a tile with a long name is missing");
    if (l == NULL) {
        return;
    }
    if (!centre_visible(l)) {
        scroll_into_view(l);
        run(50);
    }
    lv_area_t a;
    lv_obj_get_coords(l, &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(700);
    s_pressed = false;
    run(300);
}

static bool area_in(const lv_area_t *in, const lv_area_t *box)
{
    return in->x1 >= box->x1 && in->y1 >= box->y1 && in->x2 <= box->x2 && in->y2 <= box->y2;
}

/* Every tile on the page keeps its bounds (BL-184 decision; the mockup's
 * devices-long-names): the name whole lines of its font, one, or two on a wide tile
 * with no control showing; the state one line; line spacing zero; the name its device's, whole or
 * shortened with "..."; the chip, the name, the state, each control showing, the arrow's icon and
 * Arrange's grip apart from each other and inside the tile's padding. How many names are shortened. */
static unsigned tiles_keep_their_bounds(const char *what)
{
    lv_obj_t *grid = by_name("device_grid");
    unsigned cut = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); i++) {
        lv_obj_t *tile = lv_obj_get_child(grid, (int32_t)i);
        lv_obj_t *title = lv_obj_find_by_name(tile, "title_label");
        lv_obj_t *detail = lv_obj_find_by_name(tile, "detail_label");
        lv_obj_t *chip = lv_obj_find_by_name(tile, "chip");
        lv_obj_t *top = lv_obj_find_by_name(tile, "top");
        lv_obj_t *more = lv_obj_find_by_name(tile, "more");
        if (!visible(tile) || title == NULL || detail == NULL || chip == NULL || top == NULL) {
            continue;
        }
        lv_obj_t *parts[16];
        size_t n = 0;
        parts[n++] = chip;
        parts[n++] = title;
        parts[n++] = detail;
        bool control = false;
        for (uint32_t c = 0; c < lv_obj_get_child_count(tile) && n < 15u; c++) {
            lv_obj_t *o = lv_obj_get_child(tile, (int32_t)c);
            if (o == top || o == more || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            if (!lv_obj_has_flag(o, LV_OBJ_FLAG_FLOATING)) {
                control = true; /* a slider, keys or a stepper under the words */
            }
            parts[n++] = o; /* ... or Arrange's grip */
        }
        if (more != NULL && visible(more) && lv_obj_get_child_count(more) > 0u && n < 16u) {
            parts[n++] = lv_obj_get_child(more, 0); /* the arrow's icon (its 64 x 72 target is the corner) */
        }
        const ui_dev_t *d = tile_device(tile);
        const bool wide = lv_obj_get_width(tile) > DEVICE_CELL_W;
        const int32_t lines = wide && !control ? 2 : 1;
        const int32_t name_lh = lv_font_get_line_height(lv_obj_get_style_text_font(title, LV_PART_MAIN));
        const int32_t state_lh = lv_font_get_line_height(lv_obj_get_style_text_font(detail, LV_PART_MAIN));
        const char *shown = lv_label_get_text(title);
        char msg[240];
        snprintf(msg, sizeof(msg),
                 "BL-184 (%s): \"%.50s\"'s name isn't %d line(s) of %d px with line spacing 0, or its state one of %d",
                 what, shown, (int)lines, (int)name_lh, (int)state_lh);
        expect(lv_obj_get_height(title) == lines * name_lh && lv_obj_get_content_height(title) == lines * name_lh &&
                   lv_obj_get_style_text_line_space(title, LV_PART_MAIN) == 0 &&
                   lv_obj_get_height(detail) == state_lh && lv_label_get_long_mode(title) == LV_LABEL_LONG_MODE_DOTS &&
                   lv_label_get_long_mode(detail) == LV_LABEL_LONG_MODE_DOTS &&
                   lv_obj_get_style_text_line_space(detail, LV_PART_MAIN) == 0,
               msg);
        snprintf(msg, sizeof(msg),
                 "BL-184 (%s): \"%.50s\" isn't a device's name, whole or cut between characters with ...", what, shown);
        expect(d != NULL, msg);
        if (d != NULL && strcmp(shown, d->name) != 0) {
            cut++;
        }
        lv_area_t room;
        lv_obj_get_coords(tile, &room);
        room.x1 += lv_obj_get_style_pad_left(tile, LV_PART_MAIN);
        room.x2 -= lv_obj_get_style_pad_right(tile, LV_PART_MAIN);
        room.y1 += lv_obj_get_style_pad_top(tile, LV_PART_MAIN);
        room.y2 -= lv_obj_get_style_pad_bottom(tile, LV_PART_MAIN);
        for (size_t a = 0; a < n; a++) {
            lv_area_t aa;
            lv_obj_get_coords(parts[a], &aa);
            const bool corner =
                lv_obj_has_flag(parts[a], LV_OBJ_FLAG_FLOATING) || lv_obj_get_parent(parts[a]) == more;
            snprintf(msg, sizeof(msg), "BL-184 (%s): on \"%.50s\"'s tile, %s runs out of the tile's padding", what,
                     shown, name_of(parts[a]));
            expect(corner || area_in(&aa, &room), msg); /* the arrow and the grip sit in the corner */
            for (size_t b = a + 1u; b < n; b++) {
                lv_area_t bb;
                lv_area_t x;
                lv_obj_get_coords(parts[b], &bb);
                snprintf(msg, sizeof(msg), "BL-184 (%s): on \"%.50s\"'s tile, %s and %s overlap", what, shown,
                         name_of(parts[a]), name_of(parts[b]));
                expect(!lv_area_intersect(&x, &aa, &bb), msg);
            }
        }
    }
    return cut;
}

/* What LVGL asks to redraw over the watched labels (the shortened names and states): a poll that
 * gave a label its text again would lay it out and draw it anew. */
static lv_area_t s_watch[64];
static size_t s_nwatch;
static unsigned s_watch_hits;

static void on_redraw(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    for (size_t i = 0; a != NULL && i < s_nwatch; i++) {
        lv_area_t x;
        if (lv_area_intersect(&x, a, &s_watch[i])) {
            s_watch_hits++;
            return;
        }
    }
}

/* Polls that find nothing changed leave every shortened name and state as it is: the same text,
 * nothing laid out or drawn again over them (LVGL's "..." is in the label's own copy; the tile
 * compares with what it gave it, BL-184). */
static void polls_leave_names_still(const char *what)
{
    lv_obj_t *grid = by_name("device_grid");
    static char before[64][64];
    lv_obj_t *labels[64];
    s_nwatch = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid) && s_nwatch < 62u; i++) {
        lv_obj_t *tile = lv_obj_get_child(grid, (int32_t)i);
        lv_obj_t *two[2] = {lv_obj_find_by_name(tile, "title_label"), lv_obj_find_by_name(tile, "detail_label")};
        for (size_t k = 0; k < 2u; k++) {
            const char *t = two[k] != NULL && visible(two[k]) ? lv_label_get_text(two[k]) : "";
            const size_t len = strlen(t);
            if (len > 3u && strcmp(t + len - 3u, "...") == 0) {
                labels[s_nwatch] = two[k];
                snprintf(before[s_nwatch], sizeof(before[0]), "%s", t);
                lv_obj_get_coords(two[k], &s_watch[s_nwatch++]);
            }
        }
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "BL-184 (%s): no tile shortened its name or state", what);
    expect(s_nwatch > 0u, msg);
    s_watch_hits = 0;
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, on_redraw, LV_EVENT_INVALIDATE_AREA, NULL);
    run(3500); /* three polls of the view or more */
    lv_display_remove_event_cb_with_user_data(disp, on_redraw, NULL);
    bool same = true;
    for (size_t i = 0; i < s_nwatch; i++) {
        same = same && strcmp(lv_label_get_text(labels[i]), before[i]) == 0;
    }
    printf("  %s: %u shortened label(s) watched over the polls, %u redraw(s)\n", what, (unsigned)s_nwatch,
           s_watch_hits);
    snprintf(msg, sizeof(msg), "BL-184 (%s): a poll gave a shortened name or state its text again (redrawn %u times)",
             what, s_watch_hits);
    expect(same && s_watch_hits == 0u, msg);
    s_nwatch = 0;
}

/* The open sheet shows the device's whole name, wrapped in whole lines of its font and broken only
 * between characters, and its state whole; the chip, the switch and Close at the header's first
 * row; everything inside the card, the card no taller than pick_max_h (BL-184). */
static void sheet_shows_whole_name(const char *ref, bool with_switch, const char *what)
{
    lv_obj_t *sheet = device_sheet_open();
    char msg[200];
    snprintf(msg, sizeof(msg), "BL-184 (%s): a long press didn't open the sheet", what);
    expect(sheet != NULL, msg);
    if (sheet == NULL) {
        return;
    }
    const ui_dev_t *d = host_panel_device(ref);
    lv_obj_t *card = lv_obj_find_by_name(sheet, "device_card");
    lv_obj_t *chip = lv_obj_find_by_name(sheet, "sheet_chip");
    lv_obj_t *close = lv_obj_find_by_name(sheet, "device_close");
    lv_obj_t *sw = lv_obj_find_by_name(sheet, "sheet_switch");
    lv_obj_t *state = lv_obj_find_by_name(sheet, "sheet_state");
    lv_obj_t *body = lv_obj_find_by_name(sheet, "device_body");
    lv_obj_t *title = d != NULL ? find_label(card, d->name) : NULL;
    snprintf(msg, sizeof(msg), "BL-184 (%s): the sheet's title isn't the whole stored name, wrapped", what);
    expect(title != NULL && strcmp(lv_label_get_text(title), d->name) == 0 &&
               lv_label_get_long_mode(title) == LV_LABEL_LONG_MODE_WRAP,
           msg);
    if (title == NULL || card == NULL || chip == NULL || close == NULL || state == NULL || body == NULL) {
        return;
    }
    /* the lines LVGL breaks the title into: each starts on a character, and they are its height */
    const lv_font_t *font = lv_obj_get_style_text_font(title, LV_PART_MAIN);
    lv_text_attributes_t at;
    memset(&at, 0, sizeof(at));
    at.letter_space = lv_obj_get_style_text_letter_space(title, LV_PART_MAIN);
    at.line_space = lv_obj_get_style_text_line_space(title, LV_PART_MAIN);
    at.max_width = lv_obj_get_content_width(title);
    at.text_flags = LV_TEXT_FLAG_NONE;
    const char *name = d->name;
    const uint32_t len = (uint32_t)strlen(name);
    uint32_t at_byte = 0;
    int32_t lines = 0;
    bool between = true;
    while (at_byte < len && lines < 20) {
        const uint32_t step = lv_text_get_next_line(name + at_byte, len - at_byte, font, NULL, &at);
        if (step == 0u) {
            break;
        }
        at_byte += step;
        lines++;
        between = between && (at_byte >= len || ((uint8_t)name[at_byte] & 0xC0u) != 0x80u);
    }
    const int32_t lh = lv_font_get_line_height(font);
    printf("  %s: the title in %d line(s) of %d px\n", what, (int)lines, (int)lv_obj_get_content_width(title));
    snprintf(msg, sizeof(msg), "BL-184 (%s): the sheet's title breaks inside a character, or isn't %d whole line(s)",
             what, (int)lines);
    expect(between && at_byte == len && lv_obj_get_height(title) == lines * lh + at.line_space * (lines - 1), msg);
    snprintf(msg, sizeof(msg), "BL-184 (%s): the sheet's state isn't whole, wrapped", what);
    expect(strcmp(lv_label_get_text(state), lv_subject_get_string(&subject_sheet_note)) == 0 &&
               lv_label_get_long_mode(state) == LV_LABEL_LONG_MODE_WRAP &&
               lv_subject_get_string(&subject_sheet_note)[0] != '\0',
           msg);
    /* the header's first row: the title and the chip at its top, the switch and Close centred on
     * the chip */
    lv_area_t c;
    lv_area_t x;
    lv_area_t s;
    lv_area_t t;
    lv_obj_get_coords(chip, &c);
    lv_obj_get_coords(close, &x);
    lv_obj_get_coords(state, &s);
    lv_obj_get_coords(title, &t);
    const bool sw_shown = sw != NULL && visible(sw);
    snprintf(msg, sizeof(msg), "BL-184 (%s): the sheet opened isn't the held device's (its switch %s)", what,
             sw_shown ? "shown" : "hidden");
    expect(sw_shown == with_switch, msg); /* two tiles with the same name: the hold opened the right one */
    lv_area_t w = c;
    if (sw_shown) {
        lv_obj_get_coords(sw, &w);
    }
    snprintf(msg, sizeof(msg), "BL-184 (%s): Close or the switch isn't at the header's first row", what);
    expect(x.y1 - c.y1 == (lv_obj_get_height(chip) - lv_obj_get_height(close)) / 2 && t.y1 == c.y1 &&
               (!sw_shown || w.y1 - c.y1 == (lv_obj_get_height(chip) - lv_obj_get_height(sw)) / 2),
           msg);
    /* inside the card */
    lv_area_t room;
    lv_obj_get_content_coords(card, &room);
    lv_area_t b;
    lv_obj_get_coords(body, &b);
    snprintf(msg, sizeof(msg),
             "BL-184 (%s): the sheet's title, state, Close or body run out of the card, or the card is over pick_max_h",
             what);
    expect(area_in(&t, &room) && area_in(&s, &room) && area_in(&x, &room) && area_in(&b, &room) &&
               lv_obj_get_height(card) <= PICK_MAX_H && s.y1 > t.y2 && (!sw_shown || area_in(&w, &room)),
           msg);
}

/* The device body's scroll position each time the screen is drawn: the fewest from the top and how
 * many frames (a refit that let a frame show the body at its top would read here). */
static lv_obj_t *s_scroll_body;
static int32_t s_scroll_low;
static unsigned s_scroll_frames;

static void on_render(lv_event_t *e)
{
    (void)e;
    if (s_scroll_body != NULL) {
        const int32_t y = lv_obj_get_scroll_y(s_scroll_body);
        s_scroll_low = y < s_scroll_low ? y : s_scroll_low;
        s_scroll_frames++;
    }
}

/* A sheet whose header and body are taller than the card (HCTL-004, BL-184): the body is trimmed to
 * stay inside the card, its border counted on both sides; a finger scrolls the body while the
 * header and Close stay still; and with the body scrolled to its end, its words changing (a failed
 * change, "Didn't change", and back; a change under way no longer changes them, BL-219) keep where it was, clamped to its new range, in every frame drawn
 * (the refit after a change of words used to snap it back to its top). On the open sheet of `ref`,
 * whose source is idle; sends nothing. */
static void sheet_body_keeps_its_scroll(const char *ref, const char *what)
{
    lv_obj_t *sheet = device_sheet_open();
    lv_obj_t *card = sheet != NULL ? lv_obj_find_by_name(sheet, "device_card") : NULL;
    lv_obj_t *body = sheet != NULL ? lv_obj_find_by_name(sheet, "device_body") : NULL;
    lv_obj_t *close = sheet != NULL ? lv_obj_find_by_name(sheet, "device_close") : NULL;
    lv_obj_t *chip = sheet != NULL ? lv_obj_find_by_name(sheet, "sheet_chip") : NULL;
    lv_obj_t *state = sheet != NULL ? lv_obj_find_by_name(sheet, "sheet_state") : NULL;
    char msg[480];
    snprintf(msg, sizeof(msg), "BL-184 (%s): the sheet, its card, body, chip, words or Close is missing", what);
    expect(card != NULL && body != NULL && close != NULL && chip != NULL && state != NULL, msg);
    if (card == NULL || body == NULL || close == NULL || chip == NULL || state == NULL) {
        return;
    }
    const unsigned acts = host_panel_device_acts();
    lv_obj_update_layout(card);
    const int32_t border = lv_obj_get_style_border_width(card, LV_PART_MAIN);
    lv_area_t k;
    lv_area_t b;
    lv_obj_get_coords(card, &k);
    lv_obj_get_coords(body, &b);
    const int32_t over = lv_obj_get_scroll_top(body) + lv_obj_get_scroll_bottom(body);
    printf("  %s: card %d px (max %d, border %d), body %d px at %d..%d in %d..%d, %d px more to scroll\n", what,
           (int)lv_obj_get_height(card), (int)PICK_MAX_H, (int)border, (int)lv_obj_get_height(body), (int)b.y1,
           (int)b.y2, (int)k.y1, (int)k.y2, (int)over);
    snprintf(msg, sizeof(msg), "BL-184 (%s): the sheet's header and body don't overflow the card, or it isn't at its top",
             what);
    expect(over > 0 && lv_obj_get_scroll_y(body) == 0, msg);
    snprintf(msg, sizeof(msg),
             "BL-184 (%s): the trimmed body runs out of the card (its padding and border on both sides), or the "
             "card is over pick_max_h",
             what);
    expect(lv_obj_get_height(card) <= PICK_MAX_H &&
               b.y2 <= k.y2 - border - lv_obj_get_style_pad_bottom(card, LV_PART_MAIN) &&
               b.x1 >= k.x1 + border + lv_obj_get_style_pad_left(card, LV_PART_MAIN) &&
               b.x2 <= k.x2 - border - lv_obj_get_style_pad_right(card, LV_PART_MAIN),
           msg);

    /* a finger scrolls the body; the header and Close stay where they were */
    lv_area_t c0;
    lv_area_t x0;
    lv_obj_get_coords(chip, &c0);
    lv_obj_get_coords(close, &x0);
    for (int i = 0; i < 6 && lv_obj_get_scroll_bottom(body) > 0; i++) {
        lv_obj_get_coords(body, &b);
        drag((b.x1 + b.x2) / 2, (b.y1 + b.y2) / 2, -200); /* the finger may leave the body as it goes */
    }
    lv_area_t c1;
    lv_area_t x1;
    lv_obj_get_coords(chip, &c1);
    lv_obj_get_coords(close, &x1);
    const int32_t end = lv_obj_get_scroll_y(body);
    printf("  %s: scrolled the body to %d px of %d, %d left\n", what, (int)end, (int)over,
           (int)lv_obj_get_scroll_bottom(body));
    snprintf(msg, sizeof(msg),
             "BL-184 (%s): a finger didn't scroll the body to its end, the header or Close moved, or it sent something",
             what);
    expect(end > 0 && lv_obj_get_scroll_bottom(body) <= 0 && c1.y1 == c0.y1 && x1.y1 == x0.y1 && x1.x1 == x0.x1 &&
               host_panel_device_acts() == acts && device_sheet_open() == sheet,
           msg);
    shot("device_sheet_long_scrolled");

    /* its words change: where the body was is kept, clamped, in every frame drawn */
    char before[128];
    snprintf(before, sizeof(before), "%s", lv_label_get_text(state));
    s_scroll_body = body;
    s_scroll_low = INT32_MAX;
    s_scroll_frames = 0;
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, on_render, LV_EVENT_RENDER_START, NULL);
    host_panel_device_failed(ref, ACT_SET_MODE);
    run(1200);
    const int32_t pend = lv_obj_get_scroll_y(body);
    const int32_t pend_max = pend + lv_obj_get_scroll_bottom(body);
    const int32_t pend_low = s_scroll_low;
    const unsigned pend_frames = s_scroll_frames;
    const bool changed = strcmp(lv_label_get_text(state), before) != 0;
    printf("  %s: \"%s\" -> \"%s\": the body at %d px (range %d), %u frame(s) drawn, the lowest at %d\n", what,
           before, lv_label_get_text(state), (int)pend, (int)pend_max, pend_frames, (int)pend_low);
    const int32_t want = end < pend_max ? end : pend_max;
    snprintf(msg, sizeof(msg),
             "BL-184 (%s): a change of words (\"%s\" -> \"%s\") moved the scrolled body to %d px, not %d (clamped), or a "
             "frame showed it at %d",
             what, before, lv_label_get_text(state), (int)pend, (int)want, (int)pend_low);
    expect(changed && pend_frames > 0u && pend >= want - 2 && pend <= want + 2 && pend_low >= want - 2, msg);
    shot("device_sheet_long_scrolled_failed");
    s_scroll_low = INT32_MAX;
    s_scroll_frames = 0;
    host_panel_device_settle(ref);
    run(1200);
    const int32_t back = lv_obj_get_scroll_y(body);
    const int32_t back_max = back + lv_obj_get_scroll_bottom(body);
    const int32_t want_back = pend < back_max ? pend : back_max;
    printf("  %s: back to \"%s\": the body at %d px (range %d), %u frame(s), the lowest at %d\n", what,
           lv_label_get_text(state), (int)back, (int)back_max, s_scroll_frames, (int)s_scroll_low);
    snprintf(msg, sizeof(msg), "BL-184 (%s): its words coming back moved the scrolled body to %d px, not %d (clamped)",
             what, (int)back, (int)want_back);
    expect(strcmp(lv_label_get_text(state), before) == 0 && back >= want_back - 2 && back <= want_back + 2 &&
               s_scroll_low >= want_back - 2,
           msg);
    lv_display_remove_event_cb_with_user_data(disp, on_render, NULL);
    s_scroll_body = NULL;
    snprintf(msg, sizeof(msg), "BL-184 (%s): scrolling the sheet's body sent something", what);
    expect(host_panel_device_acts() == acts, msg);
}

/* BL-184: 47-byte names on every kind of tile, small and wide, with and without a control; Arrange;
 * the thermostat's stepper going and coming back; a long offline date; repeated polls; the sheets,
 * through a long press, of kinds without an arrow; a sheet taller than its card keeping its scroll.
 * Leaves the names as they were. */
static void long_names_stay_in_bounds(void)
{
    printf("Smart Home: long names (BL-184)\n");
    static char was[LONG_COUNT][DEV_NAME_LEN];
    for (size_t i = 0; i < LONG_COUNT; i++) {
        const ui_dev_t *d = host_panel_device(LONG_REFS[i]);
        snprintf(was[i], sizeof(was[i]), "%s", d != NULL ? d->name : "");
        expect(d != NULL && strlen(LONG_NAMES[i]) == 47u && i18n_utf8_valid(LONG_NAMES[i], 47u),
               "BL-184: a long name isn't 47 bytes of UTF-8, or its device is missing");
        host_panel_device_rename(LONG_REFS[i], LONG_NAMES[i]);
    }
    const int64_t DAY = 24LL * 3600LL * 1000LL;
    const int64_t pump_since = host_panel_device("switch.pool_pump")->since_ms;
    host_panel_device_since("switch.pool_pump", pump_since - 366LL * DAY); /* "Offline since Sep 18, 2025" */
    tap_area("Favorites");
    run(1200);
    expect(tiles_keep_their_bounds("Favorites") >= 6u, "BL-184: Favorites' long names weren't shortened");
    shot("devices_long_names");
    polls_leave_names_still("Favorites");
    /* the thermostat holds a range: no stepper, so two lines; the stepper back, one */
    const int32_t set = host_panel_device_setpoint("climate.hallway", UI_DEV_TEMP_UNKNOWN);
    run(1200);
    lv_obj_t *hall = long_tile("climate.hallway", 0);
    lv_obj_t *hall_name = hall != NULL ? lv_obj_find_by_name(hall, "title_label") : NULL;
    expect(hall_name != NULL && find_visible(hall, "plus") == NULL &&
               lv_obj_get_height(hall_name) == DEVICE_NAME_TWO_H,
           "BL-184: a thermostat without its stepper didn't get two lines of name");
    tiles_keep_their_bounds("no stepper");
    shot("devices_long_names_no_stepper");
    host_panel_device_setpoint("climate.hallway", set);
    run(1200);
    hall = long_tile("climate.hallway", 0);
    hall_name = hall != NULL ? lv_obj_find_by_name(hall, "title_label") : NULL;
    expect(hall_name != NULL && find_visible(hall, "plus") != NULL && lv_obj_get_height(hall_name) == DEVICE_NAME_H,
           "BL-184: a thermostat whose stepper came back kept two lines of name");
    tiles_keep_their_bounds("stepper back");

    /* the sheets: the whole name, a hold opening them on tiles without an arrow too */
    unsigned acts = host_panel_device_acts();
    lv_obj_t *front = long_tile("lock.front_door", 0);
    lv_obj_t *back = long_tile("lock.front_door", 1);
    expect(front != NULL && back != NULL, "BL-184: the two names that start the same aren't both on Favorites");
    hold_tile(front);
    char first[DEV_NAME_LEN];
    snprintf(first, sizeof(first), "%s", device_sheet_open() != NULL ? lv_subject_get_string(&subject_sheet_title) : "");
    const bool linen = strcmp(first, LONG_NAMES[1]) == 0;
    expect(linen || strcmp(first, LONG_NAMES[2]) == 0, "BL-184: a hold on a lock didn't show its whole name");
    sheet_shows_whole_name(linen ? "lock.front_door" : "lock.back_door", false, "a lock");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }
    hold_tile(back);
    expect(device_sheet_open() != NULL &&
               strcmp(lv_subject_get_string(&subject_sheet_title), LONG_NAMES[linen ? 2 : 1]) == 0,
           "BL-184: two names that start the same opened the same sheet");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }
    /* the thermostat's and the kitchen light's 47 W's read the same: the thermostat's tile comes
     * first on Favorites, the light's second */
    static const struct {
        const char *ref;
        unsigned n;
        bool sw;
        const char *what;
        const char *shot;
    } SHEETS[] = {
        {"light.kitchen", 1u, true, "wide letters beside the switch", "device_sheet_long_name"},
        {"climate.hallway", 0u, false, "wide letters without a switch", "device_sheet_long_climate"},
        {"cover.garage_door", 0u, false, "a name without a space", "device_sheet_long_unbroken"},
        {"light.living_lamp", 0u, true, "a multibyte name", NULL},
        {"scene.good_night", 0u, false, "a scene (no arrow)", NULL},
    };
    for (size_t i = 0; i < sizeof(SHEETS) / sizeof(SHEETS[0]); i++) {
        hold_tile(long_tile(SHEETS[i].ref, SHEETS[i].n));
        sheet_shows_whole_name(SHEETS[i].ref, SHEETS[i].sw, SHEETS[i].what);
        if (SHEETS[i].shot != NULL) {
            shot(SHEETS[i].shot);
        }
        if (device_sheet_open() != NULL) {
            close_device_sheet();
        }
    }
    /* A sheet taller than its card. In English at 1024x600 a 47-byte name of words wraps to four
     * lines at most, and the tallest body (the thermostat's set point and four modes) still leaves
     * the card inside pick_max_h; a name its source sent with line breaks between its words (kept,
     * as every byte is) takes twelve: over the thermostat's set point and modes, taller than the
     * card */
    static const char TALL[] = "Up\nIn\nThe\nHall\nBy\nThe\nGuest\nRoom\nDoor\nLeft\nNo\n2";
    expect(strlen(TALL) == 47u, "BL-184: the tall name isn't 47 bytes");
    host_panel_device_rename("climate.hallway", TALL);
    run(1200);
    hold_tile(long_tile("climate.hallway", 0));
    sheet_body_keeps_its_scroll("climate.hallway", "a thermostat's sheet taller than the card");
    /* the body is the sheet's own, reused at every open: closed scrolled and opened again, it starts
     * at its top */
    lv_obj_t *tall = device_sheet_open();
    lv_obj_t *tall_body = tall != NULL ? lv_obj_find_by_name(tall, "device_body") : NULL;
    const int32_t left_at = tall_body != NULL ? lv_obj_get_scroll_y(tall_body) : 0;
    if (tall != NULL) {
        close_device_sheet();
    }
    hold_tile(long_tile("climate.hallway", 0));
    tall = device_sheet_open();
    tall_body = tall != NULL ? lv_obj_find_by_name(tall, "device_body") : NULL;
    if (tall_body != NULL) {
        lv_obj_update_layout(tall_body);
    }
    printf("  closed scrolled to %d px, opened again at %d px\n", (int)left_at,
           tall_body != NULL ? (int)lv_obj_get_scroll_y(tall_body) : -1);
    expect(left_at > 0 && tall_body != NULL && lv_obj_get_scroll_y(tall_body) == 0 &&
               lv_obj_get_scroll_bottom(tall_body) > 0,
           "BL-184: a tall sheet closed scrolled didn't open again at its top");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }
    host_panel_device_rename("climate.hallway", LONG_NAMES[4]);
    run(1200);

    /* Arrange draws the same lines; the grip keeps its corner clear of the names */
    tap_name("device_arrange");
    run(300);
    tiles_keep_their_bounds("arranging");
    shot("devices_long_names_arrange");
    tap_name("device_arrange_done");
    run(300);

    /* Outside: a fan's and a cover's keys, a small switch offline since a year ago ("Offline since
     * Sep 18, 2025" shortened on its tile, whole on its sheet), a light without a level, a valve */
    tap_area("Outside");
    run(1200);
    expect(tiles_keep_their_bounds("Outside") >= 5u, "BL-184: Outside's long names weren't shortened");
    lv_obj_t *pump = long_tile("switch.pool_pump", 0);
    expect(pump != NULL && find_label_shortened(pump, "Offline since Sep 18, 2025") != NULL,
           "BL-184: a small tile's \"Offline since Sep 18, 2025\" isn't shortened with ...");
    shot("devices_long_names_outside");
    polls_leave_names_still("Outside");
    hold_tile(pump);
    sheet_shows_whole_name("switch.pool_pump", true, "an offline switch (no arrow)");
    expect(device_sheet_open() != NULL &&
               strcmp(lv_subject_get_string(&subject_sheet_note), "Offline since Sep 18, 2025") == 0,
           "BL-184: an offline switch's sheet doesn't say \"Offline since Sep 18, 2025\" whole");
    shot("device_sheet_long_offline");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }
    hold_tile(long_tile("light.patio", 0));
    sheet_shows_whole_name("light.patio", true, "a light without a level (no arrow)");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }
    expect(host_panel_device_acts() == acts, "BL-184: a long press on a tile sent something");

    /* the stored names keep every byte */
    for (size_t i = 0; i < LONG_COUNT; i++) {
        expect(strcmp(host_panel_device(LONG_REFS[i])->name, LONG_NAMES[i]) == 0, "BL-184: a stored name changed");
        host_panel_device_rename(LONG_REFS[i], was[i]);
    }
    host_panel_device_since("switch.pool_pump", pump_since);
    tap_area("Favorites");
    run(1200);
}

/* The simulated panel's clock as the UI reads it now (UTC ms; 0 while it isn't set). */
static int64_t preview_now_ms(void)
{
    ui_backend_t b;
    host_panel_backend(&b);
    return b.epoch_ms(b.ctx);
}

/* ---- BL-220: rooms made on the panel, a master code by setting, the start screen ----------
 * The proposal BL-220-device-rooms.js (approved 2026-10-19), its screens as shots. Everything is
 * put back as it was: the rooms follow Home Assistant's areas again, nothing asks for a code, and
 * the screen rests on Security. */
static hc_home_t s_home_seen;

/* The house's settings as the panel stores them now. */
static const hc_home_t *home_now(void)
{
    static uint8_t blob[HC_HOME_BLOB_MAX];
    const size_t len = host_panel_dev_home(blob, sizeof(blob));
    if (len == 0u || !hc_home_decode(&s_home_seen, blob, len)) {
        hc_home_clear(&s_home_seen);
    }
    return &s_home_seen;
}

/* The name of the room a device is in now, "" none. */
static const char *room_now(const char *ref)
{
    const hc_home_t *h = home_now();
    const int r = hc_home_room_of(h, ref, host_panel_device(ref)->area);
    return r >= 0 ? h->rooms[r].name : "";
}

static void tap_pick(const char *title)
{
    lv_obj_t *l = find_label(by_name("pick_list"), title);
    expect(l != NULL, "BL-220: a row missing from the sheet");
    if (l != NULL) {
        tap(l);
    }
    run(300);
}

static void walk_rooms(void)
{
    printf("Smart Home: rooms (BL-220)\n");
    tap_name("nav_devices");
    run(300);
    tap_area("Favorites");
    run(300);
    /* R3: Home Assistant's areas became the panel's rooms, in the order they came */
    {
        const hc_home_t *h = home_now();
        expect(h->nrooms == 3u && strcmp(h->rooms[0].name, "Living room") == 0 &&
                   strcmp(room_now("light.porch"), "Outside") == 0,
               "BL-220 R3: the areas didn't become rooms, or Porch Light isn't in Outside");
    }
    shot("bl-220-no-header");
    /* R1: a picked tile's Room - its source's room ticked - then New room, named on the keyboard */
    tap_name("device_arrange");
    run(300);
    expect(shown("device_arrange_rooms") && shown("device_arrange_add"),
           "BL-220 R2: Arrange has no Rooms beside Add");
    tap_device("Porch Light");
    run(300);
    expect(shown("device_arrange_room"), "BL-220 R1: a picked tile has no Room");
    shot("bl-220-arrange-pick");
    tap_name("device_arrange_room");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2 &&
               find_label(by_name("pick_list"), "Room from its source") != NULL &&
               find_label(by_name("pick_list"), "New room") != NULL &&
               find_label(by_name("pick_list"), "No room") != NULL,
           "BL-220 R1: the room picker doesn't offer its source's room, New room and No room");
    tap_pick("New room");
    sheet_text("Patio");
    shot("bl-220-room-new");
    tap_name("sheet_done");
    run(300);
    expect(strcmp(room_now("light.porch"), "Patio") == 0 && hc_home_placed(home_now(), "light.porch"),
           "BL-220 R1: New room didn't make Patio with Porch Light in it");
    /* a name another room has is refused, and the keyboard opens again with it */
    tap_name("device_arrange_room");
    run(300);
    tap_pick("New room");
    sheet_text("outside");
    tap_name("sheet_done");
    run(300);
    expect(find_visible(lv_screen_active(), "sheet_text") != NULL && home_now()->nrooms == 4u,
           "BL-220 R2: a room's name taken twice wasn't refused");
    tap_name("sheet_cancel");
    run(300);
    /* the room picker with rooms: Porch Light in Patio, ticked */
    tap_name("device_arrange_room");
    run(300);
    shot("bl-220-room-pick");
    tap_name("pick_close");
    run(300);
    tap_name("device_arrange_done");
    run(300);
    expect(find_label(by_name("device_areas"), "Patio") != NULL && no_title(),
           "BL-220: the new room has no tab");
    tap_area("Patio");
    run(300);
    expect(device_tile_of("Porch Light") != NULL && device_tile_of("Garage Door") == NULL,
           "BL-220: Patio's page isn't Porch Light alone");
    shot("bl-220-room-made");

    /* R2: Rooms - rename, earlier, remove (its devices stay, in no room) */
    tap_name("device_arrange");
    run(300);
    tap_name("device_arrange_rooms");
    run(300);
    expect(find_label(by_name("pick_list"), "Patio") != NULL && find_label(by_name("pick_list"), "New room") != NULL,
           "BL-220 R2: Rooms doesn't list the rooms and New room");
    shot("bl-220-rooms-sheet");
    tap_pick("Patio");
    shot("bl-220-room-sheet");
    tap_pick("Rename");
    sheet_text("Back patio");
    tap_name("sheet_done");
    run(300);
    expect(strcmp(room_now("light.porch"), "Back patio") == 0, "BL-220 R2: Rename didn't rename Patio");
    tap_pick("Earlier");
    {
        const hc_home_t *h = home_now();
        expect(h->nrooms == 4u && strcmp(h->rooms[2].name, "Back patio") == 0,
               "BL-220 R2: Earlier didn't move the room one tab to the left");
    }
    tap_pick("Remove room");
    expect(lv_subject_get_int(&subject_sheet) == 3, "BL-220 R2: Remove room didn't ask first");
    shot("bl-220-room-remove");
    tap_name("dialog_ok");
    run(300);
    expect(home_now()->nrooms == 3u && strcmp(room_now("light.porch"), "") == 0 &&
               hc_home_placed(home_now(), "light.porch"),
           "BL-220 R2: removing the room didn't leave Porch Light in no room");
    if (lv_subject_get_int(&subject_sheet) != 0) {
        tap_name("pick_close"); /* Rooms again */
        run(300);
    }
    /* R3: back to its source's room */
    tap_device("Porch Light");
    run(300);
    tap_name("device_arrange_room");
    run(300);
    tap_pick("Room from its source");
    expect(strcmp(room_now("light.porch"), "Outside") == 0 && !hc_home_placed(home_now(), "light.porch"),
           "BL-220 R3: \"Room from its source\" didn't put Porch Light back in Outside");
    tap_name("device_arrange_done");
    run(300);

    /* D5: Add on a room's page moves a device into it */
    tap_area("Kitchen");
    run(300);
    tap_name("device_arrange");
    run(300);
    tap_name("device_arrange_add");
    run(300);
    expect(find_label(by_name("pick_list"), "Garage Door") != NULL &&
               find_label(by_name("pick_list"), "Back Door") == NULL,
           "BL-220 D5: a room's Add doesn't list the devices of other rooms only");
    shot("bl-220-room-add");
    tap_pick("Garage Door");
    expect(strcmp(room_now("cover.garage_door"), "Kitchen") == 0 && lv_subject_get_int(&subject_sheet) == 2,
           "BL-220 D5: Add didn't move Garage Door into Kitchen, or closed the list");
    tap_name("pick_close");
    run(300);
    tap_device("Garage Door");
    run(300);
    tap_name("device_arrange_room");
    run(300);
    tap_pick("Room from its source");
    tap_name("device_arrange_done");
    run(300);
    expect(strcmp(room_now("cover.garage_door"), "Outside") == 0, "BL-220: Garage Door didn't go back");

    /* R5: Settings > Smart Home, a master's code first */
    tap_name("bar_settings");
    run(300);
    expect(shown("set_smarthome"), "BL-220 R5: the Settings list has no Smart Home");
    tap_name("set_smarthome");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(300);
    }
    expect(find_label(lv_screen_active(), "Arranging asks for a master code") != NULL &&
               find_label(lv_screen_active(), "Hallway") != NULL,
           "BL-220 R5: Settings > Smart Home doesn't have Arrange's switch and the devices");
    tap_text("Hallway");
    run(300);
    expect(hc_home_protected(home_now(), "climate.hallway"),
           "BL-220 R5: the Hallway's switch didn't make it ask for a master code");
    shot("bl-220-settings-smart-home");
    /* a protected thermostat: a key on its tile, its + opens the pad, a master's code passes, and
     * the next change goes through without asking (D7) */
    tap_name("nav_devices");
    run(1200);
    tap_area("Favorites");
    run(300);
    lv_obj_t *hall = device_tile_of("Hallway");
    expect(hall != NULL && find_label(hall, "\xEE\x92\xA3") != NULL,
           "BL-220 R5: a protected device's tile has no key");
    const int32_t was = host_panel_device("climate.hallway")->setpoint;
    const unsigned acts = host_panel_device_acts();
    tap(lv_obj_find_by_name(hall, "plus"));
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1 &&
               host_panel_device("climate.hallway")->setpoint == was,
           "BL-220 R5: a protected thermostat's + didn't ask for a master code first");
    shot("bl-220-device-code");
    type("123456k");
    run(1300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               host_panel_device("climate.hallway")->setpoint != was,
           "BL-220 R5: the master code didn't let the change through");
    tap(lv_obj_find_by_name(device_tile_of("Hallway"), "plus"));
    run(1300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 && host_panel_device_acts() > acts + 2u,
           "BL-220 D7: the next change asked for the code again within 90 s");
    /* R5: arranging asks for a master code once it is switched on */
    tap_name("bar_settings");
    run(300);
    tap_name("set_smarthome");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(300);
    }
    tap_text("Arranging asks for a master code");
    run(300);
    expect(hc_home_arrange_code(home_now()), "BL-220 R5: Arrange's switch didn't switch");
    tap_name("nav_devices");
    run(1200);
    tap_name("device_arrange");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1 && !shown("device_arrange_done"),
           "BL-220 R5: the pencil didn't ask for a master code");
    shot("bl-220-arrange-code");
    type("123456k");
    run(300);
    expect(shown("device_arrange_done"), "BL-220 R5: the master code didn't open Arrange");
    tap_name("device_arrange_done");
    run(300);

    /* R8: Smart Home as the start screen - after 90 s left alone the panel rests there */
    tap_name("bar_settings");
    run(300);
    tap_name("set_display");
    run(300);
    tap_text("Start screen");
    run(300);
    shot("bl-220-start-screen-pick");
    tap_pick("Smart Home");
    {
        ui_options_t saved;
        expect(host_panel_saved_options(&saved) && saved.start_view == UI_START_SMART_HOME,
               "BL-220 R8: the start screen wasn't stored");
    }
    tap_name("nav_home");
    run(91000);
    wake();
    run(300);
    expect(lv_subject_get_int(&subject_view) == 6, "BL-220 R8: 90 s alone didn't rest on Smart Home");
    /* back as it was: Security, no codes, the rooms Home Assistant's */
    tap_name("bar_settings");
    run(300);
    tap_name("set_display");
    run(300);
    tap_text("Start screen");
    run(300);
    tap_pick("Security");
    host_panel_dev_home_set(NULL, 0);
    tap_name("nav_home");
    run(300);
    tap_name("nav_devices");
    run(1200);
    tap_area("Favorites");
    run(300);
}

/* Smart Home (docs/features/24, BL-045; mockup-next n-devices, n-devices-outside, n-device-unlock). */
static void shot(const char *name);
static void walk_smart_home(void)
{
    printf("Smart Home\n");
    /* HCTL-003: with a device, the rail has Smart Home; Favorites opens first */
    tap_name("nav_devices");
    run(300);
    expect(no_title() && tile_says("Back Door", "Unlocked", true),
           "BL-220 R7: Smart Home has a title or a note over it, or Back Door isn't unlocked");
    shot("devices");
    /* HCTL-006 (BL-179): today's time; an earlier day's date, with the year for an earlier year;
     * none for a time more than a minute ahead of the clock. The preview's clock is UTC, so local
     * midnight is the UTC one here (ui_words_test.c covers another zone and New Year). */
    expect(find_label(by_name("device_grid"), "Ran at 8:30 PM") != NULL,
           "HCTL-006 (BL-179): Good Night doesn't say it ran at 8:30 PM");
    {
        const int64_t ran = host_panel_device_since("scene.good_night", 0);
        host_panel_device_since("scene.good_night", ran - 24LL * 3600LL * 1000LL);
        run(1200);
        lv_obj_t *night = device_tile_of("Good Night");
        expect(night != NULL && find_label(night, "Ran on Sep 18") != NULL && find_label_with(night, "Ran at") == NULL &&
                   find_label(night, "Scene") == NULL,
               "HCTL-006 (BL-179): a scene that ran the day before doesn't say \"Ran on Sep 18\"");
        shot("devices_ran_on");
        host_panel_device_since("scene.good_night", ran - 366LL * 24LL * 3600LL * 1000LL);
        run(1200);
        night = device_tile_of("Good Night");
        expect(night != NULL && find_label(night, "Ran on Sep 18, 2025") != NULL,
               "HCTL-006 (BL-179): a scene that ran a year before doesn't say \"Ran on Sep 18, 2025\"");
        shot("devices_ran_on_year");
        /* local midnight, from the clock as it is now (the walk has moved it on since the start) */
        const int64_t now = preview_now_ms();
        const int64_t midnight = now - now % (24LL * 3600LL * 1000LL);
        host_panel_device_since("scene.good_night", midnight + 30000LL);
        run(1200);
        expect(find_label(device_tile_of("Good Night"), "Ran at 12:00 AM") != NULL,
               "HCTL-006 (BL-179): a scene that ran just after midnight doesn't say \"Ran at 12:00 AM\"");
        host_panel_device_since("scene.good_night", midnight - 30000LL);
        run(1200);
        expect(find_label(device_tile_of("Good Night"), "Ran on Sep 18") != NULL,
               "HCTL-006 (BL-179): a scene that ran just before midnight doesn't say \"Ran on Sep 18\"");
        /* ahead of the clock: up to a minute reads as now; 61 s (measured when the tile is drawn,
         * within this run's 1.2 s) reads no time */
        host_panel_device_since("scene.good_night", preview_now_ms() + 59000LL);
        run(1200);
        night = device_tile_of("Good Night");
        expect(night != NULL && find_label_with(night, "Ran at ") != NULL && find_label(night, "Scene") == NULL,
               "HCTL-006 (BL-179): a time just inside a minute ahead doesn't read as now");
        host_panel_device_since("scene.good_night", preview_now_ms() + 61000LL + 1200LL);
        run(1200);
        night = device_tile_of("Good Night");
        expect(night != NULL && find_label(night, "Scene") != NULL && find_label_with(night, "Ran ") == NULL,
               "HCTL-006 (BL-179): a time more than a minute ahead shows a time, or not \"Scene\"");
        host_panel_device_since("scene.good_night", ran);
        run(1200);
        expect(find_label(by_name("device_grid"), "Ran at 8:30 PM") != NULL,
               "HCTL-006 (BL-179): Good Night's time didn't come back");
    }
    /* HCTL-006, LINK-046 (BL-216, proposal P1's bl-216-kp-devices-lost): no device can be reached
     * (Home Assistant's link dropped; on a link keypad, its alarm panel's device data stopped):
     * every tile offline since then; no note over the page (BL-220 R7) */
    {
        lv_obj_t *tabs = by_name("device_areas");
        const int32_t widest = lv_obj_get_style_max_width(tabs, LV_PART_MAIN);
        host_panel_devices_unreachable(true, host_panel_device("switch.pool_pump")->since_ms);
        run(1200);
        expect(no_title(), "BL-220 R7: nothing reachable put a note over the page");
        expect(find_label(device_tile_of("Front Door"), "Offline since 9:12 PM") != NULL &&
                   find_label(device_tile_of("Hallway"), "Offline since 9:12 PM") != NULL,
               "HCTL-006 (BL-216): with nothing reachable, a tile isn't offline since 9:12 PM");
        /* no shot here: the link keypad's bl-216-kp-devices-lost is this page in this state */
        host_panel_devices_unreachable(false, 0);
        run(1200);
        expect(tile_says("Back Door", "Unlocked", true) &&
                   lv_obj_get_style_max_width(tabs, LV_PART_MAIN) == widest,
               "UI-020 (BL-216): the tiles or the tabs didn't come back once the devices did");
    }

    /* BL-219 (proposal BL-219, approved 2026-10-19): a change to a light, a switch, a fan, a
     * thermostat or a scene shows at once, as Home Assistant's own screens do, and the device's next
     * report wins; a lock still says it is under way. A slow source takes each call and reports
     * only when the walk says. Everything is put back as it was for the shots that follow. */
    {
        host_panel_devices_slow(true);
        /* a tap: on at once, before any report; no "Turning on…", the chip at full strength, the
         * note counting it */
        tap_device("Kitchen Lights");
        run(1300);
        lv_obj_t *k = device_tile_of("Kitchen Lights");
        expect(host_panel_device("light.kitchen")->state == UI_DEV_ST_OFF &&
                   host_panel_device("light.kitchen")->status == UI_DEV_PENDING,
               "BL-219: the slow source reported the tap already");
        expect(k != NULL && find_label(k, "On") != NULL && find_label_with(k, "Turning") == NULL &&
                   lv_obj_get_style_opa(lv_obj_find_by_name(k, "chip"), LV_PART_MAIN) == LV_OPA_COVER,
               "BL-219: a tapped light doesn't read On at once, or reads as under way");
        shot("bl-219-devices-at-once");
        /* the device does it and reports: the tile follows the report */
        host_panel_device_answer(true);
        run(1300);
        expect(find_label(device_tile_of("Kitchen Lights"), "On · 100%") != NULL,
               "BL-219: the report didn't show once it came");
        /* the device is done with a change but its report stays as it was (it ignored it): the
         * tile shows what was asked, then the report again 2 s after the answer */
        tap_device("Kitchen Lights");
        run(300);
        expect(find_label(device_tile_of("Kitchen Lights"), "Off") != NULL,
               "BL-219: Off didn't show at once");
        host_panel_device_answer(false);
        run(1300);
        expect(find_label(device_tile_of("Kitchen Lights"), "Off") != NULL,
               "BL-219: the tile went back to the report sooner than 2 s after the answer");
        run(3000);
        expect(find_label(device_tile_of("Kitchen Lights"), "On · 100%") != NULL,
               "BL-219: with no new report, the tile didn't go back to the last one");
        /* refused: back to the report, and the tile says so (HCTL-012) */
        tap_device("Kitchen Lights");
        run(300);
        host_panel_device_failed("light.kitchen", ACT_OFF);
        run(1300);
        k = device_tile_of("Kitchen Lights");
        expect(k != NULL && find_label(k, "Didn't turn off") != NULL,
               "BL-219, HCTL-012: a refused change didn't say so");
        host_panel_device_settle("light.kitchen");
        run(1300);
        /* the newest wish: a touch while the last change is on its way shows at once and goes out
         * once the device has answered that one; never refused as busy */
        tap_device("Kitchen Lights");
        run(300);
        tap_device("Kitchen Lights");
        run(300);
        expect(find_label(device_tile_of("Kitchen Lights"), "On · 100%") != NULL &&
                   host_panel_device("light.kitchen")->action == ACT_OFF,
               "BL-219: a second tap while the first was on its way didn't show at once");
        host_panel_device_answer(true);
        run(1300);
        expect(host_panel_device("light.kitchen")->status == UI_DEV_PENDING &&
                   host_panel_device("light.kitchen")->action == ACT_ON,
               "BL-219: the newest wish didn't go out once the last change was answered");
        expect(find_label(device_tile_of("Kitchen Lights"), "On · 100%") != NULL,
               "BL-219: the tile showed the last change's report over the newest wish");
        host_panel_device_answer(true);
        run(1300);
        expect(host_panel_device("light.kitchen")->state == UI_DEV_ST_ON &&
                   find_label(device_tile_of("Kitchen Lights"), "On · 100%") != NULL,
               "BL-219: the newest wish wasn't done");
        /* a lock says it is under way until it reports: never "Locked" before it is */
        tap_device("Back Door");
        run(1300);
        lv_obj_t *b = device_tile_of("Back Door");
        expect(b != NULL && find_label(b, "Locking…") != NULL &&
                   host_panel_device("lock.back_door")->state == UI_DEV_ST_UNLOCKED,
               "BL-219: a lock didn't say \"Locking…\" until it reported");
        shot("bl-219-lock-under-way");
        host_panel_device_answer(true);
        run(1300);
        expect(find_label(device_tile_of("Back Door"), "Locked") != NULL,
               "BL-219: the lock's report didn't show");
        /* a thermostat's + shows its new set point at once */
        lv_obj_t *hall = device_tile_of("Hallway");
        lv_obj_t *plus = hall != NULL ? lv_obj_find_by_name(hall, "plus") : NULL;
        expect(plus != NULL, "BL-219: Hallway's tile has no +");
        if (plus != NULL) {
            tap(plus);
            run(300);
            expect(find_label(device_tile_of("Hallway"), "73°") != NULL &&
                       host_panel_device("climate.hallway")->setpoint == 7200,
                   "BL-219: the set point didn't show at once");
            host_panel_device_answer(false); /* it ignored it: 72° again */
            run(4300);
            expect(find_label(device_tile_of("Hallway"), "72°") != NULL,
                   "BL-219: the set point didn't go back to the report");
        }
        /* the sheet's slider stays where the finger lifted, and so does the tile's */
        hold_device("Porch Light");
        tap_sheet_slider(40);
        {
            lv_obj_t *sheet = device_sheet_open();
            lv_obj_t *slider = sheet != NULL ? lv_obj_find_by_name(sheet, "slider") : NULL;
            const int32_t v = slider != NULL ? lv_slider_get_value(slider) : -1;
            char want[32];
            snprintf(want, sizeof(want), "On · %d%%", (int)v);
            expect(v >= 30 && v <= 50 && host_panel_device("light.porch")->level == 80 &&
                       strcmp(lv_subject_get_string(&subject_sheet_note), want) == 0 &&
                       find_label(device_tile_of("Porch Light"), want) != NULL,
                   "BL-219: the sheet's slider or the words didn't stay where the finger lifted");
            shot("bl-219-sheet-at-once");
        }
        host_panel_device_answer(false); /* it ignored it: 80 % again */
        run(4300);
        expect(find_label(device_tile_of("Porch Light"), "On · 80%") != NULL,
               "BL-219: the brightness didn't go back to the report");
        close_device_sheet();
        host_panel_devices_slow(false);
        host_panel_device_state("light.kitchen", UI_DEV_ST_OFF);
        host_panel_device_state("lock.back_door", UI_DEV_ST_UNLOCKED);
        run(1300);
        expect(tile_says("Back Door", "Unlocked", true) && find_label(device_tile_of("Kitchen Lights"), "Off") != NULL,
               "BL-219: Smart Home wasn't put back");
    }

    /* HCTL-004: a tap switches a light; B030: an off light's tap turns it on (the simulator, as
     * Home Assistant, takes an "on" with brightness 0 as off) */
    tap_device("Kitchen Lights");
    run(300);
    expect(host_panel_device("light.kitchen")->state == UI_DEV_ST_ON &&
               host_panel_device("light.kitchen")->level > 0,
           "HCTL-004, B030: a tap on Kitchen Lights didn't turn it on");

    /* HCTL-005: unlocking asks for a code on the pad over the screen; a wrong one doesn't unlock */
    tap_device("Front Door");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "HCTL-005: unlocking didn't ask for a code");
    expect(host_panel_device("lock.front_door")->state == UI_DEV_ST_LOCKED,
           "HCTL-005: Front Door unlocked without a code");
    shot("device_unlock");
    type("9999k");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1 &&
               host_panel_device("lock.front_door")->state == UI_DEV_ST_LOCKED,
           "HCTL-005: a wrong code unlocked Front Door");
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               host_panel_device("lock.front_door")->state == UI_DEV_ST_UNLOCKED,
           "HCTL-005: the code didn't unlock Front Door");
    /* locking never asks */
    tap_device("Front Door");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               host_panel_device("lock.front_door")->state == UI_DEV_ST_LOCKED,
           "HCTL-005: locking asked for a code, or didn't lock");

    /* HCTL-004: a long press opens a light's sheet; its brightness applies when the finger lifts
     * and the tile follows */
    hold_device("Porch Light");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the light's sheet");
    expect(find_label(device_sheet_open(), "Brightness") != NULL,
           "HCTL-004: the light's sheet has no brightness");
    shot("device_light");
    tap_sheet_slider(30);
    {
        const int level = host_panel_device("light.porch")->level;
        expect(level >= 20 && level <= 40, "HCTL-004: the sheet's slider didn't set the brightness");
        char want[32];
        snprintf(want, sizeof(want), "On · %d%%", level);
        expect(find_label(by_name("device_grid"), want) != NULL,
               "HCTL-004: the tile didn't follow the sheet's brightness");
        /* HCTL-004, B030: the sheet's switch turns the light off, and on again at its last
         * brightness (an "on" that carried brightness 0 left it off) */
        lv_obj_t *sw = lv_obj_find_by_name(device_sheet_open(), "sheet_switch");
        expect(sw != NULL, "HCTL-004: the light's sheet has no switch");
        if (sw != NULL) {
            tap(sw);
            run(300);
            expect(host_panel_device("light.porch")->state == UI_DEV_ST_OFF,
                   "HCTL-004: the sheet's switch didn't turn the light off");
            tap(sw);
            run(300);
            expect(host_panel_device("light.porch")->state == UI_DEV_ST_ON &&
                       host_panel_device("light.porch")->level == level,
                   "HCTL-004, B030: the sheet's switch didn't turn the light on at its last brightness");
        }
    }
    close_device_sheet();

    /* HCTL-004: a thermostat's sheet: the set point and the mode */
    hold_device("Hallway");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the thermostat's sheet");
    shot("device_climate");
    /* HCTL-004, UI-023 (BL-171): its words say what its source says it is doing, with the room's
     * temperature; the set point's label and the lit key keep saying the mode. Where the source
     * doesn't say, the mode's own label - never "Heating" guessed from the mode. */
    expect(climate_words("Hallway", "Heating · 71° now"),
           "HCTL-004 (BL-171): a heating thermostat doesn't read \"Heating · 71° now\"");
    host_panel_device_climate("climate.hallway", DEV_HVAC_IDLE, 7100);
    run(1200);
    expect(climate_words("Hallway", "Idle · 71° now"),
           "HCTL-004 (BL-171): an idle thermostat doesn't read \"Idle · 71° now\" on its tile and sheet");
    expect(find_label(device_sheet_open(), "Heat to") != NULL && sheet_key_lit("Heat") && !sheet_key_lit("Cool"),
           "HCTL-004 (BL-171): an idle thermostat's sheet stopped saying its mode (Heat to, the Heat key)");
    shot("device_climate_idle");
    host_panel_device_climate("climate.hallway", DEV_HVAC_PREHEATING, 7100);
    run(1200);
    expect(climate_words("Hallway", "Preheating · 71° now"),
           "HCTL-004 (BL-171): a preheating thermostat doesn't read \"Preheating · 71° now\"");
    /* HCTL-004 (BL-171, 2026-10-07): a reported rest reads Idle while the reported
     * mode can resume and Off only in mode Off, whichever of off or idle the source calls it (some
     * thermostat integrations say off whenever the system rests, others idle in mode Off).
     * Presentation only: the reported action stays as it came. */
    host_panel_device_climate("climate.hallway", DEV_HVAC_OFF, 7100);
    run(1200);
    expect(climate_says("Hallway", "Idle · 71° now") && sheet_key_lit("Heat") &&
               host_panel_device("climate.hallway")->hvac_action == DEV_HVAC_OFF,
           "HCTL-004 (BL-171): a thermostat at rest in Heat that its source says is off doesn't read "
           "\"Idle · 71° now\" with Heat lit on its tile and sheet");
    shot("device_climate_rest_heat");
    host_panel_device_state("climate.hallway", UI_DEV_ST_OFF);
    host_panel_device_climate("climate.hallway", DEV_HVAC_IDLE, 7100);
    run(1200);
    expect(climate_says("Hallway", "Off · 71° now") && sheet_key_lit("Off") && !sheet_key_lit("Heat"),
           "HCTL-004 (BL-171): a thermostat in mode Off that its source says is idle doesn't read "
           "\"Off · 71° now\" with Off lit on its tile and sheet");
    shot("device_climate_rest_off");
    if (!s_locale) {
        /* the whole matrix (HCTL-004, docs/features/24): off and idle across every mode, with and
         * without the temperature; Heating, Cooling and Preheating as reported, even in mode Off;
         * no action or one without words: the mode's word; an unknown mode keeps what came */
        static const struct {
            uint8_t mode;
            uint8_t action;
            int32_t temp;
            const char *words;
        } CASES[] = {
            {UI_DEV_ST_HEAT, DEV_HVAC_OFF, 7100, "Idle · 71° now"},
            {UI_DEV_ST_HEAT, DEV_HVAC_IDLE, 7100, "Idle · 71° now"},
            {UI_DEV_ST_COOL, DEV_HVAC_OFF, 7100, "Idle · 71° now"},
            {UI_DEV_ST_COOL, DEV_HVAC_IDLE, 7100, "Idle · 71° now"},
            {UI_DEV_ST_AUTO, DEV_HVAC_OFF, 7100, "Idle · 71° now"},
            {UI_DEV_ST_AUTO, DEV_HVAC_IDLE, 7100, "Idle · 71° now"},
            {UI_DEV_ST_ON, DEV_HVAC_OFF, 7100, "Idle · 71° now"},
            {UI_DEV_ST_ON, DEV_HVAC_IDLE, 7100, "Idle · 71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_OFF, 7100, "Off · 71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_IDLE, 7100, "Off · 71° now"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_OFF, 7100, "Off · 71° now"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_IDLE, 7100, "Idle · 71° now"},
            {UI_DEV_ST_HEAT, DEV_HVAC_OFF, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_HEAT, DEV_HVAC_IDLE, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_COOL, DEV_HVAC_OFF, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_COOL, DEV_HVAC_IDLE, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_AUTO, DEV_HVAC_OFF, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_AUTO, DEV_HVAC_IDLE, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_ON, DEV_HVAC_OFF, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_ON, DEV_HVAC_IDLE, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_OFF, DEV_HVAC_OFF, UI_DEV_TEMP_UNKNOWN, "Off"},
            {UI_DEV_ST_OFF, DEV_HVAC_IDLE, UI_DEV_TEMP_UNKNOWN, "Off"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_OFF, UI_DEV_TEMP_UNKNOWN, "Off"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_IDLE, UI_DEV_TEMP_UNKNOWN, "Idle"},
            {UI_DEV_ST_OFF, DEV_HVAC_HEATING, 7100, "Heating · 71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_COOLING, 7100, "Cooling · 71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_PREHEATING, 7100, "Preheating · 71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_HEATING, UI_DEV_TEMP_UNKNOWN, "Heating"},
            {UI_DEV_ST_AUTO, DEV_HVAC_COOLING, 7100, "Cooling · 71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_UNKNOWN, 7100, "Off · 71° now"},
            {UI_DEV_ST_COOL, DEV_HVAC_UNKNOWN, 7100, "Cool · 71° now"},
            {UI_DEV_ST_ON, DEV_HVAC_UNKNOWN, 7100, "On · 71° now"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_UNKNOWN, 7100, "71° now"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_UNKNOWN, UI_DEV_TEMP_UNKNOWN, ""},
            {UI_DEV_ST_OFF, DEV_HVAC_FAN, 7100, "Off · 71° now"},
            {UI_DEV_ST_HEAT, DEV_HVAC_DRYING, 7100, "Heat · 71° now"},
            {UI_DEV_ST_AUTO, DEV_HVAC_DEFROSTING, 7100, "Auto · 71° now"},
            {UI_DEV_ST_UNKNOWN, DEV_HVAC_FAN, 7100, "71° now"},
            {UI_DEV_ST_OFF, DEV_HVAC_FAN, UI_DEV_TEMP_UNKNOWN, "Off"},
        };
        for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            host_panel_device_state("climate.hallway", CASES[i].mode);
            host_panel_device_climate("climate.hallway", CASES[i].action, CASES[i].temp);
            run(1200);
            const ui_dev_t *h = host_panel_device("climate.hallway");
            static char what[200];
            snprintf(what, sizeof(what),
                     "HCTL-004 (BL-171): a thermostat in mode %d reporting action %d (temperature %s) "
                     "doesn't read \"%s\" on its tile and sheet alike, or its reported fields changed",
                     (int)CASES[i].mode, (int)CASES[i].action,
                     CASES[i].temp == UI_DEV_TEMP_UNKNOWN ? "unknown" : "71°", CASES[i].words);
            expect(device_sheet_open() != NULL && climate_says("Hallway", CASES[i].words) &&
                       h->state == CASES[i].mode && h->hvac_action == CASES[i].action,
                   what);
        }
        /* failed comes first; the mode asked for isn't the mode: settled with the reported mode
         * still Off, a reported idle reads Off, and Idle once Heat is reported. A mode change
         * someone else started (an automation) shows the report while it is under way: no
         * "Changing…" (HCTL-006, BL-219) */
        host_panel_device_state("climate.hallway", UI_DEV_ST_OFF);
        host_panel_device_climate("climate.hallway", DEV_HVAC_IDLE, 7100);
        host_panel_device_pending("climate.hallway", ACT_SET_MODE);
        run(1200);
        expect(climate_says("Hallway", "Off · 71° now"),
               "HCTL-006 (BL-219): a thermostat whose mode someone else is changing doesn't show its "
               "report (\"Off · 71° now\"), or reads \"Changing…\"");
        host_panel_device_settle("climate.hallway");
        run(1200);
        expect(climate_says("Hallway", "Off · 71° now"),
               "HCTL-004 (BL-171): a settled mode change read the mode asked for, not the mode reported (Off)");
        host_panel_device_failed("climate.hallway", ACT_SET_MODE);
        run(1200);
        expect(climate_says("Hallway", "Didn't change"),
               "HCTL-006 (BL-171): a thermostat whose mode failed doesn't read \"Didn't change\" before its rest");
        host_panel_device_settle("climate.hallway");
        host_panel_device_state("climate.hallway", UI_DEV_ST_HEAT);
        run(1200);
        expect(climate_says("Hallway", "Idle · 71° now"),
               "HCTL-004 (BL-171): a resting thermostat whose reported mode became Heat doesn't read Idle");
        /* UI-020, UI-023: polls with nothing new leave the words, their label and its place still */
        host_panel_device_climate("climate.hallway", DEV_HVAC_OFF, 7100);
        run(1200);
        lv_obj_t *tile = device_tile_of("Hallway");
        lv_obj_t *l = tile != NULL ? lv_obj_find_by_name(tile, "detail_label") : NULL;
        lv_area_t was = {0};
        if (l != NULL) {
            lv_obj_get_coords(l, &was);
        }
        bool still = l != NULL && climate_says("Hallway", "Idle · 71° now");
        for (int i = 0; i < 4 && still; i++) {
            run(1200);
            lv_area_t now = {0};
            lv_obj_t *t2 = device_tile_of("Hallway");
            lv_obj_t *l2 = t2 != NULL ? lv_obj_find_by_name(t2, "detail_label") : NULL;
            if (l2 != NULL) {
                lv_obj_get_coords(l2, &now);
            }
            still = l2 == l && memcmp(&now, &was, sizeof(now)) == 0 && climate_says("Hallway", "Idle · 71° now");
        }
        expect(still, "UI-023 (BL-171): a resting thermostat's words or their label moved with nothing new");
    }
    host_panel_device_state("climate.hallway", UI_DEV_ST_HEAT);
    host_panel_device_climate("climate.hallway", DEV_HVAC_UNKNOWN, 7100);
    run(1200);
    expect(climate_words("Hallway", "Heat · 71° now") && find_label_with(by_name("device_grid"), "Heating") == NULL,
           "HCTL-004 (BL-171): without what it is doing, a thermostat doesn't read its mode (\"Heat · 71° now\")");
    host_panel_device_climate("climate.hallway", DEV_HVAC_UNKNOWN, UI_DEV_TEMP_UNKNOWN);
    run(1200);
    expect(climate_words("Hallway", "Heat"),
           "HCTL-004 (BL-171): without a temperature, a thermostat doesn't read its mode alone (\"Heat\")");
    host_panel_device_climate("climate.hallway", DEV_HVAC_HEATING, 7100);
    run(1200);
    expect(climate_words("Hallway", "Heating · 71° now"),
           "HCTL-004 (BL-171): the thermostat's words didn't come back to \"Heating · 71° now\"");
    tap(lv_obj_find_by_name(device_sheet_open(), "plus"));
    run(1300);
    expect(host_panel_device("climate.hallway")->setpoint == 7300,
           "HCTL-004: the sheet's plus didn't raise the set point");
    tap_sheet_label("Cool");
    expect(host_panel_device("climate.hallway")->state == UI_DEV_ST_COOL,
           "HCTL-004: the sheet's Cool didn't set the mode");
    /* BL-171: the mode changed and what it then does isn't known: the mode's label */
    expect(find_label(by_name("device_grid"), "Cool · 71° now") != NULL,
           "HCTL-004: the tile didn't follow the sheet's mode");
    close_device_sheet();
    /* HCTL-004, HCTL-012 (BL-171): a failed mode isn't retried by a tap on the tile: the mode
     * chosen isn't kept, and sent without one the panel refuses it ("That can't be done right
     * now"). The tap sends nothing and opens its sheet, where the mode is set again. */
    host_panel_device_failed("climate.hallway", ACT_SET_MODE);
    run(1200);
    tap_device("Hallway");
    run(300);
    expect(host_panel_device("climate.hallway")->status == UI_DEV_FAILED && device_sheet_open() != NULL &&
               strcmp(lv_subject_get_string(&subject_toast), "That can't be done right now") != 0,
           "HCTL-012: a tap on a thermostat whose mode failed sent a mode without one, or opened no sheet");
    if (device_sheet_open() == NULL) {
        hold_device("Hallway");
    }
    tap_sheet_label("Cool");
    expect(host_panel_device("climate.hallway")->status == UI_DEV_IDLE &&
               host_panel_device("climate.hallway")->state == UI_DEV_ST_COOL,
           "HCTL-012: the sheet didn't set the failed mode again");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }

    /* HCTL-004, HCTL-005: a lock's sheet; Unlock asks for the code over the sheet */
    hold_device("Front Door");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the lock's sheet");
    tap_sheet_label("Unlock");
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "HCTL-005: the sheet's Unlock didn't ask for a code");
    type("123456k");
    run(300);
    expect(host_panel_device("lock.front_door")->state == UI_DEV_ST_UNLOCKED && device_sheet_open() != NULL,
           "HCTL-005: the code didn't unlock from the sheet");
    tap_sheet_label("Lock");
    expect(host_panel_device("lock.front_door")->state == UI_DEV_ST_LOCKED,
           "HCTL-004: the sheet's Lock didn't lock");
    close_device_sheet();

    /* HCTL-006, HCTL-012 (BL-171): the tile says "Didn't run" where the source refused it, and
     * "Not confirmed" where no answer came (or the link dropped with it in flight), in amber either
     * way; no note over the page (BL-220 R7) */
    tap_area("Living room");
    run(1200);
    expect(no_title(), "BL-220 R7: the Living room has a note over it");
    {
        const int64_t ran = host_panel_device("scene.good_night")->since_ms; /* the 8:30 PM sample */
        host_panel_device_failed("scene.good_night", ACT_RUN);
        run(1200);
        expect(tile_says("Good Night", "Didn't run", true),
               "HCTL-006: a refused scene's tile doesn't say \"Didn't run\" in amber");
        shot("devices_failed_note");
        host_panel_device_unconfirmed("scene.good_night", ACT_RUN);
        run(1200);
        expect(tile_says("Good Night", "Not confirmed", true) &&
                   find_label(device_tile_of("Good Night"), "Didn't run") == NULL,
               "HCTL-012 (BL-171): an unconfirmed scene's tile doesn't say \"Not confirmed\" in amber, or says \"Didn't run\"");
        shot("devices_unconfirmed_note");
        /* HCTL-012: a tap tries it again */
        tap_device("Good Night");
        run(1200);
        expect(host_panel_device("scene.good_night")->status == UI_DEV_IDLE &&
                   find_label(device_tile_of("Good Night"), "Not confirmed") == NULL,
               "HCTL-012: a tap on an unconfirmed scene didn't run it again, or its tile kept the failure");
        host_panel_device_since("scene.good_night", ran);
        run(1200);
        expect(find_label(by_name("device_grid"), "Ran at 8:30 PM") != NULL,
               "HCTL-006: Good Night's 8:30 PM didn't come back after the retry");
    }
    tabs_take_the_row();
    /* HCTL-004, HCTL-012 (BL-171): a tap on a failed change of a value opens the device's sheet and
     * sends nothing: the value that failed isn't kept, and a retry would send the one it has now */
    {
        host_panel_device_failed("light.living_lamp", ACT_SET_LEVEL);
        run(1200);
        tap_device("Living Room Lamp");
        run(300);
        const ui_dev_t *lamp = host_panel_device("light.living_lamp");
        expect(device_sheet_open() != NULL && lamp->level == 40 && lamp->state == UI_DEV_ST_ON &&
                   lamp->status == UI_DEV_FAILED,
               "HCTL-012 (BL-171): a tap on a failed brightness sent something, or opened no sheet");
        if (device_sheet_open() != NULL) {
            close_device_sheet();
        }
        host_panel_device_settle("light.living_lamp");
        const int32_t set = host_panel_device("climate.hallway")->setpoint;
        host_panel_device_failed("climate.hallway", ACT_SET_TEMP);
        run(1200);
        tap_device("Hallway");
        run(300);
        expect(device_sheet_open() != NULL && host_panel_device("climate.hallway")->setpoint == set &&
                   host_panel_device("climate.hallway")->status == UI_DEV_FAILED,
               "HCTL-012 (BL-171): a tap on a failed set point sent something, or opened no sheet");
        if (device_sheet_open() != NULL) {
            close_device_sheet();
        }
        host_panel_device_settle("climate.hallway");
        run(1200);
        expect(find_label(device_tile_of("Hallway"), "Didn't change") == NULL,
               "BL-171: the Hallway's tile didn't come back");
    }

    /* HCTL-006: under way, failed and offline, in words */
    host_panel_devices_garage_closing();
    tap_area("Outside");
    run(1200);
    expect(find_label(device_tile_of("Side Gate"), "Jammed") != NULL &&
               find_label_with(device_tile_of("Pool Pump"), "Offline since") != NULL,
           "HCTL-006: Outside's tiles don't show the jam and the offline pump");
    expect(find_label(by_name("device_grid"), "Didn't turn on") != NULL,
           "HCTL-006: Patio Lights doesn't say its last action failed");
    shot("devices_outside");
    /* HCTL-006 (BL-179): offline since this evening reads its time; since an earlier day, its date;
     * since an earlier year, the date and year */
    expect(find_label(by_name("device_grid"), "Offline since 9:12 PM") != NULL,
           "HCTL-006 (BL-179): Pool Pump doesn't say it is offline since 9:12 PM");
    {
        const int64_t down = host_panel_device_since("switch.pool_pump", 0);
        host_panel_device_since("switch.pool_pump", down - 24LL * 3600LL * 1000LL);
        run(1200);
        lv_obj_t *pump = device_tile_of("Pool Pump");
        expect(pump != NULL && find_label(pump, "Offline since Sep 18") != NULL,
               "HCTL-006 (BL-179): a device offline since the day before doesn't say \"Offline since Sep 18\"");
        shot("devices_offline_day");
        host_panel_device_since("switch.pool_pump", down - 366LL * 24LL * 3600LL * 1000LL);
        run(1200);
        /* about 188 px in a small tile's 176: the tile ends it with "...", the sheet shows it whole
         * (BL-184) */
        expect(find_label_shortened(device_tile_of("Pool Pump"), "Offline since Sep 18, 2025") != NULL,
               "HCTL-006 (BL-179, BL-184): a small tile offline since a year before doesn't end \"Offline since Sep 18, 2025\" with ...");
        hold_device("Pool Pump");
        expect(device_sheet_open() != NULL &&
                   find_label(device_sheet_open(), "Offline since Sep 18, 2025") != NULL,
               "HCTL-006 (BL-179): a device offline since a year before doesn't say \"Offline since Sep 18, 2025\"");
        if (device_sheet_open() != NULL) {
            close_device_sheet();
        }
        host_panel_device_since("switch.pool_pump", down);
        run(1200);
        expect(find_label(by_name("device_grid"), "Offline since 9:12 PM") != NULL,
               "HCTL-006 (BL-179): Pool Pump's time didn't come back");
    }
    /* HCTL-012: nothing retries by itself; a tap on the failed tile tries again (B030: its "on"
     * is a plain on, not brightness 0) */
    tap_device("Patio Lights");
    run(300);
    expect(host_panel_device("light.patio")->state == UI_DEV_ST_ON &&
               host_panel_device("light.patio")->status == UI_DEV_IDLE,
           "HCTL-012, B030: a tap on a failed tile didn't try again, or didn't turn the light on");
    tap_device("Pool Pump"); /* offline: nothing happens */
    run(300);
    expect(host_panel_device("switch.pool_pump")->state == UI_DEV_ST_OFF,
           "HCTL-006: an offline tile took a tap");
    offline_stays_readable(); /* BL-181 */

    /* HCTL-004: a cover's sheet (position and keys) and a fan's (its speeds) */
    hold_device("Patio Awning");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the cover's sheet");
    shot("device_cover");
    tap_sheet_label("Close");
    expect(host_panel_device("cover.patio_awning")->state == UI_DEV_ST_CLOSED,
           "HCTL-004: the sheet's Close didn't close the cover");
    close_device_sheet();
    /* HCTL-004 (BL-179): a cover that can stop, or whose source doesn't say, still stops on a tap
     * while it moves: no code, stopped part-way. A guard: the turning back is for covers that can't */
    host_panel_device_state("cover.patio_awning", UI_DEV_ST_CLOSING);
    run(1200);
    tap_device("Patio Awning");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               host_panel_device("cover.patio_awning")->state == UI_DEV_ST_OPEN,
           "HCTL-004 (BL-179): a tap on a closing cover that can stop didn't stop it, or asked for a code");
    host_panel_device_state("cover.patio_awning", UI_DEV_ST_CLOSED);
    run(1200);
    hold_device("Porch Fan");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the fan's sheet");
    shot("device_fan");
    tap_sheet_label("High");
    expect(host_panel_device("fan.porch")->level == 100, "HCTL-004: the sheet's High didn't set the speed");
    expect(find_label(by_name("device_grid"), "On · High") != NULL,
           "HCTL-004: the tile didn't follow the sheet's speed");
    close_device_sheet();
    /* HCTL-004 (BL-171): a tile's key sends its own action and value */
    tap_tile_key("Porch Fan", "Medium");
    expect(host_panel_device("fan.porch")->state == UI_DEV_ST_ON && host_panel_device("fan.porch")->level == 67,
           "HCTL-004: the fan tile's Medium didn't set the speed to 67");

    /* HCTL-002, HCTL-004 (BL-171): what a device can't do, as its source says. A fan without
     * speeds is a switch, blinds that can't stop have Open and Close, a thermostat holding a range
     * has no - and +. */
    host_panel_devices_limits(true);
    tap_area("Living room");
    run(1200);
    shot("devices_limits");
    /* HCTL-004, UI-023 (BL-171): a thermostat holding a range reads its mode where its source
     * doesn't say what it is doing, and what it is doing where it does */
    expect(find_label(device_tile_of("Upstairs"), "Auto · 70° now") != NULL,
           "HCTL-004 (BL-171): Upstairs (Auto, no action known) doesn't read \"Auto · 70° now\"");
    host_panel_device_climate("climate.upstairs", DEV_HVAC_HEATING, 7000);
    run(1200);
    expect(find_label(device_tile_of("Upstairs"), "Heating · 70° now") != NULL,
           "HCTL-004 (BL-171): Upstairs, heating, doesn't read \"Heating · 70° now\"");
    host_panel_device_climate("climate.upstairs", DEV_HVAC_UNKNOWN, 7000);
    run(1200);
    {
        lv_obj_t *grid = by_name("device_grid");
        lv_obj_t *blinds = device_tile_of("Blinds");
        lv_obj_t *fan = device_tile_of("Ceiling Fan");
        lv_obj_t *upstairs = device_tile_of("Upstairs");
        expect(blinds != NULL && device_is_wide("Blinds") && find_label(blinds, "Open") != NULL &&
                   find_label(blinds, "Close") != NULL && find_label(blinds, "Stop") == NULL,
               "HCTL-004: blinds that can't stop show a Stop key, or lost Open and Close");
        expect(fan != NULL && !device_is_wide("Ceiling Fan") && find_label(grid, "Low") == NULL &&
                   find_visible(fan, "more") == NULL,
               "HCTL-004: a fan without speeds shows speed keys, an arrow, or two cells");
        expect(upstairs != NULL && find_visible(upstairs, "plus") == NULL &&
                   find_visible(upstairs, "minus") == NULL,
               "HCTL-004: a thermostat holding a range shows - and + on its tile");
        expect(find_label(grid, "Stop") == NULL, "HCTL-004: a Stop key on a page with no cover that stops");
    }
    /* HCTL-004, HCTL-005 (BL-179): a tap on blinds that move and can't stop turns them back, as
     * Home Assistant's cover toggle does: opening, they close (no code); closing, they open, and
     * opening asks for the code as it does anywhere (the simulator applies an action at once) */
    tap_device("Blinds");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 && device_sheet_open() == NULL &&
               host_panel_device("cover.blinds")->state == UI_DEV_ST_CLOSED,
           "HCTL-004 (BL-179): a tap on opening blinds that can't stop didn't close them, or asked for a code");
    host_panel_device_state("cover.blinds", UI_DEV_ST_CLOSING);
    run(1200);
    tap_device("Blinds");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1 &&
               strcmp(lv_subject_get_string(&subject_pad_title), "Open Blinds") == 0 &&
               host_panel_device("cover.blinds")->state == UI_DEV_ST_CLOSING,
           "HCTL-005 (BL-179): a tap on closing blinds that can't stop didn't ask for the code to open them");
    tap_name("pad_close"); /* the X sends nothing */
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               host_panel_device("cover.blinds")->state == UI_DEV_ST_CLOSING,
           "HCTL-005 (BL-179): closing the code pad sent something to the blinds");
    tap_device("Blinds");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1,
           "HCTL-005 (BL-179): a second tap on closing blinds didn't ask for the code");
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               host_panel_device("cover.blinds")->state == UI_DEV_ST_OPEN,
           "HCTL-004 (BL-179): the code didn't turn the closing blinds back open");
    /* their sheet: Open and Close, no Stop, and a position */
    hold_device("Blinds");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the blinds' sheet");
    if (device_sheet_open() != NULL) {
        expect(find_label(device_sheet_open(), "Stop") == NULL &&
                   find_label(device_sheet_open(), "Position") != NULL,
               "HCTL-004: the sheet of blinds that can't stop shows Stop, or no position");
        /* HCTL-005 (BL-179 question 3, kept 2026-10-07): a Position target is judged by its effect,
         * not by the way the slider moved. From a reported position, a lower target that isn't 0
         * asks for the code Open asks for (a stale report can hide that it opens them); the pad's
         * X sends nothing; only exactly 0 is fully closed and asks for none. */
        tap_sheet_slider(80);
        expect(lv_subject_get_int(&subject_modal_pad) == 1 &&
                   strcmp(lv_subject_get_string(&subject_pad_title), "Open Blinds") == 0,
               "HCTL-005 (BL-179): a position for open blinds didn't ask for the code to open them");
        type("123456k");
        run(1300);
        const int at = host_panel_device("cover.blinds")->level;
        expect(lv_subject_get_int(&subject_modal_pad) == 0 && at >= 70 && at <= 90 &&
                   host_panel_device("cover.blinds")->state == UI_DEV_ST_OPEN,
               "HCTL-005 (BL-179): the code didn't set the blinds' position");
        if (device_sheet_open() == NULL) {
            hold_device("Blinds");
        }
        tap_sheet_slider(20);
        expect(lv_subject_get_int(&subject_modal_pad) == 1 &&
                   strcmp(lv_subject_get_string(&subject_pad_title), "Open Blinds") == 0 &&
                   host_panel_device("cover.blinds")->level == at,
               "HCTL-005 (BL-179): a positive position below the reported one didn't ask for the code");
        shot("device_cover_position_code");
        tap_name("pad_close"); /* Cancel: nothing is sent */
        run(1300);
        expect(lv_subject_get_int(&subject_modal_pad) == 0 && host_panel_device("cover.blinds")->level == at &&
                   host_panel_device("cover.blinds")->state == UI_DEV_ST_OPEN &&
                   host_panel_device("cover.blinds")->status == UI_DEV_IDLE,
               "HCTL-005 (BL-179): closing the code pad sent the lower position to the blinds");
        if (device_sheet_open() == NULL) {
            hold_device("Blinds");
        }
        tap_sheet_slider(0);
        expect(lv_subject_get_int(&subject_modal_pad) == 0 && host_panel_device("cover.blinds")->level == 0 &&
                   host_panel_device("cover.blinds")->state == UI_DEV_ST_CLOSED,
               "HCTL-005 (BL-179): position 0 asked for a code, or didn't close the blinds");
        if (device_sheet_open() == NULL) {
            hold_device("Blinds");
        }
        tap_sheet_label("Close");
        expect(host_panel_device("cover.blinds")->state == UI_DEV_ST_CLOSED,
               "HCTL-004: the sheet's Close didn't close the blinds");
        close_device_sheet();
    }
    /* the tile's own keys send their own action: Close is the second of two, never a Stop */
    tap_tile_key("Blinds", "Open");
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "HCTL-005: the tile's Open didn't ask for a code");
    type("123456k");
    run(300);
    expect(host_panel_device("cover.blinds")->state == UI_DEV_ST_OPEN,
           "HCTL-004: the tile's Open didn't open the blinds");
    tap_tile_key("Blinds", "Close");
    expect(host_panel_device("cover.blinds")->state == UI_DEV_ST_CLOSED,
           "HCTL-004: the tile's Close on blinds that can't stop didn't close them");
    /* a fan without speeds switches as a switch does, and its sheet is a switch */
    tap_device("Ceiling Fan");
    run(300);
    expect(host_panel_device("fan.ceiling")->state == UI_DEV_ST_OFF,
           "HCTL-004: a tap on a fan without speeds didn't switch it off");
    hold_device("Ceiling Fan");
    expect(device_sheet_open() != NULL, "HCTL-004: a long press didn't open the fan's sheet");
    if (device_sheet_open() != NULL) {
        expect(find_visible(device_sheet_open(), "sheet_switch") != NULL &&
                   find_label(device_sheet_open(), "Low") == NULL &&
                   find_label(device_sheet_open(), "High") == NULL,
               "HCTL-004: the sheet of a fan without speeds isn't a switch");
        close_device_sheet();
    }
    /* HCTL-004, HCTL-012 (BL-171): a failed speed is a failed change of a value: a tap opens the
     * sheet and sends nothing, also where its source now says it has no speeds (the failure is
     * kept, the ability gone): never a retry, never the tap's own on */
    host_panel_device_failed("fan.ceiling", ACT_SET_LEVEL);
    run(1200);
    tap_device("Ceiling Fan");
    run(300);
    expect(device_sheet_open() != NULL && host_panel_device("fan.ceiling")->state == UI_DEV_ST_OFF &&
               host_panel_device("fan.ceiling")->level == -1 &&
               host_panel_device("fan.ceiling")->status == UI_DEV_FAILED,
           "HCTL-012 (BL-171): a tap on a failed speed of a fan without speeds sent something, or opened no sheet");
    if (device_sheet_open() != NULL) {
        close_device_sheet();
    }
    host_panel_device_settle("fan.ceiling");
    host_panel_devices_limits(false);
    run(1200);
    expect(device_tile_of("Ceiling Fan") == NULL && device_tile_of("Blinds") == NULL,
           "Smart Home: the limited devices didn't go away");
    /* HCTL-004: a device whose source doesn't say what it can keeps every control */
    tap_area("Outside");
    run(300);
    {
        lv_obj_t *awning = device_tile_of("Patio Awning");
        lv_obj_t *porch = device_tile_of("Porch Fan");
        expect(awning != NULL && find_label(awning, "Stop") != NULL && porch != NULL &&
                   find_label(porch, "Low") != NULL,
               "HCTL-004: a cover or a fan that doesn't say what it can lost Stop or its speeds");
    }

    tap_area("Living room");
    run(300);
    shot("devices_living");
    tap_area("Kitchen");
    run(300);
    shot("devices_kitchen");
    /* HCTL-004: a thermostat that holds a range (no single set point) has no - and + on its tile,
     * nor on its sheet: they would send nothing */
    host_panel_devices_range_thermostat(true);
    run(1200);
    {
        lv_obj_t *range = device_tile_of("Upstairs");
        expect(range != NULL && device_is_wide("Upstairs") && find_visible(range, "plus") == NULL &&
                   find_visible(range, "minus") == NULL,
               "HCTL-004: a thermostat holding a range shows - and + on its tile");
        tap_area("Living room");
        run(300);
        lv_obj_t *hall_tile = device_tile_of("Hallway");
        expect(hall_tile != NULL && find_visible(hall_tile, "plus") != NULL,
               "HCTL-004: a thermostat with a set point lost its - and + on its tile");
        tap_area("Kitchen");
        run(300);
        hold_device("Upstairs");
        expect(device_sheet_open() != NULL && lv_obj_find_by_name(device_sheet_open(), "plus") == NULL,
               "HCTL-004: a thermostat holding a range shows - and + on its sheet");
        if (device_sheet_open() != NULL) {
            close_device_sheet();
        }
    }
    host_panel_devices_range_thermostat(false);
    run(1200);
    expect(device_tile_of("Upstairs") == NULL, "Smart Home: the range thermostat didn't go away");
    long_names_stay_in_bounds(); /* BL-184 */
    walk_arrange();
    walk_rooms(); /* BL-220 */

    /* HCTL-003: with no device, no Smart Home: the view goes home and the rail drops it */
    host_panel_devices(false);
    run(1200);
    expect(lv_subject_get_int(&subject_view) == 0 && lv_subject_get_int(&subject_devices_avail) == 0,
           "HCTL-003: Smart Home stayed with no device to control");
    host_panel_devices(true);
    run(5200);
    expect(lv_subject_get_int(&subject_devices_avail) == 1, "HCTL-003: Smart Home didn't come back");
}
#endif

static void shot(const char *name)
{
    const double t0 = now_s();
    const unsigned frames0 = s_frames;
    const double tf0 = s_t_frames;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    const double t1 = now_s();
    s_frames = frames0; /* not a wait's frame */
    s_t_frames = tf0;
    {
        report_clipping(lv_screen_active(), name);
        report_clipping(lv_layer_top(), name);
        report_glyphs(lv_screen_active(), name);
        report_glyphs(lv_layer_top(), name);
        expect_bar_fits(name);
        /* I18N-023: a text cut to fit the screen's 127 bytes is words lost too. */
        static unsigned cuts;
        const char *last = NULL;
        const unsigned now = ui_app_text_cuts(&last);
        if (now != cuts) {
            s_clipped += now - cuts;
            printf("  cut %s: %u text(s) longer than 127 bytes, the last \"%.60s\"\n", name,
                   now - cuts, last);
            cuts = now;
        }
    }
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    /* ALARM_PREVIEW_MEM=1: LVGL's heap at every screenshot (in use now, most so far), to find
     * what a screen costs on a board with a fixed heap. */
    if (getenv("ALARM_PREVIEW_MEM") != NULL) {
        lv_mem_monitor_t mem;
        lv_mem_monitor(&mem);
        printf("  heap %s: %u KB in use, at most %u KB\n", name,
               (unsigned)((mem.total_size - mem.free_size) / 1024u),
               (unsigned)(mem.max_used / 1024u));
    }
#endif
    /* UI-037: the brand remains a state indicator on every view, including alarms. */
    lv_obj_t *brand = find_visible(lv_screen_active(), "rail_brand");
    if (find_visible(lv_screen_active(), "rail_logo") != NULL) {
        expect(brand != NULL, "rail logo is missing above Security");
        expect(lv_image_get_src(brand) != NULL, "rail logo image is missing");
        expect(lv_color_eq(lv_obj_get_style_image_recolor(brand, LV_PART_MAIN),
                           lv_subject_get_color(&subject_tone_color)),
               "rail logo lost the current alarm-state colour");
        expect(lv_obj_get_style_image_recolor_opa(brand, LV_PART_MAIN) == LV_OPA_COVER,
               "rail logo is not fully tinted by the state colour");
    }
    const double t2 = now_s();
    static uint8_t rgb[W * H * 3];
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = s_fb[i];
        const uint8_t r = (uint8_t)((c >> 11) & 0x1F);
        const uint8_t g = (uint8_t)((c >> 5) & 0x3F);
        const uint8_t b = (uint8_t)(c & 0x1F);
        rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    unsigned char *png = NULL;
    size_t size = 0;
    if (shot_png_encode(&png, &size, rgb, W, H) != 0) {
        fprintf(stderr, "PNG encoding failed\n");
        exit(1);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%02d_%s.png", s_out, ++s_shots, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "can't write %s\n", path);
        exit(1);
    }
    fwrite(png, 1, size, f);
    fclose(f);
    shot_png_free(png);
    printf("  %s\n", path);
    const double t3 = now_s();
    s_t_draw += t1 - t0;
    s_t_checks += t2 - t1;
    s_t_png += t3 - t2;
}

static void make_dir(const char *path)
{
#if defined(_WIN32)
    mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* ---- BL-132: the appearances (docs/features/07 UI-010) -------------------------------------- */
#if ALARM_LV_BUDGET_KB == 0

/* How many colours on the screens are another palette's: every colour a style or an object holds
 * that is a palette colour must be the current theme's (the screen saver: its saver= theme's). */
static unsigned off_palette(const lv_obj_t *o, const lv_color_t *pal)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR,   LV_STYLE_BG_GRAD_COLOR, LV_STYLE_BORDER_COLOR,
        LV_STYLE_OUTLINE_COLOR, LV_STYLE_SHADOW_COLOR, LV_STYLE_IMAGE_RECOLOR,
        LV_STYLE_LINE_COLOR, LV_STYLE_ARC_COLOR,     LV_STYLE_TEXT_COLOR,
    };
    unsigned bad = 0;
    for (uint32_t i = 0; i < o->style_cnt; i++) {
        for (size_t k = 0; k < sizeof(props) / sizeof(props[0]); k++) {
            lv_style_value_t v;
            if (lv_style_get_prop(o->styles[i].style, props[k], &v) == LV_STYLE_RES_FOUND &&
                !lv_color_eq(alarm_ui_theme_color(v.color, pal), v.color)) {
                const lv_obj_t *named = o;
                while (named != NULL && lv_obj_get_name(named) == NULL) {
                    named = lv_obj_get_parent(named);
                }
                printf("  0x%06X (property %u, %s style) on a %s in %s\n",
                       (unsigned)lv_color_to_u32(v.color) & 0xFFFFFFu, (unsigned)props[k],
                       o->styles[i].is_local ? "its own" : "a shared", o->class_p->name,
                       named != NULL ? lv_obj_get_name(named) : "?");
                bad++;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        bad += off_palette(lv_obj_get_child(o, (int32_t)i), pal);
    }
    return bad;
}

static void expect_theme_colours(const char *what)
{
    const uint32_t t = alarm_ui_theme_get();
    lv_display_t *d = lv_display_get_default();
    unsigned bad = 0;
    for (uint32_t i = 0; i < d->screen_cnt; i++) {
        const bool saver = lv_obj_find_by_name(d->screens[i], "saver_bg") != NULL;
        bad += off_palette(d->screens[i], alarm_ui_palettes[saver ? alarm_ui_theme_saver[t] : t]);
    }
    bad += off_palette(lv_layer_top(), alarm_ui_palette);
    if (bad > 0) {
        printf("  %s: %u colours of another appearance\n", what, bad);
    }
    expect(bad == 0, "UI-010: a colour didn't follow the appearance");
}

/* A disarmed, quiet house on Home, its toasts gone. */
static void theme_home(void)
{
    host_panel_init(1789857660000LL);
    host_panel_automations(false); /* theme_arm's Away would run Good Night on the devices' shots */
    run(500);
    wake();
    tap_name("nav_home");
    run(5000);
}

/* Armed Away with the pad, out through the front door (or auto-stay makes it Stay). */
static void theme_arm(void)
{
    tap(lv_obj_get_child(by_name("panel_arm"), 0));
    type("123456k");
    run(9000);
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(3000);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(50000);
    expect(find_label(lv_screen_active(), "Armed Away") != NULL || s_locale,
           "UI-010: not Armed Away for the appearance's picture");
}

/* Home through the front door, too late: the burglary alarm. */

static void theme_alarm(void)
{
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(300);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(40000);
    expect(find_label(lv_screen_active(), "Burglary alarm") != NULL || s_locale,
           "UI-010: no burglary alarm for the appearance's picture");
}

/* Settings > Wi-Fi > a network: its password on the keyboard. */
static void theme_keys(void)
{
    tap_name("bar_settings");
    run(300);
    tap_name("set_wifi");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
    }
    run(2500);
    tap_text("Maple Street");
    sheet_text("correct horse");
}

static void theme_keys_close(void)
{
    tap_name("sheet_cancel");
    run(300);
    tap_name("nav_home");
    run(300);
}

/* Smart Home in the theme of the moment (mockup-next a-devices-light, a-devices-contrast...). */
static void theme_devices(const char *name)
{
#if CONFIG_ALARM_HOME_CONTROL
    tap_name("nav_devices");
    run(300);
    expect_theme_colours(name);
    shot(name);
    tap_name("nav_home");
    run(300);
#else
    (void)name;
#endif
}

static void walk_themes(void)
{
    printf("Appearance: Light, High contrast, High contrast light, and Dark again\n");
    static uint16_t dark_home[W * H];
    theme_home();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    memcpy(dark_home, s_fb, sizeof(dark_home));

    ui_theme_apply(UI_THEME_LIGHT);
    run(300);
    expect(ui_theme_current() == UI_THEME_LIGHT, "UI-010: Light didn't take");
    expect_theme_colours("Light");
    shot("theme_light_home");
    theme_arm();
    shot("theme_light_armed");
    run(62000);
    expect_saver(true, "Light: armed, a minute untouched");
    expect_theme_colours("Light, the screen saver");
    shot("theme_light_saver");
    wake();
    run(300);
    theme_alarm();
    expect_theme_colours("Light, an alarm");
    shot("theme_light_alarm");
    type("123456k");
    run(500);
    tap_name("bar_settings");
    run(300);
    shot("theme_light_settings");
    theme_devices("theme_light_devices");

    ui_theme_apply(UI_THEME_CONTRAST);
    theme_home();
    expect_theme_colours("High contrast");
    shot("theme_contrast_home");
    theme_devices("theme_contrast_devices");
    theme_arm();
    theme_alarm();
    shot("theme_contrast_alarm");
    type("123456k");
    run(500);
    theme_keys();
    expect_theme_colours("High contrast, the keyboard");
    shot("theme_contrast_keys");
    theme_keys_close();

    ui_theme_apply(UI_THEME_CONTRAST_LIGHT);
    theme_home();
    expect_theme_colours("High contrast, light");
    shot("theme_contrast_light_home");
    theme_devices("theme_contrast_light_devices");
    theme_keys();
    shot("theme_contrast_light_keys");
    theme_keys_close();

    /* Dark again draws what Dark drew: nothing a theme added stays behind. */
    ui_theme_apply(UI_THEME_DARK);
    theme_home();
    expect_theme_colours("Dark again");
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    unsigned differ = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        differ += s_fb[i] != dark_home[i] ? 1u : 0u;
    }
    if (differ > 0) {
        printf("  Dark again: %u pixels differ from Dark before the other appearances\n", differ);
        shot("theme_dark_again");
    }
    expect(differ == 0, "UI-010: Dark after the other appearances isn't the Dark of before");

    /* A sheet open over a page follows a switch too, and stays open (nothing is rebuilt). */
    tap_name("bar_settings");
    run(300);
    tap_name("set_time");
    run(300);
    tap_text("Clock"); /* the clock is anyone's: no code */
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2, "UI-010: the Clock picker didn't open");
    ui_theme_apply(UI_THEME_LIGHT);
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2, "UI-010: a switch closed the open sheet");
    expect_theme_colours("Light, a pick sheet open");
    tap_name("pick_close");
    run(300);
    ui_theme_apply(UI_THEME_DARK);
    run(300);
    expect_theme_colours("Dark, the sheet closed");
    tap_name("nav_home");
    run(300);
}

/* The appearance as the touchscreen saves it (backend.save_options). */
static void expect_saved_look(uint8_t appearance, bool contrast, const char *what)
{
    ui_options_t saved;
    expect(host_panel_saved_options(&saved) && saved.appearance == appearance &&
               saved.high_contrast == contrast,
           what);
}

/* BL-132 (UI-010, UI-043): the appearance picked in Screen and sound (mockup-next a-display,
 * a-appearance, a-display-contrast), applied at once and saved; Automatic following the night
 * hours, light with them off; then Dark again for whatever comes after. Appended after every
 * other shot, so theirs keep their numbers. */
static void walk_appearance(const ui_options_t *start)
{
    printf("Appearance: picked in Screen and sound\n");
    host_panel_init(1789857660000LL + 12LL * 3600000LL); /* 10:41 in the morning */
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    tap_name("bar_settings");
    run(300);
    tap_name("set_display");
    run(300);
    lv_obj_t *body = by_name("page_body");
    expect(find_label(body, "Appearance") != NULL && find_label(body, "Dark") != NULL,
           "BL-132: Screen and sound has no Appearance row saying Dark");
    expect(find_label(body, "High contrast") != NULL, "BL-132: Screen and sound has no High contrast switch");

    /* Light: the list, then the page itself light, at once */
    tap_link("Appearance");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2, "BL-132: the Appearance row didn't open its list");
    expect(find_label(by_name("pick_list"), "Light words on a dark screen: easy on the eyes at night") != NULL &&
               find_label(by_name("pick_list"), "Night hours are off: stays light") != NULL,
           "BL-132: the appearances don't say when each suits (Automatic: the night hours are off)");
    tap_text("Light");
    run(300);
    expect(ui_theme_current() == UI_THEME_LIGHT, "UI-010: picking Light didn't make the screen light");
    expect(lv_subject_get_int(&subject_sheet) == 0, "BL-132: the list stayed open after a pick");
    expect(find_label(by_name("page_body"), "Light") != NULL, "BL-132: the Appearance row doesn't say Light");
    expect_saved_look(UI_APPEARANCE_LIGHT, false, "BL-132: Light wasn't saved");
    expect_theme_colours("Light, picked in Screen and sound");
    shot("settings_display_light");

    /* High contrast with Light, then with Dark (a-display-contrast) */
    tap_text("High contrast");
    run(300);
    expect(ui_theme_current() == UI_THEME_CONTRAST_LIGHT, "UI-010: High contrast with Light isn't black on white");
    expect_saved_look(UI_APPEARANCE_LIGHT, true, "BL-132: High contrast wasn't saved");
    expect_theme_colours("High contrast, light, from the switch");
    tap_link("Appearance");
    run(300);
    tap_text("Dark");
    run(300);
    expect(ui_theme_current() == UI_THEME_CONTRAST, "UI-010: High contrast with Dark isn't white on black");
    expect_theme_colours("High contrast, from the switch");
    shot("settings_display_contrast");
    tap_text("High contrast");
    run(300);
    expect(ui_theme_current() == UI_THEME_DARK, "UI-010: High contrast off didn't bring Dark back");

    /* Automatic with the night hours off: says so, and stays light */
    tap_link("Appearance");
    run(300);
    tap_text("Automatic");
    run(300);
    expect(ui_theme_current() == UI_THEME_LIGHT, "UI-043: Automatic with the night hours off isn't light");
    expect(find_label(by_name("page_body"), "Night hours are off: stays light") != NULL,
           "BL-132: Automatic doesn't say the night hours are off");
    expect_saved_look(UI_APPEARANCE_AUTO, false, "BL-132: Automatic wasn't saved");

    /* The night hours on, by day (22:00 to 07:00 at 10:41): light */
    ui_options_t o = *start;
    o.appearance = UI_APPEARANCE_AUTO;
    o.night_on = true;
    ui_app_apply_options(&o);
    run(300);
    expect(ui_theme_current() == UI_THEME_LIGHT, "UI-043: Automatic by day isn't light");
    /* At night (10:00 to 10:43, so now): dark at once, then light again by itself when the night
     * ends, from the clock alone */
    o.night_start = 10u * 60u;
    o.night_end = 10u * 60u + 43u;
    ui_app_apply_options(&o);
    run(300);
    expect(ui_theme_current() == UI_THEME_DARK, "UI-043: Automatic during the night hours isn't dark");
    expect_theme_colours("Automatic, at night");
    expect(find_label(by_name("page_body"), "Dark during the night hours, light by day") != NULL,
           "BL-132: Automatic doesn't say it follows the night hours");
    run(150000);
    expect(ui_theme_current() == UI_THEME_LIGHT, "UI-043: Automatic stayed dark after the night hours ended");
    expect_theme_colours("Automatic, the night over");
    wake();
    run(300);

    /* The list as drawn (a-appearance): Automatic chosen, at night */
    o.night_end = 12u * 60u;
    ui_app_apply_options(&o);
    run(300);
    expect(ui_theme_current() == UI_THEME_DARK, "UI-043: Automatic at night again isn't dark");
    tap_name("bar_settings");
    run(300);
    tap_name("set_display");
    run(300);
    tap_link("Appearance");
    run(300);
    expect(find_label(by_name("pick_list"), "Dark during the night hours (10:00 AM to 12:00 PM), light by day") != NULL,
           "BL-132: Automatic in the list doesn't say the night hours");
    shot("appearance_pick");
    tap_name("pick_close");
    run(300);

    /* Dark and off again, as every walk starts */
    ui_app_apply_options(start);
    run(300);
    expect(ui_theme_current() == UI_THEME_DARK, "UI-010: the default appearance isn't Dark");
    expect_theme_colours("Dark again, from the options");
    tap_name("nav_home");
    run(300);
}

/* ---- Installer > Zigbee (BL-144, docs/features/20 ZB-027) -------------------------------------
 * Every page as drawn (docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js, once the BL-144
 * proposal), each shot named after the drawing's screen id, over the interim demo backend
 * (host_zigbee_demo.h): the installer menu's row, the Zigbee page in every radio state, adding a
 * device with each of its endings, a device's page, adding with an install code and the codes
 * waiting, removing, the radio's details, the channel, the reset, the zone page and the sensor
 * picker, and the Troubles list's link. */

static void zb_top(void)
{
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(100);
}

/* The page scrolled so this text's label is at its top (a section heading), as drawn. */
static void zb_scroll_to_top_of(const char *text)
{
    lv_obj_t *body = by_name("page_body");
    lv_obj_t *l = find_label(body, text);
    expect(l != NULL, text);
    if (l == NULL) {
        return; /* a locale walk: reported by expect, and nothing to scroll to */
    }
    lv_area_t b, a;
    lv_obj_get_coords(body, &b);
    lv_obj_get_coords(l, &a);
    lv_obj_scroll_by_bounded(body, 0, b.y1 - a.y1 + 4, LV_ANIM_OFF);
    run(100);
}

static void zb_end(void)
{
    lv_obj_t *body = by_name("page_body");
    lv_obj_scroll_by_bounded(body, 0, -lv_obj_get_scroll_bottom(body), LV_ANIM_OFF);
    run(100);
}

/* The demo changes; the pages read it again within a second (UI-023: never faster). */
static void zb_state(host_zb_demo_t what)
{
    host_zigbee_demo_state(what);
    run(1100);
}

static void zb_has(const char *text, const char *what)
{
    expect(find_label(by_name("page_body"), text) != NULL, what);
}

static void zb_toast(const char *text, const char *what)
{
    expect(lv_subject_get_int(&subject_toast_visible) == 1 &&
               strcmp(lv_subject_get_string(&subject_toast), text) == 0,
           what);
}

/* Whether the switch of the row with this title is on. */
static bool zb_switch_on(const char *title)
{
    lv_obj_t *body = by_name("page_body");
    lv_obj_t *row = find_label(body, title);
    expect(row != NULL, title);
    if (row == NULL) {
        return false; /* a locale walk: reported by expect, and no row to climb from */
    }
    while (lv_obj_get_parent(row) != body) {
        row = lv_obj_get_parent(row);
    }
    lv_obj_t *sw = find_visible(row, "toggle");
    expect(sw != NULL, title);
    return sw != NULL && lv_obj_has_state(sw, LV_STATE_CHECKED);
}

/* Whether the pick sheet behind this row offers this choice; the sheet is closed again. */
static bool zb_pick_offers(const char *row, const char *choice)
{
    tap_link(row);
    run(300);
    const bool offered = find_label(by_name("pick_list"), choice) != NULL;
    tap_name("pick_close");
    run(400);
    return offered;
}

/* A row's value, chosen from its pick sheet. */
static void zb_pick(const char *row, const char *choice)
{
    tap_link(row);
    run(300);
    expect(find_label(by_name("pick_list"), choice) != NULL, choice);
    tap_text(choice);
    run(400);
}

/* 14-16: a siren's, a keypad's and a key fob's pages (BL-147, BL-148; ZB-023, ZB-024), from the
 * Zigbee page with the demo's eight devices; back on it at the end. */
static void walk_zigbee_roles(void)
{
    /* 14: a siren (ZB-023) */
    zb_top();
    scroll_to_text("page_body", "Hall Siren");
    tap_link("Hall Siren");
    run(300);
    expect(find_label(lv_screen_active(), "Heiman siren · Mains · Extends the network") != NULL,
           "ZB-023: the siren's page doesn't say what it is and how it is powered");
    zb_has("It sounds for", "ZB-023: the siren's page doesn't lead with what it sounds for");
    expect(zb_switch_on("Burglary and panic") && zb_switch_on("Fire and CO"),
           "ZB-023: the siren's alarms aren't as the panel keeps them");
    zb_has("How it joined", "ZB-023: the siren's page lost the device page's rows");
    expect(find_label(by_name("page_body"), "Replace it") == NULL,
           "ZB-023: a siren offers Replace it, which would lose its settings");
    shot("bl-147-siren");
    zb_scroll_to_top_of("How it sounds");
    expect(strcmp(row_label("Volume", "value_label"), "High") == 0,
           "ZB-023: the siren's volume isn't High");
    expect(strcmp(row_label("Partitions", "value_label"), "All") == 0,
           "ZB-023: the siren's partitions aren't All");
    expect(strcmp(row_label("Also sounds for", "value_label"), "None") == 0,
           "OUT-015: a Zigbee siren's Also sounds for isn't None");
    expect(strcmp(row_label("Supervision", "value_label"), "After 1 h") == 0,
           "ZB-023: the siren's supervision isn't after 1 hour");
    expect(zb_switch_on("Strobe with it") && !zb_switch_on("Chirp when arming and disarming"),
           "ZB-023: the siren's strobe and chirp aren't as the panel keeps them");
    zb_pick("Volume", "Very high");
    expect(strcmp(row_label("Volume", "value_label"), "Very high") == 0,
           "ZB-023: the siren's volume wasn't kept");
    zb_pick("Volume", "High");
    tap_text("Chirp when arming and disarming");
    run(400);
    expect(zb_switch_on("Chirp when arming and disarming"), "ZB-023: the chirp wasn't kept");
    tap_text("Chirp when arming and disarming");
    run(400);
    expect(!zb_switch_on("Chirp when arming and disarming"), "ZB-023: the chirp wasn't turned off");
    /* ZB-023: "Test it" is refused while one of its partitions is armed, in its own words */
    host_zigbee_demo_state(HOST_ZB_SIREN_ARMED);
    tap_link("Test it");
    run(300);
    zb_toast("Not while one of its partitions is armed",
             "ZB-023: a siren's test refused while armed doesn't say why");
    expect(strcmp(row_label("Test it", "detail_label"), "Sounds it for 2 seconds") == 0,
           "ZB-023: a refused siren test shows a result");
    run(4000);
    /* "Test it": a test that gets no answer says so in place, then one that sounds */
    host_zigbee_demo_state(HOST_ZB_SIREN_SILENT);
    tap_link("Test it");
    run(3200);
    expect(strcmp(row_label("Test it", "detail_label"), "Last test: no answer. Check its power") == 0,
           "ZB-023: a siren's test without an answer isn't said in place");
    tap_link("Test it");
    run(3200);
    expect(strcmp(row_label("Test it", "detail_label"), "Last test: it answered and sounded") == 0,
           "ZB-023: a siren's test result isn't said in place");
    zb_scroll_to_top_of("Check it"); /* Test it with its result, and Supervision */
    shot("bl-147-siren-end");
    /* ZB-023: a siren sounding for nothing is no longer used (nor counted among the four) */
    zb_top();
    tap_text("Fire and CO");
    run(400);
    tap_text("Burglary and panic");
    run(400);
    zb_has("The panel doesn't sound it yet: turn on what it sounds for.",
           "ZB-023: a siren with both alarms off is still used");
    expect(strcmp(row_label("Partitions", "value_label"), "None") == 0,
           "ZB-023: a siren with both alarms off still has its partitions");
    tap_name("page_back");
    run(300);

    /* 15: a keypad (ZB-024) */
    zb_top();
    scroll_to_text("page_body", "Entry Keypad");
    tap_link("Entry Keypad");
    run(300);
    expect(strcmp(row_label("Partition", "value_label"), "Home") == 0,
           "ZB-024: the keypad's partition isn't Home");
    expect(strcmp(row_label("Panic", "value_label"), "Panic alarm, audible") == 0 &&
               strcmp(row_label("Fire", "value_label"), "Fire alarm") == 0 &&
               strcmp(row_label("Emergency", "value_label"), "Medical alarm") == 0,
           "ZB-024: the keypad's buttons aren't as the panel keeps them");
    expect(zb_switch_on("Show the panel's state"), "ZB-024: the keypad doesn't show the state");
    zb_has("Codes typed on it are checked by the panel. Five wrong codes lock it for a while, like "
           "this screen.",
           "ZB-024: the keypad's page doesn't say the panel checks its codes");
    shot("bl-148-keypad");
    zb_pick("Emergency", "Off");
    expect(strcmp(row_label("Emergency", "value_label"), "Off") == 0,
           "ZB-024: the keypad's Emergency button wasn't turned off");
    zb_pick("Emergency", "Medical alarm");
    /* ZB-024: the last numbers taken between reading the page and asking: refused FULL */
    zb_state(HOST_ZB_KEYPAD_RACE);
    zb_top();
    expect(strcmp(row_label("Partition", "value_label"), "Off") == 0,
           "ZB-024: a keypad without a number arms a partition");
    zb_pick("Partition", "Home");
    zb_toast("No keypad number is free: other keypads use 1 to 8",
             "ZB-024: a keypad refused for want of a number doesn't say why");
    run(4000);
    /* no keypad number free: said on its page, and its partition can't be chosen */
    zb_state(HOST_ZB_KEYPAD_NO_SLOT);
    zb_top();
    zb_has("Every keypad number from 1 to 8 is in use by other keypads. Free one under Keypads and "
           "devices to use this one.",
           "ZB-024: a keypad with no number free doesn't say why");
    expect(strcmp(row_label("Partition", "value_label"), "Off") == 0,
           "ZB-024: a keypad with no number arms a partition");
    tap_link("Partition");
    run(300);
    zb_toast("No keypad number is free", "ZB-024: a keypad with no number free can be assigned");
    run(4000);
    tap_name("page_back");
    run(300);

    /* 16: a key fob (ZB-024) */
    zb_top();
    scroll_to_text("page_body", "Alex's key fob");
    tap_link("Alex's key fob");
    run(300);
    zb_has("Who uses it", "ZB-024: the key fob's page doesn't say whose it is");
    expect(strcmp(row_label("Belongs to", "value_label"), "Alex") == 0,
           "ZB-024: the key fob's page doesn't name its user");
    expect(strcmp(row_label("Arms", "value_label"), "Home") == 0,
           "ZB-024: the key fob doesn't arm Home");
    expect(!zb_switch_on("May disarm") && !zb_switch_on("Panic button"),
           "ZB-024: a key fob may disarm or panic without the installer turning it on");
    shot("bl-148-fob");
    tap_text("May disarm");
    run(400);
    expect(zb_switch_on("May disarm"), "ZB-024: the key fob's May disarm wasn't kept");
    tap_text("May disarm");
    run(400);
    expect(!zb_switch_on("May disarm"), "ZB-024: the key fob's May disarm wasn't turned off");
    /* ZB-024: a setting the panel can't use is refused, and the page keeps what it had */
    host_zigbee_demo_state(HOST_ZB_ROLE_INVALID);
    tap_text("May disarm");
    run(400);
    zb_toast("Not kept: the panel can't use that setting",
             "ZB-024: a key fob setting refused INVALID doesn't say why");
    run(1100);
    expect(!zb_switch_on("May disarm"), "ZB-024: a refused May disarm shows as kept");
    run(4000);
    /* ZB-024: one key fob for each user: the fob is Alex's, and the demo answers as if the master
     * had another one; a fob is never the installer's (alarm-devices.md §3) */
    host_zigbee_demo_state(HOST_ZB_FOB_TAKEN);
    expect(!zb_pick_offers("Belongs to", "Installer"), "ZB-024: a key fob may belong to the installer");
    /* the same sheet offers Alex, a standard user (the walk's timing stays as it was: the real
     * panel's shots after this one catch its devices mid-setup) */
    tap_link("Belongs to");
    run(300);
    expect(find_label(by_name("pick_list"), "Alex") != NULL,
           "ZB-024: a key fob may not belong to a standard user");
    expect(find_label(by_name("pick_list"), "Master") != NULL, "Master");
    tap_text("Master");
    run(400);
    zb_toast("That user has a key fob already",
             "ZB-024: a key fob given to a user with another one isn't refused IN_USE");
    run(1100);
    expect(strcmp(row_label("Arms", "value_label"), "Home") == 0 &&
               strcmp(row_label("Belongs to", "value_label"), "Alex") == 0,
           "ZB-024: a refused key fob user changed the fob");
    run(4000);
    tap_name("page_back");
    run(300);
    zb_state(HOST_ZB_WORKING);
}

/* Back until this text is on the page (the installer menu: "Leave installer mode"). */
static void zb_back_to(const char *text)
{
    for (int i = 0; i < 6 && find_label(by_name("page_body"), text) == NULL; i++) {
        tap_name("page_back");
        run(300);
    }
    expect(find_label(by_name("page_body"), text) != NULL, text);
}

/* BL-037: Installer > Sirens and outputs, a wired siren, a 433 MHz siren and its pairing, adding a
 * wired output, and Installer > 433 MHz (docs/features/16; the mockup's BL-037 proposal), with the
 * simulator's demo sirens (host_panel_sirens_demo); from the installer menu, back on it at the end,
 * the demo cleared so later pictures are as before. */
static void walk_sirens(void)
{
    host_panel_sirens_demo(true);
    tap_link("Partitions"); /* the menu is built again with the demo's sirens */
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Sirens and outputs");
    zb_has("What sounds and what switches when the alarm does",
           "OUT-009: the installer menu has no Sirens and outputs row");
    zb_has("2 sirens", "OUT-009: the Sirens and outputs row doesn't count the sirens");
    shot("bl-037-installer-menu");
    scroll_to_text("page_body", "433 MHz");
    zb_has("Sensors and sirens on 433 MHz", "RF-026: the installer menu has no 433 MHz row");
    zb_has("1 sensor, 1 siren", "RF-026: the 433 MHz row doesn't count its sensors and sirens");

    /* Sirens and outputs */
    scroll_to_text("page_body", "Sirens and outputs");
    tap_link("Sirens and outputs");
    run(300);
    zb_has("On the panel's terminals", "OUT-009: Sirens and outputs has no wired section");
    zb_has("Terminal GPIO 49 · Home", "OUT-009: the wired siren's line doesn't give its terminal");
    zb_has("Armed light", "OUT-009: the armed light isn't listed");
    zb_has("433 MHz siren 1", "OUT-010: the 433 MHz siren isn't listed");
    shot("bl-037-sirens");

    /* a wired siren's page */
    tap_link("Siren");
    run(300);
    zb_has("It sounds for", "OUT-009: the wired siren's page doesn't lead with what it sounds for");
    expect(zb_switch_on("Burglary and panic") && zb_switch_on("Fire and CO"),
           "OUT-009: the wired siren's alarms aren't as kept");
    zb_has("Also sounds for", "OUT-015: the wired siren has no Also sounds for");
    shot("bl-037-wired");
    zb_scroll_to_top_of("How it is wired");
    expect(strcmp(row_label("Terminal", "value_label"), "GPIO 49") == 0,
           "OUT-009: the wired siren's terminal isn't GPIO 49");
    expect(strcmp(row_label("Relay board", "value_label"), "Switches on low") == 0,
           "OUT-009: the wired siren's relay board isn't on low");
    expect(strcmp(row_label("Supervision input", "value_label"), "GPIO 50") == 0,
           "OUT-004: the wired siren's supervision input isn't GPIO 50");
    expect(host_panel_siren_pin(49), "OUT-006: an active-low siren's pin isn't high (off) at rest");
    tap_link("Test it");
    run(200);
    zb_toast("Sounding for 2 seconds", "OUT-009: the wired siren's test doesn't say it sounds");
    expect(!host_panel_siren_pin(49), "OUT-009: the wired siren's test doesn't drive its pin");
    run(2500);
    expect(host_panel_siren_pin(49), "OUT-009: the wired siren's test doesn't end");
    zb_scroll_to_top_of("How it is wired");
    shot("bl-037-wired-end");
    tap_name("page_back");
    run(300);

    /* a 433 MHz siren and its pairing */
    scroll_to_text("page_body", "433 MHz siren 1");
    tap_link("433 MHz siren 1");
    run(300);
    zb_has("Not supervised: the panel can't hear this siren. Test it now and then.",
           "OUT-012: the 433 MHz siren's page doesn't say it isn't supervised");
    shot("bl-037-rf-siren");
    scroll_to_text("page_body", "Pair it again");
    tap_link("Pair it again");
    run(300);
    const uint32_t sent = host_panel_rf_sent();
    tap_text("Send the pairing signal");
    run(4000);
    expect(host_panel_rf_sent() >= (uint32_t)(sent + 3u), "OUT-011: the pairing signal didn't go out three times");
    shot("bl-037-rf-siren-pair");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);

    /* adding a wired output: what it is, then its terminal */
    scroll_to_text("page_body", "Add a wired siren or output");
    tap_link("Add a wired siren or output");
    run(300);
    expect(find_label(by_name("pick_list"), "Smoke detector power") != NULL,
           "OUT-009: adding a wired output doesn't offer the smoke detectors' power");
    shot("bl-037-wired-add");
    tap_text("Strobe");
    run(300);
    expect(find_label(by_name("pick_list"), "GPIO 49") == NULL,
           "OUT-021: a terminal another output uses is offered");
    tap_text("GPIO 52");
    run(400);
    zb_has("It follows", "OUT-009: the new strobe's page didn't open");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);

    /* Installer > 433 MHz */
    scroll_to_text("page_body", "433 MHz");
    tap_link("433 MHz");
    run(1200);
    zb_has("Receiver", "RF-026: the 433 MHz page has no receiver row");
    zb_has("Working", "RF-026: the 433 MHz page's receiver isn't working");
    zb_has("433 MHz sirens can be sounded", "RF-026: the 433 MHz page has no transmitter row");
    zb_has("Back Door", "RF-026: the 433 MHz page doesn't list the zone on a 433 MHz contact");
    shot("bl-037-rf");
    tap_name("page_back");
    run(300);
    host_panel_sirens_demo(false);
    tap_link("Partitions");
    run(300);
    tap_name("page_back");
    run(300);
}

static void walk_zigbee(const ui_options_t *start)
{
    printf("Zigbee\n");
    host_panel_init(1789857660000LL + 12LL * 3600000LL); /* 10:41 in the morning */
    {
        /* ZB-024: the demo's key fob is Alex's, a standard user (user 3), so the walk can pick
         * another user for it; the pages before the key fob's show no users */
        ui_backend_t zbe;
        host_panel_backend(&zbe);
        expect(zbe.user_set(zbe.ctx, "123456", 3u, "2468", ALARM_AUTH_STANDARD, 0x01u, "Alex") ==
                   ALARM_OK,
               "the Zigbee walk couldn't add Alex, the key fob's user");
    }
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }

    /* 1: the installer menu's row, after Z-Wave's */
    scroll_to_text("page_body", "Zigbee");
    zb_has("Zigbee", "ZB-027: the installer menu has no Zigbee row");
    zb_has("Sensors on the panel's Zigbee radio", "ZB-027: the Zigbee row doesn't say what it is");
    zb_has("8 devices", "ZB-027: the Zigbee row doesn't count its devices");
    shot("bl-144-installer-menu");
    zb_state(HOST_ZB_INTERFERENCE);
    zb_has("Interference", "ZB-006: the Zigbee row doesn't say Interference");
    shot("bl-144-installer-menu-problem");
    zb_state(HOST_ZB_WORKING);
    zb_has("8 devices", "ZB-027: the Zigbee row's words didn't follow the radio back");

    /* 2: the Zigbee page */
    tap_link("Zigbee");
    run(300);
    expect(find_label(lv_screen_active(), "Sensors on the panel's Zigbee radio") != NULL,
           "ZB-027: the Zigbee page has no subtitle");
    zb_has("Radio · Working", "ZB-027: the Zigbee page's radio isn't working");
    zb_has("Channel 15 · 8 devices", "ZB-027: the radio row doesn't give its channel and devices");
    zb_has("Add a device", "ZB-027: the installer can't add a device");
    zb_has("Install codes waiting", "ZB-027: no Install codes waiting row");
    shot("bl-144-zigbee");
    zb_scroll_to_top_of("Devices");
    zb_has("Not heard for 2 h 30 min", "ZB-027: the unreachable device doesn't say for how long");
    zb_has("Battery low: change it soon", "ZB-027: the low battery isn't the thing to do");
    zb_has("Aqara door sensor · Battery 92 % · Signal good",
           "ZB-027: a device row doesn't give its model, battery and signal");
    zb_has("Zones 6, 9", "ZB-027: a device with two zones doesn't list both");
    shot("bl-144-zigbee-devices");
    zb_end();
    zb_has("Every device must be added again", "ZB-027: the reset doesn't warn");
    shot("bl-144-zigbee-end");

    zb_state(HOST_ZB_EMPTY);
    zb_top();
    zb_has("No devices yet. Add one: most sensors join when you hold their button until their light "
           "blinks.",
           "ZB-027: an empty network doesn't say how to add the first device");
    zb_has("A new network, with new keys", "ZB-027: the reset of an empty network says devices go");
    shot("bl-144-zigbee-empty");
    zb_state(HOST_ZB_WORKING);
    zb_state(HOST_ZB_STARTING);
    zb_top();
    zb_has("Radio · Starting", "ZB-027: the radio starting isn't said");
    tap_link("Add a device");
    run(300);
    zb_toast("The Zigbee radio isn't working", "ZB-027: a dimmed Add a device doesn't say why");
    shot("bl-144-zigbee-starting");
    run(4000); /* its toast gone */
    zb_state(HOST_ZB_NOT_CONNECTED);
    zb_has("Radio · Not connected", "ZB-027: an empty socket isn't said");
    zb_has("Devices added before are kept. They are heard again once the Zigbee radio answers.",
           "ZB-027: an empty socket doesn't say the devices are kept");
    shot("bl-144-zigbee-not-connected");
    zb_state(HOST_ZB_NOT_ANSWERING);
    zb_has("Radio · Not answering", "ZB-027: a silent radio isn't said");
    shot("bl-144-zigbee-not-answering");
    zb_state(HOST_ZB_PAUSED);
    zb_has("Radio · Paused after restarts", "ZB-027: the paused radio isn't said");
    zb_has("Try again", "ZB-027: a paused radio can't be tried again");
    shot("bl-144-zigbee-paused");
    zb_state(HOST_ZB_SOCKET_ZWAVE);
    zb_has("Radio · Socket used by Z-Wave", "ZB-027: the Z-Wave radio in the socket isn't said");
    shot("bl-144-zigbee-socket-zwave");
    zb_state(HOST_ZB_INTERFERENCE);
    zb_has("Radio · Interference", "ZB-006: interference isn't said on the Zigbee page");
    zb_has("Something nearby drowns channel 15: sensors may not be heard",
           "ZB-006: interference doesn't say which channel");
    shot("bl-144-zigbee-interference");
    /* bl-144-zigbee-locked isn't shot: installer mode ending pops back to the locked installer
     * page (ui_settings_refresh) and the trouble link asks for the installer code first, so no
     * Zigbee page shows outside installer mode (ZB-027). */
    zb_state(HOST_ZB_WORKING);
    zb_top();

    /* 3: adding a device */
    tap_link("Add a device");
    run(1300);
    zb_has("Any Zigbee 3.0 device may join", "ZB-002: the add page isn't listening");
    zb_has("Leaving this page stops listening.", "ZB-002: the add page doesn't say leaving stops it");
    run(18000); /* the countdown as drawn: once a second, nothing else moves */
    expect(find_label_with(lv_screen_active(), "Listening · 2 min 4") != NULL,
           "ZB-002: the add page doesn't count down its 3 minutes");
    shot("bl-144-add-listening");
    zb_state(HOST_ZB_JOIN_TWO);
    zb_has("2 devices joined. More can join until the time runs out.",
           "ZB-027: the devices that joined aren't counted");
    zb_has("Ready · Joined with the standard key", "ZB-027: a ready device doesn't say how it joined");
    zb_has("Reading what it can do · step 3 of 5", "ZB-027: a joining device doesn't give its step");
    shot("bl-144-add-found");
    zb_state(HOST_ZB_DOZE);
    zb_has("It went to sleep before it finished", "ZB-027: a dozing device isn't said");
    zb_has("Wake it: press its button once, then again every few seconds until it says Ready.",
           "ZB-027: a dozing device doesn't say how to wake it");
    shot("bl-144-add-asleep");
    tap_link("Aqara door sensor");
    run(300);
    zb_has("Added: Aqara door sensor", "ZB-027: the Added page doesn't say what was added");
    zb_has("A new zone", "ZB-027: the Added page offers no new zone");
    zb_has("An existing zone", "ZB-027: the Added page offers no existing zone");
    shot("bl-144-add-added");
    tap_name("page_back");
    run(300);
    zb_has("It went to sleep before it finished", "ZB-002: back from Added, the add page stopped");
    zb_state(HOST_ZB_WINDOW_ENDS);
    zb_has("Setup not finished", "ZB-027: a setup cut short isn't said");
    /* ZB-012: its setup stopped (the row DONE, as zbz_panel ends it): no step, and no promise that
     * it finishes by itself - only Set it up again goes on */
    zb_has("Aqara motion sensor", "ZB-027: a setup cut short doesn't name the device");
    expect(find_label_with(lv_screen_active(), "step 5 of 5") == NULL,
           "ZB-012: a stopped setup gives a step count");
    expect(find_label_with(lv_screen_active(), "next wakes") == NULL,
           "ZB-012: a stopped setup is said to finish by itself");
    zb_has("It joined, but its setup stopped before it finished.*",
           "ZB-012: a stopped setup doesn't say to set it up again");
    shot("bl-144-add-not-finished");
    tap_text("Open it");
    run(300);
    zb_has("Wake it to finish setting it up:*",
           "ZB-027: the device page doesn't say to wake it");
    zb_state(HOST_ZB_WAKE);
    zb_has("Make it a zone", "ZB-027: a ready device in no zone can't be made a zone");
    zb_has("Motion: Interior follower", "ZB-027: Make it a zone doesn't say what it makes");
    shot("bl-144-device-new");
    tap_name("page_back");
    run(300);
    zb_top();

    tap_link("Add a device");
    run(1300);
    zb_state(HOST_ZB_WINDOW_ENDS);
    zb_has("Nothing joined", "ZB-027: nothing joining isn't said");
    zb_has("The 3 minutes ran out", "ZB-027: why nothing joined isn't said");
    shot("bl-144-add-nothing");
    tap_text("Try again");
    run(1300);
    zb_has("Any Zigbee 3.0 device may join", "ZB-002: Try again doesn't listen again");
    zb_state(HOST_ZB_JOIN_FAILS);
    zb_has("Didn't join", "ZB-027: a join that failed isn't said");
    shot("bl-144-add-didnt-join");
    tap_text("Try again");
    run(1300);
    zb_state(HOST_ZB_JOIN_KNOWN);
    zb_has("This device is already in the network: it's Front Door.",
           "ZB-027: a device already added isn't named");
    shot("bl-144-add-already");
    zb_state(HOST_ZB_JOIN_PLUG);
    zb_has("Not a sensor", "ZB-027: a device that isn't a sensor isn't said");
    zb_has("Sonoff smart plug · Mains", "ZB-027: the plug's model and power aren't given");
    shot("bl-144-add-not-sensor");
    tap_text("Keep it");
    run(300);
    zb_has("Not a sensor: kept, it extends the network", "ZB-027: Keep it didn't keep it");
    tap_text("Stop");
    run(1300);
    zb_has("Radio · Working", "ZB-002: Stop didn't go back to the Zigbee page");

    /* 5: a device's page */
    zb_top();
    scroll_to_text("page_body", "Front Door");
    tap_link("Front Door");
    run(300);
    zb_has("Aqara MCCGQ11LM", "ZB-027: the device page doesn't give its model");
    zb_has("Door and window contact", "ZB-027: the device page doesn't say what it is");
    zb_has("With its install code", "ZB-027: the device page doesn't say how it joined");
    zb_has("2 min ago", "ZB-027: the device page doesn't say when it was heard");
    zb_has("Zone 1 · Front Door", "ZB-027: the device page doesn't say which zone uses it");
    shot("bl-144-device");
    tap_link("Check it now");
    run(2100);
    zb_end();
    zb_toast("It answered", "ZB-027: Check it now doesn't say it answered");
    shot("bl-144-device-end");
    run(4000);
    tap_name("page_back");
    run(300);
    zb_top();
    tap_link("Basement Water");
    run(300);
    zb_has("Not heard for 2 h 30 min. Change its battery or bring it closer, then press its button "
           "to wake it.",
           "ZB-027: an unreachable device doesn't lead with what to do");
    zb_has("Weak, when last heard", "ZB-027: an unreachable device's signal isn't as last heard");
    zb_has("Shows CHECK", "ZB-027: an unreachable device's zone doesn't say CHECK");
    shot("bl-144-device-unreachable");
    tap_link("Forget it: it's gone or broken");
    run(300);
    expect(find_label(lv_screen_active(), "Forget Basement Water?") != NULL,
           "ZB-027: Forget doesn't ask first");
    expect(find_label_with(lv_screen_active(), "Zone 8 keeps its settings and needs a new sensor.") != NULL,
           "ZB-027: Forget doesn't say the zone needs a new sensor");
    shot("bl-144-forget-confirm");
    tap_name("dialog_ok");
    run(2100);
    zb_toast("Forgotten: Zone 8 needs a new sensor", "ZB-027: Forget doesn't say what's left to do");
    zb_has("Radio · Working", "ZB-027: a forgotten device's page stayed open");
    expect(find_label(by_name("page_body"), "Basement Water") == NULL,
           "ZB-027: the forgotten device is still listed");
    run(4000);

    /* 4: adding with an install code (ZB-015). The panel's own keyboard; spaces and dashes as on
     * labels; the line under the title counts, then says whether the code's check matches. */
    zb_top();
    tap_link("Add with an install code");
    run(300);
    zb_has("Not typed", "ZB-015: the install code page doesn't say what is still to type");
    tap_text("Listen for this device");
    run(300);
    zb_toast("Type its address and install code first", "ZB-015: Listen with nothing typed isn't refused");
    run(4000);
    tap_link("Its address");
    run(300);
    sheet_text("02AB CD00 0000 01");
    run(1100);
    expect(find_label(lv_screen_active(), "14 of 16 characters") != NULL,
           "ZB-015: the address isn't counted while it is typed");
    tap_name("sheet_done");
    run(300);
    expect(find_label(lv_screen_active(), "14 of 16 characters") != NULL,
           "ZB-015: Done took an address that isn't whole");
    sheet_text("02AB CD00 0000 01G9");
    run(1100);
    expect(find_label(lv_screen_active(), "Only 0 to 9 and A to F: compare it with the label") != NULL,
           "ZB-015: a character that isn't hex isn't refused");
    sheet_text("02AB CD00 0000 0109");
    run(1100);
    expect(find_label(lv_screen_active(), "All 16 characters") != NULL,
           "ZB-015: a whole address isn't said whole");
    shot("bl-144-code-address");
    tap_name("sheet_done");
    run(300);
    zb_has("02AB CD00 0000 0109", "ZB-015: the address typed isn't shown");
    tap_link("Its install code");
    run(300);
    sheet_text("83FE D340 7A93 9723 A5C6 39B2 6916");
    run(1100);
    expect(find_label(lv_screen_active(), "28 of 36 characters") != NULL,
           "ZB-015: the install code isn't counted while it is typed");
    sheet_text("83FE-D340-7A93-9723-A5C6-39B2-6916-D505-C3B6"); /* dashes; its last digit wrong */
    run(1100);
    expect(find_label(lv_screen_active(), "It doesn't check out: compare it with the label") != NULL,
           "ZB-015: a code whose CRC doesn't match isn't said wrong");
    shot("bl-144-code-wrong");
    tap_name("sheet_done");
    run(300);
    expect(find_label(lv_screen_active(), "It doesn't check out: compare it with the label") != NULL,
           "ZB-015: Done took a code whose check doesn't match");
    sheet_text("83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5");
    run(1100);
    expect(find_label(lv_screen_active(), "Its check matches: the code is right") != NULL,
           "ZB-015: a right code isn't said right");
    shot("bl-144-code-entry");
    tap_name("sheet_done");
    run(300);
    zb_has("Checked", "ZB-015: a code whose check matches isn't said Checked");
    expect(find_label_with(by_name("page_body"), "C3B5") == NULL,
           "ZB-015: the install code is shown again once typed");
    shot("bl-144-code");
    tap_text("Listen for this device");
    run(1300);
    zb_has("Only this device may join", "ZB-015: listening with a code doesn't say only it may join");
    zb_has("Power it up or put it in pairing mode now. Only the device 02AB CD00 0000 0109 may join.",
           "ZB-015: listening with a code doesn't name the device");
    run(9000);
    shot("bl-144-code-listening");
    tap_text("Stop");
    run(1300);
    zb_has("Checked", "ZB-015: Stop didn't go back to the install code page");
    tap_name("page_back");
    run(300);
    zb_has("Radio · Working", "ZB-015: back from the install code page isn't the Zigbee page");

    /* 10: install codes waiting - the code given above waits (its device didn't join) */
    tap_link("Install codes waiting");
    run(300);
    zb_has("02AB CD00 0000 0109", "ZB-015: a code given when listening doesn't wait");
    expect(find_label_with(by_name("page_body"), "Added today at 10:4") != NULL,
           "ZB-015: a waiting code doesn't say when it was added");
    expect(find_label_with(by_name("page_body"), "Added on ") != NULL,
           "ZB-015: a code waiting since an earlier day doesn't give its date");
    expect(find_label_with(by_name("page_body"), "C3B5") == NULL,
           "ZB-015: a waiting install code is shown again");
    shot("bl-144-codes");
    tap_link("02AB CD00 0000 0109");
    run(300);
    expect(find_label(lv_screen_active(), "Forget this install code?") != NULL,
           "ZB-015: forgetting a code doesn't ask first");
    shot("bl-144-codes-forget");
    tap_name("dialog_ok");
    run(300);
    expect(find_label(by_name("page_body"), "02AB CD00 0000 0109") == NULL,
           "ZB-015: a forgotten code still waits");
    /* typed and left without listening: it waits, said in a toast */
    tap_text("Add an install code");
    run(300);
    tap_link("Its address");
    run(300);
    sheet_text("02AB-CD00-0000-010A");
    tap_name("sheet_done");
    run(300);
    tap_link("Its install code");
    run(300);
    sheet_text("83FED3407A939723A5C639B26916D505C3B5");
    tap_name("sheet_done");
    run(300);
    tap_name("page_back");
    run(300);
    zb_toast("Its code waits under Install codes waiting",
             "ZB-015: a code typed and not listened for isn't kept");
    zb_has("02AB CD00 0000 010A", "ZB-015: a code typed and not listened for doesn't wait");
    run(4000);
    tap_name("page_back");
    run(300);

    /* 7: the radio's details */
    zb_top();
    tap_link("Radio · Working");
    run(300);
    zb_has("Network id (PAN)", "ZB-027: the radio's details don't give the network id");
    zb_has("1A62", "ZB-027: the radio's details don't give the PAN id");
    zb_has("-92 dBm", "ZB-006: the radio's details don't give the background noise");
    zb_has("1 (it pauses at 2)", "ZB-027: the radio's details don't count the restarts it caused");
    shot("bl-144-radio");
    tap_link("Restart the radio");
    run(300);
    expect(find_label(lv_screen_active(), "Restart the Zigbee radio?") != NULL,
           "ZB-027: restarting the radio doesn't ask first");
    tap_name("dialog_ok");
    run(1100);
    zb_has("Starting", "ZB-027: the radio's state doesn't follow its restart");
    run(2200);
    zb_has("Working", "ZB-027: the radio's state didn't come back to Working");
    zb_state(HOST_ZB_PAUSED);
    zb_has("Paused after restarts", "ZB-027: the radio's details don't say it is paused");
    zb_has("2: paused", "ZB-027: the radio's details don't say the guard paused it");
    zb_has("Try again", "ZB-027: a paused radio can't be tried again from its details");
    shot("bl-144-radio-paused");
    tap_text("Try again");
    run(3300);
    zb_has("Working", "ZB-027: Try again didn't start the radio");
    tap_name("page_back");
    run(300);

    /* 8: the channel */
    zb_end();
    tap_link("Channel");
    run(300);
    zb_has("Busy", "ZB-006: a busy channel isn't said");
    zb_has("Quietest", "ZB-006: the quietest channel isn't marked");
    zb_has("Now", "ZB-006: the channel in use isn't marked");
    shot("bl-144-channel");
    tap_link("Channel 20");
    run(300);
    expect(find_label(lv_screen_active(), "Move to channel 20?") != NULL,
           "ZB-027: moving the channel doesn't ask first");
    shot("bl-144-channel-confirm");
    tap_name("dialog_ok");
    run(300);
    zb_has("Moving to channel 20: every device follows.", "ZB-027: the channel move isn't said");
    run(4200);
    tap_name("page_back");
    run(300);
    zb_top();
    expect(find_label_with(by_name("page_body"), "Channel 20 · ") != NULL,
           "ZB-027: the network didn't move to channel 20");

    /* 12: a zone with a Zigbee sensor (the zone source knows the demo's devices from here) */
    host_zigbee_demo_publish(HOST_ZB_PUBLISH_ZONES);
    run(300);
    scroll_to_text("page_body", "Front Door");
    tap_link("Front Door");
    run(300);
    scroll_to_text("page_body", "Zone 1 · Front Door");
    tap_link("Zone 1 · Front Door");
    run(300);
    zb_has("Zigbee · Aqara door sensor", "ZB-027: a Zigbee zone's source doesn't name Zigbee and its model");
    zb_has("Battery 92 % · Signal good · Heard 2 min ago",
           "ZB-027: a Zigbee zone doesn't give its device's battery, signal and when heard");
    zb_scroll_to_top_of("Its sensor");
    shot("bl-144-zone-edit");
    tap_link("Zigbee device");
    run(300);
    zb_has("Aqara MCCGQ11LM", "ZB-027: the zone's Zigbee device row doesn't open its page");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);

    /* 6: removing a device - a sleeping one waits for it to wake */
    zb_top();
    tap_link("Remove a device");
    run(300);
    expect(find_label(lv_screen_active(), "It is asked to leave the network") != NULL,
           "ZB-027: Remove a device doesn't offer the devices");
    shot("bl-144-remove-pick");
    tap_text("Front Door");
    run(300);
    expect(find_label(lv_screen_active(), "Remove Front Door?") != NULL, "ZB-027: removing doesn't ask first");
    expect(find_label_with(lv_screen_active(), "Zone 1 keeps its settings and needs a new sensor.") != NULL,
           "ZB-027: removing doesn't say the zone needs a new sensor");
    shot("bl-144-remove-confirm");
    tap_name("dialog_ok");
    run(20300);
    zb_has("Asked it to leave", "ZB-027: the remove page doesn't say it asked");
    zb_has("A sleeping sensor answers when it wakes: press its button now.",
           "ZB-027: removing a sleeping device doesn't say to wake it");
    expect(find_label_with(lv_screen_active(), "Waiting · 1 min 4") != NULL,
           "ZB-027: the remove page doesn't count down its wait");
    shot("bl-144-remove-asked");
    zb_state(HOST_ZB_REMOVE_NO_ANSWER);
    zb_has("It didn't answer", "ZB-027: no answer isn't said");
    zb_has("Front Door is still in the network", "ZB-027: no answer doesn't say it stays");
    zb_has("Forget it anyway", "ZB-027: no answer doesn't offer Forget");
    shot("bl-144-remove-no-answer");
    tap_text("Try again");
    run(1100);
    zb_has("Asked it to leave", "ZB-027: Try again didn't ask it again");
    zb_state(HOST_ZB_REMOVE_ANSWERS);
    zb_has("Removed: Front Door", "ZB-027: a removed device isn't said removed");
    zb_has("Zone 1 needs a new sensor: it shows CHECK until it has one.",
           "ZB-027: removing doesn't say what the zone needs");
    shot("bl-144-remove-done");
    tap_text("Done");
    run(300);
    zb_has("Radio · Working", "ZB-027: Done on the remove page isn't the Zigbee page");
    expect(find_label(by_name("page_body"), "Front Door") == NULL, "ZB-027: the removed device is still listed");

    /* 11: the zone editor's sensor picker - join a new device, then pick its input */
    tap_name("page_back");
    run(300);
    tap_text("Zones");
    run(300);
    tap_text("Add a zone");
    run(300);
    tap_text("Zigbee sensor");
    run(1100);
    expect(find_label(lv_screen_active(), "Add a new Zigbee device") != NULL,
           "ZB-002: the Zigbee sensor picker can't add a new device");
    tap_text("Add a new Zigbee device");
    run(1300);
    zb_has("Any Zigbee 3.0 device may join", "ZB-002: the picker's add doesn't listen");
    zb_state(HOST_ZB_JOIN_ONE);
    zb_has("Tap a ready device to pick its input.", "ZB-002: the picker's add doesn't say what next");
    tap_link("Aqara door sensor");
    run(300);
    host_zigbee_demo_publish(HOST_ZB_PUBLISH_NEWEST);
    run(1100);
    expect(find_label_with(lv_screen_active(), "Just added") != NULL,
           "ZB-002: back in the picker, the device just added isn't marked");
    expect(find_label(lv_screen_active(), "Add a new Zigbee device") != NULL,
           "ZB-002: the picker lost its add row");
    shot("bl-144-sensor-picker");
    zb_back_to("Leave installer mode");

    /* 9: reset the network - asked twice, then the installer code once more (ZB-017) */
    scroll_to_text("page_body", "Zigbee");
    tap_link("Zigbee");
    run(300);
    zb_end();
    tap_link("Reset the Zigbee network");
    run(300);
    expect(find_label(lv_screen_active(), "Reset the Zigbee network?") != NULL,
           "ZB-017: the reset doesn't ask first");
    expect(find_label_with(lv_screen_active(), "devices lose the panel") != NULL,
           "ZB-017: the reset doesn't say the devices lose the panel");
    shot("bl-144-reset");
    tap_name("dialog_ok");
    run(300);
    /* ZB-017: asked twice, as the Panel Manager asks, then the installer code */
    expect(find_label(lv_screen_active(), "Reset it now?") != NULL,
           "ZB-017: the reset doesn't ask a second time");
    expect(find_label(lv_screen_active(), "Enter the installer code once more") == NULL,
           "ZB-017: the reset asks for the code after one confirmation");
    shot("bl-144-reset-again");
    tap_name("dialog_ok");
    run(300);
    expect(find_label(lv_screen_active(), "Enter the installer code once more") != NULL,
           "ZB-017: the reset doesn't ask for the installer code again");
    shot("bl-144-reset-code");
    type("123456k"); /* the master's code: not the installer's */
    run(300);
    expect(find_label(lv_screen_active(), "Wrong code, try again") != NULL,
           "ZB-017: the reset took a code that isn't the installer's");
    /* ZB-017: the keypad's lockout ends the try with its own words, and nothing is reset */
    host_zigbee_demo_code_once(HOST_ZB_CODE_LOCKED);
    type("69420k");
    run(300);
    zb_toast("Too many wrong codes: try again later",
             "ZB-017: the reset's code during the keypad's lockout isn't said");
    expect(find_label(lv_screen_active(), "Enter the installer code once more") == NULL,
           "ZB-017: the reset's pad stays open during the keypad's lockout");
    expect(find_label_with(by_name("page_body"), "No devices yet") == NULL,
           "ZB-017: the network was reset during the keypad's lockout");
    run(4000);
    tap_link("Reset the Zigbee network");
    run(300);
    tap_name("dialog_ok");
    run(300);
    tap_name("dialog_ok");
    run(300);
    type("69420k");
    run(2500);
    zb_top();
    zb_has("No devices yet. Add one: most sensors join when you hold their button until their light "
           "blinks.",
           "ZB-017: the reset didn't start a new network");
    zb_has("None", "ZB-017: install codes still wait after the reset");
    zb_state(HOST_ZB_WORKING);
    walk_zigbee_roles();

    tap_name("page_back");
    run(300);
    walk_sirens(); /* BL-037: on the installer menu */
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);

    /* 13: the sensor radio trouble opens the Zigbee page; outside installer mode, after the
     * installer code (ZB-027: looking needs installer mode) */
    host_panel_trouble(ALARM_TRBL_SENSOR_RADIO, true);
    host_zigbee_demo_state(HOST_ZB_NOT_ANSWERING);
    run(1500);
    tap_name("bar_trouble");
    run(300);
    expect(find_label(lv_screen_active(), "Wireless sensors may not be heard · Tap to see the radio") != NULL,
           "ZB-027: the sensor radio trouble doesn't say it opens the radio's page");
    shot("bl-144-troubles");
    tap_text("Sensor radio problem");
    run(300);
    expect(find_label(by_name("page_body"), "Radio · Not answering") == NULL,
           "ZB-027: the sensor radio trouble shows the Zigbee page without the installer code");
    zb_toast("Enter the installer code to see the radio",
             "ZB-027: the sensor radio trouble doesn't say the radio needs the installer code");
    tap_text("Enter the installer code");
    run(300);
    type("69420k");
    run(600);
    zb_has("Radio · Not answering",
           "ZB-027: the sensor radio trouble doesn't open the Zigbee page after the code");
    expect(lv_subject_get_int(&subject_page_back) == 1,
           "ZB-027: the Zigbee page opened from the trouble has no way back");
    tap_name("page_back");
    run(300);
    expect(find_label(by_name("page_body"), "Leave installer mode") != NULL,
           "ZB-027: back from the trouble's Zigbee page isn't the installer menu");
    zb_state(HOST_ZB_WORKING);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    host_panel_trouble(ALARM_TRBL_SENSOR_RADIO, false);
    tap_name("nav_home");
    run(300);
}

/* ---- Installer > Zigbee over the panel's real Zigbee (BL-143 ZB-M6; docs/features/20 testing.md
 * §10, ZB-027) ----------------------------------------------------------------------------------
 * The same pages as walk_zigbee(), over zbz_panel and the fake radio's virtual devices
 * (host_zigbee_real.h) instead of the demo model: Installer > Zigbee with no devices, Add a device,
 * the window, the door contact and the motion sensor pressing their buttons and joining, their
 * setup reaching Ready, the door's page and a rename. Appended after every other shot: those keep
 * their numbers, and the demo's shots stay the demo's. */
static void zb_wait_for(const char *text, uint32_t ms, const char *what)
{
    for (uint32_t t = 0; t < ms && find_label_with(lv_screen_active(), text) == NULL; t += 100u) {
        run(100);
    }
    expect(find_label_with(lv_screen_active(), text) != NULL, what);
}

static void walk_zigbee_real(const ui_options_t *start)
{
    printf("Zigbee, the panel's own\n");
    host_panel_zigbee_use_real(true);
    host_panel_init(1789857660000LL + 12LL * 3600000LL); /* 10:41 in the morning */
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    scroll_to_text("page_body", "Zigbee");
    tap_link("Zigbee");
    run(1300);
    zb_has("Radio · Working", "ZB-027: the real Zigbee's radio isn't working");
    zb_has("No devices yet. Add one: most sensors join when you hold their button until their light "
           "blinks.",
           "ZB-027: the real Zigbee starts with devices");
    shot("zb-real-zigbee");

    tap_link("Add a device");
    run(1300);
    zb_has("Any Zigbee 3.0 device may join", "ZB-002: the real add isn't listening");
    expect(find_label_with(lv_screen_active(), "Listening · 2 min 5") != NULL,
           "ZB-002: the real add doesn't count down its 3 minutes");
    shot("zb-real-add-listening");
    /* the door contact presses its button 3 s after the window opened, the motion sensor 3 s later;
     * each is set up over the fake radio while it listens */
    zb_wait_for("1 device joined", 4000u, "ZB-027: no virtual device joined the real add");
    /* each setup answer comes 250 ms late (host_zigbee_real.c SETUP_STEP_MS): the door is named
     * once its setup reads its model, and by then the motion sensor has joined and is being set
     * up step by step - the shot shows both */
    zb_wait_for("Door sensor", 4000u, "ZB-027: the joined device's model wasn't read");
    expect(find_label_with(lv_screen_active(), "step") != NULL,
           "ZB-027: a joined device's setup isn't shown step by step");
    shot("zb-real-add-joined");
    zb_wait_for("2 devices joined", 8000u, "ZB-027: the second virtual device didn't join");
    for (uint32_t t = 0; t < 15000u && find_label_with(lv_screen_active(), "step") != NULL;
         t += 100u) {
        run(100);
    }
    expect(find_label_with(lv_screen_active(), "Ready") != NULL,
           "ZB-027: the joined devices' setup didn't reach Ready");
    expect(find_label_with(lv_screen_active(), "step") == NULL,
           "ZB-027: a joined device is still being set up");
    shot("zb-real-add-ready");
    tap_text("Stop");
    run(1300);
    zb_has("Radio · Working", "ZB-002: Stop didn't go back to the Zigbee page");
    zb_has("Channel 15 · 2 devices", "ZB-027: the real Zigbee doesn't count its two devices");

    /* the door contact's page, and a rename (ZB-021) */
    zb_top();
    scroll_to_text("page_body", "Door sensor");
    tap_link("Door sensor");
    run(1300);
    zb_has("SONOFF SNZB-04PR2", "ZB-027: the device page doesn't give the real model");
    zb_has("With the standard key", "ZB-027: the device page doesn't say how it joined");
    zb_has("Battery 92 %", "ZB-011: the device page doesn't give the battery it reported");
    shot("zb-real-device");
    tap_link("Name");
    run(300);
    sheet_text("Porch Door");
    tap_name("sheet_done");
    run(1300);
    expect(find_label_with(lv_screen_active(), "Porch Door") != NULL,
           "ZB-021: the rename didn't reach the real device's page");
    shot("zb-real-device-renamed");
    tap_name("page_back");
    run(1300);
    zb_top();
    zb_has("Porch Door", "ZB-021: the renamed device isn't listed by its name");

    zb_back_to("Leave installer mode");
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
    host_panel_zigbee_use_real(false); /* the demo again from the next host_panel_init() */
}

/* B008: installer mode held at another keypad: Home's state card says so, its shield drawn in the
 * 96 px icon font (the glyph check, OPT-005), and the bar's banner answers that the installer
 * works at that keypad. Only then is Home in installer mode: this keypad ends it when Settings is
 * left, and the Panel Manager holds it only inside one change. */
static void walk_installer_elsewhere(void)
{
    printf("Installer mode at another keypad\n");
    wake();
    tap_name("nav_home");
    run(4000); /* the last walk's toasts gone */
    expect(host_panel_keypad2_program(true) == ALARM_OK,
           "B008: installer mode at another keypad was refused");
    run(1500);
    expect(strcmp(lv_subject_get_string(&subject_headline), "Installer mode") == 0 &&
               find_label(lv_screen_active(), "Installer mode") != NULL,
           "B008: Home doesn't show installer mode entered elsewhere");
    expect(lv_subject_get_int(&subject_bar_banner) == 2,
           "B008: the status bar doesn't say the installer is working");
    shot("home_installer_mode_elsewhere");
    tap_name("program_banner");
    run(300);
    expect(lv_subject_get_int(&subject_toast_visible) == 1 &&
               strcmp(lv_subject_get_string(&subject_toast),
                      "The installer is working at another keypad") == 0,
           "B008: the banner doesn't say the installer works at another keypad");
    expect(lv_subject_get_int(&subject_bar_banner) == 2,
           "B008: a tap at this keypad ended the installer's mode elsewhere");
    host_panel_keypad2_program(false);
    run(1500);
    expect(strcmp(lv_subject_get_string(&subject_headline), "Installer mode") != 0 &&
               lv_subject_get_int(&subject_bar_banner) != 2,
           "B008: Home still shows installer mode after it ended elsewhere");
    run(4000);
}

/* BL-131 finding 7: Activity is the whole event log (LOG-007), with LOG-008's private step and
 * LOG-010's loss warning. Appended after every other shot, so the pictures before keep their
 * numbers; the names follow mockup-next's n-activity-* screens. */
static bool activity_shows_silent(void)
{
    lv_obj_t *list = by_name("event_list");
    return find_label_with(list, "Silent") != NULL;
}

static void activity_private_with(const char *keys)
{
    scroll_to_text("event_list", "Show private events");
    tap_text("Show private events");
    run(300);
    type(keys);
    run(300);
}

static void activity_to_end(void)
{
    lv_obj_t *list = by_name("event_list");
    lv_obj_update_layout(list);
    lv_obj_scroll_by_bounded(list, 0, -lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
    run(50);
}

/* The clock moves on a day at a time, the panel left alone. */
/* A visible label under root that starts with a short date and a comma ("Sep 20, 10:41 AM"):
 * whichever day the walk has reached by then (the automations walk before it moves the clock). */
static lv_obj_t *find_dated_label(lv_obj_t *root)
{
    if (root == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_label_class) && visible(root)) {
        const char *t = lv_label_get_text(root);
        size_t i = 4u;
        if (strlen(t) > 7u && isupper((unsigned char)t[0]) && islower((unsigned char)t[1]) &&
            islower((unsigned char)t[2]) && t[3] == ' ' && isdigit((unsigned char)t[4])) {
            while (isdigit((unsigned char)t[i])) {
                i++;
            }
            if (t[i] == ',' && t[i + 1u] == ' ') {
                return root;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = find_dated_label(lv_obj_get_child(root, (int32_t)i));
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

static void activity_days_pass(unsigned days)
{
    for (unsigned m = 0; m < days * 1440u; m++) {
        s_ms += 60000u;
        host_panel_advance(60000u);
    }
    run(1500);
    wake();
    run(300);
    tap_name("nav_activity");
    run(300);
}

static alarm_result_t activity_remote(alarm_cmd_type_t type)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_AWAY;
    cmd.arm_flags = ALARM_ARM_BYPASS_FAULTED;
    cmd.panic = ALARM_PANIC_SILENT;
    snprintf(cmd.code, sizeof(cmd.code), "%s", "123456");
    return host_panel_remote_command(&cmd);
}

static void walk_activity_log(void)
{
    printf("Activity: the whole event log, private events, the log's loss\n");
    wake();
    tap_name("nav_home");
    run(4000);
    /* A silent panic and its disarm: a silent event in the log (LOG-008). */
    expect(activity_remote(ALARM_CMD_PANIC) == ALARM_OK, "LOG-008: the silent panic was refused");
    run(1000);
    expect(activity_remote(ALARM_CMD_DISARM) == ALARM_OK, "LOG-008: disarming the panic failed");
    run(1000);
    activity_remote(ALARM_CMD_DISARM); /* clears the alarm memory */
    run(5000);
    host_panel_log_loss(12, 3); /* LOG-010: latched until the installer clears it */
    run(1200);

    tap_name("nav_activity");
    run(300);
    expect(!activity_shows_silent(), "LOG-008: a silent event shows on the public Activity list");
    expect(find_label(by_name("event_list"), "12 events couldn't be saved") == NULL,
           "LOG-010: the loss warning shows without a code");
    expect(find_label_with(lv_screen_active(), "Settings has the full event log") == NULL,
           "BL-131 finding 7: Activity still sends people to Settings for the log");
    activity_to_end();
    shot("activity_long");

    tap_text("Show private events");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "LOG-008: Show private events asks no code");
    shot("activity_private_ask");
    type("2468k"); /* a user's code: refused */
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1 && !activity_shows_silent(),
           "LOG-008: a user code showed the private events");
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0, "LOG-008: the master code was refused");
    expect(activity_shows_silent(), "LOG-008: a master code doesn't show the silent events");
    expect(find_label(by_name("event_list"), "Hide private events") != NULL,
           "LOG-008: the row doesn't offer to hide the private events");
    lv_obj_scroll_to_y(by_name("event_list"), 0, LV_ANIM_OFF);
    run(100);
    expect(find_label(by_name("event_list"), "12 events couldn't be saved") != NULL &&
               find_label(by_name("event_list"), "Clear the warning") == NULL,
           "LOG-010: a master sees no loss warning, or may clear it");
    shot("activity_loss_master");
    activity_to_end(); /* the row that hides them again */
    shot("activity_private");

    tap_text("Hide private events");
    run(300);
    expect(!activity_shows_silent(), "LOG-008: Hide private events left them shown");

    /* The installer's code clears the warning (docs/features/14). */
    activity_private_with("69420k");
    lv_obj_scroll_to_y(by_name("event_list"), 0, LV_ANIM_OFF);
    run(100);
    expect(find_label(by_name("event_list"), "Clear the warning") != NULL,
           "LOG-010: the installer can't clear the log's loss");
    shot("activity_loss");
    tap_text("Clear the warning");
    run(300);
    expect(find_label(by_name("event_list"), "12 events couldn't be saved") == NULL,
           "LOG-010: Clear the warning left the warning");

    /* Forgotten on leaving Activity ... */
    tap_name("nav_home");
    run(300);
    tap_name("nav_activity");
    run(300);
    expect(!activity_shows_silent(), "LOG-008: leaving Activity didn't forget the code");
    /* ... at the screen saver ... */
    activity_private_with("123456k");
    expect(activity_shows_silent(), "LOG-008: the master code no longer shows them");
    run(62000);
    wake();
    run(300);
    if (lv_subject_get_int(&subject_view) != 2) {
        tap_name("nav_activity");
        run(300);
    }
    expect(!activity_shows_silent(), "LOG-008: the screen saver didn't forget the code");
    /* ... and when an arming starts; while armed the row is dimmed (UI-022). */
    activity_private_with("123456k");
    expect(activity_remote(ALARM_CMD_ARM) == ALARM_OK, "UI-022: the house would not arm");
    run(1500);
    if (lv_subject_get_int(&subject_view) != 2) {
        tap_name("nav_activity");
        run(300);
    }
    expect(!activity_shows_silent(), "LOG-008: arming didn't hide the private events");
    expect(find_label(by_name("event_list"), "Once the system is disarmed") != NULL,
           "UI-022: the private step isn't dimmed while armed");
    activity_to_end();
    shot("activity_armed");
    tap_text("Show private events");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0,
           "UI-022: the dimmed private step opened the code pad while armed");
    expect(activity_remote(ALARM_CMD_DISARM) == ALARM_OK, "UI-022: disarming failed");
    run(5000);

    /* LOG-004: today's events say only their time; yesterday's "Yesterday, 10:41 AM"; older
     * ones the short date and the time ("Sep 20, 10:41 AM"). After the last shot. */
    expect(find_label(by_name("event_list"), "Yesterday, *") == NULL,
           "LOG-004: today's events say Yesterday");
    activity_days_pass(1);
    expect(find_label(by_name("event_list"), "Yesterday, *") != NULL,
           "LOG-004: yesterday's events don't say Yesterday with their time");
    activity_days_pass(1);
    expect(find_label(by_name("event_list"), "Yesterday, *") == NULL &&
               find_dated_label(by_name("event_list")) != NULL,
           "LOG-004: older events don't say their date and time");
    tap_name("nav_home");
    run(300);
}
#endif

#if ALARM_LV_BUDGET_KB == 0
/* ---- Settings > Automations (docs/features/41 U15, BL-168) ----------------------------------- */

/* Saturday 19 September 2026, 22:41 (the walk's clock, UTC): the outcomes' times of day. */
#define AUTO_T0     1789857660000LL
#define AUTO_AT(h, m) (AUTO_T0 - ((22LL * 60LL + 41LL) - ((h) * 60LL + (m))) * 60000LL)

static void auto_rule(auto_rule_t *r, uint8_t trig, uint16_t zone)
{
    memset(r, 0, sizeof(*r));
    r->when.kind = trig;
    r->when.zone = zone;
}

static auto_action_t *auto_act(auto_rule_t *r, uint8_t kind, uint8_t action, const char *ref,
                               const char *name)
{
    auto_action_t *a = &r->then[r->n++];
    memset(a, 0, sizeof(*a));
    a->kind = AUTO_ACT_DEVICE;
    a->source = DSRC_HA;
    a->dev_kind = kind;
    a->dev_action = action;
    a->value = -1;
    snprintf(a->ref, sizeof(a->ref), "%s", ref);
    snprintf(a->name, sizeof(a->name), "%s", name);
    return a;
}

/* Saved as the system master saves one, through the panel's own checks (AUT-002). */
static void auto_save_as_master(const auto_rule_t *r, const char *what)
{
    auto_check_t why = AUTO_OK;
    const auto_save_t res = host_panel_auto_save(r, ALARM_USER_MASTER, host_panel_auto_gen(), &why, NULL);
    if (res != AUTO_SAVE_OK) {
        fprintf(stderr, "automation '%s' not saved: %d, %d\n", what, (int)res, (int)why);
    }
    expect(res == AUTO_SAVE_OK, "AUT-002: the walk's automation wasn't saved");
}

static void auto_status_at(uint16_t uid, uint8_t result, uint8_t index, int64_t at)
{
    auto_status_t st;
    memset(&st, 0, sizeof(st));
    st.result = result;
    st.action_index = index;
    st.at_epoch_ms = at;
    host_panel_auto_force_status(uid, &st);
}

/* "When Basement Water detects water: Close Garden Water" (the house's valve). */
static void auto_leak_rule(auto_rule_t *r)
{
    auto_rule(r, AUTO_TRIG_ZONE_OPENED, 8);
    auto_act(r, DEV_VALVE, ACT_CLOSE, "valve.garden", "Garden Water");
}

/* Harbor House's list as the mockup draws it: the leak rule before the garage one, which stays
 * turned off. */
static void auto_house(void)
{
    auto_rule_t r[AUTO_MAX];
    expect(host_panel_auto_rules(r, AUTO_MAX) == 4u, "AUT-001: Harbor House doesn't start with four");
    const auto_rule_t garage = r[3];
    expect(host_panel_auto_delete_slot(4, host_panel_auto_gen()) == AUTO_SAVE_OK,
           "AUT-010: the garage rule wasn't deleted");
    auto_rule_t one;
    auto_leak_rule(&one);
    auto_save_as_master(&one, "leak");
    one = garage;
    one.uid = 0;
    one.slot = 0;
    auto_save_as_master(&one, "garage");
}

/* Settings > Automations: the line shows once BL-131 finding 7 took Event log out of the
 * column (DESIGN.md R4). */
static void auto_open_section(void)
{
    lv_obj_t *line = find_visible(lv_screen_active(), "set_automations");
    if (line == NULL) { /* a fresh panel (host_panel_init) is back on Home */
        tap_name("bar_settings");
        run(300);
        line = find_visible(lv_screen_active(), "set_automations");
    }
    expect(line != NULL, "R4: Settings shows no Automations line");
    lv_obj_send_event(line, LV_EVENT_CLICKED, NULL);
    run(1100);
}

static bool auto_note(const char *text)
{
    return strcmp(lv_subject_get_string(&subject_page_note), text) == 0;
}

static void auto_back_to_list(void)
{
    tap_name("page_back");
    run(1100);
}

static void walk_automations(const ui_options_t *start)
{
    printf("Automations\n");
    host_panel_init(AUTO_T0);
    host_panel_automations(true);
    ui_app_apply_options(start);
    auto_house();
    auto_status_at(1, AUTO_RES_DONE, 0, AUTO_AT(19, 42));
    auto_status_at(2, AUTO_RES_NO_ANSWER, 0, AUTO_AT(8, 2));
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    tap_name("bar_settings");
    run(300);

    /* AUT-010, D3: the system master's or the installer's code first (screen 6) */
    auto_open_section();
    expect(lv_subject_get_int(&subject_settings_pad) == 1,
           "AUT-010: Automations opened without the master or installer code");
    tap(lv_obj_find_by_name(by_name("settings_pad"), "pad_close"));
    run(300);
    expect(find_label(by_name("page_body"), "Enter the master or installer code") != NULL &&
               auto_note("Automations are changed with the master or installer code."),
           "AUT-010: the locked page doesn't ask for the master or installer code");
    shot("automations_locked");
    tap_text("Enter the master or installer code");
    run(300);
    type("123456k");
    run(1100);

    /* AUT-001: one sentence per automation, in slot order (screen 2) */
    expect(auto_note("5 of 16 automations"), "AUT-001: the list doesn't count its automations");
    lv_obj_t *body = by_name("page_body");
    expect(find_label(body, "When Front Door opens") != NULL &&
               find_label(body, "Turn on Porch Light for 10 minutes") != NULL &&
               find_label(body, "When the panel is armed Away") != NULL &&
               find_label(body, "Lock Front Door \xC2\xB7 Run Good Night") != NULL &&
               find_label(body, "When a fire or CO alarm sounds") != NULL &&
               find_label(body, "Turn off Hallway") != NULL &&
               find_label(body, "When Basement Water detects water") != NULL &&
               find_label(body, "At 11:00 PM on weekdays") != NULL &&
               find_label(body, "Close Garage Door") != NULL,
           "AUT-001: an automation's sentences are missing from the list");
    expect(find_label(body, "Didn't work at 8:02 AM") != NULL,
           "AUT-006: the list doesn't say the armed Away rule didn't work");
    expect(find_label(body, "Off") != NULL, "the turned-off garage rule doesn't say Off");
    shot("automations");

    /* AUT-008: saved elsewhere (the Panel Manager) - the list follows at the next tick */
    expect(host_panel_auto_set_off(3, true, host_panel_auto_gen()) == AUTO_SAVE_OK,
           "AUT-008: the fire rule wasn't turned off elsewhere");
    run(1100);
    expect(find_label(by_name("page_body"), "Off") != NULL &&
               lv_obj_get_child_count(by_name("page_body")) > 0u,
           "AUT-008: the list didn't follow a change made elsewhere");
    expect(host_panel_auto_set_off(3, false, host_panel_auto_gen()) == AUTO_SAVE_OK,
           "AUT-008: the fire rule wasn't turned on again elsewhere");
    run(1100);

    /* One automation (screen 16): its state on the switch, When, Only between, Then */
    tap_text("When Front Door opens");
    run(1100);
    body = by_name("page_body");
    expect(find_label(body, "On \xC2\xB7 ran at 7:42 PM") != NULL,
           "AUT-006: the page doesn't say when it ran");
    expect(find_label(body, "Zone 1") != NULL && find_label(body, "6:00 PM to 6:00 AM") != NULL &&
               find_label(body, "Turn on Porch Light") != NULL && find_label(body, "Outside") != NULL &&
               find_label(body, "10 minutes") != NULL && find_label(body, "Add a second action") != NULL &&
               find_label(body, "Try it now") != NULL,
           "AUT-001: the page's rows don't match the automation");
    shot("automation");
    /* the switch turns it off and on, through the store */
    lv_obj_t *sw = lv_obj_find_by_name(by_name("page_body"), "toggle");
    tap(sw);
    run(1100);
    expect(find_label(by_name("page_body"), "Off \xC2\xB7 it won't run") != NULL,
           "AUT-010: turning it off didn't say it won't run");
    tap(lv_obj_find_by_name(by_name("page_body"), "toggle"));
    run(1100);
    expect(find_label(by_name("page_body"), "On \xC2\xB7 ran at 7:42 PM") != NULL,
           "AUT-010: turning it on again didn't bring its state back");
    /* AUT-006: Try it now - its outcome in the switch's detail, no toast */
    host_panel_auto_force_status(1, NULL);
    tap_text("Try it now");
    run(1500);
    expect(find_label_with(by_name("page_body"), "tried at") != NULL,
           "AUT-006: Try it now's outcome isn't on the switch");
    auto_status_at(1, AUTO_RES_DONE, 0, AUTO_AT(19, 42));
    auto_back_to_list();

    /* Two actions (screen 17), then the failed one (18) */
    auto_status_at(2, AUTO_RES_DONE, 0, AUTO_T0 - 86400000LL - 3LL * 3600000LL);
    tap_text("When the panel is armed Away");
    run(1100);
    body = by_name("page_body");
    expect(find_label(body, "On \xC2\xB7 ran on Sep 18") != NULL,
           "AUT-006: the page doesn't say which day it ran");
    expect(find_label(body, "Once exit time is over") != NULL && find_label(body, "Lock Front Door") != NULL &&
               find_label(body, "Run Good Night") != NULL && find_label(body, "Add a second action") == NULL &&
               find_label(body, "Turn it off after") == NULL,
           "AUT-001: two actions: no second Add, no Turn it off after");
    shot("automation_two");
    /* UI-023: the outcome changes the switch's words in place, at the next tick */
    auto_status_at(2, AUTO_RES_NO_ANSWER, 0, AUTO_AT(8, 2));
    run(1100);
    expect(find_label(by_name("page_body"), "On \xC2\xB7 didn't work at 8:02 AM: Front Door didn't answer") != NULL,
           "AUT-006: the page doesn't say why its last run didn't work");
    shot("automation_failed");
    auto_back_to_list();

    /* AUT-008: an automation deleted elsewhere takes its page back to the list */
    tap_text("When Basement Water detects water");
    run(1100);
    expect(host_panel_auto_delete_slot(4, host_panel_auto_gen()) == AUTO_SAVE_OK,
           "AUT-008: the leak rule wasn't deleted elsewhere");
    run(1100);
    expect(lv_subject_get_int(&subject_page_back) == 0 && auto_note("4 of 16 automations"),
           "AUT-008: a deleted automation's page didn't go back to the list");

    /* The clock isn't set: times wait (screen 5, AUT-005) */
    host_panel_init(0);
    host_panel_automations(true);
    auto_house();
    run(500);
    auto_open_section();
    expect(find_label(by_name("page_body"), "The clock isn't set, so times wait until it is.") != NULL,
           "AUT-005: the list doesn't say times wait for the clock");
    shot("automations_clock");

    /* None yet (screen 3) */
    host_panel_init(AUTO_T0);
    host_panel_automations(false);
    run(500);
    auto_open_section();
    expect(auto_note("") &&
               find_label(by_name("page_body"),
                          "The panel can do things by itself, like turning on the porch light when the front door opens.") != NULL,
           "AUT-001: the empty list doesn't say what an automation is");
    shot("automations_empty");

    /* Rows with a problem (screen 6) and their pages (19, 20) */
    auto_rule_t r;
    auto_rule(&r, AUTO_TRIG_ZONE_OPENED, 1); /* its light has left the panel */
    r.when.win_from = 18u * 60u + 1u;
    r.when.win_to = 6u * 60u + 1u;
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.old_porch", "Porch Light")->off_after_min = 10;
    expect(host_panel_auto_inject(&r), "the missing device's rule wasn't stored");
    auto_rule(&r, AUTO_TRIG_DISARMED, 0); /* saved by newer software */
    r.rec = AUTO_REC_NEWER;
    expect(host_panel_auto_inject(&r), "the newer rule wasn't stored");
    auto_rule(&r, AUTO_TRIG_ZONE_OPENED, 6); /* a zone that can't start one now */
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.kitchen", "Kitchen Lights")->off_after_min = 5;
    expect(host_panel_auto_inject(&r), "the zone's rule wasn't stored");
    auto_leak_rule(&r);
    auto_save_as_master(&r, "leak");
    auto_status_at(1, AUTO_RES_MISSING, 0, AUTO_AT(21, 44));
    auto_open_section();
    body = by_name("page_body");
    expect(auto_note("4 of 16 automations") && find_label(body, "Porch Light isn't on the panel now") != NULL &&
               find_label(body, "Made by newer software") != NULL &&
               find_label(body, "Something this software can't show") != NULL &&
               find_label(body, "Can't run: open it to fix it") != NULL,
           "AUT-008: the rows with a problem don't say what it is");
    shot("automations_problems");
    tap_text("Porch Light isn't on the panel now");
    run(1100);
    expect(find_label(by_name("page_body"), "Missing: it was called Porch Light \xC2\xB7 pick another device") != NULL &&
               find_label(by_name("page_body"),
                          "On \xC2\xB7 didn't work at 9:44 PM: Porch Light isn't on the panel now") != NULL,
           "AUT-006: the page doesn't say its device is missing");
    shot("automation_missing");
    auto_back_to_list();
    tap_text("Made by newer software");
    run(1100);
    expect(find_label(by_name("page_body"), "Can't run: made by newer software") != NULL &&
               find_label(by_name("page_body"), "Try it now") == NULL &&
               find_label(by_name("page_body"), "Delete this automation") != NULL,
           "AUT-008: a newer automation's page isn't read-only with Delete");
    shot("automation_newer");
    auto_back_to_list();
    tap_text("When Kitchen Smoke is triggered");
    run(1100);
    expect(find_label(by_name("page_body"), "Can't run: Kitchen Smoke can't start an automation now") != NULL,
           "AUT-004: the page doesn't say its zone can't start an automation");
    shot("automation_cant_run");
    auto_back_to_list();

    /* 16, the most (screen 4) */
    host_panel_init(AUTO_T0);
    host_panel_automations(true);
    auto_house();
    auto_status_at(2, AUTO_RES_NO_ANSWER, 0, AUTO_AT(8, 2));
    auto_rule(&r, AUTO_TRIG_DISARMED, 0);
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.living_lamp", "Living Room Lamp");
    auto_save_as_master(&r, "disarmed");
    auto_rule(&r, AUTO_TRIG_ZONE_OPENED, 4);
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.kitchen", "Kitchen Lights")->off_after_min = 5;
    auto_save_as_master(&r, "back door");
    auto_rule(&r, AUTO_TRIG_ALARM, 0);
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.porch", "Porch Light");
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.kitchen", "Kitchen Lights");
    auto_save_as_master(&r, "alarm");
    auto_rule(&r, AUTO_TRIG_ZONE_OPENED, 5);
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.kitchen", "Kitchen Lights")->off_after_min = 5;
    auto_save_as_master(&r, "motion");
    auto_rule(&r, AUTO_TRIG_ARMED, 0);
    r.when.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT);
    auto_act(&r, DEV_LIGHT, ACT_OFF, "light.living_lamp", "Living Room Lamp");
    auto_act(&r, DEV_LOCK, ACT_LOCK, "lock.front_door", "Front Door");
    auto_save_as_master(&r, "night");
    auto_rule(&r, AUTO_TRIG_TIME, 0);
    r.when.minute = 17u * 60u;
    auto_act(&r, DEV_FAN, ACT_ON, "fan.porch", "Porch Fan");
    auto_save_as_master(&r, "5 pm");
    auto_rule(&r, AUTO_TRIG_TIME, 0);
    r.when.minute = 23u * 60u + 30u;
    auto_act(&r, DEV_FAN, ACT_OFF, "fan.porch", "Porch Fan");
    auto_save_as_master(&r, "11:30 pm");
    auto_rule(&r, AUTO_TRIG_ZONE_CLOSED, 1);
    auto_act(&r, DEV_LOCK, ACT_LOCK, "lock.front_door", "Front Door");
    auto_save_as_master(&r, "closes");
    auto_rule(&r, AUTO_TRIG_ARMED, 0);
    r.when.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_STAY);
    auto_act(&r, DEV_COVER, ACT_CLOSE, "cover.garage_door", "Garage Door");
    auto_save_as_master(&r, "stay");
    auto_rule(&r, AUTO_TRIG_ZONE_OPENED, 3);
    snprintf(auto_act(&r, DEV_CLIMATE, ACT_SET_MODE, "climate.hallway", "Hallway")->mode, DEV_MODE_LEN, "off");
    auto_save_as_master(&r, "window");
    auto_rule(&r, AUTO_TRIG_TIME, 0);
    r.when.minute = 6u * 60u + 30u;
    r.when.days = 0x1Fu;
    auto_act(&r, DEV_LIGHT, ACT_ON, "light.living_lamp", "Living Room Lamp");
    auto_save_as_master(&r, "6:30 am");
    auto_rule(&r, AUTO_TRIG_DISARMED, 0);
    auto_act(&r, DEV_LIGHT, ACT_OFF, "light.porch", "Porch Light");
    expect(host_panel_auto_save(&r, ALARM_USER_MASTER, host_panel_auto_gen(), NULL, NULL) == AUTO_SAVE_FULL,
           "AUT-001: a seventeenth automation was stored");
    run(500);
    auto_open_section();
    expect(auto_note("16 of 16 automations") &&
               find_label(by_name("page_body"), "16 is the most. Delete one to add another.") != NULL,
           "AUT-001: at 16 the list doesn't say it is full");
    shot("automations_full");
    tap_name("nav_home");
    run(4000);

    /* BL-170 C6 (docs/features/41 phase C), appended so every earlier picture keeps its number:
     * an arming automation's page and a disarming one's (drawings automation-arm-page,
     * automation-disarm-page), "What should it do?" with Disarm (automation-then-arm, without the
     * Arm row: its sheet waits for a layout part) and Disarm's confirm (automation-disarm-confirm) */
    host_panel_init(AUTO_T0);
    host_panel_automations(true);
    auto_house();
    auto_rule(&r, AUTO_TRIG_TIME, 0);
    r.when.minute = 22u * 60u + 30u;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5;
    r.then[0].value = -1;
    auto_save_as_master(&r, "arm night");
    auto_rule(&r, AUTO_TRIG_TIME, 0);
    r.when.minute = 10u * 60u;
    r.when.days = 0x1Fu;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_DISARM;
    r.then[0].partition = 1;
    r.then[0].value = -1;
    auto_act(&r, DEV_LOCK, ACT_UNLOCK, "lock.front_door", "Front Door");
    auto_save_as_master(&r, "disarm");
    auto_status_at(7, AUTO_RES_DONE, 0, AUTO_AT(22, 30));
    auto_status_at(8, AUTO_RES_DONE, 0, AUTO_AT(10, 0));
    run(500);
    tap_name("bar_settings");
    run(300);
    auto_open_section();
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(1100);
    }
    tap_text("At 10:30 PM every day");
    run(1100);
    body = by_name("page_body");
    expect(find_label(body, "Arm Night") != NULL &&
               find_label(body, "Set up by Master \xC2\xB7 warns 5 minutes before") != NULL &&
               find_label(body, "Try it now") == NULL && find_label(body, "Add a second action") == NULL,
           "D30, AUT-011: the arming automation's page isn't as drawn");
    shot("automation_arm_page");
    auto_back_to_list();
    /* DESIGN.md "Disarming" 5: the nudge shows once nothing arms the partition again */
    expect(host_panel_auto_delete_slot(6, host_panel_auto_gen()) == AUTO_SAVE_OK,
           "the arming automation wasn't deleted");
    run(1100);
    tap_text("At 10:00 AM on weekdays");
    run(1100);
    body = by_name("page_body");
    expect(find_label(body, "Disarm the panel") != NULL && find_label(body, "Set up by Master") != NULL &&
               find_label(body, "Nothing arms it again. Add one that arms in the evening.") != NULL,
           "D30: the disarming automation's page isn't as drawn");
    shot("automation_disarm_page");
    auto_back_to_list();
    tap_text("Add an automation");
    run(300);
    tap_text("Start from scratch");
    run(300);
    tap_text("When a door or window opens");
    run(300);
    tap_text("Front Door");
    run(300);
    expect(find_label(by_name("pick_list"), "Disarm the panel") != NULL,
           "D23: \"What should it do?\" doesn't offer Disarm after a door");
    shot("automation_then_disarm");
    tap_text("Disarm the panel");
    run(300);
    expect(strcmp(lv_subject_get_string(&subject_sheet_title), "Disarm the panel when Front Door opens?") == 0,
           "AUT-016: Disarm after a door doesn't ask its confirm");
    shot("automation_disarm_confirm");
    tap_name("dialog_cancel");
    run(300);

    /* BL-170 C6t2, appended: "Arm the panel" from the bedtime idea (drawings automation-arm,
     * -arm-bypass), an arming automation's page that bypasses, and Away with two partitions
     * (automation-arm-away) */
    tap_text("Add an automation");
    run(300);
    tap_text("Arm Night at bedtime");
    run(300);
    tap_name("time_ok");
    run(300);
    lv_obj_t *form = by_name("form_body");
    expect(find_label(form, "Warns at 10:25 PM, arms Night at 10:30 PM.") != NULL &&
               find_label(form, "5 minutes") != NULL && find_label(form, "Bypass open zones") != NULL,
           "AUT-012, D29: the arm sheet isn't as drawn (automation-arm)");
    shot("automation_arm");
    tap(lv_obj_find_by_name(lv_obj_get_parent(lv_obj_get_parent(find_label(form, "Bypass open zones"))), "toggle"));
    run(300);
    expect(find_label(form, "Warns at 10:25 PM, arms Night at 10:30 PM, bypassing open zones.") != NULL,
           "D29: the arm sheet doesn't say it bypasses (automation-arm-bypass)");
    shot("automation_arm_bypass");
    tap_name("form_ok");
    run(1100);
    tap_text("At 10:30 PM every day");
    run(1100);
    expect(find_label(by_name("page_body"),
                      "Set up by Master \xC2\xB7 warns 5 minutes before \xC2\xB7 bypasses open zones") != NULL,
           "D29, D30: the arming automation's page doesn't say it bypasses");
    shot("automation_arm_page_bypass");
    auto_back_to_list();
    ui_backend_t abe;
    host_panel_backend(&abe);
    alarm_config_t *one = (alarm_config_t *)malloc(sizeof(alarm_config_t));
    alarm_config_t *two = (alarm_config_t *)malloc(sizeof(alarm_config_t));
    expect(one != NULL && two != NULL && abe.get_config(abe.ctx, one), "the preview has no configuration to read");
    *two = *one;
    two->partitions[1].enabled = true;
    snprintf(two->partitions[1].name, ALARM_NAME_LEN, "Studio");
    two->users[ALARM_USER_MASTER - 1u].partitions = (uint8_t)(two->users[ALARM_USER_MASTER - 1u].partitions | 0x03u);
    expect(abe.set_config(abe.ctx, two) == ALARM_OK, "a second partition was refused");
    free(two);
    run(1100);
    tap_text("Add an automation");
    run(300);
    tap_text("Arm Night at bedtime");
    run(300);
    tap_name("time_ok");
    run(300);
    tap_name("level_away");
    run(300);
    form = by_name("form_body");
    expect(find_label(form, "Inside sensors will be on.") != NULL && find_label(form, "Partition") != NULL &&
               find_label(form, "Warns at 10:25 PM, arms Home Away at 10:30 PM.") != NULL,
           "AUT-011: the arm sheet with Away and two partitions isn't as drawn (automation-arm-away)");
    shot("automation_arm_away");
    tap_name("form_cancel");
    run(300);
    expect(abe.set_config(abe.ctx, one) == ALARM_OK, "one partition again was refused"); /* the walks after */
    free(one);
    /* ... and no bedtime arming in them: its 10:30 PM comes round in the Activity walk's days */
    auto_rule_t *rules = (auto_rule_t *)malloc(AUTO_MAX * sizeof(auto_rule_t));
    const size_t nrules = rules != NULL ? abe.auto_list(abe.ctx, rules, AUTO_MAX) : 0u;
    for (size_t i = 0; i < nrules; i++) {
        if (rules[i].n >= 1u && rules[i].then[0].kind == AUTO_ACT_ARM) {
            expect(host_panel_auto_delete_slot(rules[i].slot, host_panel_auto_gen()) == AUTO_SAVE_OK,
                   "the bedtime arming wasn't deleted");
        }
    }
    free(rules);
    run(1100);
    tap_name("nav_home");
    run(4000);
}

/* ---- An empty Zones filter says so (BL-177) ------------------------------------------------
 * The green check and "No open zones" / "No bypassed zones" / "No zone troubles" (zones_none)
 * take the list's place. */

static bool zones_none_shown(void)
{
    return find_visible(lv_screen_active(), "zones_none") != NULL;
}

static bool zone_list_shown(void)
{
    return find_visible(lv_screen_active(), "zone_list") != NULL;
}

/* These words are on screen now. */
static bool zones_says(const char *text)
{
    return find_label(lv_screen_active(), text) != NULL;
}

/* Zone z's card in the Zones list is on screen (the list itself shown too). */
static bool zone_card_shown(uint16_t z)
{
    lv_obj_t *list = lv_obj_find_by_name(lv_screen_active(), "zone_list");
    for (uint32_t i = 0; list != NULL && i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *card = lv_obj_get_child(list, (int32_t)i);
        if ((uintptr_t)lv_obj_get_user_data(card) == z) {
            return visible(card);
        }
    }
    return false;
}

/* A Zones filter's segment, tapped as a finger would: 0 All, 1 Open, 2 Bypassed, 3 Trouble. */
static void zone_filter_tap(int32_t filter)
{
    tap(lv_obj_get_child(by_name("zone_filters"), filter));
}

/* Every swap between the list and the check, timed: none may come within a second of the last
 * (UI-020, UI-023). */
static bool s_swap_watch;
static unsigned s_swaps;
static uint32_t s_swap_at;
static uint32_t s_swap_gap_min;

static void zones_swap_seen(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    if (!s_swap_watch) {
        return;
    }
    if (s_swaps > 0u && s_ms - s_swap_at < s_swap_gap_min) {
        s_swap_gap_min = s_ms - s_swap_at;
    }
    s_swaps++;
    s_swap_at = s_ms;
}

/* The Open filter on the Zones list with zone 3 open, zone 5 bypassed and zone 8's battery low:
 * the check comes only once the list has stayed empty for a second, a chattering contact right
 * after it never swaps the list and the check within a second, and a zone opening later shows its
 * card within that second. Bypassed and Trouble show their zones. Ends on All, zone 3 open. */
static void walk_zones_open_none(void)
{
    /* UI-020, UI-023 (BL-177) */
    zone_filter_tap(1);
    run(100);
    expect(zone_card_shown(3) && !zones_none_shown(),
           "BL-177: Open didn't show the open zone 3, or showed the empty check");
    lv_observer_t *obs = lv_subject_add_observer(&subject_zones_empty, zones_swap_seen, NULL);
    s_swaps = 0u;
    s_swap_gap_min = UINT32_MAX;
    s_swap_watch = true;
    host_panel_zone(3, ALARM_ZEV_RESTORE);
    const uint32_t closed_at = s_ms;
    run(300);
    expect(!zones_none_shown(), "BL-177: the empty check came before the list settled (UI-023)");
    for (uint32_t t = 0; t < 1500u && !zones_none_shown(); t += 10u) {
        run(10);
    }
    expect(zones_none_shown() && !zone_list_shown(),
           "BL-177: Open with no open zone didn't show the check in place of the list");
    expect(s_ms - closed_at >= 1000u,
           "BL-177: the empty check came less than a second after the last zone closed");
    expect(zones_says("No open zones") && !zones_says("No bypassed zones") &&
               !zones_says("No zone troubles"),
           "BL-177: the empty Open filter didn't say \"No open zones\" (and only that)");
    shot("zones_open_none");
    /* A chattering contact right after the check came: open 100 ms, closed 100 ms, for 3 s. */
    for (int i = 0; i < 15; i++) {
        host_panel_zone(3, ALARM_ZEV_FAULT);
        run(100);
        if (i == 0) {
            expect(zones_none_shown(),
                   "BL-177: a zone opening right after the check removed it within a second");
        }
        host_panel_zone(3, ALARM_ZEV_RESTORE);
        run(100);
    }
    expect(s_swap_gap_min >= 1000u && s_swaps <= 4u,
           "BL-177: a chattering contact swapped the list and the check within a second (UI-020)");
    run(2100); /* closed and settled: the check again */
    expect(zones_none_shown(), "BL-177: the check didn't come back once the contact settled closed");
    run(1100);
    host_panel_zone(3, ALARM_ZEV_FAULT); /* a second after the last swap: shown at once */
    run(100);
    expect(!zones_none_shown() && zone_card_shown(3),
           "BL-177: a zone opening didn't replace the settled check with its card at once");
    expect(s_swap_gap_min >= 1000u,
           "BL-177: the list and the check swapped within a second of each other (UI-020)");
    s_swap_watch = false;
    lv_observer_remove(obs);
    zone_filter_tap(2);
    run(100);
    expect(zone_card_shown(5) && !zones_none_shown(),
           "BL-177: Bypassed didn't show the bypassed zone 5, or showed the empty check");
    zone_filter_tap(3);
    run(100);
    expect(zone_card_shown(8) && !zones_none_shown(),
           "BL-177: Trouble didn't show zone 8's low battery, or showed the empty check");
    zone_filter_tap(0);
    run(100);
    expect(!zones_none_shown() && zone_list_shown(), "BL-177: All didn't show the list");
}

/* The quiet house (no zone bypassed or in trouble): Bypassed and Trouble say so at once, a tap
 * being no zone's change. Ends on Home, the filter on All. */
static void walk_zones_empty(void)
{
    tap_name("nav_zones");
    run(300);
    zone_filter_tap(2);
    expect(zones_none_shown() && !zone_list_shown() && zones_says("No bypassed zones") &&
               !zones_says("No open zones") && !zones_says("No zone troubles"),
           "BL-177: Bypassed with no bypassed zone didn't say \"No bypassed zones\" at once");
    shot("zones_bypassed_none");
    zone_filter_tap(3);
    expect(zones_none_shown() && zones_says("No zone troubles") &&
               !zones_says("No bypassed zones"),
           "BL-177: Trouble with no zone trouble didn't say \"No zone troubles\" at once");
    shot("zones_trouble_none");
    zone_filter_tap(0);
    expect(!zones_none_shown() && zone_list_shown(), "BL-177: All didn't bring the list back");
    tap_name("nav_home");
    run(300);
}

/* ---- An automation's arming warning at the wall (docs/features/41 phase C, C5) -------------
 * The approved screens (mockup screens/home.js): the banner and Home's card above
 * the tiles (home-arm-warning), not ready (-warning-not-ready), Put off's choice and code pad
 * (-put-off-choice, -put-off) and the quiet note after it (-put-off-home). */

/* The simulator moves on by ms; someone stands at the panel, so no screen saver. */
static void auto_arm_later(uint32_t ms)
{
    host_panel_advance(ms);
    s_ms += ms;
    lv_display_trigger_activity(NULL);
    run(1100);
}

static void walk_auto_arm(const ui_options_t *start)
{
    printf("Arming by automation: the warning at the wall\n");
    host_panel_init(AUTO_AT(22, 20));
    host_panel_automations(true);
    ui_app_apply_options(start);
    ui_backend_t b;
    host_panel_backend(&b);
    expect(b.user_set(b.ctx, "123456", 4, "4444", ALARM_AUTH_STANDARD, 0x01u, "Pat") == ALARM_OK,
           "C5: a user Pat wasn't added");
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 22u * 60u + 30u;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5;
    r.then[0].value = -1;
    auto_save_as_master(&r, "a Night arm at 10:30 PM");
    run(500);
    wake();
    tap_name("nav_home");
    run(300);

    auto_arm_later(6u * 60000u); /* 10:26 PM: the warning since 10:25 */
    expect(lv_subject_get_int(&subject_bar_banner) == 5 &&
               strcmp(lv_subject_get_string(&subject_arm_banner), "Arming Night at 10:30 PM") == 0,
           "AUT-012: no banner during the warning");
    expect(find_visible(lv_screen_active(), "arm_strip") != NULL &&
               find_visible(lv_screen_active(), "arm_strip_put_off") != NULL,
           "AUT-012, D26: no card with Put off above the tiles");
    shot("home_arm_warning");

    host_panel_zone(3, ALARM_ZEV_FAULT);
    run(1500);
    expect(find_visible(lv_screen_active(), "arm_strip_note") != NULL, "D28: no not-ready line");
    shot("home_arm_warning_not_ready");
    host_panel_zone(3, ALARM_ZEV_RESTORE);
    run(1500);

    tap_name("arm_strip_put_off");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 9 /* the put-off */, "D26: Put off didn't open the choice");
    shot("home_arm_put_off_choice");
    tap_name("poff_hour");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "D26: An hour later didn't ask for a code");
    type("44");
    run(300);
    shot("home_arm_put_off");
    type("44k");
    auto_arm_later(0u);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 && lv_subject_get_int(&subject_arm_strip) == 2 &&
               strcmp(lv_subject_get_string(&subject_arm_strip_title), "Arms Night at 11:30 PM") == 0 &&
               strcmp(lv_subject_get_string(&subject_arm_strip_detail), "Put off by Pat") == 0,
           "D27: no quiet note after the put-off");
    expect(lv_subject_get_int(&subject_bar_banner) != 5, "D20: the banner stayed after the put-off");
    shot("home_arm_put_off_home");
}
/* ---- BL-202: the zone's Spoken name, its editor and Screen and sound > Announcements ---------
 * (docs/features/08 voice-first-build.md D03, D06, D09; docs/features/07 UI-045). Appended after
 * every other walk, so the pictures before keep their numbers; the names follow the proposal's
 * bl-202-* drawings, on the real en-US voice (host_panel.c loads it). */

/* The row (or switch row) that holds a label with this text is dimmed (ui_row_disable). */
static bool voice_dimmed(lv_obj_t *root, const char *text)
{
    lv_obj_t *l = find_label(root, text);
    for (lv_obj_t *o = l; o != NULL && o != root; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_state(o, LV_STATE_DISABLED)) {
            return true;
        }
    }
    return false;
}

static bool voice_editor_open(void)
{
    return find_visible(lv_screen_active(), "form_body") != NULL &&
           find_label(by_name("form_body"), "Words, in the order they are said") != NULL;
}

/* Add word > Find a word, then the word found. */
static void voice_add_found(const char *query, const char *word)
{
    tap_text("Add word");
    run(200);
    tap_text("Find a word");
    run(200);
    sheet_text(query);
    tap_name("sheet_done");
    run(200);
    tap_text(word);
    run(200);
}

static void voice_installer_zone(const char *zone)
{
    wake();
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Zones");
    run(300);
    tap_text(zone);
    run(300);
}

/* The zone page drawn again (back to Zones, and Front Door again): nothing changed, nothing saved. */
static void voice_zone_again(void)
{
    tap_name("page_back");
    run(300);
    tap_text("Front Door");
    run(300);
}

/* The last Preview the voice was given since the log was cleared: played and then stopped before
 * its end (ui_backend_t.voice_preview_stop). */
static bool voice_preview_stopped(void)
{
    host_voice_log_t log[16];
    const unsigned n = host_panel_voice_log(log, 16u);
    for (unsigned i = n; i > 0u; i--) {
        if (log[i - 1u].speech && log[i - 1u].kind == (uint8_t)ANN_KIND_PREVIEW) {
            return log[i - 1u].cancelled && !log[i - 1u].completed;
        }
    }
    return false;
}

static void walk_voice(const ui_options_t *start)
{
    printf("Spoken names and Announcements (BL-202)\n");
    host_panel_init(AUTO_AT(10, 52));
    ui_options_t opt = *start;
    opt.voice_enabled = false;
    ui_app_apply_options(&opt);
    host_panel_voice_pretend(-1);
    run(500);

    /* Screen and sound: the link under Sound, Off as shipped (D06) */
    wake();
    tap_name("bar_settings");
    run(300);
    tap_name("set_display");
    run(300);
    lv_obj_t *body = by_name("page_body");
    expect(find_label(body, "Announcements") != NULL, "D06: Screen and sound has no Announcements row");
    {
        lv_obj_t *clicks = find_label(body, "Key clicks");
        lv_obj_t *link = find_label(body, "Announcements");
        lv_obj_t *screen = find_label(body, "Screen");
        lv_area_t a = {0}, b = {0}, c = {0};
        if (clicks != NULL && link != NULL && screen != NULL) {
            lv_obj_get_coords(clicks, &a);
            lv_obj_get_coords(link, &b);
            lv_obj_get_coords(screen, &c);
        }
        expect(a.y1 < b.y1 && b.y1 < c.y1, "D06: Announcements isn't under Sound, after Key clicks");
    }
    shot("voice_sound_link");
    tap_link("Announcements");
    run(300);
    body = by_name("page_body");
    expect(find_label(body, "The panel says what is happening, in words") != NULL,
           "D06: the Announcements page doesn't say what they are");
    expect(voice_dimmed(body, "Status messages") && voice_dimmed(body, "Zone chime"),
           "D06: the two kinds aren't dimmed while Announcements are off");
    /* Decided 2026-10-12: no Walk test switch (the walk test always speaks, WALK-016), and no
     * voice language or pack rows (Time and language sets the language). */
    expect(find_label(body, "Walk test") == NULL && find_label(body, "Voice language") == NULL &&
               find_label(body, "Voice pack") == NULL &&
               find_label_with(body, "English (United States)") == NULL,
           "D06: the page still shows a Walk test switch or the voice's language or pack");
    shot("voice_announcements_off");
    tap_text("Preview"); /* works with Announcements off, changes nothing */
    run(200);

    /* Turning on is refused without a ready voice, with the reason in a line (no toast) */
    host_panel_voice_pretend(UI_VOICE_NONE);
    tap_text("Announcements");
    run(300);
    body = by_name("page_body");
    expect(find_label(body, "Announcements cannot be turned on because the voice pack is not installed.") != NULL,
           "D06: turning Announcements on without a pack isn't refused with its reason");
    expect(find_label(body, "Needs a voice pack") != NULL, "D06: the page doesn't say a voice pack is needed");
    expect(voice_dimmed(body, "Preview"), "D06: Preview isn't dimmed without a pack");
    expect(lv_subject_get_int(&subject_toast_visible) == 0, "D06: the refusal is a toast");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(50);
    shot("voice_announcements_refused");
    host_panel_voice_pretend(UI_VOICE_UNAVAILABLE);
    tap_text("Announcements");
    run(300);
    body = by_name("page_body");
    expect(find_label(body, "Announcements cannot be turned on because voice is unavailable.") != NULL &&
               find_label(body, "Voice is unavailable.") != NULL,
           "D06: turning Announcements on with an unusable voice isn't refused with its reason");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(50);
    shot("voice_announcements_unavailable");
    host_panel_voice_pretend(-1);
    tap_text("Announcements");
    run(300);
    body = by_name("page_body");
    expect(find_label_with(body, "cannot be turned on") == NULL, "D06: the refusal stays after it was turned on");
    expect(!voice_dimmed(body, "Status messages"), "D06: the two kinds stay dimmed once on");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(50);
    shot("voice_announcements_on");
    tap_name("page_back");
    run(300);
    expect(strcmp(row_label("Announcements", "value_label"), "On") == 0,
           "D06: the Announcements row doesn't say On");

    /* The zone page: Spoken name right after Name; a new zone's words are chosen (D03) */
    voice_installer_zone("Front Door");
    body = by_name("page_body");
    {
        static const char *const order[] = {"Name", "Spoken name", "Type"};
        int32_t last_y = -1;
        for (size_t i = 0; i < 3u; i++) {
            lv_obj_t *l = find_label(body, order[i]);
            lv_area_t a = {0};
            if (l != NULL) {
                lv_obj_get_coords(l, &a);
            }
            expect(l != NULL && a.y1 > last_y, "D03: Spoken name isn't right after Name");
            last_y = a.y1;
        }
    }
    expect(strcmp(row_label("Spoken name", "value_label"), "Choose words") == 0,
           "D03: a zone without words doesn't say Choose words");
    shot("voice_zone_empty");

    /* The editor, empty: Preview dimmed; a lease while it is open */
    tap_link("Spoken name");
    run(300);
    expect(voice_editor_open(), "D03: Spoken name doesn't open the editor");
    expect(host_panel_voice_leases() == 1u, "D03: the editor holds no lease on the voice pack");
    body = by_name("form_body");
    expect(find_label(body, "No words yet. Add words, or use the zone name.") != NULL,
           "D03: the empty editor doesn't say how to start");
    expect(voice_dimmed(body, "Preview") && !voice_dimmed(body, "Add word") && !voice_dimmed(body, "Use zone name"),
           "D03: the empty editor's buttons aren't as drawn");
    expect(find_label(body, "Voice language: English (United States)") != NULL,
           "D09: the editor doesn't say the voice's language");
    shot("voice_editor_empty");

    /* Use zone name: the draft from the name */
    tap_text("Use zone name");
    run(300);
    body = by_name("form_body");
    expect(find_label(body, "Front") != NULL && find_label(body, "Door") != NULL,
           "D03: Use zone name didn't make Front Door's words");
    expect(!voice_dimmed(body, "Preview"), "D03: Preview is dimmed with words");
    shot("voice_editor_words");
    tap_text("Preview");
    run(200);

    /* A word's actions: the last can't move later (ui_pick_sheet_dim) */
    tap_text("Door");
    run(300);
    lv_obj_t *pick = by_name("pick_list");
    expect(find_label(lv_screen_active(), "Word 2 of 2") != NULL, "D03: a word's actions don't say which word");
    expect(voice_dimmed(pick, "Move later") && !voice_dimmed(pick, "Move earlier"),
           "D03: the last word's Move later isn't dimmed");
    shot("voice_word_actions");
    tap_text("Move earlier");
    run(300);
    expect(voice_editor_open(), "D03: a word's action doesn't come back to the editor");
    {
        lv_area_t a = {0}, b = {0};
        lv_obj_get_coords(find_label(by_name("form_body"), "Door"), &a);
        lv_obj_get_coords(find_label(by_name("form_body"), "Front"), &b);
        expect(a.y1 < b.y1, "D03: Move earlier didn't move the word");
    }
    tap_text("Door");
    run(300);
    tap_text("Remove");
    run(300);
    expect(find_label(by_name("form_body"), "Door") == NULL, "D03: Remove didn't remove the word");

    /* Add word: Find a word or the four groups (D09) */
    tap_text("Add word");
    run(300);
    pick = by_name("pick_list");
    expect(find_label(pick, "Find a word") != NULL && find_label(pick, "Position") != NULL &&
               find_label(pick, "Rooms and places") != NULL && find_label(pick, "Things") != NULL &&
               find_label(pick, "Numbers") != NULL,
           "D09: Add word doesn't offer Find a word and the four groups");
    expect(find_label(pick, "Front, Back, Upstairs...") != NULL &&
               find_label(pick, "One to Nine, Ten, Twenty, Hundred...") != NULL,
           "D09: the groups don't show their words");
    shot("voice_add_word");
    tap_text("Rooms and places");
    run(300);
    expect(find_label(by_name("pick_list"), "Garage") != NULL && find_label(by_name("pick_list"), "B") != NULL,
           "D09: a group isn't its words under their letters");
    shot("voice_add_group");
    tap_text("Garage");
    run(300);
    expect(voice_editor_open() && find_label(by_name("form_body"), "Garage") != NULL,
           "D09: a word picked from a group isn't added");

    /* Find a word: the submitted search, its matches, all words 64 a page, no match */
    tap_text("Add word");
    run(200);
    tap_text("Find a word");
    run(300);
    expect(find_label(lv_screen_active(), "Enter the start of a word, then tap Search.") != NULL,
           "D03: Find a word doesn't say how to search");
    sheet_text("gar");
    shot("voice_find_word");
    tap_name("sheet_done");
    run(300);
    expect(find_label(lv_screen_active(), "gar: 2 matches") != NULL && find_label(by_name("pick_list"), "Garden") != NULL,
           "D03: Find a word doesn't list the matches with their count");
    shot("voice_find_results");
    tap_name("pick_close");
    run(300);
    expect(voice_editor_open(), "D03: Cancel on the matches doesn't go back to the editor");
    tap_text("Add word");
    run(200);
    tap_text("Find a word");
    run(200);
    sheet_text("");
    tap_name("sheet_done");
    run(300);
    expect(find_label(lv_screen_active(), "All words: 64 of 221, page 1 of 4") != NULL,
           "D03: an empty search doesn't list all words, 64 a page");
    expect(voice_dimmed(by_name("pick_list"), "Previous page") && !voice_dimmed(by_name("pick_list"), "Next page"),
           "D03: page 1's Previous isn't dimmed, or its Next is");
    shot("voice_find_paged");
    tap_text("Next page");
    run(300);
    expect(find_label(lv_screen_active(), "All words: 64 of 221, page 2 of 4") != NULL, "D03: Next page didn't page");
    tap_name("pick_close");
    run(300);
    tap_text("Add word");
    run(200);
    tap_text("Find a word");
    run(200);
    sheet_text("zz");
    tap_name("sheet_done");
    run(300);
    expect(find_label(lv_screen_active(), "No words match \"zz\".") != NULL, "D03: no match isn't said");
    shot("voice_find_none");
    tap_name("pick_close");
    run(300);
    tap_text("Add word");
    run(200);
    tap_text("Find a word");
    run(200);
    tap_name("sheet_cancel");
    run(300);
    expect(voice_editor_open(), "D03: Cancel on Find a word doesn't go back to the editor");

    /* Cancel discards the draft and gives the lease back */
    tap_name("form_cancel");
    run(300);
    expect(host_panel_voice_leases() == 0u, "D03: the editor kept its lease after Cancel");
    expect(strcmp(row_label("Spoken name", "value_label"), "Choose words") == 0, "D03: Cancel kept the draft");

    /* Eight words: Add word dimmed; over 10 s: never previewed or approved */
    tap_link("Spoken name");
    run(300);
    tap_text("Use zone name");
    run(200);
    static const char *const more[][2] = {{"up", "Upstairs"}, {"back", "Back"}, {"bedr", "Bedroom"},
                                          {"wind", "Window"}, {"one", "One"}, {"hund", "Hundred"}};
    for (size_t i = 0; i < 6u; i++) {
        voice_add_found(more[i][0], more[i][1]);
    }
    body = by_name("form_body");
    expect(find_label(body, "Eight words is the most.") != NULL && voice_dimmed(body, "Add word"),
           "D03: eight words don't dim Add word with its reason");
    lv_obj_scroll_to_y(by_name("form_body"), LV_COORD_MAX, LV_ANIM_OFF); /* the lines under the buttons */
    run(50);
    shot("voice_editor_full");
    tap_name("form_cancel");
    run(300);
    host_panel_voice_word_gap(1000u); /* eight words over 10 s: the voice's words are all short */
    tap_link("Spoken name");
    run(300);
    for (size_t i = 0; i < 8u; i++) {
        voice_add_found("sump", "Sump pump");
    }
    body = by_name("form_body");
    expect(find_label(body, "This name is too long to say. Remove a word.") != NULL && voice_dimmed(body, "Preview"),
           "D02: a draft over 10 s isn't said to be too long, or can be previewed");
    lv_obj_scroll_to_y(by_name("form_body"), LV_COORD_MAX, LV_ANIM_OFF); /* the lines under the buttons */
    run(50);
    shot("voice_editor_long");
    tap_name("form_ok"); /* saved as a draft, never approved */
    run(300);
    expect(host_panel_voice_leases() == 0u, "D03: the editor kept its lease after Done");
    expect(strcmp(row_label("Spoken name", "value_label"), "Review spoken name") == 0,
           "D07: a draft too long to say looks approved");
    tap_name("page_back");
    run(300);
    tap_text("Front Door");
    run(300);
    expect(strcmp(row_label("Spoken name", "value_label"), "Review spoken name") == 0,
           "D07: a draft too long to say was approved");
    host_panel_voice_word_gap(0u); /* the pack's own gap again */

    /* Done approves with the save (D07): the row then shows the words */
    tap_link("Spoken name");
    run(300);
    for (int i = 0; i < 8; i++) {
        tap_text("Sump pump");
        run(200);
        tap_text("Remove");
        run(200);
    }
    tap_text("Use zone name");
    run(200);
    tap_name("form_ok");
    run(300);
    tap_name("page_back"); /* the save, then the approval */
    run(300);
    tap_text("Front Door");
    run(300);
    expect(strcmp(row_label("Spoken name", "value_label"), "Front Door") == 0,
           "D07: Done and the save didn't approve the words");
    shot("voice_zone_chosen");

    /* Use zone name with a part the voice can't say: the draft stays, the part is named */
    tap_link("Name");
    run(200);
    sheet_text("Front Door 2nd floor");
    tap_name("sheet_done");
    run(300);
    expect(strcmp(row_label("Spoken name", "value_label"), "Review spoken name") == 0 &&
               strcmp(row_label("Spoken name", "detail_label"), "The name, partition or sensor changed") == 0,
           "D01: a new name doesn't ask for the words to be looked at again");
    shot("voice_zone_review");
    tap_link("Spoken name");
    run(300);
    tap_text("Use zone name");
    run(300);
    expect(find_label(by_name("form_body"),
                      "The panel can’t say \"2nd\". The words stay as they are. Choose words yourself, or rename the zone.") != NULL,
           "D03: Use zone name doesn't name the part it can't say");
    expect(find_label(by_name("form_body"), "Front") != NULL, "D03: Use zone name changed the draft it couldn't make");
    shot("voice_editor_unmatched");
    tap_name("form_cancel");
    run(300);
    tap_link("Name");
    run(200);
    sheet_text("Front Door");
    tap_name("sheet_done");
    run(300);
    expect(strcmp(row_label("Spoken name", "value_label"), "Front Door") == 0,
           "D01: the name back as it was doesn't show the approved words");

    /* Announcements off, no pack, a pack that can't be used: the words stay (D03) */
    opt.voice_enabled = false;
    ui_app_apply_options(&opt);
    voice_zone_again();
    expect(strcmp(row_label("Spoken name", "value_label"), "Announcements are off") == 0 &&
               strcmp(row_label("Spoken name", "detail_label"), "Front Door") == 0,
           "D03: the row doesn't say Announcements are off, with the words");
    shot("voice_zone_off");
    host_panel_voice_pretend(UI_VOICE_NONE);
    voice_zone_again();
    expect(strcmp(row_label("Spoken name", "value_label"), "Voice pack not installed") == 0,
           "D03: the row doesn't say the pack isn't installed");
    shot("voice_zone_nopack");
    tap_link("Spoken name");
    run(300);
    body = by_name("form_body");
    expect(host_panel_voice_leases() == 0u, "D03: a lease without a ready voice");
    expect(voice_dimmed(body, "Add word") && voice_dimmed(body, "Use zone name") && voice_dimmed(body, "Preview") &&
               find_label(body, "Voice pack not installed.") != NULL,
           "D03: the editor without a pack isn't as drawn");
    lv_obj_scroll_to_y(by_name("form_body"), LV_COORD_MAX, LV_ANIM_OFF); /* the lines under the buttons */
    run(50);
    shot("voice_editor_nopack");
    tap_name("form_cancel");
    run(300);
    host_panel_voice_pretend(UI_VOICE_UNAVAILABLE);
    voice_zone_again();
    expect(strcmp(row_label("Spoken name", "value_label"), "Voice is unavailable") == 0,
           "D03: the row doesn't say the voice is unavailable");
    shot("voice_zone_unavailable");
    host_panel_voice_pretend(-1);
    voice_zone_again();

    /* Cancel stops the Preview the editor played, at once (code review finding 2; D03) */
    tap_link("Spoken name");
    run(300);
    tap_text("Use zone name");
    run(200);
    host_panel_voice_log_clear();
    tap_text("Preview");
    run(50);
    tap_name("form_cancel");
    run(50);
    expect(voice_preview_stopped(), "D03: Cancel didn't stop the editor's Preview");
    expect(host_panel_voice_leases() == 0u, "D03: the editor kept its lease after Cancel");

    /* The remote banner's question replacing the editor in one call ends it: its lease back, its
     * draft gone (finding 4; paused already, so the question takes the editor's place at once) */
    {
        ui_backend_t b;
        host_panel_backend(&b);
        if (b.remote_set != NULL) {
            tap_link("Spoken name");
            run(300);
            expect(host_panel_voice_leases() == 1u, "D03: the editor holds no lease");
            ui_remote_session_t rs;
            memset(&rs, 0, sizeof(rs));
            rs.active = true;
            rs.paused = true;
            snprintf(rs.who, sizeof(rs.who), "Alex");
            snprintf(rs.where, sizeof(rs.where), "198.51.100.23");
            (void)b.remote_set(b.ctx, true, false);
            ui_app_remote_session(&rs);
            run(300);
            expect(voice_editor_open(), "D03: a remote session closed the editor");
            /* the banner's own press (its action), wherever the sheet's backdrop lies */
            lv_obj_send_event(by_name("remote_banner"), LV_EVENT_CLICKED, NULL);
            run(300);
            expect(find_label(lv_screen_active(), "Allow") != NULL, "the remote banner didn't ask over the editor");
            expect(host_panel_voice_leases() == 0u, "D03: the remote banner's question left the editor's lease held");
            tap_name("dialog_cancel"); /* Allow */
            run(300);
            expect(!voice_editor_open(), "D03: the editor came back after the remote banner's question");
            memset(&rs, 0, sizeof(rs));
            ui_app_remote_session(&rs);
            (void)b.remote_set(b.ctx, false, false);
            run(300);
        }
    }

    /* A language change with the editor open ends it first: closed, its lease back (finding 5) */
    tap_link("Spoken name");
    run(300);
    tap_text("Use zone name");
    run(200);
    expect(host_panel_voice_leases() == 1u, "D03: the editor holds no lease");
    {
        ui_options_t lang = opt;
        host_panel_lang_greek(true); /* a language every walk has: el-GR, its words English */
        snprintf(lang.lang, sizeof(lang.lang), "%s", "el-GR");
        ui_app_apply_options(&lang);
        run(300);
        expect(lv_subject_get_int(&subject_sheet) == 0, "I18N-014: the editor stayed open through a language change");
        expect(host_panel_voice_leases() == 0u, "D03: the editor kept its lease through a language change");
        ui_app_apply_options(&opt);
        host_panel_lang_greek(false);
        run(300);
    }
    expect(strcmp(row_label("Spoken name", "detail_label"), "Front Door") == 0, /* Announcements are off here */
           "D03: a language change with the editor open changed the zone's words");

    /* Installer mode ending under the editor closes it, its draft dropped, its lease given back:
     * nothing can be armed while it is open (UI-045) */
    tap_link("Spoken name");
    run(300);
    tap_text("Use zone name");
    run(200);
    expect(host_panel_voice_leases() == 1u, "D03: the editor holds no lease");
    {
        ui_backend_t b;
        host_panel_backend(&b);
        b.program_exit(b.ctx);
    }
    run(1500);
    expect(!voice_editor_open() && lv_subject_get_int(&subject_sheet) == 0,
           "UI-045: the editor stays open after installer mode ended");
    expect(host_panel_voice_leases() == 0u, "UI-045: the editor kept its lease after installer mode ended");
    tap_name("page_back");
    run(300);
    tap_name("nav_home");
    run(300);

    /* Announcements follows a pack installed while it is open, within a second (finding 6; D06) */
    host_panel_voice_pretend(UI_VOICE_NONE);
    wake();
    tap_name("bar_settings");
    run(300);
    tap_name("set_display");
    run(300);
    tap_link("Announcements");
    run(300);
    body = by_name("page_body");
    expect(find_label(body, "Needs a voice pack") != NULL && voice_dimmed(body, "Preview"),
           "D06: Announcements without a pack isn't as drawn");
    host_panel_voice_pretend(-1); /* the pack becomes ready */
    run(1000);
    body = by_name("page_body");
    expect(find_label(body, "Needs a voice pack") == NULL &&
               find_label(body, "The panel says what is happening, in words") != NULL && !voice_dimmed(body, "Preview"),
           "D06: Announcements didn't show a pack that became ready while it was open");
    /* Its Preview stops as the page is left (finding 2) */
    host_panel_voice_log_clear();
    tap_text("Preview");
    run(50);
    tap_name("page_back");
    run(50);
    expect(voice_preview_stopped(), "D06: leaving Announcements didn't stop its Preview");
    tap_name("nav_home");
    run(300);
}

#endif

#if ALARM_LV_BUDGET_KB == 0
/* The open keyboard has a key that shows this text. */
static bool kb_has_key(const char *text)
{
    lv_obj_t *kb = by_name("sheet_kb");
    for (uint32_t i = 0; kb != NULL && i < 64u; i++) {
        const char *k = lv_buttonmatrix_get_button_text(kb, i);
        if (k == NULL) {
            break;
        }
        if (strcmp(k, text) == 0) {
            return true;
        }
    }
    return false;
}

/* The letters pack (docs/features/39 asset-packs.md "Decisions / The letters", "The panel"): the
 * walk so far ran with it, as a panel the Flasher set up. Here it is taken away at run time
 * (the generation changes, as when a pack is installed while the panel runs): a letter outside the
 * image's set draws as the placeholder box (OPT-010, I18N-011) and the keyboard has no àé key, the
 * space bar in its place (OPT-014, I18N-019; the proposal approved 2026-10-10). Then the pack comes
 * back and so does the key. Appended after every other shot: those keep their numbers. */
static void walk_without_letters(const ui_options_t *start)
{
    printf("Without the letters pack\n");
    host_panel_init(AUTO_AT(10, 50));
    ui_app_apply_options(start);
    run(500);
    /* Zoë, named in the Panel Manager or on a panel with the pack: her ë is a box without it. */
    ui_backend_t b;
    host_panel_backend(&b);
    expect(b.user_set(b.ctx, "123456", 3, "3579", ALARM_AUTH_STANDARD, 0x01u, "Zo\xC3\xAB") == ALARM_OK,
           "OPT-010: a user Zoë wasn't added");
    /* ë and ł: letters of the pack, not of the shipped catalogs (i18n_chars.txt keeps é, ñ, ü) */
    lv_font_t **const text_fonts[] = {&font_caption, &font_body, &font_label, &font_title};
    lv_font_glyph_dsc_t g;
    for (size_t f = 0; f < sizeof(text_fonts) / sizeof(text_fonts[0]); f++) {
        expect(lv_font_get_glyph_dsc(*text_fonts[f], &g, 0xEBu, 0) &&
                   lv_font_get_glyph_dsc(*text_fonts[f], &g, 0x142u, 0),
               "OPT-010: a text font doesn't reach the letters pack's letters");
    }
    host_panel_letters(false);
    run(1200); /* the screen looks at the generation once a second */
    for (size_t f = 0; f < sizeof(text_fonts) / sizeof(text_fonts[0]); f++) {
        expect(!lv_font_get_glyph_dsc(*text_fonts[f], &g, 0xEBu, 0) &&
                   !lv_font_get_glyph_dsc(*text_fonts[f], &g, 0x142u, 0),
               "OPT-010: a text font draws a letter of the pack without it");
        expect(lv_font_get_glyph_dsc(*text_fonts[f], &g, 'e', 0) && g.adv_w > 0,
               "OPT-010: a text font lost its own letters with the pack");
    }

    wake();
    tap_name("bar_settings");
    tap_name("set_users");
    type("123456k");
    run(300);
    expect(find_label(by_name("page_body"), "Zo\xC3\xAB") != NULL,
           "OPT-010: the accented name isn't listed without the letters pack");
    shot("users_without_letters");
    tap_text("Zo\xC3\xAB");
    run(300);
    tap_text("Name");
    run(200);
    expect(!kb_has_key("\xC3\xA0\xC3\xA9") && kb_has_key("space") && kb_has_key("123"),
           "OPT-014: the keyboard offers the accent pages without the letters pack");
    shot("keyboard_without_letters");
    press_key("\xEE\x87\xA4"); /* shift: the capitals have no àé key either */
    expect(!kb_has_key("\xC3\xA0\xC3\xA9") && kb_has_key("Q"),
           "OPT-014: the capitals offer the accent pages without the letters pack");
    tap_name("sheet_cancel");
    run(300);

    /* The pack loaded again (installed while the panel runs): the letters draw and the key is
     * back the next time a keyboard opens. */
    host_panel_letters(true);
    run(1200);
    expect(lv_font_get_glyph_dsc(font_body, &g, 0xEBu, 0),
           "OPT-010: the letters pack installed while the panel runs isn't taken up");
    tap_text("Name");
    run(200);
    expect(kb_has_key("\xC3\xA0\xC3\xA9") && kb_has_key("space"),
           "OPT-014: the àé key isn't back with the letters pack");
    tap_name("sheet_cancel");
    run(300);
    tap_name("nav_home");
    run(300);

    /* A language's own font sits between the built-in font and the letters (I18N-049, OPT-010):
     * the pretend el-GR's Greek body font, and the letters behind it. */
    host_panel_lang_world(true);
    host_panel_lang_greek(true);
    stored_language("el-GR");
    run(1200);
    expect(lv_font_get_glyph_dsc(font_body, &g, 0x3A9u, 0) && lv_font_get_glyph_dsc(font_body, &g, 0xEBu, 0),
           "OPT-010: the letters pack isn't behind a language's own font");
    host_panel_letters(false);
    run(1200);
    expect(lv_font_get_glyph_dsc(font_body, &g, 0x3A9u, 0) && !lv_font_get_glyph_dsc(font_body, &g, 0xEBu, 0),
           "OPT-010: a language's font lost its letters, or kept the pack's, as the pack went");
    host_panel_letters(true);
    stored_language("en-US");
    host_panel_lang_greek(false);
    host_panel_lang_world(false);
    run(1200);
}
#endif

/* ---- BL-188: an update's progress, Stop, the status bar's banner and the last result ---------- */

static ui_fw_status_t fw_run(uint8_t stage)
{
    ui_fw_status_t st = {
        .running = "0.6.0", .latest = "0.6.1", .available = true, .has_source = true, .percent = -1,
        .release = true, .stage = stage, .target = "0.6.1", .previous = "0.6.0",
    };
    switch (stage) {
    case UI_FW_STAGE_DOWNLOADING: st.phase = UI_FW_DOWNLOADING; break;
    case UI_FW_STAGE_WAITING: st.phase = UI_FW_WAITING; break;
    case UI_FW_STAGE_INSTALLING: st.phase = UI_FW_RESTARTING; break;
    default: break;
    }
    return st;
}

/* Presets the update, lets the page and the bar follow (they poll once a second), takes the shot. */
static void fw_shot(const ui_fw_status_t *st, const char *name)
{
    host_panel_fw_preset(st);
    run(1500);
    shot(name);
}

static void fw_open_page(void)
{
    tap_name("bar_settings");
    run(300);
    tap_name("set_about");
    run(300);
    tap_text("Software update");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(1200);
    }
}

/* ---- BL-186: Installer > Updates, the update server and the channel ---------------------------- */

static bool page_row_disabled(const char *title)
{
    lv_obj_t *body = by_name("page_body");
    lv_obj_t *row = find_label(body, title);
    if (row == NULL) {
        return false;
    }
    while (lv_obj_get_parent(row) != body) {
        row = lv_obj_get_parent(row);
    }
    return lv_obj_has_state(row, LV_STATE_DISABLED);
}

static void hold_row(const char *text)
{
    scroll_to_text("page_body", text);
    lv_obj_t *hold = find_label(lv_screen_active(), text);
    expect(hold != NULL, "BL-186: no hold row");
    if (hold == NULL) {
        return;
    }
    lv_area_t ha;
    lv_obj_get_coords(lv_obj_get_parent(hold), &ha);
    s_px = (ha.x1 + ha.x2) / 2;
    s_py = (ha.y1 + ha.y2) / 2;
    s_pressed = true;
    run(2300);
    s_pressed = false;
    run(500);
}

/* Types a domain into the server page's text sheet and asks. */
static void fwsrv_check(const char *domain)
{
    scroll_to_text("page_body", "Change the update server");
    tap_text("Change the update server");
    run(300);
    sheet_text(domain);
    tap_name("sheet_done");
    run(300);
}

static void update_server_shots(void)
{
    printf("BL-186: Installer > Updates, the update server and the channel\n");
    ui_backend_t be;
    host_panel_backend(&be);
    ui_fw_status_t st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.available = false;
    host_panel_fw_preset(&st);
    host_panel_fw_server_preset("roboalarms.com", true, true);
    run(1500);

    /* the menu: Updates after This panel, before Test and start over */
    scroll_to_text("page_body", "Test and start over");
    lv_obj_t *body = by_name("page_body");
    lv_area_t a1, a2, a3;
    lv_obj_t *l_dev = find_label(body, "Developer mode");
    lv_obj_t *l_up = find_label(body, "Updates");
    lv_obj_t *l_test = find_label(body, "Test and start over");
    expect(l_dev != NULL && l_up != NULL && l_test != NULL, "BL-186: the Updates group is missing");
    if (l_dev != NULL && l_up != NULL && l_test != NULL) {
        lv_obj_get_coords(l_dev, &a1);
        lv_obj_get_coords(l_up, &a2);
        lv_obj_get_coords(l_test, &a3);
        expect(a1.y1 < a2.y1 && a2.y1 < a3.y1, "BL-186: Updates is not between This panel and Test and start over");
    }
    expect(find_label(body, "Update server") != NULL && find_label(body, "roboalarms.com") != NULL &&
               find_label(body, "Where updates come from") != NULL && find_label(body, "Stable") != NULL,
           "BL-186: the Updates rows lack their words");
    shot("fwsrv_menu");

    /* the factory installer code: Change is dimmed, with the reason */
    tap_text("Update server");
    run(300);
    expect(find_label(by_name("page_body"), "Change the installer code first: the factory one is public") != NULL &&
               page_row_disabled("Change the update server"),
           "BL-186: the factory-code state doesn't dim Change");
    shot("fwsrv_factory_code");
    tap_name("page_back");
    run(300);

    /* the installer code changed: the page */
    expect(be.program_set_code(be.ctx, ALARM_USER_INSTALLER, "7351") == ALARM_OK, "BL-186: the installer code wouldn't change");
    run(500);
    scroll_to_text("page_body", "Update server");
    tap_text("Update server");
    run(300);
    expect(find_label(by_name("page_body"), "The RoboAlarms update server") != NULL &&
               find_label(by_name("page_body"), "RoboAlarms release") != NULL &&
               find_label(by_name("page_body"), "3F9A 1C2B 77D0 E415") != NULL &&
               !page_row_disabled("Change the update server") &&
               find_label_with(by_name("page_body"), "https://") == NULL,
           "BL-186: the server page lacks the domain, the key or Change");
    shot("fwsrv_server");

    /* typing: an empty field, then a domain */
    tap_text("Change the update server");
    run(300);
    expect(strcmp(lv_subject_get_string(&subject_sheet_ok), "Check") == 0 &&
               find_label_with(lv_screen_active(), "The domain only, like roboalarms.com") != NULL,
           "BL-186: the text sheet lacks its note or Check");
    sheet_text("example.net");
    shot("fwsrv_type");
    sheet_text("");
    tap_name("sheet_done");
    run(300);
    expect(strcmp(lv_subject_get_string(&subject_toast), "Type a domain, like roboalarms.com") == 0,
           "BL-186: an empty field doesn't say what to type");
    fwsrv_check("localhost");
    expect(strcmp(lv_subject_get_string(&subject_toast), "Type a domain, like roboalarms.com") == 0 &&
               find_label(by_name("page_body"), "RoboAlarms release") != NULL,
           "BL-186: a non-domain started a check");
    fwsrv_check("example.net");
    run(300);
    expect(find_label_with(by_name("page_body"), "Checking example.net") != NULL &&
               find_label(by_name("page_body"), "Change the update server") == NULL,
           "BL-186: the checking row is missing");
    shot("fwsrv_checking");
    run(3000);
    expect(find_label_with(lv_screen_active(), "Use example.net for updates?") != NULL &&
               find_label(by_name("page_body"), "Hold to use this server") != NULL &&
               find_label(by_name("page_body"), "8C41 0E9D 52A7 B3F6") != NULL,
           "BL-186: no warning for a key the panel doesn't trust");
    shot("fwsrv_warning");

    /* the factory code is back while the installer reads the warning: the hold is refused */
    expect(be.program_set_code(be.ctx, ALARM_USER_INSTALLER, "69420") == ALARM_OK, "BL-186: the code wouldn't go back");
    run(500);
    hold_row("Hold to use this server");
    expect(strcmp(lv_subject_get_string(&subject_toast), "Change the installer code first") == 0 &&
               find_label(by_name("page_body"), "The RoboAlarms update server") != NULL,
           "BL-186: the factory code didn't refuse the new server");
    shot("fwsrv_refused_factory");
    expect(be.program_set_code(be.ctx, ALARM_USER_INSTALLER, "7351") == ALARM_OK, "BL-186: the code wouldn't change");
    run(500);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Update server");
    tap_text("Update server");
    run(300);

    /* back from the warning forgets the check */
    fwsrv_check("example.net");
    run(3500);
    tap_name("page_back");
    run(300);
    expect(find_label(by_name("page_body"), "The RoboAlarms update server") != NULL, "BL-186: back from the warning");

    /* the three refusals */
    fwsrv_check("example.org");
    run(3500);
    expect(find_label_with(by_name("page_body"), "example.org isn't an update server") != NULL, "BL-186: not a server");
    shot("fwsrv_not_a_server");
    fwsrv_check("example.com");
    run(3500);
    expect(find_label_with(by_name("page_body"), "example.com has no firmware for this panel") != NULL, "BL-186: no firmware");
    shot("fwsrv_no_firmware");
    fwsrv_check("builds.example.co.uk");
    run(3500);
    expect(find_label_with(by_name("page_body"), "builds.example.co.uk can't be reached") != NULL, "BL-186: unreachable");
    shot("fwsrv_unreachable");

    /* going back during a check forgets it */
    fwsrv_check("example.net");
    run(300);
    tap_name("page_back");
    run(3500);
    expect(find_label(by_name("page_body"), "Update channel") != NULL, "BL-186: leaving during a check");

    /* the warning, held: the server changes */
    scroll_to_text("page_body", "Update server");
    tap_text("Update server");
    run(300);
    fwsrv_check("example.net");
    run(3500);
    hold_row("Hold to use this server");
    expect(find_label(by_name("page_body"), "Not the RoboAlarms server") != NULL &&
               find_label(by_name("page_body"), "Go back to roboalarms.com") != NULL,
           "BL-186: the new server isn't in use");
    shot("fwsrv_server_other");
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Update channel");
    expect(find_label(by_name("page_body"), "Not the RoboAlarms server") != NULL, "BL-186: the menu row isn't amber");
    shot("fwsrv_menu_other");

    /* going back: the same check, the usual question for a trusted key */
    tap_text("Update server");
    run(300);
    tap_text("Go back to roboalarms.com");
    run(3500);
    expect(find_label_with(lv_screen_active(), "Use roboalarms.com for updates?") != NULL &&
               lv_subject_get_int(&subject_sheet) != 0,
           "BL-186: a trusted key didn't ask the usual question");
    shot("fwsrv_review");
    tap_text("Use it");
    run(500);
    expect(find_label(by_name("page_body"), "The RoboAlarms update server") != NULL, "BL-186: going back didn't switch");

    /* RoboAlarms' key new to a panel that runs another project's image: the warning, with its key */
    host_panel_fw_server_preset("example.net", false, false);
    tap_name("page_back");
    run(300);
    tap_text("Update server");
    run(300);
    tap_text("Go back to roboalarms.com");
    run(3500);
    expect(find_label_with(lv_screen_active(), "Use roboalarms.com for updates?") != NULL &&
               find_label(by_name("page_body"), "3F9A 1C2B 77D0 E415") != NULL,
           "BL-186: no warning for RoboAlarms' key when it is new");
    shot("fwsrv_warning_robo");
    tap_name("page_back");
    run(300);
    host_panel_fw_server_preset("roboalarms.com", true, true);
    tap_name("page_back");
    run(300);

    /* the channel */
    scroll_to_text("page_body", "Update channel");
    tap_text("Update channel");
    run(300);
    expect(find_label(lv_screen_active(), "Tested the longest. Right for most homes") != NULL &&
               find_label(lv_screen_active(), "Special builds for developers") != NULL,
           "BL-186: the channel list lacks its details");
    shot("fwsrv_channel");
    tap_text("Beta");
    run(500);
    expect(strcmp(row_label("Update channel", "value_label"), "Beta") == 0, "BL-186: the channel didn't change");
    shot("fwsrv_menu_beta");

    /* held while an update runs */
    st = fw_run(UI_FW_STAGE_DOWNLOADING);
    st.percent = 42;
    st.size = 2831155u;
    st.bytes = 1153434u;
    host_panel_fw_preset(&st);
    run(2500);
    expect(page_row_disabled("Update server") && page_row_disabled("Update channel") &&
               find_label(by_name("page_body"), "Not while an update runs") != NULL,
           "BL-186: the Updates rows aren't held while an update runs");
    shot("fwsrv_held");
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.available = false;
    host_panel_fw_preset(&st);
    run(2500);
    expect(!page_row_disabled("Update server"), "BL-186: the rows stayed held after the run");
    expect(be.program_set_code(be.ctx, ALARM_USER_INSTALLER, "69420") == ALARM_OK, "BL-186: the factory code wouldn't return");
    run(500);
}

static void update_progress_shots(void)
{
    printf("BL-188: an update's progress, Stop, the banner, the last result\n");
    ui_fw_status_t st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    host_panel_fw_preset(&st);
    run(1500);

    /* the question: blue Install, says it can be stopped */
    tap_text("Install version 0.6.1");
    run(300);
    expect((find_label_with(lv_screen_active(), "you can stop the update") != NULL), "BL-188: the install question doesn't say it can be stopped");
    shot("upd_install_question");
    tap_name("dialog_cancel");
    run(300);

    /* a plain check is not a run */
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_CHECKING;
    fw_shot(&st, "upd_plain_check");
    expect(lv_subject_get_int(&subject_bar_banner) < 6, "BL-188: a plain check shows the update banner");
    expect(find_label(by_name("page_body"), "Steps") == NULL, "BL-188: a plain check shows the steps");

    /* connecting, no size yet */
    st = fw_run(UI_FW_STAGE_DOWNLOADING);
    st.percent = 0;
    fw_shot(&st, "upd_connecting");
    expect((find_label(lv_screen_active(), "Starting the download") != NULL), "BL-188: connecting doesn't say it starts");
    expect(lv_subject_get_int(&subject_bar_banner) == 6, "BL-188: no banner while connecting");
    expect(strcmp(lv_subject_get_string(&subject_update_banner), "Downloading an update") == 0,
           "BL-188: the banner's words while downloading");

    /* downloading at 42 % */
    st.bytes = 1153434u;
    st.size = 2831155u;
    st.percent = 42;
    fw_shot(&st, "upd_downloading");
    expect(find_label_with(by_name("page_body"), "of 2.7 MB") != NULL &&
               find_label_with(by_name("page_body"), "42 %") != NULL,
           "BL-188: the download's size and percent are not shown");
    lv_obj_t *level = lv_obj_find_by_name(by_name("page_body"), "level");
    expect(level != NULL && !lv_obj_has_flag(level, LV_OBJ_FLAG_HIDDEN) && lv_bar_get_value(level) == 42,
           "BL-188: the level bar doesn't show 42");
    expect(find_label(by_name("page_body"), "Stop the update") != NULL, "BL-188: no Stop while downloading");
    expect(find_label(by_name("page_body"), "Check again") == NULL, "BL-188: Check again while it runs");
    st.bytes = 1239000u;
    st.percent = 43;
    host_panel_fw_preset(&st);
    run(1500);
    level = lv_obj_find_by_name(by_name("page_body"), "level");
    expect(lv_bar_get_value(level) == 43, "BL-188: the bar doesn't follow the percent");

    /* the Stop question */
    tap_text("Stop the update");
    run(300);
    expect((find_label(lv_screen_active(), "Stop the update?") != NULL) && (find_label(lv_screen_active(), "Keep updating") != NULL),
           "BL-188: the Stop question lacks its words");
    shot("upd_stop_question");
    tap_name("dialog_cancel");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 0, "BL-188: Keep updating didn't close the question");

    /* the run ends under the question: it closes */
    tap_text("Stop the update");
    run(300);
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    host_panel_fw_preset(&st);
    run(1500);
    expect(lv_subject_get_int(&subject_sheet) == 0, "BL-188: the Stop question stayed after the run ended");

    /* Stop with nothing running */
    st = fw_run(UI_FW_STAGE_DOWNLOADING);
    st.percent = 42;
    st.size = 2831155u;
    st.bytes = 1153434u;
    host_panel_fw_preset(&st);
    run(1500);
    tap_text("Stop the update");
    run(300);
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    host_panel_fw_preset(&st); /* it ended, but the question is not closed yet in this instant */
    tap_name("dialog_ok");
    run(300);

    /* Stop for real */
    st = fw_run(UI_FW_STAGE_DOWNLOADING);
    st.percent = 42;
    st.size = 2831155u;
    st.bytes = 1153434u;
    host_panel_fw_preset(&st);
    run(1500);
    tap_text("Stop the update");
    run(300);
    tap_name("dialog_ok");
    run(1500);
    expect((find_label(lv_screen_active(), "Stopping the update") != NULL), "BL-188: Stop doesn't say it is stopping");
    expect(find_label(by_name("page_body"), "Stop the update") == NULL, "BL-188: Stop offered while stopping");
    shot("upd_stopping");
    expect(strcmp(lv_subject_get_string(&subject_update_banner), "Stopping the update") == 0,
           "BL-188: the banner doesn't say it is stopping");

    /* stopped */
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.last.result = UI_FW_LAST_STOPPED;
    snprintf(st.last.version, sizeof(st.last.version), "0.6.1");
    snprintf(st.last.by, sizeof(st.last.by), "Alex");
    fw_shot(&st, "upd_stopped");
    expect(find_label_with(by_name("page_body"), "Stopped by Alex") != NULL, "BL-188: Stopped doesn't say by whom");
    expect(lv_subject_get_int(&subject_bar_banner) < 6, "BL-188: the banner stayed after the stop");
    expect(find_label(by_name("page_body"), "Install version 0.6.1") != NULL, "BL-188: no Install after a stop");

    /* checking the download */
    st = fw_run(UI_FW_STAGE_VERIFYING);
    st.phase = UI_FW_DOWNLOADING;
    st.percent = 100;
    st.size = 2831155u;
    st.bytes = 2831155u;
    fw_shot(&st, "upd_checking");

    /* waiting: disarmed (installer mode), armed, an alarm, a walk test, trying again */
    st = fw_run(UI_FW_STAGE_WAITING);
    st.size = 2831155u;
    snprintf(st.reason, sizeof(st.reason), "Installer mode is on");
    fw_shot(&st, "upd_waiting");
    expect(strcmp(lv_subject_get_string(&subject_update_banner), "Update waiting · installs once disarmed") == 0,
           "BL-188: the waiting banner's words");
    snprintf(st.reason, sizeof(st.reason), "The system is armed");
    fw_shot(&st, "upd_waiting_armed");
    snprintf(st.reason, sizeof(st.reason), "Trying again shortly");
    fw_shot(&st, "upd_trying_again");

    /* the restart, going back */
    st = fw_run(UI_FW_STAGE_INSTALLING);
    fw_shot(&st, "upd_restarting");
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.confirming = true;
    snprintf(st.running, sizeof(st.running), "0.6.1");
    st.available = false;
    fw_shot(&st, "upd_self_test");
    expect(strcmp(lv_subject_get_string(&subject_update_banner), "Self-test after the update") == 0,
           "BL-188: the self-test banner's words");
    st = fw_run(UI_FW_STAGE_ROLLING_BACK);
    snprintf(st.running, sizeof(st.running), "0.6.1");
    st.available = false;
    fw_shot(&st, "upd_going_back");
    expect(lv_subject_get_int(&subject_bar_banner) == 7, "BL-188: going back is not the amber banner");

    /* the last result */
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_FAILED;
    st.failed_install = true;
    snprintf(st.reason, sizeof(st.reason), "The download broke off");
    fw_shot(&st, "upd_failed");
    expect(lv_subject_get_int(&subject_bar_banner) < 6, "BL-188: the banner stayed after a failure");
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.available = false;
    snprintf(st.running, sizeof(st.running), "0.6.1");
    st.last.result = UI_FW_LAST_UPDATED;
    snprintf(st.last.version, sizeof(st.last.version), "0.6.1");
    st.last.at = (uint32_t)(1789857660000LL / 1000) - 3600u;
    fw_shot(&st, "upd_updated");
    expect(find_label_with(by_name("page_body"), "Updated to version 0.6.1") != NULL, "BL-188: Updated isn't told");
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.last.result = UI_FW_LAST_ROLLED_BACK;
    snprintf(st.last.version, sizeof(st.last.version), "0.6.1");
    snprintf(st.rolled_back, sizeof(st.rolled_back), "0.6.1");
    fw_shot(&st, "upd_rolled_back");
    expect(find_label(by_name("page_body"), "Install version 0.6.1") != NULL, "BL-188: no Install after a rollback");

    /* the Wi-Fi module and a developer's file: their own words, no steps, no Stop */
    st = fw_run(UI_FW_STAGE_WIFI_MODULE);
    st.release = false;
    st.percent = 42;
    fw_shot(&st, "upd_wifi_module");
    expect(find_label(by_name("page_body"), "Stop the update") == NULL &&
               find_label(by_name("page_body"), "Steps") == NULL,
           "BL-188: the Wi-Fi module's update has steps or Stop");
    expect(strcmp(lv_subject_get_string(&subject_update_banner), "Updating the Wi-Fi module") == 0,
           "BL-188: the Wi-Fi module's banner words");
    st = fw_run(UI_FW_STAGE_FILE);
    st.release = false;
    st.percent = 42;
    fw_shot(&st, "upd_file");
    expect(lv_subject_get_int(&subject_bar_banner) < 6, "BL-188: a developer's file has the update banner");

    /* About's row and the Settings list's tag while a run goes */
    st = fw_run(UI_FW_STAGE_DOWNLOADING);
    st.percent = 42;
    st.size = 2831155u;
    st.bytes = 1153434u;
    host_panel_fw_preset(&st);
    run(1500);
    tap_name("page_back");
    run(500);
    /* the code was given in this visit: the row shows the step and the percent (OTA-022) */
    expect(find_label(by_name("page_body"), "Downloading version 0.6.1") != NULL &&
               find_label(by_name("page_body"), "42 %") != NULL && find_label(by_name("page_body"), "Updating") == NULL,
           "BL-188: About's row doesn't show the version and percent after the code");
    shot("about_update_running_master");
    st.bytes = 1239000u;
    st.percent = 43;
    host_panel_fw_preset(&st);
    run(1500);
    expect(find_label(by_name("page_body"), "43 %") != NULL, "BL-188: About's percent doesn't follow the run");
    /* a visit without the code: only "Updating" */
    tap_name("nav_home");
    run(300);
    tap_name("bar_settings");
    run(300);
    tap_name("set_about");
    run(500);
    expect(find_label(by_name("page_body"), "Updating") != NULL &&
               find_label_with(by_name("page_body"), "Downloading version") == NULL &&
               find_label(by_name("page_body"), "42 %") == NULL && find_label(by_name("page_body"), "43 %") == NULL,
           "BL-188: About's row doesn't say Updating, and only that, without the code");
    expect(find_visible(lv_screen_active(), "set_about_tag_running") != NULL,
           "BL-188: the Settings list's tag doesn't say Updating");
    shot("about_update_running");

    /* Home: the banner; armed: Update waiting; an alarm has the screen */
    tap_name("nav_home");
    run(500);
    shot("home_update_banner");
    expect(lv_subject_get_int(&subject_bar_banner) == 6, "BL-188: Home has no update banner");
    expect_bar_fits("home_update_banner");
    st = fw_run(UI_FW_STAGE_INSTALLING);
    fw_shot(&st, "home_update_restarting");
    st = fw_run(UI_FW_STAGE_ROLLING_BACK);
    fw_shot(&st, "home_update_going_back");
    expect_bar_fits("home_update_going_back");
    st = fw_run(UI_FW_STAGE_WAITING);
    snprintf(st.reason, sizeof(st.reason), "The system is armed");
    host_panel_fw_preset(&st);
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_AWAY;
    snprintf(cmd.code, sizeof(cmd.code), "%s", "123456");
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "BL-188: the house would not arm");
    /* arming in the seconds before the restart puts it off: exit time runs under "Update waiting" */
    run(2000);
    shot("home_update_given_up");
    expect(lv_subject_get_int(&subject_bar_banner) == 6,
           "BL-188: exit time has no 'Update waiting' banner");
    run(68000);
    shot("home_update_armed");
    expect(lv_subject_get_int(&subject_bar_banner) == 6, "BL-188: Home, armed, has no 'Update waiting' banner");
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(500);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(40000);
    expect(lv_subject_get_int(&subject_bar_banner) != 6, "BL-188: the update banner stayed over an alarm");
    shot("home_update_alarm");
    cmd.type = ALARM_CMD_DISARM;
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "BL-188: the house would not disarm");
    run(5000);

    /* back to the page the next test expects */
    st = fw_run(UI_FW_STAGE_NONE);
    st.phase = UI_FW_IDLE;
    st.latest[0] = '\0';
    st.available = false;
    st.rolled_back[0] = '\0';
    snprintf(st.latest, sizeof(st.latest), "0.6.0");
    snprintf(st.rolled_back, sizeof(st.rolled_back), "0.6.1");
    host_panel_fw_preset(&st);
    run(1500);
    fw_open_page();
}

/* ---- BL-048: the panel link (docs/features/11; host_link_demo.h) ------------------------------
 * The setup on a panel's first start with the role and a keypad's pairing, a keypad's Home,
 * Emergency, Settings and About, and the alarm panel's Keypads and devices with adding, a keypad's
 * page and removing it. Shots are named as the proposal's screens (BL-048-panel-link.js). */
#if ALARM_LV_BUDGET_KB == 0
static void link_state(host_link_demo_t what)
{
    host_link_demo_state(what);
    run(1100); /* the screen reads the link once a second (UI-027) */
}

static void link_has(const char *text, const char *what)
{
    expect(find_label(lv_screen_active(), text) != NULL ||
               find_label_with(lv_screen_active(), text) != NULL,
           what);
}

static uint8_t link_last_kind(void)
{
    ui_link_req_t r;
    host_link_demo_last_request(&r);
    return r.kind;
}

static bool link_tile_dimmed(void)
{
    lv_obj_t *tile = lv_obj_get_child(by_name("panel_arm"), 0);
    return tile != NULL && lv_obj_has_state(tile, LV_STATE_DISABLED);
}

/* Press the hold button that shows this text and keep the finger there for ms (the caller lets
 * go: s_pressed = false). */
static void link_hold(const char *text, uint32_t ms)
{
    lv_obj_t *label = find_label(lv_screen_active(), text);
    expect(label != NULL, "BL-048: no hold button to press");
    if (label == NULL) {
        return;
    }
    scroll_into_view(lv_obj_get_parent(label));
    run(50);
    lv_area_t a;
    lv_obj_get_coords(lv_obj_get_parent(label), &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(ms);
}

static void walk_link_setup(const ui_options_t *start)
{
    printf("The panel link: the setup's role and a keypad's pairing (BL-048)\n");
    host_panel_init(1789857660000LL); /* 10:41 PM */
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    link_state(HOST_LINK_FRESH);
    host_panel_forget_wifi();
    ui_app_first_setup();
    run(300);
    past_the_language();
    expect(find_label(by_name("wiz_body"), "Choose its role") != NULL,
           "BL-048: Welcome on a first start doesn't list the role");
    expect(lv_subject_get_int(&subject_wiz_steps) == 6,
           "BL-048: the alarm panel's setup with the role doesn't count six steps");
    expect(find_visible(lv_screen_active(), "wiz_exit") == NULL,
           "BL-048: a first start offers Leave setup");
    shot("bl-048-setup-welcome");
    tap_name("wiz_next");
    run(300);
    expect_step(1, "BL-048: the role is the setup's first counted step");
    link_has("The alarm panel", "BL-048: the role step has no alarm panel row");
    shot("bl-048-setup-role");
    tap_text("A keypad");
    run(300);
    expect(lv_subject_get_int(&subject_wiz_steps) == 4,
           "BL-048: a keypad's setup doesn't count four steps");
    expect_step(1, "BL-048: choosing a keypad moved the setup on");
    shot("bl-048-setup-role-keypad");
    tap_name("wiz_next");
    run(300);
    expect_step(2, "LINK-053: nearby enrollment is the second step");
    link_has("Find your alarm panel", "LINK-053: missing nearby discovery");
    link_has("Select Wi-Fi", "LINK-053: missing manual fallback before discovery");
    shot("bl-048-radio-start");
    tap_name("wiz_next");
    run(300);
    expect_step(3, "BL-048: the pairing isn't a keypad's third step");
    expect(link_last_kind() == UI_LINK_REQ_KP_PAIR_START,
           "BL-048: the pairing step didn't start looking");
    expect(lv_subject_get_int(&subject_wiz_next_on) == 0, "BL-048: Next is lit while it looks");
    link_has("Looking for the alarm panel", "BL-048: the pairing step doesn't say it looks");
    shot("bl-048-setup-pair-looking");
    run(31000);
    link_has("Still looking", "BL-048: 30 s without an alarm panel doesn't say what to check");
    link_has("Select Wi-Fi", "LINK-053: missing fallback after discovery timeout");
    shot("bl-048-setup-pair-not-found");
    host_link_demo_master_found();
    run(1100);
    link_has("Found Harbor House", "BL-048: the alarm panel found isn't named");
    link_has("588 460", "BL-048: the code isn't shown in two groups of three");
    shot("bl-048-setup-pair-code");
    tap_text("No, cancel");
    run(300);
    link_has("Not paired: the codes didn't match", "BL-048: No doesn't end the pairing");
    expect(lv_subject_get_int(&subject_wiz_next_on) == 1, "BL-048: no Try again after a mismatch");
    shot("bl-048-setup-pair-mismatch");
    tap_name("wiz_next"); /* Try again */
    run(300);
    expect(link_last_kind() == UI_LINK_REQ_KP_PAIR_RETRY,
           "BL-048: Try again didn't ask to try again");
    link_has("Looking for the alarm panel", "BL-048: Try again doesn't look again");
    host_link_demo_master_found();
    run(1100);
    host_link_demo_pair_fail(UI_LINK_FAIL_REFUSED);
    run(1100);
    link_has("Not paired: rejected at the alarm panel", "BL-048: a rejection isn't said");
    shot("bl-048-setup-pair-refused");
    tap_name("wiz_next");
    run(300);
    host_link_demo_master_found();
    run(1100);
    host_link_demo_pair_fail(UI_LINK_FAIL_TIMEOUT);
    run(1100);
    link_has("Not paired: no answer in time", "BL-048: a timeout isn't said");
    shot("bl-048-setup-pair-timeout");
    tap_name("wiz_next");
    run(300);
    host_link_demo_master_found();
    run(1100);
    tap_text("Yes, pair");
    run(300);
    expect(link_last_kind() == UI_LINK_REQ_KP_PAIR_CONFIRM, "BL-048: Yes, pair wasn't sent");
    link_has("The codes match", "BL-048: Yes doesn't wait for the installer");
    shot("bl-048-setup-pair-waiting");
    host_link_demo_master_allows();
    run(1100);
    expect_step(4, "BL-048: a paired keypad isn't on its last step");
    link_has("Keypad paired", "LINK-053: pairing must be retained during Wi-Fi setup");
    host_link_demo_wifi_setup(1, true);
    run(1100);
    link_has("Setting up Wi-Fi", "LINK-053: missing automatic provisioning progress");
    expect(lv_subject_get_int(&subject_wiz_next_on) == 0, "LINK-053: premature completion");
    shot("bl-048-radio-sharing");
    host_link_demo_wifi_setup(4, false);
    run(1100);
    link_has("Reconnecting to the alarm panel", "LINK-053: lost link hidden");
    expect(lv_subject_get_int(&subject_wiz_next_on) == 0, "LINK-053: offline completion allowed");
    shot("bl-048-radio-lost");
    host_link_demo_wifi_setup(4, true);
    run(1100);
    expect(lv_subject_get_int(&subject_wiz_next_on) == 1, "LINK-053: radio-only completion disabled");
    shot("bl-048-radio-done-radio");
    tap_text("Select Wi-Fi");
    run(2000);
    expect(link_last_kind() == UI_LINK_REQ_KP_WIFI, "LINK-053: fallback erased enrollment");
    expect_step(2, "LINK-053: manual network selection missing");
    shot("bl-048-setup-keypad-wifi");
    tap_text("Harbor House");
    sheet_text("correct horse");
    tap_name("sheet_done");
    run(3500);
    tap_name("wiz_next");
    run(1100);
    expect_step(4, "LINK-053: manual Wi-Fi restarted pairing");
    link_has("Wi-Fi connected", "LINK-053: Wi-Fi success not shown");
    expect(find_visible(lv_screen_active(), "wiz_back") == NULL,
           "BL-048: a paired keypad's last step has Back");
    shot("bl-048-setup-pair-done");
    tap_name("wiz_next"); /* Done: the role is stored, the panel is a keypad */
    run(300);
    expect(link_last_kind() == UI_LINK_REQ_KP_DONE, "BL-048: Done didn't store the role");
    expect(find_visible(lv_screen_active(), "wiz_next") == NULL,
           "BL-048: Done didn't leave the setup");
    run(1100);
    link_has("Connecting", "BL-048: a new keypad's Home doesn't say it connects");
    expect(link_tile_dimmed(),
           "BL-048: the arming tiles open while the keypad connects (LINK-008)");
    shot("bl-048-kp-home-connecting");

    /* LINK-053: stored pairing may publish after the first UI frame on reboot.
     * Resume directly into Wi-Fi setup, then offer manual selection for an older
     * master/unavailable credentials without throwing away the stored pairing. */
    host_panel_forget_wifi();
    host_link_demo_resume_pairing(false);
    run(1100);
    ui_app_first_setup();
    run(300);
    link_has("Find your alarm panel", "LINK-053: missing pending-publication state");
    host_link_demo_resume_pairing(true);
    run(2200);
    expect_step(4, "LINK-053: persisted pairing did not resume after publication");
    expect(link_last_kind() == UI_LINK_REQ_KP_WIFI, "LINK-053: resume did not request Wi-Fi");
    link_has("Keypad paired", "LINK-053: resume lost enrollment");
    expect(lv_subject_get_int(&subject_wiz_next_on) == 0, "LINK-053: resume allowed offline Done");
    shot("bl-048-radio-resume");
    /* BL-048: removed at the alarm panel (or its record erased) before Done - the setup goes
     * back near the alarm panel by itself, and a new pairing goes on to its last step again */
    host_link_demo_unpaired();
    run(1100);
    link_has("Find your alarm panel", "BL-048: a removed keypad's setup stays on its last step");
    host_link_demo_resume_pairing(true);
    run(2200);
    expect_step(4, "BL-048: pairing again didn't reach the last step");
    host_link_demo_wifi_setup(4, true);
    run(1100);
    link_has("Select Wi-Fi", "LINK-053: older-master fallback missing");
    tap_text("Select Wi-Fi");
    run(2200); /* wait for the simulated network scan */
    expect_step(2, "LINK-053: resumed manual selection missing");
    expect(link_last_kind() == UI_LINK_REQ_KP_WIFI, "LINK-053: fallback changed pairing");
    tap_text("Harbor House");
    sheet_text("correct horse");
    tap_name("sheet_done");
    run(3500);
    tap_name("wiz_next");
    run(1100);
    expect_step(4, "LINK-053: resume manual Wi-Fi re-enrolled");
    link_has("Wi-Fi connected", "LINK-053: resume did not complete Wi-Fi");
    tap_name("wiz_next");
    run(300);
}

#if CONFIG_ALARM_HOME_CONTROL
/* BL-216 (docs/features/11 "Smart Home and Home Assistant on keypads"; proposal P1, approved
 * 2026-10-16): a keypad's Smart Home is its alarm panel's page unchanged - the same tiles, note,
 * pad and words, from Harbor House's devices through the link (host_link_demo.h) - each tap judged
 * on the alarm panel, an unlock's code pad opened with no round trip. An unlock whose answer is
 * lost reads "Not confirmed" once the keypad gives it up (HCTL-012, LINK-047). Without device data
 * - the alarm panel answering but its device data stopped for 30 s, or the link lost - every tile
 * reads offline since the last of it, the note says so, and a tap sends nothing (LINK-046). Shots
 * named as the proposal's screens. */
static void walk_link_keypad_devices(void)
{
    printf("A keypad: Smart Home, its alarm panel's devices (BL-216)\n");
    const lv_color_t connected = lv_subject_get_color(&subject_tone_color); /* the frame's tone */
    tap_name("nav_devices");
    run(300);
    expect(no_title() && tile_says("Front Door", "Locked", false) && tile_says("Back Door", "Unlocked", true),
           "BL-216: a keypad's Smart Home isn't its alarm panel's page");
    shot("bl-216-kp-devices");
    const unsigned acts = host_panel_device_acts();
    tap_device("Front Door");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1 && host_panel_device_acts() == acts,
           "BL-216, HCTL-005: a keypad's unlock didn't open the pad at once, with nothing sent");
    shot("bl-216-kp-device-unlock");
    /* the alarm panel's answers lost: pending, then given up after 3 s; Back Door locked meanwhile
     * at the alarm panel */
    host_link_demo_devices_unanswered(true);
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0 &&
               tile_says("Front Door", "Unlocking…", false) &&
               host_panel_device("lock.front_door")->state == UI_DEV_ST_LOCKED,
           "BL-216: an unanswered unlock isn't shown under way, or reached the alarm panel");
    host_panel_device_state("lock.back_door", UI_DEV_ST_LOCKED);
    run(3100);
    expect(tile_says("Front Door", "Not confirmed", true),
           "HCTL-012, LINK-047 (BL-216): an unlock given up doesn't read \"Not confirmed\"");
    shot("bl-216-kp-devices-unconfirmed");
    /* a tap tries it again, with the pad; answered, the tile is the alarm panel's again */
    host_link_demo_devices_unanswered(false);
    tap_device("Front Door");
    run(300);
    type("123456k");
    run(300);
    expect(host_panel_device("lock.front_door")->state == UI_DEV_ST_UNLOCKED &&
               tile_says("Front Door", "Unlocked", true),
           "BL-216: the retried unlock didn't unlock Front Door at the alarm panel");
    tap_device("Front Door"); /* locking asks no code */
    run(300);
    host_panel_device_state("lock.back_door", UI_DEV_ST_UNLOCKED);
    run(1200);
    expect(host_panel_device("lock.front_door")->state == UI_DEV_ST_LOCKED &&
               tile_says("Back Door", "Unlocked", true),
           "BL-216: the keypad's Smart Home didn't follow the alarm panel back");
    /* LINK-046: the alarm panel still answers, but no device data for 30 s */
    host_link_demo_devices_stop(true);
    run(29000);
    expect(tile_says("Front Door", "Locked", false),
           "LINK-046: device data read stale before 30 s");
    run(2000);
    expect(find_label_with(device_tile_of("Front Door"), "Offline since") != NULL &&
               lv_color_eq(lv_subject_get_color(&subject_tone_color), connected),
           "LINK-046 (BL-216): 30 s without device data doesn't read offline, or the frame isn't "
           "the connected one");
    tap_device("Kitchen Lights");
    run(300);
    expect(host_panel_device_acts() == acts + 2u,
           "LINK-046 (BL-216): a tap without device data reached the alarm panel");
    host_link_demo_devices_stop(false);
    run(1200);
    expect(tile_says("Back Door", "Unlocked", true),
           "LINK-046 (BL-216): the devices didn't come back with their data");
    /* the link lost: every tile offline since the last word, the keypad's frame without system
     * communication */
    link_state(HOST_LINK_KEYPAD_LOST);
    {
        lv_obj_t *door = find_label_with(device_tile_of("Front Door"), "Offline since");
        printf("  a keypad without its alarm panel: \"%s\"\n", door != NULL ? lv_label_get_text(door) : "-");
        expect(door != NULL && no_title() && find_visible(lv_screen_active(), "nav_devices") != NULL,
               "LINK-046 (BL-216): a keypad without its alarm panel doesn't read its devices "
               "offline whole, or hides Smart Home");
    }
    shot("bl-216-kp-devices-lost");
    tap_device("Porch Light");
    run(300);
    expect(host_panel_device_acts() == acts + 2u && device_sheet_open() == NULL,
           "LINK-046 (BL-216): a tap without the alarm panel did something");
    tap_name("nav_home");
    run(300);
}
#endif

static void walk_link_keypad(void)
{
    printf("A keypad: Home, Emergency, Settings and About (BL-048)\n");
    link_state(HOST_LINK_KEYPAD_ONLINE);
    run(3000); /* the rail's Smart Home follows within a few seconds */
    expect(!link_tile_dimmed(), "BL-048: the arming tiles stay dimmed once connected");
    /* BL-216 (proposal P1, approved 2026-10-16): a keypad's rail shows Smart Home under the alarm
     * panel's rule, a listed device that isn't from Z-Wave (here the house's, as its backend's
     * dev_list gives them) */
    expect(find_visible(lv_screen_active(), "nav_devices") != NULL,
           "BL-216: a keypad whose alarm panel lists devices has no Smart Home on its rail");
    expect(lv_subject_get_int(&subject_bar_banner) == 8 ||
               lv_subject_get_int(&subject_bar_banner) == 0,
           "BL-048: a keypad's factory-code reminder doesn't point to the alarm panel");
    shot("bl-048-kp-home-ready");
    {
        /* The alarm panel moves this keypad to another partition: its options follow it,
         * range-checked (docs/features/11 "Roles on a P4 panel", home_gen). */
        const unsigned saves = host_panel_options_saves();
        host_link_demo_home(9); /* no such partition: use safe home */
        run(2100);
        lv_obj_t *label = lv_obj_find_by_name(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), "title_label");
        expect(strcmp(lv_label_get_text(label), "Home") == 0, "invalid assignment uses home");
        host_link_demo_home(2);
        run(2100);
        expect(strcmp(lv_label_get_text(label), "Partition 2") == 0,
               "BL-048: the keypad didn't follow its new home partition");
        host_link_demo_home(1);
        run(2100);
        expect(strcmp(lv_label_get_text(label), "Home") == 0,
               "BL-048: the keypad didn't come back to partition 1");
        expect(host_panel_options_saves() == saves, "assignment does not rewrite audio preferences");
    }
#if CONFIG_ALARM_HOME_CONTROL
    walk_link_keypad_devices();
#endif
    link_state(HOST_LINK_KEYPAD_LOST);
    link_has("No system communication",
             "BL-048: a keypad without the alarm panel doesn't say so (LINK-008)");
    link_has("State unknown · Arming is off",
             "BL-048: the lost keypad's state card doesn't say arming is off");
    expect(find_visible(lv_screen_active(), "link_strip") != NULL,
           "BL-048: no Check the alarm panel strip");
    expect(link_tile_dimmed(), "BL-048: the arming tiles open without the alarm panel (LINK-008)");
    expect(find_visible(lv_screen_active(), "nav_panic") != NULL,
           "BL-048: Panic is gone without the alarm panel");
    expect(lv_subject_get_int(&subject_bar_banner) != 8,
           "BL-048: a lost keypad shows the codes' reminder as current");
    tap(lv_obj_get_child(by_name("panel_arm"), 0));
    run(300);
    expect(lv_subject_get_int(&subject_panel) == 0, "BL-048: a dimmed arming tile opened the pad");
    shot("bl-048-kp-home-lost");
    tap_name("nav_panic");
    run(300);
    link_has("Hold a button for 2 seconds to set off the alarm on Harbor House",
             "BL-048: Emergency on a keypad doesn't name the alarm panel");
    link_has("No system communication · This keypad keeps trying for 1 minute",
             "BL-048: Emergency without the alarm panel doesn't say it keeps trying (LINK-023)");
    shot("bl-048-kp-emergency-lost");
    tap_name("nav_zones");
    run(300);
    link_has("Unknown",
             "BL-048: a keypad without the alarm panel shows its zones' last state (LINK-008)");
    tap_name("nav_home");
    run(300);
    link_state(HOST_LINK_KEYPAD_ONLINE);
    tap_name("bar_settings");
    run(300);
    expect(find_visible(lv_screen_active(), "set_users") == NULL &&
               find_visible(lv_screen_active(), "set_integrations") == NULL &&
               find_visible(lv_screen_active(), "set_walk") == NULL &&
               find_visible(lv_screen_active(), "set_automations") == NULL,
           "BL-048: a keypad's Settings list the alarm panel's sections");
    /* LINK-028 (decided 2026-10-16): a keypad speaks with its own voice - its Voice volume and
     * Announcements, as on the alarm panel; the zones' spoken names stay the alarm panel's */
    expect(find_label(by_name("page_body"), "Voice volume") != NULL &&
               find_label(by_name("page_body"), "Announcements") != NULL,
           "BL-048: a keypad's Screen and sound has no Voice volume or Announcements (LINK-028)");
    shot("bl-048-kp-settings");
    tap_link("Announcements");
    run(300);
    expect(find_label(by_name("page_body"), "Status messages") != NULL &&
               find_label(by_name("page_body"), "Zone chime") != NULL &&
               find_label(by_name("page_body"), "Uses the Voice volume in Screen and sound. Alarms, "
                                                "fire and CO always sound at full volume.") != NULL,
           "BL-048: a keypad's Announcements page isn't the alarm panel's (LINK-028)");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(50);
    shot("bl-048-kp-announcements");
    tap_name("page_back");
    run(300);
    tap_name("set_time");
    run(300);
    link_has("From Harbor House", "BL-048: a keypad's clock doesn't say where it comes from");
    tap_text("Enter a master or installer code");
    run(200);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(300);
    }
    link_has("Set on Harbor House",
             "BL-048: a keypad's time zone doesn't say it is the alarm panel's");
    expect(find_label(by_name("page_body"), "Time server") == NULL,
           "BL-048: a keypad offers a time server");
    shot("bl-048-kp-time");
    tap_name("set_about");
    run(300);
    link_has("Keypad 2 of Harbor House", "BL-048: About doesn't say what this is a keypad of");
    link_has("Connected · shows and arms Home",
             "BL-048: About doesn't say the keypad is connected");
    expect(find_label(by_name("page_body"), "Installer") == NULL,
           "BL-048: a keypad's About has Installer");
    shot("bl-048-kp-about");
    scroll_to_text("page_body", "Make it a stand-alone panel");
    shot("bl-048-kp-about-end");
    tap_text("Make it a stand-alone panel");
    run(300);
    expect(find_label(lv_screen_active(), "Hold to erase everything") == NULL,
           "BL-048: a stand-alone panel opens without the installer code (LINK-025: only after 10 "
           "min)");
    tap_text("Enter the installer code");
    run(200);
    type("69420k");
    run(300);
    link_hold("Hold to erase everything", 1100);
    shot("bl-048-kp-standalone");
    s_pressed = false;
    run(4000); /* "Keep holding for 2 seconds" goes */
    tap_name("nav_home");
    run(300);
    link_state(HOST_LINK_KEYPAD_REMOVED);
#if CONFIG_ALARM_HOME_CONTROL
    run(4000); /* off its view the rail looks every 5 s */
    expect(find_visible(lv_screen_active(), "nav_devices") == NULL,
           "BL-216: a removed keypad keeps its alarm panel's Smart Home");
#endif
    link_has("Not paired", "BL-048: a removed keypad doesn't say so (LINK-014)");
    link_has("Harbor House removed this keypad",
             "BL-048: a removed keypad doesn't say who removed it");
    expect(find_visible(lv_screen_active(), "nav_panic") == NULL,
           "BL-048: a removed keypad offers Panic");
    shot("bl-048-kp-removed");
    tap_text("Pair again");
    run(300);
    expect_step(3, "BL-048: Pair again doesn't open the pairing step");
    expect(link_last_kind() == UI_LINK_REQ_KP_PAIR_START, "BL-048: Pair again doesn't look");
    tap_name("wiz_exit");
    run(300);
    expect(link_last_kind() == UI_LINK_REQ_KP_PAIR_STOP,
           "BL-048: Leave setup doesn't stop looking");
    expect(find_visible(lv_screen_active(), "wiz_next") == NULL,
           "BL-048: Leave setup didn't leave");
}

/* What the review of the keypad role added (BL-048): the fire or CO tone a keypad keeps through a
 * lost link and its Silence (decided 2026-10-15) - it wakes the screen from its saver and keeps it
 * awake and lit as an alarm does, by night too (UI-009, UI-043) - a keypad that needs an update to
 * read its alarm panel's settings, and the no-code pages while the last status said armed
 * (LINK-025) with Troubles' Acknowledge still dimmed (UI-022). */
static void walk_link_keypad_kept(const ui_options_t *start)
{
    printf("A keypad: its kept fire tone, an update it needs, maintenance while armed (BL-048)\n");
    tap_name("nav_home");
    run(300);
    link_state(HOST_LINK_KEYPAD_LOST);
    /* Night hours all day long (00:01 to 00:00), dimmed after a minute, dark after five. */
    ui_options_t o = *start;
    o.saver_after_ms = 60000u;
    o.off_after_ms = 300000u;
    o.night_off_after_ms = 300000u;
    o.night_on = true;
    o.night_start = 1u;
    o.night_end = 0u;
    ui_app_apply_options(&o);
    run(1100);
    expect_light(o.night_brightness, "BL-048: a calm keypad at night isn't at the night's brightness");
    run(61000);
    expect_saver(true, "UI-009: a keypad without system communication, untouched for a minute");
    host_link_demo_local_alarm(true);
    run(1100);
    expect_saver(false, "BL-048: a fire tone kept on a keypad didn't wake the screen from its saver");
    expect_light(o.brightness, "BL-048: a keypad's kept fire tone isn't lit as an alarm is at night");
    run(o.off_after_ms + 60000u);
    expect_saver(false, "BL-048: the screen saver came while a keypad's kept fire tone sounds");
    expect_light(o.brightness, "BL-048: the screen dimmed while a keypad's kept fire tone sounds");
    expect(lv_subject_get_int(&subject_panel) == 6 && /* Silence */
               find_visible(lv_screen_active(), "panel_silence") != NULL,
           "BL-048: a keypad keeping a fire tone without the alarm panel offers no Silence");
    link_has("Stops the alarm sound on this keypad", "BL-048: Silence doesn't say what it stops");
    expect(find_visible(lv_screen_active(), "link_strip") != NULL,
           "BL-048: the kept fire tone hides Check the alarm panel");
    expect(lv_subject_get_int(&subject_alarm_active) == 0,
           "BL-048: a keypad without the alarm panel shows an alarm look (LINK-008)");
    shot("bl-048-kp-home-lost-silence");
    tap(lv_obj_get_child(by_name("panel_silence"), 0));
    run(300);
    expect(strcmp(lv_subject_get_string(&subject_toast), "Silenced on this keypad") == 0,
           "BL-048: Silence on a keypad without the alarm panel doesn't say it stopped here");
    host_link_demo_local_alarm(false); /* the service stops the tone and says so */
    run(1100);
    expect(lv_subject_get_int(&subject_panel) == 0 && link_tile_dimmed(),
           "BL-048: after Silence the dimmed arming tiles don't come back");
    expect_light(o.night_brightness, "BL-048: after Silence the keypad keeps an alarm's brightness");
    ui_app_apply_options(start);
    run(4000); /* the toast goes */

    link_state(HOST_LINK_KEYPAD_CONNECTING);
    host_link_demo_needs_update(true);
    run(1100);
#if CONFIG_ALARM_HOME_CONTROL
    run(4000); /* off its view the rail looks every 5 s */
    expect(find_visible(lv_screen_active(), "nav_devices") == NULL,
           "BL-216: a keypad connecting has Smart Home before its list is in");
#endif
    link_has("Update this keypad: Settings > About",
             "BL-048: a keypad that needs an update doesn't say so");
    shot("bl-048-kp-home-update");
    tap_name("bar_settings");
    run(300);
    tap_name("set_about");
    run(300);
    tap_link("Software update");
    run(300);
    expect(find_label(lv_screen_active(), "The system master or installer code updates the "
                                          "software") == NULL,
           "BL-048: a keypad that needs an update asks for a code it can't check");
    tap_name("nav_home");
    run(300);
    host_link_demo_needs_update(false);

    /* LINK-025 while the last status said armed: Settings opens for the no-code pages, and closes
     * once the alarm panel answers armed. */
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_AWAY;
    snprintf(cmd.code, sizeof(cmd.code), "%s", "123456");
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "BL-048: the house would not arm");
    link_state(HOST_LINK_KEYPAD_LOST);
    host_link_demo_maintenance(true);
    run(1100);
    expect(lv_subject_get_int(&subject_settings_locked) == 0 &&
               lv_subject_get_int(&subject_locked) == 1,
           "BL-048: the gear isn't lit, or Acknowledge and Chime don't stay dimmed (UI-022)");
    tap_name("bar_settings");
    run(300);
    expect(lv_subject_get_int(&subject_view) == 5, /* Settings */
           "BL-048: maintenance can't open Settings while the last status said armed (LINK-025)");
    tap_name("set_wifi");
    run(300);
    expect(find_label(lv_screen_active(), "A master or installer code changes the network") == NULL,
           "BL-048: maintenance's Wi-Fi asks for a code (LINK-025)");
    link_state(HOST_LINK_KEYPAD_ONLINE);
    run(300);
    expect(lv_subject_get_int(&subject_view) != 5,
           "BL-048: Settings stays open once the alarm panel answers armed (UI-022)");
    cmd.type = ALARM_CMD_DISARM;
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "BL-048: the house would not disarm");
    run(1100);
    tap_name("nav_home");
    run(300);
}

static void walk_link_master(const ui_options_t *start)
{
    printf("The alarm panel: Keypads and devices (BL-048)\n");
    host_panel_init(1789857660000LL + 17LL * 60000LL); /* 10:58 PM */
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    link_state(HOST_LINK_MASTER);
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_link("Keypads and devices");
    run(300);
    link_has("Another RoboAlarms panel on this Wi-Fi can be a keypad for this one.",
             "BL-048: Keypads and devices has no keypads section");
    shot("bl-048-devices-empty");
    tap_text("Add a keypad");
    run(300);
    expect(link_last_kind() == UI_LINK_REQ_PAIR_OPEN, "BL-048: Add a keypad didn't open pairing");
    run(13000);
    expect(find_label_with(lv_screen_active(), "Waiting for a keypad · 1 min 4") != NULL,
           "BL-048: Add a keypad doesn't count its 2 minutes down");
    shot("bl-048-add-open");
    host_link_demo_keypad_asks();
    run(1100);
    link_has("Does the keypad show this code?", "BL-048: the code isn't asked on the alarm panel");
    link_has("588 460", "BL-048: the alarm panel's code isn't the keypad's");
    shot("bl-048-add-code");
    tap_text("Yes, allow");
    run(300);
    link_has("Allowed here", "BL-048: Yes, allow doesn't wait for the keypad");
    shot("bl-048-add-allowed");
    host_link_demo_keypad_confirms();
    run(1100);
    expect(host_link_demo_keypads() == 1u, "BL-048: the keypad wasn't added");
    link_has("Added as Keypad 2", "BL-048: adding a keypad doesn't say its number");
    run(1100);
    link_has("Keypad 2", "BL-048: the new keypad isn't listed as Keypad 2");
    shot("bl-048-add-done");
    run(4000); /* the toast goes */
    tap_text("Add a keypad");
    run(300);
    run(121000);
    link_has("No keypad asked to pair", "BL-048: the 2 minutes running out isn't said");
    shot("bl-048-add-nothing");
    tap_name("page_back");
    run(300);
    expect(link_last_kind() == UI_LINK_REQ_PAIR_STOP,
           "BL-048: leaving Add a keypad doesn't close pairing");
    host_link_demo_numbers_held(0x00FDu); /* every number but 2, the new keypad's */
    tap_text("Add a keypad");
    run(300);
    link_has("Not started: keypad numbers 1 to 8 are all in use.",
             "BL-048: no number free isn't said");
    shot("bl-048-add-full");
    tap_name("page_back");
    run(300);
    link_state(HOST_LINK_MASTER_TWO);
    link_has("Hall keypad", "BL-048: the keypads aren't listed");
    link_has("Not responding", "BL-048: a silent keypad isn't said (LINK-007)");
    expect(find_label(by_name("page_body"), "Device 2") == NULL &&
               find_label(by_name("page_body"), "Device 3") == NULL,
           "BL-048: the numbers keypads hold are in the table too (LINK-001)");
    shot("bl-048-devices");
    tap_text("Hall keypad");
    run(300);
    link_has("Keypad 2 · saved when you go back",
             "BL-048: a keypad's page doesn't give its number");
    shot("bl-048-keypad-page");
    tap_text("Supervision");
    run(300);
    expect(find_label(by_name("pick_list"), "1 min") != NULL &&
               find_label(by_name("pick_list"), "5 min") != NULL &&
               find_label(by_name("pick_list"), "15 min") != NULL &&
               find_label(by_name("pick_list"), "Not supervised") != NULL,
           "BL-048: a keypad's supervision isn't 1, 5 or 15 min or not supervised");
    tap_text("15 min");
    run(300);
    {
        ui_link_keypad_t k;
        expect(host_link_demo_keypad(2, &k) && k.supervision_s == 900u,
               "BL-048: a keypad's supervision wasn't saved");
    }
    tap_name("page_back");
    run(300);
    tap_text("Bedroom keypad");
    run(300);
    link_hold("Remove this keypad", 1100);
    shot("bl-048-keypad-page-silent");
    run(1100); /* held for 2 seconds: removed, no dialog */
    s_pressed = false;
    run(300);
    expect(host_link_demo_keypads() == 1u, "BL-048: the hold didn't remove the keypad (LINK-014)");
    link_has("Bedroom keypad removed", "BL-048: removing a keypad isn't said");
    run(4000);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
}

/* Keypad updates (docs/features/11 "Updates through the link"; proposal P2, approved 2026-10-17):
 * the alarm panel's Software update lists its keypads, which update with it, each with one status
 * line and nothing to tap; Install's question says they update with it; Keypads and devices says
 * "Updating" for a keypad restarting with its update; Activity has their stages. On a keypad that
 * follows its alarm panel: Software update says where its updates come from and how far one is
 * (no Install, Check again or Stop), About's row names the alarm panel, and Home has the steady
 * banner. Each scene starts at the drawing's time; shots named as the proposal's screens. */
#define KU_650AM 1789887000000LL /* Sep 20, 6:50 AM */

static ui_fw_status_t ku_fw(uint8_t stage)
{
    ui_fw_status_t st = fw_run(stage);
    snprintf(st.running, sizeof(st.running), "0.6.1");
    snprintf(st.latest, sizeof(st.latest), "0.6.2");
    snprintf(st.target, sizeof(st.target), "0.6.2");
    snprintf(st.previous, sizeof(st.previous), "0.6.1");
    if (stage == UI_FW_STAGE_NONE) {
        st.phase = UI_FW_IDLE;
    }
    return st;
}

static void ku_scene(const ui_options_t *start, int64_t at, host_link_demo_t link)
{
    host_panel_init(at);
    ui_app_apply_options(start);
    run(500);
    wake();
    tap_name("nav_home");
    run(300);
    link_state(link);
}

/* The clock moves on, the panel left alone (the screen saver may come: woken after). */
static void ku_minutes(unsigned minutes)
{
    for (unsigned m = 0; m < minutes; m++) {
        s_ms += 60000u;
        host_panel_advance(60000u);
    }
    run(1500);
    wake();
    run(300);
}

static bool ku_page_has(const char *text)
{
    return find_label(by_name("page_body"), text) != NULL;
}

static void walk_keypad_updates(const ui_options_t *start)
{
    printf("Keypad updates: the alarm panel's keypads, a keypad's own pages (BL-048, P2)\n");
    /* the alarm panel, 0.6.2 offered, its three keypads up to date on 0.6.1 */
    ku_scene(start, KU_650AM, HOST_LINK_MASTER_THREE);
    ui_fw_status_t st = ku_fw(UI_FW_STAGE_NONE);
    host_panel_fw_preset(&st);
    fw_open_page();
    run(1500);
    expect(ku_page_has("Keypads update with this panel. They restart together once everything is "
                       "disarmed.") &&
               ku_page_has("Hall keypad") && ku_page_has("Keypad 2 · version 0.6.1") &&
               ku_page_has("Up to date"),
           "BL-048 P2: the alarm panel's Software update doesn't list its keypads");
    tap_text("Install version 0.6.2");
    run(300);
    expect(find_label_with(lv_screen_active(), "then its keypads update with it") != NULL,
           "BL-048 P2: Install's question doesn't say the keypads update with it");
    shot("bl-048-fw-master-install-question");
    tap_name("dialog_cancel");
    run(300);

    /* downloading, the keypads with it: one downloads, one receives, one is ready */
    ku_scene(start, KU_650AM + 2LL * 60000LL, HOST_LINK_MASTER_THREE);
    st = ku_fw(UI_FW_STAGE_DOWNLOADING);
    st.percent = 42;
    st.size = 2831155u;
    st.bytes = 1153434u;
    host_panel_fw_preset(&st);
    host_link_demo_keypad_fw(2, NULL, UI_LINK_FW_DOWNLOADING, 45);
    host_link_demo_keypad_fw(3, NULL, UI_LINK_FW_RECEIVING, 30);
    host_link_demo_keypad_fw(4, NULL, UI_LINK_FW_READY, 0);
    fw_open_page();
    run(1500);
    expect(ku_page_has("Downloading · 45 %") && ku_page_has("Receiving from this panel · 30 %") &&
               ku_page_has("Ready · restarts with this panel") &&
               ku_page_has("Stop the update"),
           "BL-048 P2: the keypads' rows don't follow the update, or Stop is gone");
    shot("bl-048-fw-master-keypads");
    /* UI-020: a percent follows once a second, in place */
    host_link_demo_keypad_fw(2, NULL, UI_LINK_FW_DOWNLOADING, 46);
    run(1100);
    expect(ku_page_has("Downloading · 46 %"), "BL-048 P2: a keypad's percent doesn't follow");
    /* every other status line */
    host_link_demo_keypad_fw(2, NULL, UI_LINK_FW_RESTARTING, 0);
    host_link_demo_keypad_fw(3, NULL, UI_LINK_FW_NEWER, 0);
    host_link_demo_keypad_fw(4, NULL, UI_LINK_FW_NOT_UPDATED, 0);
    run(1100);
    expect(ku_page_has("Restarting") && ku_page_has("Newer than this panel") &&
               ku_page_has("Not updated yet · tries again by itself"),
           "BL-048 P2: a keypad's status line lacks its words");

    /* after the update: two up to date, one not yet */
    ku_scene(start, KU_650AM + 41LL * 60000LL, HOST_LINK_MASTER_THREE);
    st = ku_fw(UI_FW_STAGE_NONE);
    st.available = false;
    snprintf(st.running, sizeof(st.running), "0.6.2");
    st.last.result = UI_FW_LAST_UPDATED;
    snprintf(st.last.version, sizeof(st.last.version), "0.6.2");
    st.last.at = (uint32_t)((KU_650AM + 24LL * 60000LL) / 1000);
    st.checked = (uint32_t)((KU_650AM + 27LL * 60000LL) / 1000);
    host_panel_fw_preset(&st);
    host_link_demo_keypad_fw(2, "0.6.2", UI_LINK_FW_UP_TO_DATE, 0);
    host_link_demo_keypad_fw(3, "0.6.2", UI_LINK_FW_UP_TO_DATE, 0);
    host_link_demo_keypad_fw(4, "0.6.1", UI_LINK_FW_NOT_UPDATED, 0);
    fw_open_page();
    run(1500);
    expect(ku_page_has("Updated to version 0.6.2") && ku_page_has("Keypad 4 · version 0.6.1") &&
               ku_page_has("Not updated yet · tries again by itself"),
           "BL-048 P2: after the update the keypads' rows don't say which have it");
    shot("bl-048-fw-master-keypads-done");

    /* Installer > Keypads and devices: a keypad restarting with its update */
    ku_scene(start, 1789857660000LL + 17LL * 60000LL, HOST_LINK_MASTER_TWO); /* 10:58 PM */
    host_link_demo_keypad_fw(3, NULL, UI_LINK_FW_RESTARTING, 0);
    st = ku_fw(UI_FW_STAGE_NONE);
    st.available = false;
    host_panel_fw_preset(&st);
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_link("Keypads and devices");
    run(1100);
    expect(ku_page_has("Updating") && !ku_page_has("Not responding"),
           "BL-048 P2: a keypad restarting with its update isn't \"Updating\"");
    shot("bl-048-fw-installer-keypads");
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);

    /* Activity: the keypads' stages in the alarm panel's log */
    ku_scene(start, KU_650AM + 22LL * 60000LL, HOST_LINK_MASTER_THREE); /* 7:12 AM */
    host_panel_fw_event(ALARM_FW_INSTALLING, 0, 0);
    ku_minutes(2);
    host_panel_fw_event(ALARM_FW_UPDATED, 0, 0);
    host_panel_fw_event(ALARM_FW_KEYPAD_UPDATED, 3, 0);
    host_panel_fw_event(ALARM_FW_KEYPAD_ROLLED_BACK, 2, 0);
    ku_minutes(11);
    host_panel_fw_event(ALARM_FW_KEYPAD_FAILED, 4, 20); /* BROKE_OFF */
    ku_minutes(6);
    tap_name("nav_activity");
    run(500);
    tap_text("System"); /* the filter the drawing shows */
    run(500);
    expect(lv_subject_get_int(&subject_event_filter) == 5 &&
               find_label(by_name("event_list"), "Keypad 4 update failed") != NULL &&
               find_label(by_name("event_list"), "The download broke off") != NULL &&
               find_label(by_name("event_list"), "Keypad 2 update rolled back") != NULL &&
               find_label(by_name("event_list"), "Keypad 3 updated") != NULL,
           "BL-048 P2: Activity lacks the keypads' update stages");
    shot("bl-048-fw-master-activity");
    tap_text("All");
    run(300);
    tap_name("nav_home");
    run(300);

    /* a keypad (Hall keypad, 2) following Harbor House's updates */
    ku_scene(start, KU_650AM + 2LL * 60000LL, HOST_LINK_KEYPAD_ONLINE);
    host_link_demo_fw_follows(true, false);
    st = ku_fw(UI_FW_STAGE_NONE);
    st.available = false;
    snprintf(st.latest, sizeof(st.latest), "0.6.1");
    host_panel_fw_preset(&st);
    run(3000); /* the rail's Smart Home follows within a few seconds */
    tap_name("bar_settings");
    run(300);
    tap_name("set_about");
    run(1100);
    expect(ku_page_has("From Harbor House"),
           "BL-048 P2: a keypad's About doesn't say its updates come from Harbor House");
    shot("bl-048-fw-kp-about");
    st = ku_fw(UI_FW_STAGE_DOWNLOADING);
    st.available = false;
    st.percent = 45;
    st.size = 2831155u;
    st.bytes = 1274020u;
    host_panel_fw_preset(&st);
    run(1500);
    expect(ku_page_has("Updating"),
           "BL-048 P2: a keypad's About doesn't say Updating while it runs");
    tap_text("Software update");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(1200);
    }
    expect(ku_page_has("Updates come from Harbor House") && ku_page_has("Version 0.6.1") &&
               ku_page_has("Updating to version 0.6.2") && ku_page_has("Downloading · 45 %"),
           "BL-048 P2: a keypad's Software update doesn't say where updates come from, or how far");
    expect(!ku_page_has("Install version 0.6.2") && !ku_page_has("Check again") &&
               !ku_page_has("Stop the update") && !ku_page_has("Steps"),
           "BL-048 P2: a keypad's Software update offers Install, Check again, Stop or steps");
    {
        lv_obj_t *body = by_name("page_body");
        lv_obj_t *row = find_label(body, "Updating to version 0.6.2");
        while (row != NULL && lv_obj_get_parent(row) != body) {
            row = lv_obj_get_parent(row);
        }
        lv_obj_t *level = row != NULL ? lv_obj_find_by_name(row, "level") : NULL;
        expect(level != NULL && !lv_obj_has_flag(level, LV_OBJ_FLAG_HIDDEN) &&
                   lv_bar_get_value(level) == 45,
               "BL-048 P2: a keypad's run row has no stepped bar at 45");
    }
    shot("bl-048-fw-kp-update");
    host_link_demo_fw_follows(true, true);
    st.percent = 46;
    host_panel_fw_preset(&st);
    run(2100); /* the link is read once a second, and the page once a second after it */
    expect(ku_page_has("Receiving from Harbor House · 46 %"),
           "BL-048 P2: a keypad receiving from Harbor House doesn't say so");
    st = ku_fw(UI_FW_STAGE_WAITING);
    st.available = false;
    st.size = 2831155u;
    host_panel_fw_preset(&st);
    run(1100);
    expect(ku_page_has("Ready · restarts with Harbor House once everything is disarmed"),
           "BL-048 P2: a keypad with its update doesn't say it restarts with Harbor House");
    /* Home: the steady banner, the same words from the first byte to the restart */
    host_link_demo_fw_follows(true, false);
    st = ku_fw(UI_FW_STAGE_DOWNLOADING);
    st.available = false;
    st.percent = 45;
    st.size = 2831155u;
    st.bytes = 1274020u;
    host_panel_fw_preset(&st);
    tap_name("nav_home");
    run(1500);
    expect(lv_subject_get_int(&subject_bar_banner) == 6 &&
               strcmp(lv_subject_get_string(&subject_update_banner),
                      "Updating · restarts with Harbor House") == 0,
           "BL-048 P2: a keypad's Home has no banner naming Harbor House");
    shot("bl-048-fw-kp-home");
    st = ku_fw(UI_FW_STAGE_WAITING);
    st.available = false;
    host_panel_fw_preset(&st);
    run(1500);
    expect(strcmp(lv_subject_get_string(&subject_update_banner),
                  "Updating · restarts with Harbor House") == 0,
           "BL-048 P2: a keypad's banner changes its words while it waits");
    host_link_demo_fw_follows(false, false);
    st = ku_fw(UI_FW_STAGE_NONE);
    st.available = false;
    host_panel_fw_preset(&st);
    run(1500);
}

/* KPD-006: real touch input on every page renews the temporary view, not a timer
 * started once at selection. Alarm authorization is covered by panel-link tests. */
static void walk_partition_policy(const ui_options_t *start)
{
    host_panel_init(1789857660000LL);
    host_link_demo_state(HOST_LINK_MASTER);
    ui_backend_t be;
    host_panel_backend(&be);
    alarm_config_t *cfg = malloc(sizeof *cfg);
    expect(cfg != NULL && be.get_config(be.ctx, cfg), "partition walk configuration");
    cfg->partitions[1].enabled = true;
    snprintf(cfg->partitions[1].name, ALARM_NAME_LEN, "Studio");
    expect(be.set_config(be.ctx, cfg) == ALARM_OK, "enable Studio");
    free(cfg);
    ui_options_t opt = *start;
    opt.partition = opt.partition_home = 1;
    opt.switch_allowed = false;
    opt.return_s = 30;
    ui_app_apply_options(&opt);
    run(1500);
    wake();
    tap_name("nav_home");
    expect(lv_obj_has_flag(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), LV_OBJ_FLAG_HIDDEN), "locked panel hides switcher");
    shot("bl-048-partition-locked");
    opt.switch_allowed = true;
    ui_app_apply_options(&opt);
    run(1500);
    expect(!lv_obj_has_flag(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), LV_OBJ_FLAG_HIDDEN), "switcher below left card");
    lv_area_t switch_bounds, night_bounds;
    lv_obj_get_coords(lv_obj_get_child(by_name("partition_switch"), 0), &switch_bounds);
    lv_obj_get_coords(lv_obj_get_child(by_name("panel_arm"), 2), &night_bounds);
    expect(switch_bounds.y2 == night_bounds.y2, "partition switcher and Night tile bottom edges align");
    shot("bl-048-partition-home");
    tap_text("Switch partition");
    shot("bl-048-partition-chooser");
    tap_text("Studio");
    run(1500);
    expect(strcmp(lv_label_get_text(lv_obj_find_by_name(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), "title_label")), "Studio") == 0, "selection without a code");
    shot("bl-048-partition-away");
    const unsigned saves = host_panel_options_saves();
    tap_name("nav_activity");
    run(20000);
    tap_name("nav_activity"); /* actual touch while looking at Activity */
    run(20000);
    expect(strcmp(lv_label_get_text(lv_obj_find_by_name(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), "title_label")), "Studio") == 0, "Activity input renews idle return");
    run(11000);
    expect(strcmp(lv_label_get_text(lv_obj_find_by_name(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), "title_label")), "Studio") != 0, "30 idle seconds returns home");
    expect(host_panel_options_saves() == saves, "temporary selection must not write preferences");
    tap_name("nav_home");
    opt.return_never = true;
    ui_app_apply_options(&opt);
    run(1500);
    tap_text("Switch partition");
    tap_text("Studio");
    run(35000);
    expect(strcmp(lv_label_get_text(lv_obj_find_by_name(lv_obj_find_by_name(lv_screen_active(), "partition_switch"), "title_label")), "Studio") == 0, "Never keeps the selected view");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_link("Keypads and devices");
    tap_text("This panel");
    run(300);
    shot("bl-048-partition-policy-never");
    tap_text("Never return automatically");
    run(300);
    ui_options_t saved;
    expect(host_panel_saved_options(&saved) && !saved.return_never && saved.return_s == 30,
           "Never off restores the saved 30 second delay");
    shot("bl-048-partition-policy");
    tap_name("nav_home");
    host_link_demo_state(HOST_LINK_KEYPAD_ONLINE);
    host_link_demo_partition_policy(true, false, 30);
    run(2100);
    tap_text("Switch partition");
    tap_text("Studio");
    run(20000);
    tap_name("nav_activity");
    run(20000);
    ui_link_info_t info;
    expect(be.link_info(be.ctx, &info) && info.selected == 2, "remote Activity renews selection");
    run(12000);
    expect(be.link_info(be.ctx, &info) && info.selected == 0, "remote idle returns home");
    printf("Partition policy walk passed\n");
}

/* BL-223 (proposal BL-223-keypad-master-settings.js, approved 2026-10-21): a keypad's Settings in
 * two groups, its alarm panel's Users and Installer under that panel's name; the installer menu
 * from the keypad without what stays on the alarm panel; installer mode held at another keypad;
 * the alarm panel's pages dimmed while the link is down. The preview's alarm panel is the pretend
 * panel's own backend (on a panel, proxies to it). */
static void walk_link_keypad_master_pages(void)
{
    printf("A keypad: its alarm panel's pages (BL-223)\n");
    link_state(HOST_LINK_KEYPAD_ONLINE);
    host_link_demo_remote(UI_LINK_AREA_SESSION | UI_LINK_AREA_CONFIG | UI_LINK_AREA_USERS |
                              UI_LINK_AREA_ZONES | UI_LINK_AREA_ZWAVE | UI_LINK_AREA_ZIGBEE |
                              UI_LINK_AREA_OUTPUTS,
                          false, UI_LINK_HOLDER_NONE);
    run(1100);
    tap_name("bar_settings");
    run(300);
    expect(find_visible(lv_screen_active(), "set_head_local") != NULL &&
               find_visible(lv_screen_active(), "set_head_master") != NULL &&
               find_visible(lv_screen_active(), "set_users") != NULL &&
               find_visible(lv_screen_active(), "set_installer") != NULL,
           "BL-223: a keypad's Settings has no Harbor House group (LINK-057)");
    expect(find_visible(lv_screen_active(), "set_integrations") == NULL &&
               find_visible(lv_screen_active(), "set_walk") == NULL,
           "BL-223: a keypad's Settings lists pages its alarm panel doesn't serve yet");
    expect(find_label(by_name("settings_nav"), "Harbor House") != NULL,
           "BL-223: the alarm panel's group isn't under its name");
    tap_name("set_display");
    run(300);
    shot("bl-223-kp-settings");
    tap_name("set_installer");
    run(300);
    link_has("Changes how Harbor House works", "BL-223: the keypad's Installer doesn't name its alarm panel");
    expect(find_label(by_name("page_body"), "Lost the installer code?") == NULL,
           "BL-223: a keypad offers the alarm panel's lost-code page");
    shot("bl-223-kp-installer-locked");
    tap_text("Enter the installer code");
    run(200);
    type("69420k");
    run(400);
    host_link_demo_remote(UI_LINK_AREA_SESSION | UI_LINK_AREA_CONFIG | UI_LINK_AREA_USERS |
                              UI_LINK_AREA_ZONES | UI_LINK_AREA_ZWAVE | UI_LINK_AREA_ZIGBEE |
                              UI_LINK_AREA_OUTPUTS,
                          true, (uint8_t)2);
    run(1100);
    link_has("Leave installer mode", "BL-223: the installer code didn't open the menu from a keypad");
    expect(find_label(by_name("page_body"), "Factory reset") == NULL &&
               find_label(by_name("page_body"), "Run the setup again") == NULL &&
               find_label(by_name("page_body"), "Update server") == NULL,
           "BL-223: a keypad's installer menu offers what stays on the alarm panel");
    shot("bl-223-kp-installer-menu");
    tap_link("Zones");
    run(300);
    shot("bl-223-kp-zones");
    /* the link lost while there: the page stays, dimmed, and says so (LINK-063) */
    link_state(HOST_LINK_KEYPAD_LOST);
    run(1100);
    link_has("Harbor House can't be reached right now",
             "BL-223: a page of the alarm panel's doesn't say it can't be reached");
    shot("bl-223-kp-lost");
    link_state(HOST_LINK_KEYPAD_ONLINE);
    host_link_demo_remote(UI_LINK_AREA_SESSION | UI_LINK_AREA_CONFIG | UI_LINK_AREA_USERS |
                              UI_LINK_AREA_ZONES | UI_LINK_AREA_ZWAVE | UI_LINK_AREA_ZIGBEE |
                              UI_LINK_AREA_OUTPUTS,
                          true, (uint8_t)2);
    run(1100);
    tap_name("program_banner"); /* leaves installer mode */
    run(400);
    host_link_demo_remote(0, false, UI_LINK_HOLDER_NONE);
    run(1100);
    tap_name("nav_home");
    run(300);
}

static void walk_panel_link(const ui_options_t *start)
{
    walk_link_setup(start);
    walk_link_keypad();
    walk_link_keypad_master_pages(); /* BL-223 */
    walk_link_keypad_kept(start);
    walk_link_master(start);
    walk_keypad_updates(start); /* proposal P2, appended: the shots before keep their numbers */
    host_link_demo_state(HOST_LINK_OFF); /* as without the link, for what follows */
    run(1100);
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1) {
        s_out = argv[1];
    }
    make_dir(s_out);
#if defined(_WIN32)
    _putenv("TZ=UTC0");
    _tzset();
#else
    setenv("TZ", "UTC0", 1);
    tzset();
#endif

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, on_frame, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, on_frame, LV_EVENT_REFR_READY, NULL);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, pointer_read);
    lv_indev_set_scroll_limit(touch, 20); /* what the panel uses (board_touch.c) */

    /* Saturday 19 September 2026, 22:41 */
#if ALARM_SIM_ZWAVE
    host_panel_zwave_later(); /* a panel without Z-Wave until walk_zwave() */
#endif
    host_panel_init(1789857660000LL);
    /* No automations in the main walk: its arming Away would run Good Night (docs/features/41)
     * before the Smart Home shots. auto_smoke proves them. */
    host_panel_automations(false);
    ui_backend_t be;
    host_panel_backend(&be);
    be.beep = beep_cb; /* so the checks can hear the key click */
    ui_options_t start;
    ui_app_default_options(&start);
#ifdef ALARM_PREVIEW_LOCALE
    s_cat = i18n_catalog(ALARM_PREVIEW_LOCALE);
    expect(s_cat != NULL, "the test language isn't built in (tools/i18n.py pseudo --build)");
    s_locale = true;
    load_contexts(ALARM_PREVIEW_POT);
#ifndef ALARM_PREVIEW_LIVE
    /* The panel's stored language, as a panel starts in it (docs/features/37 I18N-015). */
    snprintf(start.lang, sizeof(start.lang), "%s", ALARM_PREVIEW_LOCALE);
#endif
    printf("The walk in %s\n", ALARM_PREVIEW_LOCALE);
#endif
    ui_app_start(&be, &start);
#ifdef ALARM_PREVIEW_LIVE
    /* -Live (I18N-014): the panel starts in English and the language changes while it runs, the
     * way the Panel Manager changes it; every picture after it must be the same as the walk that
     * started in the language (tools/ui_preview.ps1 compares them). The screens are built and
     * filled in English by now; no time passes, so what moves (a spinner) is where it was. */
    ui_options_t live;
    ui_app_default_options(&live);
    snprintf(live.lang, sizeof(live.lang), "%s", ALARM_PREVIEW_LOCALE);
    /* What a change costs: 20 of them, ending in the test language (no preview time passes,
     * so the pictures stay those of one change). The panel takes some tens of times longer. */
    ui_options_t source;
    ui_app_default_options(&source); /* English */
    const clock_t t0 = clock();
    for (int i = 0; i < 10; i++) {
        ui_app_apply_options(&live);
        ui_app_apply_options(&source);
    }
    ui_app_apply_options(&live);
    const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 21.0;
    printf("Changed to %s while running: %.2f ms a change on this PC\n", ALARM_PREVIEW_LOCALE, ms);
    /* The walk is in the language from here on: the sections that put the starting options back
     * (the screen's light, the walk test) keep it, as the walk that started in it does. */
    snprintf(start.lang, sizeof(start.lang), "%s", ALARM_PREVIEW_LOCALE);
#endif
#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
    expect(ui_dev_control_begin(), "DEV-010: unauthenticated Home control unavailable");
#endif
    run(400);

#if ALARM_LV_BUDGET_KB == 0
    if (getenv("ALARM_PREVIEW_PARTITION") != NULL) {
        walk_partition_policy(&start);
        return 0;
    }
    if (getenv("ALARM_PREVIEW_THEMES") != NULL) { /* only the appearances' walk, to work on it */
        walk_themes();
        return 0;
    }
    if (getenv("ALARM_PREVIEW_LETTERS") != NULL) { /* only the walk without the letters pack */
        walk_without_letters(&start);
        return 0;
    }
    if (getenv("ALARM_PREVIEW_LINK") != NULL) { /* only the panel link's segment (BL-048) */
        walk_panel_link(&start);
        return 0;
    }
    if (getenv("ALARM_PREVIEW_ZIGBEE") != NULL) { /* only the real Zigbee's segment, to work on it */
        walk_zigbee_real(&start);
        return 0;
    }
#endif
#if ALARM_SIM_ZWAVE && ALARM_LV_BUDGET_KB == 0
    if (getenv("ALARM_PREVIEW_ZWAVE_REGION") != NULL) { /* only the region change's segment */
        walk_zwave_region_change();
        return 0;
    }
    if (getenv("ALARM_PREVIEW_SCRIPT") != NULL) {
        return run_script(getenv("ALARM_PREVIEW_SCRIPT"));
    }
#endif

    printf("Home\n");
    shot("home_ready");

    host_panel_zone(3, ALARM_ZEV_FAULT);
    run(300);
    shot("home_not_ready");

    printf("Arming Away\n");
    host_panel_zone(3, ALARM_ZEV_RESTORE);
    run(200);
    tap(lv_obj_get_child(by_name("panel_arm"), 0));
    {
        /* The dots take the hint's line without moving the header (UI-036). */
        lv_obj_t *icon = lv_obj_find_by_name(by_name("pad"), "pad_icon");
        expect(icon != NULL, "the pad has no icon");
        lv_area_t before, after;
        lv_obj_get_coords(icon, &before);
        expect(lv_subject_get_int(&subject_pad_dots_on) == 0, "dots before any digit");
        type("123");
        lv_obj_get_coords(icon, &after);
        expect(lv_subject_get_int(&subject_pad_dots_on) == 1, "no dots after three digits");
        expect(before.x1 == after.x1 && before.y1 == after.y1, "the header moved at the first digit");
    }
    shot("pad_arm_away");
    /* KPD-008: five wrong codes lock the pad, and its message counts the time left. */
    type("4k");
    for (int i = 0; i < 4; i++) {
        type("9999k");
    }
    run(1200);
    expect(strstr(lv_subject_get_string(&subject_pad_hint), "Try again in 4:5") != NULL ||
               s_locale,
           "KPD-008: the locked pad doesn't count the time left");
    {
        lv_obj_t *keys = lv_obj_find_by_name(by_name("pad"), "pad_keys");
        expect(keys != NULL && lv_obj_has_state(lv_obj_get_child(keys, 0), LV_STATE_DISABLED),
               "KPD-008: the number keys don't look disabled while locked out");
    }
    shot("pad_locked_out");
    run(5000);
    expect(strstr(lv_subject_get_string(&subject_pad_hint), "Try again in 4:5") != NULL ||
               s_locale,
           "KPD-008: the countdown didn't move on");
    run(300000);
    wake(); /* the screen saver came on while it waited */
    run(500);
    if (lv_subject_get_int(&subject_panel) != 2) {
        tap(lv_obj_get_child(by_name("panel_arm"), 0)); /* the pad closed on its own while idle */
    }
    expect(strstr(lv_subject_get_string(&subject_pad_hint), "Try again") == NULL || s_locale,
           "KPD-008: the pad still says locked after the lockout ended");
    {
        lv_obj_t *keys = lv_obj_find_by_name(by_name("pad"), "pad_keys");
        expect(keys != NULL && !lv_obj_has_state(lv_obj_get_child(keys, 0), LV_STATE_DISABLED),
               "KPD-008: the number keys stay disabled after the lockout ended");
    }
    type("123456k");
    run(9000);
    host_panel_zone(1, ALARM_ZEV_FAULT); /* leaving through the front door */
    run(3000);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(3000);
    /* No Cancel card to tap first: the pad is there, and it has no X (nothing behind it). */
    expect(lv_subject_get_int(&subject_panel) == 2, "exit time: the code pad isn't on Home");
    expect(find_label(lv_screen_active(), "Cancel arming") != NULL, "exit time: the pad isn't Cancel arming");
    expect(find_visible(lv_screen_active(), "pad_close") == NULL, "exit time: the pad has an X");
    expect(lv_subject_get_int(&subject_pad_glyph_on) == 1, "exit time: the pad has no icon");
    shot("exit_delay");
    run(47000);
    /* Armed 2 s ago, a minute after the last touch: the saver waits a full minute from here. */
    expect_saver(false, "just after exit time");
    expect(lv_subject_get_int(&subject_panel) == 2, "armed: the code pad isn't on Home");
    expect(find_label(lv_screen_active(), "Disarm") != NULL, "armed: the pad isn't Disarm");
    shot("armed_away");

    printf("Coming home\n");
    host_panel_zone(1, ALARM_ZEV_FAULT);
    run(300);
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    run(8000);
    type("12");
    shot("entry_delay");

    printf("Wrong code\n");
    type("99k");
    run(100);
    shot("pad_wrong_code");

    printf("Too late: alarm\n");
    run(25000);
    shot("alarm_burglary");
#ifdef ALARM_PREVIEW_LIVE
    /* I18N-014: a new language waits while an alarm is on the screen, and comes when it ends. */
    ui_options_t english;
    ui_app_default_options(&english);
    char held[128];
    snprintf(held, sizeof(held), "%s", lv_subject_get_string(&subject_headline));
    ui_app_apply_options(&english);
    run(200);
    expect(strcmp(lv_subject_get_string(&subject_headline), held) == 0,
           "I18N-014: the alarm's words changed language during the alarm");
#endif

    type("123456k");
#ifdef ALARM_PREVIEW_LIVE
    run(300); /* with the 200 above: the reference walk's time, so what moves moves alike */
    s_locale = false; /* the rail's caption in English, exactly */
    const bool home_in_english = find_label(lv_screen_active(), "Security") != NULL;
    s_locale = true;
    expect(home_in_english && lv_subject_get_string(&subject_headline)[0] != '[',
           "I18N-014: the language waiting for the alarm didn't come when it ended");
    ui_options_t back;
    ui_app_default_options(&back);
    snprintf(back.lang, sizeof(back.lang), "%s", ALARM_PREVIEW_LOCALE);
    ui_app_apply_options(&back); /* and back, for the rest of the walk */
#else
    run(500);
#endif
    shot("alarm_memory");
    tap_name("memory_tile");
    type("123456k");
    run(300);
    walk_zones_empty(); /* the quiet house: nothing bypassed or in trouble yet (BL-177) */

    printf("Zones, activity, troubles\n");
    host_panel_zone(3, ALARM_ZEV_FAULT);
    host_panel_zone(8, ALARM_ZEV_LOW_BATTERY);
    run(300);
    tap_name("nav_zones");
    lv_obj_t *list = by_name("zone_list");
    tap(lv_obj_find_by_name(lv_obj_get_child(list, 4), "bypass"));
    type("123456k");
    run(300);
    shot("zones");
    walk_zones_open_none(); /* UI-020, UI-023 (BL-177) */
    tap_name("nav_activity");
    shot("activity");
    expect(s_clicks > 0, "key click: pressing controls never made the click");
    expect_drag_scrolls("event_list", lv_obj_get_child(by_name("event_list"), 2),
                        "Activity, a finger on an event");
#if CONFIG_ALARM_HOME_CONTROL
    walk_smart_home();
#endif
    host_panel_trouble(ALARM_TRBL_AC_LOSS, true);
    host_panel_set_net(UI_NET_DISCONNECTED, 0);
    run(1500);
    tap_name("bar_trouble");
    shot("troubles");

    printf("Night\n");
    host_panel_trouble(ALARM_TRBL_AC_LOSS, false);
    host_panel_set_net(UI_NET_CONNECTED, -63);
    host_panel_zone(3, ALARM_ZEV_RESTORE);
    run(300);
    tap_name("nav_home");
    tap(lv_obj_get_child(by_name("panel_arm"), 2));
    type("123456k");
    run(70000);
    expect_saver(false, "10 s after exit time");
    shot("armed_night");

    printf("Fire\n");
    host_panel_zone(6, ALARM_ZEV_FAULT);
    run(1200);
    shot("fire_alarm");
    type("123456k");
    run(300);
    host_panel_zone(6, ALARM_ZEV_RESTORE);
    run(300);
    tap_name("memory_tile");
    type("123456k");
    run(300);

    printf("Emergency\n");
    tap_name("nav_panic");
    run(200);
    /* The screen says what the panel does - set the alarm off here - and never that help was
     * called (UI-029). Nothing is connected to Home Assistant yet at this point. */
    expect(find_label(lv_screen_active(),
                      "Hold a button for 2 seconds to set off the alarm on this panel") != NULL,
           "Emergency: the screen doesn't say what holding a button does");
    expect(find_label(lv_screen_active(), "Nothing is connected to pass this on*") != NULL,
           "Emergency: the screen doesn't say what can and can't leave the panel");
    expect(find_label_with(lv_screen_active(), "call for help") == NULL,
           "Emergency: the screen still promises that help is called");
    lv_obj_t *police = by_name("panic_police");
    lv_area_t a;
    lv_obj_get_coords(lv_obj_find_by_name(police, "button"), &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    run(1200);
    shot("emergency_hold");
    s_pressed = false;
    run(300);
    /* Letting go before the two seconds are up changes nothing and says so (the hold is
     * deliberate protection, UI-008). */
    expect(find_label(lv_screen_active(), "Keep holding for 2 seconds to set the alarm off") != NULL,
           "Emergency: letting go early doesn't say what the hold does");
    expect(lv_subject_get_int(&subject_alarm_active) == 0,
           "Emergency: a short press set the alarm off anyway");

    printf("Change code\n");
    tap_name("nav_home");
    tap_name("codes_banner");
    type("1234");
    shot("change_code");
    tap_name("pad_close");

    printf("Screen saver\n");
    run(62000);
    shot("screen_saver");

    /* ---- M4: settings, installer mode, first-boot setup ---- */
    printf("Settings\n");
    wake();
    tap_name("bar_settings");
    run(300);
    shot("settings_display");

    tap_name("set_users");
    type("1234");
    shot("settings_users_code");
    type("56k");
    run(300);
    shot("settings_users");
    tap_text("Add a user");
    type("2468k");
    type("2468k");
    run(300);
    tap_text("Name");
    sheet_text("Sam");
    /* The "User added. Give them a name" toast is still up. A toast that was already on screen
     * when a sheet opened has to move out of the keyboard's way like any other (UI-031), and
     * must not be able to swallow a touch meant for a key. */
    expect(lv_subject_get_int(&subject_toast_visible) == 1,
           "the toast went before the keyboard could be checked against it");
    expect_clear_of("toast", "sheet_kb", "a toast that was up before the sheet opened");
    expect_keyboard_reachable("a toast that was up before the sheet opened");
    shot("keyboard");
    tap_name("sheet_done");
    run(300);
    if (lv_subject_get_int(&subject_toast_visible) == 1) {
        expect(lv_obj_get_style_align(by_name("toast"), 0) == LV_ALIGN_BOTTOM_MID,
               "the toast stayed at the top after the sheet closed");
    }
    tap_text("What this code can do");
    run(200);
    /* A choice list fills the middle, so the toast from before goes rather than sitting on its
     * title or its rows (UI-031). */
    expect(lv_subject_get_int(&subject_toast_visible) == 0,
           "a toast from before is still over the choice list");
    shot("user_authority");
    tap_text("Guest");
    run(300);
    shot("user_page");

    printf("Wi-Fi, date and time, integrations: the master's code opens them\n");
    tap_name("set_wifi");
    run(2500);
    shot("settings_wifi");
    /* BL-131: the joined network is one row that opens its page; Forget isn't on this page. */
    expect(find_label(by_name("page_body"), "Forget this network") == NULL,
           "Forget is still on the Wi-Fi page");
    expect(find_label(by_name("page_body"), "Other networks nearby") != NULL,
           "the Wi-Fi page doesn't say the list is of the other networks");
    expect(find_label(lv_screen_active(), "Caf\xC3\xA9 Guest") != NULL,
           "a Wi-Fi network named outside ASCII isn't in the list (I18N-009)");
    tap_text("Maple Street");
    sheet_text("correct horse");
    shot("wifi_password");
    /* BL-131: a password's field ends in an eye. A tap shows what was typed and another hides
     * it, the cursor staying put; every sheet opens with it hidden. */
    {
        lv_obj_t *ta = by_name("sheet_text");
        expect(find_visible(lv_screen_active(), "sheet_eye") != NULL,
               "the password field has no eye");
        expect(lv_textarea_get_password_mode(ta), "the password sheet opened in the clear");
        tap_name("sheet_eye");
        run(100);
        expect(!lv_textarea_get_password_mode(ta) &&
                   strcmp(lv_label_get_text(lv_textarea_get_label(ta)), "correct horse") == 0,
               "the eye didn't show the password");
        expect(lv_obj_has_state(ta, LV_STATE_FOCUSED), "the eye took the cursor from the field");
        shot("wifi_password_shown");
        tap_name("sheet_eye");
        run(100);
        expect(lv_textarea_get_password_mode(ta), "a second tap on the eye didn't hide it");
        tap_name("sheet_eye"); /* shown when the sheet closes ... */
        run(100);
        tap_name("sheet_cancel");
        run(200);
        tap_text("Maple Street"); /* ... and hidden when it opens again */
        run(200);
        expect(lv_textarea_get_password_mode(ta) && lv_subject_get_int(&subject_sheet_shown) == 0,
               "the password sheet didn't open hidden");
    }
    tap_name("sheet_cancel");
    run(200);

    printf("Wi-Fi > the joined network: addresses and the connection check\n");
    tap(lv_obj_get_child(by_name("page_body"), 0)); /* the joined network's row, on top */
    run(300);
    expect(find_label(by_name("page_body"), "IP address settings") != NULL,
           "the joined network's page didn't open");
    shot("network_address");
    tap_text("Check the connection");
    run(1500);
    scroll_to_text("page_body", "Name lookup");
    shot("network_check_running");
    run(6000);
    scroll_to_text("page_body", "Connected service");
    shot("network_check_passed");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    tap_text("IP address settings");
    run(200);
    shot("network_address_mode");
    tap_text("A fixed address");
    run(7000); /* the address changed: the check runs again by itself */
    shot("network_static");
    tap_text("Router"); /* the setting, above the check's own "Router" line */
    sheet_text("192.168.1.9");
    tap_name("sheet_done");
    run(7000);
    scroll_to_text("page_body", "Internet");
    shot("network_check_failed");
    expect(find_label(by_name("page_body"), "192.168.1.9 didn't answer*") != NULL,
           "the check didn't name the wrong router");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    tap_text("Router");
    sheet_text("192.168.1.1");
    tap_name("sheet_done");
    run(7000);
    tap_text("IP address settings");
    run(200);
    tap_text("Automatic");
    run(7000);

    /* What the check says about each way the house network can break. Done here rather than
     * on a panel that is protecting a house (docs/features/09 NET-012). */
    printf("The connection check against a broken network\n");
    host_panel_net_fault(HOST_NET_DNS_DOWN, true);
    tap_text("Check again");
    run(7000);
    expect(find_label(by_name("page_body"), "192.168.1.1 didn't answer for pool.ntp.org") != NULL,
           "a broken name lookup wasn't reported");
    expect(find_label(by_name("page_body"), "1.1.1.1 answered on port 443") != NULL,
           "a broken name lookup hid a working internet");
    host_panel_net_fault(HOST_NET_DNS_DOWN, false);

    host_panel_net_fault(HOST_NET_INTERNET_DOWN, true);
    tap_text("Check again");
    run(7000);
    expect(find_label(by_name("page_body"), "Nothing answered out there*") != NULL,
           "the internet being down wasn't reported");
    expect(find_label(by_name("page_body"), "pool.ntp.org is 162.159.200.1") != NULL,
           "the internet being down hid a working name lookup");
    host_panel_net_fault(HOST_NET_INTERNET_DOWN, false);

    host_panel_net_fault(HOST_NET_NO_LEASE, true);
    tap_text("Check again");
    run(7000);
    scroll_to_text("page_body", "Address");
    shot("network_check_no_address");
    expect(find_label(by_name("page_body"), "The router gave out no address (DHCP)") != NULL,
           "a missing DHCP lease wasn't reported");
    expect(find_label(by_name("page_body"), "The panel has no address yet") != NULL,
           "the steps after a missing address were still run");
    host_panel_net_fault(HOST_NET_NO_LEASE, false);

    host_panel_net_fault(HOST_NET_WIFI_DOWN, true);
    run(1200);
    tap_text("Check again");
    run(7000);
    expect(find_label(by_name("page_body"), "Harbor House didn't answer") != NULL,
           "the access point going away wasn't reported");
    expect(find_label(by_name("page_body"), "The panel isn't on the network") != NULL,
           "the steps after a lost network were still run");
    host_panel_net_fault(HOST_NET_WIFI_DOWN, false);
    run(2000);

    /* Forget sits last, apart; a cancelled question leaves the network joined. */
    scroll_to_text("page_body", "Forget this network");
    lv_obj_scroll_to_y(by_name("page_body"), LV_COORD_MAX, LV_ANIM_OFF); /* the whole button */
    run(50);
    shot("network_forget");
    tap_text("Forget this network");
    run(200);
    expect(find_label(lv_screen_active(), "Forget this network?") != NULL,
           "Forget didn't ask first");
    tap_name("dialog_cancel");
    run(200);
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    tap_name("page_back");
    run(300);

    tap_name("set_time");
    run(300);
    shot("settings_time");
    tap_name("set_integrations");
    run(300);
    shot("integrations");
    expect(find_label_with(by_name("page_body"), "Notifications") == NULL &&
               find_label_with(by_name("page_body"), "pairs with the panel itself") == NULL,
           "the Integrations list still has a paragraph or the old group name (BL-131)");
    expect(find_label(by_name("page_body"), "Alerts to your phone") != NULL,
           "the Integrations list has no 'Alerts to your phone' group (BL-131)");
#if CONFIG_ALARM_FEATURE_HA
    tap_text("Home Assistant");
    run(300);
    shot("ha_integration"); /* the integration part, still off */
    expect(find_label_with(by_name("page_body"), "Off · switch it on") != NULL,
           "the Home Assistant switch doesn't say it is off (BL-131)");
    tap_text("Home Assistant");
    run(1500); /* the status poll rebuilds the page to Ready */
    shot("ha_ready");
    tap_text("Pair with Home Assistant");
    run(1500);
    shot("ha_pairing_wait");
    host_panel_ha_pair_request(); /* the pretend Home Assistant commits and reveals */
    run(1500);
    expect(find_label(by_name("page_body"), "588 460") != NULL,
           "the pairing code isn't on the screen (HAI-003, golden vector 1)");
    shot("ha_pairing_code");
    tap_text("Yes, allow");
    run(1500);
    expect(find_label_with(by_name("page_body"), "paired") != NULL,
           "the page doesn't say it is paired after Allow (BL-131)");
    expect(find_label_with(by_name("page_body"), "aioroboalarms") == NULL,
           "the library's name is on the Home Assistant page (BL-131)");
    shot("ha_paired");

    tap_name("page_back"); /* MQTT is its own entry in Integrations, not under Home Assistant */
    run(300);
#endif
#if CONFIG_ALARM_FEATURE_MQTT
    tap_text("MQTT");
    run(300);
#if CONFIG_ALARM_FEATURE_HA
    expect(find_label(by_name("page_body"), "Paired with the Home Assistant integration*") != NULL,
           "MQTT doesn't say why discovery stays quiet while the integration is paired (HAI-010)");
#endif
    shot("mqtt_setup");
    tap_text("Address");
    sheet_text("homeassistant.local");
    shot("mqtt_address");
    tap_name("sheet_done");
    run(300);
    tap_text("User name");
    sheet_text("alarmpanel");
    tap_name("sheet_done");
    run(300);
    tap_text("Password");
    sheet_text("s3cret-mqtt");
    tap_name("sheet_done");
    run(300);
    tap_text("Connect to the broker");
    run(3000);
    shot("mqtt_connected");
    tap_text("Security");
    run(300);
    shot("mqtt_security");
    tap_text("Not encrypted");
    run(3000);
    scroll_to_text("page_body", "Not encrypted: anyone*");
    shot("mqtt_not_encrypted");
#endif

    /* BL-131 finding 7: the event log is Activity's now, and Settings has no Event log line.
     * The two pictures keep their names and numbers; they show Activity. */
    tap_name("nav_activity");
    run(300);
    shot("event_log");
    /* LOG-010: a latched loss shows only after the private step; a master sees it, only the
     * installer clears it. */
    host_panel_log_loss(12, 3);
    run(1200);
    expect(find_label(by_name("event_list"), "12 events couldn't be saved") == NULL,
           "LOG-010/LOG-008: the loss warning shows before a master or installer code");
    scroll_to_text("event_list", "Show private events");
    tap_text("Show private events");
    run(300);
    type("123456k");
    run(300);
    lv_obj_scroll_to_y(by_name("event_list"), 0, LV_ANIM_OFF);
    run(100);
    expect(find_label(by_name("event_list"), "12 events couldn't be saved") != NULL,
           "LOG-010: the lost events aren't shown on Activity after a master code");
    expect(find_label(by_name("event_list"), "Clear the warning") == NULL,
           "LOG-010: a master code must not clear the log's loss");
    shot("event_log_loss");
    tap_name("bar_settings"); /* back where the walk was; leaving Activity forgot the code */
    run(300);
    expect(find_label(lv_screen_active(), "Event log") == NULL &&
               find_label(lv_screen_active(), "Walk test") != NULL,
           "BL-131 finding 7: Settings still has an Event log line");
    {
        /* With Automations shown (nine lines, docs/features/41) About, the last line, stays above
         * the fold: the Installer is a row on About now (BL-189). */
        lv_subject_set_int(&subject_automations_avail, 1);
        run(100);
        lv_obj_t *about = find_label(lv_screen_active(), "About");
        lv_area_t a = {0};
        if (about != NULL) {
            lv_obj_get_coords(about, &a);
        }
        expect(about != NULL && a.y2 < H && find_label(lv_screen_active(), "Automations") != NULL,
               "BL-131 finding 7: About falls below the fold with Automations shown");
        expect(find_label(lv_screen_active(), "Installer") == NULL,
               "BL-189: the Settings list still has an Installer line");
        lv_subject_set_int(&subject_automations_avail, 0);
        run(100);
    }

    printf("Installer mode\n");
    open_installer();
    run(300);
    shot("installer_locked");
    expect(find_label_with(by_name("page_body"), "69420") == NULL,
           "BL-187: the locked Installer page shows the factory code");
    expect(find_label(by_name("page_body"), "Enter the installer code") != NULL,
           "BL-189: the locked Installer page lost its code row");
    /* BL-131: the lost-code explanation is a row of its own, opened before the code */
    tap_text("Lost the installer code?");
    run(300);
    expect(find_label_with(by_name("page_body"), "Unplug the panel for ten seconds") != NULL,
           "BL-131: the Lost the installer code? row doesn't open its explanation");
    shot("installer_lost_code");
    tap_name("page_back");
    run(300);
    tap_text("Enter the installer code");
    type("69420k");
    run(300);
    shot("installer_menu");
    update_server_shots();
    scroll_to_text("page_body", "Installer and master codes");
    tap_text("Installer and master codes");
    run(300);
    expect(find_label_with(by_name("page_body"), "69420") == NULL &&
               find_label_with(by_name("page_body"), "123456") == NULL,
           "BL-187: the codes page shows factory code digits");
    expect(find_label(by_name("page_body"), "Still the factory code") != NULL,
           "BL-187: the codes page lost 'Still the factory code'");
    shot("installer_codes");
    tap_name("page_back");
    run(300);
    {
        /* BL-131 finding 18/25: four groups, the risky ones last, the Panel Manager in one
         * place, no note repeating the banner, "None" for an empty count. */
        lv_obj_t *body = by_name("page_body");
        static const char *const order[] = {"Sensors and devices", "Zones", "Partitions",
                                            "Keypads and devices", "How the alarm works",
                                            "Delays and arming", "This panel", "System name",
                                            "Installer and master codes", "Test and start over",
                                            "Walk test", "Run the setup again", "Factory reset",
                                            "Leave installer mode"};
        int32_t last_y = -1;
        for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
            lv_obj_t *l = find_label(body, order[i]);
            expect(l != NULL, "the installer menu lacks a row or group");
            if (l != NULL) {
                lv_area_t a;
                lv_obj_get_coords(l, &a);
                expect(a.y1 > last_y, "the installer menu's rows are out of the drawn order");
                last_y = a.y1;
            }
        }
        expect(find_label(lv_screen_active(), "Installer mode · arming is off until you leave") ==
                   NULL,
               "the installer menu repeats the banner in a note");
        expect(find_label(body, "System options") == NULL && find_label(body, "Web server") == NULL &&
                   find_label(body, "Remote access") == NULL,
               "the installer menu keeps a row it moved or renamed");
        expect(find_label(body, "None") != NULL, "a count of none doesn't read None");
        expect(find_label(body, "0 in use") == NULL, "a count of none reads 0 in use");
    }
#if CONFIG_ALARM_PANEL_WEB /* the Panel Manager's switches (docs/features/31) */
    {
        bool web, developer;
        be.remote_get(be.ctx, &web, &developer);
        expect(!web && !developer, "WEB-003: fresh remote access must be off");
        /* the Panel Manager in one place: its switch, the installer's sign-in under it */
        lv_obj_t *pm = find_label_with(by_name("page_body"), "Change settings from a browser");
        lv_obj_t *signin = find_label_with(by_name("page_body"), "User name: Installer");
        lv_obj_t *dev = find_label(by_name("page_body"), "Developer mode");
        lv_area_t pa = {0}, sa = {0}, da = {0};
        if (pm != NULL && signin != NULL && dev != NULL) {
            lv_obj_get_coords(pm, &pa);
            lv_obj_get_coords(signin, &sa);
            lv_obj_get_coords(dev, &da);
        }
        expect(pm != NULL && signin != NULL && dev != NULL && pa.y1 < sa.y1 && sa.y1 < da.y1,
               "the Panel Manager isn't one switch with the installer's sign-in under it");
        tap_text("Developer mode");
        run(300);
        be.remote_get(be.ctx, &web, &developer);
        expect(web && developer, "WEB-003: developer mode must enable the web server");
        expect(find_label_with(lv_screen_active(), "https://192.0.2.10/") != NULL,
               "WEB-002: local Installer must show the panel URL and certificate identity");
        shot("installer_menu_panel_manager");
        tap_text("Panel Manager");
        run(300);
        be.remote_get(be.ctx, &web, &developer);
        expect(!web && !developer, "WEB-003: stopping web access must stop developer access too");
    }
#endif
#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
    expect(!ui_dev_control_begin(), "DEV-010: remote control inherited local installer session");
#endif
    tap_text("Delays and arming");
    run(300);
    expect(find_label(lv_screen_active(), "Delays and arming") != NULL,
           "the options page's title doesn't follow its row");
    shot("installer_options");
    {
        lv_obj_t *body = by_name("page_body");
        /* A tap on minus still steps the setting. */
        lv_obj_t *minus = by_name("minus");
        lv_obj_t *value = lv_obj_find_by_name(lv_obj_get_parent(minus), "value_label");
        char was[32];
        snprintf(was, sizeof(was), "%s", lv_label_get_text(value));
        tap(minus);
        expect(strcmp(lv_label_get_text(value), was) != 0,
               "System options: a tap on minus doesn't step the setting");
        tap(lv_obj_find_by_name(lv_obj_get_parent(minus), "plus")); /* put it back */
        expect(strcmp(lv_label_get_text(value), was) == 0,
               "System options: a tap on plus doesn't step the setting back");
        /* A drag that starts on minus scrolls the page and leaves the setting alone. */
        const int32_t top = lv_obj_get_scroll_y(body);
        lv_area_t ma;
        lv_obj_get_coords(minus, &ma);
        drag((ma.x1 + ma.x2) / 2, (ma.y1 + ma.y2) / 2, -150);
        expect(lv_obj_get_scroll_y(body) - top >= 40,
               "System options: dragging from minus doesn't scroll the page");
        expect(strcmp(lv_label_get_text(value), was) == 0,
               "System options: dragging from minus changed the setting");
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
        run(100);
        /* And a finger on the setting's name scrolls it just as well. */
        expect_drag_scrolls("page_body", find_label(body, "Exit time"),
                            "System options, a finger on a setting's name");
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
        /* AUT-015, U23: the installer's switch, after Arm without a code */
        expect(find_label(body, "Allow less secure automations") != NULL &&
                   find_label(body, "Automations may disarm, unlock and open") != NULL,
               "AUT-015: System options has no Allow less secure automations switch");
#endif
    }
    tap_name("page_back");
    tap_text("Zones");
    run(300);
    shot("installer_zones");
    tap_text("Hallway Motion");
    run(300);
    tap_text("Night zone");
    run(200);
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF); /* the page's top, as drawn */
    run(100);
    shot("zone_edit");
    {
        /* BL-131 finding 23: the zone right now on top (UI-023: live words, not a button), then
         * The zone, Its sensor, What it does, Advanced; Contact and Response time only on Sensor
         * settings; Alarm limit one picker; Remove last. */
        lv_obj_t *body = by_name("page_body");
        lv_obj_t *top = lv_obj_get_child(body, 0);
        expect(top != NULL && !lv_obj_has_flag(top, LV_OBJ_FLAG_CLICKABLE) &&
                   lv_obj_find_by_name(top, "title_label") != NULL,
               "UI-023: the zone page doesn't start with how the zone is right now");
        expect(find_label_with(body, "Simulator · ") != NULL,
               "the zone's top row doesn't say its source and what it means");
        static const char *const order[] = {"The zone", "Name", "Type", "Its sensor", "Sensor",
                                            "Source", "Sensor settings", "What it does",
                                            "Night zone", "Chime", "Report this zone", "Advanced",
                                            "Cross-zone partner", "Alarm limit",
                                            "Remove this zone"};
        int32_t last_y = -1;
        for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
            lv_obj_t *l = find_label(body, order[i]);
            expect(l != NULL, "the zone page lacks a row or group");
            if (l != NULL) {
                lv_area_t a;
                lv_obj_get_coords(l, &a);
                expect(a.y1 > last_y, "the zone page's rows are out of the drawn order");
                last_y = a.y1;
            }
        }
        expect(find_label(body, "Right now") == NULL && find_label(body, "Never shut down") == NULL &&
                   find_label(body, "Supervision") == NULL && find_label(body, "Contact") == NULL &&
                   find_label(body, "Response time") == NULL,
               "the zone page keeps a row that moved");
        expect(find_label(body, "Its events go to phones and integrations, not only the event log") !=
                   NULL,
               "Report this zone (on) doesn't say what on does");
        expect(find_label_with(body, " · not supervised") != NULL,
               "the Sensor settings row doesn't say what they are set to");
        /* scrolled to the sensor's group, then to the end (n-zone-edit-2, -3) */
        lv_obj_scroll_to_y(body, lv_obj_get_y(lv_obj_get_parent(find_label(body, "Its sensor"))),
                           LV_ANIM_OFF);
        run(100);
        shot("zone_edit_sensor");
        lv_obj_scroll_by_bounded(body, 0, -10000, LV_ANIM_OFF);
        run(100);
        shot("zone_edit_end");
        /* Report's detail follows its switch at once */
        tap_text("Report this zone");
        run(200);
        expect(find_label(body, "Its events stay in the panel's event log only") != NULL,
               "Report this zone (off) doesn't say what off does");
        tap_text("Report this zone");
        run(200);
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
        run(100);
    }
    tap_text("Type");
    run(300);
    shot("zone_type");
    expect_drag_from_control("pick_list", "the choices picker");
    expect(lv_subject_get_int(&subject_sheet) == 2,
           "scrolling the choices picker picked one");
    tap_name("pick_close");
    tap_name("page_back");
    run(200);
    tap_name("page_back");
    run(200);
    tap_text("Factory reset");
    run(300);
    lv_obj_t *hold = find_label(lv_screen_active(), "Hold to erase everything");
    lv_area_t ha;
    lv_obj_get_coords(lv_obj_get_parent(hold), &ha);
    s_px = (ha.x1 + ha.x2) / 2;
    s_py = (ha.y1 + ha.y2) / 2;
    s_pressed = true;
    run(1100);
    shot("factory_reset");
    s_pressed = false;
    run(300);
    tap_name("nav_home"); /* leaving Settings forgets the codes: Software update must ask */
    run(300);
    tap_name("bar_settings");
    run(300);
    tap_name("set_about");
    run(300);
    run(3500);
    shot("about");
    expect(find_label(by_name("page_body"), "RoboAlarms Panel") != NULL, "About lost the panel's line");
    expect(find_label(by_name("page_body"), "Storage") == NULL, "About still has a Storage page");
    expect(find_label(by_name("page_body"), "System details") == NULL,
           "BL-189: About still has a System details row");
    expect(find_label(by_name("page_body"), "Running for") != NULL, "About lost Running for");
    expect(find_label(by_name("page_body"), "Panel ID") == NULL && find_label(by_name("page_body"), "Version") == NULL,
           "BL-189: About repeats the Panel ID or Version row");
    expect(find_label(by_name("page_body"), "Up to date") != NULL ||
               find_label(by_name("page_body"), "Update") != NULL,
           "BL-189: Software update's row lost its value");
    expect_drag_scrolls("page_body", find_label(by_name("page_body"), "Running for"),
                        "About, a finger on the Running for line");
    expect(find_label_with(by_name("page_body"), " d ") == NULL, "Running for is not in words");
    lv_obj_scroll_to_y(by_name("page_body"), LV_COORD_MAX, LV_ANIM_OFF); /* the end: the licence line */
    run(100);
    expect(find_label(by_name("page_body"), "Firmware space") != NULL, "About lost Firmware space");
    expect(find_label(by_name("page_body"), "Installer") != NULL, "About lost the Installer row");
    shot("about_end");
    scroll_to_text("page_body", "Software update");
    tap_text("Software update");
    run(300);
    expect(lv_subject_get_int(&subject_settings_pad) == 1,
           "BL-188: Software update opened without the master or installer code");
    shot("software_update_locked");
    type("123456k");
    run(1200);
    expect(find_label(by_name("page_body"), "Update source") == NULL &&
               find_label(by_name("page_body"), "Advanced") == NULL,
           "BL-189: Software update still has the update source");
    expect(find_label(by_name("page_body"), "Check again") != NULL, "Software update lost Check again");
    expect(find_label_with(by_name("page_body"), "Checked") == NULL,
           "Check again says it checked before any check");
    tap_text("Check again");
    run(1200);
    expect(find_label_with(by_name("page_body"), "Checked ") != NULL,
           "Check again doesn't say when it last checked");
    shot("about_update_available");
    tap_name("page_back");
    shot("about_update_install");
    expect(find_label(by_name("page_body"), "Update") != NULL, "About does not say an update waits");
    tap_text("Software update");
    const ui_fw_status_t waiting = {
        .phase = UI_FW_WAITING, .running = "0.6.0", .latest = "0.6.1", .available = true,
        .has_source = true, .percent = -1, .reason = "The system is armed",
    };
    host_panel_fw_preset(&waiting);
    run(1200);
    shot("about_update_waiting");
    const ui_fw_status_t undone = {
        .phase = UI_FW_IDLE, .running = "0.6.0", .latest = "0.6.0", .has_source = true,
        .percent = -1, .rolled_back = "0.6.1",
    };
    host_panel_fw_preset(&undone);
    run(1200);
    shot("about_rolled_back");
    {
        /* BL-186: the version it runs was withdrawn; an important fix on offer */
        ui_fw_status_t wd = {
            .phase = UI_FW_IDLE, .running = "0.6.0", .latest = "0.6.1", .available = true,
            .has_source = true, .percent = -1, .withdrawn = true, .critical = true,
        };
        fw_shot(&wd, "upd_withdrawn");
        expect(find_label(by_name("page_body"), "Version 0.6.0 was withdrawn") != NULL &&
                   find_label_with(by_name("page_body"), "0.6.1 is an important fix") != NULL &&
                   find_label(by_name("page_body"), "Install version 0.6.1") != NULL,
               "BL-186: the withdrawn row lacks its words");
        wd.available = false;
        wd.critical = false;
        fw_shot(&wd, "upd_withdrawn_alone");
        expect(find_label(by_name("page_body"), "Its publisher took it back") != NULL &&
                   find_label(by_name("page_body"), "Install version 0.6.1") == NULL,
               "BL-186: the withdrawn row alone is wrong");
        wd = (ui_fw_status_t){.phase = UI_FW_IDLE, .running = "0.6.0", .latest = "0.6.1", .available = true,
                              .has_source = true, .percent = -1, .critical = true};
        fw_shot(&wd, "upd_critical");
        expect(find_label_with(by_name("page_body"), "An important fix") != NULL, "BL-186: no important fix");
        fw_shot(&undone, "upd_critical_done");
    }
    update_progress_shots();

    printf("First-boot setup\n");
    open_installer();
    tap_text("Enter the installer code");
    type("69420k");
    run(300);
    host_panel_forget_wifi();
    tap_text("Run the setup again");
    run(300);
#ifdef ALARM_PREVIEW_LOCALE
    /* I18N-012: with more than one language the setup asks for it first. */
    expect(find_label(by_name("wiz_body"), "Region") != NULL, "the setup didn't ask for the language");
    shot("setup_language");
    tap_name("wiz_next");
    run(300);
#endif
    shot("setup_welcome");
    tap_name("wiz_next");
    run(2000);
    tap_text("Harbor House");
    sheet_text("correct horse");
    tap_name("sheet_done");
    run(3500);
    shot("setup_wifi");
    tap_name("wiz_next");
    run(300);
    shot("setup_time_zone");
    expect_drag_from_control("wiz_body", "the setup time-zone step");
    expect_step(2, "after scrolling the time zones");
    lv_obj_scroll_to_y(by_name("wiz_body"), 0, LV_ANIM_OFF);
    run(100);
    tap_name("wiz_next");
    run(300);
    tap_text("Installer code");
    type("8642");
    shot("setup_new_code");
    type("0k");
    type("86420k");
    run(300);
    shot("setup_codes");
    tap_name("wiz_next");
    run(300);
    tap_name("wiz_next");
    run(300);
    shot("setup_done");
    expect(find_visible(lv_screen_active(), "wiz_exit") == NULL,
           "setup: the last step still offers Leave setup");
    expect(lv_subject_get_int(&subject_toast_visible) == 0,
           "setup: a toast is still up on the last step");
    tap_name("wiz_next");
    run(500);
    shot("home_after_setup");

    printf("Lost installer code: power-up recovery\n");
    host_panel_init(1789857660000LL + 3600000LL); /* unplugged and back an hour later */
    run(3000);
    tap_name("bar_settings");
    open_installer();
    run(300);
    shot("installer_recovery");
    tap_text("Forgot the code?*");
    run(200);
    shot("recovery_confirm");
    tap_name("dialog_ok");
    run(300);
    shot("recovery_new_code");

    printf("Shared sections ask for a master or installer code\n");
    tap_name("nav_home");
    run(300);
    host_panel_init(1789857660000LL + 7200000LL);
    run(5000); /* let the toast go */
    tap_name("bar_settings");
    tap_name("set_wifi");
    run(300);
    shot("wifi_locked");

    printf("Setup with Wi-Fi skipped and a time zone picked: every button answers\n");
    host_panel_forget_wifi();
    ui_app_run_setup();
    run(300);
    past_the_language();
    expect_step(0, "welcome");
    tap_name("wiz_next");
    expect_step(1, "Start");
    run(3000);
    tap_name("wiz_next"); /* Skip for now */
    expect_step(2, "Skip for now");
    tap_text("Central Time");
    run(300);
    expect_step(2, "picking a time zone");
    tap_name("wiz_next");
    expect_step(3, "Next after picking a time zone");
    tap_name("wiz_next"); /* Next, factory codes kept */
    expect_step(4, "Next with the factory codes kept");
    tap_name("wiz_next");
    expect_step(5, "Next on the name step");
    tap_name("wiz_next"); /* Done */
    run(300);
    if (find_visible(lv_screen_active(), "wiz_next") != NULL) {
        fprintf(stderr, "setup: Done didn't leave the setup\n");
        exit(1);
    }

    printf("The setup run again can be left from any step\n");
    ui_app_run_setup();
    run(300);
    expect(find_visible(lv_screen_active(), "wiz_exit") != NULL,
           "setup: run again with no way out on its first step");
    past_the_language();
    tap_name("wiz_next");
    run(3000); /* the Wi-Fi step, mid-scan */
    expect_step(1, "Start");
    expect(find_visible(lv_screen_active(), "wiz_exit") != NULL,
           "setup: run again with no way out on the Wi-Fi step");
    tap_name("wiz_exit");
    run(300);
    expect(find_visible(lv_screen_active(), "wiz_next") == NULL,
           "setup: Leave setup didn't leave the setup");
    expect(lv_subject_get_int(&subject_view) == 0, "setup: Leave setup didn't land on Home");

    printf("Screen saver while armed\n");
    tap(lv_obj_get_child(by_name("panel_arm"), 0)); /* Away */
    type("123456k");
    run(65000); /* exit time, 5 s armed */
    expect_saver(false, "5 s after exit time");
    run(57000); /* a minute after arming finished */
    expect_saver(true, "a minute after exit time");
    shot("screen_saver_armed");

    printf("Armed: Settings, chime and codes are locked\n");
    wake();
    expect(lv_subject_get_int(&subject_locked) == 1, "armed: not locked");
    expect(lv_subject_get_int(&subject_bar_banner) != 1, "armed: the change-code banner shows");
    tap_name("bar_settings");
    run(100);
    expect(lv_subject_get_int(&subject_view) != 5, "armed: Settings opened");
    shot("armed_locked");
    const int32_t chime_was = lv_subject_get_int(&subject_chime);
    tap_name("bar_chime");
    run(100);
    expect(lv_subject_get_int(&subject_modal_pad) == 0, "armed: the chime panel opened");
    expect(find_label(lv_screen_active(), "Turn chime on") == NULL &&
               find_label(lv_screen_active(), "Turn chime off") == NULL,
           "armed: the chime pad opened");
    expect(lv_subject_get_int(&subject_chime) == chime_was, "armed: the chime changed anyway");
    expect(lv_subject_get_int(&subject_panel) == 2, "armed: the Disarm pad didn't come back");
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_locked) == 0, "disarmed: still locked");

    printf("Instant: quiet exit time, then no entry delay\n");
    tap(lv_obj_get_child(by_name("panel_arm"), 0)); /* Away */
    tap_text("No entry delay");
    expect(find_label(lv_screen_active(), "Arm Away") != NULL,
           "Away + No entry delay: the pad title is not the level");
    tap_name("pad_close");
    tap(lv_obj_get_child(by_name("panel_arm"), 1)); /* Stay */
    expect(find_label(lv_screen_active(), "Silent exit") == NULL, "Stay offers silent exit");
    tap_text("No entry delay");
    type("123456k");
    run(5000);
    shot("exit_instant");
    type("123456k");
    run(300);

    printf("Arming from elsewhere closes Settings and forgets the code\n");
    tap_name("bar_settings");
    tap_name("set_users");
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_settings_pad) == 0, "Users didn't open");
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_AWAY;
    snprintf(cmd.code, sizeof(cmd.code), "%s", "123456");
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "remote arming failed");
    run(300);
    expect(lv_subject_get_int(&subject_view) == 0, "remote arming left Settings open");
    cmd.type = ALARM_CMD_DISARM;
    expect(host_panel_remote_command(&cmd) == ALARM_OK, "remote disarming failed");
    run(5000);
    tap_name("bar_settings");
    tap_name("set_users");
    run(300);
    expect(lv_subject_get_int(&subject_settings_pad) == 1, "Users opened without a code");

    printf("Zone sources: a wired zone, then a sensor that stops answering\n");
    open_installer();
    run(300);
    tap_text("Enter the installer code");
    type("69420k");
    run(300);
    tap_text("Zones");
    run(300);
    expect(find_label(lv_screen_active(), "Zone 5 · Interior follower · Simulator") != NULL,
           "the zone list doesn't say where the sensors are");
    tap_text("Add a zone");
    run(300);
    shot("zone_source_pick");
    tap_text("Wired contact");
    run(300);
    host_panel_wire(4, true); /* the installer opens the new door's contact */
    run(1500);
    expect(find_label(lv_screen_active(), "J7 header · Just opened or closed") != NULL,
           "the terminal that was opened isn't marked");
    shot("zone_terminals");
    tap_text("GPIO 4");
    run(500);
    expect(find_label(lv_screen_active(), "Wired · GPIO 4") != NULL, "zone 9 isn't on GPIO 4");
    expect(find_label(lv_screen_active(), "Open") != NULL, "zone 9 doesn't show the open door");
    host_panel_wire(4, false);
    run(4000); /* the contact settles */
    /* BL-131 finding 23: the source and what it means are the top row's detail */
    lv_obj_t *facts =
        find_label(lv_screen_active(), "Wired · Works without Wi-Fi. A cut wire shows as open.");
    expect(facts != NULL, "zone 9 doesn't say what a wired contact can promise");
    expect(find_label(by_name("page_body"), "Contact") == NULL,
           "a wired zone's Contact is still on the zone page, not only in Sensor settings");
    expect(find_label_with(by_name("page_body"), "Normally closed · 350 ms · ") != NULL,
           "a wired zone's Sensor settings row doesn't say its contact and response time");
    expect(find_label(lv_screen_active(), "Closed") != NULL, "zone 9 doesn't follow its contact");
    scroll_into_view(facts);
    run(100);
    shot("zone_wired");
    tap_name("page_back");
    run(300);

    /* ZONE-005 (BL-063): a wired zone on a terminal that measures, made an end-of-line loop in
     * its Sensor settings; a short across the loop is its tamper */
    tap_text("Add a zone");
    run(300);
    tap_text("Wired contact");
    run(300);
    host_panel_wire(50, true);
    run(1500);
    tap_text("GPIO 50");
    run(500);
    host_panel_wire(50, false);
    run(1500);
    scroll_to_text("page_body", "Sensor settings");
    tap_text("Sensor settings");
    run(300);
    tap_text("Contact");
    run(300);
    expect(find_label(by_name("pick_list"), "Closed, end-of-line resistor") != NULL,
           "ZONE-005: a terminal that measures doesn't offer end-of-line loops");
    shot("zone_eol_contact");
    tap_text("Closed, end-of-line resistor");
    run(1500);
    expect(strcmp(row_label("Contact", "value_label"), "Closed, end-of-line resistor") == 0,
           "ZONE-005: the zone's Contact isn't its end-of-line loop");
    host_panel_loop_ohms(50, 0u); /* someone bridges the loop */
    run(1500);
    shot("zone_eol_short");
    host_panel_loop_ohms(50, ZEOL_OHM);
    run(1500);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);

#if CONFIG_ALARM_FEATURE_HA
    printf("A Home Assistant entity as a zone (M13, ZSRC-005, HAI-009)\n");
    tap_text("Add a zone");
    run(300);
    tap_text("Home Assistant sensor"); /* usable: the pretend integration paired earlier */
    run(300);
    expect(find_label(lv_screen_active(), "Porch Door") != NULL,
           "the shared catalog isn't in the picker");
    shot("zone_ha_catalog");
    tap_text("Find the sensor");
    sheet_text("porch");
    tap_name("sheet_done");
    run(300);
    expect(find_label(lv_screen_active(), "Porch Door") != NULL, "the search lost the match");
    expect(find_label(lv_screen_active(), "Garage Motion") == NULL,
           "the search doesn't narrow the catalog");
    shot("zone_ha_search");
    tap_text("Porch Door");
    run(500);
    expect(find_label(lv_screen_active(), "Home Assistant · Porch Door") != NULL,
           "zone 10 doesn't say its sensor lives in Home Assistant");
    host_panel_ha_entity("binary_sensor.porch_door", "on", true);
    run(1500);
    expect(find_label(lv_screen_active(), "Open") != NULL,
           "the entity turning on doesn't open the zone");
    shot("zone_ha_bound");
    host_panel_ha_entity("binary_sensor.porch_door", "off", true);
    run(1500);
    tap_name("page_back");
    run(300);
#endif

    host_panel_zone(5, ALARM_ZEV_UNREACHABLE); /* the hallway sensor stops answering */
    run(61000);
    tap_text("Hallway Motion");
    run(300);
    expect(find_label(lv_screen_active(), "Not reporting") != NULL,
           "an unreachable sensor doesn't make its zone CHECK");
    shot("zone_unreachable");

    /* Chime is switched where you are, never on Home (UI-025). Last, so the screenshots the
     * feature docs point at keep their numbers. */
    printf("Chime asks over the view you are on\n");
    tap_name("nav_zones");
    run(300);
    const int32_t chime_off = lv_subject_get_int(&subject_chime);
    tap_name("bar_chime");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "Chime didn't open the panel");
    expect(lv_subject_get_int(&subject_view) == 1, "Chime left the Zones view");
    expect(lv_subject_get_int(&subject_panel) != 2, "Chime put the pad on Home as well");
    expect(find_label(lv_screen_active(), "Turn chime on") != NULL,
           "the chime panel doesn't say what it will do");
    shot("chime_panel");
    tap_name("pad_close"); /* the X changes nothing */
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0, "the X didn't close the chime panel");
    expect(lv_subject_get_int(&subject_chime) == chime_off, "closing the panel changed the chime");
    tap_name("bar_chime");
    type("123456k");
    run(300);
    expect(lv_subject_get_int(&subject_chime) != chime_off, "the code didn't switch the chime");
    expect(lv_subject_get_int(&subject_modal_pad) == 0, "the chime panel stayed open");
    expect(lv_subject_get_int(&subject_view) == 1, "the chime panel left the Zones view behind it");

    /* The same panel carries a bypass, with the longest line of plain words. */
    lv_obj_t *zl = by_name("zone_list");
    lv_obj_t *bypass = lv_obj_find_by_name(lv_obj_get_child(zl, 0), "bypass");
    expect(bypass != NULL, "the first zone card has no Bypass button");
    tap(bypass);
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 1, "Bypass didn't open the panel");
    expect(lv_subject_get_int(&subject_view) == 1, "Bypass left the Zones view");
    shot("bypass_panel");
    tap_name("pad_close");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0, "the X didn't close the bypass panel");

    printf("Chime by one touch once the installer allows it\n");
    tap_name("bar_settings");
    open_installer();
    tap_text("Enter the installer code");
    type("69420k");
    run(300);
    tap_text("Delays and arming");
    run(300);
    tap_text("Chime needs a code"); /* off: one touch (CORE-CHIME-005) */
    run(100);
    tap_name("page_back");
    run(300);
    const int32_t chime_on = lv_subject_get_int(&subject_chime);
    tap_name("bar_chime");
    run(300);
    expect(lv_subject_get_int(&subject_modal_pad) == 0, "one-touch chime still asked for a code");
    expect(lv_subject_get_int(&subject_chime) != chime_on, "one-touch chime didn't switch");
    expect(lv_subject_get_int(&subject_view) == 5, "one-touch chime left the settings view");
    shot("chime_one_touch");
    tap_name("bar_chime");
    run(300);
    expect(lv_subject_get_int(&subject_chime) == chime_on, "one-touch chime didn't switch back");

    /* ---- checks with no screenshot of their own: the code pad's idle policy, the trouble
     * list's paging and what the Emergency screen says once a service is connected. Last, so
     * the screenshots the feature docs point at keep their numbers. ---- */

    printf("An unfinished code is forgotten after 90 idle seconds, on every pad\n");
    tap_name("nav_home");
    run(300);
    tap(lv_obj_get_child(by_name("panel_arm"), 0)); /* Away */
    type("123");
    expect(lv_subject_get_int(&subject_pad_digits) == 3, "the Home pad didn't take the digits");
    run(120000);
    expect(lv_subject_get_int(&subject_pad_digits) == 0,
           "Home: a half-typed code was still on the pad after 120 idle seconds");
    expect(lv_subject_get_int(&subject_panel) != 2, "Home: the abandoned pad stayed open");
    expect_saver(true, "with the abandoned Home pad gone");
    wake();

    tap_name("bar_settings");
    tap_name("set_users");
    run(300);
    expect(lv_subject_get_int(&subject_settings_pad) == 1, "Users didn't ask for a code");
    type("12");
    expect(lv_subject_get_int(&subject_pad_digits) == 2, "the settings pad didn't take the digits");
    run(120000);
    expect(lv_subject_get_int(&subject_pad_digits) == 0,
           "Settings: a half-typed code was still on the pad after 120 idle seconds");
    expect(lv_subject_get_int(&subject_settings_pad) == 0,
           "Settings: the abandoned code prompt stayed open");
    tap_name("nav_home");
    run(300);

    printf("During an alarm the pad stays where it is, but what was typed goes\n");
    host_panel_zone(8, ALARM_ZEV_FAULT); /* the 24-hour water zone */
    run(1500);
    expect(lv_subject_get_int(&subject_alarm_active) == 1, "the 24-hour zone didn't alarm");
    expect(lv_subject_get_int(&subject_panel) == 2, "the alarm didn't open the disarm pad");
    type("12");
    run(120000);
    expect(lv_subject_get_int(&subject_pad_digits) == 0,
           "alarm: a half-typed code was still on the pad after 120 idle seconds");
    expect(lv_subject_get_int(&subject_panel) == 2,
           "alarm: disarming got harder - the pad was taken away");
    expect(lv_subject_get_int(&subject_alarm_active) == 1, "alarm: the screen stopped showing it");
    type("123456k");
    run(500);
    host_panel_zone(8, ALARM_ZEV_RESTORE);
    run(500);
    if (lv_subject_get_int(&subject_panel) == 3) {
        tap_name("memory_tile");
        type("123456k");
        run(500);
    }

    printf("Every trouble is reachable and the count is the true one\n");
    {
        alarm_config_t *many = (alarm_config_t *)malloc(sizeof(alarm_config_t));
        expect(many != NULL, "out of memory");
        expect(be.get_config(be.ctx, many), "the preview has no configuration to read");
        for (uint16_t z = 11; z <= 70; z++) {
            alarm_zone_config_defaults(&many->zones[z - 1u], z, ALARM_ZT_PERIMETER, 1);
            snprintf(many->zones[z - 1u].name, ALARM_NAME_LEN, "Window %u", (unsigned)z);
        }
        expect(be.set_config(be.ctx, many) == ALARM_OK, "60 more zones were refused");
        free(many);
    }
    for (uint16_t z = 11; z <= 70; z++) {
        host_panel_zone(z, ALARM_ZEV_LOW_BATTERY);
    }
    run(1000);
    tap_name("bar_trouble");
    run(300);
    expect(lv_subject_get_int(&subject_trouble_count) > (int32_t)48,
           "the flood of troubles didn't get past one page, so the check proves nothing");
    expect(lv_subject_get_int(&subject_troubles_pager) == 1,
           "more troubles than fit a page but no way to the rest");
    {
        /* The note above the list counts every fault, not just the ones on this page. */
        char want[32];
        snprintf(want, sizeof(want), "%d active", (int)lv_subject_get_int(&subject_trouble_count));
        expect(strstr(lv_subject_get_string(&subject_troubles_note), want) != NULL,
               "the trouble note doesn't agree with the status bar's count: faults are being under-reported");
        /* The last zone's fault is on some page, and paging comes back round to the first. */
        bool found = false;
        for (int p = 0; p < 8 && !found; p++) {
            found = find_label(by_name("view_troubles"), "Window 70 · Low battery") != NULL;
            if (!found) {
                tap_name("troubles_more");
                run(200);
            }
        }
        expect(found, "the last zone's fault can't be reached in the trouble list");
        for (int p = 0; p < 8; p++) {
            if (find_label(by_name("view_troubles"), "Window 11 · Low battery") != NULL) {
                break;
            }
            tap_name("troubles_back");
            run(200);
        }
        expect(find_label(by_name("view_troubles"), "Window 11 · Low battery") != NULL,
               "paging back doesn't return to the first troubles");
    }

    printf("Emergency: set up but offline, then connected\n");
    host_panel_set_net(UI_NET_DISCONNECTED, 0);
    host_panel_mqtt_preset("homeassistant.local", "alarmpanel", "s3cret-mqtt", UI_MQTT_TLS_OFF,
                           true, 0);
    run(4000);
    tap_name("nav_panic");
    run(1500); /* the line is written at most once a second (UI-020) */
    expect(find_label(lv_screen_active(),
                      "A connected service is set up but offline now*") != NULL,
           "Emergency: a service that can't be reached is reported as if it could");
    host_panel_set_net(UI_NET_CONNECTED, -63);
    run(5000);
    expect(find_label(lv_screen_active(),
                      "A connected service is online and is sent the alarm*") != NULL,
           "Emergency: a connected service isn't mentioned once one is connected");
    expect(find_label_with(lv_screen_active(), "Home Assistant") == NULL,
           "Emergency: a general screen names the integration (UI-018)");

    /* PWR-011: steady status-bar battery, absent and unavailable both critical. */
    lv_subject_set_int(&subject_view, 0);
    host_panel_set_power(UI_BATTERY_UNKNOWN, 75, false);
    run(1500);
    expect(strcmp(lv_label_get_text(by_name("bar_battery_text")), "") == 0,
           "unknown battery fabricated a percentage");
    expect(!lv_obj_has_flag(lv_obj_find_by_name(lv_screen_active(), "bar_battery_strike"), LV_OBJ_FLAG_HIDDEN),
           "unknown battery lacks critical strike");
    expect(lv_color_to_u32(lv_obj_get_style_text_color(by_name("bar_battery_icon"), LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(0xF87171)), "unavailable battery is not red");
    shot("battery_unknown");
    host_panel_set_power(UI_BATTERY_ABSENT, 75, false);
    run(1500);
    expect(!lv_obj_has_flag(lv_obj_find_by_name(lv_screen_active(), "bar_battery_strike"), LV_OBJ_FLAG_HIDDEN),
           "absent battery lacks strike");
    expect(strcmp(lv_label_get_text(by_name("bar_battery_text")), "") == 0,
           "absent battery retained a percentage");
    shot("battery_absent");
    host_panel_set_power(UI_BATTERY_PRESENT, 82, false);
    run(1500);
    expect(strcmp(lv_label_get_text(by_name("bar_battery_text")), "82%") == 0,
           "valid battery percentage missing");
    expect(lv_obj_has_flag(lv_obj_find_by_name(lv_screen_active(), "bar_battery_strike"), LV_OBJ_FLAG_HIDDEN),
           "battery insertion retained strike");
    shot("battery_full");
    host_panel_set_power(UI_BATTERY_PRESENT, 100, false);
    run(1500);
    lv_area_t settings_area;
    lv_obj_get_coords(by_name("bar_settings"), &settings_area);
    expect(settings_area.x2 < W - 10, "battery percentage pushed settings off screen");
    host_panel_set_power(UI_BATTERY_PRESENT, 15, true);
    run(1500);
    expect(lv_subject_get_int(&subject_battery_warn) == 1, "low battery lacks warning tone");
    shot("battery_low");
    host_panel_set_power(UI_BATTERY_PRESENT, 101, false);
    run(1500);
    expect(strcmp(lv_label_get_text(by_name("bar_battery_text")), "") == 0,
           "invalid percentage is displayed");
    expect(lv_subject_get_int(&subject_battery_critical) == 1, "invalid sensing is not critical");
    shot("battery_unavailable");

    host_panel_set_power(UI_BATTERY_PRESENT, 62, false);
    host_panel_set_charging(true);
    run(1500);
    expect(strcmp(lv_label_get_text(by_name("bar_battery_icon")), "\xEE\x81\x94") == 0,
           "charging icon is missing");
    expect(strcmp(lv_label_get_text(by_name("bar_battery_text")), "62%") == 0,
           "charging percentage is missing");
    shot("battery_charging");

#if CONFIG_ALARM_FEATURE_NOTIFY
    /* ---- Integrations > Notifications: ntfy and Pushover (docs/features/26-28) ------------------ */
    printf("Notifications: two services that alert a phone, with no server of your own\n");
    host_panel_set_power(UI_BATTERY_PRESENT, 62, false);
    host_panel_set_charging(false);
    run(1500);
    wake();
    tap_name("bar_settings");
    run(300);
    tap_name("set_integrations");
    run(300);
    /* Setting up an integration is the system master's or the installer's (docs/features/14). */
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(300);
    }
    expect(lv_subject_get_int(&subject_settings_pad) == 0, "Integrations stayed locked");
    expect(find_label(by_name("page_body"), "Smart home") != NULL,
           "the Integrations list has no Smart home group (UI-034)");
    expect(find_label(by_name("page_body"), "Alerts to your phone") != NULL,
           "the Integrations list has no Alerts to your phone group (UI-034)");
    expect(find_label(by_name("page_body"), "ntfy") != NULL, "ntfy is not in the list");
    expect(find_label(by_name("page_body"), "Pushover") != NULL, "Pushover is not in the list");
    shot("integrations_notifications");

    /* ntfy: it arrives with a topic already made, so switching it on is the only step. */
    tap_text("ntfy");
    run(300);
    expect(find_label(by_name("page_body"), "ntfy.sh") != NULL,
           "ntfy doesn't default to the hosted service (NTFY-002)");
    expect(find_label(by_name("page_body"), "roboalarms-*") != NULL,
           "ntfy has no topic ready to use (NTFY-003)");
    shot("ntfy_page");

    /* The topic is a credential: whatever is typed, it never comes back out of the panel. */
    tap_text("Access token");
    run(300);
    sheet_text("tk_averysecrettoken");
    tap_name("sheet_done");
    run(400);
    expect(find_label(by_name("page_body"), "tk_averysecrettoken") == NULL,
           "the ntfy token is readable on the page (NTFY-008)");
    expect(find_label(by_name("page_body"), "Set") != NULL, "the ntfy token wasn't stored");

    tap_text("Server");
    run(300);
    shot("ntfy_server");
    tap_text("ntfy.sh");
    run(400);

    tap_text("Send notifications");
    run(1200);
    expect(find_label_with(by_name("page_body"), "sending to ntfy.sh") != NULL,
           "ntfy doesn't say where it is sending once it is on");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(200);
    shot("ntfy_on");

    /* What the owner is told about: the same five for both services. */
    expect(find_label(by_name("page_body"), "What to send") != NULL,
           "ntfy has no category rows (NOTE-037)");
    tap_text("Alarms");
    run(400);
    expect(find_label_with(by_name("page_body"), "nothing tells you when the siren goes") != NULL,
           "turning alarms off says nothing about what that means (NOTE-037)");
    shot("ntfy_alarms_off");
    tap_text("Alarms");
    run(400);

    /* The QR code is the short way to get the topic onto a phone, but it is the credential, so
     * it is asked for rather than drawn, and it puts itself away (NTFY-011). */
    expect(find_label(by_name("page_body"), "Add a phone") != NULL,
           "the ntfy page draws its QR code without being asked (NTFY-011)");
    tap_text("Add a phone");
    run(500);
    expect(find_label_with(by_name("page_body"), "photographs it") != NULL,
           "the QR code doesn't say what it gives away (NTFY-011)");
    lv_obj_scroll_to_y(by_name("page_body"), 60, LV_ANIM_OFF); /* the code under the switch and its sentence */
    run(200);
    shot("ntfy_qr");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(200);
    tap_text("Hide the QR code");
    run(500);
    expect(find_label(by_name("page_body"), "Add a phone") != NULL,
           "the QR code wouldn't go away again");

    /* A service that stops answering says so, rather than looking fine. */
    host_panel_notify_fault(UI_NOTIFY_NTFY, true);
    run(1500);
    expect(find_label_with(by_name("page_body"), "Can't reach") != NULL,
           "a dead ntfy server still looks healthy");
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(200);
    shot("ntfy_unreachable");
    host_panel_notify_fault(UI_NOTIFY_NTFY, false);
    run(1500);

    tap_name("page_back");
    run(300);

    /* Pushover: both credentials are the owner's own, and a wrong one is refused on the spot. */
    tap_text("Pushover");
    run(300);
    expect(find_label_with(by_name("page_body"), "Repeat alarms until answered") != NULL,
           "Pushover doesn't offer the repeat-until-answered alert (PUSH-004)");
    shot("pushover_page");

    tap_text("User key");
    run(300);
    sheet_text("notarealuserkeyatallnope00");
    tap_name("sheet_done");
    run(400);
    expect(find_label_with(lv_screen_active(), "30 letters and digits") != NULL,
           "a malformed Pushover user key was accepted (PUSH-002)");
    shot("pushover_bad_key");

    tap_text("User key");
    run(300);
    sheet_text("uQiRzpo4DXghDmr9QzzfQu27cmVRsG");
    tap_name("sheet_done");
    run(400);
    tap_text("Application token");
    run(300);
    sheet_text("azGDORePK8gMaC0QOYAMyEEuzJnyUi");
    tap_name("sheet_done");
    run(400);
    expect(find_label(by_name("page_body"), "uQiRzpo4DXghDmr9QzzfQu27cmVRsG") == NULL,
           "the Pushover user key is readable on the page (PUSH-008)");
    tap_text("Send notifications");
    run(1200);
    lv_obj_scroll_to_y(by_name("page_body"), 0, LV_ANIM_OFF);
    run(200);
    shot("pushover_on");
#endif

    /* ZB-002/003/005: exercise the same picker and zone details as a real radio would. */
    printf("Zigbee pairing, sensor reports and radio supervision\n");
    tap_name("nav_home");
    tap_name("bar_settings");
    open_installer();
    run(300);
    tap_text("Enter the installer code");
    type("69420k");
    run(300);
    tap_text("Zones");
    run(300);
    tap_text("Add a zone");
    run(300);
    tap_text("Zigbee sensor");
    run(1500);
    expect(find_label(lv_screen_active(), "WISZB-120") != NULL,
           "the Zigbee contact did not arrive in the learn window");
    shot("zigbee_learn");
    tap_text("WISZB-120");
    run(500);
    shot("zigbee_bound");
    expect(find_label(lv_screen_active(), "Zigbee · WISZB-120") != NULL,
           "the zone did not bind to the Zigbee contact");
    host_panel_zigbee_open(true);
    run(1500);
    expect(find_label(lv_screen_active(), "Open") != NULL,
           "a Zigbee report did not open the zone");
    shot("zigbee_open");
    host_panel_zigbee_radio(false);
    host_panel_zigbee_open(false);
    run(1500);
    expect(find_label(lv_screen_active(), "Open") != NULL,
           "an unplugged radio delivered a restore report");
    host_panel_zigbee_radio(true);
    host_panel_zigbee_open(false);
    run(1500);
    expect(find_label(lv_screen_active(), "Closed") != NULL,
           "the reconnected Zigbee contact did not restore");
    host_panel_zigbee_tamper(true);
    host_panel_zigbee_battery(true);
    run(1500);
    shot("zigbee_tamper_battery");
    host_panel_zigbee_tamper(false);
    host_panel_zigbee_battery(false);
    host_panel_zigbee_jam(true);
    run(31000);
    host_panel_zigbee_jam(false);
    host_panel_zigbee_open(false);
    run(1500);
    expect(find_label(lv_screen_active(), "Closed") != NULL,
           "the Zigbee zone did not recover from interference");

    /* SD-001..006 and UI-035: navigation, auth, capacity, explicit erase and stale consent. */
    tap_name("nav_home");
    run(3500); /* let the installer-exit acknowledgement clear before the screenshots */
    tap_name("bar_settings");
    tap_name("set_about");
    expect(find_label_with(by_name("page_body"), "Not affiliated") == NULL,
           "About retained the affiliation disclaimer");
#if CONFIG_ALARM_FEATURE_STORAGE
    tap_text("SD card");
    run(1200);
    shot("sd_card_locked");
    expect(find_label_with(by_name("page_body"), "59.6 GB") != NULL, "large SD capacity overflowed");
    expect(find_label(by_name("page_body"), "Format the card") == NULL, "format exposed without authority");
    tap_text("Enter the master or installer code");
    type("69420k");
    run(400);
    shot("sd_card");
    const unsigned before_format = host_panel_storage_formats();
    tap_text("Format the card");
    shot("sd_format_confirm");
    expect(find_label_with(lv_screen_active(), "Erase ALL files") != NULL, "format has no erase warning");
    tap_text("Cancel");
    expect(host_panel_storage_formats() == before_format, "cancel erased the card");
    tap_text("Format the card");
    host_panel_storage_preset(STORAGE_READY, UINT64_C(32000000000), UINT64_C(16000000000));
    tap_text("Erase and format");
    expect(host_panel_storage_formats() == before_format, "stale confirmation erased replacement card");
    run(1200);
    tap_text("Format the card");
    tap_text("Erase and format");
    run(1000);
    expect(host_panel_storage_formats() == before_format + 1u, "confirmed format was not queued");
    shot("sd_format_working");
    expect(find_label(by_name("page_body"), "Working") != NULL, "format progress missing");
    expect(find_label(by_name("page_body"), "Format the card") == NULL, "format offered while it runs");
    run(5500);
    shot("sd_format_complete");
    expect(find_label_with(by_name("page_body"), "free of") != NULL, "format never finished");
    host_panel_storage_fail(true);
    tap_text("Format the card");
    tap_text("Erase and format");
    run(6500);
    shot("sd_format_failed");
    expect(find_label(by_name("page_body"), "Card error") != NULL, "failed format claimed success");
    host_panel_storage_fail(false);
    tap_text("Check for a card");
    run(1200);
    tap_text("Safely remove");
    run(1200);
    shot("sd_safe_remove");
    expect(find_label(by_name("page_body"), "Safe to remove") != NULL, "eject did not finish");
    expect(find_label(by_name("page_body"), "Format the card") == NULL, "format offered with no card");
    host_panel_storage_preset(STORAGE_UNSUPPORTED, UINT64_C(64000000000), 0u);
    run(1200);
    shot("sd_unsupported");
    expect(find_label(by_name("page_body"), "Unsupported filesystem") != NULL, "unsupported media fabricated usage");
    host_panel_storage_preset(STORAGE_READY, UINT64_C(64000000000), 0u);
    run(1200);
    shot("sd_full");
    expect(find_label_with(by_name("page_body"), "0 B free of") != NULL, "full card shows wrapped free space");
    host_panel_storage_preset(STORAGE_NO_CARD, 0u, 0u);
    run(1200);
    shot("sd_no_card");
    expect(find_label(by_name("page_body"), "Safely remove") == NULL, "no card offers Safely remove");
    expect(find_label(by_name("page_body"), "Check for a card") != NULL, "no card offers no way to check");
#endif
    tap_name("set_about");
    expect(find_label_with(by_name("page_body"), "Not affiliated") == NULL, "details retained affiliation disclaimer");
    tap_name("nav_home");

    /* RF-003: a 433 MHz contact is learned by triggering it; its codes drive the zone. */
    printf("433 MHz sensors: learning by triggering, the bound contact, its codes\n");
    tap_name("bar_settings");
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Zones");
    run(300);
    tap_text("Add a zone");
    run(300);
    host_panel_rf_neighbour(); /* RF-010: a neighbour's remote, on the air before learning */
    tap_text("433 MHz sensor");
    run(500);
    expect(find_label_with(lv_screen_active(), "Listening: nothing heard yet") != NULL,
           "the 433 MHz learn page did not say it was listening");
    host_panel_rf_open(true); /* the installer opens the door: its open code arrives */
    host_panel_rf_glitch();   /* RF-009: a corrupted frame, heard once: never a sensor */
    run(1500);
    expect(find_label_with(lv_screen_active(), "Heard Sensor A2B3C") != NULL,
           "the 433 MHz contact's code did not arrive in the learn window");
    expect(find_label_with(lv_screen_active(), "Sensor A2F3C") == NULL,
           "a lone corrupted 433 MHz frame was offered as a sensor");
    host_panel_rf_open(false); /* ... and closes it: the sensor has answered twice (RF-011) */
    run(1500);
    expect(find_label_with(lv_screen_active(), "Use Sensor A2B3C") != NULL,
           "the 433 MHz learn page did not name the sensor that answered");
    expect(find_label_with(lv_screen_active(), "Already transmitting before you started") != NULL,
           "the neighbour's 433 MHz remote was not set apart");
    shot("rf_learn");
    tap_text("Use Sensor A2B3C");
    run(500);
    shot("rf_bound");
    expect(find_label_with(lv_screen_active(), "433 MHz · A2B3C code 5") != NULL,
           "the zone did not bind to the 433 MHz code");
    host_panel_rf_open(false); /* the closed code: nothing claims it, so it restores */
    run(1500);
    expect(find_label(lv_screen_active(), "Closed") != NULL,
           "the 433 MHz contact's closed code did not restore the zone");
    host_panel_rf_open(true);
    run(1500);
    expect(find_label(lv_screen_active(), "Open") != NULL,
           "the 433 MHz contact's open code did not open the zone");
    shot("rf_open");
    host_panel_rf_open(false);
    run(1500);
    tap_name("nav_home");

    /* docs/features/31: the Panel Manager signs in with a user name and a password set here, on the
     * panel. A system master's is on their Users page, the installer's in Installer (the user
     * name is their name, "Installer"; About still shows the panel ID). Appended last so earlier
     * screenshot numbers stay. */
#if CONFIG_ALARM_PANEL_WEB
    printf("Panel Manager sign-in: the panel ID on About, a master's password, the installer's\n");
    tap_name("bar_settings");
    tap_name("set_about");
    run(300);
    expect(find_label_with(by_name("page_body"), "0c8b95a1f2d4") != NULL,
           "About doesn't show the panel ID");
    tap_name("set_users");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("123456k");
        run(300);
    }
    tap_text("Master");
    run(300);
    expect(find_label(by_name("page_body"), "Browser sign-in") != NULL,
           "a system master can't be given a browser sign-in");
    expect(find_label_with(by_name("page_body"), "User name: Master") != NULL,
           "the master's page doesn't say their user name");
    tap_text("Browser sign-in");
    run(300);
    sheet_text("too short");
    tap_name("sheet_done");
    run(300);
    expect(find_label(by_name("page_body"), "Change the browser password") == NULL,
           "a short password was accepted");
    tap_text("Browser sign-in");
    run(300);
    sheet_text("four words make sense");
    shot("web_login_password");
    tap_name("sheet_done");
    run(300);
    sheet_text("four words make sense");
    tap_name("sheet_done");
    run(500);
    expect(find_label(by_name("page_body"), "Change the browser password") != NULL,
           "the master's browser sign-in didn't turn on");
    expect(find_label(by_name("page_body"), "Delete this user") == NULL,
           "the system master can be deleted");
    shot("web_login_master");
    tap_text("Browser sign-in"); /* the switch: off asks first */
    run(300);
    tap_name("dialog_ok");
    run(300);
    expect(find_label(by_name("page_body"), "Change the browser password") == NULL,
           "the master's browser sign-in didn't turn off");
    tap_name("page_back");
    run(300);
    /* A new (standard) user may have a sign-in too (2026-09-23); an arm-only code may not. */
    tap_text("Add a user");
    run(300);
    type("582047k");
    run(300);
    type("582047k"); /* the new code, twice */
    run(500);
    expect(find_label_with(lv_screen_active(), "· Standard") != NULL,
           "the new standard user's page didn't open");
    expect(find_label(by_name("page_body"), "Browser sign-in") != NULL,
           "a standard user can't be given a browser sign-in");
    expect(find_label(by_name("page_body"), "Delete this user") != NULL,
           "a standard user can't be deleted");
    scroll_to_text("page_body", "Delete this user");
    shot("user_page_standard"); /* Delete last and apart, the browser sign-in a switch */
    tap_text("What this code can do");
    run(300);
    tap_text("Arm only");
    run(500);
    expect(find_label_with(lv_screen_active(), "· Arm only") != NULL,
           "the new user didn't become arm-only");
    expect(find_label(by_name("page_body"), "Browser sign-in") == NULL,
           "an arm-only code is offered a browser sign-in");
    tap_name("nav_home");
    tap_name("bar_settings");
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    /* The installer signs in with their name, like everyone (2026-09-29; it was the panel ID) */
    scroll_to_text("page_body", "Browser sign-in");
    expect(find_label_with(by_name("page_body"), "User name: Installer") != NULL,
           "the installer's sign-in doesn't use their name");
    expect(find_label_with(by_name("page_body"), "User name: 0c8b95a1f2d4") == NULL,
           "the installer's sign-in still uses the panel ID");
    shot("web_login_installer");
    /* The installer's code opens Users too, and manages every user (2026-09-23). Leaving the
     * Installer section ends installer mode; the code typed for it still opens Users. */
    tap_name("set_users");
    run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type("69420k");
        run(300);
    }
    expect(find_label(by_name("page_body"), "Add a user") != NULL,
           "the installer code doesn't open Users");
    tap_text("Master");
    run(300);
    expect(find_label(by_name("page_body"), "Change the code") != NULL,
           "the installer can't change the master's code in Users");
    expect(find_label(by_name("page_body"), "Browser sign-in") != NULL,
           "the installer can't give the master a browser sign-in");
    tap_text("Browser sign-in");
    run(300);
    sheet_text("a password for somebody");
    tap_name("sheet_done");
    run(300);
    sheet_text("a password for somebody");
    tap_name("sheet_done");
    run(500);
    expect(host_panel_web_login_temporary(ALARM_USER_MASTER),
           "a password set for somebody else isn't temporary");
    shot("installer_users");

    /* docs/features/31: someone on the remote keypad. The red banner says so and the screen saver
     * stays off; a keyboard at the remote end types into the sheet or the code pad on screen;
     * the person at the panel pauses it with the banner and allows it or turns it off. */
    printf("Remote keypad: the banner, typing from a keyboard, pausing\n");
    {
        ui_remote_session_t rs;
        memset(&rs, 0, sizeof(rs));
        rs.active = true;
        snprintf(rs.who, sizeof(rs.who), "Alex");
        snprintf(rs.where, sizeof(rs.where), "198.51.100.23");
        (void)be.remote_set(be.ctx, true, false);
        ui_app_remote_session(&rs);
        run(300);
        expect(find_label_with(lv_screen_active(), "Remote: Alex") != NULL,
               "no red banner while someone uses the remote keypad");
        /* The master's name, retyped on a computer's keyboard. */
        tap_text("Name");
        run(300);
        for (int i = 0; i < 6; i++) {
            expect(ui_app_remote_key(UI_REMOTE_KEY_BACKSPACE), "backspace didn't reach the sheet");
        }
        expect(ui_app_remote_text("Pat"), "typing didn't reach the sheet");
        shot("remote_banner_typing");
        expect(ui_app_remote_key(UI_REMOTE_KEY_ENTER), "enter didn't finish the sheet");
        run(500);
        expect(find_label(by_name("page_body"), "Pat") != NULL, "the typed name wasn't saved");
        tap_name("nav_home");
        run(300);
        /* A code pad takes the digits; backspace and escape work on it too. */
        tap_name("bar_settings");
        tap_name("set_users");
        run(300);
        expect(lv_subject_get_int(&subject_settings_pad) == 1, "Users didn't ask for a code");
        expect(ui_app_remote_text("12"), "digits didn't reach the code pad");
        expect(lv_subject_get_int(&subject_pad_digits) == 2, "the code pad didn't get two digits");
        expect(ui_app_remote_key(UI_REMOTE_KEY_BACKSPACE), "backspace didn't reach the pad");
        expect(lv_subject_get_int(&subject_pad_digits) == 1, "backspace didn't remove a digit");
        expect(ui_app_remote_key(UI_REMOTE_KEY_ESCAPE), "escape didn't close the pad");
        run(300);
        tap_name("nav_home");
        run(300);
        /* The person at the panel taps the banner: the remote is paused, and asked about. */
        tap_name("remote_banner");
        run(300);
        expect(host_panel_remote_paused(), "the banner didn't pause the remote keypad");
        expect(find_label(lv_screen_active(), "Allow") != NULL, "the pause question has no Allow");
        expect(!ui_app_remote_text("9"), "a paused remote still types");
        shot("remote_paused");
        tap_name("dialog_cancel"); /* Allow */
        run(300);
        expect(!host_panel_remote_paused(), "Allow didn't let the remote go on");
        /* A pause outlives the session (the platform reports it with every session, as
         * dev_adapter.c does): a remote that leaves and comes back is still paused, and the
         * question waits for the person at the panel. */
        tap_name("remote_banner");
        run(300);
        memset(&rs, 0, sizeof(rs));
        rs.paused = host_panel_remote_paused();
        ui_app_remote_session(&rs); /* the remote leaves... */
        run(300);
        rs.active = true;
        snprintf(rs.who, sizeof(rs.who), "Alex");
        snprintf(rs.where, sizeof(rs.where), "198.51.100.23");
        ui_app_remote_session(&rs); /* ...and comes straight back */
        run(300);
        expect(host_panel_remote_paused() &&
                   find_label_with(lv_screen_active(), "Remote keypad paused") != NULL,
               "reconnecting undid the pause");
        expect(!ui_app_remote_text("9"), "a remote that came back typed while paused");
        expect(find_label(lv_screen_active(), "Allow") != NULL, "the pause question closed");
        tap_name("dialog_ok"); /* Turn off remote access */
        run(300);
        expect(!host_panel_remote_web(), "remote access stayed on");
        memset(&rs, 0, sizeof(rs));
        rs.paused = host_panel_remote_paused(); /* off: forgotten */
        ui_app_remote_session(&rs); /* the web server closing ends the session */
        run(300);
        expect(find_label_with(lv_screen_active(), "Remote:") == NULL, "the banner outlived the session");
    }
    tap_name("nav_home");
#endif

#if ALARM_LV_BUDGET_KB == 0
    /* Everything the Panel Manager sets, at the panel too (2026-09-23): partitions, keypads and
     * devices, the rest of the system options, a zone's partition and advanced settings, its
     * sensor's settings. The boards whose full UI runs on hardware (a board showing the status
     * screen keeps its screenshot numbers). */
    printf("Installer: partitions, devices, every option, a zone's sensor settings\n");
    tap_name("nav_home");
    tap_name("bar_settings");
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    expect(find_label(by_name("page_body"), "Partitions") != NULL, "Installer has no Partitions");
    tap_text("Partitions");
    run(300);
    tap_text("Partition 2");
    run(300);
    tap_text("In use");
    run(300);
    expect(find_label(by_name("page_body"), "Entry time 2") != NULL,
           "partition 2 didn't come into use");
    shot("installer_partition");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Keypads and devices");
    tap_text("Keypads and devices");
    run(300);
    tap_text("Device 2");
    run(300);
    tap_text("In use");
    run(300);
    expect(find_label(by_name("page_body"), "Home partition") != NULL,
           "a device's home partition can't be set");
    /* BL-131 (n-zone-sensor): a device's Supervision is a picker, as a zone's */
    tap_text("Supervision");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 2, "BL-131: a device's Supervision is not a picker");
    tap_text("4 hours");
    run(300);
    expect(strcmp(row_label("Supervision", "value_label"), "4 hours") == 0,
           "BL-131: a device's Supervision didn't take 4 hours");
    shot("installer_device");
    tap_text("In use"); /* back out of use: nothing answers for it here */
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_text("Delays and arming");
    run(300);
    expect(find_label(by_name("page_body"), "Cancel window") != NULL, "no cancel window");
    expect(find_label(by_name("page_body"), "Alarm limit") != NULL, "no alarm limit");
    expect(find_label(by_name("page_body"), "Walk test ends after") != NULL, "no walk test time");
    scroll_to_text("page_body", "Panic keys");
    tap_text("Panic keys");
    run(300);
    expect(find_label(by_name("page_body"), "Key C") != NULL, "no panic key C");
    shot("installer_panic_keys");
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Trouble beeps");
    tap_text("Trouble beeps");
    run(300);
    shot("installer_trouble_beeps");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_text("Zones");
    run(300);
    {
        /* the first zone in the list */
        lv_obj_t *body = by_name("page_body");
        tap(lv_obj_get_child(body, 1));
    }
    run(300);
    scroll_to_text("page_body", "Cross-zone partner");
    expect(find_label(by_name("page_body"), "Partition") != NULL,
           "a zone's partition can't be chosen with two partitions in use");
    expect(find_label(by_name("page_body"), "Alarm limit") != NULL, "no alarm limit");
    shot("installer_zone_more");
    {
        /* BL-131 finding 23: "Alarm limit" is one picker over the two stored fields it always
         * was (swinger_trips, 0 = the system setting, and ALARM_ZF_SWINGER_EXEMPT): Never sets
         * the flag and keeps the count; a count clears the flag; nothing else changes. */
        alarm_config_t *cfg = (alarm_config_t *)malloc(sizeof(alarm_config_t));
        expect(cfg != NULL, "out of memory");
        expect(be.get_config(be.ctx, cfg), "the preview has no configuration to read");
        uint16_t zone = 0;
        for (uint16_t z = 1; z <= ALARM_MAX_ZONES && zone == 0u; z++) {
            if (cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED) {
                zone = z; /* the first zone in the list, the one open */
            }
        }
        const alarm_zone_config_t was = cfg->zones[zone - 1u];
        expect((was.flags & ALARM_ZF_SWINGER_EXEMPT) == 0u && was.swinger_trips == 0u,
               "the first zone doesn't start at the system's alarm limit");
        lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(find_label(by_name("page_body"), "Alarm limit")));
        expect(find_label(row, "System setting") != NULL, "Alarm limit doesn't read System setting");
        tap_text("Alarm limit");
        run(300);
        expect(find_label(by_name("pick_list"), "6 times") != NULL &&
                   find_label(by_name("pick_list"), "Never") != NULL,
               "the Alarm limit picker lacks 1 to 6 or Never");
        tap_text("3 times");
        run(300);
        tap_text("Alarm limit");
        run(300);
        tap_text("Never");
        run(300);
        row = lv_obj_get_parent(lv_obj_get_parent(find_label(by_name("page_body"), "Alarm limit")));
        expect(find_label(row, "Never") != NULL, "Alarm limit doesn't read Never");
        tap_name("page_back"); /* leaving the zone page saves it */
        run(300);
        expect(be.get_config(be.ctx, cfg), "the preview has no configuration to read");
        expect((cfg->zones[zone - 1u].flags & ALARM_ZF_SWINGER_EXEMPT) != 0u &&
                   cfg->zones[zone - 1u].swinger_trips == 3u,
               "Never didn't set the swinger exemption, or lost the count");
        tap(lv_obj_get_child(by_name("page_body"), 1)); /* the zone again */
        run(300);
        scroll_to_text("page_body", "Alarm limit");
        tap_text("Alarm limit");
        run(300);
        tap_text("System setting");
        run(300);
        tap_name("page_back"); /* leaving the zone page saves it */
        run(300);
        expect(be.get_config(be.ctx, cfg), "the preview has no configuration to read");
        expect(cfg->zones[zone - 1u].flags == was.flags &&
                   cfg->zones[zone - 1u].swinger_trips == 0u,
               "System setting didn't clear the exemption and the count");
        free(cfg);
        tap(lv_obj_get_child(by_name("page_body"), 1)); /* the zone again */
        run(300);
    }
    scroll_to_text("page_body", "Sensor settings");
    tap_text("Sensor settings");
    run(300);
    {
        /* BL-131 findings 19, 24: pickers, not steppers holding words; both ways a sensor goes
         * quiet side by side (docs/features/22 keeps both on purpose). */
        lv_obj_t *body = by_name("page_body");
        expect(find_label(body, "Tamper input") != NULL, "no tamper input");
        expect(find_label(body, "Open when") != NULL, "no reading mode");
        expect(find_label(body, "When it goes quiet") != NULL &&
                   find_label(body, "Shows Check when it misses a check-in") != NULL &&
                   find_label(body, "Shows Check when its source loses it") != NULL,
               "Supervision and Unreachable after aren't side by side, saying what they watch");
        expect(lv_obj_find_by_name(body, "minus") == NULL,
               "Sensor settings still has a stepper (words belong in a picker)");
        expect(find_label(body, "Not supervised") != NULL && find_label(body, "Source default") != NULL,
               "Supervision or Unreachable after doesn't say its value");
    }
    shot("installer_zone_sensor");
    tap_text("Supervision");
    run(300);
    expect(find_label(by_name("pick_list"), "12 hours") != NULL, "no 12 hours supervision");
    tap_text("4 hours");
    run(300);
    expect(find_label(by_name("page_body"), "4 hours") != NULL, "Supervision didn't take 4 hours");
    tap_text("Unreachable after");
    run(300);
    expect(find_label(by_name("pick_list"), "15 min") != NULL, "no 15 min grace");
    tap_text("5 min");
    run(300);
    expect(find_label(by_name("page_body"), "5 min") != NULL, "Unreachable after didn't take 5 min");
    lv_obj_scroll_by_bounded(by_name("page_body"), 0, -10000, LV_ANIM_OFF);
    run(100);
    shot("installer_zone_sensor_end");
    tap_text("Unreachable after");
    run(300);
    tap_text("Source default"); /* put them back */
    run(300);
    tap_text("Supervision");
    run(300);
    tap_text("Not supervised");
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    /* Partition 2 out of use again: the rest of the walk is about one partition. */
    tap_text("Partitions");
    run(300);
    tap_text("Partition 2");
    run(300);
    tap_text("In use");
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);

    /* The date and time by hand (docs/features/35): offered only where the panel can keep them (a
     * clock module answers), and the hardware is never named on the screen. */
    printf("Date and time by hand\n");
    tap_name("bar_settings");
    run(300);
    tap_name("set_time");
    run(300);
    if (find_label(lv_screen_active(), "Enter a master or installer code") != NULL) {
        tap_text("Enter a master or installer code");
        type("69420k");
        run(300);
    }
    expect(find_label(by_name("page_body"), "Time zone") != NULL, "Time and language didn't open");
    expect(!clock_row_opens(),
           "Time and language offers to set the time without a clock module (RTC-008)");
    host_panel_clock_module(UI_CLOCK_MODULE_OK, false);
    tap_name("set_time");
    run(300);
    expect(clock_row_opens(), "a panel that keeps the time doesn't offer to set it");
    expect(find_label_with(lv_screen_active(), "odule") == NULL,
           "Time and language names the hardware (UI-042)");
    shot("time_settable");
    /* BL-131 finding 3: a sheet over Time and language, a calendar and rollers. Cancel leaves the
     * clock as it was. */
    tap(clock_row());
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 4, "the clock's row doesn't open Set the date and time");
    tap_day("12");
    expect(find_label_with(by_name("clock_sheet"), "September 12, 2026") != NULL,
           "Set the date and time doesn't say what it will set");
    tap_name("clock_cancel");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 0, "Cancel didn't close Set the date and time");
    expect(find_label(lv_screen_active(), "Set by hand") == NULL, "Cancel set the clock");
    /* On a 24-hour clock two rollers, 0-23 and 00-59, and no AM/PM */
    tap_text("Clock");
    run(300);
    tap_text("24-hour");
    run(300);
    tap(clock_row());
    run(300);
    expect(find_visible(lv_screen_active(), "roll_ampm") == NULL, "a 24-hour clock has an AM/PM roller");
    expect(strstr(lv_roller_get_options(by_name("roll_hour")), "23") != NULL,
           "a 24-hour clock's hours don't go to 23");
    tap_name("clock_cancel");
    run(300);
    tap_text("Clock");
    run(300);
    tap_text("Automatic");
    run(300);
    tap(clock_row());
    run(300);
    expect(find_label(by_name("clock_sheet"), "Minute") != NULL, "no minute on Set the date and time");
    expect(find_label(by_name("clock_sheet"), "September 2026") != NULL,
           "the calendar doesn't open on the panel's month");
    /* October 31st, then back to September: the day becomes the 30th, not October 1st. */
    tap_name("cal_next");
    run(60);
    tap_day("31");
    expect(find_label_with(by_name("clock_sheet"), "October 31, 2026") != NULL,
           "Set the date and time doesn't say what it will set");
    tap_name("cal_prev");
    run(60);
    expect(find_label_with(by_name("clock_sheet"), "September 30, 2026") != NULL,
           "a day September doesn't have was kept");
    roll_to("roll_hour", 0); /* 12 */
    roll_to("roll_minute", 54);
    roll_to("roll_ampm", 0); /* AM */
    expect(find_label_with(by_name("clock_sheet"), "Wednesday, September 30, 2026 · 12:54 AM") != NULL,
           "the rollers don't set the time the sheet says");
    shot("clock_set");
    /* "September 2026" opens month and year rollers where the days were; the calendar keeps
     * its size */
    {
        lv_obj_t *cal = by_name("clock_cal");
        const int32_t h = lv_obj_get_height(cal);
        tap_name("cal_title");
        run(100);
        expect(find_visible(lv_screen_active(), "roll_year") != NULL, "no month and year rollers");
        expect(lv_obj_get_height(cal) == h, "the calendar changed its size for the month and year");
        shot("clock_set_month");
        roll_to("roll_year", 2); /* 2027 */
        expect(find_label_with(by_name("clock_sheet"), "September 30, 2027") != NULL,
               "the year roller doesn't change the date");
        roll_to("roll_year", 1); /* 2026 again */
        tap_name("cal_title");
        run(100);
        expect(find_visible(lv_screen_active(), "roll_year") == NULL, "the month and year rollers stay");
    }
    tap_name("clock_ok");
    run(300);
    expect(lv_subject_get_int(&subject_sheet) == 0, "Set didn't close Set the date and time");
    expect(find_label(lv_screen_active(), "Set by hand") != NULL,
           "Time and language doesn't say the clock was set by hand");
    {
        const time_t t = (time_t)(be.epoch_ms(be.ctx) / 1000);
        struct tm tm;
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        expect(tm.tm_mon == 8 && tm.tm_mday == 30 && tm.tm_year == 126,
               "the panel's clock wasn't set to the chosen day");
    }
    host_panel_clock_module(UI_CLOCK_MODULE_LOST, true);
    tap_name("set_time");
    run(300);
    expect(clock_row_opens(), "a module waiting for the time can't be given it");
    host_panel_clock_module(UI_CLOCK_MODULE_FAILED, true);
    tap_name("set_time");
    run(300);
    expect(!clock_row_opens(), "the time can be set while the module isn't answering (RTC-009)");
    expect(find_label_with(lv_screen_active(), "odule") == NULL,
           "Time and language names the hardware (UI-042)");
    host_panel_clock_module(UI_CLOCK_MODULE_NONE, false);
    tap_name("nav_home");
    run(300);

    /* RTC-015: the setup offers the date and time only where nothing else will set the clock: a
     * clock module that lost the time, and no internet. */
    printf("Setup: the date and time\n");
    host_panel_forget_wifi();
    host_panel_clock_module(UI_CLOCK_MODULE_LOST, true);
    ui_app_run_setup();
    run(300);
    past_the_language();
    tap_name("wiz_next"); /* Start */
    run(300);
    tap_name("wiz_next"); /* Skip for now: no Wi-Fi */
    run(300);
    expect_step(2, "the time zone step (RTC-015)");
    tap_name("wiz_next");
    run(300);
    expect(find_visible(lv_screen_active(), "cal_days") != NULL,
           "setup: no date and time step with a module that lost the time and no internet (RTC-015)");
    expect(strcmp(lv_subject_get_string(&subject_wiz_icon), "\xEE\x8C\x84") == 0,
           "BL-131: the setup's date and time step doesn't wear the calendar-clock");
    expect(find_label_with(lv_screen_active(), "odule") == NULL,
           "the setup's date and time names the hardware (UI-042, RTC-015)");
    expect_step(2, "the date and time step shares the time zone's dot (RTC-015)");
    shot("setup_clock");
    /* from the panel's own time (the walk set it above: Sep 30); September stays, as the later
     * language page reads the month from the clock */
    tap_day("20");
    expect(find_label_with(by_name("wiz_body"), "Sun, Sep 20, 2026") != NULL,
           "the setup's step doesn't say what it will set");
    tap_name("wiz_next"); /* Next sets the clock */
    run(300);
    expect_step(3, "Next goes on to the codes once the clock is set (RTC-015)");
    {
        const time_t t = (time_t)(be.epoch_ms(be.ctx) / 1000);
        struct tm tm;
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        expect(tm.tm_mon == 8 && tm.tm_mday >= 19 && tm.tm_mday <= 21 && tm.tm_year == 126,
               "the setup didn't set the clock to the chosen day (RTC-015)");
    }
    tap_name("wiz_back"); /* the clock is set: back passes the step */
    run(300);
    expect_step(2, "back from the codes goes to the time zone once the clock is set (RTC-015)");
    tap_name("wiz_exit");
    run(300);

    /* a clock module that kept the time, and no module at all: no step (RTC-015) */
    for (int again = 0; again < 2; again++) {
        host_panel_forget_wifi();
        host_panel_clock_module(again == 0 ? UI_CLOCK_MODULE_OK : UI_CLOCK_MODULE_NONE, false);
        ui_app_run_setup();
        run(300);
        past_the_language();
        tap_name("wiz_next");
        run(300);
        tap_name("wiz_next");
        run(300);
        tap_name("wiz_next"); /* the time zone */
        run(300);
        expect_step(3, "the setup offers the date and time where the clock is set or can't be (RTC-015)");
        tap_name("wiz_exit");
        run(300);
    }
    host_panel_clock_module(UI_CLOCK_MODULE_NONE, false);

    /* RF-021: a 433 MHz zone's tamper and low-battery inputs are codes of the zone's own
     * sensor, one row each, picked by the installer - a contact may send its closed code before
     * its tamper code, and the old picker bound whatever came first. A low-battery code can be
     * picked by hand. Appended last so earlier screenshot numbers stay. */
    printf("433 MHz tamper and low-battery codes\n");
    tap_name("bar_settings");
    run(300);
    open_installer();
    run(300);
    if (find_label(lv_screen_active(), "Enter the installer code") != NULL) {
        tap_text("Enter the installer code");
        type("69420k");
        run(300);
    }
    tap_text("Zones");
    run(300);
    tap_text("Zone 72"); /* the 433 MHz contact learned above */
    run(300);
    scroll_to_text("page_body", "Sensor settings");
    tap_text("Sensor settings");
    run(300);
    scroll_to_text("page_body", "Tamper input");
    tap_text("Tamper input");
    run(300);
    tap_text("433 MHz sensor");
    run(500);
    expect(find_label(lv_screen_active(), "Find the tamper code") != NULL,
           "a 433 MHz zone's tamper input did not ask for a code of its sensor");
    expect(find_label_with(lv_screen_active(), "nothing heard from this sensor yet") != NULL,
           "the tamper code page did not say it was listening");
    host_panel_rf_open(true);  /* the door opened ... */
    run(1500);
    host_panel_rf_open(false); /* ... and closed: E is its closed code, never its tamper */
    run(1500);
    host_panel_rf_tamper(); /* then the cover comes off */
    run(1500);
    expect(find_label(by_name("page_body"), "Code 5") != NULL &&
               find_label(by_name("page_body"), "Code E") != NULL &&
               find_label(by_name("page_body"), "Code A") != NULL,
           "the tamper code page did not list the sensor's codes one by one");
    expect(find_label_with(by_name("page_body"), "Closes the zone") != NULL,
           "the tamper code page did not mark the closed code");
    expect(find_label_with(by_name("page_body"), "Sensor A2B3C") == NULL,
           "the tamper code page listed sensors instead of codes");
    run(4000);
    expect(find_label_with(by_name("page_body"), " s ago") != NULL,
           "the tamper code page does not say how long ago a code came");
    shot("rf_tamper_codes");
    tap_text("Code A");
    run(500);
    expect(find_label(lv_screen_active(), "433 MHz · code A") != NULL,
           "the tamper input did not take the code the installer picked");
    scroll_to_text("page_body", "Low-battery input");
    tap_text("Low-battery input");
    run(300);
    tap_text("433 MHz sensor");
    run(500);
    expect(find_label(lv_screen_active(), "Find the low-battery code") != NULL,
           "a 433 MHz zone's battery input did not ask for a code of its sensor");
    host_panel_rf_open(true);
    run(1500);
    host_panel_rf_open(false);
    run(1500);
    expect(find_label_with(by_name("page_body"), "Only its door codes so far") != NULL,
           "the low-battery page did not say it has heard only the door's codes");
    tap_text("Pick the code by hand");
    run(300);
    expect(find_label(by_name("pick_list"), "Code 3") != NULL, "the codes by hand are missing");
    expect(find_label(by_name("pick_list"), "Code 5") == NULL &&
               find_label(by_name("pick_list"), "Code A") == NULL &&
               find_label(by_name("pick_list"), "Code E") == NULL,
           "codes the zone already uses, or its closed code, were offered for its battery");
    shot("rf_battery_by_hand");
    tap_text("Code 3");
    run(500);
    expect(find_label(lv_screen_active(), "433 MHz · code 3") != NULL,
           "the low-battery input did not take the code picked by hand");
    /* RF-022: a 433 MHz sensor's settings carry its hold time, automatic until picked. */
    scroll_to_text("page_body", "How long the zone stays open*"); /* the whole row, note too */
    expect(find_label(by_name("page_body"), "Hold") != NULL &&
               find_label(by_name("page_body"), "Automatic") != NULL,
           "RF-022: a 433 MHz zone's settings did not show the Hold row as Automatic");
    shot("rf_hold");
    tap_text("Hold");
    run(300);
    tap_text("5 minutes");
    run(500);
    scroll_to_text("page_body", "Hold");
    expect(find_label(by_name("page_body"), "5 minutes") != NULL,
           "RF-022: the hold time picked did not show on the zone's settings");
    /* an explicit hold ignores the door's closed code: back to automatic for the walk that follows */
    tap_text("Hold");
    run(300);
    tap_text("Automatic");
    run(500);
    scroll_to_text("page_body", "Hold");
    expect(find_label(by_name("page_body"), "5 minutes") == NULL,
           "RF-022: the hold time did not go back to automatic");
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    tap_name("page_back");
    run(300);
    scroll_to_text("page_body", "Leave installer mode");
    tap_text("Leave installer mode");
    run(300);
    tap_name("nav_home");
    run(300);
#endif

#if ALARM_SIM_ZWAVE && ALARM_LV_BUDGET_KB == 0 /* the 7" walk's pages; the 4" has no Z-Wave */
    walk_zwave(); /* appended last: every earlier screenshot keeps its number */
#endif

#if ALARM_LV_BUDGET_KB == 0
    /* UI-007, CORE-BYP-001/007, RF-003: real RF source -> hub -> engine -> zone card.
     * Appended after existing scenes so their screenshot numbers stay stable. */
    printf("433 MHz contact: open and closed through bypass and unbypass\n");
    tap_name("nav_zones");
    run(300);
    lv_obj_t *rf_title = find_label(by_name("zone_list"), "Zone 72");
    expect(rf_title != NULL, "the learned RF zone card is missing");
    lv_obj_t *rf_card = rf_title;
    while (rf_card != NULL && lv_obj_get_parent(rf_card) != by_name("zone_list")) {
        rf_card = lv_obj_get_parent(rf_card);
    }
    lv_obj_scroll_to_view(rf_card, LV_ANIM_OFF);
    host_panel_rf_open(false);
    run(1500);
    tap(lv_obj_find_by_name(rf_card, "bypass"));
    type("123456k");
    run(300);
    expect(find_label(rf_card, "Bypassed · Closed") != NULL,
           "a bypassed RF zone hides its closed state");
    shot("rf_bypassed_closed");
    host_panel_rf_open(true);
    run(1500);
    expect(find_label(rf_card, "Bypassed · Open") != NULL,
           "a bypassed RF zone stopped showing its open state");
    shot("rf_bypassed_open");
    host_panel_rf_open(false);
    run(1500);
    expect(find_label(rf_card, "Bypassed · Closed") != NULL,
           "a bypassed RF zone stopped restoring");
    tap(lv_obj_find_by_name(rf_card, "bypass"));
    type("123456k");
    run(300);
    expect(find_label(rf_card, "Closed") != NULL, "the RF zone did not unbypass");
    host_panel_rf_open(true);
    run(1500);
    expect(find_label(rf_card, "Open") != NULL, "RF reception stopped after unbypass");
    shot("rf_unbypassed_open");
    host_panel_rf_open(false);
    run(1500);
    expect(find_label(rf_card, "Closed") != NULL, "RF restore stopped after unbypass");
    tap_name("nav_home");
#endif

#if ALARM_SIM_ZWAVE && ALARM_LV_BUDGET_KB == 0
    walk_zwave_recovery(); /* appended after every other shot of the -ZWave walk */
    walk_zwave_firmware();
    walk_zwave_switches();
    walk_zwave_smartstart();
    walk_zwave_network_care();
    walk_zwave_troubles();
#endif

#if ALARM_LV_BUDGET_KB == 0
    walk_unicode_names();
    walk_language();
#endif
#if ALARM_LV_BUDGET_KB > 0
    walk_arm_options(); /* the 4" walk's last shot of the full UI (the sounder takes none) */
#endif
    check_the_sounder();
#if ALARM_LV_BUDGET_KB == 0
    walk_language_packs(); /* after every other shot, the Z-Wave ones included */
    walk_screen_light(&start);
    walk_walk_test(&start); /* appended: the shots before keep their numbers */
    walk_arm_options();
    walk_themes(); /* BL-132, appended: every shot before is Dark and keeps its number */
    walk_appearance(&start); /* BL-132, appended too */
    walk_zigbee(&start);     /* BL-144, appended: the shots before keep their numbers */
    walk_zigbee_real(&start); /* BL-143 ZB-M6, appended too */
    walk_installer_elsewhere(); /* B008, appended too */
#endif
#if ALARM_SIM_ZWAVE && ALARM_LV_BUDGET_KB == 0
    walk_zwave_region_change(); /* after every shot: it drops every Z-Wave device */
#endif
#if ALARM_LV_BUDGET_KB == 0
    walk_automations(&start); /* BL-168, after the last walk: every earlier picture keeps its number */
    walk_activity_log(); /* BL-131 finding 7, appended: the shots before keep their numbers */
    walk_auto_arm(&start); /* BL-170 C5, appended: the shots before keep their numbers */
    walk_voice(&start);    /* BL-202, appended too */
    walk_without_letters(&start); /* BL-124, appended: the shots before keep their numbers */
    walk_panel_link(&start);      /* BL-048, appended too */
    /* KPD-006 (BL-048), appended too: its pictures are partition-switch.js's, so
     * ui_mockup.py check finds them in the walk's own shots */
    walk_partition_policy(&start);
#endif

#if ALARM_LV_BUDGET_KB > 0
    /* A board without room for the touch UI shows the status screen instead (ui_status.c): the
     * state in its colour, and a ring where the panel feels the finger. */
    printf("Status screen\n");
    {
        const ui_status_options_t sopt = {.board = "Preview", .partition = 1, .brightness = 60};
        expect(ui_status_start(&be, &sopt), "the status screen did not start");
        run(1200);
        /* check_the_sounder() left the fire alarm sounding. */
        expect(strcmp(lv_label_get_text(by_name("status_headline")), "Fire alarm") == 0,
               "status screen: the fire alarm isn't shown");
        s_px = W / 3;
        s_py = H / 3;
        s_pressed = true;
        run(100);
        lv_obj_t *ring = by_name("status_ring");
        lv_area_t a;
        lv_obj_get_coords(ring, &a);
        const int32_t dx = (a.x1 + a.x2 + 1) / 2 - s_px; /* x2 is inclusive */
        const int32_t dy = (a.y1 + a.y2 + 1) / 2 - s_py;
        if (dx < -1 || dx > 1 || dy < -1 || dy > 1) {
            printf("  ring at %d,%d..%d,%d for a finger at %d,%d\n", (int)a.x1, (int)a.y1,
                   (int)a.x2, (int)a.y2, (int)s_px, (int)s_py);
        }
        expect(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1,
               "status screen: the ring isn't where the finger is");
        shot("status_fire_touch");
        s_pressed = false;
        run(100);
        expect(find_visible(lv_screen_active(), "status_ring") == NULL,
               "status screen: the ring stays after the finger lifts");
    }
#endif

    {
        const double t0 = now_s();
        expect(voice_walk(s_out) == 0, "voice walk: what the panel says (voice_walk.c)");
        s_t_voice = now_s() - t0;
    }

    printf("%d screenshots\n", s_shots);
    printf("Time: waits %.1f s (%u frames drawn in them, %.1f s), screenshots: drawing %.1f s, "
           "checks %.1f s, PNG %.1f s; voice %.1f s\n",
           s_t_wait, s_frames, s_t_frames, s_t_draw, s_t_checks, s_t_png, s_t_voice);
    printf("%u labels cut off, %u shortened with ...; %u checks left to the English walk\n",
           s_clipped, s_shortened, s_soft_fails);
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    /* The most LVGL held at once during the whole walk, against the board's budget (its
     * manifest's CONFIG_ALARM_UI_LVGL_BUDGET_KB). LVGL's objects are mostly pointers, 8 bytes
     * here and 4 on the ESP32, so the board needs about half of the PC's figure; the board's
     * own figure is measured on the board. Reported, not yet fatal: the 4" UI is over its
     * budget until docs/features/19 Phase 3's memory work. */
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    const unsigned pc_kb = (unsigned)(mem.max_used / 1024u);
    const unsigned board_kb = (pc_kb + 1u) / 2u;
    printf("LVGL heap: at most %u KB on the PC, about %u KB on the board; its budget is %u KB\n",
           pc_kb, board_kb, (unsigned)ALARM_LV_BUDGET_KB);
    if (board_kb > (unsigned)ALARM_LV_BUDGET_KB) {
        printf("WARNING: the UI needs more LVGL memory than the board's budget "
               "(docs/features/19 Phase 3)\n");
    }
#endif
    if (s_clipped > 0) {
        printf("FAILED: %u labels cut off (I18N-022), listed above\n", s_clipped);
        return 1;
    }
    if (s_glyph_misses > 0) {
        printf("FAILED: %u icons without a glyph in the font that draws them (OPT-005), listed "
               "above\n", s_glyph_misses);
        return 1;
    }
    return 0;
}
