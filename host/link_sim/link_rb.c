/*
 * link_rb.c - the remote backend on the PC's virtual master and virtual keypad (BL-223;
 * docs/features/11 "The master's settings and installer menu on keypads", LINK-057..LINK-065).
 *
 * The virtual master serves a keypad's calls with ui_remote_serve over a small backend of its own:
 * the real engine for installer mode, codes and the configuration (as {ALARM_SRC_KEYPAD, n}), and
 * a pretend sensor source in place of zone_hub - eight simulator inputs, "Input 1" to "Input 8",
 * of which link_mrb_heard marks one as the sensor just opened while the keypad listens, and a
 * binding kept for each of zones 1..LINK_RB_ZONES. Each call runs at once, inside port.rb_run; the
 * grants are kept, ticked and sent to the keypads (RB_GRANT) by link_mrb_tick.
 *
 * The virtual keypad attaches its buffers and offers PLINK_CAP_RB; link_smoke drives a
 * ui_remote proxy over it (link_krb_port), pumping the network while a call waits, as a panel's
 * call task waits for its link task.
 */
#include <stdio.h>
#include <string.h>

#include "link_node.h"

/* The backend's context: the master, and the keypad the call is for. */
typedef struct {
    link_master_t *v;
    uint8_t device;
} rb_ctx_t;

static alarm_source_t src_of(const rb_ctx_t *c)
{
    const alarm_source_t s = {ALARM_SRC_KEYPAD, c->device};
    return s;
}

static void be_snapshot(void *ctx, alarm_snapshot_t *out)
{
    const rb_ctx_t *c = ctx;
    alarm_engine_snapshot(&c->v->eng, out);
}

static bool be_get_config(void *ctx, alarm_config_t *out)
{
    const rb_ctx_t *c = ctx;
    *out = *alarm_engine_config(&c->v->eng);
    return true;
}

static alarm_result_t be_program_enter(void *ctx, const char *code, uint8_t *user)
{
    rb_ctx_t *c = ctx;
    const alarm_source_t s = src_of(c);
    return alarm_engine_program_enter(&c->v->eng, &s, code, c->v->now, user);
}

static void be_program_exit(void *ctx)
{
    rb_ctx_t *c = ctx;
    const alarm_source_t s = src_of(c);
    alarm_engine_program_exit(&c->v->eng, &s, c->v->now);
}

static void be_keepalive(void *ctx)
{
    rb_ctx_t *c = ctx;
    alarm_engine_program_keepalive(&c->v->eng, c->v->now);
}

static alarm_result_t be_set_code(void *ctx, uint8_t user, const char *code)
{
    rb_ctx_t *c = ctx;
    const alarm_source_t s = src_of(c);
    return alarm_engine_program_set_code(&c->v->eng, &s, user, code, c->v->now);
}

static alarm_result_t be_set_config(void *ctx, const alarm_config_t *cfg)
{
    rb_ctx_t *c = ctx;
    return alarm_engine_set_config(&c->v->eng, cfg, c->v->now);
}

static alarm_result_t be_user_set(void *ctx, const char *actor, uint8_t user, const char *code,
                                  alarm_authority_t a, uint8_t parts, const char *name)
{
    rb_ctx_t *c = ctx;
    const alarm_source_t s = src_of(c);
    return alarm_engine_user_set(&c->v->eng, &s, actor, user, code, a, parts, name, c->v->now);
}

static alarm_result_t be_user_update(void *ctx, const char *actor, uint8_t user,
                                     alarm_authority_t a, uint8_t parts, const char *name)
{
    rb_ctx_t *c = ctx;
    const alarm_source_t s = src_of(c);
    return alarm_engine_user_update(&c->v->eng, &s, actor, user, a, parts, name, c->v->now);
}

static alarm_result_t be_user_delete(void *ctx, const char *actor, uint8_t user)
{
    rb_ctx_t *c = ctx;
    const alarm_source_t s = src_of(c);
    return alarm_engine_user_delete(&c->v->eng, &s, actor, user, c->v->now);
}

/* ---- the pretend sensor source ---- */

static size_t be_sources(void *ctx, ui_zone_source_t *out, size_t max)
{
    (void)ctx;
    if (max == 0u) {
        return 0;
    }
    out[0] = (ui_zone_source_t){.kind = ZSRC_SIM, .caps = ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_TEST,
                                .usable = true};
    return 1;
}

static void be_learn(void *ctx, uint8_t kind, bool on)
{
    rb_ctx_t *c = ctx;
    link_mrb_t *r = &c->v->rb;
    r->learning = on && kind == ZSRC_SIM;
    if (!on) {
        r->heard_input = 0;
    }
    link_io_say(&c->v->io, on ? "remote: listening for a sensor" : "remote: no longer listening");
}

static uint16_t zone_of_input(const link_mrb_t *r, uint64_t id)
{
    for (unsigned z = 0; z < LINK_RB_ZONES; z++) {
        if (r->bound[z] && r->bind[z].in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_SIM &&
            r->bind[z].in[ZSRC_ROLE_MAIN].addr.id == id) {
            return (uint16_t)(z + 1u);
        }
    }
    return 0;
}

static size_t be_candidates(void *ctx, uint8_t kind, const char *query, zsrc_candidate_t *out,
                            size_t max)
{
    rb_ctx_t *c = ctx;
    const link_mrb_t *r = &c->v->rb;
    (void)query;
    size_t n = 0;
    for (unsigned i = 1; kind == ZSRC_SIM && i <= 8u && n < max; i++, n++) {
        zsrc_candidate_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        d->addr.kind = ZSRC_SIM;
        d->addr.id = i;
        snprintf(d->name, sizeof(d->name), "Input %u", i);
        snprintf(d->state, sizeof(d->state), "%s", r->heard_input == i ? "open" : "closed");
        d->changed = r->learning && r->heard_input == i;
        d->best = d->changed;
        d->zone = zone_of_input(r, i);
    }
    return n;
}

static zone_hub_result_t be_bind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    rb_ctx_t *c = ctx;
    link_mrb_t *r = &c->v->rb;
    if (zone == 0u || zone > LINK_RB_ZONES || b->in[ZSRC_ROLE_MAIN].addr.kind != ZSRC_SIM) {
        return ZONE_HUB_ERR_ARG;
    }
    const uint16_t other = zone_of_input(r, b->in[ZSRC_ROLE_MAIN].addr.id);
    if (other != 0u && other != zone) {
        return ZONE_HUB_ERR_IN_USE;
    }
    r->bind[zone - 1u] = *b;
    r->bound[zone - 1u] = true;
    char line[64];
    snprintf(line, sizeof(line), "remote: zone %u bound to input %u", (unsigned)zone,
             (unsigned)b->in[ZSRC_ROLE_MAIN].addr.id);
    link_io_say(&c->v->io, line);
    return ZONE_HUB_OK;
}

static zone_hub_result_t be_unbind(void *ctx, uint16_t zone)
{
    rb_ctx_t *c = ctx;
    if (zone == 0u || zone > LINK_RB_ZONES) {
        return ZONE_HUB_ERR_ARG;
    }
    c->v->rb.bound[zone - 1u] = false;
    return ZONE_HUB_OK;
}

static bool be_info(void *ctx, uint16_t zone, zone_hub_info_t *out)
{
    rb_ctx_t *c = ctx;
    const link_mrb_t *r = &c->v->rb;
    if (zone == 0u || zone > LINK_RB_ZONES || !r->bound[zone - 1u]) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->binding = r->bind[zone - 1u];
    out->source_present = true;
    out->caps = ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_TEST;
    out->known = true;
    snprintf(out->address, sizeof(out->address), "Input %u",
             (unsigned)out->binding.in[ZSRC_ROLE_MAIN].addr.id);
    return true;
}

static void backend(rb_ctx_t *c, ui_backend_t *be)
{
    memset(be, 0, sizeof(*be));
    be->ctx = c;
    be->get_snapshot = be_snapshot;
    be->get_config = be_get_config;
    be->program_enter = be_program_enter;
    be->program_exit = be_program_exit;
    be->program_keepalive = be_keepalive;
    be->program_set_code = be_set_code;
    be->set_config = be_set_config;
    be->user_set = be_user_set;
    be->user_update = be_user_update;
    be->user_delete = be_user_delete;
    be->zone_sources = be_sources;
    be->zone_learn = be_learn;
    be->zone_candidates = be_candidates;
    be->zone_bind = be_bind;
    be->zone_unbind = be_unbind;
    be->zone_info = be_info;
}

/* ---- the master's side ---- */

void link_mrb_attach(link_master_t *v)
{
    plink_master_rb_attach(&v->m, v->rb.up, sizeof(v->rb.up), &v->rb.ans[0][0], UR_ANSWER_MAX);
}

bool link_mrb_run(void *ctx, uint32_t ticket, uint8_t device, uint16_t call, const uint8_t *args,
                  size_t len)
{
    link_master_t *v = ctx;
    link_mrb_t *r = &v->rb;
    if (device == 0u || device > PLINK_KEYPADS_MAX) {
        return false;
    }
    rb_ctx_t c = {v, device};
    ui_backend_t be;
    backend(&c, &be);
    /* the arguments copied, as a panel's runner does, and wiped after use */
    memcpy(r->args, args, len);
    size_t n = 0;
    const ui_remote_status_t st =
        ui_remote_serve(&r->srv, &be, &r->grant[device - 1u], v->m.cfg_crc, true, call, r->args,
                        len, r->out, sizeof(r->out), &n, v->now);
    plink_wipe(r->args, len);
    r->runs++;
    char line[80];
    snprintf(line, sizeof(line), "remote: keypad %u call 0x%04x: status %u", (unsigned)device,
             (unsigned)call, (unsigned)st);
    link_io_say(&v->io, line);
    v->dirty = true; /* installer mode, a configuration: the keypads see it */
    plink_master_rb_answer(&v->m, v->now, ticket, (uint8_t)st, r->out, n);
    return true;
}

void link_mrb_auth(link_master_t *v, uint8_t device, uint8_t user, uint64_t now)
{
    if (device == 0u || device > PLINK_KEYPADS_MAX || user == 0u || user > ALARM_MAX_USERS) {
        return;
    }
    const uint8_t a = (uint8_t)alarm_engine_config(&v->eng)->users[user - 1u].authority;
    ui_remote_grant_user(&v->rb.grant[device - 1u], user, a, now);
}

void link_mrb_heard(link_master_t *v, uint16_t input)
{
    v->rb.heard_input = input;
}

void link_mrb_tick(link_master_t *v, uint64_t now)
{
    link_mrb_t *r = &v->rb;
    plink_peer_info_t peers[PLINK_KEYPADS_MAX];
    const size_t n = plink_master_peers(&v->m, now, peers, PLINK_KEYPADS_MAX);
    for (size_t i = 0; i < n && i < PLINK_KEYPADS_MAX; i++) {
        const uint8_t d = peers[i].device;
        if (d == 0u || d > PLINK_KEYPADS_MAX) {
            continue;
        }
        rb_ctx_t c = {v, d};
        ui_backend_t be;
        backend(&c, &be);
        ui_remote_grant_t *g = &r->grant[d - 1u];
        const bool gone = peers[i].heard_ago_s != UINT32_MAX && peers[i].heard_ago_s >= 60u;
        ui_remote_grant_tick(&be, g, gone, now);
        /* what it holds, sent when it changes (and to each new session by the link) */
        plink_rb_grant_t want;
        memset(&want, 0, sizeof(want));
        want.user = g->user;
        want.authority = g->authority;
        want.installer = g->installer;
        if (g->user != 0u) {
            want.partitions = alarm_engine_config(&v->eng)->users[g->user - 1u].partitions;
            const uint64_t idle = now - g->last_ms;
            want.idle_left_s = (uint16_t)(idle < UR_GRANT_IDLE_MS ? (UR_GRANT_IDLE_MS - idle) / 1000u : 0u);
        }
        if (v->eng.program) {
            want.holder_kind = (uint8_t)v->eng.program_src.kind;
            want.holder_id = v->eng.program_src.id;
        }
        want.areas = ui_remote_areas(&be);
        want.idle_left_s = (uint16_t)(want.idle_left_s / 60u * 60u); /* whole minutes: no chatter */
        if (memcmp(&want, &r->sent[d - 1u], sizeof(want)) != 0) {
            r->sent[d - 1u] = want;
            plink_master_rb_grant(&v->m, d, &want);
        }
    }
}

/* ---- the keypad's side ---- */

void link_krb_attach(link_keypad_t *v)
{
    plink_keypad_rb_attach(&v->k, v->rb_args, sizeof(v->rb_args), v->rb_ans, sizeof(v->rb_ans));
}
