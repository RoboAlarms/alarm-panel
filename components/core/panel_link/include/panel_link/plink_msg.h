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

#define PLINK_PROTO 1u     /* the protocol version this firmware speaks */
#define PLINK_CAPS  0x0000u /* capability bits this firmware has (none defined in v1) */

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

#define PLINK_SF_PROGRAM     0x01u
#define PLINK_SF_EXIT_SOUNDS 0x02u

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
    uint8_t count; /* records in part[] */
    plink_part_t part[PLINK_PARTS_MAX];
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
} plink_heartbeat_t;

typedef struct {
    int64_t epoch_ms; /* 0 = not set */
    char tz[PLINK_TZ_MAX + 1];
} plink_time_t;

typedef struct {
    uint8_t reason; /* 1 = removed at the master */
} plink_bye_t;

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
bool plink_enc_heartbeat(const plink_heartbeat_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_heartbeat(const uint8_t *p, size_t len, plink_heartbeat_t *m);
bool plink_enc_time(const plink_time_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_time(const uint8_t *p, size_t len, plink_time_t *m);
bool plink_enc_bye(const plink_bye_t *m, uint8_t *out, size_t cap, size_t *len);
bool plink_dec_bye(const uint8_t *p, size_t len, plink_bye_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_MSG_H */
