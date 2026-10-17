/*
 * plink_home.h - Smart Home on keypads, the pieces both sides share (docs/features/11 "The device
 * list", "Device state"; LINK-044, LINK-045, LINK-047): the device list blob the master builds
 * from its registry and a keypad pulls and reads, its list_crc, the devices' encoded records and
 * their states_crc, and a row's record carried over by ref when a keypad installs a new list.
 *
 * The blob is compat u8 (PLINK_DEV_CAT_COMPAT), count u8, then one catalog record section per
 * device in the registry's order (plink_enc_dev_cat). A device's index is its place in the blob:
 * DEV_STATE and DEV_ACK name records by it, DEV_ACT names a device by ref only. The blob and the
 * records are RAM only, on both sides.
 *
 * Portable C11, no heap, no globals; every buffer is the caller's.
 */
#ifndef PLINK_HOME_H
#define PLINK_HOME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "home_control/home_control.h"
#include "panel_link/plink_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_DEV_BLOB_HDR 2u     /* the blob's compat and count */
#define PLINK_DEV_BLOB_MAX 12544u /* 2 + 64 x 195 = 12,482 B at the longest, rounded up */

/* A device action on the keypad (LINK-047): resent every 500 ms with the same req, given up
 * after 3 s and never sent after; the master keeps an answer it may still be asked for as long. */
#define PLINK_DEV_ACT_RETRY_MS   500u
#define PLINK_DEV_ACT_GIVE_UP_MS 3000u

/* The records of one list, by index: each the record section as encoded, its length byte first
 * (plink_enc_dev_rec), as the master built it or as a keypad received it. About 2.9 KB. */
typedef struct {
    uint8_t count;                                 /* the list's rows */
    uint8_t len[PLINK_DEV_MAX];                    /* rec[i]'s bytes; 0: no record (yet) */
    uint8_t rec[PLINK_DEV_MAX][PLINK_DEV_REC_MAX];
} plink_dev_recs_t;

/* ---- the master: from its registry ---------------------------------------------------------- */

/* Whether a registry row goes in the list: a ref that isn't empty and that the codecs carry
 * whole (LINK-047: a ref is never cut or cleaned - it could name another device). Z-Wave rows
 * are listed (both screens leave them out until BL-169). */
bool plink_home_listed(const hc_device_t *d);

/* The device list blob of the registry's rows, in their order: the rows plink_home_listed takes,
 * the first PLINK_DEV_MAX of them. false when it doesn't fit cap (PLINK_DEV_BLOB_MAX always
 * does). *len its bytes, *crc its list_crc (either may be NULL). */
bool plink_home_list_build(const hc_device_t *rows, size_t n, uint8_t *blob, size_t cap,
                           size_t *len, uint32_t *crc);
/* The records of the same rows, index for index with plink_home_list_build's blob. */
void plink_home_recs_build(const hc_device_t *rows, size_t n, plink_dev_recs_t *out);
/* One row's record: flags (online, pending, failed, unsure = hc_outcome_unknown), the action
 * under way or failed, the values and the state word, as the registry holds them. */
void plink_home_rec_of(const hc_device_t *d, plink_dev_rec_t *out);

/* ---- both sides ----------------------------------------------------------------------------- */

/* list_crc: the blob's CRC-32 (alarm_crc32), never 0 (a CRC of 0 reads as 1): 0 is "no list". */
uint32_t plink_home_list_crc(const uint8_t *blob, size_t len);
/* states_crc: the CRC-32 over each record that has bytes, as DEV_STATE carries it - its index,
 * then its section - in index order (0 for none). */
uint32_t plink_home_states_crc(const plink_dev_recs_t *recs);
/* Record index decoded (false: no such record, or it has no bytes). */
bool plink_home_rec(const plink_dev_recs_t *recs, uint8_t index, plink_dev_rec_t *out);
/* A record's values as the registry's dev_values_t, what plink_home_rec_of took from it: a
 * keypad's screen reads them as the master's reads its registry's (rec NULL: nothing known). */
void plink_home_values_of(const plink_dev_rec_t *rec, dev_values_t *out);
/* A record section as received (its length byte first, len bytes) kept for index: false when
 * index is past the list or the section isn't whole or is longer than PLINK_DEV_REC_MAX. */
bool plink_home_rec_put(plink_dev_recs_t *recs, uint8_t index, const uint8_t *sec, size_t len);

/* ---- a keypad: the blob it pulled ----------------------------------------------------------- */

/* The blob as a keypad takes it (LINK-044): compat PLINK_DEV_CAT_COMPAT (a newer or an unknown one
 * is refused, never guessed at), a count of at most PLINK_DEV_MAX and that many whole catalog
 * records; fields a newer master appended inside a record are skipped, bytes after the last
 * record ignored. *count (may be NULL): its rows. */
bool plink_home_list_check(const uint8_t *blob, size_t len, uint8_t *count);
/* A walk through a blob's rows, in index order: zero it to start. */
typedef struct {
    size_t pos;    /* the next row's offset (0: the first row's) */
    uint8_t index; /* the next row's index */
} plink_home_walk_t;

/* The next row: its index in *index, its fields in *cat (either may be NULL). *usable (may be
 * NULL) is false for a row this firmware can't use - a ref the codecs refuse - which keeps its
 * index and reads as zeros. false past the last row, and at a row that isn't whole (a blob
 * plink_home_list_check refuses). */
bool plink_home_list_next(const uint8_t *blob, size_t len, plink_home_walk_t *w, uint8_t *index,
                          plink_dev_cat_t *cat, bool *usable);
/* The index of the row whose ref is ref, byte for byte (false: none). */
bool plink_home_list_find(const uint8_t *blob, size_t len, const char *ref, uint8_t *index);

/* A row's key, to find it again in the next list without keeping the old blob: the CRC-32 of its
 * ref, never 0. keys[i] for each row of a checked blob, 0 for a row it can't use and past its
 * last. */
uint32_t plink_home_ref_key(const char *ref);
void plink_home_list_keys(const uint8_t *blob, size_t len, uint32_t keys[PLINK_DEV_MAX]);
/* A new list's records at its install (LINK-044): row i of the new list (new_keys, new_count
 * rows) takes the record the old list's row with the same key had, so a rename never blinks;
 * a row with no such row has none until its own record comes. old and out must differ. Two refs
 * with the same key (one chance in about 2^32) carry the wrong record only until the full set
 * the keypad asks for at once replaces it. */
void plink_home_carry(const uint32_t old_keys[PLINK_DEV_MAX], const plink_dev_recs_t *old,
                      const uint32_t new_keys[PLINK_DEV_MAX], uint8_t new_count,
                      plink_dev_recs_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_HOME_H */
