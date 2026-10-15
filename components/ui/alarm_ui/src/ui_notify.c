/*
 * ui_notify.c - Integrations > Notifications: ntfy and Pushover (docs/features/26, /27, /28).
 *
 * Two services that put an alert on a phone without a server of the owner's own. Each has its
 * own page and its own row in the Integrations list, beside Home Assistant and MQTT.
 *
 * The shape of these pages follows ui_integrations.c: a switch whose second line says the state,
 * rewritten once a second and never reflowed (UI-020, UI-023), then what a person does, the
 * settings last; every edit saves at once and only a refusal gets words (UI-028).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define STATUS_POLL_MS 1000u
#define NTFY_HOSTED    "https://ntfy.sh"

static ui_ntfy_settings_t s_ntfy;
static ui_pushover_settings_t s_push;
static lv_obj_t *s_row[UI_NOTIFY_PROVIDERS]; /* the live status rows, NULL when none */
static page_id_t s_page;
static uint32_t s_polled;
static uint8_t s_editing; /* which provider the open sheet belongs to */
/* What the page last asked the platform for, since both answers arrive the same way. */
typedef enum { WAIT_NOTHING = 0, WAIT_TEST, WAIT_KEYCHECK } wait_kind_t;
static wait_kind_t s_waiting_for;
static uint32_t s_qr_until; /* lv_tick the QR code goes away at, 0 = not showing */

/* The panel offers notifications at all only when the platform wired them up. */
bool ui_notify_available(void)
{
    return S.be.notify_status != NULL && S.be.ntfy_get != NULL && S.be.ntfy_set != NULL &&
           S.be.pushover_get != NULL && S.be.pushover_set != NULL;
}

static void read_status(uint8_t provider, ui_notify_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (S.be.notify_status != NULL) {
        S.be.notify_status(S.be.ctx, provider, st);
    }
}

/* "On · last sent 2 minutes ago", "On · last sent just now" - the one live phrase on these pages,
 * the line under the switch. It settles to the minute, so the words change at most once a minute
 * and never twitch (UI-020, UI-023). */
static void when_words(const ui_notify_status_t *st, char *buf, size_t len)
{
    if (!st->ever_sent) {
        /* TR: The line under a notification switch, nothing sent yet. At most 111 bytes */
        i18n_utf8_copy(buf, len, TR("On · nothing sent yet"));
        return;
    }
    const unsigned mins = (unsigned)(st->ago_s / 60u);
    if (mins == 0u) {
        /* TR: The line under a notification switch. At most 111 bytes */
        i18n_utf8_copy(buf, len, TR("On · last sent just now"));
    } else if (mins == 1u) {
        /* TR: The line under a notification switch. At most 111 bytes */
        i18n_utf8_copy(buf, len, TR("On · last sent a minute ago"));
    } else if (mins < 60u) {
        /* TR: The line under a notification switch, {0} the number of minutes. At most 111 bytes */
        I18N_FORMAT(buf, len, TRN("On · last sent {0} minute ago", "On · last sent {0} minutes ago", mins), mins);
    } else if (mins < 120u) {
        /* TR: The line under a notification switch. At most 111 bytes */
        i18n_utf8_copy(buf, len, TR("On · last sent an hour ago"));
    } else {
        /* TR: The line under a notification switch, {0} the number of hours. At most 111 bytes */
        I18N_FORMAT(buf, len, TRN("On · last sent {0} hour ago", "On · last sent {0} hours ago", mins / 60u),
                    mins / 60u);
    }
}

/* A tenth of Pushover's 10,000 a month. */
#define PUSHOVER_QUOTA_LOW 1000

/* What the service is doing right now in words: `list` is the line under its name in the
 * Integrations list, `page` the line under its switch (it carries the state). An alert is a state
 * that needs a person; the page tints it. */
static void status_words(uint8_t provider, const ui_notify_status_t *st, char *list, size_t llen,
                         char *page, size_t plen, bool *alert, lv_color_t *tone)
{
    const bool hosted = provider == UI_NOTIFY_NTFY && !s_ntfy.self_hosted;
    *alert = false;
    *tone = ui_tone_amber();
    page[0] = '\0';
    switch (st->state) {
    case UI_NOTIFY_NO_NETWORK:
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Waiting for Wi-Fi"));
        /* TR: The line under a notification switch, switched on but the panel is offline. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · waiting for Wi-Fi, alerts are kept until it is back"));
        *alert = true;
        break;
    case UI_NOTIFY_READY:
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen,
                       provider == UI_NOTIFY_NTFY
                           ? (hosted ? TR("Sending to ntfy.sh") : TR("Sending to your own server"))
                           : TR_C("notification service", "Ready"));
        /* TR: The line under a notification switch, nothing sent yet. At most 111 bytes */
        i18n_utf8_copy(page, plen,
                       provider == UI_NOTIFY_NTFY
                           ? (hosted ? TR("On · sending to ntfy.sh, nothing sent yet")
                                     : TR("On · sending to your own server, nothing sent yet"))
                           : TR("On · nothing sent yet"));
        break;
    case UI_NOTIFY_SENDING:
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Sending"));
        /* TR: The line under a notification switch while one goes out. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · sending"));
        break;
    case UI_NOTIFY_OK:
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen,
                       provider == UI_NOTIFY_NTFY
                           ? (hosted ? TR("Sending to ntfy.sh") : TR("Sending to your own server"))
                           : TR("Connected"));
        when_words(st, page, plen);
        /* PUSH-011: say so while there is still allowance, not at the end of the month. */
        if (provider == UI_NOTIFY_PUSHOVER && st->quota_left >= 0 &&
            st->quota_left < PUSHOVER_QUOTA_LOW) {
            const uint32_t left = (uint32_t)st->quota_left;
            /* TR: Pushover's monthly allowance is running out. {0} is the number left. At most 111 bytes */
            I18N_FORMAT(page, plen,
                        TRN("On · only {0} message left this month", "On · only {0} messages left this month",
                            left),
                        left);
            *alert = true;
        }
        break;
    case UI_NOTIFY_PAUSED:
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Too many today"));
        /* TR: The line under a notification switch. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · too many today, paused until tomorrow so alarms still get through"));
        *alert = true;
        break;
    case UI_NOTIFY_FAILED:
        switch (st->fail) {
        case UI_NOTIFY_FAIL_AUTH:
            /* TR: The notification service's state in the Integrations list. At most 63 bytes */
            i18n_utf8_copy(list, llen,
                           provider == UI_NOTIFY_NTFY ? TR("The server refused the token")
                                                      : TR("Pushover refused the keys"));
            break;
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        case UI_NOTIFY_FAIL_UNREACHABLE: i18n_utf8_copy(list, llen, TR("Can't reach the service")); break;
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        case UI_NOTIFY_FAIL_TLS: i18n_utf8_copy(list, llen, TR("Certificate not trusted")); break;
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        case UI_NOTIFY_FAIL_QUOTA: i18n_utf8_copy(list, llen, TR("The monthly allowance is used up")); break;
        /* TR: The notification service's state in the Integrations list. At most 63 bytes */
        default: i18n_utf8_copy(list, llen, TR("Can't send")); break;
        }
        if (st->detail[0] != '\0') {
            /* TR: The line under a notification switch when sending failed: {0} what went wrong
             * (the service's words above), {1} the reason. At most 111 bytes */
            I18N_FORMAT(page, plen, TR("{0} · {1}"), list, ui_answer("why", st->detail));
        } else {
            /* TR: The line under a notification switch when sending failed: {0} what went wrong. At most 111 bytes */
            I18N_FORMAT(page, plen, TR("{0} · it keeps trying"), list);
        }
        *alert = true;
        *tone = ui_tone_red();
        break;
    default:
        /* TR: The notification service's state in the Integrations list when it is off. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR_C("integration status", "Off"));
        /* TR: The line under a notification switch when it is off. At most 111 bytes */
        i18n_utf8_copy(page, plen,
                       provider == UI_NOTIFY_NTFY
                           ? TR("Off · alerts go to every phone on this topic")
                           : TR("Off · alerts go to every device on your Pushover account"));
        break;
    }
}

/* The list shows the state on the row's second line; the page puts it under the switch. */
static void fill_row(lv_obj_t *row, const char *list, const char *page, bool alert, lv_color_t tone)
{
    if (row == NULL) {
        return;
    }
    if (s_page == PAGE_INTEGRATIONS) {
        ui_row_set_detail(row, list, false, tone);
    } else {
        ui_row_set_detail(row, page, alert, tone);
    }
}

static void show(uint8_t provider)
{
    if (s_row[provider] == NULL) {
        return;
    }
    ui_notify_status_t st;
    read_status(provider, &st);
    char list[64];
    char page[112];
    bool alert;
    lv_color_t tone;
    status_words(provider, &st, list, sizeof(list), page, sizeof(page), &alert, &tone);
    fill_row(s_row[provider], list, page, alert, tone);
}

void ui_notify_stop(void)
{
    s_row[UI_NOTIFY_NTFY] = NULL;
    s_row[UI_NOTIFY_PUSHOVER] = NULL;
    if (s_waiting_for != WAIT_NOTHING) {
        s_waiting_for = WAIT_NOTHING; /* the answer has nowhere to land now */
        ui_set_int(&subject_busy, 0);
    }
}

/* Leaving the page puts the QR code away: it must never come back on its own (NTFY-011). */
void ui_notify_page_left(void)
{
    s_qr_until = 0u;
}

static void collect_test(void);

void ui_notify_tick(void)
{
    if (s_row[UI_NOTIFY_NTFY] == NULL && s_row[UI_NOTIFY_PUSHOVER] == NULL) {
        return;
    }
    if (lv_tick_elaps(s_polled) < STATUS_POLL_MS) {
        return;
    }
    s_polled = lv_tick_get();
    if (s_qr_until != 0u && lv_tick_get() >= s_qr_until) {
        s_qr_until = 0u;
        ui_page_rebuild();
        return; /* the rows this tick would have written are gone */
    }
    collect_test();
    show(UI_NOTIFY_NTFY);
    show(UI_NOTIFY_PUSHOVER);
}

static void rebuild_later(void *unused)
{
    (void)unused;
    ui_page_rebuild();
}

/* Every edit saves at once. Only a refusal gets words (UI-028). */
static void save_ntfy(void)
{
    char why[96] = "";
    if (S.be.ntfy_set != NULL && !S.be.ntfy_set(S.be.ctx, &s_ntfy, why, sizeof(why))) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] ? ui_answer("why", why) : TR("Not saved"));
    }
    s_ntfy.token_change = false;
    memset(s_ntfy.token, 0, sizeof(s_ntfy.token));
    lv_async_call(rebuild_later, NULL);
}

static void save_push(void)
{
    char why[96] = "";
    if (S.be.pushover_set != NULL && !S.be.pushover_set(S.be.ctx, &s_push, why, sizeof(why))) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] ? ui_answer("why", why) : TR("Not saved"));
    }
    if ((s_push.user_key_change || s_push.app_token_change) && s_waiting_for == WAIT_NOTHING) {
        s_waiting_for = WAIT_KEYCHECK; /* the platform asks Pushover about it (PUSH-006) */
    }
    s_push.user_key_change = false;
    s_push.app_token_change = false;
    memset(s_push.user_key, 0, sizeof(s_push.user_key));
    memset(s_push.app_token, 0, sizeof(s_push.app_token));
    lv_async_call(rebuild_later, NULL);
}

/* ---- what to send: the five categories, the same on both pages ---------------------------- */

static void on_category(bool on, void *ctx)
{
    const unsigned cat = (unsigned)(uintptr_t)ctx & 0xFFu;
    const uint8_t bit = NOTIFY_CAT_BIT(cat);
    uint8_t *cats = s_editing == UI_NOTIFY_NTFY ? &s_ntfy.cats : &s_push.cats;
    if (on) {
        *cats |= bit;
    } else {
        *cats &= (uint8_t)~bit;
    }
    if (s_editing == UI_NOTIFY_NTFY) {
        save_ntfy();
    } else {
        save_push();
    }
}

static void on_silent(bool on, void *ctx)
{
    (void)ctx;
    if (s_editing == UI_NOTIFY_NTFY) {
        s_ntfy.silent_too = on;
        save_ntfy();
    } else {
        s_push.silent_too = on;
        save_push();
    }
}

static void category_rows(lv_obj_t *body, uint8_t cats, bool silent_too)
{
    /* TR: a row of the notification page's "What to send" list (a switch and its line under it) */
    static const struct {
        uint8_t cat;
        const char *title;
        const char *detail;
    } ROWS[] = {
        {NOTIFY_CAT_ALARM, TR_NOOP_C("notification category", "Alarms"),
         TR_NOOP("Burglary, fire, carbon monoxide, panic")},
        {NOTIFY_CAT_ARMING, TR_NOOP_C("notification category", "Arming and disarming"),
         TR_NOOP("And who did it")},
        {NOTIFY_CAT_TROUBLE, TR_NOOP_C("notification category", "Troubles"),
         TR_NOOP("Mains power, batteries, tamper, a sensor gone quiet")},
        {NOTIFY_CAT_BYPASS, TR_NOOP_C("notification category", "Zone activity"),
         TR_NOOP("Bypassed zones and the door chime")},
        {NOTIFY_CAT_SYSTEM, TR_NOOP_C("notification category", "System and admin"),
         TR_NOOP("Restarts, updates, code and setting changes")},
    };
    ui_section(body, TR("What to send"));
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        ui_row_toggle(body, TR_C("notification category", ROWS[i].title), TR(ROWS[i].detail),
                      (cats & NOTIFY_CAT_BIT(ROWS[i].cat)) != 0u, on_category,
                      (void *)(uintptr_t)ROWS[i].cat);
    }
    if ((cats & NOTIFY_CAT_BIT(NOTIFY_CAT_ALARM)) == 0u) {
        ui_paragraph(body,
                     TR("With alarms off, nothing tells you when the siren goes. The panel still "
                     "sounds and records it."),
                     ui_tone_amber());
    }
    ui_row_toggle(body, TR("Silent alarms too"),
                  TR("A duress code or a silent panic. Off: they stay in the house"), silent_too,
                  on_silent, NULL);
    if (silent_too) {
        ui_paragraph(body,
                     TR("A silent alarm will show on your phone's lock screen, where somebody "
                     "standing next to you can see it."),
                     ui_tone_amber());
    }
}

/* ---- sending a test ------------------------------------------------------------------------ */

/* Posting a test takes as long as the service takes to answer, so the button only starts it.
 * The answer is collected by ui_notify_tick(), which is also what keeps the status row live -
 * the screen must never wait on a network request (one LVGL thread). */
static void send_test(void *ctx)
{
    const uint8_t provider = (uint8_t)(uintptr_t)ctx;
    static char why[96];
    if (S.be.notify_test == NULL) {
        return;
    }
    why[0] = '\0';
    if (!S.be.notify_test(S.be.ctx, provider, why, sizeof(why))) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] != '\0' ? ui_answer("why", why) : TR("It didn't go"));
        return;
    }
    s_waiting_for = WAIT_TEST;
    ui_set_int(&subject_busy, 1);
}

static void collect_test(void)
{
    static char why[96];
    if (s_waiting_for == WAIT_NOTHING || S.be.notify_test_result == NULL) {
        return;
    }
    bool ok = false;
    why[0] = '\0';
    if (!S.be.notify_test_result(S.be.ctx, &ok, why, sizeof(why))) {
        return; /* still trying */
    }
    const wait_kind_t was = s_waiting_for;
    s_waiting_for = WAIT_NOTHING;
    ui_set_int(&subject_busy, 0);
    if (!ok) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] != '\0' ? ui_answer("why", why)
                                 : (was == WAIT_TEST ? TR("It didn't go") : TR("That key wasn't right")));
    } else if (was == WAIT_TEST) {
        /* The outcome is on the phone, not on this screen, so it is worth saying (UI-028). */
        ui_toast(TR("Sent - check your phone"));
    }
    /* A key that checked out says nothing: the row already reads "Set". */
}

/* ---- ntfy --------------------------------------------------------------------------------- */

/* ---- the QR code: the shortest way to get a topic onto a phone --------------------------- */

#define QR_SHOWN_MS 60000u
#define QR_SIZE     180

/* Where a Pushover application is created (PUSH-003: every owner makes their own). */
#define PUSHOVER_APP_URL "https://pushover.net/apps/build"

/* The address the ntfy app subscribes to. */
static void subscribe_url(char *buf, size_t len)
{
    const char *base = s_ntfy.self_hosted && s_ntfy.server[0] != '\0' ? s_ntfy.server
                                                                      : NTFY_HOSTED;
    snprintf(buf, len, "%s/%s", base, s_ntfy.topic);
}

static void open_qr(void *ctx)
{
    (void)ctx;
    s_qr_until = lv_tick_get() + QR_SHOWN_MS;
    lv_async_call(rebuild_later, NULL);
}

static void hide_qr(void *ctx)
{
    (void)ctx;
    s_qr_until = 0u;
    lv_async_call(rebuild_later, NULL);
}

/* A still picture, never an animation: nothing on this screen may move (UI-020). */
static void show_qr(lv_obj_t *body, const char *url, const char *note, lv_color_t tone)
{

    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *qr = lv_qrcode_create(box);
    lv_qrcode_set_size(qr, QR_SIZE);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, (uint32_t)strlen(url));
    /* A quiet zone, or a phone camera struggles to find the code against a dark screen. */
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);

    ui_paragraph(body, note, tone);
    ui_row_action(body, UI_ICON_CIRCLE_X, TR("Hide the QR code"), ui_tone_gray(), hide_qr, NULL);
}

static void topic_changed(const char *text)
{
    /* ntfy allows letters, digits, - and _, up to 64 characters. */
    for (const char *p = text; *p != '\0'; p++) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        if (!ok) {
            ui_beep(UI_BEEP_ERROR);
            ui_toast(TR("Letters, digits, - and _ only"));
            return;
        }
    }
    if (text[0] == '\0') {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("A topic can't be empty"));
        return;
    }
    snprintf(s_ntfy.topic, sizeof(s_ntfy.topic), "%s", text);
    save_ntfy();
}

static void edit_topic(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Topic"),
                       TR("Anyone who knows this can read your alerts, so keep it to yourself"),
                       s_ntfy.topic, UI_TEXT_PLAIN, 63, topic_changed);
}

static void new_topic_confirmed(void)
{
    if (S.be.ntfy_new_topic == NULL) {
        return;
    }
    S.be.ntfy_new_topic(S.be.ctx, s_ntfy.topic, sizeof(s_ntfy.topic));
    save_ntfy();
    ui_toast(TR("New topic - subscribe your phone again"));
}

static void new_topic(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("New topic?"),
               TR("Every phone subscribed to the old one stops getting alerts until you "
               "subscribe it again."),
               TR("New topic"), new_topic_confirmed);
}

static void server_changed(const char *text)
{
    if (strncmp(text, "http://", 7) != 0 && strncmp(text, "https://", 8) != 0) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Start with https:// or http://"));
        return;
    }
    snprintf(s_ntfy.server, sizeof(s_ntfy.server), "%s", text);
    save_ntfy();
}

static void edit_server(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Server address"), TR("Your own ntfy, like https://ntfy.example.com"),
                       s_ntfy.server, UI_TEXT_PLAIN, 95, server_changed);
}

static void where_chosen(int32_t index)
{
    const bool own = index == 1;
    if (own && s_ntfy.server[0] == '\0') {
        s_ntfy.self_hosted = true;
        ui_text_sheet_kind(TR("Server address"), TR("Your own ntfy, like https://ntfy.example.com"), "",
                           UI_TEXT_PLAIN, 95, server_changed);
        lv_async_call(rebuild_later, NULL);
        return;
    }
    s_ntfy.self_hosted = own;
    save_ntfy();
}

static void pick_where(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[2];
    choices[0] = (ui_choice_t){UI_ICON_GLOBE, ui_tone_blue(), NOTR("ntfy.sh") /* product name */,
                               TR("The free hosted service. Nothing to set up")};
    choices[1] = (ui_choice_t){UI_ICON_SERVER, ui_tone_violet(), TR("My own server"),
                               TR("An ntfy you run yourself")};
    ui_pick_sheet(TR("Where alerts go"), TR("Both work the same on the phone"), choices, 2,
                  s_ntfy.self_hosted ? 1 : 0, where_chosen);
}

static void ntfy_token_changed(const char *text)
{
    snprintf(s_ntfy.token, sizeof(s_ntfy.token), "%s", text);
    s_ntfy.token_change = true;
    save_ntfy();
}

static void edit_ntfy_token(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Access token"),
                       TR("Only for a protected server. Stored in the panel, never shown again"),
                       "", UI_TEXT_PASSWORD, 63, ntfy_token_changed);
}

static void on_ntfy_enabled(bool on, void *ctx)
{
    (void)ctx;
    s_ntfy.enabled = on;
    save_ntfy();
}

/* The page's order: the switch with its state, adding a phone and testing it, what to send, and
 * where the alerts go, last (the settings a person seldom touches). */
static void build_ntfy(lv_obj_t *body, char *note, size_t nlen)
{
    i18n_utf8_copy(note, nlen, TR("An alert on your phone, with no account to make"));
    if (!ui_notify_available()) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    s_editing = UI_NOTIFY_NTFY;
    S.be.ntfy_get(S.be.ctx, &s_ntfy);

    s_row[UI_NOTIFY_NTFY] =
        ui_row_toggle(body, TR("Send notifications"), " ", s_ntfy.enabled, on_ntfy_enabled, NULL);
    show(UI_NOTIFY_NTFY);

    ui_section(body, TR("Your phones"));
    /* The QR code carries the topic, which is the only thing protecting the alerts, and this
     * panel hangs where anyone walking past can photograph it. So it is not drawn on sight: it
     * is asked for, it says what it gives away, and it takes itself off the screen (NTFY-011). */
    if (s_qr_until != 0u) {
        char url[176];
        subscribe_url(url, sizeof(url));
        ui_paragraph(body,
                     TR("Install ntfy on your phone and add a subscription, then scan this code or "
                     "type the topic (under Where they go). The topic is the only thing protecting "
                     "your alerts: treat it like a password."),
                     ui_tone_gray());
        show_qr(body, url,
                TR("Point your phone's camera at this. Anyone who photographs it can read your "
                   "alerts, so don't leave it showing - it goes away by itself in a minute."),
                ui_tone_amber());
    } else {
        ui_row_link(body, UI_ICON_QR_CODE, ui_tone_violet(), TR("Add a phone"),
                    TR("Install ntfy, then scan a QR code or type the topic"), NULL,
                    UI_ICON_CHEVRON_RIGHT, open_qr, NULL);
    }
    ui_row_action(body, UI_ICON_SEND, TR("Send a test notification"), ui_tone_blue(), send_test,
                  (void *)(uintptr_t)UI_NOTIFY_NTFY);

    category_rows(body, s_ntfy.cats, s_ntfy.silent_too);

    ui_section(body, TR("Where they go"));
    ui_row_link(body, UI_ICON_GLOBE, ui_tone_blue(), TR("Server"),
                s_ntfy.self_hosted ? TR("Your own ntfy") : TR("The free hosted service"),
                s_ntfy.self_hosted ? (s_ntfy.server[0] ? s_ntfy.server : TR_C("setting value", "Not set"))
                                   : NOTR("ntfy.sh") /* product name */,
                UI_ICON_CHEVRON_RIGHT, pick_where, NULL);
    if (s_ntfy.self_hosted) {
        ui_row_link(body, UI_ICON_SERVER, ui_tone_blue(), TR("Address"), NULL,
                    s_ntfy.server[0] ? s_ntfy.server : TR_C("setting value", "Not set"), UI_ICON_CHEVRON_RIGHT,
                    edit_server, NULL);
    }
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Topic"),
                TR("Keep it private: anyone with it can read your alerts"),
                s_ntfy.topic, UI_ICON_CHEVRON_RIGHT, edit_topic, NULL);
    ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_blue(), TR("Access token"),
                TR("Only if your server asks for one"), s_ntfy.token_set ? TR_C("setting value", "Set") : TR_C("setting value", "None"),
                UI_ICON_CHEVRON_RIGHT, edit_ntfy_token, NULL);
    ui_row_link(body, UI_ICON_REFRESH, ui_tone_gray(), TR("New topic"),
                TR("Phones on the old topic stop getting alerts"), NULL, UI_ICON_CHEVRON_RIGHT,
                new_topic, NULL);
}

/* ---- Pushover ----------------------------------------------------------------------------- */

static bool key_looks_right(const char *text)
{
    size_t n = 0u;
    for (const char *p = text; *p != '\0'; p++, n++) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9');
        if (!ok) {
            return false;
        }
    }
    return n == 30u;
}

static void user_key_changed(const char *text)
{
    if (!key_looks_right(text)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("A user key is 30 letters and digits"));
        return;
    }
    snprintf(s_push.user_key, sizeof(s_push.user_key), "%s", text);
    s_push.user_key_change = true;
    save_push();
}

static void edit_user_key(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("User key"), TR("The 30-character key on your Pushover home page"), "",
                       UI_TEXT_PASSWORD, 30, user_key_changed);
}

static void app_token_changed(const char *text)
{
    if (!key_looks_right(text)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("An application token is 30 letters and digits"));
        return;
    }
    snprintf(s_push.app_token, sizeof(s_push.app_token), "%s", text);
    s_push.app_token_change = true;
    save_push();
}

static void edit_app_token(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Application token"),
                       TR("Create an application on pushover.net, then copy its token here"), "",
                       UI_TEXT_PASSWORD, 30, app_token_changed);
}

static void on_push_enabled(bool on, void *ctx)
{
    (void)ctx;
    s_push.enabled = on;
    save_push();
}

static void on_emergency(bool on, void *ctx)
{
    (void)ctx;
    s_push.emergency = on;
    save_push();
}

/* Pushover's sound ids (values, not words), and the words the picker and the row show for them. */
static const char *const SOUNDS[] = {NOTR("siren"), NOTR("alien"), NOTR("persistent"), NOTR("echo"), ""};
static const char *const SOUND_NAMES[] = {
    TR_NOOP_C("Pushover sound", "Siren"), TR_NOOP_C("Pushover sound", "Alien alarm"),
    TR_NOOP_C("Pushover sound", "Persistent"), TR_NOOP_C("Pushover sound", "Echo"),
    TR_NOOP_C("Pushover sound", "Whatever the app uses")};

static const char *sound_name(const char *id)
{
    for (size_t i = 0; i < sizeof(SOUNDS) / sizeof(SOUNDS[0]); i++) {
        if (strcmp(id, SOUNDS[i]) == 0) {
            return TR_C("Pushover sound", SOUND_NAMES[i]);
        }
    }
    return id; /* set by a newer firmware or the browser: its own id is the best name */
}

static void sound_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < sizeof(SOUNDS) / sizeof(SOUNDS[0])) {
        snprintf(s_push.sound, sizeof(s_push.sound), "%s", SOUNDS[index]);
        save_push();
    }
}

static void pick_sound(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[5];
    choices[0] = (ui_choice_t){UI_ICON_SIREN, ui_tone_red(), TR_C("Pushover sound", SOUND_NAMES[0]), TR("Hard to sleep through")};
    choices[1] = (ui_choice_t){UI_ICON_SIREN, ui_tone_orange(), TR_C("Pushover sound", SOUND_NAMES[1]), TR("Loud and urgent")};
    choices[2] =
        (ui_choice_t){UI_ICON_BELL_RING, ui_tone_amber(), TR_C("Pushover sound", SOUND_NAMES[2]), TR("Repeats while it shows")};
    choices[3] = (ui_choice_t){UI_ICON_BELL, ui_tone_blue(), TR_C("Pushover sound", SOUND_NAMES[3]), TR("A clear chime")};
    choices[4] = (ui_choice_t){UI_ICON_VOLUME, ui_tone_gray(), TR_C("Pushover sound", SOUND_NAMES[4]),
                               TR("Your Pushover default")};
    int32_t sel = 4;
    for (int32_t i = 0; i < 5; i++) {
        if (strcmp(s_push.sound, SOUNDS[i]) == 0) {
            sel = i;
            break;
        }
    }
    ui_pick_sheet(TR("Sound for alarms"), TR("Only alarms use it; everything else stays quiet-ish"),
                  choices, 5, sel, sound_chosen);
}

/* The same order as ntfy: the switch with its state, the test, what to send and the alarm sound,
 * then the account (set up once), last. */
static void build_pushover(lv_obj_t *body, char *note, size_t nlen)
{
    i18n_utf8_copy(note, nlen, TR("Alerts that keep going until you answer them"));
    if (!ui_notify_available()) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    s_editing = UI_NOTIFY_PUSHOVER;
    S.be.pushover_get(S.be.ctx, &s_push);

    s_row[UI_NOTIFY_PUSHOVER] =
        ui_row_toggle(body, TR("Send notifications"), " ", s_push.enabled, on_push_enabled, NULL);
    show(UI_NOTIFY_PUSHOVER);

    ui_row_action(body, UI_ICON_SEND, TR("Send a test notification"), ui_tone_blue(), send_test,
                  (void *)(uintptr_t)UI_NOTIFY_PUSHOVER);

    category_rows(body, s_push.cats, s_push.silent_too);

    ui_section(body, TR("Alerts"));
    ui_row_toggle(body, TR("Repeat alarms until answered"),
                  TR("An alarm keeps alerting your phone until you acknowledge it"),
                  s_push.emergency, on_emergency, NULL);
    ui_row_link(body, UI_ICON_VOLUME, ui_tone_violet(), TR("Sound for alarms"), NULL,
                sound_name(s_push.sound), UI_ICON_CHEVRON_RIGHT, pick_sound,
                NULL);
    if (s_push.emergency) {
        ui_paragraph(body,
                     TR("Disarming the panel stops the repeat on your phone, so a false alarm "
                     "doesn't keep buzzing."),
                     ui_tone_gray());
    }

    ui_section(body, TR("Your Pushover account"));
    ui_row_link(body, UI_ICON_USER, ui_tone_blue(), TR("User key"), TR("From your Pushover home page"),
                s_push.user_key_set ? TR_C("setting value", "Set") : TR_C("setting value", "Not set"), UI_ICON_CHEVRON_RIGHT, edit_user_key,
                NULL);
    ui_row_link(body, UI_ICON_KEY, ui_tone_blue(), TR("Application token"),
                TR("An application you create, so the allowance is yours"),
                s_push.app_token_set ? TR_C("setting value", "Set") : TR_C("setting value", "Not set"), UI_ICON_CHEVRON_RIGHT, edit_app_token,
                NULL);
    if (!s_push.user_key_set || !s_push.app_token_set) {
        ui_paragraph(body,
                     TR("On a computer: sign in at pushover.net, copy the user key from the home "
                     "page, then create an application and copy its token."),
                     ui_tone_gray());
        /* The page where the application is created: an address, not a secret, so it may show
         * (it still takes itself away like the ntfy one, one code at a time on the screen). */
        if (s_qr_until != 0u) {
            show_qr(body, PUSHOVER_APP_URL,
                    TR("Point your phone's camera at this to open the page where you create the "
                       "application. It goes away by itself in a minute."),
                    ui_tone_gray());
        } else {
            ui_row_action(body, UI_ICON_APP_WINDOW, TR("Show a QR code for the sign-up page"),
                          ui_tone_violet(), open_qr, NULL);
        }
    }
}

/* ---- the rows in the Integrations list ----------------------------------------------------- */

static void open_ntfy(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_NTFY, 0);
}

static void open_pushover(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_PUSHOVER, 0);
}

void ui_notify_list_rows(lv_obj_t *body)
{
    if (!ui_notify_available()) {
        return;
    }
    /* In the list the service keeps its name and only the line under it changes; the page gives
     * the state the whole row. fill_row() tells them apart by this. */
    s_page = PAGE_INTEGRATIONS;
    s_polled = lv_tick_get();
    S.be.ntfy_get(S.be.ctx, &s_ntfy);
    S.be.pushover_get(S.be.ctx, &s_push);
    s_row[UI_NOTIFY_NTFY] = ui_row_link(body, UI_ICON_BELL_RING, ui_tone_orange(), NOTR("ntfy"), " ",
                                        NULL, UI_ICON_CHEVRON_RIGHT, open_ntfy, NULL);
    show(UI_NOTIFY_NTFY);
    s_row[UI_NOTIFY_PUSHOVER] =
        ui_row_link(body, UI_ICON_RADIO_TOWER, ui_tone_blue(), NOTR("Pushover"), " ", NULL,
                    UI_ICON_CHEVRON_RIGHT, open_pushover, NULL);
    show(UI_NOTIFY_PUSHOVER);
}

void ui_notify_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen, char *note,
                     size_t nlen)
{
    ui_notify_stop();
    s_page = id;
    s_polled = lv_tick_get();
    snprintf(title, tlen, "%s", id == PAGE_NTFY ? NOTR("ntfy") : NOTR("Pushover")); /* product names */
    if (!ui_settings_allowed(ACCESS_SYSTEM)) {
        ui_settings_locked(body, ACCESS_SYSTEM, id == PAGE_NTFY ? NOTR("ntfy") : NOTR("Pushover"),
                           TR("The master or installer code sets up notifications"), note, nlen);
        return;
    }
    if (id == PAGE_NTFY) {
        build_ntfy(body, note, nlen);
    } else {
        build_pushover(body, note, nlen);
    }
}
