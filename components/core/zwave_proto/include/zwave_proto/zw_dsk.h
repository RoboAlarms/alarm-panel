/*
 * zw_dsk.h - the DSK (device-specific key) and PIN as the installer reads and types them
 * (docs/features/36, security.md §4 "DSK, PIN and the SmartStart home IDs"): a DSK is the first 16
 * bytes of a joining device's X25519 public key, shown as 8 groups of 5 decimal digits - each
 * group one big-endian 16-bit word, zero-padded - joined by '-'; the PIN is the first group.
 */
#ifndef ZW_DSK_H
#define ZW_DSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "NNNNN-NNNNN-NNNNN-NNNNN-NNNNN-NNNNN-NNNNN-NNNNN" is 47 characters; +1 for the NUL. */
#define ZW_DSK_TEXT_SIZE 48u

/* Renders the 16-byte DSK as text into 'out'. False (writes nothing) when a pointer is NULL or
 * 'cap' < ZW_DSK_TEXT_SIZE. */
bool zw_dsk_to_text(const uint8_t dsk[16], char *out, size_t cap);

/* Strict parse of DSK text: exactly 8 groups of exactly 5 digits (each <= 65535), a single '-'
 * between groups, nothing else - no spaces, no short or long groups, no trailing text. False
 * leaves 'dsk' unchanged. */
bool zw_dsk_from_text(const char *text, uint8_t dsk[16]);

/* The PIN: the DSK's first group, bytes 0-1 big-endian. */
uint16_t zw_dsk_pin(const uint8_t dsk[16]);

/* Strict parse of a typed PIN: exactly 5 digits, <= 65535, nothing else. */
bool zw_pin_from_text(const char *text, uint16_t *pin);

/* The DSK as an installer types it from a label: 40 digits, with spaces or '-' anywhere (they
 * are ignored), each group of 5 <= 65535. Anything else - another character, 39 or 41 digits -
 * is refused and 'dsk' is left unchanged. */
bool zw_dsk_from_digits(const char *text, uint8_t dsk[16]);

/* SmartStart's home IDs from a DSK (security.md §4): the NWI home ID a device asks to join
 * with (bytes 8-11, bits 7-6 of the first set, bit 0 of the last cleared) and the Auth home ID
 * AddNodeToNetwork takes with it (bytes 12-15, bits 7-6 of the first cleared, bit 0 of the last
 * set). */
void zw_dsk_home_ids(const uint8_t dsk[16], uint8_t nwi[4], uint8_t auth[4]);

#ifdef __cplusplus
}
#endif

#endif /* ZW_DSK_H */
