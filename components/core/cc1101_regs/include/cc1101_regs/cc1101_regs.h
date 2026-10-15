/*
 * cc1101_regs.h - what the panel knows about the TI CC1101, as data and arithmetic
 * (docs/features/25, RF-017): register and strobe numbers, the asynchronous OOK receive table for a
 * band, the frequency word, RSSI in dBm, how a genuine chip, a clone and an empty bus answer,
 * checking a table against what the chip reads back, and noticing that the chip has reset.
 *
 * Portable C11, no heap, no bus: components/drivers/rf_rx does the SPI and uses these, so the
 * rules are host-tested (test/) instead of being found on the bench. Sources: TI CC1101
 * datasheet SWRS061 (registers, reset values, FREQ formula, RSSI) and design note DN022 (OOK).
 */
#ifndef CC1101_REGS_H
#define CC1101_REGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Command strobes (datasheet table 42). */
#define CC1101_SRES  0x30u
#define CC1101_SCAL  0x33u
#define CC1101_SRX   0x34u
#define CC1101_SIDLE 0x36u
#define CC1101_SFRX  0x3Au
#define CC1101_SNOP  0x3Du

/* Configuration registers the driver names. */
#define CC1101_IOCFG0 0x02u
#define CC1101_FREQ2  0x0Du
#define CC1101_FREQ1  0x0Eu
#define CC1101_FREQ0  0x0Fu
#define CC1101_TEST0  0x2Eu
#define CC1101_LAST_CONFIG 0x2Eu

/* Status registers (read with the burst bit, one at a time). */
#define CC1101_PARTNUM   0x30u
#define CC1101_VERSION   0x31u
#define CC1101_RSSI      0x34u
#define CC1101_MARCSTATE 0x35u
#define CC1101_PKTSTATUS 0x38u

#define CC1101_MARC_IDLE 0x01u
#define CC1101_MARC_RX   0x0Du

/* GDO0's configuration after a power-on or brown-out reset (CLK_XOSC/192, a ~135 kHz clock):
 * reading this back where the table put 0x0D means the chip has reset under the panel. */
#define CC1101_IOCFG0_RESET  0x3Fu
#define CC1101_IOCFG0_ASYNC  0x0Du /* asynchronous serial data out: what the table sets */

#define CC1101_XTAL_HZ      26000000u
#define CC1101_FREQ_MIN_KHZ 300000u /* the chip's bands: 300-348, 387-464 (and 779-928) MHz */
#define CC1101_FREQ_MAX_KHZ 464000u

typedef struct {
    uint8_t reg;
    uint8_t value;
} cc1101_reg_t;

/* Room a receive table needs (cc1101_ook_table). */
#define CC1101_TABLE_MAX 40u

/* A frequency the chip can tune to with this table (300-348 or 387-464 MHz). */
bool cc1101_band_ok(uint32_t freq_khz);
/* The FREQ2/1/0 word: f * 2^16 / 26 MHz, rounded (433.92 MHz: 0x10B071). */
uint32_t cc1101_freq_word(uint32_t freq_khz);
/* The asynchronous OOK receive table for a frequency (every configuration register the panel
 * sets, FREQ and the band's TEST0 included), in writing order; returns how many, 0 when the
 * frequency is outside the bands or 'max' is too small. */
size_t cc1101_ook_table(uint32_t freq_khz, cc1101_reg_t *out, size_t max);
/* The first entry the chip read back differently (read[i] for table[i]), or n when all agree. */
size_t cc1101_verify(const cc1101_reg_t *table, size_t n, const uint8_t *read);

/* RSSI register to dBm (datasheet 17.3: two's complement half-dB, offset 74 dB). */
int cc1101_rssi_dbm(uint8_t raw);

/* An RSSI reading while the receiver runs (RF-017, errata SWRZ020: such a read can be torn). */
typedef enum {
    CC1101_RSSI_STABLE = 0, /* two consecutive reads agreed: that value */
    CC1101_RSSI_UNSTABLE,   /* no two consecutive reads agreed within the budget: no value */
    CC1101_RSSI_BUS_ERROR,  /* a read failed on the bus (the chip is gone, not torn) */
} cc1101_rssi_result_t;

/* The reads one RSSI sample may take (the driver's budget). */
#define CC1101_RSSI_READS 8u

/* One register read: false when the bus failed. */
typedef bool (*cc1101_read_fn)(void *ctx, uint8_t *value);
/* Reads RSSI through 'read' until two consecutive reads agree, at most 'max_reads' reads; *raw
 * is written only when CC1101_RSSI_STABLE. When CC1101_RSSI_UNSTABLE after at least one read,
 * *lowest (may be NULL) gets the read with the lowest dBm (compared as cc1101_rssi_dbm(), not as
 * bytes): not a value, a lower bound on what the reads saw - a signal that never settles but never
 * drops below the jamming level is a carrier all the same (RF-005). Untouched otherwise. */
cc1101_rssi_result_t cc1101_rssi_settle(cc1101_read_fn read, void *ctx, unsigned max_reads,
                                        uint8_t *raw, uint8_t *lowest);

/* The receiver's data line in edges a second over a window (the storm guard, RF-005): 'count' and
 * 'last' are a free-running count of the symbols the receiver handed over (two edges each) now
 * and at the window's start - it may wrap once - and 'elapsed_us' the window's real length (one
 * second at least: a shorter one counts as a second). At most UINT32_MAX. Any receiver's line,
 * not only a CC1101's. */
uint32_t cc1101_edges_per_s(uint32_t count, uint32_t last, uint64_t elapsed_us);

/* MARCSTATE (low 5 bits) in words; "no answer" for 0xFF. */
const char *cc1101_state_name(uint8_t marcstate);

typedef enum {
    CC1101_ABSENT = 0, /* nothing on the bus: 0x00 / 0xFF everywhere, or a version that wanders */
    CC1101_GENUINE,    /* PARTNUM 0, VERSION 0x14 (or 0x04 on early silicon) */
    CC1101_CLONE,      /* PARTNUM 0 and a steady version TI never shipped: a compatible clone */
} cc1101_identity_t;

/* What PARTNUM and two reads of VERSION say is on the bus. */
cc1101_identity_t cc1101_identify(uint8_t partnum, uint8_t version, uint8_t version_again);

/* IOCFG0 as read back while receiving: true when the chip has lost its configuration (a reset
 * or brown-out: the register is back at its reset value, or anything but what the table set). */
bool cc1101_lost_config(uint8_t iocfg0);

#ifdef __cplusplus
}
#endif

#endif /* CC1101_REGS_H */
