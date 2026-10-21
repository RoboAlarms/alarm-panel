/*
 * zw_fwcat.h - which firmware a Z-Wave device could be updated to (docs/features/36 ZW-033),
 * from the Z-Wave JS firmware update service (https://firmware.zwave-js.io, POST
 * /api/v4/updates; github.com/zwave-js/firmware-updates, checked 2026-09-27: no API key, a
 * descriptive User-Agent required, free on a fair-use basis). The panel asks only when the
 * installer taps "Check for updates" - never on a schedule - and never updates by itself.
 *
 * Request: {"region"?, "devices":[{"manufacturerId":"0x0346","productType":"0x0101",
 * "productId":"0x0301","firmwareVersion":"1.18","sdkVersion"?}]}.
 * Response: an array, per device {"manufacturerId", "productType", "productId",
 * "firmwareVersion", "updates":[{"version", "changelog", "channel":"stable"|"beta",
 * "normalizedVersion"?, "downgrade":bool, "region"?, "files":[{"target", "url",
 * "integrity":"sha256:<64 hex>"}]}]}.
 *
 * What the panel offers (zwfc_best), and nothing else: the device answered for is exactly the
 * one asked about (manufacturer, product type, product ID); a stable release (beta only when
 * asked for); never a downgrade, nor the same version - by the service's own `downgrade` and by
 * comparing versions here; the panel's region or none; a file for firmware target 0 over https
 * with a SHA-256 integrity - the download is checked against it before anything is sent. When
 * several qualify, the highest version.
 *
 * Portable C11, no heap; the JSON goes through alarm_proto's strict reader.
 */
#ifndef ZW_FWCAT_H
#define ZW_FWCAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZWFC_URL_MAX       256u
#define ZWFC_CHANGELOG_MAX 200u /* kept for the page; a longer one is left out */
#define ZWFC_FILES_MAX     2u

typedef struct {
    uint16_t manufacturer, product_type, product_id;
    uint8_t fw_major, fw_minor;
    bool has_sdk;
    uint8_t sdk_major, sdk_minor, sdk_patch;
} zwfc_device_t;

typedef struct {
    uint8_t target;
    char url[ZWFC_URL_MAX];
    uint8_t sha256[32];
} zwfc_file_t;

typedef struct {
    uint16_t major, minor, patch;
    char version[24];          /* as the service wrote it */
    bool beta;
    char changelog[ZWFC_CHANGELOG_MAX];
    bool changelog_cut;        /* longer than kept: not shown */
    uint8_t file_count;
    zwfc_file_t files[ZWFC_FILES_MAX];
} zwfc_update_t;

typedef enum {
    ZWFC_UPDATE = 0,     /* *out is the one to offer */
    ZWFC_UP_TO_DATE,     /* the service knows the device and has nothing newer for it */
    ZWFC_UNKNOWN_DEVICE, /* the service doesn't know it */
    ZWFC_BAD_ANSWER,     /* not the JSON the service sends */
} zwfc_verdict_t;

/* The v4 request body for one device (`region` NULL or the service's words: "usa"...). Its length,
 * 0 when it doesn't fit. */
size_t zwfc_request(const zwfc_device_t *dev, const char *region, char *out, size_t cap);

/* The service's answer for `dev`. */
zwfc_verdict_t zwfc_best(const char *json, size_t len, const zwfc_device_t *dev, const char *region,
                         bool include_beta, zwfc_update_t *out);

/* The service's region word for a Z-Wave region code (serial-api.md §6.7), NULL when none. */
const char *zwfc_region(uint8_t code);

/* The ZAC93's own firmware, built in (docs/features/36 network.md §14): the files Zooz publishes,
 * with the URLs and SHA-256 of Z-Wave JS's own entry (firmwares/zooz/ZAC93-V02.json, 2026-10-20).
 * That entry matches only a radio already on 2.0-2.255, so a 1.x radio - Zooz says it takes 2.00
 * and later - would be offered nothing. True with *out the newest file for `region` ("usa" or
 * "europe") when `dev` is exactly Zooz's ZAC93 (027A:0004:0611) and older than it. From 2.20 the
 * radio reports the SDK's default identity instead: never matched here, never guessed at. */
bool zwfc_radio_builtin(const zwfc_device_t *dev, const char *region, zwfc_update_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_FWCAT_H */
