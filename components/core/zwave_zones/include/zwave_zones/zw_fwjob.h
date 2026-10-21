/*
 * zw_fwjob.h - a device's software update as the installer runs it, up to the transfer
 * (docs/features/36 ZW-033): "Check for an update" (the Z-Wave JS service's answer read by
 * zw_fwcat.h), "Install it" (its file downloaded, checked against the SHA-256 the service gave,
 * turned into the image the device takes: zw_fwfile.h), then zwz_panel_fw_start sends it.
 *
 * The owner does the talking (HTTPS in the firmware, a canned answer in the simulator) and keeps
 * the buffers; this keeps the state the device page shows and decides, the same way everywhere.
 * One device at a time: a check or an install for another device replaces the last one's.
 *
 * Offered only: an update whose one file is for the device's own firmware (target 0). One that
 * also updates other parts of the device (more targets) is named but not installed - the panel
 * never installs part of an update.
 *
 * Portable C11, caller-owned state, no heap.
 */
#ifndef ZW_FWJOB_H
#define ZW_FWJOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"
#include "zwave_host/zw_host.h"
#include "zwave_host/zw_nodes.h"
#include "zwave_zones/zw_fwcat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZWFJ_SERVICE_URL "https://firmware.zwave-js.io/api/v4/updates"
/* The service requires a User-Agent naming the client (its README). */
#define ZWFJ_USER_AGENT  "RoboAlarms-panel (github.com/RoboAlarms/alarm-panel)"
#define ZWFJ_BODY_MAX    320u

typedef struct {
    uint8_t state;       /* ui_zw_fw_check_t */
    uint16_t node, generation;
    zwfc_device_t dev;
    zwfc_update_t update; /* UI_ZW_FW_AVAILABLE / DOWNLOADING: the one offered */
    char words[96];
    bool radio;           /* the radio's own check (zwfj_check_radio) */
} zwfj_t;

void zwfj_init(zwfj_t *j);

/* What the service is asked about: its identity and installed version. False: not known yet. */
bool zwfj_device_of(const zw_node_record_t *rec, zwfc_device_t *out);

/* "Check for an update": the request body for ZWFJ_SERVICE_URL (POST, JSON), its length; 0 when
 * the device isn't one the service can be asked about (its identity or versions unknown). */
size_t zwfj_check_begin(zwfj_t *j, const zw_node_record_t *rec, const char *region, char *body,
                        size_t cap);
/* The service's answer: `http_status` 200 with its body, another status, or 0 when it couldn't be
 * reached. `region` as asked. Beta versions are never offered here. */
void zwfj_check_answer(zwfj_t *j, int http_status, const char *body, size_t len,
                       const char *region);

/* The Z-Wave radio itself (network.md §14), asked about as Z-Wave JS asks about a controller: its
 * own manufacturer, product type and product ID (GetSerialApiCapabilities), its firmware version
 * and SDK. The job then names node 0, generation 0 - "the radio" - for the calls below. 0 when the
 * radio's facts aren't known (it hasn't started). When the service answers but offers nothing,
 * the panel's own list of the ZAC93's files may (zwfc_radio_builtin). */
size_t zwfj_check_radio(zwfj_t *j, const zw_module_facts_t *f, const char *region, char *body,
                        size_t cap);

/* The region the service is asked for: the installer's choice when made, else the one the radio
 * reports (the service lists most files per region and offers none of those to a request without
 * one). NULL when neither is known. */
const char *zwfj_region(bool chosen_set, uint8_t chosen, const zw_module_facts_t *f);

/* "Install it" for (node, generation): the file's URL, NULL when nothing is on offer for it. */
const char *zwfj_download_begin(zwfj_t *j, uint16_t node, uint16_t generation);
/* "Stop" while its file downloads: back to the offer, nothing sent (the download's result, when it
 * comes, is not taken). False: nothing was downloading. */
bool zwfj_cancel(zwfj_t *j);
/* The download failed: its HTTP status, 0 when the server couldn't be reached. */
void zwfj_download_failed(zwfj_t *j, int http_status);
/* The downloaded file: checked against the promised SHA-256, then made the image in `out` (which
 * may be `file` itself for a file sent as it is). The image's length; 0 when it isn't taken
 * (UI_ZW_FW_FILE_FAILED, `words` say why). */
size_t zwfj_file(zwfj_t *j, const uint8_t *file, size_t len, uint8_t *out, size_t cap);
/* The transfer started (zwz_panel_fw_start said OK): the device page follows the operation now,
 * and asks the service again next time. */
void zwfj_started(zwfj_t *j);
/* zwz_panel_fw_start refused it (UI_ZW_FW_FILE_FAILED, `words` say why; nothing was sent). */
void zwfj_not_started(zwfj_t *j, ui_zw_req_result_t why);

/* The device page's software check for (node, generation). */
void zwfj_page(const zwfj_t *j, uint16_t node, uint16_t generation, ui_zw_device_page_t *page);

#ifdef __cplusplus
}
#endif

#endif /* ZW_FWJOB_H */
