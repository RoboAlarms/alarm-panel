/*
 * zw_gbl.h - a Gecko bootloader file (.gbl, Silicon Labs UG266) checked before a byte of it goes to
 * the Z-Wave radio's bootloader (docs/features/36 serial-api.md §8, network.md §14; ZW-025).
 *
 * The file is a chain of tags - a 32-bit ID and a 32-bit length (both little-endian), then that
 * many bytes. The first is the header (ID 0x03A617EB, 8 bytes: version and type); the last is the
 * end tag (ID 0xFC0404FC, 4 bytes) holding a CRC-32 (IEEE 802.3: reflected, polynomial
 * 0xEDB88320, initial and final value 0xFFFFFFFF) of every byte of the file before it, its own ID
 * and length included. Nothing may follow it. What the panel checks: the chain reaches the end tag
 * exactly at the end of the file, no tag runs past it, and the CRC agrees - a file damaged in the
 * download, cut short or not a GBL at all is refused before the radio is touched. Which radio a
 * file is for the file doesn't say; that is the update service's (or the installer's) word.
 *
 * Also the XMODEM-CRC helpers the upload uses (§8 step 3): 128-byte blocks, padded with 0xFF.
 *
 * Portable C11, no heap.
 */
#ifndef ZW_GBL_H
#define ZW_GBL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_GBL_TAG_HEADER 0x03A617EBu
#define ZW_GBL_TAG_END    0xFC0404FCu
/* The bootloader's own limit is the radio's flash; a ZAC93 file is a few hundred KB. */
#define ZW_GBL_MAX        (1024u * 1024u)

typedef enum {
    ZW_GBL_OK = 0,
    ZW_GBL_NOT_GBL,   /* no header tag first */
    ZW_GBL_DAMAGED,   /* a tag runs past the end, no end tag, or bytes after it */
    ZW_GBL_CRC,       /* the end tag's CRC-32 disagrees: damaged in the download */
    ZW_GBL_TOO_BIG
} zw_gbl_result_t;

/* The file as it is: ZW_GBL_OK when it may be uploaded. `tags` (optional): how many tags. */
zw_gbl_result_t zw_gbl_check(const uint8_t *data, size_t len, uint32_t *tags);

/* CRC-32 (IEEE 802.3), chainable: crc = zw_crc32(zw_crc32(0, a, alen), b, blen). */
uint32_t zw_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* ---- XMODEM-CRC (the bootloader's upload) ---- */
#define ZW_XMODEM_BLOCK 128u
#define ZW_XMODEM_FRAME (3u + ZW_XMODEM_BLOCK + 2u) /* SOH, n, 255 - n, data, CRC high, low */
#define ZW_XMODEM_SOH   0x01u
#define ZW_XMODEM_EOT   0x04u
#define ZW_XMODEM_ACK   0x06u
#define ZW_XMODEM_NAK   0x15u
#define ZW_XMODEM_CAN   0x18u
#define ZW_XMODEM_READY 0x43u /* 'C': the receiver wants CRC blocks */

/* How many blocks a file of `len` bytes takes. */
uint32_t zw_xmodem_blocks(size_t len);
/* Block `index` (0-based) of the file: its frame, numbered index + 1 (mod 256), the last block
 * padded with 0xFF, CRC-16/XMODEM (poly 0x1021, init 0) big-endian. False: past the file. */
bool zw_xmodem_frame(const uint8_t *data, size_t len, uint32_t index,
                     uint8_t out[ZW_XMODEM_FRAME]);

#ifdef __cplusplus
}
#endif

#endif /* ZW_GBL_H */
