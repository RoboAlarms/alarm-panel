/*
 * panel_app.c - the panel's start-up, shared by every board (docs/features/33 step 7).
 *
 * Start-up order: storage and networking stacks -> power rails -> audio -> alarm engine
 * (the panel is protecting from here on) -> zone sources (simulator, wired terminals) ->
 * firmware updates (self-test after an update) -> display and touch -> health -> network ->
 * MQTT -> console. A display or network failure never stops the alarm engine.
 *
 * Two rules hold across the whole of it:
 *  - Nothing here ever erases or rewrites what is stored to get past a failure. Storage that
 *    cannot be read is reported and left alone; erasing is something the owner asks for
 *    ("factory_reset confirm"), never something the panel decides (R06).
 *  - A start-up that cannot finish does not pretend otherwise: the console comes up so the
 *    fault can be read and acted on, and the panel then restarts, which is also what lets a
 *    firmware update still on probation roll itself back (R07).
 *
 * Moved here from firmware/crowpanel_p4/main/app_main.c; the board-specific parts reach it
 * through the board contract (board_api.h) and the hooks in panel_app_config_t.
 *
 * A panel stored in the panel link's keypad role (docs/features/11, ALARM_LINK) takes another
 * path, run_keypad(): no engine, event log, zone sources or integrations - the screen shows its
 * master's status through the link, and its voice speaks what the link hears (LINK-028).
 */
#include "panel_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_service.h"
#include "auto_names.h"
#include "automation_service.h"
#include "backup_service.h"
#include "board_api.h"
#include "device_service.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_link.h"
#include "net_check.h"
#include "net_mqtt.h"
#include "net_notify.h"
#include "net_wifi.h"
#include "nvs_flash.h"
#include "panel_clock.h"
#include "panel_console.h"
#include "panel_log.h"
#include "asset_packs.h"
#include "lang_packs.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "panel_voice.h"
#include "panel_storage.h"
#include "power_monitor.h"
#include "rf_service.h"
#include "sounder.h"
#include "zigbee_service.h"
#include "zone_service.h"
#include "zwave_service.h"
#if CONFIG_ALARM_LINK
#include "plink_service.h"
#endif
#if CONFIG_ALARM_FEATURE_UI
#include "ui_app.h"
#include "ui_port_esp.h"
#include "ui_status.h"
#endif

static const char *TAG = "main";
static const panel_app_config_t *s_cfg;

/* Once the engine runs, a part that fails to start is reported and lived without: an abort
 * would restart the panel into the same abort, with the service console (the recovery path)
 * never reached. */
static void start_or_log(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s (the panel keeps protecting without it)", what,
                 esp_err_to_name(err));
    }
}
#define START_OR_LOG(call) start_or_log((call), #call)

/* Filter owned by the alarm task; battery sampling belongs to the health task. On a keypad of
 * the panel link there is no alarm task: the health task keeps the filter and publishes its
 * status in s_power_seen. */
static power_monitor_t s_power;
static bool s_keypad; /* this panel runs in the keypad role (set before anything starts) */
static portMUX_TYPE s_power_mux = portMUX_INITIALIZER_UNLOCKED;
static power_sample_t s_power_seen;

#define KEYPAD_PARTITION 1u /* the local keypad shows partition 1 */

/* How often the health task looks at every part of the panel. Each part has to report inside
 * PANEL_OTA_HEALTH_PERIOD_MS (5 s), so this has to be comfortably shorter. */
#define HEALTH_TICK_MS 2000u

/* A start-up that failed leaves the service console up for this long before restarting, so
 * whoever is connected can read why and run "factory_reset confirm" if that is the way out.
 * The restart is what lets a firmware update on probation roll itself back. */
#define RECOVERY_WINDOW_MS 120000u

/* Internal RAM that Wi-Fi, lwIP and one TLS session take after the display has started, on a
 * board without PSRAM (CONFIG_ALARM_UI_LVGL_BUDGET_KB > 0): the touch UI must leave it free. */
#define UI_NET_RESERVE_KB 64u

/* ---- sounder wiring ----------------------------------------------------------------- */

static esp_err_t audio_write(void *ctx, const int16_t *frames, size_t count)
{
    (void)ctx;
    return board_audio_write(frames, count, portMAX_DELAY);
}

static void audio_amp(void *ctx, bool on)
{
    (void)ctx;
    board_audio_amp(on);
}

/* The voice (docs/features/08 voice-first-build.md D11) rides on this listener too: the engine's
 * listeners are full. Its calls only copy into the voice's queue (a zero-tick send, no lock):
 * nothing here blocks or calls the engine. A tracked tone's ticket lets speech that follows it
 * (a chime's zone name) wait for its last block. */
static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    panel_voice_engine_event(ev);
}

static void on_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone)
{
    (void)ctx;
    uint32_t ticket = 0u;
    if (partition == 0u || partition == KEYPAD_PARTITION) {
#if CONFIG_ALARM_FEATURE_VOICE
        ticket = sounder_play_tracked(sound);
#else
        sounder_play(sound);
#endif
    }
    panel_voice_engine_sound(partition, sound, zone, ticket);
}

static void on_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    sounder_set_continuous(snap->part[KEYPAD_PARTITION - 1u].sounder);
    panel_voice_engine_snapshot(snap); /* after the tone: a loud pattern cancels speech */
    /* The engine's listeners are full (alarm_service.c MAX_LISTENERS): the Zigbee sirens and
     * keypads take theirs from this one (copies only; nothing until the service started). */
    zigbee_service_engine_snapshot(snap);
}

static void on_output(void *ctx, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    (void)ctx;
    zigbee_service_engine_output(output, partition, pattern);
    panel_voice_engine_output(output, partition, pattern);
}

static void copy_power(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)e;
    (void)now;
    *(power_sample_t *)ctx = s_power.status;
}

esp_err_t panel_app_power_status(power_sample_t *out)
{
    if (s_keypad) {
        portENTER_CRITICAL(&s_power_mux);
        *out = s_power_seen;
        portEXIT_CRITICAL(&s_power_mux);
        return ESP_OK;
    }
    return alarm_service_call(copy_power, out);
}

#if CONFIG_ALARM_FEATURE_UI
static uint8_t s_ui_brightness;
static bool s_power_saving; /* LVGL task only */
static power_saving_t s_power_policy;

static void ui_power(void *ctx, ui_power_info_t *out)
{
    (void)ctx;
    power_sample_t status;
    *out = (ui_power_info_t){.state = UI_BATTERY_UNKNOWN, .percent = -1};
    if (panel_app_power_status(&status) != ESP_OK) {
        return;
    }
    out->state = status.battery_present == POWER_YES ? UI_BATTERY_PRESENT :
                 status.battery_present == POWER_NO ? UI_BATTERY_ABSENT : UI_BATTERY_UNKNOWN;
    out->percent = status.battery_percent;
    out->low = status.battery_low == POWER_YES;
    out->charging = status.charging == POWER_YES;
    /* No-charge does not prove AC loss. Conservatively save power when a valid
     * cell is present without confirmed external power; retain savings on errors. */
    const bool saving = power_saving_update(&s_power_policy, &status, alarm_service_now());
    if (saving != s_power_saving) {
        s_power_saving = saving;
        board_backlight_set(power_backlight_limit(s_ui_brightness, saving));
        sounder_set_power_saving(saving);
        panel_voice_set_power_saving(saving); /* no automatic speech, no Preview */
    }
}

/* ---- touch UI ----------------------------------------------------------------------- */

static void ui_backlight(void *ctx, uint8_t percent)
{
    (void)ctx;
    s_ui_brightness = percent;
    board_backlight_set(power_backlight_limit(percent, s_power_saving));
}
#endif

/* ---- firmware updates in Home Assistant (update entity) ---------------------------- */

#if CONFIG_ALARM_FEATURE_MQTT
static void fw_get(void *ctx, net_mqtt_fw_t *out)
{
    (void)ctx;
    panel_ota_status_t st;
    panel_ota_get_status(&st);
    snprintf(out->installed, sizeof(out->installed), "%s", st.running);
    snprintf(out->latest, sizeof(out->latest), "%s", st.available ? st.latest : st.running);
    snprintf(out->release_url, sizeof(out->release_url), "%s", st.notes_url);
    /* OTA-026: a release update is "in progress" while it downloads, is verified, installs or
     * rolls back - not while it waits (that can last all day) and not during a plain check. The
     * Wi-Fi module and a developer's file have no steps: while they write or restart. */
    const bool stepless = st.kind == PANEL_OTA_KIND_FILE || st.kind == PANEL_OTA_KIND_WIFI_MODULE;
    out->in_progress = st.stage == PANEL_OTA_STAGE_DOWNLOADING ||
                       st.stage == PANEL_OTA_STAGE_VERIFYING ||
                       st.stage == PANEL_OTA_STAGE_INSTALLING ||
                       st.stage == PANEL_OTA_STAGE_ROLLING_BACK ||
                       (stepless && (st.phase == PANEL_OTA_RECEIVING ||
                                     st.phase == PANEL_OTA_RESTARTING));
    out->percent = st.phase == PANEL_OTA_RECEIVING && (st.stage == PANEL_OTA_STAGE_DOWNLOADING ||
                                                       stepless)
                       ? st.percent
                       : -1;
}

static esp_err_t fw_install(void *ctx)
{
    (void)ctx;
    const alarm_source_t src = {.kind = ALARM_SRC_REMOTE, .id = 1};
    return panel_ota_install(&src, 0, false);
}

static void fw_changed(void *ctx)
{
    (void)ctx;
    net_mqtt_firmware_changed();
}

/* ---- the connection check's last step ----------------------------------------------- */

/* Where the connected service is, so "Check the connection" can try it too. net_check lives
 * in platform_esp, which the MQTT client builds on, so the address comes from here. */
static void check_service(void *ctx, bool *enabled, char *host, size_t host_len, uint16_t *port)
{
    (void)ctx;
    net_mqtt_settings_t s;
    net_mqtt_get_settings(&s);
    *enabled = s.enabled && s.host[0] != '\0';
    snprintf(host, host_len, "%s", s.host);
    *port = s.port != 0u ? s.port : (s.tls == NET_MQTT_TLS_OFF ? 1883u : 8883u);
}

#endif

/* ---- display service command -------------------------------------------------------- */

#if CONFIG_ALARM_SERVICE_CONSOLE
static void print_display(const board_display_stats_t *a, const board_display_stats_t *b,
                          int64_t us)
{
    const uint32_t frames = b->frames - a->frames;
    printf("frames %" PRIu32 " in %.1f s (%.1f/s), late %" PRIu32 ", longest gap %.1f ms\n", frames,
           (double)us / 1e6, us > 0 ? (double)frames * 1e6 / (double)us : 0.0, b->late - a->late,
           (double)b->max_gap_us / 1000.0);
    printf("UI redraws %" PRIu32 " (%" PRIu32 " since start)\n", b->flushes - a->flushes,
           b->flushes);
    printf("backlight %u %%%s%s, guard trips %" PRIu32 "\n", (unsigned)b->backlight,
           b->forced_off ? " (forced off in this build)" : "",
           b->guard_on ? " (held off by the flicker guard)" : "", b->guard_trips);
    printf("screen refreshes %" PRIu32 " (%" PRIu32 " slower than 50 ms), longest %.1f ms\n",
           b->refreshes - a->refreshes, b->slow_refreshes - a->slow_refreshes,
           (double)b->max_refresh_us / 1000.0);
    printf("free internal RAM %u KB (largest block %u KB), PSRAM %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024u),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
}

static void print_touch(const board_touch_stats_t *a, const board_touch_stats_t *b)
{
    printf("touch polls %" PRIu32 ", longest gap %.1f ms, touches %" PRIu32 ", longest touch "
           "%" PRIu32 " ms, I2C errors %" PRIu32 "%s\n",
           b->polls - a->polls, (double)b->max_gap_us / 1000.0, b->presses - a->presses,
           b->longest_press_ms, b->errors - a->errors, b->pressed ? ", finger down now" : "");
}

/* One line a second while someone uses the screen: frames, redraws, touch. */
static void watch_display(int seconds)
{
    printf("   s  frames late  refr slow max_ms  polls gap_ms touches err\n");
    for (int i = 1; i <= seconds; i++) {
        board_display_stats_t a;
        board_display_stats_t b;
        board_touch_stats_t ta;
        board_touch_stats_t tb;
        board_display_stats(&a, true);
        board_touch_stats(&ta, true);
        vTaskDelay(pdMS_TO_TICKS(1000));
        board_display_stats(&b, false);
        board_touch_stats(&tb, false);
        printf("%4d  %6" PRIu32 " %4" PRIu32 "  %4" PRIu32 " %4" PRIu32 " %6.1f  %5" PRIu32
               " %6.1f %7" PRIu32 " %3" PRIu32 "%s\n",
               i, b.frames - a.frames, b.late - a.late, b.refreshes - a.refreshes,
               b.slow_refreshes - a.slow_refreshes, (double)b.max_refresh_us / 1000.0,
               tb.polls - ta.polls, (double)tb.max_gap_us / 1000.0, tb.presses - ta.presses,
               tb.errors - ta.errors, tb.pressed ? "  (down)" : "");
    }
}

/* Redraws the whole screen (tests the UI's path into the frame buffer). */
static void redraw_screen(void)
{
#if CONFIG_ALARM_FEATURE_UI
    if (board_ui_lock(500)) {
        lv_obj_invalidate(lv_screen_active());
        board_ui_unlock();
    }
#endif
}

/* "display": refresh rate and late frames over 2 s, with two full redraws. "display stress <s>":
 * flash erase and write like a firmware update (on the unused fw_cache partition) while the
 * screen is redrawn and the refresh is timed. "display watch <s>": a line a second. */
static int cmd_display(int argc, char **argv)
{
    int seconds = 0;
    if (argc >= 3 && strcmp(argv[1], "watch") == 0) {
        watch_display(atoi(argv[2]) > 0 ? atoi(argv[2]) : 30);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "stress") == 0) {
        seconds = atoi(argv[2]);
    }
    board_display_stats_t a;
    board_display_stats_t b;
    board_touch_stats_t ta;
    board_touch_stats_t tb;
    board_display_stats(&a, true);
    board_touch_stats(&ta, true);
    const int64_t start = esp_timer_get_time();
    if (seconds <= 0) {
        redraw_screen();
        vTaskDelay(pdMS_TO_TICKS(1000));
        redraw_screen();
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        const esp_partition_t *p =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fw_cache");
        if (p == NULL) {
            printf("no fw_cache partition\n");
            return 1;
        }
        static uint8_t buf[1024];
        memset(buf, 0xA5, sizeof(buf));
        uint32_t off = 0;
        uint32_t sectors = 0;
        while (esp_timer_get_time() - start < (int64_t)seconds * 1000000) {
            esp_partition_erase_range(p, off, 4096);
            for (uint32_t i = 0; i < 4u; i++) {
                esp_partition_write(p, off + i * 1024u, buf, sizeof(buf));
            }
            off = (off + 4096u) % (uint32_t)p->size;
            sectors++;
            if (sectors % 4u == 0u) {
                redraw_screen();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        printf("erased and wrote %" PRIu32 " sectors\n", sectors);
    }
    board_display_stats(&b, false);
    board_touch_stats(&tb, false);
    print_display(&a, &b, esp_timer_get_time() - start);
    print_touch(&ta, &tb);
    return 0;
}

/* "health": which parts of the panel are still reporting that they do their work, and the
 * progress counters behind that. Nobody has to look at the screen for any of it. */
static int cmd_health(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const struct {
        uint32_t bit;
        const char *name;
    } PARTS[] = {
        {PANEL_OTA_OK_ENGINE, "alarm engine"},  {PANEL_OTA_OK_LOG, "event log"},
        {PANEL_OTA_OK_UI, "screen"},            {PANEL_OTA_OK_SENSORS, "sensor input"},
        {PANEL_OTA_OK_TOUCH, "touch"},          {PANEL_OTA_OK_LINK, "panel link"},
        {PANEL_OTA_OK_KEYPADS, "keypads"},
    };
    const uint32_t stale = panel_ota_health_stale();
    printf("%s\n", stale == 0u ? "every part of the panel reports that it is working"
                               : "NOT RUNNING - the panel may not be protecting the house:");
    for (size_t i = 0; i < sizeof(PARTS) / sizeof(PARTS[0]); i++) {
        if ((stale & PARTS[i].bit) != 0u) {
            printf("  %s\n", PARTS[i].name);
        }
    }
    board_touch_stats_t t;
    board_touch_stats(&t, false);
    printf("passes: log %" PRIu32 ", zone sources %" PRIu32 ", touch reads %" PRIu32
           " of %" PRIu32 "\n",
           panel_log_passes(), zone_service_passes(), t.polls - t.errors, t.polls);
    return 0;
}

static const char *power_word(power_value_t value)
{
    return value == POWER_YES ? "yes" : value == POWER_NO ? "no" : "unknown";
}

/* "power": what the panel knows about its supply; a board with its own sensing reports it
 * its own way (the power_report hook). */
static int cmd_power(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        return 1;
    }
    if (s_cfg->power_report != NULL) {
        return s_cfg->power_report();
    }
    power_sample_t status;
    if (panel_app_power_status(&status) != ESP_OK) {
        return 1;
    }
    printf("Power: AC present: %s; battery present: %s; battery low: %s; charging: %s\n"
           "  battery mV: %d; percent: %d (-1 = unavailable)\n",
           power_word(status.ac_present), power_word(status.battery_present),
           power_word(status.battery_low), power_word(status.charging), status.battery_mv,
           status.battery_percent);
    return 0;
}

static esp_err_t app_console_register(void)
{
    const esp_console_cmd_t power = {
        .command = "power", .help = "Read power sensing availability and battery measurements",
        .func = cmd_power,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&power), TAG, "power console");
    const esp_console_cmd_t health = {
        .command = "health",
        .help = "Which parts of the panel are still reporting that they work",
        .func = cmd_health,
    };
    const esp_err_t err = esp_console_cmd_register(&health);
    if (err != ESP_OK) {
        return err;
    }
    const esp_console_cmd_t cmd = {
        .command = "display",
        .help = "Display and touch check: frames, late frames, redraws, touch polls (stress <s>: "
                "with flash writes; watch <s>: a line a second)",
        .hint = "[stress <seconds> | watch <seconds>]",
        .func = cmd_display,
    };
    return esp_console_cmd_register(&cmd);
}
#endif

/* ---- zone sources ------------------------------------------------------------------- */

/* Logs every problem the board's description has with the pins in it; true when none. */
static bool board_pins_safe(const board_info_t *info, const char *what)
{
    board_issue_t issues[8];
    const size_t n = board_info_validate(info, issues, 8);
    for (size_t i = 0; i < n && i < 8; i++) {
        ESP_LOGE(TAG, "%s: GPIO%d (%s) %s%s%s", what, issues[i].gpio,
                 issues[i].use != NULL ? issues[i].use : "?", board_issue_text(issues[i].code),
                 issues[i].other != NULL ? ": " : "", issues[i].other != NULL ? issues[i].other : "");
    }
    return n == 0;
}

#if CONFIG_ALARM_ZSRC_RF || CONFIG_ALARM_FEATURE_RTC || CONFIG_ALARM_ZSRC_ZWAVE
/* The zone that uses gpio as a wired terminal, or 0. */
static uint16_t zone_on_terminal(int16_t gpio)
{
    for (uint16_t z = 1; z <= CONFIG_ALARM_MAX_ZONES; z++) {
        zone_hub_binding_t b;
        if (!zone_service_get(z, &b)) {
            continue;
        }
        for (size_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
            if (b.in[r].addr.kind == ZSRC_WIRED && b.in[r].addr.id == (uint64_t)gpio) {
                return z;
            }
        }
    }
    return 0;
}
#endif

#if CONFIG_ALARM_ZSRC_ZWAVE && (CONFIG_ALARM_FEATURE_MQTT || CONFIG_ALARM_FEATURE_HA)
static size_t zw_ha_list(void *ctx, void *out, size_t max)
{
    (void)ctx;
    return zwave_service_ha_devices(out, max);
}

static bool zw_ha_command(void *ctx, const char *key, int level, uint64_t received_ms)
{
    (void)ctx;
    return zwave_service_ha_command(key, level, received_ms);
}
#endif

#if CONFIG_ALARM_LINK && CONFIG_ALARM_FEATURE_HA
/* The link keypads' settings for Home Assistant (docs/features/23 HAI-016..019, docs/features/11
 * "Smart Home and Home Assistant on keypads"): ha_link's port onto plink_service, which owns the
 * keypads. ha_link calls these on its own task; plink_service ends a write from the link task. */
static size_t ha_kp_list(void *ctx, proto_link_keypad_t *out, size_t max)
{
    (void)ctx;
    return plink_service_keypads(out, max);
}

static ha_link_set_t ha_kp_error(plink_service_pset_t r)
{
    switch (r) {
    case PLINK_SERVICE_PSET_NOT_FOUND: return HA_LINK_SET_NOT_FOUND;
    case PLINK_SERVICE_PSET_OFFLINE: return HA_LINK_SET_OFFLINE;
    case PLINK_SERVICE_PSET_UNSUPPORTED: return HA_LINK_SET_UNSUPPORTED;
    case PLINK_SERVICE_PSET_TIMEOUT: return HA_LINK_SET_TIMEOUT;
    default: return HA_LINK_SET_BUSY; /* busy, or a body that couldn't go: nothing applied */
    }
}

static void ha_kp_done(void *ctx, uint32_t ticket, plink_service_pset_t result,
                       const uint8_t why[PROTO_PREFS_F_LAST])
{
    (void)ctx;
    if (result == PLINK_SERVICE_PSET_OK) {
        ha_link_settings_done(ticket, why, PROTO_PREFS_F_LAST);
    } else {
        ha_link_settings_failed(ticket, ha_kp_error(result));
    }
}

static ha_link_set_t ha_kp_set(void *ctx, uint8_t device, uint32_t ticket,
                               const proto_prefs_values_t *values)
{
    (void)ctx;
    uint8_t body[PLINK_SERVICE_PREFS_BODY_MAX];
    const size_t len = proto_prefs_encode(values, values->have, body, sizeof body);
    const plink_service_pset_t r =
        len == 0u ? PLINK_SERVICE_PSET_BAD
                  : plink_service_prefs_set(device, PLINK_SERVICE_ORIGIN_HA, body, len, ha_kp_done,
                                            NULL, ticket);
    return r == PLINK_SERVICE_PSET_OK ? HA_LINK_SET_SENT : ha_kp_error(r);
}
#endif

#if CONFIG_ALARM_ZSRC_ZIGBEE && CONFIG_ALARM_FEATURE_MQTT
/* The Zigbee network for MQTT (ZB-026, docs/features/20 devices.md §15): zigbee_service's answers
 * in net_mqtt's shapes. From the MQTT task only. */
static bool zb_mqtt_state(void *ctx, net_mqtt_zb_state_t *out)
{
    (void)ctx;
    zigbee_service_mqtt_state_t st;
    if (!zigbee_service_mqtt_state(&st)) {
        return false;
    }
    *out = (net_mqtt_zb_state_t){.radio = st.radio,
                                 .channel = st.channel,
                                 .devices = st.devices,
                                 .not_heard = st.not_heard,
                                 .jammed = st.jammed};
    return true;
}

static size_t zb_mqtt_zone_diags(void *ctx, net_mqtt_zb_diag_t *out, size_t max)
{
    (void)ctx;
    static zigbee_service_zone_diag_t diags[ALARM_MAX_ZONES] ALARM_SLOW_BSS;
    const size_t n = zigbee_service_zone_diags(diags, max < ALARM_MAX_ZONES ? max : ALARM_MAX_ZONES);
    for (size_t i = 0; i < n; i++) {
        out[i] = (net_mqtt_zb_diag_t){.zone = diags[i].zone,
                                      .battery_pct = diags[i].battery_pct,
                                      .signal = diags[i].signal};
    }
    return n;
}

static uint32_t zb_mqtt_changes(void *ctx)
{
    (void)ctx;
    return zigbee_service_changes();
}
#endif

#if CONFIG_ALARM_ZSRC_RF || CONFIG_ALARM_FEATURE_RTC || CONFIG_ALARM_ZSRC_ZWAVE
/* The first problem a description has, in words; false when it has none. */
static bool board_problem(const board_info_t *b, char *why, size_t len)
{
    board_issue_t issue;
    if (board_info_validate(b, &issue, 1) == 0) {
        return false;
    }
    snprintf(why, len, "GPIO %d (%s) %s%s%s", issue.gpio, issue.use != NULL ? issue.use : "?",
             board_issue_text(issue.code), issue.other != NULL ? ": " : "",
             issue.other != NULL ? issue.other : "");
    return true;
}

/* The board as the add-ons' stored places wire it (wired_board()'s order: the clock module, the
 * receiver, the Z-Wave radio), built one step at a time; each step's remaining terminals live in
 * a buffer of their own (at most four steps: a clock place, a receiver place or `rf pin`, a
 * Z-Wave place and its reset line). */
#define WIRING_STEPS 4
#define WIRING_TERMS 16
typedef struct {
    board_info_t b;
    board_terminal_t t[WIRING_STEPS][WIRING_TERMS];
    size_t used;
} wiring_t;

typedef enum { WIRE_NONE = 0, WIRE_OK, WIRE_REFUSED } wire_t;

/* A custom place's pins in words, wherever a place's "where" is shown (ADR-048). */
#define OWN_PINS "your own pins"

static void wiring_init(wiring_t *w)
{
    w->b = *board_info();
    w->used = 0;
}

/* The next step's buffer, never the one w->b's terminals are in while WIRING_STEPS covers every
 * step there can be (above). A step past them would write over the last one's terminals before
 * wiring_take() refused it: add a step kind, raise WIRING_STEPS. */
static board_terminal_t *wiring_terms(wiring_t *w)
{
    return w->t[w->used < WIRING_STEPS ? w->used : WIRING_STEPS - 1];
}

static bool wiring_take(wiring_t *w, const board_info_t *next)
{
    if (w->used >= WIRING_STEPS) {
        return false;
    }
    w->b = *next;
    w->used++;
    return true;
}
#endif

#if CONFIG_ALARM_ZSRC_ZWAVE
static char s_zw_place[16];              /* the Z-Wave radio's place this start, "" = off */
static int16_t s_zw_reset = BOARD_NO_PIN; /* its reset line this start */
static char s_zw_why[160];               /* why its custom place was not used (ZW-046) */
#endif
#if CONFIG_ALARM_FEATURE_RTC
static char s_clock_place[16]; /* the clock module's place this start (`rtc place`), "" = own */
static bool s_clock_off;       /* its custom place was refused: no bus at all (RTC-016) */
static char s_clock_why[160];  /* ... and why */
#endif
#if CONFIG_ALARM_ZSRC_RF
static char s_rf_place[16];  /* the receiver's place this start (`rf place`), "" = the board's own */
static bool s_rf_data_moved; /* `rf pin` moved its data line this start */
static bool s_rf_off;        /* its custom place was refused: no receiver at all (RF-025) */
static rf_module_t s_rf_module; /* on a custom place, the module its pins say */
static char s_rf_why[160];   /* why its custom place was not used */
#endif
#if CONFIG_ALARM_ZSRC_RF && CONFIG_ALARM_ZSRC_ZWAVE
static char s_pin_warning[200]; /* `rf pin` and `zwave reset-pin` on one GPIO (stored before) */
#endif

#if CONFIG_ALARM_FEATURE_RTC
/* The clock module at its stored place: named (`rtc place`, RTC-013) or custom (RTC-016). id gets
 * the stored place, why the reason it is refused. */
static wire_t wire_clock(wiring_t *w, char *id, size_t id_len, char *why, size_t len)
{
    if (!panel_clock_stored_place(id, id_len)) {
        return WIRE_NONE;
    }
    board_info_t next;
    if (strcmp(id, "custom") == 0) {
        int16_t sda;
        int16_t scl;
        if (panel_clock_stored_custom(&sda, &scl) != PANEL_CLOCK_CUSTOM_STORED) {
            snprintf(why, len, "its stored pins are missing or damaged");
            return WIRE_REFUSED;
        }
        if (!board_i2c_custom_on(&w->b, sda, scl, &next, wiring_terms(w), WIRING_TERMS, why, len)) {
            return WIRE_REFUSED;
        }
    } else if (!board_i2c_place_on(&w->b, id, &next, wiring_terms(w), WIRING_TERMS)) {
        snprintf(why, len, "this board has no place called %s", id);
        return WIRE_REFUSED;
    } else if (board_problem(&next, why, len)) {
        return WIRE_REFUSED;
    }
    return wiring_take(w, &next) ? WIRE_OK : WIRE_REFUSED;
}
#endif

#if CONFIG_ALARM_ZSRC_RF
/* A Kconfig pin, or the board's wiring when it says -1 (docs/features/33 step 4). */
static int16_t rf_pin(int configured, int16_t board_default)
{
    return configured >= 0 ? (int16_t)configured : board_default;
}

/* The receiver's SPI as Kconfig overrides it (the data line is chosen separately). */
static void rf_kconfig_spi(board_info_t *b, const board_info_t *info)
{
    b->rf.mosi = rf_pin(CONFIG_ALARM_RF_SPI_MOSI_GPIO, info->rf.mosi);
    b->rf.miso = rf_pin(CONFIG_ALARM_RF_SPI_MISO_GPIO, info->rf.miso);
    b->rf.sck = rf_pin(CONFIG_ALARM_RF_SPI_SCK_GPIO, info->rf.sck);
    b->rf.cs = rf_pin(CONFIG_ALARM_RF_SPI_CS_GPIO, info->rf.cs);
}

static board_rf_t rf_custom_pins(const rf_service_custom_t *c)
{
    return (board_rf_t){.spi_host = -1, /* the board's receiver host */
                        .sck = c->sck,
                        .mosi = c->mosi,
                        .miso = c->miso,
                        .cs = c->cs,
                        .data = c->data,
                        .where = OWN_PINS};
}

/* The receiver at its stored place: named (`rf place`, RF-019) or custom (RF-025). */
static wire_t wire_rf_place(wiring_t *w, char *id, size_t id_len, rf_service_custom_t *custom,
                            char *why, size_t len)
{
    if (!rf_service_stored_place(id, id_len)) {
        return WIRE_NONE;
    }
    board_info_t next;
    if (strcmp(id, "custom") == 0) {
        if (rf_service_stored_custom(custom) != RF_CUSTOM_STORED) {
            snprintf(why, len, "its stored pins are missing or damaged");
            return WIRE_REFUSED;
        }
        const board_rf_t pins = rf_custom_pins(custom);
        if (!board_rf_custom_on(&w->b, &pins, &next, wiring_terms(w), WIRING_TERMS, why, len)) {
            return WIRE_REFUSED;
        }
    } else if (!board_rf_place_on(&w->b, id, &next, wiring_terms(w), WIRING_TERMS)) {
        snprintf(why, len, "this board has no place called %s", id);
        return WIRE_REFUSED;
    } else if (board_problem(&next, why, len)) {
        return WIRE_REFUSED;
    }
    return wiring_take(w, &next) ? WIRE_OK : WIRE_REFUSED;
}

/* A receiver module moved onto a wired terminal with `rf pin` (RF-008), at the board's own
 * place only. *chosen gets the stored pin (-1: none). */
static wire_t wire_rf_pin(wiring_t *w, int16_t *chosen)
{
    const int16_t own = rf_pin(CONFIG_ALARM_RF_GDO0_GPIO, board_info()->rf.data);
    *chosen = rf_service_stored_pin();
    if (*chosen < 0 || *chosen == own) {
        return WIRE_NONE;
    }
    board_info_t next;
    if (!board_rf_data_on(&w->b, *chosen, &next, wiring_terms(w), WIRING_TERMS)) {
        return WIRE_REFUSED;
    }
    return wiring_take(w, &next) ? WIRE_OK : WIRE_REFUSED;
}
#endif

#if CONFIG_ALARM_ZSRC_ZWAVE
/* The Z-Wave radio at its stored place: named (`zwave place`, with `zwave reset-pin`) or custom
 * (ZW-046, with its own reset line). *reset gets the reset line used (-1: none); *reset_refused
 * says a named place's stored reset pin could not be had. */
static wire_t wire_zwave(wiring_t *w, char *id, size_t id_len, int16_t *reset, bool *reset_refused,
                         char *why, size_t len)
{
    *reset = BOARD_NO_PIN;
    *reset_refused = false;
    if (!zwave_service_stored_place(id, id_len)) {
        return WIRE_NONE;
    }
    board_info_t next;
    if (strcmp(id, "custom") == 0) {
        int16_t tx;
        int16_t rx;
        int16_t rs;
        if (zwave_service_stored_custom(&tx, &rx, &rs) != ZWAVE_CUSTOM_STORED) {
            snprintf(why, len, "its stored pins are missing or damaged");
            return WIRE_REFUSED;
        }
        if (!board_zwave_custom_on(&w->b, tx, rx, rs, &next, wiring_terms(w), WIRING_TERMS, why,
                                   len)) {
            return WIRE_REFUSED;
        }
        next.zwave.where = OWN_PINS;
        if (!wiring_take(w, &next)) {
            return WIRE_REFUSED;
        }
        *reset = rs;
        return WIRE_OK;
    }
    if (!board_zwave_place_on(&w->b, id, &next, wiring_terms(w), WIRING_TERMS)) {
        snprintf(why, len, "this board has no place called %s", id);
        return WIRE_REFUSED;
    }
    if (board_problem(&next, why, len) || !wiring_take(w, &next)) {
        return WIRE_REFUSED;
    }
    const int16_t rs = zwave_service_stored_reset_pin();
    if (rs >= 0) {
        char ignored[8];
        if (board_zwave_reset_on(&w->b, rs, &next, wiring_terms(w), WIRING_TERMS) &&
            !board_problem(&next, ignored, sizeof(ignored)) && wiring_take(w, &next)) {
            *reset = rs;
        } else {
            *reset_refused = true;
        }
    }
    return WIRE_OK;
}
#endif

#if CONFIG_ALARM_ZSRC_RF || CONFIG_ALARM_FEATURE_RTC || CONFIG_ALARM_ZSRC_ZWAVE
#define PART_CLOCK 1u
#define PART_RF 2u
#define PART_ZWAVE 4u

/* The board with the given parts at their stored places (named or custom, `rf pin` and the
 * Z-Wave reset line included), as wired_board() would build it: where another part's choice is
 * checked (ADR-048). A stored choice the board can't take is left out, as at start. */
static void with_parts(wiring_t *w, unsigned parts)
{
    char id[16];
    char why[8];
    wiring_init(w);
    (void)id;
    (void)why;
#if CONFIG_ALARM_FEATURE_RTC
    if ((parts & PART_CLOCK) != 0u) {
        (void)wire_clock(w, id, sizeof(id), why, sizeof(why));
    }
#endif
#if CONFIG_ALARM_ZSRC_RF
    if ((parts & PART_RF) != 0u) {
        rf_service_custom_t c;
        int16_t chosen;
        if (wire_rf_place(w, id, sizeof(id), &c, why, sizeof(why)) != WIRE_OK &&
            strcmp(id, "custom") != 0) {
            (void)wire_rf_pin(w, &chosen);
        }
    }
#endif
#if CONFIG_ALARM_ZSRC_ZWAVE
    if ((parts & PART_ZWAVE) != 0u) {
        int16_t reset;
        bool reset_refused;
        (void)wire_zwave(w, id, sizeof(id), &reset, &reset_refused, why, sizeof(why));
    }
#endif
}

/* "custom: your own pins on J7 (`<example>`)": the listing's line the Flasher probes for
 * (ADR-048), on a board with pins a person can reach. */
static void custom_help(const board_info_t *info, char *text, size_t size, size_t *used,
                        const char *example)
{
    if (info->reach_count == 0 || info->reach == NULL || *used >= size) {
        return;
    }
    const int n = snprintf(text + *used, size - *used, "custom: your own pins on %s (%s)\n",
                           info->reach[0].connector != NULL ? info->reach[0].connector : "the board",
                           example);
    if (n > 0 && (size_t)n < size - *used) {
        *used += (size_t)n;
    } else {
        text[*used] = '\0';
    }
}
#endif

#if CONFIG_ALARM_ZSRC_RF
/* `rf pin <gpio>` (RF-008): only a place the board offers outside (its receiver data pin or a
 * wired terminal) that the clock module's and the Z-Wave radio's stored places leave free, only
 * if the board stays safe with it, and never a terminal a zone uses. */
static bool rf_pin_ok(int16_t gpio, char *why, size_t len)
{
    const board_info_t *info = board_info();
    wiring_t w;
    with_parts(&w, PART_CLOCK | PART_ZWAVE);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    if (!board_rf_data_on(&w.b, gpio, &moved, terms, WIRING_TERMS)) {
        if (board_rf_data_on(info, gpio, &moved, terms, WIRING_TERMS)) {
            snprintf(why, len, "another part's stored place takes it (`rtc place`, `zwave place`, "
                     "`zwave reset-pin`)");
        } else {
            snprintf(why, len, "this board offers GPIO %d (%s) or a %s terminal (`terminals`)",
                     info->rf.data, info->rf.where != NULL ? info->rf.where : "its own place",
                     info->terminals_where != NULL ? info->terminals_where : "wired");
        }
        return false;
    }
    rf_kconfig_spi(&moved, info);
    if (board_problem(&moved, why, len)) {
        return false;
    }
    const uint16_t z = zone_on_terminal(gpio);
    if (z != 0) {
        snprintf(why, len, "zone %u uses it as a wired terminal: give that zone another "
                 "terminal first", (unsigned)z);
        return false;
    }
    return true;
}

/* `rf place <id>` (RF-019): a place the board offers, safe together with the clock module's
 * and the Z-Wave radio's places as stored, and none of its pins a terminal a zone uses. */
static bool rf_place_ok(const char *id, char *why, size_t len)
{
    const board_info_t *info = board_info();
    wiring_t w;
    with_parts(&w, PART_CLOCK | PART_ZWAVE);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    if (!board_rf_place_on(&w.b, id, &moved, terms, WIRING_TERMS)) {
        snprintf(why, len, "this board has no place called %s (`rf place` lists them)", id);
        return false;
    }
    if (board_problem(&moved, why, len)) {
        return false;
    }
    const board_rf_t *r = &board_rf_place(info, id)->rf;
    const int16_t pins[] = {r->sck, r->mosi, r->miso, r->cs, r->data};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        const uint16_t z = zone_on_terminal(pins[i]);
        if (z != 0) {
            snprintf(why, len, "zone %u uses GPIO %d as a wired terminal: give that zone another "
                     "terminal first", (unsigned)z, pins[i]);
            return false;
        }
    }
    return true;
}

/* `rf place custom ...` (RF-025, ADR-048): pins a person can reach, each once, none the board's
 * or held by the clock module's or the Z-Wave radio's stored place. A zone's terminal may be
 * taken: the hardware wins, and that zone shows Check after the restart (ZSRC-010). */
static bool rf_custom_ok(const rf_service_custom_t *c, char *why, size_t len)
{
    wiring_t w;
    with_parts(&w, PART_CLOCK | PART_ZWAVE);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    const board_rf_t pins = rf_custom_pins(c);
    return board_rf_custom_on(&w.b, &pins, &moved, terms, WIRING_TERMS, why, len);
}

/* One line per place for `rf place`, and the custom line on a board with reachable pins. */
static const char *rf_places_help(const board_info_t *info)
{
    static char text[320];
    size_t used = 0;
    text[0] = '\0';
    for (size_t i = 0; i < info->rf_place_count; i++) {
        const board_rf_t *r = &info->rf_places[i].rf;
        const int n = snprintf(text + used, sizeof(text) - used,
                               "%s: %s (SCK GPIO %d, MOSI %d, MISO %d, CSn %d, GDO0 %d)\n",
                               info->rf_places[i].id, r->where != NULL ? r->where : "?", r->sck,
                               r->mosi, r->miso, r->cs, r->data);
        if (n < 0 || (size_t)n >= sizeof(text) - used) {
            text[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    custom_help(info, text, sizeof(text), &used,
                "`rf place custom sck= mosi= miso= cs= gdo0=` or `rf place custom data=`");
    return text;
}
#endif

#if CONFIG_ALARM_FEATURE_RTC
/* `rtc place <id>` (RTC-013): a place the board offers, safe together with the receiver's and
 * the Z-Wave radio's places as stored, and neither of its pins a terminal a zone uses. */
static bool clock_place_ok(const char *id, char *why, size_t len)
{
    const board_info_t *info = board_info();
    wiring_t w;
    with_parts(&w, PART_RF | PART_ZWAVE);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    if (!board_i2c_place_on(&w.b, id, &moved, terms, WIRING_TERMS)) {
        snprintf(why, len, "this board has no place called %s (`rtc place` lists them)", id);
        return false;
    }
    if (board_problem(&moved, why, len)) {
        return false;
    }
    const board_i2c_place_t *place = board_i2c_place(info, id);
    const int16_t pins[] = {place->sda, place->scl};
    for (size_t i = 0; i < 2; i++) {
        const uint16_t z = zone_on_terminal(pins[i]);
        if (z != 0) {
            snprintf(why, len, "zone %u uses GPIO %d as a wired terminal: give that zone another "
                     "terminal first", (unsigned)z, pins[i]);
            return false;
        }
    }
    return true;
}

/* `rtc place custom sda= scl=` (RTC-016, ADR-048): as rf_custom_ok(), with an I2C controller of
 * its own. */
static bool clock_custom_ok(int16_t sda, int16_t scl, char *why, size_t len)
{
    wiring_t w;
    with_parts(&w, PART_RF | PART_ZWAVE);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    return board_i2c_custom_on(&w.b, sda, scl, &moved, terms, WIRING_TERMS, why, len);
}

/* One line per place for `rtc place`, and the custom line on a board with reachable pins. */
static const char *clock_places_help(const board_info_t *info)
{
    static char text[224];
    size_t used = 0;
    text[0] = '\0';
    for (size_t i = 0; i < info->i2c_place_count; i++) {
        const int n = snprintf(text + used, sizeof(text) - used, "%s: %s\n",
                               info->i2c_places[i].id,
                               info->i2c_places[i].where != NULL ? info->i2c_places[i].where : "?");
        if (n < 0 || (size_t)n >= sizeof(text) - used) {
            text[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    custom_help(info, text, sizeof(text), &used, "`rtc place custom sda= scl=`");
    return text;
}

/* The bus of a place with an I2C controller of its own (`rtc place`): nothing of the board is on
 * it, so it is opened here rather than handed out by the board. */
static i2c_master_bus_handle_t own_i2c_bus(int8_t port, int16_t sda, int16_t scl)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = (i2c_port_num_t)port,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    const esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C%d for the clock module (GPIO %d, %d): %s", (int)port, sda, scl,
                 esp_err_to_name(err));
        return NULL;
    }
    return bus;
}
#endif

#if CONFIG_ALARM_ZSRC_ZWAVE
/* `zwave place <id>` (docs/features/36 hardware.md §4, BRD-016): a place the board offers, safe
 * with the clock module and the receiver where they are stored, none of its pins a terminal a
 * zone uses. The socket is allowed: Zigbee then does not start (ADR-047). */
static bool zw_place_ok(const char *id, char *why, size_t len)
{
    const board_info_t *info = board_info();
    const board_zwave_place_t *place = board_zwave_place(info, id);
    if (place == NULL) {
        snprintf(why, len, "this board has no place called %s (`zwave place` lists them)", id);
        return false;
    }
    wiring_t w;
    with_parts(&w, PART_CLOCK | PART_RF);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    if (!board_zwave_place_on(&w.b, id, &moved, terms, WIRING_TERMS) ||
        board_problem(&moved, why, len)) {
        return false;
    }
    const int16_t pins[] = {place->tx, place->rx};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        const uint16_t z = zone_on_terminal(pins[i]);
        if (z != 0) {
            snprintf(why, len, "zone %u uses GPIO %d as a wired terminal: give that zone another "
                     "terminal first", (unsigned)z, pins[i]);
            return false;
        }
    }
    /* ADR-047: Zigbee is in every P4 image and the socket may still go to Z-Wave; then Zigbee does
     * not start (the start-up below tells the service, whose status says why). */
    return true;
}

/* `zwave reset-pin <gpio>` (hardware.md §5, BRD-017): a wired terminal no zone uses, at the place
 * stored now, that the clock module's and the receiver's stored places (and `rf pin`) leave
 * free. A custom place carries its own reset line (ZW-046). */
static bool zw_reset_ok(int16_t gpio, char *why, size_t len)
{
    char place[16];
    if (!zwave_service_stored_place(place, sizeof(place))) {
        snprintf(why, len, "choose where the radio is wired first (`zwave place`)");
        return false;
    }
    if (strcmp(place, "custom") == 0) {
        snprintf(why, len, "the radio is on your own pins: `zwave place custom tx= rx= reset=`");
        return false;
    }
    wiring_t w;
    with_parts(&w, PART_CLOCK | PART_RF);
    board_info_t at_place;
    board_info_t moved;
    board_terminal_t t1[WIRING_TERMS];
    board_terminal_t t2[WIRING_TERMS];
    if (!board_zwave_place_on(&w.b, place, &at_place, t1, WIRING_TERMS)) {
        snprintf(why, len, "its stored place %s is not this board's", place);
        return false;
    }
    if (!board_zwave_reset_on(&at_place, gpio, &moved, t2, WIRING_TERMS)) {
        snprintf(why, len, "only a free wired terminal can take it (`terminals`)");
        return false;
    }
    if (board_problem(&moved, why, len)) {
        return false;
    }
    const uint16_t z = zone_on_terminal(gpio);
    if (z != 0) {
        snprintf(why, len, "zone %u uses it as a wired terminal: give that zone another terminal "
                 "first", (unsigned)z);
        return false;
    }
    return true;
}

/* `zwave place custom tx= rx= [reset=]` (ZW-046, ADR-048): as rf_custom_ok(), against the clock
 * module's and the receiver's stored places (and `rf pin`). */
static bool zw_custom_ok(int16_t tx, int16_t rx, int16_t reset, char *why, size_t len)
{
    wiring_t w;
    with_parts(&w, PART_CLOCK | PART_RF);
    board_info_t moved;
    board_terminal_t terms[WIRING_TERMS];
    return board_zwave_custom_on(&w.b, tx, rx, reset, &moved, terms, WIRING_TERMS, why, len);
}

/* One line per place for `zwave place`, and the custom line on a board with reachable pins. */
static const char *zw_places_help(const board_info_t *info)
{
    static char text[288];
    size_t used = 0;
    text[0] = '\0';
    for (size_t i = 0; i < info->zwave_place_count; i++) {
        const board_zwave_place_t *p = &info->zwave_places[i];
        const int n = snprintf(text + used, sizeof(text) - used, "%s: %s (TX GPIO %d, RX %d)\n",
                               p->id, p->where != NULL ? p->where : "?", p->tx, p->rx);
        if (n < 0 || (size_t)n >= sizeof(text) - used) {
            text[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    custom_help(info, text, sizeof(text), &used, "`zwave place custom tx= rx= [reset=]`");
    return text;
}
#endif

/* The board as wired this start: the clock module's place (`rtc place`), then the receiver's
 * (`rf place`, or its data line on a terminal, `rf pin`), then the Z-Wave radio's (`zwave place`,
 * `zwave reset-pin`). A stored named place the board can't take safely is logged and left out:
 * the board's own places are always safe. A stored custom place it can't take turns that part
 * off, never back to its default pins (ADR-048). Built once, before the clock module is looked
 * for, and shared by it, the zone terminals and the receiver. */
static const board_info_t *wired_board(void)
{
    static bool built;
#if CONFIG_ALARM_ZSRC_RF || CONFIG_ALARM_FEATURE_RTC || CONFIG_ALARM_ZSRC_ZWAVE
    static wiring_t w;
    if (built) {
        return &w.b;
    }
    built = true;
    const board_info_t *info = board_info();
    wiring_init(&w);
    char why[120] = "";
    (void)info;
#else
    built = true;
    return board_info();
#endif
#if CONFIG_ALARM_FEATURE_RTC
    const wire_t clock = wire_clock(&w, s_clock_place, sizeof(s_clock_place), why, sizeof(why));
    if (clock == WIRE_OK) {
        ESP_LOGI(TAG, "clock module looked for on the %s (`rtc place %s`)", w.b.i2c.where,
                 s_clock_place);
    } else if (clock == WIRE_REFUSED && strcmp(s_clock_place, "custom") == 0) {
        /* RTC-016: failing closed - no bus, the module is not looked for anywhere */
        s_clock_off = true;
        snprintf(s_clock_why, sizeof(s_clock_why), "custom place not used: %s", why);
        ESP_LOGE(TAG, "the clock module stays off: %s (`rtc place default` forgets it)",
                 s_clock_why);
    } else if (clock == WIRE_REFUSED) {
        ESP_LOGE(TAG, "`rtc place %s` is not a safe place on this board (%s): the %s instead",
                 s_clock_place, why, info->i2c.where != NULL ? info->i2c.where : "board's own");
        s_clock_place[0] = '\0';
    }
#endif
#if CONFIG_ALARM_ZSRC_RF
    rf_service_custom_t rf_custom;
    int16_t rf_chosen = BOARD_NO_PIN;
    const wire_t rf = wire_rf_place(&w, s_rf_place, sizeof(s_rf_place), &rf_custom, why,
                                    sizeof(why));
    const bool rf_is_custom = strcmp(s_rf_place, "custom") == 0;
    if (rf == WIRE_OK && rf_is_custom) {
        s_rf_module = rf_custom.cc1101 ? RF_MODULE_CC1101 : RF_MODULE_OOK;
        ESP_LOGI(TAG, "433 MHz receiver (%s) on your own pins (`rf place custom`): they are not "
                      "offered as wired terminals", rf_custom.cc1101 ? "CC1101" : "receiver module");
    } else if (rf == WIRE_OK) {
        ESP_LOGI(TAG, "433 MHz receiver on the %s (`rf place %s`): its pins are not offered "
                      "as wired terminals", w.b.rf.where, s_rf_place);
    } else if (rf == WIRE_REFUSED && rf_is_custom) {
        /* RF-025: failing closed - the receiver stays off, its default pins untouched */
        s_rf_off = true;
        snprintf(s_rf_why, sizeof(s_rf_why), "custom place not used: %s", why);
        ESP_LOGE(TAG, "the 433 MHz receiver stays off: %s (`rf place default` forgets it)",
                 s_rf_why);
    } else {
        if (rf == WIRE_REFUSED) {
            ESP_LOGE(TAG, "`rf place %s` is not a safe place on this board (%s): the receiver "
                          "stays at its own place (`rf place default` forgets it)", s_rf_place,
                     why);
            s_rf_place[0] = '\0';
        }
        const wire_t pin = wire_rf_pin(&w, &rf_chosen);
        if (pin == WIRE_OK) {
            s_rf_data_moved = true;
            ESP_LOGI(TAG, "433 MHz receiver data on GPIO %d (`rf pin`): not offered as a "
                          "wired terminal", rf_chosen);
        } else if (pin == WIRE_REFUSED) {
            ESP_LOGE(TAG, "`rf pin` %d is not a place this board offers: the receiver stays "
                          "on GPIO %d (`rf pin default` forgets it)", rf_chosen,
                     rf_pin(CONFIG_ALARM_RF_GDO0_GPIO, info->rf.data));
        }
    }
#endif
#if CONFIG_ALARM_ZSRC_ZWAVE
    /* The Z-Wave radio (docs/features/36 hardware.md §4): off unless `zwave place` chose one; its
     * pins, and its reset line's terminal, are gone before the terminals go to the zones. */
    int16_t zw_reset = BOARD_NO_PIN;
    bool zw_reset_refused = false;
    const wire_t zw = wire_zwave(&w, s_zw_place, sizeof(s_zw_place), &zw_reset, &zw_reset_refused,
                                 why, sizeof(why));
    const bool zw_is_custom = strcmp(s_zw_place, "custom") == 0;
    if (zw == WIRE_OK) {
        s_zw_reset = zw_reset;
        ESP_LOGI(TAG, "Z-Wave radio on the %s (`zwave place %s`), reset line %d", w.b.zwave.where,
                 s_zw_place, (int)zw_reset);
        if (zw_reset_refused) {
            ESP_LOGE(TAG, "`zwave reset-pin %d` is not a free wired terminal here: the radio runs "
                          "without its reset line", (int)zwave_service_stored_reset_pin());
        }
    } else if (zw == WIRE_REFUSED && zw_is_custom) {
        /* ZW-046: failing closed - the radio stays off, no pin touched */
        snprintf(s_zw_why, sizeof(s_zw_why), "custom place not used: %s", why);
        ESP_LOGE(TAG, "the Z-Wave radio stays off: %s (`zwave place off` forgets it)", s_zw_why);
        s_zw_place[0] = '\0';
    } else if (zw == WIRE_REFUSED) {
        ESP_LOGE(TAG, "`zwave place %s` is not a safe place on this board (%s): the Z-Wave radio "
                      "stays off (`zwave place off` forgets it)", s_zw_place, why);
        s_zw_place[0] = '\0';
    }
#endif
#if CONFIG_ALARM_ZSRC_RF && CONFIG_ALARM_ZSRC_ZWAVE
    /* `rf pin` and `zwave reset-pin` on one GPIO (stored by firmware before the checks knew of
     * each other): today's result is kept - the receiver has it, the radio runs without its
     * reset line - and said in the log and in both status replies. */
    if (s_rf_data_moved && zw == WIRE_OK && zw_reset_refused &&
        zwave_service_stored_reset_pin() == rf_chosen) {
        snprintf(s_pin_warning, sizeof(s_pin_warning),
                 "warning: GPIO %d is both the receiver's data line (`rf pin`) and the Z-Wave "
                 "radio's reset line (`zwave reset-pin`): the receiver has it, the radio runs "
                 "without its reset line", (int)rf_chosen);
        ESP_LOGW(TAG, "%s", s_pin_warning);
    }
#endif
#if CONFIG_ALARM_ZSRC_RF || CONFIG_ALARM_FEATURE_RTC || CONFIG_ALARM_ZSRC_ZWAVE
    return &w.b;
#endif
}

/* The add-on parts this start runs, for the update server's checks (OTA-018): "zac93" when the
 * Z-Wave radio started at a place, "cc1101" when the 433 MHz receiver started on a module that can
 * be a CC1101 (not one configured as a fixed OOK receiver). Set by start_zone_sources(). */
static const char *s_ota_parts[2];
static size_t s_ota_parts_n;

#if CONFIG_ALARM_ZSRC_ZWAVE || CONFIG_ALARM_ZSRC_RF
static void ota_part(const char *name)
{
    if (s_ota_parts_n < sizeof(s_ota_parts) / sizeof(s_ota_parts[0])) {
        s_ota_parts[s_ota_parts_n++] = name;
    }
}
#endif

static bool start_zone_sources(void)
{
    const board_info_t *info = board_info();
    /* The board as wired (wired_board): the terminals a receiver or a clock module takes at a
     * place chosen at runtime are gone before the terminals are handed to the zone sources, so
     * the zone editor never offers them. */
    const board_info_t *wiring = wired_board();
#if CONFIG_ALARM_ZSRC_RF
    const rf_service_pins_t rules = {.board_data = rf_pin(CONFIG_ALARM_RF_GDO0_GPIO, info->rf.data),
                                     .board_where = info->rf.where,
                                     .pin_ok = rf_pin_ok,
                                     .places = rf_places_help(info),
                                     .place_ok = rf_place_ok,
                                     .custom_ok = rf_custom_ok,
                                     .custom_why = s_rf_why,
#if CONFIG_ALARM_ZSRC_ZWAVE
                                     .warning = s_pin_warning,
#endif
    };
    rf_service_pin_rules(&rules);
#endif
    static zsrc_wired_terminal_t terminals[16];
    size_t count = 0;
    for (size_t i = 0; i < wiring->terminal_count && count < sizeof(terminals) / sizeof(terminals[0]);
         i++) {
        terminals[count].gpio = (uint8_t)wiring->terminals[i].gpio;
        terminals[count].where = wiring->terminals_where;
        count++;
    }
    const zone_service_config_t cfg = {.terminals = terminals, .terminal_count = count};
#if ALARM_BENCH_FAULT_NO_ZONE_SERVICE
    /* Bench fault image (cmake/alarm_firmware.cmake): an update that breaks the sensor input
     * must fail its self-test and bring the previous image back. */
    (void)cfg;
    ESP_LOGE(TAG, "BENCH FAULT IMAGE: the zone sources are not started on purpose");
    return false;
#endif
    const esp_err_t err = zone_service_start(&cfg);
    if (err != ESP_OK) {
        /* Not swallowed any more (R18): the house has no sensor input at all, so the health
         * check below raises the trouble and a firmware update that caused it rolls back. */
        ESP_LOGE(TAG, "zone sources failed to start (%s): zones get no sensor input",
                 esp_err_to_name(err));
        return false;
    }
#if CONFIG_ALARM_ZSRC_ZIGBEE
    /* The sensor radio joins the hub as one more source (docs/features/20). An empty wireless socket
     * is not a reason to fail the start: the panel's wired and simulator zones are untouched by
     * it, and the radio's own state is reported through the console and Settings. */
    const bool zwave_has_socket = wiring->radio.tx == BOARD_NO_PIN && info->radio.tx != BOARD_NO_PIN;
    zigbee_service_set_socket_taken(zwave_has_socket);
    if (wiring->radio.tx == BOARD_NO_PIN) {
        ESP_LOGW(TAG, "no sensor-radio UART on this board, or the Z-Wave radio has it: no Zigbee");
    } else {
        const zigbee_service_config_t zb = {.uart_tx_gpio = (uint8_t)wiring->radio.tx,
                                            .uart_rx_gpio = (uint8_t)wiring->radio.rx};
        const esp_err_t zerr = zigbee_service_start(&zb);
        if (zerr != ESP_OK) {
            ESP_LOGE(TAG, "the sensor radio failed to start (%s): Zigbee sensors are unavailable",
                     esp_err_to_name(zerr));
        }
    }
#endif
#if CONFIG_ALARM_ZSRC_ZWAVE
    /* The Z-Wave radio (docs/features/36) joins the hub the same way. Its source is registered
     * with no place chosen too (zones bound to Z-Wave devices keep their bindings); then no pin
     * is touched. */
    const zwave_service_rules_t zw_rules = {
        .places = zw_places_help(info),
        .place_ok = zw_place_ok,
        .reset_ok = zw_reset_ok,
        .custom_ok = zw_custom_ok,
        .custom_why = s_zw_why,
#if CONFIG_ALARM_ZSRC_RF
        .warning = s_pin_warning,
#endif
    };
    zwave_service_rules(&zw_rules);
    const zwave_service_config_t zw = {.place = s_zw_place[0] != '\0' ? s_zw_place : NULL,
                                       .tx = wiring->zwave.tx,
                                       .rx = wiring->zwave.rx,
                                       .reset = s_zw_reset,
                                       .where = wiring->zwave.where};
    const esp_err_t zwerr = zwave_service_start(&zw);
    if (zwerr != ESP_OK) {
        ESP_LOGE(TAG, "the Z-Wave radio failed to start (%s): Z-Wave sensors are unavailable",
                 esp_err_to_name(zwerr));
    } else if (zw.place != NULL) {
        ota_part("zac93");
    }
#endif
#if CONFIG_ALARM_ZSRC_RF
    /* The 433 MHz receiver (docs/features/25) joins the hub the same way; an empty bus is not a
     * failure either, the service keeps asking for the module. The board's receiver wiring
     * unless Kconfig names other pins, and those checked against the board before anything is
     * configured: a pin on flash, a strapping pin or one the board already uses leaves the
     * receiver off instead of harming the panel. */
    if (s_rf_off) {
        /* RF-025: a refused custom place leaves the receiver off, never on its default pins */
        ESP_LOGE(TAG, "the 433 MHz receiver stays off: %s", s_rf_why);
        return true;
    }
    const bool rf_custom = strcmp(s_rf_place, "custom") == 0;
    board_info_t wired = *wiring;
    if (s_rf_place[0] == '\0') {
        /* Kconfig pins name the board's own place only; a place chosen at runtime is used as
         * the board describes it. */
        rf_kconfig_spi(&wired, info);
        wired.rf.data = s_rf_data_moved ? wiring->rf.data : rules.board_data;
    }
    if (wired.rf.spi_host < 2 || wired.rf.data == BOARD_NO_PIN ||
        !board_pins_safe(&wired, "433 MHz receiver")) {
        ESP_LOGE(TAG, "the 433 MHz receiver stays off: no safe wiring on this board");
        return true;
    }
    const rf_service_config_t rf = {.host = (spi_host_device_t)(SPI2_HOST + (wired.rf.spi_host - 2)),
                                    .mosi = wired.rf.mosi,
                                    .miso = wired.rf.miso,
                                    .sck = wired.rf.sck,
                                    .cs = wired.rf.cs,
                                    .gdo0 = wired.rf.data,
#if CONFIG_ALARM_RF_INVERT
                                    .invert = true,
#else
                                    .invert = false,
#endif
                                    .freq_khz = CONFIG_ALARM_RF_FREQ_KHZ,
#if CONFIG_ALARM_RF_MODULE_CC1101
                                    .module = rf_custom ? s_rf_module : RF_MODULE_CC1101,
#elif CONFIG_ALARM_RF_MODULE_OOK
                                    .module = rf_custom ? s_rf_module : RF_MODULE_OOK,
#else
                                    .module = rf_custom ? s_rf_module : RF_MODULE_AUTO,
#endif
                                    .place = s_rf_place[0] != '\0' ? s_rf_place : NULL,
                                    .where = wired.rf.where,
                                    .custom = rf_custom,
    };
    const esp_err_t rerr = rf_service_start(&rf);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "the 433 MHz receiver failed to start (%s): those sensors are unavailable",
                 esp_err_to_name(rerr));
    } else if (rf.module != RF_MODULE_OOK && rf.module != RF_MODULE_OFF) {
        ota_part("cc1101");
    }
#endif
    return true;
}

/* ---- health: every part of the panel keeps saying it is working (OTA-005, R18) -------- */

/* panel_ota_healthy() is a heartbeat, not a one-off. A part that reports once and stops is
 * treated as stopped: an update that broke it rolls back, and outside an update the panel says
 * so instead of looking as if it were protecting the house. Nothing here may need a person -
 * the panel is verified at three in the morning with nobody in the room.
 *
 *   engine   the alarm task answers a call posted from the health task (it can only answer
 *            while it is running its own queue)
 *   log      the event log's writer task turns at least once a second (panel_log_passes)
 *   sensors  the zone hub's task ticks its sources (zone_service_passes)
 *   ui       an LVGL timer, i.e. a completed lv_timer_handler() pass
 *   touch    a read of the touch controller it answered, finger or no finger
 */
static void beat_engine(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)ctx;
    /* Cached source only: the alarm task never waits on the battery's bus. */
    const power_source_t source = {.read = board_battery_read};
    power_monitor_poll(&s_power, &source, now);
    if (s_power.status.ac_present != POWER_UNKNOWN) {
        alarm_engine_system_trouble(e, ALARM_TRBL_AC_LOSS,
                                    s_power.status.ac_present == POWER_NO, now);
    }
    if (s_power.status.battery_low != POWER_UNKNOWN) {
        alarm_engine_system_trouble(e, ALARM_TRBL_LOW_BATTERY,
                                    s_power.status.battery_low == POWER_YES, now);
    }
    panel_ota_healthy(PANEL_OTA_OK_ENGINE); /* the alarm task dequeued this and ran it */
}

#if CONFIG_ALARM_FEATURE_UI
static uint32_t s_touch_good; /* touch polls that came back: polls minus bus errors */

static void ui_heartbeat(lv_timer_t *timer)
{
    (void)timer;
    panel_ota_healthy(PANEL_OTA_OK_UI); /* this callback is an lv_timer_handler() pass */
    board_touch_stats_t t;
    board_touch_stats(&t, false);
    const uint32_t good = t.polls - t.errors;
    if (good != s_touch_good) {
        s_touch_good = good;
        panel_ota_healthy(PANEL_OTA_OK_TOUCH);
    }
}
#endif

static void health_task(void *arg)
{
    (void)arg;
    uint32_t log_passes = panel_log_passes();
    uint32_t zone_passes = zone_service_passes();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_TICK_MS));

        const uint32_t lp = panel_log_passes();
        if (lp != log_passes) {
            log_passes = lp;
            panel_ota_healthy(PANEL_OTA_OK_LOG);
        }
        const uint32_t zp = zone_service_passes();
        if (zp != zone_passes) {
            zone_passes = zp;
            panel_ota_healthy(PANEL_OTA_OK_SENSORS);
        }
        /* Posted, not called: this task must never wait for the engine, and an alarm task that
         * stopped answering is exactly what the missing heartbeat is meant to report. */
        board_battery_poll(); /* bounded bus access outside the alarm and UI tasks */
        alarm_service_post(beat_engine, NULL);

        /* A device command whose reply never came (HCTL-012, docs/features/24). */
        device_service_tick(alarm_service_now());

        /* panel_ota watches the same heartbeats all the time, not only on probation, and logs
         * what has gone quiet. Sensor input is the part that decides whether the panel is
         * protecting the house, so it becomes the same trouble a lost source link raises. */
        zone_service_report_health((panel_ota_health_stale() & PANEL_OTA_OK_SENSORS) == 0u);
    }
}

/* ---- start-up ----------------------------------------------------------------------- */

/* R06: ESP_ERR_NVS_NO_FREE_PAGES and ESP_ERR_NVS_NEW_VERSION_FOUND used to be treated as
 * permission to erase the whole default partition - the codes, users, zone bindings, Wi-Fi and
 * preferences of a commissioned alarm - without anyone asking for it. Neither error means the
 * data is gone: a full partition needs compaction, and a newer format means this firmware is
 * older than the one that wrote it (an update that was rolled back). Both are recoverable with
 * everything intact, so nothing here erases anything. The panel comes up far enough to say why
 * and to take "factory_reset confirm" from the service console, which is the owner's explicit,
 * described-in-advance way to erase. */
static esp_err_t init_nvs(void)
{
    const esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "stored settings could not be opened (%s). Nothing has been erased: the "
                      "codes, zones and network settings are still on flash.",
                 esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", err == ESP_ERR_NVS_NEW_VERSION_FOUND
                                ? "They were written by a newer firmware; install it again to "
                                  "read them."
                                : "The settings area is full and cannot be compacted.");
    }
    return err;
}

typedef struct {
    bool display; /* the panel draws */
    bool touch;   /* and it can be operated */
} display_state_t;

static display_state_t start_display(bool keypad)
{
    display_state_t st = {false, false};
#if CONFIG_ALARM_FEATURE_UI
    lv_display_t *disp = NULL;
    esp_err_t err = board_backlight_init();
    if (err == ESP_OK) {
        err = board_display_init(&disp);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display failed (%s): running headless", esp_err_to_name(err));
        return st;
    }
    /* R18: this used to be logged and forgotten, and start_display() still said "yes". A panel
     * whose screen works but cannot be touched cannot be disarmed by whoever walks up to it. */
    if (board_touch_init(disp, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "touch failed: the screen shows but nobody can operate the panel on it");
    } else {
        st.touch = true;
    }
    ui_backend_t backend;
    ui_port_esp_backend(&backend);
    if (!keypad && s_cfg->extend_backend != NULL) {
        s_cfg->extend_backend(&backend); /* the Panel Manager: a master's */
    }
#if CONFIG_ALARM_LINK
    /* The link's entries; on a keypad also the master's status, commands and codes through the
     * link instead of the engine (docs/features/11 "Roles on a P4 panel"). */
    plink_service_extend_backend(&backend);
#endif
    backend.backlight = ui_backlight;
    backend.power_info = ui_power;
    ui_options_t opt;
    ui_port_esp_options(&opt); /* stored preferences; the setup wizard on first boot */
#if CONFIG_ALARM_LINK
    if (keypad) {
        opt.partition = plink_service_keypad_home(); /* the partition it shows and arms */
    }
#endif
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_ALARM_UI_LVGL_BUDGET_KB > 0
    /* A board without PSRAM builds the UI's LVGL objects from the heap it shares with Wi-Fi and
     * TLS, which start after this. Running out halfway would leave LVGL stuck and the panel
     * restarting into the same failure, so the UI starts only with room for its budget (the
     * manifest's CONFIG_ALARM_UI_LVGL_BUDGET_KB, which the PC preview checks the UI against)
     * and for the network behind it. Otherwise the status screen shows the alarm state. */
    const size_t need = ((size_t)CONFIG_ALARM_UI_LVGL_BUDGET_KB + UI_NET_RESERVE_KB) * 1024u;
    if (free_before < need) {
        ESP_LOGE(TAG, "touch UI not started: %u KB of internal RAM free, it needs %u KB (budget "
                      "%u KB + %u KB for the network); showing the status screen",
                 (unsigned)(free_before / 1024u), (unsigned)(need / 1024u),
                 (unsigned)CONFIG_ALARM_UI_LVGL_BUDGET_KB, (unsigned)UI_NET_RESERVE_KB);
        if (!board_ui_lock(0)) {
            return st;
        }
        const ui_status_options_t sopt = {
            .board = board_name(), .partition = KEYPAD_PARTITION, .brightness = opt.brightness};
        st.display = ui_status_start(&backend, &sopt); /* backlight after its first frame */
        if (st.display) {
            lv_timer_create(ui_heartbeat, HEALTH_TICK_MS, NULL);
        }
        board_ui_unlock();
        ESP_LOGI(TAG, "status screen %s: internal RAM free %u KB",
                 st.display ? "started" : "failed",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
        return st;
    }
#endif
    if (!board_ui_lock(0)) {
        return st;
    }
    ui_app_start(&backend, &opt); /* backlight comes on after the first frame */
    lv_timer_create(ui_heartbeat, HEALTH_TICK_MS, NULL); /* runs on the LVGL task */
    board_ui_unlock();
    ESP_LOGI(TAG, "touch UI started: internal RAM free %u KB before, %u KB after",
             (unsigned)(free_before / 1024u),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
    st.display = true;
#else
    (void)keypad;
    ESP_LOGW(TAG, "this firmware has no touch UI (ALARM_FEATURE_UI off): running headless");
#endif
    return st;
}

/* Start-up could not finish. Nothing is erased and nothing is rewritten - whatever is stored is
 * left exactly as it is for the next boot to read. The console is brought up so the fault can
 * be read and an explicit factory reset is possible, and then the panel restarts: a transient
 * cause gets another go, and a firmware update still on probation is rolled back by the
 * bootloader because nothing ever confirmed it. */
static void recovery_park(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s (%s): THE PANEL IS NOT PROTECTING THE HOUSE", what, esp_err_to_name(err));
#if CONFIG_ALARM_SERVICE_CONSOLE
    const esp_err_t cerr = panel_console_start();
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "service console unavailable too (%s)", esp_err_to_name(cerr));
    }
#endif
    for (uint32_t left = RECOVERY_WINDOW_MS; left > 0u; left -= 15000u) {
        ESP_LOGE(TAG, "%s. \"factory_reset confirm\" erases everything and starts over; "
                      "restarting in %" PRIu32 " s",
                 what, left / 1000u);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    esp_restart();
}

static void log_chip(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "%s silicon v%d.%d", CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100);
}

/* The board's audio and the sounder on it (both roles). */
static void start_audio(void)
{
    const uint32_t rate = board_info()->audio_rate_hz != 0u ? board_info()->audio_rate_hz
                                                            : SND_SAMPLE_RATE;
    if (board_audio_init(rate) == ESP_OK) {
        const sounder_sink_t sink = {
            .sample_rate_hz = rate, .ctx = NULL, .write = audio_write, .amp = audio_amp};
        ESP_ERROR_CHECK(sounder_start(&sink));
    } else {
        ESP_LOGE(TAG, "audio failed: no keypad sounds");
    }
}

/* The stored preferences and the three volumes (both roles). */
static void load_prefs(panel_prefs_t *prefs)
{
    panel_prefs_load(prefs);
    sounder_set_volume(prefs->volume); /* Beeps */
    sounder_set_chime_volume(prefs->chime_volume); /* AUD-032: each on its own */
    sounder_set_voice_volume(prefs->voice_volume);
}

/* This panel's own Announcements switches and Voice volume to its voice (both roles: a keypad's
 * are its own, D06). */
static void voice_prefs_apply(const panel_prefs_t *prefs)
{
    const panel_voice_prefs_t vp = {.enabled = prefs->voice_enabled,
                                    .status = prefs->voice_status,
                                    .chime = prefs->voice_chime,
                                    .volume = prefs->voice_volume}; /* AUD-032 */
    panel_voice_set_prefs(&vp);
}

#if CONFIG_ALARM_LINK && CONFIG_ALARM_FEATURE_UI
/* ---- the keypad role (docs/features/11 "Roles on a P4 panel") ------------------------------ */

/* A keypad's health: no engine keeps the battery's filter here, so this task does, and hands the
 * mains and battery to the link's heartbeat. The screen and touch beat from the LVGL task
 * (ui_heartbeat), the link from the link task. */
static void keypad_health_task(void *arg)
{
    (void)arg;
    const power_source_t source = {.read = board_battery_read};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_TICK_MS));
        board_battery_poll(); /* bounded bus access outside the UI task */
        power_monitor_poll(&s_power, &source, alarm_service_now());
        portENTER_CRITICAL(&s_power_mux);
        s_power_seen = s_power.status;
        portEXIT_CRITICAL(&s_power_mux);
        plink_service_keypad_power(s_power.status.ac_present == POWER_NO,
                                   s_power.status.battery_low == POWER_YES,
                                   s_power.status.battery_present == POWER_YES);
    }
}

#if CONFIG_ALARM_FEATURE_VOICE
/* The keypad's voice (LINK-028): no engine to ask, so its source is the link - a baseline is
 * sent by the link task, the zones' words come from the master's configuration copy, and a pack
 * install always waits (no install path reaches a keypad yet: BL-205 gives it a real gate). */
static bool kp_voice_baseline(void *ctx)
{
    (void)ctx;
    plink_service_voice_resync();
    return true;
}

static bool kp_voice_config(void *ctx, alarm_config_t *out, uint32_t *config_seq)
{
    (void)ctx;
    return plink_service_keypad_config(out, config_seq);
}

static bool kp_voice_locked(void *ctx)
{
    (void)ctx;
    return true;
}
#endif

/* A keypad for a master elsewhere: the board, the sounder, the languages and packs, its voice, the
 * screen with the link's backend, Wi-Fi, updates (gated by the link) and the console - and nothing
 * that runs the alarm: no engine, event log, zone sources, MQTT, notifications, Home Assistant,
 * devices, automations or Panel Manager (LINK-020). */
static void run_keypad(const panel_app_config_t *cfg)
{
    s_keypad = true;
    alarm_service_set_dormant(); /* first: nothing may queue for an engine that never runs */
    ESP_LOGI(TAG, "this panel is a keypad of the panel link (docs/features/11)");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "board: %s (%s)", board_name(), board_info()->id);
    (void)board_pins_safe(board_info(), "board description");
    ESP_ERROR_CHECK(board_power_init());
    start_audio();
    panel_prefs_t prefs;
    load_prefs(&prefs);

    /* Updates: only while the link is up and the master allows it (the gate), and the self-test
     * needs the screen, touch and the link (keypad_health_mask). */
    panel_ota_set_device(board_info()->id);
    panel_ota_set_facts("", NULL, 0);
    panel_ota_set_idle_gate(plink_service_update_idle, NULL);
    START_OR_LOG(panel_ota_start(cfg->keypad_health_mask));
    START_OR_LOG(lang_packs_start());
    START_OR_LOG(asset_packs_start());
#if CONFIG_ALARM_FEATURE_VOICE
    /* Its voice (LINK-028), with its own Announcements switches and Voice volume (D06), fed by
     * the link task from what the master sends: started before the link, so the first ONLINE
     * finds it. */
    {
        static const panel_voice_source_t source = {.baseline = kp_voice_baseline,
                                                    .config = kp_voice_config,
                                                    .install_locked = kp_voice_locked,
                                                    .ctx = NULL};
        panel_voice_set_source(&source);
        START_OR_LOG(panel_voice_start());
        voice_prefs_apply(&prefs);
        static const plink_service_voice_t voice = {.baseline = panel_voice_engine_baseline,
                                                    .pause = panel_voice_engine_pause,
                                                    .event = panel_voice_engine_event,
                                                    .sound = panel_voice_engine_sound,
                                                    .output = panel_voice_engine_output,
                                                    .snapshot = panel_voice_engine_snapshot};
        plink_service_set_voice(&voice);
    }
#endif

    /* The link before the screen, so the UI's first look at link_info finds a keypad (its setup's
     * pairing step, Home's Connecting), and before the network: a paired keypad runs no time
     * server of its own. */
    if (plink_service_start(board_name()) != ESP_OK) {
        ESP_LOGE(TAG, "panel link failed to start: this keypad shows nothing of its alarm panel");
    }
    const display_state_t disp = start_display(true);
    if (!disp.display || !disp.touch) {
        ESP_LOGE(TAG, "start-up incomplete:%s%s - this keypad can't be used",
                 disp.display ? "" : " no picture", disp.touch ? "" : " no touch");
    }
    if (alarm_mem_task_create(keypad_health_task, "health", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "health task failed to start: an update could not confirm itself");
    }
    START_OR_LOG(net_wifi_start());
    START_OR_LOG(net_check_init());
#if CONFIG_ALARM_SERVICE_CONSOLE
    START_OR_LOG(panel_console_start());
    START_OR_LOG(plink_service_console_register());
    START_OR_LOG(panel_ota_console_register());
    START_OR_LOG(lang_packs_console_register());
    START_OR_LOG(panel_voice_console_register()); /* `voice`: the counters a muted bench reads */
    START_OR_LOG(app_console_register());
    if (cfg->console != NULL) {
        START_OR_LOG(cfg->console());
    }
#endif
    /* cfg->started is left out: it starts the Panel Manager and developer access, a master's. */
    ESP_LOGI(TAG, "%s running as a keypad: internal RAM free %u KB (largest block %u KB)",
             board_name(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) /
                        1024u));
}
#endif

void panel_app_run(const panel_app_config_t *cfg)
{
    s_cfg = cfg;
    power_monitor_init(&s_power);
    if (cfg->log_chip != NULL) {
        cfg->log_chip();
    } else {
        log_chip();
    }
    const esp_err_t nvs_err = init_nvs();
    if (nvs_err != ESP_OK) {
        /* Without stored settings the engine has nothing to run on, and guessing at factory
         * defaults would overwrite the installation that is still there (R05/R06). */
        recovery_park("the panel's stored settings could not be opened", nvs_err);
        return;
    }
#if CONFIG_ALARM_LINK
    /* The role, right after NVS: a keypad starts none of the alarm (docs/features/11). */
    if (plink_service_role() == PLINK_SERVICE_KEYPAD) {
#if CONFIG_ALARM_FEATURE_UI
        if (cfg->keypad_health_mask != 0u) {
            run_keypad(cfg);
            return;
        }
#endif
        ESP_LOGE(TAG, "stored as a keypad, but this board can't be one: running as the alarm "
                      "panel");
        plink_service_force_master(); /* the link and the UI follow the role this start runs */
    }
#endif
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    /* A description with a problem is a firmware bug (its host test should have caught it);
     * say so loudly and carry on, the alarm matters more than the report. */
    ESP_LOGI(TAG, "board: %s (%s)", board_name(), board_info()->id);
    (void)board_pins_safe(board_info(), "board description");
    ESP_ERROR_CHECK(board_power_init());

    /* The battery-backed clock module, when one is fitted (docs/features/35): before the event log,
     * so the first event of this start-up already carries the time. Without one this only
     * looks, and changes nothing. */
#if CONFIG_ALARM_FEATURE_RTC
    const board_i2c_t *own_i2c = &board_info()->i2c;
    const board_i2c_t *i2c = &wired_board()->i2c; /* at its place (`rtc place`, RTC-013) */
    const panel_clock_place_rules_t clock_rules = {
        .own_where = own_i2c->sda != BOARD_NO_PIN && own_i2c->where != NULL ? own_i2c->where : "",
        .places = clock_places_help(board_info()),
        .place_ok = clock_place_ok,
        .custom_ok = clock_custom_ok,
        .custom_why = s_clock_why};
    panel_clock_place_rules(&clock_rules);
    i2c_master_bus_handle_t i2c_bus = NULL;
    if (s_clock_off) {
        /* RTC-016: a refused custom place - no bus, the board's own connector left alone */
    } else if (s_clock_place[0] != '\0') {
        /* a place's own controller (named, RTC-013, or custom, RTC-016): the wired board's port */
        i2c_bus = own_i2c_bus(i2c->port, i2c->sda, i2c->scl);
    } else if (i2c->sda != BOARD_NO_PIN) {
        i2c_bus = board_i2c_ext();
    }
    START_OR_LOG(panel_clock_start(i2c_bus, s_clock_off ? OWN_PINS : i2c->where,
                                   s_clock_place[0] != '\0' ? s_clock_place : NULL));
#endif

    start_audio();

    panel_prefs_t prefs;
    load_prefs(&prefs);
    /* docs/features/41 D30: the automations' names for the event words and the JSON, before
     * any task that reads them starts (Activity, notifications, MQTT, the Panel Manager). */
    auto_names_register(prefs.clock_mode, prefs.region, prefs.lang);

    /* The event log first: the engine's event numbers continue after its newest record. */
    const bool log_ok = panel_log_start() == ESP_OK;
    if (log_ok) {
        alarm_service_set_min_event_seq(panel_log_last_seq());
        ESP_ERROR_CHECK(alarm_service_add_listener(panel_log_listener()));
    }
    const alarm_listener_t sounder_listener = {
        .on_event = on_event,
        .on_sound = on_sound,
        .on_output = on_output,
        .on_snapshot = on_snapshot,
    };
    ESP_ERROR_CHECK(alarm_service_add_listener(&sounder_listener));
#if CONFIG_ALARM_FEATURE_MQTT
    ESP_ERROR_CHECK(alarm_service_add_listener(net_mqtt_listener()));
#endif
#if CONFIG_ALARM_FEATURE_HA
    ESP_ERROR_CHECK(alarm_service_add_listener(ha_link_listener()));
#endif
#if CONFIG_ALARM_LINK
    /* The panel link's master side (docs/features/11) can't stop the boot: without its listener
     * the link stays off and the alarm runs on. */
    const esp_err_t link_err = alarm_service_add_listener(plink_service_listener());
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "the panel link's listener not added (%s): no keypads",
                 esp_err_to_name(link_err));
    }
#endif
    ESP_ERROR_CHECK(alarm_service_add_listener(zone_service_listener()));
    if (cfg->before_engine != NULL) {
        cfg->before_engine(); /* the board's own listeners (Panel Manager sign-ins) */
    }
#if CONFIG_ALARM_FEATURE_NOTIFY
    ESP_ERROR_CHECK(alarm_service_add_listener(net_notify_listener()));
#endif
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    /* The automations' listener (docs/features/41 DESIGN.md section 5) can't stop the boot: if
     * it isn't taken the automations stay off and the alarm runs on. */
    {
        const alarm_listener_t *auto_listener = automation_service_listener();
        const esp_err_t auto_err =
            auto_listener != NULL ? alarm_service_add_listener(auto_listener) : ESP_ERR_NO_MEM;
        if (auto_err != ESP_OK) {
            ESP_LOGE(TAG, "automations' listener not added (%s)", esp_err_to_name(auto_err));
        }
        automation_service_listener_added(auto_err == ESP_OK);
    }
#endif
    /* This can fail for two reasons that both mean the panel must not carry on: code hashing
     * that does not work (nobody could ever disarm it) and a configuration that could not be
     * read. Neither is a reason to write anything: the stored installation is left untouched
     * for the next boot, which is the same rule as init_nvs() above (R06/R05/R08). */
    const esp_err_t alarm_err = alarm_service_start();
    if (alarm_err != ESP_OK) {
        recovery_park("the alarm engine did not start", alarm_err);
        return;
    }
    /* A card on the same SPI bus as the 433 MHz receiver sets the bus up first, with DMA
     * (panel_storage.h); everywhere else the card starts later, as it always has. */
    const board_info_t *info = board_info();
    const bool storage_first = info->sd.bus == BOARD_SD_SPI && info->sd.spi_host == info->rf.spi_host;
    if (storage_first && panel_storage_start(&info->sd) != ESP_OK) {
        ESP_LOGW(TAG, "optional SD storage could not start");
    }
    const bool zones_ok = start_zone_sources();

    /* After an update the new firmware is on probation: every part named in the board's mask
     * has to keep reporting that it is working, or the previous firmware returns (docs/features/13
     * OTA-005). From here on nothing aborts: the engine is protecting the house, and an abort
     * would reboot into the same abort with no console to recover from (the console starts
     * last). */
    panel_ota_set_device(board_info()->id); /* when CONFIG_PANEL_OTA_DEVICE doesn't name it */
    /* What a check tells the update server (OTA-018): board_info_t has no revision, so none. */
    panel_ota_set_facts("", s_ota_parts, s_ota_parts_n);
    backup_service_set_board(board_info()->id); /* a backup restores on this kind only */
    START_OR_LOG(panel_ota_start(cfg->health_mask));

    START_OR_LOG(lang_packs_start()); /* the stored language's pack, before the screen speaks it */
    /* The asset packs (docs/features/39 asset-packs.md, OPT-006): the letters, the saver's picture
     * and the certificates, on this task (an internal-RAM stack: the signature check reads flash
     * with the cache off), before the screen and before net_wifi_start() opens any TLS. */
    START_OR_LOG(asset_packs_start());
    /* The voice pack (docs/features/08 D12): after the sounder and the engine run, loaded on a
     * task of its own; a bad pack leaves the tones as they are. */
    START_OR_LOG(panel_voice_start());
    voice_prefs_apply(&prefs);

    const display_state_t disp = start_display(false);
    if (!disp.display) {
        ESP_LOGE(TAG, "no picture: the panel keeps protecting, but nobody can use it here");
    }
    if (!log_ok) {
        ESP_LOGE(TAG, "no event log: nothing that happens is being kept");
    }
    /* Nothing above reports a part as healthy: every heartbeat comes from the task that does
     * the work, so a part that starts and then stops is caught as well as one that never
     * started (R18). This task is the messenger for the parts that publish a progress counter
     * instead of beating themselves. */
    /* 8 KB: device_service_tick runs hc_refresh here (about 3.3 KB of frame) for the Z-Wave
     * outputs, and a result it invalidates logs from inside it (docs/features/41 phase B). */
    if (alarm_mem_task_create(health_task, "health", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "health task failed to start: an update could not confirm itself");
    }
    if (!zones_ok || !disp.touch) {
        ESP_LOGE(TAG, "start-up incomplete:%s%s - this panel is not fully working",
                 zones_ok ? "" : " no sensor input", disp.touch ? "" : " no touch");
    }
    if (!storage_first && panel_storage_start(&info->sd) != ESP_OK) {
        ESP_LOGW(TAG, "optional SD storage could not start");
    }
    START_OR_LOG(net_wifi_start());
    START_OR_LOG(net_check_init());
#if CONFIG_ALARM_FEATURE_MQTT
    net_check_set_service(check_service, NULL);
    const net_mqtt_fw_port_t fw_port = {.ctx = NULL, .get = fw_get, .install = fw_install};
    net_mqtt_set_firmware(&fw_port);
#if CONFIG_ALARM_ZSRC_ZWAVE
    /* the Z-Wave devices the panel switches, for Home Assistant (docs/features/36 ZW-036) */
    const net_mqtt_zw_port_t zw_port = {.ctx = NULL, .list = zw_ha_list,
                                        .command = zw_ha_command};
    net_mqtt_set_zwave(&zw_port);
#endif
#if CONFIG_ALARM_ZSRC_ZIGBEE
    /* ~/zigbee/state, its sensor and the Zigbee-fed zones' battery and signal (ZB-026) */
    const net_mqtt_zb_port_t zb_port = {.ctx = NULL, .state = zb_mqtt_state,
                                        .zone_diags = zb_mqtt_zone_diags,
                                        .changes = zb_mqtt_changes};
    net_mqtt_set_zigbee(&zb_port);
#endif
    panel_ota_add_listener(fw_changed, NULL);
    net_mqtt_set_model(cfg->mqtt_model != NULL ? cfg->mqtt_model : board_name());
    if (net_mqtt_start() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client failed to start: no remote reporting");
    }
#endif
#if CONFIG_ALARM_FEATURE_NOTIFY
    if (net_notify_start() != ESP_OK) {
        ESP_LOGE(TAG, "notifications failed to start: no alerts to a phone");
    }
#endif
    const bool devices_ok = device_service_start() == ESP_OK;
    if (!devices_ok) {
        ESP_LOGE(TAG, "device registry failed to start: no Devices screen");
    }
    /* the stored automations (docs/features/41): loaded on this internal-stack task; their "auto"
     * task starts here too, after the device registry it acts through. */
    if (automation_service_start() != ESP_OK) {
        ESP_LOGE(TAG, "automations failed to load");
    }
#if CONFIG_ALARM_FEATURE_HA
    ha_link_set_model(cfg->ha_model != NULL ? cfg->ha_model : board_name());
#if CONFIG_ALARM_ZSRC_ZWAVE
    /* the Z-Wave devices over the native link too: with it paired, MQTT discovery is off */
    const ha_link_zw_port_t ha_zw_port = {.ctx = NULL, .list = zw_ha_list,
                                          .command = zw_ha_command};
    ha_link_set_zwave(&ha_zw_port);
#endif
    /* Panel settings (docs/features/23 HAI-016..019): the master's own need nothing here (ha_link
     * reports them and hands a change to the screen's mailbox, panel_prefs). The link keypads'
     * come through a port, set once the panel link runs (below); until then the keypads list is
     * empty and a keypad target is not_found. */
    if (ha_link_start() != ESP_OK) {
        ESP_LOGE(TAG, "Home Assistant link failed to start");
    }
#endif
#if CONFIG_ALARM_LINK
    if (link_err == ESP_OK) {
        /* the keypad numbers 1-8 shared with the Zigbee keypads (LINK-001) */
        plink_service_set_devices_port(zigbee_service_devices_held, zigbee_service_devices_held_now,
                                       zigbee_service_set_link_devices);
        /* its devices on the keypads' Smart Home only with a running registry (BL-216) */
        plink_service_set_home(devices_ok);
        if (plink_service_start(board_name()) != ESP_OK) {
            ESP_LOGE(TAG, "panel link failed to start: no keypads");
        } else {
#if CONFIG_ALARM_FEATURE_HA
            /* the keypads and their settings for Home Assistant (HAI-016..019) */
            static const ha_link_settings_port_t ha_kp_port = {
                .ctx = NULL, .keypads = ha_kp_list, .set = ha_kp_set};
            ha_link_set_settings(&ha_kp_port);
#endif
        }
    }
#else
    (void)devices_ok; /* no keypads to show its devices to */
#endif
#if CONFIG_ALARM_SERVICE_CONSOLE
    START_OR_LOG(panel_console_start());
#if CONFIG_ALARM_FEATURE_MQTT
    START_OR_LOG(net_mqtt_console_register());
#endif
#if CONFIG_ALARM_FEATURE_NOTIFY
    START_OR_LOG(net_notify_console_register());
#endif
#if CONFIG_ALARM_FEATURE_HA
    START_OR_LOG(ha_link_console_register());
#endif
#if CONFIG_ALARM_LINK
    START_OR_LOG(plink_service_console_register());
#endif
    START_OR_LOG(device_service_console_register());
#if CONFIG_ALARM_FEATURE_AUTOMATIONS
    START_OR_LOG(automation_service_console_register());
#endif
    START_OR_LOG(rf_service_console_register());
    START_OR_LOG(zigbee_service_console_register());
#if CONFIG_ALARM_ZSRC_ZWAVE
    START_OR_LOG(zwave_service_console_register());
#endif
    START_OR_LOG(panel_storage_console_register());
    START_OR_LOG(panel_clock_console_register());
    START_OR_LOG(panel_ota_console_register());
    START_OR_LOG(lang_packs_console_register());
    START_OR_LOG(panel_voice_console_register());
    START_OR_LOG(backup_service_console_register());
    START_OR_LOG(app_console_register());
    if (cfg->console != NULL) {
        START_OR_LOG(cfg->console());
    }
#endif
    ESP_LOGI(TAG, "%s running: internal RAM free %u KB (largest block %u KB)", board_name(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) /
                        1024u));
    if (cfg->started != NULL) {
        cfg->started();
    }
}
