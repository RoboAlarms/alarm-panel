/*
 * alarm_service.h - runs alarm_core on its own FreeRTOS task.
 *
 * The alarm task owns the engine. Everyone else talks to it through this API:
 *   - sensor inputs are posted (never block),
 *   - commands and queries are synchronous calls executed on the alarm task,
 *   - status is published as a snapshot plus a change counter the UI can poll,
 *   - listeners get events, reports, sounds and outputs (called on the alarm task).
 *
 * Listener callbacks must be quick, must not block and must never call the synchronous
 * functions below (the alarm task would wait for itself).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_engine.h"
#include "alarm/alarm_keyseq.h"
#include "alarm/alarm_snapshot.h"
#include "esp_err.h"
#include "zone_hub/zone_bind_gate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keypad source ids (ALARM_KEYPAD_LOCAL, ALARM_KEYPAD_CONSOLE) are in alarm/alarm_types.h. */

typedef struct {
    void *ctx;
    void (*on_event)(void *ctx, const alarm_event_t *ev);  /* every event */
    void (*on_report)(void *ctx, const alarm_event_t *ev); /* due for remote reporting */
    void (*on_sound)(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone);
    void (*on_output)(void *ctx, alarm_output_t output, uint8_t partition,
                      alarm_pattern_t pattern);
    void (*on_snapshot)(void *ctx, const alarm_snapshot_t *snap); /* after any change */
    /* After a configuration change: the zones whose input conditions the engine forgot (new,
     * removed, retyped or moved; alarm_zone_config_resets_state), bit = zone - 1,
     * ALARM_ZONE_BITMAP_BYTES long. Whoever feeds them tells the engine again (zone_service). */
    void (*on_zones_reset)(void *ctx, const uint8_t *zones);
} alarm_listener_t;

/* Work executed on the alarm task (see alarm_service_call). */
typedef void (*alarm_service_fn)(alarm_engine_t *e, alarm_ms_t now, void *ctx);

/* Register before alarm_service_start() to also see the start-up event. */
esp_err_t alarm_service_add_listener(const alarm_listener_t *listener);

/* Event numbers continue after this one (the newest in the flash event log), even when the
 * stored runtime state is older or gone. Call before alarm_service_start(). */
void alarm_service_set_min_event_seq(uint32_t seq);

/* Loads configuration and runtime state from NVS (factory defaults on first boot),
 * starts the engine and the alarm task. Needs nvs_flash_init() first.
 *
 * Failures here are fatal to the panel, not something to carry on past: code hashing that does
 * not work (ESP_ERR_INVALID_STATE) would leave a panel nobody can disarm, and a configuration
 * that cannot be read (ESP_ERR_NO_MEM, an NVS error) must not be replaced with factory
 * defaults - the stored installation is left untouched and the caller should restart the panel
 * so the read can be tried again. */
esp_err_t alarm_service_start(void);

/* Durability of what the engine has done. ESP_OK when everything it changed is on flash.
 * Otherwise a write is still outstanding and the value is the last write error, or
 * ESP_ERR_NOT_FINISHED before the first attempt; ESP_ERR_INVALID_STATE when stored data this
 * firmware could not use is still there. The panel also raises ALARM_TRBL_STORAGE meanwhile.
 * Safe from any task. */
esp_err_t alarm_service_store_status(void);

/* Monotonic engine time (ms since boot). */
alarm_ms_t alarm_service_now(void);

/* ---- inputs (asynchronous, safe from any task, also before alarm_service_start) --- */
void alarm_service_zone_event(uint16_t zone, alarm_zone_event_t ev);
void alarm_service_system_trouble(alarm_sys_trouble_t trouble, bool active);
/* CORE-TRBL-001, P4-013, ZW-039: owner raises or clears its hold on ALARM_TRBL_STORAGE
 * (alarm_engine_storage_trouble on the alarm task): it stands while any owner holds it. */
void alarm_service_storage_trouble(alarm_storage_owner_t owner, bool active);
void alarm_service_device_checkin(uint8_t device);
/* CORE-TRBL-008, ZW-038: a device the platform can't reach (false) or hears again (true):
 * alarm_engine_device_reachable on the alarm task. */
void alarm_service_device_reachable(uint8_t device, bool reachable);

/* ---- synchronous calls (block until the alarm task has run them) -------------- */
esp_err_t alarm_service_call(alarm_service_fn fn, void *ctx);

/* The same work, without waiting for it: fn runs on the alarm task whenever it gets to it, and
 * ctx must outlive that. ESP_ERR_INVALID_STATE before the task exists, ESP_FAIL when the queue
 * is full (the alarm task is behind). Use it where the caller must never wait for the engine -
 * the start-up health check asks the alarm task to report that it is still running this way, so
 * a wedged engine stalls nothing else (docs/features/13 OTA-005). */
esp_err_t alarm_service_post(alarm_service_fn fn, void *ctx);

/* Only inside work running on the alarm task (an alarm_service_fn): how many messages - zone
 * events, troubles, commands, other calls - wait behind it, not yet seen by the engine. The
 * update's final restart (docs/features/13 OTA-004, R17) goes only when this is 0, so nothing
 * already handed to the panel is lost to it. */
size_t alarm_service_waiting(void);

/* CORE-ARM-013: hold arming (a sensor radio updating its own firmware), or end the hold. Posted,
 * never waited for; false when the alarm task's queue was full (call again). */
bool alarm_service_hold_arming(bool hold);

/* ZSRC-012, RF32: a zone binding change's reservation (zone_hub/zone_bind_gate.h), so arming
 * can't pass between the disarmed check and the change:
 *  - alarm_service_bind_reserve: waits for the alarm task (never call it holding the zone lock or
 *    a source's lock): true when every partition is disarmed. It holds nothing: an arm after it
 *    goes ahead and cancels it;
 *  - alarm_service_bind_claim: never waits, call it under the zone lock just before the change:
 *    ZONE_BIND_CLAIMED - change now, arming held (CORE-ARM-013's hold, never shown as "not
 *    ready") until the end; ZONE_BIND_CLAIM_RETRY - an engine step runs: let the zone lock go,
 *    wait a tick, claim again; ZONE_BIND_CLAIM_REFUSED - armed meanwhile or ZONE_BIND_GATE_MS
 *    passed: change nothing;
 *  - alarm_service_bind_end: never waits, after the in-memory change or a refused claim.
 * One changer at a time across the panel (ZB21: zone_service's binding changes, the Zigbee
 * task's requests and device replacement): a successful reserve makes the calling task the
 * gate's owner until its end; another task's reserve waits for it (bounded, ~0.5 s), then is
 * told busy. Claim and end from a task that doesn't own the gate change nothing (REFUSED, no-op),
 * so no task ever ends another's change. After a successful reserve, always end.
 *  - alarm_service_bind_reserve_why: alarm_service_bind_reserve saying why not - ZONE_BIND_ARMED
 *    (a partition is not disarmed: the caller's "not allowed") or ZONE_BIND_BUSY (another change
 *    holds the gate, the alarm task can't be asked, or this task already holds a reservation);
 *  - alarm_service_bind_mine: this task holds a claimed change (CLAIMED, not yet ended) - a
 *    change made inside a source's own hook (Zigbee's rebind) goes only then (zone_service). */
bool alarm_service_bind_reserve(void);
zone_bind_reserve_t alarm_service_bind_reserve_why(void);
zone_bind_claim_t alarm_service_bind_claim(void);
void alarm_service_bind_end(void);
bool alarm_service_bind_mine(void);

alarm_result_t alarm_service_command(const alarm_cmd_t *cmd, const alarm_source_t *src,
                                     alarm_cmd_result_t *result /* may be NULL */);
/* The state a command given without a code needs (alarm_service_command_as). */
typedef enum {
    ALARM_SVC_WHEN_ANY = 0,      /* any state (acknowledge) */
    ALARM_SVC_WHEN_DISARMED,     /* the partition disarmed and installer mode closed (arm, clear
                                  * the memory, chime, bypass) */
    ALARM_SVC_WHEN_NOT_DISARMED, /* in exit time, armed, in entry time, or an alarm sounding while
                                  * disarmed - burglary, panic, fire, CO or auxiliary (disarm) */
} alarm_svc_when_t;

/* alarm_engine_command_as() on the alarm task (CORE-USR-027): a command as a user the caller
 * has already identified (a Panel Manager sign-in), without a code. `when` is checked on the
 * alarm task in the same call that runs the command, so nothing can arm or disarm the partition
 * in between (a keypad, MQTT, a keyfob): when the partition is not in that state the command is
 * not run and the answer is ALARM_ERR_BAD_STATE (result, if given, left zeroed). */
alarm_result_t alarm_service_command_as(const alarm_cmd_t *cmd, const alarm_source_t *src,
                                        uint8_t user, alarm_svc_when_t when,
                                        alarm_cmd_result_t *result /* may be NULL */);
alarm_result_t alarm_service_authenticate(const alarm_source_t *src, const char *code,
                                          uint8_t partition, uint8_t *user_out);
/* alarm_service_authenticate(), and the authority of the user the code belongs to (read on the
 * alarm task in the same call; ALARM_AUTH_NONE unless the code is accepted). */
alarm_result_t alarm_service_authenticate_as(const alarm_source_t *src, const char *code,
                                             uint8_t partition, uint8_t *user_out,
                                             alarm_authority_t *authority_out);
alarm_result_t alarm_service_user_set(const alarm_source_t *src, const char *actor_code,
                                      uint8_t user_no, const char *new_code,
                                      alarm_authority_t authority, uint8_t partitions,
                                      const char *name);
alarm_result_t alarm_service_user_delete(const alarm_source_t *src, const char *actor_code,
                                         uint8_t user_no);
/* Runs a completed keypad sequence (ALARM_KS_COMMAND, _USER_SET, _USER_DELETE) with
 * keypad semantics: adding a user from a keypad creates a standard user in the
 * keypad's partition; changing an existing user's code keeps its authority, partitions and
 * name. Other kinds are handled by the keypad itself and return ALARM_ERR_BAD_ARG. */
alarm_result_t alarm_service_keypad_execute(const alarm_keyseq_result_t *ks,
                                            const alarm_source_t *src,
                                            alarm_cmd_result_t *result /* may be NULL */);
alarm_result_t alarm_service_user_update(const alarm_source_t *src, const char *actor_code,
                                         uint8_t user_no, alarm_authority_t authority,
                                         uint8_t partitions, const char *name);
/* CORE-USR-028: the same three as a user the caller has already identified (a Panel Manager
 * sign-in), without a code: the engine reads the actor's slot again and applies that user's
 * authority; nothing counts toward a keypad lockout. */
alarm_result_t alarm_service_user_set_as(const alarm_source_t *src, uint8_t actor,
                                         uint8_t user_no, const char *new_code,
                                         alarm_authority_t authority, uint8_t partitions,
                                         const char *name);
alarm_result_t alarm_service_user_update_as(const alarm_source_t *src, uint8_t actor,
                                            uint8_t user_no, alarm_authority_t authority,
                                            uint8_t partitions, const char *name);
alarm_result_t alarm_service_user_delete_as(const alarm_source_t *src, uint8_t actor,
                                            uint8_t user_no);

/* Installer mode (see alarm_engine_program_enter): code NULL or "" uses the power-up recovery. */
alarm_result_t alarm_service_program_enter(const alarm_source_t *src, const char *code,
                                           uint8_t *user_out);
void alarm_service_program_exit(const alarm_source_t *src);
void alarm_service_program_keepalive(void);
alarm_result_t alarm_service_program_set_code(const alarm_source_t *src, uint8_t user_no,
                                              const char *new_code);
/* CORE-PROG-011: installer mode for the signed-in installer, without a code (see
 * alarm_engine_program_enter_as); never the power-up recovery. */
alarm_result_t alarm_service_program_enter_as(const alarm_source_t *src, uint8_t user);

/* Installer programming: validates, applies and stores a new configuration. */
alarm_result_t alarm_service_set_config(const alarm_config_t *cfg);
/* Remote programming is one alarm-task transaction: check the observed revision, enter
 * installer mode as the signed-in user (alarm_engine_program_enter_as: the installer only),
 * apply and leave. It cannot borrow or terminate a local installer's existing session. */
alarm_result_t alarm_service_program_apply_as(const alarm_source_t *src, uint8_t user,
    uint32_t expected_revision, const alarm_config_t *cfg);
/* Copies the current configuration (about 10 KB; allocate it off the stack). */
esp_err_t alarm_service_get_config(alarm_config_t *out);
/* A changed sensor loses its zones' spoken-name approval (docs/features/08 voice-first-build.md
 * D07): one alarm-task transaction copies the running configuration into 'scratch' (caller-
 * allocated, about 10 KB, PSRAM is fine), runs alarm_config_voice_invalidate_zones() on it for
 * 'zones' (bit zone - 1, ALARM_ZONE_BITMAP_BYTES) and applies it as alarm_service_set_config()
 * does, so an edit made meanwhile is never reverted by a stale copy. *written (optional) is
 * how many zones it invalidated: 0 (and ALARM_OK) when no named zone is in use, and nothing was
 * written. Otherwise durability is alarm_service_store_status()'s, read on return. Never call
 * it holding a lock the alarm task may wait for. */
alarm_result_t alarm_service_voice_invalidate(const uint8_t *zones, alarm_config_t *scratch,
                                              unsigned *written);

/* ---- status --------------------------------------------------------------------- */
uint32_t alarm_service_snapshot_seq(void);
void alarm_service_get_snapshot(alarm_snapshot_t *out);

/* Most recent events, newest first. Returns the number copied. */
size_t alarm_service_recent_events(alarm_event_t *out, size_t max);

/* First step of a factory reset: records the factory_reset event (arg bit 0 = the event log
 * goes too) and stops every further write to NVS. The caller then erases storage and restarts
 * (panel_system_factory_reset). */
esp_err_t alarm_service_factory_reset(const alarm_source_t *src, uint8_t user, bool erase_log);

/* Records a backup made (ALARM_EV_BACKUP_MADE) or about to be restored
 * (ALARM_EV_BACKUP_RESTORED, docs/features/14 CFG-006). Restoring also stops every further
 * write of the engine's state to NVS, as a factory reset does: the caller then writes the
 * file's entries and restarts (components/services/backup_service). src NULL = the system. */
esp_err_t alarm_service_backup_event(bool restoring, const alarm_source_t *src, uint8_t user);

/* Records a firmware update step (ALARM_EV_FIRMWARE_UPDATE, logged and reported like any
 * event). src NULL = the system. */
esp_err_t alarm_service_firmware_event(alarm_fw_stage_t stage, const alarm_source_t *src,
                                       uint8_t user);

/* Records a platform event with a zone and a second argument - the Z-Wave network's
 * (ALARM_EV_ZWAVE_*, docs/features/36 network.md §15) and the Zigbee network's
 * (ALARM_EV_ZIGBEE_*, docs/features/20 devices.md §11: arg the registry id, 0 over 255; arg2 the
 * type's own value) - logged and reported like any event (alarm_engine_platform_event_ex). src
 * NULL = the system. Waits for the alarm task: not from it, and not holding a lock the alarm task
 * could want (take the events out under the zone lock, then post them after letting it go). */
esp_err_t alarm_service_platform_event(alarm_event_type_t type, uint16_t zone, uint8_t arg,
                                       uint8_t arg2, const alarm_source_t *src, uint8_t user);

/* Someone put automation uid's arming of partition off (docs/features/41 AUT-013, D26, D30):
 * ALARM_EV_ARMING_PUT_OFF by user (whose code), source {ALARM_SRC_AUTOMATION, uid}, choice an
 * alarm_put_off_t (ALARM_PUT_OFF_HOUR, _NIGHT, or _DAY for "Not today"), until_epoch_ms the new
 * arm time for an hour (0: not known). Logged and reported at once (alarm_engine_arming_put_off).
 * ESP_ERR_INVALID_ARG for a field out of range. Waits for the alarm task: not from it. */
esp_err_t alarm_service_arming_put_off(uint16_t uid, uint8_t partition, uint8_t user,
                                       alarm_put_off_t choice, int64_t until_epoch_ms);

#ifdef __cplusplus
}
#endif
