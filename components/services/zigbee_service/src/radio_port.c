/*
 * radio_port.c - the sensor radio's hardware seam (see radio_port.h): the ESP32-H2 in the
 * wireless socket running firmware/radios/h2_rcp, and Espressif's Zigbee stack (esp-zigbee-lib
 * 2.x) on the P4 as the network's coordinator and trust center (docs/features/20, ADR-020).
 *
 * Two tasks meet here, and a lock order keeps them apart:
 *   - The stack's own task ("zb_stack") runs esp_zigbee_launch_mainloop(); every signal and ZCL
 *     callback below runs on it, with the stack's lock held (esp_zigbee.h:186-197: no lock is
 *     taken "in Zigbee callbacks"). They never take the zone service's lock: what they learn goes
 *     into a queue.
 *   - The service's task (zigbee_service.c) drains that queue into the portable source under the
 *     zone service's lock (radio_port_pump), and the source's requests (permit join, enrollment,
 *     reads, the BL-143 hooks) arrive here with that lock held and take the stack's lock briefly.
 * So the zone lock may be held while the stack lock is taken, never the other way round.
 *
 * Since BL-143 (docs/features/20 devices.md §2-§3) the queue also carries zsrc_zb_report_t: how a
 * device joined, its descriptors and Basic attributes, link quality, check-ins, leaves, the
 * cluster commands the source doesn't decode and the end of each request the source made (its
 * `tag`). The pump hands them to the report sink (zbz_panel_on_report, once zigbee_service wires
 * it). The requests waiting for their end are a small table (s_rq) only ever touched with the
 * stack's lock held: by the hooks, by the stack's callbacks and by the pump's timeout check.
 *
 * Before the stack runs, the socket's UART is ours, for one question: is a radio co-processor
 * there, and which firmware does it run (a Spinel PROP_VALUE_GET of NCP_VERSION)? Only an answer
 * hands the UART to the stack. The network (its key, the paired devices) is kept by the stack in
 * the panel's NVS partition, in the stack's own namespace.
 *
 * Under the stack, OpenThread's Spinel client ends the program when its radio fails a reset, turns
 * out incompatible or stops answering for good (its DieNow: the panel restarts). The probe keeps
 * an empty socket from ever starting the stack; the guard below keeps a radio that answers the
 * probe and then fails the stack from restarting the panel over and over.
 */
#include "radio_port.h"

#if CONFIG_ALARM_ZSRC_ZIGBEE

#if !CONFIG_ZB_ENABLED || !CONFIG_ZB_ZCZR || !CONFIG_ZB_RADIO_SPINEL_UART
#error "CONFIG_ALARM_ZSRC_ZIGBEE needs the stack as coordinator over a UART RCP: set them in sdkconfig.defaults (as the P4's does)"
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_radio_spinel.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/secur.h"
#include "ezbee/tasklet.h"
#include "ezbee/zha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "zigbee_zones/zbz_ace.h"
#include "zone_service.h"

static const char *TAG = "radio";

#define RADIO_UART_NUM  UART_NUM_2
#define RADIO_BAUD      CONFIG_ALARM_ZB_UART_BAUD
#define RADIO_BUF_BYTES 1024

#define ZB_STORAGE          "nvs"   /* the panel's NVS partition; the stack keeps its own namespace */
#define ZB_STACK_TASK_BYTES 8192    /* internal RAM: the stack writes NVS from this task (B001) */
#define ZB_STACK_PRIORITY   5
#define ZB_EP               1u      /* the panel's own endpoint */
#define ZB_MAX_CHILDREN     32u     /* sleepy sensors that may hang straight off the panel */
#define ZB_FORM_WAIT_MS     60000u  /* forming a network scans every allowed channel */
#define ZB_LOCK_WAIT_MS     200u    /* how long a request from the zone side may wait */
#define ZB_RETRY_MS         1000u   /* a failed start or formation is tried again after this, */
#define ZB_RETRY_MAX_SHIFT  6u      /* doubling up to 64 s while it keeps failing */
/* Energy scan: ((1 << 1) + 1) * 15.36 ms = 46 ms on the network's channel, every
 * ZSRC_ZB_SCAN_MS (5 s). The radio hears nothing else meanwhile; MAC and APS retries cover it. */
#define ZB_ED_DURATION      1u
#define ZB_QUEUE_LEN        32u     /* events of every kind */
/* The alarm path's own room on top of those (ZB-003, ZB-024): a zone status, an enroll request
 * or a keypad's or fob's ACE command may use every slot; anything else leaves the last
 * ZB_ROOM_ALARM free, and a frame's "it is alive" (EV_FRAME), shed first, ZB_ROOM_FRAME. */
#define ZB_ROOM_ALARM       8u
#define ZB_ROOM_FRAME       12u
#define ZB_REREAD_MAX       8u      /* zone statuses lost, asked again by the pump */
#define ZB_CODE_SLOTS       8u      /* keypad commands with a code on their way to the pump */
#define ZB_ASKING           8u      /* devices being described at once */
#define ZB_NO_NWK           0xFFFEu /* short addresses from here up are not a device */

/* ---- BL-143: the source's requests and what the radio side remembers between frames ---------- */
#define ZB_RQ_MAX           16u     /* requests waiting for their end (REP_DONE) */
#define ZB_RQ_MS            20000u  /* an answer: a sleepy device's parent holds a frame 7.68 s,
                                     * then MAC and APS retries (zbz_panel's own step is 10 s for a
                                     * listening device, longer for a sleepy one) */
#define ZB_LEAVE_WAIT_MS    12000u  /* a leave answered (zbz: ZBZ_LEAVE_MS 15 s, then NO_ANSWER) */
#define ZB_CHANNEL_WAIT_MS  25000u  /* the panel's radio on the new channel (zbz: ZBZ_CHANNEL_MS 30 s) */
#define ZB_RESET_WAIT_MS    55000u  /* left the network and formed a new one (zbz: ZBZ_RESET_MS 60 s) */
#define ZB_SURVEY_MS        10000u
#define ZB_SURVEY_DURATION  3u      /* ((1 << 3) + 1) * 15.36 ms = 138 ms on each of four channels */
#define ZB_IC_MAX           16u     /* devices the trust center holds an install code for */
#define ZB_LINKS            8u      /* the last frames' link quality, by short address */
#define ZB_AWAKE_MAX        8u      /* devices asked to stay awake at their next check-in */
#define ZB_LEAVING_MAX      4u      /* devices that answered the panel's leave and are going */
#define ZB_FAST_POLL_QS     120u    /* 30 s of fast polling while a request to it waits */
#define ZB_EXPIRE_BATCH     4u      /* timeouts handed out per pump */
#define ZB_PROFILE_HA       0x0104u /* Home Automation, the profile of every Zigbee 3.0 device */
#define ZB_BCAST_RX_ON      0xFFFDu /* every device whose receiver is on when idle (Zigbee 3.6.5) */
#define ZB_NEW_CHANNEL      0xFEu   /* Mgmt_NWK_Update_req ScanDuration "change channel" (2.4.3.3.9) */
#define ZB_RAW_ASDU_MAX     (5u + ZSRC_ZB_ZCL_PAYLOAD_MAX) /* ZCL header (with a manufacturer) + payload */

/* ZCL general commands (ZCL 2.5): the source's requests and the answers they wait for. */
#define ZCL_READ_ATTR       0x00u
#define ZCL_READ_ATTR_RSP   0x01u
#define ZCL_CONFIG_REP      0x06u
#define ZCL_CONFIG_REP_RSP  0x07u
#define ZCL_DEFAULT_RSP     0x0Bu

/* The cluster commands the source's requests send, client to server, which a Default Response
 * names (ZB14): Identify's Identify (ZCL 3.5.2.3), IAS Zone's Initiate Normal Operation Mode and
 * Initiate Test Mode (ZCL 8.2.2.3). */
#define ZCL_IDENTIFY_CMD    0x00u
#define ZCL_IAS_NORMAL_MODE 0x01u
#define ZCL_IAS_TEST_MODE   0x02u

/* ZCL frame control bits (the same as EZB_ZCL_CMD_FC_*, zcl_common.h:87-141). */
#define ZCL_FC_MANUF        0x04u
#define ZCL_FC_TO_CLIENT    0x08u
#define ZCL_FC_NO_DEFAULT   0x10u

#define EV_FORMED BIT0
#define EV_FAILED BIT1

static uint64_t ms_now(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ---- what the stack learned, for the service's task ----------------------------------------- */

typedef enum {
    EV_DEVICE = 1, /* u.d: an endpoint found or described again */
    EV_GONE,       /* ieee left the network */
    EV_FRAME,      /* ieee said something: it's alive (lqi, rssi of that frame; nwk) */
    EV_ENROLL,     /* ieee/ep asked to enroll (value: IAS zone type) */
    EV_IAS,        /* ieee/ep's IAS zone status (value) */
    EV_BOOL,       /* ieee/ep's On/Off or Occupancy (cluster, value 0/1) */
    EV_VALUE,      /* ieee/ep's measured value in hundredths (cluster, value) */
    EV_BATTERY,    /* ieee's battery percent (value) */
    EV_ENERGY,     /* the channel's energy (value, dBm) */
    EV_REPORT,     /* u.r: for the report sink (BL-143, devices.md §2) */
} ev_kind_t;

/* About 180 bytes: a SIMPLE_DESC or BASIC report is the largest (devices.md §2). */
typedef struct {
    uint8_t kind;
    uint8_t endpoint;
    uint8_t lqi;      /* EV_FRAME: 0..255 as the radio measured it, 0 not measured */
    int8_t rssi;      /* EV_FRAME: dBm, 0 not measured */
    uint16_t cluster;
    uint16_t nwk;     /* EV_FRAME: the short address it came from */
    int32_t value;
    uint64_t ieee;
    union {
        zsrc_zb_desc_t d;
        zsrc_zb_report_t r;
    } u;
} ev_t;

static QueueHandle_t s_events;
static atomic_uint s_dropped;      /* since the pump last said so */
static atomic_uint s_dropped_all;  /* since start: `zigbee status`, the developer link */
static atomic_uint s_lost;         /* alarm-path events lost since the pump last said so */
static atomic_uint s_lost_all;     /* ... since start */
/* The devices whose zone status was lost: the pump reads it back (ZB-003). Written from the
 * stack's task, emptied from the service's. */
static portMUX_TYPE s_reread_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint64_t ieee;
    uint8_t endpoint;
} s_reread[ZB_REREAD_MAX];
static atomic_bool s_trace;
/* A keypad's Arm or Bypass carries its code (zbz_ace.h): its payload never sits in the queue's
 * PSRAM slots, which keep their bytes after a read, but waits here, and the pump wipes it once
 * it has it (devices.md §16). The event in the queue carries the slot's number + 1 in `value`.
 * Written from the stack's task, emptied from the service's. */
static portMUX_TYPE s_code_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR struct {
    bool used;
    uint8_t bytes[ZSRC_ZB_ZCL_PAYLOAD_MAX];
} s_code[ZB_CODE_SLOTS];

void radio_port_set_trace(bool on)
{
    atomic_store(&s_trace, on);
}

/* One line per event the pump hands the source itself (zigbee_bench.c's format). */
static void trace_ev(uint64_t now, const ev_t *e)
{
    static const char *const WHAT[] = {"?",     "device", "gone",  "frame",   "enroll", "ias",
                                       "bool",  "value",  "battery", "energy", "report"};
    const char *what = e->kind < sizeof(WHAT) / sizeof(WHAT[0]) ? WHAT[e->kind] : "?";
    printf("zbtrace %llu < %016llX ev=%s nwk=0x%04X ep=%u cl=0x%04X v=%ld lqi=%u rssi=%d\n",
           (unsigned long long)now, (unsigned long long)e->ieee, what, (unsigned)e->nwk,
           (unsigned)e->endpoint, (unsigned)e->cluster, (long)e->value, (unsigned)e->lqi,
           (int)e->rssi);
}
static void (*s_wake)(void);
/* Set under the zone lock and used under it by the pump; on_raw_zcl (the stack's task, no zone
 * lock) only asks whether there is one. Atomic for that reader: the context is stored before the
 * function, so whoever sees the function sees its context. */
static _Atomic(radio_port_report_fn) s_sink;
static _Atomic(void *) s_sink_ctx;

void radio_port_set_wake(void (*wake)(void))
{
    s_wake = wake;
}

void radio_port_set_report_sink(radio_port_report_fn fn, void *ctx)
{
    if (fn == NULL) {
        atomic_store(&s_sink, NULL);
        atomic_store(&s_sink_ctx, NULL);
        return;
    }
    atomic_store(&s_sink_ctx, ctx);
    atomic_store(&s_sink, fn);
}

static void deliver(uint64_t now, const zsrc_zb_report_t *r)
{
    const radio_port_report_fn fn = atomic_load(&s_sink);
    if (fn != NULL) {
        fn(atomic_load(&s_sink_ctx), now, r);
    }
}

/* The alarm path: what the stack has already acknowledged, so the device never sends it again -
 * a sensor's IAS zone status (the alarm itself), its enroll request, a keypad's or fob's ACE
 * command (Arm with its code, Panic, Emergency) and the ACE answers the keypad policy waits for. */
static bool alarm_path(const ev_t *e)
{
    switch (e->kind) {
    case EV_IAS:
    case EV_ENROLL: return true;
    case EV_REPORT:
        return e->u.r.kind == (uint8_t)ZSRC_ZB_REP_ZCL &&
               e->u.r.u.zcl.cluster == ZSRC_ZB_ZCL_IAS_ACE;
    default: return false;
    }
}

static void reread_note(uint64_t ieee, uint8_t endpoint)
{
    taskENTER_CRITICAL(&s_reread_mux);
    size_t free_at = ZB_REREAD_MAX;
    for (size_t i = 0; i < ZB_REREAD_MAX; i++) {
        if (s_reread[i].ieee == ieee && s_reread[i].endpoint == endpoint) {
            free_at = ZB_REREAD_MAX; /* already asked for */
            break;
        }
        if (s_reread[i].ieee == 0u && free_at == ZB_REREAD_MAX) {
            free_at = i;
        }
    }
    if (free_at < ZB_REREAD_MAX) {
        s_reread[free_at].ieee = ieee;
        s_reread[free_at].endpoint = endpoint;
    }
    taskEXIT_CRITICAL(&s_reread_mux);
}

/* Never blocks the stack. A full queue sheds frames first, then everything but the alarm path,
 * which may use the last ZB_ROOM_ALARM slots; an alarm-path event lost even so is counted apart,
 * logged as an error and, for a zone status, read back from the device (README "Rules"). */
static bool post(const ev_t *e)
{
    const bool urgent = alarm_path(e);
    const UBaseType_t keep =
        urgent ? 0u : (e->kind == (uint8_t)EV_FRAME ? ZB_ROOM_FRAME : ZB_ROOM_ALARM);
    if (s_events == NULL || uxQueueSpacesAvailable(s_events) <= keep ||
        xQueueSend(s_events, e, 0) != pdTRUE) {
        if (!urgent) {
            atomic_fetch_add(&s_dropped, 1u); /* reported by the pump */
            atomic_fetch_add(&s_dropped_all, 1u);
            return false;
        }
        atomic_fetch_add(&s_lost, 1u);
        atomic_fetch_add(&s_lost_all, 1u);
        if (e->kind == (uint8_t)EV_IAS && e->ieee != 0u) {
            reread_note(e->ieee, e->endpoint);
        }
        if (s_wake != NULL) {
            s_wake(); /* the pump says so and asks the device again */
        }
        return false;
    }
    if (s_wake != NULL) {
        s_wake();
    }
    return true;
}

static void wipe(volatile uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        b[i] = 0u; /* never a memset the compiler may drop */
    }
}

/* A code slot for a payload (the stack's task): its number + 1, or 0 when all are taken. */
static int32_t code_park(const uint8_t *p, uint8_t len)
{
    int32_t slot = 0;
    taskENTER_CRITICAL(&s_code_mux);
    for (size_t i = 0; i < ZB_CODE_SLOTS; i++) {
        if (!s_code[i].used) {
            s_code[i].used = true;
            memcpy(s_code[i].bytes, p, len);
            slot = (int32_t)i + 1;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_code_mux);
    return slot;
}

/* The payload back out of its slot (into `to`, when not NULL), and the slot wiped and freed. */
static void code_take(int32_t slot, uint8_t *to, uint8_t len)
{
    if (slot < 1 || slot > (int32_t)ZB_CODE_SLOTS) {
        return;
    }
    const size_t i = (size_t)(slot - 1);
    taskENTER_CRITICAL(&s_code_mux);
    if (to != NULL) {
        memcpy(to, s_code[i].bytes, len);
    }
    wipe(s_code[i].bytes, sizeof(s_code[i].bytes));
    s_code[i].used = false;
    taskEXIT_CRITICAL(&s_code_mux);
}

static void post_simple(ev_kind_t kind, uint64_t ieee, uint8_t ep, uint16_t cluster, int32_t value)
{
    if (ieee == 0u && kind != EV_ENERGY) {
        return; /* a frame from an address the stack can't name */
    }
    ev_t e;
    memset(&e, 0, sizeof(e));
    e.kind = (uint8_t)kind;
    e.endpoint = ep;
    e.cluster = cluster;
    e.value = value;
    e.ieee = ieee;
    post(&e);
}

/* A report to fill in and post(); everything not set is zero. */
static void report_begin(ev_t *e, zsrc_zb_report_kind_t kind, uint64_t ieee, uint16_t nwk,
                         uint8_t ep, uint32_t tag)
{
    memset(e, 0, sizeof(*e));
    e->kind = EV_REPORT;
    e->ieee = ieee;
    e->u.r.kind = (uint8_t)kind;
    e->u.r.ieee = ieee;
    e->u.r.nwk = nwk;
    e->u.r.endpoint = ep;
    e->u.r.tag = tag;
}

/* ---- a radio that restarts the panel: not over and over ------------------------------------- */

/* Kept through a restart (not a power cut), in the always-on RAM: whether the stack was starting
 * when the panel went down, and how many starts in a row the radio has ended since the network
 * last ran for a while. Two of those and Zigbee stays paused until `zigbee retry`, instead of a
 * panel that restarts every time it comes up (a flaky module that answers the probe, then fails
 * the stack's reset or version checks). The count is cleared by GUARD_STABLE_MS of a running
 * network, by a power-on, and by `zigbee retry`. */
#define GUARD_MAGIC       0x5A42474Eu /* "ZBGN": the rest is ours, not what the RAM held at power-on */
#define GUARD_PAUSE_AFTER 2u
#define GUARD_FAILS_MAX   99u
#define GUARD_STABLE_MS   (10u * 60u * 1000u)

typedef struct {
    uint32_t magic;
    uint32_t starting; /* 1 from the stack's task starting until its network is up */
    uint32_t fails;    /* the radio's client ended the panel, or a start crashed it */
    uint32_t why;      /* radio_port_stop_t: what the last one said before it did */
} guard_t;

static RTC_NOINIT_ATTR guard_t s_guard;
static bool s_guard_read;      /* this start's look at s_guard is done */
static atomic_bool s_paused;   /* the stack stays off until radio_port_resume() */
static uint64_t s_formed_at;   /* ms, when the network came up this time (the stack's task) */

/* Once per start, before the stack is started for the first time. */
static void guard_boot(void)
{
    if (s_guard_read) {
        return;
    }
    s_guard_read = true;
    const esp_reset_reason_t why = esp_reset_reason();
    const bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT ||
                         why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
                         why == ESP_RST_CPU_LOCKUP;
    if (s_guard.magic != GUARD_MAGIC || why == ESP_RST_POWERON || why == ESP_RST_BROWNOUT ||
        why == ESP_RST_PWR_GLITCH || why == ESP_RST_EXT || why == ESP_RST_UNKNOWN) {
        memset(&s_guard, 0, sizeof(s_guard));
        s_guard.magic = GUARD_MAGIC;
        return;
    }
    if (s_guard.starting != 0u && crashed && s_guard.fails < GUARD_FAILS_MAX) {
        s_guard.fails++; /* it went down while the stack was starting, without saying why */
    }
    s_guard.starting = 0u;
    if (s_guard.fails >= GUARD_PAUSE_AFTER) {
        atomic_store(&s_paused, true);
        ESP_LOGE(TAG, "the sensor radio ended the panel's last %u starts: Zigbee paused until "
                      "'zigbee retry'", (unsigned)s_guard.fails);
    } else if (s_guard.fails != 0u) {
        ESP_LOGW(TAG, "the sensor radio ended the panel's last start (%s): trying it once more",
                 radio_port_stop_words((radio_port_stop_t)s_guard.why));
    }
}

/* OpenThread's DieNow is exit(): this runs on the stack's task just before the panel restarts. */
static void guard_died(void)
{
    if (s_guard.fails < GUARD_FAILS_MAX) {
        s_guard.fails++;
    }
    s_guard.starting = 0u;
}

/* Said by the Spinel client just before it ends the program (the stack's task). */
static void guard_incompatible(void)
{
    s_guard.why = RADIO_PORT_STOP_INCOMPATIBLE;
    ESP_LOGE(TAG, "the sensor radio runs firmware the Zigbee stack can't use: the panel restarts");
}

static void guard_reset_failed(void)
{
    s_guard.why = RADIO_PORT_STOP_NO_RESET;
    ESP_LOGE(TAG, "the sensor radio did not come back from its reset: the panel restarts");
}

const char *radio_port_stop_words(radio_port_stop_t why)
{
    switch (why) {
    case RADIO_PORT_STOP_INCOMPATIBLE: return "its firmware doesn't suit the stack";
    case RADIO_PORT_STOP_NO_RESET: return "it didn't come back from a reset";
    default: return "it stopped answering";
    }
}

bool radio_port_paused(unsigned *fails, radio_port_stop_t *why)
{
    if (fails != NULL) {
        *fails = s_guard_read ? (unsigned)s_guard.fails : 0u;
    }
    if (why != NULL) {
        *why = s_guard_read ? (radio_port_stop_t)s_guard.why : RADIO_PORT_STOP_NONE;
    }
    return atomic_load(&s_paused);
}

void radio_port_resume(void)
{
    if (s_guard_read) {
        s_guard.fails = 0u;
        s_guard.why = RADIO_PORT_STOP_NONE;
    }
    atomic_store(&s_paused, false);
}

/* ---- the socket's UART before the stack: is a radio co-processor there? --------------------- */

static uint8_t s_tx_gpio;
static uint8_t s_rx_gpio;
static bool s_uart_ours;          /* our own driver is installed (never while the stack runs) */
static atomic_bool s_running;     /* the stack's task is alive and owns the UART */
static char s_version[64];        /* what the radio said it runs, kept for probes after the handover */

esp_err_t radio_port_open(uint8_t tx_gpio, uint8_t rx_gpio)
{
    s_tx_gpio = tx_gpio;
    s_rx_gpio = rx_gpio;
    if (s_uart_ours) {
        return ESP_OK;
    }
    const uart_config_t cfg = {
        .baud_rate = RADIO_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(RADIO_UART_NUM, RADIO_BUF_BYTES, RADIO_BUF_BYTES, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(RADIO_UART_NUM, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(RADIO_UART_NUM, (int)tx_gpio, (int)rx_gpio, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(RADIO_UART_NUM);
        return err;
    }
    s_uart_ours = true;
    return ESP_OK;
}

/* Spinel over HDLC-lite (OpenThread's framing): 0x7E flags, 0x7D escapes (the byte XOR 0x20)
 * for 0x7E 0x7D 0x11 0x13 0xF8, and a CRC-16/X.25 sent low byte first. */
#define HDLC_FLAG   0x7Eu
#define HDLC_ESCAPE 0x7Du

static uint16_t fcs16(uint16_t fcs, uint8_t b)
{
    fcs ^= b;
    for (int i = 0; i < 8; i++) {
        fcs = (fcs & 1u) != 0u ? (uint16_t)((fcs >> 1) ^ 0x8408u) : (uint16_t)(fcs >> 1);
    }
    return fcs;
}

static bool hdlc_special(uint8_t b)
{
    return b == HDLC_FLAG || b == HDLC_ESCAPE || b == 0x11u || b == 0x13u || b == 0xF8u;
}

static size_t hdlc_put(uint8_t *out, size_t at, uint8_t b)
{
    if (hdlc_special(b)) {
        out[at++] = HDLC_ESCAPE;
        out[at++] = (uint8_t)(b ^ 0x20u);
    } else {
        out[at++] = b;
    }
    return at;
}

/* Header 0x81 (flag bits 10, interface 0, TID 1), PROP_VALUE_GET (2), PROP_NCP_VERSION (2). */
static void send_version_request(void)
{
    static const uint8_t payload[] = {0x81u, 0x02u, 0x02u};
    uint8_t out[16];
    size_t n = 0;
    uint16_t fcs = 0xFFFFu;
    out[n++] = HDLC_FLAG;
    for (size_t i = 0; i < sizeof(payload); i++) {
        fcs = fcs16(fcs, payload[i]);
        n = hdlc_put(out, n, payload[i]);
    }
    fcs = (uint16_t)~fcs;
    n = hdlc_put(out, n, (uint8_t)(fcs & 0xFFu));
    n = hdlc_put(out, n, (uint8_t)(fcs >> 8));
    out[n++] = HDLC_FLAG;
    uart_write_bytes(RADIO_UART_NUM, out, n);
}

/* One received frame (unescaped, FCS included). A PROP_VALUE_IS (6) of NCP_VERSION (2) - the
 * answer, or the notice an RCP sends after a reset - carries the firmware's version string. */
static bool frame_version(const uint8_t *f, size_t n, char *version, size_t len)
{
    if (n < 5u) {
        return false;
    }
    uint16_t fcs = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        fcs = fcs16(fcs, f[i]);
    }
    if (fcs != 0xF0B8u) {
        return false; /* the good-frame residue of CRC-16/X.25 */
    }
    const size_t body = n - 2u;
    if ((f[0] & 0xC0u) != 0x80u || f[1] != 0x06u || f[2] != 0x02u) {
        return false;
    }
    size_t k = 0;
    for (size_t i = 3; i < body && f[i] != 0u && k + 1u < len; i++) {
        version[k++] = (char)f[i];
    }
    version[k] = '\0';
    return k > 0u;
}

static zb_probe_t port_probe(void *ctx, char *version, size_t len)
{
    (void)ctx;
    if (atomic_load(&s_running)) {
        /* The stack has the UART now: the radio is the one that answered before it. */
        snprintf(version, len, "%s", s_version);
        return ZB_PROBE_OK;
    }
    if (!s_uart_ours && radio_port_open(s_tx_gpio, s_rx_gpio) != ESP_OK) {
        return ZB_PROBE_SILENT; /* the stack has just let go of it: take it back to ask */
    }
    uart_flush_input(RADIO_UART_NUM);
    send_version_request();
    uint8_t frame[128];
    size_t n = 0;
    bool escaped = false;
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(400);
    uint8_t buf[64];
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        const int got = uart_read_bytes(RADIO_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < got; i++) {
            const uint8_t b = buf[i];
            if (b == HDLC_FLAG) {
                if (n > 0u && frame_version(frame, n, version, len)) {
                    snprintf(s_version, sizeof(s_version), "%s", version);
                    return ZB_PROBE_OK;
                }
                n = 0;
                escaped = false;
            } else if (b == HDLC_ESCAPE) {
                escaped = true;
            } else if (n < sizeof(frame)) {
                frame[n++] = escaped ? (uint8_t)(b ^ 0x20u) : b;
                escaped = false;
            }
        }
    }
    return ZB_PROBE_WAIT; /* the bring-up policy asks again, then calls the socket silent */
}

/* Writing the H2's flash from the panel is step 6 (the socket has no BOOT or RESET line): its
 * first image goes on over the module's own USB-C (firmware/radios/h2_rcp/README.md). */
static bool port_to_bootloader(void *ctx)
{
    (void)ctx;
    return false;
}

static bool port_sync(void *ctx)
{
    (void)ctx;
    return false;
}

static int port_flash_step(void *ctx)
{
    (void)ctx;
    return -1;
}

static void port_flash_reset(void *ctx)
{
    (void)ctx;
}

static bool port_restart(void *ctx)
{
    (void)ctx;
    return false;
}

const char *radio_port_image_version(void)
{
    return ""; /* this firmware carries no H2 image: whatever answers is used as it is (ZB-001) */
}

void radio_port_boot_port(zb_boot_port_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ctx = NULL;
    out->probe = port_probe;
    out->to_bootloader = port_to_bootloader;
    out->sync = port_sync;
    out->flash_step = port_flash_step;
    out->flash_reset = port_flash_reset;
    out->restart = port_restart;
}

/* ---- addresses ------------------------------------------------------------------------------ */

static uint64_t ieee_of(const ezb_address_t *a)
{
    if (a->addr_mode == EZB_ADDR_MODE_EXT) {
        return a->u.extended_addr.u64;
    }
    if (a->addr_mode == EZB_ADDR_MODE_SHORT) {
        ezb_extaddr_t ext;
        if (ezb_address_extended_by_short(a->u.short_addr, &ext) == EZB_ERR_NONE) {
            return ext.u64;
        }
    }
    return 0u;
}

static uint16_t nwk_of(uint64_t ieee)
{
    ezb_extaddr_t ext;
    ext.u64 = ieee;
    ezb_shortaddr_t nwk = ZB_NO_NWK;
    if (ezb_address_short_by_extended(&ext, &nwk) != EZB_ERR_NONE) {
        return ZB_NO_NWK;
    }
    return nwk;
}

/* The short address a received frame came from (ZB_NO_NWK when the stack can't say). */
static uint16_t nwk_in(const ezb_address_t *a)
{
    if (a->addr_mode == EZB_ADDR_MODE_SHORT) {
        return a->u.short_addr;
    }
    if (a->addr_mode == EZB_ADDR_MODE_EXT) {
        return nwk_of(a->u.extended_addr.u64);
    }
    return ZB_NO_NWK;
}

/* Where to send to `ieee`: the stack's address map first (a rejoin may have moved it), else the
 * short address the caller knows. */
static uint16_t nwk_for(uint64_t ieee, uint16_t known)
{
    const uint16_t nwk = nwk_of(ieee);
    if (nwk < ZB_NO_NWK) {
        return nwk;
    }
    return known != 0u && known < ZB_NO_NWK ? known : ZB_NO_NWK;
}

static ezb_address_t short_address(uint16_t nwk)
{
    ezb_address_t a;
    memset(&a, 0, sizeof(a));
    a.addr_mode = EZB_ADDR_MODE_SHORT;
    a.u.short_addr = nwk;
    return a;
}

/* ---- link quality (ZB-011): the APS indication carries it, the ZCL callbacks don't ---------- */

/* Only the stack's task touches these: written by the APS indication, read by the callbacks the
 * same frame reaches next. */
typedef struct {
    uint16_t nwk;
    uint8_t lqi;
    int8_t rssi;
} link_t;

static link_t s_links[ZB_LINKS];
static unsigned s_links_next;

static void link_note(uint16_t nwk, uint8_t lqi, int8_t rssi)
{
    link_t *slot = NULL;
    for (size_t i = 0; i < ZB_LINKS; i++) {
        if (s_links[i].nwk == nwk && (s_links[i].lqi != 0u || s_links[i].rssi != 0)) {
            slot = &s_links[i];
            break;
        }
    }
    if (slot == NULL) {
        slot = &s_links[s_links_next++ % ZB_LINKS];
    }
    slot->nwk = nwk;
    slot->lqi = lqi;
    slot->rssi = rssi;
}

static const link_t *link_of(uint16_t nwk)
{
    for (size_t i = 0; i < ZB_LINKS; i++) {
        if (s_links[i].nwk == nwk && (s_links[i].lqi != 0u || s_links[i].rssi != 0)) {
            return &s_links[i];
        }
    }
    return NULL;
}

/* Every frame the stack takes passes here first (aps.h:101-143): its LQI and RSSI are kept for
 * the callback it reaches next. False: the stack goes on with the frame as before. */
static bool on_aps_indication(const ezb_apsde_data_ind_t *ind)
{
    if (ind != NULL && ind->status == 0u) {
        const uint16_t nwk = nwk_in(&ind->src_address);
        if (nwk < ZB_NO_NWK) {
            link_note(nwk, ind->lqi, ind->rssi);
        }
    }
    return false;
}

/* A frame from a device: it is alive, with the frame's LQI and RSSI (the ZCL header's RSSI when
 * the APS indication said nothing about this address). */
static void post_frame(uint64_t ieee, uint16_t nwk, uint8_t ep, uint16_t cluster, int8_t rssi)
{
    if (ieee == 0u) {
        return;
    }
    ev_t e;
    memset(&e, 0, sizeof(e));
    e.kind = EV_FRAME;
    e.ieee = ieee;
    e.endpoint = ep;
    e.cluster = cluster;
    e.nwk = nwk;
    const link_t *l = nwk < ZB_NO_NWK ? link_of(nwk) : NULL;
    if (l != NULL) {
        e.lqi = l->lqi;
        e.rssi = l->rssi;
    } else {
        e.rssi = rssi;
    }
    post(&e);
}

/* ---- the trust center's view: install codes held, how each device joined (ZB-015, ZB-021) ---- */

/* Touched with the stack's lock held: the hooks, and the signals on the stack's task. */
static uint64_t s_ic[ZB_IC_MAX];  /* devices the trust center holds an install code for */
static bool s_codes_only;         /* ezb_secur_set_ic_required(true) is in force */

static bool ic_has(uint64_t ieee)
{
    for (size_t i = 0; i < ZB_IC_MAX; i++) {
        if (s_ic[i] == ieee) {
            return true;
        }
    }
    return false;
}

static void ic_note(uint64_t ieee, bool held)
{
    size_t free_slot = ZB_IC_MAX;
    for (size_t i = 0; i < ZB_IC_MAX; i++) {
        if (s_ic[i] == ieee) {
            if (!held) {
                s_ic[i] = 0u;
            }
            return;
        }
        if (s_ic[i] == 0u && free_slot == ZB_IC_MAX) {
            free_slot = i;
        }
    }
    if (held && free_slot < ZB_IC_MAX) {
        s_ic[free_slot] = ieee;
    }
}

/* The trust center's install code for a device, dropped (the stack's lock held). The mirror
 * follows only what the trust center confirms: removed, or already absent (NOT_FOUND, secur.h:
 * 78-86). Any other failure keeps it, so the next removal or forget asks the stack again and
 * join_key_of() keeps telling what the trust center still holds (ZB12). */
static bool ic_remove(uint64_t ieee)
{
    ezb_extaddr_t ext;
    ext.u64 = ieee;
    const ezb_err_t err = ezb_secur_ic_remove(&ext);
    const bool gone = err == EZB_ERR_NONE || err == EZB_ERR_NOT_FOUND;
    if (gone) {
        ic_note(ieee, false);
    }
    return gone;
}

/* The joins and rejoins the trust center just saw, until the device's announce and
 * authorization have used them. */
typedef struct {
    uint64_t ieee;
    uint16_t parent;
    uint8_t join;      /* zsrc_zb_join_t */
    bool rejoin;
    bool reported;     /* REP_JOINED is out */
} joiner_t;

static joiner_t s_joiners[ZB_ASKING];
static unsigned s_joiners_next;

static joiner_t *joiner_for(uint64_t ieee, bool create)
{
    for (size_t i = 0; i < ZB_ASKING; i++) {
        if (s_joiners[i].ieee == ieee) {
            return &s_joiners[i];
        }
    }
    if (!create) {
        return NULL;
    }
    joiner_t *j = &s_joiners[s_joiners_next++ % ZB_ASKING];
    memset(j, 0, sizeof(*j));
    j->ieee = ieee;
    return j;
}

/* Which key a device that has just joined used. esp-zigbee-lib does not say (the authorization
 * signal carries only legacy-or-TCLK and a status: app_signals.h:463-468, the update signal only
 * joined-or-rejoined: :452-458). The trust center uses the key derived from an install code for
 * an address it holds one for (BDB 10.2.4 and ezb_secur_ic_add, secur.h:67-76), and a joiner
 * without that key can't read the network key it is sent: a device that got in while a code was
 * held for it used the code; one that got in without, the Zigbee 3.0 well-known key. s_ic is
 * RAM only: this holds while zbz_panel gives every waiting code again at each start
 * (zbz_panel.h, zbz_panel_radio), and is a must-check on the bench (README "Rules"). */
static uint8_t join_key_of(uint64_t ieee)
{
    return (uint8_t)(ic_has(ieee) ? ZSRC_ZB_JOIN_INSTALL_CODE : ZSRC_ZB_JOIN_STANDARD_KEY);
}

/* ---- the source's requests, until they end (BL-143, devices.md §3) -------------------------- */

typedef enum {
    RQ_ZDO = 1, /* a ZDO request: its callback ends it (user_ctx = serial) */
    RQ_ZCL,     /* a ZCL request: its answer, matched by sequence number, ends it */
    RQ_LEAVE,   /* Mgmt_Leave_req: its answer, or the device's leave */
    RQ_CHANNEL, /* the network moving: the panel's own radio on `channel` */
    RQ_RESET,   /* the network left and formed again */
    RQ_SURVEY,  /* energy of the four channels */
} rq_kind_t;

typedef struct {
    uint32_t serial;   /* 0: free; what a callback's user_ctx carries */
    uint32_t tag;      /* the source's */
    uint64_t ieee;
    uint64_t deadline; /* ms: then it ends TIMEOUT */
    uint16_t nwk;
    uint16_t cluster;
    uint8_t kind;      /* rq_kind_t */
    uint8_t step;      /* zsrc_zb_step_t */
    uint8_t ep;
    uint8_t expect;    /* RQ_ZCL: the answer's command (a Default Response ends any) */
    uint8_t sent;      /* RQ_ZCL: the command it sent, which a Default Response that ends it names
                        * (its rsp_to_cmd, zcl_general_cmd.h:451-460; ZB14) */
    uint8_t tsn;
    bool tsn_known;    /* from the send confirmation, or ours (raw frames) */
    bool quiet;        /* RQ_ZCL asking for no answer: it ends when sent */
    bool ep_told;      /* RQ_ZCL: an answer from another endpoint was logged (once) */
    uint8_t channel;   /* RQ_CHANNEL */
} rq_t;

/* Over 600 bytes, touched only by tasks (never an ISR): PSRAM. */
static EXT_RAM_BSS_ATTR rq_t s_rq[ZB_RQ_MAX];
static uint32_t s_rq_serial;
static atomic_uint s_rq_live;
static bool s_resetting; /* reset_network: between the local reset and the new network */

static rq_t *rq_new(rq_kind_t kind, uint32_t tag, uint64_t ieee, uint16_t nwk, uint8_t ep,
                    uint16_t cluster, uint8_t step, uint32_t wait_ms)
{
    for (size_t i = 0; i < ZB_RQ_MAX; i++) {
        rq_t *q = &s_rq[i];
        if (q->serial != 0u) {
            continue;
        }
        memset(q, 0, sizeof(*q));
        if (++s_rq_serial == 0u) {
            s_rq_serial = 1u;
        }
        q->serial = s_rq_serial;
        q->tag = tag;
        q->ieee = ieee;
        q->nwk = nwk;
        q->ep = ep;
        q->cluster = cluster;
        q->kind = (uint8_t)kind;
        q->step = step;
        q->deadline = ms_now() + wait_ms;
        atomic_fetch_add(&s_rq_live, 1u);
        return q;
    }
    ESP_LOGW(TAG, "too many radio requests waiting: one refused");
    return NULL;
}

static void rq_free(rq_t *q)
{
    if (q != NULL && q->serial != 0u) {
        q->serial = 0u;
        atomic_fetch_sub(&s_rq_live, 1u);
    }
}

static rq_t *rq_by_serial(uint32_t serial)
{
    if (serial == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < ZB_RQ_MAX; i++) {
        if (s_rq[i].serial == serial) {
            return &s_rq[i];
        }
    }
    return NULL; /* it timed out meanwhile */
}

static rq_t *rq_of_kind(rq_kind_t kind, uint64_t ieee)
{
    for (size_t i = 0; i < ZB_RQ_MAX; i++) {
        if (s_rq[i].serial != 0u && s_rq[i].kind == (uint8_t)kind &&
            (ieee == 0u || s_rq[i].ieee == ieee)) {
            return &s_rq[i];
        }
    }
    return NULL;
}

static bool rq_waiting_for(uint64_t ieee)
{
    return ieee != 0u && (rq_of_kind(RQ_ZDO, ieee) != NULL || rq_of_kind(RQ_ZCL, ieee) != NULL ||
                          rq_of_kind(RQ_LEAVE, ieee) != NULL);
}

/* The answer to a ZCL request: same device, endpoint and cluster, its sequence number once the
 * send confirmation has told it (an answer before the confirmation matches the oldest such
 * request). A Default Response ends any request (a device refuses a whole read with one), but
 * only one that names the command the request sent (ZB14): a reply about another command leaves
 * it waiting for its own answer, or its time. */
/* The sequence numbers of the panel's own untagged ZCL sends (the join interview's reads, the
 * IAS CIE write, the enrollment response, the legacy read_attribute), from their send
 * confirmations: an answer to one of them is never taken for a tagged request's answer. Only the
 * stack's task touches it (its confirmations and answers). The one gap left: an untagged answer
 * that arrives before its own send confirmation, which the stack gives first in practice. */
typedef struct {
    uint64_t ieee;
    uint16_t cluster;
    uint8_t tsn;
} own_tsn_t;

#define ZB_OWN_TSN 16u
static EXT_RAM_BSS_ATTR own_tsn_t s_own_tsn[ZB_OWN_TSN]; /* 256 bytes, tasks only: PSRAM */
static unsigned s_own_tsn_next;

/* An untagged send's confirmation (af.h:210-218: its tsn, destination and cluster). */
static void on_own_cnf(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    (void)user_ctx;
    if (cnf == NULL || cnf->status != 0u) {
        return;
    }
    const uint64_t ieee = ieee_of(&cnf->dst_addr);
    if (ieee == 0u) {
        return;
    }
    own_tsn_t *o = &s_own_tsn[s_own_tsn_next++ % ZB_OWN_TSN];
    o->ieee = ieee;
    o->cluster = cnf->cluster_id;
    o->tsn = cnf->tsn;
}

/* True, and forgotten, when the answer is to one of the panel's own untagged sends. */
static bool own_tsn_take(uint64_t ieee, uint16_t cluster, uint8_t tsn)
{
    for (size_t i = 0; i < ZB_OWN_TSN; i++) {
        own_tsn_t *o = &s_own_tsn[i];
        if (o->ieee == ieee && o->cluster == cluster && o->tsn == tsn) {
            o->ieee = 0u;
            return true;
        }
    }
    return false;
}

static rq_t *rq_match_zcl(uint64_t ieee, uint8_t ep, uint16_t cluster, uint8_t tsn, uint8_t cmd,
                          uint8_t rsp_to_cmd)
{
    rq_t *loose = NULL;
    rq_t *elsewhere = NULL; /* its answer in all but the endpoint, sequence number included */
    for (size_t i = 0; i < ZB_RQ_MAX; i++) {
        rq_t *q = &s_rq[i];
        if (q->serial == 0u || q->kind != (uint8_t)RQ_ZCL || q->ieee != ieee ||
            q->cluster != cluster ||
            (cmd == ZCL_DEFAULT_RSP ? q->sent != rsp_to_cmd : q->expect != cmd)) {
            continue;
        }
        if (q->tsn_known && q->tsn != tsn) {
            continue;
        }
        if (q->ep != ep) {
            if (q->tsn_known && elsewhere == NULL) {
                elsewhere = q;
            }
            continue;
        }
        if (q->tsn_known) {
            return q;
        }
        if (loose == NULL || q->serial < loose->serial) {
            loose = q;
        }
    }
    if (loose != NULL && own_tsn_take(ieee, cluster, tsn)) {
        return NULL; /* the answer to an untagged send: the tagged request still waits */
    }
    if (loose == NULL && elsewhere != NULL && !elsewhere->ep_told) {
        /* Kept waiting (the endpoint is part of the match), but said once on the bench: a
         * device that answers from another endpoint than it was asked on times out here. */
        elsewhere->ep_told = true;
        ESP_LOGW(TAG, "%016llx answered cluster 0x%04x (command 0x%02x) from endpoint %u, asked on "
                      "%u: the request waits for its own answer",
                 (unsigned long long)ieee, (unsigned)cluster, (unsigned)cmd, (unsigned)ep,
                 (unsigned)elsewhere->ep);
    }
    return loose;
}

static void done_fill(zsrc_zb_report_t *r, const rq_t *q, uint8_t result, uint8_t zcl_status)
{
    memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)ZSRC_ZB_REP_DONE;
    r->ieee = q->ieee;
    r->nwk = q->nwk;
    r->endpoint = q->ep;
    r->tag = q->tag;
    r->u.done.step = q->step;
    r->u.done.result = result;
    r->u.done.zcl_status = result == (uint8_t)ZSRC_ZB_DONE_REFUSED ? zcl_status : 0u;
}

/* End a request from the stack's side (a callback, or a hook that ends it at once): REP_DONE
 * through the queue, after whatever its answer reported. */
static void rq_post_done(rq_t *q, uint8_t result, uint8_t zcl_status)
{
    if (q == NULL || q->serial == 0u) {
        return;
    }
    ev_t e;
    memset(&e, 0, sizeof(e));
    e.kind = EV_REPORT;
    e.ieee = q->ieee;
    done_fill(&e.u.r, q, result, zcl_status);
    rq_free(q);
    post(&e);
}

static uint8_t zdo_result(ezb_err_t error, uint8_t status, uint8_t *zstatus)
{
    *zstatus = 0u;
    if (error == EZB_ERR_TIMEOUT) {
        return ZSRC_ZB_DONE_TIMEOUT;
    }
    if (error != EZB_ERR_NONE) {
        return ZSRC_ZB_DONE_SEND_FAILED;
    }
    switch (status) {
    case EZB_ZDP_STATUS_SUCCESS: return ZSRC_ZB_DONE_OK;
    case EZB_ZDP_STATUS_TIMEOUT: return ZSRC_ZB_DONE_TIMEOUT;
    case EZB_ZDP_STATUS_NOT_SUPPORTED:
    case EZB_ZDP_STATUS_NOT_ACTIVE:
    case EZB_ZDP_STATUS_INVALID_EP:
    case EZB_ZDP_STATUS_NO_DESCRIPTOR: return ZSRC_ZB_DONE_UNSUPPORTED;
    default:
        *zstatus = status;
        return ZSRC_ZB_DONE_REFUSED;
    }
}

static void zdo_end(rq_t *q, ezb_err_t error, uint8_t status)
{
    uint8_t zstatus = 0u;
    const uint8_t result = zdo_result(error, status, &zstatus);
    rq_post_done(q, result, zstatus);
}

static uint8_t zcl_result(uint8_t status)
{
    switch (status) {
    case EZB_ZCL_STATUS_SUCCESS: return ZSRC_ZB_DONE_OK;
    case EZB_ZCL_STATUS_UNSUP_CMD:
    case EZB_ZCL_STATUS_UNSUP_ATTRIB:
    case EZB_ZCL_STATUS_UNREPORTBLE_ATTRIB:
    case EZB_ZCL_STATUS_UNSUPPORTED_CLUSTER: return ZSRC_ZB_DONE_UNSUPPORTED;
    default: return ZSRC_ZB_DONE_REFUSED;
    }
}

/* The send confirmation of a ZCL request (zcl_common.h:19-27, af.h:210-232): its sequence number,
 * or the end when it could not be sent. */
static void on_zcl_cnf(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    rq_t *q = rq_by_serial((uint32_t)(uintptr_t)user_ctx);
    if (q == NULL || cnf == NULL) {
        return;
    }
    if (cnf->status != 0u) {
        rq_post_done(q, ZSRC_ZB_DONE_SEND_FAILED, 0u);
        return;
    }
    q->tsn = cnf->tsn;
    q->tsn_known = true;
    if (q->quiet) {
        rq_post_done(q, ZSRC_ZB_DONE_OK, 0u);
    }
}

/* The confirmation context of a ZCL send (zcl_common.h:56, :68; af.h:229-232): a tagged
 * request's serial, or, untagged (q NULL), the note of its sequence number (on_own_cnf). */
static ezb_zcl_cmd_cnf_ctx_t cnf_of(const rq_t *q)
{
    ezb_zcl_cmd_cnf_ctx_t c;
    memset(&c, 0, sizeof(c));
    if (q != NULL) {
        c.cb = on_zcl_cnf;
        c.user_ctx = (void *)(uintptr_t)q->serial;
    } else {
        c.cb = on_own_cnf;
    }
    return c;
}

/* ---- devices joining: what they are (on the stack's task) ----------------------------------- */

/* The endpoints described so far, so a Basic cluster answer can be added to what was found. */
typedef struct {
    bool used;
    zsrc_zb_desc_t d;
} known_t;

static EXT_RAM_BSS_ATTR known_t s_known[ZSRC_ZB_DEV_MAX];

/* Devices whose endpoints are being asked for: what their announcement said. */
typedef struct {
    uint16_t nwk;
    uint8_t capability;
    uint8_t basic_ep;   /* the endpoint Basic was read from (0: not yet) */
    uint64_t ieee;
} asking_t;

static asking_t s_asking[ZB_ASKING];
static unsigned s_asking_next;

static known_t *known_for(uint64_t ieee, uint8_t ep, bool create)
{
    known_t *free_slot = NULL;
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        if (s_known[i].used && s_known[i].d.ieee == ieee && s_known[i].d.endpoint == ep) {
            return &s_known[i];
        }
        if (!s_known[i].used && free_slot == NULL) {
            free_slot = &s_known[i];
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->d.zone_type = ZSRC_ZB_ZONE_TYPE_NONE;
    /* What another endpoint of the same device already said (Basic answers once per device). */
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        if (&s_known[i] != free_slot && s_known[i].used && s_known[i].d.ieee == ieee) {
            memcpy(free_slot->d.manufacturer, s_known[i].d.manufacturer,
                   sizeof(free_slot->d.manufacturer));
            memcpy(free_slot->d.model, s_known[i].d.model, sizeof(free_slot->d.model));
            free_slot->d.router = s_known[i].d.router;
            free_slot->d.join = s_known[i].d.join;
            break;
        }
    }
    return free_slot;
}

static void forget_device(uint64_t ieee)
{
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        if (s_known[i].used && s_known[i].d.ieee == ieee) {
            s_known[i].used = false;
        }
    }
}

static asking_t *asking_for(uint16_t nwk)
{
    for (size_t i = 0; i < ZB_ASKING; i++) {
        if (s_asking[i].ieee != 0u && s_asking[i].nwk == nwk) {
            return &s_asking[i];
        }
    }
    return NULL;
}

/* The device behind a short address a ZDO answer is about. */
static uint64_t ieee_for(uint16_t nwk)
{
    const asking_t *a = asking_for(nwk);
    if (a != NULL) {
        return a->ieee;
    }
    ezb_address_t addr = short_address(nwk);
    return ieee_of(&addr);
}

static uint16_t cluster_bit(uint16_t cluster)
{
    switch (cluster) {
    case ZSRC_ZB_ZCL_IAS_ZONE: return ZSRC_ZB_CL_IAS_ZONE;
    case ZSRC_ZB_ZCL_ON_OFF: return ZSRC_ZB_CL_ON_OFF;
    case ZSRC_ZB_ZCL_OCCUPANCY: return ZSRC_ZB_CL_OCCUPANCY;
    case ZSRC_ZB_ZCL_TEMP_MEAS: return ZSRC_ZB_CL_TEMP;
    case ZSRC_ZB_ZCL_POWER_CFG: return ZSRC_ZB_CL_POWER_CFG;
    case ZSRC_ZB_ZCL_POLL_CTRL: return ZSRC_ZB_CL_POLL_CTRL;
    case ZSRC_ZB_ZCL_TUYA: return ZSRC_ZB_CL_TUYA;
    case ZSRC_ZB_ZCL_IAS_WD: return ZSRC_ZB_CL_IAS_WD;
    case ZSRC_ZB_ZCL_IAS_ACE: return ZSRC_ZB_CL_IAS_ACE;
    case ZSRC_ZB_ZCL_IDENTIFY: return ZSRC_ZB_CL_IDENTIFY;
    case ZSRC_ZB_ZCL_BASIC: return ZSRC_ZB_CL_BASIC;
    case ZSRC_ZB_ZCL_OTA: return ZSRC_ZB_CL_OTA;
    case ZSRC_ZB_ZCL_DIAGNOSTICS: return ZSRC_ZB_CL_DIAGNOSTICS;
    default: return 0u;
    }
}

static bool read_attrs(uint16_t nwk, uint8_t ep, uint16_t cluster, uint16_t *ids, uint8_t n,
                       const rq_t *q)
{
    ezb_zcl_read_attr_cmd_t req;
    memset(&req, 0, sizeof(req));
    req.cmd_ctrl.dst_addr = short_address(nwk);
    req.cmd_ctrl.dst_ep = ep;
    req.cmd_ctrl.src_ep = ZB_EP;
    req.cmd_ctrl.cluster_id = cluster;
    req.cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE;
    req.cmd_ctrl.cnf_ctx = cnf_of(q);
    req.payload.attr_number = n;
    req.payload.attr_field = ids;
    return ezb_zcl_read_attr_cmd_req(&req) == EZB_ERR_NONE;
}

/* The ZDO questions, untagged (serial 0: the panel's own interview after a join) or for one of
 * the source's requests. */
static void on_node_desc(const ezb_zdo_node_desc_req_result_t *result, void *user_ctx);
static void on_power_desc(const ezb_zdo_power_desc_req_result_t *result, void *user_ctx);
static void on_active_ep(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx);
static void on_simple_desc(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx);

static bool zdo_node_desc(uint16_t nwk, uint32_t serial)
{
    ezb_zdo_node_desc_req_t req;
    memset(&req, 0, sizeof(req));
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.cb = on_node_desc;
    req.user_ctx = (void *)(uintptr_t)serial;
    return ezb_zdo_node_desc_req(&req) == EZB_ERR_NONE;
}

static bool zdo_power_desc(uint16_t nwk, uint32_t serial)
{
    ezb_zdo_power_desc_req_t req;
    memset(&req, 0, sizeof(req));
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.cb = on_power_desc;
    req.user_ctx = (void *)(uintptr_t)serial;
    return ezb_zdo_power_desc_req(&req) == EZB_ERR_NONE;
}

static bool zdo_active_ep(uint16_t nwk, uint32_t serial)
{
    ezb_zdo_active_ep_req_t req;
    memset(&req, 0, sizeof(req));
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.cb = on_active_ep;
    req.user_ctx = (void *)(uintptr_t)serial;
    return ezb_zdo_active_ep_req(&req) == EZB_ERR_NONE;
}

static bool zdo_simple_desc(uint16_t nwk, uint8_t ep, uint32_t serial)
{
    ezb_zdo_simple_desc_req_t req;
    memset(&req, 0, sizeof(req));
    req.dst_nwk_addr = nwk;
    req.field.nwk_addr_of_interest = nwk;
    req.field.endpoint = ep;
    req.cb = on_simple_desc;
    req.user_ctx = (void *)(uintptr_t)serial;
    return ezb_zdo_simple_desc_req(&req) == EZB_ERR_NONE;
}

static void on_node_desc(const ezb_zdo_node_desc_req_result_t *result, void *user_ctx)
{
    rq_t *q = rq_by_serial((uint32_t)(uintptr_t)user_ctx);
    const ezb_zdp_node_desc_rsp_field_t *rsp = result != NULL ? result->rsp : NULL;
    if (result != NULL && result->error == EZB_ERR_NONE && rsp != NULL &&
        rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        const uint16_t nwk = rsp->nwk_addr_of_interest;
        const uint64_t ieee = q != NULL ? q->ieee : ieee_for(nwk);
        if (ieee != 0u) {
            post_frame(ieee, nwk, 0u, 0u, 0);
            ev_t e;
            report_begin(&e, ZSRC_ZB_REP_NODE_DESC, ieee, nwk, 0u, q != NULL ? q->tag : 0u);
            const ezb_af_node_desc_t *nd = &rsp->node_desc;
            /* Logical type: node descriptor bits 0-2 (Zigbee 2.3.2.3.1). */
            e.u.r.u.node.logical_type = (uint8_t)(nd->node_flags & 0x07u);
            e.u.r.u.node.mac_caps = nd->mac_capability_flags;
            e.u.r.u.node.manufacturer_code = nd->manufacturer_code;
            e.u.r.u.node.server_mask = nd->server_mask;
            e.u.r.u.node.max_in_transfer = nd->max_incoming_transfer_size;
            post(&e);
        }
    }
    if (q != NULL) {
        zdo_end(q, result != NULL ? result->error : EZB_ERR_FAIL, rsp != NULL ? rsp->status : 0u);
    }
}

static void on_power_desc(const ezb_zdo_power_desc_req_result_t *result, void *user_ctx)
{
    rq_t *q = rq_by_serial((uint32_t)(uintptr_t)user_ctx);
    const ezb_zdp_power_desc_rsp_field_t *rsp = result != NULL ? result->rsp : NULL;
    if (result != NULL && result->error == EZB_ERR_NONE && rsp != NULL &&
        rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        const uint16_t nwk = rsp->nwk_addr_of_interest;
        const uint64_t ieee = q != NULL ? q->ieee : ieee_for(nwk);
        if (ieee != 0u) {
            post_frame(ieee, nwk, 0u, 0u, 0);
            ev_t e;
            report_begin(&e, ZSRC_ZB_REP_POWER_DESC, ieee, nwk, 0u, q != NULL ? q->tag : 0u);
            const ezb_af_node_power_desc_t *pd = &rsp->power_desc; /* af.h:94-108 */
            e.u.r.u.power.current_mode = (uint8_t)pd->current_power_mode;
            e.u.r.u.power.available_sources = (uint8_t)pd->available_power_sources;
            e.u.r.u.power.current_source = (uint8_t)pd->current_power_source;
            e.u.r.u.power.level = (uint8_t)pd->current_power_source_level;
            post(&e);
        }
    }
    if (q != NULL) {
        zdo_end(q, result != NULL ? result->error : EZB_ERR_FAIL, rsp != NULL ? rsp->status : 0u);
    }
}

static void on_active_ep(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    const uint32_t serial = (uint32_t)(uintptr_t)user_ctx;
    rq_t *q = rq_by_serial(serial);
    const ezb_zdp_active_ep_rsp_field_t *rsp = result != NULL ? result->rsp : NULL;
    if (result != NULL && result->error == EZB_ERR_NONE && rsp != NULL &&
        rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        const uint16_t nwk = rsp->nwk_addr_of_interest;
        const uint64_t ieee = q != NULL ? q->ieee : ieee_for(nwk);
        if (ieee != 0u) {
            post_frame(ieee, nwk, 0u, 0u, 0);
            ev_t e;
            report_begin(&e, ZSRC_ZB_REP_ACTIVE_EP, ieee, nwk, 0u, q != NULL ? q->tag : 0u);
            for (uint8_t i = 0; i < rsp->active_ep_count && rsp->active_ep_list != NULL &&
                                e.u.r.u.active.count < ZSRC_ZB_EP_MAX;
                 i++) {
                e.u.r.u.active.ep[e.u.r.u.active.count++] = rsp->active_ep_list[i];
            }
            post(&e);
        }
    }
    if (q != NULL) {
        zdo_end(q, result != NULL ? result->error : EZB_ERR_FAIL, rsp != NULL ? rsp->status : 0u);
    }
}

static void on_simple_desc(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx)
{
    const uint32_t serial = (uint32_t)(uintptr_t)user_ctx;
    rq_t *q = rq_by_serial(serial);
    const ezb_zdp_simple_desc_rsp_field_t *rsp = result != NULL ? result->rsp : NULL;
    if (result == NULL || result->error != EZB_ERR_NONE || rsp == NULL ||
        rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        if (q != NULL) {
            zdo_end(q, result != NULL ? result->error : EZB_ERR_FAIL,
                    rsp != NULL ? rsp->status : 0u);
        }
        return;
    }
    const ezb_af_simple_desc_t *sd = &rsp->desc;
    const uint16_t nwk = rsp->nwk_addr_of_interest;
    asking_t *a = asking_for(nwk);
    const uint64_t ieee = q != NULL ? q->ieee : ieee_for(nwk);
    if (ieee == 0u) {
        if (q != NULL) {
            zdo_end(q, EZB_ERR_NONE, EZB_ZDP_STATUS_SUCCESS);
        }
        return;
    }
    post_frame(ieee, nwk, sd->ep_id, 0u, 0);

    /* For the registry: the lists as they came (devices.md §2). */
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_SIMPLE_DESC, ieee, nwk, sd->ep_id, q != NULL ? q->tag : 0u);
    zsrc_zb_simple_desc_t *s = &e.u.r.u.simple;
    s->endpoint = sd->ep_id;
    s->profile = sd->app_profile_id;
    s->device_id = sd->app_device_id;
    s->version = (uint8_t)sd->app_device_version;
    uint16_t clusters = 0u;
    for (unsigned i = 0; sd->app_cluster_list != NULL &&
                         i < (unsigned)sd->app_input_cluster_count + sd->app_output_cluster_count;
         i++) {
        const uint16_t id = sd->app_cluster_list[i];
        const bool in = i < sd->app_input_cluster_count; /* inputs first (af.h:119-124) */
        if (in) {
            clusters |= cluster_bit(id);
            if (s->in_count < ZSRC_ZB_EP_CLUSTERS_MAX) {
                s->in[s->in_count++] = id;
            } else {
                s->truncated = true;
            }
        } else if (s->out_count < ZSRC_ZB_EP_CLUSTERS_MAX) {
            s->out[s->out_count++] = id;
        } else {
            s->truncated = true;
        }
    }
    post(&e);

    /* For the source, as before: the endpoint and what it can report. */
    known_t *k = known_for(ieee, sd->ep_id, true);
    if (k == NULL) {
        ESP_LOGW(TAG, "no room to describe another device endpoint");
    } else {
        k->d.ieee = ieee;
        k->d.nwk = nwk;
        k->d.endpoint = sd->ep_id;
        if (a != NULL) {
            k->d.router = (a->capability & ZSRC_ZB_MAC_FFD) != 0u; /* MAC capability: full function */
        }
        const joiner_t *j = joiner_for(ieee, false);
        if (j != NULL) {
            k->d.join = j->join;
        }
        k->d.clusters = clusters;
        ev_t dev;
        memset(&dev, 0, sizeof(dev));
        dev.kind = EV_DEVICE;
        dev.ieee = ieee;
        dev.endpoint = sd->ep_id;
        dev.u.d = k->d;
        post(&dev);
    }
    if (q != NULL) {
        zdo_end(q, EZB_ERR_NONE, EZB_ZDP_STATUS_SUCCESS); /* Basic and IAS Zone: the setup's own
                                                           * steps (zbz_panel) */
    }
}

/* After a join or rejoin (its announce): what it said of itself, for the answers to the setup's
 * questions (its short address, whether it routes). Decided in ZB-M6 (devices.md §5 "The join
 * interview"): the panel no longer asks a joiner anything itself - zbz_panel's tagged setup asks
 * every question once, and the source learns the endpoints from those answers. */
static void note_announce(uint16_t nwk, uint64_t ieee, uint8_t capability)
{
    asking_t *a = asking_for(nwk);
    if (a == NULL || a->ieee != ieee) {
        a = &s_asking[s_asking_next++ % ZB_ASKING];
    }
    a->nwk = nwk;
    a->ieee = ieee;
    a->capability = capability;
    a->basic_ep = 0u;
}

/* ---- the stack's signals and messages (on the stack's task) --------------------------------- */

static EventGroupHandle_t s_ev;
static atomic_bool s_formed;
static atomic_uint s_channel;
static atomic_uint s_pan_id;
static esp_timer_handle_t s_retry_timer;
static unsigned s_retries; /* failed starts or formations in a row, on the stack's task */

/* The network as it is now (the stack's lock held). */
static void network_fill(zsrc_zb_report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)ZSRC_ZB_REP_NETWORK;
    r->u.network.channel = ezb_nwk_get_current_channel();
    r->u.network.pan_id = ezb_nwk_get_panid();
    ezb_extpanid_t ext;
    ezb_nwk_get_extended_panid(&ext);
    r->u.network.ext_pan_id = ext.u64;
    ezb_extaddr_t me;
    ezb_nwk_get_extended_address(&me);
    r->u.network.panel_ieee = me.u64;
    r->u.network.update_id = ezb_nwk_get_update_id();
}

static void formed(void)
{
    s_retries = 0;
    s_guard.starting = 0u; /* the radio got through the stack's start */
    s_formed_at = (uint64_t)(esp_timer_get_time() / 1000);
    atomic_store(&s_channel, (unsigned)ezb_nwk_get_current_channel());
    atomic_store(&s_pan_id, (unsigned)ezb_nwk_get_panid());
    atomic_store(&s_formed, true);
    xEventGroupSetBits(s_ev, EV_FORMED);
    ESP_LOGI(TAG, "Zigbee network up: channel %u, PAN 0x%04x", (unsigned)atomic_load(&s_channel),
             (unsigned)atomic_load(&s_pan_id));
    if (s_resetting) {
        /* reset_network: REP_DONE, then REP_NETWORK (zsrc_zigbee.h, reset_network) */
        s_resetting = false;
        rq_post_done(rq_of_kind(RQ_RESET, 0u), ZSRC_ZB_DONE_OK, 0u);
    }
    ev_t e;
    memset(&e, 0, sizeof(e));
    e.kind = EV_REPORT;
    network_fill(&e.u.r);
    post(&e);
}

static void commission(void *arg)
{
    (void)ezb_bdb_start_top_level_commissioning((ezb_bdb_comm_mode_mask_t)(uintptr_t)arg);
}

static atomic_uint s_retry_mode;

/* The commissioning step, from the timer's task under the stack's lock: never through
 * esp_zigbee_task_queue_post(), which traps on P4 v1.x silicon (docs/bugs/B024). The step queues
 * the stack's own work, and the stack wakes its main loop for it. A lock held past the wait tries
 * again after the shortest delay. */
static void retry_fired(void *arg)
{
    (void)arg;
    if (!atomic_load(&s_running)) {
        return;
    }
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(ZB_LOCK_WAIT_MS))) {
        (void)esp_timer_start_once(s_retry_timer, (uint64_t)ZB_RETRY_MS * 1000u);
        return;
    }
    commission((void *)(uintptr_t)atomic_load(&s_retry_mode));
    esp_zigbee_lock_release();
}

static void retry_later(uint8_t mode)
{
    if (s_retry_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = retry_fired, .name = "zb_retry"};
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) {
            return;
        }
    }
    const unsigned shift = s_retries < ZB_RETRY_MAX_SHIFT ? s_retries : ZB_RETRY_MAX_SHIFT;
    s_retries++;
    atomic_store(&s_retry_mode, mode);
    (void)esp_timer_stop(s_retry_timer);
    (void)esp_timer_start_once(s_retry_timer, ((uint64_t)ZB_RETRY_MS << shift) * 1000u);
}

static uint8_t bdb_status(const ezb_app_signal_t *signal)
{
    const void *p = ezb_app_signal_get_params(signal);
    return p != NULL ? *(const uint8_t *)p : 0xFFu;
}

/* Devices the panel asked to leave that answered: their leave indication carries the tag. */
typedef struct {
    uint64_t ieee;
    uint32_t tag;
    uint64_t until;
} leaving_t;

static leaving_t s_leaving[ZB_LEAVING_MAX];
static unsigned s_leaving_next;

static void leaving_note(uint64_t ieee, uint32_t tag)
{
    leaving_t *l = &s_leaving[s_leaving_next++ % ZB_LEAVING_MAX];
    l->ieee = ieee;
    l->tag = tag;
    l->until = ms_now() + ZB_RQ_MS;
}

static uint32_t leaving_take(uint64_t ieee)
{
    for (size_t i = 0; i < ZB_LEAVING_MAX; i++) {
        if (s_leaving[i].ieee == ieee && s_leaving[i].until > ms_now()) {
            s_leaving[i].ieee = 0u;
            return s_leaving[i].tag;
        }
    }
    return 0u;
}

/* One device going, said once even when the leave indication and the trust center's update
 * both tell it. */
static uint64_t s_last_left;
static uint64_t s_last_left_at;

static void report_left(uint64_t ieee, uint16_t nwk, bool rejoin)
{
    const uint64_t now = ms_now();
    if (ieee == 0u || (ieee == s_last_left && now - s_last_left_at < 2000u)) {
        return;
    }
    s_last_left = ieee;
    s_last_left_at = now;
    /* The panel asked it (leave): the request ends OK first, then the leave (zsrc_zigbee.h). */
    rq_t *q = rq_of_kind(RQ_LEAVE, ieee);
    uint32_t tag = 0u;
    if (q != NULL) {
        tag = q->tag;
        rq_post_done(q, ZSRC_ZB_DONE_OK, 0u);
    } else {
        tag = leaving_take(ieee);
    }
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_LEFT, ieee, nwk, 0u, tag);
    e.u.r.u.left.rejoin = rejoin;
    e.u.r.u.left.asked = tag != 0u;
    post(&e);
}

/* key_pending: told from its announce, before the stack's authorization signal - which
 * esp-zigbee-lib 2.0.4 gives only once the device's own key exchange ends (B021). */
static void report_joined(joiner_t *j, uint16_t nwk, bool key_pending)
{
    if (j->reported) {
        return;
    }
    j->reported = true;
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_JOINED, j->ieee, nwk, 0u, 0u);
    e.u.r.u.joined.join = j->join;
    e.u.r.u.joined.rejoin = j->rejoin;
    e.u.r.u.joined.parent = j->parent;
    e.u.r.u.joined.key_pending = key_pending;
    post(&e);
}

static void report_refused(uint64_t ieee, uint16_t nwk, uint8_t why)
{
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_JOIN_REFUSED, ieee, nwk, 0u, 0u);
    e.u.r.u.refused.why = why;
    post(&e);
}

/* The local reset of reset_network is done (EZB_ZDO_SIGNAL_LEAVE, bdb.h:214-221): every device
 * the panel knew went with the old network; form the new one. */
static void reset_left(void)
{
    memset(s_known, 0, sizeof(s_known));
    memset(s_asking, 0, sizeof(s_asking));
    memset(s_joiners, 0, sizeof(s_joiners));
    memset(s_leaving, 0, sizeof(s_leaving));
    memset(s_ic, 0, sizeof(s_ic)); /* "all Zigbee persistent data" went: the codes too */
    ESP_LOGI(TAG, "the Zigbee network was left: forming a new one");
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
}

static bool on_signal(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const uint8_t status = bdb_status(signal);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "the Zigbee stack did not start (0x%02x): trying again", (unsigned)status);
            xEventGroupSetBits(s_ev, EV_FAILED);
            retry_later(EZB_BDB_MODE_INITIALIZATION);
        } else if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "no Zigbee network stored: forming one");
            (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            /* The stored network, with the paired devices' keys: nothing to form. Joining stays
             * closed until the installer opens a learn window (ZB-002). */
            formed();
        }
        break;
    }
    case EZB_BDB_SIGNAL_FORMATION: {
        const uint8_t status = bdb_status(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            formed(); /* no network steering: that would open joining (ZB-002) */
        } else {
            ESP_LOGW(TAG, "forming the Zigbee network failed (0x%02x): trying again",
                     (unsigned)status);
            retry_later(EZB_BDB_MODE_NETWORK_FORMATION);
        }
        break;
    }
    case EZB_ZDO_SIGNAL_LEAVE: /* the panel itself left: only ever reset_network's doing */
        if (s_resetting) {
            reset_left();
        }
        break;
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        /* The trust center's view of a join, rejoin or leave (app_signals.h:140-153, :452-458). */
        const ezb_zdo_signal_device_update_params_t *u = ezb_app_signal_get_params(signal);
        if (u == NULL) {
            break;
        }
        const uint64_t ieee = u->device_addr.u64;
        const bool rejoin = u->status == EZB_ZDO_UPDDEV_SECURE_REJOIN ||
                            u->status == EZB_ZDO_UPDDEV_TC_REJOIN;
        if (u->status == EZB_ZDO_UPDDEV_DEVICE_LEFT) {
            report_left(ieee, u->short_addr, false);
            break;
        }
        if (u->tc_action != EZB_ZDO_TC_ACTION_ACCEPT) {
            /* 1: codes are required and none matched; 2: a rejoin the trust center refused. */
            const uint8_t why = (s_codes_only && !ic_has(ieee)) ? 1u : rejoin ? 2u : 0u;
            report_refused(ieee, u->short_addr, why);
            break;
        }
        joiner_t *j = joiner_for(ieee, true);
        j->parent = u->parent_short;
        j->rejoin = rejoin;
        j->reported = false;
        /* A rejoin keeps the key it had: how it first joined is the registry's to remember. */
        j->join = (uint8_t)(rejoin ? ZSRC_ZB_JOIN_UNKNOWN : join_key_of(ieee));
        if (rejoin) {
            report_joined(j, u->short_addr, false); /* a secured rejoin is not authorized again */
        }
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        /* A new device has its network key (app_signals.h:155-167, :463-468). */
        const ezb_zdo_signal_device_authorized_params_t *a = ezb_app_signal_get_params(signal);
        if (a == NULL) {
            break;
        }
        const uint64_t ieee = a->device_addr.u64;
        if (a->status != EZB_ZDO_AUTH_STATUS_SUCCESS) {
            report_refused(ieee, a->short_addr, 0u);
            break;
        }
        joiner_t *j = joiner_for(ieee, false);
        if (j == NULL) {
            j = joiner_for(ieee, true); /* joined straight to the panel without an update */
            j->join = join_key_of(ieee);
        }
        if (j->reported) {
            ev_t e; /* told at its announce: its key exchange ended now (B021) */
            report_begin(&e, ZSRC_ZB_REP_AUTHORIZED, ieee, a->short_addr, 0u, 0u);
            post(&e);
            break;
        }
        report_joined(j, a->short_addr, false);
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *a = ezb_app_signal_get_params(signal);
        if (a == NULL) {
            break;
        }
        const uint64_t ieee = a->device_addr.u64;
        ESP_LOGI(TAG, "device %016llx joined or rejoined (0x%04x)", (unsigned long long)ieee,
                 (unsigned)a->short_addr);
        post_frame(ieee, a->short_addr, 0u, 0u, 0);
        joiner_t *j = joiner_for(ieee, false);
        if (j != NULL && !j->reported) {
            report_joined(j, a->short_addr, !j->rejoin); /* in, before its authorization */
        }
        ev_t e;
        report_begin(&e, ZSRC_ZB_REP_ANNOUNCE, ieee, a->short_addr, 0u, 0u);
        e.u.r.u.announce.mac_caps = a->capability;
        post(&e);
        note_announce(a->short_addr, ieee, a->capability);
        break;
    }
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *l = ezb_app_signal_get_params(signal);
        if (l == NULL) {
            break;
        }
        const uint64_t ieee = l->device_addr.u64;
        const bool rejoin = l->leave_type == EZB_ZDO_LEAVE_TYPE_REJOIN;
        report_left(ieee, l->short_addr, rejoin);
        if (l->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            forget_device(ieee);
            post_simple(EV_GONE, ieee, 0u, 0u, 0);
        }
        break;
    }
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const uint8_t *s = ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "joining %s", s != NULL && *s != 0u ? "open" : "closed");
        break;
    }
    default:
        ESP_LOGD(TAG, "signal %s (0x%04x)", ezb_app_signal_to_string(type), (unsigned)type);
        break;
    }
    return true;
}

static void string_attr(const void *value, char *out, size_t len)
{
    const uint8_t *v = (const uint8_t *)value;
    size_t n = v[0]; /* ZCL character strings start with their length */
    if (n == 0xFFu) {
        n = 0u; /* an invalid string */
    }
    if (n + 1u > len) {
        n = len - 1u;
    }
    memcpy(out, v + 1, n);
    out[n] = '\0';
}

/* One Basic attribute into a report (devices.md §2 BASIC); a wrong type is left out. */
static void basic_attr(zsrc_zb_basic_t *b, uint16_t id, uint8_t type, const void *v)
{
    const bool byte = type == EZB_ZCL_ATTR_TYPE_UINT8 || type == EZB_ZCL_ATTR_TYPE_ENUM8;
    const bool text = type == EZB_ZCL_ATTR_TYPE_STRING;
    const uint8_t first = *(const uint8_t *)v;
    switch (id) {
    case 0x0000u:
        if (byte) {
            b->zcl_version = first;
            b->have |= ZSRC_ZB_BASIC_ZCL_VERSION;
        }
        break;
    case 0x0001u:
        if (byte) {
            b->app_version = first;
            b->have |= ZSRC_ZB_BASIC_APP_VERSION;
        }
        break;
    case 0x0003u:
        if (byte) {
            b->hw_version = first;
            b->have |= ZSRC_ZB_BASIC_HW_VERSION;
        }
        break;
    case 0x0004u:
        if (text) {
            string_attr(v, b->manufacturer, sizeof(b->manufacturer));
            b->have |= ZSRC_ZB_BASIC_MANUFACTURER;
        }
        break;
    case 0x0005u:
        if (text) {
            string_attr(v, b->model, sizeof(b->model));
            b->have |= ZSRC_ZB_BASIC_MODEL;
        }
        break;
    case 0x0006u:
        if (text) {
            string_attr(v, b->date_code, sizeof(b->date_code));
            b->have |= ZSRC_ZB_BASIC_DATE_CODE;
        }
        break;
    case 0x0007u:
        if (byte) {
            b->power_source = first;
            b->have |= ZSRC_ZB_BASIC_POWER;
        }
        break;
    case 0x4000u:
        if (text) {
            string_attr(v, b->sw_build, sizeof(b->sw_build));
            b->have |= ZSRC_ZB_BASIC_SW_BUILD;
        }
        break;
    default:
        break;
    }
}

/* A Basic answer or report: one REP_BASIC, and the manufacturer and model on every endpoint of
 * the device the source knows (the picker and the quirks table, ZB-007). */
static void basic_done(uint64_t ieee, uint16_t nwk, uint8_t ep, uint32_t tag, ev_t *e)
{
    const zsrc_zb_basic_t *b = &e->u.r.u.basic;
    if (b->have == 0u && tag == 0u) {
        return;
    }
    post(e);
    if ((b->have & (ZSRC_ZB_BASIC_MANUFACTURER | ZSRC_ZB_BASIC_MODEL)) == 0u) {
        return;
    }
    char manufacturer[32];
    char model[32];
    memcpy(manufacturer, b->manufacturer, sizeof(manufacturer));
    memcpy(model, b->model, sizeof(model));
    const uint16_t have = b->have;
    (void)nwk;
    (void)ep;
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        known_t *k = &s_known[i];
        if (!k->used || k->d.ieee != ieee) {
            continue;
        }
        if ((have & ZSRC_ZB_BASIC_MANUFACTURER) != 0u) {
            memcpy(k->d.manufacturer, manufacturer, sizeof(k->d.manufacturer));
        }
        if ((have & ZSRC_ZB_BASIC_MODEL) != 0u) {
            memcpy(k->d.model, model, sizeof(k->d.model));
        }
        memset(e, 0, sizeof(*e)); /* the report is out: e is reused for each endpoint */
        e->kind = EV_DEVICE;
        e->ieee = ieee;
        e->endpoint = k->d.endpoint;
        e->u.d = k->d;
        post(e);
    }
}

/* The width of a fixed-size ZCL value of this type; 0 for any other (strings, arrays, ...). The
 * stack sizes each attribute's value buffer by its declared type (zcl_general_cmd.h:192-198,
 * 350-356). */
static size_t zcl_width(uint8_t type)
{
    switch (type) {
    case EZB_ZCL_ATTR_TYPE_DATA8:
    case EZB_ZCL_ATTR_TYPE_BOOL:
    case EZB_ZCL_ATTR_TYPE_MAP8:
    case EZB_ZCL_ATTR_TYPE_UINT8:
    case EZB_ZCL_ATTR_TYPE_INT8:
    case EZB_ZCL_ATTR_TYPE_ENUM8: return 1u;
    case EZB_ZCL_ATTR_TYPE_DATA16:
    case EZB_ZCL_ATTR_TYPE_MAP16:
    case EZB_ZCL_ATTR_TYPE_UINT16:
    case EZB_ZCL_ATTR_TYPE_INT16:
    case EZB_ZCL_ATTR_TYPE_ENUM16:
    case EZB_ZCL_ATTR_TYPE_SEMI: return 2u;
    case EZB_ZCL_ATTR_TYPE_DATA24:
    case EZB_ZCL_ATTR_TYPE_MAP24:
    case EZB_ZCL_ATTR_TYPE_UINT24:
    case EZB_ZCL_ATTR_TYPE_INT24: return 3u;
    case EZB_ZCL_ATTR_TYPE_DATA32:
    case EZB_ZCL_ATTR_TYPE_MAP32:
    case EZB_ZCL_ATTR_TYPE_UINT32:
    case EZB_ZCL_ATTR_TYPE_INT32:
    case EZB_ZCL_ATTR_TYPE_SINGLE: return 4u;
    case EZB_ZCL_ATTR_TYPE_DATA64:
    case EZB_ZCL_ATTR_TYPE_MAP64:
    case EZB_ZCL_ATTR_TYPE_UINT64:
    case EZB_ZCL_ATTR_TYPE_INT64:
    case EZB_ZCL_ATTR_TYPE_DOUBLE: return 8u;
    default: return 0u;
    }
}

/* An unsigned value `width` bytes wide: the attribute's own type, or a same-width data, bitmap,
 * unsigned or enumeration type (the bits mean the same). A string, a signed, a wider or a
 * narrower value is not (ZB13). */
static bool zcl_bits(uint8_t type, size_t width)
{
    switch (type) {
    case EZB_ZCL_ATTR_TYPE_BOOL:
    case EZB_ZCL_ATTR_TYPE_DATA8:
    case EZB_ZCL_ATTR_TYPE_MAP8:
    case EZB_ZCL_ATTR_TYPE_UINT8:
    case EZB_ZCL_ATTR_TYPE_ENUM8: return width == 1u;
    case EZB_ZCL_ATTR_TYPE_DATA16:
    case EZB_ZCL_ATTR_TYPE_MAP16:
    case EZB_ZCL_ATTR_TYPE_UINT16:
    case EZB_ZCL_ATTR_TYPE_ENUM16: return width == 2u;
    default: return false;
    }
}

/* One attribute from a report or a read response (Basic goes through basic_attr). Its id and
 * declared type are checked before its value is read, and no more bytes are read than that type
 * holds: a value of another type is no sensor state and is dropped (ZB13). */
static void attribute(uint64_t ieee, uint8_t ep, uint16_t cluster, uint16_t id, uint8_t type,
                      const void *v)
{
    if (ieee == 0u || v == NULL) {
        return;
    }
    const uint8_t *b = (const uint8_t *)v;
    bool dropped = false;
    switch (cluster) {
    case ZSRC_ZB_ZCL_IAS_ZONE: {
        /* TODO(BL-143): Zone State (0x0000) and IAS_CIE_Address (0x0010), which the setup's
         * IAS_ZONE step reads, have no report in zsrc_zigbee.h yet: they are dropped here. */
        if (id != 0x0001u && id != 0x0002u) {
            break;
        }
        if (!zcl_bits(type, 2u)) { /* zone type: enum16; zone status: map16 */
            dropped = true;
            break;
        }
        uint16_t word;
        memcpy(&word, b, sizeof(word));
        if (id == 0x0001u) { /* zone type, read back after the join */
            known_t *k = known_for(ieee, ep, false);
            if (k != NULL) {
                k->d.zone_type = word;
                ev_t e;
                memset(&e, 0, sizeof(e));
                e.kind = EV_DEVICE;
                e.ieee = ieee;
                e.endpoint = ep;
                e.u.d = k->d;
                post(&e);
            }
        } else { /* zone status */
            post_simple(EV_IAS, ieee, ep, cluster, word);
        }
        break;
    }
    case ZSRC_ZB_ZCL_ON_OFF:
        if (id == 0x0000u) {
            dropped = !zcl_bits(type, 1u); /* OnOff: bool */
            if (!dropped) {
                post_simple(EV_BOOL, ieee, ep, cluster, b[0] != 0u);
            }
        }
        break;
    case ZSRC_ZB_ZCL_OCCUPANCY:
        if (id == 0x0000u) {
            dropped = !zcl_bits(type, 1u); /* Occupancy: map8 */
            if (!dropped) {
                post_simple(EV_BOOL, ieee, ep, cluster, (b[0] & 0x01u) != 0u);
            }
        }
        break;
    case ZSRC_ZB_ZCL_TEMP_MEAS:
        if (id == 0x0000u) {
            dropped = type != EZB_ZCL_ATTR_TYPE_INT16; /* MeasuredValue: int16, signed */
            if (!dropped) {
                int16_t t;
                memcpy(&t, b, sizeof(t));
                post_simple(EV_VALUE, ieee, ep, cluster, t); /* 0.01 C already */
            }
        }
        break;
    case ZSRC_ZB_ZCL_POWER_CFG:
        /* Battery percentage remaining (uint8), in half percents; 0xFF is "unknown", not
         * 100 %. */
        if (id == 0x0021u) {
            dropped = !zcl_bits(type, 1u) || type == EZB_ZCL_ATTR_TYPE_BOOL;
            if (!dropped && b[0] != 0xFFu) {
                post_simple(EV_BATTERY, ieee, ep, cluster, b[0] / 2);
            }
        }
        break;
    default:
        break;
    }
    if (dropped) {
        ESP_LOGD(TAG, "%016llx ep %u cluster 0x%04x attribute 0x%04x: type 0x%02x dropped",
                 (unsigned long long)ieee, (unsigned)ep, (unsigned)cluster, (unsigned)id,
                 (unsigned)type);
    }
}

/* ZB03: an IAS WD server's Read Attributes Response (the setup's MaxDuration read), for the
 * panel as the core expects it (zbz_wd.h zbw_max_duration_of): a REP_ZCL of cluster 0x0502,
 * command 0x01, server to client, its records byte for byte - attribute id (low byte first),
 * status, then for a success its type and value. The stack hands each value in the panel's own
 * byte order, which on the ESP32 is ZCL's (little-endian). A record whose type has no fixed
 * width, or one that would not fit, ends the records there. Posted before the request's end. */
static void wd_read_answer(uint64_t ieee, uint16_t nwk, uint8_t ep, const ezb_zcl_cmd_hdr_t *h,
                           const ezb_zcl_read_attr_rsp_variable_t *v)
{
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_ZCL, ieee, nwk, ep, 0u);
    zsrc_zb_zcl_t *z = &e.u.r.u.zcl;
    z->cluster = ZSRC_ZB_ZCL_IAS_WD;
    z->command = ZCL_READ_ATTR_RSP;
    z->server_to_client = true;
    z->manufacturer = EZB_ZCL_CMD_FC_IS_MANUF_SPEC(h->fc) ? h->manuf_code : 0u;
    z->tsn = h->tsn;
    size_t n = 0u;
    for (; v != NULL; v = v->next) {
        const bool ok = v->status == EZB_ZCL_STATUS_SUCCESS;
        const size_t w = ok ? zcl_width(v->attr_type) : 0u;
        if (ok && (w == 0u || v->attr_value == NULL)) {
            break;
        }
        const size_t need = 3u + (ok ? 1u + w : 0u);
        if (n + need > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
            break;
        }
        z->payload[n++] = (uint8_t)(v->attr_id & 0xFFu);
        z->payload[n++] = (uint8_t)(v->attr_id >> 8);
        z->payload[n++] = v->status;
        if (ok) {
            z->payload[n++] = v->attr_type;
            memcpy(&z->payload[n], v->attr_value, w);
            n += w;
        }
    }
    z->len = (uint8_t)n;
    post(&e);
}

/* The panel asked a device to stay awake (wake window), or has a request waiting for it: answer
 * its check-in with fast polling for this many quarter-seconds. */
typedef struct {
    uint64_t ieee;
    uint64_t until;
} awake_t;

static awake_t s_awake[ZB_AWAKE_MAX];

static bool awake_wanted(uint64_t ieee, uint16_t *quarter_s)
{
    const uint64_t now = ms_now();
    for (size_t i = 0; i < ZB_AWAKE_MAX; i++) {
        if (s_awake[i].ieee == ieee && s_awake[i].until > now) {
            const uint64_t qs = (s_awake[i].until - now) / 250u;
            *quarter_s = (uint16_t)(qs > 0xFFFEu ? 0xFFFEu : (qs == 0u ? 1u : qs));
            return true;
        }
    }
    if (rq_waiting_for(ieee)) {
        *quarter_s = ZB_FAST_POLL_QS;
        return true;
    }
    return false;
}

/* A cluster command the source doesn't decode, for the siren and keypad policies (REP_ZCL). */
static void report_zcl(uint64_t ieee, uint16_t nwk, const ezb_zcl_cmd_hdr_t *h, const uint8_t *p,
                       uint16_t len)
{
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_ZCL, ieee, nwk, h->src_ep, 0u);
    zsrc_zb_zcl_t *z = &e.u.r.u.zcl;
    z->cluster = h->cluster_id;
    z->command = h->cmd_id;
    z->server_to_client = EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc);
    z->want_response = !EZB_ZCL_CMD_FC_IS_DIS_DEFAULT_RSP(h->fc);
    z->manufacturer = EZB_ZCL_CMD_FC_IS_MANUF_SPEC(h->fc) ? h->manuf_code : 0u;
    z->tsn = h->tsn;
    z->len = (uint8_t)(len > ZSRC_ZB_ZCL_PAYLOAD_MAX ? ZSRC_ZB_ZCL_PAYLOAD_MAX : len);
    const bool has_code = h->cluster_id == ZSRC_ZB_ZCL_IAS_ACE && !z->server_to_client &&
                          (h->cmd_id == ZBA_CMD_ARM || h->cmd_id == ZBA_CMD_BYPASS);
    if (p != NULL && z->len != 0u) {
        if (has_code) {
            e.value = code_park(p, z->len);
            if (e.value == 0) {
                /* Every slot taken: lost like an alarm-path event on a full queue. */
                atomic_fetch_add(&s_lost, 1u);
                atomic_fetch_add(&s_lost_all, 1u);
                if (s_wake != NULL) {
                    s_wake();
                }
                wipe((volatile uint8_t *)&e, sizeof(e));
                return;
            }
        } else {
            memcpy(z->payload, p, z->len);
        }
    }
    if (!post(&e)) {
        code_take(e.value, NULL, 0u);
    }
    /* No copy of a code stays on the stack's task (devices.md §16). */
    wipe((volatile uint8_t *)&e, sizeof(e));
}

/* Every ZCL frame, raw, before the stack's clusters see it (zcl_core.h:486-505). A keypad's or a
 * fob's IAS ACE command goes to the source untouched, and the stack must not answer it: the
 * panel's ACE policy answers with the request's own sequence number (alarm-devices.md §3; this
 * answers its open question 2). With no report sink yet, the stack handles it as before. */
static bool on_raw_zcl(const ezb_zcl_raw_frame_t *f)
{
    if (f == NULL || f->header == NULL || atomic_load(&s_sink) == NULL) {
        return false;
    }
    const ezb_zcl_cmd_hdr_t *h = f->header;
    if (h->cluster_id != ZSRC_ZB_ZCL_IAS_ACE ||
        EZB_ZCL_CMD_FC_GET_FRAME_TYPE(h->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC ||
        EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc)) {
        return false;
    }
    const uint64_t ieee = ieee_of(&h->src_addr);
    if (ieee == 0u) {
        return false;
    }
    const uint16_t nwk = nwk_in(&h->src_addr);
    post_frame(ieee, nwk, h->src_ep, h->cluster_id, h->rssi);
    report_zcl(ieee, nwk, h, f->payload, f->payload_length);
    return true;
}

static void on_action(ezb_zcl_core_action_callback_id_t id, void *message)
{
    if (message == NULL) {
        return;
    }
    switch (id) {
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID: {
        const ezb_zcl_cmd_report_attr_message_t *m = message;
        if (m->in.header == NULL) {
            break;
        }
        const uint64_t ieee = ieee_of(&m->in.header->src_addr);
        const uint16_t nwk = nwk_in(&m->in.header->src_addr);
        const uint8_t ep = m->in.header->src_ep;
        post_frame(ieee, nwk, ep, m->info.cluster_id, m->in.header->rssi);
        if (m->info.cluster_id == ZSRC_ZB_ZCL_BASIC) {
            ev_t e;
            report_begin(&e, ZSRC_ZB_REP_BASIC, ieee, nwk, ep, 0u);
            for (const ezb_zcl_report_attr_variable_t *v = m->in.variables; v != NULL;
                 v = v->next) {
                if (v->attr_value != NULL) {
                    basic_attr(&e.u.r.u.basic, v->attr_id, v->attr_type, v->attr_value);
                }
            }
            if (ieee != 0u) {
                basic_done(ieee, nwk, ep, 0u, &e);
            }
            break;
        }
        for (const ezb_zcl_report_attr_variable_t *v = m->in.variables; v != NULL; v = v->next) {
            attribute(ieee, ep, m->info.cluster_id, v->attr_id, v->attr_type, v->attr_value);
        }
        break;
    }
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID: {
        const ezb_zcl_cmd_read_attr_rsp_message_t *m = message;
        if (m->in.header == NULL) {
            break;
        }
        const uint64_t ieee = ieee_of(&m->in.header->src_addr);
        const uint16_t nwk = nwk_in(&m->in.header->src_addr);
        const uint8_t ep = m->in.header->src_ep;
        post_frame(ieee, nwk, ep, m->info.cluster_id, m->in.header->rssi);
        rq_t *q = ieee != 0u ? rq_match_zcl(ieee, ep, m->info.cluster_id, m->in.header->tsn,
                                            ZCL_READ_ATTR_RSP, 0u)
                             : NULL;
        /* How the request ended: answered when any attribute was there. */
        uint8_t result = ZSRC_ZB_DONE_OK;
        uint8_t first_fail = EZB_ZCL_STATUS_SUCCESS;
        bool any_ok = m->in.variables == NULL;
        for (const ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v != NULL; v = v->next) {
            if (v->status == EZB_ZCL_STATUS_SUCCESS) {
                any_ok = true;
            } else if (first_fail == EZB_ZCL_STATUS_SUCCESS) {
                first_fail = v->status;
            }
        }
        if (!any_ok) {
            result = zcl_result(first_fail);
        }
        if (m->info.cluster_id == ZSRC_ZB_ZCL_BASIC) {
            ev_t e;
            report_begin(&e, ZSRC_ZB_REP_BASIC, ieee, nwk, ep, q != NULL ? q->tag : 0u);
            for (const ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v != NULL;
                 v = v->next) {
                if (v->status == EZB_ZCL_STATUS_SUCCESS && v->attr_value != NULL) {
                    basic_attr(&e.u.r.u.basic, v->attr_id, v->attr_type, v->attr_value);
                }
            }
            if (ieee != 0u) {
                basic_done(ieee, nwk, ep, q != NULL ? q->tag : 0u, &e);
            }
        } else {
            for (const ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v != NULL;
                 v = v->next) {
                if (v->status == EZB_ZCL_STATUS_SUCCESS) {
                    attribute(ieee, ep, m->info.cluster_id, v->attr_id, v->attr_type,
                              v->attr_value);
                }
            }
            if (m->info.cluster_id == ZSRC_ZB_ZCL_IAS_WD && ieee != 0u) {
                wd_read_answer(ieee, nwk, ep, m->in.header, m->in.variables); /* ZB03 */
            }
        }
        rq_post_done(q, result, first_fail); /* after what it answered (zsrc_zigbee.h) */
        break;
    }
    case EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID: {
        const ezb_zcl_cmd_config_report_rsp_message_t *m = message;
        if (m->in.header == NULL) {
            break;
        }
        const uint64_t ieee = ieee_of(&m->in.header->src_addr);
        post_frame(ieee, nwk_in(&m->in.header->src_addr), m->in.header->src_ep,
                   m->info.cluster_id, m->in.header->rssi);
        rq_t *q = ieee != 0u ? rq_match_zcl(ieee, m->in.header->src_ep, m->info.cluster_id,
                                            m->in.header->tsn, ZCL_CONFIG_REP_RSP, 0u)
                             : NULL;
        uint8_t status = EZB_ZCL_STATUS_SUCCESS;
        for (const ezb_zcl_config_report_rsp_variable_t *v = m->in.variables; v != NULL;
             v = v->next) {
            if (v->status != EZB_ZCL_STATUS_SUCCESS) {
                status = v->status;
                break;
            }
        }
        rq_post_done(q, zcl_result(status), status);
        break;
    }
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        const ezb_zcl_cmd_default_rsp_message_t *m = message;
        const ezb_zcl_cmd_hdr_t *h = m->in.header;
        if (h == NULL) {
            break;
        }
        const uint64_t ieee = ieee_of(&h->src_addr);
        const uint16_t nwk = nwk_in(&h->src_addr);
        post_frame(ieee, nwk, h->src_ep, m->info.cluster_id, h->rssi);
        if (ieee == 0u) {
            break;
        }
        if (m->info.cluster_id == ZSRC_ZB_ZCL_IAS_WD || m->info.cluster_id == ZSRC_ZB_ZCL_IAS_ACE) {
            /* The siren's or keypad's confirmation, for its policy (alarm-devices.md §2). */
            const uint8_t body[2] = {m->in.rsp_to_cmd, m->in.status_code};
            report_zcl(ieee, nwk, h, body, sizeof(body));
        }
        rq_t *q = rq_match_zcl(ieee, h->src_ep, m->info.cluster_id, h->tsn, ZCL_DEFAULT_RSP,
                               m->in.rsp_to_cmd);
        rq_post_done(q, zcl_result(m->in.status_code), m->in.status_code);
        break;
    }
    case EZB_ZCL_CORE_IAS_ZONE_ENROLL_CB_ID: {
        const ezb_zcl_ias_zone_enroll_req_message_t *m = message;
        if (m->in.header != NULL) {
            const uint64_t ieee = ieee_of(&m->in.header->src_addr);
            post_frame(ieee, nwk_in(&m->in.header->src_addr), m->in.header->src_ep,
                       ZSRC_ZB_ZCL_IAS_ZONE, m->in.header->rssi);
            post_simple(EV_ENROLL, ieee, m->in.header->src_ep, ZSRC_ZB_ZCL_IAS_ZONE,
                        m->in.payload.zone_type);
        }
        break;
    }
    case EZB_ZCL_CORE_IAS_ZONE_STATUS_CHANGE_NOTIF_CB_ID: {
        const ezb_zcl_ias_zone_status_change_notif_message_t *m = message;
        if (m->in.header != NULL) {
            const uint64_t ieee = ieee_of(&m->in.header->src_addr);
            post_frame(ieee, nwk_in(&m->in.header->src_addr), m->in.header->src_ep,
                       ZSRC_ZB_ZCL_IAS_ZONE, m->in.header->rssi);
            post_simple(EV_IAS, ieee, m->in.header->src_ep, ZSRC_ZB_ZCL_IAS_ZONE,
                        m->in.payload.zone_status);
        }
        break;
    }
    case EZB_ZCL_CORE_POLL_CONTROL_CHECK_IN_CB_ID: {
        /* The stack answers the check-in with the Check-in Response these fields describe
         * (poll_control.h:59-74): fast polling only while the panel wants the device awake. */
        ezb_zcl_poll_control_check_in_message_t *m = message;
        m->out.start_fast_poll = false;
        if (m->in.header != NULL) {
            const uint64_t ieee = ieee_of(&m->in.header->src_addr);
            const uint16_t nwk = nwk_in(&m->in.header->src_addr);
            uint16_t qs = 0u;
            if (ieee != 0u && awake_wanted(ieee, &qs)) {
                m->out.start_fast_poll = true;
                m->out.fast_poll_timeout = qs;
            }
            post_frame(ieee, nwk, m->in.header->src_ep, ZSRC_ZB_ZCL_POLL_CTRL,
                       m->in.header->rssi);
            if (ieee != 0u) {
                ev_t e;
                report_begin(&e, ZSRC_ZB_REP_CHECK_IN, ieee, nwk, m->in.header->src_ep, 0u);
                post(&e);
            }
        }
        break;
    }
    default:
        break;
    }
}

/* The energy scans: the source's 5-second look at the network's channel (user_ctx 0), or the
 * channel page's survey of the four (user_ctx: its request). Called per channel, then with NULL
 * when the scan is over (nwk.h:573-580). */
static const uint8_t s_survey_channels[ZSRC_ZB_CHANNELS] = {11u, 15u, 20u, 25u};
static uint8_t s_survey_known;
static int8_t s_survey_dbm[ZSRC_ZB_CHANNELS];

static void on_energy(ezb_nwk_ed_scan_result_t *result, void *user_ctx)
{
    const uint32_t serial = (uint32_t)(uintptr_t)user_ctx;
    if (result != NULL) {
        if (result->channel_number == atomic_load(&s_channel)) {
            post_simple(EV_ENERGY, 0u, 0u, 0u, result->max_rssi);
        }
        for (size_t i = 0; serial != 0u && i < ZSRC_ZB_CHANNELS; i++) {
            if (s_survey_channels[i] == result->channel_number) {
                s_survey_dbm[i] = result->max_rssi;
                s_survey_known = (uint8_t)(s_survey_known | (1u << i));
            }
        }
        return;
    }
    rq_t *q = rq_by_serial(serial);
    if (q == NULL) {
        return;
    }
    ev_t e;
    report_begin(&e, ZSRC_ZB_REP_SURVEY, 0u, 0u, 0u, q->tag);
    e.u.r.u.survey.known = s_survey_known;
    memcpy(e.u.r.u.survey.dbm, s_survey_dbm, sizeof(s_survey_dbm));
    post(&e);
    rq_post_done(q, ZSRC_ZB_DONE_OK, 0u);
}

/* ---- the stack's task -------------------------------------------------------------------- */

/* The panel's device descriptor: the stack keeps it from registration (and adds its Green Power
 * endpoint to it) but esp_zigbee_deinit doesn't free it, so stack_task does, after the deinit -
 * every restart of the stack leaked about 1 KB of internal RAM before (B024). */
static ezb_af_device_desc_t s_dev = EZB_INVALID_AF_DEVICE_DESC;

static void make_endpoint(void)
{
    /* The panel's endpoint: the IAS CIE (the client of every sensor's IAS Zone cluster) and a
     * client of the clusters its sensors report. A sensor's answers (read responses, reports)
     * travel server to client and are delivered only to a client of their cluster: the gateway
     * device brings Basic as a server, so its client is added for the manufacturer and model.
     * Since BL-143 also a client of Identify (identify) and of IAS WD (a siren's answers,
     * devices.md §3), and since ZB-M6 the IAS ACE server keypads and fobs look for in the
     * panel's Simple Descriptor (match descriptor, binding). Their commands still arrive raw
     * (on_raw_zcl takes them before the cluster does) while the report sink is set - always, once
     * zigbee_service owns zbz_panel - so the stack's own ACE server never answers one. */
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    s_dev = dev;
    ezb_zha_custom_gateway_config_t cfg = EZB_ZHA_CUSTOM_GATEWAY_CONFIG();
    ezb_af_ep_desc_t ep = ezb_zha_create_custom_gateway(ZB_EP, &cfg);
    (void)ezb_af_ep_desc_set_app_device_id(ep, EZB_ZHA_IAS_CONTROL_INDICATING_EQUIPMENT_ID);
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_ias_zone_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_occupancy_sensing_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_poll_control_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_identify_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_ias_wd_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    (void)ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_ias_ace_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER));
    (void)ezb_af_device_add_endpoint_desc(dev, ep);
    (void)ezb_af_device_desc_register(dev);
    ezb_zcl_core_action_handler_register(on_action);
    ezb_zcl_raw_command_handler_register(on_raw_zcl);
    ezb_apsde_data_indication_handler_register(on_aps_indication);
}

/* The stack is going: every request still waiting ends NO_RADIO (the stack's task, its main loop
 * over, nothing else in it). */
static void rq_fail_all(void)
{
    for (size_t i = 0; i < ZB_RQ_MAX; i++) {
        if (s_rq[i].serial != 0u) {
            rq_post_done(&s_rq[i], ZSRC_ZB_DONE_NO_RADIO, 0u);
        }
    }
    s_resetting = false;
}

static bool s_deinit; /* stack_task is in esp_zigbee_deinit (the stack's own task only) */

static void stack_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_config.device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR;
    cfg.device_config.install_code_policy = false; /* install codes: docs/features/20 ZB-002 */
    cfg.device_config.zczr_config.max_children = ZB_MAX_CHILDREN;
    cfg.platform_config.storage_partition_name = ZB_STORAGE;
    cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP;
    esp_zigbee_uart_config_t *u = &cfg.platform_config.radio_config.radio_uart_config;
    u->port = RADIO_UART_NUM;
    u->rx_pin = (gpio_num_t)s_rx_gpio;
    u->tx_pin = (gpio_num_t)s_tx_gpio;
    u->uart_config.baud_rate = RADIO_BAUD;
    u->uart_config.data_bits = UART_DATA_8_BITS;
    u->uart_config.parity = UART_PARITY_DISABLE;
    u->uart_config.stop_bits = UART_STOP_BITS_1;
    u->uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    u->uart_config.source_clk = UART_SCLK_DEFAULT;

    /* Unset, these two are an assert (a restart that says nothing); set, they say why first. */
    esp_radio_spinel_set_compatibility_error_callback(guard_incompatible);
    esp_radio_spinel_set_coprocessor_reset_failure_callback(guard_reset_failed);
    static bool at_exit;
    if (!at_exit) {
        at_exit = atexit(guard_died) == 0;
    }

    esp_err_t err = esp_zigbee_init(&cfg);
    if (err == ESP_OK) {
        ezb_aps_secur_enable_distributed_security(false); /* the panel is the trust center */
        (void)ezb_bdb_set_primary_channel_set(CONFIG_ALARM_ZB_CHANNEL_MASK);
        (void)ezb_app_signal_add_handler(on_signal);
        make_endpoint();
        err = esp_zigbee_start(false);
        if (err == ESP_OK) {
            (void)esp_zigbee_launch_mainloop(); /* returns only when the stack stops */
            ESP_LOGW(TAG, "the Zigbee stack stopped");
        }
        if (s_retry_timer != NULL) {
            (void)esp_timer_stop(s_retry_timer); /* no commissioning step into a stopped stack */
        }
        /* Every NO_RADIO end is queued before the link goes down: the service's task pumps once
         * more when it sees the stack gone, and not again until the stack is back. So the lock
         * first, then s_formed false and the ends in one hold (a hook holding the lock sends its
         * request before that, and it ends here too). Only if a hook kept the lock past the wait
         * does s_formed go false first: hooks then refuse at once (zb_lock) and the second wait
         * gets the lock. */
        bool held = esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000));
        atomic_store(&s_formed, false); /* no new requests from the zone side */
        if (!held) {
            held = esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000));
        }
        if (held) {
            rq_fail_all();
            esp_zigbee_lock_release();
        }
        s_deinit = true; /* a refused radio sleep is taken back to receiving first (B024) */
        esp_zigbee_deinit();
        s_deinit = false;
        if (s_dev != EZB_INVALID_AF_DEVICE_DESC) {
            ezb_af_free_device_desc(s_dev); /* its endpoints, clusters and attributes with it */
            s_dev = EZB_INVALID_AF_DEVICE_DESC;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the Zigbee stack would not start: %s", esp_err_to_name(err));
    }
    s_guard.starting = 0u; /* it ended by itself: a later crash is not the radio's */
    atomic_store(&s_formed, false);
    atomic_store(&s_running, false);
    xEventGroupSetBits(s_ev, EV_FAILED);
    vTaskDelete(NULL);
}

/* Starts the stack and waits for its network. Called again while it runs (after a wait that
 * timed out), it only waits again. */
bool radio_port_stack_start(void)
{
    if (s_events == NULL) {
        /* Both or neither (M03): each made into a local, published only when the other is there
         * too, the one made freed when the other fails - so a retry after a failure neither
         * leaks a group nor reaches the event-group calls below with no group. s_ev goes first:
         * s_events set is what says both are there. */
        QueueHandle_t events = xQueueCreateWithCaps(ZB_QUEUE_LEN + ZB_ROOM_ALARM, sizeof(ev_t),
                                                    MALLOC_CAP_SPIRAM);
        EventGroupHandle_t ev = events != NULL ? xEventGroupCreate() : NULL;
        if (events == NULL || ev == NULL) {
            if (events != NULL) {
                vQueueDeleteWithCaps(events);
            }
            ESP_LOGE(TAG, "no memory for the Zigbee stack's queue");
            return false;
        }
        s_ev = ev;
        s_events = events;
    }
    if (atomic_load(&s_formed)) {
        return true;
    }
    guard_boot();
    if (atomic_load(&s_paused)) {
        return false; /* the radio ended the last starts: not again until `zigbee retry` */
    }
    xEventGroupClearBits(s_ev, EV_FAILED);
    if (!atomic_load(&s_running)) {
        if (s_uart_ours) {
            uart_driver_delete(RADIO_UART_NUM); /* the stack installs its own driver on it */
            s_uart_ours = false;
        }
        xEventGroupClearBits(s_ev, EV_FORMED);
        s_guard.why = RADIO_PORT_STOP_NONE;
        s_guard.starting = 1u;
        atomic_store(&s_running, true);
        if (xTaskCreate(stack_task, "zb_stack", ZB_STACK_TASK_BYTES, NULL, ZB_STACK_PRIORITY,
                        NULL) != pdPASS) {
            ESP_LOGE(TAG, "no task for the Zigbee stack");
            atomic_store(&s_running, false);
            return false;
        }
    }
    const EventBits_t bits = xEventGroupWaitBits(s_ev, EV_FORMED | EV_FAILED, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(ZB_FORM_WAIT_MS));
    return (bits & EV_FORMED) != 0u && atomic_load(&s_formed);
}

bool radio_port_alive(void)
{
    /* The stack says nothing when the radio stops answering; what it does then is
     * docs/features/20 step 4 on the bench (pull the module out, put it back). */
    return atomic_load(&s_running) && atomic_load(&s_formed);
}

/* The stack's lock for what only touches the panel's own tables and the trust center's
 * settings: fine while reset_network runs. */
static bool zb_lock_any(void)
{
    return radio_port_alive() && esp_zigbee_lock_acquire(pdMS_TO_TICKS(ZB_LOCK_WAIT_MS));
}

/* The stack's lock for a hook that sends into the network: refused while reset_network tears
 * it down and forms the next one (s_resetting, set and cleared under this lock), so nothing is
 * sent into a network that is going. The source's link stays up meanwhile: the reset's own
 * REP_DONE, then REP_NETWORK, say when it is over (zsrc_zigbee.h, reset_network). */
static bool zb_lock(void)
{
    if (!zb_lock_any()) {
        return false;
    }
    if (s_resetting) {
        esp_zigbee_lock_release();
        return false;
    }
    return true;
}

/* One request whose time is up, or a channel move that has happened (the stack's lock taken
 * here): false when there is none. */
static bool rq_expire_one(uint64_t now, zsrc_zb_report_t *done, zsrc_zb_report_t *net,
                          bool *has_net)
{
    *has_net = false;
    if (atomic_load(&s_rq_live) == 0u || !zb_lock_any()) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < ZB_RQ_MAX && !found; i++) {
        rq_t *q = &s_rq[i];
        if (q->serial == 0u) {
            continue;
        }
        if (q->kind == (uint8_t)RQ_CHANNEL) {
            const uint8_t ch = ezb_nwk_get_current_channel();
            if (ch == q->channel) {
                atomic_store(&s_channel, ch);
                network_fill(net);
                *has_net = true;
                done_fill(done, q, ZSRC_ZB_DONE_OK, 0u);
                rq_free(q);
                found = true;
                continue;
            }
        }
        if (now >= q->deadline) {
            /* TODO(BL-143): a channel move that never reaches the panel's own radio lands here:
             * esp-zigbee-lib has no call that moves the coordinator alone (nwk.h, core.h). */
            done_fill(done, q, ZSRC_ZB_DONE_TIMEOUT, 0u);
            if (q->kind == (uint8_t)RQ_RESET) {
                s_resetting = false;
            }
            rq_free(q);
            found = true;
        }
    }
    esp_zigbee_lock_release();
    return found;
}

void radio_port_pump(zsrc_zb_t *src, uint64_t now)
{
    if (s_guard.fails != 0u && atomic_load(&s_formed) && now >= s_formed_at &&
        now - s_formed_at >= GUARD_STABLE_MS) {
        s_guard.fails = 0u; /* it has run long enough to be trusted with the next start */
    }
    ev_t e;
    while (s_events != NULL && xQueueReceive(s_events, &e, 0) == pdTRUE) {
        if (e.kind != (uint8_t)EV_REPORT && atomic_load(&s_trace)) {
            trace_ev(now, &e);
        }
        zone_service_lock();
        switch (e.kind) {
        case EV_DEVICE: (void)zsrc_zb_device_seen(src, &e.u.d, now); break;
        case EV_GONE: zsrc_zb_device_gone(src, e.ieee, now); break;
        case EV_FRAME:
            zsrc_zb_frame_seen(src, e.ieee, e.lqi, now);
            if (e.lqi != 0u || e.rssi != 0) {
                /* The last frame's link quality, for the device page (ZB-011). */
                zsrc_zb_report_t *r = &e.u.r;
                memset(r, 0, sizeof(*r));
                r->kind = (uint8_t)ZSRC_ZB_REP_LINK;
                r->ieee = e.ieee;
                r->nwk = e.nwk;
                r->endpoint = e.endpoint;
                r->u.link.lqi = e.lqi;
                r->u.link.rssi = e.rssi;
                deliver(now, r);
            }
            break;
        case EV_ENROLL:
            zsrc_zb_ias_enroll_request(src, e.ieee, e.endpoint, (uint16_t)e.value, now);
            break;
        case EV_IAS: zsrc_zb_ias_status(src, e.ieee, e.endpoint, (uint16_t)e.value, now); break;
        case EV_BOOL:
            zsrc_zb_attr_bool(src, e.ieee, e.endpoint, e.cluster, e.value != 0, now);
            break;
        case EV_VALUE:
            zsrc_zb_attr_value(src, e.ieee, e.endpoint, e.cluster, e.value, now);
            break;
        case EV_BATTERY: zsrc_zb_battery(src, e.ieee, (uint8_t)e.value, now); break;
        case EV_ENERGY: zsrc_zb_energy(src, (int8_t)e.value, now); break;
        case EV_REPORT: {
            if (e.value != 0 && e.u.r.kind == (uint8_t)ZSRC_ZB_REP_ZCL) {
                code_take(e.value, e.u.r.u.zcl.payload, e.u.r.u.zcl.len); /* a keypad's code */
            }
            deliver(now, &e.u.r);
            /* A keypad's Arm carries its code in the payload (zbz_panel.h): this copy is wiped
             * once the panel has it. */
            wipe((volatile uint8_t *)&e.u.r, sizeof(e.u.r));
            break;
        }
        default: break;
        }
        zone_service_unlock();
        zone_service_wake();
    }
    /* Requests whose time is up end TIMEOUT, after everything the queue held for them. The two
     * reports reuse e's room: the zigbee task's stack is small. */
    zsrc_zb_report_t *done = &e.u.r;
    zsrc_zb_report_t net;
    bool has_net = false;
    for (unsigned i = 0; i < ZB_EXPIRE_BATCH && rq_expire_one(now, done, &net, &has_net); i++) {
        zone_service_lock();
        deliver(now, done); /* set_channel: REP_DONE, then REP_NETWORK (zsrc_zigbee.h) */
        if (has_net) {
            deliver(now, &net);
        }
        zone_service_unlock();
        zone_service_wake();
    }
    const unsigned dropped = atomic_exchange(&s_dropped, 0u);
    if (dropped != 0u) {
        ESP_LOGW(TAG, "%u radio events dropped (queue full)", dropped);
    }
    const unsigned lost = atomic_exchange(&s_lost, 0u);
    if (lost != 0u) {
        ESP_LOGE(TAG, "%u alarm-path radio events lost (queue full): zone statuses read again",
                 lost);
    }
    /* A lost zone status, asked again (ZB-003): a listening device answers at once, a sleepy one
     * only if it polls while its parent holds the request; else its next change or supervision
     * report sets it right. The answer comes back as an EV_IAS (attribute()). */
    for (size_t i = 0; i < ZB_REREAD_MAX; i++) {
        taskENTER_CRITICAL(&s_reread_mux);
        const uint64_t ieee = s_reread[i].ieee;
        const uint8_t ep = s_reread[i].endpoint;
        s_reread[i].ieee = 0u;
        taskEXIT_CRITICAL(&s_reread_mux);
        if (ieee != 0u) {
            (void)radio_port_read_attribute(ieee, ep, ZSRC_ZB_ZCL_IAS_ZONE, 0x0002u);
        }
    }
}

void radio_port_queue_counts(uint32_t *dropped, uint32_t *alarm_lost)
{
    *dropped = atomic_load(&s_dropped_all);
    *alarm_lost = atomic_load(&s_lost_all);
}

void radio_port_network(uint8_t *channel, uint16_t *pan_id)
{
    *channel = (uint8_t)atomic_load(&s_channel);
    *pan_id = (uint16_t)atomic_load(&s_pan_id);
}

/* ---- what the source asks (the zone service's lock held: the stack's lock only briefly) ----- */

bool radio_port_permit_join(uint16_t seconds)
{
    if (!zb_lock()) {
        return false;
    }
    /* bdb.h:194-203: the duration is a uint8_t; ZSRC_ZB_LEARN_S (180) fits. */
    const ezb_err_t err = seconds == 0u
                              ? ezb_bdb_close_network()
                              : ezb_bdb_open_network((uint8_t)(seconds > 254u ? 254u : seconds));
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE;
}

bool radio_port_ias_cie_write(uint64_t ieee, uint8_t endpoint)
{
    if (!zb_lock()) {
        return false;
    }
    const uint16_t nwk = nwk_of(ieee);
    bool ok = false;
    if (nwk < ZB_NO_NWK) {
        static ezb_extaddr_t mine; /* the stack copies the request before this returns */
        ezb_nwk_get_extended_address(&mine);
        ezb_zcl_attribute_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.id = EZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID;
        attr.data.type = EZB_ZCL_ATTR_TYPE_EUI64;
        attr.data.size = 8u;
        attr.data.value = mine.u8;
        ezb_zcl_write_attr_cmd_t req;
        memset(&req, 0, sizeof(req));
        req.cmd_ctrl.dst_addr = short_address(nwk);
        req.cmd_ctrl.dst_ep = endpoint;
        req.cmd_ctrl.src_ep = ZB_EP;
        req.cmd_ctrl.cluster_id = ZSRC_ZB_ZCL_IAS_ZONE;
        req.cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE;
        req.cmd_ctrl.cnf_ctx = cnf_of(NULL);
        req.payload.attr_number = 1u;
        req.payload.attr_field = &attr;
        ok = ezb_zcl_write_attr_cmd_req(&req) == EZB_ERR_NONE;
    }
    esp_zigbee_lock_release();
    /* Queued (a refused send is false: ZB11): a sleepy sensor answers in its next wake window,
     * or not (ZB-003). */
    return ok;
}

bool radio_port_ias_enroll_response(uint64_t ieee, uint8_t endpoint, uint8_t zone_id)
{
    if (!zb_lock()) {
        return false;
    }
    const uint16_t nwk = nwk_of(ieee);
    bool ok = false;
    if (nwk < ZB_NO_NWK) {
        ezb_zcl_ias_zone_enroll_rsp_cmd_t rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.cmd_ctrl.dst_addr = short_address(nwk);
        rsp.cmd_ctrl.dst_ep = endpoint;
        rsp.cmd_ctrl.src_ep = ZB_EP;
        rsp.cmd_ctrl.cnf_ctx = cnf_of(NULL);
        rsp.payload.enroll_rsp_code = EZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
        rsp.payload.zone_id = zone_id;
        /* Only a send the stack took counts: a refused one leaves the enrollment due for the
         * source's bounded retry (ZB11). */
        ok = ezb_zcl_ias_zone_enroll_cmd_resp(&rsp) == EZB_ERR_NONE;
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_read_attribute(uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint16_t attr)
{
    if (!zb_lock()) {
        return false;
    }
    const uint16_t nwk = nwk_of(ieee);
    bool ok = false;
    if (nwk < ZB_NO_NWK) {
        static uint16_t id; /* the stack copies the request before this returns */
        id = attr;
        ok = read_attrs(nwk, endpoint, cluster, &id, 1u, NULL); /* refused: false (ZB11) */
    }
    esp_zigbee_lock_release();
    return ok;
}

/* An install code as printed on the device: 6, 8, 12 or 16 bytes and its CRC-16/X.25, low byte
 * first (Zigbee BDB; the same CRC as the Spinel frames above). */
bool radio_port_install_code_ok(const uint8_t *ic, size_t len)
{
    if (ic == NULL || (len != 8u && len != 10u && len != 14u && len != 18u)) {
        return false;
    }
    uint16_t fcs = 0xFFFFu;
    for (size_t i = 0; i + 2u < len; i++) {
        fcs = fcs16(fcs, ic[i]);
    }
    fcs = (uint16_t)~fcs;
    return ic[len - 2u] == (uint8_t)(fcs & 0xFFu) && ic[len - 1u] == (uint8_t)(fcs >> 8);
}

bool radio_port_install_code(uint64_t ieee, const uint8_t *ic, size_t len)
{
    if (ieee == 0u || !radio_port_install_code_ok(ic, len) || !zb_lock_any()) {
        return false;
    }
    const ezb_secur_ic_type_t type = len == 8u    ? EZB_SECUR_IC_TYPE_48
                                     : len == 10u ? EZB_SECUR_IC_TYPE_64
                                     : len == 14u ? EZB_SECUR_IC_TYPE_96
                                                  : EZB_SECUR_IC_TYPE_128;
    ezb_extaddr_t ext;
    ext.u64 = ieee;
    const ezb_err_t err = ezb_secur_ic_add(&ext, type, ic);
    if (err == EZB_ERR_NONE) {
        ic_note(ieee, true); /* how it joins is told by whether a code is held (join_key_of) */
    }
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE;
}

static void on_left(const ezb_zdo_nwk_mgmt_leave_req_result_t *result, void *user_ctx)
{
    (void)result;
    (void)user_ctx; /* the leave indication, when it comes, forgets the device */
}

bool radio_port_remove_device(uint64_t ieee)
{
    if (!zb_lock()) {
        return false;
    }
    const uint16_t nwk = nwk_of(ieee);
    const bool ok = nwk < ZB_NO_NWK;
    if (ok) {
        ezb_zdo_nwk_mgmt_leave_req_t req;
        memset(&req, 0, sizeof(req));
        req.dst_nwk_addr = nwk;
        req.field.device_addr.u64 = ieee;
        req.field.remove_children = false;
        req.field.rejoin = false;
        req.cb = on_left;
        (void)ezb_zdo_nwk_mgmt_leave_req(&req);
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_energy_scan(void)
{
    const unsigned channel = atomic_load(&s_channel);
    if (channel < 11u || channel > 26u || !zb_lock()) {
        return false;
    }
    ezb_nwk_scan_req_t req;
    memset(&req, 0, sizeof(req));
    req.scan_type = EZB_NWK_SCAN_TYPE_ED;
    req.scan_duration = ZB_ED_DURATION;
    req.scan_channels = 1u << channel;
    req.ed_scan_cb = on_energy;
    req.user_ctx = NULL; /* the source's own scan: no request to end */
    const ezb_err_t err = ezb_nwk_scan(&req);
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE;
}

/* ---- BL-143: the hooks appended to zsrc_zb_radio_t (devices.md §3) -------------------------- */

/* Every tagged hook: the stack's lock, a request slot, the call; false when it couldn't even be
 * queued (no radio, no route to the device, the table full, the stack refused). The contract's
 * tags are non-zero: 0 is refused, so a DONE is never mistaken for an unsolicited report. */

bool radio_port_node_desc_req(uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    const uint16_t n = nwk_for(ieee, nwk);
    rq_t *q = n < ZB_NO_NWK ? rq_new(RQ_ZDO, tag, ieee, n, 0u, 0u, ZSRC_ZB_STEP_NODE_DESC, ZB_RQ_MS)
                            : NULL;
    if (q != NULL) {
        ok = zdo_node_desc(n, q->serial);
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_power_desc_req(uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    const uint16_t n = nwk_for(ieee, nwk);
    rq_t *q = n < ZB_NO_NWK
                  ? rq_new(RQ_ZDO, tag, ieee, n, 0u, 0u, ZSRC_ZB_STEP_POWER_DESC, ZB_RQ_MS)
                  : NULL;
    if (q != NULL) {
        ok = zdo_power_desc(n, q->serial);
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_active_ep_req(uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    const uint16_t n = nwk_for(ieee, nwk);
    rq_t *q = n < ZB_NO_NWK ? rq_new(RQ_ZDO, tag, ieee, n, 0u, 0u, ZSRC_ZB_STEP_ACTIVE_EP, ZB_RQ_MS)
                            : NULL;
    if (q != NULL) {
        ok = zdo_active_ep(n, q->serial);
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_simple_desc_req(uint64_t ieee, uint16_t nwk, uint8_t endpoint, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || endpoint == 0u || endpoint > 240u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    const uint16_t n = nwk_for(ieee, nwk);
    rq_t *q = n < ZB_NO_NWK ? rq_new(RQ_ZDO, tag, ieee, n, endpoint, 0u,
                                     ZSRC_ZB_STEP_SIMPLE_DESC, ZB_RQ_MS)
                            : NULL;
    if (q != NULL) {
        ok = zdo_simple_desc(n, endpoint, q->serial);
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

static uint8_t step_of_cluster(uint16_t cluster)
{
    switch (cluster) {
    case ZSRC_ZB_ZCL_BASIC: return ZSRC_ZB_STEP_BASIC;
    case ZSRC_ZB_ZCL_IAS_ZONE: return ZSRC_ZB_STEP_IAS_ZONE;
    case ZSRC_ZB_ZCL_POLL_CTRL: return ZSRC_ZB_STEP_POLL_CTRL;
    default: return ZSRC_ZB_STEP_NONE;
    }
}

/* A ZCL request slot for `ieee`, sending command `sent` and waiting for `expect`: the short
 * address it goes to in *nwk. */
static rq_t *zcl_rq(uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint8_t sent,
                    uint8_t expect, uint8_t step, uint32_t tag, uint16_t *nwk)
{
    *nwk = nwk_of(ieee);
    if (*nwk >= ZB_NO_NWK) {
        return NULL;
    }
    rq_t *q = rq_new(RQ_ZCL, tag, ieee, *nwk, endpoint, cluster, step, ZB_RQ_MS);
    if (q != NULL) {
        q->sent = sent;
        q->expect = expect;
    }
    return q;
}

bool radio_port_read_attributes(uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                                const uint16_t *attrs, uint8_t count, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || attrs == NULL || count == 0u || count > 8u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    uint16_t nwk = ZB_NO_NWK;
    rq_t *q = zcl_rq(ieee, endpoint, cluster, ZCL_READ_ATTR, ZCL_READ_ATTR_RSP,
                     step_of_cluster(cluster), tag, &nwk);
    if (q != NULL) {
        static uint16_t ids[8]; /* the stack copies the request before this returns */
        memcpy(ids, attrs, (size_t)count * sizeof(ids[0]));
        ok = read_attrs(nwk, endpoint, cluster, ids, count, q);
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

static void on_bind(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    rq_t *q = rq_by_serial((uint32_t)(uintptr_t)user_ctx);
    if (q != NULL) {
        zdo_end(q, result != NULL ? result->error : EZB_ERR_FAIL,
                result != NULL && result->rsp != NULL ? result->rsp->status : 0u);
    }
}

bool radio_port_bind(uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    const uint16_t nwk = nwk_of(ieee);
    rq_t *q = nwk < ZB_NO_NWK
                  ? rq_new(RQ_ZDO, tag, ieee, nwk, endpoint, cluster, ZSRC_ZB_STEP_BIND, ZB_RQ_MS)
                  : NULL;
    if (q != NULL) {
        /* Bind_req to the device: its `cluster` at `endpoint` to the panel's endpoint, by the
         * panel's IEEE address (zdo_bind_mgmt.h:20-36, DstAddrMode 0x03). */
        ezb_extaddr_t mine;
        ezb_nwk_get_extended_address(&mine);
        ezb_zdo_bind_req_t req;
        memset(&req, 0, sizeof(req));
        req.dst_nwk_addr = nwk;
        req.field.src_addr.u64 = ieee;
        req.field.src_ep = endpoint;
        req.field.cluster_id = cluster;
        req.field.dst_addr_mode = 0x03u;
        req.field.dst_addr.extended_addr = mine;
        req.field.dst_ep = ZB_EP;
        req.cb = on_bind;
        req.user_ctx = (void *)(uintptr_t)q->serial;
        ok = ezb_zdo_bind_req(&req) == EZB_ERR_NONE;
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_configure_reporting(uint64_t ieee, uint8_t endpoint, const zsrc_zb_reporting_t *r,
                                    uint32_t tag)
{
    if (ieee == 0u || tag == 0u || r == NULL || !zb_lock()) {
        return false;
    }
    bool ok = false;
    uint16_t nwk = ZB_NO_NWK;
    rq_t *q = zcl_rq(ieee, endpoint, r->cluster, ZCL_CONFIG_REP, ZCL_CONFIG_REP_RSP,
                     ZSRC_ZB_STEP_REPORTING, tag, &nwk);
    if (q != NULL) {
        static ezb_zcl_config_report_record_t rec; /* the stack copies the request */
        memset(&rec, 0, sizeof(rec));
        rec.direction = EZB_ZCL_REPORTING_SEND; /* the device reports to us (zcl_type.h:62) */
        rec.attr_id = r->attr;
        rec.client.attr_type = r->type;
        rec.client.min_interval = r->min_s;
        rec.client.max_interval = r->max_s;
        /* The reportable change is as wide as the attribute's type (zcl_type.h:85-107): written
         * as u32, the little-endian low bytes are what an 8- or 16-bit type reads. */
        rec.client.reportable_change.u32 = r->change;
        ezb_zcl_config_report_cmd_t req;
        memset(&req, 0, sizeof(req));
        req.cmd_ctrl.dst_addr = short_address(nwk);
        req.cmd_ctrl.dst_ep = endpoint;
        req.cmd_ctrl.src_ep = ZB_EP;
        req.cmd_ctrl.cluster_id = r->cluster;
        req.cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE;
        req.cmd_ctrl.cnf_ctx = cnf_of(q);
        req.payload.record_number = 1u;
        req.payload.record_field = &rec;
        ok = ezb_zcl_config_report_cmd_req(&req) == EZB_ERR_NONE;
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

static void on_leave_rsp(const ezb_zdo_nwk_mgmt_leave_req_result_t *result, void *user_ctx)
{
    rq_t *q = rq_by_serial((uint32_t)(uintptr_t)user_ctx);
    if (q == NULL) {
        return; /* it left before answering: report_left ended the request */
    }
    const ezb_err_t error = result != NULL ? result->error : EZB_ERR_FAIL;
    const uint8_t status = result != NULL && result->rsp != NULL ? result->rsp->status : 0u;
    if (error == EZB_ERR_NONE && status == EZB_ZDP_STATUS_SUCCESS) {
        leaving_note(q->ieee, q->tag); /* its leave indication follows with this tag */
    }
    zdo_end(q, error, status);
}

bool radio_port_leave(uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    const uint16_t n = nwk_for(ieee, nwk);
    rq_t *q = n < ZB_NO_NWK ? rq_new(RQ_LEAVE, tag, ieee, n, 0u, 0u, ZSRC_ZB_STEP_NONE, ZB_LEAVE_WAIT_MS)
                            : NULL;
    if (q != NULL) {
        /* Mgmt_Leave_req to the device itself, rejoin off (zdo_nwk_mgmt.h:164-186; ZB-013). */
        ezb_zdo_nwk_mgmt_leave_req_t req;
        memset(&req, 0, sizeof(req));
        req.dst_nwk_addr = n;
        req.field.device_addr.u64 = ieee;
        req.field.remove_children = false;
        req.field.rejoin = false;
        req.cb = on_leave_rsp;
        req.user_ctx = (void *)(uintptr_t)q->serial;
        ok = ezb_zdo_nwk_mgmt_leave_req(&req) == EZB_ERR_NONE;
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_forget(uint64_t ieee)
{
    if (ieee == 0u || !zb_lock_any()) {
        return false;
    }
    /* A refused removal keeps the code in the mirror: forgetting again asks again (ZB12). The
     * panel's forget doesn't look at this answer (zbz_panel.c drop_device drops its record either
     * way), so a failed one keeps the mirror and is asked again only by the next forget of that
     * device (once it has joined again), never by itself. */
    const bool ok = !ic_has(ieee) || ic_remove(ieee);
    /* TODO(BL-143): esp-zigbee-lib has no call that drops one device's address map entry or its
     * trust center link key (secur.h: install codes and the network key only; nwk.h: lookups
     * only) - devices.md open question 1. What the panel itself keeps goes. */
    forget_device(ieee);
    joiner_t *j = joiner_for(ieee, false);
    if (j != NULL) {
        memset(j, 0, sizeof(*j));
    }
    for (size_t i = 0; i < ZB_AWAKE_MAX; i++) {
        if (s_awake[i].ieee == ieee) {
            s_awake[i].ieee = 0u;
        }
    }
    (void)leaving_take(ieee);
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_identify(uint64_t ieee, uint8_t endpoint, uint16_t seconds, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    uint16_t nwk = ZB_NO_NWK;
    rq_t *q = zcl_rq(ieee, endpoint, ZSRC_ZB_ZCL_IDENTIFY, ZCL_IDENTIFY_CMD, ZCL_DEFAULT_RSP,
                     ZSRC_ZB_STEP_NONE, tag, &nwk);
    if (q != NULL) {
        /* Identify (0x00) with a Default Response asked: the device blinks or beeps itself, never
         * the panel (ZB-014; identify.h:17-23, :74-80). */
        ezb_zcl_identify_cmd_t req;
        memset(&req, 0, sizeof(req));
        req.cmd_ctrl.dst_addr = short_address(nwk);
        req.cmd_ctrl.dst_ep = endpoint;
        req.cmd_ctrl.src_ep = ZB_EP;
        req.cmd_ctrl.dis_default_rsp = false;
        req.cmd_ctrl.cnf_ctx = cnf_of(q);
        req.payload.identify_time = seconds;
        ok = ezb_zcl_identify_identify_cmd_req(&req) == EZB_ERR_NONE;
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_ias_test_mode(uint64_t ieee, uint8_t endpoint, uint8_t seconds, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    uint16_t nwk = ZB_NO_NWK;
    rq_t *q = zcl_rq(ieee, endpoint, ZSRC_ZB_ZCL_IAS_ZONE,
                     seconds != 0u ? ZCL_IAS_TEST_MODE : ZCL_IAS_NORMAL_MODE, ZCL_DEFAULT_RSP,
                     ZSRC_ZB_STEP_NONE, tag, &nwk);
    if (q != NULL) {
        if (seconds != 0u) {
            /* Initiate Test Mode's duration is one byte of seconds (ias_zone.h:55-58, ZCL
             * 8.2.2.3.2), as the hook asks: the walk test asks again every
             * ZSRC_ZB_TEST_REFRESH_MS while it runs. */
            ezb_zcl_ias_zone_initiate_test_mode_cmd_t req;
            memset(&req, 0, sizeof(req));
            req.cmd_ctrl.dst_addr = short_address(nwk);
            req.cmd_ctrl.dst_ep = endpoint;
            req.cmd_ctrl.src_ep = ZB_EP;
            req.cmd_ctrl.cnf_ctx = cnf_of(q);
            req.payload.duration = seconds;
            req.payload.current_zone_sensitivity_level = 0u; /* the device's default */
            ok = ezb_zcl_ias_zone_initiate_test_mode_cmd_req(&req) == EZB_ERR_NONE;
        } else {
            ezb_zcl_ias_zone_initiate_normal_operation_mode_cmd_t req;
            memset(&req, 0, sizeof(req));
            req.cmd_ctrl.dst_addr = short_address(nwk);
            req.cmd_ctrl.dst_ep = endpoint;
            req.cmd_ctrl.src_ep = ZB_EP;
            req.cmd_ctrl.cnf_ctx = cnf_of(q);
            ok = ezb_zcl_ias_zone_initiate_normal_operation_mode_cmd_req(&req) == EZB_ERR_NONE;
        }
        if (!ok) {
            rq_free(q);
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

/* A cluster command the siren and keypad policies built (ZB-023, ZB-024), sent as the bytes they
 * are: an APS data request carrying the ZCL frame (aps.h:59-79, :152-164). ezb_zcl_custom_cmd_req
 * takes only manufacturer clusters (custom.h:95), and the clusters' own requests can't answer
 * with a request's sequence number (an Arm Response must). A new frame takes our own sequence
 * number; the stack's counter is not readable, so a clash is possible but only matters for an
 * answer from the same device and cluster within the same few seconds. */
static uint8_t s_raw_tsn = 0x80u;

/* One ZCL frame as an APS data request to `ieee`/`endpoint` (the stack's lock held): the header
 * (frame control, the manufacturer when there is one, the sequence number, the command) and the
 * payload. A tagged request slot ends when the frame's answer comes (rq_match_zcl: a Default
 * Response, or the command's own answer), or at once - sent - when none is asked for. */
static bool send_raw(uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint8_t fc,
                     uint16_t manufacturer, uint8_t tsn, uint8_t command, const uint8_t *payload,
                     uint8_t len, uint32_t tag)
{
    uint16_t nwk = ZB_NO_NWK;
    rq_t *q = zcl_rq(ieee, endpoint, cluster, command, ZCL_DEFAULT_RSP, ZSRC_ZB_STEP_NONE, tag,
                     &nwk);
    if (q == NULL) {
        return false;
    }
    static uint8_t asdu[ZB_RAW_ASDU_MAX]; /* the stack copies the request */
    size_t n = 0;
    asdu[n++] = fc;
    if ((fc & ZCL_FC_MANUF) != 0u) {
        asdu[n++] = (uint8_t)(manufacturer & 0xFFu);
        asdu[n++] = (uint8_t)((unsigned)manufacturer >> 8);
    }
    asdu[n++] = tsn;
    asdu[n++] = command;
    if (len != 0u) {
        memcpy(&asdu[n], payload, len);
        n += len;
    }
    q->tsn = tsn;
    q->tsn_known = true;

    ezb_apsde_data_req_t req;
    memset(&req, 0, sizeof(req));
    req.dst_address = short_address(nwk);
    req.src_endpoint = ZB_EP;
    req.dst_endpoint = endpoint;
    req.cluster_id = cluster;
    req.profile_id = ZB_PROFILE_HA;
    req.radius = 0u; /* the network's default */
    req.tx_options = EZB_APSDE_TX_OPT_ACK_TX; /* NWK security as every frame; no APS key */
    req.asdu_length = (uint16_t)n;
    req.asdu = asdu;
    const bool ok = ezb_apsde_data_request(&req) == EZB_ERR_NONE;
    if (!ok) {
        rq_free(q);
    } else if ((fc & ZCL_FC_NO_DEFAULT) != 0u) {
        /* Nothing will answer (an ACE answer, a squawk without a confirmation, a Default
         * Response): it ends sent. The APS confirm is not ours to take (aps.h:144-150 hands every
         * confirm to whoever registers it). */
        rq_post_done(q, ZSRC_ZB_DONE_OK, 0u);
    }
    return ok;
}

bool radio_port_zcl_command(uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || cmd == NULL || cmd->len > ZSRC_ZB_ZCL_PAYLOAD_MAX ||
        (cmd->tsn != ZSRC_ZB_TSN_NEW && cmd->tsn > 0xFFu) || !zb_lock()) {
        return false;
    }
    uint8_t fc = EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC;
    if (cmd->manufacturer != 0u) {
        fc = (uint8_t)(fc | ZCL_FC_MANUF);
    }
    if (cmd->server_to_client) {
        fc = (uint8_t)(fc | ZCL_FC_TO_CLIENT);
    }
    if (!cmd->want_response) {
        fc = (uint8_t)(fc | ZCL_FC_NO_DEFAULT);
    }
    /* The answer to a request carries the request's own sequence number (an Arm Response, a
     * Get Panel Status Response); ZSRC_ZB_TSN_NEW takes the next of ours. */
    const uint8_t tsn = cmd->tsn == ZSRC_ZB_TSN_NEW ? s_raw_tsn++ : (uint8_t)cmd->tsn;
    const bool ok = send_raw(ieee, endpoint, cmd->cluster, fc, cmd->manufacturer, tsn,
                             cmd->command, cmd->payload, cmd->len, tag);
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_zcl_default_rsp(uint64_t ieee, uint8_t endpoint, uint16_t cluster, bool to_client,
                                uint8_t tsn, uint8_t command, uint8_t status, uint32_t tag)
{
    if (ieee == 0u || tag == 0u || !zb_lock()) {
        return false;
    }
    /* A global command (frame type 0), never answered itself (ZCL 2.5.12). */
    uint8_t fc = ZCL_FC_NO_DEFAULT;
    if (to_client) {
        fc = (uint8_t)(fc | ZCL_FC_TO_CLIENT);
    }
    const uint8_t payload[2] = {command, status};
    const bool ok = send_raw(ieee, endpoint, cluster, fc, 0u, tsn, ZCL_DEFAULT_RSP, payload, 2u,
                             tag);
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_install_code_set(uint64_t ieee, const uint8_t *code, uint8_t len)
{
    if (len != 0u) {
        return radio_port_install_code(ieee, code, len);
    }
    if (ieee == 0u || !zb_lock_any()) {
        return false;
    }
    const bool ok = ic_remove(ieee);
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_codes_only(bool on)
{
    if (!zb_lock_any()) {
        return false;
    }
    const ezb_err_t err = ezb_secur_set_ic_required(on); /* secur.h:58-65 */
    if (err == EZB_ERR_NONE) {
        s_codes_only = on;
    }
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE;
}

static void on_nwk_update(const ezb_zdo_nwk_mgmt_nwk_update_req_result_t *result, void *user_ctx)
{
    (void)result; /* a channel change has no answer: the pump watches the panel's own channel */
    (void)user_ctx;
}

bool radio_port_set_channel(uint8_t channel, uint32_t tag)
{
    if (tag == 0u || channel < 11u || channel > 26u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    if (rq_of_kind(RQ_CHANNEL, 0u) == NULL && rq_of_kind(RQ_RESET, 0u) == NULL) {
        rq_t *q = rq_new(RQ_CHANNEL, tag, 0u, 0u, 0u, 0u, ZSRC_ZB_STEP_NONE, ZB_CHANNEL_WAIT_MS);
        if (q != NULL) {
            q->channel = channel;
            if (ezb_nwk_get_current_channel() == channel) {
                ok = true; /* already there: the pump ends it at once */
            } else {
                /* Mgmt_NWK_Update_req to every listening device: ScanDuration 0xFE moves them
                 * to the one channel in ScanChannels (zdo_nwk_mgmt.h:236-283; Zigbee 2.4.3.3.9:
                 * bit n of the mask is channel n, as the ED scan's mask, nwk.h:589). The
                 * network manager moves itself after its broadcast; the pump checks it did. */
                ezb_zdo_nwk_mgmt_nwk_update_req_t req;
                memset(&req, 0, sizeof(req));
                req.dst_nwk_addr = ZB_BCAST_RX_ON;
                req.field.scan_channels = 1u << channel;
                req.field.scan_duration = ZB_NEW_CHANNEL;
                req.field.scan_count = 0u;
                req.field.nwk_update_id = (uint8_t)(ezb_nwk_get_update_id() + 1u);
                req.field.nwk_mgmt_addr = 0x0000u;
                req.cb = on_nwk_update;
                req.user_ctx = NULL;
                ok = ezb_zdo_nwk_mgmt_nwk_update_req(&req) == EZB_ERR_NONE;
            }
            if (!ok) {
                rq_free(q);
            }
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_reset_network(uint32_t tag)
{
    if (tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    if (!s_resetting && rq_of_kind(RQ_CHANNEL, 0u) == NULL) {
        rq_t *q = rq_new(RQ_RESET, tag, 0u, 0u, 0u, 0u, ZSRC_ZB_STEP_NONE, ZB_RESET_WAIT_MS);
        if (q != NULL) {
            /* The local reset leaves the network and clears its data, the frame counter kept,
             * then says EZB_ZDO_SIGNAL_LEAVE (bdb.h:214-221); on_signal forms a new network.
             * Not esp_zigbee_factory_reset: that restarts the whole panel (esp_zigbee.h:234-240;
             * devices.md open question 2). */
            s_resetting = true;
            ezb_bdb_reset_via_local_action();
            ok = true;
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_channel_survey(uint32_t tag)
{
    if (tag == 0u || !zb_lock()) {
        return false;
    }
    bool ok = false;
    if (rq_of_kind(RQ_SURVEY, 0u) == NULL) {
        rq_t *q = rq_new(RQ_SURVEY, tag, 0u, 0u, 0u, 0u, ZSRC_ZB_STEP_NONE, ZB_SURVEY_MS);
        if (q != NULL) {
            s_survey_known = 0u;
            memset(s_survey_dbm, 0, sizeof(s_survey_dbm));
            ezb_nwk_scan_req_t req;
            memset(&req, 0, sizeof(req));
            req.scan_type = EZB_NWK_SCAN_TYPE_ED;
            req.scan_duration = ZB_SURVEY_DURATION;
            for (size_t i = 0; i < ZSRC_ZB_CHANNELS; i++) {
                req.scan_channels |= 1u << s_survey_channels[i];
            }
            req.ed_scan_cb = on_energy;
            req.user_ctx = (void *)(uintptr_t)q->serial;
            ok = ezb_nwk_scan(&req) == EZB_ERR_NONE;
            if (!ok) {
                rq_free(q);
            }
        }
    }
    esp_zigbee_lock_release();
    return ok;
}

bool radio_port_wake_window(uint64_t ieee, uint16_t seconds)
{
    if (ieee == 0u || !zb_lock_any()) {
        return false;
    }
    awake_t *slot = NULL;
    for (size_t i = 0; i < ZB_AWAKE_MAX; i++) {
        if (s_awake[i].ieee == ieee) {
            slot = &s_awake[i];
            break;
        }
        if (slot == NULL || s_awake[i].until < slot->until) {
            slot = &s_awake[i]; /* a free one (until 0) or the one ending first */
        }
    }
    if (seconds == 0u) {
        if (slot != NULL && slot->ieee == ieee) {
            slot->ieee = 0u;
            slot->until = 0u;
        }
    } else if (slot != NULL) {
        slot->ieee = ieee;
        slot->until = ms_now() + (uint64_t)seconds * 1000u;
    }
    esp_zigbee_lock_release();
    return true;
}

bool radio_port_restart(void)
{
    if (!zb_lock_any()) {
        return false;
    }
    /* Leaves the main loop (esp_zigbee.h:163-172): stack_task ends every waiting request
     * NO_RADIO, deinitialises the stack and lets go of the UART; the service sees the radio gone
     * and brings it up again through the probe, the stored network and its devices kept. */
    const esp_err_t err = esp_zigbee_stop();
    esp_zigbee_lock_release();
    return err == ESP_OK;
}

#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
/* esp-zigbee-lib 2.0.4's esp32p4 build is made for P4 v3: esp_zigbee_task_queue_post() reads the
 * CLIC's standard threshold and status registers (CSRs 0x347 and 0xFB1), which v1.x silicon
 * doesn't have, and the read is an illegal instruction (docs/bugs/B024). The linker sends every
 * call here instead (CMakeLists.txt, --wrap). This file never posts; the stack's own caller is
 * esp_zigbee_stop(), whose callback only tells the main loop to end. So the callback runs at once
 * on the caller's task under the stack's lock (recursive: radio_port_restart already holds it),
 * then the main loop is woken to see it (ezbee/tasklet.h; its descriptor exists while the stack
 * runs, and esp_zigbee_stop posts only then). */
esp_err_t __wrap_esp_zigbee_task_queue_post(esp_zigbee_callback_t cb, void *ctx);
esp_err_t __wrap_esp_zigbee_task_queue_post(esp_zigbee_callback_t cb, void *ctx)
{
    if (cb == NULL || xPortInIsrContext() || !atomic_load(&s_running) ||
        !esp_zigbee_lock_acquire(pdMS_TO_TICKS(ZB_LOCK_WAIT_MS))) {
        return ESP_FAIL;
    }
    cb(ctx);
    esp_zigbee_lock_release();
    ezb_tasklet_signal_pending();
    return ESP_OK;
}
#endif

/* ESP-IDF's radio Spinel layer (components/openthread/src/spinel/esp_radio_spinel.cpp) gives its
 * one SpinelDriver to Init() on every esp_zigbee_init but never calls its Deinit(), so the stack's
 * second start in one boot adds the IID list again and OpenThread ends the panel
 * (spinel_driver.cpp:82, docs/bugs/B024). ESP-IDF's own OpenThread port calls Deinit() when it
 * stops (esp_openthread_radio_spinel.cpp). The linker sends both calls here (CMakeLists.txt,
 * --wrap): a driver that was started before is reset to new first, and its reset-failure callback
 * - which the reset clears - is set again. OpenThread's C++ ABI from C: `this` is the first
 * argument, a reference is a pointer, the coprocessor type is a plain enum. */
typedef void (*ot_reset_failure_cb_t)(void *ctx);
static void *s_drv_started;              /* the driver Init last ran on */
static void *s_drv_cb_on;                /* the driver the callback below was set on */
static ot_reset_failure_cb_t s_drv_cb;
static void *s_drv_cb_ctx;

void _ZN2ot6Spinel12SpinelDriver6DeinitEv(void *self);
void __real__ZN2ot6Spinel12SpinelDriver34SetCoprocessorResetFailureCallbackEPFvPvES2_(
    void *self, ot_reset_failure_cb_t cb, void *ctx);
void __wrap__ZN2ot6Spinel12SpinelDriver34SetCoprocessorResetFailureCallbackEPFvPvES2_(
    void *self, ot_reset_failure_cb_t cb, void *ctx);
void __wrap__ZN2ot6Spinel12SpinelDriver34SetCoprocessorResetFailureCallbackEPFvPvES2_(
    void *self, ot_reset_failure_cb_t cb, void *ctx)
{
    s_drv_cb_on = self;
    s_drv_cb = cb;
    s_drv_cb_ctx = ctx;
    __real__ZN2ot6Spinel12SpinelDriver34SetCoprocessorResetFailureCallbackEPFvPvES2_(self, cb, ctx);
}

int __real__ZN2ot6Spinel12SpinelDriver4InitERNS0_15SpinelInterfaceEbPKhh(
    void *self, void *iface, bool soft_reset, const uint8_t *iids, uint8_t n);
int __wrap__ZN2ot6Spinel12SpinelDriver4InitERNS0_15SpinelInterfaceEbPKhh(
    void *self, void *iface, bool soft_reset, const uint8_t *iids, uint8_t n);
int __wrap__ZN2ot6Spinel12SpinelDriver4InitERNS0_15SpinelInterfaceEbPKhh(
    void *self, void *iface, bool soft_reset, const uint8_t *iids, uint8_t n)
{
    if (s_drv_started == self) {
        _ZN2ot6Spinel12SpinelDriver6DeinitEv(self);
        if (s_drv_cb_on == self) {
            __real__ZN2ot6Spinel12SpinelDriver34SetCoprocessorResetFailureCallbackEPFvPvES2_(
                self, s_drv_cb, s_drv_cb_ctx);
        }
        ESP_LOGI(TAG, "the radio's Spinel driver was reset for the stack's new start");
    }
    s_drv_started = self;
    return __real__ZN2ot6Spinel12SpinelDriver4InitERNS0_15SpinelInterfaceEbPKhh(self, iface,
                                                                              soft_reset, iids, n);
}

/* The stack's main loop can end with a frame still on the air: the radio is then transmitting,
 * OpenThread's RadioSpinel::Sleep() refuses anything but receiving, and esp-zigbee's
 * ezb_plat_radio_sleep() aborts on that refusal while esp_zigbee_deinit takes the network down
 * (B024, the fourth restart on the bench). The linker sends the stack's sleep calls here
 * (CMakeLists.txt): only while stack_task deinitialises it, a refused sleep first takes the radio
 * back to receiving - what the MAC does after every frame, and it drops the frame's pending answer,
 * which OpenThread then logs as unexpected - and asks again. */
esp_err_t __real_esp_radio_spinel_sleep(esp_radio_spinel_idx_t idx);
esp_err_t __wrap_esp_radio_spinel_sleep(esp_radio_spinel_idx_t idx);
esp_err_t __wrap_esp_radio_spinel_sleep(esp_radio_spinel_idx_t idx)
{
    esp_err_t err = __real_esp_radio_spinel_sleep(idx);
    const unsigned channel = atomic_load(&s_channel);
    if (err != ESP_OK && s_deinit && channel != 0u &&
        esp_radio_spinel_receive((uint8_t)channel, idx) == ESP_OK) {
        ESP_LOGW(TAG, "the stack stopped during a transmission: the radio sleeps after it");
        err = __real_esp_radio_spinel_sleep(idx);
    }
    return err;
}

/* The radio's pending table (B021). The stack puts a sleepy device's address in the H2's table
 * when it queues a frame for it, so the H2 answers the device's next poll with Frame Pending and
 * the device stays awake for the frame; once the frame is out it takes the address out again.
 * esp-zigbee-lib 2.0.4 links that last call to weak stubs of its own that only return
 * ESP_ERR_NOT_SUPPORTED: addresses never left the table, its 20 places filled up, and a device
 * that joined after that was never told a frame waited - its key exchange failed and it left.
 * ESP-IDF adds the two calls after v6.1.0 (commit 68d2d8c4); these are the same, on the same
 * object. Its RadioSpinel is a static of esp_radio_spinel.cpp, so the linker sends the add calls
 * through here first (CMakeLists.txt, --wrap) to learn where it is: an address is only ever
 * cleared after it was added. All of it runs on the stack's task, as the stack's own calls do. */
static void *s_radio_obj; /* RadioSpinel for ESP_RADIO_SPINEL_ZIGBEE, once it was used */

int __real__ZN2ot6Spinel11RadioSpinel21AddSrcMatchShortEntryEt(void *self, uint16_t short_addr);
int __wrap__ZN2ot6Spinel11RadioSpinel21AddSrcMatchShortEntryEt(void *self, uint16_t short_addr);
int __wrap__ZN2ot6Spinel11RadioSpinel21AddSrcMatchShortEntryEt(void *self, uint16_t short_addr)
{
    s_radio_obj = self;
    return __real__ZN2ot6Spinel11RadioSpinel21AddSrcMatchShortEntryEt(self, short_addr);
}

int __real__ZN2ot6Spinel11RadioSpinel19AddSrcMatchExtEntryERK12otExtAddress(void *self,
                                                                            const uint8_t *ext);
int __wrap__ZN2ot6Spinel11RadioSpinel19AddSrcMatchExtEntryERK12otExtAddress(void *self,
                                                                            const uint8_t *ext);
int __wrap__ZN2ot6Spinel11RadioSpinel19AddSrcMatchExtEntryERK12otExtAddress(void *self,
                                                                            const uint8_t *ext)
{
    s_radio_obj = self;
    return __real__ZN2ot6Spinel11RadioSpinel19AddSrcMatchExtEntryERK12otExtAddress(self, ext);
}

int _ZN2ot6Spinel11RadioSpinel23ClearSrcMatchShortEntryEt(void *self, uint16_t short_addr);
int _ZN2ot6Spinel11RadioSpinel21ClearSrcMatchExtEntryERK12otExtAddress(void *self,
                                                                     const uint8_t *ext);

esp_err_t esp_radio_spinel_clear_short_entry(uint16_t short_address, esp_radio_spinel_idx_t idx);
esp_err_t esp_radio_spinel_clear_short_entry(uint16_t short_address, esp_radio_spinel_idx_t idx)
{
    if (idx != ESP_RADIO_SPINEL_ZIGBEE || s_radio_obj == NULL) {
        return ESP_OK; /* nothing was added: nothing to take out */
    }
    return _ZN2ot6Spinel11RadioSpinel23ClearSrcMatchShortEntryEt(s_radio_obj, short_address) == 0
               ? ESP_OK
               : ESP_FAIL;
}

/* `ext_address` in the order the stack gives esp_radio_spinel_add_extended_entry (RadioSpinel
 * turns it around for the wire, the same way for both). */
esp_err_t esp_radio_spinel_clear_extended_entry(const uint8_t *ext_address,
                                                esp_radio_spinel_idx_t idx);
esp_err_t esp_radio_spinel_clear_extended_entry(const uint8_t *ext_address,
                                                esp_radio_spinel_idx_t idx)
{
    if (idx != ESP_RADIO_SPINEL_ZIGBEE || s_radio_obj == NULL || ext_address == NULL) {
        return ESP_OK;
    }
    uint8_t ext[8]; /* otExtAddress: eight bytes, no more */
    memcpy(ext, ext_address, sizeof(ext));
    return _ZN2ot6Spinel11RadioSpinel21ClearSrcMatchExtEntryERK12otExtAddress(s_radio_obj, ext) == 0
               ? ESP_OK
               : ESP_FAIL;
}

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */
