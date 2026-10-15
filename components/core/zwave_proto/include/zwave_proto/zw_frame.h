/*
 * zw_frame.h - the Z-Wave Serial API's frame layer between the panel and the ZAC93
 * (docs/features/36, serial-api.md §1 "The wire"): the checksum, building a frame, and a
 * streaming byte-at-a-time receive parser with the byte timeout and the counters a console
 * status line wants.
 *
 * Only framing. Sending one command at a time, waiting for its ACK/response/callback and
 * retrying it is components/core/zwave_host (Z2) - this component never re-enters itself and
 * never decides to retransmit anything.
 *
 * Portable C11, no heap, no bus.
 */
#ifndef ZW_FRAME_H
#define ZW_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The four control bytes and the two frame TYPEs (serial-api.md §1). */
#define ZW_SOF 0x01u
#define ZW_ACK 0x06u
#define ZW_NAK 0x15u
#define ZW_CAN 0x18u
#define ZW_TYPE_REQUEST  0x00u
#define ZW_TYPE_RESPONSE 0x01u

/* LEN is one byte, so a data frame is at most LEN+2 = 257 bytes (SOF, LEN, TYPE, FUNC, CHK, plus
 * up to LEN-3 payload bytes): LEN = payload length + 3, so a payload is at most 252 bytes. */
#define ZW_FRAME_MAX   257u
#define ZW_PAYLOAD_MAX 252u
/* Room zw_rx_t's buffer needs for the largest possible frame, with a little margin. */
#define ZW_RX_BUF_SIZE 260u
/* A partial frame with no new byte for longer than this is abandoned (serial-api.md §1
 * "Receiving"); a gap of exactly this many ms still counts as on time. */
#define ZW_BYTE_TIMEOUT_MS 150u

/* 0xFF XOR every byte from LEN through the last payload byte (SOF excluded). 'from_len' points at
 * LEN; 'n' is how many bytes to fold in - LEN, TYPE, FUNC and the payload, i.e. payload_len + 3,
 * which is LEN's own value. */
uint8_t zw_frame_checksum(const uint8_t *from_len, size_t n);

/* Writes SOF LEN TYPE FUNC PAYLOAD CHK into 'out' and returns the frame's total length
 * (payload_len + 5). Returns 0 and writes nothing when payload_len > ZW_PAYLOAD_MAX, 'cap' is too
 * small for the frame, 'out' is NULL, or 'payload' is NULL while payload_len > 0. */
size_t zw_frame_build(uint8_t *out, size_t cap, uint8_t type, uint8_t func,
                       const uint8_t *payload, size_t payload_len);

/* A parsed frame: pointers into the zw_rx_t that reported it, valid until that parser's next
 * zw_rx_feed() call. */
typedef struct {
    uint8_t type;           /* ZW_TYPE_REQUEST or ZW_TYPE_RESPONSE, not validated here */
    uint8_t func;            /* the Serial API function (serial-api.md §6), not validated here */
    const uint8_t *payload;  /* payload_len bytes; NULL when payload_len is 0 */
    size_t payload_len;
    const uint8_t *raw;      /* the whole frame, SOF through CHK, raw_len bytes */
    size_t raw_len;
} zw_frame_t;

/* What zw_rx_feed() found for the byte just given it. */
typedef enum {
    ZW_RX_NONE = 0,  /* consumed; nothing for the caller to do yet */
    ZW_RX_ACK,       /* 06 arrived while idle */
    ZW_RX_NAK,       /* 15 arrived while idle */
    ZW_RX_CAN,       /* 18 arrived while idle */
    ZW_RX_FRAME,     /* a complete frame, checksum good: ACK it now, then read it with zw_rx_frame */
    ZW_RX_BAD_FRAME, /* bad checksum, or LEN < 3: NAK it now */
} zw_rx_event_t;

/* Byte-at-a-time receive parser (serial-api.md §1 "Receiving"). Caller-owned, no heap: call
 * zw_rx_init() before use. TYPE and FUNC are not checked here - a higher layer ACKs every good
 * frame, then drops the ones it has no use for. */
typedef struct {
    uint8_t buf[ZW_RX_BUF_SIZE]; /* the frame being collected (or just completed), from SOF */
    size_t len;                  /* bytes of 'buf' filled so far */
    size_t want;                 /* total bytes 'buf' needs once LEN is known; 0 until then */
    bool in_frame;                /* SOF seen, collecting; false = idle, hunting for 01/06/15/18 */
    bool have_frame;              /* buf[0..len) is the checksum-good frame zw_rx_frame() reads */
    uint64_t last_byte_ms;        /* time of the last byte of the frame being collected */
    /* A console status line's counters. The parser only ever adds to them. */
    uint32_t frames;       /* complete frames with a good checksum */
    uint32_t discarded;    /* bytes ignored while idle (anything but 01/06/15/18) */
    uint32_t timeouts;     /* partial frames dropped by the byte timeout */
    uint32_t bad_checksum; /* complete frames whose CHK did not match */
    uint32_t bad_format;   /* frames whose LEN was below 3 */
} zw_rx_t;

/* Zeroes the counters and puts the parser at idle. */
void zw_rx_init(zw_rx_t *rx);

/* Feeds one received byte, timestamped 'now_ms' (a free-running millisecond clock). See
 * zw_rx_event_t for what each return value means. */
zw_rx_event_t zw_rx_feed(zw_rx_t *rx, uint8_t byte, uint64_t now_ms);

/* For the UART task's read timeout, so a partial frame does not wait forever when no further
 * byte arrives at all: drops a partial frame whose last byte is older than ZW_BYTE_TIMEOUT_MS
 * (counted in 'timeouts'). Returns true when it dropped one. Harmless to call while idle, or when
 * 'now_ms' has gone backwards (treated as no gap). */
bool zw_rx_expire(zw_rx_t *rx, uint64_t now_ms);

/* The frame zw_rx_feed() just reported with ZW_RX_FRAME. Returns false (leaves '*out' untouched)
 * when the last event was not ZW_RX_FRAME; the view is valid only until the next zw_rx_feed(). */
bool zw_rx_frame(const zw_rx_t *rx, zw_frame_t *out);

/* True while a partial frame is being collected (a SOF has arrived, the frame has not). */
bool zw_rx_busy(const zw_rx_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* ZW_FRAME_H */
