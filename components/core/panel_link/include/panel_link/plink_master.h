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
 */
#ifndef PLINK_MASTER_H
#define PLINK_MASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
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
/* The numbers the link holds (bit d-1). */
uint32_t plink_master_held(const plink_master_t *m);
/* Whether the link is in use (a keypad paired or pairing open): mDNS is announced only then. */
bool plink_master_in_use(const plink_master_t *m);
uint16_t plink_master_net_id(const plink_master_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_MASTER_H */
