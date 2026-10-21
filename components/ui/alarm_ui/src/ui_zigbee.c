/*
 * ui_zigbee.c - Installer > Zigbee (docs/features/20 devices.md §9, ZB-027; drawn in
 * docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js): the installer menu's row, the
 * Zigbee page (the radio, the devices, the network), adding a device with its results and "Use it
 * for", adding with an install code and the codes waiting, removing, the radio's details, the
 * channel, the reset, a device's page with Forget, and the rows other pages link in (the zone page,
 * the zone picker, the Troubles list). Zigbee's analog of ui_zwave.c, in Z-Wave's words wherever
 * the two do the same thing.
 *
 * Everything comes through ui_backend_t's zb_* calls as states and numbers, and the words are
 * here - except a model's name, its wake words (the model table's) and an input's words (the zone
 * source's). Live words are read again once a second and change only when they differ (UI-020,
 * UI-023): rows stay where they are and only their words change; a page is made again only when
 * its shape changes (the radio's state, a device's, the devices or their order).
 *
 * A siren, a keypad and a key fob (BL-147, BL-148; the proposal's pages 14-16, alarm-devices.md
 * §2-§4) open their device page too, which leads with what they do for the panel: what a siren
 * sounds for and how, with "Test it" and its result in place; the partition a keypad arms, its
 * state shown on it and its emergency buttons; whose a key fob is and what it may do. Each change
 * is a request at once (SIREN, KEYPAD, FOB): the panel keeps it, and the page is made again from
 * the panel's answer, so a refused change shows as it was. A siren's strobe is its own light:
 * nothing on this screen flashes.
 *
 * Who: looking needs installer mode, as every installer page: the Troubles list's sensor radio
 * trouble opens the Zigbee page on top of the installer menu, and outside installer mode it asks
 * for the installer code first (trouble_open). Every change needs installer mode with every
 * partition disarmed (installer mode keeps them disarmed), and the backend checks it again itself.
 * Should a page be built outside installer mode, its actions show dimmed and a tap on one says why.
 *
 * Compiled only with ALARM_UI_ZIGBEE (sources.cmake).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define POLL_MS 1000u

/* Lucide glyphs only these pages use (icons_20 carries them; the layouts' globals.xml). */
#define G_RADIO     "\xEE\x85\x82" /* U+E142 radio */
#define G_KEYBOARD  "\xEE\x8A\x84" /* U+E284 keyboard */
#define G_LIGHTBULB "\xEE\x87\x82" /* U+E1C2 lightbulb */
#define G_X         "\xEE\x86\xB2" /* U+E1B2 x */

/* The add page's checklist: Listening, Joining, Reading what it can do, Setting it up, Ready. */
#define ADD_STEPS 5u

/* ---- the backend ----------------------------------------------------------------------------- */

/* The page whose operation runs (an add or a replace): leaving that page stops it or clears its
 * result (stop_if_leaving). 0 (PAGE_DISPLAY, never an owner): none of ours. */
static page_id_t s_owner;

static bool read_status(ui_zb_status_t *st)
{
    memset(st, 0, sizeof(*st));
    return S.be.zb_status != NULL && S.be.zb_status(S.be.ctx, st);
}

bool ui_zigbee_available(void)
{
    ui_zb_status_t st;
    return read_status(&st);
}

static void read_progress(ui_zb_progress_t *p)
{
    memset(p, 0, sizeof(*p));
    if (S.be.zb_progress != NULL) {
        S.be.zb_progress(S.be.ctx, p);
    }
}

static bool read_device(uint16_t id, ui_zb_device_page_t *out)
{
    memset(out, 0, sizeof(*out));
    return id != 0u && S.be.zb_device != NULL && S.be.zb_device(S.be.ctx, id, out);
}

static void req_init(ui_zb_request_t *req, uint8_t kind, uint16_t id)
{
    memset(req, 0, sizeof(*req));
    req->kind = kind;
    req->id = id;
}

/* Asks the backend; the request is wiped after the call (ui_backend.h: the caller wipes its copy). */
static ui_zb_req_result_t ask(ui_zb_request_t *req)
{
    ui_zb_req_result_t r = UI_ZB_REQ_UNSUPPORTED;
    req->user = S.auth_user;
    if (S.be.zb_request != NULL) {
        r = S.be.zb_request(S.be.ctx, req);
    }
    memset(req, 0, sizeof(*req));
    return r;
}

static void ask_quietly(uint8_t kind)
{
    ui_zb_request_t req;
    req_init(&req, kind, 0u);
    (void)ask(&req); /* nothing to say if it was over */
}

static const char *refused_words(ui_zb_req_result_t r)
{
    switch (r) {
    /* TR: a toast, why the Zigbee radio refused a request */
    case UI_ZB_REQ_NOT_ALLOWED: return TR("Only in installer mode, with everything disarmed");
    /* TR: a toast, why the Zigbee radio refused a request */
    case UI_ZB_REQ_BUSY: return TR("The radio is busy: try again when it's done");
    /* TR: a toast, why the Zigbee radio refused a request */
    case UI_ZB_REQ_NO_RADIO: return TR("The Zigbee radio isn't working");
    /* TR: a toast, why the Zigbee radio refused a request */
    case UI_ZB_REQ_UNSUPPORTED: return TR("This panel can't do that yet");
    /* TR: a toast, why the Zigbee radio refused a request */
    case UI_ZB_REQ_FULL: return TR("No room for another device: the panel's storage is full");
    /* TR: a toast, why the Zigbee radio refused a request */
    case UI_ZB_REQ_NOT_FOUND: return TR("That device is no longer in the network");
    /* TR: a toast, why the Zigbee radio refused a request */
    default: return TR("That can't be done now");
    }
}

/* The same reasons as whole sentences for the add page (a toast is gone before it is read). */
static const char *not_started_words(ui_zb_req_result_t r)
{
    switch (r) {
    case UI_ZB_REQ_NOT_ALLOWED:
        /* TR: a paragraph on the Add page, why nothing started */
        return TR("Not started: only in installer mode, with everything disarmed.");
    /* TR: a paragraph on the Add page, why nothing started */
    case UI_ZB_REQ_BUSY: return TR("Not started: the radio is busy: try again when it's done.");
    /* TR: a paragraph on the Add page, why nothing started */
    case UI_ZB_REQ_NO_RADIO: return TR("Not started: the Zigbee radio isn't working.");
    case UI_ZB_REQ_FULL:
        /* TR: a paragraph on the Add page, why nothing started */
        return TR("Not started: there is no room for another device. Remove one you no longer "
                  "use first.");
    /* TR: a paragraph on the Add page, why nothing started */
    case UI_ZB_REQ_UNSUPPORTED: return TR("Not started: this panel can't do that yet.");
    /* TR: a paragraph on the Add page, why nothing started */
    default: return TR("Not started: that can't be done now.");
    }
}

/* A request; its refusal is said in a toast. */
static bool request(ui_zb_request_t *req)
{
    const ui_zb_req_result_t r = ask(req);
    if (r != UI_ZB_REQ_OK) {
        ui_toast(refused_words(r));
        return false;
    }
    return true;
}

/* Changes: installer mode, every partition disarmed (installer mode keeps them disarmed). */
static bool installer(void)
{
    return S.program;
}

/* ---- words ------------------------------------------------------------------------------------ */

/* "No devices", "1 device", "8 devices" (Z-Wave's words). */
static void count_words(unsigned n, char *out, size_t len)
{
    if (n == 0u) {
        /* TR: the number of Zigbee devices, in a row. At most 20 bytes */
        i18n_utf8_copy(out, len, TR("No devices"));
    } else {
        /* TR: the number of Zigbee devices, in a row. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} device", "{0} devices", n), n);
    }
}

/* How long, in the words of "Not heard for ...": "45 min", "2 h 30 min", "3 days" (Z-Wave's). */
static void span_words(uint32_t s, char *out, size_t len)
{
    const uint32_t min = s / 60u;
    if (min < 60u) {
        /* TR: a span of time, inserted into "Not heard for {0}", "{0} ago", "Reported {0} ago" and
           "Heard {0} ago" (so it must read well in all of them). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} min", "{0} min", min == 0u ? 1u : min),
                    (unsigned)(min == 0u ? 1u : min));
    } else if (min < 48u * 60u) {
        if (min % 60u == 0u) {
            /* TR: how long a device has been silent (hours). At most 18 bytes */
            I18N_FORMAT(out, len, TRN("{0} h", "{0} h", min / 60u), (unsigned)(min / 60u));
        } else {
            /* TR: how long a device has been silent: {0} hours {1} minutes. At most 18 bytes */
            I18N_FORMAT(out, len, TRN("{0} h {1} min", "{0} h {1} min", min % 60u),
                        (unsigned)(min / 60u), (unsigned)(min % 60u));
        }
    } else {
        /* TR: how long a device has been silent (days, 2 or more). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} days", "{0} days", min / (24u * 60u)),
                    (unsigned)(min / (24u * 60u)));
    }
}

/* "Just now", "2 min ago", "Not heard yet": a device page's Last heard. */
static void ago_words(uint32_t heard_s, char *out, size_t len)
{
    if (heard_s == UI_ZB_NEVER) {
        /* TR: a row's value on a device's page. At most 39 bytes */
        i18n_utf8_copy(out, len, TR("Not heard yet"));
    } else if (heard_s < 60u) {
        /* TR: a Zigbee device page's Last heard: less than a minute ago. At most 39 bytes */
        i18n_utf8_copy(out, len, TR_C("last heard", "Just now"));
    } else {
        char span[24];
        span_words(heard_s, span, sizeof(span));
        /* TR: a row's value on a device's page; {0} is how long, like "45 min" or "2 h 30 min".
         * At most 33 bytes */
        I18N_FORMAT(out, len, TR("{0} ago"), span);
    }
}

/* Appends " · part" (or "part" to an empty line): independent facts on one line. */
static void join(char *out, size_t len, const char *part)
{
    const size_t n = strlen(out);
    if (part[0] != '\0' && n < len) {
        snprintf(out + n, len - n, "%s%s", n > 0u ? " · " : "", part);
    }
}

/* Zone names: the installer's working copy while it is being edited. */
static const alarm_config_t *zone_cfg(void)
{
    return S.program && S.edit != NULL ? S.edit : S.cfg;
}

/* "Zone 3", "Zones 3, 4"; "" for none. */
static void zones_words(const uint16_t *zones, uint8_t count, char *out, size_t len)
{
    out[0] = '\0';
    if (count == 0u) {
        return;
    }
    char list[40]; /* the zone numbers: "3, 4, 5" */
    snprintf(list, sizeof(list), "%u", (unsigned)zones[0]);
    for (size_t i = 1; i < count && i < UI_ZB_ZONES_MAX; i++) {
        char one[8];
        char joined[40];
        snprintf(one, sizeof(one), "%u", (unsigned)zones[i]);
        /* TR: a list of names or numbers: {0} what is listed so far, {1} the next one */
        I18N_FORMAT(joined, sizeof(joined), TR_C("list of names", "{0}, {1}"), list, one);
        i18n_utf8_copy(list, sizeof(list), joined);
    }
    /* TR: a Zigbee device's row in the device list, its zone numbers ({0}: "3" or "3, 4").
     * At most 25 bytes */
    I18N_FORMAT(out, len, TRN("Zone {0}", "Zones {0}", count), list);
}

static bool is_role(const ui_zb_device_t *d)
{
    return d->kind == UI_ZB_KIND_SIREN || d->kind == UI_ZB_KIND_KEYPAD ||
           d->kind == UI_ZB_KIND_KEYFOB;
}

/* What a siren, keypad or key fob is, as a row's value (it feeds no zone). */
static const char *role_words(uint8_t kind)
{
    switch (kind) {
    /* TR: what a Zigbee device is, a row's value in the device list. At most 23 bytes */
    case UI_ZB_KIND_SIREN: return TR_C("Zigbee device role", "Siren");
    /* TR: what a Zigbee device is, a row's value in the device list. At most 23 bytes */
    case UI_ZB_KIND_KEYPAD: return TR_C("Zigbee device role", "Keypad");
    /* TR: what a Zigbee device is, a row's value in the device list. At most 23 bytes */
    case UI_ZB_KIND_KEYFOB: return TR_C("Zigbee device role", "Key fob");
    default: return "";
    }
}

/* What a device is called: the installer's name, else its (first) zone's name, else its model,
 * else what it is (devices.md §9, the proposal's page 5). */
static void device_name(const ui_zb_device_t *d, char *out, size_t len)
{
    if (d->name[0] != '\0') {
        snprintf(out, len, "%s", d->name);
    } else if (d->zone_count > 0u) {
        ui_zone_name(zone_cfg(), d->zones[0], out, len);
    } else if (d->model[0] != '\0') {
        snprintf(out, len, "%s", d->model);
    } else {
        /* TR: what a Zigbee device is called when it has no name, no zone and no model. At most
         * 23 bytes */
        i18n_utf8_copy(out, len, TR("Zigbee device"));
    }
}

/* "Zone 8", "Zones 6, 9", "Siren", "Not in a zone": the device list's value. */
static void value_words(const ui_zb_device_t *d, char *out, size_t len)
{
    if (is_role(d)) {
        i18n_utf8_copy(out, len, role_words(d->kind));
    } else if (d->zone_count > 0u) {
        zones_words(d->zones, d->zone_count, out, len);
    } else if (d->kind == UI_ZB_KIND_SENSOR || d->kind == UI_ZB_KIND_DEVICE) {
        /* TR: a Zigbee device's row in the device list, its zones. At most 31 bytes */
        i18n_utf8_copy(out, len, TR("Not in a zone"));
    } else {
        out[0] = '\0';
    }
}

static const char *device_glyph(const ui_zb_device_t *d)
{
    switch (d->kind) {
    case UI_ZB_KIND_SIREN: return UI_ICON_SIREN;
    case UI_ZB_KIND_KEYPAD: return G_KEYBOARD;
    case UI_ZB_KIND_KEYFOB: return UI_ICON_KEY_ROUND;
    case UI_ZB_KIND_REPEATER: return G_RADIO;
    case UI_ZB_KIND_OTHER: return UI_ICON_PLUG;
    default: return ui_device_class_glyph((alarm_device_class_t)d->device_class);
    }
}

/* "Signal good": a fact on a device's line. */
static const char *signal_fact(uint8_t signal)
{
    switch (signal) {
    /* TR: a fact on a Zigbee device's line in the device list. At most 23 bytes */
    case UI_ZB_SIGNAL_GOOD: return TR("Signal good");
    /* TR: a fact on a Zigbee device's line in the device list. At most 23 bytes */
    case UI_ZB_SIGNAL_FAIR: return TR("Signal fair");
    /* TR: a fact on a Zigbee device's line in the device list. At most 23 bytes */
    case UI_ZB_SIGNAL_WEAK: return TR("Signal weak");
    default: return "";
    }
}

/* A device page's Signal row; `then`: it is not heard now, so this is how it was. */
static const char *signal_value(uint8_t signal, bool then)
{
    switch (signal) {
    case UI_ZB_SIGNAL_GOOD:
        /* TR: a Zigbee device page's Signal row. At most 31 bytes */
        return then ? TR_C("signal", "Good, when last heard") : TR_C("signal", "Good");
    case UI_ZB_SIGNAL_FAIR:
        /* TR: a Zigbee device page's Signal row. At most 31 bytes */
        return then ? TR_C("signal", "Fair, when last heard") : TR_C("signal", "Fair");
    case UI_ZB_SIGNAL_WEAK:
        /* TR: a Zigbee device page's Signal row. At most 31 bytes */
        return then ? TR_C("signal", "Weak, when last heard") : TR_C("signal", "Weak");
    /* TR: a Zigbee device page's Signal row. At most 31 bytes */
    default: return TR_C("signal", "Not known yet");
    }
}

/* "Battery 92 %", "Mains", "Battery low"; "" when not known. */
static void power_fact(const ui_zb_device_t *d, char *out, size_t len)
{
    out[0] = '\0';
    if (d->power == UI_ZB_POWER_MAINS) {
        /* TR: a fact on a Zigbee device's line: it runs on mains power. At most 23 bytes */
        i18n_utf8_copy(out, len, TR_C("power", "Mains"));
    } else if (d->battery_low) {
        /* TR: under a device's row in the device list */
        i18n_utf8_copy(out, len, TR("Battery low"));
    } else if (d->battery_pct != UI_ZB_BATTERY_UNKNOWN) {
        /* TR: under a device's row in the device list; {0} is a percentage 0 to 100.
         * At most 35 bytes */
        I18N_FORMAT(out, len, TR("Battery {0} %"), (unsigned)d->battery_pct);
    }
}

/* "Joined with the standard key": the add flow's words for how it joined. */
static const char *joined_how(uint8_t join)
{
    switch (join) {
    /* TR: how a Zigbee device joined, after "Ready · " or under the Added title. At most 39 bytes */
    case UI_ZB_JOIN_INSTALL_CODE: return TR("Joined with its install code");
    /* TR: how a Zigbee device joined, after "Ready · " or under the Added title. At most 39 bytes */
    case UI_ZB_JOIN_STANDARD_KEY: return TR("Joined with the standard key");
    default: return "";
    }
}

/* What it is, a device page's row (never a brand). */
static const char *kind_words(const ui_zb_device_t *d)
{
    switch (d->kind) {
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case UI_ZB_KIND_SIREN: return TR_C("Zigbee device kind", "Siren or strobe");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case UI_ZB_KIND_KEYPAD: return TR_C("Zigbee device kind", "Keypad");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case UI_ZB_KIND_KEYFOB: return TR_C("Zigbee device kind", "Key fob");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case UI_ZB_KIND_REPEATER: return TR_C("Zigbee device kind", "Range extender");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case UI_ZB_KIND_OTHER: return TR_C("Zigbee device kind", "Not a sensor");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case UI_ZB_KIND_DEVICE: return TR_C("Zigbee device kind", "Not known yet");
    default: break;
    }
    switch (d->device_class) {
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
        /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
        return TR_C("Zigbee device kind", "Door and window contact");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case ALARM_DEVCLASS_MOTION: return TR_C("Zigbee device kind", "Motion sensor");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case ALARM_DEVCLASS_SMOKE: return TR_C("Zigbee device kind", "Smoke alarm");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case ALARM_DEVCLASS_CO: return TR_C("Zigbee device kind", "CO alarm");
    /* TR: what a Zigbee device is, a row's value on its page. At most 39 bytes */
    case ALARM_DEVCLASS_WATER: return TR_C("Zigbee device kind", "Water leak sensor");
    default: return ui_device_class_name((alarm_device_class_t)d->device_class);
    }
}

/* ---- rows ------------------------------------------------------------------------------------ */

/* A label's text and colour, set only when they differ (nothing is redrawn for nothing). */
static void set_words(lv_obj_t *row, const char *name, const char *text, lv_color_t color)
{
    lv_obj_t *l = ui_find(row, name);
    if (l == NULL) {
        return;
    }
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
    lv_obj_set_flag(l, LV_OBJ_FLAG_HIDDEN, text[0] == '\0');
    if (!lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(l, color, 0);
    }
}

static void set_chip(lv_obj_t *row, const char *glyph, lv_color_t tone)
{
    lv_obj_t *chip = ui_find(row, "chip");
    lv_obj_t *icon = ui_find(chip, "icon");
    if (chip == NULL || icon == NULL) {
        return;
    }
    if (!lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), tone) ||
        strcmp(lv_label_get_text(icon), glyph) != 0) {
        ui_chip_tone(chip, glyph, tone);
    }
}

/* A second line under a row's detail, for a device's one thing to do ("Not heard for 2 h 30
 * min"): the row's words box gets a caption of its own, hidden while it says nothing. */
static lv_obj_t *sub_line(lv_obj_t *row)
{
    lv_obj_t *detail = ui_find(row, "detail_label");
    if (detail == NULL) {
        return NULL;
    }
    lv_obj_t *l = lv_label_create(lv_obj_get_parent(detail));
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, lv_obj_get_style_text_font(detail, LV_PART_MAIN), 0);
    lv_obj_move_to_index(l, lv_obj_get_index(detail) + 1);
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void set_sub(lv_obj_t *sub, const char *text, lv_color_t color)
{
    if (sub == NULL) {
        return;
    }
    if (strcmp(lv_label_get_text(sub), text) != 0) {
        lv_label_set_text(sub, text);
    }
    lv_obj_set_flag(sub, LV_OBJ_FLAG_HIDDEN, text[0] == '\0');
    if (!lv_color_eq(lv_obj_get_style_text_color(sub, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(sub, color, 0);
    }
}

/* A row whose page comes with a later unit: there, without a chevron, doing nothing yet. */
static lv_obj_t *later_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                           const char *detail, const char *value)
{
    return ui_row_link(body, glyph, tone, title, detail, value, NULL, NULL, NULL);
}

/* Why a dimmed action does nothing: ctx 0 outside installer mode, 1 while the radio isn't working. */
static void say_why(void *ctx)
{
    ui_toast((uintptr_t)ctx == 0u ? refused_words(UI_ZB_REQ_NOT_ALLOWED)
                                  : refused_words(UI_ZB_REQ_NO_RADIO));
}

/* An action row: tappable when it can act, else dimmed and saying why when tapped. fn NULL: its
 * page comes later (later_row). */
static lv_obj_t *action_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                            const char *detail, const char *value, bool can, uintptr_t why,
                            ui_click_fn fn, void *ctx)
{
    lv_obj_t *row;
    if (!can) {
        row = ui_row_link(body, glyph, tone, title, detail, value, NULL, say_why, (void *)why);
        lv_obj_set_style_opa(row, LV_OPA_40, 0);
    } else if (fn == NULL) {
        row = later_row(body, glyph, tone, title, detail, value);
    } else {
        row = ui_row_link(body, glyph, tone, title, detail, value, UI_ICON_CHEVRON_RIGHT, fn, ctx);
    }
    return row;
}

static void open_device(void *ctx)
{
    ui_page_open(PAGE_ZIGBEE_DEVICE, (int32_t)(intptr_t)ctx);
}

/* The Zigbee page's rows open what comes further down. */
static void open_radio(void *ctx);
static void open_code_page(void *ctx);
static void open_codes(void *ctx);
static void open_channel(void *ctx);
static void remove_pick(void *ctx);
static void reset_tap(void *ctx);
static void code_fill(ui_zb_request_t *req);

/* ---- "Use it for": a device's inputs put to work (the Added page and the device page) --------- */

/* Stable storage for the inputs shown on screen: row callbacks get an index, never a pointer into
 * a page struct that a later tick has already overwritten. */
static ui_zb_input_t s_use_inputs[UI_ZB_INPUTS_MAX] UI_SLOW_BSS; /* PSRAM (B001) */
static size_t s_use_n;

static bool free_zone(uint16_t *out)
{
    if (S.edit == NULL) {
        return false;
    }
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (S.edit->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
            *out = z;
            return true;
        }
    }
    return false;
}

static bool bind_input(uint16_t zone, const ui_zb_input_t *in)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = in->addr;
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, (alarm_device_class_t)in->device_class);
    if (S.be.zone_bind == NULL || S.be.zone_bind(S.be.ctx, zone, &b) != ZONE_HUB_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not saved"));
        return false;
    }
    return true;
}

/* Fills s_use_inputs with the device's inputs that feed no zone yet. */
static size_t collect_use_inputs(uint16_t id)
{
    s_use_n = 0;
    ui_zb_device_page_t page;
    if (!read_device(id, &page)) {
        return 0u;
    }
    for (uint8_t i = 0; i < page.input_count && i < UI_ZB_INPUTS_MAX; i++) {
        if (page.inputs[i].zone == 0u) {
            s_use_inputs[s_use_n++] = page.inputs[i];
        }
    }
    return s_use_n;
}

/* A new zone of the input's suggested type, then its zone page to name it. */
static void use_new_zone(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i >= s_use_n || !installer()) {
        return;
    }
    uint16_t z;
    if (!free_zone(&z)) {
        ui_toast(TR("Every zone is in use"));
        return;
    }
    /* a smoke or CO input must not become a burglary zone */
    ui_installer_create_zone_for(z, (alarm_device_class_t)s_use_inputs[i].device_class);
    if (!bind_input(z, &s_use_inputs[i])) {
        return;
    }
    ui_installer_apply();
    ui_page_open(PAGE_ZONE, z);
}

static uint16_t s_pick_zones[ALARM_MAX_ZONES] UI_SLOW_BSS;
static size_t s_pick_n;
static size_t s_pick_input;    /* index into s_use_inputs */
static uint16_t s_pick_chosen; /* the zone chosen, once existing_zone_chosen ran */

static void existing_zone_confirmed(void)
{
    if (s_pick_input < s_use_n && bind_input(s_pick_chosen, &s_use_inputs[s_pick_input])) {
        ui_page_open(PAGE_ZONE, s_pick_chosen);
    }
}

static void existing_zone_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_pick_n || S.edit == NULL) {
        return;
    }
    s_pick_chosen = s_pick_zones[(size_t)index];
    char name[ALARM_NAME_LEN + 8];
    char title[96];
    /* TR: a confirmation's title; {0} is a zone's name. At most 64 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Replace {0}'s sensor?"),
                ui_zone_name(S.edit, s_pick_chosen, name, sizeof(name)));
    ui_confirm(title, TR("Its current sensor stops feeding this zone."),
               TR_C("confirm button", "Replace"), existing_zone_confirmed);
}

/* An existing zone: pick it, then a question before its sensor is replaced. */
static void use_existing_zone(void *ctx)
{
    s_pick_input = (size_t)(uintptr_t)ctx;
    if (s_pick_input >= s_use_n || S.edit == NULL || !installer()) {
        return;
    }
    static ui_choice_t choices[ALARM_MAX_ZONES] UI_SLOW_BSS; /* PSRAM (B001) */
    static char titles[ALARM_MAX_ZONES][ALARM_NAME_LEN + 8] UI_SLOW_BSS;
    s_pick_n = 0;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (S.edit->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
            continue;
        }
        s_pick_zones[s_pick_n] = z;
        ui_zone_name(S.edit, z, titles[s_pick_n], sizeof(titles[s_pick_n]));
        choices[s_pick_n].glyph = NULL;
        choices[s_pick_n].tone = ui_tone_blue();
        choices[s_pick_n].title = titles[s_pick_n];
        choices[s_pick_n].detail = NULL;
        s_pick_n++;
    }
    if (s_pick_n == 0u) {
        ui_toast(TR("No zones yet"));
        return;
    }
    ui_pick_sheet(TR("Pick a zone"), TR("Its current sensor is replaced"), choices, s_pick_n, -1,
                  existing_zone_chosen);
}

/* "Perimeter, as Zone 12": what A new zone would make of input i. */
static void new_zone_words(size_t i, char *out, size_t len)
{
    uint16_t z;
    const alarm_zone_type_t type =
        zsrc_suggest_zone_type((alarm_device_class_t)s_use_inputs[i].device_class);
    if (free_zone(&z)) {
        /* TR: under "A new zone": {0} its type ("Perimeter"), {1} its number. At most 63 bytes */
        I18N_FORMAT(out, len, TR("{0}, as Zone {1}"), ui_zone_type_name(type), (unsigned)z);
    } else {
        /* TR: under "A new zone" when every zone is used. At most 63 bytes */
        i18n_utf8_copy(out, len, TR("Every zone is in use"));
    }
}

/* The rows of "Use it for": one input as drawn, several each with its words. Returns how many. */
static size_t render_use_it_for(lv_obj_t *body, uint16_t id)
{
    const size_t n = collect_use_inputs(id);
    static char words[UI_ZB_INPUTS_MAX][64];
    for (size_t i = 0; i < n; i++) {
        new_zone_words(i, words[i], sizeof(words[i]));
        const bool one = n == 1u;
        ui_row_link(body, UI_ICON_PLUS, ui_tone_green(),
                    /* TR: a row on a Zigbee device's Added page */
                    one ? TR("A new zone") : s_use_inputs[i].words, one ? words[i] : NULL,
                    one ? NULL : TR("A new zone"), UI_ICON_CHEVRON_RIGHT, use_new_zone,
                    (void *)(uintptr_t)i);
        ui_row_link(body, UI_ICON_ARROW_LEFT_RIGHT, ui_tone_blue(),
                    /* TR: a row on a Zigbee device's Added page */
                    one ? TR("An existing zone") : s_use_inputs[i].words,
                    /* TR: under "An existing zone" on a Zigbee device's Added page */
                    one ? TR("Its sensor is replaced, after a question") : NULL,
                    /* TR: a row's value on a Zigbee device's Added page */
                    one ? NULL : TR("An existing zone"), UI_ICON_CHEVRON_RIGHT, use_existing_zone,
                    (void *)(uintptr_t)i);
    }
    return n;
}

/* The device page's "Make it a zone": the one input at once, else a pick of which. */
static void make_zone_for_input(int32_t index)
{
    if (index >= 0 && (size_t)index < s_use_n) {
        use_new_zone((void *)(uintptr_t)index);
    }
}

static void make_it_a_zone(void *ctx)
{
    const uint16_t id = (uint16_t)(uintptr_t)ctx;
    if (collect_use_inputs(id) == 0u) {
        ui_toast(TR("Nothing to use yet"));
        return;
    }
    if (s_use_n == 1u) {
        use_new_zone((void *)(uintptr_t)0);
        return;
    }
    static ui_choice_t choices[UI_ZB_INPUTS_MAX] UI_SLOW_BSS;
    for (size_t i = 0; i < s_use_n; i++) {
        choices[i].glyph = ui_device_class_glyph((alarm_device_class_t)s_use_inputs[i].device_class);
        choices[i].tone = ui_tone_blue();
        choices[i].title = s_use_inputs[i].words;
        choices[i].detail = NULL;
    }
    ui_pick_sheet(TR("Pick what to use"), TR("Makes a new zone"), choices, s_use_n, -1,
                  make_zone_for_input);
}

/* ---- the installer menu's row (the proposal's page 1) ---------------------------------------- */

static lv_obj_t *s_menu_row; /* while the installer menu shows it */
static uint32_t s_menu_polled;

/* "8 devices", "No devices", "Not connected", "Starting"; "Radio problem" or "Interference" in
 * amber. */
static void menu_words(lv_obj_t *row, const ui_zb_status_t *st)
{
    char value[32];
    lv_color_t color = COLOR_TEXT_2;
    switch (st->radio) {
    case UI_ZB_RADIO_WORKING:
        if (st->jammed) {
            /* TR: the Zigbee row's value in the installer menu, and after "Radio · " on its
             * page. At most 31 bytes */
            i18n_utf8_copy(value, sizeof(value), TR_C("radio state", "Interference"));
            color = ui_tone_amber();
        } else {
            count_words(st->devices, value, sizeof(value));
        }
        break;
    case UI_ZB_RADIO_NOT_CONNECTED:
        /* TR: the state of the Zigbee radio, a row's value */
        i18n_utf8_copy(value, sizeof(value), TR_C("radio state", "Not connected"));
        break;
    case UI_ZB_RADIO_STARTING:
        /* TR: the state of the Zigbee radio, a row's value */
        i18n_utf8_copy(value, sizeof(value), TR_C("radio state", "Starting"));
        break;
    default:
        /* TR: the Zigbee row's value in the installer menu. At most 31 bytes */
        i18n_utf8_copy(value, sizeof(value), TR("Radio problem"));
        color = ui_tone_amber();
        break;
    }
    set_words(row, "value_label", value, color);
}

static void open_zigbee(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZIGBEE, 0);
}

void ui_zigbee_menu_row(lv_obj_t *body)
{
    ui_zb_status_t st;
    if (!read_status(&st)) {
        return; /* no Zigbee service: no row (devices.md §9) */
    }
    s_menu_row = ui_row_link(body, G_RADIO, ui_tone_blue(), NOTR("Zigbee") /* a protocol name */,
                             /* TR: the Zigbee row's detail in the installer menu, and the Zigbee
                              * page's note */
                             TR("Sensors on the panel's Zigbee radio"), "-", UI_ICON_CHEVRON_RIGHT,
                             open_zigbee, NULL);
    menu_words(s_menu_row, &st);
    s_menu_polled = lv_tick_get();
}

/* ---- the Zigbee page (PAGE_ZIGBEE, the proposal's page 2) ------------------------------------- */

typedef struct {
    uint16_t id;
    lv_obj_t *row;
    lv_obj_t *sub;
} shown_t;

static struct {
    lv_obj_t *radio;   /* the page is on screen while this is set */
    lv_obj_t *codes;   /* "Install codes waiting": its value */
    lv_obj_t *channel; /* "Channel": its value */
    lv_obj_t *reset;   /* "Reset the Zigbee network": its detail */
    lv_obj_t *list;    /* the devices' rows */
    lv_obj_t *empty;   /* "No devices yet" */
    size_t mark;       /* the device rows' bindings start here */
    shown_t *shown;    /* UI_ZB_DEVICES_MAX, from ui_slow_alloc */
    size_t nshown;
    ui_zb_device_t *devs; /* UI_ZB_DEVICES_MAX, from ui_slow_alloc */
    uint32_t shape;    /* net_shape() as built: the page is made again when it changes */
    uint32_t polled;
} s_net;

typedef struct {
    const char *glyph;
    lv_color_t tone;
    char title[48];
    char detail[96];
    lv_color_t detail_color;
} look_t;

/* The radio can add and remove (the proposal's working, empty and noisy pages). */
static bool radio_usable(const ui_zb_status_t *st)
{
    return st->radio == UI_ZB_RADIO_WORKING;
}

/* What the page's rows and paragraphs depend on: a change rebuilds it (once, not each second). */
static uint32_t net_shape(const ui_zb_status_t *st)
{
    return (uint32_t)st->radio | (st->jammed ? 0x10u : 0u) | (st->devices > 0u ? 0x20u : 0u) |
           (installer() ? 0x40u : 0u);
}

/* "Radio · Working": the radio's row title, its state after the word. */
static void radio_title(look_t *l, const char *state)
{
    /* TR: the first word of the Zigbee radio's row title, before " · " and its state. At most
     * 15 bytes */
    snprintf(l->title, sizeof(l->title), "%s · %s", TR_C("Zigbee radio", "Radio"), state);
}

static void radio_look(const ui_zb_status_t *st, look_t *l)
{
    l->glyph = G_RADIO;
    l->tone = ui_tone_amber();
    l->detail_color = ui_tone_amber();
    l->detail[0] = '\0';
    switch (st->radio) {
    case UI_ZB_RADIO_WORKING: {
        char devices[24];
        count_words(st->devices, devices, sizeof(devices));
        if (st->jammed) { /* ZB-006 */
            radio_title(l, TR_C("radio state", "Interference"));
            /* TR: under the Zigbee radio's row; {0} is its channel, 11 to 26 */
            I18N_FORMAT(l->detail, sizeof(l->detail),
                        TR("Something nearby drowns channel {0}: sensors may not be heard"),
                        (unsigned)st->channel);
            break;
        }
        /* TR: the state of the Zigbee radio, a row's value; also after "Radio · " in the page's
         * first row. At most 31 bytes */
        radio_title(l, TR_C("radio state", "Working"));
        l->tone = ui_tone_blue();
        l->detail_color = COLOR_TEXT_2;
        if (st->network_known && st->channel != 0u) {
            char ch[24];
            /* TR: under the Zigbee radio's row, its channel ({0}: 11 to 26). At most 23 bytes */
            I18N_FORMAT(ch, sizeof(ch), TR("Channel {0}"), (unsigned)st->channel);
            join(l->detail, sizeof(l->detail), ch);
        }
        join(l->detail, sizeof(l->detail), devices);
        break;
    }
    case UI_ZB_RADIO_STARTING:
        radio_title(l, TR_C("radio state", "Starting"));
        /* TR: under the Zigbee radio's row while it starts */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("A few seconds"));
        l->tone = ui_tone_blue();
        l->detail_color = COLOR_TEXT_2;
        break;
    case UI_ZB_RADIO_NOT_ANSWERING:
        radio_title(l, TR_C("radio state", "Not answering"));
        /* TR: under the Zigbee radio's row on the Zigbee page */
        i18n_utf8_copy(l->detail, sizeof(l->detail),
                       TR("Check that it sits firmly in its socket; the panel keeps trying"));
        break;
    case UI_ZB_RADIO_PAUSED:
        /* TR: the state of the Zigbee radio after "Radio · ". At most 31 bytes */
        radio_title(l, TR_C("radio state", "Paused after restarts"));
        /* TR: under the Zigbee radio's row when the restart guard stopped using it */
        i18n_utf8_copy(l->detail, sizeof(l->detail),
                       st->paused_why == UI_ZB_PAUSE_FIRMWARE
                           ? TR("Its software doesn't match the panel's, so the panel stopped using it")
                           : TR("It restarted the panel twice, so the panel stopped using it"));
        break;
    case UI_ZB_RADIO_SOCKET_ZWAVE:
        /* TR: the state of the Zigbee radio after "Radio · ". At most 31 bytes */
        radio_title(l, TR_C("radio state", "Socket used by Z-Wave"));
        /* TR: under the Zigbee radio's row */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("The wireless socket holds the Z-Wave radio"));
        break;
    case UI_ZB_RADIO_UNUSABLE:
        /* TR: the state of the Zigbee radio, a row's value */
        radio_title(l, TR_C("radio state", "Can't be used"));
        /* TR: under the Zigbee radio's row */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Its software isn't one the panel can use"));
        break;
    default: /* UI_ZB_RADIO_NOT_CONNECTED: a choice, not a fault - gray */
        radio_title(l, TR_C("radio state", "Not connected"));
        /* TR: under the Zigbee radio's row when its socket is empty */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Nothing answers in the wireless socket"));
        l->tone = ui_tone_gray();
        l->detail_color = COLOR_TEXT_2;
        break;
    }
}

/* The one thing to do about a device, its row's second line: words and colour; "" none. */
static void device_sub(const ui_zb_device_t *d, char *out, size_t len, lv_color_t *color)
{
    out[0] = '\0';
    *color = ui_tone_amber();
    if (d->unreachable) {
        if (d->heard_s == UI_ZB_NEVER) {
            /* TR: under a device's row in the device list. At most 39 bytes */
            i18n_utf8_copy(out, len, TR("Not heard since the panel started"));
        } else {
            char span[24];
            span_words(d->heard_s, span, sizeof(span));
            /* TR: under a device's row in the device list; {0} is a span such as "45 min".
             * At most 21 bytes */
            I18N_FORMAT(out, len, TR("Not heard for {0}"), span);
        }
    } else if (d->state == UI_ZB_DEV_WAKE_NEEDED) {
        /* TR: under a device's row in the device list */
        i18n_utf8_copy(out, len, TR("Wake it to finish setting it up"));
    } else if (d->state == UI_ZB_DEV_SETUP_STOPPED) {
        /* TR: under a device's row in the device list */
        i18n_utf8_copy(out, len, TR("Setup stopped: open it to try again"));
    } else if (d->state == UI_ZB_DEV_SETTING_UP || d->state == UI_ZB_DEV_JOINING) {
        /* TR: under a device's row in the device list */
        i18n_utf8_copy(out, len, TR("Being set up"));
        *color = ui_tone_blue();
    } else if (d->battery_low) {
        /* TR: under a Zigbee device's row: the one thing to do. At most 63 bytes */
        i18n_utf8_copy(out, len, TR("Battery low: change it soon"));
    } else if (d->signal == UI_ZB_SIGNAL_WEAK) {
        /* TR: under a Zigbee device's row: the one thing to do. At most 63 bytes */
        i18n_utf8_copy(out, len, TR("Signal weak: move it closer or add a mains device between"));
    }
}

static void device_words_to(lv_obj_t *row, lv_obj_t *sub, const ui_zb_device_t *d)
{
    char title[48];
    char detail[96] = "";
    char value[32];
    char power[40];
    char said[64];
    lv_color_t said_color;
    device_name(d, title, sizeof(title));
    value_words(d, value, sizeof(value));
    device_sub(d, said, sizeof(said), &said_color);
    if (strcmp(title, d->model) != 0) {
        join(detail, sizeof(detail), d->model);
    }
    power_fact(d, power, sizeof(power));
    if (!d->battery_low) { /* said below, as the thing to do */
        join(detail, sizeof(detail), power);
    }
    join(detail, sizeof(detail), signal_fact(d->signal));
    const bool warn = said[0] != '\0' && lv_color_eq(said_color, ui_tone_amber());
    set_chip(row, device_glyph(d),
             d->state == UI_ZB_DEV_NOT_USABLE ? ui_tone_gray()
             : warn                            ? ui_tone_amber()
                                               : ui_tone_green());
    set_words(row, "title_label", title, COLOR_TEXT);
    set_words(row, "detail_label", detail, COLOR_TEXT_2);
    set_words(row, "value_label", value, COLOR_TEXT_2);
    set_sub(sub, said, said_color);
}

/* Unreachable devices first, then by name (the proposal's page 2). */
static bool before(const ui_zb_device_t *a, const ui_zb_device_t *b)
{
    if (a->unreachable != b->unreachable) {
        return a->unreachable;
    }
    char na[48];
    char nb[48];
    device_name(a, na, sizeof(na));
    device_name(b, nb, sizeof(nb));
    const int c = strcmp(na, nb);
    return c != 0 ? c < 0 : a->id < b->id;
}

static size_t read_devices(ui_zb_device_t *out)
{
    if (S.be.zb_devices == NULL) {
        return 0u;
    }
    size_t n = S.be.zb_devices(S.be.ctx, out, UI_ZB_DEVICES_MAX);
    n = n < UI_ZB_DEVICES_MAX ? n : UI_ZB_DEVICES_MAX;
    for (size_t i = 1; i < n; i++) { /* 64 at most: insertion */
        const ui_zb_device_t d = out[i];
        size_t j = i;
        for (; j > 0u && before(&d, &out[j - 1u]); j--) {
            out[j] = out[j - 1u];
        }
        out[j] = d;
    }
    return n;
}

/* The device rows, made again when the devices or their order changed, else only their words. */
static void show_devices(void)
{
    if (s_net.list == NULL || s_net.devs == NULL || s_net.shown == NULL) {
        return;
    }
    const size_t n = read_devices(s_net.devs);
    bool same = n == s_net.nshown;
    for (size_t i = 0; same && i < n; i++) {
        same = s_net.shown[i].id == s_net.devs[i].id;
    }
    if (!same) {
        lv_obj_clean(s_net.list);
        ui_rows_release(s_net.mark);
        s_net.nshown = 0;
        for (size_t i = 0; i < n; i++) {
            const ui_zb_device_t *d = &s_net.devs[i];
            shown_t *sh = &s_net.shown[s_net.nshown++];
            sh->id = d->id;
            /* every device opens its page; a siren's, keypad's or key fob's leads with its role
             * (bl-147-siren, bl-148-keypad, bl-148-fob) */
            sh->row = ui_row_link(s_net.list, UI_ICON_CIRCLE_DOT, ui_tone_green(), "-", "-", "-",
                                  UI_ICON_CHEVRON_RIGHT, open_device, (void *)(uintptr_t)d->id);
            sh->sub = sub_line(sh->row);
        }
    }
    for (size_t i = 0; i < s_net.nshown; i++) {
        device_words_to(s_net.shown[i].row, s_net.shown[i].sub, &s_net.devs[i]);
    }
    if (s_net.empty != NULL) {
        lv_obj_set_flag(s_net.empty, LV_OBJ_FLAG_HIDDEN, n > 0u);
    }
}

/* Its words, in place; a change of shape makes the page again (true: it was made again). */
static bool show_status(void)
{
    ui_zb_status_t st;
    if (!read_status(&st)) {
        return false;
    }
    if (net_shape(&st) != s_net.shape) {
        ui_page_rebuild();
        return true;
    }
    look_t l;
    radio_look(&st, &l);
    set_chip(s_net.radio, l.glyph, l.tone);
    set_words(s_net.radio, "title_label", l.title, COLOR_TEXT);
    set_words(s_net.radio, "detail_label", l.detail, l.detail_color);
    if (s_net.codes != NULL) {
        char codes[16];
        if (st.codes_waiting == 0u) {
            /* TR: the value of "Install codes waiting" when there are none. At most 15 bytes */
            i18n_utf8_copy(codes, sizeof(codes), TR_C("install codes waiting", "None"));
        } else {
            snprintf(codes, sizeof(codes), "%u", (unsigned)st.codes_waiting);
        }
        set_words(s_net.codes, "value_label", codes, COLOR_TEXT_2);
    }
    if (s_net.channel != NULL) {
        char ch[8] = "-";
        if (st.network_known && st.channel != 0u) {
            snprintf(ch, sizeof(ch), "%u", (unsigned)st.channel);
        }
        set_words(s_net.channel, "value_label", ch, COLOR_TEXT_2);
    }
    return false;
}

static void retry_radio(void *ctx)
{
    (void)ctx;
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_RETRY, 0u);
    if (request(&req)) {
        show_status(); /* "Starting" at once */
    }
}

static void open_add(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZIGBEE_ADD, 0);
}

static lv_obj_t *device_list(lv_obj_t *body)
{
    lv_obj_t *list = lv_obj_create(body);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, SPACE_SM, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_CLICKABLE); /* a press between rows is the page's */
    return list;
}

/* What the page says under the radio's row, by the radio's state. */
static void radio_paragraphs(lv_obj_t *body, const ui_zb_status_t *st)
{
    switch (st->radio) {
    case UI_ZB_RADIO_NOT_CONNECTED:
        if (st->devices > 0u) {
            ui_paragraph(body,
                         /* TR: a paragraph on the Zigbee page when no radio answers */
                         TR("Devices added before are kept. They are heard again once the Zigbee "
                            "radio answers."),
                         ui_tone_gray());
        }
        break;
    case UI_ZB_RADIO_NOT_ANSWERING:
        ui_paragraph(body,
                     /* TR: a paragraph on the Zigbee page while the radio doesn't answer; "Sensor
                      * radio problem" is the trouble's name on Troubles. At most 127 bytes */
                     TR("Zigbee sensors aren't heard until it answers. Their zones keep their last "
                        "state; a Sensor radio problem follows if it lasts."),
                     ui_tone_amber());
        break;
    case UI_ZB_RADIO_PAUSED:
        if (installer()) {
            ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), retry_radio, NULL);
        }
        ui_paragraph(body,
                     /* TR: a paragraph on the Zigbee page while the radio is paused; "Sensor radio
                      * problem" is the trouble's name on Troubles. At most 127 bytes */
                     TR("Zigbee sensors aren't heard while it is paused. Their zones keep their last "
                        "state; a Sensor radio problem follows if it lasts."),
                     ui_tone_amber());
        break;
    case UI_ZB_RADIO_SOCKET_ZWAVE:
        ui_paragraph(body,
                     /* TR: a paragraph on the Zigbee page when the socket holds the Z-Wave radio */
                     TR("Zigbee needs the wireless socket. The manual says where else the Z-Wave "
                        "radio can go."),
                     ui_tone_gray());
        break;
    case UI_ZB_RADIO_WORKING:
        if (st->jammed) {
            ui_paragraph(body,
                         /* TR: a paragraph on the Zigbee page while its channel is drowned out */
                         TR("Move Wi-Fi routers and other radios away from the panel, or choose a "
                            "quieter channel below."),
                         ui_tone_amber());
        }
        break;
    default: break;
    }
}

static void build_network(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                          size_t nlen)
{
    (void)arg;
    i18n_utf8_copy(title, tlen, NOTR("Zigbee")); /* a protocol name */
    i18n_utf8_copy(note, nlen, TR("Sensors on the panel's Zigbee radio"));
    if (s_net.shown == NULL) {
        s_net.shown = ui_slow_alloc(UI_ZB_DEVICES_MAX * sizeof(*s_net.shown));
    }
    if (s_net.devs == NULL) {
        s_net.devs = ui_slow_alloc(UI_ZB_DEVICES_MAX * sizeof(*s_net.devs));
    }
    ui_zb_status_t st;
    if (!read_status(&st) || s_net.shown == NULL || s_net.devs == NULL) {
        /* TR: a paragraph on a Zigbee page */
        ui_paragraph(body, TR("Zigbee isn't available on this panel."), ui_tone_gray());
        return;
    }
    const bool can = installer();
    const bool usable = can && radio_usable(&st);
    const uintptr_t why = can ? 1u : 0u;
    s_net.radio = ui_row_link(body, G_RADIO, ui_tone_gray(), "-", "-", NULL, UI_ICON_CHEVRON_RIGHT,
                              open_radio, NULL);
    radio_paragraphs(body, &st);
    if (!can) {
        ui_paragraph(body,
                     /* TR: a paragraph on the Zigbee page outside installer mode */
                     TR("Adding, removing and changing Zigbee devices is done in installer mode."),
                     ui_tone_gray());
    }
    action_row(body, UI_ICON_PLUS, ui_tone_green(), TR("Add a device"),
               /* TR: under "Add a device" on the Zigbee page */
               TR("Any Zigbee 3.0 device, for 3 minutes"), NULL, usable, why, open_add, NULL);
    action_row(body, UI_ICON_QR_CODE, ui_tone_blue(),
               /* TR: a row on the Zigbee page */
               TR("Add with an install code"), TR("For a device whose label has one"), NULL,
               usable, why, open_code_page, NULL);
    action_row(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove a device"),
               /* TR: under "Remove a device" on the Zigbee page */
               TR("Asks it to leave the network"), NULL, usable, why, remove_pick, NULL);
    s_net.codes = action_row(body, UI_ICON_KEY_ROUND, ui_tone_amber(),
                             /* TR: a row on the Zigbee page */
                             TR("Install codes waiting"),
                             TR("Devices that may join with their own key"), "-", can, 0u,
                             open_codes, NULL);
    /* TR: a section heading on the Zigbee page */
    ui_section(body, TR_C("Z-Wave section", "Devices"));
    s_net.list = device_list(body);
    s_net.empty = ui_paragraph(body,
                               /* TR: a paragraph on the Zigbee page with no devices */
                               TR("No devices yet. Add one: most sensors join when you hold their "
                                  "button until their light blinks."),
                               ui_tone_gray());
    s_net.channel = NULL;
    s_net.reset = NULL;
    if (can) {
        /* TR: a section heading on the Zigbee page */
        ui_section(body, TR_C("Z-Wave section", "Network"));
        s_net.channel = action_row(body, UI_ICON_WAVES, ui_tone_blue(),
                                   /* TR: a row on the Zigbee page */
                                   TR_C("Zigbee settings", "Channel"),
                                   TR("Where the Zigbee network talks"), "-", true, 0u,
                                   open_channel, NULL);
        s_net.reset = action_row(body, UI_ICON_ROTATE_CCW, ui_tone_red(),
                                 /* TR: a row on the Zigbee page */
                                 TR("Reset the Zigbee network"),
                                 st.devices > 0u ? TR("Every device must be added again")
                                                 /* TR: under "Reset the Zigbee network" */
                                                 : TR("A new network, with new keys"),
                                 NULL, usable, why, reset_tap, NULL);
        set_words(s_net.reset, "title_label", TR("Reset the Zigbee network"), ui_tone_red());
    }
    /* The device rows last: their bindings can then be dropped and made again on their own. */
    s_net.mark = ui_rows_mark();
    s_net.nshown = 0;
    s_net.shape = net_shape(&st);
    (void)show_status();
    show_devices();
    s_net.polled = lv_tick_get();
}

/* ---- adding a device (PAGE_ZIGBEE_ADD, the proposal's page 3) -------------------------------- */

typedef enum {
    AS_NONE = 0,      /* nothing of ours runs */
    AS_LISTEN,        /* listening, nobody joined yet */
    AS_JOINED,        /* listening, devices joined: their rows */
    AS_ALREADY,       /* the one device that joined was known already */
    AS_NOT_SENSOR,    /* the one device that joined has nothing to use as a zone */
    AS_NOTHING,       /* ended: nothing joined */
    AS_DIDNT_JOIN,    /* ended: one began to join and didn't finish */
    AS_NOT_FINISHED,  /* ended: one joined and its setup didn't finish */
    AS_FULL,          /* ended: one joined with no room left and was asked to leave */
    AS_REPLACED,      /* ended: the new device took the old one's zones */
    AS_FAILED,        /* ended: it couldn't be done */
    AS_DONE_JOINED,   /* ended: the devices that joined are ready */
} add_shape_t;

static struct {
    int32_t arg;      /* 0: add; 1: from the zone picker (a device ready goes back to it); 2: a
                       * new device in s_replace_id's place; 3: the device of the install code
                       * page (PAGE_ZIGBEE_CODE) only */
    uint32_t sig;     /* add_sig() as built: the page is made again when it changes */
    bool open;
    uint32_t polled;
    uint8_t kept;     /* joined rows whose "Not a sensor" was answered Keep it (bit per row) */
    lv_obj_t *row[UI_ZB_JOINED_MAX];
    lv_obj_t *sub[UI_ZB_JOINED_MAX];
} s_add;
static uint16_t s_replace_id;
static char s_replace_name[48];

static bool adding_op(uint8_t op)
{
    return op == UI_ZB_OP_ADD || op == UI_ZB_OP_ADD_CODE || op == UI_ZB_OP_REPLACE;
}

static add_shape_t add_shape(const ui_zb_progress_t *p)
{
    if (!adding_op(p->op) || p->step == UI_ZB_STEP_IDLE) {
        return AS_NONE;
    }
    if (p->step == UI_ZB_STEP_DONE) {
        switch (p->result) {
        case UI_ZB_RES_JOIN_FAILED: return AS_DIDNT_JOIN;
        case UI_ZB_RES_SETUP_NOT_FINISHED: return AS_NOT_FINISHED;
        case UI_ZB_RES_FULL: return AS_FULL;
        case UI_ZB_RES_REPLACED: return AS_REPLACED;
        case UI_ZB_RES_FAILED: return AS_FAILED;
        default: return p->joined_count > 0u ? AS_DONE_JOINED : AS_NOTHING;
        }
    }
    if (p->joined_count == 0u) {
        return AS_LISTEN;
    }
    const ui_zb_joined_t *j = &p->joined[0];
    if (p->joined_count == 1u && j->step == UI_ZB_STEP_DONE && (s_add.kept & 1u) == 0u) {
        if (j->result == UI_ZB_RES_ALREADY_ADDED) {
            return AS_ALREADY;
        }
        if (j->result == UI_ZB_RES_NOT_A_SENSOR) {
            return AS_NOT_SENSOR;
        }
    }
    return AS_JOINED;
}

/* The joined rows' shape: who they are, whether each ended (and how) or dozed off. */
static uint32_t add_sig(const ui_zb_progress_t *p)
{
    uint32_t h = 2166136261u;
    const uint32_t parts[3] = {(uint32_t)add_shape(p), p->joined_count, s_add.kept};
    for (size_t i = 0; i < 3u; i++) {
        h = (h ^ parts[i]) * 16777619u;
    }
    for (size_t i = 0; i < p->joined_count && i < UI_ZB_JOINED_MAX; i++) {
        const ui_zb_joined_t *j = &p->joined[i];
        const uint32_t v = (uint32_t)j->id | (uint32_t)(j->asleep ? 1u : 0u) << 16 |
                           (uint32_t)(j->step == UI_ZB_STEP_DONE ? 1u : 0u) << 17 |
                           (uint32_t)j->result << 18;
        h = (h ^ v) * 16777619u;
    }
    return h;
}

/* Where a joined device is in the five steps: 2 joining .. 5 ready. */
static unsigned step_number(uint8_t step)
{
    switch (step) {
    case UI_ZB_STEP_JOINING: return 2u;
    case UI_ZB_STEP_READING: return 3u;
    case UI_ZB_STEP_SETTING_UP: return 4u;
    case UI_ZB_STEP_DONE: return 5u;
    default: return 1u;
    }
}

static const char *const STEP_NAMES[ADD_STEPS] = {
    TR_NOOP_C("Zigbee add step", "Listening"), TR_NOOP_C("Zigbee add step", "Joining"),
    TR_NOOP_C("Zigbee add step", "Reading what it can do"),
    TR_NOOP_C("Zigbee add step", "Setting it up"), TR_NOOP_C("Zigbee add step", "Ready")};

/* "Listening · 2 min 41 s left", "Starting". */
static void listen_note(const ui_zb_progress_t *p, char *note, size_t nlen)
{
    if (p->step == UI_ZB_STEP_STARTING || p->seconds_left == 0u) {
        /* TR: the add page's subtitle while the radio is told to listen. At most 63 bytes */
        i18n_utf8_copy(note, nlen, TR("Starting"));
    } else if (p->seconds_left >= 60u) {
        char secs[4];
        snprintf(secs, sizeof(secs), "%02u", (unsigned)(p->seconds_left % 60u));
        /* TR: the add page's subtitle while listening; {0} minutes, {1} seconds ("05"). At
         * most 63 bytes */
        I18N_FORMAT(note, nlen, TR("Listening · {0} min {1} s left"),
                    (unsigned)(p->seconds_left / 60u), secs);
    } else {
        /* TR: The page's subtitle while listening. At most 60 bytes. */
        I18N_FORMAT(note, nlen,
                    TRN("Listening · {0} s left", "Listening · {0} s left",
                        (unsigned)p->seconds_left),
                    (unsigned)p->seconds_left);
    }
}

static void add_note(const ui_zb_progress_t *p, char *note, size_t nlen)
{
    if (p->step == UI_ZB_STEP_DONE) {
        /* TR: the add page's subtitle once it stopped listening. At most 63 bytes */
        i18n_utf8_copy(note, nlen, TR("Stopped listening"));
    } else {
        listen_note(p, note, nlen);
    }
}

/* The request the add page runs by its arg. */
static void add_fill(ui_zb_request_t *req)
{
    if (s_add.arg == 3) {
        code_fill(req); /* ADD_CODE with what the install code page holds */
    } else {
        req_init(req, s_add.arg == 2 ? UI_ZB_REQ_REPLACE : UI_ZB_REQ_ADD,
                 s_add.arg == 2 ? s_replace_id : 0u);
    }
}

static void add_request(void)
{
    ui_zb_request_t req;
    add_fill(&req);
    (void)request(&req);
}

static void add_again(void *ctx)
{
    (void)ctx;
    ask_quietly(UI_ZB_REQ_DISMISS); /* the ended one was seen */
    s_add.kept = 0u;
    add_request();
    ui_page_rebuild();
}

static void add_keep_listening(void *ctx)
{
    (void)ctx;
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_KEEP_LISTENING, 0u);
    (void)request(&req);
}

static void add_stop(void *ctx)
{
    (void)ctx;
    ui_page_back(); /* leaving stops listening (stop_if_leaving) */
}

static void add_done(void *ctx)
{
    (void)ctx;
    ui_page_back();
}

/* A device's page in place of the add page: back from it is the Zigbee page. */
static void add_open_device(void *ctx)
{
    ui_page_back();
    ui_page_open(PAGE_ZIGBEE_DEVICE, (int32_t)(intptr_t)ctx);
}

static void open_added(void *ctx)
{
    ui_page_open(PAGE_ZIGBEE_ADDED, (int32_t)(intptr_t)ctx);
}

static void listen_buttons(lv_obj_t *body)
{
    ui_action_pair(body, UI_ICON_TIMER, TR("Keep listening"), ui_tone_blue(), add_keep_listening,
                   G_X, TR("Stop"), ui_tone_gray(), add_stop, NULL);
}

static void code_listen_words(char *out, size_t len);

/* The checklist while nobody joined: Listening now, the rest waiting. */
static void render_listen(lv_obj_t *body)
{
    char only[160];
    code_listen_words(only, sizeof(only));
    ui_paragraph(body,
                 s_add.arg == 3 ? only
                 : s_add.arg == 2
                     /* TR: a paragraph on the Replace page while listening */
                     ? TR("Put the new device in pairing mode now: it takes the old one's place, "
                          "and its zones follow it. Keep it close to the panel.")
                     /* TR: a paragraph on the Add a Zigbee device page while listening */
                     : TR("Put the device in pairing mode now: most sensors, hold their button "
                          "until their light blinks. Keep it close to the panel."),
                 ui_tone_gray());
    for (size_t i = 0; i < ADD_STEPS; i++) {
        const char *detail = i == 0u ? (s_add.arg == 3
                                            /* TR: the Listening step's detail with an install
                                             * code */
                                            ? TR("Only this device may join")
                                        : s_add.arg == 2
                                            /* TR: the Listening step's detail when replacing */
                                            ? TR("The new device may join")
                                            /* TR: the Listening step's detail */
                                            : TR("Any Zigbee 3.0 device may join"))
                                     : TR_C("checklist state", "Waiting");
        ui_row_link(body, UI_ICON_CIRCLE_DOT, i == 0u ? ui_tone_blue() : ui_tone_gray(),
                    TR_C("Zigbee add step", STEP_NAMES[i]), detail, NULL, NULL, NULL, NULL);
    }
    listen_buttons(body);
    /* TR: a paragraph at the end of the Add a Zigbee device page */
    ui_paragraph(body, TR("Leaving this page stops listening."), ui_tone_gray());
}

/* A joined row's words: chip, detail, value, its second line. */
static void joined_words(const ui_zb_joined_t *j, size_t i, lv_obj_t *row, lv_obj_t *sub)
{
    ui_zb_device_page_t page;
    const bool have = read_device(j->id, &page);
    char detail[96];
    const char *glyph = UI_ICON_CIRCLE_DOT;
    lv_color_t tone = ui_tone_blue();
    lv_color_t detail_color = ui_tone_blue();
    const char *value = "";
    const char *said = "";
    if (j->step == UI_ZB_STEP_DONE) {
        switch (j->result) {
        case UI_ZB_RES_ADDED:
            glyph = UI_ICON_CIRCLE_CHECK;
            tone = ui_tone_green();
            detail_color = ui_tone_green();
            /* TR: a joined device's line on the add page, before " · " and how it joined */
            snprintf(detail, sizeof(detail), "%s", TR_C("Zigbee add step", "Ready"));
            join(detail, sizeof(detail), have ? joined_how(page.dev.join) : "");
            /* TR: a joined device's value on the add page: tap it to put it to work. At most
             * 23 bytes */
            value = TR("Use it");
            break;
        case UI_ZB_RES_ALREADY_ADDED: {
            char name[48] = "";
            if (have) {
                device_name(&page.dev, name, sizeof(name));
            }
            glyph = UI_ICON_INFO;
            detail_color = COLOR_TEXT_2;
            /* TR: a joined device's line on the add page; {0} is its name. At most 95 bytes */
            I18N_FORMAT(detail, sizeof(detail), TR("Already added: it's {0}"), name);
            break;
        }
        case UI_ZB_RES_NOT_A_SENSOR:
            glyph = UI_ICON_PLUG;
            tone = ui_tone_gray();
            detail_color = COLOR_TEXT_2;
            i18n_utf8_copy(detail, sizeof(detail),
                           (s_add.kept >> i & 1u) != 0u
                               /* TR: a joined device's line on the add page */
                               ? TR("Not a sensor: kept, it extends the network")
                               /* TR: a joined device's line on the add page */
                               : TR("Not a sensor"));
            break;
        default:
            glyph = UI_ICON_TRIANGLE_ALERT;
            tone = ui_tone_amber();
            detail_color = ui_tone_amber();
            /* TR: a joined device's line on the add page */
            i18n_utf8_copy(detail, sizeof(detail), TR("Setup not finished"));
            break;
        }
    } else {
        const unsigned n = step_number(j->step);
        /* TR: a joined device's line on the add page: {0} its step's name, {1} of {2} steps */
        I18N_FORMAT(detail, sizeof(detail), TR("{0} · step {1} of {2}"),
                    TR_C("Zigbee add step", STEP_NAMES[n - 1u]), n, (unsigned)ADD_STEPS);
        if (j->asleep) {
            glyph = have ? device_glyph(&page.dev) : UI_ICON_CIRCLE_DOT;
            tone = ui_tone_amber();
            detail_color = COLOR_TEXT_2;
            /* TR: a joined device's second line on the add page, in amber */
            said = TR("It went to sleep before it finished");
        }
    }
    set_chip(row, glyph, tone);
    set_words(row, "detail_label", detail, detail_color);
    set_words(row, "value_label", value, COLOR_TEXT_2);
    set_sub(sub, said, ui_tone_amber());
}

static void render_joined(const ui_zb_progress_t *p, lv_obj_t *body, bool ended)
{
    bool asleep = false;
    for (size_t i = 0; i < p->joined_count && i < UI_ZB_JOINED_MAX; i++) {
        asleep = asleep || p->joined[i].asleep;
    }
    if (!asleep && !ended) {
        char text[128];
        /* TR: a paragraph on the add page; {0} is how many devices joined */
        I18N_FORMAT(text, sizeof(text),
                    TRN("{0} device joined. More can join until the time runs out.",
                        "{0} devices joined. More can join until the time runs out.",
                        p->joined_count),
                    (unsigned)p->joined_count);
        ui_paragraph(body, text, ui_tone_gray());
    }
    /* TR: a section heading on the add page: the devices that joined */
    ui_section(body, TR("Joined"));
    uint16_t asleep_id = 0u;
    for (size_t i = 0; i < p->joined_count && i < UI_ZB_JOINED_MAX; i++) {
        const ui_zb_joined_t *j = &p->joined[i];
        const bool ready = j->step == UI_ZB_STEP_DONE && j->result == UI_ZB_RES_ADDED;
        const bool known = j->step == UI_ZB_STEP_DONE && j->result != UI_ZB_RES_ADDED;
        /* from the zone picker, a ready device goes back to it: its inputs are offered there */
        lv_obj_t *row = ui_row_link(body, UI_ICON_CIRCLE_DOT, ui_tone_blue(), j->model, "-", "",
                                    ready || known ? UI_ICON_CHEVRON_RIGHT : NULL,
                                    ready     ? (s_add.arg == 1 ? add_done : open_added)
                                    : known   ? open_device
                                              : NULL,
                                    (void *)(uintptr_t)j->id);
        s_add.row[i] = row;
        s_add.sub[i] = sub_line(row);
        joined_words(j, i, row, s_add.sub[i]);
        if (j->asleep && asleep_id == 0u) {
            asleep_id = j->id;
        }
    }
    if (asleep_id != 0u) {
        ui_zb_device_page_t page;
        char text[160];
        if (read_device(asleep_id, &page) && page.wake_words[0] != '\0') {
            char words[UI_ZB_WORDS_LEN + 8];
            i18n_utf8_copy(words, sizeof(words), TR_C("wake up", page.wake_words));
            if (words[0] >= 'A' && words[0] <= 'Z') {
                words[0] = (char)(words[0] - 'A' + 'a'); /* it goes on after the colon */
            }
            /* TR: a paragraph on the add page; {0} is the model table's words for waking it,
             * starting with a small letter */
            I18N_FORMAT(text, sizeof(text), TR("Wake it: {0}."), words);
        } else {
            /* TR: a paragraph on the add page when a joined device fell asleep */
            i18n_utf8_copy(text, sizeof(text),
                           TR("Wake it: press its button once, then again every few seconds "
                              "until it says Ready."));
        }
        ui_paragraph(body, text, ui_tone_amber());
    }
    if (s_add.arg == 1) {
        /* TR: a paragraph on the add page opened from the zone's sensor picker */
        ui_paragraph(body, TR("Tap a ready device to pick its input."), ui_tone_gray());
    }
    if (ended) {
        ui_row_action(body, UI_ICON_CHECK, TR("Done"), ui_tone_blue(), add_done, NULL);
    } else {
        listen_buttons(body);
    }
}

/* The add page's ending boxes: a status row, a paragraph. */
static void status_box(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                       const char *detail, const char *words)
{
    ui_row_link(body, glyph, tone, title, detail, NULL, NULL, NULL, NULL);
    ui_paragraph(body, words, ui_tone_gray());
}

static uint16_t s_plug_id; /* the device the "Not a sensor" box is about */

static void plug_keep(void *ctx)
{
    (void)ctx;
    s_add.kept |= 1u;
    ui_page_rebuild();
}

static void plug_remove_go(void)
{
    const uint16_t id = s_plug_id;
    ui_page_back(); /* leaving stops listening, so the radio is free to ask it */
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_REMOVE, id);
    if (request(&req)) {
        /* TR: a toast after asking a device to leave (what follows isn't on this page) */
        ui_toast(TR("Asked it to leave the network"));
    }
}

static void plug_remove(void *ctx)
{
    (void)ctx;
    char title[96];
    ui_zb_progress_t p;
    read_progress(&p);
    /* TR: a confirmation's title; {0} is a device's model. At most 95 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Remove {0}?"), p.joined_count > 0u ? p.joined[0].model : "");
    ui_confirm(title, TR("It is asked to leave the network."), TR_C("confirm button", "Remove"),
               plug_remove_go);
}

static void render_ending(const ui_zb_progress_t *p, add_shape_t shape, lv_obj_t *body, char *title,
                          size_t tlen)
{
    char detail[96];
    char text[192];
    switch (shape) {
    case AS_ALREADY: {
        const ui_zb_joined_t *j = &p->joined[0];
        ui_zb_device_page_t page;
        char name[48] = "";
        if (read_device(j->id, &page)) {
            device_name(&page.dev, name, sizeof(name));
        }
        /* TR: a box's title on the add page when the device joined before */
        I18N_FORMAT(text, sizeof(text), TR("This device is already in the network: it's {0}."), name);
        status_box(body, UI_ICON_INFO, ui_tone_blue(), TR("Already added"), j->model, text);
        ui_row_action(body, UI_ICON_CHEVRON_RIGHT, TR("Open it"), ui_tone_blue(), add_open_device,
                      (void *)(uintptr_t)j->id);
        break;
    }
    case AS_NOT_SENSOR: {
        const ui_zb_joined_t *j = &p->joined[0];
        ui_zb_device_page_t page;
        const bool have = read_device(j->id, &page);
        char power[40] = "";
        if (have) {
            power_fact(&page.dev, power, sizeof(power));
        }
        snprintf(detail, sizeof(detail), "%s", j->model);
        join(detail, sizeof(detail), power);
        s_plug_id = j->id;
        status_box(body, UI_ICON_PLUG, ui_tone_amber(), TR("Not a sensor"), detail,
                   have && page.dev.router
                       /* TR: a paragraph on the add page for a device that isn't a sensor */
                       ? TR("It has nothing the panel can use as a zone. Kept, it still extends "
                            "the network for battery sensors.")
                       /* TR: a paragraph on the add page for a device that isn't a sensor */
                       : TR("It has nothing the panel can use as a zone."));
        ui_action_pair(body, UI_ICON_CHECK, TR("Keep it"), ui_tone_blue(), plug_keep, UI_ICON_TRASH,
                       TR("Remove it"), ui_tone_red(), plug_remove, NULL);
        break;
    }
    case AS_NOTHING:
        status_box(body, UI_ICON_CIRCLE_DOT, ui_tone_gray(), TR("Nothing joined"),
                   p->result == UI_ZB_RES_STOPPED
                       /* TR: under "Nothing joined" on the add page */
                       ? TR("Listening was stopped")
                       /* TR: under "Nothing joined" on the add page */
                       : TR("The 3 minutes ran out"),
                   s_add.arg == 2
                       /* TR: a paragraph on the Replace page when nothing joined */
                       ? TR("Nothing was replaced. Put the new device in pairing mode, then try "
                            "again.")
                       /* TR: a paragraph on the add page when nothing joined */
                       : TR("Check the device's pairing steps in its manual: some need a reset "
                            "first. Then try again."));
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    case AS_DIDNT_JOIN:
        status_box(body, UI_ICON_CIRCLE_X, ui_tone_red(), TR("Didn't join"),
                   /* TR: under "Didn't join" on the add page */
                   TR("It started joining but didn't finish"),
                   /* TR: a paragraph on the add page when a device didn't finish joining */
                   TR("Reset the device (see its manual), keep it close to the panel and try "
                      "again."));
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    case AS_NOT_FINISHED: {
        const ui_zb_joined_t *j = NULL;
        for (size_t i = 0; i < p->joined_count && i < UI_ZB_JOINED_MAX && j == NULL; i++) {
            if (p->joined[i].step != UI_ZB_STEP_DONE ||
                p->joined[i].result == UI_ZB_RES_SETUP_NOT_FINISHED) {
                j = &p->joined[i];
            }
        }
        const char *model = j != NULL ? j->model : p->model;
        /* ZB-012: a setup that stopped (its row DONE, as zbz_panel ends it) never goes on by
         * itself - only "Set it up again" does - so it has no step to give, and no wake to wait
         * for. One still running (asleep) finishes when it next wakes. */
        const bool stopped = j == NULL || j->step == UI_ZB_STEP_DONE;
        if (!stopped) {
            I18N_FORMAT(detail, sizeof(detail), TR("{0} · step {1} of {2}"), model,
                        step_number(j->step), (unsigned)ADD_STEPS);
        } else if (model[0] != '\0') {
            i18n_utf8_copy(detail, sizeof(detail), model);
        } else {
            /* TR: under "Setup not finished" on the add page when the model isn't known */
            i18n_utf8_copy(detail, sizeof(detail), TR("Setup stopped"));
        }
        status_box(body, UI_ICON_TRIANGLE_ALERT, ui_tone_amber(), TR("Setup not finished"), detail,
                   stopped
                       /* TR: a paragraph on the add page when a device's setup stopped before it
                        * finished. At most 127 bytes */
                       ? TR("It joined, but its setup stopped before it finished. Open it and set "
                            "it up again (wake it first if it sleeps).")
                       /* TR: a paragraph on the add page when a device's setup didn't finish */
                       : TR("It joined, but the panel hasn't finished reading what it can do, so "
                            "it can't be used yet. It finishes when it next wakes."));
        ui_action_pair(body, UI_ICON_CHEVRON_RIGHT, TR("Open it"), ui_tone_blue(), add_open_device,
                       UI_ICON_CHECK, TR("Done"), ui_tone_gray(), add_done,
                       (void *)(uintptr_t)(j != NULL ? j->id : p->id));
        break;
    }
    case AS_FULL:
        status_box(body, UI_ICON_TRIANGLE_ALERT, ui_tone_amber(), TR("No room for it"),
                   /* TR: under "No room for it" on the add page */
                   TR("It was asked to leave the network"),
                   /* TR: a paragraph on the add page when the panel can keep no more devices */
                   TR("The panel keeps up to 64 Zigbee devices, and its storage must have room. "
                      "Remove one you no longer use, then try again."));
        ui_row_action(body, UI_ICON_CHECK, TR("Done"), ui_tone_blue(), add_done, NULL);
        break;
    case AS_REPLACED: {
        /* TR: the title of the Replace page's ending */
        i18n_utf8_copy(title, tlen, TR("Replaced"));
        /* TR: a box's title on the Replace page's ending; {0} is the old device's name */
        I18N_FORMAT(detail, sizeof(detail), TR("Replaced: {0}"), s_replace_name);
        status_box(body, UI_ICON_CIRCLE_CHECK, ui_tone_green(), detail, p->model,
                   /* TR: a paragraph on the Replace page's ending */
                   TR("The new device took its zones: nothing else to set up."));
        ui_action_pair(body, UI_ICON_CHEVRON_RIGHT, TR("Open it"), ui_tone_blue(), add_open_device,
                       UI_ICON_CHECK, TR("Done"), ui_tone_gray(), add_done,
                       (void *)(uintptr_t)p->id);
        break;
    }
    default: /* AS_FAILED or an unexpected ending: never leave a dead page */
        status_box(body, UI_ICON_CIRCLE_X, ui_tone_red(), TR("Couldn't add it"),
                   /* TR: under "Couldn't add it" on the add page */
                   TR("The radio couldn't do it"), TR("Something went wrong. Try again."));
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    }
}

static void add_title(char *title, size_t tlen)
{
    if (s_add.arg == 3) {
        i18n_utf8_copy(title, tlen, TR("Add with an install code"));
    } else if (s_add.arg == 2) {
        /* TR: the add page's title when a new device replaces one; {0} is the old one's name.
         * At most 47 bytes */
        I18N_FORMAT(title, tlen, TR("Replace {0}"), s_replace_name);
    } else {
        /* TR: The page title. At most 47 bytes. */
        i18n_utf8_copy(title, tlen, TR("Add a Zigbee device"));
    }
}

static void build_add(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                      size_t nlen)
{
    s_add.open = false;
    memset(s_add.row, 0, sizeof(s_add.row));
    memset(s_add.sub, 0, sizeof(s_add.sub));
    if (S.page_fresh) {
        s_add.arg = arg;
    }
    add_title(title, tlen);
    ui_zb_status_t st;
    if (!read_status(&st)) {
        ui_paragraph(body, TR("Zigbee isn't available on this panel."), ui_tone_gray());
        return;
    }
    if (!installer()) {
        /* TR: a paragraph on the Add a device page, for someone who is not in installer mode */
        ui_paragraph(body, TR("Adding a device is done in installer mode."), ui_tone_gray());
        return;
    }
    ui_zb_progress_t p;
    read_progress(&p);
    const bool ours = adding_op(p.op);
    const bool running = ours && p.step != UI_ZB_STEP_DONE && p.step != UI_ZB_STEP_IDLE;
    const bool this_visit = ours && s_owner == PAGE_ZIGBEE_ADD;
    if (!running && !this_visit && !S.page_fresh) {
        /* returned to (back from a page it opened), or its add dismissed elsewhere: the radio
         * never starts listening by itself */
        s_owner = 0;
        ui_paragraph(body, TR("Nothing is being added now."), ui_tone_gray());
        ui_row_action(body, UI_ICON_PLUS, TR("Add a device"), ui_tone_blue(), add_again, NULL);
        return;
    }
    if (!running && !this_visit) {
        s_add.kept = 0u;
        if (ours || p.op != UI_ZB_OP_NONE) {
            if (p.step == UI_ZB_STEP_DONE) {
                ask_quietly(UI_ZB_REQ_DISMISS); /* an ended one from before is nobody's to read */
            }
        }
        ui_zb_request_t req;
        add_fill(&req);
        const ui_zb_req_result_t r = ask(&req);
        if (r != UI_ZB_REQ_OK) {
            s_owner = 0;
            ui_paragraph(body, not_started_words(r), ui_tone_amber());
            ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
            return;
        }
        read_progress(&p);
    }
    s_owner = PAGE_ZIGBEE_ADD;
    add_note(&p, note, nlen);
    const add_shape_t shape = add_shape(&p);
    switch (shape) {
    case AS_NONE:
    case AS_LISTEN: render_listen(body); break;
    case AS_JOINED: render_joined(&p, body, false); break;
    case AS_DONE_JOINED: render_joined(&p, body, true); break;
    default: render_ending(&p, shape, body, title, tlen); break;
    }
    s_add.sig = add_sig(&p);
    s_add.open = true;
    s_add.polled = lv_tick_get();
}

static void add_tick(void)
{
    if (!s_add.open || lv_tick_elaps(s_add.polled) < POLL_MS) {
        return;
    }
    s_add.polled = lv_tick_get();
    ui_zb_progress_t p;
    read_progress(&p);
    if (add_sig(&p) != s_add.sig) {
        ui_page_rebuild();
        return;
    }
    char note[64];
    add_note(&p, note, sizeof(note));
    ui_page_note(note);
    for (size_t i = 0; i < p.joined_count && i < UI_ZB_JOINED_MAX; i++) {
        if (s_add.row[i] != NULL) {
            joined_words(&p.joined[i], i, s_add.row[i], s_add.sub[i]);
        }
    }
}

/* ---- a device that joined: "Use it for" (PAGE_ZIGBEE_ADDED, the proposal's add-added) ---------- */

static void build_added(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen)
{
    const uint16_t id = (uint16_t)arg;
    /* TR: the title of the page for a device that just joined. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Added"));
    ui_zb_device_page_t page;
    if (!read_device(id, &page)) {
        ui_paragraph(body, TR("This device is no longer in the network."), ui_tone_gray());
        return;
    }
    const ui_zb_device_t *d = &page.dev;
    snprintf(note, nlen, "%s", d->model);
    join(note, nlen, joined_how(d->join));
    char line[96] = "";
    char fact[40];
    char added[96];
    power_fact(d, fact, sizeof(fact));
    join(line, sizeof(line), fact);
    join(line, sizeof(line), signal_fact(d->signal));
    if (page.input_count > 0u && page.inputs[0].state[0] != '\0') {
        join(line, sizeof(line), ui_sensor_state_words(page.inputs[0].state));
    }
    /* TR: the box at the top of the Added page; {0} is the device's model. At most 95 bytes */
    I18N_FORMAT(added, sizeof(added), TR("Added: {0}"), d->model);
    ui_row_link(body, UI_ICON_CIRCLE_CHECK, ui_tone_green(), added, line, NULL, NULL, NULL, NULL);
    if (is_role(d)) {
        /* its page leads with what it does (bl-147-siren, bl-148-keypad, bl-148-fob) */
        ui_paragraph(body,
                     /* TR: a paragraph on the Added page for a siren, keypad or key fob */
                     TR("It feeds no zone: what it does is set on its page."), ui_tone_gray());
        ui_row_action(body, UI_ICON_CHEVRON_RIGHT, TR("Open it"), ui_tone_blue(), add_open_device,
                      (void *)(uintptr_t)id);
        return;
    }
    if (d->state != UI_ZB_DEV_READY) {
        ui_paragraph(body,
                     /* TR: a paragraph on the Added page while its setup still runs */
                     TR("Its setup isn't finished yet: wait until it says Ready."), ui_tone_amber());
        return;
    }
    if (!installer()) {
        ui_paragraph(body, TR("Adding a device is done in installer mode."), ui_tone_gray());
        return;
    }
    /* TR: a section heading on the Added page */
    ui_section(body, TR("Use it for"));
    if (render_use_it_for(body, id) > 0u) {
        ui_paragraph(body, TR("Tamper and low battery come with the zone: nothing else to set up."),
                     ui_tone_gray());
    } else {
        char zones[32];
        char text[96];
        zones_words(d->zones, d->zone_count, zones, sizeof(zones));
        /* TR: a paragraph on the Added page when its inputs all feed zones; {0} "Zone 3" */
        I18N_FORMAT(text, sizeof(text), TR("In use: {0}."), zones);
        ui_paragraph(body, text, ui_tone_gray());
    }
    ui_row_action(body, G_X, TR("Not now: keep it without a zone"), ui_tone_gray(), add_done, NULL);
}

/* ---- a device's page (PAGE_ZIGBEE_DEVICE, arg = its registry id; the proposal's page 5) ------- */

static struct {
    uint16_t id;
    bool open;
    uint32_t sig;         /* dev_sig() as built: the page is made again when it changes */
    lv_obj_t *heard;      /* "Last heard": its value in place each second */
    lv_obj_t *signal;     /* "Signal" */
    lv_obj_t *power;      /* "Power" */
    lv_obj_t *setup;      /* "Setting it up: step 3 of 11" */
    lv_obj_t *test;       /* a siren's "Test it": its last result in place */
    uint8_t test_shown;   /* what that row says (test_state()) */
    uint8_t op;           /* the operation this page asked for (ping, identify...), NONE none */
    uint32_t polled;
} s_dev;

/* What the page's rows depend on (not the live words). */
static uint32_t dev_sig(const ui_zb_device_page_t *pg)
{
    uint32_t h = 2166136261u;
    const ui_zb_device_t *d = &pg->dev;
    const uint32_t parts[] = {d->unreachable ? 1u : 0u, d->state, d->kind, d->zone_count,
                              d->join, d->router ? 1u : 0u, pg->input_count,
                              pg->can_identify ? 1u : 0u, installer() ? 1u : 0u};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        h = (h ^ parts[i]) * 16777619u;
    }
    /* a siren's, keypad's or key fob's settings: its rows show them (made again on a change) */
    const ui_zb_siren_t *sn = &pg->siren;
    const ui_zb_keypad_t *kp = &pg->keypad;
    const ui_zb_fob_t *fb = &pg->fob;
    const uint32_t roles[] = {sn->fit, sn->partitions,
                              (sn->burglary ? 1u : 0u) | (sn->fire ? 2u : 0u) |
                                  (sn->strobe ? 4u : 0u) | (sn->squawk ? 8u : 0u),
                              sn->volume, sn->supervision_min, kp->fit, kp->partition,
                              kp->panic_mask, kp->show_state ? 1u : 0u, kp->device, fb->fit,
                              fb->user, fb->partition,
                              (fb->may_disarm ? 1u : 0u) | (fb->panic ? 2u : 0u)};
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        h = (h ^ roles[i]) * 16777619u;
    }
    for (size_t i = 0; i < d->zone_count && i < UI_ZB_ZONES_MAX; i++) {
        h = (h ^ d->zones[i]) * 16777619u;
    }
    for (size_t i = 0; i < pg->input_count && i < UI_ZB_INPUTS_MAX; i++) {
        h = (h ^ pg->inputs[i].zone) * 16777619u;
    }
    for (const char *c = d->name; *c != '\0'; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    return h;
}

static void dev_power_words(const ui_zb_device_t *d, char *out, size_t len)
{
    out[0] = '\0';
    if (d->power == UI_ZB_POWER_MAINS) {
        i18n_utf8_copy(out, len, TR_C("power", "Mains"));
    } else if (d->battery_pct != UI_ZB_BATTERY_UNKNOWN) {
        I18N_FORMAT(out, len, TR("Battery {0} %"), (unsigned)d->battery_pct);
        if (d->battery_low) {
            join(out, len, TR("Battery low"));
        }
    } else if (d->power == UI_ZB_POWER_BATTERY) {
        /* TR: a Zigbee device page's Power row before its battery reported. At most 39 bytes */
        i18n_utf8_copy(out, len, TR("Battery, not reported yet"));
    } else {
        i18n_utf8_copy(out, len, TR_C("signal", "Not known yet"));
    }
    if (d->router) {
        /* TR: a fact on a Zigbee device page's Power row: it routes for others */
        join(out, len, TR("Extends the network"));
    }
}

static const char *how_joined_value(uint8_t join_how)
{
    switch (join_how) {
    /* TR: a Zigbee device page's "How it joined" row */
    case UI_ZB_JOIN_INSTALL_CODE: return TR("With its install code");
    /* TR: a Zigbee device page's "How it joined" row */
    case UI_ZB_JOIN_STANDARD_KEY: return TR("With the standard key");
    default: return TR_C("signal", "Not known yet");
    }
}

static void rename_typed(const char *text)
{
    if (!s_dev.open || !installer()) {
        return;
    }
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_RENAME, s_dev.id);
    i18n_utf8_copy(req.name, sizeof(req.name), text);
    if (request(&req)) {
        ui_page_rebuild();
    }
}

static void rename_tap(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    char name[48];
    device_name(&page.dev, name, sizeof(name));
    /* TR: the rename sheet's note on a Zigbee device's page */
    ui_text_sheet(TR("Name"), TR("Empty: named after its zone, else its model"), name, false,
                  UI_ZB_NAME_LEN - 1u, rename_typed);
}

static void device_ask(uint8_t kind)
{
    ui_zb_request_t req;
    req_init(&req, kind, s_dev.id);
    if (request(&req)) {
        s_dev.op = kind == UI_ZB_REQ_PING       ? UI_ZB_OP_PING
                   : kind == UI_ZB_REQ_IDENTIFY ? UI_ZB_OP_IDENTIFY
                   : kind == UI_ZB_REQ_SETUP    ? UI_ZB_OP_SETUP
                   : kind == UI_ZB_REQ_FORGET   ? UI_ZB_OP_FORGET
                                                : UI_ZB_OP_NONE;
    }
}

static void ping_go(void *ctx)
{
    (void)ctx;
    device_ask(UI_ZB_REQ_PING); /* its answer: dev_tick's toast */
}

static void identify_go(void *ctx)
{
    (void)ctx;
    device_ask(UI_ZB_REQ_IDENTIFY);
}

static void setup_go(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    const bool sleepy = read_device(s_dev.id, &page) && page.sleepy;
    device_ask(UI_ZB_REQ_SETUP);
    if (s_dev.op == UI_ZB_OP_SETUP) {
        if (sleepy) {
            ui_toast(TR("Press its button to wake it")); /* the next step, not seen otherwise */
        }
        ui_page_rebuild(); /* "Setting it up" at once */
    }
}

static void forget_go(void)
{
    device_ask(UI_ZB_REQ_FORGET);
}

static void forget_ask(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    char name[48];
    char title[96];
    device_name(&page.dev, name, sizeof(name));
    /* TR: a confirmation's title; {0} is a device's name. At most 95 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Forget {0}?"), name);
    /* TR: the Forget confirmation's first sentence. At most 95 bytes */
    const char *first = TR("Only when it's gone or broken: it isn't asked to leave.");
    static char text[UI_SHEET_NOTE_LENGTH];
    if (page.dev.zone_count == 0u) {
        i18n_utf8_copy(text, sizeof(text), first);
    } else {
        char zones[32];
        char second[96];
        zones_words(page.dev.zones, page.dev.zone_count, zones, sizeof(zones));
        /* TR: the Forget confirmation's second sentence; {0} is "Zone 8" or "Zones 6, 9". At
         * most 95 bytes */
        I18N_FORMAT(second, sizeof(second),
                    TRN("{0} keeps its settings and needs a new sensor.",
                        "{0} keep their settings and need a new sensor.", page.dev.zone_count),
                    zones);
        /* TR: joins two sentences on a Zigbee device's ending page. Languages that write no
         * space between sentences (Chinese, Japanese) drop it */
        I18N_FORMAT(text, sizeof(text), TR_C("two sentences", "{0} {1}"), first, second);
    }
    ui_confirm(title, text, TR("Forget it"), forget_go);
}

static void replace_go(void)
{
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    s_replace_id = s_dev.id;
    device_name(&page.dev, s_replace_name, sizeof(s_replace_name));
    ui_page_open(PAGE_ZIGBEE_ADD, 2);
}

static void replace_ask(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Replace it with a new device?"),
               /* TR: the Replace confirmation's text on a Zigbee device's page */
               TR("Have the new device ready. It takes this one's place, and its zones follow it."),
               TR("Replace it"), replace_go);
}

static void open_zone_row(void *ctx)
{
    ui_page_open(PAGE_ZONE, (int32_t)(intptr_t)ctx);
}

/* "Used by": its zones, or "Make it a zone", or its role (a siren, a keypad, a key fob). */
static void used_by_rows(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    const ui_zb_device_t *d = &pg->dev;
    if (is_role(d)) {
        return; /* it feeds no zone: its role's sections say what it does (role_rows) */
    }
    /* TR: a section heading on a Zigbee device's page: what uses it */
    ui_section(body, TR_C("Zigbee device section", "Used by"));
    if (d->zone_count > 0u) {
        for (uint8_t i = 0; i < d->zone_count && i < UI_ZB_ZONES_MAX; i++) {
            const uint16_t z = d->zones[i];
            char zname[ALARM_NAME_LEN + 8];
            char zdefault[48];
            char ztitle[sizeof(zdefault) + sizeof(zname) + 8]; /* "Zone 1 · Front Door" */
            ui_zone_name(zone_cfg(), z, zname, sizeof(zname));
            I18N_FORMAT(zdefault, sizeof(zdefault), TR_C("default zone name", "Zone {0}"),
                        (unsigned)z);
            if (strcmp(zname, zdefault) == 0) { /* still its default name: don't say it twice */
                i18n_utf8_copy(ztitle, sizeof(ztitle), zname);
            } else {
                snprintf(ztitle, sizeof(ztitle), "%s · %s", zdefault, zname);
            }
            const alarm_config_t *cfg = zone_cfg();
            const bool used = cfg != NULL && z >= 1u && z <= ALARM_MAX_ZONES &&
                              cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED;
            lv_obj_t *row = ui_row_link(
                body, device_glyph(d), d->unreachable ? ui_tone_amber() : ui_tone_green(), ztitle,
                d->unreachable
                    /* TR: under a zone's row on a Zigbee device's page while it isn't heard */
                    ? TR("Shows CHECK")
                    : used ? ui_zone_type_name(cfg->zones[z - 1u].type) : "",
                NULL, used ? UI_ICON_CHEVRON_RIGHT : NULL, used ? open_zone_row : NULL,
                (void *)(intptr_t)(int32_t)z);
            if (d->unreachable) {
                ui_row_set_detail(row, TR("Shows CHECK"), true, ui_tone_amber());
            }
        }
        return;
    }
    if (d->state == UI_ZB_DEV_READY && pg->input_count > 0u) {
        const alarm_device_class_t cls = (alarm_device_class_t)pg->inputs[0].device_class;
        char detail[64];
        /* TR: under "Make it a zone": {0} what it senses ("Motion"), {1} the zone type a new zone
         * gets ("Interior follower"). At most 63 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR_C("class and zone type", "{0}: {1}"),
                    ui_device_class_name(cls), ui_zone_type_name(zsrc_suggest_zone_type(cls)));
        if (installer()) {
            ui_row_link(body, UI_ICON_PLUS, ui_tone_green(), TR("Make it a zone"),
                        pg->input_count == 1u ? detail : NULL, NULL, UI_ICON_CHEVRON_RIGHT,
                        make_it_a_zone, (void *)(uintptr_t)d->id);
        } else {
            later_row(body, UI_ICON_PLUS, ui_tone_gray(), TR("Not in a zone"), NULL, NULL);
        }
    } else if (d->state == UI_ZB_DEV_NOT_USABLE) {
        later_row(body, UI_ICON_PLUG, ui_tone_gray(), TR("Not a sensor"),
                  /* TR: under "Not a sensor" on a Zigbee device's page */
                  TR("Nothing on it the panel can use as a zone"), NULL);
    } else {
        /* nothing to offer until its setup has read what it can do */
        later_row(body, UI_ICON_PLUS, ui_tone_gray(), TR("Make it a zone"),
                  TR("Finish its setup first"), NULL);
    }
}

/* "This device": its facts. `missing`: Last heard and Signal were said at the top already. */
static void fact_rows(lv_obj_t *body, const ui_zb_device_page_t *pg, bool missing)
{
    const ui_zb_device_t *d = &pg->dev;
    char name[48];
    char text[96];
    device_name(d, name, sizeof(name));
    /* TR: a section heading on a Zigbee device's page: its facts */
    ui_section(body, TR_C("Zigbee device section", "This device"));
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL, name,
                installer() ? UI_ICON_CHEVRON_RIGHT : NULL, installer() ? rename_tap : NULL, NULL);
    if (pg->model_id[0] != '\0') {
        snprintf(text, sizeof(text), "%s%s%s", pg->manufacturer,
                 pg->manufacturer[0] != '\0' ? " " : "", pg->model_id);
    } else {
        snprintf(text, sizeof(text), "%s", d->model);
    }
    /* TR: a row on a Zigbee device's page */
    ui_row_value(body, TR_C("Zigbee device", "Model"), text);
    /* TR: a row on a Zigbee device's page */
    ui_row_value(body, TR("What it is"), kind_words(d));
    dev_power_words(d, text, sizeof(text));
    /* TR: a row on a Zigbee device's page */
    s_dev.power = ui_row_value(body, TR_C("Zigbee device", "Power"), text);
    if (!missing) {
        s_dev.signal = ui_row_value(body, TR_C("Zigbee device", "Signal"),
                                    signal_value(d->signal, false));
        ago_words(d->heard_s, text, sizeof(text));
        s_dev.heard = ui_row_value(body, TR("Last heard"), text);
    }
    /* TR: a row on a Zigbee device's page */
    ui_row_value(body, TR("How it joined"), how_joined_value(d->join));
    /* TR: a row on a Zigbee device's page: its own software's version */
    ui_row_value(body, TR_C("Zigbee device", "Software"),
                 pg->sw_build[0] != '\0' ? pg->sw_build : TR_C("signal", "Not known yet"));
}

/* heading: false when a siren's own "Check it" (Test it, Supervision) opened the section */
static void check_rows(lv_obj_t *body, const ui_zb_device_page_t *pg, bool heading)
{
    if (!installer()) {
        return;
    }
    if (heading) {
        /* TR: a section heading on a Zigbee device's page */
        ui_section(body, TR_C("Zigbee device section", "Check it"));
    }
    if (pg->can_identify && !pg->dev.unreachable) {
        /* its own light, for a few seconds: the panel's screen never blinks */
        ui_row_link(body, G_LIGHTBULB, ui_tone_blue(), TR("Blink its light"),
                    TR("Find which device this is"), NULL, NULL, identify_go, NULL);
    }
    ui_row_link(body, UI_ICON_ACTIVITY, ui_tone_blue(), TR("Check it now"),
                /* TR: under "Check it now" on a Zigbee device's page */
                TR("Asks it to answer"), NULL, NULL, ping_go, NULL);
    ui_row_link(body, UI_ICON_REFRESH, ui_tone_blue(), TR("Set it up again"),
                /* TR: under "Set it up again" on a Zigbee device's page */
                TR("Reads what it can do and how it reports"), NULL, NULL, setup_go, NULL);
}

static void remove_this(void *ctx);

/* role: a siren, keypad or key fob, which feeds no zone - nothing for a new device to take over
 * (a replaced device loses its role: alarm-devices.md §4), so it has no "Replace it". */
static void remove_rows(lv_obj_t *body, bool role)
{
    if (!installer()) {
        return;
    }
    ui_section(body, TR("Removing it"));
    if (!role) {
        ui_row_link(body, UI_ICON_ARROW_LEFT_RIGHT, ui_tone_blue(), TR("Replace it"),
                    /* TR: under "Replace it" on a Zigbee device's page */
                    TR("A new device takes its zones"), NULL, UI_ICON_CHEVRON_RIGHT, replace_ask,
                    NULL);
    }
    lv_obj_t *rm = ui_row_link(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove this device"),
                               TR("Asks it to leave the network"), NULL, UI_ICON_CHEVRON_RIGHT,
                               remove_this, NULL);
    set_words(rm, "title_label", TR("Remove this device"), ui_tone_red());
    lv_obj_t *fg = ui_row_link(body, UI_ICON_UNLINK, ui_tone_red(),
                               /* TR: a row on a Zigbee device's page */
                               TR("Forget it: it's gone or broken"),
                               /* TR: under "Forget it" on a Zigbee device's page */
                               TR("Removed without its answer"), NULL, UI_ICON_CHEVRON_RIGHT,
                               forget_ask, NULL);
    set_words(fg, "title_label", TR("Forget it: it's gone or broken"), ui_tone_red());
}

/* The box at the top: the one thing to do, or where its setup is. */
static void top_paragraph(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    const ui_zb_device_t *d = &pg->dev;
    char text[192];
    if (d->unreachable) {
        char span[24];
        span_words(d->heard_s, span, sizeof(span));
        if (d->heard_s == UI_ZB_NEVER) {
            /* TR: the box at the top of a Zigbee device's page when it isn't heard */
            i18n_utf8_copy(text, sizeof(text),
                           TR("Not heard since the panel started. Check its power or battery, "
                              "or bring it closer."));
        } else if (d->power == UI_ZB_POWER_MAINS) {
            /* TR: the box at the top of a mains Zigbee device's page; {0} "2 h 30 min" */
            I18N_FORMAT(text, sizeof(text),
                        TR("Not heard for {0}. Check its power, or bring it closer."), span);
        } else {
            /* TR: the box at the top of a battery Zigbee device's page; {0} "2 h 30 min" */
            I18N_FORMAT(text, sizeof(text),
                        TR("Not heard for {0}. Change its battery or bring it closer, then press "
                           "its button to wake it."),
                        span);
        }
        ui_paragraph(body, text, ui_tone_amber());
        return;
    }
    switch (d->state) {
    case UI_ZB_DEV_WAKE_NEEDED: {
        char words[UI_ZB_WORDS_LEN + 8];
        if (pg->wake_words[0] != '\0') {
            i18n_utf8_copy(words, sizeof(words), TR_C("wake up", pg->wake_words));
            if (words[0] >= 'A' && words[0] <= 'Z') {
                words[0] = (char)(words[0] - 'A' + 'a'); /* it goes on after the colon */
            }
        } else {
            i18n_utf8_copy(words, sizeof(words), TR("see its manual, usually by pressing its button"));
        }
        /* TR: a paragraph on a device's page; {0} is what to do to wake it. At most 130 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Wake it to finish setting it up: {0}."), words);
        ui_paragraph(body, text, ui_tone_amber());
        break;
    }
    case UI_ZB_DEV_SETTING_UP:
    case UI_ZB_DEV_JOINING:
        /* TR: a paragraph on a device's page while its setup runs; {0} of {1} steps done.
         * At most 63 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Setting it up: step {0} of {1}."),
                    (unsigned)d->setup_done, (unsigned)d->setup_total);
        s_dev.setup = ui_paragraph(body, text, ui_tone_blue());
        break;
    case UI_ZB_DEV_SETUP_STOPPED:
        ui_paragraph(body,
                     /* TR: a paragraph on a Zigbee device's page when its setup stopped */
                     TR("Its setup stopped before it finished, so it can't be used yet. Set it up "
                        "again below."),
                     ui_tone_amber());
        break;
    case UI_ZB_DEV_NOT_USABLE:
        ui_paragraph(body,
                     pg->note[0] != '\0' ? pg->note
                                         /* TR: a paragraph on a Zigbee device's page */
                                         : TR("It has nothing the panel can use as a zone."),
                     ui_tone_gray());
        break;
    default: break;
    }
}

/* ---- a siren's, keypad's and key fob's own sections (the proposal's pages 14-16; BL-147,
 * BL-148; ZB-023, ZB-024; alarm-devices.md §2-§4) ---------------------------------------------
 * They lead the device page of a siren, keypad or key fob; the device page's own rows follow.
 * A change is a request at once; the page is made again after the event that asked (a switch
 * can't delete itself), from the panel's answer: a refused change shows as it was, and only a
 * refusal gets words (no "Saved"). */

/* Why one of a role's rows is dimmed (role_why's ctx). */
typedef enum {
    WHY_INSTALLER = 0, /* outside installer mode */
    WHY_SETUP,         /* its setup must finish first */
    WHY_SIREN_OFF,     /* a siren the panel doesn't use yet */
    WHY_KEYPAD_OFF,    /* a keypad with no partition */
    WHY_FOB_OFF,       /* a key fob nobody owns */
    WHY_NO_SLOT,       /* no keypad number free */
    WHY_NOT_HEARD,     /* a siren that isn't heard can't be tested */
} role_why_t;

static const char *role_why_words(role_why_t why)
{
    switch (why) {
    /* TR: a toast, why a Zigbee siren's, keypad's or key fob's setting can't be changed yet */
    case WHY_SETUP: return TR("Finish its setup first");
    /* TR: a toast, why a Zigbee siren's setting can't be changed yet */
    case WHY_SIREN_OFF: return TR("Turn on what it sounds for first");
    /* TR: a toast, why a Zigbee keypad's setting can't be changed yet */
    case WHY_KEYPAD_OFF: return TR("Choose its partition first");
    /* TR: a toast, why a Zigbee key fob's setting can't be changed yet */
    case WHY_FOB_OFF: return TR("Choose whose key fob it is first");
    /* TR: a toast, why a Zigbee keypad can't be used */
    case WHY_NO_SLOT: return TR("No keypad number is free");
    /* TR: a toast, why a Zigbee siren that isn't heard can't be tested */
    case WHY_NOT_HEARD: return TR("It isn't heard: check its power first");
    default: return refused_words(UI_ZB_REQ_NOT_ALLOWED);
    }
}

static void role_why(void *ctx)
{
    ui_toast(role_why_words((role_why_t)(uintptr_t)ctx));
}

/* A role's choice row: tappable when it can be changed, else dimmed and saying why when tapped. */
static lv_obj_t *role_link(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                           const char *detail, const char *value, role_why_t why, bool can,
                           ui_click_fn fn, void *ctx)
{
    if (can) {
        return ui_row_link(body, glyph, tone, title, detail, value, UI_ICON_CHEVRON_RIGHT, fn, ctx);
    }
    lv_obj_t *row = ui_row_link(body, glyph, tone, title, detail, value, NULL, role_why,
                                (void *)(uintptr_t)why);
    lv_obj_set_style_opa(row, LV_OPA_40, 0);
    return row;
}

/* A role's switch: dimmed and still when it can't be changed (the section says why). */
static lv_obj_t *role_toggle(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                             const char *detail, bool on, bool can, ui_toggle_fn fn, void *ctx)
{
    lv_obj_t *row = ui_row_toggle_icon(body, glyph, tone, title, detail, on, can ? fn : NULL, ctx);
    if (!can) {
        lv_obj_t *sw = ui_find(row, "toggle");
        if (sw != NULL) {
            lv_obj_add_state(sw, LV_STATE_DISABLED);
        }
        lv_obj_set_style_opa(row, LV_OPA_40, 0);
    }
    return row;
}

/* Why a role's request was refused, in its own words where they differ from the radio's. */
static const char *role_refused(ui_zb_req_result_t r, uint8_t kind)
{
    switch (r) {
    case UI_ZB_REQ_FULL:
        if (kind == UI_ZB_REQ_KEYPAD) {
            /* TR: a toast: a Zigbee keypad can't be used, every keypad number 1-8 is taken */
            return TR("No keypad number is free: other keypads use 1 to 8");
        }
        /* TR: a toast: the panel uses as many Zigbee sirens as it can (4) */
        return TR("The panel already uses as many sirens as it can");
    case UI_ZB_REQ_IN_USE:
        /* TR: a toast: the chosen user has another key fob already */
        return TR("That user has a key fob already");
    case UI_ZB_REQ_INVALID:
        /* TR: a toast, why a Zigbee siren's, keypad's or key fob's setting wasn't kept */
        return TR("Not kept: the panel can't use that setting");
    default: return refused_words(r);
    }
}

static void role_rebuild_later(void *unused)
{
    (void)unused;
    if (s_dev.open) {
        ui_page_rebuild();
    }
}

/* Asks the panel to keep a role's settings; the page is made again from its answer. */
static void role_ask(ui_zb_request_t *req)
{
    const uint8_t kind = req->kind;
    const ui_zb_req_result_t r = ask(req);
    if (r != UI_ZB_REQ_OK) {
        ui_toast(role_refused(r, kind));
    }
    lv_async_call(role_rebuild_later, NULL);
}

/* The partitions this panel uses (bit 0 = partition 1); partition 1 when none is set up. */
static uint8_t parts_enabled(void)
{
    const alarm_config_t *cfg = zone_cfg();
    uint8_t mask = 0;
    for (unsigned i = 0; cfg != NULL && i < ALARM_MAX_PARTITIONS; i++) {
        if (cfg->partitions[i].enabled) {
            mask = (uint8_t)(mask | (1u << i));
        }
    }
    return mask != 0u ? mask : 1u;
}

static void part_name(uint8_t p, char *out, size_t len)
{
    const alarm_config_t *cfg = zone_cfg();
    if (cfg != NULL && p >= 1u && p <= ALARM_MAX_PARTITIONS &&
        cfg->partitions[p - 1u].name[0] != '\0') {
        snprintf(out, len, "%s", cfg->partitions[p - 1u].name);
    } else {
        /* TR: the name of a partition nobody has named; {0} is its number */
        I18N_FORMAT(out, len, TR_C("default partition name", "Partition {0}"), (unsigned)p);
    }
}

/* ---- the pick sheets of these sections ---- */

typedef enum {
    PICK_SIREN_ALSO = 100, /* OUT-015 (after the others: their values stay) */
    PICK_SIREN_PARTS = 0,
    PICK_VOLUME,
    PICK_SUPERVISION,
    PICK_KEYPAD_PART,
    PICK_BUTTON,
    PICK_FOB_USER,
    PICK_FOB_PART,
} role_pick_t;

#define ROLE_PICK_MAX (ALARM_MAX_USERS + 1u)

static struct {
    uint8_t what;   /* role_pick_t */
    uint8_t button; /* PICK_BUTTON: the alarm_panic_t of the keypad's button */
    size_t n;
    uint16_t value[ROLE_PICK_MAX];
    ui_choice_t choices[ROLE_PICK_MAX];
    char words[ROLE_PICK_MAX][ALARM_NAME_LEN + 16];
} s_pick UI_SLOW_BSS; /* PSRAM (B001) */

static void pick_add(const char *glyph, lv_color_t tone, const char *title, const char *detail,
                     uint16_t value)
{
    if (s_pick.n >= ROLE_PICK_MAX) {
        return;
    }
    const size_t i = s_pick.n++;
    i18n_utf8_copy(s_pick.words[i], sizeof(s_pick.words[i]), title);
    s_pick.choices[i].glyph = glyph;
    s_pick.choices[i].tone = tone;
    s_pick.choices[i].title = s_pick.words[i];
    s_pick.choices[i].detail = detail;
    s_pick.value[i] = value;
}

static int32_t pick_index(uint16_t value)
{
    for (size_t i = 0; i < s_pick.n; i++) {
        if (s_pick.value[i] == value) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void pick_start(role_pick_t what)
{
    s_pick.what = (uint8_t)what;
    s_pick.n = 0;
}

/* ---- a siren (page 14, ZB-023) ---- */

static const char *volume_words(uint8_t v)
{
    switch (v) {
    /* TR: a Zigbee siren's volume, a row's value and a choice. At most 23 bytes */
    case 0: return TR_C("siren volume", "Low");
    /* TR: a Zigbee siren's volume, a row's value and a choice. At most 23 bytes */
    case 1: return TR_C("siren volume", "Medium");
    /* TR: a Zigbee siren's volume, a row's value and a choice. At most 23 bytes */
    case 2: return TR_C("siren volume", "High");
    /* TR: a Zigbee siren's volume, a row's value and a choice. At most 23 bytes */
    default: return TR_C("siren volume", "Very high");
    }
}

/* How long it may be silent before it is the siren trouble: the installer's, else the default. */
static uint16_t supervision_of(const ui_zb_device_page_t *pg)
{
    if (pg->siren.supervision_min != 0u) {
        return pg->siren.supervision_min;
    }
    return pg->dev.power == UI_ZB_POWER_MAINS ? 90u : 180u; /* zbz_wd.h ZBW_SILENT_*_S */
}

/* "After 1 h", "After 1 h 30 min". */
static void after_words(uint16_t min, char *out, size_t len)
{
    char span[24];
    span_words((uint32_t)min * 60u, span, sizeof(span));
    /* TR: a Zigbee siren's Supervision: silent this long, it is a trouble; {0} "1 h 30 min".
     * At most 39 bytes */
    I18N_FORMAT(out, len, TR("After {0}"), span);
}

/* "All", "Home", "Home, Garage", "None". */
static void siren_parts_words(uint8_t parts, char *out, size_t len)
{
    const uint8_t all = parts_enabled();
    out[0] = '\0';
    if (parts == 0u) {
        /* TR: a Zigbee siren's Partitions row: it sounds for none (not used by the panel) */
        i18n_utf8_copy(out, len, TR_C("partitions", "None"));
        return;
    }
    if ((parts & all) == all) {
        /* TR: a Zigbee siren's Partitions row: it sounds for every partition */
        i18n_utf8_copy(out, len, TR_C("partitions", "All"));
        return;
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((parts & (1u << i)) == 0u) {
            continue;
        }
        char name[ALARM_NAME_LEN];
        part_name((uint8_t)(i + 1u), name, sizeof(name));
        if (out[0] == '\0') {
            i18n_utf8_copy(out, len, name);
        } else {
            char joined[96];
            I18N_FORMAT(joined, sizeof(joined), TR_C("list of names", "{0}, {1}"), out, name);
            i18n_utf8_copy(out, len, joined);
        }
    }
}

/* Its settings as they are, with one change, sent. A siren the panel doesn't use yet is used for
 * every partition at high volume once anything is turned on (decided in BL-147). */
typedef enum {
    SIREN_BURGLARY = 0,
    SIREN_FIRE,
    SIREN_STROBE,
    SIREN_SQUAWK,
} siren_flag_t;

static bool siren_settings(ui_zb_request_t *req)
{
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return false;
    }
    req_init(req, UI_ZB_REQ_SIREN, s_dev.id);
    req->siren = page.siren;
    return true;
}

static void siren_flag(bool on, void *ctx)
{
    ui_zb_request_t req;
    if (!siren_settings(&req)) {
        return;
    }
    if (req.siren.partitions == 0u && on) {
        req.siren.partitions = parts_enabled();
        req.siren.volume = 2u; /* High */
    }
    const siren_flag_t flag = (siren_flag_t)(uintptr_t)ctx;
    switch (flag) {
    case SIREN_BURGLARY: req.siren.burglary = on; break;
    case SIREN_FIRE: req.siren.fire = on; break;
    case SIREN_STROBE: req.siren.strobe = on; break;
    default: req.siren.squawk = on; break;
    }
    if ((flag == SIREN_BURGLARY || flag == SIREN_FIRE) && !req.siren.burglary && !req.siren.fire) {
        req.siren.partitions = 0u; /* it sounds for nothing: the panel no longer uses it */
    }
    role_ask(&req);
}

static void siren_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_pick.n) {
        return;
    }
    const uint16_t v = s_pick.value[index];
    ui_zb_request_t req;
    if (!siren_settings(&req)) {
        return;
    }
    switch ((role_pick_t)s_pick.what) {
    case PICK_SIREN_ALSO: req.siren.also = (uint8_t)v; break;
    case PICK_SIREN_PARTS:
        req.siren.partitions = (uint8_t)v;
        req.siren.also = (uint8_t)(req.siren.also & (uint8_t)~req.siren.partitions);
        break;
    case PICK_VOLUME: req.siren.volume = (uint8_t)v; break;
    default: req.siren.supervision_min = v; break;
    }
    role_ask(&req);
}

static void siren_parts_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    const uint8_t all = parts_enabled();
    pick_start(PICK_SIREN_PARTS);
    /* TR: a choice: a Zigbee siren sounds for every partition */
    pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), TR("All partitions"), NULL, all);
    if ((all & (uint8_t)(all - 1u)) != 0u) { /* more than one: each on its own too */
        for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
            if ((all & (1u << i)) != 0u) {
                char name[ALARM_NAME_LEN];
                part_name((uint8_t)(i + 1u), name, sizeof(name));
                pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), name, NULL, (uint16_t)(1u << i));
            }
        }
    }
    /* TR: a choice: the panel doesn't use this Zigbee siren */
    pick_add(UI_ICON_BELL_OFF, ui_tone_gray(), TR("None: the panel doesn't use it"),
             /* TR: under a choice of a Zigbee siren's partitions */
             TR("Its settings are forgotten"), 0u);
    /* TR: a pick sheet's title: which partitions' alarms a Zigbee siren sounds */
    ui_pick_sheet(TR("It sounds for"), TR("The alarms of these partitions"), s_pick.choices,
                  s_pick.n, pick_index(page.siren.partitions), siren_chosen);
}

/* OUT-015: other partitions whose alarms it also sounds for (never one of its own). */
static void siren_also_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    const uint8_t offer = (uint8_t)(parts_enabled() & (uint8_t)~page.siren.partitions);
    pick_start(PICK_SIREN_ALSO);
    pick_add(UI_ICON_BELL_OFF, ui_tone_gray(), TR_C("partitions", "None"), TR("Only its own partitions"),
             0u);
    if ((offer & (uint8_t)(offer - 1u)) != 0u) {
        pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), TR("All the others"), NULL, offer);
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((offer & (1u << i)) != 0u) {
            char name[ALARM_NAME_LEN];
            part_name((uint8_t)(i + 1u), name, sizeof(name));
            pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), name, NULL, (uint16_t)(1u << i));
        }
    }
    ui_pick_sheet(TR("Also sounds for"), TR("Other partitions' alarms, never their chirps"),
                  s_pick.choices, s_pick.n, pick_index(page.siren.also), siren_chosen);
}

static void volume_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    pick_start(PICK_VOLUME);
    for (uint8_t v = 0; v < 4u; v++) {
        pick_add(NULL, ui_tone_blue(), volume_words(v), NULL, v);
    }
    /* TR: a pick sheet's title: a Zigbee siren's volume */
    ui_pick_sheet(TR("Volume"), TR("For its alarms, chirps and tests"), s_pick.choices, s_pick.n,
                  pick_index(page.siren.volume), siren_chosen);
}

static void supervision_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    static const uint16_t MINUTES[] = {60u, 120u, 180u, 360u, 720u, 1440u};
    pick_start(PICK_SUPERVISION);
    char words[48];
    after_words(page.dev.power == UI_ZB_POWER_MAINS ? 90u : 180u, words, sizeof(words));
    /* TR: under the default choice of a Zigbee siren's Supervision */
    pick_add(NULL, ui_tone_blue(), words, TR("The default for this siren"), 0u);
    for (size_t i = 0; i < sizeof(MINUTES) / sizeof(MINUTES[0]); i++) {
        after_words(MINUTES[i], words, sizeof(words));
        pick_add(NULL, ui_tone_blue(), words, NULL, MINUTES[i]);
    }
    /* TR: a pick sheet's title: how long a Zigbee siren may be silent */
    ui_pick_sheet(TR("Supervision"),
                  /* TR: a pick sheet's note: a Zigbee siren's Supervision */
                  TR("Not heard this long, it is a siren trouble. It can't be turned off."),
                  s_pick.choices, s_pick.n, pick_index(page.siren.supervision_min), siren_chosen);
}

static void siren_test(void *ctx)
{
    (void)ctx;
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_SIREN_TEST, s_dev.id);
    const ui_zb_req_result_t r = ask(&req);
    if (r != UI_ZB_REQ_OK) {
        ui_toast(r == UI_ZB_REQ_NOT_ALLOWED
                     /* TR: a toast: a Zigbee siren's test is refused while a partition is armed */
                     ? TR("Not while one of its partitions is armed")
                     : role_refused(r, UI_ZB_REQ_SIREN_TEST));
        return;
    }
    s_dev.op = UI_ZB_OP_SIREN_TEST; /* its result shows in place (dev_tick) */
    s_dev.polled = lv_tick_get() - POLL_MS; /* "Sounding now" at the next tick */
}

/* What the Test it row says (test_state's values: the row changes only when it does). */
typedef enum {
    TEST_NONE = 0,
    TEST_SOUNDING,
    TEST_ANSWERED,
    TEST_NO_ANSWER,
    TEST_FAILED,
} test_state_t;

static test_state_t test_state(const ui_zb_device_page_t *pg)
{
    if (s_dev.op == UI_ZB_OP_SIREN_TEST) {
        return TEST_SOUNDING;
    }
    switch (pg->siren.last_test) {
    case UI_ZB_RES_DONE: return TEST_ANSWERED;
    case UI_ZB_RES_NO_ANSWER: return TEST_NO_ANSWER;
    case UI_ZB_RES_NONE: return TEST_NONE;
    default: return TEST_FAILED;
    }
}

static void test_words_to(lv_obj_t *row, test_state_t t)
{
    switch (t) {
    case TEST_SOUNDING:
        /* TR: under "Test it" on a Zigbee siren's page while its test runs */
        ui_row_set_detail(row, TR("Sounding now: listen for it"), true, ui_tone_blue());
        break;
    case TEST_ANSWERED:
        /* TR: under "Test it" on a Zigbee siren's page: its last test's result */
        ui_row_set_detail(row, TR("Last test: it answered and sounded"), true, ui_tone_green());
        break;
    case TEST_NO_ANSWER:
        /* TR: under "Test it" on a Zigbee siren's page: its last test's result */
        ui_row_set_detail(row, TR("Last test: no answer. Check its power"), true, ui_tone_amber());
        break;
    case TEST_FAILED:
        /* TR: under "Test it" on a Zigbee siren's page: its last test's result */
        ui_row_set_detail(row, TR("Last test: it couldn't be sent"), true, ui_tone_amber());
        break;
    default:
        /* TR: under "Test it" on a Zigbee siren's page (ZBW_TEST_S, 2 s, at its volume) */
        ui_row_set_detail(row, TR("Sounds it for 2 seconds"), false, ui_tone_gray());
        break;
    }
    s_dev.test_shown = (uint8_t)t;
}

static void siren_rows(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    const ui_zb_siren_t *sn = &pg->siren;
    const bool ready = sn->fit == UI_ZB_FIT_AVAILABLE;
    const bool can = installer() && ready;
    const role_why_t why = !installer() ? WHY_INSTALLER : WHY_SETUP;
    const bool used = sn->partitions != 0u;
    const role_why_t unused = can ? WHY_SIREN_OFF : why; /* a row that needs it used */
    const role_why_t test_why = !can || !used ? unused : WHY_NOT_HEARD;
    char value[96];
    /* TR: a section heading on a Zigbee siren's page: which alarms sound it */
    ui_section(body, TR_C("Zigbee siren section", "It sounds for"));
    if (ready && !used) {
        ui_paragraph(body,
                     /* TR: a paragraph on a Zigbee siren's page the panel doesn't use yet */
                     TR("The panel doesn't sound it yet: turn on what it sounds for."),
                     ui_tone_amber());
    }
    /* TR: a switch on a Zigbee siren's page */
    role_toggle(body, UI_ICON_SIREN, ui_tone_red(), TR_C("siren sounds for", "Burglary and panic"),
                /* TR: under "Burglary and panic" on a Zigbee siren's page */
                TR("Its partitions' alarms; a silent panic stays silent"), used && sn->burglary, can,
                siren_flag, (void *)(uintptr_t)SIREN_BURGLARY);
    /* TR: a switch on a Zigbee siren's page */
    role_toggle(body, UI_ICON_FLAME, ui_tone_red(), TR_C("siren sounds for", "Fire and CO"),
                /* TR: under "Fire and CO" on a Zigbee siren's page */
                TR("Its fire sound for fire, its emergency sound for CO"), used && sn->fire, can,
                siren_flag, (void *)(uintptr_t)SIREN_FIRE);

    /* TR: a section heading on a Zigbee siren's page */
    ui_section(body, TR_C("Zigbee siren section", "How it sounds"));
    /* its strobe is the siren's own light: nothing on this screen flashes (UI-020) */
    /* TR: a switch on a Zigbee siren's page */
    role_toggle(body, UI_ICON_SUN, ui_tone_blue(), TR("Strobe with it"),
                /* TR: under "Strobe with it" on a Zigbee siren's page */
                TR("Its own light, while it sounds"), used && sn->strobe, can && used, siren_flag,
                (void *)(uintptr_t)SIREN_STROBE);
    /* TR: a switch on a Zigbee siren's page */
    role_toggle(body, UI_ICON_BELL, ui_tone_blue(), TR("Chirp when arming and disarming"),
                /* TR: under "Chirp when arming and disarming" on a Zigbee siren's page */
                TR("One chirp to arm, two to disarm"), used && sn->squawk, can && used, siren_flag,
                (void *)(uintptr_t)SIREN_SQUAWK);
    role_link(body, UI_ICON_VOLUME, ui_tone_blue(), TR("Volume"), NULL,
              used ? volume_words(sn->volume) : "", used ? why : unused, can && used, volume_pick,
              NULL);
    siren_parts_words(sn->partitions, value, sizeof(value));
    /* TR: a row on a Zigbee siren's page: whose alarms it sounds */
    role_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR_C("Zigbee siren", "Partitions"), NULL,
              value, why, can, siren_parts_pick, NULL);
    if (used) {
        /* OUT-015: the alarms of other partitions too (the house's siren for the shop's alarm) */
        const uint8_t also = (uint8_t)(sn->also & parts_enabled() & (uint8_t)~sn->partitions);
        if (also == 0u) {
            i18n_utf8_copy(value, sizeof(value), TR_C("partitions", "None"));
        } else {
            siren_parts_words(also, value, sizeof(value));
        }
        role_link(body, UI_ICON_LAYERS, ui_tone_violet(), TR("Also sounds for"),
                  TR("Other partitions' alarms, never their chirps"), value, why, can,
                  siren_also_pick, NULL);
    }

    /* TR: a section heading on a Zigbee device's page */
    ui_section(body, TR_C("Zigbee device section", "Check it"));
    /* TR: a row on a Zigbee siren's page: sound it briefly */
    s_dev.test = role_link(body, UI_ICON_BELL_RING, ui_tone_blue(), TR("Test it"),
                           TR("Sounds it for 2 seconds"), NULL, test_why,
                           can && used && !pg->dev.unreachable, siren_test, NULL);
    test_words_to(s_dev.test, test_state(pg));
    after_words(supervision_of(pg), value, sizeof(value));
    /* TR: a row on a Zigbee siren's page: a trouble when it isn't heard */
    role_link(body, UI_ICON_CLOCK_ALERT, ui_tone_amber(), TR("Supervision"),
              /* TR: under "Supervision" on a Zigbee siren's page */
              TR("A siren trouble when it isn't heard"), value, used ? why : unused, can && used,
              supervision_pick, NULL);
}

/* ---- a keypad (page 15, ZB-024) ---- */

/* Its three emergency buttons and the one alarm each may raise (zbz_ace.c: Emergency medical,
 * Fire fire, Panic police - never a silent panic: the keypad's Panic command is police). */
static const struct {
    alarm_panic_t kind;
    const char *button; /* TR_NOOP */
    const char *alarm;  /* TR_NOOP */
} BUTTONS[] = {
    /* TR: a Zigbee keypad's emergency button, a row's title */
    {ALARM_PANIC_POLICE, TR_NOOP_C("keypad button", "Panic"),
     /* TR: what a Zigbee keypad's Panic button raises, a row's value and a choice */
     TR_NOOP_C("keypad button does", "Panic alarm, audible")},
    /* TR: a Zigbee keypad's emergency button, a row's title */
    {ALARM_PANIC_FIRE, TR_NOOP_C("keypad button", "Fire"),
     /* TR: what a Zigbee keypad's Fire button raises, a row's value and a choice */
     TR_NOOP_C("keypad button does", "Fire alarm")},
    /* TR: a Zigbee keypad's emergency button, a row's title */
    {ALARM_PANIC_MEDICAL, TR_NOOP_C("keypad button", "Emergency"),
     /* TR: what a Zigbee keypad's Emergency button raises, a row's value and a choice */
     TR_NOOP_C("keypad button does", "Medical alarm")},
};

static const char *const BUTTON_GLYPHS[] = {UI_ICON_SIREN, UI_ICON_FLAME, UI_ICON_BELL_RING};

static bool keypad_settings(ui_zb_request_t *req)
{
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return false;
    }
    req_init(req, UI_ZB_REQ_KEYPAD, s_dev.id);
    req->keypad = page.keypad;
    return true;
}

static void keypad_state_set(bool on, void *ctx)
{
    (void)ctx;
    ui_zb_request_t req;
    if (keypad_settings(&req)) {
        req.keypad.show_state = on;
        role_ask(&req);
    }
}

static void keypad_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_pick.n) {
        return;
    }
    const uint16_t v = s_pick.value[index];
    ui_zb_request_t req;
    if (!keypad_settings(&req)) {
        return;
    }
    if (s_pick.what == PICK_KEYPAD_PART) {
        if (req.keypad.partition == 0u && v != 0u) {
            req.keypad.show_state = true; /* a new keypad shows the panel's state (decided BL-148) */
        }
        req.keypad.partition = (uint8_t)v;
    } else {
        const uint8_t bit = (uint8_t)(1u << s_pick.button);
        req.keypad.panic_mask = v != 0u ? (uint8_t)(req.keypad.panic_mask | bit)
                                        : (uint8_t)(req.keypad.panic_mask & (uint8_t)~bit);
    }
    role_ask(&req);
}

static void keypad_part_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    const uint8_t all = parts_enabled();
    pick_start(PICK_KEYPAD_PART);
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((all & (1u << i)) != 0u) {
            char name[ALARM_NAME_LEN];
            part_name((uint8_t)(i + 1u), name, sizeof(name));
            pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), name, NULL, (uint16_t)(i + 1u));
        }
    }
    /* TR: a choice: a Zigbee keypad arms and disarms nothing */
    pick_add(UI_ICON_CIRCLE_X, ui_tone_gray(), TR_C("Zigbee keypad", "Off"),
             /* TR: under "Off", a choice of a Zigbee keypad's partition */
             TR("It doesn't arm or disarm the panel"), 0u);
    /* TR: a pick sheet's title: the partition a Zigbee keypad arms and disarms */
    ui_pick_sheet(TR("Partition"), TR("The one it arms and disarms"), s_pick.choices, s_pick.n,
                  pick_index(page.keypad.partition), keypad_chosen);
}

static void button_pick(void *ctx)
{
    const size_t b = (size_t)(uintptr_t)ctx;
    ui_zb_device_page_t page;
    if (b >= sizeof(BUTTONS) / sizeof(BUTTONS[0]) || !read_device(s_dev.id, &page)) {
        return;
    }
    pick_start(PICK_BUTTON);
    s_pick.button = (uint8_t)BUTTONS[b].kind;
    pick_add(UI_ICON_CIRCLE_X, ui_tone_gray(), TR_C("keypad button does", "Off"),
             /* TR: under "Off", a choice of what a Zigbee keypad's emergency button does */
             TR("Pressing it does nothing"), 0u);
    pick_add(BUTTON_GLYPHS[b], ui_tone_red(), TR_C("keypad button does", BUTTONS[b].alarm), NULL,
             1u);
    const bool on = (page.keypad.panic_mask & (1u << (unsigned)BUTTONS[b].kind)) != 0u;
    /* TR: a pick sheet's note: what a Zigbee keypad's emergency button does */
    ui_pick_sheet(TR_C("keypad button", BUTTONS[b].button), TR("Held as the keypad's manual says."),
                  s_pick.choices, s_pick.n, on ? 1 : 0, keypad_chosen);
}

static void keypad_rows(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    const ui_zb_keypad_t *kp = &pg->keypad;
    const bool slot = kp->fit == UI_ZB_FIT_AVAILABLE;
    const bool can = installer() && slot;
    const role_why_t why = !installer()                       ? WHY_INSTALLER
                           : kp->fit == UI_ZB_FIT_NO_DEVICE_SLOT ? WHY_NO_SLOT
                                                                 : WHY_SETUP;
    const bool on = kp->partition != 0u;
    char value[ALARM_NAME_LEN + 16];
    /* TR: a section heading on a Zigbee keypad's page */
    ui_section(body, TR_C("Zigbee keypad section", "Arming"));
    if (kp->fit == UI_ZB_FIT_NO_DEVICE_SLOT) {
        ui_paragraph(body,
                     /* TR: a paragraph on a Zigbee keypad's page when no keypad number is free */
                     TR("Every keypad number from 1 to 8 is in use by other keypads. Free one "
                        "under Keypads and devices to use this one."),
                     ui_tone_amber());
    }
    if (on) {
        part_name(kp->partition, value, sizeof(value));
    } else {
        i18n_utf8_copy(value, sizeof(value), TR_C("Zigbee keypad", "Off"));
    }
    role_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR("Partition"),
              /* TR: under "Partition" on a Zigbee keypad's page */
              TR("The one it arms and disarms"), value, why, can, keypad_part_pick, NULL);
    ui_paragraph(body,
                 /* TR: a paragraph on a Zigbee keypad's page */
                 TR("Codes typed on it are checked by the panel. Five wrong codes lock it for a "
                    "while, like this screen."),
                 ui_tone_gray());
    /* TR: a switch on a Zigbee keypad's page */
    role_toggle(body, UI_ICON_SHIELD, ui_tone_blue(), TR("Show the panel's state"),
                /* TR: under "Show the panel's state" on a Zigbee keypad's page */
                TR("Its lights and sounds follow the panel"), on && kp->show_state, can && on,
                keypad_state_set, NULL);
    if (kp->device != 0u) {
        char number[8];
        snprintf(number, sizeof(number), "%u", (unsigned)kp->device);
        /* TR: a row on a Zigbee keypad's page: the keypad number the panel gave it (its events
         * and its lockout name it) */
        ui_row_value(body, TR("Keypad number"), number);
    }
    /* TR: a section heading on a Zigbee keypad's page: its emergency buttons */
    ui_section(body, TR_C("Zigbee keypad section", "Its buttons"));
    for (size_t b = 0; b < sizeof(BUTTONS) / sizeof(BUTTONS[0]); b++) {
        const bool set = on && (kp->panic_mask & (1u << (unsigned)BUTTONS[b].kind)) != 0u;
        role_link(body, BUTTON_GLYPHS[b], ui_tone_red(), TR_C("keypad button", BUTTONS[b].button),
                  NULL,
                  set ? TR_C("keypad button does", BUTTONS[b].alarm)
                      : TR_C("keypad button does", "Off"),
                  on || !can ? why : WHY_KEYPAD_OFF, can && on, button_pick,
                  (void *)(uintptr_t)b);
    }
}

/* ---- a key fob (page 16, ZB-024) ---- */

static bool fob_settings(ui_zb_request_t *req)
{
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return false;
    }
    req_init(req, UI_ZB_REQ_FOB, s_dev.id);
    req->fob = page.fob;
    return true;
}

typedef enum { FOB_DISARM = 0, FOB_PANIC } fob_flag_t;

static void fob_flag(bool on, void *ctx)
{
    ui_zb_request_t req;
    if (!fob_settings(&req)) {
        return;
    }
    if ((fob_flag_t)(uintptr_t)ctx == FOB_DISARM) {
        req.fob.may_disarm = on;
    } else {
        req.fob.panic = on;
    }
    role_ask(&req);
}

/* The first partition this user may use that the panel uses; 1 when none. */
static uint8_t user_partition(uint8_t user)
{
    const uint8_t all = parts_enabled();
    uint8_t mask = all;
    if (S.cfg != NULL && user >= 1u && user <= ALARM_MAX_USERS &&
        (S.cfg->users[user - 1u].partitions & all) != 0u) {
        mask = (uint8_t)(S.cfg->users[user - 1u].partitions & all);
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((mask & (1u << i)) != 0u) {
            return (uint8_t)(i + 1u);
        }
    }
    return 1u;
}

static void fob_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_pick.n) {
        return;
    }
    const uint16_t v = s_pick.value[index];
    ui_zb_request_t req;
    if (!fob_settings(&req)) {
        return;
    }
    if (s_pick.what == PICK_FOB_USER) {
        req.fob.user = (uint8_t)v;
        if (v != 0u && req.fob.partition == 0u) {
            req.fob.partition = user_partition((uint8_t)v);
        }
    } else {
        req.fob.partition = (uint8_t)v;
    }
    role_ask(&req);
}

static void fob_user_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page) || S.cfg == NULL) {
        return;
    }
    pick_start(PICK_FOB_USER);
    for (uint8_t u = 1; u <= ALARM_MAX_USERS; u++) {
        const alarm_user_t *usr = &S.cfg->users[u - 1u];
        if (usr->authority == ALARM_AUTH_NONE || usr->authority == ALARM_AUTH_INSTALLER ||
            usr->authority == ALARM_AUTH_DURESS) {
            continue; /* ZB-024: never the installer, never a duress code (alarm-devices.md §3) */
        }
        char name[ALARM_NAME_LEN + 8];
        ui_user_name(S.cfg, u, name, sizeof(name));
        pick_add(UI_ICON_USER, ui_authority_tone(usr->authority), name,
                 ui_authority_name(usr->authority), u);
    }
    /* TR: a choice: a Zigbee key fob belongs to nobody, so it does nothing */
    pick_add(UI_ICON_CIRCLE_X, ui_tone_gray(), TR("Nobody: it does nothing"), NULL, 0u);
    /* TR: a pick sheet's title: whose Zigbee key fob it is */
    ui_pick_sheet(TR("Belongs to"),
                  /* TR: a pick sheet's note: a Zigbee key fob acts as its user */
                  TR("It arms and disarms with this user's rights"), s_pick.choices, s_pick.n,
                  pick_index(page.fob.user), fob_chosen);
}

static void fob_part_pick(void *ctx)
{
    (void)ctx;
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        return;
    }
    const uint8_t all = parts_enabled();
    pick_start(PICK_FOB_PART);
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((all & (1u << i)) != 0u) {
            char name[ALARM_NAME_LEN];
            part_name((uint8_t)(i + 1u), name, sizeof(name));
            pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), name, NULL, (uint16_t)(i + 1u));
        }
    }
    /* TR: a pick sheet's title: the partition a Zigbee key fob arms */
    ui_pick_sheet(TR_C("Zigbee key fob", "Arms"), TR("The partition its buttons arm"),
                  s_pick.choices, s_pick.n, pick_index(page.fob.partition), fob_chosen);
}

static void fob_rows(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    const ui_zb_fob_t *fb = &pg->fob;
    const bool can = installer() && fb->fit == UI_ZB_FIT_AVAILABLE;
    const role_why_t why = !installer() ? WHY_INSTALLER : WHY_SETUP;
    const bool on = fb->user != 0u;
    char value[ALARM_NAME_LEN + 16];
    /* TR: a section heading on a Zigbee key fob's page */
    ui_section(body, TR_C("Zigbee key fob section", "Who uses it"));
    if (on && S.cfg != NULL) {
        ui_user_name(S.cfg, fb->user, value, sizeof(value));
    } else {
        /* TR: a Zigbee key fob's "Belongs to" row: nobody's */
        i18n_utf8_copy(value, sizeof(value), TR_C("key fob user", "Nobody"));
    }
    /* TR: a row on a Zigbee key fob's page */
    role_link(body, UI_ICON_USER, ui_tone_violet(), TR("Belongs to"),
              /* TR: under "Belongs to" on a Zigbee key fob's page */
              TR("Its presses are logged with this name"), value, why, can, fob_user_pick, NULL);
    if (on && fb->partition != 0u) {
        part_name(fb->partition, value, sizeof(value));
    } else {
        value[0] = '\0'; /* nobody's, or no partition yet */
    }
    /* TR: a row on a Zigbee key fob's page: the partition it arms */
    role_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR_C("Zigbee key fob", "Arms"), NULL,
              value, on || !can ? why : WHY_FOB_OFF, can && on, fob_part_pick, NULL);
    /* TR: a section heading on a Zigbee key fob's page */
    ui_section(body, TR_C("Zigbee key fob section", "What it may do"));
    /* TR: a switch on a Zigbee key fob's page */
    role_toggle(body, UI_ICON_LOCK_OPEN, ui_tone_green(), TR("May disarm"),
                /* TR: under "May disarm" on a Zigbee key fob's page */
                TR("Off: it only arms, so a lost fob can't open the house"), on && fb->may_disarm,
                can && on, fob_flag, (void *)(uintptr_t)FOB_DISARM);
    /* TR: a switch on a Zigbee key fob's page */
    role_toggle(body, UI_ICON_SIREN, ui_tone_red(), TR("Panic button"),
                /* TR: under "Panic button" on a Zigbee key fob's page */
                TR("Sounds a panic alarm"), on && fb->panic, can && on, fob_flag,
                (void *)(uintptr_t)FOB_PANIC);
    ui_paragraph(body,
                 /* TR: a paragraph on a Zigbee key fob's page */
                 TR("Disarming from a fob takes no code: turn it on only for a fob its owner "
                    "keeps with their keys."),
                 ui_tone_gray());
}

/* A siren's, keypad's or key fob's sections. Returns true when they opened "Check it". */
static bool role_rows(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    switch (pg->dev.kind) {
    case UI_ZB_KIND_SIREN: siren_rows(body, pg); return true;
    case UI_ZB_KIND_KEYPAD: keypad_rows(body, pg); return false;
    case UI_ZB_KIND_KEYFOB: fob_rows(body, pg); return false;
    default: return false;
    }
}

/* BL-215: a sensor with a sounder of its own (a smoke detector's) can also sound as a siren: the
 * siren's rows under a paragraph saying so. Returns true when they opened "Check it". */
static bool sounder_rows(lv_obj_t *body, const ui_zb_device_page_t *pg)
{
    if (pg->siren.fit == UI_ZB_FIT_NONE || pg->dev.kind == UI_ZB_KIND_SIREN) {
        return false;
    }
    /* TR: a paragraph on a Zigbee smoke detector's page that has a sounder of its own (BL-215) */
    ui_paragraph(body, TR("Its own sounder can also sound with the panel's alarms, like a siren."),
                 ui_tone_gray());
    siren_rows(body, pg);
    return true;
}

/* The page's note for a role: "Heiman siren · Mains · Extends the network". */
static void role_note(const ui_zb_device_t *d, char *note, size_t nlen)
{
    char fact[40];
    note[0] = '\0';
    join(note, nlen, d->model);
    power_fact(d, fact, sizeof(fact));
    join(note, nlen, fact);
    join(note, nlen, d->router ? TR("Extends the network") : signal_fact(d->signal));
}

static void build_device(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    const uint16_t id = (uint16_t)arg;
    if (s_dev.id != id) {
        s_dev.op = UI_ZB_OP_NONE;
    }
    s_dev.id = id;
    s_dev.open = false;
    s_dev.heard = NULL;
    s_dev.signal = NULL;
    s_dev.power = NULL;
    s_dev.setup = NULL;
    s_dev.test = NULL;
    ui_zb_device_page_t page;
    if (!read_device(id, &page)) {
        i18n_utf8_copy(title, tlen, TR("Zigbee device"));
        ui_paragraph(body, TR("This device is no longer in the network."), ui_tone_gray());
        return;
    }
    const ui_zb_device_t *d = &page.dev;
    char name[48];
    char where[32];
    device_name(d, name, sizeof(name));
    i18n_utf8_copy(title, tlen, name);
    value_words(d, where, sizeof(where));
    note[0] = '\0';
    if (is_role(d)) {
        role_note(d, note, nlen);
    } else {
        if (strcmp(name, d->model) != 0) {
            join(note, nlen, d->model);
        }
        join(note, nlen, where);
    }
    top_paragraph(body, &page);
    if (is_role(d) && !d->unreachable) {
        /* a siren, keypad or key fob: what it does first, then the device page's rows */
        const bool checked = role_rows(body, &page);
        fact_rows(body, &page, false);
        check_rows(body, &page, !checked);
        remove_rows(body, true);
    } else if (d->unreachable) {
        /* missing: what to do first - when it was heard, and how, then removing it */
        char text[48];
        ago_words(d->heard_s, text, sizeof(text));
        s_dev.heard = ui_row_value(body, TR("Last heard"), text);
        s_dev.signal = ui_row_value(body, TR_C("Zigbee device", "Signal"),
                                    signal_value(d->signal, true));
        remove_rows(body, is_role(d));
        used_by_rows(body, &page);
        if (is_role(d)) {
            (void)role_rows(body, &page); /* its own "Check it" first, then the device's */
        }
        fact_rows(body, &page, true);
        check_rows(body, &page, d->kind != UI_ZB_KIND_SIREN);
    } else if (d->zone_count == 0u && !is_role(d)) {
        used_by_rows(body, &page); /* not in a zone yet: what to do first */
        const bool sounder = sounder_rows(body, &page);
        fact_rows(body, &page, false);
        check_rows(body, &page, !sounder);
        remove_rows(body, false);
    } else {
        fact_rows(body, &page, false);
        used_by_rows(body, &page);
        const bool sounder = sounder_rows(body, &page);
        check_rows(body, &page, !sounder);
        remove_rows(body, false);
    }
    s_dev.sig = dev_sig(&page);
    s_dev.open = true;
    s_dev.polled = lv_tick_get();
}

/* The answer to what this page asked: said once, then cleared. */
static bool dev_answer(void)
{
    if (s_dev.op == UI_ZB_OP_NONE) {
        return false;
    }
    ui_zb_progress_t p;
    read_progress(&p);
    if (p.op != s_dev.op || p.id != s_dev.id || p.step != UI_ZB_STEP_DONE) {
        return false;
    }
    const uint8_t op = s_dev.op;
    s_dev.op = UI_ZB_OP_NONE;
    ask_quietly(UI_ZB_REQ_DISMISS);
    switch (op) {
    case UI_ZB_OP_PING:
        ui_toast(p.result == UI_ZB_RES_ANSWERED ? TR("It answered") : TR("No answer"));
        break;
    case UI_ZB_OP_IDENTIFY:
        /* TR: a toast after "Blink its light" on a Zigbee device's page */
        ui_toast(p.result == UI_ZB_RES_DONE ? TR("Its light blinks for a few seconds")
                                            : TR("It didn't answer: nothing blinks"));
        break;
    case UI_ZB_OP_FORGET:
        if (p.result == UI_ZB_RES_FORGOTTEN) {
            char zones[32];
            char text[96];
            zones_words(p.zones, p.zone_count, zones, sizeof(zones));
            if (p.zone_count == 0u) {
                /* TR: a toast after a Zigbee device was forgotten */
                i18n_utf8_copy(text, sizeof(text), TR("Forgotten"));
            } else {
                /* TR: a toast after a Zigbee device was forgotten; {0} "Zone 8" or "Zones 6, 9" */
                I18N_FORMAT(text, sizeof(text),
                            TRN("Forgotten: {0} needs a new sensor",
                                "Forgotten: {0} need a new sensor", p.zone_count),
                            zones);
            }
            ui_toast(text); /* the zones' need isn't on the page it goes back to */
            ui_page_back();
            return true;
        }
        ui_toast(refused_words(UI_ZB_REQ_INVALID));
        break;
    default: break; /* a setup's end, a siren's test: shown on the page itself */
    }
    return false;
}

static void dev_tick(void)
{
    if (!s_dev.open || lv_tick_elaps(s_dev.polled) < POLL_MS) {
        return;
    }
    s_dev.polled = lv_tick_get();
    if (dev_answer()) {
        return;
    }
    ui_zb_device_page_t page;
    if (!read_device(s_dev.id, &page)) {
        ui_page_back(); /* it left the network while its page was open */
        return;
    }
    if (dev_sig(&page) != s_dev.sig) {
        ui_page_rebuild();
        return;
    }
    const ui_zb_device_t *d = &page.dev;
    char text[96];
    if (s_dev.heard != NULL) {
        ago_words(d->heard_s, text, sizeof(text));
        ui_set_label(s_dev.heard, "value_label", text);
    }
    if (s_dev.signal != NULL) {
        ui_set_label(s_dev.signal, "value_label", signal_value(d->signal, d->unreachable));
    }
    if (s_dev.power != NULL) {
        dev_power_words(d, text, sizeof(text));
        ui_set_label(s_dev.power, "value_label", text);
    }
    if (s_dev.test != NULL && (uint8_t)test_state(&page) != s_dev.test_shown) {
        test_words_to(s_dev.test, test_state(&page)); /* "Test it": its result in place */
    }
    if (s_dev.setup != NULL) {
        I18N_FORMAT(text, sizeof(text), TR("Setting it up: step {0} of {1}."),
                    (unsigned)d->setup_done, (unsigned)d->setup_total);
        if (strcmp(lv_label_get_text(s_dev.setup), text) != 0) {
            lv_label_set_text(s_dev.setup, text);
        }
    }
}

/* ---- words for addresses and codes ------------------------------------------------------------ */

/* "02AB CD00 0000 0101": an address in groups of four, as most labels print it. */
static void ieee_words(uint64_t ieee, char *out, size_t len)
{
    snprintf(out, len, "%04X %04X %04X %04X", (unsigned)((ieee >> 48) & 0xFFFFu),
             (unsigned)((ieee >> 32) & 0xFFFFu), (unsigned)((ieee >> 16) & 0xFFFFu),
             (unsigned)(ieee & 0xFFFFu));
}

/* "02:AB:CD:00:00:00:00:FF": an extended id or address on the radio's page. */
static void colon_words(uint64_t v, char *out, size_t len)
{
    char *p = out;
    size_t left = len;
    out[0] = '\0';
    for (int i = 7; i >= 0 && left > 3u; i--) {
        const int n = snprintf(p, left, i > 0 ? "%02X:" : "%02X",
                               (unsigned)((v >> ((unsigned)i * 8u)) & 0xFFu));
        if (n < 0 || (size_t)n >= left) {
            break;
        }
        p += n;
        left -= (size_t)n;
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* The hex digits of what was typed, spaces, dashes and colons skipped (as on labels): how many,
 * -1 for any other character. Packed into out (cap bytes) when out isn't NULL. */
static int hex_digits(const char *text, uint8_t *out, size_t cap)
{
    int n = 0;
    for (const char *c = text; *c != '\0'; c++) {
        if (*c == ' ' || *c == '-' || *c == ':') {
            continue;
        }
        const int v = hex_value(*c);
        if (v < 0) {
            return -1;
        }
        const size_t at = (size_t)n / 2u;
        if (out != NULL && at < cap) {
            out[at] = (n % 2 == 0) ? (uint8_t)((unsigned)v << 4)
                                   : (uint8_t)(out[at] | (uint8_t)v);
        }
        n++;
    }
    return n;
}

typedef enum {
    TYPED_EMPTY = 0, /* nothing typed */
    TYPED_BAD_CHAR,  /* something that isn't 0-9, A-F, a space or a dash */
    TYPED_SHORT,     /* not all of it yet */
    TYPED_LONG,      /* too much */
    TYPED_WRONG,     /* an address of zeros or of Fs; a code whose check doesn't match */
    TYPED_OK,
} typed_t;

/* An address on a label: exactly 16 hex digits, not all 0 and not all F. */
static typed_t address_typed(const char *text, uint64_t *ieee, int *digits)
{
    uint8_t b[8] = {0};
    const int n = hex_digits(text, b, sizeof(b));
    *digits = n;
    *ieee = 0u;
    if (n == 0) {
        return TYPED_EMPTY;
    }
    if (n < 0) {
        return TYPED_BAD_CHAR;
    }
    if (n < 16) {
        return TYPED_SHORT;
    }
    if (n > 16) {
        return TYPED_LONG;
    }
    for (size_t i = 0; i < sizeof(b); i++) {
        *ieee = (*ieee << 8) | b[i];
    }
    return *ieee == 0u || *ieee == UINT64_MAX ? TYPED_WRONG : TYPED_OK;
}

/* CRC-16/X.25 (reflected 0x1021, init and final xor 0xFFFF): an install code's check. The same
 * check as zigbee_zones' zbz_code_crc(), which the backend runs again on the request - this copy
 * only says while typing whether the code is right (the UI doesn't link zigbee_zones). */
static uint16_t code_crc(const uint8_t *b, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ b[i]);
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) != 0u ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
        }
    }
    return (uint16_t)~crc;
}

/* The lengths an install code comes in, with its check: 16, 20, 28 or 36 hex digits. */
static const int CODE_DIGITS[] = {16, 20, 28, 36};

/* An install code: 16, 20, 28 or 36 hex digits, its last 4 the check of the rest (low byte
 * first). `need`: the length it is heading for while TYPED_SHORT. */
static typed_t code_typed(const char *text, int *digits, int *need)
{
    uint8_t b[18] = {0};
    const int n = hex_digits(text, b, sizeof(b));
    *digits = n;
    *need = CODE_DIGITS[0];
    if (n == 0) {
        return TYPED_EMPTY;
    }
    if (n < 0) {
        return TYPED_BAD_CHAR;
    }
    if (n > CODE_DIGITS[3]) {
        return TYPED_LONG;
    }
    bool exact = false;
    for (size_t i = 0; i < sizeof(CODE_DIGITS) / sizeof(CODE_DIGITS[0]); i++) {
        exact = exact || n == CODE_DIGITS[i];
        if (n <= CODE_DIGITS[i]) {
            *need = CODE_DIGITS[i];
            break;
        }
    }
    if (!exact) {
        return TYPED_SHORT;
    }
    const size_t len = (size_t)n / 2u;
    const uint16_t want = (uint16_t)((unsigned)b[len - 2u] | (unsigned)b[len - 1u] << 8);
    if (code_crc(b, len - 2u) == want) {
        return TYPED_OK;
    }
    if (n < CODE_DIGITS[3]) { /* a longer code on its way: still counting (the proposal's 28 of 36) */
        for (size_t i = 0; i < sizeof(CODE_DIGITS) / sizeof(CODE_DIGITS[0]); i++) {
            if (CODE_DIGITS[i] > n) {
                *need = CODE_DIGITS[i];
                break;
            }
        }
        return TYPED_SHORT;
    }
    return TYPED_WRONG;
}

/* The line under the field while an address is typed (the text sheet's note). */
static void address_check_words(const char *text, char *out, size_t len)
{
    uint64_t ieee;
    int n;
    switch (address_typed(text, &ieee, &n)) {
    case TYPED_EMPTY:
        /* TR: the note of the text sheet for a Zigbee device's address */
        i18n_utf8_copy(out, len, TR("16 characters on its label, often after MAC or IEEE"));
        break;
    case TYPED_BAD_CHAR:
        /* TR: under the field while an address or install code is typed */
        i18n_utf8_copy(out, len, TR("Only 0 to 9 and A to F: compare it with the label"));
        break;
    case TYPED_SHORT:
        /* TR: under the field while an address is typed; {0} how many so far */
        I18N_FORMAT(out, len, TR("{0} of 16 characters"), (unsigned)n);
        break;
    case TYPED_LONG:
        /* TR: under the field while an address is typed */
        i18n_utf8_copy(out, len, TR("Too many: an address has 16 characters"));
        break;
    case TYPED_WRONG:
        /* TR: under the field: an address of only zeros or only Fs */
        i18n_utf8_copy(out, len, TR("That isn't an address: compare it with the label"));
        break;
    default:
        /* TR: under the field: the address has all its characters */
        i18n_utf8_copy(out, len, TR("All 16 characters"));
        break;
    }
}

static uint32_t text_hash(const char *text)
{
    uint32_t h = 2166136261u;
    for (const char *c = text; *c != '\0'; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    return h | 1u; /* never 0: 0 is "none" */
}

/* The text Done was refused for (its hash; 0 none): a code of a whole length whose check fails is
 * then said to fail, where typing on counts towards the next length. */
static uint32_t s_code_refused;

/* The line under the field while an install code is typed: it counts, then says whether its
 * check matches (the proposal's code-entry and code-wrong screens). */
static void code_check_words(const char *text, char *out, size_t len)
{
    int n;
    int need;
    typed_t t = code_typed(text, &n, &need);
    if (t == TYPED_SHORT && (n == 16 || n == 20 || n == 28) && s_code_refused != 0u &&
        s_code_refused == text_hash(text)) {
        t = TYPED_WRONG; /* Done was tapped: it is the whole code, and it doesn't check out */
    }
    switch (t) {
    case TYPED_EMPTY:
        /* TR: the note of the text sheet for a Zigbee device's install code */
        i18n_utf8_copy(out, len, TR("On its label or box, often beside a QR code"));
        break;
    case TYPED_BAD_CHAR:
        i18n_utf8_copy(out, len, TR("Only 0 to 9 and A to F: compare it with the label"));
        break;
    case TYPED_SHORT:
        /* TR: under the field while an install code is typed; {0} how many so far, {1} how
         * many it needs (16, 20, 28 or 36) */
        I18N_FORMAT(out, len, TR("{0} of {1} characters"), (unsigned)n, (unsigned)need);
        break;
    case TYPED_LONG:
        /* TR: under the field while an install code is typed */
        i18n_utf8_copy(out, len, TR("Too many: a code has at most 36 characters"));
        break;
    case TYPED_WRONG:
        /* TR: under the field: the install code's own check (its last 4 characters) fails */
        i18n_utf8_copy(out, len, TR("It doesn't check out: compare it with the label"));
        break;
    default:
        /* TR: under the field: the install code's own check matches */
        i18n_utf8_copy(out, len, TR("Its check matches: the code is right"));
        break;
    }
}

/* What the backend said about a typed address or code (ZB-015): each its own words. */
static const char *code_refused_words(ui_zb_req_result_t r)
{
    switch (r) {
    case UI_ZB_REQ_BAD_CODE:
        /* TR: a toast: the install code typed is of the wrong length or not hex */
        return TR("That isn't an install code: it has 16, 20, 28 or 36 characters");
    case UI_ZB_REQ_CHECKSUM: return TR("It doesn't check out: compare it with the label");
    case UI_ZB_REQ_BAD_ADDRESS:
        /* TR: a toast: the address typed isn't 16 hex characters, or is all zeros */
        return TR("That isn't an address: it has 16 characters");
    case UI_ZB_REQ_FULL:
        /* TR: a toast: no room for another install code */
        return TR("No room for another install code: forget one that is waiting");
    default: return refused_words(r);
    }
}

/* ---- adding with an install code (PAGE_ZIGBEE_CODE, the proposal's page 4) ------------------ */

/* What is typed, kept while the page is in the stack and wiped as it goes (the code is a key: it
 * is never shown again once typed, and never kept by the screen after its page). */
static struct {
    char addr[32];
    char code[UI_ZB_CODE_TEXT];
    bool given;      /* the backend took it (ADD_CODE or CODE_ADD): nothing to keep on leaving */
    bool active;     /* the page is in the stack */
    uint8_t typing;  /* 1: the address's text sheet is open, 2: the code's; its note follows it */
    uint32_t polled;
} s_code;

static void code_wipe(void)
{
    memset(&s_code, 0, sizeof(s_code));
    s_code_refused = 0u;
}

static bool code_ready(uint64_t *ieee)
{
    int n;
    int need;
    return address_typed(s_code.addr, ieee, &n) == TYPED_OK &&
           code_typed(s_code.code, &n, &need) == TYPED_OK;
}

static void open_code_sheet(uint8_t which, const char *initial);

static void address_done(const char *text)
{
    uint64_t ieee;
    int n;
    if (address_typed(text, &ieee, &n) != TYPED_OK) {
        open_code_sheet(1u, text); /* Done takes exactly 16 characters: again, saying why */
        return;
    }
    char words[24];
    ieee_words(ieee, words, sizeof(words));
    snprintf(s_code.addr, sizeof(s_code.addr), "%s", words);
    s_code.given = false;
    ui_page_rebuild();
}

static void code_done(const char *text)
{
    int n;
    int need;
    if (code_typed(text, &n, &need) != TYPED_OK) {
        s_code_refused = text_hash(text);
        open_code_sheet(2u, text); /* refused until its check matches: nothing goes to the radio */
        return;
    }
    s_code_refused = 0u;
    snprintf(s_code.code, sizeof(s_code.code), "%s", text);
    s_code.given = false;
    ui_page_rebuild();
}

/* The panel's own keyboard, its digits first; the note under the title counts and checks what is
 * typed once a second (code_tick, UI-023). */
static void open_code_sheet(uint8_t which, const char *initial)
{
    char note[96];
    if (which == 1u) {
        address_check_words(initial, note, sizeof(note));
        /* TR: the text sheet's title for a Zigbee device's address */
        ui_text_sheet_kind(TR("Its address"), note, initial, UI_TEXT_NUMBER, 23u, address_done);
    } else {
        code_check_words(initial, note, sizeof(note));
        /* TR: the text sheet's title for a Zigbee device's install code */
        ui_text_sheet_kind(TR("Its install code"), note, initial, UI_TEXT_NUMBER,
                           UI_ZB_CODE_TEXT - 1u, code_done);
    }
    s_code.typing = which;
    s_code.polled = lv_tick_get();
}

static void type_address(void *ctx)
{
    (void)ctx;
    open_code_sheet(1u, s_code.addr);
}

/* A code that checks out is a key: the sheet opens empty (it is never shown again), and Cancel
 * keeps the one taken. */
static void type_code(void *ctx)
{
    (void)ctx;
    int n;
    int need;
    open_code_sheet(2u, code_typed(s_code.code, &n, &need) == TYPED_OK ? "" : s_code.code);
}

static void code_tick(void)
{
    if (s_code.typing == 0u || lv_tick_elaps(s_code.polled) < POLL_MS) {
        return;
    }
    s_code.polled = lv_tick_get();
    lv_obj_t *ta = ui_find(lv_screen_active(), "sheet_text");
    if (lv_subject_get_int(&subject_sheet) != SHEET_TEXT || ta == NULL) {
        s_code.typing = 0u; /* the sheet closed */
        return;
    }
    char note[96];
    if (s_code.typing == 1u) {
        address_check_words(lv_textarea_get_text(ta), note, sizeof(note));
    } else {
        code_check_words(lv_textarea_get_text(ta), note, sizeof(note));
    }
    if (strcmp(lv_subject_get_string(&subject_sheet_note), note) != 0) {
        ui_set_str(&subject_sheet_note, note);
    }
}

/* The add page's request with an install code (its arg 3: "Try again" asks the same). */
static void code_fill(ui_zb_request_t *req)
{
    uint64_t ieee;
    int n;
    req_init(req, UI_ZB_REQ_ADD_CODE, 0u);
    (void)address_typed(s_code.addr, &ieee, &n);
    req->ieee = ieee;
    snprintf(req->code, sizeof(req->code), "%s", s_code.code);
}

/* The add page's paragraph while it listens for the install code's device. */
static void code_listen_words(char *out, size_t len)
{
    uint64_t ieee;
    int n;
    char addr[24];
    (void)address_typed(s_code.addr, &ieee, &n);
    ieee_words(ieee, addr, sizeof(addr));
    /* TR: a paragraph on the add page with an install code; {0} the device's address */
    I18N_FORMAT(out, len,
                TR("Power it up or put it in pairing mode now. Only the device {0} may join."),
                addr);
}

static void code_listen(void *ctx)
{
    (void)ctx;
    uint64_t ieee;
    if (!installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    if (!code_ready(&ieee)) {
        /* TR: a toast on the install code page when Listen is tapped too early */
        ui_toast(TR("Type its address and install code first"));
        return;
    }
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_ADD_CODE, 0u);
    req.ieee = ieee;
    snprintf(req.code, sizeof(req.code), "%s", s_code.code);
    const ui_zb_req_result_t r = ask(&req); /* wiped */
    if (r != UI_ZB_REQ_OK) {
        ui_toast(code_refused_words(r));
        return;
    }
    s_code.given = true;
    ui_page_open(PAGE_ZIGBEE_ADD, 3); /* the same steps, for this device only */
}

/* Leaving the page with an address and code typed but not listened for: the code waits under
 * Install codes waiting (the proposal's page 4), said in a toast as nothing on screen shows it. */
static void code_leave(void)
{
    uint64_t ieee;
    if (!s_code.given && installer() && code_ready(&ieee)) {
        ui_zb_request_t req;
        req_init(&req, UI_ZB_REQ_CODE_ADD, 0u);
        req.ieee = ieee;
        snprintf(req.code, sizeof(req.code), "%s", s_code.code);
        const ui_zb_req_result_t r = ask(&req); /* wiped */
        /* TR: a toast after leaving the install code page without listening */
        ui_toast(r == UI_ZB_REQ_OK ? TR("Its code waits under Install codes waiting")
                                   : code_refused_words(r));
    }
    code_wipe();
}

static void build_code(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                       size_t nlen)
{
    (void)arg;
    /* TR: a page's title, and a row on the Zigbee page. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Add with an install code"));
    /* TR: the install code page's subtitle */
    i18n_utf8_copy(note, nlen, TR("For a device whose label has one"));
    s_code.active = true;
    ui_zb_status_t st;
    if (!read_status(&st)) {
        ui_paragraph(body, TR("Zigbee isn't available on this panel."), ui_tone_gray());
        return;
    }
    if (!installer()) {
        ui_paragraph(body, TR("Adding a device is done in installer mode."), ui_tone_gray());
        return;
    }
    uint64_t ieee;
    int n;
    int need;
    const typed_t a = address_typed(s_code.addr, &ieee, &n);
    const typed_t c = code_typed(s_code.code, &n, &need);
    lv_obj_t *row = ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Its address"),
                                TR("16 characters on its label, often after MAC or IEEE"), "-",
                                UI_ICON_CHEVRON_RIGHT, type_address, NULL);
    /* TR: the value of a row not filled in yet. At most 23 bytes */
    set_words(row, "value_label", a == TYPED_OK ? s_code.addr : TR("Not typed"), COLOR_TEXT_2);
    row = ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_amber(), TR("Its install code"),
                      TR("On its label or box, often beside a QR code"), "-",
                      UI_ICON_CHEVRON_RIGHT, type_code, NULL);
    /* the code itself is never shown again once typed: only whether it checks out */
    set_words(row, "value_label",
              /* TR: the install code row's value once its check matches. At most 23 bytes */
              c == TYPED_OK ? TR_C("install code", "Checked") : TR("Not typed"),
              c == TYPED_OK ? ui_tone_green() : COLOR_TEXT_2);
    lv_obj_t *listen = ui_row_action(body, G_RADIO, TR("Listen for this device"), ui_tone_blue(),
                                     code_listen, NULL);
    if (a != TYPED_OK || c != TYPED_OK) {
        lv_obj_set_style_opa(listen, LV_OPA_40, 0); /* offered once both are typed and check out */
    }
    ui_paragraph(body,
                 /* TR: a paragraph on the install code page */
                 TR("Only this device may join while the panel listens. It joins with its own "
                    "key, not the standard one."),
                 ui_tone_gray());
    ui_paragraph(body,
                 /* TR: a paragraph on the install code page */
                 TR("A phone can scan the QR code instead, in the Panel Manager."), ui_tone_gray());
}

/* ---- install codes waiting (PAGE_ZIGBEE_CODES, the proposal's page 10) ---------------------- */

static struct {
    bool open;
    uint32_t sig;
    uint32_t polled;
    size_t n;
    ui_zb_code_t list[UI_ZB_CODES_MAX];
    uint64_t forget; /* the code the question is about */
} s_codes;

static size_t read_codes(ui_zb_code_t *out)
{
    if (S.be.zb_codes == NULL) {
        return 0u;
    }
    const size_t n = S.be.zb_codes(S.be.ctx, out, UI_ZB_CODES_MAX);
    return n < UI_ZB_CODES_MAX ? n : UI_ZB_CODES_MAX;
}

static uint32_t codes_sig(const ui_zb_code_t *list, size_t n)
{
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ (uint32_t)list[i].ieee) * 16777619u;
        h = (h ^ (uint32_t)(list[i].ieee >> 32)) * 16777619u;
        h = (h ^ list[i].added_at) * 16777619u;
    }
    return h ^ (installer() ? 1u : 0u);
}

static void codes_forget_go(void)
{
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_CODE_FORGET, 0u);
    req.ieee = s_codes.forget;
    if (request(&req)) {
        ui_page_rebuild(); /* it leaves the list: nothing else to say */
    }
}

static void codes_forget_ask(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i >= s_codes.n || !installer()) {
        return;
    }
    s_codes.forget = s_codes.list[i].ieee;
    char addr[24];
    char text[96];
    ieee_words(s_codes.forget, addr, sizeof(addr));
    /* TR: the Forget install code confirmation; {0} is the device's address */
    I18N_FORMAT(text, sizeof(text), TR("{0} can then join only with the standard key."), addr);
    /* TR: a confirmation's title on Install codes waiting */
    ui_confirm(TR("Forget this install code?"), text, TR_C("confirm button", "Forget"),
               codes_forget_go);
}

static void open_code_page(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZIGBEE_CODE, 0);
}

static void open_codes(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZIGBEE_CODES, 0);
}

static void build_codes(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen)
{
    (void)arg;
    s_codes.open = false;
    i18n_utf8_copy(title, tlen, TR("Install codes waiting"));
    /* TR: the subtitle of Install codes waiting */
    i18n_utf8_copy(note, nlen, TR("They may join whenever the panel listens"));
    s_codes.n = read_codes(s_codes.list);
    const bool can = installer();
    for (size_t i = 0; i < s_codes.n; i++) {
        char addr[24];
        char when[48];
        char detail[80];
        ieee_words(s_codes.list[i].ieee, addr, sizeof(addr));
        if (s_codes.list[i].added_at == 0u) {
            /* TR: under a waiting install code: it was added before the clock was set */
            i18n_utf8_copy(detail, sizeof(detail), TR("Added before the panel knew the time"));
        } else {
            const int64_t ms = (int64_t)s_codes.list[i].added_at * 1000;
            char clock[24];
            ui_time_or_day_text(ms, when, sizeof(when)); /* its time when it is today's */
            ui_clock_text(ms, clock, sizeof(clock), true);
            if (strcmp(when, clock) == 0) {
                /* TR: under a waiting install code; {0} a time like "10:41 AM" */
                I18N_FORMAT(detail, sizeof(detail), TR("Added today at {0}"), clock);
            } else {
                ui_date_time_text(ms, when, sizeof(when));
                /* TR: under a waiting install code; {0} a date and time like "Sep 28, 10:41 AM" */
                I18N_FORMAT(detail, sizeof(detail), TR("Added on {0}"), when);
            }
        }
        ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_amber(), addr, detail,
                    /* TR: a waiting install code's value. At most 23 bytes */
                    TR_C("install code", "Waiting"), can ? UI_ICON_CHEVRON_RIGHT : NULL,
                    can ? codes_forget_ask : NULL, (void *)(uintptr_t)i);
    }
    if (s_codes.n == 0u) {
        /* TR: a paragraph on Install codes waiting when there are none */
        ui_paragraph(body, TR("No install codes are waiting."), ui_tone_gray());
    }
    if (can) {
        /* TR: an action on Install codes waiting */
        ui_row_action(body, UI_ICON_PLUS, TR("Add an install code"), ui_tone_blue(), open_code_page,
                      NULL);
    }
    ui_paragraph(body,
                 /* TR: a paragraph on Install codes waiting */
                 TR("A device with its code here joins with its own key when the panel listens "
                    "for new devices. Joined, it leaves this list."),
                 ui_tone_gray());
    s_codes.sig = codes_sig(s_codes.list, s_codes.n);
    s_codes.open = true;
    s_codes.polled = lv_tick_get();
}

static void codes_tick(void)
{
    if (!s_codes.open || lv_tick_elaps(s_codes.polled) < POLL_MS) {
        return;
    }
    s_codes.polled = lv_tick_get();
    static ui_zb_code_t now[UI_ZB_CODES_MAX];
    const size_t n = read_codes(now);
    if (codes_sig(now, n) != s_codes.sig) {
        ui_page_rebuild(); /* one joined, or was given in the Panel Manager */
    }
}

/* ---- removing (PAGE_ZIGBEE_REMOVE, arg = its registry id; the proposal's page 6) ------------- */

static struct {
    uint16_t id;          /* the device asked to leave */
    char name[48];        /* as it was called: it is gone once removed */
    char model[UI_ZB_MODEL_LEN];
    bool sleepy;
    uint8_t zone_count;
    uint16_t zones[UI_ZB_ZONES_MAX];
    uint8_t op;           /* UI_ZB_OP_REMOVE, or _FORGET after "Forget it anyway" */
    bool active;          /* an operation of this page's runs, or ended unseen */
    bool open;
    uint32_t sig;
    uint32_t polled;
} s_rm;

static ui_zb_device_t *s_scratch; /* UI_ZB_DEVICES_MAX, from ui_slow_alloc: the pick sheet, lookups */

static ui_zb_device_t *scratch(void)
{
    if (s_scratch == NULL) {
        s_scratch = ui_slow_alloc(UI_ZB_DEVICES_MAX * sizeof(*s_scratch));
    }
    return s_scratch;
}

static bool rm_remember(uint16_t id)
{
    ui_zb_device_page_t page;
    if (!read_device(id, &page)) {
        ui_toast(refused_words(UI_ZB_REQ_NOT_FOUND));
        return false;
    }
    s_rm.id = id;
    device_name(&page.dev, s_rm.name, sizeof(s_rm.name));
    snprintf(s_rm.model, sizeof(s_rm.model), "%s", page.dev.model);
    s_rm.sleepy = page.sleepy;
    s_rm.zone_count = page.dev.zone_count < UI_ZB_ZONES_MAX ? page.dev.zone_count
                                                             : (uint8_t)UI_ZB_ZONES_MAX;
    memcpy(s_rm.zones, page.dev.zones, sizeof(s_rm.zones));
    return true;
}

/* "Zone 1 keeps its settings and needs a new sensor." after the first sentence; one sentence when
 * it feeds no zone. */
static void with_zones_sentence(const char *first, const uint16_t *zones, uint8_t count,
                                char *out, size_t len)
{
    if (count == 0u) {
        i18n_utf8_copy(out, len, first);
        return;
    }
    char words[32];
    char second[96];
    zones_words(zones, count, words, sizeof(words));
    /* TR: the Remove and Forget confirmations' second sentence; {0} is "Zone 8" or "Zones 6, 9".
     * At most 95 bytes */
    I18N_FORMAT(second, sizeof(second),
                TRN("{0} keeps its settings and needs a new sensor.",
                    "{0} keep their settings and need a new sensor.", count),
                words);
    I18N_FORMAT(out, len, TR_C("two sentences", "{0} {1}"), first, second);
}

static void rm_start(uint8_t kind)
{
    ui_zb_request_t req;
    req_init(&req, kind, s_rm.id);
    if (request(&req)) {
        s_rm.op = kind == UI_ZB_REQ_FORGET ? UI_ZB_OP_FORGET : UI_ZB_OP_REMOVE;
        s_rm.active = true;
    }
}

static void remove_go(void)
{
    /* from a device's page, the remove page takes its place: back from it is the Zigbee page */
    if (S.depth > 0u && S.pages[S.depth - 1u].id == PAGE_ZIGBEE_DEVICE) {
        ui_page_back();
    }
    rm_start(UI_ZB_REQ_REMOVE);
    if (s_rm.active) {
        ui_page_open(PAGE_ZIGBEE_REMOVE, (int32_t)s_rm.id);
    }
}

/* "Remove Front Door?" (the proposal's remove-confirm). */
static void remove_ask_for(uint16_t id)
{
    if (!installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    if (!rm_remember(id)) {
        return;
    }
    char title[96];
    static char text[UI_SHEET_NOTE_LENGTH];
    /* TR: a confirmation's title; {0} is a device's name. At most 95 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Remove {0}?"), s_rm.name);
    /* TR: the Remove confirmation's first sentence. At most 95 bytes */
    with_zones_sentence(TR("It is asked to leave the network."), s_rm.zones, s_rm.zone_count, text,
                        sizeof(text));
    ui_confirm(title, text, TR_C("confirm button", "Remove"), remove_go);
}

static void remove_this(void *ctx)
{
    (void)ctx;
    remove_ask_for(s_dev.id);
}

static uint16_t s_pick_ids[UI_ZB_DEVICES_MAX];

static void remove_picked(int32_t index)
{
    if (index >= 0 && (size_t)index < UI_ZB_DEVICES_MAX && s_pick_ids[index] != 0u) {
        remove_ask_for(s_pick_ids[index]);
    }
}

/* "Remove a device": the devices to pick from (the proposal's remove-pick). Unlike Z-Wave nothing
 * is pressed first: the panel asks the one chosen to leave. */
static void remove_pick(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[UI_ZB_DEVICES_MAX] UI_SLOW_BSS;
    static char titles[UI_ZB_DEVICES_MAX][48] UI_SLOW_BSS;
    static char details[UI_ZB_DEVICES_MAX][32] UI_SLOW_BSS;
    ui_zb_device_t *devs = scratch();
    if (devs == NULL || !installer()) {
        return;
    }
    const size_t n = read_devices(devs);
    if (n == 0u) {
        /* TR: a toast when there is nothing to remove */
        ui_toast(TR("No devices to remove"));
        return;
    }
    memset(s_pick_ids, 0, sizeof(s_pick_ids));
    for (size_t i = 0; i < n; i++) {
        char said[64];
        lv_color_t said_color;
        device_sub(&devs[i], said, sizeof(said), &said_color);
        s_pick_ids[i] = devs[i].id;
        device_name(&devs[i], titles[i], sizeof(titles[i]));
        value_words(&devs[i], details[i], sizeof(details[i]));
        choices[i].glyph = device_glyph(&devs[i]);
        choices[i].tone = said[0] != '\0' && lv_color_eq(said_color, ui_tone_amber())
                              ? ui_tone_amber()
                              : ui_tone_green();
        choices[i].title = titles[i];
        choices[i].detail = details[i];
    }
    /* TR: the pick sheet's title and note for removing a Zigbee device */
    ui_pick_sheet(TR("Remove a device"), TR("It is asked to leave the network"), choices, n, -1,
                  remove_picked);
}

static void rm_done(void *ctx)
{
    (void)ctx;
    ui_page_back(); /* leaving dismisses what it showed (stop_if_leaving) */
}

static void rm_another(void *ctx)
{
    (void)ctx;
    ui_page_back();
    if (S.depth > 0u && S.pages[S.depth - 1u].id == PAGE_ZIGBEE) {
        remove_pick(NULL);
    }
}

static void rm_stop(void *ctx)
{
    (void)ctx;
    ui_page_back(); /* leaving while it waits stops waiting: it stays in the network */
}

static void rm_retry(void *ctx)
{
    (void)ctx;
    ask_quietly(UI_ZB_REQ_DISMISS);
    s_rm.active = false;
    rm_start(UI_ZB_REQ_REMOVE);
    ui_page_rebuild();
}

static void rm_forget_go(void)
{
    ask_quietly(UI_ZB_REQ_DISMISS);
    s_rm.active = false;
    rm_start(UI_ZB_REQ_FORGET);
    ui_page_rebuild();
}

/* "Forget it anyway" asks once more, as the device page's Forget does. */
static void rm_forget_ask(void *ctx)
{
    (void)ctx;
    char title[96];
    static char text[UI_SHEET_NOTE_LENGTH];
    /* TR: a confirmation's title; {0} is a device's name. At most 95 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Forget {0}?"), s_rm.name);
    /* TR: the Forget confirmation's first sentence. At most 95 bytes */
    with_zones_sentence(TR("Only when it's gone or broken: it isn't asked to leave."), s_rm.zones,
                        s_rm.zone_count, text, sizeof(text));
    ui_confirm(title, text, TR("Forget it"), rm_forget_go);
}

static bool rm_ours(const ui_zb_progress_t *p)
{
    return s_rm.active && p->op == s_rm.op && p->id == s_rm.id && p->step != UI_ZB_STEP_IDLE;
}

static uint32_t rm_sig(const ui_zb_progress_t *p)
{
    const bool ours = rm_ours(p);
    return (ours ? 1u : 0u) | (ours && p->step == UI_ZB_STEP_DONE ? 2u : 0u) |
           (ours ? (uint32_t)p->result << 2 : 0u) | (uint32_t)s_rm.op << 8 |
           (installer() ? 1u << 16 : 0u);
}

/* "Waiting · 1 min 40 s left": a sleepy device answers when it wakes, the panel waits 2 minutes. */
static void rm_note(const ui_zb_progress_t *p, char *note, size_t nlen)
{
    if (p->seconds_left >= 60u) {
        char secs[4];
        snprintf(secs, sizeof(secs), "%02u", (unsigned)(p->seconds_left % 60u));
        /* TR: the remove page's subtitle; {0} minutes, {1} seconds ("05"). At most 63 bytes */
        I18N_FORMAT(note, nlen, TR("Waiting · {0} min {1} s left"),
                    (unsigned)(p->seconds_left / 60u), secs);
    } else if (p->seconds_left > 0u) {
        /* TR: the remove page's subtitle; {0} seconds. At most 63 bytes */
        I18N_FORMAT(note, nlen,
                    TRN("Waiting · {0} s left", "Waiting · {0} s left", (unsigned)p->seconds_left),
                    (unsigned)p->seconds_left);
    } else {
        /* TR: the remove page's subtitle while the device is asked. At most 63 bytes */
        i18n_utf8_copy(note, nlen, TR_C("Zigbee remove", "Waiting"));
    }
}

/* "Aqara door sensor · Zone 1": what was asked to leave. */
static void rm_detail(char *out, size_t len)
{
    char zones[32];
    snprintf(out, len, "%s", s_rm.model);
    zones_words(s_rm.zones, s_rm.zone_count, zones, sizeof(zones));
    join(out, len, zones);
}

static void rm_ended(const ui_zb_progress_t *p, lv_obj_t *body, char *note, size_t nlen)
{
    char title[96];
    char detail[96];
    char text[160];
    switch (p->result) {
    case UI_ZB_RES_REMOVED:
    case UI_ZB_RES_FORGOTTEN: {
        const bool forgot = p->result == UI_ZB_RES_FORGOTTEN;
        /* TR: the remove page's subtitle once it ended */
        i18n_utf8_copy(note, nlen, TR_C("Zigbee remove", "Done"));
        if (forgot) {
            /* TR: the box on the remove page after Forget; {0} the device's name */
            I18N_FORMAT(title, sizeof(title), TR("Forgotten: {0}"), s_rm.name);
            i18n_utf8_copy(detail, sizeof(detail), TR("Removed without its answer"));
        } else {
            /* TR: the box on the remove page; {0} the device's name */
            I18N_FORMAT(title, sizeof(title), TR("Removed: {0}"), s_rm.name);
            /* TR: under "Removed: ..." on the remove page; {0} the device's model */
            I18N_FORMAT(detail, sizeof(detail), TR("{0} left the network"), s_rm.model);
        }
        ui_row_link(body, UI_ICON_CIRCLE_CHECK, ui_tone_green(), title, detail, NULL, NULL, NULL,
                    NULL);
        const uint8_t count = p->zone_count > 0u ? p->zone_count : s_rm.zone_count;
        const uint16_t *zones = p->zone_count > 0u ? p->zones : s_rm.zones;
        if (count > 0u) {
            char words[32];
            zones_words(zones, count, words, sizeof(words));
            /* TR: on the remove page; {0} is "Zone 1" or "Zones 6, 9" */
            I18N_FORMAT(text, sizeof(text),
                        TRN("{0} needs a new sensor: it shows CHECK until it has one.",
                            "{0} need a new sensor: they show CHECK until they have one.", count),
                        words);
            ui_paragraph(body, text, ui_tone_amber());
        }
        /* TR: an action on the remove page */
        ui_action_pair(body, UI_ICON_TRASH, TR("Remove another"), ui_tone_gray(), rm_another,
                       UI_ICON_CHECK, TR("Done"), ui_tone_blue(), rm_done, NULL);
        break;
    }
    case UI_ZB_RES_NO_ANSWER:
        /* TR: the remove page's subtitle when the device didn't answer */
        i18n_utf8_copy(note, nlen, TR("No answer"));
        /* TR: under "It didn't answer"; {0} the device's name */
        I18N_FORMAT(detail, sizeof(detail), TR("{0} is still in the network"), s_rm.name);
        /* TR: the box on the remove page when the device didn't answer */
        ui_row_link(body, UI_ICON_TRIANGLE_ALERT, ui_tone_amber(), TR("It didn't answer"), detail,
                    NULL, NULL, NULL, NULL);
        ui_paragraph(body,
                     /* TR: a paragraph on the remove page when the device didn't answer */
                     TR("If it is gone or broken it can't answer. Forget it anyway? The panel stops "
                        "trusting it."),
                     ui_tone_gray());
        ui_action_pair(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), rm_retry,
                       UI_ICON_UNLINK, TR("Forget it anyway"), ui_tone_red(), rm_forget_ask, NULL);
        break;
    default: /* stopped, or it couldn't be done: it stays in the network */
        i18n_utf8_copy(note, nlen, TR("Not removed"));
        I18N_FORMAT(detail, sizeof(detail), TR("{0} is still in the network"), s_rm.name);
        /* TR: the box on the remove page when it couldn't be done */
        ui_row_link(body, UI_ICON_CIRCLE_X, ui_tone_red(), TR("Not removed"), detail, NULL, NULL,
                    NULL, NULL);
        ui_row_action(body, UI_ICON_CHECK, TR("Done"), ui_tone_blue(), rm_done, NULL);
        break;
    }
}

static void build_remove(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    s_rm.open = false;
    if ((uint16_t)arg != s_rm.id) {
        (void)rm_remember((uint16_t)arg); /* opened without the question: nothing runs */
    }
    /* TR: the remove page's title; {0} the device's name. At most 47 bytes */
    I18N_FORMAT(title, tlen, TR("Remove {0}"), s_rm.name);
    if (!installer()) {
        /* TR: a paragraph on the remove page outside installer mode */
        ui_paragraph(body, TR("Removing a device is done in installer mode."), ui_tone_gray());
        return;
    }
    ui_zb_progress_t p;
    read_progress(&p);
    if (!rm_ours(&p)) {
        s_rm.active = false;
        /* TR: a paragraph on the remove page when nothing runs */
        ui_paragraph(body, TR("Nothing is being removed now."), ui_tone_gray());
        ui_row_action(body, UI_ICON_CHECK, TR("Done"), ui_tone_blue(), rm_done, NULL);
    } else if (p.step != UI_ZB_STEP_DONE) {
        char detail[96];
        rm_note(&p, note, nlen);
        rm_detail(detail, sizeof(detail));
        ui_row_link(body, UI_ICON_CLOCK, ui_tone_blue(),
                    s_rm.op == UI_ZB_OP_FORGET
                        /* TR: the box on the remove page while a device is forgotten */
                        ? TR("Forgetting it")
                        /* TR: the box on the remove page while the device is asked to leave */
                        : TR("Asked it to leave"),
                    detail, NULL, NULL, NULL, NULL);
        if (s_rm.sleepy && s_rm.op == UI_ZB_OP_REMOVE) {
            ui_paragraph(body,
                         /* TR: a paragraph on the remove page for a battery device */
                         TR("A sleeping sensor answers when it wakes: press its button now."),
                         ui_tone_gray());
        }
        /* TR: an action on the remove page while it waits */
        ui_row_action(body, G_X, TR("Stop waiting"), ui_tone_gray(), rm_stop, NULL);
    } else {
        rm_ended(&p, body, note, nlen);
    }
    s_rm.sig = rm_sig(&p);
    s_rm.open = true;
    s_rm.polled = lv_tick_get();
}

static void rm_tick(void)
{
    if (!s_rm.open || lv_tick_elaps(s_rm.polled) < POLL_MS) {
        return;
    }
    s_rm.polled = lv_tick_get();
    ui_zb_progress_t p;
    read_progress(&p);
    if (rm_sig(&p) != s_rm.sig) {
        ui_page_rebuild();
        return;
    }
    if (rm_ours(&p) && p.step != UI_ZB_STEP_DONE) {
        char note[64];
        rm_note(&p, note, sizeof(note)); /* the countdown, once a second */
        ui_page_note(note);
    }
}

/* Leaving the remove page: a wait still running stops, an ending shown is dismissed. */
static void rm_leave(void)
{
    ui_zb_progress_t p;
    read_progress(&p);
    if (rm_ours(&p) && p.step != UI_ZB_STEP_DONE) {
        ask_quietly(UI_ZB_REQ_STOP);
        read_progress(&p);
    }
    if (rm_ours(&p) && p.step == UI_ZB_STEP_DONE) {
        ask_quietly(UI_ZB_REQ_DISMISS);
    }
    s_rm.active = false;
}

/* ---- the radio's details (PAGE_ZIGBEE_RADIO, the proposal's page 7) ------------------------ */

static struct {
    bool open;
    lv_obj_t *state;
    lv_obj_t *noise;
    lv_obj_t *restarts;
    uint32_t shape;
    uint32_t polled;
} s_radio;

static const char *radio_state_word(const ui_zb_status_t *st)
{
    switch (st->radio) {
    case UI_ZB_RADIO_WORKING:
        return st->jammed ? TR_C("radio state", "Interference") : TR_C("radio state", "Working");
    case UI_ZB_RADIO_STARTING: return TR_C("radio state", "Starting");
    case UI_ZB_RADIO_NOT_ANSWERING: return TR_C("radio state", "Not answering");
    case UI_ZB_RADIO_PAUSED: return TR_C("radio state", "Paused after restarts");
    case UI_ZB_RADIO_SOCKET_ZWAVE: return TR_C("radio state", "Socket used by Z-Wave");
    case UI_ZB_RADIO_UNUSABLE: return TR_C("radio state", "Can't be used");
    default: return TR_C("radio state", "Not connected");
    }
}

static void noise_words(const ui_zb_status_t *st, char *out, size_t len)
{
    if (st->noise_known) {
        /* TR: the radio's background noise in dBm, a negative number. At most 23 bytes */
        I18N_FORMAT(out, len, TR("{0} dBm"), (int)st->noise_dbm);
    } else {
        /* TR: a value on the radio's page before it was measured. At most 31 bytes */
        i18n_utf8_copy(out, len, TR("Not measured yet"));
    }
}

static void restarts_words(const ui_zb_status_t *st, char *out, size_t len)
{
    if (st->radio == UI_ZB_RADIO_PAUSED) {
        /* TR: the radio page's "Panel restarts it caused" when it is paused; {0} how many (2) */
        I18N_FORMAT(out, len, TR("{0}: paused"), (unsigned)st->restarts);
    } else {
        /* TR: the radio page's "Panel restarts it caused"; {0} how many so far (0 or 1) */
        I18N_FORMAT(out, len, TR("{0} (it pauses at 2)"), (unsigned)st->restarts);
    }
}

static uint32_t radio_shape(const ui_zb_status_t *st)
{
    return (st->radio == UI_ZB_RADIO_PAUSED ? 1u : 0u) | (installer() ? 2u : 0u) |
           (st->network_known ? 4u : 0u) | (uint32_t)st->channel << 8;
}

static void restart_go(void)
{
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_RESTART, 0u);
    (void)request(&req); /* the State row follows it: Starting, then Working */
}

static void restart_ask(void *ctx)
{
    (void)ctx;
    if (!installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    /* TR: a confirmation on the Zigbee radio's page */
    ui_confirm(TR("Restart the Zigbee radio?"), TR("Takes a few seconds; zones keep their state."),
               TR("Restart"), restart_go);
}

static void open_radio(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZIGBEE_RADIO, 0);
}

static void build_radio(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen)
{
    (void)arg;
    s_radio.open = false;
    /* TR: a page's title: the Zigbee radio's numbers. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Radio details"));
    /* TR: the radio details page's subtitle */
    i18n_utf8_copy(note, nlen, TR("The panel's Zigbee radio"));
    ui_zb_status_t st;
    if (!read_status(&st)) {
        ui_paragraph(body, TR("Zigbee isn't available on this panel."), ui_tone_gray());
        return;
    }
    char text[48];
    s_radio.state = ui_row_value(body, TR("State"), radio_state_word(&st));
    ui_row_value(body, TR("Radio software"),
                 st.radio_version[0] != '\0' ? st.radio_version : TR("Not known yet"));
    snprintf(text, sizeof(text), "-");
    if (st.network_known && st.channel != 0u) {
        snprintf(text, sizeof(text), "%u", (unsigned)st.channel);
    }
    ui_row_value(body, TR_C("Zigbee settings", "Channel"), text);
    snprintf(text, sizeof(text), "%04X", (unsigned)st.pan_id);
    /* TR: a row on the Zigbee radio's page: its network's short id (PAN ID) */
    ui_row_value(body, TR("Network id (PAN)"), st.network_known ? text : "-");
    colon_words(st.ext_pan_id, text, sizeof(text));
    /* TR: a row on the Zigbee radio's page: its network's extended id */
    ui_row_value(body, TR("Extended id"), st.network_known ? text : "-");
    colon_words(st.panel_ieee, text, sizeof(text));
    /* TR: a row on the Zigbee radio's page: the panel's own 64-bit address */
    ui_row_value(body, TR("The panel's Zigbee address"), st.panel_ieee != 0u ? text : "-");
    noise_words(&st, text, sizeof(text));
    /* TR: a row on the Zigbee radio's page */
    s_radio.noise = ui_row_value(body, TR("Background noise"), text);
    restarts_words(&st, text, sizeof(text));
    /* TR: a row on the Zigbee radio's page: how many panel restarts the radio caused */
    s_radio.restarts = ui_row_value(body, TR("Panel restarts it caused"), text);
    if (!installer()) {
        ui_row_link(body, UI_ICON_REFRESH, ui_tone_gray(), TR("Restart the radio"),
                    TR("Done in installer mode"), NULL, NULL, NULL, NULL);
    } else if (st.radio == UI_ZB_RADIO_PAUSED) {
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), retry_radio, NULL);
    } else {
        ui_row_link(body, UI_ICON_REFRESH, ui_tone_blue(), TR("Restart the radio"),
                    TR("Takes a few seconds; zones keep their state"), NULL, NULL, restart_ask,
                    NULL);
    }
    s_radio.shape = radio_shape(&st);
    s_radio.open = true;
    s_radio.polled = lv_tick_get();
}

static void radio_tick(void)
{
    if (!s_radio.open || lv_tick_elaps(s_radio.polled) < POLL_MS) {
        return;
    }
    s_radio.polled = lv_tick_get();
    ui_zb_status_t st;
    if (!read_status(&st)) {
        return;
    }
    if (radio_shape(&st) != s_radio.shape) {
        ui_page_rebuild();
        return;
    }
    char text[48];
    set_words(s_radio.state, "value_label", radio_state_word(&st), COLOR_TEXT_2);
    noise_words(&st, text, sizeof(text));
    set_words(s_radio.noise, "value_label", text, COLOR_TEXT_2);
    restarts_words(&st, text, sizeof(text));
    set_words(s_radio.restarts, "value_label", text, COLOR_TEXT_2);
}

/* ---- the channel (PAGE_ZIGBEE_CHANNEL, the proposal's page 8) ------------------------------- */

/* The channels the panel offers, clear of the usual Wi-Fi ones (ui_zb_status_t.survey_dbm's). */
static const uint8_t CHANNELS[UI_ZB_CHANNELS] = {11u, 15u, 20u, 25u};

static struct {
    bool open;
    uint8_t op;      /* UI_ZB_OP_CHANNEL or _SURVEY this page asked for; NONE none */
    uint8_t target;  /* the channel a move goes to */
    uint32_t sig;
    uint32_t polled;
} s_chan;

typedef enum { NOISE_UNKNOWN = 0, NOISE_QUIET, NOISE_SOME, NOISE_BUSY } noise_t;

/* Words, not dBm (the drawing): busy at -75 dBm and louder, some noise to -85 dBm, else quiet. */
static noise_t channel_noise(const ui_zb_status_t *st, size_t i)
{
    if ((st->survey_known & (1u << i)) == 0u) {
        return NOISE_UNKNOWN;
    }
    return st->survey_dbm[i] >= -75 ? NOISE_BUSY : st->survey_dbm[i] >= -85 ? NOISE_SOME : NOISE_QUIET;
}

static uint32_t chan_sig(const ui_zb_status_t *st, const ui_zb_progress_t *p)
{
    uint32_t h = 2166136261u;
    const bool running = s_chan.op != UI_ZB_OP_NONE && p->op == s_chan.op &&
                         p->step != UI_ZB_STEP_DONE && p->step != UI_ZB_STEP_IDLE;
    const uint32_t parts[] = {st->channel, st->quietest, running ? 1u : 0u,
                              installer() ? 1u : 0u, st->radio};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        h = (h ^ parts[i]) * 16777619u;
    }
    for (size_t i = 0; i < UI_ZB_CHANNELS; i++) {
        h = (h ^ (uint32_t)channel_noise(st, i)) * 16777619u;
    }
    return h;
}

static void channel_go(void)
{
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_CHANNEL, 0u);
    req.channel = s_chan.target;
    if (request(&req)) {
        s_chan.op = UI_ZB_OP_CHANNEL;
        ui_page_rebuild();
    }
}

static void channel_ask(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i >= UI_ZB_CHANNELS || !installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    s_chan.target = CHANNELS[i];
    char title[64];
    /* TR: a confirmation's title; {0} a Zigbee channel, 11 to 26 */
    I18N_FORMAT(title, sizeof(title), TR("Move to channel {0}?"), (unsigned)CHANNELS[i]);
    ui_confirm(title,
               /* TR: the channel move confirmation's text */
               TR("Every device follows. Sleeping ones move when they next check in; some may "
                  "need to be added again."),
               /* TR: the channel move confirmation's button */
               TR_C("confirm button", "Move"), channel_go);
}

static void survey_go(void *ctx)
{
    (void)ctx;
    if (!installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_SURVEY, 0u);
    if (request(&req)) {
        s_chan.op = UI_ZB_OP_SURVEY;
        ui_page_rebuild();
    }
}

static void open_channel(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZIGBEE_CHANNEL, 0);
}

static void build_channel(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                          size_t nlen)
{
    (void)arg;
    s_chan.open = false;
    i18n_utf8_copy(title, tlen, TR_C("Zigbee settings", "Channel"));
    ui_zb_status_t st;
    if (!read_status(&st)) {
        ui_paragraph(body, TR("Zigbee isn't available on this panel."), ui_tone_gray());
        return;
    }
    ui_zb_progress_t p;
    read_progress(&p);
    i18n_utf8_copy(note, nlen,
                   st.survey_known != 0u
                       /* TR: the channel page's subtitle after a noise check */
                       ? TR("The quietest is marked, from the latest noise check")
                       /* TR: the channel page's subtitle before any noise check */
                       : TR("Not checked for noise yet"));
    const bool can = installer();
    const bool running = s_chan.op != UI_ZB_OP_NONE && p.op == s_chan.op &&
                         p.step != UI_ZB_STEP_DONE && p.step != UI_ZB_STEP_IDLE;
    for (size_t i = 0; i < UI_ZB_CHANNELS; i++) {
        const noise_t noise = channel_noise(&st, i);
        const bool now = st.network_known && st.channel == CHANNELS[i];
        const bool quietest = st.quietest == CHANNELS[i] && !now;
        char name[24];
        I18N_FORMAT(name, sizeof(name), TR("Channel {0}"), (unsigned)CHANNELS[i]);
        const char *words = noise == NOISE_BUSY ? TR_C("channel noise", "Busy")
                            : noise == NOISE_SOME ? TR_C("channel noise", "Some noise")
                            : noise == NOISE_QUIET
                                ? TR_C("channel noise", "Quiet")
                                /* TR: a channel's line before any noise check */
                                : TR_C("channel noise", "Not checked yet");
        const lv_color_t tone = noise == NOISE_BUSY    ? ui_tone_gray()
                                : noise == NOISE_QUIET ? ui_tone_green()
                                                       : ui_tone_blue();
        const bool tap = can && !now && !running;
        lv_obj_t *row = ui_row_link(body, UI_ICON_WAVES, tone, name, words,
                                    /* TR: the channel in use, a row's value. At most 15 bytes */
                                    now ? TR_C("channel", "Now")
                                    /* TR: the quietest channel, a row's value. At most 15 bytes */
                                    : quietest ? TR_C("channel", "Quietest")
                                               : NULL,
                                    now ? UI_ICON_CHECK : NULL, tap ? channel_ask : NULL,
                                    (void *)(uintptr_t)i);
        if (quietest) {
            set_words(row, "value_label", TR_C("channel", "Quietest"), ui_tone_green());
        }
    }
    if (running) {
        char text[96];
        if (s_chan.op == UI_ZB_OP_CHANNEL) {
            /* TR: a paragraph on the channel page during a move; {0} the new channel */
            I18N_FORMAT(text, sizeof(text), TR("Moving to channel {0}: every device follows."),
                        (unsigned)s_chan.target);
        } else {
            /* TR: a paragraph on the channel page while each channel is measured */
            i18n_utf8_copy(text, sizeof(text), TR("Checking each channel for noise."));
        }
        ui_paragraph(body, text, ui_tone_blue());
    }
    ui_paragraph(body,
                 /* TR: a paragraph on the channel page */
                 TR("Sleeping devices move when they next check in, within 2 hours. Some may need "
                    "to be added again."),
                 ui_tone_amber());
    if (can) {
        ui_row_link(body, UI_ICON_ACTIVITY, ui_tone_blue(),
                    /* TR: a row on the channel page */
                    TR("Check for noise again"),
                    /* TR: under "Check for noise again" */
                    TR("Sensors aren't heard for a few seconds"), NULL, NULL,
                    running ? NULL : survey_go, NULL);
    }
    s_chan.sig = chan_sig(&st, &p);
    s_chan.open = true;
    s_chan.polled = lv_tick_get();
}

static void chan_tick(void)
{
    if (!s_chan.open || lv_tick_elaps(s_chan.polled) < POLL_MS) {
        return;
    }
    s_chan.polled = lv_tick_get();
    ui_zb_progress_t p;
    read_progress(&p);
    if (s_chan.op != UI_ZB_OP_NONE && p.op == s_chan.op && p.step == UI_ZB_STEP_DONE) {
        const uint8_t result = p.result;
        s_chan.op = UI_ZB_OP_NONE;
        ask_quietly(UI_ZB_REQ_DISMISS);
        if (result == UI_ZB_RES_FAILED) {
            /* TR: a toast when a channel move or noise check couldn't be done */
            ui_toast(TR("The radio couldn't do it"));
        }
        read_progress(&p);
    }
    ui_zb_status_t st;
    if (read_status(&st) && chan_sig(&st, &p) != s_chan.sig) {
        ui_page_rebuild();
    }
}

/* ---- reset (the Zigbee page's own row; the proposal's page 9) -------------------------------- */

static uint8_t s_net_op; /* UI_ZB_OP_RESET while the page's reset runs; its end is dismissed */

/* The installer code once more (ZB-017): the backend checks it with the engine before anything
 * else, as it counts toward the lockout; the screen never keeps it (ask() wipes the request). */
static bool reset_code(const char *code)
{
    ui_zb_request_t req;
    req_init(&req, UI_ZB_REQ_RESET, 0u);
    snprintf(req.installer_code, sizeof(req.installer_code), "%s", code);
    const ui_zb_req_result_t r = ask(&req);
    switch (r) {
    case UI_ZB_REQ_OK: /* true: the pad closes */
        s_net_op = UI_ZB_OP_RESET; /* the page follows it: Radio · Working, No devices */
        return true;
    case UI_ZB_REQ_WRONG_CODE:
        /* a master or user code is refused like a wrong one */
        ui_pad_error(TR("Wrong code, try again"));
        return false;
    case UI_ZB_REQ_LOCKED_OUT:
        /* TR: a toast when the reset's code was refused too often */
        ui_toast(TR("Too many wrong codes: try again later"));
        return true;
    default:
        ui_toast(refused_words(r));
        return true;
    }
}

static void reset_ask_code(void)
{
    ui_pad_ask(HOST_MODAL, UI_ICON_ROTATE_CCW,
               /* TR: the code pad's title for the Zigbee network's reset */
               TR("Confirm the reset"), TR("Enter the installer code once more"), ui_tone_red(),
               reset_code);
}

/* ZB-017: the second confirmation, as the Panel Manager asks it; then the installer code. */
static void reset_ask_again(void)
{
    /* TR: the Zigbee reset's second confirmation, after "Continue" */
    ui_confirm(TR("Reset it now?"),
               /* TR: the Zigbee reset's second confirmation. At most 127 bytes */
               TR("This can't be undone. Install codes waiting are kept."), TR("Reset the network"),
               reset_ask_code);
}

static void reset_tap(void *ctx)
{
    (void)ctx;
    ui_zb_status_t st;
    if (!read_status(&st) || !installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    char text[160];
    if (st.devices == 0u) {
        /* TR: the Zigbee reset confirmation when no device has joined. At most 127 bytes */
        i18n_utf8_copy(text, sizeof(text),
                       TR("The panel starts a new Zigbee network, with new keys. No devices have "
                          "joined, so nothing else changes."));
    } else {
        /* TR: the Zigbee reset confirmation; {0} the number of devices. At most 127 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("{0} device loses the panel. It must be reset and added again, and its "
                        "zones show CHECK until then.",
                        "All {0} devices lose the panel. Each one must be reset and added again, "
                        "and their zones show CHECK until then.",
                        (unsigned)st.devices),
                    (unsigned)st.devices);
    }
    /* TR: a confirmation's title on the Zigbee page */
    ui_confirm(TR("Reset the Zigbee network?"), text, TR("Continue"), reset_ask_again);
}

/* The reset's end, once seen: nothing else needs saying - the page shows it. */
static void net_op_tick(void)
{
    if (s_net_op == UI_ZB_OP_NONE) {
        return;
    }
    ui_zb_progress_t p;
    read_progress(&p);
    if (p.op != s_net_op) {
        s_net_op = UI_ZB_OP_NONE;
        return;
    }
    if (p.step == UI_ZB_STEP_DONE) {
        const uint8_t result = p.result;
        s_net_op = UI_ZB_OP_NONE;
        ask_quietly(UI_ZB_REQ_DISMISS);
        if (result == UI_ZB_RES_FAILED) {
            ui_toast(TR("The radio couldn't do it"));
        }
    }
}

/* ---- the zone editor and the Troubles list (the proposal's pages 11 to 13) --------------------- */

/* The device whose input this is (by its address on the label: one device, any endpoint). */
static bool device_at(const zsrc_address_t *addr, ui_zb_device_page_t *out)
{
    ui_zb_device_t *devs = scratch();
    if (addr->kind != ZSRC_ZIGBEE || devs == NULL || S.be.zb_devices == NULL ||
        !ui_zigbee_available()) {
        return false;
    }
    size_t n = S.be.zb_devices(S.be.ctx, devs, UI_ZB_DEVICES_MAX);
    n = n < UI_ZB_DEVICES_MAX ? n : UI_ZB_DEVICES_MAX;
    for (size_t i = 0; i < n; i++) {
        if (read_device(devs[i].id, out) && out->ieee == addr->id) {
            return true;
        }
    }
    return false;
}

bool ui_zigbee_source_words(const zsrc_address_t *addr, char *out, size_t len)
{
    ui_zb_device_page_t page;
    if (!device_at(addr, &page)) {
        return false;
    }
    char name[48];
    if (page.dev.model[0] != '\0') {
        snprintf(name, sizeof(name), "%s", page.dev.model);
    } else {
        device_name(&page.dev, name, sizeof(name));
    }
    snprintf(out, len, "%s · %s", NOTR("Zigbee") /* a protocol name */, name);
    return true;
}

void ui_zigbee_zone_device_row(lv_obj_t *body, const zsrc_address_t *addr)
{
    ui_zb_device_page_t page;
    if (!device_at(addr, &page)) {
        return;
    }
    const ui_zb_device_t *d = &page.dev;
    char line[96] = "";
    char part[48];
    power_fact(d, part, sizeof(part));
    join(line, sizeof(line), part);
    join(line, sizeof(line), signal_fact(d->signal));
    if (d->heard_s == UI_ZB_NEVER) {
        join(line, sizeof(line), TR("Not heard yet"));
    } else {
        char span[24];
        span_words(d->heard_s, span, sizeof(span));
        /* TR: a Zigbee device's line on the zone page; {0} how long, like "2 min". At most 24
         * bytes */
        I18N_FORMAT(part, sizeof(part), TR("Heard {0} ago"), span);
        join(line, sizeof(line), part);
    }
    const bool warn = d->unreachable || d->battery_low || d->signal == UI_ZB_SIGNAL_WEAK;
    /* the facts as the second line: a long value would push the title */
    ui_row_link(body, G_RADIO, warn ? ui_tone_amber() : ui_tone_green(),
                /* TR: the zone page's row of the Zigbee device that feeds it */
                TR("Zigbee device"), line, NULL, UI_ICON_CHEVRON_RIGHT, open_device,
                (void *)(uintptr_t)d->id);
}

static void picker_add(void *ctx)
{
    (void)ctx;
    if (!installer()) {
        say_why((void *)(uintptr_t)0u);
        return;
    }
    ui_page_open(PAGE_ZIGBEE_ADD, 1); /* a device ready goes straight back to the picker */
}

void ui_zigbee_picker_row(lv_obj_t *body)
{
    if (!ui_zigbee_available()) {
        return;
    }
    ui_row_link(body, UI_ICON_PLUS, ui_tone_green(),
                /* TR: the first row of the zone editor's Zigbee sensor picker */
                TR("Add a new Zigbee device"),
                /* TR: under "Add a new Zigbee device" */
                TR("Join it now, then pick its input"), NULL, UI_ICON_CHEVRON_RIGHT, picker_add,
                NULL);
}

/* The Zigbee radio has the problem the sensor radio trouble is about. */
static bool radio_in_trouble(const ui_zb_status_t *st)
{
    return st->jammed || st->radio == UI_ZB_RADIO_NOT_ANSWERING ||
           st->radio == UI_ZB_RADIO_PAUSED || st->radio == UI_ZB_RADIO_SOCKET_ZWAVE ||
           st->radio == UI_ZB_RADIO_UNUSABLE;
}

bool ui_zigbee_trouble_line(alarm_sys_trouble_t t, ui_line_t *line)
{
    ui_zb_status_t st;
    if (t != ALARM_TRBL_SENSOR_RADIO || !read_status(&st) || !radio_in_trouble(&st)) {
        return false;
    }
    /* TR: the second line of the sensor radio trouble when it opens the Zigbee page. At most 95
     * bytes */
    i18n_utf8_copy(line->detail, sizeof(line->detail),
                   TR("Wireless sensors may not be heard · Tap to see the radio"));
    return true;
}

/* The trouble opens the Zigbee page on top of the installer menu, its back arrow to the menu as
 * drawn (ZB-027). Looking needs installer mode (the radio's page shows the network's addresses):
 * outside it, the trouble opens the locked Installer page, whose row takes the installer code as
 * always, and a toast says why; once the code is taken the Zigbee page follows (trouble_follow). */
static bool s_after_code;

static void trouble_open(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
    ui_settings_section(NOTR("installer")); /* an id, not words on the screen */
    if (S.program) {
        s_after_code = false;
        ui_page_open(PAGE_ZIGBEE, 0);
        return;
    }
    s_after_code = true;
    /* TR: a toast when the sensor radio trouble opens the locked Installer page. At most 63 bytes */
    ui_toast(TR("Enter the installer code to see the radio"));
}

/* The Zigbee page after the installer code, while the locked Installer page that asked for it is
 * still the one open; anything else chosen meanwhile forgets it. */
static void trouble_follow(void)
{
    if (!s_after_code) {
        return;
    }
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS || S.depth != 2u ||
        S.pages[1].id != PAGE_INSTALLER) { /* About, then Installer (BL-189) */
        s_after_code = false;
        return;
    }
    if (S.program) {
        s_after_code = false;
        ui_page_open(PAGE_ZIGBEE, 0);
    }
}

void ui_zigbee_trouble_link(lv_obj_t *row, bool link)
{
    if (row == NULL) {
        return;
    }
    if (lv_obj_remove_event_cb(row, trouble_open)) {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE); /* the row is reused for another line */
    }
    if (link) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, trouble_open, LV_EVENT_CLICKED, NULL);
    }
}

/* ---- the page table, the refresh ------------------------------------------------------------- */

typedef void (*page_fn)(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen);

static const struct {
    page_id_t id;
    page_fn build;
} PAGES[] = {
    {PAGE_ZIGBEE, build_network},
    {PAGE_ZIGBEE_ADD, build_add},
    {PAGE_ZIGBEE_ADDED, build_added},
    {PAGE_ZIGBEE_DEVICE, build_device},
    {PAGE_ZIGBEE_CODE, build_code},
    {PAGE_ZIGBEE_CODES, build_codes},
    {PAGE_ZIGBEE_REMOVE, build_remove},
    {PAGE_ZIGBEE_RADIO, build_radio},
    {PAGE_ZIGBEE_CHANNEL, build_channel},
};

void ui_zigbee_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                     size_t nlen)
{
    for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
        if (PAGES[i].id == id) {
            PAGES[i].build(arg, body, title, tlen, note, nlen);
            return;
        }
    }
}

void ui_zigbee_tick(void)
{
    trouble_follow();
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return;
    }
    if (s_menu_row != NULL && lv_tick_elaps(s_menu_polled) >= POLL_MS) {
        s_menu_polled = lv_tick_get();
        ui_zb_status_t st;
        if (read_status(&st)) {
            menu_words(s_menu_row, &st);
        }
    }
    if (s_net.radio != NULL && lv_tick_elaps(s_net.polled) >= POLL_MS) {
        s_net.polled = lv_tick_get();
        if (show_status()) {
            return; /* made again */
        }
        show_devices();
    }
    add_tick();
    dev_tick();
    code_tick();
    codes_tick();
    rm_tick();
    radio_tick();
    chan_tick();
    net_op_tick();
}

/* Whether a page is anywhere in the open stack (the add page keeps listening while a page it
 * opened - Added, a new zone - is on top of it; leaving it stops). */
static bool in_stack(page_id_t id)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return false;
    }
    for (size_t i = 0; i < S.depth; i++) {
        if (S.pages[i].id == id) {
            return true;
        }
    }
    return false;
}

/* Leaving the add page stops listening (devices still setting up go on), and leaving one that
 * shows a result clears it. A same-page refresh must not: S.pages[] already names the page about
 * to be built, and a change away from Settings already reads in subject_view. */
static void stop_if_leaving(void)
{
    if (s_code.active && !in_stack(PAGE_ZIGBEE_CODE)) {
        code_leave(); /* what was typed is wiped */
    }
    if (s_rm.active && !in_stack(PAGE_ZIGBEE_REMOVE)) {
        rm_leave();
    }
    if (s_chan.op != UI_ZB_OP_NONE && !in_stack(PAGE_ZIGBEE_CHANNEL)) {
        s_chan.op = UI_ZB_OP_NONE; /* a move or a check goes on; its end is the radio's */
    }
    if (s_dev.op != UI_ZB_OP_NONE && !in_stack(PAGE_ZIGBEE_DEVICE)) {
        ui_zb_progress_t p;
        read_progress(&p);
        if (p.op == s_dev.op && p.id == s_dev.id && p.step == UI_ZB_STEP_DONE) {
            ask_quietly(UI_ZB_REQ_DISMISS); /* its answer was for a page that is gone */
        }
        s_dev.op = UI_ZB_OP_NONE;
    }
    if (s_owner != PAGE_ZIGBEE_ADD || in_stack(PAGE_ZIGBEE_ADD)) {
        return;
    }
    s_owner = 0;
    ui_zb_progress_t p;
    read_progress(&p);
    if (!adding_op(p.op) || p.step == UI_ZB_STEP_IDLE) {
        return; /* not what that page ran: someone else's, left alone */
    }
    if (p.step != UI_ZB_STEP_DONE) {
        ask_quietly(UI_ZB_REQ_STOP);
        read_progress(&p); /* a stop ends it at once: its "stopped" is nobody's to read */
    }
    if (adding_op(p.op) && p.step == UI_ZB_STEP_DONE) {
        ask_quietly(UI_ZB_REQ_DISMISS);
    }
}

void ui_zigbee_stop(void)
{
    stop_if_leaving();
    s_menu_row = NULL;
    s_net.radio = NULL;
    s_net.codes = NULL;
    s_net.channel = NULL;
    s_net.reset = NULL;
    s_net.list = NULL;
    s_net.empty = NULL;
    s_net.nshown = 0;
    s_add.open = false;
    memset(s_add.row, 0, sizeof(s_add.row));
    memset(s_add.sub, 0, sizeof(s_add.sub));
    s_dev.open = false;
    s_dev.heard = NULL;
    s_dev.signal = NULL;
    s_dev.power = NULL;
    s_dev.setup = NULL;
    s_dev.test = NULL;
    s_codes.open = false;
    s_rm.open = false;
    s_radio.open = false;
    s_radio.state = NULL;
    s_radio.noise = NULL;
    s_radio.restarts = NULL;
    s_chan.open = false;
}
