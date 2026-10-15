/*
 * report.c - store-and-forward of reportable events from the event log (see proto_report.h).
 */
#include <string.h>

#include "alarm_proto/proto_report.h"

void proto_report_init(proto_report_t *r, const proto_report_port_t *port, uint32_t acked,
                       bool enabled, alarm_ms_t now)
{
    memset(r, 0, sizeof(*r));
    r->port = *port;
    r->acked = acked;
    r->enabled = enabled;
    r->down_since = now;
}

void proto_report_enable(proto_report_t *r, bool enabled, alarm_ms_t now)
{
    if (enabled && !r->enabled) {
        r->down_since = now;
        r->waiting = false;
    }
    r->enabled = enabled;
}

void proto_report_connected(proto_report_t *r, alarm_ms_t now)
{
    (void)now;
    r->connected = true;
    r->inflight = 0;
    r->live_from = r->port.newest_index(r->port.ctx) + 1u;
}

void proto_report_disconnected(proto_report_t *r, alarm_ms_t now)
{
    if (r->connected) {
        r->down_since = now;
    }
    r->connected = false;
    r->inflight = 0;
}

bool proto_report_poll(proto_report_t *r, alarm_ms_t now)
{
    if (!r->connected) {
        return false;
    }
    if (r->inflight != 0u) {
        if (now - r->sent_at < PROTO_ACK_TIMEOUT_MS) {
            return false;
        }
        r->inflight = 0; /* no acknowledgement: publish it again */
    }
    const uint32_t newest = r->port.newest_index(r->port.ctx);
    if (r->acked > newest) {
        r->acked = newest; /* the log was cleared: nothing older can be sent */
        r->acked_dirty = true;
    }
    elog_entry_t e;
    uint32_t next = r->acked;
    if (r->port.read_after(r->port.ctx, r->acked, &e, 1, PROTO_REPORT_SCAN, &next) == 0u) {
        if (next > r->acked) {
            r->acked = next; /* only other records in between */
            r->acked_dirty = true;
            return true;
        }
        r->waiting = false; /* all delivered */
        return false;
    }
    if (e.index - 1u > r->acked) {
        r->acked = e.index - 1u; /* no marker before it: never scanned again */
        r->acked_dirty = true;
    }
    if (!r->waiting) {
        r->waiting = true;
        r->waiting_since = now;
    }
    const int id = r->port.publish(r->port.ctx, &e, e.index < r->live_from);
    if (id <= 0) {
        return false;
    }
    r->inflight = e.index;
    r->msg_id = id;
    r->sent_at = now;
    return true;
}

void proto_report_acked(proto_report_t *r, int msg_id, alarm_ms_t now)
{
    if (r->inflight == 0u || msg_id != r->msg_id) {
        return;
    }
    r->acked = r->inflight;
    r->acked_dirty = true;
    r->inflight = 0;
    r->waiting_since = now; /* progress; the next poll clears 'waiting' when nothing is left */
}

bool proto_report_comm_fail(const proto_report_t *r, alarm_ms_t now)
{
    if (!r->enabled) {
        return false;
    }
    if (!r->connected && now - r->down_since >= PROTO_COMM_FAIL_MS) {
        return true;
    }
    return r->waiting && now - r->waiting_since >= PROTO_COMM_FAIL_MS;
}

bool proto_report_waiting(const proto_report_t *r)
{
    return r->inflight != 0u || r->waiting;
}
