/*
 * plink_frame.h - the panel link's v1 frame (docs/features/11 "Frame format (v1)"; LINK-003,
 * LINK-005, LINK-017): the 17-byte header, message types, sealing with AES-128-CCM, the 64-frame
 * replay window, and peer addresses for every transport.
 *
 * The header layout, the type numbers and the flag bits are an external contract between panels
 * of different firmware versions: never renumber or reuse, only add types.
 */
#ifndef PLINK_FRAME_H
#define PLINK_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_MAGIC0    0x41u /* 'A' */
#define PLINK_MAGIC1    0x4Cu /* 'L' */
#define PLINK_VERSION   1u
#define PLINK_HDR_LEN   17u
#define PLINK_FRAME_MAX 1200u /* LINK-005: a UDP datagram, never fragmented */
#define PLINK_FRAME_ESPNOW 250u /* LINK-005: ESP-NOW v1 */
#define PLINK_SEALED_OVERHEAD (PLINK_HDR_LEN + PLINK_TAG_LEN)
#define PLINK_PAYLOAD_MAX (PLINK_FRAME_MAX - PLINK_SEALED_OVERHEAD)
/* 225: a sealed payload in a 250-byte frame. Every payload but FW_PAGE and DEV_LIST_PAGE fits it
 * at its longest (LINK-043; plink_msg.h's PLINK_*_MAX). */
#define PLINK_PAYLOAD_SMALL (PLINK_FRAME_ESPNOW - PLINK_SEALED_OVERHEAD)

#define PLINK_FLAG_SEALED      0x01u
#define PLINK_FLAG_FROM_MASTER 0x02u
#define PLINK_FLAGS_KNOWN      0x03u

#define PLINK_SENDER_MASTER   0u
#define PLINK_SENDER_UNPAIRED 0xFFFFu
#define PLINK_DEVICE_FIRST    1u /* LINK-001: link keypads use device numbers 1..8 */
#define PLINK_DEVICE_LAST     8u
#define PLINK_KEYPADS_MAX     8u

#define PLINK_DIR_MASTER 0x4Du /* 'M': the nonce's first byte on frames from the master */
#define PLINK_DIR_KEYPAD 0x4Bu /* 'K' */

#define PLINK_SEQ_LIMIT   0x80000000u /* LINK-015: no side sends with seq >= 2^31 */
#define PLINK_SEQ_REKEY   0x40000000u /* the keypad starts a new handshake at 2^30 */

#define PLINK_UDP_PORT 6056u /* LINK-021 */

/* Message types (docs/features/11 "Messages"). 0x01..0x1F are plain, 0x20..0x7F sealed. */
typedef enum {
    PLINK_T_PAIR_REQ = 0x01,
    PLINK_T_PAIR_COMMIT = 0x02,
    PLINK_T_PAIR_NONCE = 0x03,
    PLINK_T_PAIR_REVEAL = 0x04,
    PLINK_T_PAIR_CONFIRM = 0x05,
    PLINK_T_PAIR_DONE = 0x06,
    PLINK_T_PAIR_FAIL = 0x07,
    PLINK_T_HELLO = 0x10,
    PLINK_T_WELCOME = 0x11,
    PLINK_T_REKEY = 0x12,
    PLINK_T_NOT_PAIRED = 0x13,
    PLINK_T_BEACON = 0x18,
    PLINK_T_STATUS = 0x20,
    PLINK_T_ZONES = 0x21,
    PLINK_T_ZONES_REQ = 0x22,
    PLINK_T_CFG_REQ = 0x23,
    PLINK_T_CFG_PAGE = 0x24,
    PLINK_T_EV_REQ = 0x25,
    PLINK_T_EVENTS = 0x26,
    PLINK_T_CMD = 0x27,
    PLINK_T_RESULT = 0x28,
    PLINK_T_SOUND = 0x29,
    PLINK_T_HEARTBEAT = 0x2A,
    PLINK_T_TIME = 0x2B,
    PLINK_T_BYE = 0x2C,
    /* Updates through the link (PLINK_CAP_FW). */
    PLINK_T_FW_INFO = 0x2D,
    PLINK_T_FW_ORDER = 0x2E,
    PLINK_T_FW_STATE = 0x2F,
    PLINK_T_FW_REQ = 0x30,
    PLINK_T_FW_PAGE = 0x31,
    /* Smart Home on keypads (PLINK_CAP_HOME). */
    PLINK_T_DEV_LIST_REQ = 0x32,
    PLINK_T_DEV_LIST_PAGE = 0x33,
    PLINK_T_DEV_STATE = 0x34,
    PLINK_T_DEV_STATE_REQ = 0x35,
    PLINK_T_DEV_ACT = 0x36,
    PLINK_T_DEV_ACK = 0x37,
    /* A keypad's settings (PLINK_CAP_PREFS). */
    PLINK_T_PREFS = 0x38,
    PLINK_T_PREFS_REQ = 0x39,
    PLINK_T_PREFS_SET = 0x3A,
    PLINK_T_WIFI_REQ = 0x3B,
    PLINK_T_WIFI = 0x3C,
} plink_type_t;

/* What this firmware knows about a type: known, sealed, and which side sends it. */
bool plink_type_known(uint8_t type);
bool plink_type_sealed(uint8_t type);      /* 0x20 and up */
bool plink_type_from_master(uint8_t type); /* COMMIT, REVEAL, DONE, WELCOME, REKEY, NOT_PAIRED,
                                            * BEACON, STATUS, ZONES, CFG_PAGE, EVENTS, RESULT,
                                            * SOUND, TIME, BYE, FW_ORDER, FW_PAGE, DEV_LIST_PAGE,
                                            * DEV_STATE, DEV_ACK, PREFS_REQ, PREFS_SET; and
                                            * PAIR_FAIL, which travels either way (plink_hdr_read
                                            * takes it from both) */

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t net_id;
    uint16_t sender;
    uint32_t sid;
    uint32_t seq;
} plink_hdr_t;

/* Writes the 17 header bytes. */
void plink_hdr_write(const plink_hdr_t *h, uint8_t out[PLINK_HDR_LEN]);

/* Reads and checks a frame's header: magic, version, a known type, no unknown flag bit, SEALED
 * matching the type's class, FROM_MASTER matching both the sender (0 = the master) and the
 * type's direction (PAIR_FAIL either way), a sealed frame long enough for its tag with sid and
 * seq not 0, a plain frame with sid and seq 0, a frame no longer than PLINK_FRAME_MAX. false:
 * drop the frame, nothing else to do. */
bool plink_hdr_read(const uint8_t *frame, size_t len, plink_hdr_t *out);

/* The 13-byte CCM nonce of a sealed frame: dir || sid || sender || net_id || seq, little-endian,
 * dir from the header's FROM_MASTER flag. */
void plink_nonce(const plink_hdr_t *h, uint8_t out[PLINK_CCM_NONCE_LEN]);

/* A plain frame: header + payload into out (cap bytes). false when it doesn't fit or h is not a
 * plain type; *out_len is the frame's length. Both builders refuse a header plink_hdr_read would
 * drop, and take a payload that already lies at out + PLINK_HDR_LEN. */
bool plink_frame_plain(const plink_hdr_t *h, const uint8_t *payload, size_t len, uint8_t *out,
                       size_t cap, size_t *out_len);

/* A sealed frame: header, the payload encrypted under key (16 bytes) and its tag. h->flags must
 * carry SEALED and the right FROM_MASTER; sid and seq not 0. false: a bad header, no room, or a
 * port call failed (then the frame's bytes in out are wiped: no plaintext is left there). */
PLINK_MUST_CHECK
bool plink_frame_seal(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                      const plink_hdr_t *h, const uint8_t *payload, size_t len, uint8_t *out,
                      size_t cap, size_t *out_len);

/* Decrypts a sealed frame whose header plink_hdr_read accepted: the payload into pt (cap bytes),
 * *pt_len its length. false: the tag failed (or pt is too small); pt is zeroed. Never updates a
 * replay window: the caller commits seq only after this returns true. */
PLINK_MUST_CHECK
bool plink_frame_open(const plink_crypto_t *c, const uint8_t key[PLINK_AES_LEN],
                      const plink_hdr_t *h, const uint8_t *frame, size_t len, uint8_t *pt,
                      size_t cap, size_t *pt_len);

/* The replay window (RFC 4303 §3.4.3, 64 frames): top is the highest seq accepted, bit i of bits
 * is top - i. A zeroed window accepts any seq from 1. */
typedef struct {
    uint32_t top;
    uint64_t bits;
} plink_window_t;

/* Would seq be new? 0, a seq 64 or more below top, and one already seen are not. Changes
 * nothing. */
bool plink_window_check(const plink_window_t *w, uint32_t seq);
/* Records seq as seen: call only after the frame's tag verified. */
void plink_window_commit(plink_window_t *w, uint32_t seq);

/* Transports (docs/features/11 "Transports"). Numbers travel nowhere, but keep them stable. */
typedef enum {
    PLINK_TP_NONE = 0,
    PLINK_TP_UDP = 1,    /* a[0..3] IPv4 address, a[4..5] port, both in network order */
    PLINK_TP_ESPNOW = 2, /* a[0..5] MAC */
    PLINK_TP_RF = 3,     /* reserved: 433 MHz */
} plink_transport_t;

#define PLINK_ADDR_MAX 16u
typedef struct {
    uint8_t tp;  /* plink_transport_t */
    uint8_t len; /* bytes of a used */
    uint8_t a[PLINK_ADDR_MAX];
} plink_addr_t;

/* An IPv4 UDP address (ip in network byte order as four bytes, port in host order). */
plink_addr_t plink_addr_udp(const uint8_t ip[4], uint16_t port);
bool plink_addr_equal(const plink_addr_t *a, const plink_addr_t *b);
/* The limited broadcast, 255.255.255.255 (UDP): always a broadcast. A subnet's directed
 * broadcast (ip | ~netmask) can't be told from a unicast address by its bytes (x.y.z.127 on a /25,
 * x.y.z.255 a host on a /23), so whoever builds one says so: the broadcast argument of
 * plink_keypad_master_addr and plink_pk_start (LINK-021). */
bool plink_addr_is_broadcast(const plink_addr_t *a);
/* "192.0.2.10:6056" (UDP) or "aa:bb:cc:dd:ee:ff" (ESP-NOW), "-" for none; out holds 24 bytes. */
void plink_addr_text(const plink_addr_t *a, char out[24]);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_FRAME_H */
