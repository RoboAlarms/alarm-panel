/*
 * zw_cc_internal.h - a shared header check for every CC codec (private: not under include/, not
 * part of the public API). A command is `CC | command | parameters` (command-classes.md
 * conventions); every public zw_cc_<cc>_<cmd>_parse() still names its own CC/command literally,
 * so this only factors out the repeated "is this even the right command, and long enough" check.
 */
#ifndef ZW_CC_INTERNAL_H
#define ZW_CC_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* p != NULL, len >= min_len (which includes the CC/command bytes themselves), p[0] == cc,
 * p[1] == cmd. Bytes past min_len are never rejected here: trailing bytes are always the
 * caller's to accept (command-classes.md §1: "extra trailing bytes are allowed"). */
static inline bool zwi_cc_header_ok(const uint8_t *p, size_t len, uint8_t cc, uint8_t cmd, size_t min_len)
{
    return p != NULL && len >= min_len && min_len >= 2u && p[0] == cc && p[1] == cmd;
}

/* Decodes a big-endian signed integer of 1, 2, 3 or 4 bytes (Multilevel Sensor §4.4: all four;
 * Configuration §4.20: 1, 2 or 4, checked by its own parsers). Returns false for any other size. */
bool zwi_signed_decode(const uint8_t *p, uint8_t size, int32_t *out);
/* Encodes 'value' into 'size' (1, 2, 3 or 4) big-endian bytes. Returns false when 'value' does
 * not fit in 'size' bytes signed, or 'size' is not 1..4. */
bool zwi_signed_encode(uint8_t *out, uint8_t size, int32_t value);

#endif /* ZW_CC_INTERNAL_H */
