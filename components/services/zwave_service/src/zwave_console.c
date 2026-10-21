/*
 * zwave_console.c - the `zwave` console command (docs/features/36 README "Console"; ZW-020).
 *
 *   zwave [status]                  the radio, the crypto self-test, the module, the operation
 *                                   running, the counters
 *   zwave place [off|socket|j7]     where the ZAC93 is wired, from the next restart
 *   zwave place custom tx=<g> rx=<g> [reset=<g>]   ... on pins of the person's own (ZW-046)
 *   zwave reset-pin [<gpio>|off]    its reset line (hardware version 2), from the next restart
 *   zwave restart                   restart the radio (the start-up again, its soft reset)
 *   zwave reset                     pulse its reset line: it restarts
 *   zwave add | remove | stop       listen for a device to add or remove; stop listening
 *   zwave allow [access auth unauth] the security question: what to grant (default: what it asks)
 *   zwave pin <5 digits>            the PIN on its label
 *   zwave nodes | node <n>          the devices; one device's facts, counters and security
 *   zwave ping <n>                  a listening device: does it answer?
 *   zwave interview <n>             ask it everything again
 *   zwave checkin <n> 1h|4h|12h     a sleeping device's check-in, from its next wake-up
 *   zwave region [<name>]           the radio's region (installer: it restarts the radio)
 *   zwave jam [<dBm>]               the background noise, jammed or not; the threshold
 *   zwave trace on|off              frames in hex on the log (development builds)
 *   zwave siren                     the sirens the engine's bell drives (OUT-016..OUT-019)
 *   zwave siren <n> [<ep>] parts <mask> [also <mask>] [burglary|fire|both] [squawk|nosquawk]
 *         [tone <burglary> <fire> <co> <arm> <disarm>] [vol <0-100>] [super <min>]
 *                                   assign or change one (Sound Switch if it has it, else Binary
 *                                   Switch); masks bit 0 = partition 1; tone 255 its default
 *   zwave siren <n> [<ep>] clear | test
 *   zwave reset-network confirm     every device loses the panel: new keys, a new network
 *
 * Every command that changes the network refuses while a partition is armed, like `zone <n>
 * bind`. The keys never appear here.
 */
#include "zwave_internal.h"

#if CONFIG_ALARM_ZSRC_ZWAVE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_service.h"
#include "board_info/board_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zone_service.h"

/* serial-api.md §6.7's regions, as the console names them and the pages say them. */
static const struct {
    uint8_t code;
    const char *id;
    const char *words;
} REGIONS[] = {
    {0x00u, "eu", "Europe"},
    {0x01u, "us", "United States"},
    {0x02u, "anz", "Australia and New Zealand"},
    {0x03u, "hk", "Hong Kong"},
    {0x05u, "in", "India"},
    {0x06u, "il", "Israel"},
    {0x07u, "ru", "Russia"},
    {0x08u, "cn", "China"},
    {0x09u, "us-lr", "United States with Long Range"},
    {0x0Bu, "eu-lr", "Europe with Long Range"},
    {0x20u, "jp", "Japan"},
    {0x21u, "kr", "Korea"},
};

static const char *region_words(uint8_t code)
{
    for (size_t i = 0; i < sizeof REGIONS / sizeof REGIONS[0]; i++) {
        if (REGIONS[i].code == code) {
            return REGIONS[i].words;
        }
    }
    return "unknown";
}

static const char *region_id(uint8_t code)
{
    for (size_t i = 0; i < sizeof REGIONS / sizeof REGIONS[0]; i++) {
        if (REGIONS[i].code == code) {
            return REGIONS[i].id;
        }
    }
    return "?";
}

static const char *radio_words(uint8_t radio)
{
    switch (radio) {
    case UI_ZW_RADIO_NOT_CONNECTED: return "not connected (no place chosen)";
    case UI_ZW_RADIO_STARTING: return "starting";
    case UI_ZW_RADIO_WORKING: return "working";
    case UI_ZW_RADIO_NOT_ANSWERING: return "not answering: check its wires; the panel keeps trying";
    case UI_ZW_RADIO_UNUSABLE: return "can't be used: not a Z-Wave controller the panel supports";
    case UI_ZW_RADIO_FOREIGN: return "belongs to another system: reset it to use it here";
    case UI_ZW_RADIO_REPLACED: return "replaced or reset: add its devices again";
    case UI_ZW_RADIO_UPDATING: return "updating its own firmware (`zwave radio-update`)";
    default: return "?";
    }
}

static const char *down_words(zw_down_reason_t r)
{
    switch (r) {
    case ZW_DOWN_NONE: return "-";
    case ZW_DOWN_NOT_ANSWERING: return "silent through the soft reset and the reset line";
    case ZW_DOWN_BAD_ANSWER: return "answers that could not be used";
    case ZW_DOWN_NOT_CONTROLLER: return "an end node, not a controller";
    case ZW_DOWN_WRONG_CHIP: return "not an 800-series chip";
    case ZW_DOWN_MISSING_FUNCTION: return "a required Serial API function is missing";
    case ZW_DOWN_WRONG_LIBRARY: return "not a static or bridge controller";
    case ZW_DOWN_BOOTLOADER: return "stuck in its bootloader";
    case ZW_DOWN_RESTARTING: return "restarting again and again";
    default: return "?";
    }
}

static const char *security_words(uint8_t s)
{
    switch (s) {
    case UI_ZW_SEC_NONE: return "not encrypted";
    case UI_ZW_SEC_S0: return "S0";
    case UI_ZW_SEC_S2_UNAUTH: return "S2 unauthenticated";
    case UI_ZW_SEC_S2_AUTH: return "S2 authenticated";
    case UI_ZW_SEC_S2_ACCESS: return "S2 access control";
    case UI_ZW_SEC_UNCONFIRMED: return "not confirmed: no key, its setup never read its classes";
    case UI_ZW_SEC_NONE_CAPABLE: return "not encrypted (it lists S2: its key exchange failed)";
    default: return "not known yet";
    }
}

static const char *state_words(uint8_t s)
{
    switch (s) {
    case UI_ZW_DEV_READY: return "ready";
    case UI_ZW_DEV_SETTING_UP: return "setting up";
    case UI_ZW_DEV_WAKE_NEEDED: return "wake it to finish";
    case UI_ZW_DEV_NO_REPORTS: return "reports nothing (no lifeline)";
    case UI_ZW_DEV_NOT_USABLE: return "another controller: remove it";
    case UI_ZW_DEV_SETUP_STOPPED: return "setup stopped (no answer): `zwave setup <n>`";
    default: return "?";
    }
}

static const char *op_words(uint8_t op)
{
    static const char *const W[] = {"none", "add", "remove", "remove failed", "replace", "ping",
                                    "identify", "routes", "check-in", "region", "reset",
                                    "restart", "software update", "radio update"};
    return op < sizeof W / sizeof W[0] ? W[op] : "?";
}

static const char *step_words(uint8_t step)
{
    static const char *const W[] = {"idle", "starting", "listening: put the device in pairing mode",
                                    "connecting", "asks for security: `zwave allow`",
                                    "asks for its PIN: `zwave pin <5 digits>`", "securing",
                                    "reading what it can do", "working", "done"};
    return step < sizeof W / sizeof W[0] ? W[step] : "?";
}

static const char *result_words(uint8_t r)
{
    static const char *const W[] = {"-", "added", "added without encryption", "downgraded",
                                    "added, but it reports nothing", "nothing joined",
                                    "did not finish joining", "already added",
                                    "a controller: not added", "stopped", "removed",
                                    "removed a device of another system", "nothing removed",
                                    "it still answers: remove it the normal way", "answered",
                                    "no answer", "done", "failed",
                                    "joined, setup not finished: it goes on",
                                    "did not finish in time: it may still be listed",
                                    "refused: the radio does not count it as failed, or said "
                                    "not removed - nothing removed",
                                    "it took its new software", "it did not take it",
                                    "the radio restarted: try again"};
    return r < sizeof W / sizeof W[0] ? W[r] : "?";
}

static const char *req_words(ui_zw_req_result_t r)
{
    switch (r) {
    case UI_ZW_REQ_OK: return "ok";
    case UI_ZW_REQ_NOT_ALLOWED: return "not allowed now";
    case UI_ZW_REQ_BUSY: return "busy: the panel is reading another device or running another operation";
    case UI_ZW_REQ_NO_RADIO: return "the radio is not working";
    case UI_ZW_REQ_INVALID: return "not now, not that device, or a value out of range";
    case UI_ZW_REQ_UNSUPPORTED: return "not in this firmware";
    case UI_ZW_REQ_LOW_BATTERY: return "its battery is too low for an update";
    case UI_ZW_REQ_FULL: return "no room for another device: the panel's storage is full";
    default: return "?";
    }
}

static void keys_words(uint8_t keys, char *out, size_t len)
{
    snprintf(out, len, "%s%s%s%s", (keys & ZW_S2_KEY_ACCESS) != 0u ? "access " : "",
             (keys & ZW_S2_KEY_AUTH) != 0u ? "auth " : "",
             (keys & ZW_S2_KEY_UNAUTH) != 0u ? "unauth " : "",
             (keys & ZW_S2_KEY_S0) != 0u ? "s0 " : "");
}

static bool parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0u || v > 0xFFFFu) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static const char *crypto_words(zwsvc_crypto_t c)
{
    switch (c) {
    case ZWSVC_CRYPTO_PASSED: return "passed";
    case ZWSVC_CRYPTO_FAILED:
        return "FAILED (AES-128, X25519 or the random source): the radio stays off";
    default: return "not run (no place chosen)";
    }
}

static bool running(void)
{
    if (!zwsvc_running()) {
        if (zwsvc_crypto_check() == ZWSVC_CRYPTO_FAILED) {
            printf("the Z-Wave radio is off: its crypto self-test failed (`zwave status`)\n");
        } else if (zwsvc_config()->place != NULL) {
            printf("the Z-Wave radio is off: it did not start (`zwave status`, the boot log)\n");
        } else {
            printf("the Z-Wave radio is off: no place chosen (`zwave place socket|j7`, then "
                   "`restart`)\n");
        }
        return false;
    }
    return true;
}

static bool disarmed(void)
{
    if (!zone_service_all_disarmed()) {
        printf("ERROR armed: every partition must be disarmed\n");
        return false;
    }
    return true;
}

static int ask(uint8_t kind, uint16_t node, const ui_zw_request_t *extra)
{
    zwsvc_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = ZWSVC_REQ_UI;
    r.allowed = zone_service_all_disarmed();
    if (extra != NULL) {
        r.ui = *extra;
    }
    r.ui.kind = kind;
    r.ui.node = node;
    if (node != 0u) {
        zone_service_lock();
        r.ui.generation = zwz_panel_generation(zwsvc_panel(), node);
        zone_service_unlock();
    }
    const ui_zw_req_result_t res = zwsvc_submit(&r);
    printf("%s\n", req_words(res));
    return res == UI_ZW_REQ_OK ? 0 : 1;
}

/* ---- status ---------------------------------------------------------------------------------- */

static void print_progress(const ui_zw_progress_t *g)
{
    if (g->op == UI_ZW_OP_NONE) {
        printf("operation: none\n");
        return;
    }
    printf("operation: %s - %s", op_words(g->op), step_words(g->step));
    if (g->step == UI_ZW_STEP_DONE) {
        printf(" (%s)", result_words(g->result));
    }
    if (g->seconds_left > 0u) {
        printf(", %" PRIu32 " s left", g->seconds_left);
    }
    printf("\n");
    if (g->step == UI_ZW_STEP_ASK_ALLOW) {
        char words[32];
        keys_words(g->requested, words, sizeof words);
        printf("  it asks for: %s%s\n", words, g->csa ? "(and trust without its PIN: CSA)" : "");
    }
    if (g->step == UI_ZW_STEP_ASK_PIN) {
        printf("  its key: %s - the first 5 digits are the PIN on its label\n", g->dsk);
    }
    if (g->node != 0u) {
        printf("  device %u (generation %u) %s, %s\n", (unsigned)g->node, (unsigned)g->generation,
               g->model[0] != '\0' ? g->model : "", security_words(g->security));
    }
    if (g->step == UI_ZW_STEP_READING) {
        printf("  interview step %u of %u%s\n", (unsigned)g->setup_done, (unsigned)g->setup_total,
               g->asleep ? " - it went to sleep: wake it to finish" : "");
    }
    if (g->op == UI_ZW_OP_FIRMWARE) {
        static const char *const WHY[] = {"-", "not upgradable", "not the device the file is for",
                                          "its battery is low", "refused",
                                          "the file arrived damaged", "it could not store or "
                                          "start it", "it stopped answering",
                                          "not woken in time", "the radio could not reach it",
                                          "stopped", "the radio went away"};
        printf("  %s%u of %u parts sent", g->fw_wake ? "waiting for it to wake up; " : "",
               (unsigned)g->fw_done, (unsigned)g->fw_total);
        if (g->step == UI_ZW_STEP_DONE && g->result == UI_ZW_RES_FW_FAILED) {
            printf(" - %s", g->fw_why < sizeof WHY / sizeof WHY[0] ? WHY[g->fw_why] : "?");
        }
        if (g->step == UI_ZW_STEP_DONE && g->result == UI_ZW_RES_FW_UPDATED) {
            printf(" - it restarts for %u s, then it is read again", (unsigned)g->fw_wait_s);
        }
        printf("\n");
    }
}

/* `zwave output` / `zwave garage` (ZW-034, ZW-035): what a device switches, and a garage door -
 * the device page's controls, for the bench. Every partition disarmed; opening a garage door here
 * is the installer's own action at the panel. */
static int cmd_output(int argc, char **argv)
{
    uint16_t node = 0u, ep = 0u;
    if (argc == 3 && parse_u16(argv[2], &node)) {
        ui_zw_device_page_t page;
        zone_service_lock();
        const bool found = zwz_panel_device(zwsvc_panel(), zwsvc_now_ms(), node,
                                            zwz_panel_generation(zwsvc_panel(), node), &page);
        zone_service_unlock();
        if (!found) {
            printf("no device %u\n", (unsigned)node);
            return 1;
        }
        for (size_t i = 0; i < page.output_count; i++) {
            const ui_zw_output_t *o = &page.outputs[i];
            printf("end point %u: %s, level %d%s%s%s%s\n", (unsigned)o->endpoint,
                   o->kind == UI_ZW_OUT_DIMMER ? "dimmer" : o->kind == UI_ZW_OUT_RELAY ? "relay"
                                                                                       : "switch",
                   (int)o->level, o->busy ? ", busy" : "", o->garage ? ", garage door " : "",
                   o->garage ? o->garage_state : "", o->last > 1u ? " (last command failed)" : "");
        }
        if (page.output_count == 0u) {
            printf("it switches nothing\n");
        }
        return 0;
    }
    int level = -1;
    if (argc == 5 && parse_u16(argv[2], &node) && parse_u16(argv[3], &ep)) {
        if (strcmp(argv[4], "on") == 0) {
            level = 100;
        } else if (strcmp(argv[4], "off") == 0) {
            level = 0;
        } else {
            uint16_t v = 0u;
            level = parse_u16(argv[4], &v) && v <= 100u ? (int)v : -1;
        }
    }
    if (level < 0 || ep > 255u) {
        printf("zwave output <node> [<end point> on|off|<0-100>]\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    ui_zw_request_t extra;
    memset(&extra, 0, sizeof extra);
    extra.endpoint = (uint8_t)ep;
    extra.level = (int16_t)level;
    return ask(UI_ZW_REQ_OUTPUT, node, &extra);
}

static int cmd_garage(int argc, char **argv)
{
    uint16_t node = 0u, ep = 0u, zone = 0u, travel = 0u;
    const bool ok = argc >= 5 && parse_u16(argv[2], &node) && parse_u16(argv[3], &ep) && ep < 256u;
    ui_zw_request_t extra;
    memset(&extra, 0, sizeof extra);
    extra.endpoint = (uint8_t)ep;
    uint8_t kind = 0u;
    if (ok && argc == 5 && (strcmp(argv[4], "open") == 0 || strcmp(argv[4], "close") == 0)) {
        kind = UI_ZW_REQ_GARAGE_MOVE;
        extra.level = strcmp(argv[4], "open") == 0 ? 100 : 0;
    } else if (ok && argc == 5 && strcmp(argv[4], "clear") == 0) {
        kind = UI_ZW_REQ_GARAGE;
    } else if (ok && (argc == 6 || argc == 7) && strcmp(argv[4], "set") == 0 &&
               parse_u16(argv[5], &zone) && zone > 0u &&
               (argc == 6 || parse_u16(argv[6], &travel))) {
        kind = UI_ZW_REQ_GARAGE;
        extra.zone = zone;
        extra.seconds = travel;
    }
    if (kind == 0u) {
        printf("zwave garage <node> <end point> open|close|clear|set <zone> [<travel s>]\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    const int rc = ask(kind, node, &extra);
    if (rc == 0 && kind == UI_ZW_REQ_GARAGE_MOVE) {
        printf("`zwave output %u` says where the door is and how the press ended\n",
               (unsigned)node);
    }
    return rc;
}

/* `zwave jam [<dBm>]` (network.md §9, ZW-024): the background noise per channel, jammed or not,
 * the threshold; with a number, the threshold the quietest channel must reach (-100..-20 dBm),
 * stored with the Z-Wave settings. Setting it wants every partition disarmed. */
static int cmd_jam(int argc, char **argv)
{
    if (argc >= 3) {
        char *end = NULL;
        const long v = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || v < -100 || v > -20) {
            printf("zwave jam [<dBm>, -100 to -20]\n");
            return 1;
        }
        if (!disarmed()) {
            return 1;
        }
        zone_service_lock();
        const bool ok = zwz_panel_set_jam(zwsvc_panel(), zwsvc_now_ms(), (int8_t)v);
        zone_service_unlock();
        printf(ok ? "jamming threshold %ld dBm (stored)\n" : "not set: %ld dBm\n", v);
        return ok ? 0 : 1;
    }
    zone_service_lock();
    const zw_jam_t j = *zw_net_jam(&zwsvc_panel()->net);
    zone_service_unlock();
    printf("%s; threshold %d dBm; ", j.jammed ? "JAMMED" : "not jammed", (int)j.threshold);
    if (!j.has_reading) {
        printf("no background noise reading (yet)\n");
        return 0;
    }
    printf("background noise:");
    for (uint8_t i = 0; i < j.channels && i < ZW_JAM_CHANNELS; i++) {
        const int8_t v = j.rssi[i];
        if (v == ZW_RSSI_NOT_AVAILABLE) {
            printf(" -");
        } else if (v == ZW_RSSI_SATURATED) {
            printf(" saturated");
        } else if (v == ZW_RSSI_NO_SIGNAL) {
            printf(" quiet");
        } else {
            printf(" %d", (int)v);
        }
    }
    printf(" dBm (quietest %d)\n", (int)j.quietest);
    return 0;
}

/* "-2.5" -> -25: dBm with at most one decimal, in tenths; false when it isn't one. */
static bool parse_dbm10(const char *s, int16_t *out)
{
    const bool neg = *s == '-';
    long whole = 0;
    long tenth = 0;
    s += neg || *s == '+' ? 1 : 0;
    if (*s < '0' || *s > '9') {
        return false;
    }
    while (*s >= '0' && *s <= '9' && whole < 1000) {
        whole = whole * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') {
            return false;
        }
        tenth = *s++ - '0';
    }
    if (*s != '\0' || whole >= 1000) {
        return false;
    }
    const long v = whole * 10 + tenth;
    *out = (int16_t)(neg ? -v : v);
    return true;
}

static void print_dbm10(int16_t v)
{
    const int a = v < 0 ? -(int)v : (int)v;
    printf("%s%d.%d dBm", v < 0 ? "-" : "", a / 10, a % 10);
}

/* `zwave power` (network.md §12 power, phase 3; ZW-025): the radio's transmit power, and a
 * change within the power it came with (the first the panel read - Zooz sets the legal value):
 * `zwave power <dBm>` (-10 to +20, one decimal) and `zwave power lr 10|14|20`. Every partition
 * disarmed to change it; the radio restarts to apply it. */
static int cmd_power(int argc, char **argv)
{
    zwsvc_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = ZWSVC_REQ_POWER;
    zone_service_lock();
    zw_net_power_t pw = *zw_net_power(&zwsvc_panel()->net);
    const zwz_cfg_t cfg = zwsvc_panel()->setup.cfg;
    zone_service_unlock();
    if (argc < 3) {
        if (!pw.known) {
            const ui_zw_req_result_t res = zwsvc_submit(&r);
            if (res != UI_ZW_REQ_OK) {
                printf("can't read it: %s\n", res == UI_ZW_REQ_UNSUPPORTED
                                                  ? "the radio isn't working, or has no power setting"
                                                  : req_words(res));
                return 1;
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
            zone_service_lock();
            pw = *zw_net_power(&zwsvc_panel()->net);
            zone_service_unlock();
        }
        if (!pw.known) {
            printf("not read yet: ask again in a moment\n");
            return 1;
        }
        printf("power ");
        print_dbm10(pw.dbm10);
        printf(" (calibration ");
        print_dbm10(pw.cal_dbm10);
        printf("); came with ");
        if (cfg.power_cap_set) { print_dbm10(cfg.power_cap_dbm10); } else { printf("?"); }
        if (pw.lr_known) {
            printf("; Long Range maximum ");
            print_dbm10(pw.lr_dbm10);
            printf(", came with ");
            if (cfg.lr_cap_set) { print_dbm10(cfg.lr_cap_dbm10); } else { printf("?"); }
        }
        printf("\n`zwave power <dBm>` / `zwave power lr 10|14|20`: at most what it came with\n");
        return 0;
    }
    r.power_set = true;
    r.power_lr = strcmp(argv[2], "lr") == 0;
    const char *value = r.power_lr ? (argc >= 4 ? argv[3] : "") : argv[2];
    if (!parse_dbm10(value, &r.power_dbm10)) {
        printf("zwave power [<dBm>|lr 10|14|20]\n");
        return 1;
    }
    const bool capped = r.power_lr ? cfg.lr_cap_set : cfg.power_cap_set;
    const int16_t cap = r.power_lr ? cfg.lr_cap_dbm10 : cfg.power_cap_dbm10;
    if (!capped) {
        printf("read it first (`zwave power`)\n");
        return 1;
    }
    if (r.power_dbm10 > cap) {
        printf("not above what the radio came with (");
        print_dbm10(cap);
        printf("): its maker's legal setting\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    r.allowed = true;
    const ui_zw_req_result_t res = zwsvc_submit(&r);
    if (res == UI_ZW_REQ_OK) {
        printf("sent: the radio restarts to apply it; `zwave power` reads it back\n");
        return 0;
    }
    printf("not changed: %s\n", res == UI_ZW_REQ_INVALID
                                     ? (r.power_lr ? "Long Range takes 10, 14 or 20 dBm"
                                                   : "-10 to +20 dBm, or the radio can't change it")
                                     : req_words(res));
    return 1;
}

/* `zwave smartstart` (network.md §4.2, ZW-024): the provisioning list - devices registered by
 * their QR code's digits or their 40-digit key join by themselves when powered, while everything
 * is disarmed. Every partition disarmed to change it. */
static const char *prov_words(ui_zw_prov_result_t r)
{
    switch (r) {
    case UI_ZW_PROV_ADDED: return "registered: it joins when powered";
    case UI_ZW_PROV_UPDATED: return "done";
    case UI_ZW_PROV_JOINED_ALREADY: return "registered; that device is in the network already";
    case UI_ZW_PROV_FULL: return "the list is full (32)";
    case UI_ZW_PROV_BAD_CODE: return "not a Z-Wave code or key";
    case UI_ZW_PROV_CHECKSUM: return "a Z-Wave code, damaged or mistyped";
    case UI_ZW_PROV_S2_ONLY: return "its code can't join by itself: add it with its PIN";
    case UI_ZW_PROV_NOT_ALLOWED: return "not allowed";
    default: return "no such entry";
    }
}

static int cmd_smartstart(int argc, char **argv)
{
    static ALARM_SLOW_BSS ui_zw_prov_t list[UI_ZW_PROV_MAX];
    const size_t n = zwave_service_ui_prov(list, UI_ZW_PROV_MAX);
    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        static const char *const STATE[] = {"waiting", "joined", "stopped", "off"};
        for (size_t i = 0; i < n; i++) {
            const ui_zw_prov_t *e = &list[i];
            char dsk[48];
            (void)zw_dsk_to_text(e->dsk, dsk, sizeof dsk);
            /* the PIN (the first group) is never printed: it is the device's secret */
            printf("%u. %s %-7s keys 0x%02x%s  xxxxx%s", (unsigned)(i + 1u),
                   e->name[0] != '\0' ? e->name : "(no name)",
                   e->state < 4u ? STATE[e->state] : "?", (unsigned)e->grant,
                   e->long_range ? " long-range" : "", &dsk[5]);
            if (e->node != 0u) {
                printf("  node %u", (unsigned)e->node);
            }
            if (e->failures != 0u) {
                printf("  %u failed", (unsigned)e->failures);
            }
            printf("\n");
        }
        if (n == 0u) {
            printf("no device registered\n");
        }
        return 0;
    }
    if (!disarmed()) {
        return 1;
    }
    ui_zw_prov_result_t r = UI_ZW_PROV_NOT_FOUND;
    if (strcmp(argv[2], "add") == 0 && argc >= 4) {
        r = zwsvc_prov_register(argv[3], argc >= 5 ? argv[4] : NULL, true);
    } else if (argc == 4 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0 ||
                             strcmp(argv[2], "forget") == 0)) {
        uint16_t i = 0u;
        if (!parse_u16(argv[3], &i) || i == 0u || i > n) {
            printf("no entry %s (`zwave smartstart list`)\n", argv[3]);
            return 1;
        }
        const uint8_t op = strcmp(argv[2], "on") == 0    ? UI_ZW_PROV_ON
                           : strcmp(argv[2], "off") == 0 ? UI_ZW_PROV_OFF_OP
                                                         : UI_ZW_PROV_FORGET;
        r = zwsvc_prov_change(list[i - 1u].dsk, op, 0u, true);
    } else {
        printf("zwave smartstart [list|add <QR digits or 40-digit key> [<name>]|on <i>|off <i>|"
               "forget <i>]\n");
        return 1;
    }
    printf("%s\n", prov_words(r));
    return r == UI_ZW_PROV_ADDED || r == UI_ZW_PROV_UPDATED || r == UI_ZW_PROV_JOINED_ALREADY
               ? 0
               : 1;
}

/* `zwave firmware`: a device's software (ZW-033) - the update service asked, the version it offers
 * installed, a running update stopped. Only when asked; every partition disarmed; the device page
 * shows the same job. */
static int cmd_firmware(int argc, char **argv)
{
    char words[200];
    if (argc == 2) {
        zwsvc_fw_describe(words, sizeof words);
        printf("%s\n", words);
        ui_zw_progress_t g;
        zwave_service_ui_progress(&g);
        if (g.op == UI_ZW_OP_FIRMWARE) {
            print_progress(&g);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "stop") == 0) {
        return ask(UI_ZW_REQ_STOP, 0u, NULL);
    }
    uint16_t node = 0u;
    if (argc != 4 || !parse_u16(argv[2], &node) ||
        (strcmp(argv[3], "check") != 0 && strcmp(argv[3], "install") != 0)) {
        printf("zwave firmware [<node> check|install | stop]\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    zwsvc_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = ZWSVC_REQ_FW;
    r.allowed = true;
    r.console = true;
    r.ui.kind = strcmp(argv[3], "check") == 0 ? UI_ZW_REQ_FW_CHECK : UI_ZW_REQ_FW_INSTALL;
    r.ui.node = node;
    zone_service_lock();
    r.ui.generation = zwz_panel_generation(zwsvc_panel(), node);
    zone_service_unlock();
    const ui_zw_req_result_t res = zwsvc_submit(&r);
    printf("%s%s\n", req_words(res),
           res == UI_ZW_REQ_OK ? ": `zwave firmware` follows it" : "");
    return res == UI_ZW_REQ_OK ? 0 : 1;
}

/* `zwave radio-update [check|install]` (network.md §14, ZW-025): the radio's own firmware from the
 * Z-Wave JS update service, asked about as Z-Wave JS asks about a controller (its product IDs and
 * firmware version); install downloads the file offered, checks its SHA-256 and its .gbl, and
 * sends it over the wire - Z-Wave sensors unheard meanwhile, arming held until the radio is back.
 * Every partition disarmed. Without a word: the last check and the update's progress. */
static int cmd_radio_update(int argc, char **argv)
{
    static const char *const CHECK[] = {"not checked", "checking", "up to date",
                                        "not known to the update service", "available",
                                        "check failed", "downloading", "file failed"};
    if (argc == 2) {
        ui_zw_status_t st;
        (void)zwave_service_ui_status(&st);
        printf("radio firmware %u.%02u: %s%s%s%s%s\n", (unsigned)st.firmware_major,
               (unsigned)st.firmware_minor, st.radio_fw_check < 8u ? CHECK[st.radio_fw_check] : "?",
               st.radio_fw_version[0] != '\0' ? ", version " : "", st.radio_fw_version,
               st.radio_fw_words[0] != '\0' ? " - " : "", st.radio_fw_words);
        ui_zw_progress_t g;
        zwave_service_ui_progress(&g);
        if (g.op == UI_ZW_OP_RADIO_UPDATE) {
            printf("update: %u of %u blocks%s", (unsigned)g.fw_done, (unsigned)g.fw_total,
                   g.step == UI_ZW_STEP_DONE
                       ? (g.result == UI_ZW_RES_FW_UPDATED ? ", updated" : ", failed")
                       : ", sending");
            if (g.fw_error != 0u) {
                printf(" (bootloader error 0x%02X)", (unsigned)g.fw_error);
            }
            printf("\n");
        }
        return 0;
    }
    if (argc != 3 || (strcmp(argv[2], "check") != 0 && strcmp(argv[2], "install") != 0)) {
        printf("zwave radio-update [check|install]\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    zwsvc_req_t r;
    memset(&r, 0, sizeof r);
    r.kind = ZWSVC_REQ_FW;
    r.allowed = true;
    r.console = true;
    r.ui.kind = strcmp(argv[2], "check") == 0 ? UI_ZW_REQ_FW_CHECK : UI_ZW_REQ_FW_INSTALL;
    r.ui.node = 0u; /* the radio itself */
    const ui_zw_req_result_t res = zwsvc_submit(&r);
    printf("%s%s\n", req_words(res),
           res == UI_ZW_REQ_OK ? ": `zwave radio-update` follows it" : "");
    return res == UI_ZW_REQ_OK ? 0 : 1;
}

/* "place <id>: <where> - UART<n> TX GPIO <tx>, RX GPIO <rx>, reset line GPIO <g>|none" */
static void print_place(const zwave_service_config_t *c)
{
    printf("place %s: %s - UART%d TX GPIO %d, RX GPIO %d, reset line %s", c->place, c->where,
           CONFIG_ALARM_ZWAVE_UART_NUM, (int)c->tx, (int)c->rx, c->reset >= 0 ? "GPIO " : "none");
    if (c->reset >= 0) {
        printf("%d", (int)c->reset);
    }
    printf("\n");
}

/* The stored custom layout's line, exactly "custom tx=<g> rx=<g>[ reset=<g>]" (ZW-046: the
 * Flasher reads it back), then why it is not used and the pin warning, this start. */
static void print_custom(void)
{
    const zwave_service_rules_t *rules = zwsvc_rules();
    int16_t tx;
    int16_t rx;
    int16_t reset;
    if (zwave_service_stored_custom(&tx, &rx, &reset) == ZWAVE_CUSTOM_STORED) {
        if (reset >= 0) {
            printf("custom tx=%d rx=%d reset=%d\n", (int)tx, (int)rx, (int)reset);
        } else {
            printf("custom tx=%d rx=%d\n", (int)tx, (int)rx);
        }
    }
    if (rules->custom_why != NULL && rules->custom_why[0] != '\0') {
        printf("%s\n", rules->custom_why);
    }
    if (rules->warning != NULL && rules->warning[0] != '\0') {
        printf("%s\n", rules->warning);
    }
}

/* The reset line stored for the next start: the custom place's own, or `zwave reset-pin`'s. */
static int16_t next_reset_pin(void)
{
    int16_t tx;
    int16_t rx;
    int16_t reset;
    const zwave_custom_t custom = zwave_service_stored_custom(&tx, &rx, &reset);
    return custom == ZWAVE_CUSTOM_NONE ? zwave_service_stored_reset_pin() : reset;
}

static int cmd_status(void)
{
    const zwave_service_config_t *c = zwsvc_config();
    char stored[16];
    const bool has_stored = zwave_service_stored_place(stored, sizeof stored);
    const int16_t stored_pin = next_reset_pin();
    if (!zwsvc_running()) {
        if (zwsvc_crypto_check() == ZWSVC_CRYPTO_FAILED) {
            printf("Z-Wave radio off: its crypto self-test failed at start (place %s unused)\n",
                   c->place);
        } else if (c->place != NULL) {
            printf("Z-Wave radio off: it did not start at place %s (the boot log says why)\n",
                   c->place);
        } else if (zwsvc_rules()->custom_why != NULL && zwsvc_rules()->custom_why[0] != '\0') {
            /* ZW-046: a custom place refused at start; print_custom() below says why */
            printf("Z-Wave radio off: its place on your own pins was not used\n");
        } else {
            printf("Z-Wave radio off: no place chosen\n");
        }
        printf("crypto self-test: %s\n", crypto_words(zwsvc_crypto_check()));
        if (has_stored) {
            printf("place %s from the next restart\n", stored);
        }
        print_custom(); /* ZW-046: before the hint that ends the reply */
        printf("`zwave place` lists the places this board offers\n");
        return 0;
    }
    print_place(c);
    if (!has_stored || strcmp(stored, c->place) != 0 || stored_pin != c->reset) {
        printf("from the next restart: place %s, reset line %d\n", has_stored ? stored : "off",
               (int)stored_pin);
    }
    print_custom(); /* ZW-046: after the place lines */
    ui_zw_status_t st;
    ui_zw_progress_t g;
    zw_module_facts_t f;
    zw_link_down_t down;
    zw_host_stats_t hs;
    zw_net_stats_t ns;
    zone_service_lock();
    zwz_panel_t *p = zwsvc_panel();
    zwz_panel_status(p, alarm_service_now(), &st);
    zwz_panel_progress(p, alarm_service_now(), &g);
    f = *zw_host_facts(&p->net.host);
    down = *zw_host_down_reason(&p->net.host);
    hs = *zw_host_stats(&p->net.host);
    ns = *zw_net_stats(&p->net);
    const size_t log_lost = p->log_lost;
    zone_service_unlock();
    printf("radio: %s\n", radio_words(st.radio));
    if (st.radio == UI_ZW_RADIO_NOT_ANSWERING || st.radio == UI_ZW_RADIO_UNUSABLE) {
        printf("  why: %s (step %u, detail %02X)\n", down_words(down.reason), (unsigned)down.step,
               (unsigned)down.detail);
    }
    printf("crypto self-test: %s\n", crypto_words(zwsvc_crypto_check()));
    printf("module: firmware %u.%02u, Z-Wave %u.%u.%u (%s, library type %u), chip %02X v%u%s\n",
           (unsigned)f.firmware_major, (unsigned)f.firmware_minor, (unsigned)f.protocol_major,
           (unsigned)f.protocol_minor, (unsigned)f.protocol_patch, f.library,
           (unsigned)f.library_type, (unsigned)f.chip_type, (unsigned)f.chip_version,
           f.update_note ? " - update the Z-Wave radio's software" : "");
    printf("network: home ID %08" PRIX32 ", the panel is node %u, region %s (%02X), %u device(s), "
           "%u not heard; new devices check in every %" PRIu32 " s\n",
           f.home_id, (unsigned)f.node_id, st.region_known ? region_words(st.region) : "unknown",
           (unsigned)st.region, (unsigned)st.devices, (unsigned)st.not_heard, st.check_in_s);
    print_progress(&g);
    zw_link_stats_t ls;
    zw_link_stats(&ls);
    printf("link: rx %" PRIu32 " bytes, frames %" PRIu32 ", NAKed %" PRIu32 " damaged + %" PRIu32
           " unready, lost %" PRIu32 ", byte timeouts %" PRIu32 ", stray bytes %" PRIu32
           ", overflows %" PRIu32 "; tx %" PRIu32 " writes, %" PRIu32 " bytes, %" PRIu32
           " refused%s\n",
           ls.rx_bytes, ls.frames, ls.naks, ls.full, ls.lost, ls.timeouts, ls.discarded,
           ls.overflows, ls.tx_frames, ls.tx_bytes, ls.tx_lost,
           zw_link_tracing() ? "; tracing" : "");
    printf("host: start-ups %" PRIu32 ", link ups %" PRIu32 " downs %" PRIu32 ", restarts %" PRIu32
           ", recoveries %" PRIu32 ", reset pulses %" PRIu32 ", bootloader %" PRIu32 "\n",
           hs.attempts, hs.link_ups, hs.link_downs, hs.restarts, hs.recoveries, hs.reset_pulses,
           hs.bootloader);
    printf("network: checks %" PRIu32 ", cache writes %" PRIu32 ", foreign %" PRIu32
           ", malformed %" PRIu32 ", queued for sleepers %" PRIu32 ", pings %" PRIu32 "\n",
           ns.checks, ns.stores, ns.foreign, ns.malformed, ns.queued, ns.pings);
    zwsvc_stats_t ss;
    zwsvc_stats(&ss);
    printf("service: NVS writes %" PRIu32 " (%" PRIu32 " failed), events %" PRIu32 " (%" PRIu32
           " failed, %u lost), requests %" PRIu32 " (%" PRIu32 " timed out)\n",
           ss.stores, ss.store_failures, ss.events, ss.events_failed, (unsigned)log_lost,
           ss.requests, ss.request_timeouts);
    /* the tasks' stack high-water marks: bytes never used (0 of 0: not started) */
    printf("task stacks never used: zwave %u of %u, link %u of %u, firmware worker %u of %u "
           "bytes\n",
           (unsigned)ss.stack_free, (unsigned)ss.stack_size, (unsigned)ss.link_stack_free,
           (unsigned)ss.link_stack_size, (unsigned)ss.fw_stack_free,
           (unsigned)ss.fw_stack_size);
    return 0;
}

/* ---- the devices ----------------------------------------------------------------------------- */

static int cmd_nodes(void)
{
    if (!running()) {
        return 1;
    }
    ui_zw_device_t *list = heap_caps_calloc(UI_ZW_DEVICES_MAX, sizeof(*list), MALLOC_CAP_DEFAULT);
    if (list == NULL) {
        printf("no memory\n");
        return 1;
    }
    zone_service_lock();
    const size_t n = zwz_panel_devices(zwsvc_panel(), alarm_service_now(), list,
                                       UI_ZW_DEVICES_MAX);
    zone_service_unlock();
    printf("%u device(s)\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const ui_zw_device_t *d = &list[i];
        printf("%3u gen %-3u %-20s %-19s %-9s %s", (unsigned)d->node, (unsigned)d->generation,
               state_words(d->state), security_words(d->security),
               d->listening ? "listening" : "sleeping", d->unreachable ? "UNREACHABLE " : "");
        if (d->heard_s != UI_ZW_NEVER) {
            printf("heard %" PRIu32 " s ago ", d->heard_s);
        } else {
            printf("not heard since the start ");
        }
        if (d->battery_pct != UI_ZW_BATTERY_UNKNOWN) {
            printf("battery %u%%%s ", (unsigned)d->battery_pct, d->battery_low ? " LOW" : "");
        }
        for (uint8_t z = 0u; z < d->zone_count; z++) {
            printf("%s%u", z == 0u ? "zone " : ",", (unsigned)d->zones[z]);
        }
        printf(" %s\n", d->model);
    }
    heap_caps_free(list);
    return 0;
}

static int cmd_node(uint16_t node)
{
    if (!running()) {
        return 1;
    }
    typedef struct {
        zw_node_record_t rec;
        zw_health_node_t health;
        bool has_health;
        zw_s2_node_stats_t s2;
        bool has_s2;
        zsrc_zw_device_t dev;
        bool has_dev;
        ui_zw_device_page_t page;
        uint8_t interview;
        uint32_t queued;
    } facts_t;
    facts_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_DEFAULT);
    if (f == NULL) {
        printf("no memory\n");
        return 1;
    }
    bool found = false;
    zone_service_lock();
    zwz_panel_t *p = zwsvc_panel();
    const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, node);
    if (rec != NULL) {
        found = true;
        f->rec = *rec;
        for (size_t i = 0; i < p->net.health.node_count && i < ZW_HEALTH_NODES_MAX; i++) {
            if (p->net.health.nodes[i].node_id == node) {
                f->health = p->net.health.nodes[i];
                f->has_health = true;
            }
        }
        const zw_s2_node_stats_t *s = zw_s2_node_stats(&p->net.s2, node);
        if (s != NULL) {
            f->s2 = *s;
            f->has_s2 = true;
        }
        const zsrc_zw_device_t *d = zsrc_zw_find(zwsvc_source(), node);
        if (d != NULL && d->generation == rec->generation) {
            f->dev = *d;
            f->has_dev = true;
        }
        (void)zwz_panel_device(p, alarm_service_now(), node, rec->generation, &f->page);
        f->interview = zw_iv_step(&p->net.iv, node);
        f->queued = (uint32_t)zw_health_queue_len(&p->net.health, node);
    }
    zone_service_unlock();
    if (!found) {
        printf("no device %u in this network (`zwave nodes`)\n", (unsigned)node);
        heap_caps_free(f);
        return 1;
    }
    const zw_node_record_t *r = &f->rec;
    printf("device %u, generation %u: %s\n", (unsigned)node, (unsigned)r->generation,
           f->page.dev.model[0] != '\0' ? f->page.dev.model : "(no quirks row)");
    if (r->has_identity) {
        printf("  identity: manufacturer %04X, product type %04X, product %04X\n",
               (unsigned)r->manufacturer_id, (unsigned)r->product_type, (unsigned)r->product_id);
    }
    printf("  protocol: %s%s%s%s, classes %02X/%02X/%02X\n", r->is_listening ? "listening" : "",
           r->is_flirs ? "FLiRS" : "", !r->is_listening && !r->is_flirs ? "sleeping" : "",
           r->is_routing ? ", routing" : "", (unsigned)r->basic_class, (unsigned)r->generic_class,
           (unsigned)r->specific_class);
    printf("  security: %s (classes %02X)\n", security_words(f->page.dev.security),
           (unsigned)r->security_classes);
    printf("  interview: step %u of %u done", (unsigned)r->interview_step,
           (unsigned)ZW_IV_STEP_DONE);
    if (f->interview != 0u) {
        printf(r->interview_step >= ZW_IV_STEP_DONE /* finished: BL-130's catch-up */
                   ? ", asking its alarm sensor types now (step %u)"
                   : ", asking step %u now",
               (unsigned)f->interview);
    }
    printf("\n");
    printf("  state: %s\n", state_words(f->page.dev.state));
    if (r->has_wake_up) {
        printf("  wake-up: every %" PRIu32 " s (limits %" PRIu32 "-%" PRIu32 " step %" PRIu32
               "), to node %u", r->wake_up_interval_seconds, r->wake_up_min_seconds,
               r->wake_up_max_seconds, r->wake_up_step_seconds, (unsigned)r->wake_up_notify_node_id);
        if (f->page.check_in_next_s != 0u) {
            printf("; %" PRIu32 " s from its next wake-up", f->page.check_in_next_s);
        }
        printf("\n");
    }
    if (r->has_lifeline) {
        printf("  lifeline: group %u%s\n", (unsigned)r->lifeline_group,
               r->lifeline_set ? ", set" : ", not set yet");
    }
    printf("  command classes:");
    for (uint8_t i = 0u; i < r->cc_count; i++) {
        printf(" %02X%s", (unsigned)r->ccs[i].cc, r->ccs[i].secure ? "s" : "");
    }
    printf("\n");
    if (f->has_health) {
        printf("  supervision: %s, heard %" PRIu64 " ms ago, %s, %" PRIu32 " queued\n",
               f->health.reachable ? "reachable" : "UNREACHABLE",
               alarm_service_now() - f->health.last_heard_ms,
               f->health.awake ? "awake" : "asleep or listening", f->queued);
    }
    if (f->has_s2) {
        printf("  S2: decrypted %" PRIu32 ", failures %" PRIu32 ", duplicates %" PRIu32
               ", at the wrong level %" PRIu32 ", malformed %" PRIu32 ", nonce reports %" PRIu32
               ", resends %" PRIu32 "\n",
               f->s2.decrypted, f->s2.decrypt_failures, f->s2.duplicates, f->s2.wrong_level,
               f->s2.malformed, f->s2.nonce_reports, f->s2.resends);
    }
    if (f->has_dev) {
        printf("  zone source: battery %s%u%%, tamper %s, low battery %s%s\n",
               f->dev.battery_pct == ZSRC_ZW_BATTERY_UNKNOWN ? "unknown " : "",
               f->dev.battery_pct == ZSRC_ZW_BATTERY_UNKNOWN ? 0u : (unsigned)f->dev.battery_pct,
               f->dev.tamper_src != 0u ? "yes" : "no", f->dev.lowbat_src != 0u ? "yes" : "no",
               f->dev.gone ? ", LEFT THE NETWORK" : "");
    }
    for (uint8_t i = 0u; i < f->page.input_count; i++) {
        const ui_zw_input_t *in = &f->page.inputs[i];
        printf("  input %u/%u: %s, now %s%s", (unsigned)in->endpoint, (unsigned)in->slot,
               in->words, in->state[0] != '\0' ? in->state : "not known", in->zone != 0u ? ", zone " : "");
        if (in->zone != 0u) {
            printf("%u", (unsigned)in->zone);
        }
        printf("\n");
    }
    heap_caps_free(f);
    return 0;
}

/* ---- where it is wired ----------------------------------------------------------------------- */

/* `zwave place custom tx=<g> rx=<g> [reset=<g>]` (ZW-046): keys in any order, each once; every
 * reply starts "place custom from the next restart (`restart`)" or "not custom: ". Without
 * reset= there is no reset line. */
static int cmd_place_custom(int argc, char **argv)
{
    static const char *const KEYS[] = {"tx", "rx", "reset"};
    const zwave_service_rules_t *rules = zwsvc_rules();
    int16_t v[3];
    char why[120] = "";
    if (!board_custom_parse(argc, argv, KEYS, 3, 2, v, why, sizeof why)) {
        printf("not custom: %s\n", why);
        return 1;
    }
    snprintf(why, sizeof why, "this board has no pins for an add-on of your own");
    if (rules->custom_ok == NULL || !rules->custom_ok(v[0], v[1], v[2], why, sizeof why)) {
        printf("not custom: %s\n", why);
        return 1;
    }
    const esp_err_t err = zwsvc_store_custom(v[0], v[1], v[2]);
    if (err != ESP_OK) {
        printf("not custom: could not store it (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("place custom from the next restart (`restart`): the radio on your own pins, %s; they "
           "are no longer offered as wired terminals\n",
           v[2] >= 0 ? "with its reset line" : "without a reset line");
    return 0;
}

static int cmd_place(int argc, char **argv)
{
    const zwave_service_rules_t *rules = zwsvc_rules();
    char stored[16];
    const bool has = zwave_service_stored_place(stored, sizeof stored);
    const bool was_custom = has && strcmp(stored, "custom") == 0;
    if (argc == 2) {
        const zwave_service_config_t *c = zwsvc_config();
        const char *now_place = zwsvc_running() ? c->place : "off";
        const char *next = has ? stored : "off";
        if (zwsvc_running() && strcmp(c->place, "custom") == 0) {
            print_place(c); /* ZW-046: the status's line */
        } else if (zwsvc_running()) {
            printf("place %s: %s\n", c->place, c->where);
        } else {
            printf("place off\n");
        }
        if (strcmp(next, now_place) != 0) {
            printf("place %s from the next restart\n", next);
        }
        print_custom();
        printf("%s", rules->places != NULL ? rules->places : "");
        printf("`zwave place <place>` wires it there, `zwave place off` takes it away\n");
        return 0;
    }
    if (strcmp(argv[2], "custom") == 0) {
        return cmd_place_custom(argc - 3, argv + 3);
    }
    if (strcmp(argv[2], "off") == 0) {
        esp_err_t err = zwsvc_store_str("place", NULL);
        if (err == ESP_OK && was_custom) {
            err = zwsvc_forget_custom(); /* ZW-046: after the place */
        }
        if (err != ESP_OK) {
            printf("could not store it (%s)\n", esp_err_to_name(err));
            return 1;
        }
        printf("place off from the next restart (`restart`): no pin is touched\n");
        return 0;
    }
    char why[120] = "this board offers no place for it";
    if (strlen(argv[2]) >= sizeof stored || rules->place_ok == NULL ||
        !rules->place_ok(argv[2], why, sizeof why)) {
        printf("not %s: %s\n", argv[2], why);
        return 1;
    }
    esp_err_t err = zwsvc_store_str("place", argv[2]);
    if (err == ESP_OK && was_custom) {
        err = zwsvc_forget_custom(); /* ZW-046: leaving custom clears the reset pin too */
    }
    if (err != ESP_OK) {
        printf("could not store it (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("place %s from the next restart (`restart`); %s\n", argv[2],
           was_custom ? "no reset line (`zwave reset-pin`)"
                      : "a reset line stays where it was (`zwave reset-pin`)");
    return 0;
}

static int cmd_reset_pin(int argc, char **argv)
{
    const zwave_service_rules_t *rules = zwsvc_rules();
    const int16_t stored = next_reset_pin();
    char place[16];
    if (argc > 2 && zwave_service_stored_place(place, sizeof place) &&
        strcmp(place, "custom") == 0) {
        /* ZW-046: the custom place carries its own reset line */
        printf("not %s%s: the radio is on your own pins: `zwave place custom tx= rx= reset=`\n",
               strcmp(argv[2], "off") == 0 ? "" : "GPIO ", argv[2]);
        return 1;
    }
    if (argc == 2) {
        const zwave_service_config_t *c = zwsvc_config();
        if (zwsvc_running() && c->reset >= 0) {
            printf("reset line on GPIO %d\n", (int)c->reset);
        } else {
            printf("no reset line (a hung radio stays down until the panel restarts)\n");
        }
        if (stored != (zwsvc_running() ? c->reset : (int16_t)-1)) {
            printf("reset line %d from the next restart (-1: none)\n", (int)stored);
        }
        printf("`zwave reset-pin <gpio>` on a wired terminal (`terminals`), `off` for none; only "
               "a module etched \"Version 2.0\" has one\n");
        return 0;
    }
    if (strcmp(argv[2], "off") == 0) {
        const esp_err_t err = zwsvc_store_u8("rstpin", -1);
        printf(err == ESP_OK ? "no reset line from the next restart\n"
                             : "could not store it\n");
        return err == ESP_OK ? 0 : 1;
    }
    char *end = NULL;
    const long gpio = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || gpio < 0 || gpio > 63) {
        printf("zwave reset-pin <gpio>|off\n");
        return 1;
    }
    char why[120] = "this firmware cannot place it";
    if (rules->reset_ok == NULL || !rules->reset_ok((int16_t)gpio, why, sizeof why)) {
        printf("not GPIO %ld: %s\n", gpio, why);
        return 1;
    }
    const esp_err_t err = zwsvc_store_u8("rstpin", (int)gpio);
    if (err != ESP_OK) {
        printf("could not store it (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("reset line on GPIO %ld from the next restart (`restart`); it is no longer offered as a "
           "wired terminal\n", gpio);
    return 0;
}

/* ---- the network ----------------------------------------------------------------------------- */

static int cmd_allow(int argc, char **argv)
{
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "access") == 0) {
            r.keys |= ZW_S2_KEY_ACCESS;
        } else if (strcmp(argv[i], "auth") == 0) {
            r.keys |= ZW_S2_KEY_AUTH;
        } else if (strcmp(argv[i], "unauth") == 0) {
            r.keys |= ZW_S2_KEY_UNAUTH;
        } else {
            printf("zwave allow [access] [auth] [unauth]\n");
            return 1;
        }
    }
    if (r.keys == 0u) {
        ui_zw_progress_t g;
        zone_service_lock();
        zwz_panel_progress(zwsvc_panel(), alarm_service_now(), &g);
        zone_service_unlock();
        r.keys = g.requested; /* what it asks for */
    }
    return ask(UI_ZW_REQ_ALLOW, 0u, &r);
}

static int cmd_pin(int argc, char **argv)
{
    char *end = NULL;
    const unsigned long pin = argc >= 3 ? strtoul(argv[2], &end, 10) : 100000u;
    bool digits = argc >= 3 && strlen(argv[2]) == 5u;
    for (size_t i = 0; digits && i < 5u; i++) {
        digits = argv[2][i] >= '0' && argv[2][i] <= '9';
    }
    /* 5 digits (leading zeros kept), at most 65535: the key's first group is 16 bits */
    if (!digits || *end != '\0' || pin > UI_ZW_PIN_MAX) {
        printf("zwave pin <5 digits, 00000-65535>: the first group of the key on its label\n");
        return 1;
    }
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.pin = (uint32_t)pin;
    return ask(UI_ZW_REQ_PIN, 0u, &r);
}

static int cmd_feedback(int argc, char **argv)
{
    uint16_t node = 0u;
    if (argc == 2) {
        zwf_binding_t bindings[ZWF_TARGETS];
        bool live[ZWF_TARGETS];
        zone_service_lock();
        const zwf_t *f = zwsvc_feedback();
        for (size_t i = 0; i < ZWF_TARGETS; i++) {
            bindings[i] = f->target[i].binding; live[i] = f->target[i].live;
        }
        zone_service_unlock();
        for (size_t i = 0; i < ZWF_TARGETS; i++) {
            printf("slot %u: node %u generation %u partition %u (%s)\n", (unsigned)(i + 1u),
                   (unsigned)bindings[i].node, (unsigned)bindings[i].generation,
                   (unsigned)bindings[i].partition, bindings[i].partition == 0u ? "off" :
                   live[i] ? "eligible" : "waiting for matching secure device");
        }
        return 0;
    }
    uint16_t slot = 0u, partition = 0u;
    const bool off = argc == 4 && strcmp(argv[3], "off") == 0;
    if ((argc != 5 && !off) || !parse_u16(argv[2], &slot) || slot > ZWF_TARGETS ||
        (!off && (!parse_u16(argv[3], &node) || !parse_u16(argv[4], &partition) ||
                  partition > ALARM_MAX_PARTITIONS))) {
        printf("zwave keypad-feedback [<slot 1-8> <node> <partition> | <slot> off]\n");
        return 1;
    }
    if (!disarmed()) { return 1; }
    const zwsvc_req_t req = {.kind = ZWSVC_REQ_FEEDBACK, .allowed = true,
        .ui = {.node = node}, .feedback_slot = (uint8_t)(slot - 1u),
        .feedback_partition = (uint8_t)partition};
    const ui_zw_req_result_t result = zwsvc_submit(&req);
    printf("%s (output feedback only; keypad control assignment is separate)\n", req_words(result));
    return result == UI_ZW_REQ_OK ? 0 : 1;
}

/* `zwave keypad-control`: which keypads' buttons may arm and disarm (ZW-031). The users' codes
 * are the panel's; nothing here names or shows one. */
static int cmd_control(int argc, char **argv)
{
    if (argc == 2) {
        zwkc_binding_t b[ZWKC_SLOTS];
        bool live[ZWKC_SLOTS];
        zone_service_lock();
        const zwkc_t *k = zwsvc_keyctl();
        for (size_t i = 0; i < ZWKC_SLOTS; i++) {
            b[i] = k->slot[i].binding;
            live[i] = zwkc_live(k, i, &zwsvc_panel()->net);
        }
        zone_service_unlock();
        for (size_t i = 0; i < ZWKC_SLOTS; i++) {
            if (b[i].partition == 0u) {
                printf("slot %u: off\n", (unsigned)(i + 1u));
                continue;
            }
            printf("slot %u: node %u generation %u partition %u, emergency buttons%s%s%s%s (%s)\n",
                   (unsigned)(i + 1u), (unsigned)b[i].node, (unsigned)b[i].generation,
                   (unsigned)b[i].partition,
                   (b[i].panic_mask & (1u << ALARM_PANIC_FIRE)) != 0u ? " fire" : "",
                   (b[i].panic_mask & (1u << ALARM_PANIC_POLICE)) != 0u ? " police" : "",
                   (b[i].panic_mask & (1u << ALARM_PANIC_MEDICAL)) != 0u ? " medical" : "",
                   b[i].panic_mask == 0u ? " none" : "",
                   live[i] ? "active" : "not active: not the device or class it was assigned at");
        }
        return 0;
    }
    uint16_t slot = 0u, node = 0u, partition = 0u;
    uint8_t panic = 0u;
    const bool off = argc == 4 && strcmp(argv[3], "off") == 0;
    bool ok = parse_u16(argv[2], &slot) && slot <= ZWKC_SLOTS;
    if (ok && !off) {
        ok = (argc == 5 || argc == 6) && parse_u16(argv[3], &node) &&
             parse_u16(argv[4], &partition) && partition <= ALARM_MAX_PARTITIONS;
        for (char *t = argc == 6 ? strtok(argv[5], ",") : NULL; ok && t != NULL;
             t = strtok(NULL, ",")) {
            if (strcmp(t, "fire") == 0) {
                panic |= (uint8_t)(1u << ALARM_PANIC_FIRE);
            } else if (strcmp(t, "police") == 0) {
                panic |= (uint8_t)(1u << ALARM_PANIC_POLICE);
            } else if (strcmp(t, "medical") == 0) {
                panic |= (uint8_t)(1u << ALARM_PANIC_MEDICAL);
            } else {
                ok = false;
            }
        }
    }
    if (!ok || (argc != 4 && !off && argc < 5)) {
        printf("zwave keypad-control [<slot 1-8> <node> <partition> [fire,police,medical] | "
               "<slot> off]\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    zwsvc_req_t req;
    memset(&req, 0, sizeof req);
    req.kind = ZWSVC_REQ_CONTROL;
    req.allowed = true;
    req.ui.node = node;
    req.feedback_slot = (uint8_t)(slot - 1u);
    req.feedback_partition = off ? 0u : (uint8_t)partition;
    req.control_panic = panic;
    const ui_zw_req_result_t result = zwsvc_submit(&req);
    printf("%s (its buttons arm and disarm with the users' own codes; `zwave keypad-feedback` is "
           "separate)\n", req_words(result));
    return result == UI_ZW_REQ_OK ? 0 : 1;
}

/* A number, decimal or 0x hex, at most max. */
static bool parse_num(const char *s, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static void siren_list(void)
{
    zws_siren_t s[ZWS_SIRENS];
    bool used[ZWS_SIRENS];
    zone_service_lock();
    const zwz_panel_t *p = zwsvc_panel();
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        const zws_siren_t *x = zwz_panel_siren(p, i);
        used[i] = x != NULL;
        if (x != NULL) {
            s[i] = *x;
        }
    }
    const bool radio = p->sirens.radio_up;
    zone_service_unlock();
    const uint64_t now = zwsvc_now_ms();
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        if (!used[i]) {
            printf("siren %u: none\n", (unsigned)(i + 1u));
            continue;
        }
        const zws_config_t *c = &s[i].c;
        printf("siren %u: node %u generation %u ep %u, %s, partitions 0x%02x also 0x%02x, %s%s%s, "
               "tones %u/%u/%u arm %u disarm %u, volume %u%s, supervision %u min\n",
               (unsigned)(i + 1u), (unsigned)c->node, (unsigned)c->generation,
               (unsigned)c->endpoint,
               c->kind == ZWS_KIND_SOUND ? "Sound Switch" : "Binary Switch",
               (unsigned)c->partitions, (unsigned)c->also,
               c->burglary && c->fire ? "burglary and fire" : c->fire ? "fire" : "burglary",
               c->squawk ? ", squawk" : "", "", (unsigned)c->tone_burglary,
               (unsigned)c->tone_fire, (unsigned)c->tone_co, (unsigned)c->tone_arm,
               (unsigned)c->tone_disarm, (unsigned)c->volume, c->volume == 0u ? " (its own)" : "",
               (unsigned)(c->supervision_min != 0u ? c->supervision_min : ZWS_SUPERVISION_DEF_MIN));
        printf("   now: %s%s%s%s; heard %lu s ago%s; %lu set(s) confirmed, %lu report(s)\n",
               s[i].value != 0u ? "sounding" : "quiet",
               s[i].pending ? ", command not confirmed yet" : "",
               s[i].unconfirmed ? ", TROUBLE: commands not taken" : "",
               s[i].silent ? ", TROUBLE: not heard" : "",
               (unsigned long)(now >= s[i].heard_at ? (now - s[i].heard_at) / 1000u : 0u),
               radio ? "" : " (the radio is down: silence not counted)",
               (unsigned long)s[i].sets, (unsigned long)s[i].reports);
    }
}

/* `zwave siren`: Z-Wave sirens driven by the engine's bell (docs/features/36 sirens.md). */
static int cmd_siren(int argc, char **argv)
{
    static const char USAGE[] =
        "zwave siren [<node> [<ep>] parts <mask> [also <mask>] [burglary|fire|both] "
        "[squawk|nosquawk] [tone <b> <f> <co> <arm> <disarm>] [vol <0-100>] [super <min>] | "
        "<node> [<ep>] clear|test]\n";
    if (argc == 2) {
        siren_list();
        return 0;
    }
    uint16_t node = 0u;
    if (!parse_u16(argv[2], &node)) {
        printf("%s", USAGE);
        return 1;
    }
    int a = 3;
    unsigned long v = 0u;
    uint8_t ep = 0u;
    if (a < argc && parse_num(argv[a], 127u, &v)) {
        ep = (uint8_t)v;
        a++;
    }
    zwsvc_req_t req;
    memset(&req, 0, sizeof req);
    req.kind = ZWSVC_REQ_SIREN;
    req.siren.node = node;
    req.siren.endpoint = ep;
    if (a < argc && (strcmp(argv[a], "clear") == 0 || strcmp(argv[a], "test") == 0) &&
        a + 1 == argc) {
        const bool clear = strcmp(argv[a], "clear") == 0;
        if (clear && !disarmed()) {
            return 1;
        }
        req.siren_op = clear ? ZWSVC_SIREN_CLEAR : ZWSVC_SIREN_TEST;
        req.allowed = zone_service_all_disarmed();
        const ui_zw_req_result_t r = zwsvc_submit(&req);
        printf("%s%s\n", req_words(r),
               !clear && r == UI_ZW_REQ_NOT_ALLOWED ? " (one of its partitions is armed)" : "");
        return r == UI_ZW_REQ_OK ? 0 : 1;
    }
    /* assign or change: from its assignment as it is, else the defaults */
    zone_service_lock();
    const zwz_panel_t *p = zwsvc_panel();
    const uint16_t generation = zwz_panel_generation(p, node);
    zws_defaults(&req.siren, node, generation, ep, 0u);
    for (size_t i = 0; i < ZWS_SIRENS; i++) {
        const zws_siren_t *x = zwz_panel_siren(p, i);
        if (x != NULL && x->c.node == node && x->c.endpoint == ep && x->c.generation == generation) {
            req.siren = x->c;
            req.siren.kind = 0u; /* the panel picks again from what it lists */
        }
    }
    zone_service_unlock();
    if (generation == 0u) {
        printf("no device %u in this network\n", (unsigned)node);
        return 1;
    }
    bool ok = a < argc;
    while (ok && a < argc) {
        const char *w = argv[a++];
        if ((strcmp(w, "parts") == 0 || strcmp(w, "also") == 0) && a < argc &&
            parse_num(argv[a], (1u << ALARM_MAX_PARTITIONS) - 1u, &v)) {
            if (w[0] == 'p') {
                req.siren.partitions = (uint8_t)v;
            } else {
                req.siren.also = (uint8_t)v;
            }
            a++;
        } else if (strcmp(w, "burglary") == 0 || strcmp(w, "fire") == 0 ||
                   strcmp(w, "both") == 0) {
            req.siren.burglary = w[0] != 'f';
            req.siren.fire = w[0] != 'b' || w[1] == 'o';
        } else if (strcmp(w, "squawk") == 0 || strcmp(w, "nosquawk") == 0) {
            req.siren.squawk = w[0] == 's';
        } else if (strcmp(w, "tone") == 0 && a + 5 <= argc) {
            uint8_t *t[5] = {&req.siren.tone_burglary, &req.siren.tone_fire, &req.siren.tone_co,
                             &req.siren.tone_arm, &req.siren.tone_disarm};
            for (size_t k = 0; k < 5u && ok; k++) {
                ok = parse_num(argv[a++], 255u, &v);
                *t[k] = (uint8_t)v;
            }
        } else if (strcmp(w, "vol") == 0 && a < argc && parse_num(argv[a], ZWS_VOLUME_MAX, &v)) {
            req.siren.volume = (uint8_t)v;
            a++;
        } else if (strcmp(w, "super") == 0 && a < argc && parse_num(argv[a], 0xFFFFu, &v)) {
            req.siren.supervision_min = (uint16_t)v;
            a++;
        } else {
            ok = false;
        }
    }
    if (!ok) {
        printf("%s", USAGE);
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    req.siren_op = ZWSVC_SIREN_SET;
    req.allowed = true;
    const ui_zw_req_result_t r = zwsvc_submit(&req);
    printf("%s%s\n", req_words(r),
           r == UI_ZW_REQ_INVALID
               ? " (a device whose setup is done, listing Sound Switch or Binary Switch at that "
                 "end point; a partition; burglary or fire; volume 0-100; supervision 0 or 60+ min)"
               : "");
    return r == UI_ZW_REQ_OK ? 0 : 1;
}

static int cmd_checkin(int argc, char **argv)
{
    uint16_t node = 0u;
    if (argc < 4 || !parse_u16(argv[2], &node)) {
        printf("zwave checkin <n> 1h|4h|12h\n");
        return 1;
    }
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    r.seconds = strcmp(argv[3], "1h") == 0    ? 3600u
                : strcmp(argv[3], "4h") == 0  ? 4u * 3600u
                : strcmp(argv[3], "12h") == 0 ? 12u * 3600u
                                              : 0u;
    if (r.seconds == 0u) {
        printf("zwave checkin <n> 1h|4h|12h\n");
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    const int rc = ask(UI_ZW_REQ_CHECK_IN, node, &r);
    if (rc == 0) {
        printf("device %u checks in every %s from its next wake-up\n", (unsigned)node, argv[3]);
    }
    return rc;
}

static int cmd_region(int argc, char **argv)
{
    if (argc == 2) {
        ui_zw_status_t st;
        zone_service_lock();
        zwz_panel_status(zwsvc_panel(), alarm_service_now(), &st);
        zone_service_unlock();
        printf("region %s: %s\noffered:", st.region_known ? region_id(st.region) : "?",
               st.region_known ? region_words(st.region) : "not known");
        for (uint8_t i = 0u; i < st.region_count; i++) {
            printf(" %s", region_id(st.regions[i]));
        }
        printf("\n`zwave region <name>` restarts the radio; devices added in another region stop "
               "working until they are added again\n");
        return 0;
    }
    ui_zw_request_t r;
    memset(&r, 0, sizeof r);
    bool known = false;
    for (size_t i = 0; i < sizeof REGIONS / sizeof REGIONS[0]; i++) {
        if (strcmp(argv[2], REGIONS[i].id) == 0) {
            r.region = REGIONS[i].code;
            known = true;
        }
    }
    if (!known) {
        printf("no region %s (`zwave region` lists the radio's)\n", argv[2]);
        return 1;
    }
    if (!disarmed()) {
        return 1;
    }
    return ask(UI_ZW_REQ_REGION, 0u, &r);
}

static int cmd_zwave(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        return cmd_status();
    }
    const char *sub = argv[1];
    if (strcmp(sub, "place") == 0) {
        return cmd_place(argc, argv);
    }
    if (strcmp(sub, "reset-pin") == 0) {
        return cmd_reset_pin(argc, argv);
    }
    if (strcmp(sub, "jam") == 0) {
        return cmd_jam(argc, argv);
    }
    if (strcmp(sub, "trace") == 0) {
#if CONFIG_ALARM_BUILD_DEV
        if (argc < 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
            printf("zwave trace on|off\n");
            return 1;
        }
        zw_link_trace(strcmp(argv[2], "on") == 0);
        printf("trace %s\n", zw_link_tracing() ? "on: every frame in hex on the log" : "off");
        return 0;
#else
        printf("the frame trace is in development builds only (-Profile dev)\n");
        return 1;
#endif
    }
    if (!running()) {
        return 1;
    }
    uint16_t node = 0u;
    if (strcmp(sub, "nodes") == 0) {
        return cmd_nodes();
    }
    if (strcmp(sub, "node") == 0) {
        if (argc < 3 || !parse_u16(argv[2], &node)) {
            printf("zwave node <n>\n");
            return 1;
        }
        return cmd_node(node);
    }
    if (strcmp(sub, "add") == 0) {
        if (!disarmed()) {
            return 1;
        }
        const int rc = ask(UI_ZW_REQ_ADD, 0u, NULL);
        if (rc == 0) {
            printf("listening 60 s: put the device in pairing mode; `zwave status` follows it, "
                   "`zwave stop` ends it\n");
        }
        return rc;
    }
    if (strcmp(sub, "remove") == 0) {
        if (!disarmed()) {
            return 1;
        }
        const int rc = ask(UI_ZW_REQ_REMOVE, 0u, NULL);
        if (rc == 0) {
            printf("listening 60 s: put the device in pairing mode, as to add it\n");
        }
        return rc;
    }
    if (strcmp(sub, "stop") == 0) {
        return ask(UI_ZW_REQ_STOP, 0u, NULL);
    }
    if (strcmp(sub, "allow") == 0) {
        return cmd_allow(argc, argv);
    }
    if (strcmp(sub, "pin") == 0) {
        return cmd_pin(argc, argv);
    }
    if (strcmp(sub, "ping") == 0) {
        if (argc < 3 || !parse_u16(argv[2], &node)) {
            printf("zwave ping <n>\n");
            return 1;
        }
        const int rc = ask(UI_ZW_REQ_PING, node, NULL);
        if (rc == 0) {
            printf("`zwave status` shows its answer\n");
        } else {
            printf("(a sleeping device answers at its next wake-up only)\n");
        }
        return rc;
    }
    if (strcmp(sub, "interview") == 0) {
        if (argc < 3 || !parse_u16(argv[2], &node)) {
            printf("zwave interview <n>\n");
            return 1;
        }
        if (!disarmed()) {
            return 1;
        }
        /* BL-152, as Z-Wave JS's refreshInfo: a re-read waiting for its wake-up is left alone;
         * one under way starts again from step 1 (so does the touchscreen's "Set it up again") */
        zone_service_lock();
        const bool rereading = zw_iv_refreshing(&zwsvc_panel()->net.iv, node);
        const bool waiting = rereading && !zw_iv_asking(&zwsvc_panel()->net.iv, node);
        zone_service_unlock();
        if (waiting) {
            printf("%s: its reading waits for its wake-up already\n", req_words(UI_ZW_REQ_OK));
            return 0;
        }
        zwsvc_req_t r;
        memset(&r, 0, sizeof r);
        r.kind = ZWSVC_REQ_INTERVIEW;
        r.allowed = true;
        r.ui.node = node;
        const ui_zw_req_result_t res = zwsvc_submit(&r);
        if (res == UI_ZW_REQ_OK && rereading) {
            printf("%s: its reading started again from step 1\n", req_words(res));
        } else {
            printf("%s\n", req_words(res));
        }
        return res == UI_ZW_REQ_OK ? 0 : 1;
    }
    if (strcmp(sub, "checkin") == 0) {
        return cmd_checkin(argc, argv);
    }
    if (strcmp(sub, "setup") == 0) {
        /* an interview that stopped continues where it stopped; a finished device is read again
         * from step 1 (BL-152), working on its old facts meanwhile */
        if (argc < 3 || !parse_u16(argv[2], &node)) {
            printf("zwave setup <n>: continue a setup that stopped, or read a set-up device "
                   "again (its zones keep working)\n");
            return 1;
        }
        if (!disarmed()) {
            return 1;
        }
        return ask(UI_ZW_REQ_SETUP, node, NULL);
    }
    if (strcmp(sub, "remove-failed") == 0) { /* the device page's "reset, broken or gone" */
        if (argc < 3 || !parse_u16(argv[2], &node)) {
            printf("zwave remove-failed <n>: pings it, then only a device the radio lists as "
                   "failed is removed\n");
            return 1;
        }
        if (!disarmed()) {
            return 1;
        }
        const int rc = ask(UI_ZW_REQ_REMOVE_FAILED, node, NULL);
        if (rc == 0) {
            printf("checking it no longer answers; `zwave status` follows it\n");
        }
        return rc;
    }
    if (strcmp(sub, "keypad-feedback") == 0) { return cmd_feedback(argc, argv); }
    if (strcmp(sub, "keypad-control") == 0) { return cmd_control(argc, argv); }
    if (strcmp(sub, "firmware") == 0) { return cmd_firmware(argc, argv); }
    if (strcmp(sub, "output") == 0) { return cmd_output(argc, argv); }
    if (strcmp(sub, "garage") == 0) { return cmd_garage(argc, argv); }
    if (strcmp(sub, "siren") == 0) { return cmd_siren(argc, argv); }
    if (strcmp(sub, "smartstart") == 0) { return cmd_smartstart(argc, argv); }
    if (strcmp(sub, "power") == 0) { return cmd_power(argc, argv); }
    if (strcmp(sub, "radio-update") == 0) { return cmd_radio_update(argc, argv); }
    if (strcmp(sub, "region") == 0) {
        return cmd_region(argc, argv);
    }
    if (strcmp(sub, "restart") == 0) {
        if (!disarmed()) {
            return 1;
        }
        return ask(UI_ZW_REQ_RESTART, 0u, NULL);
    }
    if (strcmp(sub, "reset") == 0) {
        if (!disarmed()) {
            return 1;
        }
        zwsvc_req_t r;
        memset(&r, 0, sizeof r);
        r.kind = ZWSVC_REQ_PULSE;
        r.allowed = true;
        const ui_zw_req_result_t res = zwsvc_submit(&r);
        printf("%s\n", res == UI_ZW_REQ_UNSUPPORTED
                           ? "no reset line (`zwave reset-pin <gpio>`, a module etched \"Version "
                             "2.0\")"
                           : req_words(res));
        return res == UI_ZW_REQ_OK ? 0 : 1;
    }
    if (strcmp(sub, "reset-network") == 0) {
        if (argc < 3 || strcmp(argv[2], "confirm") != 0) {
            printf("zwave reset-network confirm: every device loses the panel and must be reset "
                   "and added again; their zones show CHECK until then\n");
            return 1;
        }
        if (!disarmed()) {
            return 1;
        }
        return ask(UI_ZW_REQ_RESET, 0u, NULL);
    }
    printf("zwave [status|place|reset-pin|restart|reset|add|remove|stop|allow|pin|nodes|node <n>|"
           "ping <n>|interview <n>|setup <n>|remove-failed <n>|keypad-feedback|keypad-control|"
           "firmware [<n> check|install|stop]|output <n> [<ep> on|off|<0-100>]|"
           "garage <n> <ep> open|close|clear|set <zone> [<s>]|"
           "siren [<n> [<ep>] parts <mask> ...|clear|test]|"
           "smartstart [list|add <code> [<name>]|on|off|forget <i>]|"
           "checkin <n> 1h|4h|12h|"
           "region [<name>]|jam [<dBm>]|power [<dBm>|lr 10|14|20]|radio-update [check|install]|"
           "trace on|off|"
           "reset-network confirm]\n");
    return 1;
}

esp_err_t zwave_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "zwave",
        .help = "Z-Wave radio (ZAC93): status, place, reset-pin, restart, reset, add, remove, stop, "
                "allow, pin, nodes, node <n>, ping <n>, interview <n>, checkin <n> 1h|4h|12h, "
                "region, trace, keypad-feedback, siren, reset-network confirm",
        .hint = "[status|place|add|remove|nodes|node|...]",
        .func = cmd_zwave,
    };
    return esp_console_cmd_register(&cmd);
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
