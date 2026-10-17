/*
 * link_node.c - see link_node.h.
 */
#include "link_node.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "link_fw.h"
#include "panel_link/plink_store.h"
#include "panel_link/plink_view.h"

#define VIRTUAL_EPOCH_MS 1790000000000LL /* link_smoke's wall clock at ms 0 */
#define LINK_FW          "link_sim 1"
#define LINK_TZ          "UTC0"
/* What both sides offer (LINK-043): Smart Home and settings on keypads (BL-216). */
#define LINK_CAPS        (PLINK_CAP_HOME | PLINK_CAP_PREFS)
/* A keypad's announcements' fields: the PC keypad has no voice, so it doesn't take them (as a
 * panel built without one). */
#define LINK_PREFS_VOICE                                                                           \
    (PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_VOLUME) | PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_ENABLED) |  \
     PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_STATUS) | PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_CHIME))

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

/* ---- Smart Home and settings, both sides ---------------------------------------------------- */

static const char *const ACTION_NAMES[] = {"none",   "on",   "off",   "toggle", "lock",
                                           "unlock", "open", "close", "stop",   "level",
                                           "temp",   "mode", "run"};
_Static_assert(sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0]) == (size_t)ACT_COUNT,
               "a word for every dev_action_t");

const char *link_action_name(uint8_t action)
{
    return action < (uint8_t)ACT_COUNT ? ACTION_NAMES[action] : "action";
}

uint8_t link_action_parse(const char *word)
{
    for (uint8_t a = 1; word != NULL && a < (uint8_t)ACT_COUNT; a++) {
        if (strcmp(word, ACTION_NAMES[a]) == 0) {
            return a;
        }
    }
    return (uint8_t)ACT_NONE;
}

const char *link_dev_result_name(uint8_t result)
{
    switch (result) {
    case PLINK_DEV_OK: return "done";
    case PLINK_DEV_NEED_CODE: return "needs a code";
    case PLINK_DEV_CODE_REFUSED: return "the code was refused";
    case PLINK_DEV_BUSY: return "busy";
    case PLINK_DEV_UNREACHABLE: return "can't be reached";
    case PLINK_DEV_REFUSED: return "refused";
    case PLINK_DEV_UNAVAILABLE: return "the master serves no devices";
    default: return "refused";
    }
}

static const char *kind_word(uint8_t kind)
{
    static const char *const t[] = {"device", "light", "switch", "lock",  "thermostat",
                                    "cover",  "fan",   "valve",  "scene", "script"};
    return kind < sizeof(t) / sizeof(t[0]) ? t[kind] : "device";
}

/* Appends to out (at *at, cap bytes) as snprintf would. */
static void add_text(char *out, size_t cap, size_t *at, const char *fmt, ...) LINK_PRINTF(4, 5);
static void add_text(char *out, size_t cap, size_t *at, const char *fmt, ...)
{
    if (*at >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + *at, cap - *at, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *at += (size_t)n < cap - *at ? (size_t)n : cap - *at - 1u;
    }
}

/* label then hundredths of a degree as a number: "21.50", "-1.25". */
static void add_hundredths(char *out, size_t cap, size_t *at, const char *label, int32_t x)
{
    const uint32_t a = x < 0 ? 0u - (uint32_t)x : (uint32_t)x;
    add_text(out, cap, at, "%s%s%u.%02u", label, x < 0 ? "-" : "", (unsigned)(a / 100u),
             (unsigned)(a % 100u));
}

void link_prefs_text(const proto_prefs_values_t *v, char *out, size_t cap)
{
    size_t at = 0;
    if (cap == 0u) {
        return;
    }
    out[0] = '\0';
    for (unsigned id = 1; id <= PROTO_PREFS_F_LAST; id++) {
        const proto_prefs_field_t *f = proto_prefs_field(id);
        uint32_t x = 0;
        if (f == NULL || !proto_prefs_get(v, id, &x)) {
            continue;
        }
        add_text(out, cap, &at, "%s%s ", at > 0u ? ", " : "", f->key);
        if ((f->flags & PROTO_PREFS_FL_SWITCH) != 0u) {
            add_text(out, cap, &at, "%s", x != 0u ? "on" : "off");
        } else if ((f->flags & PROTO_PREFS_FL_DELAY) != 0u) {
            if (x == 0u) {
                add_text(out, cap, &at, "never");
            } else {
                add_text(out, cap, &at, "%u s", (unsigned)(x / 1000u));
            }
        } else if (id == PROTO_PREFS_F_NIGHT_START || id == PROTO_PREFS_F_NIGHT_END) {
            add_text(out, cap, &at, "%02u:%02u", (unsigned)(x / 60u), (unsigned)(x % 60u));
        } else {
            add_text(out, cap, &at, "%u", (unsigned)x);
        }
    }
    if (at == 0u) {
        snprintf(out, cap, "none");
    }
}

/* A whole number of at most max (false: not one). */
static bool whole(const char *s, unsigned long max, unsigned long *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    unsigned long v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        const unsigned long d = (unsigned long)(*p - '0');
        if (d > max || v > (max - d) / 10u) {
            return false; /* past max (and never wraps) */
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

const char *link_prefs_parse(const char *key, const char *text, uint8_t *id, uint32_t *value)
{
    const uint8_t i = key != NULL ? proto_prefs_id(key) : 0u;
    const proto_prefs_field_t *f = proto_prefs_field(i);
    if (f == NULL) {
        return "not a settings key (brightness, saver_level, saver_after, off_after, night, "
               "night_start, night_end, night_level, night_off, clock, appearance, contrast, "
               "beeps, chime_vol, voice_vol, key_click, voice, voice_stat, voice_chime)";
    }
    unsigned long v = 0;
    if ((f->flags & PROTO_PREFS_FL_SWITCH) != 0u) {
        if (text != NULL && (strcmp(text, "on") == 0 || strcmp(text, "1") == 0)) {
            v = 1u;
        } else if (text != NULL && (strcmp(text, "off") == 0 || strcmp(text, "0") == 0)) {
            v = 0u;
        } else {
            return "on or off";
        }
    } else if ((f->flags & PROTO_PREFS_FL_DELAY) != 0u) {
        if (text != NULL && strcmp(text, "never") == 0) {
            v = 0u;
        } else if (whole(text, 86400u, &v)) {
            v *= 1000u;
        } else {
            return "seconds (0 or never: never)";
        }
    } else if (i == PROTO_PREFS_F_NIGHT_START || i == PROTO_PREFS_F_NIGHT_END) {
        const char *colon = text != NULL ? strchr(text, ':') : NULL;
        unsigned long h = 0, m = 0;
        char hh[4];
        if (colon == NULL || colon - text < 1 || colon - text > 2) {
            return "a time of day, HH:MM";
        }
        memcpy(hh, text, (size_t)(colon - text));
        hh[colon - text] = '\0';
        if (!whole(hh, 23u, &h) || strlen(colon + 1) != 2u || !whole(colon + 1, 59u, &m)) {
            return "a time of day, HH:MM";
        }
        v = h * 60u + m;
    } else if (!whole(text, 0xFFFFFFFFul, &v)) {
        return "a whole number";
    }
    if (proto_prefs_check(i, (uint32_t)v) != PROTO_PREFS_WHY_NONE) {
        return "out of range";
    }
    *id = i;
    *value = (uint32_t)v;
    return NULL;
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

/* ---- the virtual master: Smart Home and settings on keypads (BL-216) ------------------------- */

static dev_info_t *house_add(link_house_t *h, uint8_t kind, const char *ref, const char *name,
                             const char *area, const char *state)
{
    dev_info_t *d = &h->dev[h->n++];
    memset(d, 0, sizeof(*d));
    d->source = (uint8_t)DSRC_HA;
    d->kind = kind;
    snprintf(d->ref, sizeof(d->ref), "%s", ref);
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->area, sizeof(d->area), "%s", area);
    snprintf(d->state, sizeof(d->state), "%s", state);
    d->online = true;
    return d;
}

/* The house's devices (link_node.h), as Home Assistant would report them. */
static void house_init(link_house_t *h, uint32_t epoch_s)
{
    memset(h, 0, sizeof(*h));
    dev_info_t *d = house_add(h, DEV_LIGHT, "light.porch", "Porch Light", "Outside", "on");
    d->values.flags = DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET;
    d->values.level = 80u;
    d->values.caps = DEV_CAP_KNOWN | DEV_CAP_DIM;
    d = house_add(h, DEV_LIGHT, "light.kitchen", "Kitchen Lights", "Kitchen", "off");
    d->values.flags = DEV_VAL_LEVEL_SET;
    d->values.caps = DEV_CAP_KNOWN | DEV_CAP_DIM;
    (void)house_add(h, DEV_LOCK, "lock.front_door", "Front Door", "Living room", "locked");
    d = house_add(h, DEV_COVER, "cover.garage_door", "Garage Door", "Outside", "closed");
    d->values.flags = DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET;
    d->values.caps = DEV_CAP_KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION;
    d = house_add(h, DEV_CLIMATE, "climate.hallway", "Hallway", "Living room", "heat");
    d->values.flags = DEV_VAL_SETPOINT | DEV_VAL_TEMP;
    d->values.setpoint = 2150;
    d->values.temp = 2100;
    d->values.modes = DEV_MODE_OFF | DEV_MODE_HEAT | DEV_MODE_COOL | DEV_MODE_AUTO;
    d->values.hvac_action = (uint8_t)DEV_HVAC_HEATING;
    d = house_add(h, DEV_SCENE, "scene.good_night", "Good Night", "Living room", "");
    d->values.since = epoch_s - 7200u; /* it ran two hours ago */
}

static dev_info_t *house_find(link_house_t *h, const char *ref)
{
    for (size_t i = 0; ref != NULL && i < h->n; i++) {
        if (strcmp(h->dev[i].ref, ref) == 0) {
            return &h->dev[i];
        }
    }
    return NULL;
}

/* What an action did, as the house reports it afterwards. */
static void house_apply(dev_info_t *d, const link_house_act_t *a, uint32_t epoch_s)
{
    const char *word = NULL;
    dev_values_t *x = &d->values;
    switch (a->action) {
    case ACT_ON:
        word = "on";
        if (d->kind == DEV_LIGHT && a->value >= 0) {
            x->level = (uint8_t)(a->value > 100 ? 100 : a->value);
            x->flags |= DEV_VAL_LEVEL;
            word = a->value > 0 ? "on" : "off"; /* a brightness of 0 is off (B030) */
        }
        break;
    case ACT_OFF: word = "off"; break;
    case ACT_TOGGLE: word = strcmp(d->state, "on") == 0 ? "off" : "on"; break;
    case ACT_LOCK: word = "locked"; break;
    case ACT_UNLOCK: word = "unlocked"; break;
    case ACT_OPEN:
        word = "open";
        x->level = 100u;
        break;
    case ACT_CLOSE:
        word = "closed";
        x->level = 0u;
        break;
    case ACT_STOP: break; /* stopped where it was */
    case ACT_SET_LEVEL: {
        const int32_t l = a->value < 0 ? 0 : a->value > 100 ? 100 : a->value;
        x->level = (uint8_t)l;
        x->flags |= DEV_VAL_LEVEL;
        word = d->kind == DEV_COVER ? (l > 0 ? "open" : "closed") : (l > 0 ? "on" : "off");
        break;
    }
    case ACT_SET_TEMP:
        x->setpoint = a->value;
        x->flags |= DEV_VAL_SETPOINT;
        break;
    case ACT_SET_MODE:
        word = a->mode;
        x->hvac_action = strcmp(a->mode, "off") == 0    ? (uint8_t)DEV_HVAC_OFF
                         : strcmp(a->mode, "cool") == 0 ? (uint8_t)DEV_HVAC_COOLING
                         : strcmp(a->mode, "heat") == 0 ? (uint8_t)DEV_HVAC_HEATING
                                                        : (uint8_t)DEV_HVAC_IDLE;
        break;
    case ACT_RUN: x->since = epoch_s; break;
    default: break;
    }
    if (word != NULL) {
        snprintf(d->state, sizeof(d->state), "%s", word);
    }
}

static bool hs_online(void *ctx)
{
    const link_master_t *v = ctx;
    return !v->house.gone; /* the house's Home Assistant link */
}

static size_t hs_list(void *ctx, dev_info_t *out, size_t max)
{
    link_master_t *v = ctx;
    const size_t n = v->house.gone ? 0u : v->house.n < max ? v->house.n : max;
    memcpy(out, v->house.dev, n * sizeof(out[0]));
    return n;
}

/* The source's act: sent on, answered LINK_HOUSE_MS later (house_tick). */
static bool hs_act(void *ctx, const char *ref, dev_action_t action, int32_t value, const char *mode,
                   uint32_t request_id)
{
    link_master_t *v = ctx;
    const dev_info_t *d = house_find(&v->house, ref);
    if (d == NULL || !d->online || request_id == 0u) {
        return false;
    }
    for (size_t i = 0; i < LINK_HOUSE_ACTS; i++) {
        link_house_act_t *a = &v->house.acts[i];
        if (a->req == 0u) {
            memset(a, 0, sizeof(*a));
            a->req = request_id;
            snprintf(a->ref, sizeof(a->ref), "%s", ref);
            a->action = (uint8_t)action;
            a->value = value;
            snprintf(a->mode, sizeof(a->mode), "%s", mode != NULL ? mode : "");
            a->due = v->now + LINK_HOUSE_MS;
            return true;
        }
    }
    return false;
}

static const device_source_ops_t HOUSE_OPS = {
    .kind = (uint8_t)DSRC_HA,
    .name = "house",
    .online = hs_online,
    .list = hs_list,
    .act = hs_act,
};

/* The house answers the actions whose time has come: the device changed, then the answer. */
static void house_tick(link_master_t *v, uint64_t now)
{
    for (size_t i = 0; i < LINK_HOUSE_ACTS; i++) {
        link_house_act_t *a = &v->house.acts[i];
        if (a->req == 0u || a->due > now) {
            continue;
        }
        dev_info_t *d = house_find(&v->house, a->ref);
        const bool ok = d != NULL && d->online;
        if (ok) {
            house_apply(d, a, (uint32_t)(m_epoch(v) / 1000));
        }
        const uint32_t req = a->req;
        memset(a, 0, sizeof(*a));
        v->house.sink->changed(v->house.sink->ctx, (uint8_t)DSRC_HA);
        v->house.sink->result(v->house.sink->ctx, req, ok, ok ? "" : "unavailable", false);
    }
}

static uint64_t house_next(const link_master_t *v)
{
    uint64_t t = UINT64_MAX;
    for (size_t i = 0; i < LINK_HOUSE_ACTS; i++) {
        if (v->house.acts[i].req != 0u && v->house.acts[i].due < t) {
            t = v->house.acts[i].due;
        }
    }
    return t;
}

static void h_changed(void *ctx)
{
    link_master_t *v = ctx;
    v->dev_dirty = true;
}

static void h_result(void *ctx, uint32_t request_id, const char *ref, bool ok, const char *error)
{
    (void)request_id;
    link_master_t *v = ctx;
    v->dev_dirty = true;
    if (ok) {
        say(&v->io, "device %s: done", ref);
    } else {
        say(&v->io, "device %s: failed (%s)", ref, error != NULL ? error : "");
    }
}

/* The registry's list and records to the link, as the plink_dev task builds them: into the blob
 * the link isn't reading (it keeps the last one handed in). */
static void dev_build(link_master_t *v, uint64_t now)
{
    hc_refresh(&v->hc);
    const hc_device_t *p[HC_MAX_DEVICES];
    const size_t n = hc_list(&v->hc, p, HC_MAX_DEVICES);
    for (size_t i = 0; i < n; i++) {
        v->rows[i] = *p[i];
    }
    const unsigned next = v->dblob_cur ^ 1u;
    size_t len = 0;
    uint32_t crc = 0;
    v->dev_build_at = now + LINK_DEV_BUILD_MS;
    if (!plink_home_list_build(v->rows, n, v->dblob[next], sizeof(v->dblob[next]), &len, &crc)) {
        say(&v->io, "the keypads' device list could not be made");
        return;
    }
    plink_home_recs_build(v->rows, n, &v->drecs);
    v->dblob_cur = next;
    if (crc != v->dev_list_crc) {
        say(&v->io, "devices: %u listed for the keypads (list %08x)", (unsigned)n, (unsigned)crc);
    }
    v->dev_list_crc = crc;
    v->dev_count = n;
    plink_master_home(&v->m, now, v->dblob[next], len, crc, &v->drecs);
}

/* port.dev_run: copied for the judge, which runs between two calls of the link manager. */
static bool m_dev_run(void *ctx, uint32_t ticket, uint8_t device, uint8_t home,
                      const plink_dev_act_t *act)
{
    link_master_t *v = ctx;
    for (size_t i = 0; i < LINK_DEV_JOBS; i++) {
        link_dev_job_t *j = &v->jobs[i];
        if (!j->used) {
            j->used = true;
            j->ticket = ticket;
            j->device = device;
            j->home = home;
            j->act = *act;
            return true;
        }
    }
    return false;
}

/* The one judge of both screens, as device_service_person_act (LINK-048): a keypad's action as
 * {ALARM_SRC_KEYPAD, n} for its home partition. */
static uint8_t dev_judge(link_master_t *v, uint64_t now, const link_dev_job_t *j, uint8_t *why)
{
    *why = 0u;
    const plink_dev_act_t *a = &j->act;
    const hc_device_t *d = hc_get(&v->hc, a->ref);
    if (d == NULL || d->info.source == (uint8_t)DSRC_ZWAVE ||
        !hc_action_valid((dev_kind_t)d->info.kind, (dev_action_t)a->action)) {
        return PLINK_DEV_REFUSED;
    }
    if (hc_needs_code((dev_kind_t)d->info.kind, (dev_action_t)a->action, a->value)) {
        if (a->code[0] == '\0') {
            return PLINK_DEV_NEED_CODE;
        }
        const alarm_source_t src = {ALARM_SRC_KEYPAD, j->device};
        uint8_t user = 0;
        alarm_result_t r = alarm_engine_authenticate(&v->eng, &src, a->code, j->home, now, &user);
        /* an arm-only or a guest code may not unlock (BL-045) */
        if (r == ALARM_OK && user >= 1u && user <= ALARM_MAX_USERS) {
            const alarm_authority_t au = alarm_engine_config(&v->eng)->users[user - 1u].authority;
            if (au == ALARM_AUTH_ARM_ONLY || au == ALARM_AUTH_GUEST) {
                r = ALARM_ERR_NOT_AUTHORIZED;
            }
        }
        if (r != ALARM_OK) {
            *why = (uint8_t)r;
            return PLINK_DEV_CODE_REFUSED;
        }
    }
    const char *mode = NULL;
    if (a->action == (uint8_t)ACT_SET_MODE) {
        mode = hc_mode_word(a->mode);
        if (mode == NULL) {
            return PLINK_DEV_REFUSED;
        }
    }
    switch (hc_act(&v->hc, a->ref, (dev_action_t)a->action, a->value, mode)) {
    case HC_OK: v->dev_dirty = true; return PLINK_DEV_OK; /* the row shows it pending */
    case HC_ERR_BUSY: return PLINK_DEV_BUSY;
    case HC_ERR_OFFLINE: return PLINK_DEV_UNREACHABLE;
    default: return PLINK_DEV_REFUSED;
    }
}

/* The keypads' device actions: each judged (its code wiped), then the registry built, then the
 * answers - so a DEV_ACK's record already shows the action under way, as the plink_dev task posts
 * RES_DEVICES before RES_DEV_ACK. */
static void dev_jobs(link_master_t *v, uint64_t now)
{
    struct {
        uint32_t ticket;
        uint8_t result, why;
    } ans[LINK_DEV_JOBS];
    size_t n = 0;
    for (size_t i = 0; i < LINK_DEV_JOBS; i++) {
        link_dev_job_t *j = &v->jobs[i];
        if (!j->used) {
            continue;
        }
        uint8_t why = 0;
        const uint8_t r = dev_judge(v, now, j, &why);
        v->dev_runs++;
        say(&v->io, "keypad %u: %s %s%s: %s%s%s", (unsigned)j->device,
            link_action_name(j->act.action), j->act.ref,
            j->act.code[0] != '\0' ? " with a code" : "", link_dev_result_name(r),
            r == PLINK_DEV_CODE_REFUSED ? ": " : "",
            r == PLINK_DEV_CODE_REFUSED ? alarm_result_name((alarm_result_t)why) : "");
        ans[n].ticket = j->ticket;
        ans[n].result = r;
        ans[n].why = why;
        n++;
        plink_wipe(j, sizeof(*j));
    }
    if (n == 0u) {
        return;
    }
    dev_build(v, now);
    v->dev_dirty = false;
    for (size_t i = 0; i < n; i++) {
        plink_master_dev_answer(&v->m, now, ans[i].ticket, ans[i].result, ans[i].why);
    }
}

static void m_prefs_done(void *ctx, uint8_t device, const plink_prefs_done_t *d)
{
    link_master_t *v = ctx;
    link_pset_end_t *e = &v->pset_end;
    const unsigned count = e->count + 1u;
    memset(e, 0, sizeof(*e));
    e->count = count;
    e->device = device;
    e->req = d->req;
    e->result = d->result;
    e->refused_count = d->refused_count < PLINK_PREFS_REFUSED_MAX
                           ? d->refused_count
                           : (uint8_t)PLINK_PREFS_REFUSED_MAX;
    if (e->refused_count > 0u && d->refused != NULL) {
        memcpy(e->refused, d->refused, 2u * (size_t)e->refused_count);
    }
    proto_prefs_decoded_t kept;
    if (d->result == PLINK_PSET_OK && d->fields != NULL &&
        proto_prefs_decode(d->fields, d->fields_len, PROTO_PREFS_KNOWN, &kept)) {
        e->values = kept.values;
    }
    switch (d->result) {
    case PLINK_PSET_OK:
        say(&v->io, "keypad %u answered the settings change: %u field%s refused", (unsigned)device,
            (unsigned)e->refused_count, e->refused_count == 1u ? "" : "s");
        for (size_t i = 0; i < e->refused_count; i++) {
            const proto_prefs_field_t *f = proto_prefs_field(e->refused[2u * i]);
            say(&v->io, "  %s refused: %s", f != NULL ? f->key : "a newer field",
                proto_prefs_why_word(e->refused[2u * i + 1u]));
        }
        break;
    case PLINK_PSET_TIMEOUT:
        say(&v->io, "keypad %u didn't answer the settings change in 6 s", (unsigned)device);
        break;
    default:
        say(&v->io, "keypad %u: the settings change ended, the keypad offline or removed",
            (unsigned)device);
        break;
    }
}

/* Each keypad's settings report, said when a new one came. */
static void say_reports(link_master_t *v, uint64_t now)
{
    if (v->io.say == NULL) {
        return;
    }
    for (uint8_t d = 1; d <= PLINK_KEYPADS_MAX; d++) {
        plink_keypad_home_t kh;
        if (!plink_master_keypad_home(&v->m, now, d, &kh) || !kh.prefs.have ||
            kh.prefs.count == v->said_report[d - 1u]) {
            continue;
        }
        v->said_report[d - 1u] = kh.prefs.count;
        link_master_say_prefs(v, now, d);
    }
}

const hc_device_t *link_master_device(const link_master_t *v, const char *ref)
{
    return hc_get(&v->hc, ref);
}

bool link_master_device_online(link_master_t *v, uint64_t now_ms, const char *ref, bool online)
{
    dev_info_t *d = house_find(&v->house, ref);
    if (d == NULL) {
        return false;
    }
    v->now = now_ms;
    d->online = online;
    d->values.since = online ? 0u : (uint32_t)(m_epoch(v) / 1000);
    v->house.sink->changed(v->house.sink->ctx, (uint8_t)DSRC_HA);
    link_master_sync(v, now_ms);
    return true;
}

void link_master_house(link_master_t *v, uint64_t now_ms, bool connected)
{
    v->now = now_ms;
    v->house.gone = !connected;
    v->house.sink->changed(v->house.sink->ctx, (uint8_t)DSRC_HA);
    link_master_sync(v, now_ms);
}

/* A row's words: "on, level 80 %, pending (unlock)". */
static void row_text(const hc_device_t *r, char *out, size_t cap)
{
    size_t at = 0;
    const dev_values_t *x = &r->info.values;
    add_text(out, cap, &at, "%s", r->info.state[0] != '\0' ? r->info.state : "-");
    if ((x->flags & DEV_VAL_LEVEL) != 0u) {
        add_text(out, cap, &at, ", level %u %%", (unsigned)x->level);
    }
    if ((x->flags & DEV_VAL_SETPOINT) != 0u) {
        add_hundredths(out, cap, &at, ", set to ", x->setpoint);
    }
    if (!r->info.online) {
        add_text(out, cap, &at, ", offline");
    } else if (r->pending != 0u) {
        add_text(out, cap, &at, ", pending (%s)", link_action_name(r->action));
    } else if (r->failed) {
        add_text(out, cap, &at, ", %s (%s)", hc_outcome_unknown(r) ? "not confirmed" : "failed",
                 link_action_name(r->action));
    }
}

void link_master_say_devices(link_master_t *v, uint64_t now_ms)
{
    link_master_sync(v, now_ms);
    say(&v->io, "%u device%s for the keypads (list %08x):", (unsigned)v->dev_count,
        v->dev_count == 1u ? "" : "s", (unsigned)v->dev_list_crc);
    for (size_t i = 0; i < v->dev_count; i++) {
        const hc_device_t *r = &v->rows[i];
        char text[160];
        text[0] = '\0';
        row_text(r, text, sizeof(text));
        say(&v->io, "  %u %s \"%s\" (%s, %s): %s", (unsigned)i, r->info.ref, r->info.name,
            r->info.area[0] != '\0' ? r->info.area : "no area", kind_word(r->info.kind), text);
    }
}

static const char *voice_word(uint8_t voice)
{
    switch (voice) {
    case PLINK_VOICE_READY: return "ready";
    case PLINK_VOICE_LOADING: return "loading";
    case PLINK_VOICE_NONE: return "none";
    default: return "not known";
    }
}

void link_master_say_prefs(link_master_t *v, uint64_t now_ms, uint8_t device)
{
    plink_keypad_home_t kh;
    if (!plink_master_keypad_home(&v->m, now_ms, device, &kh)) {
        say(&v->io, "no keypad %u", (unsigned)device);
        return;
    }
    if (!kh.prefs.have) {
        say(&v->io, "keypad %u: no settings report yet (%s)", (unsigned)device,
            (kh.caps & PLINK_CAP_PREFS) != 0u ? "it shares settings"
                                              : "its session doesn't share settings");
        return;
    }
    proto_prefs_decoded_t d;
    char text[400];
    if (proto_prefs_decode(kh.prefs.fields, kh.prefs.fields_len, PROTO_PREFS_KNOWN, &d)) {
        link_prefs_text(&d.values, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "(not a fields list)");
    }
    unsigned takes = 0;
    for (uint32_t m = kh.prefs.accepts; m != 0u; m &= m - 1u) {
        takes++;
    }
    say(&v->io, "keypad %u settings (panel id %s, voice %s, takes %u field%s): %s",
        (unsigned)device, kh.prefs.panel_id[0] != '\0' ? kh.prefs.panel_id : "none",
        voice_word(kh.prefs.voice), takes, takes == 1u ? "" : "s", text);
}

bool link_master_prefs_set(link_master_t *v, uint64_t now_ms, uint8_t device, uint8_t id,
                           uint32_t value)
{
    v->now = now_ms;
    const proto_prefs_field_t *f = proto_prefs_field(id);
    plink_keypad_home_t kh;
    if (f == NULL || !plink_master_keypad_home(&v->m, now_ms, device, &kh)) {
        say(&v->io, "no keypad %u", (unsigned)device);
        return false;
    }
    /* Only a field both sides know (LINK-051): what the keypad takes is in its report. */
    if (!kh.prefs.have) {
        say(&v->io, "keypad %u hasn't reported its settings yet", (unsigned)device);
        return false;
    }
    if ((kh.prefs.accepts & PROTO_PREFS_BIT(id)) == 0u) {
        say(&v->io, "keypad %u doesn't take %s", (unsigned)device, f->key);
        return false;
    }
    proto_prefs_values_t pv;
    memset(&pv, 0, sizeof(pv));
    if (proto_prefs_put(&pv, id, value) != PROTO_PREFS_WHY_NONE) {
        say(&v->io, "%s: out of range", f->key);
        return false;
    }
    uint8_t body[PROTO_PREFS_ENC_MAX];
    const size_t len = proto_prefs_encode(&pv, PROTO_PREFS_BIT(id), body, sizeof(body));
    uint32_t req = v->pset_req + 1u;
    if (req == 0u) {
        req = 1u; /* 0 is no write */
    }
    uint8_t why = 0;
    if (!plink_master_prefs_set(&v->m, now_ms, device, req, PLINK_ORIGIN_CONSOLE, body, len,
                                &why)) {
        static const char *const t[] = {"sent",
                                        "no such keypad",
                                        "it is offline",
                                        "its session doesn't share settings",
                                        "a change to it is under way",
                                        "",
                                        "not a fields list"};
        say(&v->io, "keypad %u: the settings change wasn't sent: %s", (unsigned)device,
            why < sizeof(t) / sizeof(t[0]) ? t[why] : "refused");
        return false;
    }
    v->pset_req = req;
    say(&v->io, "keypad %u: settings change sent (%s)", (unsigned)device, f->key);
    link_master_sync(v, now_ms);
    return true;
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
        /* The keypads' device actions, then a registry that moved (or the compare build). */
        dev_jobs(v, now_ms);
        if (v->dev_dirty || now_ms >= v->dev_build_at) {
            v->dev_dirty = false;
            dev_build(v, now_ms);
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
        bool more = v->dirty || v->fresh_count > 0u || v->dev_dirty;
        for (size_t i = 0; i < LINK_MASTER_RUNS; i++) {
            more = more || v->runs[i].used;
        }
        for (size_t i = 0; i < LINK_DEV_JOBS; i++) {
            more = more || v->jobs[i].used;
        }
        if (!more) {
            break;
        }
    }
    say_pairing(v, now_ms);
    say_reports(v, now_ms);
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
    if (io->mfw != NULL) {
        v->cfg.partitions[0].exit_delay_s = 45u; /* the shortest: updates' scenarios arm whole */
    }
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
    port.dev_run = m_dev_run;
    port.prefs_done = m_prefs_done;
    /* Updates through the link (link_fw.h): offered with the PC's stand-in for panel_ota. */
    port.fw_read = io->mfw != NULL ? link_mfw_read : NULL;

    /* The registry and its one source (BL-216), before the first build. */
    house_init(&v->house, (uint32_t)(m_epoch(v) / 1000));
    const hc_port_t hp = {.ctx = v, .changed = h_changed, .result = h_result};
    hc_init(&v->hc, &hp);
    (void)hc_register(&v->hc, &HOUSE_OPS, v);
    v->house.sink = hc_sink(&v->hc);
    v->dev_dirty = true;
    if (!plink_random(c, &v->pset_req, sizeof(v->pset_req))) {
        v->pset_req = 0x5E770000u; /* writes' ids start from a random base (LINK-018) */
    }

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
    plink_master_init(&v->m, c, &port, have, name != NULL ? name : "PC master", id,
                      (uint16_t)(LINK_CAPS | (io->mfw != NULL ? PLINK_CAP_FW : 0u)), now_ms);
    const unsigned count = have != NULL ? rec.count : 0u;
    plink_wipe(&rec, sizeof(rec));
    plink_master_net(&v->m, now_ms, true);
    plink_master_time(&v->m, now_ms, m_epoch(v), LINK_TZ);
    if (io->mfw != NULL) {
        link_mfw_boot(v, now_ms);
    }
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
    if (v->io.mfw != NULL && v->io.mfw->down) {
        return; /* restarting: off */
    }
    plink_master_input(&v->m, now_ms, from, frame, len);
    link_master_sync(v, now_ms);
}

void link_master_tick(link_master_t *v, uint64_t now_ms)
{
    v->now = now_ms;
    if (v->io.mfw != NULL && !link_mfw_tick(v, now_ms)) {
        return; /* restarting: off, or it just started again */
    }
    if (alarm_engine_next_deadline(&v->eng) <= now_ms) {
        alarm_engine_tick(&v->eng, now_ms);
    }
    house_tick(v, now_ms);
    hc_tick(&v->hc, now_ms);
    if (plink_master_next(&v->m) <= now_ms) {
        plink_master_tick(&v->m, now_ms);
    }
    link_master_sync(v, now_ms);
}

uint64_t link_master_next(const link_master_t *v)
{
    if (v->io.mfw != NULL && v->io.mfw->down) {
        return link_mfw_next(v);
    }
    uint64_t t = alarm_engine_next_deadline(&v->eng);
    const uint64_t more[3] = {plink_master_next(&v->m), house_next(v), v->dev_build_at};
    for (size_t i = 0; i < 3u; i++) {
        t = more[i] < t ? more[i] : t;
    }
    if (v->io.mfw != NULL) {
        const uint64_t f = link_mfw_next(v);
        t = f < t ? f : t;
    }
    return t;
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
        char name[PLINK_NAME_MAX + 3];
        if (p->name[0] != '\0') {
            snprintf(name, sizeof(name), "\"%s\"", p->name);
        } else {
            snprintf(name, sizeof(name), "\"Keypad %u\"", (unsigned)p->device); /* not renamed */
        }
        char sup[16];
        if (p->supervision_s == 0u) {
            snprintf(sup, sizeof(sup), "off");
        } else {
            snprintf(sup, sizeof(sup), "%u s", (unsigned)p->supervision_s);
        }
        say(&v->io,
            "keypad %u %s (%s): home partition %u, %s%s%s, supervision %s, heard %s at %s, "
            "fw %s, up %u s",
            (unsigned)p->device, name, kind_name(p->kind), (unsigned)p->home,
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
        plink_keypad_master_addr(&v->k, v->now, &v->find, plink_addr_is_broadcast(&v->find));
    }
}

/* The PC keypad's settings as a panel's come from the factory (panel_prefs_defaults), without
 * the announcements' fields: it has no voice. */
static void kp_prefs_defaults(link_keypad_t *v)
{
    static const struct {
        uint8_t id;
        uint32_t value;
    } F[] = {
        {PROTO_PREFS_F_BRIGHTNESS, 80u},
        {PROTO_PREFS_F_SAVER_BRIGHTNESS, 25u},
        {PROTO_PREFS_F_SAVER_AFTER, 60000u},
        {PROTO_PREFS_F_OFF_AFTER, 0u},
        {PROTO_PREFS_F_NIGHT_ON, 0u},
        {PROTO_PREFS_F_NIGHT_START, 22u * 60u},
        {PROTO_PREFS_F_NIGHT_END, 7u * 60u},
        {PROTO_PREFS_F_NIGHT_BRIGHTNESS, 20u},
        {PROTO_PREFS_F_NIGHT_OFF_AFTER, 60000u},
        {PROTO_PREFS_F_CLOCK_MODE, 0u},
        {PROTO_PREFS_F_APPEARANCE, 0u},
        {PROTO_PREFS_F_HIGH_CONTRAST, 0u},
        {PROTO_PREFS_F_VOLUME, 70u},
        {PROTO_PREFS_F_CHIME_VOLUME, 70u},
        {PROTO_PREFS_F_KEY_CLICK, 1u},
    };
    memset(&v->prefs, 0, sizeof(v->prefs));
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        (void)proto_prefs_put(&v->prefs, F[i].id, F[i].value);
    }
    v->accepts = PROTO_PREFS_KNOWN & ~(uint32_t)LINK_PREFS_VOICE;
}

/* Its settings report (LINK-050): the manager sends it at a session's first STATUS, after a change
 * at most once a second, and on PREFS_REQ. */
static void kp_prefs_report(link_keypad_t *v, uint64_t now)
{
    uint8_t body[PROTO_PREFS_ENC_MAX];
    const size_t len = proto_prefs_encode(&v->prefs, v->accepts, body, sizeof(body));
    if (!plink_keypad_prefs_report(&v->k, now, v->panel_id, (uint8_t)PLINK_VOICE_NONE, v->accepts,
                                   body, len)) {
        say(&v->io, "this keypad's settings couldn't be reported");
    }
}

/* Its home partition is in an alarm, entry time or exit time: a change from elsewhere waits
 * (UI-046's HELD). */
static bool kp_held(const link_keypad_t *v, uint64_t now)
{
    const alarm_snapshot_t *s = plink_keypad_snapshot(&v->k);
    const uint8_t p = link_keypad_home(v, now);
    if ((s->partitions & (1u << (p - 1u))) == 0u) {
        return false;
    }
    const alarm_partition_status_t *st = &s->part[p - 1u];
    return st->triggered || st->state == ALARM_STATE_EXIT_DELAY ||
           st->state == ALARM_STATE_ENTRY_DELAY;
}

static const char *origin_word(uint8_t origin)
{
    switch (origin) {
    case PLINK_ORIGIN_HA: return "Home Assistant";
    case PLINK_ORIGIN_CONSOLE: return "the master's console";
    default: return "elsewhere";
    }
}

/* A settings change from the master (PREFS_SET, LINK-051), applied as the screen applies one:
 * each field checked, a field it doesn't take refused, all held during an alarm, entry or exit
 * time; then answered with the values it keeps now. */
static void kp_prefs_take(link_keypad_t *v, uint64_t now)
{
    plink_prefs_change_t c;
    if (!plink_keypad_take_prefs_set(&v->k, &c)) {
        return;
    }
    uint8_t refused[2u * PLINK_PREFS_REFUSED_MAX];
    uint8_t n = 0;
    proto_prefs_decoded_t d;
    if (!proto_prefs_decode(c.fields, c.len, v->accepts, &d)) {
        memset(&d, 0, sizeof(d)); /* (the link's codec lets none through) */
    }
    for (unsigned id = 1; id <= PROTO_PREFS_F_LAST && n < PLINK_PREFS_REFUSED_MAX; id++) {
        if ((d.refused & PROTO_PREFS_BIT(id)) != 0u) {
            refused[2u * n] = (uint8_t)id;
            refused[2u * n + 1u] = d.why[id - 1u];
            n++;
        }
    }
    for (size_t i = 0;
         i < d.unknown_n && i < PROTO_PREFS_UNKNOWN_MAX && n < PLINK_PREFS_REFUSED_MAX; i++) {
        refused[2u * n] = d.unknown[i]; /* a newer master's field */
        refused[2u * n + 1u] = (uint8_t)PROTO_PREFS_WHY_NOT_ALLOWED;
        n++;
    }
    const bool held = kp_held(v, now);
    for (unsigned id = 1; id <= PROTO_PREFS_F_LAST; id++) {
        uint32_t x = 0;
        if (!proto_prefs_get(&d.values, id, &x)) {
            continue;
        }
        if (held) {
            if (n < PLINK_PREFS_REFUSED_MAX) {
                refused[2u * n] = (uint8_t)id;
                refused[2u * n + 1u] = (uint8_t)PROTO_PREFS_WHY_HELD;
                n++;
            }
        } else {
            (void)proto_prefs_put(&v->prefs, id, x);
        }
    }
    if (!held && d.values.have != 0u) {
        v->prefs_applied++;
    }
    char text[200];
    link_prefs_text(&d.values, text, sizeof(text));
    say(&v->io, "settings change from %s: %s%s; %u refused", origin_word(c.origin),
        held ? "held (an alarm, entry or exit time) " : "", text, (unsigned)n);
    uint8_t body[PROTO_PREFS_ENC_MAX];
    const size_t len = proto_prefs_encode(&v->prefs, v->accepts, body, sizeof(body));
    (void)plink_keypad_prefs_answer(&v->k, now, c.req, refused, n, body, len);
}

void link_keypad_init(link_keypad_t *v, const plink_crypto_t *c, const link_io_t *io,
                      link_store_t *store, const char *name, const char *panel_id,
                      const plink_addr_t *master, uint64_t now_ms)
{
    memset(v, 0, sizeof(*v));
    v->c = c;
    v->io = *io;
    v->store = store;
    v->now = now_ms;
    snprintf(v->panel_id, sizeof(v->panel_id), "%s", panel_id != NULL ? panel_id : "1c2d3e");
    kp_prefs_defaults(v);
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
    /* Updates through the link (link_fw.h): the relay's writer, with the PC's stand-in for
     * panel_ota. */
    port.fw_write = io->kfw != NULL ? link_kfw_write : NULL;

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
    const uint16_t caps = (uint16_t)(LINK_CAPS | (io->kfw != NULL ? PLINK_CAP_FW : 0u));
    plink_keypad_init(&v->k, c, &port, have, v->cfg_buf, sizeof(v->cfg_buf), &v->cfg, PLINK_KIND_PC,
                      &cap, caps, name != NULL ? name : "PC keypad",
                      io->kfw != NULL ? io->kfw->running : LINK_FW, now_ms);
    plink_wipe(&rec, sizeof(rec));
    plink_keypad_home_attach(&v->k, &v->mirror); /* before the first tick: HOME is offered */
    if (io->kfw != NULL) {
        link_kfw_boot(v, now_ms); /* its facts, last result, master's version, trial */
    }
    kp_prefs_report(v, now_ms);
    if (master != NULL) {
        plink_keypad_master_addr(&v->k, now_ms, master, plink_addr_is_broadcast(master));
    }
}

void link_keypad_master(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *master)
{
    v->now = now_ms;
    v->have_find = true;
    v->find = *master;
    plink_keypad_master_addr(&v->k, now_ms, master, plink_addr_is_broadcast(master));
}

/* An address, or the broadcast address when there is no mDNS on the PC (255.255.255.255, the one
 * broadcast the PC uses): the keypad's pairing then asks every panel on the subnet and pairs with
 * the first master that answers (LINK-021; a second one answering ends the attempt, as two
 * masters open for pairing must). */
void link_keypad_pair(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *master)
{
    v->now = now_ms;
    plink_keypad_pair_start(&v->k, now_ms, master, plink_addr_is_broadcast(master), 0u);
}

void link_keypad_input(link_keypad_t *v, uint64_t now_ms, const plink_addr_t *from,
                       const uint8_t *frame, size_t len)
{
    v->now = now_ms;
    if (v->io.kfw != NULL && v->io.kfw->down) {
        return; /* restarting: off */
    }
    plink_keypad_input(&v->k, now_ms, from, frame, len);
    if (v->io.kfw != NULL) {
        (void)link_kfw_tick(v, now_ms); /* an order is taken at once */
    }
}

void link_keypad_tick(link_keypad_t *v, uint64_t now_ms)
{
    v->now = now_ms;
    if (v->io.kfw != NULL && !link_kfw_tick(v, now_ms)) {
        return; /* restarting: off, or it just started again */
    }
    if (plink_keypad_next(&v->k) <= now_ms) {
        plink_keypad_tick(&v->k, now_ms);
    }
    kp_prefs_take(v, now_ms);
}

uint64_t link_keypad_next(const link_keypad_t *v)
{
    uint64_t t = plink_keypad_next(&v->k);
    if (v->io.kfw != NULL) {
        const uint64_t f = link_kfw_next(v);
        t = v->io.kfw->down || f < t ? f : t;
    }
    return t;
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

/* A row as this keypad's screen shows it: "unlocked, level 80 %, not confirmed (unlock)". */
static void view_text(const plink_dev_view_t *d, char *out, size_t cap)
{
    size_t at = 0;
    out[0] = '\0';
    if (!d->have_rec) {
        add_text(out, cap, &at, "no record yet, offline");
        return;
    }
    add_text(out, cap, &at, "%s", d->word[0] != '\0' ? d->word : "-");
    if ((d->values.flags & DEV_VAL_LEVEL) != 0u) {
        add_text(out, cap, &at, ", level %u %%", (unsigned)d->values.level);
    }
    if ((d->values.flags & DEV_VAL_SETPOINT) != 0u) {
        add_hundredths(out, cap, &at, ", set to ", d->values.setpoint);
    }
    if (!d->online) {
        add_text(out, cap, &at, ", offline%s",
                 d->since_last_data ? " since the last device data" : "");
    } else if (d->pending) {
        add_text(out, cap, &at, ", pending (%s)", link_action_name(d->action));
    } else if (d->failed) {
        add_text(out, cap, &at, ", %s (%s)", d->unsure ? "not confirmed" : "failed",
                 link_action_name(d->action));
    }
}

static uint32_t view_key(const plink_dev_view_t *d)
{
    char text[160];
    view_text(d, text, sizeof(text));
    uint32_t h = 2166136261u; /* FNV-1a: what the line says changed */
    for (const char *p = text; *p != '\0'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

/* The mirror's news: a list installed, the device data fresh or not (LINK-046), a row's state
 * changed while fresh, and the answers to this keypad's own device actions. */
static void say_devices_news(link_keypad_t *v, uint64_t now)
{
    plink_dev_info_t di;
    plink_keypad_dev_info(&v->k, now, &di);
    bool rebase = false;
    if (di.list_seq != v->said_list_seq) {
        v->said_list_seq = di.list_seq;
        rebase = true;
        if (di.list_crc != 0u) {
            say(&v->io, "devices: %u from the master (list %08x)", (unsigned)di.count,
                (unsigned)di.list_crc);
        }
    }
    if (di.list_crc != 0u && di.fresh != v->said_fresh) {
        v->said_fresh = di.fresh;
        rebase = true;
        say(&v->io, di.fresh ? "devices: device data from the master"
                             : "devices: no device data from the master: every device reads "
                               "offline (LINK-046)");
    }
    if (rebase) {
        v->said_dev_set = 0u;
    }
    if (di.seq != v->said_dev_seq || rebase) {
        v->said_dev_seq = di.seq;
        for (uint8_t i = 0; i < di.count && i < PLINK_DEV_MAX; i++) {
            plink_dev_view_t d;
            if (!plink_keypad_dev_view(&v->k, now, i, &d) || !d.usable || !d.have_rec) {
                continue;
            }
            const uint32_t key = view_key(&d);
            const uint64_t bit = 1ull << i;
            const bool known = (v->said_dev_set & bit) != 0u;
            if (known && v->said_dev[i] == key) {
                continue;
            }
            v->said_dev[i] = key;
            v->said_dev_set |= bit;
            if (known && di.fresh) {
                char text[160];
                view_text(&d, text, sizeof(text));
                say(&v->io, "device \"%s\": %s", d.cat.name, text);
            }
        }
    }
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        link_dev_pending_t *p = &v->acts[i];
        if (p->ticket == 0u) {
            continue;
        }
        plink_dev_answer_t ans;
        const plink_cmd_state_t st = plink_keypad_dev_act_state(&v->k, p->ticket, &ans);
        if (st == PLINK_CMD_PENDING) {
            continue;
        }
        if (st == PLINK_CMD_DONE) {
            if (ans.result == PLINK_DEV_CODE_REFUSED) {
                say(&v->io, "%s: %s (%s)", p->what, link_dev_result_name(ans.result),
                    alarm_result_name((alarm_result_t)ans.why));
            } else {
                say(&v->io, "%s: %s", p->what, link_dev_result_name(ans.result));
            }
        } else if (st == PLINK_CMD_NO_ANSWER) {
            say(&v->io, "%s: no answer from the master in 3 s: not confirmed", p->what);
        }
        /* as the screen stops waiting: a refusal or the give-up shows on the device's row */
        plink_keypad_dev_act_release(&v->k, p->ticket);
        p->ticket = 0u;
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
    say_devices_news(v, now_ms);
}

void link_keypad_say_devices(link_keypad_t *v, uint64_t now_ms)
{
    plink_dev_info_t di;
    plink_keypad_dev_info(&v->k, now_ms, &di);
    if (!di.attached) {
        say(&v->io, "this keypad has no device mirror");
        return;
    }
    if (di.list_crc == 0u) {
        say(&v->io, "no device list from the master yet (%s)",
            di.shared ? "its session shares Smart Home" : "its session doesn't share Smart Home");
        return;
    }
    char heard[40];
    if (di.heard_ms == 0u) {
        snprintf(heard, sizeof(heard), "no device data yet");
    } else {
        snprintf(heard, sizeof(heard), "the last device data %u s ago",
                 (unsigned)((now_ms - di.heard_ms) / 1000u));
    }
    say(&v->io, "%u device%s from the master (list %08x), %s, %s:", (unsigned)di.count,
        di.count == 1u ? "" : "s", (unsigned)di.list_crc, di.fresh ? "fresh" : "NOT fresh", heard);
    for (uint8_t i = 0; i < di.count && i < PLINK_DEV_MAX; i++) {
        plink_dev_view_t d;
        if (!plink_keypad_dev_view(&v->k, now_ms, i, &d)) {
            continue;
        }
        if (!d.usable) {
            say(&v->io, "  %u (a device this firmware can't use)", (unsigned)i);
            continue;
        }
        char text[160];
        view_text(&d, text, sizeof(text));
        say(&v->io, "  %u %s \"%s\" (%s, %s): %s", (unsigned)i, d.cat.ref, d.cat.name,
            d.cat.area[0] != '\0' ? d.cat.area : "no area", kind_word(d.cat.kind), text);
    }
}

uint32_t link_keypad_dev_act(link_keypad_t *v, uint64_t now_ms, const char *ref, uint8_t action,
                             int32_t value, uint8_t mode, const char *code)
{
    v->now = now_ms;
    uint32_t ticket = 0;
    const uint8_t r = plink_keypad_dev_act(&v->k, now_ms, ref, action, value, mode, code, &ticket);
    if (r != PLINK_DEV_OK) {
        say(&v->io, "%s %s: %s (nothing sent)", link_action_name(action), ref,
            link_dev_result_name(r));
        return 0u;
    }
    for (size_t i = 0; i < PLINK_DEV_ACTS_MAX; i++) {
        if (v->acts[i].ticket == 0u) {
            v->acts[i].ticket = ticket;
            snprintf(v->acts[i].what, sizeof(v->acts[i].what), "%s %.*s", link_action_name(action),
                     (int)(sizeof(v->acts[i].what) - 16u), ref);
            break;
        }
    }
    return ticket;
}

void link_keypad_say_prefs(link_keypad_t *v, uint64_t now_ms)
{
    plink_keypad_info_t in;
    plink_keypad_info(&v->k, now_ms, &in);
    char text[400];
    link_prefs_text(&v->prefs, text, sizeof(text));
    say(&v->io, "settings of this keypad (panel id %s, no voice; %s): %s", v->panel_id,
        (in.caps & PLINK_CAP_PREFS) != 0u ? "its master holds them"
                                          : "its session doesn't share them",
        text);
}

bool link_keypad_prefs_set(link_keypad_t *v, uint64_t now_ms, uint8_t id, uint32_t value)
{
    const proto_prefs_field_t *f = proto_prefs_field(id);
    if (f == NULL || (v->accepts & PROTO_PREFS_BIT(id)) == 0u) {
        say(&v->io, "this keypad doesn't have %s", f != NULL ? f->key : "that setting");
        return false;
    }
    if (proto_prefs_put(&v->prefs, id, value) != PROTO_PREFS_WHY_NONE) {
        say(&v->io, "%s: out of range", f->key);
        return false;
    }
    kp_prefs_report(v, now_ms); /* as a save on its screen: the master hears of it */
    say(&v->io, "%s changed here: reported to the master", f->key);
    return true;
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
