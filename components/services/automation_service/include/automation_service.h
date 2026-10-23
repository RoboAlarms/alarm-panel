/*
 * automation_service.h - the panel's built-in automations (docs/features/41): the stored rules in
 * NVS (namespace "automations", auto_store's format) and what the touchscreen and the Panel
 * Manager call to read and edit them. Compiled only with CONFIG_ALARM_FEATURE_AUTOMATIONS
 * (AUT-008); without it every reader reports nothing and every edit is refused READ_ONLY.
 *
 * Every reader takes the service's short lock and copies out: nothing hands out a pointer into
 * the store, and no reader waits behind an NVS write. Edits write NVS on the CALLER's task, which
 * must run on an internal-RAM stack (the LVGL task holding board_ui_lock, the HTTPS worker): with
 * flash auto-suspend off, a task whose stack is in PSRAM must never write flash. An edit from such
 * a task is refused FAILED and logged.
 *
 * The rules run on the service's own "auto" task (DESIGN.md section 5): the engine's occurrences
 * come from automation_service_listener(), the device actions go out through device_service.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "alarm_service.h"
#include "automation/auto_run.h"
#include "automation/auto_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The engine listener (AUT-003): register it with alarm_service_add_listener BEFORE
 * alarm_service_start, then report how that went with automation_service_listener_added. The
 * first call sets up the observer and its 32-deep occurrence queue; NULL when that failed. The
 * listener runs on the alarm task, never blocks and never takes a lock; occurrences wait in the
 * queue (the dropped ones counted) until automation_service_start starts the task. */
const alarm_listener_t *automation_service_listener(void);
/* Whether alarm_service_add_listener took it. Without it the automations stay off: the store
 * still loads and edits still work, but nothing runs (a log line says so). */
void automation_service_listener_added(bool ok);

/* Loads the store (once, at start; a second call reloads) and, once the listener was added and
 * the alarm runs, starts the "auto" task. A store that loaded read-only (an NVS read error, a
 * newer meta) still starts: its rules are shown and run, and edits are refused. */
esp_err_t automation_service_start(void);

/* The rules in slot order, copied into out[max]; returns how many. */
size_t automation_service_list(auto_rule_t *out, size_t max);

/* One rule by uid; false when there is none. */
bool automation_service_get(uint16_t uid, auto_rule_t *out);

/* The store's generation (a page sends it back with its edit) and whether it is read-only. */
uint16_t automation_service_gen(void);
bool automation_service_read_only(void);

/* The panel as the validator sees it now (AUT-002, AUT-015): the enabled partitions, each zone's
 * type, partition and class into zones[] (env->zones points at it) and the installer option
 * opt.auto_less_secure, all read from the live configuration on the alarm task
 * (alarm_service_call: never call it from the alarm task); arm_supported true (phase C); the
 * wired outputs set "For automations" (outputs_service_auto_mask, BL-224). False,
 * *env all zero, when the configuration couldn't be read - or without the feature. The editors
 * build their pickers (hc_auto_offer) and confirms from it; every save builds its own. */
bool automation_service_env(auto_env_t *env, auto_zone_t zones[ALARM_MAX_ZONES]);

/* The edits: auto_store_save / _delete / _delete_slot / _set_off under the save mutex. `user`
 * is the signed-in user who saves: every save makes them the maker (an alarm action acts as
 * them, AUT-011; a less secure one runs only while they are a system master or installer,
 * AUT-015). A save checks the rule against automation_service_env, read then (never a caller's
 * env: nobody stores a less secure rule while the option is off, AUT-015), and answers FAILED,
 * nothing written, when it can't be read. Every edit answers AUTO_SAVE_LOCKED while the latest
 * engine snapshot is locked (armed or in alarm, alarm_snapshot_locked: AUT-010); Try it now
 * isn't refused then. */
auto_save_t automation_service_save(const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                                    auto_check_t *why, uint16_t *uid_out);
auto_save_t automation_service_delete(uint16_t uid, uint16_t expected_gen);
auto_save_t automation_service_delete_slot(uint8_t slot, uint16_t expected_gen);
auto_save_t automation_service_set_off(uint16_t uid, bool off, uint16_t expected_gen);

/* The rule's last outcome since the panel started (RAM only; all zero, AUTO_RES_NONE, when it
 * hasn't run, the task doesn't run or there's no such uid). Returns false for an unknown uid. */
bool automation_service_status(uint16_t uid, auto_status_t *out);

/* Try it now: asks the task to run the rule's device actions (auto_run_try) and waits for its
 * answer. AUTOMATION_TRY_BUSY when the task hasn't taken the request within 100 ms (it is then
 * dropped and never runs) or isn't running; once taken, the answer is waited for. The outcome
 * itself comes later in automation_service_status. */
#define AUTOMATION_TRY_BUSY ((auto_try_t)100)
auto_try_t automation_service_try(uint16_t uid);

/* ---- Arming by automation (phase C, DESIGN.md section 3 "Arming" 2 and 4) ---- */

/* The arming warnings and holds now (auto_warning_t.state ON, HOUR, NIGHT or DAY; one per
 * automation), copied into out[max]: the count. RAM only; a restart forgets them (R5). Readers
 * on any task: a short copy under the service's lock, no engine call. */
size_t automation_service_warnings(auto_warning_t *out, size_t max);
/* Automation uid's warning or hold; false (and *out zeroed, state NONE) when it has none. */
bool automation_service_warning(uint16_t uid, auto_warning_t *out);

/* ---- Reminders (BL-224, DESIGN.md section 15) ---- */

/* The reminders' notes showing now ("Not armed yet · reminder at 11:00 PM": a Remind to arm
 * automation found its partition disarmed at its time; on until that partition is armed, 6:00 AM,
 * or the automation changes), copied into out[max]: the count, one per automation, all with on
 * = 1. RAM only; a restart forgets them. Readers on any task: a short copy under the service's
 * lock, no engine call. */
size_t automation_service_reminders(auto_remind_t *out, size_t max);

/* Put off automation uid's warning (auto_run_put_off on the task: AUT-013, D26, D27) for user,
 * whose code the caller has checked with ALARM_CMD_CODE_CHECK on the warning's partition
 * (may_disarm its answer: NIGHT needs it). Asked of the task and waited for as Try it now is:
 * AUTOMATION_POFF_BUSY when the task didn't take it within 100 ms (it then never runs). */
#define AUTOMATION_POFF_BUSY ((auto_put_off_t)100)
auto_put_off_t automation_service_put_off(uint16_t uid, uint8_t choice, uint8_t user,
                                          bool may_disarm);

/* Console command "automations" (read-only, AUT-010): the stored rules, their last outcome and
 * the service's state. */
esp_err_t automation_service_console_register(void);

#ifdef __cplusplus
}
#endif
