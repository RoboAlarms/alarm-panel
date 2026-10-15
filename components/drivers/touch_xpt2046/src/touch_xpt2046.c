/*
 * touch_xpt2046.c - XPT2046 over SPI (see touch_xpt2046.h).
 *
 * Every conversion is a control byte followed by two bytes that carry the 12-bit result
 * (MSB first, three bits late). Differential reference mode, 12 bits: X 0xD0, Y 0x90, Z1 0xB0,
 * Z2 0xC0. The low two bits keep the ADC powered between the conversions of a poll (01); the
 * last conversion powers it down (00), which re-arms the pen interrupt.
 */
#include "touch_xpt2046/touch_xpt2046.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "xpt2046";

#define CMD_X  0xD0u
#define CMD_Y  0x90u
#define CMD_Z1 0xB0u
#define CMD_Z2 0xC0u
#define ADC_ON 0x01u

esp_err_t touch_xpt2046_open(touch_xpt2046_t *t, spi_host_device_t host, int cs_gpio,
                             uint32_t clock_hz)
{
    memset(t, 0, sizeof(*t));
    const spi_device_interface_config_t cfg = {
        .clock_speed_hz = (int)clock_hz,
        .mode = 0,
        .spics_io_num = cs_gpio,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(host, &cfg, &t->dev), TAG, "add device");
    return ESP_OK;
}

static esp_err_t convert(touch_xpt2046_t *t, uint8_t cmd, uint16_t *out)
{
    spi_transaction_t tr = {
        .length = 24,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {cmd, 0, 0, 0},
    };
    const esp_err_t err = spi_device_polling_transmit(t->dev, &tr);
    if (err != ESP_OK) {
        t->errors++;
        return err;
    }
    *out = (uint16_t)((((uint16_t)tr.rx_data[1] << 8) | tr.rx_data[2]) >> 3) & 0x0FFFu;
    return ESP_OK;
}

esp_err_t touch_xpt2046_read(touch_xpt2046_t *t, touch_raw_t *out)
{
    if (t->dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The whole poll at once: the display's transfers wait, and the samples are close in time. */
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(t->dev, portMAX_DELAY), TAG, "bus");
    esp_err_t err = convert(t, CMD_Z1 | ADC_ON, &out->z1);
    if (err == ESP_OK) {
        err = convert(t, CMD_Z2 | ADC_ON, &out->z2);
    }
    for (uint32_t i = 0; i < TOUCH_SAMPLES && err == ESP_OK; i++) {
        err = convert(t, CMD_X | ADC_ON, &out->x[i]);
        if (err == ESP_OK) {
            const bool last = i + 1u == TOUCH_SAMPLES;
            err = convert(t, last ? CMD_Y : (uint8_t)(CMD_Y | ADC_ON), &out->y[i]);
        }
    }
    spi_device_release_bus(t->dev);
    return err;
}

void touch_xpt2046_close(touch_xpt2046_t *t)
{
    if (t->dev != NULL) {
        spi_bus_remove_device(t->dev);
        t->dev = NULL;
    }
}
