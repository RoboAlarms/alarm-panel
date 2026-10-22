/*
 * plink_internal.h - shared between the files of plink_service (not a public API).
 *
 * One task, the link task, owns the socket, the role's link manager and everything the managers'
 * ports touch; the other tasks reach it through hand-overs that never make it wait:
 *   - requests (the UI's link_request, the console): one at a time, answered within 2 s
 *     (link_request);
 *   - on a master: the alarm listener's flag and queues, the worker's and plink_dev's results
 *     (plink_master_svc.c, plink_home_svc.c), the settings writes (plink_service_prefs_set);
 *   - on a keypad: commands and device actions from the UI, and the screen's answers to a settings
 *     change (plink_keypad_svc.c).
 * What the others read - the keypads, the pairing, the keypad's view of its master, the keypads'
 * settings reports - the link task publishes in g_pub under link_pub_lock(), and the UI, the
 * console and Home Assistant's link copy it from there.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ALARM_LINK

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_keypad.h"
#include "panel_link/plink_master.h"
#include "panel_link/plink_store.h"
#include "plink_service.h"
#include "ui_backend/ui_backend.h"

/* NVS "link" (docs/features/11 "Decisions", LINK-022): never in a backup (backup_policy.c leaves
 * it out), erased by a factory reset. Key names and value numbers are stored: never change them. */
#define LINK_NS         "link"
#define LINK_KEY_ROLE   "role"      /* u8, plink_service_role_t */
#define LINK_KEY_MASTER "master"    /* blob, plink_store_master_encode */
#define LINK_KEY_KEYPAD "keypad"    /* blob, plink_store_keypad_encode */
#define LINK_KEY_STATIC "master_ip" /* u32, a keypad's master set by hand (network order) */
#define LINK_KEY_MAINT  "otamaint"  /* u8 1: an update went in through LINK-025's maintenance */
/* Updates through the link (docs/features/11, BL-048): */
#define LINK_KEY_MVER   "mver" /* blob, a keypad's master's version (LINK-029): schema u8 1, net_id
                                * u16 little-endian, version (len u8, bytes) */
#define LINK_KEY_KUP    "kup"  /* u32, bit d-1: a master's keypads on line when its own restart into
                                * an update came (LINK-041), erased once it runs confirmed */

#define LINK_POLL_MS      50u /* the longest select() wait: the hand-overs are looked at */
#define LINK_REQ_WAIT_MS  2000u
#define LINK_STATUS_FRESH 15u /* s: a status this old still allows an update (docs/features/11) */

#define LINK_MDNS_SERVICE "_roboalarms-link" /* 15 characters, RFC 6763's longest: never longer */
#define LINK_MDNS_PROTO   "_udp"

/* The name a master gives itself until its configuration is read, and keeps while partition 1 has
 * none: the setup's "Name your system" names partition 1, and the master is called after it. */
#define LINK_DEFAULT_NAME "RoboAlarms Panel"

/* ---- plink_psa.c ----------------------------------------------------------------------------- */
/* The three primitives on PSA; only the link task calls in (and the start-up's check before it). */
bool plink_psa_port(plink_crypto_t *out);
/* The last AES key is kept between the blocks of a frame: this destroys it and wipes its copy, so
 * no session key outlives its session in RAM (LINK-014). The link task calls it at the end of
 * every turn; the start-up after its check. */
void plink_psa_forget(void);

/* ---- plink_service.c ------------------------------------------------------------------------- */
typedef struct {
    plink_service_role_t role;
    plink_crypto_t crypto;
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    char model[32];
    char fw[PLINK_FW_MAX + 1];
    TaskHandle_t task; /* set once the link task exists */
    int sock;
} link_t;
extern link_t g_link;

/* What the link task publishes for the UI, the console and the update gate (link_pub_lock). */
typedef struct {
    bool running; /* the link task runs (false: the link is off, see the log) */
    /* a master */
    uint16_t net_id;
    size_t keypads;
    ui_link_keypad_t keypad[PLINK_KEYPADS_MAX];
    uint8_t kind[PLINK_KEYPADS_MAX];  /* plink_kind_t, for the console */
    int8_t rssi[PLINK_KEYPADS_MAX];   /* its last heartbeat's, dBm (0: none yet) */
    uint16_t caps[PLINK_KEYPADS_MAX]; /* its live session's common capabilities (0: none) */
    /* PLINK_KEYPADS_MAX, PSRAM (NULL before the start): each keypad's Smart Home and settings
     * side, its newest settings report with it (plink_master_keypad_home), index for index with
     * keypad[]. Read only under link_pub_lock: a copy of g_pub carries the pointer, not them. */
    plink_keypad_home_t *home;
    uint8_t keypads_down;
    bool pairing_open; /* mDNS says pair=1 */
    bool setup_keypad; /* the setup's own pairing as a keypad runs (Option C): only while its
                        * manager exists, from KP_PAIR_START until KP_PAIR_STOP or the setup ends */
    bool setup_paired; /* ... and it paired: */
    uint8_t setup_device, setup_home;
    char setup_master[PLINK_NAME_MAX + 1];
    char setup_master_addr[24];
    char setup_home_name[PLINK_NAME_MAX + 1]; /* "" not known */
    /* either side's pairing */
    bool pair_valid;
    ui_link_pair_t pair;
    /* a keypad */
    bool kinfo_valid;
    ui_link_info_t kinfo;
    plink_keypad_info_t kraw;
    uint8_t kreport; /* the master's reporting (PLINK_REPORT_*), as this keypad can pass it on */
} link_pub_t;
extern link_pub_t g_pub;
void link_pub_lock(void);
void link_pub_unlock(void);

uint64_t link_now(void);     /* the engine's clock (alarm_service_now): monotonic ms */
int64_t link_epoch_ms(void); /* the wall clock, 0 before 2025 (not set) */
bool link_send(const plink_addr_t *to, const uint8_t *frame, size_t len);
void link_addr_ip(const plink_addr_t *a, char out[24]); /* "192.0.2.57", "-" for none */
plink_addr_t link_addr_of(uint32_t ip_net_order);       /* UDP on port 6056 */
bool link_subnet_broadcast(plink_addr_t *out);          /* this panel's subnet, port 6056 */
bool link_wifi_up(void);
esp_err_t link_radio_start(void);
bool link_radio_ready(void);
unsigned link_radio_channel(void);
void link_radio_tune(unsigned channel);
bool link_radio_send(const uint8_t *mac, const uint8_t *frame, size_t len);
bool link_radio_receive(plink_addr_t *from, uint8_t *frame, size_t *len);
size_t link_mtu(void *ctx, uint8_t transport);
bool link_test_drops(uint8_t transport);
#if CONFIG_ALARM_BUILD_DEV
void link_test_drop(uint8_t transport);
#endif
void ksvc_beacon(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len);
bool link_mdns_init(void); /* the shared responder, started once (false: no network yet) */

/* NVS "link": read a blob (false when absent or too big), write one (NULL: erase). Only from a
 * task with an internal-RAM stack (the link task, the console, the start-up). */
bool link_nvs_read(const char *key, void *buf, size_t cap, size_t *len);
esp_err_t link_nvs_write(const char *key, const void *buf, size_t len);
esp_err_t link_nvs_u32(const char *key, uint32_t value, bool erase);
bool link_nvs_get_u32(const char *key, uint32_t *out);
esp_err_t link_nvs_flag(const char *key, bool on); /* u8 1, or erased */
bool link_nvs_get_flag(const char *key);
esp_err_t link_store_role(plink_service_role_t role);
/* Factory reset into a role (the console's `plink role ... confirm`): records the reset when an
 * engine runs, erases NVS, stores the role and restarts. Does not return. */
void link_reset_into(plink_service_role_t role);

/* Requests from the UI and the console: kinds are ui_link_request_t's, plus the console's. */
enum {
    LINK_REQ_PAIR_FORCE = 100, /* master, `plink pair force`: no installer mode (dev builds) */
    LINK_REQ_KP_PAIR_AT,       /* keypad, `plink pair <ip>`: ask that master, no discovery */
    LINK_REQ_KP_STATIC,        /* keypad, `plink master <ip>|off`: its master by hand (stored) */
    LINK_REQ_KP_FORGET,        /* keypad, `plink forget` */
    LINK_REQ_KP_FORCE,         /* master, `plink kp force`: the setup's keypad pairing although the
                                * setup is done (dev builds; a bench panel keeps its Wi-Fi) */
    LINK_REQ_FW_AGAIN,         /* master, `plink fw update <n>`: keypad device asked again now */
};
typedef struct {
    uint8_t kind;
    bool allow;
    uint8_t device;
    uint8_t home;
    uint16_t supervision_s;
    char name[48];
    uint32_t ip; /* network order; 0 none */
    bool bench;  /* from the service console of a development build: no installer mode needed */
    bool switch_allowed;
    bool return_never;
    uint16_t return_s;
    char pair_code[8];
    uint8_t pair_attempts;
} link_req_t;
/* Hands req to the link task and waits for its answer (a ui_link_result_t), LINK_REQ_WAIT_MS at
 * most. UI_LINK_REQ_BUSY then: withdrawn, so it never runs, unless the link task had already taken
 * it (a slow NVS write) - it may then still be done. Never from the link task. */
uint8_t link_request(const link_req_t *req);

/* ---- plink_master_svc.c: the master's side (the link task unless said) ----------------------- */
esp_err_t msvc_prepare(uint64_t now); /* the start-up: manager, buffers, worker */
void msvc_input(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len);
void msvc_poll(uint64_t now); /* hand-overs, timers, mDNS, publishing */
uint64_t msvc_next(void);
uint8_t msvc_request(uint64_t now, const link_req_t *req);
uint32_t msvc_stack_free(void); /* the worker's, for the console */
/* Keypad device's lockout now, read on the alarm task and kept for its STATUS (LINK-027,
 * KPD-008): after a command or a device action that carried a code. Waits for the alarm task:
 * the worker and plink_dev only, never the link task. */
void msvc_lockout_refresh(uint8_t device);
/* plink_dev's hand-overs to the link task, on the worker's result queue (they wait while it is
 * full, as the worker's do): a build in device buffer buf (the link task then calls
 * hsvc_devices_in), and a device action's answer (plink_master_dev_answer). Posted in that order
 * after an action, so its DEV_ACK carries the record that shows it. */
void msvc_post_devices(uint8_t buf, size_t len, uint32_t crc);
void msvc_post_dev_ack(uint32_t ticket, uint8_t result, uint8_t why);

/* The worker is woken at once (it looks every second anyway). Any task. */
void msvc_kick(void);

/* A keypad's own wired inputs (plink_io_svc.c, LINK-055). The keypad's side: its terminals read
 * (kio_prepare returns PLINK_CAP_IO when it has any), sent by the link task (kio_poll), shown by
 * the console (kio_print). The master's side: the zone source ZSRC_KEYPAD registered
 * (miosvc_prepare returns PLINK_CAP_IO), the manager's port (miosvc_io_state, the link task),
 * supervision edges and removals told, the worker hands it all to the zone hub (miosvc_apply). */
/* An automation's arming warnings on keypads (plink_auto_svc.c, LINK-056). The master's side: the
 * offer, the worker's copy of the warnings (masvc_worker), the link task's feed to the manager
 * (masvc_poll), a keypad's put-off on the worker (masvc_put_off). The keypad's side: the offer,
 * the link task's copy and the chime (kasvc_poll), the screen's read (kasvc_warnings). */
/* ---- plink_mrb_svc.c: the remote backend on a master (BL-223, LINK-057..LINK-065) ----------- */
/* Its buffers (PSRAM): PLINK_CAP_RB, or 0 without them. Then the manager's buffers attached, its
 * port (the link task), each turn's answers and grants (the link task), a code accepted for a
 * keypad and the calls and grants' upkeep (the worker). */
uint16_t mrbsvc_prepare(void);
void mrbsvc_attach(plink_master_t *m);
bool mrbsvc_run(void *ctx, uint32_t ticket, uint8_t device, uint16_t call, const uint8_t *args,
                size_t len);
void mrbsvc_poll(plink_master_t *m, uint64_t now);
void mrbsvc_auth(uint8_t device, uint8_t user);
void mrbsvc_worker(void);
/* The keypad holding this panel's installer mode now, 0 none (any task). */
uint8_t mrbsvc_holder(void);
/* ---- plink_krb_svc.c: the remote backend on a keypad ---------------------------------------- */
/* Its buffers (PSRAM): PLINK_CAP_RB, or 0 without them; the manager's buffers attached; each turn
 * of the link task; the proxies put into the screen's backend; the generation that moves when a
 * read's answer changes; the last call's status (ui_remote_status_t). */
uint16_t krbsvc_prepare(void);
void krbsvc_attach(plink_keypad_t *k);
void krbsvc_poll(plink_keypad_t *k, uint64_t now);
void krbsvc_backend(ui_backend_t *be);
uint32_t krbsvc_gen(void);
uint8_t krbsvc_last(void);

uint16_t masvc_caps(void);
void masvc_worker(void);
void masvc_poll(plink_master_t *m, uint64_t now);
alarm_result_t masvc_put_off(const plink_cmd_t *c, const alarm_source_t *src, plink_result_t *res);
uint16_t kasvc_caps(void);
void kasvc_poll(plink_keypad_t *k, uint64_t now, uint8_t partition);
size_t kasvc_warnings(auto_warning_t *out, size_t max);

uint16_t kio_prepare(void);
void kio_poll(plink_keypad_t *k, uint64_t now);
void kio_print(void);
uint16_t miosvc_prepare(void);
void miosvc_io_state(void *ctx, uint8_t device, const plink_io_state_t *st);
void miosvc_reachable(uint8_t device, bool up);
void miosvc_removed(uint8_t device);
void miosvc_apply(void);

/* ---- plink_fwm_svc.c: a master's keypad updates (docs/features/11 "Updates through the link",
 * BL-048). The link task unless said. ------------------------------------------------------------ */
/* The start-up, before the manager: the planner's memory (PSRAM), the event queue, link/kup, the
 * restart's hook. ESP_ERR_NO_MEM: updates are left out (logged), the link runs without them. */
esp_err_t fsvc_prepare(uint64_t now);
/* PLINK_CAP_FW once fsvc_prepare succeeded (LINK-043). */
uint16_t fsvc_caps(void);
/* The manager's port.fw_read: its spare slot, its running one (both map flash windows: this
 * task's internal-RAM stack) or the copy in PSRAM, by id8. */
size_t fsvc_fw_read(void *ctx, const uint8_t id8[PLINK_ID8_LEN], uint32_t offset, uint8_t *out,
                    size_t len);
/* Each turn, before the manager's tick: UPDATING, LINK-041's question, the planner's step. */
void fsvc_poll(plink_master_t *m, uint64_t now);
/* With link_pub_lock held, from the keypads' rows (peers): the published copies. true when the
 * keypads' part of MQTT's firmware state changed (the caller tells net_mqtt after unlocking). */
bool fsvc_publish_locked(const plink_peer_info_t *peers, size_t n);
/* LINK_REQ_FW_AGAIN: a ui_link_result_t. */
uint8_t fsvc_request(plink_master_t *m, uint64_t now, const link_req_t *req);
/* The worker's turn: the Activity events handed over, posted (they wait for the alarm task). */
void fsvc_worker(void);
/* `plink fw` on a master (the console task): prints the planner's view. */
int fsvc_console_show(void);

/* ---- plink_fwk_svc.c: a keypad's updates from its master (docs/features/11 "The keypad",
 * BL-048). The link task unless said. ------------------------------------------------------------ */
/* What the keypad offers: PLINK_CAP_FW, with the relay's writer (kfw_write). */
uint16_t kfw_caps(void);
/* The relay's writer (port.fw_write): panel_ota_write on the link task (internal-RAM stack). */
size_t kfw_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len, uint32_t *busy_ms);
/* Right after plink_keypad_init: its master's version back (link/mver) and the cap, its facts and
 * last result, its trial after an install its master ordered. */
void kfw_boot(plink_keypad_t *k, uint64_t now);
/* Each turn, after the manager's tick: link/mver and the cap, the orders through panel_ota, its
 * run's state, the gate (asked twice) and INSTALL, the trial for panel_ota's self-test, the
 * published copy. kept_tone: a fire or CO tone kept through a lost link (its gate's KEPT_TONE). */
void kfw_poll(plink_keypad_t *k, uint64_t now, const plink_keypad_info_t *info, bool kept_tone);
/* `plink fw` on a keypad (the console task). */
int kfw_console_show(void);

/* ---- plink_home_svc.c: Smart Home and settings on the master's keypads (BL-216) --------------
 * docs/features/11 "Smart Home and Home Assistant on keypads". The link task unless said. */
#define LINK_DEV_JOBS 8u /* device actions waiting or running: one in flight per keypad */
/* The start-up, before the manager: the settings queue and the published reports; with home
 * control and a running registry (plink_service_set_home) the device buffers and the plink_dev
 * task. ESP_ERR_NO_MEM: the link stays off; Smart Home alone failing leaves it out (logged). */
esp_err_t hsvc_prepare(void);
/* What this master offers its keypads (LINK-043): PLINK_CAP_HOME once hsvc_prepare started the
 * device side, PLINK_CAP_PREFS in a build with the UI. */
uint16_t hsvc_caps(void);
/* The manager's port.dev_run and port.prefs_done. */
bool hsvc_dev_run(void *ctx, uint32_t ticket, uint8_t device, uint8_t home,
                  const plink_dev_act_t *act);
void hsvc_prefs_done(void *ctx, uint8_t device, const plink_prefs_done_t *done);
/* A build plink_dev posted (msvc_post_devices): to the manager, and the buffer it used before
 * back to plink_dev. */
void hsvc_devices_in(plink_master_t *m, uint64_t now, uint8_t buf, size_t len, uint32_t crc);
/* Each turn, before the manager's tick: the settings writes queued by plink_service_prefs_set. */
void hsvc_poll(plink_master_t *m, uint64_t now);
/* Whether a live keypad has PLINK_CAP_HOME (publish): plink_dev builds only then. */
void hsvc_wanted(bool wanted);
/* For the console (its task): plink_dev's stack minimum free (0: no such task), and a copy of
 * the device list the manager serves now and its records (false: none yet). */
uint32_t hsvc_stack_free(void);
typedef struct {
    bool offered;      /* PLINK_CAP_HOME offered */
    bool wanted;       /* a live keypad has it */
    uint32_t builds;   /* builds handed to the manager since the start */
    uint32_t list_crc; /* the copy's */
    uint32_t states_crc;
    size_t len;        /* the blob's bytes */
    size_t rows;       /* the registry's rows it was built from */
    uint64_t built_ms; /* link_now() of its build */
} hsvc_dev_info_t;
bool hsvc_dev_copy(uint8_t *blob, size_t cap, plink_dev_recs_t *recs, hsvc_dev_info_t *info);

/* ---- plink_keypad_svc.c: a keypad's side - the role's, or a master's setup pairing ----------- */
esp_err_t ksvc_prepare(uint64_t now, bool setup_only);
bool ksvc_active(void); /* a keypad manager exists */
/* The setup's pairing on a master (Option C) is over without Done (KP_PAIR_STOP, or the setup
 * ended another way): a finished pairing is forgotten (its keys wiped; the other master drops the
 * keypad it never heard from, LINK-016's provisional rule), the published setup state cleared and
 * the manager gone - its buffers are kept for the next KP_PAIR_START. Nothing without one. */
void ksvc_setup_end(uint64_t now);
void ksvc_input(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len);
void ksvc_poll(uint64_t now);
uint64_t ksvc_next(void);
uint8_t ksvc_request(uint64_t now, const link_req_t *req);
/* The setup's pairing on a master (Option C): KP_DONE stores the record and the keypad role and
 * restarts; false when nothing was paired. */
bool ksvc_setup_done(void);
/* The UI's copies on a keypad, and of the setup's pairing on a master (any task). */
uint32_t ksvc_ui_seq(void);
void ksvc_ui_snapshot(alarm_snapshot_t *out);
bool ksvc_ui_config(alarm_config_t *out);
size_t ksvc_ui_events(alarm_event_t *out, size_t max);
/* A keypad's commands from the UI: waits up to 3 s for the master's answer (the codes are wiped
 * once handed over). A panic not answered by then is still being sent: ALARM_OK. */
alarm_result_t ksvc_command(const plink_cmd_t *cmd, plink_result_t *res);
/* Smart Home on a keypad (BL-216; docs/features/11 "Device actions"). ksvc_dev_copy: the master's
 * devices as the screen shows them (ui_backend_t.dev_list; 0 without a mirror, or before the
 * list is in - the rail's Smart Home stays hidden), and (info, may be NULL) the mirror's state for
 * the console. Any task; a copy under its own lock, never link_pub_lock. */
typedef struct {
    bool offered;            /* a mirror: PLINK_CAP_HOME offered */
    bool fresh;              /* ONLINE, with device data within 30 s (LINK-046) */
    size_t rows;             /* the screen's rows */
    plink_dev_info_t mirror; /* as of the link task's last turn (heard_ms too) */
    uint64_t copied_ms;      /* link_now() of the rows' copy (0: none yet) */
} ksvc_dev_info_t;
size_t ksvc_dev_copy(ui_dev_t *out, size_t max, ksvc_dev_info_t *info);
/* ui_backend_t.dev_act on a keypad (the LVGL task): steps 1-4 answered at once from the copy
 * (UNREACHABLE, REFUSED, NEED_CODE), else handed to the link task and waited for 1 s (3 s with a
 * code); no answer by then is UI_DEV_OK, the tile pending until the answer or the give-up. */
ui_dev_result_t ksvc_dev_act(const char *ref, uint8_t action, int32_t value, const char *code,
                             alarm_result_t *code_result);

#endif /* CONFIG_ALARM_LINK */
