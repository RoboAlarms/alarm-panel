/*
 * link_node.c - see link_node.h.
 */
#include "link_node.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "panel_link/plink_store.h"
#include "panel_link/plink_view.h"

#define VIRTUAL_EPOCH_MS 1790000000000LL /* link_smoke's wall clock at ms 0 */
#define LINK_FW          "link_sim 1"
#define LINK_TZ          "UTC0"

_Static_assert(ALARM_CODE_HASH_LEN == PLINK_SHA256_LEN, "the code hash is one SHA-256");

/* ---- small helpers ------------------------------------------------------------------------- */

static void say(const link_io_t *io, const char *fmt, ...) LINK_PRINTF(2, 3);
static void say(const link_io_t *io, const char *fmt, ...)
{
    if (io->say == NULL) {
        return;
    }
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    io->say(io->say_ctx, line);
}

void link_code_text(const char *code, char out[8])
{
    if (strlen(code) == 6u) {
        snprintf(out, 8, "%.3s %.3s", code, code + 3);
    } else {
        snprintf(out, 8, "%.7s", code);
    }
}

const char *link_fail_name(uint8_t reason)
{
    static const char *const t[] = {
        "none",
        "refused (a person said no)",
        "a step took too long",
        "a commitment or MAC did not verify",
        "another attempt is under way",
        "pairing is not open at the master",
        "no keypad number is free",
        "more than one keypad or master at once",
        "3 attempts used: the window closed",
        "the keypad can't hold the master's zones",
        "cancelled",
        "a value changed mid-attempt",
        "the record could not be stored",
        "no common protocol version",
    };
    return reason < sizeof(t) / sizeof(t[0]) ? t[reason] : "unknown";
}

const char *link_state_text(uint8_t state)
{
    switch (state) {
    case PLINK_LINK_NOT_PAIRED: return "not paired";
    case PLINK_LINK_CONNECTING: return "connecting";
    case PLINK_LINK_ONLINE: return "online";
    case PLINK_LINK_LOST: return "no system communication";
    default: return "unknown";
    }
}

static const char *cmd_name(uint8_t type)
{
    static const char *const t[] = {"arm",   "disarm",       "silence", "bypass",
                                    "unbypass", "quick bypass", "chime",   "exit restart",
                                    "panic", "walk test",    "code check"};
    return type < sizeof(t) / sizeof(t[0]) ? t[type] : "command";
}

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case PLINK_KIND_PANEL: return "panel";
    case PLINK_KIND_SMALL: return "small screen";
    case PLINK_KIND_PC: return "PC";
    default: return "unknown";
    }
}

/* One event as a line: "#12 armed, partition 1, user 2, keypad 1". */
static void event_text(const alarm_event_t *ev, char *out, size_t cap)
{
    int n = snprintf(out, cap, "#%u %s", (unsigned)ev->seq, alarm_event_name(ev->type));
    if (n < 0 || (size_t)n >= cap) {
        return;
    }
    size_t at = (size_t)n;
    if (ev->partition != 0u) {
        n = snprintf(out + at, cap - at, ", partition %u", (unsigned)ev->partition);
        at += n > 0 ? (size_t)n : 0u;
    }
    if (ev->zone != 0u && at < cap) {
        const bool device =
            (ev->type == ALARM_EV_TROUBLE || ev->type == ALARM_EV_TROUBLE_RESTORE) &&
            ev->arg2 == ALARM_TRBL_FAMILY_DEVICE;
        n = snprintf(out + at, cap - at, ", %s %u", device ? "keypad" : "zone",
                     (unsigned)ev->zone);
        at += n > 0 ? (size_t)n : 0u;
    }
    if (ev->user != 0u && at < cap) {
        n = snprintf(out + at, cap - at, ", user %u", (unsigned)ev->user);
        at += n > 0 ? (size_t)n : 0u;
    }
    if (ev->source.kind != ALARM_SRC_SYSTEM && at < cap) {
        snprintf(out + at, cap - at, ", from %s %u",
                 alarm_source_name((alarm_source_kind_t)ev->source.kind),
                 (unsigned)ev->source.id);
    }
}

/* ---- records ------------------------------------------------------------------------------- */

bool link_store_init(link_store_t *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->path = path;
    if (path == NULL) {
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return true; /* nothing stored yet */
    }
    const size_t n = fread(s->buf, 1, sizeof(s->buf), f);
    const bool more = fgetc(f) != EOF;
    const bool err = ferror(f) != 0;
    fclose(f);
    if (err || more) {
        plink_wipe(s->buf, sizeof(s->buf));
        return false;
    }
    s->len = n;
    return true;
}

bool link_store_write(link_store_t *s, const uint8_t *buf, size_t len)
{
    if (len == 0u || len > sizeof(s->buf)) {
        s->failures++;
        return false;
    }
    if (s->path != NULL) {
        char tmp[512];
        if (strlen(s->path) + 5u > sizeof(tmp)) {
            s->failures++;
            return false;
        }
        snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);
        FILE *f = fopen(tmp, "wb");
        if (f == NULL) {
            s->failures++;
            return false;
        }
        const bool wrote = fwrite(buf, 1, len, f) == len;
        if (fclose(f) != 0 || !wrote) {
            remove(tmp);
            s->failures++;
            return false;
        }
        remove(s->path); /* Windows' rename doesn't replace */
        if (rename(tmp, s->path) != 0) {
            remove(tmp);
            s->failures++;
            return false;
        }
    }
    plink_wipe(s->buf, sizeof(s->buf));
    memcpy(s->buf, buf, len);
    s->len = len;
    s->writes++;
    return true;
}

bool link_store_erase(link_store_t *s)
{
    if (s->path != NULL) {
        FILE *f = fopen(s->path, "rb");
        if (f != NULL) {
            fclose(f);
            if (remove(s->path) != 0) {
                s->failures++;
                return false;
            }
        }
    }
    plink_wipe(s->buf, sizeof(s->buf));
    s->len = 0u;
    s->erases++;
    return true;
}

/* ---- the virtual master: the engine -------------------------------------------------------- */

static int64_t m_epoch(const link_master_t *v)
{
    return v->io.epoch_ms != NULL ? v->io.epoch_ms() : VIRTUAL_EPOCH_MS + (int64_t)v->now;
}

/* The code hash: SHA-256 over a label, the salt and the code (a panel uses PBKDF2; the codes of a
 * simulated house only need to differ). */
static void m_hash(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                   uint8_t out[ALARM_CODE_HASH_LEN])
{
    (void)ctx;
    static const char label[] = "link_sim code";
    const plink_iov_t parts[3] = {
        {label, sizeof(label) - 1u}, {salt, ALARM_CODE_SALT_LEN}, {code, strlen(code)}};
    plink_sha256(parts, 3u, out);
}

static void house(alarm_config_t *cfg, const uint8_t salt[ALARM_CODE_SALT_LEN])
{
    alarm_config_defaults(cfg);
    cfg->opt.power_up_settle_s = 0u;
    memcpy(cfg->code_salt, salt, ALARM_CODE_SALT_LEN);
    snprintf(cfg->partitions[0].name, sizeof(cfg->partitions[0].name), "%s", "Home");
    static const struct {
        alarm_zone_type_t type;
        const char *name;
    } zones[] = {
        {ALARM_ZT_ENTRY_EXIT_1, "Front Door"},
        {ALARM_ZT_PERIMETER, "Living Window"},
        {ALARM_ZT_INTERIOR_FOLLOWER, "Hall Motion"},
        {ALARM_ZT_FIRE, "Hall Smoke"},
    };
    for (uint16_t i = 0; i < (uint16_t)(sizeof(zones) / sizeof(zones[0])); i++) {
        alarm_zone_config_defaults(&cfg->zones[i], (uint16_t)(i + 1u), zones[i].type, 1u);
        snprintf(cfg->zones[i].name, sizeof(cfg->zones[i].name), "%s", zones[i].name);
    }
    alarm_config_set_default_users(cfg, m_hash, NULL); /* 01 installer, 02 master */
    uint8_t h[ALARM_CODE_HASH_LEN];
    m_hash(NULL, cfg->code_salt, "1111", h);
    alarm_config_set_user(cfg, 3u, ALARM_AUTH_STANDARD, 0x01u, "Alex", h);
}

static void e_event(void *ctx, const alarm_event_t *ev)
{
    link_master_t *v = ctx;
    if (v->ring_count < LINK_MASTER_EVENTS) {
        v->ring[(v->ring_head + v->ring_count) % LINK_MASTER_EVENTS] = *ev;
        v->ring_count++;
    } else {
        v->ring[v->ring_head] = *ev;
        v->ring_head = (v->ring_head + 1u) % LINK_MASTER_EVENTS;
    }
    if (v->fresh_count < LINK_MASTER_FRESH) {
        v->fresh[v->fresh_count++] = *ev;
    }
    v->dirty = true;
}

static void e_status(void *ctx, uint8_t partition)
{
    (void)partition;
    link_master_t *v = ctx;
    v->dirty = true;
}

static void e_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone)
{
    link_master_t *v = ctx;
    if (v->sound_count < sizeof(v->sounds) / sizeof(v->sounds[0])) {
        plink_sound_t *s = &v->sounds[v->sound_count++];
        s->partition = partition;
        s->sound = (uint8_t)sound;
        s->zone = zone;
    }
}

static void e_config_changed(void *ctx)
{
    link_master_t *v = ctx;
    v->dirty = true;
}

static int64_t e_epoch(void *ctx)
{
    return m_epoch(ctx);
}

/* ---- the virtual master: the link's port --------------------------------------------------- */

static bool m_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    link_master_t *v = ctx;
    return v->io.send(v->io.send_ctx, to, frame, len);
}

static bool m_store(void *ctx, const plink_master_rec_t *rec)
{
    link_master_t *v = ctx;
    uint8_t buf[PLINK_STORE_MAX];
    size_t len = 0;
    const bool ok = plink_store_master_encode(rec, buf, sizeof(buf), &len) &&
                    link_store_write(v->store, buf, len);
    plink_wipe(buf, sizeof(buf));
    if (!ok) {
        say(&v->io, "the master's record could not be stored");
    }
    return ok;
}

static bool m_run(void *ctx, uint32_t ticket, uint8_t device, const plink_cmd_t *cmd)
{
    link_master_t *v = ctx;
    for (size_t i = 0; i < LINK_MASTER_RUNS; i++) {
        if (!v->runs[i].used) {
            v->runs[i].used = true;
            v->runs[i].ticket = ticket;
            v->runs[i].device = device;
            v->runs[i].cmd = *cmd;
            return true;
        }
    }
    return false;
}

static void m_reachable(void *ctx, uint8_t device, bool reachable)
{
    link_master_t *v = ctx;
    v->reachable_calls++;
    if (device >= 1u && device <= 32u) {
        const uint32_t bit = 1u << (device - 1u);
        v->down = reachable ? (v->down & ~bit) : (v->down | bit);
    }
    alarm_engine_device_reachable(&v->eng, device, reachable, v->now);
    say(&v->io, reachable ? "keypad %u: its supervision trouble is clear"
                          : "keypad %u: not responding, supervision trouble raised",
        (unsigned)device);
}

static uint32_t m_taken(void *ctx)
{
    link_master_t *v = ctx;
    const alarm_config_t *cfg = alarm_engine_config(&v->eng);
    uint32_t taken = 0;
    for (unsigned d = 0; d < PLINK_KEYPADS_MAX && d < ALARM_MAX_DEVICES; d++) {
        if (cfg->devices[d].enabled) {
            taken |= 1u << d;
        }
    }
    return taken;
}

static void m_held(void *ctx, uint32_t held)
{
    link_master_t *v = ctx;
    say(&v->io, "the link holds keypad numbers 0x%02x", (unsigned)held);
}

static uint32_t m_lockout(void *ctx, uint8_t device, uint64_t now_ms)
{
    link_master_t *v = ctx;
    const alarm_source_t src = {ALARM_SRC_KEYPAD, device};
    const alarm_ms_t left = alarm_engine_lockout_left(&v->eng, &src, now_ms);
    return left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
}

static const alarm_event_t *ring_at(const link_master_t *v, size_t i)
{
    return &v->ring[(v->ring_head + i) % LINK_MASTER_EVENTS];
}

static size_t m_events(void *ctx, uint32_t after_seq, alarm_event_t *out, size_t max, bool *gap)
{
    link_master_t *v = ctx;
    size_t first = v->ring_count;
    for (size_t i = 0; i < v->ring_count; i++) {
        if (ring_at(v, i)->seq > after_seq) {
            first = i;
            break;
        }
    }
    *gap = false;
    if (first == 0u && v->ring_count > 0u && ring_at(v, 0)->seq > after_seq + 1u) {
        *gap = true; /* the ring has already dropped some */
    }
    size_t n = v->ring_count - first;
    if (n > max) {
        first += n - max;
        n = max;
        *gap = true;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = *ring_at(v, first + i);
    }
    return n;
}

static void m_note(void *ctx, plink_note_t note, uint8_t device)
{
    link_master_t *v = ctx;
    switch (note) {
    case PLINK_NOTE_PAIRED: say(&v->io, "keypad %u paired", (unsigned)device); break;
    case PLINK_NOTE_PAIR_LIMIT: say(&v->io, "pairing: 3 attempts used, the window closed"); break;
    case PLINK_NOTE_PAIR_OVERLAP: say(&v->io, "pairing: more than one keypad at once"); break;
    case PLINK_NOTE_REMOVED: say(&v->io, "keypad %u removed", (unsigned)device); break;
    case PLINK_NOTE_CLONE:
        say(&v->io, "a second device is using keypad %u's keys", (unsigned)device);
        break;
    case PLINK_NOTE_PROVISIONAL_DROPPED:
        say(&v->io, "keypad %u never connected after pairing: dropped", (unsigned)device);
        break;
    case PLINK_NOTE_STORE_FAILED: say(&v->io, "the link's record could not be written"); break;
    default: break;
    }
}

/* ---- the virtual master -------------------------------------------------------------------- */

static void push_config(link_master_t *v, uint32_t gen)
{
    v->cfg_gen = gen;
    v->cfg_known = true;
    /* Two buffers in turn: the link keeps reading the last one until it is handed the next. */
    const unsigned next = v->blob_cur ^ 1u;
    size_t len = 0;
    uint32_t crc = 0;
    if (plink_cfg_for_keypads(alarm_engine_config(&v->eng), &v->scratch, v->blob[next],
                              sizeof(v->blob[next]), &len, &crc) != ALARM_OK) {
        say(&v->io, "the keypads' configuration copy could not be made");
        return;
    }
    plink_wipe(&v->scratch, sizeof(v->scratch));
    v->blob_cur = next;
    plink_master_config(&v->m, v->blob[next], len, crc);
}

/* A keypad's command, as {ALARM_SRC_KEYPAD, n}: a panel runs it on the alarm task; here it runs
 * between two calls of the link manager. The code is wiped after use. */
static void run_one(link_master_t *v, uint64_t now, link_run_t *r)
{
    const alarm_source_t src = {ALARM_SRC_KEYPAD, r->device};
    alarm_cmd_result_t cr;
    memset(&cr, 0, sizeof(cr));
    alarm_result_t rc = ALARM_ERR_BAD_ARG;
    char what[48];
    switch (r->cmd.op) {
    case PLINK_OP_COMMAND:
        rc = alarm_engine_command(&v->eng, &r->cmd.cmd, &src, now, &cr);
        if (r->cmd.cmd.type == ALARM_CMD_ARM) {
            snprintf(what, sizeof(what), "arm %s",
                     alarm_level_name((alarm_arm_level_t)r->cmd.cmd.level));
        } else {
            snprintf(what, sizeof(what), "%s", cmd_name((uint8_t)r->cmd.cmd.type));
        }
        break;
    case PLINK_OP_AUTH: {
        uint8_t user = 0;
        rc = alarm_engine_authenticate(&v->eng, &src, r->cmd.cmd.code, r->cmd.cmd.partition, now,
                                       &user);
        cr.user = user;
        snprintf(what, sizeof(what), "code check");
        break;
    }
    case PLINK_OP_CHANGE_CODE: {
        /* As a panel does: the code's own user, its authority, partitions and name kept. */
        uint8_t user = 0;
        rc = alarm_engine_authenticate(&v->eng, &src, r->cmd.cmd.code, 0u, now, &user);
        if (rc == ALARM_OK && user >= 1u && user <= ALARM_MAX_USERS) {
            const alarm_user_t *u = &alarm_engine_config(&v->eng)->users[user - 1u];
            char name[ALARM_NAME_LEN];
            snprintf(name, sizeof(name), "%s", u->name);
            rc = alarm_engine_user_set(&v->eng, &src, r->cmd.cmd.code, user, r->cmd.new_code,
                                       u->authority, u->partitions, name, now);
        }
        cr.user = user;
        snprintf(what, sizeof(what), "change code");
        break;
    }
    default: snprintf(what, sizeof(what), "op %u", (unsigned)r->cmd.op); break;
    }
    say(&v->io, "keypad %u: %s, partition %u: %s", (unsigned)r->device, what,
        (unsigned)r->cmd.cmd.partition, alarm_result_name(rc));
    if (v->answer_count < LINK_MASTER_RUNS) {
        link_answer_t *a = &v->answers[v->answer_count++];
        memset(a, 0, sizeof(*a));
        a->ticket = r->ticket;
        a->res.req = r->cmd.req;
        a->res.result = (uint8_t)rc;
        a->res.user = cr.user;
        a->res.not_ready_zone = cr.not_ready_zone;
        a->res.may_disarm = cr.may_disarm ? 1u : 0u;
    }
    plink_wipe(&r->cmd, sizeof(r->cmd));
    r->used = false;
}

static void say_pairing(link_master_t *v, uint64_t now)
{
    plink_pair_info_t pi;
    plink_master_pair_info(&v->m, now, &pi);
    if (pi.state == v->said_pair && strcmp(pi.code, v->said_code) == 0) {
        return;
    }
    v->said_pair = pi.state;
    snprintf(v->said_code, sizeof(v->said_code), "%s", pi.code);
    char addr[24];
    plink_addr_text(&pi.peer, addr);
    char code[8];
    link_code_text(pi.code, code);
    switch (pi.state) {
    case PLINK_PS_IDLE: say(&v->io, "pairing closed"); break;
    case PLINK_PS_OPEN:
        say(&v->io, "pairing open for %u s: start pairing on the keypad", (unsigned)pi.left_s);
        break;
    case PLINK_PS_WORKING:
        say(&v->io, "pairing: exchanging keys with \"%s\" at %s", pi.peer_name, addr);
        break;
    case PLINK_PS_COMPARE:
        say(&v->io,
            "pairing code %s for keypad \"%s\" at %s: if the keypad shows the same code, type "
            "allow (else reject)",
            code, pi.peer_name, addr);
        break;
    case PLINK_PS_WAITING: say(&v->io, "pairing: waiting for the other side"); break;
    case PLINK_PS_DONE: say(&v->io, "pairing done: keypad %u", (unsigned)pi.device); break;
    case PLINK_PS_FAILED: say(&v->io, "pairing failed: %s", link_fail_name(pi.fail)); break;
    default: break;
    }
}

void link_master_sync(link_master_t *v, uint64_t now_ms)
{
    v->now = now_ms;
    for (unsigned round = 0; round < 4u; round++) {
        for (size_t i = 0; i < LINK_MASTER_RUNS; i++) {
            if (v->runs[i].used) {
                run_one(v, now_ms, &v->runs[i]);
            }
        }
        const uint32_t gen = alarm_engine_config_generation(&v->eng);
        if (!v->cfg_known || gen != v->cfg_gen) {
            push_config(v, gen);
        }
        /* Events first, then the status that carries their seq (docs/features/11 "Event
         * record"), then the answers. */
        for (size_t i = 0; i < v->fresh_count; i++) {
            char line[160];
            event_text(&v->fresh[i], line, sizeof(line));
            say(&v->io, "event %s", line);
            plink_master_event(&v->m, now_ms, &v->fresh[i]);
        }
        v->fresh_count = 0u;
        for (size_t i = 0; i < v->sound_count; i++) {
            plink_master_sound(&v->m, now_ms, v->sounds[i].partition, v->sounds[i].sound,
                               v->sounds[i].zone);
        }
        v->sound_count = 0u;
        if (v->dirty) {
            v->dirty = false;
            alarm_engine_snapshot(&v->eng, &v->snap);
            v->snap.seq = ++v->snap_seq;
            v->snap.config_seq = gen;
            plink_master_snapshot(&v->m, now_ms, &v->snap, m_epoch(v), PLINK_REPORT_NONE);
        }
        for (size_t i = 0; i < v->answer_count; i++) {
            plink_master_result(&v->m, now_ms, v->answers[i].ticket, &v->answers[i].res);
        }
        v->answer_count = 0u;
        bool more = v->dirty || v->fresh_count > 0u;
        for (size_t i = 0; i < LINK_MASTER_RUNS; i++) {
            more = more || v->runs[i].used;
        }
        if (!more) {
            break;
        }
    }
    say_pairing(v, now_ms);
}

bool link_master_init(link_master_t *v, const plink_crypto_t *c, const link_io_t *io,
                      link_store_t *store, const char *name, const char *panel_id,
                      uint64_t now_ms)
{
    memset(v, 0, sizeof(*v));
    v->c = c;
    v->io = *io;
    v->store = store;
    v->now = now_ms;

    uint8_t salt[ALARM_CODE_SALT_LEN];
    if (!plink_random(c, salt, sizeof(salt))) {
        memset(salt, 0x5A, sizeof(salt));
    }
    house(&v->cfg, salt);
    alarm_ports_t ports;
    memset(&ports, 0, sizeof(ports));
    ports.ctx = v;
    ports.on_event = e_event;
    ports.on_status = e_status;
    ports.on_sound = e_sound;
    ports.on_config_changed = e_config_changed;
    ports.hash_code = m_hash;
    ports.get_epoch_ms = e_epoch;
    char why[96];
    if (alarm_config_validate(&v->cfg, why, sizeof(why)) != ALARM_OK) {
        say(io, "the simulated house is not valid: %s", why);
        return false;
    }
    if (alarm_engine_init(&v->eng, &v->cfg, &ports, NULL, now_ms) != ALARM_OK) {
        say(io, "the engine did not start");
        return false;
    }

    plink_master_port_t port;
    memset(&port, 0, sizeof(port));
    port.ctx = v;
    port.send = m_send;
    port.store = m_store;
    port.run = m_run;
    port.reachable = m_reachable;
    /* port.checkin stays NULL: the engine's check-in clears a device trouble at once, but the
     * link clears a keypad's only when both directions are fresh again (LINK-007), through
     * port.reachable. A link keypad isn't in the engine's device table, so the engine has no
     * timer of its own for it to feed. */
    port.devices_taken = m_taken;
    port.held_changed = m_held;
    port.lockout_left = m_lockout;
    port.events_after = m_events;
    port.note = m_note;

    plink_master_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    const plink_master_rec_t *have = NULL;
    if (store->len > 0u) {
        if (plink_store_master_decode(store->buf, store->len, &rec)) {
            have = &rec;
        } else {
            say(io, "the stored record could not be read: starting with no keypads");
        }
    }
    char id[PLINK_PANEL_ID_LEN + 1];
    snprintf(id, sizeof(id), "%s", panel_id != NULL ? panel_id : "0ac5e1");
    plink_master_init(&v->m, c, &port, have, name != NULL ? name : "PC master", id, now_ms);
    const unsigned count = have != NULL ? rec.count : 0u;
    plink_wipe(&rec, sizeof(rec));
    plink_master_net(&v->m, now_ms, true);
    plink_master_time(&v->m, now_ms, m_epoch(v), LINK_TZ);
    v->dirty = true;
    link_master_sync(v, now_ms);
    say(io, "master \"%s\" (panel %s): %u keypad%s stored", name != NULL ? name : "PC master", id,
        count, count == 1u ? "" : "s");
    return true;
}

void link_master_input(link_master_t *v, uint64_t now_ms, const plink_addr_t *from,
                       const uint8_t *frame, size_t len)
{
    v->now = now_ms;
    plink_master_input(&v->m, now_ms, from, frame, len);
    link_master_sync(v, now_ms);
}

void link_master_tick(link_master_t *v, uint64_t now_ms)
{
    v->now = now_ms;
    if (alarm_engine_next_deadline(&v->eng) <= now_ms) {
        alarm_engine_tick(&v->eng, now_ms);
    }
    if (plink_master_next(&v->m) <= now_ms) {
        plink_master_tick(&v->m, now_ms);
    }
    link_master_sync(v, now_ms);
}

uint64_t link_master_next(const link_master_t *v)
{
    const uint64_t a = alarm_engine_next_deadline(&v->eng);
    const uint64_t b = plink_master_next(&v->m);
    return a < b ? a : b;
}

alarm_result_t link_master_command(link_master_t *v, uint64_t now_ms, const alarm_cmd_t *cmd)
{
    v->now = now_ms;
    const alarm_source_t src = {ALARM_SRC_KEYPAD, ALARM_KEYPAD_CONSOLE};
    const alarm_result_t rc = alarm_engine_command(&v->eng, cmd, &src, now_ms, NULL);
    v->dirty = true;
    link_master_sync(v, now_ms);
    return rc;
}

void link_master_zone(link_master_t *v, uint64_t now_ms, uint16_t zone, bool open)
{
    v->now = now_ms;
    alarm_engine_zone_event(&v->eng, zone, open ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE, now_ms);
    v->dirty = true;
    link_master_sync(v, now_ms);
}

static void say_troubles(const link_io_t *io, const alarm_snapshot_t *s)
{
    char line[256];
    size_t at = 0;
    line[0] = '\0';
    for (unsigned t = 0; t < (unsigned)ALARM_TRBL_COUNT; t++) {
        if ((s->sys_troubles & (1u << t)) != 0u && at < sizeof(line)) {
            const int n = snprintf(line + at, sizeof(line) - at, " %s",
                                   alarm_sys_trouble_name((alarm_sys_trouble_t)t));
            at += n > 0 ? (size_t)n : 0u;
        }
    }
    for (unsigned d = 0; d < 32u; d++) {
        if ((s->dev_troubles & (1u << d)) != 0u && at < sizeof(line)) {
            const int n = snprintf(line + at, sizeof(line) - at, " keypad-%u", d + 1u);
            at += n > 0 ? (size_t)n : 0u;
        }
    }
    say(io, "troubles:%s", line[0] != '\0' ? line : " none");
}

static void say_partitions(const link_io_t *io, const alarm_snapshot_t *s,
                           const alarm_config_t *cfg, uint64_t now_ms)
{
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) == 0u) {
            continue;
        }
        const alarm_partition_status_t *st = &s->part[p];
        const char *name = cfg != NULL ? cfg->partitions[p].name : "";
        char delay[48] = "";
        if (st->delay_remaining_ms > 0u) {
            const uint64_t gone = now_ms > s->taken_ms ? now_ms - s->taken_ms : 0u;
            const uint64_t left =
                st->delay_remaining_ms > gone ? st->delay_remaining_ms - gone : 0u;
            snprintf(delay, sizeof(delay), ", %u s left", (unsigned)((left + 999u) / 1000u));
        }
        say(io, "partition %u \"%s\": %s%s%s%s, %s, %u open, %u bypassed, %u troubles%s%s", p + 1u,
            name, alarm_state_name(st->state), st->level != ALARM_LEVEL_NONE ? " " : "",
            st->level != ALARM_LEVEL_NONE ? alarm_level_name(st->level) : "", delay,
            st->ready ? "ready" : "not ready", (unsigned)st->faults, (unsigned)st->bypassed,
            (unsigned)st->troubles, st->chime ? ", chime on" : "",
            st->triggered ? ", ALARM" : "");
    }
}

static void say_zones(const link_io_t *io, const alarm_snapshot_t *s, const alarm_config_t *cfg)
{
    for (uint16_t z = 1; z <= s->zones_used && z <= ALARM_MAX_ZONES; z++) {
        const alarm_zone_status_t *zs = &s->zone[z - 1u];
        const char *name = "";
        if (cfg != NULL) {
            if (cfg->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
                continue;
            }
            name = cfg->zones[z - 1u].name;
        }
        say(io, "zone %u \"%s\": %s%s%s%s", (unsigned)z, name,
            (zs->flags & ALARM_ZS_FAULT) != 0u ? "open" : "closed",
            (zs->flags & ALARM_ZS_BYPASSED) != 0u ? ", bypassed" : "",
            (zs->flags & ALARM_ZS_ALARM) != 0u ? ", in alarm memory" : "",
            (zs->flags & (ALARM_ZS_TROUBLE | ALARM_ZS_SUPERVISION)) != 0u ? ", trouble" : "");
    }
}

void link_master_say_status(link_master_t *v, uint64_t now_ms)
{
    link_master_sync(v, now_ms);
    alarm_snapshot_t s;
    alarm_engine_snapshot(&v->eng, &s);
    const alarm_config_t *cfg = alarm_engine_config(&v->eng);
    say_partitions(&v->io, &s, cfg, now_ms);
    say_zones(&v->io, &s, cfg);
    say_troubles(&v->io, &s);
    plink_pair_info_t pi;
    plink_master_pair_info(&v->m, now_ms, &pi);
    if (plink_master_pairing(&v->m)) {
        say(&v->io, "pairing: open, %u s left", (unsigned)pi.left_s);
    } else {
        say(&v->io, "pairing: closed");
    }
    plink_peer_info_t peers[PLINK_KEYPADS_MAX];
    const size_t n = plink_master_peers(&v->m, now_ms, peers, PLINK_KEYPADS_MAX);
    size_t online = 0;
    for (size_t i = 0; i < n; i++) {
        online += peers[i].online ? 1u : 0u;
    }
    say(&v->io, "net %04x, %u keypad%s, %u online", (unsigned)plink_master_net_id(&v->m),
        (unsigned)n, n == 1u ? "" : "s", (unsigned)online);
}

void link_master_say_keypads(link_master_t *v, uint64_t now_ms)
{
    plink_peer_info_t peers[PLINK_KEYPADS_MAX];
    const size_t n = plink_master_peers(&v->m, now_ms, peers, PLINK_KEYPADS_MAX);
    if (n == 0u) {
        say(&v->io, "no keypads: type pair to pair one");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const plink_peer_info_t *p = &peers[i];
        char addr[24];
        plink_addr_text(&p->addr, addr);
        char heard[24];
        if (p->heard_ago_s == UINT32_MAX) {
            snprintf(heard, sizeof(heard), "never");
        } else {
            snprintf(heard, sizeof(heard), "%u s ago", (unsigned)p->heard_ago_s);
        }
        char sup[16];
        if (p->supervision_s == 0u) {
            snprintf(sup, sizeof(sup), "off");
        } else {
            snprintf(sup, sizeof(sup), "%u s", (unsigned)p->supervision_s);
        }
        say(&v->io,
            "keypad %u \"%s\" (%s): home partition %u, %s%s%s, supervision %s, heard %s at %s, "
            "fw %s, up %u s",
            (unsigned)p->device, p->name, kind_name(p->kind), (unsigned)p->home,
            p->online ? "online" : "offline", p->provisional ? ", not yet connected" : "",
            p->down ? ", TROUBLE" : "", sup, heard, addr, p->hb.fw[0] != '\0' ? p->hb.fw : "-",
            (unsigned)p->hb.uptime_s);
    }
}

/* ---- the virtual keypad -------------------------------------------------------------------- */

static bool k_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    link_keypad_t *v = ctx;
    return v->io.send(v->io.send_ctx, to, frame, len);
}

static bool k_store(void *ctx, const plink_keypad_rec_t *rec)
{
    link_keypad_t *v = ctx;
    if (rec == NULL) {
        return link_store_erase(v->store);
    }
    uint8_t buf[PLINK_STORE_MAX];
    size_t len = 0;
    const bool ok = plink_store_keypad_encode(rec, buf, sizeof(buf), &len) &&
                    link_store_write(v->store, buf, len);
    plink_wipe(buf, sizeof(buf));
    if (!ok) {
        say(&v->io, "the keypad's record could not be stored");
    }
    return ok;
}

/* port.resolve: before a handshake retry, look for the master again. On the PC that is where
 * it was said to be (--master), or the broadcast address. */
static void k_resolve(void *ctx)
{
    link_keypad_t *v = ctx;
    if (v->have_find) {
        plink_keypad_master_addr(&v->k, v->now, &v->find);
    }
}

static bool is_broadcast(const plink_addr_t *a)
{
    return a->tp == PLINK_TP_UDP && a->len >= 4u && a->a[0] == 255u && a->a[1] == 255u &&
           a->a[2] == 255u && a->a[3] == 255u;
}

void link_keypad_init(link_keypad_t *v, const plink_crypto_t *c, const link_io_t *io,
                      link_store_t *store, const char *name, const plink_addr_t *master,
                      uint64_t now_ms)
{
    memset(v, 0, sizeof(*v));
    v->c = c;
    v->io = *io;
    v->store = store;
    v->now = now_ms;
    if (master != NULL) {
        v->have_find = true;
        v->find = *master;
    }
    memset(v->said_part_state, 0xFF, sizeof(v->said_part_state)); /* nothing said yet */
    plink_keypad_port_t port;
    memset(&port, 0, sizeof(port));
    port.ctx = v;
    port.send = k_send;
    port.store = k_store;
    port.resolve = k_resolve;

    plink_keypad_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    const plink_keypad_rec_t *have = NULL;
    if (store->len > 0u) {
        if (plink_store_keypad_decode(store->buf, store->len, &rec)) {
            have = &rec;
            say(io, "paired with \"%s\" (panel %s) as keypad %u, home partition %u",
                rec.master_name, rec.panel_id, (unsigned)rec.device, (unsigned)rec.home);
        } else {
            say(io, "the stored record could not be read: not paired");
        }
    }
    const plink_caps_t cap = {
        .partitions = ALARM_MAX_PARTITIONS,
        .zones = ALARM_MAX_ZONES,
        .users = ALARM_MAX_USERS,
        .devices = ALARM_MAX_DEVICES,
    };
    plink_keypad_init(&v->k, c, &port, have, v->cfg_buf, sizeof(v->cfg_buf), &v->cfg,
                      PLINK_KIND_PC, &cap, name != NULL ? name : "PC keypad", LINK_FW, now_ms);
    plink_wipe(&rec, sizeof(rec));
    if (master != NULL) {
        plink_keypad_master_addr(&v->k, now_ms, master);
    }
}

void link_keypad_master(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *master)
{
    v->now = now_ms;
    v->have_find = true;
    v->find = *master;
    plink_keypad_master_addr(&v->k, now_ms, master);
}

void link_keypad_pair(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *master)
{
    v->now = now_ms;
    v->pair_asked = is_broadcast(master);
    v->pair_found = false;
    plink_keypad_pair_start(&v->k, now_ms, master, 0u);
}

/* Pairing asked by broadcast (no mDNS on the PC). The keypad's pairing takes PAIR_COMMIT only
 * from the address it asked - a master that answers from elsewhere ends the attempt as a second
 * master - so the first master to answer is where the pairing starts again, addressed to it.
 * Asking ends at the masters that answered (PAIR_FAIL by broadcast), and a second master that
 * answers stops it all, as two masters open for pairing must (docs/features/11 "Pairing" 2).
 * true: the frame was taken here. */
static bool pair_discovery(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *from,
                           const uint8_t *frame, size_t len)
{
    plink_hdr_t h;
    if ((!v->pair_asked && !v->pair_found) || !plink_hdr_read(frame, len, &h) ||
        h.type != PLINK_T_PAIR_COMMIT) {
        return false;
    }
    plink_pair_info_t pi;
    plink_keypad_pair_info(&v->k, now_ms, &pi);
    if (pi.state == PLINK_PS_IDLE || pi.state == PLINK_PS_DONE || pi.state == PLINK_PS_FAILED) {
        v->pair_asked = v->pair_found = false;
        return false;
    }
    char addr[24];
    plink_addr_text(from, addr);
    if (v->pair_asked) {
        v->pair_asked = false;
        v->pair_found = true;
        v->found = *from;
        say(&v->io, "a master answered from %s: pairing with it", addr);
        plink_keypad_pair_cancel(&v->k, now_ms);
        plink_keypad_pair_start(&v->k, now_ms, from, h.net_id);
        return true;
    }
    if (!plink_addr_equal(from, &v->found)) {
        v->pair_found = false;
        say(&v->io,
            "a second master answered from %s: more than one alarm panel is open for pairing - "
            "pairing stopped (give the address: pair IP)",
            addr);
        plink_keypad_pair_cancel(&v->k, now_ms);
        return true;
    }
    return false;
}

void link_keypad_input(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *from,
                       const uint8_t *frame, size_t len)
{
    v->now = now_ms;
    if (pair_discovery(v, now_ms, from, frame, len)) {
        return;
    }
    plink_keypad_input(&v->k, now_ms, from, frame, len);
}

void link_keypad_tick(link_keypad_t *v, uint64_t now_ms)
{
    v->now = now_ms;
    if (plink_keypad_next(&v->k) <= now_ms) {
        plink_keypad_tick(&v->k, now_ms);
    }
}

uint64_t link_keypad_next(const link_keypad_t *v)
{
    return plink_keypad_next(&v->k);
}

uint8_t link_keypad_home(const link_keypad_t *v, uint64_t now_ms)
{
    plink_keypad_info_t in;
    plink_keypad_info(&v->k, now_ms, &in);
    return in.home >= 1u && in.home <= ALARM_MAX_PARTITIONS ? in.home : 1u;
}

static void say_link(link_keypad_t *v, const plink_keypad_info_t *in)
{
    char addr[24];
    plink_addr_text(&in->master, addr);
    switch (in->state) {
    case PLINK_LINK_NOT_PAIRED:
        if (in->unpaired_why == PLINK_UNPAIRED_REMOVED) {
            say(&v->io, "link: not paired - this keypad was removed at the master");
        } else {
            say(&v->io, "link: not paired - type pair to pair with a master");
        }
        break;
    case PLINK_LINK_CONNECTING:
        say(&v->io, "link: connecting to \"%s\" at %s", in->master_name, addr);
        break;
    case PLINK_LINK_ONLINE:
        say(&v->io, "link: online with \"%s\" at %s as keypad %u, home partition %u",
            in->master_name, addr, (unsigned)in->device, (unsigned)in->home);
        break;
    case PLINK_LINK_LOST:
        say(&v->io, "link: no system communication (nothing from \"%s\" for 30 s)",
            in->master_name);
        break;
    default: break;
    }
    if (in->cfg_bad_crc != 0u) {
        say(&v->io, "the master's configuration could not be read: this keypad needs an update");
    }
}

static void say_keypad_pairing(link_keypad_t *v, uint64_t now_ms)
{
    plink_pair_info_t pi;
    plink_keypad_pair_info(&v->k, now_ms, &pi);
    if (pi.state == v->said_pair && strcmp(pi.code, v->said_code) == 0) {
        return;
    }
    v->said_pair = pi.state;
    snprintf(v->said_code, sizeof(v->said_code), "%s", pi.code);
    char addr[24];
    plink_addr_text(&pi.peer, addr);
    char code[8];
    link_code_text(pi.code, code);
    switch (pi.state) {
    case PLINK_PS_OPEN: say(&v->io, "pairing: asking for a master at %s", addr); break;
    case PLINK_PS_WORKING:
        say(&v->io, "pairing: exchanging keys with \"%s\" at %s", pi.peer_name, addr);
        break;
    case PLINK_PS_COMPARE:
        say(&v->io,
            "pairing code %s from master \"%s\" at %s: if the master shows the same code, type "
            "yes (else no)",
            code, pi.peer_name, addr);
        break;
    case PLINK_PS_WAITING: say(&v->io, "pairing: waiting for the installer at the master"); break;
    case PLINK_PS_DONE: say(&v->io, "pairing done: keypad %u", (unsigned)pi.device); break;
    case PLINK_PS_FAILED: say(&v->io, "pairing failed: %s", link_fail_name(pi.fail)); break;
    default: break;
    }
}

void link_keypad_report(link_keypad_t *v, uint64_t now_ms)
{
    plink_keypad_info_t in;
    plink_keypad_info(&v->k, now_ms, &in);
    if (!v->said_any || in.state != v->said_state || in.unpaired_why != v->said_why) {
        v->said_any = true;
        v->said_state = in.state;
        v->said_why = in.unpaired_why;
        say_link(v, &in);
    }
    say_keypad_pairing(v, now_ms);

    const uint32_t cfg_seq = plink_keypad_config_seq(&v->k);
    const alarm_config_t *cfg = plink_keypad_config(&v->k);
    if (cfg_seq != v->said_cfg_seq && cfg != NULL) {
        v->said_cfg_seq = cfg_seq;
        unsigned zones = 0;
        for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
            zones += cfg->zones[z].type != ALARM_ZT_NOT_USED ? 1u : 0u;
        }
        say(&v->io, "configuration from the master: %u zones", zones);
    }

    const alarm_snapshot_t *s = plink_keypad_snapshot(&v->k);
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) == 0u) {
            continue;
        }
        const alarm_partition_status_t *st = &s->part[p];
        if ((uint8_t)st->state != v->said_part_state[p] ||
            (uint8_t)st->level != v->said_part_level[p]) {
            v->said_part_state[p] = (uint8_t)st->state;
            v->said_part_level[p] = (uint8_t)st->level;
            say(&v->io, "partition %u: %s%s%s", p + 1u, alarm_state_name(st->state),
                st->level != ALARM_LEVEL_NONE ? " " : "",
                st->level != ALARM_LEVEL_NONE ? alarm_level_name(st->level) : "");
        }
    }

    alarm_event_t ev[PLINK_EVENTS_KEPT];
    const size_t n = plink_keypad_events(&v->k, ev, PLINK_EVENTS_KEPT); /* newest first */
    if (n > 0u) {
        if (ev[0].seq < v->said_ev_seq) {
            v->said_ev_seq = 0u; /* the master restarted: its numbers began again */
        }
        const size_t first_batch = v->said_ev_seq == 0u ? 5u : n;
        size_t fresh = 0;
        while (fresh < n && ev[fresh].seq > v->said_ev_seq) {
            fresh++;
        }
        if (fresh > first_batch) {
            fresh = first_batch;
        }
        for (size_t i = fresh; i > 0u; i--) {
            char line[160];
            event_text(&ev[i - 1u], line, sizeof(line));
            say(&v->io, "event %s", line);
        }
        v->said_ev_seq = ev[0].seq;
    }

    plink_sound_t snd;
    while (plink_keypad_take_sound(&v->k, &snd)) {
        if (snd.zone != 0u) {
            say(&v->io, "sound: %s, partition %u, zone %u",
                alarm_sound_name((alarm_sound_t)snd.sound), (unsigned)snd.partition,
                (unsigned)snd.zone);
        } else {
            say(&v->io, "sound: %s, partition %u", alarm_sound_name((alarm_sound_t)snd.sound),
                (unsigned)snd.partition);
        }
    }
    plink_time_t t;
    if (plink_keypad_take_time(&v->k, &t)) {
        say(&v->io, "clock from the master (time zone %s)", t.tz[0] != '\0' ? t.tz : "-");
    }

    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        link_pending_t *p = &v->pending[i];
        if (p->req == 0u) {
            continue;
        }
        plink_result_t res;
        memset(&res, 0, sizeof(res));
        const plink_cmd_state_t st = plink_keypad_command_state(&v->k, p->req, &res);
        if (st == PLINK_CMD_PENDING) {
            continue;
        }
        if (st == PLINK_CMD_DONE) {
            if (res.result == ALARM_ERR_NOT_READY && res.not_ready_zone != 0u) {
                say(&v->io, "%s: %s (zone %u)", p->what,
                    alarm_result_name((alarm_result_t)res.result), (unsigned)res.not_ready_zone);
            } else if (res.user != 0u) {
                say(&v->io, "%s: %s (user %u)", p->what,
                    alarm_result_name((alarm_result_t)res.result), (unsigned)res.user);
            } else {
                say(&v->io, "%s: %s", p->what, alarm_result_name((alarm_result_t)res.result));
            }
        } else if (st == PLINK_CMD_NO_ANSWER) {
            say(&v->io, "%s: no answer from the master", p->what);
        }
        plink_keypad_command_release(&v->k, p->req);
        p->req = 0u;
    }
}

uint32_t link_keypad_command(link_keypad_t *v, uint64_t now_ms, plink_cmd_t *cmd,
                             const char *what)
{
    const uint32_t req = plink_keypad_command(&v->k, now_ms, cmd);
    plink_wipe(cmd, sizeof(*cmd));
    if (req == 0u) {
        plink_keypad_info_t in;
        plink_keypad_info(&v->k, now_ms, &in);
        say(&v->io, "%s: refused here: %s", what,
            in.state != PLINK_LINK_ONLINE ? "the link is not online"
                                          : "too many commands are waiting");
        return 0u;
    }
    for (size_t i = 0; i < PLINK_CMDS_MAX; i++) {
        if (v->pending[i].req == 0u) {
            v->pending[i].req = req;
            snprintf(v->pending[i].what, sizeof(v->pending[i].what), "%s", what);
            break;
        }
    }
    return req;
}

void link_keypad_say_status(link_keypad_t *v, uint64_t now_ms)
{
    plink_keypad_info_t in;
    plink_keypad_info(&v->k, now_ms, &in);
    say_link(v, &in);
    if (in.state != PLINK_LINK_NOT_PAIRED) {
        char age[24];
        if (in.status_age_s == UINT32_MAX) {
            snprintf(age, sizeof(age), "none yet");
        } else {
            snprintf(age, sizeof(age), "%u s old", (unsigned)in.status_age_s);
        }
        say(&v->io, "for %u s; status %s%s", (unsigned)in.since_s, age,
            in.maintenance ? "; maintenance pages open without a code (LINK-025)" : "");
    }
    if (plink_keypad_panic_pending(&v->k)) {
        say(&v->io, "a panic is still being sent");
    }
    const alarm_snapshot_t *s = plink_keypad_snapshot(&v->k);
    if (s->partitions == 0u) {
        say(&v->io, "no status from the master yet");
        return;
    }
    const alarm_config_t *cfg = plink_keypad_config(&v->k);
    say_partitions(&v->io, s, cfg, now_ms);
    say_zones(&v->io, s, cfg);
    say_troubles(&v->io, s);
    if (s->local_lockout_until > now_ms) {
        say(&v->io, "this keypad is locked out for %u s",
            (unsigned)((s->local_lockout_until - now_ms + 999u) / 1000u));
    }
}
