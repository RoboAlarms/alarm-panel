/*
 * zw_nodes.h - the node cache (docs/features/36, network.md §2 "What lives where" / "Node
 * cache" / "At every start"; ZW-017): what the interview learned about each node, as a
 * versioned, self-describing blob like the alarm configuration blob (alarm_core/src/codec.c)
 * and the zone binding blob (zone_hub/src/bind_codec.c) - same shape (`MAGIC | format | compat |
 * header length | payload length | CRC-32`, then `field | group | len | value` items), a
 * dedicated CRC-32 (zw_nodes.c's own copy - zwave_host depends on nothing but zwave_proto, so it
 * does not reach into alarm_core for one), and reconciliation against what the module reports at
 * start-up (serial-api.md §5 step 19).
 *
 * Byte order: this blob's own multi-byte fields (header lengths, the CRC, node IDs, generation,
 * wake-up seconds, icons, manufacturer/product IDs...) are little-endian, following the storage
 * convention of the blobs above - *not* Z-Wave's own wire convention (big-endian, serial-api.md/
 * command-classes.md), which this file never touches directly.
 */
#ifndef ZW_NODES_H
#define ZW_NODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits (network.md §2, zones.md §7 "The source's timers and limits") ----------------------
 * Chosen from what the project's own sensor line-up (zones.md §8's quirks table) actually carries,
 * with headroom, not from the Z-Wave specification's own ceilings:
 * - ZW_NODES_MAX matches zsrc_zwave's ZSRC_ZW_DEV_MAX (zones.md §7): the zone source can use no
 *   more devices than this cache can remember.
 * - A modern Z-Wave Plus security sensor's own command class list runs to roughly 17-25 entries
 *   (Z-Wave Plus Info, Version, Manufacturer Specific, Device Reset Locally, Association, AGI,
 *   Multi Channel Association, Supervision, Security 2, Transport Service, CRC-16, Multi Command,
 *   Notification or Binary/Alarm Sensor, Battery, Wake Up, Configuration...): ZW_NODE_CC_MAX
 *   gives it room to spare.
 * - None of the project's own sensors (zones.md §8) use Multi Channel: it is a rare "extra
 *   terminal" feature, not the common case, so ZW_NODE_ENDPOINTS_MAX (endpoint 0, the device
 *   itself, plus this many Multi Channel endpoints) and ZW_NODE_ENDPOINT_CC_MAX are modest -
 *   zones.md's own ceiling of 7 Multi Channel endpoints is an upper bound on what the zone
 *   *offering* layer will use, not a requirement that every device's cache entry store that many.
 * - ZW_NODE_NOTIFICATION_MAX (notification types tracked per endpoint): a combination smoke/CO
 *   detector reports Smoke, Heat, CO, Home Security (tamper), Power Management and System - 6
 *   types - so 4 was too tight (a CO type left undecoded could mean the panel never offers that
 *   detector's CO input: a life-safety gap, not just a cosmetic one); 8 gives that combination
 *   room to grow.
 * - ZW_NODE_MASK_BYTES (any supported-item bit mask: binary sensor types, multilevel sensor
 *   types, a notification's event mask) covers every type command-classes.md §4.5's event table
 *   lists with room to spare (32 bits).
 */
#define ZW_NODES_MAX 64u
#define ZW_NODE_CC_MAX 32u
#define ZW_NODE_ENDPOINTS_MAX 4u /* index 0 = the device itself; 1..3 = Multi Channel endpoints */
#define ZW_NODE_ENDPOINT_CC_MAX 12u
#define ZW_NODE_NOTIFICATION_MAX 8u
#define ZW_NODE_MASK_BYTES 4u
#define ZW_NODE_DSK_BYTES 16u

/* Bounded raw storage for what this build does not decode. Two distinct reasons land here, kept
 * the same way (network.md §2: "unknown fields and groups are kept and written back"):
 * - a field or group this build has never heard of (an older field ID, or one from a later
 *   milestone this firmware predates);
 * - a well-formed item of a field this build *does* know, but whose count or index is beyond
 *   this firmware's own fixed limits above (more CCs than ZW_NODE_CC_MAX, an endpoint index or a
 *   mask longer than this build holds, a 9th notification type on one endpoint...) - a later
 *   firmware with bigger limits wrote it, and a rollback must not throw it away along with the
 *   rest of that node (its generation is part of the zone binding address, zones.md "Addresses":
 *   losing the node loses every zone bound to it). Counted separately in
 *   zw_node_cache_t.over_limit so the platform can tell "unknown" from "known but outgrown"
 *   apart if it ever needs to.
 * Per node (kept against the node that was current) and, separately, for the cache as a whole: an
 * entire unknown group (which names no node in particular), and also - since ZW_NODES_MAX is a
 * real, fixed ceiling - a node beyond the ZW_NODES_MAX'th distinct one, whose whole item stream
 * (its own node-start marker onward) is preserved the same way rather than decoded, so a cache
 * this build cannot fully hold still round-trips unchanged. Both are simple concatenations of
 * complete items (their own field|group|len|value bytes, unparsed) and are written back by
 * zw_node_cache_encode() - per node right after that node's own recognised fields, skipping any
 * raw item a later decoded write already covers (so a value this firmware *can* now represent
 * supersedes the old raw copy instead of duplicating it): field + endpoint for the repeated
 * per-endpoint fields 0A/0C/0D/10/12, field + endpoint + notification type for 0B (several of those
 * legitimately coexist per endpoint, so a raw one must wait for a decoded entry of its own exact
 * type, not just any type on the same endpoint) - the cache-wide ones at the very end. When
 * either bounded area fills up, the item that no longer fits is *dropped*, not the cache decode:
 * zw_node_cache_t.unknown_lost counts the bytes lost this way (the platform can log it); nothing
 * else about the decode fails or changes because of it. */
#define ZW_NODE_UNKNOWN_BYTES 128u
#define ZW_NODE_FW_TARGETS_MAX 4u /* other firmware targets a record keeps (field 11) */
#define ZW_CACHE_UNKNOWN_GROUP_BYTES 512u

/* field 04 security: a granted-classes bit mask (security.md §1); this value is reserved to mean
 * "not yet known" (a node record starts here, and every other value - including 00, "confirmed no
 * security" - is a real answer). */
#define ZW_NODE_SECURITY_UNKNOWN 0xFFu

/* field 0x10 push/pull (network.md §5 "push or pull"): whether the node's notifications reach the
 * panel by themselves or must be polled. */
#define ZW_NODE_MODE_PULL 0u
#define ZW_NODE_MODE_PUSH 1u

/* One command class this node (or one of its endpoints) lists. */
typedef struct {
    uint8_t cc;
    uint8_t version;   /* 0 = not yet asked (root list only; an endpoint's list carries no version) */
    bool supported;
    bool secure;
} zw_node_cc_t;

/* One notification type an endpoint reports, and the events it can raise for it. */
typedef struct {
    uint8_t notification_type;
    uint8_t event_mask[ZW_NODE_MASK_BYTES];
    uint8_t event_mask_len;
} zw_node_notification_t;

/* Endpoint 0 is the device itself (its own class and CC list live in the node's own `protocol`/
 * `ccs` fields, not here - `has_class` is never set for it); 1.. are Multi Channel endpoints. */
typedef struct {
    bool used; /* anything at all is known about this endpoint */

    bool has_class; /* field 0A: Multi Channel Capability Report (1.. only) */
    uint8_t generic_class;
    uint8_t specific_class;
    uint8_t cc_count;
    uint8_t ccs[ZW_NODE_ENDPOINT_CC_MAX]; /* plain CC IDs: Capability Report carries no version */

    uint8_t notification_count; /* field 0B, one wire item per entry */
    zw_node_notification_t notifications[ZW_NODE_NOTIFICATION_MAX];

    bool has_binary_sensor_mask; /* field 0C */
    uint8_t binary_sensor_mask[ZW_NODE_MASK_BYTES];
    uint8_t binary_sensor_mask_len;

    bool has_multilevel_mask; /* field 0D */
    uint8_t multilevel_mask[ZW_NODE_MASK_BYTES];
    uint8_t multilevel_mask_len;

    bool has_push_pull; /* field 0x10 */
    uint8_t push_pull_mode; /* ZW_NODE_MODE_* */

    /* field 0x12: the Alarm Sensor (legacy) types its Supported Report lists, bit t = type t
     * (bit 0, general purpose, is a real type). A blob written before this field has none: its
     * endpoints read as "not asked" (BL-085). */
    bool has_alarm_sensor_mask;
    uint8_t alarm_sensor_mask[ZW_NODE_MASK_BYTES];
    uint8_t alarm_sensor_mask_len;
} zw_node_endpoint_t;

/* One node's cache entry (network.md §2's table, field by field). */
typedef struct {
    uint16_t node_id; /* field 00 (group 01): starts the node */

    uint16_t generation; /* 01; 0 = never set (a stored node's is always > 0) */

    bool has_protocol; /* 02 */
    bool is_listening;
    bool is_flirs;
    bool is_routing;
    bool supports_long_range;
    uint8_t basic_class;
    uint8_t generic_class;
    uint8_t specific_class;

    uint8_t cc_count; /* 03 */
    zw_node_cc_t ccs[ZW_NODE_CC_MAX];

    uint8_t security_classes; /* 04: ZW_NODE_SECURITY_UNKNOWN until known */

    bool has_dsk; /* 05 */
    uint8_t dsk[ZW_NODE_DSK_BYTES];

    bool has_identity; /* 06 */
    uint16_t manufacturer_id;
    uint16_t product_type;
    uint16_t product_id;

    bool has_zwave_plus; /* 07 */
    uint8_t zwave_plus_role_type;
    uint8_t zwave_plus_node_type;
    uint16_t installer_icon;
    uint16_t user_icon;

    bool has_wake_up; /* 08 */
    uint32_t wake_up_interval_seconds;
    uint32_t wake_up_min_seconds;
    uint32_t wake_up_max_seconds;
    uint32_t wake_up_step_seconds;
    uint8_t wake_up_notify_node_id; /* Wake Up predates 16-bit node IDs: always one byte */

    bool has_lifeline; /* 09 */
    uint8_t lifeline_group;
    bool lifeline_set;

    zw_node_endpoint_t endpoints[ZW_NODE_ENDPOINTS_MAX]; /* 0A, 0B, 0C, 0D, 0x10, 0x12 */

    uint8_t interview_step; /* 0E; 0 = none completed yet (network.md §5 numbers steps from 1) */

    bool has_quirk; /* 0F: by model IDs, the same shape as `identity` */
    uint16_t quirk_manufacturer_id;
    uint16_t quirk_product_type;
    uint16_t quirk_product_id;

    /* 11: its own versions, from its Version Report (the interview's step 6; ZW-033, firmware
     * updates): firmware target 0, hardware (0xFF: not reported, a v1 report), its Z-Wave
     * protocol, and the other firmware targets' (up to ZW_NODE_FW_TARGETS_MAX; more are kept
     * raw). */
    bool has_versions;
    uint8_t fw_major, fw_minor;
    uint8_t hw_version;
    uint8_t protocol_major, protocol_minor;
    uint8_t fw_target_count;                       /* other targets reported (1..) */
    uint8_t fw_targets[ZW_NODE_FW_TARGETS_MAX][2]; /* each major, minor */

    uint8_t unknown[ZW_NODE_UNKNOWN_BYTES]; /* raw, complete items this build does not know */
    uint16_t unknown_len;
} zw_node_record_t;

/* The whole cache. A compact list (no gaps): `nodes[0..node_count-1]` are the live records. */
typedef struct {
    zw_node_record_t nodes[ZW_NODES_MAX];
    uint8_t node_count;

    uint8_t unknown_groups[ZW_CACHE_UNKNOWN_GROUP_BYTES]; /* raw items of an unrecognised group */
    size_t unknown_groups_len;

    size_t unknown_lost; /* bytes dropped because a raw area (per node or the one above) was full */
    size_t over_limit;   /* items kept raw because they were beyond this firmware's own limits,
                             not because the field/group was unrecognised - see ZW_NODE_UNKNOWN_BYTES's
                             comment; also counts one for a node beyond ZW_NODES_MAX */
} zw_node_cache_t;

void zw_node_cache_init(zw_node_cache_t *cache);

/* CRC-32/ISO-HDLC (zlib's crc32; the same polynomial as alarm_core/src/codec.c's alarm_crc32() and
 * zone_hub/src/bind_codec.c's blob use). Start a message with crc = 0; chainable across several
 * calls the same way, each continuing from the previous one's return value - the codec here
 * always CRCs one contiguous payload in a single call. */
uint32_t zw_nodes_crc32(uint32_t crc, const void *data, size_t len);

/* ---- accessors: find, add, remove, iterate ------------------------------------------------------ */

zw_node_record_t *zw_node_find(zw_node_cache_t *cache, uint16_t node_id);
const zw_node_record_t *zw_node_find_const(const zw_node_cache_t *cache, uint16_t node_id);
/* NULL when 'node_id' is already present, is 0, or the cache is at ZW_NODES_MAX. */
zw_node_record_t *zw_node_add(zw_node_cache_t *cache, uint16_t node_id);
/* true if 'node_id' was present (and is now gone); the remaining nodes keep their relative order. */
bool zw_node_remove(zw_node_cache_t *cache, uint16_t node_id);
static inline size_t zw_node_count(const zw_node_cache_t *cache) { return cache->node_count; }
/* 'index' < zw_node_count(cache); NULL otherwise. */
zw_node_record_t *zw_node_at(zw_node_cache_t *cache, size_t index);
const zw_node_record_t *zw_node_at_const(const zw_node_cache_t *cache, size_t index);

/* ---- codec (network.md §2) ------------------------------------------------------------------------
 * encode() never writes at or past 'cap'. On failure (a buffer too small) it returns 0; the bytes
 * of 'out' below 'cap' may then hold part of the payload, but its 16-byte header is written only on
 * success - a failed encode's buffer is not a blob to store. decode() never partially applies a
 * damaged blob: a refusal of the header, the CRC or the items (its first pass) leaves 'cache'
 * untouched; a second-pass failure (not expected once the first agreed) leaves it empty
 * (zw_node_cache_init()). After false, then, 'cache' is either as it was or empty: call it on a
 * scratch cache and swap if "refused changes nothing" must hold for the caller's live cache. */
size_t zw_node_cache_encode(const zw_node_cache_t *cache, uint8_t *out, size_t cap);
bool zw_node_cache_decode(zw_node_cache_t *cache, const uint8_t *buf, size_t len);

/* ---- reconciliation (network.md §2 "At every start", pure - no I/O) ------------------------------- */

typedef enum {
    ZW_RECONCILE_NORMAL = 0,       /* home ID matches the stored keys': reconcile the node lists */
    ZW_RECONCILE_REPLACED_OR_RESET, /* home ID differs, module empty, keys+cache for the old one */
    ZW_RECONCILE_FIRST_START,      /* no keys at all, module empty: generate keys for this home ID */
    ZW_RECONCILE_FOREIGN,          /* a module whose nodes the panel must not use (see network.md) */
} zw_reconcile_case_t;

typedef struct {
    zw_reconcile_case_t reconcile_case;
    /* ZW_RECONCILE_NORMAL only: nodes the module lists that the cache does not - interview them. */
    uint16_t to_interview[ZW_NODES_MAX];
    size_t to_interview_count;
    /* ZW_RECONCILE_NORMAL only: nodes the cache has that the module no longer lists - forget them. */
    uint16_t to_forget[ZW_NODES_MAX];
    size_t to_forget_count;
} zw_reconcile_result_t;

/* 'have_keys' false means no keys are stored at all ('keys_home_id' is then ignored). */
void zw_node_reconcile(uint32_t module_home_id, const uint16_t *module_nodes, size_t module_node_count,
                        bool have_keys, uint32_t keys_home_id, const zw_node_cache_t *cache,
                        zw_reconcile_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_NODES_H */
