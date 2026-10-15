/*
 * rf_service.h - the panel's 433 MHz sensor receiver (docs/features/25): a CC1101 on the wireless
 * socket's SPI, or a fixed superheterodyne receiver module (RX470-4 / WL101-341, the Amazon
 * "433 MHz transmitter and receiver kits") on the data pin alone; the data line captured by the
 * RMT peripheral, decoded (components/core/rf_decode)
 * and handed to the portable zsrc_rf source, which is what the zone hub and the zone editor see.
 *
 * Nothing here is on the alarm task: the service has its own, and reaches the hub through
 * zone_service's lock like every other source. A missing receiver is not a failure: the
 * service probes the bus every RF_SERVICE_PROBE_MS so a module wired later is found without a
 * restart, and the source is "usable" (offered in the picker) only while a receiver answers.
 */
#ifndef RF_SERVICE_H
#define RF_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "rf_decode/rf_decode.h"
#include "zone_hub/zsrc_rf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_SERVICE_PROBE_MS 30000u /* how often an empty bus is asked again */

/* Which receiver the panel may use (RF-001). Stored numbers (NVS "rf"/"mode"): never reorder. */
typedef enum {
    RF_MODULE_AUTO = 0,   /* a CC1101 if one answers on SPI, else a module on the data pin */
    RF_MODULE_CC1101 = 1, /* only the CC1101 */
    RF_MODULE_OOK = 2,    /* only a fixed receiver module (RX470-4, WL101-341, RXB6, SRX882) */
    RF_MODULE_OFF = 3,    /* none fitted: no pin is touched, nothing is probed (RF-020) */
} rf_module_t;

typedef struct {
    spi_host_device_t host;
    int mosi, miso, sck, cs; /* the CC1101's SPI */
    int gdo0;                /* the demodulated line, into the RMT peripheral */
    bool invert;             /* the line idles high on this module */
    uint32_t freq_khz;       /* the band, unless NVS remembers another (`rf freq`) */
    rf_module_t module;      /* unless NVS remembers another (`rf module`) */
    const char *place;       /* the board place it is wired at (`rf place`), NULL: its own */
    const char *where;       /* ... in words: "J7 header" */
    bool custom;             /* place "custom" (RF-025): module is the place's (cc1101 or ook),
                              * the stored mode (kept "off" for older firmware) is not read */
} rf_service_config_t;

typedef struct {
    bool enabled;       /* the service runs */
    uint8_t module;     /* rf_module_t allowed */
    const char *receiver; /* what receives now: "CC1101", "receiver module", "none" */
    bool present;       /* a receiver is there */
    uint32_t edges_per_s; /* on the data pin, last second */
    uint8_t duty_pct;     /* ... and how much of it the line was high */
    uint32_t storms;      /* times the capture paused for a flood of edges */
    uint8_t version;    /* its VERSION register */
    uint8_t marcstate;  /* its state machine (0x0D = receiving) */
    const char *state;  /* ... in words */
    int8_t rssi_dbm;    /* the last sample between frames */
    uint32_t freq_khz;
    uint32_t spi_errors;
    uint32_t receptions, symbols, overruns;      /* the capture */
    uint32_t pulses, frames, rejected, glitches; /* the decoder */
    uint32_t transmissions, unknown, bursts;     /* the source */
    uint32_t confirmed;                          /* transmissions heard twice (RF-009) */
    uint32_t replays;                            /* rolling codes heard again (RF-016) */
    bool learning;
    bool jammed;
    bool have_last;      /* a transmission has been confirmed since start */
    rf_packet_t last;    /* ... the last one */
    uint64_t last_at_ms; /* alarm time of it */
    char words[96];      /* one line for Settings and the console */
} rf_service_status_t;

/* Where the data line may go (RF-008): the app knows the board and the zones, the service
 * only asks. Given before rf_service_start, and even when the receiver stays off, so that
 * `rf pin default` can always put a bad choice right. */
/* The receiver on pins the person chose (`rf place custom`, RF-025; NVS rf/c_sck, c_mosi,
 * c_miso, c_cs, c_gdo0 for a CC1101, rf/c_data for a receiver module). */
typedef struct {
    bool cc1101;                     /* else a receiver module on data alone */
    int16_t sck, mosi, miso, cs;     /* a CC1101's SPI, -1 for a receiver module */
    int16_t data;                    /* its GDO0, or the module's data line */
} rf_service_custom_t;

typedef enum {
    RF_CUSTOM_NONE = 0, /* the stored place is not "custom" */
    RF_CUSTOM_STORED,   /* it is, and its pins read back */
    RF_CUSTOM_DAMAGED,  /* it is, but its pins are missing, mixed or out of range */
} rf_custom_state_t;

typedef struct {
    int16_t board_data;      /* the board's own place for the data line */
    const char *board_where; /* ... in words: "wireless-module socket" */
    /* May the data line move to gpio? false with the reason in why. NULL: `rf pin` refuses. */
    bool (*pin_ok)(int16_t gpio, char *why, size_t len);
    /* The board's other places for the whole receiver (RF-019), for `rf place`: one line per
     * place, "j7: J7 header (SCK IO52, ...)"; "" or NULL when it has none. */
    const char *places;
    /* May the receiver move to the place called id? false with the reason in why. */
    bool (*place_ok)(const char *id, char *why, size_t len);
    /* May the receiver go on these pins of the person's own (RF-025)? NULL: refused. */
    bool (*custom_ok)(const rf_service_custom_t *pins, char *why, size_t len);
    /* Lines `rf` and `rf place` print this start, NULL when none: why a stored custom place
     * was not used ("custom place not used: ..."), and a warning about the stored pins. */
    const char *custom_why;
    const char *warning;
} rf_service_pins_t;

void rf_service_pin_rules(const rf_service_pins_t *rules);
/* The data pin chosen with `rf pin` (NVS "rf"/"pin"), or -1 for the board's own. The app reads
 * it before starting the zone sources, so a wired terminal the receiver takes is not offered. */
int16_t rf_service_stored_pin(void);
/* The place chosen with `rf place` (NVS "rf"/"place"), copied into out: false for the board's
 * own place. Read before the zone sources start, like the pin: a place takes terminals. */
bool rf_service_stored_place(char *out, size_t len);
/* The pins stored with `rf place custom` (RF-025), into *out when RF_CUSTOM_STORED. */
rf_custom_state_t rf_service_stored_custom(rf_service_custom_t *out);

esp_err_t rf_service_start(const rf_service_config_t *cfg);
void rf_service_status(rf_service_status_t *out);
/* Ask the bus again now (a module was just wired). */
void rf_service_retry(void);
/* Remember it across restarts (NVS "rf"/"khz"), then retune. ESP_ERR_INVALID_ARG outside the
 * chip's bands; the NVS error when it could not be stored - then nothing changes, the receiver
 * stays on the frequency it had (RF11). */
esp_err_t rf_service_set_frequency(uint32_t khz);
/* Which receiver to use, from the next start (NVS "rf"/"mode"). While the place is custom
 * (RF-025) only RF_MODULE_OFF is taken, and it takes the custom place away; any other is
 * ESP_ERR_INVALID_STATE. */
esp_err_t rf_service_set_module(rf_module_t module);
/* Log every transmission heard, known sensor or not: the bench's ears. */
void rf_service_raw(bool on);
/* The portable source (NULL before start), for the PC simulator's counterpart and tests. */
zsrc_rf_t *rf_service_source(void);
/* The `rf` console command (status, retry, freq, module, pin, place, raw, capture). */
esp_err_t rf_service_console_register(void);

#ifdef __cplusplus
}
#endif

#endif /* RF_SERVICE_H */
