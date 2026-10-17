/*
 * plink_service.h - the panel link on a panel (docs/features/11, BL-048): the role (master or
 * keypad, NVS "link"), one task that owns the UDP socket on port 6056 and the link manager of the
 * role (components/core/panel_link), the crypto port on PSA, mDNS, and around them:
 *   - on a master: the alarm listener, a worker that runs the keypads' commands on the alarm task,
 *     supervision through alarm_service_device_reachable, the device numbers shared with Zigbee,
 *     its Smart Home devices and actions for its keypads and their settings (Home Assistant);
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
#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_prefs.h"
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

/* ---- the master: Smart Home and settings on its keypads (docs/features/11 "Smart Home and Home
 * Assistant on keypads", BL-216) ------------------------------------------------------------- */

/* Whether the device registry runs (device_service_start() succeeded): only then, and only in a
 * build with home control, does this master offer its devices to its keypads (PLINK_CAP_HOME,
 * docs/features/11 "Capabilities"). Before plink_service_start(); never called: not offered. */
void plink_service_set_home(bool registry_runs);

/* The link keypads for Home Assistant's keypads list (ha_link_settings_port_t.keypads; docs/
 * features/23 HAI-018), in device order: the master's name for each ("Keypad <n>" when it has
 * none), its home partition, online (a live session heard in the last 30 s), its firmware and
 * signal, and once it reported its settings (LINK-050) its own panel id ("" while another keypad
 * with a lower number reports the same, as plink_master_keypad_home says), the values it stores,
 * the fields it accepts and its voice - its id alone while it is online in a session without
 * PLINK_CAP_PREFS (its report is from before: nothing of it settable). A copy under the link's
 * publishing lock, which the link task refreshes every 0.5 s: never a wait for the link itself.
 * At most max; 0 on a keypad and while the link is off. Any task. */
size_t plink_service_keypads(proto_link_keypad_t *out, size_t max);

/* Who asked for a settings write: PREFS_SET's origin (docs/features/11 "Frozen values"). */
#define PLINK_SERVICE_ORIGIN_HA      1u /* Home Assistant */
#define PLINK_SERVICE_ORIGIN_CONSOLE 2u /* the master's service console */

/* A fields body: count u8 and the TLVs (proto_prefs_encode), every field v1 knows at most. */
#define PLINK_SERVICE_PREFS_BODY_MAX PROTO_PREFS_ENC_MAX

/* How a settings write to a keypad went: the panel link's plink_pset_t numbers (asserted). */
typedef enum {
    PLINK_SERVICE_PSET_OK = 0,      /* queued; at its end, answered (perhaps with refusals) */
    PLINK_SERVICE_PSET_NOT_FOUND,   /* no keypad has that number (or the link isn't a master's) */
    PLINK_SERVICE_PSET_OFFLINE,     /* no live session, or not heard for 30 s; or removed since */
    PLINK_SERVICE_PSET_UNSUPPORTED, /* no settings over its link (no PREFS: older firmware) */
    PLINK_SERVICE_PSET_BUSY,        /* a write to it is in flight, or the queue is full */
    PLINK_SERVICE_PSET_TIMEOUT,     /* sent, and no answer within 6 s */
    PLINK_SERVICE_PSET_BAD,         /* not a fields body of at most PLINK_SERVICE_PREFS_BODY_MAX */
} plink_service_pset_t;

/* A write's end, from the link task: OK with why[id - 1] the PROTO_PREFS_WHY_* of each field the
 * keypad refused and 0 for each it applied (its report, in plink_service_keypads, already holds
 * the values it stores now); any other result for the whole write, why all 0. Copies and
 * returns: never blocks, never calls back into the link. */
typedef void (*plink_service_prefs_done_fn)(void *ctx, uint32_t ticket, plink_service_pset_t result,
                                            const uint8_t why[PROTO_PREFS_F_LAST]);

/* Changes keypad device's settings (LINK-051; the keypad checks each value and applies it through
 * its screen, UI-046). fields is a fields body holding only fields this master knows and the
 * keypad accepts (its "can"). Never blocks: the write is queued for the link task (4 deep), and
 * PLINK_SERVICE_PSET_OK means exactly one done(ctx, ticket, ...) follows - OK with the keypad's
 * answer, or NOT_FOUND, OFFLINE, UNSUPPORTED, BUSY (one in flight to it already), TIMEOUT. Any
 * other return is the refusal now, and nothing follows: BAD, NOT_FOUND (device not 1..8, or the
 * link isn't running as a master), BUSY (the queue is full). Any task. */
plink_service_pset_t plink_service_prefs_set(uint8_t device, uint8_t origin, const uint8_t *fields,
                                             size_t len, plink_service_prefs_done_fn done,
                                             void *ctx, uint32_t ticket);

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
