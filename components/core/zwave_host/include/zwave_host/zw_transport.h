/*
 * zw_transport.h - the Serial API transport and request scheduler between the panel and the
 * ZAC93 (docs/features/36, serial-api.md §1 "Sending", §2, §6.8, §6.12 and §7; requirement
 * ZW-003).
 *
 * One Serial API command at a time: write its frame, wait for the ACK (retransmitting after
 * NAK, CAN or silence), then for its response and its callbacks; queued requests wait their turn
 * by priority class. It lives in the zwave task. The zw_link task owns the UART: it feeds a
 * zw_rx_t, writes ACK/NAK for the module's frames itself at once, and hands this transport the
 * complete frames, the ACK/NAK/CAN bytes it hears and the bytes the parser discarded while idle.
 * The transport writes frames through its port. It never blocks and never reads a clock: time
 * comes in as milliseconds, and zw_transport_next_deadline() says when to call
 * zw_transport_poll() next, so the task can sleep until then or until the link has news.
 *
 * Only zw_transport_poll() writes. Every call first expires what is due at `now_ms`, so the
 * outcome does not depend on how late the task gets round to polling: an ACK arriving after
 * its 1600 ms is late whether or not poll ran in between.
 *
 * A gate (zw_transport_set_gate) holds back the classes that need a working link while the
 * module is being started or recovered; zw_transport_fail_queued() ends the requests that
 * cannot wait for it (link down).
 *
 * Portable C11: caller-owned state, no heap, no globals.
 */
#ifndef ZW_TRANSPORT_H
#define ZW_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ZW_TRANSPORT_MUST_CHECK __attribute__((warn_unused_result))
#else
#define ZW_TRANSPORT_MUST_CHECK
#endif

/* Requests waiting to be sent (the one in flight is not counted). Changes the layout of
 * zw_transport_t: define it the same for every file that includes this header. */
#ifndef ZW_TRANSPORT_QUEUE_LEN
#define ZW_TRANSPORT_QUEUE_LEN 16u
#endif

/* Priority classes, most urgent first; first come, first served within a class. */
typedef enum {
    ZW_PRIO_REPLY = 0,  /* nonce and Supervision replies */
    ZW_PRIO_MODULE,     /* commands to the module itself */
    ZW_PRIO_NODE,       /* commands to nodes */
    ZW_PRIO_INTERVIEW,
    ZW_PRIO_BACKGROUND,
    ZW_PRIO_COUNT
} zw_prio_t;

/* A set of classes, for zw_transport_fail_queued(). */
#define ZW_PRIO_BIT(p) (1u << (unsigned)(p))
#define ZW_PRIO_ALL    ((1u << (unsigned)ZW_PRIO_COUNT) - 1u)

/* What a command waits for after its ACK (zw_request_t.expect); 0 = the ACK finishes it. */
#define ZW_EXPECT_RESPONSE 0x01u /* a response: TYPE 01 with the same FUNC */
#define ZW_EXPECT_CALLBACK 0x02u /* a callback: TYPE 00, same FUNC, payload[0] = its callback ID */

/* A callback's place in the command, as the request's classifier sees it. */
typedef enum {
    ZW_CALLBACK_FINAL = 0,   /* the command is done */
    ZW_CALLBACK_INTERMEDIATE /* a report that does not end it (RequestNodeNeighborUpdate's
                              * "started"): keep waiting. AddNode and RemoveNode end at their
                              * first report (Ready); the ones after it are the module's own
                              * frames (serial-api.md §1). */
} zw_callback_kind_t;

/* How a command ended. */
typedef enum {
    ZW_TX_OK = 0,
    ZW_TX_NO_ACK,           /* 3 transmissions without an ACK (NAK, CAN or 1600 ms each) */
    ZW_TX_NOT_ACCEPTED,     /* the response said no (the request's check; SendData: empty) */
    ZW_TX_NOT_QUEUED,       /* SendData answered "not queued" 3 times */
    ZW_TX_RESPONSE_TIMEOUT, /* no response within its timeout (10 s unless the request says);
                             * SendData with a callback: no response, and then a callback that
                             * was not OK (`callback` holds it) or none 30 s after the ACK */
    ZW_TX_CALLBACK_TIMEOUT, /* no (further) callback in time; SendData: none 30 s after "queued" */
    ZW_TX_NO_RESTART,       /* SoftReset: no SerialAPIStarted within 5 s of its ACK */
    ZW_TX_MODULE_RESTARTED, /* an unexpected SerialAPIStarted ended it */
    ZW_TX_LINK_DOWN         /* never sent: failed while queued because the link went down
                             * (zw_transport_fail_queued); try again when it is up */
} zw_tx_outcome_t;

/* What zw_request_t.done hears. The payload views are valid only during that call. */
typedef struct {
    zw_tx_outcome_t status;
    uint8_t func;
    uint8_t callback_id;     /* the one allocated for it; 0 when it had none */
    uint8_t transmissions;   /* its frame written, all attempts and resends counted */
    const uint8_t *response; /* the response's payload; NULL when none arrived */
    size_t response_len;
    const uint8_t *callback; /* the final callback's payload (callback ID first); a SoftReset's
                              * SerialAPIStarted payload; NULL when none */
    size_t callback_len;
} zw_tx_result_t;

/*
 * A request: copied by zw_transport_submit(), so the caller's buffers may go at once.
 *
 * The callbacks (response_ok, callback_kind, done) run inside the transport's calls. They may
 * submit and cancel requests - done() may queue the next step of a sequence - but must not feed
 * link events or poll (those calls are refused while an owner callback runs, and counted in
 * stats.reentries).
 *
 * A lost ACK (serial-api.md §1): when the command's response - or, for a command that has only a
 * callback, its callback - arrives while the transport still waits for the ACK, the module took
 * the frame; the answer counts as the ACK too, and the command is never sent again.
 *
 * Built-in rules by FUNC (serial-api.md §6.8, §6.12), which override the fields named:
 * - SendData (13): "not queued" (response 00) is sent again 500 ms later, 3 transmissions at
 *   most, then NOT_QUEUED; with a callback, SendDataAbort (16) goes out 20 s after "queued" and
 *   the command fails 30 s after it (response_ok, callback_timeout_ms unused). When "queued"
 *   never comes after the ACK, the callback is still awaited on the same clocks, counted from
 *   the ACK: a callback with transmit status OK ends it OK; one that is not OK, or none (the
 *   module not answering), ends it RESPONSE_TIMEOUT (serial-api.md §7).
 * - SoftReset (08): finished by the SerialAPIStarted that follows its ACK, 5 s at most; a lone
 *   byte whose low nibble is 6 (discarded on its own, not inside text) counts as its ACK once
 *   (some firmware damages it, serial-api.md §6.8). expect, the timeouts and the classifier are
 *   unused.
 */
typedef struct {
    uint8_t func;                 /* the Serial API function */
    const uint8_t *payload;       /* NULL when payload_len is 0 */
    size_t payload_len;           /* at most ZW_PAYLOAD_MAX */
    uint8_t expect;               /* 0, or ZW_EXPECT_RESPONSE and/or ZW_EXPECT_CALLBACK */
    uint8_t callback_id_at;       /* with a callback: the payload byte the transport overwrites
                                   * with the callback ID it allocates (1..255, never 0) */
    zw_prio_t prio;
    uint32_t response_timeout_ms; /* 0: 10 s */
    uint32_t callback_timeout_ms; /* with a callback: the longest wait for it, started again by
                                   * each intermediate one; must not be 0 (SendData: unused) */
    /* NULL: any response is accepted; false: the command fails with NOT_ACCEPTED. */
    bool (*response_ok)(void *ctx, const uint8_t *payload, size_t len);
    /* NULL: the first callback is the final one. It sees every callback, so it may also note
     * what an intermediate one says. */
    zw_callback_kind_t (*callback_kind)(void *ctx, const uint8_t *payload, size_t len);
    /* Called once when the command ends, however it ends; NULL: nobody asks. Not called for a
     * request cancelled before it was sent. */
    void (*done)(void *ctx, uint32_t tag, const zw_tx_result_t *result);
    void *ctx;                    /* the owner's context, handed to the three above */
    uint32_t tag;                 /* the owner's label, for zw_transport_cancel() */
} zw_request_t;

typedef enum {
    ZW_SUBMIT_OK = 0,
    ZW_SUBMIT_FULL,     /* ZW_TRANSPORT_QUEUE_LEN requests already wait */
    ZW_SUBMIT_INVALID,  /* a field out of range: payload length or pointer, expect,
                         * callback_id_at, a callback without callback_timeout_ms, prio */
    ZW_SUBMIT_LINK_DOWN /* (zw_host_submit) the link is down: try again when it is up */
} zw_submit_t;

/* What the transport tells its owner besides the results of commands. */
typedef enum {
    /* §7: no ACK after 3 transmissions, a command ACKed but its response missing (10 s), the
     * SendData callback missing 30 s after "queued", or SendData "not queued" 3 times. Told
     * after the failed command's done(). */
    ZW_TRANSPORT_NOT_ANSWERING = 1,
    /* §2: a SerialAPIStarted the host did not ask for (frame = it). The command in flight, if
     * any, has already failed with MODULE_RESTARTED. */
    ZW_TRANSPORT_MODULE_RESTARTED
} zw_transport_event_t;

/* The transport's view of the world. None may call back into the transport. */
typedef struct {
    void *ctx;
    /* Writes bytes to the UART, through the zw_link task; false when they could not leave (the
     * frame then counts as a failed transmission, as a NAK). */
    bool (*write)(void *ctx, const uint8_t *bytes, size_t len);
    /* A module frame no command was waiting for (zw_link has ACKed it): unsolicited requests,
     * and stray responses or callbacks. Optional. */
    void (*frame)(void *ctx, const zw_frame_t *frame);
    /* Bytes the parser discarded while idle, as they came (bootloader text). Optional. */
    void (*text)(void *ctx, const uint8_t *bytes, size_t len);
    /* See zw_transport_event_t; frame is NULL for NOT_ANSWERING. Optional. */
    void (*event)(void *ctx, zw_transport_event_t ev, const zw_frame_t *frame);
} zw_transport_port_t;

/* Counters for a console status line and for tests. The transport only adds to them. */
typedef struct {
    uint32_t submitted, refused, cancelled;
    uint32_t commands;      /* commands started */
    uint32_t completed;     /* ended with ZW_TX_OK */
    uint32_t failed;        /* ended otherwise */
    uint32_t transmissions; /* command frames written, retransmissions included */
    uint32_t acks, naks, cans, ack_timeouts, write_failures;
    uint32_t lost_acks;     /* answers that came while the ACK was awaited (it was lost) */
    uint32_t dropped;       /* queued requests failed by zw_transport_fail_queued() */
    uint32_t not_queued;    /* SendData "not queued" answers */
    uint32_t aborts;        /* SendDataAbort frames written */
    uint32_t intermediate;  /* intermediate callbacks */
    uint32_t unclaimed;     /* frames handed to port.frame */
    uint32_t stray;         /* ACK, NAK or CAN that nothing waited for */
    uint32_t restarts;      /* unexpected SerialAPIStarted */
    uint32_t reentries;     /* link calls refused because an owner callback was running */
} zw_transport_stats_t;

/* ---- State (caller-owned; read it through the functions below) --------------------------- */

typedef enum {
    ZW_TX_PHASE_IDLE = 0,
    ZW_TX_PHASE_SEND,          /* to be written at `due` (after a failed transmission or a
                                * "not queued": the wait before the next one) */
    ZW_TX_PHASE_WAIT_ACK,
    ZW_TX_PHASE_WAIT_RESPONSE,
    ZW_TX_PHASE_WAIT_CALLBACK,
    ZW_TX_PHASE_WAIT_RESTART   /* SoftReset: acknowledged, waiting for SerialAPIStarted */
} zw_tx_phase_t;

typedef struct {
    uint8_t func, expect, callback_id_at, prio;
    uint32_t response_timeout_ms, callback_timeout_ms;
    bool (*response_ok)(void *ctx, const uint8_t *payload, size_t len);
    zw_callback_kind_t (*callback_kind)(void *ctx, const uint8_t *payload, size_t len);
    void (*done)(void *ctx, uint32_t tag, const zw_tx_result_t *result);
    void *ctx;
    uint32_t tag;
    size_t payload_len;
    uint8_t payload[ZW_PAYLOAD_MAX];
} zw_tx_cmd_t;

typedef struct {
    zw_tx_cmd_t cmd;
    uint64_t seq;  /* submission order (64 bits: it never wraps) */
    bool used;
    bool failing;  /* picked by zw_transport_fail_queued(), its done() still to be called */
} zw_tx_slot_t;

typedef struct {
    zw_tx_cmd_t cmd;
    zw_tx_phase_t phase;
    uint64_t due;          /* SEND: when to write; WAIT_*: when waiting ends in failure */
    uint64_t abort_at;     /* SendData: when to write SendDataAbort; 0 when written or none */
    uint8_t callback_id;
    uint8_t attempts;      /* failed transmissions since the last (re)send */
    uint8_t not_queued;    /* SendData "not queued" answers so far */
    uint8_t transmissions;
    bool damaged_ack_used; /* SoftReset: a lone byte with low nibble 6 already taken as its ACK */
    bool response_lost;    /* SendData: ACKed, its "queued" response missing; its callback still
                            * awaited (only a successful one ends it OK) */
    bool has_response;
    size_t response_len;
    uint8_t response[ZW_PAYLOAD_MAX];
} zw_tx_active_t;

typedef struct {
    zw_transport_port_t port;
    zw_tx_slot_t queue[ZW_TRANSPORT_QUEUE_LEN];
    uint64_t next_seq;
    zw_tx_active_t active;
    uint8_t next_callback_id;   /* 1..255 */
    uint64_t abort_ack_until;   /* a SendDataAbort's ACK is awaited until then (0: none); the
                                 * next command waits for it, so it cannot take that ACK */
    zw_prio_t gate;             /* the least a request may need to start (zw_transport_set_gate) */
    unsigned depth;             /* > 0 while an owner callback runs */
    uint32_t events;            /* port.event calls so far: a poll that raised one starts nothing */
    zw_transport_stats_t stats;
} zw_transport_t;

/* ---- Functions ---------------------------------------------------------------------------- */

/* Empty queue, nothing in flight, callback IDs from 1, the gate open. port->write is
 * required. */
void zw_transport_init(zw_transport_t *t, const zw_transport_port_t *port);

/* Queues a copy of *req. Refuses with FULL or INVALID (and counts it); never sends from here -
 * the next zw_transport_poll() does. */
ZW_TRANSPORT_MUST_CHECK
zw_submit_t zw_transport_submit(zw_transport_t *t, const zw_request_t *req);

/* Drops every queued request with this tag, without calling their done(); the command in
 * flight goes on. Returns how many were dropped. */
size_t zw_transport_cancel(zw_transport_t *t, uint32_t tag);

/* The dispatch gate: which queued requests may start. The gate ranks the classes by what they
 * need, not by urgency - a module command needs only the module, everything else needs the
 * link up - in the order MODULE, REPLY, NODE, INTERVIEW, BACKGROUND, and lets a request start
 * when its class is no further down that order than max_prio. So ZW_PRIO_MODULE lets only
 * module commands out (start-up and recovery: even nonce replies wait, they are SendData) and
 * ZW_PRIO_BACKGROUND everything (open, the default). The command in flight is not affected;
 * held requests keep their place and go, most urgent first, once the gate lets them. */
void zw_transport_set_gate(zw_transport_t *t, zw_prio_t max_prio);

/* Ends every queued request whose class is in `classes` (a set of ZW_PRIO_BIT()) with `status`
 * - ZW_TX_LINK_DOWN when the link went down - calling their done() in the order they would have
 * been sent. The command in flight goes on. done() may submit again; what it submits is not
 * ended here. Returns how many were ended. */
size_t zw_transport_fail_queued(zw_transport_t *t, unsigned classes, zw_tx_outcome_t status);

/* The module cannot answer the command in flight - the host found it in its bootloader, where
 * every byte is a menu key: the command ends now with `status` (its done() called), and nothing
 * more of it is written - no retransmission, no SendDataAbort. Queued requests stay queued (the
 * owner closes the gate first). Refused, like the link's news, while an owner callback runs. */
void zw_transport_halt(zw_transport_t *t, zw_tx_outcome_t status);

/* The link's news, in the order zw_link heard it: an ACK, NAK or CAN byte (any other value is
 * ignored)... */
void zw_transport_on_control(zw_transport_t *t, uint64_t now_ms, uint8_t byte);
/* ...a complete, checksum-good frame that zw_link has already ACKed (the view is only read
 * during the call)... */
void zw_transport_on_frame(zw_transport_t *t, uint64_t now_ms, const zw_frame_t *frame);
/* ...and bytes the parser discarded while idle. */
void zw_transport_on_text(zw_transport_t *t, uint64_t now_ms, const uint8_t *bytes, size_t len);

/* Timeouts, retransmissions, SendDataAbort, and the next queued command when the line is free:
 * the only call that writes. Call it after the link's news and at zw_transport_next_deadline().
 * A call that told the owner of an event (port.event) starts no new command: the owner may
 * close the gate first; its next poll, due at once, goes on. */
void zw_transport_poll(zw_transport_t *t, uint64_t now_ms);

/* When zw_transport_poll() has work next: 0 = at once (a queued command the gate lets out can
 * go), UINT64_MAX = not before the link, a new request or the gate brings news. */
uint64_t zw_transport_next_deadline(const zw_transport_t *t);

/* A command in flight. */
bool zw_transport_busy(const zw_transport_t *t);

/* Requests waiting to be sent, held by the gate or not. */
size_t zw_transport_queued(const zw_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* ZW_TRANSPORT_H */
