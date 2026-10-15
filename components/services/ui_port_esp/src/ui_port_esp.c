#include "sdkconfig.h"
#include "i18n/i18n.h"
/*
 * ui_port_esp.c - the touch UI's backend on ESP-IDF: alarm_service for status, commands, codes
 * and installer mode (the local touchscreen is keypad 0), panel_log for the event log, net_wifi
 * for Wi-Fi, addresses and time, net_check for the connection check, net_mqtt and ha_link for
 * the MQTT and Home Assistant pages, panel_prefs and sounder for this keypad's preferences, panel_system
 * for versions and the factory reset, zone_service for the zones' sensors. Calls block the
 * LVGL task until the alarm task has answered; the alarm task never waits for LVGL, so this
 * can't deadlock.
 */
#include "ui_port_esp.h"
#include "panel_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "alarm/alarm_keyseq.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_service.h"
#include "esp_attr.h"
#include "ha_link.h"
#include "asset_packs.h"
#include "lang_packs.h"
#include "net_check.h"
#include "net_mqtt.h"
#include "net_notify.h"
#include "net_wifi.h"
#include "panel_clock.h"
#include "panel_log.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "panel_system.h"
#include "panel_voice.h"
#include "sounder.h"
#include "zigbee_service.h"
#include "zone_service.h"
#include "zwave_service.h"
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
#include "auto_names.h"
#include "automation_service.h"
#endif
#if CONFIG_ALARM_HOME_CONTROL
#include "device_service.h"
#include "nvs.h"
#include "ui_backend/ui_dev_model.h"
#endif
#if CONFIG_ALARM_FEATURE_VOICE
#include "ui_backend/ui_voice.h"
#endif

#define EPOCH_2025_S 1735689600LL

static const alarm_source_t SRC = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};

static uint32_t snapshot_seq(void *ctx)
{
    (void)ctx;
    return alarm_service_snapshot_seq();
}

static void get_snapshot(void *ctx, alarm_snapshot_t *out)
{
    (void)ctx;
    alarm_service_get_snapshot(out);
}

static bool get_config(void *ctx, alarm_config_t *out)
{
    (void)ctx;
    return alarm_service_get_config(out) == ESP_OK;
}

static size_t recent_events(void *ctx, alarm_event_t *out, size_t max)
{
    (void)ctx;
    return alarm_service_recent_events(out, max);
}

static alarm_ms_t now_ms(void *ctx)
{
    (void)ctx;
    return alarm_service_now();
}

static int64_t epoch_ms(void *ctx)
{
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec < EPOCH_2025_S) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static alarm_result_t command(void *ctx, const alarm_cmd_t *cmd, alarm_cmd_result_t *res)
{
    (void)ctx;
    return alarm_service_command(cmd, &SRC, res);
}

static alarm_result_t change_code(void *ctx, const char *current, const char *new_code,
                                  uint8_t *user_out)
{
    (void)ctx;
    uint8_t user = 0;
    alarm_result_t r = alarm_service_authenticate(&SRC, current, 0, &user);
    if (r == ALARM_OK) {
        /* Same path as code + CODE + user on a keypad: keeps the user's authority and name. */
        alarm_keyseq_result_t ks;
        memset(&ks, 0, sizeof(ks));
        ks.kind = ALARM_KS_USER_SET;
        ks.cmd.partition = 1;
        strlcpy(ks.cmd.code, current, sizeof(ks.cmd.code));
        ks.user_no = user;
        strlcpy(ks.new_code, new_code, sizeof(ks.new_code));
        r = alarm_service_keypad_execute(&ks, &SRC, NULL);
        memset(&ks, 0, sizeof(ks));
    }
    if (user_out != NULL) {
        *user_out = user;
    }
    return r;
}

static ui_net_state_t map_state(net_wifi_state_t st)
{
    switch (st) {
    case NET_WIFI_NO_COPROCESSOR: return UI_NET_NO_RADIO;
    case NET_WIFI_UNCONFIGURED: return UI_NET_UNCONFIGURED;
    case NET_WIFI_CONNECTING: return UI_NET_CONNECTING;
    case NET_WIFI_CONNECTED: return UI_NET_CONNECTED;
    case NET_WIFI_DISCONNECTED: return UI_NET_DISCONNECTED;
    case NET_WIFI_STARTING:
    default: return UI_NET_STARTING;
    }
}

static ui_net_state_t net_state(void *ctx, int *rssi_out)
{
    (void)ctx;
    net_wifi_status_t st;
    net_wifi_get_status(&st);
    *rssi_out = st.rssi;
    return map_state(st.state);
}

static void beep(void *ctx, ui_beep_t b)
{
    (void)ctx;
    switch (b) {
    case UI_BEEP_KEY: sounder_key_click(); break;
    case UI_BEEP_TEST: sounder_play(ALARM_SOUND_ACK); break;
    default: sounder_play(ALARM_SOUND_ERROR); break;
    }
}

/* ---- codes, installer mode and users --------------------------------------------------------- */

static alarm_result_t authenticate(void *ctx, const char *code, uint8_t *user_out)
{
    (void)ctx;
    return alarm_service_authenticate(&SRC, code, 0, user_out);
}

static alarm_result_t program_enter(void *ctx, const char *code, uint8_t *user_out)
{
    (void)ctx;
    return alarm_service_program_enter(&SRC, code, user_out);
}

static void program_exit(void *ctx)
{
    (void)ctx;
    alarm_service_program_exit(&SRC);
}

static void program_keepalive(void *ctx)
{
    (void)ctx;
    alarm_service_program_keepalive();
}

static alarm_result_t program_set_code(void *ctx, uint8_t user, const char *new_code)
{
    (void)ctx;
    return alarm_service_program_set_code(&SRC, user, new_code);
}

static alarm_result_t set_config(void *ctx, const alarm_config_t *cfg)
{
    (void)ctx;
    return alarm_service_set_config(cfg);
}

static alarm_result_t user_set(void *ctx, const char *actor, uint8_t user, const char *code,
                               alarm_authority_t authority, uint8_t partitions, const char *name)
{
    (void)ctx;
    return alarm_service_user_set(&SRC, actor, user, code, authority, partitions, name);
}

static alarm_result_t user_update(void *ctx, const char *actor, uint8_t user,
                                  alarm_authority_t authority, uint8_t partitions,
                                  const char *name)
{
    (void)ctx;
    return alarm_service_user_update(&SRC, actor, user, authority, partitions, name);
}

static alarm_result_t user_delete(void *ctx, const char *actor, uint8_t user)
{
    (void)ctx;
    return alarm_service_user_delete(&SRC, actor, user);
}

/* ---- event log --------------------------------------------------------------------------------- */

static size_t log_read(void *ctx, uint32_t before, elog_filter_fn filter, void *filter_ctx,
                       elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before)
{
    (void)ctx;
    return panel_log_read(before, filter, filter_ctx, out, max, scan, next_before);
}

static uint32_t boot_id(void *ctx)
{
    (void)ctx;
    return panel_log_boot();
}

static void log_loss(void *ctx, uint32_t *records, uint32_t *reports)
{
    (void)ctx;
    panel_log_loss(records, reports);
}

static void log_clear_loss(void *ctx)
{
    (void)ctx;
    panel_log_clear_loss();
}

/* ---- network and time -------------------------------------------------------------------------- */

static void wifi_info(void *ctx, ui_wifi_info_t *out)
{
    (void)ctx;
    net_wifi_status_t st;
    net_wifi_get_status(&st);
    memset(out, 0, sizeof(*out));
    out->state = map_state(st.state);
    strlcpy(out->ssid, st.ssid, sizeof(out->ssid));
    strlcpy(out->ip, st.ip, sizeof(out->ip));
    strlcpy(out->mac, st.mac, sizeof(out->mac));
    out->rssi = st.rssi;
    out->fail = st.fail == NET_WIFI_FAIL_PASSWORD    ? UI_WIFI_FAIL_PASSWORD
                : st.fail == NET_WIFI_FAIL_NOT_FOUND ? UI_WIFI_FAIL_NOT_FOUND
                : st.fail == NET_WIFI_FAIL_OTHER     ? UI_WIFI_FAIL_OTHER
                                                     : UI_WIFI_FAIL_NONE;
    out->time_synced = st.time_synced;
    strlcpy(out->radio_fw, st.coproc_fw, sizeof(out->radio_fw));
    strlcpy(out->mask, st.mask, sizeof(out->mask));
    strlcpy(out->gateway, st.gateway, sizeof(out->gateway));
    strlcpy(out->dns1, st.dns1, sizeof(out->dns1));
    strlcpy(out->dns2, st.dns2, sizeof(out->dns2));
    out->static_ip = st.static_ip;
}

static void ip_get(void *ctx, ui_ip_config_t *out)
{
    (void)ctx;
    net_ip_config_t c;
    net_wifi_get_ip_config(&c);
    memset(out, 0, sizeof(*out));
    out->use_static = c.use_static;
    strlcpy(out->ip, c.ip, sizeof(out->ip));
    strlcpy(out->mask, c.mask, sizeof(out->mask));
    strlcpy(out->gateway, c.gateway, sizeof(out->gateway));
    strlcpy(out->dns1, c.dns1, sizeof(out->dns1));
    strlcpy(out->dns2, c.dns2, sizeof(out->dns2));
}

static bool ip_set(void *ctx, const ui_ip_config_t *in, char *why, size_t why_len)
{
    (void)ctx;
    net_ip_config_t c;
    memset(&c, 0, sizeof(c));
    c.use_static = in->use_static;
    strlcpy(c.ip, in->ip, sizeof(c.ip));
    strlcpy(c.mask, in->mask, sizeof(c.mask));
    strlcpy(c.gateway, in->gateway, sizeof(c.gateway));
    strlcpy(c.dns1, in->dns1, sizeof(c.dns1));
    strlcpy(c.dns2, in->dns2, sizeof(c.dns2));
    const esp_err_t err = net_wifi_set_ip_config(&c);
    if (err == ESP_ERR_INVALID_ARG && why != NULL) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Those addresses don't go together"));
    } else if (err != ESP_OK && why != NULL) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Couldn't store the addresses"));
    }
    return err == ESP_OK;
}

static bool net_check_begin(void *ctx)
{
    (void)ctx;
    return net_check_start() == ESP_OK;
}

/* The two step lists are the same walk, in the same order and with the same states
 * (docs/features/09 NET-012): the UI's copy is portable, the platform's is not. */
_Static_assert((int)UI_CHECK_COUNT == (int)NET_CHECK_COUNT, "check steps out of step");
_Static_assert((int)UI_CHECK_SKIPPED == (int)NET_CHECK_SKIPPED, "check states out of step");

static void net_check_read(void *ctx, ui_netcheck_t *out)
{
    (void)ctx;
    net_check_report_t rep;
    net_check_get(&rep);
    memset(out, 0, sizeof(*out));
    out->running = rep.running;
    out->done = rep.done;
    for (size_t i = 0; i < UI_CHECK_COUNT; i++) {
        out->steps[i].state = rep.steps[i].state;
        out->steps[i].ms = rep.steps[i].ms;
        out->steps[i].words = rep.steps[i].words; /* a string literal: it outlives the copy */
        for (size_t v = 0; v < 2u; v++) {
            strlcpy(out->steps[i].value[v], rep.steps[i].value[v], sizeof(out->steps[i].value[v]));
        }
    }
}

static bool wifi_scan(void *ctx)
{
    (void)ctx;
    return net_wifi_scan_start() == ESP_OK;
}

static size_t wifi_results(void *ctx, ui_wifi_ap_t *out, size_t max, bool *done)
{
    (void)ctx;
    net_wifi_ap_t aps[NET_WIFI_SCAN_MAX];
    const size_t n = net_wifi_scan_results(aps, max < NET_WIFI_SCAN_MAX ? max : NET_WIFI_SCAN_MAX,
                                           done);
    for (size_t i = 0; i < n; i++) {
        strlcpy(out[i].ssid, aps[i].ssid, sizeof(out[i].ssid));
        out[i].rssi = aps[i].rssi;
        out[i].secure = aps[i].secure;
    }
    return n;
}

static void wifi_connect(void *ctx, const char *ssid, const char *password)
{
    (void)ctx;
    net_wifi_set_credentials(ssid, password);
}

static void get_timezone(void *ctx, char *out, size_t len)
{
    (void)ctx;
    net_wifi_get_timezone(out, len);
}

static void set_timezone(void *ctx, const char *tz)
{
    (void)ctx;
    net_wifi_set_timezone(tz);
}

static void get_ntp(void *ctx, char *out, size_t len)
{
    (void)ctx;
    net_wifi_get_ntp_server(out, len);
}

static void set_ntp(void *ctx, const char *server)
{
    (void)ctx;
    net_wifi_set_ntp_server(server);
}

#if CONFIG_ALARM_FEATURE_RTC
static void clock_info(void *ctx, ui_clock_info_t *out)
{
    (void)ctx;
    panel_clock_status_t st;
    panel_clock_get(&st);
    memset(out, 0, sizeof(*out));
    switch (st.module) {
    case CLOCK_MODULE_OK: out->module = UI_CLOCK_MODULE_OK; break;
    case CLOCK_MODULE_LOST: out->module = UI_CLOCK_MODULE_LOST; break;
    case CLOCK_MODULE_FAILED: out->module = UI_CLOCK_MODULE_FAILED; break;
    case CLOCK_MODULE_OTHER: out->module = UI_CLOCK_MODULE_OTHER; break;
    default: out->module = UI_CLOCK_MODULE_NONE; break;
    }
    switch (st.source) {
    case CLOCK_SRC_MODULE: out->source = UI_CLOCK_FROM_MODULE; break;
    case CLOCK_SRC_INTERNET: out->source = UI_CLOCK_FROM_INTERNET; break;
    case CLOCK_SRC_MANUAL: out->source = UI_CLOCK_BY_HAND; break;
    default: out->source = UI_CLOCK_NOT_SET; break;
    }
    out->lost_at_start = st.lost_at_start;
    out->temp_known = st.temp_known;
    out->temp_c4 = st.temp_c4;
}

static bool clock_set(void *ctx, int64_t epoch_ms, uint8_t user, char *why, size_t why_len)
{
    (void)ctx;
    char who[24];
    snprintf(who, sizeof(who), "user %u", (unsigned)user);
    const esp_err_t err = panel_clock_set(epoch_ms, who);
    if (err == ESP_OK) {
        /* docs/features/35 RTC-014: the time set by hand is logged with the user (the touchscreen
         * and the Panel Manager reach here alike, so the source is the system's). */
        (void)alarm_service_platform_event(ALARM_EV_CLOCK_SET, 0u, 0u, 0u, NULL, user);
        return true;
    }
    if (why != NULL && why_len > 0u) {
        snprintf(why, why_len, "%s",
                 err == ESP_ERR_NOT_SUPPORTED
                     ? I18N_NOOP_C("why", "This panel can't keep a time set by hand")
                     : I18N_NOOP_C("why", "Pick a date from 2025 to 2099"));
    }
    return false;
}
#endif

/* ---- languages (docs/features/37 language-packs.md) --------------------------------------------- */

#if CONFIG_ALARM_FEATURE_LANG_PACKS
/* The languages are lang_packs': an installed pack is loaded on the service's task, never on the
 * LVGL one (lang_packs_catalog() queues it and lang_packs_generation() changes once it is in). */
static const i18n_catalog_t *lang_catalog(void *ctx, const char *locale)
{
    (void)ctx;
    return lang_packs_catalog(locale);
}

static size_t lang_list(void *ctx, ui_lang_t *out, size_t max)
{
    (void)ctx;
    static EXT_RAM_BSS_ATTR lang_pack_info_t info[LANG_PACKS_LIST_MAX]; /* the LVGL task only */
    const size_t n = lang_packs_list(info, max < LANG_PACKS_LIST_MAX ? max : LANG_PACKS_LIST_MAX);
    for (size_t i = 0; i < n; i++) {
        /* the same field sizes on both sides: whole copies, each ends in a NUL */
        _Static_assert(sizeof(out[i].locale) == sizeof(info[i].locale), "locale");
        _Static_assert(sizeof(out[i].name) == sizeof(info[i].name), "name");
        _Static_assert(sizeof(out[i].version) == sizeof(info[i].version), "version");
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].locale, info[i].locale, sizeof(out[i].locale) - 1u);
        memcpy(out[i].name, info[i].name, sizeof(out[i].name) - 1u);
        memcpy(out[i].version, info[i].version, sizeof(out[i].version) - 1u);
        out[i].draft = info[i].draft;
        /* lang_pack_state_t and ui_lang_state_t list the same states in the same order */
        out[i].state = info[i].state == LANG_PACK_READY   ? UI_LANG_READY
                       : info[i].state == LANG_PACK_OTHER ? UI_LANG_OTHER
                       : info[i].state == LANG_PACK_REFUSED ? UI_LANG_REFUSED
                                                           : UI_LANG_DAMAGED;
        out[i].missing = info[i].missing;
    }
    return n;
}

static bool lang_font(void *ctx, const char *locale, const char *role, const void **data,
                      size_t *size)
{
    (void)ctx;
    return lang_packs_font(locale, role, data, size);
}

static uint32_t lang_generation(void *ctx)
{
    (void)ctx;
    return lang_packs_generation();
}

/* The asset packs (docs/features/39 asset-packs.md "The panel"): RAM only, any task. */
static bool asset_font(void *ctx, const char *role, const void **data, size_t *size)
{
    (void)ctx;
    return asset_packs_font(role, data, size);
}

static bool asset_picture(void *ctx, const char *name, const void **data, size_t *size)
{
    (void)ctx;
    return asset_packs_picture(name, data, size);
}

static uint32_t asset_generation(void *ctx)
{
    (void)ctx;
    return asset_packs_generation();
}

static bool asset_letters_match(void *ctx, uint32_t fingerprint)
{
    (void)ctx;
    return asset_packs_letters_match(fingerprint);
}
#endif

/* ---- this keypad and the panel ----------------------------------------------------------------- */

static void save_options(void *ctx, const ui_options_t *opt)
{
    (void)ctx;
    panel_prefs_t p = {
        .brightness = opt->brightness,
        .saver_brightness = opt->saver_brightness,
        .saver_after_ms = opt->saver_after_ms,
        .off_after_ms = opt->off_after_ms,
        .night_on = opt->night_on,
        .night_start = opt->night_start,
        .night_end = opt->night_end,
        .night_brightness = opt->night_brightness,
        .night_off_after_ms = opt->night_off_after_ms,
        .clock_mode = opt->clock_mode,
        .volume = opt->volume,
        .chime_volume = opt->chime_volume,
        .voice_volume = opt->voice_volume,
        .key_click = opt->key_click,
        .appearance = opt->appearance,
        .high_contrast = opt->high_contrast,
        .voice_status = opt->voice_status,
        .voice_chime = opt->voice_chime,
    };
#if CONFIG_ALARM_FEATURE_VOICE
    /* D06: announcements are switched on only while the voice is ready; the screen shows the
     * refusal itself. One already on stays on through a save of another option while the voice
     * is still loading at start-up (the service says nothing until it is ready). */
    if (opt->voice_enabled) {
        panel_prefs_t stored;
        panel_prefs_load(&stored);
        p.voice_enabled = stored.voice_enabled || panel_voice_ready();
    }
#endif
    memcpy(p.lang, opt->lang, sizeof(p.lang));
    p.lang[sizeof(p.lang) - 1u] = '\0';
    memcpy(p.region, opt->region, sizeof(p.region));
    p.region[sizeof(p.region) - 1u] = '\0';
    panel_prefs_save(&p);
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    auto_names_set_clock(p.clock_mode, p.region, p.lang); /* an automation's words, D30 */
#endif
#if CONFIG_ALARM_FEATURE_VOICE
    const panel_voice_prefs_t vp = {
        .enabled = p.voice_enabled,
        .status = p.voice_status,
        .chime = p.voice_chime,
        .volume = p.voice_volume, /* the voice's own volume (AUD-032) */
    };
    panel_voice_set_prefs(&vp);
#endif
}

static void set_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    sounder_set_volume(percent); /* Beeps only: the chime and the voice have their own (AUD-032) */
}

static void set_chime_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    sounder_set_chime_volume(percent); /* AUD-032 */
}

static void set_voice_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    sounder_set_voice_volume(percent);  /* AUD-032 */
    panel_voice_set_volume(percent); /* 0 = no speech (docs/features/08 D06) */
}

static void about(void *ctx, ui_about_t *out)
{
    (void)ctx;
    panel_about_t a;
    panel_system_about(&a);
    memset(out, 0, sizeof(*out));
    strlcpy(out->firmware, a.firmware, sizeof(out->firmware));
    strlcpy(out->built, a.built, sizeof(out->built));
    strlcpy(out->idf, a.idf, sizeof(out->idf));
    strlcpy(out->chip, a.chip, sizeof(out->chip));
    out->uptime_s = a.uptime_s;
    out->heap_internal_kb = a.heap_internal_kb;
    out->heap_psram_kb = a.heap_psram_kb;
    panel_log_stats(&out->log_count, &out->log_capacity);
}

/* ---- the voice (docs/features/08 voice-first-build.md D03, D06, D07, D09) ------------------- */
#if CONFIG_ALARM_FEATURE_VOICE
_Static_assert((int)UI_VOICE_OFF == (int)PANEL_VOICE_OFF && (int)UI_VOICE_NONE == (int)PANEL_VOICE_NONE &&
                   (int)UI_VOICE_LOADING == (int)PANEL_VOICE_LOADING &&
                   (int)UI_VOICE_READY == (int)PANEL_VOICE_READY &&
                   (int)UI_VOICE_UNAVAILABLE == (int)PANEL_VOICE_UNAVAILABLE,
               "ui_voice_state_t and panel_voice_state_t map one to one");
_Static_assert((int)UI_VOICE_PREVIEW_OK == (int)PANEL_VOICE_PREVIEW_OK &&
                   (int)UI_VOICE_PREVIEW_UNAVAILABLE == (int)PANEL_VOICE_PREVIEW_UNAVAILABLE &&
                   (int)UI_VOICE_PREVIEW_BAD_WORDS == (int)PANEL_VOICE_PREVIEW_BAD_WORDS &&
                   (int)UI_VOICE_PREVIEW_TOO_LONG == (int)PANEL_VOICE_PREVIEW_TOO_LONG &&
                   (int)UI_VOICE_PREVIEW_QUIET == (int)PANEL_VOICE_PREVIEW_QUIET,
               "ui_voice_preview_t and panel_voice_preview_t map one to one");

static void voice_info(void *ctx, ui_voice_info_t *out)
{
    (void)ctx;
    panel_voice_status_t st;
    panel_voice_status(&st);
    memset(out, 0, sizeof(*out));
    out->state = (uint8_t)st.state;
    out->draft = st.draft;
    strlcpy(out->locale, st.locale, sizeof(out->locale));
    strlcpy(out->voice_name, st.voice_id, sizeof(out->voice_name));
    out->pack_revision = st.pack_revision;
}

static const vp_pack_t *voice_pack_acquire(void *ctx)
{
    (void)ctx;
    return panel_voice_pack_acquire();
}

static void voice_pack_release(void *ctx, const vp_pack_t *pack)
{
    (void)ctx;
    panel_voice_pack_release(pack);
}

static uint8_t voice_preview(void *ctx, const uint16_t *ids, size_t n)
{
    (void)ctx;
    return (uint8_t)panel_voice_preview(ids, n);
}

static void voice_preview_stop(void *ctx)
{
    (void)ctx;
    panel_voice_preview_stop(); /* Preview only: announcements and tones go on */
}

typedef struct {
    uint16_t zone;
    ui_voice_seen_t *out;
    bool ok;
} voice_seen_req_t;

static void voice_seen_on_alarm_task(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    voice_seen_req_t *r = ctx;
    r->ok = ui_voice_seen_engine(e, r->zone, r->out);
}

/* D07: the identity the editor's Done saw, read on the alarm task. */
static bool voice_seen(void *ctx, uint16_t zone, ui_voice_seen_t *out)
{
    (void)ctx;
    voice_seen_req_t r = {.zone = zone, .out = out, .ok = false};
    return out != NULL && alarm_service_call(voice_seen_on_alarm_task, &r) == ESP_OK && r.ok;
}

typedef struct {
    uint16_t zone;
    const ui_voice_seen_t *seen;
    bool saved;
    const uint16_t *ids;
    size_t n;
    uint32_t pack_revision;
    uint8_t result;
} voice_approve_req_t;

static void voice_approve_on_alarm_task(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    voice_approve_req_t *r = ctx;
    if (zone_service_voice_settling(r->zone)) {
        r->result = UI_VOICE_APPROVE_STALE; /* a changed sensor's binding still in flight */
        return;
    }
    r->result = ui_voice_approve_engine(e, r->zone, r->seen, r->saved, r->ids, r->n,
                                        r->pack_revision, now);
}

/* D07: the page's save and the approval Done asked for (ui_backend/ui_voice.h). A lease on the
 * pack, held until the approval has answered: it must be the pack whose words the editor showed
 * and must say them (ui_voice_check_pack), else nothing is approved and an unsayable draft is
 * only stored. The save is set_config's own (alarm_service_set_config: its zone resets and its
 * store), always applied; then the approval on the alarm task, against the identity Done saw:
 * with a save, the generation must have moved by exactly that save (ui_voice_approve_engine), so
 * these two alarm-task calls approve only what one transaction would - nothing else can have
 * changed the configuration before, between or after them. */
static uint8_t voice_apply_approve(void *ctx, const alarm_config_t *cfg, uint16_t zone,
                                   const ui_voice_seen_t *seen, const uint16_t *ids, size_t n,
                                   alarm_result_t *saved)
{
    (void)ctx;
    if (saved != NULL) {
        *saved = ALARM_OK;
    }
    const vp_pack_t *lease = panel_voice_pack_acquire();
    uint8_t v = ui_voice_check_pack(lease, seen, ids, n);
    if (cfg != NULL) {
        const alarm_result_t r = alarm_service_set_config(cfg);
        if (saved != NULL) {
            *saved = r;
        }
        if (r != ALARM_OK && v == UI_VOICE_APPROVE_OK) {
            v = UI_VOICE_APPROVE_REFUSED; /* nothing saved: nothing to approve */
        }
    }
    if (v == UI_VOICE_APPROVE_OK) {
        voice_approve_req_t r = {
            .zone = zone,
            .seen = seen,
            .saved = cfg != NULL,
            .ids = ids,
            .n = n,
            .pack_revision = lease->pack_revision,
            .result = UI_VOICE_APPROVE_UNAVAILABLE,
        };
        v = alarm_service_call(voice_approve_on_alarm_task, &r) == ESP_OK
                ? r.result
                : UI_VOICE_APPROVE_REFUSED; /* the alarm task didn't take it */
    }
    if (lease != NULL) {
        panel_voice_pack_release(lease);
    }
    return v;
}
#endif

#if CONFIG_ALARM_FEATURE_STORAGE
static void storage_sd(void *ctx, storage_sd_t *out)
{ (void)ctx; panel_storage_status(out); }
static void storage_flash(void *ctx, storage_flash_t *out)
{ (void)ctx; panel_storage_flash(out); }
static bool storage_action(void *ctx, storage_action_t action, uint32_t generation, uint8_t user)
{ (void)ctx; return panel_storage_request(action, generation, user) == ESP_OK; }
#endif

static void setup_done(void *ctx)
{
    (void)ctx;
    panel_setup_set_done();
}

static void factory_reset(void *ctx, bool erase_log)
{
    (void)ctx;
    panel_system_factory_reset(&SRC, 0, erase_log); /* restarts */
}

/* ---- Home Assistant (MQTT) ----------------------------------------------------------------- */

#if CONFIG_ALARM_FEATURE_MQTT
static void mqtt_get(void *ctx, ui_mqtt_settings_t *out)
{
    (void)ctx;
    net_mqtt_settings_t s;
    net_mqtt_get_settings(&s);
    net_mqtt_status_t st;
    net_mqtt_get_status(&st);
    memset(out, 0, sizeof(*out));
    out->enabled = s.enabled;
    snprintf(out->host, sizeof(out->host), "%s", s.host);
    out->port = s.port;
    snprintf(out->user, sizeof(out->user), "%s", s.user);
    out->password_set = s.password_set;
    out->tls = s.tls;
    out->ca_loaded = st.ca_loaded;
    out->discovery = s.discovery;
    out->commands = s.commands;
    out->allow_panic = s.allow_panic;
    snprintf(out->prefix, sizeof(out->prefix), "%s", s.prefix);
    snprintf(out->panel_id, sizeof(out->panel_id), "%s", s.panel_id[0] ? s.panel_id : st.panel_id);
}

static bool mqtt_set(void *ctx, const ui_mqtt_settings_t *in, char *why, size_t why_len)
{
    (void)ctx;
    net_mqtt_settings_t s;
    net_mqtt_get_settings(&s);
    s.enabled = in->enabled;
    snprintf(s.host, sizeof(s.host), "%s", in->host);
    s.port = in->port;
    snprintf(s.user, sizeof(s.user), "%s", in->user);
    snprintf(s.password, sizeof(s.password), "%s", in->password);
    s.tls = in->tls;
    s.discovery = in->discovery;
    s.commands = in->commands;
    s.allow_panic = in->allow_panic; /* the web lets only the installer change it (BL-107) */
    snprintf(s.prefix, sizeof(s.prefix), "%s", in->prefix);
    snprintf(s.panel_id, sizeof(s.panel_id), "%s", in->panel_id);
    net_mqtt_status_t st;
    net_mqtt_get_status(&st);
    if (strcmp(s.panel_id, st.panel_id) == 0 && s.panel_id[0] != '\0') {
        net_mqtt_settings_t stored;
        net_mqtt_get_settings(&stored);
        if (stored.panel_id[0] == '\0') {
            s.panel_id[0] = '\0'; /* still the one from the MAC: keep following it */
        }
    }
    const bool ok = net_mqtt_set_settings(&s, in->password_change, why, why_len) == ESP_OK;
    memset(&s, 0, sizeof(s));
    return ok;
}

static void mqtt_status(void *ctx, ui_mqtt_status_t *out)
{
    (void)ctx;
    net_mqtt_status_t st;
    net_mqtt_get_status(&st);
    memset(out, 0, sizeof(*out));
    out->state = (ui_mqtt_state_t)st.state;
    out->fail = (ui_mqtt_fail_t)st.fail;
    out->waiting = st.waiting;
    out->sent = st.sent;
    snprintf(out->broker, sizeof(out->broker), "%s", st.broker);
}

#endif
/* ---- push notifications to a phone (net_notify, docs/features/26-28) ---- */

/* The two enums are kept in the same order, so a service's state crosses without a table. */
#if CONFIG_ALARM_FEATURE_NOTIFY
static void notify_status(void *ctx, uint8_t provider, ui_notify_status_t *out)
{
    (void)ctx;
    net_notify_status_t st;
    net_notify_get_status(provider, &st);
    memset(out, 0, sizeof(*out));
    out->state = (ui_notify_state_t)st.state;
    out->fail = (ui_notify_fail_t)st.fail;
    out->waiting = st.waiting;
    out->sent = st.sent;
    out->today = st.today;
    out->limit = st.limit;
    out->ever_sent = st.ever_sent;
    out->ago_s = st.ago_s;
    out->quota_left = st.quota_left;
    snprintf(out->detail, sizeof(out->detail), "%s", st.detail);
}

static bool notify_test(void *ctx, uint8_t provider, char *why, size_t why_len)
{
    (void)ctx;
    return net_notify_test_start(provider, why, why_len) == ESP_OK;
}

static bool notify_test_result(void *ctx, bool *ok, char *why, size_t why_len)
{
    (void)ctx;
    return net_notify_test_result(ok, why, why_len);
}

static void ntfy_get(void *ctx, ui_ntfy_settings_t *out)
{
    (void)ctx;
    net_notify_ntfy_t s;
    net_notify_ntfy_get(&s);
    memset(out, 0, sizeof(*out));
    out->enabled = s.enabled;
    out->self_hosted = s.self_hosted;
    snprintf(out->server, sizeof(out->server), "%s", s.server);
    snprintf(out->topic, sizeof(out->topic), "%s", s.topic);
    out->token_set = s.token_set;
    out->ca_loaded = s.ca_loaded;
    out->cats = s.cats;
    out->silent_too = s.silent_too;
}

static bool ntfy_set(void *ctx, const ui_ntfy_settings_t *in, char *why, size_t why_len)
{
    (void)ctx;
    net_notify_ntfy_t s;
    memset(&s, 0, sizeof(s));
    s.enabled = in->enabled;
    s.self_hosted = in->self_hosted;
    snprintf(s.server, sizeof(s.server), "%s", in->server);
    snprintf(s.topic, sizeof(s.topic), "%s", in->topic);
    snprintf(s.token, sizeof(s.token), "%s", in->token);
    s.cats = in->cats;
    s.silent_too = in->silent_too;
    const esp_err_t err = net_notify_ntfy_set(&s, in->token_change, why, why_len);
    memset(&s, 0, sizeof(s)); /* it carried a token */
    return err == ESP_OK;
}

static void ntfy_new_topic(void *ctx, char *out, size_t len)
{
    (void)ctx;
    net_notify_new_topic(out, len);
}

static void pushover_get(void *ctx, ui_pushover_settings_t *out)
{
    (void)ctx;
    net_notify_pushover_t s;
    net_notify_pushover_get(&s);
    memset(out, 0, sizeof(*out));
    out->enabled = s.enabled;
    out->user_key_set = s.user_key_set;
    out->app_token_set = s.app_token_set;
    out->cats = s.cats;
    out->silent_too = s.silent_too;
    out->emergency = s.emergency;
    snprintf(out->sound, sizeof(out->sound), "%s", s.sound);
}

static bool pushover_set(void *ctx, const ui_pushover_settings_t *in, char *why, size_t why_len)
{
    (void)ctx;
    net_notify_pushover_t s;
    memset(&s, 0, sizeof(s));
    s.enabled = in->enabled;
    snprintf(s.user_key, sizeof(s.user_key), "%s", in->user_key);
    snprintf(s.app_token, sizeof(s.app_token), "%s", in->app_token);
    s.cats = in->cats;
    s.silent_too = in->silent_too;
    s.emergency = in->emergency;
    snprintf(s.sound, sizeof(s.sound), "%s", in->sound);
    const esp_err_t err =
        net_notify_pushover_set(&s, in->user_key_change, in->app_token_change, why, why_len);
    memset(&s, 0, sizeof(s)); /* it carried both credentials */
    return err == ESP_OK;
}


#endif
/* ---- the local Home Assistant link (ha_link, docs/features/23) ---- */

#if CONFIG_ALARM_FEATURE_HA
static void halink_status(void *ctx, ui_halink_status_t *out)
{
    (void)ctx;
    ha_link_status_t st;
    ha_link_get_status(&st);
    memset(out, 0, sizeof(*out));
    out->state = (ui_halink_state_t)st.state; /* the two enums are kept in the same order */
    out->enabled = st.enabled;
    out->paired = st.paired;
    snprintf(out->client, sizeof(out->client), "%s", st.client);
    snprintf(out->code, sizeof(out->code), "%s", st.code);
    out->window_s = st.window_s;
    snprintf(out->panel_id, sizeof(out->panel_id), "%s", st.panel_id);
    out->port = st.port;
}

static bool halink_enable(void *ctx, bool on)
{
    (void)ctx;
    return ha_link_set_enabled(on) == ESP_OK;
}

static bool halink_pair_begin(void *ctx)
{
    (void)ctx;
    return ha_link_pair_begin() == ESP_OK;
}

static void halink_pair_decide(void *ctx, bool allow)
{
    (void)ctx;
    ha_link_pair_decide(allow);
}

static void halink_pair_cancel(void *ctx)
{
    (void)ctx;
    ha_link_pair_cancel();
}

static void halink_unpair(void *ctx)
{
    (void)ctx;
    ha_link_unpair();
}

#endif
/* ---- firmware updates ---- */

static void fw_status(void *ctx, ui_fw_status_t *out)
{
    (void)ctx;
    panel_ota_status_t st;
    panel_ota_get_status(&st);
    memset(out, 0, sizeof(*out));
    switch (st.phase) {
    case PANEL_OTA_CHECKING: out->phase = UI_FW_CHECKING; break;
    case PANEL_OTA_RECEIVING: out->phase = UI_FW_DOWNLOADING; break;
    case PANEL_OTA_WAITING: out->phase = UI_FW_WAITING; break;
    case PANEL_OTA_RESTARTING: out->phase = UI_FW_RESTARTING; break;
    case PANEL_OTA_FAILED: out->phase = UI_FW_FAILED; break;
    default: out->phase = UI_FW_IDLE; break;
    }
    strlcpy(out->running, st.running, sizeof(out->running));
    strlcpy(out->latest, st.latest, sizeof(out->latest));
    out->available = st.available;
    out->has_source = st.source[0] != '\0';
    out->percent = st.percent;
    strlcpy(out->reason,
            st.phase == PANEL_OTA_FAILED ? st.error : st.reason[0] != '\0' ? st.reason : st.waiting_for,
            sizeof(out->reason));
    out->confirming = st.probation;
    strlcpy(out->rolled_back, st.rolled_back, sizeof(out->rolled_back));
    out->checked = st.checked;

    /* the run (BL-188) */
    switch (st.stage) {
    case PANEL_OTA_STAGE_DOWNLOADING: out->stage = UI_FW_STAGE_DOWNLOADING; break;
    case PANEL_OTA_STAGE_VERIFYING: out->stage = UI_FW_STAGE_VERIFYING; break;
    case PANEL_OTA_STAGE_WAITING: out->stage = UI_FW_STAGE_WAITING; break;
    case PANEL_OTA_STAGE_INSTALLING: out->stage = UI_FW_STAGE_INSTALLING; break;
    case PANEL_OTA_STAGE_STOPPING: out->stage = UI_FW_STAGE_STOPPING; break;
    case PANEL_OTA_STAGE_ROLLING_BACK: out->stage = UI_FW_STAGE_ROLLING_BACK; break;
    case PANEL_OTA_STAGE_WIFI_MODULE: out->stage = UI_FW_STAGE_WIFI_MODULE; break;
    case PANEL_OTA_STAGE_FILE: out->stage = UI_FW_STAGE_FILE; break;
    default: out->stage = UI_FW_STAGE_NONE; break;
    }
    out->release = st.kind == PANEL_OTA_KIND_RELEASE;
    strlcpy(out->target, st.target, sizeof(out->target));
    strlcpy(out->previous, st.previous, sizeof(out->previous));
    out->bytes = st.bytes;
    out->size = st.size;
    out->failed_install = st.failed_install;
    switch (st.last.result) {
    case PANEL_OTA_LAST_UPDATED: out->last.result = UI_FW_LAST_UPDATED; break;
    case PANEL_OTA_LAST_ROLLED_BACK: out->last.result = UI_FW_LAST_ROLLED_BACK; break;
    case PANEL_OTA_LAST_STOPPED: out->last.result = UI_FW_LAST_STOPPED; break;
    case PANEL_OTA_LAST_FAILED: out->last.result = UI_FW_LAST_FAILED; break;
    default: out->last.result = UI_FW_LAST_NONE; break;
    }
    strlcpy(out->last.version, st.last.version, sizeof(out->last.version));
    strlcpy(out->last.from, st.last.from, sizeof(out->last.from));
    out->last.at = st.last.at;
    strlcpy(out->last.reason, st.last.reason, sizeof(out->last.reason));
    if (st.last.result == PANEL_OTA_LAST_STOPPED && st.last.by_user != 0u) {
        /* who stopped it, by the household's name for them ("" = the system or no one) */
        static EXT_RAM_BSS_ATTR alarm_config_t cfg; /* the LVGL task only */
        if (alarm_service_get_config(&cfg) == ESP_OK && st.last.by_user <= ALARM_MAX_USERS &&
            cfg.users[st.last.by_user - 1u].name[0] != '\0') {
            strlcpy(out->last.by, cfg.users[st.last.by_user - 1u].name, sizeof(out->last.by));
        } else {
            snprintf(out->last.by, sizeof(out->last.by), "User %02u", (unsigned)st.last.by_user);
        }
    }
    strlcpy(out->installed_channel, st.installed_channel, sizeof(out->installed_channel));
    out->withdrawn = st.withdrawn;
    out->critical = st.critical;
}

static void fw_check(void *ctx)
{
    (void)ctx;
    panel_ota_check();
}

static bool fw_install(void *ctx, uint8_t user)
{
    (void)ctx;
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    return panel_ota_install(&src, user, false) == ESP_OK;
}

static uint8_t fw_stop(void *ctx, uint8_t user)
{
    (void)ctx;
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    switch (panel_ota_stop(&src, user)) {
    case ESP_OK: return UI_FW_STOP_OK;
    case ESP_ERR_NOT_FOUND: return UI_FW_STOP_NOTHING;
    default: return UI_FW_STOP_NOT_STOPPABLE;
    }
}

/* ---- the update server (BL-186) ---- */

static void fw_server(void *ctx, ui_fw_server_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    panel_ota_server_t srv;
    panel_ota_get_server(&srv);
    strlcpy(out->domain, srv.domain, sizeof(out->domain));
    out->official = srv.official;
    strlcpy(out->key_name, srv.key_name, sizeof(out->key_name));
    if (srv.has_key) {
        proto_fwsrv_fingerprint(srv.key_sha256, out->fingerprint);
    }
    panel_ota_status_t st;
    panel_ota_get_status(&st);
    strlcpy(out->channel, st.channel, sizeof(out->channel));
}

static bool fw_server_check(void *ctx, const char *typed)
{
    (void)ctx;
    return panel_ota_server_check(typed) == ESP_OK; /* ESP_ERR_INVALID_ARG / _STATE: false */
}

static void fw_server_check_get(void *ctx, ui_fw_srv_check_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    panel_ota_srv_check_t c;
    panel_ota_server_check_get(&c);
    strlcpy(out->domain, c.domain, sizeof(out->domain));
    switch (c.state) {
    case PANEL_OTA_SRV_CHECKING: out->result = UI_FW_SRV_CHECKING; return;
    case PANEL_OTA_SRV_DONE: break;
    default: out->result = UI_FW_SRV_NONE; return;
    }
    switch (c.result) {
    case PANEL_OTA_SRV_OK: out->result = UI_FW_SRV_OK; break;
    case PANEL_OTA_SRV_UNREACHABLE: out->result = UI_FW_SRV_UNREACHABLE; break;
    case PANEL_OTA_SRV_NOT_A_SERVER: out->result = UI_FW_SRV_NOT_A_SERVER; break;
    case PANEL_OTA_SRV_NO_FIRMWARE: out->result = UI_FW_SRV_NO_FIRMWARE; break;
    default: out->result = UI_FW_SRV_BAD_DOMAIN; break;
    }
    if (c.result == PANEL_OTA_SRV_OK) {
        strlcpy(out->key_name, c.key_name, sizeof(out->key_name));
        memcpy(out->key_sha256, c.key_sha256, sizeof(out->key_sha256));
        proto_fwsrv_fingerprint(c.key_sha256, out->fingerprint);
        out->trusted = c.trusted;
    }
}

static void fw_server_check_forget(void *ctx)
{
    (void)ctx;
    panel_ota_server_check_forget();
}

static uint8_t fw_server_commit(void *ctx, const char *domain, const uint8_t key_sha256[32],
                                uint8_t user)
{
    (void)ctx;
    static EXT_RAM_BSS_ATTR alarm_snapshot_t snap; /* the LVGL task only */
    alarm_service_get_snapshot(&snap);
    const bool factory = (snap.default_codes & ALARM_DEFAULT_CODE_INSTALLER) != 0u;
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    /* the server shown, never the latest check's: another installer's check of another server
     * with the same key must not be kept by this hold (panel_ota compares both) */
    switch (panel_ota_server_commit(domain, key_sha256, factory, &src, user)) {
    case ESP_OK: return UI_FW_COMMIT_OK;
    case ESP_ERR_NOT_ALLOWED: return UI_FW_COMMIT_FACTORY_CODE;
    default: {
        panel_ota_status_t st;
        panel_ota_get_status(&st);
        return st.stage != PANEL_OTA_STAGE_NONE || st.probation ? UI_FW_COMMIT_RUNNING
                                                                 : UI_FW_COMMIT_STALE;
    }
    }
}

static bool fw_set_channel(void *ctx, const char *channel, uint8_t user)
{
    (void)ctx;
    const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    return panel_ota_set_channel(channel, &src, user) == ESP_OK;
}

/* ---- zone sources (zone_service) ---- */

static size_t zone_sources(void *ctx, ui_zone_source_t *out, size_t max)
{
    (void)ctx;
    zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
    const size_t n = zone_service_sources(list, ZONE_HUB_MAX_SOURCES);
    size_t k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        out[k].kind = list[i].kind;
        out[k].caps = list[i].caps;
        out[k].usable = list[i].usable;
        k++;
    }
    return k;
}

static void zone_learn(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    zone_service_learn(kind, on);
}

static size_t zone_candidates(void *ctx, uint8_t kind, const char *query, zsrc_candidate_t *out,
                              size_t max)
{
    (void)ctx;
    return zone_service_candidates(kind, query, 0, out, max);
}

static zone_hub_result_t zone_bind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    return zone_service_bind(zone, b);
}

static zone_hub_result_t zone_unbind(void *ctx, uint16_t zone)
{
    (void)ctx;
    return zone_service_unbind(zone);
}

static bool zone_info(void *ctx, uint16_t zone, zone_hub_info_t *out)
{
    (void)ctx;
    return zone_service_info(zone, out);
}

/* ---- Smart Home (device_service, docs/features/24, BL-045) ---- */

#if CONFIG_ALARM_HOME_CONTROL
/* dev_list's copy of the registry: the LVGL task is its only caller. */
static EXT_RAM_BSS_ATTR hc_device_t s_dev_rows[HC_MAX_DEVICES];

static size_t dev_list(void *ctx, ui_dev_t *out, size_t max)
{
    (void)ctx;
    const size_t rows = device_service_list(s_dev_rows, HC_MAX_DEVICES);
    size_t n = 0u;
    for (size_t i = 0; i < rows && n < max; i++) {
        const hc_device_t *d = &s_dev_rows[i];
        ui_dev_t *u = &out[n++];
        memset(u, 0, sizeof(*u));
        u->kind = d->info.kind;
        u->source = d->info.source;
        u->state = ui_dev_state_parse(d->info.kind, d->info.state);
        u->status = !d->info.online ? UI_DEV_OFFLINE
                    : d->pending != 0u ? UI_DEV_PENDING
                    : d->failed        ? UI_DEV_FAILED
                                       : UI_DEV_IDLE;
        /* The registry names the action under way or failed whoever started it - this screen,
         * the console, an automation - so the tile says "Didn't unlock", not "Didn't respond". */
        u->action = u->status == UI_DEV_PENDING || u->status == UI_DEV_FAILED
                        ? d->action
                        : (uint8_t)ACT_NONE;
        /* HCTL-012: "didn't" only for a source's refusal; no answer in time or a dropped link
         * (the registry's own failures, by origin) is "couldn't confirm" (BL-171) */
        u->unsure = u->status == UI_DEV_FAILED && hc_outcome_unknown(d) ? 1u : 0u;
        /* Brightness, fan speed, position, a thermostat's setpoint, temperature, modes and what
         * it is doing, what a cover or a fan can do and since when, as far as the source knows
         * them (BL-171, BL-174); a cover's class isn't sent yet (no garage colours for a Home
         * Assistant cover). u->kind is set above: the fill reads it. */
        ui_dev_values_fill(u, &d->info.values);
        snprintf(u->ref, sizeof(u->ref), "%s", d->info.ref);
        snprintf(u->name, sizeof(u->name), "%s", d->info.name);
        snprintf(u->area, sizeof(u->area), "%s", d->info.area);
    }
    return n;
}

static ui_dev_result_t dev_act(void *ctx, const char *ref, uint8_t action, int32_t value,
                               const char *code, alarm_result_t *code_result)
{
    (void)ctx;
    hc_device_t d;
    if (ref == NULL || !device_service_get(ref, &d) || d.info.source == (uint8_t)DSRC_ZWAVE ||
        !hc_action_valid((dev_kind_t)d.info.kind, (dev_action_t)action)) {
        return UI_DEV_REFUSED;
    }
    /* HCTL-005, judged here and not by the screen: an unlock or an opening needs a code the
     * engine accepts, a wrong one counting toward the keypad's lockout like any other. */
    if (hc_needs_code((dev_kind_t)d.info.kind, (dev_action_t)action, value)) {
        if (code == NULL || code[0] == '\0') {
            return UI_DEV_NEED_CODE;
        }
        uint8_t user = 0;
        alarm_authority_t authority = ALARM_AUTH_NONE;
        alarm_result_t r = alarm_service_authenticate_as(&SRC, code, 0, &user, &authority);
        /* Until the owner rules otherwise (BL-045), the codes that may disarm at will may
         * unlock: an arm-only or a guest code may not. A duress code unlocks as asked, without
         * the silent duress report disarming sends (decided in BL-045). */
        if (r == ALARM_OK && (authority == ALARM_AUTH_ARM_ONLY || authority == ALARM_AUTH_GUEST)) {
            r = ALARM_ERR_NOT_AUTHORIZED;
        }
        if (r != ALARM_OK) {
            if (code_result != NULL) {
                *code_result = r;
            }
            return UI_DEV_CODE_REFUSED;
        }
        /* The event with who did it is HCTL-007's (docs/features/24 Tasks): not logged yet. */
        (void)user;
    }
    /* A thermostat's mode comes as a ui_dev_state_t (ui_backend.h): the source gets its word. */
    const char *mode = NULL;
    if (action == ACT_SET_MODE) {
        mode = value == UI_DEV_ST_OFF    ? "off"
               : value == UI_DEV_ST_HEAT ? "heat"
               : value == UI_DEV_ST_COOL ? "cool"
               : value == UI_DEV_ST_AUTO ? "auto"
                                         : NULL;
        if (mode == NULL) {
            return UI_DEV_REFUSED;
        }
    }
    switch (device_service_act(ref, (dev_action_t)action, value, mode)) {
    case HC_OK: return UI_DEV_OK;
    case HC_ERR_BUSY: return UI_DEV_BUSY;
    case HC_ERR_OFFLINE: return UI_DEV_UNREACHABLE;
    default: return UI_DEV_REFUSED;
    }
}

/* The layout is one blob in the "ui" namespace (ui_dev_model.h describes it). */
static size_t dev_layout_get(void *ctx, uint8_t *out, size_t max)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK) {
        return 0u;
    }
    size_t len = 0;
    if (nvs_get_blob(h, "dev_layout", NULL, &len) != ESP_OK || len == 0u || len > max ||
        nvs_get_blob(h, "dev_layout", out, &len) != ESP_OK) {
        len = 0;
    }
    nvs_close(h);
    return len;
}

static bool dev_layout_set(void *ctx, const uint8_t *blob, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (blob == NULL || len == 0u || len > UI_DEVL_BLOB_MAX ||
        nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, "dev_layout", blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}
#endif

/* ---- Automations (automation_service, docs/features/41) ----
 * The touchscreen calls these on the LVGL task, holding board_ui_lock; its stack is internal RAM
 * (board_display.c's task_stack_caps), so the edits may write NVS there (automation_service.h
 * refuses an edit from a PSRAM stack). The service answers LOCKED while armed or in alarm. */

#if CONFIG_ALARM_FEATURE_AUTOMATIONS
_Static_assert(UI_AUTO_TRY_BUSY == AUTOMATION_TRY_BUSY, "ui_backend.h and automation_service.h");

static size_t auto_list(void *ctx, auto_rule_t *out, size_t max)
{
    (void)ctx;
    return automation_service_list(out, max);
}

static uint16_t auto_gen(void *ctx)
{
    (void)ctx;
    return automation_service_gen();
}

static bool auto_read_only(void *ctx)
{
    (void)ctx;
    return automation_service_read_only();
}

/* The installer option, read live from the configuration (AUT-015). The LVGL task is the only
 * caller: the zones land in a static. */
static bool auto_less_secure(void *ctx)
{
    (void)ctx;
    static EXT_RAM_BSS_ATTR auto_zone_t zones[ALARM_MAX_ZONES];
    auto_env_t env;
    return automation_service_env(&env, zones) && env.less_secure;
}

static auto_save_t auto_save(void *ctx, const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                             auto_check_t *why, uint16_t *uid_out)
{
    (void)ctx;
    return automation_service_save(r, user, expected_gen, why, uid_out);
}

static auto_save_t auto_delete_slot(void *ctx, uint8_t slot, uint16_t expected_gen)
{
    (void)ctx;
    return automation_service_delete_slot(slot, expected_gen);
}

static auto_save_t auto_set_off(void *ctx, uint16_t uid, bool off, uint16_t expected_gen)
{
    (void)ctx;
    return automation_service_set_off(uid, off, expected_gen);
}

static bool auto_status(void *ctx, uint16_t uid, auto_status_t *out)
{
    (void)ctx;
    return automation_service_status(uid, out);
}

static auto_try_t auto_try(void *ctx, uint16_t uid)
{
    (void)ctx;
    return automation_service_try(uid);
}

_Static_assert(UI_AUTO_POFF_BUSY == AUTOMATION_POFF_BUSY, "ui_backend.h and automation_service.h");

static size_t auto_warnings(void *ctx, auto_warning_t *out, size_t max)
{
    (void)ctx;
    return automation_service_warnings(out, max);
}

/* DESIGN.md section 3 "Arming" 4, section 8: anyone at the wall, any user code. The code is
 * checked on the alarm task as the keypad's (lockout counts; a duress code reports silently and
 * is accepted), then the put-off is asked of the automation task with that code's user. */
static auto_put_off_t auto_put_off(void *ctx, uint16_t uid, uint8_t choice, const char *code,
                                   alarm_result_t *code_result)
{
    (void)ctx;
    alarm_result_t unused;
    if (code_result == NULL) {
        code_result = &unused;
    }
    *code_result = ALARM_ERR_BAD_ARG;
    auto_warning_t w;
    if (!automation_service_warning(uid, &w) || w.state != (uint8_t)AUTO_WARN_ON) {
        return AUTO_POFF_NO_WARNING; /* nothing to put off: no code is spent on it */
    }
    if (code == NULL || code[0] == '\0' || strlen(code) > ALARM_CODE_MAX_LEN ||
        (choice != (uint8_t)AUTO_PUT_OFF_HOUR && choice != (uint8_t)AUTO_PUT_OFF_NIGHT)) {
        return AUTO_POFF_BAD_ARG;
    }
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_CODE_CHECK;
    cmd.partition = w.partition;
    memcpy(cmd.code, code, strlen(code) + 1u);
    alarm_cmd_result_t res;
    memset(&res, 0, sizeof(res));
    *code_result = alarm_service_command(&cmd, &SRC, &res);
    volatile char *wipe = cmd.code; /* the digits don't stay on the stack */
    for (size_t i = 0; i < sizeof(cmd.code); i++) {
        wipe[i] = '\0';
    }
    if (*code_result != ALARM_OK) {
        return UI_AUTO_POFF_CODE;
    }
    return automation_service_put_off(uid, choice, res.user, res.may_disarm);
}
#endif

/* ---- Z-Wave (zwave_service, docs/features/36 screens.md §12) ---- */

#if CONFIG_ALARM_ZSRC_ZWAVE
static bool zw_status(void *ctx, ui_zw_status_t *out)
{
    (void)ctx;
    return zwave_service_ui_status(out);
}

static size_t zw_devices(void *ctx, ui_zw_device_t *out, size_t max)
{
    (void)ctx;
    return zwave_service_ui_devices(out, max);
}

static bool zw_device(void *ctx, uint16_t node, uint16_t generation, ui_zw_device_page_t *out)
{
    (void)ctx;
    return zwave_service_ui_device(node, generation, out);
}

static ui_zw_req_result_t zw_request(void *ctx, const ui_zw_request_t *req)
{
    (void)ctx;
    return zwave_service_ui_request(req);
}

static void zw_progress(void *ctx, ui_zw_progress_t *out)
{
    (void)ctx;
    zwave_service_ui_progress(out);
}

static size_t zw_prov(void *ctx, ui_zw_prov_t *out, size_t max)
{
    (void)ctx;
    return zwave_service_ui_prov(out, max);
}

static ui_zw_prov_result_t zw_prov_register(void *ctx, const char *code, const char *name)
{
    (void)ctx;
    return zwave_service_ui_prov_register(code, name);
}

static ui_zw_prov_result_t zw_prov_change(void *ctx, const uint8_t dsk[16], uint8_t op,
                                          uint8_t arg)
{
    (void)ctx;
    return zwave_service_ui_prov_change(dsk, op, arg);
}
#endif

#if CONFIG_ALARM_ZSRC_ZIGBEE
/* The Zigbee pages (ZB-027): zigbee_service reads its zbz_panel under the zone lock; a request
 * checks installer mode, the partitions and RESET's installer code (as this keypad) itself. */
static bool zb_status(void *ctx, ui_zb_status_t *out)
{
    (void)ctx;
    return zigbee_service_ui_status(out);
}

static size_t zb_devices(void *ctx, ui_zb_device_t *out, size_t max)
{
    (void)ctx;
    return zigbee_service_ui_devices(out, max);
}

static bool zb_device(void *ctx, uint16_t id, ui_zb_device_page_t *out)
{
    (void)ctx;
    return zigbee_service_ui_device(id, out);
}

static ui_zb_req_result_t zb_request(void *ctx, const ui_zb_request_t *req)
{
    (void)ctx;
    return zigbee_service_ui_request(req);
}

static void zb_progress(void *ctx, ui_zb_progress_t *out)
{
    (void)ctx;
    zigbee_service_ui_progress(out);
}

static size_t zb_codes(void *ctx, ui_zb_code_t *out, size_t max)
{
    (void)ctx;
    return zigbee_service_ui_codes(out, max);
}
#endif

void ui_port_esp_backend(ui_backend_t *out)
{
    memset(out, 0, sizeof(*out));
    out->snapshot_seq = snapshot_seq;
    out->get_snapshot = get_snapshot;
    out->get_config = get_config;
    out->recent_events = recent_events;
    out->now_ms = now_ms;
    out->epoch_ms = epoch_ms;
    out->command = command;
    out->change_code = change_code;
    out->net_state = net_state;
    out->beep = beep;
    out->authenticate = authenticate;
    out->program_enter = program_enter;
    out->program_exit = program_exit;
    out->program_keepalive = program_keepalive;
    out->program_set_code = program_set_code;
    out->set_config = set_config;
    out->user_set = user_set;
    out->user_update = user_update;
    out->user_delete = user_delete;
    out->log_read = log_read;
    out->boot_id = boot_id;
    out->log_loss = log_loss;
    out->log_clear_loss = log_clear_loss;
    out->wifi_info = wifi_info;
    out->wifi_scan = wifi_scan;
    out->wifi_results = wifi_results;
    out->wifi_connect = wifi_connect;
    out->ip_get = ip_get;
    out->ip_set = ip_set;
    out->net_check_start = net_check_begin;
    out->net_check_status = net_check_read;
    out->get_timezone = get_timezone;
    out->set_timezone = set_timezone;
    out->get_ntp = get_ntp;
    out->set_ntp = set_ntp;
#if CONFIG_ALARM_FEATURE_RTC
    out->clock_info = clock_info;
    out->clock_set = clock_set;
#endif
#if CONFIG_ALARM_FEATURE_LANG_PACKS
    out->lang_catalog = lang_catalog;
    out->lang_list = lang_list;
    out->lang_generation = lang_generation;
    out->lang_font = lang_font;
    out->asset_font = asset_font;
    out->asset_picture = asset_picture;
    out->asset_generation = asset_generation;
    out->asset_letters_match = asset_letters_match;
#endif
    out->save_options = save_options;
    out->set_volume = set_volume;
    out->set_chime_volume = set_chime_volume;
    out->set_voice_volume = set_voice_volume;
    out->about = about;
#if CONFIG_ALARM_FEATURE_STORAGE
    out->storage_sd = storage_sd;
    out->storage_flash = storage_flash;
    out->storage_action = storage_action;
#endif
#if CONFIG_ALARM_FEATURE_VOICE
    out->voice_info = voice_info;
    out->voice_pack_acquire = voice_pack_acquire;
    out->voice_pack_release = voice_pack_release;
    out->voice_preview = voice_preview;
    out->voice_seen = voice_seen;
    out->voice_apply_approve = voice_apply_approve;
    out->voice_preview_stop = voice_preview_stop;
#endif
    out->setup_done = setup_done;
    out->factory_reset = factory_reset;
#if CONFIG_ALARM_FEATURE_MQTT
    out->mqtt_get = mqtt_get;
    out->mqtt_set = mqtt_set;
    out->mqtt_status = mqtt_status;
#endif
#if CONFIG_ALARM_FEATURE_NOTIFY
    out->notify_status = notify_status;
    out->notify_test = notify_test;
    out->notify_test_result = notify_test_result;
    out->ntfy_get = ntfy_get;
    out->ntfy_set = ntfy_set;
    out->ntfy_new_topic = ntfy_new_topic;
    out->pushover_get = pushover_get;
    out->pushover_set = pushover_set;
#endif
#if CONFIG_ALARM_FEATURE_HA
    out->halink_status = halink_status;
    out->halink_enable = halink_enable;
    out->halink_pair_begin = halink_pair_begin;
    out->halink_pair_decide = halink_pair_decide;
    out->halink_pair_cancel = halink_pair_cancel;
    out->halink_unpair = halink_unpair;
#endif
    out->fw_status = fw_status;
    out->fw_check = fw_check;
    out->fw_install = fw_install;
    out->fw_stop = fw_stop;
    out->fw_server = fw_server;
    out->fw_server_check = fw_server_check;
    out->fw_server_check_get = fw_server_check_get;
    out->fw_server_check_forget = fw_server_check_forget;
    out->fw_server_commit = fw_server_commit;
    out->fw_set_channel = fw_set_channel;
    out->zone_sources = zone_sources;
    out->zone_learn = zone_learn;
    out->zone_candidates = zone_candidates;
    out->zone_bind = zone_bind;
    out->zone_unbind = zone_unbind;
    out->zone_info = zone_info;
#if CONFIG_ALARM_HOME_CONTROL
    out->dev_list = dev_list;
    out->dev_act = dev_act;
    out->dev_layout_get = dev_layout_get;
    out->dev_layout_set = dev_layout_set;
#endif
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    out->auto_list = auto_list;
    out->auto_gen = auto_gen;
    out->auto_read_only = auto_read_only;
    out->auto_less_secure = auto_less_secure;
    out->auto_save = auto_save;
    out->auto_delete_slot = auto_delete_slot;
    out->auto_set_off = auto_set_off;
    out->auto_status = auto_status;
    out->auto_try = auto_try;
    out->auto_warnings = auto_warnings;
    out->auto_put_off = auto_put_off;
#endif
#if CONFIG_ALARM_ZSRC_ZWAVE
    out->zw_status = zw_status;
    out->zw_devices = zw_devices;
    out->zw_device = zw_device;
    out->zw_request = zw_request;
    out->zw_progress = zw_progress;
    out->zw_prov = zw_prov;
    out->zw_prov_register = zw_prov_register;
    out->zw_prov_change = zw_prov_change;
#endif
#if CONFIG_ALARM_ZSRC_ZIGBEE
    out->zb_status = zb_status;
    out->zb_devices = zb_devices;
    out->zb_device = zb_device;
    out->zb_request = zb_request;
    out->zb_progress = zb_progress;
    out->zb_codes = zb_codes;
#endif
}

void ui_port_esp_options(ui_options_t *out)
{
    ui_options_defaults(out);
    panel_prefs_t p;
    panel_prefs_load(&p);
    out->brightness = p.brightness;
    out->saver_brightness = p.saver_brightness;
    out->saver_after_ms = p.saver_after_ms;
    out->off_after_ms = p.off_after_ms;
    out->night_on = p.night_on;
    out->night_start = p.night_start;
    out->night_end = p.night_end;
    out->night_brightness = p.night_brightness;
    out->night_off_after_ms = p.night_off_after_ms;
    out->clock_mode = p.clock_mode;
    out->volume = p.volume;
    out->chime_volume = p.chime_volume;
    out->voice_volume = p.voice_volume;
    out->key_click = p.key_click;
    out->appearance = p.appearance;
    out->high_contrast = p.high_contrast;
    out->voice_enabled = p.voice_enabled;
    out->voice_status = p.voice_status;
    out->voice_chime = p.voice_chime;
    memcpy(out->lang, p.lang, sizeof(out->lang));
    memcpy(out->region, p.region, sizeof(out->region));
    out->run_setup = !panel_setup_done();
}
