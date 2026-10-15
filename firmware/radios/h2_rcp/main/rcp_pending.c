/*
 * rcp_pending.c - the radio's pending table, two corrections (docs/bugs/B021). The panel's Zigbee
 * stack puts a sleepy device's address in this table while it holds a frame for the device; the
 * radio then answers the device's poll with Frame Pending set, and the device stays awake for the
 * frame.
 *
 * 1. A full table says so. ESP-IDF's OpenThread port (esp_openthread_radio.c) ignores the
 *    driver's refusal and answers success, so the panel believed an address was in a full table
 *    and the device was told nothing waited. Refused here, the panel's stack falls back to Frame
 *    Pending on every poll (its mac_indirect_transmit does on a failed add).
 * 2. Frame Pending in the frame itself. A frame to a device whose address is still in the table
 *    goes out with Frame Pending set, so the device polls again for the next one - its key
 *    exchange is several frames in a row. ESP-IDF adds this after v6.1.0
 *    (espressif/esp-zigbee-sdk#893, Espressif's patch to esp_ieee802154_ack.c, which looks
 *    the address up in the driver's own table); that table is private, so this file keeps the
 *    same entries beside it. A secured frame is left alone: its integrity code covers the bit.
 *
 * The linker sends OpenThread's calls here (CMakeLists.txt, --wrap). All of them run on the
 * OpenThread task, so the copy needs no lock; the driver's interrupt reads only its own table.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_ieee802154.h"
#include "openthread/error.h"
#include "openthread/platform/radio.h"
#include "sdkconfig.h"

#define PENDING_MAX CONFIG_IEEE802154_PENDING_TABLE_SIZE
#define SHORT_LEN   2u
#define EXT_LEN     8u

/* From the driver's private esp_ieee802154_frame.h (ESP-IDF v6.1). */
uint8_t ieee802154_frame_get_dst_addr(const uint8_t *frame, uint8_t *addr);
#define FRAME_DST_SHORT 0x08u
#define FRAME_DST_EXT   0x0Cu
#define FCF0            1u    /* frame[0] is the length */
#define FCF_SECURITY    0x08u
#define FCF_PENDING     0x10u

typedef struct {
    uint8_t addr[EXT_LEN];
    bool used;
} entry_t;

/* What the driver's table holds: [0] short addresses, [1] extended ones. */
static entry_t s_table[2][PENDING_MAX];

static entry_t *find(bool is_short, const uint8_t *addr)
{
    const size_t len = is_short ? SHORT_LEN : EXT_LEN;
    for (size_t i = 0; i < PENDING_MAX; i++) {
        entry_t *e = &s_table[is_short ? 0 : 1][i];
        if (e->used && memcmp(e->addr, addr, len) == 0) {
            return e;
        }
    }
    return NULL;
}

static otError add(bool is_short, const uint8_t *addr)
{
    if (esp_ieee802154_add_pending_addr(addr, is_short) != ESP_OK) {
        return OT_ERROR_NO_BUFS; /* full: the panel answers every poll with Frame Pending */
    }
    if (find(is_short, addr) == NULL) {
        for (size_t i = 0; i < PENDING_MAX; i++) {
            entry_t *e = &s_table[is_short ? 0 : 1][i];
            if (!e->used) {
                memcpy(e->addr, addr, is_short ? SHORT_LEN : EXT_LEN);
                e->used = true;
                break;
            }
        }
    }
    return OT_ERROR_NONE;
}

static otError clear(bool is_short, const uint8_t *addr)
{
    (void)esp_ieee802154_clear_pending_addr(addr, is_short);
    entry_t *e = find(is_short, addr);
    if (e != NULL) {
        e->used = false;
    }
    return OT_ERROR_NONE;
}

otError __wrap_otPlatRadioAddSrcMatchShortEntry(otInstance *instance, uint16_t short_address);
otError __wrap_otPlatRadioAddSrcMatchShortEntry(otInstance *instance, uint16_t short_address)
{
    (void)instance;
    uint8_t addr[SHORT_LEN];
    memcpy(addr, &short_address, sizeof(addr)); /* as the port hands it to the driver */
    return add(true, addr);
}

otError __wrap_otPlatRadioAddSrcMatchExtEntry(otInstance *instance, const otExtAddress *ext);
otError __wrap_otPlatRadioAddSrcMatchExtEntry(otInstance *instance, const otExtAddress *ext)
{
    (void)instance;
    return add(false, ext->m8);
}

otError __wrap_otPlatRadioClearSrcMatchShortEntry(otInstance *instance, uint16_t short_address);
otError __wrap_otPlatRadioClearSrcMatchShortEntry(otInstance *instance, uint16_t short_address)
{
    (void)instance;
    uint8_t addr[SHORT_LEN];
    memcpy(addr, &short_address, sizeof(addr));
    return clear(true, addr);
}

otError __wrap_otPlatRadioClearSrcMatchExtEntry(otInstance *instance, const otExtAddress *ext);
otError __wrap_otPlatRadioClearSrcMatchExtEntry(otInstance *instance, const otExtAddress *ext)
{
    (void)instance;
    return clear(false, ext->m8);
}

void __wrap_otPlatRadioClearSrcMatchShortEntries(otInstance *instance);
void __wrap_otPlatRadioClearSrcMatchShortEntries(otInstance *instance)
{
    (void)instance;
    esp_ieee802154_reset_pending_table(true);
    memset(s_table[0], 0, sizeof(s_table[0]));
}

void __wrap_otPlatRadioClearSrcMatchExtEntries(otInstance *instance);
void __wrap_otPlatRadioClearSrcMatchExtEntries(otInstance *instance)
{
    (void)instance;
    esp_ieee802154_reset_pending_table(false);
    memset(s_table[1], 0, sizeof(s_table[1]));
}

esp_err_t __real_esp_ieee802154_transmit(const uint8_t *frame, bool cca);
esp_err_t __wrap_esp_ieee802154_transmit(const uint8_t *frame, bool cca);
esp_err_t __wrap_esp_ieee802154_transmit(const uint8_t *frame, bool cca)
{
    const esp_ieee802154_pending_mode_t mode = esp_ieee802154_get_pending_mode();
    if ((mode == ESP_IEEE802154_AUTO_PENDING_ENABLE || mode == ESP_IEEE802154_AUTO_PENDING_ENHANCED) &&
        (frame[FCF0] & FCF_SECURITY) == 0u) {
        uint8_t addr[EXT_LEN];
        const uint8_t dst = ieee802154_frame_get_dst_addr(frame, addr);
        if ((dst == FRAME_DST_SHORT && find(true, addr) != NULL) ||
            (dst == FRAME_DST_EXT && find(false, addr) != NULL)) {
            ((uint8_t *)frame)[FCF0] |= FCF_PENDING; /* OpenThread's own frame buffer */
        }
    }
    return __real_esp_ieee802154_transmit(frame, cca);
}
