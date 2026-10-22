/*
 * plink_keypad.h - a keypad's side of the panel link (docs/features/11; LINK-002, -006, -008,
 * -014, -015, -016, -017, -018, -023, -025, -026): its master, the handshake, the master's status
 * rebuilt for the UI, the configuration and events pulled from the master, commands and panic.
 *
 * Portable and single-threaded: one task owns a plink_keypad_t; time comes in as the keypad's own
 * monotonic ms. The platform copies what the UI needs (snapshot, configuration, events) under its
 * own lock after each call; nothing here blocks.
 *
 * Link state (LINK-008): NOT_PAIRED; CONNECTING from start-up (or a new pairing) until a session,
 * the first STATUS and a configuration matching its cfg_crc are all in; ONLINE; LOST after 30 s
 * without an authenticated STATUS, back to ONLINE only after a full sync (a STATUS since, and the
 * configuration it names) and no sooner than 3 s after entering LOST. A handshake starts at
 * start-up, after 15 s without an authenticated STATUS (counted from the last WELCOME too: a new
 * session brings a STATUS at once), on a REKEY for the live sid (at most every 5 s), at seq 2^30,
 * every 24 h; failures back off 1, 2, 4 ... 30 s with jitter (2 s at most while a panic waits).
 * A WELCOME is worked out only from where its HELLO went (unless that was a broadcast, as
 * plink_keypad_master_addr was told), at most PLINK_WELCOME_CHECKS per HELLO. Not paired, the
 * no-code maintenance pages are open (LINK-025).
 *
 * Smart Home and settings (LINK-043..052; docs/features/11 "Smart Home and Home Assistant on
 * keypads"). With a mirror attached (plink_keypad_home_attach; without one the keypad never offers
 * PLINK_CAP_HOME), the keypad mirrors its master's device list: pulled by the list_crc a DEV_STATE
 * names, page by page as the configuration is, kept installed until a new one is whole (or until a
 * session without HOME starts: dropped then, so Smart Home hides), each row's record carried over
 * by ref at an install, then every record asked for. A record applies only
 * under the installed list and from a newer frame of the session for its device (LINK-026); while
 * the records' CRC differs from the master's, DEV_STATE_REQ goes 250 ms on, then at least 1 s
 * apart, doubling to 30 s. Device data (a DEV_STATE, or a DEV_ACK's record, under the installed
 * list) older than 30 s, or a link not ONLINE, reads offline (LINK-046). A device action
 * (plink_keypad_dev_act) is refused at once when it can't go, else resent every 500 ms with the
 * same req, given up after 3 s and never sent after; 2 slots of its own, apart from the
 * commands'. List and state requests wait while a panic is being sent. In a
 * session with PLINK_CAP_PREFS the keypad reports its settings (plink_keypad_prefs_report: PREFS
 * at the session's first STATUS, after a change at most once a second, and on PREFS_REQ; each
 * HEARTBEAT carries their CRC), takes each PREFS_SET once (plink_keypad_take_prefs_set) and
 * answers it (plink_keypad_prefs_answer), a repeat from the answer kept 30 s. Nothing of this goes
 * on a transport whose frames are under 250 B.
 *
 * Updates through the link (LINK-029..037; docs/features/11 "Updates through the link", "The
 * keypad"), in a session with PLINK_CAP_FW on a transport of 250 B or more. The core takes the
 * master's orders and answers each with FW_STATE at once (an order seen before is answered again
 * and never runs twice; a new one from an older frame of the session than the newest taken is
 * dropped); refuses what it can tell is wrong; hands PROBE, FETCH, a new RELAY, INSTALL, CANCEL and
 * INFO to its service (plink_keypad_take_fw_order), which reports back (plink_keypad_fw_report,
 * _progress, _probe, _last, _info, _gate) for the core to send as FW_STATE and FW_INFO; and runs
 * the relay itself: one FW_REQ out at a time, by image id (id8) and offset from the first byte the
 * writer lacks up to the first pages already in past it, within the order's page, count and gap and
 * a 16 KB window; resent after 300 ms doubling to 2 s (300 ms after its last page when it was
 * answered in part) for the bytes still missing; the bytes go to the writer (port.fw_write) in
 * order, at most 4 KB a turn, the next write and request no sooner than the last write kept the
 * flash busy. Downloading goes on while armed and during the master's own update: the run pauses
 * (FW_STATE flag PAUSED, why the reason) only while the link isn't up - a live session with FW and
 * a STATUS under 15 s old (NO_STATUS) -, the view shows an alarm sounding or an entry or exit time
 * (NOT_QUIET), its own panic is still being sent (PANIC_PENDING) or the service's gate says no (a
 * kept fire or CO tone, plink_keypad_fw_gate); it fails after 60 s without a new byte while not
 * paused (STALLED) or 30 min paused (TIMEOUT), and goes on across a new session, the 24 h rekey and
 * a master restart, by id8. 3 runs failed before READY within 24 h: a new FETCH or RELAY is refused
 * (BUDGET); a TIMEOUT while the gate said no (anything but a missing STATUS) isn't a failed run,
 * nor is a fetch whose check never reached the server or that the server answered (1-19: nothing
 * was written). An INSTALL records the version it names (the INSTALL hold,
 * plink_keypad_fw_install_for), held only in the session it came in and while no other order but
 * INFO came after it: a new session, a PROBE, FETCH, CANCEL or RELAY taken after it (but a RELAY
 * that goes on) and the run's end each void it - so a master that restarted or gave up that INSTALL
 * never finds the keypad restarting on it. A CANCEL that comes once the restart has started is
 * refused BUSY and the restart goes on; put off (READY again, OTA-028), the run ends as that CANCEL
 * said (STOPPED, CANCELLED), the hold with it. A READY image is discarded after 24 h without an
 * INSTALL held, on CANCEL, on an order for another image and on removal. The version the master
 * runs - the one the keypad may not pass (LINK-029; Decisions, keypad updates, 5) - is kept as INFO
 * names it (plink_keypad_master_version): only INFO, from the paired master's session in frame
 * order (the other orders name the master's target, which may be ahead of what it runs; an INFO
 * with no version names nothing); a newer or an older one replaces it, and removal, a new pairing
 * or a session without PLINK_CAP_FW clears it. The cap the service applies is that version while
 * paired ("": nothing passes) and none unpaired (plink_keypad_version_cap). After an install its
 * master ordered, the keypad is on trial with its master (plink_keypad_fw_trial): an INFO naming
 * its version (or a newer one) passes it, an older one fails it, and none within 10 min fails it.
 */
#ifndef PLINK_KEYPAD_H
#define PLINK_KEYPAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_home.h"
#include "panel_link/plink_msg.h"
#include "panel_link/plink_pair.h"
#include "panel_link/plink_session.h"
#include "panel_link/plink_store.h"
#include "panel_link/plink_view.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_HEARTBEAT_MS       10000u /* LINK-006, ±1 s jitter */
#define PLINK_TENTATIVE_HB_MS    500u
#define PLINK_REHANDSHAKE_MS     15000u
#define PLINK_LOST_MS            30000u /* LINK-008 */
#define PLINK_LOST_MIN_MS        3000u
#define PLINK_REKEY_GAP_MS       5000u
#define PLINK_SESSION_MAX_MS     86400000u
#define PLINK_BACKOFF_MAX_MS     30000u
#define PLINK_CMD_RETRY_MS       500u   /* LINK-018 */
#define PLINK_CMD_GIVE_UP_MS     3000u
#define PLINK_PANIC_RETRY_MS     1000u  /* LINK-023 */
#define PLINK_PANIC_KEEP_MS      60000u
#define PLINK_PULL_RETRY_MS      300u
#define PLINK_PULL_RETRY_MAX_MS  2000u
#define PLINK_PULL_OUTSTANDING   2u
#define PLINK_EVENTS_KEPT        50u
#define PLINK_MAINTENANCE_MS     600000u /* LINK-025 */
#define PLINK_NOT_PAIRED_HINT_MS 300000u
#define PLINK_CMDS_MAX           4u      /* commands in flight (one is panic's) */
#define PLINK_WELCOME_CHECKS     8u      /* WELCOMEs worked out (X25519) per HELLO, at most */
/* Smart Home and settings (LINK-045..051); a device action's resend and give-up are
 * PLINK_DEV_ACT_RETRY_MS and PLINK_DEV_ACT_GIVE_UP_MS (plink_home.h). */
#define PLINK_DEV_ACTS_MAX       2u      /* device actions in flight, apart from the commands */
#define PLINK_DEV_STALE_MS       30000u  /* LINK-046: no device data this long reads offline */
#define PLINK_DEV_REQ_WAIT_MS    250u    /* LINK-045: DEV_STATE_REQ this long after a mismatch, */
#define PLINK_DEV_REQ_GAP_MS     1000u   /* then at least this far apart, ... */
#define PLINK_DEV_REQ_GAP_MAX_MS 30000u  /* ... doubling up to this */
#define PLINK_PREFS_GAP_MS       1000u   /* LINK-050: an unasked-for PREFS at most this often */
#define PLINK_PREFS_KEEP_MS      30000u  /* LINK-051: a PREFS_SET's answer kept for its repeats */
#define PLINK_PREFS_OWN_MAX      128u    /* the keypad's own fields body (v1's 19 fields: 69 B) */
#define PLINK_PREFS_REFUSED_MAX  32u     /* refusals an answer carries, at most */
/* Updates through the link (LINK-032..035); a FW_REQ's resend is PLINK_PULL_RETRY_MS doubling to
 * PLINK_PULL_RETRY_MAX_MS. */
#define PLINK_FW_WINDOW          16384u    /* LINK-032: bytes asked for ahead of the writer */
#define PLINK_FW_WRITE_MAX       4096u     /* in-order bytes handed to the writer in one turn */
#define PLINK_FW_ISLANDS         8u        /* runs of pages kept past a missing one */
#define PLINK_FW_FRESH_MS        15000u    /* LINK-034: the link is up - a STATUS this recent */
#define PLINK_FW_STALL_MS        60000u    /* LINK-035: no new byte this long, not paused */
#define PLINK_FW_PAUSE_MAX_MS    1800000u  /* ... paused this long */
#define PLINK_FW_READY_KEEP_MS   86400000u /* a READY image without an INSTALL this long */
#define PLINK_FW_BUDGET          3u        /* failed runs within PLINK_FW_BUDGET_MS: BUDGET */
#define PLINK_FW_BUDGET_MS       86400000u
#define PLINK_FW_STATE_GAP_MS    1000u     /* FW_STATE for bytes at most this often, ... */
#define PLINK_FW_STATE_EVERY_MS  10000u    /* ... and this often while a run exists */
#define PLINK_FW_INFO_GAP_MS     2000u     /* FW_INFO at most this often */
#define PLINK_FW_SEEN            8u        /* order ids remembered: a repeat is answered */
#define PLINK_FW_TRIAL_MS        600000u   /* LINK-034: on trial without an INFO this long fails */
#define PLINK_FW_QUEUE           4u        /* orders waiting for the service */

typedef enum {
    PLINK_LINK_NOT_PAIRED = 0,
    PLINK_LINK_CONNECTING,
    PLINK_LINK_ONLINE,
    PLINK_LINK_LOST,
} plink_link_state_t;

/* Why the keypad isn't paired (plink_keypad_info). */
typedef enum {
    PLINK_UNPAIRED_NEVER = 0,
    PLINK_UNPAIRED_REMOVED = 1, /* a BYE from the master */
} plink_unpaired_t;

typedef enum {
    PLINK_CMD_NONE = 0,  /* no such request */
    PLINK_CMD_PENDING,
    PLINK_CMD_DONE,      /* the master answered: see the result */
    PLINK_CMD_NO_ANSWER, /* given up (3 s; a panic after 60 s) */
} plink_cmd_state_t;

typedef struct {
    void *ctx;
    bool (*send)(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len);
    size_t (*mtu)(void *ctx, uint8_t transport); /* NULL: PLINK_FRAME_MAX */
    /* Writes (rec) or erases (NULL) the keypad's record now; false when it didn't happen (tried
     * again every 5 s). */
    bool (*store)(void *ctx, const plink_keypad_rec_t *rec);
    /* Look for the master again (mDNS, then a broadcast) and report it through
     * plink_keypad_master_addr: called before every handshake retry after the first failure
     * (docs/features/11 "Sessions"). May be NULL. */
    void (*resolve)(void *ctx);
    /* The relay's writer (LINK-032): bytes [offset, offset + len) of the image, in order - offset
     * is always the first byte it hasn't taken, len at most PLINK_FW_WRITE_MAX. Returns how many it
     * took now (0: busy, tried again later); *busy_ms (0 when not set) how long the write kept the
     * flash busy: the next write and the next FW_REQ wait at least that long, so the flash is busy
     * at most half the time. The last byte taken ends the transfer: the service checks the image
     * and reports READY or FAILED. Must not call into the keypad. NULL: this keypad takes no relay
     * (a RELAY is refused BAD_ORDER, and FW_INFO says it can't). */
    size_t (*fw_write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len,
                       uint32_t *busy_ms);
    /* LINK-053: matched reply; valid only during the call, never log secrets. */
    void (*wifi_credentials)(void *ctx, const plink_wifi_t *wifi);
} plink_keypad_port_t;

typedef struct {
    bool used;
    uint32_t req;
    plink_cmd_t cmd;       /* the codes are wiped once answered or given up */
    uint64_t started_ms, next_ms;
    bool panic;
    uint8_t state;         /* plink_cmd_state_t */
    plink_result_t res;
} plink_kcmd_t;

/* One pull request out (private). */
typedef struct {
    bool used;
    uint32_t off;
    uint16_t max;
    uint64_t due_ms;   /* sent again then */
    uint32_t retry_ms; /* 300 ms, doubling to 2 s */
} plink_kpull_t;

/* A blob pulled from the master page by page, by its CRC (private): the configuration (CFG_REQ,
 * its length named by STATUS) and the device list (DEV_LIST_REQ, its length learned from its first
 * page). The first requests go 0-500 ms after the change, 2 are kept out, each retried after
 * 300 ms doubling to 2 s; a blob that fails is remembered and not pulled again. */
typedef struct {
    uint8_t *buf;          /* the blob being pulled (caller's memory) */
    size_t cap;
    uint8_t type;          /* the requests' message type */
    bool pulling;
    uint32_t crc, total, have; /* total 0: known at the first page; have: bytes in from offset 0 */
    uint32_t ahead_off, ahead_len; /* one page in past the first gap */
    plink_kpull_t req[PLINK_PULL_OUTSTANDING];
    uint32_t bad_crc;      /* a blob that failed (or doesn't fit): not pulled again */
    uint64_t next_ms;      /* the pull's first requests */
} plink_pull_t;

/* The mirror of the master's devices (LINK-044, LINK-045): the caller's memory, about 31 KB (PSRAM
 * on a panel), attached by plink_keypad_home_attach. Private: read it through the getters below. */
typedef struct {
    uint8_t blob[PLINK_DEV_BLOB_MAX];   /* the installed list */
    uint32_t blob_len;
    uint32_t list_crc;                  /* 0: none installed yet */
    uint16_t off[PLINK_DEV_MAX];        /* each row's offset in blob */
    uint32_t keys[PLINK_DEV_MAX];       /* each row's ref key (plink_home_ref_key; 0: unusable) */
    plink_dev_recs_t recs;              /* each row's record as it came (recs.count: the rows) */
    uint32_t frame_seq[PLINK_DEV_MAX];  /* LINK-026: the frame each record last came in */
    uint8_t in[PLINK_DEV_BLOB_MAX];     /* the list being pulled (the installed one stays) */
    uint32_t in_keys[PLINK_DEV_MAX];    /* an install's scratch */
    plink_dev_recs_t carry;             /* an install's scratch */
} plink_dev_mirror_t;

/* What a device action looks like on its device's tile once its caller stopped waiting (the
 * screen's 1 s, 3 s with a code; plink_keypad_dev_act_release), until a later record for that
 * device or the next action on it (docs/features/11 "Device actions"). */
typedef enum {
    PLINK_DEV_OVL_NONE = 0, /* the record as it is */
    PLINK_DEV_OVL_PENDING,  /* still being sent: shown pending */
    PLINK_DEV_OVL_FAILED,   /* the master refused it after the caller stopped waiting: failed */
    PLINK_DEV_OVL_UNSURE,   /* no answer in 3 s: failed and unsure ("Not confirmed", HCTL-012) */
} plink_dev_overlay_t;

/* One device action (private). */
typedef struct {
    bool used;
    uint32_t req;
    plink_dev_act_t act;   /* its code wiped after its last copy, or once answered */
    uint32_t key;          /* its device's ref key */
    uint64_t started_ms, next_ms;
    uint8_t state;         /* plink_cmd_state_t */
    uint8_t result, why;   /* DONE: the master's answer */
    bool released;         /* its caller stopped waiting: the overlay shows */
    uint8_t overlay;       /* plink_dev_overlay_t */
} plink_kact_t;

/* A settings change from the master (LINK-051): PREFS_SET's req, origin and fields. */
typedef struct {
    uint32_t req;
    uint8_t origin;                       /* plink_origin_t */
    uint8_t len;                          /* the fields body's bytes */
    uint8_t fields[PLINK_PREFS_BODY_MAX]; /* the fields to change, count first (proto_prefs) */
} plink_prefs_change_t;

/* The keypad's settings on the link (private). */
typedef struct {
    bool have;                            /* a report was given */
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    uint8_t voice;                        /* plink_voice_t */
    uint32_t accepts;
    uint8_t len;
    uint8_t fields[PLINK_PREFS_OWN_MAX];  /* the values now, a fields body */
    uint32_t crc;                         /* proto_prefs_crc of fields: HEARTBEAT's prefs_crc */
    bool due;                             /* an unasked-for PREFS waits */
    uint64_t next_ms;                     /* ... and goes no sooner */
    uint32_t first_sid;                   /* the session whose first STATUS asked for one */
    uint8_t in_state;                     /* the change below: 0 none, 1 waiting, 2 taken */
    plink_prefs_change_t in;
    uint32_t ans_req;                     /* the last answer (0: none), for repeats of its req */
    uint64_t ans_ms;
    uint8_t ans_count;
    uint8_t ans_refused[2u * PLINK_PREFS_REFUSED_MAX];
} plink_kprefs_t;

/* The trial with the master (plink_keypad_fw_trial_state; local: never on the wire). */
typedef enum {
    PLINK_FWT_NONE = 0, /* no trial */
    PLINK_FWT_WAIT,     /* on trial: no INFO yet */
    PLINK_FWT_PASS,     /* an INFO named its version, or a newer one: the master runs it */
    PLINK_FWT_FAIL,     /* an INFO named an older one (the master went back), or none in 10 min */
} plink_fw_trial_t;

/* An order seen (private): a repeat is answered again, never run twice. */
typedef struct {
    uint32_t order;
    uint8_t op;
    uint8_t why; /* its refusal (0: taken) */
} plink_kfw_seen_t;

/* The keypad's side of updates through the link (private; LINK-029..037). */
typedef struct {
    /* orders */
    plink_kfw_seen_t seen[PLINK_FW_SEEN];
    uint8_t seen_next;
    uint32_t order;           /* the newest order taken (0: none): every FW_STATE echoes it ... */
    uint8_t op;               /* ... with its op ... */
    uint8_t refused;          /* ... and its refusal as the why, until the run moves (0: taken) */
    bool cancel_busy;         /* a CANCEL refused BUSY (its restart under way), no order taken
                               * since: its restart put off (READY again), the run ends as that
                               * CANCEL said - the INSTALL hold with it (OTA-028) */
    uint32_t order_fs;        /* LINK-026: the frame the newest order came in (this session) */
    plink_fw_order_t q[PLINK_FW_QUEUE]; /* for the service, oldest first (take_fw_order) */
    uint8_t q_count;
    bool abort;               /* the core ended the run: an order-0 CANCEL for the service */
    /* the run: FETCH or RELAY, and FW_STATE's run section */
    uint32_t run;             /* the order that started it (0: none yet) */
    uint8_t run_op;
    uint8_t state, flags, why;
    uint32_t bytes, size;
    uint8_t id8[PLINK_ID8_LEN];
    char target[PLINK_FW_VERSION_MAX + 1];
    char install[PLINK_FW_VERSION_MAX + 1]; /* the version an INSTALL named ("": none) */
    uint64_t ready_ms;        /* READY since */
    uint64_t fail_ms[PLINK_FW_BUDGET]; /* when the last runs failed (0: none) */
    /* the relay (RELAY, RECEIVING): the pull and the window */
    bool go;                  /* the service's writer is open: the pull runs */
    uint16_t page;            /* the order's page, count and gap */
    uint8_t count;
    uint16_t gap_ms;
    uint8_t hold;             /* why it is paused (0: it isn't) */
    uint8_t svc_hold;         /* the service's gate (plink_keypad_fw_gate; 0 passes) */
    uint64_t progress_ms;     /* the last new byte, the start, or the end of a pause */
    uint64_t paused_ms;       /* paused since */
    uint32_t wrote;           /* bytes the writer took */
    uint32_t have;            /* bytes in from offset 0 (those past wrote wait in win) */
    uint32_t ask_hi;          /* the end of what was asked for */
    uint32_t isl_lo[PLINK_FW_ISLANDS], isl_hi[PLINK_FW_ISLANDS]; /* pages in past have, sorted */
    uint8_t isl_n;
    bool req_out;             /* a FW_REQ waits for its bytes, ... */
    uint32_t req_end;         /* ... up to here; ... */
    uint64_t req_due;         /* ... it is sent again then, ... */
    uint32_t req_retry;       /* ... after 300 ms, doubling to 2 s */
    uint64_t req_ms;          /* the last FW_REQ (0: none) */
    uint64_t write_ms;        /* the next write and FW_REQ no sooner (the flash was busy) */
    /* FW_STATE's last section (the service's) and probe section */
    uint8_t probation, last, failed_mask;
    char last_version[PLINK_FW_VERSION_MAX + 1];
    uint8_t asked, probe_why, key_trusted;
    char offered[PLINK_FW_VERSION_MAX + 1];
    uint8_t offered_key8[PLINK_KEY8_LEN];
    uint64_t probe_ms;
    /* sending */
    bool st_due;              /* a change to report: FW_STATE */
    uint64_t st_ms;           /* the last FW_STATE (0: none) ... */
    uint32_t st_bytes;        /* ... and its bytes */
    bool have_info;
    plink_fw_info_t info;     /* the keypad's facts (the service's) */
    bool info_due;
    uint8_t info_can;         /* the can bits the last FW_INFO carried */
    uint64_t info_ms;         /* the last FW_INFO (0: none) */
    uint32_t info_sid;        /* the session whose first STATUS asked for one */
    /* the master's version (LINK-029): the one the keypad may not pass */
    char mver[PLINK_FW_VERSION_MAX + 1]; /* as INFO last named it ("": none) */
    uint32_t mver_seq;        /* moves at each change of mver */
    /* the trial with its master (LINK-034) */
    char trial[PLINK_FW_VERSION_MAX + 1]; /* the version on trial ("": none) */
    uint64_t trial_until;     /* failed then, without an INFO */
    uint8_t trial_res;        /* plink_fw_trial_t */
    uint8_t win[PLINK_FW_WINDOW]; /* the relay's window: image byte o at win[o % PLINK_FW_WINDOW] */
} plink_kfw_t;

/* The remote backend's call under way (LINK-058; private). */
#define PLINK_RB_RETRY_MS        1000u  /* no word for this long: the arguments go again */
#define PLINK_RB_GIVE_UP_MS      3000u  /* a call with no answer: given up (LINK-063) */
#define PLINK_RB_GIVE_UP_LONG_MS 10000u /* ... one whose arguments go in parts */
#define PLINK_RB_MORE_RETRY_MS   300u
#define PLINK_RB_PARTS_PER_TICK  4u
typedef struct {
    uint8_t *args;          /* the caller's buffers (plink_keypad_rb_attach) */
    size_t args_cap;
    uint8_t *ans;
    size_t ans_cap;
    bool busy;              /* a call: */
    bool done;
    uint32_t req;
    uint32_t next_req;      /* the next req (from a random base, never 0) */
    uint16_t call;
    size_t args_len;
    size_t sent;            /* argument bytes sent in this round */
    uint64_t started_ms;
    uint64_t heard_ms;      /* this round began, or the last RB_ANSWER part came */
    uint8_t status;
    size_t ans_total, ans_got;
    bool more_due;
    uint64_t more_at;
    bool have_grant;
    plink_rb_grant_t grant;
    uint32_t grant_sid, grant_seq;
} plink_krb_t;

typedef struct {
    /* private */
    const plink_crypto_t *c;
    plink_keypad_port_t port;
    bool paired;
    uint8_t unpaired_why;
    plink_keypad_rec_t rec;
    uint8_t store_due;           /* 1: rec to write, 2: the record to erase (retried) */
    uint64_t store_at_ms;
    plink_pair_k_t pair;
    bool pairing;
    uint8_t kind;
    plink_caps_t cap;
    uint16_t caps;               /* what this keypad offers (PLINK_CAPS_KNOWN at most) */
    uint32_t wifi_req; /* outstanding enrollment request; zero rejects all replies */
    uint16_t caps_asked;         /* what its caller offered: HOME goes only with a mirror */
    char name[PLINK_NAME_MAX + 1];
    char fw[PLINK_FW_MAX + 1];
    bool have_master;
    plink_addr_t master;         /* where sealed frames go: moved only by authenticated ones */
    bool have_disc;
    plink_addr_t disc;           /* where discovery last found the master: HELLOs go there */
    bool disc_bcast;             /* disc is a broadcast (said by the caller, or 255.255.255.255) */
    plink_session_t live;
    bool tentative;              /* the live session waits for its first frame from the master */
    plink_session_t old;         /* still accepted while tentative */
    plink_hs_k_t hs;
    plink_addr_t hs_to;          /* where the latest HELLO went: its WELCOME comes from there */
    bool hs_bcast;               /* hs_to was a broadcast: the WELCOME comes from any address */
    uint8_t hs_checks;           /* WELCOMEs worked out for it (PLINK_WELCOME_CHECKS at most) */
    uint64_t hs_due_ms, hs_backoff_ms, last_rekey_ms, session_started_ms;
    uint64_t status_heard_ms;    /* the last authenticated STATUS or WELCOME (+15 s: handshake) */
    uint64_t status_ms;          /* last STATUS whose system section was applied */
    uint64_t silent_since_ms;    /* LINK-025: the last status, or the start of connecting */
    uint64_t lost_since_ms;
    uint64_t state_since_ms;
    uint64_t hb_due_ms;
    uint64_t not_paired_since_ms, not_paired_last_ms; /* a run of NOT_PAIRED hints */
    uint8_t state;               /* plink_link_state_t */
    plink_view_t view;
    uint32_t seq;                /* snapshot counter for the UI */
    uint32_t config_seq;
    uint32_t items_sid;          /* LINK-026: the session the frame seqs below count in */
    uint32_t status_frame_seq;   /* LINK-026: the newest frame seq applied per item */
    uint32_t part_frame_seq[ALARM_MAX_PARTITIONS];
    uint32_t zone_frame_seq[ALARM_MAX_ZONES];
    uint32_t time_frame_seq;
    plink_pull_t cfg_pull;       /* the configuration (into the caller's cfg_buf); its bad_crc:
                                  * one that failed to decode, not pulled again */
    uint32_t cfg_installed_crc;
    alarm_config_t *cfg;         /* the decoded configuration (caller's memory) */
    bool have_cfg;
    bool zones_want;             /* the zones' CRC differs from the master's */
    uint64_t zones_next_ms;
    uint32_t zones_gap_ms;
    bool ev_want;                /* the master has events newer than the newest kept */
    uint64_t ev_next_ms;
    uint32_t ev_retry_ms;
    uint32_t ev_frame_seq;       /* the frame that brought the newest kept event */
    uint32_t ev_fill;            /* a hole in the events after this seq is being pulled (0 none) */
    uint8_t ev_fill_tries;
    bool ev_back;                /* the pull under way is a backfill: the newest 50 again */
    uint8_t ev_back_tries;       /* backfills since the ring started */
    alarm_event_t events[PLINK_EVENTS_KEPT]; /* oldest first */
    size_t ev_count;
    plink_kcmd_t cmds[PLINK_CMDS_MAX];
    uint8_t cmd_released;        /* bit i: cmds[i]'s caller stopped waiting */
    uint32_t next_req;
    plink_sound_t sounds[4];
    uint8_t sound_head, sound_count;
    plink_time_t time;
    uint64_t time_at_ms;
    bool time_changed;
    uint8_t power_flags;
    int8_t rssi;
    uint64_t started_ms;
    /* Smart Home (LINK-044..047): none without a mirror */
    plink_dev_mirror_t *home;
    plink_pull_t dev_pull;       /* the device list (into home->in) */
    uint32_t dev_named_crc;      /* the list the master's newest DEV_STATE names (0: none yet) */
    uint32_t dev_named_states;   /* ... and its states_crc */
    uint32_t dev_head_seq;       /* LINK-026: the frame that head came in */
    bool dev_want;               /* the records' CRC differs: DEV_STATE_REQ due at dev_next_ms */
    uint64_t dev_next_ms;
    uint32_t dev_gap_ms;
    uint64_t dev_req_ms;         /* the last DEV_STATE_REQ (0: none): the next 1 s on at least */
    uint64_t dev_heard_ms;       /* the last device data under the installed list (0: none) */
    bool dev_fresh;              /* ... within PLINK_DEV_STALE_MS (the tick clears it) */
    uint32_t dev_seq;            /* moves with every change the platform copies */
    uint32_t dev_list_seq;       /* moves at every install */
    plink_kact_t acts[PLINK_DEV_ACTS_MAX];
    plink_kprefs_t prefs;        /* settings (LINK-050, LINK-051) */
    plink_kfw_t upd;             /* updates through the link (LINK-029..037) */
    plink_krb_t rb;              /* the remote backend (LINK-058) */
    plink_auto_warn_t auto_warn; /* LINK-056: the master's newest arming warnings ... */
    uint64_t auto_heard_ms;      /* ... when they came (0: none) */
    uint32_t auto_sid, auto_seq; /* ... and the frame they came in: an older one never replaces it */
    plink_out_t out;             /* frame scratch: pairing's answers and sealed frames */
    uint8_t pt[PLINK_FRAME_MAX]; /* an opened frame's payload */
    union {
        plink_status_t st;
        alarm_event_t ev[PLINK_EVENTS_KEPT];
    } scr;                       /* decode scratch */
} plink_keypad_t;

/* rec: the stored record, or NULL when not paired. cfg_buf (ALARM_CONFIG_BLOB_MAX bytes) and cfg
 * (an alarm_config_t) are the caller's memory (PSRAM on a panel). name, fw, kind and cap describe
 * this keypad for pairing and HELLO; caps are the capability bits it offers there (PLINK_CAP_*,
 * masked with PLINK_CAPS_KNOWN; LINK-043): each session keeps those its master offers too.
 * PLINK_CAP_HOME is offered only once a mirror is attached (plink_keypad_home_attach). */
void plink_keypad_init(plink_keypad_t *k, const plink_crypto_t *c, const plink_keypad_port_t *port,
                       const plink_keypad_rec_t *rec, uint8_t *cfg_buf, size_t cfg_cap,
                       alarm_config_t *cfg, uint8_t kind, const plink_caps_t *cap, uint16_t caps,
                       const char *name, const char *fw, uint64_t now_ms);

/* Where the master is now (discovery: mDNS, set by hand, or the subnet's broadcast when mDNS
 * finds nothing). A paired keypad sends its HELLO there; its address moves only on an
 * authenticated WELCOME or sealed frame. broadcast: addr is the subnet's broadcast (ip | ~netmask,
 * which its bytes don't tell; 255.255.255.255 always is one), so the WELCOME may come from any
 * address (LINK-021). */
void plink_keypad_master_addr(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *addr,
                              bool broadcast);
void plink_keypad_input(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *from,
                        const uint8_t *frame, size_t len);
void plink_keypad_tick(plink_keypad_t *k, uint64_t now_ms);
uint64_t plink_keypad_next(const plink_keypad_t *k);
/* The keypad's own address changed (DHCP): a HEARTBEAT goes at once. */
void plink_keypad_addr_changed(plink_keypad_t *k, uint64_t now_ms);
/* Power and signal for the heartbeat. */
void plink_keypad_set_power(plink_keypad_t *k, uint8_t flags, int8_t rssi);

/* ---- pairing (a person on the keypad) ---- */
/* master and broadcast as plink_pk_start's: asked by broadcast, the first master to answer is
 * the attempt's. */
void plink_keypad_pair_start(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *master,
                             bool broadcast, uint16_t net_id);
void plink_keypad_pair_retry(plink_keypad_t *k, uint64_t now_ms);
void plink_keypad_pair_confirm(plink_keypad_t *k, uint64_t now_ms, bool match);
void plink_keypad_pair_cancel(plink_keypad_t *k, uint64_t now_ms);
void plink_keypad_pair_info(const plink_keypad_t *k, uint64_t now_ms, plink_pair_info_t *out);

/* ---- what the UI shows ---- */
typedef struct {
    uint8_t state;            /* plink_link_state_t */
    uint8_t unpaired_why;     /* plink_unpaired_t when NOT_PAIRED */
    bool not_paired_hint;     /* the master has said "not paired" for 5 min */
    uint32_t since_s;         /* seconds in this state */
    uint32_t status_age_s;    /* UINT32_MAX none yet */
    bool maintenance;         /* LINK-025: 10 min without system communication, or not paired */
    uint8_t device;
    uint8_t selected; /* active partition; home when no temporary selection */
    uint8_t home;
    char master_name[PLINK_NAME_MAX + 1];
    plink_addr_t master;
    uint16_t caps;            /* the live session's common capabilities (0: none, or no session) */
    /* The configuration the latest status names, when this firmware couldn't read it (0 none):
     * the keypad needs an update. Forgotten once another configuration installs or the master
     * names the installed one again. */
    uint32_t cfg_bad_crc;
    bool switch_allowed;
    bool return_never;
    uint16_t return_s; /* 0 = default 30 seconds */
} plink_keypad_info_t;

void plink_keypad_info(const plink_keypad_t *k, uint64_t now_ms, plink_keypad_info_t *out);
/* The rebuilt snapshot (snap.seq == plink_keypad_seq) and the counters the UI compares. */
const alarm_snapshot_t *plink_keypad_snapshot(const plink_keypad_t *k);
uint32_t plink_keypad_seq(const plink_keypad_t *k);
uint32_t plink_keypad_config_seq(const plink_keypad_t *k);
/* The installed configuration, NULL before the first. */
const alarm_config_t *plink_keypad_config(const plink_keypad_t *k);
/* The newest events, newest first; returns how many. */
size_t plink_keypad_events(const plink_keypad_t *k, alarm_event_t *out, size_t max);
/* A one-shot sound from the master to play now (false: none waiting). */
bool plink_keypad_take_sound(plink_keypad_t *k, plink_sound_t *out);
/* The master's time when it changed since the last call (false: no change). */
bool plink_keypad_take_time(plink_keypad_t *k, plink_time_t *out);
/* The master's epoch now, from the last status (0 not known). */
int64_t plink_keypad_epoch_ms(const plink_keypad_t *k, uint64_t now_ms);
/* The master's engine time now (an event's mono_ms clock), from the last status (0 not known:
 * none yet, or a master that sends none). */
uint64_t plink_keypad_mono_ms(const plink_keypad_t *k, uint64_t now_ms);
/* The master's reporting state (PLINK_REPORT_*). */
uint8_t plink_keypad_report(const plink_keypad_t *k);
/* The newest STATUS's flags as sent (PLINK_SF_*: FIRE and CO sound on any of the master's
 * partitions, UPDATING and HOLD; LINK-030, LINK-039); 0 before the first. */
uint8_t plink_keypad_master_flags(const plink_keypad_t *k);
/* The continuous pattern to play (alarm_sound_t): fire, then CO, from the STATUS flags or any
 * partition held, else the home partition's (plink_view_continuous). */
uint8_t plink_keypad_continuous(const plink_keypad_t *k);

/* ---- commands ---- */
/* Sends a command (ops of plink_op_t; req filled in here). Returns its req, or 0 when it was
 * refused at once: the link isn't ONLINE and it isn't a panic (LINK-023), or no slot is free. */
uint32_t plink_keypad_command(plink_keypad_t *k, uint64_t now_ms, const plink_cmd_t *cmd);
/* Where request req stands; *res filled when DONE. */
plink_cmd_state_t plink_keypad_command_state(const plink_keypad_t *k, uint32_t req,
                                             plink_result_t *res);
/* The caller stopped waiting (the UI's 3 s): the slot frees once answered or given up; a panic
 * keeps being sent. */
void plink_keypad_command_release(plink_keypad_t *k, uint32_t req);
/* A panic is still being sent (for the Emergency words). */
bool plink_keypad_panic_pending(const plink_keypad_t *k);

/* ---- Smart Home: the master's devices (LINK-043..047) ---- */
/* The mirror's memory (NULL: none - a keypad built without home control, or without the memory).
 * Only with one does the keypad offer PLINK_CAP_HOME. Call it right after plink_keypad_init,
 * before the first tick: a session already up keeps the offer its HELLO made. */
void plink_keypad_home_attach(plink_keypad_t *k, plink_dev_mirror_t *mem);

typedef struct {
    bool attached;     /* a mirror: this keypad offers PLINK_CAP_HOME */
    bool shared;       /* the live session holds HOME: the master serves it devices */
    uint32_t list_crc; /* the installed list (0: none yet - Smart Home appears once it is in) */
    uint8_t count;     /* its rows */
    uint32_t list_seq; /* moves each time another list installs */
    uint32_t seq;      /* moves with every change to copy: list, record, action, freshness */
    bool fresh;        /* LINK-046: ONLINE, with device data within 30 s; otherwise every row reads
                        * offline since heard_ms, and an action sends nothing */
    uint64_t heard_ms; /* the last device data, on the keypad's clock (0: none); it moves without
                        * seq while the data stays fresh (nothing to redraw then), so a copy made
                        * at seq reads it again itself (a summary every 10 s) */
} plink_dev_info_t;

void plink_keypad_dev_info(const plink_keypad_t *k, uint64_t now_ms, plink_dev_info_t *out);
/* Row index of the installed list, in the master's order: its catalog fields (zeros, with *usable
 * false, for a row this firmware can't use; either may be NULL). false past the last row. */
bool plink_keypad_dev_row(const plink_keypad_t *k, uint8_t index, plink_dev_cat_t *cat,
                          bool *usable);
/* Row index's record (false: none has come yet). */
bool plink_keypad_dev_rec(const plink_keypad_t *k, uint8_t index, plink_dev_rec_t *rec);
/* Row index's overlay (plink_dev_overlay_t) and the action it is about (*action, may be NULL). */
uint8_t plink_keypad_dev_overlay(const plink_keypad_t *k, uint8_t index, uint8_t *action);

/* Row index as the keypad's screen shows it (docs/features/11 "Device actions" step 7, LINK-046):
 * the master's record, with this keypad's own action over it once its caller stopped waiting
 * (its overlay: pending while it is sent, failed for a late refusal, failed and unsure once given
 * up - "Not confirmed"), and offline while the device data isn't fresh. The screen's ui_dev_t is
 * made from these by ui_dev_from_parts, as the master's from its registry. */
typedef struct {
    plink_dev_cat_t cat; /* its catalog fields (zeros when !usable) */
    bool usable;         /* false: a row this firmware can't use (a ref the codecs refuse) */
    bool have_rec;       /* its record has come (none yet: offline, nothing known) */
    char word[PLINK_DEV_WORD_MAX + 1]; /* the source's state word */
    dev_values_t values;               /* what the record knows (plink_home_values_of) */
    bool online;          /* false: offline - its record says so, it has none yet, or stale */
    bool since_last_data; /* offline because the device data isn't fresh (not ONLINE, or none for
                           * 30 s): since the last device data (plink_dev_info_t.heard_ms), not
                           * values.since - unless its record already said offline since a time */
    bool pending;         /* an action under way: the record's, or this keypad's still being sent */
    bool failed;          /* the last action failed: the record's, or this keypad's */
    bool unsure;          /* ... and whether it happened isn't known (HCTL-012) */
    uint8_t action;       /* dev_action_t pending or failed (ACT_NONE: none) */
} plink_dev_view_t;

/* Row index of the installed list for the screen (false, *out zeroed: past the last row or no list
 * installed). */
bool plink_keypad_dev_view(const plink_keypad_t *k, uint64_t now_ms, uint8_t index,
                           plink_dev_view_t *out);

/* A device action, naming its device by ref (LINK-047; docs/features/11 "Device actions"): action
 * a dev_action_t, value its value, mode one PLINK_DEV_MODE_* bit for ACT_SET_MODE (ignored
 * otherwise), code the user's code or NULL. Returns PLINK_DEV_OK with *ticket (its req, not 0)
 * when it goes: its first copy now, then every 500 ms with the same req until answered or 3 s
 * have passed, never after; its code is wiped after its last copy. Otherwise a plink_dev_result_t
 * at once, with nothing sent: UNAVAILABLE without a mirror; UNREACHABLE when the link isn't
 * ONLINE, the session doesn't share HOME on a transport of 250 B or more, the device data is stale
 * (LINK-046), or the row is offline (or has no record yet); REFUSED for a ref the installed list
 * doesn't hold, a Z-Wave row (until BL-169), an action its kind doesn't take, a mode that isn't
 * one bit, or a code that isn't digits only; NEED_CODE when hc_needs_code and code is empty;
 * BUSY while an action on that device, or one in each slot, is still being sent. */
uint8_t plink_keypad_dev_act(plink_keypad_t *k, uint64_t now_ms, const char *ref, uint8_t action,
                             int32_t value, uint8_t mode, const char *code, uint32_t *ticket);

typedef struct {
    uint8_t result;  /* DONE: plink_dev_result_t (an unknown one reads REFUSED) */
    uint8_t why;     /* CODE_REFUSED: the alarm_result_t, so the pad says why */
    uint8_t overlay; /* plink_dev_overlay_t */
} plink_dev_answer_t;

/* Where action ticket stands: PENDING, DONE (*out: the master's answer; its record is already in
 * the mirror) or NO_ANSWER (given up after 3 s); NONE once its slot went to another action. */
plink_cmd_state_t plink_keypad_dev_act_state(const plink_keypad_t *k, uint32_t ticket,
                                             plink_dev_answer_t *out);
/* The caller stopped waiting: from now on its device shows the action's overlay - pending while
 * it is sent, failed for a refusal, failed and unsure once given up - until a later record for
 * that device or the next action on it. */
void plink_keypad_dev_act_release(plink_keypad_t *k, uint32_t ticket);

/* ---- settings (LINK-050..052) ---- */
/* The keypad's settings now: its panel id (6 lowercase hex characters; anything else the master
 * keeps as none), its voice (plink_voice_t), the fields it accepts (bit id - 1) and their values,
 * a fields body (proto_prefs_encode, count first; at most PLINK_PREFS_OWN_MAX). Sent as PREFS in a
 * session with PLINK_CAP_PREFS: at its first STATUS, after a change at most once a second, and on
 * PREFS_REQ; every HEARTBEAT carries its prefs_crc. false: fields isn't a fields body. */
bool plink_keypad_prefs_report(plink_keypad_t *k, uint64_t now_ms, const char *panel_id,
                               uint8_t voice, uint32_t accepts, const uint8_t *fields, size_t len);
/* A settings change from the master to apply (false: none waiting): each PREFS_SET req is given
 * once. A repeat of it is ignored until it is answered, then answered again from that answer for
 * 30 s, so a change is never applied twice; another req waits while one is being applied (the
 * master's resend brings it back). */
bool plink_keypad_take_prefs_set(plink_keypad_t *k, plink_prefs_change_t *out);
/* The answer to change req: the fields refused, refused_count x (id u8, why u8:
 * plink_prefs_why_t; at most PLINK_PREFS_REFUSED_MAX are sent), and the values now stored (a
 * fields body, as for plink_keypad_prefs_report; NULL keeps the report's). Sent as PREFS at once
 * and kept 30 s for its repeats. false: a bad argument (fields that aren't a body were not taken;
 * the answer still went). */
bool plink_keypad_prefs_answer(plink_keypad_t *k, uint64_t now_ms, uint32_t req,
                               const uint8_t *refused, uint8_t refused_count, const uint8_t *fields,
                               size_t len);

/* ---- updates through the link (LINK-029..037) ---- */
/* The next order for the service, in the order they came (false: none waiting): what to do is
 * out->op.
 *   - INFO: the keypad's facts may be asked for again (plink_keypad_fw_info); FW_INFO goes anyway.
 *   - PROBE: one check at out->server and out->channel with want = out->version; the answer
 *     through plink_keypad_fw_probe.
 *   - FETCH: the release run for exactly out->version at out->server and out->channel; a new
 *     RELAY: open the writer for that image (its size, sha256 and key), then report RECEIVING for
 *     the pull to start, or FAILED. Either one ends whatever run the service had (out->order names
 *     the new run). The order's server and key become the keypad's trust record when it differs
 *     (LINK-036): no person is asked.
 *   - INSTALL: the gate may restart into the READY image if plink_keypad_fw_install_for names it.
 *   - CANCEL: stop and discard the run and any READY image. out->order 0 is the core's own: the
 *     run ended here (stalled, timed out, the READY image kept 24 h, the keypad removed or paired
 *     again) - the same.
 * A RELAY that continues the run (the same id8 and size) isn't handed out. Orders wait in a queue
 * of PLINK_FW_QUEUE: one that finds it full isn't taken (nor answered), and the master's resend
 * brings it back. */
bool plink_keypad_take_fw_order(plink_keypad_t *k, plink_fw_order_t *out);

/* The service's run as it stands (run: the order that started it; a report about another run, or
 * one that ended, is ignored): state a plink_fw_run_state_t, why a plink_fw_why_t. RECEIVING on a
 * relay starts the pull (the writer is open), FETCHING, VERIFYING, READY and INSTALLING hold it;
 * FAILED, STOPPED and IDLE end the run - a FAILED one that never got READY counts toward the
 * budget. PROBING and KEY_WAIT are ignored. Each change of state or why goes as FW_STATE at
 * once. */
void plink_keypad_fw_report(plink_keypad_t *k, uint64_t now_ms, uint32_t run, uint8_t state,
                            uint8_t why);
/* A fetch's progress: bytes of size, and the image's id8 once known (NULL keeps it). FW_STATE
 * for bytes goes at most once a second. A relay's bytes are the core's: ignored for one. */
void plink_keypad_fw_progress(plink_keypad_t *k, uint64_t now_ms, uint32_t run, uint32_t bytes,
                              uint32_t size, const uint8_t *id8);
/* A PROBE's answer: why (plink_fw_why_t; OK when the server offers one), the version offered
 * ("" none), the key it is signed with (key8, NULL: not known) and whether the keypad trusts that
 * key. FW_STATE's probe section from now on (asked 1, age_s counted from now). */
void plink_keypad_fw_probe(plink_keypad_t *k, uint64_t now_ms, uint8_t why, const char *offered,
                           const uint8_t *key8, bool key_trusted);
/* FW_STATE's last section: probation (1 while the new image's self-test runs), the last result
 * (plink_fw_last_t) for version, and the health bits that failed. */
void plink_keypad_fw_last(plink_keypad_t *k, uint64_t now_ms, uint8_t probation, uint8_t last,
                          const char *version, uint8_t failed_mask);
/* The keypad's facts for FW_INFO: sent at each session's first STATUS, after a change and on
 * INFO, at most once every 2 s. The core clears PLINK_FWI_RELAY without a writer and sets
 * PLINK_FWI_CFG_UNREADABLE while the newest status names a configuration it couldn't read; a
 * RELAY larger than info->slot is refused (TOO_BIG), a FETCH or RELAY for a version that isn't
 * newer than info->version too (NOT_NEWER). */
void plink_keypad_fw_info(plink_keypad_t *k, uint64_t now_ms, const plink_fw_info_t *info);
/* The service's part of the gate (a kept fire or CO tone: KEPT_TONE; 0: it passes): the relay
 * pauses while it says no, and the service checks it again before a restart. */
void plink_keypad_fw_gate(plink_keypad_t *k, uint64_t now_ms, uint8_t why);
/* The link's part of the INSTALL gate (LINK-034): 0 when it passes, else why not - NO_STATUS (no
 * live session with a STATUS under 15 s old: ONLINE or the needs-update state), NOT_QUIET (the
 * master's HOLD, or proto_fw_install_allowed() says no on the rebuilt snapshot: anything armed, an
 * alarm), PANIC_PENDING (a panic still being sent). The master's UPDATING doesn't hold it: keypads
 * restart with their master. */
uint8_t plink_keypad_fw_hold(const plink_keypad_t *k, uint64_t now_ms);
/* Whether the master's INSTALL for version has arrived (the INSTALL hold, LINK-034): an INSTALL
 * naming exactly it, since the run started, in this session, and no order but INFO since. */
bool plink_keypad_fw_install_for(const plink_keypad_t *k, const char *version);
/* What FW_STATE says now (for the screen and the console). */
void plink_keypad_fw_status(const plink_keypad_t *k, uint64_t now_ms, plink_fw_state_t *out);
/* LINK-029 (Decisions, keypad updates, 5: never past its master): the version the master runs, as
 * the paired master's authenticated INFO last named it - at each session and on its
 * confirmed-version change; only INFO for the application (kind 0), in frame order; an INFO with
 * no version (the master on its self-test) names nothing, nor does any other order (they name the
 * master's target, which may be ahead of what it runs). A newer one replaces it (the master was
 * updated), an older one too (it was put back by cable, or went back: the cap follows the
 * master). "" while none is known: never heard since pairing, or cleared by removal, a new pairing
 * or a session without PLINK_CAP_FW (a master that names no version) - then the keypad offers no
 * Install while paired. *seq (NULL: not wanted) differs from its last value at each change of the
 * cap below: of this version (the clearing included), and at each pairing and each removal even
 * while none is known; restoring doesn't move it. Whenever *seq moves the service stores the
 * version with the pairing's net_id (NVS link/mver, erased when "": plink_keypad_version_cap gives
 * both) and applies the cap - so a restart writes nothing. Never NULL. */
const char *plink_keypad_master_version(const plink_keypad_t *k, uint32_t *seq);
/* The cap on every update of this keypad (docs/features/11 "Never past its master";
 * panel_ota_set_version_cap), from its pairing and the version above:
 *   - false: no cap - not paired (never, removed, or made a stand-alone panel: no master);
 *   - true: paired - nothing past *version; "" while its master's version isn't known: nothing
 *     passes (no Install, every offer refused); a version the service can't parse: nothing passes.
 * *version (never NULL) and *net_id (the pairing's network, 0 unpaired) may be NULL: not wanted.
 * The service applies it right after plink_keypad_master_version_restore, whatever *seq says (a
 * restart moves no seq: the version restored is capped from the first moment), then whenever *seq
 * moves (a version named or cleared, a pairing, a removal). */
bool plink_keypad_version_cap(const plink_keypad_t *k, const char **version, uint16_t *net_id);
/* At start-up, right after plink_keypad_init: the version the service stored (NVS link/mver) and
 * the net_id stored with it, so the cap holds before the first INFO. Taken only while paired on
 * that network (a version of an earlier pairing - a power cut before the service erased it - is
 * not) and before any is heard; *seq doesn't move. */
void plink_keypad_master_version_restore(plink_keypad_t *k, const char *version, uint16_t net_id);

/* The trial with its master (LINK-034; docs/features/11 "On trial with the master"): the service
 * starts it at its start-up, right after plink_keypad_init, when it runs an image its master
 * ordered installed and is on probation (version: the one it runs now). From then on an INFO
 * naming version or a newer one passes it, one naming an older version fails it (the master went
 * back), and none within 10 min of now fails it. */
void plink_keypad_fw_trial(plink_keypad_t *k, uint64_t now_ms, const char *version);
/* Where the trial stands now (plink_fw_trial_t): NONE without one, WAIT, PASS or FAIL - the
 * service's self-test passes only on PASS and goes back on FAIL. */
uint8_t plink_keypad_fw_trial_state(const plink_keypad_t *k, uint64_t now_ms);

/* Forget the master (a person made it a stand-alone panel, or paired again): the record erased,
 * the session wiped. */
bool plink_keypad_forget(plink_keypad_t *k, uint64_t now_ms);

/* Request/retry only while enrolled and in an authenticated WIFI session. Stop
 * before manual network selection so a late reply cannot overwrite it. */
bool plink_keypad_wifi_request(plink_keypad_t *k, uint64_t now);
void plink_keypad_wifi_cancel(plink_keypad_t *k);

/* LINK-055: this keypad's wired terminals to its master (IO_STATE), in a live session holding
 * PLINK_CAP_IO. false: no such session now (the platform sends again at its next change or
 * period). */
bool plink_keypad_io_state(plink_keypad_t *k, uint64_t now, const plink_io_state_t *st);
/* A live session holding PLINK_CAP_IO is up (the platform sends its whole state when it comes). */
bool plink_keypad_io_ready(const plink_keypad_t *k);

/* LINK-056: the master's arming warnings as its newest AUTO_WARN said, with each record's
 * arm_in_ms counted down to now. false (*out empty): none heard in this pairing, or the newest is
 * older than PLINK_AUTO_STALE_MS (the master stopped saying them). */
bool plink_keypad_auto_warn(const plink_keypad_t *k, uint64_t now_ms, plink_auto_warn_t *out);

/* The remote backend (LINK-058, LINK-063; docs/features/11 "The master's settings and installer
 * menu on keypads"): one call of the master's ui_backend_t at a time. args and ans are the
 * caller's (ui_remote's UR_ARGS_MAX and UR_ANSWER_MAX; PSRAM is fine), kept until the keypad goes.
 * A keypad that offers PLINK_CAP_RB attaches them before it connects. */
void plink_keypad_rb_attach(plink_keypad_t *k, uint8_t *args, size_t args_cap, uint8_t *ans,
                            size_t ans_cap);
/* Starts a call: its arguments (len bytes, copied unless they already are the attached buffer)
 * go in parts at the next ticks, again from the start after 1 s without a word, until the answer
 * is in or PLINK_RB_GIVE_UP_MS (PLINK_RB_GIVE_UP_LONG_MS for arguments in parts) has passed.
 * *req names it for plink_keypad_rb_poll. false: one runs, nothing attached, or the session
 * doesn't hold PLINK_CAP_RB on a transport of 250 B frames. */
bool plink_keypad_rb_start(plink_keypad_t *k, uint64_t now_ms, uint16_t call, const uint8_t *args,
                           size_t len, uint32_t *req);
/* PLINK_RB_PENDING while it runs; then its status once (the answer, *len bytes, in the attached
 * buffer until the next call; PLINK_RB_NO_ANSWER when none came), and the call is over: its
 * arguments are wiped. Another req: PLINK_RB_GONE. */
#define PLINK_RB_PENDING 0xFFu
uint8_t plink_keypad_rb_poll(plink_keypad_t *k, uint32_t req, size_t *len);
/* What the master holds for this keypad, as its newest RB_GRANT said; false: none yet. */
bool plink_keypad_rb_grant(const plink_keypad_t *k, plink_rb_grant_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_KEYPAD_H */
