#pragma once
/*
 * ds3231.h - the Maxim/Analog Devices DS3231 real-time clock as data (docs/features/35).
 *
 * The register file (0x00..0x12), BCD time in and out, the checks that tell a DS3231 from
 * something else answering at its address, and the control/status values the panel wants for a
 * clock that lives on its coin cell. components/drivers/rtc_ds3231 does the I2C with it.
 *
 * The module keeps UTC in 24-hour mode; the time zone is the panel's business (docs/features/09).
 * Portable C11: no heap, no globals, no ESP-IDF.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_I2C_ADDR 0x68u

enum {
    DS3231_REG_SECONDS = 0x00,
    DS3231_REG_MINUTES = 0x01,
    DS3231_REG_HOURS = 0x02,
    DS3231_REG_WEEKDAY = 0x03,
    DS3231_REG_DATE = 0x04,
    DS3231_REG_MONTH = 0x05, /* bit 7: century */
    DS3231_REG_YEAR = 0x06,
    DS3231_REG_CONTROL = 0x0E,
    DS3231_REG_STATUS = 0x0F,
    DS3231_REG_AGING = 0x10,
    DS3231_REG_TEMP_MSB = 0x11,
    DS3231_REG_TEMP_LSB = 0x12,
};
#define DS3231_TIME_LEN 7u  /* 0x00..0x06 */
#define DS3231_REG_COUNT 19u /* 0x00..0x12: everything the panel reads at once */

/* Control register (0x0E). */
#define DS3231_CTRL_EOSC 0x80u  /* 1: the oscillator stops when on the coin cell */
#define DS3231_CTRL_BBSQW 0x40u /* 1: square wave on the coin cell */
#define DS3231_CTRL_CONV 0x20u
#define DS3231_CTRL_RS2 0x10u
#define DS3231_CTRL_RS1 0x08u
#define DS3231_CTRL_INTCN 0x04u /* 1: INT/SQW is the alarm interrupt, not a square wave */
#define DS3231_CTRL_A2IE 0x02u
#define DS3231_CTRL_A1IE 0x01u

/* Status register (0x0F). Bits 6..4 always read 0. */
#define DS3231_STAT_OSF 0x80u     /* the oscillator stopped: the time is not to be trusted */
#define DS3231_STAT_EN32K 0x08u   /* the 32 kHz output is on */
#define DS3231_STAT_BSY 0x04u
#define DS3231_STAT_A2F 0x02u
#define DS3231_STAT_A1F 0x01u

/* The years the module can count (century bit 0: 2000-2099, 1: 2100-2199). Its leap-year rule
 * is right until 2100. */
#define DS3231_YEAR_MIN 2000
#define DS3231_YEAR_MAX 2199

/* A date and time in UTC. weekday: 1 = Monday .. 7 = Sunday (ISO 8601). */
typedef struct {
    int32_t year;
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..31 */
    uint8_t hour;   /* 0..23 */
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
} rtc_civil_t;

/* Days since 1970-01-01 of a proleptic Gregorian date (negative before). */
int64_t rtc_days_from_civil(int32_t year, unsigned month, unsigned day);
/* Days in a month (0 for a month outside 1..12). */
unsigned rtc_days_in_month(int32_t year, unsigned month);
void rtc_civil_from_epoch(int64_t epoch_s, rtc_civil_t *out);
/* false for an impossible date or time (February 30th, 24:00, second 60). */
bool rtc_epoch_from_civil(const rtc_civil_t *c, int64_t *epoch_s);

/* The seven time registers for a moment (24-hour mode, century bit, weekday); false outside
 * DS3231_YEAR_MIN..MAX. */
bool ds3231_encode_time(int64_t epoch_s, uint8_t out[DS3231_TIME_LEN]);
/* The moment the seven time registers hold. false when any digit is not BCD, a field is out
 * of range or the date does not exist. A clock another device left in 12-hour mode reads
 * correctly. The weekday register is not checked (nothing depends on it). */
bool ds3231_decode_time(const uint8_t in[DS3231_TIME_LEN], int64_t *epoch_s);

/* Whether the 19 registers read from address 0x68 come from a DS3231 (or its twin, the
 * DS3232): the bits the datasheet says always read 0 are 0 and the temperature is one a chip
 * can have. A DS1307 (RAM from 0x08), an MPU-6050 or anything else sharing the address fails. */
bool ds3231_identify(const uint8_t regs[DS3231_REG_COUNT]);

typedef struct {
    bool osc_stopped;        /* OSF: first power, or the coin cell ran flat */
    bool stops_on_battery;   /* EOSC: the clock would stop at the next power cut */
    bool out_32khz;          /* EN32kHz: costs coin-cell current */
    int16_t temp_c4;         /* the chip's temperature in quarter degrees Celsius */
    int8_t aging;            /* the aging offset register (left as the factory set it) */
} ds3231_status_t;
void ds3231_parse_status(const uint8_t regs[DS3231_REG_COUNT], ds3231_status_t *out);

/* The control register the panel wants: the oscillator keeps running on the coin cell, no
 * square wave, INT/SQW as the (unused) alarm interrupt with both alarm interrupts off. The
 * temperature conversion bits are kept. */
uint8_t ds3231_control_wanted(uint8_t control);
/* The status register the panel wants: the 32 kHz output off (it drains the coin cell), the
 * alarm flags cleared, and OSF cleared when clear_osf (the time has just been written). */
uint8_t ds3231_status_wanted(uint8_t status, bool clear_osf);

#ifdef __cplusplus
}
#endif
