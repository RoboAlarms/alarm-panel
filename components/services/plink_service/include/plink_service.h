/*
 * plink_service.h - the panel link on a panel (docs/features/11, BL-048): the role (master or
 * keypad, NVS "link"), one task that owns the UDP socket on port 6056 and the link manager of the
 * role (components/core/panel_link), the crypto port on PSA, mDNS, and around them:
 *   - on a master: the alarm listener, a worker that runs the keypads' commands on the alarm task,
 *     supervision through alarm_service_device_reachable, the device numbers shared with Zigbee;
 *   - on a keypad: the UI backend that shows the master's status and sends its commands, the
 *     sounds, what its voice hears, the time and the update gate;
 *   - the UI's link entries for either role and the `plink` console command.
 *
 * The component follows ALARM_LINK the "absent" way (components/services/alarm_features): with the
 * option off nothing here is compiled, and callers guard their calls with #if CONFIG_ALARM_LINK.
 * The link task never takes board_ui_lock and never calls the UI; the UI's entries read copies.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_snapshot.h"
#include "alarm_service.h"
#include "esp_err.h"
#include "sdkconfig.h"
#if CONFIG_ALARM_FEATURE_UI
#include "ui_backend/ui_backend.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The role, stored in NVS "link", key "role" (u8). A stored contract: never renumber. */
typedef enum {
    PLINK_SERVICE_MASTER = 0, /* the alarm panel; also when nothing is stored */
    PLINK_SERVICE_KEYPAD = 1, /* a keypad for a master elsewhere in the house */
} plink_service_role_t;

/* The stored role, read once (after nvs_flash_init()); later calls give the same answer. */
plink_service_role_t plink_service_role(void);

/* The start-up runs the alarm panel although a keypad role is stored (a build or board that
 * can't be a keypad): from here on plink_service_role() answers MASTER for this start, so the
 * link runs the master's side and the UI the alarm panel's. The stored role is left as it is.
 * Before plink_service_start() and the UI. */
void plink_service_force_master(void);

/* ---- the master ------------------------------------------------------------------------------ */

/* The alarm listener: register it before alarm_service_start(). Its callbacks only hand over
 * (a snapshot flag, events and sounds into queues that never wait). */
const alarm_listener_t *plink_service_listener(void);

/* The engine device numbers 1..8 the link's keypads share (LINK-001): others_held answers the
 * numbers the Zigbee keypads hold (zigbee_service_devices_held, which waits for the zone lock: the
 * link's worker reads it every second, never the link task); others_held_now the same without
 * waiting, false while the lock is busy (zigbee_service_devices_held_now: the link task's check of
 * a new keypad's number); link_held is told the link's own whenever they change, and a new
 * keypad's number before its pairing is answered (zigbee_service_set_link_devices). Before
 * plink_service_start(). */
void plink_service_set_devices_port(uint32_t (*others_held)(void),
                                    bool (*others_held_now)(uint32_t *out),
                                    void (*link_held)(uint32_t mask));

/* ---- both roles ------------------------------------------------------------------------------ */

/* Starts the link for the stored role: the crypto check (the link stays off when it fails, said in
 * the log), the socket, the task, and on a master the worker. model: this panel's kind of hardware
 * (board_name()), for the log; a keypad names itself "Keypad <panel id>" when pairing. Once only;
 * on a master after alarm_service_start() and the network, on a keypad after
 * alarm_service_set_dormant() and esp_netif_init(), before the UI starts (the UI's first look at
 * link_info then finds the keypad role). */
esp_err_t plink_service_start(const char *model);

/* ---- the keypad ------------------------------------------------------------------------------ */

/* Its home partition from its stored record (1..ALARM_MAX_PARTITIONS; 1 when not paired): the
 * UI's ui_options_t.partition before it starts (docs/features/11 "Roles"). */
uint8_t plink_service_keypad_home(void);

/* Mains and battery for the heartbeat (the master's keypads list shows them), from the task that
 * polls the power monitor. Never waits. */
void plink_service_keypad_power(bool mains_lost, bool battery_low, bool battery_present);

/* The update gate (panel_ota_set_idle_gate): idle only while the link is up and the master's
 * newest status, under 15 s old, allows an update (proto_fw_install_allowed); the same for a
 * keypad that hears its master but can't read its configuration (it needs this update); a keypad
 * not paired, or in its no-code maintenance (LINK-025), may update too. */
bool plink_service_update_idle(void *ctx, const char **why);

/* ---- the keypad's voice (docs/features/11 LINK-028) --------------------------------------------
 * A keypad speaks its home partition's announcements from what its master sends (the master's
 * events, one-shot sounds and status), as an alarm panel's voice hears its engine: the link task
 * hands them on (components/core/panel_link plink_vfeed: covered events only, never anything
 * older than its last baseline, nothing while the link isn't ONLINE). The six calls are the
 * voice's producer entries (panel_voice_engine_*), made on the link task; each only copies and
 * returns. With a voice set, the master's one-shot sounds are played tracked while it is fed. */
typedef struct {
    void (*baseline)(uint8_t partition, const alarm_snapshot_t *snap);
    void (*pause)(void);
    void (*event)(const alarm_event_t *ev);
    void (*sound)(uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket);
    void (*output)(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);
    void (*snapshot)(const alarm_snapshot_t *snap);
} plink_service_voice_t;
/* Before plink_service_start(); v is kept (a static). NULL: no voice. */
void plink_service_set_voice(const plink_service_voice_t *v);
/* The voice wants a silent baseline (start-up, a lost fact): the link task sends one while the
 * link is ONLINE (and does at every ONLINE anyway). Any task; a flag only. */
void plink_service_voice_resync(void);
/* The keypad's copy of its master's configuration and its generation (the snapshots'
 * config_seq), for the voice's zone words. Any task; false before the first one. */
bool plink_service_keypad_config(alarm_config_t *out, uint32_t *config_seq);

#if CONFIG_ALARM_FEATURE_UI
/* The UI backend's link entries (link_info, link_pair_status, link_keypads, link_request) for the
 * role; on a keypad also the entries the link serves (status, configuration, events, commands,
 * codes) and the master-only ones turned off (NULL, or ALARM_ERR_UNAVAILABLE where the UI calls
 * without looking). After ui_port_esp_backend(), before the UI starts. */
void plink_service_extend_backend(ui_backend_t *be);
#endif

/* The `plink` console command (the service console only; ESP_OK without it). */
esp_err_t plink_service_console_register(void);

#ifdef __cplusplus
}
#endif
