/*
 * zw_cc_internal.c - the signed 1/2/3/4-byte codec every value-carrying report/set shares (see
 * zw_cc_internal.h). Written with explicit branches, not a cast of an out-of-range unsigned value
 * to signed, so it stays defined behaviour regardless of the two's-complement assumption C permits
 * but does not (pre-C23) require.
 */
#include "zw_cc_internal.h"

bool zwi_signed_decode(const uint8_t *p, uint8_t size, int32_t *out)
{
    if (p == NULL || out == NULL) {
        return false;
    }
    switch (size) {
    case 1u:
        *out = (p[0] < 0x80u) ? (int32_t)p[0] : (int32_t)p[0] - 256;
        return true;
    case 2u: {
        const uint32_t u = ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        *out = (u < 0x8000u) ? (int32_t)u : (int32_t)u - 65536;
        return true;
    }
    case 3u: {
        const uint32_t u = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        *out = (u < 0x800000u) ? (int32_t)u : (int32_t)u - 16777216; /* sign-extended */
        return true;
    }
    case 4u: {
        const uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                            (uint32_t)p[3];
        *out = (u < 0x80000000u) ? (int32_t)u : -(int32_t)(0xFFFFFFFFu - u) - 1;
        return true;
    }
    default:
        return false;
    }
}

bool zwi_signed_encode(uint8_t *out, uint8_t size, int32_t value)
{
    if (out == NULL) {
        return false;
    }
    switch (size) {
    case 1u:
        if (value < -128 || value > 127) {
            return false;
        }
        out[0] = (uint8_t)value; /* signed -> unsigned is well-defined modular wrap (C11 6.3.1.3p2) */
        return true;
    case 2u:
        if (value < -32768 || value > 32767) {
            return false;
        }
        out[0] = (uint8_t)((uint32_t)value >> 8);
        out[1] = (uint8_t)value;
        return true;
    case 3u:
        if (value < -8388608 || value > 8388607) {
            return false;
        }
        out[0] = (uint8_t)((uint32_t)value >> 16);
        out[1] = (uint8_t)((uint32_t)value >> 8);
        out[2] = (uint8_t)value;
        return true;
    case 4u: {
        const uint32_t u = (uint32_t)value;
        out[0] = (uint8_t)(u >> 24);
        out[1] = (uint8_t)(u >> 16);
        out[2] = (uint8_t)(u >> 8);
        out[3] = (uint8_t)u;
        return true;
    }
    default:
        return false;
    }
}
