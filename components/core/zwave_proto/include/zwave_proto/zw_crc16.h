/*
 * zw_crc16.h - the CRC-16 that Z-Wave's CRC-16 Encapsulation command class uses
 * (docs/features/36, security.md §11 "CRC-16 encapsulation"): CRC-16-CCITT, poly 0x1021, init
 * 0x1D0F, no reflection, no final XOR (the parameter set the CRC catalogue calls CRC-16/AUG-CCITT,
 * alias CRC-16/SPI-FUJITSU).
 */
#ifndef ZW_CRC16_H
#define ZW_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The starting value: start a message with this, or carry on a previous call's return. */
#define ZW_CRC16_INIT 0x1D0Fu

/* Chainable: uint16_t crc = zw_crc16(zw_crc16(ZW_CRC16_INIT, a, alen), b, blen). */
uint16_t zw_crc16(uint16_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CRC16_H */
