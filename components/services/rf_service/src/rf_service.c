/*
 * rf_service.c - the 433 MHz sensor receiver (see rf_service.h).
 *
 * Two kinds of receiver feed the same decoder and source (RF-001):
 *   - a CC1101 on SPI, its demodulated line on the data pin: tunable, with RSSI for jamming;
 *   - a fixed superheterodyne module (RX470-4 / WL101-341 / RXB6 / SRX882 - the "433 MHz
 *     transmitter and receiver kits"): VCC, GND and DATA on the data pin, nothing to configure.
 *     Its AGC turns the empty band into a stream of static, which is how the service tells a
 *     module on the pin from an empty pin (pulled down: no edges at all), and a carrier holds
 *     its line high, which is how jamming shows without an RSSI.
 * `auto` (the default) asks the SPI bus for a CC1101 first and, with none there, listens to the
 * data pin. `rf module cc1101|ook|auto|off` fixes the choice per panel (NVS "rf"/"mode"); `off`
 * (RF-020) touches no pin at all, for a panel without a receiver - or with the sensor radio in the
 * wireless socket, whose pins `auto` would otherwise probe. `rf place` (RF-019) moves the whole
 * receiver to another place the board offers (a CC1101 on the 7"'s J7 header).
 */
#include "rf_service.h"
#include "alarm_mem.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_ALARM_ZSRC_RF

#include "alarm_service.h"
#include "esp_check.h"
#include "board_info/board_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rf_decode/rf_decode.h"
#include "rf_rx/cc1101.h"
#include "rf_rx/rf_capture.h"
#include "zone_service.h"

_Static_assert(RF_CAPTURE_IDLE_US == RF_RECEPTION_IDLE_US,
               "the capture ends a reception where the decoder expects it to");

static const char *TAG = "rf";

#define TASK_STACK     4096
#define TASK_PRIORITY  4
#define POLL_MS        100u   /* the task looks around this often when nothing arrives */
#define RSSI_EVERY_MS  200u   /* a noise sample, between frames (RF-005) */
#define RSSI_QUIET_MS  100u   /* ... only when nothing was heard for this long */
#define RSSI_BUSY_MS   1000u  /* ... and at least this often when the line never goes quiet: a
                               * noise jammer keeps the demodulator toggling, and waiting for a
                               * quiet line would never measure it (RF-005) */
#define CHECK_EVERY_MS 5000u  /* the CC1101 is asked whether it is still receiving */
#define RECAL_EVERY_MS 3600000u /* ... and calibrated again this often: TI asks for regular
                                 * calibration (it holds over about +-40 C), and receivers left
                                 * alone for hours have been seen to go deaf (rtl_433_ESP #16) */
#define WINDOW_MS      1000u  /* edge rate and line duty are measured over this */
#define DUMP_MAX       4096u  /* pulses a console capture keeps */

/* The superheterodyne module (see the top). All three are for the bench to settle. */
#define OOK_FOUND_EDGES   20u      /* edges in one window: something drives the pin */
#define OOK_SILENT_MS     300000u  /* no edge at all this long: the module is gone */
#define OOK_CARRIER_DUTY  85u      /* % of a window high: a carrier holds the line */
#define STORM_PAUSE_MS    10000u   /* a pin flooded with edges rests this long */

/* What the console asks of the task (S.requests). The task's notification only wakes it: the
 * capture's interrupt counts wake-ups in the same notification value, so bits there would be
 * read out of a count (a burst of receptions asked for a retune: docs/bugs/B011). */
#define NB_RETRY  0x01u
#define NB_RETUNE 0x02u

typedef enum { RX_NONE = 0, RX_CC1101, RX_OOK } rx_kind_t;

static struct {
    rf_service_config_t cfg;
    cc1101_t chip;          /* the rf task's alone: CS is driven by hand, so two tasks on the
                             * bus would split each other's transactions (docs/bugs/B011) */
    volatile uint8_t marcstate; /* the chip's state at the last look, for `rf status` */
    rf_capture_t cap;
    rf_decoder_t dec;
    zsrc_rf_t src;
    TaskHandle_t task;
    _Atomic uint32_t requests; /* NB_* from the console, taken by the task */
    bool started;
    bool capturing;
    bool raw;
    uint8_t mode;       /* rf_module_t: what the owner allows */
    rx_kind_t kind;     /* what is receiving now */
    uint32_t freq_khz;
    rf_packet_t last;       /* the last confirmed transmission's packet */
    bool have_last;
    uint64_t last_at;       /* alarm ms */
    int64_t last_pulse_us;  /* esp time of the last pulse */
    int64_t last_edge_us;   /* ... of any edge, static included (a module's heartbeat) */
    int8_t rssi;
    /* the current window (edge rate, duty) and the last finished one */
    int64_t window_at;
    uint32_t window_edges;
    uint64_t window_high_us;
    uint64_t window_total_us;
    uint32_t edges_per_s;
    uint8_t duty_pct;
    uint32_t last_received; /* S.cap.received when the window opened */
    int64_t storm_until;
    uint32_t storms;
    bool storm_ended;  /* a storm's pause ended within the current window */
    bool window_storm; /* the last finished window had a storm or part of its pause (RF-005) */
    /* `rf capture`: the console borrows the pulses for a while */
    volatile bool dumping;
    uint32_t *dump;
    volatile uint32_t dump_n;
    rf_service_pins_t pins; /* `rf pin`: where the data line may go (RF-008) */
} S;

#define NO_PIN_STORED 0xFFFFu /* NVS "rf"/"pin" absent: the board's own place */

static const char *kind_words(rx_kind_t k)
{
    return k == RX_CC1101 ? "CC1101" : k == RX_OOK ? "receiver module" : "none";
}

static const char *mode_words(uint8_t m)
{
    return m == RF_MODULE_CC1101 ? "cc1101"
           : m == RF_MODULE_OOK  ? "ook"
           : m == RF_MODULE_OFF  ? "off"
                                 : "auto";
}

/* The place the receiver runs at, for `rf` and `rf place`: "default" is the board's own. */
static const char *place_id(void)
{
    return S.cfg.place != NULL && S.cfg.place[0] != '\0' ? S.cfg.place : "default";
}

static uint64_t now_ms(void)
{
    return alarm_service_now();
}

static void receiver_present(bool on)
{
    zone_service_lock();
    zsrc_rf_receiver(&S.src, on, now_ms());
    zone_service_unlock();
    zone_service_wake();
}

/* "EV1527 A2B3C code 5 (A2B3C5, T 350 us, PT2262 shape)": the words `rf raw on` logs and the
 * Flasher's 433 MHz page reads. */
static void packet_words(const rf_packet_t *p, char *out, size_t len)
{
    char words[48];
    zsrc_rf_code_words(p->proto, p->device, p->data, words, sizeof(words));
    snprintf(out, len, "%s (%06" PRIX32 ", T %u us%s%s%s)", words, p->code, (unsigned)p->unit_us,
             (p->flags & RF_PKT_PT2262) != 0u ? ", PT2262 shape" : "",
             (p->flags & RF_PKT_BATTERY_LOW) != 0u ? ", battery low" : "",
             rf_address_is_common(p->proto, p->device) ? ", common factory address" : "");
}

static void deliver(const rf_packet_t *p)
{
    const zsrc_rf_packet_t zp = {.proto = p->proto, .bits = p->bits, .flags = p->flags,
                                 .data = p->data, .unit_us = p->unit_us, .device = p->device,
                                 .code = p->code, .extra = p->extra};
    const uint64_t now = now_ms();
    zone_service_lock();
    /* true once per transmission, when its frame arrives again (RF-009): a lone frame - noise,
     * or a corrupted copy, which EV1527 cannot tell from a real code - is not logged or shown */
    const bool confirmed = zsrc_rf_packet(&S.src, &zp, now);
    zone_service_unlock();
    zone_service_wake(); /* a bound code's zone reacts to its first frame */
    if (!confirmed) {
        return;
    }
    S.last = *p;
    S.have_last = true;
    S.last_at = now;
    if (S.raw) {
        char words[128];
        packet_words(p, words, sizeof(words));
        ESP_LOGI(TAG, "%s", words);
    }
}

static void drain(void)
{
    rf_capture_pulse_t pulse;
    while (rf_capture_next(&S.cap, &pulse)) {
        rf_packet_t packets[RF_DECODE_MAX_OUT];
        size_t got = 0;
        if (pulse.width_us == 0u) {
            got = rf_decoder_gap(&S.dec, packets);
        } else {
            S.last_pulse_us = esp_timer_get_time();
            S.last_edge_us = S.last_pulse_us;
            S.window_edges++;
            S.window_total_us += pulse.width_us;
            if (pulse.high) {
                S.window_high_us += pulse.width_us;
            }
            got = S.kind != RX_NONE ? rf_decoder_pulse(&S.dec, pulse.high, pulse.width_us, packets) : 0u;
        }
        if (S.dumping && S.dump != NULL && S.dump_n < DUMP_MAX) {
            S.dump[S.dump_n] = (pulse.high ? 0x80000000u : 0u) | (pulse.width_us & 0x7FFFFFFFu);
            S.dump_n = S.dump_n + 1u;
        }
        for (size_t i = 0; i < got; i++) {
            deliver(&packets[i]);
        }
    }
}

static bool capture_on(void)
{
    if (!S.capturing) {
        if (rf_capture_start(&S.cap, S.cfg.gdo0, S.cfg.invert, S.task) != ESP_OK) {
            return false;
        }
        S.capturing = true;
        S.last_received = 0; /* rf_capture_start() cleared its counters */
    }
    return true;
}

/* A CC1101 answers on SPI: tune it, then listen to its line. */
static bool try_cc1101(void)
{
    if (cc1101_probe(&S.chip) != ESP_OK) {
        return false;
    }
    if (cc1101_rx_ook(&S.chip, S.freq_khz) != ESP_OK) {
        ESP_LOGE(TAG, "the CC1101 would not tune: wiring, or a clone that differs");
        return false;
    }
    if (!capture_on()) {
        return false;
    }
    rf_decoder_init(&S.dec);
    S.marcstate = CC1101_MARC_RX; /* cc1101_rx_ook() saw it settle there */
    S.kind = RX_CC1101;
    receiver_present(true);
    return true;
}

static void lost(const char *why)
{
    ESP_LOGW(TAG, "the %s stopped answering (%s): asking again every %u s", kind_words(S.kind),
             why, (unsigned)(RF_SERVICE_PROBE_MS / 1000u));
    S.chip.present = false;
    S.marcstate = 0xFFu;
    S.kind = RX_NONE;
    receiver_present(false);
}

/* The whole table again, read back, calibrated, back in RX (cc1101_rx_ook()). */
static bool retune(const char *why)
{
    if (cc1101_rx_ook(&S.chip, S.freq_khz) != ESP_OK) {
        lost(why);
        return false;
    }
    S.marcstate = CC1101_MARC_RX;
    return true;
}

/* Once a second: the edge rate and the line's duty over the last window; a storm of edges
 * pauses the capture (a clock on the pin would otherwise keep its interrupt busy). true when a
 * window closed (the duty is new). */
static bool close_window(int64_t now)
{
    if (now - S.window_at < (int64_t)WINDOW_MS * 1000) {
        return false;
    }
    /* The input rate, symbols the ring had no room for included (RF12): a task that falls behind
     * must not hide the flood the guard is for. Over the window's real length; the counter wraps. */
    const uint32_t received = S.cap.received;
    const uint32_t edges =
        cc1101_edges_per_s(received, S.last_received, (uint64_t)(now - S.window_at));
    S.last_received = received;
    S.edges_per_s = edges;
    /* A level that holds makes no pulses (a reception ends after RF_CAPTURE_IDLE_US), so a
     * carrier holding a module's line high would read as 0 % high and never as jamming. The
     * time since the last pulse counts at the level the line holds now (RF-005). */
    if (S.capturing && !S.cap.paused) {
        const int64_t from = S.last_edge_us > S.window_at ? S.last_edge_us : S.window_at;
        if (now - from > (int64_t)RF_CAPTURE_IDLE_US) {
            const int64_t span = now - from;
            const uint64_t quiet =
                (uint64_t)(span < (int64_t)WINDOW_MS * 1000 ? span : (int64_t)WINDOW_MS * 1000);
            S.window_total_us += quiet;
            if (rf_capture_line_high(&S.cap)) {
                S.window_high_us += quiet;
            }
        }
    }
    S.duty_pct = S.window_total_us != 0u ? (uint8_t)(S.window_high_us * 100u / S.window_total_us)
                                         : 0u;
    S.window_at = now;
    S.window_edges = 0;
    S.window_high_us = 0;
    S.window_total_us = 0;
    if (edges > RF_CAPTURE_STORM_EDGES && S.capturing) {
        S.storms++;
        S.storm_until = now + (int64_t)STORM_PAUSE_MS * 1000;
        rf_capture_pause(&S.cap, true);
        ESP_LOGW(TAG, "%" PRIu32 " edges a second on GPIO %d: not a receiver's line, capture "
                      "paused %u s", edges, S.cfg.gdo0, (unsigned)(STORM_PAUSE_MS / 1000u));
    }
    /* The window saw the storm, or part of its pause: the receiver could not hear in it. */
    S.window_storm = S.storm_until != 0 || S.storm_ended;
    S.storm_ended = false;
    return true;
}

static void rf_task(void *arg)
{
    (void)arg;
    int64_t next_probe = 0;
    int64_t next_capture = 0;
    int64_t next_rssi = 0;
    int64_t last_rssi = 0;
    int64_t next_check = 0;
    int64_t next_recal = 0;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
        const uint32_t bits = atomic_exchange(&S.requests, 0u);
        const int64_t now = esp_timer_get_time();

        if (S.storm_until != 0 && now >= S.storm_until) {
            S.storm_until = 0;
            S.storm_ended = true; /* the window closing next was partly paused */
            rf_capture_pause(&S.cap, false);
        }
        drain();
        const bool window_closed = close_window(now);

        if (S.kind == RX_NONE) {
            /* The bus first: a CC1101 wins over a line that merely toggles. */
            if (S.mode != RF_MODULE_OOK && ((bits & NB_RETRY) != 0u || now >= next_probe)) {
                next_probe = now + (int64_t)RF_SERVICE_PROBE_MS * 1000;
                if (try_cc1101()) {
                    next_check = now + (int64_t)CHECK_EVERY_MS * 1000;
                    next_recal = now + (int64_t)RECAL_EVERY_MS * 1000;
                    continue;
                }
            }
            /* A capture that won't start (a pin or a feature this chip refuses) is tried again
             * with the next probe, not every poll: its error would fill the log ten times a
             * second. `rf retry` tries it at once. */
            if ((bits & NB_RETRY) != 0u) {
                next_capture = 0;
            }
            if (S.mode == RF_MODULE_CC1101 || now < next_capture) {
                continue;
            }
            if (!capture_on()) {
                next_capture = now + (int64_t)RF_SERVICE_PROBE_MS * 1000;
                continue;
            }
            if (S.storm_until == 0 && S.edges_per_s >= OOK_FOUND_EDGES) {
                rf_decoder_init(&S.dec);
                S.kind = RX_OOK;
                ESP_LOGI(TAG, "a receiver module drives GPIO %d (%" PRIu32 " edges/s)",
                         S.cfg.gdo0, S.edges_per_s);
                receiver_present(true);
            }
            continue;
        }

        if (S.kind == RX_OOK) {
            /* No edge for minutes is a module gone - unless a carrier holds its line high: that
             * module is there, and jammed (counted above). */
            if (now - S.last_edge_us > (int64_t)OOK_SILENT_MS * 1000 &&
                !rf_capture_line_high(&S.cap)) {
                lost("its line has been silent");
                continue;
            }
            /* One carrier sample per window (zsrc_rf_carrier(): a module's come every second),
             * not one per wake-up: a module's static wakes the task many times a second.
             * RF-005: a window with a storm in it counts as a carrier. The storm pause measures
             * no duty (it would read 0 %, "no carrier") while the flood leaves the receiver deaf
             * ten seconds in eleven; a flood that goes on storms again in the first window after
             * its pause, so it stays a carrier, and the windows after it ends are judged by
             * their duty again. Skipping those windows instead would carry the last sample's
             * judgement - made before the flood, "no carrier" - across the whole pause. */
            if (window_closed) {
                zone_service_lock();
                zsrc_rf_carrier(&S.src, S.window_storm || S.duty_pct >= OOK_CARRIER_DUTY,
                                now_ms());
                zone_service_unlock();
            }
            continue;
        }

        /* the CC1101 */
        if ((bits & NB_RETUNE) != 0u && !retune("retune failed")) {
            continue;
        }
        if (now >= next_rssi) {
            next_rssi = now + (int64_t)RSSI_EVERY_MS * 1000;
            if (now - S.last_pulse_us > (int64_t)RSSI_QUIET_MS * 1000 ||
                now - last_rssi >= (int64_t)RSSI_BUSY_MS * 1000) {
                int8_t rssi = INT8_MIN;
                const cc1101_rssi_result_t r = cc1101_read_rssi(&S.chip, &rssi);
                if (r == CC1101_RSSI_BUS_ERROR) {
                    lost("SPI");
                    continue;
                }
                /* RF-017, RF14: when no two reads agreed there is no value - not a noise level,
                 * not a lost chip: `rf status` keeps the last validated one and the next tick
                 * samples again (last_rssi stays). RF-005: the lowest of the reads still counts
                 * as a carrier when even it is at the jamming level - a jammer whose RSSI keeps
                 * moving between reads must not look quiet; below it, a lower bound proves
                 * nothing and the jamming count keeps its last sample. */
                if (r == CC1101_RSSI_STABLE) {
                    last_rssi = now;
                    S.rssi = rssi;
                    zone_service_lock();
                    zsrc_rf_noise(&S.src, rssi, now_ms());
                    zone_service_unlock();
                } else if (rssi >= ZSRC_RF_JAM_DBM) {
                    zone_service_lock();
                    zsrc_rf_noise(&S.src, rssi, now_ms());
                    zone_service_unlock();
                }
            }
        }
        if (now >= next_check) {
            next_check = now + (int64_t)CHECK_EVERY_MS * 1000;
            const uint8_t state = cc1101_marcstate(&S.chip);
            bool reset = false;
            if (state == 0xFFu || cc1101_check(&S.chip, &reset) != ESP_OK) {
                lost("SPI");
                continue;
            }
            S.marcstate = state;
            if (reset || now >= next_recal) {
                /* RF-017: a brown-out put it back at its reset values (GDO0 a 135 kHz clock), or
                 * the hour is up: the whole table again, read back, calibrated */
                if (reset) {
                    ESP_LOGW(TAG, "the CC1101 lost its settings (a reset or brown-out): configuring it again");
                }
                next_recal = now + (int64_t)RECAL_EVERY_MS * 1000;
                (void)retune("reconfigure");
            } else if (state != CC1101_MARC_RX) {
                ESP_LOGW(TAG, "receiver in state %s: back to RX", cc1101_state_name(state));
                (void)retune("RX");
            }
        }
    }
}

static uint32_t stored_u32(const char *key, uint32_t fallback, uint32_t lo, uint32_t hi)
{
    nvs_handle_t h;
    uint32_t v = fallback;
    if (nvs_open("rf", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u32(h, key, &v) != ESP_OK || v < lo || v > hi) {
            v = fallback;
        }
        nvs_close(h);
    }
    return v;
}

static esp_err_t store_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("rf", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, key, v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

void rf_service_pin_rules(const rf_service_pins_t *rules)
{
    S.pins = *rules;
}

int16_t rf_service_stored_pin(void)
{
    const uint32_t v = stored_u32("pin", NO_PIN_STORED, 0u, 63u);
    return v == NO_PIN_STORED ? (int16_t)-1 : (int16_t)v;
}

bool rf_service_stored_place(char *out, size_t len)
{
    nvs_handle_t h;
    bool found = false;
    if (out == NULL || len == 0) {
        return false;
    }
    out[0] = '\0';
    if (nvs_open("rf", NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        found = nvs_get_str(h, "place", out, &n) == ESP_OK && out[0] != '\0';
        nvs_close(h);
    }
    if (!found) {
        out[0] = '\0';
    }
    return found;
}

/* The custom place's pins (RF-025), in rf_service_custom_t's order: never reuse a key. */
static const char *const CUSTOM_KEYS[] = {"c_sck", "c_mosi", "c_miso", "c_cs", "c_gdo0", "c_data"};
#define CUSTOM_KEY_COUNT (sizeof(CUSTOM_KEYS) / sizeof(CUSTOM_KEYS[0]))

static bool stored_is_custom(void)
{
    char place[16];
    return rf_service_stored_place(place, sizeof(place)) && strcmp(place, "custom") == 0;
}

rf_custom_state_t rf_service_stored_custom(rf_service_custom_t *out)
{
    *out = (rf_service_custom_t){.sck = -1, .mosi = -1, .miso = -1, .cs = -1, .data = -1};
    if (!stored_is_custom()) {
        return RF_CUSTOM_NONE;
    }
    int16_t v[CUSTOM_KEY_COUNT];
    bool damaged = false;
    nvs_handle_t h;
    if (nvs_open("rf", NVS_READONLY, &h) != ESP_OK) {
        return RF_CUSTOM_DAMAGED;
    }
    for (size_t i = 0; i < CUSTOM_KEY_COUNT; i++) {
        uint8_t u = 0;
        const esp_err_t err = nvs_get_u8(h, CUSTOM_KEYS[i], &u);
        v[i] = err == ESP_OK ? (int16_t)u : (int16_t)-1;
        damaged = damaged || (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || u > 63u;
    }
    nvs_close(h);
    const bool spi = v[0] >= 0 && v[1] >= 0 && v[2] >= 0 && v[3] >= 0 && v[4] >= 0;
    const bool none = v[0] < 0 && v[1] < 0 && v[2] < 0 && v[3] < 0 && v[4] < 0;
    if (damaged || !((spi && v[5] < 0) || (none && v[5] >= 0))) {
        return RF_CUSTOM_DAMAGED;
    }
    out->cc1101 = spi;
    out->sck = v[0];
    out->mosi = v[1];
    out->miso = v[2];
    out->cs = v[3];
    out->data = spi ? v[4] : v[5];
    return RF_CUSTOM_STORED;
}

static esp_err_t erase_in(nvs_handle_t h, const char *key)
{
    const esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

/* Stored in this order (RF-025, ADR-048): the place erased, the mode "off" (older firmware opens
 * nothing), the pins, and place "custom" last: a reset part-way never mixes old and new pins. */
static esp_err_t store_custom(const rf_service_custom_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("rf", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = erase_in(h, "place");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "mode", (uint32_t)RF_MODULE_OFF);
    }
    const int16_t v[CUSTOM_KEY_COUNT] = {c->cc1101 ? c->sck : -1,  c->cc1101 ? c->mosi : -1,
                                         c->cc1101 ? c->miso : -1, c->cc1101 ? c->cs : -1,
                                         c->cc1101 ? c->data : -1, c->cc1101 ? -1 : c->data};
    for (size_t i = 0; i < CUSTOM_KEY_COUNT && err == ESP_OK; i++) {
        err = v[i] >= 0 ? nvs_set_u8(h, CUSTOM_KEYS[i], (uint8_t)v[i]) : erase_in(h, CUSTOM_KEYS[i]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "place", "custom");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* No "mode" stored: the module comes from the board's configuration. */
#define MODE_NONE UINT32_MAX

/* Back to a named or default place after a custom store failed part-way (RF-025): the custom
 * pins erased first, then the mode and place there were before. */
static esp_err_t restore_place(const char *place, uint32_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("rf", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < CUSTOM_KEY_COUNT && err == ESP_OK; i++) {
        err = erase_in(h, CUSTOM_KEYS[i]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = mode == MODE_NONE ? erase_in(h, "mode") : nvs_set_u32(h, "mode", mode);
    }
    if (err == ESP_OK && place[0] != '\0') {
        err = nvs_set_str(h, "place", place);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* The custom pins forgotten, once the place says something else (RF-025). */
static esp_err_t forget_custom(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("rf", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < CUSTOM_KEY_COUNT && err == ESP_OK; i++) {
        err = erase_in(h, CUSTOM_KEYS[i]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* RF-025, RF13: once the stored place says something else, the old custom pins are unused (only
 * place "custom" reads them), so forgetting them is a tidy-up of its own, after the place change
 * has been stored: its failure is logged, never the command's result (the next start already
 * follows the new place). It is tried again by the next `rf place <place>|default` and by
 * rf_service_start() - which the app calls only when the receiver's wiring is safe, so a later
 * start may not reach it; left over, the pins are unused either way. */
static void tidy_custom(void)
{
    const esp_err_t err = forget_custom();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "the old custom pins stay stored, unused (%s): the next `rf place`, or a "
                      "start that reaches the receiver, tries again", esp_err_to_name(err));
    }
}

/* Custom pins left under a place that is not "custom" (a tidy-up that failed, RF13). false
 * whenever the place cannot be read for certain: a custom layout's pins are never erased on a
 * guess. */
static bool custom_left_over(void)
{
    nvs_handle_t h;
    if (nvs_open("rf", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char place[16];
    size_t n = sizeof(place);
    const esp_err_t perr = nvs_get_str(h, "place", place, &n);
    const bool not_custom =
        perr == ESP_ERR_NVS_NOT_FOUND || (perr == ESP_OK && strcmp(place, "custom") != 0);
    bool any = false;
    for (size_t i = 0; i < CUSTOM_KEY_COUNT && not_custom && !any; i++) {
        uint8_t u = 0;
        any = nvs_get_u8(h, CUSTOM_KEYS[i], &u) != ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_close(h);
    return not_custom && any;
}

/* The stored custom layout's line, exactly "custom" and its key=value pairs (RF-025, the
 * Flasher reads it back), then why it is not used and the pin warning, this start. */
static void print_custom(void)
{
    rf_service_custom_t c;
    if (rf_service_stored_custom(&c) == RF_CUSTOM_STORED) {
        if (c.cc1101) {
            printf("custom sck=%d mosi=%d miso=%d cs=%d gdo0=%d\n", c.sck, c.mosi, c.miso, c.cs,
                   c.data);
        } else {
            printf("custom data=%d\n", c.data);
        }
    }
    if (S.pins.custom_why != NULL && S.pins.custom_why[0] != '\0') {
        printf("%s\n", S.pins.custom_why);
    }
    if (S.pins.warning != NULL && S.pins.warning[0] != '\0') {
        printf("%s\n", S.pins.warning);
    }
}

/* `rf place custom sck= mosi= miso= cs= gdo0=` or `rf place custom data=` (RF-025): the keys in
 * any order, each once; every reply starts "place custom from the next restart (`restart`)" or
 * "not custom: ". */
static int cmd_place_custom(int argc, char **argv)
{
    static const char *const CC1101_KEYS[] = {"sck", "mosi", "miso", "cs", "gdo0"};
    static const char *const MODULE_KEYS[] = {"data"};
    bool module = false;
    for (int i = 0; i < argc; i++) {
        module = module || strncmp(argv[i], "data=", 5) == 0;
    }
    int16_t v[5];
    char why[120] = "";
    const bool parsed = module ? board_custom_parse(argc, argv, MODULE_KEYS, 1, 1, v, why, sizeof(why))
                               : board_custom_parse(argc, argv, CC1101_KEYS, 5, 5, v, why, sizeof(why));
    if (!parsed) {
        printf("not custom: %s\n", why);
        return 1;
    }
    const rf_service_custom_t c = module ? (rf_service_custom_t){.cc1101 = false, .sck = -1,
                                                                  .mosi = -1, .miso = -1,
                                                                  .cs = -1, .data = v[0]}
                                         : (rf_service_custom_t){.cc1101 = true, .sck = v[0],
                                                                  .mosi = v[1], .miso = v[2],
                                                                  .cs = v[3], .data = v[4]};
    snprintf(why, sizeof(why), "this board has no pins for an add-on of your own");
    if (S.pins.custom_ok == NULL || !S.pins.custom_ok(&c, why, sizeof(why))) {
        printf("not custom: %s\n", why);
        return 1;
    }
    /* What it was set up for, put back if the store fails part-way: store_custom() erases the
     * place first (RF-025). */
    rf_service_custom_t before;
    const bool was_custom = rf_service_stored_custom(&before) == RF_CUSTOM_STORED;
    char place[16];
    if (was_custom || !rf_service_stored_place(place, sizeof(place))) {
        place[0] = '\0';
    }
    const uint32_t mode = stored_u32("mode", MODE_NONE, RF_MODULE_AUTO, RF_MODULE_OFF);
    const esp_err_t err = store_custom(&c);
    if (err != ESP_OK) {
        const esp_err_t back = was_custom ? store_custom(&before) : restore_place(place, mode);
        printf("not custom: could not store it (%s); %s\n", esp_err_to_name(err),
               back == ESP_OK ? "the receiver stays as it was"
                              : "nor put the earlier place back: `rf place` shows what is stored");
        return 1;
    }
    printf("place custom from the next restart (`restart`): a %s on your own pins; they are no "
           "longer offered as wired terminals\n", c.cc1101 ? "CC1101" : "receiver module");
    return 0;
}

/* `rf pin <gpio>|default`: stored, used from the next start (the zone terminals are handed out
 * once, at start, and the receiver's pin must be taken out of them first). */
static esp_err_t forget(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("rf", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

static int cmd_pin(int argc, char **argv)
{
    const int16_t own = S.pins.board_data;
    const char *where = S.pins.board_where != NULL ? S.pins.board_where : "the board's own place";
    const int16_t stored = rf_service_stored_pin();
    const int16_t next = stored >= 0 ? stored : own;
    if (S.started && strcmp(place_id(), "default") != 0) {
        printf("the receiver is at the %s place (`rf place`): its data line is GPIO %d there; "
               "`rf pin` applies to the board's own place only\n", place_id(), S.cfg.gdo0);
        if (argc == 2) {
            return 0;
        }
    }
    if (argc == 2) {
        if (S.started && S.cfg.gdo0 == own) {
            printf("data on GPIO %d (%s)\n", own, where);
        } else if (S.started) {
            printf("data on GPIO %d (set with `rf pin`; the board's own place is GPIO %d, %s)\n",
                   S.cfg.gdo0, own, where);
        } else {
            printf("the receiver is not running\n");
        }
        if (!S.started || next != S.cfg.gdo0) {
            printf("GPIO %d from the next restart\n", next);
        }
        printf("`rf pin <gpio>` moves it (GPIO %d on the %s, or a wired terminal: `terminals`), "
               "`rf pin default` puts it back\n", own, where);
        return 0;
    }
    if (strcmp(argv[2], "default") == 0) {
        const esp_err_t err = forget("pin");
        if (err != ESP_OK) {
            printf("could not store it (%s)\n", esp_err_to_name(err));
            return 1;
        }
        printf("data on GPIO %d (%s) from the next restart (`restart`)\n", own, where);
        return 0;
    }
    char *end = NULL;
    const long gpio = strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || gpio < 0 || gpio > 63) {
        printf("rf pin <gpio>|default\n");
        return 1;
    }
    char why[120] = "this firmware cannot move it";
    if (S.pins.pin_ok == NULL || !S.pins.pin_ok((int16_t)gpio, why, sizeof(why))) {
        printf("not GPIO %ld: %s\n", gpio, why);
        return 1;
    }
    const esp_err_t err = (int16_t)gpio == own ? forget("pin") : store_u32("pin", (uint32_t)gpio);
    if (err != ESP_OK) {
        printf("could not store it (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("data on GPIO %ld from the next restart (`restart`)%s\n", gpio,
           (int16_t)gpio == own ? "" : "; it is no longer offered as a wired terminal");
    return 0;
}

/* `rf place [<place>|default]` (RF-019): the whole receiver at another place the board offers,
 * stored and used from the next start. Lines the Flasher reads: "place <id>: <where>" for the
 * place in use, "place <id> from the next restart" when a different one is stored. */
static int cmd_place(int argc, char **argv)
{
    const char *own = S.pins.board_where != NULL ? S.pins.board_where : "the board's own place";
    char stored[16];
    const bool moved = rf_service_stored_place(stored, sizeof(stored));
    const char *next = moved ? stored : "default";
    if (argc == 2) {
        if (S.started) {
            printf("place %s: %s\n", place_id(),
                   S.cfg.where != NULL && strcmp(place_id(), "default") != 0 ? S.cfg.where : own);
        } else {
            printf("the receiver is not running\n");
        }
        if (!S.started || strcmp(next, place_id()) != 0) {
            printf("place %s from the next restart\n", next);
        }
        print_custom();
        printf("default: %s\n%s", own, S.pins.places != NULL ? S.pins.places : "");
        printf("`rf place <place>` moves the receiver there, `rf place default` puts it back\n");
        return 0;
    }
    if (strcmp(argv[2], "custom") == 0) {
        return cmd_place_custom(argc - 3, argv + 3);
    }
    if (strcmp(argv[2], "default") == 0) {
        const esp_err_t err = forget("place");
        if (err != ESP_OK) {
            printf("could not store it (%s)\n", esp_err_to_name(err));
            return 1;
        }
        /* RF-025: the place first; from a custom place the receiver stays off until `rf module`.
         * The old pins go next, a tidy-up of their own (RF13). */
        tidy_custom();
        printf("place default (%s) from the next restart (`restart`)\n", own);
        return 0;
    }
    char why[120] = "this board offers no other place";
    if (strlen(argv[2]) >= sizeof(stored) || S.pins.place_ok == NULL ||
        !S.pins.place_ok(argv[2], why, sizeof(why))) {
        printf("not %s: %s\n", argv[2], why);
        return 1;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("rf", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "place", argv[2]);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        printf("could not store it (%s)\n", esp_err_to_name(err));
        return 1;
    }
    tidy_custom(); /* RF-025: the place is first, the old pins go next (RF13) */
    printf("place %s from the next restart (`restart`); its pins are no longer offered as wired "
           "terminals\n", argv[2]);
    return 0;
}

esp_err_t rf_service_start(const rf_service_config_t *cfg)
{
    if (S.started) {
        return ESP_OK;
    }
    S.cfg = *cfg;
    if (!cfg->custom && custom_left_over()) {
        /* RF13: an earlier tidy-up failed; the place already says otherwise. Only a start that
         * gets here retries it (the app skips this call when the wiring isn't safe). */
        tidy_custom();
    }
    S.freq_khz = stored_u32("khz", cfg->freq_khz, CC1101_FREQ_MIN_KHZ, CC1101_FREQ_MAX_KHZ);
    /* RF-025: on custom pins the place decides the module; the stored mode stays "off". */
    S.mode = cfg->custom ? (uint8_t)cfg->module
                         : (uint8_t)stored_u32("mode", cfg->module, RF_MODULE_AUTO, RF_MODULE_OFF);
    S.rssi = INT8_MIN;
    const zsrc_sink_t sink = zone_service_sink();
    zsrc_rf_init(&S.src, &sink);
    const zone_hub_result_t r = zone_service_register(zsrc_rf_ops(), &S.src);
    if (r != ZONE_HUB_OK) {
        ESP_LOGE(TAG, "the zone hub refused the RF source (%s)", zone_service_result_name(r));
        return ESP_ERR_INVALID_STATE;
    }
    if (S.mode == RF_MODULE_OFF) {
        /* RF-020: registered (bindings to 433 MHz sensors stay, never usable), no pin, no task. */
        S.started = true;
        ESP_LOGI(TAG, "433 MHz receiver off (`rf module off`): no pin is touched");
        return ESP_OK;
    }
    if (S.mode != RF_MODULE_OOK) {
        const cc1101_pins_t pins = {.host = cfg->host,
                                    .mosi = cfg->mosi,
                                    .miso = cfg->miso,
                                    .sck = cfg->sck,
                                    .cs = cfg->cs};
        ESP_RETURN_ON_ERROR(cc1101_open(&S.chip, &pins), TAG, "SPI");
    }
    /* Internal stack: the SPI driver's polling transfers and the RMT ring are internal-RAM
     * matters and 4 KiB is cheap; NVS is written from here (`rf freq`, `rf module`). */
    if (xTaskCreatePinnedToCore(rf_task, "rf", TASK_STACK, NULL, TASK_PRIORITY, &S.task,
                                tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    S.started = true;
    ESP_LOGI(TAG, "433 MHz receiver service (%s, %s place): SPI %d/%d/%d cs %d, data on GPIO %d, "
             "%lu kHz", mode_words(S.mode), place_id(), cfg->mosi, cfg->miso, cfg->sck, cfg->cs,
             cfg->gdo0, (unsigned long)S.freq_khz);
    return ESP_OK;
}

void rf_service_status(rf_service_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = S.started;
    if (!S.started) {
        snprintf(out->words, sizeof(out->words), "433 MHz receiver: not started");
        return;
    }
    out->module = S.mode;
    out->receiver = kind_words(S.kind);
    out->present = S.kind != RX_NONE;
    out->version = S.chip.version;
    /* The state at the task's last look (at most CHECK_EVERY_MS old): only the rf task talks to
     * the chip (docs/bugs/B011). */
    out->marcstate = S.kind == RX_CC1101 ? S.marcstate : 0xFFu;
    out->state = S.kind == RX_CC1101 ? cc1101_state_name(out->marcstate) : "-";
    out->rssi_dbm = S.kind == RX_CC1101 ? S.rssi : INT8_MIN;
    out->freq_khz = S.freq_khz;
    out->spi_errors = S.chip.spi_errors;
    out->edges_per_s = S.edges_per_s;
    out->duty_pct = S.duty_pct;
    out->storms = S.storms;
    out->receptions = S.cap.receptions;
    out->symbols = S.cap.symbols;
    out->overruns = S.cap.overruns;
    out->pulses = S.dec.pulses;
    out->frames = S.dec.frames;
    out->rejected = S.dec.rejected;
    out->glitches = S.dec.glitches;
    zone_service_lock();
    out->transmissions = S.src.transmissions;
    out->confirmed = S.src.confirmed;
    out->unknown = S.src.unknown;
    out->bursts = S.src.bursts;
    out->replays = S.src.replays;
    out->learning = S.src.learning;
    out->jammed = zsrc_rf_jammed(&S.src);
    zone_service_unlock();
    out->have_last = S.have_last;
    out->last = S.last;
    out->last_at_ms = S.last_at;
    if (S.mode == RF_MODULE_OFF) {
        snprintf(out->words, sizeof(out->words), "433 MHz receiver: off (`rf module` turns it on)");
    } else if (S.kind == RX_NONE) {
        snprintf(out->words, sizeof(out->words),
                 S.mode == RF_MODULE_OOK ? "No receiver module drives GPIO %d yet"
                 : S.mode == RF_MODULE_CC1101 ? "No CC1101 answers on SPI (GPIO %d is its data)"
                                              : "No receiver yet: no CC1101, GPIO %d quiet",
                 S.cfg.gdo0);
    } else if (out->jammed) {
        snprintf(out->words, sizeof(out->words), "%s: jammed", kind_words(S.kind));
    } else if (S.kind == RX_OOK) {
        snprintf(out->words, sizeof(out->words), "Receiver module on GPIO %d, %" PRIu32
                 " edges/s, line %u%% high", S.cfg.gdo0, S.edges_per_s, (unsigned)S.duty_pct);
    } else {
        snprintf(out->words, sizeof(out->words), "CC1101 at %lu.%03lu MHz, %s, noise %d dBm",
                 (unsigned long)(S.freq_khz / 1000u), (unsigned long)(S.freq_khz % 1000u),
                 out->state, (int)S.rssi);
    }
}

static void request(uint32_t what)
{
    if (S.task != NULL) {
        atomic_fetch_or(&S.requests, what);
        xTaskNotifyGive(S.task);
    }
}

void rf_service_retry(void)
{
    request(NB_RETRY);
}

esp_err_t rf_service_set_frequency(uint32_t khz)
{
    if (!cc1101_band_ok(khz)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* RF11: stored first; only a stored frequency is tuned to, so a failed store changes nothing
     * (the receiver and the next start both stay on the old one). */
    const esp_err_t err = store_u32("khz", khz);
    if (err != ESP_OK) {
        return err;
    }
    S.freq_khz = khz;
    request(NB_RETUNE);
    return ESP_OK;
}

esp_err_t rf_service_set_module(rf_module_t module)
{
    if ((unsigned)module > RF_MODULE_OFF) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Stored; applies from the next start, since the SPI bus is only opened when a CC1101
     * may be there. RF-025: while the place is custom the mode stays "off" (older firmware opens
     * nothing); "off" takes the custom place away. */
    if (stored_is_custom()) {
        if (module != RF_MODULE_OFF) {
            return ESP_ERR_INVALID_STATE;
        }
        /* RF13: "off" first (a custom place keeps it already), then the place: a failure in
         * either leaves the receiver as it was (custom pins, off) and is the result. The old
         * pins go after the place, a tidy-up of their own that never changes the result. */
        esp_err_t err = store_u32("mode", (uint32_t)RF_MODULE_OFF);
        if (err == ESP_OK) {
            err = forget("place");
        }
        if (err != ESP_OK) {
            return err;
        }
        tidy_custom();
        return ESP_OK;
    }
    return store_u32("mode", (uint32_t)module);
}

void rf_service_raw(bool on)
{
    S.raw = on;
}

zsrc_rf_t *rf_service_source(void)
{
    return S.started ? &S.src : NULL;
}

/* ---- the console ---------------------------------------------------------------------------- */

static int cmd_rf(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "pin") == 0) {
        return cmd_pin(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "place") == 0) {
        return cmd_place(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "retry") == 0) {
        rf_service_retry();
        vTaskDelay(pdMS_TO_TICKS(300));
    } else if (argc >= 3 && strcmp(argv[1], "freq") == 0) {
        const uint32_t khz = (uint32_t)strtoul(argv[2], NULL, 10);
        const esp_err_t err = rf_service_set_frequency(khz);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("rf freq <kHz>: 300000-348000 or 387000-464000 (%s)\n", esp_err_to_name(err));
            return 1;
        }
        if (err != ESP_OK) {
            /* RF11: storage failed - nothing changed, say so (not the bands) */
            printf("could not store it (%s): the receiver stays at %lu.%03lu MHz\n",
                   esp_err_to_name(err), (unsigned long)(S.freq_khz / 1000u),
                   (unsigned long)(S.freq_khz % 1000u));
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    } else if (argc >= 3 && strcmp(argv[1], "module") == 0) {
        const rf_module_t m = strcmp(argv[2], "cc1101") == 0 ? RF_MODULE_CC1101
                              : strcmp(argv[2], "ook") == 0  ? RF_MODULE_OOK
                              : strcmp(argv[2], "auto") == 0 ? RF_MODULE_AUTO
                              : strcmp(argv[2], "off") == 0  ? RF_MODULE_OFF
                                                             : (rf_module_t)0xFF;
        const esp_err_t err = rf_service_set_module(m);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("not %s: the receiver is on your own pins (`rf place custom`): `rf place "
                   "default` first\n", argv[2]);
            return 1;
        }
        if (err != ESP_OK) {
            printf("rf module auto|cc1101|ook|off\n");
            return 1;
        }
        printf("receiver %s from the next restart (`restart`)\n", mode_words((uint8_t)m));
        return 0;
    } else if (argc >= 3 && strcmp(argv[1], "raw") == 0) {
        rf_service_raw(strcmp(argv[2], "on") == 0);
        printf("raw %s\n", S.raw ? "on: every transmission is logged" : "off");
        return 0;
    } else if (argc >= 3 && strcmp(argv[1], "capture") == 0) {
        uint32_t ms = (uint32_t)strtoul(argv[2], NULL, 10);
        if (ms == 0u || ms > 10000u) {
            printf("rf capture <ms>: 1-10000\n");
            return 1;
        }
        if (!S.capturing) {
            printf("the data pin is not being captured yet\n");
            return 1;
        }
        uint32_t *buf = alarm_mem_slow_calloc(DUMP_MAX, sizeof(uint32_t));
        if (buf == NULL) {
            printf("no memory\n");
            return 1;
        }
        S.dump = buf;
        S.dump_n = 0;
        S.dumping = true;
        printf("capturing %lu ms: trigger the sensor now\n", (unsigned long)ms);
        vTaskDelay(pdMS_TO_TICKS(ms));
        S.dumping = false;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS * 3)); /* the task is out of the buffer by now */
        const uint32_t n = S.dump_n;
        printf("pulses %lu (H high, L low, microseconds; 0 = the line went quiet)\n",
               (unsigned long)n);
        for (uint32_t i = 0; i < n; i++) {
            printf("%c%lu%s", (buf[i] & 0x80000000u) != 0u ? 'H' : 'L',
                   (unsigned long)(buf[i] & 0x7FFFFFFFu), (i % 16u == 15u || i + 1u == n) ? "\n" : " ");
        }
        S.dump = NULL;
        heap_caps_free(buf);
        return 0;
    } else if (argc >= 2 && strcmp(argv[1], "status") != 0) {
        printf("rf [status|retry|freq <kHz>|module auto|cc1101|ook|off|pin <gpio>|default|"
               "place <place>|default|custom <pins>|raw on|off|capture <ms>]\n");
        return 1;
    }
    rf_service_status_t st;
    rf_service_status(&st);
    if (!st.enabled) {
        print_custom(); /* RF-025: before the line that ends the reply */
    }
    printf("%s\n", st.words);
    if (st.enabled) {
        print_custom();
        printf("module=%s receiver=%s edges/s=%" PRIu32 " high=%u%% storms=%" PRIu32 " place=%s\n",
               mode_words(st.module), st.receiver, st.edges_per_s, (unsigned)st.duty_pct,
               st.storms, place_id());
        printf("cc1101 version=0x%02X state=%s(0x%02X) spi_errors=%" PRIu32 " verify_errors=%" PRIu32
               " resets=%" PRIu32 "\n", st.version, st.state, st.marcstate, st.spi_errors,
               S.chip.verify_errors, S.chip.resets);
        /* RF-017, RF14: RSSI samples whose reads never agreed (no value; a line of its own, the
         * lines above are read by the Flasher) */
        printf("rssi unstable=%" PRIu32 "\n", S.chip.rssi_unstable);
        printf("capture receptions=%" PRIu32 " symbols=%" PRIu32 " overruns=%" PRIu32 "\n",
               st.receptions, st.symbols, st.overruns);
        printf("decoder pulses=%" PRIu32 " frames=%" PRIu32 " rejected=%" PRIu32 " glitches=%" PRIu32
               "\n", st.pulses, st.frames, st.rejected, st.glitches);
        printf("source transmissions=%" PRIu32 " unknown=%" PRIu32 " bursts=%" PRIu32
               " learning=%u jammed=%u confirmed=%" PRIu32 " replays=%" PRIu32 "\n",
               st.transmissions, st.unknown, st.bursts, (unsigned)st.learning, (unsigned)st.jammed,
               st.confirmed, st.replays);
        if (st.have_last) {
            char words[48];
            zsrc_rf_code_words(st.last.proto, st.last.device, st.last.data, words, sizeof(words));
            printf("last %s (%06" PRIX32 ", T %u us) at %" PRIu64 " ms\n", words, st.last.code,
                   (unsigned)st.last.unit_us, st.last_at_ms);
        }
    }
    return 0;
}

esp_err_t rf_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "rf",
        .help = "433 MHz receiver: status, retry, freq <kHz>, module auto|cc1101|ook|off, "
                "pin <gpio>|default, place <place>|default|custom <key>=<gpio>..., raw on|off, "
                "capture <ms>",
        .hint = "[status|retry|freq|module|pin|place|raw|capture]",
        .func = cmd_rf};
    return esp_console_cmd_register(&cmd);
}

#else /* CONFIG_ALARM_ZSRC_RF */

esp_err_t rf_service_start(const rf_service_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void rf_service_status(rf_service_status_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->words, sizeof(out->words), "433 MHz receiver: not in this firmware");
}

void rf_service_retry(void) {}

esp_err_t rf_service_set_frequency(uint32_t khz)
{
    (void)khz;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rf_service_set_module(rf_module_t module)
{
    (void)module;
    return ESP_ERR_NOT_SUPPORTED;
}

void rf_service_raw(bool on)
{
    (void)on;
}

zsrc_rf_t *rf_service_source(void)
{
    return NULL;
}

esp_err_t rf_service_console_register(void)
{
    return ESP_OK;
}

void rf_service_pin_rules(const rf_service_pins_t *rules)
{
    (void)rules;
}

int16_t rf_service_stored_pin(void)
{
    return -1;
}

bool rf_service_stored_place(char *out, size_t len)
{
    if (out != NULL && len > 0) {
        out[0] = '\0';
    }
    return false;
}

rf_custom_state_t rf_service_stored_custom(rf_service_custom_t *out)
{
    *out = (rf_service_custom_t){.sck = -1, .mosi = -1, .miso = -1, .cs = -1, .data = -1};
    return RF_CUSTOM_NONE;
}

#endif /* CONFIG_ALARM_ZSRC_RF */
