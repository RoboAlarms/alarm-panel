/*
 * rf_decode.h - pulse-train decoders for one-way RF sensors (docs/features/25, RF-002, RF-012..).
 *
 * The receiver (a CC1101 in asynchronous OOK mode, or a bare superheterodyne module) turns the
 * carrier into a level on one pin; the panel's RMT peripheral turns that level into pulse widths;
 * this file turns pulse widths into packets. It is a streaming decoder: feed it every pulse as it
 * comes (level and width), and it hands back a packet the moment a frame completes. It never
 * allocates, never waits, and knows nothing about zones - zsrc_rf (zone_hub) does that.
 *
 * How (RF-002, RF-012):
 *   1. A de-glitch filter heals the line first: a pulse shorter than RF_GLITCH_US is noise cut
 *      into a longer pulse (or a spike in a gap), so it is merged with its neighbours instead of
 *      being taken for an edge. Every decoder sees the healed line.
 *   2. Every protocol in the table below listens to the same pulses at once, each with its own
 *      state. A protocol's frame is a sync pair then exactly its number of bit pairs, each pair
 *      a "mark" (the pulse that starts it: high, or low for the inverted Holtek protocols) and a
 *      "space". The unit T comes from the whole sync pair (its total is many T long, so a
 *      receiver that stretches marks and shrinks spaces by the same microseconds moves it very
 *      little); each bit is the template (in T) its mark and space are nearest to, and a pair
 *      that is near neither is no bit. A frame ends at the next sync, at the silence after the
 *      transmission, or at anything that is not a bit - and only a frame with exactly the right
 *      number of bits counts: a pulse pair lost to an overrun or a merged glitch never shifts
 *      the bits into another code.
 *   3. The protocol's own rules then check the frame (the HT6P20B anti-code, the Manchester
 *      pairs of the arctech codes, KeeLoq's layout) and split it into who sent it (device) and
 *      what it says (data), with what else it carries (battery low, a rolling code).
 *
 * Protocols (433.92 MHz OOK; rf_proto_t numbers are stored in bindings through zsrc_rf, so they
 * are never renumbered, only added, and ZSRC_RF_PROTO_* repeats them):
 *   RF_PROTO_EV1527   EV1527 / HS1527 / RT1527 learning code and the PT2262 / PT2264 / SC2262 /
 *                     SC5262 tri-state family (the same frame on air): sync 1T high + 31T low,
 *                     '0' 1T high + 3T low, '1' 3T high + 1T low, 24 bits = 20-bit address + 4
 *                     data bits (open, closed, tamper, low battery... by make). No check.
 *   RF_PROTO_HT6P20B  Holtek HT6P20B / HT6P20D (the alarm sensors and remotes of South America):
 *                     inverted, pilot 23T low + 1T high, '0' 1T low + 2T high, '1' 2T low + 1T
 *                     high, 28 bits = 22-bit address + 2 data bits + the anti-code 0101.
 *   RF_PROTO_HT12E    Holtek HT12E / HT640 and the SM5212 (older fixed-code remotes and sensors):
 *                     inverted, pilot 36T low + 1T high, bits as HT6P20B, 12 bits = 8 address +
 *                     4 data bits.
 *   RF_PROTO_KEELOQ   Microchip KeeLoq HCS200 / HCS201 / HCS300 / HCS301 rolling code (alarm key
 *                     fobs, some sensors): preamble, 10 TE header, '1' 1TE high + 2TE low, '0'
 *                     2TE high + 1TE low, 66 bits sent least significant first: a 32-bit
 *                     encrypted hopping code, the 28-bit serial number, 4 button bits, the
 *                     battery-low bit and the repeat bit. The hopping code cannot be checked
 *                     without the maker's key, but it changes with every press, so the same one
 *                     twice is a replay (zsrc_rf refuses it).
 *   RF_PROTO_ARCTECH  "arctech" self-learning codes (Nexa, KlikAanKlikUit, HomeEasy, Proove,
 *                     Intertechno ITT): sync 1T high + ~10T low, each bit two pulse pairs (a
 *                     Manchester pair: 1T+1T then 1T+5T, or the other way round), 32 bits =
 *                     26-bit house code + group + on/off + 4-bit unit; door and motion sensors
 *                     send on for open/motion and off for closed.
 */
#ifndef RF_DECODE_H
#define RF_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RF_PROTO_NONE = 0,
    RF_PROTO_EV1527 = 1,  /* 24 bits: 20-bit address + 4 data bits; EV1527, PT2262 family */
    RF_PROTO_HT6P20B = 2, /* 28 bits: 22-bit address + 2 data bits + anti-code 0101 */
    RF_PROTO_HT12E = 3,   /* 12 bits: 8-bit address + 4 data bits */
    RF_PROTO_KEELOQ = 4,  /* 66 bits: 32-bit hopping code, 28-bit serial, 4 buttons, 2 status */
    RF_PROTO_ARCTECH = 5, /* 32 bits: 26-bit house, group, on/off, 4-bit unit */
    RF_PROTO_COUNT
} rf_proto_t;

#define RF_EV1527_BITS 24u

/* EV1527 timing limits (RF-002): T from the whole sync pair (32T). */
#define RF_EV1527_UNIT_MIN_US 120u
#define RF_EV1527_UNIT_MAX_US 800u

/* The receiver ends a reception when its line has been quiet this long (rf_rx's
 * RF_CAPTURE_IDLE_US is this) and the decoder gets a gap instead of the low: a space longer
 * than this can't be measured, so every protocol's longest space at its slowest T must fit. */
#define RF_RECEPTION_IDLE_US 30000u /* EV1527 at 800 us: 24.8 ms; HT12E at 800 us: 28.8 ms (B025) */

/* A pulse shorter than this is not an edge but noise cut into a longer pulse (RF-012): below
 * any honest pulse of the protocols here (their shortest is 1T >= 100 us, shrunk by at most
 * about half by a receiver), and the RMT peripheral's own filter stops at ~3 us. */
#define RF_GLITCH_US 60u

/* Packets one call can complete at most: one per protocol (in practice one; two protocols
 * finishing on the same silence would need the same pulses to be valid frames of both). */
#define RF_DECODE_MAX_OUT 8u

/* What else a packet says (rf_packet_t.flags). */
#define RF_PKT_CHECKED     0x01u /* the frame carries a checksum it passed: one frame is enough */
#define RF_PKT_BATTERY_LOW 0x02u /* the transmitter says its battery is low (KeeLoq VLOW) */
#define RF_PKT_REPEAT      0x04u /* the transmitter marks the frame a repeat (KeeLoq RPT) */
#define RF_PKT_PT2262      0x08u /* EV1527 family: every bit pair is 00, 11 or 01 (tri-state) */
#define RF_PKT_ROLLING     0x10u /* 'extra' is a rolling code: the same one twice is a replay */
#define RF_PKT_GROUP       0x20u /* arctech: a group command (every unit of the house code) */

typedef struct {
    uint8_t proto;    /* rf_proto_t */
    uint8_t bits;     /* bits in the frame (data bits, after any Manchester pairing) */
    uint8_t flags;    /* RF_PKT_* */
    uint8_t data;     /* what it says: data bits, buttons, on/off */
    uint16_t unit_us; /* the transmitter's T, measured on this frame */
    uint32_t device;  /* who sent it: address, serial number, house code and unit */
    uint32_t code;    /* the frame as one number for people (EV1527: all 24 bits) */
    uint32_t extra;   /* KeeLoq's hopping code; arctech's long form (bits 36): its 4 bits after
                       * the unit (rtl_433's dim level); 0 for the others */
} rf_packet_t;

/* What the panel needs to know about a protocol (the table the decoders use, for callers). */
typedef struct {
    uint8_t proto;
    const char *name;     /* "EV1527", for logs and the console */
    uint8_t device_bits;  /* bits in rf_packet_t.device */
    uint8_t data_bits;    /* bits in rf_packet_t.data */
    bool checked;         /* its frames carry a checksum (one frame is enough) */
    bool rolling;         /* its frames carry a rolling code */
} rf_proto_info_t;

/* The protocol's facts, NULL for a number this firmware does not decode. */
const rf_proto_info_t *rf_proto_info(uint8_t proto);
/* "EV1527"; "RF" for an unknown number. */
const char *rf_proto_name(uint8_t proto);

/* ---- the decoder ------------------------------------------------------------------------------ */

/* One protocol's state inside the decoder (see rf_decode.c). */
typedef struct {
    uint8_t phase;      /* hunting for a sync, or inside a frame */
    bool have_mark;     /* a mark waits for its space */
    uint32_t mark_us;
    uint32_t unit_x16;  /* T of the frame being decoded, in sixteenths of a microsecond */
    uint8_t nbits;
    uint8_t bits[12];   /* up to 96 bit pairs, first in the most significant bit of bits[0] */
    /* How alike the frame's pairs of each kind ('0', '1') are: one transmitter's frame is one
     * clock through one receiver, so its marks, spaces and pair lengths of a kind hardly vary;
     * a collision or noise that happens to fit the templates does (RF-013). */
    uint16_t mark_min[2], mark_max[2];
    uint16_t space_min[2], space_max[2];
    uint16_t len_min[2], len_max[2];
    uint32_t frames;    /* this protocol's packets handed back */
    uint32_t rejected;  /* its frames that began with a sync and then broke its rules */
} rf_proto_state_t;

/* One decoder per receiver. Call rf_decoder_init() first. */
typedef struct {
    /* the de-glitch filter: one pulse is held back until the next shows it is whole */
    bool held;
    bool held_high;
    uint32_t held_us;
    bool absorb;        /* a glitch was merged into the held pulse: the pulse after it joins too */
    rf_proto_state_t proto[RF_PROTO_COUNT]; /* by rf_proto_t; [0] unused */
    uint32_t enabled;   /* bit n: rf_proto_t n decodes (all by default) */
    /* counters for the bench (RF-002 diagnostics): never reset by the decoder itself */
    uint32_t pulses;    /* pulses fed in */
    uint32_t frames;    /* packets handed back, every protocol */
    uint32_t rejected;  /* frames that started with a sync and then broke the timing */
    uint32_t glitches;  /* pulses too short to be anything, merged into their neighbours */
} rf_decoder_t;

void rf_decoder_init(rf_decoder_t *d);
/* Only these protocols decode (bit n = rf_proto_t n); the others are skipped. */
void rf_decoder_enable(rf_decoder_t *d, uint32_t mask);

/* One pulse: the level and how long it lasted; width 0 is a gap (the line went quiet: the
 * capture ended). Returns how many packets completed (0..RF_DECODE_MAX_OUT), written to out. */
size_t rf_decoder_pulse(rf_decoder_t *d, bool high, uint32_t width_us, rf_packet_t *out);

/* The line went quiet for longer than any pulse (the capture ended): completes a frame that only
 * lacked its final space, drops anything else half-decoded. Same return as rf_decoder_pulse. */
size_t rf_decoder_gap(rf_decoder_t *d, rf_packet_t *out);

/* The two halves of an EV1527 code: the 20-bit address that names the transmitter and the 4 data
 * bits that name what it says. */
static inline uint32_t rf_ev1527_address(uint32_t code)
{
    return (code >> 4) & 0xFFFFFu;
}
static inline uint8_t rf_ev1527_data(uint32_t code)
{
    return (uint8_t)(code & 0xFu);
}

/* An address that many transmitters share from the factory (RF-006): all zeros, all ones, or a
 * PT2262 whose address pins were all left open (every tri-state symbol 'F'). A zone bound to
 * one may answer the neighbour's sensor. */
bool rf_address_is_common(uint8_t proto, uint32_t device);

/* ---- the encoders: what a transmitter sends, for the tests ------------------------------------ */

typedef struct {
    bool high;
    uint32_t width_us;
} rf_pulse_t;

/* One frame of the packet's protocol, from its fields (proto, device, data, flags, extra; code
 * for EV1527; arctech with bits 36: the long form, extra's 4 bits after the 32), with the sync
 * first, as pulses of the given unit. Returns the number of pulses written, 0 when 'max' is too
 * small or the packet cannot be sent. A transmitter repeats its
 * frame back to back while it transmits (see rf_encode_burst). */
size_t rf_encode(const rf_packet_t *p, uint16_t unit_us, rf_pulse_t *out, size_t max);

/* 'frames' frames of the packet back to back, then the silence after them (a pulse of width 0
 * is not written: the caller ends with rf_decoder_gap). Returns pulses written, 0 if too small. */
size_t rf_encode_burst(const rf_packet_t *p, uint16_t unit_us, unsigned frames, rf_pulse_t *out,
                       size_t max);

/* One EV1527 frame (sync then 24 bits). Returns the number of pulses written, 0 when 'max' is
 * too small (it needs 2 + 2 * 24). */
size_t rf_ev1527_encode(uint32_t code, uint16_t unit_us, rf_pulse_t *out, size_t max);

/* A packet with its fields filled as the decoder would for these values (for the tests; the PC
 * simulator hands packets straight to the 433 MHz source): device and data are masked to the
 * protocol's widths. */
rf_packet_t rf_packet_make(uint8_t proto, uint32_t device, uint8_t data, uint8_t flags,
                           uint32_t extra);

#ifdef __cplusplus
}
#endif

#endif /* RF_DECODE_H */
