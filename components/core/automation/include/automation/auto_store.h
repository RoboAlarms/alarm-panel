/* auto_store - the automations as stored (docs/features/41 DESIGN.md section 6 "Stored format",
 * AUT-008): the store's logic over a key-value port, so automation_service is a thin NVS wrapper
 * and the PC tests run every load outcome and every refusal.
 *
 * Namespace AUTO_STORE_NAMESPACE (the port opens it; the keys below are namespace-less):
 *   meta     compat u8 (1) | gen u16 LE | next_uid u16 LE
 *   a01..a16 one record per key (auto_encode's), so an edit rewrites one record.
 * Never renamed: stored panels hold them.
 *
 * Load outcomes (alarm_store.c's): an absent key is an empty slot; a record decodes OK, NEWER or
 * BROKEN (kept and shown; only OK is editable) or BAD (its bytes kept, never overwritten); any
 * other read error, or a meta this firmware can't read (a higher compat, too short), makes the
 * whole store read-only and every edit is refused. A missing meta beside records rebuilds
 * next_uid from the records (max uid + 1) and gen 0; a present one never lets next_uid fall to
 * or below a stored uid. Loading never writes: a rebuilt meta is stored with the next change.
 *
 * A change writes meta first - gen + 1 (one gen per change), and next_uid + 1 for an add - then
 * its record or its erase. A refused meta write changes nothing. A refused record write or erase
 * after it leaves the rule as it was but keeps the new gen and next_uid, as NVS has them: so a
 * power cut between the two writes never lets a page loaded before it save over the change, nor
 * hands a uid out twice. After AUTO_SAVE_FAILED the caller reads gen again.
 *
 * Portable C11, caller-owned state, no heap. The caller serializes every call.
 */
#ifndef AUTOMATION_AUTO_STORE_H
#define AUTOMATION_AUTO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_codec.h"
#include "automation/automation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTO_STORE_NAMESPACE "automations" /* 11 characters, within NVS's 15 */
#define AUTO_STORE_KEY_META  "meta"
#define AUTO_STORE_KEY_LEN   4u            /* "a01".."a16" and its NUL */
#define AUTO_META_COMPAT     1u            /* the newest meta this firmware reads and writes */
#define AUTO_META_LEN        5u            /* compat u8 | gen u16 | next_uid u16 */

/* NVS entries a blob of `bytes` costs, as a constant expression (auto_nvs_entries's count). */
#define AUTO_NVS_ENTRIES(bytes) (1u + ((bytes) + 3999u) / 4000u + ((bytes) + 31u) / 32u)

/* What the automations never take, modelled on zbz_panel.h's ZBZ_NVS_RESERVE / ZBZ_NVS_ADD_MIN:
 * the alarm configuration rewritten whole at its largest (NVS writes the new value before it
 * erases the old one) and one more record. Under AUTO_NVS_ADD_MIN free entries no automation is
 * added (an add costs a record and a meta rewrite on top of the reserve, and the first one the
 * namespace's own entry); a change of an existing record still writes - it frees its old entries
 * once written. */
#define AUTO_NVS_RESERVE \
    (AUTO_NVS_ENTRIES(ALARM_CONFIG_BLOB_MAX) + AUTO_NVS_ENTRIES(AUTO_RECORD_MAX))
#define AUTO_NVS_ADD_MIN \
    (AUTO_NVS_RESERVE + AUTO_NVS_ENTRIES(AUTO_RECORD_MAX) + AUTO_NVS_ENTRIES(AUTO_META_LEN) + 1u)
#define AUTO_NVS_NOT_TOLD SIZE_MAX /* free_entries' "unknown": no limit */

/* How a get went. */
typedef enum {
    AUTO_KV_OK = 0,   /* *len bytes read */
    AUTO_KV_ABSENT,   /* never stored */
    AUTO_KV_TOO_BIG,  /* stored, longer than cap: a record from newer firmware */
    AUTO_KV_ERROR,    /* stored but unreadable */
} auto_kv_t;

/* The key-value port (the service's NVS namespace; a fake in the tests). */
typedef struct {
    void *ctx;
    auto_kv_t (*get)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len);
    /* true when stored and committed */
    bool (*set)(void *ctx, const char *key, const uint8_t *data, size_t len);
    /* true when gone (an absent key included) */
    bool (*erase)(void *ctx, const char *key);
    /* free NVS entries now; AUTO_NVS_NOT_TOLD when unknown. NULL: no limit */
    size_t (*free_entries)(void *ctx);
} auto_kv_port_t;

/* What an edit came to. Append only (the web's answers map them). */
typedef enum {
    AUTO_SAVE_OK = 0,
    AUTO_SAVE_INVALID,       /* the validator refused it (*why says which) */
    AUTO_SAVE_CHANGED,       /* expected_gen isn't the store's: a stale page */
    AUTO_SAVE_FULL,          /* AUTO_MAX already stored (or every uid used) */
    AUTO_SAVE_NO_ROOM,       /* an add under AUTO_NVS_ADD_MIN free NVS entries */
    AUTO_SAVE_READ_ONLY,     /* the store loaded read-only */
    AUTO_SAVE_NOT_EDITABLE,  /* a NEWER, BROKEN or BAD record: only delete works */
    AUTO_SAVE_NOT_FOUND,     /* no automation with that uid (or slot) */
    AUTO_SAVE_FAILED,        /* the record's write was refused; nothing changed */
    AUTO_SAVE_LOCKED,        /* armed or in alarm (alarm_snapshot_locked): nobody changes them
                                then (AUT-010). automation_service's answer; auto_store never
                                gives it. Never stored */
} auto_save_t;

typedef struct {
    auto_rule_t rules[AUTO_MAX];             /* index slot - 1; valid where used */
    bool used[AUTO_MAX];
    uint16_t bad_len[AUTO_MAX];              /* a BAD record's bytes, as stored; 0 otherwise */
    uint8_t bad[AUTO_MAX][AUTO_RECORD_MAX];  /* also the load's read buffer */
    uint16_t gen;
    uint16_t next_uid;                       /* 0: every uid used */
    bool read_only;
} auto_store_t;

/* The key of slot 1..AUTO_MAX ("a01".."a16") into out[AUTO_STORE_KEY_LEN]. */
void auto_store_key(uint8_t slot, char out[AUTO_STORE_KEY_LEN]);

/* Reads meta and every record. Returns false when the store is read-only. */
bool auto_store_load(auto_store_t *s, const auto_kv_port_t *kv);

/* Adds (r->uid 0: a new uid, the first empty slot) or changes (r->uid of a stored OK record) an
 * automation. made_by is `user` for every save (an edit makes the editor the maker, D23) - never
 * the caller's - except an Arm saved by the installer (env->by_installer): it acts as the system
 * master (ALARM_USER_MASTER) and saved_by keeps `user` (BL-224, AUT-018); saved_by is 0 otherwise.
 * A change keeps the stored record's ignorable bytes. *why (may be NULL) gets the validator's
 * answer; *uid_out (may be NULL) the uid saved. */
auto_save_t auto_store_save(auto_store_t *s, const auto_kv_port_t *kv, const auto_rule_t *r,
                            const auto_env_t *env, uint8_t user, uint16_t expected_gen,
                            auto_check_t *why, uint16_t *uid_out);

/* auto_store_save, and the record's changed_at (D30): now_epoch_s on an add or when the edit
 * changes the sentence (auto_rule_sentence_changed), else the stored one is kept; the caller's
 * r->changed_at is never taken. now_epoch_s 0 = the clock isn't set (stored as unknown).
 * auto_store_save is this with 0. */
auto_save_t auto_store_save_at(auto_store_t *s, const auto_kv_port_t *kv, const auto_rule_t *r,
                               const auto_env_t *env, uint8_t user, uint16_t expected_gen,
                               uint32_t now_epoch_s, auto_check_t *why, uint16_t *uid_out);

/* Deletes by uid, or by slot (a BAD or a NEWER record may have no uid to name it by). */
auto_save_t auto_store_delete(auto_store_t *s, const auto_kv_port_t *kv, uint16_t uid,
                              uint16_t expected_gen);
auto_save_t auto_store_delete_slot(auto_store_t *s, const auto_kv_port_t *kv, uint8_t slot,
                                   uint16_t expected_gen);

/* Turns an OK record off or on: rewrites it, ignorable bytes kept. No change: OK, no write. */
auto_save_t auto_store_set_off(auto_store_t *s, const auto_kv_port_t *kv, uint16_t uid, bool off,
                               uint16_t expected_gen);

/* The readers: copies, never pointers into the store. list fills out[] in slot order and returns
 * how many (at most max); get returns false when no record has that uid. */
size_t auto_store_list(const auto_store_t *s, auto_rule_t *out, size_t max);
bool auto_store_get(const auto_store_t *s, uint16_t uid, auto_rule_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_AUTO_STORE_H */
