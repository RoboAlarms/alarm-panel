/*
 * ds3231.c - the DS3231's registers: BCD time, identification, status and the control values
 * the panel wants (docs/features/35 RTC-002, RTC-007). Datasheet: Maxim DS3231 rev 10 (19-5170).
 */
#include "rtc_clock/ds3231.h"

#include <stddef.h>

/* ---- calendar ------------------------------------------------------------------------------ */

/* Howard Hinnant's days_from_civil / civil_from_days (public domain), in 64-bit. */
int64_t rtc_days_from_civil(int32_t year, unsigned month, unsigned day)
{
    const int64_t y = (int64_t)year - (month <= 2u ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                   /* [0, 399] */
    const int64_t mp = (int64_t)month + (month > 2u ? -3 : 9);            /* [0, 11] */
    const int64_t doy = (153 * mp + 2) / 5 + (int64_t)day - 1;            /* [0, 365] */
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

static bool leap(int32_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

unsigned rtc_days_in_month(int32_t year, unsigned month)
{
    static const uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1u || month > 12u) {
        return 0u;
    }
    return DAYS[month - 1u] + ((month == 2u && leap(year)) ? 1u : 0u);
}

void rtc_civil_from_epoch(int64_t epoch_s, rtc_civil_t *out)
{
    int64_t days = epoch_s / 86400;
    int64_t secs = epoch_s % 86400;
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    out->year = (int32_t)(yoe + era * 400 + (m <= 2 ? 1 : 0));
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->hour = (uint8_t)(secs / 3600);
    out->minute = (uint8_t)((secs / 60) % 60);
    out->second = (uint8_t)(secs % 60);
    /* 1970-01-01 was a Thursday (ISO 4). */
    int64_t wd = (days + 3) % 7;
    if (wd < 0) {
        wd += 7;
    }
    out->weekday = (uint8_t)(wd + 1);
}

bool rtc_epoch_from_civil(const rtc_civil_t *c, int64_t *epoch_s)
{
    if (c->month < 1u || c->month > 12u || c->day < 1u ||
        c->day > rtc_days_in_month(c->year, c->month) || c->hour > 23u || c->minute > 59u ||
        c->second > 59u) {
        return false;
    }
    *epoch_s = rtc_days_from_civil(c->year, c->month, c->day) * 86400 +
               (int64_t)c->hour * 3600 + (int64_t)c->minute * 60 + (int64_t)c->second;
    return true;
}

/* ---- BCD ----------------------------------------------------------------------------------- */

static uint8_t to_bcd(unsigned v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* false when either nibble is not a decimal digit */
static bool from_bcd(uint8_t b, unsigned *out)
{
    const unsigned hi = (unsigned)b >> 4, lo = (unsigned)b & 0x0Fu;
    if (hi > 9u || lo > 9u) {
        return false;
    }
    *out = hi * 10u + lo;
    return true;
}

bool ds3231_encode_time(int64_t epoch_s, uint8_t out[DS3231_TIME_LEN])
{
    rtc_civil_t c;
    rtc_civil_from_epoch(epoch_s, &c);
    if (c.year < DS3231_YEAR_MIN || c.year > DS3231_YEAR_MAX) {
        return false;
    }
    const bool century = c.year >= 2100;
    out[DS3231_REG_SECONDS] = to_bcd(c.second);
    out[DS3231_REG_MINUTES] = to_bcd(c.minute);
    out[DS3231_REG_HOURS] = to_bcd(c.hour); /* bit 6 clear: 24-hour mode */
    out[DS3231_REG_WEEKDAY] = c.weekday;
    out[DS3231_REG_DATE] = to_bcd(c.day);
    out[DS3231_REG_MONTH] = (uint8_t)(to_bcd(c.month) | (century ? 0x80u : 0u));
    out[DS3231_REG_YEAR] = to_bcd((unsigned)(c.year - (century ? 2100 : 2000)));
    return true;
}

bool ds3231_decode_time(const uint8_t in[DS3231_TIME_LEN], int64_t *epoch_s)
{
    unsigned sec, min, hour, day, month, year;
    /* The bits the datasheet says read 0 must: anything else is not a time the chip wrote. */
    if ((in[DS3231_REG_SECONDS] & 0x80u) != 0u || (in[DS3231_REG_MINUTES] & 0x80u) != 0u ||
        (in[DS3231_REG_HOURS] & 0x80u) != 0u || (in[DS3231_REG_DATE] & 0xC0u) != 0u ||
        (in[DS3231_REG_MONTH] & 0x60u) != 0u) {
        return false;
    }
    if (!from_bcd(in[DS3231_REG_SECONDS], &sec) || !from_bcd(in[DS3231_REG_MINUTES], &min) ||
        !from_bcd(in[DS3231_REG_DATE], &day) ||
        !from_bcd((uint8_t)(in[DS3231_REG_MONTH] & 0x1Fu), &month) ||
        !from_bcd(in[DS3231_REG_YEAR], &year)) {
        return false;
    }
    const uint8_t h = in[DS3231_REG_HOURS];
    if ((h & 0x40u) != 0u) { /* 12-hour mode: bit 5 is PM, hours 1..12 */
        if (!from_bcd((uint8_t)(h & 0x1Fu), &hour) || hour < 1u || hour > 12u) {
            return false;
        }
        hour = (hour % 12u) + (((h & 0x20u) != 0u) ? 12u : 0u);
    } else if (!from_bcd((uint8_t)(h & 0x3Fu), &hour)) {
        return false;
    }
    const rtc_civil_t c = {
        .year = (int32_t)year + (((in[DS3231_REG_MONTH] & 0x80u) != 0u) ? 2100 : 2000),
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)min,
        .second = (uint8_t)sec,
        .weekday = 0,
    };
    return rtc_epoch_from_civil(&c, epoch_s);
}

/* ---- identification and status ------------------------------------------------------------- */

bool ds3231_identify(const uint8_t regs[DS3231_REG_COUNT])
{
    /* Fixed-zero bits (datasheet, Figure 1 "Timekeeping Registers"): seconds, minutes and hours
     * bit 7, the weekday's bits 7..3, the date's bits 7..6, the month's bits 6..5, the status
     * register's bits 6..4 and the temperature LSB's bits 5..0. */
    if ((regs[DS3231_REG_SECONDS] & 0x80u) != 0u || (regs[DS3231_REG_MINUTES] & 0x80u) != 0u ||
        (regs[DS3231_REG_HOURS] & 0x80u) != 0u || (regs[DS3231_REG_WEEKDAY] & 0xF8u) != 0u ||
        (regs[DS3231_REG_DATE] & 0xC0u) != 0u || (regs[DS3231_REG_MONTH] & 0x60u) != 0u ||
        (regs[DS3231_REG_STATUS] & 0x70u) != 0u || (regs[DS3231_REG_TEMP_LSB] & 0x3Fu) != 0u) {
        return false;
    }
    /* Its operating range is -40..+85 C; indoors it reads something in between. */
    const int8_t t = (int8_t)regs[DS3231_REG_TEMP_MSB];
    return t >= -40 && t <= 85;
}

void ds3231_parse_status(const uint8_t regs[DS3231_REG_COUNT], ds3231_status_t *out)
{
    const uint8_t st = regs[DS3231_REG_STATUS];
    const uint8_t ctrl = regs[DS3231_REG_CONTROL];
    out->osc_stopped = (st & DS3231_STAT_OSF) != 0u;
    out->out_32khz = (st & DS3231_STAT_EN32K) != 0u;
    out->stops_on_battery = (ctrl & DS3231_CTRL_EOSC) != 0u;
    /* 10-bit two's complement, 0.25 C per step: MSB is the whole degrees, LSB bits 7..6. */
    const int16_t raw = (int16_t)((int16_t)(int8_t)regs[DS3231_REG_TEMP_MSB] * 4 +
                                  (int16_t)((unsigned)regs[DS3231_REG_TEMP_LSB] >> 6));
    out->temp_c4 = raw;
    out->aging = (int8_t)regs[DS3231_REG_AGING];
}

uint8_t ds3231_control_wanted(uint8_t control)
{
    return (uint8_t)((control & (DS3231_CTRL_CONV | DS3231_CTRL_RS2 | DS3231_CTRL_RS1)) |
                     DS3231_CTRL_INTCN);
}

uint8_t ds3231_status_wanted(uint8_t status, bool clear_osf)
{
    /* Writing 1 to OSF leaves it as it is; writing 0 clears it. The alarm flags clear the same
     * way, BSY is read-only and bits 6..4 are 0. */
    return (uint8_t)(clear_osf ? 0u : (status & DS3231_STAT_OSF));
}
