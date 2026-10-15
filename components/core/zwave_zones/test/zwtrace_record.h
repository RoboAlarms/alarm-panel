/*
 * zwtrace_record.h - a zwtrace recording made on the PC: what the panel's zw_link task would put
 * in the frame trace (components/services/zwave_service/src/zw_link.c, zw_trace.c) for the bytes
 * crossing a pretend wire, written as "zwtrace 1" text (zwtrace.h). The host tests record the
 * stack against the fake ZAC93 with it, to make fixtures and to prove the replay (record, replay,
 * the same outcome).
 *
 * The panel's writes go in as they are written (zwt_rec_panel: a frame is `tx`, a lone ACK, NAK
 * or CAN `ctx`); the module's bytes go through the link's own parser, as zw_link reads them
 * (zwt_rec_module): `crx` for an ACK, NAK or CAN, `rx` for a frame followed by the link's `ctx`
 * answer (ACK for a good one, NAK for a damaged one, which goes in as it came), `rx` for a frame
 * the byte timeout cut off, and `text` runs for the bytes outside frames.
 *
 * Test support: caller-owned state and output buffer, no heap.
 */
#ifndef ZWTRACE_RECORD_H
#define ZWTRACE_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_frame.h"

typedef struct {
    char *out;
    size_t cap, len;
    bool full;      /* something did not fit: the recording is cut */
    uint32_t seq;   /* the last entry's number (the first is 1) */
    uint32_t entries;
    zw_rx_t rx;     /* the link's parser for the module's bytes */
    uint8_t partial[ZW_RX_BUF_SIZE]; /* the frame being collected, as it came */
    size_t partial_n;
} zwt_rec_t;

/* An empty recording into out (the "zwtrace 1" line written). */
void zwt_rec_init(zwt_rec_t *r, char *out, size_t cap);
/* A `# text` line; `lost <n>`. */
void zwt_rec_comment(zwt_rec_t *r, const char *text);
void zwt_rec_lost(zwt_rec_t *r, uint32_t n);
/* What the panel wrote at `at`. */
void zwt_rec_panel(zwt_rec_t *r, uint64_t at, const uint8_t *bytes, size_t len);
/* Bytes from the module at `at`, as the link reads them. */
void zwt_rec_module(zwt_rec_t *r, uint64_t at, const uint8_t *bytes, size_t len);
/* The link's byte timeout with no byte coming: a partial frame older than 150 ms goes in cut off
 * (call it at the end of a recording, too). */
void zwt_rec_expire(zwt_rec_t *r, uint64_t at);

#endif /* ZWTRACE_RECORD_H */
