/*
 * zigbee_console.c - the service console's `zigbee` command (devices.md §12; BL-143 ZB-M6), the
 * way zwave_console.c is Z-Wave's. Everything goes through the request slot the pages use, as
 * the panel itself (user 0), with `allowed` = every partition disarmed (zbz_panel decides which
 * requests need it); the destructive ones need the word `confirm`, and the network reset the
 * installer code typed after it, checked with the engine as the console's keypad source (a wrong
 * one counts toward its lockout, ZB-017). Install codes are never printed back.
 */
#include "zigbee_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"

#define USAGE                                                                                      \
    "zigbee [status|retry|restart|devices|device <id> [signature]|trace on|off|add|stop|keep|remove <id>|forget <id> "      \
    "confirm|replace <id>|identify <id>|ping <id>|setup <id>|rename <id> [name]|codes|code <ieee> " \
    "<install code>|code forget <ieee>|survey|channel <n> confirm|reset confirm <installer code>|" \
    "progress|dismiss]"

#if CONFIG_ALARM_ZSRC_ZIGBEE

#include "alarm/alarm_types.h"
#include "esp_attr.h"
#include "radio_port.h"
#include "zone_service.h"

/* The console task's stack is small; internal RAM is the reserve (docs/bugs/B001). The console
 * runs one command at a time. */
static EXT_RAM_BSS_ATTR ui_zb_device_t s_list[UI_ZB_DEVICES_MAX];
static EXT_RAM_BSS_ATTR ui_zb_device_page_t s_page;
static EXT_RAM_BSS_ATTR ui_zb_request_t s_req;
static EXT_RAM_BSS_ATTR ui_zb_progress_t s_progress;
static EXT_RAM_BSS_ATTR ui_zb_code_t s_codes[UI_ZB_CODES_MAX];

static void ieee_print(uint64_t ieee, char out[24])
{
    (void)zsrc_zb_ieee_str(ieee, out, 24);
}

/* A registry id: 1..65535. */
static bool parse_id(const char *s, uint16_t *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0u || v > UINT16_MAX) {
        printf("a device is its id (1..65535, `zigbee devices`)\n");
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static void print_zones(const uint16_t *zones, uint8_t count)
{
    if (count == 0u) {
        printf("-");
        return;
    }
    for (uint8_t z = 0u; z < count && z < UI_ZB_ZONES_MAX; z++) {
        printf("%s%u", z == 0u ? "" : ",", (unsigned)zones[z]);
    }
}

static void print_progress(void)
{
    zigbee_service_ui_progress(&s_progress);
    const ui_zb_progress_t *g = &s_progress;
    printf("operation %s: %s", zbsvc_word(ZBSVC_W_OP, g->op), zbsvc_word(ZBSVC_W_STEP, g->step));
    if (g->step == (uint8_t)UI_ZB_STEP_DONE) {
        printf(" (%s)", zbsvc_word(ZBSVC_W_RESULT, g->result));
    }
    if (g->seconds_left != 0u) {
        printf(", %u s left", (unsigned)g->seconds_left);
    }
    if (g->id != 0u) {
        printf(", device %u", (unsigned)g->id);
    }
    if (g->old_id != 0u) {
        printf(" replacing %u", (unsigned)g->old_id);
    }
    if (g->zone_count != 0u) {
        printf(", zones ");
        print_zones(g->zones, g->zone_count);
    }
    printf("\n");
    for (uint8_t i = 0u; i < g->joined_count && i < UI_ZB_JOINED_MAX; i++) {
        printf("  joined %u: %s %s%s\n", (unsigned)g->joined[i].id,
               zbsvc_word(ZBSVC_W_STEP, g->joined[i].step),
               zbsvc_word(ZBSVC_W_RESULT, g->joined[i].result),
               g->joined[i].asleep ? " (asleep: wake it)" : "");
    }
}

static void print_status(void)
{
    zigbee_service_status_t status;
    zigbee_service_status(&status);
    printf("%s\n", status.words);
    if (!status.enabled) {
        return;
    }
    printf("ready=%u update=%u%% version=%s devices=%u channel=%u pan=%04x jammed=%u\n",
           (unsigned)status.ready, (unsigned)status.percent, status.version,
           (unsigned)status.devices, (unsigned)status.channel, (unsigned)status.pan_id,
           (unsigned)status.jammed);
    if (status.energy_known) {
        printf("energy=%d dBm (jammed at %d dBm held %u s)\n", (int)status.energy_dbm,
               (int)ZSRC_ZB_JAM_DBM, (unsigned)(ZSRC_ZB_JAM_HOLD_MS / 1000u));
    }
    if (status.radio_fails != 0u) {
        radio_port_stop_t why = RADIO_PORT_STOP_NONE;
        (void)radio_port_paused(NULL, &why);
        printf("the radio ended %u start%s in a row (%s)%s\n", (unsigned)status.radio_fails,
               status.radio_fails == 1u ? "" : "s", radio_port_stop_words(why),
               status.paused ? ": paused, 'zigbee retry' tries again" : "");
    }
    ui_zb_status_t st;
    if (zigbee_service_ui_status(&st)) {
        printf("radio %s%s%s, %u device%s (%u not heard), %u install code%s waiting%s",
               zbsvc_word(ZBSVC_W_RADIO, st.radio), st.radio == (uint8_t)UI_ZB_RADIO_PAUSED ? " " : "",
               st.radio == (uint8_t)UI_ZB_RADIO_PAUSED ? zbsvc_word(ZBSVC_W_PAUSE, st.paused_why)
                                                       : "",
               (unsigned)st.devices, st.devices == 1u ? "" : "s", (unsigned)st.not_heard,
               (unsigned)st.codes_waiting, st.codes_waiting == 1u ? "" : "s",
               st.storage_full ? ", storage full" : "");
        if (st.learn_s != 0u) {
            printf(", listening %u s", (unsigned)st.learn_s);
        }
        printf("\n");
    }
    zbsvc_stats_t s;
    zbsvc_stats(&s);
    /* the zigbee task's stack high-water mark (devices.md §16): bytes it never used */
    printf("task stack %u of %u bytes never used; nvs free entries %u; stores %u (%u failed); "
           "events %u (%u failed); requests %u (%u timed out); keypad posts %u (%u late); "
           "radio events dropped %u, alarm events lost %u\n",
           (unsigned)s.stack_free, (unsigned)s.stack_size, (unsigned)s.nvs_free,
           (unsigned)s.stores, (unsigned)s.store_failures, (unsigned)s.events,
           (unsigned)s.events_failed, (unsigned)s.requests, (unsigned)s.request_timeouts,
           (unsigned)s.ace_posts, (unsigned)s.ace_failed, (unsigned)s.radio_dropped,
           (unsigned)s.alarm_lost);
    print_progress();
}

/* One line per device: id, address, model, state, signal, battery, zones (devices.md §12). */
static void print_devices(void)
{
    const size_t n = zigbee_service_ui_devices(s_list, UI_ZB_DEVICES_MAX);
    for (size_t i = 0; i < n && i < UI_ZB_DEVICES_MAX; i++) {
        ui_zb_device_t *d = &s_list[i];
        d->name[UI_ZB_NAME_LEN - 1u] = '\0';
        d->model[UI_ZB_MODEL_LEN - 1u] = '\0';
        char ieee[24] = "?";
        if (zigbee_service_ui_device(d->id, &s_page)) {
            ieee_print(s_page.ieee, ieee);
        }
        printf("%3u %s %-24s %-12s %-6s ", (unsigned)d->id, ieee,
               d->name[0] != '\0' ? d->name : (d->model[0] != '\0' ? d->model : "?"),
               zbsvc_word(ZBSVC_W_DEVICE, d->state), zbsvc_word(ZBSVC_W_SIGNAL, d->signal));
        if (d->battery_pct == UI_ZB_BATTERY_UNKNOWN) {
            printf("battery ?   ");
        } else {
            printf("battery %3u%%%s", (unsigned)d->battery_pct, d->battery_low ? "!" : " ");
        }
        printf(" zones ");
        print_zones(d->zones, d->zone_count);
        printf("%s\n", d->unreachable ? " (not heard)" : "");
    }
    if (n == 0u) {
        printf("no Zigbee devices\n");
    }
}

static int print_device(uint16_t id)
{
    if (!zigbee_service_ui_device(id, &s_page)) {
        printf("no device %u\n", (unsigned)id);
        return 1;
    }
    ui_zb_device_page_t *p = &s_page;
    char ieee[24];
    ieee_print(p->ieee, ieee);
    p->manufacturer[UI_ZB_MODEL_LEN - 1u] = '\0';
    p->model_id[UI_ZB_MODEL_LEN - 1u] = '\0';
    p->sw_build[sizeof(p->sw_build) - 1u] = '\0';
    p->note[UI_ZB_WORDS_LEN - 1u] = '\0';
    p->dev.model[UI_ZB_MODEL_LEN - 1u] = '\0';
    p->dev.name[UI_ZB_NAME_LEN - 1u] = '\0';
    printf("device %u %s nwk %04x: %s (%s), \"%s\"\n", (unsigned)id, ieee, (unsigned)p->nwk,
           p->dev.model, zbsvc_word(ZBSVC_W_KIND, p->dev.kind), p->dev.name);
    printf("reports %s / %s, software %s; %s, %s%s; joined %s\n",
           p->manufacturer[0] != '\0' ? p->manufacturer : "?",
           p->model_id[0] != '\0' ? p->model_id : "?", p->sw_build[0] != '\0' ? p->sw_build : "?",
           zbsvc_word(ZBSVC_W_POWER, p->dev.power), p->sleepy ? "sleepy" : "awake",
           p->dev.router ? ", router" : "", zbsvc_word(ZBSVC_W_JOIN, p->dev.join));
    printf("state %s, setup %u/%u (step %u), signal %s (lqi %u rssi %d), check-in %u s%s\n",
           zbsvc_word(ZBSVC_W_DEVICE, p->dev.state), (unsigned)p->dev.setup_done,
           (unsigned)p->dev.setup_total, (unsigned)p->setup_step,
           zbsvc_word(ZBSVC_W_SIGNAL, p->dev.signal), (unsigned)p->lqi, (int)p->rssi,
           (unsigned)p->check_in_s, p->dev.unreachable ? ", not heard" : "");
    if (p->note[0] != '\0') {
        printf("note: %s\n", p->note);
    }
    for (uint8_t i = 0u; i < p->input_count && i < UI_ZB_INPUTS_MAX; i++) {
        ui_zb_input_t *in = &p->inputs[i];
        in->words[sizeof(in->words) - 1u] = '\0';
        in->state[sizeof(in->state) - 1u] = '\0';
        printf("  input ep %u: %s [%s] zone %u\n", (unsigned)in->endpoint, in->words,
               in->state[0] != '\0' ? in->state : "?", (unsigned)in->zone);
    }
    return 0;
}

static void print_codes(void)
{
    const size_t n = zigbee_service_ui_codes(s_codes, UI_ZB_CODES_MAX);
    for (size_t i = 0; i < n && i < UI_ZB_CODES_MAX; i++) {
        char ieee[24];
        ieee_print(s_codes[i].ieee, ieee);
        printf("%s waiting since %lu\n", ieee, (unsigned long)s_codes[i].added_at);
    }
    if (n == 0u) {
        printf("no install codes waiting\n");
    }
}

/* The request as the panel (user 0), every partition disarmed deciding `allowed`. */
static int submit(void)
{
    const ui_zb_req_result_t r = zbsvc_submit(&s_req, zone_service_all_disarmed());
    memset(&s_req, 0, sizeof(s_req)); /* an install code may be in it */
    printf("%s\n", zbsvc_word(ZBSVC_W_ANSWER, (uint8_t)r));
    if (r == UI_ZB_REQ_NOT_ALLOWED) {
        printf("every partition must be disarmed\n");
    }
    return r == UI_ZB_REQ_OK ? 0 : 1;
}

static int usage(void)
{
    printf("%s\n", USAGE);
    return 1;
}

static bool is(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static int cmd_zigbee(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    memset(&s_req, 0, sizeof(s_req));
    if (is(sub, "status") && argc <= 2) {
        print_status();
        return 0;
    }
    if (is(sub, "retry") && argc == 2) {
        if (zbsvc_stack_up()) {
            /* ZB21: stopping a running stack leaves the Zigbee sensors unheard for seconds, so
             * it is the RESTART request, under the binding gate: every partition disarmed. */
            s_req.kind = UI_ZB_REQ_RESTART;
            return submit();
        }
        zigbee_service_retry(); /* a paused or stopped radio: lifts the pause, reads again */
        printf("trying the sensor radio again\n");
        return 0;
    }
    if (!zbsvc_started()) {
        printf("%s\n", "the sensor radio service is not running");
        return 1;
    }
    if (is(sub, "devices") && argc == 2) {
        print_devices();
        return 0;
    }
    if (is(sub, "device") && argc == 3) {
        uint16_t id = 0u;
        return parse_id(argv[2], &id) ? print_device(id) : 1;
    }
    if (is(sub, "device") && argc == 4 && is(argv[3], "signature")) {
        uint16_t id = 0u; /* the device as a fixture row (testing.md §2) */
        return parse_id(argv[2], &id) ? zbsvc_print_signature(id) : 1;
    }
    if (is(sub, "trace") && argc == 3 && (is(argv[2], "on") || is(argv[2], "off"))) {
        zbsvc_trace_set(is(argv[2], "on")); /* the exchanges for a replay (testing.md §7) */
        printf("zigbee trace %s\n", zbsvc_trace_on() ? "on: zbtrace lines on this console" : "off");
        return 0;
    }
    if (is(sub, "codes") && argc == 2) {
        print_codes();
        return 0;
    }
    if (is(sub, "progress") && argc == 2) {
        print_progress();
        return 0;
    }
    /* "pair" is the earlier name of add / stop */
    if ((is(sub, "add") && argc == 2) || (is(sub, "pair") && argc == 2)) {
        s_req.kind = UI_ZB_REQ_ADD;
        return submit();
    }
    if ((is(sub, "stop") && argc == 2) || (is(sub, "pair") && argc == 3 && is(argv[2], "off"))) {
        s_req.kind = UI_ZB_REQ_STOP;
        return submit();
    }
    static const struct {
        const char *word;
        uint8_t kind;
    } SIMPLE[] = {{"keep", UI_ZB_REQ_KEEP_LISTENING}, {"survey", UI_ZB_REQ_SURVEY},
                  {"restart", UI_ZB_REQ_RESTART},     {"dismiss", UI_ZB_REQ_DISMISS}};
    for (size_t i = 0; i < sizeof(SIMPLE) / sizeof(SIMPLE[0]); i++) {
        if (is(sub, SIMPLE[i].word) && argc == 2) {
            s_req.kind = SIMPLE[i].kind;
            return submit();
        }
    }
    static const struct {
        const char *word;
        uint8_t kind;
        bool confirm;
    } BY_ID[] = {{"remove", UI_ZB_REQ_REMOVE, false},     {"forget", UI_ZB_REQ_FORGET, true},
                 {"replace", UI_ZB_REQ_REPLACE, false},   {"identify", UI_ZB_REQ_IDENTIFY, false},
                 {"ping", UI_ZB_REQ_PING, false},         {"setup", UI_ZB_REQ_SETUP, false}};
    for (size_t i = 0; i < sizeof(BY_ID) / sizeof(BY_ID[0]); i++) {
        if (!is(sub, BY_ID[i].word)) {
            continue;
        }
        if (argc != (BY_ID[i].confirm ? 4 : 3) || (BY_ID[i].confirm && !is(argv[3], "confirm"))) {
            printf("zigbee %s <id>%s\n", BY_ID[i].word, BY_ID[i].confirm ? " confirm" : "");
            return 1;
        }
        if (!parse_id(argv[2], &s_req.id)) {
            return 1;
        }
        s_req.kind = BY_ID[i].kind;
        return submit();
    }
    if (is(sub, "rename") && (argc == 3 || argc == 4)) {
        if (!parse_id(argv[2], &s_req.id)) {
            return 1;
        }
        if (argc == 4 && strlen(argv[3]) >= sizeof(s_req.name)) {
            printf("a name is at most %u characters\n", (unsigned)(sizeof(s_req.name) - 1u));
            return 1;
        }
        s_req.kind = UI_ZB_REQ_RENAME;
        if (argc == 4) {
            memcpy(s_req.name, argv[3], strlen(argv[3]) + 1u); /* "" gives it its default */
        }
        return submit();
    }
    if (is(sub, "code") && argc == 4) {
        const bool forget = is(argv[2], "forget");
        if (!zbsvc_parse_ieee(argv[forget ? 3 : 2], &s_req.ieee)) {
            printf("the device's address is 16 hex digits (00:11:22:33:44:55:66:77)\n");
            return 1;
        }
        if (forget) {
            s_req.kind = UI_ZB_REQ_CODE_FORGET;
        } else {
            if (strlen(argv[3]) >= sizeof(s_req.code)) {
                printf("that install code is too long\n");
                return 1;
            }
            s_req.kind = UI_ZB_REQ_CODE_ADD; /* kept until its device joins; the panel checks it */
            memcpy(s_req.code, argv[3], strlen(argv[3]) + 1u);
        }
        return submit();
    }
    if (is(sub, "channel") && argc == 4 && is(argv[3], "confirm")) {
        char *end = NULL;
        const unsigned long ch = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || ch < ALARM_ZB_CHANNEL_MIN ||
            ch > ALARM_ZB_CHANNEL_MAX) {
            printf("a channel is 11..26 (the panel offers 11, 15, 20, 25)\n");
            return 1;
        }
        s_req.kind = UI_ZB_REQ_CHANNEL;
        s_req.channel = (uint8_t)ch;
        return submit();
    }
    if (is(sub, "reset") && argc == 4 && is(argv[2], "confirm")) {
        /* the installer code, checked as the console's keypad source: never kept, never echoed */
        char code[ALARM_CODE_MAX_LEN + 1];
        memset(code, 0, sizeof(code));
        const size_t len = strlen(argv[3]);
        if (len <= ALARM_CODE_MAX_LEN) {
            memcpy(code, argv[3], len);
        }
        memset(argv[3], 0, len);
        const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_CONSOLE};
        const ui_zb_req_result_t r = zbsvc_reset_code(&src, code);
        if (r != UI_ZB_REQ_OK) {
            printf("%s\n", zbsvc_word(ZBSVC_W_ANSWER, (uint8_t)r));
            return 1;
        }
        s_req.kind = UI_ZB_REQ_RESET;
        return submit();
    }
    if (is(sub, "forget") || is(sub, "channel") || is(sub, "reset")) {
        printf("zigbee forget <id> confirm | channel <n> confirm | reset confirm <installer code>\n");
        return 1;
    }
    return usage();
}

#else /* CONFIG_ALARM_ZSRC_ZIGBEE */

static int cmd_zigbee(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    zigbee_service_status_t status;
    zigbee_service_status(&status);
    printf("%s\n", status.words);
    return 0;
}

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */

esp_err_t zigbee_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "zigbee",
        .help = "Sensor radio and its Zigbee devices: status, devices, adding, removing, install "
                "codes, the channel; changes with every partition disarmed",
        .hint = USAGE,
        .func = cmd_zigbee};
    return esp_console_cmd_register(&cmd);
}
