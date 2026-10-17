/*
 * proto_fw.h - firmware updates (docs/features/13): versions, the release manifest, when an update
 * may be installed, the serial upload framing, and the Home Assistant update entity.
 *
 * Portable C11 like the rest of alarm_proto. The image itself is signed and verified by
 * ESP-IDF on the device; the manifest only says what exists, so it needs no signature.
 */
#ifndef ALARM_PROTO_FW_H
#define ALARM_PROTO_FW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "alarm_proto/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versions (semantic versioning) ------------------------------------------------------ */

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    char pre[24]; /* pre-release ("rc.1", "test"), "" for a release */
} proto_version_t;

/* "1.2.3", "v1.2.3", "1.2.3-rc.1", "1.2.3+build.7" (build metadata ignored). false for anything
 * else (a git hash is not a version). */
bool proto_version_parse(const char *text, proto_version_t *out);

/* <0, 0, >0 by semantic versioning precedence (1.2.3-rc.1 < 1.2.3 < 1.2.4). */
int proto_version_compare(const proto_version_t *a, const proto_version_t *b);

/* OTA-014: may 'candidate' replace 'running'? Only newer, unless 'allow_older' (installer's
 * explicit choice). Unparsable versions only with allow_older. */
bool proto_version_is_upgrade(const char *running, const char *candidate, bool allow_older);

/* ---- release manifest (OTA-002) --------------------------------------------------------- */

typedef struct {
    char version[32];
    char url[256];     /* absolute; relative URLs are resolved against the manifest's URL */
    uint32_t size;     /* bytes, 0 = unknown */
    uint8_t sha256[32];
    bool has_sha256;
    char notes_url[192];
} proto_fw_artifact_t;

/* Pick this device's image from a manifest:
 *   {"version":"0.6.1","notes_url":"...","artifacts":[{"device":"crowpanel_p4","chip_rev":"v1",
 *    "url":"crowpanel_p4_rev1.bin","size":1958464,"sha256":"<64 hex>"}]}
 * chip_rev may be NULL (devices with one image). Returns ALARM_OK, ALARM_ERR_BAD_ARG (not a
 * valid manifest) or ALARM_ERR_DISABLED (no image for this device). */
alarm_result_t proto_fw_manifest_pick(const char *json, size_t len, const char *device,
                                      const char *chip_rev, const char *manifest_url,
                                      proto_fw_artifact_t *out);

typedef struct {
    char locale[16];   /* BCP 47, 2-15 of [A-Za-z0-9-] */
    char version[32];  /* the entry's "version", else the manifest's */
    char url[256];     /* absolute: a relative one resolved against the manifest's URL */
    uint32_t size;     /* bytes, 0 = not given */
    uint8_t sha256[32];
    bool has_sha256;
} proto_fw_language_t;

/* The manifest's language packs (docs/features/37 language-packs.md, OTA-016), in the manifest's
 * order, at most max: an entry without a valid locale or url, or with a malformed size or sha256,
 * is skipped; no "languages" key is 0 entries. Returns how many were written, or SIZE_MAX when the
 * text is not a valid manifest (the same test proto_fw_manifest_pick applies). */
size_t proto_fw_manifest_languages(const char *json, size_t len, const char *manifest_url,
                                   proto_fw_language_t *out, size_t max);

/* The kinds of asset pack (docs/features/39 asset-packs.md, OPT-007): the same numbers as the pack
 * store's kinds 3-5. The manifest's words are "letters", "picture", "certificates". */
typedef enum {
    PROTO_FW_ASSET_LETTERS = 3,
    PROTO_FW_ASSET_PICTURE = 4,
    PROTO_FW_ASSET_CERTS = 5,
} proto_fw_asset_kind_t;

typedef struct {
    proto_fw_asset_kind_t kind;
    char version[32];  /* the entry's "version", else the manifest's */
    char url[256];     /* absolute: a relative one resolved against the manifest's URL */
    uint32_t size;     /* bytes, 0 = not given */
    uint8_t sha256[32];
    bool has_sha256;
} proto_fw_asset_t;

/* The manifest's asset packs ("assets", OPT-007), with the rules of proto_fw_manifest_languages:
 * in the manifest's order, at most max; an entry without a kind or url, of a kind this firmware
 * does not know ("web" included, for now), or with a malformed size or sha256 is skipped; no
 * "assets" key is 0 entries. SIZE_MAX when the text is not a valid manifest. */
size_t proto_fw_manifest_assets(const char *json, size_t len, const char *manifest_url,
                                proto_fw_asset_t *out, size_t max);

/* ---- install policy (OTA-004) ----------------------------------------------------------- */

/* A new image may be started only when every enabled partition is disarmed, nothing sounds,
 * no walk test runs and installer mode is off. why (may be NULL) gets a short reason. */
bool proto_fw_install_allowed(const alarm_snapshot_t *snap, const char **why);

/* ---- serial upload (OTA-013) ------------------------------------------------------------ */

/* Standard base64 (RFC 4648, padding required). Returns the decoded length, or SIZE_MAX on bad
 * input or when it doesn't fit. */
size_t proto_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap);

/* One upload line "<seq> <crc32 as 8 hex digits> <base64>" (the CRC covers the decoded bytes,
 * alarm_crc32). Returns the decoded length and the sequence number, or SIZE_MAX when the line
 * is malformed or the CRC doesn't match. */
size_t proto_fw_line_decode(const char *line, uint32_t *seq, uint8_t *out, size_t cap);

/* ---- Home Assistant update entity (OTA-010) --------------------------------------------- */

typedef struct {
    const char *installed;   /* running version */
    const char *latest;      /* newest known version (= installed when up to date) */
    const char *title;       /* "RoboAlarms Panel firmware" */
    const char *release_url; /* NULL or "" = none: the key is left out */
    bool in_progress;
    int percent;             /* 0..100 while downloading, -1 = none */
} proto_fw_state_t;

/* State JSON for .../firmware (the keys Home Assistant's MQTT update entity reads). */
void proto_fw_state(pj_writer_t *w, const proto_fw_state_t *st);

/* update entity "Firmware" (device class firmware); can_install adds the install command. */
void proto_ha_update(pj_writer_t *w, const proto_ident_t *id, bool can_install);

/* ---- updates through the panel link (docs/features/11 "Updates through the link") ------- */

/* What an update run ends in, in the panel link's why numbers (docs/features/11, the why table:
 * FW_STATE's why). The same numbers as panel_link's PLINK_FW_WHY_* (plink_msg.h), on the wire and
 * never renumbered. 0 and 1-19, the outcomes of a check, are proto_fwsrv_why_t (proto_fwsrv.h);
 * these are the other classes. A reader takes a value it doesn't know by its decade. */
typedef enum {
    PROTO_FW_WHY_OK = 0,
    /* 20-29: the transfer (tried again, or the relay instead) */
    PROTO_FW_WHY_BROKE_OFF = 20,     /* the connection broke off */
    PROTO_FW_WHY_TOO_SLOW = 21,      /* below the floor (OTA-027) */
    PROTO_FW_WHY_BAD_SIZE = 22,      /* not the length named (more, fewer, or a server's other) */
    PROTO_FW_WHY_BAD_SHA = 23,       /* not the SHA-256 named */
    PROTO_FW_WHY_STALLED = 24,       /* the relay: 60 s without a new byte (the keypad's core) */
    PROTO_FW_WHY_TIMEOUT = 25,       /* the relay: 30 min paused; a READY image: 24 h, no INSTALL */
    PROTO_FW_WHY_NO_MEMORY = 26,
    PROTO_FW_WHY_FLASH = 27,         /* writing the slot, or switching to it, failed */
    /* 30-49: refused, never tried again (the same signed image fails the same way) */
    PROTO_FW_WHY_KEY = 30,           /* signed with a key this panel doesn't trust (LINK-036) */
    PROTO_FW_WHY_CHIP = 31,          /* another chip, or a chip revision outside its range */
    PROTO_FW_WHY_PROJECT = 32,       /* a signed project this panel doesn't take */
    PROTO_FW_WHY_NOT_NEWER = 33,     /* not newer than what runs (OTA-014) */
    PROTO_FW_WHY_SIGNATURE = 34,     /* not an image, or its image or signature check failed */
    PROTO_FW_WHY_TOO_BIG = 35,       /* larger than the slot */
    PROTO_FW_WHY_UNKNOWN_KIND = 36,
    PROTO_FW_WHY_UNKNOWN_OP = 37,
    PROTO_FW_WHY_BUDGET = 38,
    PROTO_FW_WHY_BAD_ORDER = 40,     /* an order this panel can't follow */
    PROTO_FW_WHY_WRONG_VERSION = 41, /* not the version ordered, or past its master's */
    /* 50-59: waiting */
    PROTO_FW_WHY_MASTER_BUSY = 50,
    PROTO_FW_WHY_NOT_QUIET = 51,
    PROTO_FW_WHY_PANIC_PENDING = 52,
    PROTO_FW_WHY_KEPT_TONE = 53,
    PROTO_FW_WHY_NO_STATUS = 54,
    PROTO_FW_WHY_BUSY = 55,          /* another update runs here */
    /* 60-69: stopped */
    PROTO_FW_WHY_STOPPED = 60,       /* a person at this panel (OTA-027) */
    PROTO_FW_WHY_CANCELLED = 61,     /* its master */
} proto_fw_why_t;

/* A why's name in the JSON (~/event's keypad_failed "reason", next to its number "why"): the why
 * table's name in lower case ("stalled", "unknown_device"), for proto_fwsrv_why_t's 1-17 and
 * these; "unknown" for 0 and for a value this firmware doesn't know (a reader takes it by its
 * decade, from the number). An external contract: names are added, never changed. */
const char *proto_fw_why_name(unsigned why);

/* Never past its master (docs/features/11 LINK-029, decision 5): the cap on what a paired keypad
 * may install by any way but a cable. */
typedef enum {
    PROTO_FW_CAP_NONE = 0, /* no master (a stand-alone panel, a master, an unpaired keypad) */
    PROTO_FW_CAP_UNKNOWN,  /* paired, its master's version not heard yet: nothing passes */
    PROTO_FW_CAP_AT,       /* at most its master's version */
} proto_fw_cap_t;

/* May `version` be installed under the cap? NONE: yes; UNKNOWN: no; AT: when it is not newer than
 * cap_version (semantic precedence, build metadata aside: "0.9.0+local" passes a cap of "0.9.0").
 * A version or a cap that doesn't parse never passes AT. */
bool proto_fw_cap_allows(proto_fw_cap_t cap, const char *cap_version, const char *version);

/* A chip revision (major * 100 + minor) inside an image's range: at least min_full, and at most
 * max_full unless that is 0 or 0xFFFF (no newest, as ESP-IDF's bootloader reads it). */
bool proto_fw_chip_rev_fits(uint16_t rev_full, uint16_t min_full, uint16_t max_full);

/* What the first PROTO_FW_HEADER_LEN bytes of an ESP-IDF application image say: the image header
 * (24 bytes), its first segment's header (8) and the application description (256) that opens an
 * application's first segment. Read byte by byte, little-endian. The writer holds these bytes back
 * until they pass (LINK-033), so a wrong image is refused before any flash is written. */
#define PROTO_FW_HEADER_LEN   288u
#define PROTO_FW_CHIP_ESP32P4 0x0012u /* esp_chip_id_t ESP_CHIP_ID_ESP32P4 */
#define PROTO_FW_CHIP_ESP32C6 0x000Du /* esp_chip_id_t ESP_CHIP_ID_ESP32C6 */

typedef struct {
    bool image_magic;      /* the image header's magic (0xE9) */
    bool app_magic;        /* the application description's magic word (0xABCD5432) */
    uint8_t segments;      /* segment_count */
    uint16_t chip_id;      /* PROTO_FW_CHIP_* */
    uint16_t min_rev_full; /* the oldest chip revision it runs on, major * 100 + minor */
    uint16_t max_rev_full; /* the newest; 0 or 0xFFFF: no newest */
    bool hash_appended;
    char version[33];      /* the description's version, at most 32 characters */
    char project[33];      /* its signed project name */
} proto_fw_header_t;

/* false (out zeroed) when buf is NULL or shorter than PROTO_FW_HEADER_LEN. Whether it is an image
 * at all is proto_fw_header_judge's question. */
bool proto_fw_header_read(const uint8_t *buf, size_t len, proto_fw_header_t *out);

/* The panel that judges an image: what it is and what it may take. */
typedef struct {
    uint16_t chip_id;        /* this chip, PROTO_FW_CHIP_* */
    uint16_t rev_full;       /* its revision, major * 100 + minor */
    const char *project;     /* the running image's signed project name */
    const char *version;     /* the running version */
    bool allow_older;        /* a person forced it (OTA-014); never on an ordered run */
    proto_fw_cap_t cap;      /* never past its master */
    const char *cap_version;
} proto_fw_self_t;

/* The header's verdict, in the order it is asked (LINK-033: magic, chip, chip revision range,
 * project, newer; then the cap). Appended only. */
typedef enum {
    PROTO_FW_HDR_OK = 0,
    PROTO_FW_HDR_NOT_AN_IMAGE,   /* no image or application magic */
    PROTO_FW_HDR_OTHER_CHIP,     /* built for another chip */
    PROTO_FW_HDR_OTHER_REVISION, /* this chip's revision is outside the image's range */
    PROTO_FW_HDR_OTHER_PROJECT,  /* ota_profile_accepts() refuses its project */
    PROTO_FW_HDR_NOT_NEWER,      /* not newer than the running version, unless allow_older */
    PROTO_FW_HDR_PAST_CAP,       /* newer than its master's version (or that version not known) */
} proto_fw_hdr_verdict_t;

proto_fw_hdr_verdict_t proto_fw_header_judge(const proto_fw_header_t *h, const proto_fw_self_t *me);

/* The verdict in the why numbers: NOT_AN_IMAGE is SIGNATURE (the image check), the chip and its
 * revision CHIP, the project PROJECT, NOT_NEWER NOT_NEWER and PAST_CAP WRONG_VERSION. */
proto_fw_why_t proto_fw_header_why(proto_fw_hdr_verdict_t v);

/* Reads len bytes at offset of a slot into out; false when it can't. */
typedef bool (*proto_fw_read_fn)(void *ctx, uint32_t offset, void *out, size_t len);

/* The exact length of the application image at the start of a slot of `limit` bytes, for the
 * relay from a master's own slot (docs/features/11 "The master", image sources): its header, its
 * segments (1-16), the checksum padded to 16 bytes, the SHA-256 when appended, and the 4 KB
 * signature sector when one starts at the next 4 KB boundary (its first byte 0xE7, a Secure Boot
 * v2 signature block). 0 when it isn't an application image for chip_id, or doesn't fit. */
uint32_t proto_fw_image_size(proto_fw_read_fn read, void *ctx, uint32_t limit, uint16_t chip_id);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PROTO_FW_H */
