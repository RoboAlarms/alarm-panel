/*
 * board.c - the ESP32-S3-DevKitC-1's side of the board contract (board_api.h).
 *
 * A headless board: no display, touch, speaker or battery, so those calls answer
 * ESP_ERR_NOT_SUPPORTED (or nothing measured) and the panel runs without a UI. What it has is
 * the BOOT button (setup recovery, docs/features/31 headless-setup.md) and the add-on I2C bus
 * for a clock module (docs/features/35), created on first use.
 */
#include "board.h"

#include "board_desc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "board";

const board_info_t *board_info(void)
{
    return &board_esp32s3_devkitc_desc;
}

const char *board_name(void)
{
    return board_esp32s3_devkitc_desc.name;
}

esp_err_t board_power_init(void)
{
    /* The BOOT button has its pull-up on the board; the internal one as well, for clones
     * without it. */
    const gpio_config_t boot = {.pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
                                .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&boot);
    ESP_LOGI(TAG, "%s: headless", board_name());
    return ESP_OK;
}

bool board_boot_button_down(void)
{
    return gpio_get_level(BOARD_BOOT_BUTTON_GPIO) == 0;
}

/* ---- no screen ---- */

esp_err_t board_backlight_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_backlight_set(uint8_t percent)
{
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    if (out_disp != NULL) {
        *out_disp = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void board_display_stats(board_display_stats_t *out, bool reset_max)
{
    (void)reset_max;
    *out = (board_display_stats_t){0};
}

bool board_ui_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true; /* no LVGL task */
}

void board_ui_unlock(void) {}

esp_err_t board_touch_init(lv_display_t *disp, lv_indev_t **out_indev)
{
    (void)disp;
    if (out_indev != NULL) {
        *out_indev = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void board_touch_stats(board_touch_stats_t *out, bool reset_max)
{
    (void)reset_max;
    *out = (board_touch_stats_t){0};
}

/* ---- no speaker ---- */

esp_err_t board_audio_init(uint32_t sample_rate_hz)
{
    (void)sample_rate_hz;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_audio_write(const int16_t *stereo_frames, size_t frames, TickType_t timeout)
{
    (void)stereo_frames;
    (void)frames;
    (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}

void board_audio_amp(bool on)
{
    (void)on;
}

/* ---- the add-on I2C bus (IO8 SDA, IO9 SCL) ---- */

i2c_master_bus_handle_t board_i2c_ext(void)
{
    static i2c_master_bus_handle_t s_bus;
    if (s_bus == NULL) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = 0,
            .sda_io_num = BOARD_I2C_SDA_GPIO,
            .scl_io_num = BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) {
            ESP_LOGW(TAG, "add-on I2C bus: not set up");
            s_bus = NULL;
        }
    }
    return s_bus;
}

/* ---- no battery ---- */

void board_battery_poll(void) {}

bool board_battery_read(void *ctx, power_sample_t *out)
{
    (void)ctx;
    (void)out;
    return false; /* the power monitor reports "unknown" */
}
