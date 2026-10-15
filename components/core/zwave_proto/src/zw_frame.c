/*
 * zw_frame.c - the Serial API frame layer (see zw_frame.h): the checksum, building a frame, and
 * the byte-at-a-time receive parser.
 */
#include "zwave_proto/zw_frame.h"

#include <string.h>

uint8_t zw_frame_checksum(const uint8_t *from_len, size_t n)
{
    uint8_t chk = 0xFFu;
    if (from_len == NULL) {
        return chk;
    }
    for (size_t i = 0; i < n; i++) {
        chk = (uint8_t)(chk ^ from_len[i]);
    }
    return chk;
}

size_t zw_frame_build(uint8_t *out, size_t cap, uint8_t type, uint8_t func,
                       const uint8_t *payload, size_t payload_len)
{
    if (out == NULL || payload_len > ZW_PAYLOAD_MAX || (payload == NULL && payload_len > 0u)) {
        return 0u;
    }
    const size_t total = payload_len + 5u;
    if (cap < total) {
        return 0u;
    }
    out[0] = ZW_SOF;
    out[1] = (uint8_t)(payload_len + 3u);
    out[2] = type;
    out[3] = func;
    for (size_t i = 0; i < payload_len; i++) {
        out[4u + i] = payload[i];
    }
    out[4u + payload_len] = zw_frame_checksum(&out[1], payload_len + 3u);
    return total;
}

void zw_rx_init(zw_rx_t *rx)
{
    memset(rx, 0, sizeof(*rx));
}

/* Back to idle with nothing worth keeping: a dropped partial frame, or one whose LEN was a
 * format error. A completed frame (good or bad checksum) is not reset through here - see the end
 * of zw_rx_feed(), which keeps a good frame's bytes readable until the next zw_rx_feed(). */
static void rx_reset_to_idle(zw_rx_t *rx)
{
    rx->len = 0u;
    rx->want = 0u;
    rx->in_frame = false;
}

zw_rx_event_t zw_rx_feed(zw_rx_t *rx, uint8_t byte, uint64_t now_ms)
{
    rx->have_frame = false;

    if (rx->in_frame) {
        const uint64_t gap = now_ms >= rx->last_byte_ms ? now_ms - rx->last_byte_ms : 0u;
        if (gap > ZW_BYTE_TIMEOUT_MS) {
            rx->timeouts++;
            rx_reset_to_idle(rx);
            /* Falls through: this byte is processed as if the parser had always been idle, so a
             * SOF right after the gap can start a new frame. */
        }
    }

    if (!rx->in_frame) {
        if (byte == ZW_SOF) {
            rx->buf[0] = byte;
            rx->len = 1u;
            rx->want = 0u;
            rx->in_frame = true;
            rx->last_byte_ms = now_ms;
            return ZW_RX_NONE;
        }
        if (byte == ZW_ACK) {
            return ZW_RX_ACK;
        }
        if (byte == ZW_NAK) {
            return ZW_RX_NAK;
        }
        if (byte == ZW_CAN) {
            return ZW_RX_CAN;
        }
        rx->discarded++;
        return ZW_RX_NONE;
    }

    /* Collecting a frame: whatever this byte's value, it is simply the next byte of it - 01,
     * 06, 15 and 18 are ordinary data once a frame is under way. */
    rx->buf[rx->len] = byte;
    rx->len++;
    rx->last_byte_ms = now_ms;

    if (rx->want == 0u) {
        /* This byte is LEN (buf[1]): a frame needs at least 5 bytes in all (serial-api.md §1). */
        if (byte < 3u) {
            rx->bad_format++;
            rx_reset_to_idle(rx);
            return ZW_RX_BAD_FRAME;
        }
        rx->want = (size_t)byte + 2u;
        return ZW_RX_NONE;
    }

    if (rx->len < rx->want) {
        return ZW_RX_NONE;
    }

    /* A complete frame. CHK covers LEN through the last payload byte - 'n' bytes, where n is
     * LEN's own value (see zw_frame_build). */
    const uint8_t want_chk = zw_frame_checksum(&rx->buf[1], (size_t)rx->buf[1]);
    const uint8_t got_chk = rx->buf[rx->len - 1u];
    rx->in_frame = false;
    rx->want = 0u;
    if (want_chk != got_chk) {
        rx->bad_checksum++;
        rx->len = 0u; /* nothing valid to read: forget it, unlike the good frame below */
        return ZW_RX_BAD_FRAME;
    }
    rx->frames++;
    rx->have_frame = true;
    /* 'len' (and 'buf') deliberately survive so zw_rx_frame() can still read them; the next
     * zw_rx_feed() call clears 'have_frame' and, if it is a new SOF, overwrites both anyway. */
    return ZW_RX_FRAME;
}

bool zw_rx_expire(zw_rx_t *rx, uint64_t now_ms)
{
    if (!rx->in_frame) {
        return false;
    }
    const uint64_t gap = now_ms >= rx->last_byte_ms ? now_ms - rx->last_byte_ms : 0u;
    if (gap <= ZW_BYTE_TIMEOUT_MS) {
        return false;
    }
    rx->timeouts++;
    rx_reset_to_idle(rx);
    return true;
}

bool zw_rx_frame(const zw_rx_t *rx, zw_frame_t *out)
{
    if (!rx->have_frame || out == NULL) {
        return false;
    }
    const uint8_t payload_len = (uint8_t)((unsigned)rx->buf[1] - 3u);
    out->type = rx->buf[2];
    out->func = rx->buf[3];
    out->payload_len = payload_len;
    out->payload = payload_len > 0u ? &rx->buf[4] : NULL;
    out->raw = rx->buf;
    out->raw_len = rx->len;
    return true;
}

bool zw_rx_busy(const zw_rx_t *rx)
{
    return rx->in_frame;
}
