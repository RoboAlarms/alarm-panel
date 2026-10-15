/*
 * rtc_ds3231.c - the DS3231 on an I2C bus (docs/features/35). See rtc_ds3231.h.
 */
#include "rtc_ds3231/rtc_ds3231.h"

#include <string.h>

#include "rtc_clock/ds3231.h"

static i2c_master_dev_handle_t s_dev;

esp_err_t rtc_ds3231_open(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dev != NULL) {
        return ESP_OK;
    }
    const esp_err_t err = i2c_master_probe(bus, DS3231_I2C_ADDR, RTC_DS3231_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err; /* ESP_ERR_NOT_FOUND: no acknowledgement */
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = RTC_DS3231_CLOCK_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

esp_err_t rtc_ds3231_read(uint8_t reg, uint8_t *out, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0u || len > DS3231_REG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, RTC_DS3231_TIMEOUT_MS);
}

esp_err_t rtc_ds3231_write(uint8_t reg, const uint8_t *data, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0u || len > DS3231_REG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The register pointer and the data in one transaction: the chip copies the time registers
     * into its counters when the write ends, all together. */
    uint8_t buf[1u + DS3231_REG_COUNT];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1u, RTC_DS3231_TIMEOUT_MS);
}
