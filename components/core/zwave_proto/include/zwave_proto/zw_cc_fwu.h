/*
 * zw_cc_fwu.h - Firmware Update Meta Data (7A): updating a device's own firmware over the air
 * (docs/features/36 ZW-033). Layouts as Z-Wave JS's FirmwareUpdateMetaDataCC (checked 2026-09-27);
 * multi-byte fields big-endian. Commands include their CC and command bytes.
 *
 *   01 MD Get                 -
 *   02 MD Report              manufacturer(2) firmware ID(2) checksum(2) | v3: upgradable(1)
 *                             additional targets n(1) max fragment(2) n x firmware ID(2) | v5:
 *                             hardware(1) | v6: flags(1): 01 continues to function, 02 activation
 *                             (v7), 04 non-secure transfer (v8), 08 resume (v8)
 *   03 Request Get            manufacturer(2) firmware ID(2) checksum(2) | v3: target(1)
 *                             fragment size(2) | v4: flags(1) 01 activation | v5: hardware(1)
 *   04 Request Report         status(1) (FF OK; 00 wrong manufacturer or ID, 01 authentication
 *                             expected, 02 fragment too large, 03 not upgradable, 04 wrong
 *                             hardware, 05 already updating, 06 battery low)
 *   05 Get                    reports(1) report number(2, bit 15 reserved)
 *   06 Report                 last<<15 | report number(2) data | v2: CRC-16 (1D0F) over the whole
 *                             command, CC and command bytes included
 *   07 Status Report          status(1) | v3: wait time(2 s) (00 checksum, 01 transmission failed,
 *                             02 wrong manufacturer, 03 wrong firmware ID, 04 wrong target, 05/06
 *                             header, 07 memory, 08 hardware; FD waiting for activation, FE OK no
 *                             restart, FF OK restart pending)
 *   08 Activation Set         manufacturer(2) firmware ID(2) checksum(2) target(1) | v5: hw(1)
 *   09 Activation Report      manufacturer(2) firmware ID(2) checksum(2) target(1) status(1)
 *                             (FF OK, 00 invalid firmware, 01 activation failed) | v5: hw(1)
 *
 * The image checksum is CRC-16 1D0F over the whole image.
 */
#ifndef ZW_CC_FWU_H
#define ZW_CC_FWU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_CC_FIRMWARE_UPDATE 0x7Au

#define ZW_FWU_MD_GET            0x01u
#define ZW_FWU_MD_REPORT         0x02u
#define ZW_FWU_REQUEST_GET       0x03u
#define ZW_FWU_REQUEST_REPORT    0x04u
#define ZW_FWU_GET               0x05u
#define ZW_FWU_REPORT            0x06u
#define ZW_FWU_STATUS_REPORT     0x07u
#define ZW_FWU_ACTIVATION_SET    0x08u
#define ZW_FWU_ACTIVATION_REPORT 0x09u

#define ZW_FWU_TARGETS_MAX 4u /* additional targets kept from an MD Report */

/* Request Report status */
#define ZW_FWU_REQ_OK               0xFFu
#define ZW_FWU_REQ_WRONG_ID         0x00u
#define ZW_FWU_REQ_AUTH_EXPECTED    0x01u
#define ZW_FWU_REQ_FRAGMENT_TOO_BIG 0x02u
#define ZW_FWU_REQ_NOT_UPGRADABLE   0x03u
#define ZW_FWU_REQ_WRONG_HARDWARE   0x04u
#define ZW_FWU_REQ_IN_PROGRESS      0x05u
#define ZW_FWU_REQ_BATTERY_LOW      0x06u

/* Status Report status */
#define ZW_FWU_STATUS_CHECKSUM       0x00u
#define ZW_FWU_STATUS_TRANSMISSION   0x01u
#define ZW_FWU_STATUS_WRONG_MANUF    0x02u
#define ZW_FWU_STATUS_WRONG_ID       0x03u
#define ZW_FWU_STATUS_WRONG_TARGET   0x04u
#define ZW_FWU_STATUS_HEADER_INFO    0x05u
#define ZW_FWU_STATUS_HEADER_FORMAT  0x06u
#define ZW_FWU_STATUS_MEMORY         0x07u
#define ZW_FWU_STATUS_WRONG_HARDWARE 0x08u
#define ZW_FWU_STATUS_WAIT_ACTIVATE  0xFDu
#define ZW_FWU_STATUS_OK_NO_RESTART  0xFEu
#define ZW_FWU_STATUS_OK_RESTART     0xFFu

typedef struct {
    uint16_t manufacturer_id, firmware_id, checksum;
    bool upgradable;             /* v1/v2 reports don't say: true */
    uint8_t version_seen;        /* the shape the report had: 1 (6 bytes), 3, 5, 6 */
    uint16_t max_fragment;       /* v3+; 0: not said */
    uint8_t target_count;        /* additional targets (1..); kept up to ZW_FWU_TARGETS_MAX */
    uint16_t target_ids[ZW_FWU_TARGETS_MAX];
    uint8_t hardware;            /* v5+; 0xFF: not said */
    bool continues_to_function, supports_activation, supports_non_secure, supports_resume;
} zw_fwu_md_t;

bool zw_fwu_md_report_parse(const uint8_t *p, size_t len, zw_fwu_md_t *out);
size_t zw_fwu_md_report_build(uint8_t *out, size_t cap, const zw_fwu_md_t *in); /* v6 shape */
size_t zw_fwu_md_get_build(uint8_t *out, size_t cap);

typedef struct {
    uint16_t manufacturer_id, firmware_id, checksum;
    uint8_t target;
    uint16_t fragment_size;
    bool activation;
    uint8_t hardware; /* 0xFF: not sent */
    uint8_t cc_version; /* the device's 7A version: which fields go */
} zw_fwu_request_t;

size_t zw_fwu_request_get_build(uint8_t *out, size_t cap, const zw_fwu_request_t *in);
bool zw_fwu_request_get_parse(const uint8_t *p, size_t len, zw_fwu_request_t *out);
bool zw_fwu_request_report_parse(const uint8_t *p, size_t len, uint8_t *status);

typedef struct {
    uint8_t reports;
    uint16_t report_number; /* 1.. */
} zw_fwu_get_t;
bool zw_fwu_get_parse(const uint8_t *p, size_t len, zw_fwu_get_t *out);
size_t zw_fwu_get_build(uint8_t *out, size_t cap, const zw_fwu_get_t *in);

/* A fragment: with_crc for a device's 7A v2+. */
size_t zw_fwu_report_build(uint8_t *out, size_t cap, uint16_t report_number, bool last,
                           const uint8_t *data, size_t len, bool with_crc);
/* A fragment as a device receives it (for the simulated device and tests): false when its CRC
 * (with_crc) is wrong. */
bool zw_fwu_report_parse(const uint8_t *p, size_t len, bool with_crc, uint16_t *report_number,
                         bool *last, const uint8_t **data, size_t *data_len);

bool zw_fwu_status_report_parse(const uint8_t *p, size_t len, uint8_t *status, bool *has_wait,
                                uint16_t *wait_s);
size_t zw_fwu_status_report_build(uint8_t *out, size_t cap, uint8_t status, uint16_t wait_s);

typedef struct {
    uint16_t manufacturer_id, firmware_id, checksum;
    uint8_t target;
    uint8_t status;   /* Activation Report only */
    uint8_t hardware; /* 0xFF: not sent */
} zw_fwu_activation_t;
size_t zw_fwu_activation_set_build(uint8_t *out, size_t cap, const zw_fwu_activation_t *in);
bool zw_fwu_activation_report_parse(const uint8_t *p, size_t len, zw_fwu_activation_t *out);

/* CRC-16 1D0F over the image: the checksum Request Get and Activation Set carry. */
uint16_t zw_fwu_image_checksum(const uint8_t *image, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_FWU_H */
