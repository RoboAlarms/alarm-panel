/* auto_store - the automations as stored (docs/features/41 DESIGN.md section 6, AUT-008).
 * See auto_store.h for the load outcomes and the order of writes. */
#include "automation/auto_store.h"

#include <string.h>

#define NONE AUTO_MAX /* find_uid's "no such automation" */

void auto_store_key(uint8_t slot, char out[AUTO_STORE_KEY_LEN])
{
    out[0] = 'a';
    out[1] = (char)('0' + (slot / 10u) % 10u);
    out[2] = (char)('0' + slot % 10u);
    out[3] = '\0';
}

static unsigned find_uid(const auto_store_t *s, uint16_t uid)
{
    if (uid == 0u) {
        return NONE;
    }
    for (unsigned i = 0; i < AUTO_MAX; i++) {
        if (s->used[i] && s->rules[i].uid == uid) {
            return i;
        }
    }
    return NONE;
}

/* compat | gen | next_uid, little-endian; a refused write is stored with the next change */
static bool write_meta(const auto_kv_port_t *kv, uint16_t gen, uint16_t next_uid)
{
    const uint8_t m[AUTO_META_LEN] = {
        (uint8_t)AUTO_META_COMPAT,
        (uint8_t)(gen & 0xFFu), (uint8_t)((unsigned)gen >> 8),
        (uint8_t)(next_uid & 0xFFu), (uint8_t)((unsigned)next_uid >> 8),
    };
    return kv->set != NULL && kv->set(kv->ctx, AUTO_STORE_KEY_META, m, sizeof m);
}

/* Meta first, as NVS will hold it after this change: false (nothing changed) when refused. */
static bool advance_meta(auto_store_t *s, const auto_kv_port_t *kv, bool add)
{
    const uint16_t gen = (uint16_t)((s->gen + 1u) & 0xFFFFu);
    const uint16_t next = add ? (uint16_t)((s->next_uid + 1u) & 0xFFFFu) /* 0 after 0xFFFF: no more */
                              : s->next_uid;
    if (!write_meta(kv, gen, next)) {
        return false;
    }
    s->gen = gen;
    s->next_uid = next;
    return true;
}

/* Reads meta: false when it makes the store read-only. */
static bool load_meta(auto_store_t *s, const auto_kv_port_t *kv, bool *present)
{
    uint8_t m[32]; /* a longer meta from newer firmware still reads: its first fields are ours */
    size_t len = 0u;
    *present = false;
    switch (kv->get(kv->ctx, AUTO_STORE_KEY_META, m, sizeof m, &len)) {
    case AUTO_KV_ABSENT:
        return true;
    case AUTO_KV_OK:
        break;
    case AUTO_KV_TOO_BIG:
    case AUTO_KV_ERROR:
    default:
        return false;
    }
    /* too short, never written (0, 0xFF erased) or a higher compat: not ours to rewrite */
    if (len < AUTO_META_LEN || m[0] == 0u || m[0] == 0xFFu || m[0] > AUTO_META_COMPAT) {
        return false;
    }
    s->gen = (uint16_t)((unsigned)m[1] | ((unsigned)m[2] << 8));
    s->next_uid = (uint16_t)((unsigned)m[3] | ((unsigned)m[4] << 8));
    *present = true;
    return true;
}

bool auto_store_load(auto_store_t *s, const auto_kv_port_t *kv)
{
    if (s == NULL) {
        return false;
    }
    memset(s, 0, sizeof *s);
    if (kv == NULL || kv->get == NULL) {
        s->read_only = true;
        return false;
    }
    bool meta = false;
    if (!load_meta(s, kv, &meta)) {
        s->read_only = true;
    }
    unsigned max_uid = 0u;
    for (unsigned i = 0; i < AUTO_MAX; i++) {
        char key[AUTO_STORE_KEY_LEN];
        auto_store_key((uint8_t)(i + 1u), key);
        size_t len = 0u;
        auto_rule_t *r = &s->rules[i];
        switch (kv->get(kv->ctx, key, s->bad[i], AUTO_RECORD_MAX, &len)) {
        case AUTO_KV_ABSENT:
            continue;
        case AUTO_KV_ERROR:
            s->read_only = true; /* what the slot holds is unknown: nothing may be written */
            continue;
        case AUTO_KV_TOO_BIG:
            /* longer than any record this firmware writes: newer software's, shown read-only */
            memset(r, 0, sizeof *r);
            r->rec = (uint8_t)AUTO_REC_NEWER;
            break;
        case AUTO_KV_OK:
        default:
            if (len > AUTO_RECORD_MAX) {
                len = AUTO_RECORD_MAX; /* a port that overstates: never read past the buffer */
            }
            if (auto_decode(s->bad[i], len, r) == AUTO_REC_BAD) {
                s->bad_len[i] = (uint16_t)len; /* kept as stored, never overwritten */
            }
            break;
        }
        r->slot = (uint8_t)(i + 1u);
        s->used[i] = true;
        if (r->uid > max_uid) {
            max_uid = r->uid;
        }
    }
    /* never hand out a uid a stored record has (a meta damaged, or restored behind its records) */
    const uint16_t after = (uint16_t)((max_uid + 1u) & 0xFFFFu); /* 0: every uid used */
    if (!meta) {
        s->gen = 0u;
        s->next_uid = max_uid == 0u ? 1u : after;
    } else if (s->next_uid != 0u && max_uid >= s->next_uid) {
        s->next_uid = after;
    }
    return !s->read_only;
}

/* Writes meta, then slot idx's new record; the rule in RAM changes only once its record is
 * stored (gen and next_uid once meta is). */
static auto_save_t commit(auto_store_t *s, const auto_kv_port_t *kv, unsigned idx,
                          const auto_rule_t *c, bool add)
{
    uint8_t buf[AUTO_RECORD_MAX];
    const size_t n = auto_encode(c, buf, sizeof buf);
    char key[AUTO_STORE_KEY_LEN];
    auto_store_key((uint8_t)(idx + 1u), key);
    if (n == 0u || kv->set == NULL || !advance_meta(s, kv, add)) {
        return AUTO_SAVE_FAILED;
    }
    if (!kv->set(kv->ctx, key, buf, n)) {
        return AUTO_SAVE_FAILED; /* the uid and the gen are spent: harmless */
    }
    s->rules[idx] = *c;
    s->used[idx] = true;
    s->bad_len[idx] = 0u;
    return AUTO_SAVE_OK;
}

auto_save_t auto_store_save(auto_store_t *s, const auto_kv_port_t *kv, const auto_rule_t *r,
                            const auto_env_t *env, uint8_t user, uint16_t expected_gen,
                            auto_check_t *why, uint16_t *uid_out)
{
    return auto_store_save_at(s, kv, r, env, user, expected_gen, 0u, why, uid_out);
}

auto_save_t auto_store_save_at(auto_store_t *s, const auto_kv_port_t *kv, const auto_rule_t *r,
                               const auto_env_t *env, uint8_t user, uint16_t expected_gen,
                               uint32_t now_epoch_s, auto_check_t *why, uint16_t *uid_out)
{
    if (why != NULL) {
        *why = AUTO_OK;
    }
    if (s == NULL || kv == NULL || r == NULL) {
        return AUTO_SAVE_FAILED;
    }
    if (s->read_only) {
        return AUTO_SAVE_READ_ONLY;
    }
    if (expected_gen != s->gen) {
        return AUTO_SAVE_CHANGED;
    }
    const bool add = r->uid == 0u;
    unsigned idx = NONE;
    if (add) {
        for (unsigned i = 0; i < AUTO_MAX; i++) {
            if (!s->used[i]) {
                idx = i;
                break;
            }
        }
        if (idx == NONE || s->next_uid == 0u) {
            return AUTO_SAVE_FULL;
        }
        if (kv->free_entries != NULL) {
            const size_t free_now = kv->free_entries(kv->ctx);
            if (free_now != AUTO_NVS_NOT_TOLD && free_now < AUTO_NVS_ADD_MIN) {
                return AUTO_SAVE_NO_ROOM;
            }
        }
    } else {
        idx = find_uid(s, r->uid);
        if (idx == NONE) {
            return AUTO_SAVE_NOT_FOUND;
        }
        if (s->rules[idx].rec != (uint8_t)AUTO_REC_OK) {
            return AUTO_SAVE_NOT_EDITABLE;
        }
    }

    auto_rule_t c = *r;
    c.uid = add ? s->next_uid : s->rules[idx].uid;
    c.slot = (uint8_t)(idx + 1u);
    c.rec = (uint8_t)AUTO_REC_OK;
    /* D23, AUT-011, AUT-015: every save makes the saver the maker (an edit makes the editor
     * it), never the request: an alarm action acts as this person, and a less secure one runs
     * only while this slot still holds a system master or installer. */
    c.made_by = user;
    c.saved_by = 0u;
    /* BL-224 (approved 2026-10-21): the installer may set up an arming automation; the
     * installer code never arms (USR-001), so it acts as the system master, checked by the
     * engine at each arming like any maker. saved_by keeps who set it up, for the words. */
    if (env != NULL && env->by_installer && c.n == 1u && c.then[0].kind == (uint8_t)AUTO_ACT_ARM) {
        c.made_by = (uint8_t)ALARM_USER_MASTER;
        c.saved_by = user;
    }
    if (add) {
        c.keep_len = 0u;
        memset(c.keep, 0, sizeof c.keep);
    } else {
        /* the fields newer software stored and this firmware doesn't know, written back */
        c.keep_len = s->rules[idx].keep_len;
        memcpy(c.keep, s->rules[idx].keep, sizeof c.keep);
    }
    /* D30: changed_at is the store's, never the request's: the time of the save that changed
     * the sentence (0 when the caller can't tell the time); an edit that reads the same keeps
     * the stored one. */
    c.changed_at = add || auto_rule_sentence_changed(&s->rules[idx], &c) ? now_epoch_s
                                                                          : s->rules[idx].changed_at;
    const auto_check_t check = auto_rule_check(&c, env);
    if (why != NULL) {
        *why = check;
    }
    if (check != AUTO_OK) {
        return AUTO_SAVE_INVALID;
    }
    const auto_save_t res = commit(s, kv, idx, &c, add);
    if (res == AUTO_SAVE_OK && uid_out != NULL) {
        *uid_out = c.uid;
    }
    return res;
}

auto_save_t auto_store_delete_slot(auto_store_t *s, const auto_kv_port_t *kv, uint8_t slot,
                                   uint16_t expected_gen)
{
    if (s == NULL || kv == NULL) {
        return AUTO_SAVE_FAILED;
    }
    if (s->read_only) {
        return AUTO_SAVE_READ_ONLY;
    }
    if (expected_gen != s->gen) {
        return AUTO_SAVE_CHANGED;
    }
    if (slot < 1u || slot > AUTO_MAX || !s->used[slot - 1u]) {
        return AUTO_SAVE_NOT_FOUND;
    }
    char key[AUTO_STORE_KEY_LEN];
    auto_store_key(slot, key);
    /* meta first; next_uid unchanged: a deleted uid is never handed out again */
    if (kv->erase == NULL || !advance_meta(s, kv, false)) {
        return AUTO_SAVE_FAILED;
    }
    if (!kv->erase(kv->ctx, key)) {
        return AUTO_SAVE_FAILED; /* the rule stays; its gen is spent */
    }
    const unsigned idx = slot - 1u;
    s->used[idx] = false;
    memset(&s->rules[idx], 0, sizeof s->rules[idx]);
    s->bad_len[idx] = 0u;
    return AUTO_SAVE_OK;
}

auto_save_t auto_store_delete(auto_store_t *s, const auto_kv_port_t *kv, uint16_t uid,
                              uint16_t expected_gen)
{
    if (s == NULL || kv == NULL) {
        return AUTO_SAVE_FAILED;
    }
    if (s->read_only) {
        return AUTO_SAVE_READ_ONLY;
    }
    if (expected_gen != s->gen) {
        return AUTO_SAVE_CHANGED;
    }
    const unsigned idx = find_uid(s, uid);
    if (idx == NONE) {
        return AUTO_SAVE_NOT_FOUND;
    }
    return auto_store_delete_slot(s, kv, (uint8_t)(idx + 1u), expected_gen);
}

auto_save_t auto_store_set_off(auto_store_t *s, const auto_kv_port_t *kv, uint16_t uid, bool off,
                               uint16_t expected_gen)
{
    if (s == NULL || kv == NULL) {
        return AUTO_SAVE_FAILED;
    }
    if (s->read_only) {
        return AUTO_SAVE_READ_ONLY;
    }
    if (expected_gen != s->gen) {
        return AUTO_SAVE_CHANGED;
    }
    const unsigned idx = find_uid(s, uid);
    if (idx == NONE) {
        return AUTO_SAVE_NOT_FOUND;
    }
    if (s->rules[idx].rec != (uint8_t)AUTO_REC_OK) {
        return AUTO_SAVE_NOT_EDITABLE;
    }
    if (s->rules[idx].off == off) {
        return AUTO_SAVE_OK; /* nothing to write */
    }
    auto_rule_t c = s->rules[idx];
    c.off = off;
    return commit(s, kv, idx, &c, false);
}

size_t auto_store_list(const auto_store_t *s, auto_rule_t *out, size_t max)
{
    size_t n = 0u;
    if (s == NULL || out == NULL) {
        return 0u;
    }
    for (unsigned i = 0; i < AUTO_MAX && n < max; i++) {
        if (s->used[i]) {
            out[n++] = s->rules[i];
        }
    }
    return n;
}

bool auto_store_get(const auto_store_t *s, uint16_t uid, auto_rule_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    const unsigned idx = find_uid(s, uid);
    if (idx == NONE) {
        return false;
    }
    *out = s->rules[idx];
    return true;
}
