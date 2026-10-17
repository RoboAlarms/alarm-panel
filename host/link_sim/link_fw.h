/*
 * link_fw.h - updates through the link on the PC (host/link_sim; docs/features/11 "Updates through
 * the link"): a stand-in for panel_ota on both sides, and the master's and the keypad's update
 * services around the link core (plink_fw.h, plink_master.h, plink_keypad.h) as plink_service
 * runs them on a panel (components/services/plink_service: plink_fwm_svc.c, plink_fwk_svc.c).
 *
 * The stand-in for panel_ota:
 *   - images: a model's release of a version is a fake image of about 1 MB whose bytes follow from
 *     its model and version, with its SHA-256 (link_img_make); its project and chip range match
 *     every PC panel;
 *   - the update server: the master's (for a copy for another model) and the keypad's (for its
 *     FETCH) answer, are unreachable (off: the keypad's FETCH fails NO_CONNECT, so it gets the
 *     relay) or refuse (NOT_OFFERED: it waits for the master's next version);
 *   - a download: its own update on the master (fw install V) or a keypad's FETCH, at about
 *     300 KB/s; a relay written into RAM, the flash kept busy 1 ms a write;
 *   - a restart: the node is off for 3 s (frames to it are lost, it sends nothing), then starts
 *     again on the new version with its link record, on its self-test - or back on the version
 *     before after a failed one;
 *   - the self-test: the master's passes 15 s after its start with a keypad back for 10 s
 *     (LINK-041; none paired: 15 s), fails after 90 s without one, or 20 s on when made to fail
 *     (fw bad); a keypad's passes once its trial with its master passed (an INFO naming its
 *     version) and its link has been up for 10 s, and fails when the trial fails (its master went
 *     back, or no INFO in 10 min) or, made to fail, after those 10 s.
 * What a panel keeps in flash and NVS (the versions, the spare slot, link/mver, the trust record,
 * "ordered install of V") is in link_mfw_t and link_kfw_t, the caller's: a restart keeps it, a new
 * process starts from --version.
 */
#ifndef LINK_FW_H
#define LINK_FW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_node.h"
#include "panel_link/plink_fw.h"

#define LINK_FW_DEVICE  "pc_panel"      /* the PC panels' model (--model gives another) */
#define LINK_FW_PROJECT "roboalarms-pc" /* every PC image's project */
#define LINK_FW_SLOT    4718592u        /* an OTA slot of a panel */
/* The master's own restart waits while a keypad on line still downloads or receives its target
 * (decision 7: they download alongside it and restart with it), at most this long from when its
 * own update was in its spare slot. */
#define LINK_FW_KEYPADS_WAIT_MS 1800000u

/* A model's release of a version: its bytes (heap), length and SHA-256. */
typedef struct {
    char device[PLINK_FW_DEVICE_MAX + 1];
    char version[PLINK_FW_VERSION_MAX + 1];
    uint32_t size;
    uint8_t sha[PLINK_SHA256_LEN];
    uint8_t *bytes; /* NULL: none */
} link_img_t;

/* The release of device's version (false: no memory); link_img_free frees it. */
bool link_img_make(link_img_t *img, const char *device, const char *version);
void link_img_free(link_img_t *img);

/* An update server's answers (link_fw_server). */
enum { LINK_SRV_ON = 0, LINK_SRV_OFF, LINK_SRV_REFUSE };

/* ---- the master ------------------------------------------------------------------------------ */
typedef struct link_mfw {
    /* kept across its restarts (the panel's flash and NVS) */
    char name[PLINK_NAME_MAX + 1];
    char id[PLINK_PANEL_ID_LEN + 1];
    char device[PLINK_FW_DEVICE_MAX + 1];
    char running[PLINK_FW_VERSION_MAX + 1];
    char prev[PLINK_FW_VERSION_MAX + 1];  /* the version before (a failed self-test's) */
    bool confirmed;                       /* else on its self-test */
    char spare[PLINK_FW_VERSION_MAX + 1]; /* its own update downloaded and checked ("": none) */
    char getting[PLINK_FW_VERSION_MAX + 1]; /* its own update downloading ("": none) */
    uint32_t got;
    uint64_t spare_at;                    /* its own update in its spare slot since */
    bool bad_test;                        /* its next self-test fails */
    uint8_t server;                       /* its update server (copies): LINK_SRV_* */
    bool going_back;                      /* the restart under way is back to prev */
    bool down;                            /* restarting: off until boot_at */
    uint64_t boot_at;
    link_img_t run, spare_img;            /* its slots */
    /* RAM: a start clears it */
    uint64_t started, back_since, plan_at, restart_at, get_at, copy_due;
    bool restart_soon;                    /* its own restart allowed: INSTALLs go */
    link_img_t copy_img;
    plink_fw_world_t w;
    plink_fw_step_t s;
    plink_fw_log_t log;
    uint8_t said_state[PLINK_KEYPADS_MAX];
    uint8_t said_pc[PLINK_KEYPADS_MAX];
    char said_ver[PLINK_KEYPADS_MAX][PLINK_FW_VERSION_MAX + 1];
    uint64_t said_at[PLINK_KEYPADS_MAX];
    uint8_t said_own; /* its own run as last said */
    uint8_t said_own_pc;
} link_mfw_t;

/* Before link_master_init: its name, panel id, model and the version it runs (confirmed). */
void link_mfw_setup(link_mfw_t *f, const char *name, const char *id, const char *device,
                    const char *version);
/* From link_master_init, once the link manager is made (a restart too). */
void link_mfw_boot(link_master_t *v, uint64_t now_ms);
/* The link manager's port.fw_read: its own image (running or spare slot) or the copy, by id8. */
size_t link_mfw_read(void *ctx, const uint8_t id8[PLINK_ID8_LEN], uint32_t offset, uint8_t *out,
                     size_t len);
/* Each turn before the link manager's tick: its own update, the planner (once a second, every
 * 100 ms while its restart waits for the INSTALLs' echoes), its orders, the copy, its restart and
 * self-test; the rows and the events said as they change. false: it is restarting (off), nothing
 * else runs - or it just started again (link_master_init ran). */
bool link_mfw_tick(link_master_t *v, uint64_t now_ms);
uint64_t link_mfw_next(const link_master_t *v);
/* The console: Install of version (its own update: downloaded, then the restart once everything is
 * disarmed, its keypads with it), each keypad's row, ask keypad n again (plink fw update <n>), its
 * next self-test made to fail, its update server for copies. */
void link_mfw_install(link_master_t *v, uint64_t now_ms, const char *version);
void link_mfw_say(link_master_t *v, uint64_t now_ms);
bool link_mfw_again(link_master_t *v, uint64_t now_ms, uint8_t device);

/* ---- the keypad ------------------------------------------------------------------------------ */
typedef struct link_kfw {
    /* kept across its restarts (the panel's flash and NVS) */
    char name[PLINK_NAME_MAX + 1];
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    bool have_find;
    plink_addr_t find;
    char device[PLINK_FW_DEVICE_MAX + 1];
    char running[PLINK_FW_VERSION_MAX + 1];
    char prev[PLINK_FW_VERSION_MAX + 1];
    bool probation;                          /* on its self-test */
    char ordered[PLINK_FW_VERSION_MAX + 1];  /* "ordered install of V" (panel_ota's NVS) */
    uint8_t last;                            /* plink_fw_last_t, for version: */
    char last_version[PLINK_FW_VERSION_MAX + 1];
    uint8_t failed;                          /* the health bits a failed self-test named */
    char mver[PLINK_FW_VERSION_MAX + 1];     /* link/mver: its master's version ... */
    uint16_t mver_net;                       /* ... and the pairing's net */
    char server[PLINK_FW_SERVER_MAX + 1];    /* ota/trust: its update server ... */
    uint8_t key[PLINK_FW_KEY_LEN];           /* ... and the key accepted for it (zeros: none) */
    uint8_t srv;                             /* its update server answers: LINK_SRV_* */
    bool bad_test;                           /* its next self-test fails */
    bool down;                               /* restarting: off from down_at until boot_at */
    uint64_t down_at, boot_at;
    /* RAM: a start clears it */
    uint32_t mver_seq;
    bool capped;
    char cap[PLINK_FW_VERSION_MAX + 1];
    uint64_t online_from;                    /* ONLINE since (0: not) */
    uint32_t run;                            /* the order that started its run (0: none) */
    uint8_t run_op;
    char v[PLINK_FW_VERSION_MAX + 1];
    uint32_t size;
    uint8_t sha[PLINK_SHA256_LEN];
    uint8_t *buf;                            /* a relay's bytes (heap) */
    uint32_t wrote;
    link_img_t img;                          /* a FETCH's: what its server offers */
    uint64_t fetch_at;
    bool ready;
    uint8_t asks;
    uint64_t ask_at;
    plink_fw_state_t said;
    uint64_t said_at;
} link_kfw_t;

/* Before link_keypad_init: its name, panel id, where its master is (NULL: not known), model and
 * the version it runs. */
void link_kfw_setup(link_kfw_t *f, const char *name, const char *panel_id,
                    const plink_addr_t *master, const char *device, const char *version);
/* From link_keypad_init, once the link manager is made (a restart too). */
void link_kfw_boot(link_keypad_t *v, uint64_t now_ms);
/* The link manager's port.fw_write (the relay's writer). */
size_t link_kfw_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len,
                      uint32_t *busy_ms);
/* Each turn and after each frame: its master's version kept and the cap applied, the orders, the
 * download, the image checked, the gate asked twice and the restart, its self-test. false: it is
 * restarting (off) - or it just started again (link_keypad_init ran). */
bool link_kfw_tick(link_keypad_t *v, uint64_t now_ms);
uint64_t link_kfw_next(const link_keypad_t *v);
/* The console: its run and its master's version; its update server; its next self-test fails. */
void link_kfw_say(link_keypad_t *v, uint64_t now_ms);

#endif /* LINK_FW_H */
