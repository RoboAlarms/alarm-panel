/*
 * zw_dsk.c - DSK and PIN text (see zw_dsk.h). Digits are written and read by hand, not with
 * snprintf/strtoul: this keeps the parser strict (a library function would happily accept
 * leading '+', whitespace or overflow-wrapped numbers) and sidesteps the truncation warnings
 * snprintf draws from Linux GCC here.
 */
#include "zwave_proto/zw_dsk.h"

bool zw_dsk_to_text(const uint8_t dsk[16], char *out, size_t cap)
{
    if (dsk == NULL || out == NULL || cap < ZW_DSK_TEXT_SIZE) {
        return false;
    }
    size_t pos = 0u;
    for (unsigned g = 0u; g < 8u; g++) {
        if (g > 0u) {
            out[pos] = '-';
            pos++;
        }
        const unsigned word = ((unsigned)dsk[g * 2u] << 8) | (unsigned)dsk[g * 2u + 1u];
        unsigned place = 10000u;
        for (unsigned d = 0u; d < 5u; d++) {
            out[pos] = (char)('0' + (word / place) % 10u);
            pos++;
            place /= 10u;
        }
    }
    out[pos] = '\0';
    return true;
}

bool zw_dsk_from_text(const char *text, uint8_t dsk[16])
{
    if (text == NULL || dsk == NULL) {
        return false;
    }
    uint8_t parsed[16];
    size_t pos = 0u;
    for (unsigned g = 0u; g < 8u; g++) {
        if (g > 0u) {
            if (text[pos] != '-') {
                return false;
            }
            pos++;
        }
        unsigned value = 0u;
        for (unsigned d = 0u; d < 5u; d++) {
            const char c = text[pos];
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10u + (unsigned)(c - '0');
            pos++;
        }
        if (value > 65535u) {
            return false;
        }
        parsed[g * 2u] = (uint8_t)(value >> 8);
        parsed[g * 2u + 1u] = (uint8_t)value;
    }
    if (text[pos] != '\0') {
        return false;
    }
    for (size_t i = 0u; i < 16u; i++) {
        dsk[i] = parsed[i];
    }
    return true;
}

uint16_t zw_dsk_pin(const uint8_t dsk[16])
{
    return (uint16_t)(((unsigned)dsk[0] << 8) | (unsigned)dsk[1]);
}

bool zw_pin_from_text(const char *text, uint16_t *pin)
{
    if (text == NULL || pin == NULL) {
        return false;
    }
    unsigned value = 0u;
    for (unsigned d = 0u; d < 5u; d++) {
        const char c = text[d];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + (unsigned)(c - '0');
    }
    if (text[5] != '\0' || value > 65535u) {
        return false;
    }
    *pin = (uint16_t)value;
    return true;
}

bool zw_dsk_from_digits(const char *text, uint8_t dsk[16])
{
    if (text == NULL || dsk == NULL) {
        return false;
    }
    char digits[41];
    size_t n = 0u;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ' ' || *p == '-') {
            continue;
        }
        if (*p < '0' || *p > '9' || n == 40u) {
            return false;
        }
        digits[n] = *p;
        n++;
    }
    if (n != 40u) {
        return false;
    }
    uint8_t parsed[16];
    for (unsigned g = 0u; g < 8u; g++) {
        unsigned value = 0u;
        for (unsigned d = 0u; d < 5u; d++) {
            value = value * 10u + (unsigned)(digits[g * 5u + d] - '0');
        }
        if (value > 65535u) {
            return false;
        }
        parsed[g * 2u] = (uint8_t)(value >> 8);
        parsed[g * 2u + 1u] = (uint8_t)value;
    }
    for (size_t i = 0u; i < 16u; i++) {
        dsk[i] = parsed[i];
    }
    return true;
}

void zw_dsk_home_ids(const uint8_t dsk[16], uint8_t nwi[4], uint8_t auth[4])
{
    for (size_t i = 0u; i < 4u; i++) {
        nwi[i] = dsk[8u + i];
        auth[i] = dsk[12u + i];
    }
    nwi[0] = (uint8_t)((unsigned)nwi[0] | 0xC0u);
    nwi[3] = (uint8_t)((unsigned)nwi[3] & 0xFEu);
    auth[0] = (uint8_t)((unsigned)auth[0] & 0x3Fu);
    auth[3] = (uint8_t)((unsigned)auth[3] | 0x01u);
}
