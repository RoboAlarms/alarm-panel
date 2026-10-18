/*
 * plink_console.c - the `plink` console command (the service console only, docs/features/11):
 * the link's state, the role, and pairing and the keypads from a bench without the screens.
 * Requests go to the link task like the UI's (link_request); nothing here prints a key or a user
 * code. The pairing's 6-digit code is printed: it is the one both screens show to be compared.
 *
 *   plink                              the role, the link's state, the keypads or the master
 *   plink role keypad|master confirm   factory reset into that role (everything stored is erased)
 *   master: plink pair [force], allow, reject, stop, list, remove <n>, home <n> <p>,
 *           super <n> <seconds>        (force: without installer mode, development builds only),
 *           dev [<i>]                  Smart Home for keypads: the list and records it serves
 *           prefs <n> [set <key> <v>]  keypad n's settings report (set: development builds only)
 *           fw                         keypad updates: each keypad's row and the planner's view
 *           fw update <n>              ask keypad n again now (a wait, a refusal, a going back
 *                                      forgotten)
 *   keypad: plink find, master <ip>|off, pair [<ip>], yes, no, forget, cmd away|stay|night|off|
 *           chime|panic [<code>] (as the touchscreen sends it; the status also shows what it shows)
 *           dev [<i>]                  Smart Home from its master: the mirror and the screen's rows
 *           fw                         its updates from its master: its run, its master's version
 *   a master whose setup isn't done (the setup's keypad branch, Option C): plink kp pair [<ip>],
 *           kp yes, kp no, kp stop, kp done (stores the keypad role, keeps Wi-Fi, restarts);
 *           kp force first lets a bench panel whose setup is done do it (development builds)
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK && CONFIG_ALARM_SERVICE_CONSOLE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "alarm_mem.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "net_wifi.h"

static const char *const LINK_STATE[] = {"not paired", "connecting", "online",
                                         "no system communication"};
static const char *const PAIR_STATE[] = {
    "idle", "open",  "exchanging keys", "comparing the code", "waiting for the other side",
    "done", "failed"};

static const char *result_word(uint8_t r)
{
    switch (r) {
    case UI_LINK_REQ_OK: return "done";
    case UI_LINK_REQ_NOT_ALLOWED:
        return "not allowed: installer mode with every partition disarmed";
    case UI_LINK_REQ_FULL: return "keypad numbers 1-8 are all in use";
    case UI_LINK_REQ_BUSY: return "busy, try again";
    case UI_LINK_REQ_FAILED: return "failed (see the log)";
    default: return "refused in this role, or a bad argument";
    }
}

static int ask(const link_req_t *req)
{
    link_req_t r_req = *req;
#if CONFIG_ALARM_BUILD_DEV
    /* the service console of a development build is the bench: it may change the keypads
     * without installer mode, as `plink pair force` opens pairing (the console can factory-reset
     * the panel anyway) */
    r_req.bench = true;
#endif
    const uint8_t r = link_request(&r_req);
    printf("%s\n", result_word(r));
    return r == UI_LINK_REQ_OK ? 0 : 1;
}

static bool parse_ip(const char *text, uint32_t *out)
{
    esp_ip4_addr_t a;
    if (text == NULL || esp_netif_str_to_ip4(text, &a) != ESP_OK || a.addr == 0u) {
        return false;
    }
    *out = a.addr;
    return true;
}

static bool parse_num(const char *text, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end = NULL;
    const unsigned long v = text != NULL ? strtoul(text, &end, 10) : 0u;
    if (text == NULL || end == text || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static void print_pair(const ui_link_pair_t *p)
{
    printf("  pairing: %s",
           p->state < sizeof PAIR_STATE / sizeof PAIR_STATE[0] ? PAIR_STATE[p->state] : "?");
    if (p->state == UI_LINK_PAIR_FAILED) {
        printf(" (reason %u)", (unsigned)p->fail);
    }
    if (p->left_s != 0u) {
        printf(", %" PRIu32 " s", p->left_s);
    }
    if (p->peer_name[0] != '\0' || p->peer_addr[0] != '\0') {
        printf(", with \"%s\" at %s", p->peer_name, p->peer_addr);
    }
    printf("\n");
    if (p->code[0] != '\0') {
        printf("  code on both screens: %.3s %s\n", p->code, p->code + 3);
    }
}

/* The console task's copy of what the link task published (one command at a time). */
static link_pub_t s_pub;

static const link_pub_t *published(void)
{
    link_pub_lock();
    s_pub = g_pub;
    link_pub_unlock();
    return &s_pub;
}

/* What a keypad's screen shows now (the snapshot rebuilt from the master's status), for a bench
 * without the touchscreen: each partition and the troubles. */
static alarm_snapshot_t s_view EXT_RAM_BSS_ATTR;

static void print_keypad_view(void)
{
    ksvc_ui_snapshot(&s_view);
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s_view.partitions & (1u << p)) == 0u) {
            continue;
        }
        const alarm_partition_status_t *ps = &s_view.part[p];
        printf("  shows partition %u: %s", p + 1u, alarm_state_name(ps->state));
        if (ps->level != ALARM_LEVEL_NONE) {
            printf(" %s", alarm_level_name(ps->level));
        }
        printf("%s, %u faulted, %u bypassed, %u troubles", ps->ready ? ", ready" : ", not ready",
               (unsigned)ps->faults, (unsigned)ps->bypassed, (unsigned)ps->troubles);
        if (ps->delay_remaining_ms != 0u) {
            /* as the screen counts it down: from when the status arrived (taken_ms, the
             * keypad's own clock, the backend's now_ms) */
            const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
            const uint64_t gone = now > s_view.taken_ms ? now - s_view.taken_ms : 0u;
            const uint64_t left = ps->delay_remaining_ms > gone ? ps->delay_remaining_ms - gone : 0u;
            printf(", %u s left", (unsigned)((left + 999u) / 1000u));
        }
        if (ps->sounder != ALARM_SOUND_NONE) {
            printf(", sounding %s", alarm_sound_name(ps->sounder));
        }
        printf("\n");
    }
    if (s_view.local_lockout_until != 0u) {
        const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
        if (s_view.local_lockout_until > now) {
            printf("  this keypad is locked out for %u s more\n",
                   (unsigned)((s_view.local_lockout_until - now + 999u) / 1000u));
        }
    }
    printf("  system troubles 0x%08" PRIx32 ", keypads missing 0x%08" PRIx32
           ", zones used %u, newest event #%" PRIu32 "\n",
           s_view.sys_troubles, s_view.dev_troubles, (unsigned)s_view.zones_used,
           s_view.last_event_seq);
}

/* `plink cmd ...` on a keypad: a command through the same path as the touchscreen's (the
 * master runs it as this keypad, LINK-002); the code is wiped after. */
static int keypad_cmd(int argc, char **argv)
{
    if (argc < 3) {
        printf("plink cmd away|stay|night|off|chime|panic [<code>]\n");
        return 1;
    }
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = PLINK_OP_COMMAND;
    link_pub_lock();
    c.cmd.partition = g_pub.kinfo.home >= 1u ? g_pub.kinfo.home : 1u;
    link_pub_unlock();
    const char *what = argv[2];
    if (strcmp(what, "away") == 0 || strcmp(what, "stay") == 0 || strcmp(what, "night") == 0) {
        c.cmd.type = ALARM_CMD_ARM;
        c.cmd.level = what[0] == 'a' ? ALARM_LEVEL_AWAY
                      : what[0] == 's' ? ALARM_LEVEL_STAY : ALARM_LEVEL_NIGHT;
    } else if (strcmp(what, "off") == 0) {
        c.cmd.type = ALARM_CMD_DISARM;
    } else if (strcmp(what, "chime") == 0) {
        c.cmd.type = ALARM_CMD_CHIME;
    } else if (strcmp(what, "panic") == 0) {
        c.cmd.type = ALARM_CMD_PANIC;
        c.cmd.panic = ALARM_PANIC_POLICE;
    } else {
        printf("plink cmd away|stay|night|off|chime|panic [<code>]\n");
        return 1;
    }
    if (argc >= 4) {
        snprintf(c.cmd.code, sizeof c.cmd.code, "%s", argv[3]);
    }
    plink_result_t res;
    memset(&res, 0, sizeof res);
    const int64_t t0 = esp_timer_get_time();
    const alarm_result_t r = ksvc_command(&c, &res);
    const int64_t t1 = esp_timer_get_time();
    plink_wipe(&c, sizeof c);
    printf("%s (user %u, not ready zone %u) in %u ms\n", alarm_result_name(r), (unsigned)res.user,
           (unsigned)res.not_ready_zone, (unsigned)((t1 - t0) / 1000));
    return r == ALARM_OK ? 0 : 1;
}

static void print_status(void)
{
    const link_pub_t *pub = published();
    const bool keypad = plink_service_role() == PLINK_SERVICE_KEYPAD;
    printf("Panel link: this panel is %s; the link is %s\n",
           keypad ? "a keypad" : "the alarm panel (master)", pub->running ? "running" : "off");
    if (!pub->running) {
        return;
    }
    printf("  panel %s, UDP %u, stack minimum free: link %u bytes", g_link.panel_id,
           (unsigned)PLINK_UDP_PORT, (unsigned)uxTaskGetStackHighWaterMark(g_link.task));
    if (!keypad) {
        printf(", worker %u, plink_dev %u", (unsigned)msvc_stack_free(),
               (unsigned)hsvc_stack_free());
    }
    printf("\n");
    if (keypad) {
        const plink_keypad_info_t *i = &pub->kraw;
        printf("  %s for %" PRIu32 " s", i->state < 4u ? LINK_STATE[i->state] : "?", i->since_s);
        if (i->status_age_s != UINT32_MAX) {
            printf(", last status %" PRIu32 " s ago", i->status_age_s);
        }
        printf("%s%s\n", i->maintenance ? ", maintenance open" : "",
               i->cfg_bad_crc != 0u ? ", needs an update to read its master" : "");
        if (i->state != PLINK_LINK_NOT_PAIRED) {
            char addr[24];
            plink_addr_text(&i->master, addr);
            printf("  master \"%s\" at %s: keypad %u, home partition %u\n", i->master_name, addr,
                   (unsigned)i->device, (unsigned)i->home);
        } else if (i->unpaired_why == PLINK_UNPAIRED_REMOVED) {
            printf("  removed at the alarm panel: pair again\n");
        }
        print_keypad_view();
    } else {
        printf("  net %04x, %u keypad(s), %u not responding, pairing %s\n", (unsigned)pub->net_id,
               (unsigned)pub->keypads, (unsigned)pub->keypads_down,
               pub->pairing_open ? "open" : "closed");
    }
    if (pub->pair_valid) {
        print_pair(&pub->pair);
    }
}

static int list_keypads(void)
{
    const link_pub_t *pub = published();
    if (pub->keypads == 0u) {
        printf("no keypads\n");
        return 0;
    }
    printf("  n  home  supervision  state            address          firmware  name\n");
    for (size_t i = 0; i < pub->keypads; i++) {
        const ui_link_keypad_t *k = &pub->keypad[i];
        const char *state = k->waiting     ? "never connected"
                            : k->down      ? "NOT RESPONDING"
                            : k->connected ? "connected"
                                           : "not heard";
        /* a keypad nobody named is "Keypad N", as the screen calls it */
        char name[sizeof k->name];
        if (k->name[0] != '\0') {
            snprintf(name, sizeof name, "%s", k->name);
        } else {
            snprintf(name, sizeof name, "Keypad %u", (unsigned)k->device);
        }
        printf("  %u  %4u  %9us  %-15s  %-15s  %-8s  %s\n", (unsigned)k->device, (unsigned)k->home,
               (unsigned)k->supervision_s, state, k->addr, k->fw, name);
    }
    return 0;
}

/* ---- Smart Home and settings on the master's keypads (BL-216) -------------------------------- */

static const char *const KIND_WORD[] = {"-",     "light", "switch", "lock",  "climate",
                                        "cover", "fan",   "valve",  "scene", "script"};
static const char *const SOURCE_WORD[] = {"-", "ha", "zigbee", "zwave"};

static const char *kind_word(uint8_t k)
{
    return k < sizeof KIND_WORD / sizeof KIND_WORD[0] ? KIND_WORD[k] : "?";
}

static const char *source_word(uint8_t s)
{
    return s < sizeof SOURCE_WORD / sizeof SOURCE_WORD[0] ? SOURCE_WORD[s] : "?";
}

/* The console task's copies (one command at a time): the records of the list the master serves
 * now, one keypad's settings side. */
static plink_dev_recs_t s_recs EXT_RAM_BSS_ATTR;
static plink_keypad_home_t s_kh EXT_RAM_BSS_ATTR;

/* `plink dev [<i>]` on a master: whether Smart Home is offered and which keypads have it, then
 * the list and the records the manager serves (plink_dev's newest build): every row in brief, or
 * row i in full. */
static int dev_show(int argc, char **argv)
{
    unsigned long only = 0;
    if (argc >= 3 && !parse_num(argv[2], 0, PLINK_DEV_MAX - 1u, &only)) {
        printf("plink dev [<index 0..%u>]\n", (unsigned)PLINK_DEV_MAX - 1u);
        return 1;
    }
    uint8_t *blob = alarm_mem_slow_malloc(PLINK_DEV_BLOB_MAX);
    if (blob == NULL) {
        printf("not enough PSRAM for a copy of the list\n");
        return 1;
    }
    hsvc_dev_info_t info;
    const bool have = hsvc_dev_copy(blob, PLINK_DEV_BLOB_MAX, &s_recs, &info);
    printf("Smart Home for keypads: %s; %s; %" PRIu32 " build(s) handed to the link\n",
           info.offered ? "offered" : "not offered (no home control, or the registry didn't start)",
           info.wanted ? "a keypad has it" : "no keypad has it now (no builds)", info.builds);
    const link_pub_t *pub = published();
    for (size_t i = 0; i < pub->keypads; i++) {
        printf("  keypad %u: %s, Smart Home %s, settings %s\n", (unsigned)pub->keypad[i].device,
               pub->caps[i] != 0u ? "in session" : "no session",
               (pub->caps[i] & PLINK_CAP_HOME) != 0u ? "yes" : "no",
               (pub->caps[i] & PLINK_CAP_PREFS) != 0u ? "yes" : "no");
    }
    if (!have) {
        printf("  no list built yet\n");
        free(blob);
        return 0;
    }
    const uint64_t now = (uint64_t)alarm_service_now();
    printf("  list %08" PRIx32 ", %u bytes, %u row(s) from %u in the registry, states %08" PRIx32
           ", built %u s ago\n",
           info.list_crc, (unsigned)info.len, (unsigned)s_recs.count, (unsigned)info.rows,
           info.states_crc, (unsigned)((now > info.built_ms ? now - info.built_ms : 0u) / 1000u));
    if (argc < 3) {
        printf("   i  kind     source  flags  state         name (area)  ref\n");
    }
    plink_home_walk_t w;
    memset(&w, 0, sizeof w);
    uint8_t index = 0;
    plink_dev_cat_t cat;
    bool usable = false;
    while (plink_home_list_next(blob, info.len, &w, &index, &cat, &usable)) {
        if (argc >= 3 && index != only) {
            continue;
        }
        plink_dev_rec_t rec;
        const bool hr = plink_home_rec(&s_recs, index, &rec);
        if (!hr) {
            memset(&rec, 0, sizeof rec);
        }
        const char flags[5] = {(rec.flags & PLINK_DEVF_ONLINE) != 0u ? 'o' : '-',
                               (rec.flags & PLINK_DEVF_PENDING) != 0u ? 'p' : '-',
                               (rec.flags & PLINK_DEVF_FAILED) != 0u ? 'f' : '-',
                               (rec.flags & PLINK_DEVF_UNSURE) != 0u ? 'u' : '-', '\0'};
        if (argc < 3) {
            printf("  %2u  %-7s  %-6s  %-5s  %-12s  %s (%s)  %s%s\n", (unsigned)index,
                   kind_word(cat.kind), source_word(cat.source), hr ? flags : "none", rec.word,
                   cat.name, cat.area, cat.ref, usable ? "" : " [can't be used]");
            continue;
        }
        printf("  %u: %s from %s, \"%s\" in \"%s\", ref %s%s\n", (unsigned)index,
               kind_word(cat.kind), source_word(cat.source), cat.name, cat.area, cat.ref,
               usable ? "" : " [can't be used]");
        if (!hr) {
            printf("  no record yet\n");
            continue;
        }
        printf("  flags %s (online, pending, failed, unsure), action %u, state \"%s\"\n", flags,
               (unsigned)rec.action, rec.word);
        printf("  values 0x%02x: level %u, modes 0x%02x, hvac %u, setpoint %" PRId32
               ", temp %" PRId32 ", since %" PRIu32 ", caps 0x%02x\n",
               (unsigned)rec.vflags, (unsigned)rec.level, (unsigned)rec.modes,
               (unsigned)rec.hvac_action, rec.setpoint, rec.temp, rec.since, (unsigned)rec.caps);
    }
    free(blob);
    return 0;
}

/* Keypad device's settings side as the link task last published it, into s_kh. */
static bool keypad_home(uint8_t device, int8_t *rssi)
{
    bool found = false;
    link_pub_lock();
    for (size_t i = 0; g_pub.home != NULL && i < g_pub.keypads; i++) {
        if (g_pub.keypad[i].device == device) {
            s_kh = g_pub.home[i];
            *rssi = g_pub.rssi[i];
            found = s_kh.device == device;
        }
    }
    link_pub_unlock();
    return found;
}

/* A field's value as Home Assistant writes it: a delay in seconds, a switch on or off. */
static void print_field(const proto_prefs_field_t *f, uint32_t v)
{
    if ((f->flags & PROTO_PREFS_FL_SWITCH) != 0u) {
        printf("%s=%s", f->key, v != 0u ? "on" : "off");
    } else if ((f->flags & PROTO_PREFS_FL_DELAY) != 0u) {
        printf("%s=%" PRIu32 "s", f->key, v / 1000u);
    } else {
        printf("%s=%" PRIu32, f->key, v);
    }
}

#if CONFIG_ALARM_BUILD_DEV
static const char *pset_word(plink_service_pset_t r)
{
    switch (r) {
    case PLINK_SERVICE_PSET_OK: return "answered";
    case PLINK_SERVICE_PSET_NOT_FOUND: return "no such keypad";
    case PLINK_SERVICE_PSET_OFFLINE: return "offline (no session, or not heard for 30 s)";
    case PLINK_SERVICE_PSET_UNSUPPORTED: return "unsupported (no settings over its link)";
    case PLINK_SERVICE_PSET_BUSY: return "busy (a write in flight), try again";
    case PLINK_SERVICE_PSET_TIMEOUT: return "sent, never answered (6 s)";
    default: return "not a settings body";
    }
}

/* `plink prefs <n> set`: the console waits for the write's end (the link task's done). */
static SemaphoreHandle_t s_set_done;
static portMUX_TYPE s_set_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_set_ticket;
static plink_service_pset_t s_set_result;
static uint8_t s_set_why[PROTO_PREFS_F_LAST];

static void console_set_done(void *ctx, uint32_t ticket, plink_service_pset_t result,
                             const uint8_t why[PROTO_PREFS_F_LAST])
{
    (void)ctx;
    bool mine = false;
    portENTER_CRITICAL(&s_set_mux);
    if (ticket == s_set_ticket) {
        s_set_result = result;
        memcpy(s_set_why, why, sizeof s_set_why);
        mine = true;
    }
    portEXIT_CRITICAL(&s_set_mux);
    if (mine) {
        xSemaphoreGive(s_set_done);
    }
}

static int prefs_set(uint8_t device, const char *key, const char *text)
{
    const uint8_t id = proto_prefs_id(key);
    const proto_prefs_field_t *f = id != 0u ? proto_prefs_field(id) : NULL;
    if (f == NULL) {
        printf("no such setting: %s\n", key);
        return 1;
    }
    int8_t rssi = 0;
    if (!keypad_home(device, &rssi) || !s_kh.prefs.have) {
        printf("no settings report from keypad %u yet\n", (unsigned)device);
        return 1;
    }
    if ((s_kh.prefs.accepts & PROTO_PREFS_BIT(id)) == 0u) {
        printf("keypad %u doesn't take %s\n", (unsigned)device, key);
        return 1;
    }
    const bool delay = (f->flags & PROTO_PREFS_FL_DELAY) != 0u;
    unsigned long v = 0;
    if ((f->flags & PROTO_PREFS_FL_SWITCH) != 0u &&
        (strcmp(text, "on") == 0 || strcmp(text, "off") == 0)) {
        v = text[1] == 'n' ? 1u : 0u;
    } else if (!parse_num(text, 0, delay ? 86400u : f->max, &v)) {
        printf("%s: a whole number%s, at most %lu\n", key, delay ? " of seconds" : "",
               delay ? 86400ul : (unsigned long)f->max);
        return 1;
    }
    if (delay) {
        v *= 1000u; /* seconds, as Home Assistant writes them, to the wire's ms */
    }
    proto_prefs_values_t vals;
    memset(&vals, 0, sizeof vals);
    if (proto_prefs_put(&vals, id, (uint32_t)v) != PROTO_PREFS_WHY_NONE) {
        printf("%s: out of range\n", key);
        return 1;
    }
    uint8_t body[PLINK_SERVICE_PREFS_BODY_MAX];
    const size_t len = proto_prefs_encode(&vals, vals.have, body, sizeof body);
    if (s_set_done == NULL) {
        s_set_done = xSemaphoreCreateBinaryWithCaps(alarm_mem_slow_caps());
        if (s_set_done == NULL) {
            printf("no memory\n");
            return 1;
        }
    }
    (void)xSemaphoreTake(s_set_done, 0); /* the end of an earlier write given up on */
    portENTER_CRITICAL(&s_set_mux);
    const uint32_t ticket = ++s_set_ticket;
    portEXIT_CRITICAL(&s_set_mux);
    plink_service_pset_t r = plink_service_prefs_set(device, PLINK_SERVICE_ORIGIN_CONSOLE, body,
                                                     len, console_set_done, NULL, ticket);
    if (r == PLINK_SERVICE_PSET_OK) {
        if (xSemaphoreTake(s_set_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
            printf("no end within 8 s\n");
            return 1;
        }
        portENTER_CRITICAL(&s_set_mux);
        r = s_set_result;
        const uint8_t why = s_set_why[id - 1u];
        portEXIT_CRITICAL(&s_set_mux);
        if (r == PLINK_SERVICE_PSET_OK) {
            if (why == 0u) {
                printf("applied\n");
                return 0;
            }
            printf("refused: %s\n", proto_prefs_why_word(why));
            return 1;
        }
    }
    printf("%s\n", pset_word(r));
    return 1;
}
#endif /* CONFIG_ALARM_BUILD_DEV */

/* `plink prefs <n> [set <key> <value>]` on a master: keypad n's settings report as the master
 * keeps it (LINK-050), or a change to one field (development builds: Home Assistant's path, its
 * origin the console's). */
static int prefs_cmd(uint8_t device, int argc, char **argv)
{
    if (argc >= 4) {
#if CONFIG_ALARM_BUILD_DEV
        if (argc == 6 && strcmp(argv[3], "set") == 0) {
            return prefs_set(device, argv[4], argv[5]);
        }
        printf("plink prefs <n> [set <key> <value>] (a delay in seconds, a switch on|off)\n");
#else
        printf("plink prefs <n> set: development builds only\n");
#endif
        return 1;
    }
    int8_t rssi = 0;
    if (!keypad_home(device, &rssi)) {
        printf("no keypad %u\n", (unsigned)device);
        return 1;
    }
    printf("keypad %u: %s, signal %d dBm, Smart Home %s, settings %s\n", (unsigned)device,
           s_kh.online ? "online" : "not heard for 30 s", (int)rssi,
           (s_kh.caps & PLINK_CAP_HOME) != 0u ? "yes" : "no",
           (s_kh.caps & PLINK_CAP_PREFS) != 0u ? "yes" : "no");
    const plink_prefs_report_t *r = &s_kh.prefs;
    if (!r->have) {
        printf("  no settings report yet\n");
        return 0;
    }
    printf("  report %" PRIu32 ": panel id %s%s, voice %u, accepts 0x%05" PRIx32 ", crc %08" PRIx32
           ", %u bytes\n",
           r->count, r->panel_id[0] != '\0' ? r->panel_id : "-",
           s_kh.id_taken ? " (a lower keypad number reports the same)" : "", (unsigned)r->voice,
           r->accepts, r->crc, (unsigned)r->fields_len);
    proto_prefs_decoded_t dec;
    if (!proto_prefs_decode(r->fields, r->fields_len, PROTO_PREFS_KNOWN, &dec)) {
        printf("  its fields don't decode\n");
        return 1;
    }
    printf(" ");
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        uint32_t v = 0;
        const proto_prefs_field_t *f = proto_prefs_field(id);
        if (f != NULL && proto_prefs_get(&dec.values, id, &v)) {
            printf(" ");
            print_field(f, v);
        }
    }
    printf("\n");
    if (dec.unknown_n != 0u) {
        printf("  %u field(s) this firmware doesn't know\n", (unsigned)dec.unknown_n);
    }
    return 0;
}

/* ---- Smart Home on a keypad (BL-216) ------------------------------------------------------- */

static const char *const STATUS_WORD[] = {"idle", "pending", "failed", "offline"};

/* `plink dev [<i>]` on a keypad: its mirror of the alarm panel's devices and what its screen shows
 * of them (the copy ui_backend_t.dev_list reads), every row in brief or row i in full. */
static int kp_dev_show(int argc, char **argv)
{
    unsigned long only = 0;
    if (argc >= 3 && !parse_num(argv[2], 0, PLINK_DEV_MAX - 1u, &only)) {
        printf("plink dev [<row 0..%u>]\n", (unsigned)PLINK_DEV_MAX - 1u);
        return 1;
    }
    ui_dev_t *devs = alarm_mem_slow_malloc(PLINK_DEV_MAX * sizeof *devs);
    if (devs == NULL) {
        printf("not enough PSRAM for a copy of the devices\n");
        return 1;
    }
    ksvc_dev_info_t info;
    const size_t n = ksvc_dev_copy(devs, PLINK_DEV_MAX, &info);
    if (!info.offered) {
        printf("Smart Home: not offered (no home control in this build, or no PSRAM for it)\n");
        free(devs);
        return 0;
    }
    const plink_dev_info_t *m = &info.mirror;
    const uint64_t now = link_now();
    printf("Smart Home from the alarm panel: %s; list %08" PRIx32 " with %u row(s); device data %s",
           m->shared ? "in this session" : "not in this session (no HOME, or no session)",
           m->list_crc, (unsigned)m->count,
           info.fresh ? "fresh" : "stale or the link isn't online (every tile offline)");
    if (m->heard_ms != 0u && now >= m->heard_ms) {
        printf(", last %u s ago", (unsigned)((now - m->heard_ms) / 1000u));
    }
    printf("\n  the screen shows %u device(s), copied %u s ago\n", (unsigned)n,
           (unsigned)(info.copied_ms != 0u && now >= info.copied_ms ? (now - info.copied_ms) / 1000u
                                                                    : 0u));
    if (argc < 3 && n > 0u) {
        printf("   i  kind     status   state  action  name (area)  ref\n");
    }
    for (size_t i = 0; i < n; i++) {
        const ui_dev_t *d = &devs[i];
        if (argc >= 3 && i != only) {
            continue;
        }
        const char *status =
            d->status < sizeof STATUS_WORD / sizeof STATUS_WORD[0] ? STATUS_WORD[d->status] : "?";
        if (argc < 3) {
            printf("  %2u  %-7s  %-7s  %5u  %6u  %s (%s)  %s%s\n", (unsigned)i, kind_word(d->kind),
                   status, (unsigned)d->state, (unsigned)d->action, d->name, d->area, d->ref,
                   d->unsure != 0u ? " [not confirmed]" : "");
            continue;
        }
        printf("  %u: %s from %s, \"%s\" in \"%s\", ref %s\n", (unsigned)i, kind_word(d->kind),
               source_word(d->source), d->name, d->area, d->ref);
        printf("  %s%s, state %u, action %u, level %d, modes 0x%02x, hvac %u, setpoint %" PRId32
               ", temp %" PRId32 ", caps 0x%02x, since %lld ms\n",
               status, d->unsure != 0u ? " (not confirmed)" : "", (unsigned)d->state,
               (unsigned)d->action, (int)d->level, (unsigned)d->modes, (unsigned)d->hvac_action,
               d->setpoint, d->temperature, (unsigned)d->caps, (long long)d->since_ms);
    }
    free(devs);
    return 0;
}

static int find_masters(void)
{
    if (!link_mdns_init()) {
        printf("no network yet\n");
        return 1;
    }
    mdns_result_t *results = NULL;
    if (mdns_query_ptr(LINK_MDNS_SERVICE, LINK_MDNS_PROTO, 2000, 8, &results) != ESP_OK) {
        printf("mDNS query failed\n");
        return 1;
    }
    unsigned n = 0;
    for (const mdns_result_t *r = results; r != NULL; r = r->next) {
        char ip[16] = "?";
        for (const mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip, sizeof ip);
                break;
            }
        }
        printf("  %s:%u", ip, (unsigned)r->port);
        for (size_t i = 0; i < r->txt_count; i++) {
            if (r->txt[i].key != NULL && r->txt[i].value != NULL) {
                printf(" %s=%s", r->txt[i].key, r->txt[i].value);
            }
        }
        printf("\n");
        n++;
    }
    if (results != NULL) {
        mdns_query_results_free(results);
    }
    printf("%u alarm panel(s) announce the panel link\n", n);
    return 0;
}

static int cmd_plink(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "radio") == 0) {
        printf("ESP-NOW bridge %s, channel %u; LAN dropped %u, radio dropped %u\n",
               link_radio_ready() ? "ready" : "unavailable", link_radio_channel(),
               (unsigned)link_test_drops(PLINK_TP_UDP), (unsigned)link_test_drops(PLINK_TP_ESPNOW));
        return 0;
    }
#if CONFIG_ALARM_BUILD_DEV
    if (argc == 3 && strcmp(argv[1], "radio") == 0 && strcmp(argv[2], "window") == 0) {
        net_wifi_radio_window();
        printf("Requested a five-second Wi-Fi disconnect; saved credentials are unchanged\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "drop") == 0) {
        uint8_t tp;
        if (strcmp(argv[2], "lan") == 0)
            tp = PLINK_TP_UDP;
        else if (strcmp(argv[2], "radio") == 0)
            tp = PLINK_TP_ESPNOW;
        else if (strcmp(argv[2], "off") == 0)
            tp = PLINK_TP_NONE;
        else {
            printf("plink drop lan|radio|off\n");
            return 1;
        }
        link_test_drop(tp);
        printf("Bench transport drop %s (expires in 90 seconds; not stored)\n", argv[2]);
        return 0;
    }
#endif
    const bool keypad = plink_service_role() == PLINK_SERVICE_KEYPAD;
    link_req_t req;
    memset(&req, 0, sizeof req);
    if (argc == 1) {
        print_status();
        return 0;
    }
    const char *what = argv[1];
    if (strcmp(what, "role") == 0) {
        if (argc != 4 || strcmp(argv[3], "confirm") != 0 ||
            (strcmp(argv[2], "keypad") != 0 && strcmp(argv[2], "master") != 0)) {
            printf("plink role keypad|master confirm: a factory reset into that role - codes, "
                   "users, zones, Wi-Fi and pairings are erased\n");
            return 1;
        }
        link_reset_into(strcmp(argv[2], "keypad") == 0 ? PLINK_SERVICE_KEYPAD
                                                       : PLINK_SERVICE_MASTER); /* restarts */
        return 0;
    }
    if (!keypad && strcmp(what, "kp") == 0 && argc >= 3) {
        /* the setup's keypad branch from a bench (Option C): as the wizard's requests */
        const char *step = argv[2];
        if (strcmp(step, "pair") == 0) {
            req.kind = UI_LINK_REQ_KP_PAIR_START;
            if (argc >= 4) {
                req.kind = LINK_REQ_KP_PAIR_AT;
                if (!parse_ip(argv[3], &req.ip)) {
                    printf("an IPv4 address, e.g. 192.0.2.10\n");
                    return 1;
                }
            }
        } else if (strcmp(step, "yes") == 0 || strcmp(step, "no") == 0) {
            req.kind = UI_LINK_REQ_KP_PAIR_CONFIRM;
            req.allow = step[0] == 'y';
        } else if (strcmp(step, "stop") == 0) {
            req.kind = UI_LINK_REQ_KP_PAIR_STOP;
        } else if (strcmp(step, "done") == 0) {
            req.kind = UI_LINK_REQ_KP_DONE;
        } else if (strcmp(step, "force") == 0) {
            req.kind = LINK_REQ_KP_FORCE; /* development builds: the setup's pairing anyway */
        } else {
            printf("plink kp pair [<ip>] | yes | no | stop | done\n");
            return 1;
        }
        return ask(&req);
    }
    if (!keypad) {
        unsigned long n = 0;
        unsigned long v = 0;
        if (strcmp(what, "pair") == 0) {
            req.kind = argc >= 3 && strcmp(argv[2], "force") == 0 ? LINK_REQ_PAIR_FORCE
                                                                  : UI_LINK_REQ_PAIR_OPEN;
            return ask(&req);
        }
        if (strcmp(what, "allow") == 0 || strcmp(what, "reject") == 0) {
            req.kind = UI_LINK_REQ_PAIR_DECIDE;
            req.allow = what[0] == 'a';
            return ask(&req);
        }
        if (strcmp(what, "stop") == 0) {
            req.kind = UI_LINK_REQ_PAIR_STOP;
            return ask(&req);
        }
        if (strcmp(what, "list") == 0) {
            return list_keypads();
        }
        if (strcmp(what, "dev") == 0) {
            return dev_show(argc, argv);
        }
        if (strcmp(what, "fw") == 0 && argc == 2) {
            return fsvc_console_show();
        }
        if (strcmp(what, "fw") == 0 && argc == 4 && strcmp(argv[2], "update") == 0 &&
            parse_num(argv[3], 1, 8, &n)) {
            req.kind = LINK_REQ_FW_AGAIN;
            req.device = (uint8_t)n;
            return ask(&req);
        }
        if (strcmp(what, "prefs") == 0 && argc >= 3 && parse_num(argv[2], 1, 8, &n)) {
            return prefs_cmd((uint8_t)n, argc, argv);
        }
        if (strcmp(what, "remove") == 0 && argc == 3 && parse_num(argv[2], 1, 8, &n)) {
            req.kind = UI_LINK_REQ_KEYPAD_REMOVE;
            req.device = (uint8_t)n;
            return ask(&req);
        }
        if ((strcmp(what, "home") == 0 || strcmp(what, "super") == 0) && argc == 4 &&
            parse_num(argv[2], 1, 8, &n)) {
            /* KEYPAD_SET changes the three at once: the others as they are */
            link_pub_lock();
            const ui_link_keypad_t *k = NULL;
            for (size_t i = 0; i < g_pub.keypads; i++) {
                if (g_pub.keypad[i].device == n) {
                    k = &g_pub.keypad[i];
                    req.home = k->home;
                    req.supervision_s = k->supervision_s;
                }
            }
            link_pub_unlock();
            if (k == NULL) {
                printf("no keypad %lu\n", n);
                return 1;
            }
            const bool home = what[0] == 'h';
            if (!parse_num(argv[3], home ? 1u : 0u, home ? ALARM_MAX_PARTITIONS : 3600u, &v)) {
                printf(home ? "a partition 1..%u\n" : "seconds: 0 (not supervised) or 60..3600\n",
                       (unsigned)ALARM_MAX_PARTITIONS);
                return 1;
            }
            req.kind = UI_LINK_REQ_KEYPAD_SET;
            req.device = (uint8_t)n;
            if (home) {
                req.home = (uint8_t)v;
            } else {
                req.supervision_s = (uint16_t)v;
            }
            return ask(&req);
        }
    } else {
        if (strcmp(what, "find") == 0) {
            return find_masters();
        }
        if (strcmp(what, "master") == 0 && argc == 3) {
            req.kind = LINK_REQ_KP_STATIC;
            if (strcmp(argv[2], "off") != 0 && !parse_ip(argv[2], &req.ip)) {
                printf("an IPv4 address, e.g. 192.0.2.10, or off\n");
                return 1;
            }
            return ask(&req);
        }
        if (strcmp(what, "pair") == 0) {
            req.kind = UI_LINK_REQ_KP_PAIR_START;
            if (argc >= 3) {
                req.kind = LINK_REQ_KP_PAIR_AT;
                if (!parse_ip(argv[2], &req.ip)) {
                    printf("an IPv4 address, e.g. 192.0.2.10\n");
                    return 1;
                }
            }
            return ask(&req);
        }
        if (strcmp(what, "yes") == 0 || strcmp(what, "no") == 0) {
            req.kind = UI_LINK_REQ_KP_PAIR_CONFIRM;
            req.allow = what[0] == 'y';
            return ask(&req);
        }
        if (strcmp(what, "forget") == 0) {
            req.kind = LINK_REQ_KP_FORGET;
            return ask(&req);
        }
        if (strcmp(what, "cmd") == 0) {
            return keypad_cmd(argc, argv);
        }
        if (strcmp(what, "dev") == 0) {
            return kp_dev_show(argc, argv);
        }
        if (strcmp(what, "fw") == 0) {
            return kfw_console_show();
        }
    }
    printf("plink: see `help plink`\n");
    return 1;
}

esp_err_t plink_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "plink",
        .help = "Panel link (docs/features/11): status; role keypad|master confirm (a factory "
                "reset); master: pair [force], allow, reject, stop, list, remove <n>, "
                "home <n> <p>, super <n> <s>, dev [<i>], prefs <n> [set <key> <v>], fw (keypad "
                "updates), fw update <n> (ask it again now); keypad: find, "
                "master <ip>|off, pair [<ip>], yes, no, forget, cmd "
                "away|stay|night|off|chime|panic [<code>], dev [<i>], fw; a master in its setup: "
                "kp pair [<ip>]|yes|no|stop|done|force",
        .hint = "[role|pair|allow|reject|stop|list|remove|home|super|dev|prefs|fw|find|master|yes|"
                "no|forget|cmd|kp]",
        .func = cmd_plink,
    };
    return esp_console_cmd_register(&cmd);
}

#elif CONFIG_ALARM_LINK

esp_err_t plink_service_console_register(void)
{
    return ESP_OK;
}

#endif
