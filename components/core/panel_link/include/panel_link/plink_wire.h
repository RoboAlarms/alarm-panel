/*
 * plink_wire.h - little-endian byte writing and reading for the panel link's messages and stored
 * records. A writer or reader that runs out of room sets ok = false and does nothing more, so a
 * codec checks ok once at the end instead of after every field.
 */
#ifndef PLINK_WIRE_H
#define PLINK_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t len;
    bool ok;
} plink_w_t;

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    bool ok;
    bool pad; /* a section's reader: past the end reads zeros and ok stays true */
} plink_r_t;

void plink_w_init(plink_w_t *w, uint8_t *buf, size_t cap);
void plink_w_u8(plink_w_t *w, uint8_t v);
void plink_w_u16(plink_w_t *w, uint16_t v);
void plink_w_u32(plink_w_t *w, uint32_t v);
void plink_w_u64(plink_w_t *w, uint64_t v);
void plink_w_i64(plink_w_t *w, int64_t v);
void plink_w_bytes(plink_w_t *w, const void *p, size_t len);
/* A string: a length byte, then at most max bytes of s (cut on a whole UTF-8 character). */
void plink_w_str(plink_w_t *w, const char *s, size_t max);
/* Starts a section: writes a placeholder length byte and returns its offset for _end. */
size_t plink_w_section(plink_w_t *w);
/* Ends it: the length byte becomes the bytes written since (ok = false over 255). */
void plink_w_section_end(plink_w_t *w, size_t at);

void plink_r_init(plink_r_t *r, const uint8_t *buf, size_t len);
uint8_t plink_r_u8(plink_r_t *r);
uint16_t plink_r_u16(plink_r_t *r);
uint32_t plink_r_u32(plink_r_t *r);
uint64_t plink_r_u64(plink_r_t *r);
int64_t plink_r_i64(plink_r_t *r);
void plink_r_bytes(plink_r_t *r, void *out, size_t len);
/* A pointer to the next len bytes (NULL and ok = false when there are fewer). */
const uint8_t *plink_r_take(plink_r_t *r, size_t len);
/* A string written by plink_w_str into out (cap bytes, NUL-terminated): longer ones are cut on a
 * whole character, invalid UTF-8 and control or bidi-override characters are dropped
 * (docs/features/11 "Messages": a name from the network is shown on a screen). */
void plink_r_str(plink_r_t *r, char *out, size_t cap);
/* A section: a reader over its bytes; r moves past it. A section shorter than a reader expects
 * reads as zeros past its end (fields appended later default to 0, LINK-017). */
plink_r_t plink_r_section(plink_r_t *r);
/* Bytes left. */
size_t plink_r_left(const plink_r_t *r);

/* A copy of a string that is safe to show: valid UTF-8 only, no control or bidi-override
 * characters, at most cap - 1 bytes on a whole character. */
void plink_clean_text(const char *in, size_t in_len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_WIRE_H */
