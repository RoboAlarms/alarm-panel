/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The RCP's radio and host link (adapted from ESP-IDF's examples/openthread/ot_rcp). The panel
 * side must match: the same baud rate (components/services/zigbee_service, CONFIG_ALARM_ZB_*).
 */
#pragma once

#include "esp_openthread_types.h"

#if !CONFIG_OPENTHREAD_RCP_UART
#error "The panel reaches the H2 over the socket's UART: CONFIG_OPENTHREAD_RCP_UART"
#endif

/* The socket's two UART pins. Unset, UART0 keeps its own pins - GPIO24 transmits and GPIO23
 * receives on the ESP32-H2 - which is how the Elecrow module is expected to be wired; if the
 * module's schematic says otherwise, set RCP_UART_PIN_MANUAL (sdkconfig.defaults). */
#if CONFIG_RCP_UART_PIN_MANUAL
#define RCP_UART_RX_PIN CONFIG_RCP_UART_RX_PIN
#define RCP_UART_TX_PIN CONFIG_RCP_UART_TX_PIN
#else
#define RCP_UART_RX_PIN UART_PIN_NO_CHANGE
#define RCP_UART_TX_PIN UART_PIN_NO_CHANGE
#endif

#define RCP_RADIO_CONFIG()                                                                         \
    {                                                                                              \
        .radio_mode = RADIO_MODE_NATIVE,                                                           \
    }

#define RCP_HOST_CONFIG()                                                                          \
    {                                                                                              \
        .host_connection_mode = HOST_CONNECTION_MODE_RCP_UART,                                     \
        .host_uart_config =                                                                        \
            {                                                                                      \
                .port = 0,                                                                         \
                .uart_config =                                                                     \
                    {                                                                              \
                        .baud_rate = CONFIG_RCP_UART_BAUD,                                         \
                        .data_bits = UART_DATA_8_BITS,                                             \
                        .parity = UART_PARITY_DISABLE,                                             \
                        .stop_bits = UART_STOP_BITS_1,                                             \
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,                                     \
                        .rx_flow_ctrl_thresh = 0,                                                  \
                        .source_clk = UART_SCLK_DEFAULT,                                           \
                    },                                                                             \
                .rx_pin = RCP_UART_RX_PIN,                                                         \
                .tx_pin = RCP_UART_TX_PIN,                                                         \
            },                                                                                     \
    }

#define RCP_PORT_CONFIG()                                                                          \
    {                                                                                              \
        .storage_partition_name = "nvs",                                                           \
        .netif_queue_size = 10,                                                                    \
        .task_queue_size = 10,                                                                     \
    }
