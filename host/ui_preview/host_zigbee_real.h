/*
 * host_zigbee_real.h - the panel's real Zigbee on the PC (docs/features/20 testing.md §10;
 * BL-143 ZB-M6, ZB-027): ui_backend_t's zb_* calls over the firmware's own zbz_panel_t
 * (components/core/zigbee_zones), its zone source zsrc_zb_t registered with the pretend panel's
 * zone hub, and the fake radio (zigbee_zones_fake) with seven virtual devices built from the
 * device signatures - a door contact, a motion sensor, a smoke alarm, a water sensor, a siren, a
 * keypad and a key fob - none of them in the network at the start.
 *
 * It is wired as zigbee_service wires it on a panel: the radio's reports go to
 * zbz_panel_on_report, the sensor traffic straight to the source and on to the hub's zones; what
 * zbz_panel hands out to store is kept in memory (the simulator's NVS: a restart of the panel,
 * host_zigbee_real_restart, reads it back); what it logs, a keypad's or fob's command and the
 * siren's bell trouble go to the engine through the owner's port, after the panel's own work, as
 * the firmware hands them to the alarm task. Time is the host's clock, in ms.
 *
 * The virtual devices behave on their own: when the join window opens (Add a device, Replace),
 * the first two not yet in the network press their pairing button 3 s and 6 s later and join,
 * and their setup runs to Ready over the fake radio (a sleepy one listens 30 s after each of its
 * own frames, as a device keeps polling fast while it is set up). An install-code add listens for
 * its own device: that one joins first, with its code. Their sensors are tripped by the caller
 * (host_zigbee_real_trip: the simulator's buttons).
 *
 * No LVGL and no engine here: host_panel.c (the preview and the simulator) and the host test
 * test_host_zigbee_real.c both drive it. Host only; single-threaded.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"
#include "zigbee_zones/zbz_panel.h"
#include "zone_hub/zone_hub.h"

/* The virtual devices, in the order they join. */
typedef enum {
    HOST_ZBR_DOOR = 0, /* a door contact (IAS contact) */
    HOST_ZBR_MOTION,   /* a motion sensor (IAS motion) */
    HOST_ZBR_SMOKE,    /* a smoke alarm (IAS fire) */
    HOST_ZBR_WATER,    /* a water sensor (IAS water) */
    HOST_ZBR_SIREN,    /* a mains siren (IAS WD) */
    HOST_ZBR_KEYPAD,   /* a keypad (IAS ACE) */
    HOST_ZBR_FOB,      /* a key fob (IAS ACE, no keypad) */
    HOST_ZBR_COUNT,
} host_zbr_dev_t;

/* What it needs from the pretend panel around it; any may be NULL. */
typedef struct {
    void *ctx;
    /* installer mode with every partition disarmed (zbz_panel_request's `allowed`) */
    bool (*allowed)(void *ctx);
    /* RESET's installer code typed again (ZB-017), checked by the engine: UI_ZB_REQ_OK,
     * UI_ZB_REQ_WRONG_CODE or UI_ZB_REQ_LOCKED_OUT (NULL: every code is wrong) */
    ui_zb_req_result_t (*installer_code)(void *ctx, const char *code);
    uint32_t (*wall_clock)(void *ctx);    /* Unix seconds, 0 not set */
    uint32_t (*devices_taken)(void *ctx); /* engine device numbers others hold (bit n-1) */
    /* a keypad's or fob's command, run by the engine now (the alarm task's part) */
    alarm_result_t (*ace_run)(void *ctx, const zba_post_t *post, alarm_cmd_result_t *detail);
    void (*bell_trouble)(void *ctx, bool active); /* a siren went silent, or is heard again */
    void (*log)(void *ctx, const zbz_log_t *event); /* an event for the engine's log */
    /* a rebind of a zone by the Zigbee pages (the sensor may change: the panel's spoken-name
     * approval is invalidated first, D07); NULL: the hub is bound directly */
    zone_hub_result_t (*bind)(void *ctx, uint16_t zone, const zone_hub_binding_t *b);
} host_zbr_port_t;

/* A fresh world at `now`: the store emptied, the virtual devices out of the network, the radio
 * working on channel 15; its zone source is registered with `hub` (before zone_hub_start), its
 * zone events go to `sink`. */
void host_zigbee_real_init(zone_hub_t *hub, const zsrc_sink_t *sink, const host_zbr_port_t *port,
                           uint64_t now);
/* The panel restarts: a new zbz_panel from what was stored; the network (the stack's) and the
 * devices in it stay. */
void host_zigbee_real_restart(uint64_t now);
/* Everything due up to `now`, each at its own time, then what is to be stored, logged and run. */
void host_zigbee_real_run(uint64_t now);
/* The earliest time something is due (UINT64_MAX: nothing). */
uint64_t host_zigbee_real_next_due(void);
/* The engine's news for the sirens and keypads, as the alarm task posts it (zbz_panel_output,
 * zbz_panel_partition); handed on at the panel's present time. */
void host_zigbee_real_output(uint8_t partition, alarm_output_t output, alarm_pattern_t pattern);
void host_zigbee_real_partition(uint8_t partition, const zba_view_t *view);
/* A walk test started or ended (zbz_panel_walk_test), and the zones it covers (zbz_walk_covered:
 * zbz_panel_walk_scope), as zigbee_service's take_engine_news and refresh_walk_scope hand them on. */
void host_zigbee_real_walk(bool on, const uint8_t covered[ALARM_ZONE_BITMAP_BYTES]);
/* Fills out's zb_* entries (their ctx is not used). */
void host_zigbee_real_backend(ui_backend_t *out);

/* ---- the virtual devices ------------------------------------------------------------------- */

/* Its pairing button: it joins when it is out of the network and the window is open; in it, the
 * press wakes it (a check-in). False: neither. */
bool host_zigbee_real_press(host_zbr_dev_t which);
/* The next device out of the network presses its button (the window must be open); its index,
 * HOST_ZBR_COUNT none. */
host_zbr_dev_t host_zigbee_real_press_next(void);
bool host_zigbee_real_trip(host_zbr_dev_t which, bool on); /* its sensor: open, motion, smoke... */
bool host_zigbee_real_tamper(host_zbr_dev_t which, bool on);
bool host_zigbee_real_battery(host_zbr_dev_t which, uint8_t percent);
bool host_zigbee_real_leave(host_zbr_dev_t which); /* it leaves for good, by itself */
/* It never answers this setup step (a ZSRC_ZB_STEP_*), until called again with ZSRC_ZB_STEP_NONE:
 * its setup stops after its tries (ZB-012). False: no such device. */
bool host_zigbee_real_stall(host_zbr_dev_t which, uint8_t step);
/* It stops answering the panel's requests (reads, commands) until unmuted; its own frames still
 * go out. False: no such device. */
bool host_zigbee_real_mute(host_zbr_dev_t which, bool on);
/* A keypad's or fob's IAS ACE Arm (mode ZBA_ARM_*, a keypad's code or NULL), as a person presses
 * it; what it causes is run at once. False: not joined, or no IAS ACE client. */
bool host_zigbee_real_arm(host_zbr_dev_t which, uint8_t mode, const char *code);
/* The cluster commands the panel sent to its IAS ACE (keypad, fob) or IAS WD (siren) endpoint,
 * and the last of them in `last` (may be NULL); 0 none. */
uint32_t host_zigbee_real_received(host_zbr_dev_t which, zsrc_zb_zcl_t *last);
/* The keypads' and fobs' answers zbz_panel_ace_result took in time (since init). */
unsigned host_zigbee_real_ace_answered(void);
void host_zigbee_real_unplug(bool unplugged);      /* the radio module out of its socket */
void host_zigbee_real_noise(bool on);              /* something drowns the channel out (ZB-006) */
bool host_zigbee_real_joined(host_zbr_dev_t which);
uint64_t host_zigbee_real_ieee(host_zbr_dev_t which);
/* Its registry id (the pages' argument), 0 not in the registry. */
uint16_t host_zigbee_real_id(host_zbr_dev_t which);
/* The panel itself, for checks (read only). */
const zbz_panel_t *host_zigbee_real_panel(void);
