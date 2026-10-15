/*
 * zwtrace_replay.h - a recorded ZAC93 on a pretend wire: a zwtrace recording (zwtrace.h) plays the
 * module's side while the stack under test plays the panel's, on a fake clock (docs/features/36
 * README "Test and acceptance"; test/traces/README.md).
 *
 * The recording's entries have three parts to play:
 *   - the panel's writes (`tx`, and a `ctx` that does not answer an `rx`: the start-up's lone NAK):
 *     the stack must make them. When it writes, the write is matched to the next one the
 *     recording has: the same bytes, except a callback ID (the byte zw_api_callback_id_offset()
 *     names; zero where the recording has zero). The first write that is not the one recorded -
 *     or one after the recording's last - is a mismatch: the replay stops there and keeps a message
 *     with both frames in hex (`mismatch`), for the test to fail with. A recorded callback ID is
 *     remembered with the stack's own for it, per function.
 *   - the module's entries (`rx`, `crx`, `text`): delivered in their order at their recorded time
 *     after the panel's write before them (the time that write was matched, plus the gap the
 *     recording shows - so a response keeps its delay after its request, and a frame the module
 *     sent on its own, a SerialAPIStarted after a reset, comes as long after the last write as it
 *     did), and never before every write recorded ahead of them was made. A callback's or status
 *     report's recorded callback ID becomes the stack's (its checksum recomputed).
 *   - the link's answers (`ctx` right after an `rx`): the module's bytes go through the link's own
 *     parser (zw_link.c), which answers a good frame the way the recording did - ACK, handed to the
 *     stack; NAK (the panel had no room), not handed on - and NAKs a damaged one; a frame the byte
 *     timeout cut off is dropped as it was. A recorded ACK for a frame whose checksum is wrong here
 *     is a mismatch (a damaged fixture).
 * The stack's own control bytes (a `ctx` it writes) are checked loosely: a different or missing one
 * is counted in `control_diffs`, and the replay goes on.
 *
 * A `# panel restarted` line ends a segment: the owner restarts the stack and calls
 * zwt_replay_next_segment(); `lost` lines are counted (`lost`), nothing is made up for them.
 *
 * Test support: caller-owned state (static: it is big), no heap, no test framework.
 */
#ifndef ZWTRACE_REPLAY_H
#define ZWTRACE_REPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_frame.h"
#include "zwtrace.h"

#define ZWT_CALLBACK_MAP 64u
#define ZWT_MISMATCH_LEN 1400u

/* The stack as the wire sees it (the fake wire's fake_host_t, again): the link's news, its clock
 * and its deadline. `text` may be NULL. */
typedef struct {
    void *ctx;
    void (*control)(void *ctx, uint64_t now_ms, uint8_t byte);
    void (*frame)(void *ctx, uint64_t now_ms, const zw_frame_t *frame);
    void (*text)(void *ctx, uint64_t now_ms, const uint8_t *bytes, size_t len);
    void (*poll)(void *ctx, uint64_t now_ms);
    uint64_t (*next_deadline)(void *ctx);
} zwt_host_t;

typedef struct {
    const zwt_trace_t *trace;
    zwt_host_t host;
    uint64_t now;
    zw_rx_t rx;              /* the link's parser for the module's bytes */
    /* the segment being played: entries [begin, end) */
    size_t begin, end;
    size_t panel;            /* the next write the stack owes; end: none */
    size_t module;           /* the next module entry to deliver */
    uint64_t start_at;       /* the segment's start on the replay's clock... */
    uint64_t start_t;        /* ...and in the recording */
    uint8_t role[ZWT_ENTRIES_MAX];
    int32_t before[ZWT_ENTRIES_MAX];     /* the panel's write before each entry; -1 none */
    uint64_t matched_at[ZWT_ENTRIES_MAX]; /* when the stack made each write */
    struct {
        uint8_t func, recorded, stack;
    } cb[ZWT_CALLBACK_MAP];
    size_t cb_n, cb_next;
    /* what happened */
    uint32_t matched;        /* writes made as recorded */
    uint32_t delivered;      /* module entries delivered */
    uint32_t remapped;       /* module frames whose callback ID became the stack's */
    uint32_t honored_naks;   /* good frames the recording NAKed: NAKed here too, not handed on */
    uint32_t control_diffs;  /* the stack's own control bytes that differ from the recording */
    uint32_t lost;           /* entries the recording lost, in the segments played */
    uint32_t writes_after;   /* writes after the first mismatch (not matched) */
    uint32_t mismatches;
    char mismatch[ZWT_MISMATCH_LEN]; /* the first one */
    bool stuck;              /* the wire did not settle */
} zwt_replay_t;

/* Ready to play the trace's first segment, the clock at now_ms. */
void zwt_replay_init(zwt_replay_t *r, const zwt_trace_t *t, const zwt_host_t *host, uint64_t now_ms);
/* A recording that begins in the middle of the panel's life (`zwave trace`): skip to its first
 * start-up - the panel's lone NAK of serial-api.md §5 step 1. False when it has none. */
bool zwt_replay_skip_to_startup(zwt_replay_t *r);

/* The stack's write port: what it writes to the module, matched to the recording (never
 * delivers anything itself: that happens in settle, after the stack's call returns). */
bool zwt_replay_write(zwt_replay_t *r, const uint8_t *bytes, size_t len);

/* Everything at the current time: the stack's poll, the module's entries due, until nothing moves. */
void zwt_replay_settle(zwt_replay_t *r);
/* The clock to t_ms, stopping at every deadline of the stack and every entry due on the way. */
void zwt_replay_run_until(zwt_replay_t *r, uint64_t t_ms);

/* The next write the recording expects of the stack, and when (its recorded gap after the write
 * before it); NULL when the segment has none left. */
const zwt_entry_t *zwt_replay_owed(const zwt_replay_t *r, uint64_t *due);
/* The module has something recorded before that write still to deliver, due then: true. What the
 * module said comes first; only then does the owed write tell what the stack is to do. */
bool zwt_replay_module_ahead(const zwt_replay_t *r, uint64_t *due);
/* When the segment's last module entry is due, once every write is made (UINT64_MAX while one is
 * still owed; the clock now when nothing is left). */
uint64_t zwt_replay_last_due(const zwt_replay_t *r);
/* Every write made and every entry delivered, without a mismatch. */
bool zwt_replay_done(const zwt_replay_t *r);
/* The segment ends at `# panel restarted` (more follows it). */
bool zwt_replay_at_restart(const zwt_replay_t *r);
/* On to the segment after the marker, the clock at now_ms (the owner has restarted the stack). */
bool zwt_replay_next_segment(zwt_replay_t *r, uint64_t now_ms);
/* A mismatch the owner found (a write the stack never made): kept like the replay's own. */
void zwt_replay_fail(zwt_replay_t *r, const char *what, const zwt_entry_t *e);

#endif /* ZWTRACE_REPLAY_H */
