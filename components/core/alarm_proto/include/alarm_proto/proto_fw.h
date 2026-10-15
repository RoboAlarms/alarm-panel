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

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PROTO_FW_H */
