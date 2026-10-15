/* Optional removable storage. Only this worker mounts/formats; UI reads snapshots.
 * SDMMC slot 0 is retained for the boot lifetime, independently of Wi-Fi's slot 1. */
#include "panel_storage.h"
#include "i18n/i18n.h"
#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_STORAGE
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "alarm_service.h"
#include "esp_console.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#if CONFIG_PANEL_STORAGE_SDSPI
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#endif
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#define SD_PATH "/sdcard"
static const char *TAG = "storage";
static SemaphoreHandle_t s_lock, s_files;
static QueueHandle_t s_queue;
static storage_sd_t s_sd;
static storage_flash_t s_flash;
static sdmmc_host_t s_host;
static sdmmc_card_t s_card;
static FATFS *s_fs;
static BYTE s_drive = FF_DRV_NOT_USED;
static char s_volume[8];
static bool s_slot_ready;
typedef struct { storage_action_t action; uint32_t generation; uint8_t user; } request_t;

static void publish(const storage_sd_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sd = *st;
    xSemaphoreGive(s_lock);
}
void panel_storage_status(storage_sd_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) { out->state = STORAGE_ERROR; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_sd;
    xSemaphoreGive(s_lock);
}
void panel_storage_flash(storage_flash_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_flash;
    xSemaphoreGive(s_lock);
}

static void flash_info(void)
{
    storage_flash_t f = {0};
    f.chip_valid = esp_flash_get_size(NULL, &f.chip_bytes) == ESP_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update) f.update_bytes = update->size;
    if (running) {
        f.slot_bytes = running->size;
        snprintf(f.running_slot, sizeof(f.running_slot), "%s", running->label);
        esp_image_metadata_t metadata;
        const esp_partition_pos_t position = {.offset = running->address, .size = running->size};
        if (esp_image_get_metadata(&position, &metadata) == ESP_OK) {
            f.image_bytes = storage_image_bytes(metadata.image_len, running->size,
                                               CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME);
            f.image_valid = f.image_bytes != 0u;
        }
    }
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
             ESP_PARTITION_SUBTYPE_ANY, NULL); it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        f.allocated_bytes += p->size;
        if (f.count < STORAGE_PARTITIONS) {
            storage_partition_t *dst = &f.partitions[f.count++];
            snprintf(dst->label, sizeof(dst->label), "%s", p->label);
            dst->bytes = p->size;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_flash = f;
    xSemaphoreGive(s_lock);
}

/* Caller owns s_files and all leases have closed their files. No host teardown here. */
static void detach(void)
{
    if (s_fs) {
        f_mount(NULL, s_volume, 0);
        esp_vfs_fat_unregister_path(SD_PATH);
        s_fs = NULL;
    }
    if (s_drive != FF_DRV_NOT_USED) {
        ff_diskio_unregister(s_drive);
        s_drive = FF_DRV_NOT_USED;
    }
    memset(&s_card, 0, sizeof(s_card));
}

/* Every message is one of the sentences marked I18N_NOOP_C("storage", ...) in this file: the
 * console, the Panel Manager and MQTT get the English, the touchscreen looks the sentence up in
 * its language (docs/features/37). Keep them whole sentences under 96 bytes. */
static void failure(storage_sd_t *st, storage_state_t state, const char *message)
{
    st->state = state;
    st->usage_valid = false;
    st->total_bytes = st->free_bytes = 0;
    st->filesystem[0] = '\0';
    if (state == STORAGE_NO_CARD || state == STORAGE_EJECTED) st->name[0] = '\0';
    snprintf(st->message, sizeof(st->message), "%s", message);
}

static bool read_usage(storage_sd_t *st)
{
    DWORD clusters;
    FATFS *fs;
    if (f_getfree(s_volume, &clusters, &fs) != FR_OK || fs->n_fatent < 2u) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Couldn't read free space. Safely remove or check the card again."));
        return false;
    }
    const uint64_t cluster_bytes = (uint64_t)fs->csize * s_card.csd.sector_size;
    st->total_bytes = (uint64_t)(fs->n_fatent - 2u) * cluster_bytes;
    st->free_bytes = (uint64_t)clusters * cluster_bytes;
    st->usage_valid = st->free_bytes <= st->total_bytes;
    if (!st->usage_valid) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Card reports inconsistent free space. Check it on a computer."));
        return false;
    }
    snprintf(st->filesystem, sizeof(st->filesystem), "%s",
             fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12");
    st->state = STORAGE_READY;
    return true;
}

static void probe(storage_sd_t *st)
{
    const uint32_t generation = st->generation + 1u;
    detach();
    memset(st, 0, sizeof(*st));
    st->generation = generation;
    if (!s_slot_ready) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "SD interface unavailable. Restart the panel to retry."));
        return;
    }
    esp_err_t err = sdmmc_card_init(&s_host, &s_card);
    if (err != ESP_OK) {
        failure(st, STORAGE_NO_CARD, I18N_NOOP_C("storage", "Insert a card, then choose Check card. It may also be unreadable."));
        ESP_LOGW(TAG, "card probe: %s", esp_err_to_name(err));
        return;
    }
    if (!s_card.is_mem || s_card.csd.capacity <= 0 || s_card.csd.sector_size != 512) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Unsupported card geometry. The card has not been changed."));
        return;
    }
    st->capacity_bytes = (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
    for (size_t i = 0; i < 5u && s_card.cid.name[i]; i++) {
        const unsigned char ch = (unsigned char)s_card.cid.name[i];
        st->name[i] = ch >= 32u && ch <= 126u ? (char)ch : '?';
    }
    if (ff_diskio_get_drive(&s_drive) != ESP_OK) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "No filesystem drive available."));
        return;
    }
    snprintf(s_volume, sizeof(s_volume), "%u:", (unsigned)s_drive);
    ff_diskio_register_sdmmc(s_drive, &s_card);
    ff_sdmmc_set_disk_status_check(s_drive, true);
    const esp_vfs_fat_conf_t conf = {.base_path = SD_PATH, .fat_drive = s_volume, .max_files = 4};
    if (esp_vfs_fat_register(&conf, &s_fs) != ESP_OK) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Couldn't register the card filesystem."));
        detach();
        return;
    }
    const FRESULT result = f_mount(s_fs, s_volume, 1);
    if (result == FR_NO_FILESYSTEM) {
        failure(st, STORAGE_UNSUPPORTED, I18N_NOOP_C("storage", "No supported FAT filesystem. Files have not been changed."));
    } else if (result != FR_OK) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Couldn't mount the card. Files have not been changed."));
    } else if (read_usage(st)) {
        snprintf(st->message, sizeof(st->message), I18N_NOOP_C("storage", "Card is ready. Safely remove before taking it out."));
    }
}

typedef struct { uint8_t user; bool authorized, idle; } access_t;
static void access_on_alarm(alarm_engine_t *engine, alarm_ms_t now, void *ctx)
{
    (void)now;
    access_t *a = ctx;
    const alarm_config_t *cfg = alarm_engine_config(engine);
    if (a->user > 0u && a->user <= ALARM_MAX_USERS) {
        const alarm_authority_t auth = cfg->users[a->user - 1u].authority;
        a->authorized = auth == ALARM_AUTH_MASTER || auth == ALARM_AUTH_INSTALLER;
    }
    a->idle = true;
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        alarm_partition_status_t status;
        alarm_engine_partition_status(engine, p, &status);
        if (status.state != ALARM_STATE_DISARMED || status.bell != ALARM_PAT_OFF ||
            status.triggered) a->idle = false;
    }
}

static bool access_ok(uint8_t user)
{
    access_t access = {.user = user};
    return alarm_service_call(access_on_alarm, &access) == ESP_OK &&
           access.authorized && access.idle;
}

static void format_card(storage_sd_t *st, uint8_t user)
{
    /* No card-detect pin: reinitialize and compare CID immediately before destructive IO.
     * Do not reprobe/mount with format_if_mount_failed: ordinary mount is never destructive. */
    sdmmc_card_t current = {0};
    if (sdmmc_card_init(&s_host, &current) != ESP_OK ||
        memcmp(&current.cid, &s_card.cid, sizeof(current.cid)) != 0) {
        detach();
        st->generation++;
        st->capacity_bytes = 0;
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Card changed or stopped responding. Check it and confirm again."));
        return;
    }
    s_card = current;
    void *work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) { failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Not enough memory to format. Card was not erased.")); return; }
    if (!access_ok(user)) {
        heap_caps_free(work);
        snprintf(st->message, sizeof(st->message), I18N_NOOP_C("storage", "Format canceled: disarm and use a master or installer code."));
        return;
    }
    f_mount(NULL, s_volume, 0);
    /* Replace every old partition only after confirmation. Two FATs; automatic cluster size. */
    const LBA_t sizes[4] = {100, 0, 0, 0};
    const MKFS_PARM options = {.fmt = FM_FAT | FM_FAT32, .n_fat = 2};
    FRESULT result = f_fdisk(s_drive, sizes, work);
    if (result == FR_OK) result = f_mkfs(s_volume, &options, work, 4096);
    heap_caps_free(work);
    st->generation++;
    if (result == FR_OK) result = f_mount(s_fs, s_volume, 1);
    if (result != FR_OK || !read_usage(st)) {
        failure(st, STORAGE_ERROR, I18N_NOOP_C("storage", "Format failed. Card contents may be lost. Check the card before retrying."));
    } else {
        snprintf(st->message, sizeof(st->message), I18N_NOOP_C("storage", "Format complete. Card is ready."));
    }
}

static void storage_task(void *ctx)
{
    (void)ctx;
    flash_info();
    storage_sd_t st = {0};
    xSemaphoreTake(s_files, portMAX_DELAY);
    probe(&st);
    publish(&st);
    xSemaphoreGive(s_files);
    for (;;) {
        request_t request;
        const bool requested = xQueueReceive(s_queue, &request, pdMS_TO_TICKS(5000)) == pdTRUE;
        if (xSemaphoreTake(s_files, requested ? portMAX_DELAY : 0) != pdTRUE) continue;
        panel_storage_status(&st);
        if (requested) {
            /* Request reserves WORKING before queueing, preventing new file leases. */
            if (!access_ok(request.user) || request.generation != st.generation) {
                /* Restore a fresh view without running the requested destructive action. */
                probe(&st);
                snprintf(st.message, sizeof(st.message), I18N_NOOP_C("storage", "Action canceled. Disarm and confirm again."));
            } else if (request.action == STORAGE_CHECK) {
                probe(&st);
            } else if (request.action == STORAGE_EJECT) {
                detach();
                st.generation++;
                st.capacity_bytes = 0;
                failure(&st, STORAGE_EJECTED, I18N_NOOP_C("storage", "Safe to remove. After insertion, choose Check card."));
            } else {
                format_card(&st, request.user);
                if (st.state == STORAGE_WORKING) {
                    if (s_fs) read_usage(&st);
                    else failure(&st, STORAGE_ERROR, I18N_NOOP_C("storage", "Check the card again."));
                }
            }
        } else if (st.state == STORAGE_READY || st.state == STORAGE_UNSUPPORTED) {
            if (sdmmc_get_status(&s_card) != ESP_OK) {
                detach();
                st.generation++;
                st.capacity_bytes = 0;
                failure(&st, STORAGE_NO_CARD, I18N_NOOP_C("storage", "Card removed or not responding. Insert it and choose Check card."));
            } else if (st.state == STORAGE_READY) {
                read_usage(&st);
            }
        }
        publish(&st);
        xSemaphoreGive(s_files);
    }
}

esp_err_t panel_storage_request(storage_action_t action, uint32_t generation, uint8_t user)
{
    if (!s_queue || !access_ok(user) || xSemaphoreTake(s_files, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    storage_sd_t st;
    panel_storage_status(&st);
    if (!storage_allowed(&st, action, generation, true, true, false)) {
        xSemaphoreGive(s_files);
        return ESP_ERR_INVALID_STATE;
    }
    const request_t request = {.action = action, .generation = generation, .user = user};
    st.state = STORAGE_WORKING;
    snprintf(st.message, sizeof(st.message), "%s", action == STORAGE_FORMAT ?
        I18N_NOOP_C("storage", "Formatting. Keep the card inserted and power connected.") :
        I18N_NOOP_C("storage", "Checking the card..."));
    publish(&st);
    const bool queued = xQueueSend(s_queue, &request, 0) == pdTRUE;
    if (!queued) { failure(&st, STORAGE_ERROR, I18N_NOOP_C("storage", "Storage is busy. Check the card again.")); publish(&st); }
    xSemaphoreGive(s_files);
    return queued ? ESP_OK : ESP_FAIL;
}

bool panel_storage_acquire(const char **path)
{
    if (!path || !s_files || xSemaphoreTake(s_files, 0) != pdTRUE) return false;
    storage_sd_t st;
    panel_storage_status(&st);
    if (st.state != STORAGE_READY) { xSemaphoreGive(s_files); return false; }
    *path = SD_PATH;
    return true;
}
void panel_storage_release(void) { xSemaphoreGive(s_files); }

#if CONFIG_PANEL_STORAGE_SDSPI
/* A card on an SPI bus (the classic ESP32 boards). The bus is set up here with DMA; a device
 * that shares it (a CC1101 on the same header) joins afterwards, which is why the app starts
 * storage first on such a board. A bus somebody else set up without DMA can't carry the card's
 * 512-byte blocks, so that is refused rather than tried. */
static bool spi_slot_init(const board_sd_t *sd)
{
    const spi_host_device_t host = (spi_host_device_t)(SPI2_HOST + (sd->spi_host - 2));
    const spi_bus_config_t bus = {
        .mosi_io_num = sd->mosi,
        .miso_io_num = sd->miso,
        .sclk_io_num = sd->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "the SD card's SPI bus was already set up by another device: start "
                      "storage before whatever shares the bus");
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI bus: %s", esp_err_to_name(err));
        return false;
    }
    err = sdspi_host_init();
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = host;
    dev.gpio_cs = sd->cs;
    sdspi_dev_handle_t handle = 0;
    if (err == ESP_OK) {
        err = sdspi_host_init_device(&dev, &handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI device: %s", esp_err_to_name(err));
        return false;
    }
    s_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    s_host.slot = handle;
    return true;
}
#endif /* CONFIG_PANEL_STORAGE_SDSPI */

esp_err_t panel_storage_start(const board_sd_t *sd)
{
    if (s_lock) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    s_files = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(request_t));
    if (!s_lock || !s_files || !s_queue) {
        if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
        if (s_files) { vSemaphoreDelete(s_files); s_files = NULL; }
        if (s_lock) {
            failure(&s_sd, STORAGE_ERROR, I18N_NOOP_C("storage", "Storage service could not start: not enough memory."));
        }
        return ESP_ERR_NO_MEM;
    }
    if (sd == NULL || sd->bus == BOARD_SD_NONE) {
        ESP_LOGW(TAG, "no SD slot on this board");
#if CONFIG_PANEL_STORAGE_SDMMC
    } else if (sd->bus == BOARD_SD_SDMMC_1BIT) {
        s_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
        s_host.slot = SDMMC_HOST_SLOT_0;
        s_host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
        s_host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = sd->clk;
        slot.cmd = sd->cmd;
        slot.d0 = sd->d0;
        slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        s_slot_ready =
            sdmmc_host_init() == ESP_OK && sdmmc_host_init_slot(s_host.slot, &slot) == ESP_OK;
#endif
#if CONFIG_PANEL_STORAGE_SDSPI
    } else if (sd->bus == BOARD_SD_SPI && sd->spi_host >= 2) {
        s_slot_ready = spi_slot_init(sd);
#endif
    } else {
        ESP_LOGE(TAG, "this firmware has no driver for the board's SD card bus "
                      "(PANEL_STORAGE_SDMMC / PANEL_STORAGE_SDSPI)");
    }
    if (xTaskCreate(storage_task, "storage", 6144, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        failure(&s_sd, STORAGE_ERROR, I18N_NOOP_C("storage", "Storage worker could not start: not enough memory."));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static int cmd_storage(int argc, char **argv)
{
    (void)argc; (void)argv;
    storage_sd_t sd;
    storage_flash_t flash;
    panel_storage_status(&sd);
    panel_storage_flash(&flash);
    printf("SD: %s; %s\n", storage_state_name(sd.state), sd.message);
    printf("capacity=%" PRIu64 " total=%" PRIu64 " free=%" PRIu64 " usage_valid=%d fs=%s\n",
           sd.capacity_bytes, sd.total_bytes, sd.free_bytes, sd.usage_valid, sd.filesystem);
    printf("flash=%" PRIu32 " firmware=%" PRIu32 " slot=%" PRIu32 " update=%" PRIu32 " image_valid=%d\n",
           flash.chip_bytes, flash.image_bytes, flash.slot_bytes, flash.update_bytes, flash.image_valid);
    return 0;
}

esp_err_t panel_storage_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "storage", .help = "Read SD and flash capacity; never formats", .func = cmd_storage};
    return esp_console_cmd_register(&cmd);
}

#else /* !CONFIG_ALARM_FEATURE_STORAGE: no Storage page, no SD card (docs/features/33 step 6) */

#include <string.h>

esp_err_t panel_storage_start(const board_sd_t *sd)
{
    (void)sd;
    return ESP_ERR_NOT_SUPPORTED;
}

void panel_storage_status(storage_sd_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = STORAGE_UNSUPPORTED;
}

void panel_storage_flash(storage_flash_t *out)
{
    memset(out, 0, sizeof(*out));
}

esp_err_t panel_storage_request(storage_action_t action, uint32_t generation, uint8_t user)
{
    (void)action;
    (void)generation;
    (void)user;
    return ESP_ERR_NOT_SUPPORTED;
}

bool panel_storage_acquire(const char **path)
{
    (void)path;
    return false;
}

void panel_storage_release(void) {}

esp_err_t panel_storage_console_register(void)
{
    return ESP_OK;
}

#endif /* CONFIG_ALARM_FEATURE_STORAGE */
