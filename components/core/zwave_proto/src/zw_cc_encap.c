/*
 * zw_cc_encap.c - §1 encapsulation: unwrap one received layer at a time, wrap for sending (see
 * zw_cc_encap.h). Security 2/0 and Transport Service are only recognised, never decoded (that is
 * security.md / zwave_host's job); Supervision and Multi Channel reuse their own CC's codec, CRC-16
 * reuses zw_crc16.h.
 */
#include "zwave_proto/zw_cc_encap.h"

#include "zwave_proto/zw_crc16.h"

bool zw_cc_unwrap(const uint8_t *command, size_t len, zw_cc_unwrap_t *out)
{
    if (command == NULL || len < 1u || out == NULL) {
        return false;
    }
    *out = (zw_cc_unwrap_t){0};
    const uint8_t cc = command[0];

    if (cc == (uint8_t)ZW_CC_SECURITY_2) {
        out->kind = ZW_CC_UNWRAP_SECURITY_2;
        out->inner_len = len - 1u;
        out->inner = out->inner_len > 0u ? &command[1] : NULL;
        return true;
    }
    if (cc == (uint8_t)ZW_CC_SECURITY_0) {
        out->kind = ZW_CC_UNWRAP_SECURITY_0;
        out->inner_len = len - 1u;
        out->inner = out->inner_len > 0u ? &command[1] : NULL;
        return true;
    }
    if (cc == (uint8_t)ZW_CC_TRANSPORT_SERVICE) {
        /* not supported in v1 (§1: "drop and count"): recognised, nothing to hand onward. */
        out->kind = ZW_CC_UNWRAP_TRANSPORT_SERVICE;
        return true;
    }
    if (len >= 2u && cc == (uint8_t)ZW_CC_SUPERVISION && command[1] == ZW_CMD_SUPERVISION_GET) {
        zw_cc_supervision_get_t g;
        if (!zw_cc_supervision_get_parse(command, len, &g)) {
            return false;
        }
        out->kind = ZW_CC_UNWRAP_SUPERVISION;
        out->supervision_status_updates_wanted = g.status_updates_wanted;
        out->supervision_session_id = g.session_id;
        out->inner = g.command;
        out->inner_len = g.command_len;
        return true;
    }
    if (len >= 2u && cc == (uint8_t)ZW_CC_MULTI_CHANNEL && command[1] == ZW_CMD_MULTI_CHANNEL_COMMAND_ENCAPSULATION) {
        zw_cc_multi_channel_command_encapsulation_t m;
        if (!zw_cc_multi_channel_command_encapsulation_parse(command, len, &m)) {
            return false;
        }
        out->kind = ZW_CC_UNWRAP_MULTI_CHANNEL;
        out->source_endpoint = m.source_endpoint;
        out->destination_endpoint = m.destination;
        out->destination_is_mask = m.destination_is_mask;
        out->inner = m.command;
        out->inner_len = m.command_len;
        return true;
    }
    if (len >= 2u && cc == (uint8_t)ZW_CC_CRC16_ENCAPSULATION && command[1] == ZW_CMD_CRC16_ENCAPSULATE) {
        if (len < 4u) {
            return false;
        }
        const uint16_t computed = zw_crc16(ZW_CRC16_INIT, command, len - 2u);
        const uint16_t got = (uint16_t)(((uint16_t)command[len - 2u] << 8) | command[len - 1u]);
        if (computed != got) {
            return false; /* §1: CRC mismatch - refuse the whole command */
        }
        out->kind = ZW_CC_UNWRAP_CRC16;
        out->inner_len = len - 4u;
        out->inner = out->inner_len > 0u ? &command[2] : NULL;
        return true;
    }
    if (len >= 2u && cc == (uint8_t)ZW_CC_MULTI_COMMAND && command[1] == ZW_CMD_MULTI_COMMAND_ENCAPSULATE) {
        if (len < 3u) {
            return false;
        }
        out->kind = ZW_CC_UNWRAP_MULTI_COMMAND;
        out->inner = &command[2]; /* count | count x (length . command), for the iterator below */
        out->inner_len = len - 2u;
        return true;
    }

    out->kind = ZW_CC_UNWRAP_NONE;
    out->inner = command;
    out->inner_len = len;
    return true;
}

bool zw_cc_multi_command_iter_init(zw_cc_multi_command_iter_t *it, const uint8_t *payload, size_t len)
{
    if (it == NULL || payload == NULL || len < 1u) {
        return false;
    }
    it->remaining = payload[0];
    it->pos = &payload[1];
    it->end = payload + len;
    return true;
}

bool zw_cc_multi_command_iter_next(zw_cc_multi_command_iter_t *it, const uint8_t **command,
                                    size_t *command_len)
{
    if (it == NULL || command == NULL || command_len == NULL || it->pos == NULL || it->end == NULL) {
        return false;
    }
    if (it->remaining == 0u) {
        return false;
    }
    if ((size_t)(it->end - it->pos) < 1u) {
        return false;
    }
    const uint8_t clen = it->pos[0];
    if ((size_t)(it->end - it->pos) < 1u + (size_t)clen) {
        return false;
    }
    *command = &it->pos[1];
    *command_len = clen;
    it->pos += 1u + (size_t)clen;
    it->remaining = (uint8_t)((unsigned)it->remaining - 1u);
    return true;
}

size_t zw_cc_wrap_multi_channel(uint8_t *out, size_t cap, uint8_t source_endpoint, uint8_t destination,
                                 bool destination_is_mask, const uint8_t *inner, size_t inner_len)
{
    const zw_cc_multi_channel_command_encapsulation_t in = {
        .source_endpoint = source_endpoint,
        .destination_is_mask = destination_is_mask,
        .destination = destination,
        .command = inner,
        .command_len = inner_len,
    };
    return zw_cc_multi_channel_command_encapsulation_build(out, cap, &in);
}

size_t zw_cc_wrap_supervision(uint8_t *out, size_t cap, uint8_t session_id, bool status_updates_wanted,
                               const uint8_t *inner, size_t inner_len)
{
    const zw_cc_supervision_get_t in = {
        .status_updates_wanted = status_updates_wanted,
        .session_id = (uint8_t)(session_id & ZW_SUPERVISION_SESSION_ID_MASK),
        .command = inner,
        .command_len = inner_len,
    };
    return zw_cc_supervision_get_build(out, cap, &in);
}

size_t zw_cc_wrap_crc16(uint8_t *out, size_t cap, const uint8_t *inner, size_t inner_len)
{
    if (out == NULL || (inner == NULL && inner_len > 0u)) {
        return 0u;
    }
    const size_t total = 4u + inner_len;
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_CRC16_ENCAPSULATION;
    out[1] = ZW_CMD_CRC16_ENCAPSULATE;
    for (size_t i = 0; i < inner_len; i++) {
        out[2u + i] = inner[i];
    }
    const uint16_t crc = zw_crc16(ZW_CRC16_INIT, out, 2u + inner_len);
    out[2u + inner_len] = (uint8_t)(crc >> 8);
    out[3u + inner_len] = (uint8_t)crc;
    return total;
}

size_t zw_cc_wrap_multi_command(uint8_t *out, size_t cap, const uint8_t *const *commands,
                                 const size_t *command_lens, size_t count)
{
    if (out == NULL || (count > 0u && (commands == NULL || command_lens == NULL)) || count > 0xFFu) {
        return 0u;
    }
    size_t total = 3u;
    for (size_t i = 0; i < count; i++) {
        if (command_lens[i] > 0xFFu) {
            return 0u;
        }
        total += 1u + command_lens[i];
    }
    if (cap < total) {
        return 0u;
    }
    out[0] = (uint8_t)ZW_CC_MULTI_COMMAND;
    out[1] = ZW_CMD_MULTI_COMMAND_ENCAPSULATE;
    out[2] = (uint8_t)count;
    size_t pos = 3u;
    for (size_t i = 0; i < count; i++) {
        out[pos] = (uint8_t)command_lens[i];
        pos += 1u;
        for (size_t j = 0; j < command_lens[i]; j++) {
            out[pos + j] = commands[i][j];
        }
        pos += command_lens[i];
    }
    return total;
}
