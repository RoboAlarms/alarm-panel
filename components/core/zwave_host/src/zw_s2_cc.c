/*
 * zw_s2_cc.c - the Security 2 command class's wire format (security.md §5-7, §9). See
 * zw_s2_cc.h.
 */
#include "zwave_host/zw_s2_cc.h"

#include <string.h>

/* `9F cmd` then n payload bytes: false unless it is that command with at least n bytes. */
static bool is_cmd(const uint8_t *p, size_t len, uint8_t cmd, size_t n)
{
    return p != NULL && len >= 2u + n && p[0] == ZW_S2_CC && p[1] == cmd;
}

/* Writes `9F cmd` and n payload bytes (NULL: none); returns 2 + n, or 0 when it does not fit. */
static size_t put_cmd(uint8_t *out, size_t cap, uint8_t cmd, const uint8_t *payload, size_t n)
{
    if (out == NULL || cap < 2u + n || (n > 0u && payload == NULL)) {
        return 0u;
    }
    out[0] = ZW_S2_CC;
    out[1] = cmd;
    if (n > 0u) {
        memmove(&out[2], payload, n);
    }
    return 2u + n;
}

/* ---- The key exchange (§5) ----------------------------------------------------------------- */

size_t zw_s2_kex_get_build(uint8_t *out, size_t cap)
{
    return put_cmd(out, cap, ZW_S2_KEX_GET, NULL, 0u);
}

bool zw_s2_kex_get_parse(const uint8_t *p, size_t len)
{
    return is_cmd(p, len, ZW_S2_KEX_GET, 0u);
}

static size_t kex_build(uint8_t *out, size_t cap, uint8_t cmd, const zw_s2_kex_t *in)
{
    if (in == NULL) {
        return 0u;
    }
    const uint8_t b[4] = {in->flags, in->schemes, in->curves, in->keys};
    return put_cmd(out, cap, cmd, b, sizeof b);
}

static bool kex_parse(const uint8_t *p, size_t len, uint8_t cmd, zw_s2_kex_t *out)
{
    if (out == NULL || !is_cmd(p, len, cmd, 4u)) {
        return false;
    }
    out->flags = p[2];
    out->schemes = p[3];
    out->curves = p[4];
    out->keys = p[5];
    return true;
}

size_t zw_s2_kex_report_build(uint8_t *out, size_t cap, const zw_s2_kex_t *in)
{
    return kex_build(out, cap, ZW_S2_KEX_REPORT, in);
}

bool zw_s2_kex_report_parse(const uint8_t *p, size_t len, zw_s2_kex_t *out)
{
    return kex_parse(p, len, ZW_S2_KEX_REPORT, out);
}

size_t zw_s2_kex_set_build(uint8_t *out, size_t cap, const zw_s2_kex_t *in)
{
    return kex_build(out, cap, ZW_S2_KEX_SET, in);
}

bool zw_s2_kex_set_parse(const uint8_t *p, size_t len, zw_s2_kex_t *out)
{
    return kex_parse(p, len, ZW_S2_KEX_SET, out);
}

size_t zw_s2_kex_fail_build(uint8_t *out, size_t cap, uint8_t type)
{
    return put_cmd(out, cap, ZW_S2_KEX_FAIL, &type, 1u);
}

bool zw_s2_kex_fail_parse(const uint8_t *p, size_t len, uint8_t *type)
{
    if (type == NULL || !is_cmd(p, len, ZW_S2_KEX_FAIL, 1u)) {
        return false;
    }
    *type = p[2];
    return true;
}

size_t zw_s2_pubkey_build(uint8_t *out, size_t cap, const zw_s2_pubkey_t *in)
{
    if (in == NULL || in->key == NULL || out == NULL || cap < 3u + ZW_S2_PUBKEY_LEN) {
        return 0u;
    }
    out[0] = ZW_S2_CC;
    out[1] = ZW_S2_PUBLIC_KEY_REPORT;
    out[2] = in->including ? ZW_S2_PUBKEY_INCLUDING : 0u;
    memmove(&out[3], in->key, ZW_S2_PUBKEY_LEN);
    return 3u + ZW_S2_PUBKEY_LEN;
}

bool zw_s2_pubkey_parse(const uint8_t *p, size_t len, zw_s2_pubkey_t *out)
{
    if (out == NULL || !is_cmd(p, len, ZW_S2_PUBLIC_KEY_REPORT, 1u + ZW_S2_PUBKEY_LEN)) {
        return false;
    }
    out->including = (p[2] & ZW_S2_PUBKEY_INCLUDING) != 0u;
    out->key = &p[3];
    return true;
}

size_t zw_s2_netkey_get_build(uint8_t *out, size_t cap, uint8_t key)
{
    return put_cmd(out, cap, ZW_S2_NETWORK_KEY_GET, &key, 1u);
}

bool zw_s2_netkey_get_parse(const uint8_t *p, size_t len, uint8_t *key)
{
    if (key == NULL || !is_cmd(p, len, ZW_S2_NETWORK_KEY_GET, 1u)) {
        return false;
    }
    *key = p[2];
    return true;
}

size_t zw_s2_netkey_report_build(uint8_t *out, size_t cap, const zw_s2_netkey_report_t *in)
{
    if (in == NULL || in->netkey == NULL || out == NULL || cap < 3u + ZW_S2_NETKEY_LEN) {
        return 0u;
    }
    out[0] = ZW_S2_CC;
    out[1] = ZW_S2_NETWORK_KEY_REPORT;
    out[2] = in->key;
    memmove(&out[3], in->netkey, ZW_S2_NETKEY_LEN);
    return 3u + ZW_S2_NETKEY_LEN;
}

bool zw_s2_netkey_report_parse(const uint8_t *p, size_t len, zw_s2_netkey_report_t *out)
{
    if (out == NULL || !is_cmd(p, len, ZW_S2_NETWORK_KEY_REPORT, 1u + ZW_S2_NETKEY_LEN)) {
        return false;
    }
    out->key = p[2];
    out->netkey = &p[3];
    return true;
}

size_t zw_s2_netkey_verify_build(uint8_t *out, size_t cap)
{
    return put_cmd(out, cap, ZW_S2_NETWORK_KEY_VERIFY, NULL, 0u);
}

bool zw_s2_netkey_verify_parse(const uint8_t *p, size_t len)
{
    return is_cmd(p, len, ZW_S2_NETWORK_KEY_VERIFY, 0u);
}

size_t zw_s2_transfer_end_build(uint8_t *out, size_t cap, uint8_t flags)
{
    return put_cmd(out, cap, ZW_S2_TRANSFER_END, &flags, 1u);
}

bool zw_s2_transfer_end_parse(const uint8_t *p, size_t len, uint8_t *flags)
{
    if (flags == NULL || !is_cmd(p, len, ZW_S2_TRANSFER_END, 1u)) {
        return false;
    }
    *flags = p[2];
    return true;
}

/* ---- Nonces (§6) --------------------------------------------------------------------------- */

size_t zw_s2_nonce_get_build(uint8_t *out, size_t cap, uint8_t seq)
{
    return put_cmd(out, cap, ZW_S2_NONCE_GET, &seq, 1u);
}

bool zw_s2_nonce_get_parse(const uint8_t *p, size_t len, uint8_t *seq)
{
    if (seq == NULL || !is_cmd(p, len, ZW_S2_NONCE_GET, 1u)) {
        return false;
    }
    *seq = p[2];
    return true;
}

size_t zw_s2_nonce_report_build(uint8_t *out, size_t cap, const zw_s2_nonce_report_t *in)
{
    if (in == NULL || out == NULL) {
        return 0u;
    }
    const bool sos = (in->flags & ZW_S2_NONCE_SOS) != 0u;
    const size_t n = 4u + (sos ? ZW_S2_EI_BYTES : 0u);
    if (cap < n || (sos && in->receiver_ei == NULL)) {
        return 0u;
    }
    out[0] = ZW_S2_CC;
    out[1] = ZW_S2_NONCE_REPORT;
    out[2] = in->seq;
    out[3] = in->flags;
    if (sos) {
        memmove(&out[4], in->receiver_ei, ZW_S2_EI_BYTES);
    }
    return n;
}

bool zw_s2_nonce_report_parse(const uint8_t *p, size_t len, zw_s2_nonce_report_t *out)
{
    if (out == NULL || !is_cmd(p, len, ZW_S2_NONCE_REPORT, 2u)) {
        return false;
    }
    out->seq = p[2];
    out->flags = p[3];
    out->receiver_ei = NULL;
    if ((out->flags & ZW_S2_NONCE_SOS) != 0u) {
        if (len < 4u + ZW_S2_EI_BYTES) {
            return false;
        }
        out->receiver_ei = &p[4];
    }
    return true;
}

/* ---- Message Encapsulation (§7) ------------------------------------------------------------ */

bool zw_s2_ext_parse(const uint8_t *p, size_t len, bool encrypted, zw_s2_ext_t *out, size_t *used)
{
    if (out == NULL || used == NULL || (p == NULL && len > 0u)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    size_t pos = 0u;
    bool more = true;
    while (more) {
        if (len - pos < 2u) {
            return false; /* an extension announced but not there */
        }
        const size_t l = p[pos];
        const uint8_t flags = p[pos + 1u];
        if (l < 2u || l > len - pos) {
            return false;
        }
        const uint8_t *data = &p[pos + 2u];
        switch (flags & ZW_S2_EXT_TYPE) {
        case ZW_S2_EXT_SPAN:
            if (!encrypted && l == ZW_S2_EXT_SPAN_LEN && !out->span) {
                out->span = true;
                out->sender_ei = data;
            }
            break;
        case ZW_S2_EXT_MPAN:
            if (encrypted && l == ZW_S2_EXT_MPAN_LEN) {
                out->mpan = true;
            }
            break;
        case ZW_S2_EXT_MGRP:
            if (!encrypted && l == ZW_S2_EXT_MGRP_LEN && !out->mgrp) {
                out->mgrp = true;
                out->group = data[0];
            }
            break;
        case ZW_S2_EXT_MOS:
            if (!encrypted && l == ZW_S2_EXT_MOS_LEN) {
                out->mos = true;
            }
            break;
        default:
            if ((flags & ZW_S2_EXT_CRITICAL) != 0u) {
                return false; /* an extension it must understand and does not */
            }
            break;
        }
        pos += l;
        more = ((unsigned)flags & ZW_S2_EXT_MORE) != 0u;
    }
    *used = pos;
    return true;
}

bool zw_s2_encap_parse(const uint8_t *p, size_t len, zw_s2_encap_t *out)
{
    if (out == NULL || !is_cmd(p, len, ZW_S2_MESSAGE_ENCAPSULATION, 2u + ZW_S2_TAG_BYTES)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->seq = p[2];
    out->flags = p[3];
    size_t used = 0u;
    /* the extensions may not run into the tag */
    const size_t room = len - ZW_S2_ENCAP_HEADER - ZW_S2_TAG_BYTES;
    if ((out->flags & ZW_S2_ENCAP_EXT) != 0u &&
        !zw_s2_ext_parse(&p[ZW_S2_ENCAP_HEADER], room, false, &out->found, &used)) {
        return false;
    }
    out->ext = &p[ZW_S2_ENCAP_HEADER];
    out->ext_len = used;
    out->ciphertext = &p[ZW_S2_ENCAP_HEADER + used];
    out->ciphertext_len = room - used;
    out->tag = &p[len - ZW_S2_TAG_BYTES];
    return true;
}

size_t zw_s2_encap_header_build(uint8_t *out, size_t cap, uint8_t seq, const uint8_t *sender_ei)
{
    const size_t n = ZW_S2_ENCAP_HEADER + (sender_ei != NULL ? ZW_S2_EXT_SPAN_LEN : 0u);
    if (out == NULL || cap < n) {
        return 0u;
    }
    out[0] = ZW_S2_CC;
    out[1] = ZW_S2_MESSAGE_ENCAPSULATION;
    out[2] = seq;
    out[3] = sender_ei != NULL ? ZW_S2_ENCAP_EXT : 0u;
    if (sender_ei != NULL) {
        out[4] = ZW_S2_EXT_SPAN_LEN;
        out[5] = ZW_S2_EXT_CRITICAL | ZW_S2_EXT_SPAN;
        memmove(&out[6], sender_ei, ZW_S2_EI_BYTES);
    }
    return n;
}

size_t zw_s2_aad_build(uint8_t *out, size_t cap, uint16_t sender, uint16_t destination,
                       uint32_t home_id, uint16_t message_len, const uint8_t *hdr,
                       size_t hdr_len)
{
    const bool wide = sender >= 256u || destination >= 256u;
    const size_t ids = wide ? 4u : 2u;
    const size_t n = ids + 6u + hdr_len;
    if (out == NULL || cap < n || (hdr == NULL && hdr_len > 0u)) {
        return 0u;
    }
    size_t i = 0u;
    if (wide) {
        out[i++] = (uint8_t)(sender >> 8);
        out[i++] = (uint8_t)sender;
        out[i++] = (uint8_t)(destination >> 8);
        out[i++] = (uint8_t)destination;
    } else {
        out[i++] = (uint8_t)sender;
        out[i++] = (uint8_t)destination;
    }
    out[i++] = (uint8_t)(home_id >> 24);
    out[i++] = (uint8_t)(home_id >> 16);
    out[i++] = (uint8_t)(home_id >> 8);
    out[i++] = (uint8_t)home_id;
    out[i++] = (uint8_t)(message_len >> 8);
    out[i++] = (uint8_t)message_len;
    if (hdr_len > 0u) {
        memmove(&out[i], hdr, hdr_len);
    }
    return n;
}

/* ---- Commands Supported (§9) --------------------------------------------------------------- */

size_t zw_s2_commands_supported_get_build(uint8_t *out, size_t cap)
{
    return put_cmd(out, cap, ZW_S2_COMMANDS_SUPPORTED_GET, NULL, 0u);
}

bool zw_s2_commands_supported_get_parse(const uint8_t *p, size_t len)
{
    return is_cmd(p, len, ZW_S2_COMMANDS_SUPPORTED_GET, 0u);
}

size_t zw_s2_commands_supported_report_build(uint8_t *out, size_t cap, const uint8_t *ccs,
                                             size_t n)
{
    return put_cmd(out, cap, ZW_S2_COMMANDS_SUPPORTED_REPORT, ccs, n);
}

bool zw_s2_commands_supported_report_parse(const uint8_t *p, size_t len, const uint8_t **ccs,
                                           size_t *n)
{
    if (ccs == NULL || n == NULL || !is_cmd(p, len, ZW_S2_COMMANDS_SUPPORTED_REPORT, 0u)) {
        return false;
    }
    *n = len - 2u;
    *ccs = *n > 0u ? &p[2] : NULL;
    return true;
}
