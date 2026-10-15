#pragma once
/*
 * touch_xpt2046.h - the XPT2046 resistive touch controller on an SPI bus (docs/features/19).
 *
 * Reads one poll's raw samples (TOUCH_SAMPLES positions per axis and the two pressure
 * readings) as a touch_raw_t; turning them into a steady screen point is touch_calib's job
 * (components/core/touch_calib). The controller can share its bus with a display: the SPI
 * driver serialises the two, and the controller runs at its own slow clock.
 */
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "touch_calib/touch_calib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_XPT2046_CLOCK_HZ 2000000 /* the datasheet's 2.5 MHz at 2.7 V, with margin */

typedef struct {
    spi_device_handle_t dev;
    uint32_t errors; /* SPI transactions that failed */
} touch_xpt2046_t;

/* Adds the controller to an initialised SPI bus. */
esp_err_t touch_xpt2046_open(touch_xpt2046_t *t, spi_host_device_t host, int cs_gpio,
                             uint32_t clock_hz);

/* One poll: TOUCH_SAMPLES readings of each axis and the pressure pair, the controller left
 * powered down with its pen interrupt armed. */
esp_err_t touch_xpt2046_read(touch_xpt2046_t *t, touch_raw_t *out);

void touch_xpt2046_close(touch_xpt2046_t *t);

#ifdef __cplusplus
}
#endif
