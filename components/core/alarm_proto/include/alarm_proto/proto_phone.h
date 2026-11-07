/*
 * proto_phone.h - direct mode and the panel's own push (alarm-server's docs/features/02-Panel-and-App-API,
 * direct-mode.md and push-api.md; BL-225): pairing a phone by QR code, what a phone signs, the phone
 * table and the messages for push.roboalarms.com.
 *
 * Portable C11 like the rest of alarm_proto: the crypto is the caller's. The strings signed and the
 * JSON are an external contract with the app and the relay: never change, only add. Tests reference
 * DIR-NNN and PUSH-NNN.
 */
#ifndef PROTO_PHONE_H
#define PROTO_PHONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "alarm_proto/proto_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_PHONE_MAX        16u
#define PROTO_PHONE_KEY_LEN    32u
#define PROTO_PHONE_SIG_LEN    64u
#define PROTO_PHONE_NAME_LEN   32u
#define PROTO_PHONE_ID_LEN     17u  /* 16 hex digits of SHA-256(public key) + NUL */
#define PROTO_PHONE_HANDLE_LEN 128u
#define PROTO_PHONE_KID_LEN    12u
#define PROTO_PHONE_TOKEN_LEN  16u  /* 128 bits */
#define PROTO_PHONE_CHALLENGE_LEN 32u
#define PROTO_PHONE_PAIR_MS    300000u /* DIR-001: the QR code lasts 5 minutes */
#define PROTO_PHONE_PAIR_TRIES 3u      /* a wrong token three times closes the window */
#define PROTO_PHONE_CHALLENGE_MS 60000u
#define PROTO_PHONE_SESSION_MS   28800000u /* 8 hours */
#define PROTO_PHONE_SIGNED_MAX   160u

/* One paired phone, stored whole in NVS: a new field goes at the end, read by its length. */
typedef struct {
    uint8_t used;
    uint8_t user;                         /* the panel user it acts as */
    uint8_t pub[PROTO_PHONE_KEY_LEN];     /* its Ed25519 key, private half behind its biometrics */
    char name[PROTO_PHONE_NAME_LEN];
    /* Direct push, empty when it takes alerts from the server (PUSH-008). */
    char handle[PROTO_PHONE_HANDLE_LEN];
    char kid[PROTO_PHONE_KID_LEN];
    uint8_t push_key[PROTO_PHONE_KEY_LEN];
    uint8_t levels;                       /* bits of proto_phone_level_t it wants */
    uint8_t reserved[3];
} proto_phone_t;

typedef enum {
    PROTO_PUSH_ALARM = 0,
    PROTO_PUSH_ALARM_OVER,
    PROTO_PUSH_CONNECTION,
    PROTO_PUSH_TROUBLE,
    PROTO_PUSH_ACTIVITY,
} proto_phone_level_t;
#define PROTO_PUSH_ALL_LEVELS 0x1Fu

const char *proto_push_level_name(proto_phone_level_t level);

/* A slot's id: the first 16 hex digits of SHA-256(its public key). */
void proto_phone_id(const uint8_t pub[PROTO_PHONE_KEY_LEN], char out[PROTO_PHONE_ID_LEN]);

/* The table (caller-owned, 16 entries). NULL when none. */
proto_phone_t *proto_phone_find(proto_phone_t *table, const char *id);
proto_phone_t *proto_phone_free_slot(proto_phone_t *table);
/* Removes every phone of this user (DIR-004: the person's role changed or they were deleted);
 * returns how many went. */
size_t proto_phone_remove_user(proto_phone_t *table, uint8_t user);

/* The window a QR code opens: the token works once. */
typedef struct {
    uint8_t token[PROTO_PHONE_TOKEN_LEN];
    uint8_t user;
    uint8_t wrong;
    bool open;
    uint64_t until_ms;
} proto_phone_window_t;

void proto_phone_window_open(proto_phone_window_t *w, const uint8_t token[PROTO_PHONE_TOKEN_LEN],
                             uint8_t user, uint64_t now_ms);
/* DIR-001: true when this is the token and the window still stands; a right one closes it. A wrong
 * one counts, and the third closes the window. */
bool proto_phone_window_use(proto_phone_window_t *w, const uint8_t token[PROTO_PHONE_TOKEN_LEN],
                            uint64_t now_ms);

/* roboalarms://pair?id=…&h=<hosts>&fp=<hex>&t=<base64url>. false when it doesn't fit. */
bool proto_phone_pair_uri(const char *panel_id, const char *hosts, const uint8_t fp[32],
                          const uint8_t token[PROTO_PHONE_TOKEN_LEN], char *out, size_t cap);

/* What a phone signs for a session: "roboalarms-phone-v1" 0 panel id 0 challenge(base64url).
 * Returns the length, 0 when it doesn't fit. */
size_t proto_phone_session_bytes(const char *panel_id, const char *challenge, uint8_t *out,
                                 size_t cap);
/* For one action (DIR-003): "roboalarms-phone-action-v1" 0 panel id 0 challenge 0 action. */
size_t proto_phone_action_bytes(const char *panel_id, const char *challenge, const char *action,
                                uint8_t *out, size_t cap);

/* Does this Panel Manager request need a fresh signature from a phone (DIR-003)? Disarm and
 * silence, and everything that changes people or opens Setup mode. */
bool proto_phone_guarded(const char *method, const char *path, const char *action);

/* ---- pushing through the relay (push-api.md) --------------------------------------------------- */

/* The additional data of a message: "ra-push-v1|" handle "|" level. Returns the length. */
size_t proto_push_aad(const char *handle, const char *level, char *out, size_t cap);

/* The plaintext (push-api.md "The encrypted message"). */
typedef struct {
    const char *title;
    const char *body;
    const char *loc;
    const char *loc_name;
    const char *kind;
    const char *ref;
    const char *at;       /* ISO 8601 */
    const char *category; /* alarm | arm_failed | none */
    const char *thread;
    const char *sound;    /* alarm | chime | none */
} proto_push_plain_t;
void proto_push_plain(pj_writer_t *w, const proto_push_plain_t *p);

/* Standard base64 with padding, for HTTP Message Signatures. Returns the length, 0 if it doesn't fit. */
size_t proto_push_b64(const uint8_t *in, size_t len, char *out, size_t cap);

/* "sha-256=:<base64>:" of the body. */
bool proto_push_digest(const void *body, size_t len, char *out, size_t cap);

/* The value of Signature-Input after "sig1=" (RFC 9421):
 * ("@method" "@target-uri" "content-digest");created=<t>;keyid="<id>";alg="ed25519" */
bool proto_push_sig_params(int64_t created, const char *keyid, char *out, size_t cap);

/* The signature base (RFC 9421 §2.5), every line but the last ending in LF. Returns the length. */
size_t proto_push_sig_base(const char *method, const char *target_uri, const char *digest,
                           const char *params, char *out, size_t cap);

/* Whether a phone acting as a user with this authority and these partitions (bit 0 = partition 1)
 * is told about an event in partition (0 = system): the same as what its history shows. A user who
 * can't have a phone (deleted, arm-only, guest, duress) hears nothing. */
bool proto_phone_hears(alarm_authority_t authority, uint8_t partitions, uint8_t partition);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_PHONE_H */
