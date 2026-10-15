/*
 * board.c - the LCDWiki E32R40T's side of the board contract (board_api.h): identification,
 * power-up, backlight, and the parts whose drivers are still to come.
 *
 * The display is board_display.c, the touch board_touch.c. At power-up the backlight, the
 * amplifier and the RGB LED are driven off, so the display's uninitialised memory is never lit
 * (CONTRIBUTING.md, "Display safety") and the speaker never hums. The backlight is a 25 kHz PWM
 * (docs/features/19 CYD-011: 20 kHz or more, no visible flicker), lit only when the UI asks after
 * its first frame. Audio (the DAC) and the battery ADC are still to come (docs/features/19 Phase 2):
 * they answer ESP_ERR_NOT_SUPPORTED and the panel carries on without them.
 */
#include "board.h"

#include "board_desc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

#define BACKLIGHT_HZ   25000
#define BACKLIGHT_BITS LEDC_TIMER_10_BIT
#define BACKLIGHT_MAX  1023u

static bool s_backlight_ready;
static uint8_t s_backlight; /* what the UI asked for */
static bool s_held_on;      /* the touch calibration's targets are showing */

#define HOLD_ON_PERCENT 70u

static esp_err_t backlight_apply(void)
{
    uint32_t percent = s_backlight;
    if (s_held_on && percent < HOLD_ON_PERCENT) {
        percent = HOLD_ON_PERCENT;
    }
    const uint32_t duty = percent * BACKLIGHT_MAX / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

const board_info_t *board_info(void)
{
    return &board_e32r40t_desc;
}

const char *board_name(void)
{
    return board_e32r40t_desc.name;
}

static void drive(int gpio, int level)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&io) == ESP_OK) {
        gpio_set_level(gpio, level);
    }
}

esp_err_t board_power_init(void)
{
    /* Dark until the UI's first full frame: the PWM at 0 % from the start (a plain GPIO first
     * would leave the pin reserved and LEDC complaining when it takes it over). */
    if (board_backlight_init() != ESP_OK) {
        drive(BOARD_LCD_BACKLIGHT_GPIO, 0);
    }
    drive(BOARD_AUDIO_AMP_EN_GPIO, 1);  /* amplifier off (low = on) */
    drive(BOARD_LED_RED_GPIO, 1);       /* common anode: high = off */
    drive(BOARD_LED_GREEN_GPIO, 1);
    drive(BOARD_LED_BLUE_GPIO, 1);
    ESP_LOGI(TAG, "%s: backlight, amplifier and LED off", board_name());
    return ESP_OK;
}

esp_err_t board_backlight_init(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BACKLIGHT_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0, /* still off */
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t board_backlight_set(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_backlight_ready, ESP_ERR_INVALID_STATE, TAG, "backlight not set up");
    s_backlight = percent > 100u ? 100u : percent;
    return backlight_apply();
}

void board_backlight_hold_on(bool on)
{
    if (s_backlight_ready && on != s_held_on) {
        s_held_on = on;
        backlight_apply();
    }
}

uint8_t board_backlight_requested(void)
{
    return s_backlight;
}

/* ---- audio: docs/features/19 Phase 2 (DAC channel 1 on IO26, FM8002E enable on IO4) ---- */

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

/* ---- battery: docs/features/19 Phase 2 (ADC1 on IO34) ---- */

/* Its I2C header's pins serve as wired zone terminals (board_desc.c): no add-on bus. */
i2c_master_bus_handle_t board_i2c_ext(void)
{
    return NULL;
}

void board_battery_poll(void) {}

bool board_battery_read(void *ctx, power_sample_t *out)
{
    (void)ctx;
    (void)out;
    return false; /* nothing measured yet: the power monitor reports "unknown" */
}
