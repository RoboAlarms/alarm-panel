/*
 * rf_mock.h - mock 433 MHz devices for the host tests (docs/features/25 "Mock devices"): the
 * sensors, remotes and neighbours people have, each as what it puts on the air - its chip's frame
 * with that product's own timings, the code it sends for each event, how many frames, its habits
 * (a PIR asleep after a trigger, a smoke alarm repeating while it sounds, a cover that re-sends
 * the door's state, a cell that sags and slows the oscillator) - and a model of the air and the
 * receiver between it and the panel. The tests play them as pulses into the real decoder
 * (rf_decode) and hand what it decodes to the real source (zsrc_rf), as the rf task does
 * (components/services/rf_service).
 *
 * Every profile says where its behaviour comes from (rtl_433's decoders and flex configs, a
 * seller, Tasmota captures, our bench) and what is assumed. A profile with proto 0 is a
 * neighbour (a weather sensor, another alarm system's sensor): nothing of it may decode.
 *
 * Portable C11, no heap, no test framework.
 */
#ifndef RF_MOCK_H
#define RF_MOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rf_decode/rf_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The receiver ends a reception after the line is quiet this long (rf_capture.h's
 * RF_CAPTURE_IDLE_US, from RF_RECEPTION_IDLE_US): the decoder then gets a gap, not the low. */
#define RFM_IDLE_US RF_RECEPTION_IDLE_US

typedef enum {
    RFM_OPEN,        /* a contact's magnet leaves */
    RFM_CLOSE,       /* ... and returns */
    RFM_MOTION,      /* a PIR sees someone */
    RFM_ALARM,       /* smoke, water, a panic button: the condition (re-sent while it lasts) */
    RFM_TAMPER,      /* the cover comes off */
    RFM_TAMPER_BACK, /* the cover goes back */
    RFM_BATTERY,     /* the cell is low: sent after each event (see the profile) */
    RFM_TEST,        /* the test button */
    RFM_BUTTON_1,    /* a remote's buttons; on alarm fobs: arm, disarm, home, panic */
    RFM_BUTTON_2,
    RFM_BUTTON_3,
    RFM_BUTTON_4,
    RFM_HEARTBEAT,   /* a periodic transmission */
    RFM_EVENTS
} rfm_event_t;

#define RFM_NONE       0xFFu /* the device sends nothing for this event */
#define RFM_DOOR_STATE 0xFEu /* it re-sends the door's current code (open or close) */

typedef enum {
    RFM_PWM,     /* a bit is a (mark, space) pair: zero (short, long), one (long, short) */
    RFM_PPM,     /* fixed marks, the bit in the space after it (zero_space, one_space) */
    RFM_ENCODER, /* the decoder's own encoder for `proto` at `unit_us` (KeeLoq, arctech, Holtek) */
} rfm_shape_t;

typedef struct {
    const char *id;      /* "ev1527_contact_e7" */
    const char *name;    /* "An EV1527 door contact that sends E open and 7 closed" */
    const char *source;  /* where its behaviour comes from */
    const char *assumed; /* what is not known and assumed here ("" nothing) */
    rfm_shape_t shape;
    uint8_t proto;       /* what the panel decodes it as (rf_proto_t); 0: nothing, a neighbour */
    uint8_t bits;        /* data bits in a frame (the code word, most significant first) */
    uint8_t addr_bits;   /* bits of the code word that are its address (the rest: the data) */
    /* Timings in us, at a fresh cell. A sync pair comes before each frame (0 0: none). */
    uint32_t sync_mark, sync_space;
    uint16_t zero_mark, zero_space, one_mark, one_space;
    uint8_t preamble;    /* pairs of (pre_us, pre_us) before each frame (LaCrosse: 4) */
    uint16_t pre_us;
    uint8_t trailer;     /* pairs of (trail_us, trail_us) after each frame (a smoke alarm's stop pulses: 2) */
    uint16_t trail_us;
    uint32_t frame_gap_us; /* silence after each frame (0: the next frame's sync follows) */
    uint16_t unit_us;    /* RFM_ENCODER: T */
    bool long_open;      /* arctech: its open report is the 36-bit form (72 physical bits) ... */
    uint8_t open_tail;   /* ... with these 4 bits after the unit (an arctech door contact's: 10) */
    uint16_t frames;     /* frames per event */
    uint8_t code[RFM_EVENTS]; /* the data it sends per event: RFM_NONE, RFM_DOOR_STATE */
    uint32_t lockout_ms; /* a PIR: no trigger this long after one */
    bool lockout_quiet;  /* ... and motion inside it starts it again (it needs quiet) */
    uint32_t repeat_ms;  /* a lasting alarm (RFM_ALARM) is sent again this often */
    uint32_t heartbeat_ms; /* a periodic transmission (0 none) */
    uint16_t sag_pct;    /* its oscillator's speed on a low cell, in % of a fresh one's widths
                          * (100: none; an EV1527 on a 12 V cell at 5 V: 131) */
} rfm_profile_t;

extern const rfm_profile_t RFM_PROFILES[];
extern const size_t RFM_PROFILE_COUNT;
const rfm_profile_t *rfm_profile(const char *id); /* NULL: no such profile */

/* ---- the air ---------------------------------------------------------------------------------- */

#define RFM_MAX_HIGHS 24000u /* marks on the air at once (queued, not yet played) */

typedef struct {
    int32_t shift_us;   /* the slicer: every mark longer by this, its space shorter (strong: +) */
    uint32_t jitter_us; /* every edge moved by up to this, either way */
    uint32_t agc_us;    /* after a quiet line, marks in the first this-long of a burst are lost */
    uint32_t static_per_s; /* edges a second of static on an empty band (a superhet's AGC turned
                            * up: ~2000, docs/features/25 "Robustness" row 4) */
} rfm_channel_t;

/* Packets the decoder hands out arrive here, at the time the receiver would have them. */
typedef void (*rfm_deliver_fn)(void *ctx, const rf_packet_t *p, uint64_t now_us);

typedef struct {
    uint64_t start_us, end_us;
} rfm_high_t;

typedef struct {
    rfm_high_t highs[RFM_MAX_HIGHS]; /* marks queued, in absolute time */
    size_t n;
    uint64_t played_us;  /* everything before this is on its way to the decoder */
    uint64_t last_edge;  /* the end of the last mark played */
    uint64_t burst_start; /* the first mark after the last quiet line (the AGC settles from it) */
    uint64_t on_air_end; /* the end of the last mark on the air, before the receiver */
    bool quiet;          /* the receiver ended its reception (a gap was given) */
    rfm_channel_t ch;
    uint32_t seed;
    rf_decoder_t dec;
    rfm_deliver_fn deliver;
    void *ctx;
    uint32_t packets;    /* handed to deliver */
} rfm_air_t;

/* An empty band from `now_us`, heard through `ch` (NULL: a perfect receiver). */
void rfm_air_init(rfm_air_t *a, const rfm_channel_t *ch, rfm_deliver_fn deliver, void *ctx,
                  uint64_t now_us);
/* Plays everything queued that starts before `until_us`: overlapping transmissions add up (OOK
 * has no capture effect), the channel moves the edges, the decoder gets pulses and gaps. */
void rfm_air_play(rfm_air_t *a, uint64_t until_us);
/* Queues raw pulses (high first or not) from `at_us`; returns when they end. */
uint64_t rfm_air_pulses(rfm_air_t *a, const rf_pulse_t *p, size_t n, uint64_t at_us);

/* ---- a device --------------------------------------------------------------------------------- */

typedef struct {
    const rfm_profile_t *p;
    uint32_t address;    /* its address bits (EV1527: 20) */
    bool open;           /* a contact's door */
    bool low;            /* its cell is low: slower (sag_pct), and its battery code follows */
    bool alarm;          /* a lasting alarm (smoke, water) */
    uint64_t next_repeat_us;
    uint64_t asleep_until_us; /* a PIR's lockout */
    uint64_t next_heartbeat_us;
    uint32_t hop;        /* KeeLoq's counter */
    uint32_t sent;       /* bursts it sent */
} rfm_device_t;

void rfm_device_init(rfm_device_t *d, const rfm_profile_t *p, uint32_t address);
/* The data it sends for `ev` now (the door's state for a cover that re-sends it), RFM_NONE. */
uint8_t rfm_code_for(const rfm_device_t *d, rfm_event_t ev);
/* What it puts on the air for `ev` at `now_us` (a PIR asleep, or an event it has no code for,
 * sends nothing; a low cell adds its battery code after). Returns the end of what it sent,
 * `now_us` if nothing. */
uint64_t rfm_event(rfm_air_t *a, rfm_device_t *d, rfm_event_t ev, uint64_t now_us);
/* A lasting alarm starts or ends (smoke, water): sent now, then every repeat_ms in rfm_tick. */
uint64_t rfm_alarm(rfm_air_t *a, rfm_device_t *d, bool on, uint64_t now_us);
/* Its own transmissions due by `now_us` (alarm repeats, heartbeats). */
void rfm_tick(rfm_air_t *a, rfm_device_t *d, uint64_t now_us);
/* One burst of `data` as this device sends it (frames from the profile; 0: its own count). */
uint64_t rfm_burst(rfm_air_t *a, rfm_device_t *d, uint8_t data, unsigned frames, uint64_t now_us);

/* The packet the decoder should make of this device sending `data` (its address and data, the
 * protocol's fields), for comparisons. */
rf_packet_t rfm_expected(const rfm_device_t *d, uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* RF_MOCK_H */
