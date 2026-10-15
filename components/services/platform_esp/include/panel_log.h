/*
 * panel_log.h - the event log on the "eventlog" flash partition (components/core/event_log).
 *
 * Engine listeners queue records here and never wait for flash; a low-priority writer task
 * appends them. Readers (UI, console) take the log's lock. See docs/features/05-Event-Log.
 *
 * Losing a record is a defect, not a statistic (LOG-010): the queue keeps slots back for
 * reportable events, the writer tries a failed append again, a reportable event that still
 * can't be kept is offered to every handler registered with panel_log_add_lost_handler()
 * for immediate delivery, and whatever no one could take is counted in NVS and holds the
 * storage trouble until it is cleared.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "alarm_service.h"
#include "esp_err.h"
#include "event_log/elog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the log (scans the partition, advances the boot counter) and starts the writer task.
 * Call after nvs_flash_init() and before alarm_service_start(). Without a usable partition the
 * panel keeps running and raises the storage trouble. */
esp_err_t panel_log_start(void);

/* Listener that feeds the log: register it with alarm_service_add_listener(). */
const alarm_listener_t *panel_log_listener(void);

/* Newest engine event sequence number on flash; the engine continues after it. */
uint32_t panel_log_last_seq(void);

/* This boot's number (stamped on every record; the UI compares it to tell "since power-up"). */
uint32_t panel_log_boot(void);

/* Blocks until everything queued is on flash, or timeout_ms passes. */
bool panel_log_flush(uint32_t timeout_ms);

/* elog_read() under the log's lock (see event_log/elog.h). */
size_t panel_log_read(uint32_t before, elog_filter_fn filter, void *filter_ctx,
                      elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before);

/* elog_read_after() under the log's lock: oldest first, after a log index (MQTT replay). */
size_t panel_log_read_after(uint32_t after, elog_filter_fn filter, void *filter_ctx,
                            elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_after);

/* Index of the newest record on flash, 0 when empty. */
uint32_t panel_log_newest_index(void);

/* The log is mounted and usable. */
bool panel_log_ready(void);

/* Records kept now, and the guaranteed capacity. */
void panel_log_stats(uint32_t *count, uint32_t *capacity);

/* Completed passes of the writer task. It turns at least once a second whether or not there is
 * anything to write, so a value that stops moving means the log stopped running - which is what
 * the panel's health check reports (docs/features/13 OTA-005). Safe from any task. */
uint32_t panel_log_passes(void);

/* ---- records that were lost (LOG-010) ------------------------------------------------------ */

/* A reportable event the log could not keep. Return true after taking it over for immediate
 * delivery (net_mqtt publishes it live, without replay; net_notify pushes it to a phone).
 * Called on the alarm task or on the log's writer task, so it must not block and must not
 * call back into the engine.
 *
 * Every registered handler is offered the event, because they are different destinations and
 * one taking it does not tell the others: the record counts as delivered when any of them
 * returns true, and as lost when none does (or when there are none). Register once each,
 * after panel_log_start() and before the panel is in service; ESP_ERR_NO_MEM when the slots
 * are full. */
#define PANEL_LOG_LOST_HANDLERS 2
typedef bool (*panel_log_lost_fn)(void *ctx, const alarm_event_t *ev);
esp_err_t panel_log_add_lost_handler(panel_log_lost_fn fn, void *ctx);
/* Gives a slot back (a component whose start-up failed half way). */
void panel_log_remove_lost_handler(panel_log_lost_fn fn, void *ctx);

/* Records that never reached the flash, and how many of those were reportable events nobody
 * could deliver. Both are kept in NVS and latch: a later successful write never clears them. */
void panel_log_loss(uint32_t *records, uint32_t *reports);

/* Forget the latched loss (a service action: it drops the storage trouble with it). */
void panel_log_clear_loss(void);

/* Erase every record (factory reset). */
esp_err_t panel_log_clear(void);

#ifdef __cplusplus
}
#endif
