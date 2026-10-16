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

/* One configuration request out (private). */
typedef struct {
    bool used;
    uint32_t off;
    uint16_t max;
    uint64_t due_ms;   /* sent again then */
    uint32_t retry_ms; /* 300 ms, doubling to 2 s */
} plink_kpull_t;

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
    char name[PLINK_NAME_MAX + 1];
    char fw[PLINK_FW_MAX + 1];
    bool have_master;
    plink_addr_t master;         /* where sealed frames go: moved only by authenticated ones */
    bool have_disc;
    plink_addr_t disc;           /* where discovery last found the master: HELLOs go there */
    plink_session_t live;
    bool tentative;              /* the live session waits for its first frame from the master */
    plink_session_t old;         /* still accepted while tentative */
    plink_hs_k_t hs;
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
    uint8_t *cfg_buf;            /* the blob being pulled (caller's memory) */
    size_t cfg_cap;
    bool cfg_pulling;
    uint32_t cfg_pull_crc, cfg_total, cfg_have; /* cfg_have: bytes in from offset 0 */
    uint32_t cfg_ahead_off, cfg_ahead_len;      /* one page in past the first gap */
    plink_kpull_t cfg_req[PLINK_PULL_OUTSTANDING];
    uint32_t cfg_bad_crc;        /* a blob that failed to decode: not pulled again */
    uint64_t cfg_next_ms;        /* the pull's first requests (0-500 ms after the change) */
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
    plink_out_t out;             /* frame scratch: pairing's answers and sealed frames */
    uint8_t pt[PLINK_FRAME_MAX]; /* an opened frame's payload */
    union {
        plink_status_t st;
        alarm_event_t ev[PLINK_EVENTS_KEPT];
    } scr;                       /* decode scratch */
} plink_keypad_t;

/* rec: the stored record, or NULL when not paired. cfg_buf (ALARM_CONFIG_BLOB_MAX bytes) and cfg
 * (an alarm_config_t) are the caller's memory (PSRAM on a panel). name, fw, kind and cap describe
 * this keypad for pairing and HELLO. */
void plink_keypad_init(plink_keypad_t *k, const plink_crypto_t *c, const plink_keypad_port_t *port,
                       const plink_keypad_rec_t *rec, uint8_t *cfg_buf, size_t cfg_cap,
                       alarm_config_t *cfg, uint8_t kind, const plink_caps_t *cap,
                       const char *name, const char *fw, uint64_t now_ms);

/* Where the master is now (discovery: mDNS, a broadcast answer, or set by hand). A paired keypad
 * sends its HELLO there; its address moves only on an authenticated WELCOME or sealed frame. */
void plink_keypad_master_addr(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *addr);
void plink_keypad_input(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *from,
                        const uint8_t *frame, size_t len);
void plink_keypad_tick(plink_keypad_t *k, uint64_t now_ms);
uint64_t plink_keypad_next(const plink_keypad_t *k);
/* The keypad's own address changed (DHCP): a HEARTBEAT goes at once. */
void plink_keypad_addr_changed(plink_keypad_t *k, uint64_t now_ms);
/* Power and signal for the heartbeat. */
void plink_keypad_set_power(plink_keypad_t *k, uint8_t flags, int8_t rssi);

/* ---- pairing (a person on the keypad) ---- */
void plink_keypad_pair_start(plink_keypad_t *k, uint64_t now_ms, const plink_addr_t *master,
                             uint16_t net_id);
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
    bool maintenance;         /* LINK-025: 10 min without system communication */
    uint8_t device;
    uint8_t home;
    char master_name[PLINK_NAME_MAX + 1];
    plink_addr_t master;
    uint32_t cfg_bad_crc;     /* a configuration this firmware couldn't read (0 none) */
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
/* The master's reporting state (PLINK_REPORT_*). */
uint8_t plink_keypad_report(const plink_keypad_t *k);

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

/* Forget the master (a person made it a stand-alone panel, or paired again): the record erased,
 * the session wiped. */
bool plink_keypad_forget(plink_keypad_t *k, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_KEYPAD_H */
