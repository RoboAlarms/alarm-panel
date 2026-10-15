/*
 * asset_pack.h - the strict reader of an asset pack's payload, `RAST` format 1 (docs/features/39
 * asset-packs.md "The pack file (format 1)", OPT-012): the letters, the saver's picture, the
 * certificates and later the Panel Manager's files, kept in the pack store as kinds 3-6.
 *
 * The byte layout is a stored contract: tools/asset_pack.py writes it and the Flasher's
 * roboflash/packstore.py (read_asset) reads it too. Never move or renumber a field; a format change
 * adds test/asset_pack_fixture_v2.h (asset_pack_fixture_v1.h is never edited).
 *
 * Portable C11: no heap, no globals. The reader is handed the payload (the file's first `payload
 * length` bytes, after ps_envelope() and the signature check) and checks only the format; the
 * kind's own rules (asset_pack_check_kind: its name, its parts, the picture's JPEG, the draft)
 * come after it, and the loader adds what depends on the firmware (the letters' fingerprint, the
 * certificates' version).
 */
#ifndef ASSET_PACK_H
#define ASSET_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pack_store/pack_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASSET_PACK_FORMAT      1u
#define ASSET_PACK_HEADER_SIZE 128u
#define ASSET_PACK_PART_SIZE   32u
#define ASSET_PACK_MAX_PARTS   16u
#define ASSET_PACK_NAME_MAX    15u /* a name, a version or a part's name: 1..15 characters */
#define ASSET_PACK_FLAG_DRAFT  0x01u

/* Part types (never renumber). */
enum {
    ASSET_PART_FONT = 1,  /* an LVGL binary font (lv_font_conv --format bin) */
    ASSET_PART_JPEG = 2,  /* a baseline JPEG */
    ASSET_PART_CERTS = 3  /* an ESP-IDF certificate bundle (gen_crt_bundle.py's format) */
};

typedef enum {
    ASSET_PACK_OK = 0,
    ASSET_PACK_ERR_ARG,          /* NULL, too long, not 4-byte aligned, or a big-endian machine */
    ASSET_PACK_ERR_SHORT,        /* shorter than the header, or than the header and part table */
    ASSET_PACK_ERR_MAGIC,        /* not `RAST` */
    ASSET_PACK_ERR_FORMAT,       /* format other than 1 or header size other than 128 */
    ASSET_PACK_ERR_LENGTH,       /* the payload length field differs from the bytes given */
    ASSET_PACK_ERR_KIND,         /* kind outside 3-6 */
    ASSET_PACK_ERR_FLAGS,        /* unknown flag bits */
    ASSET_PACK_ERR_COUNT,        /* part count 0 or over 16 */
    ASSET_PACK_ERR_NAME,         /* the pack's name: empty, unterminated, not printable, a space */
    ASSET_PACK_ERR_VERSION,      /* the version: the same rules */
    ASSET_PACK_ERR_PARTS_OFFSET, /* parts offset other than 128 */
    ASSET_PACK_ERR_RESERVED,     /* reserved bytes of the header or of a part record not 0 */
    ASSET_PACK_ERR_PART_NAME,    /* a part's name bad or repeated */
    ASSET_PACK_ERR_PART_TYPE,    /* a part's type 0 or unknown */
    ASSET_PACK_ERR_PART_SIZE,    /* a part's size 0 */
    ASSET_PACK_ERR_LAYOUT,       /* a part not where the canonical layout puts it, or past the end */
    ASSET_PACK_ERR_PADDING       /* an alignment byte between or after the parts not 0 */
} asset_pack_result_t;

typedef struct {
    const char *name;    /* NUL-terminated, inside the payload */
    uint32_t type;       /* ASSET_PART_* */
    const uint8_t *data; /* inside the payload, 4-byte aligned */
    uint32_t size;       /* at least 1 */
} asset_part_t;

typedef struct {
    const uint8_t *payload;
    uint32_t payload_length;
    uint8_t kind;        /* PS_KIND_LETTERS .. PS_KIND_WEB */
    uint8_t flags;
    bool draft;          /* made by a development build */
    const char *name;    /* the store entry's name, NUL-terminated, inside the payload */
    const char *version; /* the firmware version it was made with */
    uint32_t match;      /* the letters: the fonts' fingerprint; 0 for the others */
    uint16_t part_count; /* 1..16 */
    asset_part_t parts[ASSET_PACK_MAX_PARTS];
} asset_pack_t;

/* Checks the payload of an asset pack (unpadded, without the signature sector) and fills *out; the
 * payload must stay where it is for as long as out is used. Refuses everything in asset-packs.md
 * "A reader refuses", each with its result; on a refusal *out is zeroed. Reads nothing outside
 * [payload, payload + len); allocates nothing. */
asset_pack_result_t asset_pack_open(const void *payload, size_t len, asset_pack_t *out);

/* A short English name of a result, for logs ("ok", "layout", ...); never NULL. */
const char *asset_pack_result_name(asset_pack_result_t r);

/* The part at index i (0-based), or NULL past the count. */
const asset_part_t *asset_pack_part(const asset_pack_t *p, uint16_t i);

/* The part of this name, or NULL. */
const asset_part_t *asset_pack_find(const asset_pack_t *p, const char *name);

/* true when the pack was made by a development build (flag bit 0). */
bool asset_pack_is_draft(const asset_pack_t *p);

/* true when the pack was made with exactly this firmware version (the certificates rule, OPT-013).
 * NULL or an empty version never matches. */
bool asset_pack_made_for(const asset_pack_t *p, const char *version);

/* The store entry's name of an asset kind ("letters", "saver", "certs", "web"), or NULL for a kind
 * outside 3-6. */
const char *asset_pack_store_name(uint8_t kind);

/* ---- the kinds' rules (asset-packs.md "The kinds", OPT-006), on a pack asset_pack_open() took ---- */

#define ASSET_PICTURE_MAX_W 1024u /* the saver's photo: at most 1024 x 600 */
#define ASSET_PICTURE_MAX_H 600u

typedef enum {
    ASSET_RULE_OK = 0,
    ASSET_RULE_KIND,       /* the header's kind is not the store entry's */
    ASSET_RULE_NOT_TAKEN,  /* a kind the firmware does not take yet (the web pages) */
    ASSET_RULE_NAME,       /* the pack's or the store entry's name is not the kind's */
    ASSET_RULE_DRAFT,      /* a draft, and the firmware is a release */
    ASSET_RULE_NOT_FONTS,  /* letters: a part that is not a font */
    ASSET_RULE_NOT_PHOTO,  /* picture: not one `photo` JPEG part, or a match set */
    ASSET_RULE_PHOTO_SIZE, /* picture: not a baseline JPEG of at most 1024 x 600 */
    ASSET_RULE_NOT_BUNDLE  /* certificates: not one `bundle` certificates part, or a match set */
} asset_rule_t;

/* The kind's rules: `kind` the store entry's kind (letters, picture or certificates), `entry` its
 * name (NULL on install, where the name is the kind's), `release` true on a release firmware. A
 * letters part of a role the firmware does not know passes (it is skipped). The certificates'
 * version rule is the loader's (asset_pack_made_for): an installed pack may wait for its firmware. */
asset_rule_t asset_pack_check_kind(const asset_pack_t *p, uint8_t kind, const char *entry, bool release);

/* A short English reason for a refusal ("its name is not the kind's", ...); "" for ASSET_RULE_OK,
 * never NULL. */
const char *asset_rule_reason(asset_rule_t r);

/* A baseline JPEG (SOF0, which the saver's decoder takes) of at most max_w x max_h: the markers up
 * to the frame header, every length bounded; reads nothing outside [d, d + n). The full decode is
 * the saver's (a JPEG that does not decode there means the plain background). */
bool asset_jpeg_fits(const uint8_t *d, uint32_t n, uint32_t max_w, uint32_t max_h);

#ifdef __cplusplus
}
#endif

#endif /* ASSET_PACK_H */
