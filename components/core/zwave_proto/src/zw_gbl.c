/*
 * zw_gbl.c - a Gecko bootloader file checked, and the XMODEM-CRC blocks the upload sends. See
 * zw_gbl.h.
 */
#include "zwave_proto/zw_gbl.h"

#include <string.h>

#include "zwave_proto/zw_crc16.h"

static uint32_t u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t zw_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

zw_gbl_result_t zw_gbl_check(const uint8_t *data, size_t len, uint32_t *tags)
{
    uint32_t n = 0u;
    if (tags != NULL) {
        *tags = 0u;
    }
    if (data == NULL || len < 8u || u32_le(data) != ZW_GBL_TAG_HEADER) {
        return ZW_GBL_NOT_GBL;
    }
    if (len > ZW_GBL_MAX) {
        return ZW_GBL_TOO_BIG;
    }
    size_t at = 0u;
    while (len - at >= 8u) {
        const uint32_t id = u32_le(&data[at]);
        const uint32_t tlen = u32_le(&data[at + 4u]);
        if (tlen > len - at - 8u) {
            return ZW_GBL_DAMAGED; /* runs past the end */
        }
        n++;
        if (id == ZW_GBL_TAG_END) {
            if (tlen != 4u || at + 12u != len) {
                return ZW_GBL_DAMAGED; /* the end tag holds the CRC, and ends the file */
            }
            if (tags != NULL) {
                *tags = n;
            }
            const uint32_t crc = zw_crc32(0u, data, at + 8u);
            return crc == u32_le(&data[at + 8u]) ? ZW_GBL_OK : ZW_GBL_CRC;
        }
        at += 8u + (size_t)tlen;
    }
    return ZW_GBL_DAMAGED; /* no end tag */
}

uint32_t zw_xmodem_blocks(size_t len)
{
    return (uint32_t)((len + ZW_XMODEM_BLOCK - 1u) / ZW_XMODEM_BLOCK);
}

bool zw_xmodem_frame(const uint8_t *data, size_t len, uint32_t index,
                     uint8_t out[ZW_XMODEM_FRAME])
{
    const size_t start = (size_t)index * ZW_XMODEM_BLOCK;
    if (data == NULL || start >= len) {
        return false;
    }
    const size_t n = len - start < ZW_XMODEM_BLOCK ? len - start : ZW_XMODEM_BLOCK;
    const uint8_t number = (uint8_t)((index + 1u) & 0xFFu);
    out[0] = ZW_XMODEM_SOH;
    out[1] = number;
    out[2] = (uint8_t)(0xFFu - (unsigned)number);
    memcpy(&out[3], &data[start], n);
    memset(&out[3u + n], 0xFF, ZW_XMODEM_BLOCK - n);
    const uint16_t crc = zw_crc16(0x0000u, &out[3], ZW_XMODEM_BLOCK);
    out[3u + ZW_XMODEM_BLOCK] = (uint8_t)(crc >> 8);
    out[4u + ZW_XMODEM_BLOCK] = (uint8_t)(crc & 0xFFu);
    return true;
}
