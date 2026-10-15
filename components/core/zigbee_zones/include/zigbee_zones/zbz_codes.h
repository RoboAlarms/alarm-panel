/*
 * zbz_codes.h - install codes (docs/features/20 devices.md §6; ZB-015): typed or scanned on the
 * touchscreen or in the Panel Manager, checked, given to the trust center, and kept until their
 * device joins with them - then forgotten. An install code is optional (decided
 * 2026-10-02): any Zigbee 3.0 device may also join in the installer's window with the standard
 * key, and each device records which way it came in (zbz_registry.h ZBZ_F_JOIN).
 *
 * A code is 6, 8, 12 or 16 bytes followed by its CRC-16/X.25 (two bytes, low first): 8, 10, 14 or
 * 18 bytes as printed, in hex, often in groups of four. The console's `zigbee code` checks the
 * same way (zigbee_service.c); this is the portable copy the pages and the Panel Manager use.
 *
 * Stored with the registry (namespace ZBZ_NVS_NAMESPACE, key ZBZ_KEY_CODES):
 *   compat (1) | count u8 | count x (entry length u8 | ieee u64 | code length u8 | code bytes |
 *   added at u32)
 * Little-endian; a later format only appends fields inside an entry (its length byte covers them,
 * and a reader skips what it doesn't know). The trust center is
 * given every waiting code again at each start (whether the stack keeps them across a restart is
 * a bench question, docs/features/20 step 5).
 *
 * Built in src/zbz_codes.c, tested by test/test_zbz_codes.c with a frozen v1 blob
 * (codes_fixture_v1.h). Portable C11, caller-owned state, no heap.
 */
#ifndef ZBZ_CODES_H
#define ZBZ_CODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZBZ_CODES_MAX      16u /* codes waiting at once */
#define ZBZ_CODE_MAX       18u /* the longest code with its CRC */
#define ZBZ_KEY_CODES      "codes"
#define ZBZ_CODES_COMPAT   1u
#define ZBZ_CODES_BLOB_MAX (2u + ZBZ_CODES_MAX * (1u + 8u + 1u + ZBZ_CODE_MAX + 4u))

typedef enum {
    ZBZ_CODE_OK = 0,
    ZBZ_CODE_BAD_LENGTH,   /* not 8, 10, 14 or 18 bytes, or not hex */
    ZBZ_CODE_CHECKSUM,     /* its CRC doesn't match: a typo */
    ZBZ_CODE_BAD_ADDRESS,  /* not 16 hex digits, or all zero / all F */
    ZBZ_CODE_FULL,
    ZBZ_CODE_NOT_FOUND,
} zbz_code_result_t;

typedef struct {
    uint64_t ieee;
    uint8_t len;               /* with its CRC */
    uint8_t code[ZBZ_CODE_MAX];
    uint32_t added_at;         /* Unix seconds, 0 the clock wasn't set */
} zbz_code_t;

typedef struct {
    zbz_code_t codes[ZBZ_CODES_MAX];
    size_t count;
    bool dirty;
} zbz_codes_t;

/* CRC-16/X.25 (poly 0x1021 reflected, init and final xor 0xFFFF) over `len` bytes. */
uint16_t zbz_code_crc(const uint8_t *bytes, size_t len);
/* Hex text (spaces, dashes and colons ignored, either case) into a code with its CRC checked. */
zbz_code_result_t zbz_code_parse(const char *text, uint8_t out[ZBZ_CODE_MAX], uint8_t *len);
/* 16 hex digits (separators ignored, an optional 0x) into an address. */
zbz_code_result_t zbz_ieee_parse(const char *text, uint64_t *ieee);
/* A scanned QR code's text: the Zigbee 3.0 install-code form ("Z:<address>$I:<code>", optional
 * trailing %... fields) and a bare "<address> <code>" pair; anything else BAD_LENGTH. Which forms
 * real labels use is a bench question (testing.md §9). */
zbz_code_result_t zbz_code_parse_qr(const char *text, uint64_t *ieee, uint8_t out[ZBZ_CODE_MAX],
                                    uint8_t *len);

void zbz_codes_init(zbz_codes_t *c);
/* Kept (an address already there gets the new code). FULL when there is no room. */
zbz_code_result_t zbz_codes_add(zbz_codes_t *c, uint64_t ieee, const uint8_t *code, uint8_t len,
                                uint32_t now_s);
zbz_code_result_t zbz_codes_forget(zbz_codes_t *c, uint64_t ieee);
const zbz_code_t *zbz_codes_find(const zbz_codes_t *c, uint64_t ieee);

size_t zbz_codes_encode(const zbz_codes_t *c, uint8_t *out, size_t cap);
/* false when damaged or of a later compat (then empty: the trust center keeps what it has). */
bool zbz_codes_decode(zbz_codes_t *c, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_CODES_H */
