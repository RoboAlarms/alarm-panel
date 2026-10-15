/*
 * rf_mock.c - mock 433 MHz devices and the air between them and the panel (rf_mock.h).
 */
#include "rf_mock.h"

#include <string.h>

/* ---- the profiles ------------------------------------------------------------------------------ */

#define N RFM_NONE
#define DOOR RFM_DOOR_STATE
/* codes, in rfm_event_t order: open, close, motion, alarm, tamper, tamper back, battery, test,
 * buttons 1-4, heartbeat */
#define CODES(o, c, m, al, t, tb, b, te, b1, b2, b3, b4, hb) {o, c, m, al, t, tb, b, te, b1, b2, b3, b4, hb}
/* An EV1527-family PWM frame: sync (mark, space), zero (mark, space), one (mark, space). */
#define PWM24(sm, ss, zm, zs, om, os) \
    .shape = RFM_PWM, .proto = RF_PROTO_EV1527, .bits = 24u, .addr_bits = 20u, .sync_mark = (sm), \
    .sync_space = (ss), .zero_mark = (zm), .zero_space = (zs), .one_mark = (om), .one_space = (os)

const rfm_profile_t RFM_PROFILES[] = {
    /* ---- door and window contacts ---- */
    {.id = "ev1527_contact_69", .name = "An EV1527 door contact that sends 6 open and 9 closed",
     .source = "our bench 2026-09-25 (docs/sensors, B012): EV1527, T 420 us, about 36 frames an event, "
               "6 open, 9 closed, and the cover re-sends the door's code; its listing: low voltage 2.2-2.4 V",
     .assumed = "its low-battery code (never seen)",
     PWM24(420u, 13020u, 420u, 1260u, 1260u, 420u), .frames = 36u, .sag_pct = 100u,
     .code = CODES(0x6, 0x9, N, N, DOOR, DOOR, N, N, N, N, N, N, N)},
    {.id = "ev1527_contact_e7", .name = "An EV1527 door contact that sends E open and 7 closed",
     .source = "rtl_433's decoder for it: open E, close 7, tamper B, battery F (below 5 V of its 12 V cell); "
               "pulses 304-560 / 860-1016 us (short 420, long 960), older sync mark 480 us, 25 frames",
     .assumed = "the sync space (rtl_433 only bounds it under 9.9 ms); the battery code after each event",
     PWM24(480u, 9300u, 420u, 960u, 960u, 420u), .frames = 25u, .sag_pct = 131u,
     .code = CODES(0xE, 0x7, N, N, 0xB, N, 0xF, N, N, N, N, N, N)},
    {.id = "ev1527_contact_ae", .name = "An EV1527 door contact that sends A open and E closed",
     .source = "ioBroker forum and Home Assistant community (Sonoff RF Bridge / Tasmota): open A, "
               "close E, battery 6, tamper 7",
     .assumed = "the timings (rc-switch protocol 1: T 350 us) and 8 frames an event",
     PWM24(350u, 10850u, 350u, 1050u, 1050u, 350u), .frames = 8u, .sag_pct = 100u,
     .code = CODES(0xA, 0xE, N, N, 0x7, N, 0x6, N, N, N, N, N, N)},
    {.id = "ev1527_open_only_t280", .name = "An EV1527 door contact that sends A open and nothing on closing",
     .source = "an rtl_433 flex config for it: pulses 280 / 990 us (3.5 : 1), "
               "no closed event",
     .assumed = "its data nibble (A), the sync (31 T) and 8 frames",
     PWM24(280u, 8680u, 280u, 990u, 990u, 280u), .frames = 8u, .sag_pct = 100u,
     .code = CODES(0xA, N, N, N, N, N, N, N, N, N, N, N, N)},
    {.id = "ev1527_contact_7c", .name = "An EV1527 door contact that sends 7 open and C closed",
     .source = "rtl_433's decoder for it: pulses 568 / 1704 us, 17.2 ms between messages; 7 (normal "
               "zone) on opening, C on closing (newer units), D low battery, 3 tamper",
     .assumed = "the frame count (8)",
     PWM24(568u, 17200u, 568u, 1704u, 1704u, 568u), .frames = 8u, .sag_pct = 100u,
     .code = CODES(0x7, 0xC, N, N, 0x3, N, 0xD, N, N, N, N, N, N)},
    {.id = "ev1527_12v_slow", .name = "An EV1527 contact at T 500 us on a 12 V cell",
     .source = "rc-switch protocol 5 (T 500 us); docs/features/25: an EV1527's oscillator is about "
               "31 % slower at 5 V of 12 V (a low A23 cell)",
     .assumed = "a product with exactly these numbers (a contact at T 568 us is one like it)",
     PWM24(500u, 15500u, 500u, 1500u, 1500u, 500u), .frames = 20u, .sag_pct = 131u,
     .code = CODES(0x6, 0x9, N, N, N, N, 0x3, N, N, N, N, N, N)},
    {.id = "ev1527_t740", .name = "A slow EV1527 contact (T about 740 us)",
     .source = "a Tasmota RF Bridge capture of a door contact: Sync 22858, Low 754, High 2368 us "
               "(T = 22858 / 31 = 737 us; its sync is longer than the old 20 ms idle, B025)",
     .assumed = "which product it is; open A and closed E; 8 frames; no sag (beyond 800 us it "
                "would be out of the decoder's range)",
     PWM24(740u, 22940u, 740u, 2220u, 2220u, 740u), .frames = 8u, .sag_pct = 100u,
     .code = CODES(0xA, 0xE, N, N, N, N, N, N, N, N, N, N, N)},
    {.id = "arctech_contact", .name = "An arctech door contact (on when opened, off when closed)",
     .source = "rtl_433's arctech decoders (on when opened, off when closed; T 270 us; "
               "every message repeated about 5-15 times, 10 ms apart); an rtl_433_tests "
               "capture of a door contact: the open report is 36 bits (72 physical), its last "
               "4 bits 10, the closed one 32 (RF03)",
     .assumed = "5 frames, the fewest rtl_433 gives",
     .shape = RFM_ENCODER, .proto = RF_PROTO_ARCTECH, .unit_us = 270u, .long_open = true,
     .open_tail = 0xAu, .frames = 5u, .sag_pct = 100u,
     .code = CODES(0x1, 0x0, N, N, N, N, N, N, N, N, N, N, N)},
    {.id = "ht6p20b_contact", .name = "A Holtek HT6P20B door contact (South American alarms)",
     .source = "Holtek HT6P20B datasheet; rc-switch protocol 6",
     .assumed = "T 400 us, data 1 open and 2 closed, 6 frames",
     .shape = RFM_ENCODER, .proto = RF_PROTO_HT6P20B, .unit_us = 400u, .frames = 6u, .sag_pct = 100u,
     .code = CODES(0x1, 0x2, N, N, N, N, N, N, N, N, N, N, N)},

    /* ---- motion ---- */
    {.id = "ev1527_pir_t330", .name = "An EV1527 mini PIR that sends A on motion (T 330 us)",
     .source = "rtl_433's decoder for it: motion A; the newer timing (sync mark 340 us, pulses 304-560 / "
               "860-1016 us)",
     .assumed = "pulses 330 / 900 us, the sync space, 25 frames, and a 5 s sleep after a trigger",
     PWM24(340u, 10230u, 330u, 900u, 900u, 330u), .frames = 25u, .lockout_ms = 5000u, .sag_pct = 100u,
     .code = CODES(N, N, 0xA, N, N, N, N, N, N, N, N, N, N)},
    {.id = "sonoff_pir3_rf", .name = "Sonoff PIR3-RF (normal mode)",
     .source = "Sonoff: in normal mode it triggers again only after a minute without motion "
               "(alert mode: every 5 s)",
     .assumed = "its code (A) and timings (rc-switch protocol 1), 8 frames",
     PWM24(350u, 10850u, 350u, 1050u, 1050u, 350u), .frames = 8u, .lockout_ms = 60000u,
     .lockout_quiet = true, .sag_pct = 100u,
     .code = CODES(N, N, 0xA, N, N, N, N, N, N, N, N, N, N)},
    {.id = "ev1527_pir_t400", .name = "An EV1527 PIR that sends A on motion (T 400 us)",
     .source = "an rtl_433 flex config for it: pulses 400 / 1200 us, at least 4 repeats",
     .assumed = "its code (A), the sync (31 T), 6 frames and a 5 s sleep",
     PWM24(400u, 12400u, 400u, 1200u, 1200u, 400u), .frames = 6u, .lockout_ms = 5000u, .sag_pct = 100u,
     .code = CODES(N, N, 0xA, N, N, N, N, N, N, N, N, N, N)},
    {.id = "ev1527_motion_t188", .name = "An EV1527 driveway motion sensor (T 188 us)",
     .source = "an rtl_433 flex config for it: pulses 188 / 596 us, sync and 24 bits, "
               "sent over and over while it sees motion (at least 10 repeats)",
     .assumed = "its code (8), the sync space (31 T) and 12 frames",
     PWM24(188u, 5828u, 188u, 596u, 596u, 188u), .frames = 12u, .sag_pct = 100u,
     .code = CODES(N, N, 0x8, N, N, N, N, N, N, N, N, N, N)},

    /* ---- smoke and water ---- */
    {.id = "ev1527_smoke_6", .name = "An EV1527 smoke alarm that sends 6 on test and on smoke",
     .source = "rogerfrost.com, Tasmota RF Bridge: Sync 12460, Low 450, High 1240 us, code 94B6C6 "
               "on its test button; tech.scargill.net: a unique code on test and on smoke, nothing "
               "in normal operation",
     .assumed = "10 frames, and the alarm re-sent every 10 s while it sounds",
     PWM24(450u, 12460u, 450u, 1240u, 1240u, 450u), .frames = 10u, .repeat_ms = 10000u, .sag_pct = 100u,
     .code = CODES(N, N, N, 0x6, N, N, N, 0x6, N, N, N, N, N)},
    {.id = "smoke_wakeup_3", .name = "A smoke alarm with a wake-up pulse that sends 3 on test and on smoke",
     .source = "rtl_433's decoder for it: a 436 us wake-up pulse and 11.76 ms gap, 24 bits (0: 436 + "
               "1299 us, 1: 1202 + 526 us), 2 stop pulses, 8 times; the last nibble is always 3",
     .assumed = "the alarm re-sent every 10 s while it sounds",
     PWM24(436u, 11764u, 436u, 1299u, 1202u, 526u), .trailer = 2u, .trail_us = 434u, .frames = 8u,
     .repeat_ms = 10000u, .sag_pct = 100u,
     .code = CODES(N, N, N, 0x3, N, N, N, 0x3, N, N, N, N, N)},
    {.id = "ev1527_water_5", .name = "An EV1527 water leak sensor that sends 5 while wet",
     .source = "rtl_433's decoder for it: water 5, the newer timing",
     .assumed = "pulses 330 / 900 us, 25 frames, re-sent every 10 s while wet",
     PWM24(340u, 10230u, 330u, 900u, 900u, 330u), .frames = 25u, .repeat_ms = 10000u, .sag_pct = 100u,
     .code = CODES(N, N, N, 0x5, N, N, N, N, N, N, N, N, N)},
    {.id = "ev1527_water_8", .name = "An EV1527 water leak sensor that sends 8 (T 500 us)",
     .source = "an rtl_433 flex config for it: pulses 500 / 1500 us, a 2048 us bit, "
               "one fixed code repeated while it alarms",
     .assumed = "its data nibble (8), the sync (31 T), 20 frames, re-sent every 10 s while wet",
     PWM24(500u, 15500u, 500u, 1548u, 1500u, 548u), .frames = 20u, .repeat_ms = 10000u, .sag_pct = 100u,
     .code = CODES(N, N, N, 0x8, N, N, N, N, N, N, N, N, N)},

    /* ---- remotes and key fobs ---- */
    {.id = "ev1527_remote4", .name = "EV1527 4-button remote",
     .source = "rtl_433 conf/EV1527-4Button-Universal-Remote.conf: pulses 369 / 1072 us, buttons "
               "1, 2, 4, 8 (each button a data pin), at least 3 repeats",
     .assumed = "the sync space (31 T) and 6 frames a press",
     PWM24(369u, 11439u, 369u, 1072u, 1072u, 369u), .frames = 6u, .sag_pct = 100u,
     .code = CODES(N, N, N, N, N, N, N, N, 0x1, 0x2, 0x4, 0x8, N)},
    {.id = "sonoff_rm433", .name = "Sonoff RM433 remote",
     .source = "rtl_433 conf/sonoff_rm433.conf: pulses 260 / 744 us, buttons A-H 8, 12, 4, 9, 2, "
               "5, 1, 3",
     .assumed = "the sync space (31 T) and 6 frames a press",
     PWM24(260u, 8060u, 260u, 744u, 744u, 260u), .frames = 6u, .sag_pct = 100u,
     .code = CODES(N, N, N, N, N, N, N, N, 0x8, 0xC, 0x4, 0x9, N)},
    {.id = "ev1527_alarm_remote", .name = "An EV1527 alarm remote (arm 8, disarm 1, home 4, alarm 2)",
     .source = "rtl_433's decoder for it: arm 8, disarm 1, home 4, alarm 2",
     .assumed = "the frame count (8)",
     PWM24(568u, 17200u, 568u, 1704u, 1704u, 568u), .frames = 8u, .sag_pct = 100u,
     .code = CODES(N, N, N, 0x2, N, N, N, N, 0x8, 0x1, 0x4, 0x2, N)},
    {.id = "hcs301_fob", .name = "Microchip HCS301 KeeLoq key fob",
     .source = "HCS301 datasheet DS21143 (a new hopping code each press, VLOW when the cell is "
               "low); rtl_433 hcs200.c",
     .assumed = "TE 400 us, buttons 1, 2, 4, 8, and 4 code words a press",
     .shape = RFM_ENCODER, .proto = RF_PROTO_KEELOQ, .unit_us = 400u, .frames = 4u, .sag_pct = 100u,
     .code = CODES(N, N, N, N, N, N, N, N, 0x1, 0x2, 0x4, 0x8, N)},

    /* ---- neighbours: nothing of these may decode ---- */
    {.id = "nexus_th", .name = "Nexus-style temperature sensor (a neighbour's weather station)",
     .source = "rtl_433 nexus.c: PPM, 500 us pulses, 1000 / 2000 us gaps, a 4 ms sync gap, 36 bits "
               "12 times",
     .assumed = "a reading every 58 s",
     .shape = RFM_PPM, .bits = 36u, .addr_bits = 32u, .sync_mark = 500u, .sync_space = 4000u,
     .zero_mark = 500u, .zero_space = 1000u, .one_mark = 500u, .one_space = 2000u, .frames = 12u,
     .heartbeat_ms = 58000u, .sag_pct = 100u,
     .code = CODES(N, N, N, N, N, N, N, N, N, N, N, N, 0x7)},
    {.id = "lacrosse_tx141th", .name = "LaCrosse TX141TH-Bv2 (a neighbour's weather station)",
     .source = "rtl_433 lacrosse_tx141x.c: 4 x (833, 833 us) preamble, 40 bits of 208 / 417 us in "
               "a 625 us period, 12 packets back to back",
     .assumed = "a reading every 50 s",
     .shape = RFM_PWM, .bits = 40u, .addr_bits = 32u, .preamble = 4u, .pre_us = 833u,
     .zero_mark = 208u, .zero_space = 417u, .one_mark = 417u, .one_space = 208u, .frames = 12u,
     .heartbeat_ms = 50000u, .sag_pct = 100u,
     .code = CODES(N, N, N, N, N, N, N, N, N, N, N, N, 0x3)},
    {.id = "ppm41_other_contact", .name = "A contact of another alarm system (433 MHz, PPM, 41 bits)",
     .source = "rtl_433's decoder for it: a 9 ms / 4.5 ms sync, 562 us pulses, 1687 / 562 us gaps, 41 "
               "bits, 5 times 40 ms apart",
     .assumed = "",
     .shape = RFM_PPM, .bits = 41u, .addr_bits = 33u, .sync_mark = 9000u, .sync_space = 4500u,
     .zero_mark = 562u, .zero_space = 1687u, .one_mark = 562u, .one_space = 562u, .frames = 5u,
     .frame_gap_us = 40000u, .sag_pct = 100u,
     .code = CODES(0x4, 0x84, N, N, N, N, N, N, N, N, N, N, N)},
    {.id = "pwm37_other_contact", .name = "A contact of another alarm system (PWM, 37 bits with a checksum)",
     .source = "rtl_433's decoder for it: PWM 400 / 800 us, 37 bits with an XOR checksum, 6 "
               "times",
     .assumed = "5 ms between the repeats",
     .shape = RFM_PWM, .bits = 37u, .addr_bits = 25u, .zero_mark = 400u, .zero_space = 800u,
     .one_mark = 800u, .one_space = 400u, .frames = 6u, .frame_gap_us = 5000u, .sag_pct = 100u,
     .code = CODES(0xC1, 0x81, N, N, N, N, N, N, N, N, N, N, N)},
};
const size_t RFM_PROFILE_COUNT = sizeof(RFM_PROFILES) / sizeof(RFM_PROFILES[0]);

#undef N
#undef DOOR
#undef CODES
#undef PWM24

const rfm_profile_t *rfm_profile(const char *id)
{
    for (size_t i = 0; i < RFM_PROFILE_COUNT; i++) {
        if (strcmp(RFM_PROFILES[i].id, id) == 0) {
            return &RFM_PROFILES[i];
        }
    }
    return NULL;
}

/* ---- the air ------------------------------------------------------------------------------------- */

static uint32_t rnd(rfm_air_t *a)
{
    a->seed = a->seed * 1103515245u + 12345u;
    return (a->seed >> 8) & 0xFFFFFFu;
}

static int32_t rnd_pm(rfm_air_t *a, uint32_t range)
{
    if (range == 0u) {
        return 0;
    }
    return (int32_t)(rnd(a) % (2u * range + 1u)) - (int32_t)range;
}

void rfm_air_init(rfm_air_t *a, const rfm_channel_t *ch, rfm_deliver_fn deliver, void *ctx,
                  uint64_t now_us)
{
    memset(a, 0, sizeof(*a));
    if (ch != NULL) {
        a->ch = *ch;
    }
    a->seed = 0x5EEDu;
    a->quiet = true;
    a->last_edge = a->played_us = now_us; /* the band is empty from here */
    a->deliver = deliver;
    a->ctx = ctx;
    rf_decoder_init(&a->dec);
}

static void add_high(rfm_air_t *a, uint64_t start, uint64_t end)
{
    if (end <= start || a->n >= RFM_MAX_HIGHS) {
        return;
    }
    a->highs[a->n].start_us = start;
    a->highs[a->n].end_us = end;
    a->n++;
}

uint64_t rfm_air_pulses(rfm_air_t *a, const rf_pulse_t *p, size_t n, uint64_t at_us)
{
    uint64_t t = at_us;
    for (size_t i = 0; i < n; i++) {
        if (p[i].high) {
            add_high(a, t, t + p[i].width_us);
        }
        t += p[i].width_us;
    }
    return t;
}

static void sort_highs(rfm_high_t *h, size_t n)
{
    for (size_t i = 1; i < n; i++) { /* insertion sort: queued nearly in order */
        const rfm_high_t v = h[i];
        size_t j = i;
        while (j > 0u && h[j - 1u].start_us > v.start_us) {
            h[j] = h[j - 1u];
            j--;
        }
        h[j] = v;
    }
}

/* OR of everything on the air: overlapping marks become one. */
static size_t merge(rfm_high_t *h, size_t n)
{
    if (n == 0u) {
        return 0u;
    }
    size_t w = 0;
    for (size_t i = 1; i < n; i++) {
        if (h[i].start_us <= h[w].end_us) {
            if (h[i].end_us > h[w].end_us) {
                h[w].end_us = h[i].end_us;
            }
        } else {
            h[++w] = h[i];
        }
    }
    return w + 1u;
}

static void hand_out(rfm_air_t *a, size_t got, const rf_packet_t *pk, uint64_t now)
{
    for (size_t i = 0; i < got; i++) {
        a->packets++;
        if (a->deliver != NULL) {
            a->deliver(a->ctx, &pk[i], now);
        }
    }
}

static void give_gap(rfm_air_t *a, uint64_t now)
{
    rf_packet_t pk[RF_DECODE_MAX_OUT];
    hand_out(a, rf_decoder_gap(&a->dec, pk), pk, now);
    a->quiet = true;
}

static void feed(rfm_air_t *a, bool high, uint64_t width, uint64_t now)
{
    rf_packet_t pk[RF_DECODE_MAX_OUT];
    hand_out(a, rf_decoder_pulse(&a->dec, high, (uint32_t)width, pk), pk, now);
}

/* Static on a quiet band: random short marks between `from` and `to`, in order (a superhet's AGC
 * turned up). The caller starts it only where the band has been empty for a reception's idle:
 * inside a burst the gaps (a sync's 31 T, at most ~25 ms) are far shorter than the AGC takes to
 * turn up again (assumed; the RXB6's AGC settles in ~0.17 s). */
static size_t add_static(rfm_air_t *a, rfm_high_t *out, size_t n, size_t max, uint64_t from, uint64_t to)
{
    if (a->ch.static_per_s == 0u || to <= from + 4000u) {
        return n;
    }
    const uint64_t span = to - from - 2000u;
    const uint64_t count = span * a->ch.static_per_s / 2u / 1000000u; /* a mark is two edges */
    const uint64_t slot = count != 0u ? span / count : 0u;
    for (uint64_t i = 0; i < count && slot > 80u && n < max; i++) {
        const uint64_t at = from + 1000u + i * slot + rnd(a) % (slot / 2u);
        out[n].start_us = at;
        out[n].end_us = at + 40u + rnd(a) % (slot / 2u); /* within its slot: no two merge */
        n++;
    }
    return n;
}

static uint64_t later(uint64_t x, uint64_t y)
{
    return x > y ? x : y;
}

void rfm_air_play(rfm_air_t *a, uint64_t until_us)
{
    static rfm_high_t now_on[RFM_MAX_HIGHS];
    static rfm_high_t heard[RFM_MAX_HIGHS + 8192u];
    const size_t room = sizeof(heard) / sizeof(heard[0]);
    size_t take = 0;
    size_t keep = 0;
    sort_highs(a->highs, a->n);
    for (size_t i = 0; i < a->n; i++) {
        if (a->highs[i].start_us < until_us) {
            now_on[take++] = a->highs[i];
        } else {
            a->highs[keep++] = a->highs[i];
        }
    }
    a->n = keep;
    take = merge(now_on, take);

    /* The receiver, in time order: static where the band has been empty; the AGC loses the first
     * part of a burst after a quiet line; the slicer moves the edge between a mark and its space;
     * every edge jitters. */
    size_t m = 0;
    uint64_t from = a->last_edge;
    for (size_t i = 0; i < take; i++) {
        const rfm_high_t h = now_on[i];
        m = add_static(a, heard, m, room - (take - i), later(from, a->on_air_end + RFM_IDLE_US),
                       h.start_us); /* static never takes the room of a mark */
        if (a->on_air_end == 0u || h.start_us >= a->on_air_end + RFM_IDLE_US) {
            a->burst_start = h.start_us;
        }
        a->on_air_end = later(a->on_air_end, h.end_us);
        if (a->ch.agc_us != 0u && h.start_us < a->burst_start + a->ch.agc_us) {
            continue;
        }
        int64_t st = (int64_t)h.start_us + rnd_pm(a, a->ch.jitter_us);
        int64_t e = (int64_t)h.end_us + a->ch.shift_us + rnd_pm(a, a->ch.jitter_us);
        if (e <= st) {
            e = st + 1;
        }
        if (m < room) {
            heard[m].start_us = (uint64_t)st;
            heard[m].end_us = (uint64_t)e;
            m++;
        }
        from = (uint64_t)e;
    }
    m = add_static(a, heard, m, room, later(from, a->on_air_end + RFM_IDLE_US), until_us);
    sort_highs(heard, m); /* in order but for jitter: cheap */
    m = merge(heard, m);

    /* The decoder gets the pulses as the capture hands them over: a quiet line longer than the
     * idle ends the reception (a gap), and the next reception starts at its first rising edge. */
    for (size_t i = 0; i < m; i++) {
        const rfm_high_t *h = &heard[i];
        if (h->start_us < a->last_edge) {
            continue; /* moved behind what was already played: lost */
        }
        if (!a->quiet) {
            const uint64_t low = h->start_us - a->last_edge;
            if (low >= RFM_IDLE_US) {
                give_gap(a, a->last_edge + RFM_IDLE_US);
            } else {
                feed(a, false, low, h->start_us);
            }
        }
        a->quiet = false;
        feed(a, true, h->end_us - h->start_us, h->end_us);
        a->last_edge = h->end_us;
    }
    if (!a->quiet && until_us >= a->last_edge + RFM_IDLE_US) {
        give_gap(a, a->last_edge + RFM_IDLE_US);
    }
    if (until_us > a->played_us) {
        a->played_us = until_us;
    }
}

/* ---- a device ------------------------------------------------------------------------------------ */

void rfm_device_init(rfm_device_t *d, const rfm_profile_t *p, uint32_t address)
{
    memset(d, 0, sizeof(*d));
    d->p = p;
    d->address = address;
    d->hop = 0x1000u + address % 977u;
    if (p->heartbeat_ms != 0u) {
        d->next_heartbeat_us = (uint64_t)p->heartbeat_ms * 1000u;
    }
}

uint8_t rfm_code_for(const rfm_device_t *d, rfm_event_t ev)
{
    if ((unsigned)ev >= (unsigned)RFM_EVENTS) {
        return RFM_NONE;
    }
    const uint8_t c = d->p->code[ev];
    if (c == RFM_DOOR_STATE) {
        return d->open ? d->p->code[RFM_OPEN] : d->p->code[RFM_CLOSE];
    }
    return c;
}

rf_packet_t rfm_expected(const rfm_device_t *d, uint8_t data)
{
    const rfm_profile_t *p = d->p;
    const uint8_t flags = (uint8_t)(p->proto == RF_PROTO_KEELOQ && d->low ? RF_PKT_BATTERY_LOW : 0u);
    rf_packet_t pk = rf_packet_make(p->proto, d->address, data, flags, p->proto == RF_PROTO_KEELOQ ? d->hop : 0u);
    if (p->proto == RF_PROTO_ARCTECH && p->long_open && data == p->code[RFM_OPEN]) {
        pk.bits = 36u; /* RF03: the long form, as an arctech door contact sends its open report */
        pk.extra = p->open_tail & 0xFu;
    }
    return pk;
}

static uint32_t sagged(const rfm_device_t *d, uint32_t us)
{
    return d->low ? (uint32_t)((uint64_t)us * d->p->sag_pct / 100u) : us;
}

#define FRAME_MAX 400u

/* One frame of a PWM or PPM profile, the code word most significant bit first. */
static size_t frame(const rfm_device_t *d, uint64_t word, rf_pulse_t *out)
{
    const rfm_profile_t *p = d->p;
    size_t n = 0;
#define PUT(h, w) do { out[n].high = (h); out[n].width_us = sagged(d, (w)); n++; } while (0)
    for (unsigned i = 0; i < p->preamble; i++) {
        PUT(true, p->pre_us);
        PUT(false, p->pre_us);
    }
    if (p->sync_mark != 0u || p->sync_space != 0u) {
        PUT(true, p->sync_mark);
        PUT(false, p->sync_space);
    }
    for (unsigned i = 0; i < p->bits; i++) {
        const bool one = ((word >> (p->bits - 1u - i)) & 1u) != 0u;
        PUT(true, one ? p->one_mark : p->zero_mark);
        PUT(false, one ? p->one_space : p->zero_space);
    }
    for (unsigned i = 0; i < p->trailer; i++) {
        PUT(true, p->trail_us);
        PUT(false, p->trail_us);
    }
#undef PUT
    if (p->frame_gap_us != 0u && n > 0u) {
        out[n - 1u].width_us += p->frame_gap_us;
    }
    return n;
}

uint64_t rfm_burst(rfm_air_t *a, rfm_device_t *d, uint8_t data, unsigned frames, uint64_t now_us)
{
    static rf_pulse_t buf[FRAME_MAX * 40u];
    const rfm_profile_t *p = d->p;
    if (frames == 0u) {
        frames = p->frames;
    }
    size_t n = 0;
    if (p->shape == RFM_ENCODER) {
        const rf_packet_t pk = rfm_expected(d, data);
        n = rf_encode_burst(&pk, (uint16_t)sagged(d, p->unit_us), frames, buf, sizeof(buf) / sizeof(buf[0]));
        if (p->proto == RF_PROTO_KEELOQ) {
            d->hop++; /* the next press hops */
        }
    } else {
        const unsigned data_bits = (unsigned)(p->bits - p->addr_bits);
        const uint64_t word = ((uint64_t)d->address << data_bits) | (uint64_t)(data & ((1u << data_bits) - 1u));
        for (unsigned f = 0; f < frames && n + FRAME_MAX <= sizeof(buf) / sizeof(buf[0]); f++) {
            n += frame(d, word, &buf[n]);
        }
    }
    d->sent++;
    return rfm_air_pulses(a, buf, n, now_us);
}

uint64_t rfm_event(rfm_air_t *a, rfm_device_t *d, rfm_event_t ev, uint64_t now_us)
{
    const rfm_profile_t *p = d->p;
    if (ev == RFM_OPEN) {
        d->open = true;
    } else if (ev == RFM_CLOSE) {
        d->open = false;
    }
    if (ev == RFM_MOTION && p->lockout_ms != 0u) {
        if (now_us < d->asleep_until_us) {
            if (p->lockout_quiet) {
                d->asleep_until_us = now_us + (uint64_t)p->lockout_ms * 1000u;
            }
            return now_us; /* asleep: nothing */
        }
        d->asleep_until_us = now_us + (uint64_t)p->lockout_ms * 1000u;
    }
    const uint8_t c = rfm_code_for(d, ev);
    if (c == RFM_NONE) {
        return now_us;
    }
    uint64_t end = rfm_burst(a, d, c, 0u, now_us);
    const uint8_t batt = p->code[RFM_BATTERY];
    if (d->low && batt != RFM_NONE && ev != RFM_BATTERY) {
        end = rfm_burst(a, d, batt, 0u, end + 200000u); /* its battery code, a moment later */
    }
    return end;
}

uint64_t rfm_alarm(rfm_air_t *a, rfm_device_t *d, bool on, uint64_t now_us)
{
    d->alarm = on;
    if (!on) {
        return now_us;
    }
    d->next_repeat_us = now_us + (uint64_t)d->p->repeat_ms * 1000u;
    return rfm_event(a, d, RFM_ALARM, now_us);
}

void rfm_tick(rfm_air_t *a, rfm_device_t *d, uint64_t now_us)
{
    const rfm_profile_t *p = d->p;
    if (d->alarm && p->repeat_ms != 0u && now_us >= d->next_repeat_us) {
        (void)rfm_event(a, d, RFM_ALARM, d->next_repeat_us);
        d->next_repeat_us += (uint64_t)p->repeat_ms * 1000u;
    }
    if (p->heartbeat_ms != 0u && now_us >= d->next_heartbeat_us) {
        (void)rfm_event(a, d, RFM_HEARTBEAT, d->next_heartbeat_us);
        d->next_heartbeat_us += (uint64_t)p->heartbeat_ms * 1000u;
    }
}
