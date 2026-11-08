/*
 * server.c - the server link's messages, the remote ceiling and the replay queue (proto_server.h).
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_report.h"
#include "alarm_proto/proto_server.h"

/* ---- identity and sign-in -------------------------------------------------------------------- */

void proto_srv_id(const uint8_t pub[PROTO_SRV_KEY_LEN], char out[PROTO_SRV_ID_LEN])
{
    uint8_t hash[32];
    proto_sha256(pub, PROTO_SRV_KEY_LEN, hash);
    proto_hex(hash, 8u, out);
}

bool proto_srv_topic(const char *id, const char *suffix, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "ra/%s%s", id, suffix);
    return n > 0 && (size_t)n < cap;
}

bool proto_srv_host_norm(const char *host, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = host; *p != '\0' && *p != ':'; p++) {
        if (n + 1u >= cap) {
            return false;
        }
        const char c = *p;
        out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (cap == 0u) {
        return false;
    }
    out[n] = '\0';
    return n > 0u;
}

size_t proto_srv_sign_bytes(const char *host, const char *id, uint64_t counter, uint8_t *out,
                            size_t cap)
{
    static const char PREFIX[] = "roboalarms-mqtt-v1"; /* its NUL is the first separator */
    char norm[PROTO_SRV_HOST_LEN];
    if (!proto_srv_host_norm(host, norm, sizeof(norm))) {
        return 0u;
    }
    char num[24];
    snprintf(num, sizeof(num), "%llu", (unsigned long long)counter);
    const size_t hl = strlen(norm);
    const size_t il = strlen(id);
    const size_t nl = strlen(num);
    const size_t total = sizeof(PREFIX) + hl + 1u + il + 1u + nl;
    if (total > cap) {
        return 0u;
    }
    uint8_t *p = out;
    memcpy(p, PREFIX, sizeof(PREFIX));
    p += sizeof(PREFIX);
    memcpy(p, norm, hl + 1u);
    p += hl + 1u;
    memcpy(p, id, il + 1u);
    p += il + 1u;
    memcpy(p, num, nl);
    return total;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t proto_srv_b64url(const uint8_t *in, size_t len, char *out, size_t cap)
{
    const size_t need = len / 3u * 4u + (len % 3u == 0u ? 0u : len % 3u + 1u);
    if (len == 0u || need + 1u > cap) {
        return 0u;
    }
    size_t o = 0;
    size_t i = 0;
    for (; i + 3u <= len; i += 3u) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8) | in[i + 2u];
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = B64[(v >> 6) & 63u];
        out[o++] = B64[v & 63u];
    }
    if (len - i == 1u) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
    } else if (len - i == 2u) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8);
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = B64[(v >> 6) & 63u];
    }
    out[o] = '\0';
    return o;
}

size_t proto_srv_unb64url(const char *in, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t o = 0;
    for (const char *p = in; *p != '\0'; p++) {
        const char *at = strchr(B64, *p);
        if (at == NULL) {
            return 0u;
        }
        acc = (acc << 6) | (uint32_t)(at - B64);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (o >= cap) {
                return 0u;
            }
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (bits >= 6u || (acc & ((1u << bits) - 1u)) != 0u) {
        return 0u; /* a lone character, or stray bits at the end */
    }
    return o;
}

bool proto_srv_password(uint64_t counter, const uint8_t sig[PROTO_SRV_SIG_LEN],
                        const uint8_t pub[PROTO_SRV_KEY_LEN], char *out, size_t cap)
{
    const int n = snprintf(out, cap, "%llu.", (unsigned long long)counter);
    if (n <= 0 || (size_t)n >= cap) {
        return false;
    }
    size_t used = (size_t)n;
    const size_t s = proto_srv_b64url(sig, PROTO_SRV_SIG_LEN, out + used, cap - used);
    if (s == 0u || used + s + 1u >= cap) {
        return false;
    }
    used += s;
    out[used++] = '.';
    return proto_srv_b64url(pub, PROTO_SRV_KEY_LEN, out + used, cap - used) != 0u;
}

/* ---- the remote ceiling ---------------------------------------------------------------------- */

void proto_srv_ceiling_defaults(proto_srv_ceiling_t *c)
{
    memset(c, 0, sizeof(*c));
    c->remote_disarm = true;
    c->remote_setup = true;
    c->remote_people = true;
}

static bool user_ok(uint8_t user)
{
    return user >= 1u && user <= ALARM_MAX_USERS;
}

bool proto_srv_user_remote(const proto_srv_ceiling_t *c, uint8_t user)
{
    if (!user_ok(user)) {
        return false;
    }
    const unsigned i = user - 1u;
    return (c->remote_users[i / 8u] & (1u << (i % 8u))) != 0u;
}

void proto_srv_user_set_remote(proto_srv_ceiling_t *c, uint8_t user, bool on, int64_t until_ms)
{
    if (!user_ok(user)) {
        return;
    }
    const unsigned i = user - 1u;
    if (on) {
        c->remote_users[i / 8u] = (uint8_t)(c->remote_users[i / 8u] | (1u << (i % 8u)));
        c->until_ms[i] = until_ms;
    } else {
        c->remote_users[i / 8u] = (uint8_t)(c->remote_users[i / 8u] & ~(1u << (i % 8u)));
        c->until_ms[i] = 0;
    }
}

proto_srv_refusal_t proto_srv_check(const proto_srv_ceiling_t *c, uint8_t user, proto_srv_act_t act,
                                    int64_t now_ms)
{
    if (!proto_srv_user_remote(c, user)) {
        return PROTO_SRV_NOT_ALLOWED;
    }
    const int64_t until = c->until_ms[user - 1u];
    if (until != 0 && (now_ms <= 0 || now_ms >= until)) {
        return PROTO_SRV_EXPIRED;
    }
    switch (act) {
    case PROTO_SRV_ACT_DISARM:
        return c->remote_disarm ? PROTO_SRV_OK : PROTO_SRV_REMOTE_OFF;
    case PROTO_SRV_ACT_SETUP:
        return c->remote_setup ? PROTO_SRV_OK : PROTO_SRV_REMOTE_OFF;
    case PROTO_SRV_ACT_PEOPLE:
        return c->remote_people ? PROTO_SRV_OK : PROTO_SRV_REMOTE_OFF;
    case PROTO_SRV_ACT_ORDINARY:
    default:
        return PROTO_SRV_OK;
    }
}

const char *proto_srv_refusal_name(proto_srv_refusal_t r)
{
    switch (r) {
    case PROTO_SRV_NOT_ALLOWED:
        return "not_allowed";
    case PROTO_SRV_EXPIRED:
        return "expired";
    case PROTO_SRV_REMOTE_OFF:
        return "remote_off";
    case PROTO_SRV_OK:
    default:
        return "ok";
    }
}

/* ---- messages the panel sends ---------------------------------------------------------------- */

void proto_srv_hello(pj_writer_t *w, const proto_srv_hello_t *h)
{
    pj_obj(w);
    pj_kstr(w, "t", "hello");
    pj_kint(w, "api", PROTO_SRV_API);
    pj_kstr(w, "id", h->id);
    pj_kstr(w, "model", h->model);
    pj_kstr(w, "fw", h->fw);
    pj_karr(w, "caps");
    for (size_t i = 0; i < h->caps_n; i++) {
        pj_str(w, h->caps[i]);
    }
    pj_arr_end(w);
    pj_kstr(w, "pub", h->pub);
    pj_kstr(w, "seal_pub", h->seal_pub);
    pj_kstr(w, "seal_sig", h->seal_sig);
    pj_kbool(w, "linked", h->linked);
    pj_kuint(w, "boot", h->boot);
    pj_kuint(w, "uptime_ms", h->uptime_ms);
    if (h->now_ms > 0) {
        pj_kint(w, "now_ms", h->now_ms);
    } else {
        pj_knull(w, "now_ms");
    }
    pj_kstr(w, "clock", h->clock);
    pj_kstr(w, "tz", (h->tz != NULL && h->tz[0] != '\0') ? h->tz : NULL);
    pj_kuint(w, "last_seq", h->last_seq);
    pj_kuint(w, "oldest_seq", h->oldest_seq);
    pj_kstr(w, "path", h->path);
    pj_obj_end(w);
}

size_t proto_srv_seal_signed(const char *seal_pub, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "rs1-seal|%s", seal_pub);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0u;
}

void proto_srv_online(pj_writer_t *w, bool online)
{
    pj_obj(w);
    pj_kbool(w, "online", online);
    pj_obj_end(w);
}

const char *proto_srv_clock_name(uint8_t clock)
{
    switch (clock) {
    case PROTO_SRV_CLOCK_RTC:
        return "rtc";
    case PROTO_SRV_CLOCK_NTP:
        return "ntp";
    case PROTO_SRV_CLOCK_MANUAL:
        return "manual";
    case PROTO_SRV_CLOCK_SERVER:
        return "server";
    case PROTO_SRV_CLOCK_NONE:
    default:
        return "none";
    }
}

void proto_srv_event(pj_writer_t *w, const alarm_config_t *cfg, const elog_entry_t *e, bool replayed,
                     const char *title, const char *body, const proto_srv_actor_t *actor)
{
    pj_obj(w);
    pj_kstr(w, "t", "event");
    proto_event_fields(w, cfg, &e->ev, replayed);
    if (e->ev.epoch_ms > 0) {
        pj_kint(w, "at_ms", e->ev.epoch_ms);
    } else {
        pj_knull(w, "at_ms");
    }
    pj_kuint(w, "boot", e->boot);
    pj_kuint(w, "uptime_ms", e->ev.mono_ms);
    pj_kstr(w, "clock", proto_srv_clock_name(e->clock));
    pj_kbool(w, "silent", (e->ev.flags & ALARM_EVF_SILENT) != 0u);
    if (title != NULL) {
        pj_kobj(w, "words");
        pj_kstr(w, "key", alarm_event_name(e->ev.type));
        pj_kstr(w, "text", title);
        pj_kstr(w, "body", body);
        pj_obj_end(w);
    }
    if (actor != NULL && (actor->name[0] != '\0' || actor->account[0] != '\0')) {
        pj_kobj(w, "actor");
        pj_kstr(w, "account", actor->account);
        pj_kstr(w, "name", actor->name);
        pj_kstr(w, "via", actor->via);
        pj_obj_end(w);
    }
    pj_obj_end(w);
}

void proto_srv_actors_note(proto_srv_actors_t *t, uint32_t before_seq, uint32_t after_seq,
                           const proto_srv_actor_t *actor)
{
    if (after_seq <= before_seq || actor == NULL ||
        (actor->name[0] == '\0' && actor->account[0] == '\0')) {
        return;
    }
    proto_srv_actor_run_t *r = &t->run[t->next];
    t->next = (uint8_t)((t->next + 1u) % PROTO_SRV_ACTOR_RUNS);
    r->used = true;
    r->first_seq = before_seq + 1u;
    r->last_seq = after_seq;
    r->actor = *actor;
}

const proto_srv_actor_t *proto_srv_actors_find(const proto_srv_actors_t *t, const alarm_event_t *ev)
{
    if (ev->source.kind != ALARM_SRC_REMOTE ||
        (ev->source.id != ALARM_SRC_ID_SERVER_APP && ev->source.id != ALARM_SRC_ID_SERVER_WEB)) {
        return NULL;
    }
    for (size_t i = 0; i < PROTO_SRV_ACTOR_RUNS; i++) {
        const proto_srv_actor_run_t *r = &t->run[i];
        if (r->used && ev->seq >= r->first_seq && ev->seq <= r->last_seq) {
            return &r->actor;
        }
    }
    return NULL;
}

void proto_srv_snapshot(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                        const proto_when_t *when, const proto_srv_panel_t *panel)
{
    proto_link_snapshot_open(w, cfg, snap, when);
    pj_kobj(w, "panel");
    pj_kstr(w, "name", panel->name);
    pj_kstr(w, "tz", panel->tz);
    pj_kstr(w, "region", panel->region);
    pj_kstr(w, "fw", panel->fw);
    pj_kstr(w, "power", panel->power);
    if (panel->battery_pct >= 0) {
        pj_kint(w, "battery", panel->battery_pct);
    } else {
        pj_knull(w, "battery");
    }
    pj_kbool(w, "setup_mode", panel->setup_mode);
    pj_obj_end(w);
    pj_obj_end(w);
}

void proto_srv_people(pj_writer_t *w, const alarm_config_t *cfg, const proto_srv_ceiling_t *c,
                      const uint8_t *browser)
{
    static const uint8_t NO_CODE[ALARM_CODE_HASH_LEN] = {0};
    pj_obj(w);
    pj_kstr(w, "t", "people");
    pj_karr(w, "users");
    for (unsigned u = 1u; u <= ALARM_MAX_USERS; u++) {
        const alarm_user_t *user = &cfg->users[u - 1u];
        if (user->authority == ALARM_AUTH_NONE) {
            continue;
        }
        pj_obj(w);
        pj_kuint(w, "number", u);
        pj_kstr(w, "name", user->name);
        pj_kstr(w, "authority", alarm_authority_name(user->authority));
        pj_karr(w, "partitions");
        for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
            if ((user->partitions & (1u << p)) != 0u) {
                pj_uint(w, p + 1u);
            }
        }
        pj_arr_end(w);
        pj_kbool(w, "has_code", memcmp(user->code_hash, NO_CODE, sizeof(NO_CODE)) != 0);
        pj_kbool(w, "browser",
                 browser != NULL && (browser[(u - 1u) / 8u] & (1u << ((u - 1u) % 8u))) != 0u);
        pj_kbool(w, "remote", proto_srv_user_remote(c, (uint8_t)u));
        if (c->until_ms[u - 1u] != 0) {
            pj_kint(w, "until_ms", c->until_ms[u - 1u]);
        } else {
            pj_knull(w, "until_ms");
        }
        pj_obj_end(w);
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

void proto_srv_ceiling(pj_writer_t *w, const proto_srv_ceiling_t *c)
{
    pj_obj(w);
    pj_kstr(w, "t", "ceiling");
    pj_kuint(w, "rev", c->rev);
    pj_karr(w, "remote_users");
    for (unsigned u = 1u; u <= ALARM_MAX_USERS; u++) {
        if (proto_srv_user_remote(c, (uint8_t)u)) {
            pj_uint(w, u);
        }
    }
    pj_arr_end(w);
    pj_kbool(w, "remote_disarm", c->remote_disarm);
    pj_kbool(w, "remote_setup", c->remote_setup);
    pj_kbool(w, "remote_people", c->remote_people);
    pj_kbool(w, "ask_code", c->ask_code);
    pj_obj_end(w);
}

void proto_srv_health(pj_writer_t *w, const proto_srv_health_t *h)
{
    pj_obj(w);
    pj_kstr(w, "t", "health");
    pj_kstr(w, "power", h->power);
    if (h->battery_pct >= 0) {
        pj_kint(w, "battery_pct", h->battery_pct);
    } else {
        pj_knull(w, "battery_pct");
    }
    pj_kint(w, "rssi", h->rssi);
    pj_kuint(w, "uptime_s", h->uptime_s);
    pj_kuint(w, "heap_kb", h->heap_kb);
    pj_kstr(w, "clock", h->clock);
    pj_kstr(w, "path", h->path);
    pj_obj_end(w);
}

void proto_srv_config(pj_writer_t *w, const char *domain, uint32_t rev)
{
    pj_obj(w);
    pj_kstr(w, "t", "config");
    pj_kstr(w, "domain", domain);
    pj_kuint(w, "rev", rev);
    pj_obj_end(w);
}

static void type_only(pj_writer_t *w, const char *t)
{
    pj_obj(w);
    pj_kstr(w, "t", t);
    pj_obj_end(w);
}

void proto_srv_link_start(pj_writer_t *w)
{
    type_only(w, "link_start");
}

void proto_srv_link_cancel(pj_writer_t *w)
{
    type_only(w, "link_cancel");
}

void proto_srv_unlink(pj_writer_t *w)
{
    type_only(w, "unlink");
}

void proto_srv_link_done(pj_writer_t *w, const char *claim, bool ok, uint8_t user)
{
    pj_obj(w);
    pj_kstr(w, "t", "link_done");
    pj_kstr(w, "claim", claim);
    pj_kbool(w, "ok", ok);
    pj_knum(w, "user", user);
    pj_obj_end(w);
}

void proto_srv_res(pj_writer_t *w, const char *id, int status, const char *error)
{
    pj_obj(w);
    pj_kstr(w, "t", "res");
    pj_kstr(w, "id", id);
    pj_kint(w, "status", status);
    if (error != NULL) {
        pj_kobj(w, "body");
        pj_kstr(w, "error", error);
        pj_obj_end(w);
    }
    pj_obj_end(w);
}

void proto_srv_result(pj_writer_t *w, const proto_srv_result_t *r)
{
    pj_obj(w);
    pj_kstr(w, "t", "result");
    pj_kstr(w, "id", r->id);
    pj_kstr(w, "action", r->action);
    pj_kstr(w, "result", r->result);
    pj_knum(w, "user", r->user);
    pj_knum(w, "zone", r->zone);
    pj_kstr(w, "words", r->words);
    pj_knum(w, "partition", r->partition);
    if (r->state != NULL) {
        pj_kstr(w, "state", r->state);
        pj_kstr(w, "level", r->level);
        pj_kuint(w, "delay_left_ms", r->delay_left_ms);
    }
    pj_obj_end(w);
}

/* ---- messages the panel receives ------------------------------------------------------------- */

static bool get_u8(pj_reader_t *r, uint8_t *out)
{
    int64_t v;
    if (!pj_get_int(r, &v) || v < 0 || v > UINT8_MAX) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static void parse_time(pj_reader_t *r, proto_srv_in_t *out)
{
    proto_srv_time_t *t = &out->u.time;
    char key[20];
    int64_t v;
    bool have = false;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "server_ms") == 0 && pj_peek(r) == PJ_NUMBER && pj_get_int(r, &v)) {
            t->server_ms = v;
            have = v > 0;
        } else if (strcmp(key, "echo_uptime_ms") == 0 && pj_peek(r) == PJ_NUMBER &&
                   pj_get_int(r, &v) && v >= 0) {
            t->echo_uptime_ms = (uint64_t)v;
        } else if (strcmp(key, "ack_seq") == 0 && pj_peek(r) == PJ_NUMBER && pj_get_int(r, &v) &&
                   v >= 0 && v <= (int64_t)UINT32_MAX) {
            t->ack_seq = (uint32_t)v;
        } else if (strcmp(key, "location_id") == 0 && pj_peek(r) == PJ_STRING) {
            (void)pj_get_str(r, t->location_id, sizeof(t->location_id));
        }
    }
    out->type = have ? PROTO_SRV_IN_TIME : PROTO_SRV_IN_BAD;
}

static void parse_command(pj_reader_t *r, proto_srv_in_t *out)
{
    proto_srv_command_t *c = &out->u.command;
    char key[20];
    int64_t v;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            (void)pj_get_str(r, c->id, sizeof(c->id));
        } else if (strcmp(key, "as") == 0) {
            (void)get_u8(r, &c->as);
        } else if (strcmp(key, "action") == 0) {
            (void)pj_get_str(r, c->action, sizeof(c->action));
        } else if (strcmp(key, "partition") == 0) {
            (void)get_u8(r, &c->partition);
        } else if (strcmp(key, "level") == 0) {
            (void)pj_get_str(r, c->level, sizeof(c->level));
        } else if (strcmp(key, "zone") == 0 && pj_peek(r) == PJ_NUMBER) {
            if (pj_get_int(r, &v) && v >= 0 && v <= UINT16_MAX) {
                c->zone = (uint16_t)v;
            }
        } else if (strcmp(key, "no_entry_delay") == 0) {
            (void)pj_get_bool(r, &c->no_entry_delay);
        } else if (strcmp(key, "silent_exit") == 0) {
            (void)pj_get_bool(r, &c->silent_exit);
        } else if (strcmp(key, "bypass_open") == 0) {
            (void)pj_get_bool(r, &c->bypass_open);
        } else if (strcmp(key, "sealed_code") == 0 && pj_peek(r) == PJ_STRING) {
            (void)pj_get_str(r, c->sealed_code, sizeof(c->sealed_code));
        } else if (strcmp(key, "actor") == 0 && pj_peek(r) == PJ_OBJECT && pj_enter_obj(r)) {
            char k2[12];
            while (pj_next_member(r, k2, sizeof(k2))) {
                if (strcmp(k2, "name") == 0) {
                    (void)pj_get_str(r, c->actor_name, sizeof(c->actor_name));
                } else if (strcmp(k2, "via") == 0) {
                    (void)pj_get_str(r, c->actor_via, sizeof(c->actor_via));
                } else if (strcmp(k2, "account") == 0) {
                    (void)pj_get_str(r, c->actor_account, sizeof(c->actor_account));
                }
            }
        }
    }
    out->type = c->id[0] != '\0' && c->action[0] != '\0' ? PROTO_SRV_IN_COMMAND : PROTO_SRV_IN_BAD;
}

static void parse_req(pj_reader_t *r, proto_srv_in_t *out)
{
    proto_srv_req_t *q = &out->u.req;
    char key[12];
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            (void)pj_get_str(r, q->id, sizeof(q->id));
        } else if (strcmp(key, "as") == 0) {
            (void)get_u8(r, &q->as);
        } else if (strcmp(key, "method") == 0) {
            (void)pj_get_str(r, q->method, sizeof(q->method));
        } else if (strcmp(key, "path") == 0) {
            (void)pj_get_str(r, q->path, sizeof(q->path));
        } else if (strcmp(key, "fresh") == 0) {
            (void)pj_get_bool(r, &q->fresh);
        } else if (strcmp(key, "actor") == 0 && pj_peek(r) == PJ_OBJECT && pj_enter_obj(r)) {
            char k2[12];
            while (pj_next_member(r, k2, sizeof(k2))) {
                if (strcmp(k2, "name") == 0) {
                    (void)pj_get_str(r, q->actor.name, sizeof(q->actor.name));
                } else if (strcmp(k2, "via") == 0) {
                    (void)pj_get_str(r, q->actor.via, sizeof(q->actor.via));
                } else if (strcmp(k2, "account") == 0) {
                    (void)pj_get_str(r, q->actor.account, sizeof(q->actor.account));
                }
            }
        } else if (strcmp(key, "body") == 0 && pj_peek(r) != PJ_NULL) {
            (void)pj_peek(r); /* positions at the value's first byte */
            const char *start = r->p;
            pj_skip(r);
            if (!r->error) {
                q->body = start;
                q->body_len = (size_t)(r->p - start);
            }
        }
    }
    out->type = q->id[0] != '\0' && q->path[0] != '\0' ? PROTO_SRV_IN_REQ : PROTO_SRV_IN_BAD;
}

static void parse_people_link(pj_reader_t *r, proto_srv_in_t *out)
{
    proto_srv_people_link_t *l = &out->u.people_link;
    char key[12];
    int64_t v;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
            (void)pj_get_str(r, l->id, sizeof(l->id));
        } else if (strcmp(key, "user") == 0) {
            (void)get_u8(r, &l->user);
        } else if (strcmp(key, "on") == 0) {
            (void)pj_get_bool(r, &l->on);
        } else if (strcmp(key, "until_ms") == 0 && pj_peek(r) == PJ_NUMBER) {
            if (pj_get_int(r, &v) && v > 0) {
                l->until_ms = v;
            }
        } else if (strcmp(key, "as") == 0) {
            (void)get_u8(r, &l->as);
        } else if (strcmp(key, "fresh") == 0) {
            (void)pj_get_bool(r, &l->fresh);
        }
    }
    out->type = l->id[0] != '\0' && l->user != 0u ? PROTO_SRV_IN_PEOPLE_LINK : PROTO_SRV_IN_BAD;
}

void proto_srv_parse(const char *json, size_t len, bool is_ack, bool is_time, proto_srv_in_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = PROTO_SRV_IN_BAD;
    /* First the type: it can come after other keys, so one pass finds it and a second reads. */
    char t[16] = "";
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return;
    }
    char key[24];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "t") == 0) {
            (void)pj_get_str(&r, t, sizeof(t));
        }
    }
    if (r.error || !pj_finished(&r)) {
        return;
    }
    if (t[0] == '\0') {
        if (is_ack) {
            strcpy(t, "ack");
        } else if (is_time) {
            strcpy(t, "time");
        }
    }
    pj_read_init(&r, json, len);
    (void)pj_enter_obj(&r);
    int64_t v;
    if (strcmp(t, "time") == 0) {
        parse_time(&r, out);
    } else if (strcmp(t, "ack") == 0) {
        while (pj_next_member(&r, key, sizeof(key))) {
            if (strcmp(key, "seq") == 0 && pj_get_int(&r, &v) && v >= 0 &&
                v <= (int64_t)UINT32_MAX) {
                out->u.ack_seq = (uint32_t)v;
                out->type = PROTO_SRV_IN_ACK;
            }
        }
    } else if (strcmp(t, "resync") == 0) {
        out->type = PROTO_SRV_IN_RESYNC;
    } else if (strcmp(t, "unsupported") == 0) {
        out->type = PROTO_SRV_IN_UNSUPPORTED;
    } else if (strcmp(t, "unlink") == 0) {
        out->type = PROTO_SRV_IN_UNLINK;
    } else if (strcmp(t, "contacts") == 0) {
        out->type = PROTO_SRV_IN_CONTACTS;
    } else if (strcmp(t, "link_code") == 0) {
        proto_srv_link_code_t *c = &out->u.link_code;
        while (pj_next_member(&r, key, sizeof(key))) {
            if (strcmp(key, "code") == 0) {
                (void)pj_get_str(&r, c->code, sizeof(c->code));
            } else if (strcmp(key, "url") == 0) {
                (void)pj_get_str(&r, c->url, sizeof(c->url));
            } else if (strcmp(key, "expires_s") == 0 && pj_get_int(&r, &v) && v > 0 &&
                       v <= 86400) {
                c->expires_s = (uint32_t)v;
            }
        }
        out->type = c->code[0] != '\0' ? PROTO_SRV_IN_LINK_CODE : PROTO_SRV_IN_BAD;
    } else if (strcmp(t, "link_claim") == 0) {
        proto_srv_link_claim_t *c = &out->u.link_claim;
        while (pj_next_member(&r, key, sizeof(key))) {
            if (strcmp(key, "claim") == 0) {
                (void)pj_get_str(&r, c->claim, sizeof(c->claim));
            } else if (strcmp(key, "account_name") == 0) {
                (void)pj_get_str(&r, c->account_name, sizeof(c->account_name));
            } else if (strcmp(key, "account_email_masked") == 0) {
                (void)pj_get_str(&r, c->account_email_masked, sizeof(c->account_email_masked));
            } else if (strcmp(key, "location_name") == 0) {
                (void)pj_get_str(&r, c->location_name, sizeof(c->location_name));
            }
        }
        out->type = c->claim[0] != '\0' ? PROTO_SRV_IN_LINK_CLAIM : PROTO_SRV_IN_BAD;
    } else if (strcmp(t, "linked") == 0) {
        while (pj_next_member(&r, key, sizeof(key))) {
            if (strcmp(key, "location_id") == 0) {
                (void)pj_get_str(&r, out->u.location_id, sizeof(out->u.location_id));
            }
        }
        out->type = out->u.location_id[0] != '\0' ? PROTO_SRV_IN_LINKED : PROTO_SRV_IN_BAD;
    } else if (strcmp(t, "command") == 0) {
        parse_command(&r, out);
    } else if (strcmp(t, "req") == 0) {
        parse_req(&r, out);
    } else if (strcmp(t, "people_link") == 0) {
        parse_people_link(&r, out);
    } else {
        out->type = PROTO_SRV_IN_UNKNOWN;
    }
}

/* ---- commands --------------------------------------------------------------------------------- */

bool proto_srv_command_map(const proto_srv_command_t *in, alarm_cmd_t *cmd, proto_srv_when_t *when,
                           proto_srv_act_t *act)
{
    /* The Panel Manager's POST partition table (WEB-024), and "silence" for "acknowledge". */
    static const struct {
        const char *name;
        alarm_cmd_type_t type;
        proto_srv_when_t when;
        alarm_walk_op_t walk_op;
        proto_srv_act_t act;
    } ACTIONS[] = {
        {"arm", ALARM_CMD_ARM, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_TOGGLE, PROTO_SRV_ACT_ORDINARY},
        {"disarm", ALARM_CMD_DISARM, PROTO_SRV_WHEN_NOT_DISARMED, ALARM_WALK_TOGGLE,
         PROTO_SRV_ACT_DISARM},
        {"clear", ALARM_CMD_DISARM, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_TOGGLE,
         PROTO_SRV_ACT_ORDINARY},
        {"acknowledge", ALARM_CMD_SILENCE, PROTO_SRV_WHEN_ANY, ALARM_WALK_TOGGLE,
         PROTO_SRV_ACT_DISARM},
        {"silence", ALARM_CMD_SILENCE, PROTO_SRV_WHEN_ANY, ALARM_WALK_TOGGLE, PROTO_SRV_ACT_DISARM},
        {"chime", ALARM_CMD_CHIME, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_TOGGLE,
         PROTO_SRV_ACT_ORDINARY},
        {"bypass", ALARM_CMD_BYPASS, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_TOGGLE,
         PROTO_SRV_ACT_ORDINARY},
        {"unbypass", ALARM_CMD_UNBYPASS, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_TOGGLE,
         PROTO_SRV_ACT_ORDINARY},
        {"walk_test_start", ALARM_CMD_WALK_TEST, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_START,
         PROTO_SRV_ACT_ORDINARY},
        {"walk_test_end", ALARM_CMD_WALK_TEST, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_END,
         PROTO_SRV_ACT_ORDINARY},
        {"walk_test_extend", ALARM_CMD_WALK_TEST, PROTO_SRV_WHEN_DISARMED, ALARM_WALK_EXTEND,
         PROTO_SRV_ACT_ORDINARY},
    };
    static const alarm_arm_level_t LEVELS[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY,
                                               ALARM_LEVEL_NIGHT};
    memset(cmd, 0, sizeof(*cmd));
    size_t a = 0;
    const size_t n = sizeof(ACTIONS) / sizeof(ACTIONS[0]);
    while (a < n && strcmp(in->action, ACTIONS[a].name) != 0) {
        a++;
    }
    if (a == n || in->partition < 1u || in->partition > ALARM_MAX_PARTITIONS) {
        return false;
    }
    cmd->type = ACTIONS[a].type;
    cmd->partition = in->partition;
    *when = ACTIONS[a].when;
    *act = ACTIONS[a].act;
    if (cmd->type == ALARM_CMD_WALK_TEST) {
        cmd->walk_op = (uint8_t)ACTIONS[a].walk_op;
    } else if (cmd->type == ALARM_CMD_ARM) {
        size_t l = 0;
        const size_t ln = sizeof(LEVELS) / sizeof(LEVELS[0]);
        while (l < ln && strcmp(in->level, alarm_level_name(LEVELS[l])) != 0) {
            l++;
        }
        if (l == ln) {
            return false;
        }
        cmd->level = LEVELS[l];
        cmd->arm_flags = (uint8_t)((in->no_entry_delay ? ALARM_ARM_NO_ENTRY_DELAY : 0u) |
                                   (in->silent_exit ? ALARM_ARM_SILENT_EXIT : 0u) |
                                   (in->bypass_open ? ALARM_ARM_BYPASS_FAULTED : 0u));
    } else if (cmd->type == ALARM_CMD_BYPASS || cmd->type == ALARM_CMD_UNBYPASS) {
        if (in->zone < 1u || in->zone > ALARM_MAX_ZONES) {
            return false;
        }
        cmd->zone = in->zone;
    }
    return true;
}

proto_srv_run_t proto_srv_command_run(proto_srv_act_t act, bool ask_code, bool sealed)
{
    if (act != PROTO_SRV_ACT_DISARM) {
        return PROTO_SRV_RUN_AS_USER;
    }
    if (sealed) {
        return PROTO_SRV_RUN_WITH_CODE;
    }
    return ask_code ? PROTO_SRV_RUN_CODE_REQUIRED : PROTO_SRV_RUN_AS_USER;
}

proto_srv_act_t proto_srv_req_act(const char *method, const char *path)
{
    if (strcmp(method, "GET") == 0) {
        return PROTO_SRV_ACT_ORDINARY;
    }
    if (strncmp(path, "people", 6) == 0 || strncmp(path, "setup/people", 12) == 0) {
        return PROTO_SRV_ACT_PEOPLE;
    }
    /* Installer programming, as the touchscreen keeps it behind Setup mode; the installer and
     * master codes too (set there without the old one, by the installer or the Owner). */
    if (strncmp(path, "setup/alarm", 11) == 0 || strncmp(path, "setup-mode", 10) == 0 ||
        strncmp(path, "zwave", 5) == 0 || strncmp(path, "zigbee", 6) == 0 ||
        strncmp(path, "keypads", 7) == 0 || strncmp(path, "alarm", 5) == 0 ||
        strncmp(path, "sources", 7) == 0 || strncmp(path, "binding", 7) == 0 ||
        strncmp(path, "installer-code", 14) == 0) {
        return PROTO_SRV_ACT_SETUP;
    }
    return PROTO_SRV_ACT_ORDINARY;
}

const char *proto_srv_req_refusal(const char *method, const char *path, bool fresh)
{
    if (strcmp(method, "GET") == 0) {
        return NULL;
    }
    const size_t n = strcspn(path, "?");
    if (n == 9u && strncmp(path, "partition", 9) == 0) {
        return "use_command";
    }
    const proto_srv_act_t act = proto_srv_req_act(method, path);
    if ((act == PROTO_SRV_ACT_PEOPLE || act == PROTO_SRV_ACT_SETUP) && !fresh) {
        return "step_up";
    }
    return NULL;
}

/* The field up to the next '|' (or the end, when last), copied into out; NULL when it doesn't fit. */
static const char *sealed_field(const char *p, bool last, char *out, size_t cap)
{
    const char *end = last ? p + strlen(p) : strchr(p, '|');
    if (end == NULL || (size_t)(end - p) >= cap) {
        return NULL;
    }
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return last ? end : end + 1;
}

bool proto_srv_sealed_code(const char *plain, int64_t now_ms, const proto_srv_command_t *in,
                           char *code, size_t cap)
{
    return proto_srv_sealed_for(plain, now_ms, in->action, in->partition, code, cap);
}

bool proto_srv_sealed_for(const char *plain, int64_t now_ms, const char *action_for, unsigned target,
                          char *code, size_t cap)
{
    if (cap > 0u) {
        code[0] = '\0';
    }
    if (now_ms <= 0 || strncmp(plain, "rs1|", 4) != 0) {
        return false;
    }
    char at[20];
    char action[20];
    char part[4];
    char digits[ALARM_CODE_MAX_LEN + 1];
    const char *p = sealed_field(plain + 4, false, at, sizeof(at));
    p = p != NULL ? sealed_field(p, false, action, sizeof(action)) : NULL;
    p = p != NULL ? sealed_field(p, false, part, sizeof(part)) : NULL;
    p = p != NULL ? sealed_field(p, true, digits, sizeof(digits)) : NULL;
    if (p == NULL || at[0] == '\0' || part[0] == '\0') {
        return false;
    }
    int64_t ms = 0;
    for (const char *c = at; *c != '\0'; c++) {
        if (*c < '0' || *c > '9' || ms > INT64_MAX / 20) {
            return false;
        }
        ms = ms * 10 + (*c - '0');
    }
    unsigned partition = 0u;
    for (const char *c = part; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        partition = partition * 10u + (unsigned)(*c - '0');
    }
    const int64_t age = now_ms > ms ? now_ms - ms : ms - now_ms;
    const size_t n = strlen(digits);
    if (age > PROTO_SRV_SEALED_MS || strcmp(action, action_for) != 0 || partition != target ||
        n < ALARM_CODE_MIN_LEN || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
    }
    memcpy(code, digits, n + 1u);
    memset(digits, 0, sizeof(digits));
    return true;
}

alarm_authority_t proto_srv_role_authority(const char *role)
{
    static const struct {
        const char *role;
        alarm_authority_t authority;
    } ROLES[] = {
        {"owner", ALARM_AUTH_MASTER},
        {"master", ALARM_AUTH_MASTER},
        {"admin", ALARM_AUTH_INSTALLER},
        {"installer", ALARM_AUTH_INSTALLER},
        {"area_manager", ALARM_AUTH_PARTITION_MASTER},
        {"partition_master", ALARM_AUTH_PARTITION_MASTER},
        {"guest", ALARM_AUTH_GUEST},
        {"arm_only", ALARM_AUTH_ARM_ONLY},
        {"duress", ALARM_AUTH_DURESS},
    };
    for (size_t i = 0; i < sizeof(ROLES) / sizeof(ROLES[0]); i++) {
        if (strcmp(role, ROLES[i].role) == 0) {
            return ROLES[i].authority;
        }
    }
    return ALARM_AUTH_STANDARD;
}

void proto_srv_people_parse(const char *json, size_t len, proto_srv_people_t *out)
{
    memset(out, 0, sizeof(*out));
    char action[12] = "";
    char role[20] = "";
    bool have_parts = false;
    bool bad = false;
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return;
    }
    char key[16];
    int64_t v;
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "action") == 0 && pj_peek(&r) == PJ_STRING) {
            (void)pj_get_str(&r, action, sizeof(action));
        } else if (strcmp(key, "number") == 0) {
            bad |= !get_u8(&r, &out->number);
        } else if (strcmp(key, "name") == 0 && pj_peek(&r) == PJ_STRING) {
            bad |= !pj_get_str(&r, out->name, sizeof(out->name));
        } else if (strcmp(key, "role") == 0 && pj_peek(&r) == PJ_STRING) {
            (void)pj_get_str(&r, role, sizeof(role));
        } else if (strcmp(key, "sealed_code") == 0 && pj_peek(&r) == PJ_STRING) {
            bad |= !pj_get_str(&r, out->sealed_code, sizeof(out->sealed_code));
        } else if (strcmp(key, "partitions") == 0 && pj_peek(&r) == PJ_ARRAY && pj_enter_arr(&r)) {
            have_parts = true;
            while (pj_next_element(&r)) {
                if (pj_peek(&r) != PJ_NUMBER) {
                    bad = true;
                    pj_skip(&r);
                } else if (!pj_get_int(&r, &v) || v < 1 || v > ALARM_MAX_PARTITIONS) {
                    bad = true;
                } else {
                    out->partitions = (uint8_t)(out->partitions | (1u << (v - 1)));
                }
            }
        }
    }
    if (r.error || !pj_finished(&r)) {
        bad = true;
    }
    if (strcmp(action, "create") == 0) {
        out->kind = PROTO_SRV_PEOPLE_CREATE;
        out->authority = proto_srv_role_authority(role);
        bad |= out->name[0] == '\0' || !have_parts || out->partitions == 0u || out->sealed_code[0] == '\0';
    } else if (strcmp(action, "set_code") == 0) {
        out->kind = PROTO_SRV_PEOPLE_SET_CODE;
        bad |= out->number < 1u || out->number > ALARM_MAX_USERS || out->sealed_code[0] == '\0';
    } else {
        out->kind = PROTO_SRV_PEOPLE_OTHER;
        return;
    }
    if (bad) {
        memset(out->sealed_code, 0, sizeof(out->sealed_code));
        out->kind = PROTO_SRV_PEOPLE_BAD;
    }
}

const proto_srv_seen_entry_t *proto_srv_seen_find(const proto_srv_seen_t *s, const char *id,
                                                  alarm_ms_t now)
{
    for (size_t i = 0; i < PROTO_SRV_SEEN_N; i++) {
        const proto_srv_seen_entry_t *e = &s->e[i];
        if (e->id[0] != '\0' && strcmp(e->id, id) == 0 && now - e->at < PROTO_SRV_SEEN_MS) {
            return e;
        }
    }
    return NULL;
}

void proto_srv_seen_add(proto_srv_seen_t *s, const char *id, const char *res, size_t len,
                        alarm_ms_t now)
{
    proto_srv_seen_entry_t *e = &s->e[s->next];
    s->next = (uint8_t)((s->next + 1u) % PROTO_SRV_SEEN_N);
    snprintf(e->id, sizeof(e->id), "%s", id);
    e->at = now;
    if (len >= sizeof(e->res)) {
        len = 0; /* too long to keep: a repeat says it ran, without the details */
    }
    memcpy(e->res, res, len);
    e->res[len] = '\0';
    e->len = (uint16_t)len;
}

/* ---- the clock -------------------------------------------------------------------------------- */

#define CLOCK_SET_MS 2000
#define CLOCK_TRUST_RTT_MS 5000u /* half of this is the most the estimate can be off */

void proto_srv_clock_check(int64_t server_ms, uint64_t echo_uptime_ms, uint64_t uptime_ms,
                           int64_t now_ms, proto_srv_clock_t *out)
{
    memset(out, 0, sizeof(*out));
    out->round_trip_ms = uptime_ms > echo_uptime_ms ? uptime_ms - echo_uptime_ms : 0u;
    out->server_now = server_ms + (int64_t)(out->round_trip_ms / 2u);
    if (now_ms <= 0) {
        out->set_clock = true;
        out->trusted = out->round_trip_ms <= CLOCK_TRUST_RTT_MS;
        return;
    }
    /* A time from long after the hello it echoes (the server answers again once linked) says nothing
     * about the offset: a panel with a clock keeps it. */
    if (out->round_trip_ms > CLOCK_TRUST_RTT_MS) {
        out->trusted = false;
        return;
    }
    out->trusted = true;
    out->offset_ms = out->server_now - now_ms;
    out->set_clock = out->offset_ms > CLOCK_SET_MS || out->offset_ms < -CLOCK_SET_MS;
}

/* ---- the replay queue ------------------------------------------------------------------------- */

void proto_srvq_init(proto_srvq_t *q, const proto_srvq_port_t *port, uint32_t acked,
                     uint32_t acked_seq, bool enabled, alarm_ms_t now)
{
    memset(q, 0, sizeof(*q));
    q->port = *port;
    q->acked = acked;
    q->acked_seq = acked_seq;
    q->sent = acked;
    q->enabled = enabled;
    q->down_since = now;
}

void proto_srvq_enable(proto_srvq_t *q, bool enabled, alarm_ms_t now)
{
    if (enabled && !q->enabled) {
        q->down_since = now;
        q->waiting_since = 0;
    }
    q->enabled = enabled;
}

void proto_srvq_start_from_now(proto_srvq_t *q, uint32_t newest_seq)
{
    q->acked = q->port.newest_index(q->port.ctx);
    q->acked_seq = newest_seq;
    q->sent = q->acked;
    q->skip_upto = 0;
    q->window_n = 0;
    q->waiting_since = 0;
    q->acked_dirty = true;
}

void proto_srvq_connected(proto_srvq_t *q, alarm_ms_t now)
{
    (void)now;
    q->connected = true;
    q->running = false;
    q->window_n = 0;
    q->sent = q->acked;
    q->live_from = q->port.newest_index(q->port.ctx) + 1u;
}

void proto_srvq_disconnected(proto_srvq_t *q, alarm_ms_t now)
{
    if (q->connected) {
        q->down_since = now;
    }
    q->connected = false;
    q->running = false;
    q->window_n = 0;
}

void proto_srvq_time(proto_srvq_t *q, uint32_t ack_seq, alarm_ms_t now)
{
    (void)now;
    q->window_n = 0;
    q->skip_upto = 0;
    q->running = q->connected;
    if (ack_seq != 0u && ack_seq != q->acked_seq) {
        if (ack_seq < q->acked_seq) {
            q->acked = 0; /* the server lost events: from the oldest record kept */
        }
        q->acked_seq = ack_seq;
        q->skip_upto = ack_seq;
        q->acked_dirty = true;
    }
    q->sent = q->acked;
}

void proto_srvq_ack(proto_srvq_t *q, uint32_t seq, alarm_ms_t now)
{
    uint8_t gone = 0;
    while (gone < q->window_n && q->window[gone].seq <= seq) {
        q->acked = q->window[gone].index;
        q->acked_seq = q->window[gone].seq;
        q->acked_dirty = true;
        gone++;
    }
    if (gone == 0u) {
        return; /* an old ack, or nothing of ours */
    }
    q->window_n = (uint8_t)(q->window_n - gone);
    for (uint8_t i = 0; i < q->window_n; i++) {
        q->window[i] = q->window[i + gone];
    }
    q->waiting_since = q->window_n != 0u ? now : 0u;
}

bool proto_srvq_poll(proto_srvq_t *q, alarm_ms_t now)
{
    if (!q->running || q->window_n >= PROTO_SRVQ_WINDOW) {
        return false;
    }
    const uint32_t newest = q->port.newest_index(q->port.ctx);
    if (q->sent > newest) {
        q->sent = newest; /* the log was cleared: nothing older can be sent */
        q->acked = newest;
        q->acked_dirty = true;
    }
    elog_entry_t e;
    uint32_t next = q->sent;
    if (q->port.read_after(q->port.ctx, q->sent, &e, 1u, PROTO_SRVQ_SCAN, &next) == 0u) {
        if (next > q->sent) {
            q->sent = next; /* only other records in between */
            if (q->window_n == 0u) {
                q->acked = next;
                q->acked_dirty = true;
            }
            return true;
        }
        return false;
    }
    if (e.ev.seq <= q->skip_upto) {
        q->sent = e.index; /* the server has it already */
        if (q->window_n == 0u) {
            q->acked = e.index;
            q->acked_dirty = true;
        }
        return true;
    }
    if (!q->port.publish(q->port.ctx, &e, e.index < q->live_from)) {
        return false;
    }
    q->window[q->window_n].index = e.index;
    q->window[q->window_n].seq = e.ev.seq;
    q->window_n++;
    q->sent = e.index;
    if (q->waiting_since == 0u) {
        q->waiting_since = now;
    }
    return true;
}

bool proto_srvq_comm_fail(const proto_srvq_t *q, alarm_ms_t now)
{
    if (!q->enabled) {
        return false;
    }
    /* The owner's now can be from just before a change it made in the same pass. */
    if (!q->connected) {
        return now >= q->down_since && now - q->down_since >= PROTO_COMM_FAIL_MS;
    }
    return q->waiting_since != 0u && now >= q->waiting_since &&
           now - q->waiting_since >= PROTO_COMM_FAIL_MS;
}
