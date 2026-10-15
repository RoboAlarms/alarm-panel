/*
 * sim_main.c - the interactive panel simulator: the real alarm engine, the real event log, the
 * LVGL Pro UI and its logic in a window on the PC, with the panel's 1024x600 screen on the
 * left and buttons on the right that put the panel into the states worth looking at.
 *
 * It exists so the UI can be tried without flashing a panel that is protecting a house. Click
 * the screen as you would touch it; the buttons on the right stand in for the things that
 * happen to a real panel - a door opening, mains power failing, the router going away.
 *
 * Two LVGL displays share one window:
 *   - the panel display is exactly 1024x600 and renders straight into s_panel_fb, so what you
 *     see is what the panel draws, pixel for pixel;
 *   - the window display holds a canvas showing that buffer, the buttons and the log.
 * Presses on the canvas are handed to the panel display's own pointer device with the panel's
 * scroll threshold, so drags and taps behave as they do on the glass.
 *
 *   ui_sim [screenshot folder]
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "alarm/alarm_strings.h"
#include "alarm_ui_gen.h"
#include "host_panel.h"
#include "lvgl.h"
#include "sim_audio.h"
#include "shot_png.h"
#include "ui_app.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* The board profile's screen (cmake/alarm_board.cmake); the 7" panel's without one. */
#ifndef ALARM_SCREEN_W
#define ALARM_SCREEN_W 1024
#define ALARM_SCREEN_H 600
#endif
#define PANEL_W ALARM_SCREEN_W
#define PANEL_H ALARM_SCREEN_H
#define SIDE_W  430
#define LOG_H   170
#define WIN_W   (PANEL_W + SIDE_W)
#define WIN_H   (PANEL_H + LOG_H)

#define MASTER_CODE "123456" /* the simulator's own panel keeps the factory codes */

static uint16_t s_panel_fb[PANEL_W * PANEL_H];
static lv_display_t *s_panel;
static lv_obj_t *s_canvas;
static lv_obj_t *s_state_label;
static lv_obj_t *s_log_label;
static lv_obj_t *s_net_buttons[HOST_NET_FAULT_COUNT];
static lv_obj_t *s_mute_button;
static int32_t s_px;
static int32_t s_py;
static bool s_pressed;
static uint64_t s_last_real;
static const char *s_shot_dir = "shots";
static int s_shots;
static char s_log[8][96];
static size_t s_log_n;

/* ---- the world's clock ---------------------------------------------------------------- */

static uint64_t real_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static uint32_t tick_cb(void)
{
    return (uint32_t)real_ms();
}

/* Give the engine and the zone hub 'ms' more, in steps small enough that their deadlines
 * still land in order (the scripted preview uses the same 10 ms step). */
static void advance(uint64_t ms)
{
    for (uint64_t t = 0; t < ms; t += 10u) {
        const uint64_t step = (ms - t) < 10u ? (ms - t) : 10u;
        host_panel_advance((alarm_ms_t)step);
    }
}

/* ---- the log strip ---------------------------------------------------------------------- */

static void say(const char *fmt, ...)
{
    char line[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (s_log_n == sizeof(s_log) / sizeof(s_log[0])) {
        memmove(s_log[0], s_log[1], sizeof(s_log) - sizeof(s_log[0]));
        s_log_n--;
    }
    snprintf(s_log[s_log_n++], sizeof(s_log[0]), "%s", line);
    char all[sizeof(s_log) + 8];
    size_t used = 0;
    for (size_t i = 0; i < s_log_n; i++) {
        used += (size_t)snprintf(all + used, sizeof(all) - used, "%s%s", i ? "\n" : "", s_log[i]);
    }
    if (s_log_label != NULL) {
        lv_label_set_text(s_log_label, all);
    }
    printf("%s\n", line);
    fflush(stdout);
}

/* ---- the panel's screen ----------------------------------------------------------------- */

static void panel_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (s_canvas != NULL) {
        lv_obj_invalidate(s_canvas); /* the window redraws the new frame */
    }
    lv_display_flush_ready(disp);
}

static void panel_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_px;
    data->point.y = s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* A press anywhere on the canvas becomes a touch at the same spot on the panel's glass. */
static void on_canvas(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_pressed = false;
        return;
    }
    lv_indev_t *mouse = lv_indev_active();
    if (mouse == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(mouse, &p);
    lv_area_t a;
    lv_obj_get_coords(s_canvas, &a);
    s_px = p.x - a.x1;
    s_py = p.y - a.y1;
    s_pressed = true;
}

static void save_shot(void)
{
    static uint8_t rgb[PANEL_W * PANEL_H * 3];
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        const uint16_t c = s_panel_fb[i];
        const uint8_t r = (uint8_t)((c >> 11) & 0x1F);
        const uint8_t g = (uint8_t)((c >> 5) & 0x3F);
        const uint8_t b = (uint8_t)(c & 0x1F);
        rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/sim_%02d.png", s_shot_dir, ++s_shots);
    unsigned char *png = NULL;
    size_t size = 0;
    if (shot_png_encode(&png, &size, rgb, PANEL_W, PANEL_H) == 0) {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(png, 1, size, f);
            fclose(f);
            say("Saved %s", path);
        }
    }
    shot_png_free(png);
}

/* ---- what the buttons do ---------------------------------------------------------------- */

#if ALARM_SIM_ZWAVE
#define ZW_SIM_DOOR_ZONE 9u /* zones 1-8 are the demo zones (host_panel_init); this one is free */
#endif

enum {
    A_DISARM = 0,
    A_ARM_AWAY,
    A_ARM_STAY,
    A_WALK_REMOTE,
    A_ZONE_FRONT,
    A_ZONE_MOTION,
    A_ZONE_BATTERY,
    A_ZONE_LOST,
    A_FIRE,
    A_CO,
    A_WATER,
    A_TRB_AC,
    A_TRB_CLEAR,
    A_SKIP_30,
    A_SKIP_5M,
    A_NET_HEALTHY,
    A_FORGET_WIFI,
    A_HA_PAIR,
    A_HA_PORCH,
    A_HA_LOST,
    A_ZB_PAIR,
    A_ZB_OPEN,
    A_ZB_TAMPER,
    A_ZB_BATTERY,
    A_ZB_LEAVE,
    A_ZB_UNPLUG,
    A_ZB_JAM,
    A_ZB_MOTION,  /* the real Zigbee's virtual devices (host_zigbee_real.h) */
    A_ZB_SMOKE,
    A_ZB_WATER,
    A_ZB_RESTART,
    A_RF_OPEN,
    A_RF_NEIGHBOUR,
    A_RF_GLITCH,
    A_RF_TAMPER,
    A_RF_BATTERY,
    A_RF_UNPLUG,
    A_RF_JAM,
    A_CLOCK_FIT,
    A_CLOCK_LOST,
    A_CLOCK_FAIL,
    A_SD_READY, A_SD_REMOVE, A_SD_FULL, A_SD_UNSUPPORTED, A_SD_FAIL,
    A_REMOTE_SESSION,
    A_NTFY_ON,
    A_PUSH_ON,
    A_NOTIFY_DOWN,
    A_EIGHT_ZONES,
    A_SHOT,
    A_RESET,
    A_MUTE,
#if ALARM_SIM_ZWAVE
    A_ZW_ADD_DOOR,
    A_ZW_PIN,
    A_ZW_ADD_MOTION,
    A_ZW_DOOR_OPEN,
    A_ZW_MOTION_OPEN,
    A_ZW_TAMPER,
    A_ZW_BATTERY,
    A_ZW_SLEEP_MISS,
    A_ZW_RADIO,
    A_ZW_RESTART,
    A_ZW_BIND_DOOR,
    A_ZW_PRESS_SPARE_SECURE,
    A_ZW_PRESS_SPARE_INSECURE,
    A_ZW_WAKE_SPARE_INSECURE,
    A_ZW_PRESS_DOOR_REMOVE,
#endif
    A_NET_FAULT_BASE = 100, /* + host_net_fault_t */
};

static bool s_front_open;
static bool s_ha_porch;
static bool s_ha_lost;
static bool s_zb_open;
static bool s_zb_tamper;
static bool s_zb_battery;
static bool s_zb_unplug;
static bool s_zb_jam;
static bool s_zb_motion;
static bool s_zb_smoke;
static bool s_zb_water;
static bool s_rf_open;
static bool s_rf_unplug;
static bool s_rf_jam;
static bool s_clock_fitted;
static bool s_clock_failed;
static bool s_notify_down;
static bool s_motion_open;
static bool s_battery_low;
static bool s_lost;
static bool s_fire;
static bool s_co;
static bool s_water;
static bool s_ac_fail;
#if ALARM_SIM_ZWAVE
static bool s_zw_door_open;
static bool s_zw_motion_open;
static bool s_zw_tamper;
static bool s_zw_battery_low;
static bool s_zw_sleep_miss;
static bool s_zw_radio_out;
static uint32_t s_zw_seq_seen;
#endif

static void command(alarm_cmd_type_t type, alarm_arm_level_t level, const char *what)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = 1;
    cmd.level = level;
    snprintf(cmd.code, sizeof(cmd.code), "%s", MASTER_CODE);
    const alarm_result_t r = host_panel_remote_command(&cmd);
    say("%s: %s", what, r == ALARM_OK ? "done" : alarm_result_name(r));
}

static void net_fault_pressed(host_net_fault_t fault)
{
    static const char *const NAMES[HOST_NET_FAULT_COUNT] = {
        "Wi-Fi down", "no DHCP lease", "router down", "name lookups down",
        "internet down", "time server down", "service down",
    };
    const bool on = !host_panel_net_faulty(fault);
    host_panel_net_fault(fault, on);
    say("House network: %s %s", NAMES[fault], on ? "ON" : "off");
}

static void on_action(lv_event_t *e)
{
    const int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action >= A_NET_FAULT_BASE) {
        net_fault_pressed((host_net_fault_t)(action - A_NET_FAULT_BASE));
        return;
    }
    switch (action) {
    case A_DISARM: command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, "Disarm"); break;
    case A_ARM_AWAY: command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, "Arm Away"); break;
    case A_ARM_STAY: command(ALARM_CMD_ARM, ALARM_LEVEL_STAY, "Arm Stay"); break;
    case A_WALK_REMOTE: {
        /* docs/features/40 WALK-023: a walk test the Panel Manager starts (or ends), signed in
         * as the master; the keypad's code + TEST toggle, so a second press ends it. */
        alarm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ALARM_CMD_WALK_TEST;
        cmd.partition = 1;
        snprintf(cmd.code, sizeof(cmd.code), "%s", MASTER_CODE);
        const alarm_result_t r = host_panel_manager_command(&cmd);
        say("Walk test from the Panel Manager: %s", r == ALARM_OK ? "done" : alarm_result_name(r));
        break;
    }
    case A_ZONE_FRONT:
        s_front_open = !s_front_open;
        host_panel_zone(1, s_front_open ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE);
        say("Front Door %s", s_front_open ? "opened" : "closed");
        break;
    case A_ZONE_MOTION:
        s_motion_open = !s_motion_open;
        host_panel_zone(5, s_motion_open ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE);
        say("Hallway Motion %s", s_motion_open ? "sees someone" : "clear");
        break;
    case A_ZONE_BATTERY:
        s_battery_low = !s_battery_low;
        host_panel_zone(8, s_battery_low ? ALARM_ZEV_LOW_BATTERY : ALARM_ZEV_LOW_BATTERY_RESTORE);
        say("Basement Water battery %s", s_battery_low ? "low" : "good");
        break;
    case A_ZONE_LOST:
        s_lost = !s_lost;
        host_panel_zone(5, s_lost ? ALARM_ZEV_UNREACHABLE : ALARM_ZEV_REACHABLE);
        say("Hallway Motion %s", s_lost ? "stopped answering" : "answers again");
        break;
    case A_FIRE:
        s_fire = !s_fire;
        host_panel_zone(6, s_fire ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE);
        say("Kitchen Smoke %s", s_fire ? "in alarm" : "restored");
        break;
    case A_CO:
        s_co = !s_co;
        host_panel_zone(7, s_co ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE);
        say("Hallway CO %s (listen for temporal-4, not the fire pattern)",
            s_co ? "in alarm" : "restored");
        break;
    case A_WATER:
        s_water = !s_water;
        host_panel_zone(8, s_water ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE);
        say("Basement Water %s", s_water ? "sees water (24-hour aux)" : "dry");
        break;
    case A_TRB_AC:
        s_ac_fail = !s_ac_fail;
        host_panel_trouble(ALARM_TRBL_AC_LOSS, s_ac_fail);
        say("Mains power %s", s_ac_fail ? "lost" : "back");
        break;
    case A_TRB_CLEAR:
        host_panel_trouble(ALARM_TRBL_AC_LOSS, false);
        host_panel_zone(8, ALARM_ZEV_LOW_BATTERY_RESTORE);
        host_panel_zone(5, ALARM_ZEV_REACHABLE);
        s_ac_fail = s_battery_low = s_lost = false;
        say("Troubles cleared");
        break;
    case A_SKIP_30:
        advance(30000u);
        say("Skipped 30 seconds");
        break;
    case A_SKIP_5M:
        advance(300000u);
        say("Skipped 5 minutes");
        break;
    case A_NET_HEALTHY:
        host_panel_net_clear_faults();
        say("House network: everything healthy again");
        break;
    case A_FORGET_WIFI:
        host_panel_forget_wifi();
        say("Wi-Fi forgotten (Settings > Wi-Fi starts from nothing)");
        break;
    case A_HA_PAIR:
        host_panel_ha_pair_request();
        say("Home Assistant asks to pair (needs the window open: Settings > Integrations > "
            "Home Assistant > Pair)");
        break;
    case A_HA_PORCH:
        s_ha_porch = !s_ha_porch;
        host_panel_ha_entity("binary_sensor.porch_door", s_ha_porch ? "on" : "off", true);
        say("Porch Door (a Home Assistant sensor) %s", s_ha_porch ? "opened" : "closed");
        break;
    case A_ZB_PAIR:
        if (host_panel_zigbee_is_real()) {
            const char *who = host_panel_zigbee_press();
            if (who != NULL) {
                say("The Zigbee %s pressed its pairing button and joins", who);
            } else {
                say("Nothing joins: open Installer > Zigbee > Add a device first (two devices "
                    "join by themselves), or every virtual device is in already");
            }
            break;
        }
        host_panel_zigbee_pair();
        say(host_panel_zigbee_paired()
                ? "A Frient door contact and motion sensor joined; bind them in Settings > "
                  "Zones > Source"
                : "No radio: plug the module back in first");
        break;
    case A_ZB_OPEN:
        s_zb_open = !s_zb_open;
        host_panel_zigbee_open(s_zb_open);
        say("The Zigbee contact %s", s_zb_open ? "opened" : "closed");
        break;
    case A_ZB_TAMPER:
        s_zb_tamper = !s_zb_tamper;
        host_panel_zigbee_tamper(s_zb_tamper);
        say("The Zigbee contact's case is %s", s_zb_tamper ? "open (tamper)" : "closed again");
        break;
    case A_ZB_BATTERY:
        s_zb_battery = !s_zb_battery;
        host_panel_zigbee_battery(s_zb_battery);
        say("The Zigbee contact's battery is %s", s_zb_battery ? "nearly flat" : "healthy");
        break;
    case A_ZB_LEAVE:
        host_panel_zigbee_leave();
        s_zb_open = false;
        s_zb_tamper = false;
        s_zb_battery = false;
        say("The Zigbee contact left the network: its zone shows CHECK after the grace");
        break;
    case A_ZB_UNPLUG:
        s_zb_unplug = !s_zb_unplug;
        host_panel_zigbee_radio(!s_zb_unplug);
        say("The ESP32-H2 module is %s", s_zb_unplug ? "out of its socket" : "back in");
        break;
    case A_ZB_JAM:
        s_zb_jam = !s_zb_jam;
        host_panel_zigbee_jam(s_zb_jam);
        say("The Zigbee channel is %s", s_zb_jam ? "being drowned out" : "quiet again");
        break;
    /* each toggle changes only when its device took the trip, so the button and the device stay in
     * step while it is out of the network */
    case A_ZB_MOTION:
        if (!host_panel_zigbee_trip(1u, !s_zb_motion)) {
            say("The Zigbee motion sensor isn't in the network yet");
            break;
        }
        s_zb_motion = !s_zb_motion;
        say(s_zb_motion ? "The Zigbee motion sensor sees movement"
                        : "The Zigbee motion sensor is quiet");
        break;
    case A_ZB_SMOKE:
        if (!host_panel_zigbee_trip(2u, !s_zb_smoke)) {
            say("The Zigbee smoke alarm isn't in the network yet");
            break;
        }
        s_zb_smoke = !s_zb_smoke;
        say(s_zb_smoke ? "The Zigbee smoke alarm sees smoke" : "The Zigbee smoke alarm cleared");
        break;
    case A_ZB_WATER:
        if (!host_panel_zigbee_trip(3u, !s_zb_water)) {
            say("The Zigbee water sensor isn't in the network yet");
            break;
        }
        s_zb_water = !s_zb_water;
        say(s_zb_water ? "The Zigbee water sensor is wet" : "The Zigbee water sensor is dry");
        break;
    case A_ZB_RESTART:
        host_panel_zigbee_restart();
        say("The panel restarted: Zigbee read its devices and settings back from storage");
        break;
    case A_RF_OPEN:
        s_rf_open = !s_rf_open;
        host_panel_rf_open(s_rf_open);
        say("The 433 MHz contact sent its %s code (learn it under Zones > Add a zone > 433 MHz)",
            s_rf_open ? "open" : "closed");
        break;
    case A_RF_NEIGHBOUR:
        host_panel_rf_neighbour();
        say("A neighbour's 433 MHz remote was pressed (another sensor on the air)");
        break;
    case A_RF_GLITCH:
        host_panel_rf_glitch();
        say("One corrupted 433 MHz frame: heard once, so never offered as a sensor");
        break;
    case A_RF_TAMPER:
        host_panel_rf_tamper();
        say("The 433 MHz contact sent its tamper code");
        break;
    case A_RF_BATTERY:
        host_panel_rf_battery();
        say("The 433 MHz contact sent its low-battery code");
        break;
    case A_RF_UNPLUG:
        s_rf_unplug = !s_rf_unplug;
        host_panel_rf_receiver(!s_rf_unplug);
        say("The CC1101 is %s", s_rf_unplug ? "off its bus: the source's trouble after a minute"
                                            : "answering again");
        break;
    case A_RF_JAM:
        s_rf_jam = !s_rf_jam;
        host_panel_rf_jam(s_rf_jam);
        say("The 433 MHz band is %s", s_rf_jam ? "jammed: a trouble after 30 s + a minute"
                                               : "quiet again");
        break;
    case A_CLOCK_FIT:
        s_clock_fitted = !s_clock_fitted;
        s_clock_failed = false;
        host_panel_clock_module(s_clock_fitted ? UI_CLOCK_MODULE_OK : UI_CLOCK_MODULE_NONE, false);
        say("%s", s_clock_fitted ? "A DS3231 clock module is on the I2C connector: Settings > "
                                   "Date and time can set the time by hand"
                                 : "No clock module: the time comes from the internet only");
        break;
    case A_CLOCK_LOST:
        s_clock_fitted = true;
        s_clock_failed = false;
        host_panel_clock_module(UI_CLOCK_MODULE_LOST, true);
        say("The clock module's coin cell went flat during a power cut: it lost the time");
        break;
    case A_CLOCK_FAIL:
        s_clock_failed = !s_clock_failed;
        host_panel_clock_module(s_clock_failed ? UI_CLOCK_MODULE_FAILED
                                               : (s_clock_fitted ? UI_CLOCK_MODULE_OK
                                                                 : UI_CLOCK_MODULE_NONE),
                                false);
        say("The clock module %s", s_clock_failed ? "stopped answering on its bus"
                                                  : "answers again");
        break;
    case A_HA_LOST:
        s_ha_lost = !s_ha_lost;
        host_panel_ha_entity("binary_sensor.porch_door", s_ha_porch ? "on" : "off", !s_ha_lost);
        say("Porch Door %s in Home Assistant", s_ha_lost ? "unavailable" : "back");
        break;
    case A_SD_READY:
        host_panel_storage_fail(false);
        host_panel_storage_preset(STORAGE_READY, UINT64_C(64000000000), UINT64_C(48000000000));
        say("64 GB SD card inserted (simulated)"); break;
    case A_SD_REMOVE:
        host_panel_storage_preset(STORAGE_NO_CARD, 0u, 0u);
        say("SD card removed (simulated)"); break;
    case A_SD_FULL:
        host_panel_storage_preset(STORAGE_READY, UINT64_C(64000000000), 0u);
        say("SD card full (simulated)"); break;
    case A_SD_UNSUPPORTED:
        host_panel_storage_preset(STORAGE_UNSUPPORTED, UINT64_C(64000000000), 0u);
        say("SD card has an unsupported filesystem (simulated)"); break;
    case A_SD_FAIL:
        host_panel_storage_fail(true);
        say("Next simulated format will fail; Insert SD card resets this"); break;
    case A_REMOTE_SESSION: {
        /* Someone on the Panel Manager's remote keypad (docs/features/31): the red banner; tap it
         * on the screen to pause them and answer Allow or Turn off remote access. */
        static bool on;
        on = !on;
        ui_remote_session_t rs;
        memset(&rs, 0, sizeof(rs));
        rs.active = on;
        rs.paused = host_panel_remote_paused(); /* a pause outlives the session */
        snprintf(rs.who, sizeof(rs.who), "Alex");
        snprintf(rs.where, sizeof(rs.where), "198.51.100.23");
        ui_app_remote_session(&rs);
        say(on ? "Alex is on the remote keypad: the banner shows; tap it to pause them"
               : "The remote keypad session ended (a pause stays until answered)");
        break;
    }
    case A_NTFY_ON:
        host_panel_notify_preset(UI_NOTIFY_NTFY, true, true);
        say("ntfy is set up and sending (Settings > Integrations > Notifications > ntfy)");
        break;
    case A_PUSH_ON:
        host_panel_notify_preset(UI_NOTIFY_PUSHOVER, true, true);
        say("Pushover is set up with both credentials, alarms repeat until answered");
        break;
    case A_NOTIFY_DOWN:
        s_notify_down = !s_notify_down;
        host_panel_notify_fault(UI_NOTIFY_NTFY, s_notify_down);
        host_panel_notify_fault(UI_NOTIFY_PUSHOVER, s_notify_down);
        say("The notification services %s", s_notify_down ? "stop answering" : "answer again");
        break;
    case A_EIGHT_ZONES:
        /* One burglary that trips the whole ground floor: the gate must turn this into two
         * notifications, not eight (docs/features/26 NOTE-033). */
        for (uint16_t z = 1u; z <= 8u; z++) {
            host_panel_zone(z, ALARM_ZEV_FAULT);
            advance(700u);
        }
        say("Eight zones faulted in one alarm - the phone should get two messages, not eight");
        break;
    case A_MUTE:
        sim_audio_mute(!sim_audio_muted());
        say("Sound %s (the panel goes on sounding underneath)",
            sim_audio_muted() ? "muted" : "on");
        break;
    case A_SHOT: save_shot(); break;
    case A_RESET:
        host_panel_init(1789857660000LL);
        host_panel_net_clear_faults();
        s_front_open = s_motion_open = s_battery_low = s_lost = s_fire = s_ac_fail = false;
        s_co = s_water = false;
        s_zb_open = s_zb_tamper = s_zb_battery = s_zb_unplug = s_zb_jam = false;
        s_zb_motion = s_zb_smoke = s_zb_water = false;
#if ALARM_SIM_ZWAVE
        s_zw_door_open = s_zw_motion_open = s_zw_tamper = s_zw_battery_low = false;
        s_zw_sleep_miss = s_zw_radio_out = false;
        s_zw_seq_seen = 0u;
#endif
        say("Simulator reset");
        break;
#if ALARM_SIM_ZWAVE
    case A_ZW_ADD_DOOR:
        host_panel_zwave_add_secure();
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_PIN:
        host_panel_zwave_pin();
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_ADD_MOTION:
        host_panel_zwave_add_insecure();
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_DOOR_OPEN:
        s_zw_door_open = !s_zw_door_open;
        host_panel_zwave_door(s_zw_door_open);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_MOTION_OPEN:
        s_zw_motion_open = !s_zw_motion_open;
        host_panel_zwave_motion(s_zw_motion_open);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_TAMPER:
        s_zw_tamper = !s_zw_tamper;
        host_panel_zwave_tamper(s_zw_tamper);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_BATTERY:
        s_zw_battery_low = !s_zw_battery_low;
        host_panel_zwave_battery(s_zw_battery_low);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_SLEEP_MISS:
        s_zw_sleep_miss = !s_zw_sleep_miss;
        host_panel_zwave_sleep_miss(s_zw_sleep_miss);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_RADIO:
        s_zw_radio_out = !s_zw_radio_out;
        host_panel_zwave_radio(!s_zw_radio_out);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_RESTART:
        host_panel_zwave_restart();
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_BIND_DOOR:
        (void)host_panel_zwave_bind_door(ZW_SIM_DOOR_ZONE);
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_PRESS_SPARE_SECURE:
        host_panel_zwave_press_spare_secure();
        say("%s (label PIN %05u)", host_panel_zwave_status(),
            (unsigned)host_panel_zwave_spare_secure_pin());
        break;
    case A_ZW_PRESS_SPARE_INSECURE:
        host_panel_zwave_press_spare_insecure();
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_WAKE_SPARE_INSECURE:
        host_panel_zwave_wake_spare_insecure();
        say("%s", host_panel_zwave_status());
        break;
    case A_ZW_PRESS_DOOR_REMOVE:
        host_panel_zwave_press_door_remove();
        say("%s", host_panel_zwave_status());
        break;
#endif
    default: break;
    }
}

/* ---- the side panel --------------------------------------------------------------------- */

static void side_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_color(l, lv_color_hex(0x7c8aa5), 0);
    lv_obj_set_style_pad_top(l, 6, 0);
}

static lv_obj_t *side_button(lv_obj_t *parent, const char *text, int action, bool toggle)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 195, 34);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1d2634), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x8b3a3a), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x2c3950), 0);
    if (toggle) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
    }
    lv_obj_add_event_cb(b, on_action, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xdfe6f2), 0);
    lv_obj_center(l);
    return b;
}

static void build_side(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, PANEL_W, 0);
    lv_obj_set_size(col, SIDE_W, WIN_H);
    lv_obj_set_style_bg_color(col, lv_color_hex(0x0d131e), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(col, 14, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_style_pad_column(col, 8, 0);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "Panel simulator");
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

    lv_obj_t *note = lv_label_create(col);
    /* Plain ASCII in the simulator's own chrome: it uses LVGL's built-in font, which has no
     * glyph for the middle dot the panel's own fonts carry. */
    lv_label_set_text(note,
                      "Click the screen as you would touch it.\n"
                      "Master code 123456   |   installer 69420");
    lv_obj_set_width(note, lv_pct(100));
    lv_obj_set_style_text_color(note, lv_color_hex(0x7c8aa5), 0);

    side_title(col, "The system");
    side_button(col, "Disarm", A_DISARM, false);
    side_button(col, "Arm Away", A_ARM_AWAY, false);
    side_button(col, "Arm Stay", A_ARM_STAY, false);
    side_button(col, "Walk test from the Panel Manager", A_WALK_REMOTE, false);
    side_button(col, "Kitchen smoke alarm", A_FIRE, true);
    side_button(col, "Hallway CO alarm", A_CO, true);

    side_title(col, "Sensors and troubles");
    side_button(col, "Front Door", A_ZONE_FRONT, true);
    side_button(col, "Hallway Motion", A_ZONE_MOTION, true);
    side_button(col, "Basement water leak", A_WATER, true);
    side_button(col, "Sensor battery low", A_ZONE_BATTERY, true);
    side_button(col, "Sensor stops answering", A_ZONE_LOST, true);
    side_button(col, "Mains power lost", A_TRB_AC, true);
    side_button(col, "Clear troubles", A_TRB_CLEAR, false);

    side_title(col, "The house network");
    static const char *const FAULTS[HOST_NET_FAULT_COUNT] = {
        "Wi-Fi goes away", "No DHCP lease", "Router silent",   "Name lookups fail",
        "Internet down",   "Time server down", "Service down",
    };
    for (int i = 0; i < HOST_NET_FAULT_COUNT; i++) {
        s_net_buttons[i] = side_button(col, FAULTS[i], A_NET_FAULT_BASE + i, true);
    }
    side_button(col, "All healthy", A_NET_HEALTHY, false);
    side_button(col, "Forget Wi-Fi", A_FORGET_WIFI, false);
    side_button(col, "Home Assistant pairs", A_HA_PAIR, false);
    side_button(col, "Porch Door (via HA)", A_HA_PORCH, true);
    side_button(col, "Porch Door unavailable", A_HA_LOST, true);
    if (host_panel_zigbee_is_real()) {
        /* the panel's real Zigbee over the fake radio (host_zigbee_real.h): add devices from
         * Installer > Zigbee > Add a device; two press their buttons by themselves */
        side_button(col, "Zigbee: next device's pairing button", A_ZB_PAIR, false);
    } else {
        side_button(col, "A Zigbee sensor pairs", A_ZB_PAIR, false);
    }
    side_button(col, "Zigbee contact opens", A_ZB_OPEN, true);
    side_button(col, "Zigbee sensor tampered", A_ZB_TAMPER, true);
    side_button(col, "Zigbee battery flat", A_ZB_BATTERY, true);
    side_button(col, "Zigbee sensor leaves", A_ZB_LEAVE, false);
    side_button(col, "H2 module unplugged", A_ZB_UNPLUG, true);
    side_button(col, "Zigbee channel jammed", A_ZB_JAM, true);
    if (host_panel_zigbee_is_real()) {
        side_button(col, "Zigbee motion sees movement", A_ZB_MOTION, true);
        side_button(col, "Zigbee smoke alarm", A_ZB_SMOKE, true);
        side_button(col, "Zigbee water leak", A_ZB_WATER, true);
        side_button(col, "Panel restarts (Zigbee)", A_ZB_RESTART, false);
    }
    side_button(col, "433 MHz contact opens", A_RF_OPEN, true);
    side_button(col, "Neighbour's 433 MHz remote", A_RF_NEIGHBOUR, false);
    side_button(col, "Corrupted 433 MHz frame", A_RF_GLITCH, false);
    side_button(col, "433 MHz tamper code", A_RF_TAMPER, false);
    side_button(col, "433 MHz battery code", A_RF_BATTERY, false);
    side_button(col, "CC1101 unplugged", A_RF_UNPLUG, true);
    side_button(col, "433 MHz band jammed", A_RF_JAM, true);

#if ALARM_SIM_ZWAVE
    side_title(col, "Z-Wave (docs/features/36 Z6)");
    side_button(col, "Add secure door sensor (S2)", A_ZW_ADD_DOOR, false);
    side_button(col, "Type its PIN", A_ZW_PIN, false);
    side_button(col, "Add insecure motion sensor", A_ZW_ADD_MOTION, false);
    side_button(col, "Door sensor opens", A_ZW_DOOR_OPEN, true);
    side_button(col, "Motion sensor sees movement", A_ZW_MOTION_OPEN, true);
    side_button(col, "Door sensor tamper", A_ZW_TAMPER, true);
    side_button(col, "Door sensor battery low", A_ZW_BATTERY, true);
    side_button(col, "Door sensor misses check-ins", A_ZW_SLEEP_MISS, true);
    side_button(col, "Z-Wave module unplugged", A_ZW_RADIO, true);
    side_button(col, "Panel restarts (Z-Wave)", A_ZW_RESTART, false);
    side_button(col, "Bind Z-Wave door to zone 9", A_ZW_BIND_DOOR, false);
    /* The touchscreen's own Installer > Z-Wave > Add/Remove a device (Z7b) waits for a sensor's
     * button; these stand in for it. */
    side_button(col, "Z-Wave: new sensor's button (secure)", A_ZW_PRESS_SPARE_SECURE, false);
    side_button(col, "Z-Wave: new sensor's button (not secure)", A_ZW_PRESS_SPARE_INSECURE, false);
    /* Once it is in: its button wakes it, so a check-in chosen on its page goes out at once. */
    side_button(col, "Z-Wave: that sensor wakes up", A_ZW_WAKE_SPARE_INSECURE, false);
    side_button(col, "Z-Wave: door sensor's button (remove)", A_ZW_PRESS_DOOR_REMOVE, false);
    /* TODO(Z-Wave phase 2): a "channel jammed" button - network.md has no jam detector yet
     * (docs/features/36 README, Z6's "Skip jamming"). */
#endif

    side_title(col, "Clock module (DS3231)");
    side_button(col, "Clock module fitted", A_CLOCK_FIT, true);
    side_button(col, "Its coin cell went flat", A_CLOCK_LOST, false);
    side_button(col, "Clock module stops answering", A_CLOCK_FAIL, true);

    side_title(col, "SD storage");
    side_button(col, "Insert SD card", A_SD_READY, false);
    side_button(col, "Remove SD card", A_SD_REMOVE, false);
    side_button(col, "SD card full", A_SD_FULL, false);
    side_button(col, "Unsupported SD format", A_SD_UNSUPPORTED, false);
    side_button(col, "Next format fails", A_SD_FAIL, false);

    side_title(col, "Remote keypad");
    side_button(col, "Someone on the remote keypad", A_REMOTE_SESSION, true);

    side_title(col, "Notifications");
    side_button(col, "ntfy is set up", A_NTFY_ON, false);
    side_button(col, "Pushover is set up", A_PUSH_ON, false);
    side_button(col, "Services stop answering", A_NOTIFY_DOWN, true);
    side_button(col, "Eight zones at once", A_EIGHT_ZONES, false);

    side_title(col, "Sound");
    s_mute_button = side_button(col, "Mute sound", A_MUTE, true);
    lv_obj_t *vol = lv_label_create(col);
    lv_label_set_text(vol, "Volume and key clicks: Settings > Display and sound.");
    lv_label_set_long_mode(vol, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(vol, lv_pct(100));
    lv_obj_set_style_text_color(vol, lv_color_hex(0x7c8aa5), 0);

    side_title(col, "Time and housekeeping");
    side_button(col, "Skip 30 seconds", A_SKIP_30, false);
    side_button(col, "Skip 5 minutes", A_SKIP_5M, false);
    side_button(col, "Save screenshot", A_SHOT, false);
    side_button(col, "Reset simulator", A_RESET, false);
}

static void build_log(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 0, PANEL_H);
    lv_obj_set_size(box, PANEL_W, LOG_H);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0d131e), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);

    s_state_label = lv_label_create(box);
    lv_obj_set_width(s_state_label, lv_pct(100));
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x8fe3b0), 0);
    lv_label_set_text(s_state_label, "");

    s_log_label = lv_label_create(box);
    lv_obj_set_width(s_log_label, lv_pct(100));
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_log_label, lv_color_hex(0x9fb0c9), 0);
    lv_label_set_text(s_log_label, "");
}

/* One line saying where the panel is, refreshed once a second. */
/* The panel's backlight (UI-009, UI-043), shown as its picture fading to black: the screen saver
 * dims it, "Screen off" and the night hours put it out, a touch brings it back. */
static void show_backlight(void)
{
    static int shown = -2;
    const int pct = host_panel_backlight();
    if (pct == shown || s_canvas == NULL) {
        return;
    }
    const int was = shown;
    shown = pct;
    const int level = pct < 0 ? 100 : pct;
    lv_obj_set_style_image_opa(s_canvas, (lv_opa_t)(level * 255 / 100), 0);
    if (pct == 0) {
        say("Backlight off (dark): touch the screen to wake it.");
    } else if (was == 0) {
        say("Backlight on, %d%%.", pct); /* not every step of a slider */
    }
}

static void refresh_state(const ui_backend_t *be)
{
    if (s_state_label == NULL) {
        return;
    }
    alarm_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    be->get_snapshot(be->ctx, &snap);
    ui_wifi_info_t w;
    memset(&w, 0, sizeof(w));
    be->wifi_info(be->ctx, &w);
    const alarm_partition_status_t *p = &snap.part[0];
    char text[256];
    snprintf(text, sizeof(text), "Partition 1: %s%s   |   network: %s%s%s   |   sounder: %s%s",
             p->level == ALARM_LEVEL_NONE ? "disarmed" : alarm_level_name(p->level),
             p->triggered ? " - IN ALARM" : "",
             w.state == UI_NET_CONNECTED ? "connected " : "not connected",
             w.state == UI_NET_CONNECTED ? w.ip : "",
             w.state == UI_NET_CONNECTED && w.static_ip ? " (fixed address)" : "",
             p->sounder == ALARM_SOUND_NONE ? "quiet" : alarm_sound_name(p->sounder),
             sim_audio_muted() ? " (muted)" : "");
    lv_label_set_text(s_state_label, text);
    if (s_mute_button != NULL) {
        lv_obj_set_state(s_mute_button, LV_STATE_CHECKED, sim_audio_muted());
    }
    for (int i = 0; i < HOST_NET_FAULT_COUNT; i++) {
        if (s_net_buttons[i] != NULL) {
            lv_obj_set_state(s_net_buttons[i], LV_STATE_CHECKED,
                             host_panel_net_faulty((host_net_fault_t)i));
        }
    }
#if ALARM_SIM_ZWAVE
    /* zw_net's own events (an interview finishing, a late check-in going UNREACHABLE, the module
     * noticing the radio is back) happen between button presses, on the simulator's clock: catch
     * up the log here, once a second, same as the rest of this function. */
    const uint32_t zw_seq = host_panel_zwave_seq();
    if (zw_seq != s_zw_seq_seen) {
        s_zw_seq_seen = zw_seq;
        say("%s", host_panel_zwave_status());
    }
#endif
}

/* ---- main --------------------------------------------------------------------------------- */

static void make_dir(const char *path)
{
#ifdef _WIN32
    mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

#ifdef _WIN32
/* Without this Windows pretends the screen is 96 DPI and stretches the window afterwards,
 * which leaves the part LVGL painted sitting in a larger, part-black frame. Looked up at run
 * time so the simulator still starts on Windows versions that don't have it. */
static void become_dpi_aware(void)
{
    typedef BOOL(WINAPI * set_ctx_fn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    set_ctx_fn set_ctx =
        user32 ? (set_ctx_fn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext")
               : NULL;
    /* -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    if (set_ctx == NULL || !set_ctx((HANDLE)(intptr_t)-4)) {
        SetProcessDPIAware();
    }
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1) {
        s_shot_dir = argv[1];
    }
#ifdef _WIN32
    become_dpi_aware();
#endif
    make_dir(s_shot_dir);
    /* Zigbee: the panel's real zbz_panel over the fake radio and its virtual devices (BL-143
     * ZB-M6); ALARM_SIM_ZIGBEE_DEMO=1 brings back the preview's demo model */
    host_panel_zigbee_use_real(getenv("ALARM_SIM_ZIGBEE_DEMO") == NULL);
    /* The letters pack (docs/features/39 OPT-010, OPT-014): loaded, as on a panel the Flasher set
     * up; ALARM_SIM_LETTERS=0 shows a panel without it (boxes, no àé key) */
    const char *letters = getenv("ALARM_SIM_LETTERS");
    if (letters != NULL && strcmp(letters, "0") == 0) {
        host_panel_letters(false);
    }
#ifdef _WIN32
    _putenv("TZ=EST5EDT,M3.2.0,M11.1.0");
    _tzset();
#else
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
#endif

    lv_init();
    lv_tick_set_cb(tick_cb);

    /* The panel's own display first, so a frame it draws reaches the window in the same pass. */
    s_panel = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(s_panel, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_panel, s_panel_fb, NULL, sizeof(s_panel_fb),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_panel, panel_flush);

#ifdef _WIN32
    /* allow_dpi_override: keep one window pixel per panel pixel whatever Windows' display
     * scaling says, so the screen on the left is exactly what the panel draws. */
    lv_display_t *win = lv_windows_create_display(L"Alarm panel simulator", WIN_W, WIN_H, 100,
                                                  true, true);
    if (win == NULL) {
        fprintf(stderr, "could not open the simulator window\n");
        return 1;
    }
    lv_windows_acquire_pointer_indev(win);
    const HWND hwnd = lv_windows_get_display_window_handle(win);
#else
    fprintf(stderr, "The interactive simulator needs the LVGL Windows backend; on Linux and\n"
                    "macOS use the scripted preview instead: tools/ui_preview.ps1 / the cmake\n"
                    "commands in host/ui_preview.\n");
    return 1;
#endif

    lv_obj_t *root = lv_display_get_screen_active(win);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x05080e), 0);

    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_panel_fb, PANEL_W, PANEL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, on_canvas, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, on_canvas, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, on_canvas, LV_EVENT_PRESS_LOST, NULL);

    build_side(root);
    build_log(root);

    /* The panel's touchscreen: its own pointer device, with the panel's scroll threshold. */
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, panel_pointer);
    lv_indev_set_display(touch, s_panel);
    lv_indev_set_scroll_limit(touch, 20); /* board_touch.c */

    lv_display_set_default(s_panel); /* ui_app_start builds on the panel's screen */

    /* The speakers, before the world starts: the panel's sounds are wired exactly as they are
     * on the board, so what comes out here is what the panel would play on the wall. */
    const bool audible = sim_audio_start();
    if (audible) {
        const host_panel_sound_t sound = {
            .ctx = NULL,
            .continuous = sim_audio_continuous,
            .one_shot = sim_audio_one_shot,
            .key_click = sim_audio_key_click,
            .volume = sim_audio_volume,
            .chime_volume = sim_audio_chime_volume,
            .voice_volume = sim_audio_voice_volume,
            /* the voice: tones with tickets and speech through the panel's mixer */
            .tone = sim_audio_tone,
            .ticket = sim_audio_ticket,
            .speak = sim_audio_speak,
            .speech_cancel = sim_audio_speech_cancel,
            .speech_done = sim_audio_speech_done,
            .speech_due = sim_audio_speech_due,
            .power_saving = sim_audio_power_saving,
        };
        host_panel_sound(&sound);
    }

    host_panel_init(1789857660000LL); /* Saturday 19 September 2026, 22:41 */
    ui_backend_t be;
    host_panel_backend(&be);
    ui_options_t start;
    ui_app_default_options(&start);
#ifdef ALARM_PREVIEW_LOCALE
    /* -Locale: the panel's stored language is a test language (docs/features/37 I18N-024) */
    snprintf(start.lang, sizeof(start.lang), "%s", ALARM_PREVIEW_LOCALE);
#endif
    ui_app_start(&be, &start);

    say("Simulator ready. The screen on the left is the panel.");
    if (audible) {
        say("Sound is on, at %d%% of the panel's own level.", SIM_AUDIO_TRIM_PCT);
    } else {
        say("No sound card: the simulator runs, but you will not hear the sounder.");
    }
    s_last_real = real_ms();
    uint64_t status_at = 0;

    for (;;) {
#ifdef _WIN32
        if (!IsWindow(hwnd)) {
            break;
        }
#endif
        const uint64_t now = real_ms();
        const uint64_t delta = now - s_last_real;
        s_last_real = now;
        lv_lock(); /* the window runs on its own thread (see lv_conf.h) */
        if (delta > 0u) {
            advance(delta > 250u ? 250u : delta); /* a stall shouldn't fast-forward the panel */
        }
        if (now - status_at >= 1000u) {
            status_at = now;
            refresh_state(&be);
        }
        lv_timer_handler();
        show_backlight();
        lv_unlock();
#ifdef _WIN32
        Sleep(5);
#endif
    }
    host_panel_sound(NULL); /* nothing may reach the mixer once it is closing */
    sim_audio_stop();
    return 0;
}
