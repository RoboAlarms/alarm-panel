/*
 * rf_capture.c - RMT capture of the receiver's line (see rf_capture.h).
 */
#include "rf_rx/rf_capture.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "rf_capture";

/* The RMT's idle threshold is 15 bits of ticks (RMT_LL_MAX_IDLE_VALUE): at 1 us, 32.767 ms. */
_Static_assert((uint64_t)RF_CAPTURE_IDLE_US * RF_CAPTURE_RESOLUTION_HZ / 1000000u <= 32767u,
               "the reception's idle fits the RMT's idle counter");

/* Chips with ping-pong receive (the P4) hand over every half-buffer while a burst goes on
 * (partial receive), so two memory blocks do. The classic ESP32 has none: it hands a reception
 * over only when it ends (or its memory is full), so the channel takes four blocks - about ten
 * EV1527 frames - and a longer burst continues as the next reception. */
#if SOC_RMT_SUPPORT_RX_PINGPONG
#define RF_CAPTURE_PARTIAL     true
#define RF_CAPTURE_MEM_SYMBOLS 96u
#else
#define RF_CAPTURE_PARTIAL     false
#define RF_CAPTURE_MEM_SYMBOLS 256u
#endif

/* A symbol of two zero-length halves in the ring marks the end of a reception (a gap). */
static inline bool is_gap(const rmt_symbol_word_t *s)
{
    return s->duration0 == 0u && s->duration1 == 0u;
}

static inline uint32_t ring_used(const rf_capture_t *cap)
{
    return cap->head - cap->tail;
}

/* Interrupt context: no logging, no blocking. */
static bool IRAM_ATTR on_receive(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata,
                                 void *user_ctx)
{
    (void)chan;
    rf_capture_t *cap = user_ctx;
    BaseType_t woken = pdFALSE;
    const uint32_t room = RF_CAPTURE_RING_SYMBOLS - ring_used(cap);
    size_t n = edata->num_symbols;
    cap->received += (uint32_t)n; /* before the ring's room clips it (the storm guard) */
    if (n + 1u > room) {
        cap->overruns += (uint32_t)(n + 1u - room);
        n = room > 1u ? room - 1u : 0u;
    }
    for (size_t i = 0; i < n; i++) {
        cap->ring[cap->head % RF_CAPTURE_RING_SYMBOLS] = edata->received_symbols[i];
        cap->head++;
    }
    cap->symbols += (uint32_t)n;
    if (edata->flags.is_last) {
        cap->receptions++;
        if (ring_used(cap) < RF_CAPTURE_RING_SYMBOLS) {
            rmt_symbol_word_t gap;
            gap.val = 0u;
            cap->ring[cap->head % RF_CAPTURE_RING_SYMBOLS] = gap;
            cap->head++;
        }
        /* Listen again at once: the next transmission may follow within a frame. */
        const rmt_receive_config_t again = {.signal_range_min_ns = RF_CAPTURE_MIN_PULSE_NS,
                                            .signal_range_max_ns = RF_CAPTURE_IDLE_US * 1000u,
                                            .flags.en_partial_rx = RF_CAPTURE_PARTIAL};
        rmt_receive(cap->chan, cap->rx, RF_CAPTURE_RX_SYMBOLS * sizeof(rmt_symbol_word_t),
                    &again);
    }
    if (cap->waker != NULL) {
        vTaskNotifyGiveFromISR(cap->waker, &woken);
    }
    return woken == pdTRUE;
}

esp_err_t rf_capture_start(rf_capture_t *cap, int gpio, bool invert, TaskHandle_t waker)
{
    memset(cap, 0, sizeof(*cap));
    cap->gpio = gpio;
    cap->invert = invert;
    cap->waker = waker;
    cap->rx = heap_caps_calloc(RF_CAPTURE_RX_SYMBOLS, sizeof(rmt_symbol_word_t),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    cap->ring = heap_caps_calloc(RF_CAPTURE_RING_SYMBOLS, sizeof(rmt_symbol_word_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (cap->rx == NULL || cap->ring == NULL) {
        rf_capture_stop(cap);
        return ESP_ERR_NO_MEM;
    }
    const rmt_rx_channel_config_t cfg = {
        .gpio_num = (gpio_num_t)gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RF_CAPTURE_RESOLUTION_HZ,
        /* No DMA; the memory the chip's receive style needs (RF_CAPTURE_MEM_SYMBOLS). */
        .mem_block_symbols = RF_CAPTURE_MEM_SYMBOLS,
        .intr_priority = 0,
        .flags = {.invert_in = invert ? 1u : 0u, .with_dma = 0u, .allow_pd = 0u},
    };
    esp_err_t ret = rmt_new_rx_channel(&cfg, &cap->chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "no RMT receive channel on GPIO %d: %s", gpio, esp_err_to_name(ret));
        rf_capture_stop(cap);
        return ret;
    }
    const rmt_rx_event_callbacks_t cbs = {.on_recv_done = on_receive};
    ESP_GOTO_ON_ERROR(rmt_rx_register_event_callbacks(cap->chan, &cbs, cap), fail, TAG, "callback");
    ESP_GOTO_ON_ERROR(rmt_enable(cap->chan), fail, TAG, "enable");
    const rmt_receive_config_t rx = {.signal_range_min_ns = RF_CAPTURE_MIN_PULSE_NS,
                                     .signal_range_max_ns = RF_CAPTURE_IDLE_US * 1000u,
                                     .flags.en_partial_rx = RF_CAPTURE_PARTIAL};
    ESP_GOTO_ON_ERROR(rmt_receive(cap->chan, cap->rx, RF_CAPTURE_RX_SYMBOLS * sizeof(rmt_symbol_word_t), &rx),
                      fail, TAG, "receive");
    /* A pin with nothing on it reads low, not the air's static: an unwired data pin must look
     * like no receiver at all (the service tells a module from an empty pin by its edges).
     * The ESP32's input-only pins (34-39) have no internal pulls: there the board's wiring
     * keeps the line quiet, or a pull-down is fitted with the receiver (docs/features/25). */
    if (GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        gpio_pulldown_en((gpio_num_t)gpio);
    }
    ESP_LOGI(TAG, "capturing on GPIO %d%s", gpio, invert ? " (inverted)" : "");
    return ESP_OK;
fail:
    rf_capture_stop(cap);
    return ret;
}

esp_err_t rf_capture_pause(rf_capture_t *cap, bool pause)
{
    if (cap->chan == NULL || cap->paused == pause) {
        return ESP_OK;
    }
    esp_err_t err;
    if (pause) {
        err = rmt_disable(cap->chan);
    } else {
        err = rmt_enable(cap->chan);
        if (err == ESP_OK) {
            const rmt_receive_config_t rx = {.signal_range_min_ns = RF_CAPTURE_MIN_PULSE_NS,
                                             .signal_range_max_ns = RF_CAPTURE_IDLE_US * 1000u,
                                             .flags.en_partial_rx = RF_CAPTURE_PARTIAL};
            err = rmt_receive(cap->chan, cap->rx,
                              RF_CAPTURE_RX_SYMBOLS * sizeof(rmt_symbol_word_t), &rx);
        }
    }
    if (err == ESP_OK) {
        cap->paused = pause;
    }
    return err;
}

void rf_capture_stop(rf_capture_t *cap)
{
    if (cap->chan != NULL) {
        rmt_disable(cap->chan);
        rmt_del_channel(cap->chan);
        cap->chan = NULL;
    }
    heap_caps_free(cap->rx);
    heap_caps_free(cap->ring);
    cap->rx = NULL;
    cap->ring = NULL;
}

bool rf_capture_next(rf_capture_t *cap, rf_capture_pulse_t *out)
{
    if (ring_used(cap) == 0u) {
        return false;
    }
    const rmt_symbol_word_t *s = &cap->ring[cap->tail % RF_CAPTURE_RING_SYMBOLS];
    if (is_gap(s)) {
        out->high = false;
        out->width_us = 0u;
        cap->tail++;
        cap->half = 0;
        return true;
    }
    /* Each symbol is two pulses; 'half' remembers which one is next. */
    if (cap->half == 0u) {
        out->high = s->level0 != 0u;
        out->width_us = s->duration0;
        cap->half = 1;
        return true;
    }
    out->high = s->level1 != 0u;
    out->width_us = s->duration1; /* 0 = the level outlasted the capture: a gap */
    cap->half = 0;
    cap->tail++;
    return true;
}

bool rf_capture_line_high(const rf_capture_t *cap)
{
    if (cap->chan == NULL) {
        return false;
    }
    /* The pad itself: the RMT's invert_in applies inside the GPIO matrix, not here. */
    return (gpio_get_level((gpio_num_t)cap->gpio) != 0) != cap->invert;
}
