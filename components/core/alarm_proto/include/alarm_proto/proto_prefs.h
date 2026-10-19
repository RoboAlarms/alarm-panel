/*
 * proto_prefs.h - a panel's Screen and sound settings as fields: on the panel link (PREFS,
 * PREFS_SET, HEARTBEAT's prefs_crc; docs/features/11 "Settings fields", LINK-050..052) and in
 * the Home Assistant link's JSON (settings, keypads, settings_set; docs/features/23 HAI-017).
 *
 * One table serves both: each field has a frozen id, a wire size, a range, a JSON key and
 * whether it changes how the screen looks. On the link a field is a TLV (id u8, len u8, value
 * little-endian); a list of them (count u8, then the TLVs, in id order when this code writes
 * them) is the body of a PREFS or PREFS_SET fields section. A decoder skips an id it doesn't
 * know by its length, and refuses a known id with the wrong length or an out-of-range value
 * for that field alone.
 *
 * The ids, sizes, ranges, JSON keys and why numbers are an external contract between firmware
 * versions and with the Home Assistant integration: never renumber, reuse or rename, only
 * append (components/core/AGENTS.md). Every JSON key is 11 bytes or less, forever.
 *
 * The fields mirror platform_esp's panel_prefs_t (named in the comments; alarm_proto doesn't
 * depend on it). Never a settings field: codes, role, pairing, home partition, name,
 * supervision, Wi-Fi and IP, time and time zone, language and region, updates, factory reset,
 * the setup, the Panel Manager, developer access (LINK-052).
 *
 * Portable C11, no heap, no globals.
 */
#ifndef PROTO_PREFS_H
#define PROTO_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Field ids: the TLV's id, bit id - 1 in a mask. Never renumbered or reused. */
typedef enum {
    PROTO_PREFS_F_BRIGHTNESS = 1,        /* brightness, u8 10..100 */
    PROTO_PREFS_F_SAVER_BRIGHTNESS = 2,  /* saver_brightness, u8 0..50 */
    PROTO_PREFS_F_SAVER_AFTER = 3,       /* saver_after_ms, u32: 0 (never), or 10 s..24 h */
    PROTO_PREFS_F_OFF_AFTER = 4,         /* off_after_ms, u32, as 3 */
    PROTO_PREFS_F_NIGHT_ON = 5,          /* night_on, u8 0/1 */
    PROTO_PREFS_F_NIGHT_START = 6,       /* night_start, u16 minutes after midnight 0..1439 */
    PROTO_PREFS_F_NIGHT_END = 7,         /* night_end, u16 0..1439 */
    PROTO_PREFS_F_NIGHT_BRIGHTNESS = 8,  /* night_brightness, u8 10..100 */
    PROTO_PREFS_F_NIGHT_OFF_AFTER = 9,   /* night_off_after_ms, u32, as 3 */
    PROTO_PREFS_F_CLOCK_MODE = 10,       /* clock_mode, u8 0 the region's, 1 12 h, 2 24 h */
    PROTO_PREFS_F_APPEARANCE = 11,       /* appearance, u8 0 dark, 1 light, 2 automatic */
    PROTO_PREFS_F_HIGH_CONTRAST = 12,    /* high_contrast, u8 0/1 */
    PROTO_PREFS_F_VOLUME = 13,           /* volume (Beeps), u8 0..100 */
    PROTO_PREFS_F_CHIME_VOLUME = 14,     /* chime_volume, u8 0..100 */
    PROTO_PREFS_F_VOICE_VOLUME = 15,     /* voice_volume, u8 0..100 */
    PROTO_PREFS_F_KEY_CLICK = 16,        /* key_click, u8 0/1 */
    PROTO_PREFS_F_VOICE_ENABLED = 17,    /* voice_enabled, u8 0/1; on only while the voice is
                                          * ready (the panel's rule: WHY_VOICE_NOT_READY) */
    PROTO_PREFS_F_VOICE_STATUS = 18,     /* voice_status, u8 0/1 */
    PROTO_PREFS_F_VOICE_CHIME = 19,      /* voice_chime, u8 0/1 */
} proto_prefs_id_t;

#define PROTO_PREFS_F_LAST 19u /* the highest id this version knows */

/* Why a field was refused (PREFS refusals section, settings_result's "refused"). Never
 * renumbered; 0 = not refused. A number this version doesn't know reads as NOT_ALLOWED
 * (proto_prefs_why_read). */
typedef enum {
    PROTO_PREFS_WHY_NONE = 0,
    PROTO_PREFS_WHY_NOT_ALLOWED = 1,     /* "not_allowed": not a field this panel takes */
    PROTO_PREFS_WHY_RANGE = 2,           /* "out_of_range" */
    PROTO_PREFS_WHY_IN_USE = 3,          /* "in_use": the panel's own Screen and sound page is open,
                                          * the setup runs, or a change is unsaved (UI-046) */
    PROTO_PREFS_WHY_HELD = 4,            /* "held": an alarm, entry time or exit time */
    PROTO_PREFS_WHY_VOICE_NOT_READY = 5, /* "voice_not_ready" */
    PROTO_PREFS_WHY_BAD = 6,             /* a TLV of the wrong length, or an id named twice; no word
                                          * of its own in "refused" (proto_prefs_why_word) */
    PROTO_PREFS_WHY_BUSY = 7,            /* "busy" */
} proto_prefs_why_t;

#define PROTO_PREFS_WHY_LAST 7u

/* A set of fields: bit id - 1 (PREFS' accepts, a values struct's have). */
typedef uint32_t proto_prefs_mask_t;
#define PROTO_PREFS_BIT(id)  ((proto_prefs_mask_t)1u << ((unsigned)(id) - 1u))
#define PROTO_PREFS_KNOWN    ((proto_prefs_mask_t)0x0007FFFFu) /* ids 1..19 */
/* How the screen looks (ids 1, 2, 5-8, 11, 12): a remote change at most once per 2 s (UI-046). */
#define PROTO_PREFS_LOOK     ((proto_prefs_mask_t)0x00000CF3u)

/* Field flags. */
#define PROTO_PREFS_FL_LOOK   0x01u /* in PROTO_PREFS_LOOK */
#define PROTO_PREFS_FL_SWITCH 0x02u /* 0 or 1; JSON true / false */
#define PROTO_PREFS_FL_DELAY  0x04u /* milliseconds: 0 (never) or min..max; JSON whole seconds */

#define PROTO_PREFS_KEY_MAX     11u /* a JSON key's bytes without the NUL, in every version */
#define PROTO_PREFS_TLV_MAX     68u /* all 19 fields' TLVs */
#define PROTO_PREFS_ENC_MAX     69u /* count + all 19 TLVs: the fields section's body (70 B with
                                     * the section's length byte) */
#define PROTO_PREFS_JSON_MAX    320u /* proto_prefs_json_values at its longest (host-tested) */
#define PROTO_PREFS_UNKNOWN_MAX 8u   /* unknown ids or keys a decoder names */

typedef struct {
    uint8_t id;        /* PROTO_PREFS_F_* */
    uint8_t size;      /* wire bytes: 1, 2 or 4 */
    uint8_t flags;     /* PROTO_PREFS_FL_* */
    const char *key;   /* the JSON key */
    uint32_t min, max; /* the range on the wire; a delay also takes 0 */
} proto_prefs_field_t;

/* One member per field, the wire's units (a delay in ms), and which ones hold a value. */
typedef struct {
    proto_prefs_mask_t have;     /* bit id - 1: that member holds a value */
    uint8_t brightness;          /* 1 */
    uint8_t saver_brightness;    /* 2 */
    uint32_t saver_after_ms;     /* 3 */
    uint32_t off_after_ms;       /* 4 */
    bool night_on;               /* 5 */
    uint16_t night_start;        /* 6 */
    uint16_t night_end;          /* 7 */
    uint8_t night_brightness;    /* 8 */
    uint32_t night_off_after_ms; /* 9 */
    uint8_t clock_mode;          /* 10 */
    uint8_t appearance;          /* 11 */
    bool high_contrast;          /* 12 */
    uint8_t volume;              /* 13 */
    uint8_t chime_volume;        /* 14 */
    uint8_t voice_volume;        /* 15 */
    bool key_click;              /* 16 */
    bool voice_enabled;          /* 17 */
    bool voice_status;           /* 18 */
    bool voice_chime;            /* 19 */
} proto_prefs_values_t;

/* What a decoder took and refused. */
typedef struct {
    proto_prefs_values_t values;     /* the fields taken: values.have */
    proto_prefs_mask_t refused;      /* the known fields refused (bit id - 1) */
    uint8_t why[PROTO_PREFS_F_LAST]; /* why[id - 1]: a refused field's PROTO_PREFS_WHY_*, else 0 */
    uint8_t unknown_n;               /* ids this version doesn't know, skipped by length (a
                                      * PREFS_SET's are refused NOT_ALLOWED by id); a repeat of
                                      * one named below isn't counted again; at most 255 */
    uint8_t unknown[PROTO_PREFS_UNKNOWN_MAX]; /* the first PROTO_PREFS_UNKNOWN_MAX of them */
} proto_prefs_decoded_t;

typedef struct {
    proto_prefs_values_t values;
    proto_prefs_mask_t refused;
    uint8_t why[PROTO_PREFS_F_LAST];
    uint8_t unknown_n; /* keys this version doesn't know, refused "not_allowed"; as above */
    char unknown[PROTO_PREFS_UNKNOWN_MAX][PROTO_PREFS_KEY_MAX + 1u]; /* the first of them */
} proto_prefs_parsed_t;

/* ---- the table ---------------------------------------------------------------------------- */

/* A field by id; NULL for an id this version doesn't know (0 included). */
const proto_prefs_field_t *proto_prefs_field(unsigned id);
/* A field's id by JSON key; 0 for a key this version doesn't know. */
uint8_t proto_prefs_id(const char *key);

/* 0 (PROTO_PREFS_WHY_NONE) when the field takes the value; RANGE; NOT_ALLOWED for an unknown id. */
proto_prefs_why_t proto_prefs_check(unsigned id, uint32_t value);
/* Stores a checked value and sets its have bit; anything refused leaves v as it was. */
proto_prefs_why_t proto_prefs_put(proto_prefs_values_t *v, unsigned id, uint32_t value);
/* false when the id is unknown or v doesn't have it. A switch reads 0 or 1. */
bool proto_prefs_get(const proto_prefs_values_t *v, unsigned id, uint32_t *value);

/* A why number from the wire: an unknown one (0 included) reads as NOT_ALLOWED. */
proto_prefs_why_t proto_prefs_why_read(uint8_t wire);
/* Its word in settings_result's "refused": one of the six docs/features/23 protocol.md lists,
 * "not_allowed" for an unknown number and for BAD (which settings_set's parser never gives a
 * field: it makes the whole request bad). */
const char *proto_prefs_why_word(uint8_t why);

/* ---- the panel link (TLV) ----------------------------------------------------------------- */

/* count u8, then a TLV per field in v->have & mask, in id order. Returns the length (at least 1),
 * or 0 when it doesn't fit cap (PROTO_PREFS_ENC_MAX always does). Values go as they are. */
size_t proto_prefs_encode(const proto_prefs_values_t *v, proto_prefs_mask_t mask, uint8_t *out,
                          size_t cap);

/* A fields section's body (plink_prefs_t.fields as received). Each known field is taken, or
 * refused alone: BAD for a wrong length or an id named twice (neither copy is taken), NOT_ALLOWED
 * outside accept, RANGE. Unknown ids are skipped by their length and named in out->unknown; bytes
 * after the count's last TLV are skipped (appended later). len 0 reads as an empty list. false
 * when a TLV runs past len (out is then empty). */
bool proto_prefs_decode(const uint8_t *in, size_t len, proto_prefs_mask_t accept,
                        proto_prefs_decoded_t *out);

/* prefs_crc: CRC-32 (alarm_crc32) of a fields section's whole body - count, TLVs and whatever a
 * later version appended, without the section's length byte - as the keypad encoded it; the
 * master takes it over the body it received (plink_prefs_t.fields, fields_len), so it holds
 * across versions. _values_crc encodes all of v->have first. */
uint32_t proto_prefs_crc(const uint8_t *fields, size_t len);
uint32_t proto_prefs_values_crc(const proto_prefs_values_t *v);

/* ---- the Home Assistant link (JSON) ------------------------------------------------------- */

/* The "values" object: v->have's fields in id order; a delay in whole seconds (ms / 1000), a
 * switch true / false, the others as numbers. The caller writes the key before it. */
void proto_prefs_json_values(pj_writer_t *w, const proto_prefs_values_t *v);
/* The "can" array: the keys of mask's known fields, in id order. */
void proto_prefs_json_keys(pj_writer_t *w, proto_prefs_mask_t mask);

/* The next value of r as a settings_set's "values" object. A switch takes true / false, a delay
 * whole seconds (0, or 10..86400), the others integers. Each known key is taken, or refused alone:
 * NOT_ALLOWED outside accept, RANGE. An unknown key is refused NOT_ALLOWED by name
 * (out->unknown). false (out empty) when the whole request is bad (settings_result's error
 * "bad"): the value isn't an object, a key is empty or longer than PROTO_PREFS_KEY_MAX (no
 * settings key ever is), a known key's value isn't of its kind (a switch true / false, the others
 * whole numbers within int64) or the key is named twice, or the JSON is damaged. Only damaged
 * JSON leaves r in error; otherwise r is past the value, so the caller reads the rest of the
 * message. */
bool proto_prefs_json_parse(pj_reader_t *r, proto_prefs_mask_t accept, proto_prefs_parsed_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_PREFS_H */
