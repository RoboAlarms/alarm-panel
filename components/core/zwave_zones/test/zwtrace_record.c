/*
 * zwtrace_record.c - a zwtrace recording made on the PC (see zwtrace_record.h). The module's side
 * follows zw_link.c's feed() step by step, so a recording of the fake reads as one of the panel.
 */
#include "zwtrace_record.h"

#include <stdio.h>
#include <string.h>

#include "zwtrace.h"

#define TEXT_RUN 64u /* zw_link.c: the longest text run it hands on */

static void put(zwt_rec_t *r, const char *s, size_t n)
{
    if (r->full || n + 1u > r->cap - r->len) {
        r->full = true;
        return;
    }
    memcpy(&r->out[r->len], s, n);
    r->len += n;
    r->out[r->len] = '\0';
}

static void entry(zwt_rec_t *r, uint64_t at, const char *dir, const uint8_t *bytes, size_t len)
{
    char head[64];
    char hex[2u * ZWT_ENTRY_MAX + 1u];
    if (len == 0u) {
        return;
    }
    r->seq++;
    r->entries++;
    const int h = snprintf(head, sizeof head, "%lu %llu %s ", (unsigned long)r->seq,
                           (unsigned long long)at, dir);
    if (h <= 0 || (size_t)h >= sizeof head || len > ZWT_ENTRY_MAX) {
        r->full = true;
        return;
    }
    const size_t n = zwt_hex(bytes, len, hex, sizeof hex);
    put(r, head, (size_t)h);
    put(r, hex, n);
    put(r, "\n", 1u);
}

static bool is_control(uint8_t b)
{
    return b == ZW_ACK || b == ZW_NAK || b == ZW_CAN;
}

/* zw_link.c's partial frame: a byte of the frame being collected (`starts`: its SOF). */
static void partial_add(zwt_rec_t *r, bool starts, uint8_t b)
{
    if (starts) {
        r->partial_n = 0u;
    }
    if (r->partial_n < sizeof r->partial) {
        r->partial[r->partial_n++] = b;
    }
}

/* What was collected, cut off by the byte timeout, into the recording. */
static void partial_out(zwt_rec_t *r, uint64_t at)
{
    if (r->partial_n > 0u) {
        entry(r, at, "rx", r->partial, r->partial_n);
    }
    r->partial_n = 0u;
}

void zwt_rec_init(zwt_rec_t *r, char *out, size_t cap)
{
    memset(r, 0, sizeof *r);
    r->out = out;
    r->cap = cap;
    if (cap > 0u) {
        out[0] = '\0';
    }
    zw_rx_init(&r->rx);
    put(r, "zwtrace 1\n", 10u);
}

void zwt_rec_comment(zwt_rec_t *r, const char *text)
{
    put(r, "# ", 2u);
    put(r, text, strlen(text));
    put(r, "\n", 1u);
}

void zwt_rec_lost(zwt_rec_t *r, uint32_t n)
{
    char line[32];
    const int w = snprintf(line, sizeof line, "lost %lu\n", (unsigned long)n);
    if (w > 0 && (size_t)w < sizeof line) {
        put(r, line, (size_t)w);
    } else {
        r->full = true;
    }
}

void zwt_rec_panel(zwt_rec_t *r, uint64_t at, const uint8_t *bytes, size_t len)
{
    /* zw_link.c drain_tx(): a frame, or a lone control byte (the start-up's NAK) */
    entry(r, at, len == 1u && is_control(bytes[0]) ? "ctx" : "tx", bytes, len);
}

void zwt_rec_module(zwt_rec_t *r, uint64_t at, const uint8_t *bytes, size_t len)
{
    uint8_t text[TEXT_RUN];
    size_t tn = 0u;
    for (size_t i = 0; i < len; i++) {
        const uint32_t discarded = r->rx.discarded;
        const uint32_t timeouts = r->rx.timeouts;
        const bool was_busy = zw_rx_busy(&r->rx);
        const zw_rx_event_t ev = zw_rx_feed(&r->rx, bytes[i], at);
        const bool cut = r->rx.timeouts != timeouts;
        if (cut) {
            partial_out(r, at); /* the frame before this byte waited too long for it */
        }
        if (r->rx.discarded != discarded) {
            text[tn++] = bytes[i];
            if (tn == sizeof text) {
                entry(r, at, "text", text, tn);
                tn = 0u;
            }
            continue;
        }
        entry(r, at, "text", text, tn);
        tn = 0u;
        switch (ev) {
        case ZW_RX_ACK:
        case ZW_RX_NAK:
        case ZW_RX_CAN: entry(r, at, "crx", &bytes[i], 1u); break;
        case ZW_RX_FRAME: {
            zw_frame_t f;
            if (zw_rx_frame(&r->rx, &f)) {
                static const uint8_t ack = ZW_ACK;
                r->partial_n = 0u;
                entry(r, at, "rx", f.raw, f.raw_len);
                entry(r, at, "ctx", &ack, 1u); /* the link's ACK, at once */
            }
            break;
        }
        case ZW_RX_BAD_FRAME: {
            static const uint8_t nak = ZW_NAK;
            partial_add(r, false, bytes[i]);
            entry(r, at, "rx", r->partial, r->partial_n); /* the damaged frame as it came */
            entry(r, at, "ctx", &nak, 1u);
            r->partial_n = 0u;
            break;
        }
        default: partial_add(r, !was_busy || cut, bytes[i]); break;
        }
    }
    entry(r, at, "text", text, tn);
}

void zwt_rec_expire(zwt_rec_t *r, uint64_t at)
{
    if (zw_rx_expire(&r->rx, at)) {
        partial_out(r, at);
    }
}
