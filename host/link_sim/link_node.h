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
 *
 * Smart Home and settings on keypads (BL-216; docs/features/11 "Smart Home and Home Assistant on
 * keypads"): both sides offer PLINK_CAP_HOME and PLINK_CAP_PREFS. The virtual master's devices are
 * home_control's real registry over one fake source, a small house's Home Assistant:
 *   light.porch "Porch Light" (Outside, dimmable, on at 80 %), light.kitchen "Kitchen Lights"
 *   (Kitchen, off), lock.front_door "Front Door" (Living room, locked), cover.garage_door "Garage
 *   Door" (Outside, closed), climate.hallway "Hallway" (Living room, heating to 21.5 from 21.0),
 *   scene.good_night "Good Night" (Living room).
 * The source answers an action LINK_HOUSE_MS after it was sent. The registry's list and records go
 * to the link as the plink_dev task builds them (when the registry moves, at once after an action,
 * every 2 s to compare), and a keypad's device action is judged as device_service_person_act
 * judges it: by ref, a Z-Wave row or an action its kind doesn't take refused, the code an unlock or
 * an opening needs checked by the engine as {ALARM_SRC_KEYPAD, n} for the keypad's home partition
 * (arm-only and guest codes refused), a thermostat's mode bit made its word (hc_mode_word). A
 * settings write (link_master_prefs_set) goes as Home Assistant's would, from the master's console.
 * The virtual keypad attaches a mirror (the master's devices, as its screen would read them) and
 * keeps its own Screen and sound settings, the factory ones of a panel without a voice: it reports
 * them (PREFS) and takes a write from the master (PREFS_SET) as its screen would - each field
 * checked (proto_prefs), all held during an alarm, entry or exit time on its home partition
 * (UI-046) - and answers with the values it keeps then.
 */
#ifndef LINK_NODE_H
#define LINK_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_engine.h"
#include "alarm/alarm_snapshot.h"
#include "alarm_proto/proto_prefs.h"
#include "home_control/home_control.h"
#include "panel_link/plink_home.h"
#include "panel_link/plink_keypad.h"
#include "panel_link/plink_master.h"
#include "ui_remote/ui_remote.h"

#if defined(__GNUC__) || defined(__clang__)
#define LINK_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define LINK_PRINTF(f, a)
#endif

/* One frame to a peer (UDP for link_sim and link_smoke). */
typedef bool (*link_send_fn)(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len);
/* One line to show (no line end). */
typedef void (*link_say_fn)(void *ctx, const char *line);

struct link_mfw; /* link_fw.h */
struct link_kfw;

typedef struct {
    link_send_fn send;
    void *send_ctx;
    link_say_fn say; /* NULL: quiet */
    void *say_ctx;
    /* The wall clock in ms; NULL: a fixed virtual epoch plus the node's ms (link_smoke). */
    int64_t (*epoch_ms)(void);
    /* Updates through the link (link_fw.h): the master's or the keypad's update state, set up with
     * link_mfw_setup or link_kfw_setup (and kept across its restarts); NULL: neither side offers
     * PLINK_CAP_FW. */
    struct link_mfw *mfw;
    struct link_kfw *kfw;
} link_io_t;

/* One line through io's say (quiet without one). */
void link_io_say(const link_io_t *io, const char *line);

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

/* ---- Smart Home and settings, both sides --------------------------------------------------- */
/* A dev_action_t's console word ("unlock"), and back (ACT_NONE: none such). */
const char *link_action_name(uint8_t action);
uint8_t link_action_parse(const char *word);
/* A plink_dev_result_t as words ("refused: a wrong code" needs why: the alarm_result_t). */
const char *link_dev_result_name(uint8_t result);
/* The fields v holds, as the console shows them: "brightness 80, saver_after 60 s, night off". */
void link_prefs_text(const proto_prefs_values_t *v, char *out, size_t cap);
/* A field named by its JSON key (proto_prefs.h) and a value as typed - a delay in seconds (0:
 * never), a switch on/off, a time of day HH:MM, the rest whole numbers - in the wire's units.
 * NULL when taken, else why not. */
const char *link_prefs_parse(const char *key, const char *text, uint8_t *id, uint32_t *value);

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

#define LINK_HOUSE_DEVICES 8u
#define LINK_HOUSE_ACTS    8u
#define LINK_HOUSE_MS      300u  /* the fake source answers an action this long after it is sent */
#define LINK_DEV_JOBS      8u    /* keypads' device actions waiting for the judge (port.dev_run) */
#define LINK_DEV_BUILD_MS  2000u /* the registry built and compared at least this often */

/* An action the fake source was sent: answered at due. */
typedef struct {
    uint32_t req; /* hc's request id; 0: free */
    char ref[DEV_REF_LEN];
    uint8_t action;
    int32_t value;
    char mode[DEV_MODE_LEN];
    uint64_t due;
} link_house_act_t;

/* The registry's one source: a fictional house's Home Assistant (private). */
typedef struct {
    dev_info_t dev[LINK_HOUSE_DEVICES];
    size_t n;
    link_house_act_t acts[LINK_HOUSE_ACTS];
    const dsrc_sink_t *sink;
    bool gone; /* its Home Assistant not connected: the registry empty */
} link_house_t;

/* A keypad's device action handed over by port.dev_run (private; the code wiped once judged). */
typedef struct {
    bool used;
    uint32_t ticket;
    uint8_t device, home;
    plink_dev_act_t act;
} link_dev_job_t;

/* How the last settings write ended (port.prefs_done). */
typedef struct {
    unsigned count; /* writes ended so far */
    uint8_t device;
    uint32_t req;
    uint8_t result; /* plink_pset_t */
    uint8_t refused_count;
    uint8_t refused[2u * PLINK_PREFS_REFUSED_MAX];
    proto_prefs_values_t values; /* OK: what the keypad keeps now (values.have: the fields) */
} link_pset_end_t;

/* The remote backend on the virtual master (link_rb.c; BL-223): its server, each keypad's grant
 * and what was last sent of it, the link's buffers, and a pretend sensor source of eight inputs
 * with a binding kept for each of zones 1..LINK_RB_ZONES. */
#define LINK_RB_ZONES 16u
typedef struct {
    ui_remote_server_t srv;
    ui_remote_grant_t grant[PLINK_KEYPADS_MAX];
    plink_rb_grant_t sent[PLINK_KEYPADS_MAX];
    uint8_t up[UR_ARGS_MAX];
    uint8_t ans[PLINK_KEYPADS_MAX][UR_ANSWER_MAX];
    uint8_t args[UR_ARGS_MAX];
    uint8_t out[UR_ANSWER_MAX];
    zone_hub_binding_t bind[LINK_RB_ZONES];
    bool bound[LINK_RB_ZONES];
    bool learning;
    uint16_t heard_input; /* the input opened while listening (0: none) */
    unsigned runs;        /* calls served */
} link_mrb_t;

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
    /* Smart Home and settings on keypads (BL-216) */
    hc_t hc; /* the registry, as device_service's */
    link_house_t house;
    bool dev_dirty;                       /* the registry moved: build at the next sync */
    uint64_t dev_build_at;                /* the next compare build */
    hc_device_t rows[HC_MAX_DEVICES];     /* the build's copy of the registry */
    uint8_t dblob[2][PLINK_DEV_BLOB_MAX]; /* the link keeps the last one handed in */
    unsigned dblob_cur;
    plink_dev_recs_t drecs;
    uint32_t dev_list_crc; /* the list handed in last (0: none yet) */
    size_t dev_count;
    link_dev_job_t jobs[LINK_DEV_JOBS];
    unsigned dev_runs; /* actions judged so far */
    uint32_t pset_req; /* the last settings write's req */
    link_pset_end_t pset_end;
    uint32_t said_report[PLINK_KEYPADS_MAX]; /* each keypad's settings report said (its count) */
    link_mrb_t rb; /* the remote backend (BL-223) */
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
/* The registry's devices as the keypads get them. */
void link_master_say_devices(link_master_t *v, uint64_t now_ms);
/* The house's device ref can't be reached (online false) or can again: false when there is no
 * such device. */
bool link_master_device_online(link_master_t *v, uint64_t now_ms, const char *ref, bool online);
/* The house's Home Assistant connected again, or not: then its registry is empty, as a panel's is
 * until Home Assistant's catalog comes (the keypads get the empty list). */
void link_master_house(link_master_t *v, uint64_t now_ms, bool connected);
/* Keypad device's settings as it last reported them. */
void link_master_say_prefs(link_master_t *v, uint64_t now_ms, uint8_t device);
/* A settings write to keypad device (LINK-051), as Home Assistant's would go through the master,
 * from its console (PLINK_ORIGIN_CONSOLE): field id (proto_prefs.h) to value, the wire's units.
 * false (said why) when refused at once; otherwise its end comes through port.prefs_done into
 * link_master_t.pset_end (its req: link_master_t.pset_req). */
bool link_master_prefs_set(link_master_t *v, uint64_t now_ms, uint8_t device, uint8_t id,
                           uint32_t value);
/* The house's device ref as the registry has it now (NULL: none). */
const hc_device_t *link_master_device(const link_master_t *v, const char *ref);

/* The remote backend (link_rb.c, BL-223): the master's runner (port.rb_run), a code accepted for
 * keypad device (its grant), the sensor opened while a keypad listens, and the grants' upkeep. */
void link_mrb_attach(link_master_t *v);
bool link_mrb_run(void *ctx, uint32_t ticket, uint8_t device, uint16_t call, const uint8_t *args,
                  size_t len);
void link_mrb_auth(link_master_t *v, uint8_t device, uint8_t user, uint64_t now);
void link_mrb_heard(link_master_t *v, uint16_t input);
void link_mrb_tick(link_master_t *v, uint64_t now);

/* ---- the virtual keypad -------------------------------------------------------------------- */
typedef struct {
    uint32_t req;
    char what[40];
} link_pending_t;

/* A device action sent from the keypad's console (private). */
typedef struct {
    uint32_t ticket; /* 0: none */
    char what[DEV_NAME_LEN + 24];
} link_dev_pending_t;

typedef struct {
    /* private */
    const plink_crypto_t *c;
    link_io_t io;
    link_store_t *store;
    plink_keypad_t k;
    plink_dev_mirror_t mirror; /* the master's devices (about 31 KB) */
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    proto_prefs_values_t prefs; /* its Screen and sound settings */
    uint32_t accepts;           /* the fields it takes (a panel without a voice) */
    unsigned prefs_applied;     /* settings changes from the master applied */
    link_dev_pending_t acts[PLINK_DEV_ACTS_MAX];
    uint32_t said_list_seq, said_dev_seq;
    bool said_fresh;
    uint64_t said_dev_set; /* bit i: said_dev[i] holds row i's last word */
    uint32_t said_dev[PLINK_DEV_MAX];
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
    uint8_t rb_args[UR_ARGS_MAX]; /* the remote backend's call (BL-223) */
    uint8_t rb_ans[UR_ANSWER_MAX];
} link_keypad_t;

/* The keypad's remote backend buffers, attached at each start (link_rb.c). */
void link_krb_attach(link_keypad_t *v);

/* name: what the keypad calls itself (NULL "PC keypad"); panel_id: its own panel id for its
 * settings report, six lowercase hex characters (NULL "1c2d3e"); master: where to look for the
 * master (an address, or 255.255.255.255 to ask by broadcast; port.resolve looks there again),
 * NULL when not known (a paired keypad then says HELLO to no one until link_keypad_master). */
void link_keypad_init(link_keypad_t *v, const plink_crypto_t *c, const link_io_t *io,
                      link_store_t *store, const char *name, const char *panel_id,
                      const plink_addr_t *master, uint64_t now_ms);
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
/* The master's devices as this keypad mirrors them, each row as its screen would show it. */
void link_keypad_say_devices(link_keypad_t *v, uint64_t now_ms);
/* A device action from this keypad (plink_keypad_dev_act; mode: a PLINK_DEV_MODE_* bit for
 * ACT_SET_MODE; code NULL or the user's code). Its answer is said by link_keypad_report; returns
 * its ticket, 0 when refused at once (said why). */
uint32_t link_keypad_dev_act(link_keypad_t *v, uint64_t now_ms, const char *ref, uint8_t action,
                             int32_t value, uint8_t mode, const char *code);
/* Its own settings, and a change made here (as on its screen: reported to the master). */
void link_keypad_say_prefs(link_keypad_t *v, uint64_t now_ms);
bool link_keypad_prefs_set(link_keypad_t *v, uint64_t now_ms, uint8_t id, uint32_t value);

#endif /* LINK_NODE_H */
