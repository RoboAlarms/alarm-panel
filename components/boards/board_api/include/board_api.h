#pragma once
/* board_api.h - what every board provides (docs/features/33 step 4, ADR-035).
 *
 * One board component is linked into each firmware and implements these functions; the app
 * (firmware/<board>/main) calls them and hands board_info() to the services, which never name
 * a board. A board's own extras (the CrowPanel's I2C bus, frame capture) stay in its own
 * header. Fields of the statistics a board doesn't measure stay 0.
 *
 * Bring-up order: board_power_init() -> board_backlight_init() -> board_display_init() ->
 * board_touch_init() -> first frame -> board_backlight_set() -> board_audio_init() (the
 * amplifier stays off until sound flows). The backlight stays off until a first full frame
 * is on the panel (CONTRIBUTING.md, "Display safety"). board_backlight_init() may also come
 * first, to switch off a backlight a restart left lit; a second call does nothing. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_info/board_info.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "power_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The board described in data: chip, screen, pins, terminals, SD, RF and radio wiring. */
const board_info_t *board_info(void);

/* A name for people and the log. */
const char *board_name(void);

/* Power rails that must be up before the display. */
esp_err_t board_power_init(void);

/* Backlight. percent 0 = off, 1..100 = dim..full. A board's safety guard and
 * CONFIG_BOARD_BACKLIGHT_FORCE_OFF can hold it off regardless. */
esp_err_t board_backlight_init(void);
esp_err_t board_backlight_set(uint8_t percent);

/* The display and LVGL (starts the LVGL task). The backlight stays off. */
esp_err_t board_display_init(lv_display_t **out_disp);

/* Display refresh and safety statistics (docs/features/06 P4-012, docs/features/21 `display`). */
typedef struct {
    uint32_t frames;     /* frames sent since start */
    uint32_t flushes;    /* UI redraws copied into the frame buffer since start */
    uint32_t late;       /* frames that came later than two frame times, since start */
    uint32_t max_gap_us; /* longest time between two frames since the last reset_max */
    bool guard_on;       /* the flicker guard holds the backlight off now */
    uint32_t guard_trips;
    uint32_t refreshes;      /* screen refreshes that drew something, since start */
    uint32_t slow_refreshes; /* of those, the ones longer than 50 ms */
    uint32_t max_refresh_us; /* longest refresh (draw + copy) since the last reset_max */
    uint8_t backlight;   /* requested brightness */
    bool forced_off;     /* CONFIG_BOARD_BACKLIGHT_FORCE_OFF */
} board_display_stats_t;

void board_display_stats(board_display_stats_t *out, bool reset_max);

/* LVGL runs on its own task: hold this lock around LVGL calls made from any other task.
 * LVGL callbacks and timers already run with it held. */
bool board_ui_lock(uint32_t timeout_ms);
void board_ui_unlock(void);

/* The touch controller, registered with LVGL (polled). */
esp_err_t board_touch_init(lv_display_t *disp, lv_indev_t **out_indev);

/* Touch diagnostics: polls, touches, bus errors; the longest gap between two polls shows how
 * long the UI task was busy (taps shorter than that can be missed). */
typedef struct {
    uint32_t polls;
    uint32_t presses;
    uint32_t errors;
    uint32_t max_gap_us;       /* since the last reset_max */
    uint32_t longest_press_ms; /* since the last reset_max */
    bool pressed;              /* a finger is on the screen now */
} board_touch_stats_t;

void board_touch_stats(board_touch_stats_t *out, bool reset_max);

/* Audio out: 16-bit stereo frames at sample_rate_hz; the board converts them for its output
 * (I2S amplifiers, a DAC). The amplifier starts off. */
esp_err_t board_audio_init(uint32_t sample_rate_hz);
esp_err_t board_audio_write(const int16_t *stereo_frames, size_t frames, TickType_t timeout);
void board_audio_amp(bool on);

/* The bus of the board's add-on I2C connector (board_info()->i2c; docs/features/35: the clock
 * module), created on first use - which may be before the display. NULL when the board has no
 * connector or the bus could not be set up. Devices added to it share it with the board's own
 * parts safely: the driver runs one transaction at a time. */
i2c_master_bus_handle_t board_i2c_ext(void);

/* Battery: the health task polls, and power_monitor reads the cached sample through
 * board_battery_read (a power_source_t reader). Boards without a battery report none. */
void board_battery_poll(void);
bool board_battery_read(void *ctx, power_sample_t *out);

#ifdef __cplusplus
}
#endif
