/*
 * tls_gate_esp.c - the panel's one TLS gate, shared by every listener (tls_gate.h, SEC-012).
 */
#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "tls_gate.h"

static const char *TAG = "tls_gate";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static tls_gate_t s_gate; /* under s_mux */
static int64_t s_logged_us;
static uint32_t s_unlogged;

static void peer_address(int fd, uint8_t out[16])
{
    memset(out, 0, 16);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) {
        return;
    }
    if (ss.ss_family == AF_INET) {
        out[10] = 0xff;
        out[11] = 0xff;
        memcpy(out + 12, &((const struct sockaddr_in *)&ss)->sin_addr.s_addr, 4);
    }
#if CONFIG_LWIP_IPV6
    else if (ss.ss_family == AF_INET6) {
        memcpy(out, &((const struct sockaddr_in6 *)&ss)->sin6_addr, 16);
    }
#endif
}

/* Closed with a reset, not a FIN: no TIME_WAIT, so a refused or failed connection leaves nothing of
 * itself in lwIP's internal RAM. */
static void close_hard(int fd)
{
    const struct linger now = {.l_onoff = 1, .l_linger = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &now, sizeof(now));
}

bool tls_gate_fd(int fd, const char *who)
{
    uint8_t addr[16];
    peer_address(fd, addr);
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    const tls_gate_verdict_t v = tls_gate_admit(&s_gate, addr, now, free_internal, block);
    bool log = false;
    uint32_t n = 0;
    if (v != TLS_GATE_ADMIT) {
        n = ++s_unlogged;
        if (now - s_logged_us >= 10000000) {
            log = true;
            s_logged_us = now;
            s_unlogged = 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    /* A flood mustn't fill the log, which costs time and flash: once every ten seconds. */
    if (log) {
        ESP_LOGW(TAG, "%" PRIu32 " connection(s) closed before the handshake (latest: %s, %s; internal RAM %u, largest %u)",
                 n, who, tls_gate_verdict_name(v), (unsigned)free_internal, (unsigned)block);
    }
    if (v != TLS_GATE_ADMIT) {
        close_hard(fd);
    }
    return v == TLS_GATE_ADMIT;
}

void tls_gate_fd_failed(int fd)
{
    uint8_t addr[16];
    peer_address(fd, addr);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    tls_gate_failed(&s_gate, addr, now);
    portEXIT_CRITICAL(&s_mux);
    close_hard(fd);
}

void tls_gate_fd_trust(int fd)
{
    uint8_t addr[16];
    peer_address(fd, addr);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    tls_gate_trust(&s_gate, addr, now);
    portEXIT_CRITICAL(&s_mux);
}

void tls_gate_counts(uint32_t out[4])
{
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_gate.counts, sizeof(s_gate.counts));
    portEXIT_CRITICAL(&s_mux);
}
