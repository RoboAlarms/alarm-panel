#pragma once
/*
 * rtc_ds3231.h - a DS3231 real-time clock module on an I2C bus (docs/features/35).
 *
 * Only the bus work: finding the chip at 0x68 and reading or writing its registers in one
 * transaction each. Everything the registers mean is components/core/rtc_clock's. Every call
 * gives up after RTC_DS3231_TIMEOUT_MS, so a module that hangs the bus never holds the caller
 * (the touch controller shares the CrowPanel's bus) for longer than that.
 */
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTC_DS3231_CLOCK_HZ 100000u /* modules sit on a lead; the chip does 400 kHz */
#define RTC_DS3231_TIMEOUT_MS 50

/* Looks for a chip at 0x68 (an address probe: nothing is written). ESP_ERR_NOT_FOUND when
 * nothing acknowledges - no module, the usual case. On success the device is on the bus. */
esp_err_t rtc_ds3231_open(i2c_master_bus_handle_t bus);
/* Reads len registers from reg (at most 19). ESP_ERR_INVALID_STATE before a successful open. */
esp_err_t rtc_ds3231_read(uint8_t reg, uint8_t *out, size_t len);
/* Writes len registers from reg in one transaction (at most 19). */
esp_err_t rtc_ds3231_write(uint8_t reg, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
