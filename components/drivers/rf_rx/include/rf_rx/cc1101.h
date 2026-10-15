/*
 * cc1101.h - a TI CC1101 transceiver as the panel's OOK receiver (docs/features/25, RF-001, RF-017).
 *
 * The chip is used in asynchronous serial mode: it demodulates whatever is on the air at the
 * tuned frequency and puts the raw bit line on GDO0, where the RMT peripheral captures it
 * (rf_capture.h). The panel never transmits. This file is the SPI half; what is written and how
 * answers are judged (the receive table, the frequency word, RSSI, genuine / clone / absent,
 * read-back, reset detection) is the host-tested components/core/cc1101_regs.
 *
 * Every configuration is read back register by register after it is written (a loose MOSI wire
 * or a clone that ignores a register is found at once, not on the air), and cc1101_check() tells
 * a chip that has reset under the panel (a brown-out puts GDO0 back to its 135 kHz clock) from one
 * that is still receiving, so the service configures it again.
 *
 * CS is driven by hand rather than by the SPI driver: the chip wants CS held low until its
 * MISO line drops (CHIP_RDYn) before the first clock, which matters after reset and sleep.
 */
#ifndef RF_RX_CC1101_H
#define RF_RX_CC1101_H

#include <stdbool.h>
#include <stdint.h>

#include "cc1101_regs/cc1101_regs.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host; /* SPI2_HOST on the CrowPanel's wireless socket */
    int mosi, miso, sck, cs; /* GPIOs */
} cc1101_pins_t;

typedef struct {
    cc1101_pins_t pins;
    spi_device_handle_t dev;
    bool bus_ours;      /* this driver initialised the bus (and frees it) */
    bool present;       /* the last probe found a CC1101 */
    cc1101_identity_t identity; /* genuine or a clone */
    uint8_t partnum;    /* 0x00 on a genuine part */
    uint8_t version;    /* 0x14 (or 0x04 on old silicon); clones report others */
    uint32_t freq_khz;  /* what cc1101_rx_ook() tuned to */
    uint32_t spi_errors;
    uint32_t verify_errors; /* configurations the chip did not read back as written */
    uint8_t bad_reg;        /* ... the last register that differed */
    uint32_t resets;        /* times the chip was found reset while receiving */
    uint32_t rssi_unstable; /* RSSI samples whose reads never agreed (skipped) */
} cc1101_t;

/* Opens the SPI bus and device (nothing on the air yet). The bus may already be initialised by
 * another driver on the same host; then it is shared. */
esp_err_t cc1101_open(cc1101_t *c, const cc1101_pins_t *pins);

/* Resets the chip and reads its identity. ESP_OK with c->present true when a CC1101 (or a
 * clone) answered; ESP_ERR_NOT_FOUND when the bus answered with nothing (no module, no power,
 * wrong wiring). */
esp_err_t cc1101_probe(cc1101_t *c);

/* Writes the OOK receiver table for the band, reads it back, calibrates and enters RX with the
 * demodulated line on GDO0. ESP_ERR_INVALID_RESPONSE when a register did not read back as
 * written (c->bad_reg). freq_khz within the chip's bands (433920 for the cheap sensors, 315000
 * and 345000 for the American ones). */
esp_err_t cc1101_rx_ook(cc1101_t *c, uint32_t freq_khz);

/* Is the chip still configured? *lost true when it has reset (IOCFG0 is not what the table set);
 * ESP_OK unless the bus failed. */
esp_err_t cc1101_check(cc1101_t *c, bool *lost);

esp_err_t cc1101_strobe(cc1101_t *c, uint8_t strobe);
/* reg 0x00-0x2E is a configuration register, 0x30-0x3D a status register. */
esp_err_t cc1101_read(cc1101_t *c, uint8_t reg, uint8_t *value);
esp_err_t cc1101_write(cc1101_t *c, uint8_t reg, uint8_t value);

/* MARCSTATE, 0xFF when the bus failed. */
uint8_t cc1101_marcstate(cc1101_t *c);
/* The received signal strength, only meaningful in RX. RSSI is read until two consecutive reads
 * agree (errata SWRZ020: an RSSI read while receiving can be torn, RF-017), at most
 * CC1101_RSSI_READS reads: CC1101_RSSI_STABLE with *dbm the value; CC1101_RSSI_UNSTABLE when none
 * agreed (no value - never shown as the RSSI - and the chip is still there; counted in
 * rssi_unstable): *dbm is then the lowest of the reads, a lower bound only, which still proves a
 * carrier when even it is at the jamming level (RF-005); CC1101_RSSI_BUS_ERROR when the bus failed
 * (*dbm untouched). */
cc1101_rssi_result_t cc1101_read_rssi(cc1101_t *c, int8_t *dbm);

#ifdef __cplusplus
}
#endif

#endif /* RF_RX_CC1101_H */
