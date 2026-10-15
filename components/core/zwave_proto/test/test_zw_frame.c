/*
 * test_zw_frame.c - the Serial API frame layer, CRC-16 and DSK/PIN text (components/core/
 * zwave_proto, docs/features/36 serial-api.md §1, security.md §4/§11), proved without a ZAC93.
 *
 * Requirements: ZW-003, ZW-SEC-012, ZW-SEC-013
 */
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_proto/zw_dsk.h"
#include "zwave_proto/zw_frame.h"

void setUp(void) {}
void tearDown(void) {}

/* A small deterministic PRNG (xorshift32, like rf_decode's tests): the same numbers on every
 * compiler and platform, so a failure can always be replayed from the fixed seed below. */
static uint32_t SEED = 0xC0FFEE01u;

static uint32_t rnd(void)
{
    uint32_t x = SEED;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    SEED = x;
    return x;
}

static uint8_t rnd_byte(void)
{
    return (uint8_t)(rnd() & 0xFFu);
}

/* Feeds every byte of 'bytes' at the same timestamp (no gaps) and returns the last event. */
static zw_rx_event_t feed_all(zw_rx_t *rx, const uint8_t *bytes, size_t n, uint64_t t)
{
    zw_rx_event_t ev = ZW_RX_NONE;
    for (size_t i = 0; i < n; i++) {
        ev = zw_rx_feed(rx, bytes[i], t);
    }
    return ev;
}

/* ---- zw_frame_build / zw_frame_checksum ------------------------------------------------------- */

/* ZW-003: serial-api.md §1's own worked example, GetControllerVersion, built byte-exact. */
static void test_the_worked_example_builds_byte_exact(void)
{
    uint8_t out[ZW_FRAME_MAX];
    const size_t n = zw_frame_build(out, sizeof(out), ZW_TYPE_REQUEST, 0x15u, NULL, 0u);
    const uint8_t expect[] = {0x01u, 0x03u, 0x00u, 0x15u, 0xE9u};
    TEST_ASSERT_EQUAL_UINT(sizeof(expect), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, out, sizeof(expect));
}

static void test_build_refusals(void)
{
    uint8_t out[ZW_FRAME_MAX];
    uint8_t payload[ZW_PAYLOAD_MAX + 1u] = {0};

    TEST_ASSERT_EQUAL_UINT(0u, zw_frame_build(out, sizeof(out), 0u, 0u, payload, ZW_PAYLOAD_MAX + 1u));
    TEST_ASSERT_EQUAL_UINT(0u, zw_frame_build(out, 4u, ZW_TYPE_REQUEST, 0x15u, NULL, 0u)); /* cap 4 < 5 */
    TEST_ASSERT_EQUAL_UINT(0u, zw_frame_build(out, sizeof(out), 0u, 0u, NULL, 5u)); /* NULL payload, len>0 */
    TEST_ASSERT_EQUAL_UINT(0u, zw_frame_build(NULL, sizeof(out), 0u, 0u, NULL, 0u));
    /* payload_len 0 with a NULL payload is fine (an empty payload has nothing to point at). */
    TEST_ASSERT_EQUAL_UINT(5u, zw_frame_build(out, sizeof(out), 0u, 0u, NULL, 0u));
}

/* The largest possible frame: 252-byte payload, 257 bytes total, round-tripped through the
 * parser. */
static void test_the_maximum_frame_round_trips(void)
{
    uint8_t payload[ZW_PAYLOAD_MAX];
    for (size_t i = 0; i < ZW_PAYLOAD_MAX; i++) {
        payload[i] = (uint8_t)(i * 3u + 7u);
    }
    uint8_t out[ZW_FRAME_MAX];
    const size_t n = zw_frame_build(out, sizeof(out), ZW_TYPE_RESPONSE, 0x42u, payload, ZW_PAYLOAD_MAX);
    TEST_ASSERT_EQUAL_UINT(ZW_FRAME_MAX, n);
    TEST_ASSERT_EQUAL_UINT(0u, zw_frame_build(out, ZW_FRAME_MAX - 1u, ZW_TYPE_RESPONSE, 0x42u,
                                               payload, ZW_PAYLOAD_MAX)); /* cap one short */

    zw_rx_t rx;
    zw_rx_init(&rx);
    TEST_ASSERT_EQUAL(ZW_RX_FRAME, feed_all(&rx, out, n, 1000u));
    zw_frame_t f;
    TEST_ASSERT_TRUE(zw_rx_frame(&rx, &f));
    TEST_ASSERT_EQUAL_UINT8(ZW_TYPE_RESPONSE, f.type);
    TEST_ASSERT_EQUAL_UINT8(0x42u, f.func);
    TEST_ASSERT_EQUAL_UINT(ZW_PAYLOAD_MAX, f.payload_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, f.payload, ZW_PAYLOAD_MAX);
    TEST_ASSERT_EQUAL_UINT(ZW_FRAME_MAX, f.raw_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(out, f.raw, ZW_FRAME_MAX);
}

/* ---- the receive parser: control bytes, garbage, checksums, LEN ------------------------------- */

static void test_ack_nak_can_while_idle(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    TEST_ASSERT_EQUAL(ZW_RX_ACK, zw_rx_feed(&rx, ZW_ACK, 0u));
    TEST_ASSERT_EQUAL(ZW_RX_NAK, zw_rx_feed(&rx, ZW_NAK, 1u));
    TEST_ASSERT_EQUAL(ZW_RX_CAN, zw_rx_feed(&rx, ZW_CAN, 2u));
    TEST_ASSERT_FALSE(zw_rx_busy(&rx));
    TEST_ASSERT_EQUAL_UINT32(0u, rx.discarded);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.frames);
}

/* Bytes 06/15/18 (and even another 01) inside a frame are data, not control bytes: every byte up
 * to the last one returns NONE, only the final CHK completes the frame. */
static void test_control_bytes_inside_a_frame_are_data(void)
{
    const uint8_t payload[] = {ZW_ACK, ZW_NAK, ZW_CAN, ZW_SOF, 0xAAu};
    uint8_t out[ZW_FRAME_MAX];
    const size_t n = zw_frame_build(out, sizeof(out), ZW_TYPE_REQUEST, 0x77u, payload, sizeof(payload));
    TEST_ASSERT_TRUE(n > 0u);

    zw_rx_t rx;
    zw_rx_init(&rx);
    for (size_t i = 0; i + 1u < n; i++) {
        TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, out[i], 0u));
    }
    TEST_ASSERT_EQUAL(ZW_RX_FRAME, zw_rx_feed(&rx, out[n - 1u], 0u));
    zw_frame_t f;
    TEST_ASSERT_TRUE(zw_rx_frame(&rx, &f));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, f.payload, sizeof(payload));
}

static void test_garbage_before_sof_is_discarded_and_counted(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    const uint8_t garbage[] = {0x02u, 0x99u, 0x50u, 0xFFu};
    for (size_t i = 0; i < sizeof(garbage); i++) {
        TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, garbage[i], 0u));
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(garbage), rx.discarded);

    const uint8_t expect[] = {0x01u, 0x03u, 0x00u, 0x15u, 0xE9u};
    TEST_ASSERT_EQUAL(ZW_RX_FRAME, feed_all(&rx, expect, sizeof(expect), 0u));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(garbage), rx.discarded); /* unchanged by the frame */
    TEST_ASSERT_EQUAL_UINT32(1u, rx.frames);
}

static void test_bad_checksum_is_nak_and_counted(void)
{
    uint8_t bad[] = {0x01u, 0x03u, 0x00u, 0x15u, 0xE8u}; /* CHK off by one */
    zw_rx_t rx;
    zw_rx_init(&rx);
    TEST_ASSERT_EQUAL(ZW_RX_BAD_FRAME, feed_all(&rx, bad, sizeof(bad), 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, rx.bad_checksum);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.bad_format);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.frames);
    zw_frame_t f;
    TEST_ASSERT_FALSE(zw_rx_frame(&rx, &f)); /* nothing to read after a bad frame */
    TEST_ASSERT_FALSE(zw_rx_busy(&rx));
}

/* LEN 0, 1 and 2 are format errors, reported the instant LEN arrives (not once more bytes turn
 * up); the parser is back at idle right after, so the next good frame still parses. */
static void test_short_len_is_bad_frame_at_once_then_recovers(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    const uint8_t good[] = {0x01u, 0x03u, 0x00u, 0x15u, 0xE9u};

    for (uint8_t len = 0u; len <= 2u; len++) {
        TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, ZW_SOF, 0u));
        TEST_ASSERT_TRUE(zw_rx_busy(&rx));
        TEST_ASSERT_EQUAL(ZW_RX_BAD_FRAME, zw_rx_feed(&rx, len, 0u)); /* at once: no more bytes fed */
        TEST_ASSERT_FALSE(zw_rx_busy(&rx));
        TEST_ASSERT_EQUAL(ZW_RX_FRAME, feed_all(&rx, good, sizeof(good), 0u));
    }
    TEST_ASSERT_EQUAL_UINT32(3u, rx.bad_format);
    TEST_ASSERT_EQUAL_UINT32(3u, rx.frames);
}

/* ---- byte timeout and zw_rx_expire -------------------------------------------------------------- */

/* A 150 ms gap between bytes of a partial frame is still on time; 151 ms drops it, and a SOF
 * arriving right after the drop starts a fresh frame that parses normally. */
static void test_byte_timeout_150_keeps_151_drops(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);

    /* Exactly 150 ms between every byte: never times out. */
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x01u, 0u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x03u, 150u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x00u, 300u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x15u, 450u));
    TEST_ASSERT_EQUAL(ZW_RX_FRAME, zw_rx_feed(&rx, 0xE9u, 600u));
    TEST_ASSERT_EQUAL_UINT32(0u, rx.timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, rx.frames);

    /* A 151 ms gap after the SOF drops the partial frame; the byte that arrives late is then
     * processed as if the parser had been idle all along. */
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x01u, 1000u));
    TEST_ASSERT_TRUE(zw_rx_busy(&rx));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x01u, 1151u)); /* dropped, then starts anew */
    TEST_ASSERT_EQUAL_UINT32(1u, rx.timeouts);
    TEST_ASSERT_TRUE(zw_rx_busy(&rx));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x03u, 1151u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x00u, 1151u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x15u, 1151u));
    TEST_ASSERT_EQUAL(ZW_RX_FRAME, zw_rx_feed(&rx, 0xE9u, 1151u));
    TEST_ASSERT_EQUAL_UINT32(1u, rx.timeouts);
    TEST_ASSERT_EQUAL_UINT32(2u, rx.frames);
}

static void test_rx_expire_boundary(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    TEST_ASSERT_FALSE(zw_rx_expire(&rx, 999999u)); /* idle: nothing to expire */

    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, ZW_SOF, 0u));
    TEST_ASSERT_FALSE(zw_rx_expire(&rx, 150u)); /* exactly on time */
    TEST_ASSERT_TRUE(zw_rx_busy(&rx));
    TEST_ASSERT_EQUAL_UINT32(0u, rx.timeouts);
    TEST_ASSERT_TRUE(zw_rx_expire(&rx, 151u)); /* one ms late */
    TEST_ASSERT_FALSE(zw_rx_busy(&rx));
    TEST_ASSERT_EQUAL_UINT32(1u, rx.timeouts);
}

static void test_now_going_backwards_is_harmless(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x01u, 100000u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x03u, 50u));    /* clock stepped backwards */
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x00u, 0u));
    TEST_ASSERT_EQUAL(ZW_RX_NONE, zw_rx_feed(&rx, 0x15u, 0u));
    TEST_ASSERT_EQUAL(ZW_RX_FRAME, zw_rx_feed(&rx, 0xE9u, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, rx.timeouts);
}

/* ---- back-to-back frames and the view's fields -------------------------------------------------- */

static void test_two_frames_back_to_back(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    const uint8_t a[] = {0x01u, 0x03u, 0x00u, 0x15u, 0xE9u};
    const uint8_t payload_b[] = {0x11u, 0x22u, 0x33u};
    uint8_t b[ZW_FRAME_MAX];
    const size_t blen = zw_frame_build(b, sizeof(b), ZW_TYPE_RESPONSE, 0x04u, payload_b, sizeof(payload_b));

    TEST_ASSERT_EQUAL(ZW_RX_FRAME, feed_all(&rx, a, sizeof(a), 0u));
    zw_frame_t fa;
    TEST_ASSERT_TRUE(zw_rx_frame(&rx, &fa));
    TEST_ASSERT_EQUAL_UINT8(0x00u, fa.type);
    TEST_ASSERT_EQUAL_UINT8(0x15u, fa.func);
    TEST_ASSERT_EQUAL_UINT(0u, fa.payload_len);
    TEST_ASSERT_NULL(fa.payload);

    TEST_ASSERT_EQUAL(ZW_RX_FRAME, feed_all(&rx, b, blen, 0u));
    zw_frame_t fb;
    TEST_ASSERT_TRUE(zw_rx_frame(&rx, &fb));
    TEST_ASSERT_EQUAL_UINT8(ZW_TYPE_RESPONSE, fb.type);
    TEST_ASSERT_EQUAL_UINT8(0x04u, fb.func);
    TEST_ASSERT_EQUAL_UINT(sizeof(payload_b), fb.payload_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload_b, fb.payload, sizeof(payload_b));
    TEST_ASSERT_EQUAL_UINT(blen, fb.raw_len);
    TEST_ASSERT_EQUAL_UINT32(2u, rx.frames);
}

static void test_rx_busy(void)
{
    zw_rx_t rx;
    zw_rx_init(&rx);
    TEST_ASSERT_FALSE(zw_rx_busy(&rx));
    zw_rx_feed(&rx, ZW_SOF, 0u);
    TEST_ASSERT_TRUE(zw_rx_busy(&rx));
    zw_rx_feed(&rx, 0x03u, 0u);
    zw_rx_feed(&rx, 0x00u, 0u);
    zw_rx_feed(&rx, 0x15u, 0u);
    TEST_ASSERT_TRUE(zw_rx_busy(&rx));
    zw_rx_feed(&rx, 0xE9u, 0u); /* completes */
    TEST_ASSERT_FALSE(zw_rx_busy(&rx));
}

/* ---- CRC-16 -------------------------------------------------------------------------------------- */

/* ZW-SEC-013: security.md §11 "CRC-16 encapsulation". */
static void test_crc16_vector(void)
{
    const uint8_t data[] = {0x56u, 0x01u, 0x20u, 0x02u};
    TEST_ASSERT_EQUAL_HEX16(0x4D26u, zw_crc16(ZW_CRC16_INIT, data, sizeof(data)));
}

/* The CRC catalogue's check value for CRC-16/AUG-CCITT (alias SPI-FUJITSU: poly 0x1021, init
 * 0x1D0F, no reflection, no final XOR - the same parameters security.md §11 uses) over ASCII
 * "123456789"; kept because it agrees with the vector above. */
static void test_crc16_catalogue_check_value(void)
{
    const uint8_t data[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0xE5CCu, zw_crc16(ZW_CRC16_INIT, data, 9u));
}

static void test_crc16_chains_across_calls(void)
{
    const uint8_t part1[] = {0x56u, 0x01u};
    const uint8_t part2[] = {0x20u, 0x02u};
    const uint16_t chained = zw_crc16(zw_crc16(ZW_CRC16_INIT, part1, sizeof(part1)), part2, sizeof(part2));
    TEST_ASSERT_EQUAL_HEX16(0x4D26u, chained);
}

/* ---- DSK and PIN text ------------------------------------------------------------------------- */

/* ZW-SEC-012: security.md §11's temporary-key vector, "device DSK". */
static void test_dsk_vector(void)
{
    const uint8_t dsk[16] = {0xdeu, 0x9eu, 0xdbu, 0x7du, 0x7bu, 0x7du, 0xc1u, 0xb4u,
                              0xd3u, 0x5bu, 0x61u, 0xc2u, 0xecu, 0xe4u, 0x35u, 0x37u};
    char text[ZW_DSK_TEXT_SIZE];
    TEST_ASSERT_TRUE(zw_dsk_to_text(dsk, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("56990-56189-31613-49588-54107-25026-60644-13623", text);
    TEST_ASSERT_EQUAL_UINT16(56990u, zw_dsk_pin(dsk));

    uint8_t back[16];
    TEST_ASSERT_TRUE(zw_dsk_from_text(text, back));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(dsk, back, 16u);

    uint16_t pin = 0u;
    TEST_ASSERT_TRUE(zw_pin_from_text("56990", &pin));
    TEST_ASSERT_EQUAL_UINT16(56990u, pin);
}

static void test_dsk_to_text_refusals(void)
{
    const uint8_t dsk[16] = {0};
    char text[ZW_DSK_TEXT_SIZE];
    TEST_ASSERT_FALSE(zw_dsk_to_text(NULL, text, sizeof(text)));
    TEST_ASSERT_FALSE(zw_dsk_to_text(dsk, NULL, sizeof(text)));
    TEST_ASSERT_FALSE(zw_dsk_to_text(dsk, text, ZW_DSK_TEXT_SIZE - 1u));
    TEST_ASSERT_TRUE(zw_dsk_to_text(dsk, text, ZW_DSK_TEXT_SIZE));
}

static void test_dsk_round_trips_random(void)
{
    for (int t = 0; t < 200; t++) {
        uint8_t dsk[16];
        for (size_t i = 0; i < 16u; i++) {
            dsk[i] = rnd_byte();
        }
        char text[ZW_DSK_TEXT_SIZE];
        TEST_ASSERT_TRUE(zw_dsk_to_text(dsk, text, sizeof(text)));
        TEST_ASSERT_EQUAL_UINT(47u, strlen(text));
        uint8_t back[16];
        TEST_ASSERT_TRUE(zw_dsk_from_text(text, back));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(dsk, back, 16u);
        TEST_ASSERT_EQUAL_UINT16(((uint16_t)dsk[0] << 8) | dsk[1], zw_dsk_pin(dsk));
    }
}

/* Strict rejection: wrong group count, wrong digit count, an out-of-range group, non-digits,
 * whitespace, a trailing separator, empty text and NULL all leave 'dsk' untouched and fail. */
static void test_dsk_from_text_rejects(void)
{
    uint8_t dsk[16];
    memset(dsk, 0xAAu, sizeof(dsk));

    static const char *const bad[] = {
        "56990-56189-31613-49588-54107-25026-60644",           /* 7 groups */
        "56990-56189-31613-49588-54107-25026-60644-13623-1",   /* 9 groups */
        "5699-56189-31613-49588-54107-25026-60644-13623",      /* 4-digit group */
        "569900-56189-31613-49588-54107-25026-60644-13623",    /* 6-digit group */
        "65536-56189-31613-49588-54107-25026-60644-13623",     /* group > 65535 */
        "5699A-56189-31613-49588-54107-25026-60644-13623",     /* a letter */
        "56990-56189-31613-49588-54107-25026-60644-13623 ",    /* trailing space */
        "56990-56189-31613-49588-54107-25026-60644-13623-",    /* trailing '-' */
        "",                                                    /* empty */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t out[16];
        memset(out, 0x55u, sizeof(out));
        TEST_ASSERT_FALSE(zw_dsk_from_text(bad[i], out));
        for (size_t j = 0; j < 16u; j++) { /* unchanged on failure */
            TEST_ASSERT_EQUAL_HEX8(0x55u, out[j]);
        }
    }
    TEST_ASSERT_FALSE(zw_dsk_from_text(NULL, dsk));
}

static void test_pin_from_text(void)
{
    uint16_t pin = 0xFFFFu;
    TEST_ASSERT_TRUE(zw_pin_from_text("00000", &pin));
    TEST_ASSERT_EQUAL_UINT16(0u, pin);
    TEST_ASSERT_TRUE(zw_pin_from_text("65535", &pin));
    TEST_ASSERT_EQUAL_UINT16(65535u, pin);

    TEST_ASSERT_FALSE(zw_pin_from_text("1234", &pin));   /* 4 digits */
    TEST_ASSERT_FALSE(zw_pin_from_text("123456", &pin)); /* 6 digits */
    TEST_ASSERT_FALSE(zw_pin_from_text("65536", &pin));  /* out of range */
    TEST_ASSERT_FALSE(zw_pin_from_text("1234A", &pin));  /* a letter */
    TEST_ASSERT_FALSE(zw_pin_from_text("", &pin));
    TEST_ASSERT_FALSE(zw_pin_from_text("12345", NULL));
    TEST_ASSERT_FALSE(zw_pin_from_text(NULL, &pin));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_worked_example_builds_byte_exact);
    RUN_TEST(test_build_refusals);
    RUN_TEST(test_the_maximum_frame_round_trips);
    RUN_TEST(test_ack_nak_can_while_idle);
    RUN_TEST(test_control_bytes_inside_a_frame_are_data);
    RUN_TEST(test_garbage_before_sof_is_discarded_and_counted);
    RUN_TEST(test_bad_checksum_is_nak_and_counted);
    RUN_TEST(test_short_len_is_bad_frame_at_once_then_recovers);
    RUN_TEST(test_byte_timeout_150_keeps_151_drops);
    RUN_TEST(test_rx_expire_boundary);
    RUN_TEST(test_now_going_backwards_is_harmless);
    RUN_TEST(test_two_frames_back_to_back);
    RUN_TEST(test_rx_busy);
    RUN_TEST(test_crc16_vector);
    RUN_TEST(test_crc16_catalogue_check_value);
    RUN_TEST(test_crc16_chains_across_calls);
    RUN_TEST(test_dsk_vector);
    RUN_TEST(test_dsk_to_text_refusals);
    RUN_TEST(test_dsk_round_trips_random);
    RUN_TEST(test_dsk_from_text_rejects);
    RUN_TEST(test_pin_from_text);
    return UNITY_END();
}
