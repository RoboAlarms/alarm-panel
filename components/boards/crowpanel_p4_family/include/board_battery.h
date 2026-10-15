#pragma once
/* The CrowPanel's battery (STC sensing over I2C). board_battery_poll() and
 * board_battery_read() are the board contract's (board_api.h); this adds the raw diagnostic
 * the console shows. Only the health task polls; consumers copy a small cached record. */
#include "board_api.h"
#include "esp_err.h"

typedef struct {
    esp_err_t error;
    uint8_t raw[11];
    bool decoded;
    uint64_t sampled_ms;
} board_battery_diagnostic_t;
void board_battery_diagnostic(board_battery_diagnostic_t *out);
