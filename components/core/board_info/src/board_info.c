#include "board_info/board_info.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- chip facts ----------------------------------------------------------------------------
 * Valid and output-capable pins are ESP-IDF v6.1's SOC_GPIO_VALID_GPIO_MASK and
 * SOC_GPIO_VALID_OUTPUT_GPIO_MASK (components/soc/<chip>/include/soc/soc_caps.h). Strapping,
 * flash and PSRAM pins are from the chips' datasheets. */

#define BIT64(n) (UINT64_C(1) << (n))
#define RANGE64(lo, hi) (((UINT64_C(1) << ((hi) - (lo) + 1)) - 1u) << (lo))

typedef struct {
    board_chip_t chip;
    const char *name;
    int16_t pin_count;
    uint64_t valid;
    uint64_t output;
    uint64_t pullup;       /* pins with an internal pull-up */
    uint64_t strapping;
    uint64_t flash;        /* always taken by the module's flash */
    uint64_t psram;        /* taken when the module carries PSRAM */
    uint64_t octal;        /* ESP32-S3: also taken by octal flash/PSRAM */
} chip_rules_t;

static const chip_rules_t CHIPS[] = {
    {
        .chip = BOARD_CHIP_ESP32,
        .name = "ESP32",
        .pin_count = 40,
        .valid = RANGE64(0, 39) & ~(BIT64(24) | RANGE64(28, 31)),
        /* GPIO34-39 are input only, without pull resistors */
        .output = (RANGE64(0, 39) & ~(BIT64(24) | RANGE64(28, 31))) & ~RANGE64(34, 39),
        .pullup = (RANGE64(0, 39) & ~(BIT64(24) | RANGE64(28, 31))) & ~RANGE64(34, 39),
        .strapping = BIT64(0) | BIT64(2) | BIT64(5) | BIT64(12) | BIT64(15),
        .flash = RANGE64(6, 11),
        .psram = BIT64(16) | BIT64(17), /* WROVER: PSRAM CS and clock */
        .octal = 0,
    },
    {
        .chip = BOARD_CHIP_ESP32S3,
        .name = "ESP32-S3",
        .pin_count = 49,
        .valid = RANGE64(0, 48) & ~RANGE64(22, 25),
        .output = RANGE64(0, 48) & ~RANGE64(22, 25),
        .pullup = RANGE64(0, 48) & ~RANGE64(22, 25),
        .strapping = BIT64(0) | BIT64(3) | BIT64(45) | BIT64(46),
        .flash = RANGE64(26, 32),     /* quad SPI flash and the PSRAM chip select */
        .psram = 0,
        .octal = RANGE64(33, 37),
    },
    {
        .chip = BOARD_CHIP_ESP32P4,
        .name = "ESP32-P4",
        .pin_count = 55,
        .valid = RANGE64(0, 54),
        .output = RANGE64(0, 54),
        .pullup = RANGE64(0, 54),
        .strapping = RANGE64(34, 38),
        .flash = 0, /* flash and PSRAM have their own pins */
        .psram = 0,
        .octal = 0,
    },
};

static const chip_rules_t *rules(board_chip_t chip)
{
    for (size_t i = 0; i < sizeof(CHIPS) / sizeof(CHIPS[0]); i++) {
        if (CHIPS[i].chip == chip) {
            return &CHIPS[i];
        }
    }
    return NULL;
}

static bool in_mask(const chip_rules_t *r, uint64_t mask, int16_t gpio)
{
    return r != NULL && gpio >= 0 && gpio < r->pin_count && (mask & BIT64(gpio)) != 0;
}

bool board_chip_has_pin(board_chip_t chip, int16_t gpio)
{
    const chip_rules_t *r = rules(chip);
    return r != NULL && in_mask(r, r->valid, gpio);
}

bool board_chip_pin_can_output(board_chip_t chip, int16_t gpio)
{
    const chip_rules_t *r = rules(chip);
    return r != NULL && in_mask(r, r->output, gpio);
}

bool board_chip_pin_has_pullup(board_chip_t chip, int16_t gpio)
{
    const chip_rules_t *r = rules(chip);
    return r != NULL && in_mask(r, r->pullup, gpio);
}

bool board_chip_pin_is_strapping(board_chip_t chip, int16_t gpio)
{
    const chip_rules_t *r = rules(chip);
    return r != NULL && in_mask(r, r->strapping, gpio);
}

bool board_chip_pin_is_reserved(board_chip_t chip, bool psram, bool octal_psram, int16_t gpio)
{
    const chip_rules_t *r = rules(chip);
    if (r == NULL) {
        return false;
    }
    uint64_t mask = r->flash;
    if (psram) {
        mask |= r->psram;
    }
    if (octal_psram) {
        mask |= r->octal;
    }
    return in_mask(r, mask, gpio);
}

const char *board_chip_name(board_chip_t chip)
{
    const chip_rules_t *r = rules(chip);
    return r != NULL ? r->name : "unknown chip";
}

const char *board_issue_text(board_issue_code_t code)
{
    switch (code) {
    case BOARD_ISSUE_NONE:
        return "fine";
    case BOARD_ISSUE_NO_SUCH_PIN:
        return "the chip has no such GPIO";
    case BOARD_ISSUE_RESERVED:
        return "reserved for flash or PSRAM";
    case BOARD_ISSUE_INPUT_ONLY:
        return "input-only pin used as an output";
    case BOARD_ISSUE_NO_PULLUP:
        return "needs a pull-up the pin doesn't have (fit one outside)";
    case BOARD_ISSUE_STRAPPING:
        return "an outside signal could hold this strapping pin at reset";
    case BOARD_ISSUE_IN_USE:
        return "already in use";
    case BOARD_ISSUE_BAD_DESCRIPTION:
        return "incomplete board description";
    case BOARD_ISSUE_OTHER_BUS:
        return "already a line of another bus";
    case BOARD_ISSUE_OTHER_SIGNAL:
        return "already another signal of the same bus";
    }
    return "unknown issue";
}

/* ---- validation ------------------------------------------------------------------------ */

/* Every pin a description uses, gathered once: its own parts and what it offers services. */
#define MAX_USES 96 /* the 7" uses about 40; 1.5 KB of stack while validating */

typedef struct {
    int16_t gpio;
    uint8_t flags;
    uint8_t bus;    /* BOARD_BUS_*, for a shared line */
    uint8_t signal; /* board_signal_t */
    const char *use;
} use_t;

typedef struct {
    use_t uses[MAX_USES];
    size_t count;
    board_issue_t *issues;
    size_t max;
    size_t found;
} ctx_t;

static void report(ctx_t *c, board_issue_code_t code, int16_t gpio, const char *use,
                   const char *other)
{
    if (c->found < c->max) {
        c->issues[c->found] = (board_issue_t){.code = code, .gpio = gpio, .use = use, .other = other};
    }
    c->found++;
}

static board_issue_code_t pin_issue(const board_info_t *info, int16_t gpio, uint8_t flags)
{
    if (!board_chip_has_pin(info->chip, gpio)) {
        return BOARD_ISSUE_NO_SUCH_PIN;
    }
    if (board_chip_pin_is_reserved(info->chip, info->psram, info->octal_psram, gpio)) {
        return BOARD_ISSUE_RESERVED;
    }
    if ((flags & BOARD_PIN_OUT) != 0 && !board_chip_pin_can_output(info->chip, gpio)) {
        return BOARD_ISSUE_INPUT_ONLY;
    }
    if ((flags & BOARD_PIN_PULLUP) != 0 && (flags & BOARD_PIN_EXT_PULLUP) == 0 &&
        !board_chip_pin_has_pullup(info->chip, gpio)) {
        return BOARD_ISSUE_NO_PULLUP;
    }
    if ((flags & BOARD_PIN_EXTERNAL) != 0 && board_chip_pin_is_strapping(info->chip, gpio)) {
        return BOARD_ISSUE_STRAPPING;
    }
    return BOARD_ISSUE_NONE;
}

/* What a second use of a pin clashes with (BRD-020): a pin is shared only by two bus lines of
 * the same bus and the same signal. */
static board_issue_code_t clash(const use_t *u, int16_t gpio, uint8_t flags, uint8_t bus,
                                uint8_t signal)
{
    if (u->gpio != gpio) {
        return BOARD_ISSUE_NONE;
    }
    if ((u->flags & BOARD_PIN_SHARED) == 0 || (flags & BOARD_PIN_SHARED) == 0) {
        return BOARD_ISSUE_IN_USE;
    }
    if (bus == BOARD_BUS_NONE || signal == BOARD_SIG_NONE || u->bus != bus) {
        return BOARD_ISSUE_OTHER_BUS;
    }
    return u->signal != signal ? BOARD_ISSUE_OTHER_SIGNAL : BOARD_ISSUE_NONE;
}

static void add_on(ctx_t *c, const board_info_t *info, int16_t gpio, uint8_t flags, uint8_t bus,
                   uint8_t signal, const char *use)
{
    if (gpio == BOARD_NO_PIN) {
        return;
    }
    if ((flags & BOARD_PIN_SHARED) == 0) {
        bus = BOARD_BUS_NONE;
        signal = BOARD_SIG_NONE;
    }
    const board_issue_code_t code = pin_issue(info, gpio, flags);
    if (code != BOARD_ISSUE_NONE) {
        report(c, code, gpio, use, NULL);
    }
    for (size_t i = 0; i < c->count; i++) {
        const board_issue_code_t why = clash(&c->uses[i], gpio, flags, bus, signal);
        if (why != BOARD_ISSUE_NONE) {
            report(c, why, gpio, use, c->uses[i].use);
            break;
        }
    }
    if (c->count < MAX_USES) {
        c->uses[c->count++] =
            (use_t){.gpio = gpio, .flags = flags, .bus = bus, .signal = signal, .use = use};
    } else {
        report(c, BOARD_ISSUE_BAD_DESCRIPTION, gpio, use, "too many pins");
    }
}

/* A pin on no bus. */
static void add(ctx_t *c, const board_info_t *info, int16_t gpio, uint8_t flags, const char *use)
{
    add_on(c, info, gpio, flags, BOARD_BUS_NONE, BOARD_SIG_NONE, use);
}

/* SPI bus lines: shared with whatever else sits on the same bus, line for line. */
#define SPI_OUT (BOARD_PIN_OUT | BOARD_PIN_SHARED)
#define SPI_IN (BOARD_PIN_IN | BOARD_PIN_SHARED)

static uint8_t spi_bus(int8_t host)
{
    return host >= 0 ? BOARD_BUS_SPI(host) : (uint8_t)BOARD_BUS_NONE;
}

static uint8_t i2c_bus(int8_t port)
{
    return port >= 0 ? BOARD_BUS_I2C(port) : (uint8_t)BOARD_BUS_NONE;
}

size_t board_info_validate(const board_info_t *info, board_issue_t *issues, size_t max)
{
    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.issues = issues;
    c.max = issues != NULL ? max : 0;

    /* The sounder's rates (components/core/sounder, snd_set_rate): a 10 ms block must be a
     * whole number of frames. */
    const bool rate_ok = info == NULL || info->audio_rate_hz == 0u ||
                         (info->audio_rate_hz >= 8000u && info->audio_rate_hz <= 32000u &&
                          info->audio_rate_hz % 100u == 0u);
    bool places_ok = info == NULL ||
                     ((info->rf_place_count == 0 || info->rf_places != NULL) &&
                      (info->i2c_place_count == 0 || info->i2c_places != NULL) &&
                      (info->zwave_place_count == 0 || info->zwave_places != NULL));
    for (size_t i = 0; places_ok && info != NULL && i < info->rf_place_count; i++) {
        places_ok = info->rf_places[i].id != NULL && info->rf_places[i].id[0] != '\0';
    }
    for (size_t i = 0; places_ok && info != NULL && i < info->i2c_place_count; i++) {
        places_ok = info->i2c_places[i].id != NULL && info->i2c_places[i].id[0] != '\0' &&
                    info->i2c_places[i].port >= 0;
    }
    for (size_t i = 0; places_ok && info != NULL && i < info->zwave_place_count; i++) {
        places_ok = info->zwave_places[i].id != NULL && info->zwave_places[i].id[0] != '\0';
    }
    /* the pins a person can reach (BRD-018): real pins, with their printed names */
    places_ok = places_ok && (info == NULL || info->reach_count == 0 || info->reach != NULL);
    for (size_t i = 0; places_ok && info != NULL && i < info->reach_count; i++) {
        places_ok = info->reach[i].label != NULL && info->reach[i].connector != NULL &&
                    board_chip_has_pin(info->chip, info->reach[i].gpio);
    }
    /* a Z-Wave radio in use has both its UART's pins */
    const bool zwave_ok = info == NULL || !info->zwave.wired ||
                          (info->zwave.tx != BOARD_NO_PIN && info->zwave.rx != BOARD_NO_PIN);
    if (info == NULL || info->id == NULL || info->name == NULL || rules(info->chip) == NULL ||
        info->sd.bus > BOARD_SD_SPI || (info->pin_count > 0 && info->pins == NULL) ||
        (info->terminal_count > 0 && info->terminals == NULL) || !rate_ok || !places_ok ||
        !zwave_ok) {
        report(&c, BOARD_ISSUE_BAD_DESCRIPTION, BOARD_NO_PIN,
               info != NULL && info->id != NULL ? info->id : "board description", NULL);
        return c.found;
    }

    for (size_t i = 0; i < info->pin_count; i++) {
        const board_pin_t *p = &info->pins[i];
        add_on(&c, info, p->gpio, p->flags, p->bus, p->signal, p->use);
    }
    for (size_t i = 0; i < info->terminal_count; i++) {
        const board_terminal_t *t = &info->terminals[i];
        const uint8_t flags = (uint8_t)(BOARD_PIN_IN | BOARD_PIN_EXTERNAL | BOARD_PIN_PULLUP |
                                        (t->external_pullup ? BOARD_PIN_EXT_PULLUP : 0));
        add(&c, info, t->gpio, flags, "wired zone terminal");
    }
    if (info->sd.bus == BOARD_SD_SDMMC_1BIT) {
        add(&c, info, info->sd.clk, BOARD_PIN_OUT, "SD CLK");
        add(&c, info, info->sd.cmd, BOARD_PIN_OUT | BOARD_PIN_IN, "SD CMD");
        add(&c, info, info->sd.d0, BOARD_PIN_OUT | BOARD_PIN_IN, "SD D0");
    } else if (info->sd.bus == BOARD_SD_SPI) {
        const uint8_t sd = spi_bus(info->sd.spi_host);
        add_on(&c, info, info->sd.sck, SPI_OUT, sd, BOARD_SIG_SCK, "SD SCK");
        add_on(&c, info, info->sd.mosi, SPI_OUT, sd, BOARD_SIG_MOSI, "SD MOSI");
        add_on(&c, info, info->sd.miso, SPI_IN, sd, BOARD_SIG_MISO, "SD MISO");
        add(&c, info, info->sd.cs, BOARD_PIN_OUT, "SD CS");
    }
    const uint8_t rf = spi_bus(info->rf.spi_host);
    add_on(&c, info, info->rf.sck, SPI_OUT, rf, BOARD_SIG_SCK, "RF SCK");
    add_on(&c, info, info->rf.mosi, SPI_OUT, rf, BOARD_SIG_MOSI, "RF MOSI");
    add_on(&c, info, info->rf.miso, SPI_IN, rf, BOARD_SIG_MISO, "RF MISO");
    add(&c, info, info->rf.cs, BOARD_PIN_OUT, "RF CS");
    add(&c, info, info->rf.data, BOARD_PIN_IN | BOARD_PIN_EXTERNAL, "RF data");
    add(&c, info, info->radio.tx, BOARD_PIN_OUT, "radio UART TX");
    add(&c, info, info->radio.rx, BOARD_PIN_IN | BOARD_PIN_EXTERNAL, "radio UART RX");
    /* The add-on connector's lines are a bus: shared with the board's own I2C parts. */
    const uint8_t i2c = i2c_bus(info->i2c.port);
    add_on(&c, info, info->i2c.sda, BOARD_PIN_OUT | BOARD_PIN_IN | BOARD_PIN_SHARED, i2c,
           BOARD_SIG_SDA, "I2C connector SDA");
    add_on(&c, info, info->i2c.scl, BOARD_PIN_OUT | BOARD_PIN_SHARED, i2c, BOARD_SIG_SCL,
           "I2C connector SCL");
    if (info->zwave.wired) {
        /* The module drives the panel's RX (idle high) and pulls its own reset line up
         * (hardware.md §5): both are outside signals a strapping pin must not see at reset. */
        add(&c, info, info->zwave.tx, BOARD_PIN_OUT, "Z-Wave radio TX");
        add(&c, info, info->zwave.rx, BOARD_PIN_IN | BOARD_PIN_EXTERNAL, "Z-Wave radio RX");
        add(&c, info, info->zwave.reset, BOARD_PIN_OUT | BOARD_PIN_EXTERNAL, "Z-Wave radio reset");
    }
    return c.found;
}

bool board_rf_data_on(const board_info_t *info, int16_t gpio, board_info_t *out,
                      board_terminal_t *terms, size_t max_terms)
{
    if (info == NULL || out == NULL || gpio == BOARD_NO_PIN ||
        (info->terminal_count > 0 && info->terminals == NULL)) {
        return false;
    }
    bool offered = gpio == info->rf.data;
    size_t kept = 0;
    for (size_t i = 0; i < info->terminal_count; i++) {
        if (info->terminals[i].gpio == gpio) {
            offered = true;
            continue;
        }
        if (terms == NULL || kept >= max_terms) {
            return false;
        }
        terms[kept++] = info->terminals[i];
    }
    if (!offered) {
        return false;
    }
    *out = *info;
    out->terminals = kept > 0 ? terms : NULL;
    out->terminal_count = kept;
    out->rf.data = gpio;
    return true;
}

/* *out = info without the terminals on any of pins; false when terms can't hold the rest. */
static bool without_terminals(const board_info_t *info, const int16_t *pins, size_t n,
                              board_info_t *out, board_terminal_t *terms, size_t max_terms)
{
    size_t kept = 0;
    for (size_t i = 0; i < info->terminal_count; i++) {
        bool taken = false;
        for (size_t p = 0; p < n; p++) {
            taken = taken || (pins[p] != BOARD_NO_PIN && info->terminals[i].gpio == pins[p]);
        }
        if (taken) {
            continue;
        }
        if (terms == NULL || kept >= max_terms) {
            return false;
        }
        terms[kept++] = info->terminals[i];
    }
    *out = *info;
    out->terminals = kept > 0 ? terms : NULL;
    out->terminal_count = kept;
    return true;
}

const board_rf_place_t *board_rf_place(const board_info_t *info, const char *id)
{
    if (info == NULL || id == NULL || (info->rf_place_count > 0 && info->rf_places == NULL)) {
        return NULL;
    }
    for (size_t i = 0; i < info->rf_place_count; i++) {
        if (info->rf_places[i].id != NULL && strcmp(info->rf_places[i].id, id) == 0) {
            return &info->rf_places[i];
        }
    }
    return NULL;
}

bool board_rf_place_on(const board_info_t *info, const char *id, board_info_t *out,
                       board_terminal_t *terms, size_t max_terms)
{
    const board_rf_place_t *place = board_rf_place(info, id);
    if (place == NULL || out == NULL || (info->terminal_count > 0 && info->terminals == NULL)) {
        return false;
    }
    const int16_t pins[] = {place->rf.sck, place->rf.mosi, place->rf.miso, place->rf.cs,
                            place->rf.data};
    if (!without_terminals(info, pins, sizeof(pins) / sizeof(pins[0]), out, terms, max_terms)) {
        return false;
    }
    out->rf = place->rf;
    return true;
}

const board_i2c_place_t *board_i2c_place(const board_info_t *info, const char *id)
{
    if (info == NULL || id == NULL || (info->i2c_place_count > 0 && info->i2c_places == NULL)) {
        return NULL;
    }
    for (size_t i = 0; i < info->i2c_place_count; i++) {
        if (info->i2c_places[i].id != NULL && strcmp(info->i2c_places[i].id, id) == 0) {
            return &info->i2c_places[i];
        }
    }
    return NULL;
}

bool board_i2c_place_on(const board_info_t *info, const char *id, board_info_t *out,
                        board_terminal_t *terms, size_t max_terms)
{
    const board_i2c_place_t *place = board_i2c_place(info, id);
    if (place == NULL || out == NULL || (info->terminal_count > 0 && info->terminals == NULL)) {
        return false;
    }
    const int16_t pins[] = {place->sda, place->scl};
    if (!without_terminals(info, pins, sizeof(pins) / sizeof(pins[0]), out, terms, max_terms)) {
        return false;
    }
    out->i2c = (board_i2c_t){
        .sda = place->sda, .scl = place->scl, .where = place->where, .port = place->port};
    return true;
}

const board_zwave_place_t *board_zwave_place(const board_info_t *info, const char *id)
{
    if (info == NULL || id == NULL || (info->zwave_place_count > 0 && info->zwave_places == NULL)) {
        return NULL;
    }
    for (size_t i = 0; i < info->zwave_place_count; i++) {
        if (info->zwave_places[i].id != NULL && strcmp(info->zwave_places[i].id, id) == 0) {
            return &info->zwave_places[i];
        }
    }
    return NULL;
}

/* Whether a pin is one of two (BOARD_NO_PIN never is). */
static bool one_of(int16_t gpio, int16_t a, int16_t b)
{
    return gpio != BOARD_NO_PIN && (gpio == a || gpio == b);
}

bool board_zwave_place_on(const board_info_t *info, const char *id, board_info_t *out,
                          board_terminal_t *terms, size_t max_terms)
{
    const board_zwave_place_t *place = board_zwave_place(info, id);
    if (place == NULL || out == NULL || (info->terminal_count > 0 && info->terminals == NULL)) {
        return false;
    }
    const int16_t pins[] = {place->tx, place->rx};
    if (!without_terminals(info, pins, sizeof(pins) / sizeof(pins[0]), out, terms, max_terms)) {
        return false;
    }
    out->zwave = (board_zwave_t){.wired = true,
                                 .place = place->id,
                                 .tx = place->tx,
                                 .rx = place->rx,
                                 .reset = BOARD_NO_PIN,
                                 .where = place->where};
    if (one_of(info->radio.tx, place->tx, place->rx) || one_of(info->radio.rx, place->tx, place->rx)) {
        /* the module takes the sensor-radio socket's UART: no ESP32-H2 there */
        out->radio.tx = BOARD_NO_PIN;
        out->radio.rx = BOARD_NO_PIN;
    }
    return true;
}

bool board_zwave_reset_on(const board_info_t *info, int16_t gpio, board_info_t *out,
                          board_terminal_t *terms, size_t max_terms)
{
    if (info == NULL || out == NULL || !info->zwave.wired || gpio == BOARD_NO_PIN ||
        (info->terminal_count > 0 && info->terminals == NULL)) {
        return false;
    }
    bool offered = false;
    size_t kept = 0;
    for (size_t i = 0; i < info->terminal_count; i++) {
        if (info->terminals[i].gpio == gpio) {
            offered = true;
            continue;
        }
        if (terms == NULL || kept >= max_terms) {
            return false;
        }
        terms[kept++] = info->terminals[i];
    }
    if (!offered) {
        return false;
    }
    *out = *info;
    out->terminals = kept > 0 ? terms : NULL;
    out->terminal_count = kept;
    out->zwave.reset = gpio;
    return true;
}

/* ---- custom places (BRD-021) ------------------------------------------------------------ */

static void say(char *why, size_t len, const char *fmt, ...)
{
    if (why == NULL || len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(why, len, fmt, ap);
    va_end(ap);
}

const board_reach_t *board_reach(const board_info_t *info, int16_t gpio)
{
    if (info == NULL || info->reach == NULL || gpio == BOARD_NO_PIN) {
        return NULL;
    }
    for (size_t i = 0; i < info->reach_count; i++) {
        if (info->reach[i].gpio == gpio) {
            return &info->reach[i];
        }
    }
    return NULL;
}

/* The part a custom call moves: its own current pins are not "another part". */
typedef enum { PART_RF, PART_I2C, PART_ZWAVE } part_t;

/* One pin of a custom call and the signal it carries, for the why texts. */
typedef struct {
    int16_t gpio;
    const char *signal; /* "MOSI", "SDA", "TX" */
} want_t;

/* Who already holds gpio on info, other than the part being moved: the board's own pins and
 * its other parts. A wired zone terminal is no holder (the caller takes it). NULL when free. */
static const char *held_by(const board_info_t *info, int16_t gpio, part_t moving)
{
    for (size_t i = 0; i < info->pin_count; i++) {
        if (info->pins[i].gpio == gpio) {
            return info->pins[i].use;
        }
    }
    const board_sd_t *sd = &info->sd;
    if ((sd->bus == BOARD_SD_SDMMC_1BIT && (gpio == sd->clk || gpio == sd->cmd || gpio == sd->d0)) ||
        (sd->bus == BOARD_SD_SPI &&
         (gpio == sd->sck || gpio == sd->mosi || gpio == sd->miso || gpio == sd->cs))) {
        return "the SD card";
    }
    if (one_of(gpio, info->radio.tx, info->radio.rx)) {
        return "the sensor radio's UART";
    }
    if (moving != PART_I2C && one_of(gpio, info->i2c.sda, info->i2c.scl)) {
        return "the I2C modules";
    }
    const board_rf_t *rf = &info->rf;
    if (moving != PART_RF && (one_of(gpio, rf->sck, rf->mosi) || one_of(gpio, rf->miso, rf->cs) ||
                              one_of(gpio, rf->data, BOARD_NO_PIN))) {
        return "the 433 MHz receiver";
    }
    const board_zwave_t *zw = &info->zwave;
    if (moving != PART_ZWAVE && zw->wired &&
        (one_of(gpio, zw->tx, zw->rx) || one_of(gpio, zw->reset, BOARD_NO_PIN))) {
        return "the Z-Wave radio";
    }
    return NULL;
}

/* "J7's IO pins only": where the reachable pins are, for a refusal. */
static void reach_hint(const board_info_t *info, char *buf, size_t len)
{
    const char *conn = info->reach[0].connector;
    bool one = true;
    bool io = true;
    for (size_t i = 0; i < info->reach_count; i++) {
        one = one && strcmp(info->reach[i].connector, conn) == 0;
        io = io && strncmp(info->reach[i].label, "IO", 2) == 0;
    }
    if (one) {
        (void)snprintf(buf, len, "%s's %spins only", conn, io ? "IO " : "");
    } else {
        (void)snprintf(buf, len, "the header pins only");
    }
}

/* The checks every custom call makes (BRD-021), on the pins given (BOARD_NO_PIN skipped). */
static bool custom_pins_ok(const board_info_t *info, const want_t *w, size_t n, part_t moving,
                           char *why, size_t len)
{
    if (info->reach_count == 0 || info->reach == NULL) {
        say(why, len, "this board has no pins for an add-on of your own");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (w[i].gpio != BOARD_NO_PIN && board_reach(info, w[i].gpio) == NULL) {
            char hint[48];
            reach_hint(info, hint, sizeof(hint));
            say(why, len, "GPIO %d is not a pin you can reach on this board (%s)", w[i].gpio, hint);
            return false;
        }
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (w[i].gpio != BOARD_NO_PIN && w[i].gpio == w[j].gpio) {
                say(why, len, "GPIO %d is used twice (%s and %s)", w[i].gpio, w[i].signal,
                    w[j].signal);
                return false;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        const char *by = w[i].gpio != BOARD_NO_PIN ? held_by(info, w[i].gpio, moving) : NULL;
        if (by != NULL) {
            say(why, len, "GPIO %d (%s) is already in use: %s", w[i].gpio, w[i].signal, by);
            return false;
        }
    }
    return true;
}

/* *built without the terminals on the call's pins, validated; copied to *out only when safe. */
static bool custom_finish(const board_info_t *info, const want_t *w, size_t n, board_info_t *built,
                          board_terminal_t *terms, size_t max_terms, char *why, size_t len)
{
    int16_t pins[5];
    for (size_t i = 0; i < n && i < 5; i++) {
        pins[i] = w[i].gpio;
    }
    const board_info_t moved = *built;
    if (!without_terminals(info, pins, n < 5 ? n : 5, built, terms, max_terms)) {
        say(why, len, "too many wired terminals to keep");
        return false;
    }
    /* without_terminals() copied info: put the part's new wiring back */
    built->rf = moved.rf;
    built->i2c = moved.i2c;
    built->zwave = moved.zwave;
    board_issue_t issue;
    if (board_info_validate(built, &issue, 1) != 0) {
        say(why, len, "GPIO %d (%s): %s%s%s", issue.gpio, issue.use != NULL ? issue.use : "?",
            board_issue_text(issue.code), issue.other != NULL ? ": " : "",
            issue.other != NULL ? issue.other : "");
        return false;
    }
    return true;
}

static bool custom_args_ok(const board_info_t *info, board_info_t *out, char *why, size_t len)
{
    if (info == NULL || out == NULL || (info->terminal_count > 0 && info->terminals == NULL) ||
        (info->pin_count > 0 && info->pins == NULL)) {
        say(why, len, "no board description");
        return false;
    }
    return true;
}

bool board_rf_custom_on(const board_info_t *info, const board_rf_t *pins, board_info_t *out,
                        board_terminal_t *terms, size_t max_terms, char *why, size_t len)
{
    if (!custom_args_ok(info, out, why, len)) {
        return false;
    }
    if (pins == NULL || pins->data == BOARD_NO_PIN) {
        say(why, len, "the receiver needs its data pin");
        return false;
    }
    const int spi_given = (pins->sck != BOARD_NO_PIN) + (pins->mosi != BOARD_NO_PIN) +
                          (pins->miso != BOARD_NO_PIN) + (pins->cs != BOARD_NO_PIN);
    if (spi_given != 0 && spi_given != 4) {
        say(why, len, "a CC1101 needs sck, mosi, miso, cs and data; a receiver module only data");
        return false;
    }
    const want_t w[] = {{pins->sck, "SCK"},
                        {pins->mosi, "MOSI"},
                        {pins->miso, "MISO"},
                        {pins->cs, "CS"},
                        {pins->data, "data"}};
    const size_t n = sizeof(w) / sizeof(w[0]);
    if (!custom_pins_ok(info, w, n, PART_RF, why, len)) {
        return false;
    }
    board_info_t built = *info;
    built.rf = *pins;
    built.rf.spi_host = pins->spi_host >= 0 ? pins->spi_host : info->rf.spi_host;
    built.rf.where = pins->where != NULL ? pins->where : "your own pins";
    if (!custom_finish(info, w, n, &built, terms, max_terms, why, len)) {
        return false;
    }
    *out = built;
    return true;
}

bool board_i2c_custom_on(const board_info_t *info, int16_t sda, int16_t scl, board_info_t *out,
                         board_terminal_t *terms, size_t max_terms, char *why, size_t len)
{
    if (!custom_args_ok(info, out, why, len)) {
        return false;
    }
    if (sda == BOARD_NO_PIN || scl == BOARD_NO_PIN) {
        say(why, len, "the I2C modules need sda and scl");
        return false;
    }
    if (info->i2c_place_count == 0 || info->i2c_places == NULL || info->i2c_places[0].port < 0) {
        say(why, len, "this board has no I2C controller for the modules on their own");
        return false;
    }
    const want_t w[] = {{sda, "SDA"}, {scl, "SCL"}};
    const size_t n = sizeof(w) / sizeof(w[0]);
    if (!custom_pins_ok(info, w, n, PART_I2C, why, len)) {
        return false;
    }
    board_info_t built = *info;
    built.i2c = (board_i2c_t){
        .sda = sda, .scl = scl, .where = "your own pins", .port = info->i2c_places[0].port};
    if (!custom_finish(info, w, n, &built, terms, max_terms, why, len)) {
        return false;
    }
    *out = built;
    return true;
}

bool board_zwave_custom_on(const board_info_t *info, int16_t tx, int16_t rx, int16_t reset,
                           board_info_t *out, board_terminal_t *terms, size_t max_terms,
                           char *why, size_t len)
{
    if (!custom_args_ok(info, out, why, len)) {
        return false;
    }
    if (tx == BOARD_NO_PIN || rx == BOARD_NO_PIN) {
        say(why, len, "the Z-Wave radio needs tx and rx");
        return false;
    }
    const want_t w[] = {{tx, "TX"}, {rx, "RX"}, {reset, "reset"}};
    const size_t n = sizeof(w) / sizeof(w[0]);
    if (!custom_pins_ok(info, w, n, PART_ZWAVE, why, len)) {
        return false;
    }
    board_info_t built = *info;
    built.zwave = (board_zwave_t){.wired = true,
                                  .place = "custom",
                                  .tx = tx,
                                  .rx = rx,
                                  .reset = reset,
                                  .where = "your own pins"};
    if (!custom_finish(info, w, n, &built, terms, max_terms, why, len)) {
        return false;
    }
    *out = built;
    return true;
}

/* ---- the console's key=value arguments (BRD-022) ------------------------------------------ */

#define UNSEEN INT16_MIN

bool board_custom_parse(int argc, char **argv, const char *const *keys, size_t nkeys,
                        size_t nrequired, int16_t *out, char *why, size_t len)
{
    if (argc < 0 || (argc > 0 && argv == NULL) || (nkeys > 0 && keys == NULL) ||
        (nkeys > 0 && out == NULL) || nrequired > nkeys) {
        say(why, len, "bad arguments");
        return false;
    }
    for (size_t k = 0; k < nkeys; k++) {
        out[k] = UNSEEN;
    }
    for (int a = 0; a < argc; a++) {
        const char *arg = argv[a] != NULL ? argv[a] : "";
        const char *eq = strchr(arg, '=');
        if (eq == NULL || eq == arg) {
            say(why, len, "%s is not key=value", arg);
            return false;
        }
        const size_t klen = (size_t)(eq - arg);
        size_t k = 0;
        while (k < nkeys && !(keys[k] != NULL && strlen(keys[k]) == klen &&
                              strncmp(keys[k], arg, klen) == 0)) {
            k++;
        }
        if (k == nkeys) {
            char known[96] = "";
            size_t used = 0;
            for (size_t i = 0; i < nkeys && used < sizeof(known); i++) {
                const int w = snprintf(known + used, sizeof(known) - used, "%s%s=",
                                       i > 0 ? " " : "", keys[i] != NULL ? keys[i] : "?");
                used += w > 0 ? (size_t)w : 0u;
            }
            say(why, len, "%.*s= is not one of: %s", (int)klen, arg, known);
            return false;
        }
        if (out[k] != UNSEEN) {
            say(why, len, "%s= is given twice", keys[k]);
            return false;
        }
        const char *v = eq + 1;
        int value = 0;
        bool ok = *v != '\0';
        for (; ok && *v != '\0'; v++) {
            ok = *v >= '0' && *v <= '9';
            value = ok ? value * 10 + (*v - '0') : value;
            ok = ok && value <= 63;
        }
        if (!ok) {
            say(why, len, "%s is not a GPIO number", arg);
            return false;
        }
        out[k] = (int16_t)value;
    }
    for (size_t k = 0; k < nkeys; k++) {
        if (out[k] == UNSEEN) {
            if (k < nrequired) {
                say(why, len, "%s= is missing", keys[k]);
                return false;
            }
            out[k] = BOARD_NO_PIN;
        }
    }
    return true;
}
