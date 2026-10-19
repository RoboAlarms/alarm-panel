/*
 * proto_fwsrv.h - talking to a RoboAlarms firmware server (docs/features/13 OTA-017..OTA-025).
 *
 * - The update server is a domain ("roboalarms.com"); the panel asks https://firmware.<domain>.
 * - A check is POST /api/v1/check with what the panel is; the answer says what to install.
 * - GET /api/v1/keys lists the server's signing keys. The panel trusts the key of the image it runs,
 *   plus one key per server that the installer saw and accepted (the trust record below). A key is
 *   named by its digest the way ESP-IDF computes it: the SHA-256 of the 776-byte public key in a
 *   Secure Boot v2 (RSA-3072) signature block.
 *
 * Portable C11 like the rest of alarm_proto: no I/O here, the device does the HTTPS.
 */
#ifndef ALARM_PROTO_FWSRV_H
#define ALARM_PROTO_FWSRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto.h"
#include "alarm_proto/proto_fw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_FWSRV_DOMAIN_MAX   96u /* bytes with the NUL; longer domains are refused */
#define PROTO_FWSRV_BASE_MAX     (PROTO_FWSRV_DOMAIN_MAX + 20u)
#define PROTO_FWSRV_KEY_NAME_MAX 48u
#define PROTO_FWSRV_KEYS_MAX     4u  /* keys read from one GET /api/v1/keys answer */
#define PROTO_FWSRV_FINGERPRINT_LEN 20u /* "3F9A 1C2B 77D0 E415" and the NUL */

/* ---- the server's domain (OTA-017) ------------------------------------------------------- */

/* What a person typed, made into a domain: spaces around it, a leading "https://" (or "http://")
 * and a trailing "/" are dropped, letters lowercased. Then it must be labels of [a-z0-9-] (1-63
 * each, not starting or ending with '-') joined by single dots, at least two labels, the last one
 * letters only, at most PROTO_FWSRV_DOMAIN_MAX - 1 in all. "example.net" and
 * "builds.example.co.uk" pass; "localhost", "192.0.2.7", "example", "exa mple.net", "a..b" and
 * anything with a path, port or "@" don't. false: not a domain (out is then "").  */
bool proto_fwsrv_domain(const char *typed, char *out, size_t cap);

/* The server's address for a stored server: "https://firmware.<domain>" for a domain, or a
 * development address ("http://192.0.2.10:8080", set at the service console only) as it is, without
 * a trailing "/". false when it is neither, or it doesn't fit. */
bool proto_fwsrv_base_url(const char *server, char *out, size_t cap);

/* true for "stable", "beta", "alpha" and "dev" (OTA-019). */
bool proto_fwsrv_channel_valid(const char *channel);

/* ---- the check (OTA-018) ----------------------------------------------------------------- */

typedef struct {
    const char *channel;   /* "stable" when NULL or "" */
    const char *device;    /* the board's id ("crowpanel_dhe04107d") */
    const char *board_rev; /* "1.1"; NULL or "" leaves it out */
    const char *chip_rev;  /* "v1", "v3" */
    const char *project;   /* the signed project name of the image it runs */
    const char *version;   /* the version it runs */
    const char *const *peripherals; /* "cc1101", "zac93" */
    size_t peripheral_count;
    const char *const *languages;   /* BCP 47 locales it wants packs for */
    size_t language_count;
    /* A keypad's check only: the version of the one release its master ordered, the version the
     * master runs (docs/features/11 LINK-029, LINK-031; the server's FWS-028). NULL or "" leaves
     * it out, and the check is as it always was. */
    const char *want;
} proto_fwsrv_facts_t;

/* The request body of POST /api/v1/check, as one JSON object. "want" comes last, and only when
 * facts->want is set: never "" (the server takes text that is no version as bad input, FWS-028). */
void proto_fwsrv_check_request(pj_writer_t *w, const proto_fwsrv_facts_t *facts);

/* Appended only, never renumbered. */
typedef enum {
    PROTO_FWSRV_UP_TO_DATE = 0,
    PROTO_FWSRV_UPDATE,          /* "update": true */
    PROTO_FWSRV_UNKNOWN_DEVICE,  /* its catalogue doesn't have this board, chip or build */
    PROTO_FWSRV_STEPPING_STONE,  /* newer releases exist, none it may install yet */
    PROTO_FWSRV_HELD_BACK,       /* newer releases exist, their hardware rules leave it out */
    PROTO_FWSRV_OTHER,           /* a reason this panel doesn't know, on a check without want:
                                    treated as up to date */
    /* On a check with want only (FWS-028); without want these words read as OTHER, as before. */
    PROTO_FWSRV_UNKNOWN_VERSION, /* 6, "unknown_version": the server has no release of that version
                                    for this panel's chip revision and project (or want names no
                                    release): refuses nothing */
    PROTO_FWSRV_NOT_OFFERED,     /* 7, "not_offered": that release is withdrawn, or on a channel
                                    this panel's channel doesn't see: a refusal */
    PROTO_FWSRV_REFUSED,         /* 8: a reason this panel doesn't know, on a check with want: a
                                    refusal ("a reason added later refuses", FWS-028) */
} proto_fwsrv_reason_t;

typedef struct {
    proto_fwsrv_reason_t reason;
    /* "installed": what the server knows of the version the panel runs */
    char installed_channel[8];   /* "" = null (not on the server, a local build) */
    bool installed_withdrawn;
    /* PROTO_FWSRV_UPDATE only */
    char channel[8];             /* the offered release's channel */
    bool critical;
    proto_fw_artifact_t image;   /* version, url (absolute, resolved against base_url), size,
                                    sha256, notes_url */
    char key_name[PROTO_FWSRV_KEY_NAME_MAX]; /* image.signing_key, "" = not given */
    uint8_t key_sha256[32];
    bool has_key;
} proto_fwsrv_answer_t;

/* Reads a check answer. ALARM_OK; ALARM_ERR_BAD_ARG: not a check answer (not JSON, no "update"
 * boolean, an update without an image url, or a malformed sha256). An "update": true without a
 * valid "version" is malformed too. */
alarm_result_t proto_fwsrv_check_answer(const char *json, size_t len, const char *base_url,
                                        proto_fwsrv_answer_t *out);

/* The answer to a check that carried want (facts->want; NULL or "": none, and this is
 * proto_fwsrv_check_answer byte for byte). With want, "unknown_version" and "not_offered" are
 * read, and a reason this panel doesn't know (or one too long to be one it knows) is
 * PROTO_FWSRV_REFUSED, never up to date. A missing or null reason stays UP_TO_DATE. An offer is
 * read whatever its version: proto_fwsrv_check_why says whether it is the one named. */
alarm_result_t proto_fwsrv_check_answer_want(const char *json, size_t len, const char *base_url,
                                             const char *want, proto_fwsrv_answer_t *out);

/* A check's outcome in the panel link's why numbers (docs/features/11 "Updates through the link",
 * the why table: FW_STATE's why, its probe section's why). The same numbers as panel_link's
 * PLINK_FW_WHY_* (plink_msg.h), on the wire and never renumbered; 0 and 1-19 only, the classes a
 * check can end in. A reader takes a value it doesn't know by its decade: one in 10-19 blocks. */
typedef enum {
    PROTO_FWSRV_WHY_OK = 0,               /* the server offers exactly want */
    /* 1-9: the server wasn't reached (the device's HTTPS layer sets 1-4) */
    PROTO_FWSRV_WHY_NO_NETWORK = 1,
    PROTO_FWSRV_WHY_NO_LOOKUP = 2,
    PROTO_FWSRV_WHY_NO_CONNECT = 3,
    PROTO_FWSRV_WHY_SERVER_ERROR = 4,
    PROTO_FWSRV_WHY_NOT_A_SERVER = 5,     /* an answer that isn't a check answer */
    /* 10-19: the server's answer */
    PROTO_FWSRV_WHY_UNKNOWN_DEVICE = 10,  /* refuses */
    PROTO_FWSRV_WHY_HELD_BACK = 11,       /* refuses */
    PROTO_FWSRV_WHY_STEPPING_STONE = 12,  /* refuses */
    PROTO_FWSRV_WHY_OTHER_VERSION = 13,   /* offers a version that isn't want (a server that
                                             ignores want offers its newest) */
    PROTO_FWSRV_WHY_UP_TO_DATE = 14,      /* nothing newer than the version it runs */
    PROTO_FWSRV_WHY_UNKNOWN_VERSION = 15, /* no release of want for this panel: refuses nothing */
    PROTO_FWSRV_WHY_NOT_OFFERED = 16,     /* refuses */
    PROTO_FWSRV_WHY_REFUSED = 17,         /* a reason this panel doesn't know, on a check with
                                             want: refuses */
} proto_fwsrv_why_t;

/* The why of a check that carried want (LINK-031: FETCH on OK, the relay on 13, 14 and 15,
 * blocked on 10-12 and 16-19). parsed: what proto_fwsrv_check_answer_want returned for a, with
 * the same want; anything but ALARM_OK, or a NULL a, is NOT_A_SERVER. An offer is OK only when
 * its version is want exactly, byte for byte ("0.9.0+local" is not "0.9.0"), else OTHER_VERSION.
 * A reason this panel doesn't know is REFUSED with want, even read without it. NULL or "" want
 * (no check with want): every offer is OTHER_VERSION and an unknown reason UP_TO_DATE. */
proto_fwsrv_why_t proto_fwsrv_check_why(alarm_result_t parsed, const proto_fwsrv_answer_t *a,
                                        const char *want);

/* The language packs in a check answer or in a GET /api/v1/languages answer:
 * the same rules as proto_fw_manifest_languages. SIZE_MAX: not an answer of either kind. */
size_t proto_fwsrv_languages(const char *json, size_t len, const char *base_url,
                             proto_fw_language_t *out, size_t max);

/* The asset packs ("assets", OPT-007) in a check answer or in a GET /api/v1/languages answer: the
 * rules of proto_fw_manifest_assets. An answer with "languages" and no "assets" is 0 entries.
 * SIZE_MAX: not an answer of either kind. */
size_t proto_fwsrv_assets(const char *json, size_t len, const char *base_url,
                          proto_fw_asset_t *out, size_t max);

/* The address of GET /api/v1/languages (OTA-025), for a panel adding a language it doesn't
 * have yet: base_url (from proto_fwsrv_base_url) + "/api/v1/languages?device=&chip_rev=&version="
 * from facts, then "&project=" when facts->project is set (it adds the drafts of a development
 * build). Values are percent-encoded (RFC 3986's unreserved characters stay as they are); the other
 * facts are not sent. false (out "") without a base, device, chip_rev or version, or when it
 * doesn't fit cap. */
bool proto_fwsrv_languages_url(const char *base_url, const proto_fwsrv_facts_t *facts, char *out,
                               size_t cap);

/* ---- signing keys (OTA-020) ------------------------------------------------------------- */

typedef struct {
    char name[PROTO_FWSRV_KEY_NAME_MAX];
    uint8_t sha256[32]; /* the ESP-IDF digest */
} proto_fwsrv_key_t;

/* GET /api/v1/keys: {"keys": [{"name": "...", "sha256": "<64 hex>", ...}]}, in the server's order,
 * at most max; an entry without a name or a valid sha256 is skipped. SIZE_MAX: not an answer. */
size_t proto_fwsrv_keys(const char *json, size_t len, proto_fwsrv_key_t *out, size_t max);

/* The key a new server's check shows the installer (OTA-020): the offered image's signing key
 * (offered, NULL or has_key false = not given; its name filled from the list when it has none);
 * else the only listed key; else the first listed key the panel trusts already (trusted, n_trusted
 * digests); else the first listed key. false when there is no key at all (out is then zeroed). */
bool proto_fwsrv_key_to_show(const proto_fwsrv_key_t *offered, bool has_offered,
                             const proto_fwsrv_key_t *keys, size_t n_keys,
                             const uint8_t (*trusted)[32], size_t n_trusted,
                             proto_fwsrv_key_t *out);

/* The digest of the key in a Secure Boot v2 RSA signature block (1216 bytes, as ESP-IDF appends
 * them): false unless the block's magic (0xE7), version (2) and CRC-32 over its first 1196 bytes
 * are right. */
bool proto_fwsrv_sigblock_key(const uint8_t *block, size_t len, uint8_t digest[32]);

/* "3F9A 1C2B 77D0 E415": the first 16 hex digits of a digest, upper case, in groups of four, as the
 * firmware server's Signing keys page shows them. */
void proto_fwsrv_fingerprint(const uint8_t digest[32], char out[PROTO_FWSRV_FINGERPRINT_LEN]);

/* ---- the trust record (NVS "ota"/"trust") ------------------------------------------------ */

/* The server the panel uses and the key the installer accepted for it. key absent (has_key
 * false): only the running image's key is trusted, as for this build's own server. */
typedef struct {
    char server[PROTO_FWSRV_DOMAIN_MAX];
    char key_name[PROTO_FWSRV_KEY_NAME_MAX];
    uint8_t key_sha256[32];
    bool has_key;
} proto_fwsrv_trust_t;

#define PROTO_FWSRV_TRUST_BLOB_MAX 192u

/* A versioned blob with its own CRC. encode returns the length (0 = doesn't fit); decode is
 * false for anything damaged, cut short or of an unknown version (the caller then falls back to
 * this build's own server). */
size_t proto_fwsrv_trust_encode(const proto_fwsrv_trust_t *t, uint8_t *out, size_t cap);
bool proto_fwsrv_trust_decode(const uint8_t *in, size_t len, proto_fwsrv_trust_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PROTO_FWSRV_H */
