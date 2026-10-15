/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The ESP32-H2's radio co-processor for the panel's Zigbee (docs/features/20). Adapted from
 * ESP-IDF's OpenThread RCP example (examples/openthread/ot_rcp, public domain / CC0): the host
 * link is fixed to the UART that reaches the CrowPanel's wireless socket, and the example's
 * optional console and external-coexistence commands are left out - the H2 does nothing but
 * pass frames to and from the panel, which runs the Zigbee stack itself.
 */
#include "esp_event.h"
#include "esp_openthread.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "rcp_config.h"

#if !SOC_IEEE802154_SUPPORTED
#error "The RCP needs a chip with an IEEE 802.15.4 radio (the ESP32-H2)"
#endif

void app_main(void)
{
    /* Two eventfds: the OpenThread task's queue and the radio driver. */
    const esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 2};

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    static esp_openthread_config_t config = {
        .netif_config = {0},
        .platform_config =
            {
                .radio_config = RCP_RADIO_CONFIG(),
                .host_config = RCP_HOST_CONFIG(),
                .port_config = RCP_PORT_CONFIG(),
            },
    };
    ESP_ERROR_CHECK(esp_openthread_start(&config));
}
