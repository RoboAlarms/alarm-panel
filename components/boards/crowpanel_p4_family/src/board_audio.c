/*
 * board_audio.c - I2S1 output (16-bit stereo) to the board's sound parts: the amplifiers straight
 * on I2S (two NS4168 on the 7" V1.1; the 10.1" V1.2 plays the same way), or an ES8311 codec
 * feeding an NS4263B amplifier where the 10.1"'s schematic draws one
 * (CONFIG_BOARD_CROWPANEL_P4_ES8311). The amplifier enable (GPIO30) is active low on all and
 * stays off until silence is flowing, which prevents the power-on pop Elecrow warns about.
 */
#include "crowpanel_p4.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#if CONFIG_BOARD_CROWPANEL_P4_ES8311
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"
#endif

static const char *TAG = "board_audio";

static i2s_chan_handle_t s_tx;

void board_audio_amp(bool on)
{
    gpio_set_level(BOARD_AUDIO_AMP_EN_GPIO, on ? 0 : 1);
}

#if CONFIG_BOARD_CROWPANEL_P4_ES8311
/* The ES8311 on I2C0 (address BOARD_AUDIO_CODEC_ADDR), clocked by the I2S MCLK, as a DAC only.
 * Its DAC stays at 0 dB: the NS4263B runs from the same 3.3 V, so full scale out of the codec is
 * full scale into the amplifier, and the sounder sets the volume in the samples as on the 7".
 * Espressif's esp_codec_dev does the register work. A board with no codec answering keeps plain
 * I2S, as on the 7" (the 10.1" this was tried on, and Elecrow's V1.2 examples), and GPIO24 goes
 * back to the board: the PDM microphone's clock there, not to be driven at 8 MHz. */
static void codec_start(uint32_t sample_rate_hz)
{
    i2c_master_bus_handle_t bus = board_i2c_ext();
    if (bus == NULL || i2c_master_probe(bus, BOARD_AUDIO_CODEC_ADDR, 50) != ESP_OK) {
        gpio_reset_pin(BOARD_AUDIO_MCLK_GPIO);
        ESP_LOGI(TAG, "no ES8311 at 0x%02x: plain I2S", BOARD_AUDIO_CODEC_ADDR);
        return;
    }
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = BOARD_AUDIO_CODEC_ADDR << 1, /* esp_codec_dev takes the 8-bit address */
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl == NULL) {
        ESP_LOGE(TAG, "ES8311 control: no sound");
        return;
    }
    es8311_codec_cfg_t cfg = {
        .ctrl_if = ctrl,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, /* GPIO30 is board_audio_amp()'s */
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 3.3f, .codec_dac_voltage = 3.3f},
        .mclk_div = 256, /* I2S_STD_CLK_DEFAULT_CONFIG's MCLK multiple */
    };
    const audio_codec_if_t *codec = es8311_codec_new(&cfg);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate_hz,
    };
    if (codec == NULL || codec->set_fs(codec, &fs) != ESP_CODEC_DEV_OK ||
        codec->enable(codec, true) != ESP_CODEC_DEV_OK ||
        codec->set_vol(codec, 0.0f) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 setup failed: no sound");
        return;
    }
    ESP_LOGI(TAG, "ES8311 codec ready");
}
#endif

esp_err_t board_audio_init(uint32_t sample_rate_hz)
{
    if (s_tx != NULL) {
        return ESP_OK;
    }
    board_audio_amp(false);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4; /* 4 x 256 frames: ~32 ms latency at 32 kHz */
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true; /* play silence on underrun */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "I2S channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
#if CONFIG_BOARD_CROWPANEL_P4_ES8311
            .mclk = BOARD_AUDIO_MCLK_GPIO, /* the codec runs from it: before it is set up */
#else
            .mclk = I2S_GPIO_UNUSED,
#endif
            .bclk = BOARD_AUDIO_BCLK_GPIO,
            .ws = BOARD_AUDIO_LRCLK_GPIO,
            .dout = BOARD_AUDIO_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "I2S std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S enable");

    /* Prime with silence before the amplifiers are ever enabled. */
    static const int16_t silence[2 * 256];
    size_t written = 0;
    i2s_channel_write(s_tx, silence, sizeof(silence), &written, pdMS_TO_TICKS(50));
#if CONFIG_BOARD_CROWPANEL_P4_ES8311
    codec_start(sample_rate_hz);
#endif
    ESP_LOGI(TAG, "I2S audio ready (%u Hz)", (unsigned)sample_rate_hz);
    return ESP_OK;
}

esp_err_t board_audio_write(const int16_t *stereo_frames, size_t frames, TickType_t timeout)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t written = 0;
    return i2s_channel_write(s_tx, stereo_frames, frames * 2u * sizeof(int16_t), &written, timeout);
}
