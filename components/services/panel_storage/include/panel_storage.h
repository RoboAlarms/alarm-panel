#pragma once
#include "board_info/board_info.h"
#include "esp_err.h"
#include "storage.h"

/* Call before net_wifi_start: installs the card's slot once, avoiding concurrent controller
 * init. sd is the board's wiring (board_info()->sd): SDMMC 1-bit or an SPI bus. On an SPI bus
 * that another device shares, call it before that device starts (it sets the bus up with
 * DMA). A board without a slot (BOARD_SD_NONE) reports the interface as unavailable. */
esp_err_t panel_storage_start(const board_sd_t *sd);
void panel_storage_status(storage_sd_t *out);
/* The console's `storage` command (with CONFIG_ALARM_SERVICE_CONSOLE). */
esp_err_t panel_storage_console_register(void);
void panel_storage_flash(storage_flash_t *out);
/* User is a locally authenticated system master/installer. Authority and idle state are
 * checked on the alarm task again when the worker executes. No credentials are stored. */
esp_err_t panel_storage_request(storage_action_t action, uint32_t generation, uint8_t user);
/* Future filesystem clients: worker tasks only. Close/flush all files before release.
 * Maintenance refuses an active lease. Never retain paths/FILEs past the lease lifetime. */
bool panel_storage_acquire(const char **path);
void panel_storage_release(void);
