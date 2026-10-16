/*
 * plink_console.c - the `plink` console command (the service console only, docs/features/11):
 * the link's state, the role, and pairing and the keypads from a bench without the screens.
 * Requests go to the link task like the UI's (link_request); nothing here prints a key or a user
 * code. The pairing's 6-digit code is printed: it is the one both screens show to be compared.
 *
 *   plink                              the role, the link's state, the keypads or the master
 *   plink role keypad|master confirm   factory reset into that role (everything stored is erased)
 *   master: plink pair [force], allow, reject, stop, list, remove <n>, home <n> <p>,
 *           super <n> <seconds>        (force: without installer mode, development builds only)
 *   keypad: plink find, master <ip>|off, pair [<ip>], yes, no, forget, cmd away|stay|night|off|
 *           chime|panic [<code>] (as the touchscreen sends it; the status also shows what it shows)
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
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"

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
        printf(", worker %u", (unsigned)msvc_stack_free());
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
                "home <n> <p>, super <n> <s>; keypad: find, master <ip>|off, pair [<ip>], yes, "
                "no, forget, cmd away|stay|night|off|chime|panic [<code>]; a master in its setup: "
                "kp pair [<ip>]|yes|no|stop|done|force",
        .hint = "[role|pair|allow|reject|stop|list|remove|home|super|find|master|yes|no|forget|cmd|kp]",
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
