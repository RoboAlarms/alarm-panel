/*
 * outputs_service.c - the wired outputs and the 433 MHz sirens (see outputs_service.h).
 */
#include "outputs_service.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_ALARM_OUTPUTS

#include "alarm_service.h"
#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rf_service.h"
#include "zone_service.h"

static const char *TAG = "outputs";

#define TASK_STACK    4096
/* Above the UI and the radios, below the alarm task: a temporal pattern's edges are the
 * standard's (NFPA 72), and the task does nothing long. */
#define TASK_PRIORITY 8
#define QUEUE_LEN     16
#define LOOK_MS       1000u /* the transmitter's presence, and the store, are looked at this often */

typedef struct {
    uint8_t kind;      /* 0 output, 1 armed */
    uint8_t partition;
    uint8_t out;       /* alarm_output_t */
    uint8_t pattern;   /* alarm_pattern_t, or armed 0/1 */
} msg_t;

static struct {
    outputs_service_config_t cfg;
    SemaphoreHandle_t lock;
    QueueHandle_t q;
    TaskHandle_t task;
    ow_t ow;
    rfs_t rf;
    bool started;
    _Atomic bool resync;            /* a message was lost: replay the latest of everything */
    uint8_t latest[ALARM_MAX_PARTITIONS + 1u][3]; /* the latest pattern of each output */
    uint8_t armed_bits;             /* from the snapshots, the alarm task's alone */
    bool armed_known;
    portMUX_TYPE mux;
} S = {.mux = portMUX_INITIALIZER_UNLOCKED};

static uint64_t now_ms(void)
{
    return alarm_service_now();
}

static void lock(void)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(S.lock);
    if (S.task != NULL) {
        xTaskNotifyGive(S.task); /* deadlines may have moved */
    }
}

/* ---- the pins ------------------------------------------------------------------------------- */

static bool is_terminal(uint8_t gpio)
{
    for (size_t i = 0; i < S.cfg.terminal_count; i++) {
        if (S.cfg.terminals[i] == gpio) {
            return true;
        }
    }
    return false;
}

static bool pin_setup(void *ctx, uint8_t gpio, bool output, bool level)
{
    (void)ctx;
    if (!is_terminal(gpio) || !zone_service_reserve_terminal(gpio, true)) {
        return false;
    }
    if (output) {
        /* the level first, so the pin never shows the other one (OUT-006) */
        gpio_set_level((gpio_num_t)gpio, level ? 1 : 0);
    }
    const gpio_config_t io = {.pin_bit_mask = 1ull << gpio,
                              .mode = output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
                              .pull_up_en = output ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
    if (gpio_config(&io) != ESP_OK) {
        (void)zone_service_reserve_terminal(gpio, false);
        return false;
    }
    if (output) {
        gpio_set_level((gpio_num_t)gpio, level ? 1 : 0);
    }
    return true;
}

static void pin_release(void *ctx, uint8_t gpio)
{
    (void)ctx;
    gpio_reset_pin((gpio_num_t)gpio);
    (void)zone_service_reserve_terminal(gpio, false);
}

static void pin_write(void *ctx, uint8_t gpio, bool level)
{
    (void)ctx;
    gpio_set_level((gpio_num_t)gpio, level ? 1 : 0);
}

static uint8_t pin_read(void *ctx, uint8_t gpio)
{
    (void)ctx;
    return gpio_get_level((gpio_num_t)gpio) != 0 ? 1u : 0u;
}

static void wired_trouble(void *ctx, bool on)
{
    (void)ctx;
    alarm_service_bell_trouble(ALARM_BELL_OWNER_WIRED, on);
}

/* ---- the transmitter ------------------------------------------------------------------------- */

static bool rf_send(void *ctx, const rf_packet_t *p, uint16_t unit_us, uint8_t frames)
{
    (void)ctx;
    return rf_service_transmit(p, unit_us, frames);
}

static void rf_trouble(void *ctx, bool on)
{
    (void)ctx;
    alarm_service_bell_trouble(ALARM_BELL_OWNER_RF, on);
}

/* ---- the store ------------------------------------------------------------------------------- */

static void store_blob(const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(OW_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, buf, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not store %s (%s): tried again", key, esp_err_to_name(err));
    }
}

static bool load_blob(const char *key, uint8_t *buf, size_t *len)
{
    nvs_handle_t h;
    if (nvs_open(OW_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const esp_err_t err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err == ESP_OK;
}

/* Under the lock, from the task (internal stack: NVS writes). */
static void save_if_dirty(void)
{
    if (S.ow.dirty) {
        uint8_t buf[OW_BLOB_MAX > OW_AUTO_BLOB_MAX ? OW_BLOB_MAX : OW_AUTO_BLOB_MAX];
        size_t n = ow_encode(&S.ow, buf, sizeof(buf));
        if (n != 0u) {
            store_blob(OW_KEY, buf, n);
            n = ow_auto_encode(&S.ow, buf, sizeof(buf)); /* BL-224: a key of its own */
            if (n != 0u) {
                store_blob(OW_AUTO_KEY, buf, n);
                S.ow.dirty = false;
            }
        }
    }
    if (S.rf.dirty) {
        uint8_t buf[RFS_BLOB_MAX];
        const size_t n = rfs_encode(&S.rf, buf, sizeof(buf));
        if (n != 0u) {
            store_blob(RFS_KEY, buf, n);
            S.rf.dirty = false;
        }
    }
}

static void load(void)
{
    const uint64_t now = now_ms();
    uint8_t buf[OW_AUTO_BLOB_MAX > RFS_BLOB_MAX ? OW_AUTO_BLOB_MAX : RFS_BLOB_MAX];
    _Static_assert(OW_AUTO_BLOB_MAX >= OW_BLOB_MAX, "the load buffer holds outputs/wired");
    size_t len = sizeof(buf);
    ow_config_t oc[OW_CHANNELS];
    memset(oc, 0, sizeof(oc));
    bool any = false;
    if (load_blob(OW_KEY, buf, &len)) {
        any = true;
        if (!ow_decode(buf, len, oc)) {
            memset(oc, 0, sizeof(oc));
            ESP_LOGW(TAG, "the stored wired outputs are damaged: none started");
        }
    }
    len = sizeof(buf);
    if (load_blob(OW_AUTO_KEY, buf, &len)) { /* BL-224: the outputs for automations */
        any = true;
        if (!ow_auto_decode(buf, len, oc)) {
            ESP_LOGW(TAG, "the stored outputs for automations are damaged: none started");
        }
    }
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        if (oc[i].function != OW_FN_NONE) {
            const ow_result_t r = ow_set(&S.ow, now, i, &oc[i]);
            if (r != OW_OK) {
                ESP_LOGW(TAG, "output %u on GPIO %u not started (%d): a zone or another "
                              "output has its pin", (unsigned)i + 1u, oc[i].gpio, (int)r);
            }
        }
    }
    if (any) {
        S.ow.dirty = false; /* what was read is what is stored */
    }
    len = sizeof(buf);
    if (load_blob(RFS_KEY, buf, &len)) {
        rfs_config_t cfg[RFS_SIRENS];
        if (rfs_decode(buf, len, cfg)) {
            for (size_t i = 0; i < RFS_SIRENS; i++) {
                if (cfg[i].used) {
                    (void)rfs_set(&S.rf, now, i, &cfg[i]);
                }
            }
        } else {
            ESP_LOGW(TAG, "the stored 433 MHz sirens are damaged: none started");
        }
        S.rf.dirty = false;
    }
}

/* ---- the task -------------------------------------------------------------------------------- */

static void apply(const msg_t *m, uint64_t now)
{
    if (m->kind == 1u) {
        const bool armed = m->pattern != 0u;
        if (m->out != 0u) { /* first view: no chirp */
            ow_set_armed(&S.ow, now, m->partition, armed);
            rfs_set_armed(&S.rf, m->partition, armed);
        } else {
            ow_armed(&S.ow, now, m->partition, armed);
            rfs_armed(&S.rf, now, m->partition, armed);
        }
        return;
    }
    ow_output(&S.ow, now, m->partition, (alarm_output_t)m->out, (alarm_pattern_t)m->pattern);
    rfs_output(&S.rf, now, m->partition, (alarm_output_t)m->out, (alarm_pattern_t)m->pattern);
}

static void outputs_task(void *arg)
{
    (void)arg;
    uint64_t next_look = 0u;
    for (;;) {
        const uint64_t now = now_ms();
        lock();
        uint64_t next = ow_next_deadline(&S.ow, now);
        const uint64_t rnext = rfs_next_deadline(&S.rf, now);
        xSemaphoreGive(S.lock);
        if (rnext < next) {
            next = rnext;
        }
        if (next_look < next) {
            next = next_look;
        }
        const uint64_t wait_ms = next > now ? next - now : 0u;
        msg_t m;
        const bool got = xQueueReceive(S.q, &m, wait_ms > 0x7FFFFFFFu ? portMAX_DELAY
                                                                    : pdMS_TO_TICKS(wait_ms)) == pdTRUE;
        (void)ulTaskNotifyTake(pdTRUE, 0);
        const uint64_t t = now_ms();
        lock();
        if (got) {
            apply(&m, t);
            while (xQueueReceive(S.q, &m, 0) == pdTRUE) {
                apply(&m, t);
            }
        }
        if (atomic_exchange(&S.resync, false)) {
            uint8_t latest[ALARM_MAX_PARTITIONS + 1u][3];
            portENTER_CRITICAL(&S.mux);
            memcpy(latest, S.latest, sizeof(latest));
            portEXIT_CRITICAL(&S.mux);
            for (uint8_t p = 0; p <= ALARM_MAX_PARTITIONS; p++) {
                for (uint8_t o = 0; o < 3u; o++) {
                    if ((p == 0u) == (o == (uint8_t)ALARM_OUT_SMOKE_RESET)) {
                        ow_output(&S.ow, t, p, (alarm_output_t)o, (alarm_pattern_t)latest[p][o]);
                        rfs_output(&S.rf, t, p, (alarm_output_t)o, (alarm_pattern_t)latest[p][o]);
                    }
                }
            }
        }
        ow_poll(&S.ow, t);
        if (t >= next_look) {
            next_look = t + LOOK_MS;
            const bool tx = rf_service_can_transmit();
            if (tx != S.rf.transmitter) {
                rfs_transmitter(&S.rf, t, tx);
            }
            save_if_dirty();
        }
        rfs_poll(&S.rf, t);
        xSemaphoreGive(S.lock);
    }
}

/* ---- the engine's news (alarm task) ---------------------------------------------------------- */

static void post(const msg_t *m)
{
    if (S.q == NULL || xQueueSend(S.q, m, 0) != pdTRUE) {
        atomic_store(&S.resync, true);
        if (S.task != NULL) {
            xTaskNotifyGive(S.task);
        }
    }
}

void outputs_service_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    if (partition > ALARM_MAX_PARTITIONS || (unsigned)output > (unsigned)ALARM_OUT_SMOKE_RESET) {
        return;
    }
    if (pattern != ALARM_PAT_TEST) { /* a one-shot is never replayed */
        portENTER_CRITICAL(&S.mux);
        S.latest[partition][output] = (uint8_t)pattern;
        portEXIT_CRITICAL(&S.mux);
    }
    const msg_t m = {.kind = 0u, .partition = partition, .out = (uint8_t)output,
                     .pattern = (uint8_t)pattern};
    post(&m);
}

void outputs_service_engine_snapshot(const alarm_snapshot_t *snap)
{
    uint8_t bits = 0u;
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if ((snap->partitions & (1u << i)) != 0u && snap->part[i].state != ALARM_STATE_DISARMED) {
            bits = (uint8_t)(bits | (1u << i));
        }
    }
    if (S.armed_known && bits == S.armed_bits) {
        return;
    }
    const bool first = !S.armed_known;
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        const uint8_t bit = (uint8_t)(1u << i);
        if (first || ((bits ^ S.armed_bits) & bit) != 0u) {
            const msg_t m = {.kind = 1u, .partition = (uint8_t)(i + 1u), .out = first ? 1u : 0u,
                             .pattern = (bits & bit) != 0u ? 1u : 0u};
            post(&m);
        }
    }
    S.armed_bits = bits;
    S.armed_known = true;
}

/* ---- the pages' and the console's calls ------------------------------------------------------ */

void outputs_service_wired(outputs_wired_info_t out[OW_CHANNELS])
{
    memset(out, 0, sizeof(outputs_wired_info_t) * OW_CHANNELS);
    if (!S.started) {
        return;
    }
    lock();
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        out[i].c = S.ow.ch[i].c;
        out[i].on = S.ow.ch[i].on;
        out[i].trouble = S.ow.ch[i].trouble;
    }
    unlock();
}

static esp_err_t ow_err(ow_result_t r)
{
    switch (r) {
    case OW_OK: return ESP_OK;
    case OW_ERR_IN_USE: return ESP_ERR_INVALID_STATE;
    case OW_ERR_PIN: return ESP_FAIL;
    case OW_ERR_ARMED:
    case OW_ERR_BUSY: return ESP_ERR_INVALID_STATE;
    case OW_ERR_ARG:
    default: return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t outputs_service_wired_set(size_t slot, const ow_config_t *c)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (c->function != OW_FN_NONE &&
        (!is_terminal(c->gpio) || (c->sense != OW_SENSE_NONE && !is_terminal(c->sense)))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (c->function != OW_FN_NONE &&
        (zone_service_terminal_in_use(c->gpio) ||
         (c->sense != OW_SENSE_NONE && zone_service_terminal_in_use(c->sense)))) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const ow_result_t r = ow_set(&S.ow, now_ms(), slot, c);
    unlock();
    return ow_err(r);
}

esp_err_t outputs_service_wired_clear(size_t slot)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const ow_result_t r = ow_clear(&S.ow, slot);
    unlock();
    return ow_err(r);
}

esp_err_t outputs_service_wired_test(size_t slot)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const ow_result_t r = ow_test(&S.ow, now_ms(), slot);
    unlock();
    return ow_err(r);
}

size_t outputs_service_auto_list(outputs_auto_t *out, size_t max)
{
    size_t n = 0u;
    if (!S.started) {
        return 0u;
    }
    lock();
    for (size_t i = 0; i < OW_CHANNELS && n < max; i++) {
        const ow_channel_t *ch = &S.ow.ch[i];
        if (ch->c.function == OW_FN_AUTOMATION && ch->pin_ok) {
            out[n].number = (uint8_t)(i + 1u);
            out[n].gpio = ch->c.gpio;
            snprintf(out[n].name, sizeof(out[n].name), "%s", ch->c.name);
            n++;
        }
    }
    xSemaphoreGive(S.lock); /* nothing moved: no need to wake the task */
    return n;
}

uint8_t outputs_service_auto_mask(void)
{
    if (!S.started) {
        return 0u;
    }
    lock();
    const uint8_t mask = ow_auto_mask(&S.ow);
    xSemaphoreGive(S.lock);
    return mask;
}

bool outputs_service_auto_set(uint8_t number, bool on)
{
    if (!S.started || number < 1u || number > OW_CHANNELS) {
        return false;
    }
    lock();
    const ow_result_t r = ow_auto_set(&S.ow, now_ms(), (size_t)(number - 1u), on);
    unlock();
    return r == OW_OK;
}

size_t outputs_service_free_terminals(size_t slot, uint8_t *out, size_t max)
{
    size_t n = 0u;
    if (!S.started) {
        return 0u;
    }
    lock();
    for (size_t i = 0; i < S.cfg.terminal_count && n < max; i++) {
        const uint8_t g = S.cfg.terminals[i];
        const int owner = ow_find_pin(&S.ow, g);
        if ((owner < 0 || (size_t)owner == slot) && !zone_service_terminal_in_use(g)) {
            out[n++] = g;
        }
    }
    unlock();
    return n;
}

void outputs_service_rf(outputs_rf_info_t out[RFS_SIRENS])
{
    memset(out, 0, sizeof(outputs_rf_info_t) * RFS_SIRENS);
    if (!S.started) {
        return;
    }
    lock();
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        out[i].c = S.rf.s[i].c;
        out[i].sounding = S.rf.s[i].sound != SIREN_QUIET;
        out[i].sent = S.rf.s[i].sent;
        out[i].transmitter = S.rf.transmitter;
    }
    unlock();
}

bool outputs_service_rf_transmitter(void)
{
    return rf_service_can_transmit();
}

static esp_err_t rfs_err(rfs_result_t r)
{
    switch (r) {
    case RFS_OK: return ESP_OK;
    case RFS_ERR_UNKNOWN: return ESP_ERR_NOT_FOUND;
    case RFS_ERR_SEND: return ESP_ERR_NOT_FOUND;
    case RFS_ERR_ARMED:
    case RFS_ERR_BUSY:
    case RFS_ERR_IN_USE: return ESP_ERR_INVALID_STATE;
    case RFS_ERR_ARG:
    default: return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t outputs_service_rf_add(size_t *slot)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const int free_slot = rfs_free_slot(&S.rf);
    rfs_result_t r = RFS_ERR_ARG;
    if (free_slot >= 0) {
        uint32_t avoid[RFS_SIRENS];
        size_t n = 0u;
        for (size_t i = 0; i < RFS_SIRENS; i++) {
            if (S.rf.s[i].c.used) {
                avoid[n++] = S.rf.s[i].c.address;
            }
        }
        rfs_config_t c;
        rfs_defaults(&c, rfs_pick_address(esp_random(), avoid, n));
        r = rfs_set(&S.rf, now_ms(), (size_t)free_slot, &c);
        *slot = (size_t)free_slot;
    }
    unlock();
    return free_slot < 0 ? ESP_ERR_NO_MEM : rfs_err(r);
}

esp_err_t outputs_service_rf_set(size_t slot, const rfs_config_t *c)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const rfs_result_t r = rfs_set(&S.rf, now_ms(), slot, c);
    unlock();
    return rfs_err(r);
}

esp_err_t outputs_service_rf_clear(size_t slot)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const rfs_result_t r = rfs_clear(&S.rf, now_ms(), slot);
    unlock();
    return rfs_err(r);
}

esp_err_t outputs_service_rf_pair(size_t slot)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const rfs_result_t r = rfs_pair(&S.rf, now_ms(), slot);
    unlock();
    return rfs_err(r);
}

esp_err_t outputs_service_rf_test(size_t slot)
{
    if (!S.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const rfs_result_t r = rfs_test(&S.rf, now_ms(), slot);
    unlock();
    return rfs_err(r);
}

esp_err_t outputs_service_start(const outputs_service_config_t *cfg)
{
    if (S.started) {
        return ESP_OK;
    }
    S.cfg = *cfg;
    S.lock = xSemaphoreCreateMutex();
    S.q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    if (S.lock == NULL || S.q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const ow_port_t wp = {.setup = pin_setup, .release = pin_release, .write = pin_write,
                          .read = pin_read, .bell_trouble = wired_trouble};
    ow_init(&S.ow, &wp);
    const rfs_port_t rp = {.send = rf_send, .bell_trouble = rf_trouble};
    rfs_init(&S.rf, &rp);
    load(); /* the pins go to their inactive level here, before the engine says anything */
    S.started = true;
    if (xTaskCreatePinnedToCore(outputs_task, "outputs", TASK_STACK, NULL, TASK_PRIORITY, &S.task,
                                tskNO_AFFINITY) != pdPASS) {
        S.started = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "outputs: %u terminals on the %s", (unsigned)cfg->terminal_count,
             cfg->where != NULL ? cfg->where : "board");
    return ESP_OK;
}

/* ---- the console ----------------------------------------------------------------------------- */

static uint8_t mask_of(const char *s)
{
    /* "1,3" -> bits 0 and 2 */
    uint8_t m = 0u;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p >= '1' && *p <= '8') {
            m = (uint8_t)(m | (1u << (unsigned)(*p - '1')));
        }
    }
    return m;
}

static void mask_words(uint8_t m, char *out, size_t len)
{
    size_t n = 0u;
    out[0] = '\0';
    for (unsigned i = 0; i < 8u && n + 3u < len; i++) {
        if ((m & (1u << i)) != 0u) {
            n += (size_t)snprintf(out + n, len - n, "%s%u", n != 0u ? "," : "", i + 1u);
        }
    }
    if (n == 0u) {
        snprintf(out, len, "-");
    }
}

static void print_all(void)
{
    outputs_wired_info_t w[OW_CHANNELS];
    outputs_service_wired(w);
    printf("wired outputs (terminals on the %s):\n", S.cfg.where != NULL ? S.cfg.where : "board");
    for (size_t i = 0; i < OW_CHANNELS; i++) {
        const ow_config_t *c = &w[i].c;
        if (c->function == OW_FN_NONE) {
            printf("  %u: free\n", (unsigned)i + 1u);
            continue;
        }
        if (c->function == OW_FN_AUTOMATION) {
            printf("  %u: automation \"%s\" on GPIO %u%s, %s\n", (unsigned)i + 1u, c->name, c->gpio,
                   c->active_low ? " (active low)" : "", w[i].on ? "ON" : "off");
            continue;
        }
        char parts[24];
        char also[24];
        mask_words(c->partitions, parts, sizeof(parts));
        mask_words(c->also, also, sizeof(also));
        printf("  %u: %s on GPIO %u%s, partitions %s, also %s%s%s%s%s, %s%s\n", (unsigned)i + 1u,
               ow_function_name(c->function), c->gpio, c->active_low ? " (active low)" : "", parts,
               also, c->burglary ? ", burglary" : "", c->fire ? ", fire" : "",
               c->squawk ? ", squawk" : "", c->sense != OW_SENSE_NONE ? ", supervised" : "",
               w[i].on ? "ON" : "off", w[i].trouble ? ", TROUBLE" : "");
    }
    outputs_rf_info_t r[RFS_SIRENS];
    outputs_service_rf(r);
    printf("433 MHz sirens (%s):\n", rf_service_can_transmit() ? "transmitter ready"
                                                               : "nothing can transmit");
    for (size_t i = 0; i < RFS_SIRENS; i++) {
        const rfs_config_t *c = &r[i].c;
        if (!c->used) {
            printf("  %u: free\n", (unsigned)i + 1u);
            continue;
        }
        char parts[24];
        char also[24];
        mask_words(c->partitions, parts, sizeof(parts));
        mask_words(c->also, also, sizeof(also));
        printf("  %u: EV1527 %05" PRIX32 " codes alarm %u stop %u arm %u disarm %u, partitions %s, "
               "also %s%s%s%s, %s, sent %" PRIu32 "\n", (unsigned)i + 1u, c->address,
               c->code_alarm, c->code_stop, c->code_arm, c->code_disarm, parts, also,
               c->burglary ? ", burglary" : "", c->fire ? ", fire" : "", c->squawk ? ", squawk" : "",
               r[i].sounding ? "SOUNDING" : "quiet", r[i].sent);
    }
}

static const char *err_words(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "done";
    case ESP_ERR_INVALID_STATE: return "refused: armed, sounding, or the pin is a zone's or another output's";
    case ESP_ERR_NOT_FOUND: return "refused: no such siren or output for automations, or nothing can transmit";
    case ESP_ERR_NO_MEM: return "refused: no free place";
    case ESP_FAIL: return "the pin could not be set up";
    default: return "refused: check the words";
    }
}

/* Options after the function: parts <m> also <m> burglary|fire|both squawk low sense <gpio> */
static void parse_scope(int argc, char **argv, int from, uint8_t *parts, uint8_t *also, bool *burg,
                        bool *fire, bool *squawk, bool *low, uint8_t *sense)
{
    for (int i = from; i < argc; i++) {
        if (strcmp(argv[i], "parts") == 0 && i + 1 < argc) {
            *parts = mask_of(argv[++i]);
        } else if (strcmp(argv[i], "also") == 0 && i + 1 < argc) {
            *also = mask_of(argv[++i]);
        } else if (strcmp(argv[i], "burglary") == 0) {
            *burg = true;
            *fire = false;
        } else if (strcmp(argv[i], "fire") == 0) {
            *burg = false;
            *fire = true;
        } else if (strcmp(argv[i], "both") == 0) {
            *burg = true;
            *fire = true;
        } else if (strcmp(argv[i], "squawk") == 0) {
            *squawk = true;
        } else if (strcmp(argv[i], "low") == 0 && low != NULL) {
            *low = true;
        } else if (strcmp(argv[i], "sense") == 0 && i + 1 < argc && sense != NULL) {
            *sense = (uint8_t)strtoul(argv[++i], NULL, 10);
        }
    }
}

static int cmd_out(int argc, char **argv)
{
    if (argc < 2) {
        print_all();
        return 0;
    }
    if (strcmp(argv[1], "wired") == 0 && argc >= 4) {
        const long n = strtol(argv[2], NULL, 10);
        if (n < 1 || n > (long)OW_CHANNELS) {
            printf("out wired <1-%u> ...\n", (unsigned)OW_CHANNELS);
            return 1;
        }
        const size_t slot = (size_t)(n - 1);
        esp_err_t err;
        if (strcmp(argv[3], "clear") == 0) {
            err = outputs_service_wired_clear(slot);
        } else if (strcmp(argv[3], "test") == 0) {
            err = outputs_service_wired_test(slot);
        } else if (strcmp(argv[3], "on") == 0 || strcmp(argv[3], "off") == 0) {
            err = outputs_service_auto_set((uint8_t)n, strcmp(argv[3], "on") == 0) ? ESP_OK
                                                                                 : ESP_ERR_NOT_FOUND;
        } else if (strcmp(argv[3], "auto") == 0) {
            if (argc < 5) {
                printf("out wired <n> auto <gpio> [low] [name <words>]\n");
                return 1;
            }
            ow_config_t c = {.function = OW_FN_AUTOMATION, .sense = OW_SENSE_NONE};
            c.gpio = (uint8_t)strtoul(argv[4], NULL, 10);
            for (int i = 5; i < argc; i++) {
                if (strcmp(argv[i], "low") == 0) {
                    c.active_low = true;
                } else if (strcmp(argv[i], "name") == 0) {
                    size_t len = 0u;
                    for (int k = i + 1; k < argc; k++) { /* the rest, as typed */
                        const int w = snprintf(c.name + len, sizeof(c.name) - len, "%s%s",
                                               k > i + 1 ? " " : "", argv[k]);
                        if (w < 0 || (size_t)w >= sizeof(c.name) - len) {
                            break; /* ow_set keeps what fits, whole characters */
                        }
                        len += (size_t)w;
                    }
                    break;
                }
            }
            err = outputs_service_wired_set(slot, &c);
        } else {
            ow_config_t c = {.sense = OW_SENSE_NONE, .partitions = 0x01u, .burglary = true,
                             .fire = true};
            c.function = strcmp(argv[3], "siren") == 0    ? OW_FN_SIREN
                         : strcmp(argv[3], "strobe") == 0 ? OW_FN_STROBE
                         : strcmp(argv[3], "smoke") == 0  ? OW_FN_SMOKE_RESET
                         : strcmp(argv[3], "armed") == 0  ? OW_FN_ARMED
                                                          : OW_FN_COUNT;
            if (c.function == OW_FN_COUNT || argc < 5) {
                printf("out wired <n> siren|strobe|smoke|armed <gpio> [parts 1,2] [also 3] "
                       "[burglary|fire|both] [squawk] [low] [sense <gpio>] | out wired <n> auto "
                       "<gpio> [low] [name <words>] | out wired <n> test|clear|on|off\n");
                return 1;
            }
            c.gpio = (uint8_t)strtoul(argv[4], NULL, 10);
            parse_scope(argc, argv, 5, &c.partitions, &c.also, &c.burglary, &c.fire, &c.squawk,
                        &c.active_low, &c.sense);
            err = outputs_service_wired_set(slot, &c);
        }
        printf("%s\n", err_words(err));
        if (err == ESP_OK) {
            print_all();
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "rf") == 0 && argc >= 3) {
        if (strcmp(argv[2], "add") == 0) {
            size_t slot = 0u;
            esp_err_t err = outputs_service_rf_add(&slot);
            if (err == ESP_OK && argc > 3) {
                outputs_rf_info_t r[RFS_SIRENS];
                outputs_service_rf(r);
                rfs_config_t c = r[slot].c;
                parse_scope(argc, argv, 3, &c.partitions, &c.also, &c.burglary, &c.fire,
                            &c.squawk, NULL, NULL);
                err = outputs_service_rf_set(slot, &c);
            }
            printf("%s%s\n", err_words(err),
                   err == ESP_OK ? ": put the siren in its learning mode, then `out rf <n> pair`" : "");
            print_all();
            return err == ESP_OK ? 0 : 1;
        }
        const long n = strtol(argv[2], NULL, 10);
        if (n < 1 || n > (long)RFS_SIRENS || argc < 4) {
            printf("out rf add [parts 1,2] [also 3] [burglary|fire|both] [squawk] | out rf <n> "
                   "pair|test|clear|codes <alarm> <stop> <arm> <disarm>|set [options]\n");
            return 1;
        }
        const size_t slot = (size_t)(n - 1);
        esp_err_t err;
        if (strcmp(argv[3], "pair") == 0) {
            err = outputs_service_rf_pair(slot);
        } else if (strcmp(argv[3], "test") == 0) {
            err = outputs_service_rf_test(slot);
        } else if (strcmp(argv[3], "clear") == 0) {
            err = outputs_service_rf_clear(slot);
        } else {
            outputs_rf_info_t r[RFS_SIRENS];
            outputs_service_rf(r);
            rfs_config_t c = r[slot].c;
            if (!c.used) {
                err = ESP_ERR_NOT_FOUND;
            } else if (strcmp(argv[3], "codes") == 0 && argc >= 8) {
                c.code_alarm = (uint8_t)strtoul(argv[4], NULL, 0);
                c.code_stop = (uint8_t)strtoul(argv[5], NULL, 0);
                c.code_arm = (uint8_t)strtoul(argv[6], NULL, 0);
                c.code_disarm = (uint8_t)strtoul(argv[7], NULL, 0);
                err = outputs_service_rf_set(slot, &c);
            } else if (strcmp(argv[3], "set") == 0) {
                c.squawk = false;
                parse_scope(argc, argv, 4, &c.partitions, &c.also, &c.burglary, &c.fire,
                            &c.squawk, NULL, NULL);
                err = outputs_service_rf_set(slot, &c);
            } else {
                err = ESP_ERR_INVALID_ARG;
            }
        }
        printf("%s\n", err_words(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("out | out wired <n> ... | out rf ...  (`out wired` or `out rf` alone for the words)\n");
    return 1;
}

esp_err_t outputs_service_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "out",
        .help = "Sirens and outputs: `out` lists them; `out wired <n> siren|strobe|smoke|armed "
                "<gpio> [parts 1,2] [also 3] [burglary|fire|both] [squawk] [low] [sense <gpio>]`, "
                "`out wired <n> auto <gpio> [low] [name <words>]` (for automations), "
                "`out wired <n> test|clear|on|off`; `out rf add [...]`, `out rf <n> pair|test|clear|set "
                "[...]|codes <alarm> <stop> <arm> <disarm>`",
        .hint = "[wired|rf]",
        .func = cmd_out};
    return esp_console_cmd_register(&cmd);
}

#else /* CONFIG_ALARM_OUTPUTS */

esp_err_t outputs_service_start(const outputs_service_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}
void outputs_service_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    (void)output;
    (void)partition;
    (void)pattern;
}
void outputs_service_engine_snapshot(const alarm_snapshot_t *snap)
{
    (void)snap;
}
void outputs_service_wired(outputs_wired_info_t out[OW_CHANNELS])
{
    memset(out, 0, sizeof(outputs_wired_info_t) * OW_CHANNELS);
}
esp_err_t outputs_service_wired_set(size_t slot, const ow_config_t *c)
{
    (void)slot;
    (void)c;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_wired_clear(size_t slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_wired_test(size_t slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}
size_t outputs_service_free_terminals(size_t slot, uint8_t *out, size_t max)
{
    (void)slot;
    (void)out;
    (void)max;
    return 0u;
}
size_t outputs_service_auto_list(outputs_auto_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0u;
}
uint8_t outputs_service_auto_mask(void)
{
    return 0u;
}
bool outputs_service_auto_set(uint8_t number, bool on)
{
    (void)number;
    (void)on;
    return false;
}
void outputs_service_rf(outputs_rf_info_t out[RFS_SIRENS])
{
    memset(out, 0, sizeof(outputs_rf_info_t) * RFS_SIRENS);
}
bool outputs_service_rf_transmitter(void)
{
    return false;
}
esp_err_t outputs_service_rf_add(size_t *slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_rf_set(size_t slot, const rfs_config_t *c)
{
    (void)slot;
    (void)c;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_rf_clear(size_t slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_rf_pair(size_t slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_rf_test(size_t slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t outputs_service_console_register(void)
{
    return ESP_OK;
}

#endif /* CONFIG_ALARM_OUTPUTS */
