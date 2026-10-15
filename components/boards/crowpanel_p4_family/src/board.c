/*
 * board.c - power rails and backlight for the CrowPanel Advanced P4 boards. Which board it is
 * (board_info(), board_name()) is each board's own (board_<board>/src/board_id.c).
 */
#include "crowpanel_p4.h"

#include "board_internal.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "board";

static esp_ldo_channel_handle_t s_ldo_phy;
static esp_ldo_channel_handle_t s_ldo_aux;
static bool s_backlight_ready;
static uint8_t s_requested;  /* what the UI asked for */
static bool s_held_off;      /* the flicker guard holds it off */
static bool s_lit;           /* the backlight is on now */
static bool s_reveal_wait;   /* asked to light from dark: waits for a fresh picture */
static uint32_t s_reveal_id; /* ... for this request */
static bool s_reveal_go;     /* the panel shows one */
static bool s_shut_down;     /* a restart is under way */
/* The UI sets the level from the LVGL task and the flicker guard holds it off from the timer
 * task, on two cores: without this lock the two writes could cross and leave the backlight on
 * while the guard believed it was holding it off (P4-012). */
static SemaphoreHandle_t s_backlight_lock;

esp_err_t board_power_init(void)
{
    /* The backlight off first: a panic or watchdog restart resets only the CPUs, and the PWM
     * keeps its duty, lighting whatever the panel holds until the display starts (B023). */
    if (board_backlight_init() != ESP_OK) {
        ESP_LOGW(TAG, "backlight not ready yet"); /* start_display tries again; headless if not */
    }
    if (s_ldo_phy == NULL) {
        const esp_ldo_channel_config_t phy = {
            .chan_id = BOARD_LDO_MIPI_PHY_CHAN,
            .voltage_mv = BOARD_LDO_MIPI_PHY_MV,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&phy, &s_ldo_phy), TAG, "MIPI PHY LDO");
    }
    if (s_ldo_aux == NULL) {
        const esp_ldo_channel_config_t aux = {
            .chan_id = BOARD_LDO_AUX_CHAN,
            .voltage_mv = BOARD_LDO_AUX_MV,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&aux, &s_ldo_aux), TAG, "aux LDO");
    }

    /* Keep the audio amplifiers off (active low enable) until silence is flowing. */
    const gpio_config_t amp = {
        .pin_bit_mask = 1ULL << BOARD_AUDIO_AMP_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "amp gpio");
    gpio_set_level(BOARD_AUDIO_AMP_EN_GPIO, 1);
    ESP_LOGI(TAG, "%s: power rails up", board_name());
    return ESP_OK;
}

esp_err_t board_backlight_init(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }
    const ledc_timer_config_t timer = {
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .freq_hz = 30000,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    if (s_backlight_lock == NULL) {
        s_backlight_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_backlight_lock != NULL, ESP_ERR_NO_MEM, TAG, "backlight lock");
    }
    s_backlight_ready = true;
    return ESP_OK;
}

static esp_err_t backlight_apply(void)
{
    uint8_t percent = s_requested;
#if CONFIG_BOARD_BACKLIGHT_FORCE_OFF
    percent = 0;
#endif
    if (s_held_off || s_shut_down) {
        percent = 0;
    }
    /* From dark, light only once the panel shows a picture drawn since: until then it shows
     * what the frame buffer held, an earlier screen (docs/bugs/B023). A change of level while
     * lit applies at once. */
    if (percent != 0u && !s_lit && !s_reveal_go) {
        if (!s_reveal_wait) {
            s_reveal_wait = true;
            s_reveal_id = board_display_reveal_request();
        }
        return ESP_OK;
    }
    s_reveal_wait = false;
    s_reveal_go = false;
    s_lit = percent != 0u;
    /* Elecrow's mapping: the MT9201 driver needs a minimum duty to light up. */
    const uint32_t duty = (percent == 0) ? 0u : (uint32_t)percent * 18u + 200u;
#if CONFIG_BOARD_DISPLAY_DIAG
    if (duty != 0u) {
        board_display_diag_lit();
    }
#endif
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t board_backlight_set(uint8_t percent)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_backlight_lock, portMAX_DELAY);
    s_requested = percent > 100 ? 100 : percent;
    const esp_err_t err = backlight_apply();
    xSemaphoreGive(s_backlight_lock);
    return err;
}

void board_backlight_hold_off(bool off)
{
    if (!s_backlight_ready) {
        return;
    }
    xSemaphoreTake(s_backlight_lock, portMAX_DELAY);
    if (off != s_held_off) {
        s_held_off = off;
        backlight_apply();
    }
    xSemaphoreGive(s_backlight_lock);
}

uint8_t board_backlight_requested(void)
{
    return s_requested;
}

void board_backlight_reveal(uint32_t id)
{
    if (!s_backlight_ready) {
        return;
    }
    xSemaphoreTake(s_backlight_lock, portMAX_DELAY);
    if (s_reveal_wait && id == s_reveal_id) {
        s_reveal_go = true;
        backlight_apply();
    }
    xSemaphoreGive(s_backlight_lock);
}

void board_backlight_shutdown(void)
{
    if (!s_backlight_ready) {
        return;
    }
    /* Bounded: a restart doesn't wait on whoever holds the lock; the LEDC driver has its own. */
    const bool locked = xSemaphoreTake(s_backlight_lock, pdMS_TO_TICKS(100)) == pdTRUE;
    s_shut_down = true;
    backlight_apply();
    if (locked) {
        xSemaphoreGive(s_backlight_lock);
    }
}
