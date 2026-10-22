/*
 * plink_io_svc.c - a keypad's own wired inputs as its master's zones (docs/features/11 LINK-055).
 *
 * On a keypad (with a screen or without one): its wired terminals, read and debounced as on an
 * alarm panel (zsrc_wired, every 10 ms on an esp_timer), their levels and change counts kept here
 * and sent to the master by the link task (IO_STATE) at each change, every PLINK_IO_EVERY_MS and
 * when a session comes up. Nothing here runs the alarm: the master's zones decide.
 *
 * On the master: each keypad's IO_STATE (from the link task) is copied and handed to the zone
 * source ZSRC_KEYPAD (zsrc_keypad) on the worker task under the zone lock, with the keypads'
 * supervision edges: the link task never waits for the zone lock (LINK-027).
 */
#include "sdkconfig.h"
#if CONFIG_ALARM_LINK

#include <stdatomic.h>
#include <string.h>

#include "alarm_service.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "plink_internal.h"
#include "zone_hub/zsrc_keypad.h"
#include "zone_hub/zsrc_wired.h"
#include "zone_service.h"

static const char *TAG = "plink_io";

/* ---- the keypad's side ----------------------------------------------------------------------- */

static zsrc_wired_terminal_t s_terms[PLINK_IO_MAX];
static size_t s_term_count;
static zsrc_wired_t s_wired;
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static plink_io_state_t s_state;  /* under s_mux */
static atomic_bool s_changed;     /* a level moved since the last send */
static uint64_t s_sent_at;        /* link task: the last IO_STATE (0: none this session) */
static bool s_was_ready;          /* link task: a session with PLINK_CAP_IO was up */

void plink_service_set_terminals(const uint8_t *gpios, size_t n)
{
    s_term_count = 0;
    for (size_t i = 0; i < n && s_term_count < PLINK_IO_MAX; i++) {
        s_terms[s_term_count].gpio = gpios[i];
        s_terms[s_term_count].where = "keypad";
        s_term_count++;
    }
}

static bool pin_setup(void *ctx, uint8_t gpio, bool on)
{
    (void)ctx;
    if (!on) {
        return gpio_reset_pin((gpio_num_t)gpio) == ESP_OK;
    }
    const gpio_config_t io = {.pin_bit_mask = 1ull << gpio,
                              .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE,
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
    return gpio_config(&io) == ESP_OK;
}

static uint8_t pin_read(void *ctx, uint8_t gpio)
{
    (void)ctx;
    return gpio_get_level((gpio_num_t)gpio) != 0 ? 1u : 0u;
}

/* The wired source's report: a terminal's debounced level (1 = the loop is open). */
static void level(void *ctx, const zsrc_address_t *a, const char *state, int32_t value,
                  uint64_t now)
{
    (void)ctx;
    (void)state;
    (void)now;
    bool moved = false;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < s_state.count; i++) {
        plink_io_rec_t *r = &s_state.rec[i];
        if (r->gpio != a->id) {
            continue;
        }
        const uint8_t flags = (uint8_t)(PLINK_IO_KNOWN | (value != 0 ? PLINK_IO_ACTIVE : 0u));
        if ((r->flags & PLINK_IO_KNOWN) != 0u && flags != r->flags) {
            r->changes++;
        }
        moved = flags != r->flags;
        r->flags = flags;
    }
    portEXIT_CRITICAL(&s_mux);
    if (moved) {
        atomic_store(&s_changed, true); /* the link task's next turn (50 ms at most) sends it */
    }
}

static void sample(void *arg)
{
    (void)arg;
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    zsrc_wired_ops()->tick(&s_wired, now);
}

uint16_t kio_prepare(void)
{
    if (s_term_count == 0u || s_timer != NULL) {
        return s_term_count != 0u ? PLINK_CAP_IO : 0u;
    }
    const zsrc_gpio_port_t gpio = {.setup = pin_setup, .read = pin_read};
    const zsrc_sink_t sink = {.input = level};
    zsrc_wired_init(&s_wired, s_terms, s_term_count, &gpio, &sink);
    s_state.count = (uint8_t)s_term_count;
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    zsrc_mapping_t m;
    memset(&m, 0, sizeof(m));
    for (size_t i = 0; i < s_term_count; i++) {
        s_state.rec[i].gpio = s_terms[i].gpio;
        const zsrc_address_t a = zsrc_wired_address(s_terms[i].gpio);
        if (!zsrc_wired_ops()->watch(&s_wired, &a, &m, now)) {
            ESP_LOGW(TAG, "terminal IO%u could not be read", (unsigned)s_terms[i].gpio);
        }
    }
    const esp_timer_create_args_t t = {.callback = sample, .name = "plink_io"};
    if (esp_timer_create(&t, &s_timer) != ESP_OK ||
        esp_timer_start_periodic(s_timer, ZSRC_WIRED_POLL_MS * 1000u) != ESP_OK) {
        ESP_LOGE(TAG, "the terminals are not read: none offered to the master");
        return 0u;
    }
    ESP_LOGI(TAG, "%u wired terminal(s) offered to the master as its zones' inputs",
             (unsigned)s_term_count);
    return PLINK_CAP_IO;
}

void kio_poll(plink_keypad_t *k, uint64_t now)
{
    if (k == NULL || s_timer == NULL) {
        return;
    }
    const bool ready = plink_keypad_io_ready(k);
    if (!ready) {
        s_was_ready = false;
        return;
    }
    const bool fresh = !s_was_ready; /* a new session: the whole state at once */
    const bool changed = atomic_load(&s_changed);
    if (!fresh && !changed && now - s_sent_at < PLINK_IO_EVERY_MS) {
        return;
    }
    plink_io_state_t st;
    portENTER_CRITICAL(&s_mux);
    st = s_state;
    portEXIT_CRITICAL(&s_mux);
    if (plink_keypad_io_state(k, now, &st)) {
        atomic_store(&s_changed, false);
        s_sent_at = now;
        s_was_ready = true;
    }
}

uint64_t kio_next(uint64_t now)
{
    if (s_timer == NULL || !s_was_ready) {
        return UINT64_MAX;
    }
    return atomic_load(&s_changed) ? now : s_sent_at + PLINK_IO_EVERY_MS;
}

void kio_print(void)
{
    if (s_timer == NULL) {
        printf("  wired inputs: none offered\n");
        return;
    }
    plink_io_state_t st;
    portENTER_CRITICAL(&s_mux);
    st = s_state;
    portEXIT_CRITICAL(&s_mux);
    printf("  wired inputs for the master:");
    for (size_t i = 0; i < st.count; i++) {
        const plink_io_rec_t *r = &st.rec[i];
        printf(" IO%u %s", (unsigned)r->gpio,
               (r->flags & PLINK_IO_KNOWN) == 0u   ? "?"
               : (r->flags & PLINK_IO_ACTIVE) != 0u ? "open"
                                                   : "closed");
    }
    printf("\n");
}

/* ---- the master's side ----------------------------------------------------------------------- */

static zsrc_keypad_t s_kp;
static bool s_kp_on;
static plink_io_state_t s_pending[PLINK_KEYPADS_MAX]; /* under s_mux */
static atomic_uint s_io_dirty;     /* bit d-1: keypad d's newest report waits */
static atomic_uint s_reach_up;     /* bit d-1: heard (as the last edge said) */
static atomic_uint s_reach_dirty;  /* bit d-1: an edge waits */
static atomic_uint s_removed;      /* bit d-1: removed, forget what it reported */

uint16_t miosvc_prepare(void)
{
    const zsrc_sink_t sink = zone_service_sink();
    zsrc_keypad_init(&s_kp, &sink);
    const zone_hub_result_t r = zone_service_register(zsrc_keypad_ops(), &s_kp);
    if (r != ZONE_HUB_OK) {
        ESP_LOGW(TAG, "keypad inputs not offered as zones (%s)", zone_service_result_name(r));
        return 0u;
    }
    s_kp_on = true;
    return PLINK_CAP_IO;
}

/* The link task: copy and hand over (never waits). */
void miosvc_io_state(void *ctx, uint8_t device, const plink_io_state_t *st)
{
    (void)ctx;
    if (!s_kp_on || device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_pending[device - 1u] = *st;
    portEXIT_CRITICAL(&s_mux);
    atomic_fetch_or(&s_io_dirty, 1u << (device - 1u));
    msvc_kick();
}

void miosvc_reachable(uint8_t device, bool up)
{
    if (!s_kp_on || device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST) {
        return;
    }
    const unsigned bit = 1u << (device - 1u);
    if (up) {
        atomic_fetch_or(&s_reach_up, bit);
    } else {
        atomic_fetch_and(&s_reach_up, ~bit);
    }
    atomic_fetch_or(&s_reach_dirty, bit);
}

void miosvc_removed(uint8_t device)
{
    if (s_kp_on && device >= PLINK_DEVICE_FIRST && device <= PLINK_DEVICE_LAST) {
        atomic_fetch_or(&s_removed, 1u << (device - 1u));
    }
}

/* The worker task: into the zone source under the zone lock. */
void miosvc_apply(void)
{
    if (!s_kp_on) {
        return;
    }
    const unsigned dirty = atomic_exchange(&s_io_dirty, 0u);
    const unsigned reach = atomic_exchange(&s_reach_dirty, 0u);
    const unsigned removed = atomic_exchange(&s_removed, 0u);
    if ((dirty | reach | removed) == 0u) {
        return;
    }
    const uint64_t now = alarm_service_now();
    zone_service_lock();
    for (uint8_t d = 0; d < PLINK_KEYPADS_MAX; d++) {
        const unsigned bit = 1u << d;
        if ((removed & bit) != 0u) {
            zsrc_keypad_forget(&s_kp, (uint8_t)(d + 1u), now);
            continue;
        }
        if ((reach & bit) != 0u && (atomic_load(&s_reach_up) & bit) == 0u) {
            zsrc_keypad_online(&s_kp, (uint8_t)(d + 1u), false, now);
        }
        if ((dirty & bit) != 0u) {
            plink_io_state_t st;
            portENTER_CRITICAL(&s_mux);
            st = s_pending[d];
            portEXIT_CRITICAL(&s_mux);
            zsrc_keypad_term_t terms[PLINK_IO_MAX];
            for (size_t i = 0; i < st.count; i++) {
                terms[i].gpio = st.rec[i].gpio;
                terms[i].flags = st.rec[i].flags;
                terms[i].changes = st.rec[i].changes;
            }
            zsrc_keypad_report(&s_kp, (uint8_t)(d + 1u), terms, st.count, now);
        }
    }
    zone_service_unlock();
    zone_service_wake();
}

#endif /* CONFIG_ALARM_LINK */
