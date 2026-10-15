/*
 * proto_report.h - store-and-forward of reportable events to an MQTT broker (docs/features/10
 * MQTT-005, -006, -011; docs/features/05 LOG-006).
 *
 * The event log is the queue: every event that becomes due for remote reporting gets a
 * "reported" marker in the log (ELOG_KIND_REPORTED). The reporter publishes the markers in log
 * order, one at a time, and only moves on when the broker acknowledged the publish (QoS 1).
 * 'acked' is the log index up to which everything is delivered; the owner saves it now and
 * then, so after a reconnect or a restart the rest is sent again (at least once: a consumer
 * drops duplicates by the event's seq). Portable C11: the log, the clock and the broker are
 * reached through the port.
 */
#ifndef ALARM_PROTO_REPORT_H
#define ALARM_PROTO_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "event_log/elog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_COMM_FAIL_MS    300000u /* MQTT-011: 5 minutes */
#define PROTO_ACK_TIMEOUT_MS  30000u  /* publish again when the broker doesn't acknowledge */
#define PROTO_REPORT_SCAN     256u    /* log records looked at per poll */

typedef struct {
    void *ctx;
    /* elog_read_after() with a filter for ELOG_KIND_REPORTED (max entries, oldest first). */
    size_t (*read_after)(void *ctx, uint32_t after, elog_entry_t *out, size_t max,
                         uint32_t scan, uint32_t *next_after);
    /* elog_newest_index(). */
    uint32_t (*newest_index)(void *ctx);
    /* Publish one event with QoS 1. Returns the message id (> 0), or -1 when it can't be sent
     * now (tried again on the next poll). */
    int (*publish)(void *ctx, const elog_entry_t *marker, bool replayed);
} proto_report_port_t;

typedef struct {
    proto_report_port_t port;
    uint32_t acked;          /* every marker up to this log index was acknowledged */
    uint32_t inflight;       /* log index of the marker being published, 0 = none */
    int msg_id;              /* its message id */
    alarm_ms_t sent_at;
    uint32_t live_from;      /* markers from this index on became due while connected */
    bool enabled;            /* reporting is configured (comm_fail only then) */
    bool connected;
    alarm_ms_t down_since;   /* broker unreachable since (valid while !connected) */
    bool waiting;            /* a report is waiting for the broker */
    alarm_ms_t waiting_since;/* ... without progress since */
    bool acked_dirty;        /* acked moved since the owner last saved it */
} proto_report_t;

/* acked: the saved position. Without one (reporting just set up) pass the log's newest index
 * so the history before isn't sent. */
void proto_report_init(proto_report_t *r, const proto_report_port_t *port, uint32_t acked,
                       bool enabled, alarm_ms_t now);

/* Reporting configured or not (unconfigured never raises comm_fail). */
void proto_report_enable(proto_report_t *r, bool enabled, alarm_ms_t now);

/* The broker connection came up (call after the log caught up) / went down. What was in
 * flight is published again after the next connect. */
void proto_report_connected(proto_report_t *r, alarm_ms_t now);
void proto_report_disconnected(proto_report_t *r, alarm_ms_t now);

/* Publishes the next marker when nothing is in flight (or the acknowledgement timed out).
 * Returns true when it did something and should be called again right away. */
bool proto_report_poll(proto_report_t *r, alarm_ms_t now);

/* The broker acknowledged a message; moves on when it's the one in flight. */
void proto_report_acked(proto_report_t *r, int msg_id, alarm_ms_t now);

/* MQTT-011: reporting is configured and the broker has been unreachable, or a report has
 * waited without progress, for PROTO_COMM_FAIL_MS. */
bool proto_report_comm_fail(const proto_report_t *r, alarm_ms_t now);

/* Something is waiting for the broker (in flight, or found and not yet sent). */
bool proto_report_waiting(const proto_report_t *r);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PROTO_REPORT_H */
