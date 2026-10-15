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
