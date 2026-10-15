/*
 * test_rf_decode.c - the pulse decoders (components/core/rf_decode, docs/features/25).
 *
 * Every frame here is made by the component's own encoders and then sent through a small model
 * of what happens between a sensor and the panel (docs/features/25 "What goes wrong on the air"):
 * the receiver's slicer moves edges (marks stretched, spaces shrunk), its AGC fills silence with
 * static and garbles the first frame of a burst, noise cuts glitches into pulses, the capture
 * loses pulses when its task falls behind, two sensors transmit at once, a bit arrives flipped,
 * the line is upside down, a carrier sits on the band. The decoder may lose frames to all of
 * that; what it must never do is hand back a code the transmitter did not send - except a
 * flipped bit, which a code without a checksum cannot show, and which is why a zone acts only
 * on a frame that arrived twice (zone_hub's rf suite, RF-014).
 *
 * Captures from real sensors (the Flasher's "Record 3 seconds", `rf capture`) go in test/rf/
 * with an "# expect:" line and are decoded by test_recorded_captures (RF-002).
 *
 * Requirements: RF-002, RF-012, RF-013, RF-018
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rf_decode/rf_decode.h"
#include "unity.h"

#define CODE 0xA2B3C5u /* address 0xA2B3C, data 5 */
#define UNIT 350u
#define FRAME (2u + 2u * RF_EV1527_BITS)
#define MAXP 60000u
#define MAXPK 4096u

static rf_decoder_t D;

/* ---- the harness: pulse streams, a channel, and what came out -------------------------------- */

typedef struct {
    rf_pulse_t p[MAXP];
    size_t n;
} stream_t;

static stream_t A, B, C;

typedef struct {
    rf_packet_t p[MAXPK];
    size_t n;
} got_t;

static got_t GOT;
static uint32_t SEED;

static uint32_t rnd(void)
{
    /* xorshift32: the same numbers on every compiler, so every failure can be replayed */
    uint32_t x = SEED != 0u ? SEED : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    SEED = x;
    return x;
}

static uint32_t rnd_in(uint32_t lo, uint32_t hi)
{
    return lo + rnd() % (hi - lo + 1u);
}

static void s_clear(stream_t *s)
{
    s->n = 0;
}

/* A pulse; the same level as the last one makes that one longer (a line has no two highs in a
 * row). */
static void s_push(stream_t *s, bool high, uint32_t us)
{
    if (us == 0u) {
        return;
    }
    if (s->n > 0u && s->p[s->n - 1u].high == high && s->p[s->n - 1u].width_us != 0u) {
        s->p[s->n - 1u].width_us += us;
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(s->n < MAXP, "stream buffer full");
    s->p[s->n].high = high;
    s->p[s->n].width_us = us;
    s->n++;
}

static void s_append(stream_t *s, const rf_pulse_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_push(s, p[i].high, p[i].width_us);
    }
}

static rf_pulse_t TMP[MAXP];

/* 'frames' frames of the packet back to back. */
static void s_burst(stream_t *s, const rf_packet_t *pk, uint16_t unit, unsigned frames)
{
    const size_t n = rf_encode_burst(pk, unit, frames, TMP, MAXP);
    TEST_ASSERT_TRUE_MESSAGE(n > 0u, "the encoder refused the packet");
    s_append(s, TMP, n);
}

/* The quiet line between transmissions: low. */
static void s_quiet(stream_t *s, uint32_t us)
{
    s_push(s, false, us);
}

/* The capture ended here (the line idle past its limit): the decoder is told with a gap. */
static void s_gap(stream_t *s)
{
    TEST_ASSERT_TRUE_MESSAGE(s->n < MAXP, "stream buffer full");
    s->p[s->n].high = false;
    s->p[s->n].width_us = 0u;
    s->n++;
}

/* A receiver's AGC static on an empty band: alternating pulses of min..max us. */
static void s_static(stream_t *s, size_t count, uint32_t min_us, uint32_t max_us)
{
    bool high = s->n > 0u ? !s->p[s->n - 1u].high : true;
    for (size_t i = 0; i < count; i++) {
        s_push(s, high, rnd_in(min_us, max_us));
        high = !high;
    }
}

/* Feed a stream (then the silence that ends a capture) into a fresh decoder. */
static void run_into(const stream_t *s, bool fresh)
{
    if (fresh) {
        rf_decoder_init(&D);
        GOT.n = 0;
    }
    rf_packet_t out[RF_DECODE_MAX_OUT];
    for (size_t i = 0; i < s->n; i++) {
        const size_t k = rf_decoder_pulse(&D, s->p[i].high, s->p[i].width_us, out);
        for (size_t j = 0; j < k && GOT.n < MAXPK; j++) {
            GOT.p[GOT.n++] = out[j];
        }
    }
    const size_t k = rf_decoder_gap(&D, out);
    for (size_t j = 0; j < k && GOT.n < MAXPK; j++) {
        GOT.p[GOT.n++] = out[j];
    }
}

static void run(const stream_t *s)
{
    run_into(s, true);
}

static bool same(const rf_packet_t *a, const rf_packet_t *b)
{
    return a->proto == b->proto && a->device == b->device && a->data == b->data &&
           a->extra == b->extra;
}

static size_t count_of(const rf_packet_t *want)
{
    size_t n = 0;
    for (size_t i = 0; i < GOT.n; i++) {
        n += same(&GOT.p[i], want);
    }
    return n;
}

/* Everything that came out is one of the packets allowed (the safety property). */
static void assert_only(const rf_packet_t *allowed, size_t n_allowed, const char *what)
{
    for (size_t i = 0; i < GOT.n; i++) {
        bool ok = false;
        for (size_t j = 0; j < n_allowed; j++) {
            ok = ok || same(&GOT.p[i], &allowed[j]);
        }
        if (!ok) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: a code nobody sent: %s %lX data %X", what,
                     rf_proto_name(GOT.p[i].proto), (unsigned long)GOT.p[i].device,
                     (unsigned)GOT.p[i].data);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

static rf_packet_t ev(uint32_t code)
{
    return rf_packet_make(RF_PROTO_EV1527, (code >> 4) & 0xFFFFFu, (uint8_t)(code & 0xFu), 0u, 0u);
}

/* One packet of each protocol, with fields that exercise every bit position. */
static rf_packet_t sample(uint8_t proto, uint32_t salt)
{
    switch (proto) {
    case RF_PROTO_EV1527: return ev(0x5A5A5u << 4 | (salt & 0xFu));
    case RF_PROTO_HT6P20B: return rf_packet_make(proto, 0x2C5A93u ^ salt, (uint8_t)(salt & 3u), 0u, 0u);
    case RF_PROTO_HT12E: return rf_packet_make(proto, 0xA5u ^ salt, (uint8_t)(0x9u ^ (salt & 0xFu)), 0u, 0u);
    case RF_PROTO_KEELOQ:
        return rf_packet_make(proto, 0x0ABCDEFu ^ salt, (uint8_t)(1u << (salt & 3u)), 0u,
                              0xDEADBEEFu ^ (salt * 2654435761u));
    case RF_PROTO_ARCTECH: return rf_packet_make(proto, (0x2345678u ^ salt) << 4 | (salt & 0xFu), (uint8_t)(salt & 1u), 0u, 0u);
    default: return rf_packet_make(proto, 0u, 0u, 0u, 0u);
    }
}

/* A typical unit for each protocol (the chips' datasheets / rc-switch). */
static uint16_t unit_of(uint8_t proto)
{
    switch (proto) {
    case RF_PROTO_HT6P20B: return 450u;
    case RF_PROTO_HT12E: return 300u;
    case RF_PROTO_KEELOQ: return 400u;
    case RF_PROTO_ARCTECH: return 260u;
    default: return UNIT;
    }
}

/* Marks (the protocol's active level) longer, spaces shorter by delta us: a receiver's slicer. */
static void ch_shift(stream_t *s, int32_t delta, bool mark_high)
{
    for (size_t i = 0; i < s->n; i++) {
        const bool mark = s->p[i].high == mark_high;
        const int64_t w = (int64_t)s->p[i].width_us + (mark ? delta : -delta);
        s->p[i].width_us = (uint32_t)(w < 1 ? 1 : w);
    }
}

static void ch_scale(stream_t *s, unsigned high_pct, unsigned low_pct)
{
    for (size_t i = 0; i < s->n; i++) {
        s->p[i].width_us = s->p[i].width_us * (s->p[i].high ? high_pct : low_pct) / 100u;
        if (s->p[i].width_us == 0u) {
            s->p[i].width_us = 1u;
        }
    }
}

/* Every edge moved at random by up to 'us' (a noisy slicer): the pulse on either side of an
 * edge gains what the other loses, so the time line stays whole. */
static void ch_jitter(stream_t *s, uint32_t us)
{
    for (size_t i = 0; i + 1u < s->n; i++) {
        const int32_t j = (int32_t)rnd_in(0u, 2u * us) - (int32_t)us;
        const int64_t a = (int64_t)s->p[i].width_us + j;
        const int64_t b = (int64_t)s->p[i + 1u].width_us - j;
        if (a >= 1 && b >= 1) {
            s->p[i].width_us = (uint32_t)a;
            s->p[i + 1u].width_us = (uint32_t)b;
        }
    }
}

/* The capture lost a mark and its space (its task fell behind): the pulses around the hole
 * join, as the RMT hands them over. */
static void ch_drop_pair(stream_t *s, size_t at)
{
    TEST_ASSERT_TRUE(at + 2u <= s->n);
    const size_t n = s->n;
    stream_t *t = &C;
    s_clear(t);
    for (size_t i = 0; i < n; i++) {
        if (i != at && i != at + 1u) {
            s_push(t, s->p[i].high, s->p[i].width_us);
        }
    }
    memcpy(s->p, t->p, t->n * sizeof(t->p[0]));
    s->n = t->n;
}

/* Noise cut into pulse 'at': a spike of the other level, 'w' us, in its middle. */
static void ch_glitch(stream_t *s, size_t at, uint32_t w)
{
    TEST_ASSERT_TRUE(at < s->n && s->n + 2u <= MAXP);
    const rf_pulse_t p = s->p[at];
    TEST_ASSERT_TRUE(p.width_us > w + 2u);
    memmove(&s->p[at + 3u], &s->p[at + 1u], (s->n - at - 1u) * sizeof(s->p[0]));
    const uint32_t first = (p.width_us - w) / 2u;
    s->p[at] = (rf_pulse_t){p.high, first};
    s->p[at + 1u] = (rf_pulse_t){!p.high, w};
    s->p[at + 2u] = (rf_pulse_t){p.high, p.width_us - w - first};
    s->n += 2u;
}

/* One bit flipped on the way: a pair's mark and space swap lengths (1T+3T becomes 3T+1T). */
static void ch_flip(stream_t *s, size_t at)
{
    const uint32_t w = s->p[at].width_us;
    s->p[at].width_us = s->p[at + 1u].width_us;
    s->p[at + 1u].width_us = w;
}

static void ch_invert(stream_t *s)
{
    for (size_t i = 0; i < s->n; i++) {
        s->p[i].high = !s->p[i].high;
    }
}

/* Two transmitters at once, neither much louder: an OOK receiver hears carrier while either
 * sends (the high intervals OR together). b starts 'offset' us after a. */
static void ch_collide(const stream_t *a, const stream_t *b, uint64_t offset, stream_t *out)
{
    static uint64_t edges_a[MAXP * 2u], edges_b[MAXP * 2u];
    size_t na = 0, nb = 0;
    uint64_t t = 0;
    for (size_t i = 0; i < a->n; i++) {
        if (a->p[i].high) {
            edges_a[na++] = t;
            edges_a[na++] = t + a->p[i].width_us;
        }
        t += a->p[i].width_us;
    }
    const uint64_t end_a = t;
    t = offset;
    for (size_t i = 0; i < b->n; i++) {
        if (b->p[i].high) {
            edges_b[nb++] = t;
            edges_b[nb++] = t + b->p[i].width_us;
        }
        t += b->p[i].width_us;
    }
    const uint64_t end = t > end_a ? t : end_a;
    /* walk time in 1 us steps would be slow: merge the two interval lists */
    s_clear(out);
    uint64_t now = 0;
    size_t ia = 0, ib = 0;
    while (ia < na || ib < nb) {
        uint64_t s0, e0;
        if (ib >= nb || (ia < na && edges_a[ia] <= edges_b[ib])) {
            s0 = edges_a[ia];
            e0 = edges_a[ia + 1u];
            ia += 2u;
        } else {
            s0 = edges_b[ib];
            e0 = edges_b[ib + 1u];
            ib += 2u;
        }
        /* extend while the next interval of either list overlaps */
        for (;;) {
            if (ia < na && edges_a[ia] <= e0) {
                e0 = edges_a[ia + 1u] > e0 ? edges_a[ia + 1u] : e0;
                ia += 2u;
            } else if (ib < nb && edges_b[ib] <= e0) {
                e0 = edges_b[ib + 1u] > e0 ? edges_b[ib + 1u] : e0;
                ib += 2u;
            } else {
                break;
            }
        }
        if (s0 > now) {
            s_push(out, false, (uint32_t)(s0 - now));
        }
        s_push(out, true, (uint32_t)(e0 - s0));
        now = e0;
    }
    if (end > now) {
        s_push(out, false, (uint32_t)(end - now));
    }
}

void setUp(void)
{
    rf_decoder_init(&D);
    GOT.n = 0;
    SEED = 12345u;
    s_clear(&A);
    s_clear(&B);
    s_clear(&C);
}

void tearDown(void) {}

/* ---- one frame, the fields, the table (RF-002, RF-012) ---------------------------------------- */

static void test_a_frame_decodes(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 1u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(1u, GOT.n);
    TEST_ASSERT_EQUAL_UINT8(RF_PROTO_EV1527, GOT.p[0].proto);
    TEST_ASSERT_EQUAL_HEX32(CODE, GOT.p[0].code);
    TEST_ASSERT_EQUAL_HEX32(0xA2B3Cu, GOT.p[0].device);
    TEST_ASSERT_EQUAL_UINT8(5u, GOT.p[0].data);
    TEST_ASSERT_EQUAL_UINT8(24u, GOT.p[0].bits);
    TEST_ASSERT_UINT16_WITHIN(3u, UNIT, GOT.p[0].unit_us);
    TEST_ASSERT_EQUAL_UINT32(1u, D.frames);
    TEST_ASSERT_EQUAL_UINT32(0u, D.rejected);
}

static void test_the_address_and_the_data_split(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xA2B3Cu, rf_ev1527_address(CODE));
    TEST_ASSERT_EQUAL_UINT8(5u, rf_ev1527_data(CODE));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFu, rf_ev1527_address(0xFFFFFFu));
}

/* The numbers are stored in bindings (zsrc_rf): they never change. */
static void test_the_protocol_table_is_stable(void)
{
    TEST_ASSERT_EQUAL_INT(1, RF_PROTO_EV1527);
    TEST_ASSERT_EQUAL_INT(2, RF_PROTO_HT6P20B);
    TEST_ASSERT_EQUAL_INT(3, RF_PROTO_HT12E);
    TEST_ASSERT_EQUAL_INT(4, RF_PROTO_KEELOQ);
    TEST_ASSERT_EQUAL_INT(5, RF_PROTO_ARCTECH);
    static const struct {
        uint8_t proto;
        const char *name;
        uint8_t device_bits, data_bits;
        bool rolling;
    } want[] = {
        {RF_PROTO_EV1527, "EV1527", 20u, 4u, false},  {RF_PROTO_HT6P20B, "HT6P20B", 22u, 2u, false},
        {RF_PROTO_HT12E, "HT12E", 8u, 4u, false},     {RF_PROTO_KEELOQ, "KeeLoq", 28u, 4u, true},
        {RF_PROTO_ARCTECH, "arctech", 30u, 1u, false},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const rf_proto_info_t *info = rf_proto_info(want[i].proto);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_STRING(want[i].name, info->name);
        TEST_ASSERT_EQUAL_UINT8(want[i].device_bits, info->device_bits);
        TEST_ASSERT_EQUAL_UINT8(want[i].data_bits, info->data_bits);
        TEST_ASSERT_EQUAL(want[i].rolling, info->rolling);
        TEST_ASSERT_FALSE(info->checked); /* none of these carries a checksum (RF-014) */
    }
    TEST_ASSERT_NULL(rf_proto_info(0u));
    TEST_ASSERT_NULL(rf_proto_info(RF_PROTO_COUNT));
    TEST_ASSERT_EQUAL_STRING("RF", rf_proto_name(200u));
}

/* RF-012: every protocol, many random packets: what goes out comes back, field for field. */
static void test_every_protocol_round_trips(void)
{
    for (uint8_t proto = 1u; proto < RF_PROTO_COUNT; proto++) {
        const rf_proto_info_t *info = rf_proto_info(proto);
        for (unsigned k = 0; k < 200u; k++) {
            uint32_t dev = rnd();
            uint8_t data = (uint8_t)rnd();
            if (proto == RF_PROTO_KEELOQ && (data & 0xFu) == 0u) {
                data = 1u; /* a KeeLoq frame always carries a pressed button */
            }
            const uint8_t flags = proto == RF_PROTO_KEELOQ ? (uint8_t)(rnd() & (RF_PKT_BATTERY_LOW | RF_PKT_REPEAT))
                                  : proto == RF_PROTO_ARCTECH ? (uint8_t)(rnd() & RF_PKT_GROUP) : 0u;
            const rf_packet_t want = rf_packet_make(proto, dev, data, flags, rnd());
            s_clear(&A);
            s_quiet(&A, 30000u);
            s_burst(&A, &want, unit_of(proto), 3u);
            run(&A);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s packet %u", info->name, k);
            TEST_ASSERT_TRUE_MESSAGE(count_of(&want) >= 2u, msg);
            assert_only(&want, 1u, msg);
            const rf_packet_t *g = &GOT.p[0];
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(want.flags & (RF_PKT_BATTERY_LOW | RF_PKT_REPEAT | RF_PKT_GROUP | RF_PKT_ROLLING | RF_PKT_PT2262),
                                            g->flags & (RF_PKT_BATTERY_LOW | RF_PKT_REPEAT | RF_PKT_GROUP | RF_PKT_ROLLING | RF_PKT_PT2262), msg);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(want.code, g->code, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(want.bits, g->bits, msg);
        }
    }
}

static void test_repeats_decode_one_by_one(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 4u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(4u, count_of(&want));
    assert_only(&want, 1u, "four repeats");
}

/* A PT2262 sends its sync after the code word, so a burst ends with a sync mark: the last frame
 * still counts (the frame before the sync is whole), and the first code word, which has no sync
 * before it, is the one lost. */
static void test_a_sync_after_the_code_word_pt2262(void)
{
    const rf_packet_t want = ev(0x55F0F0u);
    s_burst(&A, &want, UNIT, 4u);
    /* move the first sync to the end: code, sync, code, sync ... */
    stream_t *s = &B;
    s_clear(s);
    s_append(s, &A.p[2], A.n - 2u);
    s_push(s, true, UNIT);
    s_push(s, false, UNIT * 31u);
    run(s);
    TEST_ASSERT_EQUAL_UINT(3u, count_of(&want));
    TEST_ASSERT_TRUE(GOT.p[0].flags & RF_PKT_PT2262);
    assert_only(&want, 1u, "PT2262 order");
}

static void test_pt2262_shaped_codes_are_recognised(void)
{
    rf_packet_t p = ev(0x0F0F0Fu); /* every pair 00 or 11: a tri-state remote could send it */
    s_burst(&A, &p, UNIT, 2u);
    run(&A);
    TEST_ASSERT_TRUE(GOT.n > 0u && (GOT.p[0].flags & RF_PKT_PT2262) != 0u);
    s_clear(&A);
    p = ev(CODE); /* 1010 ...: a 10 pair, which tri-state symbols never make */
    s_burst(&A, &p, UNIT, 2u);
    run(&A);
    TEST_ASSERT_TRUE(GOT.n > 0u && (GOT.p[0].flags & RF_PKT_PT2262) == 0u);
}

/* ---- the transmitter's speed and its drift (RF-002) ------------------------------------------ */

static void test_every_transmitter_speed_decodes(void)
{
    static const uint16_t units[] = {120u, 150u, 200u, 300u, 400u, 500u, 600u, 800u};
    for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
        s_clear(&A);
        const rf_packet_t want = ev(0x5A5A5Au);
        s_burst(&A, &want, units[u], 2u);
        run(&A);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, count_of(&want), "a frame at this T did not decode");
        TEST_ASSERT_UINT16_WITHIN(units[u] / 50u + 2u, units[u], GOT.p[0].unit_us);
    }
}

/* Feed a stream as the receiver hands it over: a low as long as a reception's idle ends the
 * reception (the capture stops measuring it), so the decoder gets a gap instead (rf_capture's
 * RF_CAPTURE_IDLE_US is RF_RECEPTION_IDLE_US). */
static void run_received(const stream_t *s)
{
    rf_decoder_init(&D);
    GOT.n = 0;
    rf_packet_t out[RF_DECODE_MAX_OUT];
    for (size_t i = 0; i < s->n; i++) {
        size_t k;
        if (!s->p[i].high && s->p[i].width_us >= RF_RECEPTION_IDLE_US) {
            k = rf_decoder_gap(&D, out);
        } else {
            k = rf_decoder_pulse(&D, s->p[i].high, s->p[i].width_us, out);
        }
        for (size_t j = 0; j < k && GOT.n < MAXPK; j++) {
            GOT.p[GOT.n++] = out[j];
        }
    }
    const size_t k = rf_decoder_gap(&D, out);
    for (size_t j = 0; j < k && GOT.n < MAXPK; j++) {
        GOT.p[GOT.n++] = out[j];
    }
}

/* RF-002, RF-024: every protocol at the slowest T the decoder takes (its table's t_max) fits
 * in one reception - its sync's long part (EV1527's 31 T, HT12E's 36 T pilot) is shorter than the
 * idle that ends a reception - or a slow transmitter, or a 12 V EV1527 whose cell sags (B025),
 * would decode here and never on a panel. Of three frames, the first after the silence (a long-low
 * sync has no pilot then) or the last (its last mark may be the next sync's) may be lost. */
static void test_every_protocol_at_its_slowest_fits_a_reception(void)
{
    static const struct {
        uint8_t proto;
        uint16_t slowest_us; /* rf_decode.c PROTOS: t_max_us */
    } SLOWEST[] = {{RF_PROTO_EV1527, RF_EV1527_UNIT_MAX_US}, {RF_PROTO_HT6P20B, 800u},
                   {RF_PROTO_HT12E, 800u}, {RF_PROTO_KEELOQ, 800u}, {RF_PROTO_ARCTECH, 500u}};
    TEST_ASSERT_EQUAL_UINT(RF_PROTO_COUNT - 1u, sizeof(SLOWEST) / sizeof(SLOWEST[0]));
    for (size_t i = 0; i < sizeof(SLOWEST) / sizeof(SLOWEST[0]); i++) {
        const rf_packet_t want = sample(SLOWEST[i].proto, 0x5u);
        s_clear(&A);
        s_quiet(&A, 100000u);
        s_burst(&A, &want, SLOWEST[i].slowest_us, 3u);
        s_quiet(&A, 100000u);
        run_received(&A);
        const bool checked = (want.flags & RF_PKT_CHECKED) != 0u;
        TEST_ASSERT_TRUE_MESSAGE(count_of(&want) >= (checked ? 1u : 2u),
                                 rf_proto_name(SLOWEST[i].proto));
        assert_only(&want, 1u, rf_proto_name(SLOWEST[i].proto));
    }
}

static void test_a_transmitter_too_fast_or_too_slow_is_no_sync(void)
{
    const rf_packet_t p = ev(CODE);
    s_burst(&A, &p, 80u, 3u); /* 80 us units: nothing a fixed-code chip makes */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(0u, count_of(&p));
    s_clear(&A);
    s_burst(&A, &p, 1200u, 3u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(0u, count_of(&p));
}

/* An RC oscillator runs slower as the battery sags and the cold sets in (EV1527: T moves with
 * supply and temperature): T drifts 20 % over a long burst. T is measured on every frame. */
static void test_a_drifting_oscillator_decodes(void)
{
    const rf_packet_t want = ev(CODE);
    for (unsigned f = 0; f < 10u; f++) {
        s_burst(&A, &want, (uint16_t)(300u + f * 7u), 1u);
    }
    run(&A);
    TEST_ASSERT_EQUAL_UINT(10u, count_of(&want));
}

/* ---- the receiver bends the pulses (RF-002, RF-013) ------------------------------------------- */

/* A slicer moves every edge by about the same microseconds: marks longer and spaces shorter (a
 * strong signal) or the other way round (a weak one). Up to 0.6 T either way still decodes. */
static void test_edges_moved_by_the_slicer_still_decode(void)
{
    static const int32_t deltas[] = {-200, -150, -100, -50, 50, 100, 150, 200};
    for (size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
        s_clear(&A);
        const rf_packet_t want = ev(0x0123FEu);
        s_burst(&A, &want, UNIT, 3u);
        ch_shift(&A, deltas[i], true);
        run(&A);
        char msg[64];
        snprintf(msg, sizeof(msg), "edges moved %ld us", (long)deltas[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(3u, count_of(&want), msg);
    }
}

/* The same for every protocol, by a quarter of its unit: the 1T/2T protocols (the Holteks,
 * KeeLoq) have half the room of EV1527's 1T/3T, so this is about their limit. */
static void test_every_protocol_survives_the_slicer(void)
{
    for (uint8_t proto = 1u; proto < RF_PROTO_COUNT; proto++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const rf_packet_t want = sample(proto, 7u);
            s_clear(&A);
            s_quiet(&A, 30000u);
            s_burst(&A, &want, unit_of(proto), 3u);
            const bool inverted = proto == RF_PROTO_HT6P20B || proto == RF_PROTO_HT12E;
            ch_shift(&A, sign * (int32_t)(unit_of(proto) / 4u), !inverted);
            run(&A);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s, edges moved %d x T/4", rf_proto_name(proto), sign);
            TEST_ASSERT_TRUE_MESSAGE(count_of(&want) >= 2u, msg);
            assert_only(&want, 1u, msg);
        }
    }
}

/* Proportional bending (every high longer by a share of itself, every low shorter): a receiver
 * does not really do this - it moves edges by microseconds (above) - but moderate amounts must
 * decode all the same. Beyond about a quarter a '1' no longer lasts 4T and is refused. */
static void test_stretched_and_shrunk_pulses_still_decode(void)
{
    rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 2u);
    ch_scale(&A, 115u, 90u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));

    s_clear(&A);
    want = ev(0x0123FEu);
    s_burst(&A, &want, 280u, 2u);
    ch_scale(&A, 80u, 110u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));

    /* the old 65 % / 125 % case, which the unit-from-the-sync-high decoder took: the edges
     * moved the way a slicer really moves them - the same microseconds - decode */
    s_clear(&A);
    s_burst(&A, &want, 280u, 2u);
    ch_shift(&A, -(int32_t)(280u * 35u / 100u), true);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));
}

/* Past what a bit can be told from (edges moved 0.9 T): frames are lost, never mistaken. */
static void test_edges_moved_too_far_lose_frames_not_codes(void)
{
    for (int sign = -1; sign <= 1; sign += 2) {
        s_clear(&A);
        const rf_packet_t want = ev(CODE);
        s_burst(&A, &want, UNIT, 6u);
        ch_shift(&A, sign * (int32_t)(UNIT * 9u / 10u), true);
        run(&A);
        assert_only(&want, 1u, "edges moved 0.9 T");
    }
}

/* A noisy slicer moves each edge at random: 12 % of T decodes every frame; 30 % loses some and
 * never makes a wrong one. */
static void test_jittered_edges(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 20u);
    ch_jitter(&A, UNIT * 12u / 100u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(20u, count_of(&want));

    for (unsigned round = 0; round < 50u; round++) {
        s_clear(&A);
        s_burst(&A, &want, UNIT, 10u);
        ch_jitter(&A, UNIT * 30u / 100u);
        run(&A);
        assert_only(&want, 1u, "30 % jitter");
    }
}

/* A superheterodyne receiver's AGC has turned the gain right up on the empty band when the
 * burst starts: the first few pulses come out chopped (docs/features/25). The first frame is lost;
 * the rest decode. */
static void test_the_first_frame_lost_to_the_agc(void)
{
    const rf_packet_t want = ev(CODE);
    s_static(&A, 300u, 60u, 900u);
    const size_t start = A.n;
    s_burst(&A, &want, UNIT, 6u);
    for (size_t i = start; i < start + 14u; i++) {
        A.p[i].width_us = A.p[i].high ? A.p[i].width_us / 3u + 1u : A.p[i].width_us + 200u;
    }
    run(&A);
    TEST_ASSERT_TRUE(count_of(&want) >= 5u);
    assert_only(&want, 1u, "AGC settling");
}

/* An upside-down line (a module that idles high, CONFIG_ALARM_RF_INVERT not set): nothing, and
 * certainly no wrong code. */
static void test_an_inverted_line_decodes_nothing(void)
{
    for (uint8_t proto = 1u; proto < RF_PROTO_COUNT; proto++) {
        const rf_packet_t want = sample(proto, 3u);
        s_clear(&A);
        s_burst(&A, &want, unit_of(proto), 4u);
        ch_invert(&A);
        run(&A);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, count_of(&want), rf_proto_name(proto));
        assert_only(NULL, 0u, "inverted line");
    }
}

/* ---- noise, glitches and lost pulses (RF-013) ----------------------------------------------- */

static void test_noise_before_the_sync_is_ignored(void)
{
    const rf_packet_t want = ev(CODE);
    s_static(&A, 400u, 40u, 2040u); /* the band's usual chatter */
    s_quiet(&A, 700u);
    s_burst(&A, &want, UNIT, 2u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));
    assert_only(&want, 1u, "noise first");
}

static void test_a_glitch_inside_a_pulse_is_healed(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 2u);
    ch_glitch(&A, 2u, 15u); /* the first bit's mark, cut in two by 15 us of low */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));
    TEST_ASSERT_EQUAL_UINT32(1u, D.glitches);
}

/* The AGC's spikes land in the long sync low; shorter than RF_GLITCH_US they heal. */
static void test_spikes_in_the_sync_low_are_healed(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 3u);
    const size_t syncs[] = {1u, 1u + FRAME, 1u + 2u * FRAME};
    for (size_t k = 3; k-- > 0u;) { /* from the last: an earlier index stays where it was */
        TEST_ASSERT_FALSE(A.p[syncs[k]].high);
        ch_glitch(&A, syncs[k], 25u + 10u * (uint32_t)k);
    }
    run(&A);
    TEST_ASSERT_EQUAL_UINT(3u, count_of(&want));
    TEST_ASSERT_EQUAL_UINT32(3u, D.glitches);
}

/* A glitch in every frame at a random place, of 10..55 us: healed, every frame decodes. */
static void test_glitches_anywhere_are_healed(void)
{
    const rf_packet_t want = ev(0x3C3C3Cu);
    for (unsigned round = 0; round < 200u; round++) {
        s_clear(&A);
        s_burst(&A, &want, UNIT, 2u);
        const size_t at = rnd_in(0u, (uint32_t)A.n - 2u);
        if (A.p[at].width_us > 120u) {
            ch_glitch(&A, at, rnd_in(10u, RF_GLITCH_US - 5u));
        }
        run(&A);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, count_of(&want), "a glitch broke a frame");
    }
}

/* A glitch longer than the filter (a real pulse of noise) breaks its frame, never a code. */
static void test_long_glitches_lose_frames_not_codes(void)
{
    const rf_packet_t want = ev(0x3C3C3Cu);
    for (unsigned round = 0; round < 300u; round++) {
        s_clear(&A);
        s_burst(&A, &want, UNIT, 3u);
        for (unsigned g = 0; g < 3u; g++) {
            const size_t at = rnd_in(0u, (uint32_t)A.n - 2u);
            if (A.p[at].width_us > 300u) {
                ch_glitch(&A, at, rnd_in(RF_GLITCH_US, 250u));
            }
        }
        run(&A);
        assert_only(&want, 1u, "long glitches");
    }
}

/* RF-013: the capture drops a mark and its space (an overrun): that frame is lost, the next one
 * decodes - and the bits never shift into another code. Every position, several codes, both
 * the EV1527 order and the PT2262 order (sync after the code word). */
static void test_a_lost_pulse_pair_never_shifts_into_another_code(void)
{
    static const uint32_t codes[] = {0xA2B3C5u, 0x000001u, 0xFFFFFEu, 0x555554u, 0xAAAAABu, 0x0F0F0Eu};
    for (size_t c = 0; c < sizeof(codes) / sizeof(codes[0]); c++) {
        const rf_packet_t want = ev(codes[c]);
        for (size_t drop = 0; drop < FRAME * 2u; drop += 2u) {
            for (int order = 0; order < 2; order++) {
                s_clear(&A);
                s_burst(&A, &want, UNIT, 4u); /* the drop lands in frames 2 and 3 */
                if (order == 1) { /* the PT2262 order: code word, then sync */
                    s_clear(&B);
                    s_append(&B, &A.p[2], A.n - 2u);
                    s_push(&B, true, UNIT);
                    s_push(&B, false, UNIT * 31u);
                    A = B;
                }
                ch_drop_pair(&A, FRAME + drop < A.n - 2u ? FRAME + drop : A.n - 2u);
                run(&A);
                char msg[80];
                snprintf(msg, sizeof(msg), "code %06lX, pair %u dropped, order %d",
                         (unsigned long)codes[c], (unsigned)drop, order);
                assert_only(&want, 1u, msg);
                TEST_ASSERT_TRUE_MESSAGE(count_of(&want) >= 1u, msg);
            }
        }
    }
}

/* The same for every protocol: pairs dropped at random places. */
static void test_lost_pairs_never_make_another_code_in_any_protocol(void)
{
    for (uint8_t proto = 1u; proto < RF_PROTO_COUNT; proto++) {
        for (unsigned round = 0; round < 150u; round++) {
            const rf_packet_t want = sample(proto, round);
            s_clear(&A);
            s_quiet(&A, 30000u);
            s_burst(&A, &want, unit_of(proto), 3u);
            ch_drop_pair(&A, rnd_in(1u, (uint32_t)A.n - 3u));
            run(&A);
            assert_only(&want, 1u, rf_proto_name(proto));
        }
    }
}

/* A pulse pair too many (a noise pair right after a frame, or inside it) breaks that frame. */
static void test_an_extra_pair_breaks_its_frame(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 3u);
    /* two more '0' bits at the end of the first frame */
    s_clear(&B);
    s_append(&B, A.p, FRAME);
    s_push(&B, true, UNIT);
    s_push(&B, false, 3u * UNIT);
    s_append(&B, &A.p[FRAME], A.n - FRAME);
    run(&B);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));
    assert_only(&want, 1u, "an extra pair");
}

static void test_a_truncated_frame_is_rejected_and_the_next_one_decodes(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 1u);
    A.n -= 6u; /* the transmitter dies three bits early */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(0u, GOT.n);
    TEST_ASSERT_EQUAL_UINT32(1u, D.rejected);
    TEST_ASSERT_EQUAL_UINT32(0u, D.frames);
    s_clear(&A);
    s_burst(&A, &want, UNIT, 1u);
    run_into(&A, false);
    TEST_ASSERT_EQUAL_UINT(1u, count_of(&want));
}

/* The last frame of a burst loses its final space to the silence. When its last bit is a '1'
 * (a long mark) the mark alone says so; a '0' mark is as short as the next sync's mark, so that
 * last frame is given up rather than risk a shifted code (the frames before it have counted). */
static void test_the_last_frame_of_a_burst_ends_on_the_silence(void)
{
    rf_packet_t want = ev(0xA2B3C5u); /* ends in 1 */
    s_burst(&A, &want, UNIT, 2u);
    A.n -= 1u; /* the final space is the silence itself */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));

    s_clear(&A);
    want = ev(0xA2B3C4u); /* ends in 0 */
    s_burst(&A, &want, UNIT, 2u);
    A.n -= 1u;
    run(&A);
    TEST_ASSERT_EQUAL_UINT(1u, count_of(&want));
    assert_only(&want, 1u, "a burst ending in 0");
}

/* A bit flipped on the air (both of a pair's edges moved a whole 2T): EV1527 has no checksum,
 * so that one frame is another code, and nothing here can tell. The frames before and after it
 * are right. This is why zsrc_rf acts only on a frame it has received twice (RF-014). */
static void test_a_flipped_bit_is_another_code_once(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 3u);
    ch_flip(&A, FRAME + 2u + 2u * 21u); /* the second frame's bit 22 */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(3u, GOT.n);
    TEST_ASSERT_EQUAL_UINT(2u, count_of(&want));
    const rf_packet_t bent = ev(CODE ^ 0x000004u);
    TEST_ASSERT_EQUAL_UINT(1u, count_of(&bent));
}

/* ---- two transmitters at once (RF-013) ---------------------------------------------------------- */

/* The door opens and the PIR sees whoever opened it: both transmit. While their frames overlap
 * nothing (or one of them) decodes; never a mixture. Once one is done the other decodes.
 * Their clocks differ by 3 % or more, as two sensors' RC oscillators do: the overlap then slides
 * along the frame and leaves pairs that are no bits (the frame's consistency, RF-013). Two
 * transmitters within a fraction of a percent stay aligned for frames on end, and the OR of
 * their carriers can be a clean frame of a third code - see the next test. */
static void test_colliding_transmitters_never_mix_into_a_third_code(void)
{
    const rf_packet_t door = ev(0xA2B3C5u);
    const rf_packet_t pir = ev(0x1F0D7Au);
    const rf_packet_t both[] = {door, pir};
    for (unsigned round = 0; round < 300u; round++) {
        s_clear(&A);
        s_clear(&B);
        const uint16_t ua = (uint16_t)rnd_in(300u, 400u);
        uint16_t ub = (uint16_t)rnd_in(300u, 400u);
        if (ub * 100u < ua * 103u && ua * 100u < ub * 103u) {
            ub = (uint16_t)(ua * 104u / 100u);
        }
        s_burst(&A, &door, ua, 6u);
        s_burst(&B, &pir, ub, 6u);
        const uint64_t offset = rnd_in(0u, 200000u);
        static stream_t mixed;
        ch_collide(&A, &B, offset, &mixed);
        run(&mixed);
        char what[96];
        snprintf(what, sizeof(what), "collision round %u (T %u and %u us, %lu us apart)", round,
                 (unsigned)ua, (unsigned)ub, (unsigned long)offset);
        assert_only(both, 2u, what);
    }
}

/* The limit of a code without a checksum: two transmitters with the same clock, one bit apart,
 * OR into frames of a third code, the same frame after frame. The decoder cannot tell; the
 * third code is an address nobody bound (zsrc_rf counts it unknown), and learning names the
 * sensor that answered most (RF-011). Kept here so the limit stays written down. */
static void test_two_aligned_transmitters_can_make_a_third_code(void)
{
    const rf_packet_t door = ev(0xA2B3C5u);
    const rf_packet_t pir = ev(0x1F0D7Au);
    s_burst(&A, &door, 350u, 6u);
    s_burst(&B, &pir, 350u, 6u);
    static stream_t mixed;
    ch_collide(&A, &B, 0u, &mixed); /* exactly together: every high of either is carrier */
    run(&mixed);
    const rf_packet_t either = ev(0xA2B3C5u | 0x1F0D7Au);
    TEST_ASSERT_TRUE(count_of(&either) >= 5u);
    TEST_ASSERT_EQUAL_UINT(0u, count_of(&door));
    TEST_ASSERT_EQUAL_UINT(0u, count_of(&pir));
}

/* A sensor far away and one next to the panel: the strong one "captures" the receiver (the
 * weak one's carrier only adds a little to the strong one's) - modelled as the strong one alone
 * plus the weak one's frames where the strong is quiet. */
static void test_a_collision_where_one_finishes_first(void)
{
    const rf_packet_t door = ev(0xA2B3C5u);
    const rf_packet_t pir = ev(0x1F0D7Au);
    s_burst(&A, &door, 350u, 4u);
    s_burst(&B, &pir, 330u, 8u);
    static stream_t mixed;
    ch_collide(&A, &B, 0u, &mixed); /* overlapping from the start; the door stops first */
    run(&mixed);
    const rf_packet_t both[] = {door, pir};
    assert_only(both, 2u, "overlap");
    TEST_ASSERT_TRUE(count_of(&pir) >= 2u); /* the PIR's last frames are alone on the air */
}

/* ---- other protocols and other devices on the band (RF-012) ----------------------------------- */

/* Each protocol's frames decode as that protocol only, never as another (the same pulses read
 * with another protocol's unit and templates break its rules). */
static void test_protocols_do_not_read_each_other(void)
{
    for (uint8_t proto = 1u; proto < RF_PROTO_COUNT; proto++) {
        for (unsigned k = 0; k < 400u; k++) {
            uint8_t data = (uint8_t)rnd();
            if (proto == RF_PROTO_KEELOQ && (data & 0xFu) == 0u) {
                data = 2u;
            }
            const rf_packet_t want = rf_packet_make(proto, rnd(), data, 0u, rnd());
            s_clear(&A);
            s_quiet(&A, 30000u);
            const uint16_t unit = (uint16_t)(unit_of(proto) * rnd_in(80u, 120u) / 100u);
            s_burst(&A, &want, unit, 4u);
            run(&A);
            for (size_t i = 0; i < GOT.n; i++) {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s frames read as %s", rf_proto_name(proto),
                         rf_proto_name(GOT.p[i].proto));
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(proto, GOT.p[i].proto, msg);
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "%s round %u (T %u us, device %lX data %u): %u of 4 frames",
                     rf_proto_name(proto), k, (unsigned)unit, (unsigned long)want.device,
                     (unsigned)want.data, (unsigned)count_of(&want));
            TEST_ASSERT_TRUE_MESSAGE(count_of(&want) >= 3u, msg);
        }
    }
}

/* Only the protocols enabled decode. */
static void test_a_protocol_can_be_turned_off(void)
{
    const rf_packet_t hol = sample(RF_PROTO_HT6P20B, 1u);
    s_quiet(&A, 30000u);
    s_burst(&A, &hol, 450u, 3u);
    rf_decoder_init(&D);
    rf_decoder_enable(&D, 1u << RF_PROTO_EV1527);
    GOT.n = 0;
    run_into(&A, false);
    TEST_ASSERT_EQUAL_UINT(0u, GOT.n);
    run(&A);
    TEST_ASSERT_TRUE(count_of(&hol) >= 2u);
}

/* Weather stations and the like send Manchester or other pulse shapes: random ones never
 * decode twice alike. */
static void test_foreign_signals_never_decode_twice(void)
{
    for (unsigned round = 0; round < 100u; round++) {
        s_clear(&A);
        /* a Manchester frame at 500 us half-bits, 64 bits, repeated 3 times */
        uint32_t bits[2] = {rnd(), rnd()};
        for (unsigned f = 0; f < 3u; f++) {
            s_quiet(&A, 8000u);
            for (unsigned i = 0; i < 64u; i++) {
                const bool one = ((bits[i / 32u] >> (i % 32u)) & 1u) != 0u;
                s_push(&A, one, 500u);
                s_push(&A, !one, 500u);
            }
        }
        run(&A);
        for (size_t i = 0; i < GOT.n; i++) {
            TEST_ASSERT_TRUE_MESSAGE(count_of(&GOT.p[i]) < 2u, "a Manchester frame decoded twice alike");
        }
    }
}

/* Hours of an empty band's static, at a superheterodyne module's ~2000 edges a second: the
 * decoders find a frame now and then (random pulses can spell one), never the same one twice
 * in a row - which is what a transmission is (RF-014). */
static void test_static_never_makes_a_transmission(void)
{
    rf_decoder_init(&D);
    GOT.n = 0;
    size_t repeats = 0;
    rf_packet_t last;
    memset(&last, 0, sizeof(last));
    rf_packet_t out[RF_DECODE_MAX_OUT];
    bool high = true;
    for (uint32_t i = 0; i < 2000000u; i++) { /* ~17 minutes of static */
        uint32_t w;
        const uint32_t r = rnd() % 100u;
        w = r < 60u ? rnd_in(40u, 400u) : r < 95u ? rnd_in(400u, 1500u) : rnd_in(1500u, 15000u);
        const size_t k = rf_decoder_pulse(&D, high, w, out);
        for (size_t j = 0; j < k; j++) {
            repeats += same(&out[j], &last);
            last = out[j];
            GOT.n++;
        }
        high = !high;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, repeats, "static decoded the same frame twice in a row");
    TEST_ASSERT_TRUE_MESSAGE(GOT.n < 20u, "static decoded too many frames");
}

static void test_garbage_never_decodes_and_never_breaks(void)
{
    bool high = false;
    size_t packets = 0;
    rf_packet_t out[RF_DECODE_MAX_OUT];
    for (unsigned i = 0; i < 20000u; i++) {
        high = !high;
        packets += rf_decoder_pulse(&D, high, rnd() % 15000u, out);
    }
    packets += rf_decoder_gap(&D, out);
    TEST_ASSERT_EQUAL_UINT32(20000u, D.pulses);
    TEST_ASSERT_TRUE_MESSAGE(packets < 3u, "noise decoded as packets");
}

/* A carrier held on the band (a jammer, a stuck transmitter without modulation): nothing. */
static void test_a_carrier_decodes_nothing(void)
{
    const rf_packet_t want = ev(CODE);
    s_burst(&A, &want, UNIT, 2u);
    s_push(&A, true, 5000000u); /* five seconds of carrier */
    s_quiet(&A, 20000u);
    s_burst(&A, &want, UNIT, 2u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(4u, count_of(&want));
    assert_only(&want, 1u, "carrier");
}

/* ---- protocol rules (RF-012) ------------------------------------------------------------------- */

static void test_keeloq_fields(void)
{
    const rf_packet_t want = rf_packet_make(RF_PROTO_KEELOQ, 0x0ABCDEFu, 0x2u,
                                            RF_PKT_BATTERY_LOW, 0x12345678u);
    s_quiet(&A, 30000u);
    s_burst(&A, &want, 400u, 3u);
    run(&A);
    TEST_ASSERT_TRUE(GOT.n >= 2u);
    TEST_ASSERT_EQUAL_HEX32(0x0ABCDEFu, GOT.p[0].device);
    TEST_ASSERT_EQUAL_UINT8(0x2u, GOT.p[0].data);
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, GOT.p[0].extra);
    TEST_ASSERT_TRUE(GOT.p[0].flags & RF_PKT_BATTERY_LOW);
    TEST_ASSERT_TRUE(GOT.p[0].flags & RF_PKT_ROLLING);
    TEST_ASSERT_FALSE(GOT.p[0].flags & RF_PKT_REPEAT);
    /* every speed the chip's baud select gives */
    static const uint16_t te[] = {100u, 200u, 400u, 700u};
    for (size_t i = 0; i < sizeof(te) / sizeof(te[0]); i++) {
        s_clear(&A);
        s_quiet(&A, 30000u);
        s_burst(&A, &want, te[i], 3u);
        run(&A);
        TEST_ASSERT_TRUE_MESSAGE(count_of(&want) >= 2u, "a KeeLoq speed");
    }
}

/* No button in the fixed part: not a frame a KeeLoq encoder makes. */
static void test_keeloq_without_a_button_is_refused(void)
{
    rf_packet_t p = rf_packet_make(RF_PROTO_KEELOQ, 0x0ABCDEFu, 0x2u, 0u, 0x12345678u);
    p.data = 0u;
    s_quiet(&A, 30000u);
    s_burst(&A, &p, 400u, 3u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(0u, GOT.n);
}

static void test_ht6p20b_needs_its_anti_code(void)
{
    const rf_packet_t want = sample(RF_PROTO_HT6P20B, 2u);
    s_quiet(&A, 30000u);
    s_burst(&A, &want, 450u, 3u);
    run(&A);
    TEST_ASSERT_TRUE(count_of(&want) >= 2u);
    TEST_ASSERT_EQUAL_HEX32((want.device << 6) | ((uint32_t)want.data << 4) | 0x5u, GOT.p[0].code);
    /* the last anti-code bit flipped: 0100 */
    const size_t last_mark = A.n - 2u;
    TEST_ASSERT_FALSE(A.p[last_mark].high); /* inverted: marks are lows */
    s_clear(&B);
    s_quiet(&B, 30000u);
    s_burst(&B, &want, 450u, 1u);
    ch_flip(&B, B.n - 2u);
    run(&B);
    TEST_ASSERT_EQUAL_UINT(0u, GOT.n);
}

static void test_arctech_fields_and_manchester(void)
{
    const rf_packet_t on = rf_packet_make(RF_PROTO_ARCTECH, (0x1234567u << 4) | 0x9u, 1u, 0u, 0u);
    s_quiet(&A, 30000u);
    s_burst(&A, &on, 260u, 4u);
    run(&A);
    TEST_ASSERT_TRUE(count_of(&on) >= 3u);
    TEST_ASSERT_EQUAL_HEX32((0x1234567u << 4) | 0x9u, GOT.p[0].device);
    TEST_ASSERT_EQUAL_UINT8(1u, GOT.p[0].data);
    TEST_ASSERT_FALSE(GOT.p[0].flags & RF_PKT_GROUP);
    const rf_packet_t group_off = rf_packet_make(RF_PROTO_ARCTECH, (0x1234567u << 4), 0u, RF_PKT_GROUP, 0u);
    s_clear(&A);
    s_quiet(&A, 30000u);
    s_burst(&A, &group_off, 260u, 3u);
    run(&A);
    TEST_ASSERT_TRUE(count_of(&group_off) >= 2u);
    TEST_ASSERT_TRUE(GOT.p[0].flags & RF_PKT_GROUP);
    /* one physical symbol changed: its Manchester pair is 00 or 11 - refused */
    s_clear(&A);
    s_quiet(&A, 30000u);
    s_burst(&A, &on, 260u, 1u);
    /* the first data pulse pair after the sync (pulses 1..): the sync is quiet+H+L, then pairs */
    const size_t first = 3u; /* A: quiet low merged with..., sync H, sync L, then the first pair */
    TEST_ASSERT_TRUE(A.p[first].high);
    A.p[first + 1u].width_us = A.p[first + 3u].width_us; /* both halves of bit 0 now alike */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(0u, count_of(&on));
    assert_only(NULL, 0u, "a broken Manchester pair");
}

/* RF-002, RF03: the Proove / Nexa LMST-606 door contact. Its closed report is the ordinary
 * 32-bit arctech frame (64 physical bits); its open report carries 4 bits more (36 bits, 72
 * physical; rtl_433 nexa.c reads them as a dim level, 10 here). Both name the same house code
 * and unit, so a binding learned from either matches the other. The widths are rtl_433's own
 * measurements (-c 0, its OOK demodulator) of the public recordings tests/nexa_LMST-606/gfile001.cu8
 * (open) and gfile002.cu8 (closed) of merbanan/rtl_433_tests at 1aa8c29, the first three frames
 * of each as measured: the sync, the bit pairs, then the end mark and the silence after it. */
#define NEXA_DEVICE ((0x17F9D86u << 4) | 0x9u) /* rtl_433: id 25140614, unit 9 */
#define NEXA_OPEN_PAIRS 74u
#define NEXA_CLOSED_PAIRS 66u
static const uint16_t NEXA_OPEN[][74][2] = {
    {
        {276, 2680}, {272, 268}, {272, 1300}, {272, 1288}, {272, 276}, {272, 272}, {268, 1300}, {268, 1292},
        {268, 284}, {268, 1292}, {264, 284}, {268, 1292}, {272, 276}, {272, 1288}, {272, 276}, {276, 1288},
        {272, 284}, {272, 1288}, {272, 280}, {276, 1288}, {268, 276}, {272, 1296}, {264, 284}, {268, 276},
        {264, 1296}, {276, 268}, {272, 1296}, {276, 1284}, {272, 276}, {276, 1288}, {272, 276}, {280, 1280},
        {272, 284}, {272, 272}, {272, 1296}, {268, 1296}, {264, 284}, {268, 1288}, {272, 280}, {268, 272},
        {272, 1296}, {272, 268}, {276, 1296}, {272, 272}, {272, 1296}, {268, 272}, {280, 1288}, {272, 1292},
        {264, 288}, {272, 1288}, {272, 276}, {276, 272}, {268, 1296}, {272, 272}, {268, 1296}, {272, 1296},
        {264, 284}, {268, 1288}, {272, 284}, {264, 272}, {272, 1296}, {276, 264}, {268, 1300}, {272, 1292},
        {268, 292}, {272, 1288}, {268, 284}, {272, 268}, {276, 1292}, {272, 1288}, {272, 284}, {268, 268},
        {272, 1292}, {268, 10004},
    },
    {
        {272, 2680}, {272, 268}, {276, 1300}, {264, 1292}, {268, 284}, {268, 272}, {268, 1304}, {264, 1292},
        {272, 280}, {272, 1284}, {272, 280}, {268, 1288}, {272, 280}, {276, 1284}, {272, 284}, {264, 1292},
        {268, 288}, {272, 1288}, {272, 280}, {268, 1292}, {268, 280}, {276, 1284}, {272, 280}, {268, 272},
        {272, 1296}, {276, 268}, {272, 1292}, {272, 1288}, {272, 280}, {272, 1288}, {276, 276}, {268, 1288},
        {276, 280}, {276, 264}, {276, 1300}, {268, 1292}, {268, 276}, {272, 1288}, {272, 280}, {272, 268},
        {272, 1296}, {272, 268}, {276, 1296}, {272, 268}, {272, 1300}, {272, 268}, {268, 1300}, {272, 1288},
        {272, 288}, {272, 1288}, {264, 284}, {268, 276}, {268, 1296}, {268, 276}, {268, 1300}, {272, 1292},
        {264, 284}, {268, 1288}, {276, 272}, {280, 268}, {268, 1300}, {268, 268}, {272, 1296}, {272, 1288},
        {272, 292}, {272, 1292}, {268, 284}, {264, 272}, {268, 1300}, {272, 1292}, {268, 280}, {268, 272},
        {272, 1288}, {272, 10004},
    },
    {
        {272, 2680}, {268, 272}, {268, 1296}, {272, 1288}, {276, 276}, {268, 272}, {272, 1304}, {264, 1288},
        {272, 280}, {268, 1292}, {268, 280}, {272, 1292}, {268, 284}, {268, 1288}, {272, 276}, {272, 1288},
        {272, 284}, {272, 1288}, {280, 268}, {280, 1284}, {276, 276}, {272, 1284}, {276, 280}, {272, 264},
        {276, 1296}, {272, 268}, {272, 1296}, {272, 1292}, {268, 280}, {272, 1284}, {276, 280}, {268, 1292},
        {268, 288}, {272, 268}, {276, 1292}, {272, 1292}, {268, 276}, {272, 1292}, {272, 284}, {264, 272},
        {272, 1300}, {272, 268}, {268, 1296}, {276, 268}, {272, 1300}, {268, 268}, {272, 1300}, {272, 1288},
        {268, 292}, {268, 1288}, {272, 272}, {276, 268}, {272, 1300}, {272, 268}, {276, 1296}, {272, 1284},
        {276, 276}, {272, 1288}, {272, 276}, {272, 272}, {268, 1300}, {272, 272}, {272, 1296}, {268, 1292},
        {268, 296}, {272, 1284}, {272, 284}, {268, 268}, {272, 1296}, {280, 1284}, {268, 280}, {272, 272},
        {272, 1292}, {264, 10004},
    },
};
static const uint16_t NEXA_CLOSED[][66][2] = {
    {
        {272, 2676}, {272, 272}, {272, 1296}, {272, 1288}, {272, 276}, {272, 268}, {276, 1296}, {276, 1284},
        {272, 272}, {280, 1284}, {276, 272}, {276, 1284}, {276, 276}, {276, 1288}, {272, 276}, {272, 1288},
        {272, 284}, {276, 1288}, {268, 276}, {280, 1280}, {276, 276}, {272, 1288}, {276, 276}, {272, 268},
        {272, 1296}, {276, 264}, {276, 1296}, {272, 1288}, {276, 272}, {276, 1284}, {276, 276}, {272, 1288},
        {276, 280}, {276, 268}, {272, 1296}, {272, 1288}, {272, 276}, {280, 1284}, {272, 276}, {272, 268},
        {276, 1296}, {272, 268}, {276, 1292}, {280, 264}, {272, 1296}, {272, 264}, {280, 1292}, {276, 1284},
        {276, 284}, {272, 1288}, {272, 280}, {272, 264}, {276, 1292}, {276, 264}, {280, 1292}, {276, 268},
        {272, 1296}, {276, 1284}, {272, 276}, {280, 264}, {276, 1292}, {276, 268}, {272, 1296}, {268, 1296},
        {268, 268}, {268, 10004},
    },
    {
        {272, 2680}, {272, 268}, {272, 1300}, {272, 1284}, {276, 276}, {272, 272}, {276, 1292}, {272, 1284},
        {276, 276}, {272, 1288}, {272, 276}, {280, 1280}, {276, 276}, {276, 1284}, {276, 280}, {268, 1284},
        {276, 284}, {276, 1280}, {276, 280}, {272, 1288}, {272, 276}, {272, 1288}, {272, 280}, {272, 268},
        {276, 1292}, {276, 264}, {272, 1296}, {276, 1284}, {276, 276}, {276, 1284}, {276, 276}, {272, 1288},
        {272, 284}, {276, 264}, {276, 1296}, {272, 1288}, {272, 276}, {280, 1288}, {264, 280}, {272, 268},
        {272, 1292}, {276, 268}, {280, 1288}, {276, 268}, {276, 1292}, {276, 264}, {276, 1296}, {268, 1292},
        {272, 284}, {272, 1284}, {276, 276}, {276, 264}, {276, 1292}, {276, 268}, {272, 1296}, {272, 268},
        {276, 1292}, {276, 1284}, {276, 272}, {280, 264}, {276, 1296}, {276, 264}, {276, 1292}, {276, 1284},
        {276, 268}, {272, 10004},
    },
    {
        {280, 2676}, {272, 268}, {276, 1292}, {276, 1284}, {276, 276}, {272, 268}, {272, 1296}, {276, 1284},
        {276, 276}, {272, 1288}, {272, 276}, {276, 1288}, {268, 284}, {272, 1284}, {272, 280}, {272, 1288},
        {276, 276}, {276, 1288}, {272, 276}, {276, 1284}, {276, 276}, {276, 1284}, {276, 272}, {276, 268},
        {272, 1300}, {272, 268}, {268, 1300}, {280, 1280}, {272, 276}, {276, 1288}, {272, 272}, {280, 1284},
        {276, 280}, {272, 268}, {276, 1296}, {272, 1288}, {276, 272}, {276, 1284}, {276, 276}, {276, 264},
        {272, 1300}, {272, 268}, {272, 1296}, {276, 268}, {272, 1296}, {276, 264}, {276, 1292}, {272, 1292},
        {272, 288}, {272, 1284}, {272, 276}, {276, 264}, {276, 1292}, {276, 264}, {280, 1292}, {276, 268},
        {272, 1296}, {276, 1280}, {280, 272}, {280, 260}, {280, 1292}, {272, 268}, {276, 1292}, {276, 1288},
        {272, 264}, {272, 10004},
    },
};

static void s_recorded(stream_t *s, const uint16_t (*pairs)[2], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_push(s, true, pairs[i][0]);
        s_push(s, false, pairs[i][1]);
    }
}

/* One recorded open frame with its bit pairs cut to (or padded with its own last pairs to)
 * 'phys' physical bits, then its end mark and silence. */
static void s_open_cut(stream_t *s, unsigned phys)
{
    const uint16_t(*f)[2] = NEXA_OPEN[0];
    s_recorded(s, f, 1u); /* the sync */
    for (unsigned i = 0; i < phys; i++) {
        const unsigned at = i < 72u ? 1u + i : 71u + (i - 72u) % 2u;
        s_recorded(s, &f[at], 1u);
    }
    s_recorded(s, &f[NEXA_OPEN_PAIRS - 1u], 1u);
}

static void test_nexa_lmst606_recorded_open_and_closed(void)
{
    const rf_packet_t closed = rf_packet_make(RF_PROTO_ARCTECH, NEXA_DEVICE, 0u, 0u, 0u);
    s_clear(&A);
    s_quiet(&A, 30000u);
    for (size_t f = 0; f < 3u; f++) {
        s_recorded(&A, NEXA_OPEN[f], NEXA_OPEN_PAIRS);
    }
    run(&A);
    TEST_ASSERT_EQUAL_UINT(3u, GOT.n);
    for (size_t i = 0; i < GOT.n; i++) {
        TEST_ASSERT_EQUAL_UINT8(RF_PROTO_ARCTECH, GOT.p[i].proto);
        TEST_ASSERT_EQUAL_HEX32(NEXA_DEVICE, GOT.p[i].device); /* the closed report's address */
        TEST_ASSERT_EQUAL_UINT8(1u, GOT.p[i].data);            /* on: opened */
        TEST_ASSERT_EQUAL_UINT8(36u, GOT.p[i].bits);
        TEST_ASSERT_EQUAL_HEX32(0xAu, GOT.p[i].extra);         /* rtl_433's dim_value 10 */
        TEST_ASSERT_EQUAL_HEX32(closed.code | 0x10u, GOT.p[i].code);
        TEST_ASSERT_FALSE(GOT.p[i].flags & RF_PKT_GROUP);
    }
    s_clear(&A);
    s_quiet(&A, 30000u);
    for (size_t f = 0; f < 3u; f++) {
        s_recorded(&A, NEXA_CLOSED[f], NEXA_CLOSED_PAIRS);
    }
    run(&A);
    TEST_ASSERT_EQUAL_UINT(3u, GOT.n);
    TEST_ASSERT_EQUAL_UINT(3u, count_of(&closed));
    TEST_ASSERT_EQUAL_UINT8(32u, GOT.p[0].bits);
    TEST_ASSERT_EQUAL_HEX32(closed.code, GOT.p[0].code);

    /* The encoder sends the long form too (the mock LMST-606 of zone_hub's rf_devices suite). */
    rf_packet_t open = rf_packet_make(RF_PROTO_ARCTECH, NEXA_DEVICE, 1u, 0u, 0u);
    open.bits = 36u;
    open.extra = 0xAu;
    s_clear(&A);
    s_quiet(&A, 30000u);
    s_burst(&A, &open, 270u, 4u);
    run(&A);
    TEST_ASSERT_TRUE(count_of(&open) >= 3u);
    assert_only(&open, 1u, "the 36-bit form, encoded");

    /* Strict framing still: 32 or 36 bits exactly, every pair Manchester. A frame cut short or
     * run long by a pair or more is no frame - 64 physical bits being the 32-bit form, a frame
     * of its own. */
    static const unsigned BAD[] = {62u, 63u, 65u, 66u, 68u, 70u, 71u, 73u, 74u, 76u};
    char msg[64];
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        s_clear(&A);
        s_quiet(&A, 30000u);
        s_open_cut(&A, BAD[i]);
        run(&A);
        snprintf(msg, sizeof(msg), "%u physical bits", BAD[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, GOT.n, msg);
    }
    s_clear(&A);
    s_quiet(&A, 30000u);
    s_open_cut(&A, 72u);
    run(&A);
    TEST_ASSERT_EQUAL_UINT(1u, GOT.n);
    /* a broken Manchester pair among the 4 extra bits: logical bit 32 is physical bits 64 (long)
     * and 65 (short); the stream is the quiet, the sync's mark and space, then bit i's mark at
     * 3 + 2i and its space at 4 + 2i */
    s_clear(&A);
    s_quiet(&A, 30000u);
    s_open_cut(&A, 72u);
    const size_t sp64 = 4u + 2u * 64u;
    TEST_ASSERT_TRUE(!A.p[sp64].high && A.p[sp64].width_us > 1000u);
    A.p[sp64 + 2u].width_us = A.p[sp64].width_us; /* bit 65 long too */
    run(&A);
    TEST_ASSERT_EQUAL_UINT(0u, GOT.n);
}

/* RF-018: addresses many transmitters share from the factory. */
static void test_common_factory_addresses(void)
{
    TEST_ASSERT_TRUE(rf_address_is_common(RF_PROTO_EV1527, 0x00000u));
    TEST_ASSERT_TRUE(rf_address_is_common(RF_PROTO_EV1527, 0xFFFFFu));
    TEST_ASSERT_TRUE(rf_address_is_common(RF_PROTO_EV1527, 0x55555u)); /* PT2262, pins open */
    TEST_ASSERT_FALSE(rf_address_is_common(RF_PROTO_EV1527, 0xA2B3Cu));
    TEST_ASSERT_TRUE(rf_address_is_common(RF_PROTO_HT12E, 0xFFu));
    TEST_ASSERT_FALSE(rf_address_is_common(RF_PROTO_HT12E, 0x5Au));
    TEST_ASSERT_TRUE(rf_address_is_common(RF_PROTO_HT6P20B, 0u));
    TEST_ASSERT_FALSE(rf_address_is_common(RF_PROTO_KEELOQ, 0x0ABCDEFu));
    TEST_ASSERT_FALSE(rf_address_is_common(99u, 0u));
}

static void test_the_encoder_needs_room(void)
{
    rf_pulse_t p[FRAME];
    TEST_ASSERT_EQUAL_UINT(0u, rf_ev1527_encode(CODE, UNIT, p, FRAME - 1u));
    TEST_ASSERT_EQUAL_UINT(0u, rf_ev1527_encode(CODE, 0u, p, FRAME));
    TEST_ASSERT_EQUAL_UINT(FRAME, rf_ev1527_encode(CODE, UNIT, p, FRAME));
    const rf_packet_t k = sample(RF_PROTO_KEELOQ, 1u);
    TEST_ASSERT_EQUAL_UINT(0u, rf_encode(&k, 400u, p, FRAME)); /* 66 bits need more */
    const rf_packet_t none = rf_packet_make(77u, 1u, 1u, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, rf_encode(&none, 400u, p, FRAME));
}

/* ---- recorded captures (RF-002): the .txt files in test/rf/ ---------------------------------------- */

#ifdef RF_FIXTURE_DIR
/* One capture: "# expect: EV1527 A2B3C 5" lines (protocol, device and data in hex; or
 * "# expect: nothing") and rows of H350 L1050 ... as `rf capture` and the Flasher write them,
 * where H0 / L0 is the line going quiet (the capture's gap). Every expected code must decode at
 * least twice (a transmission, RF-014), and no other code twice. */
static void check_capture(const char *path)
{
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    rf_packet_t expect[8];
    size_t n_expect = 0;
    bool nothing = false;
    s_clear(&A);
    static char line[65536];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "# expect:", 9) == 0) {
            char name[16] = "";
            unsigned long dev = 0;
            unsigned data = 0;
            if (sscanf(line + 9, " %15s %lx %x", name, &dev, &data) == 3 && n_expect < 8u) {
                for (uint8_t p = 1u; p < RF_PROTO_COUNT; p++) {
                    if (strcmp(name, rf_proto_name(p)) == 0) {
                        expect[n_expect++] = rf_packet_make(p, (uint32_t)dev, (uint8_t)data, 0u, 0u);
                    }
                }
            } else if (strstr(line + 9, "nothing") != NULL) {
                nothing = true;
            }
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        for (char *tok = strtok(line, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n")) {
            if ((tok[0] == 'H' || tok[0] == 'L') && tok[1] >= '0' && tok[1] <= '9') {
                const unsigned long w = strtoul(tok + 1, NULL, 10);
                if (w == 0u) {
                    s_gap(&A);
                } else {
                    s_push(&A, tok[0] == 'H', (uint32_t)w);
                }
            }
        }
    }
    fclose(f);
    TEST_ASSERT_TRUE_MESSAGE(nothing || n_expect > 0u, "a capture without an # expect: line");
    TEST_ASSERT_TRUE_MESSAGE(A.n > 0u, "a capture without pulses");
    run(&A);
    for (size_t i = 0; i < n_expect; i++) {
        TEST_ASSERT_TRUE_MESSAGE(count_of(&expect[i]) >= 2u, path);
    }
    for (size_t i = 0; i < GOT.n; i++) {
        bool ok = false;
        for (size_t j = 0; j < n_expect; j++) {
            ok = ok || same(&GOT.p[i], &expect[j]);
        }
        TEST_ASSERT_TRUE_MESSAGE(ok || count_of(&GOT.p[i]) < 2u, path);
    }
}

/* Every .txt file in test/rf/ (RF_FIXTURE_FILES, comma-separated, listed by CMake when it
 * configures: a new capture needs a re-configure). */
static void test_recorded_captures(void)
{
    static const char all[] = RF_FIXTURE_FILES;
    unsigned n = 0;
    for (const char *name = all; *name != '\0';) {
        const char *end = strchr(name, ',');
        const size_t len = end != NULL ? (size_t)(end - name) : strlen(name);
        if (len > 0u) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%.*s", RF_FIXTURE_DIR, (int)len, name);
            check_capture(path);
            n++;
        }
        name += len + (end != NULL ? 1u : 0u);
    }
    TEST_ASSERT_TRUE_MESSAGE(n > 0u, "no captures in test/rf/");
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_frame_decodes);
    RUN_TEST(test_the_address_and_the_data_split);
    RUN_TEST(test_the_protocol_table_is_stable);
    RUN_TEST(test_every_protocol_round_trips);
    RUN_TEST(test_repeats_decode_one_by_one);
    RUN_TEST(test_a_sync_after_the_code_word_pt2262);
    RUN_TEST(test_pt2262_shaped_codes_are_recognised);
    RUN_TEST(test_every_transmitter_speed_decodes);
    RUN_TEST(test_every_protocol_at_its_slowest_fits_a_reception);
    RUN_TEST(test_a_transmitter_too_fast_or_too_slow_is_no_sync);
    RUN_TEST(test_a_drifting_oscillator_decodes);
    RUN_TEST(test_edges_moved_by_the_slicer_still_decode);
    RUN_TEST(test_every_protocol_survives_the_slicer);
    RUN_TEST(test_stretched_and_shrunk_pulses_still_decode);
    RUN_TEST(test_edges_moved_too_far_lose_frames_not_codes);
    RUN_TEST(test_jittered_edges);
    RUN_TEST(test_the_first_frame_lost_to_the_agc);
    RUN_TEST(test_an_inverted_line_decodes_nothing);
    RUN_TEST(test_noise_before_the_sync_is_ignored);
    RUN_TEST(test_a_glitch_inside_a_pulse_is_healed);
    RUN_TEST(test_spikes_in_the_sync_low_are_healed);
    RUN_TEST(test_glitches_anywhere_are_healed);
    RUN_TEST(test_long_glitches_lose_frames_not_codes);
    RUN_TEST(test_a_lost_pulse_pair_never_shifts_into_another_code);
    RUN_TEST(test_lost_pairs_never_make_another_code_in_any_protocol);
    RUN_TEST(test_an_extra_pair_breaks_its_frame);
    RUN_TEST(test_a_truncated_frame_is_rejected_and_the_next_one_decodes);
    RUN_TEST(test_the_last_frame_of_a_burst_ends_on_the_silence);
    RUN_TEST(test_a_flipped_bit_is_another_code_once);
    RUN_TEST(test_colliding_transmitters_never_mix_into_a_third_code);
    RUN_TEST(test_two_aligned_transmitters_can_make_a_third_code);
    RUN_TEST(test_a_collision_where_one_finishes_first);
    RUN_TEST(test_protocols_do_not_read_each_other);
    RUN_TEST(test_a_protocol_can_be_turned_off);
    RUN_TEST(test_foreign_signals_never_decode_twice);
    RUN_TEST(test_static_never_makes_a_transmission);
    RUN_TEST(test_garbage_never_decodes_and_never_breaks);
    RUN_TEST(test_a_carrier_decodes_nothing);
    RUN_TEST(test_keeloq_fields);
    RUN_TEST(test_keeloq_without_a_button_is_refused);
    RUN_TEST(test_ht6p20b_needs_its_anti_code);
    RUN_TEST(test_arctech_fields_and_manchester);
    RUN_TEST(test_nexa_lmst606_recorded_open_and_closed);
    RUN_TEST(test_common_factory_addresses);
    RUN_TEST(test_the_encoder_needs_room);
#ifdef RF_FIXTURE_DIR
    RUN_TEST(test_recorded_captures);
#endif
    return UNITY_END();
}
