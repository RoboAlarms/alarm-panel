/*
 * phone.c - direct mode and the panel's own push (see proto_phone.h).
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_phone.h"
#include "alarm_proto/proto_server.h"

const char *proto_push_level_name(proto_phone_level_t level)
{
    switch (level) {
    case PROTO_PUSH_ALARM:
        return "alarm";
    case PROTO_PUSH_ALARM_OVER:
        return "alarm_over";
    case PROTO_PUSH_CONNECTION:
        return "connection";
    case PROTO_PUSH_TROUBLE:
        return "trouble";
    case PROTO_PUSH_ACTIVITY:
    default:
        return "activity";
    }
}

void proto_phone_id(const uint8_t pub[PROTO_PHONE_KEY_LEN], char out[PROTO_PHONE_ID_LEN])
{
    uint8_t hash[32];
    proto_sha256(pub, PROTO_PHONE_KEY_LEN, hash);
    proto_hex(hash, 8u, out);
}

proto_phone_t *proto_phone_find(proto_phone_t *table, const char *id)
{
    for (size_t i = 0; i < PROTO_PHONE_MAX; i++) {
        char have[PROTO_PHONE_ID_LEN];
        if (table[i].used != 0u) {
            proto_phone_id(table[i].pub, have);
            if (strcmp(have, id) == 0) {
                return &table[i];
            }
        }
    }
    return NULL;
}

proto_phone_t *proto_phone_free_slot(proto_phone_t *table)
{
    for (size_t i = 0; i < PROTO_PHONE_MAX; i++) {
        if (table[i].used == 0u) {
            return &table[i];
        }
    }
    return NULL;
}

size_t proto_phone_remove_user(proto_phone_t *table, uint8_t user)
{
    size_t n = 0;
    for (size_t i = 0; i < PROTO_PHONE_MAX; i++) {
        if (table[i].used != 0u && table[i].user == user) {
            memset(&table[i], 0, sizeof(table[i]));
            n++;
        }
    }
    return n;
}

void proto_phone_window_open(proto_phone_window_t *w, const uint8_t token[PROTO_PHONE_TOKEN_LEN],
                             uint8_t user, uint64_t now_ms)
{
    memcpy(w->token, token, PROTO_PHONE_TOKEN_LEN);
    w->user = user;
    w->wrong = 0u;
    w->open = true;
    w->until_ms = now_ms + PROTO_PHONE_PAIR_MS;
}

bool proto_phone_window_use(proto_phone_window_t *w, const uint8_t token[PROTO_PHONE_TOKEN_LEN],
                            uint64_t now_ms)
{
    if (!w->open || now_ms >= w->until_ms) {
        w->open = false;
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < PROTO_PHONE_TOKEN_LEN; i++) {
        diff |= (uint8_t)(w->token[i] ^ token[i]);
    }
    if (diff != 0u) {
        w->wrong++;
        if (w->wrong >= PROTO_PHONE_PAIR_TRIES) {
            w->open = false;
        }
        return false;
    }
    w->open = false; /* once */
    return true;
}

bool proto_phone_pair_uri(const char *panel_id, const char *hosts, const uint8_t fp[32],
                          const uint8_t token[PROTO_PHONE_TOKEN_LEN], char *out, size_t cap)
{
    char hex[65];
    char tok[32];
    proto_hex(fp, 32u, hex);
    if (proto_srv_b64url(token, PROTO_PHONE_TOKEN_LEN, tok, sizeof(tok)) == 0u) {
        return false;
    }
    const int n = snprintf(out, cap, "roboalarms://pair?id=%s&h=%s&fp=%s&t=%s", panel_id, hosts, hex, tok);
    return n > 0 && (size_t)n < cap;
}

static size_t join0(const char *const *parts, size_t n, uint8_t *out, size_t cap)
{
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        const size_t l = strlen(parts[i]);
        const size_t sep = i + 1u < n ? 1u : 0u;
        if (used + l + sep > cap) {
            return 0u;
        }
        memcpy(out + used, parts[i], l);
        used += l;
        if (sep != 0u) {
            out[used++] = 0u;
        }
    }
    return used;
}

size_t proto_phone_session_bytes(const char *panel_id, const char *challenge, uint8_t *out,
                                 size_t cap)
{
    const char *const parts[] = {"roboalarms-phone-v1", panel_id, challenge};
    return join0(parts, 3u, out, cap);
}

size_t proto_phone_action_bytes(const char *panel_id, const char *challenge, const char *action,
                                uint8_t *out, size_t cap)
{
    const char *const parts[] = {"roboalarms-phone-action-v1", panel_id, challenge, action};
    return join0(parts, 4u, out, cap);
}

bool proto_phone_guarded(const char *method, const char *path, const char *action)
{
    if (strcmp(method, "GET") == 0) {
        return false;
    }
    if (strcmp(path, "partition") == 0) {
        return action != NULL && (strcmp(action, "disarm") == 0 ||
                                  strcmp(action, "acknowledge") == 0);
    }
    return strncmp(path, "people", 6) == 0 || strncmp(path, "alarm", 5) == 0 ||
           strncmp(path, "config/alarm", 12) == 0 || strncmp(path, "sources", 7) == 0 ||
           strncmp(path, "binding", 7) == 0;
}

size_t proto_push_aad(const char *handle, const char *level, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "ra-push-v1|%s|%s", handle, level);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0u;
}

void proto_push_plain(pj_writer_t *w, const proto_push_plain_t *p)
{
    pj_obj(w);
    pj_kint(w, "v", 1);
    pj_kstr(w, "title", p->title);
    pj_kstr(w, "body", p->body);
    pj_kstr(w, "loc", p->loc);
    pj_kstr(w, "loc_name", p->loc_name);
    pj_kstr(w, "kind", p->kind);
    pj_kstr(w, "ref", p->ref);
    pj_kstr(w, "at", p->at);
    pj_kstr(w, "category", p->category);
    pj_kstr(w, "thread", p->thread);
    pj_kstr(w, "sound", p->sound);
    pj_obj_end(w);
}

size_t proto_push_b64(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = (len + 2u) / 3u * 4u;
    if (need + 1u > cap) {
        return 0u;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3u) {
        const uint32_t a = in[i];
        const uint32_t b = i + 1u < len ? in[i + 1u] : 0u;
        const uint32_t c = i + 2u < len ? in[i + 2u] : 0u;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out[o++] = T[(v >> 18) & 63u];
        out[o++] = T[(v >> 12) & 63u];
        out[o++] = i + 1u < len ? T[(v >> 6) & 63u] : '=';
        out[o++] = i + 2u < len ? T[v & 63u] : '=';
    }
    out[o] = '\0';
    return o;
}

bool proto_push_digest(const void *body, size_t len, char *out, size_t cap)
{
    uint8_t hash[32];
    char b64[48];
    proto_sha256(body, len, hash);
    if (proto_push_b64(hash, sizeof(hash), b64, sizeof(b64)) == 0u) {
        return false;
    }
    const int n = snprintf(out, cap, "sha-256=:%s:", b64);
    return n > 0 && (size_t)n < cap;
}

bool proto_push_sig_params(int64_t created, const char *keyid, char *out, size_t cap)
{
    const int n = snprintf(out, cap,
                           "(\"@method\" \"@target-uri\" \"content-digest\");created=%lld;"
                           "keyid=\"%s\";alg=\"ed25519\"",
                           (long long)created, keyid);
    return n > 0 && (size_t)n < cap;
}

size_t proto_push_sig_base(const char *method, const char *target_uri, const char *digest,
                           const char *params, char *out, size_t cap)
{
    const int n = snprintf(out, cap,
                           "\"@method\": %s\n\"@target-uri\": %s\n\"content-digest\": %s\n"
                           "\"@signature-params\": %s",
                           method, target_uri, digest, params);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0u;
}

bool proto_phone_hears(alarm_authority_t authority, uint8_t partitions, uint8_t partition)
{
    if (authority != ALARM_AUTH_INSTALLER && authority != ALARM_AUTH_MASTER &&
        authority != ALARM_AUTH_PARTITION_MASTER && authority != ALARM_AUTH_STANDARD) {
        return false;
    }
    if (partition == 0u) {
        return true;
    }
    return partition <= 8u && (partitions & (1u << (partition - 1u))) != 0u;
}
