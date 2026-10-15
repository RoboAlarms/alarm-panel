/*
 * panel_console.h - serial service console (UART0 / USB): status, zone simulator,
 * virtual keypad, troubles, Wi-Fi and time zone. Type "help" for the command list.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the REPL task. Call after alarm_service_start(). */
esp_err_t panel_console_start(void);

#ifdef __cplusplus
}
#endif
