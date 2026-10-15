/*
 * zw_cc_supervision.c - Supervision (see zw_cc_supervision.h, command-classes.md §4.14).
 */
#include "zwave_proto/zw_cc_supervision.h"

#include "zw_cc_internal.h"

size_t zw_cc_supervision_get_build(uint8_t *out, size_t cap, const zw_cc_supervision_get_t *in)
{
    if (out == NULL || in == NULL || (in->command == NULL && in->command_len > 0u) || in->command_len > 0xFFu) {
        return 0u;
    }
    const size_t total = 4u + in->command_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_SUPERVISION;
    out[1] = ZW_CMD_SUPERVISION_GET;
    out[2] = (uint8_t)((in->status_updates_wanted ? 0x80u : 0x00u) |
                        (in->session_id & ZW_SUPERVISION_SESSION_ID_MASK));
    out[3] = (uint8_t)in->command_len;
    for (size_t i = 0; i < in->command_len; i++) {
        out[4u + i] = in->command[i];
    }
    return total;
}

bool zw_cc_supervision_get_parse(const uint8_t *p, size_t len, zw_cc_supervision_get_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_SUPERVISION, ZW_CMD_SUPERVISION_GET, 4u) || out == NULL) {
        return false;
    }
    const uint8_t flags = p[2];
    const size_t clen = p[3];
    if (len < 4u + clen) {
        return false;
    }
    out->status_updates_wanted = ((unsigned)flags & 0x80u) != 0u;
    out->session_id = (uint8_t)(flags & ZW_SUPERVISION_SESSION_ID_MASK);
    out->command = clen > 0u ? &p[4] : NULL;
    out->command_len = clen;
    return true;
}

size_t zw_cc_supervision_report_build(uint8_t *out, size_t cap, const zw_cc_supervision_report_t *in)
{
    if (out == NULL || in == NULL || cap < 5u) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_SUPERVISION;
    out[1] = ZW_CMD_SUPERVISION_REPORT;
    out[2] = (uint8_t)((in->more_updates_follow ? 0x80u : 0x00u) | (in->wake_up_request ? 0x40u : 0x00u) |
                        (in->session_id & ZW_SUPERVISION_SESSION_ID_MASK));
    out[3] = in->status;
    out[4] = in->duration;
    return 5u;
}

bool zw_cc_supervision_report_parse(const uint8_t *p, size_t len, zw_cc_supervision_report_t *out)
{
    if (!zwi_cc_header_ok(p, len, (uint8_t)ZW_CC_SUPERVISION, ZW_CMD_SUPERVISION_REPORT, 5u) || out == NULL) {
        return false;
    }
    const uint8_t flags = p[2];
    out->more_updates_follow = ((unsigned)flags & 0x80u) != 0u;
    out->wake_up_request = (flags & 0x40u) != 0u;
    out->session_id = (uint8_t)(flags & ZW_SUPERVISION_SESSION_ID_MASK);
    out->status = p[3];
    out->duration = p[4];
    return true;
}
