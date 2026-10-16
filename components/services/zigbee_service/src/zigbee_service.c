/*
 * zigbee_service.c - the sensor radio service (docs/features/20): the ESP32-H2 in the wireless
 * socket as a zone source, and since ZB-M6 (BL-143) the panel's Zigbee devices.
 *
 * The portable source and the bring-up policy are host-tested; radio_port.c is the hardware
 * seam (the socket's UART and Espressif's Zigbee stack as coordinator). CONFIG_ALARM_ZSRC_ZIGBEE
 * is on in every P4 image (ADR-047); an empty socket never starts the stack (the probe), and a
 * socket the Z-Wave radio holds is refused at run time. Writing the radio's flash from the panel
 * is not built yet: its first image goes on over the module's own USB-C
 * (firmware/radios/h2_rcp/README.md).
 *
 * The zigbee task owns zigbee_zones' zbz_panel (devices.md; the way zwave_service owns zwz_panel):
 * the registry and the install codes, each device's setup, the operations the pages, the console
 * and the developer link ask for, the sirens and keypads. Every call into it - the radio's
 * reports (radio_port_pump hands them to zbz_panel_on_report with the zone lock held), its polls,
 * the requests, the engine's news - is made with the zone service's lock held, because the panel
 * feeds the Zigbee zone source the zone task ticks too; the readers (the pages, MQTT, the console)
 * take the same lock from their own tasks. After unlocking the task writes NVS "zbpanel" (the
 * take calls: it is the task's stack, internal RAM, that writes - docs/bugs/B001), logs the
 * panel's events through the alarm task and runs the keypads' commands there.
 *
 * Nothing here runs on the alarm task but the two calls the start-up's listener hands on
 * (zigbee_service_engine_output, _snapshot), which only copy the engine's outputs and partition
 * views into mailboxes the zigbee task empties.
 */
#include "zigbee_service.h"
#include "zigbee_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static atomic_bool s_socket_zwave;

void zigbee_service_set_socket_taken(bool by_zwave)
{
    atomic_store(&s_socket_zwave, by_zwave);
}

#define SOCKET_ZWAVE_WORDS "Sensor radio off: the socket is used by the Z-Wave radio"

#if CONFIG_ALARM_ZSRC_ZIGBEE

#include <sys/time.h>
#include <time.h>

#include "alarm/alarm_strings.h"
#include "alarm_proto/proto_zb.h"
#include "alarm_service.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_port.h"
#include "zone_service.h"

static const char *TAG = "zigbee";

/* Internal RAM: it writes NVS (docs/bugs/B001). 8 KB, Z-Wave's: zbz_panel_on_report copies a
 * record (about 300 bytes) and the store takes a 256-byte record at a time; `zigbee status`
 * shows the high-water mark the bench reads (devices.md §16). */
#define ZB_TASK_STACK   8192
#define ZB_MAX_WAIT_MS  5000u
#define ZB_UP_WAIT_MS   1000u /* with the network up: the status refresh; radio events wake us */
#define ZB_WALK_SCOPE_MS 30000u /* a running walk test's scope asked again (the zones' bindings) */
#define REQUEST_MS      2000u /* a page's request waits this long for the zigbee task */
#define STORE_RETRY_MS  30000u /* NVS refused a write: try again this much later */
#define LOAD_RETRY_MS   2000u  /* NVS could not be read at start: the first retry, then doubled */
#define LOAD_TRIES_MAX  6u     /* reads again after it, then only `zigbee retry` or a restart */
#define ZB_LOST_HOLD_MS 300000u /* a lost alarm-path event keeps its trouble this long */
#define OUT_QUEUE_LEN   16u   /* the engine's bell and strobe changes on their way */

/* ---- decided contracts, checked by the compiler --------------------------------------------- */

/* The log (devices.md §11): zbz_event_t 1..9 are ALARM_EV_ZIGBEE_* in the same order. */
static const alarm_event_type_t ZB_EVENTS[] = {
    [ZBZ_EV_ADDED] = ALARM_EV_ZIGBEE_ADDED,
    [ZBZ_EV_REMOVED] = ALARM_EV_ZIGBEE_REMOVED,
    [ZBZ_EV_FORGOTTEN] = ALARM_EV_ZIGBEE_FORGOTTEN,
    [ZBZ_EV_LEFT] = ALARM_EV_ZIGBEE_LEFT,
    [ZBZ_EV_REPLACED] = ALARM_EV_ZIGBEE_REPLACED,
    [ZBZ_EV_JOIN_REFUSED] = ALARM_EV_ZIGBEE_JOIN_REFUSED,
    [ZBZ_EV_CHANNEL] = ALARM_EV_ZIGBEE_CHANNEL,
    [ZBZ_EV_NETWORK_RESET] = ALARM_EV_ZIGBEE_NETWORK_RESET,
    [ZBZ_EV_RADIO_RESTARTED] = ALARM_EV_ZIGBEE_RADIO_RESTARTED,
};
_Static_assert(sizeof(ZB_EVENTS) / sizeof(ZB_EVENTS[0]) == (size_t)ZBZ_EV_RADIO_RESTARTED + 1u,
               "every zbz_event_t has its ALARM_EV_ZIGBEE_*");
_Static_assert((int)ALARM_EV_ZIGBEE_RADIO_RESTARTED - (int)ALARM_EV_ZIGBEE_ADDED ==
                   (int)ZBZ_EV_RADIO_RESTARTED - (int)ZBZ_EV_ADDED,
               "ALARM_EV_ZIGBEE_* run as zbz_event_t does");
/* ADDED's arg2 is the record's join, a zsrc_zb_join_t: alarm_zb_join_t has the same numbers. */
_Static_assert((int)ALARM_ZB_JOIN_UNKNOWN == (int)ZSRC_ZB_JOIN_UNKNOWN &&
                   (int)ALARM_ZB_JOIN_INSTALL_CODE == (int)ZSRC_ZB_JOIN_INSTALL_CODE &&
                   (int)ALARM_ZB_JOIN_STANDARD_KEY == (int)ZSRC_ZB_JOIN_STANDARD_KEY &&
                   (int)ALARM_ZB_JOIN_COUNT == (int)ZSRC_ZB_JOIN_STANDARD_KEY + 1,
               "alarm_zb_join_t matches zsrc_zb_join_t");
/* ... and the pages' ui_zb_join_t. */
_Static_assert((int)UI_ZB_JOIN_UNKNOWN == (int)ZSRC_ZB_JOIN_UNKNOWN &&
                   (int)UI_ZB_JOIN_INSTALL_CODE == (int)ZSRC_ZB_JOIN_INSTALL_CODE &&
                   (int)UI_ZB_JOIN_STANDARD_KEY == (int)ZSRC_ZB_JOIN_STANDARD_KEY,
               "ui_zb_join_t matches zsrc_zb_join_t");
/* JOIN_REFUSED's arg2 is the radio's u.refused.why (zsrc_zigbee.h: 0 not said, 1 no install code
 * matched while codes are required, 2 an unknown device's rejoin); RADIO_RESTARTED's is
 * zbz_panel's 0 asked, 1 by itself, 2 the guard paused it; CHANNEL's the channel. */
_Static_assert(ALARM_ZB_REFUSED_NOT_SAID == 0 && ALARM_ZB_REFUSED_NO_CODE == 1 &&
                   ALARM_ZB_REFUSED_REJOIN == 2 && ALARM_ZB_REFUSED_COUNT == 3,
               "alarm_zb_refused_t matches the radio's u.refused.why");
_Static_assert(ALARM_ZB_RESTART_ASKED == 0 && ALARM_ZB_RESTART_BY_ITSELF == 1 &&
                   ALARM_ZB_RESTART_PAUSED == 2 && ALARM_ZB_RESTART_COUNT == 3,
               "alarm_zb_restart_t matches zbz_panel's RADIO_RESTARTED arg");
_Static_assert(ALARM_ZB_CHANNEL_MIN == 11u && ALARM_ZB_CHANNEL_MAX == 26u,
               "ALARM_EV_ZIGBEE_CHANNEL carries an IEEE 802.15.4 channel");
/* What MQTT gets (proto_zb.h, through panel_app and net_mqtt) is ui_backend's numbers passed
 * straight through: the radio's state, a zone's signal bucket and the unknown battery. */
_Static_assert((int)UI_ZB_RADIO_NOT_CONNECTED == (int)PROTO_ZB_RADIO_NOT_CONNECTED &&
                   (int)UI_ZB_RADIO_STARTING == (int)PROTO_ZB_RADIO_STARTING &&
                   (int)UI_ZB_RADIO_WORKING == (int)PROTO_ZB_RADIO_WORKING &&
                   (int)UI_ZB_RADIO_NOT_ANSWERING == (int)PROTO_ZB_RADIO_NOT_ANSWERING &&
                   (int)UI_ZB_RADIO_PAUSED == (int)PROTO_ZB_RADIO_PAUSED &&
                   (int)UI_ZB_RADIO_SOCKET_ZWAVE == (int)PROTO_ZB_RADIO_SOCKET_ZWAVE &&
                   (int)UI_ZB_RADIO_UNUSABLE == (int)PROTO_ZB_RADIO_UNUSABLE &&
                   (int)PROTO_ZB_RADIO_COUNT == (int)PROTO_ZB_RADIO_UNUSABLE + 1,
               "ui_zb_radio_t numbers its states as proto_zb_radio_t does");
_Static_assert((int)UI_ZB_SIGNAL_UNKNOWN == (int)PROTO_SIGNAL_UNKNOWN &&
                   (int)UI_ZB_SIGNAL_GOOD == (int)PROTO_SIGNAL_GOOD &&
                   (int)UI_ZB_SIGNAL_FAIR == (int)PROTO_SIGNAL_FAIR &&
                   (int)UI_ZB_SIGNAL_WEAK == (int)PROTO_SIGNAL_WEAK &&
                   (int)PROTO_SIGNAL_COUNT == (int)PROTO_SIGNAL_WEAK + 1,
               "ui_zb_signal_t numbers its buckets as proto_signal_t does");
_Static_assert(UI_ZB_BATTERY_UNKNOWN == PROTO_BATTERY_UNKNOWN,
               "an unknown battery is the same number on the pages and on MQTT");

/* ---- the service's state ------------------------------------------------------------------- */

/* The source (about 5 KB) and the panel (about 20 KB, devices.md §16): tasks only, nothing an ISR
 * or DMA touches, so PSRAM. */
static EXT_RAM_BSS_ATTR zsrc_zb_t s_src;
static EXT_RAM_BSS_ATTR zbz_panel_t s_panel;
static zb_boot_t s_boot;
static TaskHandle_t s_task;
static bool s_started;
static atomic_bool s_stack_up;
static atomic_bool s_retry;
static atomic_bool s_restart; /* a page's RESTART: stop the running stack, the task brings it up */
static bool s_ever_answered;  /* the radio answered a probe since the panel started */
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static zigbee_service_status_t s_status;
static zbz_radio_info_t s_info_told; /* what zbz_panel_radio was last told (the task's) */
static bool s_info_valid;
static EXT_RAM_BSS_ATTR zbsvc_stats_t s_stats;
static atomic_uint s_changed; /* zbz_panel's "something a page shows changed" */

/* The requests (the pages, the console, the developer link), one at a time. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_req_lock;
static SemaphoreHandle_t s_req_done;
static EXT_RAM_BSS_ATTR struct {
    bool pending;
    uint32_t seq, done_seq;
    ui_zb_request_t req;
    bool allowed;
    ui_zb_req_result_t result;
} s_rq;

/* The store. */
static uint64_t s_store_retry_at;
static bool s_storage_trouble; /* its last store pass failed */
/* ZB33: what is stored has been read (a new panel's nothing counts). Until then the radio stays
 * stopped, nothing is written over what NVS holds, and the read is tried again. */
static bool s_loaded;
static unsigned s_load_tries;
static uint64_t s_load_retry_at;
static bool s_trouble_held; /* ALARM_STORAGE_ZIGBEE raised (tell_storage_trouble) */
static EXT_RAM_BSS_ATTR uint8_t s_blob[ZBZ_CODES_BLOB_MAX > ZBA_BLOB_MAX ? ZBZ_CODES_BLOB_MAX
                                                                         : ZBA_BLOB_MAX];
static EXT_RAM_BSS_ATTR uint8_t s_record[ZBZ_RECORD_MAX];

/* The engine's news (the alarm task fills them; the zigbee task empties them). */
typedef struct {
    uint8_t partition;
    uint8_t out;     /* alarm_output_t */
    uint8_t pattern; /* alarm_pattern_t */
} out_msg_t;
static QueueHandle_t s_outq;
static atomic_bool s_engine_ready; /* s_outq made: the engine's news is taken */
static atomic_bool s_out_resync; /* the queue was full: replay the latest of each */
static uint8_t s_out_latest[ALARM_MAX_PARTITIONS][2]; /* bell pattern, strobe pattern; s_mux */
static zba_view_t s_views[ALARM_MAX_PARTITIONS];      /* s_mux */
static zba_view_t s_views_told[ALARM_MAX_PARTITIONS]; /* the task's */
static uint8_t s_views_told_known;                    /* the task's: bit p-1, s_views_told fed */
static uint8_t s_views_dirty;                         /* s_mux: bit p-1 */
static uint8_t s_views_seen;                          /* s_mux: bit p-1, a snapshot told it */
static bool s_walk;                                   /* s_mux: a walk test runs */
static bool s_walk_told;
static bool s_walk_scope_on;       /* refresh_walk_scope: what the source was last told */
static uint32_t s_walk_scope_seq;
static uint64_t s_walk_scope_at;
static uint64_t s_fob_users;       /* s_mux: users a fob may act as (bit u-1), refresh_taken */
static atomic_uint s_cfg_seq;                         /* the engine's configuration counter */
static uint32_t s_cfg_seen;
static atomic_bool s_cfg_known;
static atomic_uint s_taken; /* engine device numbers held elsewhere (bit n-1), zbz's port */
static atomic_uint s_link_taken; /* the panel link's keypads' numbers (bit n-1), LINK-001 */

/* The siren trouble (zbw_port_t.bell_trouble): wanted under the zone lock, told after it. */
static atomic_int s_bell_want = -1;
static int s_bell_told = -1;

/* The keypads' and fobs' commands for the alarm task (zbsvc_ace_post): the zigbee task's only. */
static EXT_RAM_BSS_ATTR struct {
    bool used;
    zba_post_t post;
} s_ace[ZBA_PENDING_MAX];

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* Erase a copy that held a code: never a plain memset the compiler may drop. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n-- > 0u) {
        *b++ = 0u;
    }
}

/* ---- what the source and the panel ask the radio to do ------------------------------------- */

static bool radio_permit_join(void *ctx, uint16_t seconds)
{
    (void)ctx;
    return s_stack_up && radio_port_permit_join(seconds);
}

static bool radio_cie_write(void *ctx, uint64_t ieee, uint8_t endpoint)
{
    (void)ctx;
    zbsvc_trace_out("cie_write", ieee, endpoint, 0u, 0u);
    return s_stack_up && radio_port_ias_cie_write(ieee, endpoint);
}

static bool radio_enroll_response(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t zone_id)
{
    (void)ctx;
    zbsvc_trace_out("enroll_response", ieee, endpoint, zone_id, 0u);
    return s_stack_up && radio_port_ias_enroll_response(ieee, endpoint, zone_id);
}

static bool radio_read_attribute(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cluster,
                                 uint16_t attr)
{
    (void)ctx;
    zbsvc_trace_out("read_attribute", ieee, ep, ((uint32_t)cluster << 16) | attr, 0u);
    return s_stack_up && radio_port_read_attribute(ieee, ep, cluster, attr);
}

static bool radio_remove_device(void *ctx, uint64_t ieee)
{
    (void)ctx;
    return s_stack_up && radio_port_remove_device(ieee);
}

static bool radio_energy_scan(void *ctx)
{
    (void)ctx;
    return s_stack_up && radio_port_energy_scan();
}

/* BL-143: the hooks appended to zsrc_zb_radio_t (devices.md §3), each straight to radio_port. */

static bool radio_node_desc_req(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("node_desc_req", ieee, 0u, nwk, tag);
    return s_stack_up && radio_port_node_desc_req(ieee, nwk, tag);
}

static bool radio_power_desc_req(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("power_desc_req", ieee, 0u, nwk, tag);
    return s_stack_up && radio_port_power_desc_req(ieee, nwk, tag);
}

static bool radio_active_ep_req(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("active_ep_req", ieee, 0u, nwk, tag);
    return s_stack_up && radio_port_active_ep_req(ieee, nwk, tag);
}

static bool radio_simple_desc_req(void *ctx, uint64_t ieee, uint16_t nwk, uint8_t ep, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("simple_desc_req", ieee, ep, nwk, tag);
    return s_stack_up && radio_port_simple_desc_req(ieee, nwk, ep, tag);
}

static bool radio_read_attributes(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cluster,
                                  const uint16_t *attrs, uint8_t count, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("read_attributes", ieee, ep, ((uint32_t)cluster << 16) | count, tag);
    return s_stack_up && radio_port_read_attributes(ieee, ep, cluster, attrs, count, tag);
}

static bool radio_bind(void *ctx, uint64_t ieee, uint8_t ep, uint16_t cluster, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("bind", ieee, ep, cluster, tag);
    return s_stack_up && radio_port_bind(ieee, ep, cluster, tag);
}

static bool radio_configure_reporting(void *ctx, uint64_t ieee, uint8_t ep,
                                      const zsrc_zb_reporting_t *r, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("configure_reporting", ieee, ep, ((uint32_t)r->cluster << 16) | r->attr, tag);
    return s_stack_up && radio_port_configure_reporting(ieee, ep, r, tag);
}

static bool radio_leave(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("leave", ieee, 0u, nwk, tag);
    return s_stack_up && radio_port_leave(ieee, nwk, tag);
}

static bool radio_forget(void *ctx, uint64_t ieee)
{
    (void)ctx;
    zbsvc_trace_out("forget", ieee, 0u, 0u, 0u);
    return s_stack_up && radio_port_forget(ieee);
}

static bool radio_identify(void *ctx, uint64_t ieee, uint8_t ep, uint16_t seconds, uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("identify", ieee, ep, seconds, tag);
    return s_stack_up && radio_port_identify(ieee, ep, seconds, tag);
}

static bool radio_ias_test_mode(void *ctx, uint64_t ieee, uint8_t ep, uint8_t seconds,
                                uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_out("ias_test_mode", ieee, ep, seconds, tag);
    return s_stack_up && radio_port_ias_test_mode(ieee, ep, seconds, tag);
}

static bool radio_zcl_command(void *ctx, uint64_t ieee, uint8_t ep, const zsrc_zb_zcl_t *cmd,
                              uint32_t tag)
{
    (void)ctx;
    zbsvc_trace_zcl('>', ieee, ep, cmd, tag);
    return s_stack_up && radio_port_zcl_command(ieee, ep, cmd, tag);
}

static bool radio_install_code(void *ctx, uint64_t ieee, const uint8_t *code, uint8_t len)
{
    (void)ctx;
    return s_stack_up && radio_port_install_code_set(ieee, code, len);
}

static bool radio_codes_only(void *ctx, bool on)
{
    (void)ctx;
    return s_stack_up && radio_port_codes_only(on);
}

static bool radio_set_channel(void *ctx, uint8_t channel, uint32_t tag)
{
    (void)ctx;
    return s_stack_up && radio_port_set_channel(channel, tag);
}

static bool radio_reset_network(void *ctx, uint32_t tag)
{
    (void)ctx;
    return s_stack_up && radio_port_reset_network(tag);
}

static bool radio_channel_survey(void *ctx, uint32_t tag)
{
    (void)ctx;
    return s_stack_up && radio_port_channel_survey(tag);
}

/* ---- zbz_panel's ports (called in the zigbee task, the zone lock held) ---------------------- */

static bool port_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    return zone_service_get(zone, out); /* the zone lock is recursive */
}

static bool port_rebind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    /* ZB-013, RF32: only under the claim zbz_panel_replace_gated holds (zone_service refuses it
     * otherwise); stored when the zone lock goes (from this task's stack). */
    return zone_service_bind(zone, b) == ZONE_HUB_OK;
}

/* The wall clock once it is set - from 2025 on, the touch UI's own rule - else 0. */
static uint32_t port_wall_clock(void *ctx)
{
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec < 1735689600LL ? 0u : (uint32_t)tv.tv_sec;
}

static void port_changed(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_changed, 1u);
}

/* From the copy refresh_taken() keeps and the panel link's numbers: never waits. */
static uint32_t port_devices_taken(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_taken) | atomic_load(&s_link_taken);
}

/* A siren's or a keypad's frame (zbw_port_t.send, zba_port_t.send). */
static bool port_send(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                      uint32_t tag)
{
    return radio_zcl_command(ctx, ieee, endpoint, cmd, tag);
}

/* The bell trouble (ZB-023): posted after the zone lock is let go (post may wait for the alarm
 * task's queue). */
static void port_bell_trouble(void *ctx, bool active)
{
    (void)ctx;
    atomic_store(&s_bell_want, active ? 1 : 0);
}

/* A keypad's refusal (zba_port_t.default_rsp): as the IAS ACE server, to the client. */
static bool port_default_rsp(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t tsn,
                             uint8_t command, uint8_t status, uint32_t tag)
{
    (void)ctx;
    return s_stack_up && radio_port_zcl_default_rsp(ieee, endpoint, ZSRC_ZB_ZCL_IAS_ACE, true, tsn,
                                                    command, status, tag);
}

/* The radio side's reports, from radio_port_pump with the zone lock held. */
static void panel_report(void *ctx, uint64_t now, const zsrc_zb_report_t *r)
{
    (void)ctx;
    zbsvc_trace_report(now, r);
    zbsvc_sig_capture(r); /* `zigbee device <id> signature` */
    zbz_panel_on_report(&s_panel, now, r);
}

/* ---- NVS "zbpanel" (devices.md §4) --------------------------------------------------------- */

/* A load's NVS: the open namespace, and the first read that failed for any reason but an absent
 * key (ZB33) - a key NVS could not read is not a key that isn't there. */
typedef struct {
    nvs_handle_t h;
    esp_err_t err;
    char key[NVS_KEY_NAME_MAX_SIZE];
} store_ctx_t;

static void store_failed(store_ctx_t *c, const char *key, esp_err_t err)
{
    if (c->err == ESP_OK) {
        c->err = err;
        snprintf(c->key, sizeof(c->key), "%s", key);
    }
}

static size_t store_read(void *ctx, const char *key, uint8_t *buf, size_t cap)
{
    store_ctx_t *c = (store_ctx_t *)ctx;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(c->h, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0u; /* never stored */
    }
    if (err != ESP_OK) {
        store_failed(c, key, err);
        return 0u;
    }
    if (len == 0u) {
        return 0u;
    }
    if (len > cap) {
        return len; /* too long: zbz drops it as damaged, nothing read */
    }
    err = nvs_get_blob(c->h, key, buf, &len);
    if (err != ESP_OK) {
        store_failed(c, key, err);
        return 0u;
    }
    return len;
}

/* ZW-039: Zigbee's own hold on the storage trouble - while its store passes fail, and (ZB33)
 * while what it stored could not be read. Told from the zigbee task. */
static void tell_storage_trouble(void)
{
    const bool want = s_storage_trouble || !s_loaded;
    if (want != s_trouble_held) {
        s_trouble_held = want;
        alarm_service_storage_trouble(ALARM_STORAGE_ZIGBEE, want);
    }
}

/* Entries NVS can still give out (zbz_panel_nvs_free); UINT32_MAX when it can't say. */
static uint32_t nvs_free_entries(void)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return ZBZ_NVS_NOT_TOLD;
    }
    return st.available_entries > UINT32_MAX - 1u ? UINT32_MAX - 1u
                                                  : (uint32_t)st.available_entries;
}

/* The budget as NVS has it now, to the panel (the zone lock taken here). */
static void tell_nvs_free(void)
{
    const uint32_t entries = nvs_free_entries();
    s_stats.nvs_free = entries;
    zone_service_lock();
    zbz_panel_nvs_free(&s_panel, entries);
    zone_service_unlock();
}

static esp_err_t write_blob(nvs_handle_t h, const char *key, const uint8_t *buf, size_t len)
{
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    return err;
}

/* One of the values beside the registry: taken under the lock, written after it. */
typedef size_t (*take_fn)(zbz_panel_t *p, uint8_t *buf, size_t cap);

static uint8_t store_one(nvs_handle_t h, uint8_t dirty, uint8_t bit, take_fn take, const char *key)
{
    if ((dirty & bit) == 0u) {
        return 0u;
    }
    zone_service_lock();
    const size_t n = take(&s_panel, s_blob, sizeof(s_blob));
    zone_service_unlock();
    uint8_t failed = 0u;
    if (n == 0u) {
        /* nothing due after all, or it didn't fit its buffer: the bit stays when it didn't */
        zone_service_lock();
        failed = (uint8_t)(zbz_panel_dirty(&s_panel) & bit);
        zone_service_unlock();
    } else if (write_blob(h, key, s_blob, n) != ESP_OK) {
        failed = bit;
    }
    wipe(s_blob, sizeof(s_blob)); /* install codes pass through it */
    return failed;
}

/* What changed, to NVS - after unlocking, from this task (its stack is internal RAM): the
 * registry's keys one by one (meta last, as zbz_reg_take orders them), then codes, cfg, wd, ace.
 * A write that failed is asked for again (zbz_panel_redo) in STORE_RETRY_MS. */
static void persist(void)
{
    if (!s_loaded) {
        return; /* ZB33: what NVS holds could not be read: never written over with less */
    }
    if (s_store_retry_at != 0u && now_ms() < s_store_retry_at) {
        return;
    }
    zone_service_lock();
    const uint8_t dirty = zbz_panel_dirty(&s_panel);
    zone_service_unlock();
    if (dirty == 0u) {
        return;
    }
    nvs_handle_t h;
    uint8_t failed = 0u;
    if (nvs_open(ZBZ_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        failed = dirty;
    } else {
        for (unsigned guard = 0; (dirty & ZBZ_DIRTY_REGISTRY) != 0u && guard <= ZBZ_DEV_MAX + 1u;
             guard++) {
            char key[ZBZ_KEY_LEN];
            size_t len = 0u;
            zone_service_lock();
            const zbz_take_t t = zbz_panel_take_registry(&s_panel, key, s_record,
                                                         sizeof(s_record), &len);
            zone_service_unlock();
            if (t == ZBZ_TAKE_NONE) {
                break;
            }
            esp_err_t err = ESP_OK;
            if (t == ZBZ_TAKE_WRITE) {
                err = write_blob(h, key, s_record, len);
            } else {
                err = nvs_erase_key(h, key);
                if (err == ESP_ERR_NVS_NOT_FOUND) {
                    err = ESP_OK;
                }
                if (err == ESP_OK) {
                    err = nvs_commit(h);
                }
            }
            if (err != ESP_OK) {
                failed |= ZBZ_DIRTY_REGISTRY;
            }
        }
        failed |= store_one(h, dirty, ZBZ_DIRTY_CODES, zbz_panel_take_codes, ZBZ_KEY_CODES);
        failed |= store_one(h, dirty, ZBZ_DIRTY_CFG, zbz_panel_take_cfg, ZBZ_CFG_KEY);
        failed |= store_one(h, dirty, ZBZ_DIRTY_WD, zbz_panel_take_wd, ZBW_KEY);
        failed |= store_one(h, dirty, ZBZ_DIRTY_ACE, zbz_panel_take_ace, ZBA_KEY);
        nvs_close(h);
    }
    s_stats.stores++;
    if (failed != 0u) {
        zone_service_lock();
        zbz_panel_redo(&s_panel, failed);
        zone_service_unlock();
        s_stats.store_failures++;
        s_store_retry_at = now_ms() + STORE_RETRY_MS;
        ESP_LOGE(TAG, "storing the Zigbee devices failed (0x%02x): tried again in %u s",
                 (unsigned)failed, (unsigned)(STORE_RETRY_MS / 1000u));
        s_storage_trouble = true; /* ZW-039: Zigbee's hold, while its passes fail */
    } else {
        s_store_retry_at = 0u;
        s_storage_trouble = false; /* the first pass where everything stored */
    }
    tell_storage_trouble();
    tell_nvs_free(); /* after each write (devices.md §4) */
}

/* ---- the log (devices.md §11) -------------------------------------------------------------- */

/* The panel's events into the log - after unlocking: it waits for the alarm task. As the local
 * touchscreen when someone asked, else as the panel itself. */
static void log_events(void)
{
    zbz_log_t logs[ZBZ_LOG_MAX];
    size_t n = 0u;
    zone_service_lock();
    while (n < ZBZ_LOG_MAX && zbz_panel_take_log(&s_panel, &logs[n])) {
        n++;
    }
    zone_service_unlock();
    for (size_t i = 0; i < n; i++) {
        const zbz_log_t *l = &logs[i];
        if (l->type == 0u || l->type > (uint8_t)ZBZ_EV_RADIO_RESTARTED) {
            ESP_LOGW(TAG, "event %u has no log type: not logged", (unsigned)l->type);
            continue;
        }
        const alarm_event_type_t type = ZB_EVENTS[l->type];
        /* Who asked, and where: the touchscreen (or the console) or the Panel Manager (ZB-019). */
        const alarm_source_t src =
            l->user == 0u ? (alarm_source_t){.kind = ALARM_SRC_SYSTEM, .id = 0}
            : l->via == UI_ZB_VIA_WEB
                ? (alarm_source_t){.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_PANEL_MANAGER}
                : (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
        /* arg the registry id (0 over 255: "A device"), arg2 the type's own value */
        const uint8_t arg = l->device <= UINT8_MAX ? (uint8_t)l->device : 0u;
        const esp_err_t err = alarm_service_platform_event(type, l->zone, arg, l->arg, &src,
                                                           l->user);
        s_stats.events++;
        if (err != ESP_OK) {
            s_stats.events_failed++;
            ESP_LOGE(TAG, "event %s not logged (%s)", alarm_event_name(type), esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "%s: device %u zone %u (%u)", alarm_event_name(type),
                     (unsigned)l->device, (unsigned)l->zone, (unsigned)l->arg);
        }
    }
}

/* ---- the engine's news (from the alarm task; never blocks, never takes the zone lock) ------- */

/* The engine's listener slots were full when this was written (alarm_service.c MAX_LISTENERS, 8
 * then: the start-up's six, the Panel Manager's sign-ins, Z-Wave's; 12 now, the automations' the
 * 9th), so the start-up's own listener hands these two on (panel_app.c on_output, on_snapshot);
 * recorded in devices.md §16. */
void zigbee_service_engine_output(alarm_output_t output, uint8_t partition,
                                  alarm_pattern_t pattern)
{
    if (!atomic_load(&s_engine_ready)) {
        return;
    }
    if ((output != ALARM_OUT_BELL && output != ALARM_OUT_STROBE) || partition == 0u ||
        partition > ALARM_MAX_PARTITIONS) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_out_latest[partition - 1u][output == ALARM_OUT_BELL ? 0 : 1] = (uint8_t)pattern;
    taskEXIT_CRITICAL(&s_mux);
    const out_msg_t m = {.partition = partition, .out = (uint8_t)output, .pattern = (uint8_t)pattern};
    if (xQueueSend(s_outq, &m, 0) != pdTRUE) {
        atomic_store(&s_out_resync, true); /* the latest of each is replayed instead */
    }
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* The cause a keypad shows for an alarm (zba_view_t.cause): the most urgent in its memory. */
static uint8_t cause_of(uint16_t memory)
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

static void view_of(const alarm_partition_status_t *st, uint64_t at, zba_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->state = (uint8_t)st->state;
    v->level = (uint8_t)st->level;
    v->ready = st->ready;
    v->delay_s = (uint16_t)((st->delay_remaining_ms + 999u) / 1000u);
    v->at = at;
    v->in_alarm = st->bell != ALARM_PAT_OFF ||
                  (st->triggered && st->state != ALARM_STATE_DISARMED);
    v->cause = v->in_alarm ? cause_of((uint16_t)(st->memory & ~ALARM_MEM_CANCELED))
                           : (uint8_t)ALARM_CAUSE_NONE;
}

void zigbee_service_engine_snapshot(const alarm_snapshot_t *snap)
{
    if (!atomic_load(&s_engine_ready) || snap == NULL) {
        return;
    }
    const uint64_t at = now_ms();
    zba_view_t views[ALARM_MAX_PARTITIONS];
    bool walk = false;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        view_of(&snap->part[p], at, &views[p]);
        walk = walk || ((snap->partitions & (1u << p)) != 0u && snap->part[p].walk_test);
    }
    taskENTER_CRITICAL(&s_mux);
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        s_views[p] = views[p];
        if ((snap->partitions & (1u << p)) != 0u) {
            s_views_dirty = (uint8_t)(s_views_dirty | (1u << p));
            s_views_seen = (uint8_t)(s_views_seen | (1u << p));
        }
    }
    s_walk = walk;
    taskEXIT_CRITICAL(&s_mux);
    atomic_store(&s_cfg_seq, snap->config_seq);
    atomic_store(&s_cfg_known, true);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static bool view_differs(const zba_view_t *a, const zba_view_t *b)
{
    return a->state != b->state || a->level != b->level || a->ready != b->ready ||
           a->delay_s != b->delay_s || a->in_alarm != b->in_alarm || a->cause != b->cause;
}

/* The engine's news into the panel (the zone lock held by the caller). */
static void take_engine_news(uint64_t now)
{
    out_msg_t m;
    while (s_outq != NULL && xQueueReceive(s_outq, &m, 0) == pdTRUE) {
        zbz_panel_output(&s_panel, now, m.partition, (alarm_output_t)m.out,
                         (alarm_pattern_t)m.pattern);
    }
    if (atomic_exchange(&s_out_resync, false)) {
        uint8_t latest[ALARM_MAX_PARTITIONS][2];
        taskENTER_CRITICAL(&s_mux);
        memcpy(latest, s_out_latest, sizeof(latest));
        taskEXIT_CRITICAL(&s_mux);
        for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
            zbz_panel_output(&s_panel, now, p, ALARM_OUT_BELL, (alarm_pattern_t)latest[p - 1u][0]);
            zbz_panel_output(&s_panel, now, p, ALARM_OUT_STROBE,
                             (alarm_pattern_t)latest[p - 1u][1]);
        }
    }
    zba_view_t views[ALARM_MAX_PARTITIONS];
    taskENTER_CRITICAL(&s_mux);
    const uint8_t dirty = s_views_dirty;
    s_views_dirty = 0u;
    memcpy(views, s_views, sizeof(views));
    const bool walk = s_walk;
    taskEXIT_CRITICAL(&s_mux);
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS; p++) {
        const uint8_t bit = (uint8_t)(1u << (p - 1u));
        /* the first view since the start (or since a late load: replay_engine_news) always */
        if ((dirty & bit) != 0u && ((s_views_told_known & bit) == 0u ||
                                    view_differs(&views[p - 1u], &s_views_told[p - 1u]))) {
            s_views_told[p - 1u] = views[p - 1u];
            s_views_told_known = (uint8_t)(s_views_told_known | bit);
            zbz_panel_partition(&s_panel, now, p, &views[p - 1u]);
        }
    }
    if (walk != s_walk_told) {
        s_walk_told = walk;
        zbz_panel_walk_test(&s_panel, now, walk); /* ZB-022: its scope follows (refresh_walk_scope) */
    }
}

/* ZB33: a load that lands late (load_again) starts the sirens and keypads afresh
 * (zbz_panel_load: their outputs and the partitions it knew are gone) on a panel the engine has
 * been telling all along. What it told is told again the way a normal start tells it: each
 * partition's latest view as the first since the start, each partition's last bell and strobe
 * (both kept as the news arrives, also while unloaded), and the walk test's scope with the
 * devices now known. Only flags: take_engine_news feeds them under the zone lock in this same
 * turn of the zigbee task, and nothing here waits for the alarm task. */
static void replay_engine_news(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_views_dirty = (uint8_t)(s_views_dirty | s_views_seen);
    taskEXIT_CRITICAL(&s_mux);
    s_views_told_known = 0u;
    atomic_store(&s_out_resync, true);
    s_walk_scope_seq = atomic_load(&s_cfg_seq) + 1u; /* refresh_walk_scope asks again */
}

/* Who a key fob may belong to (zba_fob_may_be): a fob's command is refused here too if its user
 * has changed since. */
static bool fob_user_ok(uint8_t user)
{
    if (user == 0u || user > 64u) {
        return false;
    }
    taskENTER_CRITICAL(&s_mux);
    const bool ok = (s_fob_users & ((uint64_t)1u << (user - 1u))) != 0u;
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

/* ZB-022: the zones a running walk test covers (zbz_walk_covered), from the engine's configuration
 * and state, to the source (zbz_panel_walk_scope) - outside the zone lock while it asks: it waits
 * for the alarm task. Asked when the walk test starts or ends, when the configuration changes and
 * every ZB_WALK_SCOPE_MS while it runs (the zones' bindings). Until a sensor is in the scope it
 * stays out of IAS test mode: its trips arrive as faults, which the engine handles (WALK-007). */
static void refresh_walk_scope(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool walk = s_walk;
    taskEXIT_CRITICAL(&s_mux);
    const uint64_t at = now_ms();
    const uint32_t seq = atomic_load(&s_cfg_seq);
    if (walk == s_walk_scope_on &&
        (!walk || (seq == s_walk_scope_seq && at - s_walk_scope_at < ZB_WALK_SCOPE_MS))) {
        return;
    }
    uint8_t covered[ALARM_ZONE_BITMAP_BYTES];
    memset(covered, 0, sizeof(covered));
    if (walk) {
        alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM);
        alarm_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM);
        const bool ok = cfg != NULL && snap != NULL && alarm_service_get_config(cfg) == ESP_OK;
        if (ok) {
            alarm_service_get_snapshot(snap);
            zbz_walk_covered(cfg, snap, covered);
        }
        heap_caps_free(cfg);
        heap_caps_free(snap);
        if (!ok) {
            return; /* asked again at the next turn */
        }
    }
    s_walk_scope_on = walk;
    s_walk_scope_seq = seq;
    s_walk_scope_at = at;
    zone_service_lock();
    zbz_panel_walk_scope(&s_panel, now_ms(), covered);
    zone_service_unlock();
}

/* The engine device numbers held by the installer's device table (zbz_panel_port_t
 * .devices_taken), refreshed when the configuration changes - outside the zone lock: it waits
 * for the alarm task. The panel-link keypads' numbers (docs/features/11 LINK-001) come on top,
 * from zigbee_service_set_link_devices. */
static void refresh_taken(void)
{
    if (!atomic_load(&s_cfg_known)) {
        return;
    }
    const uint32_t seq = atomic_load(&s_cfg_seq);
    if (seq == s_cfg_seen && s_cfg_seen != 0u) {
        return;
    }
    alarm_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM);
    if (cfg == NULL) {
        return; /* tried again at the next turn */
    }
    if (alarm_service_get_config(cfg) == ESP_OK) {
        uint32_t taken = 0u;
        for (unsigned d = 0; d < ALARM_MAX_DEVICES && d < 32u; d++) {
            if (cfg->devices[d].enabled) {
                taken |= 1u << d;
            }
        }
        atomic_store(&s_taken, taken);
        uint64_t fob_users = 0u;
        for (unsigned u = 0; u < ALARM_MAX_USERS && u < 64u; u++) {
            if (zba_fob_may_be(cfg->users[u].authority)) {
                fob_users |= (uint64_t)1u << u;
            }
        }
        taskENTER_CRITICAL(&s_mux);
        s_fob_users = fob_users;
        taskEXIT_CRITICAL(&s_mux);
        s_cfg_seen = seq != 0u ? seq : 1u;
    }
    heap_caps_free(cfg);
}

/* ---- the keypads' and fobs' commands (zbz_ace.h zba_post_t) --------------------------------- */

bool zbsvc_ace_post(void *ctx, const zba_post_t *post)
{
    (void)ctx;
    if (post == NULL) {
        return false;
    }
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        if (!s_ace[i].used) {
            s_ace[i].post = *post;
            s_ace[i].used = true;
            if (s_task != NULL) {
                xTaskNotifyGive(s_task);
            }
            return true;
        }
    }
    return false;
}

/* The state a fob's command needs, as panel_web's actions ask it: an arm only while disarmed, a
 * disarm only while not; a panic in any state. The engine's own checks still apply - this guards
 * against a stale partition view in zbz_ace. */
static alarm_svc_when_t fob_when(const alarm_cmd_t *cmd)
{
    switch (cmd->type) {
    case ALARM_CMD_ARM:
        return ALARM_SVC_WHEN_DISARMED;
    case ALARM_CMD_DISARM:
        return ALARM_SVC_WHEN_NOT_DISARMED;
    default:
        return ALARM_SVC_WHEN_ANY;
    }
}

/* Each waiting command on the alarm task - outside the zone lock: it waits for its answer - then
 * the answer to the panel by its ticket. A keypad's carries its code (alarm_service_command: the
 * engine checks it and counts a wrong one toward the keypad's lockout); a fob's runs as its user
 * (alarm_service_command_as, the same rules as that user's code). The copy is wiped at once. */
static void ace_run(void)
{
    for (size_t i = 0; i < ZBA_PENDING_MAX; i++) {
        if (!s_ace[i].used) {
            continue;
        }
        zba_post_t post = s_ace[i].post;
        wipe(&s_ace[i].post, sizeof(s_ace[i].post));
        s_ace[i].used = false;
        alarm_cmd_result_t detail;
        memset(&detail, 0, sizeof(detail));
        const alarm_result_t r =
            post.user == 0u ? alarm_service_command(&post.cmd, &post.source, &detail)
            : !fob_user_ok(post.user)
                ? ALARM_ERR_NOT_AUTHORIZED /* its user is no longer one a fob may be */
                : alarm_service_command_as(&post.cmd, &post.source, post.user,
                                           fob_when(&post.cmd), &detail);
        const uint32_t ticket = post.ticket;
        wipe(&post, sizeof(post));
        s_stats.ace_posts++;
        zone_service_lock();
        if (!zbz_panel_ace_result(&s_panel, now_ms(), ticket, r, &detail)) {
            s_stats.ace_failed++; /* answered too late: the keypad asks again */
        }
        zone_service_unlock();
    }
}

/* ---- requests ------------------------------------------------------------------------------ */

/* Installer mode with every partition disarmed: the pages' `allowed` (ui_backend.h zb_request;
 * zbz_panel_request lets PING, IDENTIFY, SURVEY and DISMISS through without it). */
static bool installer_and_disarmed(void)
{
    alarm_snapshot_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (s == NULL) {
        return false;
    }
    alarm_service_get_snapshot(s);
    bool ok = s->program;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) != 0u && s->part[p].state != ALARM_STATE_DISARMED) {
            ok = false;
        }
    }
    heap_caps_free(s);
    return ok;
}

static bool adds_something(uint8_t kind)
{
    return kind == (uint8_t)UI_ZB_REQ_ADD || kind == (uint8_t)UI_ZB_REQ_ADD_CODE ||
           kind == (uint8_t)UI_ZB_REQ_REPLACE || kind == (uint8_t)UI_ZB_REQ_CODE_ADD;
}

/* ZB33: what a page can change without the radio and that the stored devices hold: refused while
 * they could not be read, or a late load (load_again) would throw it away. */
static bool stores_without_radio(uint8_t kind)
{
    return kind == (uint8_t)UI_ZB_REQ_CODE_ADD || kind == (uint8_t)UI_ZB_REQ_CODE_FORGET ||
           kind == (uint8_t)UI_ZB_REQ_ADD_CODE || kind == (uint8_t)UI_ZB_REQ_RENAME ||
           kind == (uint8_t)UI_ZB_REQ_SIREN || kind == (uint8_t)UI_ZB_REQ_KEYPAD ||
           kind == (uint8_t)UI_ZB_REQ_FOB;
}

/* ---- the binding gate (ZB21, RF32) ---------------------------------------------------------- */

/* `allowed` is a snapshot taken on the asking task (the pages, the Panel Manager, the console,
 * the developer link) before the request waited for s_req_lock and this task, and the alarm task
 * arms on its own meanwhile. So a request that needs it, and a device replacement's rebinds, run
 * under the alarm service's binding gate (zone_hub/zone_bind_gate.h): reserved here holding no
 * lock, claimed under the zone lock, ended before the zone lock goes (zbz_panel_request_gated,
 * zbz_panel_replace_gated). Armed since the snapshot: NOT_ALLOWED, or the replacement FAILED,
 * the radio and the bindings untouched.
 *
 * Arming waits from the claim to the end (CORE-ARM-013's hold, never shown as "not ready";
 * refresh_snapshot): the request's own work under the zone lock and its radio hooks -
 * set_channel's Mgmt_NWK_Update_req, reset_network's local reset, leave, permit_join, and
 * RESTART's esp_zigbee_stop (gate_after) - each waiting at most ZB_LOCK_WAIT_MS (200 ms) for the
 * stack's lock; a replacement's rebinds are in-memory hub changes. The channel move, the new
 * network and the stack's restart then happen in the stack, after the end: the change committed
 * while every partition was disarmed is what the hold protects. Most hooks only queue work for
 * the stack; whether ezb_bdb_reset_via_local_action clears the stored network before it returns,
 * and how long esp_zigbee_stop takes, is unmeasured: the hold's length is a bench check. */
static void restart_if_asked(void);

static zone_bind_reserve_t gate_reserve(void *ctx)
{
    (void)ctx;
    return alarm_service_bind_reserve_why(); /* waits for the alarm task: no lock held */
}

static zone_bind_claim_t gate_claim(void *ctx)
{
    (void)ctx;
    return alarm_service_bind_claim();
}

static void gate_end(void *ctx)
{
    (void)ctx;
    alarm_service_bind_end();
}

static void gate_lock(void *ctx)
{
    (void)ctx;
    zone_service_lock();
}

static void gate_unlock(void *ctx)
{
    (void)ctx;
    zone_service_unlock();
}

static void gate_pause(void *ctx)
{
    (void)ctx;
    vTaskDelay(1); /* an engine step runs, or another change holds the gate */
}

/* RESTART and RETRY answered OK restart the stack (zbz_panel.h) at once, under the claim. */
static void gate_after(void *ctx, const ui_zb_request_t *req, ui_zb_req_result_t r)
{
    (void)ctx;
    if (r == UI_ZB_REQ_OK &&
        (req->kind == (uint8_t)UI_ZB_REQ_RESTART || req->kind == (uint8_t)UI_ZB_REQ_RETRY)) {
        atomic_store(&s_restart, true);
        restart_if_asked();
    }
}

static const zbz_gate_port_t s_bind_gate = {.ctx = NULL,
                                            .reserve = gate_reserve,
                                            .claim = gate_claim,
                                            .end = gate_end,
                                            .lock = gate_lock,
                                            .unlock = gate_unlock,
                                            .pause = gate_pause,
                                            .after = gate_after};

/* On the zigbee task, holding no lock. */
static ui_zb_req_result_t run_request(const ui_zb_request_t *req, bool allowed)
{
    if (!s_loaded && stores_without_radio(req->kind)) {
        return UI_ZB_REQ_NO_RADIO; /* the radio waits for them too (load_again) */
    }
    if (adds_something(req->kind)) {
        tell_nvs_free(); /* just before a new device or code is let in (devices.md §4) */
    }
    if (req->kind == (uint8_t)UI_ZB_REQ_SURVEY && !zone_service_all_disarmed()) {
        /* ZB-016: the survey's energy scans leave the panel deaf about 4 x 138 ms (a sensor's
         * frame then rests on its retries), and its result serves only a channel change, which
         * needs every partition disarmed: not while anything is armed, from any caller. */
        return UI_ZB_REQ_NOT_ALLOWED;
    }
    return zbz_panel_request_gated(&s_panel, now_ms(), req, allowed, &s_bind_gate); /* ZB21 */
}

static bool take_request(void)
{
    static EXT_RAM_BSS_ATTR ui_zb_request_t req; /* the zigbee task's alone */
    uint32_t seq = 0u;
    bool allowed = false;
    bool have = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_rq.pending) {
        req = s_rq.req;
        wipe(&s_rq.req, sizeof(s_rq.req)); /* the slot keeps no install code once taken */
        allowed = s_rq.allowed;
        seq = s_rq.seq;
        s_rq.pending = false;
        have = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!have) {
        return false;
    }
    const ui_zb_req_result_t r = run_request(&req, allowed);
    wipe(&req, sizeof(req)); /* an install code passes through it */
    taskENTER_CRITICAL(&s_mux);
    s_rq.done_seq = seq;
    s_rq.result = r;
    taskEXIT_CRITICAL(&s_mux);
    (void)xSemaphoreGive(s_req_done);
    return true;
}

/* Waits up to `ms` for the answer to `seq`. */
static bool await_answer(uint32_t seq, uint32_t ms, ui_zb_req_result_t *result)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(ms);
    for (;;) {
        const TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= limit || xSemaphoreTake(s_req_done, limit - spent) != pdTRUE) {
            return false;
        }
        taskENTER_CRITICAL(&s_mux);
        const bool answered = s_rq.done_seq == seq;
        if (answered) {
            *result = s_rq.result;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (answered) {
            return true;
        }
    }
}

ui_zb_req_result_t zbsvc_submit(const ui_zb_request_t *req, bool allowed)
{
    if (!s_started || req == NULL) {
        return UI_ZB_REQ_NO_RADIO;
    }
    (void)xSemaphoreTake(s_req_lock, portMAX_DELAY);
    (void)xSemaphoreTake(s_req_done, 0); /* an answer nobody waited for any more */
    taskENTER_CRITICAL(&s_mux);
    const uint32_t seq = ++s_rq.seq;
    s_rq.req = *req;
    wipe(s_rq.req.installer_code, sizeof(s_rq.req.installer_code)); /* checked by the caller */
    s_rq.allowed = allowed;
    s_rq.pending = true;
    taskEXIT_CRITICAL(&s_mux);
    s_stats.requests++;
    xTaskNotifyGive(s_task);
    ui_zb_req_result_t result = UI_ZB_REQ_BUSY;
    bool answered = await_answer(seq, REQUEST_MS, &result);
    if (!answered) {
        bool taken = true;
        taskENTER_CRITICAL(&s_mux);
        if (s_rq.done_seq == seq) {
            answered = true; /* answered as the wait ended */
            result = s_rq.result;
        } else if (s_rq.pending && s_rq.seq == seq) {
            taken = false;
            s_rq.pending = false; /* never taken: withdrawn */
            wipe(&s_rq.req, sizeof(s_rq.req));
        }
        taskEXIT_CRITICAL(&s_mux);
        /* Taken and running (a slow store, the zone lock): an add window opening or a device
         * being removed must not read "busy" (devices.md §6), so its own answer is waited for
         * once more. The request is never run twice either way. */
        if (!answered && taken) {
            answered = await_answer(seq, REQUEST_MS, &result);
        }
        if (!answered) {
            s_stats.request_timeouts++;
        }
    }
    (void)xSemaphoreGive(s_req_lock);
    return result;
}

/* ---- the task ------------------------------------------------------------------------------ */

/* Writing the radio takes every wireless sensor off the air for minutes, so it waits for a
 * disarmed panel (ZB-001), exactly as a firmware update does (docs/features/13). */
static bool may_update_now(void)
{
    return zone_service_all_disarmed();
}

static void stack_up(bool up, uint64_t now)
{
    if (s_stack_up == up) {
        return;
    }
    s_stack_up = up;
    zone_service_lock();
    zsrc_zb_link(&s_src, up, now);
    zone_service_unlock();
    zone_service_wake();
    ESP_LOGI(TAG, "sensor radio %s", up ? "ready" : "gone");
}

static uint8_t pause_why(radio_port_stop_t why)
{
    switch (why) {
    case RADIO_PORT_STOP_INCOMPATIBLE: return (uint8_t)UI_ZB_PAUSE_FIRMWARE;
    case RADIO_PORT_STOP_NO_RESET: return (uint8_t)UI_ZB_PAUSE_RESET;
    default: return (uint8_t)UI_ZB_PAUSE_NO_ANSWER;
    }
}

/* The radio as the pages see it (zbz_radio_info_t), from the bring-up, the guard and the source
 * (read under the zone lock by the caller). */
static void radio_info(zbz_radio_info_t *info, const zigbee_service_status_t *st, uint64_t now)
{
    memset(info, 0, sizeof(*info));
    unsigned fails = 0u;
    radio_port_stop_t why = RADIO_PORT_STOP_NONE;
    const bool paused = radio_port_paused(&fails, &why);
    info->restarts = (uint8_t)(fails > UINT8_MAX ? UINT8_MAX : fails);
    const size_t vlen = strnlen(st->version, sizeof(info->version) - 1u);
    memcpy(info->version, st->version, vlen);
    info->version[vlen] = '\0';
    if (paused) {
        info->state = (uint8_t)UI_ZB_RADIO_PAUSED;
        info->paused_why = pause_why(why);
    } else if (st->ready) {
        info->state = (uint8_t)UI_ZB_RADIO_WORKING;
    } else if (st->boot == ZB_BOOT_NEEDS_HELP) {
        info->state = (uint8_t)UI_ZB_RADIO_UNUSABLE;
    } else if (st->boot == ZB_BOOT_NO_RADIO) {
        info->state = (uint8_t)(s_ever_answered ? UI_ZB_RADIO_NOT_ANSWERING
                                                : UI_ZB_RADIO_NOT_CONNECTED);
    } else {
        info->state = (uint8_t)UI_ZB_RADIO_STARTING;
    }
    info->jammed = st->jammed;
    info->noise_known = st->energy_known;
    info->noise_dbm = st->energy_dbm;
    if (s_src.learning && s_src.learn_until > now) {
        const uint64_t s = (s_src.learn_until - now + 999u) / 1000u;
        info->learn_s = (uint16_t)(s > UINT16_MAX ? UINT16_MAX : s);
    }
}

/* The radio task owns s_boot. Readers see a complete published snapshot; the panel is told the
 * radio's state when it changes (zbz_panel_radio). */
static void publish_status(uint64_t now)
{
    zigbee_service_status_t status = {0};
    status.enabled = true;
    status.boot = zb_boot_state(&s_boot);
    status.words = zb_boot_words(&s_boot);
    status.percent = s_boot.percent;
    snprintf(status.version, sizeof(status.version), "%s", s_boot.found);
    s_ever_answered = s_ever_answered || s_boot.found[0] != '\0' || s_stack_up;
    status.ready = s_stack_up;
    unsigned fails = 0;
    status.paused = radio_port_paused(&fails, NULL);
    status.radio_fails = (uint8_t)(fails > UINT8_MAX ? UINT8_MAX : fails);
    if (status.paused) {
        status.words = "Sensor radio paused: it restarted the panel (zigbee retry)";
    }
    if (!s_loaded) {
        /* ZB33: the console's words (zigbee_console.c), never a page's: the radio waits */
        status.words = "Sensor radio waiting to read the stored devices (zigbee retry)";
    }
    zone_service_lock();
    status.jammed = zsrc_zb_jammed(&s_src);
    status.energy_known = s_src.link_up && s_src.energy_at != 0u;
    status.energy_dbm = s_src.energy_dbm;
    const size_t devices = zbz_reg_count(&s_panel.reg);
    status.devices = (uint8_t)(devices > UINT8_MAX ? UINT8_MAX : devices);
    zbz_radio_info_t info;
    radio_info(&info, &status, now);
    if (!s_info_valid || memcmp(&info, &s_info_told, sizeof(info)) != 0) {
        s_info_told = info;
        s_info_valid = true;
        zbz_panel_radio(&s_panel, now, &info);
    }
    zone_service_unlock();
    radio_port_network(&status.channel, &status.pan_id);
    taskENTER_CRITICAL(&s_status_mux);
    s_status = status;
    taskEXIT_CRITICAL(&s_status_mux);
}

static void wake_task(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* The bell trouble as the sirens last wanted it, to the alarm task (it may wait for its queue). */
static void tell_bell(void)
{
    const int want = atomic_load(&s_bell_want);
    if (want >= 0 && want != s_bell_told) {
        alarm_service_system_trouble(ALARM_TRBL_BELL, want != 0);
        s_bell_told = want;
    }
}

static uint32_t s_lost_seen;  /* alarm-path events lost since start, as last seen */
static uint64_t s_lost_until; /* their trouble holds until then; 0: none */

/* A radio event that may have carried an alarm, lost on a full queue (ZB-003, ZB-024): nobody
 * can say what it was, so the "sensors may not be heard" trouble holds for ZB_LOST_HOLD_MS after
 * the last one (a lost zone status is also read back from its device, radio_port_pump). */
static void tell_lost(uint64_t now)
{
    uint32_t dropped = 0u;
    uint32_t lost = 0u;
    radio_port_queue_counts(&dropped, &lost);
    if (lost != s_lost_seen) {
        s_lost_seen = lost;
        if (s_lost_until == 0u) {
            zone_service_report_lost((uint8_t)ZSRC_ZIGBEE, true);
        }
        s_lost_until = now + ZB_LOST_HOLD_MS;
    } else if (s_lost_until != 0u && now >= s_lost_until) {
        s_lost_until = 0u;
        zone_service_report_lost((uint8_t)ZSRC_ZIGBEE, false);
    }
}

/* A RESTART or RETRY the panel answered OK (zbz_panel.h): the running stack is stopped and the
 * bring-up starts it again; a paused or silent radio is tried again at once. */
static void restart_if_asked(void)
{
    if (!atomic_exchange(&s_restart, false)) {
        return;
    }
    if (s_stack_up && radio_port_restart()) {
        ESP_LOGI(TAG, "restarting the sensor radio (asked)");
        return; /* the loop sees it gone and brings it up again (zb_boot_lost) */
    }
    radio_port_resume(); /* RETRY: the pause after it restarted the panel is lifted */
    if (!s_stack_up) {
        atomic_store(&s_retry, true);
    }
}

static void load_again(uint64_t now);

static void zigbee_task(void *arg)
{
    (void)arg;
    zb_boot_start(&s_boot, now_ms());
    for (;;) {
        const uint64_t now = now_ms();
        restart_if_asked();
        if (atomic_exchange(&s_retry, false) && !s_stack_up) {
            if (!s_loaded) {
                s_load_tries = 0u; /* ZB33: asked, so the stored devices are read again too */
                s_load_retry_at = 0u;
            }
            zb_boot_start(&s_boot, now); /* never under a running stack: it owns the UART */
        }
        load_again(now);
        const zb_boot_state_t before = zb_boot_state(&s_boot);
        const bool may_update = !zb_boot_ready(&s_boot) && now >= zb_boot_next(&s_boot) &&
                                may_update_now();
        zb_boot_tick(&s_boot, now, may_update);
        const zb_boot_state_t after = zb_boot_state(&s_boot);
        if (after != before) {
            ESP_LOGI(TAG, "%s", zb_boot_words(&s_boot));
        }

        if (zb_boot_ready(&s_boot)) {
            if (!s_stack_up && !s_loaded) {
                /* ZB33: not on a panel that could not read its devices (load_again). */
                radio_port_pump(&s_src, now);
            } else if (!s_stack_up) {
                radio_port_pump(&s_src, now); /* a stopped stack's last ends, if any are left */
                /* The radio runs our firmware: bring the Zigbee network up on top of it. */
                if (radio_port_stack_start()) {
                    stack_up(true, now_ms()); /* forming may have taken a while */
                } else {
                    ESP_LOGE(TAG, "the sensor radio answers but the Zigbee network would not "
                                  "start");
                    zb_boot_lost(&s_boot, now);
                }
            } else if (!radio_port_alive()) {
                radio_port_pump(&s_src, now); /* what it said last: requests ended NO_RADIO */
                stack_up(false, now);
                zb_boot_lost(&s_boot, now); /* the module was pulled out, or stopped answering */
            } else {
                radio_port_pump(&s_src, now); /* reports to the source and to zbz_panel */
            }
        } else {
            if (s_stack_up) {
                stack_up(false, now);
            }
            /* The stack's last words after it stopped: its requests' NO_RADIO ends, queued
             * while it shut down (radio_port.c stack_task). Nothing else arrives then. */
            radio_port_pump(&s_src, now);
        }
        publish_status(now_ms());

        const bool asked = take_request();
        zone_service_lock();
        const uint64_t t = now_ms();
        take_engine_news(t);
        zbz_panel_poll(&s_panel, t);
        const uint64_t next = zbz_panel_next_deadline(&s_panel);
        zone_service_unlock();
        /* RF32: a replacement the pump or the poll found moves its zones under a claimed gate */
        zbz_panel_replace_gated(&s_panel, now_ms(), &s_bind_gate);
        if (asked) {
            zone_service_wake(); /* the source may have news for the hub */
        }
        persist();
        tell_storage_trouble();
        log_events();
        ace_run();
        tell_bell();
        tell_lost(now_ms());
        refresh_taken();
        refresh_walk_scope();
        s_stats.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

        const uint64_t boot_next = zb_boot_next(&s_boot);
        uint64_t wait = s_stack_up ? ZB_UP_WAIT_MS : ZB_MAX_WAIT_MS;
        const uint64_t at = now_ms();
        if (!s_stack_up && boot_next != UINT64_MAX) {
            const uint64_t ms = boot_next > at ? boot_next - at : 0u;
            wait = ms < wait ? ms : wait;
        }
        if (!s_loaded && s_load_tries < LOAD_TRIES_MAX) {
            const uint64_t ms = s_load_retry_at > at ? s_load_retry_at - at : 0u;
            wait = ms < wait ? ms : wait;
        }
        if (next != UINT64_MAX) {
            const uint64_t ms = next > at ? next - at : 0u;
            wait = ms < wait ? ms : wait;
        }
        const TickType_t ticks = pdMS_TO_TICKS((uint32_t)wait);
        (void)ulTaskNotifyTake(pdTRUE, ticks > 0 ? ticks : 1); /* never a busy loop */
    }
}

/* ---- life cycle ---------------------------------------------------------------------------- */

/* What is stored, into the panel (before the radio is up), and the NVS budget. False when NVS
 * could not be read (ZB33): an absent namespace or key is a new panel, as always, but any other
 * failure - the namespace that won't open, a length or a value that won't read - leaves the panel
 * empty and unloaded (nothing half-read), the radio stopped and the read due again. A damaged or
 * later stored format is not a read failure: its verdict is logged as before. */
static bool load_panel(void)
{
    store_ctx_t c = {.err = ESP_OK};
    zbz_reg_load_t verdict = ZBZ_REG_EMPTY;
    zone_service_lock();
    const esp_err_t open = nvs_open(ZBZ_NVS_NAMESPACE, NVS_READONLY, &c.h);
    if (open == ESP_OK) {
        const zbz_store_port_t store = {.ctx = &c, .read = store_read};
        verdict = zbz_panel_load(&s_panel, &store);
        nvs_close(c.h);
    } else {
        if (open != ESP_ERR_NVS_NOT_FOUND) {
            store_failed(&c, ZBZ_NVS_NAMESPACE, open);
        }
        verdict = zbz_panel_load(&s_panel, NULL); /* nothing stored yet: a new panel */
    }
    if (c.err != ESP_OK) {
        (void)zbz_panel_load(&s_panel, NULL); /* nothing half-read stays */
    }
    const size_t n = zbz_reg_count(&s_panel.reg);
    zone_service_unlock();
    s_loaded = c.err == ESP_OK;
    if (!s_loaded) {
        ESP_LOGE(TAG, "reading the stored Zigbee devices failed at \"%s\" (%s): the sensor radio "
                      "waits for them",
                 c.key, esp_err_to_name(c.err));
        return false;
    }
    switch (verdict) {
    case ZBZ_REG_DAMAGED:
        ESP_LOGE(TAG, "some stored Zigbee devices were damaged: found again by their traffic");
        break;
    case ZBZ_REG_FUTURE:
        ESP_LOGW(TAG, "the Zigbee devices were stored by a later firmware: read-only");
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "%u Zigbee device(s) stored", (unsigned)n);
    tell_nvs_free(); /* at start, before the radio is up (devices.md §4) */
    return true;
}

/* ZB33: the stored devices read again, on the zigbee task (its stack is internal RAM), while
 * they could not be: LOAD_TRIES_MAX more tries after the start's, LOAD_RETRY_MS apart and
 * doubling (about 2 minutes in all), then only when the installer asks (`zigbee retry`) or at the
 * next start. */
static void load_again(uint64_t now)
{
    if (s_loaded || s_load_tries >= LOAD_TRIES_MAX || now < s_load_retry_at) {
        return;
    }
    s_load_tries++;
    if (load_panel()) {
        ESP_LOGI(TAG, "the stored Zigbee devices read on try %u", s_load_tries + 1u);
        replay_engine_news(); /* the sirens and keypads hear the engine again (alarm first) */
        return;
    }
    if (s_load_tries >= LOAD_TRIES_MAX) {
        ESP_LOGE(TAG, "the stored Zigbee devices still can't be read: `zigbee retry` tries again");
        return;
    }
    s_load_retry_at = now + ((uint64_t)LOAD_RETRY_MS << s_load_tries);
}

esp_err_t zigbee_service_start(const zigbee_service_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "no pins for the wireless socket");
    s_req_lock = xSemaphoreCreateMutex();
    s_req_done = xSemaphoreCreateBinary();
    s_outq = xQueueCreate(OUT_QUEUE_LEN, sizeof(out_msg_t));
    ESP_RETURN_ON_FALSE(s_req_lock != NULL && s_req_done != NULL && s_outq != NULL, ESP_ERR_NO_MEM,
                        TAG, "request queue");
    const esp_err_t err = radio_port_open(cfg->uart_tx_gpio, cfg->uart_rx_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the wireless socket's UART would not open (%s)", esp_err_to_name(err));
        return err;
    }

    const zsrc_zb_radio_t radio = {.ctx = NULL,
                                   .permit_join = radio_permit_join,
                                   .ias_cie_write = radio_cie_write,
                                   .ias_enroll_response = radio_enroll_response,
                                   .read_attribute = radio_read_attribute,
                                   .remove_device = radio_remove_device,
                                   .energy_scan = radio_energy_scan,
                                   .node_desc_req = radio_node_desc_req,
                                   .power_desc_req = radio_power_desc_req,
                                   .active_ep_req = radio_active_ep_req,
                                   .simple_desc_req = radio_simple_desc_req,
                                   .read_attributes = radio_read_attributes,
                                   .bind = radio_bind,
                                   .configure_reporting = radio_configure_reporting,
                                   .leave = radio_leave,
                                   .forget = radio_forget,
                                   .identify = radio_identify,
                                   .ias_test_mode = radio_ias_test_mode,
                                   .zcl_command = radio_zcl_command,
                                   .install_code = radio_install_code,
                                   .codes_only = radio_codes_only,
                                   .set_channel = radio_set_channel,
                                   .reset_network = radio_reset_network,
                                   .channel_survey = radio_channel_survey};
    const zsrc_sink_t sink = zone_service_sink();
    const zbz_panel_port_t port = {.ctx = NULL,
                                   .binding = port_binding,
                                   .rebind = port_rebind,
                                   .wall_clock = port_wall_clock,
                                   .changed = port_changed,
                                   .devices_taken = port_devices_taken,
                                   .ace_post = zbsvc_ace_post,
                                   .replace_waits = true}; /* RF32: zbz_panel_replace_gated */
    const zbw_port_t wd = {.ctx = NULL, .send = port_send, .bell_trouble = port_bell_trouble,
                           .checkin = NULL};
    const zba_port_t ace = {.ctx = NULL, .send = port_send, .default_rsp = port_default_rsp};
    zone_service_lock();
    zsrc_zb_init(&s_src, &radio, &sink);
    zbz_panel_init(&s_panel, &port, &s_src, &radio, &wd, &ace);
    zone_service_unlock();
    if (!load_panel()) {
        s_load_retry_at = now_ms() + LOAD_RETRY_MS; /* read again by the zigbee task (ZB33) */
    }

    /* The engine's news for sirens and keypads, from now on; the partitions as they are now. */
    atomic_store(&s_engine_ready, true);
    alarm_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM);
    if (snap != NULL) {
        alarm_service_get_snapshot(snap);
        zigbee_service_engine_snapshot(snap);
        heap_caps_free(snap);
    }

    zone_service_lock();
    radio_port_set_report_sink(panel_report, NULL); /* zbz_panel_on_report (devices.md §2) */
    zone_service_unlock();
    radio_port_set_wake(wake_task);
    zb_boot_port_t bport;
    radio_port_boot_port(&bport);
    zb_boot_init(&s_boot, &bport, radio_port_image_version());

    const zone_hub_result_t r = zone_service_register(zsrc_zb_ops(), &s_src);
    if (r != ZONE_HUB_OK) {
        ESP_LOGE(TAG, "the zone hub would not take the sensor radio (%s)",
                 zone_service_result_name(r));
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreatePinnedToCore(zigbee_task, "zigbee", ZB_TASK_STACK, NULL,
                                CONFIG_ALARM_TASK_PRIORITY - 1, &s_task, tskNO_AFFINITY) !=
        pdPASS) {
        ESP_LOGE(TAG, "no task for the sensor radio");
        return ESP_ERR_NO_MEM;
    }
    s_stats.stack_size = ZB_TASK_STACK;
    s_started = true;
    ESP_LOGI(TAG, "sensor radio on the wireless socket (TX %u, RX %u), carrying %s",
             (unsigned)cfg->uart_tx_gpio, (unsigned)cfg->uart_rx_gpio,
             radio_port_image_version()[0] != '\0' ? radio_port_image_version() : "no image");
    return ESP_OK;
}

zsrc_zb_t *zigbee_service_source(void)
{
    return s_started ? &s_src : NULL;
}

void zigbee_service_retry(void)
{
    if (!s_started) {
        return;
    }
    /* The installer asked: the pause after a restart is lifted too; a running stack is restarted
     * rather than probed under (the stack owns the UART). */
    atomic_store(&s_restart, true);
    wake_task();
}

void zigbee_service_status(zigbee_service_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->words = "Sensor radio off";
    if (atomic_load(&s_socket_zwave)) {
        out->socket_zwave = true;
        out->words = SOCKET_ZWAVE_WORDS;
        return;
    }
    if (!s_started) {
        return;
    }
    taskENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_mux);
}

void zigbee_service_set_report_sink(zigbee_service_report_fn fn, void *ctx)
{
    /* the same shape (radio_port_report_fn); NULL gives the reports back to the service's own
     * zbz_panel */
    radio_port_set_report_sink(fn != NULL ? fn : (s_started ? panel_report : NULL), ctx);
}

bool zigbee_service_wake_window(uint64_t ieee, uint16_t seconds)
{
    return s_started && s_stack_up && radio_port_wake_window(ieee, seconds);
}

bool zigbee_service_restart_radio(void)
{
    if (!s_started || !s_stack_up || !radio_port_restart()) {
        return false;
    }
    wake_task(); /* the task sees the radio gone and brings it up again */
    return true;
}

/* ---- the touch UI's calls and the readers (ZB-027) ---------------------------------------- */

bool zigbee_service_ui_status(ui_zb_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (atomic_load(&s_socket_zwave)) {
        out->radio = (uint8_t)UI_ZB_RADIO_SOCKET_ZWAVE;
        return true;
    }
    if (!s_started) {
        return false; /* no Zigbee on this panel: the pages hide */
    }
    zone_service_lock();
    zbz_panel_status(&s_panel, now_ms(), out);
    zone_service_unlock();
    return true;
}

size_t zigbee_service_ui_devices(ui_zb_device_t *out, size_t max)
{
    if (!s_started || out == NULL) {
        return 0u;
    }
    zone_service_lock();
    const size_t n = zbz_panel_devices(&s_panel, now_ms(), out, max);
    zone_service_unlock();
    return n;
}

bool zigbee_service_ui_device(uint16_t id, ui_zb_device_page_t *out)
{
    if (!s_started || out == NULL) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    zone_service_lock();
    const bool found = zbz_panel_device(&s_panel, now_ms(), id, out);
    zone_service_unlock();
    return found;
}

void zigbee_service_ui_progress(ui_zb_progress_t *out)
{
    if (!s_started) {
        memset(out, 0, sizeof(*out));
        return;
    }
    zone_service_lock();
    zbz_panel_progress(&s_panel, now_ms(), out);
    zone_service_unlock();
}

size_t zigbee_service_ui_codes(ui_zb_code_t *out, size_t max)
{
    if (!s_started || out == NULL) {
        return 0u;
    }
    zone_service_lock();
    const size_t n = zbz_panel_codes(&s_panel, out, max);
    zone_service_unlock();
    return n;
}

ui_zb_req_result_t zbsvc_reset_code(const alarm_source_t *src, char code[ALARM_CODE_MAX_LEN + 1])
{
    uint8_t user = 0u;
    alarm_authority_t authority = ALARM_AUTH_NONE;
    code[ALARM_CODE_MAX_LEN] = '\0';
    /* On the alarm task, waited for here (never in the zigbee task, never under the zone lock):
     * a wrong code counts toward this source's lockout like any other (ZB-017). */
    const alarm_result_t r = code[0] == '\0'
                                 ? ALARM_ERR_CODE_REQUIRED
                                 : alarm_service_authenticate_as(src, code, 0, &user, &authority);
    wipe(code, ALARM_CODE_MAX_LEN + 1u);
    return zbz_reset_code_result(r, authority);
}

ui_zb_req_result_t zigbee_service_ui_request(const ui_zb_request_t *req)
{
    if (req == NULL) {
        return UI_ZB_REQ_INVALID;
    }
    if (!s_started) {
        return UI_ZB_REQ_NO_RADIO;
    }
    if (req->kind == (uint8_t)UI_ZB_REQ_RESET) {
        /* the installer code typed again, checked as the touchscreen before anything else */
        char code[ALARM_CODE_MAX_LEN + 1];
        memcpy(code, req->installer_code, sizeof(code));
        const alarm_source_t src = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_LOCAL};
        const ui_zb_req_result_t r = zbsvc_reset_code(&src, code);
        if (r != UI_ZB_REQ_OK) {
            return r;
        }
    }
    return zbsvc_submit(req, installer_and_disarmed());
}

ui_zb_req_result_t zigbee_service_web_request(const ui_zb_request_t *req, bool allowed)
{
    if (req == NULL) {
        return UI_ZB_REQ_INVALID;
    }
    if (!s_started) {
        return UI_ZB_REQ_NO_RADIO;
    }
    ui_zb_request_t r = *req;
    r.via = UI_ZB_VIA_WEB; /* the event log names the Panel Manager (ZB-019) */
    const ui_zb_req_result_t result = zbsvc_submit(&r, allowed);
    wipe(&r, sizeof(r)); /* an install code, if it carried one */
    return result;
}

uint16_t zigbee_service_device_of_zone(uint16_t zone)
{
    if (!s_started) {
        return 0u;
    }
    zone_service_lock();
    const uint16_t id = zbz_panel_device_of_zone(&s_panel, zone);
    zone_service_unlock();
    return id;
}

uint32_t zigbee_service_changes(void)
{
    return atomic_load(&s_changed);
}

/* ---- MQTT (devices.md §15, ZB-026) --------------------------------------------------------- */

bool zigbee_service_mqtt_state(zigbee_service_mqtt_state_t *out)
{
    memset(out, 0, sizeof(*out));
    ui_zb_status_t st;
    if (!zigbee_service_ui_status(&st)) {
        return false;
    }
    out->radio = st.radio;
    out->channel = st.network_known ? st.channel : 0u;
    out->devices = st.devices;
    out->not_heard = st.not_heard;
    out->jammed = st.jammed;
    return true;
}

size_t zigbee_service_zone_diags(zigbee_service_zone_diag_t *out, size_t max)
{
    if (!s_started || out == NULL || max == 0u) {
        return 0u;
    }
    /* the readers' copy: one at a time (the MQTT task), PSRAM */
    static EXT_RAM_BSS_ATTR ui_zb_device_t list[UI_ZB_DEVICES_MAX];
    static SemaphoreHandle_t one;
    if (one == NULL) {
        one = xSemaphoreCreateMutex();
        if (one == NULL) {
            return 0u;
        }
    }
    (void)xSemaphoreTake(one, portMAX_DELAY);
    size_t n = 0u;
    zone_service_lock();
    const uint64_t now = now_ms();
    const size_t count = zbz_panel_devices(&s_panel, now, list, UI_ZB_DEVICES_MAX);
    for (uint16_t z = 1u; count > 0u && z <= ALARM_MAX_ZONES && n < max; z++) {
        const uint16_t id = zbz_panel_device_of_zone(&s_panel, z);
        for (size_t i = 0; id != 0u && i < count; i++) {
            if (list[i].id == id) {
                out[n].zone = z;
                out[n].battery_pct = list[i].battery_pct;
                out[n].signal = list[i].signal;
                n++;
                break;
            }
        }
    }
    zone_service_unlock();
    (void)xSemaphoreGive(one);
    return n;
}

/* ---- for the console and the developer link ------------------------------------------------ */

bool zbsvc_started(void)
{
    return s_started;
}

bool zbsvc_stack_up(void)
{
    return s_started && atomic_load(&s_stack_up);
}

void zbsvc_stats(zbsvc_stats_t *out)
{
    *out = s_stats;
    radio_port_queue_counts(&out->radio_dropped, &out->alarm_lost);
}

zbz_panel_t *zbsvc_panel(void)
{
    return s_started ? &s_panel : NULL;
}

zsrc_zb_t *zbsvc_source(void)
{
    return s_started ? &s_src : NULL;
}

uint64_t zbsvc_now_ms(void)
{
    return now_ms();
}

uint32_t zigbee_service_devices_held(void)
{
    if (!s_started) {
        return 0u; /* no keypad was ever assigned: the panel isn't built yet */
    }
    zone_service_lock();
    const uint32_t held = zbz_panel_devices_held(&s_panel);
    zone_service_unlock();
    return held;
}

bool zigbee_service_devices_held_now(uint32_t *out)
{
    *out = 0u;
    if (!s_started) {
        return true;
    }
    if (!zone_service_try_lock()) {
        return false; /* busy: the caller keeps what it read before */
    }
    *out = zbz_panel_devices_held(&s_panel);
    zone_service_unlock();
    return true;
}

void zigbee_service_set_link_devices(uint32_t mask)
{
    atomic_store(&s_link_taken, mask);
}

#else /* CONFIG_ALARM_ZSRC_ZIGBEE */

esp_err_t zigbee_service_start(const zigbee_service_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

zsrc_zb_t *zigbee_service_source(void)
{
    return NULL;
}

void zigbee_service_retry(void) {}

void zigbee_service_status(zigbee_service_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->words = "Sensor radio not built into this firmware";
    if (atomic_load(&s_socket_zwave)) {
        out->socket_zwave = true;
    }
}

void zigbee_service_set_report_sink(zigbee_service_report_fn fn, void *ctx)
{
    (void)fn;
    (void)ctx;
}

bool zigbee_service_wake_window(uint64_t ieee, uint16_t seconds)
{
    (void)ieee;
    (void)seconds;
    return false;
}

bool zigbee_service_restart_radio(void)
{
    return false;
}

bool zigbee_service_ui_status(ui_zb_status_t *out)
{
    memset(out, 0, sizeof(*out));
    return false;
}

size_t zigbee_service_ui_devices(ui_zb_device_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0u;
}

bool zigbee_service_ui_device(uint16_t id, ui_zb_device_page_t *out)
{
    (void)id;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

void zigbee_service_ui_progress(ui_zb_progress_t *out)
{
    memset(out, 0, sizeof(*out));
}

size_t zigbee_service_ui_codes(ui_zb_code_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0u;
}

ui_zb_req_result_t zigbee_service_ui_request(const ui_zb_request_t *req)
{
    (void)req;
    return UI_ZB_REQ_UNSUPPORTED;
}

ui_zb_req_result_t zigbee_service_web_request(const ui_zb_request_t *req, bool allowed)
{
    (void)req;
    (void)allowed;
    return UI_ZB_REQ_UNSUPPORTED;
}

uint16_t zigbee_service_device_of_zone(uint16_t zone)
{
    (void)zone;
    return 0u;
}

uint32_t zigbee_service_changes(void)
{
    return 0u;
}

bool zigbee_service_mqtt_state(zigbee_service_mqtt_state_t *out)
{
    memset(out, 0, sizeof(*out));
    return false;
}

void zigbee_service_engine_output(alarm_output_t output, uint8_t partition,
                                  alarm_pattern_t pattern)
{
    (void)output;
    (void)partition;
    (void)pattern;
}

void zigbee_service_engine_snapshot(const alarm_snapshot_t *snap)
{
    (void)snap;
}

size_t zigbee_service_zone_diags(zigbee_service_zone_diag_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0u;
}

uint32_t zigbee_service_devices_held(void)
{
    return 0u;
}

bool zigbee_service_devices_held_now(uint32_t *out)
{
    *out = 0u;
    return true;
}

void zigbee_service_set_link_devices(uint32_t mask)
{
    (void)mask;
}

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */
