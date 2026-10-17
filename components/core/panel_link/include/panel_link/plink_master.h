/*
 * plink_master.h - the master's side of the panel link (docs/features/11; LINK-001, -006, -007,
 * -014, -015, -016, -018, -021, -026, -027): its keypads, their sessions, pairing, status to each
 * keypad, commands run for them, supervision.
 *
 * Portable and single-threaded: one task owns a plink_master_t and calls everything here; time
 * comes in as ms on the same clock as the snapshots' taken_ms (the engine's). Frames go out and
 * work goes to the platform through the port; nothing here blocks or waits.
 *
 * Commands (LINK-018, LINK-027): a CMD from a keypad's live session is checked (op, fields, a
 * partition other than the keypad's home is answered NOT_AUTHORIZED, rate limits), looked up in
 * the result cache keyed on (keypad, req) with a digest of the command, and otherwise handed to
 * port.run with a ticket; the answer comes back through plink_master_result. A retry while it
 * runs waits for that answer; a retry after it is answered from the cache for 90 s after its last
 * copy; the same req with another digest runs as a new command. Of a keypad's 8 answers, panics
 * never hold the last 2 (PLINK_PANIC_FREE_SLOTS), so commands after a run of panics still run.
 *
 * Events: each is pushed as it comes; a STATUS never says less than what was pushed; EV_REQ is
 * answered with up to PLINK_EV_ANSWER_MAX events, in as many frames as it takes.
 *
 * Supervision (LINK-007): level-based. A keypad is down when nothing authenticated came from it
 * for its supervision time, or it reported a status older than 30 s for that long; port.reachable
 * is called on every change, true before a removed keypad's number is freed. Paused while the
 * master's own network is down (plink_master_net), resumed a period after it is back. A
 * provisional keypad (paired, no handshake yet) is not supervised and is dropped after 10 min.
 *
 * Smart Home and settings on keypads (LINK-043..051; docs/features/11 "Smart Home and Home
 * Assistant on keypads"): the platform hands in its registry's device list and records
 * (plink_master_home, built with plink_home.h); DEV_LIST_REQ is served from that blob as CFG_REQ
 * is; changed records go in rounds, the full set at each new session and on DEV_STATE_REQ, a
 * summary every 10 s. A DEV_ACT is answered from its own cache of 8 per keypad (90 s after the
 * last copy; an entry reused 3 s after it) or handed to port.dev_run, one at a time from its own bucket; the answer comes back
 * through plink_master_dev_answer, and every DEV_ACK carries the device's record as it is when
 * that DEV_ACK goes. A keypad's settings report (PREFS) is kept, asked for again while its
 * heartbeat's prefs_crc differs, and a write (plink_master_prefs_set) is resent until the PREFS
 * carrying its req answers it (port.prefs_done). Nothing of this shares a cache, slot, bucket or
 * frame budget with the alarm traffic: it goes only to a session with its capability bit, on a
 * transport whose frames are 250 B or more, and only in a tick where that keypad has no STATUS
 * change waiting and no RESULT due - at most 2 frames per keypad and 8 per tick.
 *
 * Updates through the link (LINK-029..037; docs/features/11 "The master"): every STATUS carries
 * UPDATING (plink_master_fw_self: the master's own update runs, or it is on its self-test) and
 * HOLD (proto_fw_install_allowed() says no on the snapshot), a change of either sending a STATUS
 * as any change does. The service's planner (plink_fw.h) hands in orders (plink_master_fw_order):
 * one outstanding per keypad - a new one supersedes it and the old one is never sent again -
 * numbered from a random base drawn at the first, sent at the next tick and again after 300 ms,
 * doubling to 2 s, until a FW_STATE echoes its id. FW_INFO and FW_STATE are kept (a newer frame
 * of the session replaces them; plink_master_fw_keypad). FW_REQ is served statelessly, as CFG_REQ
 * is, only to the keypad whose newest order but INFO is a RELAY of that id8 (an INFO naming the
 * master's version doesn't stop a relay; any other order does), while the master is quiet (HOLD
 * and UPDATING clear): from the request's offset, count pages of min(the request's page, the
 * order's, the frame's room - 18) bytes read through port.fw_read, never past the image, from a
 * byte bucket of its own (256 KB/s, 16 KB/s on frames under 1,200 B, 16 KB at once) and at most 16
 * pages a tick; anything else is dropped, the keypad's resend comes back. A keypad installing or
 * on its self-test (a FW_STATE saying INSTALLING or probation) isn't raised by supervision for up
 * to 10 min from when its FW_STATE first said so, and plink_master_fw_busy tells the service to put
 * off the master's own restart meanwhile - one such hold in each of its sessions (a restart is a
 * new one), not begun again while its FW_STATEs go on saying so, nor by a FW_STATE saying neither
 * within it; one saying neither after it, or a new INSTALL, lets the next begin. All of it only in
 * a session with PLINK_CAP_FW, on frames of 250 B or more, behind the alarm traffic as the Smart
 * Home frames are.
 */
#ifndef PLINK_MASTER_H
#define PLINK_MASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_fw.h"
#include "panel_link/plink_home.h"
#include "panel_link/plink_msg.h"
#include "panel_link/plink_pair.h"
#include "panel_link/plink_session.h"
#include "panel_link/plink_store.h"
#include "panel_link/plink_view.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_STATUS_EVERY_MS    10000u /* LINK-006 */
#define PLINK_STATUS_REPEAT1_MS  250u
#define PLINK_STATUS_REPEAT2_MS  1000u
#define PLINK_STATUS_MIN_GAP_MS  100u
#define PLINK_PENDING_MS         20000u /* a pending handshake */
#define PLINK_RESULT_KEEP_MS     90000u /* LINK-018 */
#define PLINK_RESULTS_PER_KEYPAD 8u     /* answers kept per keypad (bitmasks below: at most 8) */
#define PLINK_PROVISIONAL_MS     600000u
#define PLINK_HELLO_SEEN         4u     /* n_k remembered per keypad */
#define PLINK_X25519_PER_S       4u     /* handshakes answered per second, all keypads */
#define PLINK_TIME_EVERY_MS      600000u
#define PLINK_STATUS_STALE_S     30u    /* a keypad reporting an older status is not fresh */
#define PLINK_EV_ANSWER_MAX      50u    /* events in an EV_REQ's answer: what a keypad keeps */
#define PLINK_PANIC_FREE_SLOTS   2u     /* answers a keypad's panics never take (commands') */

/* Smart Home and settings on keypads (LINK-045, LINK-047, LINK-049..051). */
#define PLINK_DEV_RESULTS_PER_KEYPAD 8u     /* DEV_ACT answers kept per keypad (bitmasks: <= 8):
                                             * an entry is reused 3 s after its last copy, so the
                                             * 500 ms bucket, not the cache, sets the pace */
#define PLINK_DEV_PUSH_MS            500u   /* changed records: a round per keypad at most this */
#define PLINK_DEV_PUSH_SMALL_MS      1000u  /* ... on a transport whose frames are under 1,200 B */
#define PLINK_DEV_SUMMARY_MS         10000u /* a DEV_STATE at least this often (else a summary) */
#define PLINK_DEV_LIST_QUEUE         2u     /* DEV_LIST_REQs waiting for a tick, per keypad */
#define PLINK_HOME_PER_KEYPAD        2u     /* DEV_* and PREFS* frames per keypad per tick */
#define PLINK_HOME_PER_TICK          8u     /* ... and in all */
#define PLINK_HOME_HEARD_MS          30000u /* a keypad heard this recently takes a write */
#define PLINK_PREFS_REQ_GAP_MS       2000u  /* PREFS_REQ at most this often while the CRC differs */
#define PLINK_PREFS_SET_RETRY_MS     1000u
#define PLINK_PREFS_SET_GIVE_UP_MS   6000u
/* A fields body's sizes, PLINK_PREFS_BODY_MAX and PLINK_PREFS_SET_BODY_MAX: plink_msg.h. */

/* Updates through the link (LINK-030, LINK-032, LINK-035). */
#define PLINK_FW_ORDER_RETRY_MS     300u    /* an order is sent again after this, doubling ... */
#define PLINK_FW_ORDER_RETRY_MAX_MS 2000u   /* ... to this, until a FW_STATE echoes it */
#define PLINK_FW_AWAY_MS            30000u  /* unanswered (or unheard) this long: away */
#define PLINK_FW_HOLD_MS            600000u /* installing or on its self-test: not raised so long */
#define PLINK_FW_RATE               262144u /* FW_PAGE bytes a second, ... */
#define PLINK_FW_RATE_SMALL         16384u  /* ... on frames under 1,200 B, ... */
#define PLINK_FW_BURST              16384u  /* ... at most this at once */
#define PLINK_FW_PAGES_PER_TICK     16u

/* How plink_master_prefs_set went (local: never on the wire). */
typedef enum {
    PLINK_PSET_OK = 0,      /* sent; or, in port.prefs_done, answered (perhaps with refusals) */
    PLINK_PSET_NOT_FOUND,   /* no keypad has that number */
    PLINK_PSET_OFFLINE,     /* no live session, or not heard for 30 s; in prefs_done: removed */
    PLINK_PSET_UNSUPPORTED, /* its session lacks PLINK_CAP_PREFS, or its frames are under 250 B */
    PLINK_PSET_BUSY,        /* a write to it is in flight */
    PLINK_PSET_TIMEOUT,     /* in prefs_done: no answer within 6 s */
    PLINK_PSET_BAD,         /* req 0, or fields not a fields body of at most 69 B */
} plink_pset_t;

/* The end of a write (port.prefs_done). The pointers are valid during the call only. */
typedef struct {
    uint32_t req;
    uint8_t result;          /* plink_pset_t: OK, TIMEOUT or OFFLINE */
    uint8_t refused_count;   /* OK: the fields it refused */
    const uint8_t *refused;  /* refused_count x (id u8, why u8: plink_prefs_why_t) */
    const uint8_t *fields;   /* OK: the values it stores now, a fields body (count first) */
    size_t fields_len;
} plink_prefs_done_t;

/* Things worth a line in the log (port.note). */
typedef enum {
    PLINK_NOTE_PAIRED = 1,        /* device: the new keypad */
    PLINK_NOTE_PAIR_LIMIT,        /* 3 attempts used: the window closed */
    PLINK_NOTE_PAIR_OVERLAP,      /* two keypads at once */
    PLINK_NOTE_REMOVED,           /* device */
    PLINK_NOTE_CLONE,             /* device: a second address with keypad n's keys */
    PLINK_NOTE_PROVISIONAL_DROPPED, /* device: never connected after pairing */
    PLINK_NOTE_STORE_FAILED,      /* the record could not be written */
} plink_note_t;

typedef struct {
    void *ctx;
    /* One frame to a peer; false when it couldn't be queued (the protocol recovers). */
    bool (*send)(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len);
    /* Largest frame on a transport (PLINK_FRAME_MAX for UDP). NULL: PLINK_FRAME_MAX. */
    size_t (*mtu)(void *ctx, uint8_t transport);
    /* Writes the master's record now (NVS); false when it wasn't stored. */
    bool (*store)(void *ctx, const plink_master_rec_t *rec);
    /* Runs a keypad's command off this task as {ALARM_SRC_KEYPAD, device}; the answer comes back
     * through plink_master_result(ticket). The platform copies what it needs and wipes its copy
     * of the codes after use. false: no room now (the keypad's retry will come). */
    bool (*run)(void *ctx, uint32_t ticket, uint8_t device, const plink_cmd_t *cmd);
    /* Supervision edge (alarm_service_device_reachable): false raises the trouble. */
    void (*reachable)(void *ctx, uint8_t device, bool reachable);
    /* A keypad's HEARTBEAT said both directions are fresh (alarm_service_device_checkin): never
     * while supervision holds it down or its status is stale. May be NULL. */
    void (*checkin)(void *ctx, uint8_t device);
    /* Numbers 1..8 others hold: Zigbee keypads, enabled device-table entries (bit d-1). Must not
     * block. NULL: none. */
    uint32_t (*devices_taken)(void *ctx);
    /* The link's own numbers changed (bit d-1): the Zigbee side is told. May be NULL. */
    void (*held_changed)(void *ctx, uint32_t held);
    /* This keypad's lockout left at now, ms (kept by the platform after each of its commands);
     * must not block. NULL: 0. */
    uint32_t (*lockout_left)(void *ctx, uint8_t device, uint64_t now_ms);
    /* The newest events after after_seq, oldest first, at most max; *gap when older ones after
     * after_seq were left out. Must not block. */
    size_t (*events_after)(void *ctx, uint32_t after_seq, alarm_event_t *out, size_t max,
                           bool *gap);
    void (*note)(void *ctx, plink_note_t note, uint8_t device); /* may be NULL */
    /* Runs a keypad's device action off this task: the one judge of both screens as {KEYPAD,
     * device}, its code checked for the keypad's home partition (LINK-048); the answer comes back
     * through plink_master_dev_answer(ticket). The platform copies what it needs and wipes its
     * copy of the code after use. false: no room now (the keypad's resend comes back). NULL:
     * this master serves no devices (UNAVAILABLE). */
    bool (*dev_run)(void *ctx, uint32_t ticket, uint8_t device, uint8_t home,
                    const plink_dev_act_t *act);
    /* A settings write ended (plink_master_prefs_set): the keypad's answer, or none in time, or
     * the keypad removed. Called from inside plink_master_input, _tick and _remove, once the
     * master's state is settled: it may start the next write (plink_master_prefs_set) and read
     * plink_master_keypad_home (which already holds the newest report), and must call nothing
     * else here. May be NULL. */
    void (*prefs_done)(void *ctx, uint8_t device, const plink_prefs_done_t *done);
    /* Bytes [offset, offset + len) of the image whose SHA-256 starts with id8 (the running slot,
     * or a copy fetched for keypads): returns how many it read into out (fewer: the page goes
     * shorter, the keypad asks for the rest; 0: none, nothing goes). Must not block for long nor
     * call into the master. NULL: no relay is served. */
    size_t (*fw_read)(void *ctx, const uint8_t id8[PLINK_ID8_LEN], uint32_t offset, uint8_t *out,
                      size_t len);
} plink_master_port_t;

typedef struct {
    uint32_t req;
    uint8_t digest[PLINK_SHA256_LEN];
    bool pending;
    uint32_t ticket;
    plink_result_t res;
    uint64_t last_ms; /* the last copy of this req */
    uint64_t first_ms; /* its first copy */
} plink_cached_t;

/* A DEV_ACT's answer (LINK-047): never its code. */
typedef struct {
    uint32_t req;
    uint8_t digest[PLINK_SHA256_LEN]; /* plink_dev_act_digest */
    bool pending;
    uint32_t ticket;
    uint8_t result;                   /* plink_dev_result_t */
    uint8_t why;
    uint64_t last_ms;                 /* the last copy of this req (0: the entry is free) */
    char ref[PLINK_REF_MAX + 1];      /* the device: each DEV_ACK carries its record then */
} plink_dev_cached_t;

/* A keypad's settings report as the master keeps it (LINK-050): RAM only. */
typedef struct {
    bool have;
    uint32_t count;                         /* reports kept so far: moves with each one */
    char panel_id[PLINK_PANEL_ID_LEN + 1];  /* "" when it named none, or not 6 lowercase hex */
    uint8_t voice;                          /* plink_voice_t */
    uint32_t accepts;                       /* bit id - 1: a field it takes */
    uint32_t crc;                           /* prefs_crc: proto_prefs_crc over fields */
    uint8_t fields_len;
    uint8_t fields[PLINK_PREFS_BODY_MAX];   /* the fields section's body as received */
} plink_prefs_report_t;

/* One keypad's Smart Home and settings (private). About 1 KB. */
typedef struct {
    uint64_t changed;           /* bit i: record i changed and waits for the next round */
    uint64_t round;             /* bit i: record i goes in the round under way */
    uint64_t push_at;           /* the next round starts no sooner */
    uint64_t summary_at;        /* the next DEV_STATE at the latest; 0: at once (a new list, a
                                 * request under another, the transport big enough again) */
    uint8_t list_repeats;       /* a new list is named again at +250 ms and +1 s (LINK-044) */
    plink_dev_list_req_t list_q[PLINK_DEV_LIST_QUEUE]; /* pages to serve, oldest first */
    uint8_t list_n;
    uint8_t req_tokens;         /* DEV_LIST_REQ and DEV_STATE_REQ (token bucket) */
    uint64_t req_tokens_at;
    plink_dev_cached_t cache[PLINK_DEV_RESULTS_PER_KEYPAD];
    uint8_t ack_due;            /* bit i: cache[i]'s DEV_ACK goes at the next tick */
    bool act_running;           /* one action in flight */
    uint32_t act_ticket;
    uint8_t act_tokens;         /* DEV_ACT (token bucket) */
    uint64_t act_tokens_at;
    plink_prefs_report_t report;
    uint32_t report_sid, report_seq; /* the frame the report came in: a newer one replaces it */
    uint8_t prefs_tokens;       /* PREFS (token bucket) */
    uint64_t prefs_tokens_at;
    bool prefs_want;            /* the heartbeat's prefs_crc isn't the report's */
    uint64_t prefs_req_at;      /* the last PREFS_REQ (0 none) */
    bool set_busy;              /* a write in flight */
    uint32_t set_req;
    uint8_t set_origin;
    uint8_t set_len;
    uint8_t set_fields[PLINK_PREFS_SET_BODY_MAX];
    uint64_t set_started_ms, set_next_ms;
} plink_home_peer_t;

/* One keypad's updates through the link (private). */
typedef struct {
    bool have_info;
    plink_fw_info_t info;       /* its newest FW_INFO ... */
    uint32_t info_sid, info_seq; /* ... the frame it came in ... */
    uint64_t info_ms;           /* ... and when */
    bool have_state;
    plink_fw_state_t state;     /* its newest FW_STATE, the same way */
    uint32_t state_sid, state_seq;
    uint64_t state_ms;
    plink_fw_order_t order;     /* the order outstanding (order.order 0: none) */
    bool acked;                 /* a FW_STATE echoed it: no more resends */
    uint8_t why;                /* ... with this why */
    uint64_t order_ms;          /* given then */
    uint64_t next_ms;           /* sent again then */
    uint32_t retry_ms;          /* 300 ms, doubling to 2 s */
    bool relay;                 /* a relay is served: its RELAY is the newest order but INFO */
    uint8_t relay_id8[PLINK_ID8_LEN];
    uint32_t relay_size;
    uint16_t relay_page;
    bool req;                   /* a FW_REQ waits for the tick: */
    plink_fw_req_t req_q;
    uint32_t sessions;          /* moves with each new session */
    uint64_t hold_ms;           /* installing or on its self-test: this hold began then (0: none) */
    bool hold_on;               /* ... and its newest FW_STATE still says so */
    bool hold_renew;            /* a new session since the hold began: one more may begin */
    uint8_t tokens;             /* FW_INFO and FW_STATE (token bucket) */
    uint64_t tokens_at;
} plink_fw_peer_t;

/* One keypad at run time (private). */
typedef struct {
    bool used;
    plink_peer_rec_t rec;
    plink_session_t live;
    plink_session_t pending;
    uint8_t welcome[128];
    size_t welcome_len;
    uint8_t pending_hello[128];
    size_t pending_hello_len;
    uint64_t pending_until;
    uint8_t seen_nk[PLINK_HELLO_SEEN][PLINK_NONCE_LEN];
    uint8_t seen_count;
    bool have_addr;
    plink_addr_t addr;
    uint64_t heard_ms;          /* last authenticated frame */
    uint64_t live_heard_ms;
    uint16_t status_age_s;      /* as the keypad last reported */
    uint64_t stale_since_ms;    /* 0 = its status is fresh */
    bool down;                  /* supervision: told the platform it's down */
    uint64_t status_due_ms;     /* the next periodic STATUS */
    uint64_t repeat1_ms, repeat2_ms; /* repeats of a changed STATUS (0 = none) */
    uint64_t last_status_ms;
    uint32_t sent_zones_crc;
    uint64_t time_due_ms;
    plink_cached_t cache[PLINK_RESULTS_PER_KEYPAD];
    uint64_t cmd_tokens_at;     /* rate limit (token bucket) */
    uint8_t cmd_tokens;
    bool cmd_running;           /* one command in flight (panic has its own slot) */
    bool panic_running;
    uint32_t bye_left;          /* BYEs still to send after removal */
    uint64_t bye_at;
    plink_heartbeat_t hb;       /* the last heartbeat */
    uint32_t addr_flips;        /* clone detection */
    uint64_t requests_at;       /* CFG/ZONES/EV request rate limit */
    uint8_t requests;
    bool removing;              /* removed: only its BYEs still go (used is false) */
    uint64_t provisional_until; /* a provisional keypad is dropped then */
    uint8_t live_nk[PLINK_NONCE_LEN];    /* n_k of the live session's HELLO */
    uint8_t pending_nk[PLINK_NONCE_LEN]; /* ... and of the pending one's */
    uint8_t seen_next;          /* the next seen_nk slot */
    uint64_t rekey_at;          /* the last REKEY sent (0 none) */
    uint64_t change_ms;         /* a changed STATUS waits since then (0 none) */
    uint16_t zones_lo, zones_hi; /* zones changed since the last STATUS (0 none) */
    uint32_t ev_sent_seq;       /* the newest event this session has been sent */
    uint8_t result_due;         /* bit i: cache[i] answered, its RESULT not sent yet */
    uint8_t cache_panic;        /* bit i: cache[i] is a panic (resent for 60 s, not 3 s) */
    uint32_t cmd_ticket, panic_ticket; /* the commands in flight */
    uint64_t hs_tokens_at;      /* its fresh handshakes (token bucket) */
    uint8_t hs_tokens;
    plink_home_peer_t home;     /* Smart Home and settings */
    plink_fw_peer_t fw;         /* updates through the link */
} plink_peer_t;

typedef struct {
    /* private */
    const plink_crypto_t *c;
    plink_master_port_t port;
    uint16_t caps;                  /* what this master offers (PLINK_CAPS_KNOWN at most) */
    plink_master_rec_t rec;
    plink_peer_t peer[PLINK_KEYPADS_MAX];
    plink_pair_m_t pair;
    char name[PLINK_NAME_MAX + 1];
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    alarm_snapshot_t snap;
    bool have_snap;
    uint64_t snap_changed_ms;
    int64_t epoch_ms;
    uint64_t epoch_at_ms;
    uint8_t report;
    const uint8_t *cfg;
    size_t cfg_len;
    uint32_t cfg_crc;
    plink_time_t time;
    bool time_changed;
    bool net_up;
    uint64_t net_up_since_ms;
    uint32_t next_ticket;
    uint64_t x25519_window_ms;
    uint8_t x25519_count;
    uint8_t out[PLINK_FRAME_MAX];   /* frame scratch */
    uint8_t pt[PLINK_FRAME_MAX];    /* payload scratch */
    alarm_event_t ev[PLINK_EV_ANSWER_MAX]; /* event scratch */
    uint64_t time_at_ms;            /* time.epoch_ms was the wall clock then */
    uint32_t zones_crc;             /* of the snapshot's zones 1..zones_used */
    uint64_t not_paired_at;         /* the last NOT_PAIRED sent (0 none) */
    bool store_due;                 /* the record must be written again */
    uint64_t store_at;
    bool in_run;                    /* inside port.run: answers wait until it returns */
    plink_out_t pout;               /* the pairing's answer */
    plink_status_t st_a, st_b;      /* status scratch */
    /* Smart Home: the device list (the blob stays the caller's) and a copy of its records. */
    const uint8_t *dev_blob;
    size_t dev_blob_len;
    uint32_t dev_list_crc;          /* 0: no list */
    plink_dev_recs_t dev_recs;
    uint32_t dev_rec_crc[PLINK_DEV_MAX]; /* each record's CRC-32: what changed */
    uint32_t dev_states_crc;
    uint8_t home_first;             /* the keypad whose home frames go first at the next tick */
    /* Updates through the link. */
    bool fw_updating;               /* STATUS's UPDATING (plink_master_fw_self) */
    uint32_t fw_next_order;         /* the next order's id (0: the random base not drawn yet) */
    uint32_t fw_mbytes;             /* FW_PAGE's byte bucket, in thousandths of a byte */
    uint64_t fw_bytes_at;
} plink_master_t;

/* rec: the stored record (NULL or a zero net_id: none yet; a net id is chosen at the first
 * pairing). name: the master's name for keypads (the installation's name); panel_id: six hex
 * characters. caps: the capability bits this master offers (PLINK_CAP_*, masked with
 * PLINK_CAPS_KNOWN; LINK-043): each session keeps those its keypad offers too. Every stored
 * keypad starts as heard at now (the engine's grace at start-up). */
void plink_master_init(plink_master_t *m, const plink_crypto_t *c, const plink_master_port_t *port,
                       const plink_master_rec_t *rec, const char *name,
                       const char panel_id[PLINK_PANEL_ID_LEN + 1], uint16_t caps,
                       uint64_t now_ms);

/* A frame from the network (any transport) with its source. */
void plink_master_input(plink_master_t *m, uint64_t now_ms, const plink_addr_t *from,
                        const uint8_t *frame, size_t len);
/* Timers; call at plink_master_next() at the latest. */
void plink_master_tick(plink_master_t *m, uint64_t now_ms);
uint64_t plink_master_next(const plink_master_t *m);

/* The engine's newest snapshot (copied), with the wall clock now and the reporting state: a
 * changed STATUS goes to every live keypad (coalesced, repeated, LINK-006). */
void plink_master_snapshot(plink_master_t *m, uint64_t now_ms, const alarm_snapshot_t *snap,
                           int64_t epoch_ms, uint8_t report);
/* The keypads' copy of the configuration (plink_cfg_for_keypads); the blob stays the caller's
 * and must stay valid until the next call. */
void plink_master_config(plink_master_t *m, const uint8_t *blob, size_t len, uint32_t crc);
/* A new event: pushed to live keypads before the STATUS that carries its seq. */
void plink_master_event(plink_master_t *m, uint64_t now_ms, const alarm_event_t *ev);
/* A one-shot sound (the engine's on_sound): to the keypads whose home it concerns (partition 0
 * = all); fire and CO to every keypad. */
void plink_master_sound(plink_master_t *m, uint64_t now_ms, uint8_t partition, uint8_t sound,
                        uint16_t zone);
/* The wall clock and time zone; sent at each new session, on a change, every 10 min. */
void plink_master_time(plink_master_t *m, uint64_t now_ms, int64_t epoch_ms, const char *tz);
/* The master's name (shown on keypads and in pairing). */
void plink_master_name(plink_master_t *m, const char *name);
/* The master's own network is up or down (supervision pauses while it's down). */
void plink_master_net(plink_master_t *m, uint64_t now_ms, bool up);
/* A command's answer from the platform (port.run's ticket). */
void plink_master_result(plink_master_t *m, uint64_t now_ms, uint32_t ticket,
                         const plink_result_t *res);

/* ---- pairing (the installer, in installer mode) ---- */
/* Opens the window; false when all 8 numbers are taken (nothing opened) or a net id couldn't be
 * stored. */
bool plink_master_pair_open(plink_master_t *m, uint64_t now_ms);
void plink_master_pair_cancel(plink_master_t *m, uint64_t now_ms);
void plink_master_pair_decide(plink_master_t *m, uint64_t now_ms, bool allow);
void plink_master_pair_info(const plink_master_t *m, uint64_t now_ms, plink_pair_info_t *out);
/* Whether pairing is open (mDNS pair=1). */
bool plink_master_pairing(const plink_master_t *m);

/* ---- the keypads ---- */
typedef struct {
    uint8_t device;
    char name[PLINK_NAME_MAX + 1];
    uint8_t home;
    uint8_t kind;
    uint16_t supervision_s;
    bool provisional;
    bool online;          /* a live session heard within its supervision time */
    uint16_t caps;        /* the live session's common capabilities (0: none, or no session) */
    bool down;            /* the trouble is raised */
    uint32_t heard_ago_s; /* UINT32_MAX never */
    plink_addr_t addr;
    plink_heartbeat_t hb;
} plink_peer_info_t;

/* The keypads, by number: returns how many there are and fills the first max into out (out may
 * be NULL with max 0, to count them). */
size_t plink_master_peers(const plink_master_t *m, uint64_t now_ms, plink_peer_info_t *out,
                          size_t max);
/* Removes keypad device (LINK-014): its trouble cleared, BYE sealed and sent (three times over
 * 2 s), its keys wiped from RAM, then the record stored without it. false: no such keypad, or
 * the record could not be written (the keypad is refused anyway; storing is retried at tick). */
bool plink_master_remove(plink_master_t *m, uint64_t now_ms, uint8_t device);
bool plink_master_rename(plink_master_t *m, uint8_t device, const char *name);
bool plink_master_set_home(plink_master_t *m, uint64_t now_ms, uint8_t device, uint8_t home);
bool plink_master_set_supervision(plink_master_t *m, uint64_t now_ms, uint8_t device,
                                  uint16_t seconds);
/* ---- Smart Home and settings on keypads (LINK-043..051) ---- */
/* The registry's device list and its records, as plink_home_list_build and plink_home_recs_build
 * made them from one copy of the registry (index for index): the blob stays the caller's and must
 * stay valid until the next call; the records are copied. A blob of NULL (or list_crc 0) is no
 * list: no DEV_STATE goes and every DEV_ACT is answered UNAVAILABLE. A new list_crc clears what
 * each keypad still had to be sent (records under the new list are of no use until it installs
 * it) and sends a DEV_STATE at once, which names the new list; otherwise the records whose bytes
 * changed go in the next round. */
void plink_master_home(plink_master_t *m, uint64_t now_ms, const uint8_t *blob, size_t len,
                       uint32_t list_crc, const plink_dev_recs_t *recs);
/* A device action's answer from the platform (port.dev_run's ticket): result a
 * plink_dev_result_t, why the alarm_result_t of a refused code. Its DEV_ACK goes at the next tick
 * with the device's record as it is then (plink_master_home's newest). */
void plink_master_dev_answer(plink_master_t *m, uint64_t now_ms, uint32_t ticket, uint8_t result,
                             uint8_t why);
/* Changes keypad device's settings (LINK-051): fields is a fields body (count first, the fields
 * to change, only ids both this master and the keypad's accepts know), req the write's id (not 0).
 * Refused at once (false, *why: plink_pset_t) when no keypad has that number, a bad body, the
 * keypad offline (no live session, or not heard for 30 s), unsupported (no PLINK_CAP_PREFS, or a
 * transport under 250 B), or busy (one write in flight). Otherwise PREFS_SET goes at the next tick
 * and every second with the same req until the PREFS carrying req answers it, or 6 s have
 * passed: port.prefs_done says which, once. */
bool plink_master_prefs_set(plink_master_t *m, uint64_t now_ms, uint8_t device, uint32_t req,
                            uint8_t origin, const uint8_t *fields, size_t len, uint8_t *why);

/* A keypad's Smart Home and settings side, for the platform's list of keypads. */
typedef struct {
    uint8_t device;
    bool online;   /* a live session heard within the last 30 s */
    uint16_t caps; /* the live session's common capabilities (0: none, or no session) */
    bool id_taken; /* a keypad with a lower number reports the same panel id: prefs.panel_id is
                    * "" here (the platform logs it) */
    plink_prefs_report_t prefs; /* its newest settings report (prefs.have: one came) */
} plink_keypad_home_t;

/* Keypad device's (false: no such keypad). */
bool plink_master_keypad_home(const plink_master_t *m, uint64_t now_ms, uint8_t device,
                              plink_keypad_home_t *out);

/* ---- updates through the link (LINK-029..037) ---- */
/* The master's own update runs, or it is on its self-test: STATUS's UPDATING (LINK-030); a change
 * sends a STATUS to every live keypad, as any change does (LINK-006). */
void plink_master_fw_self(plink_master_t *m, uint64_t now_ms, bool updating);
/* STATUS's HOLD now: proto_fw_install_allowed() says no on the master's snapshot (true before the
 * first snapshot). The planner's quiet is its opposite. */
bool plink_master_fw_hold(const plink_master_t *m);
/* An order for keypad device (the planner's: everything but its id): numbered here (a random base
 * drawn at the first order since start, then counted, never 0), it supersedes the one outstanding
 * - which is never sent again - and goes at the next tick in a session with PLINK_CAP_FW on frames
 * of 250 B or more (held until there is one), then again after 300 ms, doubling to 2 s, until a
 * FW_STATE echoes its id. Returns the id; 0: no such keypad, or op isn't one of plink_fw_op_t. */
uint32_t plink_master_fw_order(plink_master_t *m, uint64_t now_ms, uint8_t device,
                               const plink_fw_order_t *order);
/* Keypad device as the planner sees it (false, *out zeroed: no such keypad). */
bool plink_master_fw_keypad(const plink_master_t *m, uint64_t now_ms, uint8_t device,
                            plink_fw_kp_t *out);
/* A keypad installs or is on its self-test (each for up to 10 min): the master puts off its own
 * restart (LINK-030). */
bool plink_master_fw_busy(const plink_master_t *m, uint64_t now_ms);

/* The numbers the link holds (bit d-1). */
uint32_t plink_master_held(const plink_master_t *m);
/* Whether the link is in use (a keypad paired or pairing open): mDNS is announced only then. */
bool plink_master_in_use(const plink_master_t *m);
uint16_t plink_master_net_id(const plink_master_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_MASTER_H */
