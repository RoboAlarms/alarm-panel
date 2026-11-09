/*
 * ui_integrations.c - Settings > Integrations: the ways another system talks to the panel. Two
 * of them, side by side, each with its own page: the Home Assistant integration (the panel is
 * found by mDNS and paired on the screen, docs/features/23 HAI-003) and MQTT, which speaks to any
 * broker - Node-RED, your own server, or Home Assistant without the integration (docs/features/10).
 * This section is the only place the UI names Home Assistant; the rest of the panel speaks of
 * "connected services" (docs/features/10, docs/features/14).
 *
 * Settings > Integrations > RoboAlarms links the panel to an alarm-server (BL-225, docs/features/43):
 * a code to type on the website or app, then Allow here once the panel names who is asking. A user's
 * page offers Add a phone (direct mode): a QR code, then Allow. Both are drawn in the mockup's
 * proposal BL-225-server-link.js.
 *
 * Opens with the system master or the installer code. Every change is handed to the backend at
 * once, which checks, stores and applies it.
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define STATUS_POLL_MS 1000u

static ui_mqtt_settings_t s_set; /* what the MQTT page shows and edits */
static lv_obj_t *s_ha_row;       /* the row saying how the integration is doing, NULL when none */
static lv_obj_t *s_mqtt_row;     /* the row saying how the broker connection is doing */
static ui_mqtt_status_t s_shown;
static ui_halink_status_t s_ha;  /* the link state the open page was built for */
static page_id_t s_page;         /* which page the rows belong to */
static uint32_t s_polled;
static bool s_enable_after_host; /* "Connect" was switched on before an address was set */
static lv_obj_t *s_srv_row;      /* the RoboAlarms row (the list) or its status row (its page) */
static ui_srvlink_status_t s_srv; /* what the open page was built for */
static uint8_t s_phone_user;     /* PAGE_PHONE_ADD: whose phone */
static bool s_phone_open;        /* PAGE_PHONE_ADD: the QR code is up */
static bool s_phone_asking;      /* PAGE_PHONE_ADD: a phone waits for Allow */
static bool s_srv_page;          /* PAGE_ROBOALARMS is the open page */

static bool available(void)
{
    return S.be.mqtt_get != NULL && S.be.mqtt_set != NULL && S.be.mqtt_status != NULL;
}

static bool ha_available(void)
{
    return S.be.halink_status != NULL && S.be.halink_enable != NULL &&
           S.be.halink_pair_begin != NULL && S.be.halink_pair_decide != NULL &&
           S.be.halink_pair_cancel != NULL && S.be.halink_unpair != NULL;
}

static bool srv_available(void)
{
    return S.be.srvlink_status != NULL && S.be.srvlink_begin != NULL && S.be.srvlink_decide != NULL &&
           S.be.srvlink_cancel != NULL && S.be.srvlink_unlink != NULL &&
           S.be.srvlink_set_ceiling != NULL;
}

static bool phones_available(void)
{
    return S.be.phone_list != NULL && S.be.phone_pair_begin != NULL && S.be.phone_asking != NULL &&
           S.be.phone_decide != NULL && S.be.phone_pair_cancel != NULL && S.be.phone_remove != NULL;
}

static void read_srv(ui_srvlink_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (S.be.srvlink_status != NULL) {
        S.be.srvlink_status(S.be.ctx, st);
    }
}

static void read_status(ui_mqtt_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (S.be.mqtt_status != NULL) {
        S.be.mqtt_status(S.be.ctx, st);
    }
}

static void read_ha(ui_halink_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (S.be.halink_status != NULL) {
        S.be.halink_status(S.be.ctx, st);
    }
}

/* The MQTT connection in words: `list` is the line under its name in the Integrations list, `page`
 * the line under the switch on its own page (it carries the state: "On · connected to ..."). An
 * alert is a state that needs a person; the page tints it. */
static void status_words(const ui_mqtt_status_t *st, char *list, size_t llen, char *page,
                         size_t plen, bool *alert, lv_color_t *tone)
{
    *alert = false;
    *tone = ui_tone_amber();
    switch (st->state) {
    case UI_MQTT_NO_NETWORK:
        /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Waiting for Wi-Fi"));
        /* TR: The line under the MQTT switch, switched on but the panel is offline. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · waiting for Wi-Fi"));
        *alert = true;
        break;
    case UI_MQTT_CONNECTING:
        /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Connecting"));
        /* TR: The line under the MQTT switch while it connects. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · connecting to the broker"));
        break;
    case UI_MQTT_CONNECTED:
        /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Connected"));
        /* TR: The line under the MQTT switch once connected, {0} the broker's address (up to 80
         * bytes). At most 111 bytes */
        I18N_FORMAT(page, plen, TR("On · connected to {0}"), st->broker);
        break;
    case UI_MQTT_FAILED:
        *alert = true;
        *tone = ui_tone_red();
        switch (st->fail) {
        case UI_MQTT_FAIL_UNREACHABLE:
            /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
            i18n_utf8_copy(list, llen, TR("Can't reach the broker"));
            /* TR: The line under the MQTT switch, it tries again by itself. At most 111 bytes */
            i18n_utf8_copy(page, plen, TR("On · can't reach the broker, check the address and port"));
            break;
        case UI_MQTT_FAIL_AUTH:
            /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
            i18n_utf8_copy(list, llen, TR("The broker refused the sign-in"));
            /* TR: The line under the MQTT switch. At most 111 bytes */
            i18n_utf8_copy(page, plen, TR("On · the broker refused the sign-in, check the user name and password"));
            break;
        case UI_MQTT_FAIL_TLS:
            /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
            i18n_utf8_copy(list, llen, TR("Certificate not trusted"));
            /* TR: The line under the MQTT switch. At most 111 bytes */
            i18n_utf8_copy(page, plen, TR("On · certificate not trusted, check the security setting"));
            break;
        default:
            /* TR: The MQTT row's state in the Integrations list. At most 63 bytes */
            i18n_utf8_copy(list, llen, TR("Can't connect"));
            /* TR: The line under the MQTT switch, it tries again by itself. At most 111 bytes */
            i18n_utf8_copy(page, plen, TR("On · can't connect, trying again"));
            break;
        }
        break;
    default:
        /* TR: The MQTT row in the Integrations list when it is off. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Off · any broker: Node-RED, your own scripts"));
        /* TR: The line under the MQTT switch when it is off. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("Off · it connects to the broker you set below"));
        break;
    }
}

/* The integration in words, as for MQTT. */
static void ha_words(const ui_halink_status_t *st, char *list, size_t llen, char *page,
                     size_t plen, bool *alert, lv_color_t *tone)
{
    *alert = false;
    *tone = ui_tone_amber();
    switch (st->state) {
    case UI_HALINK_NO_NETWORK:
        /* TR: The Home Assistant row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Waiting for Wi-Fi"));
        /* TR: The line under the Home Assistant switch, switched on but the panel is offline. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · waiting for Wi-Fi"));
        *alert = true;
        break;
    case UI_HALINK_READY:
        /* TR: The Home Assistant row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Ready to pair"));
        /* TR: The line under the Home Assistant switch. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · ready to pair"));
        break;
    case UI_HALINK_PAIRING:
    case UI_HALINK_PAIRING_CODE:
        /* TR: The Home Assistant row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, st->state == UI_HALINK_PAIRING ? TR("Waiting for Home Assistant")
                                                                  : TR("Check the code"));
        /* TR: The line under the Home Assistant switch while pairing is open. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · pairing"));
        break;
    case UI_HALINK_PAIRED_IDLE:
        /* TR: The Home Assistant row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Paired"));
        /* TR: The line under the Home Assistant switch, paired but Home Assistant is not connected. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · paired, waiting for Home Assistant"));
        break;
    case UI_HALINK_CONNECTED:
        /* TR: The Home Assistant row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Connected"));
        /* TR: The line under the Home Assistant switch. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("On · connected and paired"));
        break;
    default:
        /* TR: The Home Assistant row in the Integrations list when it is off. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR_C("integration status", "Off"));
        /* TR: The line under the Home Assistant switch when it is off. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("Off · switch it on so Home Assistant can find the panel"));
        break;
    }
}

/* The link to alarm-server in words, as for MQTT. */
static void srv_words(const ui_srvlink_status_t *st, char *list, size_t llen, char *page, size_t plen,
                      bool *alert, lv_color_t *tone)
{
    *alert = false;
    *tone = ui_tone_amber();
    switch (st->state) {
    case UI_SRVLINK_NO_NETWORK:
        /* TR: The RoboAlarms row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Waiting for Wi-Fi"));
        /* TR: The line under RoboAlarms' status on its page. At most 111 bytes */
        i18n_utf8_copy(page, plen, TR("Linked · waiting for Wi-Fi"));
        *alert = true;
        break;
    case UI_SRVLINK_CONNECTING:
        i18n_utf8_copy(list, llen, TR("Connecting"));
        /* TR: The line under RoboAlarms' status on its page, {0} the server's address. At most 111 bytes */
        I18N_FORMAT(page, plen, TR("Connecting to {0}"), st->host);
        break;
    case UI_SRVLINK_FAILED:
        *alert = true;
        *tone = ui_tone_red();
        /* TR: The RoboAlarms row's state in the Integrations list. At most 63 bytes */
        i18n_utf8_copy(list, llen, st->old_software ? TR("Needs a software update") : TR("Can't connect"));
        /* TR: The line under RoboAlarms' status on its page, {0} the server's address. At most 111 bytes */
        I18N_FORMAT(page, plen,
                    st->old_software ? TR("{0} needs newer software on this panel")
                                     : TR("Can't reach {0}, trying again"),
                    st->host);
        break;
    case UI_SRVLINK_WAIT_CODE:
    case UI_SRVLINK_SHOW_CODE:
    case UI_SRVLINK_CLAIMED:
        /* TR: The RoboAlarms row's state in the Integrations list while linking. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Linking"));
        i18n_utf8_copy(page, plen, TR("Linking"));
        break;
    case UI_SRVLINK_CONNECTED:
        i18n_utf8_copy(list, llen, TR("Connected"));
        /* TR: The line under RoboAlarms' status on its page, {0} the server's address. At most 111 bytes */
        I18N_FORMAT(page, plen, TR("Connected to {0}"), st->host);
        break;
    default:
        /* TR: The RoboAlarms row in the Integrations list when not linked. At most 63 bytes */
        i18n_utf8_copy(list, llen, TR("Not linked · the RoboAlarms app and website"));
        i18n_utf8_copy(page, plen, TR("Not linked"));
        break;
    }
}

/* On the Integrations list the row keeps its name and only the line beneath changes; on a page of
 * its own the switch's line says it. */
static void fill_row(lv_obj_t *row, const char *list, const char *page, bool alert, lv_color_t tone)
{
    if (s_page == PAGE_INTEGRATIONS) {
        ui_row_set_detail(row, list, false, tone);
    } else {
        ui_row_set_detail(row, page, alert, tone);
    }
}

static void show_ha(void)
{
    if (s_ha_row == NULL) {
        return;
    }
    char list[64];
    char page[112];
    bool alert;
    lv_color_t tone;
    read_ha(&s_ha);
    ha_words(&s_ha, list, sizeof(list), page, sizeof(page), &alert, &tone);
    fill_row(s_ha_row, list, page, alert, tone);
}

static void show_srv(void)
{
    if (s_srv_row == NULL) {
        return;
    }
    char list[64];
    char page[112];
    bool alert;
    lv_color_t tone;
    read_srv(&s_srv);
    srv_words(&s_srv, list, sizeof(list), page, sizeof(page), &alert, &tone);
    fill_row(s_srv_row, list, page, alert, tone);
}

static void show_mqtt(void)
{
    if (s_mqtt_row == NULL) {
        return;
    }
    char list[64];
    char page[112];
    bool alert;
    lv_color_t tone;
    read_status(&s_shown);
    status_words(&s_shown, list, sizeof(list), page, sizeof(page), &alert, &tone);
    fill_row(s_mqtt_row, list, page, alert, tone);
}

/* Add a phone is on the page stack (it is being rebuilt, or a sheet is over it). */
static bool phone_page_open(void)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return false;
    }
    for (size_t i = 0; i < S.depth; i++) {
        if (S.pages[i].id == PAGE_PHONE_ADD) {
            return true;
        }
    }
    return false;
}

void ui_integrations_stop(void)
{
    s_ha_row = NULL;
    s_mqtt_row = NULL;
    s_srv_row = NULL;
    s_srv_page = false;
    if (s_phone_open && !phone_page_open()) {
        s_phone_open = false;
        if (S.be.phone_pair_cancel != NULL) {
            S.be.phone_pair_cancel(S.be.ctx); /* leaving the page closes the window; a rebuild doesn't */
        }
    }
}

void ui_integrations_tick(void)
{
    const bool phone_page = s_page == PAGE_PHONE_ADD && s_phone_open;
    if ((s_ha_row == NULL && s_mqtt_row == NULL && s_srv_row == NULL && !phone_page && !s_srv_page) ||
        lv_tick_elaps(s_polled) < STATUS_POLL_MS) {
        return;
    }
    s_polled = lv_tick_get();
    if (phone_page) {
        char name[32];
        uint8_t user = 0;
        if (S.be.phone_asking(S.be.ctx, name, sizeof(name), &user) != s_phone_asking) {
            ui_page_rebuild(); /* a phone asked: the page turns into Allow */
        }
        return;
    }
    if (s_srv_page) {
        ui_srvlink_status_t st;
        read_srv(&st);
        /* rows appear and disappear with the state; the countdown is its own line, once a minute */
        if (st.state != s_srv.state || st.linked != s_srv.linked || strcmp(st.code, s_srv.code) != 0 ||
            st.code_left_s / 60u != s_srv.code_left_s / 60u) {
            ui_page_rebuild();
        }
        return;
    }
    if (s_srv_row != NULL) {
        ui_srvlink_status_t st;
        read_srv(&st);
        if (st.state != s_srv.state || st.old_software != s_srv.old_software) {
            show_srv();
        }
    }
    if (s_page == PAGE_HOME_ASSISTANT) {
        ui_halink_status_t st;
        read_ha(&st);
        if (st.state != s_ha.state || st.paired != s_ha.paired ||
            strcmp(st.code, s_ha.code) != 0) {
            /* rows appear and disappear with the state: rebuild the page */
            ui_page_rebuild();
        }
        return;
    }
    if (s_ha_row != NULL) { /* the list's Home Assistant row */
        ui_halink_status_t st;
        read_ha(&st);
        if (st.state != s_ha.state || strcmp(st.client, s_ha.client) != 0) {
            show_ha();
        }
    }
    if (s_mqtt_row != NULL) {
        ui_mqtt_status_t st;
        read_status(&st);
        if (st.state != s_shown.state || st.fail != s_shown.fail ||
            st.waiting != s_shown.waiting || st.sent != s_shown.sent ||
            strcmp(st.broker, s_shown.broker) != 0) {
            show_mqtt();
        }
    }
}

/* ---- saving (MQTT) --------------------------------------------------------------------------- */

static void rebuild_later(void *unused)
{
    (void)unused;
    ui_page_rebuild();
}

/* The page is rebuilt after the event that changed something (a switch can't delete itself),
 * and the rebuilt rows show the new values: only a refusal gets words (no "Saved" toasts). */
static void save(void)
{
    char why[96] = "";
    if (!S.be.mqtt_set(S.be.ctx, &s_set, why, sizeof(why))) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(why[0] ? ui_answer("why", why) : TR("Not saved"));
    }
    s_set.password_change = false;
    memset(s_set.password, 0, sizeof(s_set.password));
    lv_async_call(rebuild_later, NULL);
}

static void host_changed(const char *text);

static void on_enabled(bool on, void *ctx)
{
    (void)ctx;
    if (on && s_set.host[0] == '\0') {
        s_enable_after_host = true; /* connect once there is an address */
        ui_text_sheet_kind(TR("Broker address"), TR("The computer running the MQTT broker, like "
                           "mqtt.local or 192.168.1.10"), "", UI_TEXT_PLAIN, 63,
                           host_changed);
        lv_async_call(rebuild_later, NULL); /* the switch shows off until then */
        return;
    }
    s_set.enabled = on;
    save();
}

static void host_changed(const char *text)
{
    snprintf(s_set.host, sizeof(s_set.host), "%s", text);
    if (s_enable_after_host && text[0] != '\0') {
        s_set.enabled = true;
    }
    s_enable_after_host = false;
    save();
}

static void edit_host(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Broker address"), TR("Name or IP address, like mqtt.local or 192.168.1.10"),
                       s_set.host, UI_TEXT_PLAIN, 63, host_changed);
}

static void port_changed(const char *text)
{
    unsigned long port = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || port > 65535u) {
            port = 70000u;
            break;
        }
        port = port * 10u + (unsigned long)(*p - '0');
    }
    if (port > 65535u) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("A port is a number up to 65535"));
        return;
    }
    s_set.port = (uint16_t)port;
    save();
}

static void edit_port(void *ctx)
{
    (void)ctx;
    char text[8] = "";
    if (s_set.port != 0u) {
        snprintf(text, sizeof(text), "%u", (unsigned)s_set.port);
    }
    ui_text_sheet_kind(TR("Port"), TR("Leave empty for the usual one: 1883, or 8883 when encrypted"),
                       text, UI_TEXT_NUMBER, 5, port_changed);
}

static void user_changed(const char *text)
{
    snprintf(s_set.user, sizeof(s_set.user), "%s", text);
    save();
}

static void edit_user(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("User name"), TR("The broker's user for the panel. Empty: none"), s_set.user,
                       UI_TEXT_PLAIN, 63, user_changed);
}

static void password_changed(const char *text)
{
    snprintf(s_set.password, sizeof(s_set.password), "%s", text);
    s_set.password_change = true;
    save();
}

static void edit_password(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Password"), TR("Stored in the panel, never shown again. Empty: none"), "",
                       UI_TEXT_PASSWORD, 63, password_changed);
}

static const ui_choice_t *security_choices(size_t *n)
{
    static ui_choice_t choices[3];
    choices[0] = (ui_choice_t){UI_ICON_LOCK_KEYHOLE, ui_tone_green(), TR("Encrypted"),
                               TR("The broker has a certificate from a public authority")};
    choices[1] = (ui_choice_t){UI_ICON_LOCK_KEYHOLE, ui_tone_green(), TR("Encrypted, own certificate"),
                               TR("Your own certificate authority, loaded on the service console")};
    choices[2] = (ui_choice_t){UI_ICON_SHIELD_OFF, ui_tone_amber(), TR("Not encrypted"),
                               TR("Only on a home network you trust")};
    *n = 3;
    return choices;
}

static const uint8_t SECURITY_TLS[3] = {UI_MQTT_TLS_PUBLIC, UI_MQTT_TLS_OWN_CA, UI_MQTT_TLS_OFF};

static void security_chosen(int32_t index)
{
    if (index < 0 || index > 2) {
        return;
    }
    s_set.tls = SECURITY_TLS[index];
    save();
}

static void pick_security(void *ctx)
{
    (void)ctx;
    size_t n;
    const ui_choice_t *choices = security_choices(&n);
    int32_t selected = 0;
    for (int32_t i = 0; i < 3; i++) {
        if (SECURITY_TLS[i] == s_set.tls) {
            selected = i;
        }
    }
    ui_pick_sheet(TR("Security"), TR("How the panel talks to the broker"), choices, n, selected,
                  security_chosen);
}

static void on_discovery(bool on, void *ctx)
{
    (void)ctx;
    s_set.discovery = on;
    save();
}

static void on_commands(bool on, void *ctx)
{
    (void)ctx;
    s_set.commands = on;
    save();
}

static void prefix_changed(const char *text)
{
    snprintf(s_set.prefix, sizeof(s_set.prefix), "%s",
             text[0] ? text : NOTR("alarmpanel")); /* the default topic prefix: a value */
    save();
}

static void edit_prefix(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Topic prefix"), TR("Start of every topic. Empty: alarmpanel"), s_set.prefix,
                       UI_TEXT_PLAIN, 31, prefix_changed);
}

static void panel_id_changed(const char *text)
{
    snprintf(s_set.panel_id, sizeof(s_set.panel_id), "%s", text);
    save();
}

static void edit_panel_id(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Panel ID"), TR("Lowercase letters and digits. Empty: from the panel's address"),
                       s_set.panel_id, UI_TEXT_PLAIN, 16, panel_id_changed);
}

/* ---- the integration (docs/features/23) ----------------------------------------------------------- */

static void on_ha_enabled(bool on, void *ctx)
{
    (void)ctx;
    if (!S.be.halink_enable(S.be.ctx, on)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not changed"));
    }
    lv_async_call(rebuild_later, NULL); /* the status row says what the link is doing now */
}

static void ha_pair(void *ctx)
{
    (void)ctx;
    if (!S.be.halink_pair_begin(S.be.ctx)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Switch the integration on first"));
        return;
    }
    lv_async_call(rebuild_later, NULL);
}

static void ha_allow(void *ctx)
{
    (void)ctx;
    S.be.halink_pair_decide(S.be.ctx, true);
    lv_async_call(rebuild_later, NULL); /* the page turns into its Paired form */
}

static void ha_reject(void *ctx)
{
    (void)ctx;
    S.be.halink_pair_decide(S.be.ctx, false);
    lv_async_call(rebuild_later, NULL);
}

static void ha_cancel(void *ctx)
{
    (void)ctx;
    S.be.halink_pair_cancel(S.be.ctx);
    lv_async_call(rebuild_later, NULL);
}

static void ha_unpair(void *ctx)
{
    (void)ctx;
    S.be.halink_unpair(S.be.ctx);
    lv_async_call(rebuild_later, NULL); /* the Paired section disappears */
}

/* ---- RoboAlarms: the link to alarm-server (BL-225) ------------------------------------------------ */

static void srv_connect(void *ctx)
{
    (void)ctx;
    if (!S.be.srvlink_begin(S.be.ctx, S.auth_user)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not now: it is already linked or linking"));
    }
    lv_async_call(rebuild_later, NULL);
}

static void srv_cancel(void *ctx)
{
    (void)ctx;
    S.be.srvlink_cancel(S.be.ctx);
    lv_async_call(rebuild_later, NULL);
}

static void srv_allow(void *ctx)
{
    (void)ctx;
    S.be.srvlink_decide(S.be.ctx, true);
    lv_async_call(rebuild_later, NULL);
}

static void srv_reject(void *ctx)
{
    (void)ctx;
    S.be.srvlink_decide(S.be.ctx, false);
    lv_async_call(rebuild_later, NULL);
}

static void srv_unlink(void *ctx)
{
    (void)ctx;
    if (!S.be.srvlink_unlink(S.be.ctx)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Unlinked for now, but not saved: it is tried again"));
    }
    lv_async_call(rebuild_later, NULL);
}

/* The four switches of the remote ceiling: only someone at the panel changes them (D6). */
static void srv_ceiling(bool on, void *ctx)
{
    ui_srvlink_status_t next = s_srv;
    switch ((uintptr_t)ctx) {
    case 0: next.remote_disarm = on; break;
    case 1: next.remote_setup = on; break;
    case 2: next.remote_people = on; break;
    default: next.ask_code = on; break;
    }
    if (!S.be.srvlink_set_ceiling(S.be.ctx, &next)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not saved"));
    }
    lv_async_call(rebuild_later, NULL);
}

/* "7KQ 2MZ": two groups of three are easier to copy. */
static void grouped(const char *code, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; code[i] != '\0' && n + 2u < cap; i++) {
        if (i == 3u) {
            out[n++] = ' ';
        }
        out[n++] = code[i];
        out[n] = '\0';
    }
}

/* A still picture, never an animation (UI-020), with a quiet zone so a phone finds it. */
static void link_qr(lv_obj_t *body, const char *text)
{
#if LV_USE_QRCODE
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *qr = lv_qrcode_create(box);
    lv_qrcode_set_size(qr, 180);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, text, (uint32_t)strlen(text));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);
#else
    (void)body;
    (void)text;
#endif
}

static void build_roboalarms(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the RoboAlarms page's note. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("See and control this panel from the RoboAlarms app and website"));
    if (!srv_available()) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    read_srv(&s_srv);
    s_srv_page = true;
    char value[112];
    switch (s_srv.state) {
    case UI_SRVLINK_IDLE:
        ui_row_link(body, UI_ICON_GLOBE, ui_tone_gray(), TR("Server"), NULL, s_srv.host, NULL, NULL, NULL);
        ui_row_action(body, UI_ICON_LOG_IN, TR("Connect to RoboAlarms"), ui_tone_blue(), srv_connect, NULL);
        ui_paragraph(body,
                     TR("You get a code to type on the website or in the app, then allow it here. "
                        "The alarm works the same whether it is linked or not."),
                     ui_tone_gray());
        return;
    case UI_SRVLINK_WAIT_CODE:
        ui_paragraph(body, TR("Asking the server for a code..."), ui_tone_gray());
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Cancel"), ui_tone_gray(), srv_cancel, NULL);
        return;
    case UI_SRVLINK_SHOW_CODE: {
        char code[16];
        grouped(s_srv.code, code, sizeof(code));
        ui_big_value(body, TR("On your phone or computer, enter"), code);
        link_qr(body, s_srv.url);
        /* TR: under the link code: where to type it, {0} the address, {1} the minutes left. At most 127 bytes */
        char minutes[12];
        snprintf(minutes, sizeof(minutes), "%u", (unsigned)((s_srv.code_left_s + 59u) / 60u));
        I18N_FORMAT(value, sizeof(value), TR("{0} · {1} min left, or scan the code"), s_srv.url, minutes);
        ui_paragraph(body, value, ui_tone_gray());
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Cancel"), ui_tone_gray(), srv_cancel, NULL);
        return;
    }
    case UI_SRVLINK_CLAIMED: {
        for (char *b = strstr(s_srv.email, "\xE2\x80\xA2"); b != NULL; b = strstr(b, "\xE2\x80\xA2")) {
            b[0] = '*'; /* a bullet (U+2022) isn't in the screen's fonts */
            memmove(b + 1, b + 3, strlen(b + 3) + 1u);
        }
        /* TR: who is asking to link the panel: {0} the account's name, {1} its masked e-mail. At most 111 bytes */
        I18N_FORMAT(value, sizeof(value), TR("{0} ({1})"), s_srv.account, s_srv.email);
        char detail[112];
        /* TR: under who is asking: the place they link it as, {0} its name. At most 111 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("Wants to link this panel as {0}"), s_srv.location);
        ui_row_link(body, UI_ICON_USER, ui_tone_blue(), value, detail, NULL, NULL, NULL, NULL);
        ui_action_pair(body, UI_ICON_CIRCLE_X, TR("Don't allow"), ui_tone_red(), srv_reject,
                       UI_ICON_CIRCLE_CHECK, TR("Allow"), ui_tone_green(), srv_allow, NULL);
        ui_paragraph(body, TR("If you don't know this account, don't allow it."), ui_tone_amber());
        return;
    }
    default:
        break; /* linked: the status and what the server may do */
    }
    s_srv_row = ui_row_link(body, UI_ICON_SHIELD_CHECK, ui_tone_green(), TR("Linked"), " ", NULL, NULL,
                            NULL, NULL);
    show_srv();
    ui_section(body, TR("What the app and website may do"));
    ui_row_toggle(body, TR("Disarm and silence"), TR("Arming always works"), s_srv.remote_disarm,
                  srv_ceiling, (void *)(uintptr_t)0);
    ui_row_toggle(body, TR("Open Setup mode"), NULL, s_srv.remote_setup, srv_ceiling,
                  (void *)(uintptr_t)1);
    ui_row_toggle(body, TR("Change people and codes"), NULL, s_srv.remote_people, srv_ceiling,
                  (void *)(uintptr_t)2);
    ui_row_toggle(body, TR("Ask for the alarm code to disarm"),
                  TR("A duress code then works from the app"), s_srv.ask_code, srv_ceiling,
                  (void *)(uintptr_t)3);
    ui_paragraph(body, TR("Only someone at the panel changes these. Off is off for everyone."),
                 ui_tone_gray());
    ui_section(body, "");
    ui_row_hold(body, TR("Unlink"), srv_unlink, NULL);
    ui_paragraph(body, TR("Works without the server too, and changes nothing else on the panel."),
                 ui_tone_gray());
}

/* ---- a phone for a person (direct mode) ----------------------------------------------------------- */

static void open_phone_add(void *ctx)
{
    ui_page_open(PAGE_PHONE_ADD, (int32_t)(uintptr_t)ctx);
}

static void phone_remove(void *ctx)
{
    const ui_phone_t *p = ctx;
    if (!S.be.phone_remove(S.be.ctx, p->id)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not removed"));
    }
    lv_async_call(rebuild_later, NULL);
}

void ui_phone_rows(lv_obj_t *body, uint8_t user)
{
    if (!phones_available()) {
        return;
    }
    static ui_phone_t list[UI_PHONES_MAX];
    const size_t n = S.be.phone_list(S.be.ctx, list, UI_PHONES_MAX);
    /* TR: a user's page: the phones paired with the panel directly. At most 47 bytes */
    ui_section(body, TR("Phones"));
    for (size_t i = 0; i < n; i++) {
        if (list[i].user != user) {
            continue;
        }
        /* TR: a paired phone's row on its person's page, held to remove it. {0} the phone's name. At most 63 bytes */
        char text[80];
        I18N_FORMAT(text, sizeof(text), TR("Remove {0}"), list[i].name);
        ui_row_hold(body, text, phone_remove, &list[i]);
    }
    ui_row_link(body, UI_ICON_PLUS, ui_tone_blue(), TR("Add a phone"),
                TR("The RoboAlarms app, on this Wi-Fi"), NULL, UI_ICON_CHEVRON_RIGHT, open_phone_add,
                (void *)(uintptr_t)user);
}

static void back_later(void *unused)
{
    (void)unused;
    ui_page_back();
}

/* The button that was pressed can't delete its own page: back after the event. */
static void phone_allow(void *ctx)
{
    (void)ctx;
    S.be.phone_decide(S.be.ctx, true);
    s_phone_open = false;
    ui_toast(TR("Phone added"));
    lv_async_call(back_later, NULL);
}

static void phone_reject(void *ctx)
{
    (void)ctx;
    S.be.phone_decide(S.be.ctx, false);
    s_phone_open = false;
    lv_async_call(back_later, NULL);
}

static void build_phone_add(lv_obj_t *body, int32_t arg, char *note, size_t nlen)
{
    s_phone_user = (uint8_t)arg;
    i18n_utf8_copy(note, nlen, TR("The RoboAlarms app, straight to this panel"));
    if (!phones_available() || !ui_settings_allowed(ACCESS_USERS)) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    char name[32];
    uint8_t user = 0;
    s_phone_asking = S.be.phone_asking(S.be.ctx, name, sizeof(name), &user);
    if (s_phone_asking) {
        s_phone_open = true;
        char who[ALARM_NAME_LEN + 8];
        char detail[112];
        /* TR: a phone asks to be added: {0} the person's name. At most 111 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("Add this phone for {0}?"), ui_user_name(S.cfg, user, who, sizeof(who)));
        ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_blue(), name, detail, NULL, NULL, NULL, NULL);
        ui_action_pair(body, UI_ICON_CIRCLE_X, TR("Don't allow"), ui_tone_red(), phone_reject,
                       UI_ICON_CIRCLE_CHECK, TR("Allow"), ui_tone_green(), phone_allow, NULL);
        return;
    }
    static char uri[256];
    if (!s_phone_open) {
        char why[96] = "";
        if (!S.be.phone_pair_begin(S.be.ctx, s_phone_user, uri, sizeof(uri), why, sizeof(why))) {
            ui_paragraph(body, why[0] ? ui_answer("why", why) : TR("Not available now"), ui_tone_amber());
            return;
        }
        s_phone_open = true;
    }
    ui_paragraph(body, TR("Open the RoboAlarms app and scan this code, on this panel's Wi-Fi."),
                 ui_tone_gray());
    link_qr(body, uri);
    ui_paragraph(body, TR("It works for 5 minutes, once. Then you allow the phone here."), ui_tone_gray());
}

void ui_phone_add_build(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    s_ha_row = NULL;
    s_mqtt_row = NULL;
    s_srv_row = NULL;
    s_page = PAGE_PHONE_ADD;
    s_polled = lv_tick_get();
    /* TR: the title of a person's Add a phone page. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Add a phone"));
    build_phone_add(body, arg, note, nlen);
}

/* ---- pages ---------------------------------------------------------------------------------- */

static void open_home_assistant(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_HOME_ASSISTANT, 0);
}

static void open_roboalarms(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ROBOALARMS, 0);
}

static void open_mqtt(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_MQTT, 0);
}

/* The list: one row per integration, each saying what it is doing right now, in two groups -
 * the systems that watch and control the panel, and the services that alert a person. The rows
 * say it themselves: no paragraph between the groups. */
static void build_list(lv_obj_t *body, char *note, size_t nlen)
{
    i18n_utf8_copy(note, nlen, TR("Smart-home systems and alerts to your phone"));
    const bool smart_home = available() || ha_available() || srv_available();
    if (!smart_home && !ui_notify_available()) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    if (smart_home) {
        ui_section(body, TR("Smart home"));
        if (srv_available()) {
            s_srv_row = ui_row_link(body, UI_ICON_GLOBE, ui_tone_blue(), NOTR("RoboAlarms" /* product name */),
                                    " ", NULL, UI_ICON_CHEVRON_RIGHT, open_roboalarms, NULL);
            show_srv();
        }
        if (ha_available()) {
            s_ha_row = ui_row_link(body, UI_ICON_HOUSE_WIFI, ui_tone_violet(), NOTR("Home Assistant" /* product name */),
                                   " ", NULL, UI_ICON_CHEVRON_RIGHT, open_home_assistant, NULL);
            show_ha();
        }
        if (available()) {
            S.be.mqtt_get(S.be.ctx, &s_set);
            s_mqtt_row = ui_row_link(body, UI_ICON_SERVER, ui_tone_blue(), NOTR("MQTT" /* protocol name */), " ", NULL,
                                     UI_ICON_CHEVRON_RIGHT, open_mqtt, NULL);
            show_mqtt();
        }
    }
    if (ui_notify_available()) {
        ui_section(body, TR("Alerts to your phone"));
        ui_notify_list_rows(body);
    }
}

/* The Home Assistant page: the integration, found on the network and paired here. One switch
 * carries the state; what follows is what the state asks of a person. */
static void build_home_assistant(lv_obj_t *body, char *note, size_t nlen)
{
    i18n_utf8_copy(note, nlen, TR("Found on your network and paired on this screen"));
    if (!ha_available()) {
        ui_paragraph(body,
                     TR("This panel build has no Home Assistant integration. Connect it over MQTT "
                     "instead: Integrations > MQTT."),
                     ui_tone_gray());
        return;
    }
    read_ha(&s_ha);
    s_ha_row = ui_row_toggle(body, NOTR("Home Assistant" /* product name */), " ", s_ha.enabled,
                             on_ha_enabled, NULL);
    show_ha();

    if (!s_ha.enabled) {
        ui_paragraph(body,
                     TR("Home Assistant finds the panel on your network and you allow it here. "
                     "Nothing to type, and no Home Assistant password."),
                     ui_tone_gray());
        return;
    }
    switch (s_ha.state) {
    case UI_HALINK_PAIRING_CODE: {
        char code[16] = ""; /* "588 460": two groups of three are easier to compare */
        for (size_t i = 0, n = 0; s_ha.code[i] != '\0' && n + 2u < sizeof(code); i++) {
            if (i == 3u) {
                code[n++] = ' ';
            }
            code[n++] = s_ha.code[i];
            code[n] = '\0';
        }
        ui_big_value(body, TR("Does Home Assistant show this code?"), code);
        ui_action_pair(body, UI_ICON_CIRCLE_X, TR("No, reject"), ui_tone_red(), ha_reject,
                       UI_ICON_CIRCLE_CHECK, TR("Yes, allow"), ui_tone_green(), ha_allow, NULL);
        ui_paragraph(body,
                     TR("If the codes differ, something on your network is in between: reject it."),
                     ui_tone_amber());
        break;
    }
    case UI_HALINK_PAIRING:
        ui_paragraph(body,
                     TR("Waiting for Home Assistant. There: Settings > Devices & services, the "
                     "discovered RoboAlarms Panel, Add. Pairing stays open for two minutes."),
                     ui_tone_gray());
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop pairing"), ui_tone_gray(), ha_cancel, NULL);
        break;
    case UI_HALINK_PAIRED_IDLE:
    case UI_HALINK_CONNECTED:
        ui_paragraph(body, TR("Find the panel in Home Assistant under Settings > Devices & services."),
                     ui_tone_gray());
        ui_section(body, "");
        ui_row_hold(body, TR("Unpair"), ha_unpair, NULL);
        ui_paragraph(body,
                     TR("Removes the panel from Home Assistant until it pairs again."),
                     ui_tone_gray());
        break;
    default: /* ready or waiting for Wi-Fi */
        ui_row_action(body, UI_ICON_LOG_IN, TR("Pair with Home Assistant"), ui_tone_blue(),
                      ha_pair, NULL);
        ui_paragraph(body,
                     TR("Both screens will show the same six-digit code; you allow it here. "
                     "Nothing to type, and the panel never sees a Home Assistant password."),
                     ui_tone_gray());
        break;
    }
}

/* MQTT follows the order of the ntfy page: the switch with its state, then what a person does
 * (the broker to connect to), then the rest. */
static void build_mqtt(lv_obj_t *body, char *note, size_t nlen)
{
    i18n_utf8_copy(note, nlen, TR("Through any broker, for Node-RED or your own scripts"));
    if (!available()) {
        ui_paragraph(body, TR("Not available on this panel."), ui_tone_gray());
        return;
    }
    S.be.mqtt_get(S.be.ctx, &s_set);
    if (ha_available()) {
        read_ha(&s_ha); /* paired? then discovery stays quiet here (docs/features/23 HAI-010) */
    }
    s_mqtt_row = ui_row_toggle(body, TR("Connect to the broker"), " ", s_set.enabled, on_enabled, NULL);
    show_mqtt();

    char value[80];
    ui_section(body, TR("Broker"));
    ui_row_link(body, UI_ICON_SERVER, ui_tone_blue(), TR("Address"), NULL,
                s_set.host[0] ? s_set.host : TR_C("setting value", "Not set"), UI_ICON_CHEVRON_RIGHT, edit_host, NULL);
    if (s_set.port != 0u) {
        snprintf(value, sizeof(value), "%u", (unsigned)s_set.port);
    } else {
        /* TR: the port row's value, {0} the usual port number. At most 74 bytes */
        I18N_FORMAT(value, sizeof(value), TR("{0} (usual)"), s_set.tls == UI_MQTT_TLS_OFF ? "1883" : "8883");
    }
    ui_row_link(body, UI_ICON_HASH, ui_tone_blue(), TR("Port"), NULL, value, UI_ICON_CHEVRON_RIGHT,
                edit_port, NULL);
    ui_row_link(body, UI_ICON_USER, ui_tone_blue(), TR("User name"), NULL,
                s_set.user[0] ? s_set.user : TR_C("setting value", "None"), UI_ICON_CHEVRON_RIGHT, edit_user, NULL);
    ui_row_link(body, UI_ICON_KEY, ui_tone_blue(), TR("Password"), NULL,
                s_set.password_set ? TR_C("setting value", "Set") : TR_C("setting value", "None"), UI_ICON_CHEVRON_RIGHT, edit_password, NULL);
    const bool encrypted = s_set.tls != UI_MQTT_TLS_OFF;
    const char *detail = s_set.tls == UI_MQTT_TLS_PUBLIC ? TR("Public certificate")
                         : s_set.tls == UI_MQTT_TLS_OWN_CA
                             ? (s_set.ca_loaded ? TR("Own certificate") : TR("Own certificate, not loaded"))
                             : TR("Home network only");
    ui_row_link(body, encrypted ? UI_ICON_LOCK_KEYHOLE : UI_ICON_SHIELD_OFF,
                encrypted ? ui_tone_green() : ui_tone_amber(), TR("Security"), detail,
                encrypted ? TR("Encrypted") : TR("Not encrypted"), UI_ICON_CHEVRON_RIGHT, pick_security,
                NULL);
    if (!encrypted) {
        ui_paragraph(body, TR("Not encrypted: anyone on your network can read what the panel sends, "
                           "and the codes sent to it. Use it only on a home network you trust."),
                     ui_tone_amber());
    } else if (s_set.tls == UI_MQTT_TLS_OWN_CA && !s_set.ca_loaded) {
        ui_paragraph(body, TR("Load your certificate authority on the service console (mqtt ca), "
                           "or choose another setting."),
                     ui_tone_amber());
    }

    ui_section(body, TR("What goes over the broker"));
    ui_row_toggle(body, TR("Announce the panel"),
                  TR("Discovery messages: the panel, every zone, troubles and events"),
                  s_set.discovery, on_discovery, NULL);
    ui_row_toggle(body, TR("Arm and disarm over MQTT"),
                  TR("Off: anything listening can only watch. The panel always checks the code"),
                  s_set.commands, on_commands, NULL);
    ui_row_value(body, TR("Code to arm"),
                 S.cfg->opt.remote_arm_without_code ? TR("Not needed (installer setting)")
                                                    : TR("Needed"));
    if (s_set.discovery && ha_available() && s_ha.paired) {
        ui_paragraph(body, TR("Paired with the Home Assistant integration, so the panel is added "
                           "there directly and no discovery messages go to the broker."),
                     ui_tone_gray());
    }

    ui_section(body, TR("Advanced"));
    ui_row_link(body, UI_ICON_TAG, ui_tone_gray(), TR("Topic prefix"), NULL, s_set.prefix,
                UI_ICON_CHEVRON_RIGHT, edit_prefix, NULL);
    ui_row_link(body, UI_ICON_TAG, ui_tone_gray(), TR("Panel ID"), TR("Part of every topic"),
                s_set.panel_id, UI_ICON_CHEVRON_RIGHT, edit_panel_id, NULL);
}

void ui_integrations_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen, char *note,
                           size_t nlen)
{
    s_ha_row = NULL;
    s_mqtt_row = NULL;
    s_srv_row = NULL;
    s_page = id;
    memset(&s_shown, 0, sizeof(s_shown));
    memset(&s_ha, 0, sizeof(s_ha));
    s_polled = lv_tick_get();
    i18n_utf8_copy(title, tlen,
             id == PAGE_ROBOALARMS     ? NOTR("RoboAlarms" /* product name */)
             : id == PAGE_HOME_ASSISTANT ? NOTR("Home Assistant" /* product name */)
             : id == PAGE_MQTT         ? NOTR("MQTT" /* protocol name */)
                                       : TR("Integrations"));
    if (!ui_settings_allowed(ACCESS_SYSTEM)) {
        ui_settings_locked(body, ACCESS_SYSTEM, TR("Integrations"),
                           TR("The master or Admin code connects other systems"), note, nlen);
        return;
    }
    if (id == PAGE_ROBOALARMS) {
        build_roboalarms(body, note, nlen);
    } else if (id == PAGE_HOME_ASSISTANT) {
        build_home_assistant(body, note, nlen);
    } else if (id == PAGE_MQTT) {
        build_mqtt(body, note, nlen);
    } else {
        build_list(body, note, nlen);
    }
}
