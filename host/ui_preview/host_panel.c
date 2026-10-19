/*
 * host_panel.c - the alarm engine running in-process for the UI preview.
 * Mirrors the panel's factory configuration (components/services/platform_esp/src/alarm_store.c) including
 * the eight demo zones, bound to the simulator source as the panel binds them on its first start
 * with zone sources. The zone hub is the real components/core/zone_hub with the simulator and the
 * wired source on fake terminals (the J7 header's GPIOs). The event log is the real
 * components/core/event_log ring on a RAM flash; Wi-Fi, time zone and preferences are simulated.
 */
#include "host_panel.h"
#include "host_link_demo.h"
#include "host_zigbee_demo.h"
#include "host_zigbee_real.h"
#include "i18n/i18n.h"
#include "notify/notify_auto.h"
#include "ui_backend/ui_check_words.h"
#include "ui_backend/ui_dev_model.h"
#include "ui_backend/ui_prefs.h"
#include "ui_backend/ui_voice.h"
#include "annunciator/annunciator.h"
#include "sounder/sounder_mix.h"
#include "sounder/speech_boost.h"
#include "voice_pack/voice_pack.h"
#include "voice_render.h"
#include "home_control/home_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_keyseq.h"
#include "alarm_proto/proto.h"
#include "event_log/elog.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_ha.h"
#include "zone_hub/zsrc_sim.h"
#include "zone_hub/zsrc_wired.h"
#include "zone_hub/zsrc_rf.h"
#include "zone_hub/zsrc_zigbee.h"

#if ALARM_SIM_ZWAVE
#include <stdarg.h>
#include "alarm/alarm_strings.h"
#include "fake_zac93.h"
#include "fake_zdev.h"
#include "zw_host_crypto.h"
#include "zone_hub/zsrc_zwave.h"
#include "zwave_host/zw_crypto.h"
#include "zwave_proto/zw_cc_basic.h"
#include "zwave_proto/zw_cc_notification.h"
#include "zwave_proto/zw_gbl.h"
#include "zwave_zones/zwz_panel.h"
#include "zwave_zones/zwz_store.h"
#include "zwave_zones/zw_fwjob.h"
#include "alarm_proto/proto_link.h"
#include "automation/auto_clock.h"
#include "automation/auto_run.h"
#include "automation/auto_watch.h"
#include "automation/automation.h"
#include "rtc_clock/ds3231.h"

/* The node cache's encoding at its fullest, as zwave_service sizes its buffer (network.md §2). */
#define ZW_SIM_NODES_BLOB (ZW_NODES_MAX * 800u + 1024u)
#endif

#define RING       100
#define LOG_SECTOR 4096u
#define LOG_SIZE   (LOG_SECTOR * 16u)

typedef struct {
    const char *ssid;
    int8_t rssi;
    bool secure;
} fake_ap_t;

static const fake_ap_t APS[] = {
    {"Harbor House", -52, true},  {"Harbor House 5G", -61, true}, {"Maple Street", -70, true},
    /* A network name outside ASCII draws as itself (docs/features/37 I18N-009). */
    {"DIRECT-42 Printer", -74, true}, {"Caf\xC3\xA9 Guest", -81, false}, {"Neighbors", -86, true},
};

/* BL-143 ZB-M6: the panel's real Zigbee (host_zigbee_real.h) instead of the demo model, from the
 * next host_panel_init() on; kept outside H, which host_panel_init() clears. */
static bool s_zb_real;

static struct {
    alarm_engine_t eng;
    alarm_ms_t now;
    int64_t epoch_at_start;
    alarm_snapshot_t snap;
    uint32_t seq;
    uint32_t config_seq;
    bool dirty;
    alarm_event_t ring[RING];
    size_t head;
    size_t count;
    ui_net_state_t net;
    int rssi;
    char ssid[33];
    ui_wifi_fail_t fail;
    alarm_ms_t connect_at; /* simulated association finishes */
    bool connect_ok;
    alarm_ms_t scan_done_at;
    char tz[64];
    char ntp[64];
    /* simulated addressing, house network and connection check */
    ui_ip_config_t ip;
    bool faults[HOST_NET_FAULT_COUNT];
    ui_netcheck_t check;
    size_t check_step;    /* the step running now */
    alarm_ms_t check_at;  /* when it finishes */
    uint8_t log_mem[LOG_SIZE];
    elog_t log;
    /* simulated Home Assistant connection */
    ui_mqtt_settings_t mqtt;
    char mqtt_password[64];
    ui_mqtt_state_t mqtt_state;
    ui_mqtt_fail_t mqtt_fail;
    alarm_ms_t mqtt_at; /* the simulated connection attempt ends */
    uint32_t mqtt_sent;
    /* zone sources */
    zone_hub_t hub;
    zsrc_sim_t sim;
    zsrc_wired_t wired;
    zsrc_ha_t ha_src; /* the real Home Assistant source, fed by the pretend integration */
    zsrc_zb_t zb;     /* the real Zigbee source, fed by a pretend ESP32-H2 radio */
    zsrc_rf_t rf;     /* the real 433 MHz source, fed by a pretend CC1101 (docs/features/25) */
    bool rf_present;  /* the receiver answers on its bus */
    bool rf_noise;    /* a carrier sits on the band */
    bool zb_present;  /* the module is plugged in */
    bool zb_join_pending;
    bool zb_scan_pending;
    bool zb_noise;
    /* the real Zigbee's news from the engine, handed on after the engine's call (s_zb_real) */
    uint8_t zb_out[ALARM_MAX_PARTITIONS + 1u][2]; /* [partition][bell, strobe]: latest pattern */
    uint32_t zb_out_dirty;                        /* bit: partition */
    zba_view_t zb_view[ALARM_MAX_PARTITIONS];     /* each partition's view as last told */
    bool zb_view_told[ALARM_MAX_PARTITIONS];
    bool zb_walk_on;             /* the walk test's scope as last told (zigbee_service's) */
    uint32_t zb_walk_seq;
    uint64_t zb_walk_at;
    uint32_t source_faults;
    uint8_t pins[64]; /* fake terminal levels: 1 = open loop */
    ui_ntfy_settings_t ntfy;
    char ntfy_token[64];
    ui_pushover_settings_t push;
    char push_user[48];
    char push_app[48];
    ui_notify_state_t notify_state[UI_NOTIFY_PROVIDERS];
    ui_notify_fail_t notify_fail[UI_NOTIFY_PROVIDERS];
    uint32_t notify_sent[UI_NOTIFY_PROVIDERS];
    uint64_t notify_last[UI_NOTIFY_PROVIDERS];
    bool notify_ever[UI_NOTIFY_PROVIDERS];
    bool notify_down[UI_NOTIFY_PROVIDERS];
    uint32_t topic_seq;
    /* the battery-backed clock module (docs/features/35): none, as on most panels */
    ui_clock_module_t clock_module;
    bool clock_lost_at_start;
    bool clock_by_hand;
#if ALARM_SIM_ZWAVE
    /* docs/features/36: the Z-Wave world ("The Z-Wave world" below). `nvs` is what a panel keeps
     * in NVS "zwave" - keys, gen, nodes, cfg, chk - written as zwz_panel_dirty() says, the way
     * zwave_service's persist() writes them: host_panel_zwave_restart() starts a fresh zwz_panel
     * from it, as zwave_service_start() does. Everything here is new only at zwave_world_init()
     * (a whole simulator reset). */
    struct {
        zwz_panel_t panel; /* the panel's Z-Wave: the firmware's own (zwave_zones) */
        zwz_store_t store; /* its writes into `nvs`: the firmware's own too (zwz_store.h) */
        zsrc_zw_t src;     /* its zone source: registered with H.hub once, in zwave_world_init */
        struct {
            uint8_t keys[ZWZ_KEYS_BLOB];
            size_t keys_len;
            uint16_t gen;
            uint8_t nodes[ZW_SIM_NODES_BLOB];
            size_t nodes_len;
            uint8_t cfg[ZWZ_CFG_BLOB];
            size_t cfg_len;
            uint8_t chk[ZWZ_CHECKINS_BLOB];
            size_t chk_len;
            uint8_t garage[ZWO_GARAGE_BLOB];
            size_t garage_len;
            uint8_t prov[ZW_PROV_BLOB_MAX];
            size_t prov_len;
        } nvs;
        fake_zac93_t zac;
        fake_wire_t wire;
        zw_host_crypto_t hc;
        zw_crypto_port_t crypto;
        fake_zdev_t door;   /* sleeping, S2 Authenticated/Access/Unauthenticated requested */
        fake_zdev_t motion; /* mains, no Security 2 */
        /* Z7b: two more, in radio range but not yet joined, for the add flow's own walk-through
         * (screens.md §4/§13) - the door and motion above are already in the network by the time
         * the pages' own shots start (Z7a's walk), so they can't be added again through the UI. */
        fake_zdev_t spare_secure;   /* another sleeping S2 sensor: the PIN page, end to end */
        fake_zdev_t spare_insecure; /* a ZSE43 tilt sensor: sleeping on battery, no Security 2 */
        /* ZW-029: a keypad woken by the panel's call (FLiRS), on batteries, S2 with its PIN and
         * the indicator a keypad's sounds and lights need; and a device that joins with an empty
         * node information and then hears nothing (reset, or out of range) */
        fake_zdev_t keypad;
        fake_zdev_t stray;
        /* ZW-034/035: a light switch and a three-relay garage kit (the spare tilt sensor above
         * says where its door is), in range, not joined; S2, mains. Their IDs are made up: the
         * real models' are to be read from the bench (COMPATIBILITY notes) */
        fake_zdev_t light;
        fake_zdev_t relay;
        fake_zdev_t dimmer; /* a dimmer (Multilevel Switch), for the level choice */
        /* ZW-024: a SmartStart door sensor, registered by its key: once powered it asks to join
         * every 8 s (as a real one keeps asking) until it is in */
        fake_zdev_t smart;
        /* ZW-024 §10: a light switch in a box, for the broken one's place */
        fake_zdev_t spare_light;
        bool smart_powered;
        uint64_t smart_next_ask;
        zwf_t feedback; /* the keypad feedback zwave_service keeps (NVS zwave/keypad_fb) */
        zwkc_t keyctl;  /* ... and keypad control (NVS zwave/keypad_ctl) */
        struct {        /* zwave_service's keypad mailbox: claimed under "the lock", run after */
            bool used;
            uint8_t slot;
            uint32_t home;
            uint16_t generation;
            zw_s2_rx_t rx;
            uint8_t cmd[64];
        } kc_box;
        alarm_result_t kc_last;  /* the engine's answer to the keypad's last command */
        /* ZW-033: a device's software update as zwave_service runs it - the job (check,
         * download, verify), a simulated update service a second away, the image the transfer
         * sends; `fw_offline`: the service can't be reached */
        zwfj_t fwjob;
        uint64_t fw_due;
        uint8_t fw_user;
        bool fw_offline;
        uint8_t fw_image[1024];
        /* network.md §14: the radio's own - its job (node 0), the service a second away, the
         * .gbl it offers (2.30) and the image the bootloader receives */
        zwfj_t radiojob;
        uint64_t radio_due;
        uint8_t radio_image[2048];
        uint8_t kc_seq;          /* the simulated keypad's own sequence numbers */
        bool door_added, motion_added;
        bool door_open, motion_open;
        char status[160];
        uint32_t seq;
        /* The simulator's own add buttons (zwave_watch): the device being added (0 none, 1 the
         * door, 2 the motion sensor) until it is in the network, then its node until the add's end
         * was seen; the node IDs the two got, for zwave_smoke.c and the walk. */
        uint8_t adding;
        uint16_t button_node;
        uint16_t door_node, motion_node;
        bool stuck_said; /* the simulated wire stopped settling: said once */
        uint8_t status_force; /* host_panel_zwave_status_force() */
        bool on;         /* the world runs: from the start, or host_panel_zwave_begin() */
        /* what the log strip last said something about (zwave_watch) */
        struct {
            uint8_t radio, op, step, result;
            uint64_t devices_at; /* the devices are looked at again from then; 0: not yet */
            size_t n;
            struct {
                uint16_t node, generation;
                bool ready, unreachable, stopped;
            } dev[UI_ZW_DEVICES_MAX];
        } seen;
    } zw;
#endif
} H;

#if ALARM_SIM_ZWAVE
static bool s_zw_later; /* host_panel_zwave_later(): outlives host_panel_init()'s reset */
#endif

/* The panel's wired terminals (board_pins.h BOARD_WIRED_TERMINALS). */
static const zsrc_wired_terminal_t TERMINALS[] = {
    {2, "J7 header"},  {3, "J7 header"},  {4, "J7 header"},  {5, "J7 header"},
    {49, "J7 header"}, {50, "J7 header"}, {51, "J7 header"}, {52, "J7 header"},
};

static const alarm_source_t SRC = {.kind = ALARM_SRC_KEYPAD, .id = 0};
static void devices_reset(int64_t epoch_at_start); /* Smart Home's fictional house */
/* The automations (docs/features/41 U12): fed by the engine's ports, run after each engine call. */
static void auto_feed_event(const alarm_event_t *ev);
static void auto_feed_snapshot(const alarm_snapshot_t *s);
static bool auto_pump(void);
static uint64_t auto_next_deadline(void);
static void automations_reset(void);

/* Not a real hash (the panel uses PBKDF2): the preview only needs determinism. */
static void preview_hash(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                         uint8_t out[ALARM_CODE_HASH_LEN])
{
    (void)ctx;
    for (unsigned block = 0; block < ALARM_CODE_HASH_LEN / 8u; block++) {
        uint64_t h = 1469598103934665603ull ^ (uint64_t)(block * 0x9E3779B97F4A7C15ull);
        for (unsigned i = 0; i < ALARM_CODE_SALT_LEN; i++) {
            h = (h ^ salt[i]) * 1099511628211ull;
        }
        for (const char *c = code; *c != '\0'; c++) {
            h = (h ^ (uint8_t)*c) * 1099511628211ull;
        }
        for (unsigned b = 0; b < 8u; b++) {
            out[block * 8u + b] = (uint8_t)(h >> (8u * b));
        }
    }
}

/* ---- RAM flash for the event log ---------------------------------------------------------- */

static int flash_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    memcpy(buf, H.log_mem + off, len);
    return 0;
}

static int flash_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    const uint8_t *d = buf;
    for (uint32_t i = 0; i < len; i++) {
        H.log_mem[off + i] &= d[i];
    }
    return 0;
}

static int flash_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    memset(H.log_mem + off, 0xFF, len);
    return 0;
}

/* ---- the keypad sounder ------------------------------------------------------------------- */

/* Kept outside H so a simulator reset does not take the sound away with the rest of the world. */
static host_panel_sound_t s_snd;

void host_panel_sound(const host_panel_sound_t *snd)
{
    if (snd == NULL) {
        memset(&s_snd, 0, sizeof(s_snd));
    } else {
        s_snd = *snd;
    }
}

static void say_sound(alarm_sound_t sound)
{
    if (s_snd.one_shot != NULL) {
        s_snd.one_shot(s_snd.ctx, sound);
    }
}

/* ---- the voice (below): the policy the panel runs, over this engine ---- */
static uint32_t vx_tone(alarm_sound_t sound);
static void vx_sound(uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket);
static void vx_event(const alarm_event_t *ev);
static void vx_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);
static void vx_snapshot(const alarm_snapshot_t *snap);
static void vx_pump(void);
static void vx_power_poll(void);
static uint64_t vx_next_deadline(void);
static void vx_init(void);
static void vx_apply_prefs(void);
static void vx_apply_pack(void);
static uint8_t voice_state(void);
static uint8_t vx_preview(const uint16_t *ids, size_t n);

/* ---- engine ports ------------------------------------------------------------------------- */

/* The panel plays one-shots for its own partition and for system-wide sounds (partition 0). */
static void on_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone)
{
    (void)ctx;
    uint32_t ticket = 0u;
    if (partition == 0u || partition == 1u) {
        ticket = vx_tone(sound);
    }
    vx_sound(partition, sound, zone, ticket);
}

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    H.ring[H.head] = *ev;
    H.head = (H.head + 1u) % RING;
    if (H.count < RING) {
        H.count++;
    }
    elog_append(&H.log, ELOG_KIND_EVENT, ev);
    H.dirty = true;
    vx_event(ev);
    auto_feed_event(ev); /* queued: the runner sees it after the engine call returns */
#if ALARM_SIM_ZWAVE
    if (H.zw.on) {
        zwf_event(&H.zw.feedback, ev); /* zwave_service's feedback_event: refusals to keypads */
    }
#endif
}

static void on_report(void *ctx, const alarm_event_t *ev)
{
    if (H.mqtt_state == UI_MQTT_CONNECTED) {
        H.mqtt_sent++;
    }
    (void)ctx;
    alarm_event_t marker = *ev;
    marker.mono_ms = H.now;
    elog_append(&H.log, ELOG_KIND_REPORTED, &marker);
}

static void on_status(void *ctx, uint8_t partition)
{
    (void)ctx;
    (void)partition;
    H.dirty = true;
}

/* The bell and strobe, for the real Zigbee's sirens: kept, handed on in after() (a port never
 * re-enters the engine). */
static void on_output(void *ctx, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    (void)ctx;
    vx_output(output, partition, pattern);
    if (partition > ALARM_MAX_PARTITIONS ||
        (output != ALARM_OUT_BELL && output != ALARM_OUT_STROBE)) {
        return;
    }
    H.zb_out[partition][output == ALARM_OUT_BELL ? 0 : 1] = (uint8_t)pattern;
    H.zb_out_dirty |= 1u << partition;
}

static void on_config_changed(void *ctx)
{
    (void)ctx;
    H.config_seq++;
    H.dirty = true;
}

static int64_t epoch_ms(void *ctx)
{
    (void)ctx;
    return H.epoch_at_start == 0 ? 0 : H.epoch_at_start + (int64_t)H.now;
}

/* ---- zone sources ------------------------------------------------------------------------- */

static void vx_commit(void);

static void hub_zone_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    alarm_engine_zone_event(&H.eng, zone, ev, H.now);
    vx_commit();
}

/* D07 (docs/features/08 voice-first-build.md): a zone whose sensor changes loses its spoken-name
 * approval before the new binding is committed, as zone_service does on the panel; a mapping-only
 * change keeps it. The first start (zones_start) binds without this: nothing was approved. */
static void host_voice_identity(uint16_t zone, const zone_hub_binding_t *next)
{
    zone_hub_binding_t cur;
    const bool had = zone_hub_get(&H.hub, zone, &cur);
    if (zone == 0u || zone > ALARM_MAX_ZONES || zone_hub_same_sensors(had ? &cur : NULL, next)) {
        return;
    }
    const alarm_config_t *now = alarm_engine_config(&H.eng);
    if (now->zones[zone - 1u].type == ALARM_ZT_NOT_USED) {
        return;
    }
    static alarm_config_t cfg;
    cfg = *now;
    if (alarm_config_voice_invalidate_zone(&cfg, zone) == ALARM_OK &&
        alarm_engine_set_config(&H.eng, &cfg, H.now) == ALARM_OK) {
        H.config_seq++;
        H.dirty = true;
    } else {
        printf("voice: zone %u's sensor changed, its spoken name could not be invalidated\n",
               (unsigned)zone);
    }
}

static zone_hub_result_t host_zone_bind(uint16_t zone, const zone_hub_binding_t *b)
{
    host_voice_identity(zone, b);
    return zone_hub_bind(&H.hub, zone, b, H.now);
}

static void host_zone_unbind(uint16_t zone)
{
    host_voice_identity(zone, NULL);
    zone_hub_unbind(&H.hub, zone, H.now);
}

static void hub_source_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    if (kind >= 32u) {
        return;
    }
    const uint32_t bit = 1u << kind;
    H.source_faults = on ? H.source_faults | bit : H.source_faults & ~bit;
    alarm_engine_system_trouble(&H.eng, ALARM_TRBL_SENSOR_RADIO, H.source_faults != 0u, H.now);
    vx_commit();
}

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                       uint64_t now)
{
    (void)ctx;
    zone_hub_input(&H.hub, a, state, value, now);
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    zone_hub_event(&H.hub, a, ev, now);
}

static void sink_reachable(void *ctx, const zsrc_address_t *a, bool reachable, uint64_t now)
{
    (void)ctx;
    zone_hub_reachable(&H.hub, a, reachable, now);
}

static void sink_link(void *ctx, uint8_t kind, bool up, uint64_t now)
{
    (void)ctx;
    zone_hub_link(&H.hub, kind, up, now);
}

static void sink_link_now(void *ctx, uint8_t kind, uint64_t now)
{
    (void)ctx;
    zone_hub_link_now(&H.hub, kind, now);
}

/* ---- the pretend ESP32-H2 radio (docs/features/20) --------------------------------------------- */
/* Two sensors that join the moment the installer opens the learn window, which is what really
 * happens when a contact's pairing button is held. The rest of the buttons drive the first. */
#define ZB_CONTACT_IEEE 0x00158D0001A2B3C4ull
#define ZB_MOTION_IEEE  0x00124B00229988FFull

static void zb_join_demo_devices(void);

static bool zb_permit_join(void *ctx, uint16_t seconds)
{
    (void)ctx;
    if (seconds > 0u) {
        H.zb_join_pending = true; /* deliver after the source has opened its learn window */
    }
    return true;
}

static bool zb_cie(void *ctx, uint64_t ieee, uint8_t endpoint)
{
    (void)ctx;
    (void)ieee;
    (void)endpoint;
    return true;
}

static bool zb_enroll(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t zone_id)
{
    (void)ctx;
    (void)ieee;
    (void)endpoint;
    (void)zone_id;
    return true;
}

static bool zb_read_attr(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cluster, uint16_t attr)
{
    (void)ctx;
    (void)ieee;
    (void)ep;
    (void)cluster;
    (void)attr;
    return true;
}

static bool zb_remove(void *ctx, uint64_t ieee)
{
    (void)ctx;
    (void)ieee;
    return true;
}

static bool zb_scan(void *ctx)
{
    (void)ctx;
    H.zb_scan_pending = true;
    return true;
}

static bool pin_setup(void *ctx, uint8_t gpio, bool on)
{
    (void)ctx;
    (void)gpio;
    (void)on;
    return true;
}

static uint8_t pin_read(void *ctx, uint8_t gpio)
{
    (void)ctx;
    return gpio < sizeof(H.pins) ? H.pins[gpio] : 0u;
}

static void zb_real_start(const zsrc_sink_t *sink);
static void zb_real_news(void);

static void zones_start(const alarm_config_t *cfg)
{
    const zone_hub_port_t port = {.ctx = NULL, .zone_event = hub_zone_event,
                                  .source_trouble = hub_source_trouble};
    zone_hub_init(&H.hub, &port);
    const zsrc_sink_t sink = {.ctx = NULL, .input = sink_input, .event = sink_event,
                              .reachable = sink_reachable, .link = sink_link,
                              .link_now = sink_link_now};
    zsrc_sim_init(&H.sim, &sink);
    zone_hub_register(&H.hub, zsrc_sim_ops(), &H.sim, H.now);
    const zsrc_gpio_port_t gpio = {.ctx = NULL, .setup = pin_setup, .read = pin_read};
    zsrc_wired_init(&H.wired, TERMINALS, sizeof(TERMINALS) / sizeof(TERMINALS[0]), &gpio, &sink);
    zone_hub_register(&H.hub, zsrc_wired_ops(), &H.wired, H.now);
    zsrc_ha_init(&H.ha_src, &sink); /* usable once the pretend Home Assistant pairs */
    zone_hub_register(&H.hub, zsrc_ha_ops(), &H.ha_src, H.now);
    if (s_zb_real) {
        zb_real_start(&sink); /* zbz_panel, its source and the fake radio (host_zigbee_real.h) */
    } else {
        const zsrc_zb_radio_t radio = {.ctx = NULL,
                                       .permit_join = zb_permit_join,
                                       .ias_cie_write = zb_cie,
                                       .ias_enroll_response = zb_enroll,
                                       .read_attribute = zb_read_attr,
                                       .remove_device = zb_remove,
                                       .energy_scan = zb_scan};
        zsrc_zb_init(&H.zb, &radio, &sink);
        zone_hub_register(&H.hub, zsrc_zb_ops(), &H.zb, H.now);
        zsrc_zb_link(&H.zb, true, H.now); /* the module is in its socket, the network formed */
    }
    H.zb_present = true;
    zsrc_rf_init(&H.rf, &sink);
    zone_hub_register(&H.hub, zsrc_rf_ops(), &H.rf, H.now);
    H.rf_present = true;
    zsrc_rf_receiver(&H.rf, true, H.now); /* a CC1101 answers on the socket's SPI */
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) { /* the panel's first start */
        if (cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED) {
            zone_hub_binding_t b;
            memset(&b, 0, sizeof(b));
            b.in[ZSRC_ROLE_MAIN].addr = zsrc_sim_address(z);
            zone_hub_bind(&H.hub, z, &b, H.now);
        }
    }
    zone_hub_start(&H.hub, H.now);
}

static void after(void)
{
    if (H.zb_join_pending) {
        H.zb_join_pending = false;
        zb_join_demo_devices();
    }
    /* An automation's failure is an engine event (a new snapshot): a few rounds settle it. */
    for (int round = 0; round < 4; round++) {
        if (H.dirty) {
            H.dirty = false;
            alarm_engine_snapshot(&H.eng, &H.snap);
            H.snap.config_seq = H.config_seq;
            H.snap.seq = ++H.seq;
            /* The keypad follows its own partition's status, as app_main's on_snapshot does. */
            if (s_snd.continuous != NULL) {
                s_snd.continuous(s_snd.ctx, H.snap.part[0].sounder);
            }
            vx_snapshot(&H.snap);
            auto_feed_snapshot(&H.snap);
        }
        if (!auto_pump()) {
            break;
        }
    }
    vx_pump();
    if (s_zb_real) {
        zb_real_news();
    }
}

static void demo_zone(alarm_config_t *cfg, uint16_t z, alarm_zone_type_t type,
                      alarm_device_class_t cls, const char *name)
{
    alarm_zone_config_defaults(&cfg->zones[z - 1u], z, type, 1);
    cfg->zones[z - 1u].device_class = (uint8_t)cls;
    snprintf(cfg->zones[z - 1u].name, ALARM_NAME_LEN, "%s", name);
}

#if ALARM_SIM_ZWAVE
/* ---- The Z-Wave world (docs/features/36 Z6, Z7) ---------------------------------------------
 * The panel's own Z-Wave, as the firmware runs it: one zwz_panel_t (components/core/zwave_zones:
 * the network, the bridge to the zone source, what the Z-Wave pages show and ask for, what is
 * logged and stored) - the one components/services/zwave_service owns on a panel - against the
 * fake ZAC93 (components/core/zwave_host/test/fake_zac93.c) and four simulated devices, on the
 * simulator's own clock (H.now). It is driven the way zwave_service drives it: the link's news go
 * in, it is polled at its deadline, the pages' requests and the simulator's buttons go through
 * zwz_panel_request(), and after every run what it says changed goes into the simulator's "NVS"
 * (H.zw.nvs, as persist() writes it) and what it logged into the engine (as log_events() does:
 * the Activity list's "Z-Wave device added" and the rest). Its zone source is registered with the
 * real H.hub, as zsrc_rf and zsrc_zb are in zones_start(): a bound Z-Wave input opens, closes,
 * tampers and troubles zones for real.
 *
 * Nothing here decides anything about the network: the log strip's words (H.zw.status) are read
 * from what the pages see and what is logged, and the buttons answer as an installer would. */

static void zwn_note(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(H.zw.status, sizeof H.zw.status, fmt, args);
    va_end(args);
    H.zw.seq++;
}

/* ---- zwz_panel's port, and the wire (the zw_link task's side) -------------------------------- */

static bool zwp_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&H.zw.wire, bytes, len);
}

static bool zwp_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    return zone_hub_get(&H.hub, zone, out);
}

static void zww_control(void *ctx, uint64_t now, uint8_t b)
{
    (void)ctx;
    zwz_panel_on_control(&H.zw.panel, now, b);
}

static void zww_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    (void)ctx;
    zwz_panel_on_frame(&H.zw.panel, now, f);
}

static void zww_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    (void)ctx;
    zwz_panel_on_text(&H.zw.panel, now, b, n);
}

/* The zwave task's poll, at the deadline zwz_panel gives (zone_hub_tick() itself runs from
 * host_panel_advance()). */
static void zww_poll(void *ctx, uint64_t now)
{
    (void)ctx;
    zwz_panel_poll(&H.zw.panel, now);
}

static uint64_t zww_deadline(void *ctx)
{
    (void)ctx;
    return zwz_panel_next_deadline(&H.zw.panel);
}

/* ---- the panel starts from what it stored (zwave_service_start) ------------------------------ */

/* command-classes.md §5: what the panel answers about itself - a placeholder identity (the project
 * has no manufacturer ID of its own yet), as zwave_service's identity() builds it. */
static void zw_identity(zw_cc_panel_identity_t *id)
{
    memset(id, 0, sizeof *id);
    id->firmware_major = 1u;
    id->firmware_minor = 5u;
    id->manufacturer_id = 0xFFFFu;
    id->product_type = 0xFFFFu;
    id->product_id = 0xFFFFu;
    id->zwave_plus_role_type = 0x00u; /* central static controller */
    id->zwave_plus_node_type = 0x00u;
    id->installer_icon = 0x0500u; /* generic gateway */
    id->user_icon = 0x0500u;
}

/* zwave_service.c load_setup(): the settings, the keys, the inclusion counter and the devices' own
 * check-ins, from the simulator's NVS. */
static void zw_load_setup(zwz_panel_setup_t *setup, zwz_checkin_t *checkins, size_t *checkin_n)
{
    memset(setup, 0, sizeof *setup);
    if (H.zw.nvs.cfg_len > 0u && !zwz_cfg_decode(&setup->cfg, H.zw.nvs.cfg, H.zw.nvs.cfg_len)) {
        zwn_note("Z-Wave: the stored settings are unreadable: the defaults");
    }
    if (H.zw.nvs.keys_len > 0u &&
        !zwz_keys_decode(&setup->keys, H.zw.nvs.keys, H.zw.nvs.keys_len)) {
        zwn_note("Z-Wave: the stored network keys are unreadable");
    }
    setup->generation = H.zw.nvs.gen;
    setup->jam = true; /* network.md §9, as the firmware */
    zw_identity(&setup->identity);
    *checkin_n = 0u;
    if (H.zw.nvs.chk_len > 0u && !zwz_checkins_decode(checkins, ZWZ_PANEL_CHECKINS, checkin_n,
                                                      H.zw.nvs.chk, H.zw.nvs.chk_len)) {
        zwn_note("Z-Wave: the stored check-ins are unreadable: the default for every device");
    }
}

/* The panel's Z-Wave starts from what is stored, as zwave_service_start() starts it: a fresh
 * zwz_panel (the network, the bridge, the pages' view) with the stored node cache, the link opened
 * (a new wire to the same module) and started. The zone source and its bindings stay as they are:
 * they belong to the hub, and the simulator's hub does not restart - the source only starts with
 * its link down again, until the network has been checked (ZSRC-015). */
static void zwp_send_done(void *ctx, uint32_t tag, zw_s2_result_t result)
{
    (void)ctx;
    (void)zwf_done(&H.zw.feedback, tag, result, H.now);
}

/* zwave_service's keyctl_claim(): an assigned keypad's command copied, never run here. */
static bool zwp_command(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    size_t slot = ZWKC_SLOTS;
    const zw_node_record_t *rec = zw_node_find_const(&H.zw.panel.net.nodes, rx->node);
    if (H.zw.kc_box.used || rec == NULL || rx->len > sizeof H.zw.kc_box.cmd ||
        !zwkc_claims(&H.zw.keyctl, &H.zw.panel.net, rx, &slot)) {
        return false;
    }
    H.zw.kc_box.used = true;
    H.zw.kc_box.slot = (uint8_t)slot;
    H.zw.kc_box.home = H.zw.panel.net.keys_home_id;
    H.zw.kc_box.generation = rec->generation;
    H.zw.kc_box.rx = *rx;
    memcpy(H.zw.kc_box.cmd, rx->command, rx->len);
    H.zw.kc_box.rx.command = H.zw.kc_box.cmd;
    return true;
}

static bool kc_engine(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *src,
                      zwk_reply_t *reply)
{
    (void)ctx;
    reply->result = alarm_engine_command(&H.eng, cmd, src, H.now, &reply->detail);
    return reply->result != ALARM_ERR_UNAVAILABLE;
}

/* zwave_service's keyctl_run(): the claimed command through the engine, the copy erased. */
static void zwave_keyctl_run(void)
{
    if (!H.zw.kc_box.used) {
        return;
    }
    zwk_reply_t reply;
    const zwk_result_t r = zwkc_run(&H.zw.keyctl, H.zw.kc_box.slot, H.zw.kc_box.home,
                                    H.zw.kc_box.generation, &H.zw.kc_box.rx, H.now, &reply);
    zw_wipe(&H.zw.kc_box, sizeof H.zw.kc_box);
    H.zw.kc_last = reply.result;
    zwn_note("Z-Wave: the keypad's command: %s",
             r == ZWK_HANDLED || r == ZWK_DUPLICATE ? alarm_result_name(reply.result)
                                                    : "not taken");
    after();
}

/* zwave_service's feedback_poll(): the engine's state to the assigned keypads' own sounds. */
static void zwave_feedback_poll(void)
{
    alarm_snapshot_t snap;
    alarm_engine_snapshot(&H.eng, &snap);
    zwf_view_t view;
    zwf_view(&snap, &view);
    zwf_snapshot(&H.zw.feedback, &view);
    zwf_poll(&H.zw.feedback, &H.zw.panel.net, H.now, NULL, NULL);
}

static bool all_disarmed(void);

/* A device's Time or Date Get: the simulated panel's local time (zwave_service's port_local_time). */
static bool zwp_local_time(void *ctx, zwz_local_time_t *out)
{
    (void)ctx;
    const int64_t ms = H.epoch_at_start == 0 ? 0 : H.epoch_at_start + (int64_t)H.now;
    if (ms < 1735689600000LL) {
        return false;
    }
    const time_t t = (time_t)(ms / 1000);
    const struct tm *tm = localtime(&t);
    if (tm == NULL) {
        return false;
    }
    out->year = (uint16_t)(tm->tm_year + 1900);
    out->month = (uint8_t)(tm->tm_mon + 1);
    out->day = (uint8_t)tm->tm_mday;
    out->hour = (uint8_t)tm->tm_hour;
    out->minute = (uint8_t)tm->tm_min;
    out->second = (uint8_t)tm->tm_sec;
    return true;
}

/* SmartStart listens only while every partition is disarmed (zwave_service's port_disarmed). */
static bool zwp_disarmed(void *ctx)
{
    (void)ctx;
    return all_disarmed();
}

/* ZW-035: where a garage's tilt sensor says the door is (zwave_service's port_position). */
static zwo_position_t zwp_position(void *ctx, uint16_t zone)
{
    (void)ctx;
    zone_hub_info_t in;
    if (!zone_hub_info(&H.hub, zone, &in) || !in.known || in.unreachable || in.check ||
        in.link_down) {
        return ZWO_POS_UNKNOWN;
    }
    return in.active ? ZWO_POS_OPEN : ZWO_POS_CLOSED;
}

static void zwave_panel_boot(void)
{
    static const zwz_panel_port_t port = {.ctx = NULL,
                                          .write = zwp_write,
                                          .reset_line = NULL,
                                          .binding = zwp_binding,
                                          .send_done = zwp_send_done,
                                          .command = zwp_command,
                                          .zone_position = zwp_position,
                                          .disarmed = zwp_disarmed,
                                          .local_time = zwp_local_time};
    static const fake_host_t wire_host = {NULL,     zww_control, zww_frame,
                                          zww_text, zww_poll,    zww_deadline};
    zwz_panel_setup_t setup;
    zwz_checkin_t checkins[ZWZ_PANEL_CHECKINS];
    size_t checkin_n = 0u;
    zw_load_setup(&setup, checkins, &checkin_n);
    zwz_panel_init(&H.zw.panel, &port, &setup, checkins, checkin_n, &H.zw.src, &H.zw.crypto);
    zwz_panel_set_feedback(&H.zw.panel, &H.zw.feedback);
    zwz_panel_set_keyctl(&H.zw.panel, &H.zw.keyctl);
    zw_wipe(&setup, sizeof setup);
    zwz_store_init(&H.zw.store);
    if (H.zw.nvs.nodes_len > 0u &&
        !zwz_panel_load_nodes(&H.zw.panel, H.zw.nvs.nodes, H.zw.nvs.nodes_len)) {
        zwn_note("Z-Wave: the stored node cache is damaged: its devices are interviewed again");
    }
    if (H.zw.nvs.garage_len > 0u &&
        !zwz_panel_load_garage(&H.zw.panel, H.zw.nvs.garage, H.zw.nvs.garage_len)) {
        zwn_note("Z-Wave: the stored garage doors are damaged");
    }
    if (H.zw.nvs.prov_len > 0u &&
        !zwz_panel_load_prov(&H.zw.panel, H.zw.nvs.prov, H.zw.nvs.prov_len)) {
        zwn_note("Z-Wave: the stored SmartStart list is damaged");
    }
    zsrc_zw_link(&H.zw.src, false, H.now);
    fake_wire_init(&H.zw.wire, &H.zw.zac, &wire_host, H.now);
    zwz_panel_start(&H.zw.panel, H.now);
    H.zw.adding = 0u;
    H.zw.button_node = 0u;
    H.zw.stuck_said = false;
    memset(&H.zw.seen, 0, sizeof H.zw.seen);
    H.zw.seen.radio = UI_ZW_RADIO_STARTING;
}

/* ---- after every run: what zwave_service does after unlocking -------------------------------- */

/* persist(): what zwz_panel_dirty() says changed, into the simulator's NVS through zwave_service's
 * own zwz_store_run - the keys first, the counter before the node cache. A value that does not fit
 * its slot is refused, as NVS would. */
static bool zws_put(uint8_t *slot, size_t cap, size_t *slot_len, const uint8_t *data, size_t len)
{
    if (len > cap) {
        return false;
    }
    memcpy(slot, data, len);
    *slot_len = len;
    return true;
}

static bool zws_set_blob(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (strcmp(key, ZWZ_STORE_KEY_KEYS) == 0) {
        return zws_put(H.zw.nvs.keys, sizeof H.zw.nvs.keys, &H.zw.nvs.keys_len, data, len);
    }
    if (strcmp(key, ZWZ_STORE_KEY_NODES) == 0) {
        return zws_put(H.zw.nvs.nodes, sizeof H.zw.nvs.nodes, &H.zw.nvs.nodes_len, data, len);
    }
    if (strcmp(key, ZWZ_STORE_KEY_CFG) == 0) {
        return zws_put(H.zw.nvs.cfg, sizeof H.zw.nvs.cfg, &H.zw.nvs.cfg_len, data, len);
    }
    if (strcmp(key, ZWZ_STORE_KEY_CHK) == 0) {
        return zws_put(H.zw.nvs.chk, sizeof H.zw.nvs.chk, &H.zw.nvs.chk_len, data, len);
    }
    if (strcmp(key, ZWZ_STORE_KEY_GARAGE) == 0) {
        return zws_put(H.zw.nvs.garage, sizeof H.zw.nvs.garage, &H.zw.nvs.garage_len, data, len);
    }
    if (strcmp(key, ZWZ_STORE_KEY_PROV) == 0) {
        return zws_put(H.zw.nvs.prov, sizeof H.zw.nvs.prov, &H.zw.nvs.prov_len, data, len);
    }
    return false;
}

static bool zws_set_u16(void *ctx, const char *key, uint16_t v)
{
    (void)ctx;
    if (strcmp(key, ZWZ_STORE_KEY_GEN) != 0) {
        return false;
    }
    H.zw.nvs.gen = v;
    return true;
}

static void zws_trouble(void *ctx, bool failed)
{
    (void)ctx;
    if (failed) {
        zwn_note("Z-Wave: storing the network failed: tried again in %u s",
                 (unsigned)(ZWZ_STORE_RETRY_MS / 1000u));
    }
}

static void zwave_persist(void)
{
    static uint8_t blob[ZW_SIM_NODES_BLOB];
    static uint8_t prov[ZW_PROV_BLOB_MAX];
    static const zwz_store_port_t port = {.ctx = NULL,
                                          .set_blob = zws_set_blob,
                                          .set_u16 = zws_set_u16,
                                          .trouble = zws_trouble,
                                          .nodes_buf = blob,
                                          .nodes_cap = sizeof blob,
                                          .prov_buf = prov,
                                          .prov_cap = sizeof prov};
    (void)zwz_store_run(&H.zw.store, &H.zw.panel, H.now, &port);
}

/* log_events(): what it logged, into the engine - as the local touchscreen when a user asked,
 * else as the panel itself. The Activity list then says "Z-Wave device added" and the rest. */
static void zwave_log(void)
{
    static const alarm_source_t PANEL = {.kind = ALARM_SRC_SYSTEM, .id = 0};
    zwz_log_t l;
    while (zwz_panel_take_log(&H.zw.panel, &l)) {
        const alarm_result_t r = alarm_engine_platform_event_ex(
            &H.eng, (alarm_event_type_t)l.type, l.zone, l.arg, l.arg2,
            l.user != 0u ? &SRC : &PANEL, l.user, H.now);
        zwn_note("Z-Wave: logged %s (device %u, zone %u)%s",
                 alarm_event_name((alarm_event_type_t)l.type), (unsigned)l.arg,
                 (unsigned)l.zone, r == ALARM_OK ? "" : ": the engine refused it");
    }
}

/* ---- the log strip, and the simulator's buttons as the installer ----------------------------- */

static const char *zw_radio_words(uint8_t radio)
{
    switch (radio) {
    case UI_ZW_RADIO_WORKING: return "working";
    case UI_ZW_RADIO_STARTING: return "starting";
    case UI_ZW_RADIO_NOT_ANSWERING: return "not answering";
    case UI_ZW_RADIO_UNUSABLE: return "not a controller the panel can use";
    case UI_ZW_RADIO_FOREIGN: return "another system's";
    case UI_ZW_RADIO_REPLACED: return "replaced or reset";
    case UI_ZW_RADIO_UPDATING: return "updating";
    default: return "not connected";
    }
}

static const char *zw_op_words(uint8_t op)
{
    switch (op) {
    case UI_ZW_OP_ADD: return "adding";
    case UI_ZW_OP_REMOVE: return "removing";
    case UI_ZW_OP_REMOVE_FAILED: return "removing a broken";
    case UI_ZW_OP_PING: return "checking";
    case UI_ZW_OP_REGION: return "changing the region";
    case UI_ZW_OP_RESET: return "resetting the network";
    case UI_ZW_OP_RESTART: return "restarting the radio";
    default: return "working";
    }
}

static const char *zw_result_words(uint8_t result)
{
    switch (result) {
    case UI_ZW_RES_ADDED: return "added";
    case UI_ZW_RES_ADDED_INSECURE: return "added without encryption";
    case UI_ZW_RES_DOWNGRADED: return "added with weaker security";
    case UI_ZW_RES_NO_REPORTS: return "added, but it can't report";
    case UI_ZW_RES_NOTHING_JOINED: return "nothing joined";
    case UI_ZW_RES_JOIN_FAILED: return "it did not finish joining";
    case UI_ZW_RES_ALREADY_ADDED: return "already added";
    case UI_ZW_RES_CONTROLLER: return "another controller, not added";
    case UI_ZW_RES_STOPPED: return "stopped";
    case UI_ZW_RES_REMOVED: return "removed";
    case UI_ZW_RES_REMOVED_FOREIGN: return "another system's device was reset";
    case UI_ZW_RES_NOTHING_REMOVED: return "nothing was removed";
    case UI_ZW_RES_STILL_ANSWERS: return "it still answers";
    case UI_ZW_RES_ANSWERED: return "it answered";
    case UI_ZW_RES_NO_ANSWER: return "no answer";
    case UI_ZW_RES_DONE: return "done";
    case UI_ZW_RES_FAILED: return "failed";
    case UI_ZW_RES_SETUP_PAUSED: return "joined, its setup not finished";
    case UI_ZW_RES_TIMED_OUT: return "did not finish in time";
    case UI_ZW_RES_REFUSED: return "refused by the radio: nothing removed";
    default: return "ended";
    }
}

/* The operation the pages follow, as a line of the log strip. */
static void zw_progress_note(const ui_zw_progress_t *g)
{
    const unsigned node = g->node;
    switch (g->step) {
    case UI_ZW_STEP_STARTING: zwn_note("Z-Wave: %s: telling the radio", zw_op_words(g->op)); break;
    case UI_ZW_STEP_LISTENING:
        zwn_note("Z-Wave: listening for a device to %s...",
                 g->op == UI_ZW_OP_ADD ? "add" : "remove");
        break;
    case UI_ZW_STEP_CONNECTING: zwn_note("Z-Wave: a device answered"); break;
    case UI_ZW_STEP_ASK_ALLOW:
        zwn_note("Z-Wave: device %u asks for keys %02X - %s", node, (unsigned)g->requested,
                 H.zw.adding != 0u ? "the installer grants them" : "the pages decide");
        break;
    case UI_ZW_STEP_ASK_PIN:
        zwn_note("Z-Wave: device %u - type the PIN on its label", node);
        break;
    case UI_ZW_STEP_SECURING: zwn_note("Z-Wave: device %u joined; exchanging keys", node); break;
    case UI_ZW_STEP_READING:
        zwn_note("Z-Wave: device %u added; reading what it can do", node);
        break;
    case UI_ZW_STEP_WORKING: zwn_note("Z-Wave: %s...", zw_op_words(g->op)); break;
    case UI_ZW_STEP_DONE:
        if (node != 0u) {
            zwn_note("Z-Wave: %s device %u: %s", zw_op_words(g->op), node,
                     zw_result_words(g->result));
        } else {
            zwn_note("Z-Wave: %s: %s", zw_op_words(g->op), zw_result_words(g->result));
        }
        break;
    default: break; /* dismissed: nothing to say */
    }
}

/* The devices as the Z-Wave page lists them - set up, heard, gone - once a second of the
 * simulator's time: for the log strip only. */
static void zw_watch_devices(void)
{
    static ui_zw_device_t devs[UI_ZW_DEVICES_MAX];
    if (H.now < H.zw.seen.devices_at) {
        return;
    }
    const bool first = H.zw.seen.devices_at == 0u;
    H.zw.seen.devices_at = H.now + 1000u;
    const size_t n = zwz_panel_devices(&H.zw.panel, H.now, devs, UI_ZW_DEVICES_MAX);
    for (size_t i = 0; i < n; i++) {
        const ui_zw_device_t *d = &devs[i];
        const bool ready = d->state != UI_ZW_DEV_SETTING_UP && d->state != UI_ZW_DEV_WAKE_NEEDED &&
                           d->state != UI_ZW_DEV_SETUP_STOPPED;
        size_t k = 0;
        while (k < H.zw.seen.n && (H.zw.seen.dev[k].node != d->node ||
                                   H.zw.seen.dev[k].generation != d->generation)) {
            k++;
        }
        if (k == H.zw.seen.n) {
            if (!first) {
                zwn_note("Z-Wave: device %u is in the network%s", (unsigned)d->node,
                         ready ? "" : "; being set up");
            }
        } else if (ready && !H.zw.seen.dev[k].ready) {
            zwn_note("Z-Wave: device %u is set up", (unsigned)d->node);
        } else if (d->state == UI_ZW_DEV_SETUP_STOPPED && !H.zw.seen.dev[k].stopped) {
            zwn_note("Z-Wave: device %u stopped answering: its setup stopped", (unsigned)d->node);
        } else if (d->unreachable != H.zw.seen.dev[k].unreachable) {
            zwn_note(d->unreachable ? "Z-Wave: device %u missed its check-ins: UNREACHABLE"
                                    : "Z-Wave: device %u is heard again",
                     (unsigned)d->node);
        }
    }
    for (size_t k = 0; k < H.zw.seen.n; k++) {
        bool there = false;
        for (size_t i = 0; i < n && !there; i++) {
            there = devs[i].node == H.zw.seen.dev[k].node &&
                    devs[i].generation == H.zw.seen.dev[k].generation;
        }
        if (!there) {
            zwn_note("Z-Wave: device %u is no longer in the network",
                     (unsigned)H.zw.seen.dev[k].node);
        }
    }
    H.zw.seen.n = n;
    for (size_t i = 0; i < n; i++) {
        H.zw.seen.dev[i].node = devs[i].node;
        H.zw.seen.dev[i].generation = devs[i].generation;
        H.zw.seen.dev[i].ready = devs[i].state != UI_ZW_DEV_SETTING_UP &&
                                 devs[i].state != UI_ZW_DEV_WAKE_NEEDED &&
                                 devs[i].state != UI_ZW_DEV_SETUP_STOPPED;
        H.zw.seen.dev[i].stopped = devs[i].state == UI_ZW_DEV_SETUP_STOPPED;
        H.zw.seen.dev[i].unreachable = devs[i].unreachable;
    }
}

/* A request as the installer makes it on the pages; the simulator's buttons are their own
 * installer mode. */
static ui_zw_req_result_t zw_installer_asks(uint8_t kind, uint8_t keys, uint32_t pin)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof req);
    req.kind = kind;
    req.user = ALARM_USER_INSTALLER;
    req.keys = keys;
    req.pin = pin;
    return zwz_panel_request(&H.zw.panel, H.now, &req, true);
}

static bool zw_added(uint8_t result)
{
    return result == UI_ZW_RES_ADDED || result == UI_ZW_RES_ADDED_INSECURE ||
           result == UI_ZW_RES_DOWNGRADED || result == UI_ZW_RES_NO_REPORTS;
}

/* After every run: the log strip, and the simulator's own add buttons answering as the installer
 * on the pages would - the security question (allow what the device asks for) and, once the add
 * ended, the result seen (dismissed, as leaving the add page does). True when it asked for
 * something: the wire runs again at once. */
static bool zwave_watch(void)
{
    zwz_panel_t *p = &H.zw.panel;
    ui_zw_status_t st;
    zwz_panel_status(p, H.now, &st);
    /* a silent radio's recovery tries again and again: "not answering" is said once */
    const bool retry =
        st.radio == UI_ZW_RADIO_STARTING && H.zw.seen.radio == UI_ZW_RADIO_NOT_ANSWERING;
    if (st.radio != H.zw.seen.radio && !retry) {
        H.zw.seen.radio = st.radio;
        zwn_note("Z-Wave: the radio is %s", zw_radio_words(st.radio));
    }
    ui_zw_progress_t g;
    zwz_panel_progress(p, H.now, &g);
    if (g.op != H.zw.seen.op || g.step != H.zw.seen.step || g.result != H.zw.seen.result) {
        H.zw.seen.op = g.op;
        H.zw.seen.step = g.step;
        H.zw.seen.result = g.result;
        zw_progress_note(&g);
    }
    zw_watch_devices();
    if (g.op != UI_ZW_OP_ADD) {
        return false;
    }
    if (H.zw.adding != 0u && g.node != 0u &&
        (g.step == UI_ZW_STEP_READING || (g.step == UI_ZW_STEP_DONE && zw_added(g.result)))) {
        /* in the network: the button's questions are over, its end is still to be seen */
        if (H.zw.adding == 1u) {
            H.zw.door_node = g.node;
        } else {
            H.zw.motion_node = g.node;
        }
        H.zw.adding = 0u;
        H.zw.button_node = g.node;
    }
    if (H.zw.adding != 0u && g.step == UI_ZW_STEP_ASK_ALLOW) {
        return zw_installer_asks(UI_ZW_REQ_ALLOW, g.requested, 0u) == UI_ZW_REQ_OK;
    }
    if (g.step == UI_ZW_STEP_DONE &&
        (H.zw.adding != 0u || (H.zw.button_node != 0u && g.node == H.zw.button_node))) {
        /* its end is left for whoever follows it (the pages show it; B015 UX-004: dismissing it
         * here took the result away from a page that was showing this add) */
        H.zw.adding = 0u;
        H.zw.button_node = 0u;
    }
    return false;
}

/* The world up to H.now - the wire, the module and its devices, zwz_panel's news and polls - then
 * what zwave_service does after unlocking (store, log) and the log strip; once more at once when
 * the buttons' installer answered something. */
/* ---- ZW-033: the simulated update service ------------------------------------------------- */

static bool zw_allowed(void);

/* The file it offers: a Gecko bootloader image (its tag, then a pattern). */
static size_t sim_fw_file(uint8_t *out, size_t cap)
{
    static const uint8_t TAG[4] = {0xEBu, 0x17u, 0xA6u, 0x03u};
    const size_t n = 600u < cap ? 600u : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = i < sizeof TAG ? TAG[i] : (uint8_t)(i * 7u + 3u);
    }
    return n;
}

/* The radio's own firmware on offer (network.md §14): a whole .gbl the fake bootloader takes. */
static void sim_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8 & 0xFFu);
    p[2] = (uint8_t)(v >> 16 & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static size_t sim_radio_file(uint8_t *out, size_t cap)
{
    const size_t data = 1500u;
    if (cap < 16u + 12u + data + 12u) {
        return 0u;
    }
    size_t at = 0u;
    sim_u32(&out[at], ZW_GBL_TAG_HEADER);
    sim_u32(&out[at + 4u], 8u);
    sim_u32(&out[at + 8u], 0x03000000u);
    sim_u32(&out[at + 12u], 0u);
    at += 16u;
    sim_u32(&out[at], 0xF40A0AF4u);
    sim_u32(&out[at + 4u], (uint32_t)(4u + data));
    sim_u32(&out[at + 8u], 0u);
    for (size_t i = 0; i < data; i++) {
        out[at + 12u + i] = (uint8_t)(i * 11u + 1u);
    }
    at += 12u + data;
    sim_u32(&out[at], ZW_GBL_TAG_END);
    sim_u32(&out[at + 4u], 4u);
    sim_u32(&out[at + 8u], zw_crc32(0u, out, at + 8u));
    return at + 12u;
}

/* The service's answer for the radio: 2.30 while it runs something older. */
static size_t sim_radio_answer(char *out, size_t cap)
{
    static uint8_t file[2048];
    const size_t n = sim_radio_file(file, sizeof file);
    uint8_t d[32];
    proto_sha256(file, n, d);
    char sha[65];
    for (size_t i = 0; i < 32u; i++) {
        snprintf(&sha[2u * i], 3u, "%02x", d[i]);
    }
    const zwfc_device_t *dev = &H.zw.radiojob.dev;
    const bool offer = dev->fw_major < 2u || (dev->fw_major == 2u && dev->fw_minor < 30u);
    return (size_t)snprintf(
        out, cap,
        "[{\"manufacturerId\":\"0x%04x\",\"productType\":\"0x%04x\",\"productId\":\"0x%04x\","
        "\"firmwareVersion\":\"%u.%u\",\"updates\":[%s%s%s]}]",
        (unsigned)dev->manufacturer, (unsigned)dev->product_type, (unsigned)dev->product_id,
        (unsigned)dev->fw_major, (unsigned)dev->fw_minor,
        offer ? "{\"version\":\"2.30\",\"changelog\":\"Z-Wave SDK 7.24.2.\",\"channel\":\"stable\","
                "\"downgrade\":false,\"files\":[{\"target\":0,\"url\":"
                "\"https://example.com/zwave/radio-2.30.gbl\",\"integrity\":\"sha256:"
              : "",
        offer ? sha : "", offer ? "\"}]}" : "");
}

static const char *zw_region_word(void);

/* The radio's exchange a second later: the check's answer, or its file checked and sent. */
static void zwave_radio_run(void)
{
    if (H.zw.radio_due == 0u || H.now < H.zw.radio_due) {
        return;
    }
    H.zw.radio_due = 0u;
    zwfj_t *j = &H.zw.radiojob;
    if (j->state == UI_ZW_FW_CHECKING) {
        static char answer[1024];
        const size_t n = sim_radio_answer(answer, sizeof answer);
        zwfj_check_answer(j, H.zw.fw_offline ? 0 : 200, answer, n, zw_region_word());
    } else if (j->state == UI_ZW_FW_DOWNLOADING) {
        static uint8_t file[2048];
        const size_t len = sim_radio_file(file, sizeof file);
        const size_t n = zwfj_file(j, file, len, H.zw.radio_image, sizeof H.zw.radio_image);
        if (n == 0u) {
            return;
        }
        H.zw.zac.next_firmware[0] = 2u;
        H.zw.zac.next_firmware[1] = 30u;
        H.zw.zac.otw_block_ms = 400u; /* its 13 blocks take a few seconds */
        const ui_zw_req_result_t r = zwz_panel_radio_update(&H.zw.panel, H.now, H.zw.radio_image,
                                                            n, zw_allowed(), H.zw.fw_user);
        if (r == UI_ZW_REQ_OK) {
            zwfj_started(j);
        } else {
            zwfj_not_started(j, r);
        }
    }
}

/* Its answer: version 1.20 for the door and motion sensors and the keypad while they run 1.10,
 * nothing newer after; any other device unknown. */
static size_t sim_fw_answer(char *out, size_t cap)
{
    uint8_t file[600];
    const size_t n = sim_fw_file(file, sizeof file);
    uint8_t d[32];
    proto_sha256(file, n, d);
    char sha[65];
    for (size_t i = 0; i < 32u; i++) {
        snprintf(&sha[2u * i], 3u, "%02x", d[i]);
    }
    const zwfc_device_t *dev = &H.zw.fwjob.dev;
    const bool door = dev->manufacturer == H.zw.door.m.manufacturer &&
                      dev->product_type == H.zw.door.m.product_type &&
                      dev->product_id == H.zw.door.m.product_id;
    const bool motion = dev->manufacturer == H.zw.motion.m.manufacturer &&
                        dev->product_type == H.zw.motion.m.product_type &&
                        dev->product_id == H.zw.motion.m.product_id;
    const bool keypad = dev->manufacturer == H.zw.keypad.m.manufacturer &&
                        dev->product_type == H.zw.keypad.m.product_type &&
                        dev->product_id == H.zw.keypad.m.product_id;
    if (!door && !motion && !keypad) {
        return (size_t)snprintf(out, cap, "[]");
    }
    const bool offer = dev->fw_major == 1u && dev->fw_minor < 20u;
    return (size_t)snprintf(
        out, cap,
        "[{\"manufacturerId\":\"0x%04x\",\"productType\":\"0x%04x\",\"productId\":\"0x%04x\","
        "\"firmwareVersion\":\"%u.%u\",\"updates\":[%s%s%s]}]",
        (unsigned)dev->manufacturer, (unsigned)dev->product_type, (unsigned)dev->product_id,
        (unsigned)dev->fw_major, (unsigned)dev->fw_minor,
        offer ? "{\"version\":\"1.20\",\"changelog\":\"Longer battery life.\\nFixes a tamper "
               "report that could be missed.\",\"channel\":\"stable\",\"downgrade\":false,"
               "\"files\":[{\"target\":0,\"url\":\"https://example.com/zwave/sensor-1.20.gbl\","
               "\"integrity\":\"sha256:"
             : "",
        offer ? sha : "", offer ? "\"}]}" : "");
}

static const char *zw_region_word(void)
{
    const zwz_cfg_t *c = &H.zw.panel.setup.cfg;
    return c->region_set ? zwfc_region(c->region) : NULL;
}

/* What the service's HTTPS exchange is on the panel, a second later: the check's answer, or the
 * download (checked, made the image, the transfer started). */
static void zwave_fw_run(void)
{
    if (H.zw.fw_due == 0u || H.now < H.zw.fw_due) {
        return;
    }
    H.zw.fw_due = 0u;
    zwfj_t *j = &H.zw.fwjob;
    if (j->state == UI_ZW_FW_CHECKING) {
        if (H.zw.fw_offline) {
            zwfj_check_answer(j, 0, NULL, 0u, zw_region_word());
            return;
        }
        static char answer[1024];
        const size_t n = sim_fw_answer(answer, sizeof answer);
        zwfj_check_answer(j, 200, answer, n, zw_region_word());
    } else if (j->state == UI_ZW_FW_DOWNLOADING) {
        if (H.zw.fw_offline) {
            zwfj_download_failed(j, 0);
            return;
        }
        static uint8_t file[1024];
        const size_t len = sim_fw_file(file, sizeof file);
        const size_t n = zwfj_file(j, file, len, H.zw.fw_image, sizeof H.zw.fw_image);
        if (n == 0u) {
            return;
        }
        const ui_zw_req_result_t r =
            zwz_panel_fw_start(&H.zw.panel, H.now, j->node, j->generation, 0u, H.zw.fw_image, n,
                               zw_allowed(), H.zw.fw_user);
        if (r == UI_ZW_REQ_OK) {
            zwfj_started(j);
        } else {
            zwfj_not_started(j, r);
        }
    }
}

static void zwave_world_run(void)
{
    if (H.zw.smart_powered && H.zw.smart.status == FAKE_ZDEV_OUT && H.now >= H.zw.smart_next_ask) {
        H.zw.smart_next_ask = H.now + 8000u; /* heard only while the radio listens for it */
        if (fake_zac93_smartstart_ask(&H.zw.zac, H.now, &H.zw.smart)) {
            zwn_note("Z-Wave: the SmartStart sensor asks to join");
        }
    }
    for (unsigned i = 0; i < 4u; i++) {
        zwave_fw_run();
        zwave_radio_run();
        zwave_feedback_poll();
        fake_wire_run_until(&H.zw.wire, H.now);
        zwave_keyctl_run();
        if (H.zw.wire.stuck && !H.zw.stuck_said) {
            H.zw.stuck_said = true;
            zwn_note("Z-Wave: the simulated wire stopped settling");
        }
        zwave_persist();
        zwave_log();
        /* network.md §14, CORE-ARM-013: arming held while the radio updates, as zwave_service */
        const bool held = zwz_panel_arming_held(&H.zw.panel, H.now);
        if (held != alarm_engine_arming_held(&H.eng)) {
            alarm_engine_hold_arming(&H.eng, held);
        }
        if (!zwave_watch()) {
            break;
        }
    }
}

/* ---- the world ------------------------------------------------------------------------------- */

/* A whole simulator reset: the radio, the devices, the zone source, the panel's Z-Wave and what it
 * stored are all new. Called from host_panel_init() after zones_start() (H.hub exists by then), or
 * from host_panel_zwave_begin(). */
static void zwave_world_init(void)
{
    memset(&H.zw, 0, sizeof H.zw);
    zw_host_crypto_init(&H.zw.hc, 0x5A574156455A4143ull); /* "ZWAVEZAC", a fixed seed */
    H.zw.crypto = zw_host_crypto_port(&H.zw.hc);
    fake_zac93_init(&H.zw.zac);

    /* Realistic per-model events (docs/features/36 Z7): a real ZSE41 reports only its door
     * open/closed and its cover tamper, not every Notification event 1..23 a bare CC list would
     * let the fake claim - the picker must not offer a door contact "· motion" (fake_zdev_cc.c
     * answers Notification Event Supported Get with events 1..23 unless told otherwise). */
    static const uint8_t ACCESS_EVENTS[] = {0x16u, 0x17u};         /* door open / closed */
    static const uint8_t SECURITY_EVENTS_ZSE41[] = {0x03u};        /* cover tamper only */
    static const uint8_t SECURITY_EVENTS_ZSE18[] = {0x03u, 0x08u}; /* tamper, motion */
    static const uint8_t SECURITY_EVENTS_ZSE43[] = {0x03u, 0x09u}; /* tamper, product moved */

    fake_zdev_init(&H.zw.door, 0xD00Fu); /* the Zooz-like sleeping S2 door sensor */
    H.zw.door.m.answers = true; /* the default is "record only", for the lower layers' tests */
    H.zw.door.nif_fallback = true; /* as a real S2 device: a failed exchange -> all CCs in the clear */
    fake_zdev_notification_events(&H.zw.door, ZW_NOTIFICATION_TYPE_ACCESS_CONTROL, ACCESS_EVENTS,
                                  sizeof ACCESS_EVENTS);
    fake_zdev_notification_events(&H.zw.door, ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                  SECURITY_EVENTS_ZSE41, sizeof SECURITY_EVENTS_ZSE41);
    /* ZW-033: it takes software updates (1.10 -> the simulated service's 1.20) */
    H.zw.door.m.secure_ccs[H.zw.door.m.secure_n++] = ZW_CC_FIRMWARE_UPDATE;
    H.zw.door.m.versions[H.zw.door.m.versions_n++] = (fake_cc_version_t){ZW_CC_FIRMWARE_UPDATE, 5};
    H.zw.door.fw.on = true;
    H.zw.door.fw.firmware_id = 0x0041u;
    H.zw.door.fw.wait_s = 5u;
    H.zw.door.fw.after_major = 1u;
    H.zw.door.fw.after_minor = 20u;
    fake_zdev_init(&H.zw.motion, 0x0F0Fu);
    /* a Zooz ZSE18 (the quirks table's), not a second ZSE41 */
    H.zw.motion.m.product_type = 0x0301u;
    H.zw.motion.m.product_id = 0x0012u;
    fake_zdev_no_s2(&H.zw.motion);
    fake_zdev_listening(&H.zw.motion);
    /* Association and AGI: its lifeline (network.md §6) - without it a sensor reports nothing to
     * the panel and offers no input (zones.md §2). Mains powered: no Battery, no Wake Up. */
    static const uint8_t MOTION_NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x7A};
    memcpy(H.zw.motion.nif, MOTION_NIF, sizeof MOTION_NIF);
    H.zw.motion.nif_len = sizeof MOTION_NIF;
    H.zw.motion.m.notif_types[0] = ZW_NOTIFICATION_TYPE_HOME_SECURITY;
    H.zw.motion.m.notif_n = 1u;
    H.zw.motion.m.secure_n = 0u;
    H.zw.motion.m.answers = true;
    H.zw.motion.m.versions[H.zw.motion.m.versions_n++] =
        (fake_cc_version_t){ZW_CC_FIRMWARE_UPDATE, 5};
    H.zw.motion.fw.on = true;
    H.zw.motion.fw.firmware_id = 0x0012u;
    H.zw.motion.fw.wait_s = 3u;
    H.zw.motion.fw.after_major = 1u;
    H.zw.motion.fw.after_minor = 20u;
    fake_zdev_notification_events(&H.zw.motion, ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                  SECURITY_EVENTS_ZSE18, sizeof SECURITY_EVENTS_ZSE18);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.door);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.motion);

    /* Z7b: a spare of each kind, in range but not yet joined - host_panel_zwave_press_spare_*()
     * presses its button once the pages' own "Add a device" has told the module to listen. */
    fake_zdev_init(&H.zw.spare_secure, 0x5A5Eu);
    H.zw.spare_secure.m.answers = true;
    H.zw.spare_secure.nif_fallback = true;
    fake_zdev_notification_events(&H.zw.spare_secure, ZW_NOTIFICATION_TYPE_ACCESS_CONTROL,
                                  ACCESS_EVENTS, sizeof ACCESS_EVENTS);
    fake_zdev_notification_events(&H.zw.spare_secure, ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                  SECURITY_EVENTS_ZSE41, sizeof SECURITY_EVENTS_ZSE41);
    fake_zdev_init(&H.zw.spare_insecure, 0x0F0Eu);
    /* Zooz ZSE43 tilt and shock sensor (zw_quirks.c): a distinct name from the motion sensor
     * above, so a test can tell their candidate rows apart. A real ZSE43 is battery powered and
     * sleeps like the door sensor; unlike it, no Security 2 - so, like the motion sensor, its
     * lifeline (Association, AGI) must be in its plain node information, not only in a secure
     * command class list it never gets asked for; Battery and Wake Up are added to it for the
     * check-in and battery level a sleeper needs (door_model()'s 90 % battery is left as it is). */
    static const uint8_t ZSE43_NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x80, 0x84};
    H.zw.spare_insecure.m.product_type = 0x7000u;
    H.zw.spare_insecure.m.product_id = 0xE003u;
    fake_zdev_no_s2(&H.zw.spare_insecure);
    memcpy(H.zw.spare_insecure.nif, ZSE43_NIF, sizeof ZSE43_NIF);
    H.zw.spare_insecure.nif_len = sizeof ZSE43_NIF;
    H.zw.spare_insecure.m.notif_types[0] = ZW_NOTIFICATION_TYPE_ACCESS_CONTROL; /* tilt */
    H.zw.spare_insecure.m.notif_types[1] = ZW_NOTIFICATION_TYPE_HOME_SECURITY;
    H.zw.spare_insecure.m.notif_n = 2u;
    H.zw.spare_insecure.m.secure_n = 0u;
    H.zw.spare_insecure.m.answers = true;
    fake_zdev_notification_events(&H.zw.spare_insecure, ZW_NOTIFICATION_TYPE_ACCESS_CONTROL,
                                  ACCESS_EVENTS, sizeof ACCESS_EVENTS);
    fake_zdev_notification_events(&H.zw.spare_insecure, ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                  SECURITY_EVENTS_ZSE43, sizeof SECURITY_EVENTS_ZSE43);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.spare_secure);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.spare_insecure);

    /* ZW-028/029: a keypad (a keypad model with a feedback profile: its sounds and lights can
     * show the panel's state) and a device that stops answering right after joining. Neither is
     * in the quirks table: the pages call them "Z-Wave device". */
    fake_zdev_init(&H.zw.keypad, 0x4B50u);
    H.zw.keypad.m.answers = true;
    H.zw.keypad.nif_fallback = true;
    fake_zdev_listening(&H.zw.keypad);
    H.zw.keypad.m.listening = false;
    H.zw.keypad.m.flirs = true;
    H.zw.keypad.m.manufacturer = 0x0346u;
    H.zw.keypad.m.product_type = 0x0101u;
    H.zw.keypad.m.product_id = 0x0301u;
    H.zw.keypad.m.notif_n = 0u;
    H.zw.keypad.m.secure_ccs[H.zw.keypad.m.secure_n++] = ZW_CC_INDICATOR;
    H.zw.keypad.m.versions[H.zw.keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_INDICATOR, 3};
    H.zw.keypad.m.secure_ccs[H.zw.keypad.m.secure_n++] = ZW_CC_ENTRY_CONTROL;
    H.zw.keypad.m.versions[H.zw.keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_ENTRY_CONTROL, 1};
    H.zw.keypad.m.secure_ccs[H.zw.keypad.m.secure_n++] = ZW_CC_BATTERY;
    H.zw.keypad.m.versions[H.zw.keypad.m.versions_n++] = (fake_cc_version_t){ZW_CC_BATTERY, 1};
    /* ZW-033: it takes software updates too (woken by the panel's call: no wake-up to wait for) */
    H.zw.keypad.m.secure_ccs[H.zw.keypad.m.secure_n++] = ZW_CC_FIRMWARE_UPDATE;
    H.zw.keypad.m.versions[H.zw.keypad.m.versions_n++] =
        (fake_cc_version_t){ZW_CC_FIRMWARE_UPDATE, 5};
    H.zw.keypad.fw.on = true;
    H.zw.keypad.fw.firmware_id = 0x0101u;
    H.zw.keypad.fw.wait_s = 3u;
    H.zw.keypad.fw.after_major = 1u;
    H.zw.keypad.fw.after_minor = 20u;
    fake_zdev_init(&H.zw.stray, 0x5747u);
    H.zw.stray.m.answers = true;
    fake_zdev_listening(&H.zw.stray);
    H.zw.stray.m.listening = false;
    H.zw.stray.m.flirs = true;
    H.zw.stray.nif_len = 0u; /* its node information came empty */
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.keypad);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.stray);
    static const fake_zdev_t *const switchers[3] = {&H.zw.light, &H.zw.relay, &H.zw.dimmer};
    for (size_t k = 0; k < 3u; k++) {
        fake_zdev_t *d = (fake_zdev_t *)switchers[k];
        fake_zdev_init(d, k == 0u ? 0x4C49u : k == 1u ? 0x5245u : 0x444Du);
        d->m.answers = true;
        d->nif_fallback = true;
        fake_zdev_listening(d);
        d->m.notif_n = 0u;
        d->m.sw_report = true;
        d->m.manufacturer = 0x027Au;
        d->m.product_type = 0x5A00u; /* made up: not in the quirks table */
        d->m.product_id = k == 0u ? 0x0071u : k == 1u ? 0x0016u : 0x0072u;
        const uint8_t cc = k == 2u ? ZW_CC_MULTILEVEL_SWITCH : ZW_CC_BINARY_SWITCH;
        d->m.secure_ccs[d->m.secure_n++] = cc;
        d->m.versions[d->m.versions_n++] = (fake_cc_version_t){cc, k == 2u ? 4 : 2};
    }
    H.zw.relay.m.secure_ccs[H.zw.relay.m.secure_n++] = 0x60u;
    H.zw.relay.m.versions[H.zw.relay.m.versions_n++] = (fake_cc_version_t){0x60u, 4};
    H.zw.relay.m.ep_n = 3u;
    for (size_t e = 0; e < 3u; e++) {
        fake_ep_t *ep = &H.zw.relay.m.eps[e];
        ep->generic = 0x10u;
        ep->specific = 0x01u;
        ep->ccs[0] = 0x5Eu;
        ep->cc_n = 1u;
        ep->secure_ccs[0] = ZW_CC_BINARY_SWITCH;
        ep->secure_n = 1u;
    }
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.light);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.relay);
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.dimmer);
    fake_zdev_init(&H.zw.smart, 0x534D5254u); /* "SMRT" */
    H.zw.smart.m.answers = true;
    fake_zdev_listening(&H.zw.smart); /* powered and awake while it asks, and after */
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.smart);
    fake_zdev_init(&H.zw.spare_light, 0x53504C54u); /* "SPLT" */
    H.zw.spare_light.m.answers = true;
    H.zw.spare_light.nif_fallback = true;
    fake_zdev_listening(&H.zw.spare_light);
    H.zw.spare_light.m.notif_n = 0u;
    H.zw.spare_light.m.sw_report = true;
    H.zw.spare_light.m.manufacturer = 0x027Au;
    H.zw.spare_light.m.product_type = 0x5A00u;
    H.zw.spare_light.m.product_id = 0x0071u;
    H.zw.spare_light.m.secure_ccs[H.zw.spare_light.m.secure_n++] = ZW_CC_BINARY_SWITCH;
    H.zw.spare_light.m.versions[H.zw.spare_light.m.versions_n++] =
        (fake_cc_version_t){ZW_CC_BINARY_SWITCH, 2};
    (void)fake_zac93_add_device(&H.zw.zac, &H.zw.spare_light);
    zwf_init(&H.zw.feedback);
    static const zwk_port_t kc_port = {.ctx = NULL, .command = kc_engine};
    zwkc_init(&H.zw.keyctl, &kc_port);

    /* The zone source joins the hub as zwave_service_start() registers it: its sink straight into
     * the same H.hub every other source uses (the firmware's zone_service takes a lock around
     * them; here there is only one thread). */
    static const zsrc_sink_t sink = {.ctx = NULL,
                                     .input = sink_input,
                                     .event = sink_event,
                                     .reachable = sink_reachable,
                                     .link = sink_link,
                                     .link_now = sink_link_now};
    zsrc_zw_init(&H.zw.src, &sink);
    (void)zone_hub_register(&H.hub, zsrc_zw_ops(), &H.zw.src, H.now);

    zwave_panel_boot(); /* a new panel: nothing stored yet */
    H.zw.on = true;
    zwn_note("Z-Wave: ready. The door sensor's label PIN is %05u.", (unsigned)H.zw.door.pin);
}

void host_panel_zwave_later(void)
{
    s_zw_later = true;
}

void host_panel_zwave_begin(void)
{
    if (!H.zw.on) {
        zwave_world_init();
        after();
    }
}

/* ---- the simulator's buttons ----------------------------------------------------------------- */

/* "Add a device" as the installer asks for it on the pages (the button is its own installer mode),
 * then the device's own button a moment later - two taps in real life too (host/ui_sim's button
 * stands for both). Its security question and its end are answered in zwave_watch(), its PIN by
 * host_panel_zwave_pin(). */
static void zw_button_add(uint8_t which, fake_zdev_t *d, bool *added, const char *what)
{
    if (!H.zw.on) {
        return;
    }
    if (*added) {
        zwn_note("Z-Wave: the %s is already in the network", what);
        return;
    }
    const ui_zw_req_result_t r = zw_installer_asks(UI_ZW_REQ_ADD, 0u, 0u);
    if (r != UI_ZW_REQ_OK) {
        zwn_note("Z-Wave: could not start listening (%s)",
                 r == UI_ZW_REQ_BUSY ? "busy" : "the radio isn't working");
        return;
    }
    H.zw.adding = which;
    zwave_world_run();
    host_panel_advance(100u); /* AddNode(any) needs a moment to reach the module first */
    (void)fake_zac93_press(&H.zw.zac, H.now, d);
    *added = true;
}

void host_panel_zwave_add_secure(void)
{
    zw_button_add(1u, &H.zw.door, &H.zw.door_added, "door sensor");
}

void host_panel_zwave_add_insecure(void)
{
    zw_button_add(2u, &H.zw.motion, &H.zw.motion_added, "motion sensor");
}

/* Z7b: the spares' buttons, for the add pages' own walk-through (screens.md §4/§13) - unlike
 * host_panel_zwave_add_secure/insecure, these do not also ask the radio to listen: the pages' own
 * "Add a device" (ui_backend_t zw_request) already did, and the installer at the pages answers
 * the inclusion's questions. */
void host_panel_zwave_press_spare_secure(void)
{
    if (!H.zw.on) {
        return;
    }
    (void)fake_zac93_press(&H.zw.zac, H.now, &H.zw.spare_secure);
}

void host_panel_zwave_press_spare_insecure(void)
{
    if (!H.zw.on) {
        return;
    }
    (void)fake_zac93_press(&H.zw.zac, H.now, &H.zw.spare_insecure);
}

/* ZW-028/029: the keypad's and the stray device's buttons, once the pages' own "Add a device" is
 * listening. The stray one joins, and from then on hears nothing (reset, or gone out of range). */
void host_panel_zwave_press_keypad(void)
{
    if (H.zw.on) {
        (void)fake_zac93_press(&H.zw.zac, H.now, &H.zw.keypad);
    }
}

uint16_t host_panel_zwave_keypad_pin(void)
{
    return H.zw.keypad.pin;
}

void host_panel_zwave_press_stray(void)
{
    if (H.zw.on && fake_zac93_press(&H.zw.zac, H.now, &H.zw.stray)) {
        H.zw.stray.silent = true;
    }
}

/* The keypad's buttons: a code typed and Away / Home / Disarm (or an emergency button, code
 * NULL) pressed - its Entry Control Notification, at its own class, as a real keypad sends it. */
void host_panel_zwave_keypad_keys(const char *code, uint8_t event)
{
    if (!H.zw.on) {
        return;
    }
    const zw_cc_entry_notification_t n = {++H.zw.kc_seq,
                                          code != NULL ? ZW_ENTRY_DATA_ASCII : ZW_ENTRY_DATA_NONE,
                                          event, (const uint8_t *)code,
                                          code != NULL ? strlen(code) : 0u};
    uint8_t cmd[40];
    const size_t len = zw_cc_entry_notification_build(cmd, sizeof cmd, &n);
    const zw_s2_result_t r = fake_zdev_report(&H.zw.keypad, H.now, ZW_SEC_AUTO, cmd, len);
    zw_wipe(cmd, sizeof cmd);
    zwn_note("Z-Wave: the keypad's %s button%s", event == ZW_ENTRY_EVENT_ARM_AWAY  ? "Away"
                                                 : event == ZW_ENTRY_EVENT_ARM_HOME ? "Home"
                                                 : event == ZW_ENTRY_EVENT_DISARM_ALL
                                                     ? "Disarm"
                                                     : "emergency",
             r == ZW_S2_OK ? "" : " (not sent)");
}

alarm_result_t host_panel_zwave_keypad_last(void)
{
    return H.zw.kc_last;
}

/* The keypad's power is taken away (or its batteries): it hears nothing. */
void host_panel_zwave_keypad_silent(bool silent)
{
    H.zw.keypad.silent = silent;
}

/* Its next n frames never reach the module: it hears and ACKs the panel, its answers are lost. */
void host_panel_zwave_keypad_lose_replies(unsigned n)
{
    H.zw.keypad.lose_out = n;
}

/* The radio never counts a device as failed (a module that keeps a ghost). */
void host_panel_zwave_radio_keeps_failed(bool keeps)
{
    H.zw.zac.failed_off = keeps;
}

/* The node the stray or the keypad got: in the panel's list, and in the radio's own list. */
static uint16_t zw_node_of(const fake_zdev_t *d)
{
    return d->node;
}

uint16_t host_panel_zwave_stray_node(void)
{
    return zw_node_of(&H.zw.stray);
}

uint16_t host_panel_zwave_keypad_node(void)
{
    return zw_node_of(&H.zw.keypad);
}

bool host_panel_zwave_in_radio(uint16_t node)
{
    return node != 0u && zw_node_list_test(H.zw.zac.id.node_list, node);
}

bool host_panel_zwave_in_panel(uint16_t node)
{
    return H.zw.on && node != 0u && zw_node_find_const(&H.zw.panel.net.nodes, node) != NULL;
}

/* The keypad's own sounds and lights: the Indicator Sets it got (their first indicator ID). */
unsigned host_panel_zwave_keypad_indicators(uint8_t *last_id)
{
    unsigned n = 0u;
    for (size_t i = 0; i < H.zw.keypad.got_n && i < sizeof H.zw.keypad.got / sizeof H.zw.keypad.got[0];
         i++) {
        if (H.zw.keypad.got[i][0] == ZW_CC_INDICATOR) {
            n++;
            if (last_id != NULL) {
                *last_id = H.zw.keypad.got[i][4];
            }
        }
    }
    return n;
}

/* The spare's own PIN (its label, in fake_zdev_init's own words): what a test types back through
 * the UI's own PIN page, the way an installer reads it off the box. */
uint16_t host_panel_zwave_spare_secure_pin(void)
{
    return H.zw.spare_secure.pin;
}

/* The ZSE43's button pressed once: it wakes up and checks in, as a sleeping sensor does - what
 * was waiting for it (a check-in chosen on its page, network.md §7) goes out then. */
void host_panel_zwave_wake_spare_insecure(void)
{
    if (!H.zw.on) {
        return;
    }
    fake_zdev_wake(&H.zw.spare_insecure, H.now);
    zwn_note("Z-Wave: the tilt sensor wakes up");
}

/* ZW-034/035: the light switch's and the relay kit's pairing buttons (the pages' "Add a device"
 * listening); the light switch pressed by hand; the tilt sensor's door moving. */
void host_panel_zwave_press_light(void)
{
    if (H.zw.on) {
        (void)fake_zac93_press(&H.zw.zac, H.now, &H.zw.light);
    }
}

void host_panel_zwave_press_dimmer(void)
{
    if (H.zw.on) {
        (void)fake_zac93_press(&H.zw.zac, H.now, &H.zw.dimmer);
    }
}

uint16_t host_panel_zwave_dimmer_pin(void)
{
    return H.zw.dimmer.pin;
}

uint8_t host_panel_zwave_dimmer_level(void)
{
    return H.zw.dimmer.m.sw[0];
}

void host_panel_zwave_press_relay(void)
{
    if (H.zw.on) {
        (void)fake_zac93_press(&H.zw.zac, H.now, &H.zw.relay);
    }
}

void host_panel_zwave_light_toggle(void)
{
    if (!H.zw.on) {
        return;
    }
    const uint8_t v = H.zw.light.m.sw[0] != 0u ? 0x00u : 0xFFu;
    fake_zdev_switch_press(&H.zw.light, H.now, 0u, ZW_CC_BINARY_SWITCH, v);
    zwn_note("Z-Wave: the light switch is pressed %s", v != 0u ? "on" : "off");
}

void host_panel_zwave_keypad_jam_report(void)
{
    if (!H.zw.on) {
        return;
    }
    /* Notification: Home Security, "RF jamming detected" (network.md §9) */
    static const uint8_t cmd[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x0C, 0x00};
    (void)fake_zdev_report(&H.zw.keypad, H.now, ZW_SEC_AUTO, cmd, sizeof cmd);
    zwn_note("Z-Wave: the keypad reports radio jamming around it");
}

void host_panel_zwave_tilt(bool open)
{
    if (!H.zw.on) {
        return;
    }
    const uint8_t cmd[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, open ? 0x16 : 0x17, 0x00};
    fake_zdev_wake(&H.zw.spare_insecure, H.now);
    (void)fake_zdev_report(&H.zw.spare_insecure, H.now, ZW_SEC_AUTO, cmd, sizeof cmd);
    zwn_note("Z-Wave: the garage door is %s (tilt sensor)", open ? "open" : "closed");
}

/* The relay kit's relay `ep` as it is now (0 off, FF on), for a test of the press. */
uint8_t host_panel_zwave_relay_level(uint8_t ep)
{
    return ep <= 3u ? H.zw.relay.m.sw[ep] : 0u;
}

uint32_t host_panel_zwave_relay_sets(void)
{
    return H.zw.relay.m.sw_sets;
}

uint16_t host_panel_zwave_light_pin(void)
{
    return H.zw.light.pin;
}

uint16_t host_panel_zwave_relay_pin(void)
{
    return H.zw.relay.pin;
}

/* ZW-024 §9: every channel as loud as `dbm` (the simulated background noise). */
void host_panel_zwave_noise(int dbm)
{
    for (size_t i = 0; i < 3u; i++) {
        H.zw.zac.bg_rssi[i] = (int8_t)dbm;
    }
    zwn_note("Z-Wave: the background noise is %d dBm", dbm);
}

/* ZW-024 §10: the light switch breaks (it hears nothing; the radio lists it as failed only once a
 * message to it goes unanswered, such as the replacement check's pings); the spare one's pairing
 * button, while "Replace it" waits. */
void host_panel_zwave_break_light(void)
{
    if (!H.zw.on) {
        return;
    }
    H.zw.light.silent = true;
    zwn_note("Z-Wave: the light switch broke");
}

void host_panel_zwave_press_spare_light(void)
{
    if (H.zw.on && fake_zac93_press_replace(&H.zw.zac, H.now, &H.zw.spare_light)) {
        zwn_note("Z-Wave: the spare light switch joins in the broken one's place");
    }
}

uint16_t host_panel_zwave_spare_light_pin(void)
{
    return H.zw.spare_light.pin;
}

/* Something the simulated radio had no room for (a device past FAKE_ZAC93_DEVICES...): 0. */
uint32_t host_panel_zwave_fake_overflow(void)
{
    return H.zw.zac.overflow;
}

/* ZW-024: the SmartStart sensor gets its battery: it asks to join until it is in. */
void host_panel_zwave_smart_power(void)
{
    if (!H.zw.on) {
        return;
    }
    H.zw.smart_powered = true;
    H.zw.smart_next_ask = H.now;
    zwn_note("Z-Wave: the SmartStart sensor is powered");
}

/* Its button while the pages' "Remove a device" listens (an exclusion resets it: powered, it asks
 * to join again). */
void host_panel_zwave_press_smart_remove(void)
{
    if (H.zw.on && fake_zac93_press_remove(&H.zw.zac, H.now, &H.zw.smart)) {
        H.zw.smart_next_ask = H.now + 8000u;
    }
}

/* Its 40-digit key as its label prints it (the PIN first). */
void host_panel_zwave_smart_key(char *out, size_t cap)
{
    snprintf(out, cap, "%s", H.zw.smart.dsk);
}

bool host_panel_zwave_smart_joined(void)
{
    return H.zw.smart.status != FAKE_ZDEV_OUT;
}

/* The door sensor's button pressed once: it wakes up (a software update waiting for it starts). */
void host_panel_zwave_wake_door(void)
{
    if (!H.zw.on) {
        return;
    }
    fake_zdev_wake(&H.zw.door, H.now);
    zwn_note("Z-Wave: the door sensor wakes up");
}

/* ZW-045: the module offers this region too, from its next start-up. */
void host_panel_zwave_offer_region(uint8_t code)
{
    if (!H.zw.on || H.zw.zac.id.region_count >= FAKE_REGIONS_MAX) {
        return;
    }
    for (size_t i = 0; i < H.zw.zac.id.region_count; i++) {
        if (H.zw.zac.id.regions[i] == code) {
            return;
        }
    }
    H.zw.zac.id.regions[H.zw.zac.id.region_count++] = code;
}

/* The door sensor's own button, while the pages' own "Remove a device" is listening for an
 * exclusion (screens.md §7) - a physical press works however the module last heard from it. */
void host_panel_zwave_press_door_remove(void)
{
    if (!H.zw.on) {
        return;
    }
    (void)fake_zac93_press_remove(&H.zw.zac, H.now, &H.zw.door);
}

/* The device joining now, told by its key: the rest of its DSK, as the PIN page shows it. */
static const fake_zdev_t *zw_labelled(const char *dsk)
{
    const fake_zdev_t *const all[] = {&H.zw.door,   &H.zw.motion, &H.zw.spare_secure,
                                      &H.zw.spare_insecure, &H.zw.keypad, &H.zw.stray,
                                      &H.zw.light,  &H.zw.relay,  &H.zw.dimmer, &H.zw.smart,
                                      &H.zw.spare_light};
    for (size_t i = 0; strlen(dsk) > 5u && i < sizeof all / sizeof all[0]; i++) {
        if (strlen(all[i]->dsk) > 5u && strcmp(all[i]->dsk + 5, dsk + 5) == 0) {
            return all[i];
        }
    }
    return &H.zw.door;
}

/* The installer reads the PIN off the label of the device that is joining and types it back. */
void host_panel_zwave_pin(void)
{
    if (!H.zw.on) {
        return;
    }
    ui_zw_progress_t g;
    zwz_panel_progress(&H.zw.panel, H.now, &g);
    if (g.op != UI_ZW_OP_ADD || g.step != UI_ZW_STEP_ASK_PIN) {
        zwn_note("Z-Wave: no PIN is being asked for right now");
        return;
    }
    if (zw_installer_asks(UI_ZW_REQ_PIN, 0u, zw_labelled(g.dsk)->pin) != UI_ZW_REQ_OK) {
        zwn_note("Z-Wave: the PIN was not taken");
    }
    zwave_world_run();
    after();
}

static size_t zwave_notif(uint8_t *out, size_t cap, uint8_t type, uint8_t event)
{
    const zw_cc_notification_report_t r = {
        .status = ZW_NOTIFICATION_STATUS_ON,
        .notification_type = type,
        .event = event,
    };
    return zw_cc_notification_report_build(out, cap, &r);
}

void host_panel_zwave_door(bool open)
{
    if (!H.zw.on) {
        return;
    }
    H.zw.door_open = open;
    uint8_t cmd[16];
    const size_t n =
        zwave_notif(cmd, sizeof cmd, ZW_NOTIFICATION_TYPE_ACCESS_CONTROL, open ? 0x16u : 0x17u);
    const zw_s2_result_t r = fake_zdev_report(&H.zw.door, H.now, ZW_SEC_AUTO, cmd, n);
    zwn_note("Z-Wave: door sensor %s%s", open ? "open" : "closed",
             r == ZW_S2_OK ? "" : " (not delivered - add it and let it finish its interview)");
}

void host_panel_zwave_motion(bool open)
{
    if (!H.zw.on) {
        return;
    }
    H.zw.motion_open = open;
    uint8_t cmd[16];
    const size_t n = zwave_notif(cmd, sizeof cmd, ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                 open ? 0x08u : ZW_NOTIFICATION_EVENT_IDLE);
    const zw_s2_result_t r = fake_zdev_report(&H.zw.motion, H.now, ZW_SEC_NONE, cmd, n);
    zwn_note("Z-Wave: motion sensor %s%s", open ? "sees movement" : "idle again",
             r == ZW_S2_OK ? "" : " (not delivered - add it first)");
}

void host_panel_zwave_tamper(bool on)
{
    if (!H.zw.on) {
        return;
    }
    uint8_t cmd[16];
    const size_t n = zwave_notif(cmd, sizeof cmd, ZW_NOTIFICATION_TYPE_HOME_SECURITY,
                                 on ? 0x03u : ZW_NOTIFICATION_EVENT_IDLE);
    const zw_s2_result_t r = fake_zdev_report(&H.zw.door, H.now, ZW_SEC_AUTO, cmd, n);
    zwn_note("Z-Wave: door sensor's cover is %s%s", on ? "removed (tamper)" : "back on",
             r == ZW_S2_OK ? "" : " (not delivered - add it first)");
}

void host_panel_zwave_battery(bool low)
{
    if (!H.zw.on) {
        return;
    }
    const zw_cc_battery_report_t br = {.level = low ? ZW_BATTERY_LOW : 90u};
    uint8_t cmd[8];
    const size_t n = zw_cc_battery_report_build(cmd, sizeof cmd, &br);
    const zw_s2_result_t r = fake_zdev_report(&H.zw.door, H.now, ZW_SEC_AUTO, cmd, n);
    zwn_note("Z-Wave: door sensor battery %s%s", low ? "low" : "healthy (90%)",
             r == ZW_S2_OK ? "" : " (not delivered - add it first)");
}

void host_panel_zwave_sleep_miss(bool on)
{
    if (!H.zw.on) {
        return;
    }
    H.zw.door.m.auto_wake = !on;
    zwn_note("Z-Wave: door sensor %s waking itself up to check in", on ? "stopped" : "resumed");
}

void host_panel_zwave_radio(bool present)
{
    if (!H.zw.on) {
        return;
    }
    H.zw.zac.fault.dead = !present;
    zwn_note("Z-Wave: the ZAC93 module is %s", present ? "plugged back in" : "pulled out");
}

/* The panel restarts: its Z-Wave starts again from what it stored (keys, gen, nodes, cfg, chk), as
 * zwave_service_start() starts it; the radio and the devices keep running. */
void host_panel_zwave_restart(void)
{
    if (!H.zw.on) {
        return;
    }
    zwave_panel_boot();
    zwn_note("Z-Wave: panel restarted (%u device(s) in the stored cache)",
             (unsigned)zw_node_count(&H.zw.panel.net.nodes));
}

uint32_t host_panel_zwave_seq(void)
{
    return H.zw.seq;
}

const char *host_panel_zwave_status(void)
{
    return H.zw.status;
}

/* ---- for zwave_smoke.c and the walk: what the pages see -------------------------------------- */

uint16_t host_panel_zwave_door_node(void)
{
    return H.zw.door_node;
}

uint16_t host_panel_zwave_motion_node(void)
{
    return H.zw.motion_node;
}

/* A device's page as the pages read it; false when it is not in the network. */
static bool zw_page_of(uint16_t node, ui_zw_device_page_t *out)
{
    const uint16_t gen = H.zw.on && node != 0u ? zwz_panel_generation(&H.zw.panel, node) : 0u;
    return gen != 0u && zwz_panel_device(&H.zw.panel, H.now, node, gen, out);
}

/* Its interview finished: its page no longer says it is being set up (or that it stopped). */
static bool zw_set_up(uint16_t node)
{
    ui_zw_device_page_t page;
    return zw_page_of(node, &page) && page.dev.state != UI_ZW_DEV_SETTING_UP &&
           page.dev.state != UI_ZW_DEV_WAKE_NEEDED && page.dev.state != UI_ZW_DEV_SETUP_STOPPED;
}

bool host_panel_zwave_door_ready(void)
{
    return zw_set_up(H.zw.door_node);
}

bool host_panel_zwave_motion_ready(void)
{
    return zw_set_up(H.zw.motion_node);
}

bool host_panel_zwave_door_unreachable(void)
{
    ui_zw_device_page_t page;
    return zw_page_of(H.zw.door_node, &page) && page.dev.unreachable;
}

bool host_panel_zwave_link_up(void)
{
    return H.zw.on && zw_host_link(&H.zw.panel.net.host) == ZW_LINK_UP;
}

bool host_panel_zwave_pin_pending(void)
{
    if (!H.zw.on) {
        return false;
    }
    ui_zw_progress_t g;
    zwz_panel_progress(&H.zw.panel, H.now, &g);
    return g.op == UI_ZW_OP_ADD && g.step == UI_ZW_STEP_ASK_PIN;
}

bool host_panel_zwave_door_known(void)
{
    return H.zw.on && H.zw.door_node != 0u &&
           zwz_panel_generation(&H.zw.panel, H.zw.door_node) != 0u;
}

/* "Bind the Z-Wave door to zone N" (the simulator's button; the pages' source picker does the
 * same): the door's contact input, through the hub's candidate list and binding call. */
bool host_panel_zwave_bind_door(uint16_t zone)
{
    if (!H.zw.on) {
        return false;
    }
    if (H.zw.door_node == 0u) {
        zwn_note("Z-Wave: add the door sensor first");
        return false;
    }
    zsrc_candidate_t c[16];
    const size_t n = zone_hub_candidates(&H.hub, ZSRC_ZWAVE, NULL, 0u, c, 16u);
    for (size_t i = 0; i < n; i++) {
        uint32_t home = 0u;
        uint16_t gen = 0u;
        uint16_t node = 0u;
        uint8_t ep = 0u;
        uint8_t slot = 0u;
        if (zsrc_zw_address_parse(&c[i].addr, &home, &gen, &node, &ep, &slot) &&
            node == H.zw.door_node && ep == 0u && slot == ZSRC_ZW_SLOT_CONTACT) {
            zone_hub_binding_t b;
            memset(&b, 0, sizeof b);
            b.in[ZSRC_ROLE_MAIN].addr = c[i].addr;
            zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map,
                                  (alarm_device_class_t)c[i].device_class);
            const zone_hub_result_t r = host_zone_bind(zone, &b);
            zwn_note("Z-Wave: door bound to zone %u (%s)", (unsigned)zone,
                     r == ZONE_HUB_OK ? c[i].name : "failed to bind");
            return r == ZONE_HUB_OK;
        }
    }
    zwn_note("Z-Wave: no door contact candidate yet (still interviewing?)");
    return false;
}

/* For proving the zone outcomes (zwave_smoke.c): the real alarm engine's zone status and the
 * source's own sensor_radio trouble, exactly what a real binding would show on the zone page. */
uint16_t host_panel_zone_flags(uint16_t zone)
{
    alarm_zone_status_t st;
    memset(&st, 0, sizeof st);
    alarm_engine_zone_status(&H.eng, zone, &st);
    return st.flags;
}

bool host_panel_sensor_radio_trouble(void)
{
    return (alarm_engine_system_troubles(&H.eng, NULL) & (1u << ALARM_TRBL_SENSOR_RADIO)) != 0u;
}

/* ---- the Z-Wave pages' backend (ui_backend_t zw_*, docs/features/36 screens.md §12) -----------
 * As zwave_service's zwave_service_ui_* calls: zwz_panel reads, and its requests with "installer
 * mode with every partition disarmed" worked out here. */

static bool all_disarmed(void);

/* ZW-005, ZW-016, ZW-018: installer mode with every partition disarmed (zwave_service.c
 * installer_and_disarmed()). */
static bool zw_allowed(void)
{
    return alarm_engine_program_active(&H.eng) && all_disarmed();
}

static bool be_zw_status(void *ctx, ui_zw_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (!H.zw.on) {
        return false; /* the preview's panel has no Z-Wave until its walk gets there */
    }
    zwz_panel_status(&H.zw.panel, H.now, out);
    if (H.zw.status_force != 0u) { /* BL-152: the Troubles walk's jammed or silent radio */
        out->jammed = H.zw.status_force == 1u;
        if (H.zw.status_force == 2u) {
            out->radio = UI_ZW_RADIO_NOT_ANSWERING;
        }
    }
    out->radio_fw_check = H.zw.radiojob.state;
    snprintf(out->radio_fw_version, sizeof out->radio_fw_version, "%s", H.zw.radiojob.update.version);
    snprintf(out->radio_fw_words, sizeof out->radio_fw_words, "%s", H.zw.radiojob.words);
    return true;
}

static size_t be_zw_devices(void *ctx, ui_zw_device_t *out, size_t max)
{
    (void)ctx;
    return H.zw.on ? zwz_panel_devices(&H.zw.panel, H.now, out, max) : 0u;
}

static bool be_zw_device(void *ctx, uint16_t node, uint16_t generation, ui_zw_device_page_t *out)
{
    (void)ctx;
    if (!H.zw.on) {
        memset(out, 0, sizeof *out);
        return false;
    }
    if (!zwz_panel_device(&H.zw.panel, H.now, node, generation, out)) {
        return false;
    }
    zwfj_page(&H.zw.fwjob, node, generation, out);
    return true;
}

/* BL-152 (ZW-019): the status the pages and the Troubles list read says the radio is jammed (1) or
 * not answering (2), or is left alone (0); the chain behind it is the host tests'. */
void host_panel_zwave_status_force(unsigned what)
{
    H.zw.status_force = (uint8_t)what;
}

void host_panel_zwave_fw_offline(bool offline)
{
    H.zw.fw_offline = offline;
}

/* A request goes to the network at once, as the zwave task takes it: then the wire runs, what
 * changed is stored and what happened logged. */
static ui_zw_req_result_t be_zw_request(void *ctx, const ui_zw_request_t *req)
{
    (void)ctx;
    if (req == NULL) {
        return UI_ZW_REQ_INVALID;
    }
    if (!H.zw.on) {
        return UI_ZW_REQ_NO_RADIO;
    }
    ui_zw_req_result_t r;
    if (req->kind == UI_ZW_REQ_FEEDBACK) {
        /* zwave_service's ZWSVC_REQ_FEEDBACK: installer and disarmed, the device the page showed,
         * its own slot or the first free one, zwf_assign's checks (stored at once here) */
        r = UI_ZW_REQ_INVALID;
        if (!zw_allowed()) {
            r = UI_ZW_REQ_NOT_ALLOWED;
        } else if (zwz_panel_generation(&H.zw.panel, req->node) == req->generation &&
                   req->partition <= ALARM_MAX_PARTITIONS) {
            const size_t slot = req->partition == 0u ? zwf_slot_of(&H.zw.feedback, req->node)
                                                     : zwf_slot_for(&H.zw.feedback, req->node);
            if (slot >= ZWF_TARGETS) {
                r = req->partition == 0u ? UI_ZW_REQ_OK : UI_ZW_REQ_BUSY;
            } else if (zwf_assign(&H.zw.feedback, slot, &H.zw.panel.net, req->node,
                                  req->partition)) {
                r = UI_ZW_REQ_OK;
                /* the simulated keypad keeps what it hears from here on (its record is short:
                 * its interview's questions filled it) */
                H.zw.keypad.got_n = 0u;
            }
        }
    } else if (req->kind == UI_ZW_REQ_CONTROL) {
        /* zwave_service's ZWSVC_REQ_CONTROL: the same checks, then zwkc_assign's */
        r = UI_ZW_REQ_INVALID;
        if (!zw_allowed()) {
            r = UI_ZW_REQ_NOT_ALLOWED;
        } else if (zwz_panel_generation(&H.zw.panel, req->node) == req->generation &&
                   req->partition <= ALARM_MAX_PARTITIONS) {
            const size_t slot = req->partition == 0u ? zwkc_slot_of(&H.zw.keyctl, req->node)
                                                     : zwkc_slot_for(&H.zw.keyctl, req->node);
            if (slot >= ZWKC_SLOTS) {
                r = req->partition == 0u ? UI_ZW_REQ_OK : UI_ZW_REQ_BUSY;
            } else if (zwkc_assign(&H.zw.keyctl, slot, &H.zw.panel.net, req->node,
                                   req->partition, req->panic_mask)) {
                r = UI_ZW_REQ_OK;
            }
        }
    } else if ((req->kind == UI_ZW_REQ_FW_CHECK || req->kind == UI_ZW_REQ_FW_INSTALL) &&
               req->node == 0u) {
        /* the radio itself (network.md §14), as zwave_service does it */
        r = UI_ZW_REQ_INVALID;
        if (!zw_allowed()) {
            r = UI_ZW_REQ_NOT_ALLOWED;
        } else if (req->kind == UI_ZW_REQ_FW_CHECK) {
            char body[ZWFJ_BODY_MAX];
            if (zwfj_check_radio(&H.zw.radiojob, zw_host_facts(&H.zw.panel.net.host),
                                 zw_region_word(), body, sizeof body) > 0u) {
                H.zw.radio_due = H.now + 1000u;
                r = UI_ZW_REQ_OK;
            }
        } else if (zwfj_download_begin(&H.zw.radiojob, 0u, 0u) != NULL) {
            H.zw.fw_user = req->user;
            H.zw.radio_due = H.now + 1000u;
            r = UI_ZW_REQ_OK;
        }
    } else if (req->kind == UI_ZW_REQ_FW_CHECK || req->kind == UI_ZW_REQ_FW_INSTALL) {
        /* zwave_service's: installer and disarmed, the device the page showed; the service a
         * second away (zwave_fw_run) */
        const zw_node_record_t *rec = zw_node_find_const(&H.zw.panel.net.nodes, req->node);
        r = UI_ZW_REQ_INVALID;
        if (!zw_allowed()) {
            r = UI_ZW_REQ_NOT_ALLOWED;
        } else if (rec == NULL || rec->generation != req->generation ||
                   !zwz_panel_fw_updatable(&H.zw.panel, req->node, req->generation)) {
            r = UI_ZW_REQ_INVALID;
        } else if (req->kind == UI_ZW_REQ_FW_CHECK) {
            char body[ZWFJ_BODY_MAX];
            if (zwfj_check_begin(&H.zw.fwjob, rec, zw_region_word(), body, sizeof body) > 0u) {
                H.zw.fw_due = H.now + 1000u;
                r = UI_ZW_REQ_OK;
            }
        } else if (zwfj_download_begin(&H.zw.fwjob, req->node, req->generation) != NULL) {
            H.zw.fw_user = req->user;
            H.zw.fw_due = H.now + 1000u;
            r = UI_ZW_REQ_OK;
        }
    } else if (req->kind == UI_ZW_REQ_STOP && zwfj_cancel(&H.zw.fwjob)) {
        r = UI_ZW_REQ_OK; /* its download stopped: nothing sent */
    } else {
        r = zwz_panel_request(&H.zw.panel, H.now, req, zw_allowed());
    }
    zwave_world_run();
    after();
    return r;
}

/* SmartStart's list: as zwave_service does it - changed at once (data only), installer mode for
 * a change; the world runs after, so the radio listens (or stops) and the list is stored. */
static size_t be_zw_prov(void *ctx, ui_zw_prov_t *out, size_t max)
{
    (void)ctx;
    return H.zw.on ? zwz_panel_prov(&H.zw.panel, out, max) : 0u;
}

static ui_zw_prov_result_t be_zw_prov_register(void *ctx, const char *code, const char *name)
{
    (void)ctx;
    if (!H.zw.on) {
        return UI_ZW_PROV_NOT_FOUND;
    }
    const ui_zw_prov_result_t r = zwz_panel_prov_register(
        &H.zw.panel, H.now, code, name, alarm_engine_program_active(&H.eng));
    zwave_world_run();
    after();
    return r;
}

static ui_zw_prov_result_t be_zw_prov_change(void *ctx, const uint8_t dsk[16], uint8_t op,
                                             uint8_t arg)
{
    (void)ctx;
    if (!H.zw.on) {
        return UI_ZW_PROV_NOT_FOUND;
    }
    const ui_zw_prov_result_t r = zwz_panel_prov_change(
        &H.zw.panel, H.now, dsk, op, arg, alarm_engine_program_active(&H.eng));
    zwave_world_run();
    after();
    return r;
}

static void be_zw_progress(void *ctx, ui_zw_progress_t *out)
{
    (void)ctx;
    if (!H.zw.on) {
        memset(out, 0, sizeof *out);
        return;
    }
    zwz_panel_progress(&H.zw.panel, H.now, out);
}
#endif /* ALARM_SIM_ZWAVE */

static bool all_disarmed(void);

/* The Zigbee demo backend's check (host_zigbee_demo.h): the same rule as zw_allowed(). */
static bool zb_demo_allowed(void)
{
    return alarm_engine_program_active(&H.eng) && all_disarmed();
}

/* The panel link's demo backend (host_link_demo.h): the pretend panel's clock. */
static int64_t link_demo_epoch(void)
{
    return H.epoch_at_start == 0 ? 0 : H.epoch_at_start + (int64_t)H.now;
}

static void storage_reset(void);
static void ha_link_up(bool up);
static bool host_panel_ha_paired_and_on(void);
static void zb_demo_hooks(void);

static void prefs_remote_reset(void);

void host_panel_init(int64_t epoch_ms_at_start)
{
    memset(&H, 0, sizeof(H));
    prefs_remote_reset(); /* no change from elsewhere waiting */
    storage_reset();
    devices_reset(epoch_ms_at_start);
    H.epoch_at_start = epoch_ms_at_start;
    vx_init();
    host_zigbee_demo_init(zb_demo_allowed); /* Installer > Zigbee (BL-144), interim */
    host_link_demo_init(zb_demo_allowed, link_demo_epoch); /* the panel link (BL-048), off */
    zb_demo_hooks();
    H.now = 5000;
    H.config_seq = 1;
    H.net = UI_NET_CONNECTED;
    H.rssi = -58;
    snprintf(H.ssid, sizeof(H.ssid), "Harbor House");
    snprintf(H.tz, sizeof(H.tz), "EST5EDT,M3.2.0,M11.1.0");
    snprintf(H.ntp, sizeof(H.ntp), "pool.ntp.org");
    H.mqtt.tls = UI_MQTT_TLS_PUBLIC;
    H.mqtt.discovery = true;
    H.mqtt.commands = true;
    snprintf(H.mqtt.prefix, sizeof(H.mqtt.prefix), "alarmpanel");
    snprintf(H.mqtt.panel_id, sizeof(H.mqtt.panel_id), "a1b2c3");

    memset(H.log_mem, 0xFF, sizeof(H.log_mem));
    const elog_flash_t flash = {NULL, LOG_SIZE, LOG_SECTOR, flash_read, flash_write, flash_erase};
    elog_open(&H.log, &flash, 1);

    static alarm_config_t cfg;
    alarm_config_defaults(&cfg);
    for (unsigned i = 0; i < ALARM_CODE_SALT_LEN; i++) {
        cfg.code_salt[i] = (uint8_t)(0x5Au ^ i);
    }
    alarm_config_set_default_users(&cfg, preview_hash, NULL);
    snprintf(cfg.partitions[0].name, ALARM_NAME_LEN, "Home");
    cfg.opt.power_up_settle_s = 0;
    demo_zone(&cfg, 1, ALARM_ZT_ENTRY_EXIT_1, ALARM_DEVCLASS_DOOR, "Front Door");
    demo_zone(&cfg, 2, ALARM_ZT_ENTRY_EXIT_2, ALARM_DEVCLASS_GARAGE_DOOR, "Garage Door");
    demo_zone(&cfg, 3, ALARM_ZT_PERIMETER, ALARM_DEVCLASS_WINDOW, "Living Room Window");
    demo_zone(&cfg, 4, ALARM_ZT_PERIMETER, ALARM_DEVCLASS_DOOR, "Back Door");
    demo_zone(&cfg, 5, ALARM_ZT_INTERIOR_FOLLOWER, ALARM_DEVCLASS_MOTION, "Hallway Motion");
    demo_zone(&cfg, 6, ALARM_ZT_FIRE, ALARM_DEVCLASS_SMOKE, "Kitchen Smoke");
    demo_zone(&cfg, 7, ALARM_ZT_CO, ALARM_DEVCLASS_CO, "Hallway CO");
    demo_zone(&cfg, 8, ALARM_ZT_24H_AUX, ALARM_DEVCLASS_WATER, "Basement Water");
#if ALARM_SIM_ZWAVE
    /* zones_start()'s own loop binds every configured zone to the simulator source first; "Bind
     * Z-Wave door to zone 9" (sim_main.c) rebinds this one once the door sensor has joined. The
     * preview's world starts later, without it (host_panel_zwave_later). */
    if (!s_zw_later) {
        demo_zone(&cfg, 9, ALARM_ZT_PERIMETER, ALARM_DEVCLASS_DOOR, "Z-Wave Door");
    }
#endif

    alarm_ports_t ports;
    memset(&ports, 0, sizeof(ports));
    ports.on_event = on_event;
    ports.on_report = on_report;
    ports.on_sound = on_sound;
    ports.on_status = on_status;
    ports.on_output = on_output;
    ports.on_config_changed = on_config_changed;
    ports.hash_code = preview_hash;
    ports.get_epoch_ms = epoch_ms;
    if (alarm_engine_init(&H.eng, &cfg, &ports, NULL, H.now) != ALARM_OK) {
        fprintf(stderr, "engine init failed\n");
    }
    zones_start(&cfg);
    if (host_panel_ha_paired_and_on()) {
        ha_link_up(true); /* the paired Home Assistant reconnects after a restart */
    }
#if ALARM_SIM_ZWAVE
    if (!s_zw_later) {
        zwave_world_init();
    }
#endif
    automations_reset();
    H.dirty = true;
    after();
}

/* The preview's fake LAN: the router is 192.168.1.1 and the panel's lease is 192.168.1.42.
 * A fixed address pointing somewhere else fails at the router, so the walk-through can show
 * both a run that passes and one that doesn't. */
#define FAKE_ROUTER "192.168.1.1"
#define FAKE_LEASE  "192.168.1.42"
#define FAKE_MASK   "255.255.255.0"

static bool faulty(host_net_fault_t f)
{
    return H.faults[f];
}

/* The address the panel has: its fixed one, or the router's lease - and none at all when the
 * router is refusing to hand one out. */
static const char *live_ip(void)
{
    if (H.net != UI_NET_CONNECTED) {
        return "";
    }
    if (H.ip.use_static && H.ip.ip[0] != '\0') {
        return H.ip.ip;
    }
    return faulty(HOST_NET_NO_LEASE) ? "" : FAKE_LEASE;
}

static const char *live_gateway(void)
{
    if (live_ip()[0] == '\0') {
        return "";
    }
    return H.ip.use_static && H.ip.gateway[0] != '\0' ? H.ip.gateway : FAKE_ROUTER;
}

/* The router answers only if it is really the house router and it isn't playing dead. */
static bool router_reachable(void)
{
    return strcmp(live_gateway(), FAKE_ROUTER) == 0 && !faulty(HOST_NET_ROUTER_DOWN);
}

void host_panel_net_fault(host_net_fault_t fault, bool on)
{
    if (fault < HOST_NET_FAULT_COUNT) {
        H.faults[fault] = on;
    }
}

bool host_panel_net_faulty(host_net_fault_t fault)
{
    return fault < HOST_NET_FAULT_COUNT && H.faults[fault];
}

void host_panel_net_clear_faults(void)
{
    memset(H.faults, 0, sizeof(H.faults));
}

/* As net_check.c keeps a result: the words (ui_check_words.h), their values, the English. */
static void check_result(size_t step, uint8_t state, uint16_t ms, const char *words,
                         const char *v0, const char *v1)
{
    ui_check_result_t *r = &H.check.steps[step];
    r->state = state;
    r->ms = ms;
    r->words = words;
    i18n_utf8_copy(r->value[0], sizeof(r->value[0]), v0); /* whole characters */
    i18n_utf8_copy(r->value[1], sizeof(r->value[1]), v1);
}

/* One step of the simulated check finishes every 600 ms. */
static void netcheck_step(void)
{
    if (!H.check.running || H.now < H.check_at) {
        return;
    }
    char where[64];
    const bool router_ok = router_reachable();
    const bool dns_ok = router_ok && !faulty(HOST_NET_DNS_DOWN);
    const size_t step = H.check_step;
    switch (step) {
    case UI_CHECK_WIFI:
        if (H.net == UI_NET_CONNECTED) {
            check_result(step, UI_CHECK_PASS, 120, UI_CHECK_W_JOINED, H.ssid, NULL);
        } else if (H.ssid[0] == '\0') {
            check_result(step, UI_CHECK_FAIL, 0, UI_CHECK_W_NO_NETWORK, NULL, NULL);
        } else {
            check_result(step, UI_CHECK_FAIL, 0, UI_CHECK_W_NO_ANSWER, H.ssid, NULL);
        }
        break;
    case UI_CHECK_ADDRESS:
        if (live_ip()[0] == '\0') {
            check_result(step, UI_CHECK_FAIL, 0,
                         H.ip.use_static ? UI_CHECK_W_FIXED_NOT_TAKEN : UI_CHECK_W_NO_LEASE, NULL,
                         NULL);
            break;
        }
        check_result(step, UI_CHECK_PASS, 0,
                     H.ip.use_static ? UI_CHECK_W_ADDRESS_FIXED : UI_CHECK_W_ADDRESS_LEASED,
                     live_ip(), H.ip.use_static && H.ip.mask[0] ? H.ip.mask : FAKE_MASK);
        break;
    case UI_CHECK_ROUTER:
        check_result(step, router_ok ? UI_CHECK_PASS : UI_CHECK_FAIL, router_ok ? 3u : 0u,
                     router_ok ? UI_CHECK_W_ANSWERED : UI_CHECK_W_WRONG_ROUTER, live_gateway(),
                     NULL);
        break;
    case UI_CHECK_DNS:
        if (dns_ok) {
            check_result(step, UI_CHECK_PASS, 28, UI_CHECK_W_RESOLVED, H.ntp, "162.159.200.1");
        } else {
            check_result(step, UI_CHECK_FAIL, 0, UI_CHECK_W_DNS_SILENT,
                         H.ip.dns1[0] ? H.ip.dns1 : FAKE_ROUTER, H.ntp);
        }
        break;
    case UI_CHECK_INTERNET: {
        /* Reached by address, so a broken name lookup doesn't hide a working internet. */
        const bool ok = router_ok && !faulty(HOST_NET_INTERNET_DOWN);
        check_result(step, ok ? UI_CHECK_PASS : UI_CHECK_FAIL, ok ? 41u : 0u,
                     ok ? UI_CHECK_W_PORT_ANSWERED : UI_CHECK_W_NOTHING_OUT, "1.1.1.1",
                     ok ? "443" : "9.9.9.9");
        break;
    }
    case UI_CHECK_TIME: {
        const bool ok = dns_ok && !faulty(HOST_NET_TIME_DOWN);
        check_result(step, ok ? UI_CHECK_PASS : UI_CHECK_FAIL, ok ? 34u : 0u,
                     ok ? UI_CHECK_W_CLOCK_RIGHT : UI_CHECK_W_NTP_SILENT, H.ntp, NULL);
        break;
    }
    default: {
        const unsigned port = H.mqtt.port != 0u ? H.mqtt.port : 8883u;
        const bool ok = router_ok && !faulty(HOST_NET_SERVICE_DOWN);
        snprintf(where, sizeof(where), "%.48s:%u", H.mqtt.host, port);
        if (!H.mqtt.enabled || H.mqtt.host[0] == '\0') {
            check_result(step, UI_CHECK_SKIPPED, 0, UI_CHECK_W_NO_SERVICE, NULL, NULL);
        } else if (ok) {
            check_result(step, UI_CHECK_PASS, 12, UI_CHECK_W_ANSWERED, where, NULL);
        } else {
            check_result(step, UI_CHECK_FAIL, 0, UI_CHECK_W_NOTHING_AT, where, NULL);
        }
        break;
    }
    }
    /* Without a link or an address the rest was never reached; a broken step further along
     * still lets the ones after it run, so one run tells you everything. */
    if (H.check.steps[step].state == UI_CHECK_FAIL && step <= UI_CHECK_ADDRESS) {
        for (size_t i = step + 1u; i < UI_CHECK_COUNT; i++) {
            check_result(i, UI_CHECK_SKIPPED, 0,
                         step == UI_CHECK_WIFI ? UI_CHECK_W_OFF_NETWORK : UI_CHECK_W_NO_ADDRESS,
                         NULL, NULL);
        }
        H.check_step = UI_CHECK_COUNT;
    } else {
        H.check_step++;
    }
    if (H.check_step >= UI_CHECK_COUNT) {
        H.check.running = false;
        H.check.done = true;
        return;
    }
    H.check.steps[H.check_step].state = UI_CHECK_RUNNING;
    H.check_at = H.now + 600u;
}

static void wifi_step(void)
{
    if (H.connect_at != 0u && H.now >= H.connect_at) {
        H.connect_at = 0;
        if (H.connect_ok) {
            H.net = UI_NET_CONNECTED;
            H.rssi = -55;
            H.fail = UI_WIFI_FAIL_NONE;
        } else {
            H.net = UI_NET_DISCONNECTED;
            H.fail = UI_WIFI_FAIL_PASSWORD;
        }
    }
    /* The access point going away, and coming back when the fault is switched off. */
    if (H.ssid[0] != '\0' && H.connect_at == 0u) {
        if (faulty(HOST_NET_WIFI_DOWN) && H.net == UI_NET_CONNECTED) {
            H.net = UI_NET_DISCONNECTED;
            H.fail = UI_WIFI_FAIL_NOT_FOUND;
            H.rssi = 0;
        } else if (!faulty(HOST_NET_WIFI_DOWN) && H.net == UI_NET_DISCONNECTED &&
                   H.fail == UI_WIFI_FAIL_NOT_FOUND) {
            H.net = UI_NET_CONNECTED;
            H.fail = UI_WIFI_FAIL_NONE;
            H.rssi = -55;
        }
    }
}

/* The simulated broker: a name ending in ".invalid" doesn't answer, the password "wrong" is
 * refused, an own certificate authority must be loaded first; anything else connects. */
static void mqtt_step(void)
{
    if (!H.mqtt.enabled) {
        H.mqtt_state = UI_MQTT_OFF;
        return;
    }
    if (H.net != UI_NET_CONNECTED) {
        H.mqtt_state = UI_MQTT_NO_NETWORK;
        H.mqtt_at = H.now + 1500u;
        return;
    }
    if (H.mqtt_state == UI_MQTT_OFF || H.mqtt_state == UI_MQTT_NO_NETWORK) {
        H.mqtt_state = UI_MQTT_CONNECTING;
        H.mqtt_at = H.now + 1500u;
    }
    if (H.mqtt_state == UI_MQTT_CONNECTING && H.now >= H.mqtt_at) {
        const size_t n = strlen(H.mqtt.host);
        H.mqtt_fail = UI_MQTT_FAIL_NONE;
        if (n > 8u && strcmp(H.mqtt.host + n - 8u, ".invalid") == 0) {
            H.mqtt_fail = UI_MQTT_FAIL_UNREACHABLE;
        } else if (strcmp(H.mqtt_password, "wrong") == 0) {
            H.mqtt_fail = UI_MQTT_FAIL_AUTH;
        } else if (H.mqtt.tls == UI_MQTT_TLS_OWN_CA) {
            H.mqtt_fail = UI_MQTT_FAIL_TLS;
        }
        H.mqtt_state = H.mqtt_fail == UI_MQTT_FAIL_NONE ? UI_MQTT_CONNECTED : UI_MQTT_FAILED;
    }
}

void host_panel_advance(alarm_ms_t ms)
{
    host_zigbee_demo_advance((uint32_t)ms);
    host_link_demo_advance((uint32_t)ms);
    const alarm_ms_t end = H.now + ms;
    while (H.now < end) { /* the hub's deadlines (wired polling, grace times) in order */
        uint64_t next = zone_hub_next_deadline(&H.hub, H.now);
        if (s_zb_real) { /* ... and the real Zigbee's: the air, setups, the join window */
            const uint64_t z = host_zigbee_real_next_due();
            next = z < next ? z : next;
        }
        const uint64_t a = auto_next_deadline(); /* ... and the automations' (docs/features/41) */
        next = a < next ? a : next;
        const uint64_t vd = vx_next_deadline(); /* ... and the voice's: a tone's end, an utterance's */
        next = vd < next ? vd : next;
        H.now = next < end && next > H.now ? next : end;
        zone_hub_tick(&H.hub, H.now);
        if (s_zb_real) {
            host_zigbee_real_run(H.now);
        }
        if (H.zb_scan_pending) {
            H.zb_scan_pending = false;
            zsrc_zb_energy(&H.zb, H.zb_noise ? -20 : -85, H.now);
        }
        alarm_engine_tick(&H.eng, H.now);
        vx_power_poll();
        vx_commit(); /* this tick's events with its snapshot, whether or not automations run */
        vx_pump();
        if (host_panel_automations_active() && (a <= H.now || H.dirty)) {
            after(); /* the runner at its deadline, or with what the engine just did */
        }
    }
#if ALARM_SIM_ZWAVE
    if (H.zw.on) {
        zwave_world_run(); /* the ZAC93, its devices and zwz_panel up to H.now */
    }
#endif
    wifi_step();
    mqtt_step();
    netcheck_step();
    after();
}

void host_panel_zone(uint16_t zone, alarm_zone_event_t ev)
{
    zone_hub_binding_t b;
    if (zone_hub_get(&H.hub, zone, &b) && b.in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_SIM) {
        zsrc_sim_event(&H.sim, (uint16_t)b.in[ZSRC_ROLE_MAIN].addr.id, ev, H.now);
    } else {
        alarm_engine_zone_event(&H.eng, zone, ev, H.now);
    }
    after();
}

void host_panel_wire(uint8_t gpio, bool open)
{
    if (gpio < sizeof(H.pins)) {
        H.pins[gpio] = open ? 1u : 0u;
    }
}

alarm_result_t host_panel_remote_command(const alarm_cmd_t *cmd)
{
    static const alarm_source_t remote = {.kind = ALARM_SRC_REMOTE, .id = 1};
    const alarm_result_t r = alarm_engine_command(&H.eng, cmd, &remote, H.now, NULL);
    after();
    return r;
}

alarm_result_t host_panel_manager_command(const alarm_cmd_t *cmd)
{
    static const alarm_source_t manager = {.kind = ALARM_SRC_REMOTE,
                                           .id = ALARM_SRC_ID_PANEL_MANAGER};
    const alarm_result_t r = alarm_engine_command(&H.eng, cmd, &manager, H.now, NULL);
    after();
    return r;
}

alarm_result_t host_panel_keypad2_program(bool on)
{
    static const alarm_source_t keypad2 = {.kind = ALARM_SRC_KEYPAD, .id = 2};
    alarm_result_t r = ALARM_OK;
    if (on) { /* the installer, whatever their code is by now */
        r = alarm_engine_program_enter_as(&H.eng, &keypad2, ALARM_USER_INSTALLER, H.now);
    } else {
        alarm_engine_program_exit(&H.eng, &keypad2, H.now);
    }
    after();
    return r;
}

void host_panel_trouble(alarm_sys_trouble_t trouble, bool active)
{
    alarm_engine_system_trouble(&H.eng, trouble, active, H.now);
    after();
}

void host_panel_set_net(ui_net_state_t state, int rssi)
{
    H.net = state;
    H.rssi = rssi;
}

void host_panel_forget_wifi(void)
{
    H.net = UI_NET_UNCONFIGURED;
    H.ssid[0] = '\0';
    H.rssi = 0;
}

/* ---- backend -------------------------------------------------------------------------- */

static uint32_t be_seq(void *ctx)
{
    (void)ctx;
    return H.seq;
}

static void be_snapshot(void *ctx, alarm_snapshot_t *out)
{
    (void)ctx;
    *out = H.snap;
}

static bool be_config(void *ctx, alarm_config_t *out)
{
    (void)ctx;
    *out = *alarm_engine_config(&H.eng);
    return true;
}

static size_t be_events(void *ctx, alarm_event_t *out, size_t max)
{
    (void)ctx;
    const size_t n = max < H.count ? max : H.count;
    for (size_t i = 0; i < n; i++) {
        out[i] = H.ring[(H.head + RING - 1u - i) % RING];
    }
    return n;
}

static alarm_ms_t be_now(void *ctx)
{
    (void)ctx;
    return H.now;
}

static alarm_result_t be_command(void *ctx, const alarm_cmd_t *cmd, alarm_cmd_result_t *res)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_command(&H.eng, cmd, &SRC, H.now, res);
    after();
    return r;
}

static alarm_result_t be_change_code(void *ctx, const char *current, const char *new_code,
                                     uint8_t *user_out)
{
    (void)ctx;
    uint8_t user = 0;
    alarm_result_t r = alarm_engine_authenticate(&H.eng, &SRC, current, 0, H.now, &user);
    if (r == ALARM_OK) {
        const alarm_user_t *u = &alarm_engine_config(&H.eng)->users[user - 1u];
        r = alarm_engine_user_set(&H.eng, &SRC, current, user, new_code, u->authority,
                                  u->partitions, NULL, H.now);
    }
    if (user_out != NULL) {
        *user_out = user;
    }
    after();
    return r;
}

static ui_net_state_t be_net(void *ctx, int *rssi)
{
    (void)ctx;
    *rssi = H.rssi;
    return H.net;
}

static alarm_result_t be_authenticate(void *ctx, const char *code, uint8_t *user_out)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_authenticate(&H.eng, &SRC, code, 0, H.now, user_out);
    after();
    return r;
}

static alarm_result_t be_program_enter(void *ctx, const char *code, uint8_t *user_out)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_program_enter(&H.eng, &SRC, code, H.now, user_out);
    after();
    return r;
}

static void be_program_exit(void *ctx)
{
    (void)ctx;
    alarm_engine_program_exit(&H.eng, &SRC, H.now);
    after();
}

static void be_program_keepalive(void *ctx)
{
    (void)ctx;
    alarm_engine_program_keepalive(&H.eng, H.now);
}

static alarm_result_t be_program_set_code(void *ctx, uint8_t user, const char *code)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_program_set_code(&H.eng, &SRC, user, code, H.now);
    after();
    return r;
}

static alarm_result_t be_set_config(void *ctx, const alarm_config_t *cfg)
{
    (void)ctx;
    uint8_t reset[ALARM_ZONE_BITMAP_BYTES] = {0};
    const alarm_config_t *old = alarm_engine_config(&H.eng);
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (alarm_zone_config_resets_state(&old->zones[z - 1u], &cfg->zones[z - 1u])) {
            reset[(z - 1u) / 8u] |= (uint8_t)(1u << ((z - 1u) % 8u));
        }
    }
    const alarm_result_t r = alarm_engine_set_config(&H.eng, cfg, H.now);
    if (r == ALARM_OK) {
        zone_hub_resync(&H.hub, reset, H.now); /* as zone_service does on the panel */
        H.config_seq++;
        H.dirty = true;
    }
    after();
    return r;
}

static alarm_result_t be_user_set(void *ctx, const char *actor, uint8_t user, const char *code,
                                  alarm_authority_t authority, uint8_t partitions, const char *name)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_user_set(&H.eng, &SRC, actor, user, code, authority,
                                                   partitions, name, H.now);
    after();
    return r;
}

static alarm_result_t be_user_update(void *ctx, const char *actor, uint8_t user,
                                     alarm_authority_t authority, uint8_t partitions,
                                     const char *name)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_user_update(&H.eng, &SRC, actor, user, authority,
                                                      partitions, name, H.now);
    after();
    return r;
}

static alarm_result_t be_user_delete(void *ctx, const char *actor, uint8_t user)
{
    (void)ctx;
    const alarm_result_t r = alarm_engine_user_delete(&H.eng, &SRC, actor, user, H.now);
    after();
    return r;
}

static size_t be_log_read(void *ctx, uint32_t before, elog_filter_fn filter, void *filter_ctx,
                          elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before)
{
    (void)ctx;
    return elog_read(&H.log, before, filter, filter_ctx, out, max, scan, next_before);
}

static uint32_t be_boot(void *ctx)
{
    (void)ctx;
    return H.log.boot;
}

static void be_wifi_info(void *ctx, ui_wifi_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->state = H.net;
    snprintf(out->ssid, sizeof(out->ssid), "%s", H.ssid);
    if (H.net == UI_NET_CONNECTED) {
        snprintf(out->ip, sizeof(out->ip), "%s", live_ip());
        snprintf(out->mask, sizeof(out->mask), "%s",
                 H.ip.use_static && H.ip.mask[0] ? H.ip.mask : FAKE_MASK);
        snprintf(out->gateway, sizeof(out->gateway), "%s", live_gateway());
        snprintf(out->dns1, sizeof(out->dns1), "%s",
                 H.ip.dns1[0] != '\0' ? H.ip.dns1 : FAKE_ROUTER);
        snprintf(out->dns2, sizeof(out->dns2), "%s", H.ip.dns2);
        out->static_ip = H.ip.use_static;
        out->rssi = (int8_t)H.rssi;
    }
    snprintf(out->mac, sizeof(out->mac), "58:CF:79:1A:2B:3C");
    out->fail = H.fail;
    out->time_synced = H.net == UI_NET_CONNECTED;
    snprintf(out->radio_fw, sizeof(out->radio_fw), "ESP32-C6 2.12.3");
}

static void be_ip_get(void *ctx, ui_ip_config_t *out)
{
    (void)ctx;
    *out = H.ip;
}

static bool be_ip_set(void *ctx, const ui_ip_config_t *in, char *why, size_t why_len)
{
    (void)ctx;
    if (in->use_static && (in->ip[0] == '\0' || in->mask[0] == '\0' || in->gateway[0] == '\0')) {
        snprintf(why, why_len, "Those addresses don't go together");
        return false;
    }
    H.ip = *in;
    return true;
}

static bool be_net_check_start(void *ctx)
{
    (void)ctx;
    if (H.check.running) {
        return false;
    }
    memset(&H.check, 0, sizeof(H.check));
    H.check.running = true;
    H.check_step = 0;
    H.check.steps[0].state = UI_CHECK_RUNNING;
    H.check_at = H.now + 600u;
    return true;
}

static void be_net_check_status(void *ctx, ui_netcheck_t *out)
{
    (void)ctx;
    *out = H.check;
}

static bool be_wifi_scan(void *ctx)
{
    (void)ctx;
    H.scan_done_at = H.now + 1500u;
    return true;
}

static size_t be_wifi_results(void *ctx, ui_wifi_ap_t *out, size_t max, bool *done)
{
    (void)ctx;
    *done = H.now >= H.scan_done_at;
    if (!*done) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < sizeof(APS) / sizeof(APS[0]) && n < max; i++, n++) {
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", APS[i].ssid);
        out[n].rssi = APS[i].rssi;
        out[n].secure = APS[i].secure;
    }
    return n;
}

static void be_wifi_connect(void *ctx, const char *ssid, const char *password)
{
    (void)ctx;
    if (ssid[0] == '\0') {
        host_panel_forget_wifi();
        return;
    }
    snprintf(H.ssid, sizeof(H.ssid), "%s", ssid);
    H.net = UI_NET_CONNECTING;
    H.fail = UI_WIFI_FAIL_NONE;
    H.connect_at = H.now + 2500u;
    H.connect_ok = strcmp(password, "wrong") != 0;
}

static void be_get_tz(void *ctx, char *out, size_t len)
{
    (void)ctx;
    snprintf(out, len, "%s", H.tz);
}

static void be_set_tz(void *ctx, const char *tz)
{
    (void)ctx;
    snprintf(H.tz, sizeof(H.tz), "%s", tz); /* not applied: screenshots stay in UTC */
}

static void be_get_ntp(void *ctx, char *out, size_t len)
{
    (void)ctx;
    snprintf(out, len, "%s", H.ntp);
}

static void be_set_ntp(void *ctx, const char *server)
{
    (void)ctx;
    snprintf(H.ntp, sizeof(H.ntp), "%s", server[0] ? server : "pool.ntp.org");
}

/* ---- the clock module (docs/features/35) ------------------------------------------------------- */

void host_panel_clock_module(ui_clock_module_t state, bool lost_at_start)
{
    H.clock_module = state;
    H.clock_lost_at_start = lost_at_start;
    H.clock_by_hand = false; /* a new module state: nothing has set the time by hand yet */
    H.dirty = true;
}

static void be_clock_info(void *ctx, ui_clock_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->module = (uint8_t)H.clock_module;
    out->lost_at_start = H.clock_lost_at_start;
    out->temp_known = H.clock_module == UI_CLOCK_MODULE_OK;
    out->temp_c4 = 97; /* 24.25 C */
    if (H.clock_by_hand) {
        out->source = UI_CLOCK_BY_HAND;
    } else if (H.net == UI_NET_CONNECTED) {
        out->source = UI_CLOCK_FROM_INTERNET;
    } else if (H.clock_module == UI_CLOCK_MODULE_OK && H.epoch_at_start != 0) {
        out->source = UI_CLOCK_FROM_MODULE;
    }
}

static bool be_clock_set(void *ctx, int64_t epoch_ms, uint8_t user, char *why, size_t why_len)
{
    (void)ctx;
    (void)user;
    ui_clock_info_t c;
    be_clock_info(NULL, &c);
    if (!ui_clock_settable(&c)) {
        snprintf(why, why_len, "This panel can't keep a time set by hand");
        return false;
    }
    if (epoch_ms < 1735689600000LL || epoch_ms > 4102444799000LL) {
        snprintf(why, why_len, "Pick a date from 2025 to 2099");
        return false;
    }
    H.epoch_at_start = epoch_ms - (int64_t)H.now;
    H.clock_by_hand = true;
    H.clock_module = UI_CLOCK_MODULE_OK; /* written: it keeps the time now */
    H.dirty = true;
    return true;
}

/* What the panel would store (panel_prefs "ui"): kept, so the walk sees a choice saved. It
 * outlives host_panel_init(), as the panel's store outlives the engine restarting. */
static ui_options_t s_saved_options;
static bool s_options_saved;
static unsigned s_options_saves;
static void VP_set_options(const ui_options_t *opt); /* the voice's, below */

static void be_save_options(void *ctx, const ui_options_t *opt)
{
    (void)ctx;
    s_saved_options = *opt;
    s_options_saved = true;
    s_options_saves++;
    VP_set_options(opt);
}

bool host_panel_saved_options(ui_options_t *out)
{
    if (s_options_saved) {
        *out = s_saved_options;
    }
    return s_options_saved;
}

unsigned host_panel_options_saves(void)
{
    return s_options_saves;
}

/* A change of the settings from elsewhere (docs/features/07 UI-046): the panel's mailbox
 * (platform_esp's panel_prefs_remote_*) on one thread and the simulated clock - the same
 * bookkeeping (ui_prefs.h's box), the same conversion to the screen's patch (ui_port_esp's
 * prefs_remote_take), the same BUSY for a patch not taken within 3 s. Emptied by
 * host_panel_init(). */
static struct {
    ui_prefs_box_t box;
    proto_prefs_values_t patch;
    host_panel_prefs_done_fn done;
    void *ctx;
} s_prefs_remote;

uint32_t host_panel_prefs_submit(const proto_prefs_values_t *patch, host_panel_prefs_done_fn done,
                                 void *ctx)
{
    if (patch == NULL || done == NULL) {
        return 0u;
    }
    const uint32_t ticket = ui_prefs_box_submit(&s_prefs_remote.box, (uint32_t)H.now);
    if (ticket != 0u) {
        s_prefs_remote.patch = *patch;
        s_prefs_remote.done = done;
        s_prefs_remote.ctx = ctx;
    }
    return ticket;
}

bool host_panel_prefs_cancel(uint32_t ticket)
{
    return ui_prefs_box_cancel(&s_prefs_remote.box, ticket);
}

static bool be_prefs_remote_take(void *ctx, ui_prefs_patch_t *out, uint32_t *ticket)
{
    (void)ctx;
    uint32_t t = 0u;
    const ui_prefs_take_t r = ui_prefs_box_take(&s_prefs_remote.box, (uint32_t)H.now, &t);
    if (r == UI_PREFS_TAKE_EXPIRED) {
        uint8_t why[PROTO_PREFS_F_LAST];
        memset(why, 0, sizeof(why));
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            if ((s_prefs_remote.patch.have & PROTO_PREFS_BIT(id)) != 0u) {
                why[id - 1u] = (uint8_t)PROTO_PREFS_WHY_BUSY;
            }
        }
        s_prefs_remote.done(s_prefs_remote.ctx, t, why);
        return false;
    }
    if (r != UI_PREFS_TAKE_OK) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        uint32_t x = 0u;
        if (proto_prefs_get(&s_prefs_remote.patch, id, &x)) {
            (void)ui_prefs_put(out, id, x);
        }
    }
    *ticket = t;
    return true;
}

static void be_prefs_remote_done(void *ctx, uint32_t ticket, const uint8_t why[UI_PREFS_F_LAST])
{
    (void)ctx;
    if (ui_prefs_box_done(&s_prefs_remote.box, ticket)) {
        s_prefs_remote.done(s_prefs_remote.ctx, ticket, why);
    }
}

static void prefs_remote_reset(void)
{
    const uint32_t last = s_prefs_remote.box.last; /* tickets go on counting */
    memset(&s_prefs_remote, 0, sizeof(s_prefs_remote));
    s_prefs_remote.box.last = last;
}

/* The backlight the UI asked for (UI-009, UI-043): the panel's PWM here is just a number, which
 * the simulator shows by dimming its picture of the screen and the preview checks. It outlives
 * host_panel_init(), as a real backlight outlives the alarm engine restarting. */
static int s_backlight = -1;

static void be_backlight(void *ctx, uint8_t percent)
{
    (void)ctx;
    s_backlight = percent;
}

int host_panel_backlight(void)
{
    return s_backlight;
}

/* The touch UI's own sounds, mapped as ui_port_esp.c maps them on the panel. */
static void be_beep(void *ctx, ui_beep_t beep)
{
    (void)ctx;
    if (beep == UI_BEEP_KEY) {
        if (s_snd.key_click != NULL) {
            s_snd.key_click(s_snd.ctx);
        }
    } else {
        say_sound(beep == UI_BEEP_TEST ? ALARM_SOUND_ACK : ALARM_SOUND_ERROR);
    }
}

/* The three everyday volumes (AUD-032), each on its own, as ui_port_esp.c sets them. */
static void VP_set_volumes(uint8_t beeps, uint8_t chime, uint8_t voice); /* the voice's, below */
static void VP_volumes(uint8_t *beeps, uint8_t *chime, uint8_t *voice);

static void be_set_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    if (s_snd.volume != NULL) {
        s_snd.volume(s_snd.ctx, percent);
    }
    uint8_t b, c, v;
    VP_volumes(&b, &c, &v);
    VP_set_volumes(percent, c, v); /* Beeps only: the voice no longer follows it */
}

static void be_set_chime_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    if (s_snd.chime_volume != NULL) {
        s_snd.chime_volume(s_snd.ctx, percent);
    }
    uint8_t b, c, v;
    VP_volumes(&b, &c, &v);
    VP_set_volumes(b, percent, v);
}

static void be_set_voice_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    if (s_snd.voice_volume != NULL) {
        s_snd.voice_volume(s_snd.ctx, percent);
    }
    uint8_t b, c, v;
    VP_volumes(&b, &c, &v);
    VP_set_volumes(b, c, percent); /* 0 = no speech (D06) */
}

/* ---- the voice (docs/features/08 voice-first-build.md D03, D06, D07, D09) -------------------
 * The real en-US voice, packed unsigned at build time from the tracked assets (HOST_VOICE_PACK,
 * host/ui_preview/voice_pack.cmake) and opened with vp_open, as the panel opens its installed
 * pack after the signature check. Without Python at build time there is no pack: NONE. The
 * environment variable ALARM_PREVIEW_VOICE=none|loading|unavailable (or
 * host_panel_voice_pretend()) makes the voice look that way, for the screens' states. It
 * outlives host_panel_init(), as the installed pack outlives the engine restarting. */
static struct {
    bool tried;
    uint8_t *file;
    vp_pack_t pack;
    uint8_t state;   /* ui_voice_state_t: what the pack is */
    int pretend;     /* -1: the pack's own state, else the ui_voice_state_t shown */
    uint16_t word_gap_ms; /* the pack's own (host_panel_voice_word_gap) */
    unsigned leases;
    host_voice_player_t player;
    void *player_ctx;
} s_voice = {.pretend = -1};

static void voice_load(void)
{
    if (s_voice.tried) {
        return;
    }
    s_voice.tried = true;
    s_voice.state = UI_VOICE_NONE;
    const char *env = getenv("ALARM_PREVIEW_VOICE");
    if (env != NULL && strcmp(env, "none") == 0) {
        s_voice.pretend = UI_VOICE_NONE;
    } else if (env != NULL && strcmp(env, "loading") == 0) {
        s_voice.pretend = UI_VOICE_LOADING;
    } else if (env != NULL && strcmp(env, "unavailable") == 0) {
        s_voice.pretend = UI_VOICE_UNAVAILABLE;
    }
    /* -VoicePack <file> (tools/ui_sim.ps1, tools/ui_preview.ps1): another unsigned pack. */
    const char *path = getenv("ALARM_VOICE_PACK");
#ifdef HOST_VOICE_PACK
    if (path == NULL || path[0] == '\0') {
        path = HOST_VOICE_PACK;
    }
#endif
    if (path == NULL || path[0] == '\0') {
        printf("voice: built without a pack (state NONE)\n");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("voice: no pack at %s (state NONE)\n", path);
        return;
    }
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        len = ftell(f);
    }
    rewind(f);
    if (len > 0) {
        s_voice.file = malloc((size_t)len);
    }
    if (s_voice.file == NULL || fread(s_voice.file, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(s_voice.file);
        s_voice.file = NULL;
        s_voice.state = UI_VOICE_UNAVAILABLE;
        printf("voice: cannot read %s (state UNAVAILABLE)\n", path);
        return;
    }
    fclose(f);
    vp_problem_t why;
    const vp_err_t err = vp_open(s_voice.file, (size_t)len, &s_voice.pack, &why);
    if (err != VP_OK) {
        s_voice.state = UI_VOICE_UNAVAILABLE;
        printf("voice: pack refused: %s at entry %u (state UNAVAILABLE)\n", vp_err_name(err),
               (unsigned)why.entry);
        return;
    }
    s_voice.state = UI_VOICE_READY;
    s_voice.word_gap_ms = s_voice.pack.word_gap_ms;
    printf("voice: loaded %s %s revision %lu%s (state READY, %u clips)\n", s_voice.pack.locale,
           s_voice.pack.voice_id, (unsigned long)s_voice.pack.pack_revision,
           vp_is_draft(&s_voice.pack) ? " draft" : "", (unsigned)s_voice.pack.clip_count);
}

static uint8_t voice_state(void)
{
    voice_load();
    return s_voice.pretend >= 0 ? (uint8_t)s_voice.pretend : s_voice.state;
}

void host_panel_voice_pretend(int state)
{
    voice_load();
    s_voice.pretend = state;
    vx_apply_pack();
}

unsigned host_panel_voice_leases(void)
{
    return s_voice.leases;
}

void host_panel_voice_word_gap(uint16_t ms)
{
    voice_load();
    if (s_voice.file != NULL) {
        s_voice.pack.word_gap_ms = (ms != 0u) ? ms : s_voice.word_gap_ms;
    }
}

void host_panel_voice_player(host_voice_player_t play, void *ctx)
{
    s_voice.player = play;
    s_voice.player_ctx = ctx;
}

static void be_voice_info(void *ctx, ui_voice_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->state = voice_state();
    if (out->state == UI_VOICE_NONE || s_voice.file == NULL) {
        return; /* no pack: no locale, voice or revision */
    }
    out->draft = vp_is_draft(&s_voice.pack);
    snprintf(out->locale, sizeof(out->locale), "%s", s_voice.pack.locale);
    snprintf(out->voice_name, sizeof(out->voice_name), "%s", s_voice.pack.voice_id);
    out->pack_revision = s_voice.pack.pack_revision;
}

static const vp_pack_t *be_voice_pack_acquire(void *ctx)
{
    (void)ctx;
    if (voice_state() != UI_VOICE_READY) {
        return NULL;
    }
    s_voice.leases++;
    return &s_voice.pack;
}

static void be_voice_pack_release(void *ctx, const vp_pack_t *pack)
{
    (void)ctx;
    if (pack != NULL && s_voice.leases > 0u) {
        s_voice.leases--;
    }
}

/* D03: the words checked as the panel's voice checks them; then one trace line, and the
 * simulator's audio when it has plugged a player in (host_panel_voice_player). Volume, power
 * saving and alarms (QUIET) are the player's to judge. */
static uint8_t be_voice_preview(void *ctx, const uint16_t *ids, size_t n)
{
    (void)ctx;
    if (voice_state() != UI_VOICE_READY) {
        return UI_VOICE_PREVIEW_UNAVAILABLE;
    }
    vp_recipe_check_t chk;
    if (ids == NULL || !vp_check_recipe(&s_voice.pack, ids, n, &chk)) {
        return ids != NULL && chk.verdict == VP_RECIPE_TOO_LONG ? UI_VOICE_PREVIEW_TOO_LONG
                                                                : UI_VOICE_PREVIEW_BAD_WORDS;
    }
    printf("voice preview:");
    for (size_t i = 0; i < n; i++) {
        printf(" %u", (unsigned)ids[i]);
    }
    printf("\n");
    if (s_voice.player != NULL) { /* a player of its own takes the words as they are */
        return s_voice.player(s_voice.player_ctx, &s_voice.pack, ids, n);
    }
    return vx_preview(ids, n);
}

/* D07: the identity the editor's Done saw (ui_backend/ui_voice.h). */
static bool be_voice_seen(void *ctx, uint16_t zone, ui_voice_seen_t *out)
{
    (void)ctx;
    return ui_voice_seen_engine(&H.eng, zone, out);
}

/* D07: as the panel's port does - a lease on the pack, which must be the one the editor showed
 * and must say the words; the page's save (be_set_config, always applied) and the approval
 * against the identity Done saw, one transaction on the engine (ui_backend/ui_voice.h). */
static uint8_t be_voice_apply_approve(void *ctx, const alarm_config_t *cfg, uint16_t zone,
                                      const ui_voice_seen_t *seen, const uint16_t *ids, size_t n,
                                      alarm_result_t *saved)
{
    const vp_pack_t *lease = be_voice_pack_acquire(ctx);
    const uint8_t r = ui_voice_apply_approve(&H.eng, lease, cfg, zone, seen, ids, n,
                                             be_set_config, ctx, saved, H.now);
    if (r == UI_VOICE_APPROVE_OK) {
        H.config_seq++; /* the configuration changed, as after be_set_config */
        H.dirty = true;
    }
    be_voice_pack_release(ctx, lease);
    return r;
}

/* ---- the voice policy on the PC (docs/features/08 voice-first-build.md D05, D11, D15) -------
 * The panel's wiring (panel_app.c's listener, panel_voice.c) over the host engine: events, sounds,
 * the bell and each snapshot go to the core annunciator; the zone words come from the
 * configuration; the utterances it hands out are played by the simulator (s_snd.speak: tones
 * with tickets, speech through the panel's mixer) or, with no sound card, rendered with
 * voice_render to a WAV and a trace line, in virtual time (the preview). */
#define VX_TONE_STEP_MS 10u
#define VX_LOG_MAX 64u

static struct {
    ann_prefs_t prefs; /* the keypad's Announcements, the voice volume; survives host_panel_init() */
    uint8_t beeps_volume; /* the virtual card's Beeps and Chime volumes (AUD-032) */
    uint8_t chime_volume;
    char dir[200];
    bool dir_set;
    unsigned wav_n;
    host_voice_log_t log[VX_LOG_MAX];
    unsigned log_n;
    void *codec;       /* voice_render's decoder state */
    size_t codec_bytes;
} VP = {.prefs = {.voice_status = true, .voice_chime = true, .volume = 70u},
        .beeps_volume = 70u,
        .chime_volume = 70u};

static struct {
    bool on;
    ann_t ann;
    ann_zone_words_t words[ALARM_MAX_ZONES];
    bool words_valid;
    bool exit_sounds;
    uint32_t next_ticket;
    uint64_t bell_due;
    bool act;                /* an utterance plays */
    uint32_t act_id;
    uint8_t act_kind;        /* its ann_kind_t */
    uint32_t preview_cutoff; /* Preview utterances with an id up to this are stopped */
    snd_mixer_t vmix;        /* virtual card: the panel's mixer, tones and speech */
    uint64_t vtime;
    unsigned vpending;       /* tracked tones whose end is not read yet */
    alarm_sound_t cont;      /* the continuous pattern the snapshot asks for */
    vr_t vr;
    struct {                 /* the virtual card's utterance */
        bool on, finished, failed;
        bool pulled_all;     /* vr_pull returned 0: the boost's held samples follow */
        speech_boost_t sb;   /* AUD-033, as the panel's worker */
        bool done;           /* ended, the policy not told yet */
        FILE *wav;
        uint32_t frames;
        int16_t ring[SND_SAMPLE_RATE * 120u / 1000u];
        size_t ring_n;
        snd_mixer_t start_mix; /* the mixer as the first recorded block found it (tests) */
    } vs;
    power_saving_t psave;    /* the battery policy (power_saving_update) */
    power_sample_t psample;
} V;

static bool vx_virtual_tones(void)
{
    return s_snd.tone == NULL && s_snd.one_shot == NULL;
}

static bool vx_zone_words(void *ctx, uint16_t zone, ann_zone_words_t *out)
{
    (void)ctx;
    if (!V.words_valid || zone == 0u || zone > ALARM_MAX_ZONES) {
        return false;
    }
    *out = V.words[zone - 1u];
    return out->count != 0u;
}

static uint32_t vx_clip_samples(void *ctx, uint16_t id)
{
    (void)ctx;
    return voice_state() == UI_VOICE_READY ? ann_pack_clip_samples(&s_voice.pack, id) : 0u;
}

static void vx_apply_prefs(void)
{
    if (V.on) {
        ann_set_prefs(&V.ann, &VP.prefs, H.now);
    }
}

static void vx_apply_pack(void)
{
    if (!V.on) {
        return;
    }
    ann_pack_t p = {0};
    if (voice_state() == UI_VOICE_READY) {
        p.ready = true;
        p.revision = s_voice.pack.pack_revision;
    }
    ann_set_pack(&V.ann, &p, H.now);
}

static void vx_init(void)
{
    if (V.vs.wav != NULL) {
        fclose(V.vs.wav);
    }
    memset(&V, 0, sizeof(V));
    V.psample = (power_sample_t){.ac_present = POWER_YES, .battery_mv = -1, .battery_percent = -1};
    VP.prefs.power_saving = false;
    const ann_ports_t ports = {.ctx = NULL, .zone_words = vx_zone_words, .clip_samples = vx_clip_samples};
    ann_init(&V.ann, &ports, 1u);
    snd_init(&V.vmix);
    snd_set_volume(&V.vmix, VP.beeps_volume);
    snd_set_chime_volume(&V.vmix, VP.chime_volume);
    snd_set_voice_volume(&V.vmix, VP.prefs.volume);
    V.on = true;
    vx_apply_prefs();
    vx_apply_pack();
}

void host_panel_voice_out(const char *dir)
{
    VP.dir_set = dir != NULL;
    VP.wav_n = 0u;
    if (dir != NULL) {
        snprintf(VP.dir, sizeof(VP.dir), "%s", dir);
    }
}

void host_panel_voice_prefs(bool enabled, bool status, bool chime)
{
    VP.prefs.voice_enabled = enabled;
    VP.prefs.voice_status = status;
    VP.prefs.voice_chime = chime;
    vx_apply_prefs();
}

static void VP_volumes(uint8_t *beeps, uint8_t *chime, uint8_t *voice)
{
    *beeps = VP.beeps_volume;
    *chime = VP.chime_volume;
    *voice = VP.prefs.volume;
}

static void VP_set_volumes(uint8_t beeps, uint8_t chime, uint8_t voice)
{
    VP.beeps_volume = beeps;
    VP.chime_volume = chime;
    VP.prefs.volume = voice; /* the voice follows its own volume (AUD-032) */
    snd_set_volume(&V.vmix, beeps);
    snd_set_chime_volume(&V.vmix, chime);
    snd_set_voice_volume(&V.vmix, voice);
    vx_apply_prefs();
}

static void VP_set_options(const ui_options_t *opt)
{
    VP.prefs.voice_enabled = opt->voice_enabled;
    VP.prefs.voice_status = opt->voice_status;
    VP.prefs.voice_chime = opt->voice_chime;
    VP_set_volumes(opt->volume, opt->chime_volume, opt->voice_volume);
}

unsigned host_panel_voice_log(host_voice_log_t *out, unsigned max)
{
    const unsigned n = VP.log_n < max ? VP.log_n : max;
    memcpy(out, VP.log, n * sizeof(*out));
    return n;
}

void host_panel_voice_log_clear(void)
{
    VP.log_n = 0u;
}

static host_voice_log_t *vx_log(void)
{
    if (VP.log_n >= VX_LOG_MAX) { /* the oldest go */
        memmove(VP.log, VP.log + 1, (VX_LOG_MAX - 1u) * sizeof(VP.log[0]));
        VP.log_n = VX_LOG_MAX - 1u;
    }
    host_voice_log_t *e = &VP.log[VP.log_n++];
    memset(e, 0, sizeof(*e));
    e->at_ms = H.now;
    return e;
}

/* Reads the zones' spoken names when the configuration changed (the panel's worker does it off
 * the alarm task; the engine's own pointer is read here, after the call). */
static void vx_refresh_words(void)
{
    static uint32_t seq = 0xFFFFFFFFu;
    if (V.words_valid && seq == H.config_seq) {
        return;
    }
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    for (unsigned z = 0u; z < ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = &cfg->zones[z];
        ann_zone_words_t *w = &V.words[z];
        memset(w, 0, sizeof(*w));
        const unsigned n = alarm_zone_word_count(zc);
        for (unsigned i = 0u; i < n && i < ANN_CLIPS_MAX; i++) {
            w->ids[i] = alarm_zone_word(zc, i);
        }
        w->count = (uint8_t)(n < ANN_CLIPS_MAX ? n : ANN_CLIPS_MAX);
        w->binding_revision = zc->voice_binding_revision;
        w->approved_revision = zc->voice_approved_revision;
        w->pack_revision = zc->voice_pack_revision;
    }
    V.exit_sounds = cfg->opt.exit_sounds;
    V.words_valid = true;
    seq = H.config_seq;
}

/* The engine's one-shot for the keypad: the ticket the policy waits on (0: not tracked). */
static uint32_t vx_tone(alarm_sound_t sound)
{
    if (s_snd.tone != NULL) {
        const uint32_t t = ++V.next_ticket;
        (void)s_snd.tone(s_snd.ctx, sound, t);
        return t;
    }
    if (s_snd.one_shot != NULL) {
        say_sound(sound);
        return 0u;
    }
    if (!V.on) {
        return 0u;
    }
    /* the virtual card: the same mixer, its time moved by vx_pump() */
    const uint32_t t = ++V.next_ticket;
    host_voice_log_t *e = vx_log();
    e->sound = sound;
    (void)snd_play_tracked(&V.vmix, sound, t);
    V.vpending++;
    return t;
}

static void vx_sound(uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket)
{
    if (V.on) {
        ann_sound(&V.ann, partition, sound, zone, ticket);
    }
}

static void vx_event(const alarm_event_t *ev)
{
    if (V.on) {
        ann_event(&V.ann, ev);
    }
}

static void vx_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    if (!V.on) {
        return;
    }
    ann_output(&V.ann, output, partition, pattern);
    if (output == ALARM_OUT_BELL && partition == 1u && pattern == ALARM_PAT_TEST) {
        V.bell_due = H.now + 2000u; /* the bell test pulse (panel_voice.c PV_BELL_TEST_MS) */
    }
}

static void vx_snapshot(const alarm_snapshot_t *snap)
{
    if (!V.on) {
        return;
    }
    if (vx_virtual_tones()) {
        snd_set_continuous(&V.vmix, snap->part[0].sounder);
        V.cont = snap->part[0].sounder;
    }
    vx_refresh_words();
    ann_state_t st;
    ann_state_from_snapshot(snap, 1u, V.exit_sounds, &st);
    ann_commit(&V.ann, &st, H.now);
}

/* Voice only: this engine call's events are in, its fresh snapshot is committed (it does not
 * re-enter the hub, and leaves H.dirty to after(), which feeds the automations). */
static void vx_commit(void)
{
    if (!V.on) {
        return;
    }
    static alarm_snapshot_t snap;
    alarm_engine_snapshot(&H.eng, &snap);
    snap.config_seq = H.config_seq;
    vx_snapshot(&snap);
}

/* ---- the power policy (panel_app.c's ui_power) ---- */
static void vx_power_apply(bool saving)
{
    VP.prefs.power_saving = saving;
    vx_apply_prefs(); /* the policy: no automatic speech, no Preview; what plays is cancelled */
    snd_set_power_saving(&V.vmix, saving);
    if (s_snd.power_saving != NULL) {
        s_snd.power_saving(s_snd.ctx, saving);
    }
}

static void vx_power_poll(void)
{
    if (!V.on) {
        return;
    }
    const bool was = V.psave.saving;
    const bool saving = power_saving_update(&V.psave, &V.psample, H.now);
    if (saving != was) {
        vx_power_apply(saving);
    }
}

void host_panel_power_sample(const power_sample_t *sample)
{
    V.psample = *sample;
    vx_power_poll();
    vx_pump();
}

bool host_panel_voice_mixer_at_start(snd_mixer_t *out)
{
    if (!V.on || V.vs.start_mix.rate == 0u) {
        return false;
    }
    *out = V.vs.start_mix;
    return true;
}

bool host_panel_power_saving(void)
{
    return V.psave.saving;
}

static const char *vx_kind_name(unsigned kind)
{
    static const char *const names[] = {"none",       "entry",     "arming",    "armed",
                                        "disarmed",   "walk-start", "walk-end", "walk-warning",
                                        "not-ready",  "walk-trip", "chime",     "preview"};
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

/* Starts an utterance the policy handed out. false: it could not (the policy is told). */
static bool vx_start(const ann_utt_t *u)
{
    vr_item_t items[ANN_CLIPS_MAX];
    for (unsigned i = 0u; i < u->count && i < ANN_CLIPS_MAX; i++) {
        items[i] = (vr_item_t){.clip_id = u->clips[i], .gap_before_ms = u->gap_ms[i]};
    }
    if (VP.codec == NULL) {
        VP.codec_bytes = vc_state_bytes();
        VP.codec = malloc(VP.codec_bytes);
    }
    if (VP.codec == NULL || voice_state() != UI_VOICE_READY ||
        vr_begin(&V.vr, &s_voice.pack, items, u->count, VP.codec, VP.codec_bytes) != 0) {
        printf("voice: %s not started\n", vx_kind_name(u->kind));
        return false;
    }
    const uint32_t total = vr_total_samples_32k(&V.vr);
    const uint32_t ms = (total + VR_SAMPLES_PER_MS - 1u) / VR_SAMPLES_PER_MS;
    host_voice_log_t *e = vx_log();
    e->speech = true;
    e->kind = u->kind;
    e->zone = u->zone;
    e->count = u->count;
    memcpy(e->clips, u->clips, sizeof(e->clips));
    memcpy(e->gap_ms, u->gap_ms, sizeof(e->gap_ms));
    e->total_32k = total;
    printf("voice: %s %u ", vx_kind_name(u->kind), (unsigned)u->zone);
    for (unsigned i = 0u; i < u->count; i++) {
        printf("%s%u", i == 0u ? "" : ",", (unsigned)u->clips[i]);
    }
    printf(" %u\n", (unsigned)ms);
    if (s_snd.speak != NULL) {
        return s_snd.speak(s_snd.ctx, &s_voice.pack, items, u->count, u->id);
    }
    /* the virtual card plays it through the panel's mixer; the WAV is what the speaker gets */
    e->id = u->id;
    memset(&V.vs, 0, sizeof(V.vs));
    V.vs.on = true;
    V.vs.start_mix = V.vmix;
    sb_begin(&V.vs.sb, V.vmix.voice_volume);
    if (VP.dir_set) {
        char path[320];
        e->wav_number = ++VP.wav_n;
        snprintf(path, sizeof(path), "%s/%u-%s.wav", VP.dir, e->wav_number, vx_kind_name(u->kind));
        V.vs.wav = fopen(path, "wb");
        if (V.vs.wav != NULL) {
            const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm',
                                     't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x00, 0x7D, 0, 0, 0x00, 0xF4,
                                     0x01, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
            fwrite(hdr, 1, sizeof(hdr), V.vs.wav);
        }
    }
    return true;
}

static host_voice_log_t *vx_log_of(uint32_t id)
{
    for (unsigned i = VP.log_n; i > 0u; i--) {
        if (VP.log[i - 1u].speech && VP.log[i - 1u].id == id) {
            return &VP.log[i - 1u];
        }
    }
    return NULL;
}

/* The utterance ends: played to its last sample, or cancelled (an alarm, a new identity, power
 * saving). The WAV holds what was recorded until then. */
static void vx_speech_end(bool completed)
{
    host_voice_log_t *e = vx_log_of(V.act_id);
    if (e != NULL) {
        e->completed = completed;
        e->cancelled = !completed;
        e->wav_samples = V.vs.frames;
    }
    if (V.vs.wav != NULL) {
        const uint32_t data = V.vs.frames * 4u, riff = data + 36u;
        fseek(V.vs.wav, 4, SEEK_SET);
        for (unsigned i = 0u; i < 4u; i++) {
            fputc((int)((riff >> (8u * i)) & 0xFFu), V.vs.wav);
        }
        fseek(V.vs.wav, 40, SEEK_SET);
        for (unsigned i = 0u; i < 4u; i++) {
            fputc((int)((data >> (8u * i)) & 0xFFu), V.vs.wav);
        }
        fclose(V.vs.wav);
        V.vs.wav = NULL;
    }
    printf("voice: %s\n", completed ? "completed" : "cancelled");
    V.vs.on = false;
    V.vs.ring_n = 0u;
    V.vs.done = true;
}

/* Must the virtual card render? Tones to read, an utterance, or the tail of a sound (the
 * amplifier rests after it) - but not a continuous pattern, which nobody records. */
static bool vx_card_busy(void)
{
    return V.vpending > 0u || V.vs.on || (V.cont == ALARM_SOUND_NONE && !snd_idle(&V.vmix));
}

/* AUD-034: speech queued or playing - the mixer holds the exit beeps for it, as the panel's
 * sounder task does with panel_voice's flag (sounder_speech_due). */
static bool vx_due(void)
{
    return V.on && ann_due(&V.ann, H.now);
}

/* One 10 ms block through the panel's mixer (snd_render_pcm); the utterance is recorded from it. */
static void vx_block(void)
{
    const size_t cap = sizeof(V.vs.ring) / sizeof(V.vs.ring[0]);
    while (V.vs.on && !V.vs.finished && !V.vs.failed && V.vs.ring_n < cap) {
        size_t room = cap - V.vs.ring_n;
        if (room > 640u) {
            room = 640u;
        }
        if (V.vs.pulled_all) {
            const size_t held = sb_flush(&V.vs.sb, V.vs.ring + V.vs.ring_n, room);
            V.vs.ring_n += held;
            V.vs.finished = held == 0u;
            continue;
        }
        const int32_t n = vr_pull(&V.vr, V.vs.ring + V.vs.ring_n, room);
        if (n < 0) {
            V.vs.failed = true;
        } else if (n == 0) {
            V.vs.pulled_all = true;
        } else {
            sb_process(&V.vs.sb, V.vs.ring + V.vs.ring_n, (size_t)n);
            V.vs.ring_n += (size_t)n;
        }
    }
    int16_t block[SND_BLOCK_FRAMES * 2u];
    const bool speaking = V.vs.on;
    snd_set_speech_due(&V.vmix, vx_due());
    const snd_pcm_block_t b =
        snd_render_pcm(&V.vmix, block, SND_BLOCK_FRAMES, V.vs.ring, V.vs.ring_n, speaking);
    if (b.speech_consumed > 0u) {
        V.vs.ring_n -= b.speech_consumed;
        memmove(V.vs.ring, V.vs.ring + b.speech_consumed, V.vs.ring_n * sizeof(int16_t));
    }
    V.vtime += VX_TONE_STEP_MS;
    if (!speaking) {
        return;
    }
    if (b.speech_rejected || V.vs.failed) { /* over an alarm, fire, CO or aux: nothing recorded */
        vx_speech_end(false);
        return;
    }
    if (V.vs.wav != NULL) {
        for (size_t i = 0; i < SND_BLOCK_FRAMES * 2u; i++) { /* little-endian on any host */
            const uint8_t le[2] = {(uint8_t)((uint16_t)block[i] & 0xFFu),
                                   (uint8_t)((uint16_t)block[i] >> 8)};
            fwrite(le, 1, 2, V.vs.wav);
        }
    }
    V.vs.frames += SND_BLOCK_FRAMES;
    if (V.vs.finished && V.vs.ring_n == 0u) {
        vx_speech_end(true);
    }
}

/* Moves the virtual sound card to H.now and gives the policy the ends of the tones. */
static void vx_tones(void)
{
    uint32_t t;
    bool played;
    if (vx_virtual_tones()) {
        while (vx_card_busy() && V.vtime + VX_TONE_STEP_MS <= H.now) {
            vx_block();
            snd_ticket_t o;
            while (snd_ticket_next(&V.vmix, &o)) {
                if (V.vpending > 0u) {
                    V.vpending--;
                }
                ann_ticket(&V.ann, o.ticket, o.outcome == SND_TICKET_PLAYED, V.vtime);
            }
        }
        if (!vx_card_busy()) {
            V.vtime = H.now;
        }
    } else if (s_snd.ticket != NULL) {
        while (s_snd.ticket(s_snd.ctx, &t, &played)) {
            ann_ticket(&V.ann, t, played, H.now);
        }
    }
}

/* A Preview stopped by voice_preview_stop (panel_voice's s_preview_cutoff): every one queued before it. */
static bool vx_preview_stopped(uint8_t kind, uint32_t id)
{
    return kind == (uint8_t)ANN_KIND_PREVIEW && id <= V.preview_cutoff;
}

static void vx_pump_round(void);

/* One round of the voice worker, then the sound card told whether speech is due (AUD-034). */
static void vx_pump(void)
{
    vx_pump_round();
    if (s_snd.speech_due != NULL) {
        s_snd.speech_due(s_snd.ctx, vx_due());
    }
}

/* One round of the voice worker: tones' ends, the bell pulse, the active utterance, the next. */
static void vx_pump_round(void)
{
    if (!V.on) {
        return;
    }
    vx_tones();
    if (V.bell_due != 0u && H.now >= V.bell_due) {
        V.bell_due = 0u;
        ann_bell_test_done(&V.ann, H.now);
    }
    for (unsigned round = 0u; round < 8u; round++) {
        if (V.act) {
            bool done = false;
            if (s_snd.speak != NULL) {
                uint32_t id;
                bool completed;
                if (s_snd.speech_done != NULL && s_snd.speech_done(s_snd.ctx, &id, &completed) &&
                    id == V.act_id) {
                    done = true;
                }
            } else if (V.vs.done) {
                done = true;
                V.vs.done = false;
            }
            const bool stopped = vx_preview_stopped(V.act_kind, V.act_id);
            if (!done && (stopped || !ann_current(&V.ann, V.act_id))) {
                if (s_snd.speech_cancel != NULL) {
                    s_snd.speech_cancel(s_snd.ctx);
                } else if (s_snd.speak == NULL && V.vs.on) {
                    vx_speech_end(false);
                }
                if (stopped) {
                    ann_done(&V.ann, V.act_id, H.now); /* the policy didn't end it: Preview's stop did */
                }
                V.vs.done = false;
                V.act = false; /* canceled by the policy */
            } else if (done) {
                ann_done(&V.ann, V.act_id, H.now);
                V.act = false;
            } else {
                return;
            }
        }
        ann_utt_t u;
        if (!ann_next(&V.ann, H.now, &u)) {
            return;
        }
        if (vx_preview_stopped(u.kind, u.id)) {
            printf("voice: preview stopped before it started\n");
            ann_done(&V.ann, u.id, H.now);
        } else if (vx_start(&u)) {
            V.act = true;
            V.act_id = u.id;
            V.act_kind = u.kind;
        } else {
            ann_done(&V.ann, u.id, H.now);
        }
    }
}

/* The next time the voice needs the clock (0: none), for host_panel_advance()'s steps. */
static uint64_t vx_next_deadline(void)
{
    uint64_t next = UINT64_MAX;
    if (!V.on) {
        return next;
    }
    if (V.bell_due > H.now && V.bell_due < next) {
        next = V.bell_due;
    }
    if (vx_virtual_tones() && vx_card_busy() && H.now + VX_TONE_STEP_MS < next) {
        next = H.now + VX_TONE_STEP_MS;
    }
    if (V.psave.candidate != V.psave.saving && V.psave.since + 15000u > H.now &&
        V.psave.since + 15000u < next) {
        next = V.psave.since + 15000u; /* the power policy's 15 s of evidence */
    }
    return next;
}

/* D03: through the policy, as the panel's panel_voice_preview: volume, power saving, an alarm and
 * a full queue are its to judge; it plays after anything more important. */
static uint8_t vx_preview(const uint16_t *ids, size_t n)
{
    const ann_preview_result_t r = ann_preview(&V.ann, ids, n, false, H.now);
    vx_pump();
    switch (r) {
    case ANN_PREVIEW_OK: return UI_VOICE_PREVIEW_OK;
    case ANN_PREVIEW_NO_PACK: return UI_VOICE_PREVIEW_UNAVAILABLE;
    case ANN_PREVIEW_EMPTY:
    case ANN_PREVIEW_BAD_CLIP:
    case ANN_PREVIEW_TOO_MANY: return UI_VOICE_PREVIEW_BAD_WORDS;
    case ANN_PREVIEW_TOO_LONG: return UI_VOICE_PREVIEW_TOO_LONG;
    case ANN_PREVIEW_MUTED:
    case ANN_PREVIEW_ALARM:
    case ANN_PREVIEW_BUSY:
    default: return UI_VOICE_PREVIEW_QUIET;
    }
}

/* Preview only, as panel_voice_preview_stop: what is queued or playing ends now (the WAV and the
 * log say cancelled); automatic speech and tones go on. A player of its own (the simulator's)
 * plays its words to the end. */
static void be_voice_preview_stop(void *ctx)
{
    (void)ctx;
    if (!V.on) {
        return;
    }
    V.preview_cutoff = V.ann.next_id; /* every Preview queued so far: ids only grow */
    printf("voice: preview stop\n");
    vx_pump();
}

/* The simulated latched loss of the event log (LOG-010). */
static uint32_t s_log_lost;
static uint32_t s_log_lost_reports;

void host_panel_log_loss(uint32_t records, uint32_t reports)
{
    s_log_lost = records;
    s_log_lost_reports = reports;
}

void host_panel_fw_event(uint8_t stage, uint8_t keypad, uint8_t why)
{
    const alarm_source_t src = keypad != 0u
                                   ? (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = keypad}
                                   : (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0};
    (void)alarm_engine_platform_event_ex(&H.eng, ALARM_EV_FIRMWARE_UPDATE, 0, stage, why, &src, 0,
                                         H.now);
    H.dirty = true;
}

static void be_log_loss(void *ctx, uint32_t *records, uint32_t *reports)
{
    (void)ctx;
    *records = s_log_lost;
    *reports = s_log_lost_reports;
}

static void be_log_clear_loss(void *ctx)
{
    (void)ctx;
    s_log_lost = 0u;
    s_log_lost_reports = 0u;
}

static void be_about(void *ctx, ui_about_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    snprintf(out->firmware, sizeof(out->firmware), "0.6.0 (PC preview)");
    snprintf(out->built, sizeof(out->built), "Sep 19 2026 22:41:00");
    snprintf(out->idf, sizeof(out->idf), "v6.1");
    snprintf(out->chip, sizeof(out->chip), "ESP32-P4 v1.0 (v1.x image)");
    out->uptime_s = (uint32_t)(H.now / 1000u) + 3u * 86400u;
    out->heap_internal_kb = 214;
    out->heap_psram_kb = 29870;
    out->log_count = H.log.count;
    out->log_capacity = elog_capacity(&H.log);
    snprintf(out->panel_id, sizeof(out->panel_id), "0c8b95a1f2d4");
}

/* Storage fake models asynchronous completion; never touches the host filesystem. */
static storage_sd_t s_sd;
static uint64_t s_storage_done;
static unsigned s_formats;
static bool s_format_fail;
void host_panel_storage_preset(storage_state_t state, uint64_t capacity, uint64_t free_bytes)
{
    const uint32_t generation = s_sd.generation + 1u;
    memset(&s_sd, 0, sizeof(s_sd));
    s_sd.generation = generation;
    s_sd.state = state;
    s_sd.capacity_bytes = capacity;
    s_sd.total_bytes = capacity;
    s_sd.free_bytes = free_bytes;
    s_sd.usage_valid = state == STORAGE_READY;
    if (capacity) snprintf(s_sd.name, sizeof(s_sd.name), "SD64G");
    snprintf(s_sd.filesystem, sizeof(s_sd.filesystem), "FAT32");
    snprintf(s_sd.message, sizeof(s_sd.message), "%s", state == STORAGE_READY ?
             "Card is ready. Safely remove before taking it out." :
             state == STORAGE_UNSUPPORTED ? "No supported FAT filesystem. Files have not been changed." :
             "Insert a card, then choose Check card.");
    s_storage_done = 0;
}
static void storage_reset(void)
{
    s_formats = 0;
    s_format_fail = false;
    host_panel_storage_preset(STORAGE_READY, UINT64_C(64000000000), UINT64_C(48000000000));
}
void host_panel_storage_fail(bool fail) { s_format_fail = fail; }
unsigned host_panel_storage_formats(void) { return s_formats; }
static void be_storage_sd(void *ctx, storage_sd_t *out)
{
    (void)ctx;
    if (s_sd.generation == 0u) host_panel_storage_preset(STORAGE_READY, UINT64_C(64000000000), UINT64_C(48000000000));
    if (s_storage_done && H.now >= s_storage_done) {
        s_storage_done = 0;
        s_sd.generation++;
        s_sd.state = s_format_fail ? STORAGE_ERROR : STORAGE_READY;
        s_sd.usage_valid = !s_format_fail;
        s_sd.free_bytes = s_format_fail ? 0u : s_sd.total_bytes;
        snprintf(s_sd.message, sizeof(s_sd.message), "%s", s_format_fail ?
                 "Format failed. Card contents may be lost. Check the card before retrying." : "Format complete. Card is ready.");
    }
    *out = s_sd;
}
static void be_storage_flash(void *ctx, storage_flash_t *out)
{
    (void)ctx;
    *out = (storage_flash_t){.chip_valid = true, .image_valid = true,
        .chip_bytes = 16u * 1024u * 1024u, .image_bytes = 0x251000,
        .slot_bytes = 4608u * 1024u, .update_bytes = 4608u * 1024u,
        .allocated_bytes = 0xfc3000, .running_slot = "ota_0", .count = 4,
        .partitions = {{"ota_0", 4608u * 1024u}, {"ota_1", 4608u * 1024u},
                       {"nvs", 128u * 1024u}, {"eventlog", 384u * 1024u}}};
}
static bool be_storage_action(void *ctx, storage_action_t action, uint32_t generation, uint8_t user)
{
    (void)ctx;
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    const bool access = user > 0u && user <= ALARM_MAX_USERS &&
        (cfg->users[user - 1u].authority == ALARM_AUTH_MASTER || cfg->users[user - 1u].authority == ALARM_AUTH_INSTALLER);
    bool idle = true;
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        alarm_partition_status_t status;
        alarm_engine_partition_status(&H.eng, p, &status);
        if (status.state != ALARM_STATE_DISARMED || status.bell != ALARM_PAT_OFF || status.triggered) idle = false;
    }
    if (!storage_allowed(&s_sd, action, generation, access, idle, false)) return false;
    if (action == STORAGE_FORMAT) {
        s_formats++;
        s_sd.state = STORAGE_WORKING;
        snprintf(s_sd.message, sizeof(s_sd.message), "Formatting. Keep the card inserted and power connected.");
        s_storage_done = H.now + 5000u;
    } else if (action == STORAGE_EJECT) {
        host_panel_storage_preset(STORAGE_EJECTED, 0u, 0u);
        snprintf(s_sd.message, sizeof(s_sd.message), "Safe to remove. After insertion, choose Check card.");
    } else host_panel_storage_preset(STORAGE_READY, UINT64_C(64000000000), UINT64_C(48000000000));
    return true;
}

/* ---- push notifications: pretend ntfy and Pushover (docs/features/26-28) ------------------------ */

/* Where each service stands, given the network and what has been set up. The real thing works
 * the same way: nothing is "connected", it either got the last one through or it didn't. */
static void notify_step(void)
{
    for (unsigned p = 0; p < UI_NOTIFY_PROVIDERS; p++) {
        const bool on = p == UI_NOTIFY_NTFY ? H.ntfy.enabled : H.push.enabled;
        if (!on) {
            H.notify_state[p] = UI_NOTIFY_OFF;
            H.notify_fail[p] = UI_NOTIFY_FAIL_NONE;
            continue;
        }
        if (host_panel_net_faulty(HOST_NET_WIFI_DOWN) ||
            host_panel_net_faulty(HOST_NET_INTERNET_DOWN)) {
            H.notify_state[p] = UI_NOTIFY_NO_NETWORK;
            continue;
        }
        if (H.notify_down[p]) {
            H.notify_state[p] = UI_NOTIFY_FAILED;
            continue;
        }
        H.notify_fail[p] = UI_NOTIFY_FAIL_NONE;
        H.notify_state[p] = H.notify_ever[p] ? UI_NOTIFY_OK : UI_NOTIFY_READY;
    }
}

static void be_notify_status(void *ctx, uint8_t provider, ui_notify_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->quota_left = -1;
    if (provider >= UI_NOTIFY_PROVIDERS) {
        return;
    }
    notify_step();
    out->state = H.notify_state[provider];
    out->fail = H.notify_fail[provider];
    out->sent = H.notify_sent[provider];
    out->ever_sent = H.notify_ever[provider];
    out->ago_s = H.notify_ever[provider]
                     ? (uint32_t)((H.now - H.notify_last[provider]) / 1000u)
                     : 0u;
    if (H.notify_state[provider] == UI_NOTIFY_FAILED) {
        snprintf(out->detail, sizeof(out->detail),
                 provider == UI_NOTIFY_NTFY ? "The server didn't answer"
                                            : "Pushover said the token is invalid");
        out->fail = provider == UI_NOTIFY_NTFY ? UI_NOTIFY_FAIL_UNREACHABLE
                                               : UI_NOTIFY_FAIL_AUTH;
    }
}

/* The panel starts a test and collects the answer a moment later, because posting it can take
 * ten seconds and the screen must not wait (NOTE-023). The pretend service answers at once, so
 * the result is simply held until the page asks. */
static bool s_test_done;
static bool s_test_ok;
static char s_test_why[80];

static bool be_notify_test(void *ctx, uint8_t provider, char *why, size_t why_len)
{
    (void)ctx;
    if (provider >= UI_NOTIFY_PROVIDERS) {
        return false;
    }
    notify_step();
    if (H.notify_state[provider] == UI_NOTIFY_OFF) {
        snprintf(why, why_len, "Switch it on first");
        return false; /* no point starting one */
    }
    s_test_done = true;
    s_test_why[0] = '\0';
    if (H.notify_state[provider] == UI_NOTIFY_NO_NETWORK) {
        s_test_ok = false;
        snprintf(s_test_why, sizeof(s_test_why), "The panel isn't on the network");
    } else if (H.notify_down[provider]) {
        s_test_ok = false;
        snprintf(s_test_why, sizeof(s_test_why), provider == UI_NOTIFY_NTFY
                                                     ? "The server didn't answer"
                                                     : "Pushover refused the keys");
    } else {
        s_test_ok = true;
        H.notify_sent[provider]++;
        H.notify_last[provider] = H.now;
        H.notify_ever[provider] = true;
    }
    notify_step();
    return true;
}

static bool be_notify_test_result(void *ctx, bool *ok, char *why, size_t why_len)
{
    (void)ctx;
    if (!s_test_done) {
        return false;
    }
    s_test_done = false;
    if (ok != NULL) {
        *ok = s_test_ok;
    }
    if (why != NULL) {
        snprintf(why, why_len, "%s", s_test_why);
    }
    return true;
}

static void be_ntfy_new_topic(void *ctx, char *out, size_t len);

/* A panel that has never been set up still arrives ready: the hosted service, a topic nobody
 * could guess, and the categories most people want. Switching it on is then the only step
 * (NTFY-002, NTFY-003, NOTE-037). */
static void notify_defaults(void)
{
    if (H.ntfy.topic[0] == '\0') {
        be_ntfy_new_topic(NULL, H.ntfy.topic, sizeof(H.ntfy.topic));
        H.ntfy.cats = NOTIFY_CATS_DEFAULT;
        H.ntfy.self_hosted = false;
    }
    if (H.push.cats == 0u) {
        H.push.cats = NOTIFY_CATS_DEFAULT;
        H.push.emergency = true;
        snprintf(H.push.sound, sizeof(H.push.sound), "siren");
    }
}

static void be_ntfy_get(void *ctx, ui_ntfy_settings_t *out)
{
    (void)ctx;
    notify_defaults();
    *out = H.ntfy;
    memset(out->token, 0, sizeof(out->token));
    out->token_set = H.ntfy_token[0] != '\0';
    out->token_change = false;
}

static bool be_ntfy_set(void *ctx, const ui_ntfy_settings_t *in, char *why, size_t why_len)
{
    (void)ctx;
    if (in->enabled && in->topic[0] == '\0') {
        snprintf(why, why_len, "Set a topic first");
        return false;
    }
    if (in->enabled && in->self_hosted && in->server[0] == '\0') {
        snprintf(why, why_len, "Set your server's address first");
        return false;
    }
    const bool token_change = in->token_change;
    char token[sizeof(H.ntfy_token)];
    snprintf(token, sizeof(token), "%s", in->token);
    H.ntfy = *in;
    memset(H.ntfy.token, 0, sizeof(H.ntfy.token));
    H.ntfy.token_change = false;
    if (token_change) {
        snprintf(H.ntfy_token, sizeof(H.ntfy_token), "%s", token);
    }
    memset(token, 0, sizeof(token));
    notify_step();
    return true;
}

/* An unguessable topic, the way the panel makes one: a fixed prefix and random characters. */
static void be_ntfy_new_topic(void *ctx, char *out, size_t len)
{
    (void)ctx;
    static const char ALPHABET[] = "abcdefghijkmnpqrstuvwxyz23456789";
    char tail[9];
    for (unsigned i = 0; i < 8u; i++) {
        H.topic_seq = H.topic_seq * 1103515245u + 12345u;
        tail[i] = ALPHABET[(H.topic_seq >> 16) % (sizeof(ALPHABET) - 1u)];
    }
    tail[8] = '\0';
    snprintf(out, len, "roboalarms-%s", tail);
}

static void be_pushover_get(void *ctx, ui_pushover_settings_t *out)
{
    (void)ctx;
    notify_defaults();
    *out = H.push;
    memset(out->user_key, 0, sizeof(out->user_key));
    memset(out->app_token, 0, sizeof(out->app_token));
    out->user_key_set = H.push_user[0] != '\0';
    out->app_token_set = H.push_app[0] != '\0';
    out->user_key_change = false;
    out->app_token_change = false;
}

static bool be_pushover_set(void *ctx, const ui_pushover_settings_t *in, char *why, size_t why_len)
{
    (void)ctx;
    const bool have_user = in->user_key_change ? in->user_key[0] != '\0' : H.push_user[0] != '\0';
    const bool have_app = in->app_token_change ? in->app_token[0] != '\0' : H.push_app[0] != '\0';
    if (in->enabled && (!have_user || !have_app)) {
        snprintf(why, why_len, "Both the user key and the application token are needed");
        return false;
    }
    /* The real panel checks the key with Pushover before saving it. */
    if (in->user_key_change && strncmp(in->user_key, "uQiRzpo4", 8) != 0 &&
        in->user_key[0] != '\0') {
        snprintf(why, why_len, "Pushover doesn't know that user key");
        return false;
    }
    char user[sizeof(H.push_user)];
    char app[sizeof(H.push_app)];
    snprintf(user, sizeof(user), "%s", in->user_key);
    snprintf(app, sizeof(app), "%s", in->app_token);
    const bool user_change = in->user_key_change;
    const bool app_change = in->app_token_change;
    H.push = *in;
    memset(H.push.user_key, 0, sizeof(H.push.user_key));
    memset(H.push.app_token, 0, sizeof(H.push.app_token));
    H.push.user_key_change = false;
    H.push.app_token_change = false;
    if (user_change) {
        snprintf(H.push_user, sizeof(H.push_user), "%s", user);
    }
    if (app_change) {
        snprintf(H.push_app, sizeof(H.push_app), "%s", app);
    }
    memset(user, 0, sizeof(user));
    memset(app, 0, sizeof(app));
    notify_step();
    return true;
}

void host_panel_notify_preset(uint8_t provider, bool enabled, bool sent_once)
{
    if (provider == UI_NOTIFY_NTFY) {
        H.ntfy.enabled = enabled;
        if (H.ntfy.topic[0] == '\0') {
            be_ntfy_new_topic(NULL, H.ntfy.topic, sizeof(H.ntfy.topic));
        }
    } else {
        H.push.enabled = enabled;
        snprintf(H.push_user, sizeof(H.push_user), "uQiRzpo4DXghDmr9QzzfQu27cmVRsG");
        snprintf(H.push_app, sizeof(H.push_app), "azGDORePK8gMaC0QOYAMyEEuzJnyUi");
    }
    if (sent_once) {
        H.notify_ever[provider] = true;
        H.notify_sent[provider] = 3u;
        H.notify_last[provider] = H.now > 120000u ? H.now - 120000u : 0u;
    }
    notify_step();
}

void host_panel_notify_fault(uint8_t provider, bool on)
{
    if (provider < UI_NOTIFY_PROVIDERS) {
        H.notify_down[provider] = on;
        notify_step();
    }
}

static void be_setup_done(void *ctx)
{
    (void)ctx;
}

static void be_mqtt_get(void *ctx, ui_mqtt_settings_t *out)
{
    (void)ctx;
    *out = H.mqtt;
    memset(out->password, 0, sizeof(out->password));
    out->password_set = H.mqtt_password[0] != '\0';
}

static bool be_mqtt_set(void *ctx, const ui_mqtt_settings_t *in, char *why, size_t why_len)
{
    (void)ctx;
    if (!proto_prefix_valid(in->prefix)) {
        snprintf(why, why_len, "Letters, digits, - and _ only, words split by /");
        return false;
    }
    if (in->panel_id[0] != '\0' && !proto_panel_id_valid(in->panel_id)) {
        snprintf(why, why_len, "Lowercase letters, digits, - and _ only");
        return false;
    }
    if (in->enabled && in->host[0] == '\0') {
        snprintf(why, why_len, "Set the broker's address first");
        return false;
    }
    const bool restart = in->enabled != H.mqtt.enabled || strcmp(in->host, H.mqtt.host) != 0 ||
                         in->port != H.mqtt.port || in->tls != H.mqtt.tls ||
                         strcmp(in->user, H.mqtt.user) != 0 || in->password_change;
    const bool ca = H.mqtt.ca_loaded;
    H.mqtt = *in;
    H.mqtt.ca_loaded = ca;
    if (H.mqtt.panel_id[0] == '\0') {
        snprintf(H.mqtt.panel_id, sizeof(H.mqtt.panel_id), "a1b2c3");
    }
    if (in->password_change) {
        snprintf(H.mqtt_password, sizeof(H.mqtt_password), "%s", in->password);
    }
    memset(H.mqtt.password, 0, sizeof(H.mqtt.password));
    H.mqtt.password_change = false;
    if (restart) {
        H.mqtt_state = UI_MQTT_OFF;
    }
    mqtt_step();
    return true;
}

static void be_mqtt_status(void *ctx, ui_mqtt_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->state = H.mqtt_state;
    out->fail = H.mqtt_fail;
    out->sent = H.mqtt_sent;
    const unsigned port = H.mqtt.port != 0u ? H.mqtt.port
                          : H.mqtt.tls == UI_MQTT_TLS_OFF ? 1883u : 8883u;
    snprintf(out->broker, sizeof(out->broker), "%s:%u", H.mqtt.host, port);
}

void host_panel_mqtt_preset(const char *host, const char *user, const char *password,
                            uint8_t tls, bool enabled, uint32_t sent)
{
    snprintf(H.mqtt.host, sizeof(H.mqtt.host), "%s", host);
    snprintf(H.mqtt.user, sizeof(H.mqtt.user), "%s", user);
    snprintf(H.mqtt_password, sizeof(H.mqtt_password), "%s", password);
    H.mqtt.tls = tls;
    H.mqtt.enabled = enabled;
    H.mqtt_sent = sent;
    H.mqtt_state = UI_MQTT_OFF;
    mqtt_step();
}

static void be_factory_reset(void *ctx, bool erase_log)
{
    (void)ctx;
    printf("  (factory reset requested, event log %s)\n", erase_log ? "too" : "kept");
}

/* ---- the local Home Assistant link (a scripted fake of ha_link, docs/features/23) ---- */

static struct {
    bool enabled;
    bool paired;
    bool window;     /* pairing open, waiting for the pretend Home Assistant */
    bool code_shown; /* it arrived and revealed: both screens show 588460 (golden vector 1) */
    char client[48];
} s_ha;

static void be_halink_status(void *ctx, ui_halink_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->enabled = s_ha.enabled;
    out->paired = s_ha.paired;
    snprintf(out->client, sizeof(out->client), "%s", s_ha.client);
    snprintf(out->panel_id, sizeof(out->panel_id), "a1b2c3");
    out->port = 6054;
    if (!s_ha.enabled) {
        out->state = UI_HALINK_OFF;
    } else if (H.net != UI_NET_CONNECTED) {
        out->state = UI_HALINK_NO_NETWORK;
    } else if (s_ha.code_shown) {
        out->state = UI_HALINK_PAIRING_CODE;
        snprintf(out->code, sizeof(out->code), "588460");
        out->window_s = 90;
    } else if (s_ha.window) {
        out->state = UI_HALINK_PAIRING;
        out->window_s = 110;
    } else if (s_ha.paired) {
        out->state = UI_HALINK_CONNECTED;
    } else {
        out->state = UI_HALINK_READY;
    }
}

static void storage_reset(void);
static void ha_link_up(bool up);

static bool be_halink_enable(void *ctx, bool on)
{
    (void)ctx;
    s_ha.enabled = on;
    if (!on) {
        s_ha.window = false;
        s_ha.code_shown = false;
        if (s_ha.paired) {
            ha_link_up(false); /* bound zones lose their sensor until it is back on */
        }
    } else if (s_ha.paired) {
        ha_link_up(true);
    }
    return true;
}

static bool be_halink_pair_begin(void *ctx)
{
    (void)ctx;
    if (!s_ha.enabled) {
        return false;
    }
    s_ha.window = true;
    s_ha.code_shown = false;
    return true;
}

/* What the pretend Home Assistant shares once paired (its options, HAI-009): three sensors
 * the picker can find and bind. The preview's checks use these names. */
static void ha_share_demo_catalog(void)
{
    static const struct {
        const char *id, *name, *area, *cls, *state;
    } DEMO[] = {
        {"binary_sensor.porch_door", "Porch Door", "Porch", "door", "off"},
        {"binary_sensor.garage_motion", "Garage Motion", "Garage", "motion", "off"},
        {"binary_sensor.attic_window", "Attic Window", "Attic", "window", "off"},
    };
    zsrc_ha_catalog_begin(&H.ha_src);
    for (size_t i = 0; i < sizeof(DEMO) / sizeof(DEMO[0]); i++) {
        zsrc_ha_entity_t e;
        memset(&e, 0, sizeof(e));
        snprintf(e.id, sizeof(e.id), "%s", DEMO[i].id);
        snprintf(e.name, sizeof(e.name), "%s", DEMO[i].name);
        snprintf(e.area, sizeof(e.area), "%s", DEMO[i].area);
        snprintf(e.state, sizeof(e.state), "%s", DEMO[i].state);
        e.device_class = (uint8_t)zsrc_ha_device_class(DEMO[i].cls);
        zsrc_ha_catalog_add(&H.ha_src, &e);
    }
}

static void ha_link_up(bool up)
{
    zsrc_ha_link(&H.ha_src, up, H.now);
    if (up) {
        ha_share_demo_catalog();
    }
    after();
}

static bool host_panel_ha_paired_and_on(void)
{
    return s_ha.enabled && s_ha.paired;
}

static void be_halink_pair_decide(void *ctx, bool allow)
{
    (void)ctx;
    if (!s_ha.code_shown) {
        return;
    }
    s_ha.code_shown = false;
    s_ha.window = false;
    if (allow) {
        s_ha.paired = true;
        snprintf(s_ha.client, sizeof(s_ha.client), "aioroboalarms 0.1.0");
        ha_link_up(true); /* the integration connects and shares its entities */
    }
}

static void be_halink_pair_cancel(void *ctx)
{
    (void)ctx;
    s_ha.window = false;
    s_ha.code_shown = false;
}

static void be_halink_unpair(void *ctx)
{
    (void)ctx;
    s_ha.paired = false;
    s_ha.client[0] = '\0';
    ha_link_up(false);
}

void host_panel_ha_pair_request(void)
{
    if (s_ha.window) {
        s_ha.code_shown = true; /* the pretend Home Assistant committed and revealed */
    }
}

void host_panel_ha_entity(const char *entity_id, const char *state, bool available)
{
    zsrc_ha_state(&H.ha_src, entity_id, state, available, H.now);
    after();
}

/* ---- the pretend Zigbee sensors (docs/features/20) ---------------------------------------------- */

/* What the demo sensors are saying now, so tamper and a flat battery compose with open/closed
 * the way one real status word does. */
static uint16_t s_zb_status;

static void zb_join_one(uint64_t ieee, uint8_t ep, uint16_t zone_type, const char *manuf,
                        const char *model, bool router)
{
    zsrc_zb_desc_t d;
    memset(&d, 0, sizeof(d));
    d.ieee = ieee;
    d.nwk = (uint16_t)(ieee & 0xFFFFu);
    d.endpoint = ep;
    d.clusters = (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_POWER_CFG);
    d.zone_type = zone_type;
    d.router = router;
    snprintf(d.manufacturer, sizeof(d.manufacturer), "%s", manuf);
    snprintf(d.model, sizeof(d.model), "%s", model);
    zsrc_zb_device_seen(&H.zb, &d, H.now);
    /* A real sensor asks to enroll the moment it joins, and the panel answers it. */
    zsrc_zb_ias_enroll_request(&H.zb, ieee, ep, zone_type, H.now);
    zsrc_zb_ias_status(&H.zb, ieee, ep, 0u, H.now);
    zsrc_zb_battery(&H.zb, ieee, 92u, H.now);
}

/* The Zigbee demo backend's devices were given to the zone source (host_zigbee_demo_publish):
 * the pretend radio's own two sensors then stay out, as the walk shows the demo's. */
static bool s_zb_demo_published;

static void zb_join_demo_devices(void)
{
    if (!H.zb_present || s_zb_demo_published ||
        zsrc_zb_device(&H.zb, ZB_CONTACT_IEEE, 1u) != NULL) {
        return;
    }
    zb_join_one(ZB_CONTACT_IEEE, 1u, ZSRC_ZB_ZONE_TYPE_CONTACT, "Frient A/S", "WISZB-120", false);
    zb_join_one(ZB_MOTION_IEEE, 1u, ZSRC_ZB_ZONE_TYPE_MOTION, "Frient A/S", "MOSZB-140", false);
    s_zb_status = 0u;
}

/* ---- the Zigbee demo backend's hooks (host_zigbee_demo.h) ---- */

static uint16_t zb_zone_type_of(uint8_t device_class)
{
    switch ((alarm_device_class_t)device_class) {
    case ALARM_DEVCLASS_MOTION: return ZSRC_ZB_ZONE_TYPE_MOTION;
    case ALARM_DEVCLASS_SMOKE: return ZSRC_ZB_ZONE_TYPE_FIRE;
    case ALARM_DEVCLASS_CO: return ZSRC_ZB_ZONE_TYPE_CO;
    case ALARM_DEVCLASS_WATER: return ZSRC_ZB_ZONE_TYPE_WATER;
    default: return ZSRC_ZB_ZONE_TYPE_CONTACT;
    }
}

static void zb_demo_join(uint64_t ieee, uint8_t endpoint, uint8_t device_class,
                         const char *manufacturer, const char *model, bool router)
{
    s_zb_demo_published = true;
    if (!H.zb_present || zsrc_zb_device(&H.zb, ieee, endpoint) != NULL) {
        return;
    }
    zb_join_one(ieee, endpoint, zb_zone_type_of(device_class), manufacturer, model, router);
    after();
}

static void zb_demo_bind(uint16_t zone, zsrc_address_t addr, uint8_t device_class)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof b);
    b.in[ZSRC_ROLE_MAIN].addr = addr;
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, (alarm_device_class_t)device_class);
    (void)host_zone_bind(zone, &b);
    after();
}

/* ZB-017: the reset's installer code, checked by the engine (a wrong one counts toward the
 * keypad's lockout); any code that isn't the installer's is wrong. */
static host_zb_code_t zb_demo_code(const char *code)
{
    uint8_t user = 0;
    const alarm_result_t r = alarm_engine_authenticate(&H.eng, &SRC, code, 0, H.now, &user);
    after();
    if (r == ALARM_ERR_LOCKED_OUT) {
        return HOST_ZB_CODE_LOCKED;
    }
    return r == ALARM_OK && user == ALARM_USER_INSTALLER ? HOST_ZB_CODE_OK : HOST_ZB_CODE_WRONG;
}

static uint32_t zb_demo_now_s(void)
{
    return (uint32_t)(epoch_ms(NULL) / 1000);
}

static void zb_demo_hooks(void)
{
    s_zb_demo_published = false;
    const host_zb_hooks_t hooks = {.installer_code = zb_demo_code,
                                   .now_s = zb_demo_now_s,
                                   .join = zb_demo_join,
                                   .bind = zb_demo_bind};
    host_zigbee_demo_hooks(&hooks);
}

void host_panel_zigbee_pair(void)
{
    if (s_zb_real) {
        (void)host_zigbee_real_press_next();
        after();
        return;
    }
    zone_hub_learn(&H.hub, ZSRC_ZIGBEE, true, H.now); /* opens the window; they join into it */
    after();
    zone_hub_learn(&H.hub, ZSRC_ZIGBEE, false, H.now);
    after();
}

bool host_panel_zigbee_paired(void)
{
    if (s_zb_real) {
        return host_zigbee_real_id(HOST_ZBR_DOOR) != 0u;
    }
    return zsrc_zb_device(&H.zb, ZB_CONTACT_IEEE, 1u) != NULL;
}

void host_panel_zigbee_radio(bool present)
{
    if (s_zb_real) {
        host_zigbee_real_unplug(!present);
        after();
        return;
    }
    H.zb_present = present;
    zsrc_zb_link(&H.zb, present, H.now); /* unplugged is the sensor radio's own trouble */
    after();
}

void host_panel_zigbee_jam(bool on)
{
    /* Real simulated time passes; all timestamps remain on the same clock. */
    if (s_zb_real) {
        host_zigbee_real_noise(on);
        after();
        return;
    }
    H.zb_noise = on;
    zsrc_zb_energy(&H.zb, on ? -20 : -85, H.now);
    after();
}

static void zb_say_status(uint16_t bit, bool on)
{
    if (!H.zb_present || !host_panel_zigbee_paired() || H.zb_noise) {
        return;
    }
    s_zb_status = on ? (uint16_t)(s_zb_status | bit) : (uint16_t)(s_zb_status & (uint16_t)~bit);
    zsrc_zb_ias_status(&H.zb, ZB_CONTACT_IEEE, 1u, s_zb_status, H.now);
    after();
}

void host_panel_zigbee_open(bool open)
{
    if (s_zb_real) {
        (void)host_panel_zigbee_trip(HOST_ZBR_DOOR, open);
        return;
    }
    zb_say_status(ZSRC_ZB_IAS_ALARM1, open);
}

void host_panel_zigbee_tamper(bool on)
{
    if (s_zb_real) {
        (void)host_zigbee_real_tamper(HOST_ZBR_DOOR, on);
        after();
        return;
    }
    zb_say_status(ZSRC_ZB_IAS_TAMPER, on);
}

void host_panel_zigbee_battery(bool low)
{
    if (s_zb_real) {
        (void)host_zigbee_real_battery(HOST_ZBR_DOOR, low ? 8u : 92u);
        after();
        return;
    }
    if (!H.zb_present || !host_panel_zigbee_paired() || H.zb_noise) {
        return;
    }
    zsrc_zb_battery(&H.zb, ZB_CONTACT_IEEE, low ? 8u : 92u, H.now);
    after();
}

void host_panel_zigbee_leave(void)
{
    if (s_zb_real) {
        (void)host_zigbee_real_leave(HOST_ZBR_DOOR);
        after();
        return;
    }
    zsrc_zb_device_gone(&H.zb, ZB_CONTACT_IEEE, H.now);
    after();
}

/* ---- the real Zigbee (host_zigbee_real.h; BL-143 ZB-M6) ---- */

static bool zbr_allowed(void *ctx)
{
    (void)ctx;
    return zb_demo_allowed();
}

static ui_zb_req_result_t zbr_code(void *ctx, const char *code)
{
    (void)ctx;
    switch (zb_demo_code(code)) { /* the engine's check, a wrong code counted toward the lockout */
    case HOST_ZB_CODE_OK: return UI_ZB_REQ_OK;
    case HOST_ZB_CODE_LOCKED: return UI_ZB_REQ_LOCKED_OUT;
    default: return UI_ZB_REQ_WRONG_CODE;
    }
}

static uint32_t zbr_wall(void *ctx)
{
    (void)ctx;
    return zb_demo_now_s();
}

/* The device numbers the installer turned on in the engine's device table. */
static uint32_t zbr_taken(void *ctx)
{
    (void)ctx;
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    uint32_t taken = 0u;
    for (unsigned i = 0; cfg != NULL && i < ALARM_MAX_DEVICES; i++) {
        taken |= cfg->devices[i].enabled ? 1u << i : 0u;
    }
    return taken;
}

/* A keypad's command with its code, a fob's as its user: zigbee_service's ace_run. */
/* A fob's command needs its state, as zigbee_service's fob_when() asks alarm_service's
 * command_state_ok(): an arm only while the partition is disarmed and installer mode closed, a
 * disarm only while not disarmed (or an alarm sounds); anything else in any state. */
static bool zbr_fob_state_ok(const alarm_cmd_t *cmd)
{
    if (cmd->type != ALARM_CMD_ARM && cmd->type != ALARM_CMD_DISARM) {
        return true;
    }
    alarm_partition_status_t ps;
    alarm_engine_partition_status(&H.eng, cmd->partition, &ps);
    const bool disarmed = ps.state == ALARM_STATE_DISARMED;
    if (cmd->type == ALARM_CMD_ARM) {
        return disarmed && !alarm_engine_program_active(&H.eng);
    }
    const bool sounding = ps.sounder == ALARM_SOUND_ALARM || ps.sounder == ALARM_SOUND_FIRE ||
                          ps.sounder == ALARM_SOUND_CO || ps.sounder == ALARM_SOUND_AUX;
    return !disarmed || sounding;
}

static alarm_result_t zbr_ace_run(void *ctx, const zba_post_t *post, alarm_cmd_result_t *detail)
{
    (void)ctx;
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    if (post->user != 0u &&
        (cfg == NULL || post->user > ALARM_MAX_USERS ||
         !zba_fob_may_be((alarm_authority_t)cfg->users[post->user - 1u].authority))) {
        if (detail != NULL) {
            memset(detail, 0, sizeof(*detail));
        }
        return ALARM_ERR_NOT_AUTHORIZED; /* its user is no longer one a fob may be (fob_user_ok) */
    }
    if (post->user != 0u && !zbr_fob_state_ok(&post->cmd)) {
        if (detail != NULL) {
            memset(detail, 0, sizeof(*detail));
        }
        return ALARM_ERR_BAD_STATE;
    }
    const alarm_result_t r =
        post->user != 0u
            ? alarm_engine_command_as(&H.eng, &post->cmd, &post->source, post->user, H.now, detail)
            : alarm_engine_command(&H.eng, &post->cmd, &post->source, H.now, detail);
    H.dirty = true;
    return r;
}

static void zbr_bell(void *ctx, bool active)
{
    (void)ctx;
    alarm_engine_system_trouble(&H.eng, ALARM_TRBL_BELL, active, H.now);
    H.dirty = true;
}

/* zbz_event_t 1..9 are ALARM_EV_ZIGBEE_ADDED.. in the same order (zigbee_service's log_events). */
static void zbr_log(void *ctx, const zbz_log_t *e)
{
    (void)ctx;
    if (e->type == 0u || e->type > (uint8_t)ZBZ_EV_RADIO_RESTARTED) {
        return;
    }
    const alarm_event_type_t type =
        (alarm_event_type_t)((int)ALARM_EV_ZIGBEE_ADDED + (int)e->type - (int)ZBZ_EV_ADDED);
    const alarm_source_t src =
        e->user == 0u ? (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0}
        : e->via == UI_ZB_VIA_WEB
            ? (alarm_source_t){.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_PANEL_MANAGER}
            : (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
    const uint8_t arg = e->device <= UINT8_MAX ? (uint8_t)e->device : 0u;
    (void)alarm_engine_platform_event_ex(&H.eng, type, e->zone, arg, e->arg, &src, e->user, H.now);
    H.dirty = true;
}

static zone_hub_result_t zbr_bind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    return host_zone_bind(zone, b);
}

static void zb_real_start(const zsrc_sink_t *sink)
{
    const host_zbr_port_t port = {.ctx = NULL,
                                  .allowed = zbr_allowed,
                                  .installer_code = zbr_code,
                                  .wall_clock = zbr_wall,
                                  .devices_taken = zbr_taken,
                                  .ace_run = zbr_ace_run,
                                  .bell_trouble = zbr_bell,
                                  .log = zbr_log,
                                  .bind = zbr_bind};
    host_zigbee_real_init(&H.hub, sink, &port, H.now);
}

/* The cause a keypad shows for an alarm: the most urgent in the partition's memory
 * (zigbee_service's cause_of). */
static uint8_t zb_cause_of(uint16_t memory)
{
    static const uint8_t order[] = {ALARM_CAUSE_FIRE,         ALARM_CAUSE_CO,
                                    ALARM_CAUSE_PANIC_FIRE,   ALARM_CAUSE_PANIC_MEDICAL,
                                    ALARM_CAUSE_PANIC_POLICE, ALARM_CAUSE_PANIC_SILENT};
    for (size_t i = 0; i < sizeof(order); i++) {
        if ((memory & (1u << order[i])) != 0u) {
            return order[i];
        }
    }
    for (unsigned c = 1u; c < (unsigned)ALARM_CAUSE_COUNT; c++) {
        if ((memory & (1u << c)) != 0u) {
            return (uint8_t)c;
        }
    }
    return (uint8_t)ALARM_CAUSE_NONE;
}

/* The walk test and its scope, as zigbee_service's take_engine_news and refresh_walk_scope tell
 * zbz_panel (ZB-022): when it starts or ends, when the configuration changes and every 30 s while
 * it runs (the zones' bindings). */
static void zb_real_walk(uint64_t now)
{
    alarm_snapshot_t snap;
    alarm_engine_snapshot(&H.eng, &snap);
    bool walk = false;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        walk = walk || ((snap.partitions & (1u << p)) != 0u && snap.part[p].walk_test);
    }
    if (walk == H.zb_walk_on &&
        (!walk || (H.config_seq == H.zb_walk_seq && now - H.zb_walk_at < 30000u))) {
        return;
    }
    uint8_t covered[ALARM_ZONE_BITMAP_BYTES];
    memset(covered, 0, sizeof(covered));
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    if (walk && cfg != NULL) {
        zbz_walk_covered(cfg, &snap, covered);
    }
    H.zb_walk_on = walk;
    H.zb_walk_seq = H.config_seq;
    H.zb_walk_at = now;
    host_zigbee_real_walk(walk, covered);
}

/* The engine's news for the sirens and keypads, as zigbee_service takes it from the alarm task:
 * the bell and strobe patterns, each partition's view when it changed. */
static void zb_real_news(void)
{
    const uint64_t now = H.now;
    for (uint8_t p = 0; p <= ALARM_MAX_PARTITIONS; p++) {
        if ((H.zb_out_dirty & (1u << p)) != 0u) {
            host_zigbee_real_output(p, ALARM_OUT_BELL, (alarm_pattern_t)H.zb_out[p][0]);
            host_zigbee_real_output(p, ALARM_OUT_STROBE, (alarm_pattern_t)H.zb_out[p][1]);
        }
    }
    H.zb_out_dirty = 0u;
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        alarm_partition_status_t st;
        alarm_engine_partition_status(&H.eng, p, &st);
        zba_view_t v;
        memset(&v, 0, sizeof(v));
        v.state = (uint8_t)st.state;
        v.level = (uint8_t)st.level;
        v.ready = st.ready;
        v.delay_s = (uint16_t)((st.delay_remaining_ms + 999u) / 1000u);
        v.at = now;
        v.in_alarm = st.bell != ALARM_PAT_OFF || (st.triggered && st.state != ALARM_STATE_DISARMED);
        v.cause = v.in_alarm ? zb_cause_of((uint16_t)(st.memory & ~ALARM_MEM_CANCELED))
                             : (uint8_t)ALARM_CAUSE_NONE;
        zba_view_t *told = &H.zb_view[p - 1u];
        if (!H.zb_view_told[p - 1u] || told->state != v.state || told->level != v.level ||
            told->ready != v.ready || told->delay_s != v.delay_s ||
            told->in_alarm != v.in_alarm || told->cause != v.cause) {
            *told = v;
            H.zb_view_told[p - 1u] = true;
            host_zigbee_real_partition(p, &v);
        }
    }
    zb_real_walk(now);
    host_zigbee_real_run(now);
}

void host_panel_zigbee_use_real(bool on)
{
    s_zb_real = on;
}

bool host_panel_zigbee_is_real(void)
{
    return s_zb_real;
}

const char *host_panel_zigbee_press(void)
{
    static const char *const NAMES[HOST_ZBR_COUNT] = {
        "door contact", "motion sensor", "smoke alarm", "water sensor",
        "siren",        "keypad",        "key fob",
    };
    if (!s_zb_real) {
        return NULL;
    }
    const host_zbr_dev_t d = host_zigbee_real_press_next();
    after();
    return d != HOST_ZBR_COUNT ? NAMES[d] : NULL;
}

bool host_panel_zigbee_trip(uint8_t which, bool on)
{
    if (!s_zb_real) {
        return false;
    }
    const bool ok = host_zigbee_real_trip((host_zbr_dev_t)which, on);
    after();
    return ok;
}

void host_panel_zigbee_restart(void)
{
    if (s_zb_real) {
        host_zigbee_real_restart(H.now);
        after();
    }
}

/* ---- the pretend CC1101 and its 433 MHz contact (docs/features/25) ---- */

/* One transmission, as the real receiver hands it over: `frames` repeats of the frame. The
 * demo contact sends 5 when it opens, E when it closes, A for its cover and 3 for a flat
 * battery. */
static void rf_frames(uint32_t device, uint8_t data, unsigned frames)
{
    if (!H.rf_present || H.rf_noise) {
        return; /* nothing gets through a jammed band, and nothing without a receiver */
    }
    const zsrc_rf_packet_t p = zsrc_rf_ev1527((device << 4) | data);
    for (unsigned i = 0; i < frames; i++) {
        zsrc_rf_packet(&H.rf, &p, H.now + i * 40u);
    }
    after();
}

static void rf_send(uint8_t data)
{
    rf_frames(HOST_RF_DEMO_DEVICE, data, 4u);
}

void host_panel_rf_neighbour(void)
{
    rf_frames(HOST_RF_NEIGHBOUR_DEVICE, 0x1u, 4u);
}

void host_panel_rf_glitch(void)
{
    rf_frames(HOST_RF_DEMO_DEVICE ^ 0x00400u, 0x5u, 1u); /* one bit flipped, heard once */
}

void host_panel_rf_receiver(bool present)
{
    H.rf_present = present;
    zsrc_rf_receiver(&H.rf, present, H.now);
    after();
}

void host_panel_rf_open(bool open)
{
    rf_send(open ? 0x5u : 0xEu);
}

void host_panel_rf_tamper(void)
{
    rf_send(0xAu);
}

void host_panel_rf_battery(void)
{
    rf_send(0x3u);
}

void host_panel_rf_jam(bool on)
{
    H.rf_noise = on;
    zsrc_rf_noise(&H.rf, on ? -40 : -95, H.now); /* a carrier that stays trips it in 30 s */
    after();
}

/* ---- firmware updates (a scripted release server) ---- */

static ui_fw_status_t s_fw = {
    .phase = UI_FW_IDLE, .running = "0.6.0", .has_source = true, .percent = -1,
};

void host_panel_fw_preset(const ui_fw_status_t *st)
{
    s_fw = *st;
}

static void be_fw_status(void *ctx, ui_fw_status_t *out)
{
    (void)ctx;
    *out = s_fw;
    out->has_source = true;
}

static void be_fw_check(void *ctx)
{
    (void)ctx;
    snprintf(s_fw.latest, sizeof(s_fw.latest), "0.6.1");
    s_fw.available = true;
    s_fw.checked = (uint32_t)(epoch_ms(NULL) / 1000); /* 0 while the world's clock isn't set */
}

static bool be_fw_install(void *ctx, uint8_t user)
{
    (void)ctx;
    (void)user;
    s_fw.phase = UI_FW_DOWNLOADING;
    s_fw.stage = UI_FW_STAGE_DOWNLOADING; /* a run: the preview's presets move it on */
    s_fw.release = true;
    snprintf(s_fw.target, sizeof(s_fw.target), "%s", s_fw.latest[0] != '\0' ? s_fw.latest : "0.6.1");
    snprintf(s_fw.previous, sizeof(s_fw.previous), "%s", s_fw.running);
    s_fw.bytes = 1153434u; /* 1.1 of 2.7 MB */
    s_fw.size = 2831155u;
    s_fw.percent = 42;
    s_fw.last.result = UI_FW_LAST_NONE;
    return true;
}

/* BL-188: Stop. The run is "stopping" until a preset ends it (a real panel takes seconds). */
static uint8_t be_fw_stop(void *ctx, uint8_t user)
{
    (void)ctx;
    (void)user;
    if (s_fw.stage == UI_FW_STAGE_NONE || s_fw.stage == UI_FW_STAGE_ROLLING_BACK) {
        return UI_FW_STOP_NOTHING;
    }
    if (!s_fw.release) {
        return UI_FW_STOP_NOT_STOPPABLE;
    }
    s_fw.stage = UI_FW_STAGE_STOPPING;
    return UI_FW_STOP_OK;
}

/* ---- the update server (BL-186): RoboAlarms' own, and fictional ones that answer after a moment ---- */

typedef struct {
    char domain[96];
    bool official;
    char key_name[48];
    uint8_t sha[32];
    char channel[8];
} sim_srv_t;

static const uint8_t SIM_KEY_ROBO[8] = {0x3F, 0x9A, 0x1C, 0x2B, 0x77, 0xD0, 0xE4, 0x15};
static const uint8_t SIM_KEY_EXAMPLE[8] = {0x8C, 0x41, 0x0E, 0x9D, 0x52, 0xA7, 0xB3, 0xF6};
#define SIM_CHECK_MS 2000u

static sim_srv_t s_srv = {.domain = "roboalarms.com", .official = true,
                          .key_name = "RoboAlarms release", .sha = {0x3F, 0x9A, 0x1C, 0x2B, 0x77, 0xD0, 0xE4, 0x15},
                          .channel = "stable"};
static ui_fw_srv_check_t s_chk;         /* the check being run or finished */
static alarm_ms_t s_chk_due;
static bool s_robo_trusted = true;      /* RoboAlarms' key is trusted (false: a version signed by another project) */

void host_panel_fw_server_preset(const char *domain, bool official, bool robo_trusted)
{
    memset(&s_srv, 0, sizeof(s_srv));
    snprintf(s_srv.domain, sizeof(s_srv.domain), "%s", domain);
    s_srv.official = official;
    snprintf(s_srv.channel, sizeof(s_srv.channel), "%s", "stable");
    const uint8_t *k = official ? SIM_KEY_ROBO : SIM_KEY_EXAMPLE;
    memcpy(s_srv.sha, k, 8);
    snprintf(s_srv.key_name, sizeof(s_srv.key_name), "%s", official ? "RoboAlarms release" : "Example Builds");
    s_robo_trusted = robo_trusted;
    memset(&s_chk, 0, sizeof(s_chk));
}

static void sim_fingerprint(const uint8_t sha[32], char out[20])
{
    snprintf(out, 20, "%02X%02X %02X%02X %02X%02X %02X%02X", sha[0], sha[1], sha[2], sha[3], sha[4], sha[5],
             sha[6], sha[7]);
}

static void be_fw_server(void *ctx, ui_fw_server_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    snprintf(out->domain, sizeof(out->domain), "%s", s_srv.domain);
    out->official = s_srv.official;
    snprintf(out->key_name, sizeof(out->key_name), "%s", s_srv.key_name);
    sim_fingerprint(s_srv.sha, out->fingerprint);
    snprintf(out->channel, sizeof(out->channel), "%s", s_srv.channel);
}

static bool be_fw_server_check(void *ctx, const char *typed)
{
    (void)ctx;
    memset(&s_chk, 0, sizeof(s_chk));
    char d[96];
    snprintf(d, sizeof(d), "%s", typed);
    if (strncmp(d, "https://", 8) == 0) {
        memmove(d, d + 8, strlen(d + 8) + 1);
    }
    size_t n = strlen(d);
    while (n > 0u && d[n - 1u] == '/') {
        d[--n] = '\0';
    }
    bool dot = false, ok = n > 2u && d[0] != '.' && d[n - 1u] != '.';
    for (size_t i = 0; i < n && ok; i++) {
        const char c = d[i];
        dot = dot || c == '.';
        ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    }
    if (!ok || !dot) {
        s_chk.result = UI_FW_SRV_BAD_DOMAIN;
        return false;
    }
    if (s_fw.stage != UI_FW_STAGE_NONE || s_fw.confirming) {
        s_chk.result = UI_FW_SRV_NONE;
        return false;
    }
    snprintf(s_chk.domain, sizeof(s_chk.domain), "%s", d);
    s_chk.result = UI_FW_SRV_CHECKING;
    s_chk_due = H.now + SIM_CHECK_MS;
    return true;
}

static void be_fw_server_check_get(void *ctx, ui_fw_srv_check_t *out)
{
    (void)ctx;
    if (s_chk.result == UI_FW_SRV_CHECKING && H.now >= s_chk_due) {
        const char *d = s_chk.domain;
        if (strcmp(d, "example.net") == 0 || strcmp(d, "roboalarms.com") == 0) {
            const bool robo = strcmp(d, "roboalarms.com") == 0;
            s_chk.result = UI_FW_SRV_OK;
            snprintf(s_chk.key_name, sizeof(s_chk.key_name), "%s", robo ? "RoboAlarms release" : "Example Builds");
            memset(s_chk.key_sha256, 0, sizeof(s_chk.key_sha256));
            memcpy(s_chk.key_sha256, robo ? SIM_KEY_ROBO : SIM_KEY_EXAMPLE, 8);
            sim_fingerprint(s_chk.key_sha256, s_chk.fingerprint);
            s_chk.trusted = robo ? s_robo_trusted : false;
        } else if (strcmp(d, "example.org") == 0) {
            s_chk.result = UI_FW_SRV_NOT_A_SERVER;
        } else if (strcmp(d, "example.com") == 0) {
            s_chk.result = UI_FW_SRV_NO_FIRMWARE;
        } else {
            s_chk.result = UI_FW_SRV_UNREACHABLE;
        }
    }
    *out = s_chk;
}

static void be_fw_server_check_forget(void *ctx)
{
    (void)ctx;
    memset(&s_chk, 0, sizeof(s_chk));
}

static uint8_t be_fw_server_commit(void *ctx, const char *domain, const uint8_t key_sha256[32],
                                   uint8_t user)
{
    (void)ctx;
    (void)user;
    if (s_fw.stage != UI_FW_STAGE_NONE || s_fw.confirming) {
        return UI_FW_COMMIT_RUNNING;
    }
    if (s_chk.result != UI_FW_SRV_OK || domain == NULL || strcmp(domain, s_chk.domain) != 0 ||
        memcmp(key_sha256, s_chk.key_sha256, 32) != 0) {
        return UI_FW_COMMIT_STALE;
    }
    if (!s_chk.trusted && (alarm_engine_default_codes(&H.eng) & ALARM_DEFAULT_CODE_INSTALLER) != 0u) {
        return UI_FW_COMMIT_FACTORY_CODE;
    }
    const bool robo = strcmp(s_chk.domain, "roboalarms.com") == 0;
    snprintf(s_srv.domain, sizeof(s_srv.domain), "%s", s_chk.domain);
    s_srv.official = robo;
    snprintf(s_srv.key_name, sizeof(s_srv.key_name), "%s", s_chk.key_name);
    memcpy(s_srv.sha, s_chk.key_sha256, sizeof(s_srv.sha));
    if (robo) {
        s_robo_trusted = true;
    }
    memset(&s_chk, 0, sizeof(s_chk));
    be_fw_check(NULL);
    return UI_FW_COMMIT_OK;
}

static bool be_fw_set_channel(void *ctx, const char *channel, uint8_t user)
{
    (void)ctx;
    (void)user;
    if (s_fw.stage != UI_FW_STAGE_NONE || s_fw.confirming) {
        return false;
    }
    snprintf(s_srv.channel, sizeof(s_srv.channel), "%s", channel);
    be_fw_check(NULL);
    return true;
}

static size_t be_zone_sources(void *ctx, ui_zone_source_t *out, size_t max)
{
    (void)ctx;
    zone_hub_source_info_t list[ZONE_HUB_MAX_SOURCES];
    const size_t n = zone_hub_sources(&H.hub, list, ZONE_HUB_MAX_SOURCES);
    size_t k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        out[k].kind = list[i].kind;
        out[k].caps = list[i].caps;
        out[k].usable = list[i].usable;
        k++;
    }
    return k;
}

static void be_zone_learn(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    zone_hub_learn(&H.hub, kind, on, H.now);
    after();
}

static size_t be_zone_candidates(void *ctx, uint8_t kind, const char *query, zsrc_candidate_t *out,
                                 size_t max)
{
    (void)ctx;
    return zone_hub_candidates(&H.hub, kind, query, 0, out, max);
}

static bool all_disarmed(void)
{
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        alarm_partition_status_t ps;
        alarm_engine_partition_status(&H.eng, p, &ps);
        if (ps.state != ALARM_STATE_DISARMED) {
            return false;
        }
    }
    return true;
}

static zone_hub_result_t be_zone_bind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    if (!all_disarmed()) {
        return ZONE_HUB_ERR_BUSY;
    }
    const zone_hub_result_t r = host_zone_bind(zone, b);
    after();
    return r;
}

static zone_hub_result_t be_zone_unbind(void *ctx, uint16_t zone)
{
    (void)ctx;
    if (!all_disarmed()) {
        return ZONE_HUB_ERR_BUSY;
    }
    host_zone_unbind(zone);
    after();
    return ZONE_HUB_OK;
}

static bool be_zone_info(void *ctx, uint16_t zone, zone_hub_info_t *out)
{
    (void)ctx;
    return zone_hub_info(&H.hub, zone, out);
}

static ui_power_info_t s_power = {.percent = -1};
void host_panel_set_charging(bool charging)
{
    s_power.charging = charging;
}
void host_panel_set_power(ui_battery_state_t state, int percent, bool low)
{
    s_power = (ui_power_info_t){.state = state, .percent = percent, .low = low};
}
static void be_power(void *ctx, ui_power_info_t *out)
{
    (void)ctx;
    *out = s_power;
}

static bool s_remote_web, s_remote_developer;
static bool s_remote_paused; /* the remote keypad paused by the person at the panel (the banner) */
static void be_remote_get(void *ctx, bool *web, bool *developer)
{
    (void)ctx;
    *web = s_remote_web;
    *developer = s_remote_developer;
}
static bool be_remote_set(void *ctx, bool web, bool developer)
{
    (void)ctx;
    if (developer && !web)
        return false;
    s_remote_web = web;
    s_remote_developer = developer;
    if (!web)
        s_remote_paused = false; /* as the panel: remote access off forgets the pause */
    return true;
}
static void be_remote_pause(void *ctx, bool paused)
{
    (void)ctx;
    s_remote_paused = paused;
}
bool host_panel_remote_paused(void)
{
    return s_remote_paused;
}
bool host_panel_remote_web(void)
{
    return s_remote_web;
}
static void be_remote_info(void *ctx, char *out, size_t capacity)
{
    (void)ctx;
    snprintf(out, capacity, "https://192.0.2.10/\nCertificate SHA256 (preview):\n0123456789abcdef\n0123456789abcdef\n0123456789abcdef\n0123456789abcdef");
}
/* Panel Manager sign-in: which users have one (the panel keeps salted hashes, web_login.c),
 * and whether it was set by somebody else (changed at the first sign-in). */
static bool s_web_login[ALARM_MAX_USERS];
static bool s_web_login_temporary[ALARM_MAX_USERS];
static bool be_web_login_get(void *ctx, uint8_t user)
{
    (void)ctx;
    return user >= 1u && user <= ALARM_MAX_USERS && s_web_login[user - 1u];
}
static bool be_web_login_set(void *ctx, uint8_t user, const char *password, bool temporary,
                             char *why, size_t why_len)
{
    (void)ctx;
    if (user < 1u || user > ALARM_MAX_USERS) {
        snprintf(why, why_len, "The panel is busy. Try again");
        return false;
    }
    if (strlen(password) < 12u) {
        snprintf(why, why_len, "Use at least 12 characters");
        return false;
    }
    s_web_login[user - 1u] = true;
    s_web_login_temporary[user - 1u] = temporary;
    return true;
}
static void be_web_login_clear(void *ctx, uint8_t user)
{
    (void)ctx;
    if (user >= 1u && user <= ALARM_MAX_USERS) {
        s_web_login[user - 1u] = false;
        s_web_login_temporary[user - 1u] = false;
    }
}
bool host_panel_web_login_temporary(uint8_t user)
{
    return user >= 1u && user <= ALARM_MAX_USERS && s_web_login[user - 1u] &&
           s_web_login_temporary[user - 1u];
}
/* ---- languages (ui_backend_t lang_*; docs/features/37 language-packs.md) ----------------------
 * Without the pack world the backend answers with the catalogs built into this PC build, as if
 * they were installed, ready and complete - the screens look as they do without a backend set.
 * host_panel_lang_world() pretends a panel with packs: es-US ready but three phrases short and a
 * draft, fr-CA made for 0.6.0, de-DE refused; the language it-IT, which the walk stores, isn't
 * installed. host_panel_lang_greek() adds a fifth, el-GR, ready, without words (its catalog is
 * empty: every phrase stays English) but with a font: a Greek body font, fixtures/font_greek_body.bin
 * (I18N-049; fixtures/README.md says how it was made). The test languages (en-XA, ar-XB) bring a
 * headline font, fixtures/font_test_headline.bin: the image's headline holds only the shipped
 * catalogs' letters and the letters pack has no headline part (docs/features/39 asset-packs.md),
 * so their made-up accents would draw as boxes, wider than letters, and the walk would measure
 * those instead of the words. */
static bool s_lang_world;
static bool s_lang_greek;
static uint32_t s_lang_gen = 1u;
static void *s_greek_font;
static size_t s_greek_font_size;
static void *s_test_headline;
static size_t s_test_headline_size;

void host_panel_lang_greek(bool on)
{
    s_lang_greek = on;
    s_lang_gen++;
}

void host_panel_zone_name(uint16_t zone, const char *name)
{
    /* The engine owns its copy of the configuration; the preview's world is its own. */
    alarm_config_t *cfg = (alarm_config_t *)(uintptr_t)alarm_engine_config(&H.eng);
    if (zone >= 1u && zone <= ALARM_MAX_ZONES) {
        snprintf(cfg->zones[zone - 1u].name, ALARM_NAME_LEN, "%s", name);
        H.config_seq++;
        H.dirty = true;
    }
}

/* A fixture's bytes, read from next to this source file into buf; 0 when it can't be read or
 * doesn't fit. */
static size_t fixture_read(const char *name, uint8_t *buf, size_t cap)
{
    char path[512];
    const char *src = __FILE__;
    const char *slash = strrchr(src, '/');
    const char *back = strrchr(src, '\\');
    if (back != NULL && (slash == NULL || back > slash)) {
        slash = back;
    }
    const size_t dir = slash != NULL ? (size_t)(slash - src) + 1u : 0u;
    if (dir + 48u >= sizeof(path)) {
        return 0u;
    }
    memcpy(path, src, dir);
    snprintf(path + dir, sizeof(path) - dir, "fixtures/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0u;
    }
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n == cap ? 0u : n;
}

/* The fixtures' bytes, read once. */
static bool greek_font_load(void)
{
    static uint8_t buf[16384];
    if (s_greek_font == NULL && (s_greek_font_size = fixture_read("font_greek_body.bin", buf, sizeof(buf))) > 0u) {
        s_greek_font = buf;
    }
    return s_greek_font != NULL;
}

static bool test_headline_load(void)
{
    static uint8_t buf[131072];
    if (s_test_headline == NULL &&
        (s_test_headline_size = fixture_read("font_test_headline.bin", buf, sizeof(buf))) > 0u) {
        s_test_headline = buf;
    }
    return s_test_headline != NULL;
}

void host_panel_lang_world(bool on)
{
    s_lang_world = on;
    s_lang_gen++;
}

static const i18n_catalog_t *be_lang_catalog(void *ctx, const char *locale)
{
    (void)ctx;
    if (s_lang_greek && strcmp(locale, "el-GR") == 0) {
        static const i18n_entry_t none[1] = {{0xFFFFFFFFu, 0u}};
        static const i18n_catalog_t greek = {"el-GR", 0u, 0u, 1u, none, "\0"};
        return &greek;
    }
    if (s_lang_world && strcmp(locale, "es-US") != 0) {
        return NULL; /* the others: not loaded, or not there */
    }
    return i18n_catalog(locale);
}

static void lang_fill(ui_lang_t *l, const char *locale, const char *name, const char *version,
                      bool draft, ui_lang_state_t state, uint32_t missing)
{
    memset(l, 0, sizeof(*l));
    snprintf(l->locale, sizeof(l->locale), "%s", locale);
    snprintf(l->name, sizeof(l->name), "%s", name);
    snprintf(l->version, sizeof(l->version), "%s", version);
    l->draft = draft;
    l->state = (uint8_t)state;
    l->missing = missing;
}

static size_t be_lang_list(void *ctx, ui_lang_t *out, size_t max)
{
    (void)ctx;
    size_t n = 0;
    if (s_lang_world) {
        if (n < max) {
            lang_fill(&out[n++], "es-US", "Espa\xC3\xB1ol", "0.7.0", true, UI_LANG_READY, 3u);
        }
        if (n < max) {
            lang_fill(&out[n++], "fr-CA", "Fran\xC3\xA7" "ais (Canada)", "0.6.0", false,
                      UI_LANG_OTHER, 0u);
        }
        if (n < max) {
            lang_fill(&out[n++], "de-DE", "Deutsch", "0.7.0", false, UI_LANG_REFUSED, 0u);
        }
        if (s_lang_greek && n < max) {
            lang_fill(&out[n++], "el-GR", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC",
                      "0.7.0", false, UI_LANG_READY, 0u);
        }
        return n;
    }
    for (size_t i = 0; i < i18n_catalog_count() && n < max; i++) {
        const i18n_catalog_t *c = i18n_catalog_at(i);
        lang_fill(&out[n++], c->locale, i18n_language_name(c), "", c->preview, UI_LANG_READY, 0u);
    }
    return n;
}

/* The firmware gets this from the picture pack (the build makes it from the same JPEG). */
#include "../../components/ui/alarm_ui/images/saver_forest_jpg.c"

static bool be_asset_picture(void *ctx, const char *name, const void **data, size_t *size)
{
    (void)ctx;
    if (strcmp(name, "photo") != 0) {
        return false;
    }
    *data = ui_img_saver_forest_jpg;
    *size = ui_img_saver_forest_jpg_size;
    return true;
}

static bool be_lang_font(void *ctx, const char *locale, const char *role, const void **data,
                         size_t *size)
{
    (void)ctx;
    /* A test language's headline font (see "languages" above). */
    const size_t len = strlen(locale);
    if (strcmp(role, "headline") == 0 && len == 5u &&
        (strcmp(locale + 2, "-XA") == 0 || strcmp(locale + 2, "-XB") == 0)) {
        if (!test_headline_load()) {
            return false;
        }
        *data = s_test_headline;
        *size = s_test_headline_size;
        return true;
    }
    /* The Greek test font, for the body and for the label font a card's title is set in (a pixel
     * smaller than the label: a test, not a picture of a real pack). */
    if (!s_lang_greek || strcmp(locale, "el-GR") != 0 ||
        (strcmp(role, "body") != 0 && strcmp(role, "label") != 0) || !greek_font_load()) {
        return false;
    }
    *data = s_greek_font;
    *size = s_greek_font_size;
    return true;
}

/* ---- the letters pack (docs/features/39 asset-packs.md "The panel", OPT-010) ---------------------
 * What a panel with the letters pack loaded hands the screen: the layout's committed parts,
 * fonts/letters/font_<role>.bin (ALARM_LETTERS_DIR, from the build), read once and kept. A layout
 * without a letters folder (480x320 keeps its full fonts) answers none. host_panel_letters(false)
 * takes the pack away at run time, as on a panel without it; either way the generation changes. */
#ifndef ALARM_LETTERS_DIR
#define ALARM_LETTERS_DIR ""
#endif
static bool s_letters_off;
static uint32_t s_asset_gen = 1u;
static struct {
    void *data;
    size_t size;
    bool tried;
} s_letters[4];

void host_panel_letters(bool on)
{
    s_letters_off = !on;
    s_asset_gen++;
}

static bool be_asset_font(void *ctx, const char *role, const void **data, size_t *size)
{
    (void)ctx;
    static const char *const roles[] = {"caption", "body", "label", "title"};
    if (s_letters_off || ALARM_LETTERS_DIR[0] == '\0' || role == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        if (strcmp(role, roles[i]) != 0) {
            continue;
        }
        if (!s_letters[i].tried) {
            s_letters[i].tried = true;
            char path[1024];
            snprintf(path, sizeof(path), "%s/font_%s.bin", ALARM_LETTERS_DIR, role);
            FILE *f = fopen(path, "rb");
            if (f == NULL) {
                return false; /* no letters for this layout */
            }
            long n = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                n = ftell(f);
            }
            void *buf = n > 0 && n <= 256L * 1024L ? malloc((size_t)n) : NULL;
            if (buf != NULL && fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                s_letters[i].data = buf;
                s_letters[i].size = (size_t)n;
            } else {
                free(buf);
                fprintf(stderr, "cannot read %s\n", path);
            }
            fclose(f);
        }
        *data = s_letters[i].data;
        *size = s_letters[i].size;
        return s_letters[i].data != NULL;
    }
    return false;
}

static uint32_t be_asset_generation(void *ctx)
{
    (void)ctx;
    return s_asset_gen;
}

/* ---- Smart Home (docs/features/24, BL-045): a fictional house's devices ------------------------
 * What the panel's device_service would list, with what its source can't tell today (brightness,
 * set point, a garage door's class) filled in. Actions apply at once; Patio Lights' last action
 * failed until it is tried again; Pool Pump is offline. An unlock or an opening takes a code the
 * engine accepts (HCTL-005), as on the panel (ui_port_esp.c). The layout is stored as arranged:
 * Favorites as mockup-next's n-devices draws it. */
#define DEV_WORLD_MAX 24
static struct {
    ui_dev_t dev[DEV_WORLD_MAX];
    size_t n;
    uint8_t layout[UI_DEVL_BLOB_MAX];
    size_t layout_len;
    bool none; /* host_panel_devices(false): a panel without any */
    size_t extra; /* host_panel_devices_extra(true): how many were added at the end */
} s_dev;

static ui_dev_t *dev_add(const char *ref, const char *name, const char *area, uint8_t kind,
                         uint8_t state, int level, uint8_t caps)
{
    ui_dev_t *d = &s_dev.dev[s_dev.n++];
    memset(d, 0, sizeof(*d));
    snprintf(d->ref, sizeof(d->ref), "%s", ref);
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->area, sizeof(d->area), "%s", area);
    d->kind = kind;
    d->source = DSRC_HA; /* the fictional house is Home Assistant's */
    d->state = state;
    d->level = (int8_t)level;
    d->caps = caps;
    d->setpoint = UI_DEV_TEMP_UNKNOWN;
    d->temperature = UI_DEV_TEMP_UNKNOWN;
    return d;
}

static void devices_reset(int64_t epoch_at_start)
{
    memset(&s_dev, 0, sizeof(s_dev));
    const char *LIVING = "Living room", *KITCHEN = "Kitchen", *OUTSIDE = "Outside";
    dev_add("light.porch", "Porch Light", OUTSIDE, DEV_LIGHT, UI_DEV_ST_ON, 80, UI_DEV_CAP_LEVEL);
    dev_add("lock.front_door", "Front Door", LIVING, DEV_LOCK, UI_DEV_ST_LOCKED, -1, 0);
    dev_add("lock.back_door", "Back Door", KITCHEN, DEV_LOCK, UI_DEV_ST_UNLOCKED, -1, 0);
    dev_add("cover.garage_door", "Garage Door", OUTSIDE, DEV_COVER, UI_DEV_ST_CLOSED, 0,
            UI_DEV_CAP_ENTRANCE);
    dev_add("lock.side_gate", "Side Gate", OUTSIDE, DEV_LOCK, UI_DEV_ST_JAMMED, -1, 0);
    ui_dev_t *patio = dev_add("light.patio", "Patio Lights", OUTSIDE, DEV_LIGHT, UI_DEV_ST_OFF, -1, 0);
    patio->status = UI_DEV_FAILED;
    patio->action = ACT_ON;
    ui_dev_t *pump = dev_add("switch.pool_pump", "Pool Pump", OUTSIDE, DEV_SWITCH, UI_DEV_ST_OFF, -1, 0);
    pump->status = UI_DEV_OFFLINE;
    pump->since_ms = epoch_at_start - 89LL * 60000LL; /* 9:12 PM */
    dev_add("cover.patio_awning", "Patio Awning", OUTSIDE, DEV_COVER, UI_DEV_ST_OPEN, 100,
            UI_DEV_CAP_LEVEL);
    dev_add("fan.porch", "Porch Fan", OUTSIDE, DEV_FAN, UI_DEV_ST_ON, 33, 0);
    dev_add("valve.garden", "Garden Water", OUTSIDE, DEV_VALVE, UI_DEV_ST_CLOSED, -1, 0);
    ui_dev_t *hall = dev_add("climate.hallway", "Hallway", LIVING, DEV_CLIMATE, UI_DEV_ST_HEAT, -1, 0);
    hall->setpoint = 7200;
    hall->temperature = 7100;
    hall->modes = UI_DEV_MODE_OFF | UI_DEV_MODE_HEAT | UI_DEV_MODE_COOL | UI_DEV_MODE_AUTO;
    hall->hvac_action = DEV_HVAC_HEATING; /* its source says it is heating (BL-171) */
    ui_dev_t *night = dev_add("scene.good_night", "Good Night", LIVING, DEV_SCENE, UI_DEV_ST_UNKNOWN, -1, 0);
    night->since_ms = epoch_at_start - 131LL * 60000LL; /* 8:30 PM the same evening: a time shows
                                                           * for today, a date for an earlier day
                                                           * (BL-179) */
    dev_add("light.living_lamp", "Living Room Lamp", LIVING, DEV_LIGHT, UI_DEV_ST_ON, 40, UI_DEV_CAP_LEVEL);
    dev_add("light.kitchen", "Kitchen Lights", KITCHEN, DEV_LIGHT, UI_DEV_ST_OFF, 0, UI_DEV_CAP_LEVEL);

    /* As arranged: the order above, Favorites flagged, Kitchen Lights made one cell. */
    static ui_devl_t layout;
    ui_devl_clear(&layout);
    static const char *const FAV[] = {"light.porch", "lock.front_door", "lock.back_door", "cover.garage_door",
                                      "climate.hallway", "scene.good_night", "light.living_lamp", "light.kitchen"};
    for (size_t i = 0; i < s_dev.n; i++) {
        ui_devl_entry_t *e = &layout.e[layout.n++];
        snprintf(e->ref, sizeof(e->ref), "%s", s_dev.dev[i].ref);
        e->flags = 0;
        for (size_t f = 0; f < sizeof(FAV) / sizeof(FAV[0]); f++) {
            if (strcmp(FAV[f], e->ref) == 0) {
                e->flags = UI_DEVL_FAV;
            }
        }
        if (strcmp(e->ref, "light.kitchen") == 0) {
            e->flags |= UI_DEVL_SIZE_SMALL << UI_DEVL_SIZE_SHIFT;
        }
    }
    s_dev.layout_len = ui_devl_encode(&layout, s_dev.layout, sizeof(s_dev.layout));
    /* The registry lists them room by room (its order sets the order of the area tabs). */
    static const char *const ROOMS[] = {"Living room", "Kitchen", "Outside"};
    ui_dev_t sorted[DEV_WORLD_MAX];
    size_t k = 0;
    for (size_t r = 0; r < sizeof(ROOMS) / sizeof(ROOMS[0]); r++) {
        for (size_t i = 0; i < s_dev.n; i++) {
            if (strcmp(s_dev.dev[i].area, ROOMS[r]) == 0) {
                sorted[k++] = s_dev.dev[i];
            }
        }
    }
    memcpy(s_dev.dev, sorted, k * sizeof(ui_dev_t));
    s_dev.n = k;
}

void host_panel_devices(bool on)
{
    s_dev.none = !on;
}

static bool dev_has(uint8_t kind, uint8_t caps)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (s_dev.dev[i].kind == kind && (s_dev.dev[i].caps & caps) == caps) {
            return true;
        }
    }
    return false;
}

void host_panel_devices_extra(bool on)
{
    s_dev.n -= s_dev.extra; /* the extra ones are always the last */
    s_dev.extra = 0;
    if (!on) {
        return;
    }
    const size_t before = s_dev.n;
    /* a script: never offered to an automation (AUT-002) */
    if (s_dev.n < DEV_WORLD_MAX) {
        dev_add("script.movie_night", "Movie Night", "Living room", DEV_SCRIPT, UI_DEV_ST_UNKNOWN,
                -1, 0);
    }
    if (!dev_has(DEV_LOCK, 0) && s_dev.n < DEV_WORLD_MAX) {
        dev_add("lock.side_door", "Side Door", "Kitchen", DEV_LOCK, UI_DEV_ST_LOCKED, -1, 0);
    }
    if (!dev_has(DEV_COVER, UI_DEV_CAP_ENTRANCE) && s_dev.n < DEV_WORLD_MAX) {
        dev_add("cover.garage_door", "Garage Door", "Outside", DEV_COVER, UI_DEV_ST_CLOSED, 0,
                UI_DEV_CAP_ENTRANCE);
    }
    s_dev.extra = s_dev.n - before;
}

#define RANGE_THERMOSTAT "climate.upstairs"

void host_panel_devices_range_thermostat(bool on)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, RANGE_THERMOSTAT) == 0) {
            memmove(&s_dev.dev[i], &s_dev.dev[i + 1u], (s_dev.n - i - 1u) * sizeof(ui_dev_t));
            s_dev.n--;
            break;
        }
    }
    if (!on || s_dev.n >= DEV_WORLD_MAX) {
        return;
    }
    ui_dev_t *t = dev_add(RANGE_THERMOSTAT, "Upstairs", "Kitchen", DEV_CLIMATE, UI_DEV_ST_AUTO, -1, 0);
    t->temperature = 7000; /* the set point stays UI_DEV_TEMP_UNKNOWN: it holds a range */
    t->modes = UI_DEV_MODE_OFF | UI_DEV_MODE_HEAT | UI_DEV_MODE_COOL | UI_DEV_MODE_AUTO;
}

void host_panel_devices_unreachable(bool on, int64_t since_ms)
{
    static ui_dev_t was[DEV_WORLD_MAX];
    static size_t was_n;
    static bool saved;
    if (on && !saved) {
        memcpy(was, s_dev.dev, sizeof(was));
        was_n = s_dev.n;
        saved = true;
        for (size_t i = 0; i < s_dev.n; i++) {
            s_dev.dev[i].status = UI_DEV_OFFLINE;
            s_dev.dev[i].action = ACT_NONE;
            s_dev.dev[i].unsure = 0u;
            s_dev.dev[i].since_ms = since_ms;
        }
    } else if (!on && saved) {
        memcpy(s_dev.dev, was, sizeof(was));
        s_dev.n = was_n;
        saved = false;
    }
}

/* Takes a device out of the world by its ref, if it is there. */
static void dev_remove(const char *ref)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            memmove(&s_dev.dev[i], &s_dev.dev[i + 1u], (s_dev.n - i - 1u) * sizeof(ui_dev_t));
            s_dev.n--;
            return;
        }
    }
}

void host_panel_devices_rooms(bool on)
{
    static const char *const REFS[] = {"light.bedroom", "light.guest_room", "light.laundry",
                                       "light.office"};
    static const char *const ROOMS[] = {"Primary Bedroom", "Guest Bedroom", "Laundry Room",
                                        "Home Office"};
    for (size_t i = 0; i < sizeof(REFS) / sizeof(REFS[0]); i++) {
        dev_remove(REFS[i]);
    }
    if (!on || s_dev.n + sizeof(REFS) / sizeof(REFS[0]) > DEV_WORLD_MAX) {
        return;
    }
    for (size_t i = 0; i < sizeof(REFS) / sizeof(REFS[0]); i++) {
        dev_add(REFS[i], ROOMS[i], ROOMS[i], DEV_LIGHT, UI_DEV_ST_OFF, -1, 0);
    }
}

void host_panel_devices_limits(bool on)
{
    static const char *const REFS[] = {"fan.ceiling", "cover.blinds", RANGE_THERMOSTAT};
    for (size_t i = 0; i < sizeof(REFS) / sizeof(REFS[0]); i++) {
        dev_remove(REFS[i]);
    }
    if (!on || s_dev.n + 3u > DEV_WORLD_MAX) {
        return;
    }
    /* what Home Assistant says they can do (BL-171): a fan without speeds, blinds that can't stop */
    dev_add("fan.ceiling", "Ceiling Fan", "Living room", DEV_FAN, UI_DEV_ST_ON, -1, UI_DEV_CAP_NO_SPEED);
    dev_add("cover.blinds", "Blinds", "Living room", DEV_COVER, UI_DEV_ST_OPENING, 60,
            UI_DEV_CAP_LEVEL | UI_DEV_CAP_NO_STOP);
    ui_dev_t *t = dev_add(RANGE_THERMOSTAT, "Upstairs", "Living room", DEV_CLIMATE, UI_DEV_ST_AUTO, -1, 0);
    t->temperature = 7000; /* the set point stays UI_DEV_TEMP_UNKNOWN: it holds a range */
    t->modes = UI_DEV_MODE_OFF | UI_DEV_MODE_HEAT | UI_DEV_MODE_COOL | UI_DEV_MODE_AUTO;
}

void host_panel_device_failed(const char *ref, uint8_t action)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].status = UI_DEV_FAILED;
            s_dev.dev[i].action = action;
            s_dev.dev[i].unsure = 0u; /* refused: it didn't happen (HCTL-012) */
        }
    }
}

void host_panel_device_unconfirmed(const char *ref, uint8_t action)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].status = UI_DEV_FAILED;
            s_dev.dev[i].action = action;
            s_dev.dev[i].unsure = 1u; /* no answer, or the link dropped with it in flight */
        }
    }
}

void host_panel_device_pending(const char *ref, uint8_t action)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].status = UI_DEV_PENDING;
            s_dev.dev[i].action = action;
            s_dev.dev[i].unsure = 0u;
        }
    }
}

void host_panel_device_offline(const char *ref, int64_t since_ms)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].status = UI_DEV_OFFLINE;
            s_dev.dev[i].action = ACT_NONE;
            s_dev.dev[i].unsure = 0u;
            s_dev.dev[i].since_ms = since_ms;
        }
    }
}

static unsigned s_dev_acts;

unsigned host_panel_device_acts(void)
{
    return s_dev_acts;
}

int32_t host_panel_device_setpoint(const char *ref, int32_t setpoint)
{
    int32_t was = UI_DEV_TEMP_UNKNOWN;
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            was = s_dev.dev[i].setpoint;
            s_dev.dev[i].setpoint = setpoint;
        }
    }
    return was;
}

void host_panel_device_settle(const char *ref)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].status = UI_DEV_IDLE;
            s_dev.dev[i].action = ACT_NONE;
            s_dev.dev[i].unsure = 0u;
        }
    }
}

void host_panel_device_state(const char *ref, uint8_t state)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].state = state;
        }
    }
}

void host_panel_device_rename(const char *ref, const char *name)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            snprintf(s_dev.dev[i].name, sizeof(s_dev.dev[i].name), "%s", name);
        }
    }
}

void host_panel_devices_area_rename(const char *from, const char *to)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].area, from) == 0) {
            snprintf(s_dev.dev[i].area, sizeof(s_dev.dev[i].area), "%s", to);
        }
    }
}

void host_panel_device_climate(const char *ref, uint8_t hvac_action, int32_t temperature)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            s_dev.dev[i].hvac_action = hvac_action;
            s_dev.dev[i].temperature = temperature;
        }
    }
}

int64_t host_panel_device_since(const char *ref, int64_t since_ms)
{
    int64_t was = 0;
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            was = s_dev.dev[i].since_ms;
            s_dev.dev[i].since_ms = since_ms;
        }
    }
    return was;
}

void host_panel_devices_garage_closing(void)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, "cover.garage_door") == 0) {
            s_dev.dev[i].state = UI_DEV_ST_CLOSING;
            s_dev.dev[i].status = UI_DEV_PENDING;
            s_dev.dev[i].action = ACT_CLOSE;
        }
    }
}

const ui_dev_t *host_panel_device(const char *ref)
{
    for (size_t i = 0; i < s_dev.n; i++) {
        if (strcmp(s_dev.dev[i].ref, ref) == 0) {
            return &s_dev.dev[i];
        }
    }
    return NULL;
}

static size_t be_dev_list(void *ctx, ui_dev_t *out, size_t max)
{
    (void)ctx;
    const size_t n = s_dev.none ? 0 : (s_dev.n < max ? s_dev.n : max);
    memcpy(out, s_dev.dev, n * sizeof(ui_dev_t));
    return n;
}

/* An action's effect on a device, a person's (be_dev_act) or an automation's (auto_answer). */
static void dev_apply(ui_dev_t *d, uint8_t action, int32_t value)
{
    d->status = UI_DEV_IDLE;
    d->action = ACT_NONE;
    d->unsure = 0u;
    switch (action) {
    case ACT_ON:
        if (d->kind == DEV_LIGHT && value >= 0) {
            /* a brightness with the "on", as Home Assistant's turn_on takes it: 0 is off (B030) */
            d->level = (int8_t)(value > 100 ? 100 : value);
            d->state = value > 0 ? UI_DEV_ST_ON : UI_DEV_ST_OFF;
            break;
        }
        d->state = UI_DEV_ST_ON; /* -1: on, at its last level */
        if (d->level == 0) {
            d->level = 100; /* switched on: its last level, here full */
        }
        break;
    case ACT_OFF: d->state = UI_DEV_ST_OFF; break;
    case ACT_LOCK: d->state = UI_DEV_ST_LOCKED; break;
    case ACT_UNLOCK: d->state = UI_DEV_ST_UNLOCKED; break;
    case ACT_OPEN: d->state = UI_DEV_ST_OPEN; d->level = 100; break;
    case ACT_CLOSE: d->state = UI_DEV_ST_CLOSED; d->level = 0; break;
    case ACT_STOP:
        /* stopped part-way, as Home Assistant reports a cover it stopped; its position kept */
        if (d->state == UI_DEV_ST_OPENING || d->state == UI_DEV_ST_CLOSING) {
            d->state = UI_DEV_ST_OPEN;
        }
        break;
    case ACT_SET_LEVEL:
        d->level = (int8_t)(value < 0 ? 0 : value > 100 ? 100 : value);
        d->state = d->kind == DEV_COVER ? (value > 0 ? UI_DEV_ST_OPEN : UI_DEV_ST_CLOSED)
                                        : (value > 0 ? UI_DEV_ST_ON : UI_DEV_ST_OFF);
        break;
    case ACT_SET_TEMP: d->setpoint = value; break;
    case ACT_SET_MODE:
        /* a thermostat's mode, as a ui_dev_state_t (ui_backend.h): only one it has */
        if ((value == UI_DEV_ST_OFF && (d->modes & UI_DEV_MODE_OFF) != 0u) ||
            (value == UI_DEV_ST_HEAT && (d->modes & UI_DEV_MODE_HEAT) != 0u) ||
            (value == UI_DEV_ST_COOL && (d->modes & UI_DEV_MODE_COOL) != 0u) ||
            (value == UI_DEV_ST_AUTO && (d->modes & UI_DEV_MODE_AUTO) != 0u)) {
            if (d->state != (uint8_t)value) {
                /* what it then does isn't modelled here: its words are the mode's (BL-171) */
                d->hvac_action = DEV_HVAC_UNKNOWN;
            }
            d->state = (uint8_t)value;
        }
        break;
    case ACT_RUN: d->since_ms = H.epoch_at_start + (int64_t)(H.now - 5000); break;
    default: break;
    }
}

static ui_dev_result_t be_dev_act(void *ctx, const char *ref, uint8_t action, int32_t value,
                                  const char *code, alarm_result_t *code_result)
{
    (void)ctx;
    s_dev_acts++;
    ui_dev_t *d = (ui_dev_t *)host_panel_device(ref);
    if (d == NULL || !hc_action_valid((dev_kind_t)d->kind, (dev_action_t)action)) {
        return UI_DEV_REFUSED;
    }
    if (d->status == UI_DEV_OFFLINE) {
        return UI_DEV_UNREACHABLE;
    }
    if (d->status == UI_DEV_PENDING) {
        return UI_DEV_BUSY;
    }
    if (hc_needs_code((dev_kind_t)d->kind, (dev_action_t)action, value)) {
        if (code == NULL || code[0] == '\0') {
            return UI_DEV_NEED_CODE;
        }
        uint8_t user = 0;
        alarm_result_t r = alarm_engine_authenticate(&H.eng, &SRC, code, 0, H.now, &user);
        after();
        /* the firmware port's rule: an arm-only or a guest code may not unlock */
        if (r == ALARM_OK && user >= 1u && user <= ALARM_MAX_USERS) {
            const alarm_authority_t a = alarm_engine_config(&H.eng)->users[user - 1u].authority;
            if (a == ALARM_AUTH_ARM_ONLY || a == ALARM_AUTH_GUEST) {
                r = ALARM_ERR_NOT_AUTHORIZED;
            }
        }
        if (r != ALARM_OK) {
            if (code_result != NULL) {
                *code_result = r;
            }
            return UI_DEV_CODE_REFUSED;
        }
    }
    dev_apply(d, action, value);
    return UI_DEV_OK;
}

/* ---- automations (docs/features/41 U12) ------------------------------------------------------
 * The real core, wired as the panel's automation service will be (DESIGN.md section 5,
 * "Simulator."): the engine's events (on_event) and each snapshot after an engine call (after())
 * go through the observer (auto_watch) into one queue; the queue is handed to the runner
 * (auto_run) only after the engine call has returned, never inside an engine port. The runner's
 * device port acts on the fictional house above (s_dev), as device_service_act_auto does on the
 * panel: the same checks in the same order, layer 5 (hc_auto_allowed with the installer option,
 * AUT-002, AUT-015) included, and an answer 300 ms later. The maker check before a less secure
 * send reads the simulator engine's user, as the service reads the panel's. A failed Lock or Close is logged as the panel logs it
 * (ALARM_EV_AUTOMATION_FAILED), again from the queue's processing.
 *
 * Local time is the simulator's clock (H.epoch_at_start + H.now) under the process's time zone,
 * as zwp_local_time reads it: the PC simulator sets TZ to the panel's fixed EST5EDT,M3.2.0,
 * M11.1.0 (sim_main.c); the preview pins UTC0 so that its pictures don't depend on the PC.
 *
 * Harbor House's automations are seeded at reset, from the touchscreen mockup's list
 * (docs/features/07 mockup screens/automations.js). Its leak rule isn't: the house has
 * a water zone (Basement Water) but no valve on the water main, and the simulator's devices and
 * zones stay as they are. host_panel_automations(false) runs without any. */
#define AUTO_Q_MAX      32u
#define AUTO_FLIGHT_MAX 16u
#define AUTO_FAIL_MAX   8u
#define AUTO_ANSWER_MS  300u
#define AUTO_CHIME_MS   60000u /* the arming warning's chime (DESIGN.md section 3 "Arming" 2) */

typedef struct {
    uint32_t req;
    uint64_t at;  /* the answer comes then */
    uint8_t action;
    int32_t value;
    char ref[DEV_REF_LEN];
} auto_flight_sim_t;

typedef struct {
    uint16_t uid, zone;
    uint8_t reason, dev_action;
} auto_fail_sim_t;

typedef struct {
    char key[AUTO_STORE_KEY_LEN + 1u];
    bool used;
    uint16_t len;
    uint8_t data[AUTO_RECORD_MAX];
} auto_kv_sim_t;

typedef struct {
    uint16_t uid; /* 0: unused */
    auto_status_t st;
} auto_forced_sim_t;

static struct {
    bool on; /* rules seeded (host_panel_automations) */
    bool ready;
    bool busy;
    auto_watch_t watch;
    auto_run_t run;
    auto_clock_t clock;
    auto_store_t store;          /* the rules, as automation_service keeps them */
    auto_kv_sim_t kv[AUTO_MAX + 1u]; /* the store's "NVS": meta and a01..a16 */
    bool kv_fail;                /* host_panel_auto_store_fail: every write refused */
    auto_forced_sim_t forced[AUTO_MAX];
    auto_rule_t rules[AUTO_MAX]; /* the store's list, as the runner was given it */
    size_t n;
    auto_zone_t zones[ALARM_MAX_ZONES];
    uint32_t env_seq; /* the H.config_seq the runner's zones were read at */
    auto_occ_t q[AUTO_Q_MAX];
    size_t qn;
    auto_flight_sim_t flight[AUTO_FLIGHT_MAX];
    auto_fail_sim_t fail[AUTO_FAIL_MAX];
    size_t nfail;
    uint32_t next_req;
    host_panel_auto_counts_t counts;
    /* phase C: arming and disarming */
    auto_warning_t warn[AUTO_MAX]; /* the warning mailbox (automation_service's s_warn) */
    size_t nwarn;
    bool engine_called;   /* an arm or disarm port ran the engine: a new snapshot is due */
    bool chiming;         /* a warning is ON and has chimed */
    uint64_t chime_next;
} s_auto;

/* ---- the store's key-value port: RAM, refusing every write while kv_fail ---- */

static auto_kv_sim_t *auto_kv_row(const char *key, bool add)
{
    for (size_t i = 0; i < AUTO_MAX + 1u; i++) {
        if (s_auto.kv[i].used && strcmp(s_auto.kv[i].key, key) == 0) {
            return &s_auto.kv[i];
        }
    }
    for (size_t i = 0; add && i < AUTO_MAX + 1u; i++) {
        if (!s_auto.kv[i].used) {
            return &s_auto.kv[i];
        }
    }
    return NULL;
}

static auto_kv_t auto_kv_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    const auto_kv_sim_t *row = auto_kv_row(key, false);
    if (row == NULL) {
        return AUTO_KV_ABSENT;
    }
    if (row->len > cap) {
        return AUTO_KV_TOO_BIG;
    }
    memcpy(buf, row->data, row->len);
    *len = row->len;
    return AUTO_KV_OK;
}

static bool auto_kv_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    auto_kv_sim_t *row = s_auto.kv_fail || len > AUTO_RECORD_MAX ? NULL : auto_kv_row(key, true);
    if (row == NULL) {
        return false;
    }
    snprintf(row->key, sizeof(row->key), "%s", key);
    memcpy(row->data, data, len);
    row->len = (uint16_t)len;
    row->used = true;
    return true;
}

static bool auto_kv_erase(void *ctx, const char *key)
{
    (void)ctx;
    if (s_auto.kv_fail) {
        return false;
    }
    auto_kv_sim_t *row = auto_kv_row(key, false);
    if (row != NULL) {
        memset(row, 0, sizeof(*row));
    }
    return true;
}

static const auto_kv_port_t *auto_kv(void)
{
    static const auto_kv_port_t port = {.ctx = NULL,
                                        .get = auto_kv_get,
                                        .set = auto_kv_set,
                                        .erase = auto_kv_erase,
                                        .free_entries = NULL};
    return &port;
}

/* An empty store, loaded as the service loads it at start. */
static void auto_store_empty(void)
{
    memset(s_auto.kv, 0, sizeof(s_auto.kv));
    memset(s_auto.forced, 0, sizeof(s_auto.forced));
    s_auto.kv_fail = false;
    (void)auto_store_load(&s_auto.store, auto_kv());
}

static void auto_feed_event(const alarm_event_t *ev)
{
    if (!s_auto.ready) {
        return;
    }
    s_auto.qn += auto_watch_event(&s_auto.watch, ev, &s_auto.q[s_auto.qn], AUTO_Q_MAX - s_auto.qn);
}

static void auto_feed_snapshot(const alarm_snapshot_t *s)
{
    if (!s_auto.ready) {
        return;
    }
    s_auto.qn += auto_watch_snapshot(&s_auto.watch, s, &s_auto.q[s_auto.qn], AUTO_Q_MAX - s_auto.qn);
}

/* The simulator's local time, as the panel's localtime_r under its TZ gives it. */
static void auto_local_now(auto_local_t *out)
{
    memset(out, 0, sizeof(*out));
    const int64_t ms = epoch_ms(NULL);
    if (ms < AUTO_CLOCK_EPOCH_MIN * 1000LL) {
        return;
    }
    const time_t t = (time_t)(ms / 1000);
    const struct tm *tm = localtime(&t);
    if (tm == NULL) {
        return;
    }
    const rtc_civil_t c = {tm->tm_year + 1900,     (uint8_t)(tm->tm_mon + 1), (uint8_t)tm->tm_mday,
                           (uint8_t)tm->tm_hour,   (uint8_t)tm->tm_min,       (uint8_t)tm->tm_sec,
                           0};
    (void)auto_local_from_civil(&c, (int64_t)(ms / 1000), true, out);
}

static ui_dev_t *auto_device(uint8_t source, const char *ref)
{
    /* the fictional house is Home Assistant's; a panel without devices lists none */
    if (source != DSRC_HA || s_dev.none) {
        return NULL;
    }
    return (ui_dev_t *)host_panel_device(ref);
}

/* device_service_act_auto's checks, in hc_act_auto's order (AUT-002, AUT-015). */
static int auto_port_act(void *ctx, const auto_action_t *a, bool less_secure,
                         uint32_t *request_id)
{
    (void)ctx;
    *request_id = 0;
    ui_dev_t *d = auto_device(a->source, a->ref);
    if (d == NULL) {
        return HC_ERR_UNKNOWN;
    }
    if (d->status == UI_DEV_OFFLINE) {
        return HC_ERR_OFFLINE;
    }
    if (d->status == UI_DEV_PENDING) {
        return HC_ERR_BUSY;
    }
    if (!hc_action_valid((dev_kind_t)d->kind, (dev_action_t)a->dev_action)) {
        return HC_ERR_ARG;
    }
    if (!hc_auto_allowed((dev_kind_t)d->kind, (dev_action_t)a->dev_action, a->value, a->mode,
                         less_secure)) {
        s_auto.counts.refused++; /* layer 5, on the device's live kind */
        return HC_ERR_NOT_ALLOWED;
    }
    size_t f = 0;
    while (f < AUTO_FLIGHT_MAX && s_auto.flight[f].req != 0u) {
        f++;
    }
    if (f == AUTO_FLIGHT_MAX) {
        return HC_ERR_BUSY; /* more under way than the simulator keeps */
    }
    /* a mode word is a thermostat's mode, as be_dev_act takes it (a ui_dev_state_t) */
    int32_t value = a->value;
    if (a->dev_action == ACT_SET_MODE) {
        value = strcmp(a->mode, "off") == 0    ? UI_DEV_ST_OFF
                : strcmp(a->mode, "heat") == 0 ? UI_DEV_ST_HEAT
                : strcmp(a->mode, "cool") == 0 ? UI_DEV_ST_COOL
                : strcmp(a->mode, "auto") == 0 ? UI_DEV_ST_AUTO
                                               : -1;
    }
    if (++s_auto.next_req == 0u) {
        s_auto.next_req = 1u;
    }
    auto_flight_sim_t *fl = &s_auto.flight[f];
    fl->req = s_auto.next_req;
    fl->at = H.now + AUTO_ANSWER_MS;
    fl->action = a->dev_action;
    fl->value = value;
    snprintf(fl->ref, sizeof(fl->ref), "%s", a->ref);
    d->status = UI_DEV_PENDING; /* the tile shows it under way, as a person's action */
    d->action = a->dev_action;
    s_auto.counts.sent++;
    *request_id = fl->req;
    return HC_OK;
}

static bool auto_port_settled(void *ctx, uint8_t source)
{
    (void)ctx;
    (void)source;
    return true; /* the simulator's list is always complete */
}

static bool auto_port_kind(void *ctx, uint8_t source, const char *ref, uint8_t *kind)
{
    (void)ctx;
    const ui_dev_t *d = auto_device(source, ref);
    if (d == NULL) {
        return false;
    }
    *kind = d->kind;
    return true;
}

/* AUT-015, D23: the maker's slot still holds a system master or installer, read from the
 * simulator's engine at send time. */
static bool auto_port_maker_ok(void *ctx, uint8_t user, bool *option_on)
{
    (void)ctx;
    *option_on = alarm_engine_config(&H.eng)->opt.auto_less_secure; /* read live (AUT-015) */
    if (user == 0u || user > ALARM_MAX_USERS) {
        return false;
    }
    const alarm_authority_t a = alarm_engine_config(&H.eng)->users[user - 1u].authority;
    return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_INSTALLER;
}

/* Kept, and logged once the runner's call has returned (auto_pump). */
static void auto_port_failed(void *ctx, uint16_t uid, uint8_t reason, uint8_t dev_action,
                             uint16_t zone)
{
    (void)ctx;
    if (s_auto.nfail < AUTO_FAIL_MAX) {
        s_auto.fail[s_auto.nfail++] = (auto_fail_sim_t){uid, zone, reason, dev_action};
    }
}

/* ---- phase C: arming and disarming, as automation_service's ports (DESIGN.md section 3) ----
 * The engine is called from the runner's call inside auto_pump - after the engine call that fed
 * it has returned, never inside an engine port; its events go into the queue for the next pump
 * (auto_feed_event) and auto_pump asks after() for a new snapshot. */

/* alarm_service_command_as's state check (alarm_service.c command_state_ok): the same rule as
 * zbr_fob_state_ok, for one partition and one `when`. */
static bool auto_state_ok(uint8_t p, bool want_disarmed)
{
    alarm_partition_status_t ps;
    alarm_engine_partition_status(&H.eng, p, &ps);
    const bool disarmed = ps.state == ALARM_STATE_DISARMED;
    if (want_disarmed) {
        return disarmed && !alarm_engine_program_active(&H.eng);
    }
    const bool sounding = ps.sounder == ALARM_SOUND_ALARM || ps.sounder == ALARM_SOUND_FIRE ||
                          ps.sounder == ALARM_SOUND_CO || ps.sounder == ALARM_SOUND_AUX;
    return !disarmed || sounding;
}

/* port_arm: "Bypass open zones" only while the installer option is on now (decided
 * 2026-10-05); BAD_STATE is "armed already" only while the partition isn't disarmed (in
 * installer mode it is a refusal). */
static alarm_result_t auto_port_arm(void *ctx, uint16_t uid, uint8_t p, uint8_t level,
                                    uint8_t flags, uint8_t maker, bool *already, uint16_t *zone)
{
    (void)ctx;
    *already = false;
    *zone = 0;
    if (!alarm_engine_config(&H.eng)->opt.auto_less_secure) {
        flags &= (uint8_t)~ALARM_ARM_BYPASS_FAULTED;
    }
    if (!auto_state_ok(p, true)) {
        alarm_partition_status_t ps;
        alarm_engine_partition_status(&H.eng, p, &ps);
        *already = ps.state != ALARM_STATE_DISARMED;
        return ALARM_ERR_BAD_STATE;
    }
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = p;
    cmd.level = (alarm_arm_level_t)level;
    cmd.arm_flags = flags;
    const alarm_source_t src = {.kind = ALARM_SRC_AUTOMATION, .id = uid};
    alarm_cmd_result_t res;
    memset(&res, 0, sizeof(res));
    const alarm_result_t r = alarm_engine_command_as(&H.eng, &cmd, &src, maker, H.now, &res);
    H.dirty = true;
    s_auto.engine_called = true;
    if (r == ALARM_ERR_BAD_STATE) {
        alarm_partition_status_t ps;
        alarm_engine_partition_status(&H.eng, p, &ps);
        *already = ps.state != ALARM_STATE_DISARMED;
    } else if (r == ALARM_ERR_NOT_READY) {
        *zone = res.not_ready_zone;
    }
    return r;
}

static alarm_result_t auto_port_disarm(void *ctx, uint16_t uid, uint8_t p, uint8_t maker,
                                       bool *already)
{
    (void)ctx;
    *already = false;
    if (!auto_state_ok(p, false)) {
        *already = true;
        return ALARM_ERR_BAD_STATE;
    }
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_DISARM;
    cmd.partition = p;
    const alarm_source_t src = {.kind = ALARM_SRC_AUTOMATION, .id = uid};
    const alarm_result_t r = alarm_engine_command_as(&H.eng, &cmd, &src, maker, H.now, NULL);
    H.dirty = true;
    s_auto.engine_called = true;
    *already = r == ALARM_ERR_BAD_STATE;
    return r;
}

/* The warning mailbox (automation_service's port_warning). */
static void auto_port_warning(void *ctx, const auto_warning_t *w)
{
    (void)ctx;
    size_t i = 0;
    while (i < s_auto.nwarn && s_auto.warn[i].uid != w->uid) {
        i++;
    }
    if (w->state == AUTO_WARN_NONE) {
        if (i < s_auto.nwarn) {
            memmove(&s_auto.warn[i], &s_auto.warn[i + 1u],
                    (s_auto.nwarn - i - 1u) * sizeof(s_auto.warn[0]));
            s_auto.nwarn--;
        }
    } else if (i < s_auto.nwarn) {
        s_auto.warn[i] = *w;
    } else if (s_auto.nwarn < AUTO_MAX) {
        s_auto.warn[s_auto.nwarn++] = *w;
    }
}

/* automation_service's port_put_off: ALARM_EV_ARMING_PUT_OFF into the engine; Not tonight or
 * Not today from the warning just published. */
static void auto_port_put_off(void *ctx, uint16_t uid, uint8_t partition, uint8_t user,
                              uint8_t choice, int64_t until_epoch_ms)
{
    (void)ctx;
    alarm_put_off_t c = ALARM_PUT_OFF_HOUR;
    if (choice != (uint8_t)AUTO_PUT_OFF_HOUR) {
        c = ALARM_PUT_OFF_NIGHT;
        for (size_t i = 0; i < s_auto.nwarn; i++) {
            if (s_auto.warn[i].uid == uid && s_auto.warn[i].state == AUTO_WARN_DAY) {
                c = ALARM_PUT_OFF_DAY;
            }
        }
    }
    (void)alarm_engine_arming_put_off(&H.eng, partition, uid, user, c, until_epoch_ms, H.now);
    H.dirty = true;
}

/* The chime (automation_service's chime()): at a warning's start, then once a minute while one
 * is ON, never during a hold. The simulator's sounder has no background one-shot of its own
 * (host_panel_sound_t), so it is played only while the keypad's partition sounds nothing. */
static void auto_chime(void)
{
    bool any_on = false;
    for (size_t i = 0; i < s_auto.nwarn && !any_on; i++) {
        any_on = s_auto.warn[i].state == AUTO_WARN_ON;
    }
    if (!any_on) {
        s_auto.chiming = false;
        return;
    }
    if (!s_auto.chiming || H.now >= s_auto.chime_next) {
        if (H.snap.part[0].sounder == ALARM_SOUND_NONE) {
            say_sound(ALARM_SOUND_CHIME);
        }
        s_auto.chiming = true;
        s_auto.chime_next = H.now + AUTO_CHIME_MS;
    }
}

/* The runner's view of the panel, from the engine's configuration. */
static void auto_env(auto_env_t *env)
{
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    memset(env, 0, sizeof(*env));
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if (cfg->partitions[p].enabled) {
            env->partitions_enabled |= (uint8_t)(1u << p);
        }
    }
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        s_auto.zones[z].type = (uint8_t)cfg->zones[z].type;
        s_auto.zones[z].partition = cfg->zones[z].partition;
        s_auto.zones[z].dev_class = cfg->zones[z].device_class;
    }
    env->arm_supported = true; /* phase C */
    env->zones = s_auto.zones;
    env->less_secure = cfg->opt.auto_less_secure; /* AUT-015 */
}

static void auto_give_rules(void)
{
    auto_env_t env;
    auto_env(&env);
    s_auto.n = auto_store_list(&s_auto.store, s_auto.rules, AUTO_MAX);
    auto_run_rules(&s_auto.run, s_auto.rules, s_auto.n, &env);
    s_auto.env_seq = H.config_seq;
}

static auto_action_t *auto_seed_action(auto_rule_t *r, uint8_t kind, uint8_t action,
                                       const char *ref, const char *name)
{
    auto_action_t *a = &r->then[r->n++];
    memset(a, 0, sizeof(*a));
    a->kind = AUTO_ACT_DEVICE;
    a->source = DSRC_HA;
    a->dev_kind = kind;
    a->dev_action = action;
    a->value = -1;
    snprintf(a->ref, sizeof(a->ref), "%s", ref);
    snprintf(a->name, sizeof(a->name), "%s", name);
    return a;
}

static void auto_seed_rule(auto_rule_t *r, uint8_t trig)
{
    memset(r, 0, sizeof(*r));
    r->when.kind = trig;
}

/* Saved as a person saves one: checked, made by the system master, the next uid and slot. */
static void auto_seed_save(const auto_rule_t *r)
{
    auto_env_t env;
    auto_env(&env);
    auto_check_t why = AUTO_OK;
    const auto_save_t res = auto_store_save(&s_auto.store, auto_kv(), r, &env, ALARM_USER_MASTER,
                                            s_auto.store.gen, &why, NULL);
    if (res != AUTO_SAVE_OK) {
        /* A seed the validator refuses is a broken fixture, not a state to test: stop loudly. */
        fprintf(stderr, "host_panel: a seeded automation wasn't saved (%d, %d)\n", (int)res,
                (int)why);
        abort();
    }
}

/* Harbor House's automations (the mockup's list, screens/automations.js): uids and slots
 * 1..4, in this order. */
static void auto_seed(void)
{
    auto_rule_t r;
    /* "When Front Door opens: Turn on Porch Light for 10 minutes", only between 6:00 PM and
     * 6:00 AM (the idea's window) */
    auto_seed_rule(&r, AUTO_TRIG_ZONE_OPENED);
    r.when.zone = 1;
    r.when.win_from = 18u * 60u + 1u;
    r.when.win_to = 6u * 60u + 1u;
    auto_seed_action(&r, DEV_LIGHT, ACT_ON, "light.porch", "Porch Light")->off_after_min = 10;
    auto_seed_save(&r);
    /* "When the panel is armed Away: Lock Front Door, Run Good Night" */
    auto_seed_rule(&r, AUTO_TRIG_ARMED);
    r.when.levels = ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY);
    auto_seed_action(&r, DEV_LOCK, ACT_LOCK, "lock.front_door", "Front Door");
    auto_seed_action(&r, DEV_SCENE, ACT_RUN, "scene.good_night", "Good Night")->confirmed = 1;
    auto_seed_save(&r);
    /* "When a fire or CO alarm sounds: Turn off Hallway" (the thermostat's "off" mode) */
    auto_seed_rule(&r, AUTO_TRIG_FIRE);
    auto_action_t *a = auto_seed_action(&r, DEV_CLIMATE, ACT_SET_MODE, "climate.hallway", "Hallway");
    snprintf(a->mode, sizeof(a->mode), "off");
    auto_seed_save(&r);
    /* "At 11:00 PM on weekdays: Close Garage Door", turned off */
    auto_seed_rule(&r, AUTO_TRIG_TIME);
    r.when.minute = 23u * 60u;
    r.when.days = AUTO_DAY_BIT(1) | AUTO_DAY_BIT(2) | AUTO_DAY_BIT(3) | AUTO_DAY_BIT(4) |
                  AUTO_DAY_BIT(5);
    r.off = true;
    auto_seed_action(&r, DEV_COVER, ACT_CLOSE, "cover.garage_door", "Garage Door");
    auto_seed_save(&r);
}

/* notify's automation hook (docs/features/41 D30), as auto_names.c is on the panel: the store's
 * rule by uid, its trigger clause, changed_at and maker; false once it's gone. The preview runs
 * on one thread, so no lock. */
static bool auto_names_lookup(void *ctx, const i18n_catalog_t *cat, const alarm_config_t *cfg,
                              uint16_t uid, notify_auto_info_t *out)
{
    (void)ctx;
    auto_rule_t r;
    if (!s_auto.ready || !auto_store_get(&s_auto.store, uid, &r)) {
        return false;
    }
    notify_auto_when(cat, cfg, NULL, false, &r.when, out->clause, sizeof(out->clause));
    out->changed_at = r.changed_at;
    out->maker = r.made_by;
    return true;
}

/* A put-off's "until 11:30 PM": the PC's local time, on the preview's 12-hour clock. */
static void auto_names_time(void *ctx, const i18n_catalog_t *cat, int64_t epoch_ms, char *out,
                            size_t len)
{
    (void)ctx;
    const time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) {
#else
    if (localtime_r(&t, &tm) == NULL) {
#endif
        out[0] = '\0';
        return;
    }
    notify_clock_text(cat, false, (unsigned)tm.tm_hour, (unsigned)tm.tm_min, out, len);
}

static void automations_reset(void)
{
    const notify_hooks_t names = {auto_names_lookup, auto_names_time, NULL};
    notify_set_hooks(&names);
    memset(&s_auto, 0, sizeof(s_auto));
    const auto_ports_t ports = {.ctx = NULL,
                                .device_act = auto_port_act,
                                .source_settled = auto_port_settled,
                                .device_kind = auto_port_kind,
                                .arm = auto_port_arm,
                                .warning = auto_port_warning,
                                .failed = auto_port_failed,
                                .maker_ok = auto_port_maker_ok,
                                .disarm = auto_port_disarm,
                                .put_off = auto_port_put_off};
    auto_watch_init(&s_auto.watch, H.now,
                    (uint32_t)alarm_engine_config(&H.eng)->opt.power_up_settle_s * 1000u);
    auto_run_init(&s_auto.run, &ports, H.now);
    auto_clock_init(&s_auto.clock);
    s_auto.on = true;
    auto_store_empty();
    auto_seed();
    auto_give_rules();
    s_auto.ready = true;
}

void host_panel_automations(bool on)
{
    s_auto.on = on;
    auto_store_empty();
    if (on) {
        auto_seed();
    }
    auto_give_rules();
}

/* A time rule that can run needs the clock read at least once a second (auto_clock.h). */
static bool auto_time_rule_on(void)
{
    for (size_t i = 0; i < s_auto.n; i++) {
        if (s_auto.rules[i].when.kind == AUTO_TRIG_TIME && !s_auto.rules[i].off) {
            return true;
        }
    }
    return false;
}

static uint64_t auto_next_deadline(void)
{
    if (!s_auto.ready) {
        return UINT64_MAX;
    }
    uint64_t next = auto_run_next_deadline(&s_auto.run, H.now);
    for (size_t f = 0; f < AUTO_FLIGHT_MAX; f++) {
        if (s_auto.flight[f].req != 0u && s_auto.flight[f].at < next) {
            next = s_auto.flight[f].at;
        }
    }
    if ((auto_time_rule_on() || auto_run_wants_clock(&s_auto.run)) && H.now + 1000u < next) {
        next = H.now + 1000u;
    }
    if (s_auto.chiming && s_auto.chime_next < next) {
        next = s_auto.chime_next;
    }
    return next <= H.now ? H.now + 1u : next; /* auto_pump has just run what was due */
}

/* The devices' answers that are due: the action takes effect (dev_apply, as a person's). */
static void auto_answers(void)
{
    for (size_t f = 0; f < AUTO_FLIGHT_MAX; f++) {
        auto_flight_sim_t *fl = &s_auto.flight[f];
        if (fl->req == 0u || fl->at > H.now) {
            continue;
        }
        const uint32_t req = fl->req;
        fl->req = 0;
        ui_dev_t *d = (ui_dev_t *)host_panel_device(fl->ref);
        if (d == NULL || d->status == UI_DEV_OFFLINE) {
            if (d != NULL) {
                d->action = ACT_NONE;
            }
            auto_run_result(&s_auto.run, req, false, AUTO_WHY_NO_ANSWER, H.now);
            continue;
        }
        dev_apply(d, fl->action, fl->value);
        auto_run_result(&s_auto.run, req, true, 0, H.now);
    }
}

/* After an engine call: the queue to the runner, the clock, the answers, the deadlines, then
 * the failures into the engine. True when the engine was called (a new snapshot is due). */
static bool auto_pump(void)
{
    if (!s_auto.ready || s_auto.busy) {
        return false;
    }
    s_auto.busy = true;
    if (s_auto.env_seq != H.config_seq) {
        auto_give_rules(); /* the configuration changed: zones and partitions again */
    }
    auto_local_t lt;
    auto_local_now(&lt);
    for (size_t i = 0; i < s_auto.qn; i++) {
        auto_run_occurrence(&s_auto.run, &s_auto.q[i], &lt, H.now);
    }
    s_auto.qn = 0;
    auto_span_t spans[2];
    const size_t ns = auto_clock_step(&s_auto.clock, H.now, &lt, spans);
    auto_run_time(&s_auto.run, &s_auto.clock, &lt, spans, ns, H.now);
    auto_answers();
    auto_run_tick(&s_auto.run, H.now);
    auto_chime();
    const bool logged = s_auto.nfail != 0u || s_auto.engine_called;
    s_auto.engine_called = false;
    for (size_t i = 0; i < s_auto.nfail; i++) {
        const auto_fail_sim_t *f = &s_auto.fail[i];
        const alarm_source_t src = {.kind = ALARM_SRC_AUTOMATION, .id = f->uid};
        (void)alarm_engine_platform_event_ex(&H.eng, ALARM_EV_AUTOMATION_FAILED, f->zone,
                                             f->reason, f->dev_action, &src, 0, H.now);
    }
    s_auto.nfail = 0;
    s_auto.busy = false;
    return logged;
}

void host_panel_auto_status(uint16_t uid, auto_status_t *out)
{
    auto_run_status(&s_auto.run, uid, out);
}

size_t host_panel_auto_rules(auto_rule_t *out, size_t max)
{
    const size_t n = s_auto.n < max ? s_auto.n : max;
    memcpy(out, s_auto.rules, n * sizeof(auto_rule_t));
    return n;
}

/* TEST-ONLY (host_panel.h): the rule into the store as given - no auto_rule_check, no lock, no
 * write, no gen - as if a firmware had stored it before. */
bool host_panel_auto_inject(const auto_rule_t *r)
{
    auto_store_t *st = &s_auto.store;
    if (r == NULL) {
        return false;
    }
    unsigned idx = AUTO_MAX;
    if (r->slot >= 1u && r->slot <= AUTO_MAX && !st->used[r->slot - 1u]) {
        idx = r->slot - 1u;
    }
    for (unsigned i = 0; idx == AUTO_MAX && i < AUTO_MAX; i++) {
        idx = st->used[i] ? AUTO_MAX : i;
    }
    const uint16_t uid = r->uid != 0u ? r->uid : st->next_uid;
    if (idx == AUTO_MAX || (r->rec != AUTO_REC_BAD && uid == 0u)) {
        return false;
    }
    st->rules[idx] = *r;
    st->rules[idx].slot = (uint8_t)(idx + 1u);
    st->rules[idx].uid = r->rec == AUTO_REC_BAD ? r->uid : uid;
    st->used[idx] = true;
    st->bad_len[idx] = 0u;
    if (r->rec == AUTO_REC_BAD) {
        st->bad[idx][0] = 0xFFu; /* bytes nobody can read */
        st->bad_len[idx] = 1u;
    }
    if (uid != 0u && st->next_uid != 0u && uid >= st->next_uid) {
        st->next_uid = uid == 0xFFFFu ? 0u : (uint16_t)(uid + 1u); /* never handed out again */
    }
    auto_give_rules();
    return true;
}

/* TEST-ONLY (host_panel.h): an OK rule, the next uid and slot, made_by as given. */
bool host_panel_auto_add_unchecked(const auto_rule_t *r)
{
    if (r == NULL) {
        return false;
    }
    auto_rule_t c = *r;
    c.uid = 0;
    c.slot = 0;
    c.rec = AUTO_REC_OK;
    return host_panel_auto_inject(&c);
}

auto_check_t host_panel_auto_check(const auto_rule_t *r)
{
    auto_env_t env;
    auto_env(&env);
    return auto_rule_check(r, &env);
}

/* ---- the edits, as automation_service answers them (AUT-010, AUT-015) ---- */

static bool auto_known(uint16_t uid)
{
    for (size_t i = 0; i < s_auto.n; i++) {
        if (uid != 0u && s_auto.rules[i].uid == uid) {
            return true;
        }
    }
    return false;
}

/* What every edit answers first: the panel locked (AUT-010). The store answers the rest. */
static bool auto_locked(void)
{
    return alarm_snapshot_locked(&H.snap);
}

static auto_save_t auto_done(auto_save_t res)
{
    auto_give_rules(); /* a refused write may still have moved gen (auto_store.h) */
    return res;
}

uint16_t host_panel_auto_gen(void)
{
    return s_auto.store.gen;
}

auto_save_t host_panel_auto_save(const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                                 auto_check_t *why, uint16_t *uid_out)
{
    if (why != NULL) {
        *why = AUTO_OK;
    }
    if (auto_locked()) {
        return AUTO_SAVE_LOCKED;
    }
    if (r == NULL) {
        if (why != NULL) {
            *why = AUTO_ERR_INCOMPLETE;
        }
        return AUTO_SAVE_INVALID;
    }
    auto_env_t env;
    auto_env(&env); /* the panel now, never a caller's (AUT-015) */
    const alarm_config_t *cfg = alarm_engine_config(&H.eng);
    env.by_installer = user >= 1u && user <= ALARM_MAX_USERS &&
                       cfg->users[user - 1u].authority == ALARM_AUTH_INSTALLER; /* BL-187 */
    /* the record's changed_at (D30): the wall clock while it is set, as the service stamps it */
    const int64_t ms = epoch_ms(NULL);
    const uint32_t now_s =
        ms >= AUTO_CLOCK_EPOCH_MIN * 1000LL && ms / 1000 <= (int64_t)UINT32_MAX ? (uint32_t)(ms / 1000)
                                                                               : 0u;
    return auto_done(auto_store_save_at(&s_auto.store, auto_kv(), r, &env, user, expected_gen,
                                        now_s, why, uid_out));
}

auto_save_t host_panel_auto_delete(uint16_t uid, uint16_t expected_gen)
{
    if (auto_locked()) {
        return AUTO_SAVE_LOCKED;
    }
    return auto_done(auto_store_delete(&s_auto.store, auto_kv(), uid, expected_gen));
}

auto_save_t host_panel_auto_delete_slot(uint8_t slot, uint16_t expected_gen)
{
    if (auto_locked()) {
        return AUTO_SAVE_LOCKED;
    }
    return auto_done(auto_store_delete_slot(&s_auto.store, auto_kv(), slot, expected_gen));
}

auto_save_t host_panel_auto_set_off(uint16_t uid, bool off, uint16_t expected_gen)
{
    if (auto_locked()) {
        return AUTO_SAVE_LOCKED;
    }
    return auto_done(auto_store_set_off(&s_auto.store, auto_kv(), uid, off, expected_gen));
}

/* ---- the test controls (host_panel.h) ---- */

void host_panel_auto_read_only(bool on)
{
    s_auto.store.read_only = on;
}

void host_panel_auto_store_fail(bool on)
{
    s_auto.kv_fail = on;
}

void host_panel_auto_force_status(uint16_t uid, const auto_status_t *st)
{
    auto_forced_sim_t *free_row = NULL;
    for (size_t i = 0; i < AUTO_MAX; i++) {
        auto_forced_sim_t *f = &s_auto.forced[i];
        if (f->uid == uid && uid != 0u) {
            if (st == NULL) {
                memset(f, 0, sizeof(*f));
            } else {
                f->st = *st;
            }
            return;
        }
        if (f->uid == 0u && free_row == NULL) {
            free_row = f;
        }
    }
    if (st != NULL && uid != 0u && free_row != NULL) {
        free_row->uid = uid;
        free_row->st = *st;
    }
}

void host_panel_auto_busy(bool on)
{
    s_auto.busy = on;
}

void host_panel_auto_less_secure(bool on)
{
    alarm_config_t cfg = *alarm_engine_config(&H.eng);
    cfg.opt.auto_less_secure = on;
    (void)be_set_config(NULL, &cfg); /* as the installer's page saves it */
}

/* ---- ui_backend_t's automation hooks ---- */

static size_t be_auto_list(void *ctx, auto_rule_t *out, size_t max)
{
    (void)ctx;
    return auto_store_list(&s_auto.store, out, max);
}

static uint16_t be_auto_gen(void *ctx)
{
    (void)ctx;
    return s_auto.store.gen;
}

static bool be_auto_read_only(void *ctx)
{
    (void)ctx;
    return s_auto.store.read_only;
}

static bool be_auto_less_secure(void *ctx)
{
    (void)ctx;
    return alarm_engine_config(&H.eng)->opt.auto_less_secure; /* live (AUT-015) */
}

static auto_save_t be_auto_save(void *ctx, const auto_rule_t *r, uint8_t user,
                                uint16_t expected_gen, auto_check_t *why, uint16_t *uid_out)
{
    (void)ctx;
    return host_panel_auto_save(r, user, expected_gen, why, uid_out);
}

static auto_save_t be_auto_delete_slot(void *ctx, uint8_t slot, uint16_t expected_gen)
{
    (void)ctx;
    return host_panel_auto_delete_slot(slot, expected_gen);
}

static auto_save_t be_auto_set_off(void *ctx, uint16_t uid, bool off, uint16_t expected_gen)
{
    (void)ctx;
    return host_panel_auto_set_off(uid, off, expected_gen);
}

static bool be_auto_status(void *ctx, uint16_t uid, auto_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < AUTO_MAX; i++) {
        if (uid != 0u && s_auto.forced[i].uid == uid) {
            *out = s_auto.forced[i].st;
            return true;
        }
    }
    if (!auto_known(uid)) {
        return false;
    }
    auto_run_status(&s_auto.run, uid, out);
    return true;
}

static auto_try_t be_auto_try(void *ctx, uint16_t uid)
{
    (void)ctx;
    if (!s_auto.ready || s_auto.busy) {
        return UI_AUTO_TRY_BUSY;
    }
    const auto_try_t res = auto_run_try(&s_auto.run, uid, H.now);
    H.dirty = true; /* its sends are answered as the clock runs (host_panel_advance) */
    return res;
}

static size_t be_auto_warnings(void *ctx, auto_warning_t *out, size_t max)
{
    (void)ctx;
    const size_t n = s_auto.nwarn < max ? s_auto.nwarn : max;
    memcpy(out, s_auto.warn, n * sizeof(out[0]));
    return n;
}

/* ui_port_esp's auto_put_off: the code checked as the keypad's (ALARM_CMD_CODE_CHECK on the
 * warning's partition), then the runner's put-off by that code's user. */
static auto_put_off_t be_auto_put_off(void *ctx, uint16_t uid, uint8_t choice, const char *code,
                                      alarm_result_t *code_result)
{
    (void)ctx;
    alarm_result_t unused;
    if (code_result == NULL) {
        code_result = &unused;
    }
    *code_result = ALARM_ERR_BAD_ARG;
    if (!s_auto.ready || s_auto.busy) {
        return UI_AUTO_POFF_BUSY;
    }
    const auto_warning_t *w = NULL;
    for (size_t i = 0; i < s_auto.nwarn && w == NULL; i++) {
        w = s_auto.warn[i].uid == uid ? &s_auto.warn[i] : NULL;
    }
    if (w == NULL || w->state != AUTO_WARN_ON) {
        return AUTO_POFF_NO_WARNING;
    }
    if (code == NULL || code[0] == '\0' || strlen(code) > ALARM_CODE_MAX_LEN ||
        (choice != AUTO_PUT_OFF_HOUR && choice != AUTO_PUT_OFF_NIGHT)) {
        return AUTO_POFF_BAD_ARG;
    }
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_CODE_CHECK;
    cmd.partition = w->partition;
    memcpy(cmd.code, code, strlen(code) + 1u);
    alarm_cmd_result_t res;
    memset(&res, 0, sizeof(res));
    *code_result = alarm_engine_command(&H.eng, &cmd, &SRC, H.now, &res);
    H.dirty = true;
    after();
    if (*code_result != ALARM_OK) {
        return UI_AUTO_POFF_CODE;
    }
    auto_local_t lt;
    auto_local_now(&lt);
    const auto_put_off_t r =
        auto_run_put_off(&s_auto.run, uid, choice, res.user, res.may_disarm, &lt, H.now);
    after();
    return r;
}

void host_panel_auto_counts(host_panel_auto_counts_t *out)
{
    *out = s_auto.counts;
}

bool host_panel_automations_active(void)
{
    return s_auto.ready && s_auto.n > 0u;
}

static size_t be_dev_layout_get(void *ctx, uint8_t *out, size_t max)
{
    (void)ctx;
    if (s_dev.layout_len == 0 || s_dev.layout_len > max) {
        return 0;
    }
    memcpy(out, s_dev.layout, s_dev.layout_len);
    return s_dev.layout_len;
}

size_t host_panel_dev_layout(uint8_t *out, size_t max)
{
    return be_dev_layout_get(NULL, out, max);
}

static bool be_dev_layout_set(void *ctx, const uint8_t *blob, size_t len)
{
    (void)ctx;
    if (len == 0 || len > sizeof(s_dev.layout)) {
        return false;
    }
    memcpy(s_dev.layout, blob, len);
    s_dev.layout_len = len;
    return true;
}

static uint32_t be_lang_generation(void *ctx)
{
    (void)ctx;
    return s_lang_gen;
}

/* ---- Zigbee's pages: the real backend or the demo model (the walk keeps the demo's shots) ---- */

static ui_backend_t s_zb_demo_be;
static ui_backend_t s_zb_real_be;

static const ui_backend_t *zb_be(void)
{
    return s_zb_real ? &s_zb_real_be : &s_zb_demo_be;
}

static bool hp_zb_status(void *ctx, ui_zb_status_t *out)
{
    return zb_be()->zb_status(ctx, out);
}

static size_t hp_zb_devices(void *ctx, ui_zb_device_t *out, size_t max)
{
    return zb_be()->zb_devices(ctx, out, max);
}

static bool hp_zb_device(void *ctx, uint16_t id, ui_zb_device_page_t *out)
{
    return zb_be()->zb_device(ctx, id, out);
}

static ui_zb_req_result_t hp_zb_request(void *ctx, const ui_zb_request_t *req)
{
    const ui_zb_req_result_t r = zb_be()->zb_request(ctx, req);
    if (s_zb_real) {
        after(); /* what it logged and changed in the engine */
    }
    return r;
}

static void hp_zb_progress(void *ctx, ui_zb_progress_t *out)
{
    zb_be()->zb_progress(ctx, out);
}

static size_t hp_zb_codes(void *ctx, ui_zb_code_t *out, size_t max)
{
    return zb_be()->zb_codes(ctx, out, max);
}

void host_panel_backend(ui_backend_t *out)
{
    memset(out, 0, sizeof(*out));
    out->lang_catalog = be_lang_catalog;
    out->lang_list = be_lang_list;
    out->lang_generation = be_lang_generation;
    out->lang_font = be_lang_font;
    /* The asset packs' hooks on the PC: the letters pack's fonts from the layout's committed parts,
     * the picture pack's part `photo` as the embedded JPEG; the letters' match stays NULL
     * (docs/features/39 asset-packs.md "The panel"). */
    out->asset_font = be_asset_font;
    out->asset_generation = be_asset_generation;
    out->asset_picture = be_asset_picture;
    out->snapshot_seq = be_seq;
    out->get_snapshot = be_snapshot;
    out->get_config = be_config;
    out->recent_events = be_events;
    out->now_ms = be_now;
    out->epoch_ms = epoch_ms;
    out->command = be_command;
    out->change_code = be_change_code;
    out->net_state = be_net;
    out->authenticate = be_authenticate;
    out->program_enter = be_program_enter;
    out->program_exit = be_program_exit;
    out->program_keepalive = be_program_keepalive;
    out->program_set_code = be_program_set_code;
    out->set_config = be_set_config;
    out->user_set = be_user_set;
    out->user_update = be_user_update;
    out->user_delete = be_user_delete;
    out->log_read = be_log_read;
    out->boot_id = be_boot;
    out->log_loss = be_log_loss;
    out->log_clear_loss = be_log_clear_loss;
    out->wifi_info = be_wifi_info;
    out->power_info = be_power;
    out->wifi_scan = be_wifi_scan;
    out->wifi_results = be_wifi_results;
    out->wifi_connect = be_wifi_connect;
    out->ip_get = be_ip_get;
    out->ip_set = be_ip_set;
    out->net_check_start = be_net_check_start;
    out->net_check_status = be_net_check_status;
    out->get_timezone = be_get_tz;
    out->set_timezone = be_set_tz;
    out->get_ntp = be_get_ntp;
    out->set_ntp = be_set_ntp;
    out->clock_info = be_clock_info;
    out->clock_set = be_clock_set;
    out->save_options = be_save_options;
    out->prefs_remote_take = be_prefs_remote_take; /* as ui_port_esp_backend, in both roles */
    out->prefs_remote_done = be_prefs_remote_done;
    out->backlight = be_backlight;
    /* The rest follows the board's options (host_features.h), as the firmware's backend
     * does: remote access and browser sign-in come with the Panel Manager
     * (panel_web_ui_backend), the others with their feature (ui_port_esp_backend). */
    out->remote_get = CONFIG_ALARM_PANEL_WEB ? be_remote_get : NULL;
    out->remote_set = CONFIG_ALARM_PANEL_WEB ? be_remote_set : NULL;
    out->remote_pause = CONFIG_ALARM_PANEL_WEB ? be_remote_pause : NULL;
    out->remote_info = CONFIG_ALARM_PANEL_WEB ? be_remote_info : NULL;
    out->web_login_get = CONFIG_ALARM_PANEL_WEB ? be_web_login_get : NULL;
    out->web_login_set = CONFIG_ALARM_PANEL_WEB ? be_web_login_set : NULL;
    out->web_login_clear = CONFIG_ALARM_PANEL_WEB ? be_web_login_clear : NULL;
    out->beep = be_beep;
    out->set_volume = be_set_volume;
    out->set_chime_volume = be_set_chime_volume;
    out->set_voice_volume = be_set_voice_volume;
    out->about = be_about;
    out->storage_sd = CONFIG_ALARM_FEATURE_STORAGE ? be_storage_sd : NULL;
    out->storage_flash = CONFIG_ALARM_FEATURE_STORAGE ? be_storage_flash : NULL;
    out->storage_action = CONFIG_ALARM_FEATURE_STORAGE ? be_storage_action : NULL;
    out->setup_done = be_setup_done;
    out->factory_reset = be_factory_reset;
    out->mqtt_get = CONFIG_ALARM_FEATURE_MQTT ? be_mqtt_get : NULL;
    out->mqtt_set = CONFIG_ALARM_FEATURE_MQTT ? be_mqtt_set : NULL;
    out->mqtt_status = CONFIG_ALARM_FEATURE_MQTT ? be_mqtt_status : NULL;
    out->notify_status = CONFIG_ALARM_FEATURE_NOTIFY ? be_notify_status : NULL;
    out->notify_test = CONFIG_ALARM_FEATURE_NOTIFY ? be_notify_test : NULL;
    out->notify_test_result = CONFIG_ALARM_FEATURE_NOTIFY ? be_notify_test_result : NULL;
    out->ntfy_get = CONFIG_ALARM_FEATURE_NOTIFY ? be_ntfy_get : NULL;
    out->ntfy_set = CONFIG_ALARM_FEATURE_NOTIFY ? be_ntfy_set : NULL;
    out->ntfy_new_topic = CONFIG_ALARM_FEATURE_NOTIFY ? be_ntfy_new_topic : NULL;
    out->pushover_get = CONFIG_ALARM_FEATURE_NOTIFY ? be_pushover_get : NULL;
    out->pushover_set = CONFIG_ALARM_FEATURE_NOTIFY ? be_pushover_set : NULL;
    out->halink_status = CONFIG_ALARM_FEATURE_HA ? be_halink_status : NULL;
    out->halink_enable = CONFIG_ALARM_FEATURE_HA ? be_halink_enable : NULL;
    out->halink_pair_begin = CONFIG_ALARM_FEATURE_HA ? be_halink_pair_begin : NULL;
    out->halink_pair_decide = CONFIG_ALARM_FEATURE_HA ? be_halink_pair_decide : NULL;
    out->halink_pair_cancel = CONFIG_ALARM_FEATURE_HA ? be_halink_pair_cancel : NULL;
    out->halink_unpair = CONFIG_ALARM_FEATURE_HA ? be_halink_unpair : NULL;
    out->fw_status = be_fw_status;
    out->fw_check = be_fw_check;
    out->fw_install = be_fw_install;
    out->fw_stop = be_fw_stop;
    out->fw_server = be_fw_server;
    out->fw_server_check = be_fw_server_check;
    out->fw_server_check_get = be_fw_server_check_get;
    out->fw_server_check_forget = be_fw_server_check_forget;
    out->fw_server_commit = be_fw_server_commit;
    out->fw_set_channel = be_fw_set_channel;
    out->zone_sources = be_zone_sources;
    out->zone_learn = be_zone_learn;
    out->zone_candidates = be_zone_candidates;
    out->zone_bind = be_zone_bind;
    out->zone_unbind = be_zone_unbind;
    out->zone_info = be_zone_info;
    out->dev_list = CONFIG_ALARM_HOME_CONTROL ? be_dev_list : NULL;
    out->dev_act = CONFIG_ALARM_HOME_CONTROL ? be_dev_act : NULL;
    out->dev_layout_get = CONFIG_ALARM_HOME_CONTROL ? be_dev_layout_get : NULL;
    out->dev_layout_set = CONFIG_ALARM_HOME_CONTROL ? be_dev_layout_set : NULL;
    /* the automations come with their feature (ui_port_esp_backend: automation_service) */
    out->auto_list = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_list : NULL;
    out->auto_gen = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_gen : NULL;
    out->auto_read_only = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_read_only : NULL;
    out->auto_less_secure = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_less_secure : NULL;
    out->auto_save = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_save : NULL;
    out->auto_delete_slot = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_delete_slot : NULL;
    out->auto_set_off = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_set_off : NULL;
    out->auto_status = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_status : NULL;
    out->auto_try = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_try : NULL;
    out->auto_warnings = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_warnings : NULL;
    out->auto_put_off = CONFIG_ALARM_FEATURE_AUTOMATIONS ? be_auto_put_off : NULL;
    /* the voice comes with its feature (ui_port_esp_backend: panel_voice) */
    out->voice_info = CONFIG_ALARM_FEATURE_VOICE ? be_voice_info : NULL;
    out->voice_pack_acquire = CONFIG_ALARM_FEATURE_VOICE ? be_voice_pack_acquire : NULL;
    out->voice_pack_release = CONFIG_ALARM_FEATURE_VOICE ? be_voice_pack_release : NULL;
    out->voice_preview = CONFIG_ALARM_FEATURE_VOICE ? be_voice_preview : NULL;
    out->voice_seen = CONFIG_ALARM_FEATURE_VOICE ? be_voice_seen : NULL;
    out->voice_apply_approve = CONFIG_ALARM_FEATURE_VOICE ? be_voice_apply_approve : NULL;
    out->voice_preview_stop = CONFIG_ALARM_FEATURE_VOICE ? be_voice_preview_stop : NULL;
    if (CONFIG_ALARM_FEATURE_VOICE) {
        voice_load(); /* its log line once, at start */
    }
    /* link_*: the panel link's demo model (BL-048), off (answering as a firmware that hasn't said)
     * until the walk turns it on */
    host_link_demo_backend(out);
    /* zb_*: the real Zigbee or the demo model, as host_panel_zigbee_use_real() chose */
    host_zigbee_demo_backend(&s_zb_demo_be);
    host_zigbee_real_backend(&s_zb_real_be);
    out->zb_status = hp_zb_status;
    out->zb_devices = hp_zb_devices;
    out->zb_device = hp_zb_device;
    out->zb_request = hp_zb_request;
    out->zb_progress = hp_zb_progress;
    out->zb_codes = hp_zb_codes;
#if ALARM_SIM_ZWAVE
    out->zw_status = be_zw_status; /* false until the world runs */
    out->zw_devices = be_zw_devices;
    out->zw_device = be_zw_device;
    out->zw_request = be_zw_request;
    out->zw_progress = be_zw_progress;
    out->zw_prov = be_zw_prov;
    out->zw_prov_register = be_zw_prov_register;
    out->zw_prov_change = be_zw_prov_change;
#endif
}
