/*
 * rf_tx.c - 433 MHz transmissions through the RMT peripheral (see rf_tx.h).
 *
 * A channel is made for each transmission and deleted after it: the receiver's RMT channel and
 * this one may want the same pin (a CC1101's GDO0), and transmissions are rare (a siren's codes).
 */
#include "rf_rx/rf_tx.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "rf_tx";

#define TX_RESOLUTION_HZ 1000000u /* 1 us per tick */
#define TX_MEM_SYMBOLS   48u      /* one channel's memory on every chip with an RMT */
#define TX_TAIL_US       1000u    /* the line low after the last pulse */

esp_err_t rf_tx_idle(int gpio)
{
    const gpio_config_t io = {.pin_bit_mask = 1ull << gpio,
                              .mode = GPIO_MODE_OUTPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE,
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)gpio, 0), TAG, "level");
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "config");
    return gpio_set_level((gpio_num_t)gpio, 0);
}

static uint16_t width_of(uint32_t p)
{
    uint32_t w = p & ~RF_TX_PULSE_HIGH;
    if (w > RF_TX_MAX_US) {
        w = RF_TX_MAX_US;
    }
    return (uint16_t)(w == 0u ? 1u : w);
}

esp_err_t rf_tx_send(int gpio, const uint32_t *pulses, size_t n, uint32_t timeout_ms)
{
    if (pulses == NULL || n == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t symbols = n / 2u + 1u;
    rmt_symbol_word_t *sym = heap_caps_calloc(symbols, sizeof(*sym), MALLOC_CAP_INTERNAL);
    if (sym == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t k = 0u;
    for (size_t i = 0; i < n; i += 2u) {
        sym[k].level0 = (pulses[i] & RF_TX_PULSE_HIGH) != 0u ? 1u : 0u;
        sym[k].duration0 = width_of(pulses[i]);
        if (i + 1u < n) {
            sym[k].level1 = (pulses[i + 1u] & RF_TX_PULSE_HIGH) != 0u ? 1u : 0u;
            sym[k].duration1 = width_of(pulses[i + 1u]);
        } else {
            sym[k].level1 = 0u;
            sym[k].duration1 = TX_TAIL_US;
        }
        k++;
    }
    esp_err_t ret = rf_tx_idle(gpio);
    rmt_channel_handle_t chan = NULL;
    rmt_encoder_handle_t enc = NULL;
    if (ret != ESP_OK) {
        goto done;
    }
    const rmt_tx_channel_config_t cfg = {
        .gpio_num = (gpio_num_t)gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = TX_RESOLUTION_HZ,
        .mem_block_symbols = TX_MEM_SYMBOLS,
        .trans_queue_depth = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_tx_channel(&cfg, &chan), done, TAG, "channel");
    rmt_copy_encoder_config_t ecfg;
    memset(&ecfg, 0, sizeof(ecfg));
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&ecfg, &enc), done, TAG, "encoder");
    ESP_GOTO_ON_ERROR(rmt_enable(chan), done, TAG, "enable");
    const rmt_transmit_config_t tx = {.loop_count = 0, .flags.eot_level = 0};
    ret = rmt_transmit(chan, enc, sym, k * sizeof(*sym), &tx);
    if (ret == ESP_OK) {
        ret = rmt_tx_wait_all_done(chan, (int)timeout_ms);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "a transmission did not end within %u ms", (unsigned)timeout_ms);
        }
    }
    (void)rmt_disable(chan);
done:
    if (chan != NULL) {
        (void)rmt_del_channel(chan);
    }
    if (enc != NULL) {
        (void)rmt_del_encoder(enc);
    }
    heap_caps_free(sym);
    (void)rf_tx_idle(gpio); /* no carrier after it, whatever happened */
    return ret;
}
