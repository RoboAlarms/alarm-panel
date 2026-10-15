/*
 * zw_health.c - wake-up handling and supervision per node (see zw_health.h, network.md §7-§8).
 */
#include <string.h>

#include "zwave_host/zw_health.h"

/* ---- interval choice (§7, pure) ----------------------------------------------------------------- */

uint32_t zw_health_choose_interval(uint32_t wanted_s, uint32_t min_s, uint32_t max_s, uint32_t step_s)
{
    if (wanted_s == ZW_HEALTH_INTERVAL_NEVER) {
        return ZW_HEALTH_INTERVAL_NEVER;
    }
    uint32_t v = wanted_s;
    if (v < min_s) {
        v = min_s;
    }
    if (max_s > 0u && v > max_s) {
        v = max_s;
    }
    if (step_s > 0u && v > min_s) {
        const uint32_t offset = v - min_s;
        const uint32_t rounded = ((offset + step_s / 2u) / step_s) * step_s;
        v = min_s + rounded;
        if (v < min_s) {
            v = min_s;
        }
        if (max_s > 0u && v > max_s) {
            v = max_s;
        }
    }
    return v;
}

/* ---- init and node lookup ------------------------------------------------------------------------ */

void zw_health_init(zw_health_t *h)
{
    memset(h, 0, sizeof(*h));
}

static zw_health_node_t *find_node(zw_health_t *h, uint16_t node_id)
{
    if (node_id == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < h->node_count; i++) {
        if (h->nodes[i].node_id == node_id) {
            return &h->nodes[i];
        }
    }
    return NULL;
}

static const zw_health_node_t *find_node_const(const zw_health_t *h, uint16_t node_id)
{
    return find_node((zw_health_t *)h, node_id);
}

/* ---- node lifecycle -------------------------------------------------------------------------- */

bool zw_health_add(zw_health_t *h, uint16_t node_id, bool is_listening, uint32_t interval_s)
{
    if (node_id == 0u || h->node_count >= ZW_HEALTH_NODES_MAX || find_node(h, node_id) != NULL) {
        return false;
    }
    zw_health_node_t *node = &h->nodes[h->node_count];
    memset(node, 0, sizeof(*node));
    node->node_id = node_id;
    node->is_listening = is_listening;
    node->interval_s = interval_s;
    node->reachable = true; /* assumed until disproven */
    h->node_count++;
    return true;
}

bool zw_health_remove(zw_health_t *h, uint16_t node_id)
{
    for (size_t i = 0; i < h->node_count; i++) {
        if (h->nodes[i].node_id == node_id) {
            const size_t rest = h->node_count - i - 1u;
            if (rest > 0u) {
                memmove(&h->nodes[i], &h->nodes[i + 1u], rest * sizeof(zw_health_node_t));
            }
            h->node_count--;
            memset(&h->nodes[h->node_count], 0, sizeof(zw_health_node_t));
            for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
                if (h->pool[p].used && h->pool[p].node_id == node_id) {
                    memset(&h->pool[p], 0, sizeof(h->pool[p]));
                }
            }
            return true;
        }
    }
    return false;
}

bool zw_health_set_interval(zw_health_t *h, uint16_t node_id, uint32_t interval_s)
{
    zw_health_node_t *node = find_node(h, node_id);
    if (node == NULL) {
        return false;
    }
    node->interval_s = interval_s;
    return true;
}

bool zw_health_is_listening(const zw_health_t *h, uint16_t node_id, bool *out)
{
    const zw_health_node_t *node = find_node_const(h, node_id);
    if (node == NULL) {
        return false;
    }
    *out = node->is_listening;
    return true;
}

uint32_t zw_health_interval(const zw_health_t *h, uint16_t node_id)
{
    const zw_health_node_t *node = find_node_const(h, node_id);
    return node != NULL ? node->interval_s : ZW_HEALTH_INTERVAL_NEVER;
}

bool zw_health_is_reachable(const zw_health_t *h, uint16_t node_id)
{
    const zw_health_node_t *node = find_node_const(h, node_id);
    return node != NULL && node->reachable;
}

/* ---- shared reachability bookkeeping ------------------------------------------------------------- */

static size_t mark_reachable(zw_health_node_t *node, zw_health_action_t *out, size_t max)
{
    if (!node->reachable) {
        node->reachable = true;
        if (max > 0u) {
            out[0].kind = ZW_HEALTH_ACTION_REACHABLE;
            out[0].node_id = node->node_id;
            return 1u;
        }
    }
    return 0u;
}

static size_t mark_unreachable(zw_health_node_t *node, zw_health_action_t *out, size_t max)
{
    if (node->reachable) {
        node->reachable = false;
        if (max > 0u) {
            out[0].kind = ZW_HEALTH_ACTION_UNREACHABLE;
            out[0].node_id = node->node_id;
            return 1u;
        }
    }
    return 0u;
}

/* ---- events in -------------------------------------------------------------------------------- */

size_t zw_health_heard(zw_health_t *h, uint16_t node_id, uint64_t now_ms, zw_health_action_t *out,
                        size_t max)
{
    zw_health_node_t *node = find_node(h, node_id);
    if (node == NULL) {
        return 0u;
    }
    node->last_heard_ms = now_ms;
    node->ping_outstanding = false;
    node->ping_failed_once = false;
    node->first_fail_ms = 0u;
    return mark_reachable(node, out, max);
}

size_t zw_health_acked(zw_health_t *h, uint16_t node_id, uint64_t now_ms, zw_health_action_t *out,
                        size_t max)
{
    const zw_health_node_t *node = find_node_const(h, node_id);
    if (node == NULL || !node->is_listening) {
        return 0u; /* §8: "every ACK" counts for listening devices only */
    }
    return zw_health_heard(h, node_id, now_ms, out, max);
}

static size_t queue_len_locked(const zw_health_t *h, uint16_t node_id);

size_t zw_health_wakeup(zw_health_t *h, uint16_t node_id, uint64_t now_ms, zw_health_action_t *out,
                         size_t max)
{
    zw_health_node_t *node = find_node(h, node_id);
    if (node == NULL) {
        return 0u;
    }
    size_t n = 0u;
    if (node->interval_s != ZW_HEALTH_INTERVAL_NEVER) {
        const uint64_t expected = node->last_heard_ms + (uint64_t)node->interval_s * 1000u;
        if (now_ms > expected + ZW_HEALTH_LATE_WAKEUP_MS && n < max) {
            out[n].kind = ZW_HEALTH_ACTION_REREAD_INTERVAL;
            out[n].node_id = node_id;
            n++;
        }
    }
    node->last_heard_ms = now_ms;
    node->awake = true;
    if (n < max) {
        n += mark_reachable(node, &out[n], max - n);
    }
    if (queue_len_locked(h, node_id) > 0u) {
        if (n < max) {
            out[n].kind = ZW_HEALTH_ACTION_SEND_QUEUE;
            out[n].node_id = node_id;
            n++;
        }
    } else {
        node->no_more_info_pending = true;
        node->no_more_info_due_ms = now_ms + ZW_HEALTH_NO_MORE_INFO_MS;
    }
    return n;
}

void zw_health_set_busy(zw_health_t *h, uint16_t node_id, bool interviewing, bool in_repair)
{
    zw_health_node_t *node = find_node(h, node_id);
    if (node == NULL) {
        return;
    }
    node->interviewing = interviewing;
    node->in_repair = in_repair;
}

void zw_health_restart(zw_health_t *h, uint64_t now_ms)
{
    for (size_t i = 0; i < h->node_count; i++) {
        zw_health_node_t *node = &h->nodes[i];
        node->ping_outstanding = false;
        node->ping_failed_once = false;
        node->first_fail_ms = 0u;
        if (node->is_listening) {
            /* due for a verification ping ZW_HEALTH_RESTART_PING_DELAY_MS from now, not after a
             * full ZW_HEALTH_PING_IDLE_MS of silence: back-date last_heard_ms to get there through
             * the ordinary ping-due arithmetic, rather than a second special-cased timer. */
            const uint64_t back = ZW_HEALTH_PING_IDLE_MS > ZW_HEALTH_RESTART_PING_DELAY_MS
                                       ? ZW_HEALTH_PING_IDLE_MS - ZW_HEALTH_RESTART_PING_DELAY_MS
                                       : 0u;
            node->last_heard_ms = now_ms > back ? now_ms - back : 0u;
        } else {
            node->last_heard_ms = now_ms; /* the full T + 10 min again */
            node->awake = false;
            node->no_more_info_pending = false;
        }
        /* reachability itself is not changed: no new evidence either way, see zw_health.h */
    }
}

/* ---- the wake-up queue ------------------------------------------------------------------------ */

static size_t queue_len_locked(const zw_health_t *h, uint16_t node_id)
{
    size_t n = 0u;
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        if (h->pool[p].used && h->pool[p].node_id == node_id) {
            n++;
        }
    }
    return n;
}

size_t zw_health_queue_len(const zw_health_t *h, uint16_t node_id)
{
    return queue_len_locked(h, node_id);
}

bool zw_health_queue_put(zw_health_t *h, uint16_t node_id, uint32_t kind, bool is_interview_step,
                          const uint8_t *data, size_t data_len)
{
    zw_health_node_t *node = find_node(h, node_id);
    if (node == NULL || kind == ZW_HEALTH_PING_TAG || data_len > ZW_HEALTH_QUEUE_ENTRY_BYTES ||
        (data == NULL && data_len > 0u)) {
        return false;
    }
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        if (h->pool[p].used && h->pool[p].node_id == node_id && h->pool[p].kind == kind) {
            h->pool[p].is_interview_step = is_interview_step;
            memcpy(h->pool[p].data, data, data_len);
            h->pool[p].data_len = (uint8_t)data_len;
            /* the seq (queue position) and in_flight state are kept: this is a replacement, not a
             * new arrival - network.md §7 "a newer command of the same kind replaces the older" */
            return true;
        }
    }
    if (queue_len_locked(h, node_id) >= ZW_HEALTH_QUEUE_PER_NODE_MAX) {
        return false;
    }
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        if (!h->pool[p].used) {
            h->pool[p].used = true;
            h->pool[p].in_flight = false;
            h->pool[p].node_id = node_id;
            h->pool[p].kind = kind;
            h->pool[p].is_interview_step = is_interview_step;
            h->pool[p].seq = h->next_seq++;
            memcpy(h->pool[p].data, data, data_len);
            h->pool[p].data_len = (uint8_t)data_len;
            node->no_more_info_pending = false; /* there is something left again */
            return true;
        }
    }
    return false; /* the shared pool is exhausted */
}

bool zw_health_queue_cancel(zw_health_t *h, uint16_t node_id, uint32_t kind)
{
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        if (h->pool[p].used && h->pool[p].node_id == node_id && h->pool[p].kind == kind) {
            memset(&h->pool[p], 0, sizeof(h->pool[p]));
            return true;
        }
    }
    return false;
}

bool zw_health_queue_peek(const zw_health_t *h, uint16_t node_id, uint32_t kind, uint8_t *out_data,
                          size_t out_cap, size_t *out_len)
{
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        const zw_health_queue_entry_t *e = &h->pool[p];
        if (e->used && e->node_id == node_id && e->kind == kind) {
            const size_t n = e->data_len < out_cap ? e->data_len : out_cap;
            if (n > 0u) {
                memcpy(out_data, e->data, n);
            }
            *out_len = e->data_len;
            return true;
        }
    }
    return false;
}

bool zw_health_queue_take(zw_health_t *h, uint16_t node_id, uint32_t *out_kind, uint8_t *out_data,
                           size_t out_cap, size_t *out_len, bool *out_is_interview_step)
{
    const zw_health_node_t *node = find_node_const(h, node_id);
    if (node == NULL || !node->awake) {
        return false;
    }
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        if (h->pool[p].used && h->pool[p].node_id == node_id && h->pool[p].in_flight) {
            return false; /* §7: one at a time */
        }
    }
    size_t best = ZW_HEALTH_QUEUE_POOL_SIZE;
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        const zw_health_queue_entry_t *e = &h->pool[p];
        if (!e->used || e->node_id != node_id) {
            continue;
        }
        if (best == ZW_HEALTH_QUEUE_POOL_SIZE) {
            best = p;
            continue;
        }
        const zw_health_queue_entry_t *b = &h->pool[best];
        const bool e_first = (e->is_interview_step && !b->is_interview_step) ||
                              (e->is_interview_step == b->is_interview_step && e->seq < b->seq);
        if (e_first) {
            best = p;
        }
    }
    if (best == ZW_HEALTH_QUEUE_POOL_SIZE) {
        return false;
    }
    zw_health_queue_entry_t *e = &h->pool[best];
    e->in_flight = true;
    if (out_kind != NULL) {
        *out_kind = e->kind;
    }
    if (out_len != NULL) {
        *out_len = e->data_len;
    }
    if (out_is_interview_step != NULL) {
        *out_is_interview_step = e->is_interview_step;
    }
    if (out_data != NULL) {
        const size_t n = (size_t)e->data_len < out_cap ? (size_t)e->data_len : out_cap;
        memcpy(out_data, e->data, n);
    }
    return true;
}

/* ---- zw_health_sent(): a ping's or a queue entry's outcome ------------------------------------- */

size_t zw_health_sent(zw_health_t *h, uint16_t node_id, uint32_t tag, bool delivered, uint64_t now_ms,
                       zw_health_action_t *out, size_t max)
{
    zw_health_node_t *node = find_node(h, node_id);
    if (node == NULL) {
        return 0u;
    }

    if (tag == ZW_HEALTH_PING_TAG) {
        node->ping_outstanding = false;
        if (delivered) {
            node->ping_failed_once = false;
            node->first_fail_ms = 0u;
            node->last_heard_ms = now_ms;
            return mark_reachable(node, out, max);
        }
        if (!node->ping_failed_once) {
            node->ping_failed_once = true;
            node->first_fail_ms = now_ms;
            return 0u; /* one failure: wait for the second, §8 */
        }
        node->ping_failed_once = false; /* the cycle starts over; UNREACHABLE only reports once */
        node->first_fail_ms = 0u;
        return mark_unreachable(node, out, max);
    }

    /* a queue entry's outcome */
    for (size_t p = 0; p < ZW_HEALTH_QUEUE_POOL_SIZE; p++) {
        zw_health_queue_entry_t *e = &h->pool[p];
        if (!(e->used && e->in_flight && e->node_id == node_id && e->kind == tag)) {
            continue;
        }
        if (delivered) {
            memset(e, 0, sizeof(*e));
            node->last_heard_ms = now_ms;
            size_t n = mark_reachable(node, out, max);
            if (node->awake && queue_len_locked(h, node_id) == 0u) {
                node->no_more_info_pending = true;
                node->no_more_info_due_ms = now_ms + ZW_HEALTH_NO_MORE_INFO_MS;
            }
            return n;
        }
        if (node->is_listening) {
            memset(e, 0, sizeof(*e)); /* not the queue's job to retry a listening node's send */
        } else {
            e->in_flight = false; /* §7: goes back in the queue, the node counts as asleep */
            node->awake = false;
            node->no_more_info_pending = false;
        }
        return 0u;
    }
    return 0u; /* a mismatched or already-resolved tag: ignored, see zw_health.h */
}

/* ---- polling -------------------------------------------------------------------------------------
 * Mirrors zw_health_next_deadline()'s own conditions exactly - every deadline it reports is one
 * of the checks below, and vice versa. */

static bool listening_ping_due(const zw_health_node_t *node, uint64_t now_ms, uint64_t *due_out)
{
    if (node->ping_outstanding) {
        return false;
    }
    const uint64_t due = node->ping_failed_once ? node->first_fail_ms + ZW_HEALTH_PING_RETRY_MS
                                                 : node->last_heard_ms + ZW_HEALTH_PING_IDLE_MS;
    if (due_out != NULL) {
        *due_out = due;
    }
    return now_ms >= due;
}

static bool sleeping_unreachable_due(const zw_health_node_t *node, uint64_t now_ms, uint64_t *due_out)
{
    if (node->interval_s == ZW_HEALTH_INTERVAL_NEVER || !node->reachable) {
        return false;
    }
    const uint64_t due = node->last_heard_ms + (uint64_t)node->interval_s * 1000u + ZW_HEALTH_SLEEP_GRACE_MS;
    if (due_out != NULL) {
        *due_out = due;
    }
    return now_ms >= due;
}

static bool no_more_info_due(const zw_health_node_t *node, uint64_t now_ms)
{
    return node->no_more_info_pending && !node->interviewing && !node->in_repair &&
           node->interval_s != ZW_HEALTH_INTERVAL_NEVER && now_ms >= node->no_more_info_due_ms;
}

size_t zw_health_poll(zw_health_t *h, uint64_t now_ms, zw_health_action_t *out, size_t max)
{
    size_t n = 0u;
    for (size_t i = 0; i < h->node_count && n < max; i++) {
        zw_health_node_t *node = &h->nodes[i];
        if (node->is_listening) {
            if (listening_ping_due(node, now_ms, NULL)) {
                node->ping_outstanding = true;
                out[n].kind = ZW_HEALTH_ACTION_PING;
                out[n].node_id = node->node_id;
                n++;
            }
            continue;
        }
        if (sleeping_unreachable_due(node, now_ms, NULL) && n < max) {
            node->reachable = false;
            out[n].kind = ZW_HEALTH_ACTION_UNREACHABLE;
            out[n].node_id = node->node_id;
            n++;
        }
        if (no_more_info_due(node, now_ms) && n < max) {
            node->no_more_info_pending = false;
            node->awake = false;
            out[n].kind = ZW_HEALTH_ACTION_NO_MORE_INFO;
            out[n].node_id = node->node_id;
            n++;
        }
    }
    return n;
}

uint64_t zw_health_next_deadline(const zw_health_t *h)
{
    uint64_t soonest = UINT64_MAX;
    for (size_t i = 0; i < h->node_count; i++) {
        const zw_health_node_t *node = &h->nodes[i];
        uint64_t due;
        if (node->is_listening) {
            if (!node->ping_outstanding) {
                (void)listening_ping_due(node, 0u, &due);
                if (due < soonest) {
                    soonest = due;
                }
            }
            continue;
        }
        if (node->interval_s != ZW_HEALTH_INTERVAL_NEVER && node->reachable) {
            (void)sleeping_unreachable_due(node, 0u, &due);
            if (due < soonest) {
                soonest = due;
            }
        }
        if (node->no_more_info_pending && !node->interviewing && !node->in_repair &&
            node->interval_s != ZW_HEALTH_INTERVAL_NEVER) {
            if (node->no_more_info_due_ms < soonest) {
                soonest = node->no_more_info_due_ms;
            }
        }
    }
    return soonest;
}
