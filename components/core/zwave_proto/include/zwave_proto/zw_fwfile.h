/*
 * zw_fwfile.h - a device firmware file into the image a Firmware Update Meta Data transfer sends
 * (docs/features/36 ZW-033). Only formats the panel can read exactly are taken:
 *   - raw images, sent as they are: Silicon Labs Gecko bootloader files (.gbl, which start with the
 *     GBL tag 0x03A617EB little-endian), and plain binary (.bin, .ota);
 *   - Intel HEX (.hex): data records into one image from the lowest address to the highest, gaps
 *     filled with FF; extended segment (02) and linear (04) address records; the checksum of every
 *     line checked; an end-of-file record required.
 * Refused with a reason: compressed or encrypted vendor formats (.otz, .hec, "aeotec" containers),
 * ZIP archives - the panel never guesses at a transformation it can't verify.
 */
#ifndef ZW_FWFILE_H
#define ZW_FWFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZW_FWFILE_UNKNOWN = 0, /* refused */
    ZW_FWFILE_GECKO,       /* .gbl: as it is */
    ZW_FWFILE_BINARY,      /* .bin, .ota: as it is */
    ZW_FWFILE_HEX,         /* Intel HEX: converted */
    ZW_FWFILE_UNSUPPORTED  /* known, but not readable here (.otz, .hec, .zip) */
} zw_fwfile_format_t;

/* From the file name's extension (the URL's last path part) and its first bytes. */
zw_fwfile_format_t zw_fwfile_guess(const char *name, const uint8_t *data, size_t len);

/* Intel HEX text into `out` (at most `cap` bytes); the image's length, 0 when the text is not a
 * complete, well-formed HEX file or the image would not fit. */
size_t zw_fwfile_hex_to_bin(const uint8_t *hex, size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZW_FWFILE_H */
