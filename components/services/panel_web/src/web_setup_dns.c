/* web_setup_dns.c - the setup Wi-Fi's name server (docs/features/42, the owner 2026-10-21): while a
 * board without a screen is in setup or recovery, every name a phone on its setup Wi-Fi looks up
 * is answered with the panel's address there, so whatever plain-HTTP address is typed lands on the
 * setup page (panel_web.c plain_serve redirects it), as a router's own setup does. Only A queries
 * get an address; anything else is answered empty. It listens on the setup Wi-Fi's address only,
 * never on the home network, and drops what it gets while setup is closed.
 *
 * Its task never writes flash (its stack may be slow memory). */
#include "web_setup.h"

#include <string.h>

#include "alarm_mem.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net_wifi.h"

static const char *TAG = "setup_dns";

#define DNS_PORT  53
#define DNS_MAX   512
#define DNS_TTL_S 60u

/* The answer to one query in buf (n bytes) at out; its length, 0: nothing to send. */
static size_t answer(const uint8_t *q, size_t n, uint8_t *out, size_t cap, uint32_t ip_be)
{
    if (n < 12u || n > cap - 16u || (q[2] & 0x80u) != 0u || ((q[2] >> 3) & 0x0Fu) != 0u ||
        q[4] != 0u || q[5] != 1u) {
        return 0u; /* not a standard query with one question */
    }
    size_t at = 12u;
    while (at < n && q[at] != 0u) {
        if ((q[at] & 0xC0u) != 0u) {
            return 0u; /* no pointers in a question */
        }
        at += 1u + q[at];
    }
    if (at + 5u > n) {
        return 0u;
    }
    const size_t qend = at + 5u; /* the name's zero, QTYPE, QCLASS */
    const bool a_in = q[at + 1] == 0u && q[at + 2] == 1u && q[at + 3] == 0u && q[at + 4] == 1u;
    memcpy(out, q, qend);
    out[2] = (uint8_t)(0x80u | (q[2] & 0x01u) | 0x04u); /* a response, authoritative, RD kept */
    out[3] = 0x80u;                                     /* RA, no error */
    out[6] = 0u;
    out[7] = a_in ? 1u : 0u; /* ANCOUNT */
    memset(out + 8, 0, 4);   /* NSCOUNT, ARCOUNT */
    size_t len = qend;
    if (a_in) {
        const uint8_t rr[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                              0x00, 0x00, 0x00, (uint8_t)DNS_TTL_S, 0x00, 0x04};
        memcpy(out + len, rr, sizeof(rr));
        len += sizeof(rr);
        memcpy(out + len, &ip_be, 4);
        len += 4u;
    }
    return len;
}

static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t in[DNS_MAX], out[DNS_MAX + 16];
    const uint32_t ip_be = inet_addr(NET_WIFI_SETUP_AP_IP);
    int sock = -1;
    for (;;) {
        if (!net_wifi_setup_ap_on(NULL) || !web_setup_listener()) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT)};
            me.sin_addr.s_addr = ip_be; /* the setup Wi-Fi only */
            const struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
            if (sock < 0 || bind(sock, (struct sockaddr *)&me, sizeof(me)) != 0 ||
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                if (sock >= 0) {
                    close(sock);
                    sock = -1;
                }
                vTaskDelay(pdMS_TO_TICKS(1000)); /* the setup Wi-Fi's address isn't up yet */
                continue;
            }
            ESP_LOGI(TAG, "answering names on the setup Wi-Fi");
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        const size_t len = answer(in, (size_t)n, out, sizeof(out), ip_be);
        if (len > 0u) {
            (void)sendto(sock, out, len, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

void web_setup_dns_start(void)
{
    if (alarm_mem_task_create(dns_task, "setup_dns", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "not started: typed addresses won't reach the setup page");
    }
}
