/*
 * device_service.h - the Devices registry on the panel (docs/features/24, M13): home_control with
 * its own lock, the Home Assistant device source fed by ha_link, and what the UI reads.
 * Compiled only with CONFIG_ALARM_HOME_CONTROL (HCTL-010); without it every call is a no-op
 * that reports nothing.
 *
 * Every function is safe from any task. Actions are asynchronous: hc's port reports results;
 * ha_link sends the calls from its own task (device_service only queues through the sender).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h" /* alarm_source_t, alarm_result_t: device_service_person_act */
#include "esp_err.h"
#include "home_control/device_source.h" /* dev_values_t: ha_link feeds them in every build */
#if CONFIG_ALARM_HOME_CONTROL
#include "home_control/home_control.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the registry. Everything below reports "not available" until this has succeeded, and
 * a failure leaves nothing allocated, so it can simply be called again. */
esp_err_t device_service_start(void);

/* A change counter (docs/features/11 "Device state", BL-216): it moves whenever the registry may
 * show something new - a source reported a change, an action ended (its answer, "timeout",
 * "offline"), an action was sent (the row shows it pending), or a send its source refused at once
 * cleared the failure the row showed (home_control's port.changed) - so a reader that copies the
 * list when it moves (the panel link's device builds) misses nothing it would have to poll for.
 * Only equality means anything; it wraps. Lock-free: safe from any task, before the start too. 0
 * without home control. */
uint32_t device_service_seq(void);

/* What became of an action a person asked for (device_service_person_act). The same numbers as
 * the touchscreen's ui_dev_result_t and the panel link's DEV_ACK result (asserted where both are
 * known): append only. */
typedef enum {
    DS_PERSON_OK = 0,        /* sent: the device's row follows it */
    DS_PERSON_NEED_CODE,     /* the action needs a user code (HCTL-005) and none came */
    DS_PERSON_CODE_REFUSED,  /* the code wasn't accepted: *why says why (lockout included) */
    DS_PERSON_BUSY,          /* an action is already under way on it */
    DS_PERSON_UNREACHABLE,   /* its source is offline */
    DS_PERSON_REFUSED,       /* no such device, one this screen can't act on, or the action
                              * doesn't fit it */
} ds_person_t;

/* One judge for both screens (docs/features/11 "Device actions", LINK-048; HCTL-005): an action a
 * person asked for at a keypad - the panel's own screen ({ALARM_SRC_KEYPAD, ALARM_KEYPAD_LOCAL},
 * partition 0: a code of any partition) or a link keypad ({ALARM_SRC_KEYPAD, n}, its home
 * partition). In order:
 *   - REFUSED for an unknown ref, a Z-Wave row (Smart Home doesn't show them yet, BL-169) or an
 *     action that doesn't fit the device's live kind (hc_action_valid);
 *   - a device that asks for a master code (hc_home.h, BL-220 R5): NEED_CODE without a code,
 *     unless the same src gave a master's code for such a device within DEVICE_SERVICE_GRANT_MS;
 *     with one, only a code of a master (system or partition) or the installer is accepted
 *     (anything else CODE_REFUSED, ALARM_ERR_NOT_AUTHORIZED), and it covers an unlock too;
 *   - where hc_needs_code says so (unlock, an opening), NEED_CODE without a code; with one, the
 *     engine checks it for src and partition like any keypad code (a wrong one counts toward
 *     src's lockout); an arm-only or a guest code is refused as ALARM_ERR_NOT_AUTHORIZED, a
 *     duress code acts as asked without the silent report (decided in BL-045): CODE_REFUSED with
 *     *why;
 *   - ACT_SET_MODE takes one DEV_MODE_* bit in mode_bit (hc_mode_word): any other is REFUSED;
 *     other actions ignore it;
 *   - device_service_act: OK, BUSY, UNREACHABLE (offline), REFUSED (anything else).
 * value is device_service_act's, passed on unchanged. code NULL or "" for none; *why (may be NULL)
 * is ALARM_OK except with CODE_REFUSED. Blocks while the alarm task checks a code: never call it
 * on the alarm task or holding the device lock. Who did it isn't logged yet (HCTL-007). Without
 * home control: REFUSED. */
ds_person_t device_service_person_act(const alarm_source_t *src, uint8_t partition,
                                      const char *ref, uint8_t action, int32_t value,
                                      uint8_t mode_bit, const char *code, alarm_result_t *why);

/* ---- the house's Smart Home settings (home_control/hc_home.h, BL-220) ----------------------
 * One blob, the same for every screen: the rooms, where a person put each device, the devices
 * that ask for a master code, and whether arranging asks for one. Kept in NVS ("ui", "dev_home")
 * and in RAM, read at the start. Without home control: nothing is stored. */

/* The settings as stored: their length, 0 when there are none (the defaults) or max is short. */
size_t device_service_home_get(uint8_t *out, size_t max);

/* Stores new settings (a blob hc_home_decode accepts) and moves device_service_seq, so the keypads'
 * device list follows. Writes NVS: call it only from a task whose stack is in internal RAM (the
 * touchscreen's). False when the blob isn't one or couldn't be stored. */
bool device_service_home_set(const uint8_t *blob, size_t len);

#if CONFIG_ALARM_HOME_CONTROL
/* For a screen that knows no rooms (a link keypad, HCTL-013): each row's area becomes its room's
 * name ("" none), and the rows are put in the rooms' order, a room's devices keeping the
 * registry's order among themselves. */
void device_service_home_apply(hc_device_t *rows, size_t n);
#endif

/* How long a master code, once given for a device that asks for one, lets the same screen change
 * such devices without asking again (BL-220 D7): from the last such change. */
#define DEVICE_SERVICE_GRANT_MS 90000u

/* Expires a device command whose reply never came (HCTL-012). Call it about once a second with
 * the monotonic clock (alarm_service_now()); safe before device_service_start(). */
void device_service_tick(uint64_t now_ms);

/* ---- fed by ha_link ------------------------------------------------------------------------ */
void device_service_ha_link(bool up);
void device_service_ha_catalog_begin(void);
/* AUT-006: the automations' "missing vs unreachable" - true for DSRC_HA once its link has been up
 * and a catalog has begun arriving, both for 30 s or more (a link drop starts it again); false for
 * every other source (Z-Wave is docs/features/41 phase B) and without home control. now_ms is
 * alarm_service_now()'s clock. Lock-free: safe from any task. */
bool device_service_source_settled(uint8_t source, uint64_t now_ms);
/* One catalog entity marked control:true (protocol.md), with its level, setpoint, temperature
 * and modes as far as they are known (values NULL: none). */
void device_service_ha_catalog_add(const char *entity_id, const char *name, const char *area,
                                   const char *domain, const char *state,
                                   const dev_values_t *values);
/* A state push; its values replace the last ones (NULL: none known now). */
void device_service_ha_state(const char *entity_id, const char *state, bool available,
                             const dev_values_t *values);
/* A call_result came back. */
void device_service_ha_result(uint32_t request_id, bool ok, const char *error);

/* ha_link's sender: queue {"t":"call"} for its task (never send on the caller's task). */
typedef bool (*device_service_send_fn)(void *ctx, const char *entity, const char *action,
                                       bool has_value, int32_t value, const char *mode,
                                       uint32_t request_id);
void device_service_set_sender(device_service_send_fn fn, void *ctx);

#if CONFIG_ALARM_HOME_CONTROL
/* ---- the UI and the console ---- */
size_t device_service_list(hc_device_t *out, size_t max); /* copies, newest state */
/* One device by its registry key (a copy); false when there is no such device. */
bool device_service_get(const char *ref, hc_device_t *out);
/* HCTL-005/HCTL-006: the caller checked the code already where hc_needs_code says so. */
hc_result_t device_service_act(const char *ref, dev_action_t action, int32_t value,
                               const char *mode);
/* HCTL-011 / AUT-002 / AUT-015: an automation's action - hc_act_auto under the same lock:
 * refused with HC_ERR_NOT_ALLOWED outside what an automation may do (docs/features/41 DESIGN
 * section 3, ADR-050), judged by the device's live kind; an unlock, open or stop only with
 * less_secure (the installer option "Allow less secure automations"). *request_id (may be NULL)
 * gets the id the result listener will report, 0 when nothing was sent. */
hc_result_t device_service_act_auto(const char *ref, dev_action_t action, int32_t value,
                                    const char *mode, bool less_secure, uint32_t *request_id);

/* HCTL-012: every sent action's outcome, with its request id: the answer, "timeout", or
 * "offline" (its connection or its device went away). ref may name a device that is gone.
 * Called WITH THE DEVICE LOCK HELD, on whichever task drove the registry (ha_link's, the
 * health task's tick, a caller of device_service_*): the listener must only copy what it needs
 * and enqueue it - never call any device_service_* function (the lock isn't recursive, so that
 * deadlocks) and never block. ref and error are valid only during the call. */
typedef void (*device_service_result_fn)(void *ctx, uint32_t request_id, const char *ref,
                                         bool ok, const char *error);
/* One listener; a second call replaces it, NULL clears it. Safe before device_service_start. */
void device_service_set_result_listener(device_service_result_fn fn, void *ctx);
#endif

/* Console command "devices" (list, act). */
esp_err_t device_service_console_register(void);

#ifdef __cplusplus
}
#endif
