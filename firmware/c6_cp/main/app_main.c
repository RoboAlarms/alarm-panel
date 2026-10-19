#include "espnow_bridge.h"
#include "nvs_flash.h"
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    espnow_bridge_start();
}
