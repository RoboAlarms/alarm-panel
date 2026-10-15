/*
 * backup.h - the panel's backup file (docs/features/14 CFG-005, CFG-006): one password-encrypted
 * file with everything needed to rebuild a panel, and nothing that is the panel itself.
 *
 * The file:
 *   header (44 bytes, authenticated as the AEAD's associated data)
 *     "RABK" | format u8 (1) | kdf u8 (1: PBKDF2-HMAC-SHA256) | aead u8 (1: AES-256-GCM) |
 *     reserved u8 (0) | iterations u32 | salt[16] | nonce[12] | payload_len u32
 *   ciphertext[payload_len] | tag[16]
 * All integers little-endian. The key is PBKDF2-HMAC-SHA256(password, salt, iterations, 32).
 *
 * The payload (the plaintext) is a list of records: type u8 | fields.
 *   META  (1): format u8 | created_s u64 (Unix seconds, 0 = unknown) | firmware[str] | board[str]
 *   ENTRY (2): ns[str] | key[str] | type u8 (backup_type_t) | len u32 | value[len]
 *   END   (0xFF): count u32 (the ENTRY records before it)
 *   any other type (a later format's): len u32 | bytes, skipped by this reader
 * where [str] is len u8 | bytes (no NUL). One META first, one END last, nothing after it.
 * An ENTRY is one NVS entry, so the firmware writes it back as it read it; which entries a
 * backup holds is the policy (backup_policy_keeps), data rather than code, and the same table
 * decides what a restore replaces on the panel it lands on.
 *
 * The cryptography comes through a port: the firmware's is PSA (mbedTLS), the host tests' is
 * written from the standards in test/ and checked against NIST and Python's known answers.
 *
 * Portable C11: no heap, no globals. Record and file formats are an external contract: a
 * backup written by one firmware must restore on a later one. Never renumber a record, a type
 * or a header field; add new record types (readers skip what they don't know).
 */
#ifndef BACKUP_H
#define BACKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define BACKUP_MUST_CHECK __attribute__((warn_unused_result))
#else
#define BACKUP_MUST_CHECK
#endif

#define BACKUP_MAGIC        "RABK"
#define BACKUP_FORMAT       1u
#define BACKUP_KDF_PBKDF2   1u /* PBKDF2-HMAC-SHA256 */
#define BACKUP_AEAD_GCM     1u /* AES-256-GCM */
#define BACKUP_HEADER_LEN   44u
#define BACKUP_SALT_LEN     16u
#define BACKUP_NONCE_LEN    12u
#define BACKUP_TAG_LEN      16u
#define BACKUP_KEY_LEN      32u
#define BACKUP_OVERHEAD     (BACKUP_HEADER_LEN + BACKUP_TAG_LEN)

/* PBKDF2 work: the default costs the panel a few seconds; a file asking for more than the
 * maximum is refused rather than tying the panel up. */
#define BACKUP_ITER_DEFAULT 200000u
#define BACKUP_ITER_MIN     10000u
#define BACKUP_ITER_MAX     2000000u

#define BACKUP_PASSWORD_MIN 8u  /* characters (bytes) the installer must choose, at least */
#define BACKUP_PASSWORD_MAX 64u

#define BACKUP_PAYLOAD_MAX  (256u * 1024u) /* a file larger than this is refused */
#define BACKUP_NAME_MAX     15u            /* an NVS namespace or key (NVS_KEY_NAME_MAX_SIZE - 1) */
#define BACKUP_STR_MAX      3999u          /* an NVS string without its NUL (NVS takes 4000) */
#define BACKUP_BLOB_MAX     (64u * 1024u)  /* one blob (the Z-Wave node cache is the largest) */
#define BACKUP_TEXT_MAX     31u            /* the META strings */

/* Record types (never renumber). */
#define BACKUP_REC_META  0x01u
#define BACKUP_REC_ENTRY 0x02u
#define BACKUP_REC_END   0xFFu

/* An entry's value type, as NVS stores it (never renumber). */
typedef enum {
    BACKUP_T_U8 = 1,
    BACKUP_T_I8 = 2,
    BACKUP_T_U16 = 3,
    BACKUP_T_I16 = 4,
    BACKUP_T_U32 = 5,
    BACKUP_T_I32 = 6,
    BACKUP_T_U64 = 7,
    BACKUP_T_I64 = 8,
    BACKUP_T_STR = 9,  /* without its NUL */
    BACKUP_T_BLOB = 10,
} backup_type_t;

typedef enum {
    BACKUP_OK = 0,
    BACKUP_ERR_ARG,       /* a bad argument or a buffer too small */
    BACKUP_ERR_PASSWORD,  /* too short or too long (making a file) */
    BACKUP_ERR_FORMAT,    /* not a backup file, a format this firmware can't read, or damaged */
    BACKUP_ERR_DECRYPT,   /* the password is wrong, or the file was changed */
    BACKUP_ERR_CRYPTO,    /* the crypto port failed */
    BACKUP_ERR_BOARD,     /* made on another kind of panel */
    BACKUP_ERR_CONTENT,   /* an entry this firmware refuses (identity, a bad configuration) */
} backup_result_t;

/* ---- the crypto port ------------------------------------------------------------------------ */
typedef struct {
    void *ctx;
    /* out = PBKDF2-HMAC-SHA256(password, salt, iterations), out_len bytes (RFC 8018 §5.2). */
    bool (*pbkdf2_sha256)(void *ctx, const uint8_t *password, size_t password_len,
                          const uint8_t *salt, size_t salt_len, uint32_t iterations, uint8_t *out,
                          size_t out_len);
    /* AES-256-GCM (NIST SP 800-38D), 12-byte nonce, 16-byte tag. ct may equal pt. */
    bool (*gcm_encrypt)(void *ctx, const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t len,
                        uint8_t *ct, uint8_t tag[16]);
    /* false when the tag doesn't match (pt is then zeroed). pt may equal ct. */
    bool (*gcm_decrypt)(void *ctx, const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t len,
                        const uint8_t tag[16], uint8_t *pt);
    /* len bytes from a cryptographically secure generator. */
    bool (*random)(void *ctx, uint8_t *buf, size_t len);
} backup_crypto_t;

/* ---- the payload ---------------------------------------------------------------------------- */
typedef struct {
    uint64_t created_s;                 /* Unix seconds, 0 = the panel didn't know the time */
    char firmware[BACKUP_TEXT_MAX + 1]; /* the version that made it */
    char board[BACKUP_TEXT_MAX + 1];    /* the kind of panel (ALARM_BOARD: "crowpanel_p4") */
} backup_meta_t;

typedef struct {
    char ns[BACKUP_NAME_MAX + 1];
    char key[BACKUP_NAME_MAX + 1];
    uint8_t type;       /* backup_type_t */
    const uint8_t *value; /* integers little-endian, 1..8 bytes by type; strings without NUL */
    uint32_t len;
} backup_entry_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint32_t count;
    bool failed; /* something didn't fit: backup_writer_finish refuses */
} backup_writer_t;

/* Starts a payload in buf with its META record. */
void backup_writer_init(backup_writer_t *w, uint8_t *buf, size_t cap, const backup_meta_t *meta);
/* Appends one entry (its value checked against its type). false when it doesn't fit or is
 * malformed; the writer then refuses to finish. */
bool backup_writer_add(backup_writer_t *w, const backup_entry_t *e);
/* Appends END; the payload is buf[0..*len). */
BACKUP_MUST_CHECK
backup_result_t backup_writer_finish(backup_writer_t *w, size_t *len);

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    uint32_t count;
    bool ended;
} backup_reader_t;

/* Checks the META record and positions the reader on the first entry. */
BACKUP_MUST_CHECK
backup_result_t backup_reader_init(backup_reader_t *r, const uint8_t *payload, size_t len,
                                   backup_meta_t *meta);
/* The next entry: BACKUP_OK with *e filled (e->value points into the payload), or BACKUP_OK
 * with *done set at END (the count checked, nothing after it). Unknown record types are
 * skipped. BACKUP_ERR_FORMAT for anything malformed. */
BACKUP_MUST_CHECK
backup_result_t backup_reader_next(backup_reader_t *r, backup_entry_t *e, bool *done);

/* ---- the file ------------------------------------------------------------------------------- */
/* Encrypts payload[0..len) into out (len + BACKUP_OVERHEAD bytes) with a fresh salt and nonce
 * from the port's generator. iterations 0 = BACKUP_ITER_DEFAULT. */
BACKUP_MUST_CHECK
backup_result_t backup_seal(const backup_crypto_t *c, const char *password, uint32_t iterations,
                            const uint8_t *payload, size_t len, uint8_t *out, size_t cap,
                            size_t *out_len);
/* The same with the salt and nonce given (tests and frozen fixtures only: a real file always
 * takes fresh ones). */
BACKUP_MUST_CHECK
backup_result_t backup_seal_with(const backup_crypto_t *c, const char *password,
                                 uint32_t iterations, const uint8_t salt[16],
                                 const uint8_t nonce[12], const uint8_t *payload, size_t len,
                                 uint8_t *out, size_t cap, size_t *out_len);
/* Checks the header, derives the key and decrypts into payload (at least the header's
 * payload_len bytes). Nothing is returned unless the tag matches. */
BACKUP_MUST_CHECK
backup_result_t backup_open(const backup_crypto_t *c, const char *password, const uint8_t *file,
                            size_t len, uint8_t *payload, size_t cap, size_t *payload_len);
/* The header alone, without the password (how much room backup_open will need, and the
 * work it asks for). */
BACKUP_MUST_CHECK
backup_result_t backup_peek(const uint8_t *file, size_t len, uint32_t *payload_len,
                            uint32_t *iterations);

/* A password the installer may choose: BACKUP_PASSWORD_MIN..MAX bytes, printable. */
bool backup_password_ok(const char *password);

/* ---- what a backup holds -------------------------------------------------------------------- */
/* Whether an NVS entry belongs in a backup. The same answer decides what a restore replaces:
 * an entry the policy keeps is erased on the target unless the file has it, one it doesn't
 * keep is never touched (the panel's identity, its certificates and keys, the developer
 * enrollment, counters and anything in flight). */
bool backup_policy_keeps(const char *ns, const char *key);
/* The namespaces the policy keeps anything from, NULL-terminated. */
const char *const *backup_policy_namespaces(void);
/* An entry an older firmware put in its backups that this one no longer keeps (the update
 * source's old "ota"/"src"): a file with it is still restored, and the entry is left out. */
bool backup_policy_retired(const char *ns, const char *key);

/* What a restore writes for one of the file's entries (CFG-006, S05): the entry as it is
 * (*write true, *out = *e), nothing (*write false: a retired entry, or an update-server record
 * that doesn't decode - the panel then uses its own server), or the update-server record
 * ("ota"/"trust", alarm_proto's proto_fwsrv_trust_t) re-encoded into buf without its accepted
 * signing key. The server comes back; a key comes back only through the installer seeing its
 * fingerprint and confirming it again (docs/features/13 OTA-020): until then the panel trusts
 * its own image's key, and a release signed with another is refused. out->value may point
 * into buf (at least PROTO_FWSRV_TRUST_BLOB_MAX bytes) or into the file. */
BACKUP_MUST_CHECK
backup_result_t backup_restore_entry(const backup_entry_t *e, uint8_t *buf, size_t cap,
                                     backup_entry_t *out, bool *write);

/* ---- the panel's entries, through a port (the firmware's NVS; a fake in the tests) ----------- */
/* Only "nothing there" is absence: a namespace or entry that can't be read is a failure, never
 * an empty namespace (S04) - a backup missing it would look complete, and its restore would
 * erase what it lacks. */
typedef enum {
    BACKUP_STORE_OK = 0,
    BACKUP_STORE_ABSENT, /* no such namespace, no entries, or past the last one */
    BACKUP_STORE_FAILED, /* anything else */
} backup_store_status_t;

typedef struct {
    void *ctx;
    /* Opens ns to read its values. */
    backup_store_status_t (*open)(void *ctx, const char *ns);
    /* Lists ns's entries: the first, then the next (key and its BACKUP_T_* type, 0 for a kind
     * of entry a backup doesn't hold). ABSENT when there is none (more). */
    backup_store_status_t (*first)(void *ctx, const char *ns, char key[BACKUP_NAME_MAX + 1],
                                   uint8_t *type);
    backup_store_status_t (*next)(void *ctx, char key[BACKUP_NAME_MAX + 1], uint8_t *type);
    /* Ends the listing (also after ABSENT or FAILED). */
    void (*end)(void *ctx);
    /* The open namespace's value of key, as backup_entry_t holds it, into buf (cap bytes). */
    backup_store_status_t (*read)(void *ctx, const char *key, uint8_t type, uint8_t *buf,
                                  size_t cap, uint32_t *len);
    /* Closes what open opened. */
    void (*close)(void *ctx);
} backup_store_t;

/* Adds every entry of ns the policy keeps to w (value: cap bytes of scratch). OK also for a
 * namespace that isn't there; FAILED when the namespace can't be opened, listed or one of its
 * entries read; ABSENT never. *fits false when the writer refused an entry (too big, malformed). */
BACKUP_MUST_CHECK
backup_store_status_t backup_collect(backup_writer_t *w, const backup_store_t *s, const char *ns,
                                     uint8_t *value, size_t cap, bool *fits);

/* For a restore: up to max of ns's keys that it erases before writing the file's (the policy's,
 * but not alarm/config, which is written in place), into keys; *n of them. OK when *n < max is
 * all of them (*n == max: erase those and ask again); FAILED when the listing failed - then
 * nothing may be taken as cleared. */
BACKUP_MUST_CHECK
backup_store_status_t backup_erasable(const backup_store_t *s, const char *ns,
                                      char (*keys)[BACKUP_NAME_MAX + 1], size_t max, size_t *n);

/* A human-readable name for a result, for logs and the console. */
const char *backup_result_name(backup_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* BACKUP_H */
