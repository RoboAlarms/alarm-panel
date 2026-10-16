/*
 * ui_net.c - Wi-Fi and the clock on the touchscreen: Settings > Wi-Fi and the setup's Wi-Fi step
 * (the connection status, a scan of nearby networks, a password on the on-screen keyboard,
 * hidden networks, and what happened when connecting fails); the cog on the status row opens
 * the address settings (the router's address or a fixed one, and the name servers) with the
 * connection check under them; and Settings > Time and language (what the clock says, the time
 * zone, the time server, and with a clock module the date and time set by hand, docs/features/35;
 * its language rows are ui_lang.c's). Every one of these opens with a master or installer code;
 * what the clock says and the 12/24-hour clock show without one.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "i18n/i18n_date.h"
#include "ui_backend/ui_check_words.h"
#include "ui_internal.h"

#define MAX_APS        16u
#define CONNECT_MAX_MS 30000u
#define POLL_MS        500u
#define CHECK_POLL_MS  1000u /* live words change at most once a second (UI-020, UI-023) */

static lv_obj_t *s_status; /* status row */
static lv_obj_t *s_list;   /* network rows */
static lv_obj_t *s_section; /* the heading over them: "Other" once one is joined */
static bool s_setup;
static bool s_active;
static bool s_scanning;
static bool s_connecting;
static uint32_t s_connect_start;
static uint32_t s_polled;
static ui_wifi_ap_t s_aps[MAX_APS];
static size_t s_n_aps;
static char s_ssid[33];
static size_t s_rows_mark; /* network rows are refilled after each scan */

/* The clock's row, on top of Time and language (BL-131): what the clock says and what set it, kept
 * to the minute while it shows; another source rebuilds the page. */
static lv_obj_t *s_clock_row;
static int64_t s_clock_minute;
static uint8_t s_clock_source;
static bool s_clock_settable;
static uint32_t s_clock_polled;

/* The address page (PAGE_NETWORK). */
static ui_ip_config_t s_ip;    /* what that page shows and edits */
static bool s_ip_dirty;        /* a fixed address is being filled in: show the working copy */
static bool s_check_auto;      /* run the check once the page comes back after a change */
static lv_obj_t *s_check_list; /* the check's result rows, NULL when the page is gone */
static size_t s_check_mark;    /* the rows below it are refilled as the check runs */
static ui_netcheck_t s_check UI_SLOW_BSS; /* what those rows show (this thread's only) */
static bool s_check_shown;     /* a result is on screen */
static uint32_t s_check_polled;

static void busy(bool on)
{
    ui_set_int(&subject_busy, on ? 1 : 0);
}

static const char *signal_glyph(int rssi)
{
    return rssi >= -60 ? UI_ICON_WIFI : rssi >= -70 ? UI_ICON_WIFI_HIGH
           : rssi >= -80 ? UI_ICON_WIFI_LOW : UI_ICON_WIFI_ZERO;
}

/* The signal in words, "" when the Wi-Fi module doesn't report it (0). The callers join it to
 * the facts before it with " · ". */
static const char *signal_words(int rssi)
{
    /* TR: how strong a Wi-Fi network's signal is, after the state or the address on the same
     * line ("Password · good signal"). At most 20 bytes */
    return rssi == 0     ? ""
           : rssi >= -60 ? TR("strong signal")
           : rssi >= -70 ? TR("good signal")
           : rssi >= -80 ? TR("fair signal")
                         : TR("weak signal");
}

static void status_text(const ui_wifi_info_t *w, char *title, size_t tlen, char *detail,
                        size_t dlen, lv_color_t *tone)
{
    *tone = ui_tone_amber();
    switch (w->state) {
    case UI_NET_CONNECTED:
        snprintf(title, tlen, "%s", w->ssid);
        {
            const char *sig = signal_words(w->rssi);
            /* TR: the Wi-Fi status line when connected; the address and the signal follow it. At
             * most 12 bytes */
            const char *connected = TR("Connected");
            if (sig[0] != '\0') {
                snprintf(detail, dlen, "%s · %s · %s", connected, sig, w->ip);
            } else {
                snprintf(detail, dlen, "%s · %s", connected, w->ip);
            }
        }
        *tone = ui_tone_green();
        break;
    case UI_NET_CONNECTING:
    case UI_NET_STARTING:
        i18n_utf8_copy(title, tlen, w->ssid[0] ? w->ssid : TR("Wi-Fi"));
        i18n_utf8_copy(detail, dlen, TR("Connecting..."));
        *tone = ui_tone_blue();
        break;
    case UI_NET_NO_RADIO:
        /* TR: the Wi-Fi status title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Wi-Fi module not responding"));
        i18n_utf8_copy(detail, dlen, TR("The panel keeps protecting without the network"));
        *tone = ui_tone_red();
        break;
    case UI_NET_DISCONNECTED:
        snprintf(title, tlen, "%s", w->ssid);
        i18n_utf8_copy(detail, dlen,
                       w->fail == UI_WIFI_FAIL_PASSWORD    ? TR("Can't connect: check the password")
                       : w->fail == UI_WIFI_FAIL_NOT_FOUND ? TR("Can't find this network")
                                                           : TR("Not connected, trying again"));
        break;
    default:
        /* TR: the Wi-Fi status title. At most 47 bytes */
        i18n_utf8_copy(title, tlen, TR("Not connected"));
        i18n_utf8_copy(detail, dlen, TR("Choose a network below"));
        *tone = ui_tone_gray();
        break;
    }
}

static void update_status(void)
{
    if (s_status == NULL || S.be.wifi_info == NULL) {
        return;
    }
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    S.be.wifi_info(S.be.ctx, &w);
    char title[48];
    char detail[96];
    lv_color_t tone;
    status_text(&w, title, sizeof(title), detail, sizeof(detail), &tone);
    ui_set_label(s_status, "title_label", title);
    ui_set_label(s_status, "detail_label", detail);
    lv_obj_remove_flag(ui_find(s_status, "detail_label"), LV_OBJ_FLAG_HIDDEN);
    ui_chip_tone(ui_find(s_status, "chip"),
                 w.state == UI_NET_CONNECTED ? signal_glyph(w.rssi) : UI_ICON_WIFI_OFF, tone);
}

bool ui_net_connected(void)
{
    if (S.be.wifi_info == NULL) {
        return false;
    }
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    S.be.wifi_info(S.be.ctx, &w);
    return w.state == UI_NET_CONNECTED;
}

/* ---- connecting ---------------------------------------------------------------------------- */

static void connect(const char *ssid, const char *password)
{
    if (S.be.wifi_connect == NULL) {
        return;
    }
    if (ssid != s_ssid) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    }
    S.be.wifi_connect(S.be.ctx, s_ssid, password);
    s_connecting = true;
    s_connect_start = lv_tick_get();
    busy(true); /* the status row says "Connecting to ..." - no toast on top of it */
    update_status();
}

static void password_done(const char *text)
{
    connect(s_ssid, text);
}

static void ask_password(const char *ssid)
{
    if (ssid != s_ssid) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    }
    char title[64];
    /* TR: the keyboard sheet's title when joining a network; {0} is the network's name. At most
     * 31 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Password for {0}"), ssid);
    ui_text_sheet(title, TR("Letters and symbols: 123 and #+= switch the keys"), "", true, 64,
                  password_done);
}

static void on_ap(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i >= s_n_aps) {
        return;
    }
    if (s_aps[i].secure) {
        ask_password(s_aps[i].ssid);
    } else {
        connect(s_aps[i].ssid, "");
    }
}

static void hidden_ssid_done(const char *text)
{
    if (text[0] == '\0') {
        return;
    }
    ask_password(text);
}

static void on_other(void *ctx)
{
    (void)ctx;
    ui_text_sheet(TR("Network name"), TR("Exactly as it is set on the router (hidden networks too)"),
                  "", false, 32, hidden_ssid_done);
}

static void forget_confirmed(void)
{
    if (S.be.wifi_connect != NULL) {
        S.be.wifi_connect(S.be.ctx, "", "");
    }
    ui_page_back(); /* the Wi-Fi page says Not connected: no toast on top of it */
}

static void on_forget(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Forget this network?"),
               TR("The panel disconnects. The clock and updates need the network; the "
                  "alarm itself doesn't."),
               TR("Forget"), forget_confirmed);
}

/* ---- scanning ------------------------------------------------------------------------------ */

static void fill_list(void)
{
    if (s_list == NULL) {
        return;
    }
    lv_obj_clean(s_list);
    ui_rows_release(s_rows_mark);
    /* The joined network has its own row on top: the list holds the others. */
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    const bool joined = w.state == UI_NET_CONNECTED && w.ssid[0] != '\0';
    if (s_section != NULL) {
        ui_set_label(s_section, "text_label",
                     joined ? TR("Other networks nearby") : TR("Networks nearby"));
    }
    for (size_t i = 0; i < s_n_aps; i++) {
        if (joined && strcmp(s_aps[i].ssid, w.ssid) == 0) {
            continue;
        }
        char detail[64];
        /* TR: the line under a nearby network: whether it asks for a password. At most 25 bytes */
        const char *security = s_aps[i].secure ? TR_C("Wi-Fi network", "Password")
                                               : TR_C("Wi-Fi network", "Open");
        const char *sig = signal_words(s_aps[i].rssi);
        if (sig[0] != '\0') {
            snprintf(detail, sizeof(detail), "%s · %s", security, sig);
        } else {
            i18n_utf8_copy(detail, sizeof(detail), security);
        }
        ui_row_link(s_list, signal_glyph(s_aps[i].rssi), ui_tone_blue(), s_aps[i].ssid, detail,
                    NULL, s_aps[i].secure ? UI_ICON_LOCK : NULL, on_ap, (void *)(uintptr_t)i);
    }
    if (s_n_aps == 0u) {
        ui_paragraph(s_list,
                     s_scanning ? TR("Looking for networks...") : TR("No networks found nearby."),
                     ui_tone_gray());
    }
    ui_row_link(s_list, UI_ICON_PLUS, ui_tone_gray(), TR("Other network..."),
                TR("Type the name"),
                NULL, UI_ICON_CHEVRON_RIGHT, on_other, NULL);
}

static void start_scan(void)
{
    s_n_aps = 0;
    s_scanning = S.be.wifi_scan != NULL && S.be.wifi_scan(S.be.ctx);
    busy(s_scanning);
    fill_list();
}

static void on_rescan(void *ctx)
{
    (void)ctx;
    start_scan();
}

/* ---- the way to the address settings --------------------------------------------------------- */

/* The joined network's row opens its page. It does when the panel is offline too, while a network
 * is set: a fixed address typed wrong is exactly when you need to get back in and undo it. */
static bool advanced_available(void)
{
    return S.be.ip_get != NULL && S.be.ip_set != NULL;
}

static void open_advanced(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_NETWORK, 0);
}

void ui_net_build(lv_obj_t *body, bool setup)
{
    s_setup = setup;
    s_active = true;
    s_connecting = false;
    s_ip_dirty = false; /* back on this page: the address settings show what is stored */
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    const bool opens = !setup && w.ssid[0] != '\0' && advanced_available();
    s_status = ui_row_link(body, UI_ICON_WIFI, ui_tone_blue(), TR("Wi-Fi"), "", NULL,
                           opens ? UI_ICON_CHEVRON_RIGHT : NULL, opens ? open_advanced : NULL, NULL);
    update_status();
    s_section = ui_section(body, w.state == UI_NET_CONNECTED ? TR("Other networks nearby")
                                                             : TR("Networks nearby"));
    s_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, SPACE_SM, 0);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    ui_row_action(body, UI_ICON_ROTATE_CCW, TR("Look again"), ui_tone_blue(), on_rescan, NULL);
    s_rows_mark = ui_rows_mark();
    if (w.state == UI_NET_NO_RADIO) {
        fill_list();
        return;
    }
    start_scan();
}

void ui_net_stop(void)
{
    s_active = false;
    s_clock_row = NULL;
    s_status = NULL;
    s_list = NULL;
    s_section = NULL;
    s_check_list = NULL;
    s_scanning = false;
    s_connecting = false;
}

static void check_tick(void);
static void clock_row_tick(void);
static void rebuild_later(void *unused);

void ui_net_tick(void)
{
    check_tick();
    clock_row_tick();
    if (!s_active || lv_tick_elaps(s_polled) < POLL_MS) {
        return;
    }
    s_polled = lv_tick_get();
    if (s_scanning && S.be.wifi_results != NULL) {
        bool done = false;
        s_n_aps = S.be.wifi_results(S.be.ctx, s_aps, MAX_APS, &done);
        if (done) {
            s_scanning = false;
            busy(s_connecting);
            fill_list();
        }
    }
    if (s_connecting && S.be.wifi_info != NULL) {
        ui_wifi_info_t w;
        memset(&w, 0, sizeof(w));
        S.be.wifi_info(S.be.ctx, &w);
        const bool timeout = lv_tick_elaps(s_connect_start) >= CONNECT_MAX_MS;
        if (w.state == UI_NET_CONNECTED && strcmp(w.ssid, s_ssid) == 0) {
            s_connecting = false;
            busy(false); /* the status row now says Connected, and opens the network's page */
            if (s_setup) {
                fill_list();
            } else {
                lv_async_call(rebuild_later, NULL);
            }
        } else if ((w.state == UI_NET_DISCONNECTED && w.fail != UI_WIFI_FAIL_NONE &&
                    lv_tick_elaps(s_connect_start) > 3000u) ||
                   timeout) {
            s_connecting = false;
            busy(false);
            ui_beep(UI_BEEP_ERROR);
            ui_toast(w.fail == UI_WIFI_FAIL_PASSWORD    ? TR("Wrong password? The router refused it")
                     : w.fail == UI_WIFI_FAIL_NOT_FOUND ? TR("That network doesn't answer")
                                                        : TR("Couldn't connect. The panel keeps trying"));
        }
    }
    update_status();
    (void)s_setup;
}

/* ---- addresses: reading and writing them as words -------------------------------------------- */

/* Strict dotted quad: four numbers 0..255, no spaces, no leading zeros beyond one digit. */
static bool ip_parse(const char *s, uint8_t out[4])
{
    if (s == NULL) {
        return false;
    }
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10u + (unsigned)(*s++ - '0');
            digits++;
        }
        if (v > 255u || (digits > 1 && s[-digits] == '0')) {
            return false;
        }
        out[part] = (uint8_t)v;
        if (part < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    return *s == '\0';
}

/* A usable address: parses, and isn't the "no address" one. */
static bool ip_ok(const char *s)
{
    uint8_t q[4];
    return s[0] != '\0' && ip_parse(s, q) && (q[0] | q[1] | q[2] | q[3]) != 0u;
}

/* How many leading ones a mask has, -1 when it isn't a mask (255.255.0.255 and such). */
static int mask_prefix(const char *s)
{
    uint8_t q[4];
    if (!ip_parse(s, q)) {
        return -1;
    }
    uint32_t bits = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
    int n = 0;
    while (n < 32 && (bits & 0x80000000u) != 0u) {
        bits <<= 1;
        n++;
    }
    return bits == 0u ? n : -1;
}

/* Accepts "255.255.255.0", "24" or "/24" and writes the dotted form. */
static bool mask_normalise(const char *in, char *out, size_t len)
{
    if (in[0] == '/') {
        in++;
    }
    if (in[0] >= '0' && in[0] <= '9' && strchr(in, '.') == NULL) {
        unsigned n = 0;
        for (const char *p = in; *p != '\0'; p++) {
            if (*p < '0' || *p > '9' || n > 32u) {
                return false;
            }
            n = n * 10u + (unsigned)(*p - '0');
        }
        if (n < 1u || n > 30u) { /* /31 and /32 leave no room for a router */
            return false;
        }
        const uint32_t bits = n == 0u ? 0u : 0xFFFFFFFFu << (32u - n);
        snprintf(out, len, "%u.%u.%u.%u", (unsigned)(bits >> 24) & 0xFFu,
                 (unsigned)(bits >> 16) & 0xFFu, (unsigned)(bits >> 8) & 0xFFu,
                 (unsigned)bits & 0xFFu);
        return true;
    }
    if (mask_prefix(in) < 1 || mask_prefix(in) > 30) {
        return false;
    }
    snprintf(out, len, "%s", in);
    return true;
}

/* Everything a fixed address needs is filled in and sensible. */
static bool static_complete(const ui_ip_config_t *c, char *why, size_t wlen)
{
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    if (!ip_ok(c->ip) || !ip_ok(c->mask) || !ip_ok(c->gateway)) {
        i18n_utf8_copy(why, wlen, TR("Fill in the address, the network and the router"));
        return false;
    }
    ip_parse(c->ip, ip);
    ip_parse(c->mask, mask);
    ip_parse(c->gateway, gw);
    if (mask_prefix(c->mask) < 1) {
        i18n_utf8_copy(why, wlen, TR("That network doesn't look right"));
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & mask[i]) != (gw[i] & mask[i])) {
            i18n_utf8_copy(why, wlen, TR("The router must be on the same network as the panel"));
            return false;
        }
    }
    if (memcmp(ip, gw, 4) == 0) {
        i18n_utf8_copy(why, wlen, TR("The panel and the router can't share one address"));
        return false;
    }
    return true;
}

/* ---- the address page ------------------------------------------------------------------------ */

static void rebuild_later(void *unused)
{
    (void)unused;
    ui_page_rebuild();
}

/* Hands the settings to the platform, unless a fixed address is still incomplete: then the
 * page keeps what has been typed so far and says what is missing (nothing is applied, so the
 * panel stays on the network it is on). */
static void apply_ip(const char *done_text)
{
    char why[96] = "";
    if (s_ip.use_static && !static_complete(&s_ip, why, sizeof(why))) {
        s_ip_dirty = true;
        lv_async_call(rebuild_later, NULL);
        return;
    }
    if (!S.be.ip_set(S.be.ctx, &s_ip, why, sizeof(why))) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] ? ui_answer("why", why) : TR("Not saved"));
        s_ip_dirty = true;
    } else {
        s_ip_dirty = false;
        s_check_auto = true; /* the address changed: see whether it still works */
        if (done_text != NULL) {
            ui_toast(done_text);
        }
    }
    lv_async_call(rebuild_later, NULL);
}

static void mode_chosen(int32_t index)
{
    const bool want_static = index == 1;
    if (want_static == s_ip.use_static) {
        return;
    }
    s_ip.use_static = want_static;
    if (want_static && s_ip.ip[0] == '\0') {
        /* Start from the address the router gave: usually right except for the last number. */
        ui_wifi_info_t w;
        memset(&w, 0, sizeof(w));
        if (S.be.wifi_info != NULL) {
            S.be.wifi_info(S.be.ctx, &w);
        }
        snprintf(s_ip.ip, sizeof(s_ip.ip), "%s", w.ip);
        snprintf(s_ip.mask, sizeof(s_ip.mask), "%s", w.mask[0] ? w.mask : "255.255.255.0");
        snprintf(s_ip.gateway, sizeof(s_ip.gateway), "%s", w.gateway);
    }
    apply_ip(NULL); /* the mode row changes, and the connection check starts by itself */
}

static void pick_mode(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[2];
    choices[0] = (ui_choice_t){UI_ICON_HOUSE_WIFI, ui_tone_blue(), TR("Automatic"),
                               TR("The usual way: the router hands out an address (DHCP)")};
    choices[1] = (ui_choice_t){UI_ICON_TAG, ui_tone_violet(), TR("A fixed address"),
                               TR("The panel always has the same address on your network")};
    ui_pick_sheet(TR("IP address settings"),
                  TR("A fixed address is handy for firewall rules"), choices, 2,
                  s_ip.use_static ? 1 : 0, mode_chosen);
}

/* One address field: which one, what to call it, and what an empty entry means. */
typedef struct {
    size_t offset;    /* into ui_ip_config_t */
    const char *title;
    const char *detail;
    const char *note;  /* the sheet's line of help */
    const char *empty; /* the value shown when it isn't set */
    bool is_mask;
    bool may_be_empty;
} ip_field_t;

/* TR: Settings > Wi-Fi > address settings, a fixed address's fields: the title, the line under it,
 * the help on the keyboard sheet and the value shown while the field is empty. */
static const ip_field_t IP_FIELDS[] = {
    {offsetof(ui_ip_config_t, ip), TR_NOOP("Address"), TR_NOOP("This panel, on your network"),
     TR_NOOP("Like 192.168.1.42. Pick one outside the router's DHCP range"), TR_NOOP("Not set"),
     false, false},
    {offsetof(ui_ip_config_t, mask), TR_NOOP("Subnet mask"), TR_NOOP("Which addresses count as local"),
     TR_NOOP("Like 255.255.255.0, or just 24"), TR_NOOP("Not set"), true, false},
    {offsetof(ui_ip_config_t, gateway), TR_NOOP("Router"),
     TR_NOOP("The way out to the rest of the network"),
     TR_NOOP("Your router's address, like 192.168.1.1"), TR_NOOP("Not set"), false, false},
    {offsetof(ui_ip_config_t, dns1), TR_NOOP("DNS server"), TR_NOOP("Turns names into addresses"),
     TR_NOOP("Like 192.168.1.1 or 1.1.1.1. Empty: the router's"), TR_NOOP("Automatic"),
     false, true},
    {offsetof(ui_ip_config_t, dns2), TR_NOOP("Second DNS server"),
     TR_NOOP("Used when the first doesn't answer"), TR_NOOP("Empty: none"), TR_NOOP("None"), false,
     true},
};
#define IP_FIELD_COUNT (sizeof(IP_FIELDS) / sizeof(IP_FIELDS[0]))

static size_t s_editing; /* the field the text sheet is filling in */

static char *ip_field(const ip_field_t *f)
{
    return (char *)&s_ip + f->offset;
}

static void ip_field_done(const char *text)
{
    if (s_editing >= IP_FIELD_COUNT) {
        return;
    }
    const ip_field_t *f = &IP_FIELDS[s_editing];
    char *slot = ip_field(f);
    char value[16] = "";
    if (text[0] == '\0') {
        if (!f->may_be_empty) {
            ui_beep(UI_BEEP_ERROR);
            ui_toast(TR("This one is needed for a fixed address"));
            return;
        }
    } else if (f->is_mask) {
        if (!mask_normalise(text, value, sizeof(value))) {
            ui_beep(UI_BEEP_ERROR);
            ui_toast(TR("Type 255.255.255.0, or just 24"));
            return;
        }
    } else if (!ip_ok(text) || strlen(text) >= sizeof(value)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("That isn't an address like 192.168.1.42"));
        return;
    } else {
        snprintf(value, sizeof(value), "%s", text);
    }
    if (strcmp(slot, value) == 0) {
        return;
    }
    snprintf(slot, 16, "%s", value);
    apply_ip(NULL); /* the edited row shows the new value */
}

static void edit_ip_field(void *ctx)
{
    s_editing = (size_t)(uintptr_t)ctx;
    if (s_editing >= IP_FIELD_COUNT) {
        return;
    }
    const ip_field_t *f = &IP_FIELDS[s_editing];
    ui_text_sheet_kind(TR(f->title), TR(f->note), ip_field(f), UI_TEXT_NUMBER, 15, ip_field_done);
}

/* ---- checking the connection ----------------------------------------------------------------- */

static lv_obj_t *s_check_rows[UI_CHECK_COUNT];

/* TR: Settings > Wi-Fi > address settings, the connection check's steps, one row each. */
static const char *const CHECK_NAMES[UI_CHECK_COUNT] = {
    TR_NOOP("Wi-Fi"),       TR_NOOP("Address"),     TR_NOOP("Router"),
    TR_NOOP("Name lookup"), TR_NOOP("Internet"),    TR_NOOP("Time server"),
    TR_NOOP("Connected service"),
};

static void check_look(uint8_t state, const char **glyph, lv_color_t *tone)
{
    switch (state) {
    case UI_CHECK_PASS:
        *glyph = UI_ICON_CIRCLE_CHECK;
        *tone = ui_tone_green();
        break;
    case UI_CHECK_FAIL:
        *glyph = UI_ICON_CIRCLE_X;
        *tone = ui_tone_red();
        break;
    case UI_CHECK_WARN:
        *glyph = UI_ICON_TRIANGLE_ALERT;
        *tone = ui_tone_amber();
        break;
    case UI_CHECK_RUNNING:
        *glyph = UI_ICON_REFRESH;
        *tone = ui_tone_blue();
        break;
    case UI_CHECK_SKIPPED:
        *glyph = UI_ICON_CIRCLE_SLASH;
        *tone = ui_tone_gray();
        break;
    default:
        *glyph = UI_ICON_CIRCLE_DOT;
        *tone = ui_tone_gray();
        break;
    }
}

static void check_row_text(size_t i, const ui_check_result_t *r)
{
    lv_obj_t *row = s_check_rows[i];
    if (row == NULL) {
        return;
    }
    const char *glyph;
    lv_color_t tone;
    check_look(r->state, &glyph, &tone);
    ui_chip_tone(ui_find(row, "chip"), glyph, tone);
    char words[96];
    const char *detail = r->state == UI_CHECK_RUNNING ? TR("Checking...") : TR("Waiting its turn");
    if (r->words != NULL) {
        /* ui_check_words.h: the check's words in this screen's language, its values in them */
        ui_check_line(words, sizeof(words), TR_C("connection check", r->words), r->value[0],
                      r->value[1]);
        detail = words;
    }
    ui_set_label(row, "detail_label", detail);
    char value[16] = "";
    if (r->ms != 0u && (r->state == UI_CHECK_PASS || r->state == UI_CHECK_WARN)) {
        /* TR: how long a connection-check step took, at the end of its row; {0} is a number of
         * milliseconds. At most 12 bytes */
        I18N_FORMAT(value, sizeof(value), TRN("{0} ms", "{0} ms", r->ms), (unsigned)r->ms);
    }
    ui_set_label(row, "value_label", value);
    lv_obj_t *label = ui_find(row, "value_label");
    if (label != NULL) {
        lv_obj_set_flag(label, LV_OBJ_FLAG_HIDDEN, value[0] == '\0');
    }
}

/* The rows are made once per run and then only their words change: the list never reflows
 * while the check walks through it (UI-020, UI-023). */
static void check_rows_build(void)
{
    if (s_check_list == NULL) {
        return;
    }
    lv_obj_clean(s_check_list);
    ui_rows_release(s_check_mark);
    memset(s_check_rows, 0, sizeof(s_check_rows));
    for (size_t i = 0; i < UI_CHECK_COUNT; i++) {
        const char *glyph;
        lv_color_t tone;
        check_look(s_check.steps[i].state, &glyph, &tone);
        s_check_rows[i] = ui_row_link(s_check_list, glyph, tone, TR(CHECK_NAMES[i]), " ", " ", NULL,
                                      NULL, NULL);
        check_row_text(i, &s_check.steps[i]);
    }
    s_check_shown = true;
}

static void read_check(ui_netcheck_t *out)
{
    memset(out, 0, sizeof(*out));
    if (S.be.net_check_status != NULL) {
        S.be.net_check_status(S.be.ctx, out);
    }
}

static void check_tick(void)
{
    if (s_check_list == NULL || !s_check_shown || lv_tick_elaps(s_check_polled) < CHECK_POLL_MS) {
        return;
    }
    s_check_polled = lv_tick_get();
    ui_netcheck_t now;
    read_check(&now);
    const bool was_running = s_check.running;
    for (size_t i = 0; i < UI_CHECK_COUNT; i++) {
        if (now.steps[i].state != s_check.steps[i].state ||
            now.steps[i].ms != s_check.steps[i].ms ||
            now.steps[i].words != s_check.steps[i].words ||
            memcmp(now.steps[i].value, s_check.steps[i].value, sizeof(now.steps[i].value)) != 0) {
            s_check.steps[i] = now.steps[i];
            check_row_text(i, &s_check.steps[i]);
        }
    }
    s_check.running = now.running;
    s_check.done = now.done;
    if (was_running && !now.running) {
        busy(false);
        lv_async_call(rebuild_later, NULL); /* the page's note becomes the run's summary */
    }
}

static void start_check(void)
{
    if (S.be.net_check_start == NULL || !S.be.net_check_start(S.be.ctx)) {
        read_check(&s_check);
        if (s_check.running) {
            busy(true); /* one is already going (the address just changed): follow it */
            check_rows_build();
            return;
        }
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("The panel can't check the connection right now"));
        return;
    }
    busy(true);
    read_check(&s_check);
    check_rows_build();
}

static void on_check(void *ctx)
{
    (void)ctx;
    start_check();
}

/* One line on how the run went, for the page's note. */
static void check_summary(char *out, size_t len)
{
    size_t failed = 0;
    size_t warned = 0;
    for (size_t i = 0; i < UI_CHECK_COUNT; i++) {
        failed += s_check.steps[i].state == UI_CHECK_FAIL;
        warned += s_check.steps[i].state == UI_CHECK_WARN;
    }
    if (failed != 0u) {
        /* TR: the address page's line under the title after a connection check; {0} is how
         * many steps failed. At most 100 bytes */
        I18N_FORMAT(out, len,
                    TRN("{0} check didn't pass: see the red lines below",
                        "{0} checks didn't pass: see the red lines below", failed),
                    (unsigned)failed);
    } else if (warned != 0u) {
        i18n_utf8_copy(out, len, TR("Everything the panel needs works; see the amber lines"));
    } else {
        i18n_utf8_copy(out, len, TR("Everything passed"));
    }
}

/* The connected network's page (PAGE_NETWORK): its name for the title, how it is going for the
 * note, its address, how the panel gets that and its DNS server, the connection check and, apart
 * at the bottom, Forget. */
void ui_net_advanced_build(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    s_check_list = NULL;
    s_check_shown = false;
    memset(s_check_rows, 0, sizeof(s_check_rows));
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    lv_color_t tone;
    char detail[96];
    char name[48];
    status_text(&w, name, sizeof(name), detail, sizeof(detail), &tone);
    if (w.ssid[0] != '\0') {
        i18n_utf8_copy(title, tlen, w.ssid);
    }
    if (w.state == UI_NET_CONNECTED) {
        const char *sig = signal_words(w.rssi);
        if (sig[0] != '\0') {
            snprintf(note, nlen, "%s · %s", TR("Connected"), sig);
        } else {
            i18n_utf8_copy(note, nlen, TR("Connected"));
        }
    } else {
        i18n_utf8_copy(note, nlen, detail);
    }
    if (!advanced_available()) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    if (!s_ip_dirty) {
        S.be.ip_get(S.be.ctx, &s_ip);
    }
    read_check(&s_check); /* the platform owns the check: a run left going is picked back up */

    ui_row_value(body, TR("IP address"), w.ip[0] ? w.ip : TR("No address"));
    ui_row_link(body, UI_ICON_SLIDERS, ui_tone_blue(), TR("IP address settings"),
                TR("Automatic from the router, or a fixed address"),
                s_ip.use_static ? TR_C("address mode", "Fixed") : TR("Automatic"),
                UI_ICON_CHEVRON_RIGHT, pick_mode, NULL);
    for (size_t i = 0; i < IP_FIELD_COUNT; i++) {
        const ip_field_t *f = &IP_FIELDS[i];
        const bool dns = f->may_be_empty; /* the two DNS servers */
        if (!dns && !s_ip.use_static) {
            continue; /* the router decides these */
        }
        if (dns && i == IP_FIELD_COUNT - 1u && s_ip.dns1[0] == '\0') {
            continue; /* no first DNS server set: don't ask for a second */
        }
        const char *value = ip_field(f);
        ui_row_link(body, dns ? UI_ICON_SERVER : UI_ICON_TAG, ui_tone_blue(), TR(f->title),
                    TR(f->detail), value[0] != '\0' ? value : TR(f->empty), UI_ICON_CHEVRON_RIGHT,
                    edit_ip_field, (void *)(uintptr_t)i);
    }
    char why[96] = "";
    if (s_ip.use_static && !static_complete(&s_ip, why, sizeof(why))) {
        char text[192];
        /* TR: under the fixed-address fields when one is missing or wrong; {0} is the reason,
         * a sentence. At most 90 bytes */
        I18N_FORMAT(text, sizeof(text), TR("{0}. Until then the panel keeps asking the router."),
                    (const char *)why);
        ui_paragraph(body, text, ui_tone_amber());
    } else if (s_ip.use_static) {
        ui_paragraph(body, TR("Make sure no other device is given this address: pick one outside "
                            "the range your router hands out."),
                     ui_tone_gray());
    }

    ui_row_action(body, UI_ICON_HEART_PULSE,
                  s_check.done || s_check.running ? TR("Check again") : TR("Check the connection"),
                  ui_tone_blue(), on_check, NULL);
    s_check_mark = ui_rows_mark();
    s_check_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_check_list);
    lv_obj_set_size(s_check_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_check_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_check_list, SPACE_SM, 0);
    lv_obj_remove_flag(s_check_list, LV_OBJ_FLAG_SCROLLABLE);
    if (s_check_auto) {
        s_check_auto = false;
        start_check();
    } else if (s_check.done || s_check.running) {
        busy(s_check.running);
        check_rows_build();
    }
    if (s_check.done && !s_check.running) {
        check_summary(note, nlen);
    }
    if (w.ssid[0] != '\0') {
        ui_section(body, ""); /* a gap: Forget stands apart */
        ui_row_action(body, UI_ICON_WIFI_OFF, TR("Forget this network"), ui_tone_red(), on_forget,
                      NULL);
    }
}

/* ---- date and time --------------------------------------------------------------------------- */

static void tz_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < UI_TIMEZONE_COUNT && S.be.set_timezone != NULL) {
        S.be.set_timezone(S.be.ctx, UI_TIMEZONES[index].posix);
        S.last_minute = -1; /* the row and the clock show the new zone */
    }
    ui_page_rebuild();
}

static void pick_tz(void *ctx)
{
    (void)ctx;
    ui_tz_pick(tz_chosen);
}

static void ntp_changed(const char *text)
{
    if (S.be.set_ntp != NULL) {
        S.be.set_ntp(S.be.ctx, text); /* the row shows the server */
    }
    ui_page_rebuild();
}

static void edit_ntp(void *ctx)
{
    (void)ctx;
    char server[64] = "";
    if (S.be.get_ntp != NULL) {
        S.be.get_ntp(S.be.ctx, server, sizeof(server));
    }
    ui_text_sheet(TR("Time server"), TR("Leave empty for pool.ntp.org"), server, false, 63,
                  ntp_changed);
}

static void open_set_clock(void *ctx);

/* What set the clock last (a time set by hand stays that until the next sync); settable: a clock
 * module can keep a time set by hand (RTC-008). The hardware that keeps the time is never named
 * here: a person sees what they can do, its state is for the console and the Flasher. */
static uint8_t clock_source(bool *settable)
{
    ui_clock_info_t c;
    memset(&c, 0, sizeof(c));
    if (S.be.clock_info != NULL) {
        S.be.clock_info(S.be.ctx, &c);
    }
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    *settable = S.be.clock_set != NULL && ui_clock_settable(&c);
    return c.source == UI_CLOCK_NOT_SET && w.time_synced ? (uint8_t)UI_CLOCK_FROM_INTERNET : c.source;
}

static int64_t clock_now(char *buf, size_t len)
{
    const int64_t ms = S.be.epoch_ms(S.be.ctx);
    if (ms > 0) {
        ui_date_time_long(ms, buf, len);
    } else {
        /* TR: Time and language, the clock's row while the panel has no time at all. At most 63
         * bytes */
        i18n_utf8_copy(buf, len, TR("The clock isn't set"));
    }
    return ms > 0 ? ms / 60000 : -1;
}

static void clock_row(lv_obj_t *body)
{
    bool settable = false;
    const uint8_t source = clock_source(&settable);
    const char *words;
    switch (source) {
    case UI_CLOCK_FROM_INTERNET:
        /* TR: Time and language, under the date and time: what keeps the clock right */
        words = TR("Kept right by the internet");
        break;
    case UI_CLOCK_FROM_MODULE:
        /* TR: Time and language, under the date and time: the panel kept the time through a power
         * cut, with no internet since */
        words = TR("Kept while the panel was off");
        break;
    case UI_CLOCK_BY_HAND:
        /* TR: Time and language, under the date and time: someone set the clock on this screen or
         * in the Panel Manager */
        words = TR("Set by hand");
        break;
    default:
        /* TR: Time and language, under the date and time while nothing has set the clock: it can
         * be set by hand (a tap on the row) */
        words = settable ? TR("Not set: no internet yet. Tap to set it")
                         /* TR: Time and language, under the date and time while nothing has set
                          * the clock and it can't be set by hand */
                         : TR("Not set: it sets itself once Wi-Fi is connected");
        break;
    }
    const bool unset = source == UI_CLOCK_NOT_SET;
    char now[64];
    s_clock_minute = clock_now(now, sizeof(now));
    s_clock_source = source;
    s_clock_settable = settable;
    s_clock_polled = lv_tick_get();
    s_clock_row = ui_row_link(body, UI_ICON_CALENDAR_CLOCK, unset ? ui_tone_amber() : ui_tone_green(),
                              now, words,
                              /* TR: Time and language, on the clock's row while nothing has set it:
                               * a tap sets it. At most 15 bytes */
                              unset && settable ? TR_C("set the clock", "Set") : NULL,
                              settable ? UI_ICON_CHEVRON_RIGHT : NULL,
                              settable ? open_set_clock : NULL, NULL);
}

/* Once a second while the row shows: a new minute changes its title (UI-020: at most once a
 * second), a new source or a module that can now keep a time rebuilds the page. */
static void clock_row_tick(void)
{
    if (s_clock_row == NULL || lv_tick_elaps(s_clock_polled) < 1000u) {
        return;
    }
    s_clock_polled = lv_tick_get();
    bool settable = false;
    if (clock_source(&settable) != s_clock_source || settable != s_clock_settable) {
        if (lv_subject_get_int(&subject_sheet) == SHEET_NONE && S.pad == PAD_NONE) {
            ui_page_rebuild();
        }
        return;
    }
    char now[64];
    const int64_t minute = clock_now(now, sizeof(now));
    if (minute != s_clock_minute) {
        s_clock_minute = minute;
        ui_set_label(s_clock_row, "title_label", now);
    }
}

/* A keypad's Time and language (BL-048): the clock and the time zone come from the alarm panel
 * (docs/features/11 "Roles on a P4 panel": a keypad follows its TIME and runs no time server), so
 * they are read-only here and say so; the 12/24-hour clock, the language and the region are this
 * keypad's, the language and region behind a code as on any panel. */
static void keypad_time_build(lv_obj_t *body, bool unlocked, char *note, size_t nlen)
{
    const char *master = ui_link_master_name();
    char now[64];
    char detail[128];
    bool settable = false;
    s_clock_source = clock_source(&settable);
    s_clock_settable = settable;
    s_clock_minute = clock_now(now, sizeof(now));
    s_clock_polled = lv_tick_get();
    /* TR: a keypad's Time and language, under the date and time: it comes from the alarm panel
     * named {0}. At most 80 bytes */
    I18N_FORMAT(detail, sizeof(detail), TR("From {0}"), master);
    s_clock_row = ui_row_link(body, UI_ICON_CALENDAR_CLOCK, ui_tone_green(), now, detail, NULL,
                              NULL, NULL, NULL);
    char tz[64] = "UTC0";
    if (S.be.get_timezone != NULL) {
        S.be.get_timezone(S.be.ctx, tz, sizeof(tz));
    }
    const int i = ui_tz_find(tz);
    /* TR: a keypad's Time and language, under Time zone: it is set on the alarm panel named {0}.
     * At most 80 bytes */
    I18N_FORMAT(detail, sizeof(detail), TR("Set on {0}"), master);
    ui_row_link(body, UI_ICON_GLOBE, ui_tone_blue(), TR("Time zone"), detail,
                i >= 0 ? TR_C("time zone", UI_TIMEZONES[i].name) : tz, NULL, NULL, NULL);
    ui_lang_clock_row(body); /* this keypad's and anyone's */
    if (!unlocked) {
        /* TR: a keypad's Time and language without a code, the line under the title. At most 127
         * bytes */
        i18n_utf8_copy(note, nlen, TR("A master or installer code changes the language"));
        /* TR: a locked page: its title (cut to 31 bytes). At most 31 bytes */
        ui_settings_unlock_row(body, ACCESS_SHARED, TR("Time and language"));
        return;
    }
    /* TR: Settings > Time and language, the heading over the language rows */
    ui_section(body, TR("Language"));
    ui_lang_rows(body);
}

void ui_time_build(lv_obj_t *body, bool unlocked, char *note, size_t nlen)
{
    if (ui_link_keypad()) {
        keypad_time_build(body, unlocked, note, nlen);
        return;
    }
    clock_row(body);
    if (!unlocked) {
        /* TR: Settings > Time and language without a code, the line under the title. At most 127
         * bytes */
        i18n_utf8_copy(note, nlen, TR("A master or installer code changes the time zone and the language"));
        ui_lang_clock_row(body); /* this keypad's and anyone's */
        /* TR: a locked page: its title (cut to 31 bytes). At most 31 bytes */
        ui_settings_unlock_row(body, ACCESS_SHARED, TR("Time and language"));
        return;
    }
    char tz[64] = "UTC0";
    char server[64] = "pool.ntp.org";
    if (S.be.get_timezone != NULL) {
        S.be.get_timezone(S.be.ctx, tz, sizeof(tz));
    }
    if (S.be.get_ntp != NULL) {
        S.be.get_ntp(S.be.ctx, server, sizeof(server));
    }
    const int i = ui_tz_find(tz);
    ui_row_link(body, UI_ICON_GLOBE, ui_tone_blue(), TR("Time zone"),
                i >= 0 ? TR_C("time zone places", UI_TIMEZONES[i].places) : tz,
                i >= 0 ? TR_C("time zone", UI_TIMEZONES[i].name) : TR("Custom"),
                UI_ICON_CHEVRON_RIGHT, pick_tz, NULL);
    ui_lang_clock_row(body);
    /* TR: Settings > Time and language, the heading over the language rows */
    ui_section(body, TR("Language"));
    ui_lang_rows(body);
    /* TR: Settings > Time and language, the heading over what few people change */
    ui_section(body, TR("Advanced"));
    ui_row_link(body, UI_ICON_SERVER, ui_tone_blue(), TR("Time server"),
                /* TR: Settings > Time and language, under Time server */
                TR("Where the clock comes from"), server, UI_ICON_CHEVRON_RIGHT, edit_ntp, NULL);
}

/* ---- setting the clock by hand (docs/features/35 RTC-008, BL-131 finding 3) --------------------
 * Only with a clock module: without one the time would be gone at the next power cut, and the
 * time server sets it anyway. A calendar and rollers (clock_picker.xml) choose this panel's local
 * time, to the minute: on a sheet over Time and language, or narrower on the setup's step. */

#define SET_YEAR_FIRST 2025
#define SET_YEAR_LAST  2099

static struct {
    int32_t year, month, day, hour, minute;
} s_set;

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

static int32_t days_in(int32_t year, int32_t month)
{
    static const int8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return DAYS[(month - 1) % 12] + ((month == 2 && leap) ? 1 : 0);
}

/* The weekday of a date (0 Sunday), Gregorian (Sakamoto's method). */
static int32_t weekday_of(int32_t year, int32_t month, int32_t day)
{
    static const int8_t T[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    const int32_t y = month < 3 ? year - 1 : year;
    return (y + y / 4 - y / 100 + y / 400 + T[(month - 1) % 12] + day) % 7;
}

/* The panel's local time now; false while it has none. */
static bool local_now(struct tm *tm)
{
    const int64_t ms = S.be.epoch_ms(S.be.ctx);
    if (ms <= 0) {
        return false;
    }
    const time_t now = (time_t)(ms / 1000);
#if defined(_WIN32)
    localtime_s(tm, &now);
#else
    localtime_r(&now, tm);
#endif
    return true;
}

/* The chosen moment in this panel's time zone; false when it doesn't exist. */
static bool set_moment(struct tm *tm, time_t *t)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = (int)s_set.year - 1900;
    tm->tm_mon = (int)s_set.month - 1;
    tm->tm_mday = (int)s_set.day;
    tm->tm_hour = (int)s_set.hour;
    tm->tm_min = (int)s_set.minute;
    tm->tm_isdst = -1; /* whatever daylight saving says for that day */
    *t = mktime(tm);
    return *t != (time_t)-1;
}

/* The sheet over Time and language says it in full; the setup's narrower step, short. */
static bool picker_in_full(lv_obj_t *picker)
{
    return lv_obj_get_screen(picker) == S.main;
}

/* "The clock will say": the chosen moment in words, as this keypad writes dates. */
static void picker_result(lv_obj_t *picker)
{
    char text[72];
    struct tm tm;
    time_t t;
    if (set_moment(&tm, &t)) {
        i18n_civil_t c;
        civil_from_tm(&c, &tm);
        const char *pattern;
        if (picker_in_full(picker)) {
            /* TR: Set the date and time, after "The clock will say": the date and time chosen in
             * full on a 12-hour clock, in CLDR pattern letters (EEEE weekday, MMMM month, d day, y
             * year, h hour 1-12, mm minute, a AM/PM). The order may change. At most 71 bytes of
             * result */
            const char *const pattern12 = TR_C("date pattern", "EEEE, MMMM d, y · h:mm a");
            /* TR: Set the date and time, after "The clock will say": the date and time chosen in
             * full on a 24-hour clock, in CLDR pattern letters (EEEE weekday, MMMM month, d day, y
             * year, HH hour 00-23, mm minute). The order may change. At most 71 bytes of result */
            const char *const pattern24 = TR_C("date pattern", "EEEE, MMMM d, y · HH:mm");
            pattern = ui_clock_24h() ? pattern24 : pattern12;
        } else {
            /* Short names: the setup's step is narrower ("Wed, Sep 30, 2026 · 12:54 AM").
             * TR: the date and time chosen in Set the date and time, on a 12-hour clock, in CLDR
             * pattern letters (EEE weekday short, MMM month short, d day, y year, h hour 1-12, mm
             * minute, a AM/PM). The order may change. At most 40 bytes of result */
            const char *const pattern12 = TR_C("date pattern", "EEE, MMM d, y · h:mm a");
            /* TR: the date and time chosen in Set the date and time, on a 24-hour clock, in CLDR
             * pattern letters (EEE weekday short, MMM month short, d day, y year, HH hour 00-23, mm
             * minute). The order may change. At most 40 bytes of result */
            const char *const pattern24 = TR_C("date pattern", "EEE, MMM d, y · HH:mm");
            pattern = ui_clock_24h() ? pattern24 : pattern12;
        }
        i18n_date_in(text, sizeof(text), S.catalog, ui_region(), pattern, &c);
    } else {
        i18n_utf8_copy(text, sizeof(text), TR("Not a time this panel can use"));
    }
    ui_set_label(picker, "clock_result", text);
}

/* The column of the 1st, in the region's week (I18N-016: Sunday or Monday first). */
static int32_t first_column(void)
{
    const int32_t first = (int32_t)ui_region()->first_day;
    return (weekday_of(s_set.year, s_set.month, 1) - first + 7) % 7;
}

/* The calendar for the chosen month: its heading, the week's initials, the days (the chosen one
 * filled, today's ringed) and the month and year rollers; then the words under it. */
static void picker_refresh(lv_obj_t *picker)
{
    const int32_t last = days_in(s_set.year, s_set.month);
    if (s_set.day > last) {
        s_set.day = last; /* a day the month doesn't have becomes its last */
    }
    i18n_civil_t c;
    memset(&c, 0, sizeof(c));
    c.year = (int16_t)s_set.year;
    c.month = (uint8_t)s_set.month;
    c.day = 1;
    c.weekday = (uint8_t)weekday_of(s_set.year, s_set.month, 1);
    char text[48];
    /* TR: Set the date and time, the calendar's heading: the month on show, in CLDR pattern letters
     * (MMMM month, y year). The order may change. At most 47 bytes of result */
    i18n_date_in(text, sizeof(text), S.catalog, ui_region(), TR_C("date pattern", "MMMM y"), &c);
    ui_set_label(picker, "cal_month", text);

    const int32_t first = (int32_t)ui_region()->first_day;
    lv_obj_t *week = ui_find(picker, "cal_week");
    for (int32_t col = 0; col < 7 && week != NULL; col++) {
        lv_obj_t *label = lv_obj_get_child(week, col);
        if (label == NULL) {
            break;
        }
        c.weekday = (uint8_t)((first + col) % 7);
        char name[24];
        i18n_date(name, sizeof(name), S.catalog, NOTR("EEE"), &c);
        size_t n = name[0] != '\0' ? 1u : 0u; /* the short name's first letter: S M T W T F S */
        while (name[n] != '\0' && ((unsigned)(unsigned char)name[n] & 0xC0u) == 0x80u) {
            n++;
        }
        name[n] = '\0';
        if (strcmp(lv_label_get_text(label), name) != 0) {
            lv_label_set_text(label, name);
        }
    }

    struct tm now;
    const bool known = local_now(&now);
    const bool this_month = known && now.tm_year + 1900 == s_set.year && now.tm_mon + 1 == s_set.month;
    const int32_t col1 = first_column();
    lv_obj_t *days = ui_find(picker, "cal_days");
    const uint32_t rows = days != NULL ? lv_obj_get_child_count(days) : 0u;
    for (uint32_t r = 0; r < rows; r++) {
        lv_obj_t *row = lv_obj_get_child(days, (int32_t)r);
        const uint32_t cells = lv_obj_get_child_count(row);
        for (uint32_t k = 0; k < cells; k++) {
            lv_obj_t *cell = lv_obj_get_child(row, (int32_t)k);
            const int32_t d = (int32_t)(r * 7u + k) - col1 + 1;
            const bool real = d >= 1 && d <= last;
            char num[8] = "";
            if (real) {
                snprintf(num, sizeof(num), "%d", (int)d);
            }
            ui_set_label(cell, "day_label", num);
            lv_obj_set_flag(cell, LV_OBJ_FLAG_CLICKABLE, real);
            lv_obj_set_state(cell, LV_STATE_CHECKED, real && d == s_set.day);
            lv_obj_set_state(cell, LV_STATE_USER_1, real && this_month && d == now.tm_mday);
        }
    }
    lv_obj_t *month = ui_find(picker, "roll_month");
    if (month != NULL) {
        lv_roller_set_selected(month, (uint32_t)(s_set.month - 1), LV_ANIM_OFF);
    }
    lv_obj_t *year = ui_find(picker, "roll_year");
    if (year != NULL) {
        lv_roller_set_selected(year, (uint32_t)(s_set.year - SET_YEAR_FIRST), LV_ANIM_OFF);
    }
    picker_result(picker);
}

/* The month before or after, within the years the panel can be set to. */
static void cal_move(lv_obj_t *picker, int32_t months)
{
    int32_t index = s_set.year * 12 + (s_set.month - 1) + months;
    if (index < SET_YEAR_FIRST * 12) {
        index = SET_YEAR_FIRST * 12;
    } else if (index > SET_YEAR_LAST * 12 + 11) {
        index = SET_YEAR_LAST * 12 + 11;
    }
    s_set.year = index / 12;
    s_set.month = index % 12 + 1;
    picker_refresh(picker);
}

static void on_cal_prev(lv_event_t *e)
{
    cal_move((lv_obj_t *)lv_event_get_user_data(e), -1);
}

static void on_cal_next(lv_event_t *e)
{
    cal_move((lv_obj_t *)lv_event_get_user_data(e), 1);
}

/* "September 2026": month and year rollers where the days were, or the days again. The
 * calendar keeps its height, so nothing around it moves. */
static void cal_pick_show(lv_obj_t *picker, bool show)
{
    lv_obj_t *body = ui_find(picker, "cal_body");
    lv_obj_t *pick = ui_find(picker, "cal_pick");
    lv_obj_t *week = ui_find(picker, "cal_week");
    lv_obj_t *days = ui_find(picker, "cal_days");
    if (body == NULL || pick == NULL || week == NULL || days == NULL) {
        return;
    }
    if (show) {
        lv_obj_update_layout(body);
        lv_obj_set_height(body, lv_obj_get_height(body));
    } else {
        lv_obj_set_height(body, LV_SIZE_CONTENT);
    }
    lv_obj_set_flag(week, LV_OBJ_FLAG_HIDDEN, show);
    lv_obj_set_flag(days, LV_OBJ_FLAG_HIDDEN, show);
    lv_obj_set_flag(pick, LV_OBJ_FLAG_HIDDEN, !show);
}

static void on_cal_title(lv_event_t *e)
{
    lv_obj_t *picker = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *pick = ui_find(picker, "cal_pick");
    cal_pick_show(picker, pick != NULL && lv_obj_has_flag(pick, LV_OBJ_FLAG_HIDDEN));
}

static void on_cal_day(lv_event_t *e)
{
    lv_obj_t *picker = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *cell = lv_event_get_current_target_obj(e);
    const int32_t d = (int32_t)(intptr_t)lv_obj_get_user_data(cell) - first_column() + 1;
    if (d >= 1 && d <= days_in(s_set.year, s_set.month)) {
        s_set.day = d;
        picker_refresh(picker);
    }
}

static void on_cal_month(lv_event_t *e)
{
    s_set.month = (int32_t)lv_roller_get_selected(lv_event_get_current_target_obj(e)) + 1;
    picker_refresh((lv_obj_t *)lv_event_get_user_data(e));
}

static void on_cal_year(lv_event_t *e)
{
    s_set.year = SET_YEAR_FIRST + (int32_t)lv_roller_get_selected(lv_event_get_current_target_obj(e));
    picker_refresh((lv_obj_t *)lv_event_get_user_data(e));
}

static void on_clock_time(lv_event_t *e)
{
    lv_obj_t *picker = (lv_obj_t *)lv_event_get_user_data(e);
    const int32_t m = ui_time_rollers_get(ui_find(picker, "clock_time"));
    s_set.hour = m / 60;
    s_set.minute = m % 60;
    picker_result(picker);
}

/* Fills a clock_picker from the chosen moment: its days (cal_day cells, made here), the month
 * names and years, the time rollers, and hooks up what a tap does. */
static void picker_bind(lv_obj_t *picker)
{
    if (picker == NULL) {
        return;
    }
    lv_obj_t *days = ui_find(picker, "cal_days");
    const uint32_t rows = days != NULL ? lv_obj_get_child_count(days) : 0u;
    for (uint32_t r = 0; r < rows; r++) {
        lv_obj_t *row = lv_obj_get_child(days, (int32_t)r);
        if (lv_obj_get_child_count(row) != 0u) {
            continue;
        }
        for (uint32_t k = 0; k < 7u; k++) {
            lv_obj_t *cell = cal_day_create(row);
            lv_obj_set_user_data(cell, (void *)(intptr_t)(r * 7u + k));
            lv_obj_add_event_cb(cell, on_cal_day, LV_EVENT_CLICKED, picker);
        }
    }
    ui_event_once(ui_find(picker, "cal_prev"), on_cal_prev, LV_EVENT_CLICKED, picker);
    ui_event_once(ui_find(picker, "cal_next"), on_cal_next, LV_EVENT_CLICKED, picker);
    ui_event_once(ui_find(picker, "cal_title"), on_cal_title, LV_EVENT_CLICKED, picker);

    char options[400];
    size_t used = 0;
    i18n_civil_t c;
    memset(&c, 0, sizeof(c));
    c.year = 2026;
    c.day = 1;
    for (uint8_t m = 1; m <= 12u && used + 2u < sizeof(options); m++) {
        c.month = m;
        if (m > 1u) {
            options[used++] = '\n';
        }
        used += i18n_date(options + used, sizeof(options) - used, S.catalog, NOTR("MMMM"), &c);
    }
    options[used] = '\0';
    ui_roller_fill(ui_find(picker, "roll_month"), options, s_set.month - 1, false);
    ui_event_once(ui_find(picker, "roll_month"), on_cal_month, LV_EVENT_VALUE_CHANGED, picker);
    used = 0;
    for (int32_t y = SET_YEAR_FIRST; y <= SET_YEAR_LAST && used + 6u < sizeof(options); y++) {
        used += (size_t)snprintf(options + used, sizeof(options) - used, y > SET_YEAR_FIRST ? "\n%d" : "%d",
                                 (int)y);
    }
    ui_roller_fill(ui_find(picker, "roll_year"), options, s_set.year - SET_YEAR_FIRST, false);
    ui_event_once(ui_find(picker, "roll_year"), on_cal_year, LV_EVENT_VALUE_CHANGED, picker);
    cal_pick_show(picker, false);

    ui_time_rollers_setup(ui_find(picker, "clock_time"), s_set.hour * 60 + s_set.minute, 1, on_clock_time,
                          picker);
    picker_refresh(picker);
}

/* Start from the panel's time, or from the start of 2026 when it has none. */
void ui_clock_fields_start(void)
{
    struct tm tm;
    if (!local_now(&tm)) {
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = 126;
        tm.tm_mday = 1;
    }
    s_set.year = tm.tm_year + 1900;
    s_set.month = tm.tm_mon + 1;
    s_set.day = tm.tm_mday;
    s_set.hour = tm.tm_hour;
    s_set.minute = tm.tm_min;
    if (s_set.year < SET_YEAR_FIRST || s_set.year > SET_YEAR_LAST) {
        s_set.year = s_set.year < SET_YEAR_FIRST ? SET_YEAR_FIRST : SET_YEAR_LAST;
    }
}

/* Sets the clock to the chosen moment; false (with the error beep and a toast) when it can't. */
bool ui_clock_set_chosen(void)
{
    struct tm tm;
    time_t t;
    char why[64] = "";
    if (!set_moment(&tm, &t)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not a time this panel can use"));
        return false;
    }
    if (S.be.clock_set == NULL ||
        !S.be.clock_set(S.be.ctx, (int64_t)t * 1000, S.auth_user, why, sizeof(why))) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] != '\0' ? ui_answer("why", why) : TR("The clock was not set"));
        return false;
    }
    S.last_minute = -1; /* the clock on screen shows the new time */
    return true;
}

/* The setup offers the date and time only where nothing else will set the clock (RTC-015): a
 * clock module that can keep a time, no time set yet, and no internet to set it. */
bool ui_clock_setup_needed(void)
{
    ui_clock_info_t c;
    memset(&c, 0, sizeof(c));
    if (S.be.clock_set == NULL || S.be.clock_info == NULL) {
        return false;
    }
    S.be.clock_info(S.be.ctx, &c);
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    if (S.be.wifi_info != NULL) {
        S.be.wifi_info(S.be.ctx, &w);
    }
    return ui_clock_settable(&c) && c.source == UI_CLOCK_NOT_SET && !w.time_synced;
}

/* ---- Set the date and time: the sheet over Time and language ----------------------------------- */

/* The days are made again the next time it opens: their objects aren't kept while it is shut. */
static void clock_sheet_closed(void)
{
    lv_obj_t *days = ui_find(ui_find(S.main, "clock_sheet_picker"), "cal_days");
    const uint32_t rows = days != NULL ? lv_obj_get_child_count(days) : 0u;
    for (uint32_t r = 0; r < rows; r++) {
        lv_obj_clean(lv_obj_get_child(days, (int32_t)r));
    }
}

static bool clock_sheet_set(void)
{
    if (!ui_clock_set_chosen()) {
        return false; /* the error beep and toast are up; the sheet stays */
    }
    ui_page_rebuild(); /* Time and language now says "Set by hand" */
    return true;
}

static void clock_sheet_show(void)
{
    lv_obj_t *picker = ui_find(S.main, "clock_sheet_picker");
    if (picker == NULL) {
        return;
    }
    ui_clock_fields_start();
    ui_sheet_open(SHEET_CLOCK, clock_sheet_set, clock_sheet_closed);
    picker_bind(picker);
}

static void open_set_clock(void *ctx)
{
    (void)ctx;
    if (ui_settings_allowed(ACCESS_SHARED)) {
        clock_sheet_show();
    } else {
        ui_page_open(PAGE_SET_CLOCK, 0); /* a master or installer code first */
    }
}

static void set_clock_unlocked(void *unused)
{
    (void)unused;
    if (S.depth > 1u && S.pages[S.depth - 1u].id == PAGE_SET_CLOCK) {
        ui_page_back(); /* under the sheet: Time and language */
    }
}

/* PAGE_SET_CLOCK once a code opened it: the sheet opens at once, and the page under it goes back
 * to Time and language, where Set and Cancel leave the person. */
void ui_clock_build(lv_obj_t *body, char *note, size_t nlen)
{
    (void)body;
    (void)note;
    (void)nlen;
    clock_sheet_show();
    lv_async_call(set_clock_unlocked, NULL);
}

/* The setup's date and time step (RTC-015): the same calendar and rollers, narrower. */
void ui_clock_picker(lv_obj_t *body)
{
    picker_bind(clock_picker_create(body, CAL_W_SETUP, CAL_CELL_H_SETUP, ROLLER_W_SETUP, ROLLER_AMPM_W_SETUP,
                                    SPACE_LG, SPACE_MD));
}
