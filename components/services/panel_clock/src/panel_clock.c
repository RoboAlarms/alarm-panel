/*
 * panel_clock.c - the battery-backed clock module (docs/features/35). See panel_clock.h.
 *
 * Start-up (on the start-up task, before the event log): one read of the whole register file;
 * a module that kept the time sets the system clock, the control and status registers are set
 * up for the coin cell. Then a task:
 *   - after each time-server sync (net_wifi_time_syncs) and after a person set the time, writes
 *     the module at the system clock's next whole second (writing the seconds register restarts
 *     the module's second, so the two tick together);
 *   - a minute after start-up or a write, then hourly, reads the module on its second tick and
 *     lets the keeper decide: nothing, slew the system clock, or put the module back.
 * The bus is shared with the touch controller on the CrowPanel's I2C connector: every
 * transaction is bounded (RTC_DS3231_TIMEOUT_MS) and the task stack is internal RAM (the I2C
 * interrupt reads the buffers, CONTRIBUTING.md "Internal RAM is the reserve"). `rtc place j7`
 * (RTC-013) moves the module to two J7 pins with a controller of its own; the app opens that
 * bus from the next start.
 */
#include "panel_clock.h"

#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_RTC

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "board_info/board_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "alarm_service.h"
#include "net_wifi.h"
#include "rtc_ds3231/rtc_ds3231.h"

static const char *TAG = "clock";

#define TASK_TICK_MS 1000u
#define TICK_POLL_MS 2u      /* reading the seconds register while waiting for its tick */
#define TICK_WAIT_MS 1100u
#define ALIGN_SLACK_US 30000 /* a write later than this into the second waits for the next */

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static clock_keeper_t s_keeper;
static const char *s_where = "";
static const char *s_place = "default"; /* the board place the bus is at (`rtc place`) */
static bool s_bus; /* the module was opened on the bus */
static panel_clock_place_rules_t s_rules;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static int64_t system_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void utc_text(int64_t epoch_s, char *buf, size_t len)
{
    rtc_civil_t c;
    rtc_civil_from_epoch(epoch_s, &c);
    snprintf(buf, len, "%04" PRId32 "-%02u-%02u %02u:%02u:%02u UTC", c.year, c.month, c.day,
             c.hour, c.minute, c.second);
}

static void apply(const clock_action_t *a, const char *why)
{
    if (a->kind == CLOCK_DO_SET) {
        const struct timeval tv = {.tv_sec = (time_t)(a->epoch_ms / 1000),
                                   .tv_usec = (suseconds_t)((a->epoch_ms % 1000) * 1000)};
        settimeofday(&tv, NULL);
        char t[32];
        utc_text(a->epoch_ms / 1000, t, sizeof(t));
        ESP_LOGI(TAG, "clock set to %s (%s)", t, why);
        net_wifi_clock_set(); /* clears "clock not set" if it was raised */
    } else if (a->kind == CLOCK_DO_SLEW) {
        const struct timeval d = {.tv_sec = (time_t)(a->delta_ms / 1000),
                                  .tv_usec = (suseconds_t)((a->delta_ms % 1000) * 1000)};
        if (adjtime(&d, NULL) != 0) {
            ESP_LOGW(TAG, "slewing by %" PRId64 " ms refused", a->delta_ms);
        } else {
            ESP_LOGI(TAG, "slewing the clock by %" PRId64 " ms to follow the module", a->delta_ms);
        }
    }
}

static bool read_all(uint8_t regs[DS3231_REG_COUNT])
{
    return s_bus && rtc_ds3231_read(DS3231_REG_SECONDS, regs, DS3231_REG_COUNT) == ESP_OK;
}

/* The control and status registers for a clock that lives on its coin cell (RTC-007). */
static bool set_up(const uint8_t regs[DS3231_REG_COUNT], bool clear_osf)
{
    const uint8_t cs[2] = {ds3231_control_wanted(regs[DS3231_REG_CONTROL]),
                           ds3231_status_wanted(regs[DS3231_REG_STATUS], clear_osf)};
    if (!clear_osf && cs[0] == regs[DS3231_REG_CONTROL] && cs[1] == regs[DS3231_REG_STATUS]) {
        return true;
    }
    return rtc_ds3231_write(DS3231_REG_CONTROL, cs, sizeof(cs)) == ESP_OK;
}

/* ---- writing the module ------------------------------------------------------------------ */

static bool write_module(void)
{
    uint8_t regs[DS3231_REG_COUNT];
    if (!read_all(regs)) { /* its control/status, and proof it still answers */
        return false;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        const uint32_t wait_ms = (uint32_t)((1000000 - tv.tv_usec) / 1000) + 1u;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        gettimeofday(&tv, NULL);
        if (tv.tv_usec > ALIGN_SLACK_US) {
            continue; /* the task was held up: try the next second */
        }
        uint8_t t[DS3231_TIME_LEN];
        if (!ds3231_encode_time((int64_t)tv.tv_sec, t) ||
            !clock_keeper_epoch_ok((int64_t)tv.tv_sec)) {
            return false; /* the panel's own clock isn't one to copy */
        }
        if (rtc_ds3231_write(DS3231_REG_SECONDS, t, sizeof(t)) != ESP_OK) {
            return false;
        }
        char when[32];
        utc_text((int64_t)tv.tv_sec, when, sizeof(when));
        ESP_LOGI(TAG, "module set to %s", when);
        return set_up(regs, true); /* the time is good now: clear the oscillator-stop flag */
    }
    return false;
}

/* ---- comparing ----------------------------------------------------------------------------- */

/* Reads the module right after its seconds tick over; *at_ms is the system clock then. */
static bool read_on_tick(uint8_t regs[DS3231_REG_COUNT], int64_t *at_ms)
{
    uint8_t first;
    if (!s_bus || rtc_ds3231_read(DS3231_REG_SECONDS, &first, 1) != ESP_OK) {
        return false;
    }
    const uint64_t until = now_ms() + TICK_WAIT_MS;
    while (now_ms() < until) {
        vTaskDelay(pdMS_TO_TICKS(TICK_POLL_MS));
        uint8_t sec;
        if (rtc_ds3231_read(DS3231_REG_SECONDS, &sec, 1) != ESP_OK) {
            return false;
        }
        if (sec != first) {
            *at_ms = system_ms() - (int64_t)TICK_POLL_MS / 2;
            return read_all(regs);
        }
    }
    return false; /* a module whose seconds don't move has stopped */
}

static void compare(void)
{
    uint8_t regs[DS3231_REG_COUNT];
    int64_t at_ms = 0;
    const bool ok = read_on_tick(regs, &at_ms);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const clock_module_t before = s_keeper.module;
    const clock_action_t a = clock_keeper_compare(&s_keeper, ok, regs, at_ms, now_ms());
    const clock_module_t after = s_keeper.module;
    const int64_t offset = s_keeper.offset_known ? s_keeper.last_offset_ms : 0;
    const uint32_t jumps = s_keeper.jumps;
    xSemaphoreGive(s_lock);
    if (after != before) {
        ESP_LOGW(TAG, "clock module: %s", clock_module_name(after));
    }
    if (ok && (after == CLOCK_MODULE_OK || after == CLOCK_MODULE_LOST)) {
        (void)set_up(regs, false); /* someone may have changed its set-up */
    }
    static uint32_t s_jumps_seen;
    if (jumps != s_jumps_seen) {
        s_jumps_seen = jumps;
        ESP_LOGW(TAG, "the module was %" PRId64 " ms off the panel's clock: putting it back", offset);
    }
    apply(&a, "the module, read on its tick");
}

/* ---- the task ---------------------------------------------------------------------------- */

static void clock_task(void *arg)
{
    (void)arg;
    uint32_t syncs = net_wifi_time_syncs();
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TASK_TICK_MS));
        const uint32_t n = net_wifi_time_syncs();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (n != syncs) {
            syncs = n;
            clock_keeper_internet(&s_keeper, now_ms());
        }
        const bool write = clock_keeper_write_due(&s_keeper, now_ms());
        const bool check = !write && clock_keeper_check_due(&s_keeper, now_ms());
        xSemaphoreGive(s_lock);
        if (write) {
            const bool ok = write_module();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            clock_keeper_written(&s_keeper, ok, now_ms());
            xSemaphoreGive(s_lock);
            if (!ok) {
                ESP_LOGW(TAG, "the clock module could not be written; trying again in a minute");
            }
        } else if (check) {
            compare();
        }
    }
}

void panel_clock_place_rules(const panel_clock_place_rules_t *rules)
{
    s_rules = *rules;
}

bool panel_clock_stored_place(char *out, size_t len)
{
    nvs_handle_t h;
    bool found = false;
    if (out == NULL || len == 0) {
        return false;
    }
    out[0] = '\0';
    if (nvs_open("clock", NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        found = nvs_get_str(h, "place", out, &n) == ESP_OK && out[0] != '\0';
        nvs_close(h);
    }
    if (!found) {
        out[0] = '\0';
    }
    return found;
}

/* The custom place's pins (RTC-016): never reuse a key. */
static const char *const CUSTOM_KEYS[] = {"c_sda", "c_scl"};

static bool stored_is_custom(void)
{
    char place[16];
    return panel_clock_stored_place(place, sizeof(place)) && strcmp(place, "custom") == 0;
}

panel_clock_custom_t panel_clock_stored_custom(int16_t *sda, int16_t *scl)
{
    *sda = -1;
    *scl = -1;
    if (!stored_is_custom()) {
        return PANEL_CLOCK_CUSTOM_NONE;
    }
    nvs_handle_t h;
    uint8_t v[2] = {0xFFu, 0xFFu};
    bool ok = nvs_open("clock", NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_u8(h, CUSTOM_KEYS[0], &v[0]) == ESP_OK &&
             nvs_get_u8(h, CUSTOM_KEYS[1], &v[1]) == ESP_OK && v[0] <= 63u && v[1] <= 63u;
        nvs_close(h);
    }
    if (!ok) {
        return PANEL_CLOCK_CUSTOM_DAMAGED;
    }
    *sda = (int16_t)v[0];
    *scl = (int16_t)v[1];
    return PANEL_CLOCK_CUSTOM_STORED;
}

static esp_err_t erase_in(nvs_handle_t h, const char *key)
{
    const esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

/* RTC-016, ADR-048: the place erased first, the pins next, place "custom" last. */
static esp_err_t store_custom(int16_t sda, int16_t scl)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("clock", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = erase_in(h, "place");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, CUSTOM_KEYS[0], (uint8_t)sda);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, CUSTOM_KEYS[1], (uint8_t)scl);
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

/* The stored custom layout's line (RTC-016, the Flasher reads it back), then why it is not
 * used this start. */
static void print_custom(void)
{
    int16_t sda;
    int16_t scl;
    if (panel_clock_stored_custom(&sda, &scl) == PANEL_CLOCK_CUSTOM_STORED) {
        printf("custom sda=%d scl=%d\n", sda, scl);
    }
    if (s_rules.custom_why != NULL && s_rules.custom_why[0] != '\0') {
        printf("%s\n", s_rules.custom_why);
    }
}

esp_err_t panel_clock_start(i2c_master_bus_handle_t bus, const char *where, const char *place)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_place = place != NULL && place[0] != '\0' ? place : "default";
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clock_keeper_init(&s_keeper);
    s_where = where != NULL ? where : "";
    if (bus == NULL) {
        return ESP_OK; /* no connector on this board: nothing to look for, no task */
    }
    uint8_t regs[DS3231_REG_COUNT];
    memset(regs, 0xFF, sizeof(regs));
    const esp_err_t err = rtc_ds3231_open(bus);
    s_bus = err == ESP_OK;
    const bool answered = read_all(regs);
    const clock_action_t a = clock_keeper_start(&s_keeper, answered, regs, now_ms());
    switch (s_keeper.module) {
    case CLOCK_MODULE_ABSENT:
        ESP_LOGI(TAG, "no clock module on the %s", s_where);
        return ESP_OK; /* the usual case: no task either */
    case CLOCK_MODULE_OTHER:
        ESP_LOGW(TAG, "something at 0x68 on the %s is not a DS3231: left alone", s_where);
        return ESP_OK;
    case CLOCK_MODULE_LOST:
        ESP_LOGW(TAG, "the clock module has lost the time (first use, or its coin cell is flat): "
                      "the time server or a person will set it");
        break;
    default:
        break;
    }
    (void)set_up(regs, false);
    apply(&a, "the clock module kept it");
    if (xTaskCreatePinnedToCore(clock_task, "clock", 3584, NULL, 3, &s_task, tskNO_AFFINITY) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void panel_clock_get(panel_clock_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->built = true;
    out->where = s_where;
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->module = s_keeper.module;
    out->source = s_keeper.source;
    out->internet_trusted = clock_keeper_internet_trusted(&s_keeper, now_ms());
    out->lost_at_start = s_keeper.lost_at_start;
    out->temp_known = s_keeper.temp_known;
    out->temp_c4 = s_keeper.temp_c4;
    out->offset_known = s_keeper.offset_known;
    out->offset_ms = s_keeper.last_offset_ms;
    out->writes = s_keeper.writes;
    out->write_failures = s_keeper.write_failures;
    out->slews = s_keeper.slews;
    out->jumps = s_keeper.jumps;
    xSemaphoreGive(s_lock);
    /* A panel whose clock the time server set before the module's own start-up was read (or
     * that has no module) still says where its time came from. */
    if (out->source == CLOCK_SRC_NONE) {
        net_wifi_status_t w;
        net_wifi_get_status(&w);
        if (w.time_synced) {
            out->source = CLOCK_SRC_INTERNET;
        }
    }
}

esp_err_t panel_clock_set(int64_t epoch_ms, const char *who)
{
    if (s_lock == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool can = clock_keeper_can_set(&s_keeper);
    const clock_action_t a = can ? clock_keeper_manual(&s_keeper, epoch_ms, now_ms())
                                 : (clock_action_t){CLOCK_DO_NOTHING, 0, 0};
    xSemaphoreGive(s_lock);
    if (!can) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (a.kind != CLOCK_DO_SET) {
        return ESP_ERR_INVALID_ARG;
    }
    char why[48];
    snprintf(why, sizeof(why), "by hand, %s", who != NULL ? who : "?");
    apply(&a, why);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task); /* the module next */
    }
    return ESP_OK;
}

/* ---- console ----------------------------------------------------------------------------- */

static const char *module_key(clock_module_t m)
{
    switch (m) {
    case CLOCK_MODULE_OK: return "ok";
    case CLOCK_MODULE_LOST: return "lost";
    case CLOCK_MODULE_FAILED: return "failed";
    case CLOCK_MODULE_OTHER: return "other";
    default: return "absent";
    }
}

static const char *source_key(clock_source_t s)
{
    switch (s) {
    case CLOCK_SRC_MODULE: return "module";
    case CLOCK_SRC_INTERNET: return "internet";
    case CLOCK_SRC_MANUAL: return "manual";
    default: return "none";
    }
}

/* "24.25 C", "-0.50 C" */
static void temp_text(int16_t c4, char *buf, size_t len)
{
    const int q = c4 < 0 ? -(int)c4 : (int)c4;
    snprintf(buf, len, "%s%d.%02d C", c4 < 0 ? "-" : "", q / 4, (q % 4) * 25);
}

/* Three lines; the Flasher reads the first two (tools/flasher/roboflash/clock_test.py): words
 * for people, then the facts as key=value. */
static void print_status(void)
{
    panel_clock_status_t st;
    panel_clock_get(&st);
    const int64_t now = system_ms();
    char temp[16] = "";
    if (st.temp_known) {
        temp_text(st.temp_c4, temp, sizeof(temp));
    }
    if (st.module == CLOCK_MODULE_ABSENT && st.where[0] == '\0') {
        printf("Clock module: none (this board has no I2C connector)\n");
    } else if (st.module == CLOCK_MODULE_ABSENT) {
        printf("Clock module: not fitted (none answered on the %s at start-up)\n", st.where);
    } else {
        printf("Clock module: %s%s%s\n", clock_module_name(st.module), temp[0] ? ", " : "", temp);
    }
    char offset[24] = "?";
    if (st.offset_known) {
        snprintf(offset, sizeof(offset), "%" PRId64, st.offset_ms);
    }
    print_custom(); /* RTC-016: before the facts line */
    printf("rtc module=%s source=%s trusted=%d lost_at_start=%d temp_c4=%d offset_ms=%s "
           "writes=%" PRIu32 " write_failures=%" PRIu32 " slews=%" PRIu32 " jumps=%" PRIu32
           " epoch=%" PRId64 " place=%s\n",
           module_key(st.module), source_key(st.source), st.internet_trusted ? 1 : 0,
           st.lost_at_start ? 1 : 0, st.temp_known ? st.temp_c4 : 0, offset, st.writes,
           st.write_failures, st.slews, st.jumps, now / 1000, s_place);
    char t[32];
    utc_text(now / 1000, t, sizeof(t));
    printf("panel clock %s (%s)\n", now >= CLOCK_EPOCH_MIN_S * 1000 ? t : "not set",
           clock_source_name(st.source));
}

/* `rtc place [<place>|default]` (RTC-013), stored and used from the next start. Lines the
 * Flasher reads: "place <id>: <where>" for the place in use, "place <id> from the next restart"
 * when a different one is stored. */
static int cmd_place(int argc, char **argv)
{
    const char *own = s_rules.own_where != NULL && s_rules.own_where[0] != '\0'
                          ? s_rules.own_where : "no connector";
    char stored[16];
    const char *next = panel_clock_stored_place(stored, sizeof(stored)) ? stored : "default";
    if (argc == 2) {
        printf("place %s: %s\n", s_place, strcmp(s_place, "default") == 0 ? own : s_where);
        if (strcmp(next, s_place) != 0) {
            printf("place %s from the next restart\n", next);
        }
        print_custom();
        printf("default: %s\n%s", own, s_rules.places != NULL ? s_rules.places : "");
        printf("`rtc place <place>` looks for the module there, `rtc place default` puts it "
               "back\n");
        return 0;
    }
    nvs_handle_t h;
    esp_err_t err;
    if (strcmp(argv[2], "custom") == 0) {
        /* `rtc place custom sda=<g> scl=<g>` (RTC-016): keys in any order, each once. */
        static const char *const KEYS[] = {"sda", "scl"};
        int16_t v[2];
        char why[120] = "";
        if (!board_custom_parse(argc - 3, argv + 3, KEYS, 2, 2, v, why, sizeof(why))) {
            printf("not custom: %s\n", why);
            return 1;
        }
        snprintf(why, sizeof(why), "this board has no pins for an add-on of your own");
        if (s_rules.custom_ok == NULL || !s_rules.custom_ok(v[0], v[1], why, sizeof(why))) {
            printf("not custom: %s\n", why);
            return 1;
        }
        err = store_custom(v[0], v[1]);
        if (err != ESP_OK) {
            printf("not custom: could not store it (%s)\n", esp_err_to_name(err));
            return 1;
        }
        printf("place custom from the next restart (`restart`): the module is looked for on your "
               "own pins, with an I2C controller of its own\n");
        return 0;
    }
    if (strcmp(argv[2], "default") == 0) {
        err = nvs_open("clock", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = erase_in(h, "place");
            for (size_t i = 0; i < 2 && err == ESP_OK; i++) {
                err = erase_in(h, CUSTOM_KEYS[i]); /* RTC-016: after the place */
            }
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err != ESP_OK) {
            printf("could not store it (%s)\n", esp_err_to_name(err));
            return 1;
        }
        printf("place default (%s) from the next restart (`restart`)\n", own);
        return 0;
    }
    char why[120] = "this board offers no other place";
    if (strlen(argv[2]) >= sizeof(stored) || s_rules.place_ok == NULL ||
        !s_rules.place_ok(argv[2], why, sizeof(why))) {
        printf("not %s: %s\n", argv[2], why);
        return 1;
    }
    err = nvs_open("clock", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "place", argv[2]);
        for (size_t i = 0; i < 2 && err == ESP_OK; i++) {
            err = erase_in(h, CUSTOM_KEYS[i]); /* RTC-016: after the place */
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        printf("could not store it (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("place %s from the next restart (`restart`)\n", argv[2]);
    return 0;
}

static int cmd_rtc(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "place") == 0) {
        return cmd_place(argc, argv);
    }
    if (strcmp(argv[1], "set") == 0 && argc == 3) {
        char *end = NULL;
        const long long s = strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            printf("rtc set <seconds since 1970, UTC>\n");
            return 1;
        }
        const esp_err_t err = panel_clock_set((int64_t)s * 1000, "the service console");
        if (err == ESP_ERR_NOT_SUPPORTED) {
            printf("refused: no clock module answers, so the time would be lost at the next "
                   "power cut\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("refused: that is not a time between 2025 and 2099\n");
            return 1;
        }
        /* RTC-014: logged as "Clock set" by the system (user 0). */
        (void)alarm_service_platform_event(ALARM_EV_CLOCK_SET, 0u, 0u, 0u, NULL, 0u);
        char t[32];
        utc_text((int64_t)s, t, sizeof(t));
        printf("clock set to %s; the module follows at the next second\n", t);
        return 0;
    }
    if (strcmp(argv[1], "check") == 0) {
        if (s_task == NULL) {
            printf("no clock module\n");
            return 1;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_keeper.next_check_ms = 0; /* the task compares at its next turn */
        xSemaphoreGive(s_lock);
        xTaskNotifyGive(s_task);
        printf("comparing with the module; `rtc` shows the result in a few seconds\n");
        return 0;
    }
    printf("rtc [status] | rtc set <epoch seconds> | rtc check | rtc place [<place>|default|"
           "custom sda=<gpio> scl=<gpio>]\n");
    return 1;
}

esp_err_t panel_clock_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "rtc",
        .help = "Battery-backed clock module: status, set <epoch seconds, UTC>, check, "
                "place <place>|default|custom sda=<gpio> scl=<gpio>",
        .hint = "[status|set|check|place]",
        .func = cmd_rtc,
    };
    return esp_console_cmd_register(&cmd);
}

#else /* CONFIG_ALARM_FEATURE_RTC */

#include <stdio.h>
#include <string.h>

#include "esp_console.h"

void panel_clock_place_rules(const panel_clock_place_rules_t *rules)
{
    (void)rules;
}

bool panel_clock_stored_place(char *out, size_t len)
{
    if (out != NULL && len > 0) {
        out[0] = '\0';
    }
    return false;
}

panel_clock_custom_t panel_clock_stored_custom(int16_t *sda, int16_t *scl)
{
    *sda = -1;
    *scl = -1;
    return PANEL_CLOCK_CUSTOM_NONE;
}

esp_err_t panel_clock_start(i2c_master_bus_handle_t bus, const char *where, const char *place)
{
    (void)bus;
    (void)where;
    (void)place;
    return ESP_OK;
}

void panel_clock_get(panel_clock_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->where = "";
}

esp_err_t panel_clock_set(int64_t epoch_ms, const char *who)
{
    (void)epoch_ms;
    (void)who;
    return ESP_ERR_NOT_SUPPORTED;
}

static int cmd_rtc(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Clock module: not in this firmware\n");
    return 1;
}

esp_err_t panel_clock_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "rtc", .help = "Battery-backed clock module (not in this firmware)",
        .hint = NULL, .func = cmd_rtc};
    return esp_console_cmd_register(&cmd);
}

#endif /* CONFIG_ALARM_FEATURE_RTC */
