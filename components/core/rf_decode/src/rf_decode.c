/*
 * rf_decode.c - the pulse decoders (see rf_decode.h): the de-glitch filter, the pair engine every
 * protocol shares, and each protocol's frame rules.
 *
 * Why pairs: every protocol here sends each bit as a mark and a space of a few units T, and a
 * frame as a sync pair (one long part) then a fixed number of bit pairs. A real receiver moves
 * the edges - its slicer and AGC stretch the marks and shrink the spaces (or the other way round)
 * by roughly the same microseconds - so the sum of a pair stays what the transmitter sent while
 * its parts do not. The unit comes from the whole sync pair; each bit pair is the template it is
 * nearest to, by a clear margin, or no bit at all.
 */
#include "rf_decode/rf_decode.h"

#include <string.h>

enum { HUNT = 0, BITS = 1 };

/* A protocol's frame, in tenths of its unit T. */
typedef struct rf_pair_proto rf_pair_proto_t;
typedef bool (*finish_fn)(const rf_pair_proto_t *p, const rf_proto_state_t *s, rf_packet_t *out);

struct rf_pair_proto {
    rf_proto_info_t info;
    uint8_t pairs;          /* bit pairs in a frame */
    uint8_t pairs_long;     /* the length of a second form the protocol also sends, 0 = none
                             * (arctech: 72, the LMST-606's open report) */
    bool mark_low;          /* the marks are low pulses (the inverted Holtek protocols) */
    uint16_t t_min_us;      /* the unit's range */
    uint16_t t_max_us;
    uint16_t sync_mark;     /* the sync pair, tenths of T */
    uint16_t sync_space;
    uint16_t zero_mark;     /* a '0' pair */
    uint16_t zero_space;
    uint16_t one_mark;      /* a '1' pair */
    uint16_t one_space;
    uint16_t guard;         /* the silence every frame ends with (tenths of T), 0 = none: the
                             * last bit's mark then stands alone, whatever its length */
    finish_fn finish;
};

/* ---- bits ----------------------------------------------------------------------------------- */

static bool get_bit(const rf_proto_state_t *s, unsigned i)
{
    return ((unsigned)s->bits[i / 8u] >> (7u - i % 8u) & 1u) != 0u;
}

static uint16_t clamp16(uint32_t v)
{
    return v > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)v;
}

static void widen(uint16_t *lo, uint16_t *hi, uint32_t v)
{
    const uint16_t w = clamp16(v);
    if (w < *lo) {
        *lo = w;
    }
    if (w > *hi) {
        *hi = w;
    }
}

/* A bit, with the widths that made it (space 0: its space was the silence after the frame). */
static void put_bit(rf_proto_state_t *s, bool one, uint32_t mark, uint32_t space)
{
    const unsigned i = s->nbits;
    if (i >= sizeof(s->bits) * 8u) {
        return;
    }
    const uint8_t mask = (uint8_t)(0x80u >> (i % 8u));
    if (one) {
        s->bits[i / 8u] = (uint8_t)(s->bits[i / 8u] | mask);
    } else {
        s->bits[i / 8u] = (uint8_t)(s->bits[i / 8u] & (uint8_t)~mask);
    }
    s->nbits++;
    const unsigned k = one ? 1u : 0u;
    widen(&s->mark_min[k], &s->mark_max[k], mark);
    if (space != 0u) {
        widen(&s->space_min[k], &s->space_max[k], space);
        widen(&s->len_min[k], &s->len_max[k], mark + space);
    }
}

/* One transmitter's frame through one receiver: every mark, space and pair length of a kind
 * within max(0.8 T, 160 us) of each other (pair lengths: max(0.6 T, 150 us)) - several times
 * the jitter of a real slicer, far less than what a collision or a fit of noise leaves. */
static bool consistent(const rf_proto_state_t *s)
{
    const uint32_t unit = s->unit_x16 / 16u;
    const uint32_t part = unit * 8u / 10u > 160u ? unit * 8u / 10u : 160u;
    const uint32_t len = unit * 6u / 10u > 150u ? unit * 6u / 10u : 150u;
    for (unsigned k = 0; k < 2u; k++) {
        if ((s->mark_max[k] >= s->mark_min[k] && (uint32_t)(s->mark_max[k] - s->mark_min[k]) > part) ||
            (s->space_max[k] >= s->space_min[k] && (uint32_t)(s->space_max[k] - s->space_min[k]) > part) ||
            (s->len_max[k] >= s->len_min[k] && (uint32_t)(s->len_max[k] - s->len_min[k]) > len)) {
            return false;
        }
    }
    return true;
}

/* Bits [from, from + n) as a number, the first one most significant (n <= 32). */
static uint32_t msb_first(const rf_proto_state_t *s, unsigned from, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        v = (v << 1) | (get_bit(s, from + i) ? 1u : 0u);
    }
    return v;
}

/* Bits [from, from + n) as a number, the first one least significant (n <= 32). */
static uint32_t lsb_first(const rf_proto_state_t *s, unsigned from, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        v |= (get_bit(s, from + i) ? 1u : 0u) << i;
    }
    return v;
}

/* ---- the protocols' frame rules ------------------------------------------------------------ */

/* Tri-state symbols are the bit pairs 00 ('0'), 11 ('1') and 01 ('F'); 10 never occurs. */
static bool pt2262_shape(uint32_t code, unsigned bits)
{
    for (unsigned i = 0; i < bits; i += 2u) {
        if (((code >> (bits - 2u - i)) & 3u) == 2u) {
            return false;
        }
    }
    return true;
}

static bool finish_ev1527(const rf_pair_proto_t *p, const rf_proto_state_t *s, rf_packet_t *out)
{
    (void)p;
    const uint32_t code = msb_first(s, 0u, RF_EV1527_BITS);
    out->code = code;
    out->device = rf_ev1527_address(code);
    out->data = rf_ev1527_data(code);
    out->bits = RF_EV1527_BITS;
    if (pt2262_shape(code, RF_EV1527_BITS)) {
        out->flags = (uint8_t)(out->flags | RF_PKT_PT2262);
    }
    return true;
}

/* HT6P20B: A0..A21, D0, D1, then the anti-code 0101 the chip always ends with. */
static bool finish_ht6p20b(const rf_pair_proto_t *p, const rf_proto_state_t *s, rf_packet_t *out)
{
    (void)p;
    if (msb_first(s, 24u, 4u) != 0x5u) {
        return false;
    }
    out->code = msb_first(s, 0u, 28u);
    out->device = msb_first(s, 0u, 22u);
    out->data = (uint8_t)msb_first(s, 22u, 2u);
    out->bits = 28u;
    return true;
}

/* HT12E: A0..A7 then D8..D11, each sent as a bit. */
static bool finish_ht12e(const rf_pair_proto_t *p, const rf_proto_state_t *s, rf_packet_t *out)
{
    (void)p;
    out->code = msb_first(s, 0u, 12u);
    out->device = msb_first(s, 0u, 8u);
    out->data = (uint8_t)msb_first(s, 8u, 4u);
    out->bits = 12u;
    return true;
}

/* KeeLoq, least significant bit first: 32 hopping-code bits, the 28-bit serial number, the four
 * button bits, VLOW, RPT. */
static bool finish_keeloq(const rf_pair_proto_t *p, const rf_proto_state_t *s, rf_packet_t *out)
{
    (void)p;
    out->extra = lsb_first(s, 0u, 32u);
    out->device = lsb_first(s, 32u, 28u);
    out->data = (uint8_t)lsb_first(s, 60u, 4u);
    out->code = out->device;
    out->bits = 66u;
    out->flags = (uint8_t)(out->flags | RF_PKT_ROLLING);
    if (get_bit(s, 64u)) {
        out->flags = (uint8_t)(out->flags | RF_PKT_BATTERY_LOW);
    }
    if (get_bit(s, 65u)) {
        out->flags = (uint8_t)(out->flags | RF_PKT_REPEAT);
    }
    /* A button pressed shows as its bit set; none at all is not something the chip sends. */
    return out->data != 0u;
}

/* arctech: each data bit is a Manchester pair of physical bits (a short-space pair and a
 * long-space pair, in one order or the other); two alike is a broken frame. Then 26 bits of
 * house code, the group bit, on/off and the 4-bit unit, first bit most significant. The long
 * form (36 bits: the Proove / Nexa LMST-606's open report, RF03) has 4 bits more after the unit,
 * rtl_433 nexa.c's dim level: they go in 'extra', so the address and the on/off stay the 32-bit
 * form's and a binding learned from one report matches the other. */
static bool finish_arctech(const rf_pair_proto_t *p, const rf_proto_state_t *s, rf_packet_t *out)
{
    (void)p;
    const unsigned n = s->nbits / 2u; /* 32 or 36: the engine takes no other length */
    uint32_t v = 0;
    uint32_t tail = 0;
    for (unsigned i = 0; i < n; i++) {
        const bool a = get_bit(s, 2u * i);
        const bool b = get_bit(s, 2u * i + 1u);
        if (a == b) {
            return false;
        }
        if (i < 32u) {
            v = (v << 1) | (a ? 1u : 0u);
        } else {
            tail = (tail << 1) | (a ? 1u : 0u);
        }
    }
    const uint32_t house = v >> 6;
    const bool group = ((v >> 5) & 1u) != 0u;
    const bool on = ((v >> 4) & 1u) != 0u;
    const uint32_t unit = v & 0xFu;
    out->code = v;
    out->device = (house << 4) | unit;
    out->data = on ? 1u : 0u;
    out->bits = (uint8_t)n;
    out->extra = tail;
    if (group) {
        out->flags = (uint8_t)(out->flags | RF_PKT_GROUP);
    }
    return true;
}

/* The table: names and widths are stored meanings (rf_proto_t), the timings are the chips' own
 * (docs/features/25 "Protocols" has the sources). Sync and bit pairs in tenths of T. */
static const rf_pair_proto_t PROTOS[] = {
    {.info = {RF_PROTO_EV1527, "EV1527", 20u, 4u, false, false},
     .pairs = 24u, .mark_low = false, .t_min_us = RF_EV1527_UNIT_MIN_US,
     .t_max_us = RF_EV1527_UNIT_MAX_US, .sync_mark = 10u, .sync_space = 310u,
     .zero_mark = 10u, .zero_space = 30u, .one_mark = 30u, .one_space = 10u,
     .finish = finish_ev1527},
    {.info = {RF_PROTO_HT6P20B, "HT6P20B", 22u, 2u, false, false},
     .pairs = 28u, .mark_low = true, .t_min_us = 200u, .t_max_us = 800u,
     .sync_mark = 230u, .sync_space = 10u,
     .zero_mark = 10u, .zero_space = 20u, .one_mark = 20u, .one_space = 10u,
     .finish = finish_ht6p20b},
    {.info = {RF_PROTO_HT12E, "HT12E", 8u, 4u, false, false},
     .pairs = 12u, .mark_low = true, .t_min_us = 150u, .t_max_us = 800u,
     .sync_mark = 360u, .sync_space = 10u,
     .zero_mark = 10u, .zero_space = 20u, .one_mark = 20u, .one_space = 10u,
     .finish = finish_ht12e},
    {.info = {RF_PROTO_KEELOQ, "KeeLoq", 28u, 4u, false, true},
     .pairs = 66u, .mark_low = false, .t_min_us = 100u, .t_max_us = 800u,
     .sync_mark = 10u, .sync_space = 100u,
     .zero_mark = 20u, .zero_space = 10u, .one_mark = 10u, .one_space = 20u,
     .guard = 390u, .finish = finish_keeloq},
    {.info = {RF_PROTO_ARCTECH, "arctech", 30u, 1u, false, false},
     .pairs = 64u, .pairs_long = 72u, .mark_low = false, .t_min_us = 150u, .t_max_us = 500u,
     .sync_mark = 10u, .sync_space = 100u,
     .zero_mark = 10u, .zero_space = 10u, .one_mark = 10u, .one_space = 50u,
     .finish = finish_arctech},
};
#define N_PROTOS (sizeof(PROTOS) / sizeof(PROTOS[0]))
_Static_assert(N_PROTOS + 1u == RF_PROTO_COUNT, "every rf_proto_t has a row in PROTOS");
_Static_assert(RF_DECODE_MAX_OUT >= N_PROTOS, "one call can complete one frame per protocol");

static const rf_pair_proto_t *proto_of(uint8_t proto)
{
    for (size_t i = 0; i < N_PROTOS; i++) {
        if (PROTOS[i].info.proto == proto) {
            return &PROTOS[i];
        }
    }
    return NULL;
}

const rf_proto_info_t *rf_proto_info(uint8_t proto)
{
    const rf_pair_proto_t *p = proto_of(proto);
    return p != NULL ? &p->info : NULL;
}

const char *rf_proto_name(uint8_t proto)
{
    const rf_proto_info_t *i = rf_proto_info(proto);
    return i != NULL ? i->name : "RF";
}

/* ---- the pair engine --------------------------------------------------------------------------- */

/* Is n bit pairs a whole frame of the protocol: its length, or its second form's? */
static bool whole(const rf_pair_proto_t *p, unsigned n)
{
    return n == p->pairs || (p->pairs_long != 0u && n == p->pairs_long);
}

/* The most bit pairs a frame of the protocol has. */
static unsigned most_pairs(const rf_pair_proto_t *p)
{
    return p->pairs_long > p->pairs ? p->pairs_long : p->pairs;
}

/* A width in tenths of the frame's unit. */
static uint32_t in_tenths(uint32_t us, uint32_t unit_x16)
{
    const uint64_t v = (uint64_t)us * 160u / (unit_x16 != 0u ? unit_x16 : 1u);
    return v > 0xFFFFu ? 0xFFFFu : (uint32_t)v;
}

static uint32_t absdiff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

/* Is (mark, space) this protocol's sync? *unit_x16 gets the unit it implies. The unit comes from
 * the whole pair; its long part must be within 30 % of the template, its short part within
 * 0.4x..2.5x (a receiver moves the short part by the most, relative to its length). */
static bool is_sync(const rf_pair_proto_t *p, uint32_t m, uint32_t s, uint32_t *unit_x16)
{
    const uint32_t total = (uint32_t)p->sync_mark + p->sync_space;
    const uint64_t u = ((uint64_t)m + s) * 160u / total;
    if (u < (uint64_t)p->t_min_us * 16u || u > (uint64_t)p->t_max_us * 16u) {
        return false;
    }
    const uint32_t mt = in_tenths(m, (uint32_t)u);
    const uint32_t st = in_tenths(s, (uint32_t)u);
    const bool long_space = p->sync_space > p->sync_mark;
    const uint32_t lv = long_space ? st : mt;
    const uint32_t lt = long_space ? p->sync_space : p->sync_mark;
    const uint32_t sv = long_space ? mt : st;
    const uint32_t stt = long_space ? p->sync_mark : p->sync_space;
    if (absdiff(lv, lt) * 10u > lt * 3u) {
        return false;
    }
    if (sv * 10u < stt * 4u || sv * 10u > stt * 25u) {
        return false;
    }
    if (unit_x16 != NULL) {
        *unit_x16 = (uint32_t)u;
    }
    return true;
}

/* The bit a (mark, space) pair is, or -1 for none: the template it is nearest to (the sum of
 * the distances of its two parts, in tenths of T), within three quarters of that template's
 * length, nearer by at least a quarter of the templates' distance from each other, and as long
 * as that template within 30 %. The length is the check that matters most: a receiver moves the
 * edge between mark and space, not the pair's ends, so a real bit keeps its length while
 * another protocol's pulses read in this one's unit do not (EV1527 read as HT12E, KeeLoq read
 * as arctech). */
static int bit_of(const rf_pair_proto_t *p, uint32_t unit_x16, uint32_t m, uint32_t s)
{
    const uint32_t mt = in_tenths(m, unit_x16);
    const uint32_t st = in_tenths(s, unit_x16);
    const uint32_t d0 = absdiff(mt, p->zero_mark) + absdiff(st, p->zero_space);
    const uint32_t d1 = absdiff(mt, p->one_mark) + absdiff(st, p->one_space);
    const uint32_t apart = absdiff(p->zero_mark, p->one_mark) + absdiff(p->zero_space, p->one_space);
    const bool one = d1 < d0;
    const uint32_t best = one ? d1 : d0;
    const uint32_t other = one ? d0 : d1;
    const uint32_t len = one ? (uint32_t)p->one_mark + p->one_space
                             : (uint32_t)p->zero_mark + p->zero_space;
    if (best * 4u > len * 3u || (other - best) * 4u < apart) {
        return -1;
    }
    if (absdiff(mt + st, len) * 10u > len * 3u) {
        return -1;
    }
    return one ? 1 : 0;
}

/* The bit a mark alone is, when its space is the silence after the frame, or -1: only when the
 * templates' marks differ. Unless every frame of the protocol ends in its own guard silence
 * (KeeLoq), a mark as short as the sync's own is refused: it may be the next sync's mark (a
 * PT2262 sends its sync after the code word), and taking it for the last bit of a frame that
 * lost a pair would shift that frame into another code. */
static int bit_of_mark(const rf_pair_proto_t *p, uint32_t unit_x16, uint32_t m)
{
    if (p->zero_mark == p->one_mark) {
        return -1;
    }
    const uint32_t mt = in_tenths(m, unit_x16);
    const uint32_t d0 = absdiff(mt, p->zero_mark);
    const uint32_t d1 = absdiff(mt, p->one_mark);
    const bool one = d1 < d0;
    const uint32_t best = one ? d1 : d0;
    const uint32_t other = one ? d0 : d1;
    const uint32_t apart = absdiff(p->zero_mark, p->one_mark);
    const uint32_t want = one ? p->one_mark : p->zero_mark;
    if (best * 10u > want * 6u + 5u || (other - best) * 4u < apart) {
        return -1;
    }
    if (p->guard == 0u && p->sync_mark < p->sync_space && absdiff(want, p->sync_mark) * 2u < apart) {
        return -1; /* as short as the sync's mark: ambiguous */
    }
    return one ? 1 : 0;
}

/* A sync inside a frame belongs to the same transmitter: its long part, measured in the frame's
 * unit, is the template's (0.7x..1.6x). Longer is the silence after the frame (KeeLoq's guard,
 * the end of a burst); shorter is one of the frame's own long pairs (arctech's 1T+5T), whatever
 * unit the pair alone would imply. */
static bool sync_in_frame(const rf_pair_proto_t *p, const rf_proto_state_t *s, uint32_t m, uint32_t sp)
{
    const bool long_space = p->sync_space > p->sync_mark;
    const uint32_t lv = in_tenths(long_space ? sp : m, s->unit_x16);
    const uint32_t lt = long_space ? p->sync_space : p->sync_mark;
    return lv * 10u >= lt * 7u && lv * 10u <= lt * 16u;
}

/* Is this space so long that the transmission stopped after the mark before it? */
static bool space_ends(const rf_pair_proto_t *p, uint32_t unit_x16, uint32_t s)
{
    const uint32_t longest = p->zero_space > p->one_space ? p->zero_space : p->one_space;
    return in_tenths(s, unit_x16) > longest * 2u + 10u;
}

typedef struct {
    rf_packet_t *out;
    size_t n;
    rf_decoder_t *d;
} sink_t;

static void emit(sink_t *k, const rf_pair_proto_t *p, rf_proto_state_t *s)
{
    rf_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.proto = p->info.proto;
    pkt.unit_us = (uint16_t)((s->unit_x16 + 8u) / 16u);
    if (p->info.checked) {
        pkt.flags = RF_PKT_CHECKED;
    }
    if (!consistent(s) || !p->finish(p, s, &pkt)) {
        s->rejected++;
        k->d->rejected++;
        return;
    }
    s->frames++;
    k->d->frames++;
    if (k->n < RF_DECODE_MAX_OUT) {
        k->out[k->n++] = pkt;
    }
}

/* A frame that began with a sync broke off. Counted only when it had got somewhere: the band's
 * noise starts a "sync" of some protocol all the time. */
static void broke(sink_t *k, const rf_pair_proto_t *p, rf_proto_state_t *s)
{
    if (s->phase == BITS && s->nbits >= 8u && s->nbits * 2u >= p->pairs / 2u) {
        s->rejected++;
        k->d->rejected++;
    }
}

static void start(rf_proto_state_t *s, uint32_t unit_x16)
{
    s->phase = BITS;
    s->unit_x16 = unit_x16;
    s->nbits = 0;
    memset(s->bits, 0, sizeof(s->bits));
    for (unsigned k = 0; k < 2u; k++) {
        s->mark_min[k] = s->space_min[k] = s->len_min[k] = 0xFFFFu;
        s->mark_max[k] = s->space_max[k] = s->len_max[k] = 0u;
    }
}

/* A whole pair: a bit of the frame, the pair after its last bit, or the next sync. */
static void on_pair(sink_t *k, const rf_pair_proto_t *p, rf_proto_state_t *s, uint32_t m, uint32_t sp)
{
    uint32_t unit = 0;
    bool sync = is_sync(p, m, sp, &unit);
    if (s->phase == BITS) {
        sync = sync && sync_in_frame(p, s, m, sp);
        const int bit = sync ? -1 : bit_of(p, s->unit_x16, m, sp);
        if (bit >= 0 && s->nbits < most_pairs(p)) {
            put_bit(s, bit == 1, m, sp);
            return;
        }
        if (bit < 0 && whole(p, s->nbits)) {
            emit(k, p, s); /* the pair after the last bit: the next sync, the silence, noise */
        } else if (bit < 0 && !sync && whole(p, s->nbits + 1u) && space_ends(p, s->unit_x16, sp)) {
            const int last = bit_of_mark(p, s->unit_x16, m); /* its space is the silence */
            if (last >= 0) {
                put_bit(s, last == 1, m, 0u);
                emit(k, p, s);
            } else {
                broke(k, p, s);
            }
        } else {
            broke(k, p, s); /* a bit too many, too few, or something that is no bit */
        }
        s->phase = HUNT;
    }
    if (sync) {
        start(s, unit);
    }
}

/* The line went quiet: a frame that only lacked its last space ends here, or its last mark was
 * the next sync's. */
static void on_silence(sink_t *k, const rf_pair_proto_t *p, rf_proto_state_t *s)
{
    if (s->phase == BITS) {
        if (s->have_mark && whole(p, s->nbits + 1u)) {
            const int last = bit_of_mark(p, s->unit_x16, s->mark_us);
            if (last >= 0) {
                put_bit(s, last == 1, s->mark_us, 0u);
                emit(k, p, s);
            } else {
                broke(k, p, s);
            }
        } else if (whole(p, s->nbits)) {
            emit(k, p, s);
        } else {
            broke(k, p, s);
        }
    }
    s->phase = HUNT;
    s->have_mark = false;
}

/* One whole pulse (healed) to one protocol. */
static void on_pulse(sink_t *k, const rf_pair_proto_t *p, rf_proto_state_t *s, bool high, uint32_t us)
{
    const bool mark = high != p->mark_low;
    if (mark) {
        if (s->have_mark) {
            /* Two marks in a row: the filter merged a whole space away. */
            broke(k, p, s);
            s->phase = HUNT;
        }
        s->have_mark = true;
        s->mark_us = us;
        return;
    }
    if (!s->have_mark) {
        return; /* a space with no mark before it: the line has just been switched on */
    }
    s->have_mark = false;
    on_pair(k, p, s, s->mark_us, us);
}

static void to_decoders(rf_decoder_t *d, sink_t *k, bool high, uint32_t us)
{
    for (size_t i = 0; i < N_PROTOS; i++) {
        const uint8_t proto = PROTOS[i].info.proto;
        if ((d->enabled & (1u << proto)) != 0u) {
            on_pulse(k, &PROTOS[i], &d->proto[proto], high, us);
        }
    }
}

/* ---- the decoder ------------------------------------------------------------------------------ */

void rf_decoder_init(rf_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    for (size_t i = 0; i < N_PROTOS; i++) {
        d->enabled |= 1u << PROTOS[i].info.proto;
    }
}

void rf_decoder_enable(rf_decoder_t *d, uint32_t mask)
{
    d->enabled = mask;
}

size_t rf_decoder_pulse(rf_decoder_t *d, bool high, uint32_t width_us, rf_packet_t *out)
{
    d->pulses++;
    if (width_us == 0u) {
        return rf_decoder_gap(d, out);
    }
    sink_t k = {out, 0u, d};
    if (width_us < RF_GLITCH_US) {
        /* Noise cut into the pulse being held (or a spike in a gap): it and the pulse after it
         * are that pulse. A spike with nothing before it is dropped. */
        d->glitches++;
        if (d->held) {
            d->held_us += width_us;
            d->absorb = true;
        }
        return 0u;
    }
    if (d->held && (high == d->held_high || d->absorb)) {
        d->held_us += width_us; /* the other side of a glitch, or an edge the capture lost */
        d->absorb = false;
        return 0u;
    }
    if (d->held) {
        to_decoders(d, &k, d->held_high, d->held_us);
    }
    d->held = true;
    d->held_high = high;
    d->held_us = width_us;
    d->absorb = false;
    return k.n;
}

size_t rf_decoder_gap(rf_decoder_t *d, rf_packet_t *out)
{
    sink_t k = {out, 0u, d};
    if (d->held) {
        to_decoders(d, &k, d->held_high, d->held_us);
        d->held = false;
        d->absorb = false;
    }
    for (size_t i = 0; i < N_PROTOS; i++) {
        const uint8_t proto = PROTOS[i].info.proto;
        if ((d->enabled & (1u << proto)) != 0u) {
            on_silence(&k, &PROTOS[i], &d->proto[proto]);
        }
    }
    return k.n;
}

/* ---- addresses ---------------------------------------------------------------------------------- */

bool rf_address_is_common(uint8_t proto, uint32_t device)
{
    const rf_proto_info_t *i = rf_proto_info(proto);
    if (i == NULL || i->device_bits == 0u || i->device_bits > 32u) {
        return false;
    }
    const uint32_t all = i->device_bits == 32u ? 0xFFFFFFFFu : (1u << i->device_bits) - 1u;
    device &= all;
    if (device == 0u || device == all) {
        return true;
    }
    if (proto == RF_PROTO_EV1527) {
        /* A PT2262 with every address pin open: each tri-state symbol 'F' is the pair 01. */
        return device == (0x55555555u & all);
    }
    return false;
}

/* ---- the encoders ------------------------------------------------------------------------------- */

typedef struct {
    rf_pulse_t *out;
    size_t max;
    size_t n;
    bool full;
} tx_t;

static void tx(tx_t *t, bool high, uint32_t us)
{
    if (us == 0u) {
        return;
    }
    if (t->n > 0u && t->out[t->n - 1u].high == high) {
        t->out[t->n - 1u].width_us += us; /* the same level again: one longer pulse */
        return;
    }
    if (t->n >= t->max) {
        t->full = true;
        return;
    }
    t->out[t->n].high = high;
    t->out[t->n].width_us = us;
    t->n++;
}

/* A pair of the protocol: its mark then its space, tenths of T. */
static void tx_pair(tx_t *t, const rf_pair_proto_t *p, uint16_t unit, uint16_t mark_x10,
                    uint16_t space_x10)
{
    tx(t, !p->mark_low, (uint32_t)unit * mark_x10 / 10u);
    tx(t, p->mark_low, (uint32_t)unit * space_x10 / 10u);
}

static void tx_bit(tx_t *t, const rf_pair_proto_t *p, uint16_t unit, bool one)
{
    tx_pair(t, p, unit, one ? p->one_mark : p->zero_mark, one ? p->one_space : p->zero_space);
}

static void tx_msb(tx_t *t, const rf_pair_proto_t *p, uint16_t unit, uint32_t v, unsigned n)
{
    for (unsigned i = n; i-- > 0u;) {
        tx_bit(t, p, unit, ((v >> i) & 1u) != 0u);
    }
}

static void tx_lsb(tx_t *t, const rf_pair_proto_t *p, uint16_t unit, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        tx_bit(t, p, unit, ((v >> i) & 1u) != 0u);
    }
}

rf_packet_t rf_packet_make(uint8_t proto, uint32_t device, uint8_t data, uint8_t flags,
                           uint32_t extra)
{
    rf_packet_t p;
    memset(&p, 0, sizeof(p));
    p.proto = proto;
    const rf_proto_info_t *i = rf_proto_info(proto);
    if (i == NULL) {
        return p;
    }
    const uint32_t dmask = i->device_bits >= 32u ? 0xFFFFFFFFu : (1u << i->device_bits) - 1u;
    p.device = device & dmask;
    p.data = (uint8_t)(data & ((1u << i->data_bits) - 1u));
    p.flags = flags;
    switch (proto) {
    case RF_PROTO_EV1527:
        p.code = (p.device << 4) | p.data;
        p.bits = RF_EV1527_BITS;
        if (pt2262_shape(p.code, RF_EV1527_BITS)) {
            p.flags = (uint8_t)(p.flags | RF_PKT_PT2262);
        }
        break;
    case RF_PROTO_HT6P20B:
        p.code = (p.device << 6) | ((uint32_t)p.data << 4) | 0x5u;
        p.bits = 28u;
        break;
    case RF_PROTO_HT12E:
        p.code = (p.device << 4) | p.data;
        p.bits = 12u;
        break;
    case RF_PROTO_KEELOQ:
        p.extra = extra;
        p.code = p.device;
        p.bits = 66u;
        p.flags = (uint8_t)(p.flags | RF_PKT_ROLLING);
        break;
    case RF_PROTO_ARCTECH:
        p.code = ((p.device >> 4) << 6) | ((flags & RF_PKT_GROUP) != 0u ? 0x20u : 0u) |
                 ((uint32_t)(p.data & 1u) << 4) | (p.device & 0xFu);
        p.bits = 32u;
        break;
    default:
        break;
    }
    if (i->checked) {
        p.flags = (uint8_t)(p.flags | RF_PKT_CHECKED);
    }
    return p;
}

static void tx_frame(tx_t *t, const rf_pair_proto_t *p, const rf_packet_t *pk, uint16_t unit,
                     bool last)
{
    switch (p->info.proto) {
    case RF_PROTO_EV1527:
        tx_pair(t, p, unit, p->sync_mark, p->sync_space);
        tx_msb(t, p, unit, pk->code, RF_EV1527_BITS);
        break;
    case RF_PROTO_HT6P20B:
        tx_pair(t, p, unit, p->sync_mark, p->sync_space);
        tx_msb(t, p, unit, (pk->device << 6) | ((uint32_t)pk->data << 4) | 0x5u, 28u);
        break;
    case RF_PROTO_HT12E:
        tx_pair(t, p, unit, p->sync_mark, p->sync_space);
        tx_msb(t, p, unit, (pk->device << 4) | pk->data, 12u);
        break;
    case RF_PROTO_KEELOQ: {
        for (unsigned i = 0; i < 11u; i++) { /* the preamble: 23 pulses of 1 TE */
            tx_pair(t, p, unit, 10u, 10u);
        }
        tx_pair(t, p, unit, p->sync_mark, p->sync_space); /* the last one, then the header */
        tx_lsb(t, p, unit, pk->extra, 32u);
        tx_lsb(t, p, unit, pk->device, 28u);
        tx_lsb(t, p, unit, pk->data, 4u);
        tx_bit(t, p, unit, (pk->flags & RF_PKT_BATTERY_LOW) != 0u);
        tx_bit(t, p, unit, (pk->flags & RF_PKT_REPEAT) != 0u);
        if (!last) {
            tx(t, false, (uint32_t)unit * 39u); /* the guard time before the next preamble */
        }
        break;
    }
    case RF_PROTO_ARCTECH: {
        tx_pair(t, p, unit, p->sync_mark, p->sync_space);
        /* bits 36: the long form, the 4 bits of 'extra' after the 32 of the code */
        const bool wide = pk->bits == 36u;
        const uint64_t v = wide ? ((uint64_t)pk->code << 4) | (pk->extra & 0xFu) : pk->code;
        for (unsigned i = wide ? 36u : 32u; i-- > 0u;) {
            const bool one = ((v >> i) & 1u) != 0u;
            tx_bit(t, p, unit, one);  /* a '1' is long-space then short-space */
            tx_bit(t, p, unit, !one);
        }
        tx(t, true, unit); /* the end: 1T high, then about 40T of low */
        if (!last) {
            tx(t, false, (uint32_t)unit * 40u);
        }
        break;
    }
    default:
        t->full = true;
        break;
    }
}

size_t rf_encode(const rf_packet_t *p, uint16_t unit_us, rf_pulse_t *out, size_t max)
{
    return rf_encode_burst(p, unit_us, 1u, out, max);
}

size_t rf_encode_burst(const rf_packet_t *p, uint16_t unit_us, unsigned frames, rf_pulse_t *out,
                       size_t max)
{
    const rf_pair_proto_t *proto = proto_of(p->proto);
    if (proto == NULL || unit_us == 0u || frames == 0u) {
        return 0u;
    }
    tx_t t = {out, max, 0u, false};
    for (unsigned f = 0; f < frames; f++) {
        tx_frame(&t, proto, p, unit_us, f + 1u == frames);
    }
    return t.full ? 0u : t.n;
}

size_t rf_ev1527_encode(uint32_t code, uint16_t unit_us, rf_pulse_t *out, size_t max)
{
    const rf_packet_t p = rf_packet_make(RF_PROTO_EV1527, (code >> 4) & 0xFFFFFu,
                                         (uint8_t)(code & 0xFu), 0u, 0u);
    if (max < 2u + 2u * RF_EV1527_BITS) {
        return 0u;
    }
    return rf_encode(&p, unit_us, out, max);
}
