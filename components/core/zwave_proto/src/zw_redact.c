/*
 * zw_redact.c - credentials kept out of what the panel records about its Z-Wave link. See
 * zw_redact.h.
 */
#include "zwave_proto/zw_redact.h"

#include <string.h>

#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_frame.h"

#define DEPTH_MAX 4u /* clear encapsulations inside one another: more is blanked whole */

bool zw_redact_sensitive_cc(uint8_t cc)
{
    return cc == (uint8_t)ZW_CC_ENTRY_CONTROL || cc == 0x63u /* User Code */ ||
           cc == 0x83u /* User Credential */ || cc == 0x4Cu /* Door Lock Logging */ ||
           cc == 0x98u /* Security 0 */ || cc == (uint8_t)ZW_CC_TRANSPORT_SERVICE;
}

static bool blank(uint8_t *p, size_t n)
{
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        any = any || p[i] != 0u;
        p[i] = 0u;
    }
    return any;
}

static bool redact(uint8_t *c, size_t n, unsigned depth)
{
    if (n < 2u) {
        return false;
    }
    if (depth >= DEPTH_MAX) {
        return blank(&c[1], n - 1u); /* nested past reason: nothing of it trusted */
    }
    const uint8_t cc = c[0];
    const uint8_t cmd = c[1];
    if (zw_redact_sensitive_cc(cc)) {
        return blank(&c[2], n - 2u);
    }
    if (cc == (uint8_t)ZW_CC_MULTI_CHANNEL && cmd == 0x0Du && n > 4u) {
        return redact(&c[4], n - 4u, depth + 1u); /* 60 0D source destination command */
    }
    if (cc == (uint8_t)ZW_CC_SUPERVISION && cmd == 0x01u && n > 4u) {
        return redact(&c[4], n - 4u, depth + 1u); /* 6C 01 properties length command */
    }
    if (cc == (uint8_t)ZW_CC_CRC16_ENCAPSULATION && cmd == 0x01u && n > 4u) {
        return redact(&c[2], n - 4u, depth + 1u); /* 56 01 command CRC(2) */
    }
    if (cc == 0x8Fu && cmd == 0x01u && n > 3u) { /* Multi Command: 8F 01 count (length command)* */
        bool any = false;
        size_t at = 3u;
        for (unsigned i = 0; i < c[2] && at < n; i++) {
            const size_t len = c[at];
            if (len == 0u || at + 1u + len > n) {
                return blank(&c[at], n - at) || any; /* the rest doesn't parse: blanked */
            }
            any = redact(&c[at + 1u], len, depth + 1u) || any;
            at += 1u + len;
        }
        return any;
    }
    return false;
}

bool zw_redact_command(uint8_t *cmd, size_t len)
{
    return cmd != NULL && redact(cmd, len, 0u);
}

/* The command inside a parsed frame, as an offset into the payload. */
static bool command_at(const uint8_t *payload, size_t plen, uint8_t func, zw_nid_width_t w,
                       size_t *at, size_t *n)
{
    if (func == (uint8_t)ZW_FUNC_APPLICATION_COMMAND_HANDLER ||
        func == (uint8_t)ZW_FUNC_BRIDGE_APPLICATION_COMMAND_HANDLER) {
        zw_api_app_cmd_t a;
        if (!zw_api_app_cmd_parse(payload, plen, w, (zw_func_t)func, &a) || a.command == NULL) {
            return false;
        }
        *at = (size_t)(a.command - payload);
        *n = a.command_len;
        return true;
    }
    if (func == (uint8_t)ZW_FUNC_SEND_DATA) {
        zw_api_send_data_req_t s;
        if (!zw_api_send_data_req_parse(payload, plen, w, &s) || s.command == NULL) {
            return false;
        }
        *at = (size_t)(s.command - payload);
        *n = s.command_len;
        return true;
    }
    return false;
}

bool zw_redact_frame(uint8_t *frame, size_t len)
{
    if (frame == NULL || len < 5u || frame[0] != 0x01u) {
        return false; /* ACK/NAK/CAN, text, or nothing that could be a data frame */
    }
    const bool whole = (size_t)frame[1] + 2u == len && frame[1] >= 3u &&
                       zw_frame_checksum(&frame[1], frame[1]) == frame[len - 1u];
    if (!whole) {
        return blank(&frame[4], len - 4u); /* where its command is can't be trusted */
    }
    uint8_t *payload = &frame[4];
    const size_t plen = (size_t)frame[1] - 3u;
    bool changed = false;
    static const zw_nid_width_t WIDTHS[2] = {ZW_NID_8, ZW_NID_16};
    for (size_t i = 0; i < 2u; i++) { /* the width in use isn't known here: both readings */
        size_t at = 0u;
        size_t n = 0u;
        if (command_at(payload, plen, frame[3], WIDTHS[i], &at, &n) && at + n <= plen) {
            changed = zw_redact_command(&payload[at], n) || changed;
        }
    }
    if (changed) {
        frame[len - 1u] = zw_frame_checksum(&frame[1], frame[1]);
    }
    return changed;
}
