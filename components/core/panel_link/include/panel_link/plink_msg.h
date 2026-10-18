/*
 * plink_msg.h - the panel link's message payloads (docs/features/11 "Messages"; LINK-005,
 * LINK-017): one struct per message and its encoder and decoder. The header and the sealing are
 * plink_frame.h; this is what goes inside.
 *
 * Rules (LINK-017): fields are only appended; every variable part (a section, a record) carries
 * its length, and a decoder takes what it knows and skips the rest; a decoder of a payload
 * shorter than v1's fixed part refuses it. Encoders return false when the payload doesn't fit
 * cap; decoders return false for a payload they can't use (the frame is then dropped).
 *
 * The four messages that end in a MAC (PAIR_CONFIRM, PAIR_DONE, HELLO, WELCOME) are encoded with
 * their mac field as given; the MAC covers the whole frame before it, so the caller builds the
 * frame and then fills the last 16 bytes (plink_mac_fill in plink_pair.h / plink_session.h). Their
 * decoders take the MAC from the payload's last 16 bytes, whatever lies between.
 *
 * The payload layouts are an external contract between firmware versions: never reorder.
 */
#ifndef PLINK_MSG_H
#define PLINK_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_engine.h"
#include "alarm/alarm_types.h"
#include "panel_link/plink_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WIFI_REQ: nonzero request id u32 LE. WIFI: echoed id, ssid[33], password[65].
 * Fixed terminated strings; empty SSID means unavailable. Only in a sealed session
 * with WIFI negotiated. Receivers wipe these buffers immediately after use. */
typedef struct { char ssid[33]; char password[65]; } plink_wifi_t;
bool plink_enc_wifi(const plink_wifi_t *wifi, uint32_t req, uint8_t *out, size_t cap);
bool plink_dec_wifi(const uint8_t *p, size_t n, uint32_t req, plink_wifi_t *out);

#define PLINK_PROTO 1u     /* the protocol version this firmware speaks */

/* Capability bits (the caps of PAIR_REQ, PAIR_COMMIT, HELLO and WELCOME; LINK-017, LINK-043). A
 * panel offers them at run time (plink_master_init, plink_keypad_init); a session keeps the bits
 * both sides offered, and a type or a field of a capability goes only in a session with its bit.
 * Never renumbered or reused; the next free bit is 0x0010. */
#define PLINK_CAP_WIFI  0x0008u /* authenticated, requested enrollment credentials */
#define PLINK_CAP_FW    0x0001u /* updates through the link: FW_INFO..FW_PAGE */
#define PLINK_CAP_HOME  0x0002u /* Smart Home on keypads: DEV_LIST_REQ..DEV_ACK */
#define PLINK_CAP_PREFS 0x0004u /* a keypad's settings: PREFS, PREFS_REQ, PREFS_SET, HEARTBEAT's
                                 * section */
/* What this firmware's codecs know: an offer is masked with it, so a peer's unknown bit is never
 * common. */
#define PLINK_CAPS_KNOWN (PLINK_CAP_FW | PLINK_CAP_HOME | PLINK_CAP_PREFS | PLINK_CAP_WIFI)

#define PLINK_NAME_MAX  47u /* names on the wire, bytes without the NUL */
#define PLINK_FW_MAX    31u
#define PLINK_TZ_MAX    63u
#define PLINK_PANEL_ID_LEN 6u /* the master's panel id, six hex characters (proto_panel_id) */
#define PLINK_PARTS_MAX 8u  /* partition records a STATUS can carry (the mask is 8 bits) */
#define PLINK_ZONE_REC_LEN  7u
#define PLINK_EVENT_REC_LEN 32u /* the record's fields add up to 32 bytes */
#define PLINK_CFG_PAGE_HDR  14u /* CFG_PAGE's bytes before the page: cfg_crc, total, offset, len */

/* What a keypad is (PAIR_REQ kind). Numbers are stored in the master's records: never renumber. */
typedef enum {
    PLINK_KIND_UNKNOWN = 0,
    PLINK_KIND_PANEL = 1, /* a touchscreen panel in the keypad role (P4) */
    PLINK_KIND_SMALL = 2, /* a small screen (the 4" CYD, docs/features/19) */
    PLINK_KIND_PC = 3,    /* the PC virtual keypad (host/link_sim) */
} plink_kind_t;

/* PAIR_FAIL reasons. On the wire: never renumber. */
typedef enum {
    PLINK_FAIL_NONE = 0,
    PLINK_FAIL_REFUSED = 1,     /* a person said no on either side */
    PLINK_FAIL_TIMEOUT = 2,     /* a step took longer than 30 s, or the comparison 2 min */
    PLINK_FAIL_MISMATCH = 3,    /* a commitment or a MAC didn't verify */
    PLINK_FAIL_BUSY = 4,        /* another attempt is under way */
    PLINK_FAIL_CLOSED = 5,      /* pairing isn't open */
    PLINK_FAIL_FULL = 6,        /* no keypad number is free */
    PLINK_FAIL_OVERLAP = 7,     /* more than one keypad or master at once */
    PLINK_FAIL_LIMIT = 8,       /* 3 attempts used: the window closed */
    PLINK_FAIL_CAPACITY = 9,    /* the keypad can't hold the master's zones */
    PLINK_FAIL_CANCELLED = 10,  /* cancelled on either side */
    PLINK_FAIL_INTERRUPTED = 11,/* a step's value changed mid-attempt (LINK-016) */
    PLINK_FAIL_STORE = 12,      /* the record could not be stored */
    PLINK_FAIL_VERSION = 13,    /* no common protocol version */
} plink_fail_t;

/* A keypad's capacities (PAIR_REQ, HELLO): what its configuration copy can hold. */
typedef struct {
    uint8_t partitions;
    uint16_t zones;
    uint8_t users;
    uint8_t devices;
} plink_caps_t;

/* ---- pairing (plain) ---------------------------------------------------------------------- */
typedef struct {
    uint32_t attempt;
    uint8_t proto;
    uint16_t caps;
    uint8_t kind; /* plink_kind_t */
    plink_caps_t cap;
    uint8_t pk[PLINK_X25519_LEN];
    char name[PLINK_NAME_MAX + 1];
    char fw[PLINK_FW_MAX + 1];
} plink_pair_req_t;

typedef struct {
    uint32_t attempt;
    uint8_t proto;
    uint16_t caps;
    uint8_t pk[PLINK_X25519_LEN];
    uint8_t commit[PLINK_SHA256_LEN];
    char name[PLINK_NAME_MAX + 1];
    char panel_id[PLINK_PANEL_ID_LEN + 1];
} plink_pair_commit_t;

typedef struct {
    uint32_t attempt;
    uint8_t n[PLINK_NONCE_LEN];
} plink_pair_nonce_t; /* PAIR_NONCE (n_k) and PAIR_REVEAL (n_m) */

typedef struct {
    uint32_t attempt;
    uint8_t mac[PLINK_MAC_LEN];
} plink_pair_confirm_t;

typedef struct {
    uint32_t attempt;
    uint8_t device;
    uint8_t home;
    uint8_t mac[PLINK_MAC_LEN];
} plink_pair_done_t;

typedef struct {
    uint32_t attempt;
    uint8_t reason; /* plink_fail_t */
} plink_pair_fail_t;

/* ---- handshake (plain) -------------------------------------------------------------------- */
typedef struct {
    uint8_t proto;
    uint16_t caps;
    plink_caps_t cap; /* HELLO only */
    uint8_t n[PLINK_NONCE_LEN];
    uint8_t e[PLINK_X25519_LEN];
    uint8_t mac[PLINK_MAC_LEN];
} plink_hello_t; /* HELLO and WELCOME (WELCOME has no capacities section) */

typedef struct {
    uint32_t sid;
} plink_rekey_t;

/* ---- status (sealed) ---------------------------------------------------------------------- */
#define PLINK_PF_READY         0x01u
#define PLINK_PF_CHIME         0x02u
#define PLINK_PF_WALK_TEST     0x04u
#define PLINK_PF_TRIGGERED     0x08u
#define PLINK_PF_EXIT_ERROR    0x10u
#define PLINK_PF_TROUBLE_UNACK 0x20u
#define PLINK_PF_QUICK_EXIT    0x40u /* the exit time is a quick exit, not an arming (LINK-028) */

#define PLINK_SF_PROGRAM     0x01u
#define PLINK_SF_EXIT_SOUNDS 0x02u
#define PLINK_SF_UPDATING    0x04u /* the master runs an update or is on its self-test */
#define PLINK_SF_HOLD        0x08u /* proto_fw_install_allowed() says no on the master's snapshot */
#define PLINK_SF_FIRE        0x10u /* fire sounds on any of the master's partitions */
#define PLINK_SF_CO          0x20u /* CO sounds on any of the master's partitions */

/* STATUS report byte: the master's reporting (UI-029's words). */
#define PLINK_REPORT_NONE    0u
#define PLINK_REPORT_OFFLINE 1u
#define PLINK_REPORT_ONLINE  2u

/* One partition record, as on the wire (docs/features/11 "STATUS"). Durations are worked out
 * when the frame is sealed; walk_id is the master's raw walk start (an identity, 0 = none). */
typedef struct {
    uint8_t number; /* 1-based */
    uint8_t state, level, arm_flags, armed_by;
    uint8_t arm_src_kind;
    uint16_t arm_src_id;
    uint8_t ready_levels;
    uint8_t flags; /* PLINK_PF_* */
    uint8_t sounder, bell;
    uint32_t delay_left_ms;
    uint16_t memory, faults, bypassed, alarms, troubles;
    uint32_t walk_left_ms, walk_age_ms, walk_id, walk_seq;
    uint16_t walk_tested, walk_total;
    uint8_t walk_flags, walk_started_by, walk_ended_by, walk_end_how;
    uint8_t walk_src_kind;
    uint16_t walk_src_id;
} plink_part_t;

typedef struct {
    int64_t epoch_ms;
    uint8_t partitions; /* enabled, bit 0 = partition 1 */
    uint8_t home;
    uint8_t selected; /* optional system-section tail; 0 means home */
    uint32_t sys_troubles, sys_restored, dev_troubles;
    uint8_t default_codes;
    uint8_t flags; /* PLINK_SF_* */
    uint32_t recovery_left_ms;
    uint32_t lockout_left_ms;
    uint8_t report; /* PLINK_REPORT_* */
    uint16_t zones_used;
    uint32_t last_event_seq;
    uint32_t cfg_crc, cfg_len;
    uint32_t zones_crc;
    uint64_t mono_ms; /* the master's engine time when the frame was sealed (0: not sent) */
    uint8_t count; /* records in part[] */
    plink_part_t part[PLINK_PARTS_MAX];
    bool switch_allowed;
    bool return_never;
    uint16_t return_s; /* 0 = default 30 seconds */
} plink_status_t;

typedef struct {
    uint32_t zones_crc;
    uint16_t first;
    uint16_t count;
    uint8_t rec_len;      /* decode: as sent */
    const uint8_t *recs;  /* decode: points into the payload; encode: unused */
} plink_zones_t;

typedef struct {
    uint16_t first;
    uint16_t count;
} plink_zones_req_t;

typedef struct {
    uint32_t cfg_crc;
    uint32_t offset;
    uint16_t max;
} plink_cfg_req_t;

typedef struct {
    uint32_t cfg_crc;
    uint32_t total;
    uint32_t offset;
    const uint8_t *data; /* points into the payload (decode) or the caller's blob (encode) */
    uint16_t len;        /* the page's bytes; offset + len <= total (both codecs refuse others) */
} plink_cfg_page_t;

typedef struct {
    uint32_t after_seq;
    uint8_t max;
} plink_ev_req_t;

#define PLINK_EVF_GAP 0x01u /* EVENTS flags: older events after after_seq were left out */

/* ---- commands (sealed) -------------------------------------------------------------------- */
typedef enum {
    PLINK_OP_COMMAND = 1,     /* an alarm_cmd_t */
    PLINK_OP_AUTH = 2,        /* check a code (ui_backend_t.authenticate) */
    PLINK_OP_CHANGE_CODE = 3, /* ui_backend_t.change_code: code = current, new_code */
    PLINK_OP_GOTO = 4,        /* select a view under installer policy, without a code */
    PLINK_OP_ACTIVITY = 5, /* real interaction on the selected partition; no alarm action */
} plink_op_t;

typedef struct {
    uint32_t req;
    uint8_t op; /* plink_op_t */
    alarm_cmd_t cmd; /* COMMAND: everything; AUTH: partition; CHANGE_CODE: partition. The code is
                      * cmd.code for every op. */
    char new_code[ALARM_CODE_MAX_LEN + 1];
} plink_cmd_t;

typedef struct {
    uint32_t req;
    uint8_t result; /* alarm_result_t */
    uint8_t user;
    uint16_t not_ready_zone;
    uint8_t may_disarm;
} plink_result_t;

typedef struct {
    uint8_t partition;
    uint8_t sound; /* alarm_sound_t */
    uint16_t zone;
} plink_sound_t;

#define PLINK_HB_MAINS_LOST    0x01u
#define PLINK_HB_BATTERY_LOW   0x02u
#define PLINK_HB_BATTERY_THERE 0x04u
#define PLINK_STATUS_AGE_NONE  0xFFFFu

typedef struct {
    uint8_t flags; /* PLINK_HB_* */
    int8_t rssi;
    uint32_t uptime_s;
    uint16_t status_age_s; /* PLINK_STATUS_AGE_NONE before the first status */
    char fw[PLINK_FW_MAX + 1];
    /* The appended section (a session with PLINK_CAP_PREFS): encoded only when has_prefs, read
     * only when bytes follow fw, so v1's heartbeat without it decodes as it always did; has_prefs
     * only when the section holds prefs_crc whole (an empty one carries none). */
    bool has_prefs;
    uint32_t prefs_crc; /* proto_prefs_crc of the keypad's PREFS fields body */
} plink_heartbeat_t;

typedef struct {
    int64_t epoch_ms; /* 0 = not set */
    char tz[PLINK_TZ_MAX + 1];
} plink_time_t;

typedef struct {
    uint8_t reason; /* 1 = removed at the master */
} plink_bye_t;

/* ---- updates through the link (sealed, PLINK_CAP_FW; docs/features/11 "Updates through the
 * link") ---------------------------------------------------------------------------------------
 * Every value below is on the wire: never renumbered, only appended. */
#define PLINK_ID8_LEN        8u  /* an image's id: the first 8 bytes of its SHA-256 */
#define PLINK_KEY8_LEN       8u  /* a signing key's short id */
#define PLINK_FW_KEY_LEN     32u /* FW_ORDER's key */
#define PLINK_FW_VERSION_MAX 32u /* versions in FW_INFO, FW_ORDER, FW_STATE */
#define PLINK_FW_DEVICE_MAX  31u
#define PLINK_FW_BOARD_MAX   15u
#define PLINK_FW_CHIP_MAX    3u
#define PLINK_FW_PROJECT_MAX 32u
#define PLINK_FW_CP_MAX      23u /* the co-processor's version */
#define PLINK_FW_SERVER_MAX  95u
#define PLINK_FW_CHANNEL_MAX 7u
#define PLINK_FW_PAGE_HDR    18u /* FW_PAGE's bytes before the data: id8, total, offset, len */
/* The longest payloads (all within PLINK_PAYLOAD_SMALL, LINK-043). */
#define PLINK_FW_INFO_MAX    166u
#define PLINK_FW_ORDER_MAX   219u /* a later field goes in a new section, sent only where it fits */
#define PLINK_FW_STATE_MAX   144u
#define PLINK_FW_REQ_LEN     16u

/* FW_INFO can. */
#define PLINK_FWI_FETCH          0x01u /* it fetches from an update server itself */
#define PLINK_FWI_RELAY          0x02u /* it takes an image through the link */
#define PLINK_FWI_KEY_ACCEPT     0x04u /* it can accept a new signing key (the key screen) */
#define PLINK_FWI_CFG_UNREADABLE 0x08u /* it can't read the master's configuration */

typedef enum {
    PLINK_FW_OP_INFO = 1,
    PLINK_FW_OP_PROBE = 2,
    PLINK_FW_OP_FETCH = 3,
    PLINK_FW_OP_RELAY = 4,
    PLINK_FW_OP_INSTALL = 5,
    PLINK_FW_OP_CANCEL = 6,
} plink_fw_op_t;

typedef enum {
    PLINK_FW_KIND_APP = 0,    /* the P4 application */
    PLINK_FW_KIND_C6 = 1,     /* reserved: v1 refuses it */
    PLINK_FW_KIND_LANG = 2,   /* reserved: a language pack */
    PLINK_FW_KIND_ASSETS = 3, /* reserved: an asset pack */
} plink_fw_kind_t;

/* FW_STATE state: where the keypad's run is. */
typedef enum {
    PLINK_FWS_IDLE = 0,
    PLINK_FWS_PROBING = 1,
    PLINK_FWS_FETCHING = 2,
    PLINK_FWS_RECEIVING = 3,
    PLINK_FWS_KEY_WAIT = 4,
    PLINK_FWS_VERIFYING = 5,
    PLINK_FWS_READY = 6,
    PLINK_FWS_INSTALLING = 7,
    PLINK_FWS_FAILED = 8,
    PLINK_FWS_STOPPED = 9,
} plink_fw_run_state_t;

#define PLINK_FWF_PAUSED 0x01u /* FW_STATE flags */

/* FW_STATE last: the PANEL_OTA_LAST_* values. */
typedef enum {
    PLINK_FW_LAST_NONE = 0,
    PLINK_FW_LAST_UPDATED = 1,
    PLINK_FW_LAST_ROLLED_BACK = 2,
    PLINK_FW_LAST_STOPPED = 3,
    PLINK_FW_LAST_FAILED = 4,
} plink_fw_last_t;

/* Why, grouped by decade (plink_fw_why_class): a reader takes a value it doesn't know by its
 * decade, and an unknown value in 10-19 blocks. */
typedef enum {
    PLINK_FW_WHY_OK = 0,
    /* 1-9: the server wasn't reached */
    PLINK_FW_WHY_NO_NETWORK = 1,
    PLINK_FW_WHY_NO_LOOKUP = 2,
    PLINK_FW_WHY_NO_CONNECT = 3,
    PLINK_FW_WHY_SERVER_ERROR = 4,
    PLINK_FW_WHY_NOT_A_SERVER = 5,
    /* 10-19: the server's answer */
    PLINK_FW_WHY_UNKNOWN_DEVICE = 10,
    PLINK_FW_WHY_HELD_BACK = 11,
    PLINK_FW_WHY_STEPPING_STONE = 12,
    PLINK_FW_WHY_OTHER_VERSION = 13,
    PLINK_FW_WHY_UP_TO_DATE = 14,
    /* The update server's answers to a check with want (FWS-028): no release of that version for
     * this panel (a build of the owner's own: the master relays its own slot), or not offered to
     * it. */
    PLINK_FW_WHY_UNKNOWN_VERSION = 15,
    PLINK_FW_WHY_NOT_OFFERED = 16,
    /* 20-29: the transfer (tried again, or the relay instead) */
    PLINK_FW_WHY_BROKE_OFF = 20,
    PLINK_FW_WHY_TOO_SLOW = 21,
    PLINK_FW_WHY_BAD_SIZE = 22,
    PLINK_FW_WHY_BAD_SHA = 23,
    PLINK_FW_WHY_STALLED = 24,
    PLINK_FW_WHY_TIMEOUT = 25,
    PLINK_FW_WHY_NO_MEMORY = 26,
    PLINK_FW_WHY_FLASH = 27,
    /* 30-49: refused, never tried again */
    PLINK_FW_WHY_KEY = 30,
    PLINK_FW_WHY_CHIP = 31,
    PLINK_FW_WHY_PROJECT = 32,
    PLINK_FW_WHY_NOT_NEWER = 33,
    PLINK_FW_WHY_SIGNATURE = 34,
    PLINK_FW_WHY_TOO_BIG = 35,
    PLINK_FW_WHY_UNKNOWN_KIND = 36,
    PLINK_FW_WHY_UNKNOWN_OP = 37,
    PLINK_FW_WHY_BUDGET = 38,
    PLINK_FW_WHY_BAD_ORDER = 40,
    PLINK_FW_WHY_WRONG_VERSION = 41,
    /* 50-59: waiting */
    PLINK_FW_WHY_MASTER_BUSY = 50,
    PLINK_FW_WHY_NOT_QUIET = 51,
    PLINK_FW_WHY_PANIC_PENDING = 52,
    PLINK_FW_WHY_KEPT_TONE = 53,
    PLINK_FW_WHY_NO_STATUS = 54,
    PLINK_FW_WHY_BUSY = 55,
    /* 60-69: stopped */
    PLINK_FW_WHY_STOPPED = 60,   /* a person at the keypad */
    PLINK_FW_WHY_CANCELLED = 61, /* the master */
} plink_fw_why_t;

/* A why's class, by its decade (local: never on the wire). */
typedef enum {
    PLINK_FWC_OK = 0,      /* 0 */
    PLINK_FWC_NOT_REACHED, /* 1-9 */
    PLINK_FWC_SERVER,      /* 10-19 */
    PLINK_FWC_TRANSFER,    /* 20-29 */
    PLINK_FWC_REFUSED,     /* 30-49 */
    PLINK_FWC_WAITING,     /* 50-59 */
    PLINK_FWC_STOPPED,     /* 60-69 */
    PLINK_FWC_UNKNOWN,     /* 70 and up: a class this firmware doesn't know */
} plink_fw_why_class_t;

plink_fw_why_class_t plink_fw_why_class(uint8_t why);

/* FW_INFO: one section with every field. */
typedef struct {
    uint8_t can; /* PLINK_FWI_* */
    char device[PLINK_FW_DEVICE_MAX + 1];
    char board_rev[PLINK_FW_BOARD_MAX + 1];
    char chip_rev[PLINK_FW_CHIP_MAX + 1];
    uint16_t chip_rev_full;
    char project[PLINK_FW_PROJECT_MAX + 1];
    char version[PLINK_FW_VERSION_MAX + 1];
    uint32_t slot;
    uint8_t own_key8[PLINK_KEY8_LEN];
    uint8_t accepted_key8[PLINK_KEY8_LEN]; /* zeros: none */
    char cp_version[PLINK_FW_CP_MAX + 1];
} plink_fw_info_t;

/* FW_ORDER: the order section, the fetch section, the relay section. */
typedef struct {
    uint32_t order; /* never 0 (both codecs refuse it) */
    uint8_t op;     /* plink_fw_op_t */
    uint8_t kind;   /* plink_fw_kind_t */
    char version[PLINK_FW_VERSION_MAX + 1];
    uint32_t size;
    uint8_t sha256[PLINK_SHA256_LEN];
    uint8_t key[PLINK_FW_KEY_LEN]; /* zeros: not known */
    char server[PLINK_FW_SERVER_MAX + 1];
    char channel[PLINK_FW_CHANNEL_MAX + 1];
    uint16_t page;
    uint8_t count;
    uint16_t gap_ms;
} plink_fw_order_t;

/* FW_STATE: the run section, the last section, the probe section. */
typedef struct {
    uint32_t order;
    uint8_t op;    /* plink_fw_op_t */
    uint8_t state; /* plink_fw_run_state_t */
    uint8_t flags; /* PLINK_FWF_* */
    uint8_t why;   /* plink_fw_why_t */
    uint32_t bytes, size;
    uint8_t id8[PLINK_ID8_LEN];
    char target[PLINK_FW_VERSION_MAX + 1];
    uint8_t probation;
    uint8_t last; /* plink_fw_last_t */
    char last_version[PLINK_FW_VERSION_MAX + 1];
    uint8_t failed_mask;
    uint8_t asked;
    uint8_t probe_why; /* plink_fw_why_t */
    uint32_t age_s;
    char offered[PLINK_FW_VERSION_MAX + 1];
    uint8_t offered_key8[PLINK_KEY8_LEN];
    uint8_t key_trusted;
} plink_fw_state_t;

/* FW_REQ: one section with every field. */
typedef struct {
    uint8_t id8[PLINK_ID8_LEN];
    uint32_t offset;
    uint16_t page;
    uint8_t count;
} plink_fw_req_t;

/* FW_PAGE: fixed, like CFG_PAGE. */
typedef struct {
    uint8_t id8[PLINK_ID8_LEN];
    uint32_t total;
    uint32_t offset;
    const uint8_t *data; /* points into the payload (decode) or the caller's image (encode) */
    uint16_t len;        /* offset + len <= total (both codecs refuse others) */
} plink_fw_page_t;

/* ---- Smart Home on keypads (sealed, PLINK_CAP_HOME; docs/features/11 "Smart Home and Home
 * Assistant on keypads") ----------------------------------------------------------------------
 * Every value below is on the wire: never renumbered, only appended. */
#define PLINK_DEV_MAX          64u  /* devices v1 carries (indexes 0..63): more needs a new bit */
#define PLINK_REF_MAX          95u  /* a device's ref (its source's address), bytes */
#define PLINK_AREA_MAX         47u
#define PLINK_DEV_WORD_MAX     23u  /* a device's state word */
#define PLINK_DEV_CAT_COMPAT   1u   /* the device list blob's compat byte */
#define PLINK_DEV_CAT_MAX      195u /* a catalog record section at its longest, length byte too */
#define PLINK_DEV_REC_MAX      44u  /* a record section at its longest, length byte too */
#define PLINK_DEV_ITEM_MAX     45u  /* ... with its index: one record in DEV_STATE */
#define PLINK_DEV_STATE_HEAD   10u  /* DEV_STATE's head section, length byte too */
#define PLINK_DEV_STATE_REQ_LEN 6u
#define PLINK_DEV_ACT_LEN      119u /* every DEV_ACT, whatever its ref and code */
#define PLINK_DEV_ACK_MAX      56u

/* A record's flags. */
#define PLINK_DEVF_ONLINE  0x01u
#define PLINK_DEVF_PENDING 0x02u
#define PLINK_DEVF_FAILED  0x04u
#define PLINK_DEVF_UNSURE  0x08u /* hc_outcome_unknown: it may or may not have happened */

/* DEV_ACT mode for ACT_SET_MODE: one of these bits (device_source.h's DEV_MODE_* numbers, asserted
 * in test_plink_msg.c); the master refuses any other value. */
#define PLINK_DEV_MODE_OFF  0x01u
#define PLINK_DEV_MODE_HEAT 0x02u
#define PLINK_DEV_MODE_COOL 0x04u
#define PLINK_DEV_MODE_AUTO 0x08u

/* DEV_ACK result: 0..5 are ui_dev_result_t's numbers (asserted in test_plink_msg.c); an unknown
 * value reads as REFUSED. */
typedef enum {
    PLINK_DEV_OK = 0,
    PLINK_DEV_NEED_CODE = 1,
    PLINK_DEV_CODE_REFUSED = 2, /* why: the alarm_result_t */
    PLINK_DEV_BUSY = 3,
    PLINK_DEV_UNREACHABLE = 4,
    PLINK_DEV_REFUSED = 5,
    PLINK_DEV_UNAVAILABLE = 6, /* this master serves no devices */
} plink_dev_result_t;

/* A catalog record section: one device in the list blob (compat u8 = PLINK_DEV_CAT_COMPAT, count
 * u8, then one record per device in the registry's order, paged as DEV_LIST_PAGE). Later fields
 * are appended inside the section. */
typedef struct {
    uint8_t kind;   /* dev_kind_t */
    uint8_t source; /* dsrc_kind_t */
    char ref[PLINK_REF_MAX + 1];
    char name[PLINK_NAME_MAX + 1];
    char area[PLINK_AREA_MAX + 1];
} plink_dev_cat_t;

/* A device's record section: the registry's dev_values_t with its flags, action and state word
 * (DEV_STATE, DEV_ACK). */
typedef struct {
    uint8_t flags;       /* PLINK_DEVF_* */
    uint8_t action;      /* dev_action_t: the one under way, or the last one when it failed */
    uint8_t vflags;      /* DEV_VAL_* */
    uint8_t level;
    uint8_t modes;       /* DEV_MODE_* */
    uint8_t hvac_action; /* dev_hvac_t */
    int32_t setpoint;    /* hundredths of a degree */
    int32_t temp;
    uint32_t since;      /* Unix seconds */
    uint8_t caps;        /* DEV_CAP_* */
    char word[PLINK_DEV_WORD_MAX + 1];
} plink_dev_rec_t;

/* DEV_LIST_REQ and DEV_LIST_PAGE are CFG_REQ's and CFG_PAGE's layouts (cfg_crc carries list_crc);
 * their codecs below are CFG_REQ's and CFG_PAGE's. */
typedef plink_cfg_req_t plink_dev_list_req_t;
typedef plink_cfg_page_t plink_dev_list_page_t;

/* DEV_STATE: the head section, then count records as sent, each its index u8 and its record
 * section. The master encodes a record once (plink_enc_dev_item) and DEV_STATE copies the bytes;
 * the keypad keeps them as they came, so its states_crc is exact even for fields a newer master
 * appended. count 0 is the summary. */
typedef struct {
    uint32_t list_crc;
    uint32_t states_crc;
    uint8_t count;        /* records in items */
    const uint8_t *items; /* the records: decode, into the payload; encode, the caller's bytes */
    size_t items_len;
} plink_dev_state_t;

typedef struct {
    uint32_t list_crc;
    uint8_t first;
    uint8_t count; /* 0 = all */
} plink_dev_state_req_t;

/* DEV_ACT: the ref and the code travel zero-padded, so every DEV_ACT is PLINK_DEV_ACT_LEN bytes.
 * Both codecs refuse a ref longer than PLINK_REF_MAX or one with a byte plink_clean_text would
 * drop (cut or cleaned, it could name another device), and a code that isn't digits only. */
typedef struct {
    uint32_t req;
    uint8_t action; /* dev_action_t */
    int32_t value;
    uint8_t mode;   /* ACT_SET_MODE: one PLINK_DEV_MODE_* bit */
    uint8_t flags;  /* 0 in v1 */
    char ref[PLINK_REF_MAX + 1];
    char code[ALARM_CODE_MAX_LEN + 1];
} plink_dev_act_t;

/* DEV_ACK: the optional section carries the device's record now. Empty, it carries none (so a
 * later section can follow an answer without a record); not empty, it holds list_crc, the index
 * and a whole record section, or the payload is refused. */
typedef struct {
    uint32_t req;
    uint8_t result; /* plink_dev_result_t */
    uint8_t why;    /* CODE_REFUSED: the alarm_result_t */
    bool has_rec;
    uint32_t list_crc;
    uint8_t index;
    plink_dev_rec_t rec;
    const uint8_t *rec_raw; /* decode: the record section as sent, its length byte first */
    size_t rec_raw_len;
} plink_dev_ack_t;

/* ---- a keypad's settings (sealed, PLINK_CAP_PREFS) --------------------------------------------
 * A fields section's body is proto_prefs.h's, carried as one span: count u8, then count TLVs (id
 * u8, len u8, the value little-endian), then whatever a later version appends inside the section.
 * Here it is opaque bytes, checked only for that framing; proto_prefs_encode writes it,
 * proto_prefs_decode reads it, and prefs_crc is proto_prefs_crc over exactly these bytes (the
 * section's length byte left out), so a master's CRC over a body it received is the keypad's. */
#define PLINK_PREFS_MAX     128u /* PREFS with every v1 field and every one refused */
#define PLINK_PREFS_SET_MAX 75u  /* PREFS_SET with every v1 field */
#define PLINK_PREFS_BODY_MAX     255u /* a fields section's body: its length is one byte */
#define PLINK_PREFS_SET_BODY_MAX (PLINK_PREFS_SET_MAX - 6u) /* 69 B: every field v1 knows */

/* Why a field was refused: proto_prefs.h's PROTO_PREFS_WHY_* (asserted in plink_msg.c). An
 * unknown value reads as NOT_ALLOWED. */
typedef enum {
    PLINK_PREFS_NOT_ALLOWED = 1,
    PLINK_PREFS_RANGE = 2,
    PLINK_PREFS_IN_USE = 3,
    PLINK_PREFS_HELD = 4,
    PLINK_PREFS_VOICE_NOT_READY = 5,
    PLINK_PREFS_BAD = 6,
    PLINK_PREFS_BUSY = 7,
} plink_prefs_why_t;

/* PREFS voice: the keypad's voice. */
typedef enum {
    PLINK_VOICE_UNKNOWN = 0,
    PLINK_VOICE_READY = 1,
    PLINK_VOICE_LOADING = 2,
    PLINK_VOICE_NONE = 3,
} plink_voice_t;

/* PREFS_SET origin: who asked. An unknown value means elsewhere. */
typedef enum {
    PLINK_ORIGIN_HA = 1,      /* Home Assistant */
    PLINK_ORIGIN_CONSOLE = 2, /* the master's console */
} plink_origin_t;

/* PREFS: the answer section, the info section, the refusals section, the fields section. */
typedef struct {
    uint32_t req; /* the PREFS_SET this answers, 0 = unsolicited */
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    uint8_t voice;    /* plink_voice_t */
    uint32_t accepts; /* bit id-1: a field the keypad takes */
    uint8_t refused_count;
    const uint8_t *refused; /* refused_count x (id u8, why u8): decode, into the payload */
    uint8_t field_count;    /* decode: the body's count (0 for an empty body); encode: not read */
    const uint8_t *fields;  /* the fields section's body, count first: decode, into the payload */
    size_t fields_len;      /* the body's bytes, 0 for an empty section */
} plink_prefs_t;

typedef struct {
    uint32_t have_crc; /* the prefs_crc of the report the master holds */
} plink_prefs_req_t;

/* PREFS_SET: req and origin, then the fields section (the fields to change). */
typedef struct {
    uint32_t req;          /* never 0 (both codecs refuse it) */
    uint8_t origin;        /* plink_origin_t */
    uint8_t field_count;   /* decode: the body's count (0 for an empty body); encode: not read */
    const uint8_t *fields; /* the fields section's body, count first, as PREFS' */
    size_t fields_len;
} plink_prefs_set_t;

/* ---- codecs ------------------------------------------------------------------------------- */
bool plink_enc_pair_req(const plink_pair_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_pair_req(const uint8_t *p, size_t len, plink_pair_req_t *m);
bool plink_enc_pair_commit(const plink_pair_commit_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_pair_commit(const uint8_t *p, size_t len, plink_pair_commit_t *m);
bool plink_enc_pair_nonce(const plink_pair_nonce_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_pair_nonce(const uint8_t *p, size_t len, plink_pair_nonce_t *m);
bool plink_enc_pair_confirm(const plink_pair_confirm_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_pair_confirm(const uint8_t *p, size_t len, plink_pair_confirm_t *m);
bool plink_enc_pair_done(const plink_pair_done_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_pair_done(const uint8_t *p, size_t len, plink_pair_done_t *m);
bool plink_enc_pair_fail(const plink_pair_fail_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_pair_fail(const uint8_t *p, size_t len, plink_pair_fail_t *m);
/* hello = true: HELLO (with the capacities section); false: WELCOME. */
bool plink_enc_hello(const plink_hello_t *m, bool hello, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_hello(const uint8_t *p, size_t len, bool hello, plink_hello_t *m);
bool plink_enc_rekey(const plink_rekey_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_rekey(const uint8_t *p, size_t len, plink_rekey_t *m);

/* STATUS: the system section and as many records, from first_part on, as fit cap; *written is
 * how many records went in (a smaller transport sends the rest in another frame). false only when
 * not even the system section and one record fit. */
bool plink_enc_status(const plink_status_t *m, uint8_t first_part, uint8_t *out, size_t cap,
                      size_t *len, uint8_t *written);
bool plink_dec_status(const uint8_t *p, size_t len, plink_status_t *m);

/* ZONES: zones first..first+count-1 from z (z[0] is zone first) as fit cap; *written is how many. */
bool plink_enc_zones(uint32_t zones_crc, uint16_t first, const alarm_zone_status_t *z,
                     uint16_t count, uint8_t *out, size_t cap, size_t *len, uint16_t *written);
/* Refuses records for zone 0 or past zone 65535 (both codecs), and fewer records than count. */
bool plink_dec_zones(const uint8_t *p, size_t len, plink_zones_t *m);
/* Record i of a decoded ZONES (i < m->count) into out. */
void plink_zones_rec(const plink_zones_t *m, uint16_t i, alarm_zone_status_t *out);
/* The CRC-32 of zones 1..zones_used encoded as zone records (STATUS zones_crc). */
uint32_t plink_zones_crc(const alarm_zone_status_t *zone, uint16_t zones_used);

bool plink_enc_zones_req(const plink_zones_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_zones_req(const uint8_t *p, size_t len, plink_zones_req_t *m);
bool plink_enc_cfg_req(const plink_cfg_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_cfg_req(const uint8_t *p, size_t len, plink_cfg_req_t *m);
bool plink_enc_cfg_page(const plink_cfg_page_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_cfg_page(const uint8_t *p, size_t len, plink_cfg_page_t *m);
bool plink_enc_ev_req(const plink_ev_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_ev_req(const uint8_t *p, size_t len, plink_ev_req_t *m);

/* EVENTS: ev[0..n-1] oldest first, as many as fit cap (the newest kept: the oldest are dropped
 * and the gap flag set); *written is how many. */
bool plink_enc_events(const alarm_event_t *ev, size_t n, uint8_t flags, uint8_t *out, size_t cap,
                      size_t *len, size_t *written);
/* Decodes up to max events into out, oldest first (the newest max when there are more: the gap
 * flag is then set in *flags); *n how many, *flags the flags byte. */
bool plink_dec_events(const uint8_t *p, size_t len, alarm_event_t *out, size_t max, size_t *n,
                      uint8_t *flags);

/* CMD: codes must be digits only, at most ALARM_CODE_MAX_LEN (the encoder refuses others, the
 * decoder too). The codes travel zero-padded to the same length. */
bool plink_enc_cmd(const plink_cmd_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_cmd(const uint8_t *p, size_t len, plink_cmd_t *m);
/* The digest the master's result cache keeps (LINK-018): SHA-256 over op and the op section,
 * never the codes. */
void plink_cmd_digest(const plink_cmd_t *m, uint8_t out[PLINK_SHA256_LEN]);

bool plink_enc_result(const plink_result_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_result(const uint8_t *p, size_t len, plink_result_t *m);
bool plink_enc_sound(const plink_sound_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_sound(const uint8_t *p, size_t len, plink_sound_t *m);
/* HEARTBEAT: the section only when m->has_prefs; read only when bytes follow fw (a section too
 * short for prefs_crc reads as none). */
bool plink_enc_heartbeat(const plink_heartbeat_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_heartbeat(const uint8_t *p, size_t len, plink_heartbeat_t *m);
bool plink_enc_time(const plink_time_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_time(const uint8_t *p, size_t len, plink_time_t *m);
bool plink_enc_bye(const plink_bye_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_bye(const uint8_t *p, size_t len, plink_bye_t *m);

/* Updates through the link. A decoder needs every section v1 has; a section reads as far as this
 * firmware knows (LINK-017). */
bool plink_enc_fw_info(const plink_fw_info_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_fw_info(const uint8_t *p, size_t len, plink_fw_info_t *m);
bool plink_enc_fw_order(const plink_fw_order_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_fw_order(const uint8_t *p, size_t len, plink_fw_order_t *m);
bool plink_enc_fw_state(const plink_fw_state_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_fw_state(const uint8_t *p, size_t len, plink_fw_state_t *m);
bool plink_enc_fw_req(const plink_fw_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_fw_req(const uint8_t *p, size_t len, plink_fw_req_t *m);
/* FW_PAGE: bytes after the data are ignored. */
bool plink_enc_fw_page(const plink_fw_page_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_fw_page(const uint8_t *p, size_t len, plink_fw_page_t *m);

/* Smart Home on keypads. DEV_LIST_REQ and DEV_LIST_PAGE: CFG_REQ's and CFG_PAGE's codecs. */
bool plink_enc_dev_list_req(const plink_dev_list_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_dev_list_req(const uint8_t *p, size_t len, plink_dev_list_req_t *m);
bool plink_enc_dev_list_page(const plink_dev_list_page_t *m, uint8_t *out, size_t cap,
                             size_t *len);
bool plink_dec_dev_list_page(const uint8_t *p, size_t len, plink_dev_list_page_t *m);
/* DEV_STATE: the encoder copies m->items, which must be exactly m->count records; the decoder
 * refuses fewer records than count and ignores bytes after them. */
bool plink_enc_dev_state(const plink_dev_state_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_dev_state(const uint8_t *p, size_t len, plink_dev_state_t *m);
/* The next record of a decoded DEV_STATE (*pos from 0): its index, its fields and its bytes as
 * sent (*item: the index and the record section; any output may be NULL). false: no more. */
bool plink_dev_state_next(const plink_dev_state_t *m, size_t *pos, uint8_t *index,
                          plink_dev_rec_t *rec, const uint8_t **item, size_t *item_len);
bool plink_enc_dev_state_req(const plink_dev_state_req_t *m, uint8_t *out, size_t cap,
                             size_t *len);
bool plink_dec_dev_state_req(const uint8_t *p, size_t len, plink_dev_state_req_t *m);
bool plink_enc_dev_act(const plink_dev_act_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_dev_act(const uint8_t *p, size_t len, plink_dev_act_t *m);
/* The digest the master's answer cache keeps for a DEV_ACT (LINK-047): SHA-256 over the act
 * section and the padded ref as they go on the wire, never the code nor the request id. */
void plink_dev_act_digest(const plink_dev_act_t *m, uint8_t out[PLINK_SHA256_LEN]);
/* DEV_ACK: the optional section only when m->has_rec; read only when bytes follow why, and an
 * empty one reads as no record. */
bool plink_enc_dev_ack(const plink_dev_ack_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_dev_ack(const uint8_t *p, size_t len, plink_dev_ack_t *m);

/* A record section, its length byte first. The decoder reads the section at p (the bytes after
 * it are not read): appended fields skipped, missing ones 0. *used (may be NULL): the section's
 * bytes when it is whole within len, 0 otherwise. false: it isn't whole. */
bool plink_enc_dev_rec(const plink_dev_rec_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_dev_rec(const uint8_t *p, size_t len, plink_dev_rec_t *m, size_t *used);
/* One record as DEV_STATE carries it: index, then the record section (PLINK_DEV_ITEM_MAX at
 * most). */
bool plink_enc_dev_item(uint8_t index, const plink_dev_rec_t *m, uint8_t *out, size_t cap,
                        size_t *len);
/* A catalog record section, as above. The encoder refuses a ref the DEV_ACT codecs would refuse
 * and cuts a name or an area on a whole character; the decoder returns false for such a ref
 * (with *used still set when the section is whole, so a list can skip that device). */
bool plink_enc_dev_cat(const plink_dev_cat_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_dev_cat(const uint8_t *p, size_t len, plink_dev_cat_t *m, size_t *used);

/* A keypad's settings. A fields body is empty, or its count and that many whole TLVs, then any
 * bytes a later version appended (kept: they are in prefs_crc); both codecs refuse a TLV that
 * runs past the body, and the encoder a body over 255 bytes. PREFS's refusals: refused_count
 * pairs within their section (bytes after them skipped). */
bool plink_enc_prefs(const plink_prefs_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_prefs(const uint8_t *p, size_t len, plink_prefs_t *m);
bool plink_enc_prefs_req(const plink_prefs_req_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_prefs_req(const uint8_t *p, size_t len, plink_prefs_req_t *m);
bool plink_enc_prefs_set(const plink_prefs_set_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_prefs_set(const uint8_t *p, size_t len, plink_prefs_set_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_MSG_H */
