/*
 * plink_internal.h - shared between the files of plink_service (not a public API).
 *
 * One task, the link task, owns the socket, the role's link manager and everything the managers'
 * ports touch; the other tasks reach it through hand-overs that never make it wait:
 *   - requests (the UI's link_request, the console): one at a time, answered within 2 s
 *     (link_request);
 *   - on a master: the alarm listener's flag and queues, the worker's results (plink_master_svc.c);
 *   - on a keypad: commands from the UI (plink_keypad_svc.c).
 * What the others read - the keypads, the pairing, the keypad's view of its master - the link task
 * publishes in g_pub under link_pub_lock(), and the UI and the console copy it from there.
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

#define LINK_POLL_MS      50u /* the longest select() wait: the hand-overs are looked at */
#define LINK_REQ_WAIT_MS  2000u
#define LINK_STATUS_FRESH 15u /* s: a status this old still allows an update (docs/features/11) */

#define LINK_MDNS_SERVICE "_roboalarms-link" /* 15 characters, RFC 6763's longest: never longer */
#define LINK_MDNS_PROTO   "_udp"

/* ---- plink_psa.c ----------------------------------------------------------------------------- */
/* The three primitives on PSA; only the link task calls in (and the start-up's check before it). */
bool plink_psa_port(plink_crypto_t *out);

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
    uint8_t kind[PLINK_KEYPADS_MAX]; /* plink_kind_t, for the console */
    uint8_t keypads_down;
    bool pairing_open; /* mDNS says pair=1 */
    bool setup_keypad; /* the setup's own pairing as a keypad runs (Option C) */
    bool setup_paired; /* ... and it paired: */
    uint8_t setup_device, setup_home;
    char setup_master[PLINK_NAME_MAX + 1];
    char setup_master_addr[24];
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
};
typedef struct {
    uint8_t kind;
    bool allow;
    uint8_t device;
    uint8_t home;
    uint16_t supervision_s;
    char name[48];
    uint32_t ip; /* network order; 0 none */
} link_req_t;
/* Hands req to the link task and waits for its answer (a ui_link_result_t), LINK_REQ_WAIT_MS at
 * most (UI_LINK_REQ_BUSY then). Never from the link task. */
uint8_t link_request(const link_req_t *req);

/* ---- plink_master_svc.c: the master's side (the link task unless said) ----------------------- */
esp_err_t msvc_prepare(uint64_t now); /* the start-up: manager, buffers, worker */
void msvc_input(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len);
void msvc_poll(uint64_t now); /* hand-overs, timers, mDNS, publishing */
uint64_t msvc_next(void);
uint8_t msvc_request(uint64_t now, const link_req_t *req);
uint32_t msvc_stack_free(void); /* the worker's, for the console */

/* ---- plink_keypad_svc.c: a keypad's side - the role's, or a master's setup pairing ----------- */
esp_err_t ksvc_prepare(uint64_t now, bool setup_only);
bool ksvc_active(void); /* a keypad manager exists */
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

#endif /* CONFIG_ALARM_LINK */
