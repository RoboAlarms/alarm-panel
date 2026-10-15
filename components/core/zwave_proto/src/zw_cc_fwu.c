/*
 * zw_cc_fwu.c - Firmware Update Meta Data (7A). See zw_cc_fwu.h.
 */
#include "zwave_proto/zw_cc_fwu.h"

#include <string.h>

#include "zwave_proto/zw_crc16.h"

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] << 8 | (unsigned)p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static bool is(const uint8_t *p, size_t len, uint8_t cmd, size_t min)
{
    return p != NULL && len >= 2u + min && p[0] == ZW_CC_FIRMWARE_UPDATE && p[1] == cmd;
}

bool zw_fwu_md_report_parse(const uint8_t *p, size_t len, zw_fwu_md_t *out)
{
    if (out == NULL || !is(p, len, ZW_FWU_MD_REPORT, 6u)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const uint8_t *q = &p[2];
    const size_t n = len - 2u;
    out->manufacturer_id = be16(&q[0]);
    out->firmware_id = be16(&q[2]);
    out->checksum = be16(&q[4]);
    out->upgradable = true;
    out->hardware = 0xFFu;
    out->version_seen = 1u;
    if (n < 10u) {
        return true; /* v1/v2 (trailing bytes of a partial v3 are not believed) */
    }
    out->upgradable = q[6] == 0xFFu;
    const size_t targets = q[7];
    out->max_fragment = be16(&q[8]);
    if (n < 10u + 2u * targets) {
        return false; /* it says more targets than it carries */
    }
    out->version_seen = 3u;
    out->target_count = (uint8_t)targets;
    for (size_t i = 0; i < targets && i < ZW_FWU_TARGETS_MAX; i++) {
        out->target_ids[i] = be16(&q[10u + 2u * i]);
    }
    size_t at = 10u + 2u * targets;
    if (n > at) {
        out->hardware = q[at++];
        out->version_seen = 5u;
    }
    if (n > at) {
        const uint8_t f = q[at];
        out->version_seen = 6u;
        out->continues_to_function = (f & 0x01u) != 0u;
        out->supports_activation = (f & 0x02u) != 0u;
        out->supports_non_secure = (f & 0x04u) != 0u;
        out->supports_resume = (f & 0x08u) != 0u;
    }
    return true;
}

size_t zw_fwu_md_report_build(uint8_t *out, size_t cap, const zw_fwu_md_t *in)
{
    const size_t targets = in->target_count <= ZW_FWU_TARGETS_MAX ? (size_t)in->target_count
                                                                  : ZW_FWU_TARGETS_MAX;
    const size_t n = 2u + 10u + 2u * targets + 2u;
    if (out == NULL || cap < n) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_MD_REPORT;
    put16(&out[2], in->manufacturer_id);
    put16(&out[4], in->firmware_id);
    put16(&out[6], in->checksum);
    out[8] = in->upgradable ? 0xFFu : 0x00u;
    out[9] = (uint8_t)targets;
    put16(&out[10], in->max_fragment);
    for (size_t i = 0; i < targets; i++) {
        put16(&out[12u + 2u * i], in->target_ids[i]);
    }
    size_t at = 12u + 2u * targets;
    out[at++] = in->hardware;
    out[at++] = (uint8_t)((in->continues_to_function ? 0x01u : 0u) |
                          (in->supports_activation ? 0x02u : 0u) |
                          (in->supports_non_secure ? 0x04u : 0u) |
                          (in->supports_resume ? 0x08u : 0u));
    return at;
}

size_t zw_fwu_md_get_build(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_MD_GET;
    return 2u;
}

size_t zw_fwu_request_get_build(uint8_t *out, size_t cap, const zw_fwu_request_t *in)
{
    const uint8_t v = in->cc_version;
    size_t n = 2u + 6u;
    if (v >= 3u) {
        n += 3u;
    }
    if (v >= 4u) {
        n += 1u;
    }
    if (v >= 5u && in->hardware != 0xFFu) {
        n += 1u;
    }
    if (out == NULL || cap < n) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_REQUEST_GET;
    put16(&out[2], in->manufacturer_id);
    put16(&out[4], in->firmware_id);
    put16(&out[6], in->checksum);
    size_t at = 8u;
    if (v >= 3u) {
        out[at++] = in->target;
        put16(&out[at], in->fragment_size);
        at += 2u;
    }
    if (v >= 4u) {
        out[at++] = in->activation ? 0x01u : 0x00u;
    }
    if (v >= 5u && in->hardware != 0xFFu) {
        out[at++] = in->hardware;
    }
    return at;
}

bool zw_fwu_request_get_parse(const uint8_t *p, size_t len, zw_fwu_request_t *out)
{
    if (out == NULL || !is(p, len, ZW_FWU_REQUEST_GET, 6u)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->manufacturer_id = be16(&p[2]);
    out->firmware_id = be16(&p[4]);
    out->checksum = be16(&p[6]);
    out->hardware = 0xFFu;
    out->cc_version = 1u;
    out->fragment_size = 0u;
    if (len >= 11u) {
        out->target = p[8];
        out->fragment_size = be16(&p[9]);
        out->cc_version = 3u;
    }
    if (len >= 12u) {
        out->activation = (p[11] & 0x01u) != 0u;
        out->cc_version = 4u;
    }
    if (len >= 13u) {
        out->hardware = p[12];
        out->cc_version = 5u;
    }
    return true;
}

bool zw_fwu_request_report_parse(const uint8_t *p, size_t len, uint8_t *status)
{
    if (status == NULL || !is(p, len, ZW_FWU_REQUEST_REPORT, 1u)) {
        return false;
    }
    *status = p[2];
    return true;
}

bool zw_fwu_get_parse(const uint8_t *p, size_t len, zw_fwu_get_t *out)
{
    if (out == NULL || !is(p, len, ZW_FWU_GET, 3u)) {
        return false;
    }
    out->reports = p[2];
    out->report_number = (uint16_t)(be16(&p[3]) & 0x7FFFu);
    return out->reports != 0u && out->report_number != 0u;
}

size_t zw_fwu_get_build(uint8_t *out, size_t cap, const zw_fwu_get_t *in)
{
    if (out == NULL || cap < 5u) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_GET;
    out[2] = in->reports;
    put16(&out[3], (uint16_t)(in->report_number & 0x7FFFu));
    return 5u;
}

size_t zw_fwu_report_build(uint8_t *out, size_t cap, uint16_t report_number, bool last,
                           const uint8_t *data, size_t len, bool with_crc)
{
    const size_t n = 4u + len + (with_crc ? 2u : 0u);
    if (out == NULL || cap < n || (len > 0u && data == NULL) || report_number == 0u ||
        report_number > 0x7FFFu) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_REPORT;
    put16(&out[2], (uint16_t)((last ? 0x8000u : 0u) | (unsigned)report_number));
    if (len > 0u) {
        memcpy(&out[4], data, len);
    }
    if (with_crc) {
        put16(&out[4u + len], zw_crc16(ZW_CRC16_INIT, out, 4u + len));
    }
    return n;
}

bool zw_fwu_report_parse(const uint8_t *p, size_t len, bool with_crc, uint16_t *report_number,
                         bool *last, const uint8_t **data, size_t *data_len)
{
    if (!is(p, len, ZW_FWU_REPORT, with_crc ? 4u : 2u)) {
        return false;
    }
    const size_t body = len - 4u - (with_crc ? 2u : 0u);
    if (with_crc && zw_crc16(ZW_CRC16_INIT, p, len - 2u) != be16(&p[len - 2u])) {
        return false;
    }
    *report_number = (uint16_t)(be16(&p[2]) & 0x7FFFu);
    *last = ((unsigned)p[2] & 0x80u) != 0u;
    *data = &p[4];
    *data_len = body;
    return *report_number != 0u;
}

bool zw_fwu_status_report_parse(const uint8_t *p, size_t len, uint8_t *status, bool *has_wait,
                                uint16_t *wait_s)
{
    if (status == NULL || !is(p, len, ZW_FWU_STATUS_REPORT, 1u)) {
        return false;
    }
    *status = p[2];
    const bool w = len >= 5u;
    if (has_wait != NULL) {
        *has_wait = w;
    }
    if (wait_s != NULL) {
        *wait_s = (uint16_t)(w ? (unsigned)be16(&p[3]) : 0u);
    }
    return true;
}

size_t zw_fwu_status_report_build(uint8_t *out, size_t cap, uint8_t status, uint16_t wait_s)
{
    if (out == NULL || cap < 5u) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_STATUS_REPORT;
    out[2] = status;
    put16(&out[3], wait_s);
    return 5u;
}

size_t zw_fwu_activation_set_build(uint8_t *out, size_t cap, const zw_fwu_activation_t *in)
{
    const size_t n = in->hardware != 0xFFu ? 10u : 9u;
    if (out == NULL || cap < n) {
        return 0u;
    }
    out[0] = ZW_CC_FIRMWARE_UPDATE;
    out[1] = ZW_FWU_ACTIVATION_SET;
    put16(&out[2], in->manufacturer_id);
    put16(&out[4], in->firmware_id);
    put16(&out[6], in->checksum);
    out[8] = in->target;
    if (n == 10u) {
        out[9] = in->hardware;
    }
    return n;
}

bool zw_fwu_activation_report_parse(const uint8_t *p, size_t len, zw_fwu_activation_t *out)
{
    if (out == NULL || !is(p, len, ZW_FWU_ACTIVATION_REPORT, 8u)) {
        return false;
    }
    out->manufacturer_id = be16(&p[2]);
    out->firmware_id = be16(&p[4]);
    out->checksum = be16(&p[6]);
    out->target = p[8];
    out->status = p[9];
    out->hardware = (uint8_t)(len >= 11u ? (unsigned)p[10] : 0xFFu);
    return true;
}

uint16_t zw_fwu_image_checksum(const uint8_t *image, size_t len)
{
    return image != NULL ? zw_crc16(ZW_CRC16_INIT, image, len) : (uint16_t)ZW_CRC16_INIT;
}
