/*
 * net_check.c - the connection check (docs/features/09 NET-012). See net_check.h for the steps.
 *
 * Every probe is a plain, short, read-only network operation: a ping to the router, one name
 * lookup, a TCP connection that is closed again, one SNTP question. The internet step goes to
 * a public resolver's address (no name lookup needed, so it tells DNS trouble apart from a
 * router that can't reach the internet); nothing about the panel is sent anywhere.
 */
#include "net_check.h"

#include "i18n/i18n.h"
#include "ui_backend/ui_check_words.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "net_wifi.h"
#include "ping/ping_sock.h"

static const char *TAG = "netchk";

/* The internet step tries these in turn: well-known public resolvers that answer on 443 and
 * need no name lookup of their own. Two of them, so one being blocked isn't mistaken for
 * "the internet is down". */
static const char *const INTERNET_TARGETS[] = {"1.1.1.1", "9.9.9.9"};
#define INTERNET_PORT 443

#define PROBE_NAME     "pool.ntp.org" /* looked up when the time server is an address */
#define LINK_WAIT_MS   12000u         /* the address settings were just changed: let it rejoin */
#define TCP_TIMEOUT_MS 4000u
#define NTP_TIMEOUT_MS 3000u
#define NTP_EPOCH_DELTA 2208988800u   /* 1900 -> 1970 */

static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_running;
static net_check_report_t s_report;
static net_check_service_fn s_service_fn;
static void *s_service_ctx;

/* ---- the report ------------------------------------------------------------------------- */

/* A step's result: its words (ui_check_words.h) and what goes in their {0} and {1}. The
 * console and the Panel Manager put them together in English, the touchscreen in its language
 * (docs/features/37); the line itself isn't kept, which halves the report. */
static void step_set(net_check_step_t step, net_check_state_t state, uint32_t ms,
                     const char *words, const char *v0, const char *v1)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    net_check_result_t *r = &s_report.steps[step];
    r->state = (uint8_t)state;
    r->ms = (uint16_t)(ms > 65535u ? 65535u : ms);
    r->words = words;
    i18n_utf8_copy(r->value[0], sizeof(r->value[0]), v0); /* whole characters */
    i18n_utf8_copy(r->value[1], sizeof(r->value[1]), v1);
    xSemaphoreGive(s_lock);
}

static void step_running(net_check_step_t step)
{
    step_set(step, NET_CHECK_RUNNING, 0, NULL, NULL, NULL);
}

/* Everything from 'from' on was never reached. */
static void skip_rest(net_check_step_t from, const char *why)
{
    for (int i = (int)from; i < NET_CHECK_COUNT; i++) {
        step_set((net_check_step_t)i, NET_CHECK_SKIPPED, 0, why, NULL, NULL);
    }
}

/* ---- probes ------------------------------------------------------------------------------ */

/* Shared by ping_probe and the ping task's callbacks. Each side holds a reference: the caller
 * from the start, the ping task from esp_ping_start() until its end callback has run (the SDK
 * calls on_ping_end exactly once for a started batch, also one stopped before it ran). Whoever
 * lets go last frees it, so a ping that ends after the caller gave up waiting still frees its
 * context and semaphore, and neither side touches them once freed (M06). */
typedef struct {
    SemaphoreHandle_t done;
    uint32_t replies;
    uint32_t rtt_ms;
    atomic_uint refs;
} ping_ctx_t;

static void ping_ctx_release(ping_ctx_t *c)
{
    if (atomic_fetch_sub(&c->refs, 1u) == 1u) {
        vSemaphoreDelete(c->done);
        free(c);
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *c = (ping_ctx_t *)args;
    uint32_t gap = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap));
    if (c->rtt_ms == 0u || gap < c->rtt_ms) {
        c->rtt_ms = gap;
    }
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *c = (ping_ctx_t *)args;
    uint32_t received = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    c->replies = received;
    xSemaphoreGive(c->done);
    ping_ctx_release(c); /* the task's reference: nothing here touches c after this */
}

/* true when the address answered at least one of three pings. ok_out false: the ping couldn't
 * even be sent (that isn't the address's fault). */
static bool ping_probe(const char *addr, uint32_t *rtt_ms, bool *could_send)
{
    *rtt_ms = 0;
    *could_send = false;
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    ip4_addr_t v4;
    if (!ip4addr_aton(addr, &v4)) {
        return false;
    }
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4 = v4;

    /* On the heap, not this stack: esp_ping_stop() and esp_ping_delete_session() only raise
     * flags, and the ping task still calls back with this context once its batch ends. */
    ping_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return false;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->done == NULL) {
        free(ctx);
        return false;
    }
    atomic_init(&ctx->refs, 1u); /* the caller's */
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 3;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 900;
    cfg.task_stack_size = 3072;
    esp_ping_callbacks_t cbs = {
        .cb_args = ctx,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = NULL,
        .on_ping_end = on_ping_end,
    };
    esp_ping_handle_t ping = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK || ping == NULL) {
        ping_ctx_release(ctx); /* no session: no callback will come */
        return false;
    }
    *could_send = true;
    bool ok = false;
    bool ended = true; /* no batch started: no callback will come */
    atomic_fetch_add(&ctx->refs, 1u); /* the ping task's, before it can call back */
    if (esp_ping_start(ping) != ESP_OK) {
        ping_ctx_release(ctx); /* no batch started: its end callback will never run */
    } else {
        ended = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(5000)) == pdTRUE;
        if (ended) {
            ok = ctx->replies > 0u;
            *rtt_ms = ctx->rtt_ms;
        } else {
            esp_ping_stop(ping); /* the batch ends after at most one more ping */
            ended = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(3000)) == pdTRUE;
        }
    }
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    if (!ended) {
        /* The ping task is behind: its end callback frees the context when it comes. */
        ESP_LOGW(TAG, "ping to %s never finished", addr);
    }
    ping_ctx_release(ctx); /* the caller's: ctx is not touched after this */
    return ok;
}

/* Opens a TCP connection and closes it again. */
static bool tcp_probe(const char *addr, uint16_t port, uint32_t *took_ms)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        return false;
    }
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return false;
    }
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    const int64_t t0 = esp_timer_get_time();
    bool ok = connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    if (!ok && errno == EINPROGRESS) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        struct timeval tv;
        tv.tv_sec = (time_t)(TCP_TIMEOUT_MS / 1000u);
        tv.tv_usec = (long)((TCP_TIMEOUT_MS % 1000u) * 1000u);
        if (select(s + 1, NULL, &writable, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
    }
    *took_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    close(s);
    return ok;
}

/* One name lookup. out gets the first IPv4 address found. */
static bool dns_probe(const char *name, char *out, size_t len, uint32_t *took_ms)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    const int64_t t0 = esp_timer_get_time();
    const int err = getaddrinfo(name, NULL, &hints, &res);
    *took_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err != 0 || res == NULL) {
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return false;
    }
    const struct sockaddr_in *sa = (const struct sockaddr_in *)(void *)res->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, out, (socklen_t)len);
    freeaddrinfo(res);
    return out[0] != '\0';
}

/* One SNTP question. off_s: how far the panel's clock is from the server's. */
static bool ntp_probe(const char *addr, uint32_t *took_ms, long *off_s)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(123);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        return false;
    }
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return false;
    }
    struct timeval tv;
    tv.tv_sec = (time_t)(NTP_TIMEOUT_MS / 1000u);
    tv.tv_usec = (long)((NTP_TIMEOUT_MS % 1000u) * 1000u);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B; /* no warning, version 3, client */
    const int64_t t0 = esp_timer_get_time();
    bool ok = false;
    if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa, sizeof(sa)) == (int)sizeof(pkt)) {
        memset(pkt, 0, sizeof(pkt));
        if (recvfrom(s, pkt, sizeof(pkt), 0, NULL, NULL) == (int)sizeof(pkt)) {
            const uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                                  ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
            if (secs > NTP_EPOCH_DELTA) {
                const time_t server = (time_t)(secs - NTP_EPOCH_DELTA);
                const time_t here = time(NULL);
                *off_s = (long)(server - here);
                ok = true;
            }
        }
    }
    *took_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    close(s);
    return ok;
}

/* ---- the steps ---------------------------------------------------------------------------- */

static bool is_address(const char *s)
{
    ip4_addr_t v4;
    return s[0] != '\0' && ip4addr_aton(s, &v4) != 0;
}

/* Waits for the link, because the check is most often run right after the address settings
 * changed and the panel is still rejoining. */
static bool check_wifi(net_wifi_status_t *st)
{
    step_running(NET_CHECK_WIFI);
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        net_wifi_get_status(st);
        if (st->state == NET_WIFI_CONNECTED || st->state == NET_WIFI_NO_COPROCESSOR ||
            st->state == NET_WIFI_UNCONFIGURED ||
            (uint32_t)((esp_timer_get_time() - t0) / 1000) >= LINK_WAIT_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    const uint32_t took = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (st->state == NET_WIFI_CONNECTED) {
        step_set(NET_CHECK_WIFI, NET_CHECK_PASS, took, UI_CHECK_W_JOINED, st->ssid, NULL);
        return true;
    }
    if (st->state == NET_WIFI_NO_COPROCESSOR) {
        step_set(NET_CHECK_WIFI, NET_CHECK_FAIL, 0, UI_CHECK_W_MODULE_SILENT, NULL, NULL);
    } else if (st->state == NET_WIFI_UNCONFIGURED) {
        step_set(NET_CHECK_WIFI, NET_CHECK_FAIL, 0, UI_CHECK_W_NO_NETWORK, NULL, NULL);
    } else if (st->fail == NET_WIFI_FAIL_PASSWORD) {
        step_set(NET_CHECK_WIFI, NET_CHECK_FAIL, 0, UI_CHECK_W_PASSWORD, st->ssid, NULL);
    } else if (st->fail == NET_WIFI_FAIL_NOT_FOUND) {
        step_set(NET_CHECK_WIFI, NET_CHECK_FAIL, 0, UI_CHECK_W_NO_ANSWER, st->ssid, NULL);
    } else {
        step_set(NET_CHECK_WIFI, NET_CHECK_FAIL, 0, UI_CHECK_W_NOT_JOINED, st->ssid, NULL);
    }
    return false;
}

static bool check_address(const net_wifi_status_t *st)
{
    step_running(NET_CHECK_ADDRESS);
    if (st->ip[0] == '\0') {
        step_set(NET_CHECK_ADDRESS, NET_CHECK_FAIL, 0,
                 st->static_ip ? UI_CHECK_W_FIXED_NOT_TAKEN : UI_CHECK_W_NO_LEASE, NULL, NULL);
        return false;
    }
    step_set(NET_CHECK_ADDRESS, NET_CHECK_PASS, 0,
             st->static_ip ? UI_CHECK_W_ADDRESS_FIXED : UI_CHECK_W_ADDRESS_LEASED, st->ip,
             st->mask);
    return true;
}

static void check_router(const net_wifi_status_t *st)
{
    step_running(NET_CHECK_ROUTER);
    if (st->gateway[0] == '\0' || strcmp(st->gateway, "0.0.0.0") == 0) {
        step_set(NET_CHECK_ROUTER, NET_CHECK_FAIL, 0, UI_CHECK_W_NO_ROUTER, NULL, NULL);
        return;
    }
    uint32_t rtt = 0;
    bool could_send = false;
    if (ping_probe(st->gateway, &rtt, &could_send)) {
        step_set(NET_CHECK_ROUTER, NET_CHECK_PASS, rtt, UI_CHECK_W_ANSWERED, st->gateway, NULL);
    } else if (!could_send) {
        step_set(NET_CHECK_ROUTER, NET_CHECK_WARN, 0, UI_CHECK_W_NO_PING, NULL, NULL);
    } else {
        step_set(NET_CHECK_ROUTER, NET_CHECK_FAIL, 0, UI_CHECK_W_WRONG_ROUTER, st->gateway, NULL);
    }
}

/* Resolved: the address the time step then asks. */
static void check_dns(const net_wifi_status_t *st, const char *name, char *resolved, size_t len)
{
    step_running(NET_CHECK_DNS);
    uint32_t took = 0;
    if (dns_probe(name, resolved, len, &took)) {
        step_set(NET_CHECK_DNS, NET_CHECK_PASS, took, UI_CHECK_W_RESOLVED, name, resolved);
        return;
    }
    resolved[0] = '\0';
    if (st->dns1[0] != '\0') {
        step_set(NET_CHECK_DNS, NET_CHECK_FAIL, 0, UI_CHECK_W_DNS_SILENT, st->dns1, name);
    } else {
        step_set(NET_CHECK_DNS, NET_CHECK_FAIL, 0, UI_CHECK_W_NO_DNS, name, NULL);
    }
}

static void check_internet(void)
{
    step_running(NET_CHECK_INTERNET);
    for (size_t i = 0; i < sizeof(INTERNET_TARGETS) / sizeof(INTERNET_TARGETS[0]); i++) {
        uint32_t took = 0;
        if (tcp_probe(INTERNET_TARGETS[i], INTERNET_PORT, &took)) {
            char port[8];
            snprintf(port, sizeof(port), "%d", INTERNET_PORT);
            step_set(NET_CHECK_INTERNET, NET_CHECK_PASS, took, UI_CHECK_W_PORT_ANSWERED,
                     INTERNET_TARGETS[i], port);
            return;
        }
    }
    step_set(NET_CHECK_INTERNET, NET_CHECK_FAIL, 0, UI_CHECK_W_NOTHING_OUT, INTERNET_TARGETS[0],
             INTERNET_TARGETS[1]);
}

static void check_time(const char *server, const char *resolved)
{
    step_running(NET_CHECK_TIME);
    const char *addr = is_address(server) ? server : resolved;
    if (addr == NULL || addr[0] == '\0') {
        step_set(NET_CHECK_TIME, NET_CHECK_SKIPPED, 0, UI_CHECK_W_NOT_FOUND, server, NULL);
        return;
    }
    uint32_t took = 0;
    long off = 0;
    if (!ntp_probe(addr, &took, &off)) {
        step_set(NET_CHECK_TIME, NET_CHECK_FAIL, 0, UI_CHECK_W_NTP_SILENT, server, NULL);
        return;
    }
    const long slip = off < 0 ? -off : off;
    if (slip > 5) {
        char out_by[12];
        snprintf(out_by, sizeof(out_by), "%ld", slip);
        step_set(NET_CHECK_TIME, NET_CHECK_WARN, took, UI_CHECK_W_CLOCK_OUT, server, out_by);
    } else {
        step_set(NET_CHECK_TIME, NET_CHECK_PASS, took, UI_CHECK_W_CLOCK_RIGHT, server, NULL);
    }
}

static void check_service(void)
{
    step_running(NET_CHECK_SERVICE);
    bool enabled = false;
    char host[64] = "";
    uint16_t port = 0;
    if (s_service_fn != NULL) {
        s_service_fn(s_service_ctx, &enabled, host, sizeof(host), &port);
    }
    if (!enabled || host[0] == '\0' || port == 0u) {
        step_set(NET_CHECK_SERVICE, NET_CHECK_SKIPPED, 0, UI_CHECK_W_NO_SERVICE, NULL, NULL);
        return;
    }
    char addr[16] = "";
    uint32_t took = 0;
    if (is_address(host)) {
        snprintf(addr, sizeof(addr), "%.15s", host);
    } else if (!dns_probe(host, addr, sizeof(addr), &took)) {
        step_set(NET_CHECK_SERVICE, NET_CHECK_FAIL, 0, UI_CHECK_W_NOT_FOUND, host, NULL);
        return;
    }
    char where[64];
    snprintf(where, sizeof(where), "%.48s:%u", host, (unsigned)port);
    if (tcp_probe(addr, port, &took)) {
        step_set(NET_CHECK_SERVICE, NET_CHECK_PASS, took, UI_CHECK_W_ANSWERED, where, NULL);
    } else {
        step_set(NET_CHECK_SERVICE, NET_CHECK_FAIL, 0, UI_CHECK_W_NOTHING_AT, where, NULL);
    }
}

/* ---- the run ------------------------------------------------------------------------------- */

static void check_task(void *arg)
{
    (void)arg;
    net_wifi_status_t st;
    char server[64] = "";
    net_wifi_get_ntp_server(server, sizeof(server));

    if (check_wifi(&st)) {
        if (check_address(&st)) {
            char resolved[16] = "";
            check_router(&st);
            check_dns(&st, is_address(server) ? PROBE_NAME : server, resolved, sizeof(resolved));
            check_internet();
            check_time(server, resolved);
            check_service();
        } else {
            skip_rest(NET_CHECK_ROUTER, UI_CHECK_W_NO_ADDRESS);
        }
    } else {
        skip_rest(NET_CHECK_ADDRESS, UI_CHECK_W_OFF_NETWORK);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_report.running = false;
    s_report.done = true;
    xSemaphoreGive(s_lock);
    portENTER_CRITICAL(&s_mux);
    s_running = false;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "connection check finished");
    vTaskDelete(NULL);
}

esp_err_t net_check_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void net_check_set_service(net_check_service_fn fn, void *ctx)
{
    s_service_fn = fn;
    s_service_ctx = ctx;
}

esp_err_t net_check_start(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bool mine = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_running) {
        s_running = true;
        mine = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!mine) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_report, 0, sizeof(s_report));
    s_report.running = true;
    xSemaphoreGive(s_lock);
    /* getaddrinfo, TLS-less sockets and the ping session: 5 KB is comfortable. */
    if (xTaskCreate(check_task, "netchk", 5120, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_report.running = false;
        xSemaphoreGive(s_lock);
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_check_get(net_check_report_t *out)
{
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_report;
    xSemaphoreGive(s_lock);
}
