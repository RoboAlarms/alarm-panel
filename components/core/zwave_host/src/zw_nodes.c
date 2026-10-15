/*
 * zw_nodes.c - the node cache codec and reconciliation (see zw_nodes.h, network.md §2).
 *
 * Decoding is two passes over the same bytes, never one: pass 1 (`run_pass()` with `cache ==
 * NULL`) checks every refusal rule using only small stack scratch (up to ZW_NODES_MAX seen node
 * IDs, and small per-current-node counters) and touches no struct at all; only once it has
 * agreed the whole blob is well formed does pass 2 (`cache != NULL`) walk the identical bytes
 * again and actually fill the caller's cache, which by then cannot fail. This is what makes "a
 * refused blob changes nothing in the cache you pass" possible without malloc (a full-size
 * scratch copy of `zw_node_cache_t` would be tens of kilobytes - too large for a task stack) and
 * without a mutable global (a `static` scratch copy would itself be exactly that).
 */
#include <string.h>

#include "zwave_host/zw_nodes.h"

#define HDR_LEN 16u
#define ZW_NODES_BLOB_FORMAT 1u
#define ZW_NODES_BLOB_COMPAT 1u

/* ---- little-endian header/field helpers (this blob's own convention - see zw_nodes.h) ----------- */

static uint32_t rd_le(const uint8_t *p, unsigned n)
{
    uint32_t v = 0u;
    for (unsigned i = 0; i < n; i++) {
        v |= (uint32_t)p[i] << (8u * i);
    }
    return v;
}

static void wr_le(uint8_t *p, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)rd_le(p, 2u);
}

/* ---- CRC-32: the same polynomial and algorithm as alarm_core/src/codec.c's alarm_crc32() and
 * zone_hub/src/bind_codec.c's blob (CRC-32/ISO-HDLC, aka zlib's crc32: poly 0xEDB88320 reflected,
 * init/final complement) - a local copy, not a shared dependency (zwave_host depends on nothing
 * but zwave_proto, docs/features/36 "Separation rules"). Exposed (not `static`) so a test can
 * check it against the standard's own published check value. ---------------------------------- */

uint32_t zw_nodes_crc32(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t nibble[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ nibble[crc & 15u];
        crc = (crc >> 4) ^ nibble[crc & 15u];
    }
    return ~crc;
}

/* ---- accessors ------------------------------------------------------------------------------- */

void zw_node_cache_init(zw_node_cache_t *cache)
{
    memset(cache, 0, sizeof(*cache));
}

zw_node_record_t *zw_node_find(zw_node_cache_t *cache, uint16_t node_id)
{
    if (cache == NULL || node_id == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < cache->node_count; i++) {
        if (cache->nodes[i].node_id == node_id) {
            return &cache->nodes[i];
        }
    }
    return NULL;
}

const zw_node_record_t *zw_node_find_const(const zw_node_cache_t *cache, uint16_t node_id)
{
    return zw_node_find((zw_node_cache_t *)cache, node_id);
}

zw_node_record_t *zw_node_add(zw_node_cache_t *cache, uint16_t node_id)
{
    if (cache == NULL || node_id == 0u || cache->node_count >= ZW_NODES_MAX ||
        zw_node_find(cache, node_id) != NULL) {
        return NULL;
    }
    zw_node_record_t *rec = &cache->nodes[cache->node_count];
    memset(rec, 0, sizeof(*rec));
    rec->node_id = node_id;
    rec->security_classes = ZW_NODE_SECURITY_UNKNOWN;
    cache->node_count++;
    return rec;
}

bool zw_node_remove(zw_node_cache_t *cache, uint16_t node_id)
{
    if (cache == NULL) {
        return false;
    }
    for (size_t i = 0; i < cache->node_count; i++) {
        if (cache->nodes[i].node_id == node_id) {
            const size_t rest = (size_t)cache->node_count - i - 1u;
            if (rest > 0u) {
                memmove(&cache->nodes[i], &cache->nodes[i + 1u], rest * sizeof(zw_node_record_t));
            }
            cache->node_count--;
            memset(&cache->nodes[cache->node_count], 0, sizeof(zw_node_record_t));
            return true;
        }
    }
    return false;
}

zw_node_record_t *zw_node_at(zw_node_cache_t *cache, size_t index)
{
    if (cache == NULL || index >= cache->node_count) {
        return NULL;
    }
    return &cache->nodes[index];
}

const zw_node_record_t *zw_node_at_const(const zw_node_cache_t *cache, size_t index)
{
    return zw_node_at((zw_node_cache_t *)cache, index);
}

/* ---- encoding -------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} writer_t;

static void put(writer_t *w, const void *data, size_t n)
{
    if (n == 0u) {
        return;
    }
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void put_item(writer_t *w, uint8_t field, const void *value, size_t len)
{
    const uint8_t head[3] = {field, 0x01u, (uint8_t)len};
    put(w, head, sizeof(head));
    put(w, value, len);
}

static void put_le_item(writer_t *w, uint8_t field, uint32_t v, unsigned width)
{
    uint8_t b[4];
    wr_le(b, v, width);
    put_item(w, field, b, width);
}

static void put_endpoint(writer_t *w, const zw_node_endpoint_t *ep, uint8_t e)
{
    if (ep->has_class) {
        uint8_t v[4u + ZW_NODE_ENDPOINT_CC_MAX];
        v[0] = e;
        v[1] = ep->generic_class;
        v[2] = ep->specific_class;
        v[3] = ep->cc_count;
        memcpy(&v[4], ep->ccs, ep->cc_count);
        put_item(w, 0x0Au, v, 4u + (size_t)ep->cc_count);
    }
    for (uint8_t n = 0; n < ep->notification_count; n++) {
        const zw_node_notification_t *nt = &ep->notifications[n];
        uint8_t v[3u + ZW_NODE_MASK_BYTES];
        v[0] = e;
        v[1] = nt->notification_type;
        v[2] = nt->event_mask_len;
        memcpy(&v[3], nt->event_mask, nt->event_mask_len);
        put_item(w, 0x0Bu, v, 3u + (size_t)nt->event_mask_len);
    }
    if (ep->has_binary_sensor_mask) {
        uint8_t v[2u + ZW_NODE_MASK_BYTES];
        v[0] = e;
        v[1] = ep->binary_sensor_mask_len;
        memcpy(&v[2], ep->binary_sensor_mask, ep->binary_sensor_mask_len);
        put_item(w, 0x0Cu, v, 2u + (size_t)ep->binary_sensor_mask_len);
    }
    if (ep->has_multilevel_mask) {
        uint8_t v[2u + ZW_NODE_MASK_BYTES];
        v[0] = e;
        v[1] = ep->multilevel_mask_len;
        memcpy(&v[2], ep->multilevel_mask, ep->multilevel_mask_len);
        put_item(w, 0x0Du, v, 2u + (size_t)ep->multilevel_mask_len);
    }
    if (ep->has_push_pull) {
        const uint8_t v[2] = {e, ep->push_pull_mode};
        put_item(w, 0x10u, v, 2u);
    }
    if (ep->has_alarm_sensor_mask) {
        uint8_t v[2u + ZW_NODE_MASK_BYTES];
        v[0] = e;
        v[1] = ep->alarm_sensor_mask_len;
        memcpy(&v[2], ep->alarm_sensor_mask, ep->alarm_sensor_mask_len);
        put_item(w, 0x12u, v, 2u + (size_t)ep->alarm_sensor_mask_len);
    }
}

/* Whether 'r' already carries a decoded, in-limits value for 'field' (and, for a repeated
 * per-endpoint field, 'endpoint'; for 0B, also its own notification 'type' - several types
 * legitimately coexist per endpoint, so a raw one must wait for a decoded entry of its own exact
 * type, not just any type on the same endpoint) - the same condition put_node()/put_endpoint()
 * use to decide whether to write it. A raw item this record does NOT cover this way is one this
 * build could never apply (either it never heard of the field at all, or the item was beyond this
 * firmware's own limits when it was decoded) - it is written back verbatim. One that IS covered
 * means a later, in-limits decode (or a fresh interview) has since filled the same field for
 * real: the live value supersedes the old raw copy, which is then left out to avoid writing it
 * twice. 'type' is only meaningful for field 0B; every other caller passes 0. */
static bool node_covers_field(const zw_node_record_t *r, uint8_t field, uint8_t endpoint, uint8_t type)
{
    switch (field) {
    case 0x01u: return r->generation != 0u;
    case 0x02u: return r->has_protocol;
    case 0x03u: return r->cc_count > 0u;
    case 0x04u: return r->security_classes != ZW_NODE_SECURITY_UNKNOWN;
    case 0x05u: return r->has_dsk;
    case 0x06u: return r->has_identity;
    case 0x07u: return r->has_zwave_plus;
    case 0x08u: return r->has_wake_up;
    case 0x09u: return r->has_lifeline;
    case 0x0Au: return endpoint < ZW_NODE_ENDPOINTS_MAX && r->endpoints[endpoint].has_class;
    case 0x0Bu: {
        if (endpoint >= ZW_NODE_ENDPOINTS_MAX) {
            return false;
        }
        const zw_node_endpoint_t *ep = &r->endpoints[endpoint];
        for (uint8_t i = 0; i < ep->notification_count; i++) {
            if (ep->notifications[i].notification_type == type) {
                return true;
            }
        }
        return false;
    }
    case 0x0Cu: return endpoint < ZW_NODE_ENDPOINTS_MAX && r->endpoints[endpoint].has_binary_sensor_mask;
    case 0x0Du: return endpoint < ZW_NODE_ENDPOINTS_MAX && r->endpoints[endpoint].has_multilevel_mask;
    case 0x0Eu: return r->interview_step != 0u;
    case 0x0Fu: return r->has_quirk;
    case 0x10u: return endpoint < ZW_NODE_ENDPOINTS_MAX && r->endpoints[endpoint].has_push_pull;
    case 0x11u: return r->has_versions;
    case 0x12u: return endpoint < ZW_NODE_ENDPOINTS_MAX && r->endpoints[endpoint].has_alarm_sensor_mask;
    default: return false; /* a genuinely unknown field: never superseded, always written back */
    }
}

/* 'r->unknown' is a concatenation of complete items (field|group|len|value, unparsed); this walks
 * it and writes back only the ones node_covers_field() says are not superseded by a live value. */
static void put_kept_unknown(writer_t *w, const zw_node_record_t *r)
{
    size_t pos = 0u;
    while (pos < r->unknown_len) {
        const uint8_t *item = &r->unknown[pos];
        const uint8_t field = item[0];
        const uint8_t vlen = item[2];
        const size_t item_len = 3u + (size_t)vlen;
        const bool per_endpoint =
            field == 0x0Au || field == 0x0Bu || field == 0x0Cu || field == 0x0Du || field == 0x10u ||
            field == 0x12u;
        const uint8_t endpoint = (per_endpoint && vlen >= 1u) ? (unsigned)item[3] : 0u;
        const uint8_t type = (field == 0x0Bu && vlen >= 2u) ? (unsigned)item[4] : 0u;
        if (!node_covers_field(r, field, endpoint, type)) {
            put(w, item, item_len);
        }
        pos += item_len;
    }
}

static void put_node(writer_t *w, const zw_node_record_t *r)
{
    uint8_t idbuf[2];
    wr_le(idbuf, r->node_id, 2u);
    put_item(w, 0x00u, idbuf, 2u);

    if (r->generation != 0u) {
        put_le_item(w, 0x01u, r->generation, 2u);
    }
    if (r->has_protocol) {
        const uint8_t v[4] = {
            (uint8_t)((r->is_listening ? 0x01u : 0x00u) | (r->is_flirs ? 0x02u : 0x00u) |
                      (r->is_routing ? 0x04u : 0x00u) | (r->supports_long_range ? 0x08u : 0x00u)),
            r->basic_class, r->generic_class, r->specific_class,
        };
        put_item(w, 0x02u, v, 4u);
    }
    if (r->cc_count > 0u) {
        uint8_t v[(size_t)ZW_NODE_CC_MAX * 3u];
        for (uint8_t k = 0; k < r->cc_count; k++) {
            v[(size_t)k * 3u] = r->ccs[k].cc;
            v[(size_t)k * 3u + 1u] = r->ccs[k].version;
            v[(size_t)k * 3u + 2u] =
                (uint8_t)((r->ccs[k].supported ? 0x01u : 0x00u) | (r->ccs[k].secure ? 0x02u : 0x00u));
        }
        put_item(w, 0x03u, v, (size_t)r->cc_count * 3u);
    }
    if (r->security_classes != ZW_NODE_SECURITY_UNKNOWN) {
        put_item(w, 0x04u, &r->security_classes, 1u);
    }
    if (r->has_dsk) {
        put_item(w, 0x05u, r->dsk, ZW_NODE_DSK_BYTES);
    }
    if (r->has_identity) {
        uint8_t v[6];
        wr_le(&v[0], r->manufacturer_id, 2u);
        wr_le(&v[2], r->product_type, 2u);
        wr_le(&v[4], r->product_id, 2u);
        put_item(w, 0x06u, v, 6u);
    }
    if (r->has_zwave_plus) {
        uint8_t v[6];
        v[0] = r->zwave_plus_role_type;
        v[1] = r->zwave_plus_node_type;
        wr_le(&v[2], r->installer_icon, 2u);
        wr_le(&v[4], r->user_icon, 2u);
        put_item(w, 0x07u, v, 6u);
    }
    if (r->has_wake_up) {
        uint8_t v[17];
        wr_le(&v[0], r->wake_up_interval_seconds, 4u);
        wr_le(&v[4], r->wake_up_min_seconds, 4u);
        wr_le(&v[8], r->wake_up_max_seconds, 4u);
        wr_le(&v[12], r->wake_up_step_seconds, 4u);
        v[16] = r->wake_up_notify_node_id;
        put_item(w, 0x08u, v, 17u);
    }
    if (r->has_lifeline) {
        const uint8_t v[2] = {r->lifeline_group, r->lifeline_set ? 1u : 0u};
        put_item(w, 0x09u, v, 2u);
    }
    for (uint8_t e = 0; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        put_endpoint(w, &r->endpoints[e], e);
    }
    if (r->interview_step != 0u) {
        put_item(w, 0x0Eu, &r->interview_step, 1u);
    }
    if (r->has_quirk) {
        uint8_t v[6];
        wr_le(&v[0], r->quirk_manufacturer_id, 2u);
        wr_le(&v[2], r->quirk_product_type, 2u);
        wr_le(&v[4], r->quirk_product_id, 2u);
        put_item(w, 0x0Fu, v, 6u);
    }
    if (r->has_versions) {
        /* fw major, minor, hardware, protocol major, minor, target count, (major, minor) each */
        uint8_t v[6u + 2u * ZW_NODE_FW_TARGETS_MAX];
        const uint8_t n = r->fw_target_count <= ZW_NODE_FW_TARGETS_MAX ? r->fw_target_count
                                                                       : ZW_NODE_FW_TARGETS_MAX;
        v[0] = r->fw_major;
        v[1] = r->fw_minor;
        v[2] = r->hw_version;
        v[3] = r->protocol_major;
        v[4] = r->protocol_minor;
        v[5] = n;
        for (uint8_t i = 0; i < n; i++) {
            v[6u + 2u * i] = r->fw_targets[i][0];
            v[7u + 2u * i] = r->fw_targets[i][1];
        }
        put_item(w, 0x11u, v, (uint8_t)(6u + 2u * n));
    }
    put_kept_unknown(w, r);
}

size_t zw_node_cache_encode(const zw_node_cache_t *cache, uint8_t *out, size_t cap)
{
    if (cache == NULL || out == NULL || cap < HDR_LEN || cache->node_count > ZW_NODES_MAX) {
        return 0u;
    }
    writer_t w = {out, cap, HDR_LEN, false};
    for (size_t i = 0; i < cache->node_count; i++) {
        put_node(&w, &cache->nodes[i]);
    }
    put(&w, cache->unknown_groups, cache->unknown_groups_len);
    if (w.overflow) {
        return 0u;
    }
    const size_t payload = w.len - HDR_LEN;
    memcpy(out, "ZWND", 4u);
    out[4] = ZW_NODES_BLOB_FORMAT;
    out[5] = ZW_NODES_BLOB_COMPAT;
    wr_le(out + 6, HDR_LEN, 2u);
    wr_le(out + 8, (uint32_t)payload, 4u);
    wr_le(out + 12, zw_nodes_crc32(0u, out + HDR_LEN, payload), 4u);
    return w.len;
}

/* ---- decoding: see the file header comment for why this is two passes over the same bytes. --- */

static void keep_local(zw_node_cache_t *cache, zw_node_record_t *rec, const uint8_t *item, size_t item_len)
{
    if (item_len > (size_t)ZW_NODE_UNKNOWN_BYTES - (size_t)rec->unknown_len) {
        cache->unknown_lost += item_len;
        return;
    }
    memcpy(rec->unknown + rec->unknown_len, item, item_len);
    rec->unknown_len = (uint16_t)((size_t)rec->unknown_len + item_len);
}

static void keep_global(zw_node_cache_t *cache, const uint8_t *item, size_t item_len)
{
    if (item_len > (size_t)ZW_CACHE_UNKNOWN_GROUP_BYTES - cache->unknown_groups_len) {
        cache->unknown_lost += item_len;
        return;
    }
    memcpy(cache->unknown_groups + cache->unknown_groups_len, item, item_len);
    cache->unknown_groups_len += item_len;
}

/* One pass over the payload. 'cache' NULL: validate only, touch no struct (pass 1). 'cache' set:
 * apply into it (pass 2, only run once pass 1 has already agreed the blob is sound). 'seen_ids' is
 * scratch sized ZW_NODES_MAX, reused with 'seen_count' reset to 0 between the two passes; a node
 * beyond that many distinct IDs is preserved as opaque bytes (keep_global()) rather than decoded,
 * and is not itself checked for being a duplicate (a design choice, not a gap either pass hides:
 * distinguishing every possible duplicate among nodes this build cannot represent anyway buys
 * nothing real). */
static bool run_pass(zw_node_cache_t *cache, const uint8_t *p, const uint8_t *end, uint16_t *seen_ids,
                      size_t *seen_count)
{
    bool have_cur = false;
    bool overflow = false;
    uint8_t notif_count[ZW_NODE_ENDPOINTS_MAX] = {0};
    zw_node_record_t *cur = NULL;

    while (p < end) {
        if ((size_t)(end - p) < 3u) {
            return false; /* an item header running past the end */
        }
        const uint8_t field = p[0];
        const uint8_t group = p[1];
        const uint8_t vlen = p[2];
        const uint8_t *val = p + 3;
        if ((size_t)(end - val) < vlen) {
            return false; /* an item's value running past the end */
        }
        const uint8_t *item = p;
        const size_t item_len = 3u + (size_t)vlen;
        p = val + vlen;

        if (group == 0x01u && field == 0x00u) {
            if (vlen != 2u) {
                return false;
            }
            const uint16_t node_id = rd_le16(val);
            if (node_id == 0u) {
                return false;
            }
            for (size_t i = 0; i < *seen_count; i++) {
                if (seen_ids[i] == node_id) {
                    return false; /* a node ID twice */
                }
            }
            have_cur = true;
            memset(notif_count, 0, sizeof(notif_count));
            if (*seen_count < ZW_NODES_MAX) {
                seen_ids[(*seen_count)++] = node_id;
                overflow = false;
                if (cache != NULL) {
                    cur = zw_node_add(cache, node_id);
                    if (cur == NULL) {
                        return false; /* not expected once pass 1 agreed; refuse rather than corrupt */
                    }
                }
            } else {
                overflow = true;
                cur = NULL;
                if (cache != NULL) {
                    cache->over_limit++; /* one whole node beyond ZW_NODES_MAX: kept raw too */
                    keep_global(cache, item, item_len);
                }
            }
            continue;
        }

        if (have_cur && overflow) {
            if (cache != NULL) {
                keep_global(cache, item, item_len);
            }
            continue;
        }
        if (group != 0x01u) {
            if (cache != NULL) {
                keep_global(cache, item, item_len);
            }
            continue;
        }
        if (!have_cur) {
            return false; /* a node field before any node start */
        }

        switch (field) {
        case 0x01u:
            if (vlen != 2u) {
                return false;
            }
            if (cur != NULL) {
                cur->generation = rd_le16(val);
            }
            break;
        case 0x02u:
            if (vlen != 4u) {
                return false;
            }
            if (cur != NULL) {
                cur->has_protocol = true;
                cur->is_listening = (val[0] & 0x01u) != 0u;
                cur->is_flirs = (val[0] & 0x02u) != 0u;
                cur->is_routing = (val[0] & 0x04u) != 0u;
                cur->supports_long_range = (val[0] & 0x08u) != 0u;
                cur->basic_class = val[1];
                cur->generic_class = val[2];
                cur->specific_class = val[3];
            }
            break;
        case 0x03u: {
            if ((unsigned)vlen % 3u != 0u) {
                return false; /* damage: not a whole number of (cc, version, flags) triples */
            }
            const uint8_t count = (uint8_t)((unsigned)vlen / 3u);
            if (count > ZW_NODE_CC_MAX) {
                /* well formed, just more CCs than this firmware holds: kept raw, not refused */
                if (cache != NULL) {
                    cache->over_limit++;
                    keep_local(cache, cur, item, item_len);
                }
                break;
            }
            if (cur != NULL) {
                cur->cc_count = count;
                for (uint8_t k = 0; k < count; k++) {
                    cur->ccs[k].cc = val[(size_t)k * 3u];
                    cur->ccs[k].version = val[(size_t)k * 3u + 1u];
                    cur->ccs[k].supported = (val[(size_t)k * 3u + 2u] & 0x01u) != 0u;
                    cur->ccs[k].secure = (val[(size_t)k * 3u + 2u] & 0x02u) != 0u;
                }
            }
            break;
        }
        case 0x04u:
            if (vlen != 1u) {
                return false;
            }
            if (cur != NULL) {
                cur->security_classes = val[0];
            }
            break;
        case 0x05u:
            if (vlen != ZW_NODE_DSK_BYTES) {
                return false;
            }
            if (cur != NULL) {
                cur->has_dsk = true;
                memcpy(cur->dsk, val, ZW_NODE_DSK_BYTES);
            }
            break;
        case 0x06u:
            if (vlen != 6u) {
                return false;
            }
            if (cur != NULL) {
                cur->has_identity = true;
                cur->manufacturer_id = rd_le16(&val[0]);
                cur->product_type = rd_le16(&val[2]);
                cur->product_id = rd_le16(&val[4]);
            }
            break;
        case 0x07u:
            if (vlen != 6u) {
                return false;
            }
            if (cur != NULL) {
                cur->has_zwave_plus = true;
                cur->zwave_plus_role_type = val[0];
                cur->zwave_plus_node_type = val[1];
                cur->installer_icon = rd_le16(&val[2]);
                cur->user_icon = rd_le16(&val[4]);
            }
            break;
        case 0x08u:
            if (vlen != 17u) {
                return false;
            }
            if (cur != NULL) {
                cur->has_wake_up = true;
                cur->wake_up_interval_seconds = rd_le(&val[0], 4u);
                cur->wake_up_min_seconds = rd_le(&val[4], 4u);
                cur->wake_up_max_seconds = rd_le(&val[8], 4u);
                cur->wake_up_step_seconds = rd_le(&val[12], 4u);
                cur->wake_up_notify_node_id = val[16];
            }
            break;
        case 0x09u:
            if (vlen != 2u || val[1] > 1u) {
                return false;
            }
            if (cur != NULL) {
                cur->has_lifeline = true;
                cur->lifeline_group = val[0];
                cur->lifeline_set = val[1] != 0u;
            }
            break;
        case 0x0Au: {
            if (vlen < 4u) {
                return false; /* damage: can't even read the fixed part */
            }
            const uint8_t ep_idx = val[0];
            const uint8_t cc_count = val[3];
            if (vlen != 4u + (size_t)cc_count) {
                return false; /* damage: length contradicts the field's own declared count */
            }
            if (ep_idx >= ZW_NODE_ENDPOINTS_MAX || cc_count > ZW_NODE_ENDPOINT_CC_MAX) {
                /* well formed, just an endpoint or a CC list beyond this firmware: kept raw */
                if (cache != NULL) {
                    cache->over_limit++;
                    keep_local(cache, cur, item, item_len);
                }
                break;
            }
            if (cur != NULL) {
                zw_node_endpoint_t *ep = &cur->endpoints[ep_idx];
                ep->used = true;
                ep->has_class = true;
                ep->generic_class = val[1];
                ep->specific_class = val[2];
                ep->cc_count = cc_count;
                memcpy(ep->ccs, &val[4], cc_count);
            }
            break;
        }
        case 0x0Bu: {
            if (vlen < 3u) {
                return false; /* damage: can't even read the fixed part */
            }
            const uint8_t ep_idx = val[0];
            const uint8_t ntype = val[1];
            const uint8_t mlen = val[2];
            if (vlen != 3u + (size_t)mlen) {
                return false; /* damage: length contradicts the field's own declared mask length */
            }
            const bool ep_ok = ep_idx < ZW_NODE_ENDPOINTS_MAX;
            const bool room_ok = ep_ok && mlen <= ZW_NODE_MASK_BYTES &&
                                  notif_count[ep_idx] < ZW_NODE_NOTIFICATION_MAX;
            if (!room_ok) {
                /* well formed, just an endpoint, a mask or a notification count beyond this
                 * firmware: kept raw, not refused */
                if (cache != NULL) {
                    cache->over_limit++;
                    keep_local(cache, cur, item, item_len);
                }
                break;
            }
            notif_count[ep_idx]++;
            if (cur != NULL) {
                zw_node_endpoint_t *ep = &cur->endpoints[ep_idx];
                ep->used = true;
                zw_node_notification_t *nt = &ep->notifications[ep->notification_count++];
                nt->notification_type = ntype;
                nt->event_mask_len = mlen;
                memcpy(nt->event_mask, &val[3], mlen);
            }
            break;
        }
        case 0x0Cu:
        case 0x0Du:
        case 0x12u: { /* the same shape: endpoint, mask length, mask */
            if (vlen < 2u) {
                return false; /* damage: can't even read the fixed part */
            }
            const uint8_t ep_idx = val[0];
            const uint8_t mlen = val[1];
            if (vlen != 2u + (size_t)mlen) {
                return false; /* damage: length contradicts the field's own declared mask length */
            }
            if (ep_idx >= ZW_NODE_ENDPOINTS_MAX || mlen > ZW_NODE_MASK_BYTES) {
                /* well formed, just an endpoint or a mask beyond this firmware: kept raw */
                if (cache != NULL) {
                    cache->over_limit++;
                    keep_local(cache, cur, item, item_len);
                }
                break;
            }
            if (cur != NULL) {
                zw_node_endpoint_t *ep = &cur->endpoints[ep_idx];
                ep->used = true;
                if (field == 0x0Cu) {
                    ep->has_binary_sensor_mask = true;
                    ep->binary_sensor_mask_len = mlen;
                    memcpy(ep->binary_sensor_mask, &val[2], mlen);
                } else if (field == 0x0Du) {
                    ep->has_multilevel_mask = true;
                    ep->multilevel_mask_len = mlen;
                    memcpy(ep->multilevel_mask, &val[2], mlen);
                } else {
                    ep->has_alarm_sensor_mask = true;
                    ep->alarm_sensor_mask_len = mlen;
                    memcpy(ep->alarm_sensor_mask, &val[2], mlen);
                }
            }
            break;
        }
        case 0x0Eu:
            if (vlen != 1u) {
                return false;
            }
            if (cur != NULL) {
                cur->interview_step = val[0];
            }
            break;
        case 0x0Fu:
            if (vlen != 6u) {
                return false;
            }
            if (cur != NULL) {
                cur->has_quirk = true;
                cur->quirk_manufacturer_id = rd_le16(&val[0]);
                cur->quirk_product_type = rd_le16(&val[2]);
                cur->quirk_product_id = rd_le16(&val[4]);
            }
            break;
        case 0x10u: {
            if (vlen != 2u || val[1] > 1u) {
                return false; /* damage: not the field's own boolean shape */
            }
            const uint8_t ep_idx = val[0];
            if (ep_idx >= ZW_NODE_ENDPOINTS_MAX) {
                /* well formed, just an endpoint beyond this firmware: kept raw, not refused */
                if (cache != NULL) {
                    cache->over_limit++;
                    keep_local(cache, cur, item, item_len);
                }
                break;
            }
            if (cur != NULL) {
                zw_node_endpoint_t *ep = &cur->endpoints[ep_idx];
                ep->used = true;
                ep->has_push_pull = true;
                ep->push_pull_mode = val[1];
            }
            break;
        }
        case 0x11u: {
            if (vlen < 6u || vlen != 6u + 2u * (size_t)val[5]) {
                return false; /* damage: not the field's own shape */
            }
            if (val[5] > ZW_NODE_FW_TARGETS_MAX) {
                if (cache != NULL) { /* well formed, more targets than kept: raw */
                    cache->over_limit++;
                    keep_local(cache, cur, item, item_len);
                }
                break;
            }
            if (cur != NULL) {
                cur->has_versions = true;
                cur->fw_major = val[0];
                cur->fw_minor = val[1];
                cur->hw_version = val[2];
                cur->protocol_major = val[3];
                cur->protocol_minor = val[4];
                cur->fw_target_count = val[5];
                for (uint8_t i = 0; i < val[5]; i++) {
                    cur->fw_targets[i][0] = val[6u + 2u * i];
                    cur->fw_targets[i][1] = val[7u + 2u * i];
                }
            }
            break;
        }
        default:
            if (cache != NULL && cur != NULL) {
                keep_local(cache, cur, item, item_len);
            }
            break;
        }
    }
    return true;
}

bool zw_node_cache_decode(zw_node_cache_t *cache, const uint8_t *buf, size_t len)
{
    if (cache == NULL) {
        return false;
    }
    if (buf == NULL || len < HDR_LEN || memcmp(buf, "ZWND", 4u) != 0 || buf[5] > ZW_NODES_BLOB_COMPAT) {
        return false;
    }
    const uint32_t hdr = rd_le(buf + 6, 2u);
    const uint32_t payload = rd_le(buf + 8, 4u);
    if (hdr < HDR_LEN || hdr > (uint32_t)len || payload > (uint32_t)len - hdr) {
        return false;
    }
    if (zw_nodes_crc32(0u, buf + hdr, (size_t)payload) != rd_le(buf + 12, 4u)) {
        return false;
    }

    const uint8_t *p = buf + hdr;
    const uint8_t *end = p + payload;
    uint16_t seen_ids[ZW_NODES_MAX];
    size_t seen_count = 0u;
    if (!run_pass(NULL, p, end, seen_ids, &seen_count)) {
        return false; /* refused: 'cache' was never touched */
    }

    zw_node_cache_init(cache);
    seen_count = 0u;
    if (!run_pass(cache, p, end, seen_ids, &seen_count)) {
        zw_node_cache_init(cache); /* not expected - pass 1 already agreed - but never half-apply */
        return false;
    }
    return true;
}

/* ---- reconciliation (network.md §2 "At every start") ------------------------------------------ */

void zw_node_reconcile(uint32_t module_home_id, const uint16_t *module_nodes, size_t module_node_count,
                        bool have_keys, uint32_t keys_home_id, const zw_node_cache_t *cache,
                        zw_reconcile_result_t *out)
{
    out->to_interview_count = 0u;
    out->to_forget_count = 0u;

    if (!have_keys) {
        out->reconcile_case = (module_node_count == 0u) ? ZW_RECONCILE_FIRST_START : ZW_RECONCILE_FOREIGN;
        return;
    }
    if (keys_home_id != module_home_id) {
        out->reconcile_case =
            (module_node_count == 0u) ? ZW_RECONCILE_REPLACED_OR_RESET : ZW_RECONCILE_FOREIGN;
        return;
    }

    out->reconcile_case = ZW_RECONCILE_NORMAL;
    for (size_t i = 0; i < module_node_count && out->to_interview_count < ZW_NODES_MAX; i++) {
        if (zw_node_find_const(cache, module_nodes[i]) == NULL) {
            out->to_interview[out->to_interview_count++] = module_nodes[i];
        }
    }
    for (size_t i = 0; i < zw_node_count(cache) && out->to_forget_count < ZW_NODES_MAX; i++) {
        const zw_node_record_t *rec = zw_node_at_const(cache, i);
        bool found = false;
        for (size_t j = 0; j < module_node_count; j++) {
            if (module_nodes[j] == rec->node_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            out->to_forget[out->to_forget_count++] = rec->node_id;
        }
    }
}
