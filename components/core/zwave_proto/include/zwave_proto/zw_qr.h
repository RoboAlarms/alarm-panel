/*
 * zw_qr.h - the Z-Wave QR code on a device's label (docs/features/36, network.md §4.2): a string
 * of decimal digits, `90` + version + checksum + requested keys + the DSK + TLV blocks. Version
 * 01 is a SmartStart code (the device can be pre-registered and joins by itself); version 00 is
 * an S2-only code (it carries the DSK for a classic inclusion).
 *
 * Strict: every digit is accounted for, the checksum must match, product type and product ID
 * must be there, an unknown critical block refuses the code. Whitespace (a scanner's line breaks,
 * a pasted code's spaces) is removed first; anything else that is not a digit refuses it.
 * Portable C11, no heap.
 */
#ifndef ZW_QR_H
#define ZW_QR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest code read (digits after whitespace is removed). Real codes are 90-140 digits. */
#define ZW_QR_MAX_DIGITS 512u
#define ZW_QR_MIN_DIGITS 52u

#define ZW_QR_VERSION_S2         0u
#define ZW_QR_VERSION_SMARTSTART 1u

/* TLV types (the 2-digit type field is type x 2 + critical). */
#define ZW_QR_TLV_PRODUCT_TYPE 0u
#define ZW_QR_TLV_PRODUCT_ID   1u
#define ZW_QR_TLV_MAX_INTERVAL 2u
#define ZW_QR_TLV_UUID16       3u
#define ZW_QR_TLV_PROTOCOLS    4u

/* Supported protocols (TLV 4). Without the block: classic only. */
#define ZW_QR_PROTO_CLASSIC    0x01u
#define ZW_QR_PROTO_LONG_RANGE 0x02u

typedef enum {
    ZW_QR_OK = 0,
    ZW_QR_NOT_ZWAVE,     /* not digits, or not starting `90`: some other QR code */
    ZW_QR_TOO_SHORT,     /* fewer than 52 digits, or a block cut short */
    ZW_QR_TOO_LONG,
    ZW_QR_CHECKSUM,      /* damaged or mistyped */
    ZW_QR_VERSION,       /* a version this panel does not know */
    ZW_QR_BAD_NUMBER,    /* a field out of its range (a DSK group above 65535...) */
    ZW_QR_CRITICAL,      /* an unknown block marked critical */
    ZW_QR_MISSING        /* no product type or product ID block */
} zw_qr_result_t;

typedef struct {
    uint8_t version;          /* ZW_QR_VERSION_* */
    uint8_t requested_keys;   /* security.md §1 mask (ZW_S2_KEY_* bits) */
    uint8_t dsk[16];
    uint8_t generic;          /* product type */
    uint8_t specific;
    uint16_t icon;
    uint16_t manufacturer;    /* product ID */
    uint16_t product_type;
    uint16_t product_id;
    uint16_t app_version;     /* major x 256 + minor */
    bool has_max_interval;
    uint16_t max_interval_s;  /* n x 128 s */
    bool has_uuid;
    uint8_t uuid_format;
    uint8_t uuid[16];
    uint8_t protocols;        /* ZW_QR_PROTO_* (classic when the code does not say) */
} zw_qr_t;

/* Parses `text` (NUL-terminated). ZW_QR_OK fills `out`; anything else leaves it zeroed. */
zw_qr_result_t zw_qr_parse(const char *text, zw_qr_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_QR_H */
