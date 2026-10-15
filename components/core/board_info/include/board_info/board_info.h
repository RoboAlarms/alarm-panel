#pragma once
/* board_info - what a board is, in data (docs/features/33 step 4, ADR-035).
 *
 * Every board component describes itself in a board_info_t: the chip, the screen, what it
 * carries, every pin its own parts use, and the places services may attach things (wired zone
 * terminals, the SD card, a 433 MHz receiver, a sensor radio, an I2C module, a Z-Wave radio). The app hands the description
 * to the services, so no service names a board. board_info_validate() refuses a description
 * that could harm the board. A pin someone chose in Kconfig is checked the same way: the app
 * copies the description, puts the chosen pins in and validates the copy before anything is
 * configured.
 *
 * Portable C11: no ESP-IDF, no heap. GPIO numbers are plain ints; -1 means "not wired". */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_NO_PIN (-1)

typedef enum {
    BOARD_CHIP_ESP32 = 1, /* classic ESP32 (D0WD, WROOM-32) */
    BOARD_CHIP_ESP32S3,
    BOARD_CHIP_ESP32P4,
} board_chip_t;

typedef enum { BOARD_TOUCH_NONE = 0, BOARD_TOUCH_CAPACITIVE, BOARD_TOUCH_RESISTIVE } board_touch_t;
typedef enum { BOARD_AUDIO_NONE = 0, BOARD_AUDIO_I2S, BOARD_AUDIO_DAC } board_audio_t;
typedef enum { BOARD_SD_NONE = 0, BOARD_SD_SDMMC_1BIT, BOARD_SD_SPI } board_sd_bus_t;

/* How a pin is used. */
enum {
    BOARD_PIN_OUT = 1u << 0,        /* the chip drives it */
    BOARD_PIN_IN = 1u << 1,         /* the chip reads it */
    BOARD_PIN_SHARED = 1u << 2,     /* a bus line several devices share (SPI SCK/MOSI/MISO) */
    BOARD_PIN_EXTERNAL = 1u << 3,   /* driven from outside the board at any time (a door
                                     * contact, a receiver's data line): never a strapping pin */
    BOARD_PIN_PULLUP = 1u << 4,     /* an input that needs a pull-up (a contact to GND) */
    BOARD_PIN_EXT_PULLUP = 1u << 5, /* ...and has one fitted outside the chip */
};

/* The bus a shared line belongs to (BRD-020): a controller of a kind. Two uses share a pin
 * only on the same bus and for the same signal; zero is "on no bus" and never shares. */
#define BOARD_BUS_NONE 0u
#define BOARD_BUS_SPI(host) ((uint8_t)(0x10u | ((unsigned)(host) & 0x0Fu))) /* SPI2 = 2 */
#define BOARD_BUS_I2C(port) ((uint8_t)(0x20u | ((unsigned)(port) & 0x0Fu))) /* I2C0 = 0 */

/* Which line of a bus a shared pin is. */
typedef enum {
    BOARD_SIG_NONE = 0,
    BOARD_SIG_SCK,
    BOARD_SIG_MOSI,
    BOARD_SIG_MISO,
    BOARD_SIG_SDA,
    BOARD_SIG_SCL,
} board_signal_t;

/* One pin a board part uses, for the validator and for messages. A BOARD_PIN_SHARED pin names
 * its bus and signal (BRD-020); any other pin leaves both zero. */
typedef struct {
    const char *use; /* "LCD SCK", "touch CS" */
    int16_t gpio;
    uint8_t flags;   /* BOARD_PIN_* */
    uint8_t bus;     /* BOARD_BUS_*: the bus a shared pin is on */
    uint8_t signal;  /* board_signal_t: its line of that bus */
} board_pin_t;

/* A pin a person can reach for an add-on of their own (BRD-018): a header pin straight to the
 * chip, as printed on the board. */
typedef struct {
    int16_t gpio;
    const char *label;     /* as printed: "IO2" */
    const char *connector; /* "J7" */
} board_reach_t;

/* A wired zone terminal: a contact between the GPIO and GND. */
typedef struct {
    int16_t gpio;
    bool external_pullup; /* a pull-up is fitted outside the chip (needed where it has none) */
} board_terminal_t;

typedef struct {
    board_sd_bus_t bus;
    int8_t spi_host;               /* BOARD_SD_SPI: the SPI host number (2 = SPI2, 3 = SPI3) */
    int16_t clk, cmd, d0;          /* BOARD_SD_SDMMC_1BIT */
    int16_t sck, mosi, miso, cs;   /* BOARD_SD_SPI */
} board_sd_t;

/* Where a CC1101 (SPI + GDO0) or a receiver module (data only) goes on this board. */
typedef struct {
    int8_t spi_host;
    int16_t sck, mosi, miso, cs, data;
    const char *where; /* "wireless-module socket" */
} board_rf_t;

/* Another place the board offers for the 433 MHz receiver, chosen at runtime (`rf place`,
 * docs/features/25 RF-019): a CC1101 on the 7"'s J7 header instead of its wireless-module socket,
 * which then stays free for the sensor radio. rf.data is the CC1101's GDO0 there. */
typedef struct {
    const char *id; /* short, for the console and NVS: "j7" */
    board_rf_t rf;
} board_rf_place_t;

typedef struct {
    int16_t tx, rx;    /* the panel's TX and RX */
    const char *where;
} board_uart_t;

/* The I2C connector add-on modules plug into (docs/features/35: the battery-backed clock). The bus
 * belongs to the board component, which hands it out with board_i2c_ext() (board_api.h). */
typedef struct {
    int16_t sda, scl;  /* BOARD_NO_PIN when the board has none */
    const char *where; /* "I2C connector (PH2.0, 4 pins)" */
    int8_t port;       /* its I2C controller: 0 for the board's own connector, a place's port
                        * once moved (the validator's bus, BRD-020) */
} board_i2c_t;

/* Another place for the add-on I2C modules, chosen at runtime (`rtc place`, docs/features/35
 * RTC-013): the 7"'s clock module on two J7 pins with an I2C controller of its own, away from
 * the touch screen's bus. The app creates that bus; nothing else of the board is on it. */
typedef struct {
    const char *id;    /* "j7" */
    int8_t port;       /* the I2C controller it has to itself */
    int16_t sda, scl;
    const char *where; /* "J7 header, IO27 (SDA) and IO28 (SCL)" */
} board_i2c_place_t;

/* A place the Z-Wave radio (a Zooz ZAC93 on a UART, docs/features/36 hardware.md §4) can connect
 * to, chosen at runtime (`zwave place`): the panel's TX (to the module's RX) and RX. */
typedef struct {
    const char *id;    /* short, for the console and NVS: "socket", "j7" */
    int16_t tx, rx;
    const char *where; /* "wireless-module socket" */
} board_zwave_place_t;

/* The Z-Wave radio as wired this start: off unless `wired`. A description leaves it off (zero);
 * board_zwave_place_on() puts it at a place and board_zwave_reset_on() adds its reset line. */
typedef struct {
    bool wired;
    const char *place; /* the place's id */
    int16_t tx, rx;
    int16_t reset;     /* the module's reset line (its hardware version 2), BOARD_NO_PIN: none */
    const char *where;
} board_zwave_t;

typedef struct {
    const char *id;       /* matches firmware/<id>/ and the simulator profile: "crowpanel_p4" */
    const char *name;     /* for people: "Elecrow CrowPanel Advanced 7\" (V1.1)" */
    board_chip_t chip;
    bool psram;           /* the module carries PSRAM */
    bool octal_psram;     /* ESP32-S3: octal PSRAM/flash also takes GPIO33-37 */
    uint16_t width, height; /* the screen as the UI draws it (landscape) */
    board_touch_t touch;
    board_audio_t audio;
    uint32_t audio_rate_hz; /* what its audio output plays: 0 = the sounder's default (32 kHz),
                             * else 8000..32000 in whole hundreds of Hz (snd_set_rate) */
    bool battery;         /* the panel can read a battery */

    const board_pin_t *pins; /* every pin the board's own parts use, connected or not (BRD-019) */
    size_t pin_count;

    const board_reach_t *reach; /* the pins a person can reach for their own add-ons (BRD-018) */
    size_t reach_count;

    const board_terminal_t *terminals; /* wired zone terminals offered (may be empty) */
    size_t terminal_count;
    const char *terminals_where;       /* "J7 header" */

    board_sd_t sd;
    board_rf_t rf;         /* all BOARD_NO_PIN when the board has no place for one */
    board_uart_t radio;    /* the sensor-radio socket's UART, or BOARD_NO_PIN */
    board_i2c_t i2c;       /* the add-on I2C connector, or BOARD_NO_PIN */
    board_zwave_t zwave;   /* the Z-Wave radio this start: off in a description */

    const board_rf_place_t *rf_places;   /* other places for the receiver (may be empty) */
    size_t rf_place_count;
    const board_i2c_place_t *i2c_places; /* other places for I2C add-ons (may be empty) */
    size_t i2c_place_count;
    const board_zwave_place_t *zwave_places; /* where a Z-Wave radio can connect (may be empty) */
    size_t zwave_place_count;
} board_info_t;

/* ---- validation ------------------------------------------------------------------------- */

typedef enum {
    BOARD_ISSUE_NONE = 0,
    BOARD_ISSUE_NO_SUCH_PIN,     /* the chip has no such GPIO */
    BOARD_ISSUE_RESERVED,        /* flash or PSRAM uses it */
    BOARD_ISSUE_INPUT_ONLY,      /* driven as an output, but the pin can only read */
    BOARD_ISSUE_NO_PULLUP,       /* needs a pull-up the pin doesn't have, none fitted */
    BOARD_ISSUE_STRAPPING,       /* an external signal could hold a strapping pin at reset */
    BOARD_ISSUE_IN_USE,          /* two uses of one pin that aren't a shared bus */
    BOARD_ISSUE_BAD_DESCRIPTION, /* a missing id/name/chip, an unknown bus kind, an audio
                                  * rate the sounder can't play */
    /* appended (BRD-020): never renumber */
    BOARD_ISSUE_OTHER_BUS,       /* two bus lines on one pin, but of different buses */
    BOARD_ISSUE_OTHER_SIGNAL,    /* one bus, but two of its signals on one pin (SCK and MOSI) */
} board_issue_code_t;

typedef struct {
    board_issue_code_t code;
    int16_t gpio;
    const char *use;   /* the use that found the problem */
    const char *other; /* BOARD_ISSUE_IN_USE: the use already holding the pin */
} board_issue_t;

/* Checks a description; writes up to max issues and returns how many there are in total
 * (0 = the description is safe). */
size_t board_info_validate(const board_info_t *info, board_issue_t *issues, size_t max);

/* Short text for an issue code ("reserved for flash or PSRAM"). */
const char *board_issue_text(board_issue_code_t code);

/* The 433 MHz receiver's data line moved to another pin the board offers outside: its own
 * receiver data pin or one of its wired zone terminals (a module wired to the J7 header,
 * `rf pin`, docs/features/25 RF-008). On success *out is the board as it will then be wired:
 * rf.data is gpio, and that terminal is no longer offered for zones (the remaining terminals
 * are copied to terms, which *out points at). Returns false for any other pin, or when terms
 * is too small. Validate *out with board_info_validate() before configuring anything. */
bool board_rf_data_on(const board_info_t *info, int16_t gpio, board_info_t *out,
                      board_terminal_t *terms, size_t max_terms);

/* The board's other receiver place called id ("j7"), or NULL. */
const board_rf_place_t *board_rf_place(const board_info_t *info, const char *id);

/* The receiver moved to the place called id (`rf place`, BRD-014): *out is the board as it
 * will then be wired, rf holding that place's pins, and every one of them that was a wired
 * zone terminal no longer offered (the remaining terminals are copied to terms). False for a
 * place the board doesn't offer, or when terms is too small. Validate *out before use. */
bool board_rf_place_on(const board_info_t *info, const char *id, board_info_t *out,
                       board_terminal_t *terms, size_t max_terms);

/* The board's other place for I2C add-ons called id, or NULL. */
const board_i2c_place_t *board_i2c_place(const board_info_t *info, const char *id);

/* The add-on I2C modules moved to the place called id (`rtc place`, BRD-015): *out has i2c
 * there, and a terminal it takes is no longer offered. The bus is the place's own controller
 * (board_i2c_place()->port). False for a place the board doesn't offer. Validate *out. */
bool board_i2c_place_on(const board_info_t *info, const char *id, board_info_t *out,
                        board_terminal_t *terms, size_t max_terms);

/* The board's place for the Z-Wave radio called id, or NULL. */
const board_zwave_place_t *board_zwave_place(const board_info_t *info, const char *id);

/* The Z-Wave radio at the place called id (`zwave place`, BRD-016): *out is the board as it will
 * then be wired - zwave wired there without a reset line, every wired zone terminal on its pins
 * no longer offered (the rest copied to terms), and a place on the sensor-radio socket's UART
 * takes that socket (radio becomes BOARD_NO_PIN: the module sits where the ESP32-H2 would). False
 * for a place the board doesn't offer, or when terms is too small. Validate *out: it refuses a
 * place whose pins another place in use holds (the clock module's J7 place, a receiver's). */
bool board_zwave_place_on(const board_info_t *info, const char *id, board_info_t *out,
                          board_terminal_t *terms, size_t max_terms);

/* Its reset line on one of the board's wired zone terminals (`zwave reset-pin`, BRD-017,
 * hardware.md §5): *out has zwave.reset = gpio and that terminal no longer offered, the way
 * board_rf_data_on() takes the receiver's data pin. False when the radio is not wired, for a pin
 * that is no terminal, or when terms is too small. Validate *out (the line is an output the
 * module pulls up: not on an input-only or a strapping pin). */
bool board_zwave_reset_on(const board_info_t *info, int16_t gpio, board_info_t *out,
                          board_terminal_t *terms, size_t max_terms);

/* ---- custom places (BRD-021) ----------------------------------------------------------------
 * A part on pins the person chose. Each function checks that every pin is one the board lets a
 * person reach (info->reach), that no pin is used twice in the call, and that none is the
 * board's own (pins, sd, i2c, radio) or held by another part already in info (the receiver, the
 * I2C modules, the Z-Wave radio; not the part being moved). A wired zone terminal may be taken:
 * it is no longer offered (the rest are copied to terms, which *out points at) - its zone keeps
 * its binding, and the caller shows it as Check later. *out is then validated as well. On false,
 * why says which GPIO and signal, *out is untouched, and the caller turns the part off: never
 * back to its default pins. why may be NULL. */

/* The 433 MHz receiver on pins the person chose: a CC1101 (sck, mosi, miso, cs and data, its
 * GDO0, on pins->spi_host, or the board's receiver host when that is negative) or a receiver
 * module (data only, the rest BOARD_NO_PIN). pins->where may be NULL ("your own pins"). */
bool board_rf_custom_on(const board_info_t *info, const board_rf_t *pins, board_info_t *out,
                        board_terminal_t *terms, size_t max_terms, char *why, size_t len);

/* The add-on I2C modules (the clock) on two pins the person chose, with an I2C controller of
 * their own: the port of the board's first I2C place (on the 7", its J7 place's). */
bool board_i2c_custom_on(const board_info_t *info, int16_t sda, int16_t scl, board_info_t *out,
                         board_terminal_t *terms, size_t max_terms, char *why, size_t len);

/* The Z-Wave radio on pins the person chose: the panel's TX and RX, and its reset line or
 * BOARD_NO_PIN for none. zwave.place is "custom". */
bool board_zwave_custom_on(const board_info_t *info, int16_t tx, int16_t rx, int16_t reset,
                           board_info_t *out, board_terminal_t *terms, size_t max_terms,
                           char *why, size_t len);

/* Whether gpio is a pin the board lets a person reach, and its entry (NULL when not). */
const board_reach_t *board_reach(const board_info_t *info, int16_t gpio);

/* The console's `key=value` arguments (BRD-022): argv[0..argc-1] each "key=value", the keys
 * from keys[0..nkeys-1], of which the first nrequired must be given; out[i] is keys[i]'s GPIO,
 * BOARD_NO_PIN for an optional key not given. Values are decimal GPIO numbers 0-63. False, with
 * why, for an argument that is not key=value, an unknown or repeated key, a missing required
 * key or a value that is not a GPIO number ("sck= is missing", "cs=x is not a GPIO number"). */
bool board_custom_parse(int argc, char **argv, const char *const *keys, size_t nkeys,
                        size_t nrequired, int16_t *out, char *why, size_t len);

/* Facts about a chip's pins, for the validator and the tests. */
bool board_chip_has_pin(board_chip_t chip, int16_t gpio);
bool board_chip_pin_can_output(board_chip_t chip, int16_t gpio);
bool board_chip_pin_has_pullup(board_chip_t chip, int16_t gpio);
bool board_chip_pin_is_strapping(board_chip_t chip, int16_t gpio);
bool board_chip_pin_is_reserved(board_chip_t chip, bool psram, bool octal_psram, int16_t gpio);
const char *board_chip_name(board_chip_t chip);

#ifdef __cplusplus
}
#endif
