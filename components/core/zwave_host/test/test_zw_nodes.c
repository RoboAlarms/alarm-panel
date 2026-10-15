/*
 * test_zw_nodes.c - the node cache codec and reconciliation (zwave_host/src/zw_nodes.c,
 * docs/features/36 network.md §2; ZW-017).
 *
 * Regenerate the frozen fixture after a deliberate format change with:
 *   ZW_DUMP_NODES=1 build/host/test_zw_nodes
 * which prints ZW_NODES_FIXTURE_V1 as a C array and exits before running any test; paste it into
 * zw_nodes_fixture_v1.h (never edit that file by hand) and check it against network.md §2 again
 * before freezing it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "zwave_host/zw_nodes.h"

void setUp(void) {}
void tearDown(void) {}

#define BUF_MAX 4096u

/* ================================================================================================
 * CRC-32: the standard's own published check value, to prove this local copy is bit-exact with
 * the algorithm alarm_core's and zone_hub's blobs use (CRC-32/ISO-HDLC, aka zlib's crc32).
 * ================================================================================================ */

static void test_crc32_standard_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, zw_nodes_crc32(0u, "123456789", 9u));
}

/* ================================================================================================
 * Accessors
 * ================================================================================================ */

static void test_accessors(void)
{
    zw_node_cache_t cache;
    zw_node_cache_init(&cache);
    TEST_ASSERT_EQUAL_UINT(0u, zw_node_count(&cache));
    TEST_ASSERT_NULL(zw_node_find(&cache, 5u));

    zw_node_record_t *a = zw_node_add(&cache, 5u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT16(5u, a->node_id);
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, a->security_classes); /* fresh default */
    TEST_ASSERT_NULL(zw_node_add(&cache, 5u));  /* duplicate refused */
    TEST_ASSERT_NULL(zw_node_add(&cache, 0u));  /* node ID 0 refused */

    zw_node_record_t *b = zw_node_add(&cache, 9u);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT(2u, zw_node_count(&cache));
    TEST_ASSERT_EQUAL_UINT16(5u, zw_node_at(&cache, 0u)->node_id);
    TEST_ASSERT_EQUAL_UINT16(9u, zw_node_at(&cache, 1u)->node_id);
    TEST_ASSERT_NULL(zw_node_at(&cache, 2u));

    TEST_ASSERT_TRUE(zw_node_remove(&cache, 5u));
    TEST_ASSERT_FALSE(zw_node_remove(&cache, 5u)); /* already gone */
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(&cache));
    TEST_ASSERT_EQUAL_UINT16(9u, zw_node_at(&cache, 0u)->node_id); /* the remaining one shifted down */

    /* the limit: ZW_NODES_MAX total, this one plus enough new ones to fill it, then one more */
    for (uint16_t id = 100u; zw_node_count(&cache) < ZW_NODES_MAX; id++) {
        TEST_ASSERT_NOT_NULL(zw_node_add(&cache, id));
    }
    TEST_ASSERT_EQUAL_UINT(ZW_NODES_MAX, zw_node_count(&cache));
    TEST_ASSERT_NULL(zw_node_add(&cache, 999u));
}

/* ================================================================================================
 * A full round trip: every field of every kind this codec knows, in one cache.
 * ================================================================================================ */

static void fill_full_record(zw_node_record_t *r, uint16_t node_id)
{
    memset(r, 0, sizeof(*r));
    r->node_id = node_id;
    r->generation = 3u;
    r->has_protocol = true;
    r->is_listening = true;
    r->is_routing = true;
    r->basic_class = 0x04u;
    r->generic_class = 0x21u;
    r->specific_class = 0x01u;
    r->cc_count = 3u;
    r->ccs[0] = (zw_node_cc_t){.cc = 0x5Eu, .version = 2u, .supported = true, .secure = false};
    r->ccs[1] = (zw_node_cc_t){.cc = 0x71u, .version = 8u, .supported = true, .secure = true};
    r->ccs[2] = (zw_node_cc_t){.cc = 0x80u, .version = 1u, .supported = true, .secure = false};
    r->security_classes = 0x04u;
    r->has_dsk = true;
    for (unsigned i = 0; i < ZW_NODE_DSK_BYTES; i++) {
        r->dsk[i] = (uint8_t)(i + 1u);
    }
    r->has_identity = true;
    r->manufacturer_id = 0x027Au;
    r->product_type = 0x7000u;
    r->product_id = 0xE001u;
    r->has_zwave_plus = true;
    r->zwave_plus_role_type = 0x06u;
    r->zwave_plus_node_type = 0x00u;
    r->installer_icon = 0x0C0Du;
    r->user_icon = 0x0C0Du;
    r->has_wake_up = true;
    r->wake_up_interval_seconds = 3600u;
    r->wake_up_min_seconds = 30u;
    r->wake_up_max_seconds = 86400u;
    r->wake_up_step_seconds = 30u;
    r->wake_up_notify_node_id = 1u;
    r->has_lifeline = true;
    r->lifeline_group = 1u;
    r->lifeline_set = true;
    r->endpoints[1].used = true;
    r->endpoints[1].has_class = true;
    r->endpoints[1].generic_class = 0x21u;
    r->endpoints[1].specific_class = 0x01u;
    r->endpoints[1].cc_count = 2u;
    r->endpoints[1].ccs[0] = 0x30u;
    r->endpoints[1].ccs[1] = 0x71u;
    r->endpoints[1].notification_count = 1u;
    r->endpoints[1].notifications[0].notification_type = 0x07u;
    r->endpoints[1].notifications[0].event_mask_len = 2u;
    r->endpoints[1].notifications[0].event_mask[0] = 0x82u;
    r->endpoints[1].notifications[0].event_mask[1] = 0x00u;
    r->endpoints[1].has_binary_sensor_mask = true;
    r->endpoints[1].binary_sensor_mask_len = 2u;
    r->endpoints[1].binary_sensor_mask[0] = 0x04u;
    r->endpoints[1].has_multilevel_mask = true;
    r->endpoints[1].multilevel_mask_len = 1u;
    r->endpoints[1].multilevel_mask[0] = 0x01u;
    r->endpoints[1].has_push_pull = true;
    r->endpoints[1].push_pull_mode = ZW_NODE_MODE_PUSH;
    r->endpoints[1].has_alarm_sensor_mask = true;
    r->endpoints[1].alarm_sensor_mask_len = 1u;
    r->endpoints[1].alarm_sensor_mask[0] = 0x16u;
    r->interview_step = 16u;
    r->has_quirk = true;
    r->quirk_manufacturer_id = 0x027Au;
    r->quirk_product_type = 0x7000u;
    r->quirk_product_id = 0xE001u;
}

static void assert_full_record_equal(const zw_node_record_t *a, const zw_node_record_t *b)
{
    TEST_ASSERT_EQUAL_UINT16(a->node_id, b->node_id);
    TEST_ASSERT_EQUAL_UINT16(a->generation, b->generation);
    TEST_ASSERT_EQUAL(a->has_protocol, b->has_protocol);
    TEST_ASSERT_EQUAL(a->is_listening, b->is_listening);
    TEST_ASSERT_EQUAL(a->is_flirs, b->is_flirs);
    TEST_ASSERT_EQUAL(a->is_routing, b->is_routing);
    TEST_ASSERT_EQUAL(a->supports_long_range, b->supports_long_range);
    TEST_ASSERT_EQUAL_UINT8(a->basic_class, b->basic_class);
    TEST_ASSERT_EQUAL_UINT8(a->generic_class, b->generic_class);
    TEST_ASSERT_EQUAL_UINT8(a->specific_class, b->specific_class);
    TEST_ASSERT_EQUAL_UINT8(a->cc_count, b->cc_count);
    for (uint8_t i = 0; i < a->cc_count; i++) {
        TEST_ASSERT_EQUAL_HEX8(a->ccs[i].cc, b->ccs[i].cc);
        TEST_ASSERT_EQUAL_UINT8(a->ccs[i].version, b->ccs[i].version);
        TEST_ASSERT_EQUAL(a->ccs[i].supported, b->ccs[i].supported);
        TEST_ASSERT_EQUAL(a->ccs[i].secure, b->ccs[i].secure);
    }
    TEST_ASSERT_EQUAL_HEX8(a->security_classes, b->security_classes);
    TEST_ASSERT_EQUAL(a->has_dsk, b->has_dsk);
    if (a->has_dsk) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(a->dsk, b->dsk, ZW_NODE_DSK_BYTES);
    }
    TEST_ASSERT_EQUAL(a->has_identity, b->has_identity);
    TEST_ASSERT_EQUAL_UINT16(a->manufacturer_id, b->manufacturer_id);
    TEST_ASSERT_EQUAL_UINT16(a->product_type, b->product_type);
    TEST_ASSERT_EQUAL_UINT16(a->product_id, b->product_id);
    TEST_ASSERT_EQUAL(a->has_zwave_plus, b->has_zwave_plus);
    TEST_ASSERT_EQUAL_UINT8(a->zwave_plus_role_type, b->zwave_plus_role_type);
    TEST_ASSERT_EQUAL_UINT8(a->zwave_plus_node_type, b->zwave_plus_node_type);
    TEST_ASSERT_EQUAL_UINT16(a->installer_icon, b->installer_icon);
    TEST_ASSERT_EQUAL_UINT16(a->user_icon, b->user_icon);
    TEST_ASSERT_EQUAL(a->has_wake_up, b->has_wake_up);
    TEST_ASSERT_EQUAL_UINT32(a->wake_up_interval_seconds, b->wake_up_interval_seconds);
    TEST_ASSERT_EQUAL_UINT32(a->wake_up_min_seconds, b->wake_up_min_seconds);
    TEST_ASSERT_EQUAL_UINT32(a->wake_up_max_seconds, b->wake_up_max_seconds);
    TEST_ASSERT_EQUAL_UINT32(a->wake_up_step_seconds, b->wake_up_step_seconds);
    TEST_ASSERT_EQUAL_UINT8(a->wake_up_notify_node_id, b->wake_up_notify_node_id);
    TEST_ASSERT_EQUAL(a->has_lifeline, b->has_lifeline);
    TEST_ASSERT_EQUAL_UINT8(a->lifeline_group, b->lifeline_group);
    TEST_ASSERT_EQUAL(a->lifeline_set, b->lifeline_set);
    for (unsigned e = 0; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        const zw_node_endpoint_t *ea = &a->endpoints[e];
        const zw_node_endpoint_t *eb = &b->endpoints[e];
        TEST_ASSERT_EQUAL(ea->used, eb->used);
        TEST_ASSERT_EQUAL(ea->has_class, eb->has_class);
        if (ea->has_class) {
            TEST_ASSERT_EQUAL_UINT8(ea->generic_class, eb->generic_class);
            TEST_ASSERT_EQUAL_UINT8(ea->specific_class, eb->specific_class);
            TEST_ASSERT_EQUAL_UINT8(ea->cc_count, eb->cc_count);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(ea->ccs, eb->ccs, ea->cc_count);
        }
        TEST_ASSERT_EQUAL_UINT8(ea->notification_count, eb->notification_count);
        for (uint8_t n = 0; n < ea->notification_count; n++) {
            TEST_ASSERT_EQUAL_HEX8(ea->notifications[n].notification_type, eb->notifications[n].notification_type);
            TEST_ASSERT_EQUAL_UINT8(ea->notifications[n].event_mask_len, eb->notifications[n].event_mask_len);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(ea->notifications[n].event_mask, eb->notifications[n].event_mask,
                                          ea->notifications[n].event_mask_len);
        }
        TEST_ASSERT_EQUAL(ea->has_binary_sensor_mask, eb->has_binary_sensor_mask);
        TEST_ASSERT_EQUAL(ea->has_multilevel_mask, eb->has_multilevel_mask);
        TEST_ASSERT_EQUAL(ea->has_push_pull, eb->has_push_pull);
        if (ea->has_push_pull) {
            TEST_ASSERT_EQUAL_UINT8(ea->push_pull_mode, eb->push_pull_mode);
        }
        TEST_ASSERT_EQUAL(ea->has_alarm_sensor_mask, eb->has_alarm_sensor_mask);
        if (ea->has_alarm_sensor_mask) {
            TEST_ASSERT_EQUAL_UINT8(ea->alarm_sensor_mask_len, eb->alarm_sensor_mask_len);
            if (ea->alarm_sensor_mask_len > 0u) { /* empty: asked, none (BL-130) */
                TEST_ASSERT_EQUAL_UINT8_ARRAY(ea->alarm_sensor_mask, eb->alarm_sensor_mask,
                                              ea->alarm_sensor_mask_len);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT8(a->interview_step, b->interview_step);
    TEST_ASSERT_EQUAL(a->has_quirk, b->has_quirk);
    if (a->has_quirk) {
        TEST_ASSERT_EQUAL_UINT16(a->quirk_manufacturer_id, b->quirk_manufacturer_id);
        TEST_ASSERT_EQUAL_UINT16(a->quirk_product_type, b->quirk_product_type);
        TEST_ASSERT_EQUAL_UINT16(a->quirk_product_id, b->quirk_product_id);
    }
}

static void test_round_trip_full_cache(void)
{
    zw_node_cache_t cache;
    zw_node_cache_init(&cache);
    fill_full_record(zw_node_add(&cache, 5u), 5u);
    zw_node_record_t plain = {0};
    plain.node_id = 9u; /* a node with almost nothing known yet: every optional field absent */
    plain.security_classes = ZW_NODE_SECURITY_UNKNOWN;
    memcpy(zw_node_add(&cache, 9u), &plain, sizeof(plain));

    uint8_t buf[BUF_MAX];
    const size_t n = zw_node_cache_encode(&cache, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0u);

    zw_node_cache_t out;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&out, buf, n));
    TEST_ASSERT_EQUAL_UINT(2u, zw_node_count(&out));
    assert_full_record_equal(zw_node_find(&cache, 5u), zw_node_find(&out, 5u));
    assert_full_record_equal(zw_node_find(&cache, 9u), zw_node_find(&out, 9u));

    /* encoding again from the decoded cache reproduces the same bytes (a stable round trip) */
    uint8_t buf2[BUF_MAX];
    const size_t n2 = zw_node_cache_encode(&out, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_UINT(n, n2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buf, buf2, n);
}

/* ================================================================================================
 * Zero values are left out (except field 04's own FF-is-the-default convention).
 * ================================================================================================ */

static void test_zero_values_omitted(void)
{
    zw_node_cache_t cache;
    zw_node_cache_init(&cache);
    zw_node_record_t *r = zw_node_add(&cache, 5u); /* generation 0, no other field set */
    uint8_t buf[BUF_MAX];
    const size_t n = zw_node_cache_encode(&cache, buf, sizeof(buf));
    /* header (16) + one item: field 00, group 01, len 2, node ID - nothing else */
    TEST_ASSERT_EQUAL_UINT(16u + 5u, n);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[16]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[17]);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[18]);

    /* security_classes 00 ("confirmed no security") is NOT the default (FF is) and is written */
    r->security_classes = 0x00u;
    uint8_t buf2[BUF_MAX];
    const size_t n2 = zw_node_cache_encode(&cache, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_UINT(16u + 5u + 4u, n2); /* one more item: field 04, len 1 (3-byte header + 1) */
    TEST_ASSERT_EQUAL_HEX8(0x04u, buf2[21]);

    zw_node_cache_t out;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&out, buf, n)); /* the earlier, FF-default blob - still intact */
    TEST_ASSERT_EQUAL_HEX8(ZW_NODE_SECURITY_UNKNOWN, zw_node_find(&out, 5u)->security_classes);
    TEST_ASSERT_EQUAL_UINT16(0u, zw_node_find(&out, 5u)->generation); /* absent -> default 0 */
}

/* ================================================================================================
 * Unknown fields (per node) and an unknown group (cache-wide) survive decode then encode
 * byte for byte.
 * ================================================================================================ */

static size_t build_header(uint8_t *out, const uint8_t *payload, size_t payload_len)
{
    memcpy(out, "ZWND", 4u);
    out[4] = 1u;
    out[5] = 1u;
    out[6] = 16u;
    out[7] = 0u;
    out[8] = (uint8_t)payload_len;
    out[9] = (uint8_t)(payload_len >> 8);
    out[10] = 0u;
    out[11] = 0u;
    const uint32_t crc = zw_nodes_crc32(0u, payload, payload_len);
    out[12] = (uint8_t)crc;
    out[13] = (uint8_t)(crc >> 8);
    out[14] = (uint8_t)(crc >> 16);
    out[15] = (uint8_t)(crc >> 24);
    memcpy(out + 16, payload, payload_len);
    return 16u + payload_len;
}

static void test_unknown_fields_and_groups_round_trip(void)
{
    /* node 5: field 04 (known), then an unknown field 7F (group 01); node 9: field 0E (known);
     * finally, after both nodes, an item of an entirely unknown group 02 - already in the exact
     * order zw_node_cache_encode() itself would produce, so decode -> encode reproduces it
     * byte for byte (see the file's own header comment on canonical field order). */
    const uint8_t payload[] = {
        0x00, 0x01, 0x02, 0x05, 0x00,             /* node 5 */
        0x04, 0x01, 0x01, 0x00,                    /* security: 00, confirmed none */
        0x7F, 0x01, 0x02, 0xAB, 0xCD,               /* unknown field, group 01 */
        0x00, 0x01, 0x02, 0x09, 0x00,             /* node 9 */
        0x0E, 0x01, 0x01, 0x03,                    /* interview step 3 */
        0x00, 0x02, 0x03, 0x01, 0x02, 0x03,         /* unknown group 02 */
    };
    uint8_t blob[64];
    const size_t blob_len = build_header(blob, payload, sizeof(payload));

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    TEST_ASSERT_EQUAL_UINT(2u, zw_node_count(&cache));

    const zw_node_record_t *n5 = zw_node_find(&cache, 5u);
    TEST_ASSERT_NOT_NULL(n5);
    TEST_ASSERT_EQUAL_HEX8(0x00u, n5->security_classes);
    TEST_ASSERT_EQUAL_UINT16(5u, n5->unknown_len);
    const uint8_t expect_unknown[5] = {0x7F, 0x01, 0x02, 0xAB, 0xCD};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect_unknown, n5->unknown, 5u);

    const zw_node_record_t *n9 = zw_node_find(&cache, 9u);
    TEST_ASSERT_NOT_NULL(n9);
    TEST_ASSERT_EQUAL_UINT8(3u, n9->interview_step);
    TEST_ASSERT_EQUAL_UINT16(0u, n9->unknown_len);

    TEST_ASSERT_EQUAL_UINT(6u, cache.unknown_groups_len);
    const uint8_t expect_group[6] = {0x00, 0x02, 0x03, 0x01, 0x02, 0x03};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect_group, cache.unknown_groups, 6u);
    TEST_ASSERT_EQUAL_UINT(0u, cache.unknown_lost);

    uint8_t out[128];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);
}

static void test_unknown_area_overflow_is_lost_not_refused(void)
{
    /* one node, then far more unknown-group bytes than ZW_CACHE_UNKNOWN_GROUP_BYTES can hold:
     * decode still succeeds, the overflow is only counted. */
    uint8_t payload[6u + (size_t)(ZW_CACHE_UNKNOWN_GROUP_BYTES + 100u) / 61u * 61u];
    size_t n = 0u;
    payload[n++] = 0x00; payload[n++] = 0x01; payload[n++] = 0x02; payload[n++] = 0x01; payload[n++] = 0x00;
    while (n + 61u <= sizeof(payload)) {
        payload[n++] = 0x00; /* field */
        payload[n++] = 0x02; /* an unknown group */
        payload[n++] = 58u;  /* value length */
        memset(&payload[n], 0xEE, 58u);
        n += 58u;
    }
    uint8_t *blob = malloc(16u + n);
    TEST_ASSERT_NOT_NULL(blob);
    const size_t blob_len = build_header(blob, payload, n);

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(&cache));
    TEST_ASSERT_TRUE(cache.unknown_groups_len <= ZW_CACHE_UNKNOWN_GROUP_BYTES);
    TEST_ASSERT_TRUE(cache.unknown_lost > 0u);
    free(blob);
}

/* ================================================================================================
 * Damage is refused, and changes nothing in the cache passed to it.
 * ================================================================================================ */

static void seed_sentinel(zw_node_cache_t *cache)
{
    zw_node_cache_init(cache);
    zw_node_record_t *r = zw_node_add(cache, 42u);
    r->generation = 7u;
}

static void assert_is_sentinel(const zw_node_cache_t *cache)
{
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(cache));
    const zw_node_record_t *r = zw_node_find_const(cache, 42u);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT16(7u, r->generation);
}

static void expect_refused(const uint8_t *blob, size_t len)
{
    zw_node_cache_t cache;
    seed_sentinel(&cache);
    TEST_ASSERT_FALSE(zw_node_cache_decode(&cache, blob, len));
    assert_is_sentinel(&cache); /* unchanged */
}

static void test_damage_refused(void)
{
    zw_node_cache_t good_cache;
    zw_node_cache_init(&good_cache);
    fill_full_record(zw_node_add(&good_cache, 5u), 5u);
    uint8_t good[BUF_MAX];
    const size_t good_len = zw_node_cache_encode(&good_cache, good, sizeof(good));
    TEST_ASSERT_TRUE(good_len > 0u);

    uint8_t buf[BUF_MAX];

    /* bad magic */
    memcpy(buf, good, good_len);
    buf[0] ^= 0xFFu;
    expect_refused(buf, good_len);

    /* a compat this build does not understand */
    memcpy(buf, good, good_len);
    buf[5] = 2u;
    expect_refused(buf, good_len);

    /* a bad CRC */
    memcpy(buf, good, good_len);
    buf[good_len - 1u] ^= 0xFFu;
    expect_refused(buf, good_len);

    /* header length past the buffer */
    memcpy(buf, good, good_len);
    buf[6] = 0xFFu;
    buf[7] = 0xFFu;
    expect_refused(buf, good_len);

    /* payload length past the buffer */
    memcpy(buf, good, good_len);
    buf[8] = 0xFFu;
    buf[9] = 0xFFu;
    expect_refused(buf, good_len);

    /* an item's declared value length runs past the end (truncated buffer) */
    expect_refused(good, good_len - 1u);

    /* a field before any node start */
    {
        const uint8_t payload[] = {0x0Eu, 0x01u, 0x01u, 0x01u};
        uint8_t blob[32];
        const size_t n = build_header(blob, payload, sizeof(payload));
        expect_refused(blob, n);
    }

    /* a node ID twice */
    {
        const uint8_t payload[] = {
            0x00, 0x01, 0x02, 0x05, 0x00, 0x00, 0x01, 0x02, 0x05, 0x00,
        };
        uint8_t blob[32];
        const size_t n = build_header(blob, payload, sizeof(payload));
        expect_refused(blob, n);
    }

    /* node ID 0 */
    {
        const uint8_t payload[] = {0x00, 0x01, 0x02, 0x00, 0x00};
        uint8_t blob[32];
        const size_t n = build_header(blob, payload, sizeof(payload));
        expect_refused(blob, n);
    }

    /* a known field with the wrong length (protocol, field 02, needs exactly 4) */
    {
        const uint8_t payload[] = {0x00, 0x01, 0x02, 0x05, 0x00, 0x02, 0x01, 0x03, 0x01, 0x02, 0x03};
        uint8_t blob[32];
        const size_t n = build_header(blob, payload, sizeof(payload));
        expect_refused(blob, n);
    }
}

/* ================================================================================================
 * The table's limits: right at the ceiling is accepted normally.
 * ================================================================================================ */

static void test_limits(void)
{
    /* exactly ZW_NODE_CC_MAX CCs: accepted */
    {
        zw_node_cache_t cache;
        zw_node_cache_init(&cache);
        zw_node_record_t *r = zw_node_add(&cache, 5u);
        r->cc_count = ZW_NODE_CC_MAX;
        for (uint8_t i = 0; i < ZW_NODE_CC_MAX; i++) {
            r->ccs[i].cc = i;
            r->ccs[i].supported = true;
        }
        uint8_t buf[BUF_MAX];
        const size_t n = zw_node_cache_encode(&cache, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0u);
        zw_node_cache_t out;
        TEST_ASSERT_TRUE(zw_node_cache_decode(&out, buf, n));
        TEST_ASSERT_EQUAL_UINT8(ZW_NODE_CC_MAX, zw_node_find(&out, 5u)->cc_count);
        TEST_ASSERT_EQUAL_UINT(0u, out.over_limit);
    }
    /* exactly ZW_NODE_NOTIFICATION_MAX entries on one endpoint: accepted */
    {
        zw_node_cache_t cache;
        zw_node_cache_init(&cache);
        zw_node_record_t *r = zw_node_add(&cache, 5u);
        r->endpoints[0].used = true;
        r->endpoints[0].notification_count = ZW_NODE_NOTIFICATION_MAX;
        for (uint8_t i = 0; i < ZW_NODE_NOTIFICATION_MAX; i++) {
            r->endpoints[0].notifications[i].notification_type = (uint8_t)(i + 1u);
        }
        uint8_t buf[BUF_MAX];
        const size_t n = zw_node_cache_encode(&cache, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0u);
        zw_node_cache_t out;
        TEST_ASSERT_TRUE(zw_node_cache_decode(&out, buf, n));
        TEST_ASSERT_EQUAL_UINT8(ZW_NODE_NOTIFICATION_MAX, zw_node_find(&out, 5u)->endpoints[0].notification_count);
        TEST_ASSERT_EQUAL_UINT(0u, out.over_limit);
    }
}

/* ================================================================================================
 * An item beyond this firmware's own limits (but otherwise well formed) is kept like an unknown
 * field, not refused: it goes raw into the node's unknown area, round-trips, and is superseded
 * (never duplicated) once an in-limits value for the same field arrives - network.md §2's
 * "over-limit" rule, added after review so a firmware rollback cannot lose a whole node (and with
 * it the node's generation, part of the zone binding address, zones.md "Addresses") just because
 * a newer firmware once wrote something bigger than this one holds.
 * ================================================================================================ */

/* node 5, listening, basic/generic/specific 04/20/01, interview step 7 - every case below appends
 * its own over-limit item after this, so a decode proves "the node decodes with everything else". */
static size_t build_node5_prefix(uint8_t *payload)
{
    static const uint8_t prefix[16] = {
        0x00, 0x01, 0x02, 0x05, 0x00, 0x02, 0x01, 0x04, 0x01, 0x04, 0x20, 0x01, 0x0E, 0x01, 0x01, 0x07,
    };
    memcpy(payload, prefix, sizeof(prefix));
    return sizeof(prefix);
}

static void assert_node5_baseline(const zw_node_record_t *r)
{
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(r->has_protocol);
    TEST_ASSERT_TRUE(r->is_listening);
    TEST_ASSERT_EQUAL_HEX8(0x04u, r->basic_class);
    TEST_ASSERT_EQUAL_HEX8(0x20u, r->generic_class);
    TEST_ASSERT_EQUAL_HEX8(0x01u, r->specific_class);
    TEST_ASSERT_EQUAL_UINT8(7u, r->interview_step);
}

/* Two encoded caches produce identical bytes: proof that a superseded raw item leaves nothing
 * behind (no duplicate, no leftover) - 'b' is a hand-built reference with no raw baggage at all. */
static void assert_same_encoding(const zw_node_cache_t *a, const zw_node_cache_t *b)
{
    uint8_t ba[BUF_MAX];
    uint8_t bb[BUF_MAX];
    const size_t na = zw_node_cache_encode(a, ba, sizeof(ba));
    const size_t nb = zw_node_cache_encode(b, bb, sizeof(bb));
    TEST_ASSERT_TRUE(na > 0u);
    TEST_ASSERT_EQUAL_UINT(na, nb);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ba, bb, na);
}

/* ZW-033: field 11, a device's own versions - its bytes frozen here, read back, more targets than
 * kept stay raw and round-trip, a malformed one is damage. */
static void test_versions_field(void)
{
    uint8_t payload[256];
    size_t n = build_node5_prefix(payload);
    static const uint8_t ITEM[] = {0x11u, 0x01u, 8u, 1u, 26u, 3u, 7u, 18u, 1u, 2u, 5u};
    memcpy(&payload[n], ITEM, sizeof ITEM); /* fw 1.26, hw 3, protocol 7.18, target 1: 2.5 */
    n += sizeof ITEM;
    uint8_t blob[BUF_MAX];
    size_t blob_len = build_header(blob, payload, n);
    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    const zw_node_record_t *r = zw_node_find(&cache, 5u);
    TEST_ASSERT_TRUE(r->has_versions);
    TEST_ASSERT_EQUAL_UINT8(1u, r->fw_major);
    TEST_ASSERT_EQUAL_UINT8(26u, r->fw_minor);
    TEST_ASSERT_EQUAL_UINT8(3u, r->hw_version);
    TEST_ASSERT_EQUAL_UINT8(7u, r->protocol_major);
    TEST_ASSERT_EQUAL_UINT8(18u, r->protocol_minor);
    TEST_ASSERT_EQUAL_UINT8(1u, r->fw_target_count);
    TEST_ASSERT_EQUAL_UINT8(2u, r->fw_targets[0][0]);
    TEST_ASSERT_EQUAL_UINT8(5u, r->fw_targets[0][1]);
    TEST_ASSERT_EQUAL_UINT16(0u, r->unknown_len);
    uint8_t out[BUF_MAX];
    TEST_ASSERT_EQUAL_UINT(blob_len, zw_node_cache_encode(&cache, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len); /* the encoder writes the same bytes */

    /* more targets than kept: raw, round-trips */
    n = build_node5_prefix(payload);
    const uint8_t targets = ZW_NODE_FW_TARGETS_MAX + 1u;
    payload[n++] = 0x11u;
    payload[n++] = 0x01u;
    payload[n++] = (uint8_t)(6u + 2u * targets);
    const uint8_t head[6] = {1u, 2u, 0xFFu, 7u, 22u, targets};
    memcpy(&payload[n], head, 6u);
    n += 6u;
    for (uint8_t i = 0; i < targets; i++) {
        payload[n++] = i;
        payload[n++] = 0u;
    }
    blob_len = build_header(blob, payload, n);
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    TEST_ASSERT_FALSE(zw_node_find(&cache, 5u)->has_versions);
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);
    TEST_ASSERT_EQUAL_UINT(blob_len, zw_node_cache_encode(&cache, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);

    /* its length not its own shape: damage */
    n = build_node5_prefix(payload);
    static const uint8_t BAD[] = {0x11u, 0x01u, 7u, 1u, 26u, 3u, 7u, 18u, 1u, 2u};
    memcpy(&payload[n], BAD, sizeof BAD);
    n += sizeof BAD;
    blob_len = build_header(blob, payload, n);
    TEST_ASSERT_FALSE(zw_node_cache_decode(&cache, blob, blob_len));
}

/* ZW-017, ZW-024 (BL-085): field 12, an endpoint's Alarm Sensor types - read back, written again
 * the same, an endpoint or a mask beyond this firmware kept raw (and superseded once in limits),
 * a malformed one is damage. */
static void test_alarm_sensor_field(void)
{
    uint8_t payload[64];
    size_t n = build_node5_prefix(payload);
    static const uint8_t ITEM[] = {0x12u, 0x01u, 3u, 0u, 1u, 0x16u}; /* ep 0: smoke, CO, heat */
    memcpy(&payload[n], ITEM, sizeof ITEM);
    n += sizeof ITEM;
    uint8_t blob[BUF_MAX];
    size_t blob_len = build_header(blob, payload, n);
    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_TRUE(r->endpoints[0].used);
    TEST_ASSERT_TRUE(r->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(1u, r->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_HEX8(0x16u, r->endpoints[0].alarm_sensor_mask[0]);
    TEST_ASSERT_FALSE(r->endpoints[0].has_binary_sensor_mask);
    TEST_ASSERT_EQUAL_UINT16(0u, r->unknown_len);
    TEST_ASSERT_EQUAL_UINT(0u, cache.over_limit);
    /* written again (an endpoint field goes before field 0E), it reads back the same, stably */
    uint8_t out[BUF_MAX];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    zw_node_cache_t again;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&again, out, out_len));
    assert_full_record_equal(r, zw_node_find(&again, 5u));
    uint8_t out2[BUF_MAX];
    TEST_ASSERT_EQUAL_UINT(out_len, zw_node_cache_encode(&again, out2, sizeof(out2)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(out, out2, out_len);

    /* an endpoint this firmware has no room for: raw, round-trips byte for byte */
    n = build_node5_prefix(payload);
    static const uint8_t FAR_EP[] = {0x12u, 0x01u, 3u, ZW_NODE_ENDPOINTS_MAX, 1u, 0x02u};
    memcpy(&payload[n], FAR_EP, sizeof FAR_EP);
    n += sizeof FAR_EP;
    blob_len = build_header(blob, payload, n);
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_EQUAL_UINT16(sizeof FAR_EP, r->unknown_len);
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);
    TEST_ASSERT_EQUAL_UINT(blob_len, zw_node_cache_encode(&cache, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);

    /* a mask longer than this firmware holds: raw, round-trips; a later in-limits value on the same
     * endpoint supersedes it - no duplicate */
    n = build_node5_prefix(payload);
    const uint8_t mlen = ZW_NODE_MASK_BYTES + 1u;
    payload[n++] = 0x12u;
    payload[n++] = 0x01u;
    payload[n++] = (uint8_t)(2u + mlen);
    payload[n++] = 0u;
    payload[n++] = mlen;
    for (uint8_t i = 0; i < mlen; i++) {
        payload[n++] = (uint8_t)(0x10u + i);
    }
    blob_len = build_header(blob, payload, n);
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    r = zw_node_find(&cache, 5u);
    TEST_ASSERT_FALSE(r->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT16(3u + 2u + mlen, r->unknown_len);
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);
    TEST_ASSERT_EQUAL_UINT(blob_len, zw_node_cache_encode(&cache, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);
    r->endpoints[0].used = true;
    r->endpoints[0].has_alarm_sensor_mask = true;
    r->endpoints[0].alarm_sensor_mask_len = 1u;
    r->endpoints[0].alarm_sensor_mask[0] = 0x02u;
    zw_node_cache_t clean;
    zw_node_cache_init(&clean);
    zw_node_record_t *cr = zw_node_add(&clean, 5u);
    *cr = *r;
    cr->unknown_len = 0u;
    assert_same_encoding(&cache, &clean);

    /* damage: too short to hold its endpoint and length, or a length its value contradicts */
    n = build_node5_prefix(payload);
    static const uint8_t TOO_SHORT[] = {0x12u, 0x01u, 1u, 0u};
    memcpy(&payload[n], TOO_SHORT, sizeof TOO_SHORT);
    blob_len = build_header(blob, payload, n + sizeof TOO_SHORT);
    expect_refused(blob, blob_len);
    static const uint8_t CONTRADICTS[] = {0x12u, 0x01u, 3u, 0u, 2u, 0x16u};
    memcpy(&payload[n], CONTRADICTS, sizeof CONTRADICTS);
    blob_len = build_header(blob, payload, n + sizeof CONTRADICTS);
    expect_refused(blob, blob_len);
}

/* ZW-017, BL-130: field 12 with no types (mask length 0) is "asked, none" - the interview's answer
 * for a device that never answered Alarm Sensor's Supported Get. It reads back as known-empty and
 * is written again the same (no new field: a reader that knows field 12 reads an empty mask; one
 * from before field 12 keeps it raw). */
static void test_alarm_sensor_field_empty(void)
{
    uint8_t payload[64];
    size_t n = build_node5_prefix(payload);
    static const uint8_t ITEM[] = {0x12u, 0x01u, 2u, 0u, 0u}; /* ep 0: asked, none */
    memcpy(&payload[n], ITEM, sizeof ITEM);
    n += sizeof ITEM;
    uint8_t blob[BUF_MAX];
    const size_t blob_len = build_header(blob, payload, n);
    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    const zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_TRUE(r->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, r->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_UINT16(0u, r->unknown_len);
    uint8_t out[BUF_MAX];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    zw_node_cache_t again;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&again, out, out_len));
    assert_full_record_equal(r, zw_node_find(&again, 5u));
    TEST_ASSERT_TRUE(zw_node_find(&again, 5u)->endpoints[0].has_alarm_sensor_mask);
}

static void test_over_limit_cc_list(void)
{
    uint8_t payload[256];
    size_t n = build_node5_prefix(payload);
    const uint8_t count = ZW_NODE_CC_MAX + 1u; /* 33: one more than this firmware holds */
    payload[n++] = 0x03u;
    payload[n++] = 0x01u;
    payload[n++] = (uint8_t)((size_t)count * 3u);
    for (uint8_t i = 0; i < count; i++) {
        payload[n++] = i;
        payload[n++] = 0u;
        payload[n++] = 0x01u;
    }
    uint8_t blob[BUF_MAX];
    const size_t blob_len = build_header(blob, payload, n);

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_EQUAL_UINT8(0u, r->cc_count); /* not applied: over this firmware's limit */
    TEST_ASSERT_EQUAL_UINT16(3u + (size_t)count * 3u, r->unknown_len);
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);

    /* round-trips byte for byte */
    uint8_t out[BUF_MAX];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);

    /* a later, in-limits value supersedes it - no duplicate */
    r->cc_count = 2u;
    r->ccs[0] = (zw_node_cc_t){.cc = 0x20u, .supported = true};
    r->ccs[1] = (zw_node_cc_t){.cc = 0x71u, .supported = true, .secure = true};
    zw_node_cache_t clean;
    zw_node_cache_init(&clean);
    zw_node_record_t *cr = zw_node_add(&clean, 5u);
    *cr = *r;
    cr->unknown_len = 0u; /* the same live fields, but never carried the raw leftover at all */
    assert_same_encoding(&cache, &clean);
}

static void test_over_limit_endpoint_cc_list(void)
{
    uint8_t payload[64];
    size_t n = build_node5_prefix(payload);
    const uint8_t count = ZW_NODE_ENDPOINT_CC_MAX + 1u; /* 13: endpoint 1's own list, one over */
    payload[n++] = 0x0Au;
    payload[n++] = 0x01u;
    payload[n++] = (uint8_t)(4u + count);
    payload[n++] = 1u; /* endpoint 1: a valid index */
    payload[n++] = 0x20u;
    payload[n++] = 0x01u;
    payload[n++] = count;
    for (uint8_t i = 0; i < count; i++) {
        payload[n++] = (uint8_t)(0x30u + i);
    }
    uint8_t blob[BUF_MAX];
    const size_t blob_len = build_header(blob, payload, n);

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_FALSE(r->endpoints[1].has_class); /* not applied */
    TEST_ASSERT_EQUAL_UINT16(3u + 4u + count, r->unknown_len);
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);

    uint8_t out[BUF_MAX];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);

    r->endpoints[1].used = true;
    r->endpoints[1].has_class = true;
    r->endpoints[1].generic_class = 0x20u;
    r->endpoints[1].specific_class = 0x01u;
    r->endpoints[1].cc_count = 2u;
    r->endpoints[1].ccs[0] = 0x30u;
    r->endpoints[1].ccs[1] = 0x31u;
    zw_node_cache_t clean;
    zw_node_cache_init(&clean);
    zw_node_record_t *cr = zw_node_add(&clean, 5u);
    *cr = *r;
    cr->unknown_len = 0u;
    assert_same_encoding(&cache, &clean);
}

static void test_over_limit_endpoint_index(void)
{
    uint8_t payload[64];
    size_t n = build_node5_prefix(payload);
    payload[n++] = 0x0Au;
    payload[n++] = 0x01u;
    payload[n++] = 0x04u;
    payload[n++] = ZW_NODE_ENDPOINTS_MAX; /* one past the last valid index: never representable live */
    payload[n++] = 0x00u;
    payload[n++] = 0x00u;
    payload[n++] = 0x00u;
    uint8_t blob[BUF_MAX];
    const size_t blob_len = build_header(blob, payload, n);

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_EQUAL_UINT16(7u, r->unknown_len); /* 3-byte header + 4-byte value */
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);

    /* round-trips byte for byte, and stays stable across a second bounce: an endpoint index this
     * build cannot represent at all can never be superseded, only carried forward unchanged. */
    uint8_t out[BUF_MAX];
    size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);
    zw_node_cache_t again;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&again, out, out_len));
    out_len = zw_node_cache_encode(&again, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);
}

static void test_over_limit_mask(void)
{
    uint8_t payload[64];
    size_t n = build_node5_prefix(payload);
    const uint8_t mlen = ZW_NODE_MASK_BYTES + 1u; /* 5: one more than a mask this firmware holds */
    payload[n++] = 0x0Cu;
    payload[n++] = 0x01u;
    payload[n++] = (uint8_t)(2u + mlen);
    payload[n++] = 0u; /* endpoint 0 */
    payload[n++] = mlen;
    for (uint8_t i = 0; i < mlen; i++) {
        payload[n++] = (uint8_t)(0x10u + i);
    }
    uint8_t blob[BUF_MAX];
    const size_t blob_len = build_header(blob, payload, n);

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_FALSE(r->endpoints[0].has_binary_sensor_mask);
    TEST_ASSERT_EQUAL_UINT16(3u + 2u + mlen, r->unknown_len);
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);

    uint8_t out[BUF_MAX];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);

    r->endpoints[0].used = true;
    r->endpoints[0].has_binary_sensor_mask = true;
    r->endpoints[0].binary_sensor_mask_len = 2u;
    r->endpoints[0].binary_sensor_mask[0] = 0x01u;
    r->endpoints[0].binary_sensor_mask[1] = 0x02u;
    zw_node_cache_t clean;
    zw_node_cache_init(&clean);
    zw_node_record_t *cr = zw_node_add(&clean, 5u);
    *cr = *r;
    cr->unknown_len = 0u;
    assert_same_encoding(&cache, &clean);
}

/* Field 0B is matched on field + endpoint + notification type (per a code review): a raw
 * leftover for one type is superseded only by a decoded entry of that same type, not by any other
 * type on the same endpoint - so a 9th notification type (one more than ZW_NODE_NOTIFICATION_MAX)
 * behaves exactly like the other over-limit cases above, round-tripping first and only later
 * giving way once its own type is in limits.
 *
 * Unlike the other over-limit tests, this one cannot use build_node5_prefix(): put_node() writes
 * endpoint fields (0A/0B/0C/0D/10) *before* field 0E (interview step), and this is the one
 * over-limit case where the endpoint ends up with live data (the other MAX entries), so the
 * in-limits notifications must precede interview step in the input and the raw 9th one must
 * follow it, to already be in the order a canonical encode would produce. */
static void test_over_limit_notification(void)
{
    uint8_t payload[128];
    size_t n = 0u;
    static const uint8_t head[12] = {
        0x00, 0x01, 0x02, 0x05, 0x00, 0x02, 0x01, 0x04, 0x01, 0x04, 0x20, 0x01, /* node 5, protocol */
    };
    memcpy(&payload[n], head, sizeof(head));
    n += sizeof(head);
    for (uint8_t i = 1; i <= ZW_NODE_NOTIFICATION_MAX; i++) { /* the MAX in-limits entries */
        payload[n++] = 0x0Bu;
        payload[n++] = 0x01u;
        payload[n++] = 0x03u;
        payload[n++] = 0u; /* endpoint 0 */
        payload[n++] = i;  /* notification type */
        payload[n++] = 0u; /* no event mask */
    }
    payload[n++] = 0x0Eu; /* interview step, after the endpoint's in-limits data */
    payload[n++] = 0x01u;
    payload[n++] = 0x01u;
    payload[n++] = 0x07u;
    payload[n++] = 0x0Bu; /* the over-limit (MAX+1)'th type, after interview step */
    payload[n++] = 0x01u;
    payload[n++] = 0x03u;
    payload[n++] = 0u;
    payload[n++] = (uint8_t)(ZW_NODE_NOTIFICATION_MAX + 1u);
    payload[n++] = 0u;
    uint8_t blob[BUF_MAX];
    const size_t blob_len = build_header(blob, payload, n);

    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, blob, blob_len));
    zw_node_record_t *r = zw_node_find(&cache, 5u);
    assert_node5_baseline(r);
    TEST_ASSERT_EQUAL_UINT8(ZW_NODE_NOTIFICATION_MAX, r->endpoints[0].notification_count);
    TEST_ASSERT_EQUAL_UINT16(6u, r->unknown_len); /* only the 9th (one item) is over limit */
    TEST_ASSERT_EQUAL_UINT(1u, cache.over_limit);

    /* round-trips byte for byte: types 1..8 are already live, but type 9's raw item is keyed by
     * its own type, so none of its siblings supersede it */
    uint8_t out[BUF_MAX];
    const size_t out_len = zw_node_cache_encode(&cache, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(blob_len, out_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, blob_len);

    /* a later, in-limits entry of that same type (9) supersedes it - no duplicate */
    r->endpoints[0].notifications[ZW_NODE_NOTIFICATION_MAX - 1u].notification_type =
        (uint8_t)(ZW_NODE_NOTIFICATION_MAX + 1u);
    zw_node_cache_t clean;
    zw_node_cache_init(&clean);
    zw_node_record_t *cr = zw_node_add(&clean, 5u);
    *cr = *r;
    cr->unknown_len = 0u;
    assert_same_encoding(&cache, &clean);
}

/* ================================================================================================
 * The frozen fixture (network.md §2): a realistic cache with an unknown field and an unknown
 * group, produced once and never edited (see this file's own header comment to regenerate it).
 * ================================================================================================ */

static size_t build_fixture(uint8_t *out, size_t cap)
{
    zw_node_cache_t cache;
    zw_node_cache_init(&cache);

    /* node 3: Zooz ZSE41 open/close XS, S2 Authenticated, the lifeline set, one endpoint's worth
     * of Home Security notifications - and one unknown field this build does not recognise. */
    zw_node_record_t *door = zw_node_add(&cache, 3u);
    door->generation = 1u;
    door->has_protocol = true;
    door->is_listening = false;
    door->basic_class = 0x04u;
    door->generic_class = 0x20u;
    door->specific_class = 0x01u;
    door->cc_count = 4u;
    door->ccs[0] = (zw_node_cc_t){.cc = 0x5Eu, .version = 2u, .supported = true};
    door->ccs[1] = (zw_node_cc_t){.cc = 0x9Fu, .version = 1u, .supported = true};
    door->ccs[2] = (zw_node_cc_t){.cc = 0x71u, .version = 8u, .supported = true, .secure = true};
    door->ccs[3] = (zw_node_cc_t){.cc = 0x80u, .version = 1u, .supported = true, .secure = true};
    door->security_classes = 0x04u; /* Authenticated - security.md §1 has the real bit numbering */
    door->has_identity = true;
    door->manufacturer_id = 0x027Au;
    door->product_type = 0x7000u;
    door->product_id = 0xE001u;
    door->has_zwave_plus = true;
    door->zwave_plus_role_type = 0x06u;
    door->installer_icon = 0x0C0Du;
    door->user_icon = 0x0C0Du;
    door->has_wake_up = true;
    door->wake_up_interval_seconds = 3600u;
    door->wake_up_min_seconds = 30u;
    door->wake_up_max_seconds = 604800u;
    door->wake_up_step_seconds = 30u;
    door->wake_up_notify_node_id = 1u;
    door->has_lifeline = true;
    door->lifeline_group = 1u;
    door->lifeline_set = true;
    door->endpoints[0].used = true;
    door->endpoints[0].notification_count = 1u;
    door->endpoints[0].notifications[0].notification_type = 0x06u; /* Access Control */
    door->endpoints[0].notifications[0].event_mask_len = 3u;
    door->endpoints[0].notifications[0].event_mask[0] = 0x00u;
    door->endpoints[0].notifications[0].event_mask[1] = 0x03u;
    door->endpoints[0].notifications[0].event_mask[2] = 0x00u;
    door->interview_step = 16u;
    door->has_quirk = true;
    door->quirk_manufacturer_id = 0x027Au;
    door->quirk_product_type = 0x7000u;
    door->quirk_product_id = 0xE001u;
    static const uint8_t unknown_item[5] = {0x7Fu, 0x01u, 0x02u, 0xABu, 0xCDu};
    memcpy(door->unknown, unknown_item, sizeof(unknown_item));
    door->unknown_len = (uint16_t)sizeof(unknown_item);

    /* node 7: a listening motion sensor without security */
    zw_node_record_t *motion = zw_node_add(&cache, 7u);
    motion->generation = 2u;
    motion->has_protocol = true;
    motion->is_listening = true;
    motion->basic_class = 0x04u;
    motion->generic_class = 0x20u;
    motion->specific_class = 0x01u;
    motion->cc_count = 2u;
    motion->ccs[0] = (zw_node_cc_t){.cc = 0x71u, .version = 5u, .supported = true};
    motion->ccs[1] = (zw_node_cc_t){.cc = 0x85u, .version = 2u, .supported = true};
    motion->security_classes = 0x00u;
    motion->has_identity = true;
    motion->manufacturer_id = 0x027Au;
    motion->product_type = 0x0301u;
    motion->product_id = 0x0012u;
    motion->has_lifeline = true;
    motion->lifeline_group = 1u;
    motion->lifeline_set = true;
    motion->endpoints[0].used = true;
    motion->endpoints[0].notification_count = 1u;
    motion->endpoints[0].notifications[0].notification_type = 0x07u; /* Home Security */
    motion->endpoints[0].notifications[0].event_mask_len = 1u;
    motion->endpoints[0].notifications[0].event_mask[0] = 0xC0u;
    motion->interview_step = 16u;

    /* node 12: a device with two endpoints (a contact with an external terminal) */
    zw_node_record_t *two_ep = zw_node_add(&cache, 12u);
    two_ep->generation = 3u;
    two_ep->has_protocol = true;
    two_ep->basic_class = 0x04u;
    two_ep->generic_class = 0x20u;
    two_ep->specific_class = 0x01u;
    two_ep->security_classes = 0x00u;
    two_ep->endpoints[1].used = true;
    two_ep->endpoints[1].has_class = true;
    two_ep->endpoints[1].generic_class = 0x20u;
    two_ep->endpoints[1].specific_class = 0x01u;
    two_ep->endpoints[1].cc_count = 1u;
    two_ep->endpoints[1].ccs[0] = 0x30u;
    two_ep->endpoints[2].used = true;
    two_ep->endpoints[2].has_class = true;
    two_ep->endpoints[2].generic_class = 0x20u;
    two_ep->endpoints[2].specific_class = 0x01u;
    two_ep->endpoints[2].cc_count = 1u;
    two_ep->endpoints[2].ccs[0] = 0x30u;
    two_ep->interview_step = 16u;

    /* an unknown group (a future top-level record kind this build does not know) */
    static const uint8_t unknown_group[6] = {0x00u, 0x02u, 0x03u, 0x01u, 0x02u, 0x03u};
    memcpy(cache.unknown_groups, unknown_group, sizeof(unknown_group));
    cache.unknown_groups_len = sizeof(unknown_group);

    return zw_node_cache_encode(&cache, out, cap);
}

#include "zw_nodes_fixture_v1.h"

static void test_fixture_decodes_to_expected(void)
{
    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, ZW_NODES_FIXTURE_V1, sizeof(ZW_NODES_FIXTURE_V1)));
    TEST_ASSERT_EQUAL_UINT(3u, zw_node_count(&cache));

    const zw_node_record_t *door = zw_node_find(&cache, 3u);
    TEST_ASSERT_NOT_NULL(door);
    TEST_ASSERT_EQUAL_UINT16(1u, door->generation);
    TEST_ASSERT_EQUAL_HEX8(0x04u, door->security_classes);
    TEST_ASSERT_EQUAL_UINT16(0x027Au, door->manufacturer_id);
    TEST_ASSERT_EQUAL_UINT16(0xE001u, door->product_id);
    TEST_ASSERT_EQUAL_UINT8(4u, door->cc_count);
    TEST_ASSERT_EQUAL_UINT16(5u, door->unknown_len);

    const zw_node_record_t *motion = zw_node_find(&cache, 7u);
    TEST_ASSERT_NOT_NULL(motion);
    TEST_ASSERT_TRUE(motion->is_listening);
    TEST_ASSERT_EQUAL_HEX8(0x00u, motion->security_classes);

    const zw_node_record_t *two_ep = zw_node_find(&cache, 12u);
    TEST_ASSERT_NOT_NULL(two_ep);
    TEST_ASSERT_TRUE(two_ep->endpoints[1].has_class);
    TEST_ASSERT_TRUE(two_ep->endpoints[2].has_class);

    TEST_ASSERT_EQUAL_UINT(6u, cache.unknown_groups_len);

    /* written before field 12 (BL-085): no endpoint has Alarm Sensor types */
    for (size_t i = 0; i < zw_node_count(&cache); i++) {
        for (unsigned e = 0; e < ZW_NODE_ENDPOINTS_MAX; e++) {
            TEST_ASSERT_FALSE(zw_node_at(&cache, i)->endpoints[e].has_alarm_sensor_mask);
        }
    }

    /* re-encoding the decoded fixture reproduces it exactly */
    uint8_t buf[BUF_MAX];
    const size_t n = zw_node_cache_encode(&cache, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(sizeof(ZW_NODES_FIXTURE_V1), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ZW_NODES_FIXTURE_V1, buf, n);
}

#include "zw_nodes_fixture_v2.h"

/* ZW-017 (BL-085): the frozen blob with field 12 decodes to a device whose only inputs are its
 * Alarm Sensor types, and writes back byte for byte; the format-1 fixture above, written before
 * field 12, reads with no Alarm Sensor types anywhere (test_fixture_decodes_to_expected). */
static void test_fixture_v2_alarm_sensor(void)
{
    zw_node_cache_t cache;
    TEST_ASSERT_TRUE(zw_node_cache_decode(&cache, ZW_NODES_FIXTURE_V2, sizeof(ZW_NODES_FIXTURE_V2)));
    TEST_ASSERT_EQUAL_UINT(1u, zw_node_count(&cache));
    const zw_node_record_t *smoke = zw_node_find(&cache, 5u);
    TEST_ASSERT_NOT_NULL(smoke);
    TEST_ASSERT_EQUAL_UINT16(1u, smoke->generation);
    TEST_ASSERT_EQUAL_UINT8(16u, smoke->interview_step);
    TEST_ASSERT_TRUE(smoke->endpoints[0].has_alarm_sensor_mask);
    TEST_ASSERT_EQUAL_UINT8(1u, smoke->endpoints[0].alarm_sensor_mask_len);
    TEST_ASSERT_EQUAL_HEX8(0x16u, smoke->endpoints[0].alarm_sensor_mask[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, smoke->endpoints[0].notification_count);
    TEST_ASSERT_FALSE(smoke->endpoints[0].has_binary_sensor_mask);
    TEST_ASSERT_EQUAL_UINT16(0u, smoke->unknown_len);
    uint8_t buf[BUF_MAX];
    const size_t n = zw_node_cache_encode(&cache, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(sizeof(ZW_NODES_FIXTURE_V2), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ZW_NODES_FIXTURE_V2, buf, n);
}

/* ================================================================================================
 * Reconciliation: every row of network.md §2's "At every start" table.
 * ================================================================================================ */

static void test_reconcile_every_row(void)
{
    zw_node_cache_t cache;
    zw_node_cache_init(&cache);
    zw_node_add(&cache, 3u);
    zw_node_add(&cache, 7u);

    /* home ID = stored, keys for it: normal - reconcile both ways */
    {
        const uint16_t module_nodes[] = {3u, 9u}; /* 7 gone, 9 new */
        zw_reconcile_result_t out;
        zw_node_reconcile(0xC0FFEE01u, module_nodes, 2u, true, 0xC0FFEE01u, &cache, &out);
        TEST_ASSERT_EQUAL(ZW_RECONCILE_NORMAL, out.reconcile_case);
        TEST_ASSERT_EQUAL_UINT(1u, out.to_interview_count);
        TEST_ASSERT_EQUAL_UINT16(9u, out.to_interview[0]);
        TEST_ASSERT_EQUAL_UINT(1u, out.to_forget_count);
        TEST_ASSERT_EQUAL_UINT16(7u, out.to_forget[0]);
    }
    /* home ID = stored, nothing changed: reconcile with empty lists */
    {
        const uint16_t module_nodes[] = {3u, 7u};
        zw_reconcile_result_t out;
        zw_node_reconcile(0xC0FFEE01u, module_nodes, 2u, true, 0xC0FFEE01u, &cache, &out);
        TEST_ASSERT_EQUAL(ZW_RECONCILE_NORMAL, out.reconcile_case);
        TEST_ASSERT_EQUAL_UINT(0u, out.to_interview_count);
        TEST_ASSERT_EQUAL_UINT(0u, out.to_forget_count);
    }
    /* home ID differs, module empty, keys and a cache for the old home ID: replaced or reset */
    {
        zw_reconcile_result_t out;
        zw_node_reconcile(0xDEAD0002u, NULL, 0u, true, 0xC0FFEE01u, &cache, &out);
        TEST_ASSERT_EQUAL(ZW_RECONCILE_REPLACED_OR_RESET, out.reconcile_case);
        TEST_ASSERT_EQUAL_UINT(0u, out.to_interview_count);
        TEST_ASSERT_EQUAL_UINT(0u, out.to_forget_count);
    }
    /* no keys at all, module empty: first start (or keys lost) */
    {
        zw_reconcile_result_t out;
        zw_node_reconcile(0xC0FFEE01u, NULL, 0u, false, 0u, &cache, &out);
        TEST_ASSERT_EQUAL(ZW_RECONCILE_FIRST_START, out.reconcile_case);
    }
    /* no keys at all, module has nodes: foreign - use none of them */
    {
        const uint16_t module_nodes[] = {1u};
        zw_reconcile_result_t out;
        zw_node_reconcile(0xC0FFEE01u, module_nodes, 1u, false, 0u, &cache, &out);
        TEST_ASSERT_EQUAL(ZW_RECONCILE_FOREIGN, out.reconcile_case);
        TEST_ASSERT_EQUAL_UINT(0u, out.to_interview_count);
        TEST_ASSERT_EQUAL_UINT(0u, out.to_forget_count);
    }
    /* home ID differs, module has nodes, keys for another home ID: foreign */
    {
        const uint16_t module_nodes[] = {1u};
        zw_reconcile_result_t out;
        zw_node_reconcile(0xDEAD0002u, module_nodes, 1u, true, 0xC0FFEE01u, &cache, &out);
        TEST_ASSERT_EQUAL(ZW_RECONCILE_FOREIGN, out.reconcile_case);
    }
}

/* ================================================================================================
 * Fuzz (ZW-023): random bytes and mutated valid blobs, run under ASan/UBSan in CI - the decoder
 * must never read or write outside its own buffers, and a refused blob must never leave the
 * cache half filled.
 * ================================================================================================ */

/* splitmix64, fixed seed - the same PRNG and the same convention as zwave_proto/test/test_zw_fuzz.c
 * (a fixed seed per file, not shared, so either file's failures replay on their own). */
static uint64_t g_seed = 0x2545F4914F6CDD1DULL;

static uint64_t next_u64(void)
{
    uint64_t z = (g_seed += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint8_t rnd_byte(void)
{
    return (uint8_t)(next_u64() & 0xFFu);
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(next_u64() % (uint64_t)(hi - lo + 1u));
}

static void test_fuzz_random_bytes_never_crash(void)
{
    uint8_t payload[300];
    for (uint32_t iter = 0; iter < 20000u; iter++) {
        const size_t len = (size_t)rnd_range(0u, sizeof(payload));
        for (size_t i = 0; i < len; i++) {
            payload[i] = rnd_byte();
        }
        zw_node_cache_t cache;
        seed_sentinel(&cache);
        if (!zw_node_cache_decode(&cache, payload, len)) {
            assert_is_sentinel(&cache); /* refused: never half-applied */
        }
    }
}

static void test_fuzz_mutated_valid_blobs_never_crash(void)
{
    zw_node_cache_t base;
    zw_node_cache_init(&base);
    fill_full_record(zw_node_add(&base, 5u), 5u);
    zw_node_record_t plain = {0};
    plain.node_id = 12u;
    plain.security_classes = ZW_NODE_SECURITY_UNKNOWN;
    plain.endpoints[1].used = true;
    plain.endpoints[1].has_class = true;
    plain.endpoints[1].cc_count = 1u;
    plain.endpoints[1].ccs[0] = 0x30u;
    memcpy(zw_node_add(&base, 12u), &plain, sizeof(plain));
    uint8_t good[BUF_MAX];
    const size_t good_len = zw_node_cache_encode(&base, good, sizeof(good));
    TEST_ASSERT_TRUE(good_len > 0u);

    for (uint32_t iter = 0; iter < 20000u; iter++) {
        uint8_t mutated[BUF_MAX];
        memcpy(mutated, good, good_len);
        const uint32_t flips = rnd_range(1u, 6u);
        for (uint32_t f = 0; f < flips; f++) {
            mutated[rnd_range(0u, (uint32_t)good_len - 1u)] = rnd_byte();
        }
        const size_t len = (rnd_byte() & 1u) != 0u ? good_len : (size_t)rnd_range(0u, (uint32_t)good_len);

        zw_node_cache_t cache;
        seed_sentinel(&cache);
        if (!zw_node_cache_decode(&cache, mutated, len)) {
            assert_is_sentinel(&cache); /* refused: never half-applied */
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (getenv("ZW_DUMP_NODES") != NULL) {
        uint8_t buf[BUF_MAX];
        const size_t n = build_fixture(buf, sizeof(buf));
        printf("static const uint8_t ZW_NODES_FIXTURE_V1[%zu] = {\n", n);
        for (size_t i = 0; i < n; i++) {
            printf("%s0x%02X,%s", (i % 12u == 0u) ? "    " : "", buf[i], (i % 12u == 11u) ? "\n" : " ");
        }
        printf("\n};\n");
        return 0;
    }

    UNITY_BEGIN();
    RUN_TEST(test_crc32_standard_check_value);
    RUN_TEST(test_accessors);
    RUN_TEST(test_round_trip_full_cache);
    RUN_TEST(test_zero_values_omitted);
    RUN_TEST(test_unknown_fields_and_groups_round_trip);
    RUN_TEST(test_unknown_area_overflow_is_lost_not_refused);
    RUN_TEST(test_damage_refused);
    RUN_TEST(test_versions_field);
    RUN_TEST(test_alarm_sensor_field);
    RUN_TEST(test_alarm_sensor_field_empty);
    RUN_TEST(test_limits);
    RUN_TEST(test_over_limit_cc_list);
    RUN_TEST(test_over_limit_endpoint_cc_list);
    RUN_TEST(test_over_limit_endpoint_index);
    RUN_TEST(test_over_limit_mask);
    RUN_TEST(test_over_limit_notification);
    RUN_TEST(test_fixture_decodes_to_expected);
    RUN_TEST(test_fixture_v2_alarm_sensor);
    RUN_TEST(test_reconcile_every_row);
    RUN_TEST(test_fuzz_random_bytes_never_crash);
    RUN_TEST(test_fuzz_mutated_valid_blobs_never_crash);
    return UNITY_END();
}
