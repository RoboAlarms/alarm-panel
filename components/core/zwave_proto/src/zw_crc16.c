/*
 * zw_crc16.c - CRC-16-CCITT, init 0x1D0F, no reflection (see zw_crc16.h): the textbook bit-at-a-
 * time form, parameterised only by the starting value so a message can be folded in over several
 * calls.
 */
#include "zwave_proto/zw_crc16.h"

uint16_t zw_crc16(uint16_t crc, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((unsigned)crc ^ ((unsigned)data[i] << 8));
        for (unsigned bit = 0; bit < 8u; bit++) {
            if (((unsigned)crc & 0x8000u) != 0u) {
                crc = (uint16_t)((unsigned)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
