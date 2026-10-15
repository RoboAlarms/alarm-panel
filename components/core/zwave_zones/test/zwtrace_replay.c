/*
 * zwtrace_replay.c - a recorded ZAC93 on a pretend wire (see zwtrace_replay.h).
 */
#include "zwtrace_replay.h"

#include <stdio.h>
#include <string.h>

#include "zwave_proto/zw_api.h"

enum { ROLE_NONE = 0, ROLE_PANEL, ROLE_ANSWER, ROLE_MODULE, ROLE_LOST, ROLE_END };

#define TEXT_RUN 64u /* zw_link.c: the longest text run it hands on */

static bool is_control(uint8_t b)
{
    return b == ZW_ACK || b == ZW_NAK || b == ZW_CAN;
}

static const zwt_entry_t *at(const zwt_replay_t *r, size_t i)
{
    return &r->trace->e[i];
}

/* ---- the mismatch message ---------------------------------------------------------------------- */

static void msg(zwt_replay_t *r, const char *s)
{
    const size_t have = strlen(r->mismatch);
    const size_t n = strlen(s);
    const size_t room = sizeof r->mismatch - 1u - have;
    const size_t take = n < room ? n : room;
    memcpy(&r->mismatch[have], s, take);
    r->mismatch[have + take] = '\0';
}

static void msg_num(zwt_replay_t *r, uint64_t v)
{
    char digits[24];
    size_t n = 0u;
    do {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v > 0u && n < sizeof digits - 1u);
    char out[24];
    for (size_t i = 0; i < n; i++) {
        out[i] = digits[n - 1u - i];
    }
    out[n] = '\0';
    msg(r, out);
}

static void msg_hex(zwt_replay_t *r, const uint8_t *b, size_t n)
{
    char hex[2u * ZWT_ENTRY_MAX + 4u];
    (void)zwt_hex(b, n, hex, sizeof hex);
    msg(r, hex);
}

/* "tx #12 (line 40, 520 ms)" */
static void msg_entry(zwt_replay_t *r, const zwt_entry_t *e)
{
    static const char *const KINDS[] = {"rx", "tx", "crx", "ctx", "text", "lost", "restart"};
    msg(r, e->kind < sizeof KINDS / sizeof KINDS[0] ? KINDS[e->kind] : "?");
    msg(r, " #");
    msg_num(r, e->seq);
    msg(r, " (line ");
    msg_num(r, e->line);
    msg(r, ", ");
    msg_num(r, e->t);
    msg(r, " ms)");
}

/* Only the first mismatch is described; the replay stops at it. */
static bool first_mismatch(zwt_replay_t *r)
{
    r->mismatches++;
    if (r->mismatches > 1u) {
        return false;
    }
    r->mismatch[0] = '\0';
    return true;
}

static void mismatch_write(zwt_replay_t *r, const zwt_entry_t *e, const uint8_t *b, size_t n)
{
    if (!first_mismatch(r)) {
        return;
    }
    if (e == NULL) {
        msg(r, "after the recording's last write, the stack wrote\n  ");
        msg_hex(r, b, n);
        return;
    }
    msg_entry(r, e);
    msg(r, ": the stack wrote\n  ");
    msg_hex(r, b, n);
    msg(r, "\nwhere the recording has\n  ");
    msg_hex(r, zwt_bytes(r->trace, e), e->len);
}

void zwt_replay_fail(zwt_replay_t *r, const char *what, const zwt_entry_t *e)
{
    if (!first_mismatch(r)) {
        return;
    }
    msg(r, what);
    if (e != NULL) {
        msg(r, ": ");
        msg_entry(r, e);
        msg(r, "\n  ");
        msg_hex(r, zwt_bytes(r->trace, e), e->len);
    }
}

/* ---- the segment ------------------------------------------------------------------------------- */

static size_t next_panel(const zwt_replay_t *r, size_t from)
{
    while (from < r->end && r->role[from] != ROLE_PANEL) {
        from++;
    }
    return from;
}

static void classify(zwt_replay_t *r)
{
    const zwt_trace_t *t = r->trace;
    for (size_t i = 0; i < t->n; i++) {
        switch ((zwt_kind_t)t->e[i].kind) {
        case ZWT_RX:
        case ZWT_CRX:
        case ZWT_TEXT: r->role[i] = ROLE_MODULE; break;
        case ZWT_TX: r->role[i] = ROLE_PANEL; break;
        case ZWT_CTX:
            /* the link's answer to the frame just before it (zw_link.c answer()), or the stack's
             * own control byte */
            r->role[i] = i > 0u && t->e[i - 1u].kind == ZWT_RX ? ROLE_ANSWER : ROLE_PANEL;
            break;
        case ZWT_LOST: r->role[i] = ROLE_LOST; break;
        case ZWT_RESTART: r->role[i] = ROLE_END; break;
        default: r->role[i] = ROLE_NONE; break;
        }
    }
}

static void segment(zwt_replay_t *r, size_t begin, uint64_t now_ms)
{
    const zwt_trace_t *t = r->trace;
    r->begin = begin;
    r->end = begin;
    while (r->end < t->n && r->role[r->end] != ROLE_END) {
        r->end++;
    }
    r->start_at = now_ms;
    r->start_t = 0u;
    bool timed = false;
    int32_t last = -1;
    for (size_t i = begin; i < r->end; i++) {
        r->before[i] = last;
        if (r->role[i] == ROLE_PANEL) {
            last = (int32_t)i;
        }
        if (r->role[i] == ROLE_LOST) {
            r->lost += t->e[i].lost;
        } else if (!timed) {
            r->start_t = t->e[i].t;
            timed = true;
        }
    }
    r->panel = next_panel(r, begin);
    r->module = begin;
    r->cb_n = 0u; /* a restarted panel numbers its callbacks from 1 again */
    r->cb_next = 0u;
    zw_rx_init(&r->rx);
}

/* When entry i is due on the replay's clock: its recorded gap after the panel's write before it
 * (or after the segment's start). */
static uint64_t due_of(const zwt_replay_t *r, size_t i)
{
    const uint64_t t = at(r, i)->t;
    const int32_t k = r->before[i];
    if (k < 0) {
        return r->start_at + (t >= r->start_t ? t - r->start_t : 0u);
    }
    const uint64_t tk = at(r, (size_t)k)->t;
    return r->matched_at[k] + (t >= tk ? t - tk : 0u);
}

void zwt_replay_init(zwt_replay_t *r, const zwt_trace_t *t, const zwt_host_t *host, uint64_t now_ms)
{
    memset(r, 0, sizeof *r);
    r->trace = t;
    r->host = *host;
    r->now = now_ms;
    classify(r);
    segment(r, 0u, now_ms);
}

bool zwt_replay_skip_to_startup(zwt_replay_t *r)
{
    for (size_t i = r->begin; i < r->end; i++) {
        if (r->role[i] == ROLE_PANEL && at(r, i)->kind == ZWT_CTX &&
            zwt_bytes(r->trace, at(r, i))[0] == ZW_NAK) {
            r->lost = 0u;
            segment(r, i, r->now);
            return true;
        }
    }
    return false;
}

/* ---- the stack's writes ------------------------------------------------------------------------ */

static void learn(zwt_replay_t *r, uint8_t func, uint8_t recorded, uint8_t stack)
{
    if (recorded == 0u) {
        return;
    }
    for (size_t i = 0; i < r->cb_n; i++) {
        if (r->cb[i].func == func && r->cb[i].recorded == recorded) {
            r->cb[i].stack = stack;
            return;
        }
    }
    size_t slot = r->cb_n;
    if (r->cb_n < ZWT_CALLBACK_MAP) {
        r->cb_n++;
    } else {
        slot = r->cb_next;
        r->cb_next = (r->cb_next + 1u) % ZWT_CALLBACK_MAP;
    }
    r->cb[slot].func = func;
    r->cb[slot].recorded = recorded;
    r->cb[slot].stack = stack;
}

/* The stack's write is the recorded one: the same bytes but for the callback ID (both zero or
 * both not). *cb says where that byte is, and what it was on each side. */
static bool same_write(const zwt_replay_t *r, const zwt_entry_t *e, const uint8_t *w, size_t n,
                       bool *has_cb, uint8_t *rec_cb, uint8_t *stack_cb)
{
    const uint8_t *rec = zwt_bytes(r->trace, e);
    *has_cb = false;
    if (e->len != n) {
        return false;
    }
    if (!zwt_frame_ok(rec, n) || !zwt_frame_ok(w, n)) {
        return memcmp(rec, w, n) == 0; /* raw bytes (the bootloader's key): all of them */
    }
    if (rec[2] != w[2] || rec[3] != w[3]) {
        return false;
    }
    const size_t pn = n - 5u;
    size_t off = SIZE_MAX;
    if (w[2] != ZW_TYPE_REQUEST || !zw_api_callback_id_offset(w[3], &w[4], pn, &off)) {
        off = SIZE_MAX;
    }
    for (size_t i = 0; i < pn; i++) {
        if (i != off && rec[4u + i] != w[4u + i]) {
            return false;
        }
    }
    if (off != SIZE_MAX) {
        if ((rec[4u + off] == 0u) != (w[4u + off] == 0u)) {
            return false;
        }
        *has_cb = true;
        *rec_cb = rec[4u + off];
        *stack_cb = w[4u + off];
    }
    return true;
}

static void made(zwt_replay_t *r, size_t k)
{
    r->matched_at[k] = r->now;
    r->panel = next_panel(r, k + 1u);
}

bool zwt_replay_write(zwt_replay_t *r, const uint8_t *bytes, size_t len)
{
    if (bytes == NULL || len == 0u) {
        return true;
    }
    if (r->mismatches > 0u) {
        r->writes_after++;
        return true;
    }
    if (len == 1u && is_control(bytes[0])) {
        /* the stack's own control byte (the start-up's NAK): checked loosely */
        if (r->panel < r->end && at(r, r->panel)->kind == ZWT_CTX) {
            if (zwt_bytes(r->trace, at(r, r->panel))[0] != bytes[0]) {
                r->control_diffs++;
            } else {
                r->matched++;
            }
            made(r, r->panel);
        } else {
            r->control_diffs++; /* the recording has none here */
        }
        return true;
    }
    while (r->panel < r->end && at(r, r->panel)->kind == ZWT_CTX) {
        r->control_diffs++; /* recorded, but the stack wrote its next frame instead */
        made(r, r->panel);
    }
    if (r->panel >= r->end) {
        mismatch_write(r, NULL, bytes, len);
        return true;
    }
    const zwt_entry_t *e = at(r, r->panel);
    bool has_cb = false;
    uint8_t rec_cb = 0u;
    uint8_t stack_cb = 0u;
    if (!same_write(r, e, bytes, len, &has_cb, &rec_cb, &stack_cb)) {
        mismatch_write(r, e, bytes, len);
        return true;
    }
    if (has_cb) {
        learn(r, bytes[3], rec_cb, stack_cb);
    }
    r->matched++;
    made(r, r->panel);
    return true;
}

/* ---- the module's entries ---------------------------------------------------------------------- */

/* A recorded callback or status report: its callback ID becomes the one the stack used. */
static void remap(zwt_replay_t *r, uint8_t *b, size_t n)
{
    zw_api_func_info_t info;
    if (!zwt_frame_ok(b, n) || n < 6u || b[2] != ZW_TYPE_REQUEST || !zw_api_func_info(b[3], &info) ||
        info.callback == ZW_CB_NONE) {
        return;
    }
    for (size_t i = 0; i < r->cb_n; i++) {
        if (r->cb[i].func == b[3] && r->cb[i].recorded == b[4]) {
            if (r->cb[i].stack != b[4]) {
                b[4] = r->cb[i].stack;
                b[n - 1u] = zw_frame_checksum(&b[1], b[1]);
                r->remapped++;
            }
            return;
        }
    }
}

static void host_text(zwt_replay_t *r, const uint8_t *b, size_t n)
{
    if (n > 0u && r->host.text != NULL) {
        r->host.text(r->host.ctx, r->now, b, n);
    }
}

/* The module's bytes through the link (zw_link.c feed()): ACK, NAK and CAN, frames - a good one
 * answered as the recording answered it - and text runs, handed to the stack in their order. */
static void link_feed(zwt_replay_t *r, const zwt_entry_t *e, const uint8_t *b, size_t n,
                      uint8_t answer, bool recorded)
{
    uint8_t text[TEXT_RUN];
    size_t tn = 0u;
    for (size_t i = 0; i < n && r->mismatches == 0u; i++) {
        const uint32_t discarded = r->rx.discarded;
        const zw_rx_event_t ev = zw_rx_feed(&r->rx, b[i], r->now);
        if (r->rx.discarded != discarded) {
            text[tn++] = b[i];
            if (tn == sizeof text) {
                host_text(r, text, tn);
                tn = 0u;
            }
            continue;
        }
        host_text(r, text, tn);
        tn = 0u;
        switch (ev) {
        case ZW_RX_ACK:
        case ZW_RX_NAK:
        case ZW_RX_CAN: r->host.control(r->host.ctx, r->now, b[i]); break;
        case ZW_RX_FRAME: {
            zw_frame_t f;
            if (!zw_rx_frame(&r->rx, &f)) {
                break;
            }
            if (answer == ZW_ACK) {
                r->host.frame(r->host.ctx, r->now, &f);
            } else {
                r->honored_naks++; /* the panel NAKed it (no room): the module sends it again */
            }
            break;
        }
        case ZW_RX_BAD_FRAME:
            if (recorded && answer == ZW_ACK && first_mismatch(r)) {
                msg_entry(r, e);
                msg(r, ": the recording ACKed this frame, but its checksum is wrong here\n  ");
                msg_hex(r, zwt_bytes(r->trace, e), e->len);
            }
            break;
        default: break;
        }
    }
    host_text(r, text, tn);
}

static void deliver(zwt_replay_t *r, size_t j)
{
    const zwt_entry_t *e = at(r, j);
    uint8_t buf[ZWT_ENTRY_MAX];
    memcpy(buf, zwt_bytes(r->trace, e), e->len);
    uint8_t answer = ZW_ACK;
    bool recorded = false;
    if (e->kind == ZWT_RX) {
        remap(r, buf, e->len);
        if (j + 1u < r->end && r->role[j + 1u] == ROLE_ANSWER) {
            answer = zwt_bytes(r->trace, at(r, j + 1u))[0];
            recorded = true;
        }
    }
    link_feed(r, e, buf, e->len, answer, recorded);
    if (e->kind == ZWT_RX && zw_rx_busy(&r->rx)) {
        /* cut off: the link dropped it when the byte timeout ran out */
        (void)zw_rx_expire(&r->rx, r->now + ZW_BYTE_TIMEOUT_MS + 1u);
    }
    r->delivered++;
}

/* The next module entry, or end. */
static size_t next_module(const zwt_replay_t *r)
{
    size_t j = r->module;
    while (j < r->end && r->role[j] != ROLE_MODULE) {
        j++;
    }
    return j;
}

/* The next module entry may go: every write recorded before it has been made. */
static bool deliverable(const zwt_replay_t *r, size_t j)
{
    return j < r->end && r->panel > j && r->mismatches == 0u;
}

static bool deliver_due(zwt_replay_t *r)
{
    bool moved = false;
    for (;;) {
        const size_t j = next_module(r);
        r->module = j;
        if (!deliverable(r, j) || due_of(r, j) > r->now) {
            return moved;
        }
        deliver(r, j);
        r->module = j + 1u;
        moved = true;
    }
}

static uint64_t replay_deadline(const zwt_replay_t *r)
{
    const size_t j = next_module(r);
    return deliverable(r, j) ? due_of(r, j) : UINT64_MAX;
}

/* ---- the clock --------------------------------------------------------------------------------- */

void zwt_replay_settle(zwt_replay_t *r)
{
    for (unsigned guard = 0; guard < 100000u; guard++) {
        r->host.poll(r->host.ctx, r->now);
        if (!deliver_due(r)) {
            return;
        }
    }
    r->stuck = true;
}

static uint64_t wire_deadline(const zwt_replay_t *r)
{
    const uint64_t h = r->host.next_deadline(r->host.ctx);
    const uint64_t m = replay_deadline(r);
    return h < m ? h : m;
}

void zwt_replay_run_until(zwt_replay_t *r, uint64_t t_ms)
{
    zwt_replay_settle(r);
    for (unsigned guard = 0; guard < 1000000u && !r->stuck; guard++) {
        const uint64_t d = wire_deadline(r);
        if (d > t_ms) {
            break;
        }
        if (d <= r->now) {
            /* due now, yet the last settle left it: once more, then call it stuck */
            zwt_replay_settle(r);
            if (wire_deadline(r) <= r->now) {
                r->stuck = true;
            }
            continue;
        }
        r->now = d;
        zwt_replay_settle(r);
    }
    if (!r->stuck && t_ms > r->now) {
        r->now = t_ms;
        zwt_replay_settle(r);
    }
}

/* ---- where it stands --------------------------------------------------------------------------- */

const zwt_entry_t *zwt_replay_owed(const zwt_replay_t *r, uint64_t *due)
{
    if (r->panel >= r->end) {
        return NULL;
    }
    if (due != NULL) {
        *due = due_of(r, r->panel);
    }
    return at(r, r->panel);
}

bool zwt_replay_module_ahead(const zwt_replay_t *r, uint64_t *due)
{
    const size_t j = next_module(r);
    if (!deliverable(r, j)) { /* deliverable: before the owed write */
        return false;
    }
    if (due != NULL) {
        *due = due_of(r, j);
    }
    return true;
}

uint64_t zwt_replay_last_due(const zwt_replay_t *r)
{
    if (r->panel < r->end) {
        return UINT64_MAX;
    }
    uint64_t last = r->now;
    for (size_t j = r->module; j < r->end; j++) {
        if (r->role[j] == ROLE_MODULE) {
            const uint64_t d = due_of(r, j);
            last = d > last ? d : last;
        }
    }
    return last;
}

bool zwt_replay_done(const zwt_replay_t *r)
{
    return r->mismatches == 0u && r->panel >= r->end && next_module(r) >= r->end;
}

bool zwt_replay_at_restart(const zwt_replay_t *r)
{
    return r->end < r->trace->n && r->role[r->end] == ROLE_END;
}

bool zwt_replay_next_segment(zwt_replay_t *r, uint64_t now_ms)
{
    if (!zwt_replay_at_restart(r)) {
        return false;
    }
    r->now = now_ms;
    segment(r, r->end + 1u, now_ms);
    return true;
}
