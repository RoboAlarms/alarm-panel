/*
 * fake_zac93.h - a pretend ZAC93 on a pretend wire, for the host tests and, later, the PC
 * simulator (docs/features/36 README "Test and acceptance"; what alarm_core/test/sim.c is for
 * the engine).
 *
 * The module (fake_zac93_t) parses what the host writes with a zw_rx_t and answers as the
 * module does (serial-api.md §1): an ACK at once (or a NAK or CAN on demand), then what its
 * script says for that FUNC - a response and callbacks, each after a delay, or "not queued" a
 * number of times. Its own frames go out one at a time, each waiting for the host's ACK and sent
 * again after a NAK or 1600 ms, 3 times at most. Faults on demand: silence (a hung module only
 * the reset line revives, or a dead one), NAKs, a CAN, a lost or late ACK, a corrupted byte, a
 * function never answered, a SerialAPISetup sub-command refused, any frame or raw text at a given
 * time, a restart, the bootloader (its menu in one read or two; booting into it, or crashing into
 * it while running), and wrong identities (an end node, another chip, a missing function, an old
 * protocol). SetRFRegion takes only a region the module lists. SoftReset
 * restarts it (SerialAPIStarted, reason 07), the reset line too (reason 00), and SendDataAbort
 * can provoke the pending SendData callback. Every start-up command (serial-api.md §5) is answered
 * from a configurable identity (fake_identity_t; the default is a ZAC93 on SDK 7.22.1), built
 * with zwave_proto's codecs; a scripted rule for a FUNC comes first.
 *
 * The wire (fake_wire_t) plays the panel's zw_link task: the host's writes reach the module;
 * the module's bytes go through a zw_rx_t whose good frames are ACKed and bad ones NAKed at once,
 * and reach the host as frames, ACK/NAK/CAN bytes and the bytes the parser discarded ("text").
 * Bytes travel instantly; time is a fake clock that fake_wire_run_until() moves from deadline to
 * deadline of the host and the module.
 *
 * Portable C11, no heap; no test framework, so the PC simulator can use it too.
 */
#ifndef FAKE_ZAC93_H
#define FAKE_ZAC93_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fake_zdev.h"
#include "zwave_proto/zw_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_ZAC93_RULES       16u
#define FAKE_ZAC93_CALLBACKS    4u
#define FAKE_ZAC93_ANSWER_MAX  48u   /* bytes of one scripted response or callback payload */
#define FAKE_ZAC93_TIMED       32u   /* outputs waiting for their time */
#define FAKE_ZAC93_OUTBOX      16u   /* frames waiting for their turn on the wire */
#define FAKE_ZAC93_LOG         64u   /* the last frames heard from the host */
#define FAKE_ZAC93_OUT_BYTES 4096u   /* bytes sent that the wire has not taken yet */

/* One scripted answer: its payload and when it goes, counted from the request's ACK. */
typedef struct {
    uint8_t len;
    uint8_t bytes[FAKE_ZAC93_ANSWER_MAX];
    uint32_t after_ms;
} fake_answer_t;

/* "On FUNC x": what the module answers a request with that FUNC, after its ACK. */
typedef struct {
    uint8_t func;
    bool respond;                                 /* a response (TYPE 01) with response.bytes */
    fake_answer_t response;
    uint8_t not_queued;                           /* the next this-many requests get response 00
                                                   * (after response.after_ms) and nothing else */
    uint8_t callbacks;                            /* how many callbacks (TYPE 00) follow */
    fake_answer_t callback[FAKE_ZAC93_CALLBACKS]; /* each: the request's callback ID, then bytes */
    uint8_t callback_id_from_end;                 /* where the request carries its callback ID,
                                                   * counted back from its last payload byte (0:
                                                   * the last, as SendData, AddNode...) */
    bool callback_on_abort;                       /* SendData: a SendDataAbort drops the scripted
                                                   * callbacks not yet sent and sends this one */
    fake_answer_t abort_callback;
} fake_rule_t;

/* Faults, set directly by a test. The "next" ones are used up by the frame they hit. */
typedef struct {
    bool silent;            /* hung: hears and sends nothing until the reset line revives it */
    bool dead;              /* broken or unplugged: as silent, and the reset line does not help */
    unsigned nak_next;      /* NAK this many coming frames, without acting on them */
    bool can_next;          /* CAN the next frame, without acting on it */
    bool drop_next_ack;     /* act on the next frame but lose its ACK */
    uint32_t ack_delay_ms;  /* next frame: its ACK, and everything that follows, this much later */
    int corrupt_in;         /* send this many bytes untouched, then corrupt one; -1: none */
    uint8_t corrupt_xor;    /* how: XOR with this (0 counts as 0x01) */
    bool ignore_soft_reset; /* SoftReset: ACK it but do not restart */
    uint8_t mute_func;      /* ACK this function's requests but answer nothing... */
    uint8_t mute_count;     /* ...the next this-many times (FAKE_MUTE_FOREVER: always) */
    uint8_t garble_func;    /* answer this function's requests with the one-byte response 00... */
    uint8_t garble_count;   /* ...the next this-many times */
    uint8_t refuse_setup;   /* a SerialAPISetup sub-command it lists but answers with status 00 */
    bool bootloader;        /* in the Gecko bootloader: every byte gets the menu, "run" leaves */
    bool bootloader_sticky; /* "run" starts the application, which falls back into it at once */
    uint8_t menu_split;     /* the menu in two reads: this many bytes, then the rest... */
    uint32_t menu_gap_ms;   /* ...this much later (menu_split 0: in one) */
    /* Its own firmware update (serial-api.md §8), by XMODEM block number (1-based, 0: none): */
    uint32_t otw_nak_block;    /* NAK this block this many times (otw_nak_times, 1 if 0)... */
    uint8_t otw_nak_times;
    uint32_t otw_silent_block; /* answer nothing for this block, ever */
    uint32_t otw_cancel_block; /* cancel at this block with this error code... */
    uint8_t otw_cancel_error;
    uint8_t otw_cancel_times;  /* ...this many uploads (0: every one) */
    bool otw_no_upload;        /* its menu has no "upload gbl" */
    bool otw_bad_image;        /* the uploaded firmware doesn't start: "run" gets the menu again */
} fake_faults_t;

#define FAKE_MUTE_FOREVER 0xFFu
#define FAKE_REGIONS_MAX 8u

/* Who the module says it is, for every start-up command (serial-api.md §5, §6). */
typedef struct {
    uint8_t firmware[2];       /* 07: application version, major and minor */
    uint16_t manufacturer, product_type, product_id; /* 07 */
    uint8_t function_mask[32]; /* 07: bit n of byte b = function 8b + n + 1 */
    uint8_t api_version;       /* 02 */
    uint8_t api_capabilities;  /* 02: bit 0 end node, bit 2 secondary, bit 3 SIS */
    uint8_t node_list[29];     /* 02 */
    uint8_t chip_type, chip_version; /* 02 */
    char library[16];          /* 15 */
    uint8_t library_type;      /* 15 */
    uint8_t protocol[3];       /* 09: major, minor, patch */
    uint8_t controller_caps;   /* 05 */
    uint8_t setup_supported[32]; /* 0B 01: bit s = sub-command s */
    uint8_t max_payload, lr_max_payload; /* 0B 10, 0B 11 */
    uint8_t regions[FAKE_REGIONS_MAX], region_count; /* 0B 15 */
    uint8_t region;            /* 0B 20; 0B 40 sets it (one of `regions`) for the next restart */
    uint32_t home_id;          /* 20 */
    uint8_t node_id;           /* 20 */
    uint16_t suc_node_id;      /* 56; 54 sets it */
    uint8_t old_timeouts[2];   /* 06 answers these, then keeps the new ones */
    uint8_t generic, specific; /* SerialAPIStarted's device classes */
} fake_identity_t;

/* A ZAC93 on SDK 7.22.1: firmware 1.50, manufacturer 027A, product type 0004, ID 0611, an
 * 800-series static controller, primary, node 1 of home C0FFEE01, no SUC yet, region USA
 * (and USA Long Range on offer). */
void fake_identity_default(fake_identity_t *id);

/* How the module answered a frame it heard. */
typedef enum {
    FAKE_ACKED = 0,
    FAKE_NAKED,
    FAKE_CANNED,
    FAKE_ACK_LOST,
    FAKE_ACK_LATE
} fake_answered_t;

typedef struct {
    uint64_t at;
    uint8_t type, func;
    size_t payload_len;
    uint8_t payload[FAKE_ZAC93_ANSWER_MAX]; /* the first bytes of it */
    size_t raw_len;
    uint8_t raw[FAKE_ZAC93_ANSWER_MAX + 5u]; /* the first bytes of the frame, SOF on */
    fake_answered_t answered;
} fake_heard_t;

typedef struct {
    bool used;
    bool scripted;     /* planned by the test (send_at, restart_at...): survives a restart */
    uint8_t kind;      /* frame (through the outbox), raw bytes, restart, delivery to a device */
    uint8_t reason;    /* restart: SerialAPIStarted's wake-up reason */
    uint16_t node;     /* delivery: the device's node ID */
    uint64_t at;
    uint32_t seq;      /* ties go in the order they were planned */
    size_t len;
    uint8_t bytes[ZW_FRAME_MAX];
} fake_timed_t;

typedef struct {
    size_t len;
    uint8_t bytes[ZW_FRAME_MAX];
} fake_frame_buf_t;

#define FAKE_ZAC93_DEVICES 12u

typedef struct fake_zac93 {
    fake_rule_t rules[FAKE_ZAC93_RULES];
    size_t n_rules;
    fake_faults_t fault;
    fake_identity_t id;
    bool identity_answers;  /* answer the start-up commands from `id` (default); false: a bare
                             * module that answers only its script */
    uint32_t soft_reset_ms; /* SoftReset's ACK to its SerialAPIStarted */
    uint32_t boot_ms;       /* the reset line released, or "run": to SerialAPIStarted */
    /* what the host has set */
    uint8_t node_info[36];  /* SetApplicationNodeInformation's CC list, in SerialAPIStarted */
    size_t node_info_len;
    uint8_t next_region;    /* SetRFRegion: the region after the next restart */
    uint8_t node_id_bits;   /* SetNodeIDType: 8 or 16 (a restart returns to 8) */
    bool tx_report;         /* SetTxStatusReport */
    /* The air (network.md §9): GetBackgroundRSSI answers bg_rssi (bg_rssi_n channels, signed
     * dBm; three quiet ones at first); air_blocked: every SendData fails without being sent (no
     * transmit time), nothing reaches a device */
    int8_t bg_rssi[4];
    /* ZW-040: the signal the device's frames carry. rx_rssi: the RSSI byte of every frame
     * fake_zac93_from_device sends (-60 dBm at first; ZW_RSSI_NOT_AVAILABLE for none); rx_status:
     * its receive status byte (0 singlecast of this network; 0x40 foreign, 0x04 broadcast);
     * ack_rssi, ack_repeaters, ack_repeater_ids: the TX report of a delivered SendData (-60 dBm,
     * direct, at first) */
    int8_t rx_rssi;
    uint8_t rx_status;
    int8_t ack_rssi;
    uint8_t ack_repeaters;
    uint8_t ack_repeater_ids[4];
    uint8_t bg_rssi_n;
    uint32_t bg_rssi_asks;
    bool air_blocked;
    /* serial-api.md §6.12: the next jam_sends SendData callbacks say Fail with 0 transmit ticks
     * (a jammed module: nothing sent, nothing heard), then as usual; jammed_sends counts them */
    uint8_t jam_sends;
    uint32_t jammed_sends;
    /* Power (§6.7, phase 3; answered only when the identity lists 08/13/04/12/05/03): in
     * effect now, and what a Set asked for - in effect after the next restart */
    int16_t power_dbm10, power_cal_dbm10, lr_power_dbm10;
    int16_t next_power_dbm10, next_lr_power_dbm10;
    uint32_t power_sets;
    /* Its bootloader's upload (serial-api.md §8): receiving, the block being collected, what
     * was received; the firmware it runs after "run" when an upload completed */
    bool xm_receiving;
    uint8_t xm_buf[133];
    size_t xm_len;
    uint32_t xm_blocks;        /* good blocks received this upload */
    uint32_t xm_crc;           /* CRC-32 of their data, padding included */
    uint8_t xm_naks_left;
    uint8_t xm_cancels;        /* uploads cancelled so far */
    uint32_t uploads;          /* uploads completed (EOT after good blocks) */
    uint8_t next_firmware[2];  /* the application version an upload installs */
    uint32_t otw_block_ms;     /* a good block's ACK this much later (0: at once) - the preview's
                                * upload takes a while, as a real one does */
    bool reset_held;        /* the reset line is low: the module is held in reset */
    bool falls_back;        /* started from a sticky bootloader: the next NAK sends it back */

    zw_rx_t rx;             /* the module's parser for the host's bytes */
    fake_timed_t timed[FAKE_ZAC93_TIMED];
    uint32_t timed_seq;
    fake_frame_buf_t outbox[FAKE_ZAC93_OUTBOX];
    size_t outbox_head, outbox_count;
    bool in_flight;         /* outbox[outbox_head] sent, waiting for the host's ACK */
    uint8_t in_flight_tx;
    uint64_t in_flight_due;
    uint8_t out[FAKE_ZAC93_OUT_BYTES];
    size_t out_len;
    uint8_t send_data_cb;   /* a SendData whose callback has not gone out: its callback ID */

    fake_heard_t log[FAKE_ZAC93_LOG];
    size_t heard;           /* frames heard in all; the log keeps the last FAKE_ZAC93_LOG */
    uint32_t acks_in, naks_in, cans_in; /* the host's answers to the module's frames */
    uint32_t restarts;
    uint32_t resent;        /* own frames sent again after a NAK or no ACK */
    uint32_t dropped;       /* own frames given up after 3 transmissions */
    uint32_t overflow;      /* something did not fit (a test failure) */

    /* The radio network behind it: simulated devices, AddNode's status reports, SendData to a
     * device, AssignSUCReturnRoute (network.md §4). */
    fake_zdev_t *devices[FAKE_ZAC93_DEVICES];
    size_t n_devices;
    uint16_t next_node;       /* the node ID the next inclusion gives (2 first) */
    uint8_t add_state;        /* AddNode: idle, listening, a device joining */
    uint8_t add_cb;           /* the callback ID of AddNode(any): its status reports carry it */
    fake_zdev_t *joining;
    uint16_t join_id;
    uint32_t join_step_ms;    /* NodeFound, AddingEndNode, ProtocolDone come this far apart */
    uint8_t join_stall_after; /* a status after which the protocol goes quiet; 0: none */
    uint8_t join_fail_after;  /* a status after which Failed comes; 0: none */
    uint16_t join_node_id;    /* != 0: the node ID it gives (one already known, FF...) */
    bool skip_ready;          /* AddNode(any) sends no Ready: the first report is the next one */
    bool smart_listen;        /* AddNode 49: SmartStart requests are passed on (network.md §4.2) */
    uint32_t smart_asks;      /* SmartStart requests sent (ApplicationUpdate 85) */
    uint32_t smart_dsk_adds;  /* AddNode 08 heard */
    bool route_fails;         /* AssignSUCReturnRoute's callback says Fail */
    /* Route repair (network.md §11): RequestNodeNeighborUpdate says "started", then done when its
     * device hears it (awake), else failed - or failed for the next neighbor_fails asks;
     * DeleteReturnRoute answers started, then OK when its device hears it */
    uint8_t neighbor_fails;
    /* ReplaceFailedNode (network.md §10): refused ("not in the failed list") for a node not on
     * `failed`; "the node answered" when its device hears; else "ready" - then
     * fake_zac93_press_replace() gives a new device that node ID ("done") */
    bool replace_ready;
    uint16_t replace_node;
    uint8_t replace_cb;
    uint32_t replace_asks;
    uint32_t neighbor_asks, delete_route_asks, suc_route_asks;
    uint16_t routed[16];      /* the nodes asked for a neighbour update, in order */
    size_t routed_n;
    uint32_t radio_ms;        /* SendData to a device: heard at half of it, callback at its end */
    uint32_t reply_ms;        /* a device's frame reaches the host this long after it went */
    /* RemoveNode (network.md §10), the failed list, HardReset */
    uint8_t remove_state;     /* idle, listening */
    uint8_t remove_cb;
    uint8_t failed[29];       /* nodes whose last SendData got no ACK (IsFailedNode) */
    bool skip_remove_ready;   /* RemoveNode(any) sends no Ready */
    bool failed_off;          /* it never lists a node as failed */
    uint32_t hard_resets;
    uint32_t next_home_id;    /* the home ID a HardReset gives */
    uint32_t by_func[256];    /* frames heard from the host, by FUNC (the log keeps the last) */
} fake_zac93_t;

/* The default identity, no script, no faults; SoftReset answered after 20 ms, the reset line
 * and "run" after 200 ms. */
void fake_zac93_init(fake_zac93_t *f);

/* The reset line: held low (asserted) the module stops; released, it restarts - revived when it
 * was hung (fault.silent) - and sends SerialAPIStarted (reason 00) after boot_ms, or prints the
 * bootloader's menu when it boots into it. */
void fake_zac93_reset_line(fake_zac93_t *f, uint64_t now_ms, bool asserted);

/* Adds the rule for rule->func, or replaces the one there is. */
bool fake_zac93_on(fake_zac93_t *f, const fake_rule_t *rule);

/* The test's script of the world; it survives the module's restarts (which forget only the
 * module's own pending answers). */
/* A frame of the module's own at a given time (SerialAPIStarted, ApplicationCommandHandler...). */
bool fake_zac93_send_at(fake_zac93_t *f, uint64_t at_ms, uint8_t type, uint8_t func,
                        const uint8_t *payload, size_t len);
/* Raw bytes at a given time, outside any frame (bootloader text, line noise). */
bool fake_zac93_send_raw_at(fake_zac93_t *f, uint64_t at_ms, const uint8_t *bytes, size_t len);
/* A restart at a given time: everything in flight is forgotten, then SerialAPIStarted. */
bool fake_zac93_restart_at(fake_zac93_t *f, uint64_t at_ms, uint8_t reason);
/* The application crashes into the Gecko bootloader now: everything in flight is forgotten and
 * the menu printed; from then on it is fault.bootloader ("run" starts the application again). */
void fake_zac93_enter_bootloader(fake_zac93_t *f, uint64_t now_ms);

/* The i-th frame heard from the host (0 = the first), while the log still holds it. */
const fake_heard_t *fake_zac93_heard(const fake_zac93_t *f, size_t i);

/* A simulated device in radio range. */
bool fake_zac93_add_device(fake_zac93_t *f, fake_zdev_t *d);
/* Its learn button: while the module listens (AddNode any) it joins - NodeFound, AddingEndNode
 * (or AddingController) with the next node ID and its NIF, ProtocolDone, join_step_ms apart -
 * and learns its node ID at once. False when the module is not listening. */
bool fake_zac93_press(fake_zac93_t *f, uint64_t now, fake_zdev_t *d);
/* A device's frame for the panel: an ApplicationCommandHandler reply_ms later (fake_zdev's). */
void fake_zac93_from_device(fake_zac93_t *f, uint64_t now, const fake_zdev_t *d,
                            const uint8_t *cmd, size_t len);
/* ZW-042: a command from `node` with the test's receive status (0x04 broadcast, 0x08
 * multicast), plain, reply_ms later: what a device sends to everyone (a Wake Up Notification
 * with no wake-up destination), or a node the panel does not know. */
void fake_zac93_from_node_as(fake_zac93_t *f, uint64_t now, uint16_t node, uint8_t rx_status,
                             const uint8_t *cmd, size_t len);
/* Its button while the module listens for a removal (RemoveNode any): NodeFound,
 * RemovingEndNode with its node ID (0 when it belongs to another network), Done; the device is
 * out of every network after. False when the module is not listening. */
bool fake_zac93_press_remove(fake_zac93_t *f, uint64_t now, fake_zdev_t *d);
/* A device already in this network (a restored module, a test's set-up): listed, given its node
 * ID, without an inclusion. */
void fake_zac93_join(fake_zac93_t *f, fake_zdev_t *d, uint16_t node);
/* A SmartStart device outside any network, powered, asks to join (network.md §4.2): while the
 * module listens for SmartStart (AddNode 49), ApplicationUpdate 85 reply_ms later with the NWI
 * home ID of its DSK (its public key's first 16 bytes) and its node information. AddNode 08 with
 * the home IDs of its DSK then adds it as its button would. False when the module does not listen
 * for SmartStart or the device is in a network. */
bool fake_zac93_smartstart_ask(fake_zac93_t *f, uint64_t now, fake_zdev_t *d);
/* While a ReplaceFailedNode waits ("ready"): the new device's button - it takes the failed node's
 * ID (the old device, if still listed, forgets it) and the module reports "done". False when no
 * replacement waits. */
bool fake_zac93_press_replace(fake_zac93_t *f, uint64_t now, fake_zdev_t *d);
/* A device's own node information frame (its button): ApplicationUpdate 84 reply_ms later. */
void fake_zac93_node_info(fake_zac93_t *f, uint64_t now, const fake_zdev_t *d);

/* The wire's side: the host's bytes in, the clock, the module's bytes out. */
void fake_zac93_receive(fake_zac93_t *f, uint64_t now_ms, const uint8_t *bytes, size_t len);
void fake_zac93_poll(fake_zac93_t *f, uint64_t now_ms);
uint64_t fake_zac93_next_deadline(const fake_zac93_t *f);
size_t fake_zac93_take(fake_zac93_t *f, uint8_t *out, size_t cap);

/* ---- The wire ----------------------------------------------------------------------------- */

#define FAKE_WIRE_BYTES 4096u

/* The host as the wire sees it: its link calls, its clock and its deadline. */
typedef struct {
    void *ctx;
    void (*control)(void *ctx, uint64_t now_ms, uint8_t byte);
    void (*frame)(void *ctx, uint64_t now_ms, const zw_frame_t *frame);
    void (*text)(void *ctx, uint64_t now_ms, const uint8_t *bytes, size_t len);
    void (*poll)(void *ctx, uint64_t now_ms);
    uint64_t (*next_deadline)(void *ctx);
} fake_host_t;

/* Every byte on the wire, as it goes: to_module for the panel's (the host's writes and zw_link's
 * ACK/NAK), else the module's. Optional (fake_wire_t.tap). */
typedef struct {
    void *ctx;
    void (*bytes)(void *ctx, bool to_module, uint64_t at_ms, const uint8_t *bytes, size_t len);
} fake_tap_t;

typedef struct {
    fake_zac93_t *zac;
    fake_host_t host;
    fake_tap_t tap;
    zw_rx_t rx;                      /* zw_link's parser for the module's bytes */
    uint8_t to_zac[FAKE_WIRE_BYTES]; /* the host's writes and zw_link's ACK/NAK, on their way */
    size_t to_zac_len;
    uint64_t now;
    uint32_t written;                /* bytes the host wrote */
    uint32_t acks, naks;             /* zw_link's answers to the module's frames */
    bool stuck;                      /* the wire did not settle, or something did not fit */
} fake_wire_t;

void fake_wire_init(fake_wire_t *w, fake_zac93_t *zac, const fake_host_t *host, uint64_t now_ms);

/* The host's write port: bytes for the module, delivered at the current time. */
bool fake_wire_write(fake_wire_t *w, const uint8_t *bytes, size_t len);

/* Everything that happens at the current time: the host's poll, bytes both ways, the module's
 * timers, again and again until nothing moves. */
void fake_wire_settle(fake_wire_t *w);

/* Moves the clock to t_ms, stopping at every deadline of the host and the module on the way. */
void fake_wire_run_until(fake_wire_t *w, uint64_t t_ms);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ZAC93_H */
