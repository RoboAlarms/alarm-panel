/*
 * zw_jam.h - is the radio jammed? (docs/features/36 network.md §9, ZW-024). Two signs:
 *   - the background noise (GetBackgroundRSSI, every ZW_JAM_POLL_MS): jammed when the QUIETEST
 *     channel stays at or above the threshold for ZW_JAM_HOLD_MS - one busy channel is ordinary
 *     traffic, every channel loud at once is not. A reading that did not come resets the wait: a
 *     stale sample never proves a jam;
 *   - frames the module could not get on the air (SendData "fail" with no transmit time):
 *     ZW_JAM_TX_FAILS within ZW_JAM_HOLD_MS.
 * It clears ZW_JAM_HOLD_MS after the noise fell back, once no such failures are that recent.
 * Portable C11, caller-owned state, no heap; time comes in as ms.
 */
#ifndef ZW_JAM_H
#define ZW_JAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_JAM_POLL_MS       5000u
#define ZW_JAM_HOLD_MS       30000u
#define ZW_JAM_THRESHOLD_DBM (-60)  /* bench-tuned later (`zwave jam <dBm>`) */
#define ZW_JAM_TX_FAILS      3u
#define ZW_JAM_CHANNELS      4u

typedef struct {
    int8_t threshold;      /* dBm */
    bool jammed;
    bool loud;             /* the last reading's quietest channel was at or above the threshold */
    uint64_t loud_since;   /* ...since then (readings in a row) */
    uint64_t quiet_since;  /* below it since then; 0: not yet quiet */
    uint64_t fails[ZW_JAM_TX_FAILS]; /* the last transmit failures, oldest first */
    size_t fail_n;
    bool has_reading;
    int8_t rssi[ZW_JAM_CHANNELS]; /* the last reading, per channel (ZW_RSSI_* when none) */
    uint8_t channels;
    int8_t quietest;       /* its quietest channel, dBm */
} zw_jam_t;

void zw_jam_init(zw_jam_t *j, int8_t threshold_dbm);
void zw_jam_set_threshold(zw_jam_t *j, int8_t threshold_dbm);
/* A GetBackgroundRSSI reading: one byte per channel (signed dBm; 7F none, 7E saturated - loud,
 * 7D below sensitivity - quiet). True when jammed changed. */
bool zw_jam_reading(zw_jam_t *j, uint64_t now, const uint8_t *rssi, size_t n);
/* The reading did not come: the wait for a jam starts again. */
void zw_jam_no_reading(zw_jam_t *j, uint64_t now);
/* A frame the module could not transmit at all. True when jammed changed. */
bool zw_jam_tx_failed(zw_jam_t *j, uint64_t now);
/* Time passing (the failures age). True when jammed changed. */
bool zw_jam_poll(zw_jam_t *j, uint64_t now);
/* The link went away: nothing is known any more (not jammed). */
void zw_jam_reset(zw_jam_t *j);
bool zw_jam_jammed(const zw_jam_t *j);

#ifdef __cplusplus
}
#endif

#endif /* ZW_JAM_H */
