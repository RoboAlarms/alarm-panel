/*
 * rf_capture.h - the receiver's data line as pulse widths, through the RMT peripheral
 * (docs/features/25, RF-002).
 *
 * An RMT receive channel timestamps every edge on the pin at 1 us resolution, in the
 * background, with the hardware glitch filter dropping anything shorter than a real pulse.
 * Its interrupt hands the symbols to a ring buffer (internal RAM, as everything an interrupt
 * touches; the interrupt itself waits while flash is written, see the README) and wakes the
 * receiver's task with a plain notification (a count, not bits), which feeds them to the
 * decoder.
 *
 * The line idling for RF_CAPTURE_IDLE_US ends a reception: that idle is what separates one
 * sensor's transmission from the next, and the decoder is told about it as a gap.
 */
#ifndef RF_RX_RF_CAPTURE_H
#define RF_RX_RF_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_CAPTURE_RESOLUTION_HZ 1000000u /* 1 us per tick */
#define RF_CAPTURE_MIN_PULSE_NS  3000u    /* the hardware glitch filter: 255 ticks of the RMT
                                           * group clock (80 MHz) at most, so ~3 us; the
                                           * decoder drops and stitches anything below 40 us */
#define RF_CAPTURE_STORM_EDGES   20000u   /* more edges a second than any receiver's honest
                                           * noise (a clock on the pin): the capture pauses */
#define RF_CAPTURE_IDLE_US       30000u   /* the line quiet this long: the transmission ended
                                           * (RF_RECEPTION_IDLE_US: longer than any sync at its
                                           * slowest T, B025; the RMT counts at most 32767 ticks) */
#define RF_CAPTURE_RX_SYMBOLS    256u     /* one reception's buffer (a symbol is two pulses) */
#define RF_CAPTURE_RING_SYMBOLS  1024u    /* the task's backlog before symbols are dropped */

typedef struct {
    bool high;
    uint32_t width_us; /* 0 = the level went on past the capture: a gap */
} rf_capture_pulse_t;

typedef struct {
    rmt_channel_handle_t chan;
    rmt_symbol_word_t *rx;   /* the reception buffer (internal RAM) */
    rmt_symbol_word_t *ring; /* interrupt -> task */
    volatile uint32_t head;  /* written by the interrupt */
    volatile uint32_t tail;  /* read by the task */
    TaskHandle_t waker;
    int gpio;                /* the pin being captured */
    bool invert;             /* the receiver's line idles high */
    uint8_t half;            /* which pulse of the symbol at 'tail' is next */
    bool paused;
    /* for the console (RF-002 diagnostics) */
    uint32_t receptions;
    uint32_t symbols;  /* symbols queued for the task */
    uint32_t overruns; /* symbols dropped because the task fell behind */
    /* every symbol the RMT handed over, queued or dropped: the line's input rate, which the
     * storm guard measures (a full ring must not hide a flood); free-running, it wraps */
    volatile uint32_t received;
} rf_capture_t;

/* Starts capturing on the pin; 'waker' gets a task notification whenever symbols arrive
 * (vTaskNotifyGiveFromISR: the value counts wake-ups, so the task must not keep bits in it). */
esp_err_t rf_capture_start(rf_capture_t *cap, int gpio, bool invert, TaskHandle_t waker);
/* Pauses or resumes a running capture (a storm of edges, or a receiver being reconfigured). */
esp_err_t rf_capture_pause(rf_capture_t *cap, bool pause);
void rf_capture_stop(rf_capture_t *cap);

/* The next pulse, in order; false when the ring is empty. A pulse with width 0 is a gap: the
 * level lasted past the capture (the transmission ended). */
bool rf_capture_next(rf_capture_t *cap, rf_capture_pulse_t *out);

/* The line's level right now, as the decoder sees it (after 'invert'). A level that holds makes
 * no pulses at all - a reception ends after RF_CAPTURE_IDLE_US - so this is the only way to see
 * a carrier holding a receiver module's line high (RF-005). false while not capturing. */
bool rf_capture_line_high(const rf_capture_t *cap);

#ifdef __cplusplus
}
#endif

#endif /* RF_RX_RF_CAPTURE_H */
