/*
 * link_node.h - the PC's virtual master and virtual keypad (host/link_sim; docs/features/11
 * LINK-013): the portable link managers (plink_master_t, plink_keypad_t) wired the way a panel
 * wires them, but with no socket here: frames come in through *_input, leave through the send
 * callback, and time comes in as ms. link_sim drives them on the real clock over UDP; link_smoke
 * drives both on a virtual clock over 127.0.0.1.
 *
 * The virtual master runs the real alarm engine on a small house:
 *   zone 1 "Front Door" (entry/exit 1), 2 "Living Window" (perimeter), 3 "Hall Motion"
 *   (interior follower), 4 "Hall Smoke" (fire), all in partition 1 "Home";
 *   users 01 installer 69420 and 02 master 123456 (the factory codes), 03 "Alex" 1111.
 * A keypad's command runs as {ALARM_SRC_KEYPAD, n} after the link manager hands it over, its
 * supervision edges raise and clear the engine's device trouble (alarm_engine_device_reachable),
 * the keypads' configuration copy comes from plink_cfg_for_keypads, and every engine event and
 * one-shot sound goes to the link before the STATUS that follows it.
 *
 * Records (the master's keypads, the keypad's master) live in a link_store_t: a file for link_sim
 * (the record holds the pairing keys: a bench tool's file, not a panel's NVS) or memory only.
 */
#ifndef LINK_NODE_H
#define LINK_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_engine.h"
#include "alarm/alarm_snapshot.h"
#include "panel_link/plink_keypad.h"
#include "panel_link/plink_master.h"

#if defined(__GNUC__) || defined(__clang__)
#define LINK_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define LINK_PRINTF(f, a)
#endif

/* One frame to a peer (UDP for link_sim and link_smoke). */
typedef bool (*link_send_fn)(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len);
/* One line to show (no line end). */
typedef void (*link_say_fn)(void *ctx, const char *line);

typedef struct {
    link_send_fn send;
    void *send_ctx;
    link_say_fn say; /* NULL: quiet */
    void *say_ctx;
    /* The wall clock in ms; NULL: a fixed virtual epoch plus the node's ms (link_smoke). */
    int64_t (*epoch_ms)(void);
} link_io_t;

/* Where a record is kept: in a file (path) and in memory, or in memory only (path NULL). */
typedef struct {
    const char *path;
    uint8_t buf[PLINK_STORE_MAX];
    size_t len; /* 0: nothing stored */
    unsigned writes, erases, failures;
} link_store_t;

/* Reads the file when there is one; false when it exists but couldn't be read whole. */
bool link_store_init(link_store_t *s, const char *path);
bool link_store_write(link_store_t *s, const uint8_t *buf, size_t len);
bool link_store_erase(link_store_t *s);

/* "123456" -> "123 456" (out holds 8 bytes). */
void link_code_text(const char *code, char out[8]);
const char *link_fail_name(uint8_t reason);
const char *link_state_text(uint8_t state); /* plink_link_state_t */

/* ---- the virtual master -------------------------------------------------------------------- */
#define LINK_MASTER_EVENTS 256u
#define LINK_MASTER_FRESH  64u
#define LINK_MASTER_RUNS   8u

typedef struct {
    bool used;
    uint32_t ticket;
    uint8_t device;
    plink_cmd_t cmd;
} link_run_t;

typedef struct {
    uint32_t ticket;
    plink_result_t res;
} link_answer_t;

typedef struct {
    /* private */
    const plink_crypto_t *c;
    link_io_t io;
    link_store_t *store;
    uint64_t now;
    alarm_engine_t eng;
    alarm_config_t cfg;
    alarm_config_t scratch;
    uint8_t blob[2][ALARM_CONFIG_BLOB_MAX]; /* the keypads' copy: the link keeps the last one */
    unsigned blob_cur;
    uint32_t cfg_gen;
    bool cfg_known;
    alarm_snapshot_t snap;
    uint32_t snap_seq;
    bool dirty;
    alarm_event_t ring[LINK_MASTER_EVENTS]; /* the newest events, oldest first, for EV_REQ */
    size_t ring_head, ring_count;
    alarm_event_t fresh[LINK_MASTER_FRESH];  /* not yet handed to the link */
    size_t fresh_count;
    plink_sound_t sounds[16];
    size_t sound_count;
    link_run_t runs[LINK_MASTER_RUNS];
    link_answer_t answers[LINK_MASTER_RUNS];
    size_t answer_count;
    plink_master_t m;
    uint8_t said_pair;
    char said_code[PLINK_CODE_LEN];
    uint32_t down;              /* keypads the supervision port said are down (bit d-1) */
    unsigned reachable_calls;
} link_master_t;

/* name: shown on keypads (NULL "PC master"); panel_id: six hex characters (NULL "0ac5e1"). The
 * stored record, if any, comes from store. false: the engine refused the house (never). */
bool link_master_init(link_master_t *v, const plink_crypto_t *c, const link_io_t *io,
                      link_store_t *store, const char *name, const char *panel_id,
                      uint64_t now_ms);
void link_master_input(link_master_t *v, uint64_t now_ms, const plink_addr_t *from,
                       const uint8_t *frame, size_t len);
/* The engine's and the link's timers when due, then everything owed to the link. */
void link_master_tick(link_master_t *v, uint64_t now_ms);
uint64_t link_master_next(const link_master_t *v);
/* After calling plink_master_* directly: runs what the link handed over and says what changed. */
void link_master_sync(link_master_t *v, uint64_t now_ms);
/* A command from the master's own console, as the service console's keypad. */
alarm_result_t link_master_command(link_master_t *v, uint64_t now_ms, const alarm_cmd_t *cmd);
void link_master_zone(link_master_t *v, uint64_t now_ms, uint16_t zone, bool open);
void link_master_say_status(link_master_t *v, uint64_t now_ms);
void link_master_say_keypads(link_master_t *v, uint64_t now_ms);

/* ---- the virtual keypad -------------------------------------------------------------------- */
typedef struct {
    uint32_t req;
    char what[40];
} link_pending_t;

typedef struct {
    /* private */
    const plink_crypto_t *c;
    link_io_t io;
    link_store_t *store;
    plink_keypad_t k;
    uint8_t cfg_buf[ALARM_CONFIG_BLOB_MAX];
    alarm_config_t cfg;
    bool said_any;
    uint8_t said_state;
    uint8_t said_why;
    uint8_t said_pair;
    char said_code[PLINK_CODE_LEN];
    uint32_t said_ev_seq;
    uint32_t said_cfg_seq;
    uint8_t said_part_state[ALARM_MAX_PARTITIONS];
    uint8_t said_part_level[ALARM_MAX_PARTITIONS];
    link_pending_t pending[PLINK_CMDS_MAX];
    uint64_t now;
    bool have_find;     /* where to look for the master (port.resolve) */
    plink_addr_t find;
} link_keypad_t;

/* name: what the keypad calls itself (NULL "PC keypad"); master: where to look for the master
 * (an address, or 255.255.255.255 to ask by broadcast; port.resolve looks there again), NULL
 * when not known (a paired keypad then says HELLO to no one until link_keypad_master). */
void link_keypad_init(link_keypad_t *v, const plink_crypto_t *c, const link_io_t *io,
                      link_store_t *store, const char *name, const plink_addr_t *master,
                      uint64_t now_ms);
void link_keypad_master(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *master);
/* A person started pairing with the master at that address. 255.255.255.255 asks every master
 * on the LAN: the first to answer is paired with, and a second one stops it. */
void link_keypad_pair(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *master);
void link_keypad_input(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *from,
                       const uint8_t *frame, size_t len);
void link_keypad_tick(link_keypad_t *v, uint64_t now_ms);
uint64_t link_keypad_next(const link_keypad_t *v);
/* Says what changed since the last call: the link state, pairing, the partitions, new events,
 * sounds, and the answers to commands sent with link_keypad_command. */
void link_keypad_report(link_keypad_t *v, uint64_t now_ms);
/* Sends a command for the home partition and remembers it under `what` for the report; returns
 * its req (0: refused at once - not ONLINE and not a panic, or no slot). */
uint32_t link_keypad_command(link_keypad_t *v, uint64_t now_ms, plink_cmd_t *cmd,
                             const char *what);
uint8_t link_keypad_home(const link_keypad_t *v, uint64_t now_ms);
void link_keypad_say_status(link_keypad_t *v, uint64_t now_ms);

#endif /* LINK_NODE_H */
