/*
 * link_sim.c - the panel link's PC simulator (docs/features/11 LINK-013, docs/features/21 "PC
 * simulator"): a virtual master or a virtual keypad on the real clock over UDP, with a console.
 *
 *   link_sim master [--bind IP] [--port 6056] [--state FILE] [--name NAME] [--id HEX6]
 *   link_sim keypad [--master IP[:PORT]] [--bind IP] [--port 0] [--state FILE] [--name NAME]
 *                   [--id HEX6]
 *
 * The master runs the real alarm engine (link_node.h has its house) and the master's link manager;
 * the keypad runs the keypad's link manager and shows the status it rebuilds. Both take the OS's
 * random numbers, so they pair with panels on the LAN as well as with each other. Both offer Smart
 * Home and settings (BL-216): the master serves its house's devices, the keypad mirrors them and
 * acts on them, and the master changes the keypad's Screen and sound settings.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "link_console.h"
#include "link_node.h"
#include "link_os_crypto.h"
#include "link_udp.h"

#define TICK_MAX_MS 50u /* the longest wait for a datagram: the console is read this often */

static const uint8_t ANY_IP[4] = {0, 0, 0, 0};
static const uint8_t BROADCAST_IP[4] = {255, 255, 255, 255};

typedef struct {
    bool master;
    uint8_t bind_ip[4];
    uint16_t port;
    const char *state;
    const char *name;
    const char *id;
    bool have_master;
    uint8_t master_ip[4];
    uint16_t master_port;
} opts_t;

static link_udp_t U;
static link_os_crypto_t OC;
static plink_crypto_t C;
static link_store_t S;
static link_master_t M;
static link_keypad_t K;
static opts_t O;
static uint64_t T0;
static uint64_t HOLD_UNTIL; /* "wait S": no console line is taken before this */

static uint64_t now_ms(void)
{
    /* Starts at 1000 s, as a panel's engine clock is never near 0. */
    return link_os_mono_ms() - T0 + 1000000u;
}

static void say_line(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
    fflush(stdout);
}

static bool udp_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (to->tp != PLINK_TP_UDP || to->len < 6u) {
        return false;
    }
    const uint16_t port = (uint16_t)(((unsigned)to->a[4] << 8) | (unsigned)to->a[5]);
    if (!link_udp_send(&U, to->a, port, frame, len)) {
        char addr[24];
        plink_addr_text(to, addr);
        printf("send to %s failed: %s\n", addr, U.error);
        return false;
    }
    return true;
}

static void usage(void)
{
    printf("link_sim - the panel link's PC simulator (docs/features/11 LINK-013)\n"
           "\n"
           "  link_sim master [--bind IP] [--port 6056] [--state FILE] [--name NAME] [--id HEX6]\n"
           "      a virtual master: the real alarm engine with a small house and the master's\n"
           "      side of the link; its keypads are kept in FILE (default link_master.plink)\n"
           "  link_sim keypad [--master IP[:PORT]] [--bind IP] [--port 0] [--state FILE]\n"
           "                  [--name NAME] [--id HEX6]\n"
           "      a virtual keypad: the keypad's side of the link and the status it rebuilds;\n"
           "      its master is kept in FILE (default link_keypad.plink); --id is its own panel\n"
           "      id in its settings report (default 1c2d3e)\n"
           "\n"
           "There is no mDNS on the PC: give the master's address with --master, or the keypad\n"
           "broadcasts to 255.255.255.255:6056 to find it. The master answers a keypad on the\n"
           "address of its last authenticated frame. The state files hold the pairing keys.\n"
           "--bind 0.0.0.0 (the default) hears the LAN; Windows may ask to allow it through its\n"
           "firewall. --bind 127.0.0.1 keeps both on this PC.\n"
           "Type help at the console for its commands.\n");
}

static void help(void)
{
    if (O.master) {
        say_line(NULL,
                 "commands:\n"
                 "  pair            open pairing for 2 minutes (installer mode at a panel)\n"
                 "  allow | reject  the codes on both screens match, or don't\n"
                 "  stop            cancel pairing\n"
                 "  list            the keypads\n"
                 "  remove N        remove keypad N (it is told, its keys wiped)\n"
                 "  home N P        keypad N works partition P\n"
                 "  super N S       keypad N's supervision time: 60, 300, 900 s or 0 (off)\n"
                 "  rename N NAME   keypad N's name\n"
                 "  status          partitions, zones, troubles, pairing\n"
                 "  zone N open|close\n"
                 "  arm away|stay|night CODE, disarm CODE   at the master's own console\n"
                 "  devices         the house's devices, as the keypads get them\n"
                 "  device REF online|offline   the house can reach that device, or can't\n"
                 "  house off|on    the house's Home Assistant disconnected (no devices: the\n"
                 "                  keypads get the empty list), or back\n"
                 "  prefs N         keypad N's Screen and sound settings, as it reported them\n"
                 "  prefs N set KEY VALUE   change one on keypad N, as Home Assistant would\n"
                 "                  (KEY: brightness, saver_after, night, night_start, beeps...)\n"
                 "  wait S          take no command for S seconds (for scripts)\n"
                 "  quit\n"
                 "the house: zones 1 Front Door, 2 Living Window, 3 Hall Motion, 4 Hall Smoke;\n"
                 "codes 69420 (installer), 123456 (master), 1111 (Alex); devices light.porch,\n"
                 "light.kitchen, lock.front_door, cover.garage_door, climate.hallway,\n"
                 "scene.good_night");
    } else {
        say_line(NULL,
                 "commands:\n"
                 "  pair [IP[:PORT]] pair with the master there (default: --master, else a\n"
                 "                  broadcast to 255.255.255.255:6056; no mDNS on the PC)\n"
                 "  yes | no        the codes on both screens match, or don't\n"
                 "  cancel          cancel pairing\n"
                 "  master IP[:PORT] where the master is now\n"
                 "  status          the link and the master's status as rebuilt here\n"
                 "  arm away|stay|night CODE, disarm CODE, chime CODE, panic\n"
                 "  devices         the master's devices as mirrored here (its Smart Home)\n"
                 "  act REF ACTION [VALUE] [CODE]   act on one: on [LEVEL], off, toggle, lock,\n"
                 "                  unlock CODE, open CODE, close, stop, level N, temp 21.5,\n"
                 "                  mode off|heat|cool|auto, run (the master judges it)\n"
                 "  prefs           this keypad's Screen and sound settings\n"
                 "  prefs set KEY VALUE   change one here (the master hears of it)\n"
                 "  forget          forget the master (the record erased)\n"
                 "  wait S          take no command for S seconds (for scripts)\n"
                 "  quit");
    }
}

static unsigned split(char *line, char *tok[], unsigned max)
{
    unsigned n = 0;
    char *p = line;
    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        tok[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return n;
}

static bool parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == NULL || *end != '\0' || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static bool code_ok(const char *s)
{
    const size_t n = s != NULL ? strlen(s) : 0u;
    if (n < 4u || n > ALARM_CODE_MAX_LEN) {
        say_line(NULL, "a code is 4 to 10 digits");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            say_line(NULL, "a code is 4 to 10 digits");
            return false;
        }
    }
    return true;
}

static bool parse_level(const char *s, alarm_arm_level_t *out)
{
    if (s == NULL) {
        return false;
    }
    if (strcmp(s, "away") == 0) {
        *out = ALARM_LEVEL_AWAY;
    } else if (strcmp(s, "stay") == 0) {
        *out = ALARM_LEVEL_STAY;
    } else if (strcmp(s, "night") == 0) {
        *out = ALARM_LEVEL_NIGHT;
    } else {
        return false;
    }
    return true;
}

static void wait_cmd(const char *arg)
{
    unsigned long s = 0;
    if (!parse_uint(arg, 3600u, &s)) {
        say_line(NULL, "wait S: seconds, 0..3600");
        return;
    }
    HOLD_UNTIL = now_ms() + (uint64_t)s * 1000u;
}

/* "21.5" -> 2150, "-3" -> -300 (hundredths of a degree); false when it isn't one. */
static bool parse_hundredths(const char *s, int32_t *out)
{
    const bool neg = s != NULL && s[0] == '-';
    const char *p = neg ? s + 1 : s;
    char whole[8];
    const char *dot = p != NULL ? strchr(p, '.') : NULL;
    const size_t wl = dot != NULL ? (size_t)(dot - p) : (p != NULL ? strlen(p) : 0u);
    unsigned long w = 0, f = 0;
    if (p == NULL || wl == 0u || wl >= sizeof(whole)) {
        return false;
    }
    memcpy(whole, p, wl);
    whole[wl] = '\0';
    if (!parse_uint(whole, 999u, &w)) {
        return false;
    }
    if (dot != NULL) {
        const size_t fl = strlen(dot + 1);
        if (fl == 0u || fl > 2u || !parse_uint(dot + 1, 99u, &f)) {
            return false;
        }
        f = fl == 1u ? f * 10u : f;
    }
    const int32_t v = (int32_t)(w * 100u + f);
    *out = neg ? -v : v;
    return true;
}

static bool parse_mode(const char *s, uint8_t *bit)
{
    static const struct {
        const char *word;
        uint8_t bit;
    } MODES[] = {{"off", PLINK_DEV_MODE_OFF},
                 {"heat", PLINK_DEV_MODE_HEAT},
                 {"cool", PLINK_DEV_MODE_COOL},
                 {"auto", PLINK_DEV_MODE_AUTO}};
    for (size_t i = 0; s != NULL && i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        if (strcmp(s, MODES[i].word) == 0) {
            *bit = MODES[i].bit;
            return true;
        }
    }
    return false;
}

/* act REF ACTION [VALUE] [CODE]: a device action from the keypad, as a tap on its Smart Home. */
static void keypad_act(char *t[], unsigned n)
{
    static const char HOW[] = "act REF ACTION [VALUE] [CODE]: on [LEVEL], off, toggle, lock, "
                              "unlock CODE, open CODE, close, stop, level N, temp 21.5, "
                              "mode off|heat|cool|auto, run";
    const uint8_t action = link_action_parse(t[2]);
    int32_t value = action == ACT_ON ? -1 : 0; /* on: at its last level */
    uint8_t mode = 0;
    unsigned at = 3;
    unsigned long x = 0;
    bool ok = action != ACT_NONE;
    if (ok && action == ACT_SET_LEVEL) {
        ok = at < n && parse_uint(t[at++], 100u, &x);
        value = (int32_t)x;
    } else if (ok && action == ACT_SET_TEMP) {
        ok = at < n && parse_hundredths(t[at++], &value);
    } else if (ok && action == ACT_SET_MODE) {
        ok = at < n && parse_mode(t[at++], &mode);
    } else if (ok && action == ACT_ON && at < n && n == 4u) {
        ok = parse_uint(t[at++], 100u, &x); /* a light's brightness: an on needs no code */
        value = (int32_t)x;
    }
    const char *code = at < n ? t[at++] : NULL;
    if (!ok || at < n) {
        say_line(NULL, HOW);
        return;
    }
    if (code != NULL && !code_ok(code)) {
        return;
    }
    (void)link_keypad_dev_act(&K, now_ms(), t[1], action, value, mode, code);
}

/* prefs N set KEY VALUE (master) and prefs set KEY VALUE (keypad): the field and its value. */
static bool prefs_field(const char *key, const char *text, uint8_t *id, uint32_t *value)
{
    const char *why = link_prefs_parse(key, text, id, value);
    if (why != NULL) {
        char msg[400];
        snprintf(msg, sizeof(msg), "%s: %s", key, why);
        say_line(NULL, msg);
        return false;
    }
    return true;
}

/* One console line for the master; false: quit. */
static bool master_line(char *line)
{
    char *t[8];
    const unsigned n = split(line, t, 8u);
    if (n == 0u) {
        return true;
    }
    const uint64_t now = now_ms();
    char msg[160];
    unsigned long a = 0, b = 0;
    if (strcmp(t[0], "quit") == 0 || strcmp(t[0], "exit") == 0) {
        return false;
    } else if (strcmp(t[0], "help") == 0) {
        help();
    } else if (strcmp(t[0], "wait") == 0) {
        wait_cmd(n > 1u ? t[1] : NULL);
    } else if (strcmp(t[0], "pair") == 0) {
        if (!plink_master_pair_open(&M.m, now)) {
            say_line(NULL, "pairing could not open: are all 8 keypad numbers taken?");
        }
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "allow") == 0 || strcmp(t[0], "reject") == 0) {
        plink_master_pair_decide(&M.m, now, strcmp(t[0], "allow") == 0);
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "stop") == 0) {
        plink_master_pair_cancel(&M.m, now);
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "list") == 0) {
        link_master_say_keypads(&M, now);
    } else if (strcmp(t[0], "status") == 0) {
        link_master_say_status(&M, now);
    } else if (strcmp(t[0], "remove") == 0 && n == 2u && parse_uint(t[1], 255u, &a)) {
        const bool ok = plink_master_remove(&M.m, now, (uint8_t)a);
        snprintf(msg, sizeof(msg), ok ? "keypad %lu removed" : "keypad %lu: not removed", a);
        say_line(NULL, msg);
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "home") == 0 && n == 3u && parse_uint(t[1], 255u, &a) &&
               parse_uint(t[2], 255u, &b)) {
        const bool ok = plink_master_set_home(&M.m, now, (uint8_t)a, (uint8_t)b);
        snprintf(msg, sizeof(msg),
                 ok ? "keypad %lu: home partition %lu" : "keypad %lu: partition %lu refused", a,
                 b);
        say_line(NULL, msg);
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "super") == 0 && n == 3u && parse_uint(t[1], 255u, &a) &&
               parse_uint(t[2], 65535u, &b)) {
        const bool ok = plink_master_set_supervision(&M.m, now, (uint8_t)a, (uint16_t)b);
        snprintf(msg, sizeof(msg),
                 ok ? "keypad %lu: supervision %lu s" : "keypad %lu: supervision %lu s refused", a,
                 b);
        say_line(NULL, msg);
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "rename") == 0 && n >= 3u && parse_uint(t[1], 255u, &a)) {
        char name[PLINK_NAME_MAX + 1];
        name[0] = '\0';
        for (unsigned i = 2; i < n; i++) {
            const size_t at = strlen(name);
            snprintf(name + at, sizeof(name) - at, "%s%s", i > 2u ? " " : "", t[i]);
        }
        const bool ok = plink_master_rename(&M.m, (uint8_t)a, name);
        say_line(NULL, ok ? "renamed" : "not renamed");
        link_master_sync(&M, now);
    } else if (strcmp(t[0], "devices") == 0) {
        link_master_say_devices(&M, now);
    } else if (strcmp(t[0], "device") == 0 && n == 3u &&
               (strcmp(t[2], "online") == 0 || strcmp(t[2], "offline") == 0)) {
        if (!link_master_device_online(&M, now, t[1], strcmp(t[2], "online") == 0)) {
            say_line(NULL, "no such device (type devices)");
        }
    } else if (strcmp(t[0], "house") == 0 && n == 2u &&
               (strcmp(t[1], "on") == 0 || strcmp(t[1], "off") == 0)) {
        link_master_house(&M, now, strcmp(t[1], "on") == 0);
    } else if (strcmp(t[0], "prefs") == 0 && n == 2u && parse_uint(t[1], 255u, &a)) {
        link_master_say_prefs(&M, now, (uint8_t)a);
    } else if (strcmp(t[0], "prefs") == 0 && n == 5u && parse_uint(t[1], 255u, &a) &&
               strcmp(t[2], "set") == 0) {
        uint8_t id = 0;
        uint32_t value = 0;
        if (prefs_field(t[3], t[4], &id, &value)) {
            (void)link_master_prefs_set(&M, now, (uint8_t)a, id, value);
        }
    } else if (strcmp(t[0], "zone") == 0 && n == 3u && parse_uint(t[1], ALARM_MAX_ZONES, &a) &&
               a > 0u && (strcmp(t[2], "open") == 0 || strcmp(t[2], "close") == 0)) {
        link_master_zone(&M, now, (uint16_t)a, strcmp(t[2], "open") == 0);
    } else if (strcmp(t[0], "arm") == 0 && n == 3u) {
        alarm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        if (!parse_level(t[1], &cmd.level)) {
            say_line(NULL, "arm away|stay|night CODE");
        } else if (code_ok(t[2])) {
            cmd.type = ALARM_CMD_ARM;
            cmd.partition = 1u;
            snprintf(cmd.code, sizeof(cmd.code), "%s", t[2]);
            const alarm_result_t r = link_master_command(&M, now, &cmd);
            snprintf(msg, sizeof(msg), "arm %s: %s", t[1], alarm_result_name(r));
            say_line(NULL, msg);
        }
        memset(&cmd, 0, sizeof(cmd));
    } else if (strcmp(t[0], "disarm") == 0 && n == 2u) {
        if (code_ok(t[1])) {
            alarm_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = ALARM_CMD_DISARM;
            cmd.partition = 1u;
            snprintf(cmd.code, sizeof(cmd.code), "%s", t[1]);
            const alarm_result_t r = link_master_command(&M, now, &cmd);
            memset(&cmd, 0, sizeof(cmd));
            snprintf(msg, sizeof(msg), "disarm: %s", alarm_result_name(r));
            say_line(NULL, msg);
        }
    } else {
        say_line(NULL, "not a command (type help)");
    }
    return true;
}

static void keypad_cmd(alarm_cmd_type_t type, alarm_arm_level_t level, const char *code,
                       const char *what)
{
    const uint64_t now = now_ms();
    plink_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = PLINK_OP_COMMAND;
    cmd.cmd.type = type;
    cmd.cmd.partition = link_keypad_home(&K, now);
    cmd.cmd.level = level;
    cmd.cmd.panic = ALARM_PANIC_POLICE;
    if (code != NULL) {
        snprintf(cmd.cmd.code, sizeof(cmd.cmd.code), "%s", code);
    }
    (void)link_keypad_command(&K, now, &cmd, what); /* wipes cmd */
}

/* One console line for the keypad; false: quit. */
static bool keypad_line(char *line)
{
    char *t[8];
    const unsigned n = split(line, t, 8u);
    if (n == 0u) {
        return true;
    }
    const uint64_t now = now_ms();
    if (strcmp(t[0], "quit") == 0 || strcmp(t[0], "exit") == 0) {
        return false;
    } else if (strcmp(t[0], "help") == 0) {
        help();
    } else if (strcmp(t[0], "wait") == 0) {
        wait_cmd(n > 1u ? t[1] : NULL);
    } else if (strcmp(t[0], "pair") == 0) {
        uint8_t ip[4];
        uint16_t port = (uint16_t)PLINK_UDP_PORT;
        if (n > 1u) {
            if (!link_udp_parse_endpoint(t[1], (uint16_t)PLINK_UDP_PORT, ip, &port)) {
                say_line(NULL, "pair [IP[:PORT]]");
                return true;
            }
        } else if (O.have_master) {
            memcpy(ip, O.master_ip, 4);
            port = O.master_port;
        } else {
            memcpy(ip, BROADCAST_IP, 4);
            say_line(NULL, "no address given: asking by broadcast to 255.255.255.255:6056");
        }
        const plink_addr_t addr = plink_addr_udp(ip, port);
        link_keypad_pair(&K, now, &addr);
    } else if (strcmp(t[0], "yes") == 0 || strcmp(t[0], "no") == 0) {
        plink_keypad_pair_confirm(&K.k, now, strcmp(t[0], "yes") == 0);
    } else if (strcmp(t[0], "cancel") == 0) {
        plink_keypad_pair_cancel(&K.k, now);
    } else if (strcmp(t[0], "master") == 0 && n == 2u) {
        uint8_t ip[4];
        uint16_t port = 0;
        if (!link_udp_parse_endpoint(t[1], (uint16_t)PLINK_UDP_PORT, ip, &port)) {
            say_line(NULL, "master IP[:PORT]");
            return true;
        }
        const plink_addr_t addr = plink_addr_udp(ip, port);
        link_keypad_master(&K, now, &addr);
    } else if (strcmp(t[0], "status") == 0) {
        link_keypad_say_status(&K, now);
    } else if (strcmp(t[0], "arm") == 0 && n == 3u) {
        alarm_arm_level_t level = ALARM_LEVEL_NONE;
        if (!parse_level(t[1], &level)) {
            say_line(NULL, "arm away|stay|night CODE");
        } else if (code_ok(t[2])) {
            char what[24];
            snprintf(what, sizeof(what), "arm %s", t[1]);
            keypad_cmd(ALARM_CMD_ARM, level, t[2], what);
        }
    } else if (strcmp(t[0], "disarm") == 0 && n == 2u) {
        if (code_ok(t[1])) {
            keypad_cmd(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, t[1], "disarm");
        }
    } else if (strcmp(t[0], "chime") == 0 && n == 2u) {
        if (code_ok(t[1])) {
            keypad_cmd(ALARM_CMD_CHIME, ALARM_LEVEL_NONE, t[1], "chime");
        }
    } else if (strcmp(t[0], "panic") == 0) {
        keypad_cmd(ALARM_CMD_PANIC, ALARM_LEVEL_NONE, NULL, "panic");
    } else if (strcmp(t[0], "devices") == 0) {
        link_keypad_say_devices(&K, now);
    } else if (strcmp(t[0], "act") == 0 && n >= 3u) {
        keypad_act(t, n);
    } else if (strcmp(t[0], "prefs") == 0 && n == 1u) {
        link_keypad_say_prefs(&K, now);
    } else if (strcmp(t[0], "prefs") == 0 && n == 4u && strcmp(t[1], "set") == 0) {
        uint8_t id = 0;
        uint32_t value = 0;
        if (prefs_field(t[2], t[3], &id, &value)) {
            (void)link_keypad_prefs_set(&K, now, id, value);
        }
    } else if (strcmp(t[0], "forget") == 0) {
        say_line(NULL, plink_keypad_forget(&K.k, now) ? "the master is forgotten"
                                                      : "the record could not be erased");
    } else {
        say_line(NULL, "not a command (type help)");
    }
    return true;
}

static int parse_args(int argc, char **argv)
{
    memset(&O, 0, sizeof(O));
    if (argc < 2) {
        return 1;
    }
    if (strcmp(argv[1], "master") == 0) {
        O.master = true;
        O.port = (uint16_t)PLINK_UDP_PORT;
        O.state = "link_master.plink";
    } else if (strcmp(argv[1], "keypad") == 0) {
        O.port = 0u;
        O.state = "link_keypad.plink";
    } else {
        return 1;
    }
    memcpy(O.bind_ip, ANY_IP, 4);
    for (int i = 2; i < argc; i++) {
        const char *opt = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        unsigned long v = 0;
        if (val == NULL) {
            fprintf(stderr, "%s needs a value\n", opt);
            return 1;
        }
        if (strcmp(opt, "--bind") == 0) {
            if (!link_udp_parse_ip(val, O.bind_ip)) {
                fprintf(stderr, "--bind %s: not an IPv4 address\n", val);
                return 1;
            }
        } else if (strcmp(opt, "--port") == 0) {
            if (!parse_uint(val, 65535u, &v)) {
                fprintf(stderr, "--port %s: 0..65535\n", val);
                return 1;
            }
            O.port = (uint16_t)v;
        } else if (strcmp(opt, "--state") == 0) {
            O.state = val;
        } else if (strcmp(opt, "--name") == 0) {
            O.name = val;
        } else if (strcmp(opt, "--id") == 0) {
            if (strlen(val) != PLINK_PANEL_ID_LEN) {
                fprintf(stderr, "--id: six hex characters\n");
                return 1;
            }
            O.id = val;
        } else if (strcmp(opt, "--master") == 0 && !O.master) {
            if (!link_udp_parse_endpoint(val, (uint16_t)PLINK_UDP_PORT, O.master_ip,
                                         &O.master_port)) {
                fprintf(stderr, "--master %s: IP or IP:PORT\n", val);
                return 1;
            }
            O.have_master = true;
        } else {
            fprintf(stderr, "unknown option %s\n", opt);
            return 1;
        }
        i++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "help") == 0)) {
        usage();
        return 0;
    }
    if (parse_args(argc, argv) != 0) {
        usage();
        return 2;
    }
    if (!link_udp_startup()) {
        fprintf(stderr, "no sockets on this PC (WSAStartup failed)\n");
        return 1;
    }
    if (!link_os_crypto_init(&OC)) {
        fprintf(stderr, "the OS gave no random numbers\n");
        return 1;
    }
    C = link_os_crypto(&OC);
    if (!plink_crypto_check(&C)) {
        fprintf(stderr, "the crypto self-check failed (plink_crypto_check)\n");
        return 1;
    }
    if (!link_udp_open(&U, O.bind_ip, O.port, !O.master, C.insecure)) {
        fprintf(stderr, "could not open UDP %u.%u.%u.%u:%u: %s\n", O.bind_ip[0], O.bind_ip[1],
                O.bind_ip[2], O.bind_ip[3], (unsigned)O.port, U.error);
        return 1;
    }
    if (!link_store_init(&S, O.state)) {
        fprintf(stderr, "%s could not be read\n", O.state);
        return 1;
    }
    T0 = link_os_mono_ms();
    const link_io_t io = {
        .send = udp_send, .send_ctx = NULL, .say = say_line, .say_ctx = NULL,
        .epoch_ms = link_os_epoch_ms};
    printf("%s on UDP %u.%u.%u.%u:%u, records in %s\n", O.master ? "master" : "keypad",
           U.ip[0], U.ip[1], U.ip[2], U.ip[3], (unsigned)U.port, O.state);
    if (O.master) {
        if (!link_master_init(&M, &C, &io, &S, O.name, O.id, now_ms())) {
            return 1;
        }
    } else {
        plink_addr_t master = plink_addr_udp(O.master_ip, O.master_port);
        if (!O.have_master) {
            master = plink_addr_udp(BROADCAST_IP, (uint16_t)PLINK_UDP_PORT);
            if (S.len > 0u) {
                printf("no --master: looking for the master by broadcast to "
                       "255.255.255.255:6056\n");
            }
        }
        link_keypad_init(&K, &C, &io, &S, O.name, O.id, &master, now_ms());
        link_keypad_report(&K, now_ms());
    }
    printf("type help for the commands\n");
    fflush(stdout);
    if (!link_console_start()) {
        fprintf(stderr, "the console reader could not start\n");
        return 1;
    }

    uint8_t buf[PLINK_FRAME_MAX];
    bool running = true;
    while (running) {
        uint64_t now = now_ms();
        if (O.master) {
            link_master_tick(&M, now);
        } else {
            link_keypad_tick(&K, now);
            link_keypad_report(&K, now);
        }
        const uint64_t next = O.master ? link_master_next(&M) : link_keypad_next(&K);
        now = now_ms();
        uint32_t wait = TICK_MAX_MS;
        if (next <= now) {
            wait = 0u;
        } else if (next - now < wait) {
            wait = (uint32_t)(next - now);
        }
        uint8_t ip[4];
        uint16_t port = 0;
        const int got = link_udp_recv(&U, buf, sizeof(buf), ip, &port, wait);
        if (got > 0) {
            const plink_addr_t from = plink_addr_udp(ip, port);
            if (O.master) {
                link_master_input(&M, now_ms(), &from, buf, (size_t)got);
            } else {
                link_keypad_input(&K, now_ms(), &from, buf, (size_t)got);
            }
        } else if (got < 0) {
            printf("receive failed: %s\n", U.error);
            link_os_sleep_ms(TICK_MAX_MS);
        }
        char line[LINK_CONSOLE_LINE];
        while (now_ms() >= HOLD_UNTIL && link_console_line(line, sizeof(line))) {
            const bool go_on = O.master ? master_line(line) : keypad_line(line);
            memset(line, 0, sizeof(line)); /* a typed code doesn't stay behind */
            if (!go_on) {
                running = false;
                break;
            }
            if (!O.master) {
                link_keypad_report(&K, now_ms());
            }
        }
    }
    printf("bye\n");
    link_udp_close(&U);
    link_udp_cleanup();
    return 0;
}
