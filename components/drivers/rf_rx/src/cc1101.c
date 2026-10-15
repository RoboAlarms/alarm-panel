/*
 * cc1101.c - the CC1101 over SPI (see cc1101.h).
 */
#include "rf_rx/cc1101.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "cc1101";

#define SPI_HZ        4000000 /* the chip takes 10 MHz single, 6.5 MHz burst: stay well under */
#define READY_WAIT_US 2000    /* CHIP_RDYn after CS goes low; the crystal needs ~1 ms after reset */
#define STATE_WAIT_US 5000    /* IDLE -> RX with a calibration takes ~1 ms */

#define HDR_READ  0x80u
#define HDR_BURST 0x40u

/* ---- the bus, one transaction at a time ------------------------------------------------------ */

/* CS low, then the chip's "ready" (MISO low); false when it never came (nothing there). */
static bool select(cc1101_t *c)
{
    gpio_set_level((gpio_num_t)c->pins.cs, 0);
    const int64_t until = esp_timer_get_time() + READY_WAIT_US;
    while (gpio_get_level((gpio_num_t)c->pins.miso) != 0) {
        if (esp_timer_get_time() > until) {
            gpio_set_level((gpio_num_t)c->pins.cs, 1);
            c->spi_errors++;
            return false;
        }
        esp_rom_delay_us(5);
    }
    return true;
}

static void deselect(cc1101_t *c)
{
    gpio_set_level((gpio_num_t)c->pins.cs, 1);
}

/* Sends 'n' bytes (n <= 4) and returns what came back at the same clocks. */
static esp_err_t exchange(cc1101_t *c, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = n * 8u;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    memcpy(t.tx_data, tx, n);
    if (!select(c)) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = spi_device_polling_transmit(c->dev, &t);
    deselect(c);
    if (err != ESP_OK) {
        c->spi_errors++;
        return err;
    }
    if (rx != NULL) {
        memcpy(rx, t.rx_data, n);
    }
    return ESP_OK;
}

esp_err_t cc1101_strobe(cc1101_t *c, uint8_t strobe)
{
    const uint8_t tx[1] = {strobe};
    return exchange(c, tx, NULL, 1u);
}

esp_err_t cc1101_read(cc1101_t *c, uint8_t reg, uint8_t *value)
{
    /* Status registers share addresses with the strobes: the burst bit tells them apart. */
    const uint8_t hdr = (uint8_t)(HDR_READ | (reg >= 0x30u ? HDR_BURST : 0u) | (reg & 0x3Fu));
    const uint8_t tx[2] = {hdr, 0x00u};
    uint8_t rx[2] = {0, 0};
    const esp_err_t err = exchange(c, tx, rx, 2u);
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

esp_err_t cc1101_write(cc1101_t *c, uint8_t reg, uint8_t value)
{
    const uint8_t tx[2] = {(uint8_t)(reg & 0x3Fu), value};
    return exchange(c, tx, NULL, 2u);
}

uint8_t cc1101_marcstate(cc1101_t *c)
{
    uint8_t v = 0xFFu;
    if (cc1101_read(c, CC1101_MARCSTATE, &v) != ESP_OK) {
        return 0xFFu;
    }
    return v & 0x1Fu;
}

static bool read_rssi_reg(void *ctx, uint8_t *value)
{
    return cc1101_read((cc1101_t *)ctx, CC1101_RSSI, value) == ESP_OK;
}

cc1101_rssi_result_t cc1101_read_rssi(cc1101_t *c, int8_t *dbm)
{
    /* Errata SWRZ020: RSSI read while the receiver runs can be torn: read until two consecutive
     * reads agree, within a budget; none agreeing is no value (RF-017, cc1101_rssi_settle()). */
    uint8_t raw = 0u;
    uint8_t lowest = 0x80u; /* -138 dBm, the register's weakest */
    const cc1101_rssi_result_t r =
        cc1101_rssi_settle(read_rssi_reg, c, CC1101_RSSI_READS, &raw, &lowest);
    if (r == CC1101_RSSI_STABLE) {
        *dbm = (int8_t)cc1101_rssi_dbm(raw); /* -138..-11 dBm */
    } else if (r == CC1101_RSSI_UNSTABLE) {
        c->rssi_unstable++;
        *dbm = (int8_t)cc1101_rssi_dbm(lowest); /* a lower bound, not a value (RF-005) */
    }
    return r;
}

/* ---- bring-up ------------------------------------------------------------------------------- */

esp_err_t cc1101_open(cc1101_t *c, const cc1101_pins_t *pins)
{
    memset(c, 0, sizeof(*c));
    c->pins = *pins;
    /* CS by hand (see the header): an output, high = deselected, before the bus is touched. */
    const gpio_config_t cs = {.pin_bit_mask = 1ull << pins->cs,
                              .mode = GPIO_MODE_OUTPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE,
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&cs), TAG, "cs");
    gpio_set_level((gpio_num_t)pins->cs, 1);

    const spi_bus_config_t bus = {.mosi_io_num = pins->mosi,
                                  .miso_io_num = pins->miso,
                                  .sclk_io_num = pins->sck,
                                  .quadwp_io_num = -1,
                                  .quadhd_io_num = -1,
                                  .max_transfer_sz = 64};
    /* No DMA: every transfer is a few bytes in the transaction itself, which also keeps every
     * buffer off the rules DMA has about where memory lives. */
    esp_err_t err = spi_bus_initialize(pins->host, &bus, SPI_DMA_DISABLED);
    if (err == ESP_OK) {
        c->bus_ours = true;
    } else if (err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE: already initialised, shared */
        return err;
    }
    const spi_device_interface_config_t dev = {.mode = 0,
                                               .clock_speed_hz = SPI_HZ,
                                               .spics_io_num = -1,
                                               .queue_size = 1};
    ESP_RETURN_ON_ERROR(spi_bus_add_device(pins->host, &dev, &c->dev), TAG, "device");
    /* MISO is read as a plain input for CHIP_RDYn; the SPI matrix keeps its own input. */
    gpio_set_direction((gpio_num_t)pins->miso, GPIO_MODE_INPUT);
    return ESP_OK;
}

/* Datasheet 19.1.1: the manual power-on reset, then SRES. */
static esp_err_t reset(cc1101_t *c)
{
    gpio_set_level((gpio_num_t)c->pins.cs, 1);
    esp_rom_delay_us(30);
    gpio_set_level((gpio_num_t)c->pins.cs, 0);
    esp_rom_delay_us(30);
    gpio_set_level((gpio_num_t)c->pins.cs, 1);
    esp_rom_delay_us(45);
    esp_err_t err = cc1101_strobe(c, CC1101_SRES);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(1500); /* the crystal, then the reset itself */
    if (!select(c)) {
        return ESP_ERR_NOT_FOUND;
    }
    deselect(c);
    return ESP_OK;
}

esp_err_t cc1101_probe(cc1101_t *c)
{
    c->present = false;
    esp_err_t err = reset(c);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t part = 0xFFu;
    uint8_t ver = 0xFFu;
    uint8_t ver2 = 0x00u;
    ESP_RETURN_ON_ERROR(cc1101_read(c, CC1101_PARTNUM, &part), TAG, "partnum");
    ESP_RETURN_ON_ERROR(cc1101_read(c, CC1101_VERSION, &ver), TAG, "version");
    ESP_RETURN_ON_ERROR(cc1101_read(c, CC1101_VERSION, &ver2), TAG, "version");
    c->partnum = part;
    c->version = ver;
    c->identity = cc1101_identify(part, ver, ver2);
    c->present = c->identity != CC1101_ABSENT;
    if (!c->present) {
        ESP_LOGW(TAG, "no CC1101 answered (partnum 0x%02X version 0x%02X)", part, ver);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "CC1101 found (version 0x%02X)%s", ver,
             c->identity == CC1101_GENUINE ? "" : ", not a TI version number: a clone");
    return ESP_OK;
}

static bool wait_state(cc1101_t *c, uint8_t want)
{
    const int64_t until = esp_timer_get_time() + STATE_WAIT_US;
    do {
        if (cc1101_marcstate(c) == want) {
            return true;
        }
        esp_rom_delay_us(100);
    } while (esp_timer_get_time() < until);
    return false;
}

esp_err_t cc1101_rx_ook(cc1101_t *c, uint32_t freq_khz)
{
    cc1101_reg_t table[CC1101_TABLE_MAX];
    const size_t n = cc1101_ook_table(freq_khz, table, CC1101_TABLE_MAX);
    if (n == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(cc1101_strobe(c, CC1101_SIDLE), TAG, "idle");
    if (!wait_state(c, CC1101_MARC_IDLE)) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(cc1101_write(c, table[i].reg, table[i].value), TAG, "table");
    }
    /* Read every register back (RF-017): a register the chip did not take is a wiring fault or
     * a clone that differs, found now rather than as a deaf receiver. */
    uint8_t read[CC1101_TABLE_MAX];
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(cc1101_read(c, table[i].reg, &read[i]), TAG, "read back");
    }
    const size_t bad = cc1101_verify(table, n, read);
    if (bad < n) {
        c->verify_errors++;
        c->bad_reg = table[bad].reg;
        ESP_LOGE(TAG, "register 0x%02X reads back 0x%02X, not 0x%02X: check SI/SO/SCLK/CSn",
                 table[bad].reg, read[bad], table[bad].value);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_RETURN_ON_ERROR(cc1101_strobe(c, CC1101_SCAL), TAG, "cal");
    esp_rom_delay_us(1000);
    ESP_RETURN_ON_ERROR(cc1101_strobe(c, CC1101_SFRX), TAG, "flush");
    ESP_RETURN_ON_ERROR(cc1101_strobe(c, CC1101_SRX), TAG, "rx");
    if (!wait_state(c, CC1101_MARC_RX)) {
        ESP_LOGW(TAG, "did not settle in RX (state %s)", cc1101_state_name(cc1101_marcstate(c)));
        return ESP_ERR_TIMEOUT;
    }
    c->freq_khz = freq_khz;
    ESP_LOGI(TAG, "receiving OOK at %lu.%03lu MHz", (unsigned long)(freq_khz / 1000u),
             (unsigned long)(freq_khz % 1000u));
    return ESP_OK;
}

esp_err_t cc1101_check(cc1101_t *c, bool *lost)
{
    uint8_t v = 0;
    const esp_err_t err = cc1101_read(c, CC1101_IOCFG0, &v);
    if (err != ESP_OK) {
        return err;
    }
    *lost = cc1101_lost_config(v);
    if (*lost) {
        c->resets++;
    }
    return ESP_OK;
}
