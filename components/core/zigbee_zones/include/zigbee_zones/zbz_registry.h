/*
 * zbz_registry.h - the panel's Zigbee devices, one record per device (docs/features/20
 * devices.md §4; ZB-010, ZB-021): what it is, what it was named, how it joined, its endpoints and
 * where its setup is. The zone source (zone_hub's zsrc_zigbee) keeps one slot per endpoint in RAM
 * for the zones; this is the device list the pages, the console, the Panel Manager and backups
 * read, and it survives restarts.
 *
 * Stored in the panel's NVS partition (no partition change), namespace ZBZ_NVS_NAMESPACE, one key
 * per device so a change rewrites ~200 bytes and not the whole table:
 *   meta     compat (1) | next id u16 | devices u8                      (ZBZ_META_LEN bytes)
 *   d00..d63 compat (1) | fields: id u8 | length u8 | value             (one per slot)
 * Little-endian. A field whose value is zero (or "") is not written and reads as zero; a field
 * id this reader doesn't know is skipped, so a later firmware only adds fields (never renumbers
 * one, never reuses one). `compat` rises only when an older reader would misread a record: a
 * reader refuses a higher compat (ZBZ_REG_FUTURE) and the registry is then kept read-only -
 * nothing is written over a newer panel's records. The first frozen fixture
 * (test/registry_fixture_v1.h) came with the codec in ZB-M2: made once by the encoder from
 * records written out field by field, annotated field by field, and never edited after;
 * testing.md §6 lists what it must hold.
 *
 * Decided in ZB-M2 (src/zbz_registry.c):
 * - No checksum of its own: NVS keeps a CRC-32 of every blob and refuses a damaged one; the
 *   record's own checks are its lengths, its compat and its required fields.
 * - A record of a higher compat is refused, not guessed at: its device is missing from the list
 *   (and the registry read-only) until the newer firmware is back. "Read as far as known" means
 *   the records at a known compat; meta's next id is read only at a known compat.
 * - A known number field of another length is damage; an endpoint field longer than 11 bytes
 *   keeps its first 11 (a later firmware's additions); a text longer than its buffer is cut at a
 *   character boundary; endpoints past ZBZ_EP_MAX and endpoint 0 are skipped.
 * - A record dropped as damaged (or the second of two with one address or one id) is erased at
 *   the next write-back; meta is rewritten whenever it disagrees with the records. Fewer
 *   records than meta counts is DAMAGED (some were lost).
 *
 * Writes go through the take pattern (zwz_panel's): under the zone lock the owner takes the next
 * changed key and its bytes, then writes NVS after unlocking, from a task whose stack is in
 * internal RAM (docs/bugs/B001).
 *
 * Built in ZB-M2 (test/test_zbz_registry.c). Portable C11, caller-owned state, no heap.
 */
#ifndef ZBZ_REGISTRY_H
#define ZBZ_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZBZ_DEV_MAX   64u /* devices (ZB-010): the coordinator's own children are ZB_MAX_CHILDREN
                           * (32) in the firmware; the rest join through routers */
#define ZBZ_EP_MAX    4u  /* endpoints kept per device: the ones with clusters the panel uses
                           * first; a Green Power proxy (242) or a vendor profile's last */
#define ZBZ_NAME_LEN  24u /* the installer's name, UTF-8 with its NUL */
#define ZBZ_TEXT_LEN  32u /* manufacturer and model as the device reports them */
#define ZBZ_SW_LEN    24u /* Basic SW build id */

#define ZBZ_NVS_NAMESPACE "zbpanel" /* at most 15 characters; not the stack's own namespace */
#define ZBZ_KEY_META      "meta"
#define ZBZ_KEY_LEN       8u        /* "d00".."d63" and the other keys, with the NUL */
#define ZBZ_COMPAT        1u
/* Added in the ZB-M10/M11 wiring (BL-143): a compat byte no format will ever use - 0xFF is what
 * erased or overwritten flash reads as, and a later format raises compat one step at a time - so a
 * record or meta that starts with it is DAMAGED (dropped, erased at the next write-back), never
 * FUTURE (which would keep the whole registry read-only for good). 0 is damage likewise. */
#define ZBZ_COMPAT_ERASED 0xFFu
#define ZBZ_META_LEN      4u
#define ZBZ_RECORD_MAX    256u      /* the longest record this format can write */

/* Field ids of a device record (devices.md §4). Stored: never renumber, never reuse. */
#define ZBZ_F_ID           1u  /* u16, required */
#define ZBZ_F_IEEE         2u  /* u64, required */
#define ZBZ_F_NWK          3u  /* u16: the last short address known */
#define ZBZ_F_NAME         4u  /* text: the installer's name */
#define ZBZ_F_MANUFACTURER 5u  /* text (Basic 0x0004) */
#define ZBZ_F_MODEL        6u  /* text (Basic 0x0005) */
#define ZBZ_F_SW_BUILD     7u  /* text (Basic 0x4000) */
#define ZBZ_F_APP_VERSION  8u  /* u8 */
#define ZBZ_F_HW_VERSION   9u  /* u8 */
#define ZBZ_F_MFR_CODE     10u /* u16 (node descriptor) */
#define ZBZ_F_NODE_TYPE    11u /* u8 zbz_node_type_t */
#define ZBZ_F_MAC_CAPS     12u /* u8 ZSRC_ZB_MAC_* */
#define ZBZ_F_POWER_SOURCE 13u /* u8 (Basic 0x0007) */
#define ZBZ_F_JOIN         14u /* u8 zsrc_zb_join_t */
#define ZBZ_F_JOINED_AT    15u /* u32 Unix seconds */
#define ZBZ_F_ROLE         16u /* u8 zbz_role_t */
#define ZBZ_F_SETUP        17u /* u8 zbz_setup_t */
#define ZBZ_F_SETUP_STEP   18u /* u8 zsrc_zb_step_t */
#define ZBZ_F_CHECK_IN     19u /* u32 seconds (Poll Control) */
#define ZBZ_F_ENDPOINT     20u /* 11 bytes, once per endpoint: ep u8 | profile u16 | device u16 |
                                * in-cluster bits u16 | out-cluster bits u16 | zone type u16 */
#define ZBZ_F_WD_MAX_DURATION 21u /* u16: a siren's MaxDuration (IAS WD 0x0000) as read at its
                                   * setup, plus 1 (0..0xFFFE stored 1..0xFFFF): missing is "not
                                   * read", told apart from a valid 0. Added for ZB03 at compat 1:
                                   * an older reader skips it (an unknown id) */

/* What the node descriptor said it is (zero: not read yet). Stored: never renumber. */
typedef enum {
    ZBZ_NODE_UNKNOWN = 0,
    ZBZ_NODE_ROUTER,
    ZBZ_NODE_END_DEVICE,
} zbz_node_type_t;

/* What the panel uses it as. Stored: never renumber. */
typedef enum {
    ZBZ_ROLE_UNKNOWN = 0,  /* its setup hasn't said yet */
    ZBZ_ROLE_SENSOR,       /* inputs for zones (IAS Zone, On/Off, Occupancy, Temperature) */
    ZBZ_ROLE_SIREN,        /* IAS WD (ZB-023) */
    ZBZ_ROLE_KEYPAD,       /* an IAS ACE client with a keypad (ZB-024) */
    ZBZ_ROLE_KEYFOB,       /* an IAS ACE client without one, or IAS zone type key fob */
    ZBZ_ROLE_REPEATER,     /* a router with nothing the panel uses */
    ZBZ_ROLE_OTHER,        /* lights, plugs, locks: tier C (BL-149) */
} zbz_role_t;

/* Where its setup is (devices.md §5). Stored: never renumber. A record read back as RUNNING or
 * WAKE (the panel restarted during its setup) becomes STOPPED: "Set it up again" goes on from
 * `setup_step`, as herdsman resets an interview left in progress. */
typedef enum {
    ZBZ_SETUP_NEW = 0,     /* joined, nothing asked yet */
    ZBZ_SETUP_RUNNING,
    ZBZ_SETUP_WAKE,        /* waiting for a sleepy device to wake up */
    ZBZ_SETUP_STOPPED,     /* gave up after its attempts: "Set it up again" */
    ZBZ_SETUP_DONE,
    ZBZ_SETUP_NOT_USABLE,  /* done, and nothing the panel uses */
} zbz_setup_t;

typedef struct {
    uint8_t endpoint;      /* 1..240 (242: Green Power proxy, recorded, never used) */
    uint16_t profile;
    uint16_t device_id;
    uint16_t in_clusters;  /* ZSRC_ZB_CL_* bits */
    uint16_t out_clusters; /* ZSRC_ZB_CL_* bits (a keypad's IAS ACE) */
    uint16_t zone_type;    /* ZSRC_ZB_ZONE_TYPE_*, NONE when it has no IAS Zone */
} zbz_endpoint_t;

/* One device, as stored. */
typedef struct {
    bool used;
    uint16_t id;           /* 1..65535; see zbz_reg_add */
    uint64_t ieee;
    uint16_t nwk;
    char name[ZBZ_NAME_LEN];
    char manufacturer[ZBZ_TEXT_LEN];
    char model[ZBZ_TEXT_LEN];
    char sw_build[ZBZ_SW_LEN];
    uint8_t app_version, hw_version;
    uint16_t manufacturer_code;
    uint8_t node_type;     /* zbz_node_type_t */
    uint8_t mac_caps;      /* ZSRC_ZB_MAC_* */
    uint8_t power_source;  /* Basic's, 0 not known */
    uint8_t join;          /* zsrc_zb_join_t: the per-device trust record (ZB-021) */
    uint32_t joined_at;    /* Unix seconds, 0 the clock wasn't set */
    uint8_t role;          /* zbz_role_t */
    uint8_t setup;         /* zbz_setup_t */
    uint8_t setup_step;    /* zsrc_zb_step_t it is at, or stopped at */
    uint32_t check_in_s;   /* Poll Control's check-in, 0 none or not read */
    uint8_t ep_count;
    zbz_endpoint_t ep[ZBZ_EP_MAX];
    /* Added for ZB03 (ZBZ_F_WD_MAX_DURATION): its IAS WD MaxDuration, wd_max_read false when it
     * wasn't read (or answered no valid value); wd_max_s 0..0xFFFE, 0 a valid 0. */
    bool wd_max_read;
    uint16_t wd_max_s;
} zbz_device_t;

typedef struct {
    zbz_device_t dev[ZBZ_DEV_MAX];
    uint16_t next_id;      /* the next id to give (1 after 65535; ids in use are skipped) */
    uint64_t dirty;        /* bit i: slot i to write (or erase) */
    bool meta_dirty;
    bool read_only;        /* ZBZ_REG_FUTURE: never written */
} zbz_registry_t;

typedef enum {
    ZBZ_REG_OK = 0,
    ZBZ_REG_EMPTY,         /* nothing stored (a new panel) */
    ZBZ_REG_DAMAGED,       /* some records were unreadable and dropped: those devices are found
                            * again by their traffic and offered "Set it up again" */
    ZBZ_REG_FUTURE,        /* a later format: read as far as known, kept read-only */
} zbz_reg_load_t;

/* The owner's NVS, read once at start (before the service's tasks run). */
typedef struct {
    void *ctx;
    /* key's value into buf: its length; 0 when there is none; more than cap: too long (nothing
     * read - the record is dropped as damaged). */
    size_t (*read)(void *ctx, const char *key, uint8_t *buf, size_t cap);
} zbz_store_port_t;

void zbz_reg_init(zbz_registry_t *r);
zbz_reg_load_t zbz_reg_load(zbz_registry_t *r, const zbz_store_port_t *store);

/* ---- the devices ------------------------------------------------------------------------------- */

zbz_device_t *zbz_reg_find(zbz_registry_t *r, uint64_t ieee);
zbz_device_t *zbz_reg_get(zbz_registry_t *r, uint16_t id);
const zbz_device_t *zbz_reg_cget(const zbz_registry_t *r, uint16_t id);
size_t zbz_reg_count(const zbz_registry_t *r);
/* A new record for a device that joined: a free slot and the next id (never 0, never one in
 * use, so a page left open on a removed device can't show a newer one). NULL: full, read-only,
 * address 0, or an address already in it (zbz_reg_find gives that one). */
zbz_device_t *zbz_reg_add(zbz_registry_t *r, uint64_t ieee, uint16_t nwk);
/* Removed or forgotten: its slot is freed and its key erased. */
bool zbz_reg_drop(zbz_registry_t *r, uint16_t id);
/* The record changed: it is to be written. */
void zbz_reg_touch(zbz_registry_t *r, const zbz_device_t *d);

/* One endpoint's simple descriptor into the record (kept, replaced, or - full - the least useful
 * one given up); false when it can't be kept. Cluster ids become ZSRC_ZB_CL_* bits. */
bool zbz_dev_set_endpoint(zbz_device_t *d, const zsrc_zb_simple_desc_t *s);
/* A list of cluster ids as ZSRC_ZB_CL_* bits (unknown ids ignored). */
uint16_t zbz_cluster_bits(const uint16_t *ids, uint8_t count);
/* What the zone source is told about one of its endpoints (zsrc_zb_device_seen): the converter
 * testing.md §2 checks against every signature fixture. false: no such endpoint. */
bool zbz_dev_desc(const zbz_device_t *d, uint8_t endpoint, zsrc_zb_desc_t *out);
/* Router or sleepy, from what is known (node descriptor, MAC capabilities, power source). */
bool zbz_dev_router(const zbz_device_t *d);
bool zbz_dev_sleepy(const zbz_device_t *d);

/* ---- what is stored ----------------------------------------------------------------------------- */

size_t zbz_dev_encode(const zbz_device_t *d, uint8_t *out, size_t cap);
/* One record: OK, DAMAGED (truncated, a length past the end, no id or address, compat 0 or
 * ZBZ_COMPAT_ERASED: *d unused) or FUTURE (a higher compat). meta_decode likewise. */
zbz_reg_load_t zbz_dev_decode(zbz_device_t *d, const uint8_t *buf, size_t len);
size_t zbz_meta_encode(const zbz_registry_t *r, uint8_t *out, size_t cap);
zbz_reg_load_t zbz_meta_decode(zbz_registry_t *r, const uint8_t *buf, size_t len);

typedef enum {
    ZBZ_TAKE_NONE = 0,     /* nothing to store */
    ZBZ_TAKE_WRITE,        /* write `len` bytes of buf to `key` */
    ZBZ_TAKE_ERASE,        /* erase `key` */
} zbz_take_t;
/* The next change to store, its dirty bit cleared (meta last). A write that failed is asked for
 * again with zbz_reg_redo. Nothing while read-only, or when `cap` is under ZBZ_RECORD_MAX (the
 * change stays to take). */
zbz_take_t zbz_reg_take(zbz_registry_t *r, char key[ZBZ_KEY_LEN], uint8_t *buf, size_t cap,
                        size_t *len);
void zbz_reg_redo(zbz_registry_t *r, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_REGISTRY_H */
