#include "crowpanel_p4.h"
#include "board_battery.h"
#include "esp_timer.h"
#include <string.h>

static i2c_master_dev_handle_t s_device;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static power_sample_t s_sample = {.battery_mv = -1, .battery_percent = -1};
static board_battery_diagnostic_t s_diag = {.error = ESP_ERR_INVALID_STATE};

void board_battery_poll(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000u;
    board_battery_diagnostic_t diag = {.error = ESP_ERR_INVALID_STATE, .sampled_ms = now};
    power_sample_t sample;
    (void)power_source_none(NULL, &sample);
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus != NULL) {
        if (s_device == NULL) {
            diag.error = i2c_master_probe(bus, 0x2f, 25);
            if (diag.error == ESP_OK) {
                const i2c_device_config_t config = {
                    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                    .device_address = 0x2f, .scl_speed_hz = 100000,
                };
                diag.error = i2c_master_bus_add_device(bus, &config, &s_device);
            }
        }
        if (s_device != NULL) {
            /* Match Elecrow's byte-at-a-time battery-register reads. No control
             * registers are written. Each bus transaction has a 25 ms bound. */
            for (uint8_t reg = 0; reg < sizeof(diag.raw); ++reg) {
                diag.error = i2c_master_transmit_receive(s_device, &reg, 1,
                                                       &diag.raw[reg], 1, 25);
                if (diag.error != ESP_OK) {
                    break;
                }
            }
            if (diag.error == ESP_OK) {
                diag.decoded = power_decode_stc(diag.raw, sizeof(diag.raw), &sample);
            }
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_sample = sample;
    s_diag = diag;
    portEXIT_CRITICAL(&s_lock);
}

bool board_battery_read(void *ctx, power_sample_t *out)
{
    (void)ctx;
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000u;
    portENTER_CRITICAL(&s_lock);
    *out = s_sample;
    const bool valid = s_diag.decoded && s_diag.error == ESP_OK &&
                       now >= s_diag.sampled_ms && now - s_diag.sampled_ms <= 10000u;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        (void)power_source_none(NULL, out);
    }
    return valid;
}

void board_battery_diagnostic(board_battery_diagnostic_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_diag;
    portEXIT_CRITICAL(&s_lock);
}
