/*
 * plink_store.h - what the panel link keeps in NVS `link` (docs/features/11 "Pairing", LINK-014,
 * LINK-022): the master's record (its net id and its keypads) and the keypad's record (its
 * master). Both are stored contracts read by older and newer firmware: never renumber or reuse a
 * field; the format changes only with a new frozen fixture next to test/plink_store_fixture_v1.h.
 *
 * Layout (little-endian): magic 'P' 'L', kind u8 ('M' master, 'K' keypad), format u8 (1), compat
 * u8 (1: the newest a reader must understand), payload_len u16, crc32 u32 over the payload, then
 * the payload: fixed fields, then for the master one entry per keypad, each a length byte and its
 * fields (fields appended later are skipped by older readers; a damaged entry is dropped and the
 * others loaded).
 *
 * The records hold the pairing keys: never logged, never in a backup (the `link` namespace is
 * not in the backup policy), wiped by a factory reset.
 */
#ifndef PLINK_STORE_H
#define PLINK_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_SUPERVISION_DEFAULT_S 300u /* LINK-007: 5 min (decided 2026-10-15) */

#define PLINK_PEER_PROVISIONAL 0x01u /* paired, no handshake yet: not supervised (LINK-016) */

/* One keypad at the master. */
typedef struct {
    uint8_t device;         /* 1..8 */
    uint8_t home;           /* 1..ALARM_MAX_PARTITIONS */
    uint8_t kind;           /* plink_kind_t */
    uint8_t flags;          /* PLINK_PEER_* */
    uint16_t supervision_s; /* 0 = not supervised */
    uint8_t k_pair[PLINK_KEY_LEN];
    int64_t paired_epoch_ms; /* 0 = not known */
    char name[PLINK_NAME_MAX + 1];
    bool switch_allowed;
    bool return_never;
    uint16_t return_s; /* 0 = default 30 seconds */
} plink_peer_rec_t;

typedef struct {
    uint16_t net_id; /* 0 = not chosen yet */
    uint8_t count;
    plink_peer_rec_t peer[PLINK_KEYPADS_MAX];
} plink_master_rec_t;

/* A keypad's master. */
typedef struct {
    uint16_t net_id;
    uint8_t device;
    uint8_t home;
    uint8_t k_pair[PLINK_KEY_LEN];
    char master_name[PLINK_NAME_MAX + 1];
    char panel_id[PLINK_PANEL_ID_LEN + 1];
} plink_keypad_rec_t;

/* Upper bound of an encoded record. */
#define PLINK_STORE_MAX 1024u

bool plink_store_master_encode(const plink_master_rec_t *rec, uint8_t *out, size_t cap,
                               size_t *len);
/* false for a record that isn't one (wrong magic, kind, CRC, compat newer than this firmware
 * reads); *rec is then zeroed. Damaged or duplicate entries, and numbers outside 1..8, are
 * dropped. */
bool plink_store_master_decode(const uint8_t *buf, size_t len, plink_master_rec_t *rec);
bool plink_store_keypad_encode(const plink_keypad_rec_t *rec, uint8_t *out, size_t cap,
                               size_t *len);
/* false as the master's, and for a record without its fields, a network (net_id 0) or a number
 * in 1..8; *rec is then zeroed. */
bool plink_store_keypad_decode(const uint8_t *buf, size_t len, plink_keypad_rec_t *rec);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_STORE_H */
