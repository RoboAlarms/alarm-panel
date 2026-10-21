/*
 * rf_tx.h - the 433 MHz sirens' transmissions on a pin (docs/features/16 OUT-022): a train of
 * pulses played by the RMT peripheral, exact to the microsecond whatever the tasks around it do.
 * The pin is a CC1101's GDO0 while the chip is in TX (cc1101_tx_ook), or a transmitter module's
 * DATA (a WL102-341 / FS1000A: high = carrier). Between transmissions the pin is held low (no
 * carrier).
 */
#ifndef RF_RX_RF_TX_H
#define RF_RX_RF_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_TX_PULSE_HIGH  0x80000000u /* a pulse: this bit for a high level, the rest its width */
#define RF_TX_MAX_US      32767u      /* the longest pulse one RMT half-symbol holds at 1 us */

/* Plays `n` pulses (RF_TX_PULSE_HIGH | width_us; a longer one is cut to RF_TX_MAX_US) on `gpio`
 * and waits until the last has left, at most timeout_ms. The pin is an output held low before
 * and after. Blocking; the caller's task only. */
esp_err_t rf_tx_send(int gpio, const uint32_t *pulses, size_t n, uint32_t timeout_ms);

/* Holds the pin low (no carrier): a transmitter module's DATA between transmissions. */
esp_err_t rf_tx_idle(int gpio);

#ifdef __cplusplus
}
#endif

#endif /* RF_RX_RF_TX_H */
