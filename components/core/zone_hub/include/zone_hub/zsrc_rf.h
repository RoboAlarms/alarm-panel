/*
 * zsrc_rf.h - one-way RF sensors as zones (docs/features/25, docs/features/22 ZSRC-005/-008): the cheap
 * 433 MHz door contacts, PIRs, water and smoke detectors and remotes that speak the EV1527 /
 * PT2262 fixed code, received by a CC1101 or a fixed superheterodyne receiver module
 * (components/drivers/rf_rx, platform_esp/rf_service.c) and decoded by components/core/rf_decode.
 *
 * This half is portable and host-tested: the learn table, the bindings' codes, what a code means
 * to a zone, hold times, repeat collapsing and jamming. The radio is behind two calls the
 * platform makes with the zone service's lock held, like every other source:
 *
 *   zsrc_rf_packet()   a frame the decoder handed back;
 *   zsrc_rf_noise()    an RSSI sample taken between frames (RF-005), or
 *   zsrc_rf_carrier()  the same judgement from a receiver without RSSI.
 *
 * What a sensor is to the panel (RF-003): a *device* (its 20-bit EV1527 address) sends *codes*
 * (its 4 data bits): one for open, often another for closed, sometimes one for tamper and one
 * for a low battery - which is which differs by make, so the panel learns them by watching the
 * installer trigger the sensor. A binding names one (device, code): its address is
 *
 *   kind ZSRC_RF, id = protocol << 32 | device, input = code + 1 (1..16).
 *
 * Its zone is active from the moment that code is heard. It goes quiet again (the "restore")
 * when the device sends any *other* code that no binding of the same zone claims (the hub tells
 * the source which inputs are one zone's: zone_source_ops_t.group) - a closed code, typically,
 * or another zone's code of the same transmitter - or, for a sensor that only ever says "open" (most PIRs and many contacts),
 * after a hold: the binding's hold_s, or ZSRC_RF_AUTO_HOLD_S when that is 0. A binding with an
 * explicit hold_s ignores other codes and only ever times out (a PIR's own low-battery code
 * must not end its motion).
 *
 * Learning (RF-003, RF-009..011): a transmission counts only once its frame has arrived
 * ZSRC_RF_CONFIRM_FRAMES times - EV1527 has no checksum, so one frame alone may be a corrupted
 * copy of another, or noise, and would look like a new sensor (rc-switch accepts a code only on
 * its second reception, rtl_433's EV1527 decoders ask for three). A few transmissions are
 * followed at once, so a stray frame between a transmitter's repeats does not split its
 * transmission in two. The learn table holds *sensors*, each with the codes it sent: the picker
 * shows one row per sensor, the ones heard since learning began first, the ones that were
 * already transmitting before it after them; and once one sensor has clearly answered the
 * installer (heard ZSRC_RF_SURE_MIN times while learning, more than any other) it is marked
 * best.
 *
 * Zones act on the same rule (RF-014): a code without a checksum moves its zone only once its
 * frame has arrived twice (a PT2272 decoder waits for two code words too); one frame alone may
 * be a bit flipped by noise, or a collision - the close code of a door turned into another
 * sensor's open code. A protocol whose frames carry a checksum acts on the first. A code bound
 * while its confirmed transmission is still arriving (bound while learning) gets it at its next
 * frame, without the transmission counting twice (RF02).
 *
 * Every confirmed transmission of a bound sensor, whatever code it sends, is a check-in for its
 * zone (RF-015): a sensor that reports now and then (a periodic "alive" code, a door opened
 * daily) can be supervised with the zone's supervision time. A rolling-code transmitter's code
 * heard a second time is a replay and is refused (RF-016); its battery bit reaches the zone.
 *
 * These sensors send no heartbeat, so they are not supervised (RF-004: the source has no
 * ZSRC_CAP_SUPERVISED and the zone page says so) and they can be replayed by anyone with a
 * transmitter (no ZSRC_CAP_AUTH: not trusted for fire, CO or panic zones by default,
 * ZSRC-008). Jamming - a carrier above ZSRC_RF_JAM_DBM for ZSRC_RF_JAM_HOLD_MS of the last
 * ZSRC_RF_JAM_WINDOW_MS, however it is chopped up - takes the
 * source's link down, which the hub reports as the source's trouble after its grace (RF-005,
 * ZSRC-015). A receiver that stops answering does the same. A sensor's own transmission is not
 * a carrier: it is loud next to the panel, and a cheap contact talks for two seconds an event.
 */
#ifndef ZSRC_RF_H
#define ZSRC_RF_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocols, as stored in bindings (never renumber; the same numbers as rf_decode's rf_proto_t,
 * which zone_hub's rf suite checks). */
#define ZSRC_RF_PROTO_EV1527  1u /* 20-bit address, 4 data bits (EV1527, PT2262 family) */
#define ZSRC_RF_PROTO_HT6P20B 2u /* 22-bit address, 2 data bits */
#define ZSRC_RF_PROTO_HT12E   3u /* 8-bit address, 4 data bits */
#define ZSRC_RF_PROTO_KEELOQ  4u /* 28-bit serial, 4 button bits, rolling code, battery bit */
#define ZSRC_RF_PROTO_ARCTECH 5u /* 26-bit house code + 4-bit unit, on / off */
#define ZSRC_RF_PROTO_COUNT   6u

/* What else a frame said (zsrc_rf_packet_t.flags; the same bits as rf_decode's RF_PKT_*). */
#define ZSRC_RF_PKT_CHECKED     0x01u /* it carries a checksum it passed: one frame is enough */
#define ZSRC_RF_PKT_BATTERY_LOW 0x02u /* the transmitter says its battery is low */
#define ZSRC_RF_PKT_ROLLING     0x10u /* 'extra' is a rolling code: the same one again is a replay */

#define ZSRC_RF_WATCH_MAX    32u   /* bound (device, code) inputs */
#define ZSRC_RF_CAND_MAX     16u   /* sensors remembered for learning */
#define ZSRC_RF_RECENT       4u    /* transmissions followed at once (RF-009) */
#define ZSRC_RF_CONFIRM_FRAMES 2u  /* identical frames that make a transmission real (RF-009) */
#define ZSRC_RF_SURE_MIN     2u    /* transmissions while learning before a sensor can be best */
#define ZSRC_RF_NEARBY_S     600u  /* heard this recently before learning: listed as nearby */
#define ZSRC_RF_DEVICE_MAX   0xFFFFFu /* EV1527's 20-bit address (every protocol: zsrc_rf_device_bits) */
#define ZSRC_RF_CODES        16u   /* EV1527's 4 data bits (every protocol: zsrc_rf_codes) */
#define ZSRC_RF_HOPS         8u    /* rolling codes remembered per bound input (RF-016) */
#define ZSRC_RF_ORDER_MAX    4u    /* a sensor's codes kept in the order heard while learning */

#define ZSRC_RF_REPEAT_MS    500u   /* frames of one code inside this are one transmission */
#define ZSRC_RF_AUTO_HOLD_S  30u    /* a one-code sensor's fault ends after this (hold_s == 0) */
#define ZSRC_RF_BATTERY_HOLD_S 43200u /* a low-battery code picked at the panel: its trouble lasts
                                       * this long after the sensor last sent it (no sensor says
                                       * "battery good"; an automatic hold ended at the next
                                       * closed code) (RF-021) */
#define ZSRC_RF_CHECKIN_MS   60000u /* a bound sensor's transmissions count as a check-in this
                                     * often at most (RF-015) */
#define ZSRC_RF_JAM_DBM      (-55)  /* carrier at or above this between frames ... */
#define ZSRC_RF_TALK_MS      500u   /* a carrier sample this soon after a sensor's frame is that
                                     * sensor talking, not a jammer (RF-005) */
#define ZSRC_RF_JAM_HOLD_MS  30000u /* ... for this long in all ... */
#define ZSRC_RF_JAM_WINDOW_MS 60000u /* ... of the last minute is jamming (RF-005: the EN "30/60"
                                      * rule; short gaps in a jammer do not reset it) */
#define ZSRC_RF_JAM_CLEAR_MS 5000u  /* ... and it is over once the last minute holds less than this */
#define ZSRC_RF_JAM_SLOT_MS  5000u  /* the minute is kept as 12 slots of this */
#define ZSRC_RF_GRACE_S      60u    /* the hub's grace before a down link is the source's trouble */
#define ZSRC_RF_BURST_FRAMES 100u   /* more frames than this in one transmission is not a finger
                                     * on a button: counted for RF-006 */

typedef struct {
    uint8_t proto;    /* ZSRC_RF_PROTO_* */
    uint8_t bits;
    uint8_t flags;    /* ZSRC_RF_PKT_* */
    uint8_t data;     /* what it says: its data bits, buttons, on / off */
    uint16_t unit_us; /* the transmitter's T */
    uint32_t device;  /* who sent it */
    uint32_t code;    /* the frame as one number, for people */
    uint32_t extra;   /* a rolling code (KeeLoq); arctech's 4 extra bits (36-bit form); else 0 */
} zsrc_rf_packet_t;

/* An EV1527 frame's packet from its 24-bit code word (the tests and the simulator). */
zsrc_rf_packet_t zsrc_rf_ev1527(uint32_t code);

typedef struct {
    bool used;
    uint8_t proto;
    uint32_t device;    /* the transmitter's address */
    uint8_t data;       /* the code this input means */
    uint16_t hold_s;    /* from the binding; 0 = automatic */
    bool active;        /* what the hub was told */
    bool battery_low;   /* what the hub was told of the transmitter's battery bit */
    uint64_t heard_at;  /* last frame of this code */
    uint64_t checkin_at; /* the last check-in posted for it (RF-015), 0 = none yet */
    uint32_t frames;    /* frames of this code since bound */
    uint32_t hops[ZSRC_RF_HOPS]; /* its transmitter's last rolling codes (RF-016) */
    uint8_t nhops;
    bool closed_by_known; /* its transmitter's code that last ended it (its closed code) ... */
    uint8_t closed_by;    /* ... since start-up: no tamper or battery code can be that (RF-021) */
    uint32_t group;       /* the hub's group (one zone's inputs; 0 none): a code an input of the
                           * same group claims restores nothing (RF01) */
    uint32_t since_n;     /* zsrc_rf_t.frames when it was made: a transmission confirmed since
                           * then reached it; one confirmed before, still arriving, not yet */
} zsrc_rf_watch_t;

/* A transmission being received: one frame, repeated for as long as the button is held. */
typedef struct {
    bool used;
    uint8_t proto;
    uint32_t device;
    uint8_t data;
    uint32_t extra;
    uint64_t last_at; /* its last frame */
    uint32_t last_n;  /* its last frame's place in arrival order (zsrc_rf_t.frames) */
    uint32_t frames;
    bool acted;       /* confirmed, and told to the bound zones */
    uint32_t acted_n; /* zsrc_rf_t.frames at the frame that confirmed it */
    bool replay;      /* a rolling code heard before: ignored (RF-016) */
} zsrc_rf_tx_t;

/* A sensor heard: learning or not, so the ones already transmitting can be told apart. */
typedef struct {
    bool used;
    uint8_t proto;
    uint32_t device;
    uint8_t last_code;   /* the code it sent last */
    uint64_t heard_at;   /* its last confirmed transmission */
    /* since learning began (all zero before it) */
    uint32_t count;      /* confirmed transmissions */
    uint16_t codes;      /* bit n: code n */
    uint8_t first_code;  /* its first code: what the installer did first (opened the door) */
    uint64_t learn_at;   /* its first transmission */
    uint8_t order[ZSRC_RF_ORDER_MAX]; /* its codes in the order first heard (RF-021) */
    uint8_t order_n[ZSRC_RF_ORDER_MAX];  /* ... how often each was heard (at most 255) */
    uint32_t order_s[ZSRC_RF_ORDER_MAX]; /* ... when each was last heard, in s of the clock */
    uint8_t norder;
} zsrc_rf_cand_t;

typedef struct {
    zsrc_sink_t sink;
    bool receiver;      /* a receiver answers on the bus */
    bool jammed;
    bool told_up;       /* what the hub was last told: receiver && !jammed */
    /* carrier time in each ZSRC_RF_JAM_SLOT_MS of the last minute (RF-005) */
    uint32_t jam_ms[ZSRC_RF_JAM_WINDOW_MS / ZSRC_RF_JAM_SLOT_MS];
    uint64_t jam_slot;  /* the slot number (time / slot) jam_ms[slot % n] is counting */
    uint64_t jam_at;    /* the last carrier sample, 0 = none yet */
    bool jam_carrier;   /* ... and whether it heard a carrier */
    uint64_t talk_at;   /* the last frame of a transmission no longer than a burst, 0 = none */
    bool learning;
    uint64_t learn_since; /* when learning began */
    zsrc_rf_watch_t watch[ZSRC_RF_WATCH_MAX];
    zsrc_rf_cand_t cand[ZSRC_RF_CAND_MAX];
    zsrc_rf_tx_t tx[ZSRC_RF_RECENT]; /* for collapsing transmitters' repeats */
    uint64_t now;                    /* the latest frame's time */
    uint64_t clock;                  /* the hub's latest tick: "12 s ago" in the picker moves on
                                      * between frames (every second while learning) */
    /* counters for the console (RF-002, RF-006, RF-009) */
    uint32_t frames;        /* every frame handed in */
    uint32_t transmissions; /* after collapsing repeats */
    uint32_t confirmed;     /* transmissions whose frame arrived ZSRC_RF_CONFIRM_FRAMES times */
    uint32_t unknown;       /* transmissions from devices no zone is bound to */
    uint32_t bursts;        /* transmissions longer than ZSRC_RF_BURST_FRAMES (replay?) */
    uint32_t replays;       /* rolling codes heard again: refused (RF-016) */
    uint32_t refused;       /* frames of a protocol or an address this firmware does not know */
} zsrc_rf_t;

void zsrc_rf_init(zsrc_rf_t *r, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_rf_ops(void); /* register with the zsrc_rf_t as ctx */

zsrc_address_t zsrc_rf_address(uint8_t proto, uint32_t device, uint8_t data);
/* The parts of an address back out; false when it is not an RF address. */
bool zsrc_rf_address_parts(const zsrc_address_t *a, uint8_t *proto, uint32_t *device,
                           uint8_t *data);
/* A protocol's widths (0 for one this firmware does not know) and its name ("EV1527"). */
uint8_t zsrc_rf_device_bits(uint8_t proto);
uint8_t zsrc_rf_codes(uint8_t proto);
const char *zsrc_rf_proto_name(uint8_t proto);

/* The receiver answered (or stopped answering) on its bus. */
void zsrc_rf_receiver(zsrc_rf_t *r, bool present, uint64_t now);
/* A decoded frame. Returns true when it confirmed a transmission: the frame's
 * ZSRC_RF_CONFIRM_FRAMES-th arrival (RF-009), or the first of a checked protocol, once per
 * transmission; that is also when the bound zones hear of it (RF-014). */
bool zsrc_rf_packet(zsrc_rf_t *r, const zsrc_rf_packet_t *p, uint64_t now);
/* An RSSI sample (RF-005). One taken within ZSRC_RF_TALK_MS of a frame is the sensor that sent
 * it, not a carrier - unless its transmission has gone on past ZSRC_RF_BURST_FRAMES: a stuck
 * transmitter blocks the band like any jammer. */
void zsrc_rf_noise(zsrc_rf_t *r, int8_t rssi_dbm, uint64_t now);
/* The same judgement from a receiver with no RSSI (a superheterodyne module): true while a
 * carrier sits on the band - its data line held high far more than any sensor holds it. */
void zsrc_rf_carrier(zsrc_rf_t *r, bool carrier, uint64_t now);
bool zsrc_rf_jammed(const zsrc_rf_t *r);

/* One line for the console: "EV1527 A2B3C code 5" (the address as wide as the protocol's). */
void zsrc_rf_code_words(uint8_t proto, uint32_t device, uint8_t data, char *out, size_t len);

/* The picker of a zone's tamper or low-battery input (RF-021) wants the codes of the zone's own
 * sensor, one each, not a list of sensors: which code the cover sends is the installer's call,
 * since the first code heard may be the door's closing code. The query this writes for the
 * zone's main address ("codes 1 A2B3C"; "" when it is not an RF address) makes the candidates
 * that sensor's codes heard while learning, in the order first heard: name "Code 9", detail
 * "Sent first, 3 times", state "12 s ago". A code that ended one of the sensor's bound inputs
 * since start-up is its closed code: detail "Closes the zone, ...", and marked unusable - some
 * contacts send their closed code when the cover comes off too, and a tamper on it would show
 * every time the door shuts (docs/bugs/B012). */
void zsrc_rf_codes_query(const zsrc_address_t *main, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_RF_H */
