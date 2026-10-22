/*
 * panel_console.c - serial service console. The "keys" command is a virtual keypad that
 * runs the same key-sequence parser as the touchscreen (ALARM_KEYPAD_CONSOLE source).
 */
#include "panel_console.h"
#include "ui_backend/ui_check_words.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm/alarm_keyseq.h"
#include "alarm/alarm_strings.h"
#include "alarm_service.h"
#include "code_hash.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_check.h"
#include "net_wifi.h"
#include "panel_log.h"
#include "panel_system.h"
#include "sdkconfig.h"
#include "sounder.h"
#include "zone_hub/zsrc_rf.h"
#include "zone_service.h"
#include "zone_hub/zone_eol.h"

static const char *TAG = "console";

static const alarm_source_t SRC = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_CONSOLE};
static alarm_keyseq_t s_ks;

static alarm_config_t *get_config(void)
{
    alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_DEFAULT);
    if (cfg != NULL && alarm_service_get_config(cfg) != ESP_OK) {
        heap_caps_free(cfg);
        cfg = NULL;
    }
    if (cfg == NULL) {
        printf("configuration unavailable\n");
    }
    return cfg;
}

static void print_result(alarm_result_t r, const alarm_cmd_result_t *cr)
{
    if (r == ALARM_OK) {
        if (cr != NULL && cr->user != 0u) {
            printf("OK (user %u)\n", cr->user);
        } else {
            printf("OK\n");
        }
        return;
    }
    printf("ERROR %s", alarm_result_name(r));
    if (cr != NULL && cr->not_ready_zone != 0u) {
        printf(" (zone %u)", cr->not_ready_zone);
    }
    printf("\n");
}

static void print_bits(const char *label, uint32_t bits, const char *(*name)(int))
{
    if (bits == 0u) {
        return;
    }
    printf("  %s:", label);
    for (int i = 0; i < 32; i++) {
        if (bits & (1u << i)) {
            printf(" %s", name(i));
        }
    }
    printf("\n");
}

static const char *sys_trouble_name(int i)
{
    return alarm_sys_trouble_name((alarm_sys_trouble_t)i);
}

static const char *cause_name(int i)
{
    return (i == 15) ? "canceled" : alarm_cause_name((alarm_cause_t)i);
}

static void print_zone_flags(uint16_t f, uint8_t trouble)
{
    static const char *const names[] = {"fault",  "bypassed", "alarm",     "trouble",
                                        "tamper", "low_battery", "supervision", "swinger",
                                        "verifying", "not_ready", "trouble_memory"};
    if (f == 0u) {
        printf(" normal");
    }
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (f & (1u << i)) {
            printf(" %s", names[i]);
        }
    }
    if (trouble != 0u) {
        printf(" [");
        for (int i = 0; i < ALARM_ZTRBL_COUNT; i++) {
            if (trouble & (1u << i)) {
                printf(" %s", alarm_zone_trouble_name((alarm_zone_trouble_t)i));
            }
        }
        printf(" ]");
    }
}

/* Records the event log could not keep (LOG-010). They latch in NVS, so this keeps saying so
 * after a restart until someone acknowledges it with "events clear_loss". */
static void print_log_loss(void)
{
    uint32_t lost = 0;
    uint32_t lost_reports = 0;
    panel_log_loss(&lost, &lost_reports);
    if (lost != 0u) {
        printf("  %" PRIu32 " event(s) never written, %" PRIu32 " report(s) never delivered "
               "(\"events clear_loss\" to acknowledge)\n",
               lost, lost_reports);
    }
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_DEFAULT);
    alarm_config_t *cfg = get_config();
    if (s == NULL || cfg == NULL) {
        heap_caps_free(s);
        heap_caps_free(cfg);
        return 1;
    }
    alarm_service_get_snapshot(s);
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!(s->partitions & (1u << (p - 1u)))) {
            continue;
        }
        const alarm_partition_status_t *ps = &s->part[p - 1u];
        printf("Partition %u \"%s\": %s", p, cfg->partitions[p - 1u].name,
               alarm_state_name(ps->state));
        if (ps->level != ALARM_LEVEL_NONE) {
            printf(" %s%s%s", alarm_level_name(ps->level),
                   (ps->arm_flags & ALARM_ARM_NO_ENTRY_DELAY) ? " no_entry_delay" : "",
                   (ps->arm_flags & ALARM_ARM_SILENT_EXIT) ? " silent_exit" : "");
        }
        if (ps->delay_remaining_ms > 0u) {
            printf(" (%" PRIu64 " s left)", (ps->delay_remaining_ms + 999u) / 1000u);
        }
        printf("\n  %s, chime %s, sounder %s%s%s%s\n", ps->ready ? "READY" : "NOT READY",
               ps->chime ? "on" : "off", alarm_sound_name(ps->sounder),
               ps->triggered ? ", TRIGGERED" : "", ps->walk_test ? ", WALK TEST" : "",
               ps->trouble_unack ? ", troubles unacknowledged" : "");
        print_bits("alarm memory", ps->memory, cause_name);
    }
    print_bits("system troubles", s->sys_troubles, sys_trouble_name);
    print_bits("restored troubles (code + OFF to clear)", s->sys_troubles_restored,
               sys_trouble_name);
    const esp_err_t store = alarm_service_store_status();
    if (store == ESP_ERR_NOT_FINISHED) {
        printf("  a change is still on its way to flash\n");
    } else if (store == ESP_ERR_INVALID_STATE) {
        printf("  stored settings this firmware could not use are still there; the panel is "
               "running on the factory ones\n");
    } else if (store != ESP_OK) {
        printf("  a change could not be saved: %s (it is retried)\n", esp_err_to_name(store));
    }
    print_log_loss();
    if (s->dev_troubles != 0u) {
        printf("  missing devices: 0x%08" PRIx32 "\n", s->dev_troubles);
    }
    if (s->default_codes != 0u) {
        printf("  FACTORY CODES IN USE:%s%s\n",
               (s->default_codes & ALARM_DEFAULT_CODE_INSTALLER) ? " installer(69420)" : "",
               (s->default_codes & ALARM_DEFAULT_CODE_MASTER) ? " master(123456)" : "");
    }
    if (sounder_silenced()) {
        printf("  SPEAKER OFF (bench silence): this panel's speaker stays silent, alarms included\n");
    }
    bool header = false;
    for (uint16_t z = 1; z <= s->zones_used; z++) {
        const alarm_zone_status_t *zs = &s->zone[z - 1u];
        if (cfg->zones[z - 1u].type == ALARM_ZT_NOT_USED ||
            (zs->flags & (uint16_t)~ALARM_ZS_NOT_READY) == 0u) {
            continue;
        }
        if (!header) {
            printf("Zones not normal:\n");
            header = true;
        }
        printf("  %3u %-24s", z, cfg->zones[z - 1u].name);
        print_zone_flags(zs->flags, zs->trouble);
        printf("\n");
    }
    net_wifi_status_t ns;
    net_wifi_get_status(&ns);
    static const char *const nstate[] = {"starting", "no co-processor", "not configured",
                                         "connecting", "connected", "disconnected"};
    printf("Network: %s %s %s", nstate[ns.state], ns.ssid, ns.ip);
    if (ns.state == NET_WIFI_CONNECTED) {
        printf(" (%d dBm, %s)", ns.rssi, ns.static_ip ? "static" : "dhcp");
    }
    printf(", time %s", ns.time_synced ? "synced" : "not set");
    if (ns.coproc_fw[0] != '\0') {
        printf(", co-processor %s", ns.coproc_fw);
    }
    printf("\n");
    heap_caps_free(s);
    heap_caps_free(cfg);
    return 0;
}

/* "simulator", "wired GPIO 4 on the J7 header", "no sensor", plus CHECK and link notes. */
/* One source's line from the hub, NULL when it isn't registered. */
static const zone_hub_source_info_t *source_find(uint8_t kind)
{
    static zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES]; /* console task only */
    const size_t n = zone_service_sources(list, ZONE_HUB_MAX_SOURCES);
    for (size_t i = 0; i < n; i++) {
        if (list[i].kind == kind) {
            return &list[i];
        }
    }
    return NULL;
}

static const char *source_name(uint8_t kind)
{
    const zone_hub_source_info_t *src = source_find(kind);
    return src != NULL ? src->name : "?";
}

static void print_source(uint16_t z)
{
    zone_hub_info_t info;
    if (!zone_service_info(z, &info)) {
        printf("no sensor");
        return;
    }
    const zsrc_address_t *a = &info.binding.in[ZSRC_ROLE_MAIN].addr;
    if (info.lost) {
        printf("lost: its stored settings couldn't be read [CHECK]");
        return;
    }
    if (!info.source_present) {
        printf("source %u (not in this firmware)", a->kind);
    } else if (a->kind == ZSRC_SIM) {
        printf("simulator");
    } else {
        printf("%s, %s", source_name(a->kind), info.address);
        const zsrc_mapping_t *m = &info.binding.in[ZSRC_ROLE_MAIN].map;
        if (a->kind == ZSRC_WIRED) {
            printf(" (%s, %u ms)", m->invert ? "normally open" : "normally closed",
                   m->response_ms != 0u ? m->response_ms : ZSRC_WIRED_RESPONSE_MS);
        }
        /* the tamper and low-battery inputs too: which code each one took (RF-021) */
        for (uint8_t r = ZSRC_ROLE_TAMPER; r < ZSRC_ROLE_COUNT; r++) {
            const zsrc_input_t *in = &info.binding.in[r];
            if (in->addr.kind == ZSRC_NONE) {
                continue;
            }
            const char *role = r == ZSRC_ROLE_TAMPER ? "tamper" : "battery";
            uint8_t proto;
            uint32_t device;
            uint8_t data;
            if (zsrc_rf_address_parts(&in->addr, &proto, &device, &data)) {
                char words[48];
                zsrc_rf_code_words(proto, device, data, words, sizeof(words));
                printf("; %s: %s", role, words);
                if (in->map.hold_s != 0u) {
                    printf(" (hold %u s)", (unsigned)in->map.hold_s);
                }
            } else {
                printf("; %s: source %u input %u", role, (unsigned)in->addr.kind,
                       (unsigned)in->addr.input);
            }
        }
    }
    if (info.check) {
        printf(" [CHECK: not reachable]");
    } else if (info.unreachable) {
        printf(" [not reachable, grace time running]");
    }
    if (info.link_down) {
        /* which source: its one sensor_radio trouble doesn't say (BL-055) */
        const zone_hub_source_info_t *src = source_find(info.binding.in[ZSRC_ROLE_MAIN].addr.kind);
        printf(" [%s link down%s]", src != NULL ? src->name : "source",
               src != NULL && src->trouble ? ": sensor_radio trouble raised" : "");
    }
}

static int cmd_zones(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_DEFAULT);
    alarm_config_t *cfg = get_config();
    if (s == NULL || cfg == NULL) {
        heap_caps_free(s);
        heap_caps_free(cfg);
        return 1;
    }
    alarm_service_get_snapshot(s);
    for (uint16_t z = 1; z <= s->zones_used; z++) {
        const alarm_zone_config_t *zc = &cfg->zones[z - 1u];
        if (zc->type == ALARM_ZT_NOT_USED) {
            continue;
        }
        printf("%3u p%u %-24s %-18s %-12s", z, zc->partition, zc->name,
               alarm_zone_type_name(zc->type),
               alarm_device_class_name((alarm_device_class_t)zc->device_class));
        print_zone_flags(s->zone[z - 1u].flags, s->zone[z - 1u].trouble);
        printf("\n      sensor: ");
        print_source(z);
        printf("\n");
    }
    heap_caps_free(s);
    heap_caps_free(cfg);
    return 0;
}

static const struct {
    const char *name;
    alarm_zone_event_t ev;
} ZONE_EVENTS[] = {
    {"open", ALARM_ZEV_FAULT},         {"fault", ALARM_ZEV_FAULT},
    {"close", ALARM_ZEV_RESTORE},      {"restore", ALARM_ZEV_RESTORE},
    {"trouble", ALARM_ZEV_TROUBLE},    {"trouble_ok", ALARM_ZEV_TROUBLE_RESTORE},
    {"tamper", ALARM_ZEV_TAMPER},      {"tamper_ok", ALARM_ZEV_TAMPER_RESTORE},
    {"lowbat", ALARM_ZEV_LOW_BATTERY}, {"lowbat_ok", ALARM_ZEV_LOW_BATTERY_RESTORE},
    {"checkin", ALARM_ZEV_CHECKIN},    {"unreachable", ALARM_ZEV_UNREACHABLE},
    {"reachable", ALARM_ZEV_REACHABLE}, {"test", ALARM_ZEV_TEST},
};

static void zone_usage(void)
{
    printf("usage: zone <n> <event>   simulate the sensor of a simulator zone; events: open close "
           "trouble trouble_ok\n"
           "                          tamper tamper_ok lowbat lowbat_ok checkin unreachable "
           "reachable test\n"
           "       zone <n> source   where zone n's sensor is\n"
           "       zone <n> bind sim  drive zone n from here (simulator input n)\n"
           "       zone <n> bind wired <gpio> [nc|no|eol-nc|eol-no|deol] [10|350|700|1200]   a "
           "contact on a terminal (see 'terminals'; eol-*/deol: a 2k end-of-line loop with a "
           "4.7k pull-up, on a terminal that measures)\n"
           "       zone <n> bind ha <entity_id>   a sensor Home Assistant shares with the "
           "panel\n"
           "       zone <n> bind keypad <k> <gpio> [no]   a contact on link keypad k's own "
           "terminal\n"
           "       zone <n> unbind    no sensor\n"
           "Binding needs every partition disarmed.\n");
}

/* The Home Assistant sensor the touchscreen's search would offer for this entity_id, mapped as
 * its picker maps one for a zone of this device class (ui_sources.c bind()). */
static bool ha_input(uint16_t z, const char *entity, zone_hub_binding_t *b)
{
    enum { HA_FOUND_MAX = 8 }; /* a search by entity_id finds it and the few it is part of */
    zsrc_candidate_t *c = heap_caps_malloc(HA_FOUND_MAX * sizeof(*c), MALLOC_CAP_DEFAULT);
    alarm_config_t *cfg = get_config();
    bool found = false;
    if (c != NULL && cfg != NULL) {
        const size_t n = zone_service_candidates(ZSRC_HA, entity, 0, c, HA_FOUND_MAX);
        for (size_t i = 0; i < n && !found; i++) {
            if (strcmp(c[i].addr.ref, entity) == 0) {
                b->in[ZSRC_ROLE_MAIN].addr = c[i].addr;
                zsrc_mapping_defaults(&b->in[ZSRC_ROLE_MAIN].map,
                                      (alarm_device_class_t)cfg->zones[z - 1u].device_class);
                found = true;
            }
        }
        if (!found) {
            printf("%s is not a sensor Home Assistant shares with the panel\n", entity);
        }
    }
    heap_caps_free(c);
    heap_caps_free(cfg);
    return found;
}

static int zone_bind(uint16_t z, int argc, char **argv)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    if (argc >= 4 && strcmp(argv[3], "sim") == 0) {
        b.in[ZSRC_ROLE_MAIN].addr = zsrc_address(ZSRC_SIM, 0u, z);
    } else if (argc >= 5 && strcmp(argv[3], "wired") == 0) {
        b.in[ZSRC_ROLE_MAIN].addr = zsrc_address(ZSRC_WIRED, 0u, strtoul(argv[4], NULL, 10));
        zsrc_mapping_t *m = &b.in[ZSRC_ROLE_MAIN].map;
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "no") == 0) {
                m->invert = true;
            } else if (strcmp(argv[i], "eol-nc") == 0) { /* ZONE-005: end-of-line loops */
                b.in[ZSRC_ROLE_MAIN].addr.input = ZEOL_IN_NC;
            } else if (strcmp(argv[i], "eol-no") == 0) {
                b.in[ZSRC_ROLE_MAIN].addr.input = ZEOL_IN_NO;
            } else if (strcmp(argv[i], "deol") == 0) {
                b.in[ZSRC_ROLE_MAIN].addr.input = ZEOL_IN_DEOL;
            } else if (strcmp(argv[i], "nc") != 0) {
                const unsigned long ms = strtoul(argv[i], NULL, 10);
                if (ms != 10u && ms != 350u && ms != 700u && ms != 1200u) {
                    printf("response time: 10, 350, 700 or 1200 ms\n");
                    return 1;
                }
                m->response_ms = (uint16_t)ms;
            }
        }
    } else if (argc >= 6 && strcmp(argv[3], "keypad") == 0) {
        /* A keypad's own terminal (docs/features/11 LINK-055): keypad number, GPIO. */
        const unsigned long kp = strtoul(argv[4], NULL, 10), gpio = strtoul(argv[5], NULL, 10);
        if (kp < 1u || kp > 8u || gpio > 63u) {
            printf("keypad 1..8, GPIO 0..63\n");
            return 1;
        }
        b.in[ZSRC_ROLE_MAIN].addr = zsrc_address(ZSRC_KEYPAD, 0u, (kp << 8) | gpio);
        b.in[ZSRC_ROLE_MAIN].map.invert = argc >= 7 && strcmp(argv[6], "no") == 0;
    } else if (argc >= 5 && strcmp(argv[3], "ha") == 0) {
        if (!ha_input(z, argv[4], &b)) {
            return 1;
        }
    } else {
        zone_usage();
        return 1;
    }
    const zone_hub_result_t r = zone_service_bind(z, &b);
    if (r != ZONE_HUB_OK) {
        printf("ERROR %s\n", zone_service_result_name(r));
        return 1;
    }
    printf("zone %u: ", z);
    print_source(z);
    printf("\n");
    return 0;
}

static int cmd_zone(int argc, char **argv)
{
    if (argc < 3) {
        zone_usage();
        return 1;
    }
    const long zl = strtol(argv[1], NULL, 10);
    if (zl < 1 || zl > ALARM_MAX_ZONES) {
        printf("zone must be 1..%d\n", ALARM_MAX_ZONES);
        return 1;
    }
    const uint16_t z = (uint16_t)zl;
    if (strcmp(argv[2], "source") == 0) {
        printf("zone %u: ", z);
        print_source(z);
        printf("\n");
        return 0;
    }
    if (strcmp(argv[2], "bind") == 0) {
        return zone_bind(z, argc, argv);
    }
    if (strcmp(argv[2], "unbind") == 0) {
        const zone_hub_result_t r = zone_service_unbind(z);
        printf("%s\n", r == ZONE_HUB_OK ? "OK" : zone_service_result_name(r));
        return r == ZONE_HUB_OK ? 0 : 1;
    }
    for (size_t i = 0; i < sizeof(ZONE_EVENTS) / sizeof(ZONE_EVENTS[0]); i++) {
        if (strcmp(argv[2], ZONE_EVENTS[i].name) != 0) {
            continue;
        }
        zone_hub_binding_t b;
        if (zone_service_get(z, &b) && b.in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_SIM &&
            zone_service_sim((uint16_t)b.in[ZSRC_ROLE_MAIN].addr.id, ZONE_EVENTS[i].ev)) {
            return 0;
        }
        printf("zone %u isn't a simulator zone (", z);
        print_source(z);
        printf("): its events come from its sensor. 'zone %u bind sim' drives it from here.\n", z);
        return 1;
    }
    printf("unknown zone event '%s'\n", argv[2]);
    zone_usage();
    return 1;
}

static int cmd_sources(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *const caps[] = {"learn",   "search",  "supervised",  "authenticated",
                                       "tamper",  "battery", "offline",     "life-safety",
                                       "test"};
    zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
    const size_t n = zone_service_sources(list, ZONE_HUB_MAX_SOURCES);
    for (size_t i = 0; i < n; i++) {
        printf("%u %-14s %s%s:", list[i].kind, list[i].name, list[i].usable ? "usable" : "not usable",
               list[i].link_down ? (list[i].trouble ? ", link down (sensor_radio trouble raised)"
                                                    : ", link down (grace time running)")
                                 : "");
        for (unsigned b = 0; b < sizeof(caps) / sizeof(caps[0]); b++) {
            if (list[i].caps & (1u << b)) {
                printf(" %s", caps[b]);
            }
        }
        printf("\n");
    }
    return 0;
}

/* Every wired terminal with its level (read for a moment) and the zone that uses it. */
static int cmd_terminals(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    zsrc_candidate_t *c = heap_caps_malloc(ZSRC_WIRED_MAX * sizeof(*c), MALLOC_CAP_DEFAULT);
    const bool was_learning = zone_service_learning(ZSRC_WIRED); /* the zone editor may be open */
    if (c == NULL || !zone_service_learn(ZSRC_WIRED, true)) {
        heap_caps_free(c);
        printf("no wired terminals in this build\n");
        return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(500)); /* the terminals settle (the response time) */
    const size_t n = zone_service_candidates(ZSRC_WIRED, "", 0, c, ZSRC_WIRED_MAX);
    if (!was_learning) {
        zone_service_learn(ZSRC_WIRED, false);
    }
    for (size_t i = 0; i < n; i++) {
        printf("%-8s %-10s %-7s", c[i].name, c[i].detail, c[i].state);
        if (c[i].zone != 0u) {
            printf(" zone %u", c[i].zone);
        }
        printf("\n");
    }
    printf("an open loop reads open; wire each contact between its GPIO and GND (3.3 V logic)\n");
    heap_caps_free(c);
    return 0;
}

static void keypad_result(const alarm_keyseq_result_t *r)
{
    alarm_cmd_result_t cr = {0};
    switch (r->kind) {
    case ALARM_KS_COMMAND:
    case ALARM_KS_USER_SET:
    case ALARM_KS_USER_DELETE:
        print_result(alarm_service_keypad_execute(r, &SRC, &cr), &cr);
        break;
    case ALARM_KS_GOTO:
        s_ks.partition = r->partition;
        printf("keypad now on partition %u\n", r->partition);
        break;
    case ALARM_KS_STAR: {
        alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_DEFAULT);
        if (s == NULL) {
            break;
        }
        alarm_service_get_snapshot(s);
        const alarm_partition_status_t *ps = &s->part[s_ks.partition - 1u];
        if (ps->state == ALARM_STATE_ARMED &&
            (ps->level == ALARM_LEVEL_STAY || ps->level == ALARM_LEVEL_NIGHT)) {
            const alarm_cmd_t cmd = {.type = ALARM_CMD_EXIT_RESTART, .partition = s_ks.partition};
            print_result(alarm_service_command(&cmd, &SRC, &cr), &cr);
        } else {
            printf("faults:");
            for (uint16_t z = 1; z <= s->zones_used; z++) {
                if (s->zone[z - 1u].flags & ALARM_ZS_FAULT) {
                    printf(" %u", z);
                }
            }
            printf("\n");
        }
        heap_caps_free(s);
        break;
    }
    case ALARM_KS_ERROR:
    default:
        printf("key sequence error\n");
        break;
    }
}

static int cmd_keys(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: keys <keys>   digits, O=OFF W=AWAY S=STAY N=NIGHT M=MAX I=INSTANT "
               "T=TEST Y=BYPASS U=CODE H=CHIME * # A B C\n  e.g. keys 123456W   keys 123456O\n");
        return 1;
    }
    alarm_keyseq_tick(&s_ks, alarm_service_now());
    for (int a = 1; a < argc; a++) {
        for (const char *p = argv[a]; *p != '\0'; p++) {
            const char key = (char)toupper((unsigned char)*p);
            alarm_keyseq_result_t r;
            if (alarm_keyseq_key(&s_ks, key, alarm_service_now(), &r)) {
                keypad_result(&r);
            }
        }
    }
    if (alarm_keyseq_digits(&s_ks) > 0u) {
        printf("(%u digits entered, waiting for a command key)\n", alarm_keyseq_digits(&s_ks));
    }
    return 0;
}

static int cmd_trouble(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
        printf("usage: trouble <name> <on|off>\n  names:");
        for (int i = 0; i < ALARM_TRBL_COUNT; i++) {
            printf(" %s", alarm_sys_trouble_name((alarm_sys_trouble_t)i));
        }
        printf("\n");
        return 1;
    }
    for (int i = 0; i < ALARM_TRBL_COUNT; i++) {
        if (strcmp(argv[1], alarm_sys_trouble_name((alarm_sys_trouble_t)i)) == 0) {
            alarm_service_system_trouble((alarm_sys_trouble_t)i, strcmp(argv[2], "on") == 0);
            return 0;
        }
    }
    printf("unknown trouble '%s'\n", argv[1]);
    return 1;
}

/* "2026-09-19 22:41:05", or "boot 12 +3605s" while the clock was unknown. */
static void entry_time(const elog_entry_t *e, char *out, size_t len)
{
    if (e->ev.epoch_ms != 0) {
        const time_t t = (time_t)(e->ev.epoch_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(out, len, "boot %" PRIu32 " +%" PRIu32 "s", e->boot, e->uptime_s);
    }
}

static bool events_only(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_EVENT;
}

/* Newest n entries from the flash log, printed oldest first. */
static int cmd_events(int argc, char **argv)
{
    size_t n = 20;
    bool markers = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "clear_loss") == 0) {
            /* A service action: it drops the storage trouble that a lost record latched, so
             * only do it once the reason is understood (the count itself is not the fault). */
            uint32_t lost = 0;
            uint32_t lost_reports = 0;
            panel_log_loss(&lost, &lost_reports);
            panel_log_clear_loss();
            printf("acknowledged: %" PRIu32 " lost record(s), %" PRIu32 " undelivered "
                   "report(s)\n",
                   lost, lost_reports);
            return 0;
        }
        if (strcmp(argv[i], "all") == 0) {
            markers = true;
        } else {
            n = (size_t)strtoul(argv[i], NULL, 10);
        }
    }
    if (n == 0u || n > 500u) {
        n = 500u;
    }
    elog_entry_t *es = heap_caps_malloc(n * sizeof(*es), MALLOC_CAP_DEFAULT);
    if (es == NULL) {
        return 1;
    }
    n = panel_log_read(UINT32_MAX, markers ? NULL : events_only, NULL, es, n, 0, NULL);
    for (size_t i = n; i-- > 0;) {
        const elog_entry_t *en = &es[i];
        const alarm_event_t *e = &en->ev;
        char when[32];
        entry_time(en, when, sizeof(when));
        if (en->kind == ELOG_KIND_REPORTED) {
            printf("       %s  reported #%" PRIu32 "\n", when, e->seq);
            continue;
        }
        printf("#%-5" PRIu32 " %s  %-20s p%u z%-3u u%-2u arg %u/%u%s%s\n", e->seq, when,
               alarm_event_name(e->type), e->partition, e->zone, e->user, e->arg, e->arg2,
               (e->flags & ALARM_EVF_REPORT) ? " report" : "",
               (e->flags & ALARM_EVF_SILENT) ? " silent" : "");
    }
    uint32_t count = 0;
    uint32_t capacity = 0;
    panel_log_stats(&count, &capacity);
    printf("%u shown, %" PRIu32 " kept (keeps at least %" PRIu32 ")\n", (unsigned)n, count,
           capacity);
    print_log_loss();
    heap_caps_free(es);
    return 0;
}

/* The whole log as CSV, oldest first (docs/features/05 LOG-009). */
static int cmd_log_export(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    enum { PAGE = 32 };
    elog_entry_t *es = heap_caps_malloc(PAGE * sizeof(*es), MALLOC_CAP_DEFAULT);
    uint32_t count = 0;
    uint32_t capacity = 0;
    panel_log_stats(&count, &capacity);
    uint32_t *idx = heap_caps_malloc((count + 1u) * sizeof(*idx), MALLOC_CAP_DEFAULT);
    if (es == NULL || idx == NULL) {
        heap_caps_free(es);
        heap_caps_free(idx);
        return 1;
    }
    /* Collect indexes newest first, then print oldest first page by page. */
    size_t total = 0;
    uint32_t before = UINT32_MAX;
    while (total < count) {
        uint32_t next = 0;
        const size_t n = panel_log_read(before, NULL, NULL, es, PAGE, 0, &next);
        for (size_t i = 0; i < n && total < count; i++) {
            idx[total++] = es[i].index;
        }
        if (next == 0u || n == 0u) {
            break;
        }
        before = next;
    }
    printf("index,seq,kind,boot,uptime_s,epoch_ms,time,event,partition,zone,user,source,"
           "source_id,arg,arg2,flags\n");
    for (size_t i = total; i-- > 0;) {
        elog_entry_t en;
        if (panel_log_read(idx[i] + 1u, NULL, NULL, &en, 1, 0, NULL) != 1u) {
            continue;
        }
        char when[32];
        entry_time(&en, when, sizeof(when));
        const alarm_event_t *e = &en.ev;
        printf("%" PRIu32 ",%" PRIu32 ",%s,%" PRIu32 ",%" PRIu32 ",%" PRId64 ",%s,%s,%u,%u,%u,%u,"
               "%u,%u,%u,%u\n",
               en.index, e->seq, en.kind == ELOG_KIND_EVENT ? "event" : "reported", en.boot,
               en.uptime_s, e->epoch_ms, e->epoch_ms != 0 ? when : "", alarm_event_name(e->type),
               e->partition, e->zone, e->user, e->source.kind, e->source.id, e->arg, e->arg2,
               e->flags);
    }
    heap_caps_free(es);
    heap_caps_free(idx);
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        net_wifi_status_t ns;
        net_wifi_get_status(&ns);
        char ntp[64];
        net_wifi_get_ntp_server(ntp, sizeof(ntp));
        printf("state %d, ssid '%s', ip %s, mac %s, rssi %d, last failure %d, time %s (%s)\n",
               (int)ns.state, ns.ssid, ns.ip, ns.mac, ns.rssi, (int)ns.fail,
               ns.time_synced ? "synced" : "not set", ntp);
        return 0;
    }
    esp_err_t err;
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        err = net_wifi_scan_start();
        if (err == ESP_OK) {
            net_wifi_ap_t aps[NET_WIFI_SCAN_MAX];
            bool done = false;
            size_t n = 0;
            for (int i = 0; i < 100 && !done; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                n = net_wifi_scan_results(aps, NET_WIFI_SCAN_MAX, &done);
            }
            for (size_t i = 0; i < n; i++) {
                printf("  %4d dBm %s %s\n", aps[i].rssi, aps[i].secure ? "*" : " ", aps[i].ssid);
            }
            printf("%u networks\n", (unsigned)n);
            return 0;
        }
    } else if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        err = net_wifi_set_credentials("", NULL);
    } else {
        err = net_wifi_set_credentials(argv[1], argc > 2 ? argv[2] : NULL);
    }
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_ip(int argc, char **argv)
{
    net_ip_config_t c;
    net_wifi_get_ip_config(&c);
    if (argc == 1) {
        net_wifi_status_t ns;
        net_wifi_get_status(&ns);
        printf("setting: %s", c.use_static ? "static" : "dhcp");
        if (c.use_static) {
            printf(" %s %s gw %s", c.ip, c.mask, c.gateway);
        }
        printf(", dns %s %s\n", c.dns1[0] ? c.dns1 : "-", c.dns2[0] ? c.dns2 : "-");
        printf("in use:  %s %s gw %s, dns %s %s (%s)\n", ns.ip[0] ? ns.ip : "-",
               ns.mask[0] ? ns.mask : "-", ns.gateway[0] ? ns.gateway : "-",
               ns.dns1[0] ? ns.dns1 : "-", ns.dns2[0] ? ns.dns2 : "-",
               ns.static_ip ? "static" : "from the router");
        return 0;
    }
    if (strcmp(argv[1], "dhcp") == 0 && argc <= 4) {
        c.use_static = false;
    } else if (strcmp(argv[1], "static") == 0 && argc >= 5) {
        c.use_static = true;
        strlcpy(c.ip, argv[2], sizeof(c.ip));
        strlcpy(c.mask, argv[3], sizeof(c.mask));
        strlcpy(c.gateway, argv[4], sizeof(c.gateway));
    } else {
        printf("usage: ip [dhcp [<dns1> [<dns2>]] | static <ip> <mask> <gateway> "
               "[<dns1> [<dns2>]]]\n");
        return 1;
    }
    const int dns_at = c.use_static ? 5 : 2;
    c.dns1[0] = '\0';
    c.dns2[0] = '\0';
    if (argc > dns_at) {
        strlcpy(c.dns1, argv[dns_at], sizeof(c.dns1));
    }
    if (argc > dns_at + 1) {
        strlcpy(c.dns2, argv[dns_at + 1], sizeof(c.dns2));
    }
    const esp_err_t err = net_wifi_set_ip_config(&c);
    printf("%s\n", err == ESP_OK ? "saved, rejoining" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_netcheck(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *const NAMES[NET_CHECK_COUNT] = {
        "wifi", "address", "router", "lookup", "internet", "time", "service",
    };
    static const char *const STATES[] = {"waiting", "running", "pass", "FAIL", "warn", "skipped"};
    const esp_err_t err = net_check_start();
    if (err != ESP_OK) {
        printf("%s\n", err == ESP_ERR_INVALID_STATE ? "a check is already running"
                                                    : esp_err_to_name(err));
        return 1;
    }
    net_check_report_t rep;
    for (int i = 0; i < 600; i++) { /* the whole walk fits well inside a minute */
        vTaskDelay(pdMS_TO_TICKS(100));
        net_check_get(&rep);
        if (!rep.running) {
            break;
        }
    }
    net_check_get(&rep);
    int failed = 0;
    for (size_t i = 0; i < NET_CHECK_COUNT; i++) {
        const net_check_result_t *r = &rep.steps[i];
        failed += r->state == NET_CHECK_FAIL;
        char line[96];
        ui_check_line(line, sizeof(line), r->words, r->value[0], r->value[1]);
        printf("  %-9s %-7s %5u ms  %s\n", NAMES[i],
               r->state < sizeof(STATES) / sizeof(STATES[0]) ? STATES[r->state] : "?",
               (unsigned)r->ms, line);
    }
    return failed == 0 ? 0 : 1;
}

static int cmd_tz(int argc, char **argv)
{
    if (argc == 1) {
        char tz[64];
        net_wifi_get_timezone(tz, sizeof(tz));
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
        printf("TZ %s, local time %s\n", tz, buf);
        return 0;
    }
    const esp_err_t err = net_wifi_set_timezone(argv[1]);
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_volume(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: volume <0-100>\n");
        return 1;
    }
    sounder_set_volume((uint8_t)strtoul(argv[1], NULL, 10));
    sounder_play(ALARM_SOUND_ACK);
    return 0;
}

#if CONFIG_ALARM_BUILD_DEV
/* The bench silence (sounder.h, docs/features/08 AUD-025): development builds only. */
static int cmd_speaker(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "on") == 0)) {
        const esp_err_t err = sounder_set_silenced(strcmp(argv[1], "off") == 0);
        if (err != ESP_OK) {
            printf("not kept: %s\n", esp_err_to_name(err));
        }
    } else if (argc != 1) {
        printf("usage: speaker [on|off]\n");
        return 1;
    }
    printf("speaker %s\n", sounder_silenced()
                               ? "OFF (bench silence: this panel's speaker, alarms included; "
                                 "paired keypads and sirens still sound)"
                               : "on");
    return 0;
}
#endif

static int cmd_ntp(int argc, char **argv)
{
    if (argc == 1) {
        char ntp[64];
        net_wifi_get_ntp_server(ntp, sizeof(ntp));
        printf("%s\n", ntp);
        return 0;
    }
    const esp_err_t err = net_wifi_set_ntp_server(argv[1]);
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        printf("erases zones, users, codes, Wi-Fi and preferences, then restarts with the\n"
               "factory codes and the setup wizard. Add 'log' to erase the event log too:\n"
               "  factory_reset confirm [log]\n");
        return 1;
    }
    const bool erase_log = argc > 2 && strcmp(argv[2], "log") == 0;
    printf("factory reset%s, restarting\n", erase_log ? " (event log too)" : "");
    panel_system_factory_reset(&SRC, 0, erase_log);
    return 0;
}

static int cmd_memory(int argc, char **argv)
{
    const uint32_t caps[] = {MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};
    const char *names[] = {"internal", "PSRAM"};
    for (unsigned i = 0; i < 2; i++) {
        multi_heap_info_t m;
        heap_caps_get_info(&m, caps[i]);
        printf("%s: free=%u largest=%u minimum=%u allocated=%u blocks=%u\n", names[i],
               (unsigned)m.total_free_bytes, (unsigned)m.largest_free_block,
               (unsigned)m.minimum_free_bytes, (unsigned)m.total_allocated_bytes,
               (unsigned)m.allocated_blocks);
    }
    printf("crypto ready=%s faults=%" PRIu32 "\n",
           code_hash_status() == ESP_OK ? "yes" : "no", code_hash_faults());
    if (argc > 1 && strcmp(argv[1], "check") == 0) {
        const bool ok = heap_caps_check_integrity_all(true);
        printf("heap integrity: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    return 0;
}

#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
typedef struct { unsigned fail_after; esp_err_t result; } crypto_test_t;
static void do_crypto_test(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)e;
    (void)now;
    crypto_test_t *test = ctx;
    test->result = code_hash_diagnostic(test->fail_after);
}

static int cmd_crypto_test(int argc, char **argv)
{
    unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    if (count == 0u || count > 100u) {
        printf("crypto_test [1..100]\n");
        return 1;
    }
    for (unsigned i = 0; i < count; i++) {
        /* Exercise inner, outer, late-iteration clone failures and the normal path. */
        const unsigned points[] = {0, 1, 2, 2048};
        crypto_test_t test = {.fail_after = points[i % 4u], .result = ESP_FAIL};
        const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        esp_err_t err = alarm_service_call(do_crypto_test, &test);
        const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        printf("crypto test %u fail_clone=%u %s internal before=%u after=%u\n", i + 1u,
               test.fail_after, err == ESP_OK && test.result == ESP_OK ? "PASS" : "FAIL",
               (unsigned)before, (unsigned)after);
        if (err != ESP_OK || test.result != ESP_OK) {
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}
#endif


static int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_restart();
    return 0;
}

esp_err_t panel_console_start(void)
{
    /* The console has to come up even when the engine did not: a panel whose storage cannot be
     * read has nothing else left to say why, and "factory_reset confirm" from here is the
     * owner's way out. Without a configuration the virtual keypad runs on the defaults; nothing
     * else here needs one. */
    alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_DEFAULT);
    if (cfg != NULL && alarm_service_get_config(cfg) == ESP_OK) {
        alarm_keyseq_init(&s_ks, &cfg->opt, 1);
    } else {
        ESP_LOGW(TAG, "no configuration: the console runs without one");
        alarm_options_t opt;
        memset(&opt, 0, sizeof(opt));
        alarm_keyseq_init(&s_ks, &opt, 1);
    }
    heap_caps_free(cfg);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "alarm> ";
    rc.max_cmdline_length = 256;
    rc.task_stack_size = 6144;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    const esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&hw, &rc, &repl), TAG, "REPL");
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const esp_console_dev_usb_serial_jtag_config_t hw =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl), TAG, "REPL");
#else
#error "panel_console needs a UART or USB-Serial-JTAG console"
#endif

    static const esp_console_cmd_t cmds[] = {
        {.command = "memory", .help = "Heap bytes, fragmentation, low water and crypto faults",
         .hint = "[check]", .func = cmd_memory},
#if CONFIG_ALARM_CRYPTO_DIAGNOSTICS
        {.command = "crypto_test", .help = "Isolated hash failure/recovery test; no real codes",
         .hint = "[1..100]", .func = cmd_crypto_test},
#endif
        {.command = "status", .help = "Partition, troubles, zones not normal, network",
         .func = cmd_status},
        {.command = "zones", .help = "List configured zones and their sensors", .func = cmd_zones},
        {.command = "zone", .help = "Simulate a zone input, or show / bind / unbind its sensor",
         .hint = "<n> <open|close|...|source|bind ...|unbind>", .func = cmd_zone},
        {.command = "sources", .help = "Zone sources in this build and what they can do",
         .func = cmd_sources},
        {.command = "terminals", .help = "Wired terminals, their levels and zones",
         .func = cmd_terminals},
        {.command = "keys", .help = "Type on a virtual keypad (e.g. keys 123456W)",
         .hint = "<keys>", .func = cmd_keys},
        {.command = "trouble", .help = "Raise or clear a system trouble",
         .hint = "<name> <on|off>", .func = cmd_trouble},
        {.command = "events",
         .help = "Newest events from the flash log ('all' adds report markers, 'clear_loss' "
                 "acknowledges records the log could not keep)",
         .hint = "[count] [all] [clear_loss]", .func = cmd_events},
        {.command = "log_export", .help = "Print the whole event log as CSV", .func = cmd_log_export},
        {.command = "wifi", .help = "Show Wi-Fi, scan, set credentials or forget them",
         .hint = "[scan | <ssid> [<password>] | forget]", .func = cmd_wifi},
        {.command = "ip", .help = "Show or set how the panel gets its address",
         .hint = "[dhcp | static <ip> <mask> <gateway>] [<dns1> [<dns2>]]", .func = cmd_ip},
        {.command = "netcheck", .help = "Check the connection step by step and print the result",
         .func = cmd_netcheck},
        {.command = "ntp", .help = "Show or set the time server", .hint = "[server]",
         .func = cmd_ntp},
        {.command = "tz", .help = "Show or set the POSIX time zone",
         .hint = "[EST5EDT,M3.2.0,M11.1.0]", .func = cmd_tz},
        {.command = "volume", .help = "Keypad sounder volume", .hint = "<0-100>",
         .func = cmd_volume},
#if CONFIG_ALARM_BUILD_DEV
        {.command = "speaker",
         .help = "Bench silence: off keeps the speaker off, alarms included (development builds)",
         .hint = "[on|off]", .func = cmd_speaker},
#endif
        {.command = "factory_reset", .help = "Erase everything and restart with factory codes",
         .hint = "confirm [log]", .func = cmd_factory_reset},
        {.command = "restart", .help = "Restart the panel", .func = cmd_restart},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s",
                            cmds[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    return esp_console_start_repl(repl);
}
